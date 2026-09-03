#!/usr/bin/env python3
"""AP-08 focused route: the Adventure Party HUB roster, viewports, input, HUD.

What this proves
----------------
With ``Enhancements.AdventureParty`` ON and 2/3/4 controllers joined, the party
takes the ordinary Adventure route (proven by check_adventure_party_admission)
and lands in the central hub, where AP-08 now spawns the WHOLE party atomically:

* N human racers with the joined seat->character identity (trace + PACE probes);
* N viewports in the existing VIEWPORT_LAYOUT_<N>_PLAYERS composition (the 3P arm
  keeps the fourth-quadrant minimap, exactly as Tracks 3P), proven per quadrant
  with the flat-field positive control from check_race_multiplayer;
* stable per-seat input: seat i drives controller port i, with NO input swap —
  each racer moves only when its own pad accelerates (the binding witness);
* a per-viewport hub HUD (hud_init reports N viewports);
* no fail-closed spawn abort, and a stable roster generation.

With the enhancement OFF, three/four controllers route to Tracks exactly as
stock and NO party hub is expanded — no ``aparty_`` line ever appears.

Positive controls (mutating the measurement, never the sources)
---------------------------------------------------------------
* Flat-field viewport: every viewport region of one real hub frame is flattened
  in turn and the same scorer must REJECT it (a blank viewport must not pass).
* Swapped binding: the per-seat motion analysis must FAIL when each round is
  (wrongly) attributed to a rotated seat — otherwise the gate could not tell a
  correct seat->port->racer binding from a scrambled one.

Save fixture
------------
The started Adventure One slot-0 save from check_adventure_party_admission (the
host's FILE_SELECT confirm resumes it). No developer save is read or written.
"""

from __future__ import annotations

import argparse
import math
import os
import re
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path

from harness_utils import DEFAULT_BUILD_DIR, read_ppm, resolve_binary, save_env
from check_adventure_party_admission import eeprom_image
from check_race_multiplayer import region_metrics, blank_quadrant


def hub_region_live(metrics, minimap: bool) -> bool:
    """Is this region a POPULATED hub viewport (rendering the world) rather than a
    blank one? A hub lobby is not a race track: a racer free-driving may face a
    wall or flat ground for a frame, so a viewport legitimately carries far less
    colour/variance than check_race_multiplayer's track scenes — this check must
    NOT demand track richness. What it must still reject is an unrendered/blank
    viewport, which is exactly the flat-field positive control: a single fill
    colour (colours == 1, sigma == 0). The 24-colour / 3.0-sigma floor sits well
    above that fill and well below every real viewport measured (colours >= 42,
    sigma >= 4.1), so the control fails while real viewports pass. The 3P
    fourth-quadrant minimap keeps check_race_multiplayer's own criteria."""
    if minimap:
        return (metrics.colours >= 12 and metrics.sigma >= 8.0
                and metrics.nonblack >= 0.015)
    return metrics.colours >= 24 and metrics.sigma >= 3.0 and metrics.nonblack >= 0.30

ROOT = Path(__file__).resolve().parent.parent

HUB_LEVEL_ID = 0
MENU_GAME_SELECT = 19
MENU_TRACK_SELECT = 15
MENU_CHARACTER_SELECT = 3

# Visual sampling window: the hub has faded in and the per-seat driving rounds
# are under way. At 640x480 every sampled frame in this window renders all N
# viewports (the idle "flipbook" flicker only appears at reduced resolutions).
DUMP_FROM = 2760
DUMP_EVERY = 100
MIN_VISUAL_SAMPLES = 5

# Per-seat driving rounds authored into the *_hub.txt fixtures: round r holds A
# on pad (r+1) for 90 frames starting here, all other pads idle.
ROUND_START = (2700, 2880, 3060, 3240)
ROUND_LEN = 90
# Displacement (world units) the actively-driven racer must cover in its own
# round, and the margin by which it must beat every idle racer (which only
# coasts). Measured: driven ~800-1160, coasting <90 — a >=3x margin is ample.
MOVE_MIN = 150.0
DOMINANCE = 3.0

FRAMES = {2: 3300, 3: 3450, 4: 3650}

