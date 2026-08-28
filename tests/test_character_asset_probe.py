#!/usr/bin/env python3
"""ROM-free contract tests for the custom-character pipeline spike."""

from __future__ import annotations

import importlib.util
import io
import json
import math
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


def make_portrait_png(size: int = 16) -> bytes:
    def chunk(kind: bytes, payload: bytes) -> bytes:
        return (
            struct.pack(">I", len(payload)) + kind + payload
            + struct.pack(">I", zlib.crc32(kind + payload) & 0xFFFFFFFF)
        )

    # Emit RGBA scanlines without relying on an image library.
    scanlines = bytearray()
    for y in range(size):
        scanlines.append(0)
        for x in range(size):
            scanlines.extend((x * 13 & 255, y * 17 & 255, 160, 255))
    return (
        probe.PNG_SIGNATURE
        + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(bytes(scanlines)))
        + chunk(b"IEND", b"")
    )


def make_animated_glb(
        external_buffer: bool = False, with_lod: bool = False,
        volumetric: bool = False) -> bytes:
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

    if volumetric:
        # A tetrahedron makes the exact front/side/top preview gate meaningful:
        # unlike the minimal conformance triangle below, it cannot disappear
        # merely because an inspection camera is edge-on to its only surface.
        vertex_count = 4
        positions = add(
            struct.pack(
                "<12f", -0.5, 0.0, 0.4, 0.5, 0.0, 0.4,
                0.0, 0.0, -0.5, 0.0, 1.0, 0.0),
            5126, "VEC3", vertex_count,
            minimum=[-0.5, 0.0, -0.5], maximum=[0.5, 1.0, 0.4],
        )
        normals = add(
            struct.pack(
                "<12f", -0.6188527, 0.3094264, 0.7219949,
                0.6188527, 0.3094264, 0.7219949,
                0.0, 0.3011314, -0.9535827, 0.0, 1.0, 0.0),
            5126, "VEC3", vertex_count,
        )
        uvs = add(
            struct.pack("<8f", 0.0, 0.0, 1.0, 0.0, 0.5, 0.0, 0.5, 1.0),
            5126, "VEC2", vertex_count,
        )
        index_values = struct.pack(
            "<12H", 0, 2, 1, 0, 1, 3, 1, 2, 3, 2, 0, 3)
        index_count = 12
    else:
        vertex_count = 3
        positions = add(
            struct.pack("<9f", -0.5, 0.0, 0.0, 0.5, 0.0, 0.0, 0.0, 1.0, 0.0),
            5126,
            "VEC3",
            vertex_count,
            minimum=[-0.5, 0.0, 0.0],
            maximum=[0.5, 1.0, 0.0],
        )
        normals = add(
            struct.pack("<9f", *(0.0, 0.0, 1.0) * vertex_count),
            5126, "VEC3", vertex_count,
        )
        uvs = add(
            struct.pack("<6f", 0.0, 0.0, 1.0, 0.0, 0.5, 1.0),
            5126, "VEC2", vertex_count,
        )
        index_values = struct.pack("<3H", 0, 1, 2)
        index_count = 3
    joints = add(
        bytes((0, 1, 0, 0) * vertex_count), 5121, "VEC4", vertex_count,
    )
    weights = add(
        struct.pack(
            f"<{vertex_count * 4}f",
            *(0.5, 0.5, 0.0, 0.0) * vertex_count),
        5126, "VEC4", vertex_count,
    )
    # Keep the accessor order stable for the hostile-layout mutation suite:
    # POSITION, NORMAL, UV, JOINTS, WEIGHTS, then indices.
    indices = add(index_values, 5123, "SCALAR", index_count)
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
            "source_forward": "+z",
            "target_height_m": 1.25,
            "contexts": {
                "select": {
                    "anchor": "ground",
                    "translation_m": [0.0, 0.0, 0.0],
                    "rotation_xyzw": [0.0, 0.0, 0.0, 1.0],
                    "scale": 1.0,
                },
                **{
                    vehicle: {
                        "anchor": "seat",
                        "translation_m": [0.0, 0.0, 0.0],
                        "rotation_xyzw": [0.0, 0.0, 0.0, 1.0],
                        "scale": 1.0,
                    }
                    for vehicle in ("car", "hovercraft", "plane")
                },
            },
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


def rewrite_glb_binary(data: bytes, update) -> bytes:
    document, binary = probe.parse_glb(data)
    payload = bytearray(binary or b"")
    update(document, payload)
    json_chunk = json.dumps(document, separators=(",", ":")).encode("utf-8")
    json_chunk += b" " * ((-len(json_chunk)) % 4)
    output = bytearray(struct.pack("<4sII", b"glTF", 2, 0))
    output += struct.pack("<II", len(json_chunk), probe.GLB_JSON_CHUNK) + json_chunk
    output += struct.pack("<II", len(payload), probe.GLB_BIN_CHUNK) + payload
    struct.pack_into("<I", output, 8, len(output))
    return bytes(output)


