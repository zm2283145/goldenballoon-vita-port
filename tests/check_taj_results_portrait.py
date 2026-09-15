#!/usr/bin/env python3
"""Prove Taj owns a full native results portrait in a real 2P Rankings flow."""

from __future__ import annotations

import argparse
import contextlib
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

from check_taj_character_select import read_ppm
from harness_utils import DEFAULT_BUILD_DIR, resolve_binary


ROOT = Path(__file__).resolve().parent.parent
SCRIPT = ROOT / "tests/input_scripts/taj_2p_split.txt"
STATE = ROOT / "tests/fixtures/taj_mod_state_unlocked.ini"
CAPTURE_FRAMES = (7100, 12750)
MIN_FRAMES = 12780


def portrait_samples(width: int, height: int, pixels: bytes,
                     bounds: tuple[float, float, float, float]) -> list[tuple[int, int, int]]:
    x0, y0, x1, y1 = bounds
    samples: list[tuple[int, int, int]] = []
    for y in range(int(height * y0), int(height * y1)):
        for x in range(int(width * x0), int(width * x1)):
            offset = (y * width + x) * 3
            samples.append(tuple(pixels[offset:offset + 3]))
    return samples


def colour_count(samples: list[tuple[int, int, int]], name: str) -> int:
    count = 0
    for red, green, blue in samples:
        if name == "purple":
            match = (blue > 70 and red > 40 and blue > green * 1.20 and
                     red > blue * 0.45)
        elif name == "skin":
            match = (red > 125 and green > 70 and green < red * 0.93 and
                     blue < green * 0.90)
        elif name == "cyan":
            # The face/trunk, not the dark blue field behind it. Relative
            # channels survive the results menu's animated global opacity.
            match = (green > 60 and blue > 80 and green > red * 1.35 and
                     blue > green * 1.08 and blue < green * 1.50)
        elif name == "blue":
            match = blue > 90 and blue > red * 1.20 and blue > green * 1.10
        elif name == "gold":
            # Results menus animate their global opacity, so retain the jewel
            # and warm face detail at both the bright and dim cycle phases.
            match = (red > 110 and green > 65 and blue < 90 and
                     red > green * 1.20)
        elif name == "red":
            match = red > 145 and red > green * 1.35 and red > blue * 1.35
        else:
            raise ValueError(f"unknown colour class {name}")
        count += match
    return count


# Diddy's authored result card is predominantly his red cap/shirt. Floor is a
# fraction of the same 40x40 logical card footprint the samples cover.
DIDDY_RED_MIN_COVERAGE = 0.05
# The former 2.5% floor counted the tan FACE as gold. The actual thin turban
# band/jewel occupies 1.63% even in the dim results phase. Pin that detail with
# margin, and erase it in a pixel control below so absence cannot pass.
GOLD_MIN_COVERAGE = 0.012
TAJ_BOUNDS = (0.34, 0.23, 0.46, 0.39)


def validate_pixels(width: int, height: int, pixels: bytes) -> list[str]:
    failures: list[str] = []
    # Both bounds are the same 40x40 logical result-card footprint.
    taj = portrait_samples(width, height, pixels, TAJ_BOUNDS)
    diddy = portrait_samples(width, height, pixels, (0.54, 0.23, 0.66, 0.39))
    if not taj or len(taj) != len(diddy):
        return ["portrait evidence regions are invalid"]

    taj_counts = {name: colour_count(taj, name) for name in
                  ("purple", "skin", "cyan", "blue", "gold", "red")}
    diddy_counts = {name: colour_count(diddy, name) for name in
                    ("purple", "red")}
    total = len(taj)
    if taj_counts["purple"] < total * 0.12:
        failures.append("Taj card lacks the purple turban/robe")
    if taj_counts["cyan"] < total * 0.08:
        failures.append("Taj card lacks a blue elephant face/trunk")
    if taj_counts["skin"] > total * 0.04:
        failures.append("Taj card still has the old tan face/trunk")
    if taj_counts["blue"] < total * 0.25:
        failures.append("Taj card lacks its authored blue field")
    if taj_counts["gold"] < total * GOLD_MIN_COVERAGE:
        failures.append("Taj card lacks the gold turban detail")
    if taj_counts["purple"] < diddy_counts["purple"] * 3:
        failures.append("Taj result is not visually distinct from Diddy")
    # Relative-only again: Taj's card has no red, so taj_counts["red"] is 0 and
    # `diddy_red < 0` can never be true -- the assertion passed with Diddy's
    # card entirely blank. Pin an absolute floor as well, so this fails when
    # P2's portrait stops being drawn at all.
    diddy_red_floor = int(total * DIDDY_RED_MIN_COVERAGE)
    if diddy_counts["red"] < max(taj_counts["red"] * 1.5, diddy_red_floor):
        failures.append(
            "ordinary P2 no longer renders Diddy's portrait: "
            f"diddy_red={diddy_counts['red']}, taj_red={taj_counts['red']}, "
            f"floor={diddy_red_floor}, region={total}")
    mean_difference = sum(
        abs(left[channel] - right[channel])
        for left, right in zip(taj, diddy)
        for channel in range(3)
    ) / (total * 3)
    if mean_difference < 30.0:
        failures.append(
            "Taj and Diddy portrait pixels are insufficiently distinct: "
            f"mean difference={mean_difference:.2f}")
    return failures


