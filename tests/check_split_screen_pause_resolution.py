#!/usr/bin/env python3
"""Issue #61: both pause owners retain the same font and output resolution.

Run the same real two-player race with START on P1 or P2, in Restored and
Remastered, and with output-resolution UI enabled or disabled. The pause
panel's different owner colours prove the scripts did not both pause with P1.
The common RESTART RACE line must have matching yellow glyph contours, and
both players must gain edge detail over their own scaled-UI control. Matching
P1/P2 pictures alone would miss a regression that downgraded both players.

The original-bitmap Restored capture substitutes for Remastered P2 in a
negative font control: the contour comparison must reject it. All captures
are local-only ROM-derived evidence. This is backend-local pixel acceptance,
not a claim about hardware on which the gate has not run.
"""

from __future__ import annotations

import argparse
import contextlib
import os
from pathlib import Path
import re
import subprocess
import tempfile

from check_native_ui_resolution import (
    FATAL_RE, Image, UI_RE, laplacian_energy, read_ppm, scaled_box,
)
from harness_utils import DEFAULT_BUILD_DIR, resolve_binary

ROOT = Path(__file__).resolve().parents[1]
FRAME = 3350
MIN_CONTOUR_IOU = 0.94
MIN_EDGE_GAIN = 1.15


def glyph_mask(image: Image) -> set[tuple[int, int]]:
    # RESTART RACE has the same opaque yellow contour for both pause owners.
    # Exclude the differently coloured panel edges, selected/blinking row,
    # and title's optional player-number substitution in other languages.
    x0, y0, x1, y1 = scaled_box(image, .375, .47, .625, .53)
    result = set()
    for y in range(y0, y1):
        for x in range(x0, x1):
            offset = (y * image.width + x) * 3
            r, g, b = image.pixels[offset:offset + 3]
            if r > 180 and g > 160 and b < 100:
                result.add((x, y))
    if len(result) < image.width * image.height * .0005:
        raise RuntimeError("pause label is absent or has too little glyph coverage")
    return result


def contour_iou(left: Image, right: Image) -> float:
    if (left.width, left.height) != (right.width, right.height):
        raise RuntimeError("pause images have different output dimensions")
    a, b = glyph_mask(left), glyph_mask(right)
    return len(a & b) / len(a | b)


def panel_owner(image: Image, player: int) -> None:
    x0, y0, x1, y1 = scaled_box(image, .32, .4, .345, .46)
    delta = sum(
        image.pixels[(y * image.width + x) * 3]
        - image.pixels[(y * image.width + x) * 3 + 2]
        for y in range(y0, y1) for x in range(x0, x1)
    ) / ((x1 - x0) * (y1 - y0))
    if (player == 1 and delta >= -50) or (player == 2 and delta <= 50):
        raise RuntimeError(f"P{player} pause-owner panel not observed: R-B={delta:.2f}")


def pace(output: str) -> tuple[str, ...]:
    rows = tuple(re.sub(r" dtms=\S+", " dtms=<wall>", match.group(0))
                 for match in re.finditer(r"\[PACE2?\][^\n]*", output))
    if not any(row.startswith("[PACE2]") for row in rows):
        raise RuntimeError("missing P2 simulation stream")
    return rows


