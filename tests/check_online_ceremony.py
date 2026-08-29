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
from online_lane_util import (
    CUP_ROUNDS, DIRECT_BOOT_RE, FINISHED_ENGINE_RE, FORBIDDEN_ONLINE,
    SESSION_END_RE, forbidden_marker, make_fail,
)
from online_lane_util import run_engine as _run_engine

ROOT = Path(__file__).resolve().parent.parent
TICKS = 30000
CUP = 1

# Final-standings render (RESULTS): the champion is the max-points seat here.
STANDINGS_FINAL_RE = re.compile(
    r"^\[online-results\] render stage=standings mode=\d+ race=\d+ host=\d+ "
    r"placements=[\d,]+ points=(\d+),(\d+),(\d+),(\d+) secs=\d+ final=1$",
    re.MULTILINE)
CEREMONY_ENTER_RE = re.compile(
    r"^\[online-ceremony\] enter: champion seat=(\d+) name=(.+?) points=(\d+) "
    r"seats=(\d+)", re.MULTILINE)
# I-1: the enter witness also carries champLocal (1 == the crowned seat is THIS
# endpoint's own local seat). Used by the champion-on-disconnect scenario to prove
# the surviving local loser is NOT crowned [YOU].
CEREMONY_ENTER_LOCAL_RE = re.compile(
    r"^\[online-ceremony\] enter: champion seat=(\d+) name=(.+?) points=(\d+) "
    r"seats=(\d+) local=(\d+)", re.MULTILINE)
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
POSTRACE_EXIT = "[online-postrace] session end requested"

# A stalled resident-round or tournament advance is fatal for this lane, on top of
# the shared engine/online forbidden markers.
FORBIDDEN_EXTRA = ("[online-resident-live] round advance error",
                   "[online-resident-live] round advance FAILED",
                   "[online-tournament] result=error")

TOURNAMENT_ENV = {
    "MDKR_APP_TEST_ONLINE_LIVE_LOBBY_START": "1",
    "MDKR_APP_TEST_ONLINE_MODE": "tournament",
    "MDKR_APP_TEST_ONLINE_CUP": str(CUP),
    "MDKR_TEST_ONLINE_LOBBY_START": "1",
    "MDKR_TEST_ONLINE_LOBBY_TOURNAMENT": "1",
    "MDKR_TEST_ONLINE_RESULTS_HOST_PRESS": "1",
    # The MORE-RACES chooser is the sole RESULTS terminal now: at the FINAL
    # standings the host reaches FINISH the way a player would -- navigate to the
    # FINISH option (index 5) and press A -> LEAVE -> the champion CEREMONY this
    # lane witnesses -> the single FINISHED. Inert on the non-final rounds (the
    # chooser fronts only at the FINAL standings; rounds 1..N-1 auto-REMATCH).
    "MDKR_TEST_ONLINE_RESULTS_CHOOSER": "5",
}


fail = make_fail("ceremony")


def run_engine(binary: Path, rom: Path, ticks: int, timeout: int, verbose: bool,
               extra_env: dict[str, str]) -> tuple[int, str]:
    return _run_engine(binary, rom, ticks=ticks, timeout=timeout,
                       verbose=verbose, extra_env=extra_env,
                       prefix="mdkr64-ceremony-")


def _common_finish_asserts(tag: str, rc: int, output: str) -> int | None:
    """Every scenario shares this contract: full cup, one detour, ONE FINISHED,
    launcher reads FINISHED result=0, clean rc 0, no forbidden markers/park."""
    marker = forbidden_marker(output, *FORBIDDEN_ONLINE, *FORBIDDEN_EXTRA)
    if marker:
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
    # Robust to a points tie: the engine's lastpl tie-break picks ONE of the
    # max-point seats, so accept the champion being ANY max-point seat (still
    # catches a non-max, i.e. wrong, champion). On the deterministic rig (34,30)
    # there is a unique max, so this is exact there.
    champ_seats = [i for i, p in enumerate(pts) if p == champ_points]
    enter = CEREMONY_ENTER_RE.search(output)
    if not enter:
        return fail(f"[{tag}] no ceremony ENTER witness to read the champion from",
                    output)
    seat, _name, points, _seats = enter.groups()
    if int(points) != champ_points:
        return fail(f"[{tag}] ceremony champion points={points} != the standings "
                    f"#1 total {champ_points} (the DRY sort disagreed)", output)
    if int(seat) not in champ_seats:
        return fail(f"[{tag}] ceremony champion seat={seat} is not among the "
                    f"max-point seats {champ_seats} (points {pts})", output)
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
    # Prove the DEBOUNCE (15 frames), not the 6s timer: the ceremony ended while
    # the countdown was still (near) full, which only the ~15-frame debounce can do
    # -- the timer path would have counted secs down to 0. (Forced seam overrides
    # only the "remote gone" predicate; the real vacateTicks>=CER_VACATE_DEBOUNCE
    # branch is what fires -- Minor-1.)
    secs = [int(r[2]) for r in CEREMONY_RENDER_RE.findall(output)]
    if not secs:
        return fail(f"[{tag}] no ceremony render witness to time the vacate", output)
    if min(secs) < 5:
        return fail(f"[{tag}] the ceremony countdown ran down to {min(secs)}s before "
                    f"ending -- that is the 6s timer, not the ~15-frame debounce "
                    f"(the real debounce branch was not exercised)", output)
    return None


