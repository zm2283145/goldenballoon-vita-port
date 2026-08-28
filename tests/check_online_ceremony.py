#!/usr/bin/env python3
"""PD-T6f: the native champion CEREMONY sits between the final STANDINGS and the
still-single, still-unchanged FINISHED handshake -- and can NEVER hang the session.

After a full descriptor-less TOURNAMENT (composed on the SAME loopback rig
check_online_lobby_tournament.py drives), the host "A: FINISH" on the FINAL
standings no longer notes FINISHED inline: it DETOURS into the native champion
ceremony (a bounded, auto-advancing 2D celebration of the cup winner), and the
EXACT PD-T6d FINISHED note + launcher reason=FINISHED read fires EXACTLY ONCE once
the ceremony ends. This lane proves:

  (skip)   a ceremony ENTER + RENDER witness appears strictly BETWEEN the final
           `[online-results] render stage=standings ... final=1` and the single
           `[online-session] FINISHED` note; the champion the ceremony crowns is
           the SAME seat/points the STANDINGS ranked #1; FINISHED fires exactly
           ONCE, the launcher reads reason=FINISHED result=0, rc == 0. A scripted
           skip env (MDKR_TEST_ONLINE_CEREMONY_SKIP) means the lane never waits out
           the real-time hold.

  (auto)   with NO skip and NO input, the ceremony auto-advances on its own bounded
           timer to the SAME single FINISHED -- the "impossible to hang" proof (it
           needs no press from anyone), witnessed as `done ... (auto)`.

  (vacate) a remote seat vacating mid-ceremony ends it PROMPTLY (never a park) into
           the same single FINISHED, witnessed as `... (remote vacated)`.

Two-endpoint loopback, tournament cup 1 (Snowflake) -- same as the keystone lane.
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

DIRECT_BOOT_RE = re.compile(
    r"^\[online-boot\] direct race: track=(\d+) players=(\d+)$", re.MULTILINE)
# Final-standings render (RESULTS): the champion is the max-points seat here.
STANDINGS_FINAL_RE = re.compile(
    r"^\[online-results\] render stage=standings mode=\d+ race=\d+ host=\d+ "
    r"placements=[\d,]+ points=(\d+),(\d+),(\d+),(\d+) secs=\d+ final=1$",
    re.MULTILINE)
CEREMONY_ENTER_RE = re.compile(
    r"^\[online-ceremony\] enter: champion seat=(\d+) name=(\S+) points=(\d+) "
    r"seats=(\d+)", re.MULTILINE)
CEREMONY_RENDER_RE = re.compile(
    r"^\[online-ceremony\] render champion=(\d+) points=(\d+) secs=(\d+) "
    r"host=(\d+) seats=(\d+)", re.MULTILINE)
CEREMONY_DONE_RE = re.compile(
    r"^\[online-ceremony\] done: .* -> FINISHED handshake$", re.MULTILINE)
CEREMONY_DONE_AUTO_RE = re.compile(
    r"^\[online-ceremony\] done: champion celebration complete \(auto\)",
    re.MULTILINE)
CEREMONY_DONE_SKIP_RE = re.compile(
    r"^\[online-ceremony\] done: champion celebration complete \(skip\)",
    re.MULTILINE)
CEREMONY_DONE_VACATE_RE = re.compile(
    r"^\[online-ceremony\] done: champion celebration ended early "
    r"\(remote vacated\)", re.MULTILINE)
CEREMONY_EXIT_RE = re.compile(
    r"^\[online-ceremony\] exit: freed portrait assets$", re.MULTILINE)
PHASE_CEREMONY_RE = re.compile(
    r"^\[online-session\] phase=CEREMONY: final standings", re.MULTILINE)
FINISHED_ENGINE_RE = re.compile(
    r"^\[online-session\] FINISHED: final standings", re.MULTILINE)
SESSION_END_RE = re.compile(
    r"^\[online-session-end\] reason=(\w+) result=(-?\d+)", re.MULTILINE)
POSTRACE_EXIT = "[online-postrace] session end requested"

FORBIDDEN = ("[FATAL]", "[CRASH]", "AddressSanitizer",
             "online race admission rejected",
             "launcher input provider rejected",
             "engine startup rejected before authored tick one",
             "[online-resident-live] round advance error",
             "[online-resident-live] round advance FAILED",
             "[online-tournament] result=error")

TOURNAMENT_ENV = {
    "MDKR_APP_TEST_ONLINE_LIVE_LOBBY_START": "1",
    "MDKR_APP_TEST_ONLINE_MODE": "tournament",
    "MDKR_APP_TEST_ONLINE_CUP": str(CUP),
    "MDKR_TEST_ONLINE_LOBBY_START": "1",
    "MDKR_TEST_ONLINE_LOBBY_TOURNAMENT": "1",
    "MDKR_TEST_ONLINE_RESULTS_HOST_PRESS": "1",
}


def fail(message: str, output: str = "") -> int:
    print(f"FAIL online ceremony: {message}", file=sys.stderr)
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
    with tempfile.TemporaryDirectory(prefix="mdkr64-ceremony-") as temp:
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


def _common_finish_asserts(tag: str, rc: int, output: str) -> int | None:
    """Every scenario shares this contract: full cup, one detour, ONE FINISHED,
    launcher reads FINISHED result=0, clean rc 0, no forbidden markers/park."""
    for marker in FORBIDDEN:
        if marker in output:
            return fail(f"[{tag}] observed forbidden marker {marker!r}", output)
    if rc != 0:
        return fail(f"[{tag}] process exited {rc} (expected clean 0)", output)
    if POSTRACE_EXIT in output:
        return fail(f"[{tag}] took the platform-exit path mid-cup", output)
    boots = DIRECT_BOOT_RE.findall(output)
    if len(boots) != CUP_ROUNDS:
        return fail(f"[{tag}] expected {CUP_ROUNDS} direct boots (the ceremony must "
                    f"not perturb the cup), saw {len(boots)}", output)
    # The detour actually happened (RESULTS -> CEREMONY, not RESULTS -> FINISHED).
    if not PHASE_CEREMONY_RE.search(output):
        return fail(f"[{tag}] the session never detoured into the CEREMONY phase "
                    f"(RESULTS went straight to FINISHED)", output)
    # FINISHED fires EXACTLY ONCE, after the ceremony, unchanged.
    finished = FINISHED_ENGINE_RE.findall(output)
    if len(finished) != 1:
        return fail(f"[{tag}] FINISHED must fire EXACTLY ONCE, saw {len(finished)}",
                    output)
    ends = SESSION_END_RE.findall(output)
    if not any(reason == "FINISHED" and code == "0" for reason, code in ends):
        return fail(f"[{tag}] launcher never read reason=FINISHED result=0; saw "
                    f"{ends}", output)
    # No OTHER end reason leaked (the ceremony must not change the channel).
    for reason, _code in ends:
        if reason != "FINISHED":
            return fail(f"[{tag}] an unexpected session-end reason {reason!r} "
                        f"leaked (the ceremony must keep the FINISHED channel)",
                        output)
    # The ceremony entered, rendered at least once, and freed its assets.
    if not CEREMONY_ENTER_RE.search(output):
        return fail(f"[{tag}] no ceremony ENTER witness", output)
    if not CEREMONY_RENDER_RE.search(output):
        return fail(f"[{tag}] no ceremony RENDER witness", output)
    if not CEREMONY_EXIT_RE.search(output):
        return fail(f"[{tag}] the ceremony never freed its portrait assets "
                    f"(asset-free-on-vacate symmetry broken)", output)
    return None


def _champion_matches_standings(tag: str, output: str) -> int | None:
    """The ceremony's champion (order[0]) must be the SAME seat/points the final
    STANDINGS ranked #1 -- the DRY-sort agreement."""
    standings = STANDINGS_FINAL_RE.findall(output)
    if not standings:
        return fail(f"[{tag}] no FINAL standings render (final=1) to cross-check "
                    f"the champion against", output)
    pts = [int(p) for p in standings[-1]]  # last final-standings render
    champ_points = max(pts)
    champ_seat = pts.index(champ_points)
    enter = CEREMONY_ENTER_RE.search(output)
    if not enter:
        return fail(f"[{tag}] no ceremony ENTER witness to read the champion from",
                    output)
    seat, _name, points, _seats = enter.groups()
    if int(points) != champ_points:
        return fail(f"[{tag}] ceremony champion points={points} != the standings "
                    f"#1 total {champ_points} (the DRY sort disagreed)", output)
    if int(seat) != champ_seat:
        return fail(f"[{tag}] ceremony champion seat={seat} != the standings #1 "
                    f"seat {champ_seat}", output)
    # The champion witness also matches on the render line.
    render = CEREMONY_RENDER_RE.search(output)
    if render and int(render.group(2)) != champ_points:
        return fail(f"[{tag}] ceremony render points={render.group(2)} != champion "
                    f"total {champ_points}", output)
    return None  # ok


