#!/usr/bin/env python3
"""ISOLATION GUARD -- prove the online beta feature is byte-for-byte absent from a
fresh OFF (release) build, so the battle-tested offline engine cannot regress.

The native online takeover deliberately inlines beta forks into two crown-jewel
engine TUs (game/src/thread3_main.c, game/src/menu.c) and shares one TU compiled
into every build (platform/net/online_race_results.c). Isolation therefore rests
entirely on hand-maintained `#if MDKR_ENABLE_ONLINE_BETA` discipline -- and, until
this guard, had ZERO automated enforcement (isolation review I-1). A future edit
that STILL COMPILES but leaks into OFF would pass every other gate and ship an
offline regression. This guard closes that hole:

  1. Fresh-configures + builds a scratch OFF (MDKR_ENABLE_ONLINE_BETA=OFF) tree,
     reusing an existing build's already-fetched dependency sources so it needs no
     network and stays fast (~25-30s).
  2. Asserts the 3 pinned anchor object sha256 prefixes are byte-identical:
        game/src/thread3_main.c.o          20ed811d
        game/src/menu.c.o                  cfeb2121
        platform/net/online_race_results.c.o  12487bac
  3. Asserts the OFF `mdkr64` binary links ZERO online symbols (mdkr_online_* /
     party_link / ceremony) -- a second, name-based gate that also catches a leak
     whose codegen happens to leave the anchor bytes untouched.

ALL THREE gates are HARD (a failure of any one exits non-zero). The anchor bytes
are the GROUND TRUTH for isolation, so a hash mismatch is a HARD FAILURE by
default -- the symbol allowlist is a fixed set of name prefixes and cannot catch
a leak that inlines with no external symbol, uses an un-listed name, or perturbs
an offline TU's codegen without referencing anything online; only the anchor
hashes see that. The guard already pins the compiler (/usr/bin/cc) and Release,
so on the toolchain the pins were minted on the bytes must not move.

For a DELIBERATE toolchain bump (a legitimate compiler upgrade that moves the
bytes while isolation still holds), pass --allow-hash-drift: the mismatch is then
downgraded to a WARNING (the run still PASSES) and the new prefixes are printed so
the pins can be re-minted. Without that flag, a moved anchor hash fails the guard.
"""

from __future__ import annotations

import argparse
import hashlib
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# The pinned anchors: object path (relative to the build dir) -> sha256 prefix.
ANCHORS = {
    "CMakeFiles/mdkr64.dir/game/src/thread3_main.c.o": "20ed811d",
    "CMakeFiles/mdkr64.dir/game/src/menu.c.o": "cfeb2121",
    "CMakeFiles/mdkr64.dir/platform/net/online_race_results.c.o": "12487bac",
}

# BETA-FEATURE symbols that must be ABSENT from an OFF binary. Deliberately NOT a
# blanket "mdkr_online_" -- the launcher infra (lobby dispatch/view-model, the fake
# adapter, compatibility identity, mdkr_online_race_results_publish/poll) is
# pre-existing UNCONDITIONAL code that legitimately links into OFF; only the beta
# NATIVE-TAKEOVER surface below is the leak signal (matches the isolation review's
# nm check). "mdkr_online_results_" uniquely names the beta RESULTS screen and does
# NOT match the pre-existing "mdkr_online_race_results_*".
LEAK_SYMBOLS = (
    "party_link",
    "mdkr_online_session",
    "mdkr_online_charselect",
    "mdkr_online_trackselect",
    "mdkr_online_ceremony",
    "mdkr_online_results_",
    "mdkr_online_standings",
    "mdkr_online_race_boot",
    "mdkr_online_live_adapter",
    "beganWithoutDescriptor",
)

# The engine target's object directory, relative to a build dir. The OFF-leak
# object scan is SCOPED here so it is robust regardless of how the OFF tree was
# built: a beta-only TU that leaked into the release engine lands under this dir,
# whereas the SAME source compiled by a legitimate UNIT-TEST target lands under
# that test target's own CMakeFiles/<target>.dir and is NOT a leak. Concretely,
# platform/net/party_link.c carries no #if-beta guard and is compiled -- beta
# macro OFF -- into mdkr_party_link_test, so a fully-built build-off contains
# CMakeFiles/mdkr_party_link_test.dir/platform/net/party_link.c.o; an unscoped
# whole-tree scan would flag that legit test object and false-positive.
ENGINE_OBJ_DIR = "CMakeFiles/mdkr64.dir"

