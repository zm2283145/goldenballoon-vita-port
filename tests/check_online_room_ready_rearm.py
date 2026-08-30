#!/usr/bin/env python3
"""Safe 2nd-tournament room-ready RE-ARM state machine.

The native online screens take over a TOURNAMENT room via the one-shot room-ready
latch (OnlineRoom_pollRoomReadyTransition). That latch is set for the whole lifetime
of one adapter, so before this fix a SECOND tournament in the SAME session silently
fell back to the per-race ImGui path instead of the native takeover. This fix re-arms
the latch, reason-aware; with the tournament-final FINISH now dispatching the
REMATCH wrap (RESULTS -> LOBBY + fresh series) BEFORE the session returns, the
re-arm completes IMMEDIATELY on the panel's next observation and the re-take is
automatic:

  - a FINISHED native return ARMS a re-arm (OnlineRoom_armRoomReadyRearm);
    LEFT/ERROR/NONE do NOT arm;
  - the panel's per-frame observer (OnlineRoom_observeRoomReadyRearm) completes a
    pending arm in one observation -- the wrap already took the room out of the
    takeover window (RESULTS) and back to a fresh-series SELECTING during the
    session, which IS the rising edge -- so the very next poll re-takes native
    exactly once for session #2 on this endpoint (both real endpoints do the same
    on their own FINISHED returns);
  - a room transition ALONE (the wrap without a FINISHED return) never re-fires:
    the latch stays set until an arm consumes it -- one re-take per FINISHED.

The full interactive 2-tournament loop needs a live cloud adapter + a human, so this
lane drives the loopback tournament room through the wiring's re-arm edges DIRECTLY via
the MDKR_APP_TEST_ONLINE_ROOM_READY_REARM_PROBE seam (main_app.cpp). The probe reports
its own verdict; this harness asserts every sub-flag so a regression names itself:

  totalFires=3      -- exactly one native takeover per tournament + one for the
                       reset coda's fresh adapter (no re-boot loop, no spurious fire)
  t1Once=1          -- tournament #1 fired the takeover exactly once
  leftNoRearm=1     -- a LEFT/ERROR return (no arm) never re-fires even with the
                       condition still TRUE (SELECTING+2+LOBBY+tournament) -> no loop
  wrapAloneNoRefire=1 -- the production RESULTS-park -> FINISH-wrap round trip alone
                       (no FINISHED return yet) re-fires NOTHING: the latch is still
                       set and nothing is pending
  finishedRetakeOnce=1 routed2=1 -- the FINISHED arm completes on the next
                       observation and the takeover re-fires EXACTLY ONCE for
                       session #2 (route=lobby-start), with no further fires after
                       the consume -- the automatic FINISHED re-take
  noRetakeWithoutFinished=1 -- a later condition false->true cycle WITHOUT a
                       FINISHED return does not re-fire (one re-take per FINISHED)
  resetDropsPending=1 -- OnlineRoom_resetRoomReadyLatch drops a pending re-arm, so a
                       stale FINISHED cannot leak into a fresh adapter (no 4th fire)

The harness also asserts the "re-arm complete" wiring log appears exactly once (the
step-4 completion) -- a direct witness of the latch clearing, and that neither the
wrap-alone step nor the reset coda cleared a second time.

RED at the deferred-clear build: the observer waited for a condition-FALSE frame
that a FINISHED-with-wrap return never shows again (the wrapped room sits at
SELECTING+2+LOBBY forever), so the re-arm never completed and the re-take was
permanently stranded at the panel.

Registered in the tools/run_online_checks.py sweep (like the sibling online engine
lanes); also runnable standalone.
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

from harness_utils import resolve_binary
from online_lane_util import make_fail
from online_lane_util import run_engine as _run_engine

VERDICT_RE = re.compile(
    r"^\[online-room-ready-rearm-probe\] totalFires=(\d+) t1Once=(\d+) "
    r"leftNoRearm=(\d+) wrapAloneNoRefire=(\d+) finishedRetakeOnce=(\d+) "
    r"routed2=(\d+) noRetakeWithoutFinished=(\d+) resetDropsPending=(\d+) "
    r"verdict=(PASS|FAIL)$",
    re.MULTILINE)

# Direct witness that the pending arm actually completed (the step-4 observation)
# -- the wiring emits this the frame the observer consumes the arm.
REARM_COMPLETE_RE = re.compile(
    r"^\[online-room-ready\] re-arm complete ", re.MULTILINE)


fail = make_fail("room-ready re-arm")


def run_probe(binary: Path, rom: Path, timeout: int,
              verbose: bool) -> tuple[int, str]:
    # The full interactive 2-tournament loop is driven headless by the wiring probe
    # seam over the loopback tournament room.
    return _run_engine(
        binary, rom, ticks=1, timeout=timeout, verbose=verbose,
        extra_env={
            "MDKR_APP_TEST_ONLINE_ROOM_READY_REARM_PROBE": "1",
            "MDKR_APP_TEST_ONLINE_MODE": "tournament",
            "MDKR_APP_TEST_ONLINE_CUP": "1",
        }, prefix="mdkr64-rearm-")


def main() -> int:
    parser = argparse.ArgumentParser()
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
        return fail("the probe hung (re-arm state machine must be bounded, never "
                    "a loop)")

    if args.verbose:
        print(output)

    match = VERDICT_RE.search(output)
    if match is None:
        return fail(f"no probe verdict line (rc={rc}); the seam did not run")

    (total_fires, t1_once, left_no_rearm, wrap_alone_no_refire,
     finished_retake_once, routed2, no_retake_without_finished,
     reset_drops_pending, verdict) = match.groups()

    flags = {
        "t1Once": t1_once,
        "leftNoRearm": left_no_rearm,
        "wrapAloneNoRefire": wrap_alone_no_refire,
        "finishedRetakeOnce": finished_retake_once,
        "routed2": routed2,
        "noRetakeWithoutFinished": no_retake_without_finished,
        "resetDropsPending": reset_drops_pending,
    }
    bad = [name for name, value in flags.items() if value != "1"]
    # Direct log witness that the pending arm completed (Minor-1): the "re-arm
    # complete" line must appear -- and, in a correct run, exactly once (the step-4
    # completion; neither the wrap-alone step nor the reset coda may clear again).
    rearm_complete_count = len(REARM_COMPLETE_RE.findall(output))
    if verdict != "PASS" or rc != 0 or total_fires != "3" or bad or \
            rearm_complete_count != 1:
        return fail(
            f"verdict={verdict} rc={rc} totalFires={total_fires} "
            f"rearmCompleteLines={rearm_complete_count} (want 1) "
            f"failed_flags={bad or 'none'}")

    print(
        "PASS online room-ready re-arm: the 2nd-tournament re-arm state machine is "
        "safe and FINISHED-gated -- tournament #1 took over native EXACTLY ONCE; a "
        "LEFT/ERROR return did NOT re-arm even with the room-ready condition still "
        "TRUE (no re-boot loop); the production RESULTS-park -> FINISH-wrap round "
        "trip alone re-fired NOTHING; the FINISHED arm completed on the next "
        "observation and re-took native EXACTLY ONCE for session #2 "
        "(route=lobby-start, the automatic re-take); a later condition cycle "
        "without a FINISHED return did not re-fire; and resetRoomReadyLatch "
        "dropped a pending re-arm so a stale FINISHED cannot leak into a fresh "
        f"adapter (no spurious fire, totalFires={total_fires})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
