#!/usr/bin/env python3
"""Prove the native online CHARACTER SELECT screen.

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
  * the grid is laid out in the retail PLAYER SELECT roster order (row0 Krunch,
    Diddy, Drumstick, Bumper, Banjo / row1 Conker, Tiptup, T.T., Pipsy, Timber),
    a pure cell<->online-id relabel, and EVERY cell draws the name + portrait slot
    that belong to the online id it holds (the _enter grid-layout witness)
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

Mutation proof (the taken-dim cue coverage is NON-VACUOUS -- re-runnable):
  Delete/neutralize the taken-tile portrait dim in
  game/src/online/online_charselect.c (the `pr = pg = pb = CS_TAKEN_DIM;` line in
  charselect_render's `if (taken)` block), rebuild build-beta, and re-run this lane:
  it MUST FAIL. The witness 'dim' field is the luminance the render ACTUALLY handed
  the taken tile's portrait blit, so a dropped dim renders the tile normal-bright
  (210) and the DIM_CEILING assertion below catches it. Restore the line and the
  lane is GREEN again. (Before the witness reported the drawn value it re-derived
  CS_TAKEN_DIM from the same taken condition, so this exact mutation left the lane
  GREEN -- the cue was certified but never observed.)
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
GRID_RE = re.compile(
    r"^\[online-charselect\] grid cell=(\d+) id=(\d+) name=(\S+) portrait=(\d+)$",
    re.MULTILINE)
LEAVE_STUB = "[online-charselect] leave requested"
ADVANCE_RE = re.compile(
    r"^\[online-charselect\] advance: lobby left LOBBY \(phase=(\d+)\)",
    re.MULTILINE)
EXIT_RE = re.compile(
    r"^\[online-charselect\] exit: freed portrait assets", re.MULTILINE)

TARGET_CHARACTER = 2    # Pipsy, in the online id space (== grid cell == hover)
TARGET_PORTRAIT = 7     # sOnlineToPortrait[2] == CHARACTER_PIPSY == gRacerPortraits[7]
REMOTE_CHARACTER = 5    # Bumper, the scripted remote pick
REMOTE_NAME = "RIVAL"

# The CORRECT online id -> portrait-slot mapping is the engine Character enum
# (enums.h): gRacerPortraits[] is indexed by CHARACTER_*, NOT by the order the
# portrait SYMBOLS appear in menu.c's array initializer (those symbol names are
# mislabelled in the decomp). Keyed by the name the grid draws under each face so a
# future symbol-name-derived regression -- which draws the wrong face under a name --
# is caught here (verified against retail-ref-shots/ keyframes).
#   name -> CHARACTER enum value == gRacerPortraits slot the witness must report
NAME_TO_PORTRAIT = {
    "DIDDY": 9, "TIMBER": 4, "PIPSY": 7, "TIPTUP": 2, "CONKER": 3,
    "BUMPER": 1, "BANJO": 5, "KRUNCH": 0, "DRUMSTICK": 6, "T.T.": 8,
}

# The retail PLAYER SELECT visual grid order (menu.c adjacency table 1046-1067),
# as a cell index -> ONLINE id map. The grid is a PURE relabel: the published
# hover_character / reducer ids stay the online id; only the on-screen POSITION of
# each racer moves to match the retail roster:
#   row0 (cells 0-4): Krunch, Diddy, Drumstick, Bumper, Banjo
#   row1 (cells 5-9): Conker, Tiptup, T.T.,   Pipsy,  Timber
# The _enter grid-layout witness dumps cell->id + the drawn name/portrait, so this
# proves BOTH the retail order and every visible name->face pair (not just the cells
# the scripted cursor happens to visit).
CELL_TO_ONLINE = [7, 0, 8, 5, 6, 4, 3, 9, 2, 1]
ONLINE_NAMES = ["DIDDY", "TIMBER", "PIPSY", "TIPTUP", "CONKER",
                "BUMPER", "BANJO", "KRUNCH", "DRUMSTICK", "T.T."]


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

    # --- The greyed/"TAKEN" tile cue ----------------------------------------
    #     When the rival has LOCKED (confirmed) a racer, that tile MUST render
    #     greyed (a hard luminance drop) so the local player reads "unavailable"
    #     AT the tile, not just after a rejected confirm. The 'dim' field is the
    #     luminance the render ACTUALLY handed the taken tile's portrait blit
    #     (plumbed from the draw site in online_charselect.c), NOT a re-derivation of
    #     the CS_TAKEN_DIM constant -- so dropping the render's dim line reports a
    #     normal-bright tile here and this assertion FAILS (see the docstring's
    #     mutation proof). A normal (undimmed) portrait tile is drawn at 210; a
    #     genuinely greyed tile is CS_TAKEN_DIM (72). Require a hard drop to well
    #     under half a normal tile, so a dropped OR merely token dim is caught. The
    #     cursor-on-taken tile uses a distinct dimmed-gold highlight (not the pure
    #     greyed value), so the filter EXCLUDES rows where the local cursor rests on
    #     the taken tile (int(r[0]) != REMOTE_CHARACTER) -- confining the dim check to
    #     the pure greyed cue rather than relying on the scripted cursor never landing
    #     there.
    NORMAL_TILE_LUM = 210
    DIM_CEILING = NORMAL_TILE_LUM // 2   # 105: clears the 72 drop, catches 210
    taken_rows = [
        r for r in renders
        if int(r[8]) == REMOTE_CHARACTER and int(r[9]) == 1
        and int(r[11]) == REMOTE_CHARACTER
        and int(r[0]) != REMOTE_CHARACTER
    ]
    if not taken_rows:
        return fail(f"the rival locked char {REMOTE_CHARACTER} but no render row "
                    f"greyed that tile (taken={REMOTE_CHARACTER})", output)
    not_dimmed = [int(r[12]) for r in taken_rows if int(r[12]) >= DIM_CEILING]
    if not_dimmed:
        return fail(f"the taken tile was flagged but NOT genuinely dimmed (drawn "
                    f"luminance {not_dimmed[0]} >= {DIM_CEILING}; a normal tile is "
                    f"{NORMAL_TILE_LUM}, a greyed tile is 72) -- the render's "
                    f"taken-dim line was dropped or weakened", output)

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

    # EVERY witnessed cell must draw the portrait slot that BELONGS to the name
    # under it (name -> Character-enum slot). This is the direct guard against the
    # symbol-name-derived mapping bug: a table rebuilt from menu.c's mislabelled
    # portrait symbols draws e.g. PIPSY's name over T.T.'s face, which this catches.
    for r in renders:
        name, slot = r[1], int(r[2])
        want = NAME_TO_PORTRAIT.get(name)
        if want is not None and slot != want:
            return fail(f"name {name} drew portrait slot {slot}, expected {want} "
                        f"(gRacerPortraits is Character-enum ordered; "
                        f"sOnlineToPortrait[] must map online id -> CHARACTER_*)",
                        output)

    # --- Retail grid ORDER + every visible name->face pair --------------------
    #     The _enter grid-layout witness dumps every CELL's online id + drawn
    #     name/portrait. Assert the retail cell order AND that each cell's name and
    #     portrait slot belong to the online id it holds -- so a wrong cell<->id
    #     relabel (or a name/face desync) is caught for ALL ten cells, independent
    #     of the scripted cursor's path.
    grid = GRID_RE.findall(output)
    if len(grid) != len(CELL_TO_ONLINE):
        return fail(f"the grid-layout witness dumped {len(grid)} cells, expected "
                    f"{len(CELL_TO_ONLINE)} (one per cell)", output)
    seen_cells = {}
    for cell_s, id_s, name, portrait_s in grid:
        seen_cells[int(cell_s)] = (int(id_s), name, int(portrait_s))
    for cell, want_id in enumerate(CELL_TO_ONLINE):
        if cell not in seen_cells:
            return fail(f"grid cell {cell} was never witnessed", output)
        got_id, got_name, got_portrait = seen_cells[cell]
        if got_id != want_id:
            return fail(f"grid cell {cell} holds online id {got_id}, expected "
                        f"{want_id} (retail order Krunch,Diddy,Drumstick,Bumper,"
                        f"Banjo / Conker,Tiptup,T.T.,Pipsy,Timber)", output)
        if got_name != ONLINE_NAMES[want_id]:
            return fail(f"grid cell {cell} (id {want_id}) drew name {got_name}, "
                        f"expected {ONLINE_NAMES[want_id]}", output)
        if got_portrait != NAME_TO_PORTRAIT[ONLINE_NAMES[want_id]]:
            return fail(f"grid cell {cell} (name {got_name}) drew portrait slot "
                        f"{got_portrait}, expected "
                        f"{NAME_TO_PORTRAIT[ONLINE_NAMES[want_id]]}", output)

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
    # leave stub must log at most once -- not per frame at ~60 Hz.
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
