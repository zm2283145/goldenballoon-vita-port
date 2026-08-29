#!/usr/bin/env python3
"""An ONLINE race must never draw a never-posed (bind-pose / T-pose) racer.

The defect this gate exists for
-------------------------------
`Enhancements.LodBias` ("Model detail") biases the racer LOD ladder's choice
toward a MORE detailed model -- but only on the draw (`allowLodBias` in
`racer_model_index_for_view`, game/src/objects.c); the authoritative
`obj->modelIndex` is part of the [SIMHASH] v3 stream and is never moved.
Vertices, however, are posed exclusively by `obj_animate_tick()`, and it poses
ONLY `modelInstances[obj->modelIndex]`. In the online two-seat canonical
layout the racer band table's first two thresholds are ZERO, so the
authoritative ladder can never return model index 0 or 1. A bias onto either
therefore selects a ModelInstance that has NEVER been posed --
`model_instance_init` leaves it as the bind-pose copy with the sentinel
`animationID == -1` -- and every biased racer draw is a sustained T-pose,
while the race itself stays perfectly synced (observed on a real two-machine
online race, 2026-08-29, with `LodBias=2` in the reporting machine's ini).

What this gate runs
-------------------
TWO arms, both with `MDKR_ENH_LOD_BIAS=2` (the reporting machine's exact
setting), `MDKR_TEST_ANIM_LOD_WITNESS=1` (the read-only draw witness: one line
per racer draw whose DRAWN model index differs from the authoritative one,
carrying the drawn instance's animationID; -1 == never posed) and
`MDKR_DRAWDIST_TRACE=1` (the [DRAWDIST] census, whose `lodBias=` field proves
the setting actually armed and whose `lodShifted` field proves the bias
actually displaced at least one ladder choice):

  1. SENTINEL SELF-PROOF (offline, roster INACTIVE): the 2P split-screen race
     (`race_2p_split.txt`). The clamp is deliberately roster-gated, so this
     path RETAINS the defect by design -- and must therefore still produce
     `drawnAnimationID=-1` witness lines. This arm exists because the clamp
     and the defect assertion below share a single point of failure: the
     `model_instance_init` sentinel `animationID == -1`. If the init value
     ever changed, the clamp would stop firing AND the witness would stop
     reporting -1 at the same time -- the defect would return while arm 2
     stayed green. This arm turns that silent drift into a loud FAIL: no -1
     lines on the unfixed path means the sentinel or the witness plumbing
     moved, not that the world got better.
  2. DEFECT ASSERTION (online): the same in-process two-adapter live loopback
     session as check_online_engine_boot_direct.py (real DTLS, direct boot,
     autopilot), roster ACTIVE, where the clamp must hold.

What it asserts
---------------
1. Arm 1 (offline, unfixed by design): the run completes cleanly, the census
   reports `lodBias=2` (the env override landed), the witness fired, and at
   least one witness line carries `drawnAnimationID=-1` -- the sentinel and
   the witness plumbing are both alive (~3977 such lines when authored).
2. Arm 2 is a REAL converged online race: direct boot fired, the online
   rollback race ran, both endpoints converged, and the hash equals the GOLDEN
   literal -- which doubles as "LodBias and the witness are presentation-only".
3. Arm 2 positive controls, checked before the defect assertion so a broken
   seam cannot pass vacuously: the [DRAWDIST] census reports `lodBias=2`; at
   least one census row reports `lodShifted > 0`; and the witness fired at
   least once (drawn-vs-authoritative divergences exist at all).
4. Arm 2 defect assertion: ZERO witness lines carry `drawnAnimationID=-1`: no
   online racer draw ever presented a never-posed (bind-pose) model. Pre-fix
   this arm produced 3379 of 3379 divergent draws at -1; post-fix it must be
   zero -- and arm 1 has just proven the instrument can still say -1.
"""

from __future__ import annotations

import argparse
import subprocess
import tempfile
from pathlib import Path

from harness_utils import (ABORT_MARKERS, ASSERT_MARKERS, DEFAULT_BUILD_DIR,
                           find_fatal, resolve_binary)
from online_lane_util import (
    DIRECT_BOOT_RE, ENGINE_LIVE_RE, FORBIDDEN_ONLINE, ONLINE_RACE_RE,
    clean_environment, forbidden_marker, make_fail, run_engine,
)
# One GOLDEN, one owner: same track, same flow, and this lane's settings are
# presentation-only by design, so the two lanes must always agree. Importing
# (rather than copying) the literal makes a legit ROM/toolchain bump a
# one-line change instead of a drift hazard.
from check_online_engine_boot_direct import GOLDEN_RACE_HASH
import re

ROOT = Path(__file__).resolve().parent.parent
TICKS = 3000

