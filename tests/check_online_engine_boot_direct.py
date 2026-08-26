#!/usr/bin/env python3
"""Prove the VISIBLE engine reaches the ONLINE race with NO menu-nav input script.

This is the headline online-UX gate. The sibling check_online_engine_boot.py
drove the front-end to Ancient Lake by replaying tests/input_scripts/
race_2p_split.txt -- a hand-authored, time-coded walk of DKR's Title ->
Character Select -> Track Select menus. A real online player must NEVER touch or
even see those single-player screens.

Here the same in-process two-adapter live session is stood up (real libdatachannel
DTLS over the O-T2 loopback hub), the roster + launch descriptor are installed
from the lobby vote, and then the engine is booted with NO input script at all.
The direct-boot seam (mode_intro -> mdkr_online_boot_direct_race, beta only)
must take the game straight from cold boot into the manifest race -- proving the
race is reached purely from the manifest, not from scripted menu navigation.

Assertions mirror check_online_engine_boot.py exactly (booted an ONLINE rollback
race on the agreed track, sustained authored ticks off the live match-input
source, folded real peer input, no stall, clean teardown, exit 0, and the two
endpoints converged byte-for-byte), plus it emits the [online-boot] direct-race
witness and asserts the menu-nav script was NOT used.

--track <id> re-aims the same gate at any of the 20 standard race tracks: the
loopback leader fixes the track with SET_CONFIG_TRACK (env seam
MDKR_APP_TEST_ONLINE_TRACK in platform/app/online_live_wiring.cpp; a
configured session skips the track-vote step in the view exactly like retail,
so the config alone drives the manifest), the wiring derives the track's raw
ROM vehicle mask + default vehicle, and this check asserts the engine booted
THAT track. --mask 0x<mm> additionally pins the exact START_RACE
vehicle mask the wiring froze, which the engine's admission equality
(manifest mask == leveltable_vehicle_usable(track)) then proves end-to-end by
booting at all. The narrow-mask lane is:

    check_online_engine_boot_direct.py --track 8 --mask 0x2

Whale Bay: hovercraft-only (mask 0x2), non-Car default vehicle -- the
zero-coverage admission path (mask != 0x07, default != Car). Without --track
the invocation, environment and assertions are byte-identical to the
historical Ancient Lake gate, so registered lanes stay stable.
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

from harness_utils import DEFAULT_BUILD_DIR, resolve_binary

ROOT = Path(__file__).resolve().parent.parent
# With the direct-boot seam the race level (rollback runtime active) is reached a
# few dozen ticks after cold boot instead of ~2491, so nearly the whole budget is
# authored racing. Keep 3000 for a long convergence window.
TICKS = 3000

ENGINE_LIVE_RE = re.compile(
    r"^\[ENGINE-ONLINE-LIVE\] result=(-?\d+) racedTicks=(\d+) drainCalls=(\d+) "
    r"advanceFailed=(\d+) inputEnvelopes=(\d+) transportAccepted=(\d+) "
    r"transportCorrected=(\d+) transportDrained=(\d+) foldVisible=(\d+) "
    r"foldPeer=(\d+) hashVisible=([0-9a-f]{16}) hashPeer=([0-9a-f]{16}) "
    r"converged=(\d+)$",
    re.MULTILINE,
)
ONLINE_RACE_RE = re.compile(
    r"^\[ROLLBACK\] online race: loadedTrack=(\d+) raceType=(\d+) "
    r"authoredHz=(\d+)$",
    re.MULTILINE,
)
DIRECT_BOOT_RE = re.compile(
    r"^\[online-boot\] direct race: track=(\d+) players=(\d+)$",
    re.MULTILINE,
)
# Printed by the wiring only when a session-config env seam is set (--track).
CONFIG_TRACK_RE = re.compile(
    r"^\[online-live\] loopback config mode=single track=(\d+) "
    r"startMask=0x([0-9a-f]{2}) vehicle=(\d+)$",
    re.MULTILINE,
)


def fail(message: str, output: str = "") -> int:
    print(f"FAIL online engine boot (direct): {message}", file=sys.stderr)
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
    parser.add_argument("--build", default=DEFAULT_BUILD_DIR)
    parser.add_argument("--rom", type=Path, default="baserom.us.v80.z64")
    parser.add_argument("--ticks", type=int, default=TICKS)
    parser.add_argument("--timeout", type=int, default=300)
    parser.add_argument(
        "--track", type=int, default=None,
        help="leader-configured track id (env seam MDKR_APP_TEST_ONLINE_TRACK);"
             " omitted keeps the historical vote-track-5 flow byte-identical")
    parser.add_argument(
        "--mask", type=lambda text: int(text, 0), default=None,
        help="assert the wiring froze exactly this START_RACE vehicle mask"
             " (requires --track)")
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()
    if args.mask is not None and args.track is None:
        parser.error("--mask requires --track")

    binary = Path(resolve_binary(args.build)).expanduser().resolve()
    rom = args.rom.expanduser().resolve()
    for path, label in ((binary, "binary"), (rom, "ROM")):
        if not path.is_file():
            parser.error(f"missing {label}: {path}")

    with tempfile.TemporaryDirectory(prefix="mdkr64-online-direct-") as temp:
        run_dir = Path(temp)
        (run_dir / "saves").mkdir()
        (run_dir / "preferences").mkdir()
        # Deliberately NO MDKR_APP_AUTOPLAY_INPUT_SCRIPT: the race must be reached
        # from the manifest + boot alone. MDKR_TEST_SCRIPT_ONLY_INPUT stays on so
        # no stray host input can reach the game either -- the only inputs are the
        # live match transport (canonical) and MDKR_AUTOPILOT (driving line).
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
            MDKR_VIDEO_CONFIG_PATH=str(run_dir / "video.ini"),
            MDKR64_HIDDEN="1",
        )
        if args.track is not None:
            environment["MDKR_APP_TEST_ONLINE_TRACK"] = str(args.track)
        if args.verbose:
            print(f"$ {binary}", flush=True)
        try:
            process = subprocess.run(
                [str(binary)], cwd=run_dir, env=environment, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                timeout=args.timeout, check=False,
            )
        except subprocess.TimeoutExpired as error:
            return fail(f"engine run timed out (a stall would look like this): "
                        f"{error}")
        output = process.stdout or ""

    for marker in ("[FATAL]", "[CRASH]", "AddressSanitizer",
                   "online race admission rejected",
                   "launcher input provider rejected",
                   "engine startup rejected before authored tick one"):
        if marker in output:
            return fail(f"observed forbidden marker {marker!r}", output)

    if process.returncode != 0:
        return fail(f"process exited {process.returncode}", output)

    direct = DIRECT_BOOT_RE.findall(output)
    if not direct:
        return fail("the direct-boot seam never fired (the race was not reached "
                    "straight from the manifest)", output)

    # The menu-nav fixture must play no part in reaching the race here.
    if "input-script" in output or "race_2p_split" in output:
        return fail("a menu-nav input script was loaded -- this gate must reach "
                    "the race without one", output)

    if "[online-live] booting visible engine" not in output:
        return fail("the visible engine was never booted on the live transport",
                    output)

    expected_track = "5" if args.track is None else str(args.track)
    config_mask = None
    if args.track is not None:
        # The wiring must witness the leader config it froze, on the right
        # track, and (with --mask) with exactly the expected narrow mask.
        config = CONFIG_TRACK_RE.findall(output)
        if len(config) != 1:
            return fail("the loopback session-config witness never fired "
                        "(MDKR_APP_TEST_ONLINE_TRACK seam)", output)
        config_track, config_mask_hex, config_vehicle = config[0]
        config_mask = int(config_mask_hex, 16)
        if config_track != expected_track:
            return fail(f"the wiring configured track {config_track}, "
                        f"expected {expected_track}", output)
        if args.mask is not None and config_mask != args.mask:
            return fail(
                f"the wiring froze START_RACE mask {config_mask:#04x}, "
                f"expected {args.mask:#04x}", output)
        direct_track_witness = direct[0][0]
        if direct_track_witness != expected_track:
            return fail(f"direct-boot witness fired for track "
                        f"{direct_track_witness}, expected {expected_track}",
                        output)

    online_race = ONLINE_RACE_RE.findall(output)
    if not online_race:
        return fail("the engine never entered an ONLINE rollback race", output)
    loaded_track, race_type, authored_hz = online_race[0]
    if loaded_track != expected_track or race_type != "0":
        return fail(
            f"online race loaded the wrong contest track={loaded_track} "
            f"type={race_type} (expected track {expected_track}, standard 0)",
            output)

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
        return fail("the live match-input source failed to advance the race "
                    "(a stall at the transport seam)", output)
    if int(raced) < 100 or int(drains) < 100:
        return fail(f"the visible race did not sustain enough authored ticks "
                    f"through the live seam (racedTicks={raced} "
                    f"drainCalls={drains}, expected >= 100)", output)
    if int(envelopes) <= 0 and int(accepted) <= 0:
        return fail(f"no real peer input crossed the mesh into the transport "
                    f"(inputEnvelopes={envelopes} transportAccepted={accepted})",
                    output)
    if int(drained) <= 0:
        return fail(f"the transport drained no authored ticks "
                    f"(transportDrained={drained})", output)
    if "[HOST-SHUTDOWN]" not in output:
        return fail("the engine did not tear down its host cleanly", output)

    converged_ok = int(converged) == 1 and hash_visible == hash_peer and \
        int(fold_visible) >= 20
    if not converged_ok:
        return fail(
            f"the two endpoints did not converge on the canonical race input "
            f"(converged={converged} foldVisible={fold_visible} "
            f"foldPeer={fold_peer} hashVisible={hash_visible} "
            f"hashPeer={hash_peer})", output)

    direct_track, direct_players = direct[0]
    narrow = ""
    if args.track is not None:
        narrow = (f" -- leader-configured track={expected_track} "
                  f"mask={config_mask:#04x} (admission equality manifest mask "
                  "== leveltable mask held end-to-end, non-default lane)")
    print(
        "PASS online engine boot (direct): the VISIBLE engine reached the online "
        f"race with NO menu-nav script -- direct-boot track={direct_track} "
        f"players={direct_players}, ran on track {loaded_track} at {authored_hz}Hz "
        f"off the LIVE transport -- racedTicks={raced} drainCalls={drains} "
        f"inputEnvelopes={envelopes} transportAccepted={accepted} "
        f"transportDrained={drained} corrected={corrected} "
        f"convergedTicks={fold_visible} hash={hash_visible} "
        "engineExit=clean noStall=1" + narrow
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
