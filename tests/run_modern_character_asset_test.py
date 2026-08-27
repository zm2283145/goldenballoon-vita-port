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
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "tests"))

import character_asset_compiler as compiler  # noqa: E402
import character_asset_probe as probe  # noqa: E402
import character_package_manager as manager  # noqa: E402
from test_character_asset_probe import (  # noqa: E402
    make_humanoid_glb, make_portrait_png, make_v4_manifest,
)
from character_validation_fixture import accepted_validation  # noqa: E402


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--loader", required=True, type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="mdkr-modern-character-") as directory:
        portrait_bytes = make_portrait_png()
        manifest_data = make_v4_manifest(portrait_bytes, humanoid=True)
        manifest_data["identity"].update({
            "short_name": "Proof",
            "narration_name": "Pipeline Proof character",
            "sort_label": "Proof, Pipeline",
        })
        manifest_data["animations"]["states"]["race.item"] = "idle"
        model_bytes = make_humanoid_glb()
        cache = Path(directory) / "generated.mdkc"
        compiled, _ = compiler.compile_character(
            model_bytes, manifest_data, bytes(range(32)), portrait_bytes
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
        legacy_portable = Path(directory) / "legacy-portable.mdkrchar"
        legacy_v5_portable = Path(directory) / "legacy-v5-portable.mdkrchar"
        install_directory = Path(directory) / "native-install"
        model.write_bytes(model_bytes)
        manifest.write_text(json.dumps(manifest_data), encoding="utf-8")
        license_file.write_text("CC0-1.0 generated fixture\n", encoding="utf-8")
        portrait.write_bytes(portrait_bytes)
        probe.build_package(
            model, manifest, license_file, source_package,
            portrait_path=portrait,
        )
        with mock.patch.object(
                manager, "_validate_character_glb",
                side_effect=accepted_validation):
            manager.prepare(source_package, portable_package)
        with zipfile.ZipFile(portable_package) as source:
            members = {
                name: source.read(name)
                for name in probe.PORTABLE_PACKAGE_MEMBERS_V3
            }
        broken_members = dict(members)
        broken_cache = bytearray(broken_members["compiled.mdkc"])
        broken_cache[-1] ^= 0x80
        broken_members["compiled.mdkc"] = bytes(broken_cache)
        with zipfile.ZipFile(corrupt_portable, "w", allowZip64=False) as archive:
            for name in probe.PORTABLE_PACKAGE_MEMBERS_V3:
                info, payload = probe._zip_entry(name, broken_members[name])
                archive.writestr(info, payload)
        mismatched_members = dict(members)
        mismatched_members["LICENSE.txt"] += b"source changed after compilation\n"
        with zipfile.ZipFile(mismatched_portable, "w", allowZip64=False) as archive:
            for name in probe.PORTABLE_PACKAGE_MEMBERS_V3:
                info, payload = probe._zip_entry(name, mismatched_members[name])
                archive.writestr(info, payload)
        def cache_for_legacy_compiler(
                version: int, *, include_provenance: bool) -> bytes:
            legacy_sections = []
            current_cache = members["compiled.mdkc"]
            section_count = struct.unpack_from("<I", current_cache, 56)[0]
            for index in range(section_count):
                kind, _, offset, size, count, stride = struct.unpack_from(
                    "<IIQQII", current_cache,
                    64 + index * compiler.MDKC_SECTION_ENTRY_BYTES,
                )
                if kind == compiler.SECTION_IDENTITY_NAMES or (
                        kind == compiler.SECTION_PROVENANCE and
                        not include_provenance):
                    continue
                section_data = bytearray(current_cache[offset:offset + size])
                if kind == compiler.SECTION_IDENTITY:
                    # Compiler v5 and earlier reserved the final identity word.
                    struct.pack_into("<I", section_data, 20, 0)
                legacy_sections.append(compiler.Section(
                    kind, count, stride, bytes(section_data)
                ))
            digest = compiler.source_digest(
                ((name, members[name])
                 for name in probe.package_members_for_schema(
                     manifest_data["schema"])),
                compiler_id=f"mdkr-character-compiler/{version}",
            )
            return compiler._assemble(legacy_sections, digest)

        legacy_members = dict(members)
        legacy_members["compiled.mdkc"] = cache_for_legacy_compiler(
            4, include_provenance=False
        )
        with zipfile.ZipFile(legacy_portable, "w", allowZip64=False) as archive:
            for name in probe.PORTABLE_PACKAGE_MEMBERS_V3:
                info, payload = probe._zip_entry(name, legacy_members[name])
                archive.writestr(info, payload)

        v5_members = dict(members)
        v5_members["compiled.mdkc"] = cache_for_legacy_compiler(
            5, include_provenance=True
        )
        with zipfile.ZipFile(
                legacy_v5_portable, "w", allowZip64=False) as archive:
            for name in probe.PORTABLE_PACKAGE_MEMBERS_V3:
                info, payload = probe._zip_entry(name, v5_members[name])
                archive.writestr(info, payload)
        completed = subprocess.run(
            [str(args.loader), str(cache), directory, str(source_package),
             str(portable_package), str(install_directory),
             str(corrupt_portable), str(mismatched_portable),
             str(legacy_portable), str(legacy_v5_portable)],
            check=False, text=True
        )
        return completed.returncode


if __name__ == "__main__":
    raise SystemExit(main())
