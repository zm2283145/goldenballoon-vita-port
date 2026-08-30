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
  d. race 1 converges on BOTH witnesses: the reducer-agreed finish order AND the
     cross-process state-hash fold (each resident coordinator folds the same
     firstTick-anchored confirmed-input window into an FNV state hash and logs
     it; identical span + identical hash across the two processes)
  e. chooser round-trip: race 2 boots and converges the same two ways
  f. clean return: both end FINISHED, the launcher room alive, exit 0 -- the
     host's FINISH first dispatches the REMATCH wrap (the room leaves RESULTS,
     reducer-observable), so the real joiner's chooser mirror exits to its OWN
     ceremony instead of stranding
  g. re-take: a SECOND [online-room-ready] latch AND a second descriptor-less
     native session reaching CHARSELECT on BOTH endpoints (the FINISHED re-arm
     completes into the freshly wrapped fresh-series tournament room)

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
# TWO independent witnesses per race, both required:
#   1. the reducer-agreed FINISH ORDER: both endpoints independently commit the
#      SAME placements for the SAME race_index (PUBLISH_RESULTS over the real
#      cloud) -- reducer-level agreement, but a weak projection of the race
#      state (identical finish orders can mask divergent simulations);
#   2. the STATE-HASH fold: each endpoint's resident coordinator folds a FIXED
#      firstTick-anchored window of CONFIRMED canonical input frames into an
#      FNV state hash (the same fold the per-race path emits) and logs it
#      per epoch -- two endpoints that simulated the same race print the
#      IDENTICAL span AND hash. Cross-process equality of that hash is the
#      strong convergence bar.
RACE_BOOT_RE = re.compile(
    r"^\[online-session\] phase=RACE booting", re.MULTILINE)
DIRECT_BOOT_RE = re.compile(
    r"^\[online-boot\] direct race: track=(\d+) players=(\d+)$", re.MULTILINE)
RESULTS_REPORTED_RE = re.compile(
    r"^\[online-resident-live\] race results reported placements=([0-9,]+) "
    r"accepted=(\d+) race_index=(\d+)", re.MULTILINE)
RACE_FOLD_RE = re.compile(
    r"^\[online-resident-live\] race fold epoch=(\d+) race_index=(\d+) "
    r"span=(\d+)\.\.(\d+) hash=([0-9a-f]{16})$", re.MULTILINE)


def placements_for(output: str, race_index: int) -> Optional[str]:
    """The finish-order placements a process committed for a given race_index,
    or None if it never reported that race."""
    for m in RESULTS_REPORTED_RE.finditer(output):
        if int(m.group(3)) == race_index and m.group(2) == "1":
            return m.group(1)
    return None


def fold_for(output: str, race_index: int) -> Optional[tuple]:
    """The (span_start, span_end, hash) state-fold witness a process emitted for
    a given race_index, or None if that race's fold never confirmed."""
    for m in RACE_FOLD_RE.finditer(output):
        if int(m.group(2)) == race_index:
            return (m.group(3), m.group(4), m.group(5))
    return None

FORBIDDEN_MARKERS = (
    "[FATAL]", "[CRASH]", "AddressSanitizer",
    "online race admission rejected",
    "launcher input provider rejected",
)

# ---- Drop scenarios (1 + 2): peer-loss on the production native takeover ------
# The drop-injection seams (main_app.cpp) refuse ONE endpoint's authored-tick
# drain -- exactly what a peer that dropped at the race-boot barrier (scenario 1)
# / cleanly mid-race (scenario 2) does. The engine's rollback then starves its
# canonical-input boundary, which pre-fix abort()ed BOTH machines; the crash fix
# routes it to a clean return-to-room instead.
DROP_SEAM_RACE_START_RE = re.compile(
    r"^\[online-live\] TEST: race-start tick-\d+ remote input UNAVAILABLE",
    re.MULTILINE)
DROP_SEAM_MID_RACE_RE = re.compile(
    r"^\[online-live\] TEST: mid-race tick-\d+ remote input UNAVAILABLE",
    re.MULTILINE)
