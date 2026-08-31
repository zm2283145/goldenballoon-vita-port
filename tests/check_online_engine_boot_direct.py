#!/usr/bin/env python3
"""Prove the VISIBLE engine reaches the ONLINE race with NO menu-nav input script.

This is the headline online-UX gate. The sibling check_online_engine_boot.py
drove the front-end to Ancient Lake by replaying tests/input_scripts/
race_2p_split.txt -- a hand-authored, time-coded walk of DKR's Title ->
Character Select -> Track Select menus. A real online player must NEVER touch or
even see those single-player screens.

Here the same in-process two-adapter live session is stood up (real libdatachannel
DTLS over the loopback hub), the roster + launch descriptor are installed
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
from online_lane_util import (
    DIRECT_BOOT_RE, ENGINE_LIVE_RE, FORBIDDEN_ONLINE, ONLINE_RACE_RE,
    forbidden_marker, make_fail, run_engine,
)

ROOT = Path(__file__).resolve().parent.parent
# With the direct-boot seam the race level (rollback runtime active) is reached a
# few dozen ticks after cold boot instead of ~2491, so nearly the whole budget is
# authored racing. Keep 3000 for a long convergence window.
TICKS = 3000

# The GOLDEN canonical race hash for the default (vote-track-5) direct-boot flow --
# the load-bearing "the online boot reaches the SAME deterministic race sim as
# offline" invariant. Convergence only asserts hash_visible == hash_peer (the two
# endpoints in ONE run agree with EACH OTHER), so a determinism drift that stayed
# peer-consistent (RNG seed / roster order / physics tick) would keep every lane
# green while the hash silently became a DIFFERENT value (tests I-1). Pinning the
# literal here closes that overclaim: the observed hash must EQUAL this golden.
# A LEGITIMATE ROM/toolchain change is a one-line update here, never a silent green.
# Skipped when --track/--mask change the sim (a different track is a different hash).
GOLDEN_RACE_HASH = "db805fd2ee15d3ca"

# Printed by the wiring only when a session-config env seam is set (--track).
CONFIG_TRACK_RE = re.compile(
    r"^\[online-live\] loopback config mode=single track=(\d+) "
    r"startMask=0x([0-9a-f]{2}) vehicle=(\d+)$",
    re.MULTILINE,
)

# The route-opacity witness (armed below): one line per default-arm pickup (weapon
# balloon / banana) that reached a viewport route store, tagged STALE when the
# cached render opacity OR the cached visible render-gate differed -- in either
# direction -- from the value check_if_in_draw_range wrote for THIS viewport
# (another canonical seat's distance fade / obstruction).
ROUTE_OPACITY_RE = re.compile(
    r"^\[route-opacity-witness\] pickup behavior=\d+ storedOpacity=-?\d+ "
    r"freshOpacity=-?\d+ storedVisible=-?\d+ freshVisible=-?\d+( STALE)?$",
    re.MULTILINE,
)


fail = make_fail("engine boot (direct)")


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
    parser.add_argument(
        "--expect-hash", default=GOLDEN_RACE_HASH,
        help="assert the converged race hash EQUALS this golden literal (default "
             f"{GOLDEN_RACE_HASH}); pass '' to skip. Auto-skipped when --track/"
             "--mask change the sim. Update on a legit ROM/toolchain change.")
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()
    if args.mask is not None and args.track is None:
        parser.error("--mask requires --track")

    binary = Path(resolve_binary(args.build)).expanduser().resolve()
    rom = args.rom.expanduser().resolve()
    for path, label in ((binary, "binary"), (rom, "ROM")):
        if not path.is_file():
            parser.error(f"missing {label}: {path}")

    # Deliberately NO MDKR_APP_AUTOPLAY_INPUT_SCRIPT: the race must be reached from
    # the manifest + boot alone. MDKR_TEST_SCRIPT_ONLY_INPUT (in the shared env)
    # stays on so no stray host input can reach the game -- the only inputs are the
    # live match transport (canonical) and MDKR_AUTOPILOT (driving line).
    extra_env = {
        "MDKR_APP_TEST_ONLINE_LIVE": "1",
        # Blend-pass route-opacity regression guard: arm the witness so this online
        # race (two canonical viewports sharing one obj->opacity) asserts that no
        # default-arm pickup caches another seat's stale distance fade.
        "MDKR_TEST_ROUTE_OPACITY_WITNESS": "1",
    }
    if args.track is not None:
        extra_env["MDKR_APP_TEST_ONLINE_TRACK"] = str(args.track)
    try:
        returncode, output = run_engine(
            binary, rom, ticks=args.ticks, timeout=args.timeout,
            verbose=args.verbose, extra_env=extra_env,
            prefix="mdkr64-online-direct-")
    except subprocess.TimeoutExpired as error:
        return fail(f"engine run timed out (a stall would look like this): "
                    f"{error}")

    marker = forbidden_marker(output, *FORBIDDEN_ONLINE)
    if marker:
        return fail(f"observed forbidden marker {marker!r}", output)

    if returncode != 0:
        return fail(f"process exited {returncode}", output)

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

    # tests I-1: pin the GOLDEN literal on the unaltered default sim. Peer==peer
    # above proves the two endpoints agree; this proves they agree on the SAME
    # canonical value offline reaches, so a peer-consistent determinism drift is
    # no longer invisible. A --track/--mask run changes the sim, so skip it there.
    if args.track is None and args.mask is None and args.expect_hash:
        if hash_visible != args.expect_hash:
            return fail(
                f"the converged race hash {hash_visible} != the GOLDEN "
                f"{args.expect_hash} -- the online boot no longer reaches the "
                f"canonical deterministic race sim (a determinism drift, or a "
                f"legit ROM/toolchain change that needs GOLDEN_RACE_HASH bumped)",
                output)

    # The blend-pass route-opacity guard. Online presents two canonical viewports
    # sharing one obj->opacity; the transparent pass cached the PRE-refresh opacity
    # (another seat's distance fade) rather than the value check_if_in_draw_range
    # wrote for THIS viewport, so a local pickup drew translucent. The witness
    # reports every default-arm pickup that reached the blend route store; a STALE
    # tag means the cached value was cross-seat. The scenario must exercise pickups
    # at all (>=1 line, a positive control) and, post-fix, none may be STALE.
    route_lines = ROUTE_OPACITY_RE.findall(output)
    if not route_lines:
        return fail("the route-opacity witness never fired -- no pickup reached "
                    "the blend-pass route store, so this run cannot prove the "
                    "fix (positive control)", output)
    stale = [tag for tag in route_lines if tag == " STALE"]
    if stale:
        return fail(
            f"{len(stale)} of {len(route_lines)} pickup blend-route store(s) "
            f"cached a STALE cross-seat distance fade -- a local pickup drew "
            f"translucent; expected none post-fix", output)

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
        f"convergedTicks={fold_visible} hash={hash_visible}"
        + ("==GOLDEN" if (args.track is None and args.mask is None and
                          args.expect_hash) else "")
        + " engineExit=clean noStall=1" + narrow
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
