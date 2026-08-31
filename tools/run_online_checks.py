#!/usr/bin/env python3
"""Aggregating regression gate for the NATIVE ONLINE TAKEOVER (Golden Balloon beta).

None of the native-takeover engine lanes (tests/check_online_*.py) are in ctest or
tools/run_checks.py -- the "ctest N/N" figure EXCLUDES the entire feature (tests
review I-4). Whoever runs the exit gate previously had to run all of them by hand,
in the right order, one at a time. This is the one command that does it, plus the
ISOLATION guard (a fresh OFF build with zero online symbols).

WHY SERIAL (do not "optimise" this into a pool): these lanes boot the real engine
off the ROM and are TIMING-SENSITIVE -- several drive wall-clock watchdogs, live
loopback transport convergence, and countdown dwells that only behave when a lane
has the machine to itself. run_checks.py already keeps engine/GPU/wall-clock lanes
serial for the same reason; this runner runs EVERY online lane strictly one at a
time. It is a gate, not a speed contest.

Usage:
  python3 tools/run_online_checks.py                 # all lanes + isolation guard
  python3 tools/run_online_checks.py --no-isolation  # lanes only (faster)
  python3 tools/run_online_checks.py --build build-beta --rom baserom.us.v80.z64
  python3 tools/run_online_checks.py --pal-rom pal.v80.z64  # + cross-region lanes

--pal-rom is strictly additive: without it the sweep below is byte-identical to
its historical shape; with it the cross-region PAL lanes join the serial
schedule after the default lanes (see pal_lanes()).
"""

from __future__ import annotations

import argparse
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TESTS = ROOT / "tests"

# The full native-takeover lane set: the 13 exit-gate lanes + the C1 no-seam proof,
# plus coverage-fill lanes -- the isolation-guard self-test (proves the OFF
# isolation gate is non-vacuous), the 3-consecutive-tournament re-arm lane (proves
# the re-arm is repeatable, not one-shot), the LEFT/ERROR re-entry lane (proves a
# mid-session drop is not a dead end -- "Return to game" re-takes native, press-gated,
# with no auto re-arm loop), and the T3 beta hand-off render-seam lane (proves the
# launcher retired its per-race SELECTING widgets for BOTH modes and shows the
# re-entry card after a LEFT/ERROR return -- no engine boot), and the four-process
# US convergence arm (four isolated endpoint processes, including the no-render
# verifier, must produce byte-identical authority/input/canonical-event streams;
# the pal/cross arms of the same gate join via --pal-rom).
# Order is deliberate: the fast structural/meta checks first, the long multi-race
# loopback soaks last, so a quick break surfaces early.
LANES = (
    "check_online_isolation_selftest.py",   # meta: the isolation guard is non-vacuous
    "check_online_beta_handoff.py",         # T3: launcher SELECTING widgets retired ->
                                            #  universal native hand-off card (no engine)
    "check_online_engine_boot_direct.py",   # golden race hash 7da2ea67 pinned
    "check_online_session_boot.py",
    "check_online_charselect.py",
    "check_online_vehicleselect.py",
    "check_online_trackselect.py",
    "check_online_session_results.py",
    "check_online_results_chooser.py",    # T4: native "more races" RESULTS chooser
    "check_online_single_race_replay.py", # T5: single-race replay re-cycle (2 races
                                          #  back-to-back in-session + no re-boot
                                          #  loop) + the reducer-observable
                                          #  single-race FINISH wrap
    "check_online_resident_live.py",
    "check_online_session_end.py",          # incl. joiner-follow seam scenario
    "check_online_race_start_peer_loss.py", # P0: peer-loss at race start -> clean
                                            #  return-to-room, never abort()
    "check_online_midrace_transport_loss.py", # mid-race TRANSPORT sever (frozen
                                            #  pump + presence drop, the real
                                            #  kill signature) -> bounded typed
                                            #  peer LOST -> latch -> OPPONENT_LEFT
    "check_online_pause_overlay.py",        # START mid-race online: non-blocking
                                            #  overlay (sim never stops; remote
                                            #  START opens nothing; LEAVE -> clean
                                            #  LEFT) + the paused-replay belt
                                            #  (refuse recoverable, never abort)
    "check_online_joiner_terminal.py",      # exit-gate C1 no-seam joiner proof
    "check_online_ceremony.py",
    "check_online_lobby_start.py",
    "check_online_lobby_unconfigured.py",  # PRODUCTION room shape: no pre-config
                                           #  + latency room channel (the two-peer
                                           #  cloud CHARSELECT-wedge regression gate)
    "check_online_lobby_single_endpoint.py",
    "check_online_camera_capture_continuity.py",  # correction ticks keep their
                                           #  captured cameras (authored-camera
                                           #  latch survives the restore reset)
    "check_online_room_ready_rearm.py",
    "check_online_rearm_third.py",          # 3 consecutive tournaments: re-arm repeatable
    "check_online_left_reentry.py",         # LEFT/ERROR return: "Return to game" re-takes
                                            #  native (no auto re-arm; press-gated re-take)
    "check_online_process_convergence.py",  # four real endpoint processes, US
                                            #  arm (default --regions us):
                                            #  slot0/slot1/2-local/no-render
                                            #  must converge byte-identically;
                                            #  guards the no-render/US defect
                                            #  class the pal/cross arms (joined
                                            #  by --pal-rom) share
    "check_online_lobby_tournament.py",
    "check_online_tournament_cup_vehicle.py",  # mixed-vehicle cup (0) boots all 4
                                            #  rounds: the pick auto-narrows to the
                                            #  cup intersection so round-3 Hot Top
                                            #  Volcano never rejects with ILLEGAL_VEHICLE
    "check_online_final_replay.py",         # RULED final-replay: host continues via
                                            #  the wrap; both endpoints re-converge
                                            #  through the reducer into tournament #2
    "check_online_tournament.py",           # keystone: trophy-weight-rule accrual
)                                           #  + peer==peer race-hash convergence (the
#                                            GOLDEN literal is pinned only by check_online_engine_boot_direct.py)

