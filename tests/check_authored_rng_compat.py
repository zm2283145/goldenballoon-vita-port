#!/usr/bin/env python3
"""Freeze the legacy all-racer state/RNG stream for the authored 30 Hz route.

This is a raw compatibility oracle, not a statistical gameplay gate. It
protects every emitted racer row and the shared authored RNG value against the
accepted vehicle-audio/Taj integration baseline. The prior hash predated the
ordinary-car audio dispatch and therefore froze a port bug: the retail ROM's
``racer_sound_car`` consumes the shared RNG stream. That ownership was proved
directly with the pinned ares PC/return-address witness documented in
tests/README.md before this oracle was rebaselined. The fixture is encoded as a
row count, exact schema, and SHA-256 so no private ROM-derived log is shipped.
The shared route compiler advances native script entries by one fixed ticket,
matching the host input boundary's N-to-N+1 publication contract; this keeps
the exact accepted stream stable instead of rebaselining around test timing.

REBASELINE 2026-08-05 (d74efe02 -> 53c8ca2c). The superseded hash froze a
second port defect, in the same shape as the first: 670c984 corrected the
8th-place racing-line selector in func_80042D20(). racePosition is 1-indexed
(1..8) and D_800DCDA0 holds eight entries, so 8th place reads index 8, one past
the end. The port had clamped that to D_800DCDA0[7] == 2, a value the retail
game never produces; on hardware the read lands in the adjacent table and
yields D_800DCDA8[0] == 1.

That is not an argument from the disassembly's symbol names -- it is checked
against the shipped ROM on every run by check_racing_line_overrun_witness()
below, which locates the two tables by content and asserts their adjacency and
the overrun byte. The pin moved only after the ROM said which side was right,
and only after a build of this tree carrying nothing but the pre-670c984 clamp
reproduced d74efe02 exactly -- proving that single selector, and nothing else
in the 52 commits since the old pin (the bounded/extent-carrying inflate path,
the game-text terminator count, the texture-header handling, or the camera
obstruction port), accounts for the difference.

REBASELINE 2026-09-01 (53c8ca2c -> 191bee35). The crossplay campaign pinned
-ffp-contract=off across every engine lane (84b89c7d): with no flag set each
toolchain chose its own FMA fusion, so gameplay float state was not
bit-reproducible across native/wasm/mingw. The pin changes which float
roundings the compiled sim performs -- 84b89c7d measured 59 of 225 engine TUs
changing bytes, racer.c/collision.c/camera.c/particles.c among them, i.e. the
authoritative racer trajectory this oracle records -- so the raw ORACLE stream
reaches a different (still fully deterministic) sequence. The digest is neither
the old pin nor SUPERSEDED_SHA256, so the racing-line clamp is unaffected; the
row count and every field schema are unchanged, only the values drift. The
online direct-boot golden was re-minted for exactly this codegen change in the
same campaign (d6bbad62, hashVisible=hashPeer=db805fd2), but this offline oracle
was not re-frozen alongside it -- that omission is the whole delta. The trig
tables baked from the vendored .s in the same campaign (9fe5bbf5) are
hash-neutral (byte-identical to the load-time libm generation this tree
measured), and the sim_hash.c file sink (b624b584/b1064204) is I/O-only: it
mirrors the identical [SIMHASH] line to MDKR_STATE_HASH_FILE, which this lane
never sets, and never touches the hash or the sim. Measured on a fresh Release
build at 21b0519d carrying the pin (-ffp-contract=off in 225 TUs).

ENHANCED ARM, first pin 2026-09-03. Until this arm existed, every gate in the
tree that recorded an RNG stream recorded the ORIGINAL cadence only (this
oracle, tests/check_weather_rng_order.py's EXPECTED_ORIGINAL_SHA256,
tests/check_state_hash.py's cadence default), so the second compatibility
target named at platform/math_util_native.c -- "the pre-FPS native gameplay
stream at opt-in enhanced cadence" -- was held by nothing and could move
silently. docs/ref/presentation-rng-census.md states that gap as the reason it
declined to redirect its eight latent presentation-output callers. This arm
closes it: the same all-racer row schema and raw digest, recorded on the
route's own `enhanced` arm (9500 frames, one synthetic field, event divisor 1).

That move has now happened -- once, as planned; see the ENHANCED_SHA256
rebaseline note below. From here the enhanced arm is a pin like any other, and
a change to it is a regression until argued otherwise.

DRAW COUNTS, added 2026-09-03 after review. A stream digest is a weaker
instrument than it looks for the question this file exists to answer. The ROM
generator (game/src/hasm/math_util.c) is not injective: from the boot seed
0x5141564D it enters a cycle of period 20 after 11 draws, and thereafter
gCurrentRNGSeed only ever takes 20 values. Two builds whose AUTHORITATIVE draw
counts differ by k therefore record byte-identical streams whenever
k == 0 (mod 20), and differ only in phase otherwise. Measured directly on this
tree with the MDKR_TEST_AUTH_RNG_BURN seam: 20 extra authoritative draws leave
BOTH arms' digests exactly as pinned here, and every other stream oracle in the
tree green.

So each arm also pins how many times each generator was stepped, read from the
run's own [RNGDRAWS] line. That is the observable with no blind spot -- it moves
one for one -- and it is what actually holds the redirects this campaign made,
because what a cadence-conditional redirect changes IS a draw count. The burn
seam is driven as the control: it must leave the digest identical and fail the
count, or the count golden has stopped being independent.
"""

