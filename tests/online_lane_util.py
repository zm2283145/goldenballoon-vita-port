"""Shared boilerplate for the native online-takeover engine lanes.

Every tests/check_online_*.py lane boots the real engine off the ROM under the
same headless environment, scans for the same fatal/rejection markers, and
matches the same handful of stderr witness lines. Each lane used to carry its own
byte-identical copy of all of that. This module is the single source, in the
spirit of tests/harness_utils.py (whose find_fatal we reuse here rather than
re-implement).

Everything here is TEST-ONLY: no product TU, no OFF-compiled path, no offline
byte-identity. The lanes are timing-sensitive and MUST run one at a time.
"""

from __future__ import annotations

import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

from harness_utils import find_fatal

# --------------------------------------------------------------------------- #
#  Constants mirrored from the engine / launcher id space
# --------------------------------------------------------------------------- #

# GAMEMODE_ONLINE_SESSION (thread3_main.h): the separated online session mode the
# hand-off witness must report, proving the offline menu state machine was bypassed.
GAMEMODE_ONLINE_SESSION = 2
# MDKR_ONLINE_RACE_RESULT_NONE: an absent placement in a captured finish order.
PLACE_NONE = 255
# A DKR cup is four rounds (the "RACE n/4" / final-round finality copy).
CUP_ROUNDS = 4

# --------------------------------------------------------------------------- #
#  Forbidden markers
# --------------------------------------------------------------------------- #

# The online-takeover rejection markers, in addition to the engine fatals
# harness_utils.find_fatal already covers ([FATAL]/[CRASH]/sanitizers). These are
# matched as LITERAL substrings, never regex: several online witnesses contain
# [brackets] that a regex would read as a character class and mis-match.
FORBIDDEN_ONLINE = (
    "online race admission rejected",
    "launcher input provider rejected",
    "engine startup rejected before authored tick one",
)


def forbidden_marker(output: str, *extra: str) -> str | None:
    """The first fatal or forbidden marker in ``output``, or None.

    Standard engine fatals ([FATAL]/[CRASH]/sanitizers/runtime error) go through
    harness_utils.find_fatal; the online rejection markers (and any ``extra`` a
    lane adds) are matched as literal substrings so bracketed witnesses stay
    literal.
    """
    hit = find_fatal(output)
    if hit is not None:
        return hit
    for marker in extra:
        if marker in output:
            return marker
    return None


# --------------------------------------------------------------------------- #
#  Shared stderr witness regexes (byte-identical across the lanes that use them)
# --------------------------------------------------------------------------- #

# The session's RACE hand-off: how many LOBBY_WAIT ticks, then the isolation proof
# (gGameMode == GAMEMODE_ONLINE_SESSION, gCurrentMenuId still 0 == offline menu
# never entered).
SESSION_RACE_RE = re.compile(
    r"^\[online-session\] phase=RACE booting after (\d+) LOBBY_WAIT tick\(s\); "
    r"isolation gGameMode=(\d+) gCurrentMenuId=(-?\d+)", re.MULTILINE)

# The direct-boot seam: the race was reached straight from the manifest.
DIRECT_BOOT_RE = re.compile(
    r"^\[online-boot\] direct race: track=(\d+) players=(\d+)$", re.MULTILINE)

# The engine's per-race online rollback banner (loaded track / race type / Hz).
ONLINE_RACE_RE = re.compile(
    r"^\[ROLLBACK\] online race: loadedTrack=(\d+) raceType=(\d+) "
    r"authoredHz=(\d+)$", re.MULTILINE)

# The live-transport race summary: 13 capture groups incl. the two endpoint hashes
# and the converged flag.
ENGINE_LIVE_RE = re.compile(
    r"^\[ENGINE-ONLINE-LIVE\] result=(-?\d+) racedTicks=(\d+) drainCalls=(\d+) "
    r"advanceFailed=(\d+) inputEnvelopes=(\d+) transportAccepted=(\d+) "
    r"transportCorrected=(\d+) transportDrained=(\d+) foldVisible=(\d+) "
    r"foldPeer=(\d+) hashVisible=([0-9a-f]{16}) hashPeer=([0-9a-f]{16}) "
    r"converged=(\d+)$", re.MULTILINE)

