#!/usr/bin/env python3
"""START mid-race in an ONLINE session: non-blocking overlay, never a crash.

THE OWNER-REPORTED TWO-MACHINE CRASH THIS LANE PINS (online-1.6.0 beta, Ancient
Lake): the host pressed START mid-race; the NON-pausing machine hit `fault
abort` (SIGABRT crash dialog) at the pause tick; the host then saw the dead
process resolve through the mesh ladders as `[MESH] peer LOST` ~20 s later.

ROOT CAUSE (reproduced in-process at HEAD, rc 134): the retail pause runs
INSIDE the networked simulation. mode_game's pause branch consumes the
CANONICAL input of every seat, so a START press -- the local player's or the
remote's -- engages gIsPaused/menu_pause_init in the online rollback sim. A
paused sim makes mdkr_game_resimulate_tick refuse every rollback-correction
replay (admission rejects a paused game; completion rejects a replay that
ENGAGES the pause: `game-tick completion rejected ... paused=1`), and
reconcile_network_inputs escalated that refusal into the FATAL arm ->
`[ROLLBACK] online correction replay failed tick=N` -> `[FATAL] rollback lab
could not prepare canonical input` -> abort(). Online play corrects nearly
every tick, so the first START edge that crossed a correction killed the
non-pausing machine on the spot.

THE RULED FIX, both halves asserted here:
  1. ONLINE PAUSE IS A NON-BLOCKING OVERLAY (the MK8 model): the retail engage
     is suppressed deterministically on every endpoint (identical live and in
     resim -- keyed only on the online input runtime), and the LOCAL seat's
     START opens a presentation-only CONTINUE / LEAVE RACE overlay over the
     still-running race. The remote seat's START opens NOTHING locally. LEAVE
     routes to the existing clean note-LEFT return-to-room.
  2. THE BELT: a correction replay the sim REFUSES (paused / zero-rate /
     level-ending state) is a RECOVERABLE sim-state verdict, never an abort:
     `online correction replay refused tick=N (sim-state; recoverable)` -> the
     proven clean LEFT return. Pinned through the REAL mechanism via the
     test-only MDKR_APP_TEST_ONLINE_ALLOW_RETAIL_PAUSE seam (legacy engage kept
     reachable), so the belt stays load-bearing even though the overlay fix
     makes the pause class unreachable in production.

Rig: the in-process two-adapter loopback race (MDKR_APP_TEST_ONLINE_LIVE) with
the forced-prediction window (MDKR_APP_TEST_ONLINE_LIVE_PREDICT) sustaining a
correction on nearly every tick -- the WAN shape that killed the beta -- and
the synthetic-pad button script (MDKR_APP_TEST_ONLINE_SYNTH_PAD_SLOT/_SCRIPT)
driving REAL canonical START/D-pad/A edges through the sealed input stream.

Arms (each a full engine boot):
  A. remote START inside the correction storm  == the beta crash shape.
     Pre-fix: rc 134 + `completion rejected ... paused=1`. Post-fix: the
     correction spanning the START tick reconciles, the race runs to the
     budget, both endpoints converge, and no overlay opens (remote START).
  B. remote START landing ON TIME (no prediction): the suppression witness
     fires on the live pass, no overlay opens, the race completes.
  C. local START under the storm: overlay OPEN -> CLOSED (resume; race still
     running -- corrections keep reconciling while it is open) -> reOPEN ->
     D-pad DOWN -> A == LEAVE RACE -> the clean LEFT return, rc 0, zero leaks.
  D. THE BELT (legacy engage allowed): the exact pre-fix abort branch fires --
     `game-tick completion rejected ... paused=1` -- and is routed to
     `replay refused (sim-state; recoverable)` -> clean LEFT, rc 0, no abort.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

from harness_utils import ABORT_MARKERS, resolve_binary
from online_lane_util import (
    ENGINE_LIVE_RE, FORBIDDEN_ONLINE, forbidden_marker, make_fail, run_engine,
)

TICKS_BUDGET = 1200          # arms A/B: race runs to the autoplay budget
TICKS_LEAVE = 1500           # arm C leaves at ~tick 887; headroom only
START_TICK = 600             # scripted press: well past the race countdown
MIN_RACED_TICKS = 1100       # non-vacuous: the race ran far past the press
MIN_STORM_CORRECTIONS = 500  # non-vacuous: the prediction storm was real

SCRIPT_ARMED_RE = re.compile(
    r"^\[online-live\] TEST: synthetic pad script armed slot=(\d+) "
    r"windows=(\d+)", re.MULTILINE)
SUPPRESSED_RE = re.compile(
    r"^\[online-pause\] retail pause suppressed \(online race; overlay owns "
    r"START\)", re.MULTILINE)
OVERLAY_OPEN_RE = re.compile(
    r"^\[online-pause\] overlay OPEN tick=(\d+) \(sim keeps running\)",
    re.MULTILINE)
OVERLAY_CLOSED_RE = re.compile(
    r"^\[online-pause\] overlay CLOSED tick=(\d+) \(resume\)", re.MULTILINE)
LEAVE_SELECTED_RE = re.compile(
    r"^\[online-pause\] LEAVE RACE selected tick=(\d+) -> clean LEFT return",
    re.MULTILINE)
LEAVE_LEFT_RE = re.compile(
    r"^\[online-session\] LEFT: local player left the race \(pause overlay\)",
    re.MULTILINE)
BELT_ALLOWED_RE = re.compile(
    r"^\[online-pause\] TEST: retail pause engage ALLOWED \(belt lane\)",
    re.MULTILINE)
BELT_REFUSED_RE = re.compile(
    r"^\[ROLLBACK\] online correction replay refused tick=(\d+) "
    r"\(sim-state; recoverable\)", re.MULTILINE)
BELT_PAUSED_REJECT_RE = re.compile(
    r"^\[ROLLBACK\] game-tick (?:admission|completion) rejected .*paused=1",
    re.MULTILINE)
BELT_LEFT_RE = re.compile(
    r"^\[online-session\] LEFT: online peer/input lost \(recoverable boundary "
    r"starvation\)", re.MULTILINE)
RECONCILED_RE = re.compile(
    r"^\[ROLLBACK\] online correction reconciled ticks=(\d+)\.\.(\d+)",
    re.MULTILINE)
HOST_SHUTDOWN_RE = re.compile(
    r"^\[HOST-SHUTDOWN\] rom=(\d+) arena=(\d+) delayedFree=(\d+)", re.MULTILINE)
# The pre-fix fatal chain, FORBIDDEN in every arm.
REPLAY_FATAL_MARKERS = (
    "online correction replay failed",
    "could not prepare canonical input",
)

fail = make_fail("pause overlay")


def run_arm(binary: Path, rom: Path, *, name: str, ticks: int, timeout: int,
            verbose: bool, extra: dict[str, str]) -> tuple[int, str] | int:
    try:
        return run_engine(binary, rom, ticks=ticks, timeout=timeout,
                          verbose=verbose, extra_env=extra,
                          prefix=f"mdkr64-online-pause-{name}-")
    except subprocess.TimeoutExpired as error:
        return fail(f"[{name}] engine run timed out (a hang instead of a "
                    f"bounded return): {error}")


def base_env(slot: int, script: str) -> dict[str, str]:
    return {
        "MDKR_APP_TEST_ONLINE_LIVE": "1",
        "MDKR_APP_TEST_ONLINE_SYNTH_PAD_SLOT": str(slot),
        "MDKR_APP_TEST_ONLINE_SYNTH_PAD_SCRIPT": script,
    }


def check_common(name: str, returncode: int, output: str,
                 forbid_belt_refusal: bool) -> int | None:
    """Assertions shared by every arm; None when clean."""
    marker = forbidden_marker(
        output, *FORBIDDEN_ONLINE, *ABORT_MARKERS, *REPLAY_FATAL_MARKERS)
    if marker:
        return fail(f"[{name}] observed fatal/abort marker {marker!r} -- the "
                    f"pre-fix crash chain", output)
    if returncode != 0:
        return fail(f"[{name}] process exited {returncode}, expected 0",
                    output)
    if not SCRIPT_ARMED_RE.search(output):
        return fail(f"[{name}] the synthetic pad script never armed "
                    f"(vacuous run)", output)
    if forbid_belt_refusal and BELT_REFUSED_RE.search(output):
        return fail(f"[{name}] the belt fired -- the overlay fix should have "
                    f"kept the sim un-paused and the replay lawful", output)
    return None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", default="build-beta")
    parser.add_argument("--rom", type=Path, default="baserom.us.v80.z64")
    parser.add_argument("--timeout", type=int, default=300)
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    binary = Path(resolve_binary(args.build)).expanduser().resolve()
    rom = args.rom.expanduser().resolve()
    for path, label in ((binary, "binary"), (rom, "ROM")):
        if not path.is_file():
            parser.error(f"missing {label}: {path}")

    press = f"{START_TICK}-{START_TICK + 9}:0x9000"  # A held + START edge

    # --- Arm A: remote START inside the correction storm (the beta crash) ---
    result = run_arm(
        binary, rom, name="storm", ticks=TICKS_BUDGET, timeout=args.timeout,
        verbose=args.verbose,
        extra={**base_env(slot=1, script=press),
               "MDKR_APP_TEST_ONLINE_LIVE_PREDICT": "100000"})
    if isinstance(result, int):
        return result
    returncode, output = result
    bad = check_common("storm", returncode, output, forbid_belt_refusal=True)
    if bad is not None:
        return bad
    live = ENGINE_LIVE_RE.search(output)
    if not live:
        return fail("[storm] no ENGINE-ONLINE-LIVE summary", output)
    raced, corrected, converged = (
        int(live.group(2)), int(live.group(7)), int(live.group(13)))
    if raced < MIN_RACED_TICKS:
        return fail(f"[storm] race ended at tick {raced} (< {MIN_RACED_TICKS})"
                    f" -- it did not survive the START press", output)
    if corrected < MIN_STORM_CORRECTIONS:
        return fail(f"[storm] only {corrected} corrections -- the prediction "
                    f"storm (the crash's precondition) never happened", output)
    if converged != 1:
        return fail("[storm] endpoints did not converge after the START-tick "
                    "correction", output)
    if not any(int(m.group(2)) >= START_TICK for m in
               RECONCILED_RE.finditer(output)):
        return fail(f"[storm] no correction reconciled across/past the START "
                    f"tick {START_TICK} -- the crash shape was not exercised",
                    output)
    if OVERLAY_OPEN_RE.search(output):
        return fail("[storm] the REMOTE seat's START opened the LOCAL overlay",
                    output)
    storm_summary = f"raced={raced} corrections={corrected} converged"

    # --- Arm B: remote START on time (live pass) -- suppression witness ------
    result = run_arm(
        binary, rom, name="live", ticks=TICKS_BUDGET, timeout=args.timeout,
        verbose=args.verbose, extra=base_env(slot=1, script=press))
    if isinstance(result, int):
        return result
    returncode, output = result
    bad = check_common("live", returncode, output, forbid_belt_refusal=True)
    if bad is not None:
        return bad
    if not SUPPRESSED_RE.search(output):
        return fail("[live] the retail-pause suppression witness never fired "
                    "for the on-time remote START", output)
    if OVERLAY_OPEN_RE.search(output):
        return fail("[live] the REMOTE seat's START opened the LOCAL overlay",
                    output)
    live = ENGINE_LIVE_RE.search(output)
    if not live or int(live.group(2)) < MIN_RACED_TICKS:
        return fail("[live] the race did not run past the suppressed press",
                    output)

    # --- Arm C: local START under the storm -- open / resume / leave ---------
    leave_script = (
        "600-649:0x1000,650-699:0x0,"     # START edge -> OPEN; release
        "700-749:0x1000,750-799:0x0,"     # START edge -> CLOSED (resume)
        "800-849:0x1000,850-859:0x0,"     # START edge -> reOPEN
        "860-869:0x400,870-879:0x0,"      # D-pad DOWN -> row LEAVE RACE
        "880-889:0x8000")                 # A edge -> LEAVE
    result = run_arm(
        binary, rom, name="leave", ticks=TICKS_LEAVE, timeout=args.timeout,
        verbose=args.verbose,
        extra={**base_env(slot=0, script=leave_script),
               "MDKR_APP_TEST_ONLINE_LIVE_PREDICT": "100000"})
    if isinstance(result, int):
        return result
    returncode, output = result
    bad = check_common("leave", returncode, output, forbid_belt_refusal=True)
    if bad is not None:
        return bad
    opens = [int(m.group(1)) for m in OVERLAY_OPEN_RE.finditer(output)]
    closes = [int(m.group(1)) for m in OVERLAY_CLOSED_RE.finditer(output)]
    leave = LEAVE_SELECTED_RE.search(output)
    if len(opens) != 2 or len(closes) != 1 or not leave:
        return fail(f"[leave] overlay flow wrong: opens={opens} "
                    f"closes={closes} leave={bool(leave)} (want open -> "
                    f"resume -> reopen -> leave)", output)
    leave_tick = int(leave.group(1))
    if not (opens[0] < closes[0] < opens[1] < leave_tick):
        return fail(f"[leave] overlay ticks out of order: open={opens[0]} "
                    f"close={closes[0]} reopen={opens[1]} leave={leave_tick}",
                    output)
    # THE SIM NEVER STOPS: corrections kept reconciling while the overlay was
    # open (pre-fix, a paused sim refused every one of these -> abort).
    open_window = [
        m for m in RECONCILED_RE.finditer(output)
        if opens[0] <= int(m.group(1)) and int(m.group(2)) <= leave_tick]
    if not open_window:
        return fail("[leave] no correction reconciled while the overlay was "
                    "open -- cannot prove the sim kept running", output)
    if not LEAVE_LEFT_RE.search(output):
        return fail("[leave] LEAVE RACE did not take the clean LEFT return",
                    output)
    shutdown = HOST_SHUTDOWN_RE.findall(output)
    if not shutdown or tuple(map(int, shutdown[-1])) != (0, 0, 0):
        return fail(f"[leave] host teardown leaked: "
                    f"{shutdown[-1] if shutdown else 'no witness'}", output)
    leave_summary = (f"open={opens[0]} resume={closes[0]} reopen={opens[1]} "
                     f"leave={leave_tick} "
                     f"({len(open_window)} corrections while open)")

    # --- Arm D: THE BELT (legacy engage allowed) -- refuse, never abort ------
    result = run_arm(
        binary, rom, name="belt", ticks=TICKS_BUDGET, timeout=args.timeout,
        verbose=args.verbose,
        extra={**base_env(slot=1, script=press),
               "MDKR_APP_TEST_ONLINE_LIVE_PREDICT": "100000",
               "MDKR_APP_TEST_ONLINE_ALLOW_RETAIL_PAUSE": "1"})
    if isinstance(result, int):
        return result
    returncode, output = result
    marker = forbidden_marker(
        output, *FORBIDDEN_ONLINE, *ABORT_MARKERS, *REPLAY_FATAL_MARKERS)
    if marker:
        return fail(f"[belt] observed fatal/abort marker {marker!r} -- the "
                    f"belt must route the refusal, never abort", output)
    if returncode != 0:
        return fail(f"[belt] process exited {returncode}, expected 0 (clean "
                    f"LEFT return)", output)
    if not BELT_ALLOWED_RE.search(output):
        return fail("[belt] the legacy-engage seam never armed (vacuous run)",
                    output)
    if not BELT_PAUSED_REJECT_RE.search(output):
        return fail("[belt] the paused-sim replay rejection (the exact "
                    "pre-fix abort trigger) never fired", output)
    refused = BELT_REFUSED_RE.search(output)
    if not refused:
        return fail("[belt] the recoverable sim-state refusal never fired",
                    output)
    if not BELT_LEFT_RE.search(output):
        return fail("[belt] the refusal did not take the clean LEFT "
                    "return-to-room", output)
    shutdown = HOST_SHUTDOWN_RE.findall(output)
    if not shutdown or tuple(map(int, shutdown[-1])) != (0, 0, 0):
        return fail(f"[belt] host teardown leaked: "
                    f"{shutdown[-1] if shutdown else 'no witness'}", output)

    print(f"PASS online pause overlay: [storm] remote START at tick "
          f"{START_TICK} survived the correction storm ({storm_summary}); "
          f"[live] on-time remote START suppressed, no local overlay; "
          f"[leave] {leave_summary} -> clean LEFT, zero leaks; "
          f"[belt] legacy paused-replay refusal at tick "
          f"{refused.group(1)} routed recoverable -> clean LEFT, no abort")
    return 0


if __name__ == "__main__":
    sys.exit(main())
