#!/usr/bin/env python3
"""Two-session online smoke test against the REAL cloud party origin.

tests/check_online_live_transport_e2e.py proves the full online race stack
end to end, but it always stands up a LOCAL `wrangler dev --local` worker and
points both `mdkr_online_live_transport_e2e_driver` processes at
`http://127.0.0.1:PORT`. That is deliberately hermetic and says nothing about
the actual deployed edge.

This tool runs the SAME two-process driver flow (one `create`, one
`join`-by-code) but against the real production origin --
`https://party.goldenballoon.net` by default -- so connection problems that
only show up against the live Cloudflare Worker + Durable Object + TURN
config can be reproduced on demand.

Both sessions run on THIS machine. That is enough to exercise:
  * the cloud HTTP lobby routes (create / join-by-code / command),
  * the authenticated `/connect` state WebSocket,
  * the O-T1 match-signal client over `/api/match/{roomId}/signal` (real
    WSS to the real Worker),
  * the O-T2 peer mesh's SDP/ICE exchange carried over that real signal path,
  * the O-T3 live adapter's verification-phrase handshake and descriptor
    install,
  * the O-T6 per-tick race feed and its convergence check.

It does NOT exercise real cross-network NAT traversal: both peers sit behind
the same local network stack, so the DataChannel media path is effectively
loopback (host or srflx candidates resolving to the same box) regardless of
what the cloud origin advertises as ICE servers. Treat a PASS here as "cloud
SIGNALING + full lobby-state flow works", not as a NAT-traversal proof.

No production code path is weakened by this tool: the driver's `--origin`
argument already accepts `https://`/`wss://` origins unchanged (see
platform/online/match_live_transport.cpp `parseOrigin`); only `http://`/
`ws://` loopback origins are gated behind `MDKR_INTERNAL_TEST_TOKEN`, and
this tool never sets that variable. This script adds NO driver flag; it just
drives the existing `--origin` with a real HTTPS value and gives that value
an operator-friendly name (`--party-origin` / `MDKR_PARTY_ORIGIN`).

Usage:
    python3 tools/online/cloud_two_session_smoke.py \\
        --build-dir build-cloud-harness \\
        --party-origin https://party.goldenballoon.net \\
        --ticks 300 --timeout 150 \\
        --log-dir /path/to/logs -v

One-time build-dir setup (not done by this script -- machine-specific
FetchContent source paths):

    export PKG_CONFIG_PATH=<sdl2 pkgconfig dir>
    cmake -S . -B <build-dir> -G Ninja -DCMAKE_BUILD_TYPE=Release \\
        -DMDKR_ENABLE_ONLINE_BETA=ON -DMDKR_NATIVE_PHONE_PARTY=ON \\
        -DFETCHCONTENT_SOURCE_DIR_MDKR_MBEDTLS=<path>/mdkr_mbedtls-src \\
        -DFETCHCONTENT_SOURCE_DIR_MDKR_LIBDATACHANNEL=<path>/mdkr_libdatachannel-src \\
        -DFETCHCONTENT_SOURCE_DIR_WGPU_NATIVE=<path>/wgpu_native-src

This script's --build step only runs `cmake --build <build-dir> --target
mdkr_online_live_transport_e2e_driver`, which requires that one-time
configure to already have happened.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
import threading
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable, Optional

ROOT = Path(__file__).resolve().parent.parent.parent
DEFAULT_PARTY_ORIGIN = "https://party.goldenballoon.net"
NATIVE_USER_AGENT = "GoldenBalloon/1.6.0"  # matches match_signal_client.cpp

ROOM = re.compile(r"^\[E2E\] room=(\S+) code=(\d{6})$")
PHRASE = re.compile(r"^\[E2E\] phrase=(.+)$")
INSTALLED = re.compile(r"^\[E2E\] installed epoch=(\d+) firstTick=(\d+) .*$")
RESULT_OK = re.compile(r"^\[E2E\] result=ok ticks=(\d+) hash=([0-9a-f]{16})$")
RESULT_ERR = re.compile(r"^\[E2E\] result=(error|timeout)\b(.*)$")
MEMBERS2 = re.compile(r"^\[E2E\] members=2$")
READY2 = re.compile(r"^\[E2E\] ready=2$")


class SmokeFailure(RuntimeError):
    """Raised for any harness-level failure; message is the report."""


@dataclass
class PhaseLog:
    """One named checkpoint in the flow, timestamped relative to session
    start, so the final report can say exactly where things stopped."""
    name: str
    ok: bool
    detail: str
    elapsed_s: float


@dataclass
class SessionResult:
    name: str
    args: list
    log_path: Path
    exit_code: Optional[int] = None
    phases: list = field(default_factory=list)
    error: Optional[str] = None


class Driver:
    """A narrated `mdkr_online_live_transport_e2e_driver` process, tailing
    its stdout/stderr (merged) to both an in-memory buffer and a log file on
    disk in real time."""

    def __init__(self, binary: Path, args: list, name: str, log_path: Path,
                 verbose: bool):
        self.name = name
        self.log_path = log_path
        self.start_time = time.monotonic()
        environment = dict(os.environ)
        # Deliberately NOT setting MDKR_INTERNAL_TEST_TOKEN: that token only
        # ever unlocks plain-HTTP loopback origins (match_live_transport.cpp
        # parseOrigin / mdkr_party_loopback_test_url_allowed). An https://
        # origin needs no token and must not get one, so the real fail-closed
        # HTTPS-only posture is exercised exactly as production sees it.
        environment.pop("MDKR_INTERNAL_TEST_TOKEN", None)
        self.verbose = verbose
        self._log_file = log_path.open("w", encoding="utf-8")
        self.proc = subprocess.Popen(
            [str(binary), *args], stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, env=environment, text=True, bufsize=1)
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

    def wait_line(self, predicate: Callable[[str], object], description: str,
                  timeout: float):
        """Returns the truthy predicate value (e.g. a regex match) or raises
        SmokeFailure."""
        deadline = time.monotonic() + timeout
        scanned = 0
        while time.monotonic() < deadline:
            lines = self.snapshot()
            for index in range(scanned, len(lines)):
                line = lines[index]
                err = RESULT_ERR.match(line)
                if err:
                    raise SmokeFailure(
                        f"{self.name} failed before '{description}': "
                        f"{err.group(0)}")
                value = predicate(line)
                if value:
                    return value
            scanned = len(lines)
            if self.proc.poll() is not None and scanned == len(self.snapshot()):
                break
            time.sleep(0.05)
        raise SmokeFailure(
            f"{self.name} never printed '{description}' within {timeout:.0f}s; "
            f"last lines: {self.snapshot()[-12:]}")

    def wait_exit(self, timeout: float) -> int:
        try:
            return self.proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired as error:
            raise SmokeFailure(
                f"{self.name} did not exit within {timeout:.0f}s; "
                f"last lines: {self.snapshot()[-12:]}") from error

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


def build_driver(build_dir: Path, jobs: int, log_path: Path) -> None:
    cache = build_dir / "CMakeCache.txt"
    if not cache.is_file():
        raise SmokeFailure(
            f"{build_dir} is not a configured CMake build dir (no "
            "CMakeCache.txt). Run the one-time cmake configure described in "
            "this script's module docstring first.")
    cmd = ["cmake", "--build", str(build_dir),
           "--target", "mdkr_online_live_transport_e2e_driver",
           "-j", str(jobs)]
    with log_path.open("w", encoding="utf-8") as log:
        log.write(f"$ {' '.join(cmd)}\n")
        log.flush()
        result = subprocess.run(cmd, stdout=log, stderr=subprocess.STDOUT)
    if result.returncode != 0:
        raise SmokeFailure(
            f"build failed (exit {result.returncode}); see {log_path}")


def run_two_sessions(binary: Path, party_origin: str, ticks: int,
                     timeout: float, log_dir: Path, verbose: bool) -> dict:
    """Runs create+join against `party_origin` and returns a structured
    report dict. Never raises for a driver-level failure -- it captures the
    failure into the report so main() can print a precise verdict; it only
    raises SmokeFailure for harness-internal problems (e.g. missing binary)."""
    timeout_ms = int(timeout * 1000)
    create_log = log_dir / "session_create.log"
    join_log = log_dir / "session_join.log"

    report: dict = {
        "party_origin": party_origin,
        "user_agent": NATIVE_USER_AGENT,
        "ticks_requested": ticks,
        "timeout_s": timeout,
        "sessions": {},
        "phases": [],
        "verdict": None,
        "detail": None,
    }

    def phase(ok: bool, name: str, detail: str) -> None:
        report["phases"].append({"phase": name, "ok": ok, "detail": detail})
        marker = "OK  " if ok else "FAIL"
        print(f"  [{marker}] {name}: {detail}", flush=True)

    creator = joiner = None
    overall_start = time.monotonic()
    try:
        creator = Driver(
            binary, ["--origin", party_origin, "--journey", "create",
                     "--character", "1", "--track", "5", "--ticks",
                     str(ticks), "--timeout-ms", str(timeout_ms)],
            "create", create_log, verbose)

        try:
            room = creator.wait_line(
                ROOM.match, "room created (cloud HTTP create + /connect WS)",
                30.0)
        except SmokeFailure as error:
            phase(False, "cloud_create", str(error))
            raise
        code = room.group(2)
        phase(True, "cloud_create",
              f"room={room.group(1)} code={code} "
              f"(cloud signaling: HTTP create + /connect WS both succeeded)")

        joiner = Driver(
            binary, ["--origin", party_origin, "--journey", "join",
                     "--join-code", code, "--character", "2", "--track", "5",
                     "--ticks", str(ticks), "--timeout-ms", str(timeout_ms)],
            "join", join_log, verbose)

        try:
            joiner.wait_line(lambda l: l == "[E2E] joined",
                             "join accepted by cloud", 30.0)
            phase(True, "cloud_join",
                  "joiner's cloud HTTP join-by-code + /connect WS succeeded")
        except SmokeFailure as error:
            phase(False, "cloud_join", str(error))
            raise

        try:
            creator.wait_line(MEMBERS2.match, "creator sees 2 members", 30.0)
            joiner.wait_line(MEMBERS2.match, "joiner sees 2 members", 30.0)
            phase(True, "lobby_state_sync",
                  "both endpoints observe member_count>=2 over the cloud "
                  "/connect WebSocket (real-time lobby state fan-out works)")
        except SmokeFailure as error:
            phase(False, "lobby_state_sync", str(error))
            raise

        try:
            phrase_c = creator.wait_line(PHRASE.match,
                                         "verification phrase", 60.0)
            phrase_j = joiner.wait_line(PHRASE.match,
                                        "verification phrase", 60.0)
        except SmokeFailure as error:
            phase(False, "webrtc_mesh_signaling", str(error))
            raise
        if phrase_c.group(1).strip() != phrase_j.group(1).strip():
            detail = (f"verification phrases diverged: "
                      f"create={phrase_c.group(1)!r} "
                      f"join={phrase_j.group(1)!r}")
            phase(False, "webrtc_mesh_signaling", detail)
            raise SmokeFailure(detail)
        phase(True, "webrtc_mesh_signaling",
              f"both peers negotiated real WebRTC DataChannels over the "
              f"cloud /api/match/signal path and agree on SAS phrase "
              f"{phrase_c.group(1).strip()!r} (DTLS handshake + libdatachannel "
              f"mesh confirmed)")

        try:
            creator.wait_line(INSTALLED.match, "descriptor install", 60.0)
            joiner.wait_line(INSTALLED.match, "descriptor install", 60.0)
            phase(True, "descriptor_install",
                  "preflight consensus reached and match descriptor installed "
                  "on both endpoints")
        except SmokeFailure as error:
            phase(False, "descriptor_install", str(error))
            raise

        try:
            result_c = creator.wait_line(RESULT_OK.match, "race result",
                                         timeout)
            result_j = joiner.wait_line(RESULT_OK.match, "race result",
                                        timeout)
        except SmokeFailure as error:
            phase(False, "race_convergence", str(error))
            raise
        ticks_c, hash_c = int(result_c.group(1)), result_c.group(2)
        ticks_j, hash_j = int(result_j.group(1)), result_j.group(2)
        if ticks_c < ticks or ticks_j < ticks:
            detail = (f"race did not reach {ticks} confirmed ticks: "
                      f"create={ticks_c} join={ticks_j}")
            phase(False, "race_convergence", detail)
            raise SmokeFailure(detail)
        if hash_c != hash_j:
            detail = (f"STATE HASH MISMATCH after {ticks_c}/{ticks_j} ticks: "
                      f"create={hash_c} join={hash_j}")
            phase(False, "race_convergence", detail)
            raise SmokeFailure(detail)
        phase(True, "race_convergence",
              f"{ticks_c} ticks raced over the (locally-looped-back) "
              f"DataChannel mesh; both endpoints agree on state hash "
              f"{hash_c}")

        exit_c = creator.wait_exit(15)
        exit_j = joiner.wait_exit(15)
        report["sessions"]["create"] = {
            "exit_code": exit_c, "log": str(create_log)}
        report["sessions"]["join"] = {
            "exit_code": exit_j, "log": str(join_log)}
        if exit_c != 0 or exit_j != 0:
            report["verdict"] = "FAIL"
            report["detail"] = (
                f"race converged but a process exited non-zero: "
                f"create={exit_c} join={exit_j}")
        else:
            report["verdict"] = "PASS"
            report["detail"] = (
                f"CONVERGED: two live processes raced {ticks_c} ticks over "
                f"real cloud signaling + WebRTC and agree on state hash "
                f"{hash_c} (phrase={phrase_c.group(1).strip()!r})")
    except SmokeFailure as error:
        report["verdict"] = "FAIL"
        report["detail"] = str(error)
    finally:
        for label, driver in (("create", creator), ("join", joiner)):
            if driver is None:
                continue
            if label not in report["sessions"]:
                report["sessions"][label] = {
                    "exit_code": driver.proc.poll(), "log": str(driver.log_path)}
            try:
                driver.close()
            except OSError:
                pass
    report["elapsed_s"] = time.monotonic() - overall_start
    return report


def resolve_build_dir(value: str) -> Path:
    path = Path(value).expanduser()
    return (path if path.is_absolute() else ROOT / path).resolve()


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--build-dir", default="build-cloud-harness",
                        help="CMake build dir containing (or that will "
                        "contain) the driver binary")
    parser.add_argument("--build", action="store_true",
                        help="run `cmake --build` for the driver target "
                        "before the smoke run (build dir must already be "
                        "configured -- see module docstring)")
    parser.add_argument("--jobs", type=int, default=6)
    parser.add_argument(
        "--party-origin",
        default=os.environ.get("MDKR_PARTY_ORIGIN", DEFAULT_PARTY_ORIGIN),
        help="cloud party origin both sessions connect to (default: "
        "%(default)s; also settable via MDKR_PARTY_ORIGIN). Passed straight "
        "through as the driver's existing --origin argument.")
    parser.add_argument("--ticks", type=int, default=300,
                        help="confirmed authored ticks to race (default: "
                        "%(default)s; the full local e2e lane uses 1800)")
    parser.add_argument("--timeout", type=float, default=150.0,
                        help="per-phase / overall race budget in seconds")
    parser.add_argument("--log-dir", default=None,
                        help="directory for session + summary logs "
                        "(default: a mdkr-cloud-smoke-<timestamp> dir under "
                        "the system temp dir)")
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    build_dir = resolve_build_dir(args.build_dir)
    binary = build_dir / "mdkr_online_live_transport_e2e_driver"

    if args.log_dir:
        log_dir = Path(args.log_dir).expanduser().resolve()
    else:
        import tempfile
        log_dir = Path(tempfile.mkdtemp(prefix="mdkr-cloud-smoke-"))
    log_dir.mkdir(parents=True, exist_ok=True)

    print(f"cloud_two_session_smoke: party_origin={args.party_origin} "
          f"binary={binary} log_dir={log_dir}", flush=True)

    try:
        if args.build:
            build_log = log_dir / "build.log"
            print(f"building mdkr_online_live_transport_e2e_driver "
                  f"(log: {build_log}) ...", flush=True)
            build_driver(build_dir, args.jobs, build_log)

        if not binary.is_file():
            raise SmokeFailure(
                f"missing {binary}; pass --build (with a configured "
                f"--build-dir) or build it yourself first")

        report = run_two_sessions(binary, args.party_origin, args.ticks,
                                  args.timeout, log_dir, args.verbose)
    except SmokeFailure as error:
        print(f"cloud_two_session_smoke: FAIL -- {error}", file=sys.stderr)
        return 1

    summary_path = log_dir / "summary.json"
    summary_path.write_text(json.dumps(report, indent=2), encoding="utf-8")

    print()
    print(f"cloud_two_session_smoke: {report['verdict']} -- {report['detail']}")
    print(f"  elapsed: {report['elapsed_s']:.1f}s")
    print(f"  logs: {log_dir}")
    return 0 if report["verdict"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
