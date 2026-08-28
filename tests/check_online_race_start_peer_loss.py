#!/usr/bin/env python3
"""P0 CRASH REGRESSION: a peer that vanishes at RACE START returns to the room.

A real two-machine host+joiner run had BOTH endpoints abort() (hard crash) when
the WebRTC peer dropped at race start: the launcher's race-start barrier aborted
the tick-1 input drain ("[START] race-start barrier: ... aborting to the room"),
but the already-booted engine reached its first authored boundary and
rollback_game_runtime.c's validate_boundary() reported the RECOVERABLE
"[ROLLBACK] online bootstrap input unavailable tick=1" starvation. thread3_main.c
then took that false return as a lost-authority INVARIANT violation and abort()ed
the whole app, instead of returning cleanly to the Online Room. This lane had ZERO
coverage before the fix.

Reproduction (headless, no menu-nav script): the descriptor-less lobby-start
loopback session (the SAME path a real 2-machine game uses -- native
CHARSELECT/TRACKSELECT own race 1) boots the race, then the race-start peer-loss
seam (MDKR_APP_TEST_ONLINE_DROP_RACE_START_INPUT, in liveDrainMatchInput) refuses
the FIRST authored tick's remote input -- exactly what the production barrier does
on a real ICE-failed peer. The engine's tick-1 boundary then starves.

Post-fix assertions (this is the whole point):
  * the recoverable trigger genuinely fired
        [ROLLBACK] online bootstrap input unavailable tick=1   (non-vacuous)
  * the engine routed to a CLEAN return-to-room, NOT abort:
        [online-session] LEFT: peer lost at race start ... exit 0   (engine)
        [online-session-end] reason=LEFT result=0                   (launcher)
  * NO "[FATAL]" and NO SIGABRT/Abort trap -- the process exits cleanly (rc 0)

Both ROLES take this identical engine path: the host (peer=joiner lost) and the
joiner (peer=host lost/crashed) both reach validate_boundary's tick-1 starvation,
so proving the engine-side clean return proves it for both.

PRE-FIX PROOF: with the thread3_main graceful branch reverted, this SAME lane
aborts (SIGABRT, "[FATAL] rollback lab lost a registered authority allocation").
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

from harness_utils import ABORT_MARKERS, resolve_binary
from online_lane_util import (
    DIRECT_BOOT_RE, FORBIDDEN_ONLINE, ONLINE_RACE_RE, SESSION_END_RE,
    forbidden_marker, make_fail, run_engine,
)

ROOT = Path(__file__).resolve().parent.parent
TICKS = 20000

BEGIN_LOBBY_RE = re.compile(
    r"^\[online-session\] begin: lobby-start \(no descriptor\)", re.MULTILINE)
# The exact RECOVERABLE starvation the P0 crash mis-classified as fatal.
BOOTSTRAP_UNAVAIL_RE = re.compile(
    r"^\[ROLLBACK\] online bootstrap input unavailable tick=1$", re.MULTILINE)
# The engine-side clean return-to-room witness (the fix).
GRACEFUL_LEFT_RE = re.compile(
    r"^\[online-session\] LEFT: peer lost at race start", re.MULTILINE)
# The test seam actually fired (drop of the race-start remote input).
SEAM_RE = re.compile(
    r"^\[online-live\] TEST: race-start tick-\d+ remote input UNAVAILABLE",
    re.MULTILINE)

fail = make_fail("race-start peer loss")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", default="build-beta")
    parser.add_argument("--rom", type=Path, default="baserom.us.v80.z64")
    parser.add_argument("--ticks", type=int, default=TICKS)
    parser.add_argument("--timeout", type=int, default=360)
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    binary = Path(resolve_binary(args.build)).expanduser().resolve()
    rom = args.rom.expanduser().resolve()
    for path, label in ((binary, "binary"), (rom, "ROM")):
        if not path.is_file():
            parser.error(f"missing {label}: {path}")

    extra_env = {
        # descriptor-less lobby-start loopback (native owns race 1) -- the real
        # 2-machine crash path, and the sole path that prints [online-session-end].
        "MDKR_APP_TEST_ONLINE_LIVE_LOBBY_START": "1",
        "MDKR_TEST_ONLINE_LOBBY_START": "1",
        # the peer-loss seam: refuse the FIRST authored tick's remote input.
        "MDKR_APP_TEST_ONLINE_DROP_RACE_START_INPUT": "1",
    }
    try:
        returncode, output = run_engine(
            binary, rom, ticks=args.ticks, timeout=args.timeout,
            verbose=args.verbose, extra_env=extra_env,
            prefix="mdkr64-online-race-start-peer-loss-")
    except subprocess.TimeoutExpired as error:
        return fail(f"engine run timed out (a hang instead of a clean return "
                    f"would look like this): {error}")

    # --- NO abort / NO fatal (the crash we are fixing) ----------------------
    # find_fatal catches [FATAL]/[CRASH]/sanitizers; ABORT_MARKERS adds the
    # SIGABRT/Abort-trap text a crash handler / the shell would print.
    marker = forbidden_marker(output, *FORBIDDEN_ONLINE, *ABORT_MARKERS)
    if marker:
        return fail(f"observed a fatal/abort marker {marker!r} -- the peer-loss "
                    f"at race start still crashes instead of returning to the "
                    f"room", output)
    # A bare SIGABRT (no crash handler) surfaces only as a negative return code.
    if returncode < 0:
        return fail(f"process was killed by signal {-returncode} (SIGABRT=6) -- "
                    f"the engine abort()ed on the recoverable peer loss", output)
    if returncode != 0:
        return fail(f"process exited {returncode}, expected a clean 0 "
                    f"(return-to-room)", output)

    # --- The session actually reached the race (non-vacuous) ----------------
    if len(BEGIN_LOBBY_RE.findall(output)) != 1:
        return fail("the session did not begin DESCRIPTOR-LESS (lobby-start) "
                    "exactly once", output)
    if not DIRECT_BOOT_RE.search(output):
        return fail("the race was never booted (no [online-boot] direct race) -- "
                    "the drop must happen AT a real race start, not before it",
                    output)
    if not ONLINE_RACE_RE.search(output):
        return fail("the engine never entered the ONLINE rollback race before the "
                    "peer-loss drop", output)
    if not SEAM_RE.search(output):
        return fail("the race-start peer-loss seam never fired (the remote input "
                    "was never dropped)", output)

    # --- The RECOVERABLE trigger genuinely fired (the exact crash cause) -----
    if not BOOTSTRAP_UNAVAIL_RE.search(output):
        return fail("the tick-1 bootstrap-input-unavailable starvation "
                    "([ROLLBACK] online bootstrap input unavailable tick=1) never "
                    "fired -- this lane would be vacuous without it", output)

    # --- The clean return-to-room (the fix) ---------------------------------
    if not GRACEFUL_LEFT_RE.search(output):
        return fail("the engine did not route the recoverable peer loss to a "
                    "clean return-to-room ([online-session] LEFT: peer lost at "
                    "race start)", output)
    end = SESSION_END_RE.findall(output)
    if not end:
        return fail("the launcher never observed the [online-session-end] "
                    "witness (the engine->launcher clean-return handshake)",
                    output)
    reason, result = end[-1]
    if reason != "LEFT":
        return fail(f"session-end reason={reason}, expected LEFT (the peer left "
                    f"at race start)", output)
    if int(result) != 0:
        return fail(f"session-end result={result}, expected 0 (a clean return, "
                    f"not an error exit)", output)

    print(
        "PASS online race-start peer loss: a peer that VANISHED at race start "
        "(tick-1 bootstrap input unavailable -- the P0 crash trigger) routed to a "
        "CLEAN return-to-room (engine [online-session] LEFT + platform exit 0, "
        f"launcher [online-session-end] reason={reason} result={result}) instead "
        "of abort() -- no [FATAL], no SIGABRT, process exited 0. Both roles take "
        "this identical validate_boundary tick-1 path."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
