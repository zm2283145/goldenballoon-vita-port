#!/usr/bin/env python3
"""No racer draw may ever present a never-posed (bind-pose / T-pose) model.

The defect class this gate exists for
-------------------------------------
Vertices are posed exclusively by `obj_animate_tick()`, and it poses ONLY
`modelInstances[obj->modelIndex]` -- the authoritative index, part of the
[SIMHASH] v3 stream, committed once per tick from the canonical LAST
viewport's camera (`scene_build_last_viewport_basis`). The draw, however,
selects a per-viewport index from the LOCAL lens distance (`allowLodBias` in
`racer_model_index_for_view`, game/src/objects.c). Any drawn index whose
instance was never the committed index still holds the `model_instance_init`
base-mesh copy with the sentinel `animationID == -1`: the bind pose. Two
routes reach one:

  - ONLINE (no enhancement needed): on the endpoint whose seat is NOT the
    canonical last viewport, the remote racer commits near bands forever (it
    never leaves its own canonical camera), so its far-band instances are
    never posed -- a sustained T-pose whenever the local player falls behind
    and looks at it, recovering on catch-up (the 2026-08-31 two-Mac beta-4
    playtest defect; reproduced red with 183 never-posed draws, LodBias=0).
  - `Enhancements.LodBias` ("Model detail"): the draw-only bias holds a MORE
    detailed model than the ladder chose and can land on a band the committed
    ladder never visits (the 2026-08-29 two-machine race with `LodBias=2`:
    3379/3379 divergent draws never-posed; ~3977 on the offline 2P split).

The fix is the shared never-posed fence at the draw seam
(racer_model_index_for_view): a selection whose instance carries the -1
sentinel degrades to the unbiased ladder choice, then to the authoritative
committed instance -- posed either way, presentation-only, both online and
offline.

What this gate runs
-------------------
TWO arms, both with `MDKR_ENH_LOD_BIAS=2` (the 2026-08-29 reporting machine's
exact setting), `MDKR_TEST_ANIM_LOD_WITNESS=1` (the read-only draw witness:
one line per racer draw whose DRAWN index differs from the authoritative one
OR whose selection the fence moved, carrying the drawn instance's animationID
and the pre-fence REQUESTED index + animationID; -1 == never posed) and
`MDKR_DRAWDIST_TRACE=1` (the [DRAWDIST] census, whose `lodBias=` field proves
the setting actually armed and whose `lodShifted` field proves the bias
actually displaced at least one ladder choice):

  1. OFFLINE ARM (roster INACTIVE): the 2P split-screen race
     (`race_2p_split.txt`). The fence is NOT roster-gated -- the offline NPC
     T-pose sighting is the same class -- so this arm asserts the fix holds
     offline AND self-proves the instrument: the defect assertion here and
     online share a single point of failure, the `model_instance_init`
     sentinel `animationID == -1`. At least one `requestedAnimationID=-1`
     witness line (the fence catching a never-posed request) proves the
     sentinel and the witness plumbing are both alive; if the init value ever
     drifted, those lines would vanish and this arm would FAIL loudly instead
     of the defect returning while everything stayed green.
  2. ONLINE ARM: the same in-process two-adapter live loopback session as
     check_online_engine_boot_direct.py (real DTLS, direct boot, autopilot),
     roster ACTIVE.

What it asserts
---------------
1. Arm 1 (offline): the run completes cleanly, the census reports `lodBias=2`
   (the env override landed), the witness fired, at least one witness line
   carries `requestedAnimationID=-1` (sentinel + fence + witness plumbing all
   alive), and ZERO witness lines carry `drawnAnimationID=-1` (the fence
   holds offline).
2. Arm 2 is a REAL converged online race: direct boot fired, the online
   rollback race ran, both endpoints converged, and the hash equals the GOLDEN
   literal -- which doubles as "LodBias, the witness AND the fence are
   presentation-only".
3. Arm 2 positive controls, checked before the defect assertion so a broken
   seam cannot pass vacuously: the [DRAWDIST] census reports `lodBias=2`; at
   least one census row reports `lodShifted > 0`; the witness fired at least
   once; and at least one line carries `requestedAnimationID=-1` (the biased
   route still requests never-posed bands online, so the fence is genuinely
   exercised here too).
4. Arm 2 defect assertion: ZERO witness lines carry `drawnAnimationID=-1`: no
   online racer draw ever presented a never-posed (bind-pose) model.
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
    r"drawnAnimationID=(-?\d+) drawnAnimationFrame=(-?\d+) "
    r"requestedIndex=(-?\d+) requestedAnimationID=(-?\d+)$",
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
    landed rather than assuming it. The fence is unconditional, so this arm
    both asserts the offline fix and self-proves the -1 sentinel via the
    fence's requestedAnimationID=-1 witness lines.
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

    # ---- Arm 1: offline fix + sentinel self-proof (roster inactive) --------
    # Run FIRST: if the fence never catches a never-posed request here, the
    # instrument cannot say -1 and arm 2's "zero -1" verdict below would be
    # meaningless.
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
    sentinel_requested = [row for row in sentinel_witness if row[5] == "-1"]
    if not sentinel_requested:
        return fail(
            f"sentinel arm: {len(sentinel_witness)} witnessed racer draw(s) "
            f"offline with LodBias=2 and NONE reported "
            f"requestedAnimationID=-1. The model_instance_init sentinel, the "
            f"never-posed fence or the witness plumbing has drifted -- the "
            f"zero--1 assertions here and online can no longer detect the "
            f"defect", sentinel_out)
    sentinel_bind = [row for row in sentinel_witness if row[2] == "-1"]
    if sentinel_bind:
        sample = ", ".join(
            f"render={row[0]} auth={row[1]}" for row in sentinel_bind[:3])
        return fail(
            f"sentinel arm: {len(sentinel_bind)} of {len(sentinel_witness)} "
            f"witnessed racer draw(s) OFFLINE presented a NEVER-POSED model "
            f"instance (drawnAnimationID=-1 -- the bind pose / NPC T-pose; "
            f"e.g. {sample}); the draw-seam fence is not holding offline",
            sentinel_out)

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
                    "authoritative divergence or fence event was observed at "
                    "all (positive control)", output)
    requested_bind = [row for row in witness if row[5] == "-1"]
    if not requested_bind:
        return fail(
            f"{len(witness)} witnessed racer draw(s) online with LodBias=2 "
            f"and NONE reported requestedAnimationID=-1 -- the biased route "
            f"no longer requests a never-posed band, so the defect assertion "
            f"below is vacuous", output)

    # The defect: a drawn racer instance carrying the model_instance_init
    # sentinel was presented -- the bind pose, i.e. the online T-pose.
    bind_pose = [row for row in witness if row[2] == "-1"]
    if bind_pose:
        sample = ", ".join(
            f"render={row[0]} auth={row[1]}" for row in bind_pose[:3])
        return fail(
            f"{len(bind_pose)} of {len(witness)} witnessed racer draw(s) "
            f"presented a NEVER-POSED model instance (drawnAnimationID=-1 -- "
            f"the bind pose / online T-pose; e.g. {sample})", output)

    print(
        "PASS online racer LOD animation: offline arm (2P split, roster "
        f"inactive) fenced {len(sentinel_requested)} never-posed request(s) "
        f"of {len(sentinel_witness)} witnessed draw(s), none presented "
        "(drawn -1 count 0) -- the -1 sentinel, the fence and the witness "
        "plumbing are alive; online arm: with Enhancements.LodBias=2 the "
        f"converged race (hash={hash_visible}==GOLDEN, racedTicks={raced}) "
        f"witnessed {len(witness)} draw(s), fenced {len(requested_bind)} "
        "never-posed request(s), none presented (drawn -1 count 0); bias "
        "armed (lodBias=2 census) and displaced choices (lodShifted>0)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
