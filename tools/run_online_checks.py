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
"""

from __future__ import annotations

import argparse
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TESTS = ROOT / "tests"

# The full native-takeover lane set (the 13 exit-gate lanes + the C1 no-seam proof).
# Order is deliberate: fastest structural boots first, the long multi-race loopback
# soaks last, so a quick break surfaces early.
LANES = (
    "check_online_engine_boot_direct.py",   # golden race hash 7da2ea67 pinned
    "check_online_session_boot.py",
    "check_online_charselect.py",
    "check_online_trackselect.py",
    "check_online_session_results.py",
    "check_online_resident_live.py",
    "check_online_session_end.py",          # incl. joiner-follow seam scenario
    "check_online_joiner_terminal.py",      # exit-gate C1 no-seam joiner proof
    "check_online_ceremony.py",
    "check_online_lobby_start.py",
    "check_online_lobby_single_endpoint.py",
    "check_online_room_ready_rearm.py",
    "check_online_lobby_tournament.py",
    "check_online_tournament.py",           # keystone: 34,30 + peer==peer race-hash
)                                           #  convergence (the GOLDEN literal is
#                                            pinned only by check_online_engine_boot_direct.py)


def run_lane(lane: str, build: str, rom: str, verbose: bool) -> tuple[bool, float]:
    cmd = [sys.executable, str(TESTS / lane), "--build", build, "--rom", rom]
    if verbose:
        cmd.append("-v")
    start = time.monotonic()
    proc = subprocess.run(cmd, cwd=ROOT, text=True,
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    elapsed = time.monotonic() - start
    ok = proc.returncode == 0
    # Echo the lane's own PASS/FAIL tail so the aggregate log is self-describing.
    tail = "\n".join((proc.stdout or "").strip().splitlines()[-4:])
    print(f"\n===== {lane} ({elapsed:.1f}s) {'PASS' if ok else 'FAIL'} =====")
    print(tail)
    return ok, elapsed


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
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    results: list[tuple[str, bool, float]] = []
    for lane in LANES:
        ok, elapsed = run_lane(lane, args.build, args.rom, args.verbose)
        results.append((lane, ok, elapsed))
        # Keep going after a failure so the operator sees the full picture.

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
