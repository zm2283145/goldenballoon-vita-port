#!/usr/bin/env python3
"""Prove the native online CHARACTER SELECT screen (PD-T2, Strategy D2).

Where check_online_session_boot.py proves the separated boot path idles in
LOBBY_WAIT and hands off to the race, THIS lane proves the first player-facing
SCREEN of that path: the session enters its CHARSELECT phase, renders the real
racer portraits + font natively (game/src/online/online_charselect.c, which
re-implements the presentation instead of calling the offline charselect _loop),
drives the LOCAL cursor, publishes the local INTENT continuously over the
party_link reverse feed, renders BOTH seats from the forward-feed snapshot (the
remote seat display-only, including its name), and advances when the authoritative
lobby leaves LOBBY (scripted host-start).

It stands up the same in-process two-adapter live session as the session-boot
lane (real libdatachannel DTLS over the loopback hub, roster + launch descriptor
installed from the vote), but sets MDKR_TEST_ONLINE_CHARSELECT=1. That engine-side
seam (beta + env gated, inert otherwise) stands in for the launcher: it installs
the REAL party_link forward feed, publishes a scripted 2-seat LOBBY room (local
seat 0; a scripted remote "RIVAL" who has already locked Bumper and readied), then
each CHARSELECT tick acts as a minimal reducer -- it polls the screen's published
intent, converges the local seat to it, and flips the lobby to LOADING once both
seats are ready. So the screen's cursor -> intent -> reducer -> snapshot -> render
round trip is genuinely closed, headless.

The scripted cursor walks to Pipsy (online char id 2), confirms, then readies.

Assertions:
  * the CHARSELECT screen was entered + assets loaded ([online-charselect] enter)
  * the scripted forward feed installed ([online-charselect] test-script install)
  * the REMOTE seat rendered from the snapshot (a render row with the remote's
    char + ready + name RIVAL) -- display-only witness
  * the published INTENT reached character=2 + the default vehicle + ready=1, and
    that default vehicle equals the one enter() computed and is mask-legal (0..2)
  * the LOCAL seat converged and rendered (a render row seatChar=2 seatReady=1) --
    proof the round-trip completed
  * the phase advanced on the scripted host-start ([online-charselect] advance)
  * assets were freed on exit ([online-charselect] exit)
  * the session handed off to the race WITHOUT the offline menu (gGameMode=2,
    gCurrentMenuId=0) and the race still boots + converges byte-for-byte
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
    SESSION_RACE_RE, forbidden_marker, make_fail, run_engine,
)

ROOT = Path(__file__).resolve().parent.parent
TICKS = 3000

ENTER_RE = re.compile(
    r"^\[online-charselect\] enter: native screen up defaultVehicle=(\d+)",
    re.MULTILINE)
INSTALL_RE = re.compile(
    r"^\[online-charselect\] test-script install", re.MULTILINE)
RENDER_RE = re.compile(
    r"^\[online-charselect\] render cursor=(\d+) name=(\S+) portrait=(\d+) "
    r"local\{conf=(\d+) ready=(\d+) seatChar=(\d+) seatReady=(\d+)\} "
    r"remote\{seat=(-?\d+) char=(\d+) ready=(\d+) name=(\S+)\} "
    r"taken=(-?\d+) dim=(\d+) "
    r"intent\{hover=(\d+) vehicle=(\d+) confirmed=(\d+) ready=(\d+)\}$",
    re.MULTILINE)
# Named groups of RENDER_RE (1-based -> 0-based tuple index in findall):
#  0 cursor  1 name  2 portrait  3 conf  4 ready(latch)  5 seatChar  6 seatReady
#  7 remoteSeat  8 remoteChar  9 remoteReady  10 remoteName
#  11 takenTile 12 takenDim
#  13 hover  14 vehicle  15 confirmed  16 ready(intent)
LEAVE_STUB = "[online-charselect] leave requested"
ADVANCE_RE = re.compile(
    r"^\[online-charselect\] advance: lobby left LOBBY \(phase=(\d+)\)",
    re.MULTILINE)
EXIT_RE = re.compile(
    r"^\[online-charselect\] exit: freed portrait assets", re.MULTILINE)

TARGET_CHARACTER = 2    # Pipsy, in the online id space (== grid cell == hover)
TARGET_PORTRAIT = 8     # sOnlineToPortrait[2] == gRacerPortraits[8] (Pipsy)
REMOTE_CHARACTER = 5    # Bumper, the scripted remote pick
REMOTE_NAME = "RIVAL"


fail = make_fail("charselect")


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

    # The CHARSELECT seam under test: a scripted 2-seat LOBBY room + a minimal
    # launcher reducer, all engine-side, so the native screen's cursor/intent/render
    # round-trip is exercised headless.
    extra_env = {
        "MDKR_APP_TEST_ONLINE_LIVE": "1",
        "MDKR_TEST_ONLINE_CHARSELECT": "1",
    }
    try:
        returncode, output = run_engine(
            binary, rom, ticks=args.ticks, timeout=args.timeout,
            verbose=args.verbose, extra_env=extra_env,
            prefix="mdkr64-online-charselect-")
    except subprocess.TimeoutExpired as error:
        return fail(f"engine run timed out (a CHARSELECT stall would look "
                    f"like this): {error}")

    marker = forbidden_marker(output, *FORBIDDEN_ONLINE)
    if marker:
        return fail(f"observed forbidden marker {marker!r}", output)

    if returncode != 0:
        return fail(f"process exited {returncode}", output)

    # --- The native screen was entered -------------------------------------
    enter = ENTER_RE.findall(output)
    if not enter:
        return fail("the CHARSELECT screen was never entered "
                    "([online-charselect] enter)", output)
    default_vehicle = int(enter[0])
    if default_vehicle >= 3:
        return fail(f"enter() computed a non-mask-legal default vehicle "
                    f"{default_vehicle} (expected a base vehicle 0..2)", output)
    if not INSTALL_RE.search(output):
        return fail("the scripted CHARSELECT forward feed never installed", output)

    renders = RENDER_RE.findall(output)
    if not renders:
        return fail("the CHARSELECT screen produced no render witnesses", output)

    # --- The remote seat rendered from the snapshot (display-only) ----------
    remote_rows = [
        r for r in renders
        if int(r[8]) == REMOTE_CHARACTER and int(r[9]) == 1 and r[10] == REMOTE_NAME
    ]
    if not remote_rows:
        return fail(f"no render row showed the remote seat from the snapshot "
                    f"(char={REMOTE_CHARACTER} ready=1 name={REMOTE_NAME})", output)

    # --- The greyed/"TAKEN" tile cue (T9 NIT-1) -----------------------------
    #     When the rival has LOCKED (confirmed) a racer, that tile MUST render
    #     greyed (a hard luminance drop) so the local player reads "unavailable"
    #     at the tile, not just after a rejected confirm. Prove the cue is drawn
    #     on the rival's exact racer AND that the dim really dropped (< a normal
    #     tile's 255) -- the T8 gate saw the tile "not greyed"; this locks it in.
    taken_rows = [
        r for r in renders
        if int(r[8]) == REMOTE_CHARACTER and int(r[9]) == 1
        and int(r[11]) == REMOTE_CHARACTER
    ]
    if not taken_rows:
        return fail(f"the rival locked char {REMOTE_CHARACTER} but no render row "
                    f"greyed that tile (taken={REMOTE_CHARACTER})", output)
    taken_dim = int(taken_rows[0][12])
    if taken_dim >= 255:
        return fail(f"the taken tile was flagged but not dimmed (dim={taken_dim}, "
                    f"expected a hard luminance drop)", output)

    # --- Portrait/character mapping (M4): a swapped sOnlineToPortrait[] entry
    #     would draw the wrong face; the cursor->character->portrait slot must
    #     hold for the moved-to character. ----------------------------------
    cursor_target_rows = [r for r in renders if int(r[0]) == TARGET_CHARACTER]
    if not cursor_target_rows:
        return fail(f"cursor never reached character {TARGET_CHARACTER}", output)
    bad_portrait = [r for r in cursor_target_rows if int(r[2]) != TARGET_PORTRAIT]
    if bad_portrait:
        return fail(f"character {TARGET_CHARACTER} mapped to portrait "
                    f"{bad_portrait[0][2]}, expected {TARGET_PORTRAIT} "
                    f"(sOnlineToPortrait[] is wrong)", output)

    # --- The published intent (character + default vehicle + ready) ---------
    intent_rows = [
        r for r in renders
        if int(r[13]) == TARGET_CHARACTER and int(r[15]) == 1 and int(r[16]) == 1
    ]
    if not intent_rows:
        return fail(f"the local intent never reached character={TARGET_CHARACTER} "
                    f"confirmed=1 ready=1", output)
    intent_vehicle = int(intent_rows[0][14])
    if intent_vehicle != default_vehicle:
        return fail(f"published intent vehicle {intent_vehicle} != enter()'s "
                    f"default vehicle {default_vehicle}", output)
    if intent_vehicle >= 3:
        return fail(f"published intent vehicle {intent_vehicle} is not mask-legal "
                    f"(expected 0..2)", output)

    # --- The LOCAL seat converged + rendered (round-trip closed) ------------
    local_converged = [
        r for r in renders
        if int(r[5]) == TARGET_CHARACTER and int(r[6]) == 1
    ]
    if not local_converged:
        return fail(f"no render row showed the local seat converged "
                    f"(seatChar={TARGET_CHARACTER} seatReady=1)", output)

    # --- B-in-browse must NOT wedge the session (I1) ------------------------
    # The scripted input presses B once while browsing. The session must still
    # ADVANCE on the host-start (asserted below via the RACE hand-off), and the
    # PD-T6 leave stub must log at most once -- not per frame at ~60 Hz.
    leave_stub_count = output.count(LEAVE_STUB)
    if leave_stub_count != 1:
        return fail(f"the PD-T6 leave stub logged {leave_stub_count} times "
                    f"(expected EXACTLY 1: >1 means a B press wedged/spammed the "
                    f"session; 0 means the scripted browse-B was dropped and the "
                    f"I1 no-wedge coverage silently died)",
                    output)

    # --- The phase advanced on the scripted host-start ----------------------
    advance = ADVANCE_RE.findall(output)
    if not advance:
        return fail("the screen never advanced on the scripted host-start "
                    "([online-charselect] advance)", output)
    if not EXIT_RE.search(output):
        return fail("the screen never freed its portrait assets on exit", output)

    # --- Offline isolation preserved through the hand-off -------------------
    race = SESSION_RACE_RE.findall(output)
    if len(race) != 1:
        return fail(f"expected exactly one session RACE hand-off, got {race!r}",
                    output)
    _lobby_ticks, boot_gamemode, boot_menu_id = race[0]
    if int(boot_gamemode) != GAMEMODE_ONLINE_SESSION:
        return fail(f"at hand-off gGameMode={boot_gamemode}, expected "
                    f"GAMEMODE_ONLINE_SESSION ({GAMEMODE_ONLINE_SESSION})", output)
    if int(boot_menu_id) != 0:
        return fail(f"gCurrentMenuId={boot_menu_id} at hand-off -- the offline "
                    f"menu state machine was entered (expected 0)", output)
    if "input-script" in output or "race_2p_split" in output:
        return fail("a menu-nav input script was loaded -- the online path must "
                    "reach the race without one", output)

    # --- The race still boots (via the session) and converges --------------
    if not DIRECT_BOOT_RE.search(output):
        return fail("the race boot never fired after CHARSELECT", output)
    if "[online-live] booting visible engine" not in output:
        return fail("the visible engine was never booted on the live transport",
                    output)
    stats = ENGINE_LIVE_RE.findall(output)
    if len(stats) != 1:
        return fail(f"expected one ENGINE-ONLINE-LIVE witness, got {stats!r}",
                    output)
    (result, raced, drains, advance_failed, _env, _acc, _corr, _drn,
     fold_visible, _foldp, hash_visible, hash_peer, converged) = stats[0]
    if int(result) != 0:
        return fail(f"engine boot returned {result}", output)
    if int(advance_failed) != 0:
        return fail("the live match-input source failed to advance the race",
                    output)
    if int(raced) < 100 or int(drains) < 100:
        return fail(f"the visible race did not sustain enough authored ticks "
                    f"(racedTicks={raced} drainCalls={drains})", output)
    if "[HOST-SHUTDOWN]" not in output:
        return fail("the engine did not tear down its host cleanly", output)
    if not (int(converged) == 1 and hash_visible == hash_peer and
            int(fold_visible) >= 20):
        return fail(f"the two endpoints did not converge (converged={converged} "
                    f"foldVisible={fold_visible} hashVisible={hash_visible} "
                    f"hashPeer={hash_peer})", output)

    print(
        "PASS online charselect: native CHARSELECT entered, drew the real "
        f"portraits, cursor -> Pipsy (char {TARGET_CHARACTER}), published intent "
        f"(char {TARGET_CHARACTER} + default vehicle {default_vehicle} + ready), "
        f"rendered the remote seat {REMOTE_NAME} (char {REMOTE_CHARACTER}) from "
        "the snapshot, local seat converged, advanced on scripted host-start, "
        f"freed assets, handed off (gGameMode={boot_gamemode} "
        f"gCurrentMenuId={boot_menu_id}) -- race converged racedTicks={raced} "
        f"hash={hash_visible}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
