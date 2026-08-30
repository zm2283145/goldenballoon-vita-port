#!/usr/bin/env python3
"""Exit-gate: the JOINER's FINAL-standings terminal has a bound INDEPENDENT of
the host -- it can no longer hang forever on the last screen of a tournament --
proven through the now-unconditional native "MORE RACES" chooser.

THE BUG (historically masked by a test seam): on the final race BOTH consoles reach
the terminal FINAL STANDINGS. The HOST exits; the JOINER's only terminal exit USED
to be "wait for the forward feed to leave the RESULTS phase" -- but after the host
finishes, the reducer PARKS in RESULTS (no REMATCH, no CLOSE on a final race), the
wall-clock watchdog is excluded for the final standings, and pad B was swallowed at
the terminal. So in real 2-console play the joiner wedged: a hard hang.

THE CHOOSER-FLOW FIX (Track D3 + this migration): the RESULTS terminal is now the
unconditional MORE-RACES chooser. A non-host endpoint renders the display-only
joiner MIRROR, which ends the session on TWO events, each a bound INDEPENDENT of the
host: a VANISHED host (the room down to just this seat, debounced by
RES_CHOOSER_VACATE_DEBOUNCE) and a deliberate confirmed B. The old BLIND
self-advance dwell was deliberately REMOVED (it wrongly bailed while the host was
merely deliberating, D3) -- so the mirror WAITS while the host is present and only
ends when the host is genuinely gone. Either exit routes LEAVE -> the joiner's OWN
per-endpoint champion CEREMONY -> FINISHED (no hang).

THIS LANE proves the migrated contract on the two rigs that matter:

  (vanished-host)  RESIDENT soak (single final race) driving the chooser JOINER
                   MIRROR (MDKR_TEST_ONLINE_RESULTS_CHOOSER=joiner-vacate): the sole
                   remote (host) seat vacates while the feed stays in RESULTS, so the
                   mirror ends the session via its OWN debounced vanished-host exit
                   -> CEREMONY -> the SINGLE FINISHED. The impossible-to-hang proof:
                   the joiner needs no press of its own, and it is the MIRROR's exit
                   (not the removed blind dwell, not a host press) that leaves.

  (remote-gone-final, P2 / online_session !resultsIsFinal gate) a full descriptor-
                   less TOURNAMENT reaches the FINAL standings with the remote seat
                   FORCED GONE (MDKR_TEST_ONLINE_REMOTE_VACATE_AT_RESULTS_FINAL,
                   scoped to resultsIsFinal so it is inert on the non-final rounds).
                   The chooser terminal still reaches FINISHED (+ the launcher
                   reason=FINISHED read on the loopback lobby-start dispatch), and --
                   the P2 assertion -- the online_session RESULTS remote-vacate
                   detector must NOT trip a LEFT at the final standings: its
                   !resultsIsFinal gate keeps the 0.75s detector OFF the earned
                   terminal even though the host has vanished. Without the gate the
                   detector would trip LEFT first (the remote is forced gone); this
                   lane fails (VACATE_LEFT appears / reason != FINISHED) in that case.
                   The detector is only reachable on the descriptor-less
                   (beganWithoutDescriptor) loopback rig, so the gate is proven
                   LOAD-BEARING here, not vacuously on a resident soak.

Both assert the session ends via the chooser flow (never the removed blind dwell)
and detour through the champion CEREMONY into EXACTLY ONE FINISHED.
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
    SESSION_END_RE, forbidden_marker, make_fail,
)
from online_lane_util import run_engine as _run_engine

ROOT = Path(__file__).resolve().parent.parent
CUP = 1

# The chooser JOINER MIRROR: rendered display-only (joiner=1), its own debounced
# vanished-host exit, and the stand-in reducer's host-seat departure that stages it.
CHOOSER_MIRROR_RE = re.compile(
    r"^\[online-results\] chooser render mode=\d+ host=\d+ joiner=1 ", re.MULTILINE)
JOINER_VACATE_LEAVE_RE = re.compile(
    r"^\[online-results\] chooser: joiner mirror host vacated -> LEAVE$",
    re.MULTILINE)
JOINER_SEAT_VACATED_RE = re.compile(
    r"^\[online-results\] test-reducer: joiner-mirror host seat vacated",
    re.MULTILINE)
# The host's committed chooser option (a MIRROR must NEVER publish one -- watch-only).
CHOOSER_COMMIT_RE = re.compile(
    r"^\[online-results\] chooser: committed option=", re.MULTILINE)
# The tournament-final FINISH on a REAL feed-final room (race_index == 3) now
# commits the REMATCH wrap first (reducer-observable) and LEAVEs on convergence;
# the old purely-local "committed option=FINISH -> LEAVE" is the stand-in/mid-cup
# arm only.
CHOOSER_FINISH_COMMIT_RE = re.compile(
    r"^\[online-results\] chooser: committed option=FINISH choice=7 "
    r"intent\{rematch=1 mode=255\}$", re.MULTILINE)
FINISH_WRAP_LEAVE_RE = re.compile(
    r"^\[online-results\] chooser: FINISH wrap converged \(room left RESULTS\) "
    r"-> LEAVE \(ceremony\)$", re.MULTILINE)
# The removed BLIND terminal dwell/press LEAVE (the old, now-dead legacy terminal).
LEGACY_TERMINAL_RE = re.compile(
    r"^\[online-results\] finish: joiner terminal advance \((?:press|self-advance|"
    r"feed-departed)\) -> LEAVE$", re.MULTILINE)
HOST_FINISH_LEGACY_RE = re.compile(
    r"^\[online-results\] finish: host A -> LEAVE", re.MULTILINE)
PHASE_CEREMONY_RE = re.compile(
    r"^\[online-session\] phase=CEREMONY: final standings", re.MULTILINE)
# The RESULTS remote-vacate detector's LEFT note. It must NOT appear at the FINAL
# standings -- that is the LEFT the P2 gate (!resultsIsFinal) suppresses.
VACATE_LEFT_RE = re.compile(
    r"^\[online-session\] LEFT: remote seat vacated at ", re.MULTILINE)
POSTRACE_EXIT = "[online-postrace] session end requested"


fail = make_fail("joiner-terminal")


def run_engine(binary: Path, rom: Path, ticks: int, timeout: int, verbose: bool,
               extra_env: dict[str, str]) -> tuple[int, str]:
    return _run_engine(binary, rom, ticks=ticks, timeout=timeout,
                       verbose=verbose, extra_env=extra_env,
                       prefix="mdkr64-joiner-terminal-")


def _no_legacy_terminal(tag: str, output: str) -> int | None:
    """Neither of the removed legacy-terminal LEAVE paths may fire: the whole point
    of the migration is that the unconditional chooser OWNS the terminal now."""
    if LEGACY_TERMINAL_RE.search(output):
        return fail(f"[{tag}] the removed legacy joiner-terminal advance fired -- the "
                    f"unconditional chooser must own the terminal, not the old blind "
                    f"press/dwell/feed-departed path", output)
    if HOST_FINISH_LEGACY_RE.search(output):
        return fail(f"[{tag}] the removed legacy `finish: host A -> LEAVE` path fired",
                    output)
    return None


def check_vacate(binary: Path, rom: Path, verbose: bool) -> int | None:
    """(vanished-host) RESIDENT single-final-race soak driving the chooser JOINER
    MIRROR: the sole remote (host) seat vacates while the feed stays in RESULTS, so
    the mirror ends the session via its OWN debounced vanished-host exit -> CEREMONY
    -> the single FINISHED. The joiner needs no press of its own (impossible-to-hang),
    and it is the MIRROR's exit -- not the removed blind dwell -- that leaves."""
    tag = "vanished-host"
    try:
        rc, output = run_engine(
            binary, rom, ticks=12000, timeout=600, verbose=verbose,
            extra_env={
                "MDKR_TEST_ONLINE_RESIDENT": "1",  # single race == the final race
                "MDKR_TEST_ONLINE_RESULTS_CHOOSER": "joiner-vacate",
                "MDKR_TEST_ONLINE_CEREMONY_SKIP": "1",
                # NOTE: deliberately NO host-press env -- the joiner mirror ignores
                # host input, and the vanished-host exit needs no press from anyone.
            })
    except subprocess.TimeoutExpired as error:
        return fail(f"[{tag}] run timed out (the mirror never ended on the vanished "
                    f"host -- a reintroduced joiner hang): {error}")
    marker = forbidden_marker(output, *FORBIDDEN_ONLINE)
    if marker:
        return fail(f"[{tag}] observed forbidden marker {marker!r}", output)
    if rc != 0:
        return fail(f"[{tag}] process exited {rc}", output)
    guard = _no_legacy_terminal(tag, output)
    if guard is not None:
        return guard
    if not CHOOSER_MIRROR_RE.search(output):
        return fail(f"[{tag}] the terminal never rendered the display-only joiner "
                    f"mirror (joiner=1)", output)
    if CHOOSER_COMMIT_RE.search(output):
        return fail(f"[{tag}] the joiner mirror published a chooser commit (it must "
                    f"be watch-only, never drive the option list)", output)
    if not JOINER_SEAT_VACATED_RE.search(output):
        return fail(f"[{tag}] the host seat never vacated -- the control did not "
                    f"stage a vanished host", output)
    if not JOINER_VACATE_LEAVE_RE.search(output):
        return fail(f"[{tag}] the joiner mirror did not end the session on the "
                    f"vanished host via its own debounced exit (a reintroduced hang)",
                    output)
    if VACATE_LEFT_RE.search(output):
        return fail(f"[{tag}] the online_session RESULTS remote-vacate detector "
                    f"tripped a LEFT at the final standings -- the mirror, not the "
                    f"detector, must own the vanished-host exit here", output)
    if not PHASE_CEREMONY_RE.search(output):
        return fail(f"[{tag}] the session never detoured into CEREMONY off the joiner "
                    f"mirror's LEAVE", output)
    finished = FINISHED_ENGINE_RE.findall(output)
    if len(finished) != 1:
        return fail(f"[{tag}] FINISHED must fire EXACTLY ONCE across the mirror's "
                    f"vanished-host exit, saw {len(finished)}", output)
    boots = DIRECT_BOOT_RE.findall(output)
    if len(boots) < 1:
        return fail(f"[{tag}] no race booted before the final standings", output)
    return None


