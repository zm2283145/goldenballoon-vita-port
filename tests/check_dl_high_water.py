#!/usr/bin/env python3
"""AP-19 display-list high-water witness and its fail-closed assertion.

``gDisplayLists[]`` is a single allocation whose Gfx region is immediately
followed by ``gMatrixHeap[]`` (``alloc_displaylist_heap``, thread3_main.c).
Authoring past ``gCurrNumF3dCmdsPerPlayer`` commands therefore does not fault:
it overwrites matrices, and both display-list walkers go on to parse matrix
words as commands. That silent corruption is the four-viewport party-hub
overflow AddressSanitizer settled, and nothing measured the margin before it.

The engine now measures the authored length at the one place it is known --
task submission -- and reports every new high through
``[TRACE] dl_high_water: bytes=.. limit=..``. Exceeding the row aborts.

Arms
----
* **A -- the witness exists and the retail 1P route stays inside its row.**
  A one-player time trial must emit at least one witness line, every reported
  length must be below the reported limit, and the limit must be one of the
  retail rows ``gNumF3dCmdsPerPlayer[]`` holds. Without the counter there is no
  line to read and this arm fails.
* **B -- the assertion is fail-closed.** The same route with
  ``MDKR_TEST_DL_HIGH_WATER_LIMIT`` lowered below what the route authors must
  abort: the process exits nonzero having printed the ``[FATAL]`` line, with a
  witness for the length that tripped it. A counter that only printed would
  pass arm A and fail here.

The four-player soak evidence -- twenty world-lobby->race->world-lobby cycles
and five Taj rebuilds against the budget row -- lives with the rest of the
AP-19 accounting in ``check_adventure_party_performance.py`` and
``check_adventure_party_performance_soaks.py``.

Every launch is headless and muted. Exit 0 = pass.
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

from check_adventure_party_admission import eeprom_image
from harness_utils import DEFAULT_BUILD_DIR, resolve_binary, save_env

ROOT = Path(__file__).resolve().parent.parent
SOLO_SCRIPT = ROOT / "tests" / "input_scripts" / "nav_to_time_trial_race.txt"
SOLO_FRAMES = 1800

# gNumF3dCmdsPerPlayer[] (game/src/thread3_main.c) in bytes: the only rows a
# display-list buffer is ever allocated to.
GFX_BYTES = 8
RETAIL_ROW_BYTES = {4500 * GFX_BYTES, 7000 * GFX_BYTES, 11000 * GFX_BYTES}
# Well under the ~4400 commands a retail 1P race authors, so the route trips
# the assertion without any misauthored stream.
FORCED_LIMIT_COMMANDS = 64

HIGH_WATER_RE = re.compile(
    r"^\[TRACE\] dl_high_water: bytes=(\d+) limit=(\d+)$", re.MULTILINE)
FATAL_RE = re.compile(
    r"^\[FATAL\] display list overflowed its heap row "
    r"\(bytes=(\d+) limit=(\d+) commands=(-?\d+)\)$", re.MULTILINE)


def high_water_rows(output: str) -> list[tuple[int, int]]:
    return [(int(bytes_), int(limit))
            for bytes_, limit in HIGH_WATER_RE.findall(output)]


def run_route(binary: Path, rom: Path, root: Path, label: str,
              extra_env: dict[str, str], timeout: int,
              verbose: bool) -> tuple[int, str]:
    """One headless launch; returns its exit status and combined output."""
    run_dir = root / label
    save_dir = run_dir / "save"
    save_dir.mkdir(parents=True)
    (save_dir / "eeprom.bin").write_bytes(eeprom_image())
    env = {key: value for key, value in os.environ.items()
           if not key.startswith(("MDKR", "GE007_"))}
    env.update(LC_ALL="C", MDKR_AUDIO="0", MDKR64_HIDDEN="1", MDKR_TRACE="1",
               MDKR_AUTOPILOT="1", MDKR_LOAD_TRACK="5", MallocNanoZone="0")
    env.update(extra_env)
    save_env(env, str(save_dir))
    env["MDKR_VIDEO_CONFIG_PATH"] = str(run_dir / "mdkr64.ini")
    command = [
        str(binary), "--headless-frames", str(SOLO_FRAMES),
        "--input-script", str(SOLO_SCRIPT), "--rom", str(rom),
        "--window-size", "320x240",
    ]
    if verbose:
        print(f"$ ({label}) {' '.join(command)}", flush=True)
    process = subprocess.run(
        command, cwd=run_dir, env=env, text=True, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, timeout=timeout, check=False)
    return process.returncode, process.stdout or ""


def clean_failures(status: int, output: str) -> list[str]:
    failures = []
    if status != 0:
        failures.append(
            f"arm A: the retail 1P route exited {status}\n{output[-3000:]}")
    rows = high_water_rows(output)
    if not rows:
        failures.append(
            "arm A: no '[TRACE] dl_high_water: bytes=.. limit=..' line was "
            "emitted, so no display-list margin was measured at all")
        return failures
    limits = {limit for _bytes, limit in rows}
    if not limits <= RETAIL_ROW_BYTES:
        failures.append(
            f"arm A: the witness reported limits {sorted(limits)} bytes, which "
            f"are not retail display-list rows {sorted(RETAIL_ROW_BYTES)}; the "
            "counter is not reading the row the buffer was allocated to")
    over = [row for row in rows if row[0] >= row[1]]
    if over:
        failures.append(
            f"arm A: the retail 1P route reached or passed its row: {over}")
    if FATAL_RE.search(output):
        failures.append(
            "arm A: the well-authored route tripped the overflow assertion")
    return failures


def forced_failures(status: int, output: str) -> list[str]:
    failures = []
    if status == 0:
        failures.append(
            "arm B: the route exited 0 with the limit held to "
            f"{FORCED_LIMIT_COMMANDS} commands; the assertion is not "
            "fail-closed")
    fatal = FATAL_RE.search(output)
    if fatal is None:
        failures.append(
            "arm B: no '[FATAL] display list overflowed its heap row' line was "
            f"printed\n{output[-3000:]}")
        return failures
    authored, limit = int(fatal.group(1)), int(fatal.group(2))
    if limit != FORCED_LIMIT_COMMANDS * GFX_BYTES:
        failures.append(
            f"arm B: the assertion reported limit {limit} bytes, not the "
            f"{FORCED_LIMIT_COMMANDS * GFX_BYTES} the seam was set to")
    if authored <= limit:
        failures.append(
            f"arm B: the assertion fired at {authored} bytes, which does not "
            f"exceed its {limit}-byte limit")
    if not any(row[0] == authored for row in high_water_rows(output)):
        failures.append(
            f"arm B: no witness accompanied the {authored}-byte overflow")
    return failures


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", default=DEFAULT_BUILD_DIR)
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    parser.add_argument("--timeout", type=int, default=900)
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    binary = Path(os.path.abspath(resolve_binary(args.build)))
    rom = Path(os.path.abspath(args.rom))
    missing = [str(path) for path in (binary, rom, SOLO_SCRIPT)
               if not os.path.exists(path)]
    if missing:
        for path in missing:
            print(f"check_dl_high_water: FAIL -- missing {path}",
                  file=sys.stderr)
        return 1

    with tempfile.TemporaryDirectory(prefix="mdkr-dl-high-water-") as tmp:
        root = Path(tmp)
        try:
            clean = run_route(binary, rom, root, "clean", {}, args.timeout,
                              args.verbose)
            forced = run_route(
                binary, rom, root, "forced",
                {"MDKR_TEST_DL_HIGH_WATER_LIMIT": str(FORCED_LIMIT_COMMANDS)},
                args.timeout, args.verbose)
        except subprocess.TimeoutExpired as exc:
            print(f"check_dl_high_water: FAIL -- {exc}", file=sys.stderr)
            return 1
        failures = clean_failures(*clean) + forced_failures(*forced)

    if failures:
        for failure in failures:
            print(f"check_dl_high_water: FAIL -- {failure}", file=sys.stderr)
        return 1
    print("check_dl_high_water: PASS -- a retail 1P race reports its "
          "display-list high-water against the row the buffer was allocated "
          "to and stays inside it, and a length past that row aborts with the "
          "measurement that tripped it")
    return 0


if __name__ == "__main__":
    sys.exit(main())
