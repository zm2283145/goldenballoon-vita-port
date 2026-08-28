#!/usr/bin/env python3
"""META-GUARD: prove tools/check_online_isolation.py is NON-VACUOUS.

The isolation guard is the ENTIRE automated basis for the "the battle-tested
offline engine is provably unaffected" promise: it hard-fails if a fresh OFF
build drifts an anchor object byte, compiles a beta-only engine TU, or links a
beta online symbol. Every other online lane trusts it. But a guard that PASSES
on a clean OFF build proves nothing unless it also TRIPS on a broken one -- a
guard whose predicates silently never fire (an inverted comparison, an empty
allowlist, a swallowed nm error) would report GREEN forever while isolation
rotted. Nothing exercised that failure mode until this lane.

This self-test drives the guard's OWN detection predicates (extracted from its
main() so this exercises the REAL decision code, not a copy):

  gate 1  anchor byte-identity   -> check_online_isolation.anchor_mismatches
  gate 2  no beta-only TU object -> check_online_isolation.leaked_tu_objects
  gate 3  no beta online symbol  -> check_online_isolation.leaked_symbols
  M4      anti-vacuity nm guard  -> check_online_isolation.nm_vacuity_error

For EACH gate it asserts BOTH directions:
  * CLEAN inputs are ACCEPTED -- and, when a real build-off/ tree is present, the
    ACTUAL OFF artifacts (anchor object hashes, the mdkr64 nm symbol table, the
    absence of every beta TU object) are fed through the real predicates and must
    read clean, so the positive path is proven on the true release build, not a
    mock.
  * DELIBERATELY-BROKEN inputs are DETECTED -- a flipped anchor pin, a present
    beta TU object, an nm line naming each beta LEAK_SYMBOL, and a failed/empty
    nm -- each must trip its predicate. This is the non-vacuity proof: if any
    predicate were inverted or dead, the matching negative assertion here fails.

TEST-ONLY: touches no product TU, builds nothing, boots no engine. Fast and
deterministic (no wall-clock, no ROM), so it is safe to run first in the serial
gate. --build/--rom are accepted (and ignored) for run_online_checks.py parity.
"""

from __future__ import annotations

import argparse
import hashlib
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tests"))
sys.path.insert(0, str(ROOT / "tools"))

from online_lane_util import make_fail  # noqa: E402
import check_online_isolation as guard  # noqa: E402

fail = make_fail("isolation-selftest")


def _check_anchor_gate(notes: list[str]) -> int | None:
    """gate 1: anchor_mismatches accepts the pins, flags a flipped pin, and (if a
    real build-off tree exists) reads the true OFF anchor objects as byte-identical."""
    pinned = {rel: guard.ANCHORS[rel] for rel in guard.ANCHORS}
    if guard.anchor_mismatches(pinned):
        return fail("anchor gate rejected its OWN pinned prefixes -- the byte-"
                    "identity comparison is broken (would false-positive forever)")
    # Negative: flip ONE pin; the gate MUST name exactly that anchor.
    first = next(iter(guard.ANCHORS))
    broken = dict(pinned)
    broken[first] = "deadbeef"
    hit = guard.anchor_mismatches(broken)
    if hit != [first]:
        return fail(f"anchor gate did NOT flag a drifted anchor: flipping {first} "
                    f"to 'deadbeef' yielded mismatches={hit} (want [{first!r}]) -- "
                    f"the gate is VACUOUS (a real byte-drift would ship silently)")
    # Positive on the REAL artifact, when available.
    off = ROOT / "build-off"
    got = {}
    for rel in guard.ANCHORS:
        obj = off / rel
        if not obj.is_file():
            notes.append(f"anchor gate: build-off/{rel} absent (real-artifact "
                         f"positive skipped; synthetic clean+broken proven)")
            return None
        got[rel] = hashlib.sha256(obj.read_bytes()).hexdigest()[:8]
    real_mism = guard.anchor_mismatches(got)
    if real_mism:
        return fail(f"the REAL build-off anchor objects drifted from the pins: "
                    f"{real_mism} (got {got}) -- either isolation regressed or a "
                    f"pin is stale")
    notes.append("anchor gate: real build-off anchors byte-identical to the pins")
    return None


def _check_tu_gate(notes: list[str]) -> int | None:
    """gate 2: leaked_tu_objects flags a present beta TU object, ignores a benign
    present object, and reads the real build-off tree as beta-TU-free."""
    # Negative: every beta TU basename must be flagged when present.
    for rel in guard.BETA_TU_OBJECTS:
        base = Path(rel).name
        hit = guard.leaked_tu_objects({base})
        if rel not in hit:
            return fail(f"TU gate did NOT flag a leaked beta TU object {base!r} "
                        f"(got {hit}) -- a leaked CMake gate would ship silently")
    # Negative control: an anchor object (menu.c.o) is a LEGITIMATE OFF object and
    # must NOT be flagged (the gate keys on beta-only TUs, not any online-adjacent
    # name), so the gate is specific, not a blanket match.
    benign = guard.leaked_tu_objects({"menu.c.o", "main_app.cpp.o", "thread3_main.c.o"})
    if benign:
        return fail(f"TU gate false-positived on legitimate OFF objects {benign} "
                    f"-- it would block every clean release build")
    # Positive on the REAL tree, when available.
    off = ROOT / "build-off"
    if off.is_dir():
        present = {Path(rel).name for rel in guard.BETA_TU_OBJECTS
                   if list(off.rglob(Path(rel).name))}
        real = guard.leaked_tu_objects(present)
        if real:
            return fail(f"the REAL build-off tree contains beta-only engine TU "
                        f"object(s) {real} -- the CMake beta gate leaked")
        notes.append("TU gate: real build-off contains none of the beta engine TUs")
    else:
        notes.append("TU gate: build-off absent (real-artifact positive skipped)")
    return None