def _order_between(tag: str, output: str) -> int | None:
    """The ceremony enter/render must sit strictly BETWEEN the final STANDINGS
    witness and the FINISHED note."""
    m_std = list(STANDINGS_FINAL_RE.finditer(output))
    m_enter = CEREMONY_ENTER_RE.search(output)
    m_render = CEREMONY_RENDER_RE.search(output)
    m_fin = FINISHED_ENGINE_RE.search(output)
    if not (m_std and m_enter and m_render and m_fin):
        return fail(f"[{tag}] missing an ordering witness (std={bool(m_std)} "
                    f"enter={bool(m_enter)} render={bool(m_render)} "
                    f"finished={bool(m_fin)})", output)
    last_std = m_std[-1].start()
    if not (last_std < m_enter.start() < m_render.start() < m_fin.start()):
        return fail(f"[{tag}] witnesses out of order: final-standings@{last_std} "
                    f"enter@{m_enter.start()} render@{m_render.start()} "
                    f"finished@{m_fin.start()} (ceremony must sit between the final "
                    f"standings and FINISHED)", output)
    return None


def check_skip(binary: Path, rom: Path, verbose: bool) -> int | None:
    """The primary detour proof, with the scripted skip so CI never waits out the
    hold: ceremony between standings-final and the single FINISHED."""
    tag = "skip"
    try:
        rc, output = run_engine(
            binary, rom, TICKS, 900, verbose,
            extra_env={**TOURNAMENT_ENV, "MDKR_TEST_ONLINE_CEREMONY_SKIP": "1"})
    except subprocess.TimeoutExpired as error:
        return fail(f"[{tag}] run timed out (ceremony held despite the skip?): "
                    f"{error}")
    guard = _common_finish_asserts(tag, rc, output)
    if guard is not None:
        return guard
    guard = _champion_matches_standings(tag, output)
    if guard is not None:
        return guard
    guard = _order_between(tag, output)
    if guard is not None:
        return guard
    if not CEREMONY_DONE_SKIP_RE.search(output):
        return fail(f"[{tag}] the ceremony did not end via the scripted skip", output)
    return None