MENU_RE = re.compile(r"menu_init: menuId=(\d+) @frame~(\d+)")
LEVEL_RE = re.compile(r"level_load: levelId=(-?\d+) numPlayers=(-?\d+).*@frame~(\d+)")
SESSION_RE = re.compile(r"aparty_session: state=(\w+) sgen=(\d+) lgen=(\d+) host=(\d+)")
ROSTER_RE = re.compile(r"aparty_roster: n=(\d+) mask=0x([0-9a-fA-F]+)((?: c\d+=\d+)*)")
LAYOUT_RE = re.compile(r"aparty_layout: viewports=(\d+) layout=(\d+)")
BIND_RE = re.compile(r"aparty_binding: seat=(\d+) port=(\d+)")
ABORT_RE = re.compile(r"aparty_spawn_abort:")
HUD_RE = re.compile(r"hud_init: hudPlayers=(\d+) numViewports=(\d+) @frame~(\d+)")
PACE1_RE = re.compile(r"\[PACE\] frame=(\d+).*racer x=(\S+) y=(\S+) z=(\S+)")
PACEN_RE = re.compile(r"\[PACE([2-4])\] frame=(\d+) \| racer[2-4] x=(\S+) y=(\S+) z=(\S+)")
RACERINPUT_RE = re.compile(
    r"\[RACERINPUT\] tick=(\d+) player=(\d+) racer=(\d+) port=(\d+) held=([0-9a-fA-F]+)"
)
BAD_RE = re.compile(
    r"\[CRASH\]|\[FATAL\]|AddressSanitizer|UndefinedBehaviorSanitizer|"
    r"runtime error:|Assertion failed"
)

EXPECTED_CHARACTERS = {
    2: {0: 9, 1: 0},
    3: {0: 9, 1: 0, 2: 1},
    4: {0: 9, 1: 0, 2: 1, 3: 5},
}


def viewport_regions(players: int):
    """(name, bounds, is_minimap) for each populated region of the N-player
    layout. 2P is a top/bottom split; 3P is three quadrants plus the
    fourth-quadrant minimap; 4P is four quadrants."""
    if players == 2:
        return [
            ("top-half", (0.03, 0.97, 0.05, 0.45), False),
            ("bottom-half", (0.03, 0.97, 0.55, 0.95), False),
        ]
    quads = [
        ("top-left", (0.03, 0.47, 0.03, 0.47)),
        ("top-right", (0.53, 0.97, 0.03, 0.47)),
        ("bottom-left", (0.03, 0.47, 0.53, 0.97)),
        ("bottom-right", (0.53, 0.97, 0.53, 0.97)),
    ]
    if players == 3:
        return [
            (quads[0][0], quads[0][1], False),
            (quads[1][0], quads[1][1], False),
            (quads[2][0], quads[2][1], False),
            (quads[3][0], quads[3][1], True),  # fourth-quadrant minimap
        ]
    return [(name, bounds, False) for name, bounds in quads]


@dataclass(frozen=True)
class RosterLine:
    n: int
    mask: int
    characters: dict


def parse_rosters(output: str):
    out = []
    for m in ROSTER_RE.finditer(output):
        chars = {int(k[1:]): int(v)
                 for k, v in (tok.split("=") for tok in m.group(3).split())}
        out.append(RosterLine(int(m.group(1)), int(m.group(2), 16), chars))
    return out


def parse_positions(output: str):
    """playerIndex -> {frame: (x, y, z)} from the [PACE]/[PACEn] probes."""
    pos = {p: {} for p in range(4)}
    for line in output.splitlines():
        m = PACE1_RE.search(line)
        if m:
            pos[0][int(m.group(1))] = (
                float(m.group(2)), float(m.group(3)), float(m.group(4)))
            continue
        m = PACEN_RE.search(line)
        if m:
            pos[int(m.group(1)) - 1][int(m.group(2))] = (
                float(m.group(3)), float(m.group(4)), float(m.group(5)))
    return pos


def displacement(track: dict, lo: int, hi: int) -> float:
    frames = sorted(f for f in track if lo <= f <= hi)
    if len(frames) < 2:
        return 0.0
    return math.dist(track[frames[0]], track[frames[-1]])


