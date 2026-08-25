#!/usr/bin/env python3
"""ROM-free contract tests for the custom-character pipeline spike."""

from __future__ import annotations

import importlib.util
import io
import json
import struct
import sys
import tempfile
import unittest
import zipfile
import zlib
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "character_asset_probe", ROOT / "tools" / "character_asset_probe.py"
)
assert SPEC is not None and SPEC.loader is not None
probe = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(probe)
sys.path.insert(0, str(ROOT / "tools"))
import character_asset_compiler as compiler  # noqa: E402


def _align(data: bytearray, alignment: int = 4) -> None:
    data.extend(b"\0" * ((-len(data)) % alignment))


def _compiled_sections(compiled: bytes) -> dict[int, dict[str, int]]:
    section_count = struct.unpack_from("<I", compiled, 56)[0]
    sections: dict[int, dict[str, int]] = {}
    for index in range(section_count):
        kind, flags, offset, size, count, stride = struct.unpack_from(
            "<IIQQII", compiled, 64 + index * compiler.MDKC_SECTION_ENTRY_BYTES
        )
        sections[kind] = {
            "flags": flags,
            "offset": offset,
            "size": size,
            "count": count,
            "stride": stride,
        }
    return sections


def make_animated_glb(external_buffer: bool = False, with_lod: bool = False) -> bytes:
    binary = bytearray()
    views: list[dict[str, int]] = []
    accessors: list[dict[str, object]] = []

    def add(values: bytes, component: int, kind: str, count: int, *, minimum=None, maximum=None):
        _align(binary)
        offset = len(binary)
        binary.extend(values)
        view = len(views)
        views.append({"buffer": 0, "byteOffset": offset, "byteLength": len(values)})
        accessor: dict[str, object] = {
            "bufferView": view,
            "componentType": component,
            "count": count,
            "type": kind,
        }
        if minimum is not None:
            accessor["min"] = minimum
        if maximum is not None:
            accessor["max"] = maximum
        accessors.append(accessor)
        return len(accessors) - 1

    positions = add(
        struct.pack("<9f", -0.5, 0.0, 0.0, 0.5, 0.0, 0.0, 0.0, 1.0, 0.0),
        5126,
        "VEC3",
        3,
        minimum=[-0.5, 0.0, 0.0],
        maximum=[0.5, 1.0, 0.0],
    )
    normals = add(struct.pack("<9f", *(0.0, 0.0, 1.0) * 3), 5126, "VEC3", 3)
    uvs = add(struct.pack("<6f", 0.0, 0.0, 1.0, 0.0, 0.5, 1.0), 5126, "VEC2", 3)
    joints = add(bytes((0, 1, 0, 0) * 3), 5121, "VEC4", 3)
    weights = add(
        struct.pack("<12f", *(0.5, 0.5, 0.0, 0.0) * 3), 5126, "VEC4", 3
    )
    indices = add(struct.pack("<3H", 0, 1, 2), 5123, "SCALAR", 3)
    identity = (1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0,
                0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0)
    inverse_bind = add(struct.pack("<32f", *(identity + identity)), 5126, "MAT4", 2)
    times = add(struct.pack("<2f", 0.0, 1.0), 5126, "SCALAR", 2, minimum=[0.0], maximum=[1.0])
    rotations = add(
        struct.pack("<8f", 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.3826834, 0.9238795),
        5126,
        "VEC4",
        2,
    )
    _align(binary)
    png_offset = len(binary)
    png_signature = b"\x89PNG\r\n\x1a\n"

    def png_chunk(kind: bytes, payload: bytes) -> bytes:
        return (
            struct.pack(">I", len(payload))
            + kind
            + payload
            + struct.pack(">I", zlib.crc32(kind + payload) & 0xFFFFFFFF)
        )

    png = (
        png_signature
        + png_chunk(b"IHDR", struct.pack(">IIBBBBB", 1, 1, 8, 6, 0, 0, 0))
        + png_chunk(b"IDAT", zlib.compress(b"\x00\xff\x80\x40\xff"))
        + png_chunk(b"IEND", b"")
    )
    binary.extend(png)
    png_view = len(views)
    views.append({"buffer": 0, "byteOffset": png_offset, "byteLength": len(png)})
    _align(binary)
    document = {
        "asset": {"version": "2.0", "generator": "mdkr-test-fixture"},
        "scene": 0,
        "scenes": [{"nodes": [0, 2]}],
        "nodes": [
            {"name": "root", "children": [1]},
            {"name": "head"},
            {"name": "character", "mesh": 0, "skin": 0},
        ],
        "meshes": [{"name": "LOD0", "primitives": [{
            "attributes": {
                "POSITION": positions,
                "NORMAL": normals,
                "TEXCOORD_0": uvs,
                "JOINTS_0": joints,
                "WEIGHTS_0": weights,
            },
            "indices": indices,
            "material": 0,
        }]}],
        "materials": [{"name": "body", "pbrMetallicRoughness": {
            "baseColorFactor": [0.7, 0.2, 0.1, 1.0],
            "baseColorTexture": {"index": 0},
            "metallicFactor": 0.0,
            "roughnessFactor": 0.8,
        }}],
        "images": [{"name": "body", "mimeType": "image/png", "bufferView": png_view}],
        "textures": [{"name": "body", "source": 0}],
        "skins": [{"name": "rig", "joints": [0, 1], "skeleton": 0,
                   "inverseBindMatrices": inverse_bind}],
        "animations": [{
            "name": "idle",
            "samplers": [{"input": times, "output": rotations, "interpolation": "LINEAR"}],
            "channels": [{"sampler": 0, "target": {"node": 1, "path": "rotation"}}],
        }],
        "bufferViews": views,
        "accessors": accessors,
        "buffers": [{"byteLength": len(binary)}],
    }
    if with_lod:
        document["extensionsUsed"] = ["MSFT_lod"]
        document["nodes"][2]["extensions"] = {"MSFT_lod": {"ids": [3]}}
        document["nodes"].append(
            {"name": "character_lod1", "mesh": 1, "skin": 0}
        )
        document["meshes"].append(document["meshes"][0].copy())
    if external_buffer:
        document["buffers"][0]["uri"] = "mesh.bin"
    json_chunk = json.dumps(document, separators=(",", ":")).encode("utf-8")
    json_chunk += b" " * ((-len(json_chunk)) % 4)
    output = bytearray(struct.pack("<4sII", b"glTF", 2, 0))
    output += struct.pack("<II", len(json_chunk), probe.GLB_JSON_CHUNK) + json_chunk
    output += struct.pack("<II", len(binary), probe.GLB_BIN_CHUNK) + binary
    struct.pack_into("<I", output, 8, len(output))
    return bytes(output)


