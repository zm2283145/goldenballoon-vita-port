#!/usr/bin/env python3
"""Pixel-level layout gate for the opt-in widescreen HUD (issue #51).

check_widescreen_proportions.py pins the DEFAULT HUD (widescreen HUD off,
SAFE_2D) and check_widescreen_hud_scope.py pins the source shape, but until
this check nothing rendered a single frame with Video.WidescreenHUD enabled --
the entire ON-path layout was unpinned, and release 1.5.x shipped four broken
layouts behind the option.  This check drives the three reporter fixtures and
asserts the pixel geometry of each fix:

  * TT rows (race_full_3lap_tt.txt, capture evidence-selected from the ON
    run's own lap-2 [PACE] transition): the banked-lap rows are one visual
    block on the right -- the yellow "LAP n" labels sit strictly left of the
    lap-time digits (pre-fix the labels were stamped over the middle of the
    times), the digits reach the right column, and the TT lap counter sits at
    the left presentation edge instead of floating mid-screen.  The lap-2 row
    (green digits) is the measured witness: the lap-1 row's red digits share
    a palette with Ancient Lake's canyon walls and cannot be segmented
    honestly.

  * Race-start hold + Taj label (taj_unlock_select.txt, capture anchored to
    the first frame the identity label appears): during the pre-slide hold
    the ON arm's right expanded gutter must be pixel-identical to the
    world-only OFF arm (pre-fix the LEFT/CENTER groups parked inside the
    wide canvas -- the position numeral sat visibly in the gutter), and the
    magenta "TAJ MAGIC" label centroid must sit at the presentation center
    (pre-fix: 0.375 of width, one margin left).

  * Battle strip (the check_challenge_modes.py recipe: Adventure-save
    fixture + adventure_resume_race.txt + MDKR_LOAD_TRACK=27 Icicle
    Pyramid): the four-racer banana-tally strip must stay authored-centered
    -- its yellow-cluster bounding-box centroid matches the OFF arm's within
    tolerance (pre-fix: flush left at 0.36 of width vs 0.49 centered).

  * 4:3 parity rail: at a 4:3 presentation the ON arm must be BYTE-IDENTICAL
    to the OFF arm (the anchor offset formula returns zero at aspect <= 4:3
    and the slide scale is exactly 1), so enabling the option can never
    change a non-wide picture.

Every wide arm also requires the ON and OFF runs to agree on the exact
normalized [PACE] row at the capture frame, proving the option changed only
presentation.  Detector self-tests run first on synthetic rasters so each
segmenting assertion is proven able to fail before any verdict is trusted.

Usage:
    python3 tests/check_widescreen_hud_layers.py \
        --build build --rom baserom.us.v80.z64 -v
"""

from __future__ import annotations

import argparse
import os
import re
import shlex
import subprocess
import sys
import tempfile
from contextlib import nullcontext
from dataclasses import dataclass, field
from pathlib import Path

from check_adventure_two import eeprom_image
from harness_utils import (DEFAULT_BUILD_DIR, read_ppm as read_ppm_bytes,
                           resolve_binary)

REPO = Path(__file__).resolve().parent.parent
TT_SCRIPT = REPO / "tests" / "input_scripts" / "race_full_3lap_tt.txt"
TAJ_SCRIPT = REPO / "tests" / "input_scripts" / "taj_unlock_select.txt"
BATTLE_SCRIPT = REPO / "tests" / "input_scripts" / "adventure_resume_race.txt"

FATAL_MARKERS = ("[CRASH]", "[FATAL]", "AddressSanitizer", "runtime error:")

PACE_RE = re.compile(r"\[PACE\] frame=(\d+) .*")
PACE_LAP_RE = re.compile(r"\[PACE\] frame=(\d+) .*? lap=(\d+) rlap=")
PACE_CLOCK_RE = re.compile(r"\[PACE\] frame=(\d+) .*? clock=(\d+) ")

# TT fixture: race live ~2643; lap 1 banks ~4404 and lap 2 ~5908 (this run's
# own [PACE] rows select the capture, these literals only bound the dumps).
TT_FRAMES = 6200
TT_DUMP_FROM = 5900
TT_DUMP_EVERY = 50
TT_SETTLE_AFTER_LAP2 = 60          # let the LAP2 overlay finish sliding

