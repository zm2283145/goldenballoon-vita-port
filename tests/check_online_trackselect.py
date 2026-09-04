#!/usr/bin/env python3
"""Prove the native online HOST TRACK / CUP SELECT screen.

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
self-start so the session hands the flow forward in the RETAIL order --
CHARSELECT -> TRACKSELECT (browse + lock) -> the VEHICLE stage of the track
screen -- on local-ready; the vehicle stage is scripted per scenario):

  * "1"      SINGLE-RACE HOST: first proves the browse B -> back one level to
             CHARSELECT (no wedge; charselect re-confirms and re-advances), then
             on re-entry locks Whale Bay (track 8, hovercraft-only 0x2) -- the
             lock flips straight into the VEHICLE stage (retail order), whose
             auto-narrow moves the seat off Car -- then the stage's B steps back
             to the browse (the intra-screen back-stack), the browse re-locks,
             the stage confirms + the host OKs. The reducer's ready-clear on the
             lock is followed by both seats reconverging to ready, the race boots
             + converges byte-for-byte, all without the offline menu.
  * "joiner" TOURNAMENT JOINER: the local seat is a JOINER; the seam scripts a
             remote HOST locking cup 2 (Sherbet; round 0 == Whale Bay). Proves the
             joiner renders the ROOM snapshot (host=0, snap.mode=TOURNAMENT,
             snap.cup=2), FOLLOWS the host's lock into the vehicle stage (the
             retail zoom-into-setup analog) and narrows its OWN vehicle to the
             cup so BEGIN_LOADING is never refused, and the race boots.
  * "rematch" SAME-TRACK REMATCH (joiner view): the room re-fronts carrying
             LAST round's config + persisted picks (REMATCH's clear_round drops
             only ready), so the host's re-lock of the IDENTICAL track never
             ready-clears in the reducer; the scripted host presses OK while the
             joiner is still inside its 70-tick stale-lock browse dwell. Proves
             the per-round CONFIRM contract: ready latches ONLY via the vehicle
             stage's confirm, so the OK is REFUSED (NOT_READY, re-fired) until
             the joiner's stage fronts + confirms, and only then does the race
             boot. The pre-fix screen republished ready=1 from the browse: the
             OK landed instantly and the joiner was race-booted without its
             vehicle stage ever fronting.
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
TS_BACK_RE = re.compile(r"^\[online-trackselect\] back one level$",
                        re.MULTILINE)
TS_ADVANCE_RE = re.compile(
    r"^\[online-trackselect\] advance: lobby left LOBBY \(phase=(\d+)\)",
    re.MULTILINE)
TS_EXIT_RE = re.compile(
    r"^\[online-trackselect\] exit: freed world bg assets", re.MULTILINE)
# RETAIL order: the track browse follows CHARSELECT, and the VEHICLE pick is a
# STAGE of the track screen entered on the lock; the stage B steps back to the
# browse, and the browse B steps back to CHARSELECT.
VS_ENTER_RE = re.compile(r"^\[online-vehicleselect\] enter:", re.MULTILINE)
VS_ADVANCE_RE = re.compile(
    r"^\[online-vehicleselect\] advance: lobby left LOBBY \(phase=(\d+)\)",
    re.MULTILINE)
VS_BACK_RE = re.compile(
    r"^\[online-vehicleselect\] back to track browse$", re.MULTILINE)
SESS_CS_TO_TS_RE = re.compile(
    r"^\[online-session\] charselect -> trackselect", re.MULTILINE)
SESS_TS_TO_CS_RE = re.compile(
    r"^\[online-session\] trackselect -> charselect \(back one level\)",
    re.MULTILINE)
SESS_TS_TO_VS_RE = re.compile(
    r"^\[online-session\] trackselect -> vehicleselect \(track locked",
    re.MULTILINE)
# The STAY+lock flip's exit-fade witness (P1 fix): veilOnLock=1 means a lock landed
# INSIDE the 18-tick exit-fade hold; veil= is the resulting latch, which must be
# "clear" (revealed away), never "STRANDED" (left black over the vehicle stage).
SESS_TS_TO_VS_FADE_RE = re.compile(
    r"^\[online-session\] trackselect -> vehicleselect \(track locked: retail "
    r"vehicle stage; veilOnLock=(\d+) veil=(\w+)\)$", re.MULTILINE)
SESS_VS_TO_TS_RE = re.compile(
    r"^\[online-session\] vehicleselect -> trackselect \(back one stage\)",
    re.MULTILINE)
VS_RENDER_RE = re.compile(
    r"^\[online-vehicleselect\] render cursor=(\d+) vehicle=(\d+) "
    r"legal=0x([0-9a-f]+) track=(\d+) local\{seatVeh=(\d+) seatReady=(\d+) "
    r"conf=(\d+)\} remote\{seat=(-?\d+) veh=(\d+) ready=(\d+) name=(\S+)\} "
    r"intent\{vehicle=(\d+) ready=(\d+)\}$", re.MULTILINE)
TS_RENDER_RE = re.compile(
    r"^\[online-trackselect\] render mode=(\d+) col=(\d+) row=(\d+) host=(\d+) "
    r"track=(\d+) mask=0x([0-9a-f]+) vehicle=(\d+) locked\{track=(-?\d+) "
    r"cup=(-?\d+)\} seat\{r0=(\d+) r1=(\d+)\} snap\{mode=(\d+) cfgTrack=(\d+) "
    r"cup=(\d+) phase=(\d+)\} setup=(\d+)$",
    re.MULTILINE)
# findall tuple indices:
#  0 mode 1 col 2 row 3 host 4 track 5 mask 6 vehicle 7 lockedTrack 8 lockedCup
#  9 r0 10 r1 11 snapMode 12 snapCfgTrack 13 snapCup 14 snapPhase 15 setup

# The grouped track LIST's a11y annotations (semantic witnesses, change-detected
# and therefore bounded): the HOST's hovered group + track (+ its legal vehicles
# by name), and every effective LOCK -- host and joiner alike (the joiner's line
# is the feed-followed lock, its "what is being browsed" announcement). The
# native screens' self-voicing is the retail T.T. track announcer (audio); these
# lines are the assertable text half of the same contract, in the lobby's
# established content-witness pattern.
TS_A11Y_HOVER_RE = re.compile(
    r"^\[online-trackselect\] a11y hover: (.+) vehicles=([A-Z+]+)$",
    re.MULTILINE)
TS_A11Y_LOCK_RE = re.compile(
    r"^\[online-trackselect\] a11y locked: (.+) vehicles=([A-Z+]+)$",
    re.MULTILINE)

# the observable agreement check the session logs at the RACE hand-off.
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

# The SAME-TRACK REMATCH scenario's refusal machinery witnesses: the scripted
# host's OK before the joiner's stage confirm is refused NOT_READY and re-fires
# (the planner refusal-note cadence), converging once the confirm lands.
TS_REMATCH_REFUSED_RE = re.compile(
    r"^\[online-trackselect\] test-script host OK refused NOT_READY "
    r"\(joiner not stage-confirmed; re-fire armed\)$", re.MULTILINE)
TS_REMATCH_CONVERGED_RE = re.compile(
    r"^\[online-trackselect\] test-script host OK converged after (\d+) refused "
    r"tick\(s\) \(joiner stage-confirmed\) -> LOADING$", re.MULTILINE)

# carry-forward: the tournament joiner scenario drives a teardown-time
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
    """The host's LOCKED track is exactly what BOOTS. The session logs the
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
        return fail(scn, "CHARSELECT never handed off to TRACKSELECT (retail "
                    "order: the track browse follows PLAYER SELECT)", output)
    if not TS_ENTER_RE.search(output):
        return fail(scn, "TRACKSELECT was never entered", output)
    if not SESS_TS_TO_VS_RE.search(output):
        return fail(scn, "the track lock never handed off to the VEHICLE stage "
                    "(retail order: vehicles AFTER the track)", output)
    if not VS_ENTER_RE.search(output):
        return fail(scn, "the VEHICLE stage was never entered", output)
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
    vs_renders = VS_RENDER_RE.findall(output)

    # Browse B round-trip (no wedge): the browse B steps back ONE level to
    # CHARSELECT (whose seam re-confirms + re-readies and hands back), so
    # CHARSELECT -> TRACKSELECT fires at least twice, TRACKSELECT is entered at
    # least twice, and the charselect leave stub fires EXACTLY twice (each
    # charselect entry replays its browse-B script once).
    if not TS_BACK_RE.search(output):
        return fail(scn, "B on the track browse never stepped back one level",
                    output)
    if not SESS_TS_TO_CS_RE.search(output):
        return fail(scn, "no TRACKSELECT -> CHARSELECT back transition", output)
    if len(SESS_CS_TO_TS_RE.findall(output)) < 2:
        return fail(scn, "expected CHARSELECT -> TRACKSELECT at least twice (the "
                    "browse B round-trip)", output)
    # The charselect leave stub is warn-ONCE per session continuation:
    # the TRACKSELECT -> CHARSELECT back-out deliberately does NOT reset the
    # latch (continuation of the same session), so the second entry's scripted
    # browse-B stays silent -- EXACTLY one stub across the whole run.
    leave_stub = output.count(CS_LEAVE_STUB)
    if leave_stub != 1:
        return fail(scn, f"charselect leave stub logged {leave_stub} times "
                    f"(expected EXACTLY 1: warn-once across the session)", output)

    # Stage back-stack (retail: B at the setup stage returns to the browse):
    # the vehicle stage's B stepped back to the browse, the browse RE-LOCKED (a
    # fresh lock is required -- the setup latch resets on _enter, the F-I1
    # no-bounce discipline), and the stage re-entered.
    if not VS_BACK_RE.search(output):
        return fail(scn, "the vehicle stage's B never stepped back to the "
                    "browse", output)
    if not SESS_VS_TO_TS_RE.search(output):
        return fail(scn, "no vehicle-stage -> browse back transition", output)
    if len(SESS_TS_TO_VS_RE.findall(output)) < 2:
        return fail(scn, "the browse never re-locked back into the vehicle stage "
                    "(lock -> stage must fire at least twice)", output)
    if max(int(e) for e in TS_ENTER_RE.findall(output)) < 3:
        return fail(scn, "TRACKSELECT entered < 3 times (charselect round-trip + "
                    "stage round-trip)", output)
    vs_enters = len(VS_ENTER_RE.findall(output))
    if vs_enters < 2:
        return fail(scn, f"the vehicle stage re-entered {vs_enters}x; the "
                    f"back-out did not return through a fresh lock", output)

    # The lock flipped the browse into the stage (the setup latch was witnessed).
    if not [r for r in renders if int(r[15]) == 1]:
        return fail(scn, "no browse render row witnessed setup=1 (the lock -> "
                    "stage latch)", output)

    # A11Y annotations (the grouped-list contract): the entry cell announces
    # its group + track + legal vehicles, the walk announces Whale Bay as
    # HOVERCRAFT-only at 2 players (the mask truth, spoken), and the lock
    # announces itself.
    hovers = TS_A11Y_HOVER_RE.findall(output)
    if not any("ANCIENT LAKE" in h[0] for h in hovers):
        return fail(scn, "no a11y hover announcement for the entry cell "
                    "(DINO DOMAIN / ANCIENT LAKE)", output)
    if not any("WHALE BAY" in h[0] and h[1] == "HOVERCRAFT" for h in hovers):
        return fail(scn, "no a11y hover announcement naming WHALE BAY as "
                    "HOVERCRAFT-only", output)
    if not any("WHALE BAY" in l[0] for l in TS_A11Y_LOCK_RE.findall(output)):
        return fail(scn, "no a11y lock announcement for the Whale Bay lock",
                    output)

    # Host lock reached the reducer: the VEHICLE stage resolved the locked track
    # (the browse exits on the lock tick, so convergence is read on the stage).
    narrow_rows = [r for r in vs_renders
                   if int(r[3]) == LOCKED_TRACK and int(r[2], 16) == LOCKED_MASK]
    if not narrow_rows:
        return fail(scn, f"no vehicle-stage render row resolved track "
                    f"{LOCKED_TRACK} mask 0x{LOCKED_MASK:x} (the lock never "
                    f"converged)", output)

    # Vehicle auto-narrow held for Whale Bay's 0x2 mask (R-A): the stage moved
    # the seat off the CAR charselect default and published only hovercraft.
    if any(int(r[1]) != NARROWED_VEHICLE or int(r[11]) != NARROWED_VEHICLE
           for r in narrow_rows):
        return fail(scn, f"the stage's pick did not auto-narrow to "
                    f"{NARROWED_VEHICLE} on the locked track", output)

    # Ready cleared then reconverged around the config change (R-B): the lock's
    # ready-clear leaves the stage un-confirmed (seatReady=0), then the confirm +
    # remote republish reconverge both (seatReady=1, remote ready=1).
    if not [r for r in narrow_rows if int(r[5]) == 0]:
        return fail(scn, "never witnessed the ready-clear after the config "
                    "change (no un-ready stage row)", output)
    if not [r for r in narrow_rows
            if int(r[5]) == 1 and int(r[6]) == 1 and int(r[9]) == 1]:
        return fail(scn, "both seats never reconverged to ready after the "
                    "config-clear", output)

    # The room left LOBBY under the VEHICLE stage (the host's OK), not the browse.
    if not VS_ADVANCE_RE.search(output):
        return fail(scn, "the vehicle stage never advanced on the host OK",
                    output)
    if not TS_EXIT_RE.search(output):
        return fail(scn, "never freed world bg assets on exit", output)

    # LOCKED==BOOTED: the loopback froze the manifest to the SAME track the
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
                    f"F-D3 render-from-snapshot; the joiner must witness the "
                    f"host's lock on the browse before following it into the "
                    f"vehicle stage", output)

    # A11Y annotation on the JOINER: the feed-followed lock is announced (the
    # non-interactive list's "what is being browsed" line -- the joiner has no
    # cursor, so hover lines are host-only, but the host's cup lock must speak).
    if not any("SHERBET" in l[0]
               for l in TS_A11Y_LOCK_RE.findall(output)):
        return fail(scn, "the joiner never announced the host's cup lock "
                    "(a11y locked: SHERBET CUP ...)", output)

    # F-I2: the joiner narrowed its OWN vehicle to the cup (hovercraft-only
    # intersection) so BEGIN_LOADING is never refused. The joiner FOLLOWS the
    # host's lock into the vehicle stage (retail zoom-into-setup analog), so the
    # narrow is read on the stage's renders.
    vs_renders = VS_RENDER_RE.findall(output)
    if not [r for r in vs_renders
            if int(r[1]) == NARROWED_VEHICLE and int(r[11]) == NARROWED_VEHICLE]:
        return fail(scn, f"joiner's vehicle stage never narrowed the pick to "
                    f"{NARROWED_VEHICLE} for the locked cup (F-I2)", output)
    if "ILLEGAL_VEHICLE" in output:
        return fail(scn, "a BEGIN_LOADING ILLEGAL_VEHICLE refusal was observed "
                    "(F-I2 livelock)", output)

    if not VS_ADVANCE_RE.search(output):
        return fail(scn, "the joiner's vehicle stage never advanced on the "
                    "scripted host-start", output)

    # LOCKED==BOOTED: the loopback ran tournament cup 2, whose round-0 track
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