def check_champion_on_disconnect(binary: Path, rom: Path, verbose: bool) -> int | None:
    """(host-gone-champion, I-1) The champion CEREMONY must crown the TRUE cup
    winner even when the winning seat has disconnected by ceremony enter -- it must
    NOT recompute from a live snapshot that has lost that seat and mis-crown the
    surviving loser.

    Staging (a resident tournament -- a full 4-round cup): the REMOTE seat wins the
    cup (MDKR_TEST_ONLINE_RESIDENT_REMOTE_WINS flips the resident soak's placements
    so slot 1 finishes first every round), so the LOCAL endpoint is the LOSER. Both
    seats are present through the FINAL standings, so the session CAPTURES the true
    ranking (order[0] == the remote winner). Then MDKR_TEST_ONLINE_CEREMONY_REMOTE_ABSENT
    makes the remote seat GENUINELY absent from the ceremony's live snapshot -- a
    real host disconnect at ceremony enter, which no loopback rig can otherwise
    stage (the P2 vacate seam left seats=2). The terminal role-flip
    (MDKR_TEST_ONLINE_RESULTS_JOINER_TERMINAL) leaves the final standings as the
    joiner would.

    ASSERTS the ceremony crowns the CAPTURED winner: champion == the remote seat
    (seat 1) with the winner's higher points, seats == 2 (the captured ranking, NOT
    a degraded 1-seat live recompute), and champLocal == 0 (the local loser is NOT
    told it won). It ALSO asserts (M4) the departed winner still shows their REAL
    identity -- the ceremony resolves the champion name (and portrait) from the
    CAPTURED char_id, so it prints the canonical "BUMPER" (slot 1 == online char 5)
    rather than the "Pn" slot fallback a live-seat lookup gives once the winning seat
    is gone. The ceremony ends via the REAL remote-vacate path -- proof the live
    snapshot truly lost the remote seat while the crown still came from the capture.
    FINISHED still fires exactly once, rc 0.

    PRE-FIX this FAILS: recomputing over the seat-absent live snapshot yields
    count=1, order[0]=the surviving local seat, so the witness would read
    champion=0 points=<loser total> seats=1 local=1 -- the wrong-winner defect; and
    even with the correct crown, PRE-M4 the champion name degrades to "P2" with no
    portrait because the winner's live seat is gone."""
    tag = "host-gone-champion"
    try:
        rc, output = run_engine(
            binary, rom, ticks=35000, timeout=900, verbose=verbose,
            extra_env={
                "MDKR_TEST_ONLINE_RESIDENT": str(CUP_ROUNDS),  # a full 4-round cup
                "MDKR_TEST_ONLINE_RESULTS_HOST_PRESS": "1",    # advance rounds fast
                # The MORE-RACES chooser is the sole RESULTS terminal now, so the
                # local (losing) host leaves the FINAL standings the way a player
                # would: navigate to FINISH (index 5) and press A -> LEAVE ->
                # CEREMONY. The champion is crowned from the ranking CAPTURED at the
                # final standings (both seats present), so it is independent of which
                # seat leaves the terminal -- the old JOINER_TERMINAL role-flip was
                # only a way to leave it, which FINISH now does directly.
                "MDKR_TEST_ONLINE_RESULTS_CHOOSER": "5",
                "MDKR_TEST_ONLINE_RESIDENT_REMOTE_WINS": "1",
                "MDKR_TEST_ONLINE_CEREMONY_REMOTE_ABSENT": "1",
                # NO CEREMONY_SKIP: let the genuine remote-vacate end the ceremony,
                # which proves the live snapshot really lost the remote seat.
            })
    except subprocess.TimeoutExpired as error:
        return fail(f"[{tag}] run timed out (the ceremony parked, or a round hung "
                    f"under the remote-wins/absent seams): {error}")
    marker = forbidden_marker(output, *FORBIDDEN_ONLINE, *FORBIDDEN_EXTRA)
    if marker:
        return fail(f"[{tag}] observed forbidden marker {marker!r}", output)
    if rc != 0:
        return fail(f"[{tag}] process exited {rc} (expected clean 0)", output)
    if not PHASE_CEREMONY_RE.search(output):
        return fail(f"[{tag}] the session never detoured into the CEREMONY phase",
                    output)
    boots = DIRECT_BOOT_RE.findall(output)
    if len(boots) != CUP_ROUNDS:
        return fail(f"[{tag}] expected {CUP_ROUNDS} direct boots (a full cup before "
                    f"the disconnect), saw {len(boots)}", output)
    # FINISHED still fires exactly once. (This is the resident-soak engine path --
    # engine-only, so there is no launcher-dispatch [online-session-end] read here;
    # the launcher read is covered by the tournament scenarios above.)
    finished = FINISHED_ENGINE_RE.findall(output)
    if len(finished) != 1:
        return fail(f"[{tag}] FINISHED must fire EXACTLY ONCE, saw {len(finished)}",
                    output)
    # The remote was GENUINELY absent at the ceremony: it ended via the real
    # remote-vacate path (the mid-ceremony detector saw no remote seat), NOT the
    # full timer. This is the proof the live snapshot lost the remote seat -- so a
    # recompute WOULD have mis-crowned, yet the crown below is still correct.
    if not CEREMONY_DONE_VACATE_RE.search(output):
        return fail(f"[{tag}] the ceremony did not end via the remote-vacate path -- "
                    f"the remote-absent seam did not make the live snapshot "
                    f"seat-absent, so the seat-gone champion path was not exercised",
                    output)
    # The FINAL standings (both present) had the REMOTE seat (slot 1) as the cup
    # leader -- the loser-local setup staged correctly.
    standings = STANDINGS_FINAL_RE.findall(output)
    if not standings:
        return fail(f"[{tag}] no FINAL standings render to confirm the winner", output)
    pts = [int(p) for p in standings[-1]]
    if not pts[1] > pts[0]:
        return fail(f"[{tag}] the remote seat (slot 1) is not the cup leader "
                    f"(points {pts}) -- RESIDENT_REMOTE_WINS did not stage the "
                    f"loser-local setup this scenario needs", output)
    champ_points = max(pts)  # == pts[1], the departed winner's total
    # THE CORE ASSERTION: the ceremony crowned the CAPTURED true winner, not the
    # surviving local loser.
    enter = CEREMONY_ENTER_LOCAL_RE.search(output)
    if not enter:
        return fail(f"[{tag}] no ceremony ENTER witness (with champLocal) to read "
                    f"the crowned champion from", output)
    seat, name, points, seats, local = enter.groups()
    if int(seat) != 1:
        return fail(f"[{tag}] ceremony crowned seat={seat}, not the remote winner "
                    f"seat 1 -- the surviving local loser was mis-crowned (the "
                    f"pre-fix live recompute)", output)
    if int(points) != champ_points:
        return fail(f"[{tag}] ceremony champion points={points} != the winner total "
                    f"{champ_points} -- it crowned the loser's own total", output)
    if int(seats) != 2:
        return fail(f"[{tag}] ceremony used a {seats}-seat ranking, not the CAPTURED "
                    f"2-seat ranking -- it recomputed from the degraded 1-seat live "
                    f"snapshot (the I-1 defect)", output)
    if int(local) != 0:
        return fail(f"[{tag}] champLocal={local} -- the local LOSER was crowned as "
                    f"[YOU] (the pre-fix survivor mis-crown)", output)
    # M4: the DEPARTED winner still shows their REAL character/name, resolved from
    # the CAPTURED char_id -- not the live seat, which lost the winner. The resident
    # rig seats the remote winner (slot 1) as Bumper (online char 5), so the ceremony
    # must show the canonical "BUMPER". PRE-M4 the champion name/portrait were re-read
    # from the (now seat-absent) live snapshot, degrading the name to the "Pn" slot
    # fallback ("P2") with no portrait; the captured-char_id resolution restores both.
    name = name.strip()
    if re.fullmatch(r"P\d+", name):
        return fail(f"[{tag}] champion name={name!r} is a 'Pn' slot fallback -- the "
                    f"disconnected winner's identity was lost (pre-M4: the live seat "
                    f"was gone, so no real char/name/portrait resolved)", output)
    if name != "BUMPER":
        return fail(f"[{tag}] champion name={name!r}, expected the departed winner's "
                    f"canonical character name 'BUMPER' (slot 1 == online char 5), "
                    f"resolved from the captured char_id -- the M4 portrait+name fix "
                    f"(a valid char also means the real portrait is blit, not blank)",
                    output)
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

    for scenario in (check_skip, check_auto, check_vacate,
                     check_champion_on_disconnect):
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
        "never a park; assets freed on every exit path; (host-gone-champion, I-1) "
        "with the WINNING remote seat genuinely absent from the ceremony's live "
        "snapshot the ceremony crowns the CAPTURED true winner (the departed "
        "remote/higher-points seat, seats=2, champLocal=0) with their REAL "
        "character name+portrait resolved from the captured char_id (canonical "
        "'BUMPER', not the 'Pn' fallback) -- NOT the surviving local loser a 1-seat "
        "live recompute would mis-crown -- and still reaches the single FINISHED via "
        "the real remote-vacate path.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
