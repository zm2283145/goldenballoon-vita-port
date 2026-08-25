#!/usr/bin/env python3
"""Generate a license-clean MDKC fixture and exercise the native loader."""

from __future__ import annotations

import argparse
import subprocess
import sys
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "tests"))

import character_asset_compiler as compiler  # noqa: E402
from test_character_asset_probe import make_animated_glb, make_manifest  # noqa: E402


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--loader", required=True, type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="mdkr-modern-character-") as directory:
        cache = Path(directory) / "generated.mdkc"
        compiled, _ = compiler.compile_character(
            make_animated_glb(), make_manifest(), bytes(range(32))
        )
        cache.write_bytes(compiled)
        (Path(directory) / "duplicate.mdkc").write_bytes(compiled)
        (Path(directory) / "bad.mdkc").write_bytes(compiled[:-7])
        completed = subprocess.run(
            [str(args.loader), str(cache), directory], check=False, text=True
        )
        return completed.returncode


if __name__ == "__main__":
    raise SystemExit(main())