# Taj fixture: the mod-racer identity label draws for the whole race scene
# and the race-start HUD stays parked (gHudOffsetX = 320) until the intro
# camera pan hands over (~220 frames after the label fades in, on the
# current fixture); the capture is anchored to the label's first dumped
# appearance plus a short settle, and a drift guard proves the OFF arm has
# no landed HUD yet -- after the slide lands, RIGHT-anchored elements sit in
# the gutter BY DESIGN and the gutter assertion would be meaningless.
TAJ_FRAMES = 5600
TAJ_DUMP_FROM = 5250
TAJ_DUMP_EVERY = 30
TAJ_SETTLE_AFTER_LABEL = 60        # inside the pre-slide hold
# Landed-HUD drift guard band: the top-right of the OFF arm's safe area,
# where the TIME column and banana counter appear once the slide lands.
TAJ_LANDED_BAND = (0.60, 0.04, 0.85, 0.20)

# Battle fixture: arena live from ~2100, strip stable at the capture.
BATTLE_FRAMES = 2600
BATTLE_CAPTURE = 2500
BATTLE_COURSE = 27                 # Icicle Pyramid

WIDE_SIZE = "960x540"
PURE_SIZE = "720x540"

# Segmentation predicates, calibrated on the 2x-scaled dumps of all three
# fixtures (pre-fix and post-fix): HUD yellows are (255,224,~50)-bright,
# the lap-2 digits are the pure green lap colour, the identity label is the
# funfont magenta ramp.  All geometry below is in width/height fractions.
def is_yellow(r: int, g: int, b: int) -> bool:
    return r > 200 and g > 150 and b < 110


def is_green(r: int, g: int, b: int) -> bool:
    return g > 170 and r < 120 and b < 130


def is_magenta(r: int, g: int, b: int) -> bool:
    return r > 180 and b > 150 and g < 130


# TT geometry.
TT_ROW_BAND = (0.55, 0.17, 1.00, 0.32)      # banked-lap rows, top right
TT_LAPCTR_BAND = (0.00, 0.06, 0.55, 0.20)   # lap counter, top left..center
TT_DIGITS_MIN_RIGHT = 0.90                  # digits right edge, frac of W
TT_LAPCTR_MAX_LEFT = 0.13                   # lap counter left edge, frac of W
TT_MIN_DIGIT_AREA = 200
TT_MIN_LABEL_AREA = 200
TT_MIN_LAPCTR_AREA = 150

# Taj geometry.
TAJ_LABEL_BAND_HEIGHT = 0.12                # identity label band, frac of H
TAJ_LABEL_TOLERANCE = 0.02                  # |centroid - W/2|, frac of W
GUTTER_LEFT = 0.88                          # right expanded gutter starts
GUTTER_DIFF_CHANNEL = 30                    # per-channel difference floor
GUTTER_MAX_DIFF_PIXELS = 400                # pre-fix signature: ~3200

# Battle geometry.
BATTLE_STRIP_BAND = (0.00, 0.19, 1.00, 0.31)
BATTLE_MIN_CLUSTER_AREA = 60
BATTLE_MIN_CLUSTERS = 8                     # 4 racers x (banana, x, digits)
BATTLE_CENTROID_TOLERANCE = 0.02            # |on - off|, frac of W


@dataclass(frozen=True)
class Image:
    width: int
    height: int
    pixels: bytes


@dataclass(frozen=True)
class Component:
    x0: int
    y0: int
    x1: int
    y1: int
    area: int


@dataclass
class Run:
    label: str
    output: str = ""
    frames: dict[int, Path] = field(default_factory=dict)


def read_ppm(path: Path) -> Image:
    return Image(*read_ppm_bytes(path))


def components(image: Image, pred, bounds: tuple[float, float, float, float],
               min_area: int) -> list[Component]:
    x0 = round(image.width * bounds[0])
    y0 = round(image.height * bounds[1])
    x1 = round(image.width * bounds[2])
    y1 = round(image.height * bounds[3])
    pts: set[tuple[int, int]] = set()
    for y in range(max(0, y0), min(image.height, y1)):
        row = y * image.width * 3
        for x in range(max(0, x0), min(image.width, x1)):
            index = row + x * 3
            if pred(image.pixels[index], image.pixels[index + 1],
                    image.pixels[index + 2]):
                pts.add((x, y))
    comps: list[Component] = []
    while pts:
        seed = pts.pop()
        stack = [seed]
        members = [seed]
        while stack:
            x, y = stack.pop()
            for dx in (-1, 0, 1):
                for dy in (-1, 0, 1):
                    neighbour = (x + dx, y + dy)
                    if neighbour in pts:
                        pts.remove(neighbour)
                        stack.append(neighbour)
                        members.append(neighbour)
        if len(members) >= min_area:
            xs = [p[0] for p in members]
            ys = [p[1] for p in members]
            comps.append(Component(min(xs), min(ys), max(xs), max(ys),
                                   len(members)))
    return sorted(comps, key=lambda c: (c.x0, c.y0))


