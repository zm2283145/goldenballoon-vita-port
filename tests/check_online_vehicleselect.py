#!/usr/bin/env python3
"""Prove the native online VEHICLE stage of the track screen.

Retail 2P picks vehicles AFTER the track, as a stage of the track-select screen
(menu.c trackmenu_setup_render). THIS lane proves that stage for the native flow
(game/src/online/online_vehicleselect.c): the session fronts it from TRACKSELECT
once the host's pick is locked (charselect -> track browse -> lock -> VEHICLES ->
OK -> race), and the player picks car / hovercraft / plane there.

The crux the recon called out still holds: the party_link reverse feed ALREADY
carries vehicle_id and the reducer ALREADY validates CHOOSE_VEHICLE + refuses
START (BEGIN_LOADING) with ILLEGAL_VEHICLE, so the stage only drives
intent.vehicle_id from the player's pick, maps the retail per-player CONFIRM onto
intent.ready, and republishes the host's locked config + the OK
(start_requested). This lane closes that pick -> intent -> reducer -> snapshot
round trip headless, with the SAME in-process two-adapter live session the other
screen lanes use, over three scenarios keyed on the MDKR_TEST_ONLINE_VEHICLESELECT
env value:

  * "1"       SKIP-CLAMP + OK + BOOT: the combined lane pins Whale Bay (track 8,
              hovercraft-only 0x2). Retail's pick cycle SKIPS unavailable
              vehicles and CLAMPS at the ends (menu.c:12066-12086) -- it can
              never even HOVER an illegal vehicle -- so the scripted input tries
              to cycle BOTH ways (the pick must stay HOVERCRAFT, never CAR/PLANE)
              then confirms and the host OKs. The session runs the retail order
              CHARSELECT -> TRACKSELECT -> VEHICLE stage (with the browse B-back
              to CHARSELECT and the stage B-back to the browse exercised), the OK
              starts the race, and the two endpoints converge byte-for-byte on
              track 8. R3: the PUBLISHED vehicle is legal every frame and START
              is never refused.
  * "diverge" DIVERGENT PICK: no track pinned by the stage seam (all three
              legal), so the local seat picks PLANE (2) while the scripted remote
              keeps CAR (0) -- the two endpoints hold DIFFERENT vehicles and BOTH
              converge in the snapshot (the per-seat vehicle two-endpoint proof).
  * "unknown" FAIL-CLOSED mask contract (see check_unknown below).
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
UNKNOWN_TRACK = 900    # out-of-range resolved track: the fail-closed mask probe
ALL_VEHICLES = 0x7     # car+hovercraft+plane -- the permissive mask that must NOT appear
HOVERCRAFT = 1         # VEHICLE_HOVERCRAFT -- the only legal pick for Whale Bay
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
    r"intent\{vehicle=(\d+) ready=(\d+)\}$", re.MULTILINE)
# findall tuple indices:
#  0 cursor 1 vehicle 2 legal 3 track 4 seatVeh 5 seatReady 6 conf
#  7 rSeat 8 rVeh 9 rReady 10 rName 11 intentVeh 12 intentReady
VS_ADVANCE_RE = re.compile(
    r"^\[online-vehicleselect\] advance: lobby left LOBBY", re.MULTILINE)
VS_EXIT_RE = re.compile(
    r"^\[online-vehicleselect\] exit: freed vehicle-stage assets",
    re.MULTILINE)
VS_OK_RE = re.compile(
    r"^\[online-vehicleselect\] host OK -> start requested$", re.MULTILINE)
VS_REV_RE = re.compile(
    r"^\[online-vehicleselect\] all vehicles confirmed \(CAR_REV2\)$",
    re.MULTILINE)
VS_BACK_RE = re.compile(
    r"^\[online-vehicleselect\] back to track browse$", re.MULTILINE)
# The DEFERRED-START truthfulness witness: the host's OK is latched but the
# rival is NOT (or no longer) ready, so the reducer would refuse BEGIN_LOADING
# -- the footer must stop claiming "STARTING..." and keep the B escape visible.
VS_DEFER_RE = re.compile(
    r"^\[online-vehicleselect\] start deferred: start requested but the rival "
    r"is not ready \(footer: waiting \+ B backs out\)$", re.MULTILINE)
SESS_CS_TO_TS_RE = re.compile(
    r"^\[online-session\] charselect -> trackselect", re.MULTILINE)
SESS_TS_TO_VS_RE = re.compile(
    r"^\[online-session\] trackselect -> vehicleselect \(track locked",
    re.MULTILINE)
SESS_VS_TO_TS_RE = re.compile(
    r"^\[online-session\] vehicleselect -> trackselect \(back one stage\)",
    re.MULTILINE)


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


def check_skip_boot(output: str) -> int | None:
    scn = "skip-boot"
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
    if not SESS_CS_TO_TS_RE.search(output):
        return fail(f"[{scn}] CHARSELECT never handed off to TRACKSELECT (retail "
                    f"order: the track browse follows PLAYER SELECT)", output)
    ts_enter = TS_ENTER_RE.search(output)
    if ts_enter is None:
        return fail(f"[{scn}] TRACKSELECT was never entered", output)
    to_vs = SESS_TS_TO_VS_RE.search(output)
    if to_vs is None:
        return fail(f"[{scn}] the track lock never handed off to the VEHICLE "
                    f"stage (retail order: vehicles AFTER the track)", output)
    enter = VS_ENTER_RE.search(output)
    if not enter:
        return fail(f"[{scn}] the VEHICLE stage was never entered", output)
    # Ordering: charselect enter < trackselect enter < vehicle-stage enter.
    if not (CS_ENTER_RE.search(output).start() < ts_enter.start() < enter.start()):
        return fail(f"[{scn}] the CHARSELECT -> TRACKSELECT -> VEHICLE-stage "
                    f"ordering was violated", output)

    renders = VS_RENDER_RE.findall(output)
    if not renders:
        return fail(f"[{scn}] the VEHICLE stage produced no render witnesses",
                    output)
    err = assert_r3_published_legal(scn, renders, output)
    if err is not None:
        return err

    # --- Retail skip-clamp: on the hovercraft-only track the pick can NEVER
    # leave HOVERCRAFT (the scripted input tries both cycle directions). -------
    wb_rows = [r for r in renders if int(r[3]) == LOCKED_TRACK]
    if not wb_rows:
        return fail(f"[{scn}] no render row resolved track {LOCKED_TRACK}", output)
    strays = [r for r in wb_rows if int(r[1]) != HOVERCRAFT]
    if strays:
        return fail(f"[{scn}] the pick left HOVERCRAFT on the hovercraft-only "
                    f"track (retail skip-clamp broken): {strays[:3]}", output)
    if any(int(r[2], 16) != LOCKED_MASK for r in wb_rows):
        return fail(f"[{scn}] a Whale Bay row resolved a mask != "
                    f"0x{LOCKED_MASK:x}", output)

    # --- Local seat CONVERGED to the pick (hovercraft) -----------------------
    converged = [r for r in wb_rows
                 if int(r[4]) == HOVERCRAFT and int(r[11]) == HOVERCRAFT]
    if not converged:
        return fail(f"[{scn}] the local seat never converged to HOVERCRAFT "
                    f"({HOVERCRAFT}) (seatVeh + intent)", output)

    # --- The retail beats: both confirmed -> CAR_REV2 -> host OK -> start ----
    if not VS_REV_RE.search(output):
        return fail(f"[{scn}] the all-vehicles-confirmed CAR_REV2 beat never "
                    f"fired", output)
    if not VS_OK_RE.search(output):
        return fail(f"[{scn}] the host OK (start request) never fired", output)

    # (The stage back-stack round trip -- vehicle-stage B -> browse -> re-lock ->
    # stage -- is proven by check_online_trackselect.py's single-host scenario,
    # whose choreography owns the B beats.)

    if not VS_ADVANCE_RE.search(output):
        return fail(f"[{scn}] the stage never advanced on the room leaving "
                    f"LOBBY", output)
    if not VS_EXIT_RE.search(output):
        return fail(f"[{scn}] the VEHICLE stage never freed its assets on exit",
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

    if not SESS_TS_TO_VS_RE.search(output):
        return fail(f"[{scn}] the track lock never handed off to the VEHICLE "
                    f"stage", output)
    renders = VS_RENDER_RE.findall(output)
    if not renders:
        return fail(f"[{scn}] the VEHICLE stage produced no render witnesses",
                    output)

    err = assert_r3_published_legal(scn, renders, output)
    if err is not None:
        return err

    # --- Two endpoints hold DIFFERENT vehicles and BOTH converge ------------
    # Local seat converges to PLANE (2), the scripted remote keeps CAR (0); both
    # are present and legal (all three legal on the stage-seam's unpinned track).
    diverged = [
        r for r in renders
        if int(r[4]) == PLANE and int(r[8]) == CAR and int(r[7]) >= 0
        and int(r[11]) == PLANE
    ]
    if not diverged:
        return fail(f"[{scn}] no render row showed the two seats diverged + "
                    f"converged (local seatVeh={PLANE} plane, remote veh={CAR} "
                    f"car)", output)

    # The confirm latched ready over the reverse feed (intent ready=1 with the
    # divergent pick still published).
    ready_rows = [r for r in diverged if int(r[12]) == 1]
    if not ready_rows:
        return fail(f"[{scn}] the confirmed divergent pick never published "
                    f"ready=1", output)
    return None


def check_unknown(output: str) -> int | None:
    """FAIL-CLOSED contract: a resolved OUT-OF-RANGE track must yield the engine-truth
    base mask (leveltable fail-closes an unknown id to CAR-only 0x1), NEVER the
    permissive ALL (0x7) and never a silent CAR substituted over an empty mask. Note:
    this is a contract guard, not a red-first probe -- leveltable's own out-of-range
    -> CAR fallback means an unknown id can never reach the removed fail-open branch;
    that branch only fired for a malformed in-range table entry, which no reachable
    track id produces (see the vehicleselect_track_mask comment)."""
    scn = "unknown"
    marker = forbidden_marker(output, *FORBIDDEN_ONLINE)
    if marker:
        return fail(f"[{scn}] observed forbidden marker {marker!r}", output)
    if not VS_ENTER_RE.search(output):
        return fail(f"[{scn}] the VEHICLE stage was never entered", output)
    renders = VS_RENDER_RE.findall(output)
    if not renders:
        return fail(f"[{scn}] the VEHICLE stage produced no render witnesses",
                    output)
    unknown_rows = [r for r in renders if int(r[3]) == UNKNOWN_TRACK]
    if not unknown_rows:
        return fail(f"[{scn}] no render row resolved the out-of-range track "
                    f"{UNKNOWN_TRACK} (the seam did not pin it)", output)
    for r in unknown_rows:
        legal = int(r[2], 16)
        if legal == ALL_VEHICLES:
            return fail(f"[{scn}] FAIL-OPEN: the unknown track offered ALL three "
                        f"vehicles (mask 0x{legal:x}) instead of failing closed "
                        f"(row={r})", output)
        if legal != (1 << CAR):
            return fail(f"[{scn}] the unknown track's mask was 0x{legal:x}; expected "
                        f"engine-truth CAR-only 0x1 (fail-closed)", output)
    # R3 still holds: the published vehicle is inside the fail-closed mask.
    return assert_r3_published_legal(scn, unknown_rows, output)


def check_defer(output: str) -> int | None:
    """DEFERRED-START truthfulness (consistency audit #1): the host OKs while
    both seats read ready, then the rival UN-readies before BEGIN_LOADING can
    land -- the reducer refuses (NOT_READY) and the room stays in LOBBY. The
    stage must (a) witness the deferred window (the footer flips from
    "STARTING..." to the truthful WAITING-FOR-RIVAL line with the B escape
    advertised), (b) honor B during the deferral (backs the start request +
    confirm out -- intent ready drops to 0), and (c) never fake an advance
    (the room never left LOBBY). RED pre-fix: the defer witness does not exist
    ("STARTING..." sat unchanged with the B hint hidden)."""
    scn = "defer"
    marker = forbidden_marker(output, *FORBIDDEN_ONLINE)
    if marker:
        return fail(f"[{scn}] observed forbidden marker {marker!r}", output)
    if not VS_OK_RE.search(output):
        return fail(f"[{scn}] the host OK (start request) never fired -- the "
                    f"deferred window was never staged", output)
    defer = VS_DEFER_RE.search(output)
    if not defer:
        return fail(f"[{scn}] the stage never witnessed the DEFERRED start "
                    f"(startReq latched with the rival un-ready): the footer "
                    f"still claims 'STARTING...' through the refused window",
                    output)
    # (b) B backs out DURING the deferral: after the defer witness, a render
    # row shows the local seat un-confirmed with intent ready=0.
    backed = [m for m in VS_RENDER_RE.finditer(output)
              if m.start() > defer.end() and int(m.group(7)) == 0
              and int(m.group(13)) == 0]
    if not backed:
        return fail(f"[{scn}] B did not back the deferred start out (no "
                    f"un-confirmed render row after the defer witness)", output)
    # (c) the refused start never advanced the stage.
    if VS_ADVANCE_RE.search(output):
        return fail(f"[{scn}] the stage advanced although the start stayed "
                    f"refused (the room must not leave LOBBY)", output)
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
        ("1", check_skip_boot,
         {"MDKR_TEST_ONLINE_TRACKSELECT": "1",
          "MDKR_APP_TEST_ONLINE_TRACK": str(LOCKED_TRACK)}, args.ticks,
         args.timeout),
        ("diverge", check_diverge, {}, 2000, args.timeout),
        ("unknown", check_unknown, {}, 2000, args.timeout),
        ("defer", check_defer, {}, 2000, args.timeout),
    )
    for vs_value, checker, extra, ticks, timeout in scenarios:
        try:
            rc, output = run(binary, rom, vs_value, ticks, timeout, args.verbose,
                             extra)
        except subprocess.TimeoutExpired as error:
            return fail(f"[{vs_value}] engine run timed out (a VEHICLE-stage "
                        f"stall would look like this): {error}")
        if rc != 0:
            return fail(f"[{vs_value}] process exited {rc}", output)
        result = checker(output)
        if result is not None:
            return result

    print(
        "PASS online vehicleselect: native VEHICLE stage of the track screen "
        "(retail order: charselect -> track browse -> lock -> vehicles) -- "
        "SKIP-CLAMP+OK+BOOT (the pick cycles only inside hovercraft-only Whale "
        "Bay's mask, both seats confirmed -> CAR_REV2 -> host OK -> race booted "
        "track 8, endpoints converged); DIVERGE (local PLANE vs remote "
        "CAR both converged, ready published on the divergent pick); UNKNOWN "
        "(out-of-range track fails CLOSED to the engine-truth CAR-only mask); "
        "DEFER (a start latched against an un-readied rival is WITNESSED as "
        "deferred -- truthful waiting footer with the B escape -- B backs it "
        "out, and the refused start never advances). "
        "R3 held on every frame; gGameMode=2 gCurrentMenuId=0 at hand-off.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