def make_humanoid_glb(
        *, with_lod: bool = False,
        unusual_proportions: bool = False) -> bytes:
    names = (
        "mixamorig:Hips", "mixamorig:Spine", "mixamorig:Spine2",
        "mixamorig:Head", "mixamorig:LeftArm", "mixamorig:LeftForeArm",
        "mixamorig:LeftHand", "mixamorig:RightArm",
        "mixamorig:RightForeArm", "mixamorig:RightHand",
        "mixamorig:LeftUpLeg", "mixamorig:LeftLeg", "mixamorig:LeftFoot",
        "mixamorig:RightUpLeg", "mixamorig:RightLeg",
        "mixamorig:RightFoot",
    )
    children = {
        0: [1, 10, 13], 1: [2], 2: [3, 4, 7],
        4: [5], 5: [6], 7: [8], 8: [9],
        10: [11], 11: [12], 13: [14], 14: [15],
    }
    translations = {
        0: [0.0, 1.0, 0.0], 1: [0.0, 0.20, 0.0],
        2: [0.0, 0.25, 0.0], 3: [0.0, 0.30, 0.0],
        4: [0.20, 0.15, 0.0], 5: [0.30, 0.0, 0.0],
        6: [0.25, 0.0, 0.0], 7: [-0.20, 0.15, 0.0],
        8: [-0.30, 0.0, 0.0], 9: [-0.25, 0.0, 0.0],
        10: [0.15, -0.15, 0.0], 11: [0.0, -0.45, 0.0],
        12: [0.0, -0.40, 0.10], 13: [-0.15, -0.15, 0.0],
        14: [0.0, -0.45, 0.0], 15: [0.0, -0.40, 0.10],
    }
    if unusual_proportions:
        # A deliberately non-human, license-clean stress fixture: a broad,
        # squat visible body, very high head landmark, wide shoulders/hips,
        # and short terminal arm/leg chains. It exists to prove that fitting,
        # camera evidence, contact exceptions, and rendering do not silently
        # assume ordinary adult-human ratios.
        translations.update({
            0: [0.0, 0.45, 0.0], 1: [0.0, 0.08, 0.0],
            2: [0.0, 0.10, 0.0], 3: [0.0, 0.95, 0.0],
            4: [0.48, 0.05, 0.0], 5: [0.09, 0.0, 0.0],
            6: [0.07, 0.0, 0.0], 7: [-0.48, 0.05, 0.0],
            8: [-0.09, 0.0, 0.0], 9: [-0.07, 0.0, 0.0],
            10: [0.42, -0.05, 0.0], 11: [0.0, -0.09, 0.0],
            12: [0.0, -0.07, 0.08], 13: [-0.42, -0.05, 0.0],
            14: [0.0, -0.09, 0.0], 15: [0.0, -0.07, 0.08],
        })

    def update(document: dict[str, object]) -> None:
        nodes = [
            {"name": name, "translation": translations[index],
             **({"children": children[index]} if index in children else {})}
            for index, name in enumerate(names)
        ]
        character_node = len(nodes)
        character = {"name": "character", "mesh": 0, "skin": 0}
        if unusual_proportions:
            character["scale"] = [2.8, 0.65, 1.6]
        nodes.append(character)
        if with_lod:
            lod_node = len(nodes)
            nodes.append({"name": "character_lod1", "mesh": 1, "skin": 0})
            nodes[character_node]["extensions"] = {
                "MSFT_lod": {"ids": [lod_node]},
            }
        document["nodes"] = nodes
        document["scenes"] = [{"nodes": [0, character_node]}]
        document["skins"] = [{
            "name": "rig", "joints": list(range(16)), "skeleton": 0,
        }]
        document["animations"][0]["channels"][0]["target"]["node"] = 3

    return rewrite_glb_document(
        make_animated_glb(
            with_lod=with_lod, volumetric=unusual_proportions), update
    )


def make_v4_manifest(portrait: bytes, *, humanoid: bool = False) -> dict[str, object]:
    manifest = make_manifest()
    manifest["schema"] = probe.PACKAGE_SCHEMA_V4
    manifest["identity"] = {
        "portrait_file": "portrait.png",
        "portrait_sha256": probe._sha256(portrait),
        "minimap_rgb": [220, 72, 144],
    }
    if humanoid:
        nodes = (
            "mixamorig:Hips", "mixamorig:Spine", "mixamorig:Spine2",
            "mixamorig:Head", "mixamorig:LeftArm",
            "mixamorig:LeftForeArm", "mixamorig:LeftHand",
            "mixamorig:RightArm", "mixamorig:RightForeArm",
            "mixamorig:RightHand", "mixamorig:LeftUpLeg",
            "mixamorig:LeftLeg", "mixamorig:LeftFoot",
            "mixamorig:RightUpLeg", "mixamorig:RightLeg",
            "mixamorig:RightFoot",
        )
        manifest["sockets"] = {
            "seat": "mixamorig:Hips", "head": "mixamorig:Head",
        }
        manifest["rig"] = {
            "mode": "humanoid-retarget-v1",
            "reviewed": True,
            "roles": {
                role: {
                    "node": node, "inferred": False, "confidence": 1.0,
                }
                for role, node in zip(probe.HUMANOID_ROLES, nodes)
            },
        }
    else:
        manifest["rig"] = {
            "mode": "authored-clips-only", "reviewed": False, "roles": {},
        }
    return manifest