def gutter_diff(on: Image, off: Image) -> int:
    """Pixels in the right expanded gutter differing between the arms."""
    if (on.width, on.height) != (off.width, off.height):
        return on.width * on.height  # incomparable: fail loudly
    start = round(on.width * GUTTER_LEFT)
    differing = 0
    for y in range(on.height):
        row = y * on.width * 3
        for x in range(start, on.width):
            index = row + x * 3
            if (abs(on.pixels[index] - off.pixels[index]) > GUTTER_DIFF_CHANNEL
                    or abs(on.pixels[index + 1] - off.pixels[index + 1])
                    > GUTTER_DIFF_CHANNEL
                    or abs(on.pixels[index + 2] - off.pixels[index + 2])
                    > GUTTER_DIFF_CHANNEL):
                differing += 1
    return differing


# ---------------------------------------------------------------------------
# Arm analyses (pure functions of images, so the self-tests can drive them
# with synthetic rasters).

def analyze_tt_rows(image: Image, label: str) -> list[str]:
    failures: list[str] = []
    digits = [c for c in components(image, is_green, TT_ROW_BAND,
                                    TT_MIN_DIGIT_AREA)]
    if len(digits) < 4:
        return [f"{label}: lap-2 digit clusters not found "
                f"(got {len(digits)}, need >= 4) in the banked-row band"]
    digits_x0 = min(c.x0 for c in digits)
    digits_x1 = max(c.x1 for c in digits)
    digits_y0 = min(c.y0 for c in digits)
    digits_y1 = max(c.y1 for c in digits)

    def row_overlap(c: Component) -> float:
        overlap = (min(c.y1, digits_y1) - max(c.y0, digits_y0) + 1)
        return overlap / (c.y1 - c.y0 + 1)

    labels = [c for c in components(image, is_yellow, TT_ROW_BAND,
                                    TT_MIN_LABEL_AREA)
              if row_overlap(c) >= 0.5]
    if len(labels) < 2:
        failures.append(f"{label}: LAP label clusters not found in the "
                        f"lap-2 row (got {len(labels)}, need >= 2)")
    else:
        label_x1 = max(c.x1 for c in labels)
        if label_x1 >= digits_x0:
            failures.append(
                f"{label}: the LAP label overprints/overlaps the lap time "
                f"(label right edge x={label_x1} is not left of the digit "
                f"block starting x={digits_x0}); labels and transient "
                "digits do not share one anchor treatment")
    if digits_x1 < image.width * TT_DIGITS_MIN_RIGHT:
        failures.append(
            f"{label}: lap-time digits end at x={digits_x1} "
            f"({digits_x1 / image.width:.3f} of width), short of the "
            f"authored right column (>= {TT_DIGITS_MIN_RIGHT:.2f})")
    return failures


def analyze_tt_lap_counter(image: Image, label: str) -> list[str]:
    clusters = components(image, is_yellow, TT_LAPCTR_BAND,
                          TT_MIN_LAPCTR_AREA)
    if not clusters:
        return [f"{label}: TT lap counter not found in the top-left band"]
    leftmost = min(c.x0 for c in clusters)
    if leftmost > image.width * TT_LAPCTR_MAX_LEFT:
        return [
            f"{label}: TT lap counter floats at x={leftmost} "
            f"({leftmost / image.width:.3f} of width) instead of riding the "
            f"left presentation edge (<= {TT_LAPCTR_MAX_LEFT:.2f})"]
    return []


def label_centroid(image: Image) -> float | None:
    clusters = components(
        image, is_magenta,
        (0.0, 0.0, 1.0, TAJ_LABEL_BAND_HEIGHT), 25)
    if not clusters:
        return None
    x0 = min(c.x0 for c in clusters)
    x1 = max(c.x1 for c in clusters)
    return (x0 + x1) / 2 / image.width


