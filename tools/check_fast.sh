#!/usr/bin/env bash
# Tier 1: every gate that needs no ROM, no engine, no GPU and no display.
#
# About nine minutes of the complete suite's 747, and the tier that caught the
# only real finding in 29 hours of running (check_rollback_authority refusing an
# unclassified mutable file-scope declaration, in five seconds). These gates
# catch a CLASS of mistake rather than an instance: a declaration nobody
# classified, a public surface that grew, a workflow that drifted from its
# contract, a ROM-derived byte in the tree.
#
# Run it on every commit. It is not a qualification and does not pretend to be:
# run_checks labels any restricted run SUBSET n/N, and only
# "complete suite, N/N tasks" counts. See docs/TEST_SUITE_ECONOMICS.md section 9.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

BUILD_DIR="${MDKR_FAST_BUILD_DIR:-build-rel}"
PYTHON="${MDKR_PYTHON:-python3}"

printf '== tier 1: source gates ==\n'
# The runner refuses to execute without the dedicated-desktop attestation. These
# gates read the tree rather than driving hardware, so the attestation is about
# the runner's policy, not about this tier's cost.
MDKR_DEDICATED_TEST_DESKTOP=1 "$PYTHON" tools/run_checks.py --role source

if [ -d "$BUILD_DIR" ]; then
    printf '\n== tier 1: compiled unit tests ==\n'
    # ROM-free CTests. -E excludes the GPU-labelled ones: they need a real
    # surface, which is exactly what this tier promises not to require.
    ctest --test-dir "$BUILD_DIR" --output-on-failure -LE gpu
else
    printf '\n(skipping ctest: %s does not exist; configure it first)\n' "$BUILD_DIR"
fi

printf '\ntier 1 complete. This is NOT a qualification -- see section 9 of\n'
printf 'docs/TEST_SUITE_ECONOMICS.md for what tier 2 and tier 3 owe you.\n'
