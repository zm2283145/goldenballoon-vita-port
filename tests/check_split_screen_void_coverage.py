#!/usr/bin/env python3
"""Walrus Cove #61: retain hole coverage without hiding real split-screen scenery.

Each backend/preset runs the same two-player route with production defaults,
the authored foreground curtain, no curtain, and an early depth-writing curtain.
Both players must recover real scenery while retaining a visible covering of
holes. Removing the curtain and retaining its artificial depth must each fail
that same pixel verdict. Pure is compared byte-for-byte to its authored control.
SIMHASH v3 and both racer streams must agree within every comparison group.

This is local backend evidence, not unobserved Windows/NVIDIA acceptance.
Captures are private ROM-derived output and must never enter the repository.
"""

from __future__ import annotations

import argparse
from collections import Counter
import contextlib
from dataclasses import dataclass
import os
from pathlib import Path
import re
import subprocess
import tempfile

from check_native_ui_resolution import FATAL_RE, Image, read_ppm
from check_split_screen_pause_resolution import pace
from harness_utils import DEFAULT_BUILD_DIR, resolve_binary

ROOT = Path(__file__).resolve().parents[1]
TOKEN = "mdkr64-void-coverage-v1"
VOID_RE = re.compile(r"\[VOID-COVERAGE\] viewport=(\d+) policy=(\d+) primitives=(\d+)")


@dataclass(frozen=True)
class CapturePlan:
    revision: str
    start: int
    witnesses: tuple[tuple[int, int], ...]
    frames: int = 3440
    stride: int = 39

    @property
    def captures(self) -> tuple[int, ...]:
        return tuple(range(self.start, self.frames, self.stride))


US_PLAN = CapturePlan("us.v80", 3061, ((3061, 0), (3100, 1)))
PAL_PLAN = CapturePlan("pal.v80", 2980, ((2980, 0), (3058, 1)))


def capture_plan(rom: Path) -> CapturePlan:
    """Select fixed witnesses by ROM header, then verify the engine's identity.

    PAL advances along the same input route at a different rate. Its witnesses
    come from a separate dense authored-curtain survey; the pixel predicate
    and all counterfactual controls are shared with US.
    """
    with rom.open("rb") as stream:
        header = stream.read(64)
    if len(header) != 64:
        raise ValueError("ROM header is incomplete")
    if header[:4] == b"\x37\x80\x40\x12":
        header = bytes(header[index ^ 1] for index in range(64))
    elif header[:4] == b"\x40\x12\x37\x80":
        header = bytes(header[index ^ 3] for index in range(64))
    if header[:4] != b"\x80\x37\x12\x40" or header[63] != 1:
        raise ValueError("void coverage requires a supported revision 1 ROM")
    if header[62] == ord("E"):
        return US_PLAN
    if header[62] == ord("P"):
        return PAL_PLAN
    raise ValueError("void coverage has witnesses only for US and European 1.1")


@dataclass
class Arm:
    images: dict[int, Image]
    hashes: tuple[str, ...]
    racers: tuple[str, ...]


def close(a: bytes, b: bytes) -> bool:
    return max(abs(x - y) for x, y in zip(a, b)) <= 12


