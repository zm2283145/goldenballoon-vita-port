#!/usr/bin/env python3
"""A corrected weapon fire must keep the pinned item-model refcount conserved.

THE SCENARIO (refcount-erasure regression, 8e7b4bd2 class): the delayed-input
rollback lab withholds four canonical inputs and reconciles them at the target
tick, so the item probe's missile fire lands on the CORRECTED timeline -- the
replay that becomes canonical. The missile's object_model_init cache hit must
increment the pinned rocket model's ObjectModel.references, a field the
rollback authority snapshot-registers (TAG_ITEM_MODEL_REFERENCE_BASE ranges,
enumerated from the asset-lease registry) precisely so restore + replayed
re-application stay idempotent.

If resimulation FREEZES snapshot-covered counts, the corrected fire's ++ is
permanently erased: the weapon object lives on the canonical timeline while
the covered count still reads the pre-fire value, and the later live despawn
walks the pinned model toward zero and free_model_data mid-race (dangling
lease UAF / free-while-drawn). The engine's one-sided conservation validator
(references must never sit BELOW lease holds + live object holders, checked
at every authored boundary) witnesses the erasure at the very boundary the
correction completes on:

    [ROLLBACK] pinned model reference deficit: ...

which this lane treats as fatal. The delayed arm's own gate additionally
proves the restore-idempotency design post-fix: its second, exact replay of
the corrected window must reproduce byte-identical authority snapshots WITH
the re-applied ++ (exact_replay=1).

Asserts:
  - the probe armed (missile, one charge, release inside the window);
  - the corrected fire actually spawned (probe result observed=1, spawns>=1);
  - the delayed-input correction passed with byte-identical exact replay;
  - NO pinned-model reference deficit witness anywhere in the run;
  - NO mid-race pinned-model free refusal (the UAF tripwire stayed silent);
  - the pinned-model unpin witness printed (pin + witness plumbing alive);
  - no [FATAL]/[CRASH], exit 0.
"""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

from harness_utils import DEFAULT_BUILD_DIR, resolve_binary


ROOT = Path(__file__).resolve().parent.parent
SCRIPT = ROOT / "tests/input_scripts/nav_to_time_trial_race.txt"
PROCESS_TICKS = 3400
# Ancient Lake (the nav script's own level 5): the fired missile flies down
# the open lake straight and OUTLIVES the correction window, so its live
# despawn happens on a LATER live pass -- the exact asymmetry the erasure
# class needs (a corrected spawn whose ++ must survive, decremented live
# later). On Whale Bay the missile dies inside the window and the two sides
# cancel. Tick 300 is past Ancient Lake's longer intro pan (at 150 the fire
# is still refused and the probe reads observed=0).
TARGET_TICK = 300
BALLOON = 1  # missile row: fires a real projectile holding a pinned 3D model
LEVEL = 0

ARM_RE = re.compile(
    r"^\[ROLLBACK\] item probe armed: balloon=1 level=0 "
    r"weapon=(-?\d+) quantity=1 release=(\d+) mutation=0$",
    re.MULTILINE,
)
RESULT_RE = re.compile(
    r"^\[ROLLBACK\] item probe result: balloon=1 level=0 "
    r"weapon=(-?\d+) quantity=(-?\d+) spawns=(\d+) rumble=(\d+) "
    r"boost=(-?\d+) shield=(-?\d+) shieldType=(-?\d+) observed=(\d+)$",
    re.MULTILINE,
)
CORRECTION = (
    f"[ROLLBACK] delayed-input correction passed "
    f"ticks={TARGET_TICK - 3}..{TARGET_TICK} depth=4 "
    "non_input_divergence=1 exact_replay=1"
)
UNPIN_RE = re.compile(
    r"^\[ROLLBACK\] pinned model references at unpin:((?: r\d+=-?\d+)+)$",
    re.MULTILINE,
)
FORBIDDEN = (
    "[FATAL]",
    "[CRASH]",
    "AddressSanitizer",
    "pinned model reference deficit",
    "pinned item model refcount hit zero",
)