def check_rematch(output: str) -> int | None:
    """SAME-TRACK rematch re-front, joiner view: the per-player CONFIRM cannot
    be bypassed. The identical-track re-lock never ready-clears in the reducer
    (no config change), so ONLY the per-round stage-confirm contract keeps the
    scripted host's early OK refused until the joiner's vehicle stage fronts
    and confirms; the re-fired OK then converges and the race boots."""
    scn = "rematch"
    marker = forbidden_marker(output, *FORBIDDEN_ONLINE, *FORBIDDEN_EXTRA)
    if marker:
        return fail(scn, f"observed forbidden marker {marker!r}", output)

    # The boot itself must happen (the deferral must never wedge the flow) --
    # checked FIRST so the bypass regression below reads unambiguously.
    boot = DIRECT_BOOT_RE.search(output)
    if not boot:
        return fail(scn, "the race never booted (the deferred OK never "
                    "converged -- the rematch flow wedged)", output)

    # THE FINDING: the joiner's vehicle stage must front before the boot. On
    # the pre-fix screen the browse's unconditional ready=1 republish re-latched
    # the stale ready (char+vehicle persist; the same-track re-lock never
    # ready-clears), the host's OK landed instantly, and the race booted with
    # NO vehicle-stage witness.
    vs = VS_ENTER_RE.search(output)
    if vs is None:
        return fail(scn, "the race BOOTED but the joiner's VEHICLE stage never "
                    "fronted -- the per-player CONFIRM was bypassed on the "
                    "same-track rematch (stale browse ready accepted "
                    "BEGIN_LOADING)", output)
    if boot.start() < vs.start():
        return fail(scn, "the race booted BEFORE the joiner's vehicle stage "
                    "fronted", output)
    if TS_ADVANCE_RE.search(output):
        return fail(scn, "the room left LOBBY under the BROWSE (the joiner was "
                    "race-booted from the browse; the OK was never deferred)",
                    output)

    # The refusal/re-fire machinery absorbed the interim: the host's early OK
    # was refused NOT_READY BEFORE the stage fronted, then the re-fired OK
    # converged once the joiner's stage confirm landed.
    refused = TS_REMATCH_REFUSED_RE.search(output)
    if refused is None:
        return fail(scn, "the scripted host's early OK was never refused "
                    "NOT_READY (the joiner's ready re-latched from the browse)",
                    output)
    converged = TS_REMATCH_CONVERGED_RE.search(output)
    if converged is None:
        return fail(scn, "the refused OK never converged after the joiner's "
                    "stage confirm", output)
    if not (refused.start() < vs.start() < converged.start()):
        return fail(scn, "refusal -> stage front -> converged OK ordering was "
                    "violated", output)
    if int(converged.group(1)) < 1:
        return fail(scn, "the OK converged with zero refused ticks (the "
                    "deferral window never existed)", output)

    # Flow shape + the joiner's browse deferral rows: while the stale lock
    # lingered (snapCfgTrack == the re-locked track) the LOCAL seat stayed
    # un-ready (r0=0) in the browse (setup=0) -- ready no longer co-occurs with
    # the browse's transient state; it latches only at the stage confirm.
    if not SESS_CS_TO_TS_RE.search(output):
        return fail(scn, "no CHARSELECT -> TRACKSELECT hand-off", output)
    if not SESS_TS_TO_VS_RE.search(output):
        return fail(scn, "the dwell never followed the stale lock into the "
                    "vehicle stage", output)
    renders = TS_RENDER_RE.findall(output)
    defer_rows = [r for r in renders
                  if int(r[3]) == 0 and int(r[12]) == LOCKED_TRACK and
                  int(r[15]) == 0 and int(r[9]) == 0]
    if not defer_rows:
        return fail(scn, f"no browse render row witnessed the deferral state "
                    f"(host=0 cfgTrack={LOCKED_TRACK} setup=0 r0=0)", output)

    # The joiner genuinely confirmed on the stage (conf=1), and the stage owned
    # the advance out of LOBBY.
    vs_renders = VS_RENDER_RE.findall(output)
    if not [r for r in vs_renders if int(r[6]) == 1]:
        return fail(scn, "no vehicle-stage render row with conf=1 (the joiner "
                    "never stage-confirmed)", output)
    if not VS_ADVANCE_RE.search(output):
        return fail(scn, "the vehicle stage never advanced on the converged OK",
                    output)

    # The race then boots + converges on the re-locked track, exactly once.
    err = assert_race_converges(scn, output)
    if err is not None:
        return err
    return assert_locked_equals_booted(scn, output, LOCKED_TRACK)


