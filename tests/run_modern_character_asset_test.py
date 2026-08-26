#!/usr/bin/env python3
"""Generate a license-clean MDKC fixture and exercise the native loader."""

from __future__ import annotations

import argparse
import json
import struct
import subprocess
import sys
import tempfile
import zipfile
import zlib
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "tests"))

import character_asset_compiler as compiler  # noqa: E402
import character_asset_probe as probe  # noqa: E402
import character_package_manager as manager  # noqa: E402
from test_character_asset_probe import (  # noqa: E402
    make_animated_glb, make_manifest, make_portrait_png,
)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--loader", required=True, type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="mdkr-modern-character-") as directory:
        portrait_bytes = make_portrait_png()
        manifest_data = make_manifest()
        manifest_data["schema"] = probe.PACKAGE_SCHEMA_V4
        manifest_data["identity"] = {
            "portrait_file": "portrait.png",
            "portrait_sha256": probe._sha256(portrait_bytes),
            "minimap_rgb": [220, 72, 144],
        }
        manifest_data["rig"] = {
            "mode": "authored-clips-only",
            "reviewed": False,
            "roles": {
                "hips": {
                    "node": "root", "inferred": True, "confidence": 0.9,
                },
                "head": {
                    "node": "head", "inferred": False, "confidence": 1.0,
                },
            },
        }
        cache = Path(directory) / "generated.mdkc"
        compiled, _ = compiler.compile_character(
            make_animated_glb(), manifest_data, bytes(range(32)), portrait_bytes
        )
        cache.write_bytes(compiled)
        (Path(directory) / "duplicate.mdkc").write_bytes(compiled)
        (Path(directory) / "bad.mdkc").write_bytes(compiled[:-7])
        hostile_identity = bytearray(compiled)
        section_count = struct.unpack_from("<I", hostile_identity, 56)[0]
        for index in range(section_count):
            kind, _, offset, _, _, _ = struct.unpack_from(
                "<IIQQII", hostile_identity,
                64 + index * compiler.MDKC_SECTION_ENTRY_BYTES,
            )
            if kind == compiler.SECTION_IDENTITY_DATA:
                idat = hostile_identity.find(b"IDAT", offset)
                hostile_identity[idat + 4] ^= 0xFF
                break
        struct.pack_into(
            "<I", hostile_identity, 52,
            zlib.crc32(hostile_identity[compiler.MDKC_HEADER_BYTES:]) & 0xFFFFFFFF,
        )
        (Path(directory) / "bad-portrait.mdkc").write_bytes(hostile_identity)
        model = Path(directory) / "model.glb"
        manifest = Path(directory) / "manifest.json"
        license_file = Path(directory) / "LICENSE.txt"
        portrait = Path(directory) / "portrait.png"
        source_package = Path(directory) / "source.mdkrchar"
        portable_package = Path(directory) / "portable.mdkrchar"
        corrupt_portable = Path(directory) / "corrupt-portable.mdkrchar"
        mismatched_portable = Path(directory) / "mismatched-portable.mdkrchar"
        install_directory = Path(directory) / "native-install"
        model.write_bytes(make_animated_glb())
        manifest.write_text(json.dumps(manifest_data), encoding="utf-8")
        license_file.write_text("CC0-1.0 generated fixture\n", encoding="utf-8")
        portrait.write_bytes(portrait_bytes)
        probe.build_package(
            model, manifest, license_file, source_package,
            portrait_path=portrait,
        )
        manager.prepare(source_package, portable_package)
        with zipfile.ZipFile(portable_package) as source:
            members = {
                name: source.read(name)
                for name in probe.PORTABLE_PACKAGE_MEMBERS_V3
            }
        broken_cache = bytearray(members["compiled.mdkc"])
        broken_cache[-1] ^= 0x80
        members["compiled.mdkc"] = bytes(broken_cache)
        with zipfile.ZipFile(corrupt_portable, "w", allowZip64=False) as archive:
            for name in probe.PORTABLE_PACKAGE_MEMBERS_V3:
                info, payload = probe._zip_entry(name, members[name])
                archive.writestr(info, payload)
        mismatched_members = dict(members)
        mismatched_members["LICENSE.txt"] += b"source changed after compilation\n"
        with zipfile.ZipFile(mismatched_portable, "w", allowZip64=False) as archive:
            for name in probe.PORTABLE_PACKAGE_MEMBERS_V3:
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