from __future__ import annotations

import argparse
import hashlib
import math
import os
import re
import subprocess
import sys
import tempfile
from dataclasses import dataclass, replace
from pathlib import Path

from harness_utils import DEFAULT_BUILD_DIR, resolve_binary

ROOT = Path(__file__).resolve().parent.parent
ROUTE_TOOL = ROOT / "tools" / "dkr_oracle_route.py"
ROUTE = "race_state_oracle"
FRAMES = 4800
REFERENCE_COMMIT = "670c984837ce21a9bd5ff54f0a2d4339267fb872"
EXPECTED_ROWS = 27_840
EXPECTED_SHA256 = "191bee35a973b2bde6133cc6ae2c2c41961a97ec72a9b034a08574d53aacba5b"
ENHANCED_FRAMES = 9500
ENHANCED_ROWS = 54_880
# Authoritative and presentation DRAW COUNTS over each arm, from the run's own
# [RNGDRAWS] line. See the DRAW COUNTS note in the module docstring: these are
# what the digests cannot supply. The original arm never touches the
# presentation stream, because cadence_compat_rand_range() routes back to
# rand_range() at two fields; that zero is itself an assertion.
EXPECTED_AUTH_DRAWS = 27_699
EXPECTED_PRES_DRAWS = 0
ENHANCED_AUTH_DRAWS = 16_556
ENHANCED_PRES_DRAWS = 25_510
# Positive control for the draw-count goldens: burn this many extra
# authoritative draws on the first presented frame. It is a multiple of the
# generator's cycle length, so every recorded stream stays byte-identical and
# every digest in the tree still passes -- only the count moves.
AUTH_BURN_CONTROL = 20
RNG_CYCLE_PERIOD = 20
# REBASELINE 2026-09-03 (64bf3d28 -> c2ac09ae), the one move this arm was
# pinned in order to measure. The eight callers docs/ref/presentation-rng-
# census.md listed as latent presentation-output now draw through
# cadence_compat_rand_range(): engine jitter (audio_vehicle.c), the boss voice
# pick (vehicle_tricky.c), the menu-image fields and the credits cheat pick
# (menu.c). At the shipping two-field cadence that helper still calls
# rand_range(), so the ORIGINAL arm is byte-identical -- 191bee35, unchanged
# across all four redirects, which is the whole classification argument. At the
# enhanced cadence the draws move to the presentation stream, so the
# authoritative stream this arm records is exactly 989-odd engine-jitter draws
# shorter and reaches a different, still fully deterministic sequence.
#
# Only the engine jitter moved this digest; the other three classes moved it
# not at all. That is a statement about what a digest can see, not about what
# the redirects did. The boss sound and the credits are genuinely not reached on
# this route. The menu-image class IS reached -- 10 menu_image_load() calls, 3
# draws each -- and its net effect on the route is exactly -20 authoritative
# draws (16576 -> 16556, measured), which is one full turn of the generator's
# 20-draw cycle and therefore invisible to any digest by construction. The
# engine jitter registering here was phase, not sensitivity: its diverted count
# is not a multiple of 20, so it shifted the cycle and every consumer with it.
#
# This is why the arms carry draw-count goldens as well. Do not read an unmoved
# digest as an unmoved stream.
ENHANCED_SHA256 = "c2ac09ae9928a67c89551284b97bee874913bcfeee5b3192de59c12dc4194dcd"
# The pre-redirect stream, kept named so a bisect landing on it reports which
# pin it matched rather than an unexplained mismatch.
ENHANCED_SUPERSEDED_SHA256 = (
    "64bf3d28463664f716eb348d16c7fa2b36806cc32c89c1bb171a7b0a1f960767"
)
# Superseded by the racing-line rebaseline documented above. Kept named so a
# bisect that lands on the old stream reports which pin it matched.
SUPERSEDED_SHA256 = "d74efe02aec07aa59710ce457e54180c28a22022f3d35e7087096d5130dba49b"
ARES_VEHICLE_RNG_PREFIX_SHA256 = (
    "9fd7cb9aebc163b00f9c8e4bfd292f90b684b4d46415ab5e0ef594c8bfb2d16e"
)
FIELDS = (
    "frame", "map", "slot", "x", "y", "z", "xv", "yv", "zv", "fvel",
    "vel", "cp", "next", "lap", "countlap", "fin", "fpos", "ridx",
    "pidx", "vehicle", "grounded", "clock", "start", "delta", "rate", "rng",
)
FLOAT_FIELDS = {"x", "y", "z", "xv", "yv", "zv", "fvel", "vel"}


