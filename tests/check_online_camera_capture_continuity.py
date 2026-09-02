#!/usr/bin/env python3
"""Every online correction tick still captures its viewport camera.

Regression gate for the online camera-capture hole: thread3_main stamps the
main display list -- and arms the presentation authored-camera latch -- BEFORE
prepare_tick's online reconcile runs. A rollback correction's snapshot restore
runs presentation_snapshot_stage_reset(), which (correctly) drops the published
pair and history but ALSO wiped that just-armed latch. The corrected pass then
rendered and called camSetProjMtx as always, but its authored-camera records
were refused, so the tick's presentation snapshot published ZERO cameras.

Downstream: camera interpolation stayed down one tick longer than object
interpolation after every correction (objects re-identify at the correction
tick's capture; the camera could not even hold a pose there), presenting a
gliding world under a stepping camera exactly at the abrupt-kart-state moments
(wall impacts, bumps) where corrections cluster -- the owner-reported online
camera artifact family. Measured pre-fix on this rig: a 2415-correction race
captured cameras on 16 of 2431 ticks.

The fix re-arms the latch for the in-flight authored tick inside the rollback
rebuild hook (rollback_game_authority.c), roster-gated and beta-only. The
camera history stays cleared, so the correction tick's capture remains a
DISCONTINUITY and nothing ever blends across a correction.

This drives the in-process two-adapter live loopback with a full-race window of
forced prediction/correction rollbacks (MDKR_APP_TEST_ONLINE_LIVE_PREDICT) and
the camera census armed (MDKR_CAMERA_OOB_CENSUS), and asserts:
  - the run produced a sustained correction storm (>= 500 corrections);
  - the census's miss counters are ZERO: no tick with a publishing viewport
    published a camera-less snapshot (pre-fix: miss ~= corrections);
  - cameras were captured on essentially every INGAME tick;
  - the correction ticks kept the fail-closed hold discipline (holds >= the
    correction count -- blending across a correction stays impossible);
  - the endpoints still converge byte-for-byte and no [FATAL]/[CRASH] appears.
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

TICKS = 3000
PREDICT_WINDOW = 100000  # cover the whole race: a correction on every tick

CENSUS_RE = re.compile(
    r"^\[CAM-OOB\] ticks=(\d+) near=(\d+) corr=(\d+) cams=(\d+) "
    r"authoob_near=(\d+) authoob_far=(\d+) "
    r"interpchk_near=(\d+) interpchk_far=(\d+) "
    r"interpoob_near=(\d+) interpoob_far=(\d+) "
    r"hold_near=(\d+) hold_far=(\d+) blend_near=(\d+) blend_far=(\d+) "
    r".* miss_near=(\d+) miss_far=(\d+) "
    r"liveoob_near=(\d+) liveoob_far=(\d+)$", re.MULTILINE)
ENGINE_LIVE_RE = re.compile(
    r"^\[ENGINE-ONLINE-LIVE\] result=(-?\d+) racedTicks=(\d+) .* "
    r"transportCorrected=(\d+) .* hashVisible=([0-9a-f]{16}) "
    r"hashPeer=([0-9a-f]{16}) converged=(\d+)$", re.MULTILINE)


def fail(message: str, output: str = "") -> int:
    print(f"FAIL online camera capture continuity: {message}", file=sys.stderr)
    if output:
        print(output[-16000:], file=sys.stderr)
    return 1


def clean_environment(**updates: str) -> dict[str, str]:
    environment = {
        key: value for key, value in os.environ.items()
        if not key.startswith(("MDKR", "GE007_"))
    }
    environment.update(updates)
    return environment


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

    with tempfile.TemporaryDirectory(prefix="mdkr64-camera-capture-") as temp:
        run_dir = Path(temp)
        (run_dir / "saves").mkdir()
        (run_dir / "preferences").mkdir()
        environment = clean_environment(
            LC_ALL="C",
            MDKR_APP_AUTOPLAY="1",
            MDKR_APP_TEST_ONLINE_LIVE="1",
            MDKR_APP_TEST_ONLINE_LIVE_PREDICT=str(PREDICT_WINDOW),
            MDKR_APP_AUTOPLAY_TICKS=str(args.ticks),
            MDKR_APP_PREFS_DIR=str(run_dir / "preferences"),
            MDKR_AUDIO="0",
            MDKR_AUTOPILOT="1",
            MDKR_CAMERA_OOB_CENSUS="1",
            MDKR_NO_CRASH_HANDLER="1",
            MDKR_PRESENT_RATE="original",
            MDKR_PRESENT_SNAPSHOT="1",
            MDKR_RENDERER="gl",
            MDKR_ROM=str(rom),
            MDKR_SAVE_DIR=str(run_dir / "saves"),
            MDKR_STATE_HASH="3",
            MDKR_TEST_SCRIPT_ONLY_INPUT="1",
            MDKR_VIDEO_CONFIG_PATH=str(run_dir / "video.ini"),
            MDKR64_HIDDEN="1",
        )
        try:
            process = subprocess.run(
                [str(binary)], cwd=run_dir, env=environment, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                timeout=args.timeout, check=False,
            )
        except subprocess.TimeoutExpired as error:
            return fail(f"run timed out (a stall would look like this): {error}")
        output = process.stdout or ""

    # The two pinned-refcount witnesses guard the correction-erasure class:
    # this rig's storm reliably rewinds past an autopilot weapon fire, so a
    # replay that fails to re-apply the snapshot-covered references ++ shows
    # up as a conservation deficit here (and, if the count ever walks to
    # zero, as the mid-race free refusal).
    for marker in ("[FATAL]", "[CRASH]", "AddressSanitizer",
                   "pinned model reference deficit",
                   "pinned item model refcount hit zero"):
        if marker in output:
            return fail(f"observed forbidden marker {marker!r}", output)
    if process.returncode != 0:
        return fail(f"process exited {process.returncode}", output)

    census_rows = CENSUS_RE.findall(output)
    if not census_rows:
        return fail("no [CAM-OOB] census row (witness not armed?)", output)
    (ticks, near, corr, cams, authoob_near, authoob_far, interpchk_near,
     interpchk_far, interpoob_near, interpoob_far, hold_near, hold_far,
     blend_near, blend_far, miss_near, miss_far, liveoob_near,
     liveoob_far) = (int(v) for v in census_rows[-1])

    stats = ENGINE_LIVE_RE.findall(output)
    if len(stats) != 1:
        return fail(f"expected one ENGINE-ONLINE-LIVE witness, got {stats!r}",
                    output)
    result, raced, corrected, hash_visible, hash_peer, converged = stats[0]
    if int(result) != 0:
        return fail(f"engine did not run clean (result={result})", output)
    if int(corrected) < 500 or corr < 500:
        return fail(
            f"the run did not sustain a correction storm "
            f"(transportCorrected={corrected}, census corr={corr}); the "
            "capture-continuity claim would be vacuous", output)
    if int(converged) != 1 or hash_visible != hash_peer:
        return fail(
            f"endpoints did not converge byte-for-byte (converged={converged} "
            f"hashVisible={hash_visible} hashPeer={hash_peer}) -- the fix "
            "must be presentation-only", output)

    # THE defect signal: a tick whose viewport has been publishing a camera
    # published none. Pre-fix this tracks the correction count (~2400 here).
    if miss_near + miss_far != 0:
        return fail(
            f"correction ticks published camera-less snapshots "
            f"(miss_near={miss_near} miss_far={miss_far}, corrections={corr}) "
            "-- the authored-camera latch is being wiped again", output)
    # Cameras on essentially every INGAME tick (first tick has no capture).
    if cams + 5 < ticks:
        return fail(
            f"cameras captured on only {cams} of {ticks} INGAME ticks", output)
    # Fail-closed discipline: every correction tick must HOLD (discontinuity),
    # never blend across the correction.
    if hold_near < corr:
        return fail(
            f"fewer holds than corrections near corrections "
            f"(hold_near={hold_near} corr={corr}) -- a correction tick "
            "blended, which must be impossible", output)

    print(
        "PASS online camera capture continuity: "
        f"{corr} corrections over {ticks} INGAME ticks, cameras captured on "
        f"{cams} ticks (miss_near={miss_near} miss_far={miss_far}), "
        f"correction ticks all fail-closed holds (hold_near={hold_near}), "
        f"converged hash={hash_visible}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