# The beta-only engine TUs (+ party_link). If the CMake gate ever leaked (e.g. the
# game/src glob went recursive), object files for these would appear in the ENGINE
# object dir -- the single most catastrophic, still-compiling isolation break.
# Their ABSENCE from the engine dir is the strongest, false-positive-free gate.
BETA_TU_OBJECTS = (
    "game/src/online/online_session.c.o",
    "game/src/online/online_charselect.c.o",
    "game/src/online/online_trackselect.c.o",
    "game/src/online/online_race_boot.c.o",
    "game/src/online/online_results.c.o",
    "game/src/online/online_ceremony.c.o",
    "platform/net/party_link.c.o",
)

# Cache vars pulled from the reference build so the scratch configure matches the
# real OFF build (and reuses its fetched dependency sources -> no download).
FETCH_VARS = (
    "FETCHCONTENT_SOURCE_DIR_MDKR_LIBDATACHANNEL",
    "FETCHCONTENT_SOURCE_DIR_MDKR_MBEDTLS",
    "FETCHCONTENT_SOURCE_DIR_WGPU_NATIVE",
)


def fail(message: str) -> int:
    print(f"FAIL online isolation: {message}", file=sys.stderr)
    return 1


def read_cache_var(cache: Path, name: str) -> str | None:
    if not cache.is_file():
        return None
    for line in cache.read_text().splitlines():
        head, _, value = line.partition("=")
        key = head.split(":", 1)[0]
        if key == name:
            return value
    return None