@dataclass(frozen=True)
class Arm:
    """One cadence of the route, with the stream it is pinned to.

    ``frames``/``cadence``/``synth_fields`` are also declared in the route
    (tools/oracle_routes/race_state_oracle.json). They are repeated here so the
    pin says out loud what it recorded, and checked against the route on every
    run by :func:`check_route_agreement`, so an edit to either side that
    silently re-aims a digest at a different route fails instead of passing.
    """

    name: str
    cadence: str
    synth_fields: int
    frames: int
    rows: int
    digest: str
    auth_draws: int
    pres_draws: int


ORIGINAL_ARM = Arm(
    name="original", cadence="original", synth_fields=2, frames=FRAMES,
    rows=EXPECTED_ROWS, digest=EXPECTED_SHA256,
    auth_draws=EXPECTED_AUTH_DRAWS, pres_draws=EXPECTED_PRES_DRAWS,
)
ENHANCED_ARM = Arm(
    name="enhanced", cadence="enhanced", synth_fields=1, frames=ENHANCED_FRAMES,
    rows=ENHANCED_ROWS, digest=ENHANCED_SHA256,
    auth_draws=ENHANCED_AUTH_DRAWS, pres_draws=ENHANCED_PRES_DRAWS,
)
ARMS = (ORIGINAL_ARM, ENHANCED_ARM)


# game/src/racer.c's three consecutive s8 tables, by content. The AI balloon
# table is included so the signature is long enough to be unique in the image;
# the assertion is that D_800DCDA8 begins in the byte immediately after
# D_800DCDA0's eighth, which is what makes index 8 read a 1 and not a 2.
AI_BALLOON_ACTION_TABLE = bytes((1, 1, 2, 2, 4, 3, 0, 6, 4, 3, 2, 2, 5, 5, 5, 0))
RACING_LINE_TABLE = bytes((0, 0, 0, 1, 1, 2, 2, 2))          # D_800DCDA0
RACING_LINE_NEIGHBOUR = bytes((1, 1, 1, 2, 3, 2, 3, 2))      # D_800DCDA8
RACING_LINE_OVERRUN_VALUE = 1     # D_800DCDA8[0]: what 8th place actually reads
RACING_LINE_CLAMPED_VALUE = 2     # D_800DCDA0[7]: the invented clamp it replaced