def check_remote_gone_final(binary: Path, rom: Path, verbose: bool) -> int | None:
    """(remote-gone-final, P2 / online_session !resultsIsFinal gate) a descriptor-less
    TOURNAMENT reaches the FINAL standings with the remote seat FORCED GONE
    (MDKR_TEST_ONLINE_REMOTE_VACATE_AT_RESULTS_FINAL, scoped to resultsIsFinal so it
    is inert on the non-final rounds). The chooser terminal still reaches FINISHED
    (+ the launcher reason=FINISHED read), and -- THE P2 ASSERTION -- the
    online_session RESULTS remote-vacate detector must NOT trip a LEFT at the final
    standings: its !resultsIsFinal gate keeps the 0.75s detector OFF the earned
    terminal even though the host has vanished. Without the gate the detector would
    trip LEFT first; this lane fails (LEFT appears / reason != FINISHED) in that case.
    The detector is only reachable on the descriptor-less (beganWithoutDescriptor)
    loopback rig, so the gate is proven LOAD-BEARING here (a resident soak begins
    descriptor-first and never runs the detector at all). Rounds 1..N-1 proceed
    normally; the FINAL terminal is the unconditional chooser's FINISH."""
    tag = "remote-gone-final"
    try:
        rc, output = run_engine(
            binary, rom, ticks=35000, timeout=900, verbose=verbose,
            extra_env={
                "MDKR_APP_TEST_ONLINE_LIVE_LOBBY_START": "1",
                "MDKR_APP_TEST_ONLINE_MODE": "tournament",
                "MDKR_APP_TEST_ONLINE_CUP": str(CUP),
                "MDKR_TEST_ONLINE_LOBBY_START": "1",
                "MDKR_TEST_ONLINE_LOBBY_TOURNAMENT": "1",
                "MDKR_TEST_ONLINE_RESULTS_HOST_PRESS": "1",
                # The unconditional MORE-RACES chooser owns the terminal: the host
                # reaches FINISH the way a player would (navigate to FINISH, index 5).
                # With the remote forced gone at the final, the online_session vacate
                # detector is gated OFF (!resultsIsFinal) and the terminal earns its
                # FINISHED rather than being pre-empted by a spurious LEFT.
                "MDKR_TEST_ONLINE_RESULTS_CHOOSER": "5",
                "MDKR_TEST_ONLINE_REMOTE_VACATE_AT_RESULTS_FINAL": "1",
                "MDKR_TEST_ONLINE_CEREMONY_SKIP": "1",
            })
    except subprocess.TimeoutExpired as error:
        return fail(f"[{tag}] run timed out (a reintroduced hang, or the vacate "
                    f"detector pre-empted the terminal without a clean exit?): {error}")
    marker = forbidden_marker(output, *FORBIDDEN_ONLINE)
    if marker:
        return fail(f"[{tag}] observed forbidden marker {marker!r}", output)
    if rc != 0:
        return fail(f"[{tag}] process exited {rc}", output)
    guard = _no_legacy_terminal(tag, output)
    if guard is not None:
        return guard
    if POSTRACE_EXIT in output:
        return fail(f"[{tag}] took the platform-exit path mid-cup", output)
    # THE P2 ASSERTION: the RESULTS remote-vacate detector must NOT have tripped at
    # the final standings. A "vacated at" LEFT here is exactly the earned-terminal
    # pre-emption the !resultsIsFinal gate exists to prevent.
    if VACATE_LEFT_RE.search(output):
        return fail(f"[{tag}] the RESULTS remote-vacate detector tripped a LEFT at "
                    f"the final standings -- the P2 gate (!resultsIsFinal) is NOT "
                    f"holding; the earned FINISHED was pre-empted", output)
    if not CHOOSER_FINISH_COMMIT_RE.search(output):
        return fail(f"[{tag}] the chooser terminal never committed FINISH (the "
                    f"unconditional chooser must own the terminal)", output)
    if not FINISH_WRAP_LEAVE_RE.search(output):
        return fail(f"[{tag}] the feed-final FINISH did not converge its REMATCH "
                    f"wrap before leaving (the reducer-observable finish)", output)
    if not PHASE_CEREMONY_RE.search(output):
        return fail(f"[{tag}] the session never detoured into CEREMONY off the "
                    f"terminal FINISH", output)
    finished = FINISHED_ENGINE_RE.findall(output)
    if len(finished) != 1:
        return fail(f"[{tag}] FINISHED must fire EXACTLY ONCE, saw {len(finished)}",
                    output)
    ends = SESSION_END_RE.findall(output)
    if not any(reason == "FINISHED" and code == "0" for reason, code in ends):
        return fail(f"[{tag}] launcher never read reason=FINISHED result=0 (a LEFT "
                    f"would mean the vacate detector won the race); saw {ends}", output)
    for reason, _code in ends:
        if reason != "FINISHED":
            return fail(f"[{tag}] an unexpected session-end reason {reason!r} leaked "
                        f"-- the completed-cup terminal must finish FINISHED, not LEFT",
                        output)
    boots = DIRECT_BOOT_RE.findall(output)
    if len(boots) != CUP_ROUNDS:
        return fail(f"[{tag}] expected {CUP_ROUNDS} direct boots (the probe must stay "
                    f"inert on the non-final rounds), saw {len(boots)}", output)
    return None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", default="build-beta")
    parser.add_argument("--rom", type=Path, default="baserom.us.v80.z64")
    parser.add_argument("--timeout", type=int, default=900)
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    binary = Path(resolve_binary(args.build)).expanduser().resolve()
    rom = args.rom.expanduser().resolve()
    for path, label in ((binary, "binary"), (rom, "ROM")):
        if not path.is_file():
            parser.error(f"missing {label}: {path}")

    for scenario in (check_vacate, check_remote_gone_final):
        result = scenario(binary, rom, args.verbose)
        if result is not None:
            return result

    print(
        "PASS online joiner-terminal: the JOINER's FINAL-standings terminal has a "
        "bound INDEPENDENT of the host, proven through the unconditional MORE-RACES "
        "chooser (never the removed blind dwell/press): (vanished-host) a RESIDENT "
        "single-final-race soak drives the display-only joiner MIRROR, the sole "
        "remote (host) seat vacates while the feed stays in RESULTS, and the mirror "
        "ends the session via its OWN debounced vanished-host exit -> CEREMONY -> the "
        "single FINISHED (no press from anyone -- impossible to hang; the mirror "
        "never commits a chooser option); (remote-gone-final, P2) a full descriptor-"
        "less tournament reaches the FINAL standings with the remote FORCED GONE, the "
        "chooser terminal earns its FINISHED (launcher reads reason=FINISHED "
        "result=0, full 4-round cup) and the online_session RESULTS remote-vacate "
        "detector does NOT pre-empt it with a LEFT -- the !resultsIsFinal gate holds "
        "LOAD-BEARINGLY on the beganWithoutDescriptor rig where the detector actually "
        "runs (no 'vacated at' LEFT). The removed legacy terminal never fired.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