# The launcher-visible session end reason + result.
SESSION_END_RE = re.compile(
    r"^\[online-session-end\] reason=(\w+) result=(-?\d+)", re.MULTILINE)

# The engine-side FINISHED handshake at the final standings.
FINISHED_ENGINE_RE = re.compile(
    r"^\[online-session\] FINISHED: final standings", re.MULTILINE)

# The native "more races" RESULTS chooser committing the FINISH option: the host
# navigated to FINISH (index 5) and pressed A. This is online_results.c's own
# commit-path line, emitted just before the chooser returns LEAVE at the final
# standings. Lanes that reach FINISHED via MDKR_TEST_ONLINE_RESULTS_CHOOSER=5 assert
# THIS in addition to the downstream FINISHED so a wrong chooser route (a different
# committed option, or a FINISHED reached by some other path) fails loudly + directly.
# Two authoritative forms: the direct commit (single-race / non-wrap paths) and the
# tournament-final wrap commit, where FINISH is deferred until the room's departure
# from RESULTS converges (the reducer-observable wrap) before returning LEAVE.
CHOOSER_FINISH_RE = re.compile(
    r"^\[online-results\] chooser: "
    r"(?:committed option=FINISH -> LEAVE"
    r"|FINISH wrap converged \(room left RESULTS\) -> LEAVE \(ceremony\))$",
    re.MULTILINE)


# --------------------------------------------------------------------------- #
#  Environment + runner
# --------------------------------------------------------------------------- #


def clean_environment(**updates: str) -> dict[str, str]:
    """A pristine engine environment (no inherited MDKR*/GE007_ vars) plus updates."""
    environment = {
        key: value for key, value in os.environ.items()
        if not key.startswith(("MDKR", "GE007_"))
    }
    environment.update(updates)
    return environment


def make_fail(feature: str):
    """A ``fail(message, output="")`` reporter prefixed ``FAIL online <feature>:``."""
    def fail(message: str, output: str = "") -> int:
        print(f"FAIL online {feature}: {message}", file=sys.stderr)
        if output:
            print(output[-16000:], file=sys.stderr)
        return 1
    return fail


def engine_env(run_dir: Path, rom: Path, ticks: int,
               extra_env: dict[str, str] | None = None) -> dict[str, str]:
    """The shared ~15-key headless engine environment for an online lane.

    Lanes pass their own MDKR_APP_TEST_ONLINE_* / MDKR_TEST_ONLINE_* seams via
    ``extra_env`` (merged last, so a lane can also override a base value).
    """
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
    if extra_env:
        environment.update(extra_env)
    return environment


def run_engine(binary: Path, rom: Path, *, ticks: int, timeout: int = 300,
               verbose: bool = False, extra_env: dict[str, str] | None = None,
               prefix: str = "mdkr64-online-") -> tuple[int, str]:
    """Boot the engine once headless; return ``(returncode, combined_output)``.

    Creates a scratch run dir with saves/ + preferences/, builds the shared
    :func:`engine_env` (merging ``extra_env``), and runs the binary with stdout +
    stderr combined. A TimeoutExpired propagates to the caller (a stall).
    """
    with tempfile.TemporaryDirectory(prefix=prefix) as temp:
        run_dir = Path(temp)
        (run_dir / "saves").mkdir()
        (run_dir / "preferences").mkdir()
        environment = engine_env(run_dir, rom, ticks, extra_env)
        if verbose:
            extras = " ".join(f"{k}={v}" for k, v in (extra_env or {}).items())
            print(f"$ {extras} {binary}".strip(), flush=True)
        process = subprocess.run(
            [str(binary)], cwd=run_dir, env=environment, text=True,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            timeout=timeout, check=False,
        )
        return process.returncode, (process.stdout or "")