def run_arm(binary, rom, script, players, enabled, frame_dir, verbose):
    with tempfile.TemporaryDirectory(prefix="mdkr_ap_hub_") as tmp:
        root = Path(tmp)
        save_dir = root / "save"
        save_dir.mkdir()
        (save_dir / "eeprom.bin").write_bytes(eeprom_image())
        env = {k: v for k, v in os.environ.items()
               if not k.startswith(("MDKR", "GE007_"))}
        env.update(LC_ALL="C", MDKR_AUDIO="0", MDKR_TRACE="1",
                   MDKR_RACER_INPUT_TRACE="1")
        save_env(env, str(save_dir))
        frames = FRAMES[players]
        command = [
            binary, "--headless-frames", str(frames),
            "--input-script", str(ROOT / script), "--rom", rom,
            "--window-size", "640x480",
            "--video-set", f"Enhancements.AdventureParty={1 if enabled else 0}",
        ]
        if frame_dir is not None:
            command += ["--dump-frames", frame_dir]
            env["MDKR_DUMP_FROM"] = str(DUMP_FROM)
            env["MDKR_DUMP_EVERY"] = str(DUMP_EVERY)
        # Hermetic presentation clock so frame-indexed input is deterministic.
        env["MDKR_VIDEO_CONFIG_PATH"] = str(root / "mdkr64.ini")
        if verbose:
            print(f"$ ({'on' if enabled else 'off'} {players}P) {' '.join(command)}",
                  flush=True)
        proc = subprocess.run(command, cwd=root, env=env, text=True,
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                              timeout=max(240, frames // 12), check=False)
    return proc.stdout or ""


def check_binding_motion(pos, players, expected_seat_for_round):
    """Every round r drives racer expected_seat_for_round[r]; that racer must
    move MOVE_MIN and beat every other racer's displacement by DOMINANCE. Returns
    failure strings (empty == the mapping holds)."""
    failures = []
    for r in range(players):
        driven = expected_seat_for_round[r]
        lo = ROUND_START[r]
        hi = lo + ROUND_LEN
        disp = {p: displacement(pos[p], lo, hi) for p in range(players)}
        others = max((disp[p] for p in range(players) if p != driven), default=0.0)
        if disp[driven] < MOVE_MIN:
            failures.append(
                f"round {r}: expected racer {driven} to move (>= {MOVE_MIN}), "
                f"moved {disp[driven]:.1f}; disp={disp}")
        elif disp[driven] < DOMINANCE * max(others, 1.0):
            failures.append(
                f"round {r}: racer {driven} moved {disp[driven]:.1f} but did not "
                f"dominate idle racers ({others:.1f}); disp={disp}")
    return failures


def check_on_arm(output, players, frame_dir, verbose):
    failures = []
    label = f"{players}P on"

    if bad := BAD_RE.search(output):
        failures.append(f"{label}: fatal marker {bad.group(0)!r}")

    # Reached GAME_SELECT (party admitted) and the hub loaded.
    menus = [(int(m.group(1)), int(m.group(2))) for m in MENU_RE.finditer(output)]
    char = next((f for mid, f in menus if mid == MENU_CHARACTER_SELECT), -1)
    if not any(mid == MENU_GAME_SELECT and f > char for mid, f in menus):
        failures.append(f"{label}: party never reached GAME_SELECT; menus={menus}")
    loads = [(int(m.group(1)), int(m.group(3))) for m in LEVEL_RE.finditer(output)]
    if not any(level == HUB_LEVEL_ID for level, _ in loads):
        failures.append(f"{label}: no Adventure hub (levelId {HUB_LEVEL_ID}) load; "
                        f"loads={loads}")

    # No fail-closed spawn abort.
    if ABORT_RE.search(output):
        line = next(l for l in output.splitlines() if "aparty_spawn_abort" in l)
        failures.append(f"{label}: fail-closed spawn abort: {line.strip()}")

    # Session lifecycle + stable roster generation.
    sessions = [(m.group(1), int(m.group(2)), int(m.group(3)), int(m.group(4)))
                for m in SESSION_RE.finditer(output)]
    lobby = [s for s in sessions if s[0] == "ACTIVE_LOBBY"]
    if not lobby:
        failures.append(f"{label}: no ACTIVE_LOBBY session; sessions={sessions}")
    elif (lobby[0][1], lobby[0][2], lobby[0][3]) != (1, 1, 0):
        failures.append(f"{label}: ACTIVE_LOBBY sgen/lgen/host={lobby[0][1:]}, "
                        f"expected (1, 1, 0)")

    # Roster: the FORM roster and the AP-08 spawn roster must both be present and
    # identical (a stable roster, dense from host seat 0, with the joined chars).
    rosters = parse_rosters(output)
    expected_mask = (1 << players) - 1
    if len(rosters) < 2:
        failures.append(f"{label}: expected FORM + spawn roster lines, saw "
                        f"{len(rosters)}")
    spawn = rosters[-1] if rosters else None
    if spawn is not None:
        if spawn.n != players or spawn.mask != expected_mask:
            failures.append(f"{label}: spawn roster n={spawn.n} mask=0x{spawn.mask:x}, "
                            f"expected n={players} mask=0x{expected_mask:x}")
        if spawn.characters != EXPECTED_CHARACTERS[players]:
            failures.append(f"{label}: spawn roster characters {spawn.characters}, "
                            f"expected {EXPECTED_CHARACTERS[players]}")
        if any(r.n != spawn.n or r.mask != spawn.mask or r.characters != spawn.characters
               for r in rosters):
            failures.append(f"{label}: roster changed between FORM and spawn "
                            f"(unstable): {[ (r.n, r.mask, r.characters) for r in rosters ]}")

    # Viewport layout trace: N viewports, VIEWPORT_LAYOUT_<N>_PLAYERS (== N-1).
    layouts = [(int(m.group(1)), int(m.group(2))) for m in LAYOUT_RE.finditer(output)]
    if (players, players - 1) not in layouts:
        failures.append(f"{label}: no aparty_layout viewports={players} "
                        f"layout={players - 1}; saw {layouts}")

    # Per-seat binding trace: exactly N identity bindings, seat i -> port i.
    binds = sorted((int(m.group(1)), int(m.group(2))) for m in BIND_RE.finditer(output))
    if binds != [(i, i) for i in range(players)]:
        failures.append(f"{label}: bindings {binds}, expected "
                        f"{[(i, i) for i in range(players)]}")

    # HUD per viewport: hud_init reports the N-viewport layout.
    huds = [(int(m.group(1)), int(m.group(2))) for m in HUD_RE.finditer(output)]
    if (players - 1, players) not in huds:
        failures.append(f"{label}: no hud_init hudPlayers={players - 1} "
                        f"numViewports={players}; saw {huds}")

    # N human racers actually simulate in the hub (PACE probe per seat, and none
    # beyond N).
    pos = parse_positions(output)
    hub_lo = ROUND_START[0]
    for p in range(players):
        if not any(f >= hub_lo for f in pos[p]):
            failures.append(f"{label}: racer {p} published no hub PACE rows "
                            f"(seat never acquired a racer)")
    for p in range(players, 4):
        if any(f >= hub_lo for f in pos[p]):
            failures.append(f"{label}: unexpected racer {p} in an {players}-seat party")

    # Binding witness: seat i reads ONLY port i (identity + stable, no swap), and
    # each racer moves only under its own pad.
    ri = [(int(m.group(1)), int(m.group(2)), int(m.group(4)), int(m.group(5), 16))
          for m in RACERINPUT_RE.finditer(output)]
    hub_ri = [(tick, pl, port, held) for tick, pl, port, held in ri if tick >= hub_lo]
    if not hub_ri:
        failures.append(f"{label}: no [RACERINPUT] in the hub")
    if any(port != pl for _, pl, port, _ in hub_ri):
        mism = next((tick, pl, port) for tick, pl, port, _ in hub_ri if port != pl)
        failures.append(f"{label}: seat/port swap in the hub: tick={mism[0]} "
                        f"player={mism[1]} port={mism[2]} (must be identity)")
    # In each round only the driven pad supplies input.
    for r in range(players):
        lo, hi = ROUND_START[r], ROUND_START[r] + ROUND_LEN
        pressed = {pl for tick, pl, _, held in hub_ri if lo <= tick <= hi and held != 0}
        if pressed != {r}:
            failures.append(f"{label}: round {r} had input from seats {sorted(pressed)}, "
                            f"expected only seat {r}")

    identity = list(range(players))
    failures.extend(f"{label}: {msg}"
                    for msg in check_binding_motion(pos, players, identity))

    # Positive control: a rotated seat->round attribution must FAIL the motion
    # analysis (otherwise it does not actually discriminate the binding).
    rotated = [(i + 1) % players for i in range(players)]
    if not check_binding_motion(pos, players, rotated):
        failures.append(f"{label}: positive control (swapped binding) — the motion "
                        f"analysis passed with rounds attributed to the WRONG seat")

    # Visual: N populated viewports per sampled frame, plus the flat-field control.
    failures.extend(check_visual(players, frame_dir, label, verbose))
    return failures


def check_visual(players, frame_dir, label, verbose):
    failures = []
    regions = viewport_regions(players)
    dumped = []
    for name in os.listdir(frame_dir):
        m = re.fullmatch(r"frame_(\d+)\.ppm", name)
        if m:
            dumped.append((int(m.group(1)), os.path.join(frame_dir, name)))
    dumped.sort()
    if len(dumped) < MIN_VISUAL_SAMPLES:
        failures.append(f"{label}: only {len(dumped)} hub visual samples "
                        f"(want >= {MIN_VISUAL_SAMPLES})")
    for frame, path in dumped:
        try:
            width, height, pixels = read_ppm(path)
        except (OSError, ValueError) as err:
            failures.append(f"{label} frame {frame}: {err}")
            continue
        for name, bounds, minimap in regions:
            metrics = region_metrics(width, height, pixels, bounds)
            if verbose:
                kind = "minimap" if minimap else "viewport"
                print(f"  {label} frame {frame} {name} ({kind}): "
                      f"colours={metrics.colours} sigma={metrics.sigma:.1f} "
                      f"nonblack={metrics.nonblack:.1%}")
            if not hub_region_live(metrics, minimap):
                failures.append(
                    f"{label} frame {frame} {name} is not live: "
                    f"colours={metrics.colours}, sigma={metrics.sigma:.1f}, "
                    f"nonblack={metrics.nonblack:.1%}")

    # Positive control: flatten each region of one real frame; the same scorer
    # must reject every corruption.
    if dumped:
        frame, path = dumped[len(dumped) // 2]
        width, height, pixels = read_ppm(path)
        for name, bounds, minimap in regions:
            corrupted = blank_quadrant(width, height, pixels, bounds, minimap)
            metrics = region_metrics(width, height, corrupted, bounds)
            if hub_region_live(metrics, minimap):
                failures.append(
                    f"{label} positive control: flattened {name} was accepted "
                    f"(colours={metrics.colours}, sigma={metrics.sigma:.1f})")
    return failures


def check_off_arm(output, players):
    """Enhancement off: 3/4 controllers route to Tracks; no party hub expands."""
    failures = []
    label = f"{players}P off"
    if bad := BAD_RE.search(output):
        failures.append(f"{label}: fatal marker {bad.group(0)!r}")
    menus = [(int(m.group(1)), int(m.group(2))) for m in MENU_RE.finditer(output)]
    ids = [mid for mid, _ in menus]
    if MENU_TRACK_SELECT not in ids:
        failures.append(f"{label}: did not route to TRACK_SELECT as stock; menus={menus}")
    if MENU_GAME_SELECT in ids:
        failures.append(f"{label}: reached GAME_SELECT with the enhancement off")
    if "aparty_" in output:
        line = next(l for l in output.splitlines() if "aparty_" in l)
        failures.append(f"{label}: aparty_ trace with enhancement off: {line.strip()}")
    return failures


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", default=DEFAULT_BUILD_DIR)
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    parser.add_argument("--players", type=int, choices=(2, 3, 4))
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    binary = str(Path(resolve_binary(args.build)).resolve())
    rom = str(Path(args.rom).resolve())
    scripts = {n: f"tests/input_scripts/adventure_party_{n}p_hub.txt"
               for n in (2, 3, 4)}
    selected = [args.players] if args.players else [2, 3, 4]
    required = [binary, rom, *(str(ROOT / scripts[n]) for n in selected)]
    missing = [p for p in required if not os.path.exists(p)]
    if missing:
        for p in missing:
            print(f"check_adventure_party_hub: FAIL -- missing {p}", file=sys.stderr)
        return 1

    failures = []
    for players in selected:
        with tempfile.TemporaryDirectory(prefix=f"mdkr_ap_hub_{players}p_") as fdir:
            out = run_arm(binary, rom, scripts[players], players, True, fdir,
                          args.verbose)
            failures.extend(check_on_arm(out, players, fdir, args.verbose))

    # OFF-arm control: three/four controllers get no party hub.
    for players in [n for n in selected if n in (3, 4)] or [3]:
        out = run_arm(binary, rom, scripts[players], players, False, None,
                      args.verbose)
        failures.extend(check_off_arm(out, players))

    if failures:
        print("check_adventure_party_hub: FAIL", file=sys.stderr)
        for f in failures:
            print(f"  - {f}", file=sys.stderr)
        return 1
    print("check_adventure_party_hub: PASS -- 2/3/4-player parties spawn the full "
          "hub roster (N racers, N viewports incl. 3P minimap, per-seat binding, "
          "per-viewport HUD, stable roster); off routes to Tracks; visual and "
          "swapped-binding positive controls fired")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