def check_lockfade(output: str) -> int | None:
    """LOCK-IN-FADE regression (P1 fix): the host arms the retail exit fade with a
    browse-B, then LOCKS the track while the 18-tick veil is still held. The
    session's STAY+lock branch must CANCEL the armed veil -- else it strands black
    over the vehicle stage and poisons the next vehicle->trackselect B-back. The
    flip witness reports veilOnLock (a lock DID land inside the hold) and the
    resulting latch, which must be 'clear', never 'STRANDED'."""
    scn = "lockfade"
    err = check_common(scn, 0, output)
    if err is not None:
        return err

    flips = SESS_TS_TO_VS_FADE_RE.findall(output)
    if not flips:
        return fail(scn, "no trackselect -> vehicleselect flip carried the "
                    "exit-fade witness (veilOnLock/veil)", output)
    # The arm MUST have reproduced the defect path: at least one lock landed with
    # the veil still armed. Without this the scenario would pass vacuously.
    if not any(int(v) == 1 for v, _ in flips):
        return fail(scn, f"the lock never landed inside the exit-fade hold "
                    f"(no veilOnLock=1 flip) -- the B-then-A-in-18-ticks arm did "
                    f"not reproduce; flips={flips!r}", output)
    # THE FINDING: a lock inside the hold must reveal the veil away, never strand
    # it. Pre-fix the STAY+lock branch flipped fade-skipped with exitFadeArmed
    # latched, so the FADE_STAY veil was never ended (veil=STRANDED).
    stranded = [f for f in flips if f[1] != "clear"]
    if stranded:
        return fail(scn, f"a lock inside the exit-fade hold left the veil "
                    f"STRANDED (veil never revealed away over the vehicle stage; "
                    f"stranded flips={stranded!r})", output)

    # The veil never poisoned the flow: it still boots + converges on the locked
    # track, exactly like the clean single-host lane.
    err = assert_race_converges(scn, output)
    if err is not None:
        return err
    return assert_locked_equals_booted(scn, output, LOCKED_TRACK)


