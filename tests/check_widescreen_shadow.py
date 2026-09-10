#!/usr/bin/env python3
"""Closed-loop widescreen and shadow regression.

This is deliberately ROM-backed and therefore is not registered with CTest.
It complements the millisecond, ROM-free display_config unit test with five
real-game arms plus an untraced shipping arm:

  * 4:3, 16:9 and 21:9 must emit identical normalized [PACE] state streams.
    That catches the subtle DKR failure where widening object visibility changes
    AI/physics/RNG through render-side effects even though every frame looks fine.
  * Normal shadow heaps must stay in bounds and every shadow group must request
    decal depth mode.
  * Tiny fault-injection heaps must drop whole meshes without crashing or
    crossing the physical allocation.
  * MDKR_SHADOW_DECAL=0 is a positive control: the same route must report real
    non-decal groups and fewer final ZMODE_DEC triangles, proving the check did
    not pass because it rendered no shadows or because a source flag was lost
    before reaching the renderer.

Usage:
    python3 tests/check_widescreen_shadow.py \
        --build build-ws-webgpu --rom baserom.us.v80.z64 -v

Run it again against an ASan build to validate the forced-overflow arm under the
allocator:
    python3 tests/check_widescreen_shadow.py \
        --build build-ws-asan --rom baserom.us.v80.z64
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shlex
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass
from pathlib import Path

from harness_utils import DEFAULT_BUILD_DIR, resolve_binary, save_env


REPO = Path(__file__).resolve().parent.parent
SCRIPT = REPO / "tests" / "input_scripts" / "nav_to_time_trial_race.txt"
MIN_FRAMES = 3400

SHADOW_RE = re.compile(
    r"\[SHADOW\] decal=(on|off) "
    r"dataPeak=(\d+)/(\d+) triPeak=(\d+)/(\d+) vtxPeak=(\d+)/(\d+) "
    r"overflowDrops=(\d+) emptyMeshes=(\d+) drawGroups=(\d+) nonDecal=(\d+)"
)
DEPTH_RE = re.compile(
    r"\[DEPTH\] decalTriangles=(\d+) comparedTriangles=(\d+)"
)
COMPLETION_RE = re.compile(
    r"\[SDL\] headless: reached (\d+) frames, exiting cleanly\."
)


@dataclass(frozen=True)
class ShadowStats:
    decal: str
    data_peak: int
    data_cap: int
    tri_peak: int
    tri_cap: int
    vtx_peak: int
    vtx_cap: int
    overflow_drops: int
    empty_meshes: int
    draw_groups: int
    non_decal: int


@dataclass(frozen=True)
class DepthStats:
    decal_triangles: int
    compared_triangles: int


@dataclass
class RunResult:
    label: str
    command: list[str]
    returncode: int
    output: str
    pace: list[str]
    shadow: ShadowStats | None
    depth: DepthStats | None
    requested_frames: int
    timed_out: bool = False
    timeout: int = 0
    elapsed_seconds: float = 0.0


def completed_route(result: RunResult) -> bool:
    """Exit success alone is insufficient: every arm must finish its route."""
    completions = COMPLETION_RE.findall(result.output)
    return (not result.timed_out and result.returncode == 0
            and completions == [str(result.requested_frames)])


def complete_pace(result: RunResult) -> bool:
    # The producer emits exactly one row per present, starting at frame 1.
    # Matching truncated, duplicated or reordered traces cannot prove parity.
    if len(result.pace) != result.requested_frames:
        return False
    for index, row in enumerate(result.pace, 1):
        match = re.match(r"\[PACE\] frame=(\d+) ", row)
        if match is None or int(match.group(1)) != index:
            return False
    return True


def write_evidence(root: Path, result: RunResult) -> None:
    # The caller creates a fresh private directory; never overwrite an arm.
    # Logs can contain ROM-derived state and belong outside public artifacts.
    arm = root / re.sub(r"[^A-Za-z0-9_-]", "_", result.label)
    arm.mkdir()
    with (arm / "process.log").open("x", encoding="utf-8") as stream:
        stream.write(result.output)
    record = {
        "schema": "mdkr-widescreen-shadow-process/1",
        "label": result.label,
        "command": result.command,
        "returncode": result.returncode,
        "timed_out": result.timed_out,
        "timeout_seconds": result.timeout,
        "elapsed_seconds": result.elapsed_seconds,
        "requested_frames": result.requested_frames,
        "completion_frames": COMPLETION_RE.findall(result.output),
        "pace_rows": len(result.pace),
    }
    with (arm / "process.json").open("x", encoding="utf-8") as stream:
        json.dump(record, stream, indent=2)
        stream.write("\n")


def normalized_pace(output: str) -> list[str]:
    rows: list[str] = []
    for line in output.splitlines():
        marker = line.find("[PACE]")
        if marker < 0:
            continue
        row = line[marker:]
        # Wall-clock measurement is intentionally non-deterministic; every
        # simulation field to either side must remain byte-identical.
        row = re.sub(r" dtms=\S+", " dtms=<wall>", row)
        rows.append(row)
    return rows


def parse_shadow(output: str) -> ShadowStats | None:
    matches = list(SHADOW_RE.finditer(output))
    if not matches:
        return None
    match = matches[-1]
    values = [int(value) for value in match.groups()[1:]]
    return ShadowStats(match.group(1), *values)


def parse_depth(output: str) -> DepthStats | None:
    matches = list(DEPTH_RE.finditer(output))
    if not matches:
        return None
    match = matches[-1]
    return DepthStats(int(match.group(1)), int(match.group(2)))


def clean_environment(renderer: str | None, traced: bool = True) -> dict[str, str]:
    env = {
        key: value
        for key, value in os.environ.items()
        if not key.startswith("MDKR")
    }
    # Prevent a developer's diagnostic shell from silently changing either arm.
    env.update(
        MDKR_AUDIO="0",
        MDKR_AUTOPILOT="1",
        MDKR_NO_CRASH_HANDLER="1",
        MDKR64_HIDDEN="1",
        LC_ALL="C",
    )
    # This gate's central assertion -- byte-identical [PACE] streams across
    # aspect ratios -- needs MDKR_TRACE, and MDKR_TRACE also arms engine
    # behaviour of its own (mdkr_resource_trace_enabled(); wave "shadowdeep" R1
    # in docs/open-items/renderer.md). The shipping arm below therefore carries
    # the strongest subset that survives without a trace row: the [SHADOW] heap
    # census and [DEPTH] counts are emitted unconditionally at shutdown, so the
    # production shadow path can still be shown to behave the same way with
    # every diagnostic off. This gate dumps no frames, so there is no pixel
    # evidence to compare here -- check_world_shadows.py, check_shadow_visual_ab.py
    # and check_shadow_plausibility.py carry that half.
    if traced:
        env["MDKR_TRACE"] = "1"
    if renderer:
        env["MDKR_RENDERER"] = renderer
    return env


def run_case(
    binary: Path,
    rom: Path,
    frames: int,
    label: str,
    size: str,
    base_env: dict[str, str],
    overrides: dict[str, str] | None,
    timeout: int,
    verbose: bool,
    evidence: Path | None = None,
) -> RunResult:
    env = dict(base_env)
    if overrides:
        env.update(overrides)
    command = [
        str(binary),
        "--headless-frames",
        str(frames),
        "--window-size",
        size,
        "--input-script",
        str(SCRIPT),
        "--rom",
        str(rom),
    ]
    if verbose:
        print(f"$ ({label}) " + shlex.join(command), flush=True)
    started = time.monotonic()
    try:
        # A fresh directory gives each arm an independent save. This keeps the
        # trajectory reproducible and lets GL/WebGPU/ASan harnesses run in
        # parallel without racing one EEPROM file. It has to be pinned, not
        # merely used as the cwd: clean_environment() drops the MDKR_SAVE_DIR
        # the suite exports per task, and since issue #54 an unpinned save
        # resolves to the SHARED per-user directory instead of $CWD/save, where
        # an unrelated adventure-in-progress EEPROM re-routes the boot flow and
        # the [PACE]/[SHADOW]/[DEPTH] rows this gate compares come from a
        # different route on each arm. save_env() pins the video config with it
        # (check_harness_isolation.py).
        with tempfile.TemporaryDirectory(prefix=f"mdkr_{label.replace(':', '_')}_") as run_dir:
            proc = subprocess.run(
                command,
                cwd=run_dir,
                env=save_env(dict(env), run_dir),
                text=True,
                capture_output=True,
                timeout=timeout,
                check=False,
            )
        output = proc.stdout + proc.stderr
        result = RunResult(
            label,
            command,
            proc.returncode,
            output,
            normalized_pace(output),
            parse_shadow(output),
            parse_depth(output),
            frames,
        )
    except subprocess.TimeoutExpired as exc:
        # TimeoutExpired carries the raw bytes even under text=True; decode
        # them so the timed-out arm reports a timeout, not a TypeError.
        captured = "".join(
            part.decode("utf-8", "replace") if isinstance(part, bytes)
            else (part or "")
            for part in (exc.stdout, exc.stderr)
        )
        result = RunResult(
            label, command, 124, captured, normalized_pace(captured),
            parse_shadow(captured), parse_depth(captured), frames, timed_out=True
        )
    except OSError as exc:
        result = RunResult(label, command, 127, f"Arm process/setup failed: {exc}\n",
                           [], None, None, frames)
    result.timeout = timeout
    result.elapsed_seconds = time.monotonic() - started
    if evidence is not None:
        write_evidence(evidence, result)
    if verbose and result.shadow:
        print(f"  {result.shadow}")
    if verbose and result.depth:
        print(f"  {result.depth}")
    return result


def run_failures(result: RunResult, traced: bool = True) -> list[str]:
    failures: list[str] = []
    if result.timed_out:
        failures.append(
            f"{result.label}: timed out after {result.timeout}s "
            f"({len(result.pace)} partial [PACE] rows); route/teardown incomplete"
        )
    elif result.returncode != 0:
        failures.append(f"{result.label}: exit code {result.returncode}")
    if not completed_route(result):
        failures.append(
            f"{result.label}: incomplete run; requires exit 0 and exactly one "
            f"clean completion at {result.requested_frames} frames "
            f"(completion reports: {COMPLETION_RE.findall(result.output)})"
        )
    for marker in ("[CRASH]", "[FATAL]", "AddressSanitizer",
                   "UndefinedBehaviorSanitizer", "runtime error:"):
        if marker in result.output:
            failures.append(f"{result.label}: output contains {marker}")
    # [PACE] is trace-gated: an untraced arm must have none, and an arm that
    # claims to be untraced but has some is not in shipping configuration.
    if bool(result.pace) != traced:
        failures.append(
            f"{result.label}: {len(result.pace)} [PACE] rows with MDKR_TRACE "
            f"{'set' if traced else 'unset'}")
    if traced and completed_route(result) and not complete_pace(result):
        failures.append(
            f"{result.label}: incomplete or out-of-order [PACE] trace; "
            f"requires frames 1..{result.requested_frames} exactly once"
        )
    if result.shadow is None:
        failures.append(f"{result.label}: no parseable [SHADOW] report")
    if result.depth is None:
        failures.append(f"{result.label}: no parseable [DEPTH] report")
    if result.shadow is None or result.depth is None:
        return failures

    stats = result.shadow
    for name, peak, cap in (
        ("data", stats.data_peak, stats.data_cap),
        ("tri", stats.tri_peak, stats.tri_cap),
        ("vtx", stats.vtx_peak, stats.vtx_cap),
    ):
        if not 0 <= peak <= cap:
            failures.append(
                f"{result.label}: {name} high-water {peak} outside capacity {cap}"
            )
    if stats.draw_groups <= 0:
        failures.append(f"{result.label}: rendered zero shadow groups")
    if result.depth.compared_triangles <= 0:
        failures.append(f"{result.label}: emitted zero depth-compared triangles")
    if not 0 <= result.depth.decal_triangles <= result.depth.compared_triangles:
        failures.append(
            f"{result.label}: decal-triangle count "
            f"{result.depth.decal_triangles} outside compared-triangle count "
            f"{result.depth.compared_triangles}"
        )
    return failures


def first_pace_difference(reference: RunResult, candidate: RunResult) -> str:
    limit = min(len(reference.pace), len(candidate.pace))
    for index in range(limit):
        if reference.pace[index] != candidate.pace[index]:
            return (
                f"row {index} differs:\n"
                f"    {reference.label}: {reference.pace[index]}\n"
                f"    {candidate.label}: {candidate.pace[index]}"
            )
    return (
        f"row counts differ: {reference.label}={len(reference.pace)}, "
        f"{candidate.label}={len(candidate.pace)}"
    )


def pace_comparison_failures(reference: RunResult, candidate: RunResult) -> list[str]:
    if (not completed_route(reference) or not completed_route(candidate)
            or not complete_pace(reference) or not complete_pace(candidate)):
        # A stopped process has a shorter trace, not proof of altered simulation.
        # This remains a gate failure, including when the partial prefixes match.
        return [
            f"{candidate.label}: simulation comparison unavailable against "
            f"{reference.label}: incomplete run(s) or trace(s); partial streams are not "
            "evidence of simulation equivalence or divergence"
        ]
    if candidate.pace != reference.pace:
        return [
            f"{candidate.label}: simulation stream changed from {reference.label}; "
            + first_pace_difference(reference, candidate)
        ]
    return []


def print_failure_context(result: RunResult) -> None:
    print(f"\n--- {result.label}: last 50 output lines ---", file=sys.stderr)
    for line in result.output.splitlines()[-50:]:
        print(line, file=sys.stderr)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", default=DEFAULT_BUILD_DIR)
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    parser.add_argument("--frames", type=int, default=4000)
    # 180s per arm was sized against the NATIVE build. The same script is also
    # driven with the ASan binary (the widescreen_shadow_asan task), where the
    # sanitizer's instrumentation makes 4000 frames cost several times more --
    # and the arms landed at 3944 and 3965 of 4000, 98.6% and 99.1% of the way
    # through, before the wall. A ceiling that stops a route one frame from its
    # teardown is measuring the build's speed, not its correctness; every claim
    # this gate makes is read out of the [SHADOW] and [DEPTH] reports the run
    # emits at the end, so an arm that finishes slowly proves what a fast one
    # does. Sized clear of the sanitizer arm rather than beside the native one.
    parser.add_argument("--timeout", type=int, default=600, help="seconds per arm")
    parser.add_argument("--renderer", choices=("gl", "webgpu"), default=None)
    parser.add_argument("--keep-evidence", action="store_true",
                        help="retain full per-arm output/process metadata in a fresh "
                             "private temporary directory; never publish these logs")
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    binary = Path(resolve_binary(args.build)).resolve()
    rom = Path(args.rom)
    if not rom.is_absolute():
        rom = (REPO / rom).resolve()
    missing = [path for path in (binary, rom, SCRIPT) if not path.is_file()]
    if missing:
        for path in missing:
            print(f"FAIL: missing {path}", file=sys.stderr)
        return 1
    if args.frames < MIN_FRAMES:
        print(
            f"FAIL: --frames must be >= {MIN_FRAMES} so the route reaches live shadows",
            file=sys.stderr,
        )
        return 1
    if args.timeout <= 0:
        print("FAIL: --timeout must be positive", file=sys.stderr)
        return 1

    evidence = None
    if args.keep_evidence:
        evidence = Path(tempfile.mkdtemp(prefix="mdkr-widescreen-shadow-evidence-"))
        print(f"Private evidence (do not publish): {evidence}", flush=True)

    env = clean_environment(args.renderer)
    cases = [
        run_case(binary, rom, args.frames, "4:3", "640x480", env, None,
                 args.timeout, args.verbose, evidence),
        run_case(binary, rom, args.frames, "16:9", "960x540", env, None,
                 args.timeout, args.verbose, evidence),
        run_case(binary, rom, args.frames, "21:9", "1260x540", env, None,
                 args.timeout, args.verbose, evidence),
        run_case(
            binary,
            rom,
            args.frames,
            "tiny shadow heaps",
            "1260x540",
            env,
            {"MDKR_SHADOW_CAPS": "2,8,16"},
            args.timeout,
            args.verbose,
            evidence,
        ),
        run_case(
            binary,
            rom,
            args.frames,
            "decal positive control",
            "640x480",
            env,
            {"MDKR_SHADOW_DECAL": "0"},
            args.timeout,
            args.verbose,
            evidence,
        ),
    ]
    # Shipping configuration: the production 4:3 arm again with MDKR_TRACE
    # absent -- the way the shipped launchers and the web shell actually run.
    shipping = run_case(
        binary, rom, args.frames, "4:3 shipping",
        "640x480", clean_environment(args.renderer, traced=False), None,
        args.timeout, args.verbose, evidence,
    )

    failures: list[str] = []
    for result in cases:
        failures.extend(run_failures(result))
    failures.extend(run_failures(shipping, traced=False))

    reference = cases[0]
    if (completed_route(shipping) and completed_route(reference)
            and shipping.shadow and reference.shadow):
        for field in ("decal", "data_cap", "tri_cap", "vtx_cap",
                      "overflow_drops", "non_decal"):
            traced_value = getattr(reference.shadow, field)
            ship_value = getattr(shipping.shadow, field)
            if traced_value != ship_value:
                failures.append(
                    f"4:3 shipping: shadow {field} is {ship_value} with "
                    f"MDKR_TRACE unset against {traced_value} with it set. A "
                    f"diagnostic variable is changing the production shadow "
                    f"path, so every measurement above describes a program "
                    f"nobody ships (see wave 'shadowdeep' R1)"
                )
    for candidate in cases[1:3]:
        failures.extend(pace_comparison_failures(reference, candidate))

    # The diagnostic changes only the shadow render mode. It must not feed back
    # into race state, which also makes the visual A/B frames directly comparable.
    failures.extend(pace_comparison_failures(reference, cases[4]))

    for result in cases[:3]:
        if result.shadow:
            if result.shadow.decal != "on":
                failures.append(f"{result.label}: production decal mode is not on")
            if result.shadow.overflow_drops != 0:
                failures.append(
                    f"{result.label}: {result.shadow.overflow_drops} unexpected overflow drops"
                )
            if result.shadow.non_decal != 0:
                failures.append(
                    f"{result.label}: {result.shadow.non_decal} non-decal shadow groups"
                )

    forced = cases[3].shadow
    if forced:
        if forced.overflow_drops <= 0:
            failures.append("tiny shadow heaps: positive control caused no overflow drops")
        if forced.non_decal != 0:
            failures.append(
                f"tiny shadow heaps: {forced.non_decal} non-decal shadow groups"
            )

    disabled = cases[4].shadow
    disabled_depth = cases[4].depth
    if disabled and disabled_depth:
        if disabled.decal != "off":
            failures.append("decal positive control: diagnostic did not disable decal mode")
        if disabled.non_decal <= 0:
            failures.append(
                "decal positive control: no non-decal groups observed; "
                "the test is not sensitive to the old bug"
            )
        production_depth = reference.depth
        if (production_depth is not None and completed_route(reference)
                and completed_route(cases[4])
                and production_depth.decal_triangles <= disabled_depth.decal_triangles):
            failures.append(
                "decal positive control: final decoded ZMODE_DEC triangle count "
                f"did not decrease ({production_depth.decal_triangles} -> "
                f"{disabled_depth.decal_triangles})"
            )

    if failures:
        print("FAIL: widescreen/shadow regression", file=sys.stderr)
        for failure in failures:
            print(f"  - {failure}", file=sys.stderr)
        for result in [*cases, shipping]:
            if (any(failure.startswith(result.label + ":") for failure in failures)
                    or result.returncode != 0):
                print_failure_context(result)
        return 1

    normal = reference.shadow
    forced = cases[3].shadow
    assert normal is not None and forced is not None
    print(
        "PASS: 4:3/16:9/21:9 simulation streams match; "
        f"{normal.draw_groups} production shadow groups all use decal depth; "
        f"tiny heaps dropped {forced.overflow_drops} complete meshes safely; "
        "the no-decal positive control changed final decoded depth state without "
        "changing simulation"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
