#!/usr/bin/env python3
"""PD-T6ac KEYSTONE proof: a LIVE-loopback RESIDENT online session.

Where check_online_session_results.py proves residency via the SCRIPTED engine-
side seam (no transport, a stand-in reducer), THIS lane proves it on the REAL
two-adapter loopback feed: ONE mdkr64_engine_boot spans >= 2 races, the native
RESULTS screen fronting between races on the REAL reducer feed (party_link bridge
pumped from liveOverlayService + the launcher owning the mid-residency results
poll -> PUBLISH_RESULTS), the host's advance driving a REAL REMATCH through the
reverse feed (the reducer's race_index advances), and race N+1 re-cycled (roster +
match-input re-installed with a fresh match_epoch) and booted IN THE SAME engine
process.

It stands up the SAME loopback tournament room the MDKR_APP_TEST_ONLINE_LIVE lane
uses (MDKR_APP_TEST_ONLINE_MODE=tournament) but with MDKR_APP_TEST_ONLINE_LIVE_-
RESIDENT=N, so runOnlineLiveEngineSession installs the bridge + arms the resident
coordinator. MDKR_TEST_ONLINE_RESULTS_HOST_PRESS makes the host advance promptly
(a scripted press, not the ~25s countdown) so the lane runs fast and the "host
advance -> REMATCH" path is exercised explicitly.

Assertions (the deliverable):
  (a) EXACTLY N `[online-boot] direct race:` boots in ONE process (>= 2)
  (b) between races the native RESULTS screen fronts and shows the points/
      placements from the REAL reducer feed (haveResults=1, a valid finish order)
  (c) the host advance drives a REAL REMATCH via the reverse feed (one publish +
      one observed race_index advance per non-final race)
  (d) every RACE hand-off is through GAMEMODE_ONLINE_SESSION with gCurrentMenuId=0
      (the offline menu state machine is never entered)
  (e) no forbidden markers; clean exit 0
  (f) the flag-OFF lane (MDKR_APP_TEST_ONLINE_LIVE with no resident env) still
      boots ONCE and exits -- byte-behavior unchanged (no resident markers)
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
TICKS = 18000
RACES = 2

# GAMEMODE_ONLINE_SESSION aliases GAMEMODE_UNUSED_2 == 2 (game/src/thread3_main.h).
GAMEMODE_ONLINE_SESSION = 2
PLACE_NONE = 255

DIRECT_BOOT_RE = re.compile(
    r"^\[online-boot\] direct race: track=(\d+) players=(\d+)$", re.MULTILINE)
SESSION_RACE_RE = re.compile(
    r"^\[online-session\] phase=RACE booting after (\d+) LOBBY_WAIT tick\(s\); "
    r"isolation gGameMode=(\d+) gCurrentMenuId=(-?\d+)", re.MULTILINE)
REPORT_RE = re.compile(
    r"^\[online-resident-live\] race results reported "
    r"placements=(\d+),(\d+),(\d+),(\d+) accepted=(\d+) race_index=(\d+)$",
    re.MULTILINE)
RESUME_RE = re.compile(
    r"^\[online-session\] resume: RESULTS phase \(race (\d+) of (\d+); live "
    r"reducer RESULTS\) gGameMode=(\d+)", re.MULTILINE)
ENTER_RE = re.compile(
    r"^\[online-results\] enter: native results up race=(\d+) final=(\d+) "
    r"haveResults=(\d+) placements=(\d+),(\d+),(\d+),(\d+)", re.MULTILINE)
PUBLISH_REMATCH_RE = re.compile(
    r"^\[online-results\] publish: rematch \(host advance -> next race\)$",
    re.MULTILINE)
OBSERVED_REMATCH_RE = re.compile(
    r"^\[online-resident-live\] rematch observed race_index=(\d+) "
    r"\(reverse feed\)", re.MULTILINE)
ROUND_READY_RE = re.compile(
    r"^\[online-resident-live\] round (\d+) race-ready track=(\d+) epoch=(\d+) "
    r"frames=(\d+) \(roster re-installed\)$", re.MULTILINE)
NEXT_ARMED_RE = re.compile(
    r"^\[online-resident-live\] next race armed: epoch=(\d+)", re.MULTILINE)
POSTRACE_EXIT = "[online-postrace] session end requested"

FORBIDDEN = ("[FATAL]", "[CRASH]", "AddressSanitizer",
             "online race admission rejected",
             "launcher input provider rejected",
             "engine startup rejected before authored tick one",
             "[online-resident-live] round advance error",
             "[online-resident-live] round advance FAILED")


def fail(message: str, output: str = "") -> int:
    print(f"FAIL online resident live: {message}", file=sys.stderr)
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
    with tempfile.TemporaryDirectory(prefix="mdkr64-resident-live-") as temp:
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


def check_flag_off(binary: Path, rom: Path, verbose: bool) -> int | None:
    """(f) The flag-OFF lane (no resident env) must boot ONCE and exit -- proving
    the residency change is byte-behavior-neutral for every existing live lane."""
    try:
        rc, output = run_engine(
            binary, rom, ticks=3000, timeout=300, verbose=verbose,
            extra_env={"MDKR_APP_TEST_ONLINE_LIVE": "1"})
    except subprocess.TimeoutExpired as error:
        return fail(f"[flag-off] engine run timed out: {error}")
    for marker in FORBIDDEN:
        if marker in output:
            return fail(f"[flag-off] observed forbidden marker {marker!r}", output)
    if rc != 0:
        return fail(f"[flag-off] process exited {rc}", output)
    boots = DIRECT_BOOT_RE.findall(output)
    if len(boots) != 1:
        return fail(f"[flag-off] expected EXACTLY 1 direct boot (no residency), "
                    f"saw {len(boots)}", output)
    if "[online-resident-live]" in output:
        return fail("[flag-off] a resident-live marker fired without the resident "
                    "env (residency must be strictly opt-in)", output)
    return None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", default="build-beta")
    parser.add_argument("--rom", type=Path, default="baserom.us.v80.z64")
    parser.add_argument("--ticks", type=int, default=TICKS)
    parser.add_argument("--races", type=int, default=RACES)
    parser.add_argument("--timeout", type=int, default=400)
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    binary = Path(resolve_binary(args.build)).expanduser().resolve()
    rom = args.rom.expanduser().resolve()
    for path, label in ((binary, "binary"), (rom, "ROM")):
        if not path.is_file():
            parser.error(f"missing {label}: {path}")

    # Primary: the LIVE-loopback resident session driving N races + RESULTS in one
    # engine process on the real two-adapter feed.
    try:
        rc, output = run_engine(
            binary, rom, args.ticks, args.timeout, args.verbose,
            extra_env={
                "MDKR_APP_TEST_ONLINE_LIVE_RESIDENT": str(args.races),
                "MDKR_APP_TEST_ONLINE_MODE": "tournament",
                "MDKR_TEST_ONLINE_RESULTS_HOST_PRESS": "1",
            })
    except subprocess.TimeoutExpired as error:
        return fail(f"engine run timed out (a resident stall would look like "
                    f"this): {error}")

    for marker in FORBIDDEN:
        if marker in output:
            return fail(f"observed forbidden marker {marker!r}", output)
    if rc != 0:
        return fail(f"process exited {rc}", output)

    # (a) EXACTLY N direct boots in ONE process ------------------------------
    boots = DIRECT_BOOT_RE.findall(output)
    if len(boots) != args.races:
        return fail(f"expected EXACTLY {args.races} [online-boot] direct race "
                    f"boots in one process, saw {len(boots)}: {boots!r}", output)
    if "[online-resident-live] residency armed" not in output:
        return fail("residency was never armed (bridge/coordinator not installed)",
                    output)

    # (d) every RACE hand-off is through GAMEMODE_ONLINE_SESSION, menu id 0 ----
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
    if POSTRACE_EXIT in output:
        return fail("the resident run took the platform-exit path (it must "
                    "re-enter the session on the live feed, not exit)", output)

    # The launcher OWNED the mid-residency results poll: one PUBLISH_RESULTS per
    # race, accepted, with a valid finish order.
    reports = REPORT_RE.findall(output)
    if len(reports) != args.races:
        return fail(f"expected {args.races} mid-residency PUBLISH_RESULTS reports, "
                    f"got {len(reports)}: {reports!r}", output)
    for i, (p0, p1, p2, p3, accepted, _ri) in enumerate(reports):
        if int(accepted) != 1:
            return fail(f"race {i} results report was not accepted "
                        f"(PUBLISH_RESULTS refused)", output)
        present = sorted(int(p) for p in (p0, p1, p2, p3) if int(p) != PLACE_NONE)
        if present != [0, 1]:
            return fail(f"race {i} reported placements {[p0,p1,p2,p3]} are not a "
                        f"valid 2-racer finish order", output)

    # N resumes into RESULTS on the LIVE reducer feed (race k of N, final last) --
    resumes = RESUME_RE.findall(output)
    if len(resumes) != args.races:
        return fail(f"expected {args.races} live RESULTS resumes, got "
                    f"{len(resumes)}: {resumes!r}", output)
    for i, (race_no, total, gamemode) in enumerate(resumes):
        if int(total) != args.races or int(race_no) != i + 1:
            return fail(f"resume {i} was race {race_no} of {total} (expected "
                        f"{i + 1} of {args.races})", output)
        # M5: pin the isolation claim on the resume line itself -- the resident
        # post-race fork re-enters GAMEMODE_ONLINE_SESSION (never the offline menu).
        if int(gamemode) != GAMEMODE_ONLINE_SESSION:
            return fail(f"resume {i} had gGameMode={gamemode} (expected "
                        f"{GAMEMODE_ONLINE_SESSION})", output)

    # (b) the native RESULTS screen fronts on the REAL feed (haveResults=1, a
    # valid finish order read from the reducer snapshot), final flag correct.
    enters = ENTER_RE.findall(output)
    if len(enters) != args.races:
        return fail(f"expected {args.races} RESULTS enters, got {len(enters)}",
                    output)
    for idx, (_race_i, final, have, p0, p1, p2, p3) in enumerate(enters):
        if int(have) != 1:
            return fail(f"RESULTS enter {idx} had haveResults=0 -- the placements "
                        f"were not read from the reducer snapshot", output)
        want_final = 1 if idx == args.races - 1 else 0
        if int(final) != want_final:
            return fail(f"RESULTS enter {idx} final={final} (expected "
                        f"{want_final})", output)
        present = sorted(int(p) for p in (p0, p1, p2, p3) if int(p) != PLACE_NONE)
        if present != [0, 1]:
            return fail(f"RESULTS enter {idx} placements {[p0,p1,p2,p3]} are not a "
                        f"valid finish order from the snapshot", output)

    # (c) the host advance drives a REAL REMATCH via the reverse feed: one publish
    # + one observed race_index advance per NON-final race, and the per-round
    # re-cycle re-installed the roster + match-input on a FRESH epoch.
    publishes = PUBLISH_REMATCH_RE.findall(output)
    if len(publishes) != args.races - 1:
        return fail(f"expected {args.races - 1} REMATCH publishes (one per "
                    f"non-final race; the final holds), got {len(publishes)}",
                    output)
    observed = [int(n) for n in OBSERVED_REMATCH_RE.findall(output)]
    if observed != list(range(1, args.races)):
        return fail(f"the reducer race_index did not advance off the reverse-feed "
                    f"REMATCH (expected {list(range(1, args.races))}, got "
                    f"{observed})", output)
    rounds = ROUND_READY_RE.findall(output)
    armed = [int(e) for e in NEXT_ARMED_RE.findall(output)]
    if len(rounds) != args.races - 1 or len(armed) != args.races - 1:
        return fail(f"expected {args.races - 1} per-round re-cycles "
                    f"(round-ready {len(rounds)}, armed {len(armed)})", output)
    # Fresh epoch each round: epoch strictly advances (race 1 == 1, round k == k+1).
    epochs = [int(e) for _r, _t, e, _f in rounds]
    if epochs != list(range(2, args.races + 1)):
        return fail(f"per-round match_epoch did not advance freshly "
                    f"(expected {list(range(2, args.races + 1))}, got {epochs})",
                    output)
    # PD-T6h1: the round transition is now driven FRAME-BY-FRAME (the T6ac blocking
    # loopbackPumpUntil drive is gone). Each round-ready line reports how many
    # SERVICED frames the resumable coordinator spanned to re-cycle the room; a
    # blocking wait would report 0/1. Assert every advance spanned > 1 frame, i.e.
    # it genuinely stepped across the launcher's per-frame service path.
    span_frames = [int(f) for _r, _t, _e, f in rounds]
    if any(f <= 1 for f in span_frames):
        return fail(f"a round advance did NOT span multiple serviced frames "
                    f"(frames per round {span_frames}); the transition must be "
                    f"frame-stepped, not a blocking wait", output)

    # (f) the flag-OFF lane still boots once + exits (byte-behavior unchanged).
    off = check_flag_off(binary, rom, args.verbose)
    if off is not None:
        return off

    tracks = [int(t) for t, _p in boots]
    print(
        "PASS online resident live: ONE engine boot spanned "
        f"{len(boots)} races (tracks {tracks}) on the REAL two-adapter loopback "
        f"feed -- native RESULTS fronted each time from the reducer snapshot "
        f"(placements read from last_placements, haveResults=1), the host advance "
        f"drove a REAL REMATCH via the reverse feed (race_index -> {observed}), "
        f"each round re-cycled the roster + match-input on a fresh epoch {epochs} "
        f"FRAME-BY-FRAME (advance spanned {span_frames} serviced frames, no "
        f"blocking wait), gGameMode=2/gCurrentMenuId=0 throughout, no exit-path "
        f"taken; the flag-OFF lane still booted once + exited (residency strictly "
        f"opt-in)."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
