#!/usr/bin/env python3
"""P0 CRASH REGRESSION: a peer that vanishes returns to the room, never abort().

A real two-machine host+joiner run had BOTH endpoints abort() (hard crash) when
the WebRTC peer dropped. This lane covers the WHOLE peer-loss crash class -- two
distinct engine abort sites that a clean peer drop would reach:

  A. RACE START (validate_boundary): the launcher's race-start barrier aborts the
     tick-1 input drain ("[START] ... aborting to the room"); the already-booted
     engine reaches its first authored boundary and rollback validate_boundary()
     reports "[ROLLBACK] online bootstrap input unavailable tick=1". thread3_main
     used to take that as a lost-authority INVARIANT violation and abort().

  B. MID RACE (prepare_tick): a peer/console that drops cleanly AFTER tick 1 (the
     real "console drops mid-race" case) leaves the launcher input provider unable
     to supply an authored tick, so prepare_tick reports "[ROLLBACK] launcher input
     provider rejected tick=N" -> thread3_main used to abort() with "[FATAL]
     rollback lab could not prepare canonical input".

Both are RECOVERABLE peer/input starvations, not rollback invariant corruption.
This lane had ZERO coverage before the fix.

Reproduction (headless, no menu-nav script): the descriptor-less lobby-start
loopback session (the SAME path a real 2-machine game uses -- native
CHARSELECT/TRACKSELECT own race 1) boots the race, then a peer-loss seam refuses a
target authored tick's remote input (tick 1 for A via
MDKR_APP_TEST_ONLINE_DROP_RACE_START_INPUT; tick 50 for B via
MDKR_APP_TEST_ONLINE_DROP_INPUT_AT_TICK, both in liveDrainMatchInput).

Post-fix assertions, for BOTH scenarios (this is the whole point):
  * the recoverable trigger genuinely fired (non-vacuous): the exact [ROLLBACK]
    starvation line for that site (tick=1 bootstrap / tick=50 launcher-rejected)
  * the engine routed to a CLEAN return-to-room, NOT abort:
        [online-session] LEFT: online peer/input lost ... exit 0   (engine)
        [online-session-end] reason=LEFT result=0                  (launcher)
  * clean teardown, ZERO leaked allocations: [HOST-SHUTDOWN] rom=0 arena=0 delayedFree=0
  * NO "[FATAL]" and NO SIGABRT/Abort trap -- the process exits cleanly (rc 0)

Both ROLES take the identical engine path: the host (peer=joiner lost) and the
joiner (peer=host lost/crashed) both starve the same boundary.

PRE-FIX PROOF: with the thread3_main graceful branch reverted, these SAME
scenarios abort (SIGABRT) -- A: "[FATAL] rollback lab lost a registered authority
allocation"; B: "[FATAL] rollback lab could not prepare canonical input".
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
MID_RACE_TICK = 50  # a "console drops mid-race" drop, well past the opening tick

BEGIN_LOBBY_RE = re.compile(
    r"^\[online-session\] begin: lobby-start \(no descriptor\)", re.MULTILINE)
# The engine-side clean return-to-room witness (the fix), shared by both sites.
GRACEFUL_LEFT_RE = re.compile(
    r"^\[online-session\] LEFT: online peer/input lost", re.MULTILINE)
# Clean teardown / zero-leak witness.
HOST_SHUTDOWN_RE = re.compile(
    r"^\[HOST-SHUTDOWN\] rom=(\d+) arena=(\d+) delayedFree=(\d+)", re.MULTILINE)

fail = make_fail("peer loss")

# The mid-race recoverable TRIGGER line ("[ROLLBACK] launcher input provider
# rejected tick=N") is itself a FORBIDDEN_ONLINE marker -- because pre-fix it was a
# hard rejection that abort()ed. Post-fix it is the EXPECTED recoverable trigger the
# engine recovers from, so the mid-race scenario must not treat it as fatal. The
# other genuinely-fatal FORBIDDEN_ONLINE markers (admission reject, startup reject)
# stay forbidden; a real crash still surfaces via find_fatal ([FATAL]) + ABORT.
MIDRACE_FORBIDDEN = tuple(
    m for m in FORBIDDEN_ONLINE if m != "launcher input provider rejected")


def run_scenario(binary: Path, rom: Path, args, *, label: str,
                 seam_env: dict[str, str], seam_re: re.Pattern[str],
                 trigger_re: re.Pattern[str], trigger_desc: str,
                 forbidden: tuple[str, ...]) -> str | None:
    """Run one peer-loss scenario; return None on success or a failure message."""
    extra_env = {
        # descriptor-less lobby-start loopback (native owns race 1) -- the real
        # 2-machine crash path, and the sole path that prints [online-session-end].
        "MDKR_APP_TEST_ONLINE_LIVE_LOBBY_START": "1",
        "MDKR_TEST_ONLINE_LOBBY_START": "1",
    }
    extra_env.update(seam_env)
    try:
        returncode, output = run_engine(
            binary, rom, ticks=args.ticks, timeout=args.timeout,
            verbose=args.verbose, extra_env=extra_env,
            prefix=f"mdkr64-online-peer-loss-{label}-")
    except subprocess.TimeoutExpired as error:
        return (f"[{label}] engine run timed out (a hang instead of a clean "
                f"return would look like this): {error}")

    def bad(message: str) -> str:
        if output:
            print(output[-16000:], file=sys.stderr)
        return f"[{label}] {message}"

    # --- NO abort / NO fatal (the crash we are fixing) ----------------------
    marker = forbidden_marker(output, *forbidden, *ABORT_MARKERS)
    if marker:
        return bad(f"observed a fatal/abort marker {marker!r} -- the peer loss "
                   f"still crashes instead of returning to the room")
    if returncode < 0:
        return bad(f"process was killed by signal {-returncode} (SIGABRT=6) -- "
                   f"the engine abort()ed on the recoverable peer loss")
    if returncode != 0:
        return bad(f"process exited {returncode}, expected a clean 0 "
                   f"(return-to-room)")

    # --- The session actually reached the race (non-vacuous) ----------------
    if len(BEGIN_LOBBY_RE.findall(output)) != 1:
        return bad("the session did not begin DESCRIPTOR-LESS (lobby-start) "
                   "exactly once")
    if not DIRECT_BOOT_RE.search(output):
        return bad("the race was never booted (no [online-boot] direct race)")
    if not ONLINE_RACE_RE.search(output):
        return bad("the engine never entered the ONLINE rollback race before the "
                   "peer-loss drop")
    if not seam_re.search(output):
        return bad("the peer-loss seam never fired (the remote input was never "
                   "dropped)")

    # --- The RECOVERABLE trigger genuinely fired (the exact crash cause) -----
    if not trigger_re.search(output):
        return bad(f"the recoverable trigger ({trigger_desc}) never fired -- this "
                   f"scenario would be vacuous without it")

    # --- The clean return-to-room (the fix) ---------------------------------
    if not GRACEFUL_LEFT_RE.search(output):
        return bad("the engine did not route the recoverable peer loss to a clean "
                   "return-to-room ([online-session] LEFT: online peer/input lost)")
    end = SESSION_END_RE.findall(output)
    if not end:
        return bad("the launcher never observed the [online-session-end] witness")
    reason, result = end[-1]
    if reason != "LEFT":
        return bad(f"session-end reason={reason}, expected LEFT")
    if int(result) != 0:
        return bad(f"session-end result={result}, expected 0")

    # --- Clean teardown: zero leaked allocations ----------------------------
    shutdown = HOST_SHUTDOWN_RE.findall(output)
    if not shutdown:
        return bad("no [HOST-SHUTDOWN] witness -- cannot prove a clean teardown")
    rom_leak, arena_leak, delayed = shutdown[-1]
    if (int(rom_leak), int(arena_leak), int(delayed)) != (0, 0, 0):
        return bad(f"host teardown leaked allocations: rom={rom_leak} "
                   f"arena={arena_leak} delayedFree={delayed} (expected all 0)")

    print(f"  [{label}] PASS: {trigger_desc} -> clean return (reason={reason} "
          f"result={result}, rc 0, no [FATAL]/SIGABRT, HOST-SHUTDOWN "
          f"rom={rom_leak} arena={arena_leak} delayedFree={delayed})")
    return None


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

    scenarios = (
        dict(
            label="race-start",
            seam_env={"MDKR_APP_TEST_ONLINE_DROP_RACE_START_INPUT": "1"},
            seam_re=re.compile(
                r"^\[online-live\] TEST: race-start tick-\d+ remote input "
                r"UNAVAILABLE", re.MULTILINE),
            trigger_re=re.compile(
                r"^\[ROLLBACK\] online bootstrap input unavailable tick=1$",
                re.MULTILINE),
            trigger_desc="tick-1 bootstrap input unavailable (validate_boundary)",
            forbidden=FORBIDDEN_ONLINE,
        ),
        dict(
            label="mid-race",
            seam_env={"MDKR_APP_TEST_ONLINE_DROP_INPUT_AT_TICK": str(MID_RACE_TICK)},
            seam_re=re.compile(
                r"^\[online-live\] TEST: mid-race tick-\d+ remote input "
                r"UNAVAILABLE", re.MULTILINE),
            trigger_re=re.compile(
                r"^\[ROLLBACK\] launcher input provider rejected tick=%d$"
                % MID_RACE_TICK, re.MULTILINE),
            trigger_desc=(f"tick-{MID_RACE_TICK} launcher input provider rejected "
                          f"(prepare_tick)"),
            forbidden=MIDRACE_FORBIDDEN,
        ),
    )

    for scenario in scenarios:
        problem = run_scenario(binary, rom, args, **scenario)
        if problem is not None:
            return fail(problem)

    print(
        "PASS online peer loss: BOTH a race-start peer loss (tick-1 bootstrap "
        "starvation, validate_boundary) AND a mid-race peer loss (tick-"
        f"{MID_RACE_TICK} launcher-input-rejected, prepare_tick) routed to a CLEAN "
        "return-to-room (engine [online-session] LEFT + platform exit 0, launcher "
        "[online-session-end] reason=LEFT result=0, HOST-SHUTDOWN zero leaks) "
        "instead of abort() -- no [FATAL], no SIGABRT, process exited 0. Both roles "
        "take these identical boundary-starvation paths."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
