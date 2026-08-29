#!/usr/bin/env python3
"""LEFT/ERROR native re-entry: the "Return to game" control re-takes native.

Sibling of the room-ready re-arm lanes. Those prove the FINISHED auto re-arm and
that a LEFT/ERROR return must NOT auto re-arm (the room lands back at
SELECTING+2+LOBBY with the takeover condition still TRUE, so an automatic re-arm is
the proven re-boot-loop hazard). With the per-race ImGui fallback retired, that
no-re-boot state would be a dead end -- so the SELECTING body offers the player an
explicit "Return to game" control, and pressing it re-arms.

The full interactive path needs a live cloud adapter + a human, so this lane drives
the loopback room through the wiring's re-entry edges DIRECTLY via the
MDKR_APP_TEST_ONLINE_LEFT_REENTRY_PROBE seam (main_app.cpp). The probe reports its
own verdict; this harness asserts every sub-flag so a regression names itself:

  totalFires=2          -- the native takeover fired ONCE for the first session and
                           EXACTLY ONCE more after the scripted re-entry press (no
                           spurious extra fire)
  controlOffered=1      -- (a) a LEFT return records a re-entry reason while the
                           takeover is NOT engaged and the room-ready condition still
                           holds -- the exact gate under which the card draws the
                           control
  noRebootWithoutPress=1 -- (b) WITHOUT a press the takeover does NOT re-fire within
                           the frame budget, even with the per-frame re-arm observer
                           running (LEFT/ERROR never arms)
  reentryRefires=1 routed=1 -- (c) the scripted press re-fired the takeover EXACTLY
                           ONCE and re-published for the descriptor-less native boot
                           (route=lobby-start)

It also asserts the direct wiring witnesses: the "[online-room-ready] latch set"
line appears EXACTLY TWICE (once for the first takeover, once after the re-entry
press -- the takeover re-fired), and the "[online-room-ready] re-entry requested"
line appears exactly once (the press cleared the latch).

Run-checks registered (tools/run_online_checks.py), mirroring the sibling online lanes.
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
    r"^\[online-left-reentry-probe\] totalFires=(\d+) controlOffered=(\d+) "
    r"noRebootWithoutPress=(\d+) reentryRefires=(\d+) routed=(\d+) "
    r"verdict=(PASS|FAIL)$",
    re.MULTILINE)

# Direct witness that the native takeover latched (fired). It must appear TWICE: the
# first session's takeover, then again after the scripted re-entry press.
LATCH_SET_RE = re.compile(r"^\[online-room-ready\] latch set ", re.MULTILINE)
# Direct witness of the human re-entry gesture clearing the latch.
REENTRY_REQUESTED_RE = re.compile(
    r"^\[online-room-ready\] re-entry requested ", re.MULTILINE)


fail = make_fail("left re-entry")


def run_probe(binary: Path, rom: Path, timeout: int,
              verbose: bool) -> tuple[int, str]:
    return _run_engine(
        binary, rom, ticks=1, timeout=timeout, verbose=verbose,
        extra_env={
            "MDKR_APP_TEST_ONLINE_LEFT_REENTRY_PROBE": "1",
            "MDKR_APP_TEST_ONLINE_MODE": "tournament",
            "MDKR_APP_TEST_ONLINE_CUP": "1",
        }, prefix="mdkr64-left-reentry-")


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
        return fail("the probe hung (re-entry must be a single bounded re-take, "
                    "never a loop)")

    if args.verbose:
        print(output)

    match = VERDICT_RE.search(output)
    if match is None:
        return fail(f"no probe verdict line (rc={rc}); the seam did not run")

    (total_fires, control_offered, no_reboot, reentry_refires, routed,
     verdict) = match.groups()

    flags = {
        "controlOffered": control_offered,
        "noRebootWithoutPress": no_reboot,
        "reentryRefires": reentry_refires,
        "routed": routed,
    }
    bad = [name for name, value in flags.items() if value != "1"]
    latch_set_count = len(LATCH_SET_RE.findall(output))
    reentry_requested_count = len(REENTRY_REQUESTED_RE.findall(output))
    if verdict != "PASS" or rc != 0 or total_fires != "2" or bad or \
            latch_set_count != 2 or reentry_requested_count != 1:
        return fail(
            f"verdict={verdict} rc={rc} totalFires={total_fires} "
            f"latchSetLines={latch_set_count} (want 2) "
            f"reentryRequestedLines={reentry_requested_count} (want 1) "
            f"failed_flags={bad or 'none'}")

    print(
        "PASS online left re-entry: after a LEFT native return the SELECTING body "
        "offers a 'Return to game' control (takeover NOT engaged, condition still "
        "TRUE); WITHOUT a press the takeover did NOT re-fire even with the re-arm "
        "observer running (no re-boot loop); the scripted press re-took native "
        f"EXACTLY ONCE (route=lobby-start, totalFires={total_fires}, latch set "
        "twice)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
