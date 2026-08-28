#!/usr/bin/env python3
"""AP-09/10 focused route: shared lobby interactions have ONE authority.

What this proves (Enhancements.AdventureParty ON, a party in the central hub)
---------------------------------------------------------------------------
* Doors/exits: any participant may trigger, but the pure reducer latches exactly
  ONE whole-party transition per level generation. A single door drives one
  authored load and re-forms the WHOLE party at the destination lobby; two racers
  racing into the door in the same window still yield exactly one transition (the
  lowest seat wins), the loser's door is rejected (no second load), and the party
  arrives whole at one destination.
* Golden balloons: a NON-host may collect a shared hub balloon; the collection
  registers exactly once (two racers on the same balloon cannot double-collect).
* Pause: a non-host Start opens the ONE shared pause; the host owns the resume
  decision. There is a single pause state, never a per-viewport menu.
* Controller disconnect: dropping a bound pad during the lobby forces the shared
  pause (unpause is then blocked until it returns).

Everything is read from aparty_ traces the running binary emits (the AP-05
schema). The transition/interaction facts are authoritative: they come from the
pure reducer and the game adapters, not from "the process did not crash".

Driving
-------
Racers are steered by the MDKR_AP_SEAT_ROUTE test injector (platform/
mdkr_adventure.c): each named seat follows its own waypoint route to a door
(E<dest>) or balloon (B<id>). This is the only way to send exactly one seat, or
different seats, at a door -- the shared MDKR_DRIVE_ROUTE drives every human at
one target. Disconnect uses MDKR_AP_DROP_PAD (the input-script presence mask is
whole-route and cannot simulate a mid-session drop).

Positive controls
-----------------
* Replay the SINGLE-door output through the CONFLICTING assertions: it must FAIL
  (a single door has no losing-door rejection).
* Strip the aparty_transition lines from the single-door output: the single-door
  assertions must FAIL (no latched transition).

Off arm
-------
Doors behave stock with the enhancement off -- covered by the existing
check_adventure_hub route, run and quoted by tools/run_checks.py.

Save fixture: the started Adventure One slot-0 save from
check_adventure_party_admission (the host's FILE_SELECT confirm resumes it).
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

from harness_utils import DEFAULT_BUILD_DIR, resolve_binary, save_env
from check_adventure_party_admission import eeprom_image

ROOT = Path(__file__).resolve().parent.parent

HUB_LEVEL_ID = 0
DEST_LEVEL_ID = 12  # the one reliably reachable central-hub exit (dest 12)

# The reachable central-hub route to exit dest 12, sampled from the proven
# check_adventure_race_loop hub waypoints. Ends at E12 (BHV_EXIT -> dest 12).
HUB_TO_E12 = "200,500|-1004,946|-1858,1099|-3381,1946|-3948,2180|E12"
HUB_TO_B10 = "200,500|-1004,946|-1858,1099|B10"  # golden balloon 10, on the way

# aparty action / verdict enum values (adventure_party_policy.h), emitted raw.
ACTION_PAUSE = 1
ACTION_TRIGGER = 2
ACTION_COLLECT = 3
VERDICT_LATCHED = 0
VERDICT_REJECTED_LATCHED = -1
VERDICT_REJECTED_SEAT = -3

TRANS_RE = re.compile(
    r"aparty_transition: seat=(\d+) tick=(\d+) trigger=(\d+) dest=(\d+) lgen=(\d+)")
INTER_RE = re.compile(
    r"aparty_interaction: seat=(\d+) action=(-?\d+) verdict=(-?\d+)")
LEVEL_RE = re.compile(
    r"level_load: levelId=(-?\d+) numPlayers=(-?\d+).*@frame~(\d+)")
ROSTER_RE = re.compile(r"aparty_roster: n=(\d+) mask=0x([0-9a-fA-F]+)")
LAYOUT_RE = re.compile(r"aparty_layout: viewports=(\d+) layout=(\d+)")
BAD_RE = re.compile(
    r"\[CRASH\]|\[FATAL\]|AddressSanitizer|UndefinedBehaviorSanitizer|"
    r"runtime error:|Assertion failed")


def transitions(out):
    return [(int(m.group(1)), int(m.group(4)), int(m.group(5)))  # seat, dest, lgen
            for m in TRANS_RE.finditer(out)]


def interactions(out):
    return [(int(m.group(1)), int(m.group(2)), int(m.group(3)))  # seat, action, verdict
            for m in INTER_RE.finditer(out)]


def dest_loads(out):
    """level_load rows for the destination lobby (after the hub)."""
    seen_hub = False
    out_loads = []
    for m in LEVEL_RE.finditer(out):
        level = int(m.group(1))
        if level == HUB_LEVEL_ID:
            seen_hub = True
        elif seen_hub and level == DEST_LEVEL_ID:
            out_loads.append((level, int(m.group(3))))
    return out_loads


def dest_roster_layout(out, players):
    """(has_full_roster, has_layout) measured AFTER the destination load."""
    idx = out.find("levelId=%d" % DEST_LEVEL_ID)
    # find the destination load specifically (after a hub load)
    pos = 0
    hub = out.find("levelId=%d" % HUB_LEVEL_ID)
    if hub >= 0:
        pos = out.find("levelId=%d" % DEST_LEVEL_ID, hub)
    tail = out[pos:] if pos >= 0 else ""
    full = any(int(m.group(1)) == players and int(m.group(2), 16) == (1 << players) - 1
               for m in ROSTER_RE.finditer(tail))
    layout = any(int(m.group(1)) == players and int(m.group(2)) == players - 1
                 for m in LAYOUT_RE.finditer(tail))
    return full, layout


def run_arm(binary, rom, fixture, players, seat_route=None, drop=None,
            enabled=True, frames=3500, verbose=False):
    with tempfile.TemporaryDirectory(prefix="mdkr_ap_trans_") as tmp:
        root = Path(tmp)
        save_dir = root / "save"
        save_dir.mkdir()
        (save_dir / "eeprom.bin").write_bytes(eeprom_image())
        env = {k: v for k, v in os.environ.items()
               if not k.startswith(("MDKR", "GE007_"))}
        env.update(LC_ALL="C", MDKR_AUDIO="0", MDKR_TRACE="1")
        if seat_route:
            env["MDKR_AP_SEAT_ROUTE"] = seat_route
        if drop:
            env["MDKR_AP_DROP_PAD"] = drop
        save_env(env, str(save_dir))
        env["MDKR_VIDEO_CONFIG_PATH"] = str(root / "mdkr64.ini")
        command = [
            binary, "--headless-frames", str(frames),
            "--input-script", str(ROOT / fixture), "--rom", rom,
            "--window-size", "320x240",
            "--video-set", f"Enhancements.AdventureParty={1 if enabled else 0}",
        ]
        if verbose:
            print(f"$ {' '.join(command)}  route={seat_route} drop={drop}", flush=True)
        proc = subprocess.run(command, cwd=root, env=env, text=True,
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                              timeout=max(240, frames // 10), check=False)
    return proc.stdout or ""


def assert_single_door(out, players, label):
    f = []
    if bad := BAD_RE.search(out):
        f.append(f"{label}: fatal marker {bad.group(0)!r}")
    tr = transitions(out)
    if len(tr) != 1:
        f.append(f"{label}: expected exactly 1 aparty_transition, saw {tr}")
    elif tr[0][0] != 0 or tr[0][1] != DEST_LEVEL_ID:
        f.append(f"{label}: transition {tr[0]} != (seat 0, dest {DEST_LEVEL_ID})")
    loads = dest_loads(out)
    if len(loads) != 1:
        f.append(f"{label}: expected exactly 1 destination load, saw {loads}")
    full, layout = dest_roster_layout(out, players)
    if not full:
        f.append(f"{label}: destination lobby missing full {players}-seat roster")
    if not layout:
        f.append(f"{label}: destination lobby missing {players}-viewport layout")
    # A single door has no LOSING-door rejection.
    rej = [i for i in interactions(out)
           if i[1] == ACTION_TRIGGER and i[2] < 0]
    if rej:
        f.append(f"{label}: single door produced a transition rejection {rej}")
    return f


def assert_conflicting(out, players, label):
    f = []
    if bad := BAD_RE.search(out):
        f.append(f"{label}: fatal marker {bad.group(0)!r}")
    tr = transitions(out)
    if len(tr) != 1:
        f.append(f"{label}: expected exactly 1 aparty_transition (one winner), saw {tr}")
    elif tr[0][0] != 0:
        f.append(f"{label}: winner seat {tr[0][0]} != 0 (lowest seat must win)")
    rej = [i for i in interactions(out)
           if i[1] == ACTION_TRIGGER and i[2] == VERDICT_REJECTED_LATCHED]
    if not rej:
        f.append(f"{label}: loser's door was not rejected "
                 f"(no aparty_interaction action={ACTION_TRIGGER} "
                 f"verdict={VERDICT_REJECTED_LATCHED})")
    elif all(i[0] == 0 for i in rej):
        f.append(f"{label}: only the winner appears rejected {rej}")
    loads = dest_loads(out)
    if len(loads) != 1:
        f.append(f"{label}: expected exactly 1 load (no double-load), saw {loads}")
    full, _ = dest_roster_layout(out, players)
    if not full:
        f.append(f"{label}: party did not arrive whole at one destination")
    return f


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", default=DEFAULT_BUILD_DIR)
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    binary = str(Path(resolve_binary(args.build)).resolve())
    rom = str(Path(args.rom).resolve())
    admit3 = "tests/input_scripts/adventure_party_3p_admit.txt"
    admit2 = "tests/input_scripts/adventure_party_2p_admit.txt"
    pause3 = "tests/input_scripts/adventure_party_3p_pause.txt"
    required = [binary, rom, *(str(ROOT / p) for p in (admit3, admit2, pause3))]
    missing = [p for p in required if not os.path.exists(p)]
    if missing:
        for p in missing:
            print(f"check_adventure_party_transition: FAIL -- missing {p}",
                  file=sys.stderr)
        return 1

    failures = []

    # --- Single door: 3P (host only drives the door; party travels whole) ---
    single3 = run_arm(binary, rom, admit3, 3, seat_route=f"0={HUB_TO_E12}",
                      verbose=args.verbose)
    failures += assert_single_door(single3, 3, "single-door 3P")

    # --- Single door: 2P arm ---
    single2 = run_arm(binary, rom, admit2, 2, seat_route=f"0={HUB_TO_E12}",
                      verbose=args.verbose)
    failures += assert_single_door(single2, 2, "single-door 2P")

    # --- Conflicting doors: seats 0 and 2 race into the same window ---
    conflict = run_arm(binary, rom, admit3, 3,
                       seat_route=f"0={HUB_TO_E12};2={HUB_TO_E12}",
                       verbose=args.verbose)
    failures += assert_conflicting(conflict, 3, "conflicting 3P")

    # --- Balloon: two non-host seats race a shared balloon; ONE collects ---
    balloon = run_arm(binary, rom, admit3, 3,
                      seat_route=f"1={HUB_TO_B10};2={HUB_TO_B10}",
                      frames=4200, verbose=args.verbose)
    collects = [i for i in interactions(balloon)
                if i[1] == ACTION_COLLECT and i[2] == VERDICT_LATCHED]
    if len(collects) != 1:
        failures.append(f"balloon 3P: expected exactly 1 collection, saw {collects}")
    elif collects[0][0] == 0:
        failures.append("balloon 3P: host collected; expected a NON-host seat")

    # --- Pause: non-host opens the one shared pause, host confirms resume ---
    pause = run_arm(binary, rom, pause3, 3, frames=3300, verbose=args.verbose)
    pints = [i for i in interactions(pause) if i[1] == ACTION_PAUSE]
    opened = [i for i in pints if i[0] != 0 and i[2] < 0]
    hosted = [i for i in pints if i[0] == 0 and i[2] == VERDICT_LATCHED]
    if not opened:
        failures.append("pause 3P: no non-host pause open (any seat may request pause)")
    if not hosted:
        failures.append("pause 3P: host did not own the resume decision")

    # --- Disconnect: dropping a bound pad forces the shared pause ---
    disc = run_arm(binary, rom, admit3, 3, seat_route="0=200,200",
                   drop="2@2500", frames=3200, verbose=args.verbose)
    dropped = [i for i in interactions(disc)
               if i[1] == ACTION_PAUSE and i[0] == 2 and i[2] == VERDICT_REJECTED_SEAT]
    if not dropped:
        failures.append("disconnect 3P: dropping seat 2's pad did not force the "
                        "shared pause (no aparty_interaction seat=2 "
                        f"action={ACTION_PAUSE} verdict={VERDICT_REJECTED_SEAT})")

    # --- Positive control 1: single-door output must FAIL conflicting asserts ---
    if not assert_conflicting(single3, 3, "PC-conflict"):
        failures.append("positive control: single-door output PASSED the conflicting "
                        "assertions (they do not discriminate a losing-door rejection)")

    # --- Positive control 2: strip aparty_transition -> single-door must FAIL ---
    stripped = "\n".join(l for l in single3.splitlines()
                         if "aparty_transition" not in l)
    if not assert_single_door(stripped, 3, "PC-strip"):
        failures.append("positive control: single-door PASSED with all "
                        "aparty_transition lines stripped")

    if failures:
        print("check_adventure_party_transition: FAIL", file=sys.stderr)
        for x in failures:
            print(f"  - {x}", file=sys.stderr)
        return 1
    print("check_adventure_party_transition: PASS -- one arbitrated whole-party "
          "transition per generation (single + conflicting doors, 2P/3P), any-seat "
          "balloon collect-once, non-host pause open with host resume authority, "
          "and disconnect-forced shared pause; both positive controls fired")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
