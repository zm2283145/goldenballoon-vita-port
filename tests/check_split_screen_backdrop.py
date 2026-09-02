#!/usr/bin/env python3
"""Split-screen sky backdrop coverage (issue #61).

Why this exists
---------------
``render_scene()`` draws the real ``skydome_render()`` dome only when
``numViewports < 2``.  Every two-, three- and four-player viewport instead gets
``trackbg_render_gradient()``: one flat quad, spanning the ROM's hard-coded
``x = -200 .. +200`` and ``y = -150 .. +150`` (PAL 180), at ``z = 20`` behind
``D_800DD288``'s ``z = -281`` eye offset.

Those constants are not arbitrary.  At 261 units out,

    261 * tan(60/2 deg)          = 150.7   -> the authored +/-150
    261 * tan(60/2 deg) * (4/3)  = 200.9   -> the authored +/-200

so the quad is tuned to exactly fill a 4:3, 60-degree-vertical-FOV frustum --
the only frustum an N64 ever had.

The port's widescreen projection is Hor+: ``display_config.c`` keeps the
vertical FOV and derives the horizontal one from the live presentation aspect.
At any aspect wider than 4:3 the frustum is therefore wider than the quad, and
the strips the quad no longer reaches keep whatever the frame was cleared to --
black on every level whose ``voidColour`` is ``0,0,0`` (Fossil Canyon, Ancient
Lake, Whale Bay, Pirate Lagoon, Treasure Caves).  That is the reported
"the skybox in several levels during multiplayer has black lines on each side,
e.g. Fossil Canyon".

One player never sees it, because one player never reaches this function.

What this asserts
-----------------
1. DERIVED EXTENT (deterministic).  ``[TRACE] bg_backdrop:`` publishes the
   extent the renderer actually used.  At a 4:3 presentation it must be exactly
   the ROM's ``x=200 y=150`` -- the retail-parity rail, so widening a wide
   picture can never move a 4:3 one.  At 16:9 it must be ``x=267``
   (``150 * 16/9``), i.e. the quad tracks the frustum.

2. SKY COVERAGE (pixels).  Fossil Canyon's backdrop is a bright orange-to-
   yellow ramp and its ``voidColour`` is black, so an uncovered strip is
   unmistakable.  In the sky band of each viewport -- rows 3%..10% into the
   viewport's own half, which clears the split separator -- the fraction of
   pure-black pixels must stay near zero across the sampled frames.

   Measured on this fixture at 1280x720, worst frame of each viewport:

       without the widening   vp0 12.7%   vp1 10.7%
       with the widening      vp0  0.2%   vp1  4.3%

   vp1's 4.3% floor is real content (a shadowed canyon wall at frame 2900),
   which is why the two viewports carry different ceilings.  Both ceilings sit
   far below the unfixed measurement, so removing the widening fails both arms.

The black-band detector is self-tested on synthetic rasters first, so the
segmentation is proven able to fail before any real frame is judged.

Usage:
    python3 tests/check_split_screen_backdrop.py \
        --build build --rom baserom.us.v80.z64 -v
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness_utils import resolve_binary  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCRIPT = os.path.join(ROOT, "tests", "input_scripts", "race_2p_split.txt")

# Fossil Canyon: BGColour top (255,32,8) -> bottom (255,192,32), voidColour
# (0,0,0).  A bright authored sky over a black clear is the honest witness.
LEVEL_FOSSIL_CANYON = "3"

FRAMES = 3000
DUMP_FROM = 2700
DUMP_EVERY = 50

WIDE_SIZE = "1280x720"
PURE_SIZE = "1280x960"

# Sky band: rows 3%..10% into each viewport's own half.  Starting at 3% clears
# the `videoHeight >> 7` split separator, which is genuinely black.
BAND_TOP = 0.03
BAND_BOTTOM = 0.10
BLACK_LEVEL = 12

# Worst-frame ceilings.  See the module docstring for the measured pre-fix
# numbers these separate from.
SKY_BLACK_CEILING = {0: 1.5, 1: 5.0}

EXPECTED_EXTENT = {
    # presentation aspect -> (x half-extent, y half-extent)
    PURE_SIZE: (200, 150),   # 4:3 -- must reproduce the ROM's authored quad
    WIDE_SIZE: (267, 150),   # 16:9 -- 150 * 16/9 = 266.7
}


class Frame:
    __slots__ = ("width", "height", "pixels")

    def __init__(self, width: int, height: int, pixels: bytes) -> None:
        self.width = width
        self.height = height
        self.pixels = pixels


def read_ppm(path: str) -> Frame:
    data = open(path, "rb").read()
    tokens = []
    i = 0
    while len(tokens) < 4:
        while data[i:i + 1].isspace():
            i += 1
        if data[i:i + 1] == b"#":
            while data[i:i + 1] != b"\n":
                i += 1
            continue
        start = i
        while not data[i:i + 1].isspace():
            i += 1
        tokens.append(data[start:i])
    i += 1
    return Frame(int(tokens[1]), int(tokens[2]), data[i:])


def sky_black_percent(frame: Frame, viewport: int) -> float:
    """Percent of pure-black pixels in one viewport's sky band."""
    half = frame.height // 2
    y0 = viewport * half
    y1 = y0 + half
    height = y1 - y0
    first = y0 + int(height * BAND_TOP)
    last = y0 + int(height * BAND_BOTTOM)
    black = 0
    total = 0
    px = frame.pixels
    width = frame.width
    for y in range(first, last, 2):
        base = y * width * 3
        for x in range(0, width, 2):
            o = base + x * 3
            total += 1
            if px[o] < BLACK_LEVEL and px[o + 1] < BLACK_LEVEL \
                    and px[o + 2] < BLACK_LEVEL:
                black += 1
    return 100.0 * black / total if total else 0.0


