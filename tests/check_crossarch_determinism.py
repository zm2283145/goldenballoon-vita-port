#!/usr/bin/env python3
"""Cross-architecture determinism: arm64 and x86_64 must agree bit for bit.

Why this exists
---------------
The online compatibility identity (platform/online/compatibility_identity.h) is
derived from version, source commit, source-dirty and ROM revision. It does NOT
carry the CPU architecture. Matchmaking will therefore happily seat an ARM64
macOS player against an x86_64 Windows player in the same lockstep session.

Every determinism instrument this project already owns runs one binary on one
host: check_determinism.py re-runs the same executable, check_state_hash.py
re-runs the same executable, and the rollback convergence gates replay inside a
single process. All of them hold the architecture fixed, so none of them can
observe an architecture-dependent simulation. If the simulation is not
bit-identical across architectures, that is a live desync in shipped online
play, and no gate in the tree would notice.

The measurement is available on one Apple Silicon workstation: build a thin
x86_64 executable next to the native thin arm64 one, drive both through the
same ROM and the same input script, and compare the authoritative [SIMHASH]
streams tick by tick.

What it asserts
---------------
For each route:

1. **Cross-architecture identity.** The x86_64 stream and the arm64 stream are
   byte-identical for every tick. The report names the FIRST divergent tick, its
   two rows, and whether the population (``objs=``) or only the state digest
   moved -- a boolean would leave the next person bisecting from nothing.

2. **The two binaries really are different architectures.** Both are read with
   ``lipo -archs`` and must be thin, must disagree, and must together cover
   arm64 and x86_64. Two arm64 binaries compare identical for a reason that has
   nothing to do with the claim, and that pass would be silent.

3. **The two binaries were configured the same way.** Build type and every
   MDKR_* cache entry must match across the two CMakeCache.txt files. A Release
   arm64 against a Debug x86_64 measures optimisation level, not architecture.

4. **Neither binary is stale.** Both must be newer than the newest tracked
   source file, or the run is comparing two different games and calling the
   difference architectural.

5. **Positive control.** One extra arm64 arm runs with ``MDKR_RNGSEED=legacy``
   and must DIVERGE from the arm64 base stream. Without it, an empty or
   constant stream, a mis-set ``MDKR_STATE_HASH``, or a comparator that always
   returns "identical" passes this gate while proving nothing.

What it does NOT prove
----------------------
- **Not an OS gate.** Both binaries here run on macOS against the same kernel,
  the same dyld, and the same libSystem/libm images (the x86_64 slice of them,
  under Rosetta 2). It says nothing about Windows-vs-macOS, glibc-vs-msvcrt,
  or the wasm lane. A Windows x86_64 peer can still desync against a macOS
  x86_64 peer for reasons this gate cannot see.
- **Not a compiler gate.** Both binaries come from the same AppleClang. A
  divergence caused by GCC or MinGW codegen on the Windows lane is out of
  scope.
- **Not a proof about untravelled code.** It covers exactly the routes it
  drives. Cross-architecture divergence has every reason to be level-shaped:
  the mechanisms that make a simulation host-dependent (uninitialised reads,
  recycled pool bytes, pointer-derived values) are the same ones that made
  levels 41, 37 and 11 fail their own self-consistency arm in
  check_state_hash.py, and each of those needed a level that reaches it. The
  default route set includes them for that reason.
- **Not a claim about Rosetta 2 fidelity.** Rosetta translates x86_64 SSE to
  NEON. Apple documents IEEE-correct scalar semantics, but a divergence found
  here is a divergence between "this arm64 binary" and "this x86_64 binary as
  Rosetta executes it", which is what a Rosetta player would actually run and
  is NOT necessarily what an Intel Mac or a Windows PC would compute. Confirm a
  positive finding on real x86_64 hardware before calling it the whole story.

Usage
-----
    tests/check_crossarch_determinism.py [--build build] \
        [--build-x86 build-x86_64] [--rom baserom.us.v80.z64] \
        [--level 5 --level 37 --level 11] [--frames 3600] [-v]

``--build``/``--build-x86`` take a build directory or the executable inside it,
exactly like every other check. See docs/architecture/cross-architecture-
determinism.md for how to produce the x86_64 tree. Exit 0 = pass.
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import NamedTuple

from harness_utils import DEFAULT_BUILD_DIR, resolve_binary

ROOT = Path(__file__).resolve().parent.parent
SCRIPT = ROOT / "tests" / "input_scripts" / "nav_to_time_trial_race.txt"

# The same route, field set and tick budget check_state_hash.py anchors on, so a
# divergence found here is expressed in the same coordinates as every other
# authoritative-state finding in the tree.
DEFAULT_FRAMES = 3600
HASH_VERSION = "3"

# Levels whose authoritative state is known to depend on memory the engine did
# not write. They are the measured host-dependence hotspots from
# check_state_hash.py's process-nondeterminism arms, and an architecture change
# moves stack frames, allocation sizes and pool recycling order -- which is
# exactly what those defects read. Level 5 is the anchor route.
DEFAULT_LEVELS = (5, 37, 11)

WINDOW = "320x240"

# sim_hash.c formats the digest with %016llx, so a well-formed row carries
# exactly 16 hex characters. Pinning the WIDTH is what makes a torn final line
# -- the tail a killed or disk-full run leaves behind -- fail to match instead
# of parsing into a short digest that then "differs" from its peer for an I/O
# reason rather than an architectural one.
SIMHASH_RE = re.compile(
    r"^\[SIMHASH\] tick=(\d+) objs=(-?\d+) h=([0-9a-f]{16})\s*$")

# Cache entries that must agree between the two build trees. Anything that can
# change generated code changes what a float computes, so a mismatch here is
# reported as a configuration failure rather than an architecture finding.
SHARED_CACHE_KEYS = (
    "CMAKE_BUILD_TYPE",
    "CMAKE_C_FLAGS",
    "CMAKE_CXX_FLAGS",
    "CMAKE_C_FLAGS_RELEASE",
    "CMAKE_CXX_FLAGS_RELEASE",
    "CMAKE_INTERPROCEDURAL_OPTIMIZATION",
)

# Trees whose contents compile into the executable. A binary older than the
# newest file here was built from different source than the one on disk.
SOURCE_TREES = ("game", "platform", "include", "src", "lib")
SOURCE_SUFFIXES = (".c", ".h", ".cpp", ".hpp", ".m", ".mm", ".inc")


class SimHashRow(NamedTuple):
    """One authoritative tick as sim_hash.c published it."""

    tick: int
    objs: int
    digest: str
    raw: str


# --------------------------------------------------------------------------- #
#  Pure helpers (unit-tested by tests/test_crossarch_determinism.py)
# --------------------------------------------------------------------------- #


def parse_lipo_archs(text: str) -> tuple[str, ...]:
    """The architecture slices ``lipo -archs`` named, in the order printed.

    ``lipo -archs`` prints one whitespace-separated line: ``arm64``,
    ``x86_64``, or ``x86_64 arm64`` for a universal binary. Returning a tuple
    rather than a bool keeps the caller able to say WHICH slices it found,
    which is the whole content of the "these are not two architectures"
    failure.
    """

    return tuple(text.split())


# Mach-O slice names to the architecture FAMILY they belong to. Apple ships
# sub-variants (arm64e for pointer-authenticated system binaries, x86_64h for
# Haswell-and-later) that are the same architecture for this gate's purposes:
# the claim being made is "these two binaries are different architectures", and
# arm64e-versus-arm64 does not satisfy it while arm64e-versus-x86_64 does.
# Mapping rather than string-matching also keeps an unexpected slice (i386, a
# future name) failing closed instead of quietly passing a prefix test.
ARCH_FAMILIES = {
    "arm64": "arm64",
    "arm64e": "arm64",
    "arm64_32": "arm64",
    "x86_64": "x86_64",
    "x86_64h": "x86_64",
}


def architecture_family(slice_name: str) -> str | None:
    """The family ``slice_name`` belongs to, or ``None`` when unrecognised."""

    return ARCH_FAMILIES.get(slice_name)


def architecture_problems(arm_archs: tuple[str, ...],
                          x86_archs: tuple[str, ...]) -> list[str]:
    """Reasons the pair cannot support a cross-architecture claim.

    Fail closed on three separate ways this gate could pass vacuously:

    - a universal (fat) binary, because the loader picks the native slice and
      an "x86_64" build would silently run as arm64;
    - a binary that is not the architecture its role requires; and
    - two binaries of the same architecture, which is a self-comparison wearing
      this check's name.
    """

    problems: list[str] = []
    families: list[str | None] = []
    for label, archs, want in (("--build", arm_archs, "arm64"),
                               ("--build-x86", x86_archs, "x86_64")):
        family = None
        if not archs:
            problems.append(
                f"{label}: could not read any architecture from the binary")
        elif len(archs) > 1:
            problems.append(
                f"{label}: universal binary ({' '.join(archs)}). The loader "
                "runs the native slice, so an x86_64 claim here would silently "
                "measure arm64. Configure each tree with a single "
                "CMAKE_OSX_ARCHITECTURES value so both executables are thin")
        else:
            family = architecture_family(archs[0])
            if family is None:
                problems.append(
                    f"{label}: unrecognised architecture {archs[0]!r}; this "
                    "gate only knows how to reason about arm64 and x86_64")
            elif family != want:
                problems.append(
                    f"{label}: binary is {archs[0]} ({family}), expected "
                    f"{want}")
        families.append(family)
    if not problems and families[0] == families[1]:
        problems.append(
            f"both binaries are {families[0]}; this gate would compare a "
            "build against itself and pass without testing anything")
    return problems


def parse_cmake_cache(text: str) -> dict[str, str]:
    """``NAME:TYPE=value`` entries from a CMakeCache.txt, as ``{NAME: value}``.

    Comment and blank lines are skipped, and so are the internal ``NAME-ADVANCED``
    bookkeeping rows, which carry no configuration meaning and would otherwise
    double the size of every mismatch report.
    """

    values: dict[str, str] = {}
    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith(("#", "//")):
            continue
        name, separator, value = line.partition("=")
        if not separator or ":" not in name:
            continue
        key = name.split(":", 1)[0]
        if key.endswith("-ADVANCED"):
            continue
        values[key] = value
    return values


def configuration_problems(arm_cache: dict[str, str],
                           x86_cache: dict[str, str]) -> list[str]:
    """Configuration differences that would confound the architecture claim.

    Compares the shared keys above plus every ``MDKR_*`` project option, so a
    new option is covered the day it is added rather than the day someone
    remembers to list it here. Architecture-selecting keys are deliberately NOT
    compared: they are supposed to differ, and that difference is the point.
    """

    problems: list[str] = []
    keys = set(SHARED_CACHE_KEYS)
    keys.update(k for k in arm_cache if k.startswith("MDKR_"))
    keys.update(k for k in x86_cache if k.startswith("MDKR_"))
    for key in sorted(keys):
        arm = arm_cache.get(key)
        x86 = x86_cache.get(key)
        if arm == x86:
            continue
        problems.append(
            f"{key} differs: arm64={arm!r} x86_64={x86!r}. The two trees must "
            "be configured identically apart from the architecture, or this "
            "gate measures the configuration difference")
    return problems


def newest_source_mtime(root: Path) -> tuple[float, Path | None]:
    """The most recently modified compiled source under ``root``.

    Returns ``(mtime, path)``; ``(0.0, None)`` when nothing matched, which a
    caller treats as "cannot judge staleness" rather than "fresh".
    """

    newest = 0.0
    newest_path: Path | None = None
    for tree in SOURCE_TREES:
        base = root / tree
        if not base.is_dir():
            continue
        for path in base.rglob("*"):
            if path.suffix not in SOURCE_SUFFIXES or not path.is_file():
                continue
            mtime = path.stat().st_mtime
            if mtime > newest:
                newest, newest_path = mtime, path
    return newest, newest_path


def staleness_problems(binaries: dict[str, Path], newest: float,
                       newest_path: Path | None) -> list[str]:
    """Binaries older than the newest source file they are supposed to contain.

    A stale x86_64 executable is the most likely way this gate reports a
    spectacular architecture divergence that is really "you edited the
    simulation and only rebuilt one tree".
    """

    if newest_path is None:
        return []
    problems: list[str] = []
    for label, path in binaries.items():
        if path.stat().st_mtime < newest:
            problems.append(
                f"{label} ({path}) is older than {newest_path}. Rebuild both "
                "trees from the same source before comparing them; a stale "
                "binary makes a source difference look architectural")
    return problems


def parse_simhash_rows(text: str) -> list[SimHashRow]:
    """Every ``[SIMHASH]`` row in ``text``, oldest first.

    Matching the whole line rather than a prefix is deliberate: a truncated
    final line (a killed run, a full disk) must be dropped rather than parsed
    into a short digest that then "differs" from its peer for an I/O reason.
    """

    rows: list[SimHashRow] = []
    for line in text.splitlines():
        match = SIMHASH_RE.match(line)
        if match is None:
            continue
        rows.append(SimHashRow(int(match.group(1)), int(match.group(2)),
                               match.group(3), line.strip()))
    return rows


def stream_problems(rows: list[SimHashRow], expected: int,
                    label: str) -> list[str]:
    """Whether a captured stream is evidence at all.

    An empty stream, a short stream, or one whose ticks are not 0..N-1 cannot
    support an identity claim -- two empty streams are equal, and equality over
    nothing is the failure mode this project has been bitten by before. So the
    contract is checked before the comparison, not instead of it.
    """

    problems: list[str] = []
    if not rows:
        problems.append(
            f"{label}: no [SIMHASH] rows. The instrument did not arm "
            f"(MDKR_STATE_HASH={HASH_VERSION} must reach the process), so "
            "there is nothing to compare and an identity claim would be "
            "vacuous")
        return problems
    if len(rows) != expected:
        problems.append(
            f"{label}: {len(rows)} [SIMHASH] rows, expected {expected}")
    ticks = [row.tick for row in rows]
    if ticks != list(range(len(rows))):
        first_bad = next(i for i, tick in enumerate(ticks) if tick != i)
        problems.append(
            f"{label}: tick numbering is not contiguous from 0; row "
            f"{first_bad} carries tick={ticks[first_bad]}")
    return problems


def first_divergence(arm: list[SimHashRow],
                     x86: list[SimHashRow]) -> int | None:
    """Index of the first tick whose published row differs, or ``None``.

    Compares the published fields rather than the raw text so a future
    formatting change to the row cannot turn into a false divergence.
    """

    for index, (a, b) in enumerate(zip(arm, x86)):
        if (a.tick, a.objs, a.digest) != (b.tick, b.objs, b.digest):
            return index
    if len(arm) != len(x86):
        return min(len(arm), len(x86))
    return None


def divergence_report(route: str, arm: list[SimHashRow],
                      x86: list[SimHashRow], index: int) -> list[str]:
    """The triage a maintainer needs from a red run, without a second run.

    Names the first divergent tick, both rows, whether the POPULATION or only
    the state digest moved, and how the streams behave afterwards. The
    population distinction matters: a differing ``objs=`` means the two
    architectures disagree about which objects exist, which is a spawn/despawn
    or allocation-order defect, while an identical population with a different
    digest is a value defect -- typically a float. They have disjoint suspect
    lists.
    """

    lines = [f"{route}: architecture divergence at tick {index}"]
    if index >= len(arm) or index >= len(x86):
        lines.append(
            f"    the streams have different lengths (arm64 {len(arm)} rows, "
            f"x86_64 {len(x86)} rows); one run ended early")
        return lines
    a, b = arm[index], x86[index]
    lines.append(f"    arm64  {a.raw}")
    lines.append(f"    x86_64 {b.raw}")
    if a.objs != b.objs:
        lines.append(
            f"    the authoritative POPULATION differs ({a.objs} vs {b.objs}): "
            "the two architectures disagree about which objects exist, so look "
            "at spawn/despawn conditions and allocation order before float "
            "arithmetic")
    else:
        lines.append(
            f"    the population agrees ({a.objs} objects) and only the state "
            "digest moved: a field VALUE differs. Float arithmetic, an "
            "uninitialised read, or a pointer-derived value are the candidates")
    tail = list(zip(arm[index:], x86[index:]))
    differing = sum(1 for x, y in tail
                    if (x.tick, x.objs, x.digest) != (y.tick, y.objs, y.digest))
    lines.append(
        f"    {index} ticks were identical; {differing} of the {len(tail)} "
        "remaining ticks differ")
    lines.append(
        f"    bisect with: MDKR_STATE_HASH={HASH_VERSION} "
        f"MDKR_HASH_DUMP_TICK={max(index - 1, 0)} MDKR_HASH_DUMP_UNTIL={index} "
        "MDKR_HASH_DUMP_IDS=1 on both binaries, then diff the [HASHOBJ] and "
        "[HASHOBJID] rows to name the object and field that moved")
    return lines


def control_problem(base: list[SimHashRow],
                    control: list[SimHashRow]) -> str | None:
    """The positive control: a deliberately different simulation must differ.

    ``MDKR_RNGSEED=legacy`` restores the pre-fix boot seeds
    (platform/math_util_native.c), so its stream provably is not the base
    stream. If this comparator reports them identical, then the identical
    result it reported for the cross-architecture arms means nothing: the
    capture, the parse or the comparison is broken.
    """

    if first_divergence(base, control) is None:
        return ("positive control failed: the MDKR_RNGSEED=legacy arm produced "
                "a stream identical to the base arm. The capture or the "
                "comparison is broken, so every 'identical' verdict above is "
                "unsupported")
    return None


# --------------------------------------------------------------------------- #
#  Process-facing helpers
# --------------------------------------------------------------------------- #


def binary_architectures(path: Path) -> tuple[str, ...]:
    """``lipo -archs`` on ``path``; empty when the tool or the file says nothing.

    ``lipo`` ships with the command line tools on every macOS host that can
    build this project, so its absence is reported as an empty result and the
    caller fails closed rather than skipping the assertion.
    """

    lipo = shutil.which("lipo")
    if lipo is None:
        return ()
    proc = subprocess.run([lipo, "-archs", str(path)], text=True,
                          stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                          check=False)
    if proc.returncode != 0:
        return ()
    return parse_lipo_archs(proc.stdout)


def read_cmake_cache(binary: Path) -> dict[str, str]:
    """The CMakeCache.txt beside ``binary``, parsed; empty when absent."""

    cache = binary.parent / "CMakeCache.txt"
    if not cache.is_file():
        return {}
    return parse_cmake_cache(cache.read_text(encoding="utf-8",
                                             errors="replace"))


def run_arm(binary: Path, rom: Path, label: str, root: Path, level: int,
            frames: int, extra_env: dict[str, str], timeout: int,
            verbose: bool, script: Path = SCRIPT) -> list[SimHashRow]:
    """One headless run; returns its [SIMHASH] stream.

    The environment is scrubbed of MDKR*/GE007_* first and rebuilt from
    literals, exactly as check_state_hash.py does, so a maintainer's exported
    renderer, pacing or diagnostic preference cannot reach one arm and not the
    other -- which on this gate would look like an architecture difference.
    LC_ALL and TZ are pinned for the same reason at the libc layer.

    The stream is read from MDKR_STATE_HASH_FILE rather than stdout. The file
    sink is flushed per tick, so a run killed by the timeout still leaves a
    line-aligned prefix that names how far the two arms agreed, and a stdout
    pipe cannot lose or interleave rows behind the engine's other logging.
    """

    run_dir = root / label
    save_dir = run_dir / "save"
    save_dir.mkdir(parents=True)
    stream_path = run_dir / "simhash.txt"
    env = {k: v for k, v in os.environ.items()
           if not k.startswith(("MDKR", "GE007_"))}
    env.update(
        LC_ALL="C",
        LANG="C",
        TZ="UTC",
        MDKR_AUDIO="0",
        MDKR_STATE_HASH=HASH_VERSION,
        MDKR_STATE_HASH_FILE=str(stream_path),
        MDKR_AUTOPILOT="1",
        MDKR_LOAD_TRACK=str(level),
        MDKR_RENDERER="gl",
        MDKR_SAVE_DIR=str(save_dir),
        MDKR_VIDEO_CONFIG_PATH=str(save_dir / "video.ini"),
    )
    env.update(extra_env)
    command = [
        str(binary), "--headless-frames", str(frames),
        "--input-script", str(script), "--rom", str(rom),
        "--window-size", WINDOW,
    ]
    if verbose:
        print(f"$ ({label}) {' '.join(command)}", flush=True)
    try:
        process = subprocess.run(
            command, cwd=run_dir, env=env, text=True,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            timeout=timeout, check=False)
    except subprocess.TimeoutExpired as expired:
        captured = len(parse_simhash_rows(
            stream_path.read_text(encoding="utf-8", errors="replace")
            if stream_path.is_file() else ""))
        raise RuntimeError(
            f"{label}: timed out after {timeout}s with {captured} of {frames} "
            "ticks captured. The x86_64 arm runs under Rosetta 2 and is "
            "materially slower than the native one, so raise --timeout rather "
            "than reading this as a failure") from expired
    if process.returncode != 0:
        raise RuntimeError(
            f"{label}: exit {process.returncode}\n"
            f"{(process.stdout or '')[-3000:]}")
    if not stream_path.is_file():
        raise RuntimeError(
            f"{label}: MDKR_STATE_HASH_FILE was never created. The build "
            "predates the sim_hash.c file sink, or the run never reached a "
            "hashing tick")
    return parse_simhash_rows(
        stream_path.read_text(encoding="utf-8", errors="replace"))


def rosetta_available() -> bool:
    """Whether this host can execute an x86_64 binary at all.

    A missing Rosetta 2 makes every x86_64 arm die with a bare exec failure.
    Naming that up front beats reporting it once per route as a mystery exit
    code.
    """

    if sys.platform != "darwin":
        return True
    arch = shutil.which("arch")
    if arch is None:
        return True
    proc = subprocess.run([arch, "-x86_64", "/usr/bin/true"],
                          stdout=subprocess.DEVNULL,
                          stderr=subprocess.DEVNULL, check=False)
    return proc.returncode == 0


# --------------------------------------------------------------------------- #
#  Entry point
# --------------------------------------------------------------------------- #


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--build", default=DEFAULT_BUILD_DIR,
                        help="the native arm64 build directory or executable")
    parser.add_argument("--build-x86", default="build-x86_64",
                        help="the thin x86_64 build directory or executable")
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    parser.add_argument("--level", action="append", type=int, dest="levels",
                        help="MDKR_LOAD_TRACK level to race (repeatable)")
    parser.add_argument("--frames", type=int, default=DEFAULT_FRAMES)
    parser.add_argument(
        "--script", default=str(SCRIPT),
        help="input script to drive both architectures through. The default "
             "route is check_state_hash.py's anchor; point this at an "
             "Adventure-party route to cover adventure_party_spawn.c's "
             "authoritative sinf/cosf spawn positions, which the default "
             "route never reaches")
    parser.add_argument("--timeout", type=int, default=1800)
    parser.add_argument("--allow-stale", action="store_true",
                        help="skip the binary-newer-than-source guard")
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    arm_binary = Path(os.path.abspath(resolve_binary(args.build)))
    x86_binary = Path(os.path.abspath(resolve_binary(args.build_x86)))
    rom = Path(os.path.abspath(args.rom))
    script = Path(os.path.abspath(args.script))
    levels = args.levels or list(DEFAULT_LEVELS)

    if not arm_binary.exists():
        print(f"check_crossarch_determinism: FAIL\n  - no arm64 build at "
              f"{arm_binary}")
        return 1
    if not x86_binary.exists():
        print(
            "check_crossarch_determinism: FAIL\n"
            f"  - no x86_64 build at {x86_binary}. Produce one with\n"
            "      macos/Scripts/build_crossarch_x86_64.sh\n"
            "    and see docs/architecture/cross-architecture-determinism.md "
            "for the toolchain requirements. This gate fails rather than skips "
            "because a skipped cross-architecture gate reads as a passing one "
            "in the suite summary")
        return 1
    for path in (rom, script):
        if not path.exists():
            print(f"check_crossarch_determinism: FAIL\n  - missing {path}")
            return 1

    problems: list[str] = []
    problems += architecture_problems(binary_architectures(arm_binary),
                                      binary_architectures(x86_binary))
    problems += configuration_problems(read_cmake_cache(arm_binary),
                                       read_cmake_cache(x86_binary))
    if not args.allow_stale:
        newest, newest_path = newest_source_mtime(ROOT)
        problems += staleness_problems(
            {"the arm64 binary": arm_binary, "the x86_64 binary": x86_binary},
            newest, newest_path)
    if not rosetta_available():
        problems.append(
            "this host cannot execute x86_64 binaries; install Rosetta 2 with "
            "`softwareupdate --install-rosetta`")
    if problems:
        print("check_crossarch_determinism: FAIL")
        for problem in problems:
            print(f"  - {problem}")
        return 1

    print(f"check_crossarch_determinism: v{HASH_VERSION} field set, "
          f"{args.frames} ticks, levels {','.join(str(n) for n in levels)}")
    print(f"  arm64  {arm_binary}")
    print(f"  x86_64 {x86_binary}")

    failures: list[str] = []
    compared = 0
    with tempfile.TemporaryDirectory(prefix="mdkr-crossarch-") as tmp:
        root = Path(tmp)
        for level in levels:
            route = f"level {level}"
            try:
                arm = run_arm(arm_binary, rom, f"lvl{level}-arm64", root,
                              level, args.frames, {}, args.timeout,
                              args.verbose, script)
                x86 = run_arm(x86_binary, rom, f"lvl{level}-x86_64", root,
                              level, args.frames, {}, args.timeout,
                              args.verbose, script)
            except RuntimeError as error:
                failures.append(f"{route}: {error}")
                continue
            route_problems = (stream_problems(arm, args.frames,
                                              f"{route} arm64") +
                              stream_problems(x86, args.frames,
                                              f"{route} x86_64"))
            if route_problems:
                failures.extend(route_problems)
                continue
            index = first_divergence(arm, x86)
            if index is not None:
                failures.extend(divergence_report(route, arm, x86, index))
                continue
            compared += len(arm)
            print(f"  {route}: {len(arm)} ticks byte-identical across "
                  "arm64 and x86_64")

        # The control runs last and on one level only: it costs one more pass
        # and it is what stops a broken capture from passing this gate quietly.
        try:
            base = run_arm(arm_binary, rom, "control-base", root, levels[0],
                           args.frames, {}, args.timeout, args.verbose,
                           script)
            legacy = run_arm(arm_binary, rom, "control-legacy", root,
                             levels[0], args.frames,
                             {"MDKR_RNGSEED": "legacy"}, args.timeout,
                             args.verbose, script)
        except RuntimeError as error:
            failures.append(f"positive control: {error}")
        else:
            failures.extend(stream_problems(base, args.frames,
                                            "control base"))
            failures.extend(stream_problems(legacy, args.frames,
                                            "control legacy-RNG"))
            problem = control_problem(base, legacy)
            if problem is not None:
                failures.append(problem)

    if failures:
        print("check_crossarch_determinism: FAIL")
        for failure in failures:
            print(f"  - {failure}" if not failure.startswith("    ")
                  else failure)
        return 1
    print(f"check_crossarch_determinism: PASS — {compared} authoritative ticks "
          f"byte-identical between a thin arm64 and a thin x86_64 build across "
          f"{len(levels)} route(s); the legacy-RNG control diverges, so the "
          "comparison is live")
    return 0


if __name__ == "__main__":
    sys.exit(main())