def validate_capture(capture: Path) -> list[str]:
    width, height, pixels = read_ppm(capture)
    failures = validate_pixels(width, height, pixels)
    # Remove only the band/jewel colour inside Taj's card. This control must
    # specifically fail the gold assertion, not merely some other card check.
    without_gold = bytearray(pixels)
    x0, y0, x1, y1 = TAJ_BOUNDS
    for y in range(int(height * y0), int(height * y1)):
        for x in range(int(width * x0), int(width * x1)):
            offset = (y * width + x) * 3
            pixel = tuple(pixels[offset:offset + 3])
            if colour_count([pixel], "gold"):
                without_gold[offset:offset + 3] = bytes((34, 72, 190))
    if "Taj card lacks the gold turban detail" not in validate_pixels(
            width, height, bytes(without_gold)):
        failures.append("gold-removal positive control escaped detection")
    return failures


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", default=DEFAULT_BUILD_DIR)
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    parser.add_argument("--frames", type=int, default=MIN_FRAMES)
    parser.add_argument("--renderer", choices=("webgpu", "gl"), default="webgpu")
    parser.add_argument("--keep-frames", type=Path,
                        help="retain isolated captures and log for visual review")
    parser.add_argument(
        "--timeout", type=int, default=300,
        help="wall-clock limit for the long two-results capture (default: 300s)",
    )
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    binary = Path(resolve_binary(args.build)).resolve()
    rom = Path(args.rom).resolve()
    for path in (binary, rom, SCRIPT, STATE):
        if not path.is_file():
            print(f"check_taj_results_portrait: FAIL -- missing {path}",
                  file=sys.stderr)
            return 1
    if args.frames < MIN_FRAMES:
        print(f"check_taj_results_portrait: FAIL -- need at least {MIN_FRAMES} frames",
              file=sys.stderr)
        return 1

    failures: list[str] = []
    if args.keep_frames is not None:
        args.keep_frames = args.keep_frames.resolve()
        args.keep_frames.mkdir(parents=True, exist_ok=True)
    output_root = (contextlib.nullcontext(str(args.keep_frames))
                   if args.keep_frames is not None else
                   tempfile.TemporaryDirectory(prefix="mdkr-taj-results-"))
    with output_root as temporary:
        run_dir = Path(temporary)
        save_dir = run_dir / "save"
        frames_dir = run_dir / "frames"
        save_dir.mkdir()
        frames_dir.mkdir()
        shutil.copyfile(STATE, save_dir / "taj_mod_state.ini")

        env = {key: value for key, value in os.environ.items()
               if not key.startswith(("MDKR", "GE007_"))}
        env.update(
            LC_ALL="C",
            MDKR_AUDIO="0",
            MDKR_AUTOPILOT="1",
            MDKR_TRACE="1",
            MDKR_RENDERER=args.renderer,
            MDKR_DUMP_FROM=str(CAPTURE_FRAMES[0]),
            MDKR_DUMP_EVERY=str(CAPTURE_FRAMES[1] - CAPTURE_FRAMES[0]),
            MDKR_SAVE_DIR=str(save_dir),
            # Isolate the video config with the save (see check_door_blocks.py).
            MDKR_VIDEO_CONFIG_PATH=str(save_dir / "video.ini"),
        )
        command = [
            str(binary), "--headless-frames", str(args.frames),
            "--input-script", str(SCRIPT), "--dump-frames", str(frames_dir),
            "--rom", str(rom),
        ]
        if args.verbose:
            print("$ " + " ".join(command), flush=True)
        try:
            process = subprocess.run(
                command, cwd=run_dir, env=env, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                timeout=args.timeout, check=False,
            )
        except subprocess.TimeoutExpired as error:
            output = error.stdout or ""
            if isinstance(output, bytes):
                output = output.decode("utf-8", "replace")
            print(
                "check_taj_results_portrait: FAIL -- capture exceeded "
                f"{args.timeout}s\n{output[-4000:]}",
                file=sys.stderr,
            )
            return 1
        output = process.stdout or ""
        if args.keep_frames is not None:
            (run_dir / "run.log").write_text(output, encoding="utf-8")
        if process.returncode != 0:
            failures.append(f"exit code {process.returncode}")
        for marker in ("[CRASH]", "[FATAL]", "AddressSanitizer",
                       "runtime error:"):
            if marker in output:
                failures.append(f"fatal marker in output: {marker}")
        required = {
            "requested evidence renderer":
                f"[mdkr64] renderer backend: {args.renderer}",
            "real Rankings menu": "menu_init: menuId=17",
            "retail-sized native portrait":
                "taj_portrait: source=native-taj-card size=40x40 retail=40x40",
            "Taj result ownership":
                "taj_results_portrait: player=0 identity=taj source=native-taj-card",
        }
        for label, marker in required.items():
            if marker not in output:
                failures.append(f"{label}: missing {marker!r}")
        if output.count(
                "taj_portrait: source=native-taj-card size=40x40 retail=40x40") < 2:
            failures.append(
                "Taj portrait was not reconstructed after menu/stage teardown")
        if "taj_results_portrait: player=1 identity=taj" in output:
            failures.append("ordinary P2 inherited P1's Taj portrait")

        for frame in CAPTURE_FRAMES:
            capture = frames_dir / f"frame_{frame}.ppm"
            try:
                failures.extend(
                    f"frame {frame}: {failure}"
                    for failure in validate_capture(capture))
            except (OSError, ValueError) as error:
                failures.append(
                    f"could not inspect Rankings capture at frame {frame}: {error}")

    if failures:
        for failure in failures:
            print(f"check_taj_results_portrait: FAIL -- {failure}",
                  file=sys.stderr)
        return 1
    print("check_taj_results_portrait: PASS -- two real 2P Rankings loads "
          "rendered a retail-sized native Taj card while P2 remained Diddy")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
