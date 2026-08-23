#!/usr/bin/env python3
"""Issue #52: no screen-wide band while switching paks in Save Options.

During the Save Options pak-switch scroll, a menu label partly off the left
edge can produce a glyph whose right edge lands exactly at x=0.  The retail
clamp in font.c (`textureUlx < 0 && textureLrx > 0`, strictly greater) misses
that glyph, so the negative ulx wraps into the unsigned 12-bit GBI field and
the emitted G_TEXRECT is INVERTED: ul=(4064,y) lr=(0,y+48).  Real RDP hardware
marks every scanline of such a span invalid (xleft > xright) and draws zero
pixels; the port used to rasterize it as a full-width sliver of stretched font
texture -- a thin pale pink/orange band washing over the "GAME PAK" label row.

This check drives the reproduction route and closes three contracts:

* no dumped frame carries the band: no row anywhere in the frame has a
  single-colour run (Manhattan tolerance 60) wider than 55% of the frame whose
  colour is neither sky-blue nor wood-brown -- the calibrated detector had
  zero false positives over the whole route and flags exactly the two known
  band frames (2410 and 2415) on an unguarded build;
* the guard does not over-skip: every dumped frame still shows the sky, the
  wood panels, and the pink "GAME PAK" glyph pixels (every DKR glyph is a
  TEXRECT, so a guard that skipped legitimate rectangles would erase the
  labels long before anything else);
* non-vacuity: the `[RECT-SKIP]` counter is > 0, i.e. the route really made
  the game emit inverted rectangles and the interpreter really refused them.
  Without this a route drift could turn the pixel assertion vacuous.

The fixture is a checksum-valid adventure save (borrowed from
check_adventure_two.py) so the Save Options top row has three entries and the
scroll crosses the off-screen-left geometry that emits the degenerate glyph.

The PAL arm runs only when baserom.pal.v80.z64 is present (the defect and the
fix were proven on both revisions; only the label row's y shifts).  No
developer save or video config is read or changed: every process runs against
a scratch save directory with a pinned MDKR_VIDEO_CONFIG_PATH.
"""

from __future__ import annotations

import argparse
import importlib.util
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

from harness_utils import DEFAULT_BUILD_DIR, resolve_binary

BASE_SCRIPT = "tests/input_scripts/nav_to_save_options.txt"
# The reproduction extends the Save Options route with pak-switch scrolls in
# both directions; the band flashed at dump frames 2410/2415 on both
# revisions before the guard.
PAK_SWITCH_PRESSES = ("2150 RIGHT 4", "2250 RIGHT 4", "2400 LEFT 4",
                      "2500 LEFT 4")
FRAMES = 2700
DUMP_FROM = 2100
DUMP_EVERY = 5

RUN_TOLERANCE = 60          # Manhattan RGB tolerance within one run
RUN_WIDTH_FRACTION = 0.55   # a band run spans well over half the frame

# Control floors, calibrated over all 120 dumped frames of the route (top
# third of the frame): sky coverage never measured below 0.58, wood below
# 0.16, pink glyph samples below 712.  The floors sit far under those minima
# so presentation jitter cannot trip them, while a guard that started eating
# legitimate rectangles (glyphs are TEXRECTs) would fall straight through.
CONTROL_MIN_SKY = 0.30
CONTROL_MIN_WOOD = 0.05
CONTROL_MIN_TEXT_SAMPLES = 200

MENU_RE = re.compile(r"menu_init: menuId=(\d+) @frame~(\d+)")
RECT_SKIP_RE = re.compile(r"\[RECT-SKIP\] skipped=(\d+)")


