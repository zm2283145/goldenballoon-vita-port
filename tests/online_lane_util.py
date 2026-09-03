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
#  Source-of-truth pins
# --------------------------------------------------------------------------- #

# DKR's authentic trophy-race scoring weights (gTrophyRacePointsArray). BOTH
# tournament lanes apply these weights to derive a cup total from a recorded
# finish order; each used to duplicate the {9,7,5,3,1,0,0,0} literal. The C
# source of truth is kTrophyPoints in platform/online/lobby_core.c (lines 16-17),
# byte-mirrored by the party service. Pinning the literal to that source (below)
# makes a product-side weight change fail the checks loudly instead of letting a
# stale test literal silently disagree with the shipped scoring.
TROPHY_WEIGHTS_SOURCE = "platform/online/lobby_core.c"
TROPHY_WEIGHTS_SYMBOL = "kTrophyPoints"


def scan_trophy_weights(root: Path) -> tuple[int, ...] | None:
    """The authored trophy weights parsed from kTrophyPoints (lobby_core.c).

    Returns the initialiser tuple (e.g. (9, 7, 5, 3, 1, 0, 0, 0)) or None if the
    array cannot be read -- the caller fails closed on None so a moved/renamed
    source can never make the pin vacuously pass.
    """
    try:
        text = (root / TROPHY_WEIGHTS_SOURCE).read_text()
    except OSError:
        return None
    match = re.search(
        re.escape(TROPHY_WEIGHTS_SYMBOL) + r"\s*\[[^\]]*\]\s*=\s*\{([^}]*)\}",
        text)
    if match is None:
        return None
    values = re.findall(r"(\d+)\s*u?", match.group(1))
    if not values:
        return None
    return tuple(int(value) for value in values)


def check_trophy_weights_pin(root: Path, expected: tuple[int, ...],
                             fail) -> int | None:
    """Pin a lane's TROPHY_WEIGHTS literal to the kTrophyPoints C source.

    Mirrors the source-scan discipline of check_cup_rounds_pin: a product-side
    edit to the authored weights (or a drifted test literal) fails here, before
    any engine run, instead of a scoring assertion silently comparing against a
    stale weight. Returns a fail() exit code on drift, else None.
    """
    weights = scan_trophy_weights(root)
    if weights is None:
        return fail(f"could not read {TROPHY_WEIGHTS_SYMBOL} from "
                    f"{TROPHY_WEIGHTS_SOURCE} (the trophy-weight source pin "
                    f"cannot vouch for the {expected} literal)")
    if weights != tuple(expected):
        return fail(f"TROPHY_WEIGHTS drift: {TROPHY_WEIGHTS_SOURCE} "
                    f"{TROPHY_WEIGHTS_SYMBOL}={weights} != the lane's pinned "
                    f"{tuple(expected)} -- a product-side weight change must "
                    f"update the checks that apply it")
    return None


# The online-catalog id -> engine Character mapping the direct-boot racer spawn
# rides (menu.c get_character_id_from_slot -> mdkr_online_character_to_engine).
# The lobby numbers racers in its OWN catalog order and the engine Character enum
# is a DIFFERENT order, so a launch descriptor's character_id is NOT the engine
# character: a lane that compares the two id spaces for EQUALITY is asserting the
# raw-id defect (Tiptup(online 3) spawning CONKER(engine 3), T.T.(online 9)
# spawning DIDDY(engine 9)), not the shipped behaviour. Both orders are read from
# their C sources below so no test literal can drift from the product.
CHARACTER_MAP_SOURCE = "game/src/online/online_character_map.h"
CHARACTER_MAP_SYMBOL = "MDKR_ONLINE_CHARACTER_ENGINE_ORDER"
CHARACTER_ENUM_SOURCE = "game/include/enums.h"


def scan_character_enum(root: Path) -> dict[str, int] | None:
    """CHARACTER_* symbol -> engine Character value, parsed from enums.h.

    Fails closed (None) when the enum cannot be read or any member carries an
    explicit `= value`, which would break the positional numbering this derives.
    """
    try:
        text = (root / CHARACTER_ENUM_SOURCE).read_text()
    except OSError:
        return None
    match = re.search(r"enum\s+Character\s*\{(.*?)\}\s*Character\s*;",
                      text, re.DOTALL)
    if match is None or "=" in match.group(1):
        return None
    names = re.findall(r"\b(CHARACTER_[A-Z_0-9]+)\b", match.group(1))
    if not names:
        return None
    return {name: index for index, name in enumerate(names)}


