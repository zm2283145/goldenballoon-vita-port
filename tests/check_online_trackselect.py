#!/usr/bin/env python3
"""Prove the native online HOST TRACK / CUP SELECT screen (PD-T3, Strategy D2).

Where check_online_charselect.py proves the FIRST player-facing screen of the
separated online path, THIS lane proves the SECOND: the session enters its
TRACKSELECT phase (game/src/online/online_trackselect.c, which re-implements the
presentation with the game's OWN per-world background art + font instead of
calling the offline track-select _loop), the host locks a track over the
party_link reverse feed, the local vehicle auto-narrows to that track's usable
mask (R-A), the reducer's ready-clear on the config change is followed by both
seats reconverging to ready (R-B continuous republish), the host starts, and the
race boots + converges byte-for-byte -- all without ever entering the offline menu
state machine.

It stands up the same in-process two-adapter live session as the charselect lane
(real libdatachannel DTLS over the loopback hub, roster + launch descriptor from
the vote) and sets BOTH MDKR_TEST_ONLINE_CHARSELECT=1 (so the CHARSELECT seam
installs the feed, scripts the character pick, and converges the local seat) AND
MDKR_TEST_ONLINE_TRACKSELECT=1 (so the CHARSELECT seam DEFERS its self-start,
the session hands off CHARSELECT -> TRACKSELECT on the local-ready signal, and the
TRACKSELECT seam acts as the launcher reducer: it converges the local seat,
applies the host's SET_CONFIG_TRACK with the reducer's ready-clear, re-asserts the
scripted joiner's ready, and flips the lobby to LOADING on the host's start).

The scripted TRACKSELECT input FIRST proves the B -> CHARSELECT back path (no
wedge), then on re-entry walks the cursor to Whale Bay (cup 2 round 0, track 8,
hovercraft-only mask 0x2), LOCKS it, and presses Start.

Assertions:
  * the CHARSELECT screen was entered, then handed off to TRACKSELECT
  * the TRACKSELECT screen was entered (>= twice: the B round-trip)
  * the offered track-id list equals the reducer-accepted set (R-D)
  * B on TRACKSELECT returned to CHARSELECT (no wedge; the PD-T6 leave stub still
    logs EXACTLY once across the whole run)
  * the host lock reached the reducer: the snapshot configured_track converged to
    track 8
  * the vehicle auto-narrowed to hovercraft (1) for Whale Bay's 0x2 mask (R-A)
  * both seats' ready cleared then reconverged around the config change (R-B)
  * the phase advanced on the scripted host-start; assets were freed on exit
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

ROOT = Path(__file__).resolve().parent.parent
TICKS = 3000

# The reducer-accepted set (kCupTracks in platform/online/lobby_core.c ==
# online_track_table.c), cup-major then round order. R-D: the screen's offered
# list must equal this exactly.
EXPECTED_TRACKS = [5, 3, 29, 7, 13, 6, 9, 28, 8, 4, 10, 30,
                   19, 18, 20, 31, 17, 32, 33, 15]
LOCKED_TRACK = 8      # Whale Bay (cup 2 round 0)
LOCKED_MASK = 0x2     # hovercraft-only
NARROWED_VEHICLE = 1  # VEHICLE_HOVERCRAFT

GAMEMODE_ONLINE_SESSION = 2

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
    r"cup=(-?\d+)\} seat\{r0=(\d+) r1=(\d+)\} snap\{cfgTrack=(\d+) cup=(\d+) "
    r"phase=(\d+)\} start=(\d+)$",
    re.MULTILINE)
# findall tuple indices:
#  0 mode 1 col 2 row 3 host 4 track 5 mask 6 vehicle 7 lockedTrack 8 lockedCup
#  9 r0 10 r1 11 snapCfgTrack 12 snapCup 13 snapPhase 14 start

SESSION_RACE_RE = re.compile(
    r"^\[online-session\] phase=RACE booting after (\d+) LOBBY_WAIT tick\(s\); "
    r"isolation gGameMode=(\d+) gCurrentMenuId=(-?\d+)", re.MULTILINE)
DIRECT_BOOT_RE = re.compile(
    r"^\[online-boot\] direct race: track=(\d+) players=(\d+)$", re.MULTILINE)
ENGINE_LIVE_RE = re.compile(
    r"^\[ENGINE-ONLINE-LIVE\] result=(-?\d+) racedTicks=(\d+) drainCalls=(\d+) "
    r"advanceFailed=(\d+) inputEnvelopes=(\d+) transportAccepted=(\d+) "
    r"transportCorrected=(\d+) transportDrained=(\d+) foldVisible=(\d+) "
    r"foldPeer=(\d+) hashVisible=([0-9a-f]{16}) hashPeer=([0-9a-f]{16}) "
    r"converged=(\d+)$", re.MULTILINE)


def fail(message: str, output: str = "") -> int:
    print(f"FAIL online trackselect: {message}", file=sys.stderr)
    if output:
        print(output[-16000:], file=sys.stderr)
    return 1


def clean_environment(**updates: str) -> dict[str, str]:
    environment = {
        key: value for key, value in os.environ.items()
        if not key.startswith(("MDKR", "GE007_"))
    }
    environment.update(updates)
    return environment


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

    with tempfile.TemporaryDirectory(prefix="mdkr64-online-trackselect-") as temp:
        run_dir = Path(temp)
        (run_dir / "saves").mkdir()
        (run_dir / "preferences").mkdir()
        environment = clean_environment(
            LC_ALL="C",
            MDKR_APP_AUTOPLAY="1",
            MDKR_APP_TEST_ONLINE_LIVE="1",
            MDKR_APP_AUTOPLAY_TICKS=str(args.ticks),
            MDKR_APP_PREFS_DIR=str(run_dir / "preferences"),
            MDKR_AUDIO="0",
            MDKR_AUTOPILOT="1",
            MDKR_NO_CRASH_HANDLER="1",
            MDKR_PRESENT_RATE="original",
            MDKR_RENDERER="gl",
            MDKR_ROM=str(rom),
            MDKR_SAVE_DIR=str(run_dir / "saves"),
            MDKR_STATE_HASH="3",
            MDKR_TEST_SCRIPT_ONLY_INPUT="1",
            # CHARSELECT seam converges the character; TRACKSELECT seam drives the
            # track lock + host-start (and CHARSELECT defers its self-start).
            MDKR_TEST_ONLINE_CHARSELECT="1",
            MDKR_TEST_ONLINE_TRACKSELECT="1",
            MDKR_VIDEO_CONFIG_PATH=str(run_dir / "video.ini"),
            MDKR64_HIDDEN="1",
        )
        if args.verbose:
            print(f"$ {binary}", flush=True)
        try:
            process = subprocess.run(
                [str(binary)], cwd=run_dir, env=environment, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                timeout=args.timeout, check=False,
            )
        except subprocess.TimeoutExpired as error:
            return fail(f"engine run timed out (a TRACKSELECT stall would look "
                        f"like this): {error}")
        output = process.stdout or ""

    for marker in ("[FATAL]", "[CRASH]", "AddressSanitizer",
                   "online race admission rejected",
                   "launcher input provider rejected",
                   "engine startup rejected before authored tick one"):
        if marker in output:
            return fail(f"observed forbidden marker {marker!r}", output)

    if process.returncode != 0:
        return fail(f"process exited {process.returncode}", output)

    # --- CHARSELECT ran and handed off to TRACKSELECT ----------------------
    if not CS_ENTER_RE.search(output):
        return fail("the CHARSELECT screen was never entered (the flow must "
                    "pass through charselect first)", output)
    cs_to_ts = SESS_CS_TO_TS_RE.findall(output)
    if len(cs_to_ts) < 2:
        return fail(f"expected CHARSELECT -> TRACKSELECT at least twice (the B "
                    f"round-trip), got {len(cs_to_ts)}", output)

    # --- TRACKSELECT entered (>= twice) ------------------------------------
    entries = [int(e) for e in TS_ENTER_RE.findall(output)]
    if not entries:
        return fail("the TRACKSELECT screen was never entered "
                    "([online-trackselect] enter)", output)
    if max(entries) < 2:
        return fail(f"TRACKSELECT was entered only {max(entries)} time(s); the "
                    f"B->CHARSELECT round-trip requires >= 2", output)

    # --- Offered track-id list == the reducer-accepted set (R-D) ------------
    tracks_line = TS_TRACKS_RE.search(output)
    if not tracks_line:
        return fail("the screen never emitted its offered track-id list", output)
    offered = [int(x) for x in tracks_line.group(1).split()]
    if offered != EXPECTED_TRACKS:
        return fail(f"offered track ids {offered} != reducer-accepted set "
                    f"{EXPECTED_TRACKS} (R-D drift)", output)

    # --- Back-to-charselect path exercised, no wedge -----------------------
    if not TS_BACK_RE.search(output):
        return fail("B on TRACKSELECT never returned to CHARSELECT "
                    "([online-trackselect] back to charselect)", output)
    if not SESS_TS_TO_CS_RE.search(output):
        return fail("the session never logged the TRACKSELECT -> CHARSELECT "
                    "back transition", output)
    # The PD-T6 charselect leave-to-launcher stub must still log EXACTLY once
    # across the whole run: the session preserves the warn-once latch across the
    # trackselect-back re-entry, so the browse-B on the re-entered charselect does
    # NOT re-spam it (and >1 would mean a wedge/spam; 0 would mean the I1
    # browse-B was dropped).
    leave_stub_count = output.count(CS_LEAVE_STUB)
    if leave_stub_count != 1:
        return fail(f"the PD-T6 charselect leave stub logged {leave_stub_count} "
                    f"times (expected EXACTLY 1 across the whole flow)", output)

    renders = TS_RENDER_RE.findall(output)
    if not renders:
        return fail("the TRACKSELECT screen produced no render witnesses", output)

    # --- Host lock reached the reducer: configured_track converged to 8 -----
    converged_cfg = [r for r in renders if int(r[11]) == LOCKED_TRACK]
    if not converged_cfg:
        return fail(f"the snapshot configured_track never converged to the "
                    f"locked track {LOCKED_TRACK} (SET_CONFIG_TRACK did not "
                    f"reach the reducer)", output)

    # --- Vehicle auto-narrow held for Whale Bay's 0x2 mask (R-A) -----------
    narrow_rows = [
        r for r in renders
        if int(r[4]) == LOCKED_TRACK and int(r[5], 16) == LOCKED_MASK
    ]
    if not narrow_rows:
        return fail(f"no render row resolved track {LOCKED_TRACK} with mask "
                    f"0x{LOCKED_MASK:x}", output)
    if any(int(r[6]) != NARROWED_VEHICLE for r in narrow_rows):
        bad = [int(r[6]) for r in narrow_rows if int(r[6]) != NARROWED_VEHICLE]
        return fail(f"vehicle did not auto-narrow to {NARROWED_VEHICLE} for "
                    f"track {LOCKED_TRACK} (mask 0x{LOCKED_MASK:x}); saw {bad}",
                    output)
    if any(((1 << int(r[6])) & LOCKED_MASK) == 0 for r in narrow_rows):
        return fail("a resolved vehicle bit was NOT inside the track mask (R-A "
                    "violation)", output)

    # --- Ready cleared then reconverged around the config change (R-B) -------
    cfg_rows = [r for r in renders if int(r[11]) == LOCKED_TRACK]
    cleared = [r for r in cfg_rows if int(r[9]) == 0 and int(r[10]) == 0]
    reconverged = [r for r in cfg_rows if int(r[9]) == 1 and int(r[10]) == 1]
    if not cleared:
        return fail("never witnessed the ready-clear after the config change "
                    "(both seats r0=0 r1=0 with configured_track locked)", output)
    if not reconverged:
        return fail("both seats never reconverged to ready after the "
                    "config-clear (r0=1 r1=1 with configured_track locked)",
                    output)

    # --- Phase advanced on the scripted host-start; assets freed -----------
    if not TS_ADVANCE_RE.search(output):
        return fail("the screen never advanced on the scripted host-start "
                    "([online-trackselect] advance)", output)
    if not TS_EXIT_RE.search(output):
        return fail("the screen never freed its world bg assets on exit", output)

    # --- Offline isolation preserved through the hand-off ------------------
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
        return fail("the race boot never fired after TRACKSELECT", output)
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
        "PASS online trackselect: native TRACKSELECT entered (real per-world "
        "art + font), offered the reducer-accepted 20-track set, B returned to "
        "CHARSELECT (no wedge, 1 stub), host locked Whale Bay (track "
        f"{LOCKED_TRACK}) -> configured_track converged, vehicle auto-narrowed "
        f"to {NARROWED_VEHICLE} for mask 0x{LOCKED_MASK:x}, both seats "
        "reconverged to ready after the config-clear, advanced on host-start, "
        f"freed assets, handed off (gGameMode={boot_gamemode} "
        f"gCurrentMenuId={boot_menu_id}) -- race converged racedTicks={raced} "
        f"hash={hash_visible}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