# Arm 1: the 2P split race clock starts around frame 2662 (see
# tests/check_enh_draw_distance.py); 3600 leaves ~900 in-race frames, which
# authored ~2000+ never-posed draws -- the >=1 assertion has enormous margin.
SENTINEL_SCRIPT = ROOT / "tests" / "input_scripts" / "race_2p_split.txt"
SENTINEL_FRAMES = 3600

WITNESS_RE = re.compile(
    r"^\[anim-lod-witness\] renderIndex=(\d+) authoritativeIndex=(\d+) "
    r"drawnAnimationID=(-?\d+) drawnAnimationFrame=(-?\d+)$",
    re.MULTILINE,
)

DRAWDIST_RE = re.compile(
    r"^\[DRAWDIST\] frame=\d+ scale=[\d.]+ lodBias=(\d+) "
    r"authored=\d+ extended=\d+ drawn=\d+ lodShifted=(\d+)$",
    re.MULTILINE,
)

fail = make_fail("racer LOD animation")


def run_sentinel_arm(binary: Path, rom: Path, timeout: int,
                     verbose: bool) -> tuple[int, str]:
    """Arm 1: the offline 2P split race, roster inactive, LodBias=2.

    Returns ``(returncode, combined_output)``. Mirrors the headless offline
    environment tests/check_enh_draw_distance.py uses, with the witness and
    the census armed; LodBias arrives via the MDKR_ENH_LOD_BIAS env override
    (the same seam arm 2 uses), and the census `lodBias=` field proves it
    landed rather than assuming it.
    """
    with tempfile.TemporaryDirectory(prefix="mdkr64-lodanim-sentinel-") as temp:
        run_dir = Path(temp)
        save_dir = run_dir / "save"
        save_dir.mkdir()
        environment = clean_environment(
            LC_ALL="C",
            MDKR_AUDIO="0",
            MDKR_AUTOPILOT="1",
            MDKR_DRAWDIST_TRACE="1",
            MDKR_ENH_LOD_BIAS="2",
            MDKR_NO_CRASH_HANDLER="1",
            MDKR_RENDERER="gl",
            MDKR_SAVE_DIR=str(save_dir),
            MDKR_TEST_ANIM_LOD_WITNESS="1",
            MDKR_VIDEO_CONFIG_PATH=str(save_dir / "video.ini"),
            MDKR64_HIDDEN="1",
        )
        command = [
            str(binary),
            "--headless-frames", str(SENTINEL_FRAMES),
            "--window-size", "640x480",
            "--input-script", str(SENTINEL_SCRIPT),
            "--rom", str(rom),
        ]
        if verbose:
            print(f"$ (sentinel) {' '.join(command)}", flush=True)
        process = subprocess.run(
            command, cwd=run_dir, env=environment, text=True,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            timeout=timeout, check=False,
        )
        return process.returncode, (process.stdout or "")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", default=DEFAULT_BUILD_DIR)
    parser.add_argument("--rom", type=Path, default="baserom.us.v80.z64")
    parser.add_argument("--ticks", type=int, default=TICKS)
    parser.add_argument("--timeout", type=int, default=300)
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    binary = Path(resolve_binary(args.build)).expanduser().resolve()
    rom = args.rom.expanduser().resolve()
    for path, label in ((binary, "binary"), (rom, "ROM")):
        if not path.is_file():
            parser.error(f"missing {label}: {path}")

    # ---- Arm 1: sentinel self-proof (offline, roster inactive) -------------
    # Run FIRST: if the instrument cannot say -1 on the deliberately-unfixed
    # path, arm 2's "zero -1" verdict below would be meaningless.
    try:
        sentinel_rc, sentinel_out = run_sentinel_arm(
            binary, rom, args.timeout, args.verbose)
    except subprocess.TimeoutExpired as error:
        return fail(f"sentinel arm timed out: {error}")
    sentinel_fatal = find_fatal(sentinel_out, *ABORT_MARKERS, *ASSERT_MARKERS)
    if sentinel_rc != 0 or sentinel_fatal is not None:
        return fail(f"sentinel arm exited {sentinel_rc}, "
                    f"fatal={sentinel_fatal or 'none'}", sentinel_out)
    sentinel_census = DRAWDIST_RE.findall(sentinel_out)
    if not any(row[0] == "2" for row in sentinel_census):
        return fail("sentinel arm: the [DRAWDIST] census never reported "
                    "lodBias=2 -- the MDKR_ENH_LOD_BIAS env override did not "
                    "reach the enhancement offline", sentinel_out)
    sentinel_witness = WITNESS_RE.findall(sentinel_out)
    if not sentinel_witness:
        return fail("sentinel arm: the anim-lod witness never fired on the "
                    "offline 2P split -- the witness plumbing is dead, so "
                    "the online zero--1 assertion below cannot prove "
                    "anything", sentinel_out)
    sentinel_bind = [row for row in sentinel_witness if row[2] == "-1"]
    if not sentinel_bind:
        return fail(
            f"sentinel arm: {len(sentinel_witness)} divergent racer draw(s) "
            f"on the roster-INACTIVE (deliberately unfixed) path and NONE "
            f"reported drawnAnimationID=-1. The model_instance_init sentinel "
            f"or the witness plumbing has drifted -- the online assertion "
            f"below can no longer detect the defect", sentinel_out)

    # ---- Arm 2: the online race, roster active, clamp must hold ------------
    extra_env = {
        "MDKR_APP_TEST_ONLINE_LIVE": "1",
        "MDKR_ENH_LOD_BIAS": "2",
        "MDKR_TEST_ANIM_LOD_WITNESS": "1",
        "MDKR_DRAWDIST_TRACE": "1",
    }
    try:
        returncode, output = run_engine(
            binary, rom, ticks=args.ticks, timeout=args.timeout,
            verbose=args.verbose, extra_env=extra_env,
            prefix="mdkr64-online-lodanim-")
    except subprocess.TimeoutExpired as error:
        return fail(f"engine run timed out (a stall would look like this): "
                    f"{error}")

    marker = forbidden_marker(output, *FORBIDDEN_ONLINE)
    if marker:
        return fail(f"observed forbidden marker {marker!r}", output)
    if returncode != 0:
        return fail(f"process exited {returncode}", output)
    if not DIRECT_BOOT_RE.findall(output):
        return fail("the direct-boot seam never fired", output)
    if not ONLINE_RACE_RE.findall(output):
        return fail("the engine never entered an ONLINE rollback race", output)

    stats = ENGINE_LIVE_RE.findall(output)
    if len(stats) != 1:
        return fail(f"expected one ENGINE-ONLINE-LIVE witness, got {stats!r}",
                    output)
    (result, raced, _drains, advance_failed, _envelopes, _accepted, _corrected,
     _drained, fold_visible, _fold_peer, hash_visible, hash_peer,
     converged) = stats[0]
    if int(result) != 0 or int(advance_failed) != 0 or int(raced) < 100:
        return fail(f"the online race did not run cleanly (result={result} "
                    f"racedTicks={raced} advanceFailed={advance_failed})",
                    output)
    if int(converged) != 1 or hash_visible != hash_peer or \
            int(fold_visible) < 20:
        return fail(f"the two endpoints did not converge "
                    f"(converged={converged} hashVisible={hash_visible} "
                    f"hashPeer={hash_peer})", output)
    if hash_visible != GOLDEN_RACE_HASH:
        return fail(
            f"the converged hash {hash_visible} != GOLDEN {GOLDEN_RACE_HASH} "
            f"-- LodBias or the witness moved the sim, or the golden needs a "
            f"legit bump alongside check_online_engine_boot_direct.py's",
            output)

    # Positive controls FIRST -- a seam that failed to arm must fail loudly,
    # not pass by observing nothing.
    census = DRAWDIST_RE.findall(output)
    if not census:
        return fail("no [DRAWDIST] census rows -- MDKR_DRAWDIST_TRACE did not "
                    "arm, so this run cannot prove the bias was active",
                    output)
    if not any(row[0] == "2" for row in census):
        return fail("the [DRAWDIST] census never reported lodBias=2 -- the "
                    "MDKR_ENH_LOD_BIAS env override did not reach the "
                    "enhancement", output)
    if not any(int(row[1]) > 0 for row in census):
        return fail("the bias never displaced a ladder choice (lodShifted==0 "
                    "on every frame) -- the route no longer exercises the "
                    "biased draw, so the defect assertion below is vacuous",
                    output)
    witness = WITNESS_RE.findall(output)
    if not witness:
        return fail("the anim-lod witness never fired -- no drawn-vs-"
                    "authoritative divergence was observed at all (positive "
                    "control)", output)

    # The defect: a drawn racer instance carrying the model_instance_init
    # sentinel was presented -- the bind pose, i.e. the online T-pose.
    bind_pose = [row for row in witness if row[2] == "-1"]
    if bind_pose:
        sample = ", ".join(
            f"render={row[0]} auth={row[1]}" for row in bind_pose[:3])
        return fail(
            f"{len(bind_pose)} of {len(witness)} divergent racer draw(s) "
            f"presented a NEVER-POSED model instance (drawnAnimationID=-1 -- "
            f"the bind pose / online T-pose; e.g. {sample})", output)

    print(
        "PASS online racer LOD animation: sentinel arm (offline 2P, roster "
        f"inactive, unfixed by design) authored {len(sentinel_bind)} "
        f"never-posed draw(s) of {len(sentinel_witness)} divergent -- the "
        "-1 sentinel and witness plumbing are alive; online arm: with "
        f"Enhancements.LodBias=2 the converged race (hash={hash_visible}"
        f"==GOLDEN, racedTicks={raced}) drew {len(witness)} divergent racer "
        "draw(s), none never-posed (drawnAnimationID=-1 count 0); bias armed "
        "(lodBias=2 census) and displaced choices (lodShifted>0)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