def eeprom_fixture() -> bytes:
    """The valid adventure save from check_adventure_two (adventure_two=False)."""
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                        "check_adventure_two.py")
    spec = importlib.util.spec_from_file_location("mdkr_check_adventure_two",
                                                  path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module.eeprom_image(False)


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


def is_sky(r: int, g: int, b: int) -> bool:
    return b > 150 and b > r + 40 and g > r


def is_wood(r: int, g: int, b: int) -> bool:
    return r > b + 30 and g > b and r > g


def is_pink_glyph(r: int, g: int, b: int) -> bool:
    return r > 180 and b > 120 and g < r - 60 and g < b


def row_longest_run(pixels: bytes, width: int, y: int) -> tuple[int, tuple[int, int, int]]:
    """Longest run of near-identical pixels in one row, plus its anchor colour."""
    base = y * width * 3
    best_len = 0
    best_color = (0, 0, 0)
    run_len = 0
    anchor: tuple[int, int, int] | None = None
    for x in range(width):
        offset = base + x * 3
        c = (pixels[offset], pixels[offset + 1], pixels[offset + 2])
        if (anchor is not None
                and abs(c[0] - anchor[0]) + abs(c[1] - anchor[1])
                + abs(c[2] - anchor[2]) <= RUN_TOLERANCE):
            run_len += 1
        else:
            anchor = c
            run_len = 1
        if run_len > best_len:
            best_len = run_len
            best_color = anchor
    return best_len, best_color


def band_rows(pixels: bytes, width: int, height: int) -> list[tuple[int, int, tuple[int, int, int]]]:
    """Rows whose widest run is band-like: >55% of the frame, neither sky nor wood."""
    rows = []
    for y in range(height):
        length, color = row_longest_run(pixels, width, y)
        if length > RUN_WIDTH_FRACTION * width and \
                not is_sky(*color) and not is_wood(*color):
            rows.append((y, length, color))
    return rows


def control_metrics(pixels: bytes, width: int, height: int) -> tuple[float, float, int]:
    """Sky/wood coverage fractions and pink-glyph sample count, top third."""
    sky = wood = text = 0
    for y in range(height // 3):
        base = y * width * 3
        for x in range(0, width, 4):
            offset = base + x * 3
            r, g, b = pixels[offset], pixels[offset + 1], pixels[offset + 2]
            if is_sky(r, g, b):
                sky += 1
            elif is_wood(r, g, b):
                wood += 1
            elif is_pink_glyph(r, g, b):
                text += 1
    total = (height // 3) * ((width + 3) // 4)
    return sky / total, wood / total, text


def run_route(binary: str, rom: str, keep_frames: str | None,
              verbose: bool) -> tuple[str, int | str, dict[str, bytes]]:
    with tempfile.TemporaryDirectory(prefix="mdkr_saveopt_band_") as run_dir:
        root = Path(run_dir)
        save_dir = root / "save"
        save_dir.mkdir()
        (save_dir / "eeprom.bin").write_bytes(eeprom_fixture())
        frame_dir = Path(keep_frames) if keep_frames else root / "frames"
        frame_dir.mkdir(parents=True, exist_ok=True)

        script = root / "route.txt"
        base = Path(BASE_SCRIPT).read_text(encoding="utf-8")
        script.write_text(base + "\n".join(PAK_SWITCH_PRESSES) + "\n",
                          encoding="utf-8")

        env = clean_env()
        env.update(
            MDKR_AUDIO="0",
            MDKR_TRACE="1",   # menu witnesses + the [RECT-SKIP] teardown row
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


def validate_arm(label: str, output: str, rc: int | str,
                 captures: dict[str, bytes]) -> list[str]:
    failures: list[str] = []
    if rc != 0:
        failures.append(f"{label}: exit={rc}")
    for marker in ("[CRASH]", "[FATAL]"):
        if marker in output:
            line = next((l for l in output.splitlines() if marker in l), marker)
            failures.append(f"{label}: {marker} in output: {line.strip()}")

    # Route integrity: OPTIONS (12) then SAVE OPTIONS (14) must both open,
    # or the scroll never happened and every pixel assertion below is inert.
    menu_ids = [int(m.group(1)) for m in MENU_RE.finditer(output)]
    for want, name in ((12, "OPTIONS"), (14, "SAVE_OPTIONS")):
        if want not in menu_ids:
            failures.append(
                f"{label}: never reached menuId={want} ({name}); saw {menu_ids}")
    if len(captures) < 100:
        failures.append(
            f"{label}: only {len(captures)} dumped frame(s), expected ~120")

    banded: list[str] = []
    weak_controls: list[str] = []
    for name, payload in captures.items():
        try:
            width, height, pixels = ppm(payload)
        except ValueError as exc:
            failures.append(f"{label}: {name}: {exc}")
            continue
        rows = band_rows(pixels, width, height)
        if rows:
            y0, length, color = rows[0]
            banded.append(f"{name} ({len(rows)} row(s), first y={y0} "
                          f"run={length}/{width} colour={color})")
        sky, wood, text = control_metrics(pixels, width, height)
        if sky < CONTROL_MIN_SKY or wood < CONTROL_MIN_WOOD or \
                text < CONTROL_MIN_TEXT_SAMPLES:
            weak_controls.append(
                f"{name} (sky={sky:.3f} wood={wood:.3f} text={text})")
    if banded:
        failures.append(
            f"{label}: screen-wide band detected in {len(banded)} frame(s): "
            + "; ".join(banded))
    if weak_controls:
        failures.append(
            f"{label}: known-good Save Options pixels missing -- the guard may "
            f"be skipping legitimate rectangles: " + "; ".join(weak_controls))

    # Non-vacuity: the route must have made the game emit inverted rectangles
    # and the interpreter must have refused every one of them. Zero would mean
    # the route no longer exercises the defect -- fix the route, do not relax
    # this bound.
    match = RECT_SKIP_RE.search(output)
    if match is None:
        failures.append(
            f"{label}: no [RECT-SKIP] row -- either the run never reached "
            "teardown or the interpreter lacks the inverted-rectangle guard")
    elif int(match.group(1)) <= 0:
        failures.append(
            f"{label}: [RECT-SKIP] skipped=0 -- the route emitted no inverted "
            "rectangle, so the band assertion proved nothing")
    else:
        print(f"  {label}: [RECT-SKIP] skipped={match.group(1)}, "
              f"{len(captures)} frames clean")
    return failures


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", default=DEFAULT_BUILD_DIR)
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    parser.add_argument("--pal-rom", default="baserom.pal.v80.z64",
                        help="optional PAL arm; skipped when absent")
    parser.add_argument("--keep-frames", default=None,
                        help="optional directory for the US arm's captures")
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    binary = os.path.abspath(resolve_binary(args.build))
    for path in (binary, args.rom, BASE_SCRIPT):
        if not os.path.exists(path):
            print(f"FAIL: missing {path}", file=sys.stderr)
            return 1

    failures: list[str] = []
    arms = [("us", args.rom, args.keep_frames)]
    if os.path.exists(args.pal_rom):
        arms.append(("pal", args.pal_rom, None))
    else:
        print(f"  pal: skipped ({args.pal_rom} not present)")

    for label, rom, keep in arms:
        output, rc, captures = run_route(binary, rom, keep, args.verbose)
        failures.extend(validate_arm(label, output, rc, captures))

    if failures:
        print(f"FAIL: save options scroll band check ({len(failures)} issue(s))")
        for failure in failures:
            print("  - " + failure)
        return 1
    print("PASS: save options pak-switch scroll draws no inverted-rectangle "
          "band, labels and panels intact, guard non-vacuous")
    return 0


if __name__ == "__main__":
    sys.exit(main())
