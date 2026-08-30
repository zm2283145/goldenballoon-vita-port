#!/usr/bin/env python3
"""KEYSTONE PROOF: the NATIVE online screens own RACE 1.

A headless TWO-ENDPOINT loopback lane that boots BOTH endpoints at the LOBBY with
NO race descriptor and the party_link bridge installed, then drives the visible
(host) endpoint's native CHARSELECT -> TRACKSELECT via the EXISTING reverse feed
(scripted screen input dispatched to the real adapter) so the host's selection /
ready / SET_CONFIG_TRACK / START ride the real reducer, build the descriptor live
(BEGIN_LOADING), and boot race 1 -- but ONLY once the descriptor / roster /
match-input are truly ready.

Unlike check_online_engine_boot_direct / check_online_charselect (which boot the
engine race-ready, descriptor ALREADY built), here the engine begins
DESCRIPTOR-LESS and the descriptor is built DURING the native selection. This is
the proof of the highest-crash-risk change: the race-1 readiness gate
holds the boot (never dereferencing a NULL/stale descriptor) until the launcher's
real descriptor lands.

Assertions:
  * the session BEGAN descriptor-less        -> [online-session] begin: lobby-start
  * the descriptor-first begin was NOT taken -> no "begin: separated boot path"
  * CHARSELECT then VEHICLESELECT then TRACKSELECT fronted (native), offline menus
    bypassed
  * the race-1 readiness gate DEFERRED the boot when the descriptor was not ready
    -> [online-session] race-1 boot deferred ... -> LOBBY_WAIT re-wait
  * the launcher then built + armed race 1    -> [online-lobby-start] race-1 armed
  * race 1 booted EXACTLY ONCE and ONLY AFTER the descriptor was ready, on the
    track the native TRACKSELECT chose (id 3, Fossil Canyon) -- the room pre-config
    (id 5) exists ONLY to unlock READY at SELECTING; the native screen dispatched
    SET_CONFIG_TRACK=3 sent=1 over the reverse feed AFTER entering, and the BOOTED
    track (3) != pre-config (5), so the track choice is genuinely native-owned
    (not vacuously honored by the pre-config)
  * no NULL deref, no premature/failed boot, no admission rejection
  * gGameMode == GAMEMODE_ONLINE_SESSION (2) and gCurrentMenuId == 0 at hand-off
  * the engine entered the ONLINE rollback race on the native track (loadedTrack=3)

This lane runs SINGLE-RACE (no MDKR_APP_TEST_ONLINE_MODE env), so it is the single-
race native lobby-start proof. It also asserts that the PRODUCTION room-ready
GATE now ROUTES a single-race room to this native takeover:
  * OnlineRoom_roomReadyConditionHolds fires EXACTLY ONCE for a SINGLE-RACE room and
    routes to lobby-start (route=lobby-start) -- the tournament-only clause is dropped,
    so single race takes the same descriptor-less native path a tournament does. READY
    unlocks on char+vehicle (no track vote), and BEGIN_LOADING resolves the native
    TRACKSELECT's configured_track with no vote (the native screens never cast a vote).
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
from online_lane_util import (
    DIRECT_BOOT_RE, FORBIDDEN_ONLINE, GAMEMODE_ONLINE_SESSION, ONLINE_RACE_RE,
    forbidden_marker, make_fail, run_engine,
)

ROOT = Path(__file__).resolve().parent.parent
TICKS = 20000
# The native TRACKSELECT locks a track DIFFERENT from the room's READY-unlock
# pre-config, so "the booted track is the one native TRACKSELECT chose" is proven
# (not vacuously honored by the pre-config).
HOST_TRACK = 3      # Fossil Canyon -- what the native TRACKSELECT locks + boots
PRECONFIG_TRACK = 5  # Ancient Lake -- pre-configured ONLY to unlock READY at SELECTING

BEGIN_LOBBY_RE = re.compile(
    r"^\[online-session\] begin: lobby-start \(no descriptor\)", re.MULTILINE)
BEGIN_DESCRIPTOR_RE = re.compile(
    r"^\[online-session\] begin: separated boot path entered", re.MULTILINE)
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
    r"^\[online-session\] race-1 boot deferred: descriptor not ready", re.MULTILINE)
ARMED_RE = re.compile(
    r"^\[online-lobby-start\] race-1 armed: epoch=(\d+)", re.MULTILINE)
REWAIT_BOOT_RE = re.compile(
    r"^\[online-session\] phase=LOBBY_WAIT \(lobby-start re-wait\) tick=(\d+) "
    r"epoch=(\d+) bootedEpoch=(\d+) ready=1$", re.MULTILINE)
RACE_RE = re.compile(
    r"^\[online-session\] phase=RACE booting after (\d+) LOBBY_WAIT tick\(s\); "
    r"isolation gGameMode=(\d+) gCurrentMenuId=(-?\d+).*\[race=(\d+)\]$",
    re.MULTILINE)
HONORED_RE = re.compile(
    r"^\[online-boot\] track honored: (\d+)$", re.MULTILINE)
PRECONFIG_RE = re.compile(
    r"^\[online-lobby-start\] pre-config track=(\d+)", re.MULTILINE)
REVERSE_TRACK_RE = re.compile(
    r"^\[online-reverse\] SET_CONFIG_TRACK value=(\d+) sent=(\d+)$", re.MULTILINE)
ROOM_READY_PROBE_RE = re.compile(
    r"^\[online-room-ready-probe\] fires=(\d+) conditionHeld=(\d+) "
    r"published=(\d+) route=(\S+)", re.MULTILINE)
# The poll emits these [online-room-ready] diagnostic lines when it fires. The probe
# drives the poll directly (it never runs the launcher loop), so both the latch-set
# and the publish lines must appear in the probe run's output.
ROOM_READY_LATCH_RE = re.compile(
    r"^\[online-room-ready\] latch set", re.MULTILINE)
ROOM_READY_PUBLISH_RE = re.compile(
    r"^\[online-room-ready\] published adapter", re.MULTILINE)


fail = make_fail("lobby-start")


def check_room_ready_gate_single_race(binary, rom, verbose):
    """The PRODUCTION room-ready GATE routes a SINGLE-RACE room to the native
    takeover. The boot proof (this lane's main run) rides the descriptor-less engine
    seam directly; this proves the routing gate OnlineRoom_roomReadyConditionHolds now
    FIRES for a single-race room (no tournament env) -- exactly once + route=lobby-
    start, the SAME native takeover a tournament room gets. Formerly a single-race
    room deferred to the race-ready ImGui fallback (fires=0), so single race never
    reached the native path in production; that tournament-only clause is now dropped.

    The probe holds the visible endpoint as the PRODUCTION OwningLiveAdapter wrapper
    (the same wrapper class the Online Room panel builds), not a raw LiveAdapter, so
    published=1 proves the wrapper resolved through the mdkrResolveLive hook end-to-end
    -- this probe is the only coverage of the production wrapper's resolve override, so
    keep it holding the real wrapper. It also asserts the poll's [online-room-ready]
    latch-set + publish diagnostics appear."""
    try:
        returncode, output = run_engine(
            binary, rom, ticks=2000, timeout=120, verbose=verbose,
            extra_env={"MDKR_APP_TEST_ONLINE_ROOM_READY_PROBE": "1"},
            prefix="mdkr64-online-lobby-start-gate-")
    except subprocess.TimeoutExpired as error:
        return fail(f"single-race room-ready gate probe timed out: {error}")
    if returncode != 0:
        return fail(f"single-race room-ready gate probe exited {returncode}", output)
    m = ROOM_READY_PROBE_RE.search(output)
    if m is None:
        return fail("single-race room-ready gate probe emitted no result line", output)
    fires, held, published, route = m.groups()
    if fires != "1" or held != "1" or published != "1" or route != "lobby-start":
        return fail(f"the room-ready gate did NOT route the single-race room to the "
                    f"native takeover (expected fires=1 held=1 published=1 "
                    f"route=lobby-start): fires={fires} held={held} "
                    f"published={published} route={route}", output)
    # The probe holds the visible endpoint as the PRODUCTION OwningLiveAdapter wrapper
    # (published=1 above == the poll resolved that wrapper through mdkrResolveLive to
    # the exact inner it published). Confirm the poll's [online-room-ready] diagnostics
    # fired -- at least the latch-set + publish lines -- so a real-hardware takeover is
    # diagnosable from the log.
    if ROOM_READY_LATCH_RE.search(output) is None:
        return fail("the poll emitted no '[online-room-ready] latch set' line in the "
                    "probe run (room-ready diagnostics missing)", output)
    if ROOM_READY_PUBLISH_RE.search(output) is None:
        return fail("the poll emitted no '[online-room-ready] published adapter' line "
                    "in the probe run (room-ready diagnostics missing)", output)
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

    # MDKR_APP_TEST_ONLINE_LIVE_LOBBY_START (launcher): loopback pair STOPPED at
    # SELECTING, party_link installed, visible engine booted descriptor-less.
    # MDKR_TEST_ONLINE_LOBBY_START (engine-side scripted screen INPUT, no
    # self-contained feed): the native CHARSELECT/TRACKSELECT drive
    # selection/ready/track/START whose intents ride the REAL reverse feed.
    extra_env = {
        "MDKR_APP_TEST_ONLINE_LIVE_LOBBY_START": "1",
        "MDKR_TEST_ONLINE_LOBBY_START": "1",
    }
    try:
        returncode, output = run_engine(
            binary, rom, ticks=args.ticks, timeout=args.timeout,
            verbose=args.verbose, extra_env=extra_env,
            prefix="mdkr64-online-lobby-start-")
    except subprocess.TimeoutExpired as error:
        return fail(f"engine run timed out (a stuck lobby-start / a boot that "
                    f"never becomes ready would look like this): {error}")

    marker = forbidden_marker(output, *FORBIDDEN_ONLINE,
                              "[ROLLBACK] online race admission rejected")
    if marker:
        return fail(f"observed forbidden marker {marker!r}", output)

    if returncode != 0:
        return fail(f"process exited {returncode}", output)

    # --- Descriptor-less begin (the latch) ----------------------------------
    if len(BEGIN_LOBBY_RE.findall(output)) != 1:
        return fail("the session did not begin DESCRIPTOR-LESS exactly once "
                    "(mode_intro's party_link lobby-start fork)", output)
    if BEGIN_DESCRIPTOR_RE.search(output):
        return fail("the descriptor-FIRST begin fired -- the lobby-start fork "
                    "must be the ONLY begin on this lane", output)

    # --- CHARSELECT -> VEHICLESELECT -> TRACKSELECT fronted (native), ordered -
    # The native flow always inserts the VEHICLE select screen between CHARSELECT
    # and TRACKSELECT (the player picks car/hovercraft/plane); the lobby-start lane
    # scripts a confirm on it, so the full production flow-shape runs here.
    cs = CHARSELECT_ENTER_RE.search(output)
    to_vs = TO_VEHICLESELECT_RE.search(output)
    vs = VEHICLESELECT_ENTER_RE.search(output)
    to_ts = TO_TRACKSELECT_RE.search(output)
    ts = TRACKSELECT_ENTER_RE.search(output)
    if cs is None:
        return fail("native CHARSELECT never fronted", output)
    if to_vs is None or vs is None:
        return fail("native VEHICLESELECT never fronted after CHARSELECT", output)
    if to_ts is None or ts is None:
        return fail("native TRACKSELECT never fronted after VEHICLESELECT", output)
    # CHARSELECT is entered first; the hand-off enters each next screen then logs
    # the transition, so every downstream marker follows the CHARSELECT enter and
    # the chain is strictly ordered.
    if not (cs.start() < vs.start() < ts.start()):
        return fail("CHARSELECT -> VEHICLESELECT -> TRACKSELECT ordering was "
                    "violated", output)
    if not (cs.start() < to_vs.start() < to_ts.start()):
        return fail("the native screen hand-off ordering was violated", output)

    # --- Race-1 readiness gate: DEFERRED then ARMED then booted --------------
    defer = DEFER_RE.search(output)
    if defer is None:
        return fail("the race-1 readiness gate never DEFERRED the boot -- a "
                    "descriptor-less session must refuse to boot before the real "
                    "descriptor lands (risk of a NULL/stale deref)", output)
    armed = ARMED_RE.search(output)
    if armed is None:
        return fail("the launcher never armed race 1 (descriptor/roster/match-"
                    "input never became ready)", output)
    rewait = REWAIT_BOOT_RE.search(output)
    if rewait is None:
        return fail("the descriptor-less LOBBY_WAIT re-wait never reported "
                    "ready=1 (the gate never opened)", output)

    race = RACE_RE.findall(output)
    if len(race) != 1:
        return fail(f"expected EXACTLY ONE race boot, got {len(race)}: {race!r}",
                    output)
    lobby_ticks, boot_gamemode, boot_menu_id, race_index = race[0]

    # --- Ordering: booted ONLY AFTER the descriptor was ready ----------------
    race_pos = RACE_RE.search(output).start()
    if not (defer.start() < armed.start() < race_pos):
        return fail("race 1 booted before the readiness gate deferred + armed "
                    "(a premature boot)", output)
    if int(race_index) != 1:
        return fail(f"race boot reported race={race_index}, expected 1", output)

    # --- Offline isolation ---------------------------------------------------
    if int(boot_gamemode) != GAMEMODE_ONLINE_SESSION:
        return fail(f"at hand-off gGameMode={boot_gamemode}, expected "
                    f"GAMEMODE_ONLINE_SESSION ({GAMEMODE_ONLINE_SESSION})", output)
    if int(boot_menu_id) != 0:
        return fail(f"gCurrentMenuId={boot_menu_id} at hand-off -- the offline "
                    f"menu state machine was entered (expected 0)", output)
    if "input-script" in output or "race_2p_split" in output:
        return fail("a menu-nav input script was loaded -- native must own the "
                    "flow without one", output)

    # --- The NATIVE-TRACKSELECT-selected track boots, exactly once -----------
    # This is the non-vacuous track proof: the native TRACKSELECT locks a track
    # DIFFERENT from the room's READY-unlock pre-config, drives it over the reverse
    # feed, and THAT is what boots.
    preconfig = PRECONFIG_RE.findall(output)
    if not preconfig or int(preconfig[0]) != PRECONFIG_TRACK:
        return fail(f"the room pre-config witness (track {PRECONFIG_TRACK}) never "
                    f"fired (saw {preconfig!r})", output)

    # The native TRACKSELECT's SET_CONFIG_TRACK must be a REAL reverse-feed dispatch
    # (value == the native pick, sent=1) that lands AFTER the screen is entered --
    # proof the on-screen lock, not the pre-config, chose the track.
    if ts is None:
        return fail("no TRACKSELECT enter to anchor the reverse-feed dispatch",
                    output)
    reverse_after = [
        (int(m.group(1)), int(m.group(2)))
        for m in REVERSE_TRACK_RE.finditer(output)
        if m.start() > ts.start()
    ]
    native_track_sent = [
        value for value, sent in reverse_after
        if value == HOST_TRACK and sent == 1
    ]
    if not native_track_sent:
        return fail(f"the native TRACKSELECT never dispatched SET_CONFIG_TRACK="
                    f"{HOST_TRACK} (sent=1) over the reverse feed after entering "
                    f"the screen (reverse dispatches after enter: {reverse_after!r})",
                    output)
    if HOST_TRACK == PRECONFIG_TRACK:
        return fail("the native track equals the pre-config -- the track proof "
                    "would be vacuous (dedupe suppresses the dispatch)", output)

    direct = DIRECT_BOOT_RE.findall(output)
    if len(direct) != 1:
        return fail(f"expected exactly one [online-boot] direct race, got "
                    f"{direct!r}", output)
    direct_track, direct_players = direct[0]
    if int(direct_track) != HOST_TRACK:
        return fail(f"direct boot fired for track {direct_track}, expected the "
                    f"NATIVE-TRACKSELECT-selected track {HOST_TRACK} (NOT the "
                    f"pre-config {PRECONFIG_TRACK})", output)
    if int(direct_track) == PRECONFIG_TRACK:
        return fail(f"the booted track {direct_track} equals the pre-config -- the "
                    f"native TRACKSELECT choice did not take effect", output)
    honored = HONORED_RE.findall(output)
    if not honored or int(honored[-1]) != HOST_TRACK:
        return fail(f"the boot did not honor the native track {HOST_TRACK} "
                    f"(honored={honored!r})", output)
    if "[online-boot] track divergence" in output:
        return fail("the boot logged a track divergence (host track was not "
                    "what booted)", output)

    online_race = ONLINE_RACE_RE.findall(output)
    if not online_race or int(online_race[0][0]) != HOST_TRACK:
        return fail(f"the engine never entered the ONLINE rollback race on the "
                    f"native track {HOST_TRACK} (saw {online_race!r})", output)

    # --- The production ROOM-READY GATE routes single race to lobby-start ----------
    # The boot above proves the descriptor-less NATIVE path converges for a single
    # race; this proves the PRODUCTION routing gate now sends a single-race room THERE
    # (formerly it deferred to the race-ready ImGui fallback).
    gate = check_room_ready_gate_single_race(binary, rom, args.verbose)
    if gate is not None:
        return gate

    print(
        "PASS online lobby-start: NATIVE owns race 1 -- session BEGAN "
        "descriptor-less (party_link fork, no descriptor), native CHARSELECT -> "
        "VEHICLESELECT -> TRACKSELECT fronted (offline menu bypassed), the race-1 "
        "readiness gate "
        f"DEFERRED the boot until the launcher built + armed the descriptor (epoch "
        f"{armed.group(1)}), then race 1 booted EXACTLY ONCE and ONLY THEN on the "
        f"NATIVE-TRACKSELECT-selected track {direct_track} (Fossil Canyon; the host "
        f"screen dispatched SET_CONFIG_TRACK={HOST_TRACK} sent=1 over the reverse "
        f"feed AFTER entering, and track {PRECONFIG_TRACK} was pre-configured ONLY "
        f"to unlock READY -- the booted track {direct_track} != pre-config "
        f"{PRECONFIG_TRACK}), players={direct_players}, honored, no divergence, no "
        f"admission reject, gGameMode={boot_gamemode} gCurrentMenuId={boot_menu_id} "
        f"throughout -- engine entered the online rollback race "
        f"loadedTrack={online_race[0][0]} after {lobby_ticks} LOBBY_WAIT tick(s). "
        f"ROOM-READY GATE: the production room-ready gate routed the SINGLE-RACE room to the "
        f"native takeover (fires=1, route=lobby-start) -- single race now takes the "
        f"same descriptor-less native path as a tournament."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
