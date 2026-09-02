#!/usr/bin/env python3
"""ROM-free contract for run_checks' SDL flavor witness.

The 1.6.0 qualification drifted between Homebrew sdl2-compat and the pinned
upstream SDL2 the DMG bundles, and one lane was green only under the shim. The
suite now names the linked flavor on every run and can refuse the shim; this
pins the classifier on real loader output shapes so the witness cannot lie.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import run_checks  # noqa: E402

OTOOL_SHIM = """build/mdkr64:
\t/opt/homebrew/opt/sdl2-compat/lib/libSDL2-2.0.0.dylib (compatibility version 3201.0.0, current version 3201.70.0)
\t/usr/lib/libSystem.B.dylib (compatibility version 1.0.0, current version 1351.0.0)
"""
OTOOL_UPSTREAM = """build/mdkr64:
\t@rpath/libSDL2-2.0.0.dylib (compatibility version 3201.0.0, current version 3201.10.0)
\t/usr/lib/libSystem.B.dylib (compatibility version 1.0.0, current version 1351.0.0)
"""
LDD_UPSTREAM = """\tlinux-vdso.so.1 (0x00007ffd)
\tlibSDL2-2.0.so.0 => /usr/lib/x86_64-linux-gnu/libSDL2-2.0.so.0 (0x00007f)
"""
LDD_SHIM = """\tlibSDL2-2.0.so.0 => /opt/sdl2-compat/lib/libSDL2-2.0.so.0 (0x00007f)
"""
NO_SDL = """build/mdkr_unit_test:
\t/usr/lib/libSystem.B.dylib (compatibility version 1.0.0, current version 1351.0.0)
"""


def main() -> int:
    failures = []

    def expect(text: str, flavor: str, label: str) -> None:
        got, _ = run_checks.classify_sdl_link(text)
        if got != flavor:
            failures.append(f"{label}: expected {flavor}, classified {got}")

    expect(OTOOL_SHIM, run_checks.SDL_FLAVOR_SHIM, "macOS sdl2-compat")
    expect(OTOOL_UPSTREAM, run_checks.SDL_FLAVOR_UPSTREAM, "macOS upstream")
    expect(LDD_UPSTREAM, run_checks.SDL_FLAVOR_UPSTREAM, "Linux upstream")
    expect(LDD_SHIM, run_checks.SDL_FLAVOR_SHIM, "Linux shim path")
    expect(NO_SDL, run_checks.SDL_FLAVOR_STATIC, "no SDL linked")
    _, detail = run_checks.classify_sdl_link(OTOOL_UPSTREAM)
    if "3201.10.0" not in detail:
        failures.append("upstream detail must carry the linked version")

    # Positive control for the refusal: the shim must raise only when required.
    class _Fake:
        pass
    saved = run_checks.sdl_flavor_for_binary
    run_checks.sdl_flavor_for_binary = lambda _p: (run_checks.SDL_FLAVOR_SHIM, "x")
    try:
        try:
            run_checks.report_sdl_flavor(Path("mdkr64"), require_shipping=False)
        except RuntimeError:
            failures.append("shim must be a note, not a failure, without --require-shipping-sdl")
        try:
            run_checks.report_sdl_flavor(Path("mdkr64"), require_shipping=True)
        except RuntimeError:
            pass
        else:
            failures.append("--require-shipping-sdl must refuse the shim")
    finally:
        run_checks.sdl_flavor_for_binary = saved

    if failures:
        print("run_checks sdl flavor contract: FAIL")
        for failure in failures:
            print("  - " + failure)
        return 1
    print("run_checks sdl flavor contract: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
