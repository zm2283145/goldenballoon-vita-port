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


def make_humanoid_glb() -> bytes:
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

    def update(document: dict[str, object]) -> None:
        nodes = [
            {"name": name, "translation": translations[index],
             **({"children": children[index]} if index in children else {})}
            for index, name in enumerate(names)
        ]
        nodes.append({"name": "character", "mesh": 0, "skin": 0})
        document["nodes"] = nodes
        document["scenes"] = [{"nodes": [0, 16]}]
        document["skins"] = [{
            "name": "rig", "joints": list(range(16)), "skeleton": 0,
        }]
        document["animations"][0]["channels"][0]["target"]["node"] = 3

    return rewrite_glb_document(make_animated_glb(), update)


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


if __name__ == "__main__":
    unittest.main()