def check_racing_line_overrun_witness(image: bytes) -> int:
    """Prove from the ROM which value 8th place's out-of-bounds read produces.

    Returns the ROM offset of D_800DCDA0. Raises when the tables are not
    adjacent, are not unique, or when the byte one past D_800DCDA0 is not the
    value this gate's stream was rebaselined onto -- any of which would mean the
    pin rests on an assumption the shipped image does not support.
    """
    # Locate D_800DCDA0 by the run that ENDS at it, so the bytes the assertions
    # are about are not themselves part of what is being searched for.
    signature = AI_BALLOON_ACTION_TABLE + RACING_LINE_TABLE
    hits = []
    start = image.find(signature)
    while start >= 0:
        hits.append(start)
        start = image.find(signature, start + 1)
    if len(hits) != 1:
        raise ValueError(
            "racing-line witness: expected exactly one AI-balloon/D_800DCDA0 "
            f"run in the ROM, found {len(hits)}")
    table = hits[0] + len(AI_BALLOON_ACTION_TABLE)
    past = table + len(RACING_LINE_TABLE)
    # Overrun byte first, so each assertion below is independently falsifiable:
    # it shares its value with RACING_LINE_NEIGHBOUR[0], and the adjacency test
    # would otherwise absorb every mutation of it.
    if image[past] != RACING_LINE_OVERRUN_VALUE:
        raise ValueError(
            f"racing-line witness: the byte past D_800DCDA0 (ROM {past:#x}) is "
            f"{image[past]}, not {RACING_LINE_OVERRUN_VALUE} -- the stream pin "
            "assumes that is what 8th place reads")
    if image[past:past + len(RACING_LINE_NEIGHBOUR)] != RACING_LINE_NEIGHBOUR:
        raise ValueError(
            f"racing-line witness: D_800DCDA0 (ROM {table:#x}) is not followed "
            "by D_800DCDA8 -- the two tables the pin assumes are adjacent are "
            "not adjacent in this image")
    if image[past - 1] != RACING_LINE_CLAMPED_VALUE:
        raise ValueError(
            "racing-line witness: D_800DCDA0[7] is not the superseded clamp "
            f"value {RACING_LINE_CLAMPED_VALUE}")
    return table


def check_route_agreement(arm: Arm) -> None:
    """Fail when the arm this file pins is not the arm the route defines."""
    for field, expected in (("cadence", arm.cadence),
                            ("synth_fields", arm.synth_fields),
                            ("frames", arm.frames)):
        query = subprocess.run(
            [sys.executable, str(ROUTE_TOOL), "arm-field", ROUTE, arm.name, field],
            cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            timeout=30, check=False,
        )
        if query.returncode != 0:
            raise RuntimeError(
                f"route arm-field {arm.name}.{field} exited "
                f"{query.returncode}:\n{query.stdout}")
        actual = (query.stdout or "").strip()
        if actual != str(expected):
            raise ValueError(
                f"{arm.name} arm: route says {field}={actual!r}, this pin was "
                f"recorded with {field}={expected!r} -- the digest below does "
                "not describe the route that would run")


class DrawCountError(ValueError):
    """A draw-count golden moved. Distinct so the control can require it."""


def validate_draw_counts(result: Run, arm: Arm) -> None:
    """Pin how many times each generator was stepped over the arm.

    This is not a restatement of the digest. The ROM generator enters a cycle
    of period 20 eleven draws after boot, so any change to the authoritative
    draw count that is a multiple of 20 leaves the seed -- and therefore every
    recorded row, and therefore the digest -- byte-identical. Measured on this
    tree: burning 20 extra authoritative draws leaves BOTH arms' digests exactly
    as pinned above. The count is the observable with no such blind spot.
    """
    for label, actual, expected in (
            ("authoritative", result.auth_draws, arm.auth_draws),
            ("presentation", result.pres_draws, arm.pres_draws)):
        if actual != expected:
            delta = actual - expected
            note = ""
            if label == "authoritative" and delta % RNG_CYCLE_PERIOD == 0:
                note = (f" -- and {delta:+d} is a multiple of the generator's "
                        f"{RNG_CYCLE_PERIOD}-draw cycle, so the stream digest "
                        "cannot see this at all")
            raise DrawCountError(
                f"{arm.name} arm: {label} draw count {actual}, expected "
                f"{expected} ({delta:+d}){note}")


def validate(rows: list[str], arm: Arm) -> str:
    if len(rows) != arm.rows:
        raise ValueError(
            f"{arm.name} arm: expected {arm.rows} [ORACLE] rows, got {len(rows)}")
    for row_index, row in enumerate(rows):
        tokens = row.split()
        if tokens[:2] != ["[TRACE]", "[ORACLE]"] or len(tokens) != len(FIELDS) + 2:
            raise ValueError(f"row {row_index}: malformed prefix/field count: {row}")
        for expected, token in zip(FIELDS, tokens[2:]):
            key, separator, value = token.partition("=")
            if separator != "=" or key != expected or not value:
                raise ValueError(f"row {row_index}: expected {expected}=..., got {token!r}")
            try:
                if expected in FLOAT_FIELDS:
                    if not math.isfinite(float(value)):
                        raise ValueError("non-finite")
                else:
                    int(value, 10)
            except ValueError as exc:
                raise ValueError(f"row {row_index}: invalid {expected} value {value!r}") from exc
    digest = hashlib.sha256(("\n".join(rows) + "\n").encode("utf-8")).hexdigest()
    if digest == SUPERSEDED_SHA256:
        raise ValueError(
            f"raw stream SHA-256 {digest} is the SUPERSEDED pin: this build "
            "still clamps 8th place's racing-line selector to D_800DCDA0[7] "
            "instead of taking the adjacent table's first byte -- see the "
            "rebaseline note at the top of this file")
    if arm is ENHANCED_ARM and digest == ENHANCED_SUPERSEDED_SHA256:
        raise ValueError(
            f"enhanced arm: raw stream SHA-256 {digest} is the SUPERSEDED "
            "pre-redirect pin: this build still draws the eight presentation "
            "callers from the authoritative stream at enhanced cadence -- see "
            "the rebaseline note at the top of this file")
    if digest != arm.digest:
        raise ValueError(
            f"{arm.name} arm: raw stream SHA-256 {digest}, expected {arm.digest}")
    return digest


