#!/usr/bin/env python3
"""Issue #54 (Run B): a durable save write that fails must not fail SILENTLY.

Why this exists
---------------
On Linux the AppImage wrote saves to a `save/` folder inside whatever directory
it happened to be launched from. When that folder was unwritable, the reporter's
session logged 18 `[SAVE] could not create ...` lines and lost every time-trial
record -- and NOTHING WHATSOEVER was shown to the player. `write_eeprom_data`'s
-1 was discarded by `input_update` (game/src/joypad.c) and the write-relocation
fallback (`mdkr_user_paths_activate_write_fallback`) was wired only to
video-config and app-prefs writes, never to EEPROM/pak/ghost.

This check reproduces Run B: point `MDKR_SAVE_DIR` at a directory under a
read-only parent (so the save directory can never be created), drive the
Adventure new-game route (which writes a save), and assert:

  1. The engine still logged the durable-write failure loudly (`[SAVE]` ...
     `could not create`/`failed`). If it did not, the route attempted no write
     and the case would be vacuous -- so this is asserted, not assumed.
  2. Exactly ONE player-notice marker fired -- `[SAVE] progress could not be
     saved` -- rate-limited to once per session. Before the fix this marker does
     not exist and the run is silent; that is the RED state this pins.
  3. The run still exited 0 and did not crash: a failed save must degrade, not
     take the session down.

Usage:
    tests/check_save_write_notice.py [--build build] [--rom baserom.us.v80.z64] [-v]

Always runs muted + headless. Exit 0 = pass; exit 1 = at least one assertion
failed (each printed with the measured value).
"""

from __future__ import annotations

import argparse
import os
import stat
import subprocess
import sys
import tempfile

from harness_utils import DEFAULT_BUILD_DIR, resolve_binary

ADVENTURE_SCRIPT = "tests/input_scripts/adventure_hub_drive.txt"

# The new-game save is created when the Adventure intro/hub load. Measured on
# us.v80: the hub loads ~2086, so this leaves ample margin for the first save
# write without paying for a full race.
FRAMES = 6000

# What the platform save layer emits.
FAILURE_MARKERS = ("[SAVE] could not create", "[SAVE] durable write of",
                   "[SAVE] could not open")
# The new, non-silent player notice this issue adds.
NOTICE_MARKER = "[SAVE] progress could not be saved"


def run(binary: str, rom: str, save_dir: str, frames: int,
        verbose: bool) -> tuple[int, str]:
    env = {k: v for k, v in os.environ.items() if not k.startswith("MDKR_")}
    env.update(
        MDKR_AUDIO="0",
        MDKR_AUTOPILOT="1",
        MDKR_TRACE="1",
        # Run B: the save directory's parent is read-only, so `save/` can never
        # be created and every durable write fails.
        MDKR_SAVE_DIR=save_dir,
        # Isolate the video config the same way every save-isolated check must
        # (tests/check_harness_isolation.py): a repo-root mdkr64.ini must not
        # reach this engine, and it must not try to write one either.
        MDKR_VIDEO_CONFIG_PATH=os.devnull,
    )
    cmd = [binary, "--headless-frames", str(frames),
           "--input-script", ADVENTURE_SCRIPT, "--rom", rom]
    if verbose:
        print("  $ MDKR_SAVE_DIR=" + save_dir + " " + " ".join(cmd))
    timeout = max(120.0, frames / 15.0)
    try:
        proc = subprocess.run(cmd, env=env, capture_output=True, text=True,
                              timeout=timeout)
    except subprocess.TimeoutExpired as error:
        out = (error.stdout or "") + (error.stderr or "")
        if isinstance(out, bytes):
            out = out.decode("utf-8", errors="replace")
        return 124, out + f"\n[TIMEOUT] exceeded {timeout:.0f}s\n"
    return proc.returncode, proc.stdout + proc.stderr


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", default=DEFAULT_BUILD_DIR)
    ap.add_argument("--rom", default="baserom.us.v80.z64")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    binary = resolve_binary(args.build)
    for path in (binary, args.rom, ADVENTURE_SCRIPT):
        if not os.path.exists(path):
            print(f"FAIL: {path} not found -- build first / check the ROM path")
            return 1

    failures: list[str] = []
    with tempfile.TemporaryDirectory(prefix="mdkr_ro_save_") as root:
        read_only = os.path.join(root, "ro")
        os.mkdir(read_only)
        save_dir = os.path.join(read_only, "save")
        # Read-only parent: the engine can neither create nor write save/.
        os.chmod(read_only, stat.S_IRUSR | stat.S_IXUSR)
        try:
            print("run: read-only save dir, Adventure new-game route")
            rc, out = run(binary, args.rom, save_dir, FRAMES, args.verbose)
        finally:
            # Restore write so TemporaryDirectory cleanup can remove the tree.
            os.chmod(read_only, stat.S_IRWXU)

    if args.verbose:
        for line in out.splitlines():
            if "[SAVE]" in line:
                print("   " + line)

    if "[CRASH]" in out or "[FATAL]" in out:
        failures.append("run produced [CRASH]/[FATAL] -- a failed save took the "
                        "session down instead of degrading")
    if rc != 0:
        failures.append(f"run exited {rc} (negative = killed by signal {-rc}) -- "
                        "a failed durable save must not end the process")

    if not any(marker in out for marker in FAILURE_MARKERS):
        failures.append(
            "no [SAVE] durable-write failure was logged -- the route attempted "
            "no save write, so this case is vacuous (raise --frames or fix the "
            "read-only fixture)")

    notice_count = out.count(NOTICE_MARKER)
    if notice_count == 0:
        failures.append(
            f"the player was never notified: no {NOTICE_MARKER!r} line -- a "
            "failed durable save is still silent (this is the pre-fix RED state)")
    elif notice_count != 1:
        failures.append(
            f"expected exactly one {NOTICE_MARKER!r} notice per session, got "
            f"{notice_count} -- the notice is not rate-limited")

    if failures:
        print(f"\nFAIL ({len(failures)}):")
        for f in failures:
            print("  - " + f)
        return 1
    print("\nPASS: an unwritable save directory logs the failure AND surfaces one "
          "player notice, and the session still exits cleanly.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
