#!/usr/bin/env python3
"""PRODUCTION-SHAPE lobby-start: an UNCONFIGURED single-race room + room latency.

check_online_lobby_start.py proves the native screens own race 1 -- but its room
carries a track-5 pre-config whose one purpose is to "unlock READY at SELECTING".
The REAL production room has no such pre-config: the interactive launcher's
pairing (and the two-process cloud capstone's AUTOPAIR) creates a FRESH
single-race room with NO configured track, and the native screens never cast a
track vote. The first-ever real two-peer cloud run wedged exactly there: with
configured_track unset the SELECTING view gates READY behind a track VOTE, so
the reverse-feed READY was refused locally (silently) forever, seat.ready never
latched, and the session sat at CHARSELECT until the wall clock ran out.

This lane drives the SAME native lobby-start flow on the PRODUCTION room shape:
  * MDKR_APP_TEST_ONLINE_UNCONFIGURED=1 -- the loopback room builder SKIPS the
    READY-unlock pre-config (witnessed; the pre-config line must NOT appear);
  * MDKR_APP_TEST_ONLINE_ROOM_LATENCY=3 -- the loopback room channel carries
    3 pumps of latency per leg, with the State broadcast one pump SLOWER than a
    CommandResult (the real transport's HTTP-result-beats-WS-state ordering),
    so the optimistic-revision interleaving of two concurrently-writing
    endpoints is genuinely exercised, not synchronously papered over.

RECORDED RED (pre-fix, at d7021872 + these test seams only): the run burned its
entire tick budget parked on CHARSELECT --
    [online-charselect] render cursor=2 name=PIPSY portrait=8 local{conf=1
        ready=1 seatChar=2 seatReady=0} remote{seat=1 char=1 ready=1 name=-}
        ... intent{hover=2 vehicle=0 confirmed=1 ready=1}
    [ONLINE] command stale type=SET_CHARACTER error=STALE_REVISION retry=6
        (re-sending against fresh revision)   <- blind re-sends, same stale rev
    FAIL online lobby-unconfigured: native VEHICLESELECT never fronted after
    CHARSELECT (the unconfigured-room READY wedge: seat.ready never latched)
-- the exact two-peer cloud wedge (seat CHARACTER converged, seat.ready never
latched, stale-retry churn), reproduced in-process in under a minute.

GREEN bar (the fix): READY/CHANGE_SELECTION are reducer-authoritative in the
LOBBY phase (the reducer alone gates READY on character+vehicle -- the view's
vote step is the legacy ImGui picker's), the reverse-feed planner holds READY
until the AUTHORITATIVE seat shows the selections landed, and a STALE_REVISION
refusal resyncs against the next State (re-evaluating whether the command is
still needed) instead of blind-re-sending against the same stale revision. With
those in place this lane must run the full native flow to a race boot on the
track the native TRACKSELECT locks (id 3, Fossil Canyon) -- with NO pre-config
anywhere in the room's history.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

from harness_utils import resolve_binary
from online_lane_util import (
    DIRECT_BOOT_RE, FORBIDDEN_ONLINE, GAMEMODE_ONLINE_SESSION, ONLINE_RACE_RE,
    forbidden_marker, make_fail, run_engine,
)

ROOT = Path(__file__).resolve().parent.parent
TICKS = 20000
HOST_TRACK = 3  # Fossil Canyon -- what the native TRACKSELECT locks + boots

BEGIN_LOBBY_RE = re.compile(
    r"^\[online-session\] begin: lobby-start \(no descriptor\)", re.MULTILINE)
BEGIN_DESCRIPTOR_RE = re.compile(
    r"^\[online-session\] begin: separated boot path entered", re.MULTILINE)
SKIPPED_RE = re.compile(
    r"^\[online-lobby-start\] pre-config SKIPPED \(unconfigured", re.MULTILINE)
PRECONFIG_RE = re.compile(
    r"^\[online-lobby-start\] pre-config track=(\d+)", re.MULTILINE)
CHARSELECT_ENTER_RE = re.compile(
    r"^\[online-charselect\] enter:", re.MULTILINE)
TO_VEHICLESELECT_RE = re.compile(
    r"^\[online-session\] charselect -> vehicleselect", re.MULTILINE)
VEHICLESELECT_ENTER_RE = re.compile(
    r"^\[online-vehicleselect\] enter:", re.MULTILINE)
TO_TRACKSELECT_RE = re.compile(
    r"^\[online-session\] vehicleselect -> trackselect", re.MULTILINE)
TRACKSELECT_ENTER_RE = re.compile(
    r"^\[online-trackselect\] enter:", re.MULTILINE)
DEFER_RE = re.compile(
    r"^\[online-session\] race-1 boot deferred: descriptor not ready",
    re.MULTILINE)
ARMED_RE = re.compile(
    r"^\[online-lobby-start\] race-1 armed: epoch=(\d+)", re.MULTILINE)
RACE_RE = re.compile(
    r"^\[online-session\] phase=RACE booting after (\d+) LOBBY_WAIT tick\(s\); "
    r"isolation gGameMode=(\d+) gCurrentMenuId=(-?\d+).*\[race=(\d+)\]$",
    re.MULTILINE)
HONORED_RE = re.compile(
    r"^\[online-boot\] track honored: (\d+)$", re.MULTILINE)
REVERSE_TRACK_RE = re.compile(
    r"^\[online-reverse\] SET_CONFIG_TRACK value=(\d+) sent=(\d+)$",
    re.MULTILINE)


fail = make_fail("lobby-unconfigured")


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
        "MDKR_APP_TEST_ONLINE_LIVE_LOBBY_START": "1",
        "MDKR_TEST_ONLINE_LOBBY_START": "1",
        # THE two production-shape knobs (see the lane header):
        "MDKR_APP_TEST_ONLINE_UNCONFIGURED": "1",
        "MDKR_APP_TEST_ONLINE_ROOM_LATENCY": "3",
    }
    try:
        returncode, output = run_engine(
            binary, rom, ticks=args.ticks, timeout=args.timeout,
            verbose=args.verbose, extra_env=extra_env,
            prefix="mdkr64-online-lobby-unconfigured-")
    except subprocess.TimeoutExpired as error:
        return fail(f"engine run timed out (a stuck unconfigured-room "
                    f"selection would look like this): {error}")

    marker = forbidden_marker(output, *FORBIDDEN_ONLINE,
                              "[ROLLBACK] online race admission rejected")
    if marker:
        return fail(f"observed forbidden marker {marker!r}", output)
    if returncode != 0:
        return fail(f"process exited {returncode}", output)

    # --- The room is genuinely PRODUCTION-SHAPED: no pre-config anywhere ----
    if not SKIPPED_RE.search(output):
        return fail("the unconfigured-room witness never fired (the lane did "
                    "not run on the production room shape)", output)
    if PRECONFIG_RE.search(output):
        return fail("a READY-unlock pre-config fired -- this lane must run on "
                    "an UNCONFIGURED room (that pre-config is exactly what let "
                    "the production wedge escape every prior lane)", output)

    # --- Descriptor-less begin ------------------------------------------------
    if len(BEGIN_LOBBY_RE.findall(output)) != 1:
        return fail("the session did not begin DESCRIPTOR-LESS exactly once",
                    output)
    if BEGIN_DESCRIPTOR_RE.search(output):
        return fail("the descriptor-FIRST begin fired", output)

    # --- The full native chain fronted, in order ------------------------------
    # THE regression bar: at HEAD (pre-fix) the flow never left CHARSELECT --
    # seat.ready never latched on the unconfigured room, so VEHICLESELECT below
    # is precisely where the RED run died.
    cs = CHARSELECT_ENTER_RE.search(output)
    to_vs = TO_VEHICLESELECT_RE.search(output)
    vs = VEHICLESELECT_ENTER_RE.search(output)
    to_ts = TO_TRACKSELECT_RE.search(output)
    ts = TRACKSELECT_ENTER_RE.search(output)
    if cs is None:
        return fail("native CHARSELECT never fronted", output)
    if to_vs is None or vs is None:
        return fail("native VEHICLESELECT never fronted after CHARSELECT (the "
                    "unconfigured-room READY wedge: seat.ready never latched)",
                    output)
    if to_ts is None or ts is None:
        return fail("native TRACKSELECT never fronted after VEHICLESELECT",
                    output)
    if not (cs.start() < vs.start() < ts.start()):
        return fail("CHARSELECT -> VEHICLESELECT -> TRACKSELECT ordering was "
                    "violated", output)

    # --- Readiness gate: deferred, armed, booted exactly once -----------------
    defer = DEFER_RE.search(output)
    if defer is None:
        return fail("the race-1 readiness gate never DEFERRED the boot", output)
    armed = ARMED_RE.search(output)
    if armed is None:
        return fail("the launcher never armed race 1 (the flow stalled before "
                    "BEGIN_LOADING -- over latency this is the stale-revision "
                    "churn signature)", output)
    race = RACE_RE.findall(output)
    if len(race) != 1:
        return fail(f"expected EXACTLY ONE race boot, got {len(race)}: {race!r}",
                    output)
    lobby_ticks, boot_gamemode, boot_menu_id, race_index = race[0]
    race_pos = RACE_RE.search(output).start()
    if not (defer.start() < armed.start() < race_pos):
        return fail("race 1 booted before the readiness gate deferred + armed",
                    output)
    if int(race_index) != 1:
        return fail(f"race boot reported race={race_index}, expected 1", output)

    # --- Offline isolation -----------------------------------------------------
    if int(boot_gamemode) != GAMEMODE_ONLINE_SESSION:
        return fail(f"at hand-off gGameMode={boot_gamemode}, expected "
                    f"{GAMEMODE_ONLINE_SESSION}", output)
    if int(boot_menu_id) != 0:
        return fail(f"gCurrentMenuId={boot_menu_id} at hand-off", output)

    # --- The native TRACKSELECT's lock is the FIRST config this room ever saw -
    reverse_after = [
        (int(m.group(1)), int(m.group(2)))
        for m in REVERSE_TRACK_RE.finditer(output)
        if m.start() > ts.start()
    ]
    if not any(value == HOST_TRACK and sent == 1
               for value, sent in reverse_after):
        return fail(f"the native TRACKSELECT never dispatched SET_CONFIG_TRACK="
                    f"{HOST_TRACK} (sent=1) after entering (saw "
                    f"{reverse_after!r})", output)

    direct = DIRECT_BOOT_RE.findall(output)
    if len(direct) != 1:
        return fail(f"expected exactly one [online-boot] direct race, got "
                    f"{direct!r}", output)
    direct_track, direct_players = direct[0]
    if int(direct_track) != HOST_TRACK:
        return fail(f"direct boot fired for track {direct_track}, expected the "
                    f"NATIVE-TRACKSELECT-selected track {HOST_TRACK}", output)
    honored = HONORED_RE.findall(output)
    if not honored or int(honored[-1]) != HOST_TRACK:
        return fail(f"the boot did not honor the native track {HOST_TRACK} "
                    f"(honored={honored!r})", output)
    if "[online-boot] track divergence" in output:
        return fail("the boot logged a track divergence", output)

    online_race = ONLINE_RACE_RE.findall(output)
    if not online_race or int(online_race[0][0]) != HOST_TRACK:
        return fail(f"the engine never entered the ONLINE rollback race on "
                    f"track {HOST_TRACK} (saw {online_race!r})", output)

    print(
        "PASS online lobby-unconfigured: the native flow converged on the "
        "PRODUCTION room shape -- a FRESH single-race room with NO configured "
        "track (pre-config SKIPPED, witnessed) over a 3-pump-per-leg latency "
        "room channel (State one pump behind CommandResult). CHARSELECT "
        "readied on char+vehicle alone (the reducer's own gate; no track "
        "vote), VEHICLESELECT and TRACKSELECT fronted in order, the host lock "
        f"dispatched SET_CONFIG_TRACK={HOST_TRACK} as the room's FIRST-EVER "
        f"config, and race 1 booted exactly once on track {direct_track} "
        f"(players={direct_players}, honored, race={race_index}, "
        f"gGameMode={boot_gamemode}, menuId={boot_menu_id}, "
        f"{lobby_ticks} LOBBY_WAIT tick(s)). This is the in-process regression "
        "gate for the two-peer cloud CHARSELECT wedge (READY never latching "
        "over real latency on an unconfigured room)."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
