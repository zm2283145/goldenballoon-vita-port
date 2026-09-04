#!/usr/bin/env python3
"""Issue #59: best time/lap digits hold still during the track-preview flyby.

On the Tracks setup screen (and the adventure TT preview) the BEST TIME /
BEST LAP digits are colourful-font sprites drawn through
render_ortho_triangle_image, not draw_text TEXRECTs.  That builder folds the
ACTIVE camera's z roll into every glyph's billboard matrix.  The original
game read gCameras[gActiveCameraID] -- the base viewport slot, which sits
still while a preview plays -- so on hardware the digits never move.  The
camera-obstruction port replaced that read with cam_get_active_camera(),
which applies the cutscene +4 slot offset; course previews run on the
cutscene bank (camera.c, write_to_object_render_stack), so every digit
inherited the flyby camera's banking roll and visibly rocked back and forth
with the camera (issue #59, reported on Ancient Lake and Crescent Island).

This check drives the reproduction route -- Tracks -> Ancient Lake -> hold on
the setup screen while the preview flies -- and closes three contracts:

* steadiness: for every qualifying BEST TIME glyph, the per-frame glyph
  shear (x-centroid of the glyph's top half minus its bottom half, the
  direct measure of the rocking rotation) stays within a spread of
  MAX_SHEAR_SPREAD native pixels across the whole flyby.  The unfixed build
  measures a 3.8-4.0 px native spread (about 12 degrees of rock) on the
  tall glyphs; the fixed build measures under 0.15 px (sprite easing +
  antialiasing noise only);
* non-vacuity, digits: at least MIN_QUALIFYING_GLYPHS glyph windows must
  qualify, each observed clean in at least MIN_CLEAN_FRAMES dumps -- a route
  drift that never reaches the setup screen, or a mask that stops seeing
  the digits, fails instead of passing an empty assertion;
* non-vacuity, camera: the flyby must actually be moving -- the sky strip at
  the top of the frame must keep changing between dumps.  A frozen preview
  would hold the digits still for the wrong reason.

The digit mask keys on the green course-time row.  Frames where the moving
3-D background bleeds green into a glyph window (trees, grass banks) are
rejected per glyph by a pixel-count corridor around that glyph's own median,
so the metric only ever compares a glyph against clean sightings of itself.

No developer save or video config is read or changed: the run uses a scratch
save directory (a fresh save is enough -- the setup screen always renders the
default course records) and a pinned MDKR_VIDEO_CONFIG_PATH.
"""

from __future__ import annotations

import argparse
import os
import re
import statistics
import subprocess
import sys
import tempfile
from pathlib import Path

from harness_utils import DEFAULT_BUILD_DIR, resolve_binary

# nav_to_time_trial_race.txt drives TITLE -> ... -> TRACK_SELECT(15) -> A
# (enter the Ancient Lake setup screen).  Its last three presses (vehicle,
# TT toggle, GO) are dropped so the route HOLDS on the setup screen while
# the preview flyby plays underneath the best-time panel.
BASE_SCRIPT = "tests/input_scripts/nav_to_time_trial_race.txt"
PRESSES_TO_DROP = 3

FRAMES = 4600
DUMP_FROM = 2300
DUMP_EVERY = 6

# Digit band for the green BEST TIME row, as fractions of the frame; the
# authored screen is 320x240 and the dump scale is a host video setting, so
# the crop and every pixel metric are expressed relative to frame size.
BAND_X0, BAND_X1 = 540 / 1280, 950 / 1280
BAND_Y0, BAND_Y1 = 240 / 960, 330 / 960
SKY_STRIP_Y1 = 0.08

# Steadiness contract, in native (320-wide) pixels, over the 5th..95th
# percentile of each glyph's clean shear sightings.  The percentile window
# drops the sparse sightings where 3-D background bleed slips inside the
# count corridor; the defect is the BULK of the distribution (a slow
# sinusoid), so it cannot hide in the trimmed tails.  Calibrated on this
# route: unfixed 1.40-3.72 px on every one of the 8 glyphs, fixed
# 0.00-0.08 px (the sprites are pixel-identical between clean sightings).
MAX_SHEAR_SPREAD = 0.8
MIN_QUALIFYING_GLYPHS = 4
MIN_CLEAN_FRAMES = 100
SPREAD_PERCENTILES = (0.05, 0.95)
# Per-glyph clean-sighting corridor around the glyph's median mask size.
COUNT_CORRIDOR = (0.75, 1.25)
# Sky-strip mean absolute channel difference between consecutive dumps.
MIN_SKY_MOTION = 2.0

MENU_RE = re.compile(r"menu_init: menuId=(\d+)")
LOAD_RE = re.compile(r"level_load: levelId=5 .*cutscene=1")


def clean_env() -> dict[str, str]:
    return {key: value for key, value in os.environ.items()
            if not key.startswith("MDKR_")}