class CharacterAssetProbeTests(unittest.TestCase):
    def test_semantic_playback_policy_is_engine_owned(self) -> None:
        self.assertEqual((0, 0.15), compiler._semantic_policy("race.land"))
        self.assertEqual((0, 0.15), compiler._semantic_policy("select.confirm"))
        self.assertEqual((1, 0.08), compiler._semantic_policy("race.steer"))
        self.assertEqual((1, 0.15), compiler._semantic_policy("fallback"))

    def test_generated_glb_is_character_ready(self) -> None:
        report = probe.inspect_glb_bytes(make_animated_glb(), require_character=True)
        self.assertEqual([], report["errors"])
        self.assertTrue(report["character_ready"])
        self.assertEqual(1, report["triangle_count"])
        self.assertEqual(2, report["max_joints"])
        self.assertEqual(1.0, report["animations"][0]["duration_seconds"])

    def test_generated_volumetric_glb_covers_inspection_planes(self) -> None:
        report = probe.inspect_glb_bytes(
            make_animated_glb(volumetric=True), require_character=True
        )
        self.assertEqual([], report["errors"])
        self.assertTrue(report["character_ready"])
        self.assertEqual(4, report["triangle_count"])
        self.assertEqual([-0.5, 0.0, -0.5], report["bbox_min"])
        self.assertEqual([0.5, 1.0, 0.4], report["bbox_max"])

    def test_generated_unusual_humanoid_is_character_ready(self) -> None:
        data = make_humanoid_glb(unusual_proportions=True)
        report = probe.inspect_glb_bytes(data, require_character=True)
        document, _ = probe.parse_glb(data)
        self.assertEqual([], report["errors"])
        self.assertTrue(report["character_ready"])
        self.assertEqual(4, report["triangle_count"])
        character = next(
            node for node in document["nodes"]
            if node.get("name") == "character"
        )
        self.assertEqual([2.8, 0.65, 1.6], character["scale"])
        head = next(
            node for node in document["nodes"]
            if node.get("name") == "mixamorig:Head"
        )
        self.assertEqual([0.0, 0.95, 0.0], head["translation"])

    def test_external_resource_is_rejected(self) -> None:
        report = probe.inspect_glb_bytes(make_animated_glb(external_buffer=True), require_character=True)
        self.assertTrue(any("external" in error for error in report["errors"]))

    def test_accessor_contract_rejects_hostile_layouts_without_exceptions(self) -> None:
        base = make_animated_glb()

        def mutate(path: str, update) -> None:
            report = probe.inspect_glb_bytes(
                rewrite_glb_document(base, update), require_character=True
            )
            self.assertTrue(
                any(path in error for error in report["errors"]),
                (path, report["errors"]),
            )

        mutate(
            "buffers[0].byteLength exceeds",
            lambda document: document["buffers"][0].update({
                "byteLength": document["buffers"][0]["byteLength"] + 4,
            }),
        )
        mutate(
            "bufferViews[0] exceeds",
            lambda document: document["bufferViews"][0].update({
                "byteLength": document["buffers"][0]["byteLength"] + 1,
            }),
        )
        mutate(
            "accessors[0].count must be a positive integer",
            lambda document: document["accessors"][0].update({"count": 0}),
        )
        mutate(
            "accessors[0].count must be a positive integer",
            lambda document: document["accessors"][0].update({"count": "3"}),
        )
        mutate(
            "accessors[0].componentType is unsupported",
            lambda document: document["accessors"][0].update({
                "componentType": [5126],
            }),
        )
        mutate(
            "accessors[0].type is unsupported",
            lambda document: document["accessors"][0].update({
                "type": {"name": "VEC3"},
            }),
        )
        mutate(
            "accessors[0].max contains a value outside FLOAT range",
            lambda document: document["accessors"][0].update({
                "max": [1.0e100, 1.0, 1.0],
            }),
        )
        mutate(
            "accessors[0] is not component-size aligned",
            lambda document: document["accessors"][0].update({"byteOffset": 2}),
        )
        mutate(
            "bufferViews[0].byteStride must be a 4-byte multiple",
            lambda document: document["bufferViews"][0].update({"byteStride": 6}),
        )
        mutate(
            "accessors[0].bufferView is required",
            lambda document: document["accessors"][0].pop("bufferView"),
        )
        mutate(
            "accessors[0] uses sparse storage",
            lambda document: document["accessors"][0].update({"sparse": {}}),
        )
        mutate(
            "accessors[0] cannot normalize FLOAT",
            lambda document: document["accessors"][0].update({"normalized": True}),
        )
        mutate(
            "attributes.POSITION has a component/type/normalized combination",
            lambda document: document["accessors"][0].update({
                "type": "VEC2", "min": [-0.5, 0.0], "max": [0.5, 1.0],
            }),
        )
        mutate(
            "attributes.POSITION requires min and max",
            lambda document: document["accessors"][0].pop("min"),
        )
        mutate(
            "vertex attribute accessor counts do not match",
            lambda document: document["accessors"][1].update({"count": 2}),
        )
        mutate(
            ".indices must use an unnormalized unsigned SCALAR",
            lambda document: document["accessors"][5].update({
                "componentType": 5122,
            }),
        )
        mutate(
            "input/output accessor counts do not match",
            lambda document: document["accessors"][8].update({"count": 1}),
        )
        mutate(
            "inverseBindMatrices must be a FLOAT MAT4",
            lambda document: document["accessors"][6].update({"type": "MAT3"}),
        )
        mutate(
            "bufferView must not define byteStride or target",
            lambda document: document["bufferViews"][9].update({"target": 34962}),
        )

        def share_vertex_view_without_stride(document: dict) -> None:
            document["accessors"][1]["bufferView"] = 0

        mutate(
            "shared by vertex attributes but has no byteStride",
            share_vertex_view_without_stride,
        )

        mutate(
            "declared min/max do not match its binary values",
            lambda document: document["accessors"][0].update({
                "max": [2.0, 2.0, 2.0],
            }),
        )

        def binary_case(path: str, update) -> None:
            report = probe.inspect_glb_bytes(
                rewrite_glb_binary(base, update), require_character=True
            )
            self.assertTrue(
                any(path in error for error in report["errors"]),
                (path, report["errors"]),
            )

        def write_accessor_component(
            document: dict, payload: bytearray, accessor_index: int,
            component_offset: int, fmt: str, value,
        ) -> None:
            accessor = document["accessors"][accessor_index]
            view = document["bufferViews"][accessor["bufferView"]]
            struct.pack_into(
                fmt, payload,
                view.get("byteOffset", 0) + accessor.get("byteOffset", 0)
                + component_offset,
                value,
            )

        binary_case(
            "attributes.POSITION contains NaN or infinity",
            lambda document, payload: write_accessor_component(
                document, payload, 0, 0, "<f", math.nan
            ),
        )
        binary_case(
            "attributes.NORMAL contains a zero or non-finite direction",
            lambda document, payload: write_accessor_component(
                document, payload, 1, 8, "<f", 0.0
            ),
        )
        binary_case(
            "attributes.WEIGHTS_0 must contain non-negative influences",
            lambda document, payload: write_accessor_component(
                document, payload, 4, 0, "<f", -0.5
            ),
        )
        binary_case(
            "indices contains primitive restart or an out-of-range vertex index",
            lambda document, payload: write_accessor_component(
                document, payload, 5, 4, "<H", 3
            ),
        )
        binary_case(
            "JOINTS_0 exceeds an instanced skin palette",
            lambda document, payload: write_accessor_component(
                document, payload, 3, 0, "<B", 2
            ),
        )
        binary_case(
            "input values must be finite, non-negative, and strictly increasing",
            lambda document, payload: write_accessor_component(
                document, payload, 7, 4, "<f", 0.0
            ),
        )
        binary_case(
            "output contains NaN or infinity",
            lambda document, payload: write_accessor_component(
                document, payload, 8, 0, "<f", math.inf
            ),
        )
        binary_case(
            "rotation key is not a unit quaternion",
            lambda document, payload: write_accessor_component(
                document, payload, 8, 12, "<f", 0.5
            ),
        )
        binary_case(
            "inverseBindMatrices contains NaN or infinity",
            lambda document, payload: write_accessor_component(
                document, payload, 6, 0, "<f", math.nan
            ),
        )
        mutate(
            "attributes.TANGENT tangent handedness must be -1 or 1",
            lambda document: document["meshes"][0]["primitives"][0][
                "attributes"
            ].update({"TANGENT": 4}),
        )

    def test_compiler_accessor_reader_rechecks_the_storage_boundary(self) -> None:
        def rejected(update, message: str) -> None:
            document, binary = probe.parse_glb(make_animated_glb())
            update(document)
            with self.assertRaisesRegex(compiler.CompileError, message):
                reader = compiler.AccessorReader(document, binary or b"")
                reader.values(0)

        rejected(
            lambda document: document["accessors"][0].update({"count": 0}),
            "invalid count",
        )
        rejected(
            lambda document: document["accessors"][0].update({
                "componentType": [5126],
            }),
            "unsupported component/type",
        )
        rejected(
            lambda document: document["accessors"][0].pop("bufferView"),
            "has no bufferView",
        )
        rejected(
            lambda document: document["accessors"][0].update({
                "byteOffset": 2,
            }),
            "not component-size aligned",
        )

    def test_scene_material_and_embedded_png_profile_fails_closed(self) -> None:
        base = make_animated_glb()

        def rejected(path: str, update) -> None:
            model = rewrite_glb_document(base, update)
            report = probe.inspect_glb_bytes(model, require_character=True)
            self.assertTrue(
                any(path in error for error in report["errors"]),
                (path, report["errors"]),
            )
            with self.assertRaises(compiler.CompileError):
                compiler.compile_character(model, make_manifest(), bytes(32))

        rejected(
            "node hierarchy contains a cycle",
            lambda document: document["nodes"][1].update({"children": [0]}),
        )
        rejected(
            "matrix contains shear",
            lambda document: document["nodes"][2].update({
                "matrix": [
                    1.0, 0.0, 0.0, 0.0,
                    0.5, 1.0, 0.0, 0.0,
                    0.0, 0.0, 1.0, 0.0,
                    0.0, 0.0, 0.0, 1.0,
                ],
            }),
        )
        def overflow_world_transform(document: dict) -> None:
            document["nodes"][0].update({
                "children": [1, 2], "scale": [3.0e38, 3.0e38, 3.0e38],
            })
            document["nodes"][2].update({
                "scale": [3.0e38, 3.0e38, 3.0e38],
            })
            document["scenes"][0]["nodes"] = [0]

        rejected("world transform exceeds finite FLOAT range",
                 overflow_world_transform)
        rejected(
            "alphaCutoff must be from 0 to 1",
            lambda document: document["materials"][0].update({
                "alphaCutoff": {"hostile": True},
            }),
        )
        rejected(
            "mimeType must be 'image/png'",
            lambda document: document["images"][0].update({
                "mimeType": ["image/png"],
            }),
        )
        rejected(
            "textures[0].extensions must be an object",
            lambda document: document["textures"][0].update({
                "extensions": None,
            }),
        )
        rejected(
            "unsupported required extensions: KHR_materials_specular",
            lambda document: document.update({
                "extensionsUsed": ["KHR_materials_specular"],
                "extensionsRequired": ["KHR_materials_specular"],
            }),
        )

        def corrupt_png_checksum(document: dict, payload: bytearray) -> None:
            view = document["bufferViews"][document["images"][0]["bufferView"]]
            payload[view["byteOffset"] + view["byteLength"] - 5] ^= 1

        report = probe.inspect_glb_bytes(
            rewrite_glb_binary(base, corrupt_png_checksum),
            require_character=True,
        )
        self.assertTrue(any(
            "PNG has a bad chunk checksum" in error
            for error in report["errors"]
        ), report["errors"])

        def lie_about_png_height(document: dict, payload: bytearray) -> None:
            view = document["bufferViews"][document["images"][0]["bufferView"]]
            start = view["byteOffset"]
            struct.pack_into(">I", payload, start + 20, 2)
            crc = zlib.crc32(payload[start + 12:start + 29]) & 0xFFFFFFFF
            struct.pack_into(">I", payload, start + 29, crc)

        report = probe.inspect_glb_bytes(
            rewrite_glb_binary(base, lie_about_png_height),
            require_character=True,
        )
        self.assertTrue(any(
            "pixels do not match its declared dimensions" in error
            for error in report["errors"]
        ), report["errors"])

    def test_compiler_metadata_helpers_reject_hostile_types(self) -> None:
        with self.assertRaises(compiler.CompileError):
            compiler._finite({"x": 1}, "factor")
        with self.assertRaises(compiler.CompileError):
            compiler._finite_scalar([], "factor")
        with self.assertRaises(compiler.CompileError):
            compiler._source_name({"x": 1}, "fallback", "node[0]")
        with self.assertRaises(compiler.CompileError):
            compiler._texture_source({"extensions": None, "source": 0})
        with self.assertRaises(compiler.CompileError):
            compiler._item([{}], False, "node")

    def test_hostile_glb_diagnostics_are_bounded(self) -> None:
        model = rewrite_glb_document(
            make_animated_glb(),
            lambda document: document.update({
                "nodes": [None] * (probe.MAX_GLTF_DIAGNOSTICS + 100),
            }),
        )
        report = probe.inspect_glb_bytes(model, require_character=True)
        self.assertEqual(probe.MAX_GLTF_DIAGNOSTICS + 1, len(report["errors"]))
        self.assertIn("additional GLB diagnostics were suppressed",
                      report["errors"][-1])

    def test_accessor_metadata_type_fuzz_fails_as_policy_not_exception(self) -> None:
        base = make_animated_glb()
        cases = [
            ("accessors", field, value)
            for field in (
                "componentType", "type", "count", "bufferView",
                "byteOffset", "normalized", "min", "max",
            )
            for value in (None, True, [], {}, "invalid", -1)
        ] + [
            ("bufferViews", field, value)
            for field in (
                "buffer", "byteOffset", "byteLength", "byteStride", "target",
            )
            for value in (None, True, [], {}, "invalid", -1)
        ] + [
            ("buffers", field, value)
            for field in ("byteLength", "uri")
            for value in (None, True, [], {}, "invalid", -1)
        ]
        for array_name, field, value in cases:
            def update(document: dict, *, array_name=array_name,
                       field=field, value=value) -> None:
                document[array_name][0][field] = value

            report = probe.inspect_glb_bytes(
                rewrite_glb_document(base, update), require_character=True
            )
            self.assertTrue(
                report["errors"], (array_name, field, value)
            )

    def test_compiler_facing_metadata_fuzz_has_only_controlled_outcomes(self) -> None:
        base = make_animated_glb()
        fields = {
            "nodes": (
                "name", "children", "mesh", "skin", "matrix",
                "translation", "rotation", "scale", "extensions",
            ),
            "scenes": ("nodes",),
            "meshes": ("name", "primitives"),
            "materials": (
                "name", "pbrMetallicRoughness", "alphaMode",
                "alphaCutoff", "doubleSided", "normalTexture",
                "occlusionTexture", "emissiveTexture", "emissiveFactor",
            ),
            "images": ("name", "mimeType", "bufferView", "uri"),
            "textures": ("name", "source", "sampler", "extensions"),
            "skins": ("name", "joints", "skeleton", "inverseBindMatrices"),
            "animations": ("name", "samplers", "channels"),
        }
        values = (
            None, True, False, -1, 1.5, 1.0e307, 10 ** 120,
            "invalid", [], {}, {"hostile": True},
        )
        for array_name, names in fields.items():
            for field in names:
                for value in values:
                    def update(document: dict, *, array_name=array_name,
                               field=field, value=value) -> None:
                        document[array_name][0][field] = value

                    model = rewrite_glb_document(base, update)
                    report = probe.inspect_glb_bytes(
                        model, require_character=True
                    )
                    try:
                        compiler.compile_character(
                            model, make_manifest(), bytes(32)
                        )
                    except compiler.CompileError:
                        pass
                    except Exception as error:  # pragma: no cover - assertion path
                        self.fail(
                            f"uncontrolled {type(error).__name__} for "
                            f"{array_name}.{field}={value!r}: {error}"
                        )
                    self.assertIsInstance(report["errors"], list)

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
            cache = root / "compiled.mdkc"
            compiler.compile_package(first, cache)
            with zipfile.ZipFile(first) as archive:
                expected_digest = compiler.source_digest(
                    (name, archive.read(name)) for name in probe.PACKAGE_MEMBERS
                )
            self.assertEqual(expected_digest, cache.read_bytes()[20:52])

    def test_v3_identity_media_is_canonical_bounded_and_compiled(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            model = root / "model.glb"
            model.write_bytes(make_animated_glb())
            portrait = root / "portrait.png"
            portrait.write_bytes(make_portrait_png())
            manifest_data = make_manifest()
            manifest_data["schema"] = probe.PACKAGE_SCHEMA_V3
            manifest_data["identity"] = {
                "minimap_rgb": [220, 72, 144],
                "short_name": "Proof",
                "narration_name": "Pipeline Proof character",
                "sort_label": "Proof, Pipeline",
            }
            manifest = root / "manifest.json"
            manifest.write_text(json.dumps(manifest_data), encoding="utf-8")
            license_file = root / "LICENSE.txt"
            license_file.write_text("CC0-1.0 test fixture\n", encoding="utf-8")
            package = root / "identity.mdkrchar"
            probe.build_package(
                model, manifest, license_file, package, portrait_path=portrait
            )
            verified = probe.verify_package(package)
            self.assertTrue(verified["valid"], verified["errors"])
            self.assertEqual(16, verified["portrait"]["width"])
            with zipfile.ZipFile(package) as archive:
                canonical = probe.json_loads_strict(archive.read("manifest.json"))
                compiled, report = compiler.compile_character(
                    archive.read("model.glb"), canonical, bytes(32),
                    archive.read("portrait.png"),
                )
            sections = _compiled_sections(compiled)
            self.assertEqual(1, sections[compiler.SECTION_IDENTITY]["count"])
            self.assertEqual(
                1, sections[compiler.SECTION_IDENTITY_NAMES]["count"]
            )
            self.assertEqual(
                len(make_portrait_png()), sections[compiler.SECTION_IDENTITY_DATA]["size"]
            )
            self.assertTrue(report["identity_portrait"])
            self.assertEqual([220, 72, 144], report["minimap_rgb"])
            self.assertEqual("Proof", report["identity_short_name"])
            self.assertEqual(
                "Pipeline Proof character",
                report["identity_narration_name"],
            )
            self.assertEqual("Proof, Pipeline", report["identity_sort_label"])

    def test_v4_authored_clips_only_is_a_first_class_rig_mode(self) -> None:
        portrait = make_portrait_png()
        manifest = make_v4_manifest(portrait)
        model = make_animated_glb()
        policy = probe.inspect_glb_bytes(model, require_character=True)
        self.assertEqual([], probe.validate_manifest(manifest, policy))
        compiled, report = compiler.compile_character(
            model, manifest, bytes(32), portrait
        )
        sections = _compiled_sections(compiled)
        self.assertEqual(1, sections[compiler.SECTION_RIG]["count"])
        self.assertEqual(0, sections[compiler.SECTION_RIG_ROLES]["count"])
        self.assertEqual("authored-clips-only", report["rig_mode"])
        self.assertFalse(report["rig_reviewed"])
        self.assertEqual(0, report["rig_roles"])

    def test_absent_identity_name_overrides_preserve_long_display_fallback(self) -> None:
        portrait = make_portrait_png()
        manifest = make_v4_manifest(portrait)
        manifest["display_name"] = "A" * 96
        model = make_animated_glb()
        policy = probe.inspect_glb_bytes(model, require_character=True)
        self.assertEqual([], probe.validate_manifest(manifest, policy))
        compiled, report = compiler.compile_character(
            model, manifest, bytes(32), portrait
        )
        sections = _compiled_sections(compiled)
        identity_offset = sections[compiler.SECTION_IDENTITY]["offset"]
        names_offset = sections[compiler.SECTION_IDENTITY_NAMES]["offset"]
        self.assertEqual(0, struct.unpack_from(
            "<I", compiled, identity_offset + 20
        )[0])
        self.assertEqual((0, 0, 0), struct.unpack_from(
            compiler.IDENTITY_NAMES_FORMAT, compiled, names_offset
        ))
        self.assertEqual("A" * 96, report["identity_short_name"])
        self.assertEqual("A" * 96, report["identity_narration_name"])
        self.assertEqual("A" * 96, report["identity_sort_label"])

    def test_v4_humanoid_roles_compile_with_reviewed_hierarchy(self) -> None:
        portrait = make_portrait_png()
        manifest = make_v4_manifest(portrait, humanoid=True)
        model = make_humanoid_glb()
        policy = probe.inspect_glb_bytes(model, require_character=True)
        self.assertEqual([], probe.validate_manifest(manifest, policy))
        compiled, report = compiler.compile_character(
            model, manifest, bytes(32), portrait
        )
        sections = _compiled_sections(compiled)
        self.assertEqual(16, sections[compiler.SECTION_RIG_ROLES]["count"])
        self.assertEqual(struct.calcsize(compiler.RIG_ROLE_FORMAT),
                         sections[compiler.SECTION_RIG_ROLES]["stride"])
        self.assertEqual(0xFFFF, report["rig_role_mask"])
        self.assertTrue(report["rig_reviewed"])

    def test_v4_rig_validation_rejects_unsafe_role_metadata(self) -> None:
        portrait = make_portrait_png()
        manifest = make_v4_manifest(portrait, humanoid=True)
        policy = probe.inspect_glb_bytes(make_humanoid_glb(),
                                         require_character=True)
        del manifest["rig"]["roles"]["head"]
        manifest["rig"]["roles"]["hips"]["rest_rotation_xyzw"] = [0, 0, 0, 2]
        manifest["rig"]["roles"]["spine"]["node"] = \
            manifest["rig"]["roles"]["hips"]["node"]
        errors = probe.validate_manifest(manifest, policy)
        self.assertTrue(any("missing required humanoid roles" in error
                            for error in errors))
        self.assertTrue(any("normalized quaternion" in error
                            for error in errors))
        self.assertTrue(any("distinct nodes" in error for error in errors))

    def test_compiler_rejects_humanoid_role_hierarchy_mismatch(self) -> None:
        portrait = make_portrait_png()
        manifest = make_v4_manifest(portrait, humanoid=True)
        roles = manifest["rig"]["roles"]
        roles["head"]["node"], roles["foot.left"]["node"] = (
            roles["foot.left"]["node"], roles["head"]["node"]
        )
        with self.assertRaisesRegex(compiler.CompileError, "rig hierarchy"):
            compiler.compile_character(
                make_humanoid_glb(), manifest, bytes(32), portrait
            )

    def test_portrait_profile_rejects_corrupt_or_animated_png(self) -> None:
        bad_crc = bytearray(make_portrait_png())
        bad_crc[-5] ^= 1
        with self.assertRaisesRegex(probe.ProbeError, "checksum"):
            probe.inspect_portrait_png(bytes(bad_crc))
        animated = make_portrait_png().replace(
            b"IDAT", b"acTL", 1
        )
        with self.assertRaises(probe.ProbeError):
            probe.inspect_portrait_png(animated)

    def test_animationless_skin_uses_explicit_compiler_bind_fallback(self) -> None:
        model = rewrite_glb_document(
            make_animated_glb(),
            lambda document: document.pop("animations", None),
        )
        report = probe.inspect_glb_bytes(model, require_character=True)
        self.assertEqual([], report["errors"])
        self.assertTrue(any(
            "no source animation" in warning for warning in report["warnings"]
        ))
        manifest = make_manifest()
        manifest["animations"] = {
            "fallback": probe.BIND_POSE_FALLBACK,
            "states": {},
        }
        self.assertEqual([], probe.validate_manifest(manifest, report))
        first, first_report = compiler.compile_character(
            model, manifest, bytes(range(32))
        )
        second, second_report = compiler.compile_character(
            model, manifest, bytes(range(32))
        )
        self.assertEqual(first, second)
        self.assertEqual(first_report, second_report)
        self.assertEqual(1, first_report["animations"])
        self.assertEqual(0, first_report["motion_channels"])
        self.assertEqual(
            [probe.BIND_POSE_FALLBACK], first_report["static_animations"]
        )
        sections = _compiled_sections(first)
        self.assertEqual(1, sections[compiler.SECTION_ANIMATIONS]["count"])
        self.assertEqual(1, sections[compiler.SECTION_CHANNELS]["count"])
        self.assertEqual(2, sections[compiler.SECTION_KEYS]["count"])

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
        self.assertEqual(1, first_report["motion_channels"])
        self.assertEqual([], first_report["static_animations"])
        self.assertEqual(2, first_report["sockets"])
        self.assertEqual(1.25, first_report["target_height_m"])
        self.assertEqual(
            ["select", "car", "hovercraft", "plane"],
            first_report["attachment_contexts"],
        )
        sections = _compiled_sections(first)
        self.assertEqual(4, sections[compiler.SECTION_ATTACHMENTS]["count"])
        self.assertEqual(1, sections[compiler.SECTION_CALIBRATION]["count"])
        provenance = sections[compiler.SECTION_PROVENANCE]
        self.assertEqual(1, provenance["count"])
        self.assertEqual(
            struct.calcsize(compiler.PROVENANCE_FORMAT), provenance["stride"]
        )
        spdx, attribution, source_url, flags = struct.unpack_from(
            compiler.PROVENANCE_FORMAT, first, provenance["offset"]
        )
        strings = sections[compiler.SECTION_STRINGS]

        def compiled_string(offset: int) -> str:
            begin = strings["offset"] + offset
            end = first.index(b"\0", begin)
            return first[begin:end].decode("utf-8")

        self.assertEqual("CC0-1.0", compiled_string(spdx))
        self.assertEqual(
            "Generated MDKR test fixture", compiled_string(attribution)
        )
        self.assertEqual(
            "https://example.invalid/pipeline-proof",
            compiled_string(source_url),
        )
        self.assertEqual(1, flags)
        self.assertEqual(4, first_report["decoded_texture_bytes"])

    def test_disabled_animation_mapping_is_preserved_in_compiled_cache(self) -> None:
        model = make_animated_glb()
        manifest = make_manifest()
        manifest["animations"]["states"] = {"select.idle": "idle"}
        manifest["animations"]["disabled_states"] = ["select.idle"]
        policy = probe.inspect_glb_bytes(model, require_character=True)
        self.assertEqual([], probe.validate_manifest(manifest, policy))
        compiled, _ = compiler.compile_character(
            model, manifest, bytes(range(32))
        )
        sections = _compiled_sections(compiled)
        semantics = sections[compiler.SECTION_SEMANTICS]
        self.assertEqual(2, semantics["count"])
        _, _, flags, _ = struct.unpack_from(
            compiler.SEMANTIC_FORMAT,
            compiled,
            semantics["offset"] + semantics["stride"],
        )
        self.assertEqual(compiler.SEMANTIC_DISABLED, flags & 2)
        manifest["animations"]["disabled_states"] = ["race.missing"]
        errors = probe.validate_manifest(manifest, policy)
        self.assertTrue(any("unmapped states" in error for error in errors))
        manifest["animations"]["states"]["extension.wave"] = "idle"
        manifest["animations"]["disabled_states"] = ["extension.wave"]
        errors = probe.validate_manifest(manifest, policy)
        self.assertTrue(any("unsupported engine semantics" in error
                            for error in errors))

    def test_fallback_cannot_be_duplicated_as_a_state_mapping(self) -> None:
        manifest = make_manifest()
        manifest["animations"]["states"]["fallback"] = "idle"
        errors = probe.validate_manifest(
            manifest,
            probe.inspect_glb_bytes(make_animated_glb(), require_character=True),
        )
        self.assertTrue(any("'fallback' is reserved" in error
                            for error in errors))

    def test_v1_manifest_remains_valid_and_gets_safe_context_defaults(self) -> None:
        manifest = make_manifest()
        manifest["schema"] = probe.PACKAGE_SCHEMA_V1
        manifest["presentation"] = {
            "scale": [1.0, 1.0, 1.0],
            "translation_m": [0.0, 0.0, 0.0],
            "rotation_xyzw": [0.0, 0.0, 0.0, 1.0],
            "lod_bias": 0.0,
        }
        report = probe.inspect_glb_bytes(make_animated_glb(), require_character=True)
        self.assertEqual([], probe.validate_manifest(manifest, report))
        _, compiled_report = compiler.compile_character(
            make_animated_glb(), manifest, bytes(32)
        )
        self.assertFalse(compiled_report["calibration_explicit"])
        self.assertEqual(
            ["select", "car", "hovercraft", "plane"],
            compiled_report["attachment_contexts"],
        )

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
        with self.assertRaisesRegex(probe.ProbeError, "non-finite number"):
            probe.json_loads_strict('{"scale":1e10000}', "manifest")
        with self.assertRaisesRegex(probe.ProbeError, "oversized integer"):
            probe.json_loads_strict(
                '{"scale":' + ('9' * 129) + '}', "manifest"
            )

    def test_manifest_requires_nfc_unicode(self) -> None:
        manifest = make_manifest()
        manifest["display_name"] = "Cafe\u0301"
        errors = probe.validate_manifest(
            manifest, probe.inspect_glb_bytes(
                make_animated_glb(), require_character=True
            )
        )
        self.assertTrue(any("NFC-normalized Unicode" in error for error in errors))

    def test_review_text_uses_the_native_bounded_printable_profile(self) -> None:
        report = probe.inspect_glb_bytes(
            make_animated_glb(), require_character=True
        )
        for field, value in (
            ("spdx", "S" * 129),
            ("attribution", "author\nspoofed label"),
            ("source_url", "https://example.invalid/\u2066spoof"),
        ):
            manifest = make_manifest()
            manifest["license"][field] = value
            errors = probe.validate_manifest(manifest, report)
            self.assertTrue(
                any(f"manifest.license.{field}" in error for error in errors),
                (field, errors),
            )
        manifest = make_manifest()
        manifest["display_name"] = "\U0001f3c1" * 25
        errors = probe.validate_manifest(manifest, report)
        self.assertTrue(any("printable UTF-8 bytes" in error for error in errors))

    def test_spdx_expression_grammar_is_structurally_validated(self) -> None:
        for expression in (
            "MIT",
            "CC-BY-4.0",
            "GPL-2.0-or-later+",
            "MIT OR Apache-2.0",
            "MIT or Apache-2.0",
            "MIT AND (Apache-2.0 OR BSD-3-Clause)",
            "GPL-2.0-only WITH Classpath-exception-2.0",
            "GPL-2.0-only with AdditionRef-Artist-Permission",
            "LicenseRef-Community-Grant",
            "LicenseRef-.community-",
            "DocumentRef-Pack:LicenseRef-Artist-Terms",
            "DocumentRef-.pack-:LicenseRef-.artist-terms",
            "GPL-2.0-only WITH DocumentRef-Pack:AdditionRef-Terms",
        ):
            self.assertIsNone(
                probe.validate_spdx_expression(expression), expression
            )
        for expression in (
            "",
            "NONE",
            "NOASSERTION",
            "MIT Or Apache-2.0",
            "MIT Apache-2.0",
            "MIT OR",
            "OR MIT",
            "(MIT OR Apache-2.0",
            "MIT OR Apache-2.0)",
            "(MIT OR Apache-2.0) WITH Classpath-exception-2.0",
            "MIT WITH",
            "MIT WITH LicenseRef-Exception",
            "MIT WITH AdditionRef-",
            "AdditionRef-Artist-Permission",
            "LicenseRef-",
            "LicenseRef-Custom+",
            "GPL-2.0 +",
            "DocumentRef-Pack:MIT",
            "DocumentRef-:LicenseRef-Terms",
            "MIT/Apache-2.0",
            "MIT ∨ Apache-2.0",
        ):
            self.assertIsNotNone(
                probe.validate_spdx_expression(expression), expression
            )
        manifest = make_manifest()
        manifest["license"]["spdx"] = "MIT Or Apache-2.0"
        errors = probe.validate_manifest(
            manifest,
            probe.inspect_glb_bytes(make_animated_glb(), require_character=True),
        )
        self.assertTrue(any("valid SPDX expression" in error for error in errors))

    def test_checked_in_schema_matches_provenance_text_bounds(self) -> None:
        for schema_name in (
            "mdkr-character-source-v1.schema.json",
            "mdkr-character-source-v2.schema.json",
        ):
            schema = json.loads(
                (ROOT / "docs" / "ref" / schema_name).read_text(
                    encoding="utf-8"
                )
            )
            license_properties = schema["properties"]["license"]["properties"]
            self.assertEqual(128, license_properties["spdx"]["maxLength"])
            self.assertIn("authoritative expression grammar",
                          license_properties["spdx"]["description"])
            self.assertEqual(
                256, license_properties["attribution"]["maxLength"]
            )
            self.assertEqual(
                2048, license_properties["source_url"]["maxLength"]
            )

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
        self.assertGreater(report["compressed_member_bytes"], 0)
        self.assertEqual(
            report["expanded_bytes"], report["declared_expanded_bytes"]
        )
        self.assertEqual(
            probe.MAX_ARCHIVE_EXPANSION_RATIO,
            report["expansion_ratio_limit"],
        )
        self.assertEqual(
            probe.ARCHIVE_EXPANSION_SLACK_BYTES,
            report["expansion_slack_bytes"],
        )

    def test_archive_inventory_finds_deep_license_and_requires_real_model(self) -> None:
        deep = io.BytesIO()
        with zipfile.ZipFile(deep, "w") as archive:
            archive.writestr("LICENSE.txt", "CC0 fixture")
            archive.writestr("model.glb", make_animated_glb())
        middle = io.BytesIO()
        with zipfile.ZipFile(middle, "w") as archive:
            archive.writestr("deep.zip", deep.getvalue())
        outer = io.BytesIO()
        with zipfile.ZipFile(outer, "w") as archive:
            archive.writestr("middle.zip", middle.getvalue())
        report = probe.inspect_archive_bytes(outer.getvalue(), "outer.zip")
        self.assertNotIn(
            "no embedded license or copyright file", report["blockers"]
        )
        self.assertNotIn("no supported model candidate", report["blockers"])

        empty_nested = io.BytesIO()
        with zipfile.ZipFile(empty_nested, "w") as archive:
            archive.writestr("readme.txt", "no model")
        empty_outer = io.BytesIO()
        with zipfile.ZipFile(empty_outer, "w") as archive:
            archive.writestr("nested.zip", empty_nested.getvalue())
            archive.writestr("LICENSE.txt", "CC0 fixture")
        empty_report = probe.inspect_archive_bytes(
            empty_outer.getvalue(), "empty.zip"
        )
        self.assertIn("no supported model candidate", empty_report["blockers"])

    def test_archive_traversal_is_rejected(self) -> None:
        archive_file = io.BytesIO()
        with zipfile.ZipFile(archive_file, "w") as archive:
            archive.writestr("../escape.glb", b"not a model")
        with self.assertRaises(probe.ProbeError):
            probe.inspect_archive_bytes(archive_file.getvalue(), "bad.zip")

    def test_archive_member_compression_bomb_is_rejected_before_read(self) -> None:
        archive_file = io.BytesIO()
        with zipfile.ZipFile(
            archive_file, "w", compression=zipfile.ZIP_DEFLATED
        ) as archive:
            archive.writestr("model.glb", b"\0" * (8 * 1024 * 1024))
            archive.writestr("LICENSE.txt", "CC0 fixture")
        with self.assertRaisesRegex(
            probe.ProbeError, "member compression ratio"
        ):
            probe.inspect_archive_bytes(archive_file.getvalue(), "bomb.zip")

    def test_archive_aggregate_compression_bomb_is_rejected_before_read(self) -> None:
        archive_file = io.BytesIO()
        with zipfile.ZipFile(
            archive_file, "w", compression=zipfile.ZIP_DEFLATED
        ) as archive:
            for index in range(4):
                archive.writestr(
                    f"padding-{index}.bin", b"\0" * (512 * 1024)
                )
            archive.writestr("LICENSE.txt", "CC0 fixture")
        with self.assertRaisesRegex(
            probe.ProbeError, "aggregate compression ratio"
        ):
            probe.inspect_archive_bytes(
                archive_file.getvalue(), "aggregate-bomb.zip"
            )

    def test_nested_archive_reapplies_compression_budget(self) -> None:
        nested = io.BytesIO()
        with zipfile.ZipFile(
            nested, "w", compression=zipfile.ZIP_DEFLATED
        ) as archive:
            archive.writestr("model.glb", b"\0" * (8 * 1024 * 1024))
            archive.writestr("LICENSE.txt", "CC0 fixture")
        outer = io.BytesIO()
        with zipfile.ZipFile(
            outer, "w", compression=zipfile.ZIP_STORED
        ) as archive:
            archive.writestr("nested.zip", nested.getvalue())
        with self.assertRaisesRegex(
            probe.ProbeError, "member compression ratio"
        ):
            probe.inspect_archive_bytes(outer.getvalue(), "nested-bomb.zip")

    def test_archive_rejects_nonportable_compression_method(self) -> None:
        archive_file = io.BytesIO()
        with zipfile.ZipFile(
            archive_file, "w", compression=zipfile.ZIP_BZIP2
        ) as archive:
            archive.writestr("LICENSE.txt", "CC0 fixture")
        with self.assertRaisesRegex(
            probe.ProbeError, "unsupported ZIP compression method"
        ):
            probe.inspect_archive_bytes(archive_file.getvalue(), "bzip2.zip")


if __name__ == "__main__":
    unittest.main()