def fail(message: str, output: str = "") -> int:
    print(f"FAIL rollback weapon refcount: {message}", file=sys.stderr)
    if output:
        print(output[-16000:], file=sys.stderr)
    return 1


def environment(root: Path) -> dict[str, str]:
    values = {
        key: value for key, value in os.environ.items()
        if not key.startswith(("MDKR", "GE007_"))
    }
    values.update(
        LC_ALL="C",
        MDKR_AUDIO="0",
        MDKR_DUMP_EVERY="100000",
        MDKR_PRESENT_RATE="original",
        MDKR_RENDERER="gl",
        MDKR_ROLLBACK_LAB="1",
        MDKR_ROLLBACK_LAB_DELAYED_INPUT="1",
        MDKR_ROLLBACK_LAB_ROUNDTRIP="1",
        MDKR_ROLLBACK_LAB_TARGET_TICK=str(TARGET_TICK),
        MDKR_ROLLBACK_LAB_ITEM_BALLOON=str(BALLOON),
        MDKR_ROLLBACK_LAB_ITEM_LEVEL=str(LEVEL),
        MDKR_ROLLBACK_LAB_ITEM_MUTATION_CONTROL="0",
        MDKR_SAVE_DIR=str(root / "saves"),
        MDKR_VIDEO_CONFIG_PATH=str(root / "saves" / "video.ini"),
        MDKR_TEST_SCRIPT_ONLY_INPUT="1",
        MDKR64_HIDDEN="1",
    )
    return values


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", default=DEFAULT_BUILD_DIR)
    parser.add_argument("--rom", type=Path, default="baserom.us.v80.z64")
    parser.add_argument("--timeout", type=int, default=420)
    args = parser.parse_args()

    binary = Path(resolve_binary(args.build)).expanduser().resolve()
    rom = args.rom.expanduser().resolve()
    for path, label in ((binary, "binary"), (rom, "ROM"),
                        (SCRIPT, "input script")):
        if not path.is_file():
            parser.error(f"missing {label}: {path}")

    with tempfile.TemporaryDirectory(prefix="mdkr64-weapon-refcount-") as temp:
        root = Path(temp)
        (root / "saves").mkdir()
        try:
            process = subprocess.run(
                [
                    str(binary), "--rom", str(rom),
                    "--headless-ticks", str(PROCESS_TICKS),
                    "--input-script", str(SCRIPT),
                    "--window-size", "320x240",
                ],
                cwd=root,
                env=environment(root),
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                timeout=args.timeout,
                check=False,
            )
        except (OSError, subprocess.TimeoutExpired) as error:
            return fail(f"run did not complete: {error}")
    output = process.stdout or ""

    for marker in FORBIDDEN:
        if marker in output:
            return fail(f"observed forbidden marker {marker!r}", output)
    if process.returncode != 0:
        return fail(f"process exited {process.returncode}", output)

    armed = ARM_RE.findall(output)
    if len(armed) != 1:
        return fail(f"expected one armed probe, got {armed!r}", output)
    release = int(armed[0][1])
    if release != TARGET_TICK - 3:
        return fail(
            f"release tick {release} is not the first tick of the correction "
            f"window [{TARGET_TICK - 3}, {TARGET_TICK}]", output)

    if CORRECTION not in output:
        return fail(
            "the delayed-input correction (with its byte-identical exact "
            "replay of the corrected window) did not pass", output)

    results = RESULT_RE.findall(output)
    if len(results) != 1:
        return fail(f"expected one probe result, got {results!r}", output)
    spawns = int(results[0][2])
    observed = int(results[0][7])
    if observed != 1 or spawns < 1:
        return fail(
            f"the corrected missile fire was not observed "
            f"(observed={observed} spawns={spawns}); the conservation claim "
            "would be vacuous", output)

    unpin = UNPIN_RE.findall(output)
    if not unpin:
        return fail("no pinned-model unpin witness", output)

    print(
        "PASS rollback weapon refcount: corrected missile fire at tick "
        f"{release} (spawns={spawns}), correction + exact replay passed at "
        f"tick {TARGET_TICK}, no pinned reference deficit, unpin "
        f"references{unpin[-1]}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
