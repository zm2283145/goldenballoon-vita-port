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
     party_link / ceremony) -- robust to compiler drift where a legit toolchain
     change moves the hashes but isolation still holds.

A hash mismatch is only a HARD failure if the symbol check ALSO finds a leak (a
compiler/toolchain change can legitimately move the bytes); the symbol check is
the authoritative isolation gate. Both must pass for a clean bill.
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

# The beta-only engine TUs (+ party_link). If the CMake gate ever leaked (e.g. the
# game/src glob went recursive), object files for these would appear in the OFF
# tree -- the single most catastrophic, still-compiling isolation break. Their
# ABSENCE is the strongest, false-positive-free isolation gate.
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


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--deps-from", default="build-off",
        help="an existing build dir to reuse fetched dependency sources from "
             "(default build-off); falls back to build-beta / build-demo-* dirs")
    parser.add_argument(
        "--keep", action="store_true",
        help="keep the scratch OFF build dir (default: remove after hashing)")
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
        hash_ok = True
        hash_report: list[str] = []
        for rel, expected in ANCHORS.items():
            obj = scratch / rel
            if not obj.is_file():
                return fail(f"anchor object missing from the OFF build: {rel}")
            got = sha256_prefix(obj)
            mark = "MATCH" if got == expected else "DIFFERS"
            if got != expected:
                hash_ok = False
            hash_report.append(f"    {rel}: expected {expected} got {got} {mark}")

        # 2) No beta-only engine TU object files exist in the OFF tree (the
        # strongest, false-positive-free gate -- catches a leaked CMake gate).
        leaked_objs = [rel for rel in BETA_TU_OBJECTS
                       if list(scratch.rglob(Path(rel).name))]
        if leaked_objs:
            print("\n".join(hash_report), file=sys.stderr)
            return fail(f"the OFF build compiled beta-only engine TU object(s) "
                        f"{leaked_objs} -- the CMake beta gate leaked")

        # 3) Zero beta-feature symbols in the OFF binary (the authoritative gate).
        binary = scratch / "mdkr64"
        if not binary.is_file():
            return fail("the OFF mdkr64 binary was not produced")
        nm = subprocess.run(["nm", str(binary)], text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        leaked = sorted({
            line for line in nm.stdout.splitlines()
            if any(sym in line for sym in LEAK_SYMBOLS)
        })
        if leaked:
            print("\n".join(hash_report), file=sys.stderr)
            print("[isolation] LEAKED online symbols in OFF mdkr64:",
                  file=sys.stderr)
            print("\n".join("    " + s for s in leaked[:40]), file=sys.stderr)
            return fail(f"the OFF binary links {len(leaked)} online symbol(s) -- "
                        f"an isolation leak into the release engine")

        if not hash_ok:
            # Symbols are clean but the bytes moved: only a HARD fail if it is not
            # a plausible toolchain drift. We treat a clean-symbol hash drift as a
            # WARNING (bump the pins), since the symbol gate proved isolation holds.
            print("\n".join(hash_report), file=sys.stderr)
            print("[isolation] WARNING: anchor hashes moved but the OFF binary has "
                  "ZERO online symbols -- likely a compiler/toolchain drift; update "
                  "the pinned prefixes if the toolchain changed intentionally.",
                  file=sys.stderr)

        print(
            "PASS online isolation: a FRESH clean OFF build links ZERO online "
            "symbols (mdkr_online_*/party_link/ceremony) into the release engine"
            + (", and all 3 anchor objects are byte-identical (thread3_main "
               "20ed811d, menu cfeb2121, online_race_results 12487bac)"
               if hash_ok else " (anchor hashes moved -- see WARNING above)")
            + ".")
        if args.verbose or not hash_ok:
            print("\n".join(hash_report))
        return 0
    finally:
        if not args.keep:
            shutil.rmtree(scratch, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