def make_manifest() -> dict[str, object]:
    return {
        "schema": probe.PACKAGE_SCHEMA,
        "id": "org.example.pipeline-proof",
        "display_name": "Pipeline Proof",
        "renderer_profile": "modern-skeletal-v1",
        "license": {
            "spdx": "CC0-1.0",
            "attribution": "Generated MDKR test fixture",
            "source_url": "https://example.invalid/pipeline-proof",
        },
        "animations": {"fallback": "idle", "states": {"idle": "idle"}},
        "gameplay": {
            "donor": "diddy",
            "vehicles": ["car", "hovercraft", "plane"],
        },
        "presentation": {
            "scale": [1.0, 1.0, 1.0],
            "translation_m": [0.0, 0.0, 0.0],
            "rotation_xyzw": [0.0, 0.0, 0.0, 1.0],
            "lod_bias": 0.0,
        },
        "sockets": {"seat": "root", "head": "head"},
    }


def rewrite_glb_document(data: bytes, update) -> bytes:
    document, binary = probe.parse_glb(data)
    update(document)
    json_chunk = json.dumps(document, separators=(",", ":")).encode("utf-8")
    json_chunk += b" " * ((-len(json_chunk)) % 4)
    payload = binary or b""
    output = bytearray(struct.pack("<4sII", b"glTF", 2, 0))
    output += struct.pack("<II", len(json_chunk), probe.GLB_JSON_CHUNK) + json_chunk
    output += struct.pack("<II", len(payload), probe.GLB_BIN_CHUNK) + payload
    struct.pack_into("<I", output, 8, len(output))
    return bytes(output)