# NON-DEFAULT / MANUAL: real-cloud network lanes. These are NOT part of the
# default sweep -- they require live Wi-Fi + the deployed party.goldenballoon.net
# service, spawn TWO app processes, and must run standalone (never two cloud runs
# concurrent). Run each by hand from tools/online/ when qualifying the real cloud:
#
#   python3 tools/online/check_online_native_flow_cloud.py --build build-beta \
#       --rom baserom.us.v80.z64 --through e --tournament 1   # GREEN achievable bar
#   python3 tools/online/check_online_native_flow_cloud.py --build build-beta \
#       --rom baserom.us.v80.z64 --through full --tournament 1 # full 7-assertion run
#   python3 tools/online/cloud_two_process_engine_boot.py --build build-beta \
#       --rom baserom.us.v80.z64            # the legacy descriptor-first cloud boot
#
# check_online_native_flow_cloud.py is the production-path acceptance instrument:
# the ONLY automation is pairing bootstrap + injected pad input; everything after
# pairing is the production code path (self-firing takeover, native screens,
# reducer-synced selections, race, chooser, return, re-take). `--through full` is
# the GREEN bar: all seven assertions, including the tournament-final (f) both-
# endpoint FINISHED (the host's FINISH dispatches the REMATCH wrap so the joiner's
# chooser mirror observes the room leave RESULTS and exits to its OWN ceremony)
# and (g) the automatic FINISHED re-take reaching a second native CHARSELECT on
# BOTH endpoints. `--through e` / `--through c` remain as faster intermediate
# stops. See tests/README.md for the full description.
MANUAL_NETWORK_LANES = (
    "tools/online/check_online_native_flow_cloud.py",
    "tools/online/cloud_two_process_engine_boot.py",
)


