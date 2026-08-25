#!/usr/bin/env python3
"""Generate a license-clean MDKC fixture and exercise the native loader."""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "tests"))

import character_asset_compiler as compiler  # noqa: E402
import character_asset_probe as probe  # noqa: E402
import character_package_manager as manager  # noqa: E402
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
        model = Path(directory) / "model.glb"
        manifest = Path(directory) / "manifest.json"
        license_file = Path(directory) / "LICENSE.txt"
        source_package = Path(directory) / "source.mdkrchar"
        portable_package = Path(directory) / "portable.mdkrchar"
        corrupt_portable = Path(directory) / "corrupt-portable.mdkrchar"
        mismatched_portable = Path(directory) / "mismatched-portable.mdkrchar"
        install_directory = Path(directory) / "native-install"
        model.write_bytes(make_animated_glb())
        manifest.write_text(json.dumps(make_manifest()), encoding="utf-8")
        license_file.write_text("CC0-1.0 generated fixture\n", encoding="utf-8")
        probe.build_package(model, manifest, license_file, source_package)
        manager.prepare(source_package, portable_package)
        with zipfile.ZipFile(portable_package) as source:
            members = {name: source.read(name) for name in probe.PORTABLE_PACKAGE_MEMBERS}
        broken_cache = bytearray(members["compiled.mdkc"])
        broken_cache[-1] ^= 0x80
        members["compiled.mdkc"] = bytes(broken_cache)
        with zipfile.ZipFile(corrupt_portable, "w", allowZip64=False) as archive:
            for name in probe.PORTABLE_PACKAGE_MEMBERS:
                info, payload = probe._zip_entry(name, members[name])
                archive.writestr(info, payload)
        mismatched_members = dict(members)
        mismatched_members["LICENSE.txt"] += b"source changed after compilation\n"
        with zipfile.ZipFile(mismatched_portable, "w", allowZip64=False) as archive:
            for name in probe.PORTABLE_PACKAGE_MEMBERS:
                info, payload = probe._zip_entry(name, mismatched_members[name])
                archive.writestr(info, payload)
        completed = subprocess.run(
            [str(args.loader), str(cache), directory, str(source_package),
             str(portable_package), str(install_directory),
             str(corrupt_portable), str(mismatched_portable)],
            check=False, text=True
        )
        return completed.returncode


if __name__ == "__main__":
    raise SystemExit(main())