class CharacterAssetProbeTests(unittest.TestCase):
    def test_generated_glb_is_character_ready(self) -> None:
        report = probe.inspect_glb_bytes(make_animated_glb(), require_character=True)
        self.assertEqual([], report["errors"])
        self.assertTrue(report["character_ready"])
        self.assertEqual(1, report["triangle_count"])
        self.assertEqual(2, report["max_joints"])
        self.assertEqual(1.0, report["animations"][0]["duration_seconds"])

    def test_external_resource_is_rejected(self) -> None:
        report = probe.inspect_glb_bytes(make_animated_glb(external_buffer=True), require_character=True)
        self.assertTrue(any("external" in error for error in report["errors"]))

    def test_msft_lod_chain_compiles_to_distinct_primitive_levels(self) -> None:
        compiled, report = compiler.compile_character(
            make_animated_glb(with_lod=True), make_manifest(), bytes(range(32))
        )
        self.assertEqual(2, report["lod_levels"])
        sections = _compiled_sections(compiled)
        primitive = sections[compiler.SECTION_PRIMITIVES]
        stride = struct.calcsize(compiler.PRIMITIVE_FORMAT)
        first = struct.unpack_from(compiler.PRIMITIVE_FORMAT, compiled, primitive["offset"])
        second = struct.unpack_from(
            compiler.PRIMITIVE_FORMAT, compiled, primitive["offset"] + stride
        )
        self.assertEqual(0, first[7])
        self.assertEqual(1, second[7])

    def test_package_is_deterministic_and_verifies(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            model = root / "model.glb"
            model.write_bytes(make_animated_glb())
            manifest = root / "manifest.json"
            manifest.write_text(json.dumps(make_manifest()), encoding="utf-8")
            license_file = root / "LICENSE.txt"
            license_file.write_text("CC0-1.0 test fixture\n", encoding="utf-8")
            first = root / "first.mdkrchar"
            second = root / "second.mdkrchar"
            probe.build_package(model, manifest, license_file, first)
            probe.build_package(model, manifest, license_file, second)
            self.assertEqual(first.read_bytes(), second.read_bytes())
            verified = probe.verify_package(first)
            self.assertTrue(verified["valid"], verified["errors"])

    def test_compiled_cache_is_deterministic_and_sectioned(self) -> None:
        model = make_animated_glb()
        manifest = make_manifest()
        source_digest = bytes(range(32))
        first, first_report = compiler.compile_character(model, manifest, source_digest)
        second, second_report = compiler.compile_character(model, manifest, source_digest)
        self.assertEqual(first, second)
        self.assertEqual(first_report, second_report)
        magic, version, header_bytes, file_bytes = struct.unpack_from("<4sIIQ", first, 0)
        self.assertEqual(compiler.MDKC_MAGIC, magic)
        self.assertEqual(compiler.MDKC_VERSION, version)
        self.assertEqual(compiler.MDKC_HEADER_BYTES, header_bytes)
        self.assertEqual(len(first), file_bytes)
        self.assertEqual(3, first_report["vertices"])
        self.assertEqual(1, first_report["triangles"])
        self.assertEqual(2, first_report["joints"])
        self.assertEqual(1, first_report["animations"])
        self.assertEqual(2, first_report["sockets"])
        self.assertEqual(4, first_report["decoded_texture_bytes"])

    def test_compiler_rejects_png_dimensions_before_decode(self) -> None:
        header = (b"\x89PNG\r\n\x1a\n" + struct.pack(
            ">I4sII", 13, b"IHDR", compiler.MAX_TEXTURE_DIMENSION + 1, 1
        ))
        with self.assertRaisesRegex(compiler.CompileError, "dimensions"):
            compiler._png_dimensions(header, 0)

    def test_compiler_rejects_nonuniform_joint_bind_scale(self) -> None:
        model = rewrite_glb_document(
            make_animated_glb(),
            lambda document: document["nodes"][0].update({"scale": [1.0, 2.0, 1.0]}),
        )
        with self.assertRaisesRegex(compiler.CompileError, "non-uniform bind scale"):
            compiler.compile_character(model, make_manifest(), bytes(32))

    def test_compiler_rejects_missing_socket_node(self) -> None:
        manifest = make_manifest()
        manifest["sockets"] = {"seat": "missing", "head": "head"}
        with self.assertRaises(compiler.CompileError):
            compiler.compile_character(make_animated_glb(), manifest, bytes(32))

    def test_manifest_rejects_unknown_fields_at_every_fixed_level(self) -> None:
        manifest = make_manifest()
        manifest["surprise"] = True
        manifest["gameplay"]["speed_multiplier"] = 2.0
        errors = probe.validate_manifest(
            manifest, probe.inspect_glb_bytes(make_animated_glb(), require_character=True)
        )
        self.assertTrue(any("unknown field 'surprise'" in error for error in errors))
        self.assertTrue(any("unknown field 'speed_multiplier'" in error for error in errors))

    def test_json_rejects_duplicate_keys_and_nonfinite_numbers(self) -> None:
        with self.assertRaisesRegex(probe.ProbeError, "duplicate key 'id'"):
            probe.json_loads_strict('{"id":"first","id":"second"}', "manifest")
        with self.assertRaisesRegex(probe.ProbeError, "non-finite number"):
            probe.json_loads_strict('{"scale":NaN}', "manifest")

    def test_manifest_requires_nfc_unicode(self) -> None:
        manifest = make_manifest()
        manifest["display_name"] = "Cafe\u0301"
        errors = probe.validate_manifest(
            manifest, probe.inspect_glb_bytes(
                make_animated_glb(), require_character=True
            )
        )
        self.assertTrue(any("NFC-normalized Unicode" in error for error in errors))

    def test_archive_inventory_fails_closed_without_license(self) -> None:
        dae = b'''<?xml version="1.0"?><COLLADA xmlns="http://www.collada.org/2005/11/COLLADASchema" version="1.4.1"><asset><unit meter="1"/><up_axis>Y_UP</up_axis></asset><library_geometries><geometry/></library_geometries></COLLADA>'''
        nested = io.BytesIO()
        with zipfile.ZipFile(nested, "w") as archive:
            archive.writestr("model/model.dae", dae)
        nested_bytes = nested.getvalue()
        outer = io.BytesIO()
        with zipfile.ZipFile(outer, "w") as archive:
            archive.writestr("source.zip", nested_bytes)
        report = probe.inspect_archive_bytes(outer.getvalue(), "source.zip")
        self.assertIn("no embedded license or copyright file", report["blockers"])
        self.assertEqual("dae", report["nested_archives"][0]["models"][0]["format"])

    def test_archive_traversal_is_rejected(self) -> None:
        archive_file = io.BytesIO()
        with zipfile.ZipFile(archive_file, "w") as archive:
            archive.writestr("../escape.glb", b"not a model")
        with self.assertRaises(probe.ProbeError):
            probe.inspect_archive_bytes(archive_file.getvalue(), "bad.zip")


if __name__ == "__main__":
    unittest.main()