def run_command(label: str, cmd: list[str], verbose: bool) -> tuple[bool, float]:
    if verbose:
        cmd = [*cmd, "-v"]
    start = time.monotonic()
    proc = subprocess.run(cmd, cwd=ROOT, text=True,
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    elapsed = time.monotonic() - start
    ok = proc.returncode == 0
    # Echo the lane's own PASS/FAIL tail so the aggregate log is self-describing.
    tail = "\n".join((proc.stdout or "").strip().splitlines()[-4:])
    print(f"\n===== {label} ({elapsed:.1f}s) {'PASS' if ok else 'FAIL'} =====")
    print(tail)
    return ok, elapsed


def run_lane(lane: str, build: str, rom: str, verbose: bool) -> tuple[bool, float]:
    return run_command(
        lane,
        [sys.executable, str(TESTS / lane), "--build", build, "--rom", rom],
        verbose,
    )


# NON-DEFAULT: the cross-region lanes joined ONLY by --pal-rom. Strictly
# additive -- without --pal-rom the 29-lane sweep above is byte-identical.
# Each entry is (summary label, the argv after the interpreter). The two
# accepted ROM payloads are byte-identical, so a PAL endpoint racing the online
# 30 Hz manifest under the launcher-armed NTSC identity must reproduce a US
# endpoint's authority/input/event streams -- these lanes prove exactly that.
def pal_lanes(build: str, rom: str, pal_rom: str
              ) -> tuple[tuple[str, list[str]], ...]:
    convergence = str(TESTS / "check_online_process_convergence.py")
    return (
        # Same-process epoch scope: an ONLINE PAL epoch (NTSC identity armed)
        # then an OFFLINE PAL epoch that re-latches the authentic 50 Hz clock.
        ("check_online_region_reentry.py --rom pal",
         [str(TESTS / "check_online_region_reentry.py"),
          "--build", build, "--rom", pal_rom]),
        # Four all-PAL endpoints converge on the 30 Hz manifest.
        ("check_online_process_convergence.py --regions pal",
         [convergence, "--build", build, "--rom", rom, "--pal-rom", pal_rom,
          "--regions", "pal"]),
        # Mixed US<->PAL endpoints: byte-identical authority streams = the
        # cross-region capability itself.
        ("check_online_process_convergence.py --regions cross",
         [convergence, "--build", build, "--rom", rom, "--pal-rom", pal_rom,
          "--regions", "cross"]),
        # PAL live-loopback at 30 Hz whose converged hash must EQUAL the pinned
        # US golden (bit-identity), asserted via check_online_engine_boot's
        # imported --expect-hash default.
        ("check_online_engine_boot.py --rom pal --authored-hz 30",
         [str(TESTS / "check_online_engine_boot.py"),
          "--build", build, "--rom", pal_rom, "--authored-hz", "30"]),
    )


def run_isolation(verbose: bool) -> tuple[bool, list[str]]:
    print("\n===== isolation guard (fresh OFF build) =====")
    cmd = [sys.executable, str(ROOT / "tools" / "check_online_isolation.py")]
    if verbose:
        cmd.append("-v")
    proc = subprocess.run(cmd, cwd=ROOT, text=True,
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    out = (proc.stdout or "").strip()
    print(out)
    # M1(a): the guard's byte-identity gate is now HARD, but a deliberate
    # --allow-hash-drift or a degraded environment (e.g. no reference build cache)
    # still emits a WARNING while exiting 0. Surface every such line in the final
    # summary so a degradation can NEVER hide behind "RESULT: GREEN".
    warnings = [line.strip() for line in out.splitlines()
                if "[isolation] WARNING" in line]
    return proc.returncode == 0, warnings


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", default="build-beta")
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    parser.add_argument("--no-isolation", action="store_true",
                        help="skip the fresh-OFF isolation guard (lanes only)")
    parser.add_argument(
        "--pal-rom", default=None,
        help="also run the cross-region PAL lanes (region re-entry, "
             "--regions pal|cross convergence, PAL golden-equality boot) against "
             "this European pal.v80 ROM; strictly additive -- omitted, the "
             "default sweep is unchanged")
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    results: list[tuple[str, bool, float]] = []
    for lane in LANES:
        ok, elapsed = run_lane(lane, args.build, args.rom, args.verbose)
        results.append((lane, ok, elapsed))
        # Keep going after a failure so the operator sees the full picture.

    # Additive cross-region arm: the PAL lanes join the serial schedule after
    # the default sweep. Nothing here runs without --pal-rom.
    if args.pal_rom:
        for label, argv in pal_lanes(args.build, args.rom, args.pal_rom):
            ok, elapsed = run_command(label, [sys.executable, *argv],
                                      args.verbose)
            results.append((label, ok, elapsed))

    isolation_ok = True
    isolation_warnings: list[str] = []
    if not args.no_isolation:
        isolation_ok, isolation_warnings = run_isolation(args.verbose)

    passed = sum(1 for _n, ok, _t in results if ok)
    total = len(results)
    print("\n" + "=" * 64)
    print(f"NATIVE ONLINE SUITE: {passed}/{total} lanes passed"
          + ("" if args.no_isolation
             else f"; isolation guard {'PASS' if isolation_ok else 'FAIL'}"))
    for name, ok, elapsed in results:
        print(f"  [{'PASS' if ok else 'FAIL'}] {name} ({elapsed:.1f}s)")
    # M1(a): surface any isolation warning/degradation in the summary so it cannot
    # hide behind a GREEN result (e.g. an --allow-hash-drift toolchain bump).
    for warning in isolation_warnings:
        print(f"  [WARN] isolation: {warning}")
    all_ok = passed == total and isolation_ok
    print("RESULT:", "GREEN" if all_ok else "RED")
    print("NOTE: ctest excludes these engine lanes by design; this runner is the "
          "native-takeover regression gate. Lanes run SERIALLY (timing-sensitive).")
    return 0 if all_ok else 1


if __name__ == "__main__":
    sys.exit(main())