def ppm(payload: bytes) -> tuple[int, int, bytes]:
    header = payload.split(b"\n", 3)
    if len(header) != 4 or header[0] != b"P6" or header[2] != b"255":
        raise ValueError("not an 8-bit binary PPM")
    width, height = (int(value) for value in header[1].split())
    pixels = header[3]
    if len(pixels) != width * height * 3:
        raise ValueError(
            f"PPM payload is {len(pixels)} bytes, expected {width * height * 3}")
    return width, height, pixels


def is_digit_green(r: int, g: int, b: int) -> bool:
    return g > 100 and g > r + 40 and g > b + 40


def band_mask(pixels: bytes, width: int, y0: int, y1: int,
              x0: int, x1: int) -> list[tuple[int, int]]:
    pts = []
    for y in range(y0, y1):
        base = y * width * 3
        for x in range(x0, x1):
            off = base + x * 3
            if is_digit_green(pixels[off], pixels[off + 1], pixels[off + 2]):
                pts.append((x - x0, y - y0))
    return pts


def glyph_windows(profile: list[float], min_gap: int = 3,
                  min_mass: float = 8.0) -> list[tuple[int, int]]:
    windows: list[tuple[int, int]] = []
    start = None
    gap = 0
    for x, count in enumerate(profile):
        if count > 0:
            if start is None:
                start = x
            gap = 0
        elif start is not None:
            gap += 1
            if gap >= min_gap:
                if sum(profile[start:x]) >= min_mass:
                    windows.append((start, x - gap + 1))
                start = None
                gap = 0
    if start is not None and sum(profile[start:]) >= min_mass:
        windows.append((start, len(profile)))
    return windows


def glyph_shear(points: list[tuple[int, int]],
                x0: int, x1: int) -> tuple[float, int] | None:
    glyph = [(x, y) for x, y in points if x0 <= x < x1]
    if len(glyph) < 20:
        return None
    ys = [y for _, y in glyph]
    mid = (min(ys) + max(ys)) / 2.0
    top = [x for x, y in glyph if y < mid]
    bottom = [x for x, y in glyph if y >= mid]
    if not top or not bottom:
        return None
    return (sum(top) / len(top) - sum(bottom) / len(bottom), len(glyph))


def sky_signature(pixels: bytes, width: int, height: int) -> list[int]:
    """Sparse sample of the top-of-frame 3-D strip (world, above the GUI)."""
    samples: list[int] = []
    for y in range(0, max(1, int(height * SKY_STRIP_Y1)), 4):
        base = y * width * 3
        for x in range(0, width, 16):
            off = base + x * 3
            samples.extend(pixels[off:off + 3])
    return samples


def route_text() -> str:
    lines = [line for line in Path(BASE_SCRIPT).read_text(encoding="utf-8")
             .splitlines() if line.strip() and not line.startswith("#")]
    return "\n".join(lines[:-PRESSES_TO_DROP]) + "\n"


def run_route(binary: str, rom: str, keep_frames: str | None,
              verbose: bool) -> tuple[str, int | str, dict[str, bytes]]:
    with tempfile.TemporaryDirectory(prefix="mdkr_preview_steady_") as run_dir:
        root = Path(run_dir)
        save_dir = root / "save"
        save_dir.mkdir()
        frame_dir = Path(keep_frames) if keep_frames else root / "frames"
        frame_dir.mkdir(parents=True, exist_ok=True)

        script = root / "route.txt"
        script.write_text(route_text(), encoding="utf-8")

        env = clean_env()
        env.update(
            MDKR_AUDIO="0",
            MDKR_TRACE="1",
            MDKR_SAVE_DIR=str(save_dir),
            MDKR_VIDEO_CONFIG_PATH=str(save_dir / "video.ini"),
            MDKR_DUMP_FROM=str(DUMP_FROM),
            MDKR_DUMP_EVERY=str(DUMP_EVERY),
        )
        command = [binary, "--headless-frames", str(FRAMES),
                   "--input-script", str(script),
                   "--dump-frames", str(frame_dir),
                   "--rom", os.path.abspath(rom)]
        if verbose:
            print("$ " + " ".join(command))
        try:
            proc = subprocess.run(command, cwd=run_dir, env=env,
                                  stdout=subprocess.PIPE,
                                  stderr=subprocess.STDOUT, timeout=900)
            output = proc.stdout.decode("utf-8", "replace")
            rc: int | str = proc.returncode
        except subprocess.TimeoutExpired as exc:
            output = (exc.stdout or b"").decode("utf-8", "replace")
            rc = "timeout"
        captures = {path.name: path.read_bytes()
                    for path in sorted(frame_dir.glob("frame_*.ppm"))}
        return output, rc, captures


