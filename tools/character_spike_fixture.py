#!/usr/bin/env python3
"""Generate the redistributable CC0 adversarial custom-character fixture.

The model is deliberately authored in an awkward but valid form: centimeter
coordinates sit beneath an additional scene scale, the pelvis and spine are
siblings, limbs are unusually long, the head and hair are oversized, three
materials are used, and the skinned source intentionally has no animation.
Those properties make
the fixture useful for authoring UX regressions without shipping third-party
art or relying on Blender output.

The generated artistic asset, portrait, manifest, and this generator are
dedicated to the public domain under CC0-1.0.  The repository's ordinary source
license still applies to integration code outside this generated fixture.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import struct
import zlib
from dataclasses import dataclass, field
from pathlib import Path
from typing import Iterable


FIXTURE_ID = "org.mdkr.adversarial-humanoid"
FIXTURE_NAME = "Adversarial Humanoid"
BIND_POSE_FALLBACK = "$bind"
FIXTURE_ATTRIBUTION = "Golden Balloon contributors; procedural CC0 fixture"
FIXTURE_SOURCE_URL = (
    "https://github.com/akratch/goldenballoon/"
    "blob/main/tools/character_spike_fixture.py"
)
FIXTURE_LICENSE = """CC0 1.0 Universal

To the extent possible under law, the person who associated CC0 with this
procedurally generated fixture has waived all copyright and related or
neighboring rights to the fixture worldwide.

