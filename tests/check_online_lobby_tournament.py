#!/usr/bin/env python3
"""PD-T6h2b KEYSTONE proof: a FULL descriptor-less TOURNAMENT in ONE engine process.

Composes the T6h2a lobby-start boot (the engine BEGINS descriptor-less and its
native CHARSELECT -> TRACKSELECT own race 1) with the T6ac/T6h1 resident multi-race
coordinator, so a descriptor-less session runs the demo's real flow end to end
IN-PROCESS:

  lobby-start begin (no descriptor) -> native CHARSELECT -> TRACKSELECT -> START
    -> race 1 boots (T6h2a gate) -> RESULTS (real reducer snapshot) -> REMATCH
    -> race 2 -> race 3 -> race 4 -> FINAL standings HELD (feed-final; no 5th boot)

The lobby-start coordinator fronts race 1, then HANDS OFF to the resident
coordinator (g_liveResident) so rounds 2..4 re-cycle via the SAME mid-residency
PUBLISH_RESULTS + frame-stepped OnlineRoom_residentAdvanceStep the resident lane
uses. Finality is FEED-derived (MDKR_ONLINE_SESSION_CUP_ROUNDS): a descriptor-less
session has no MDKR_APP_TEST_ONLINE_LIVE_RESIDENT env to size the run.

Two-endpoint loopback (peer != nullptr), tournament cup 1 (Snowflake: rounds
13/6/9/28 -- all Car-legal, so the native Car defaults never fail admission).

Primary assertions (the deliverable):
  (a) EXACTLY MDKR_ONLINE_SESSION_CUP_ROUNDS (4) `[online-boot] direct race:` boots
      in ONE process, on the cup-1 schedule tracks [13,6,9,28]
  (b) the session BEGAN descriptor-less (1 lobby-start begin, 0 descriptor-first)
      and NATIVE CHARSELECT then TRACKSELECT fronted race 1
  (c) it COMPOSED: handed off to the resident coordinator for races 2..4
  (d) native RESULTS fronts each round on the REAL reducer snapshot (haveResults=1,
      a valid finish order), and feed-derived isFinal is 0 for races 1-3 and 1 at
      race 4 ONLY
  (e) points accrue by trophy weight (strictly increasing per race)
  (f) race_index advances 0->1->2->3 via the reverse-feed REMATCH (3 publishes, 3
      observed advances, 3 per-round re-cycles on FRESH epochs)
  (g) every RACE hand-off is GAMEMODE_ONLINE_SESSION with gCurrentMenuId=0
  (h) no NULL deref, no admission reject, no watchdog trip, clean exit 0

Wedge sub-tests (the deferred safety findings, proven to FIRE cleanly, never hang):
  (W1) START but the descriptor/match-input never arms -> the WALL-CLOCK WATCHDOG
       fires + routes to a clean exit (bounded), NOT a hang
  (W2) the room returns to LOBBY while a boot is pending (leader CANCEL_LOADING) ->
       the session UNWINDS + re-fronts CHARSELECT (never parks), and RECOVERS (race
       1 still boots)

Minor-3 pin: MDKR_ONLINE_SESSION_CUP_ROUNDS (online_session.c) is asserted equal to
MDKR_ONLINE_CUP_ROUNDS (the reducer / track-table source of truth) by source scan.
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
    CUP_ROUNDS, DIRECT_BOOT_RE, FINISHED_ENGINE_RE, FORBIDDEN_ONLINE,
    GAMEMODE_ONLINE_SESSION, PLACE_NONE, SESSION_END_RE, SESSION_RACE_RE,
    forbidden_marker, make_fail,
)
from online_lane_util import run_engine as _run_engine

ROOT = Path(__file__).resolve().parent.parent
TICKS = 30000
CUP = 1
CUP1_TRACKS = [13, 6, 9, 28]  # kCupTracks[1] (lobby_core.c) -- all Car-legal

BEGIN_DESCLESS_RE = re.compile(
    r"^\[online-session\] begin: lobby-start \(no descriptor\)", re.MULTILINE)
BEGIN_DESCFIRST_RE = re.compile(
    r"^\[online-session\] begin: separated boot path entered", re.MULTILINE)
CHARSELECT_ENTER_RE = re.compile(r"^\[online-charselect\] enter:", re.MULTILINE)
TRACKSELECT_ENTER_RE = re.compile(r"^\[online-trackselect\] enter:", re.MULTILINE)
COMPOSED_RE = re.compile(
    r"^\[online-lobby-start\] composed: handed off to resident coordinator",
    re.MULTILINE)
ENTER_RE = re.compile(
    r"^\[online-results\] enter: native results up race=(\d+) final=(\d+) "
    r"haveResults=(\d+) placements=(\d+),(\d+),(\d+),(\d+)", re.MULTILINE)
RESUME_RE = re.compile(
    r"^\[online-session\] resume: RESULTS phase \(race (\d+) of (\d+); live "
    r"reducer RESULTS\) gGameMode=(\d+)", re.MULTILINE)
PUBLISH_REMATCH_RE = re.compile(
    r"^\[online-results\] publish: rematch \(host advance -> next race\)$",
    re.MULTILINE)
OBSERVED_REMATCH_RE = re.compile(
    r"^\[online-resident-live\] rematch observed race_index=(\d+) "
    r"\(reverse feed\)", re.MULTILINE)
NEXT_ARMED_RE = re.compile(
    r"^\[online-resident-live\] next race armed: epoch=(\d+)", re.MULTILINE)
STANDINGS_POINTS_RE = re.compile(
    r"^\[online-results\] render stage=standings mode=\d+ race=\d+ host=\d+ "
    r"placements=[\d,]+ points=(\d+),(\d+),(\d+),(\d+)", re.MULTILINE)
WATCHDOG_RE = re.compile(
    r"^\[online-session\] descless wait TIMEOUT: exceeded (\d+)-frame budget "
    r"at (.+?) \(raceCount=(\d+)\)", re.MULTILINE)
UNWIND_RE = re.compile(
    r"^\[online-session\] lobby-start UNWIND: room returned to LOBBY while boot "
    r"pending", re.MULTILINE)
CANCEL_SUBMIT_RE = re.compile(
    r"^\[online-lobby-start\] WEDGE cancel-loading:.*submitted=1$", re.MULTILINE)
# PD-T6d engine->launcher FINISH/RETURN handshake witnesses.
POSTRACE_EXIT = "[online-postrace] session end requested"

# A stalled resident-round or tournament advance is fatal for this lane, on top of
# the shared engine/online forbidden markers.
FORBIDDEN_EXTRA = ("[online-resident-live] round advance error",
                   "[online-resident-live] round advance FAILED",
                   "[online-tournament] result=error")


fail = make_fail("lobby-tournament")


def run_engine(binary: Path, rom: Path, ticks: int, timeout: int, verbose: bool,
               extra_env: dict[str, str]) -> tuple[int, str]:
    return _run_engine(binary, rom, ticks=ticks, timeout=timeout,
                       verbose=verbose, extra_env=extra_env,
                       prefix="mdkr64-lobby-tournament-")


def _scan_define(path: Path, macro: str) -> int | None:
    pattern = re.compile(r"#define\s+" + re.escape(macro) + r"\s+(\d+)u?")
    try:
        for line in path.read_text().splitlines():
            m = pattern.search(line)
            if m:
                return int(m.group(1))
    except OSError:
        return None
    return None


def check_cup_rounds_pin() -> int | None:
    """Minor-3: the session's mirrored MDKR_ONLINE_SESSION_CUP_ROUNDS must equal the
    reducer / track-table MDKR_ONLINE_CUP_ROUNDS -- pinned by source scan so the
    mirrored constant (which drives feed-derived finality) cannot silently drift."""
    session = _scan_define(
        ROOT / "game/src/online/online_session.c",
        "MDKR_ONLINE_SESSION_CUP_ROUNDS")
    table = _scan_define(
        ROOT / "platform/online/online_track_table.h", "MDKR_ONLINE_CUP_ROUNDS")
    core = _scan_define(
        ROOT / "platform/online/lobby_core.h", "MDKR_ONLINE_CUP_ROUNDS")
    if session is None:
        return fail("could not read MDKR_ONLINE_SESSION_CUP_ROUNDS from "
                    "online_session.c")
    if table is None or core is None:
        return fail("could not read MDKR_ONLINE_CUP_ROUNDS from the track "
                    "table / lobby_core header")
    if not (session == table == core == CUP_ROUNDS):
        return fail(f"CUP_ROUNDS drift: session={session} track_table={table} "
                    f"lobby_core={core} (all must be {CUP_ROUNDS})")
    return None


def check_watchdog_wedge(binary: Path, rom: Path, verbose: bool) -> int | None:
    """(W1) START but the descriptor/match-input never arms -> the engine's
    wall-clock watchdog fires and the run EXITS cleanly (bounded), NOT a hang."""
    try:
        rc, output = run_engine(
            binary, rom, ticks=6000, timeout=300, verbose=verbose,
            extra_env={
                "MDKR_APP_TEST_ONLINE_LIVE_LOBBY_START": "1",
                "MDKR_TEST_ONLINE_LOBBY_START": "1",
                "MDKR_APP_TEST_ONLINE_LOBBY_WEDGE": "descriptor",
            })
    except subprocess.TimeoutExpired as error:
        return fail(f"[W1 watchdog] run HUNG (the watchdog did not fire): {error}")
    marker = forbidden_marker(output)
    if marker:
        return fail(f"[W1 watchdog] forbidden marker {marker!r}", output)
    if rc != 0:
        return fail(f"[W1 watchdog] process exited {rc} (expected a clean 0)", output)
    trips = WATCHDOG_RE.findall(output)
    if len(trips) < 1:
        return fail("[W1 watchdog] the descless wait watchdog never fired -- the "
                    "descriptor-never-builds wedge would hang in production", output)
    if DIRECT_BOOT_RE.findall(output):
        return fail("[W1 watchdog] a race booted -- the wedge should NEVER reach a "
                    "boot (descriptor/match-input never armed)", output)
    return None


def check_unwind_wedge(binary: Path, rom: Path, verbose: bool) -> int | None:
    """(W2) the room returns to LOBBY while a boot is pending (leader
    CANCEL_LOADING) -> the session UNWINDS + re-fronts CHARSELECT (never parks) and
    RECOVERS (race 1 still boots afterward)."""
    try:
        rc, output = run_engine(
            binary, rom, ticks=12000, timeout=400, verbose=verbose,
            extra_env={
                "MDKR_APP_TEST_ONLINE_LIVE_LOBBY_START": "1",
                "MDKR_TEST_ONLINE_LOBBY_START": "1",
                "MDKR_APP_TEST_ONLINE_LOBBY_WEDGE": "cancel",
            })
    except subprocess.TimeoutExpired as error:
        return fail(f"[W2 unwind] run HUNG (parked forever on a stale pending boot): "
                    f"{error}")
    marker = forbidden_marker(output)
    if marker:
        return fail(f"[W2 unwind] forbidden marker {marker!r}", output)
    if rc != 0:
        return fail(f"[W2 unwind] process exited {rc} (expected a clean 0)", output)
    if not CANCEL_SUBMIT_RE.search(output):
        return fail("[W2 unwind] the leader CANCEL_LOADING was never submitted -- "
                    "the wedge did not exercise the unwind path", output)
    if not UNWIND_RE.search(output):
        return fail("[W2 unwind] the session did NOT unwind the pending boot -- a "
                    "leader cancel-loading would brick the engine session", output)
    if len(CHARSELECT_ENTER_RE.findall(output)) < 2:
        return fail("[W2 unwind] CHARSELECT was not re-fronted after the unwind "
                    "(expected >= 2 charselect enters)", output)
    if len(DIRECT_BOOT_RE.findall(output)) < 1:
        return fail("[W2 unwind] the session never RECOVERED (race 1 never booted "
                    "after the unwind + re-select)", output)
    return None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", default="build-beta")
    parser.add_argument("--rom", type=Path, default="baserom.us.v80.z64")
    parser.add_argument("--ticks", type=int, default=TICKS)
    parser.add_argument("--timeout", type=int, default=900)
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    binary = Path(resolve_binary(args.build)).expanduser().resolve()
    rom = args.rom.expanduser().resolve()
    for path, label in ((binary, "binary"), (rom, "ROM")):
        if not path.is_file():
            parser.error(f"missing {label}: {path}")

    # Minor-3 pin (source scan; no engine run needed).
    result = check_cup_rounds_pin()
    if result is not None:
        return result

    # Primary: the FULL descriptor-less tournament in one engine process.
    try:
        rc, output = run_engine(
            binary, rom, args.ticks, args.timeout, args.verbose,
            extra_env={
                "MDKR_APP_TEST_ONLINE_LIVE_LOBBY_START": "1",
                "MDKR_APP_TEST_ONLINE_MODE": "tournament",
                "MDKR_APP_TEST_ONLINE_CUP": str(CUP),
                "MDKR_TEST_ONLINE_LOBBY_START": "1",
                "MDKR_TEST_ONLINE_LOBBY_TOURNAMENT": "1",
                "MDKR_TEST_ONLINE_RESULTS_HOST_PRESS": "1",
                # PD-T6f: the final-standings FINISH now detours through the native
                # champion CEREMONY before FINISHED. Skip its (bounded) real-time
                # hold so this lane's frame budget/counts are preserved -- FINISHED
                # still fires exactly once after the (near-instant) ceremony.
                "MDKR_TEST_ONLINE_CEREMONY_SKIP": "1",
            })
    except subprocess.TimeoutExpired as error:
        return fail(f"tournament run timed out (a compose/re-cycle stall would look "
                    f"like this): {error}")

    marker = forbidden_marker(output, *FORBIDDEN_ONLINE, *FORBIDDEN_EXTRA)
    if marker:
        return fail(f"observed forbidden marker {marker!r}", output)
    if rc != 0:
        return fail(f"process exited {rc}", output)
    if POSTRACE_EXIT in output:
        return fail("the tournament took the platform-exit path mid-cup (it must "
                    "re-enter the session on the live feed until feed-final)", output)

    # (b) BEGAN descriptor-less; native CHARSELECT then TRACKSELECT owned race 1.
    if len(BEGIN_DESCLESS_RE.findall(output)) != 1:
        return fail("expected EXACTLY 1 descriptor-less begin", output)
    if BEGIN_DESCFIRST_RE.search(output):
        return fail("a descriptor-first begin fired -- this must be descriptor-less",
                    output)
    if not CHARSELECT_ENTER_RE.search(output):
        return fail("native CHARSELECT never fronted for race 1", output)
    if not TRACKSELECT_ENTER_RE.search(output):
        return fail("native TRACKSELECT never fronted for race 1", output)

    # (c) it COMPOSED with the resident coordinator.
    if not COMPOSED_RE.search(output):
        return fail("the lobby-start boot never handed off to the resident "
                    "coordinator (races 2..4 would not run in-process)", output)

    # (a) EXACTLY CUP_ROUNDS direct boots, on the cup-1 schedule.
    boots = DIRECT_BOOT_RE.findall(output)
    if len(boots) != CUP_ROUNDS:
        return fail(f"expected EXACTLY {CUP_ROUNDS} direct boots in one process, "
                    f"saw {len(boots)}: {boots!r}", output)
    booted_tracks = [int(t) for t, _p in boots]
    if booted_tracks != CUP1_TRACKS:
        return fail(f"booted tracks {booted_tracks} != cup-1 schedule {CUP1_TRACKS}",
                    output)

    # (g) every RACE hand-off is GAMEMODE_ONLINE_SESSION with gCurrentMenuId=0.
    handoffs = SESSION_RACE_RE.findall(output)
    if len(handoffs) != CUP_ROUNDS:
        return fail(f"expected {CUP_ROUNDS} RACE hand-offs, got {len(handoffs)}",
                    output)
    for _t, gamemode, menu_id in handoffs:
        if int(gamemode) != GAMEMODE_ONLINE_SESSION:
            return fail(f"a hand-off had gGameMode={gamemode} (expected "
                        f"{GAMEMODE_ONLINE_SESSION})", output)
        if int(menu_id) != 0:
            return fail(f"gCurrentMenuId={menu_id} at a hand-off -- the offline "
                        f"menu was entered on the online path (expected 0)", output)
    if "load_menu_with_level_background" in output:
        return fail("the offline menu was loaded on the descriptor-less path", output)

    # (d) native RESULTS fronts each round on the REAL snapshot; feed-derived isFinal
    #     is 0 for races 1-3 and 1 at race 4 ONLY.
    enters = ENTER_RE.findall(output)
    if len(enters) != CUP_ROUNDS:
        return fail(f"expected {CUP_ROUNDS} RESULTS enters, got {len(enters)}",
                    output)
    for idx, (race_i, final, have, p0, p1, p2, p3) in enumerate(enters):
        if int(race_i) != idx:
            return fail(f"RESULTS enter {idx} had raceIndex={race_i}", output)
        if int(have) != 1:
            return fail(f"RESULTS enter {idx} haveResults=0 -- placements were not "
                        f"read from the reducer snapshot", output)
        want_final = 1 if idx == CUP_ROUNDS - 1 else 0
        if int(final) != want_final:
            return fail(f"RESULTS enter {idx} feed-isFinal={final} (expected "
                        f"{want_final}; must be 1 ONLY at race {CUP_ROUNDS})", output)
        present = sorted(int(p) for p in (p0, p1, p2, p3) if int(p) != PLACE_NONE)
        if present != [0, 1]:
            return fail(f"RESULTS enter {idx} placements {[p0,p1,p2,p3]} are not a "
                        f"valid 2-racer finish order", output)

    # RESULTS resumes on the LIVE reducer feed, one per race.
    resumes = RESUME_RE.findall(output)
    if len(resumes) != CUP_ROUNDS:
        return fail(f"expected {CUP_ROUNDS} live RESULTS resumes, got "
                    f"{len(resumes)}", output)
    for i, (race_no, _total, gamemode) in enumerate(resumes):
        if int(race_no) != i + 1:
            return fail(f"resume {i} was race {race_no} (expected {i + 1})", output)
        if int(gamemode) != GAMEMODE_ONLINE_SESSION:
            return fail(f"resume {i} had gGameMode={gamemode}", output)

    # (f) race_index 0->3 via the reverse-feed REMATCH: 3 publishes, 3 observed
    #     advances (1,2,3), 3 per-round re-cycles on FRESH (strictly rising) epochs.
    publishes = PUBLISH_REMATCH_RE.findall(output)
    if len(publishes) != CUP_ROUNDS - 1:
        return fail(f"expected {CUP_ROUNDS - 1} REMATCH publishes (one per non-final "
                    f"race; the final holds), got {len(publishes)}", output)
    observed = [int(n) for n in OBSERVED_REMATCH_RE.findall(output)]
    if observed != list(range(1, CUP_ROUNDS)):
        return fail(f"reducer race_index did not advance 1..{CUP_ROUNDS - 1} off the "
                    f"reverse-feed REMATCH (got {observed})", output)
    armed = [int(e) for e in NEXT_ARMED_RE.findall(output)]
    if armed != list(range(2, CUP_ROUNDS + 1)):
        return fail(f"expected {CUP_ROUNDS - 1} per-round re-cycles on fresh epochs "
                    f"[2..{CUP_ROUNDS}], got {armed}", output)

    # (e) points accrue by trophy weight: the STANDINGS totals strictly increase per
    #     race for the two racers (9,7,5,3,1 weights -> each race adds points).
    points_rows = STANDINGS_POINTS_RE.findall(output)
    if not points_rows:
        return fail("no STANDINGS points rows found", output)
    # Collapse to the distinct per-race totals in order of appearance.
    seen: list[tuple[int, int]] = []
    for p0, p1, _p2, _p3 in points_rows:
        pair = (int(p0), int(p1))
        if not seen or seen[-1] != pair:
            seen.append(pair)
    race_totals = [pair for pair in seen]
    # Sum of both seats must strictly increase each race (9+7 == 16 per race).
    sums = [a + b for a, b in race_totals]
    if len(sums) < CUP_ROUNDS:
        return fail(f"expected >= {CUP_ROUNDS} distinct cumulative point totals, "
                    f"got {race_totals}", output)
    for prev, cur in zip(sums, sums[1:]):
        if cur <= prev:
            return fail(f"cumulative points did not accrue race-over-race: {sums}",
                        output)
    if race_totals[-1] not in ((34, 30), (30, 34)):
        return fail(f"final cup points {race_totals[-1]} != the trophy-weight total "
                    f"34,30 (host wins 3, loses the last)", output)

    # (h) no watchdog trip in the happy path.
    if WATCHDOG_RE.search(output):
        return fail("the descless watchdog TRIPPED in the happy path (a re-cycle "
                    "wedged)", output)

    # PD-T6d re-audit: the final standings no longer HOLD to the tick budget. The
    # scripted host "A: FINISH" (MDKR_TEST_ONLINE_RESULTS_HOST_PRESS) now fires the
    # FINISHED handshake at race 4's final standings -- the engine notes it + the
    # launcher reads reason=FINISHED and returns cleanly to the room -- so the run
    # terminates ON the FINISH (rc 0), AFTER all 4 boots / RESULTS / REMATCH
    # witnesses above (which is why every count assertion still holds).
    if not FINISHED_ENGINE_RE.search(output):
        return fail("the engine never noted FINISHED at the final standings -- the "
                    "PD-T6d FINISH handshake did not fire (final standings would "
                    "hold forever instead of returning to the room)", output)
    ends = SESSION_END_RE.findall(output)
    if not any(reason == "FINISHED" and code == "0" for reason, code in ends):
        return fail("the launcher never read the FINISHED session end (reason="
                    f"FINISHED result=0) -- no clean return-to-room; saw {ends}",
                    output)

    # Wedge sub-tests (the deferred safety findings fire cleanly, never hang).
    result = check_watchdog_wedge(binary, rom, args.verbose)
    if result is not None:
        return result
    result = check_unwind_wedge(binary, rom, args.verbose)
    if result is not None:
        return result

    print(
        "PASS online lobby-tournament: a FULL descriptor-less TOURNAMENT ran in ONE "
        f"engine process -- lobby-start begin (no descriptor) -> native CHARSELECT+"
        f"TRACKSELECT owned race 1 -> composed with the resident coordinator -> "
        f"EXACTLY {CUP_ROUNDS} boots on the cup-1 schedule {CUP1_TRACKS}, native "
        f"RESULTS each round (haveResults=1), feed-isFinal 0 for races 1-3 and 1 at "
        f"race 4 ONLY, points accrued to {race_totals[-1]}, race_index 0->3 via "
        f"reverse-feed REMATCH ({observed}) on fresh epochs {armed}, gGameMode=2 "
        f"gCurrentMenuId=0 throughout, no admission reject / watchdog trip. WEDGES: "
        f"descriptor-never-builds -> watchdog fired + clean exit (no hang); "
        f"return-to-LOBBY-while-pending -> UNWIND re-fronted CHARSELECT + recovered.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
