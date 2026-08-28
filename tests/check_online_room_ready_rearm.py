#!/usr/bin/env python3
"""PD-T6e MINOR-4: safe 2nd-tournament room-ready RE-ARM state machine.

The native online screens take over a TOURNAMENT room via the one-shot room-ready
latch (OnlineRoom_pollRoomReadyTransition). That latch is set for the whole lifetime
of one adapter, so before this fix a SECOND tournament in the SAME session silently
fell back to the per-race ImGui path instead of the native takeover. Minor-4 re-arms
the latch, edge-triggered and reason-aware:

  - a FINISHED native return ARMS a re-arm (OnlineRoom_armRoomReadyRearm);
    LEFT/ERROR/NONE do NOT arm;
  - the panel's per-frame observer (OnlineRoom_observeRoomReadyRearm) clears the latch
    ONLY while the room-ready condition is FALSE (after FINISHED the reducer parks in
    RESULTS), so the next SELECTING+2+LOBBY+tournament arrival is a genuine false->true
    rising edge the trigger re-fires on -- exactly once, for tournament #2.

The full interactive 2-tournament loop needs a live cloud adapter + a human, so this
lane drives the loopback tournament room through the wiring's re-arm edges DIRECTLY via
the MDKR_APP_TEST_ONLINE_ROOM_READY_REARM_PROBE seam (main_app.cpp). The probe reports
its own verdict; this harness asserts every sub-flag so a regression names itself:

  totalFires=3      -- exactly one native takeover per tournament + one for the
                       reset coda's fresh adapter (no re-boot loop, no spurious fire)
  t1Once=1          -- tournament #1 fired the takeover exactly once
  leftNoRearm=1     -- a LEFT/ERROR return (no arm) never re-fires even with the
                       condition still TRUE (SELECTING+2+LOBBY+tournament) -> no loop
  finishedNoInstant=1 -- a FINISHED return (arm) does NOT instantly re-fire while the
                       condition still holds (the latch waits for the RESULTS park)
  clearedWhileFalse=1 -- the latch actually cleared (takeover-engaged flipped
                       false->true) ONLY while out of the takeover condition
  t2Once=1 routed2=1 -- tournament #2 re-takes native exactly once (route=lobby-start)
  resetDropsPending=1 -- OnlineRoom_resetRoomReadyLatch drops a pending re-arm, so a
                       stale FINISHED cannot leak into a fresh adapter (no 4th fire)

The harness also asserts the "re-arm complete" wiring log appears exactly once (the
step-4a clear) -- a direct witness of the latch clearing, and that the reset coda did
NOT clear a second time.

Standalone lane (not run-checks registered), mirroring the sibling online engine lanes.
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

VERDICT_RE = re.compile(
    r"^\[online-room-ready-rearm-probe\] totalFires=(\d+) t1Once=(\d+) "
    r"leftNoRearm=(\d+) finishedNoInstant=(\d+) clearedWhileFalse=(\d+) "
    r"t2Once=(\d+) routed2=(\d+) resetDropsPending=(\d+) verdict=(PASS|FAIL)$",
    re.MULTILINE)

# Direct witness that the latch actually cleared during the condition-false window
# (step 4a) -- the wiring emits this the frame the observer drops the latch.
REARM_COMPLETE_RE = re.compile(
    r"^\[online-room-ready\] re-arm complete ", re.MULTILINE)


def clean_environment(**updates: str) -> dict[str, str]:
    environment = {
        key: value for key, value in os.environ.items()
        if not key.startswith(("MDKR", "GE007_"))
    }
    environment.update(updates)
    return environment


def run_probe(binary: Path, rom: Path, timeout: int,
              verbose: bool) -> tuple[int, str]:
    with tempfile.TemporaryDirectory(prefix="mdkr64-rearm-") as temp:
        run_dir = Path(temp)
        (run_dir / "saves").mkdir()
        (run_dir / "preferences").mkdir()
        environment = clean_environment(
            LC_ALL="C",
            MDKR_APP_AUTOPLAY="1",
            MDKR_APP_AUTOPLAY_TICKS="1",
            MDKR_APP_PREFS_DIR=str(run_dir / "preferences"),
            MDKR_AUDIO="0",
            MDKR_AUTOPILOT="1",
            MDKR_NO_CRASH_HANDLER="1",
            MDKR_PRESENT_RATE="original",
            MDKR_RENDERER="gl",
            MDKR_ROM=str(rom),
            MDKR_SAVE_DIR=str(run_dir / "saves"),
            MDKR_STATE_HASH="3",
            MDKR_TEST_SCRIPT_ONLY_INPUT="1",
            MDKR_VIDEO_CONFIG_PATH=str(run_dir / "video.ini"),
            MDKR64_HIDDEN="1",
            MDKR_APP_TEST_ONLINE_ROOM_READY_REARM_PROBE="1",
            MDKR_APP_TEST_ONLINE_MODE="tournament",
            MDKR_APP_TEST_ONLINE_CUP="1",
        )
        if verbose:
            print(f"$ MDKR_APP_TEST_ONLINE_ROOM_READY_REARM_PROBE=1 {binary}",
                  flush=True)
        process = subprocess.run(
            [str(binary)], cwd=run_dir, env=environment, text=True,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            timeout=timeout, check=False,
        )
        return process.returncode, (process.stdout or "")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", default="build-beta")
    parser.add_argument("--rom", type=Path, default="baserom.us.v80.z64")
    parser.add_argument("--timeout", type=int, default=300)
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    binary = Path(resolve_binary(args.build)).expanduser().resolve()
    if not binary.exists():
        print(f"FAIL online room-ready re-arm: missing binary {binary}")
        return 1
    rom = args.rom.expanduser().resolve()
    if not rom.exists():
        print(f"FAIL online room-ready re-arm: missing ROM {rom}")
        return 1

    try:
        rc, output = run_probe(binary, rom, args.timeout, args.verbose)
    except subprocess.TimeoutExpired:
        print("FAIL online room-ready re-arm: the probe hung "
              "(re-arm state machine must be bounded, never a loop)")
        return 1

    if args.verbose:
        print(output)

    match = VERDICT_RE.search(output)
    if match is None:
        print("FAIL online room-ready re-arm: no probe verdict line "
              f"(rc={rc}); the seam did not run")
        return 1

    (total_fires, t1_once, left_no_rearm, finished_no_instant,
     cleared_while_false, t2_once, routed2, reset_drops_pending,
     verdict) = match.groups()

    flags = {
        "t1Once": t1_once,
        "leftNoRearm": left_no_rearm,
        "finishedNoInstant": finished_no_instant,
        "clearedWhileFalse": cleared_while_false,
        "t2Once": t2_once,
        "routed2": routed2,
        "resetDropsPending": reset_drops_pending,
    }
    bad = [name for name, value in flags.items() if value != "1"]
    # Direct log witness that the latch cleared during the condition-false window
    # (Minor-1): the "re-arm complete" line must appear -- and, in a correct run,
    # exactly once (the step-4a clear; the reset coda must NOT clear again).
    rearm_complete_count = len(REARM_COMPLETE_RE.findall(output))
    if verdict != "PASS" or rc != 0 or total_fires != "3" or bad or \
            rearm_complete_count != 1:
        print("FAIL online room-ready re-arm: "
              f"verdict={verdict} rc={rc} totalFires={total_fires} "
              f"rearmCompleteLines={rearm_complete_count} (want 1) "
              f"failed_flags={bad or 'none'}")
        return 1

    print(
        "PASS online room-ready re-arm: the 2nd-tournament re-arm state machine is "
        "safe and edge-triggered -- tournament #1 took over native EXACTLY ONCE; a "
        "LEFT/ERROR return did NOT re-arm even with the room-ready condition still "
        "TRUE (no re-boot loop); a FINISHED return armed but did NOT instantly "
        "re-fire while the condition held; the latch cleared ONLY once the room left "
        "the takeover window; and the fresh SELECTING+2+LOBBY+tournament rising edge "
        "re-took native EXACTLY ONCE for tournament #2 (route=lobby-start); and "
        "resetRoomReadyLatch dropped a pending re-arm so a stale FINISHED cannot leak "
        f"into a fresh adapter (no spurious fire, totalFires={total_fires})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