def analyze_taj_label(image: Image, label: str) -> list[str]:
    centroid = label_centroid(image)
    if centroid is None:
        return [f"{label}: identity label not found in the top band"]
    if abs(centroid - 0.5) > TAJ_LABEL_TOLERANCE:
        return [
            f"{label}: identity label centroid at {centroid:.3f} of width, "
            f"expected the presentation center 0.500 +/- "
            f"{TAJ_LABEL_TOLERANCE}"]
    return []


def strip_centroid(image: Image, label: str) -> tuple[float | None, int,
                                                      list[str]]:
    clusters = components(image, is_yellow, BATTLE_STRIP_BAND,
                          BATTLE_MIN_CLUSTER_AREA)
    if len(clusters) < BATTLE_MIN_CLUSTERS:
        return None, len(clusters), [
            f"{label}: battle banana-tally strip not found "
            f"(got {len(clusters)} clusters, need >= {BATTLE_MIN_CLUSTERS})"]
    x0 = min(c.x0 for c in clusters)
    x1 = max(c.x1 for c in clusters)
    return (x0 + x1) / 2 / image.width, len(clusters), []


# ---------------------------------------------------------------------------
# Detector self-tests: prove each assertion can fail, on synthetic rasters,
# before any engine verdict is trusted (the proportions-check discipline of
# carrying a known-bad positive control).

def synth(width: int, height: int,
          rects: list[tuple[int, int, int, int, tuple[int, int, int]]]) -> Image:
    pixels = bytearray(width * height * 3)
    for x0, y0, x1, y1, (r, g, b) in rects:
        for y in range(y0, y1):
            row = y * width * 3
            for x in range(x0, x1):
                index = row + x * 3
                pixels[index:index + 3] = bytes((r, g, b))
    return Image(width, height, bytes(pixels))


def self_test() -> list[str]:
    problems: list[str] = []
    width, height = 960, 540
    yellow = (255, 224, 60)
    green = (40, 230, 60)
    pink = (255, 60, 220)

    # TT: the pre-fix signature -- label block overlapping the digit block.
    digits = [(640, 150, 660, 170, green), (670, 150, 690, 170, green),
              (700, 150, 720, 170, green), (730, 150, 750, 170, green)]
    overlapped = synth(width, height, digits +
                       [(665, 150, 705, 170, yellow),
                        (620, 150, 640, 170, yellow)])
    if not analyze_tt_rows(overlapped, "self"):
        problems.append("TT row detector passed an overlapping LAP label")
    short = synth(width, height, digits + [(560, 150, 600, 170, yellow),
                                           (530, 150, 550, 170, yellow)])
    if not any("short of the authored right column" in item
               for item in analyze_tt_rows(short, "self")):
        problems.append("TT row detector passed digits short of the column")
    good = synth(width, height,
                 [(830, 150, 850, 170, green), (860, 150, 880, 170, green),
                  (890, 150, 910, 170, green), (915, 150, 935, 170, green),
                  (740, 150, 780, 170, yellow), (790, 150, 810, 170, yellow)])
    if analyze_tt_rows(good, "self"):
        problems.append("TT row detector rejected a disjoint ordered row")

    floats = synth(width, height, [(300, 50, 360, 90, yellow)])
    if not analyze_tt_lap_counter(floats, "self"):
        problems.append("lap counter detector passed a mid-screen float")
    if analyze_tt_lap_counter(
            synth(width, height, [(90, 50, 150, 90, yellow)]), "self"):
        problems.append("lap counter detector rejected an edge-anchored "
                        "counter")

    off_center = synth(width, height, [(300, 10, 420, 40, pink)])
    if not analyze_taj_label(off_center, "self"):
        problems.append("label detector passed an off-center label")
    if analyze_taj_label(synth(width, height, [(420, 10, 540, 40, pink)]),
                         "self"):
        problems.append("label detector rejected a centered label")

    left_strip = synth(width, height,
                       [(40 + i * 60, 110, 80 + i * 60, 160, yellow)
                        for i in range(8)])
    centered_strip = synth(width, height,
                           [(250 + i * 60, 110, 290 + i * 60, 160, yellow)
                            for i in range(8)])
    left_c, _, left_problems = strip_centroid(left_strip, "self")
    centered_c, _, centered_problems = strip_centroid(centered_strip, "self")
    if left_problems or centered_problems or left_c is None \
            or centered_c is None:
        problems.append("strip detector failed to segment synthetic strips")
    elif abs(left_c - centered_c) <= BATTLE_CENTROID_TOLERANCE:
        problems.append("strip centroid cannot distinguish flush-left from "
                        "centered")

    world = synth(width, height, [(0, 0, width, height, (30, 60, 120))])
    parked = synth(width, height, [(0, 0, width, height, (30, 60, 120)),
                                   (880, 60, 930, 130, yellow)])
    if gutter_diff(parked, world) <= GUTTER_MAX_DIFF_PIXELS:
        problems.append("gutter diff cannot see a parked HUD element")
    if gutter_diff(world, world) != 0:
        problems.append("gutter diff is nonzero for identical frames")
    return problems