def check_auto(binary: Path, rom: Path, verbose: bool) -> int | None:
    """No skip, no input: the ceremony auto-advances on its own bounded timer to
    the SAME single FINISHED -- proof it can NEVER hang (needs no press)."""
    tag = "auto"
    try:
        rc, output = run_engine(
            binary, rom, ticks=40000, timeout=900, verbose=verbose,
            extra_env=dict(TOURNAMENT_ENV))
    except subprocess.TimeoutExpired as error:
        return fail(f"[{tag}] run HUNG (the ceremony timer did not auto-advance): "
                    f"{error}")
    guard = _common_finish_asserts(tag, rc, output)
    if guard is not None:
        return guard
    guard = _order_between(tag, output)
    if guard is not None:
        return guard
    if not CEREMONY_DONE_AUTO_RE.search(output):
        return fail(f"[{tag}] the ceremony did not auto-advance on its timer (no "
                    f"`done ... (auto)` witness) -- the no-input path is unproven",
                    output)
    # The timer really counted down: at least one render showed secs > 0 and one 0.
    renders = CEREMONY_RENDER_RE.findall(output)
    secs = sorted({int(r[2]) for r in renders})
    if len(secs) < 2 or min(secs) > 1:
        return fail(f"[{tag}] the ceremony countdown did not visibly progress "
                    f"(secs seen {secs})", output)
    return None


def check_vacate(binary: Path, rom: Path, verbose: bool) -> int | None:
    """A remote seat vacating mid-ceremony ends it PROMPTLY (never a park) into the
    same single FINISHED."""
    tag = "vacate"
    try:
        rc, output = run_engine(
            binary, rom, TICKS, 900, verbose,
            extra_env={**TOURNAMENT_ENV, "MDKR_TEST_ONLINE_CEREMONY_VACATE": "1"})
    except subprocess.TimeoutExpired as error:
        return fail(f"[{tag}] run HUNG (the vacate did not end the ceremony): "
                    f"{error}")
    guard = _common_finish_asserts(tag, rc, output)
    if guard is not None:
        return guard
    guard = _order_between(tag, output)
    if guard is not None:
        return guard
    if not CEREMONY_DONE_VACATE_RE.search(output):
        return fail(f"[{tag}] the ceremony did not end via the remote-vacate path",
                    output)
    if CEREMONY_DONE_AUTO_RE.search(output):
        return fail(f"[{tag}] the ceremony ran to its full timer instead of ending "
                    f"promptly on the vacate", output)
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

    for scenario in (check_skip, check_auto, check_vacate):
        result = scenario(binary, rom, args.verbose)
        if result is not None:
            return result

    print(
        "PASS online ceremony: the native champion CEREMONY detours between the "
        "final STANDINGS and the still-single, still-unchanged FINISHED handshake "
        "-- (skip) ceremony ENTER+RENDER sit strictly between the final standings "
        "and the ONE FINISHED note (launcher reason=FINISHED result=0, rc 0), the "
        "champion it crowns is byte-for-byte the STANDINGS #1 seat/points; (auto) "
        "with NO skip and NO input the ceremony auto-advances on its bounded timer "
        "to the same single FINISHED (impossible-to-hang proof); (vacate) a remote "
        "vacating mid-ceremony ends it promptly into the same single FINISHED, "
        "never a park; assets freed on every exit path.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
