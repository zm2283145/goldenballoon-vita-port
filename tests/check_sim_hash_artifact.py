#!/usr/bin/env python3
"""On-disk per-tick state-hash artifact mode and the first-divergence comparator.

The per-tick authoritative-state hash (`[SIMHASH]` rows, platform/sim_hash.c)
is the anchor instrument for every fidelity gate. Comparing two hosts' streams
by scraping stdout logs is brittle. This check covers the two pieces that make
the stream a durable, byte-comparable artifact:

1. **Artifact mode parity.** With `MDKR_STATE_HASH_FILE=<path>` set alongside
   `MDKR_STATE_HASH`, the engine mirrors every `[SIMHASH]` line it prints to
   stdout into the given file, byte-for-byte. The file must equal exactly the
   `[SIMHASH]` lines the same run logged: it is a second sink for the same
   values, so if it ever disagreed with stdout the artifact would not be
   evidence of anything. This is the "no behaviour change" proof — the file
   carries the same hashes, in the same order, formatted identically.

2. **Comparator.** `tools/online/compare_sim_hash_artifacts.py` takes two
   artifact files and exits 0 iff both are non-empty, the same length, and
   byte-equal modulo line endings on every tick line; otherwise it exits
   non-zero and names the first divergent tick (or the truncation point). It
   fails closed with a distinct exit per failure mode: a usage error, a missing
   file, an empty file, an unparseable or non-ASCII line, and a length mismatch.
   This arm proves each of those paths, using synthetic artifact files so it
   needs no engine run.

The sink's write-FAILURE path (drop on a mid-run write error, not just a bad
open) is not exercised by an automated arm here -- a portable in-process ENOSPC
is awkward to force -- but it is verified manually and reproducibly by ignoring
SIGXFSZ so a tiny file-size cap makes write(2) return EFBIG to stdio instead of
killing the process:

    trap '' XFSZ; ulimit -f 4       # ignore the signal (survives exec)
    MDKR_STATE_HASH=3 MDKR_STATE_HASH_FILE=art.txt \\
        <mdkr64> --headless-frames N --input-script ... --rom ...

The engine then emits exactly one "cannot write MDKR_STATE_HASH_FILE" stderr
line, drops the sink and never reopens it, leaves the stdout [SIMHASH] stream
intact, and art.txt holds the byte-exact prefix plus the single torn fragment
of the failing tick -- which this comparator rejects fail-closed (exit 4).

Always muted + headless per tests/README.md. Exit 0 = pass.
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
import tempfile
from pathlib import Path

from harness_utils import DEFAULT_BUILD_DIR, find_fatal, resolve_binary

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "tests" / "input_scripts" / "nav_to_time_trial_race.txt"
COMPARATOR = ROOT / "tools" / "online" / "compare_sim_hash_artifacts.py"
# Enough ticks to exercise a live, moving object list; a short run keeps the
# single engine pass cheap while still proving the two sinks agree over
# thousands of hashed fields per tick.
FRAMES = 900

# The comparator's promised exit codes (source of truth:
# tools/online/compare_sim_hash_artifacts.py). Mirrored here so the unit arm can
# assert an EXACT code -- proving each failure mode is distinct, e.g. that a
# usage error does not collide with the missing-file code.
COMPARATOR_EXIT_USAGE = 1
COMPARATOR_EXIT_UNPARSEABLE = 4


def make_artifact(path: Path, hashes: list[str], objs: int = 4) -> None:
    """Write a synthetic artifact in the exact `[SIMHASH]` line format.

    Tick numbers are the line index so a divergence's reported tick is
    unambiguous; each hash is a caller-supplied 16 hex digits.
    """
    with path.open("w") as handle:
        for tick, digest in enumerate(hashes):
            handle.write(f"[SIMHASH] tick={tick} objs={objs} h={digest}\n")


def run_comparator(a: Path, b: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, str(COMPARATOR), str(a), str(b)],
        text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        check=False,
    )


def check_comparator(tmp: Path) -> list[str]:
    """Comparator unit coverage over synthetic artifacts. Returns failures."""
    failures: list[str] = []
    if not COMPARATOR.exists():
        return [f"comparator missing: {COMPARATOR}"]

    good = ["{:016x}".format(0x1000 + i) for i in range(6)]

    # 1. Equal, non-empty files pass.
    a = tmp / "equal_a.txt"
    b = tmp / "equal_b.txt"
    make_artifact(a, good)
    make_artifact(b, good)
    result = run_comparator(a, b)
    if result.returncode != 0:
        failures.append(
            f"equal files must pass, got rc={result.returncode}: "
            f"{result.stdout.strip()}")

    # 2. A single divergent hash is detected and its tick is named.
    diverged = list(good)
    diverged[3] = "ffffffffffffffff"
    c = tmp / "diverge.txt"
    make_artifact(c, diverged)
    result = run_comparator(a, c)
    if result.returncode == 0:
        failures.append("divergent hash must fail, but comparator passed")
    elif "tick=3" not in result.stdout and "tick 3" not in result.stdout:
        failures.append(
            "divergent hash must name the first divergent tick (3); got: "
            f"{result.stdout.strip()}")

    # 3. With MORE THAN ONE divergent tick, the FIRST one is reported -- the
    #    comparator must stop at the earliest disagreement, so a certification
    #    points at where the streams began to differ, not a later symptom.
    two_diverged = list(good)
    two_diverged[2] = "aaaaaaaaaaaaaaaa"
    two_diverged[4] = "bbbbbbbbbbbbbbbb"
    d = tmp / "two_diverge.txt"
    make_artifact(d, two_diverged)
    result = run_comparator(a, d)
    if result.returncode == 0:
        failures.append(
            "two divergent ticks must fail, but comparator passed")
    elif "tick=2" not in result.stdout and "tick 2" not in result.stdout:
        failures.append(
            "with two divergent ticks the FIRST (2) must be reported; got: "
            f"{result.stdout.strip()}")
    elif "tick=4" in result.stdout or "tick 4" in result.stdout:
        failures.append(
            "comparator reported a later divergent tick (4) instead of the "
            f"first (2); got: {result.stdout.strip()}")

    # 4. A truncated file (fewer ticks) is detected as a length mismatch.
    short = tmp / "truncated.txt"
    make_artifact(short, good[:4])
    result = run_comparator(a, short)
    if result.returncode == 0:
        failures.append("truncated file must fail, but comparator passed")
    elif "length" not in result.stdout.lower() and (
            "truncat" not in result.stdout.lower()):
        failures.append(
            f"truncated file must report a length/truncation failure; got: "
            f"{result.stdout.strip()}")

    # 5. An empty file fails closed.
    empty = tmp / "empty.txt"
    empty.write_text("")
    result = run_comparator(a, empty)
    if result.returncode == 0:
        failures.append("empty file must fail, but comparator passed")
    elif "empty" not in result.stdout.lower():
        failures.append(
            f"empty file must report an empty-file failure; got: "
            f"{result.stdout.strip()}")

    # 6. A missing file fails closed.
    missing = tmp / "does_not_exist.txt"
    result = run_comparator(a, missing)
    if result.returncode == 0:
        failures.append("missing file must fail, but comparator passed")
    elif "missing" not in result.stdout.lower() and (
            "no such" not in result.stdout.lower()
            and "not found" not in result.stdout.lower()):
        failures.append(
            f"missing file must report a missing-file failure; got: "
            f"{result.stdout.strip()}")

    # 7. A wholly malformed line fails closed rather than being silently
    #    skipped.
    garbage = tmp / "garbage.txt"
    garbage.write_text("[SIMHASH] this is not a hash line\n")
    result = run_comparator(a, garbage)
    if result.returncode == 0:
        failures.append("unparseable line must fail, but comparator passed")
    elif "parse" not in result.stdout.lower() and (
            "unparseable" not in result.stdout.lower()):
        failures.append(
            f"unparseable line must report a parse failure; got: "
            f"{result.stdout.strip()}")

    # 8. The exact shape a SIGKILL leaves: several complete, valid lines
    #    followed by a final line torn off mid-hash (the per-tick fflush
    #    guarantees whole prefix lines, but the process can die between two
    #    flushes while a line is half written). A torn tail must fail closed as
    #    unparseable, never be read as a shorter-but-valid artifact -- otherwise
    #    a truncated hash could silently pass a cross-host comparison.
    killed = tmp / "killed_midline.txt"
    with killed.open("w") as handle:
        for tick, digest in enumerate(good[:3]):
            handle.write(f"[SIMHASH] tick={tick} objs=4 h={digest}\n")
        handle.write("[SIMHASH] tick=3 objs=4 h=00000000")  # torn mid-hash
    result = run_comparator(a, killed)
    if result.returncode == 0:
        failures.append(
            "mid-hash truncated final line must fail, but comparator passed")
    elif "parse" not in result.stdout.lower() and (
            "unparseable" not in result.stdout.lower()):
        failures.append(
            "mid-hash truncated final line must report a parse failure; got: "
            f"{result.stdout.strip()}")

    # 9. A non-ASCII byte fails closed as unparseable (exit 4), never a raw
    #    UnicodeDecodeError traceback and never locale-dependent: the format is
    #    pure ASCII, so strict decoding gives the same verdict on macOS and
    #    Windows. Live-probe the exact exit code.
    nonascii = tmp / "nonascii.txt"
    nonascii.write_bytes(b"[SIMHASH] tick=0 objs=4 h=00000000\xff0000000\n")
    result = run_comparator(a, nonascii)
    if result.returncode != COMPARATOR_EXIT_UNPARSEABLE:
        failures.append(
            "non-ASCII byte must fail closed as unparseable (exit "
            f"{COMPARATOR_EXIT_UNPARSEABLE}); got rc={result.returncode}: "
            f"{result.stdout.strip()}")
    elif "Traceback" in result.stdout:
        failures.append(
            "non-ASCII byte must not produce a raw traceback; got: "
            f"{result.stdout.strip()}")

    # 10. A usage error (too few arguments) exits with the dedicated usage code
    #     (1), NOT argparse's default 2 which would collide with the missing-
    #     file code. Live-probe the exact exit code via a one-argument call.
    result = subprocess.run(
        [sys.executable, str(COMPARATOR), str(a)],
        text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        check=False)
    if result.returncode != COMPARATOR_EXIT_USAGE:
        failures.append(
            "a usage error must exit with the usage code "
            f"({COMPARATOR_EXIT_USAGE}), not argparse's default 2; got "
            f"rc={result.returncode}: {result.stdout.strip()}")

    return failures


def check_parity(binary: Path, rom: Path, tmp: Path, timeout: int,
                 verbose: bool) -> list[str]:
    """Artifact-mode parity: the file equals the stdout `[SIMHASH]` stream."""
    run_dir = tmp / "parity"
    save_dir = run_dir / "save"
    save_dir.mkdir(parents=True)
    artifact = run_dir / "sim_hash.artifact"

    env = {k: v for k, v in os.environ.items()
           if not k.startswith(("MDKR", "GE007_"))}
    env.update(
        LC_ALL="C",
        MDKR_AUDIO="0",
        MDKR_STATE_HASH="3",
        MDKR_STATE_HASH_FILE=str(artifact),
        MDKR_AUTOPILOT="1",
        MDKR_LOAD_TRACK="5",
        MDKR_RENDERER="gl",
        MDKR_SAVE_DIR=str(save_dir),
        MDKR_VIDEO_CONFIG_PATH=str(save_dir / "video.ini"),
    )
    command = [
        str(binary), "--headless-frames", str(FRAMES),
        "--input-script", str(SCRIPT), "--rom", str(rom),
        "--window-size", "320x240",
    ]
    if verbose:
        print(f"$ (parity) {' '.join(command)}", flush=True)
    process = subprocess.run(
        command, cwd=run_dir, env=env, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        timeout=timeout, check=False,
    )
    if process.returncode != 0:
        return [f"engine exit {process.returncode}: "
                f"{(process.stderr or '')[-800:]}"]
    fatal = find_fatal((process.stdout or "") + (process.stderr or ""))
    if fatal:
        return [f"engine emitted a fatal marker: {fatal}"]

    stdout_rows = [line for line in (process.stdout or "").splitlines()
                   if line.startswith("[SIMHASH]")]
    if len(stdout_rows) != FRAMES:
        return [f"expected {FRAMES} stdout [SIMHASH] rows, got "
                f"{len(stdout_rows)}"]

    if not artifact.exists():
        return [f"artifact file was not written: {artifact} "
                "(MDKR_STATE_HASH_FILE had no effect)"]
    file_rows = artifact.read_text(encoding="ascii").splitlines()

    failures: list[str] = []
    if not file_rows:
        failures.append("artifact file is empty")
    if len(file_rows) != len(stdout_rows):
        failures.append(
            f"artifact has {len(file_rows)} lines, stdout logged "
            f"{len(stdout_rows)} [SIMHASH] rows")
    for index, (file_line, stdout_line) in enumerate(
            zip(file_rows, stdout_rows)):
        if file_line != stdout_line:
            failures.append(
                f"artifact diverges from stdout at line {index}:\n"
                f"    file:   {file_line}\n"
                f"    stdout: {stdout_line}")
            break

    # The comparator must call this run's own file identical to itself: a
    # second sanity gate that the artifact is well-formed for the tool that
    # will consume it cross-host.
    if not failures and COMPARATOR.exists():
        result = run_comparator(artifact, artifact)
        if result.returncode != 0:
            failures.append(
                "comparator rejected the artifact against itself: "
                f"{result.stdout.strip()}")
    return failures


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--build", default=DEFAULT_BUILD_DIR)
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    parser.add_argument("--timeout", type=int, default=300)
    parser.add_argument("--comparator-only", action="store_true",
                        help="run only the ROM-free comparator unit arm")
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    failures: list[str] = []
    with tempfile.TemporaryDirectory(prefix="mdkr-sim-hash-artifact-") as raw:
        tmp = Path(raw)
        for problem in check_comparator(tmp):
            failures.append(f"comparator: {problem}")

        if not args.comparator_only:
            binary = Path(os.path.abspath(resolve_binary(args.build)))
            rom = Path(os.path.abspath(args.rom))
            missing = [str(p) for p in (binary, rom, SCRIPT)
                       if not os.path.exists(p)]
            if missing:
                failures.append("parity: missing inputs: " + ", ".join(missing))
            else:
                try:
                    for problem in check_parity(binary, rom, tmp,
                                                args.timeout, args.verbose):
                        failures.append(f"parity: {problem}")
                except subprocess.TimeoutExpired:
                    failures.append(
                        f"parity: engine did not finish within {args.timeout}s")

    if failures:
        print("check_sim_hash_artifact: FAIL")
        for failure in failures:
            print(f"  - {failure}")
        return 1
    parity_note = (
        "comparator arm only (parity skipped)" if args.comparator_only
        else f"artifact mode mirrors all {FRAMES} stdout [SIMHASH] rows "
             "byte-for-byte")
    print(
        "check_sim_hash_artifact: PASS — comparator detects divergence, "
        "truncation, empty, missing and unparseable inputs and accepts equal "
        f"streams; {parity_note}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
