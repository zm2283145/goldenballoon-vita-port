#!/usr/bin/env python3
"""End-to-end online multiplayer: two native processes race over the live stack.

This is the O-T6 capstone: the first time real match bytes cross a wire. A live
local MatchRoom Worker (``wrangler dev --local`` with real Durable Objects) plus
TWO ``mdkr_online_live_transport_e2e_driver`` processes -- one creator, one
joiner-by-code -- run the entire production online path against each other:

* real HTTP create/join/code/command over the MatchRoom lobby routes and the
  authenticated ``/connect`` state WebSocket (credential in the subprotocol);
* the O-T1 match-signal client over ``/api/match/{roomId}/signal``;
* the O-T2 peer mesh negotiating real WebRTC DataChannels (libdatachannel DTLS);
* the O-T3 live adapter: selections, the transcript verification phrase
  (auto-confirmed under the test token), preflight consensus, and the descriptor
  install through the O-T5 retail-identity clamp;
* the O-T6 per-tick feed: each side seals its local input into 3-frame bundles,
  fans them out on the mesh, feeds the opened remote bundles to
  ``mdkr_match_transport_receive`` and drains authored ticks.

Each process races >=1800 confirmed authored ticks and prints the FNV state hash
of its own confirmed canonical-input timeline. The check asserts BOTH phrases
matched, BOTH descriptors installed and the two end-of-race state hashes are
byte-identical -- the headline convergence verdict.

DEV lane, not release-required. The drivers speak plain ``ws://``/``http://`` to
the loopback Worker under ``MDKR_INTERNAL_TEST_TOKEN=mdkr64-party-e2e-v1`` (the
same loopback-transport token the party e2e and signal-client tests use); with
no token the transport's HTTPS/WSS-only posture is unchanged.
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
from typing import Callable

from check_browser_online_two_person import free_port
from check_browser_runtime import CheckFailure, require
from check_party_capacity import start_worker, stop_worker

ROOT = Path(__file__).resolve().parent.parent
TEST_TOKEN = "mdkr64-party-e2e-v1"

ROOM = re.compile(r"^\[E2E\] room=(\S+) code=(\d{6})$")
PHRASE = re.compile(r"^\[E2E\] phrase=(.+)$")
INSTALLED = re.compile(r"^\[E2E\] installed epoch=(\d+) firstTick=(\d+) .*$")
RESULT_OK = re.compile(r"^\[E2E\] result=ok ticks=(\d+) hash=([0-9a-f]{16})$")
RESULT_ERR = re.compile(r"^\[E2E\] result=(error|timeout)\b.*$")


def resolve(value: str) -> Path:
    path = Path(value).expanduser()
    return (path if path.is_absolute() else ROOT / path).resolve()


class Driver:
    """A narrated online race process."""

    def __init__(self, binary: Path, args: list[str], name: str, verbose: bool):
        environment = dict(os.environ)
        environment["MDKR_INTERNAL_TEST_TOKEN"] = TEST_TOKEN
        self.name = name
        self.proc = subprocess.Popen(
            [str(binary), *args], stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, env=environment, text=True, bufsize=1)
        self.verbose = verbose
        self.lines: list[str] = []
        self.lock = threading.Lock()
        self.thread = threading.Thread(target=self._pump, daemon=True)
        self.thread.start()

    def _pump(self) -> None:
        assert self.proc.stdout is not None
        for line in self.proc.stdout:
            line = line.rstrip("\n")
            if self.verbose:
                print(f"{self.name}: {line}", flush=True)
            with self.lock:
                self.lines.append(line)

    def snapshot(self) -> list[str]:
        with self.lock:
            return list(self.lines)

    def wait_line(self, predicate: Callable[[str], object], description: str,
                  timeout: float) -> object:
        deadline = time.monotonic() + timeout
        scanned = 0
        while time.monotonic() < deadline:
            lines = self.snapshot()
            for index in range(scanned, len(lines)):
                for line in (lines[index],):
                    if (err := RESULT_ERR.match(line)):
                        raise CheckFailure(
                            f"{self.name} failed before {description}: {err.group(0)}")
                    value = predicate(lines[index])
                    if value:
                        return value
            scanned = len(lines)
            if self.proc.poll() is not None and scanned == len(self.snapshot()):
                break
            time.sleep(0.05)
        raise CheckFailure(
            f"{self.name} never printed {description}; last lines: "
            f"{self.snapshot()[-10:]}")

    def wait_exit(self, timeout: float) -> int:
        try:
            return self.proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired as error:
            raise CheckFailure(
                f"{self.name} did not exit; last lines: {self.snapshot()[-10:]}"
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


def race_once(binary: Path, origin: str, ticks: int, timeout: float,
              verbose: bool) -> str:
    """Run one full two-process race. Returns the convergence verdict line."""
    creator = joiner = None
    timeout_ms = int(timeout * 1000)
    try:
        creator = Driver(
            binary, ["--origin", origin, "--journey", "create",
                     "--character", "1", "--track", "5", "--ticks", str(ticks),
                     "--timeout-ms", str(timeout_ms)], "create", verbose)
        room = creator.wait_line(ROOM.match, "room=/code=", 30.0)
        code = room.group(2)
        joiner = Driver(
            binary, ["--origin", origin, "--journey", "join", "--join-code",
                     code, "--character", "2", "--track", "5", "--ticks",
                     str(ticks), "--timeout-ms", str(timeout_ms)], "join",
            verbose)

        phrase_c = creator.wait_line(PHRASE.match, "verification phrase", 60.0)
        phrase_j = joiner.wait_line(PHRASE.match, "verification phrase", 60.0)
        require(phrase_c.group(1).strip() == phrase_j.group(1).strip(),
                f"verification phrases diverged: create={phrase_c.group(1)!r} "
                f"join={phrase_j.group(1)!r}")

        creator.wait_line(INSTALLED.match, "descriptor install", 60.0)
        joiner.wait_line(INSTALLED.match, "descriptor install", 60.0)

        race_deadline = timeout
        result_c = creator.wait_line(RESULT_OK.match, "race result", race_deadline)
        result_j = joiner.wait_line(RESULT_OK.match, "race result", race_deadline)
        ticks_c, hash_c = int(result_c.group(1)), result_c.group(2)
        ticks_j, hash_j = int(result_j.group(1)), result_j.group(2)

        require(ticks_c >= ticks and ticks_j >= ticks,
                f"race did not reach {ticks} confirmed ticks: "
                f"create={ticks_c} join={ticks_j}")
        require(hash_c == hash_j,
                f"STATE HASH MISMATCH after {ticks_c}/{ticks_j} ticks: "
                f"create={hash_c} join={hash_j}")

        require(creator.wait_exit(15) == 0, "creator exited non-zero")
        require(joiner.wait_exit(15) == 0, "joiner exited non-zero")
        return (f"CONVERGED: two live processes raced {ticks_c} ticks over real "
                f"WebRTC and agree on state hash {hash_c} "
                f"(phrase={phrase_c.group(1).strip()!r})")
    finally:
        for driver in (joiner, creator):
            if driver is not None:
                try:
                    driver.close()
                except OSError:
                    pass


def run(args: argparse.Namespace) -> None:
    build = resolve(args.build)
    binary = build / "mdkr_online_live_transport_e2e_driver"
    require(binary.is_file(),
            f"missing {binary}; build mdkr_online_live_transport_e2e_driver "
            "first (requires -DMDKR_NATIVE_PHONE_PARTY=ON)")
    shell = resolve(args.shell_dir)
    require((shell / "index.html").is_file(),
            "the online e2e lane needs the staged web shell for the Worker")

    with tempfile.TemporaryDirectory(prefix="mdkr-online-e2e-") as temp:
        root = Path(temp)
        log_path = root / "wrangler.log"
        verdict = ""
        with log_path.open("wb") as log:
            worker: subprocess.Popen[bytes] | None = None
            attempts = 2
            last_error: Exception | None = None
            for attempt in range(1, attempts + 1):
                port = free_port()
                origin = f"http://127.0.0.1:{port}"
                try:
                    worker = start_worker(origin, shell,
                                          root / f"state-{attempt}", log, 10_000)
                    verdict = race_once(binary, origin, args.ticks,
                                        args.timeout, args.verbose)
                    last_error = None
                    break
                except (CheckFailure, OSError, subprocess.SubprocessError) as error:
                    last_error = error
                    print(f"online e2e attempt {attempt} failed: {error}",
                          file=sys.stderr, flush=True)
                finally:
                    stop_worker(worker)
                    worker = None
            if last_error is not None:
                raise last_error
        details = log_path.read_text(encoding="utf-8", errors="replace")
        require("ERROR" not in details.upper(),
                "Wrangler reported an error during the online e2e lane")
    print(f"check_online_live_transport_e2e: PASS -- {verdict}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", default="build-rel")
    parser.add_argument("--shell-dir", default="dist/web")
    parser.add_argument("--ticks", type=int, default=1800)
    parser.add_argument("--timeout", type=float, default=180.0)
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()
    try:
        run(args)
        return 0
    except (CheckFailure, OSError, ValueError,
            subprocess.SubprocessError) as error:
        print(f"check_online_live_transport_e2e: FAIL -- {error}",
              file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