def coverage(normal: Image, authored: Image, disabled: Image,
             player: int) -> tuple[float, float, float, float]:
    """Measure recovery against actual scene pixels, not merely 'less blue'."""
    if len({(im.width, im.height) for im in (normal, authored, disabled)}) != 1:
        raise RuntimeError("void comparison has mismatched image dimensions")
    # Sample viewport-local pixel centres; keep away from the split separator.
    offsets = [(y * normal.width + x) * 3
               for y in range(player * normal.height // 2 + 4,
                              (player + 1) * normal.height // 2 - 4, 4)
               for x in range(4, normal.width - 4, 4)]
    dominant = Counter(authored.pixels[i:i + 3] for i in offsets).most_common(1)[0][0]
    old_flat = new_flat = recovered = retained = 0
    for i in offsets:
        a, n, d = (im.pixels[i:i + 3] for im in (authored, normal, disabled))
        old_flat += a == dominant
        new_flat += n == dominant
        if a == dominant and not close(d, dominant):
            recovered += close(n, d)
            retained += n == dominant
    count = len(offsets)
    return tuple(value / count for value in (old_flat, new_flat, recovered, retained))


def accepted(metrics: tuple[float, float, float, float]) -> bool:
    old_flat, new_flat, recovered, retained = metrics
    return old_flat > .60 and new_flat < .30 and recovered > .40 and retained > .01


def detector_controls() -> None:
    # Independent fixtures pin both obligations and prevent an arbitrary
    # non-blue replacement from masquerading as recovered scenery.
    flat, scenery, wrong = b"\x00\x53\x85", b"\xa0\xb0\xc0", b"\xc0\x10\x80"
    authored = Image(100, 100, (flat * 80 + scenery * 20) * 100)
    disabled = Image(100, 100, scenery * 10000)
    fixed = Image(100, 100, (flat * 10 + scenery * 90) * 100)
    unrelated = Image(100, 100, (flat * 10 + wrong * 70 + scenery * 20) * 100)
    for player in (0, 1):
        if not accepted(coverage(fixed, authored, disabled, player)):
            raise RuntimeError("detector rejected its scenery-plus-hole fixture")
        for name, image in (("obstruction", authored), ("deleted coverage", disabled),
                            ("unrelated replacement", unrelated)):
            if accepted(coverage(image, authored, disabled, player)):
                raise RuntimeError(f"detector accepted {name}")


def run_arm(binary: Path, rom: Path, root: Path, backend: str, mode: str,
            policy: str, timeout: int, plan: CapturePlan = US_PLAN) -> Arm:
    label = f"{backend}-{mode}-{policy}"
    work = root / label
    work.mkdir()
    (work / "save").mkdir()
    (work / "frames").mkdir()
    env = {k: v for k, v in os.environ.items()
           if not k.startswith(("MDKR", "GE007_"))}
    env.update(
        MDKR_DEDICATED_TEST_DESKTOP=os.environ["MDKR_DEDICATED_TEST_DESKTOP"],
        MDKR_AUDIO="0", MDKR64_HIDDEN="1", MDKR_TRACE="1",
        SDL_MAC_BACKGROUND_APP="1", SDL_WINDOW_NO_ACTIVATION_WHEN_SHOWN="1",
        MDKR_RENDERER=backend, MDKR_RENDER_SCALE="2", MDKR_AUTOPILOT="1",
        MDKR_SIMULATION_CADENCE="original", MDKR_LOAD_TRACK="6:0",
        MDKR_STATE_HASH="3", MDKR_VOID_COVERAGE_TRACE="1",
        MDKR_TEST_RENDER_FULL_ADMISSION="1",
        MDKR_DUMP_FROM=str(plan.start), MDKR_DUMP_EVERY=str(plan.stride),
        MDKR_SAVE_DIR=str(work / "save"),
        MDKR_VIDEO_CONFIG_PATH=str(work / "video.ini"), LC_ALL="C",
    )
    if policy != "default":
        env.update(MDKR_TEST_VOID_POLICY=policy, MDKR_TEST_VOID_TOKEN=TOKEN)
    command = [str(binary), "--headless-frames", str(plan.frames), "--window-size",
               "640x480" if mode == "pure" else "640x360", "--input-script",
               str(ROOT / "tests/input_scripts/race_2p_split.txt"),
               "--dump-frames", str(work / "frames"), "--rom", str(rom)]
    if mode != "restored":
        command.append("--" + mode)
    print(f"  {label}", flush=True)
    with (work / "run.log").open("w") as log:
        result = subprocess.run(command, cwd=work, env=env, stdout=log,
                                stderr=subprocess.STDOUT, timeout=timeout)
    output = (work / "run.log").read_text()
    if result.returncode or FATAL_RE.search(output):
        raise RuntimeError(f"{label}: exit={result.returncode}\n{output[-4000:]}")
    for marker in (f"[mdkr64] renderer backend: {backend}",
                   f"({plan.revision}) - a revision this build supports.",
                   "level_load: levelId=6 numPlayers=1",
                   "hud_init: hudPlayers=1 numViewports=2"):
        if marker not in output:
            raise RuntimeError(f"{label}: missing {marker}")
    expected_policy = {"authored": 0, "disabled": 2, "depth-write": 3}.get(
        policy, 0 if mode == "pure" else 1)
    rows = [tuple(map(int, match.groups())) for match in VOID_RE.finditer(output)]
    if policy == "disabled":
        if rows:
            raise RuntimeError(f"{label}: disabled curtain still drew")
    elif (any(row[1] != expected_policy for row in rows) or
          not all(any(row[0] == player and row[2] > 0 for row in rows)
                  for player in (0, 1))):
        raise RuntimeError(f"{label}: missing both-viewports production curtain policy")
    hashes = tuple(line for line in output.splitlines() if line.startswith("[SIMHASH]"))
    if len(hashes) != plan.frames:
        raise RuntimeError(f"{label}: expected {plan.frames} state hashes, got {len(hashes)}")
    actual = {int(path.stem.split("_")[1]) for path in (work / "frames").glob("frame_*.ppm")}
    if actual != set(plan.captures):
        raise RuntimeError(f"{label}: incomplete capture set {sorted(actual)}")
    racers = pace(output)
    if not all(any(row.startswith(marker) for row in racers)
               for marker in ("[PACE]", "[PACE2]")):
        raise RuntimeError(f"{label}: missing a racer simulation stream")
    return Arm({f: read_ppm(work / "frames" / f"frame_{f}.ppm") for f in plan.captures},
               hashes, racers)


def check_group(binary: Path, rom: Path, root: Path, backend: str, mode: str,
                timeout: int, plan: CapturePlan = US_PLAN) -> None:
    policies = ("default", "authored") if mode == "pure" else (
        "default", "authored", "disabled", "depth-write")
    arms = {p: run_arm(binary, rom, root, backend, mode, p, timeout, plan) for p in policies}
    normal = arms["default"]
    for name, arm in arms.items():
        if arm.hashes != normal.hashes or arm.racers != normal.racers:
            raise RuntimeError(f"{backend}/{mode}/{name}: rendering policy changed simulation")
    if mode == "pure":
        if normal.images != arms["authored"].images:
            raise RuntimeError(f"{backend}: Pure changed authored pixels")
        print(f"    Pure: {len(plan.captures)} byte-identical authored captures", flush=True)
        return
    for frame, player in plan.witnesses:
        authored, disabled = (arms[p].images[frame] for p in ("authored", "disabled"))
        metrics = coverage(normal.images[frame], authored, disabled, player)
        print(f"    {mode} P{player + 1} frame={frame}: old/new flat, recovered, "
              f"retained={tuple(round(x, 4) for x in metrics)}", flush=True)
        if not accepted(metrics):
            raise RuntimeError(f"{backend}/{mode}/P{player + 1}: scenery/hole coverage failed")
        for control in ("authored", "disabled", "depth-write"):
            if accepted(coverage(arms[control].images[frame], authored, disabled, player)):
                raise RuntimeError(f"{backend}/{mode}/P{player + 1}: accepted {control} control")
    print("    all three broken-render controls rejected; simulation identical", flush=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", default=DEFAULT_BUILD_DIR)
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    parser.add_argument("--renderer", choices=("webgpu", "gl"), action="append")
    parser.add_argument("--mode", choices=("restored", "remastered", "pure"), action="append")
    parser.add_argument("--keep-frames", type=Path)
    parser.add_argument("--timeout", type=int, default=240)
    args = parser.parse_args()
    if os.environ.get("MDKR_DEDICATED_TEST_DESKTOP") != "1":
        parser.error("requires a human-attested dedicated test desktop")
    binary, rom = Path(resolve_binary(args.build)).resolve(), Path(args.rom).resolve()
    if args.keep_frames:
        args.keep_frames = args.keep_frames.resolve()
        args.keep_frames.mkdir(parents=True, exist_ok=True)
    context = (contextlib.nullcontext(args.keep_frames) if args.keep_frames else
               tempfile.TemporaryDirectory(prefix="mdkr-split-void-"))
    try:
        plan = capture_plan(rom)
        print(f"  revision={plan.revision} witnesses={plan.witnesses}", flush=True)
        detector_controls()
        with context as temporary:
            for backend in args.renderer or ("webgpu", "gl"):
                for mode in args.mode or ("restored", "remastered", "pure"):
                    check_group(binary, rom, Path(temporary), backend, mode, args.timeout, plan)
    except (OSError, RuntimeError, ValueError, subprocess.TimeoutExpired) as error:
        print(f"check_split_screen_void_coverage: FAIL -- {error}")
        return 1
    print("check_split_screen_void_coverage: PASS -- "
          "selected backend/preset groups retain holes, recover scenery and preserve simulation")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