def scan_online_to_engine(root: Path) -> tuple[int, ...] | None:
    """The online-catalog id -> engine Character table, parsed from its C source.

    Reads MDKR_ONLINE_CHARACTER_ENGINE_ORDER (the single-source macro
    online_character_map.c expands) and resolves each CHARACTER_* symbol through
    the enums.h Character enum. Returns the table indexed by online id, or None
    if either source cannot be read -- callers fail closed on None so a moved or
    renamed source can never make the translation pin vacuously pass.
    """
    enum = scan_character_enum(root)
    if enum is None:
        return None
    try:
        text = (root / CHARACTER_MAP_SOURCE).read_text()
    except OSError:
        return None
    match = re.search(
        r"#define\s+" + re.escape(CHARACTER_MAP_SYMBOL) + r"\b(.*?)(?<!\\)\n\n",
        text + "\n\n", re.DOTALL)
    if match is None:
        return None
    names = re.findall(r"\b(CHARACTER_[A-Z_0-9]+)\b", match.group(1))
    if not names or any(name not in enum for name in names):
        return None
    return tuple(enum[name] for name in names)


# The two witness lines whose id spaces this pin relates: what the launcher
# handed the engine, and what the engine actually seated.
NET_LAUNCH_RE = re.compile(
    r"^\[NET-LAUNCH\] epoch=(\d+) descriptor=([0-9a-f]{16}) track=(\d+) "
    r"selections=((?:\d+:\d+/\d+,?)+)$", re.MULTILINE)
NET_SELECTIONS_RE = re.compile(
    r"^\[NET-SELECTIONS\] epoch=(\d+) racers=((?:\d+:\d+/\d+,?)+) "
    r"source=launch-descriptor$", re.MULTILINE)


def parse_seat_selections(text: str) -> list[tuple[int, int]] | None:
    """`0:9/0,1:4/0,...` -> [(character, vehicle), ...] in seat order."""
    seats: list[tuple[int, int]] = []
    for index, field in enumerate(text.split(",")):
        match = re.fullmatch(r"(\d+):(\d+)/(\d+)", field)
        if match is None or int(match.group(1)) != index:
            return None
        seats.append((int(match.group(2)), int(match.group(3))))
    return seats or None


def check_launch_selection_translation(output: str, root: Path, fail,
                                       seat_count: int | None = None
                                       ) -> int | None:
    """Pin the seated racers to the TRANSLATION of the launch descriptor.

    The descriptor carries online-catalog ids; the engine seats engine Character
    ids. This asserts, per seat, applied == scan_online_to_engine()[descriptor]
    (and vehicles unchanged -- the two vehicle catalogs are asserted equal in C by
    match_launch_builder.c), then requires at least one seat where the two ids
    DIFFER. That last clause is the non-vacuity control: if the engine ever
    regressed to handing the raw online id to the racer spawn, every seat would
    match trivially and the first clause alone would still pass.

    Returns a fail() exit code on any drift, else None.
    """
    launch = NET_LAUNCH_RE.search(output)
    applied = NET_SELECTIONS_RE.search(output)
    if launch is None or applied is None:
        return fail("missing the [NET-LAUNCH]/[NET-SELECTIONS] pair the "
                    "descriptor-vs-applied identity pin needs", output)
    table = scan_online_to_engine(root)
    if table is None:
        return fail(f"could not read {CHARACTER_MAP_SYMBOL} from "
                    f"{CHARACTER_MAP_SOURCE} / the Character enum from "
                    f"{CHARACTER_ENUM_SOURCE} (the identity pin cannot vouch "
                    f"for the seated racers)")
    descriptor = parse_seat_selections(launch.group(4))
    seated = parse_seat_selections(applied.group(2))
    if descriptor is None or seated is None or len(descriptor) != len(seated):
        return fail("unparsable descriptor/applied selection lists", output)
    if seat_count is not None and len(descriptor) != seat_count:
        return fail(f"expected {seat_count} descriptor seats, saw "
                    f"{len(descriptor)}", output)
    translated = 0
    for seat, ((wanted, wantedVehicle), (got, gotVehicle)) in enumerate(
            zip(descriptor, seated)):
        if wanted >= len(table):
            return fail(f"seat {seat} descriptor character {wanted} is outside "
                        f"the {len(table)}-entry online catalog", output)
        if got != table[wanted]:
            return fail(f"seat {seat} raced character {got}; the descriptor "
                        f"asked for online id {wanted}, which is engine "
                        f"character {table[wanted]}", output)
        if gotVehicle != wantedVehicle:
            return fail(f"seat {seat} raced vehicle {gotVehicle}; the "
                        f"descriptor asked for {wantedVehicle}", output)
        if got != wanted:
            translated += 1
    if translated == 0:
        return fail("no seat's engine character differs from its online-catalog "
                    "id, so this run cannot tell a translated spawn from a raw "
                    "passthrough -- the descriptor must seat at least one racer "
                    "whose two ids differ", output)
    return None


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