def check_freshness(output: str) -> int | None:
    """A11Y RE-ANNOUNCE FRESHNESS (change-detect key fix): the host parks on
    Spaceport Alpha -- whose 2-player vehicle mask DROPS hovercraft -- and a rival
    JOINS then LEAVES while the cursor is untouched. The a11y hover witness must
    re-announce the vehicles= list on the occupied change ALONE. Pre-fix the
    TS_NONE lock sentinels (lockedTrack<<6 / lockedCup<<11) saturated the
    occupied/host/focus bits of the change-detect key in the common no-lock state,
    so a parked rival join/leave narrowed the chips visually but never re-spoke:
    exactly ONE Spaceport announcement, one vehicles= value."""
    scn = "freshness"
    marker = forbidden_marker(output, *FORBIDDEN_ONLINE, *FORBIDDEN_EXTRA)
    if marker:
        return fail(scn, f"observed forbidden marker {marker!r}", output)
    if not TS_ENTER_RE.search(output):
        return fail(scn, "TRACKSELECT was never entered", output)

    hovers = TS_A11Y_HOVER_RE.findall(output)
    # Ordered vehicles= values announced for the PARKED Spaceport Alpha cell (its
    # FUTURE FUN group prefix disambiguates it from Spacedust Alley).
    spaceport = [h[1] for h in hovers if "SPACEPORT" in h[0]]
    if not spaceport:
        return fail(scn, "no a11y hover announcement for the parked SPACEPORT "
                    "ALPHA cell -- the walk never reached it", output)

    # THE FINDING: a parked rival join AND leave must EACH re-fire the hover line
    # (>= 3 announcements: rival absent -> joined -> left) with the vehicles= list
    # reflecting the live occupied count. Pre-fix the saturated key froze it at the
    # entry value -> exactly one line, one distinct value.
    if len(spaceport) < 3:
        return fail(scn, f"the parked SPACEPORT ALPHA cell announced "
                    f"{len(spaceport)} time(s) ({spaceport!r}); a rival join AND "
                    f"leave with the cursor parked must EACH re-announce (>= 3) -- "
                    f"the change-detect key aliased the occupied bit into the lock "
                    f"sentinels", output)
    if len(set(spaceport)) < 2:
        return fail(scn, f"the parked SPACEPORT ALPHA vehicles= list never "
                    f"changed across the rival join/leave ({spaceport!r}); the "
                    f"2-player mask (hovercraft dropped) was never re-spoken",
                    output)
    # The narrowing is real mask truth: the 1-player announcement lists HOVERCRAFT,
    # the 2-player one drops it -- both must appear.
    if not any("HOVERCRAFT" in v for v in spaceport):
        return fail(scn, f"no Spaceport announcement listed HOVERCRAFT (the "
                    f"1-player mask) -- the arm never exercised the drop "
                    f"({spaceport!r})", output)
    if not any("HOVERCRAFT" not in v for v in spaceport):
        return fail(scn, f"every Spaceport announcement listed HOVERCRAFT -- the "
                    f"2-player narrow (drop hovercraft) never re-announced "
                    f"({spaceport!r})", output)
    return None


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

    # each scenario aligns the frozen manifest with the on-screen LOCK so
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
        # SAME-TRACK rematch: the manifest is pinned to the SAME track the stale
        # room config carries (Whale Bay), so the identical-track re-lock is a
        # genuine no-config-change re-front (no reducer ready-clear).
        ("rematch", check_rematch,
         {"MDKR_APP_TEST_ONLINE_TRACK": str(LOCKED_TRACK)}, args.timeout),
        # LOCK-IN-FADE: host, single-race, identical machinery to single-host; the
        # entry-1 script arms the exit fade (browse-B) then locks the track inside
        # the 18-tick veil hold. Manifest pinned to the same Whale Bay (8).
        ("lockfade", check_lockfade,
         {"MDKR_APP_TEST_ONLINE_TRACK": str(LOCKED_TRACK)}, args.timeout),
        # A11Y-FRESHNESS: host parks on Spaceport Alpha, a rival joins then leaves
        # with the cursor untouched; the a11y hover must re-announce the vehicles=
        # list on the occupied change alone (no lock, no boot -- pure browse arm).
        ("freshness", check_freshness, {}, args.timeout),
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
        "PASS online trackselect: retail-order track screen -- SINGLE-HOST "
        "(browse B -> CHARSELECT no-wedge; locked Whale Bay -> the VEHICLE stage "
        "fronted on the lock; stage B -> browse -> re-lock -> stage; the stage "
        "auto-narrowed to hovercraft and converged; ready clear->reconverge; "
        "host OK started; LOCKED==BOOTED: manifest honored track 8, engine "
        "loadedTrack 8) and TOURNAMENT-JOINER (renders room snapshot "
        "host=0/mode=TOURNAMENT/cup=2 on the browse, follows the lock into the "
        "vehicle stage, narrows to the cup; no ILLEGAL_VEHICLE; LOCKED==BOOTED: "
        "cup-2 round-0 manifest honored track 8, engine loadedTrack 8) and "
        "SAME-TRACK-REMATCH (identical-track re-lock never ready-clears; the "
        "host's early OK was refused NOT_READY and re-fired until the joiner's "
        "vehicle stage fronted + confirmed, then booted + converged -- the "
        "per-player CONFIRM cannot be bypassed) and LOCK-IN-FADE (a browse-B arms "
        "the exit fade, then A locks the track inside the 18-tick veil hold: the "
        "STAY+lock branch cancelled the armed veil -- veilOnLock=1 veil=clear, "
        "never STRANDED -- and the flow still booted + converged) and "
        "A11Y-FRESHNESS (host parked on Spaceport Alpha; a rival join AND leave "
        "each re-announced the vehicles= list on the occupied change alone, with "
        "the 2-player hovercraft drop spoken -- the change-detect key no longer "
        "aliases the occupied bit into the lock sentinels) -- all "
        "handed off gGameMode=2 gCurrentMenuId=0, offered ids == reducer set, "
        "no track divergence"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
