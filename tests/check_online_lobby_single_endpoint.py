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
      single-endpoint witness on every round advance.

  (b) ROOM-READY trigger -- the production detection fires EXACTLY ONCE on first-
      SELECTING + 2 members + LOBBY for a TOURNAMENT room and routes to lobby-start
      (native takeover), and NEVER for a single-race room (which keeps the race-ready
      ImGui fallback). Proven via a test seam (the full interactive loop needs a live
      cloud adapter + a human).

  (c) WALL-CLOCK watchdog over ALL THREE descriptor-less waits, with an ERROR signal:
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
TICKS = 30000
CUP_ROUNDS = 4
CUP = 1
CUP1_TRACKS = [13, 6, 9, 28]  # kCupTracks[1] (lobby_core.c) -- all Car-legal

DIRECT_BOOT_RE = re.compile(
    r"^\[online-boot\] direct race: track=(\d+) players=(\d+)$", re.MULTILINE)
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
ROOM_READY_PROBE_RE = re.compile(
    r"^\[online-room-ready-probe\] fires=(\d+) conditionHeld=(\d+) "
    r"published=(\d+) route=(\S+)", re.MULTILINE)

FORBIDDEN = ("[FATAL]", "[CRASH]", "AddressSanitizer",
             "online race admission rejected")


def fail(message: str, output: str = "") -> int:
    print(f"FAIL online lobby-single-endpoint: {message}", file=sys.stderr)
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


def run_engine(binary: Path, rom: Path, ticks: int, timeout: int, verbose: bool,
               extra_env: dict[str, str]) -> tuple[int, str]:
    with tempfile.TemporaryDirectory(prefix="mdkr64-single-endpoint-") as temp:
        run_dir = Path(temp)
        (run_dir / "saves").mkdir()
        (run_dir / "preferences").mkdir()
        environment = clean_environment(
            LC_ALL="C",
            MDKR_APP_AUTOPLAY="1",
            MDKR_APP_AUTOPLAY_TICKS=str(ticks),
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
        environment.update(extra_env)
        if verbose:
            extras = " ".join(f"{k}={v}" for k, v in extra_env.items())
            print(f"$ {extras} {binary}", flush=True)
        process = subprocess.run(
            [str(binary)], cwd=run_dir, env=environment, text=True,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            timeout=timeout, check=False,
        )
        return process.returncode, (process.stdout or "")


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
        })
    for marker in FORBIDDEN:
        if marker in output:
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
    return None


def check_room_ready_trigger(binary: Path, rom: Path, verbose: bool) -> int | None:
    """(b) The room-ready trigger fires ONCE for a tournament room (route=lobby-
    start) and NEVER for a single-race room (route=race-ready fallback)."""
    rc, output = run_engine(
        binary, rom, ticks=2000, timeout=120, verbose=verbose,
        extra_env={
            "MDKR_APP_TEST_ONLINE_ROOM_READY_PROBE": "1",
            "MDKR_APP_TEST_ONLINE_MODE": "tournament",
            "MDKR_APP_TEST_ONLINE_CUP": str(CUP),
        })
    if rc != 0:
        return fail(f"[room-ready] tournament probe exited {rc} (expected 0)",
                    output)
    m = ROOM_READY_PROBE_RE.search(output)
    if not m:
        return fail("[room-ready] tournament probe emitted no result line", output)
    fires, held, published, route = m.groups()
    if fires != "1" or held != "1" or published != "1" or route != "lobby-start":
        return fail(f"[room-ready] tournament room did not route to lobby-start "
                    f"exactly once (fires={fires} held={held} published={published} "
                    f"route={route})", output)

    rc, output = run_engine(
        binary, rom, ticks=2000, timeout=120, verbose=verbose,
        extra_env={"MDKR_APP_TEST_ONLINE_ROOM_READY_PROBE": "1"})
    if rc != 0:
        return fail(f"[room-ready] single-race probe exited {rc} (expected 0)",
                    output)
    m = ROOM_READY_PROBE_RE.search(output)
    if not m:
        return fail("[room-ready] single-race probe emitted no result line", output)
    fires, _held, published, route = m.groups()
    if fires != "0" or published != "0" or route != "race-ready-fallback":
        return fail(f"[room-ready] single-race room did NOT defer to the race-ready "
                    f"fallback (fires={fires} published={published} route={route})",
                    output)
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
    for marker in ("[FATAL]", "[CRASH]", "AddressSanitizer"):
        if marker in output:
            return fail(f"[wall-clock {wedge}] forbidden marker {marker!r}", output)
    trips = WATCHDOG_WALLCLOCK_RE.findall(output)
    if not any(w == where for _ms, w, _rc in trips):
        return fail(f"[wall-clock {wedge}] the wall-clock watchdog never tripped at "
                    f"{where!r} (trips={trips})", output)
    if rc == 0:
        return fail(f"[wall-clock {wedge}] exited 0 -- a stuck wait must carry an "
                    f"ERROR signal (nonzero), not look like a normal finish", output)
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
    for marker in FORBIDDEN:
        if marker in output:
            return fail(f"[mid-cancel] forbidden marker {marker!r}", output)
    if not MID_UNWIND_RE.search(output):
        return fail("[mid-cancel] the engine did NOT unwind the mid-tournament "
                    "cancel -- it would park on a stale round with no re-front",
                    output)
    # >= 2 charselect enters: race 1's, plus the re-front after the cancel.
    if len(CHARSELECT_ENTER_RE.findall(output)) < 2:
        return fail("[mid-cancel] CHARSELECT was not re-fronted after the "
                    "mid-tournament unwind (expected >= 2 charselect enters)",
                    output)
    # At least race 1 booted before the cancel, and the session is bounded.
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

    # (b) room-ready trigger (tournament fires once -> lobby-start; single-race
    #     defers to the race-ready fallback)
    result = check_room_ready_trigger(binary, rom, args.verbose)
    if result is not None:
        return result

    # (c) wall-clock watchdog over the three descriptor-less waits + error signal
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
        f"boots on fresh epochs [2..{CUP_ROUNDS}]; the room-ready trigger fired ONCE "
        "for a tournament room (route=lobby-start) and deferred a single-race room to "
        "the race-ready fallback; the WALL-CLOCK watchdog bounded all three "
        "descriptor-less waits (race-1 re-wait, results rematch-hold, per-round) with "
        "a nonzero ERROR exit; and a mid-tournament CANCEL unwound + re-fronted "
        "CHARSELECT (never a hang).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