# ---------------------------------------------------------------------------
# Engine runs.

def clean_environment(renderer: str | None, scratch: Path,
                      widescreen_hud: bool, dump_from: int, dump_every: int,
                      extra: dict[str, str]) -> dict[str, str]:
    env = {key: value for key, value in os.environ.items()
           if not key.startswith("MDKR")}
    env.update(
        MDKR_AUDIO="0",
        MDKR_SIMULATION_CADENCE="enhanced",
        MDKR_SYNTH_FIELDS="1",
        MDKR_AUTOPILOT="1",
        MDKR_TRACE="1",
        MDKR_NO_CRASH_HANDLER="1",
        MDKR64_HIDDEN="1",
        MDKR_DUMP_FROM=str(dump_from),
        MDKR_DUMP_EVERY=str(dump_every),
        MDKR_VIDEO_CONFIG_PATH=str(scratch / "video.ini"),
        LC_ALL="C",
    )
    if widescreen_hud:
        env["MDKR_WIDESCREEN_HUD"] = "1"
    if renderer:
        env["MDKR_RENDERER"] = renderer
    env.update(extra)
    return env


def run_arm(binary: Path, rom: Path, root: Path, label: str, script: Path,
            frames: int, size: str, widescreen_hud: bool, dump_from: int,
            dump_every: int, renderer: str | None, timeout: int,
            verbose: bool, extra: dict[str, str] | None = None,
            adventure_save: bool = False) -> tuple[Run, list[str]]:
    failures: list[str] = []
    arm_root = root / label
    frame_dir = arm_root / "frames"
    run_dir = arm_root / "run"
    frame_dir.mkdir(parents=True)
    (run_dir / "save").mkdir(parents=True)
    if adventure_save:
        (run_dir / "save" / "eeprom.bin").write_bytes(eeprom_image(False))
    command = [
        str(binary),
        "--headless-frames", str(frames),
        "--window-size", size,
        "--input-script", str(script),
        "--dump-frames", str(frame_dir),
        "--rom", str(rom),
    ]
    if verbose:
        print(f"$ ({label}) " + shlex.join(command), flush=True)
    try:
        proc = subprocess.run(
            command,
            cwd=run_dir,
            env=clean_environment(renderer, arm_root, widescreen_hud,
                                  dump_from, dump_every, extra or {}),
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=timeout,
            check=False,
        )
        output = proc.stdout
        returncode = proc.returncode
    except subprocess.TimeoutExpired as exc:
        output = exc.stdout or ""
        if isinstance(output, bytes):
            output = output.decode("utf-8", "replace")
        returncode = 124
    (arm_root / "run.log").write_text(output, encoding="utf-8")

    if returncode != 0:
        failures.append(f"{label}: exited {returncode}")
    for marker in FATAL_MARKERS:
        if marker in output:
            failures.append(f"{label}: output contains {marker!r}")
    if renderer and f"[mdkr64] renderer backend: {renderer}" not in output:
        failures.append(f"{label}: did not confirm requested {renderer} "
                        "renderer")

    run = Run(label, output)
    for path in frame_dir.glob("frame_*.ppm"):
        match = re.match(r"frame_(\d+)\.ppm$", path.name)
        if match:
            run.frames[int(match.group(1))] = path
    return run, failures


