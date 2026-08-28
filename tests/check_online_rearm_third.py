#!/usr/bin/env python3
"""THREE consecutive tournaments: the room-ready re-arm is REPEATABLE.

The sibling check_online_room_ready_rearm.py proves the 2nd-tournament re-arm
across ONE FINISHED cycle (#1 -> #2) plus a FRESH-adapter reset coda. What it
CANNOT show is that the arm -> clear -> rising-edge cycle is repeatable on the
SAME adapter: a latent one-shot bug (a "re-armed once" static guard, a pending
flag never re-set, a latch that clears only the first time) would pass #1 -> #2
yet silently drop the 3rd tournament. The human test plan calls this out
explicitly ("Then a 3rd for good measure -- unexercised by any lane").

This lane drives the MDKR_APP_TEST_ONLINE_ROOM_READY_REARM3_PROBE seam
(main_app.cpp), which runs THREE consecutive tournaments through the wiring's real
re-arm edges (OnlineRoom_armRoomReadyRearm / observeRoomReadyRearm /
pollRoomReadyTransition) over the loopback tournament room, and asserts every
sub-flag so a regression names itself:

  totalFires=3          -- exactly one native takeover per tournament (#1,#2,#3),
                           no re-boot loop, no spurious/missing fire
  t1Once=1              -- tournament #1 fired the takeover exactly once
  leftNoRearm=1         -- a LEFT return WEDGED BETWEEN #1 and #2 (no arm) never
                           re-fires even with the room-ready condition STILL TRUE
                           -- the no-re-boot-loop invariant, held mid-run
  finishedNoInstant2=1  -- the FINISHED return before #2 armed but did NOT instantly
                           re-fire while the condition still held (latch waits for
                           the RESULTS park)
  t2Once=1              -- tournament #2 re-took native exactly once
  finishedNoInstant3=1  -- a SECOND FINISHED return on the SAME adapter armed
                           without an instant re-fire (repeatability of the guard)
  t3Once=1              -- tournament #3 re-took native exactly once (the gap this
                           lane closes)

It also asserts the wiring logs prove one re-arm PER FINISHED return, not once
ever: "re-arm armed (FINISHED return)" and "re-arm complete" each appear EXACTLY
TWICE (the #2 and #3 cycles; #1 needs no re-arm), and each "armed" precedes its
"complete".

Standalone lane, mirroring the sibling online engine lanes.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

from harness_utils import resolve_binary
from online_lane_util import make_fail
from online_lane_util import run_engine as _run_engine

VERDICT_RE = re.compile(
    r"^\[online-room-ready-rearm3-probe\] totalFires=(\d+) t1Once=(\d+) "
    r"leftNoRearm=(\d+) finishedNoInstant2=(\d+) t2Once=(\d+) "
    r"finishedNoInstant3=(\d+) t3Once=(\d+) verdict=(PASS|FAIL)$",
    re.MULTILINE)

REARM_ARMED_RE = re.compile(
    r"^\[online-room-ready\] re-arm armed \(FINISHED return\)", re.MULTILINE)
REARM_COMPLETE_RE = re.compile(
    r"^\[online-room-ready\] re-arm complete ", re.MULTILINE)


fail = make_fail("room-ready re-arm x3")


def run_probe(binary: Path, rom: Path, timeout: int,
              verbose: bool) -> tuple[int, str]:
    return _run_engine(
        binary, rom, ticks=1, timeout=timeout, verbose=verbose,
        extra_env={
            "MDKR_APP_TEST_ONLINE_ROOM_READY_REARM3_PROBE": "1",
            "MDKR_APP_TEST_ONLINE_MODE": "tournament",
            "MDKR_APP_TEST_ONLINE_CUP": "1",
        }, prefix="mdkr64-rearm3-")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", default="build-beta")
    parser.add_argument("--rom", type=Path, default="baserom.us.v80.z64")
    parser.add_argument("--timeout", type=int, default=300)
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    binary = Path(resolve_binary(args.build)).expanduser().resolve()
    if not binary.exists():
        return fail(f"missing binary {binary}")
    rom = args.rom.expanduser().resolve()
    if not rom.exists():
        return fail(f"missing ROM {rom}")

    try:
        rc, output = run_probe(binary, rom, args.timeout, args.verbose)
    except subprocess.TimeoutExpired:
        return fail("the probe hung (the re-arm state machine must be bounded across "
                    "all three tournaments, never a loop)")

    if args.verbose:
        print(output)

    match = VERDICT_RE.search(output)
    if match is None:
        return fail(f"no probe verdict line (rc={rc}); the REARM3 seam did not run")

    (total_fires, t1_once, left_no_rearm, finished_no_instant2, t2_once,
     finished_no_instant3, t3_once, verdict) = match.groups()

    flags = {
        "t1Once": t1_once,
        "leftNoRearm": left_no_rearm,
        "finishedNoInstant2": finished_no_instant2,
        "t2Once": t2_once,
        "finishedNoInstant3": finished_no_instant3,
        "t3Once": t3_once,
    }
    bad = [name for name, value in flags.items() if value != "1"]

    # One re-arm PER FINISHED return (not once ever): the #2 and #3 cycles each
    # arm + complete, so each log appears EXACTLY TWICE and in armed->complete order.
    armed = [m.start() for m in REARM_ARMED_RE.finditer(output)]
    complete = [m.start() for m in REARM_COMPLETE_RE.finditer(output)]
    ordered = (len(armed) == 2 and len(complete) == 2 and
               armed[0] < complete[0] < armed[1] < complete[1])

    if verdict != "PASS" or rc != 0 or total_fires != "3" or bad or not ordered:
        return fail(
            f"verdict={verdict} rc={rc} totalFires={total_fires} "
            f"rearmArmedLines={len(armed)} (want 2) "
            f"rearmCompleteLines={len(complete)} (want 2) "
            f"armedBeforeCompletePerCycle={ordered} "
            f"failed_flags={bad or 'none'}", output)

    print(
        "PASS online room-ready re-arm x3: the re-arm is REPEATABLE across THREE "
        "consecutive tournaments on the SAME adapter -- #1 took over native exactly "
        "once; a LEFT return wedged between #1 and #2 did NOT re-arm even with the "
        "room-ready condition still TRUE (no mid-run re-boot loop); each of the two "
        "FINISHED returns armed WITHOUT an instant re-fire and then re-took native "
        "exactly once on the fresh SELECTING rising edge (#2 and #3), for exactly "
        "three total takeovers; and the wiring logged 're-arm armed'/'re-arm "
        "complete' EXACTLY TWICE each in armed->complete order (one re-arm per "
        "FINISHED return, not a one-shot). This closes the 3rd-tournament gap the "
        "single-cycle probe could not reach.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