# The engine crash-fix: the recoverable boundary starvation routed to a clean
# return-to-room (validate_boundary at race start / prepare_tick mid-race).
PEER_LOSS_LEFT_RE = re.compile(
    r"^\[online-session\] LEFT: online peer/input lost", re.MULTILINE)
# The survivor's real peer-loss witnesses (any one is a truthful attribution):
# the race-start barrier abort, the mid-race peer-lost latch, or the mesh event.
BARRIER_ABORT_RE = re.compile(
    r"^\[START\] race-start barrier: remote tick-\d+ input (?:peer lost|TIMED OUT)",
    re.MULTILINE)
MIDRACE_LATCH_RE = re.compile(
    r"^\[online-live\] peer lost mid-race at tick", re.MULTILINE)
MESH_PEER_LOST_RE = re.compile(
    r"^\[MESH\] peer LOST ep=\d+ reason=(\d+)", re.MULTILINE)
# The adapter's truthful card mapping of a mesh loss (failure= is the
# MdkrOnlineViewFailure the recovery card fronts; OPPONENT_LEFT for a
# mid-race departure -- the no-demotion rule).
MESH_PEER_LOST_FAILURE_RE = re.compile(
    r"^\[MESH\] peer LOST ep=\d+ reason=(\d+) -> failure=(\d+)", re.MULTILINE)
# The launcher session-end witness the production takeover logs on return.
SESSION_END_RE = re.compile(
    r"^\[online-session-end\] reason=(\S+) result=(-?\d+)", re.MULTILINE)
# The bounded-watchdog recovery paths a PROMPT mid-race peer-loss detection
# makes unnecessary -- FORBIDDEN on the mid-race-kill survivor.
SURVIVOR_WATCHDOG_MARKERS = ("round advance TIMEOUT", "descless wait TIMEOUT")
# Slack on top of the named detection bound for real-cloud scheduling /
# line-flush jitter (the bound itself is ping interval + stale, parsed below).
KILL_DETECT_SLACK_S = 15.0


def mesh_detect_bound_ms() -> int:
    """kMdkrMatchMidRaceLossDetectBoundMs from the source of truth (the mesh
    header), never a magic instrument number."""
    text = (ROOT / "platform/online/match_peer_transport.h").read_text(
        encoding="utf-8")
    values = []
    for name in ("kMdkrMatchControlPingIntervalMs",
                 "kMdkrMatchControlPingTimeoutMs"):
        m = re.search(rf"unsigned {name} = (\d+)u", text)
        if m is None:
            raise ProofFailure(f"could not parse {name} from the mesh header")
        values.append(int(m.group(1)))
    return sum(values)