def normalized_pace(output: str, frame: int) -> str | None:
    for candidate in reversed(output.splitlines()):
        match = PACE_RE.search(candidate)
        if match and int(match.group(1)) == frame:
            return re.sub(r" dtms=\S+", " dtms=<wall>",
                          candidate.strip())
    return None


def require_pace_identity(on: Run, off: Run, frame: int) -> list[str]:
    on_pace = normalized_pace(on.output, frame)
    off_pace = normalized_pace(off.output, frame)
    if on_pace is None or off_pace is None:
        return [f"{on.label}/{off.label}: no frame-{frame} [PACE] row "
                "(fixture drift; the arms cannot be compared)"]
    if on_pace != off_pace:
        return [f"{on.label}: frame-{frame} state differs from {off.label} "
                "-- the widescreen HUD option must be presentation-only\n"
                f"    {on.label}: {on_pace}\n    {off.label}: {off_pace}"]
    return []


def load_frame(run: Run, frame: int) -> tuple[Image | None, list[str]]:
    path = run.frames.get(frame)
    if path is None:
        return None, [f"{run.label}: frame {frame} was not dumped "
                      f"(dumped {sorted(run.frames)})"]
    try:
        return read_ppm(path), []
    except ValueError as exc:
        return None, [f"{run.label}: {exc}"]


def lap2_capture_frame(run: Run) -> tuple[int | None, list[str]]:
    """Evidence-selected TT capture: the run's own lap-2 [PACE] transition."""
    transitions: list[tuple[int, int]] = []
    previous: int | None = None
    for frame_text, lap_text in PACE_LAP_RE.findall(run.output):
        lap = int(lap_text)
        if previous is not None and lap != previous:
            transitions.append((int(frame_text), lap))
        previous = lap
    lap2 = [frame for frame, lap in transitions if lap == 2]
    if not lap2:
        return None, [f"{run.label}: no lap-2 [PACE] transition; the TT "
                      "fixture no longer banks two laps (fixture drift)"]
    target = lap2[0] + TT_SETTLE_AFTER_LAP2
    dumped = [frame for frame in sorted(run.frames) if frame >= target]
    if not dumped:
        return None, [f"{run.label}: no dumped frame at/after the lap-2 "
                      f"capture target {target} (dumped "
                      f"{sorted(run.frames)})"]
    return dumped[0], []


def taj_capture_frame(run: Run) -> tuple[int | None, list[str]]:
    """First dumped frame with the identity label, plus the hold settle."""
    for frame in sorted(run.frames):
        image, problems = load_frame(run, frame)
        if problems:
            return None, problems
        if label_centroid(image) is not None:
            target = frame + TAJ_SETTLE_AFTER_LABEL
            candidates = [f for f in sorted(run.frames) if f >= target]
            if not candidates:
                return None, [f"{run.label}: no dumped frame at/after the "
                              f"hold capture target {target}"]
            return candidates[0], []
    return None, [f"{run.label}: identity label never appeared in the "
                  "dumped window (fixture drift)"]


