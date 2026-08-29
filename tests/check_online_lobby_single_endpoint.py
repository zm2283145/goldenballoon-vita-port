#!/usr/bin/env python3
"""PD-T6h2c: SINGLE-ENDPOINT (real 2-process) descriptor-less takeover + safety.

The lobby-tournament lane (check_online_lobby_tournament.py) proves the FULL
descriptor-less tournament with the TWO-endpoint loopback advance (peer != nullptr,
both endpoints re-Readied locally). This lane proves the pieces that make the native
online screens front for a REAL 2-process human game, where the LOCAL process may
drive ONLY its own endpoint and the REMOTE readies itself over the transport:

  (a) SINGLE-ENDPOINT per-round advance -- the per-round re-cycle drives ONLY the
      local (visible) endpoint (the advance step is handed a NULL peer, so it CANNOT
      poke a peer adapter); a SEPARATE remote-sim drives the stand-in remote (peer B)
      so the reducer converges over the real loopback transport. Assert >= 2 boots in
      ONE process on the single endpoint, on FRESH (strictly rising) epochs, with the
      single-endpoint witness on every round advance. The final standings then reach
      FINISH via the native "more races" chooser (host commits FINISH -> LEAVE) into
      the single FINISHED handshake.

  (b) WALL-CLOCK watchdog over ALL THREE descriptor-less waits, with an ERROR signal:
      - W1 race-1 re-wait: START but the descriptor/match-input never arms -> the
        wall-clock deadline trips at "race-1 re-wait" and the run exits NONZERO
        (ERROR, distinguishable from a normal finish), bounded, NOT a hang.
      - WA RESULTS rematch-hold (Minor-A): after race 1 the REMATCH never lands ->
        the wall-clock deadline trips at "results rematch-hold" and exits NONZERO.
      - WC mid-tournament CANCEL (Minor-C): a leader cancel mid round-2+ advance ->
        the engine UNWINDS + re-fronts CHARSELECT (never parks), and the session is
        bounded by the wall-clock watchdog (no hang).

Two loopback adapters stand in for the two processes; single-endpoint mode
(MDKR_APP_TEST_ONLINE_SINGLE_ENDPOINT) makes the LOCAL advance drive only the visible
endpoint while the remote-sim drives peer B. The engine session runs in the WALL-CLOCK
watchdog + error-signal path (singleEndpoint latch from the party_link note).

The room-ready GATE probe (production detection fires ONCE on first-SELECTING + 2
members + LOBBY and routes to lobby-start) used to run here too, but it is the
byte-identical probe check_online_lobby_start.py already carries (there holding the
PRODUCTION OwningLiveAdapter wrapper + asserting the [online-room-ready] latch/publish
diagnostics -- the load-bearing coverage), so it lives in that one lane now to avoid a
duplicate probe run.
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
    CHOOSER_FINISH_RE, CUP_ROUNDS, DIRECT_BOOT_RE, FINISHED_ENGINE_RE,
    SESSION_END_RE, forbidden_marker, make_fail,
)
from online_lane_util import run_engine as _run_engine

ROOT = Path(__file__).resolve().parent.parent
TICKS = 30000
CUP = 1
CUP1_TRACKS = [13, 6, 9, 28]  # kCupTracks[1] (lobby_core.c) -- all Car-legal

BEGIN_SINGLE_RE = re.compile(
    r"^\[online-session\] begin: lobby-start \(no descriptor\).*singleEndpoint=1",
    re.MULTILINE)
COMPOSED_SINGLE_RE = re.compile(
    r"^\[online-lobby-start\] composed:.*singleEndpoint=1 remoteSim=(\d+)",
    re.MULTILINE)
SINGLE_ADVANCE_RE = re.compile(
    r"^\[online-resident-live\] round (\d+) race-ready track=(\d+) epoch=(\d+) "
    r"frames=(\d+) \(single-endpoint; roster re-installed\)", re.MULTILINE)
NEXT_ARMED_RE = re.compile(
    r"^\[online-resident-live\] next race armed: epoch=(\d+)", re.MULTILINE)
WATCHDOG_WALLCLOCK_RE = re.compile(
    r"^\[online-session\] descless wait TIMEOUT: exceeded (\d+)ms wall-clock "
    r"deadline at (.+?) \(raceCount=(\d+)\) -- ERROR:", re.MULTILINE)
MID_UNWIND_RE = re.compile(
    r"^\[online-session\] mid-tournament UNWIND: room regressed to LOBBY",
    re.MULTILINE)
CHARSELECT_ENTER_RE = re.compile(r"^\[online-charselect\] enter:", re.MULTILINE)


fail = make_fail("lobby-single-endpoint")


def run_engine(binary: Path, rom: Path, ticks: int, timeout: int, verbose: bool,
               extra_env: dict[str, str]) -> tuple[int, str]:
    return _run_engine(binary, rom, ticks=ticks, timeout=timeout,
                       verbose=verbose, extra_env=extra_env,
                       prefix="mdkr64-single-endpoint-")


def check_single_endpoint_advance(binary: Path, rom: Path, ticks: int,
                                  timeout: int, verbose: bool) -> int | None:
    """(a) The single-endpoint per-round advance: >= 2 boots in one process on the
    single (local) endpoint, fresh epochs, single-endpoint witness each round."""
    rc, output = run_engine(
        binary, rom, ticks, timeout, verbose,
        extra_env={
            "MDKR_APP_TEST_ONLINE_LIVE_LOBBY_START": "1",
            "MDKR_APP_TEST_ONLINE_MODE": "tournament",
            "MDKR_APP_TEST_ONLINE_CUP": str(CUP),
            "MDKR_TEST_ONLINE_LOBBY_START": "1",
            "MDKR_TEST_ONLINE_LOBBY_TOURNAMENT": "1",
            "MDKR_TEST_ONLINE_RESULTS_HOST_PRESS": "1",
            "MDKR_APP_TEST_ONLINE_SINGLE_ENDPOINT": "1",
            # The MORE-RACES chooser is the sole RESULTS terminal now: at the FINAL
            # standings the host reaches FINISH the way a player would -- navigate to
            # the FINISH option (index 5) and press A -> LEAVE -> champion CEREMONY
            # -> the single FINISHED handshake this lane asserts. Inert on the non-
            # final rounds (the chooser fronts only at the FINAL standings; the
            # single-endpoint per-round re-cycle below is unchanged).
            "MDKR_TEST_ONLINE_RESULTS_CHOOSER": "5",
            # PD-T6f: skip the champion CEREMONY's bounded hold so this lane's
            # single-endpoint frame budget + FINISHED assertion are preserved.
            "MDKR_TEST_ONLINE_CEREMONY_SKIP": "1",
        })
    marker = forbidden_marker(output, "online race admission rejected")
    if marker:
        return fail(f"[single-advance] forbidden marker {marker!r}", output)
    if rc != 0:
        return fail(f"[single-advance] process exited {rc} (expected clean 0)",
                    output)
    if not BEGIN_SINGLE_RE.search(output):
        return fail("[single-advance] session did not begin descriptor-less in "
                    "singleEndpoint=1 mode", output)
    composed = COMPOSED_SINGLE_RE.search(output)
    if not composed or composed.group(1) != "1":
        return fail("[single-advance] the lobby-start boot did not compose in "
                    "single-endpoint + remote-sim mode", output)
    boots = [int(t) for t, _p in DIRECT_BOOT_RE.findall(output)]
    if len(boots) < 2:
        return fail(f"[single-advance] expected >= 2 boots on ONE endpoint, saw "
                    f"{len(boots)}: {boots!r}", output)
    if boots != CUP1_TRACKS:
        return fail(f"[single-advance] booted tracks {boots} != cup-1 schedule "
                    f"{CUP1_TRACKS}", output)
    advances = SINGLE_ADVANCE_RE.findall(output)
    if len(advances) != CUP_ROUNDS - 1:
        return fail(f"[single-advance] expected {CUP_ROUNDS - 1} SINGLE-ENDPOINT "
                    f"round advances, got {len(advances)}", output)
    # Fresh, strictly rising epochs [2..CUP_ROUNDS].
    armed = [int(e) for e in NEXT_ARMED_RE.findall(output)]
    if armed != list(range(2, CUP_ROUNDS + 1)):
        return fail(f"[single-advance] per-round re-cycles not on fresh rising "
                    f"epochs [2..{CUP_ROUNDS}], got {armed}", output)
    if WATCHDOG_WALLCLOCK_RE.search(output):
        return fail("[single-advance] the wall-clock watchdog TRIPPED in the happy "
                    "path (a single-endpoint re-cycle wedged)", output)
    if MID_UNWIND_RE.search(output):
        return fail("[single-advance] a mid-tournament UNWIND fired spuriously in "
                    "the happy path", output)
    # PD-T6d re-audit: the final standings no longer HOLD to the tick budget -- the
    # host's "A: FINISH" now fires the FINISHED handshake (engine note + launcher
    # read + clean return), so the run terminates ON the FINISH with rc 0.
    # Pin the CHOOSER route explicitly: the FINISHED above must be reached via the host
    # committing the "more races" FINISH option (index 5) -- so a wrong committed route
    # fails here, directly, instead of masquerading as a downstream FINISHED.
    if not CHOOSER_FINISH_RE.search(output):
        return fail("[single-advance] the native chooser never committed the FINISH "
                    "option (no '[online-results] chooser: committed option=FINISH -> "
                    "LEAVE') -- the FINISHED was not reached via the FINISH route",
                    output)
    if not FINISHED_ENGINE_RE.search(output):
        return fail("[single-advance] the engine never noted FINISHED at the final "
                    "standings (the FINISH handshake did not fire)", output)
    ends = SESSION_END_RE.findall(output)
    if not any(reason == "FINISHED" and code == "0" for reason, code in ends):
        return fail("[single-advance] the launcher never read the FINISHED session "
                    f"end (reason=FINISHED result=0); saw {ends}", output)
    return None


def check_wallclock_wait(binary: Path, rom: Path, verbose: bool, wedge: str,
                         where: str, ticks: int, timeout: int) -> int | None:
    """(c) A stuck single-endpoint wait is bounded by the WALL-CLOCK deadline and
    exits with an ERROR (nonzero) code -- never a hang, never a silent finish."""
    env = {
        "MDKR_APP_TEST_ONLINE_LIVE_LOBBY_START": "1",
        "MDKR_TEST_ONLINE_LOBBY_START": "1",
        "MDKR_APP_TEST_ONLINE_SINGLE_ENDPOINT": "1",
        "MDKR_APP_TEST_ONLINE_LOBBY_WEDGE": wedge,
        "MDKR_ONLINE_SESSION_DESCLESS_WAIT_DEADLINE_MS": "2000",
    }
    if wedge != "descriptor":
        # results-hold + per-round wedges need a tournament (they fire after race 1).
        env.update({
            "MDKR_APP_TEST_ONLINE_MODE": "tournament",
            "MDKR_APP_TEST_ONLINE_CUP": str(CUP),
            "MDKR_TEST_ONLINE_LOBBY_TOURNAMENT": "1",
            "MDKR_TEST_ONLINE_RESULTS_HOST_PRESS": "1",
        })
    try:
        rc, output = run_engine(binary, rom, ticks, timeout, verbose, env)
    except subprocess.TimeoutExpired as error:
        return fail(f"[wall-clock {wedge}] run HUNG (the watchdog did not fire): "
                    f"{error}")
    marker = forbidden_marker(output)
    if marker:
        return fail(f"[wall-clock {wedge}] forbidden marker {marker!r}", output)
    trips = WATCHDOG_WALLCLOCK_RE.findall(output)
    if not any(w == where for _ms, w, _rc in trips):
        return fail(f"[wall-clock {wedge}] the wall-clock watchdog never tripped at "
                    f"{where!r} (trips={trips})", output)
    if rc == 0:
        return fail(f"[wall-clock {wedge}] exited 0 -- a stuck wait must carry an "
                    f"ERROR signal (nonzero), not look like a normal finish", output)
    # PD-T6d: the watchdog trip also notes the ERROR reason, which the launcher's
    # session-end read surfaces (one uniform channel alongside the nonzero rc).
    ends = SESSION_END_RE.findall(output)
    if not any(reason == "ERROR" for reason, _code in ends):
        return fail(f"[wall-clock {wedge}] the launcher never read the ERROR session "
                    f"end (reason=ERROR); saw {ends}", output)
    return None


def check_mid_tournament_cancel(binary: Path, rom: Path,
                                verbose: bool) -> int | None:
    """(Minor-C) A leader cancel mid-tournament -> the engine UNWINDS + re-fronts
    CHARSELECT (never parks), and the session is bounded (no hang)."""
    env = {
        "MDKR_APP_TEST_ONLINE_LIVE_LOBBY_START": "1",
        "MDKR_APP_TEST_ONLINE_MODE": "tournament",
        "MDKR_APP_TEST_ONLINE_CUP": str(CUP),
        "MDKR_TEST_ONLINE_LOBBY_START": "1",
        "MDKR_TEST_ONLINE_LOBBY_TOURNAMENT": "1",
        "MDKR_TEST_ONLINE_RESULTS_HOST_PRESS": "1",
        "MDKR_APP_TEST_ONLINE_SINGLE_ENDPOINT": "1",
        "MDKR_APP_TEST_ONLINE_LOBBY_WEDGE": "cancel2",
        "MDKR_ONLINE_SESSION_DESCLESS_WAIT_DEADLINE_MS": "3000",
    }
    try:
        rc, output = run_engine(binary, rom, ticks=40000, timeout=400,
                                verbose=verbose, extra_env=env)
    except subprocess.TimeoutExpired as error:
        return fail(f"[mid-cancel] run HUNG (parked forever on the cancelled round): "
                    f"{error}")
    marker = forbidden_marker(output, "online race admission rejected")
    if marker:
        return fail(f"[mid-cancel] forbidden marker {marker!r}", output)
    if not MID_UNWIND_RE.search(output):
        return fail("[mid-cancel] the engine did NOT unwind the mid-tournament "
                    "cancel -- it would park on a stale round with no re-front",
                    output)
    # PD-T6d (Minor-4): the mid-tournament cancel now returns CLEANLY to the room --
    # note LEFT + platform_request_exit(0) -- replacing the PD-T6h2c re-front that
    # dropped the human into the doomed 900-frame advance budget (a bounded ERROR).
    # So the run exits 0 with a LEFT session-end the launcher reads (NOT a re-front,
    # NOT an error exit), after at least race 1 booted.
    if rc != 0:
        return fail(f"[mid-cancel] exited {rc} (expected a clean LEFT return 0, not "
                    f"a re-front-into-error)", output)
    ends = SESSION_END_RE.findall(output)
    if not any(reason == "LEFT" and code == "0" for reason, code in ends):
        return fail("[mid-cancel] the launcher never read the LEFT session end "
                    f"(reason=LEFT result=0); saw {ends}", output)
    # Race 1 fronted CHARSELECT + booted before the cancel.
    if len(CHARSELECT_ENTER_RE.findall(output)) < 1:
        return fail("[mid-cancel] CHARSELECT never fronted for race 1", output)
    if len(DIRECT_BOOT_RE.findall(output)) < 1:
        return fail("[mid-cancel] no race booted before the cancel", output)
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

    # (a) single-endpoint per-round advance
    result = check_single_endpoint_advance(binary, rom, args.ticks, args.timeout,
                                           args.verbose)
    if result is not None:
        return result

    # (b) wall-clock watchdog over the three descriptor-less waits + error signal
    result = check_wallclock_wait(binary, rom, args.verbose, "descriptor",
                                  "race-1 re-wait", ticks=6000, timeout=200)
    if result is not None:
        return result
    result = check_wallclock_wait(binary, rom, args.verbose, "results",
                                  "results rematch-hold", ticks=40000, timeout=400)
    if result is not None:
        return result
    result = check_wallclock_wait(binary, rom, args.verbose, "perround",
                                  "per-round re-wait", ticks=40000, timeout=400)
    if result is not None:
        return result

    # (Minor-C) mid-tournament cancel -> unwind + re-front CHARSELECT, bounded
    result = check_mid_tournament_cancel(binary, rom, args.verbose)
    if result is not None:
        return result

    print(
        "PASS online lobby-single-endpoint: the SINGLE-ENDPOINT per-round advance "
        f"drove ONLY the local endpoint (advance handed a NULL peer) for {CUP_ROUNDS} "
        f"boots on fresh epochs [2..{CUP_ROUNDS}]; the WALL-CLOCK "
        "watchdog bounded all three "
        "descriptor-less waits (race-1 re-wait, results rematch-hold, per-round) with "
        "a nonzero ERROR exit + a launcher reason=ERROR read; the final standings "
        "reached FINISH via the native chooser (committed option=FINISH -> LEAVE) into "
        "the PD-T6d FINISHED handshake (engine note + launcher reason=FINISHED "
        "+ clean return); and a mid-tournament CANCEL returned cleanly to the room "
        "(reason=LEFT, exit 0) instead of a re-front-into-error (never a hang).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
