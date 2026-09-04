#!/usr/bin/env python3
"""Prove the native online "MORE RACES" chooser on the RESULTS screen.

Where check_online_session_results.py proves the RESIDENT RESULTS/STANDINGS soak
with the OLD binary continue/leave terminal, THIS lane proves the replacement:
after a race the HOST gets the full retail replay set, each option maps to the
EXISTING party_link reverse-feed intents (REMATCH + SET_MODE -- no new reducer
command), and the session routes back to the right native screen. The joiner is
display-only (it renders the mirror + "waiting for host" and follows the host's
authoritative choice once the room leaves RESULTS).

It rides the SAME scripted RESIDENT soak the results lane uses (in-process, no DTLS
mesh) but arms the dedicated chooser seam (MDKR_TEST_ONLINE_RESULTS_CHOOSER), which
forces the chooser ON (real play gets it too; every OTHER online lane keeps the
historical terminal, gated OFF -- so the 18 sibling lanes stay byte-behaviour-
unchanged). Each scenario scripts the host navigating to a specific option and
pressing A (or, for "joiner", renders the display-only mirror):

  tournament room (mode=1): CHANGE CUP / CHANGE MODE / NEW TOURNAMENT /
                            CHANGE CHARACTER / FINISH
  single room ("single:"):  RACE AGAIN / CHANGE TRACK

Per option it asserts the committed reverse-feed intent (rematch + the SET_MODE
value) AND the session's RESULTS -> native-screen routing witness. FINISH keeps the
LEAVE path (a finished tournament detours to the champion CEREMONY -> FINISHED; at
a GENUINE feed-final -- last cup round -- the host first commits the REMATCH wrap so
the finish is reducer-observable, covered by check_online_lobby_tournament.py; this
lane's stand-in feed sits at round 1, pinning the direct-LEAVE arm). RACE AGAIN
re-races the same config (>= 2 direct boots). The joiner mirror never publishes
REMATCH (watch-only); at the tournament FINAL it exits to the joiner's OWN champion
CEREMONY -> FINISHED once the room leaves RESULTS (the ruled two-real-peer end --
never a re-selection wait on a host that may no longer be in-session).

SINGLE-RACE FINISH (the strand-class extension): a SINGLE-race room's FINISH is
reducer-observable too -- the host commits the SAME REMATCH wrap (phase-only in
single-race: lobby_core.c returns the room to LOBBY and clears placements/votes),
LEAVEs only after the room left RESULTS, and ends via the race-winner CEREMONY ->
FINISHED ("single-finish"; the two-endpoint loopback proof is
check_online_single_race_replay.py's finish scenario). The SINGLE-race joiner
mirror KEEPS THE FOLLOW exit on the observed wrap ("single-joiner"): unlike the
tournament final -- where the cup is complete whichever option the host picked --
a single-race wrap is reducer-INDISTINGUISHABLE between FINISH and RACE AGAIN,
and ceremonying on every wrap would tear the joiner's session down on every
replay; the follow lands the joiner in CHARSELECT of the freshly wrapped room,
which is exactly where a FINISHing host's automatic FINISHED re-take arrives too.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

from harness_utils import resolve_binary
from online_lane_util import (
    DIRECT_BOOT_RE, FINISHED_ENGINE_RE, FORBIDDEN_ONLINE,
    GAMEMODE_ONLINE_SESSION, SESSION_RACE_RE, forbidden_marker, make_fail,
    run_engine,
)

ROOT = Path(__file__).resolve().parent.parent

# party_link intent "no SET_MODE wanted" sentinel (MDKR_PARTY_LINK_MODE_UNSET) and
# the two real mode values, mirrored from party_link.h for the intent assertions.
MODE_UNSET = 255
MODE_SINGLE = 0
MODE_TOURNAMENT = 1

# MdkrOnlineResultsChoice values (online_results.h).
CH_RACE_AGAIN = 1
CH_CHANGE_TRACK = 2
CH_CHANGE_CUP = 3
CH_CHANGE_MODE = 4
CH_NEW_TOURNAMENT = 5
CH_CHANGE_CHAR = 6

CHOOSER_FRONTED_RE = re.compile(
    r"^\[online-results\] chooser: fronted \((\S+) mode=(\d+) host=(\d+) "
    r"joiner=(\d+)\)$", re.MULTILINE)
CHOOSER_RENDER_RE = re.compile(
    r"^\[online-results\] chooser render mode=(\d+) host=(\d+) joiner=(\d+) "
    r"cursor=(\d+) option=(.+?) committed=(\d+) choice=(\d+)$", re.MULTILINE)
CHOOSER_COMMIT_RE = re.compile(
    r"^\[online-results\] chooser: committed option=(.+?) choice=(\d+) "
    r"intent\{rematch=1 mode=(\d+)\}$", re.MULTILINE)
CHOOSER_FINISH_RE = re.compile(
    r"^\[online-results\] chooser: committed option=FINISH -> LEAVE$",
    re.MULTILINE)
CHOOSER_ADVANCE_RE = re.compile(
    r"^\[online-results\] chooser: room left RESULTS -> ADVANCE \(choice=(\d+)\)$",
    re.MULTILINE)
JOINER_FOLLOW_RE = re.compile(
    r"^\[online-results\] chooser: joiner follows host authoritative choice",
    re.MULTILINE)
# The ruled tournament-FINAL mirror exit: the cup is COMPLETE, so when the room
# leaves RESULTS (the host's FINISH/replay wrap -- the joiner cannot and need not
# distinguish which), the mirror exits to the joiner's OWN champion CEREMONY ->
# FINISHED instead of re-selection. The pre-fix mirror followed into CHARSELECT
# here -- which, when the host had FINISHed (left the session but stayed seated
# for the re-take), left the joiner waiting in a selection screen for a host that
# was no longer in-session.
JOINER_WRAP_LEAVE_RE = re.compile(
    r"^\[online-results\] chooser: joiner mirror observed the final wrap "
    r"-> LEAVE \(ceremony\)$", re.MULTILINE)
JOINER_DEPART_RE = re.compile(
    r"^\[online-results\] test-reducer: joiner-mirror feed departed", re.MULTILINE)
# The removed blind bailout: the joiner mirror ending its own session on a 10s
# dwell (or a stray press) while the host is merely deliberating.
JOINER_DWELL_LEAVE_RE = re.compile(
    r"^\[online-results\] chooser: joiner terminal \((?:press|dwell)\) -> LEAVE$",
    re.MULTILINE)
# Positive control: the mirror survived past the old dwell threshold, host present.
JOINER_HELD_RE = re.compile(
    r"^\[online-results\] chooser: joiner mirror still up past dwell "
    r"\(units=(\d+)\)$", re.MULTILINE)
# The stand-in host seat leaving, and the mirror's own vanished-host exit.
JOINER_SEAT_VACATED_RE = re.compile(
    r"^\[online-results\] test-reducer: joiner-mirror host seat vacated",
    re.MULTILINE)
JOINER_VACATE_LEAVE_RE = re.compile(
    r"^\[online-results\] chooser: joiner mirror host vacated -> LEAVE$",
    re.MULTILINE)
SESSION_ROUTE_RE = re.compile(
    r"^\[online-session\] results -> (.+?) \(chooser: (.+?)\)", re.MULTILINE)
PHASE_CEREMONY_RE = re.compile(
    r"^\[online-session\] phase=CEREMONY: final standings", re.MULTILINE)
# The SINGLE-RACE FINISH wrap: the same commit + converged-leave pair the
# tournament-final wrap emits, plus the mode-truthful session witnesses.
WRAP_COMMIT_RE = re.compile(
    r"^\[online-results\] chooser: committed option=FINISH choice=7 "
    r"intent\{rematch=1 mode=255\}$", re.MULTILINE)
WRAP_CONVERGED_RE = re.compile(
    r"^\[online-results\] chooser: FINISH wrap converged \(room left RESULTS\) "
    r"-> LEAVE \(ceremony\)$", re.MULTILINE)
REDUCER_REMATCH_RE = re.compile(
    r"^\[online-results\] test-reducer: rematch observed", re.MULTILINE)
CEREMONY_SINGLE_RE = re.compile(
    r"^\[online-session\] phase=CEREMONY: single-race FINISH", re.MULTILINE)
FINISHED_SINGLE_RE = re.compile(
    r"^\[online-session\] FINISHED: single-race FINISH", re.MULTILINE)
JOINER_GONE_RE = re.compile(
    r"^\[online-results\] test-reducer: HOST-chooser joiner seat vacated",
    re.MULTILINE)
POSTRACE_EXIT = "[online-postrace] session end requested"


fail = make_fail("results chooser")


def run(binary, rom, chooser_val, ticks, timeout, verbose):
    return run_engine(
        binary, rom, ticks=ticks, timeout=timeout, verbose=verbose,
        extra_env={"MDKR_TEST_ONLINE_RESIDENT": "1",
                   "MDKR_TEST_ONLINE_RESULTS_CHOOSER": chooser_val},
        prefix="mdkr64-online-chooser-")


def _isolation_ok(tag: str, output: str) -> int | None:
    """Every scenario: no forbidden marker, and the offline menu state machine was
    never entered on the online path (gGameMode=2, gCurrentMenuId=0 at each RACE)."""
    marker = forbidden_marker(output, *FORBIDDEN_ONLINE)
    if marker:
        return fail(f"[{tag}] observed forbidden marker {marker!r}", output)
    races = SESSION_RACE_RE.findall(output)
    if not races:
        return fail(f"[{tag}] no RACE hand-off (the soak never booted a race)",
                    output)
    for _t, gamemode, menu_id in races:
        if int(gamemode) != GAMEMODE_ONLINE_SESSION or int(menu_id) != 0:
            return fail(f"[{tag}] isolation broke at a hand-off: gGameMode="
                        f"{gamemode} gCurrentMenuId={menu_id} (expected "
                        f"{GAMEMODE_ONLINE_SESSION}/0)", output)
    return None


def check_option(binary, rom, verbose, *, tag, chooser_val, want_option,
                 want_choice, want_mode, want_route_screen, want_route_name,
                 want_front_mode, ticks):
    """A host option scenario: the chooser fronts, the host commits `want_option`
    (publishing rematch + `want_mode`), and the session routes to
    `want_route_screen` (`want_route_name`)."""
    try:
        rc, output = run(binary, rom, chooser_val, ticks, 400, verbose)
    except subprocess.TimeoutExpired as error:
        return fail(f"[{tag}] engine run timed out: {error}")
    if rc != 0:
        return fail(f"[{tag}] process exited {rc}", output)
    guard = _isolation_ok(tag, output)
    if guard is not None:
        return guard

    front = CHOOSER_FRONTED_RE.search(output)
    if not front:
        return fail(f"[{tag}] the chooser never fronted at the terminal", output)
    if int(front.group(2)) != want_front_mode:
        return fail(f"[{tag}] chooser fronted mode={front.group(2)}, expected "
                    f"{want_front_mode}", output)
    if int(front.group(3)) != 1 or int(front.group(4)) != 0:
        return fail(f"[{tag}] chooser fronted host={front.group(3)} "
                    f"joiner={front.group(4)}, expected host=1 joiner=0", output)

    # The host committed the intended option -> the intended reverse-feed intent.
    commit = None
    for m in CHOOSER_COMMIT_RE.finditer(output):
        if m.group(1) == want_option:
            commit = m
            break
    if not commit:
        got = [m.group(1) for m in CHOOSER_COMMIT_RE.finditer(output)]
        return fail(f"[{tag}] the host never committed option {want_option!r} "
                    f"(committed: {got})", output)
    if int(commit.group(2)) != want_choice:
        return fail(f"[{tag}] option {want_option!r} committed choice="
                    f"{commit.group(2)}, expected {want_choice}", output)
    if int(commit.group(3)) != want_mode:
        return fail(f"[{tag}] option {want_option!r} published SET_MODE intent "
                    f"mode={commit.group(3)}, expected {want_mode} "
                    f"(255==no SET_MODE, 0==single, 1==tournament)", output)

    # REMATCH converged (room left RESULTS) -> ADVANCE with the committed choice.
    adv = CHOOSER_ADVANCE_RE.search(output)
    if not adv or int(adv.group(1)) != want_choice:
        return fail(f"[{tag}] the chooser did not ADVANCE on choice={want_choice} "
                    f"after the room left RESULTS (REMATCH convergence): "
                    f"{adv.group(0) if adv else None}", output)

    # The session routed back to the right native screen.
    route = None
    for m in SESSION_ROUTE_RE.finditer(output):
        if m.group(2) == want_route_name:
            route = m
            break
    if not route:
        got = [(m.group(1), m.group(2)) for m in SESSION_ROUTE_RE.finditer(output)]
        return fail(f"[{tag}] the session never routed RESULTS -> "
                    f"{want_route_screen} (chooser: {want_route_name}); saw {got}",
                    output)
    if route.group(1) != want_route_screen:
        return fail(f"[{tag}] chooser {want_route_name!r} routed to "
                    f"{route.group(1)!r}, expected {want_route_screen!r}", output)
    return None


def check_race_again(binary, rom, verbose) -> int | None:
    """RACE AGAIN (single): commit -> REMATCH -> re-race the SAME config, in
    process. Proves >= 2 direct boots (the second is the re-race)."""
    tag = "race-again"
    try:
        rc, output = run(binary, rom, "single:0", 12000, 500, verbose)
    except subprocess.TimeoutExpired as error:
        return fail(f"[{tag}] engine run timed out: {error}")
    if rc != 0:
        return fail(f"[{tag}] process exited {rc}", output)
    guard = _isolation_ok(tag, output)
    if guard is not None:
        return guard
    commit = None
    for m in CHOOSER_COMMIT_RE.finditer(output):
        if m.group(1) == "RACE AGAIN":
            commit = m
            break
    if not commit or int(commit.group(2)) != CH_RACE_AGAIN or \
            int(commit.group(3)) != MODE_UNSET:
        return fail(f"[{tag}] RACE AGAIN did not commit rematch with no SET_MODE "
                    f"(saw {commit.group(0) if commit else None})", output)
    route = [m for m in SESSION_ROUTE_RE.finditer(output)
             if m.group(2) == "race again"]
    if not route or route[0].group(1) != "re-race same config":
        return fail(f"[{tag}] the session did not route RESULTS -> re-race same "
                    f"config", output)
    boots = DIRECT_BOOT_RE.findall(output)
    if len(boots) < 2:
        return fail(f"[{tag}] RACE AGAIN did not re-race the same config in "
                    f"process (expected >= 2 direct boots, saw {len(boots)}: "
                    f"{boots})", output)
    return None


def check_finish_joiner_gone(binary, rom, verbose) -> int | None:
    """FINISH with the JOINER VANISHED mid-chooser (B2 scenario 4, HOST side): the
    ruled design is that the host's chooser has NO vacate detector -- a host may
    deliberate freely -- so a joiner dropping while the host is on the chooser must
    NOT interrupt the host: it still commits FINISH, the leader-only wrap converges
    with one seat, and the champion CEREMONY (from the ranking latched while both
    were present) reaches the single FINISHED. This asserts that designed behavior
    (the host FINISHes ALONE, not stranded/crashed)."""
    tag = "finish-joiner-gone"
    try:
        rc, output = run_engine(
            binary, rom, ticks=10000, timeout=500, verbose=verbose,
            extra_env={"MDKR_TEST_ONLINE_RESIDENT": "1",
                       "MDKR_TEST_ONLINE_RESULTS_CHOOSER": "5",
                       "MDKR_TEST_ONLINE_RESULTS_JOINER_GONE": "1"},
            prefix="mdkr64-online-chooser-")
    except subprocess.TimeoutExpired as error:
        return fail(f"[{tag}] engine run timed out (the host parked when the joiner "
                    f"vanished mid-chooser?): {error}")
    if rc != 0:
        return fail(f"[{tag}] process exited {rc}", output)
    guard = _isolation_ok(tag, output)
    if guard is not None:
        return guard
    # Non-vacuous: the joiner really did vanish while the host was on the chooser.
    if not JOINER_GONE_RE.search(output):
        return fail(f"[{tag}] the joiner never vacated mid-chooser (the seam did "
                    f"not fire) -- the arm would be vacuous", output)
    # Ruled behavior: the host FINISHes ALONE -> CEREMONY -> exactly one FINISHED.
    if not CHOOSER_FINISH_RE.search(output):
        return fail(f"[{tag}] with the joiner gone, the host FINISH did not take "
                    f"the LEAVE path (it was interrupted/stranded)", output)
    if not PHASE_CEREMONY_RE.search(output):
        return fail(f"[{tag}] the host did not reach the champion CEREMONY alone",
                    output)
    if len(FINISHED_ENGINE_RE.findall(output)) != 1:
        return fail(f"[{tag}] FINISHED did not fire exactly once (host alone)",
                    output)
    return None


def check_finish(binary, rom, verbose) -> int | None:
    """FINISH (tournament final): keeps the LEAVE path -> the champion CEREMONY ->
    the single FINISHED handshake (unchanged)."""
    tag = "finish"
    try:
        rc, output = run(binary, rom, "5", 10000, 500, verbose)
    except subprocess.TimeoutExpired as error:
        return fail(f"[{tag}] engine run timed out: {error}")
    if rc != 0:
        return fail(f"[{tag}] process exited {rc}", output)
    guard = _isolation_ok(tag, output)
    if guard is not None:
        return guard
    if not CHOOSER_FINISH_RE.search(output):
        return fail(f"[{tag}] FINISH did not take the LEAVE path", output)
    if not PHASE_CEREMONY_RE.search(output):
        return fail(f"[{tag}] FINISH on the tournament final did not detour into "
                    f"the champion CEREMONY", output)
    if len(FINISHED_ENGINE_RE.findall(output)) != 1:
        return fail(f"[{tag}] FINISHED did not fire exactly once after the "
                    f"ceremony", output)
    # FINISH must NOT route to a selection screen (it ends the session).
    for m in SESSION_ROUTE_RE.finditer(output):
        return fail(f"[{tag}] FINISH wrongly routed RESULTS -> {m.group(1)} "
                    f"(chooser: {m.group(2)}) instead of finishing", output)
    return None


def check_single_finish(binary, rom, verbose) -> int | None:
    """FINISH in a SINGLE-race room: reducer-observable. The host commits
    the REMATCH wrap (rematch=1, no SET_MODE -- phase-only in single-race), the
    stand-in reducer observes it and departs RESULTS, and only THEN does the
    chooser LEAVE -- into the race-winner CEREMONY -> exactly one FINISHED. The
    pre-fix behavior was the purely-local direct leave (no reducer command): a
    second real peer's mirror could never observe the finish and the room never
    returned to SELECTING. RED at that build: no wrap commit/convergence lines."""
    tag = "single-finish"
    try:
        rc, output = run(binary, rom, "single:5", 10000, 500, verbose)
    except subprocess.TimeoutExpired as error:
        return fail(f"[{tag}] engine run timed out: {error}")
    if rc != 0:
        return fail(f"[{tag}] process exited {rc}", output)
    guard = _isolation_ok(tag, output)
    if guard is not None:
        return guard
    front = CHOOSER_FRONTED_RE.search(output)
    if not front or int(front.group(2)) != MODE_SINGLE:
        return fail(f"[{tag}] the chooser did not front in SINGLE mode "
                    f"({front.group(0) if front else None})", output)
    if not WRAP_COMMIT_RE.search(output):
        return fail(f"[{tag}] the single-race FINISH did not commit the REMATCH "
                    f"wrap (intent{{rematch=1 mode=255}}) -- the finish is not "
                    f"reducer-observable", output)
    if CHOOSER_FINISH_RE.search(output):
        return fail(f"[{tag}] the single-race FINISH took the OLD purely-local "
                    f"direct leave (no reducer transition)", output)
    if not REDUCER_REMATCH_RE.search(output):
        return fail(f"[{tag}] the stand-in reducer never observed the FINISH "
                    f"wrap's REMATCH", output)
    if not WRAP_CONVERGED_RE.search(output):
        return fail(f"[{tag}] the FINISH wrap never converged (the chooser left "
                    f"before the room departed RESULTS)", output)
    if not CEREMONY_SINGLE_RE.search(output):
        return fail(f"[{tag}] the single-race FINISH did not detour into the "
                    f"race-winner CEREMONY", output)
    if len(FINISHED_SINGLE_RE.findall(output)) != 1:
        return fail(f"[{tag}] FINISHED did not fire exactly once after the "
                    f"single-race ceremony", output)
    # FINISH must NOT route to a selection screen (it ends the session).
    for m in SESSION_ROUTE_RE.finditer(output):
        return fail(f"[{tag}] FINISH wrongly routed RESULTS -> {m.group(1)} "
                    f"(chooser: {m.group(2)}) instead of finishing", output)
    return None


def check_single_joiner(binary, rom, verbose) -> int | None:
    """The SINGLE-race joiner mirror KEEPS THE FOLLOW exit on the observed wrap
    (the ruled single-race mirror semantics). A single-race wrap is reducer-
    INDISTINGUISHABLE between the host's FINISH and RACE AGAIN (both are the same
    phase-only REMATCH), so the mirror must NOT ceremony/FINISH on it -- that
    would tear the joiner's session down on every replay. It follows into
    CHARSELECT of the freshly wrapped room, which is where a FINISHing host's
    automatic FINISHED re-take lands too (the two peers re-converge either way).
    Display-only throughout: the mirror never publishes a commit."""
    tag = "single-joiner"
    try:
        rc, output = run(binary, rom, "single:joiner", 12000, 500, verbose)
    except subprocess.TimeoutExpired as error:
        return fail(f"[{tag}] engine run timed out: {error}")
    if rc != 0:
        return fail(f"[{tag}] process exited {rc}", output)
    guard = _isolation_ok(tag, output)
    if guard is not None:
        return guard
    front = CHOOSER_FRONTED_RE.search(output)
    if not front or int(front.group(2)) != MODE_SINGLE or \
            int(front.group(4)) != 1:
        return fail(f"[{tag}] the chooser did not front as the SINGLE-mode joiner "
                    f"mirror ({front.group(0) if front else None})", output)
    if CHOOSER_COMMIT_RE.search(output):
        return fail(f"[{tag}] a joiner published a chooser commit (it must be "
                    f"watch-only)", output)
    if not JOINER_DEPART_RE.search(output):
        return fail(f"[{tag}] the host's authoritative REMATCH stand-in never "
                    f"departed the feed", output)
    if not JOINER_FOLLOW_RE.search(output):
        return fail(f"[{tag}] the SINGLE-race mirror did not FOLLOW the observed "
                    f"wrap into re-selection (the ruled single-race mirror exit)",
                    output)
    if JOINER_WRAP_LEAVE_RE.search(output):
        return fail(f"[{tag}] the SINGLE-race mirror took the tournament-final "
                    f"ceremony exit -- it cannot distinguish FINISH from RACE "
                    f"AGAIN, so ceremonying here would end the joiner's session "
                    f"on every replay", output)
    route = [m for m in SESSION_ROUTE_RE.finditer(output)
             if m.group(2) == "joiner follow"]
    if not route or route[0].group(1) != "charselect":
        return fail(f"[{tag}] the session did not route the joiner follow to "
                    f"CHARSELECT", output)
    if PHASE_CEREMONY_RE.search(output) or CEREMONY_SINGLE_RE.search(output):
        return fail(f"[{tag}] the mirror's follow wrongly detoured into a "
                    f"ceremony", output)
    if FINISHED_ENGINE_RE.search(output) or FINISHED_SINGLE_RE.search(output):
        return fail(f"[{tag}] the joiner's session FINISHED on a wrap it cannot "
                    f"attribute to a host FINISH", output)
    return None


def check_joiner(binary, rom, verbose) -> int | None:
    """The joiner mirror at the tournament FINAL: display-only (never publishes
    REMATCH), renders the "more races" mirror + "waiting for host", and -- the cup
    being COMPLETE -- exits to the joiner's OWN champion CEREMONY -> FINISHED once
    the room leaves RESULTS (the host's authoritative wrap). It must NOT follow
    into re-selection: when the host FINISHed (left the session but stayed seated
    for the re-take) a re-selecting joiner waits on a host that is no longer
    in-session. RED at the pre-fix build: the mirror followed the wrap into
    CHARSELECT ("joiner follows host authoritative choice -> ADVANCE")."""
    tag = "joiner"
    try:
        rc, output = run(binary, rom, "joiner", 12000, 500, verbose)
    except subprocess.TimeoutExpired as error:
        return fail(f"[{tag}] engine run timed out: {error}")
    if rc != 0:
        return fail(f"[{tag}] process exited {rc}", output)
    guard = _isolation_ok(tag, output)
    if guard is not None:
        return guard
    mirror = [m for m in CHOOSER_RENDER_RE.finditer(output)
              if int(m.group(3)) == 1]
    if not mirror:
        return fail(f"[{tag}] the joiner never rendered the display-only mirror "
                    f"(joiner=1)", output)
    # Display-only: a joiner NEVER commits / publishes REMATCH.
    if CHOOSER_COMMIT_RE.search(output):
        return fail(f"[{tag}] a joiner published a chooser commit (it must be "
                    f"watch-only)", output)
    if not JOINER_DEPART_RE.search(output):
        return fail(f"[{tag}] the host's authoritative REMATCH stand-in never "
                    f"departed the feed", output)
    if not JOINER_WRAP_LEAVE_RE.search(output):
        return fail(f"[{tag}] the joiner mirror did not exit on the observed final "
                    f"wrap (room left RESULTS) -> its own ceremony; a tournament-"
                    f"final mirror must end the joiner's session, not re-select",
                    output)
    if JOINER_FOLLOW_RE.search(output):
        return fail(f"[{tag}] the FINAL mirror still took the joiner-follow "
                    f"re-selection path (the pre-fix behavior that waits on a host "
                    f"no longer in-session)", output)
    for m in SESSION_ROUTE_RE.finditer(output):
        return fail(f"[{tag}] the FINAL mirror wrongly routed RESULTS -> "
                    f"{m.group(1)} (chooser: {m.group(2)}) instead of ending the "
                    f"joiner's session", output)
    if not PHASE_CEREMONY_RE.search(output):
        return fail(f"[{tag}] the joiner's session did not detour into its own "
                    f"champion CEREMONY after the final wrap", output)
    if len(FINISHED_ENGINE_RE.findall(output)) != 1:
        return fail(f"[{tag}] FINISHED did not fire exactly once after the "
                    f"joiner's ceremony", output)
    return None


def check_joiner_hold(binary, rom, verbose) -> int | None:
    """The joiner mirror must NOT bail on a blind countdown while the host is only
    deliberating. Hold the stand-in feed in RESULTS (host present) well past the old
    600-unit (10s) dwell and require the mirror to stay up: no joiner-terminal
    (dwell/press) LEAVE, while proving it actually crossed that dwell point."""
    tag = "joiner-hold"
    try:
        rc, output = run(binary, rom, "joiner-hold", 12000, 500, verbose)
    except subprocess.TimeoutExpired as error:
        return fail(f"[{tag}] engine run timed out: {error}")
    if rc != 0:
        return fail(f"[{tag}] process exited {rc}", output)
    guard = _isolation_ok(tag, output)
    if guard is not None:
        return guard
    mirror = [m for m in CHOOSER_RENDER_RE.finditer(output)
              if int(m.group(3)) == 1]
    if not mirror:
        return fail(f"[{tag}] the joiner never rendered the display-only mirror",
                    output)
    if JOINER_DWELL_LEAVE_RE.search(output):
        return fail(f"[{tag}] the joiner mirror still bailed on the blind terminal "
                    f"dwell/press while the host was present -- the 10s countdown "
                    f"must no longer end the joiner's session", output)
    # Assert on REAL behavior, not just the absence of the tombstoned log line: the
    # host is present throughout, so the mirror must not have LEFT via ANY path --
    # no champion CEREMONY, no FINISHED handshake, no platform session-end request.
    if PHASE_CEREMONY_RE.search(output):
        return fail(f"[{tag}] the mirror ended into the champion CEREMONY while the "
                    f"host was still present (a premature leave by some path)",
                    output)
    if FINISHED_ENGINE_RE.search(output):
        return fail(f"[{tag}] the session FINISHED while the host was still present "
                    f"-- the mirror must keep waiting, not end", output)
    if POSTRACE_EXIT in output:
        return fail(f"[{tag}] a platform session-end was requested while the host "
                    f"was still present", output)
    held = JOINER_HELD_RE.search(output)
    if not held:
        return fail(f"[{tag}] the mirror never crossed the old dwell threshold, so "
                    f"the hold scenario did not exercise the fix (positive control)",
                    output)
    return None


def check_joiner_vacate(binary, rom, verbose) -> int | None:
    """Negative control: a genuinely vanished host (the sole remote seat leaves while
    the feed stays in RESULTS) MUST still end the joiner's session. The MORE-RACES
    chooser fronts at the tournament FINAL standings, where the online_session
    RESULTS remote-vacate detector is gated off (!resultsIsFinal), so the mirror
    carries its own debounced vanished-host exit."""
    tag = "joiner-vacate"
    try:
        rc, output = run(binary, rom, "joiner-vacate", 12000, 500, verbose)
    except subprocess.TimeoutExpired as error:
        return fail(f"[{tag}] engine run timed out (the vanished-host exit never "
                    f"fired -- a reintroduced hang?): {error}")
    if rc != 0:
        return fail(f"[{tag}] process exited {rc}", output)
    guard = _isolation_ok(tag, output)
    if guard is not None:
        return guard
    if not JOINER_SEAT_VACATED_RE.search(output):
        return fail(f"[{tag}] the host seat never vacated -- the control did not "
                    f"stage a vanished host", output)
    if not JOINER_VACATE_LEAVE_RE.search(output):
        return fail(f"[{tag}] the joiner mirror did not end the session on the "
                    f"vanished host (a reintroduced hang without the old dwell)",
                    output)
    return None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", default="build-beta")
    parser.add_argument("--rom", type=Path, default="baserom.us.v80.z64")
    parser.add_argument("--timeout", type=int, default=500)
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    binary = Path(resolve_binary(args.build)).expanduser().resolve()
    rom = args.rom.expanduser().resolve()
    for path, label in ((binary, "binary"), (rom, "ROM")):
        if not path.is_file():
            parser.error(f"missing {label}: {path}")

    # Host option scenarios: (tag, seam, option, choice, SET_MODE, route screen,
    # route name, fronted mode, ticks). Single-race rooms use the "single:" prefix.
    options = (
        ("change-track", "single:1", "CHANGE TRACK", CH_CHANGE_TRACK, MODE_UNSET,
         "trackselect", "change track", MODE_SINGLE, 8000),
        ("change-cup", "1", "CHANGE CUP", CH_CHANGE_CUP, MODE_UNSET,
         "trackselect", "change cup", MODE_TOURNAMENT, 8000),
        ("change-mode", "2", "CHANGE MODE", CH_CHANGE_MODE, MODE_SINGLE,
         "trackselect", "change mode", MODE_TOURNAMENT, 8000),
        ("new-tournament", "3", "NEW TOURNAMENT", CH_NEW_TOURNAMENT,
         MODE_TOURNAMENT, "trackselect", "new tournament", MODE_TOURNAMENT, 8000),
        ("change-char", "4", "CHANGE CHARACTER", CH_CHANGE_CHAR, MODE_UNSET,
         "charselect", "change character", MODE_TOURNAMENT, 8000),
    )
    for (tag, seam, option, choice, mode, screen, name, front_mode,
         ticks) in options:
        err = check_option(binary, rom, args.verbose, tag=tag, chooser_val=seam,
                           want_option=option, want_choice=choice, want_mode=mode,
                           want_route_screen=screen, want_route_name=name,
                           want_front_mode=front_mode, ticks=ticks)
        if err is not None:
            return err

    for scenario in (check_race_again, check_finish, check_finish_joiner_gone,
                     check_single_finish, check_single_joiner,
                     check_joiner, check_joiner_hold, check_joiner_vacate):
        err = scenario(binary, rom, args.verbose)
        if err is not None:
            return err

    print(
        "PASS online results chooser: the native MORE RACES chooser fronts at the "
        "session decision point (single RESULTS / tournament FINAL standings) and "
        "the HOST's option maps to the existing reverse-feed intents + routes the "
        "session -- CHANGE TRACK/CUP -> TRACKSELECT (rematch, no SET_MODE), CHANGE "
        "MODE -> TRACKSELECT (rematch + SET_MODE single), NEW TOURNAMENT -> "
        "TRACKSELECT (rematch + SET_MODE tournament, cup resets), CHANGE CHARACTER "
        "-> CHARSELECT, RACE AGAIN -> re-race same config in process (>=2 boots), "
        "FINISH -> champion CEREMONY -> single FINISHED; the SINGLE-race FINISH is "
        "reducer-observable (REMATCH wrap committed + converged before the leave) "
        "and ends via the race-winner CEREMONY -> FINISHED; the joiner is display-"
        "only (mirror + waiting-for-host, never publishes REMATCH), exits the "
        "tournament-FINAL mirror to its OWN ceremony -> FINISHED on the observed "
        "wrap, KEEPS THE FOLLOW on a single-race wrap (FINISH and RACE AGAIN are "
        "reducer-indistinguishable there; the re-take re-converges the peers in "
        "CHARSELECT), keeps waiting while the host deliberates (no blind dwell), "
        "and still ends on a vanished host; gGameMode=2 gCurrentMenuId=0 "
        "throughout")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
