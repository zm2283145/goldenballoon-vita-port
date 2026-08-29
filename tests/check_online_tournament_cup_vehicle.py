#!/usr/bin/env python3
"""REGRESSION: a MIXED-VEHICLE tournament cup completes all four rounds.

A real product defect: a tournament persists ONE vehicle across every round (the
resident coordinator re-Readies + STARTs each round with the SAME seat vehicle --
there is no per-round re-selection), but the native CHARSELECT/VEHICLESELECT/
TRACKSELECT auto-narrow only clamped the pick to the cup's ROUND-0 track. Cup 0
(Dino Domain) round 0 is Ancient Lake (all three vehicles legal), so the default
car survived selection -- but round 3 is Hot Top Volcano (track 7), which is
hovercraft/plane ONLY (vehicle mask 0x6). At round 3 the reducer's
all_vehicles_legal gate REJECTED BEGIN_LOADING (ILLEGAL_VEHICLE), the round never
booted, and the tournament STALLED at raceCount=3 -- the cup was unwinnable online.

Users can pick this cup online, so this lane guards the fix: the native screens now
narrow the pick to the cup's whole INTERSECTION mask (legal for EVERY round), so a
cup-0 pick auto-narrows off car to a hovercraft/plane legal for Hot Top Volcano too.

RED (pre-fix): only 3 direct boots ([5,3,29]); at round 4
  `[START] BEGIN_LOADING result accepted=0 error=ILLEGAL_VEHICLE` ->
  `round advance TIMEOUT ... stage=5` -> `descless wait TIMEOUT ... (raceCount=3)`;
  never reaches FINISHED.
GREEN (post-fix): EXACTLY 4 boots on the cup-0 schedule [5, 3, 29, 7], no
  ILLEGAL_VEHICLE, no round-advance/descless TIMEOUT, and the cup completes to the
  single FINISHED handshake.

Descriptor-less two-endpoint loopback (the same native-takeover path a real
2-machine game uses); the loopback joiner stand-in also narrows to a cup-legal
vehicle (as a real remote endpoint's native TRACKSELECT does).
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

from harness_utils import resolve_binary
from online_lane_util import (
    CUP_ROUNDS, DIRECT_BOOT_RE, FINISHED_ENGINE_RE, FORBIDDEN_ONLINE,
    SESSION_END_RE, forbidden_marker, make_fail, run_engine,
)

ROOT = Path(__file__).resolve().parent.parent
TICKS = 34000
# kCupTracks[0] (lobby_core.c): Ancient Lake / Fossil Canyon / Jungle Falls /
# Hot Top Volcano. Round 3 (track 7) is hovercraft/plane only -- the mixed cup.
CUP0_TRACKS = [5, 3, 29, 7]

ROUND_ADVANCE_TIMEOUT_RE = re.compile(
    r"^\[online-resident-live\] round advance TIMEOUT", re.MULTILINE)
DESCLESS_TIMEOUT_RE = re.compile(
    r"^\[online-session\] descless wait TIMEOUT", re.MULTILINE)

fail = make_fail("tournament cup vehicle")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", default="build-beta")
    parser.add_argument("--rom", type=Path, default="baserom.us.v80.z64")
    parser.add_argument("--ticks", type=int, default=TICKS)
    parser.add_argument("--timeout", type=int, default=700)
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    binary = Path(resolve_binary(args.build)).expanduser().resolve()
    rom = args.rom.expanduser().resolve()
    for path, label in ((binary, "binary"), (rom, "ROM")):
        if not path.is_file():
            parser.error(f"missing {label}: {path}")

    try:
        rc, output = run_engine(
            binary, rom, ticks=args.ticks, timeout=args.timeout,
            verbose=args.verbose,
            extra_env={
                "MDKR_APP_TEST_ONLINE_LIVE_LOBBY_START": "1",
                "MDKR_APP_TEST_ONLINE_MODE": "tournament",
                "MDKR_APP_TEST_ONLINE_CUP": "0",  # the mixed-vehicle cup
                "MDKR_TEST_ONLINE_LOBBY_START": "1",
                "MDKR_TEST_ONLINE_LOBBY_TOURNAMENT": "1",
                "MDKR_TEST_ONLINE_RESULTS_HOST_PRESS": "1",
                "MDKR_TEST_ONLINE_RESULTS_CHOOSER": "5",  # navigate to FINISH
                "MDKR_TEST_ONLINE_CEREMONY_SKIP": "1",
            })
    except subprocess.TimeoutExpired as error:
        return fail(f"cup-0 tournament run timed out (a round-advance stall on the "
                    f"mixed-vehicle round would look like this): {error}")

    # A stalled round leaves an ILLEGAL_VEHICLE reject in flight; treat it (and the
    # generic online forbidden markers) as fatal.
    marker = forbidden_marker(output, *FORBIDDEN_ONLINE, "ILLEGAL_VEHICLE")
    if marker:
        return fail(f"observed forbidden marker {marker!r} -- the mixed-vehicle cup "
                    f"still rejects a round's BEGIN_LOADING (the pick was not "
                    f"narrowed to the cup intersection)", output)
    if ROUND_ADVANCE_TIMEOUT_RE.search(output) or DESCLESS_TIMEOUT_RE.search(output):
        return fail("a round-advance / descriptor-less watchdog TIMEOUT fired -- the "
                    "cup stalled instead of booting every round", output)
    if rc != 0:
        return fail(f"process exited {rc} (expected a clean 0)", output)

    # EXACTLY CUP_ROUNDS boots, on the cup-0 schedule (the round-3 track is the one
    # that used to reject).
    boots = DIRECT_BOOT_RE.findall(output)
    if len(boots) != CUP_ROUNDS:
        return fail(f"expected EXACTLY {CUP_ROUNDS} direct boots (the mixed cup must "
                    f"reach round 4), saw {len(boots)}: {boots!r}", output)
    booted_tracks = [int(t) for t, _p in boots]
    if booted_tracks != CUP0_TRACKS:
        return fail(f"booted tracks {booted_tracks} != cup-0 schedule {CUP0_TRACKS} "
                    f"(round 4 = Hot Top Volcano track 7 must boot)", output)

    # The cup completed to the single FINISHED handshake.
    if not FINISHED_ENGINE_RE.search(output):
        return fail("the cup never reached FINISHED -- it did not complete", output)
    end = SESSION_END_RE.findall(output)
    if not end or end[-1] != ("FINISHED", "0"):
        return fail(f"launcher never read reason=FINISHED result=0; saw {end}", output)

    print(
        "PASS online tournament cup vehicle: the MIXED-VEHICLE cup 0 (Dino Domain) "
        f"booted ALL {CUP_ROUNDS} rounds on {CUP0_TRACKS} -- round 4 Hot Top Volcano "
        "(hovercraft/plane only, mask 0x6) accepted the auto-narrowed pick instead of "
        "rejecting a car with ILLEGAL_VEHICLE -- and completed to the single FINISHED "
        "handshake (no round-advance/descless TIMEOUT, clean exit 0)."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
