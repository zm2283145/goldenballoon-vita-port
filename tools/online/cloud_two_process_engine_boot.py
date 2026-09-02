#!/usr/bin/env python3
"""Two SEPARATE processes, each booting the VISIBLE engine, over the LIVE cloud.

tests/check_online_engine_boot.py proves the make-or-break engine wiring: the
VISIBLE 3D engine runs a networked race driven by the LIVE adapter transport.
But it proves it with BOTH endpoints living inside ONE process, over an
in-process loopback signal hub -- not the real deployed service.

tools/online/cloud_two_session_smoke.py proves the real deployed service: two
`mdkr_online_live_transport_e2e_driver` processes race over
`https://party.goldenballoon.net` and converge -- but that driver never boots
the engine; it is a headless tick loop.

This tool closes the gap: it launches TWO SEPARATE `mdkr64` app processes on
this machine, each with `MDKR_APP_TEST_ONLINE_LIVE_CLOUD=1` (see
platform/app/main_app.cpp's runAutoplay and
platform/app/online_live_wiring.cpp's OnlineRoom_makeTestCloudLiveSession) --
one `create`, one `join`-by-code -- both against the SAME real cloud origin
(compiled into the binary as MDKR_PARTY_ORIGIN), each independently driving a
single production-shaped live adapter through the real HTTP lobby + WSS
signal + WebRTC mesh to a ready race transport, THEN each booting its own
VISIBLE 3D engine (mdkr64_engine_boot, offscreen/hidden) with that real
adapter as the match-input source -- the `peer == nullptr` production path
`liveDrainMatchInput()` already implements; the real peer process supplies
the other endpoint's input over the mesh, not an in-process double.

Both endpoints run the identical fixed-tick autoplay script
(tests/input_scripts/race_2p_split.txt, MDKR_APP_AUTOPLAY_TICKS ticks each),
so absent a stall both engines execute the exact same total tick count and
reach the exact same final authored race tick -- the two independently
computed FNV-1a canonical-input fold windows (see runOnlineLiveEngineSession's
foldConfirmedRace, main_app.cpp) therefore span the identical absolute tick
range and are directly comparable, byte for byte, across the two real OS
processes. This is what "converged" means below.

Both endpoints sit on the SAME machine, so (like cloud_two_session_smoke.py)
this does NOT exercise real cross-network NAT traversal: the DataChannel
media path is effectively loopback regardless of what the cloud origin
advertises as ICE servers. Treat a PASS here as "two independent processes,
each running the real visible engine, converge over real cloud signaling +
a real (if locally-looped-back) WebRTC data plane" -- not as a WAN proof.

Usage:
    python3 tools/online/cloud_two_process_engine_boot.py \\
        --build build-cloud-proof --rom /path/to/baserom.us.v80.z64 \\
        --ticks 3000 --timeout 240 --log-dir /path/to/logs -v
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

SCRIPT = ROOT / "tests/input_scripts/race_2p_split.txt"
TICKS = 3000

CODE_RE = re.compile(r"^\[online-live-cloud\] code=(\d{6})$", re.MULTILINE)
JOINED_RE = re.compile(r"^\[online-live-cloud\] joined$", re.MULTILINE)
MEMBERS2_RE = re.compile(r"^\[online-live-cloud\] members=2$", re.MULTILINE)
PHRASE_RE = re.compile(r"^\[online-live-cloud\] phrase=(.+)$", re.MULTILINE)
READY2_RE = re.compile(r"^\[online-live-cloud\] ready=2$", re.MULTILINE)
RACE_READY_RE = re.compile(
    r"^\[online-live-cloud\] race-ready epoch=(\d+) firstTick=(\d+) "
    r"active=(0x[0-9a-f]{2}) local=(0x[0-9a-f]{2}) remote=(0x[0-9a-f]{2})$",
    re.MULTILINE,
)
CLOUD_ERROR_RE = re.compile(
    r"^\[online-live-cloud\] result=error message=(.+)$", re.MULTILINE)
BOOT_MARKER = "[online-live] booting visible engine"
ONLINE_RACE_RE = re.compile(
    r"^\[ROLLBACK\] online race: loadedTrack=(\d+) raceType=(\d+) "
    r"authoredHz=(\d+)$",
    re.MULTILINE,
)
ENGINE_LIVE_RE = re.compile(
    r"^\[ENGINE-ONLINE-LIVE\] result=(-?\d+) racedTicks=(\d+) drainCalls=(\d+) "
    r"advanceFailed=(\d+) inputEnvelopes=(\d+) transportAccepted=(\d+) "
    r"transportCorrected=(\d+) transportDrained=(\d+) foldVisible=(\d+) "
    r"foldPeer=(\d+) hashVisible=([0-9a-f]{16}) hashPeer=([0-9a-f]{16}) "
    r"converged=(\d+)$",
    re.MULTILINE,
)
FORBIDDEN_MARKERS = (
    "[FATAL]", "[CRASH]", "AddressSanitizer",
    "online race admission rejected",
    "launcher input provider rejected",
    "engine startup rejected before authored tick one",
)


class ProofFailure(RuntimeError):
    """Harness-level failure; message is the report."""


def clean_environment(**updates: str) -> dict:
    environment = {
        key: value for key, value in os.environ.items()
        if not key.startswith(("MDKR", "GE007_"))
    }
    environment.update(updates)
    return environment


class Driver:
    """A narrated `mdkr64` app process, tailing merged stdout/stderr to both an
    in-memory buffer and a log file in real time (mirrors
    tools/online/cloud_two_session_smoke.py's Driver)."""

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
                line = lines[index]
                err = CLOUD_ERROR_RE.match(line)
                if err:
                    raise ProofFailure(
                        f"{self.name} failed before '{description}': "
                        f"{err.group(0)}")
                value = predicate(line)
                if value:
                    return value
            scanned = len(lines)
            if self.proc.poll() is not None and scanned == len(self.snapshot()):
                break
            time.sleep(0.05)
        raise ProofFailure(
            f"{self.name} never printed '{description}' within {timeout:.0f}s; "
            f"last lines: {self.snapshot()[-12:]}")

    def wait_exit(self, timeout: float) -> int:
        try:
            return self.proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired as error:
            raise ProofFailure(
                f"{self.name} did not exit within {timeout:.0f}s (a stall "
                f"would look like this); last lines: {self.snapshot()[-12:]}"
            ) from error

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


def make_env(role: str, join_code: Optional[str], rom: Path, run_dir: Path,
             ticks: int, setup_timeout_ms: int, pace_hz: int) -> dict:
    (run_dir / "saves").mkdir(parents=True)
    (run_dir / "preferences").mkdir()
    environment = clean_environment(
        LC_ALL="C",
        MDKR_APP_AUTOPLAY="1",
        MDKR_APP_AUTOPLAY_INPUT_SCRIPT=str(SCRIPT),
        MDKR_APP_AUTOPLAY_TICKS=str(ticks),
        MDKR_APP_ONLINE_PACE_HZ=str(pace_hz),
        MDKR_APP_ONLINE_ROLE=role,
        MDKR_APP_ONLINE_TIMEOUT_MS=str(setup_timeout_ms),
        MDKR_APP_PREFS_DIR=str(run_dir / "preferences"),
        MDKR_APP_TEST_ONLINE_LIVE_CLOUD="1",
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
    if join_code is not None:
        environment["MDKR_APP_ONLINE_JOIN_CODE"] = join_code
    return environment


def analyze(name: str, output: str) -> dict:
    """Apply the same witnesses tests/check_online_engine_boot.py asserts, to
    one process's captured output. Raises ProofFailure on any violation."""
    for marker in FORBIDDEN_MARKERS:
        if marker in output:
            raise ProofFailure(f"{name}: observed forbidden marker {marker!r}")
    if BOOT_MARKER not in output:
        raise ProofFailure(
            f"{name}: the visible engine was never booted on the live "
            "transport")
    online_race = ONLINE_RACE_RE.findall(output)
    if not online_race:
        raise ProofFailure(f"{name}: the engine never entered an ONLINE "
                           "rollback race")
    loaded_track, race_type, authored_hz = online_race[0]
    if loaded_track != "5" or race_type != "0":
        raise ProofFailure(
            f"{name}: online race loaded the wrong contest track="
            f"{loaded_track} type={race_type} (expected Ancient Lake 5, "
            "standard 0)")
    stats = ENGINE_LIVE_RE.findall(output)
    if len(stats) != 1:
        raise ProofFailure(f"{name}: expected one ENGINE-ONLINE-LIVE witness, "
                           f"got {stats!r}")
    (result, raced, drains, advance_failed, envelopes, accepted, corrected,
     drained, fold_visible, fold_peer, hash_visible, hash_peer,
     converged) = stats[0]
    if int(result) != 0:
        raise ProofFailure(f"{name}: engine boot returned {result}")
    if int(advance_failed) != 0:
        raise ProofFailure(f"{name}: the live match-input source failed to "
                           "advance the race (a stall at the transport seam)")
    if int(raced) < 100 or int(drains) < 100:
        raise ProofFailure(
            f"{name}: the visible race did not sustain enough authored ticks "
            f"through the live seam (racedTicks={raced} drainCalls={drains}, "
            "expected >= 100)")
    if int(envelopes) <= 0 and int(accepted) <= 0:
        raise ProofFailure(
            f"{name}: no real peer input crossed the mesh into the transport "
            f"(inputEnvelopes={envelopes} transportAccepted={accepted})")
    if int(drained) <= 0:
        raise ProofFailure(f"{name}: the transport drained no authored ticks "
                           f"(transportDrained={drained})")
    if "[HOST-SHUTDOWN]" not in output:
        raise ProofFailure(f"{name}: the engine did not tear down its host "
                           "cleanly")
    race_ready = RACE_READY_RE.findall(output)
    if len(race_ready) != 1:
        raise ProofFailure(f"{name}: expected one race-ready witness, got "
                           f"{race_ready!r}")
    return {
        "authoredHz": int(authored_hz),
        "racedTicks": int(raced),
        "drainCalls": int(drains),
        "inputEnvelopes": int(envelopes),
        "transportAccepted": int(accepted),
        "transportCorrected": int(corrected),
        "transportDrained": int(drained),
        "foldVisible": int(fold_visible),
        "hashVisible": hash_visible,
        "epoch": int(race_ready[0][0]),
        "firstTick": int(race_ready[0][1]),
    }


def run(args: argparse.Namespace) -> dict:
    binary = Path(resolve_binary(args.build)).expanduser().resolve()
    rom = args.rom.expanduser().resolve()
    for path, label in ((binary, "binary"), (rom, "ROM"),
                        (SCRIPT, "input script")):
        if not path.is_file():
            raise ProofFailure(f"missing {label}: {path}")

    log_dir = args.log_dir
    log_dir.mkdir(parents=True, exist_ok=True)
    report: dict = {
        "party_origin_note": "compiled into the binary (MDKR_PARTY_ORIGIN)",
        "ticks": args.ticks, "phases": [], "verdict": None, "detail": None,
    }

    def phase(ok: bool, name: str, detail: str) -> None:
        report["phases"].append({"phase": name, "ok": ok, "detail": detail})
        marker = "OK  " if ok else "FAIL"
        print(f"  [{marker}] {name}: {detail}", flush=True)

    creator = joiner = None
    overall_start = time.monotonic()
    with tempfile.TemporaryDirectory(prefix="mdkr64-cloud-engine-") as temp:
        root = Path(temp)
        try:
            creator = Driver(
                binary, make_env("create", None, rom, root / "create",
                                 args.ticks, args.setup_timeout_ms,
                                 args.pace_hz),
                root / "create", "create", log_dir / "create.log",
                args.verbose)
            room = creator.wait_line(CODE_RE.match, "room code (cloud create)",
                                     45.0)
            code = room.group(1)
            phase(True, "cloud_create",
                  f"code={code} (real cloud HTTP create + /connect WS)")

            joiner = Driver(
                binary, make_env("join", code, rom, root / "join",
                                 args.ticks, args.setup_timeout_ms,
                                 args.pace_hz),
                root / "join", "join", log_dir / "join.log", args.verbose)
            joiner.wait_line(JOINED_RE.match, "join accepted by cloud", 45.0)
            phase(True, "cloud_join", "joiner's cloud HTTP join-by-code + "
                  "/connect WS succeeded")

            creator.wait_line(MEMBERS2_RE.match, "creator sees 2 members", 30.0)
            joiner.wait_line(MEMBERS2_RE.match, "joiner sees 2 members", 30.0)
            phase(True, "lobby_state_sync", "both endpoints observe "
                  "member_count>=2 over the cloud /connect WebSocket")

            phrase_c = creator.wait_line(PHRASE_RE.match,
                                         "verification phrase", 60.0)
            phrase_j = joiner.wait_line(PHRASE_RE.match,
                                        "verification phrase", 60.0)
            if phrase_c.group(1).strip() != phrase_j.group(1).strip():
                detail = (f"verification phrases diverged: "
                          f"create={phrase_c.group(1)!r} "
                          f"join={phrase_j.group(1)!r}")
                phase(False, "webrtc_mesh_signaling", detail)
                raise ProofFailure(detail)
            phase(True, "webrtc_mesh_signaling",
                  f"both peers negotiated real WebRTC DataChannels and agree "
                  f"on SAS phrase {phrase_c.group(1).strip()!r}")

            creator.wait_line(READY2_RE.match, "creator sees ready=2", 30.0)
            joiner.wait_line(READY2_RE.match, "joiner sees ready=2", 30.0)
            phase(True, "descriptor_install", "preflight consensus reached "
                  "and both endpoints readied")

            race_ready_c = creator.wait_line(
                RACE_READY_RE.match, "creator race transport ready",
                float(args.setup_timeout_ms) / 1000.0)
            race_ready_j = joiner.wait_line(
                RACE_READY_RE.match, "joiner race transport ready",
                float(args.setup_timeout_ms) / 1000.0)
            if race_ready_c.group(1) != race_ready_j.group(1) or \
                    race_ready_c.group(2) != race_ready_j.group(2):
                detail = (f"epoch/firstTick disagreement: "
                          f"create={race_ready_c.groups()} "
                          f"join={race_ready_j.groups()}")
                phase(False, "race_transport_ready", detail)
                raise ProofFailure(detail)
            phase(True, "race_transport_ready",
                  f"both endpoints agree epoch={race_ready_c.group(1)} "
                  f"firstTick={race_ready_c.group(2)} -- booting the VISIBLE "
                  "engine on each")

            exit_c = creator.wait_exit(args.timeout)
            exit_j = joiner.wait_exit(args.timeout)
            report["sessions"] = {
                "create": {"exit_code": exit_c, "log": str(creator.log_path)},
                "join": {"exit_code": exit_j, "log": str(joiner.log_path)},
            }
            if exit_c != 0 or exit_j != 0:
                detail = f"a process exited non-zero: create={exit_c} join={exit_j}"
                phase(False, "engine_race", detail)
                raise ProofFailure(detail)

            stats_c = analyze("create", creator.full_output())
            stats_j = analyze("join", joiner.full_output())
            phase(True, "engine_race",
                  f"both engines booted the visible race: create.racedTicks="
                  f"{stats_c['racedTicks']} join.racedTicks="
                  f"{stats_j['racedTicks']}")

            converged = (stats_c["hashVisible"] == stats_j["hashVisible"] and
                        stats_c["foldVisible"] >= 20 and
                        stats_j["foldVisible"] >= 20)
            report["stats"] = {"create": stats_c, "join": stats_j}
            if not converged:
                detail = (
                    f"fold hashes did not converge: create.foldVisible="
                    f"{stats_c['foldVisible']} create.hash="
                    f"{stats_c['hashVisible']} join.foldVisible="
                    f"{stats_j['foldVisible']} join.hash="
                    f"{stats_j['hashVisible']}")
                phase(False, "byte_identical_convergence", detail)
                raise ProofFailure(detail)
            phase(True, "byte_identical_convergence",
                  f"both processes fold {stats_c['foldVisible']} confirmed "
                  f"authored ticks to the IDENTICAL hash "
                  f"{stats_c['hashVisible']}")

            report["verdict"] = "PASS"
            report["detail"] = (
                f"CONVERGED: two SEPARATE processes, each booting the "
                f"VISIBLE engine over the real cloud origin, agree "
                f"byte-for-byte on canonical race input hash "
                f"{stats_c['hashVisible']} over "
                f"{stats_c['foldVisible']} folded ticks "
                f"(create.racedTicks={stats_c['racedTicks']} "
                f"join.racedTicks={stats_j['racedTicks']})")
        except ProofFailure as error:
            report["verdict"] = "FAIL"
            report["detail"] = str(error)
        finally:
            for label, driver in (("create", creator), ("join", joiner)):
                if driver is None:
                    continue
                if "sessions" not in report:
                    report["sessions"] = {}
                if label not in report["sessions"]:
                    report["sessions"][label] = {
                        "exit_code": driver.proc.poll(),
                        "log": str(driver.log_path)}
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
    parser.add_argument("--ticks", type=int, default=TICKS)
    parser.add_argument("--timeout", type=float, default=240.0,
                        help="per-process wall-clock bound once the race "
                        "transport is ready (default: %(default)s)")
    parser.add_argument("--setup-timeout-ms", type=int, default=60000,
                        help="MDKR_APP_ONLINE_TIMEOUT_MS: bound on the "
                        "create/join/setup dance before the race (default: "
                        "%(default)s)")
    parser.add_argument("--pace-hz", type=int, default=30,
                        help="MDKR_APP_ONLINE_PACE_HZ: real-time pace cap on "
                        "authored-tick advance during the race, so a REAL "
                        "peer process's confirmations have wall-clock time "
                        "to land (default: %(default)s; matches the "
                        "compiled compatibility fixture's cadence)")
    parser.add_argument("--log-dir", type=Path, default=None)
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    if args.log_dir is None:
        args.log_dir = Path(tempfile.mkdtemp(prefix="mdkr-cloud-engine-"))

    print(f"cloud_two_process_engine_boot: build={args.build} "
          f"ticks={args.ticks} log_dir={args.log_dir}", flush=True)

    try:
        report = run(args)
    except ProofFailure as error:
        print(f"cloud_two_process_engine_boot: FAIL -- {error}",
              file=sys.stderr)
        return 1

    print()
    print(f"cloud_two_process_engine_boot: {report['verdict']} -- "
          f"{report['detail']}")
    print(f"  elapsed: {report['elapsed_s']:.1f}s")
    print(f"  logs: {args.log_dir}")
    return 0 if report["verdict"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
