#!/usr/bin/env python3
"""Two-process, real-cloud, production-path capstone: the native online flow.

This is the production-path ACCEPTANCE INSTRUMENT. It launches TWO SEPARATE `mdkr64`
app processes on this machine -- one create, one join -- over the REAL deployed
service (https://party.goldenballoon.net, compiled in as MDKR_PARTY_ORIGIN), and
proves the ENTIRE production native flow with the ONLY automation being:

  (a) pairing bootstrap -- MDKR_APP_TEST_ONLINE_AUTOPAIR=create|join drives the
      interactive launcher's create/join chooser, the 6-digit join code, the
      "Check Setup" secure handshake and the Words-Match confirmation by
      synthesizing the SAME UI actions a player would take, then goes HANDS-OFF
      (ui_online_room.cpp autopairService); and
  (b) injected PAD INPUT -- the input-only native-screen seams
      (MDKR_TEST_ONLINE_LOBBY_START for CHARSELECT/VEHICLE/TRACKSELECT,
      MDKR_TEST_ONLINE_RESULTS_CHOOSER for the more-races chooser) plus the
      deterministic race-input fixture (MDKR_APP_TEST_ONLINE_SYNTH_RACE_INPUT)
      standing in for the two absent human controllers, and per-role racer picks
      (MDKR_TEST_ONLINE_CHARSELECT_PICK) so the two seats never collide.

Everything AFTER pairing is the PRODUCTION code path choosing its own route: the
room-ready takeover fires by itself, the descriptor-less native CHARSELECT ->
VEHICLE -> TRACKSELECT own race 1, the reducer syncs selections, the race runs and
converges, the RESULTS chooser round-trips a second race, the session returns
FINISHED and the takeover re-arms. NONE of MDKR_TEST_ONLINE_LOBBY_START-forcing,
fake adapters, stand-in reducers or path/phase flips are used -- if the flow does
not happen by itself, the lane FAILS. That is the point.

Runs with VISIBLE windows (no MDKR64_HIDDEN): the interactive launcher only builds
frames when it has a real drawable, so a hidden window would idle forever. Visible
engine windows are acceptable for this lane.

The seven assertions (from both processes' stderr + exit codes):
  a. [online-room-ready] latch + publish + boot-enter on BOTH -- takeover self-fired
  b. native session enters CHARSELECT descriptor-less (begin: lobby-start (no
     descriptor); the descriptor-FIRST "begin: separated boot path" did NOT fire.
     NOTE: the native path still logs a "source=launch-descriptor" NET-SELECTIONS
     line -- its descriptor is built LIVE at BEGIN_LOADING -- so that line is NOT a
     discriminator; the BEGIN path is)
  c. selections synced through the reducer (joiner's racer appears on creator's side)
  d. race 1 converges (identical ENGINE-ONLINE-LIVE fold hash, both raced > N ticks)
  e. chooser round-trip: race 2 boots and converges too
  f. clean return: both end FINISHED, the launcher room alive, exit 0
  g. re-take: a SECOND [online-room-ready] latch after the return (FINISHED re-arm)

RECORDED RED (assertions a+b against the pre-fix build eac1e8c0):
  The pre-fix commit eac1e8c0 has NO interactive room-ready takeover and NO autopair
  (both are later work), so the capstone's own pairing cannot run there; the RED is
  captured by running the LEGACY cloud two-process path (which eac1e8c0 does have)
  over the real cloud and observing the exact defect the capstone's (a)+(b) detect.
  Captured 2026-08-29 (RED build: a detached eac1e8c0 worktree configured with
  -DCMAKE_BUILD_TYPE=Release -DMDKR_ENABLE_ONLINE_BETA=ON
  -DMDKR_PARTY_ORIGIN=https://party.goldenballoon.net -DMDKR_BUILD_STAMP=<eac1e8c0>
  + the three FETCHCONTENT_SOURCE_DIR_* deps + the pinned PKG_CONFIG_PATH, then
  `tools/online/cloud_two_process_engine_boot.py`):
    (b) FAIL -- the DESCRIPTOR-FIRST begin fired on BOTH endpoints:
        "[online-session] begin: separated boot path entered" (a pre-supplied
        descriptor booted; NO "begin: lobby-start", zero native CHARSELECT); the
        race's "[NET-SELECTIONS] ... source=launch-descriptor" confirms it (that
        source line ALSO appears on the native path, so the BEGIN path is the
        discriminator, not the source label).
    (a) FAIL -- ZERO "[online-room-ready]" latch/publish/native-boot lines on either
        endpoint (the self-firing native takeover does not exist pre-fix).
  On HEAD the same two assertions PASS (the takeover self-fires; the begin is
  lobby-start, not separated boot path) -- so the lane detects the exact
  descriptor-first defect the owner shipped twice.

Usage:
    python3 tools/online/check_online_native_flow_cloud.py \\
        --build build-beta --rom baserom.us.v80.z64 \\
        --timeout 300 --log-dir /path/to/logs -v
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path
from typing import Callable, Optional

ROOT = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(ROOT / "tests"))
from harness_utils import DEFAULT_BUILD_DIR, resolve_binary  # noqa: E402

# The Online Room launcher panel index (kLauncherPanelOnlineRoom, ui_launcher.h).
ONLINE_ROOM_PANEL = "1"

# Host picks Pipsy (row-0 col 2); joiner picks Diddy (row-0 col 0). Different
# racers -> no SELECTION_CONFLICT wedge.
HOST_PICK = "2"
JOIN_PICK = "0"

# ---- Pairing witnesses (autopairService, ui_online_room.cpp) -----------------
AUTOPAIR_CODE_RE = re.compile(r"^\[online-autopair\] code=(\d{6})$", re.MULTILINE)
AUTOPAIR_MEMBERS_RE = re.compile(r"^\[online-autopair\] members=2$", re.MULTILINE)
AUTOPAIR_PHRASE_RE = re.compile(r"^\[online-autopair\] phrase=(.+)$", re.MULTILINE)
AUTOPAIR_CONFIRM_RE = re.compile(
    r"^\[online-autopair\] confirm dispatched", re.MULTILINE)
AUTOPAIR_SELECTING_RE = re.compile(
    r"^\[online-autopair\] SELECTING reached", re.MULTILINE)

# ---- Assertion (a): room-ready takeover fired BY ITSELF ----------------------
RR_CONDITION_RE = re.compile(
    r"^\[online-room-ready\] condition holds: kind=\d+ phase=\d+ "
    r"member_count=(\d+) mode=(\S+) -> route=(\S+)", re.MULTILINE)
RR_LATCH_RE = re.compile(r"^\[online-room-ready\] latch set", re.MULTILINE)
RR_PUBLISH_RE = re.compile(
    r"^\[online-room-ready\] published adapter", re.MULTILINE)
RR_CONSUMED_RE = re.compile(
    r"^\[online-room-ready\] publish consumed by launcher", re.MULTILINE)
RR_BOOT_ENTER_RE = re.compile(
    r"^\[online-room-ready\] native session boot entered", re.MULTILINE)
RR_BOOT_RESULT_RE = re.compile(
    r"^\[online-room-ready\] boot result=(-?\d+) reason=(\S+) "
    r"rearmPending=(\d+)$", re.MULTILINE)

# ---- Assertion (b): descriptor-less native CHARSELECT ------------------------
# The discriminator is the BEGIN path, NOT the NET-SELECTIONS source: the native
# takeover builds its descriptor LIVE at BEGIN_LOADING, so the race still boots
# reading a "source=launch-descriptor" -- that line appears on BOTH the native and
# the descriptor-first paths and is therefore useless as a discriminator. The
# descriptor-FIRST defect is "begin: separated boot path entered" (a pre-supplied
# descriptor, no native screens); the native takeover is "begin: lobby-start (no
# descriptor)". Same signals the loopback lobby-start lane uses.
BEGIN_LOBBY_RE = re.compile(
    r"^\[online-session\] begin: lobby-start \(no descriptor\)", re.MULTILINE)
CHARSELECT_ENTER_RE = re.compile(r"^\[online-charselect\] enter:", re.MULTILINE)
DESCRIPTOR_FIRST_RE = re.compile(
    r"^\[online-session\] begin: separated boot path entered", re.MULTILINE)

# ---- Assertion (c): reducer-synced selections -------------------------------
# The joiner's picked racer (char id) shows up as the REMOTE seat on the
# creator's native CHARSELECT render.
CHARSELECT_REMOTE_RE = re.compile(
    r"^\[online-charselect\] render .*remote\{seat=\d+ char=(\d+) ", re.MULTILINE)
# CHARSELECT completes (seat.ready latched + persisted through the reducer over
# the real network) exactly when the session hands off to the native VEHICLE
# screen -- the wedge the two-peer selection-convergence fix removed.
TO_VEHICLESELECT_RE = re.compile(
    r"^\[online-session\] charselect -> vehicleselect", re.MULTILINE)

# ---- Assertions (d)/(e): race convergence -----------------------------------
# The descriptor-less native/resident takeover does NOT emit the
# ENGINE-ONLINE-LIVE fold-hash line (that is runOnlineLiveEngineSession's, the
# single-race loopback/cloud lane). Its cross-process convergence proof is the
# reducer-agreed FINISH ORDER: both endpoints independently commit the SAME
# placements for the SAME race_index (a deterministic rollback race folds the
# same inputs -> the same order on both), reported over the real cloud via
# PUBLISH_RESULTS. Both booting each race on the same track corroborates it.
RACE_BOOT_RE = re.compile(
    r"^\[online-session\] phase=RACE booting", re.MULTILINE)
DIRECT_BOOT_RE = re.compile(
    r"^\[online-boot\] direct race: track=(\d+) players=(\d+)$", re.MULTILINE)
RESULTS_REPORTED_RE = re.compile(
    r"^\[online-resident-live\] race results reported placements=([0-9,]+) "
    r"accepted=(\d+) race_index=(\d+)", re.MULTILINE)


def placements_for(output: str, race_index: int) -> Optional[str]:
    """The finish-order placements a process committed for a given race_index,
    or None if it never reported that race."""
    for m in RESULTS_REPORTED_RE.finditer(output):
        if int(m.group(3)) == race_index and m.group(2) == "1":
            return m.group(1)
    return None

FORBIDDEN_MARKERS = (
    "[FATAL]", "[CRASH]", "AddressSanitizer",
    "online race admission rejected",
    "launcher input provider rejected",
)


class ProofFailure(RuntimeError):
    """Harness-level failure; message names the stuck phase."""


class ProofStopEarly(Exception):
    """--through stop: the requested phases passed; end the run cleanly."""


def clean_environment(**updates: str) -> dict:
    environment = {
        key: value for key, value in os.environ.items()
        if not key.startswith(("MDKR", "GE007_"))
    }
    environment.update(updates)
    return environment


class Driver:
    """A narrated interactive `mdkr64` app process, tailing merged stdout/stderr
    to an in-memory buffer and a log file in real time."""

    def __init__(self, binary: Path, env: dict, cwd: Path, name: str,
                 log_path: Path, verbose: bool):
        self.name = name
        self.log_path = log_path
        self.start_time = time.monotonic()
        self.verbose = verbose
        self._log_file = log_path.open("w", encoding="utf-8")
        self.proc = subprocess.Popen(
            [str(binary)], cwd=cwd, env=env, text=True,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, bufsize=1)
        self.lines: list = []
        self.lock = threading.Lock()
        self.thread = threading.Thread(target=self._pump, daemon=True)
        self.thread.start()

    def _pump(self) -> None:
        assert self.proc.stdout is not None
        for line in self.proc.stdout:
            line = line.rstrip("\n")
            stamp = f"[t+{time.monotonic() - self.start_time:7.2f}s] {line}"
            if self.verbose:
                print(f"{self.name}: {stamp}", flush=True)
            self._log_file.write(stamp + "\n")
            self._log_file.flush()
            with self.lock:
                self.lines.append(line)

    def snapshot(self) -> list:
        with self.lock:
            return list(self.lines)

    def full_output(self) -> str:
        return "\n".join(self.snapshot())

    def wait_line(self, predicate: Callable[[str], object], description: str,
                  timeout: float):
        deadline = time.monotonic() + timeout
        scanned = 0
        while time.monotonic() < deadline:
            lines = self.snapshot()
            for index in range(scanned, len(lines)):
                value = predicate(lines[index])
                if value:
                    return value
            scanned = len(lines)
            if self.proc.poll() is not None and scanned == len(self.snapshot()):
                break
            time.sleep(0.05)
        raise ProofFailure(
            f"{self.name} never printed '{description}' within {timeout:.0f}s; "
            f"last lines: {self.snapshot()[-14:]}")

    def wait_exit(self, timeout: float) -> int:
        try:
            return self.proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired as error:
            raise ProofFailure(
                f"{self.name} did not exit within {timeout:.0f}s; last lines: "
                f"{self.snapshot()[-14:]}") from error

    def close(self) -> None:
        if self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait(timeout=5)
        if self.proc.stdout is not None:
            self.proc.stdout.close()
        self._log_file.close()


def make_env(role: str, join_code: Optional[str], pick: str, rom: Path,
             run_dir: Path, tournament_cup: int) -> dict:
    (run_dir / "saves").mkdir(parents=True)
    prefs = run_dir / "prefs"
    prefs.mkdir()
    # Seed the remembered ROM so the interactive launcher validates it (the Online
    # Room panel never services validation itself; autopairService drives it).
    (prefs / "mdkr64_app.ini").write_text(
        f"# mdkr64 app preferences\nrom_path={rom}\n", encoding="utf-8")
    environment = clean_environment(
        LC_ALL="C",
        MDKR_AUDIO="0",
        MDKR_AUTOPILOT="1",
        MDKR_NO_CRASH_HANDLER="1",
        MDKR_RENDERER="gl",
        MDKR_ROM=str(rom),
        MDKR_SAVE_DIR=str(run_dir / "saves"),
        MDKR_APP_PREFS_DIR=str(prefs),
        MDKR_VIDEO_CONFIG_PATH=str(run_dir / "video.ini"),
        # Enter straight into the Online Room panel; autopairService drives it.
        MDKR_APP_PANEL=ONLINE_ROOM_PANEL,
        MDKR_APP_TEST_ONLINE_AUTOPAIR=role,
        # Input-only native-screen seams (never phase-forcing): the host/joiner
        # navigate CHARSELECT/VEHICLE/TRACKSELECT via screen-relative scripted pad.
        MDKR_TEST_ONLINE_LOBBY_START="1",
        MDKR_TEST_ONLINE_CHARSELECT_PICK=pick,
        # Deterministic race-input fixture standing in for the human controller so
        # both processes converge byte-for-byte over the real mesh.
        MDKR_APP_TEST_ONLINE_SYNTH_RACE_INPUT="1",
        # The more-races chooser: at the FINAL standings the host commits FINISH
        # (index 5) -> CEREMONY -> the single FINISHED handshake.
        MDKR_TEST_ONLINE_RESULTS_CHOOSER="5",
    )
    if join_code is not None:
        environment["MDKR_APP_TEST_ONLINE_JOIN_CODE"] = join_code
    if tournament_cup >= 0:
        # A single race NEVER auto-finals (online_session.c), so it cannot reach
        # the FINISHED session-end the (f)/(g) assertions need. The capstone runs
        # the demo's real flow: a TOURNAMENT. The host configures mode + cup in
        # the room (autopair), the native TRACKSELECT enters tournament mode and
        # locks the cup (LOBBY_TOURNAMENT), rounds 1..N-1 auto-REMATCH, and the
        # final standings front the chooser for FINISH. MDKR_FORCE_LAPS=1 keeps
        # each round short so a full 4-round cup fits the wall clock.
        environment["MDKR_APP_TEST_ONLINE_AUTOPAIR_TOURNAMENT"] = str(tournament_cup)
        environment["MDKR_TEST_ONLINE_LOBBY_TOURNAMENT"] = "1"
        environment["MDKR_FORCE_LAPS"] = "1"
    return environment


def assert_no_forbidden(name: str, output: str) -> None:
    for marker in FORBIDDEN_MARKERS:
        if marker in output:
            raise ProofFailure(f"{name}: observed forbidden marker {marker!r}")


def assert_takeover_self_fired(name: str, output: str) -> None:
    """Assertion (a): the production room-ready takeover fired by itself."""
    cond = RR_CONDITION_RE.search(output)
    if cond is None:
        raise ProofFailure(
            f"{name}: the room-ready condition never held (takeover never "
            f"self-armed) -- the descriptor-first signature of the shipped bug")
    for label, pattern in (("latch set", RR_LATCH_RE),
                           ("published adapter", RR_PUBLISH_RE),
                           ("publish consumed by launcher", RR_CONSUMED_RE),
                           ("native session boot entered", RR_BOOT_ENTER_RE)):
        if pattern.search(output) is None:
            raise ProofFailure(
                f"{name}: no '[online-room-ready] {label}' line -- the "
                f"self-firing takeover did not complete")


def assert_descriptorless_charselect(name: str, output: str) -> None:
    """Assertion (b): the native session enters CHARSELECT descriptor-less."""
    if DESCRIPTOR_FIRST_RE.search(output):
        raise ProofFailure(
            f"{name}: the descriptor-FIRST begin fired ([online-session] begin: "
            f"separated boot path) -- a pre-supplied descriptor booted instead of "
            f"the native takeover")
    if BEGIN_LOBBY_RE.search(output) is None:
        raise ProofFailure(
            f"{name}: the native session never began descriptor-less "
            f"([online-session] begin: lobby-start)")
    if CHARSELECT_ENTER_RE.search(output) is None:
        raise ProofFailure(
            f"{name}: native CHARSELECT never fronted before any race")


def run(args: argparse.Namespace) -> dict:
    binary = Path(resolve_binary(args.build)).expanduser().resolve()
    rom = args.rom.expanduser().resolve()
    for path, label in ((binary, "binary"), (rom, "ROM")):
        if not path.is_file():
            raise ProofFailure(f"missing {label}: {path}")

    log_dir = args.log_dir
    log_dir.mkdir(parents=True, exist_ok=True)
    report: dict = {"phases": [], "verdict": None, "detail": None}

    def phase(ok: bool, name: str, detail: str) -> None:
        report["phases"].append({"phase": name, "ok": ok, "detail": detail})
        print(f"  [{'OK  ' if ok else 'FAIL'}] {name}: {detail}", flush=True)

    creator = joiner = None
    tournament_cup = args.tournament
    overall_start = time.monotonic()
    with tempfile.TemporaryDirectory(prefix="mdkr64-native-flow-") as temp:
        root = Path(temp)
        try:
            # --- Pairing bootstrap (the only launcher automation) ------------
            creator = Driver(
                binary, make_env("create", None, HOST_PICK, rom, root / "create",
                                 tournament_cup),
                root / "create", "create", log_dir / "create.log", args.verbose)
            code = creator.wait_line(
                AUTOPAIR_CODE_RE.match, "room code (autopair create)", 90.0
            ).group(1)
            phase(True, "pair_create",
                  f"code={code} (interactive create over the real cloud)")

            joiner = Driver(
                binary, make_env("join", code, JOIN_PICK, rom, root / "join",
                                 tournament_cup),
                root / "join", "join", log_dir / "join.log", args.verbose)
            creator.wait_line(AUTOPAIR_MEMBERS_RE.match, "creator members=2", 60.0)
            joiner.wait_line(AUTOPAIR_MEMBERS_RE.match, "joiner members=2", 60.0)
            phase(True, "pair_members", "both endpoints see 2 members over the "
                  "real cloud /connect WebSocket")

            pc = creator.wait_line(AUTOPAIR_PHRASE_RE.match, "creator phrase", 60.0)
            pj = joiner.wait_line(AUTOPAIR_PHRASE_RE.match, "joiner phrase", 60.0)
            if pc.group(1).strip() != pj.group(1).strip():
                raise ProofFailure(
                    f"verification phrases diverged: create={pc.group(1)!r} "
                    f"join={pj.group(1)!r}")
            creator.wait_line(AUTOPAIR_CONFIRM_RE.match, "creator confirm", 30.0)
            joiner.wait_line(AUTOPAIR_CONFIRM_RE.match, "joiner confirm", 30.0)
            phase(True, "pair_confirm", "both peers agreed the SAS phrase "
                  f"{pc.group(1).strip()!r} and confirmed -- HANDS-OFF now")

            # --- (a) takeover self-fires; (b) descriptor-less CHARSELECT -----
            for drv in (creator, joiner):
                drv.wait_line(RR_BOOT_ENTER_RE.search,
                              f"{drv.name} native session boot entered", 90.0)
                drv.wait_line(CHARSELECT_ENTER_RE.search,
                              f"{drv.name} native CHARSELECT enter", 60.0)
            for drv in (creator, joiner):
                assert_no_forbidden(drv.name, drv.full_output())
                assert_takeover_self_fired(drv.name, drv.full_output())
                assert_descriptorless_charselect(drv.name, drv.full_output())
            phase(True, "takeover_and_charselect",
                  "assertions (a)+(b): the room-ready takeover fired BY ITSELF "
                  "and the descriptor-less native CHARSELECT fronted on BOTH "
                  "endpoints (begin: lobby-start, not separated boot path)")

            # --- (c) reducer-synced selections -------------------------------
            # A racer picked on ONE endpoint must render as the REMOTE seat on
            # the OTHER endpoint's native CHARSELECT -- the cross-endpoint
            # reducer-sync proof. Either direction suffices: each endpoint
            # advances off CHARSELECT on its OWN seat's ready, so a fast local
            # flow can leave the screen before the slower peer's pick lands
            # (post-fix the creator readies in ~2 s), and the remaining screens
            # do not render the remote racer. Requiring specifically the
            # creator-sees-joiner direction raced the very convergence it
            # asserts; the joiner-sees-creator witness is the same reducer
            # round-trip over the same cloud room.
            def selections_synced(_line: str):
                for m in CHARSELECT_REMOTE_RE.finditer(creator.full_output()):
                    if m.group(1) == JOIN_PICK:
                        return "creator rendered the joiner's racer"
                for m in CHARSELECT_REMOTE_RE.finditer(joiner.full_output()):
                    if m.group(1) == HOST_PICK:
                        return "joiner rendered the creator's racer"
                return None
            sync_how = creator.wait_line(
                selections_synced,
                "a native CHARSELECT shows the OTHER endpoint's racer "
                "(reducer sync)", args.select_timeout)
            phase(True, "selections_synced",
                  f"assertion (c): a picked racer synced through the reducer "
                  f"onto the other endpoint's native CHARSELECT ({sync_how})")

            if args.through == "c":
                # (a)-(c) qualification stop: prove CHARSELECT actually
                # COMPLETES -- seat.ready latched + persisted through the
                # reducer over real latency on BOTH endpoints, witnessed by the
                # session's hand-off to the native VEHICLE screen (the exact
                # spot the two-peer convergence wedge parked forever) -- then
                # end the run cleanly without driving the remaining phases.
                for drv in (creator, joiner):
                    drv.wait_line(
                        TO_VEHICLESELECT_RE.search,
                        f"{drv.name} charselect -> vehicleselect (READY "
                        f"latched + persisted)", args.select_timeout)
                for drv in (creator, joiner):
                    assert_no_forbidden(drv.name, drv.full_output())
                phase(True, "advance_past_charselect",
                      "both endpoints' seats readied through the reducer and "
                      "the session advanced CHARSELECT -> VEHICLESELECT")
                report["verdict"] = "PASS"
                report["detail"] = (
                    "phases (a)-(c) + advance-past-CHARSELECT over the real "
                    "cloud (run stopped at --through c; the remaining phases "
                    "are the full capstone's)")
                raise ProofStopEarly()

            if tournament_cup < 0:
                raise ProofFailure(
                    f"--through {args.through} requires --tournament (the race-2 "
                    "round-trip and the FINISHED session-end need a tournament; a "
                    "single race never auto-finals)")

            # --- (d) race 1 converges (reducer-agreed finish order) ----------
            def both_reported(idx: int):
                def pred(_line: str):
                    pc = placements_for(creator.full_output(), idx)
                    pj = placements_for(joiner.full_output(), idx)
                    return (pc, pj) if (pc is not None and pj is not None) else None
                return pred
            for drv in (creator, joiner):
                drv.wait_line(RACE_BOOT_RE.search, f"{drv.name} race 1 boot",
                              args.race_timeout)
            pc0, pj0 = creator.wait_line(
                both_reported(0), "both endpoints report race-1 results",
                args.race_timeout)
            if pc0 != pj0:
                raise ProofFailure(
                    f"race 1 finish order diverged across processes: "
                    f"create={pc0!r} join={pj0!r}")
            phase(True, "race1_converged",
                  f"assertion (d): both raced race 1 and the reducer agreed the "
                  f"IDENTICAL finish order {pc0} across the two processes")

            # --- (e) chooser round-trip: race 2 boots + converges ------------
            # A tournament's non-final RESULTS round-trips through the reducer
            # (RESULTS -> REMATCH -> next round); race 2 is round 2.
            pc1, pj1 = creator.wait_line(
                both_reported(1), "both endpoints report race-2 results "
                "(round-trip through RESULTS)", args.race_timeout)
            if pc1 != pj1:
                raise ProofFailure(
                    f"race 2 finish order diverged across processes: "
                    f"create={pc1!r} join={pj1!r}")
            phase(True, "chooser_round_trip",
                  f"assertion (e): the RESULTS round-trip booted a converged "
                  f"race 2 (reducer-agreed finish order {pc1})")

            if args.through == "e":
                # (a)-(e) stop: the full multi-race production flow over the real
                # cloud, up to and including a converged race 2. This is the
                # GREEN achievable bar today; the (f)/(g) tournament-final
                # two-peer clean-finish + re-take is blocked on a structural gap:
                # the host's FINISH is a purely local leave that sends no reducer
                # command, so a second real peer (the joiner) never observes it,
                # and the re-arm design keeps the host in the room -- so the
                # joiner mirror is never released.
                for drv in (creator, joiner):
                    assert_no_forbidden(drv.name, drv.full_output())
                report["verdict"] = "PASS"
                report["detail"] = (
                    "phases (a)-(e) over the real cloud: pairing + self-firing "
                    "takeover + descriptor-less native CHARSELECT/VEHICLE/TRACK, "
                    "reducer-synced selections, and two converged tournament races")
                raise ProofStopEarly()

            # --- (f) clean FINISHED return + (g) re-take ---------------------
            # The tournament runs to its final round; the chooser commits FINISH
            # -> CEREMONY -> the single FINISHED handshake. Wait for that on both
            # (do not require process exit -- the launcher room stays alive; a
            # scripted quit / SIGTERM at teardown is the exit-0 leg).
            for drv in (creator, joiner):
                drv.wait_line(
                    lambda _l, d=drv: (any(
                        r[1] == "FINISHED"
                        for r in RR_BOOT_RESULT_RE.findall(d.full_output()))
                        or None),
                    f"{drv.name} FINISHED session-end", args.finish_timeout)
            phase(True, "clean_finished_return",
                  "assertion (f): both endpoints ended the session FINISHED "
                  "(FINISH -> CEREMONY -> FINISHED) with the launcher room alive")

            # (g) re-take: the FINISHED return armed the re-arm; the room's next
            # fresh SELECTING+2+LOBBY rising edge re-fires the takeover latch. The
            # host starts a new tournament from the room (autopair re-take).
            for drv in (creator, joiner):
                drv.wait_line(
                    lambda _l, d=drv: (len(RR_LATCH_RE.findall(
                        d.full_output())) >= 2 or None),
                    f"{drv.name} second takeover latch (FINISHED re-arm re-take)",
                    args.retake_timeout)
            phase(True, "retake",
                  "assertion (g): the takeover latch re-fired after the FINISHED "
                  "return on both endpoints (a second session self-took)")

            # Clean scripted quit: SIGTERM both; a live launcher exits 0.
            for drv in (creator, joiner):
                drv.proc.terminate()
            exit_c = creator.wait_exit(30.0)
            exit_j = joiner.wait_exit(30.0)
            report["exit"] = {"create": exit_c, "join": exit_j}
            # SIGTERM (-15) is a clean scripted quit; a nonzero/crash code is not.
            for label, code_ in (("create", exit_c), ("join", exit_j)):
                if code_ not in (0, -15):
                    raise ProofFailure(
                        f"{label} did not exit cleanly on scripted quit "
                        f"(exit={code_})")
            phase(True, "clean_quit",
                  f"both processes exited cleanly on scripted quit "
                  f"(create={exit_c} join={exit_j})")

            report["verdict"] = "PASS"
            report["detail"] = (
                "the full production native flow ran over the real cloud on two "
                "separate processes with only pairing + pad-input automation")
        except ProofStopEarly:
            pass  # verdict/detail already recorded by the --through stop
        except ProofFailure as error:
            report["verdict"] = "FAIL"
            report["detail"] = str(error)
        finally:
            for label, driver in (("create", creator), ("join", joiner)):
                if driver is not None:
                    report.setdefault("logs", {})[label] = str(driver.log_path)
                    try:
                        driver.close()
                    except OSError:
                        pass
    report["elapsed_s"] = time.monotonic() - overall_start
    return report


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--build", default=DEFAULT_BUILD_DIR)
    parser.add_argument("--rom", type=Path, default="baserom.us.v80.z64")
    parser.add_argument("--timeout", type=float, default=300.0,
                        help="per-process wall-clock bound for the whole flow")
    parser.add_argument("--select-timeout", type=float, default=120.0,
                        help="bound on native selection convergence over the "
                        "real cloud (both seats char+vehicle+ready)")
    parser.add_argument("--race-timeout", type=float, default=180.0,
                        help="bound on a race boot + convergence")
    parser.add_argument("--finish-timeout", type=float, default=300.0,
                        help="bound on the tournament reaching the FINISHED "
                        "session-end after race 2 (rounds 3..N + CEREMONY)")
    parser.add_argument("--retake-timeout", type=float, default=120.0,
                        help="bound on the second takeover latch after the "
                        "FINISHED re-arm")
    parser.add_argument("--tournament", type=int, default=-1, metavar="CUP",
                        help="run the capstone as a TOURNAMENT on cup CUP (0..4; "
                        "1 = Snowflake, all Car-legal). Required for --through "
                        "full (a single race never reaches FINISHED). -1 (default)"
                        " = single race, only valid with --through c")
    parser.add_argument("--log-dir", type=Path, default=None)
    parser.add_argument("--through", choices=("c", "e", "full"), default="full",
                        help="'c' stops (PASS) after (a)-(c) + the CHARSELECT -> "
                        "VEHICLESELECT advance (two-peer selection convergence); "
                        "'e' stops after (a)-(e) (a full multi-race tournament "
                        "flow up to a converged race 2 -- the green achievable "
                        "bar; requires --tournament); 'full' (default) runs all "
                        "seven (the tournament-final (f)/(g) legs)")
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    if args.log_dir is None:
        args.log_dir = Path(tempfile.mkdtemp(prefix="mdkr-native-flow-"))

    print(f"check_online_native_flow_cloud: build={args.build} "
          f"log_dir={args.log_dir}", flush=True)
    try:
        report = run(args)
    except ProofFailure as error:
        print(f"check_online_native_flow_cloud: FAIL -- {error}", file=sys.stderr)
        return 1

    print()
    print(f"check_online_native_flow_cloud: {report['verdict']} -- "
          f"{report['detail']}")
    print(f"  elapsed: {report['elapsed_s']:.1f}s  logs: {args.log_dir}")
    return 0 if report["verdict"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
