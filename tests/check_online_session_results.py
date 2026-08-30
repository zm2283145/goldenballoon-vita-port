#!/usr/bin/env python3
"""Prove the native online RESULTS/STANDINGS screen + the post-race RETURN into
the separated session + a scripted multi-race SOAK.

Where check_online_session_boot.py proves ONE race is reached through the
separated GAMEMODE_ONLINE_SESSION, THIS lane proves the session is RESIDENT: it
drives >= 2 engine races + the native RESULTS/STANDINGS screen in ONE engine
process via the session loop, WITHOUT the live-loopback harness (whose boot-once
wall makes multi-race impossible -- making LIVE play resident is
check_online_resident_live.py's job).

It installs a validated 2-slot roster + launch descriptor DIRECTLY (no DTLS mesh,
no match-input source), so mode_intro forks into the session and the race runs as
a plain autopilot race (both racers AI-driven, both finish; objects.c captures the
placements). With MDKR_TEST_ONLINE_RESIDENT set, the menu.c post-race hook
re-enters the session's RESULTS phase instead of exiting: the native screen
renders THIS race's placements (from mdkr_online_race_results_poll) and the cup
points (from the party_link snapshot the RESULTS test seam publishes, standing in
for the launcher reducer), the visible countdown runs (host-press advance +
auto-advance), then the session re-boots the NEXT race in the SAME process. The
final STANDINGS holds while the autoplay tick budget ends the run.

Assertions:
  * the resident roster/descriptor installed
  * >= 2 [online-boot] direct race: boots in ONE process
  * every RACE hand-off is through GAMEMODE_ONLINE_SESSION with gCurrentMenuId=0
    (the offline menu state machine is never entered)
  * exactly two RESULTS re-entries (results captured), race 1 then final race 2
  * the RESULTS witness shows the CORRECT captured placements, and the STANDINGS
    points equal the trophy-weight accrual of those placements (single + running)
  * the countdown decrements and BOTH advance paths fire (host + auto)
  * the host advancing off a non-final tournament STANDINGS publishes the
    REMATCH reverse-feed intent (never on the final race), and the scripted
    stand-in reducer observes it and advances its own cup race_index -- so the
    next race is reached via MDKR_ONLINE_REMATCH, not the old start signal
  * the resident-OFF exit path is NOT taken here (no session-end-requested), while
    the live lanes -- which leave the flag OFF -- still exit (checked by their own
    green lanes)
  * no forbidden markers; clean exit 0
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
    DIRECT_BOOT_RE, FORBIDDEN_ONLINE, GAMEMODE_ONLINE_SESSION, PLACE_NONE,
    SESSION_RACE_RE, forbidden_marker, make_fail,
)
from online_lane_util import run_engine as _run_engine

ROOT = Path(__file__).resolve().parent.parent
TICKS = 9000
RACES = 2

# Trophy weights {9,7,5,3,1,0,0,0} == gTrophyRacePointsArray / kTrophyPoints.
TROPHY = [9, 7, 5, 3, 1, 0, 0, 0]

RESIDENT_INSTALL_RE = re.compile(
    r"^\[online-resident\] soak: roster installed \(track=(\d+) slots=(\d+)\)",
    re.MULTILINE)
SESSION_BEGIN_RE = re.compile(r"^\[online-session\] begin:", re.MULTILINE)
RESUME_RE = re.compile(
    r"^\[online-session\] resume: RESULTS phase \(race (\d+) of (\d+); "
    r"results captured\)", re.MULTILINE)
RESULTS_ENTER_RE = re.compile(
    r"^\[online-results\] enter: native results up race=(\d+) final=(\d+) "
    r"haveResults=(\d+) placements=(\d+),(\d+),(\d+),(\d+)", re.MULTILINE)
RESULTS_RENDER_RE = re.compile(
    r"^\[online-results\] render stage=(\w+) mode=(\d+) race=(\d+) host=(\d+) "
    r"placements=(\d+),(\d+),(\d+),(\d+) points=(\d+),(\d+),(\d+),(\d+) "
    r"secs=(\d+) final=(\d+)$", re.MULTILINE)
ADVANCE_RE = re.compile(
    r"^\[online-results\] advance: (results -> standings|screen done) \((\w+)\)$",
    re.MULTILINE)
# PD-T6b: the host publishes the REMATCH reverse-feed intent on a non-final
# tournament STANDINGS advance, and the scripted stand-in reducer observes it and
# advances its own cup race_index (proving the screen moves to the next race via
# MDKR_ONLINE_REMATCH, not the old start_requested signal).
PUBLISH_REMATCH_RE = re.compile(
    r"^\[online-results\] publish: rematch \(host advance -> next race\)$",
    re.MULTILINE)
REDUCE_REMATCH_RE = re.compile(
    r"^\[online-results\] test-reducer: rematch observed -> race_index=(\d+)$",
    re.MULTILINE)
POSTRACE_EXIT = "[online-postrace] session end requested"


fail = make_fail("session results")


def trophy(place: int) -> int:
    return TROPHY[place] if 0 <= place < len(TROPHY) else 0


def run_engine(binary: Path, rom: Path, ticks: int, timeout: int, verbose: bool,
               extra_env: dict[str, str] | None = None) -> tuple[int, str]:
    return _run_engine(binary, rom, ticks=ticks, timeout=timeout,
                       verbose=verbose, extra_env=extra_env,
                       prefix="mdkr64-online-results-")


def check_m3_interlude(binary: Path, rom: Path, verbose: bool) -> int | None:
    """A real tournament->single interlude that FAILS if the mode-change stash-clear
    is reverted. The LOBBY_WAIT seam publishes mode=TOURNAMENT + cup 1 (whose
    round-0 track is 13) for the first half of the hold -- so the session stashes
    intended track 13 -- then flips to mode=SINGLE. The stash-clear must clear that
    stale stash on the mode change, so the boot logs 'track honored: 5' (the SINGLE
    manifest); without the clear it logs 'track divergence: snapshot=13 manifest=5'.
    Verified out-of-band: reverting the stash-clear makes exactly this scenario
    diverge."""
    scn = "m3-interlude"
    try:
        rc, output = run_engine(
            binary, rom, ticks=4000, timeout=200, verbose=verbose,
            extra_env={
                "MDKR_TEST_ONLINE_RESIDENT": "1",
                "MDKR_TEST_ONLINE_SESSION_SCRIPT": "8",
                "MDKR_TEST_ONLINE_SESSION_MODE_INTERLUDE": "1",
            })
    except subprocess.TimeoutExpired as error:
        return fail(f"[{scn}] engine run timed out: {error}")
    marker = forbidden_marker(output, *FORBIDDEN_ONLINE)
    if marker:
        return fail(f"[{scn}] observed forbidden marker {marker!r}", output)
    if rc != 0:
        return fail(f"[{scn}] process exited {rc}", output)
    if "[online-boot] track divergence" in output:
        return fail(f"[{scn}] a track divergence was logged -- the M1 sticky-stash "
                    f"clear did not fire on the tournament->single interlude",
                    output)
    honored = re.findall(r"^\[online-boot\] track honored: (\d+)$", output,
                         re.MULTILINE)
    if not honored or int(honored[0]) != 5:
        return fail(f"[{scn}] expected the first boot to log 'track honored: 5' "
                    f"after the mode interlude, got {honored!r}", output)
    return None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", default="build-beta")
    parser.add_argument("--rom", type=Path, default="baserom.us.v80.z64")
    parser.add_argument("--ticks", type=int, default=TICKS)
    parser.add_argument("--races", type=int, default=RACES)
    parser.add_argument("--timeout", type=int, default=600)
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    binary = Path(resolve_binary(args.build)).expanduser().resolve()
    rom = args.rom.expanduser().resolve()
    for path, label in ((binary, "binary"), (rom, "ROM")):
        if not path.is_file():
            parser.error(f"missing {label}: {path}")

    # Primary soak: resident post-race re-entry driving N races + RESULTS in one
    # engine process via the session loop.
    try:
        rc, output = run_engine(
            binary, rom, args.ticks, args.timeout, args.verbose,
            extra_env={"MDKR_TEST_ONLINE_RESIDENT": str(args.races)})
    except subprocess.TimeoutExpired as error:
        return fail(f"engine run timed out (a resident stall would look like "
                    f"this): {error}")

    marker = forbidden_marker(output, *FORBIDDEN_ONLINE)
    if marker:
        return fail(f"observed forbidden marker {marker!r}", output)
    if rc != 0:
        return fail(f"process exited {rc}", output)

    # --- Resident install + separated boot ----------------------------------
    install = RESIDENT_INSTALL_RE.search(output)
    if not install:
        return fail("the resident soak roster was never installed", output)
    if int(install.group(1)) != 5 or int(install.group(2)) != 2:
        return fail(f"resident roster mismatch track={install.group(1)} "
                    f"slots={install.group(2)} (expected track 5, 2 slots)",
                    output)
    if not SESSION_BEGIN_RE.search(output):
        return fail("the online SESSION mode was never entered", output)

    # --- >= 2 engine races in ONE process, all isolated ----------------------
    boots = DIRECT_BOOT_RE.findall(output)
    if len(boots) < args.races:
        return fail(f"expected >= {args.races} [online-boot] direct race boots in "
                    f"one process, saw {len(boots)}", output)
    if len(boots) != args.races:
        return fail(f"expected EXACTLY {args.races} boots (the last STANDINGS must "
                    f"NOT re-boot), saw {len(boots)}", output)
    if any(int(track) != 5 for track, _players in boots):
        return fail(f"a race booted an unexpected track: {boots!r}", output)

    # M1 (PD-T4 carry-forward): re-booting across the resident loop -- while the
    # RESULTS test seam publishes a TOURNAMENT forward feed -- must never leave a
    # stale intended-track stash that logs a spurious divergence. Every boot must
    # log the honored path on the manifest track, and NO divergence line at all.
    if "[online-boot] track divergence" in output:
        return fail("a spurious [online-boot] track divergence was logged across "
                    "the resident re-boots (M1 stash not cleared)", output)
    honored = re.findall(r"^\[online-boot\] track honored: (\d+)$", output,
                         re.MULTILINE)
    if len(honored) != args.races or any(int(t) != 5 for t in honored):
        return fail(f"expected {args.races} 'track honored: 5' boots, got "
                    f"{honored!r}", output)

    handoffs = SESSION_RACE_RE.findall(output)
    if len(handoffs) != args.races:
        return fail(f"expected {args.races} RACE hand-offs, got {len(handoffs)}",
                    output)
    for _t, gamemode, menu_id in handoffs:
        if int(gamemode) != GAMEMODE_ONLINE_SESSION:
            return fail(f"a hand-off had gGameMode={gamemode} (expected "
                        f"{GAMEMODE_ONLINE_SESSION})", output)
        if int(menu_id) != 0:
            return fail(f"gCurrentMenuId={menu_id} at a hand-off -- the offline "
                        f"menu was entered on the online path (expected 0)",
                        output)
    if "input-script" in output or "race_2p_split" in output:
        return fail("a menu-nav input script was loaded", output)
    if "load_menu_with_level_background" in output:
        return fail("the offline menu was loaded on the resident path", output)

    # --- Exactly N RESULTS re-entries (results captured) ---------------------
    resumes = RESUME_RE.findall(output)
    if len(resumes) != args.races:
        return fail(f"expected {args.races} RESULTS re-entries, got "
                    f"{len(resumes)}: {resumes!r}", output)
    for i, (race_no, total) in enumerate(resumes):
        if int(total) != args.races or int(race_no) != i + 1:
            return fail(f"resume {i} was race {race_no} of {total} (expected "
                        f"{i + 1} of {args.races})", output)
    # The resident path RE-ENTERED both times; the exit path must NOT have fired.
    if POSTRACE_EXIT in output:
        return fail("the resident run took the platform-exit path (it must "
                    "re-enter the session, not exit)", output)

    # --- The RESULTS screen showed the captured placements + accrued points --
    enters = RESULTS_ENTER_RE.findall(output)
    if len(enters) != args.races:
        return fail(f"expected {args.races} RESULTS enters, got {len(enters)}",
                    output)
    race_placements: list[list[int]] = []
    for idx, (race_i, final, have, p0, p1, p2, p3) in enumerate(enters):
        if int(have) != 1:
            return fail(f"RESULTS enter {idx} had haveResults=0 (the captured "
                        f"finish order was not polled in)", output)
        want_final = 1 if idx == args.races - 1 else 0
        if int(final) != want_final:
            return fail(f"RESULTS enter {idx} final={final} (expected "
                        f"{want_final})", output)
        placements = [int(p0), int(p1), int(p2), int(p3)]
        present = sorted(p for p in placements if p != PLACE_NONE)
        if present != [0, 1]:
            return fail(f"RESULTS enter {idx} placements {placements} are not a "
                        f"valid 2-racer finishing order (want a 0 and a 1)",
                        output)
        race_placements.append(placements)

    # Expected running points per slot from the captured placements.
    expected = [0, 0, 0, 0]
    per_race_expected: list[list[int]] = []
    for placements in race_placements:
        for slot in range(4):
            expected[slot] += trophy(placements[slot])
        per_race_expected.append(list(expected))

    renders = RESULTS_RENDER_RE.findall(output)
    if not renders:
        return fail("no RESULTS render witnesses", output)
    # Every render must be tournament mode (mode=1) so the standings stage runs.
    if any(int(r[1]) != 1 for r in renders):
        return fail("a RESULTS render was not tournament mode", output)

    # For each race, a STANDINGS render must show points == the accrued total.
    for race_i in range(args.races):
        want = per_race_expected[race_i]
        standings = [r for r in renders
                     if r[0] == "standings" and int(r[2]) == race_i]
        if not standings:
            return fail(f"no STANDINGS render for race index {race_i}", output)
        matched = [r for r in standings
                   if [int(r[8]), int(r[9]), int(r[10]), int(r[11])] == want]
        if not matched:
            got = [[int(r[8]), int(r[9]), int(r[10]), int(r[11])]
                   for r in standings]
            return fail(f"race {race_i} STANDINGS points never equalled the "
                        f"trophy-weight accrual {want} of the captured "
                        f"placements (saw {got})", output)

    # --- Countdown + BOTH advance paths --------------------------------------
    standings_secs = sorted({int(r[12]) for r in renders if r[0] == "standings"})
    if len(standings_secs) < 3 or min(standings_secs) > 1:
        return fail(f"the STANDINGS countdown did not visibly decrement to ~0 "
                    f"(saw secs {standings_secs})", output)
    advances = ADVANCE_RE.findall(output)
    kinds = {who for _stage, who in advances}
    if "host" not in kinds:
        return fail("no host-press advance was witnessed (R-D host authority)",
                    output)
    if "auto" not in kinds:
        return fail("no auto-advance (countdown-to-zero) was witnessed", output)

    # --- PD-T6b: the next race is driven by the REMATCH reverse feed ----------
    # The host advancing off a NON-final tournament STANDINGS publishes the
    # rematch intent (one per non-final race; the FINAL standings holds and never
    # publishes), and the scripted stand-in reducer observes each and advances its
    # own cup race_index 1..N-1 -- so the STANDINGS race indices the earlier
    # assertions matched came from OBSERVED rematch, not the boot count.
    rematch_publishes = PUBLISH_REMATCH_RE.findall(output)
    if len(rematch_publishes) != args.races - 1:
        return fail(f"expected {args.races - 1} REMATCH publishes (one per "
                    f"non-final tournament STANDINGS advance; the final race must "
                    f"NOT publish), got {len(rematch_publishes)}", output)
    observed = [int(n) for n in REDUCE_REMATCH_RE.findall(output)]
    if observed != list(range(1, args.races)):
        return fail(f"the stand-in reducer did not advance race_index off the "
                    f"observed REMATCH intents (expected "
                    f"{list(range(1, args.races))}, got {observed})", output)

    # M-3: a NON-vacuous M1 regression guard -- a real tournament->single
    # interlude that diverges if the sticky-stash clear is reverted.
    m3 = check_m3_interlude(binary, rom, args.verbose)
    if m3 is not None:
        return m3

    print(
        "PASS online session results: RESIDENT soak drove "
        f"{len(boots)} engine races + RESULTS/STANDINGS in ONE process via the "
        f"session loop (gGameMode=2 gCurrentMenuId=0 throughout, offline menu "
        f"never entered) -- captured placements "
        f"{race_placements}, points accrued to {expected} by trophy weight, "
        f"host + auto advance both fired, countdown decremented to 0, final "
        f"standings held; the next race was driven by the REMATCH reverse feed "
        f"({len(rematch_publishes)} publish(es), stand-in reducer advanced "
        f"race_index {observed}); no exit-path taken, clean exit 0; M-3 "
        f"mode-interlude boot honored track 5 (no divergence -- M1 clear proven)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
