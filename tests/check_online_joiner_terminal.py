#!/usr/bin/env python3
"""Exit-gate C1: the JOINER's FINAL-standings terminal has a bound INDEPENDENT of
the host -- it can no longer hang forever on the last screen of a tournament.

THE BUG (masked by a test seam): on the final race BOTH consoles take the native
takeover and reach the terminal FINAL STANDINGS. The HOST exits on A:FINISH ->
CEREMONY -> FINISHED. The JOINER's only terminal exit USED to be "wait for the
forward feed to leave the RESULTS phase" -- but after the host finishes, the
reducer PARKS in RESULTS (no REMATCH, no CLOSE on a final race), the wall-clock
watchdog is excluded for the final standings, and pad B was swallowed at the
terminal. So in real 2-console play the joiner wedged: no watchdog, no manual
escape, no launcher escape -> a hard hang requiring force-quit. The headless proof
of record used MDKR_TEST_ONLINE_RESULTS_JOINER_FINISH, which FORCED a fake feed
departure -- masking exactly the production condition (feed STAYS in RESULTS).

THE FIX: the joiner terminal now LEAVEs on ANY of an honored A/B press, a generous
self-advance DWELL (so it advances with NO input at all), or a genuine phase
departure -- all routing to LEAVE -> CEREMONY -> FINISHED (its own per-endpoint
celebration, already no-hang). The host's interactive hold is UNCHANGED.

THIS LANE proves the fix on the REAL no-seam path -- it NEVER sets
MDKR_TEST_ONLINE_RESULTS_JOINER_FINISH (the seam that masked the bug). It uses a
terminal-only role-flip seam (MDKR_TEST_ONLINE_RESULTS_JOINER_TERMINAL) that only
routes the terminal into the joiner branch; the forward feed genuinely STAYS in
RESULTS (feedDeparted never fires), so the joiner leaves ONLY via the real
self-advance/press paths:

  (press)        full descriptor-less TOURNAMENT loopback (same rig as the ceremony
                 lane) + a scripted terminal press -> the joiner honors A ->
                 CEREMONY -> the SINGLE FINISHED, launcher reads reason=FINISHED
                 result=0, rc 0. Proves the honored-press path end-to-end WITH the
                 launcher read, feed still in RESULTS (NOT feed-departed).

  (self-advance) RESIDENT soak (single final race) + NO input at the terminal ->
                 the joiner SELF-ADVANCES on its own countdown DWELL (the
                 impossible-to-hang proof: it needs no press from anyone) ->
                 CEREMONY -> the SINGLE FINISHED, rc 0. The final-standings
                 countdown visibly decremented to ~0 before it fired, so this is
                 the DWELL, not an instant exit.

Both assert the terminal advance was NOT `(feed-departed)` -- i.e. the masking
mechanism did NOT fire -- and that the host-press terminal path did NOT run.
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
CUP = 1
CUP_ROUNDS = 4

DIRECT_BOOT_RE = re.compile(
    r"^\[online-boot\] direct race: track=(\d+) players=(\d+)$", re.MULTILINE)
JOINER_TERMINAL_RE = re.compile(
    r"^\[online-results\] finish: joiner terminal advance \((press|self-advance|"
    r"feed-departed)\) -> LEAVE$", re.MULTILINE)
HOST_FINISH_RE = re.compile(
    r"^\[online-results\] finish: host A -> LEAVE", re.MULTILINE)
PHASE_CEREMONY_RE = re.compile(
    r"^\[online-session\] phase=CEREMONY: final standings", re.MULTILINE)
FINISHED_ENGINE_RE = re.compile(
    r"^\[online-session\] FINISHED: final standings", re.MULTILINE)
SESSION_END_RE = re.compile(
    r"^\[online-session-end\] reason=(\w+) result=(-?\d+)", re.MULTILINE)
# The RESULTS remote-vacate detector's LEFT note (M2 wording). It must NOT appear at
# the FINAL standings -- that is the LEFT the P2 gate (!resultsIsFinal) suppresses.
VACATE_LEFT_RE = re.compile(
    r"^\[online-session\] LEFT: remote seat vacated at ", re.MULTILINE)
# Final-standings render witness (RESULTS screen); secs is the countdown.
STANDINGS_FINAL_RE = re.compile(
    r"^\[online-results\] render stage=standings mode=\d+ race=\d+ host=\d+ "
    r"placements=[\d,]+ points=[\d,]+ secs=(\d+) final=1$", re.MULTILINE)
POSTRACE_EXIT = "[online-postrace] session end requested"

FORBIDDEN = ("[FATAL]", "[CRASH]", "AddressSanitizer",
             "online race admission rejected",
             "launcher input provider rejected",
             "engine startup rejected before authored tick one")


def fail(message: str, output: str = "") -> int:
    print(f"FAIL online joiner-terminal: {message}", file=sys.stderr)
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
    with tempfile.TemporaryDirectory(prefix="mdkr64-joiner-terminal-") as temp:
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


def _common_joiner_asserts(tag: str, rc: int, output: str,
                           want_kind: str) -> int | None:
    """Shared contract for both no-seam scenarios: the joiner LEFT the terminal via
    the REAL path (`want_kind`, never `feed-departed`), the host-press path did NOT
    fire, the session detoured through the CEREMONY into EXACTLY ONE FINISHED, and
    the run exited cleanly with no forbidden markers / no park."""
    for marker in FORBIDDEN:
        if marker in output:
            return fail(f"[{tag}] observed forbidden marker {marker!r}", output)
    if rc != 0:
        return fail(f"[{tag}] process exited {rc} (a joiner hang would time out; a "
                    f"nonzero is a wrong end reason)", output)
    advances = JOINER_TERMINAL_RE.findall(output)
    if not advances:
        return fail(f"[{tag}] the joiner never advanced off the FINAL standings "
                    f"terminal (it would have parked forever)", output)
    if "feed-departed" in advances:
        return fail(f"[{tag}] the joiner advanced via `feed-departed` -- that is the "
                    f"MASKED mechanism; the no-seam proof must fire the real "
                    f"self-advance/press path, saw {advances}", output)
    if want_kind not in advances:
        return fail(f"[{tag}] expected a `{want_kind}` joiner terminal advance, saw "
                    f"{advances}", output)
    if HOST_FINISH_RE.search(output):
        return fail(f"[{tag}] the host-press `finish: host A -> LEAVE` path fired -- "
                    f"the terminal role-flip must exercise the JOINER, not the host",
                    output)
    if not PHASE_CEREMONY_RE.search(output):
        return fail(f"[{tag}] the session never detoured into CEREMONY off the "
                    f"joiner LEAVE", output)
    finished = FINISHED_ENGINE_RE.findall(output)
    if len(finished) != 1:
        return fail(f"[{tag}] FINISHED must fire EXACTLY ONCE across the joiner "
                    f"self-advance path, saw {len(finished)}", output)
    return None


def check_press(binary: Path, rom: Path, verbose: bool) -> int | None:
    """(press) full TOURNAMENT loopback + honored terminal press -> CEREMONY ->
    the single FINISHED, WITH the launcher reason=FINISHED result=0 read; the feed
    genuinely stays in RESULTS (NOT feed-departed)."""
    tag = "press"
    try:
        rc, output = run_engine(
            binary, rom, ticks=30000, timeout=900, verbose=verbose,
            extra_env={
                "MDKR_APP_TEST_ONLINE_LIVE_LOBBY_START": "1",
                "MDKR_APP_TEST_ONLINE_MODE": "tournament",
                "MDKR_APP_TEST_ONLINE_CUP": str(CUP),
                "MDKR_TEST_ONLINE_LOBBY_START": "1",
                "MDKR_TEST_ONLINE_LOBBY_TOURNAMENT": "1",
                "MDKR_TEST_ONLINE_RESULTS_HOST_PRESS": "1",
                "MDKR_TEST_ONLINE_RESULTS_JOINER_TERMINAL": "1",
                "MDKR_TEST_ONLINE_CEREMONY_SKIP": "1",
            })
    except subprocess.TimeoutExpired as error:
        return fail(f"[{tag}] run timed out (the joiner parked on the final "
                    f"standings instead of self-advancing?): {error}")
    guard = _common_joiner_asserts(tag, rc, output, "press")
    if guard is not None:
        return guard
    if POSTRACE_EXIT in output:
        return fail(f"[{tag}] took the platform-exit path mid-cup", output)
    boots = DIRECT_BOOT_RE.findall(output)
    if len(boots) != CUP_ROUNDS:
        return fail(f"[{tag}] expected {CUP_ROUNDS} direct boots (the full cup must "
                    f"run before the joiner FINISH), saw {len(boots)}", output)
    ends = SESSION_END_RE.findall(output)
    if not any(reason == "FINISHED" and code == "0" for reason, code in ends):
        return fail(f"[{tag}] launcher never read reason=FINISHED result=0; saw "
                    f"{ends}", output)
    for reason, _code in ends:
        if reason != "FINISHED":
            return fail(f"[{tag}] an unexpected session-end reason {reason!r} leaked",
                        output)
    return None


def check_self_advance(binary: Path, rom: Path, verbose: bool) -> int | None:
    """(self-advance) RESIDENT single-final-race soak with NO terminal input -> the
    joiner SELF-ADVANCES on its own countdown DWELL -> CEREMONY -> the single
    FINISHED. The final-standings countdown visibly decremented to ~0 first, so it
    is the DWELL that fired, not an instant exit."""
    tag = "self-advance"
    try:
        rc, output = run_engine(
            binary, rom, ticks=12000, timeout=600, verbose=verbose,
            extra_env={
                "MDKR_TEST_ONLINE_RESIDENT": "1",  # single race == the final race
                "MDKR_TEST_ONLINE_RESULTS_JOINER_TERMINAL": "1",
                "MDKR_TEST_ONLINE_CEREMONY_SKIP": "1",
                # NOTE: deliberately NO host-press env, so the terminal receives NO
                # input and must fire the self-advance DWELL backstop.
            })
    except subprocess.TimeoutExpired as error:
        return fail(f"[{tag}] run timed out (the self-advance dwell never fired -- a "
                    f"joiner hang): {error}")
    guard = _common_joiner_asserts(tag, rc, output, "self-advance")
    if guard is not None:
        return guard
    boots = DIRECT_BOOT_RE.findall(output)
    if len(boots) < 1:
        return fail(f"[{tag}] no race booted before the final standings", output)
    # The DWELL really counted down: a final-standings render showed the countdown
    # near 0 before the advance (an instant exit would never reach ~0).
    final_secs = [int(s) for s in STANDINGS_FINAL_RE.findall(output)]
    if not final_secs:
        return fail(f"[{tag}] no FINAL standings render to time the dwell", output)
    if min(final_secs) > 1:
        return fail(f"[{tag}] the final-standings countdown never approached 0 "
                    f"(min={min(final_secs)}s) -- the self-advance did not wait out "
                    f"the dwell", output)
    return None


def check_remote_gone_final(binary: Path, rom: Path, verbose: bool) -> int | None:
    """(remote-gone-final, final-review P2 / code M5 / design C-4) a descriptor-less
    TOURNAMENT reaches the FINAL standings with the remote seat FORCED GONE
    (MDKR_TEST_ONLINE_REMOTE_VACATE_AT_RESULTS_FINAL, scoped to resultsIsFinal so it
    is inert on the non-final rounds). The joiner terminal must STILL reach FINISHED
    via its self-advance DWELL: the P2 gate (!resultsIsFinal on the RESULTS remote-
    vacate detector) keeps the 0.75s vacate detector OFF the final standings, so the
    joiner's earned FINISHED -> CEREMONY is NOT pre-empted by a LEFT even though the
    host has vanished during the ~10s dwell. Without the gate the detector would trip
    LEFT first; this lane fails (LEFT appears / reason != FINISHED) in that case.
    The host is NOT pressed at the terminal (the probe suppresses it), so the DWELL
    -- not a press -- is unambiguously what leaves. Rounds 1..N-1 proceed normally."""
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
                "MDKR_TEST_ONLINE_RESULTS_JOINER_TERMINAL": "1",
                "MDKR_TEST_ONLINE_REMOTE_VACATE_AT_RESULTS_FINAL": "1",
                "MDKR_TEST_ONLINE_CEREMONY_SKIP": "1",
            })
    except subprocess.TimeoutExpired as error:
        return fail(f"[{tag}] run timed out (a reintroduced hang, or the vacate "
                    f"detector pre-empted the dwell without a clean exit?): {error}")
    guard = _common_joiner_asserts(tag, rc, output, "self-advance")
    if guard is not None:
        return guard
    # THE P2 ASSERTION: the RESULTS remote-vacate detector must NOT have tripped at
    # the final standings. A "vacated at" LEFT here is exactly the earned-FINISHED
    # pre-emption the gate exists to prevent.
    if VACATE_LEFT_RE.search(output):
        return fail(f"[{tag}] the RESULTS remote-vacate detector tripped a LEFT at "
                    f"the final standings -- the P2 gate (!resultsIsFinal) is NOT "
                    f"holding; the joiner's earned FINISHED was pre-empted", output)
    ends = SESSION_END_RE.findall(output)
    if not any(reason == "FINISHED" and code == "0" for reason, code in ends):
        return fail(f"[{tag}] launcher never read reason=FINISHED result=0 (a LEFT "
                    f"would mean the vacate detector won the race); saw {ends}", output)
    for reason, _code in ends:
        if reason != "FINISHED":
            return fail(f"[{tag}] an unexpected session-end reason {reason!r} leaked "
                        f"-- the completed-cup joiner must finish FINISHED, not LEFT",
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

    for scenario in (check_press, check_self_advance, check_remote_gone_final):
        result = scenario(binary, rom, args.verbose)
        if result is not None:
            return result

    print(
        "PASS online joiner-terminal: the JOINER's FINAL-standings terminal has a "
        "bound INDEPENDENT of the host, proven on the REAL no-seam path (feed STAYS "
        "in RESULTS -- NEVER feed-departed, NEVER the masking "
        "MDKR_TEST_ONLINE_RESULTS_JOINER_FINISH seam): (press) a full descriptor-"
        "less tournament + an honored terminal A press -> CEREMONY -> the single "
        "FINISHED, launcher reads reason=FINISHED result=0, rc 0; (self-advance) a "
        "RESIDENT single-final-race soak with NO terminal input -> the joiner "
        "self-advances on its own countdown DWELL (impossible-to-hang) -> CEREMONY "
        "-> the single FINISHED, rc 0, the final-standings countdown having "
        "decremented to ~0 first; (remote-gone-final, P2) with the remote seat FORCED "
        "GONE at the final standings the joiner STILL reaches FINISHED via the dwell "
        "-- the !resultsIsFinal gate keeps the RESULTS remote-vacate detector from "
        "pre-empting the earned FINISHED with a LEFT (no 'vacated at' LEFT, reason "
        "FINISHED, full 4-round cup). The host-press terminal path never fired.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