def detector_self_test() -> list[str]:
    """The detector must see an edge band and must not invent one."""
    failures = []
    width, height = 200, 100

    # A fully painted frame: no black anywhere.
    painted = bytes([255, 128, 0] * (width * height))
    value = sky_black_percent(Frame(width, height, painted), 0)
    if value > 0.01:
        failures.append(
            f"detector self-test: a fully painted raster scored {value:.2f}% "
            "black; the detector invents bands that are not there")

    # The defect's own shape: the outer 12.5% of every row black.
    rows = []
    margin = int(width * 0.125)
    for _ in range(height):
        row = bytearray()
        for x in range(width):
            row += (bytes([0, 0, 0]) if x < margin or x >= width - margin
                    else bytes([255, 128, 0]))
        rows.append(bytes(row))
    banded = b"".join(rows)
    value = sky_black_percent(Frame(width, height, banded), 0)
    if value < 20.0:
        failures.append(
            f"detector self-test: a raster with 25% of every row black scored "
            f"only {value:.2f}%; the detector cannot see the defect it exists "
            "to catch")

    # It must read the bottom viewport, not the top one, for viewport 1.
    split = bytes([255, 128, 0] * (width * height // 2)) \
        + bytes([0, 0, 0] * (width * height // 2))
    top = sky_black_percent(Frame(width, height, split), 0)
    bottom = sky_black_percent(Frame(width, height, split), 1)
    if top > 0.01 or bottom < 99.0:
        failures.append(
            f"detector self-test: viewport selection is wrong (top={top:.1f}% "
            f"bottom={bottom:.1f}%); each half must be scored separately")
    return failures


def run_arm(binary: str, rom: str, root: str, name: str, window: str,
            verbose: bool) -> tuple[str, str, list[str]]:
    frame_dir = os.path.join(root, name)
    os.makedirs(frame_dir, exist_ok=True)
    env = {k: v for k, v in os.environ.items()
           if not k.startswith(("MDKR", "GE007_"))}
    env.update(
        MDKR_AUDIO="0",
        MDKR64_HIDDEN="1",
        MDKR_SIMULATION_CADENCE="original",
        MDKR_SYNTH_FIELDS="1",
        MDKR_TRACE="1",
        MDKR_AUTOPILOT="1",
        MDKR_LOAD_TRACK=LEVEL_FOSSIL_CANYON,
        MDKR_SAVE_DIR=os.path.join(frame_dir, "save"),
        MDKR_DUMP_FROM=str(DUMP_FROM),
        MDKR_DUMP_EVERY=str(DUMP_EVERY),
        MDKR_VIDEO_CONFIG_PATH=os.path.join(frame_dir, "mdkr64.ini"),
    )
    cmd = [binary, "--headless-frames", str(FRAMES),
           "--window-size", window,
           "--input-script", SCRIPT,
           "--dump-frames", frame_dir,
           "--rom", rom]
    if verbose:
        print("$ " + " ".join(cmd))
    proc = subprocess.run(cmd, capture_output=True, text=True, env=env)
    out = proc.stdout + proc.stderr
    failures = []
    if proc.returncode != 0:
        failures.append(f"{name}: exit code {proc.returncode}")
    for marker in ("[CRASH]", "[FATAL]"):
        if marker in out:
            failures.append(f"{name}: {marker} in output")
    if "hud_init: hudPlayers=1 numViewports=2" not in out:
        failures.append(
            f"{name}: never reached two-player split screen; the fixture "
            "did not exercise the multiplayer backdrop path at all")
    if f"level_load: levelId={LEVEL_FOSSIL_CANYON} numPlayers=1" not in out:
        failures.append(
            f"{name}: Fossil Canyon never loaded as a two-player race")
    return frame_dir, out, failures


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", default="build")
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    parser.add_argument("--keep-frames")
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    binary = resolve_binary(args.build)
    for path in (binary, args.rom, SCRIPT):
        if not os.path.exists(path):
            print(f"FAIL: missing {path}", file=sys.stderr)
            return 1

    failures = detector_self_test()
    if failures:
        print("FAIL: split-screen backdrop check (detector self-test)")
        for failure in failures:
            print(f"  - {failure}")
        return 1

    root = args.keep_frames or tempfile.mkdtemp(prefix="mdkr_bg_")
    os.makedirs(root, exist_ok=True)

    for window in (PURE_SIZE, WIDE_SIZE):
        name = window.replace("x", "_")
        frame_dir, out, problems = run_arm(
            binary, args.rom, root, name, window, args.verbose)
        failures.extend(problems)
        if problems:
            continue

        # --- arm 1: the derived extent -----------------------------------
        rows = [line for line in out.splitlines() if "bg_backdrop:" in line]
        if not rows:
            failures.append(
                f"{name}: no [TRACE] bg_backdrop row; the split-screen "
                "backdrop never derived an extent")
        else:
            fields = dict(
                token.split("=", 1)
                for token in rows[0].split("bg_backdrop:")[1].split())
            got = (int(fields["x"]), int(fields["y"]))
            want = EXPECTED_EXTENT[window]
            if got != want:
                detail = ("this is the retail-parity rail: at 4:3 the derived "
                          "quad must be exactly the ROM's authored one"
                          if window == PURE_SIZE else
                          "the quad must track the Hor+ frustum, x = y * "
                          "aspect")
                failures.append(
                    f"{name}: backdrop extent x={got[0]} y={got[1]}, "
                    f"expected x={want[0]} y={want[1]} ({detail}); "
                    f"row: {rows[0].strip()}")
            elif args.verbose:
                print(f"  {name}: {rows[0].strip()}")

        # --- arm 2: sky coverage, widescreen only ------------------------
        if window != WIDE_SIZE:
            continue
        dumps = sorted(
            (f for f in os.listdir(frame_dir) if f.endswith(".ppm")),
            key=lambda f: int("".join(c for c in f if c.isdigit())))
        if len(dumps) < 4:
            failures.append(
                f"{name}: only {len(dumps)} frame dumps; the sky band was "
                "never sampled")
            continue
        worst = {0: (0.0, ""), 1: (0.0, "")}
        for dump in dumps:
            frame = read_ppm(os.path.join(frame_dir, dump))
            for viewport in (0, 1):
                value = sky_black_percent(frame, viewport)
                if value > worst[viewport][0]:
                    worst[viewport] = (value, dump)
        for viewport in (0, 1):
            value, dump = worst[viewport]
            ceiling = SKY_BLACK_CEILING[viewport]
            if args.verbose:
                print(f"  {name}: viewport {viewport} worst sky-band black "
                      f"{value:.2f}% ({dump}), ceiling {ceiling}%")
            if value > ceiling:
                failures.append(
                    f"{name}: viewport {viewport} sky band is {value:.2f}% "
                    f"pure black at {dump} (ceiling {ceiling}%) -- the "
                    "backdrop quad does not reach the frustum edge, so the "
                    "sides of the sky are the unpainted clear colour")

    if args.keep_frames:
        print(f"  artifacts: {root}")

    if failures:
        print("FAIL: split-screen backdrop check")
        for failure in failures:
            print(f"  - {failure}")
        return 1
    print("PASS: split-screen backdrop -- the multiplayer sky quad spans the "
          "widescreen frustum with no unpainted sides, and a 4:3 presentation "
          "still derives the ROM's authored 200x150 extent")
    return 0


if __name__ == "__main__":
    sys.exit(main())
