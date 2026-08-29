#!/usr/bin/env python3
"""Prove the native online VEHICLE SELECT screen (T1, Strategy D2).

Where check_online_charselect.py proves the FIRST player-facing screen of the
separated online path and check_online_trackselect.py the track/cup screen, THIS
lane proves the NEW screen inserted between them (game/src/online/
online_vehicleselect.c): the player CHOOSES car / hovercraft / plane. Before it the
native flow only auto-narrowed a DEFAULT vehicle -- the player never picked.

The crux the recon called out: the party_link reverse feed ALREADY carries
vehicle_id and the reducer ALREADY validates CHOOSE_VEHICLE + refuses START
(BEGIN_LOADING) with ILLEGAL_VEHICLE, so the screen only drives intent.vehicle_id
from a player cursor. This lane closes that cursor -> intent -> reducer -> snapshot
round trip headless, with the SAME in-process two-adapter live session the other
screen lanes use, over TWO scenarios keyed on the MDKR_TEST_ONLINE_VEHICLESELECT
env value:

  * "1"       REJECT + CHAIN + BOOT: the combined lane pins Whale Bay (track 8,
              hovercraft-only 0x2), so the cursor seeds on the only legal slot
              (hovercraft, id 1 -- already DIFFERENT from the CHARSELECT default
              CAR 0). The scripted input moves to CAR and presses A to prove the
              ILLEGAL pick is REJECTED (buzz, no change, no ILLEGAL_VEHICLE ever),
              then confirms hovercraft. The session advances
              CHARSELECT -> VEHICLESELECT -> TRACKSELECT (and the TRACKSELECT B-back
              proves the back-stack TRACKSELECT -> VEHICLESELECT), the host starts,
              and the two endpoints converge byte-for-byte on track 8. R3: the
              PUBLISHED vehicle is legal every frame and START is never refused.
  * "diverge" DIVERGENT PICK: no track pinned (all three legal), so the local seat
              picks PLANE (2) while the scripted remote keeps CAR (0) -- the two
              endpoints hold DIFFERENT vehicles and BOTH converge in the snapshot
              (the per-seat vehicle two-endpoint proof).
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

from harness_utils import resolve_binary
from online_lane_util import (
    DIRECT_BOOT_RE, ENGINE_LIVE_RE, FORBIDDEN_ONLINE, GAMEMODE_ONLINE_SESSION,
    SESSION_RACE_RE, forbidden_marker, make_fail, run_engine,
)

ROOT = Path(__file__).resolve().parent.parent
TICKS = 3000

LOCKED_TRACK = 8       # Whale Bay (cup 2 round 0), hovercraft-only 0x2
LOCKED_MASK = 0x2
HOVERCRAFT = 1         # VEHICLE_HOVERCRAFT -- the legal pick for Whale Bay
CAR = 0                # VEHICLE_CAR -- the CHARSELECT default; illegal for Whale Bay
PLANE = 2              # VEHICLE_PLANE -- the DIVERGE local pick

CS_ENTER_RE = re.compile(r"^\[online-charselect\] enter:", re.MULTILINE)
TS_ENTER_RE = re.compile(r"^\[online-trackselect\] enter:", re.MULTILINE)
VS_ENTER_RE = re.compile(
    r"^\[online-vehicleselect\] enter: native screen up character=(\d+) "
    r"vehicle=(\d+) track=(\d+) mask=0x([0-9a-f]+)", re.MULTILINE)
VS_RENDER_RE = re.compile(
    r"^\[online-vehicleselect\] render cursor=(\d+) vehicle=(\d+) "
    r"legal=0x([0-9a-f]+) track=(\d+) local\{seatVeh=(\d+) seatReady=(\d+) "
    r"conf=(\d+)\} remote\{seat=(-?\d+) veh=(\d+) ready=(\d+) name=(\S+)\} "
    r"intent\{vehicle=(\d+) ready=1\}$", re.MULTILINE)
# findall tuple indices:
#  0 cursor 1 vehicle 2 legal 3 track 4 seatVeh 5 seatReady 6 conf
#  7 rSeat 8 rVeh 9 rReady 10 rName 11 intentVeh
VS_REJECT_RE = re.compile(
    r"^\[online-vehicleselect\] reject vehicle=(\d+) \(illegal for track=(\d+) "
    r"mask=0x([0-9a-f]+)\)$", re.MULTILINE)
VS_ADVANCE_RE = re.compile(
    r"^\[online-vehicleselect\] advance: lobby left LOBBY", re.MULTILINE)
VS_EXIT_RE = re.compile(
    r"^\[online-vehicleselect\] exit: freed portrait \+ vehicle assets",
    re.MULTILINE)
SESS_CS_TO_VS_RE = re.compile(
    r"^\[online-session\] charselect -> vehicleselect", re.MULTILINE)
SESS_VS_TO_TS_RE = re.compile(
    r"^\[online-session\] vehicleselect -> trackselect", re.MULTILINE)
SESS_TS_TO_VS_RE = re.compile(
    r"^\[online-session\] trackselect -> vehicleselect", re.MULTILINE)


fail = make_fail("vehicleselect")


def assert_r3_published_legal(scn: str, renders, output: str) -> int | None:
    """R3 core invariant: the PUBLISHED (intent) vehicle is inside the row's own
    legal mask on EVERY frame -- so the seat can never READY / START illegal."""
    for r in renders:
        legal = int(r[2], 16)
        intent_veh = int(r[11])
        if intent_veh >= 3 or ((1 << intent_veh) & legal) == 0:
            return fail(f"[{scn}] R3 VIOLATED: a render row published vehicle "
                        f"{intent_veh} outside the legal mask 0x{legal:x} "
                        f"(row={r})", output)
    return None


def run(binary, rom, vs_value, ticks, timeout, verbose, extra):
    seams = {
        "MDKR_APP_TEST_ONLINE_LIVE": "1",
        "MDKR_TEST_ONLINE_CHARSELECT": "1",
        "MDKR_TEST_ONLINE_VEHICLESELECT": vs_value,
    }
    seams.update(extra)
    return run_engine(binary, rom, ticks=ticks, timeout=timeout, verbose=verbose,
                      extra_env=seams, prefix="mdkr64-online-vehicleselect-")


def check_reject_boot(output: str) -> int | None:
    scn = "reject-boot"
    marker = forbidden_marker(output, *FORBIDDEN_ONLINE)
    if marker:
        return fail(f"[{scn}] observed forbidden marker {marker!r}", output)
    if "ILLEGAL_VEHICLE" in output:
        return fail(f"[{scn}] a BEGIN_LOADING ILLEGAL_VEHICLE refusal was observed "
                    f"-- R3 failed (a seat readied/started with an illegal "
                    f"vehicle)", output)

    if not CS_ENTER_RE.search(output):
        return fail(f"[{scn}] CHARSELECT never entered (flow must pass through it)",
                    output)
    if not SESS_CS_TO_VS_RE.search(output):
        return fail(f"[{scn}] CHARSELECT never handed off to VEHICLESELECT", output)
    enter = VS_ENTER_RE.search(output)
    if not enter:
        return fail(f"[{scn}] the VEHICLE screen was never entered", output)

    renders = VS_RENDER_RE.findall(output)
    if not renders:
        return fail(f"[{scn}] the VEHICLE screen produced no render witnesses",
                    output)

    err = assert_r3_published_legal(scn, renders, output)
    if err is not None:
        return err

    # --- Illegal pick REJECTED (CAR on the hovercraft-only track) ------------
    rejects = VS_REJECT_RE.findall(output)
    car_rejects = [r for r in rejects
                   if int(r[0]) == CAR and int(r[2], 16) == LOCKED_MASK]
    if not car_rejects:
        return fail(f"[{scn}] the illegal CAR pick was never REJECTED on the "
                    f"hovercraft-only track (rejects seen: {rejects!r})", output)

    # --- Local seat CONVERGED to the CHOSEN vehicle (hovercraft) -------------
    # Non-vacuous: the CHARSELECT default is CAR (0); the screen chose HOVERCRAFT.
    converged = [r for r in renders
                 if int(r[4]) == HOVERCRAFT and int(r[11]) == HOVERCRAFT]
    if not converged:
        return fail(f"[{scn}] the local seat never converged to the chosen vehicle "
                    f"HOVERCRAFT ({HOVERCRAFT}) (seatVeh + intent)", output)

    # --- Phase chain CHARSELECT -> VEHICLESELECT -> TRACKSELECT --------------
    to_ts = SESS_VS_TO_TS_RE.search(output)
    if to_ts is None:
        return fail(f"[{scn}] VEHICLESELECT never handed off to TRACKSELECT (the "
                    f"CHARSELECT->VEHICLE->TRACKSELECT chain broke)", output)
    ts_enter = TS_ENTER_RE.search(output)
    if ts_enter is None:
        return fail(f"[{scn}] TRACKSELECT was never entered after VEHICLESELECT",
                    output)
    # Ordering: charselect enter < vehicle enter < vehicle->trackselect.
    if not (CS_ENTER_RE.search(output).start() < enter.start() < to_ts.start()):
        return fail(f"[{scn}] the CHARSELECT -> VEHICLESELECT -> TRACKSELECT "
                    f"ordering was violated", output)

    # --- Back-stack bonus: TRACKSELECT B returned to VEHICLESELECT -----------
    if not SESS_TS_TO_VS_RE.search(output):
        return fail(f"[{scn}] TRACKSELECT B-back never returned to VEHICLESELECT "
                    f"(the native back-stack TRACKSELECT->VEHICLE broke)", output)

    if not VS_EXIT_RE.search(output):
        return fail(f"[{scn}] the VEHICLE screen never freed its assets on exit",
                    output)

    # --- Offline isolation preserved + the race booted and converged --------
    race = SESSION_RACE_RE.findall(output)
    if len(race) != 1:
        return fail(f"[{scn}] expected exactly one session RACE hand-off, got "
                    f"{race!r}", output)
    _t, boot_gamemode, boot_menu_id = race[0]
    if int(boot_gamemode) != GAMEMODE_ONLINE_SESSION:
        return fail(f"[{scn}] at hand-off gGameMode={boot_gamemode}, expected "
                    f"{GAMEMODE_ONLINE_SESSION}", output)
    if int(boot_menu_id) != 0:
        return fail(f"[{scn}] gCurrentMenuId={boot_menu_id} at hand-off -- the "
                    f"offline menu state machine was entered (expected 0)", output)
    if "input-script" in output or "race_2p_split" in output:
        return fail(f"[{scn}] a menu-nav input script was loaded", output)
    direct = DIRECT_BOOT_RE.findall(output)
    if not direct or int(direct[0][0]) != LOCKED_TRACK:
        return fail(f"[{scn}] the race did not boot on track {LOCKED_TRACK} "
                    f"(direct boot={direct!r})", output)
    stats = ENGINE_LIVE_RE.findall(output)
    if len(stats) != 1:
        return fail(f"[{scn}] expected one ENGINE-ONLINE-LIVE witness, got "
                    f"{stats!r}", output)
    (result, raced, drains, advance_failed, _e, _a, _c, _d, fold_visible, _fp,
     hash_visible, hash_peer, converged_flag) = stats[0]
    if int(result) != 0 or int(advance_failed) != 0:
        return fail(f"[{scn}] the live race failed (result={result} "
                    f"advanceFailed={advance_failed})", output)
    if int(raced) < 100 or int(drains) < 100:
        return fail(f"[{scn}] the race did not sustain ticks (raced={raced})",
                    output)
    if not (int(converged_flag) == 1 and hash_visible == hash_peer and
            int(fold_visible) >= 20):
        return fail(f"[{scn}] the two endpoints did not converge "
                    f"(converged={converged_flag} v={hash_visible} "
                    f"p={hash_peer})", output)
    return None


def check_diverge(output: str) -> int | None:
    scn = "diverge"
    marker = forbidden_marker(output, *FORBIDDEN_ONLINE)
    if marker:
        return fail(f"[{scn}] observed forbidden marker {marker!r}", output)

    if not SESS_CS_TO_VS_RE.search(output):
        return fail(f"[{scn}] CHARSELECT never handed off to VEHICLESELECT", output)
    renders = VS_RENDER_RE.findall(output)
    if not renders:
        return fail(f"[{scn}] the VEHICLE screen produced no render witnesses",
                    output)

    err = assert_r3_published_legal(scn, renders, output)
    if err is not None:
        return err

    # --- Two endpoints hold DIFFERENT vehicles and BOTH converge ------------
    # Local seat converges to PLANE (2), the scripted remote keeps CAR (0); both
    # are present and legal (all three legal on the unpinned track).
    diverged = [
        r for r in renders
        if int(r[4]) == PLANE and int(r[8]) == CAR and int(r[7]) >= 0
        and int(r[11]) == PLANE
    ]
    if not diverged:
        return fail(f"[{scn}] no render row showed the two seats diverged + "
                    f"converged (local seatVeh={PLANE} plane, remote veh={CAR} "
                    f"car)", output)

    # The confirm advanced the flow (chain still reaches TRACKSELECT).
    if not SESS_VS_TO_TS_RE.search(output):
        return fail(f"[{scn}] the confirmed vehicle never advanced "
                    f"VEHICLESELECT -> TRACKSELECT", output)
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

    scenarios = (
        ("1", check_reject_boot,
         {"MDKR_TEST_ONLINE_TRACKSELECT": "1",
          "MDKR_APP_TEST_ONLINE_TRACK": str(LOCKED_TRACK)}, args.ticks,
         args.timeout),
        ("diverge", check_diverge, {}, 2000, args.timeout),
    )
    for vs_value, checker, extra, ticks, timeout in scenarios:
        try:
            rc, output = run(binary, rom, vs_value, ticks, timeout, args.verbose,
                             extra)
        except subprocess.TimeoutExpired as error:
            return fail(f"[{vs_value}] engine run timed out (a VEHICLE-select "
                        f"stall would look like this): {error}")
        if rc != 0:
            return fail(f"[{vs_value}] process exited {rc}", output)
        result = checker(output)
        if result is not None:
            return result

    print(
        "PASS online vehicleselect: native VEHICLE select -- REJECT+BOOT (illegal "
        "CAR rejected on hovercraft-only Whale Bay; local seat converged to the "
        "chosen HOVERCRAFT (!= CHARSELECT default CAR); phase advanced "
        "CHARSELECT->VEHICLESELECT->TRACKSELECT with the TRACKSELECT->VEHICLESELECT "
        "back-stack; R3 held -- published vehicle legal every frame, no "
        "ILLEGAL_VEHICLE; race booted track 8, two endpoints converged) and "
        "DIVERGE (local PLANE vs remote CAR -- both seats picked, both converged) "
        "-- gGameMode=2 gCurrentMenuId=0, offline menu bypassed throughout"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
