#!/usr/bin/env python3
"""Prove the SEPARATED online boot path: mode_intro forks into the online SESSION
mode, idles in LOBBY_WAIT reading a scripted party_link snapshot, then hands off
to the race -- without ever entering the offline menu state machine.

This is the PD-T1 isolation gate for Strategy D (SEPARATED-BOOT-PATH). The
sibling check_online_engine_boot_direct.py proves the race is reached with no
menu-nav script; this gate additionally proves the race is now reached THROUGH a
separate GAMEMODE_ONLINE_SESSION that holds its own state and never runs the
offline GAMEMODE_MENU / gCurrentMenuId path.

It stands up the same in-process two-adapter live session (real libdatachannel
DTLS over the loopback hub), installs the roster + launch descriptor from the
lobby vote, and boots the engine with NO input script -- exactly like the direct
lane -- but ALSO sets MDKR_TEST_ONLINE_SESSION_SCRIPT=<hold>. That engine-side
seam (game/src/online/online_session.c, beta + env gated, inert otherwise)
installs the REAL party_link forward feed and publishes a scripted snapshot each
LOBBY_WAIT tick: MDKR_ONLINE_LOBBY for the first <hold> ticks, then
MDKR_ONLINE_LOADING. So the session genuinely idles in LOBBY_WAIT reading the
bridge, then boots when the room leaves selection.

Assertions:
  * the session mode was ENTERED   -> [online-session] begin
  * the scripted bridge installed   -> [online-session] test-script install
  * it IDLED in LOBBY_WAIT reading the snapshot (>= hold ticks, snapPhase=LOBBY)
  * it transitioned to the race     -> [online-session] phase=RACE
  * the offline menu was NEVER entered (gCurrentMenuId=0, i.e. MENU_BOOT never
    loaded) and no menu-nav script was used
  * the race still boots and converges byte-for-byte (reuses the direct lane's
    ENGINE-ONLINE-LIVE / ONLINE_RACE witnesses)
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
LOBBY_HOLD = 5  # ticks to idle in LOBBY_WAIT before the scripted room leaves it

SESSION_BEGIN_RE = re.compile(
    r"^\[online-session\] begin:", re.MULTILINE)
SESSION_INSTALL_RE = re.compile(
    r"^\[online-session\] test-script install hold=(\d+)$", re.MULTILINE)
SESSION_LOBBY_RE = re.compile(
    r"^\[online-session\] phase=LOBBY_WAIT tick=(\d+) haveSnap=(\d+) "
    r"snapPhase=(\d+) ready=(\d+)$", re.MULTILINE)
SESSION_RACE_RE = re.compile(
    r"^\[online-session\] phase=RACE booting after (\d+) LOBBY_WAIT tick\(s\); "
    r"isolation gGameMode=(\d+) gCurrentMenuId=(-?\d+)", re.MULTILINE)
DIRECT_BOOT_RE = re.compile(
    r"^\[online-boot\] direct race: track=(\d+) players=(\d+)$", re.MULTILINE)
ONLINE_RACE_RE = re.compile(
    r"^\[ROLLBACK\] online race: loadedTrack=(\d+) raceType=(\d+) "
    r"authoredHz=(\d+)$", re.MULTILINE)
ENGINE_LIVE_RE = re.compile(
    r"^\[ENGINE-ONLINE-LIVE\] result=(-?\d+) racedTicks=(\d+) drainCalls=(\d+) "
    r"advanceFailed=(\d+) inputEnvelopes=(\d+) transportAccepted=(\d+) "
    r"transportCorrected=(\d+) transportDrained=(\d+) foldVisible=(\d+) "
    r"foldPeer=(\d+) hashVisible=([0-9a-f]{16}) hashPeer=([0-9a-f]{16}) "
    r"converged=(\d+)$", re.MULTILINE)

# GAMEMODE_ONLINE_SESSION aliases GAMEMODE_UNUSED_2 == 2 (game/src/thread3_main.h).
GAMEMODE_ONLINE_SESSION = 2


def fail(message: str, output: str = "") -> int:
    print(f"FAIL online session boot: {message}", file=sys.stderr)
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
    parser.add_argument("--hold", type=int, default=LOBBY_HOLD)
    parser.add_argument("--timeout", type=int, default=300)
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    binary = Path(resolve_binary(args.build)).expanduser().resolve()
    rom = args.rom.expanduser().resolve()
    for path, label in ((binary, "binary"), (rom, "ROM")):
        if not path.is_file():
            parser.error(f"missing {label}: {path}")

    with tempfile.TemporaryDirectory(prefix="mdkr64-online-session-") as temp:
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
            # The separated-boot seam under test: idle in LOBBY_WAIT for <hold>
            # ticks reading a scripted party_link snapshot, then leave the lobby.
            MDKR_TEST_ONLINE_SESSION_SCRIPT=str(args.hold),
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
            return fail(f"engine run timed out (a LOBBY_WAIT stall would look "
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

    # --- Separated boot path -------------------------------------------------
    if not SESSION_BEGIN_RE.search(output):
        return fail("the online SESSION mode was never entered "
                    "(mode_intro did not fork into GAMEMODE_ONLINE_SESSION)",
                    output)

    install = SESSION_INSTALL_RE.findall(output)
    if not install:
        return fail("the scripted party_link seam never installed", output)
    if int(install[0]) != args.hold:
        return fail(f"scripted hold {install[0]}, expected {args.hold}", output)

    lobby = SESSION_LOBBY_RE.findall(output)
    if len(lobby) < args.hold:
        return fail(f"the session did not idle in LOBBY_WAIT long enough "
                    f"(saw {len(lobby)} ticks, expected >= {args.hold})", output)
    # The held ticks must have actually read the scripted LOBBY snapshot.
    held = [row for row in lobby
            if int(row[1]) == 1 and int(row[2]) == 1 and int(row[3]) == 0]
    if len(held) < args.hold:
        return fail(f"the session did not READ the scripted LOBBY snapshot while "
                    f"waiting (saw {len(held)} held ticks with haveSnap=1 "
                    f"snapPhase=1 ready=0, expected >= {args.hold})", output)

    race = SESSION_RACE_RE.findall(output)
    if len(race) != 1:
        return fail(f"expected exactly one session RACE hand-off, got {race!r}",
                    output)
    lobby_ticks, boot_gamemode, boot_menu_id = race[0]
    if int(lobby_ticks) < args.hold:
        return fail(f"session booted after only {lobby_ticks} LOBBY_WAIT ticks, "
                    f"expected >= {args.hold}", output)

    # --- Offline isolation ---------------------------------------------------
    if int(boot_gamemode) != GAMEMODE_ONLINE_SESSION:
        return fail(f"at hand-off gGameMode={boot_gamemode}, expected "
                    f"GAMEMODE_ONLINE_SESSION ({GAMEMODE_ONLINE_SESSION}) -- the "
                    f"online path must never pass through the menu mode", output)
    if int(boot_menu_id) != 0:
        return fail(f"gCurrentMenuId={boot_menu_id} at hand-off -- the offline "
                    f"menu state machine was entered on the online path "
                    f"(expected 0, i.e. MENU_BOOT never loaded)", output)
    if "input-script" in output or "race_2p_split" in output:
        return fail("a menu-nav input script was loaded -- the online path must "
                    "reach the race without one", output)

    # --- The race still boots (via the session) and converges ----------------
    if not DIRECT_BOOT_RE.search(output):
        return fail("the race boot never fired after the session hand-off", output)
    if "[online-live] booting visible engine" not in output:
        return fail("the visible engine was never booted on the live transport",
                    output)
    online_race = ONLINE_RACE_RE.findall(output)
    if not online_race:
        return fail("the engine never entered an ONLINE rollback race", output)

    stats = ENGINE_LIVE_RE.findall(output)
    if len(stats) != 1:
        return fail(f"expected one ENGINE-ONLINE-LIVE witness, got {stats!r}",
                    output)
    (result, raced, drains, advance_failed, envelopes, accepted, corrected,
     drained, fold_visible, fold_peer, hash_visible, hash_peer,
     converged) = stats[0]
    if int(result) != 0:
        return fail(f"engine boot returned {result}", output)
    if int(advance_failed) != 0:
        return fail("the live match-input source failed to advance the race",
                    output)
    if int(raced) < 100 or int(drains) < 100:
        return fail(f"the visible race did not sustain enough authored ticks "
                    f"(racedTicks={raced} drainCalls={drains}, expected >= 100)",
                    output)
    if "[HOST-SHUTDOWN]" not in output:
        return fail("the engine did not tear down its host cleanly", output)
    converged_ok = int(converged) == 1 and hash_visible == hash_peer and \
        int(fold_visible) >= 20
    if not converged_ok:
        return fail(f"the two endpoints did not converge (converged={converged} "
                    f"foldVisible={fold_visible} hashVisible={hash_visible} "
                    f"hashPeer={hash_peer})", output)

    print(
        "PASS online session boot: mode_intro forked into GAMEMODE_ONLINE_SESSION, "
        f"idled {lobby_ticks} LOBBY_WAIT tick(s) reading the scripted party_link "
        f"snapshot (>= {args.hold} held), then handed off to the race WITHOUT the "
        f"offline menu (gGameMode={boot_gamemode} gCurrentMenuId={boot_menu_id}) "
        f"-- race booted and converged racedTicks={raced} convergedTicks="
        f"{fold_visible} hash={hash_visible} engineExit=clean noStall=1"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
