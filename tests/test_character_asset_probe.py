#!/usr/bin/env python3
"""ROM-free contract tests for the custom-character pipeline spike."""

from __future__ import annotations

import importlib.util
import io
import json
import struct
import tempfile
import unittest
import zipfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "character_asset_probe", ROOT / "tools" / "character_asset_probe.py"
)
assert SPEC is not None and SPEC.loader is not None
probe = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(probe)


def _align(data: bytearray, alignment: int = 4) -> None:
    data.extend(b"\0" * ((-len(data)) % alignment))


def make_animated_glb(external_buffer: bool = False) -> bytes:
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
            "metallicFactor": 0.0,
            "roughnessFactor": 0.8,
        }}],
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
    if external_buffer:
        document["buffers"][0]["uri"] = "mesh.bin"
    json_chunk = json.dumps(document, separators=(",", ":")).encode("utf-8")
    json_chunk += b" " * ((-len(json_chunk)) % 4)
    output = bytearray(struct.pack("<4sII", b"glTF", 2, 0))
    output += struct.pack("<II", len(json_chunk), probe.GLB_JSON_CHUNK) + json_chunk
    output += struct.pack("<II", len(binary), probe.GLB_BIN_CHUNK) + binary
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

    def test_package_is_deterministic_and_verifies(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            model = root / "model.glb"
            model.write_bytes(make_animated_glb())
            manifest = root / "manifest.json"
            manifest.write_text(json.dumps({
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
            }), encoding="utf-8")
            license_file = root / "LICENSE.txt"
            license_file.write_text("CC0-1.0 test fixture\n", encoding="utf-8")
            first = root / "first.mdkrchar"
            second = root / "second.mdkrchar"
            probe.build_package(model, manifest, license_file, first)
            probe.build_package(model, manifest, license_file, second)
            self.assertEqual(first.read_bytes(), second.read_bytes())
            verified = probe.verify_package(first)
            self.assertTrue(verified["valid"], verified["errors"])

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
