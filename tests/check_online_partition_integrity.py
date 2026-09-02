#!/usr/bin/env python3
"""Prove a stale post-rollback object-list boundary never draws a spectate camera.

A solo online endpoint simulates the canonical N-player layout while presenting
one local viewport. The scene draws only the object-list tail
[gObjSortFirstActive, gObjectCount); the level's spectate BHV_CAMERA_CONTROL
objects live ahead of that boundary (get_first_active_object partitions them out)
and are never drawn.

The rollback snapshot restores gObjPtrList, gObjectCount and gObjectListStart, but
NOT the cached partition boundary gFirstActiveObjectId -- the free path lowers it
as front objects retire, and a deep catch-up restore re-materialises those objects
without raising the boundary again. The render tail then spills into the front
partition and the spectate cameras get drawn (owner-observed "floating cameras");
walking a recycled entry there can also feed a wild display-list pointer into the
walk (a rare SIGSEGV). Same root, two symptoms.

MDKR_TEST_PARTITION_STALE_AT reproduces exactly that corrupt state -- boundary
dropped below the live active region over a still-valid list -- deterministically
and offline. This gate runs the in-process visible online race with the corruption
injected and asserts:

  * DISABLED arm (control): with the fix off, the injected cameras really do reach
    render_object -- the injection is non-vacuous.
  * FIXED arm: with the fix on, the SAME corruption is still present (the render
    tail still holds the cameras) yet ZERO reach render_object -- the scene filter
    closes it -- and the two endpoints still converge byte-for-byte.
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

from harness_utils import DEFAULT_BUILD_DIR, resolve_binary

ROOT = Path(__file__).resolve().parent.parent
TICKS = 1500
INJECT_AT = 100

ENGINE_LIVE_RE = re.compile(
    r"^\[ENGINE-ONLINE-LIVE\] result=(-?\d+) .* "
    r"hashVisible=([0-9a-f]{16}) hashPeer=([0-9a-f]{16}) converged=(\d+)$",
    re.MULTILINE,
)
INJECT_RE = re.compile(r"^\[PARTITION-TRACE\] INJECTED stale boundary", re.MULTILINE)
TAIL_RE = re.compile(r"holds spectateCameras=(\d+)", re.MULTILINE)
REACHED_RE = re.compile(
    r"spectate BHV_CAMERA_CONTROL reached render_object", re.MULTILINE)


def fail(message: str, output: str = "") -> int:
    print(f"FAIL online partition integrity: {message}", file=sys.stderr)
    if output:
        print(output[-12000:], file=sys.stderr)
    return 1


def clean_environment(**updates: str) -> dict[str, str]:
    environment = {
        key: value for key, value in os.environ.items()
        if not key.startswith(("MDKR", "GE007_"))
    }
    environment.update(updates)
    return environment


def run_arm(binary: Path, rom: Path, ticks: int, timeout: int,
            disable_fix: bool) -> tuple[int, str]:
    with tempfile.TemporaryDirectory(prefix="mdkr64-partition-") as temp:
        run_dir = Path(temp)
        (run_dir / "saves").mkdir()
        (run_dir / "preferences").mkdir()
        environment = clean_environment(
            LC_ALL="C",
            MDKR_APP_AUTOPLAY="1",
            MDKR_APP_TEST_ONLINE_LIVE="1",
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
            MDKR_TEST_PARTITION_TRACE="1",
            MDKR_TEST_PARTITION_STALE_AT=str(INJECT_AT),
            MDKR_VIDEO_CONFIG_PATH=str(run_dir / "video.ini"),
            MDKR64_HIDDEN="1",
        )
        if disable_fix:
            environment["MDKR_TEST_DISABLE_PARTITION_FIX"] = "1"
        process = subprocess.run(
            [str(binary)], cwd=run_dir, env=environment, text=True,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            timeout=timeout, check=False,
        )
        return process.returncode, process.stdout or ""


def converged(output: str) -> bool:
    stats = ENGINE_LIVE_RE.findall(output)
    if len(stats) != 1:
        return False
    result, hash_visible, hash_peer, conv = stats[0]
    return int(result) == 0 and int(conv) == 1 and hash_visible == hash_peer


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", default=DEFAULT_BUILD_DIR)
    parser.add_argument("--rom", type=Path, default="baserom.us.v80.z64")
    parser.add_argument("--ticks", type=int, default=TICKS)
    parser.add_argument("--timeout", type=int, default=300)
    args = parser.parse_args()

    binary = Path(resolve_binary(args.build)).expanduser().resolve()
    rom = args.rom.expanduser().resolve()
    for path, label in ((binary, "binary"), (rom, "ROM")):
        if not path.is_file():
            parser.error(f"missing {label}: {path}")

    # Control arm: fix disabled. The injection must actually draw cameras, or the
    # gate is vacuous.
    code, out = run_arm(binary, rom, args.ticks, args.timeout, disable_fix=True)
    for marker in ("[FATAL]", "[CRASH]", "AddressSanitizer"):
        if marker in out:
            return fail(f"control arm hit {marker!r}", out)
    if code != 0:
        return fail(f"control arm exited {code}", out)
    if not INJECT_RE.search(out):
        return fail("control arm never injected the stale boundary "
                    "(online race not reached?)", out)
    control_reached = len(REACHED_RE.findall(out))
    if control_reached <= 0:
        return fail("control arm drew ZERO spectate cameras -- the injection is "
                    "vacuous, so the fixed arm would prove nothing", out)

    # Fixed arm: the corruption is still present but no camera may be drawn.
    code, out = run_arm(binary, rom, args.ticks, args.timeout, disable_fix=False)
    for marker in ("[FATAL]", "[CRASH]", "AddressSanitizer"):
        if marker in out:
            return fail(f"fixed arm hit {marker!r}", out)
    if code != 0:
        return fail(f"fixed arm exited {code}", out)
    if not INJECT_RE.search(out):
        return fail("fixed arm never injected the stale boundary", out)
    tail_hits = [int(n) for n in TAIL_RE.findall(out)]
    if not any(n > 0 for n in tail_hits):
        return fail("fixed arm shows the corruption was NOT present (render tail "
                    "never held a spectate camera) -- cannot prove the filter is "
                    "what closes it", out)
    fixed_reached = len(REACHED_RE.findall(out))
    if fixed_reached != 0:
        return fail(f"fixed arm still drew {fixed_reached} spectate camera(s) "
                    "-- the scene filter did not close the floating cameras", out)
    if not converged(out):
        return fail("fixed arm did not converge byte-for-byte with its peer "
                    "(the fix perturbed the canonical fold)", out)

    print(
        "PASS online partition integrity: a stale post-rollback boundary put the "
        f"level's spectate cameras into the render tail (control drew "
        f"{control_reached}, fix drew 0 over {max(tail_hits)} exposed-per-tick), "
        "and the two endpoints still converged byte-for-byte."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
