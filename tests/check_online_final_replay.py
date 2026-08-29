#!/usr/bin/env python3
"""RULED two-real-peer final-replay proof: the host CONTINUES, the peers re-converge.

At the tournament FINAL standings the host may pick a REPLAY option instead of
FINISH -- NEW TOURNAMENT (this lane's pick), CHANGE CUP, CHANGE MODE, CHANGE
CHARACTER or RACE AGAIN. Every one of them commits the SAME leader-only REMATCH
wrap the FINISH does (RESULTS -> LOBBY + reset_tournament_series: race_index back
to 0, points cleared -- a fresh series in the same room). The two REAL peers then
take DIFFERENT roads by design:

  - the HOST stays IN-SESSION: the chooser routes it to a native re-selection
    screen (NEW TOURNAMENT -> TRACKSELECT) where it re-locks the cup and STARTs;
  - the JOINER -- whose tournament is COMPLETE either way -- exits its mirror to
    its OWN champion ceremony -> FINISHED, and its FINISHED re-arm automatically
    RE-TAKES a fresh native session into the freshly wrapped room, where it
    re-picks / re-readies.

The RULING: this split is correct because both roads re-converge through REDUCER
AUTHORITY -- the wrapped room's LOBBY phase is the single shared selection state;
the host's START (BEGIN_LOADING) needs every seat ready again, and the next race
boots for both on a FRESH match epoch. This lane PROVES the re-convergence on the
loopback 2-endpoint rig (the same rig the keystone tournament lane drives): the
visible ENGINE is the continuing host; the peer adapter is the REAL second
endpoint in the reducer room, driven as the stand-in remote process
(MDKR_APP_TEST_ONLINE_SINGLE_ENDPOINT remote-sim) exactly the way the re-taken
joiner's fresh session re-readies itself in production.

Flow (chooser sequence "3,5" = NEW TOURNAMENT at final #1, FINISH at final #2):

  lobby-start begin -> tournament #1 races 1-4 (cup-1 schedule) -> FINAL standings
    -> host commits NEW TOURNAMENT (intent{rematch=1 mode=1}) -> the REMATCH wrap
    lands (race_index 3 -> 0, fresh series) -> the launcher's OBSERVE-ONLY
    re-cycle (the coordinator must NOT auto-drive: the host's own TRACKSELECT owns
    the re-selection) -> host re-locks cup + STARTs; the second endpoint re-readies
    -> race 5 boots on a FRESH epoch = tournament #2 race 1 (race_index 0) ->
    races 5-8 -> FINAL standings #2 -> FINISH -> wrap -> CEREMONY -> the single
    FINISHED handshake.

Primary assertions:
  (a) EXACTLY 8 direct boots in ONE process, cup-1 schedule twice
      ([13,6,9,28] x 2) -- the continuing host's tournament #2 genuinely raced
  (b) the chooser committed NEW TOURNAMENT (choice=5, intent{rematch=1 mode=1})
      and the session routed results -> trackselect (chooser: new tournament)
  (c) the coordinator observed the FINAL WRAP (race_index 3 -> 0) and took the
      OBSERVE-ONLY re-cycle -- never the auto-driving mid-cup advance, which
      would race the host's own re-selection to START on the old config
  (d) the wrapped room re-converged through the reducer: the observe-only
      re-cycle re-armed the match-input on the FRESH epoch 5 (both seats ready
      again -> BEGIN_LOADING landed), and race 5's results report carries
      race_index=0 accepted=1 -- the fresh series' round 1
  (e) RESULTS enters read the FEED's per-cup race_index -- 0..3 twice (the fresh
      series resets it) -- with feed-isFinal set at races 4 and 8 ONLY (the wrap
      genuinely reset the series' round AND finality)
  (f) every race's state-hash fold witness converged across the two endpoints
      (8 visible fold lines, 8 peer folds, all converged=1) -- the cross-endpoint
      state proof, not just the reducer-agreed finish order
  (g) tournament #2's final FINISH still ends cleanly: wrap converged -> CEREMONY
      -> FINISHED exactly once, launcher reads reason=FINISHED result=0, rc 0
  (h) isolation: every RACE hand-off is gGameMode=2 gCurrentMenuId=0; no
      watchdog trip, no admission reject

RED at the pre-fix build (the real divergence this lane exposed): the resident
coordinator's tournament re-cycle trigger was `race_index > rs->raceIndex`, which
the final wrap (3 -> 0) by definition never satisfies -- the coordinator parked in
Results forever, the continuing host's race 5 never booted, and the engine's
per-round re-wait wall-clock watchdog tripped to an ERROR exit.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

from harness_utils import resolve_binary
from online_lane_util import (
    CUP_ROUNDS, DIRECT_BOOT_RE, FINISHED_ENGINE_RE, FORBIDDEN_ONLINE,
    GAMEMODE_ONLINE_SESSION, SESSION_END_RE, SESSION_RACE_RE, forbidden_marker,
    make_fail,
)
from online_lane_util import run_engine as _run_engine

ROOT = Path(__file__).resolve().parent.parent
TICKS = 60000
CUP = 1
CUP1_TRACKS = [13, 6, 9, 28]  # kCupTracks[1] (lobby_core.c) -- all Car-legal

CHOOSER_NEWT_COMMIT_RE = re.compile(
    r"^\[online-results\] chooser: committed option=NEW TOURNAMENT choice=5 "
    r"intent\{rematch=1 mode=1\}$", re.MULTILINE)
ROUTE_NEWT_RE = re.compile(
    r"^\[online-session\] results -> trackselect \(chooser: new tournament\)$",
    re.MULTILINE)
FINAL_WRAP_OBSERVED_RE = re.compile(
    r"^\[online-resident-live\] tournament final wrap: room left RESULTS -> "
    r"LOBBY \(fresh series, race_index (\d+) -> (\d+)\) -> observe-only "
    r"re-cycle", re.MULTILINE)
MIDCUP_ADVANCE_RE = re.compile(
    r"^\[online-resident-live\] rematch observed race_index=(\d+) "
    r"\(reverse feed\)", re.MULTILINE)
NEXT_ARMED_RE = re.compile(
    r"^\[online-resident-live\] next race armed: epoch=(\d+)", re.MULTILINE)
OBSERVE_READY_RE = re.compile(
    r"^\[online-resident-live\] single race race-ready epoch=(\d+) ",
    re.MULTILINE)
RESULTS_REPORTED_RE = re.compile(
    r"^\[online-resident-live\] race results reported placements=[\d,]+ "
    r"accepted=(\d+) race_index=(\d+)$", re.MULTILINE)
ENTER_RE = re.compile(
    r"^\[online-results\] enter: native results up race=(\d+) final=(\d+) "
    r"haveResults=(\d+)", re.MULTILINE)
FOLD_RE = re.compile(
    r"^\[online-resident-live\] race fold epoch=(\d+) race_index=(\d+) "
    r"span=(\d+)\.\.(\d+) hash=([0-9a-f]{16})$", re.MULTILINE)
FOLD_PEER_RE = re.compile(
    r"^\[online-resident-live\] race fold peer epoch=(\d+) span=(\d+)\.\.(\d+) "
    r"hash=([0-9a-f]{16}) converged=(\d+)$", re.MULTILINE)
FINISH_WRAP_LEAVE_RE = re.compile(
    r"^\[online-results\] chooser: FINISH wrap converged \(room left RESULTS\) "
    r"-> LEAVE \(ceremony\)$", re.MULTILINE)
PHASE_CEREMONY_RE = re.compile(
    r"^\[online-session\] phase=CEREMONY: final standings", re.MULTILINE)
WATCHDOG_RE = re.compile(
    r"^\[online-session\] descless wait TIMEOUT:", re.MULTILINE)
POSTRACE_EXIT = "[online-postrace] session end requested"

FORBIDDEN_EXTRA = ("[online-resident-live] round advance error",
                   "[online-resident-live] round advance FAILED",
                   "[online-resident-live] single-race re-cycle TIMEOUT")


fail = make_fail("final-replay")


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

    try:
        rc, output = _run_engine(
            binary, rom, ticks=args.ticks, timeout=args.timeout,
            verbose=args.verbose,
            extra_env={
                "MDKR_APP_TEST_ONLINE_LIVE_LOBBY_START": "1",
                "MDKR_APP_TEST_ONLINE_MODE": "tournament",
                "MDKR_APP_TEST_ONLINE_CUP": str(CUP),
                "MDKR_TEST_ONLINE_LOBBY_START": "1",
                "MDKR_TEST_ONLINE_LOBBY_TOURNAMENT": "1",
                "MDKR_TEST_ONLINE_RESULTS_HOST_PRESS": "1",
                # SINGLE-ENDPOINT + remote-sim: the peer adapter is driven as the
                # stand-in REMOTE PROCESS (its own re-ready each round and across
                # the wrap) -- the loopback model of the re-taken joiner's fresh
                # session re-readying itself in the wrapped room.
                "MDKR_APP_TEST_ONLINE_SINGLE_ENDPOINT": "1",
                # The final-replay pick, then the second tournament's clean end:
                # final #1 commits NEW TOURNAMENT (index 3), final #2 FINISH
                # (index 5).
                "MDKR_TEST_ONLINE_RESULTS_CHOOSER": "3,5",
                "MDKR_TEST_ONLINE_CEREMONY_SKIP": "1",
            }, prefix="mdkr64-final-replay-")
    except subprocess.TimeoutExpired as error:
        return fail(f"run timed out (the continuing host's tournament #2 never "
                    f"completed): {error}")

    marker = forbidden_marker(output, *FORBIDDEN_ONLINE, *FORBIDDEN_EXTRA)
    if marker:
        return fail(f"observed forbidden marker {marker!r}", output)
    if rc != 0:
        return fail(f"process exited {rc} (expected clean 0)", output)
    if POSTRACE_EXIT in output:
        return fail("the run took the platform-exit path mid-cup", output)
    if WATCHDOG_RE.search(output):
        return fail("a descriptor-less wait watchdog TRIPPED -- the continuing "
                    "host's re-cycle wedged (the pre-fix divergence)", output)

    # (a) EXACTLY 8 boots, the cup-1 schedule twice.
    boots = [int(t) for t, _p in DIRECT_BOOT_RE.findall(output)]
    if boots != CUP1_TRACKS * 2:
        return fail(f"expected the cup-1 schedule twice ({CUP1_TRACKS * 2}), "
                    f"saw {boots}", output)

    # (h) isolation on all 8 hand-offs.
    handoffs = SESSION_RACE_RE.findall(output)
    if len(handoffs) != 2 * CUP_ROUNDS:
        return fail(f"expected {2 * CUP_ROUNDS} RACE hand-offs, got "
                    f"{len(handoffs)}", output)
    for _t, gamemode, menu_id in handoffs:
        if int(gamemode) != GAMEMODE_ONLINE_SESSION or int(menu_id) != 0:
            return fail(f"isolation broke at a hand-off: gGameMode={gamemode} "
                        f"gCurrentMenuId={menu_id}", output)

    # (b) the chooser committed NEW TOURNAMENT and routed to TRACKSELECT.
    if not CHOOSER_NEWT_COMMIT_RE.search(output):
        return fail("the host never committed NEW TOURNAMENT at the final "
                    "(choice=5, intent{rematch=1 mode=1})", output)
    if not ROUTE_NEWT_RE.search(output):
        return fail("the session never routed results -> trackselect on the "
                    "NEW TOURNAMENT choice (the host must continue in-session)",
                    output)

    # (c) the coordinator observed the FINAL WRAP and took the observe-only
    # re-cycle. Two wraps happen (NEW TOURNAMENT at final #1, FINISH at final
    # #2); the FIRST is the one that must re-cycle to race 5.
    wraps = FINAL_WRAP_OBSERVED_RE.findall(output)
    if not wraps or wraps[0] != ("3", "0"):
        return fail(f"the coordinator never observed the final wrap (race_index "
                    f"3 -> 0) into the observe-only re-cycle; saw {wraps}", output)
    # The mid-cup auto-advance stays mid-cup only: race_index observations are
    # 1,2,3 within EACH tournament, never a wrap-time advance.
    advances = [int(n) for n in MIDCUP_ADVANCE_RE.findall(output)]
    if advances != [1, 2, 3, 1, 2, 3]:
        return fail(f"mid-cup advances should be [1,2,3] per tournament, got "
                    f"{advances}", output)

    # (d) the wrapped room re-converged: the observe-only re-cycle re-armed the
    # match-input on the FRESH epoch 5, and race 5 reported race_index=0
    # accepted=1 (the fresh series' round 1).
    observe_epochs = [int(e) for e in OBSERVE_READY_RE.findall(output)]
    if 5 not in observe_epochs:
        return fail(f"the observe-only re-cycle never re-armed the match-input "
                    f"on the fresh epoch 5 (saw {observe_epochs}) -- the wrapped "
                    f"room never re-reached a race-ready transport", output)
    armed = [int(e) for e in NEXT_ARMED_RE.findall(output)]
    if armed != [2, 3, 4, 6, 7, 8]:
        return fail(f"per-round re-cycles should be epochs [2,3,4,6,7,8] "
                    f"(epoch 5 is the wrap's observe-only arm), got {armed}",
                    output)
    reports = [(int(a), int(ri)) for a, ri in RESULTS_REPORTED_RE.findall(output)]
    if len(reports) != 8 or any(a != 1 for a, _ri in reports):
        return fail(f"expected 8 accepted results reports, got {reports}", output)
    if [ri for _a, ri in reports] != [0, 1, 2, 3, 0, 1, 2, 3]:
        return fail(f"reported race_index sequence must wrap 0..3 twice (the "
                    f"fresh series), got {[ri for _a, ri in reports]}", output)

    # (e) RESULTS enters: the per-cup round WRAPS 0..3 twice (the second
    # tournament is a FRESH series), with feed-isFinal at races 4 and 8 ONLY.
    # The results-enter race is now the FEED's per-cup race_index (latched from
    # the snapshot at enter), matching the reported/fold sequences above -- NOT
    # the session's cumulative boot count (which would read 4..7 for the second
    # cup, disagreeing with the reducer's own race_index for the same race).
    enters = ENTER_RE.findall(output)
    if len(enters) != 8:
        return fail(f"expected 8 RESULTS enters, got {len(enters)}", output)
    for idx, (race_i, final, have) in enumerate(enters):
        want_race = idx % 4  # fresh series resets the feed race_index to 0
        want_final = 1 if idx in (3, 7) else 0
        if int(race_i) != want_race or int(final) != want_final or int(have) != 1:
            return fail(f"RESULTS enter {idx} was race={race_i} final={final} "
                        f"haveResults={have} (expected race={want_race} "
                        f"final={want_final} haveResults=1)", output)

    # (f) the state-hash fold witness converged on every race, across both
    # endpoints (the loopback corroboration of the capstone's (d)/(e) bar).
    folds = FOLD_RE.findall(output)
    peer_folds = FOLD_PEER_RE.findall(output)
    if len(folds) != 8 or len(peer_folds) != 8:
        return fail(f"expected 8 visible + 8 peer race-fold witnesses, got "
                    f"{len(folds)}/{len(peer_folds)}", output)
    if [int(ri) for _e, ri, _s, _e2, _h in folds] != [0, 1, 2, 3, 0, 1, 2, 3]:
        return fail(f"fold race_index sequence must wrap 0..3 twice, got "
                    f"{[ri for _e, ri, _s, _e2, _h in folds]}", output)
    for _epoch, _s, _e, _h, converged in peer_folds:
        if converged != "1":
            return fail(f"a race fold DIVERGED across the two endpoints: "
                        f"{peer_folds}", output)

    # (g) tournament #2's final FINISH still ends cleanly.
    if not FINISH_WRAP_LEAVE_RE.search(output):
        return fail("the second tournament's FINISH never converged its wrap "
                    "before the ceremony", output)
    if not PHASE_CEREMONY_RE.search(output):
        return fail("the session never detoured into the CEREMONY at the second "
                    "final", output)
    if len(FINISHED_ENGINE_RE.findall(output)) != 1:
        return fail("FINISHED must fire exactly once (at the second final)",
                    output)
    ends = SESSION_END_RE.findall(output)
    if not any(r == "FINISHED" and c == "0" for r, c in ends):
        return fail(f"the launcher never read reason=FINISHED result=0; saw "
                    f"{ends}", output)

    print(
        "PASS online final-replay: at the tournament FINAL the host committed NEW "
        "TOURNAMENT (rematch + SET_MODE tournament) -- the REMATCH wrap reset the "
        "series (race_index 3 -> 0) and the coordinator took the OBSERVE-ONLY "
        "re-cycle (never the auto-driving mid-cup advance), the host's own native "
        "TRACKSELECT re-locked the cup and STARTed while the second endpoint "
        "re-readied through the reducer, and race 5 booted on the fresh epoch 5 "
        "as the fresh series' round 1 -- the two roads (continuing host / "
        "re-taken second peer) re-converged through reducer authority; the full "
        "second cup then raced ([13,6,9,28] x 2, results race_index 0..3 twice, "
        "8/8 cross-endpoint state-hash folds converged) and its FINISH ended "
        "cleanly (wrap -> CEREMONY -> single FINISHED, reason=FINISHED result=0, "
        "gGameMode=2 gCurrentMenuId=0 throughout).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
