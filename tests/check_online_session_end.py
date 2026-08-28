#!/usr/bin/env python3
"""PD-T6d: the engine->launcher FINISH/RETURN handshake (session end-reason).

After a native online session ends, the engine notes WHY on the party_link
session-end channel (mdkr_party_link_note_session_end) and requests the platform
exit; the launcher's runOnlineLobbyStart{Live,Engine}Session TAKES that reason
(mdkr_party_link_take_session_end) right after mdkr64_engine_boot returns and
BEFORE OnlineRoom_clearPartyLink(), logs a witness, and the interactive loop
resumes the Online Room with the adapter/room intact. This lane proves EACH end
reason end-to-end -- the engine-side note AND the launcher-side read -- on the
descriptor-less loopback lobby-start rig:

  FINISHED  tournament complete: the host "A: FINISH" on the FINAL standings ->
            engine notes FINISHED + exit 0 -> launcher reads reason=FINISHED.
  LEFT (charselect backout): a genuine browse-B backout on CHARSELECT ->
            engine notes LEFT + exit 0 -> launcher reads reason=LEFT. (The scripted
            lobby-start lanes' tick-3 I1 browse-B still STAYs via the warn-once
            stub; only the dedicated backout seam / a live pad leaves.)
  LEFT (remote vacated pre-START, Minor-3): the forward-feed remote seat vacates
            while waiting on CHARSELECT -> the debounced detector notes LEFT + exit
            0 -> launcher reads reason=LEFT (never an indefinite park).
  LEFT (mid-tournament cancel, Minor-4): a leader CANCEL_LOADING mid round-2 ->
            the engine UNWINDS to a CLEAN LEFT return (exit 0), replacing the
            PD-T6h2c re-front-into-error -> launcher reads reason=LEFT.
  ERROR     a stuck descriptor-less wait trips the WALL-CLOCK watchdog -> engine
            notes ERROR + exit 2 -> launcher reads reason=ERROR (nonzero rc).

Each scenario runs through the loopback lobby-start dispatch
(runOnlineLobbyStartEngineSession), so the SAME launcher read that a real
room-ready takeover uses is exercised.
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

SESSION_END_RE = re.compile(
    r"^\[online-session-end\] reason=(\w+) result=(-?\d+)", re.MULTILINE)
FINISHED_ENGINE_RE = re.compile(
    r"^\[online-session\] FINISHED: final standings", re.MULTILINE)
JOINER_FOLLOW_RE = re.compile(
    r"^\[online-results\] finish: joiner terminal advance \((?:feed-departed|press|"
    r"self-advance)\) -> LEAVE",
    re.MULTILINE)
HOST_FINISH_RE = re.compile(
    r"^\[online-results\] finish: host A -> LEAVE", re.MULTILINE)
CHARSELECT_LEFT_RE = re.compile(
    r"^\[online-session\] LEFT: charselect backout", re.MULTILINE)
VACATE_LEFT_RE = re.compile(
    r"^\[online-session\] LEFT: remote seat vacated pre-START", re.MULTILINE)
MID_UNWIND_LEFT_RE = re.compile(
    r"^\[online-session\] mid-tournament UNWIND: .* -> LEFT: return to room",
    re.MULTILINE)
WATCHDOG_ERROR_RE = re.compile(
    r"^\[online-session\] descless wait TIMEOUT: .* -- ERROR:", re.MULTILINE)
DIRECT_BOOT_RE = re.compile(
    r"^\[online-boot\] direct race: track=(\d+) players=(\d+)$", re.MULTILINE)

FORBIDDEN = ("[FATAL]", "[CRASH]", "AddressSanitizer",
             "online race admission rejected")


def fail(message: str, output: str = "") -> int:
    print(f"FAIL online session-end: {message}", file=sys.stderr)
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
    with tempfile.TemporaryDirectory(prefix="mdkr64-session-end-") as temp:
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


def _no_forbidden(tag: str, output: str) -> int | None:
    for marker in FORBIDDEN:
        if marker in output:
            return fail(f"[{tag}] observed forbidden marker {marker!r}", output)
    return None


def _reads(output: str) -> list[tuple[str, str]]:
    return SESSION_END_RE.findall(output)


def check_finished(binary: Path, rom: Path, verbose: bool) -> int | None:
    """FINISHED: host A:FINISH on the final standings -> clean return-to-room."""
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
                # PD-T6f: skip the native champion CEREMONY's bounded hold so the
                # FINISHED handshake this lane asserts still fires promptly (exactly
                # once, unchanged reason/result) with the ceremony in the path.
                "MDKR_TEST_ONLINE_CEREMONY_SKIP": "1",
            })
    except subprocess.TimeoutExpired as error:
        return fail(f"[FINISHED] run timed out (final standings held instead of "
                    f"returning?): {error}")
    guard = _no_forbidden("FINISHED", output)
    if guard is not None:
        return guard
    if rc != 0:
        return fail(f"[FINISHED] exited {rc} (expected clean 0)", output)
    if not FINISHED_ENGINE_RE.search(output):
        return fail("[FINISHED] engine never noted FINISHED at the final standings",
                    output)
    if len(DIRECT_BOOT_RE.findall(output)) != CUP_ROUNDS:
        return fail("[FINISHED] the full cup did not run before the FINISH", output)
    reads = _reads(output)
    if not any(r == "FINISHED" and c == "0" for r, c in reads):
        return fail(f"[FINISHED] launcher never read reason=FINISHED result=0; saw "
                    f"{reads}", output)
    return None


def check_finished_joiner(binary: Path, rom: Path, verbose: bool) -> int | None:
    """FINISHED (IMPORTANT-1): a NON-HOST (joiner) FOLLOWS the host out of the final
    standings -> clean return-to-room, instead of parking until window-close. The
    joiner-finish seam suppresses the host "A: FINISH" at the terminal and, after a
    render grace, forces the joiner + host-departed inputs so the non-host follow
    return is exercised end-to-end (engine FINISHED note + launcher reason=FINISHED).
    The visible endpoint drove rounds 1..N-1 as host (the seam is terminal-only)."""
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
                "MDKR_TEST_ONLINE_RESULTS_JOINER_FINISH": "1",
                # PD-T6f: skip the ceremony hold; the JOINER still follows the host
                # out of the final standings, through the (near-instant) ceremony,
                # to the single FINISHED (IMPORTANT-1 no-park stays proven).
                "MDKR_TEST_ONLINE_CEREMONY_SKIP": "1",
            })
    except subprocess.TimeoutExpired as error:
        return fail(f"[FINISHED-joiner] run timed out (joiner parked on the final "
                    f"standings instead of following the host out?): {error}")
    guard = _no_forbidden("FINISHED-joiner", output)
    if guard is not None:
        return guard
    if rc != 0:
        return fail(f"[FINISHED-joiner] exited {rc} (expected clean 0)", output)
    if not JOINER_FOLLOW_RE.search(output):
        return fail("[FINISHED-joiner] the joiner never FOLLOWED the host out of "
                    "the final standings (it would park until window-close)", output)
    if HOST_FINISH_RE.search(output):
        return fail("[FINISHED-joiner] the host-FINISH path fired -- the seam must "
                    "exercise the JOINER follow, not the host press", output)
    if not FINISHED_ENGINE_RE.search(output):
        return fail("[FINISHED-joiner] the session never mapped the joiner LEAVE to "
                    "FINISHED", output)
    if len(DIRECT_BOOT_RE.findall(output)) != CUP_ROUNDS:
        return fail("[FINISHED-joiner] the full cup did not run before the FINISH "
                    "(the visible endpoint should drive rounds 1..N-1 as host)",
                    output)
    reads = _reads(output)
    if not any(r == "FINISHED" and c == "0" for r, c in reads):
        return fail(f"[FINISHED-joiner] launcher never read reason=FINISHED result=0; "
                    f"saw {reads}", output)
    return None


def check_left_charselect(binary: Path, rom: Path, verbose: bool) -> int | None:
    """LEFT: a genuine CHARSELECT browse-B backout -> clean return-to-room."""
    try:
        rc, output = run_engine(
            binary, rom, ticks=4000, timeout=200, verbose=verbose,
            extra_env={
                "MDKR_APP_TEST_ONLINE_LIVE_LOBBY_START": "1",
                "MDKR_TEST_ONLINE_LOBBY_START": "1",
                "MDKR_TEST_ONLINE_CHARSELECT_BACKOUT": "1",
            })
    except subprocess.TimeoutExpired as error:
        return fail(f"[LEFT-charselect] run HUNG (backout did not leave): {error}")
    guard = _no_forbidden("LEFT-charselect", output)
    if guard is not None:
        return guard
    if rc != 0:
        return fail(f"[LEFT-charselect] exited {rc} (expected clean 0)", output)
    if not CHARSELECT_LEFT_RE.search(output):
        return fail("[LEFT-charselect] engine never noted the charselect backout "
                    "LEFT (the browse-B did not leave to the room)", output)
    if DIRECT_BOOT_RE.findall(output):
        return fail("[LEFT-charselect] a race booted -- the backout must leave "
                    "BEFORE the race starts", output)
    reads = _reads(output)
    if not any(r == "LEFT" and c == "0" for r, c in reads):
        return fail(f"[LEFT-charselect] launcher never read reason=LEFT result=0; "
                    f"saw {reads}", output)
    return None


def check_left_remote_vacate(binary: Path, rom: Path, verbose: bool) -> int | None:
    """LEFT (Minor-3): the remote seat vacates pre-START -> clean return-to-room."""
    try:
        rc, output = run_engine(
            binary, rom, ticks=4000, timeout=200, verbose=verbose,
            extra_env={
                "MDKR_APP_TEST_ONLINE_LIVE_LOBBY_START": "1",
                "MDKR_TEST_ONLINE_LOBBY_START": "1",
                "MDKR_TEST_ONLINE_REMOTE_VACATE": "1",
            })
    except subprocess.TimeoutExpired as error:
        return fail(f"[LEFT-vacate] run HUNG (parked on the vacated remote): {error}")
    guard = _no_forbidden("LEFT-vacate", output)
    if guard is not None:
        return guard
    if rc != 0:
        return fail(f"[LEFT-vacate] exited {rc} (expected clean 0)", output)
    if not VACATE_LEFT_RE.search(output):
        return fail("[LEFT-vacate] engine never noted the pre-START remote-vacated "
                    "LEFT (it would park indefinitely)", output)
    if DIRECT_BOOT_RE.findall(output):
        return fail("[LEFT-vacate] a race booted -- a vacated remote must never "
                    "reach a race start", output)
    reads = _reads(output)
    if not any(r == "LEFT" and c == "0" for r, c in reads):
        return fail(f"[LEFT-vacate] launcher never read reason=LEFT result=0; saw "
                    f"{reads}", output)
    return None


def check_left_mid_cancel(binary: Path, rom: Path, verbose: bool) -> int | None:
    """LEFT (Minor-4): a leader mid-tournament CANCEL -> clean return-to-room."""
    try:
        rc, output = run_engine(
            binary, rom, ticks=40000, timeout=400, verbose=verbose,
            extra_env={
                "MDKR_APP_TEST_ONLINE_LIVE_LOBBY_START": "1",
                "MDKR_APP_TEST_ONLINE_MODE": "tournament",
                "MDKR_APP_TEST_ONLINE_CUP": str(CUP),
                "MDKR_TEST_ONLINE_LOBBY_START": "1",
                "MDKR_TEST_ONLINE_LOBBY_TOURNAMENT": "1",
                "MDKR_TEST_ONLINE_RESULTS_HOST_PRESS": "1",
                "MDKR_APP_TEST_ONLINE_SINGLE_ENDPOINT": "1",
                "MDKR_APP_TEST_ONLINE_LOBBY_WEDGE": "cancel2",
                "MDKR_ONLINE_SESSION_DESCLESS_WAIT_DEADLINE_MS": "3000",
            })
    except subprocess.TimeoutExpired as error:
        return fail(f"[LEFT-mid-cancel] run HUNG (parked on the cancelled round): "
                    f"{error}")
    guard = _no_forbidden("LEFT-mid-cancel", output)
    if guard is not None:
        return guard
    if rc != 0:
        return fail(f"[LEFT-mid-cancel] exited {rc} (expected a clean LEFT return "
                    f"0, not a re-front-into-error)", output)
    if not MID_UNWIND_LEFT_RE.search(output):
        return fail("[LEFT-mid-cancel] engine did not UNWIND the cancel to a clean "
                    "LEFT return", output)
    if len(DIRECT_BOOT_RE.findall(output)) < 1:
        return fail("[LEFT-mid-cancel] no race booted before the cancel", output)
    reads = _reads(output)
    if not any(r == "LEFT" and c == "0" for r, c in reads):
        return fail(f"[LEFT-mid-cancel] launcher never read reason=LEFT result=0; "
                    f"saw {reads}", output)
    return None


def check_error(binary: Path, rom: Path, verbose: bool) -> int | None:
    """ERROR: a stuck descriptor-less wait trips the wall-clock watchdog (exit 2)."""
    try:
        rc, output = run_engine(
            binary, rom, ticks=6000, timeout=200, verbose=verbose,
            extra_env={
                "MDKR_APP_TEST_ONLINE_LIVE_LOBBY_START": "1",
                "MDKR_TEST_ONLINE_LOBBY_START": "1",
                "MDKR_APP_TEST_ONLINE_SINGLE_ENDPOINT": "1",
                "MDKR_APP_TEST_ONLINE_LOBBY_WEDGE": "descriptor",
                "MDKR_ONLINE_SESSION_DESCLESS_WAIT_DEADLINE_MS": "2000",
            })
    except subprocess.TimeoutExpired as error:
        return fail(f"[ERROR] run HUNG (the watchdog did not fire): {error}")
    for marker in ("[FATAL]", "[CRASH]", "AddressSanitizer"):
        if marker in output:
            return fail(f"[ERROR] forbidden marker {marker!r}", output)
    if rc == 0:
        return fail("[ERROR] exited 0 -- a stuck wait must carry a nonzero ERROR "
                    "signal", output)
    if not WATCHDOG_ERROR_RE.search(output):
        return fail("[ERROR] the wall-clock watchdog never tripped", output)
    reads = _reads(output)
    if not any(r == "ERROR" for r, _c in reads):
        return fail(f"[ERROR] launcher never read reason=ERROR; saw {reads}", output)
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

    for scenario in (check_left_charselect, check_left_remote_vacate,
                     check_error, check_left_mid_cancel, check_finished,
                     check_finished_joiner):
        result = scenario(binary, rom, args.verbose)
        if result is not None:
            return result

    print(
        "PASS online session-end: the PD-T6d engine->launcher FINISH/RETURN "
        "handshake proved every end reason end-to-end (engine note + launcher "
        "read + clean return): FINISHED as HOST (A:FINISH on the final standings, "
        "exit 0), FINISHED as JOINER (IMPORTANT-1: the non-host FOLLOWS the host out "
        "of the final standings instead of parking, exit 0), LEFT via CHARSELECT "
        "backout (exit 0, no race booted), LEFT via remote-vacated pre-START "
        "(Minor-3; debounced, exit 0, no race booted), LEFT via mid-tournament "
        "cancel (Minor-4/T6h2c; clean return replacing the re-front, exit 0), and "
        "ERROR via the wall-clock watchdog (nonzero exit, reason=ERROR).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