def validate(output: str, rc: int | str,
             captures: dict[str, bytes]) -> list[str]:
    failures: list[str] = []
    if rc != 0:
        failures.append(f"exit={rc}")
    for marker in ("[CRASH]", "[FATAL]"):
        if marker in output:
            line = next((l for l in output.splitlines() if marker in l), marker)
            failures.append(f"{marker} in output: {line.strip()}")

    menu_ids = [int(m.group(1)) for m in MENU_RE.finditer(output)]
    if 15 not in menu_ids:
        failures.append(f"never reached TRACK_SELECT (menuId=15); saw {menu_ids}")
    if not LOAD_RE.search(output):
        failures.append("Ancient Lake preview never loaded "
                        "(no 'level_load: levelId=5 ... cutscene=1' trace row)")
    if len(captures) < 250:
        failures.append(f"only {len(captures)} dumped frame(s), expected ~380")
    if failures:
        return failures

    per_frame: list[tuple[str, list[tuple[int, int]], list[int]]] = []
    profiles: list[list[int]] = []
    band_width = 0
    width = height = 0
    for name, payload in captures.items():
        try:
            width, height, pixels = ppm(payload)
        except ValueError as exc:
            failures.append(f"{name}: {exc}")
            continue
        x0, x1 = int(BAND_X0 * width), int(BAND_X1 * width)
        y0, y1 = int(BAND_Y0 * height), int(BAND_Y1 * height)
        band_width = x1 - x0
        points = band_mask(pixels, width, y0, y1, x0, x1)
        columns = [0] * band_width
        for x, _ in points:
            columns[x] += 1
        profiles.append(columns)
        per_frame.append((name, points, sky_signature(pixels, width, height)))
    if failures:
        return failures

    # Per-column MEDIAN across the run: the digit columns are populated in
    # every frame, while green background bleed (trees, grass banks) is
    # transient, so the median profile keeps the glyphs and drops the bleed.
    profile = [float(statistics.median(frame[x] for frame in profiles))
               for x in range(band_width)]
    windows = glyph_windows(profile)
    if len(windows) < 6:
        failures.append(
            f"only {len(windows)} glyph window(s) found in the BEST TIME band; "
            "expected the 8 glyphs of the course record")
        return failures

    scale = 320.0 / width
    qualifying = 0
    for index, (wx0, wx1) in enumerate(windows):
        sightings: list[tuple[float, int]] = []
        for _, points, _ in per_frame:
            measured = glyph_shear(points, wx0, wx1)
            if measured is not None:
                sightings.append(measured)
        if len(sightings) < MIN_CLEAN_FRAMES:
            continue
        median_count = statistics.median(count for _, count in sightings)
        clean = [shear for shear, count in sightings
                 if COUNT_CORRIDOR[0] * median_count <= count
                 <= COUNT_CORRIDOR[1] * median_count]
        if len(clean) < MIN_CLEAN_FRAMES:
            continue
        qualifying += 1
        clean.sort()
        low = clean[int(SPREAD_PERCENTILES[0] * (len(clean) - 1))]
        high = clean[int(SPREAD_PERCENTILES[1] * (len(clean) - 1))]
        spread = (high - low) * scale
        print(f"  glyph[{index}] x={wx0}..{wx1}: {len(clean)} clean sightings, "
              f"p5..p95 shear spread {spread:.2f} native px")
        if spread > MAX_SHEAR_SPREAD:
            failures.append(
                f"glyph[{index}] (x={wx0}..{wx1}) rocks: p5..p95 shear spread "
                f"{spread:.2f} native px over {len(clean)} clean sightings "
                f"(limit {MAX_SHEAR_SPREAD})")
    if qualifying < MIN_QUALIFYING_GLYPHS:
        failures.append(
            f"only {qualifying} glyph window(s) had {MIN_CLEAN_FRAMES}+ clean "
            f"sightings; the digit mask lost the course record")

    motions = []
    previous: list[int] | None = None
    for _, _, signature in per_frame:
        if previous is not None and len(previous) == len(signature):
            diff = sum(abs(a - b) for a, b in zip(previous, signature))
            motions.append(diff / len(signature))
        previous = signature
    if not motions or statistics.mean(motions) < MIN_SKY_MOTION:
        level = statistics.mean(motions) if motions else 0.0
        failures.append(
            f"flyby looks frozen: sky-strip motion {level:.2f} < "
            f"{MIN_SKY_MOTION}; the steadiness assertion would be vacuous")
    return failures


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--build", default=DEFAULT_BUILD_DIR)
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    parser.add_argument("--keep-frames", default=None,
                        help="directory to keep the dumped frames in")
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args()

    binary = os.path.abspath(resolve_binary(args.build))
    if not os.path.exists(args.rom):
        print(f"SKIP: ROM not found: {args.rom}")
        return 0

    output, rc, captures = run_route(binary, args.rom, args.keep_frames,
                                     args.verbose)
    failures = validate(output, rc, captures)
    if failures:
        for failure in failures:
            print(f"FAIL: {failure}")
        return 1
    print("PASS: best time/lap digits hold still through the track preview "
          "flyby (issue #59)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
