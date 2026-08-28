#!/usr/bin/env python3
"""Prove the native online HOST TRACK / CUP SELECT screen (PD-T3, Strategy D2).

Where check_online_charselect.py proves the FIRST player-facing screen of the
separated online path, THIS lane proves the SECOND (game/src/online/
online_trackselect.c), which re-implements the presentation with the game's OWN
per-world sky art + font in a two-stage layout (a world/cup banner strip + the
hovered world's full-width untruncated track list) instead of calling the offline
track-select _loop.

It stands up the same in-process two-adapter live session as the charselect lane
(real libdatachannel DTLS over the loopback hub) and runs TWO scenarios via the
MDKR_TEST_ONLINE_TRACKSELECT env value (with MDKR_TEST_ONLINE_CHARSELECT=1 so the
CHARSELECT seam installs the feed + scripts the character pick and then DEFERS its
self-start so the session hands off CHARSELECT -> TRACKSELECT on local-ready):

  * "1"      SINGLE-RACE HOST: first proves B -> CHARSELECT back (no wedge), then
             on re-entry locks Whale Bay (track 8, hovercraft-only 0x2) -- the R-A
             auto-narrow moves the seat off Car -- then browses AWAY to Spaceport
             Alpha (whose 2P mask drops hovercraft) to prove F-D5 (the publishable
             vehicle stays legal for the LOCKED track, not the hovered one), then
             starts. The reducer's ready-clear on the lock is followed by both
             seats reconverging to ready (R-B), the host starts, the race boots +
             converges byte-for-byte, all without the offline menu.
  * "joiner" TOURNAMENT JOINER: the local seat is a JOINER; the seam scripts a
             remote HOST locking cup 2 (Sherbet; round 0 == Whale Bay). Proves the
             joiner renders the ROOM snapshot (F-D3: host=0, snap.mode=TOURNAMENT,
             snap.cup=2) and auto-narrows its OWN vehicle to the cup's round-0
             track (F-I2) so BEGIN_LOADING is never refused, and the race boots.
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
    DIRECT_BOOT_RE, ENGINE_LIVE_RE, FORBIDDEN_ONLINE, GAMEMODE_ONLINE_SESSION,
    ONLINE_RACE_RE, SESSION_RACE_RE, forbidden_marker,
)
from online_lane_util import run_engine as _run_engine

ROOT = Path(__file__).resolve().parent.parent
TICKS = 3000

# The reducer-accepted set (kCupTracks in platform/online/lobby_core.c ==
# online_track_table.c == the screen's sTrackIds == test_online_lobby_core.c's
# literal pin), cup-major then round order. R-D: the screen's offered list must
# equal this exactly.
EXPECTED_TRACKS = [5, 3, 29, 7, 13, 6, 9, 28, 8, 4, 10, 30,
                   19, 18, 20, 31, 17, 32, 33, 15]
LOCKED_TRACK = 8      # Whale Bay (cup 2 round 0)
LOCKED_MASK = 0x2     # hovercraft-only
NARROWED_VEHICLE = 1  # VEHICLE_HOVERCRAFT
SPACEPORT_COL = 4     # Future Fun Land column
SPACEPORT_ROW = 3     # Spaceport Alpha (track 15) row
TOURN_CUP = 2         # Sherbet cup (round 0 == Whale Bay)

CS_ENTER_RE = re.compile(r"^\[online-charselect\] enter:", re.MULTILINE)
CS_LEAVE_STUB = "[online-charselect] leave requested"
TS_ENTER_RE = re.compile(
    r"^\[online-trackselect\] enter: native track select up entry=(\d+)",
    re.MULTILINE)
TS_TRACKS_RE = re.compile(r"^\[online-trackselect\] tracks: (.+)$",
                          re.MULTILINE)
TS_BACK_RE = re.compile(r"^\[online-trackselect\] back to charselect$",
                        re.MULTILINE)
TS_ADVANCE_RE = re.compile(
    r"^\[online-trackselect\] advance: lobby left LOBBY \(phase=(\d+)\)",
    re.MULTILINE)
TS_EXIT_RE = re.compile(
    r"^\[online-trackselect\] exit: freed world bg assets", re.MULTILINE)
SESS_CS_TO_TS_RE = re.compile(
    r"^\[online-session\] charselect -> trackselect", re.MULTILINE)
SESS_TS_TO_CS_RE = re.compile(
    r"^\[online-session\] trackselect -> charselect", re.MULTILINE)
TS_RENDER_RE = re.compile(
    r"^\[online-trackselect\] render mode=(\d+) col=(\d+) row=(\d+) host=(\d+) "
    r"track=(\d+) mask=0x([0-9a-f]+) vehicle=(\d+) locked\{track=(-?\d+) "
    r"cup=(-?\d+)\} seat\{r0=(\d+) r1=(\d+)\} snap\{mode=(\d+) cfgTrack=(\d+) "
    r"cup=(\d+) phase=(\d+)\} start=(\d+)$",
    re.MULTILINE)
# findall tuple indices:
#  0 mode 1 col 2 row 3 host 4 track 5 mask 6 vehicle 7 lockedTrack 8 lockedCup
#  9 r0 10 r1 11 snapMode 12 snapCfgTrack 13 snapCup 14 snapPhase 15 start

# PD-T4: the observable agreement check the session logs at the RACE hand-off.
# "honored" == the last-seen host-intended track (from the forward feed) equals
# the booted manifest track; "divergence" == they differ (the manifest still
# wins -- it is the peer-admission authority -- but we must never see it here).
TRACK_HONORED_RE = re.compile(
    r"^\[online-boot\] track honored: (\d+)$", re.MULTILINE)
TRACK_DIVERGENCE_RE = re.compile(
    r"^\[online-boot\] track divergence: snapshot=(\d+) manifest=(\d+)",
    re.MULTILINE)
# The loopback wiring's manifest-config witness (the LOCKED half): the leader
# froze the manifest via SET_CONFIG_TRACK (single) / SET_MODE+SET_CUP (tournament).
CONFIG_SINGLE_RE = re.compile(
    r"^\[online-live\] loopback config mode=single track=(\d+) "
    r"startMask=0x([0-9a-f]{2}) vehicle=(\d+)$", re.MULTILINE)
CONFIG_TOURNAMENT_RE = re.compile(
    r"^\[online-live\] loopback config mode=tournament cup=(\d+) "
    r"round1Track=(\d+) startMask=0x([0-9a-f]{2}) vehicle=(\d+)$", re.MULTILINE)

# M3 (PD-T4 carry-forward): the tournament joiner scenario drives a teardown-time
# transport continuation that logs "[online-tournament] result=error step=..." on
# any stall; treat it as fatal so a silently-degraded continuation can never pass.
FORBIDDEN_EXTRA = ("[online-tournament] result=error",)


def fail(scenario: str, message: str, output: str = "") -> int:
    print(f"FAIL online trackselect [{scenario}]: {message}", file=sys.stderr)
    if output:
        print(output[-16000:], file=sys.stderr)
    return 1


def run_engine(binary: Path, rom: Path, ts_value: str, ticks: int,
               timeout: int, verbose: bool,
               extra_env: dict[str, str] | None = None) -> tuple[int, str]:
    # The MDKR_TEST_ONLINE_* seams drive the in-engine SCREEN; a lane's extra_env
    # adds the per-scenario loopback session-config seams so the frozen manifest
    # equals the host's on-screen LOCK (single-race track / tournament cup) -- the
    # LOCKED==BOOTED proof.
    seams = {
        "MDKR_APP_TEST_ONLINE_LIVE": "1",
        "MDKR_TEST_ONLINE_CHARSELECT": "1",
        "MDKR_TEST_ONLINE_TRACKSELECT": ts_value,
    }
    if extra_env:
        seams.update(extra_env)
    return _run_engine(binary, rom, ticks=ticks, timeout=timeout,
                       verbose=verbose, extra_env=seams,
                       prefix="mdkr64-online-trackselect-")


def assert_locked_equals_booted(scn: str, output: str,
                                expected_track: int) -> int | None:
    """PD-T4: the host's LOCKED track is exactly what BOOTS. The session logs the
    manifest track as HONORED (never a divergence), the direct-boot witness fires
    on it, and the engine's rollback runtime loaded that same track -- so the
    manifest (peer-admission authority) carried the locked pick end to end."""
    if TRACK_DIVERGENCE_RE.findall(output):
        div = TRACK_DIVERGENCE_RE.findall(output)
        return fail(scn, f"a track divergence was logged {div!r} -- the booted "
                    f"manifest did not match the host-intended (locked) track",
                    output)
    honored = TRACK_HONORED_RE.findall(output)
    if len(honored) != 1:
        return fail(scn, f"expected exactly one [online-boot] track honored line, "
                    f"got {honored!r}", output)
    if int(honored[0]) != expected_track:
        return fail(scn, f"track honored={honored[0]}, expected {expected_track} "
                    f"(intended != manifest)", output)
    direct = DIRECT_BOOT_RE.findall(output)
    if not direct or int(direct[0][0]) != expected_track:
        return fail(scn, f"direct-boot witness track {direct!r}, expected "
                    f"{expected_track}", output)
    race = ONLINE_RACE_RE.findall(output)
    if not race or int(race[0][0]) != expected_track:
        return fail(scn, f"engine loadedTrack {race!r}, expected {expected_track} "
                    f"-- LOCKED ({expected_track}) != BOOTED", output)
    return None


def assert_race_converges(scn: str, output: str) -> int | None:
    """Shared: the session handed off to the race without the offline menu and
    the two endpoints converged byte-for-byte."""
    race = SESSION_RACE_RE.findall(output)
    if len(race) != 1:
        return fail(scn, f"expected exactly one session RACE hand-off, got "
                    f"{race!r}", output)
    _t, boot_gamemode, boot_menu_id = race[0]
    if int(boot_gamemode) != GAMEMODE_ONLINE_SESSION:
        return fail(scn, f"at hand-off gGameMode={boot_gamemode}, expected "
                    f"{GAMEMODE_ONLINE_SESSION}", output)
    if int(boot_menu_id) != 0:
        return fail(scn, f"gCurrentMenuId={boot_menu_id} at hand-off -- offline "
                    f"menu entered (expected 0)", output)
    if "input-script" in output or "race_2p_split" in output:
        return fail(scn, "a menu-nav input script was loaded", output)
    if not DIRECT_BOOT_RE.search(output):
        return fail(scn, "the race boot never fired after TRACKSELECT", output)
    stats = ENGINE_LIVE_RE.findall(output)
    if len(stats) != 1:
        return fail(scn, f"expected one ENGINE-ONLINE-LIVE witness, got "
                    f"{stats!r}", output)
    (result, raced, drains, advance_failed, _e, _a, _c, _d, fold_visible,
     _fp, hash_visible, hash_peer, converged) = stats[0]
    if int(result) != 0:
        return fail(scn, f"engine boot returned {result}", output)
    if int(advance_failed) != 0:
        return fail(scn, "the live match-input source failed to advance", output)
    if int(raced) < 100 or int(drains) < 100:
        return fail(scn, f"race did not sustain ticks (raced={raced} "
                    f"drains={drains})", output)
    if not (int(converged) == 1 and hash_visible == hash_peer and
            int(fold_visible) >= 20):
        return fail(scn, f"endpoints did not converge (converged={converged} "
                    f"fold={fold_visible} v={hash_visible} p={hash_peer})",
                    output)
    return None  # ok


def check_common(scn: str, rc: int, output: str) -> int | None:
    marker = forbidden_marker(output, *FORBIDDEN_ONLINE, *FORBIDDEN_EXTRA)
    if marker:
        return fail(scn, f"observed forbidden marker {marker!r}", output)
    if rc != 0:
        return fail(scn, f"process exited {rc}", output)
    if not CS_ENTER_RE.search(output):
        return fail(scn, "CHARSELECT was never entered (flow must pass through "
                    "charselect first)", output)
    if not SESS_CS_TO_TS_RE.search(output):
        return fail(scn, "CHARSELECT never handed off to TRACKSELECT", output)
    if not TS_ENTER_RE.search(output):
        return fail(scn, "TRACKSELECT was never entered", output)
    tracks_line = TS_TRACKS_RE.search(output)
    if not tracks_line:
        return fail(scn, "the screen never emitted its offered track-id list",
                    output)
    offered = [int(x) for x in tracks_line.group(1).split()]
    if offered != EXPECTED_TRACKS:
        return fail(scn, f"offered ids {offered} != reducer set {EXPECTED_TRACKS}",
                    output)
    if not TS_RENDER_RE.findall(output):
        return fail(scn, "no TRACKSELECT render witnesses", output)
    return None


def check_single_host(output: str) -> int | None:
    scn = "single-host"
    err = check_common(scn, 0, output)
    if err is not None:
        return err
    renders = TS_RENDER_RE.findall(output)

    # B -> CHARSELECT round-trip (no wedge): >=2 handoffs, a back log, and the
    # PD-T6 charselect leave stub EXACTLY once across the whole run.
    if len(SESS_CS_TO_TS_RE.findall(output)) < 2:
        return fail(scn, "expected CHARSELECT -> TRACKSELECT at least twice (the "
                    "B round-trip)", output)
    if max(int(e) for e in TS_ENTER_RE.findall(output)) < 2:
        return fail(scn, "TRACKSELECT entered < 2 times (B round-trip)", output)
    if not TS_BACK_RE.search(output):
        return fail(scn, "B on TRACKSELECT never returned to CHARSELECT", output)
    if not SESS_TS_TO_CS_RE.search(output):
        return fail(scn, "no TRACKSELECT -> CHARSELECT back transition", output)
    leave_stub = output.count(CS_LEAVE_STUB)
    if leave_stub != 1:
        return fail(scn, f"charselect leave stub logged {leave_stub} times "
                    f"(expected EXACTLY 1)", output)

    # F-I1 regression: the back-out keeps CHARSELECT for more than one tick (the
    # session gates the re-advance on the screen's OWN confirmed+ready latch, not
    # only the lagging snapshot). NOTE the synchronous seam cannot inject the live
    # reduce lag, so this guards no one-frame bounce; the async correctness is by
    # construction (the screen latch resets on _enter, independent of snapshot).
    cs_enters = len(CS_ENTER_RE.findall(output))
    if cs_enters < 2:
        return fail(scn, f"CHARSELECT re-entered {cs_enters}x; the back-out did "
                    f"not return to a fresh charselect", output)

    # Host lock reached the reducer: configured_track converged to 8.
    if not [r for r in renders if int(r[12]) == LOCKED_TRACK]:
        return fail(scn, f"snapshot configured_track never converged to "
                    f"{LOCKED_TRACK}", output)

    # Vehicle auto-narrow held for Whale Bay's 0x2 mask (R-A).
    narrow_rows = [r for r in renders
                   if int(r[4]) == LOCKED_TRACK and int(r[5], 16) == LOCKED_MASK]
    if not narrow_rows:
        return fail(scn, f"no render row resolved track {LOCKED_TRACK} mask "
                    f"0x{LOCKED_MASK:x}", output)
    if any(int(r[6]) != NARROWED_VEHICLE for r in narrow_rows):
        return fail(scn, f"vehicle did not auto-narrow to {NARROWED_VEHICLE}",
                    output)
    if any(((1 << int(r[6])) & LOCKED_MASK) == 0 for r in narrow_rows):
        return fail(scn, "a resolved vehicle bit was NOT inside the track mask",
                    output)

    # F-D5: after the lock, browsing to Spaceport Alpha (col4,row3, 2P mask drops
    # hovercraft) must NOT re-narrow off the LOCKED track's legal vehicle.
    spaceport_rows = [r for r in renders
                      if int(r[1]) == SPACEPORT_COL and int(r[2]) == SPACEPORT_ROW]
    if not spaceport_rows:
        return fail(scn, "cursor never browsed to Spaceport Alpha after locking "
                    "(F-D5 coverage would be silently skipped)", output)
    locked_while_browsing = [r for r in spaceport_rows if int(r[7]) == LOCKED_TRACK]
    if not locked_while_browsing:
        return fail(scn, "no Spaceport Alpha hover row while Whale Bay was locked",
                    output)
    if any(int(r[6]) != NARROWED_VEHICLE for r in locked_while_browsing):
        bad = [int(r[6]) for r in locked_while_browsing
               if int(r[6]) != NARROWED_VEHICLE]
        return fail(scn, f"F-D5: after locking Whale Bay, hovering Spaceport "
                    f"Alpha re-narrowed the published vehicle to {bad} (must stay "
                    f"{NARROWED_VEHICLE}, legal for the LOCKED track)", output)

    # Ready cleared then reconverged around the config change (R-B).
    cfg_rows = [r for r in renders if int(r[12]) == LOCKED_TRACK]
    if not [r for r in cfg_rows if int(r[9]) == 0 and int(r[10]) == 0]:
        return fail(scn, "never witnessed the ready-clear after the config "
                    "change", output)
    if not [r for r in cfg_rows if int(r[9]) == 1 and int(r[10]) == 1]:
        return fail(scn, "both seats never reconverged to ready after the "
                    "config-clear", output)

    if not TS_ADVANCE_RE.search(output):
        return fail(scn, "never advanced on the scripted host-start", output)
    if not TS_EXIT_RE.search(output):
        return fail(scn, "never freed world bg assets on exit", output)

    # PD-T4 LOCKED==BOOTED: the loopback froze the manifest to the SAME track the
    # screen locked (Whale Bay, 8), so the session sees snapshot==manifest, logs
    # HONORED (no divergence), and the engine boots exactly track 8.
    config = CONFIG_SINGLE_RE.findall(output)
    if len(config) != 1 or int(config[0][0]) != LOCKED_TRACK:
        return fail(scn, f"the loopback did not freeze the manifest to the locked "
                    f"track {LOCKED_TRACK} (single config witness={config!r})",
                    output)
    err = assert_race_converges(scn, output)
    if err is not None:
        return err
    return assert_locked_equals_booted(scn, output, LOCKED_TRACK)


def check_joiner(output: str) -> int | None:
    scn = "joiner"
    err = check_common(scn, 0, output)
    if err is not None:
        return err
    renders = TS_RENDER_RE.findall(output)

    # F-D3: the JOINER renders the ROOM snapshot -- host=0, and the mode + locked
    # cup come from the snapshot, not local state.
    joiner_rows = [r for r in renders if int(r[3]) == 0]
    if not joiner_rows:
        return fail(scn, "no render row with host=0 -- the joiner render path "
                    "never executed (F-D3)", output)
    room_rows = [r for r in joiner_rows
                 if int(r[11]) == 1 and int(r[13]) == TOURN_CUP]
    if not room_rows:
        return fail(scn, f"joiner never reflected the room snapshot "
                    f"(host=0, snap.mode=TOURNAMENT, snap.cup={TOURN_CUP}) -- "
                    f"F-D3 render-from-snapshot", output)

    # F-I2: the joiner auto-narrowed its OWN vehicle to the cup's round-0 track
    # (Whale Bay, hovercraft-only) so BEGIN_LOADING is never refused.
    if not [r for r in room_rows if int(r[6]) == NARROWED_VEHICLE]:
        return fail(scn, f"joiner never narrowed its vehicle to "
                    f"{NARROWED_VEHICLE} for the cup's round-0 track (F-I2)",
                    output)
    if "ILLEGAL_VEHICLE" in output:
        return fail(scn, "a BEGIN_LOADING ILLEGAL_VEHICLE refusal was observed "
                    "(F-I2 livelock)", output)

    if not TS_ADVANCE_RE.search(output):
        return fail(scn, "never advanced on the scripted host-start", output)

    # PD-T4 LOCKED==BOOTED: the loopback ran tournament cup 2, whose round-0 track
    # is Whale Bay (8) -- the SAME track the joiner's screen resolves the room's
    # locked cup to. The manifest froze that track, the session logs HONORED, and
    # the engine boots exactly track 8.
    config = CONFIG_TOURNAMENT_RE.findall(output)
    if (len(config) != 1 or int(config[0][0]) != TOURN_CUP or
            int(config[0][1]) != LOCKED_TRACK):
        return fail(scn, f"the loopback did not freeze the manifest to cup "
                    f"{TOURN_CUP} round-0 track {LOCKED_TRACK} (tournament config "
                    f"witness={config!r})", output)
    err = assert_race_converges(scn, output)
    if err is not None:
        return err
    return assert_locked_equals_booted(scn, output, LOCKED_TRACK)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", default="build-beta")
    parser.add_argument("--rom", type=Path, default="baserom.us.v80.z64")
    parser.add_argument("--ticks", type=int, default=TICKS)
    parser.add_argument("--timeout", type=int, default=300)
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    binary = Path(resolve_binary(args.build)).expanduser().resolve()
    rom = args.rom.expanduser().resolve()
    for path, label in ((binary, "binary"), (rom, "ROM")):
        if not path.is_file():
            parser.error(f"missing {label}: {path}")

    # PD-T4: each scenario aligns the frozen manifest with the on-screen LOCK so
    # the lane proves LOCKED==BOOTED. single-host fixes the manifest track to
    # Whale Bay (8) via SET_CONFIG_TRACK; the tournament joiner runs cup 2, whose
    # round-0 track is Whale Bay (8). The tournament seam also drives a teardown-
    # time transport continuation (rounds 2..4), so it gets a wider timeout.
    scenarios = (
        ("1", check_single_host,
         {"MDKR_APP_TEST_ONLINE_TRACK": str(LOCKED_TRACK)}, args.timeout),
        ("joiner", check_joiner,
         {"MDKR_APP_TEST_ONLINE_MODE": "tournament",
          "MDKR_APP_TEST_ONLINE_CUP": str(TOURN_CUP)}, max(args.timeout, 600)),
    )
    for ts_value, checker, extra_env, scn_timeout in scenarios:
        try:
            rc, output = run_engine(binary, rom, ts_value, args.ticks,
                                    scn_timeout, args.verbose, extra_env)
        except subprocess.TimeoutExpired as error:
            return fail(ts_value, f"engine run timed out: {error}")
        result = checker(output)
        if result is not None:
            return result

    print(
        "PASS online trackselect: two-stage native screen -- SINGLE-HOST "
        "(B->charselect no-wedge; locked Whale Bay -> configured_track converged; "
        "auto-narrow to hovercraft; F-D5 vehicle stays legal browsing Spaceport "
        "Alpha; ready clear->reconverge; host-start; LOCKED==BOOTED: manifest "
        "honored track 8, engine loadedTrack 8) and TOURNAMENT-JOINER (renders "
        "room snapshot host=0/mode=TOURNAMENT/cup=2; narrows to the cup round-0 "
        "track; no ILLEGAL_VEHICLE; LOCKED==BOOTED: cup-2 round-0 manifest honored "
        "track 8, engine loadedTrack 8) -- both handed off gGameMode=2 "
        "gCurrentMenuId=0, offered ids == reducer set, no track divergence"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
