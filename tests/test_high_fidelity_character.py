#!/usr/bin/env python3
"""ROM/GPU-free proof that modern geometry bypasses retail N64 mesh limits."""

from __future__ import annotations

import json
import struct
import sys
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "tests"))

import character_asset_compiler as compiler  # noqa: E402
import character_asset_probe as probe  # noqa: E402
from test_character_asset_probe import make_manifest, make_portrait_png  # noqa: E402


def make_grid_glb(
    side: int = 160, *, degenerate_uvs: bool = False,
    authored_tangent: tuple[float, float, float, float] | None = None,
    normal_map: bool = False,
) -> bytes:
    binary = bytearray()
    views: list[dict[str, int]] = []
    accessors: list[dict[str, object]] = []

    def add(payload: bytes, component: int, kind: str, count: int,
            *, minimum=None, maximum=None) -> int:
        binary.extend(b"\0" * ((-len(binary)) % 4))
        offset = len(binary)
        binary.extend(payload)
        views.append({"buffer": 0, "byteOffset": offset, "byteLength": len(payload)})
        accessor: dict[str, object] = {
            "bufferView": len(views) - 1, "componentType": component,
            "count": count, "type": kind,
        }
        if minimum is not None:
            accessor["min"] = minimum
        if maximum is not None:
            accessor["max"] = maximum
        accessors.append(accessor)
        return len(accessors) - 1

    count = side * side
    positions = bytearray()
    uvs = bytearray()
    for y in range(side):
        for x in range(side):
            positions.extend(struct.pack("<3f", x / (side - 1) - 0.5,
                                         y / (side - 1), 0.0))
            uvs.extend(struct.pack(
                "<2f",
                0.0 if degenerate_uvs else x / (side - 1),
                0.0 if degenerate_uvs else y / (side - 1),
            ))
    indices = bytearray()
    for y in range(side - 1):
        for x in range(side - 1):
            top = y * side + x
            indices.extend(struct.pack("<6I", top, top + 1, top + side,
                                       top + 1, top + side + 1, top + side))
    position_accessor = add(bytes(positions), 5126, "VEC3", count,
                            minimum=[-0.5, 0.0, 0.0], maximum=[0.5, 1.0, 0.0])
    normal_accessor = add(struct.pack("<3f", 0.0, 0.0, 1.0) * count,
                          5126, "VEC3", count)
    uv_accessor = add(bytes(uvs), 5126, "VEC2", count)
    tangent_accessor = (
        add(struct.pack("<4f", *authored_tangent) * count,
            5126, "VEC4", count)
        if authored_tangent is not None else None
    )
    joint_accessor = add(bytes((0, 0, 0, 0)) * count, 5121, "VEC4", count)
    weight_accessor = add(struct.pack("<4f", 1.0, 0.0, 0.0, 0.0) * count,
                          5126, "VEC4", count)
    index_accessor = add(bytes(indices), 5125, "SCALAR", len(indices) // 4)
    identity = (1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0,
                0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0)
    bind_accessor = add(struct.pack("<16f", *identity), 5126, "MAT4", 1)
    time_accessor = add(struct.pack("<2f", 0.0, 1.0), 5126, "SCALAR", 2,
                        minimum=[0.0], maximum=[1.0])
    animation_accessor = add(struct.pack("<6f", 0.0, 0.0, 0.0,
                                          0.0, 0.0, 0.0), 5126, "VEC3", 2)
    normal_image_view = None
    if normal_map:
        normal_image = make_portrait_png()
        binary.extend(b"\0" * ((-len(binary)) % 4))
        normal_image_offset = len(binary)
        binary.extend(normal_image)
        views.append({
            "buffer": 0, "byteOffset": normal_image_offset,
            "byteLength": len(normal_image),
        })
        normal_image_view = len(views) - 1
    document = {
        "asset": {"version": "2.0", "generator": "mdkr-high-fidelity-test"},
        "scene": 0, "scenes": [{"nodes": [0, 2]}],
        "nodes": [
            {"name": "root", "children": [1]}, {"name": "head"},
            {"name": "character", "mesh": 0, "skin": 0},
        ],
        "meshes": [{"primitives": [{
            "attributes": {
                "POSITION": position_accessor, "NORMAL": normal_accessor,
                "TEXCOORD_0": uv_accessor, "JOINTS_0": joint_accessor,
                "WEIGHTS_0": weight_accessor,
                **({"TANGENT": tangent_accessor}
                   if tangent_accessor is not None else {}),
            },
            "indices": index_accessor, "material": 0,
        }]}],
        "materials": [{
            "name": "body",
            "pbrMetallicRoughness": {
                "baseColorFactor": [0.7, 0.2, 0.1, 1.0],
                "metallicFactor": 0.0, "roughnessFactor": 0.7,
            },
            **({"normalTexture": {"index": 0}} if normal_map else {}),
        }],
        **({
            "images": [{
                "bufferView": normal_image_view, "mimeType": "image/png",
            }],
            "textures": [{"source": 0}],
        } if normal_map else {}),
        "skins": [{"name": "rig", "joints": [0], "skeleton": 0,
                   "inverseBindMatrices": bind_accessor}],
        "animations": [{"name": "idle", "samplers": [{
            "input": time_accessor, "output": animation_accessor,
            "interpolation": "LINEAR",
        }], "channels": [{"sampler": 0, "target": {
            "node": 1, "path": "translation",
        }}]}],
        "bufferViews": views, "accessors": accessors,
        "buffers": [{"byteLength": len(binary)}],
    }
    encoded = json.dumps(document, separators=(",", ":")).encode("utf-8")
    encoded += b" " * ((-len(encoded)) % 4)
    binary.extend(b"\0" * ((-len(binary)) % 4))
    output = bytearray(struct.pack("<4sII", b"glTF", 2, 0))
    output += struct.pack("<II", len(encoded), probe.GLB_JSON_CHUNK) + encoded
    output += struct.pack("<II", len(binary), probe.GLB_BIN_CHUNK) + binary
    struct.pack_into("<I", output, 8, len(output))
    return bytes(output)


class HighFidelityCharacterTests(unittest.TestCase):
    def test_ktx2_runtime_dependency_and_device_fallback_are_pinned(self) -> None:
        dependency = (ROOT / "cmake" / "character_basisu.cmake").read_text(
            encoding="utf-8"
        )
        self.assertIn(
            'set(MDKR_BASISU_COMMIT "4d6fc70eaf62ad0558e63e8d97eb9766118327a6")',
            dependency,
        )
        self.assertEqual(15, dependency.count('"external/basisu/'))
        self.assertIn('EXPECTED_HASH "SHA256=${sha256}"', dependency)
        self.assertIn("TLS_VERIFY ON", dependency)
        self.assertIn("MDKR_BASISU_LOCAL_CACHE", dependency)
        self.assertIn("file://${candidate}", dependency)

        backend = (ROOT / "platform" / "fast3d" /
                   "gfx_webgpu.c").read_text(encoding="utf-8")
        for feature in (
            "WGPUFeatureName_TextureCompressionBC",
            "WGPUFeatureName_TextureCompressionETC2",
            "WGPUFeatureName_TextureCompressionASTC",
        ):
            self.assertIn(feature, backend)
        self.assertIn("required_feature_count_without_compression", backend)
        self.assertIn("portable RGBA8 fallback", backend)
        self.assertIn("[WGPU-CHARACTER-KTX2]", backend)

    def test_webgpu_material_shader_uses_exact_srgb_transfer(self) -> None:
        shader_source = (ROOT / "platform" / "fast3d" /
                         "gfx_webgpu.c").read_text(encoding="utf-8")
        self.assertIn("v<=vec3<f32>(0.04045)", shader_source)
        self.assertIn("v<=vec3<f32>(0.0031308)", shader_source)
        self.assertIn(
            "srgbToLinear(u.fog.rgb)", shader_source,
            "display-space fog must be decoded before linear-light blending",
        )
        self.assertIn("u.eye.xyz-v.objectPos", shader_source)
        self.assertIn("u.eye.w>0.5", shader_source)
        self.assertNotIn(
            "let view=vec3<f32>(0.0,0.0,1.0)", shader_source,
            "specular response must use the exact presentation-camera eye",
        )
        self.assertNotIn(
            "pow(baseSample.rgb,vec3<f32>(2.2))", shader_source
        )
        self.assertNotIn(
            "pow(max(rgb,vec3<f32>(0.0)),vec3<f32>(1.0/2.2))",
            shader_source,
        )

    def test_fifty_thousand_triangle_skinned_grid_compiles(self) -> None:
        model = make_grid_glb()
        inspected = probe.inspect_glb_bytes(model, require_character=True)
        self.assertEqual([], inspected["errors"])
        compiled, report = compiler.compile_character(model, make_manifest(), bytes(32))
        self.assertGreater(report["triangles"], 50_000)
        self.assertEqual(25_600, report["vertices"])
        self.assertEqual(0, report["authored_tangent_primitives"])
        self.assertEqual(1, report["generated_tangent_primitives"])
        self.assertEqual(0, report["generated_tangent_degenerate_uv_triangles"])
        self.assertEqual(0, report["tangent_fallback_vertices"])
        self.assertGreater(len(compiled), 2_000_000)

    def test_model_above_old_hundred_thousand_vertex_cap_compiles(self) -> None:
        model = make_grid_glb(side=360)
        inspected = probe.inspect_glb_bytes(model, require_character=True)
        self.assertEqual([], inspected["errors"])
        compiled, report = compiler.compile_character(
            model, make_manifest(), bytes(32)
        )
        self.assertEqual(129_600, report["vertices"])
        self.assertGreater(report["triangles"], 250_000)
        self.assertGreater(len(compiled), 10_000_000)

    def test_degenerate_uvs_get_orthogonal_disclosed_fallbacks(self) -> None:
        model = make_grid_glb(side=2, degenerate_uvs=True, normal_map=True)
        compiled, report = compiler.compile_character(
            model, make_manifest(), bytes(32)
        )
        self.assertGreater(len(compiled), 0)
        self.assertEqual(2, report["generated_tangent_degenerate_uv_triangles"])
        self.assertEqual(4, report["tangent_fallback_vertices"])
        self.assertEqual(4, report["normal_map_tangent_fallback_vertices"])
        # Locate the vertex section from the authenticated section table.
        vertex_offset = struct.unpack_from(
            "<Q", compiled, 64 + compiler.MDKC_SECTION_ENTRY_BYTES + 8
        )[0]
        tangent = struct.unpack_from("<4f", compiled, vertex_offset + 24)
        normal = struct.unpack_from("<3f", compiled, vertex_offset + 12)
        self.assertAlmostEqual(0.0, sum(
            tangent[index] * normal[index] for index in range(3)
        ), places=6)
        self.assertAlmostEqual(1.0, sum(
            tangent[index] * tangent[index] for index in range(3)
        ), places=6)

    def test_parallel_authored_tangents_are_repaired_and_disclosed(self) -> None:
        model = make_grid_glb(
            side=2, authored_tangent=(0.0, 0.0, 1.0, -1.0)
        )
        _, report = compiler.compile_character(model, make_manifest(), bytes(32))
        self.assertEqual(1, report["authored_tangent_primitives"])
        self.assertEqual(0, report["generated_tangent_primitives"])
        self.assertEqual(4, report["authored_tangent_repaired_vertices"])
        self.assertEqual(4, report["tangent_fallback_vertices"])


if __name__ == "__main__":
    unittest.main()