def run_arm(binary: Path, rom: Path, root: Path, backend: str, mode: str,
            player: int, native: bool, timeout: int) -> tuple[Image, tuple[str, ...]]:
    label = f"{backend}-{mode}-p{player}-native{int(native)}"
    work = root / label
    work.mkdir()
    (work / "save").mkdir()
    frames = work / "frames"
    frames.mkdir()
    script = work / "input.txt"
    base = (ROOT / "tests/input_scripts/race_2p_split.txt").read_text()
    entries = [line for line in base.splitlines()
               if line.strip() and not line.lstrip().startswith("#")
               and int(line.split()[0]) < 3200]
    script.write_text("\n".join(entries) + f"\n3200 START 4 P{player}\n")
    env = {k: v for k, v in os.environ.items()
           if not k.startswith(("MDKR", "GE007_"))}
    env.update(
        MDKR_AUDIO="0", MDKR64_HIDDEN="1", MDKR_TRACE="1",
        MDKR_RENDERER=backend, MDKR_RENDER_SCALE="2", MDKR_AUTOPILOT="1",
        MDKR_SIMULATION_CADENCE="original", MDKR_UI_OVERLAY_TRACE="1",
        MDKR_TEST_RENDER_FULL_ADMISSION="1",
        MDKR_DUMP_FROM=str(FRAME), MDKR_DUMP_EVERY="999",
        MDKR_SAVE_DIR=str(work / "save"),
        MDKR_VIDEO_CONFIG_PATH=str(work / "video.ini"), LC_ALL="C",
    )
    # Exercise the production default, not an explicit enable that could hide
    # an accidentally disabled default. Only the counterfactual arm overrides.
    if not native:
        env["MDKR_UI_NATIVE_RES"] = "0"
    # 540-point height avoids a 6x integer source-pixel grid on Retina. At
    # 720 points GL's nearest-filtered Restored glyphs can downsample from
    # 9x to 6x with unchanged edges, making that scaled-UI control inert.
    command = [str(binary), "--headless-frames", "3370", "--window-size",
               "960x540", "--input-script", str(script), "--dump-frames",
               str(frames), "--rom", str(rom)]
    if mode == "remastered":
        command.append("--remastered")
    print(f"  {label}", flush=True)
    with (work / "run.log").open("w") as log:
        result = subprocess.run(command, cwd=work, env=env, stdout=log,
                                stderr=subprocess.STDOUT, timeout=timeout)
    output = (work / "run.log").read_text()
    if result.returncode or FATAL_RE.search(output):
        raise RuntimeError(f"{label}: exit={result.returncode}\n{output[-4000:]}")
    for marker in (f"[mdkr64] renderer backend: {backend}",
                   "level_load: levelId=5 numPlayers=1",
                   "hud_init: hudPlayers=1 numViewports=2"):
        if marker not in output:
            raise RuntimeError(f"{label}: missing {marker}")
    rows = [tuple(map(int, m.groups())) for m in UI_RE.finditer(output)]
    captured = [row for row in rows if row[0] == FRAME]
    if len(captured) != 1 or captured[0][1] != int(native):
        raise RuntimeError(f"{label}: wrong UI output pass at capture: {captured}")
    if any(row[3] or row[6] for row in rows):
        raise RuntimeError(f"{label}: world-after-overlay or output-pass failure")
    image = read_ppm(frames / f"frame_{FRAME}.ppm")
    panel_owner(image, player)
    glyph_mask(image)
    return image, pace(output)


def check_backend(binary: Path, rom: Path, root: Path, backend: str,
                  timeout: int) -> None:
    images = {}
    reference_pace = None
    for mode in ("restored", "remastered"):
        for player in (1, 2):
            for native in (True, False):
                image, stream = run_arm(binary, rom, root, backend, mode,
                                        player, native, timeout)
                images[mode, player, native] = image
                if reference_pace is None:
                    reference_pace = stream
                elif stream != reference_pace:
                    raise RuntimeError("pause owner/presentation changed simulation")
            normal = images[mode, player, True]
            control = images[mode, player, False]
            box = scaled_box(normal, .375, .47, .625, .53)
            baseline = laplacian_energy(control, box)
            gain = laplacian_energy(normal, box) / max(1, baseline)
            if gain < MIN_EDGE_GAIN:
                raise RuntimeError(f"{mode} P{player}: no native UI edge gain ({gain:.3f})")
            print(f"    {mode} P{player}: native/scaled edge gain={gain:.3f}", flush=True)
        overlap = contour_iou(images[mode, 1, True], images[mode, 2, True])
        if overlap < MIN_CONTOUR_IOU:
            raise RuntimeError(f"{mode}: P1/P2 pause font differs (IoU={overlap:.4f})")
        print(f"    {mode}: P1/P2 glyph contour IoU={overlap:.4f}", flush=True)
    wrong_font = contour_iou(images["remastered", 1, True],
                             images["restored", 2, True])
    if wrong_font >= MIN_CONTOUR_IOU:
        raise RuntimeError("font detector accepted original bitmap as Remastered P2")
    print(f"    original-bitmap font control rejected: IoU={wrong_font:.4f}", flush=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", default=DEFAULT_BUILD_DIR)
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    parser.add_argument("--renderer", choices=("webgpu", "gl"), action="append")
    parser.add_argument("--keep-frames", type=Path)
    parser.add_argument("--timeout", type=int, default=240)
    args = parser.parse_args()
    binary, rom = Path(resolve_binary(args.build)).resolve(), Path(args.rom).resolve()
    if args.keep_frames:
        args.keep_frames = args.keep_frames.resolve()
        args.keep_frames.mkdir(parents=True, exist_ok=True)
    context = (contextlib.nullcontext(args.keep_frames) if args.keep_frames else
               tempfile.TemporaryDirectory(prefix="mdkr-split-pause-"))
    try:
        with context as temporary:
            for backend in args.renderer or ("webgpu", "gl"):
                check_backend(binary, rom, Path(temporary), backend, args.timeout)
    except (OSError, RuntimeError, ValueError, subprocess.TimeoutExpired) as error:
        print(f"check_split_screen_pause_resolution: FAIL -- {error}")
        return 1
    print("check_split_screen_pause_resolution: PASS -- both pause owners retain "
          "font contours and output-resolution UI in Restored and Remastered")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
