#!/usr/bin/env python3
"""Forensics lane: a forced peer loss must leave a readable failure-ring dump.

The harness (tests/test_net_failure_ring_impairment.cpp) runs two real
match-transport endpoints across a seeded net_impairment carrier whose profile
carries a two-second outage. The outage strands the receiving endpoint further
behind than the authored rollback window can replay, the transport latches its
terminal recovery, and the launcher-side decision resolves the peer as lost --
then dumps the ring's tail beside the state-hash evidence artifact.

This lane reads that dump and requires it to carry the two things an owner
needs to explain the loss:

  * the typed loss itself, by NAME (``kind=peer_lost ... code=ping_timeout``),
    not an enum ordinal; and
  * the last stall record before it, carrying the per-peer RTT / jitter / byte
    snapshot that says what the link was doing while progress stopped.

It also refuses a dump that leaks endpoint material: every code field must have
survived the ring's redaction filter.

POSITIVE CONTROL. The identical scenario is built a second time with the ring
compiled out (MDKR_NET_FAILURE_RING_DISABLED). That arm runs the same race to
the same peer loss and is required to FAIL these assertions -- if it passes,
the assertions are not reading the recording and this lane proves nothing.

Usage:
    check_net_failure_ring_impairment.py --record <binary> --norecord <binary>
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

RECORD_RE = re.compile(
    r"^\[NETFAIL\] seq=(?P<seq>\d+) tick=(?P<tick>\d+) ms=(?P<ms>\d+) "
    r"kind=(?P<kind>[a-z_]+) slot=(?P<slot>\d+) detail=(?P<detail>\d+) "
    r"a=(?P<a>\d+) b=(?P<b>\d+) code=(?P<code>\S+)(?P<tail>.*)$")
PEER_SNAPSHOT_RE = re.compile(
    r"peer(?P<peer>\d)=rtt(?P<rtt>\d+)/jit(?P<jit>\d+)/tx(?P<tx>\d+)/"
    r"rx(?P<rx>\d+)")
STALL_KINDS = {"stall_begin", "stall_ongoing", "stall_end", "progress_watchdog"}
# A code field that ever carried an address or key material would have to hold
# one of these; the ring's filter refuses the whole source instead.
FORBIDDEN_IN_CODE = (":", "/", "@", ".", " ")


class LaneFailure(Exception):
    """The dump did not carry what a forensic reader needs."""


def run_arm(binary: Path, run_dir: Path) -> Path:
    """Run one arm in its own directory and return the expected dump path."""
    artifact = run_dir / "state_hash.txt"
    binary = binary.resolve()
    if not binary.exists():
        raise LaneFailure(f"no harness binary at {binary}")
    env = dict(os.environ)
    env["MDKR_STATE_HASH_FILE"] = str(artifact)
    # The harness never boots the engine, but the suite's per-task pin must
    # survive into every process this lane launches.
    env["MDKR_SAVE_DIR"] = str(run_dir / "save")
    (run_dir / "save").mkdir(parents=True, exist_ok=True)
    completed = subprocess.run(
        [str(binary)], cwd=str(run_dir), env=env, timeout=120,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if completed.returncode != 0:
        raise LaneFailure(
            f"{binary.name} exited {completed.returncode}:\n{completed.stdout}")
    if "[IMPAIR] peer_lost" not in completed.stdout:
        raise LaneFailure(
            f"{binary.name} never forced the peer loss:\n{completed.stdout}")
    return Path(str(artifact) + ".netfail")


def parse(dump: Path) -> list[dict]:
    if not dump.exists():
        raise LaneFailure(f"no dump at {dump}")
    records = []
    for line in dump.read_text(encoding="ascii").splitlines():
        match = RECORD_RE.match(line)
        if match is not None:
            records.append(match.groupdict())
    if not records:
        raise LaneFailure(f"dump {dump} carries no records")
    return records


def verify(dump: Path) -> str:
    """Raise LaneFailure unless the dump explains the loss. Returns a summary."""
    records = parse(dump)
    sequences = [int(record["seq"]) for record in records]
    if sequences != sorted(sequences):
        raise LaneFailure("dump is not ordered oldest record first")

    losses = [r for r in records if r["kind"] == "peer_lost"]
    if not losses:
        raise LaneFailure("dump names no peer loss")
    loss = losses[-1]
    if loss["code"] != "ping_timeout":
        raise LaneFailure(
            f"peer loss carries code {loss['code']!r}, not the reason name")

    stalls = [r for r in records
              if r["kind"] in STALL_KINDS
              and int(r["seq"]) < int(loss["seq"])]
    if not stalls:
        raise LaneFailure("dump carries no stall record before the peer loss")
    last_stall = stalls[-1]
    peers = [m.groupdict() for m in PEER_SNAPSHOT_RE.finditer(last_stall["tail"])]
    if not peers:
        raise LaneFailure("the last stall record carries no per-peer snapshot")
    if not any(int(peer["rtt"]) > 0 and int(peer["tx"]) > 0 for peer in peers):
        raise LaneFailure(
            "the last stall record's snapshot is empty (no RTT and no bytes)")

    for record in records:
        if record["code"] == "-":
            continue
        if any(bad in record["code"] for bad in FORBIDDEN_IN_CODE):
            raise LaneFailure(
                f"code field {record['code']!r} escaped the redaction filter")

    return (f"{len(records)} records, loss code={loss['code']} at tick "
            f"{loss['tick']}, last stall {last_stall['kind']} at tick "
            f"{last_stall['tick']}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--record", required=True, type=Path,
                        help="the impairment harness built WITH recording")
    parser.add_argument("--norecord", required=True, type=Path,
                        help="the same harness with the ring compiled out "
                             "(the positive control)")
    args = parser.parse_args()

    with tempfile.TemporaryDirectory(prefix="net_failure_ring_") as root:
        record_dir = Path(root) / "record"
        control_dir = Path(root) / "norecord"
        record_dir.mkdir()
        control_dir.mkdir()
        try:
            summary = verify(run_arm(args.record, record_dir))
        except LaneFailure as failure:
            print(f"FAIL: recording arm: {failure}")
            return 1
        try:
            control_summary = verify(run_arm(args.norecord, control_dir))
        except LaneFailure as failure:
            print(f"  positive control failed as required: {failure}")
        else:
            print("FAIL: the recording-disabled arm passed these assertions "
                  f"({control_summary}); they do not read the recording")
            return 1

    print(f"check_net_failure_ring_impairment: PASS -- {summary}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
