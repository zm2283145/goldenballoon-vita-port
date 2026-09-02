#!/usr/bin/env python3
"""fast3d display-list hardening: a misauthored stream stops the walk.

The renderer's two display-list walkers -- the interpreter (dkr_run_dl) and the
output-overlay prepass (dkr_scan_overlay_order) -- read commands out of
game-authored memory. When the game authors past the end of a display-list
buffer they parse whatever follows as commands, and a word that decodes as
G_DL/G_DMADL sends them into storage nothing sized for them. That is the 4P
party-hub crash settled under AddressSanitizer: the authoring defect was fixed
at its cause, and this gate covers the walkers' side of it.

Arms
----
* **A -- well-authored route (the no-change assertion).** The 4P party route
  runs hub -> lobby -> race with correctly sized heaps. Zero `[DL]` lines: the
  refusals below never fire on content the game authored properly, so nothing
  the retail or party walk draws is affected by them.
* **B -- misauthored route (the route that tripped the sanitizer).**
  MDKR_TEST_UNDERSIZED_DL_HEAP=1 keeps the retail 1P heap row while four
  viewports author against it, reproducing the exact overflowing stream. The
  run must survive the whole route -- hub, lobby and race all reached, no
  crash marker -- and both walkers must report that they stopped: the prepass
  at an opcode the interpreter does not implement, the interpreter at the same
  evidence. Before the hardening this route read past the end of an 80-byte
  global (ASan: global-buffer-overflow in dkr_scan_overlay_order, then in
  dkr_run_dl) and aborted at tick ~2401.
* **C -- retail 1P route.** The renderer is shared, so the same assertions run
  over a 1P time-trial route with MDKR_DL_CENSUS=1: zero faults counted by the
  census, and zero `[DL]` lines.

Positive controls
-----------------
* Arm B asserts the injector actually misauthored something: its peak submitted
  display-list length must exceed the 1P budget the heap was sized to (4500
  Gfx). Without that, an injector that silently stopped working would leave
  arm B asserting the hardening against a clean stream.
* Arm A asserts the same length is authored on the well-sized run, so the two
  arms differ only in the heap the game wrote into -- not in what it drew.
* Arm A's zero-`[DL]` assertion is the control for the opcode set itself: the
  prepass reports every stop through the same channel, so a set missing an
  opcode real content uses shows up there as a stop on a clean route.

`--injected-only` runs arm B alone. That is how the AddressSanitizer lane runs
it: the release build survives the reads this gate is about -- a command
fetched one past the end of an 80-byte global, and the fault printer quoting
the words of an address the walk had just refused, both land in mapped memory
and report success. Only a sanitized build makes those a failure, and
ASAN_OPTIONS=abort_on_error=1 makes the first one fatal.

Every launch is headless and muted. Exit 0 = pass.
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
from check_adventure_party_race_loop import (
    DRIVE_ROUTE, HUB_LEVEL_ID, LOBBY_LEVEL_ID, RACE_LEVEL_ID)

ROOT = Path(__file__).resolve().parent.parent

PARTY_SCRIPT = ROOT / "tests" / "input_scripts" / "adventure_party_4p_admit.txt"
SOLO_SCRIPT = ROOT / "tests" / "input_scripts" / "nav_to_time_trial_race.txt"
PARTY_FRAMES = 4600
SOLO_FRAMES = 1800

# gNumF3dCmdsPerPlayer[0] (game/src/thread3_main.c): the one-player display-list
# budget the injector holds the four-viewport hub to.
ONE_PLAYER_DL_COMMANDS = 4500

DL_FAULT_RE = re.compile(r"^\[DL\] (.*?) at depth=", re.MULTILINE)
GFXTASK_RE = re.compile(r"gfxtask: type=\d+ dl=0x[0-9a-f]+ len=(\d+)")
LEVEL_RE = re.compile(r"level_load: levelId=(-?\d+) ")
CENSUS_RE = re.compile(r"^\[DL-CENSUS\] .*faults=(\d+)", re.MULTILINE)
# The crash screen prints [CRASHSCREEN]; [CRASH] alone never matches it.
BAD_RE = re.compile(
    r"\[CRASH(?:SCREEN)?\]|\[FATAL\]|AddressSanitizer|"
    r"UndefinedBehaviorSanitizer|runtime error:|Assertion failed")

PREPASS_STOP = "overlay prepass reached an unknown display-list opcode"
INTERPRETER_STOP = "unknown display-list opcode"


def run_route(binary, rom, root, label, script, frames, extra_env, timeout,
              verbose, party=False):
    """One headless launch; returns its combined output."""
    run_dir = root / label
    save_dir = run_dir / "save"
    save_dir.mkdir(parents=True)
    (save_dir / "eeprom.bin").write_bytes(eeprom_image())
    env = {k: v for k, v in os.environ.items()
           if not k.startswith(("MDKR", "GE007_"))}
    env.update(LC_ALL="C", MDKR_AUDIO="0", MDKR64_HIDDEN="1", MDKR_TRACE="1",
               MDKR_AUTOPILOT="1", MDKR_FORCE_LAPS="1",
               # Inert on an uninstrumented build; on the ASan lane it is what
               # turns a past-the-end read into this gate's verdict.
               ASAN_OPTIONS=("abort_on_error=1:malloc_context_size=30"
                             ":detect_leaks=0"),
               MallocNanoZone="0")
    env.update(extra_env)
    save_env(env, str(save_dir))
    env["MDKR_VIDEO_CONFIG_PATH"] = str(run_dir / "mdkr64.ini")
    command = [
        str(binary), "--headless-frames", str(frames),
        "--input-script", str(script), "--rom", str(rom),
        "--window-size", "320x240",
    ]
    if party:
        command += ["--video-set", "Enhancements.AdventureParty=1"]
    if verbose:
        print(f"$ ({label}) {' '.join(command)}", flush=True)
    process = subprocess.run(
        command, cwd=run_dir, env=env, text=True, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, timeout=timeout, check=False)
    output = process.stdout or ""
    if process.returncode != 0:
        raise RuntimeError(
            f"{label}: exit {process.returncode}\n{output[-3000:]}")
    if BAD_RE.search(output):
        raise RuntimeError(
            f"{label}: fatal marker in output\n{output[-3000:]}")
    return output


def peak_dl_length(output):
    lengths = [int(m) for m in GFXTASK_RE.findall(output)]
    return max(lengths) if lengths else 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", default=DEFAULT_BUILD_DIR)
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    parser.add_argument("--timeout", type=int, default=900)
    parser.add_argument("-v", "--verbose", action="store_true")
    parser.add_argument(
        "--injected-only", action="store_true",
        help="run arm B alone (the AddressSanitizer lane's arm)")
    args = parser.parse_args()

    binary = Path(os.path.abspath(resolve_binary(args.build)))
    rom = Path(os.path.abspath(args.rom))
    missing = [str(p) for p in (binary, rom, PARTY_SCRIPT, SOLO_SCRIPT)
               if not os.path.exists(p)]
    if missing:
        for path in missing:
            print(f"check_fast3d_dl_hardening: FAIL -- missing {path}",
                  file=sys.stderr)
        return 1

    party_env = {
        "MDKR_DRIVE_ROUTE": DRIVE_ROUTE,
        "MDKR_SIMULATION_CADENCE": "enhanced",
        "MDKR_SYNTH_FIELDS": "1",
        "MDKR_AP_RACE_WINNER": "0",
        "MDKR_TEST_POSTRACE_OPTION": "1",
    }
    failures = []
    with tempfile.TemporaryDirectory(prefix="mdkr-fast3d-dl-") as tmp:
        root = Path(tmp)
        clean = solo = None
        try:
            if not args.injected_only:
                clean = run_route(binary, rom, root, "clean", PARTY_SCRIPT,
                                  PARTY_FRAMES, party_env, args.timeout,
                                  args.verbose, party=True)
            injected = run_route(
                binary, rom, root, "injected", PARTY_SCRIPT, PARTY_FRAMES,
                dict(party_env, MDKR_TEST_UNDERSIZED_DL_HEAP="1"),
                args.timeout, args.verbose, party=True)
            if not args.injected_only:
                solo = run_route(binary, rom, root, "solo", SOLO_SCRIPT,
                                 SOLO_FRAMES,
                                 {"MDKR_LOAD_TRACK": "5", "MDKR_DL_CENSUS": "1"},
                                 args.timeout, args.verbose)
        except (RuntimeError, subprocess.TimeoutExpired) as exc:
            print(f"check_fast3d_dl_hardening: FAIL -- {exc}", file=sys.stderr)
            return 1

        # --- arm A: the well-authored route reports nothing ---
        clean_faults = DL_FAULT_RE.findall(clean) if clean is not None else []
        if clean_faults:
            failures.append(
                "arm A: the well-authored 4P route reported "
                f"{len(clean_faults)} display-list faults, first "
                f"{clean_faults[0]!r}; the walkers' refusals must not fire on "
                "content the game authored properly")

        # --- arm B: the misauthored route is survived, and both walkers say so
        injected_faults = DL_FAULT_RE.findall(injected)
        if PREPASS_STOP not in injected_faults:
            failures.append(
                "arm B: the overlay prepass never stopped at an unimplemented "
                "opcode on the misauthored route; it either did not walk the "
                f"stream or walked through it. Faults seen: "
                f"{sorted(set(injected_faults))}")
        if INTERPRETER_STOP not in injected_faults:
            failures.append(
                "arm B: the interpreter never refused an unimplemented opcode "
                f"on the misauthored route. Faults seen: "
                f"{sorted(set(injected_faults))}")
        levels = {int(m) for m in LEVEL_RE.findall(injected)}
        for level, name in ((HUB_LEVEL_ID, "hub"), (LOBBY_LEVEL_ID, "lobby"),
                            (RACE_LEVEL_ID, "race")):
            if level not in levels:
                failures.append(
                    f"arm B: the misauthored route never reached the {name} "
                    f"(level {level}); levels loaded: {sorted(levels)}")

        # --- positive control: the injector really did misauthor the stream ---
        injected_peak = peak_dl_length(injected)
        clean_peak = (peak_dl_length(clean) if clean is not None
                      else ONE_PLAYER_DL_COMMANDS + 1)
        if injected_peak <= ONE_PLAYER_DL_COMMANDS:
            failures.append(
                f"positive control: the misauthored route's peak display list "
                f"({injected_peak} Gfx) did not exceed the 1P budget it was "
                f"held to ({ONE_PLAYER_DL_COMMANDS}); the injector authored "
                "nothing to walk over and arm B proves nothing")
        if clean_peak <= ONE_PLAYER_DL_COMMANDS:
            failures.append(
                f"positive control: the well-authored route's peak display "
                f"list ({clean_peak} Gfx) did not exceed {ONE_PLAYER_DL_COMMANDS}"
                ", so the two arms do not draw the same frames and arm A is "
                "not the control for arm B")

        # --- arm C: the shared renderer on a retail 1P route ---
        if solo is not None:
            solo_faults = DL_FAULT_RE.findall(solo)
            if solo_faults:
                failures.append(
                    f"arm C: the 1P route reported {len(solo_faults)} "
                    f"display-list faults, first {solo_faults[0]!r}")
            census = CENSUS_RE.search(solo)
            if census is None:
                failures.append(
                    "arm C: MDKR_DL_CENSUS=1 emitted no [DL-CENSUS] row, so "
                    "the 1P route's opcode coverage was never counted")
            elif int(census.group(1)) != 0:
                failures.append(
                    f"arm C: the 1P census counted {census.group(1)} "
                    "display-list faults")

    if failures:
        for failure in failures:
            print(f"check_fast3d_dl_hardening: FAIL -- {failure}",
                  file=sys.stderr)
        return 1
    if args.injected_only:
        print("check_fast3d_dl_hardening: PASS -- arm B only: a four-viewport "
              "party authoring past its display-list buffer is walked to a "
              "stop by both the interpreter and the overlay prepass, and the "
              "whole hub->lobby->race route still runs")
        return 0
    print("check_fast3d_dl_hardening: PASS -- a four-viewport party authoring "
          "past its display-list buffer is walked to a stop by both the "
          "interpreter and the overlay prepass, the whole route still runs, "
          "and neither walker reports anything on the well-authored party "
          "route or on a retail 1P race")
    return 0


if __name__ == "__main__":
    sys.exit(main())
