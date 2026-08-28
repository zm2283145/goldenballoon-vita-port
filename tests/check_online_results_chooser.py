#!/usr/bin/env python3
"""Prove the native online "MORE RACES" chooser on the RESULTS screen (T4).

Where check_online_session_results.py proves the RESIDENT RESULTS/STANDINGS soak
with the OLD binary continue/leave terminal, THIS lane proves the T4 replacement:
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
LEAVE path (a finished tournament detours to the champion CEREMONY -> FINISHED).
RACE AGAIN re-races the same config (>= 2 direct boots). The joiner mirror never
publishes REMATCH (watch-only) and, when the host's choice drives the room out of
RESULTS, follows into re-selection (-> CHARSELECT).
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
JOINER_DEPART_RE = re.compile(
    r"^\[online-results\] test-reducer: joiner-mirror feed departed", re.MULTILINE)
SESSION_ROUTE_RE = re.compile(
    r"^\[online-session\] results -> (.+?) \(chooser: (.+?)\)", re.MULTILINE)
PHASE_CEREMONY_RE = re.compile(
    r"^\[online-session\] phase=CEREMONY: final standings", re.MULTILINE)
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


def check_joiner(binary, rom, verbose) -> int | None:
    """The joiner mirror: display-only (never publishes REMATCH), renders the
    "more races" mirror + "waiting for host", and follows the host's authoritative
    choice once the room leaves RESULTS -> re-selection (CHARSELECT)."""
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
    if not JOINER_FOLLOW_RE.search(output):
        return fail(f"[{tag}] the joiner never followed the host's choice out of "
                    f"RESULTS", output)
    route = [m for m in SESSION_ROUTE_RE.finditer(output)
             if m.group(2) == "joiner follow"]
    if not route or route[0].group(1) != "charselect":
        return fail(f"[{tag}] the joiner did not route RESULTS -> charselect on "
                    f"the follow", output)
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

    for scenario in (check_race_again, check_finish, check_joiner):
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
        "FINISH -> champion CEREMONY -> single FINISHED; the joiner is display-only "
        "(mirror + waiting-for-host, never publishes REMATCH) and follows the host "
        "out of RESULTS -> CHARSELECT; gGameMode=2 gCurrentMenuId=0 throughout")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
