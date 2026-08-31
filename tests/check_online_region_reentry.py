#!/usr/bin/env python3
"""Same-process epoch scope of the online NTSC identity: online, then offline.

The launcher's online engine-boot lanes arm the NTSC source identity
(platform/rom_io.c) immediately before ``mdkr64_engine_boot()`` and clear it
the moment that boot returns.  This lane boots a EUROPEAN ROM through the
resident online soak and then -- in the SAME process, via the
``MDKR_TEST_ONLINE_REGION_REENTRY`` seam -- one plain offline epoch, and
asserts the whole story:

  * the ONLINE epoch consumed the NTSC clock (``source video: NTSC``) and
    printed the override witness naming the ROM's TRUE region (PAL);
  * the boot lane's clear ran before anything else (``override now 0``);
  * the OFFLINE epoch re-latched the authentic PAL 50 Hz source clock and
    printed NO second override witness.

A US ROM cannot prove this (its override is a no-op), so the lane refuses to
pass vacuously: the witness must name ROM region PAL.
"""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

from harness_utils import resolve_binary
from online_lane_util import FORBIDDEN_ONLINE, forbidden_marker, make_fail, \
    run_engine

TICKS = 900

OVERRIDE_WITNESS = ("[ROM] source identity: NTSC override armed for this "
                    "session (ROM region PAL)")
NTSC_CLOCK = "[ROM] source video: NTSC (60 Hz fields)"
PAL_CLOCK = "[ROM] source video: PAL (50 Hz fields)"
CLEARED = ("[online-resident] region re-entry: online epoch done "
           "(override now 0); booting one OFFLINE epoch")
OFFLINE_DONE = "[online-resident] region re-entry: offline epoch result=0"

fail = make_fail("region re-entry")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", default="build-beta")
    parser.add_argument(
        "--rom", type=Path, required=True,
        help="European pal.v80 ROM (a US ROM makes this proof vacuous)",
    )
    parser.add_argument("--ticks", type=int, default=TICKS)
    parser.add_argument("--timeout", type=int, default=300)
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    binary = Path(resolve_binary(args.build)).expanduser().resolve()
    rom = args.rom.expanduser().resolve()
    for path, label in ((binary, "binary"), (rom, "ROM")):
        if not path.is_file():
            parser.error(f"missing {label}: {path}")

    try:
        rc, output = run_engine(
            binary, rom, ticks=args.ticks, timeout=args.timeout,
            verbose=args.verbose,
            extra_env={
                "MDKR_TEST_ONLINE_RESIDENT": "1",
                "MDKR_TEST_ONLINE_REGION_REENTRY": "1",
            },
            prefix="mdkr64-region-reentry-")
    except subprocess.TimeoutExpired as error:
        return fail(f"run timed out: {error}")

    marker = forbidden_marker(output, *FORBIDDEN_ONLINE)
    if marker:
        return fail(f"observed forbidden marker {marker!r}", output)
    if rc != 0:
        return fail(f"process exited {rc}", output)
    if output.count(OVERRIDE_WITNESS) != 1:
        return fail(
            "the ONLINE epoch must print exactly one PAL override witness "
            f"({output.count(OVERRIDE_WITNESS)} found) -- was a European ROM "
            "passed, and did the online boot lane arm the identity?", output)
    if output.count(NTSC_CLOCK) != 1 or output.count(PAL_CLOCK) != 1:
        return fail(
            f"expected one NTSC (online) and one PAL (offline) source-clock "
            f"print, got {output.count(NTSC_CLOCK)}/{output.count(PAL_CLOCK)}",
            output)
    if output.index(NTSC_CLOCK) > output.index(PAL_CLOCK):
        return fail("the NTSC (online) epoch must precede the PAL (offline) "
                    "epoch", output)
    if CLEARED not in output:
        return fail("the boot lane did not clear the override when the "
                    "online boot returned", output)
    if OFFLINE_DONE not in output:
        return fail("the same-process offline epoch did not complete", output)
    if output.count("[HOST-SHUTDOWN] rom=0 arena=0 delayedFree=0") != 2:
        return fail("both epochs must tear down cleanly", output)

    print(
        "PASS online region re-entry: one process ran an ONLINE epoch under "
        "the armed NTSC identity (true region PAL witnessed) and a following "
        "OFFLINE epoch re-latched the authentic PAL 50 Hz source clock with "
        "the override cleared"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
