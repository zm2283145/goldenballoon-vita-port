#!/usr/bin/env python3
"""SINGLE-RACE replay re-cycle -- two single races back-to-back, IN-SESSION.

Where the tournament round-cycle (check_online_lobby_single_endpoint.py) proves
races 2..N re-cycle in ONE engine process for a TOURNAMENT, this lane proves the
SINGLE-RACE analog: after a single race the native RESULTS chooser's "MORE RACES"
options actually re-boot a fresh native race in the SAME engine session, natively
driven, with NO re-boot loop.

A single race's REMATCH keeps race_index (only a tournament advances it), so the
tournament "race_index advanced" re-cycle trigger never fires for a single race.
The single-race path adds a mode-aware OBSERVE-ONLY re-cycle to the resident coordinator
(main_app.cpp) plus a LOBBY_WAIT auto-start in the engine session (online_session.c)
for "Race Again":

  - CHANGE TRACK: the chooser commits CHANGE TRACK -> REMATCH -> the session
    re-fronts native TRACKSELECT -> the host re-locks a DIFFERENT track -> race 2
    boots on that new track. Proven by TWO boots on DIFFERENT tracks in one process.
  - RACE AGAIN: the chooser commits RACE AGAIN -> REMATCH -> the session re-enters
    LOBBY_WAIT and AUTO-PUBLISHES ready + host-START (keep-selection) so the room
    re-cycles with NO human input -> race 2 boots on the SAME track ("straight to a
    fresh RACE"). Proven by TWO boots on the SAME track in one process.
  - NO re-boot loop: the launcher re-cycle is OBSERVE-ONLY and fires exactly once
    per chooser commit -- the chooser seam is one-shot, so exactly TWO boots, never a
    third spurious re-boot. LEFT/ERROR never re-arms.
  - PEER DROP mid re-cycle: a leader CANCEL_LOADING during the re-cycle regresses
    the room to LOBBY; the engine's per-round re-wait mid-cancel unwind notes LEFT +
    exits(0) -- a CLEAN return to the room (no hang, no abort, no re-arm, no extra
    boot). This is the crash-fix guarantee, inherited by the single-race re-cycle.

Two loopback adapters stand in for the two processes (single-endpoint mode: the
LOCAL/host endpoint runs the native screens; a remote-sim drives peer B); the
resident coordinator is composed for a single race via MDKR_APP_TEST_ONLINE_SINGLE_
REPLAY (production runOnlineLobbyStartLiveSession always composes it; the plain
single-race lobby-start lane sets neither env, so it is byte-behaviour-unchanged).

The native TRACKSELECT lobby-start seam picks a track one grid row further down on
each re-entry (the cursor restores to the last locked track), so race 1 -> track 3
(Fossil Canyon) and the CHANGE TRACK race 2 -> track 29 (Jungle Falls): a genuine,
native-driven track change, not a re-config of the same track.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

from harness_utils import resolve_binary
from online_lane_util import (
    DIRECT_BOOT_RE, GAMEMODE_ONLINE_SESSION, SESSION_END_RE, SESSION_RACE_RE,
    forbidden_marker, make_fail, run_engine,
)

# race 1 native TRACKSELECT lock (grid index 1 == id 3, Fossil Canyon) and the
# CHANGE TRACK re-entry lock (index 2 == id 29, Jungle Falls) -- mirrored from
# online_trackselect.c sTrackIds[].
TRACK_RACE1 = 3
TRACK_CHANGED = 29

CHOOSER_COMMIT_RE = re.compile(
    r"^\[online-results\] chooser: committed option=(.+?) choice=(\d+) "
    r"intent\{rematch=1 mode=(\d+)\}$", re.MULTILINE)
SESSION_ROUTE_RE = re.compile(
    r"^\[online-session\] results -> (.+?) \(chooser: (.+?)\)", re.MULTILINE)
SINGLE_REPLAY_RE = re.compile(
    r"^\[online-resident-live\] single-race replay: room left RESULTS -> LOBBY "
    r"\(rematch\) -> observe-only re-cycle", re.MULTILINE)
SINGLE_ARM_RE = re.compile(
    r"^\[online-resident-live\] single race race-ready epoch=(\d+) active=(\w+) "
    r"frames=(\d+) \(observe-only re-cycle;", re.MULTILINE)
AUTOSTART_RE = re.compile(
    r"^\[online-session\] results -> re-race same config \(chooser: race again; "
    r"LIVE single-race re-cycle: LOBBY_WAIT auto-start armed\)", re.MULTILINE)
MID_UNWIND_RE = re.compile(
    r"^\[online-session\] mid-tournament UNWIND: room regressed to LOBBY "
    r"during the per-round re-wait", re.MULTILINE)
WEDGE_CANCEL_RE = re.compile(
    r"^\[online-resident-live\] WEDGE single-race re-cycle cancel:", re.MULTILINE)

fail = make_fail("single-race replay")


def _base_env(chooser: str) -> dict[str, str]:
    return {
        "MDKR_APP_TEST_ONLINE_LIVE_LOBBY_START": "1",
        "MDKR_APP_TEST_ONLINE_SINGLE_ENDPOINT": "1",
        "MDKR_APP_TEST_ONLINE_SINGLE_REPLAY": "1",
        "MDKR_TEST_ONLINE_LOBBY_START": "1",
        "MDKR_TEST_ONLINE_RESULTS_CHOOSER": chooser,
    }


def _isolation_ok(tag: str, output: str) -> int | None:
    marker = forbidden_marker(output, "online race admission rejected")
    if marker:
        return fail(f"[{tag}] forbidden marker {marker!r}", output)
    races = SESSION_RACE_RE.findall(output)
    if not races:
        return fail(f"[{tag}] no RACE hand-off (nothing booted)", output)
    for _t, gamemode, menu_id in races:
        if int(gamemode) != GAMEMODE_ONLINE_SESSION or int(menu_id) != 0:
            return fail(f"[{tag}] isolation broke: gGameMode={gamemode} "
                        f"gCurrentMenuId={menu_id}", output)
    return None


def _committed(output: str, want_option: str):
    for m in CHOOSER_COMMIT_RE.finditer(output):
        if m.group(1) == want_option:
            return m
    return None


def check_change_track(binary: Path, rom: Path, verbose: bool) -> int | None:
    """race 1 (track A) -> MORE RACES -> CHANGE TRACK -> track B -> race 2, both
    converging, native TRACKSELECT re-fronted, EXACTLY two boots (no loop)."""
    tag = "change-track"
    env = _base_env("single:1")  # single:1 == CHANGE TRACK (fires once)
    env["MDKR_ONLINE_SESSION_DESCLESS_WAIT_DEADLINE_MS"] = "600000"
    try:
        rc, out = run_engine(binary, rom, ticks=16000, timeout=400,
                             verbose=verbose, extra_env=env,
                             prefix="mdkr64-t5-change-track-")
    except subprocess.TimeoutExpired as error:
        return fail(f"[{tag}] run HUNG (the in-session re-cycle must be bounded): "
                    f"{error}")
    if rc != 0:
        return fail(f"[{tag}] process exited {rc} (expected a clean 0)", out)
    guard = _isolation_ok(tag, out)
    if guard is not None:
        return guard

    boots = [int(t) for t, _p in DIRECT_BOOT_RE.findall(out)]
    if boots != [TRACK_RACE1, TRACK_CHANGED]:
        return fail(f"[{tag}] expected EXACTLY two native boots on DIFFERENT tracks "
                    f"[{TRACK_RACE1}, {TRACK_CHANGED}] (race 1 -> MORE RACES -> change "
                    f"track -> race 2), got {boots}", out)
    commit = _committed(out, "CHANGE TRACK")
    if not commit or int(commit.group(3)) != 255:
        return fail(f"[{tag}] the host never committed CHANGE TRACK (rematch, no "
                    f"SET_MODE); saw {commit.group(0) if commit else None}", out)
    routes = [(m.group(1), m.group(2)) for m in SESSION_ROUTE_RE.finditer(out)]
    if ("trackselect", "change track") not in routes:
        return fail(f"[{tag}] the session did not re-front native TRACKSELECT on "
                    f"CHANGE TRACK; saw routes {routes}", out)
    if not SINGLE_REPLAY_RE.search(out):
        return fail(f"[{tag}] the launcher never ran the single-race observe-only "
                    f"re-cycle", out)
    arms = SINGLE_ARM_RE.findall(out)
    if len(arms) != 1:
        return fail(f"[{tag}] the observe-only re-cycle must re-arm the match-input "
                    f"EXACTLY once (one re-cycle -> two boots), saw {len(arms)}", out)
    return None


def check_race_again(binary: Path, rom: Path, verbose: bool) -> int | None:
    """race 1 -> MORE RACES -> RACE AGAIN -> race 2 SAME track, straight to a fresh
    RACE (LOBBY_WAIT auto-start, no human input), EXACTLY two boots (no loop)."""
    tag = "race-again"
    env = _base_env("single:0")  # single:0 == RACE AGAIN (fires once)
    env["MDKR_ONLINE_SESSION_DESCLESS_WAIT_DEADLINE_MS"] = "600000"
    try:
        rc, out = run_engine(binary, rom, ticks=16000, timeout=400,
                             verbose=verbose, extra_env=env,
                             prefix="mdkr64-t5-race-again-")
    except subprocess.TimeoutExpired as error:
        return fail(f"[{tag}] run HUNG: {error}")
    if rc != 0:
        return fail(f"[{tag}] process exited {rc} (expected a clean 0)", out)
    guard = _isolation_ok(tag, out)
    if guard is not None:
        return guard

    boots = [int(t) for t, _p in DIRECT_BOOT_RE.findall(out)]
    if boots != [TRACK_RACE1, TRACK_RACE1]:
        return fail(f"[{tag}] expected EXACTLY two native boots on the SAME track "
                    f"[{TRACK_RACE1}, {TRACK_RACE1}] (RACE AGAIN), got {boots}", out)
    commit = _committed(out, "RACE AGAIN")
    if not commit or int(commit.group(3)) != 255:
        return fail(f"[{tag}] the host never committed RACE AGAIN (rematch, no "
                    f"SET_MODE); saw {commit.group(0) if commit else None}", out)
    if not AUTOSTART_RE.search(out):
        return fail(f"[{tag}] RACE AGAIN did not arm the LOBBY_WAIT auto-start "
                    f"(straight to a fresh RACE)", out)
    if not SINGLE_REPLAY_RE.search(out):
        return fail(f"[{tag}] the launcher never ran the single-race observe-only "
                    f"re-cycle", out)
    arms = SINGLE_ARM_RE.findall(out)
    if len(arms) != 1:
        return fail(f"[{tag}] the observe-only re-cycle must re-arm EXACTLY once, "
                    f"saw {len(arms)}", out)
    return None


def check_peer_drop(binary: Path, rom: Path, verbose: bool) -> int | None:
    """A peer drop mid re-cycle (leader CANCEL_LOADING) -> the engine UNWINDS to a
    CLEAN LEFT return (exit 0), NO re-arm, NO extra boot, never a hang/abort."""
    tag = "peer-drop"
    env = _base_env("single:0")  # RACE AGAIN re-cycle, then cancel it mid-loading
    env["MDKR_APP_TEST_ONLINE_LOBBY_WEDGE"] = "cancel2"
    env["MDKR_ONLINE_SESSION_DESCLESS_WAIT_DEADLINE_MS"] = "6000"
    try:
        rc, out = run_engine(binary, rom, ticks=20000, timeout=400,
                             verbose=verbose, extra_env=env,
                             prefix="mdkr64-t5-peer-drop-")
    except subprocess.TimeoutExpired as error:
        return fail(f"[{tag}] run HUNG (a peer drop mid re-cycle must return "
                    f"cleanly, never park): {error}")
    marker = forbidden_marker(out, "online race admission rejected")
    if marker:
        return fail(f"[{tag}] forbidden marker {marker!r}", out)
    if rc != 0:
        return fail(f"[{tag}] exited {rc} (a peer drop mid re-cycle must be a CLEAN "
                    f"LEFT return, exit 0)", out)
    if not WEDGE_CANCEL_RE.search(out):
        return fail(f"[{tag}] the re-cycle cancel wedge never fired", out)
    if not MID_UNWIND_RE.search(out):
        return fail(f"[{tag}] the engine did NOT unwind the mid-re-cycle cancel "
                    f"(it would park with no clean return)", out)
    ends = SESSION_END_RE.findall(out)
    if not any(reason == "LEFT" and code == "0" for reason, code in ends):
        return fail(f"[{tag}] the launcher never read the LEFT session end "
                    f"(reason=LEFT result=0); saw {ends}", out)
    # No re-arm, no extra boot: only race 1 ran (the cancelled re-cycle never booted).
    boots = [int(t) for t, _p in DIRECT_BOOT_RE.findall(out)]
    if boots != [TRACK_RACE1]:
        return fail(f"[{tag}] a cancelled re-cycle must NOT boot a second race (no "
                    f"re-boot loop), got boots {boots}", out)
    return None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", default="build-beta")
    parser.add_argument("--rom", type=Path, default="baserom.us.v80.z64")
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    binary = Path(resolve_binary(args.build)).expanduser().resolve()
    rom = args.rom.expanduser().resolve()
    for path, label in ((binary, "binary"), (rom, "ROM")):
        if not path.is_file():
            parser.error(f"missing {label}: {path}")

    for scenario in (check_change_track, check_race_again, check_peer_drop):
        err = scenario(binary, rom, args.verbose)
        if err is not None:
            return err

    print(
        "PASS online single-race replay: two single races run BACK-TO-BACK in one "
        f"engine process -- CHANGE TRACK re-fronts native TRACKSELECT and boots race 2 "
        f"on a DIFFERENT track ({TRACK_RACE1} -> {TRACK_CHANGED}); RACE AGAIN "
        f"auto-starts (LOBBY_WAIT, no human input) straight into a fresh race on the "
        f"SAME track ({TRACK_RACE1} -> {TRACK_RACE1}); the observe-only re-cycle "
        "re-arms the match-input EXACTLY once per chooser commit (exactly two boots, "
        "no re-boot loop); and a peer drop mid re-cycle (leader CANCEL_LOADING) "
        "UNWINDS to a CLEAN LEFT return (exit 0, no re-arm, no extra boot, never a "
        "hang). gGameMode=2 gCurrentMenuId=0 throughout.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