def require_clock(run: Run, frame: int, want_zero: bool) -> list[str]:
    for frame_text, clock_text in PACE_CLOCK_RE.findall(run.output):
        if int(frame_text) == frame:
            clock = int(clock_text)
            if want_zero and clock != 0:
                return [f"{run.label}: frame {frame} has race clock={clock}, "
                        "not the pre-slide hold (fixture drift; re-anchor "
                        "the capture)"]
            if not want_zero and clock == 0:
                return [f"{run.label}: frame {frame} has race clock=0; the "
                        "arena is not live (fixture drift)"]
            return []
    return [f"{run.label}: no [PACE] racer row at frame {frame} "
            "(fixture drift)"]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", default=DEFAULT_BUILD_DIR)
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    parser.add_argument("--renderer", choices=("gl", "webgpu"), default=None)
    parser.add_argument("--timeout", type=int, default=240,
                        help="seconds per arm")
    parser.add_argument("--keep-frames", type=Path)
    parser.add_argument("--self-test", action="store_true",
                        help="only run the detector self-tests")
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    # Detector proof first, every invocation: a verdict from a detector that
    # cannot fail is not a verdict.
    detector_problems = self_test()
    if detector_problems:
        print("FAIL: widescreen HUD layer detectors")
        for problem in detector_problems:
            print(f"  - {problem}")
        return 1
    if args.self_test:
        print("check_widescreen_hud_layers: detector self-test OK")
        return 0

    binary = Path(resolve_binary(args.build)).resolve()
    rom = Path(args.rom).expanduser().resolve()
    if not binary.is_file():
        print(f"FAIL: binary not found: {binary}", file=sys.stderr)
        return 2
    if not rom.is_file():
        print(f"FAIL: ROM not found: {rom}", file=sys.stderr)
        return 2
    if args.timeout <= 0:
        print("FAIL: --timeout must be positive", file=sys.stderr)
        return 2

    if args.keep_frames:
        root_dir = args.keep_frames.expanduser().resolve()
        try:
            root_dir.mkdir(parents=True)
        except FileExistsError:
            print(f"FAIL: --keep-frames directory already exists: {root_dir}",
                  file=sys.stderr)
            return 2
        context = nullcontext(str(root_dir))
    else:
        context = tempfile.TemporaryDirectory(
            prefix="mdkr_widescreen_hud_layers_")

    failures: list[str] = []
    with context as root_text:
        root = Path(root_text)
        battle_env = {"MDKR_LOAD_TRACK": str(BATTLE_COURSE),
                      "MDKR_CHALLENGE_OUTCOME": "win"}

        def arm(label: str, script: Path, frames: int, size: str,
                wide: bool, dump_from: int, dump_every: int,
                extra: dict[str, str] | None = None,
                adventure_save: bool = False) -> Run:
            run, problems = run_arm(
                binary, rom, root, label, script, frames, size, wide,
                dump_from, dump_every, args.renderer, args.timeout,
                args.verbose, extra, adventure_save)
            failures.extend(problems)
            return run

        # --- TT rows -----------------------------------------------------
        tt_on = arm("tt-16x9-on", TT_SCRIPT, TT_FRAMES, WIDE_SIZE, True,
                    TT_DUMP_FROM, TT_DUMP_EVERY)
        tt_off = arm("tt-16x9-off", TT_SCRIPT, TT_FRAMES, WIDE_SIZE, False,
                     TT_DUMP_FROM, TT_DUMP_EVERY)
        capture, problems = lap2_capture_frame(tt_on)
        failures.extend(problems)
        if capture is not None:
            capture_off, problems = lap2_capture_frame(tt_off)
            failures.extend(problems)
            if capture_off is not None and capture_off != capture:
                failures.append(
                    f"tt: ON/OFF lap-2 captures differ ({capture} vs "
                    f"{capture_off}); the arms are not comparable")
            failures.extend(require_pace_identity(tt_on, tt_off, capture))
            image, problems = load_frame(tt_on, capture)
            failures.extend(problems)
            if image is not None:
                failures.extend(analyze_tt_rows(image, tt_on.label))
                failures.extend(analyze_tt_lap_counter(image, tt_on.label))
                if args.verbose:
                    print(f"  tt capture frame {capture}")

        # --- Race-start hold + Taj label ---------------------------------
        taj_on = arm("taj-16x9-on", TAJ_SCRIPT, TAJ_FRAMES, WIDE_SIZE, True,
                     TAJ_DUMP_FROM, TAJ_DUMP_EVERY)
        taj_off = arm("taj-16x9-off", TAJ_SCRIPT, TAJ_FRAMES, WIDE_SIZE,
                      False, TAJ_DUMP_FROM, TAJ_DUMP_EVERY)
        capture, problems = taj_capture_frame(taj_on)
        failures.extend(problems)
        if capture is not None:
            failures.extend(require_clock(taj_on, capture, want_zero=True))
            failures.extend(require_pace_identity(taj_on, taj_off, capture))
            on_image, problems = load_frame(taj_on, capture)
            failures.extend(problems)
            off_image, problems = load_frame(taj_off, capture)
            failures.extend(problems)
            if off_image is not None and components(
                    off_image, is_yellow, TAJ_LANDED_BAND, 150):
                # Not a layout verdict: the capture frame is no longer in
                # the pre-slide hold, so the gutter geometry below cannot be
                # interpreted.  Fail as fixture drift.
                failures.append(
                    f"{taj_off.label}: landed HUD visible at capture frame "
                    f"{capture}; the pre-slide hold moved (fixture drift; "
                    "re-anchor TAJ_SETTLE_AFTER_LABEL)")
                off_image = None
            if on_image is not None:
                failures.extend(analyze_taj_label(on_image, taj_on.label))
            if on_image is not None and off_image is not None:
                # OFF sanity: the same label must be centered there too, so
                # a font/palette change cannot silently blind the detector.
                failures.extend(analyze_taj_label(off_image, taj_off.label))
                differing = gutter_diff(on_image, off_image)
                if args.verbose:
                    print(f"  taj capture frame {capture}, gutter diff "
                          f"{differing}")
                if differing > GUTTER_MAX_DIFF_PIXELS:
                    failures.append(
                        f"{taj_on.label}: {differing} pixels of HUD in the "
                        "right expanded gutter during the pre-slide hold "
                        f"(> {GUTTER_MAX_DIFF_PIXELS}); the race-start "
                        "slide must originate offscreen in the widescreen "
                        "frame")

        # --- Battle strip -------------------------------------------------
        bat_on = arm("battle-16x9-on", BATTLE_SCRIPT, BATTLE_FRAMES,
                     WIDE_SIZE, True, BATTLE_CAPTURE, 99999,
                     battle_env, adventure_save=True)
        bat_off = arm("battle-16x9-off", BATTLE_SCRIPT, BATTLE_FRAMES,
                      WIDE_SIZE, False, BATTLE_CAPTURE, 99999,
                      battle_env, adventure_save=True)
        failures.extend(require_clock(bat_on, BATTLE_CAPTURE,
                                      want_zero=False))
        failures.extend(require_pace_identity(bat_on, bat_off,
                                              BATTLE_CAPTURE))
        on_image, problems = load_frame(bat_on, BATTLE_CAPTURE)
        failures.extend(problems)
        off_image, problems = load_frame(bat_off, BATTLE_CAPTURE)
        failures.extend(problems)
        if on_image is not None and off_image is not None:
            on_centroid, on_count, problems = strip_centroid(
                on_image, bat_on.label)
            failures.extend(problems)
            off_centroid, off_count, problems = strip_centroid(
                off_image, bat_off.label)
            failures.extend(problems)
            if on_centroid is not None and off_centroid is not None:
                if args.verbose:
                    print(f"  battle strip centroid on={on_centroid:.3f} "
                          f"off={off_centroid:.3f}")
                if on_count != off_count:
                    failures.append(
                        f"battle: strip cluster counts differ (on "
                        f"{on_count} vs off {off_count})")
                if abs(on_centroid - off_centroid) \
                        > BATTLE_CENTROID_TOLERANCE:
                    failures.append(
                        f"{bat_on.label}: strip centroid "
                        f"{on_centroid:.3f} of width vs the authored "
                        f"center {off_centroid:.3f} (tolerance "
                        f"{BATTLE_CENTROID_TOLERANCE}); the challenge "
                        "strip must stay centered, not hug the left edge")

        # --- 4:3 parity rail ----------------------------------------------
        pure_on = arm("battle-4x3-on", BATTLE_SCRIPT, BATTLE_FRAMES,
                      PURE_SIZE, True, BATTLE_CAPTURE, 99999,
                      battle_env, adventure_save=True)
        pure_off = arm("battle-4x3-off", BATTLE_SCRIPT, BATTLE_FRAMES,
                       PURE_SIZE, False, BATTLE_CAPTURE, 99999,
                       battle_env, adventure_save=True)
        on_image, problems = load_frame(pure_on, BATTLE_CAPTURE)
        failures.extend(problems)
        off_image, problems = load_frame(pure_off, BATTLE_CAPTURE)
        failures.extend(problems)
        if on_image is not None and off_image is not None:
            if (on_image.width, on_image.height) \
                    != (off_image.width, off_image.height):
                failures.append("4x3 parity: dump dimensions differ")
            elif on_image.pixels != off_image.pixels:
                failures.append(
                    "4x3 parity: enabling the widescreen HUD changed a 4:3 "
                    "presentation; the anchor offsets and the slide scale "
                    "must both be inert at aspect <= 4:3")

        if args.keep_frames:
            print(f"  artifacts: {root}")

    if failures:
        print("FAIL: widescreen HUD layout check")
        for failure in failures:
            print(f"  - {failure}")
        return 1
    print(
        "PASS: widescreen HUD layout -- TT rows share one right anchor with "
        "an edge-anchored lap counter, the race-start hold stays offscreen, "
        "the identity label and battle strip are centered, and 4:3 is "
        "byte-identical with the option on"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