def _check_symbol_gate(notes: list[str]) -> int | None:
    """gate 3: leaked_symbols flags a line naming EACH beta LEAK_SYMBOL, ignores a
    benign symbol table, and reads the real build-off/mdkr64 nm table as clean."""
    if not guard.LEAK_SYMBOLS:
        return fail("the LEAK_SYMBOLS allowlist is EMPTY -- the symbol gate can "
                    "never fire (vacuous by construction)")
    # Negative: a synthetic nm line per beta symbol must be caught.
    for sym in guard.LEAK_SYMBOLS:
        line = f"0000000000000001 T _{sym}_probe"
        if not guard.leaked_symbols(line + "\n0000000000000002 T _main"):
            return fail(f"symbol gate did NOT flag an nm line naming beta symbol "
                        f"{sym!r} -- a real leak of it into OFF would ship silently")
    # Negative control: a benign table (no beta symbols) must read clean.
    benign = ("0000000000000001 T _main\n"
              "0000000000000002 T _SDL_CreateWindow\n"
              "0000000000000003 T _mdkr_online_race_results_publish\n"
              "0000000000000004 T _mdkr_net_roster_runtime_install")
    if guard.leaked_symbols(benign):
        return fail("symbol gate false-positived on a benign OFF symbol table "
                    "(incl. the pre-existing unconditional mdkr_online_race_results_"
                    "* / roster infra that legitimately links into OFF) -- it would "
                    "block every clean release build")
    # M4 anti-vacuity guard: failed/empty nm must be rejected; a real table ok.
    if guard.nm_vacuity_error(0, benign) is not None:
        return fail("nm_vacuity_error rejected a healthy nm table (exit 0, symbols "
                    "present) -- the symbol gate could never run")
    if guard.nm_vacuity_error(1, "irrelevant") is None:
        return fail("nm_vacuity_error accepted a FAILED nm (exit 1) -- a silent nm "
                    "failure would yield a VACUOUS symbol PASS (the M4 hole)")
    for empty in ("", "   ", "\n\n"):
        if guard.nm_vacuity_error(0, empty) is None:
            return fail(f"nm_vacuity_error accepted EMPTY nm output {empty!r} -- a "
                        f"zero-symbol scan would vacuously PASS (the M4 hole)")
    # Positive on the REAL artifact, when available.
    off_bin = ROOT / "build-off" / "mdkr64"
    if off_bin.is_file():
        import subprocess
        nm = subprocess.run(["nm", str(off_bin)], text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        vac = guard.nm_vacuity_error(nm.returncode, nm.stdout)
        if vac is not None:
            return fail(f"nm on the REAL build-off/mdkr64 is unusable ({vac}) -- "
                        f"cannot prove the real symbol-gate positive")
        real = guard.leaked_symbols(nm.stdout)
        if real:
            return fail(f"the REAL build-off/mdkr64 links {len(real)} beta online "
                        f"symbol(s): {real[:8]} -- an isolation leak into the "
                        f"release engine")
        notes.append("symbol gate: real build-off/mdkr64 nm table links zero beta "
                     "online symbols")
    else:
        notes.append("symbol gate: build-off/mdkr64 absent (real-artifact positive "
                     "skipped)")
    return None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    # Accepted for run_online_checks.py parity; this lane needs neither.
    parser.add_argument("--build", default="build-off")
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    notes: list[str] = []
    for gate in (_check_anchor_gate, _check_tu_gate, _check_symbol_gate):
        result = gate(notes)
        if result is not None:
            return result

    if args.verbose:
        for note in notes:
            print(f"  [selftest] {note}")

    print(
        "PASS online isolation-selftest: the isolation guard is NON-VACUOUS -- all "
        "three HARD gates plus the M4 anti-vacuity nm guard were driven through "
        "their REAL predicates and each both ACCEPTED clean inputs (incl. the true "
        "build-off anchor objects, nm symbol table, and beta-TU-free tree where "
        "present) and DETECTED deliberately-broken inputs: a flipped anchor pin, a "
        "present beta engine TU object, an nm line naming every beta LEAK_SYMBOL, "
        "and a failed/empty nm. A silently-dead or inverted gate would fail this "
        "lane; the release-engine offline-isolation gate is proven to actually fire."
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