@dataclass(frozen=True)
class Run:
    rows: list[str]
    auth_draws: int
    pres_draws: int


def parse_draw_counts(output: str) -> tuple[int, int]:
    """Read the run's own [RNGDRAWS] summary line.

    Missing or duplicated is an error rather than a default: a silently absent
    counter would make the draw-count goldens below pass on nothing.
    """
    lines = [line for line in output.splitlines() if "[RNGDRAWS]" in line]
    if len(lines) != 1:
        raise ValueError(
            f"expected exactly one [RNGDRAWS] line, got {len(lines)} -- the "
            "draw-count goldens have nothing to compare against")
    match = re.search(r"\[RNGDRAWS\] auth=(\d+) pres=(\d+)\s*$", lines[0])
    if match is None:
        raise ValueError(f"malformed [RNGDRAWS] line: {lines[0]!r}")
    return int(match.group(1)), int(match.group(2))


def run(binary: Path, rom: Path, arm: Arm, timeout: int, verbose: bool,
        auth_burn: int = 0) -> Run:
    with tempfile.TemporaryDirectory(prefix="mdkr-authored-rng-") as tmp:
        run_dir = Path(tmp)
        script = run_dir / f"race_state_oracle_{arm.name}.txt"
        route = subprocess.run(
            [sys.executable, str(ROUTE_TOOL), "native-script", ROUTE,
             "--arm", arm.name],
            cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            timeout=30, check=False,
        )
        if route.returncode != 0:
            raise RuntimeError(f"route compiler exited {route.returncode}:\n{route.stdout}")
        script.write_text(route.stdout, encoding="utf-8")

        env = {key: value for key, value in os.environ.items()
               if not key.startswith(("MDKR", "GE007_"))}
        env.update(
            LC_ALL="C", MDKR_AUDIO="0", MDKR_SIMULATION_CADENCE=arm.cadence,
            MDKR_SYNTH_FIELDS=str(arm.synth_fields), MDKR_TRACE="1",
            MDKR_ORACLE_STATE="1",
            MDKR_RENDERER="gl", MDKR_SAVE_DIR=str(run_dir / "save"),
            # Isolate the video config with the save (see check_door_blocks.py).
            MDKR_VIDEO_CONFIG_PATH=str(run_dir / "save" / "video.ini"),
        )
        if auth_burn:
            env["MDKR_TEST_AUTH_RNG_BURN"] = str(auth_burn)
        command = [str(binary), "--headless-frames", str(arm.frames),
                   "--input-script", str(script), "--rom", str(rom)]
        if verbose:
            print("$ " + " ".join(command), flush=True)
        process = subprocess.run(
            command, cwd=run_dir, env=env, text=True, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, timeout=timeout, check=False,
        )
        if process.returncode != 0:
            raise RuntimeError(
                f"mdkr64 exited {process.returncode}:\n{(process.stdout or '')[-4000:]}")
        output = process.stdout or ""
        auth, pres = parse_draw_counts(output)
        return Run(rows=[line for line in output.splitlines() if "[ORACLE]" in line],
                   auth_draws=auth, pres_draws=pres)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", default=DEFAULT_BUILD_DIR)
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    parser.add_argument("--timeout", type=int, default=600)
    parser.add_argument("--arm", choices=[arm.name for arm in ARMS], action="append",
                        help="run only this cadence arm (repeatable; default both)")
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()
    arms = ARMS if not args.arm else tuple(
        arm for arm in ARMS if arm.name in set(args.arm))

    binary = Path(os.path.abspath(resolve_binary(args.build)))
    rom = Path(os.path.abspath(args.rom))
    for path in (binary, rom, ROUTE_TOOL):
        if not path.exists():
            print(f"FAIL: missing {path}", file=sys.stderr)
            return 1
    try:
        image = rom.read_bytes()
        table = check_racing_line_overrun_witness(image)
        # Witness control: the witness must reject an image whose overrun byte
        # is the superseded clamp value, or it is not testing anything.
        mutated = bytearray(image)
        mutated[table + len(RACING_LINE_TABLE)] = RACING_LINE_CLAMPED_VALUE
        try:
            check_racing_line_overrun_witness(bytes(mutated))
        except ValueError:
            pass
        else:
            raise RuntimeError("racing-line witness accepted a mutated ROM")
        del image, mutated

        # In-process control for check_route_agreement, so a route query that
        # silently stopped answering cannot leave the pins unguarded.
        skewed = replace(ORIGINAL_ARM, synth_fields=ORIGINAL_ARM.synth_fields + 1)
        try:
            check_route_agreement(skewed)
        except ValueError:
            pass
        else:
            raise RuntimeError(
                "route-agreement control was not rejected: an arm declaring a "
                "synth_fields the route does not define was accepted")

        digests: dict[str, str] = {}
        for arm in arms:
            check_route_agreement(arm)
            result = run(binary, rom, arm, args.timeout, args.verbose)
            digests[arm.name] = validate(result.rows, arm)
            validate_draw_counts(result, arm)

            # Broken-oracle control: prove a one-field change is rejected.
            broken = list(result.rows)
            old_rng = broken[0].rsplit("rng=", 1)[1]
            broken[0] = broken[0].rsplit("rng=", 1)[0] + f"rng={int(old_rng) ^ 1}"
            try:
                validate(broken, arm)
            except ValueError:
                pass
            else:
                raise RuntimeError(
                    f"{arm.name} arm: mutated-row positive control was not rejected")

        # Positive control for the draw-count golden, and the demonstration of
        # why it exists. Burning AUTH_BURN_CONTROL extra authoritative draws --
        # one full cycle of the generator -- returns the seed to where it
        # started, so this arm's DIGEST is byte-identical to the pin above and
        # every stream oracle in the tree still passes. The count must catch it
        # anyway. If this control ever stops failing, the count golden has
        # stopped being an independent observable and the arm is back to being
        # blind to any draw-count change that is a multiple of the cycle.
        if ENHANCED_ARM in arms:
            burned = run(binary, rom, ENHANCED_ARM, args.timeout, args.verbose,
                         auth_burn=AUTH_BURN_CONTROL)
            burned_digest = hashlib.sha256(
                ("\n".join(burned.rows) + "\n").encode("utf-8")).hexdigest()
            if burned_digest != ENHANCED_ARM.digest:
                raise RuntimeError(
                    "draw-count control: burning one full generator cycle "
                    f"changed the stream digest to {burned_digest} -- the "
                    f"cycle period is not {RNG_CYCLE_PERIOD}, so this control "
                    "no longer isolates what the digest cannot see")
            try:
                validate_draw_counts(burned, ENHANCED_ARM)
            except DrawCountError:
                pass
            else:
                raise RuntimeError(
                    "draw-count control: burning "
                    f"{AUTH_BURN_CONTROL} authoritative draws moved neither "
                    "the digest nor the count -- the count golden is vacuous")

        # The two arms must not collapse onto one stream. Either arm could be
        # made to pass on the wrong cadence -- by a pasted constant, but also by
        # a cadence selector that ignored MDKR_SIMULATION_CADENCE, or a route
        # arm edited to duplicate the other -- and this catches all of them
        # without caring which happened.
        if len(digests) == 2 and len(set(digests.values())) != 2:
            raise RuntimeError(
                "the original and enhanced arms produced the same digest -- "
                "one of them did not take the cadence it asked for")
    except (OSError, RuntimeError, subprocess.TimeoutExpired, ValueError) as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        return 1

    summary = ", ".join(
        f"{arm.name} {arm.rows} rows {digests[arm.name][:12]} "
        f"{arm.auth_draws} auth draws"
        for arm in arms if arm.name in digests)
    print(
        "check_authored_rng_compat: PASS "
        f"({summary}, reference {REFERENCE_COMMIT[:12]}, "
        f"ares car-RNG witness {ARES_VEHICLE_RNG_PREFIX_SHA256[:12]}, "
        f"racing-line overrun witness ROM {table:#x}+8 == "
        f"{RACING_LINE_OVERRUN_VALUE})"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