def sha256_prefix(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()[:8]


def present_beta_tu_basenames(build_dir: Path) -> set[str]:
    """The BETA_TU_OBJECTS basenames that exist under `build_dir`'s ENGINE object
    dir (CMakeFiles/mdkr64.dir), and ONLY there -- never under a unit-test
    target's object dir. A beta-only TU that leaks into the release engine lands
    at CMakeFiles/mdkr64.dir/<rel>, so this exact-path scan catches a real leak
    while ignoring the same source legitimately compiled into a test target.
    Feed the result to leaked_tu_objects()."""
    engine = build_dir / ENGINE_OBJ_DIR
    return {Path(rel).name for rel in BETA_TU_OBJECTS
            if (engine / rel).is_file()}


# --------------------------------------------------------------------------- #
#  Pure detection predicates (the three HARD gates + the anti-vacuity guard).
#
#  These are the guard's ENTIRE decision logic, factored out of main() so they
#  can be driven directly by tests/check_online_isolation_selftest.py against
#  deliberately-broken inputs -- proving each gate actually TRIPS on a leak (the
#  guard is non-vacuous), not just that it PASSES on a clean OFF build. main()
#  calls exactly these, so the self-test exercises the real detection code.
# --------------------------------------------------------------------------- #

def anchor_mismatches(got_by_rel: dict[str, str],
                      expected: dict[str, str] | None = None) -> list[str]:
    """Anchor rels whose sha256 prefix does NOT match the pin (empty == all
    byte-identical). This is the gate-1 (byte-identity) decision."""
    exp = ANCHORS if expected is None else expected
    return [rel for rel, got in got_by_rel.items() if got != exp.get(rel)]


def leaked_tu_objects(present_basenames) -> list[str]:
    """BETA_TU_OBJECTS whose object basename is present in the OFF tree (empty ==
    no beta-only engine TU leaked). This is the gate-2 (leaked CMake gate)
    decision. Preserves BETA_TU_OBJECTS order for a stable report."""
    present = set(present_basenames)
    return [rel for rel in BETA_TU_OBJECTS if Path(rel).name in present]


def leaked_symbols(nm_stdout: str) -> list[str]:
    """The sorted nm lines that name a beta LEAK_SYMBOL (empty == no online
    symbol linked into OFF). This is the gate-3 (symbol) decision."""
    return sorted({
        line for line in nm_stdout.splitlines()
        if any(sym in line for sym in LEAK_SYMBOLS)
    })


def nm_vacuity_error(returncode: int, nm_stdout: str) -> str | None:
    """The M4 anti-vacuity guard: a non-zero nm OR empty output means the symbol
    gate cannot run, so a clean pass would be VACUOUS. Returns a reason string
    when the gate must NOT report a pass, else None."""
    if returncode != 0:
        return f"nm failed (exit {returncode})"
    if not nm_stdout.strip():
        return "nm produced NO symbols"
    return None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--deps-from", default="build-off",
        help="an existing build dir to reuse fetched dependency sources from "
             "(default build-off); falls back to build-beta / build-demo-* dirs")
    parser.add_argument(
        "--keep", action="store_true",
        help="keep the scratch OFF build dir (default: remove after hashing)")
    parser.add_argument(
        "--allow-hash-drift", action="store_true",
        help="downgrade an anchor byte-identity MISMATCH from a HARD failure to a "
             "WARNING (still exits 0). Use ONLY for a deliberate toolchain bump: it "
             "prints the new prefixes so the pins can be re-minted. The symbol + "
             "TU-object gates stay HARD regardless.")
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    # Resolve the dependency source dirs from a reference build's cache.
    fetch_args: list[str] = []
    candidates = [args.deps_from, "build-off", "build-beta"]
    candidates += sorted(str(p.name) for p in ROOT.glob("build-demo-*"))
    cache = None
    for cand in candidates:
        c = (ROOT / cand / "CMakeCache.txt")
        if c.is_file() and read_cache_var(c, FETCH_VARS[0]):
            cache = c
            break
    if cache is not None:
        for var in FETCH_VARS:
            val = read_cache_var(cache, var)
            if val:
                fetch_args.append(f"-D{var}={val}")
        if args.verbose:
            print(f"[isolation] reusing fetched deps from {cache.parent}",
                  flush=True)
    else:
        print("[isolation] WARNING: no reference build cache found; the fresh "
              "configure may download dependencies", file=sys.stderr)

    scratch = Path(tempfile.mkdtemp(prefix="mdkr64-off-isolation-"))
    try:
        configure = [
            "cmake", "-S", str(ROOT), "-B", str(scratch), "-G", "Ninja",
            "-DCMAKE_BUILD_TYPE=Release",
            "-DCMAKE_C_COMPILER=/usr/bin/cc",
            "-DCMAKE_CXX_COMPILER=/usr/bin/c++",
            "-DMDKR_ENABLE_ONLINE_BETA=OFF",
            "-DMDKR_APP=ON",
            "-DMDKR_NATIVE_PHONE_PARTY=ON",
            "-DMDKR_WEBGPU_BACKEND=ON",
            "-DMDKR_VERSION=1.6.0",
        ] + fetch_args
        cfg = subprocess.run(configure, cwd=ROOT, text=True,
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        if cfg.returncode != 0:
            print(cfg.stdout[-8000:], file=sys.stderr)
            return fail("the fresh OFF configure failed")

        build = subprocess.run(
            ["cmake", "--build", str(scratch), "--target", "mdkr64", "-j6"],
            cwd=ROOT, text=True, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT)
        if build.returncode != 0:
            print(build.stdout[-8000:], file=sys.stderr)
            return fail("the fresh OFF build failed")

        # 1) Anchor byte-identity.
        got_by_rel: dict[str, str] = {}
        for rel in ANCHORS:
            obj = scratch / rel
            if not obj.is_file():
                return fail(f"anchor object missing from the OFF build: {rel}")
            got_by_rel[rel] = sha256_prefix(obj)
        mismatched = anchor_mismatches(got_by_rel)
        hash_ok = not mismatched
        hash_report = [
            f"    {rel}: expected {ANCHORS[rel]} got {got_by_rel[rel]} "
            f"{'MATCH' if got_by_rel[rel] == ANCHORS[rel] else 'DIFFERS'}"
            for rel in ANCHORS
        ]

        # 2) No beta-only engine TU object files exist in the ENGINE object dir
        # (the strongest, false-positive-free gate -- catches a leaked CMake
        # gate). Scoped to CMakeFiles/mdkr64.dir so a legit unit-test target that
        # compiles a listed source (e.g. party_link.c into mdkr_party_link_test)
        # is never mistaken for an engine leak.
        leaked_objs = leaked_tu_objects(present_beta_tu_basenames(scratch))
        if leaked_objs:
            print("\n".join(hash_report), file=sys.stderr)
            return fail(f"the OFF build compiled beta-only engine TU object(s) "
                        f"{leaked_objs} -- the CMake beta gate leaked")

        # 3) Zero beta-feature symbols in the OFF binary (the authoritative gate).
        binary = scratch / "mdkr64"
        if not binary.is_file():
            return fail("the OFF mdkr64 binary was not produced")
        nm = subprocess.run(["nm", str(binary)], text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        # M4: a silent nm failure yields empty stdout -> zero matches -> a VACUOUS
        # symbol PASS. Treat a non-zero nm (stripped/fat binary, toolchain quirk) or
        # empty output on a real binary as a HARD error, never a clean pass.
        vacuity = nm_vacuity_error(nm.returncode, nm.stdout)
        if vacuity is not None:
            print("\n".join(hash_report), file=sys.stderr)
            if nm.returncode != 0:
                print(f"[isolation] nm stderr: {(nm.stderr or '').strip()[:2000]}",
                      file=sys.stderr)
                return fail(f"nm failed (exit {nm.returncode}) on the OFF binary -- "
                            f"the symbol gate cannot run, so isolation is UNPROVEN (a "
                            f"silent empty-output PASS would be vacuous)")
            return fail("nm produced NO symbols for the OFF binary -- the symbol "
                        "gate would be vacuous; refusing to report a clean pass")
        leaked = leaked_symbols(nm.stdout)
        if leaked:
            print("\n".join(hash_report), file=sys.stderr)
            print("[isolation] LEAKED online symbols in OFF mdkr64:",
                  file=sys.stderr)
            print("\n".join("    " + s for s in leaked[:40]), file=sys.stderr)
            return fail(f"the OFF binary links {len(leaked)} online symbol(s) -- "
                        f"an isolation leak into the release engine")

        if not hash_ok:
            # I-1: the anchor bytes are the GROUND TRUTH for isolation. A hash
            # mismatch is a HARD failure by default -- the symbol allowlist is a
            # fixed set of name prefixes and cannot see a leak that inlines with no
            # external symbol, uses an un-listed name, or only perturbs an offline
            # TU's codegen. Only a DELIBERATE toolchain bump (--allow-hash-drift)
            # downgrades it to a WARNING so a legit compiler upgrade is not a wall.
            print("\n".join(hash_report), file=sys.stderr)
            if not args.allow_hash_drift:
                return fail(
                    "anchor object bytes MOVED on the pinned toolchain (/usr/bin/cc "
                    "+ Release). The anchor bytes are the isolation ground truth, so "
                    "this is a HARD failure: an offline TU's codegen changed. If this "
                    "is a DELIBERATE toolchain bump (not a leak), re-run with "
                    "--allow-hash-drift and re-mint the pinned prefixes above.")
            print("[isolation] WARNING: anchor hashes moved but --allow-hash-drift "
                  "was given -- treating as a deliberate toolchain bump. Re-mint the "
                  "pinned prefixes above (ANCHORS in this file) so the guard hard-"
                  "enforces the NEW toolchain's bytes.", file=sys.stderr)

        print(
            "PASS online isolation: a FRESH clean OFF build links ZERO online "
            "symbols (mdkr_online_*/party_link/ceremony) into the release engine"
            + (", and all 3 anchor objects are byte-identical (thread3_main "
               "20ed811d, menu cfeb2121, online_race_results 12487bac)"
               if hash_ok else " (anchor hashes DRIFTED -- see WARNING above; "
               "--allow-hash-drift)")
            + ".")
        if args.verbose or not hash_ok:
            print("\n".join(hash_report))
        return 0
    finally:
        if not args.keep:
            shutil.rmtree(scratch, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
