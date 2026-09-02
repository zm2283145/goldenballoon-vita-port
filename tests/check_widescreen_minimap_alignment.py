#!/usr/bin/env python3
"""Minimap marker-on-map alignment under the opt-in widescreen HUD (issue #57).

The minimap is drawn in two passes that must agree: the map image (the hub
island / the track loop) is a billboard sprite pushed straight through
render_ortho_triangle_image_transform, and every marker (player arrow, racer
dots, Taj's dot) is a billboard sprite pushed through hud_element_render.
Both anchor on the same gMinimapScreenX.  Under the WIDE_HUD ortho
(mtx_ortho_wide_tagged) the slot-0 matrix compresses x by safe/presentation,
but billboard-mode local geometry never passes through slot 0 -- the quad is
added to the anchor in clip space through the slot-2 matrix alone
(platform/fast3d/gfx_pc_dkr.c) and then mapped across the full presentation
width.  Pre-fix, every ortho sprite therefore rendered presentation/safe
(4:3 at 16:9) wider than authored: anchors stayed correct while the sprite's
texels slid sideways around them.  For a small marker that is a cosmetic
stretch; for the big map sprite it displaced map features by up to ~1.5 px
per authored unit from the anchor, so the (correctly anchored) driver marker
sat ~100 px to the right of where it belonged ON the island -- the second
half of issue #57.  camera.c now applies the recorded ortho compression
(sNativeOrthoBillboardXScale) to the slot-2 clip-x output.

Fixtures (both sides of the shared draw path):

  * Hub (adventure_hub_drive.txt, Timber's Island): Taj's magenta map dot and
    the dark gate icon baked into the island sprite.  Asserted: the dot stays
    square (pre-fix 24x18 at 1080p), and the dot-to-gate horizontal offset
    matches the widescreen-HUD-off arm (pre-fix: off +12 px, on +76 px).

  * Race (race_full_3lap.txt): the checkered finish tile baked into the
    track-map sprite and the blue player arrow.  Asserted: the checker tile
    stays square (pre-fix 30x22 -- map texels stretch), the white track-loop
    width matches the off arm (pre-fix 174 vs 130 px), and the
    arrow-to-checker offset matches the off arm.

  * 4:3 parity rail: at a 4:3 presentation the ON arm must be BYTE-IDENTICAL
    to the OFF arm; the ortho compression factor is exactly 1.0 whenever the
    widescreen HUD is not driving the frame.

Every wide pair also requires the ON and OFF runs to agree on the exact
normalized [PACE] row at each capture frame (presentation-only change), and
each ON arm must prove it engaged (the minimap actually moved by the right
anchor margin).  Detector self-tests run first on synthetic rasters so each
assertion is proven able to fail before any verdict is trusted.

Usage:
    python3 tests/check_widescreen_minimap_alignment.py \
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

from check_adventure_hub import HUB_TOUR_ROUTE
from harness_utils import (DEFAULT_BUILD_DIR, read_ppm as read_ppm_bytes,
                           resolve_binary)

REPO = Path(__file__).resolve().parent.parent
HUB_SCRIPT = REPO / "tests" / "input_scripts" / "adventure_hub_drive.txt"
RACE_SCRIPT = REPO / "tests" / "input_scripts" / "race_full_3lap.txt"

FATAL_MARKERS = ("[CRASH]", "[FATAL]", "AddressSanitizer", "runtime error:")
PACE_RE = re.compile(r"\[PACE\] frame=(\d+) .*")

# Hub fixture: island live from ~5867 (the intro cutscene is not skippable);
# the MDKR_DRIVE_ROUTE tour keeps the kart on the island for the whole window.
HUB_FRAMES = 8200
HUB_DUMP_FROM = 6350
HUB_DUMP_EVERY = 150
HUB_CAPTURES = (6350, 6500, 6650)

# Race fixture: race live ~2643; mid-race window with the markers spread out.
RACE_FRAMES = 4000
RACE_DUMP_FROM = 3180
RACE_DUMP_EVERY = 120
RACE_CAPTURES = (3180, 3300, 3420)

WIDE_SIZE = "1920x1080"   # all geometry below is calibrated at this size and
                          # scaled by width/1920 so a resize fails loudly, not
                          # silently
PURE_SIZE = "720x540"
PARITY_CAPTURE = 3300

# Minimap search band (fractions of the frame): right side, lower half.
MINIMAP_BAND = (0.55, 0.50, 1.00, 0.98)

# Calibrated at 1920x1080 on the current fixtures (see the module docstring
# for the pre-fix signatures).  All *_PX values scale with frame width.
DOT_MIN_AREA = 100          # Taj dot: measured 259-342
DOT_MAX_BBOX_PX = 40
GATE_MIN_AREA = 40          # gate icon: measured 82-156
GATE_MAX_BBOX_PX = 30
GATE_RADIUS_PX = 150        # gate-to-dot distance: 27 off / 79 pre-fix on
ARROW_MIN_AREA = 200        # player arrow: measured 726-974
ARROW_MAX_BBOX_PX = 80
CHECKER_MIN_AREA = 100      # checker tile: measured 246-342
CHECKER_MAX_BBOX_PX = 45
LOOP_MIN_AREA = 4000        # white track loop, all pieces: measured ~11000
LOOP_PIECE_MIN_AREA = 1000  # a marker can sever the thin loop line
SQUARE_MAX_RATIO = 1.15     # pre-fix marker/tile stretch is exactly 4/3
REGISTRATION_TOL_PX = 12    # |on - off| marker-to-landmark: hub pre-fix ~64
LOOP_WIDTH_RATIO_TOL = 0.12  # pre-fix on/off loop width ratio 1.34
ENGAGED_MIN_SHIFT_PX = 120  # right anchor margin is 240 px at 1920x1080
MIN_EVALUABLE_FRAMES = 2


def is_magenta(r: int, g: int, b: int) -> bool:
    return r > 150 and b > 150 and g < 100


def is_dark(r: int, g: int, b: int) -> bool:
    return r < 110 and g < 110 and b < 110


def is_blue(r: int, g: int, b: int) -> bool:
    return b > 120 and b > r + 50 and b > g + 50


def is_white(r: int, g: int, b: int) -> bool:
    return r > 215 and g > 215 and b > 215


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
    cx: float
    cy: float

    @property
    def width(self) -> int:
        return self.x1 - self.x0 + 1

    @property
    def height(self) -> int:
        return self.y1 - self.y0 + 1


def read_ppm(path: Path) -> Image:
    return Image(*read_ppm_bytes(path))


def components(image: Image, pred, min_area: int) -> list[Component]:
    x0 = round(image.width * MINIMAP_BAND[0])
    y0 = round(image.height * MINIMAP_BAND[1])
    x1 = round(image.width * MINIMAP_BAND[2])
    y1 = round(image.height * MINIMAP_BAND[3])
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
            comps.append(Component(
                min(xs), min(ys), max(xs), max(ys), len(members),
                sum(xs) / len(members), sum(ys) / len(members)))
    return sorted(comps, key=lambda c: -c.area)


def scale_of(image: Image) -> float:
    return image.width / 1920.0


def find_largest(image: Image, pred, min_area: float,
                 max_bbox: float) -> Component | None:
    for comp in components(image, pred, max(1, round(min_area))):
        if comp.width <= max_bbox and comp.height <= max_bbox:
            return comp
    return None


def find_dot(image: Image) -> Component | None:
    s = scale_of(image)
    return find_largest(image, is_magenta, DOT_MIN_AREA * s * s,
                        DOT_MAX_BBOX_PX * s)


def find_gate(image: Image, dot: Component) -> Component | None:
    """The island sprite's gate icon: the small dark cluster nearest Taj's
    dot.  The dark forest around the island forms one huge component, so the
    bbox bound is what separates the icon from the scene, and the dot's own
    drop shadow (a dark cluster that MOVES WITH the marker) is excluded by
    rejecting anything that touches the dot's expanded bbox -- a landmark
    that rides the marker cannot witness marker-vs-map registration."""
    s = scale_of(image)
    margin = 6 * s
    best: Component | None = None
    best_distance = GATE_RADIUS_PX * s
    for comp in components(image, is_dark, max(1, round(GATE_MIN_AREA * s * s))):
        if comp.width > GATE_MAX_BBOX_PX * s or comp.height > GATE_MAX_BBOX_PX * s:
            continue
        if not (comp.x1 < dot.x0 - margin or comp.x0 > dot.x1 + margin or
                comp.y1 < dot.y0 - margin or comp.y0 > dot.y1 + margin):
            continue
        distance = ((comp.cx - dot.cx) ** 2 + (comp.cy - dot.cy) ** 2) ** 0.5
        if distance < best_distance:
            best = comp
            best_distance = distance
    return best


def find_arrow(image: Image) -> Component | None:
    s = scale_of(image)
    return find_largest(image, is_blue, ARROW_MIN_AREA * s * s,
                        ARROW_MAX_BBOX_PX * s)


def find_checker(image: Image) -> Component | None:
    """The checkered finish tile baked into the track-map sprite: the largest
    small dark cluster that sits on the white loop."""
    s = scale_of(image)
    for comp in components(image, is_dark,
                           max(1, round(CHECKER_MIN_AREA * s * s))):
        if comp.width > CHECKER_MAX_BBOX_PX * s or \
                comp.height > CHECKER_MAX_BBOX_PX * s:
            continue
        pad = round(6 * s)
        white = 0
        for y in range(max(0, comp.y0 - pad),
                       min(image.height, comp.y1 + pad + 1)):
            row = y * image.width * 3
            for x in range(max(0, comp.x0 - pad),
                           min(image.width, comp.x1 + pad + 1)):
                index = row + x * 3
                if is_white(image.pixels[index], image.pixels[index + 1],
                            image.pixels[index + 2]):
                    white += 1
        if white >= 40 * s * s:
            return comp
    return None


def find_loop(image: Image) -> tuple[int, int] | None:
    """Horizontal extent [x0, x1] of the white track-map loop.  Markers
    crossing the thin loop line can sever it into several components, so the
    extent is the union of every substantial white cluster; the summed area
    must still amount to a loop."""
    s = scale_of(image)
    comps = components(image, is_white, max(1, round(LOOP_PIECE_MIN_AREA * s * s)))
    if not comps or sum(c.area for c in comps) < LOOP_MIN_AREA * s * s:
        return None
    return min(c.x0 for c in comps), max(c.x1 for c in comps)


def square_failure(comp: Component, label: str, what: str) -> list[str]:
    ratio = comp.width / comp.height
    if ratio > SQUARE_MAX_RATIO:
        return [
            f"{label}: {what} is stretched {comp.width}x{comp.height} "
            f"(w/h {ratio:.2f} > {SQUARE_MAX_RATIO}); ortho sprite texels "
            "must keep the safe area's pixel scale under WIDE_HUD"]
    return []


# ---------------------------------------------------------------------------
# Pure per-frame analyses (Image pairs in, failure strings out) so the
# self-tests can drive them with synthetic rasters.

def analyze_hub_pair(on: Image, off: Image, label: str) -> list[str]:
    s = scale_of(on)
    on_dot = find_dot(on)
    off_dot = find_dot(off)
    if on_dot is None or off_dot is None:
        return [f"{label}: Taj map dot not found "
                f"(on={on_dot is not None} off={off_dot is not None})"]
    failures = square_failure(on_dot, label, "the Taj map dot (on)")
    failures += square_failure(off_dot, label, "the Taj map dot (off)")
    if on_dot.cx - off_dot.cx < ENGAGED_MIN_SHIFT_PX * s:
        failures.append(
            f"{label}: on-arm dot only {on_dot.cx - off_dot.cx:.1f} px right "
            f"of the off arm (>= {ENGAGED_MIN_SHIFT_PX * s:.0f} expected); "
            "the widescreen HUD did not engage (fixture drift)")
        return failures
    on_gate = find_gate(on, on_dot)
    off_gate = find_gate(off, off_dot)
    if on_gate is None or off_gate is None:
        failures.append(
            f"{label}: island gate icon not found near the dot "
            f"(on={on_gate is not None} off={off_gate is not None})")
        return failures
    on_offset = on_dot.cx - on_gate.cx
    off_offset = off_dot.cx - off_gate.cx
    if abs(on_offset - off_offset) > REGISTRATION_TOL_PX * s:
        failures.append(
            f"{label}: marker mis-registered on the island: dot sits "
            f"{on_offset:+.1f} px from the gate icon with the widescreen HUD "
            f"on vs {off_offset:+.1f} px off (tolerance "
            f"{REGISTRATION_TOL_PX * s:.0f} px); the map image and the "
            "markers must shift as one")
    return failures


def analyze_race_pair(on: Image, off: Image, label: str) -> list[str]:
    s = scale_of(on)
    on_arrow = find_arrow(on)
    off_arrow = find_arrow(off)
    if on_arrow is None or off_arrow is None:
        return [f"{label}: player map arrow not found "
                f"(on={on_arrow is not None} off={off_arrow is not None})"]
    failures: list[str] = []
    if on_arrow.cx - off_arrow.cx < ENGAGED_MIN_SHIFT_PX * s:
        failures.append(
            f"{label}: on-arm arrow only {on_arrow.cx - off_arrow.cx:.1f} px "
            f"right of the off arm (>= {ENGAGED_MIN_SHIFT_PX * s:.0f} "
            "expected); the widescreen HUD did not engage (fixture drift)")
        return failures
    on_checker = find_checker(on)
    off_checker = find_checker(off)
    if on_checker is None or off_checker is None:
        failures.append(
            f"{label}: checkered finish tile not found on the track map "
            f"(on={on_checker is not None} off={off_checker is not None})")
        return failures
    failures += square_failure(on_checker, label,
                               "the checkered finish tile (on)")
    failures += square_failure(off_checker, label,
                               "the checkered finish tile (off)")
    on_offset = on_arrow.cx - on_checker.cx
    off_offset = off_arrow.cx - off_checker.cx
    if abs(on_offset - off_offset) > REGISTRATION_TOL_PX * s:
        failures.append(
            f"{label}: arrow mis-registered on the track map: "
            f"{on_offset:+.1f} px from the finish tile on vs "
            f"{off_offset:+.1f} px off (tolerance "
            f"{REGISTRATION_TOL_PX * s:.0f} px)")
    on_loop = find_loop(on)
    off_loop = find_loop(off)
    if on_loop is None or off_loop is None:
        failures.append(
            f"{label}: white track loop not found "
            f"(on={on_loop is not None} off={off_loop is not None})")
        return failures
    on_width = on_loop[1] - on_loop[0] + 1
    off_width = off_loop[1] - off_loop[0] + 1
    ratio = on_width / off_width
    if abs(ratio - 1.0) > LOOP_WIDTH_RATIO_TOL:
        failures.append(
            f"{label}: track-map loop width {on_width} px on vs "
            f"{off_width} px off (ratio {ratio:.2f}, tolerance "
            f"{LOOP_WIDTH_RATIO_TOL}); the map sprite must not stretch under "
            "WIDE_HUD")
    return failures


# ---------------------------------------------------------------------------
# Detector self-tests: prove each assertion can fail on synthetic rasters
# before any engine verdict is trusted.

def synth(width: int, height: int,
          rects: list[tuple[int, int, int, int, tuple[int, int, int]]]) -> Image:
    # Neutral mid-gray backdrop: a real frame's scenery is neither dark,
    # white, magenta nor blue, and an all-black canvas would fuse every
    # synthetic dark landmark into one band-wide component.
    pixels = bytearray(bytes((150, 150, 150)) * (width * height))
    for x0, y0, x1, y1, (r, g, b) in rects:
        for y in range(y0, y1):
            row = y * width * 3
            for x in range(x0, x1):
                index = row + x * 3
                pixels[index:index + 3] = bytes((r, g, b))
    return Image(width, height, bytes(pixels))


def self_test() -> list[str]:
    problems: list[str] = []
    width, height = 1920, 1080
    magenta = (255, 40, 220)
    dark = (60, 60, 60)
    blue = (40, 60, 220)
    white = (240, 240, 240)

    def hub_frame(dot_x: int, gate_x: int, dot_w: int = 18) -> Image:
        # The dark rect riding the dot is the marker's drop shadow: a decoy
        # landmark that moves WITH the marker.  find_gate must skip it, or
        # the registration assertion is blinded (it was, on real frames).
        return synth(width, height, [
            (gate_x, 872, gate_x + 13, 886, dark),
            (dot_x, 900, dot_x + dot_w, 918, magenta),
            (dot_x + 6, 912, dot_x + dot_w + 6, 926, dark),
        ])

    hub_off = hub_frame(1383, 1371)
    # Pre-fix signature: stretched dot, gate short of the full anchor shift.
    hub_red = hub_frame(1623, 1547, dot_w=24)
    hub_green = hub_frame(1623, 1611)
    red = analyze_hub_pair(hub_red, hub_off, "self")
    if not any("stretched" in f for f in red):
        problems.append("hub detector passed a stretched dot")
    if not any("mis-registered" in f for f in red):
        problems.append("hub detector passed a mis-registered dot")
    if analyze_hub_pair(hub_green, hub_off, "self"):
        problems.append("hub detector rejected an aligned pair")
    if not analyze_hub_pair(synth(width, height, []), hub_off, "self"):
        problems.append("hub detector passed a frame with no dot")
    if not any("did not engage" in f
               for f in analyze_hub_pair(hub_off, hub_off, "self")):
        problems.append("hub detector passed an unengaged on arm")

    def race_frame(arrow_x: int, checker_x: int, loop_w: int,
                   checker_w: int = 22) -> Image:
        loop_x = checker_x - loop_w + 30
        return synth(width, height, [
            (loop_x, 690, loop_x + loop_w, 990, white),
            (checker_x, 864, checker_x + checker_w, 886, dark),
            (arrow_x, 810, arrow_x + 40, 850, blue),
        ])

    race_off = race_frame(1509, 1545, 130)
    race_red = race_frame(1749, 1780, 174, checker_w=30)
    race_green = race_frame(1749, 1785, 130)
    red = analyze_race_pair(race_red, race_off, "self")
    if not any("stretched" in f for f in red):
        problems.append("race detector passed a stretched checker tile")
    if not any("loop width" in f for f in red):
        problems.append("race detector passed a stretched track loop")
    if analyze_race_pair(race_green, race_off, "self"):
        problems.append("race detector rejected an aligned pair")
    shifted = race_frame(1749, 1745, 130)
    if not any("mis-registered" in f
               for f in analyze_race_pair(shifted, race_off, "self")):
        problems.append("race detector passed a mis-registered arrow")

    a = synth(64, 64, [(0, 0, 64, 64, white)])
    b = synth(64, 64, [(0, 0, 64, 64, white), (5, 5, 6, 6, dark)])
    if a.pixels == b.pixels:
        problems.append("parity comparator cannot see a one-pixel change")
    return problems


# ---------------------------------------------------------------------------
# Engine runs.

@dataclass
class Run:
    label: str
    output: str = ""
    frames: dict[int, Path] = field(default_factory=dict)


def run_arm(binary: Path, rom: Path, root: Path, label: str, script: Path,
            frames: int, size: str, widescreen_hud: bool, dump_from: int,
            dump_every: int, renderer: str | None, timeout: int,
            verbose: bool, extra: dict[str, str] | None = None
            ) -> tuple[Run, list[str]]:
    failures: list[str] = []
    arm_root = root / label
    frame_dir = arm_root / "frames"
    run_dir = arm_root / "run"
    frame_dir.mkdir(parents=True)
    (run_dir / "save").mkdir(parents=True)
    env = {key: value for key, value in os.environ.items()
           if not key.startswith("MDKR")}
    env.update(
        MDKR_AUDIO="0",
        MDKR_AUTOPILOT="1",
        MDKR_TRACE="1",
        MDKR_NO_CRASH_HANDLER="1",
        MDKR64_HIDDEN="1",
        MDKR_RENDER_SCALE="1",   # pins the dump size the geometry is
                                 # calibrated against (no HiDPI 2x dumps)
        MDKR_DUMP_FROM=str(dump_from),
        MDKR_DUMP_EVERY=str(dump_every),
        MDKR_SAVE_DIR=str(run_dir / "save"),
        MDKR_VIDEO_CONFIG_PATH=str(arm_root / "video.ini"),
        LC_ALL="C",
    )
    if widescreen_hud:
        env["MDKR_WIDESCREEN_HUD"] = "1"
    if renderer:
        env["MDKR_RENDERER"] = renderer
    env.update(extra or {})
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
            command, cwd=run_dir, env=env, text=True,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            timeout=timeout, check=False)
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
            return re.sub(r" dtms=\S+", " dtms=<wall>", candidate.strip())
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


def evaluate_pair(on: Run, off: Run, captures: tuple[int, ...],
                  analyze, verbose: bool) -> list[str]:
    failures: list[str] = []
    evaluated = 0
    for frame in captures:
        on_image, problems = load_frame(on, frame)
        failures.extend(problems)
        off_image, problems = load_frame(off, frame)
        failures.extend(problems)
        if on_image is None or off_image is None:
            continue
        # The geometry is calibrated at 1920 px and every threshold scales
        # with width (scale_of), so a HiDPI 2x dump is fine -- but the two
        # arms must agree, or no pixel comparison is meaningful.
        if (on_image.width, on_image.height) \
                != (off_image.width, off_image.height):
            failures.append(
                f"{on.label}: frame {frame} dumped at {on_image.width}x"
                f"{on_image.height} vs {off_image.width}x{off_image.height} "
                "off (dump-size drift; the arms cannot be compared)")
            continue
        failures.extend(require_pace_identity(on, off, frame))
        frame_failures = analyze(on_image, off_image,
                                 f"{on.label} frame {frame}")
        failures.extend(frame_failures)
        evaluated += 1
        if verbose:
            verdict = "ok" if not frame_failures else "FAIL"
            print(f"  {on.label} frame {frame}: {verdict}")
    if evaluated < MIN_EVALUABLE_FRAMES:
        failures.append(
            f"{on.label}: only {evaluated} capture frame(s) evaluable "
            f"(need >= {MIN_EVALUABLE_FRAMES}); fixture drift")
    return failures


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", default=DEFAULT_BUILD_DIR)
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    parser.add_argument("--renderer", choices=("gl", "webgpu"), default=None)
    parser.add_argument("--timeout", type=int, default=600,
                        help="seconds per arm")
    parser.add_argument("--keep-frames", type=Path)
    parser.add_argument("--self-test", action="store_true",
                        help="only run the detector self-tests")
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    detector_problems = self_test()
    if detector_problems:
        print("FAIL: widescreen minimap alignment detectors")
        for problem in detector_problems:
            print(f"  - {problem}")
        return 1
    if args.self_test:
        print("check_widescreen_minimap_alignment: detector self-test OK")
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
            prefix="mdkr_widescreen_minimap_")

    failures: list[str] = []
    with context as root_text:
        root = Path(root_text)
        hub_env = {"MDKR_DRIVE_ROUTE": HUB_TOUR_ROUTE}

        def arm(label: str, script: Path, frames: int, size: str, wide: bool,
                dump_from: int, dump_every: int,
                extra: dict[str, str] | None = None) -> Run:
            run, problems = run_arm(
                binary, rom, root, label, script, frames, size, wide,
                dump_from, dump_every, args.renderer, args.timeout,
                args.verbose, extra)
            failures.extend(problems)
            return run

        # --- Hub island ---------------------------------------------------
        hub_on = arm("hub-16x9-on", HUB_SCRIPT, HUB_FRAMES, WIDE_SIZE, True,
                     HUB_DUMP_FROM, HUB_DUMP_EVERY, hub_env)
        hub_off = arm("hub-16x9-off", HUB_SCRIPT, HUB_FRAMES, WIDE_SIZE,
                      False, HUB_DUMP_FROM, HUB_DUMP_EVERY, hub_env)
        failures.extend(evaluate_pair(hub_on, hub_off, HUB_CAPTURES,
                                      analyze_hub_pair, args.verbose))

        # --- Race track map (the draw path is shared with the hub) --------
        race_on = arm("race-16x9-on", RACE_SCRIPT, RACE_FRAMES, WIDE_SIZE,
                      True, RACE_DUMP_FROM, RACE_DUMP_EVERY)
        race_off = arm("race-16x9-off", RACE_SCRIPT, RACE_FRAMES, WIDE_SIZE,
                       False, RACE_DUMP_FROM, RACE_DUMP_EVERY)
        failures.extend(evaluate_pair(race_on, race_off, RACE_CAPTURES,
                                      analyze_race_pair, args.verbose))

        # --- 4:3 parity rail ----------------------------------------------
        pure_on = arm("race-4x3-on", RACE_SCRIPT, RACE_FRAMES, PURE_SIZE,
                      True, PARITY_CAPTURE, 99999)
        pure_off = arm("race-4x3-off", RACE_SCRIPT, RACE_FRAMES, PURE_SIZE,
                       False, PARITY_CAPTURE, 99999)
        on_image, problems = load_frame(pure_on, PARITY_CAPTURE)
        failures.extend(problems)
        off_image, problems = load_frame(pure_off, PARITY_CAPTURE)
        failures.extend(problems)
        if on_image is not None and off_image is not None:
            if (on_image.width, on_image.height) \
                    != (off_image.width, off_image.height):
                failures.append("4x3 parity: dump dimensions differ")
            elif on_image.pixels != off_image.pixels:
                failures.append(
                    "4x3 parity: enabling the widescreen HUD changed a 4:3 "
                    "presentation; the ortho sprite compression must be "
                    "exactly 1.0 whenever WIDE_HUD is not driving the frame")

        if args.keep_frames:
            print(f"  artifacts: {root}")

    if failures:
        print("FAIL: widescreen minimap alignment check")
        for failure in failures:
            print(f"  - {failure}")
        return 1
    print(
        "PASS: widescreen minimap alignment -- markers and the map image "
        "shift as one under the widescreen HUD (hub island and race track "
        "map), sprite texels keep the safe pixel scale, and 4:3 is "
        "byte-identical with the option on"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