https://creativecommons.org/publicdomain/zero/1.0/
"""
GLB_JSON_CHUNK = 0x4E4F534A
GLB_BIN_CHUNK = 0x004E4942


def _align(payload: bytearray, alignment: int = 4) -> None:
    payload.extend(b"\0" * ((-len(payload)) % alignment))


@dataclass
class BufferBuilder:
    payload: bytearray = field(default_factory=bytearray)
    views: list[dict[str, int]] = field(default_factory=list)
    accessors: list[dict[str, object]] = field(default_factory=list)

    def add_accessor(
        self, payload: bytes, component_type: int, kind: str, count: int,
        *, minimum: list[float] | None = None,
        maximum: list[float] | None = None,
    ) -> int:
        _align(self.payload)
        offset = len(self.payload)
        self.payload.extend(payload)
        view = len(self.views)
        self.views.append({
            "buffer": 0,
            "byteOffset": offset,
            "byteLength": len(payload),
        })
        accessor: dict[str, object] = {
            "bufferView": view,
            "componentType": component_type,
            "count": count,
            "type": kind,
        }
        if minimum is not None:
            accessor["min"] = minimum
        if maximum is not None:
            accessor["max"] = maximum
        self.accessors.append(accessor)
        return len(self.accessors) - 1

    def add_blob_view(self, payload: bytes) -> int:
        _align(self.payload)
        offset = len(self.payload)
        self.payload.extend(payload)
        self.views.append({
            "buffer": 0,
            "byteOffset": offset,
            "byteLength": len(payload),
        })
        return len(self.views) - 1


@dataclass
class Geometry:
    positions: list[tuple[float, float, float]] = field(default_factory=list)
    normals: list[tuple[float, float, float]] = field(default_factory=list)
    uvs: list[tuple[float, float]] = field(default_factory=list)
    joints: list[tuple[int, int, int, int]] = field(default_factory=list)
    weights: list[tuple[float, float, float, float]] = field(default_factory=list)
    indices: list[int] = field(default_factory=list)


def _append_box(
    geometry: Geometry, minimum: tuple[float, float, float],
    maximum: tuple[float, float, float], joint: int,
) -> None:
    x0, y0, z0 = minimum
    x1, y1, z1 = maximum
    faces = (
        ((1.0, 0.0, 0.0), ((x1, y0, z0), (x1, y1, z0),
                            (x1, y1, z1), (x1, y0, z1))),
        ((-1.0, 0.0, 0.0), ((x0, y0, z1), (x0, y1, z1),
                             (x0, y1, z0), (x0, y0, z0))),
        ((0.0, 1.0, 0.0), ((x0, y1, z0), (x0, y1, z1),
                            (x1, y1, z1), (x1, y1, z0))),
        ((0.0, -1.0, 0.0), ((x0, y0, z1), (x0, y0, z0),
                             (x1, y0, z0), (x1, y0, z1))),
        ((0.0, 0.0, 1.0), ((x1, y0, z1), (x1, y1, z1),
                            (x0, y1, z1), (x0, y0, z1))),
        ((0.0, 0.0, -1.0), ((x0, y0, z0), (x0, y1, z0),
                             (x1, y1, z0), (x1, y0, z0))),
    )
    for normal, vertices in faces:
        first = len(geometry.positions)
        geometry.positions.extend(vertices)
        geometry.normals.extend((normal,) * 4)
        geometry.uvs.extend(((0.0, 0.0), (0.0, 1.0),
                             (1.0, 1.0), (1.0, 0.0)))
        geometry.joints.extend(((joint, 0, 0, 0),) * 4)
        geometry.weights.extend(((1.0, 0.0, 0.0, 0.0),) * 4)
        geometry.indices.extend((first, first + 1, first + 2,
                                 first, first + 2, first + 3))


def _flatten(values: Iterable[Iterable[float | int]]) -> list[float | int]:
    return [component for value in values for component in value]


def _primitive(builder: BufferBuilder, geometry: Geometry, material: int) -> dict:
    position_values = _flatten(geometry.positions)
    positions = builder.add_accessor(
        struct.pack(f"<{len(position_values)}f", *position_values),
        5126, "VEC3", len(geometry.positions),
        minimum=[min(value[axis] for value in geometry.positions)
                 for axis in range(3)],
        maximum=[max(value[axis] for value in geometry.positions)
                 for axis in range(3)],
    )
    normal_values = _flatten(geometry.normals)
    normals = builder.add_accessor(
        struct.pack(f"<{len(normal_values)}f", *normal_values),
        5126, "VEC3", len(geometry.normals),
    )
    uv_values = _flatten(geometry.uvs)
    uvs = builder.add_accessor(
        struct.pack(f"<{len(uv_values)}f", *uv_values),
        5126, "VEC2", len(geometry.uvs),
    )
    joint_values = _flatten(geometry.joints)
    joints = builder.add_accessor(
        bytes(joint_values), 5121, "VEC4", len(geometry.joints),
    )
    weight_values = _flatten(geometry.weights)
    weights = builder.add_accessor(
        struct.pack(f"<{len(weight_values)}f", *weight_values),
        5126, "VEC4", len(geometry.weights),
    )
    indices = builder.add_accessor(
        struct.pack(f"<{len(geometry.indices)}H", *geometry.indices),
        5123, "SCALAR", len(geometry.indices),
        minimum=[min(geometry.indices)], maximum=[max(geometry.indices)],
    )
    return {
        "attributes": {
            "POSITION": positions,
            "NORMAL": normals,
            "TEXCOORD_0": uvs,
            "JOINTS_0": joints,
            "WEIGHTS_0": weights,
        },
        "indices": indices,
        "material": material,
    }


def _png(width: int, height: int, pixels: bytes) -> bytes:
    def chunk(kind: bytes, payload: bytes) -> bytes:
        return (
            struct.pack(">I", len(payload)) + kind + payload
            + struct.pack(">I", zlib.crc32(kind + payload) & 0xFFFFFFFF)
        )

    if len(pixels) != width * height * 4:
        raise ValueError("fixture PNG pixel count is inconsistent")
    scanlines = bytearray()
    for y in range(height):
        scanlines.append(0)
        scanlines.extend(pixels[y * width * 4:(y + 1) * width * 4])
    return (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height,
                                     8, 6, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(bytes(scanlines), 9))
        + chunk(b"IEND", b"")
    )


def portrait_png() -> bytes:
    pixels = bytearray()
    for y in range(40):
        for x in range(40):
            background = (18, 28, 48, 255)
            head = 8 <= x <= 31 and 8 <= y <= 28
            hair = (x <= 12 or x >= 27) and 5 <= y <= 32
            eye = y in (17, 18) and x in (15, 16, 23, 24)
            mouth = y == 24 and 16 <= x <= 23
            if hair:
                colour = (184, 54, 142, 255)
            elif head:
                colour = (238, 172, 112, 255)
            elif eye:
                colour = (12, 18, 28, 255)
            elif mouth:
                colour = (112, 28, 42, 255)
            else:
                colour = background
            # Facial features must win over the head fill.
            if eye:
                colour = (12, 18, 28, 255)
            if mouth:
                colour = (112, 28, 42, 255)
            pixels.extend(colour)
    return _png(40, 40, bytes(pixels))


def model_glb() -> bytes:
    builder = BufferBuilder()
    body = Geometry()
    hair = Geometry()
    face = Geometry()

    # Wide torso, long arms, short legs, large head: intentionally unlike the
    # canonical reference proportions used by the solver.
    _append_box(body, (-26, 68, -12), (26, 92, 12), 1)
    _append_box(body, (-23, 88, -11), (23, 137, 11), 3)
    _append_box(body, (-30, 136, -20), (30, 190, 20), 4)
    _append_box(body, (20, 124, -9), (68, 142, 9), 5)
    _append_box(body, (66, 125, -8), (112, 141, 8), 6)
    _append_box(body, (110, 123, -10), (126, 143, 10), 7)
    _append_box(body, (-68, 124, -9), (-20, 142, 9), 8)
    _append_box(body, (-112, 125, -8), (-66, 141, 8), 9)
    _append_box(body, (-126, 123, -10), (-110, 143, 10), 10)
    _append_box(body, (4, 37, -10), (25, 76, 10), 11)
    _append_box(body, (7, 7, -9), (23, 40, 9), 12)
    _append_box(body, (5, -1, -8), (27, 11, 22), 13)
    _append_box(body, (-25, 37, -10), (-4, 76, 10), 14)
    _append_box(body, (-23, 7, -9), (-7, 40, 9), 15)
    _append_box(body, (-27, -1, -8), (-5, 11, 22), 16)

    # The hair sits on +Z, making the recognizable face direction -Z.
    _append_box(hair, (-34, 145, 17), (34, 194, 32), 17)
    _append_box(hair, (-28, 94, 24), (28, 151, 39), 18)
    _append_box(hair, (-20, 42, 30), (20, 100, 45), 19)

    # Eyes and mouth sit beyond the -Z head surface. This gives a human reviewer
    # an unambiguous front without asking geometry heuristics to infer intent.
    _append_box(face, (-18, 163, -23), (-7, 177, -19), 4)
    _append_box(face, (7, 163, -23), (18, 177, -19), 4)
    _append_box(face, (-12, 148, -23), (12, 153, -19), 4)

    body_primitive = _primitive(builder, body, 0)
    hair_primitive = _primitive(builder, hair, 1)
    face_primitive = _primitive(builder, face, 2)

    # Raw node translations are centimeters. Both skeleton and mesh carry a
    # second 0.01 scale, reproducing the nested-unit mismatch seen in the
    # private example while remaining valid glTF.
    node_specs: list[tuple[str, list[float], list[int]]] = [
        ("Skl_Root", [0.0, 0.0, 0.0], [1, 2]),
        ("Hip", [0.0, 80.0, 0.0], [11, 14]),
        ("Spine1", [0.0, 96.0, 0.0], [3]),
        ("Chest", [0.0, 28.0, 0.0], [4, 5, 8]),
        ("Head", [0.0, 40.0, 0.0], [17]),
        ("UpperArm.L", [28.0, 8.0, 0.0], [6]),
        ("LowerArm.L", [45.0, 0.0, 0.0], [7]),
        ("Hand.L", [43.0, 0.0, 0.0], []),
        ("UpperArm.R", [-28.0, 8.0, 0.0], [9]),
        ("LowerArm.R", [-45.0, 0.0, 0.0], [10]),
        ("Hand.R", [-43.0, 0.0, 0.0], []),
        ("UpperLeg.L", [15.0, -20.0, 0.0], [12]),
        ("LowerLeg.L", [0.0, -35.0, 0.0], [13]),
        ("Foot.L", [0.0, -25.0, 12.0], []),
        ("UpperLeg.R", [-15.0, -20.0, 0.0], [15]),
        ("LowerLeg.R", [0.0, -35.0, 0.0], [16]),
        ("Foot.R", [0.0, -25.0, 12.0], []),
        ("Hair.Root", [0.0, 16.0, 24.0], [18]),
        ("Hair.Mid", [0.0, -48.0, 8.0], [19]),
        ("Hair.Tip", [0.0, -52.0, 7.0], []),
    ]
    nodes: list[dict[str, object]] = []
    for index, (name, translation, children) in enumerate(node_specs):
        node: dict[str, object] = {"name": name, "translation": translation}
        if children:
            node["children"] = children
        if index == 0:
            node["scale"] = [0.01, 0.01, 0.01]
        nodes.append(node)
    mesh_node = len(nodes)
    nodes.append({
        "name": "AdversarialCharacter",
        "mesh": 0,
        "skin": 0,
        "scale": [0.01, 0.01, 0.01],
    })

    # Inverse binds are expressed in mesh-local centimeter coordinates. At
    # bind pose the nested mesh/skeleton scales cancel without baking geometry.
    raw_globals: list[tuple[float, float, float]] = []
    parents = [-1] * len(node_specs)
    for parent, (_, _, children) in enumerate(node_specs):
        for child in children:
            parents[child] = parent
    for index, (_, translation, _) in enumerate(node_specs):
        x, y, z = translation
        parent = parents[index]
        if parent >= 0:
            px, py, pz = raw_globals[parent]
            x, y, z = x + px, y + py, z + pz
        raw_globals.append((x, y, z))
    inverse_values: list[float] = []
    for x, y, z in raw_globals:
        inverse_values.extend((
            1.0, 0.0, 0.0, 0.0,
            0.0, 1.0, 0.0, 0.0,
            0.0, 0.0, 1.0, 0.0,
            -x, -y, -z, 1.0,
        ))
    inverse_bind = builder.add_accessor(
        struct.pack(f"<{len(inverse_values)}f", *inverse_values),
        5126, "MAT4", len(node_specs),
    )
    body_png = _png(1, 1, bytes((204, 112, 62, 255)))
    hair_png = _png(1, 1, bytes((184, 54, 142, 255)))
    body_view = builder.add_blob_view(body_png)
    hair_view = builder.add_blob_view(hair_png)

    document = {
        "asset": {
            "version": "2.0",
            "generator": "Golden Balloon adversarial character fixture v1",
            "extras": {
                "declaredUnitMeters": 0.01,
                "fixtureLicense": "CC0-1.0",
            },
        },
        "scene": 0,
        "scenes": [{"nodes": [0, mesh_node]}],
        "nodes": nodes,
        "meshes": [{
            "name": "AdversarialBodyAndHair",
            "primitives": [body_primitive, hair_primitive, face_primitive],
        }],
        "materials": [
            {
                "name": "Body",
                "doubleSided": True,
                "pbrMetallicRoughness": {
                    "baseColorFactor": [0.8, 0.44, 0.24, 1.0],
                    "baseColorTexture": {"index": 0},
                    "metallicFactor": 0.0,
                    "roughnessFactor": 0.85,
                },
            },
            {
                "name": "HairMask",
                "alphaMode": "MASK",
                "alphaCutoff": 0.5,
                "doubleSided": True,
                "pbrMetallicRoughness": {
                    "baseColorFactor": [0.72, 0.21, 0.56, 1.0],
                    "baseColorTexture": {"index": 1},
                    "metallicFactor": 0.0,
                    "roughnessFactor": 0.7,
                },
            },
            {
                "name": "FaceDetails",
                "doubleSided": True,
                "pbrMetallicRoughness": {
                    "baseColorFactor": [0.04, 0.03, 0.08, 1.0],
                    "metallicFactor": 0.0,
                    "roughnessFactor": 0.9,
                },
            },
        ],
        "images": [
            {"name": "BodyPixel", "mimeType": "image/png",
             "bufferView": body_view},
            {"name": "HairPixel", "mimeType": "image/png",
             "bufferView": hair_view},
        ],
        "textures": [
            {"name": "BodyPixel", "source": 0},
            {"name": "HairPixel", "source": 1},
        ],
        "skins": [{
            "name": "AdversarialRig",
            "joints": list(range(len(node_specs))),
            "skeleton": 0,
            "inverseBindMatrices": inverse_bind,
        }],
        "bufferViews": builder.views,
        "accessors": builder.accessors,
        "buffers": [{"byteLength": len(builder.payload)}],
    }
    json_chunk = json.dumps(
        document, sort_keys=True, separators=(",", ":")
    ).encode("utf-8")
    json_chunk += b" " * ((-len(json_chunk)) % 4)
    _align(builder.payload)
    output = bytearray(struct.pack("<4sII", b"glTF", 2, 0))
    output.extend(struct.pack("<II", len(json_chunk), GLB_JSON_CHUNK))
    output.extend(json_chunk)
    output.extend(struct.pack("<II", len(builder.payload), GLB_BIN_CHUNK))
    output.extend(builder.payload)
    struct.pack_into("<I", output, 8, len(output))
    return bytes(output)


def manifest(portrait: bytes) -> dict[str, object]:
    role_nodes = (
        "Skl_Root", "Spine1", "Chest", "Head",
        "UpperArm.L", "LowerArm.L", "Hand.L",
        "UpperArm.R", "LowerArm.R", "Hand.R",
        "UpperLeg.L", "LowerLeg.L", "Foot.L",
        "UpperLeg.R", "LowerLeg.R", "Foot.R",
    )
    role_names = (
        "hips", "spine", "chest", "head",
        "upper_arm.left", "lower_arm.left", "hand.left",
        "upper_arm.right", "lower_arm.right", "hand.right",
        "upper_leg.left", "lower_leg.left", "foot.left",
        "upper_leg.right", "lower_leg.right", "foot.right",
    )
    contexts = {
        "select": {
            "anchor": "ground",
            "translation_m": [0.0, 0.0, 0.0],
            "rotation_xyzw": [0.0, 0.0, 0.0, 1.0],
            "scale": 1.0,
        },
    }
    for vehicle in ("car", "hovercraft", "plane"):
        contexts[vehicle] = {
            "anchor": "seat",
            "translation_m": [0.0, 0.0, 0.0],
            "rotation_xyzw": [0.0, 0.0, 0.0, 1.0],
            "scale": 1.0,
        }
    return {
        "schema": "mdkr-character-source-v4",
        "id": FIXTURE_ID,
        "display_name": FIXTURE_NAME,
        "renderer_profile": "modern-skeletal-v1",
        "license": {
            "spdx": "CC0-1.0",
            "attribution": FIXTURE_ATTRIBUTION,
            "source_url": FIXTURE_SOURCE_URL,
        },
        "identity": {
            "portrait_file": "portrait.png",
            "portrait_sha256": hashlib.sha256(portrait).hexdigest(),
            "minimap_rgb": [214, 76, 166],
            "short_name": "Adversarial",
            "narration_name": "Adversarial Humanoid",
            "sort_label": "Adversarial Humanoid",
        },
        "animations": {
            "fallback": BIND_POSE_FALLBACK,
            "states": {},
        },
        "gameplay": {
            "donor": "diddy",
            "vehicles": ["car", "hovercraft", "plane"],
        },
        "presentation": {
            "source_forward": "+z",
            "target_height_m": 1.25,
            "contexts": contexts,
            "lod_bias": 0.0,
        },
        "sockets": {"seat": "Hip", "head": "Head"},
        "rig": {
            "mode": "humanoid-retarget-v1",
            "reviewed": True,
            "roles": {
                role: {
                    "node": node,
                    "inferred": False,
                    "confidence": 1.0,
                }
                for role, node in zip(role_names, role_nodes)
            },
        },
    }


def write_fixture(directory: Path) -> dict[str, object]:
    directory.mkdir(parents=True, exist_ok=True)
    model = model_glb()
    portrait = portrait_png()
    manifest_bytes = (
        json.dumps(manifest(portrait), indent=2, sort_keys=True) + "\n"
    ).encode("utf-8")
    files = {
        "model.glb": model,
        "portrait.png": portrait,
        "manifest.json": manifest_bytes,
        "LICENSE.txt": FIXTURE_LICENSE.encode("utf-8"),
    }
    for name, payload in files.items():
        destination = directory / name
        destination.write_bytes(payload)
    return {
        "schema": "mdkr-character-spike-fixture-v1",
        "id": FIXTURE_ID,
        "files": {
            name: {
                "bytes": len(payload),
                "sha256": hashlib.sha256(payload).hexdigest(),
            }
            for name, payload in sorted(files.items())
        },
        "properties": {
            "sibling_pelvis_spine": True,
            "nested_centimeter_scale": True,
            "hair_chain_joints": 3,
            "materials": 3,
            "animationless_skin": True,
            "unusual_proportions": True,
        },
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument("--report", type=Path)
    args = parser.parse_args()
    try:
        report = write_fixture(args.output_dir.resolve())
        payload = json.dumps(report, indent=2, sort_keys=True) + "\n"
        if args.report is not None:
            args.report.write_text(payload, encoding="utf-8")
        print(payload, end="")
        return 0
    except (OSError, ValueError) as error:
        print(json.dumps({"ok": False, "error": str(error)}))
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