def opponent_left_failure_value() -> int:
    """MDKR_ONLINE_VIEW_FAILURE_OPPONENT_LEFT's enumerator value (sequential
    value-less enum; the beta arm is compiled in for every cloud build)."""
    text = (ROOT / "platform/online/lobby_view_model.h").read_text(
        encoding="utf-8")
    m = re.search(r"typedef enum MdkrOnlineViewFailure \{(.*?)\}", text,
                  re.DOTALL)
    if m is None:
        raise ProofFailure("could not parse MdkrOnlineViewFailure")
    body = re.sub(r"/\*.*?\*/", "", m.group(1), flags=re.DOTALL)
    body = body.replace("#if MDKR_ENABLE_ONLINE_BETA", "").replace(
        "#endif", "")
    names = []
    for token in body.split(","):
        name = token.strip().split("=")[0].strip()
        if re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", name or ""):
            names.append(name)
    if "MDKR_ONLINE_VIEW_FAILURE_OPPONENT_LEFT" not in names:
        raise ProofFailure("OPPONENT_LEFT missing from MdkrOnlineViewFailure")
    return names.index("MDKR_ONLINE_VIEW_FAILURE_OPPONENT_LEFT")


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
             run_dir: Path, tournament_cup: int,
             drop: Optional[str] = None, drop_role: str = "join",
             drop_tick: int = 50) -> dict:
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
        # (index 5) -> the REMATCH wrap (room leaves RESULTS; the joiner's mirror
        # follows into its OWN ceremony) -> CEREMONY -> the single FINISHED
        # handshake on each endpoint.
        MDKR_TEST_ONLINE_RESULTS_CHOOSER="5",
    )
    if join_code is not None:
        environment["MDKR_APP_TEST_ONLINE_JOIN_CODE"] = join_code
    # Peer-drop injection on ONE endpoint (scenarios 1 + 2). The drop-role
    # endpoint refuses its race-boot tick-1 drain (race-start) or one mid-race
    # tick (mid-race); the engine's rollback then starves its boundary and takes
    # the crash-fix's clean return-to-room. The SURVIVOR starves the same peer's
    # input and takes the SAME fix. Never phase-forcing -- input refusal only.
    if drop is not None and role == drop_role:
        if drop == "race-start":
            environment["MDKR_APP_TEST_ONLINE_DROP_RACE_START_INPUT"] = "1"
        elif drop == "mid-race":
            environment["MDKR_APP_TEST_ONLINE_DROP_INPUT_AT_TICK"] = str(drop_tick)
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
                                 tournament_cup, args.drop, args.drop_role),
                root / "create", "create", log_dir / "create.log", args.verbose)
            code = creator.wait_line(
                AUTOPAIR_CODE_RE.match, "room code (autopair create)", 90.0
            ).group(1)
            phase(True, "pair_create",
                  f"code={code} (interactive create over the real cloud)")

            joiner = Driver(
                binary, make_env("join", code, JOIN_PICK, rom, root / "join",
                                 tournament_cup, args.drop, args.drop_role),
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

            # --- Drop scenarios 1 + 2: peer-loss on the production path -------
            # The end-to-end contract of the two peer-loss crash fixes, run for
            # the FIRST time on the path production actually takes (the native
            # takeover, peer==nullptr), with a drop injected on ONE endpoint.
            if args.drop is not None:
                dropped = joiner if args.drop_role == "join" else creator
                survivor = creator if args.drop_role == "join" else joiner
                scen = "1 (race-start)" if args.drop == "race-start" else "2 (mid-race)"
                # Both endpoints boot race 1 (the drop happens DURING it).
                for drv in (creator, joiner):
                    drv.wait_line(RACE_BOOT_RE.search,
                                  f"{drv.name} race 1 boot", args.race_timeout)

                # No-abort is the CORE invariant (the exact SIGABRT the crash fix
                # removed on both machines). The mid-race prepare_tick trigger line
                # ("launcher input provider rejected") is the EXPECTED recoverable
                # trigger the fix recovers from -- exempt it, as the local
                # peer-loss lane's MIDRACE_FORBIDDEN does.
                def assert_no_crash(drv, killed=False):
                    for marker in FORBIDDEN_MARKERS:
                        if marker == "launcher input provider rejected":
                            continue
                        if marker in drv.full_output():
                            raise ProofFailure(
                                f"{drv.name}: forbidden marker {marker!r} -- the "
                                f"peer loss crashed instead of returning to room")
                    rc = drv.proc.poll()
                    if rc is not None and rc < 0 and not killed:
                        raise ProofFailure(
                            f"{drv.name}: died from signal {-rc} on the peer loss "
                            f"(the abort the crash fix removes)")

                # Attribution: which clean return-to-room path an endpoint took.
                def recovery_path(drv):
                    o = drv.full_output()
                    boot = RR_BOOT_RESULT_RE.findall(o)
                    reason = boot[-1][1] if boot else "?"
                    fix = PEER_LOSS_LEFT_RE.search(o) is not None
                    mesh = MESH_PEER_LOST_RE.search(o)
                    loss = ("mid-race peer-lost latch"
                            if MIDRACE_LATCH_RE.search(o) else
                            "race-start barrier abort"
                            if BARRIER_ABORT_RE.search(o) else
                            f"mesh peer LOST reason={mesh.group(1)}" if mesh else
                            "watchdog (no peer-loss latch observed)")
                    return (f"reason={reason}"
                            + (", crash-fix LEFT" if fix else "")
                            + f", via {loss}")

                if args.drop_method == "kill":
                    # The FAITHFUL "console/transport drops" test: after the race
                    # is under way, KILL the drop-role process so its mesh actually
                    # CLOSES (its signal socket dies too -- the service broadcasts
                    # presence=false, the exact real-loss signature).
                    if args.drop == "mid-race":
                        time.sleep(args.drop_kill_delay)
                    kill_at = time.monotonic()
                    dropped.proc.kill()
                    phase(True, "drop_injected",
                          f"race 1 under way over the real cloud; KILLED the "
                          f"{dropped.name} process (real transport drop, "
                          f"scenario {scen})")
                    if args.drop == "mid-race":
                        # STRICT contract: a real mid-race kill must be
                        # detected PROMPTLY by the transport's own liveness
                        # ladder (within the named bound
                        # kMdkrMatchMidRaceLossDetectBoundMs + slack), latch
                        # the truthful OPPONENT_LEFT, and end the survivor's
                        # race via the EXISTING mid-race latch -> crash-fix
                        # LEFT. The watchdog is FORBIDDEN as the recovery.
                        bound_s = mesh_detect_bound_ms() / 1000.0
                        opponent_left = opponent_left_failure_value()
                        survivor.wait_line(
                            lambda _l: (MESH_PEER_LOST_FAILURE_RE.search(
                                survivor.full_output()) or None),
                            f"{survivor.name} typed [MESH] peer LOST within "
                            f"the named bound ({bound_s:.0f}s + "
                            f"{KILL_DETECT_SLACK_S:.0f}s slack) of the kill",
                            bound_s + KILL_DETECT_SLACK_S)
                        detect_s = time.monotonic() - kill_at
                        mapped = MESH_PEER_LOST_FAILURE_RE.search(
                            survivor.full_output())
                        if int(mapped.group(2)) != opponent_left:
                            raise ProofFailure(
                                f"{survivor.name}: mesh loss mapped to "
                                f"failure={mapped.group(2)}, expected the "
                                f"truthful OPPONENT_LEFT ({opponent_left}) -- "
                                f"peer departure must never demote to a "
                                f"connection failure")
                        survivor.wait_line(
                            MIDRACE_LATCH_RE.search,
                            f"{survivor.name} mid-race peer-loss latch "
                            f"(no ghost race)", 30.0)
                        survivor.wait_line(
                            PEER_LOSS_LEFT_RE.search,
                            f"{survivor.name} crash-fix clean return (LEFT)",
                            30.0)
                        survivor.wait_line(
                            lambda _l: (any(
                                r == ("LEFT", "0") for r in
                                SESSION_END_RE.findall(
                                    survivor.full_output())) or None),
                            f"{survivor.name} session end reason=LEFT "
                            f"result=0 (never the watchdog ERROR)", 30.0)
                        end_s = time.monotonic() - kill_at
                        sout = survivor.full_output()
                        for marker in SURVIVOR_WATCHDOG_MARKERS:
                            if marker in sout:
                                raise ProofFailure(
                                    f"{survivor.name}: watchdog path fired "
                                    f"({marker!r}) -- recovery must be the "
                                    f"peer-loss latch, the watchdog is only "
                                    f"the safety net")
                        assert_no_crash(survivor)
                        assert_no_crash(dropped, killed=True)
                        reason = mapped.group(1)
                        phase(True, "drop_truthful_prompt",
                              f"scenario {scen} REAL KILL: kill -> [MESH] "
                              f"peer LOST reason={reason} in {detect_s:.1f}s "
                              f"(bound {bound_s:.0f}s), truthful "
                              f"OPPONENT_LEFT latched, mid-race latch -> "
                              f"clean LEFT in {end_s:.1f}s -- no watchdog, "
                              f"no abort")
                        report["verdict"] = "PASS"
                        report["detail"] = (
                            f"REAL mid-race transport kill over the real "
                            f"cloud (production descriptor-less takeover): "
                            f"survivor detected the loss in {detect_s:.1f}s "
                            f"(typed reason={reason}, named bound "
                            f"{bound_s:.0f}s), latched the truthful "
                            f"OPPONENT_LEFT card, and ended via the mid-race "
                            f"latch -> crash-fix LEFT in {end_s:.1f}s; no "
                            f"watchdog, no abort on either endpoint")
                        raise ProofStopEarly()
                    survivor.wait_line(
                        lambda _l: (RR_BOOT_RESULT_RE.search(
                            survivor.full_output()) or None),
                        f"{survivor.name} bounded clean return to the room "
                        f"(no abort)", args.race_timeout)
                    assert_no_crash(survivor)
                    assert_no_crash(dropped, killed=True)
                    path = recovery_path(survivor)
                    phase(True, "drop_clean_return",
                          f"scenario {scen} (real transport drop): the survivor "
                          f"returned to the room BOUNDED and did NOT abort "
                          f"({path})")
                    report["verdict"] = "PASS"
                    report["detail"] = (
                        f"peer-drop scenario {scen} (REAL transport drop -- peer "
                        f"process killed) over the real cloud on the production "
                        f"descriptor-less native takeover (peer==nullptr): the "
                        f"survivor returned to the room bounded, no abort "
                        f"({path})")
                    raise ProofStopEarly()

                # The sanctioned drop-injection SEAM on ONE endpoint: it refuses
                # that endpoint's authored-tick drain, so the DROPPED endpoint's
                # rollback boundary starves -> the crash-fix clean return. (The
                # seam leaves the dropped endpoint's LAUNCHER in the room with its
                # mesh alive, so the survivor does not observe a mesh peer-loss --
                # it returns via the bounded round-advance / descriptor-less
                # wall-clock watchdog instead. Use --drop-method kill for the
                # survivor's peer-loss crash-fix + truthful card.)
                seam_re = (DROP_SEAM_RACE_START_RE if args.drop == "race-start"
                           else DROP_SEAM_MID_RACE_RE)
                dropped.wait_line(seam_re.search,
                                  f"{dropped.name} {args.drop} drop seam fired",
                                  args.race_timeout)
                phase(True, "drop_injected",
                      f"both endpoints booted race 1 over the real cloud; the "
                      f"{args.drop} drop seam fired on the {dropped.name}")
                # The DROPPED endpoint routes the boundary starvation to the
                # crash-fix clean return-to-room (LEFT) -- scenario 1/2's crash fix,
                # proven for the FIRST time on the production takeover path.
                dropped.wait_line(
                    PEER_LOSS_LEFT_RE.search,
                    f"{dropped.name} crash-fix clean return "
                    f"([online-session] LEFT: peer/input lost)", args.race_timeout)
                dropped.wait_line(
                    lambda _l: (any(r == ("LEFT", "0") for r in
                                    SESSION_END_RE.findall(dropped.full_output()))
                                or None),
                    f"{dropped.name} launcher read reason=LEFT result=0",
                    args.race_timeout)
                # The SURVIVOR must ALSO return to the room cleanly and BOUNDED
                # (no abort, no hang) -- via the peer-loss latch if it sees the
                # mesh drop, else the wall-clock watchdog.
                survivor.wait_line(
                    lambda _l: (RR_BOOT_RESULT_RE.search(survivor.full_output())
                                or None),
                    f"{survivor.name} bounded clean return to the room",
                    args.race_timeout)
                assert_no_crash(dropped)
                assert_no_crash(survivor)
                # R2: for a RACE-START seam drop the survivor's recovery must be
                # the PROMPT + TRUTHFUL path -- its race-start barrier catches the
                # never-delivered tick-1 and routes to the crash-fix clean return
                # (not the wall-clock watchdog). Assert it rather than merely
                # observing it (mid-race has no such prompt survivor signal).
                if args.drop == "race-start":
                    sout = survivor.full_output()
                    if not BARRIER_ABORT_RE.search(sout):
                        raise ProofFailure(
                            f"{survivor.name}: race-start drop but NO race-start "
                            f"barrier abort -- the prompt survivor path did not "
                            f"fire (it would fall back to the watchdog)")
                    if not PEER_LOSS_LEFT_RE.search(sout):
                        raise ProofFailure(
                            f"{survivor.name}: race-start barrier fired but the "
                            f"survivor did not reach the crash-fix clean return "
                            f"(LEFT)")
                spath = recovery_path(survivor)
                phase(True, "drop_clean_return",
                      f"scenario {scen} (drop seam on one endpoint): the dropped "
                      f"endpoint took the crash-fix clean return-to-room (LEFT), "
                      f"the survivor returned bounded ({spath}), NO abort on either "
                      f"-- the production-path crash-fix holds")
                report["verdict"] = "PASS"
                report["detail"] = (
                    f"peer-drop scenario {scen} over the REAL cloud on the "
                    f"production descriptor-less native takeover (peer==nullptr): "
                    f"the dropped endpoint took the crash-fix clean return-to-room "
                    f"(LEFT), the survivor returned bounded ({spath}), and NEITHER "
                    f"endpoint aborted (the exact crash the fix removes)")
                raise ProofStopEarly()

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

            # --- (d) race 1 converges: finish order AND state hash -----------
            def both_reported(idx: int):
                def pred(_line: str):
                    pc = placements_for(creator.full_output(), idx)
                    pj = placements_for(joiner.full_output(), idx)
                    return (pc, pj) if (pc is not None and pj is not None) else None
                return pred

            def assert_state_fold(idx: int, label: str) -> str:
                """Both endpoints emitted the race's state-hash fold witness over
                the IDENTICAL span with the IDENTICAL hash. The fold lands
                mid-race (well before the results report), so by the time both
                results are in it is present or genuinely missing -- a missing
                fold means the window never fully confirmed, which is itself a
                convergence failure worth failing on."""
                fc = fold_for(creator.full_output(), idx)
                fj = fold_for(joiner.full_output(), idx)
                if fc is None or fj is None:
                    raise ProofFailure(
                        f"{label}: a state-hash fold witness is missing "
                        f"(create={fc!r} join={fj!r}) -- the confirmed-input "
                        f"window never converged on one endpoint")
                if fc != fj:
                    raise ProofFailure(
                        f"{label}: the state-hash folds DIVERGED across the two "
                        f"processes: create=span {fc[0]}..{fc[1]} hash={fc[2]} "
                        f"join=span {fj[0]}..{fj[1]} hash={fj[2]} -- identical "
                        f"finish orders cannot excuse divergent simulations")
                return fc[2]

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
            hash0 = assert_state_fold(0, "race 1")
            phase(True, "race1_converged",
                  f"assertion (d): both raced race 1 and converged on BOTH "
                  f"witnesses -- reducer-agreed finish order {pc0} AND the "
                  f"cross-process state-hash fold {hash0}")

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
            hash1 = assert_state_fold(1, "race 2")
            phase(True, "chooser_round_trip",
                  f"assertion (e): the RESULTS round-trip booted a converged "
                  f"race 2 (reducer-agreed finish order {pc1}, cross-process "
                  f"state-hash fold {hash1})")

            if args.through == "e":
                # (a)-(e) stop: the full multi-race production flow over the real
                # cloud, up to and including a converged race 2. Historically the
                # green achievable bar while the tournament-final two-peer
                # clean-finish was blocked (the host's FINISH used to be a purely
                # local leave that sent no reducer command, stranding the second
                # real peer's chooser mirror); the FINISH now dispatches the
                # REMATCH wrap, so `full` is the green bar and this stop remains
                # as a faster intermediate qualification.
                for drv in (creator, joiner):
                    assert_no_forbidden(drv.name, drv.full_output())
                report["verdict"] = "PASS"
                report["detail"] = (
                    "phases (a)-(e) over the real cloud: pairing + self-firing "
                    "takeover + descriptor-less native CHARSELECT/VEHICLE/TRACK, "
                    "reducer-synced selections, and two converged tournament races")
                raise ProofStopEarly()

            # --- (f) clean FINISHED return + (g) re-take ---------------------
            # The tournament runs to its final round; the host's chooser commits
            # FINISH, which first dispatches the REMATCH wrap (the room leaves
            # RESULTS -- the reducer-observable transition the real joiner's
            # chooser mirror exits on), then -> CEREMONY -> the single FINISHED
            # handshake ON BOTH endpoints (the joiner reaches its OWN ceremony
            # from its latched final standings). Wait for that on both (do not
            # require process exit -- the launcher room stays alive; a scripted
            # quit / SIGTERM at teardown is the exit-0 leg).
            for drv in (creator, joiner):
                drv.wait_line(
                    lambda _l, d=drv: (any(
                        r[1] == "FINISHED"
                        for r in RR_BOOT_RESULT_RE.findall(d.full_output()))
                        or None),
                    f"{drv.name} FINISHED session-end", args.finish_timeout)
            phase(True, "clean_finished_return",
                  "assertion (f): both endpoints ended the session FINISHED "
                  "(FINISH -> wrap -> CEREMONY -> FINISHED) with the launcher "
                  "room alive")

            # (g) re-take: each FINISHED return arms the re-arm; the panel's
            # observer completes it immediately (the FINISH wrap already returned
            # the room to a fresh-series SELECTING+2+LOBBY), so the takeover
            # re-fires BY ITSELF on both endpoints and a SECOND descriptor-less
            # native session must reach CHARSELECT on both -- the re-take into a
            # fresh tournament, fully hands-off.
            for drv in (creator, joiner):
                drv.wait_line(
                    lambda _l, d=drv: (len(RR_LATCH_RE.findall(
                        d.full_output())) >= 2 or None),
                    f"{drv.name} second takeover latch (FINISHED re-arm re-take)",
                    args.retake_timeout)
            for drv in (creator, joiner):
                drv.wait_line(
                    lambda _l, d=drv: ((len(BEGIN_LOBBY_RE.findall(
                        d.full_output())) >= 2 and len(
                        CHARSELECT_ENTER_RE.findall(d.full_output())) >= 2)
                        or None),
                    f"{drv.name} second descriptor-less session reached native "
                    f"CHARSELECT (re-take)", args.retake_timeout)
            phase(True, "retake",
                  "assertion (g): the takeover re-fired after the FINISHED "
                  "return on BOTH endpoints and a second descriptor-less native "
                  "session reached CHARSELECT (re-take into a fresh tournament)")

            # Clean scripted quit. The (g) re-take leaves both processes INSIDE
            # the second native session (by design -- the automatic re-take), so
            # the first SIGTERM is consumed by the ENGINE session (a clean engine
            # shutdown; the launcher reads reason=NONE and does NOT re-arm -- no
            # boot loop) and the launcher keeps drawing the room. A second
            # SIGTERM then quits the launcher itself. Accept a one-step exit too
            # (a process caught at the panel quits on the first).
            def scripted_quit(drv) -> int:
                drv.proc.terminate()
                try:
                    return drv.proc.wait(timeout=20)
                except subprocess.TimeoutExpired:
                    drv.proc.terminate()
                return drv.wait_exit(20.0)
            exit_c = scripted_quit(creator)
            exit_j = scripted_quit(joiner)
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
    parser.add_argument("--drop", choices=("race-start", "mid-race"), default=None,
                        help="peer-drop scenario: inject a race-start (scenario 1) "
                        "or mid-race (scenario 2) drop on ONE endpoint after the "
                        "descriptor-less CHARSELECT, and prove BOTH endpoints take "
                        "the crash-fix clean return-to-room (no abort). Runs "
                        "through (a)-(c) + the drop; ignores --through.")
    parser.add_argument("--drop-role", choices=("create", "join"), default="join",
                        help="which endpoint the --drop is injected on (default "
                        "join). The other endpoint is the survivor.")
    parser.add_argument("--drop-method", choices=("seam", "kill"), default="seam",
                        help="'seam' (default) refuses the drop-role endpoint's "
                        "drain (proves THAT endpoint's crash-fix; the survivor "
                        "returns via the watchdog since the mesh stays alive). "
                        "'kill' SIGKILLs the drop-role process (a real transport "
                        "drop) to prove the SURVIVOR's peer-loss crash-fix + card.")
    parser.add_argument("--drop-kill-delay", type=float, default=10.0,
                        help="seconds into the race to wait before a mid-race "
                        "--drop-method kill (default 10)")
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
