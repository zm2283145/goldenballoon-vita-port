#!/usr/bin/env python3
"""Compile a validated .mdkrchar package into the private MDKC runtime cache.

The compiler is intentionally offline. The engine never parses JSON, GLB, DAE,
or FBX during a frame; it maps a bounded, versioned, little-endian cache whose
sections already match the modern renderer's vertex, material, rig, animation,
and character-definition contracts.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import struct
import sys
import zipfile
import zlib
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterable

import character_asset_probe as probe


MDKC_MAGIC = b"MDKC"
MDKC_VERSION = 2
MDKC_HEADER_BYTES = 928
MDKC_SECTION_SLOTS = 27
MDKC_SECTION_ENTRY_BYTES = 32
MDKC_FILE_MAX = 1024 * 1024 * 1024
COMPILER_ID = "mdkr-character-compiler/9"

SECTION_STRINGS = 1
SECTION_VERTICES = 2
SECTION_INDICES = 3
SECTION_PRIMITIVES = 4
SECTION_MATERIALS = 5
SECTION_TEXTURES = 6
SECTION_TEXTURE_DATA = 7
SECTION_NODES = 8
SECTION_SKINS = 9
SECTION_JOINTS = 10
SECTION_ANIMATIONS = 11
SECTION_CHANNELS = 12
SECTION_KEYS = 13
SECTION_CHARACTER = 14
SECTION_SEMANTICS = 15
SECTION_SOCKETS = 16
SECTION_ATTACHMENTS = 17
SECTION_CALIBRATION = 18
SECTION_IDENTITY = 19
SECTION_IDENTITY_DATA = 20
SECTION_RIG = 21
SECTION_RIG_ROLES = 22
SECTION_PROVENANCE = 23
SECTION_IDENTITY_NAMES = 24
SECTION_JOINT_CONSTRAINTS = 25
SECTION_SECONDARY_CHAINS = 26
SECTION_SECONDARY_JOINTS = 27

VERTEX_FORMAT = "<3f3f4f2f4H4f"
PRIMITIVE_FORMAT = "<8I"
MATERIAL_FORMAT = "<II5i4f3f5fI"
TEXTURE_FORMAT = "<IIIIiiiiII"
NODE_FORMAT = "<Ii3f4f3f"
SKIN_FORMAT = "<IIII"
JOINT_FORMAT = "<i16f"
ANIMATION_FORMAT = "<IfII"
CHANNEL_FORMAT = "<IIIIII"
KEY_FORMAT = "<f4f4f4f"
CHARACTER_FORMAT = "<IIII3f3f4ffI"
SEMANTIC_FORMAT = "<IIIf"
SOCKET_FORMAT = "<II"
ATTACHMENT_FORMAT = "<II3f4ffII"
CALIBRATION_FORMAT = "<3f3f3ffII4f"
IDENTITY_FORMAT = "<6I"
IDENTITY_NAMES_FORMAT = "<3I"
RIG_FORMAT = "<4I"
RIG_ROLE_FORMAT = "<4I4f3f"
PROVENANCE_FORMAT = "<4I"
JOINT_CONSTRAINT_FORMAT = "<2I3f3f"
SECONDARY_CHAIN_FORMAT = "<4I7f"
SECONDARY_JOINT_FORMAT = "<4I"

COMPONENTS = {
    5120: ("b", 1, True),
    5121: ("B", 1, False),
    5122: ("h", 2, True),
    5123: ("H", 2, False),
    5125: ("I", 4, False),
    5126: ("f", 4, True),
}
TYPE_COMPONENTS = {
    "SCALAR": 1,
    "VEC2": 2,
    "VEC3": 3,
    "VEC4": 4,
    "MAT4": 16,
}
DONOR_IDS = {
    "krunch": 0,
    "bumper": 1,
    "tiptup": 2,
    "conker": 3,
    "timber": 4,
    "banjo": 5,
    "drumstick": 6,
    "pipsy": 7,
    "tt": 8,
    "diddy": 9,
}
VEHICLE_BITS = {"car": 1, "hovercraft": 2, "plane": 4}
CONTEXT_IDS = {"select": 0, "car": 1, "hovercraft": 2, "plane": 3}
RIG_MODE_IDS = {"authored-clips-only": 0, "humanoid-retarget-v1": 1}
RIG_ROLE_IDS = {name: index for index, name in enumerate(probe.HUMANOID_ROLES)}
RIG_HIERARCHY = (
    ("hips", "spine"), ("spine", "chest"), ("chest", "head"),
    ("chest", "upper_arm.left"),
    ("upper_arm.left", "lower_arm.left"),
    ("lower_arm.left", "hand.left"),
    ("chest", "upper_arm.right"),
    ("upper_arm.right", "lower_arm.right"),
    ("lower_arm.right", "hand.right"),
    ("hips", "upper_leg.left"),
    ("upper_leg.left", "lower_leg.left"),
    ("lower_leg.left", "foot.left"),
    ("hips", "upper_leg.right"),
    ("upper_leg.right", "lower_leg.right"),
    ("lower_leg.right", "foot.right"),
)
SOURCE_FORWARD_IDS = {"+z": 0, "-z": 1, "+x": 2, "-x": 3}
SOURCE_FORWARD_ROTATIONS = {
    "+z": (0.0, 0.0, 0.0, 1.0),
    "-z": (0.0, 1.0, 0.0, 0.0),
    "+x": (0.0, -math.sqrt(0.5), 0.0, math.sqrt(0.5)),
    "-x": (0.0, math.sqrt(0.5), 0.0, math.sqrt(0.5)),
}
PATH_IDS = {"translation": 0, "rotation": 1, "scale": 2, "weights": 3}
INTERPOLATION_IDS = {"LINEAR": 0, "STEP": 1, "CUBICSPLINE": 2}
MIME_IDS = {"image/png": 1, "image/ktx2": 2}
SEMANTIC_DISABLED = 1 << 1
SEMANTIC_MASK_BITS = {
    "fallback": 1 << 0,
    "race.steer": 1 << 1,
    "race.reverse": 1 << 2,
    "race.boost": 1 << 3,
    "race.damage": 1 << 4,
    "race.item": 1 << 5,
    "race.spin": 1 << 6,
    "race.airborne": 1 << 7,
    "race.land": 1 << 8,
    "race.finish_win": 1 << 9,
    "race.finish_lose": 1 << 10,
    "select.idle": 1 << 11,
    "select.hover": 1 << 12,
    "select.confirm": 1 << 13,
}
MAX_TEXTURE_DIMENSION = probe.MAX_TEXTURE_DIMENSION
MAX_DECODED_TEXTURE_BYTES = probe.MAX_DECODED_TEXTURE_BYTES

# Engine-owned semantic behavior. Packages map names to clips; they do not get
# to redefine whether a damage/landing/confirmation reaction loops forever.
ONE_SHOT_SEMANTICS = {
    "race.damage", "race.land", "select.confirm",
}


def _semantic_policy(semantic: str) -> tuple[int, float]:
    return (0 if semantic in ONE_SHOT_SEMANTICS else 1, 0.08 if semantic == "race.steer" else 0.15)


def _is_ancestor(parents: list[int], ancestor: int, descendant: int) -> bool:
    """Return whether ancestor strictly contains descendant, without trusting the graph."""
    current = descendant
    for _ in range(len(parents)):
        current = parents[current]
        if current < 0:
            return False
        if current == ancestor:
            return True
    raise CompileError("node hierarchy contains a cycle")


def _orientation_preserving(
        parents: list[int], scales: list[tuple[float, float, float]],
        node: int) -> bool:
    determinant = 1.0
    for _ in range(len(parents)):
        scale = scales[node]
        determinant *= scale[0] * scale[1] * scale[2]
        if not math.isfinite(determinant) or abs(determinant) < 1.0e-12:
            return False
        node = parents[node]
        if node < 0:
            return determinant > 0.0
    raise CompileError("node hierarchy contains a cycle")


class CompileError(ValueError):
    """A deterministic asset compiler rejection."""


def source_digest(members: Iterable[tuple[str, bytes]],
                  compiler_id: str = COMPILER_ID) -> bytes:
    """Bind a cache to canonical source members and this exact compiler."""
    digest = hashlib.sha256()
    digest.update(compiler_id.encode("ascii") + b"\0")
    for name, payload in members:
        digest.update(name.encode("ascii") + b"\0")
        digest.update(struct.pack("<Q", len(payload)))
        digest.update(payload)
    return digest.digest()


@dataclass(frozen=True)
class Section:
    kind: int
    count: int
    stride: int
    data: bytes
    flags: int = 0


class StringTable:
    def __init__(self) -> None:
        self.data = bytearray(b"\0")
        self.offsets = {"": 0}

    def add(self, value: Any) -> int:
        if not isinstance(value, str):
            raise CompileError("compiled strings must be text")
        text = value
        if "\0" in text:
            raise CompileError("strings must not contain NUL bytes")
        encoded = text.encode("utf-8")
        if len(encoded) > 4096:
            raise CompileError("a compiled string exceeds 4096 UTF-8 bytes")
        existing = self.offsets.get(text)
        if existing is not None:
            return existing
        offset = len(self.data)
        self.data.extend(encoded)
        self.data.append(0)
        self.offsets[text] = offset
        return offset


def _array(document: dict[str, Any], key: str) -> list[Any]:
    value = document.get(key, [])
    if not isinstance(value, list):
        raise CompileError(f"glTF {key} must be an array")
    return value


def _item(items: list[Any], index: Any, label: str) -> dict[str, Any]:
    if (
        not isinstance(index, int) or isinstance(index, bool)
        or index < 0 or index >= len(items)
    ):
        raise CompileError(f"invalid {label} index {index!r}")
    value = items[index]
    if not isinstance(value, dict):
        raise CompileError(f"{label}[{index}] must be an object")
    return value


class AccessorReader:
    def __init__(self, document: dict[str, Any], binary: bytes) -> None:
        self.document = document
        self.binary = binary
        buffers = _array(document, "buffers")
        if (
            len(buffers) != 1 or not isinstance(buffers[0], dict)
            or buffers[0].get("uri") is not None
        ):
            raise CompileError("compiler requires exactly one embedded GLB buffer")
        declared = buffers[0].get("byteLength")
        if (
            not isinstance(declared, int) or isinstance(declared, bool)
            or declared <= 0 or declared > len(binary)
        ):
            raise CompileError("GLB buffer byteLength exceeds the BIN chunk")
        self.declared_bytes = declared

    def values(self, index: Any, *, as_float: bool = True) -> list[tuple[Any, ...]]:
        accessor = _item(_array(self.document, "accessors"), index, "accessor")
        if "sparse" in accessor:
            raise CompileError(f"accessor[{index}] uses sparse storage; normalize it before import")
        component_type = accessor.get("componentType")
        value_type = accessor.get("type")
        count = accessor.get("count")
        if (
            not isinstance(component_type, int)
            or isinstance(component_type, bool)
            or component_type not in COMPONENTS
            or not isinstance(value_type, str)
            or value_type not in TYPE_COMPONENTS
        ):
            raise CompileError(f"accessor[{index}] has an unsupported component/type")
        if not isinstance(count, int) or isinstance(count, bool) or count <= 0:
            raise CompileError(f"accessor[{index}] has an invalid count")
        code, component_bytes, signed = COMPONENTS[component_type]
        components = TYPE_COMPONENTS[value_type]
        element_bytes = component_bytes * components
        view_index = accessor.get("bufferView")
        if view_index is None:
            raise CompileError(
                f"accessor[{index}] has no bufferView; normalize it before import"
            )
        view = _item(_array(self.document, "bufferViews"), view_index, "bufferView")
        view_buffer = view.get("buffer")
        if (
            not isinstance(view_buffer, int) or isinstance(view_buffer, bool)
            or view_buffer != 0
        ):
            raise CompileError(f"accessor[{index}] references a non-GLB buffer")
        view_offset = view.get("byteOffset", 0)
        accessor_offset = accessor.get("byteOffset", 0)
        view_length = view.get("byteLength")
        stride = view.get("byteStride", element_bytes)
        if not all(
            isinstance(value, int) and not isinstance(value, bool) and value >= 0
            for value in (view_offset, accessor_offset, view_length)
        ) or view_length == 0:
            raise CompileError(f"accessor[{index}] has invalid byte bounds")
        if (
            not isinstance(stride, int) or isinstance(stride, bool)
            or stride < element_bytes or stride > 252
            or stride % component_bytes != 0
            or ("byteStride" in view and stride % 4 != 0)
        ):
            raise CompileError(f"accessor[{index}] has an invalid byteStride")
        if (
            accessor_offset % component_bytes != 0
            or (view_offset + accessor_offset) % component_bytes != 0
        ):
            raise CompileError(f"accessor[{index}] is not component-size aligned")
        if view_offset + view_length > self.declared_bytes:
            raise CompileError(f"accessor[{index}] bufferView exceeds the declared buffer")
        if accessor_offset + (count - 1) * stride + element_bytes > view_length:
            raise CompileError(f"accessor[{index}] exceeds its bufferView")
        base = view_offset + accessor_offset
        if base + (count - 1) * stride + element_bytes > self.declared_bytes:
            raise CompileError(f"accessor[{index}] exceeds the GLB BIN chunk")
        normalized = accessor.get("normalized", False)
        if not isinstance(normalized, bool) or (
            normalized and component_type == 5126
        ):
            raise CompileError(f"accessor[{index}] has an invalid normalized flag")
        unpack = struct.Struct("<" + code * components)
        output: list[tuple[Any, ...]] = []
        for item_index in range(count):
            raw = unpack.unpack_from(self.binary, base + item_index * stride)
            if as_float and component_type != 5126:
                if normalized:
                    bits = component_bytes * 8
                    if signed:
                        maximum = float((1 << (bits - 1)) - 1)
                        converted = tuple(max(float(value) / maximum, -1.0) for value in raw)
                    else:
                        maximum = float((1 << bits) - 1)
                        converted = tuple(float(value) / maximum for value in raw)
                else:
                    converted = tuple(float(value) for value in raw)
                output.append(converted)
            elif as_float:
                output.append(tuple(float(value) for value in raw))
            else:
                output.append(tuple(int(value) for value in raw))
        return output


def _finite(values: Iterable[float], label: str) -> tuple[float, ...]:
    if isinstance(values, (str, bytes, dict)):
        raise CompileError(f"{label} must be a numeric array")
    try:
        raw = tuple(values)
    except TypeError as exc:
        raise CompileError(f"{label} must be a numeric array") from exc
    if any(
        not isinstance(value, (int, float)) or isinstance(value, bool)
        for value in raw
    ):
        raise CompileError(f"{label} must contain numbers")
    try:
        result = tuple(float(value) for value in raw)
    except OverflowError as exc:
        raise CompileError(f"{label} contains a number outside FLOAT range") from exc
    if not all(math.isfinite(value) for value in result):
        raise CompileError(f"{label} contains a non-finite value")
    return result


def _finite_scalar(value: Any, label: str) -> float:
    if not isinstance(value, (int, float)) or isinstance(value, bool):
        raise CompileError(f"{label} must be a finite number")
    try:
        result = float(value)
    except OverflowError as exc:
        raise CompileError(f"{label} is outside FLOAT range") from exc
    if not math.isfinite(result):
        raise CompileError(f"{label} must be a finite number")
    return result


def _source_name(value: Any, fallback: str, label: str) -> str:
    if value is None or value == "":
        return fallback
    if not isinstance(value, str):
        raise CompileError(f"{label} name must be a string")
    return value


def _normalize3(value: Iterable[float], fallback: tuple[float, float, float]) -> tuple[float, float, float]:
    finite = _finite(value, "vector")
    if len(finite) != 3:
        raise CompileError("vector must contain three components")
    x, y, z = finite
    length = math.sqrt(x * x + y * y + z * z)
    if length < 1.0e-12:
        return fallback
    return x / length, y / length, z / length


def _orthogonal_tangent(
    normal: tuple[float, float, float],
) -> tuple[float, float, float]:
    """Return one deterministic unit vector perpendicular to ``normal``.

    Choosing a constant fallback is incorrect when that axis is parallel to the
    normal. Select the least-aligned cardinal axis, project it onto the tangent
    plane, and normalize. The input is already normalized and finite.
    """
    axis = min(range(3), key=lambda index: abs(normal[index]))
    candidate = [0.0, 0.0, 0.0]
    candidate[axis] = 1.0
    projection = sum(normal[index] * candidate[index] for index in range(3))
    tangent = tuple(
        candidate[index] - normal[index] * projection for index in range(3)
    )
    return _normalize3(tangent, (0.0, 0.0, 1.0))


def _sanitize_tangent(
    normal: tuple[float, float, float], value: Iterable[float],
) -> tuple[tuple[float, float, float, float], bool, bool]:
    tangent = _finite(value, "TANGENT")
    if len(tangent) != 4 or abs(abs(tangent[3]) - 1.0) > 1.0e-6:
        raise CompileError("TANGENT must contain XYZ plus handedness -1 or 1")
    projection = sum(normal[axis] * tangent[axis] for axis in range(3))
    orthogonal = tuple(
        tangent[axis] - normal[axis] * projection for axis in range(3)
    )
    length_squared = sum(component * component for component in orthogonal)
    fallback = length_squared < 1.0e-20
    repaired_xyz = (
        _orthogonal_tangent(normal)
        if fallback else _normalize3(orthogonal, _orthogonal_tangent(normal))
    )
    result = (*repaired_xyz, -1.0 if tangent[3] < 0.0 else 1.0)
    repaired = any(
        abs(result[index] - tangent[index]) > 1.0e-5 for index in range(4)
    )
    return result, repaired, fallback


def _tangents(
    positions: list[tuple[Any, ...]],
    normals: list[tuple[Any, ...]],
    uvs: list[tuple[Any, ...]],
    indices: list[int],
) -> tuple[list[tuple[float, float, float, float]], int, int]:
    tan1 = [[0.0, 0.0, 0.0] for _ in positions]
    tan2 = [[0.0, 0.0, 0.0] for _ in positions]
    degenerate_uv_triangles = 0
    for tri in range(0, len(indices), 3):
        a, b, c = indices[tri:tri + 3]
        if min(a, b, c) < 0 or max(a, b, c) >= len(positions):
            raise CompileError("primitive index exceeds POSITION accessor")
        p0, p1, p2 = positions[a], positions[b], positions[c]
        w0, w1, w2 = uvs[a], uvs[b], uvs[c]
        x1, x2 = p1[0] - p0[0], p2[0] - p0[0]
        y1, y2 = p1[1] - p0[1], p2[1] - p0[1]
        z1, z2 = p1[2] - p0[2], p2[2] - p0[2]
        s1, s2 = w1[0] - w0[0], w2[0] - w0[0]
        t1, t2 = w1[1] - w0[1], w2[1] - w0[1]
        denominator = s1 * t2 - s2 * t1
        if abs(denominator) < 1.0e-20:
            degenerate_uv_triangles += 1
            continue
        reciprocal = 1.0 / denominator
        sdir = ((t2 * x1 - t1 * x2) * reciprocal,
                (t2 * y1 - t1 * y2) * reciprocal,
                (t2 * z1 - t1 * z2) * reciprocal)
        tdir = ((s1 * x2 - s2 * x1) * reciprocal,
                (s1 * y2 - s2 * y1) * reciprocal,
                (s1 * z2 - s2 * z1) * reciprocal)
        for vertex in (a, b, c):
            for axis in range(3):
                tan1[vertex][axis] += sdir[axis]
                tan2[vertex][axis] += tdir[axis]
    output = []
    fallback_vertices = 0
    for index, normal_value in enumerate(normals):
        normal = _normalize3(normal_value, (0.0, 1.0, 0.0))
        tangent = tan1[index]
        projection = sum(normal[axis] * tangent[axis] for axis in range(3))
        ortho = tuple(tangent[axis] - normal[axis] * projection for axis in range(3))
        if sum(component * component for component in ortho) < 1.0e-20:
            tangent3 = _orthogonal_tangent(normal)
            fallback_vertices += 1
        else:
            tangent3 = _normalize3(ortho, _orthogonal_tangent(normal))
        cross = (
            normal[1] * tangent3[2] - normal[2] * tangent3[1],
            normal[2] * tangent3[0] - normal[0] * tangent3[2],
            normal[0] * tangent3[1] - normal[1] * tangent3[0],
        )
        handedness = -1.0 if sum(cross[axis] * tan2[index][axis] for axis in range(3)) < 0.0 else 1.0
        output.append((*tangent3, handedness))
    return output, degenerate_uv_triangles, fallback_vertices


def _quaternion_from_rotation(matrix: list[list[float]]) -> tuple[float, float, float, float]:
    trace = matrix[0][0] + matrix[1][1] + matrix[2][2]
    if trace > 0.0:
        root = math.sqrt(trace + 1.0) * 2.0
        quat = ((matrix[2][1] - matrix[1][2]) / root,
                (matrix[0][2] - matrix[2][0]) / root,
                (matrix[1][0] - matrix[0][1]) / root, 0.25 * root)
    elif matrix[0][0] > matrix[1][1] and matrix[0][0] > matrix[2][2]:
        root = math.sqrt(1.0 + matrix[0][0] - matrix[1][1] - matrix[2][2]) * 2.0
        quat = (0.25 * root, (matrix[0][1] + matrix[1][0]) / root,
                (matrix[0][2] + matrix[2][0]) / root,
                (matrix[2][1] - matrix[1][2]) / root)
    elif matrix[1][1] > matrix[2][2]:
        root = math.sqrt(1.0 + matrix[1][1] - matrix[0][0] - matrix[2][2]) * 2.0
        quat = ((matrix[0][1] + matrix[1][0]) / root, 0.25 * root,
                (matrix[1][2] + matrix[2][1]) / root,
                (matrix[0][2] - matrix[2][0]) / root)
    else:
        root = math.sqrt(1.0 + matrix[2][2] - matrix[0][0] - matrix[1][1]) * 2.0
        quat = ((matrix[0][2] + matrix[2][0]) / root,
                (matrix[1][2] + matrix[2][1]) / root, 0.25 * root,
                (matrix[1][0] - matrix[0][1]) / root)
    length = math.sqrt(sum(component * component for component in quat))
    return tuple(component / length for component in quat)  # type: ignore[return-value]


def _node_trs(node: dict[str, Any]) -> tuple[tuple[float, ...], tuple[float, ...], tuple[float, ...]]:
    if "matrix" not in node:
        translation = _finite(node.get("translation", (0.0, 0.0, 0.0)), "node translation")
        rotation = _finite(node.get("rotation", (0.0, 0.0, 0.0, 1.0)), "node rotation")
        scale = _finite(node.get("scale", (1.0, 1.0, 1.0)), "node scale")
        if len(translation) != 3 or len(rotation) != 4 or len(scale) != 3:
            raise CompileError("node TRS has an invalid component count")
        qlen = math.sqrt(sum(component * component for component in rotation))
        if qlen < 1.0e-12:
            raise CompileError("node rotation quaternion has zero length")
        return translation, tuple(component / qlen for component in rotation), scale
    values = _finite(node["matrix"], "node matrix")
    if len(values) != 16:
        raise CompileError("node matrix must contain 16 values")
    translation = values[12], values[13], values[14]
    columns = [
        [values[0], values[1], values[2]],
        [values[4], values[5], values[6]],
        [values[8], values[9], values[10]],
    ]
    scale = [math.sqrt(sum(component * component for component in column)) for column in columns]
    if min(scale) < 1.0e-12:
        raise CompileError("node matrix has a singular scale")
    determinant = (
        columns[0][0] * (columns[1][1] * columns[2][2] - columns[1][2] * columns[2][1])
        - columns[1][0] * (columns[0][1] * columns[2][2] - columns[0][2] * columns[2][1])
        + columns[2][0] * (columns[0][1] * columns[1][2] - columns[0][2] * columns[1][1])
    )
    if determinant < 0.0:
        scale[0] = -scale[0]
    rotation_matrix = [[columns[column][row] / scale[column] for column in range(3)] for row in range(3)]
    rotation = _quaternion_from_rotation(rotation_matrix)
    return translation, rotation, tuple(scale)


def _pack_records(format_string: str, records: Iterable[tuple[Any, ...]]) -> bytes:
    packer = struct.Struct(format_string)
    output = bytearray()
    for record in records:
        output.extend(packer.pack(*record))
    return bytes(output)


def _png_dimensions(payload: bytes, image_index: int) -> tuple[int, int]:
    if (len(payload) < 24 or not payload.startswith(b"\x89PNG\r\n\x1a\n") or
            payload[12:16] != b"IHDR"):
        raise CompileError(f"image[{image_index}] is not a bounded PNG payload")
    width, height = struct.unpack_from(">II", payload, 16)
    if (width == 0 or height == 0 or width > MAX_TEXTURE_DIMENSION or
            height > MAX_TEXTURE_DIMENSION):
        raise CompileError(
            f"image[{image_index}] dimensions {width}x{height} exceed the "
            f"{MAX_TEXTURE_DIMENSION}x{MAX_TEXTURE_DIMENSION} v1 limit"
        )
    return width, height


def _rgba_mip_bytes(width: int, height: int) -> int:
    total = 0
    while True:
        total += width * height * 4
        if width == 1 and height == 1:
            return total
        width = max(1, width // 2)
        height = max(1, height // 2)


def _image_bytes(document: dict[str, Any], binary: bytes,
                 image_index: int) -> tuple[bytes, int, int, int]:
    image = _item(_array(document, "images"), image_index, "image")
    if "uri" in image:
        raise CompileError(f"image[{image_index}] is external")
    mime = image.get("mimeType")
    if not isinstance(mime, str) or mime not in MIME_IDS:
        raise CompileError(f"image[{image_index}] has unsupported MIME type {mime!r}")
    view = _item(_array(document, "bufferViews"), image.get("bufferView"), "bufferView")
    offset = view.get("byteOffset", 0)
    size = view.get("byteLength")
    if (not isinstance(offset, int) or isinstance(offset, bool)
            or not isinstance(size, int) or isinstance(size, bool)
            or offset < 0 or size <= 0 or offset + size > len(binary)):
        raise CompileError(f"image[{image_index}] exceeds the GLB BIN chunk")
    payload = binary[offset:offset + size]
    if mime == "image/png":
        width, height = _png_dimensions(payload, image_index)
    else:
        inspected = probe._inspect_texture_ktx2(payload, image_index)
        width = int(inspected["width"])
        height = int(inspected["height"])
    return payload, MIME_IDS[mime], width, height


def _texture_source(texture: dict[str, Any]) -> int:
    extensions = texture.get("extensions", {})
    if not isinstance(extensions, dict):
        raise CompileError("texture extensions must be an object")
    basis = extensions.get("KHR_texture_basisu")
    if "KHR_texture_basisu" in extensions and not isinstance(basis, dict):
        raise CompileError("KHR_texture_basisu must be an object")
    if isinstance(basis, dict):
        source = basis.get("source")
        if not isinstance(source, int) or isinstance(source, bool):
            raise CompileError("KHR_texture_basisu does not name an image source")
        return source
    source = texture.get("source")
    if not isinstance(source, int) or isinstance(source, bool):
        raise CompileError("texture does not name an image source")
    return source


def _material_texture(info: Any) -> int:
    if info is None:
        return -1
    if (not isinstance(info, dict)
            or not isinstance(info.get("index"), int)
            or isinstance(info.get("index"), bool)):
        raise CompileError("material texture reference is invalid")
    tex_coord = info.get("texCoord", 0)
    if not isinstance(tex_coord, int) or isinstance(tex_coord, bool) or tex_coord != 0:
        raise CompileError("modern-skeletal-v1 supports TEXCOORD_0 only")
    return info["index"]


def _align(output: bytearray, alignment: int = 16) -> None:
    output.extend(b"\0" * ((-len(output)) % alignment))


def _assemble(sections: list[Section], source_digest: bytes) -> bytes:
    if len(sections) > MDKC_SECTION_SLOTS:
        raise CompileError("compiled cache has too many sections")
    output = bytearray(b"\0" * MDKC_HEADER_BYTES)
    table: list[tuple[Section, int]] = []
    for section in sections:
        _align(output)
        offset = len(output)
        output.extend(section.data)
        table.append((section, offset))
    if len(output) > MDKC_FILE_MAX:
        raise CompileError("compiled cache exceeds the 1 GiB hard cap")
    payload_crc = zlib.crc32(output[MDKC_HEADER_BYTES:]) & 0xFFFFFFFF
    struct.pack_into(
        "<4sIIQ32sIII",
        output,
        0,
        MDKC_MAGIC,
        MDKC_VERSION,
        MDKC_HEADER_BYTES,
        len(output),
        source_digest,
        payload_crc,
        len(table),
        0,
    )
    for index, (section, offset) in enumerate(table):
        struct.pack_into(
            "<IIQQII",
            output,
            64 + index * MDKC_SECTION_ENTRY_BYTES,
            section.kind,
            section.flags,
            offset,
            len(section.data),
            section.count,
            section.stride,
        )
    return bytes(output)


def compile_character(model: bytes, manifest: dict[str, Any], source_digest: bytes,
                      portrait: bytes | None = None) -> tuple[bytes, dict[str, Any]]:
    policy = probe.inspect_glb_bytes(model, require_character=True)
    errors = list(policy["errors"])
    errors.extend(probe.validate_manifest(manifest, policy))
    if errors:
        raise CompileError("source package failed policy: " + "; ".join(errors))
    document, binary_chunk = probe.parse_glb(model)
    binary = binary_chunk or b""
    reader = AccessorReader(document, binary)
    strings = StringTable()

    nodes = _array(document, "nodes")
    parents = [-1] * len(nodes)
    node_lods = [0] * len(nodes)
    lod_assigned: set[int] = set()
    for parent_index, node in enumerate(nodes):
        if not isinstance(node, dict):
            raise CompileError(f"node[{parent_index}] must be an object")
        children = node.get("children", [])
        if not isinstance(children, list):
            raise CompileError(f"node[{parent_index}] children must be an array")
        for child in children:
            if (not isinstance(child, int) or isinstance(child, bool)
                    or child < 0 or child >= len(nodes)):
                raise CompileError(f"node[{parent_index}] has an invalid child")
            if parents[child] != -1:
                raise CompileError(f"node[{child}] has multiple parents")
            parents[child] = parent_index
        extensions = node.get("extensions", {})
        lod_extension = extensions.get("MSFT_lod") if isinstance(extensions, dict) else None
        if lod_extension is not None:
            ids = lod_extension.get("ids") if isinstance(lod_extension, dict) else None
            if not isinstance(ids, list) or not ids or len(ids) > 3:
                raise CompileError(f"node[{parent_index}] has an invalid MSFT_lod chain")
            for level, lod_node in enumerate(ids, start=1):
                if (
                    not isinstance(lod_node, int)
                    or isinstance(lod_node, bool)
                    or lod_node < 0
                    or lod_node >= len(nodes)
                    or lod_node == parent_index
                    or lod_node in lod_assigned
                ):
                    raise CompileError(f"node[{parent_index}] MSFT_lod target is invalid or reused")
                node_lods[lod_node] = level
                lod_assigned.add(lod_node)
    node_records = []
    node_scales: list[tuple[float, ...]] = []
    node_names: dict[str, int] = {}
    for index, node in enumerate(nodes):
        name = _source_name(node.get("name"), f"node_{index}", f"node[{index}]")
        if name in node_names:
            raise CompileError(f"duplicate node name {name!r}")
        node_names[name] = index
        translation, rotation, scale = _node_trs(node)
        node_scales.append(tuple(scale))
        node_records.append((strings.add(name), parents[index], *translation, *rotation, *scale))

    vertex_records: list[tuple[Any, ...]] = []
    indices_output: list[int] = []
    mesh_primitive_records: dict[int, list[tuple[int, int, int, int]]] = {}
    authored_tangent_primitives = 0
    generated_tangent_primitives = 0
    authored_tangent_repaired_vertices = 0
    generated_tangent_degenerate_uv_triangles = 0
    tangent_fallback_vertices = 0
    tangent_fallback_vertices_by_material: dict[int, int] = {}
    meshes = _array(document, "meshes")
    for mesh_index, mesh in enumerate(meshes):
        if not isinstance(mesh, dict):
            raise CompileError(f"mesh[{mesh_index}] must be an object")
        built = []
        for primitive_index, primitive in enumerate(mesh.get("primitives", [])):
            if not isinstance(primitive, dict) or primitive.get("mode", 4) != 4:
                raise CompileError(f"mesh[{mesh_index}] primitive[{primitive_index}] is not triangles")
            if primitive.get("targets"):
                raise CompileError("morph target geometry is not yet supported by cache version 1")
            attributes = primitive.get("attributes", {})
            if not isinstance(attributes, dict):
                raise CompileError("primitive attributes must be an object")
            positions = reader.values(attributes.get("POSITION"))
            normals = reader.values(attributes.get("NORMAL"))
            uvs = reader.values(attributes.get("TEXCOORD_0"))
            if not positions or len(normals) != len(positions) or len(uvs) != len(positions):
                raise CompileError("POSITION, NORMAL, and TEXCOORD_0 counts must match and be nonzero")
            local_indices = [value[0] for value in reader.values(primitive.get("indices"), as_float=False)]
            if not local_indices or len(local_indices) % 3:
                raise CompileError("primitive index count must be nonzero and divisible by three")
            tangent_index = attributes.get("TANGENT")
            generated_fallback_vertices = 0
            if tangent_index is not None:
                authored_tangent_primitives += 1
                tangents = reader.values(tangent_index)
            else:
                generated_tangent_primitives += 1
                (tangents,
                 degenerate_uv_triangles,
                 generated_fallback_vertices) = _tangents(
                    positions, normals, uvs, local_indices
                )
                generated_tangent_degenerate_uv_triangles += (
                    degenerate_uv_triangles
                )
                tangent_fallback_vertices += generated_fallback_vertices
            joints = reader.values(attributes.get("JOINTS_0"), as_float=False) if "JOINTS_0" in attributes else [(0, 0, 0, 0)] * len(positions)
            weights = reader.values(attributes.get("WEIGHTS_0")) if "WEIGHTS_0" in attributes else [(1.0, 0.0, 0.0, 0.0)] * len(positions)
            if not all(len(values) == len(positions) for values in (tangents, joints, weights)):
                raise CompileError("primitive vertex attribute counts do not match")
            first_vertex = len(vertex_records)
            authored_fallback_vertices = 0
            for vertex_index in range(len(positions)):
                position = _finite(positions[vertex_index], "POSITION")
                normal = _normalize3(normals[vertex_index], (0.0, 1.0, 0.0))
                tangent, tangent_repaired, tangent_fallback = _sanitize_tangent(
                    normal, tangents[vertex_index]
                )
                if tangent_index is not None and tangent_repaired:
                    authored_tangent_repaired_vertices += 1
                if tangent_index is not None and tangent_fallback:
                    authored_fallback_vertices += 1
                uv = _finite(uvs[vertex_index], "TEXCOORD_0")
                joint = tuple(int(value) for value in joints[vertex_index])
                if len(position) != 3 or len(tangent) != 4 or len(uv) != 2 or len(joint) != 4:
                    raise CompileError("primitive vertex attribute has the wrong type")
                if min(joint) < 0 or max(joint) > 65535:
                    raise CompileError("JOINTS_0 value exceeds uint16")
                weight = list(_finite(weights[vertex_index], "WEIGHTS_0"))
                if len(weight) != 4 or min(weight) < 0.0:
                    raise CompileError("WEIGHTS_0 must contain four non-negative values")
                weight_sum = sum(weight)
                if weight_sum < 1.0e-12:
                    weight = [1.0, 0.0, 0.0, 0.0]
                else:
                    weight = [value / weight_sum for value in weight]
                vertex_records.append((*position, *normal, *tangent, *uv, *joint, *weight))
            if tangent_index is not None:
                tangent_fallback_vertices += authored_fallback_vertices
            first_index = len(indices_output)
            for local_index in local_indices:
                if local_index < 0 or local_index >= len(positions):
                    raise CompileError("primitive index exceeds its vertex count")
                indices_output.append(first_vertex + local_index)
            material = primitive.get("material", -1)
            if not isinstance(material, int) or isinstance(material, bool):
                raise CompileError("primitive material index is invalid")
            fallback_count = (
                generated_fallback_vertices + authored_fallback_vertices
            )
            if fallback_count != 0:
                tangent_fallback_vertices_by_material[material] = (
                    tangent_fallback_vertices_by_material.get(material, 0) +
                    fallback_count
                )
            built.append((first_vertex, len(positions), first_index, len(local_indices), material))
        mesh_primitive_records[mesh_index] = built

    primitive_records = []
    for node_index, node in enumerate(nodes):
        if "mesh" not in node:
            continue
        mesh_index = node.get("mesh")
        if (not isinstance(mesh_index, int) or isinstance(mesh_index, bool)
                or mesh_index not in mesh_primitive_records):
            raise CompileError(f"node[{node_index}] has an invalid mesh")
        skin = node.get("skin", -1)
        if not isinstance(skin, int) or isinstance(skin, bool):
            raise CompileError(f"node[{node_index}] has an invalid skin")
        for first_vertex, vertex_count, first_index, index_count, material in mesh_primitive_records[mesh_index]:
            primitive_records.append((first_vertex, vertex_count, first_index, index_count,
                                      material if material >= 0 else 0xFFFFFFFF,
                                      node_index, skin if skin >= 0 else 0xFFFFFFFF,
                                      node_lods[node_index]))
    if not primitive_records:
        raise CompileError("no scene node instantiates a mesh")

    textures_source = _array(document, "textures")
    materials = _array(document, "materials")
    if not materials:
        materials = [{}]
    texture_roles = [0] * len(textures_source)  # 1 sRGB, 2 linear data, 4 normal
    texture_cutoffs: list[float | None] = [None] * len(textures_source)
    normal_map_tangent_fallback_vertices = 0
    for material_index, material in enumerate(materials):
        if not isinstance(material, dict):
            raise CompileError(f"material[{material_index}] must be an object")
        pbr = material.get("pbrMetallicRoughness", {})
        if not isinstance(pbr, dict):
            raise CompileError("pbrMetallicRoughness must be an object")
        uses = (
            (_material_texture(pbr.get("baseColorTexture")), 1),
            (_material_texture(pbr.get("metallicRoughnessTexture")), 2),
            (_material_texture(material.get("normalTexture")), 4),
            (_material_texture(material.get("occlusionTexture")), 2),
            (_material_texture(material.get("emissiveTexture")), 1),
        )
        if uses[2][0] >= 0:
            normal_map_tangent_fallback_vertices += (
                tangent_fallback_vertices_by_material.get(material_index, 0)
            )
        for texture_index, role in uses:
            if texture_index < 0:
                continue
            if texture_index >= len(texture_roles):
                raise CompileError(f"material[{material_index}] texture index is out of range")
            if texture_roles[texture_index] not in (0, role):
                raise CompileError(
                    f"texture[{texture_index}] is reused across incompatible color/data roles"
                )
            texture_roles[texture_index] = role
        if material.get("alphaMode", "OPAQUE") == "MASK":
            base_texture = uses[0][0]
            cutoff = _finite_scalar(
                material.get("alphaCutoff", 0.5),
                f"material[{material_index}] alphaCutoff",
            )
            if not 0.0 <= cutoff <= 1.0:
                raise CompileError(f"material[{material_index}] has an invalid alphaCutoff")
            if base_texture >= 0:
                previous = texture_cutoffs[base_texture]
                if previous is not None and abs(previous - cutoff) > (0.5 / 255.0):
                    raise CompileError(
                        f"texture[{base_texture}] is reused with conflicting alpha cutoffs"
                    )
                texture_cutoffs[base_texture] = cutoff

    texture_data = bytearray()
    texture_records = []
    decoded_texture_bytes = 0
    ktx2_texture_count = 0
    ktx2_source_bytes = 0
    ktx2_etc1s_count = 0
    ktx2_uastc_count = 0
    ktx2_mip_levels: list[int] = []
    samplers = _array(document, "samplers")
    for texture_index, texture in enumerate(textures_source):
        if not isinstance(texture, dict):
            raise CompileError(f"texture[{texture_index}] must be an object")
        image_index = _texture_source(texture)
        payload, mime_id, width, height = _image_bytes(
            document, binary, image_index
        )
        if mime_id == MIME_IDS["image/ktx2"]:
            ktx2 = probe._inspect_texture_ktx2(payload, image_index)
            role = texture_roles[texture_index] or 1
            expects_srgb = role == 1
            if bool(ktx2["srgb"]) != expects_srgb:
                semantic = "sRGB color/emissive" if expects_srgb else "linear data/normal"
                encoded = "sRGB" if ktx2["srgb"] else "linear"
                raise CompileError(
                    f"texture[{texture_index}] uses {encoded} KTX2 transfer "
                    f"metadata but its material role requires {semantic} data"
                )
            ktx2_texture_count += 1
            ktx2_source_bytes += len(payload)
            if ktx2["uastc"]:
                ktx2_uastc_count += 1
            else:
                ktx2_etc1s_count += 1
            ktx2_mip_levels.append(int(ktx2["levels"]))
            decoded_texture_bytes += int(ktx2["decoded_bytes"])
        else:
            decoded_texture_bytes += _rgba_mip_bytes(width, height)
        if decoded_texture_bytes > MAX_DECODED_TEXTURE_BYTES:
            raise CompileError("decoded RGBA character textures exceed the 512 MiB v1 budget")
        data_offset = len(texture_data)
        texture_data.extend(payload)
        sampler_index = texture.get("sampler")
        sampler = _item(samplers, sampler_index, "sampler") if sampler_index is not None else {}
        sampler_values = {
            "wrapS": (sampler.get("wrapS", 10497), {33071, 33648, 10497}),
            "wrapT": (sampler.get("wrapT", 10497), {33071, 33648, 10497}),
            "minFilter": (
                sampler.get("minFilter", 9987),
                {9728, 9729, 9984, 9985, 9986, 9987},
            ),
            "magFilter": (sampler.get("magFilter", 9729), {9728, 9729}),
        }
        for field, (value, choices) in sampler_values.items():
            if (not isinstance(value, int) or isinstance(value, bool)
                    or value not in choices):
                raise CompileError(
                    f"sampler for texture[{texture_index}] has invalid {field}"
                )
        texture_records.append((
            strings.add(_source_name(
                texture.get("name"), f"texture_{texture_index}",
                f"texture[{texture_index}]",
            )), mime_id,
            data_offset, len(payload), sampler_values["wrapS"][0],
            sampler_values["wrapT"][0], sampler_values["minFilter"][0],
            sampler_values["magFilter"][0],
            texture_roles[texture_index] or 1, width | (height << 16),
        ))

    material_records = []
    for material_index, material in enumerate(materials):
        if not isinstance(material, dict):
            raise CompileError(f"material[{material_index}] must be an object")
        pbr = material.get("pbrMetallicRoughness", {})
        if not isinstance(pbr, dict):
            raise CompileError("pbrMetallicRoughness must be an object")
        alpha_mode = material.get("alphaMode", "OPAQUE")
        if alpha_mode not in ("OPAQUE", "MASK", "BLEND"):
            raise CompileError("unsupported material alphaMode")
        flags = {"OPAQUE": 0, "MASK": 1, "BLEND": 2}[alpha_mode]
        double_sided = material.get("doubleSided", False)
        if not isinstance(double_sided, bool):
            raise CompileError("material doubleSided must be Boolean")
        if double_sided:
            flags |= 4
        base_color = _finite(pbr.get("baseColorFactor", (1.0, 1.0, 1.0, 1.0)), "baseColorFactor")
        emissive = _finite(material.get("emissiveFactor", (0.0, 0.0, 0.0)), "emissiveFactor")
        if len(base_color) != 4 or len(emissive) != 3:
            raise CompileError("material factor has an invalid component count")
        normal_info = material.get("normalTexture")
        occlusion_info = material.get("occlusionTexture")
        material_records.append((
            strings.add(_source_name(
                material.get("name"), f"material_{material_index}",
                f"material[{material_index}]",
            )), flags,
            _material_texture(pbr.get("baseColorTexture")),
            _material_texture(pbr.get("metallicRoughnessTexture")),
            _material_texture(normal_info),
            _material_texture(occlusion_info),
            _material_texture(material.get("emissiveTexture")),
            *base_color, *emissive,
            _finite_scalar(pbr.get("metallicFactor", 1.0), "metallicFactor"),
            _finite_scalar(pbr.get("roughnessFactor", 1.0), "roughnessFactor"),
            _finite_scalar(normal_info.get("scale", 1.0), "normal scale")
                if isinstance(normal_info, dict) else 1.0,
            _finite_scalar(
                occlusion_info.get("strength", 1.0), "occlusion strength"
            ) if isinstance(occlusion_info, dict) else 1.0,
            _finite_scalar(material.get("alphaCutoff", 0.5), "alphaCutoff"), 0,
        ))

    joint_records = []
    skin_records = []
    joint_node_set: set[int] = set()
    for skin_index, skin in enumerate(_array(document, "skins")):
        if not isinstance(skin, dict):
            raise CompileError(f"skin[{skin_index}] must be an object")
        joint_nodes = skin.get("joints", [])
        if not isinstance(joint_nodes, list) or not joint_nodes:
            raise CompileError(f"skin[{skin_index}] has no joints")
        inverse_accessor = skin.get("inverseBindMatrices")
        inverse_values = reader.values(inverse_accessor) if inverse_accessor is not None else [
            (1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0,
             0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0)
        ] * len(joint_nodes)
        if len(inverse_values) != len(joint_nodes):
            raise CompileError(f"skin[{skin_index}] inverse-bind count does not match joints")
        first_joint = len(joint_records)
        for joint_node, inverse in zip(joint_nodes, inverse_values):
            if (not isinstance(joint_node, int) or isinstance(joint_node, bool)
                    or joint_node < 0 or joint_node >= len(nodes)):
                raise CompileError(f"skin[{skin_index}] contains an invalid joint node")
            joint_node_set.add(joint_node)
            joint_scale = node_scales[joint_node]
            if (abs(joint_scale[0] - joint_scale[1]) > 1.0e-6 or
                    abs(joint_scale[0] - joint_scale[2]) > 1.0e-6):
                raise CompileError(
                    f"joint node[{joint_node}] has non-uniform bind scale; "
                    "modern-skeletal-v1 cannot transform skinned normals correctly"
                )
            matrix = _finite(inverse, "inverse bind matrix")
            if len(matrix) != 16:
                raise CompileError("inverse bind accessor must contain MAT4 values")
            joint_records.append((joint_node, *matrix))
        skeleton = skin.get("skeleton", joint_nodes[0])
        if (not isinstance(skeleton, int) or isinstance(skeleton, bool)
                or skeleton < 0 or skeleton >= len(nodes)):
            raise CompileError(f"skin[{skin_index}] has an invalid skeleton root")
        skin_records.append((strings.add(_source_name(
                             skin.get("name"), f"skin_{skin_index}",
                             f"skin[{skin_index}]")),
                             first_joint, len(joint_nodes), skeleton))

    key_records = []
    channel_records = []
    animation_records = []
    motion_channels = 0
    static_animations = []
    for animation_index, animation in enumerate(_array(document, "animations")):
        if not isinstance(animation, dict):
            raise CompileError(f"animation[{animation_index}] must be an object")
        first_channel = len(channel_records)
        duration = 0.0
        animation_moves = False
        samplers_in_animation = animation.get("samplers", [])
        channels_in_animation = animation.get("channels", [])
        if not isinstance(samplers_in_animation, list):
            raise CompileError("animation samplers must be an array")
        if not isinstance(channels_in_animation, list):
            raise CompileError("animation channels must be an array")
        animation_name = _source_name(
            animation.get("name"), f"animation_{animation_index}",
            f"animation[{animation_index}]",
        )
        for channel in channels_in_animation:
            if not isinstance(channel, dict):
                raise CompileError("animation channel must be an object")
            sampler = _item(samplers_in_animation, channel.get("sampler"), "animation sampler")
            times = reader.values(sampler.get("input"))
            interpolation_name = sampler.get("interpolation", "LINEAR")
            if interpolation_name not in INTERPOLATION_IDS:
                raise CompileError(f"unsupported animation interpolation {interpolation_name!r}")
            target = channel.get("target", {})
            if not isinstance(target, dict) or target.get("path") not in PATH_IDS:
                raise CompileError("animation channel has an unsupported target")
            target_node = target.get("node")
            if (not isinstance(target_node, int) or isinstance(target_node, bool)
                    or target_node < 0 or target_node >= len(nodes)):
                raise CompileError("animation channel targets an invalid node")
            path_name = target["path"]
            if path_name == "scale" and target_node in joint_node_set:
                raise CompileError(
                    "joint scale animation requires a normal-matrix palette in "
                    "a later renderer profile"
                )
            values = reader.values(sampler.get("output"))
            component_count = 4 if path_name in ("rotation", "weights") else 3
            if path_name == "weights":
                raise CompileError("morph-weight animation requires cache version 2")
            cubic = interpolation_name == "CUBICSPLINE"
            if len(values) != len(times) * (3 if cubic else 1):
                raise CompileError("animation sampler input/output counts do not match")
            first_key = len(key_records)
            previous_time = -math.inf
            first_value = None
            channel_moves = False
            for key_index, time_value in enumerate(times):
                time = float(time_value[0])
                if not math.isfinite(time) or time < 0.0 or time <= previous_time:
                    raise CompileError("animation input times must be finite, non-negative, and increasing")
                previous_time = time
                duration = max(duration, time)
                if cubic:
                    incoming = _finite(values[key_index * 3], "animation incoming tangent")
                    value = _finite(values[key_index * 3 + 1], "animation value")
                    outgoing = _finite(values[key_index * 3 + 2], "animation outgoing tangent")
                else:
                    incoming = (0.0,) * component_count
                    value = _finite(values[key_index], "animation value")
                    outgoing = (0.0,) * component_count
                if len(value) != component_count:
                    raise CompileError("animation output has the wrong component count")
                if first_value is None:
                    first_value = tuple(value)
                elif tuple(value) != first_value:
                    channel_moves = True
                if any(component != 0.0 for component in (*incoming, *outgoing)):
                    channel_moves = True
                pad = 4 - component_count
                key_records.append((time, *value, *(0.0,) * pad,
                                    *incoming, *(0.0,) * pad,
                                    *outgoing, *(0.0,) * pad))
            channel_records.append((target_node, PATH_IDS[path_name],
                                    INTERPOLATION_IDS[interpolation_name],
                                    first_key, len(times), component_count))
            if channel_moves:
                motion_channels += 1
                animation_moves = True
        if not animation_moves:
            static_animations.append(animation_name)
        animation_records.append((strings.add(animation_name),
                                  duration, first_channel, len(channel_records) - first_channel))

    # A skinned source does not need to carry a fake idle merely to enter the
    # reviewed humanoid workflow. Materialize one deterministic cache-local
    # translation channel that exactly preserves the skeleton root's bind TRS.
    # The source GLB and its provenance remain untouched; motion readiness still
    # requires authored moving semantics or a fully reviewed humanoid rig.
    bind_fallback_clip = "__mdkr_bind_pose__"
    if manifest["animations"]["fallback"] == probe.BIND_POSE_FALLBACK:
        if not skin_records:
            raise CompileError("bind-pose fallback requires a skinned character")
        skeleton_node = int(skin_records[0][3])
        bind_translation = tuple(float(value) for value in node_records[skeleton_node][2:5])
        first_key = len(key_records)
        zero4 = (0.0, 0.0, 0.0, 0.0)
        for time in (0.0, 1.0):
            value4 = (*bind_translation, 0.0)
            key_records.append((time, *value4, *zero4, *zero4))
        first_channel = len(channel_records)
        channel_records.append((
            skeleton_node, PATH_IDS["translation"],
            INTERPOLATION_IDS["LINEAR"], first_key, 2, 3,
        ))
        animation_records.append((
            strings.add(bind_fallback_clip), 1.0, first_channel, 1,
        ))
        static_animations.append(probe.BIND_POSE_FALLBACK)

    gameplay = manifest["gameplay"]
    presentation = manifest["presentation"]
    vehicle_mask = sum(VEHICLE_BITS[name] for name in gameplay["vehicles"])
    bounds_min = policy.get("bbox_min")
    bounds_max = policy.get("bbox_max")
    if (
        not isinstance(bounds_min, list) or len(bounds_min) != 3
        or not isinstance(bounds_max, list) or len(bounds_max) != 3
    ):
        raise CompileError("character scene has no finite world-space bounds")
    source_height = float(bounds_max[1]) - float(bounds_min[1])
    if not math.isfinite(source_height) or source_height <= 1.0e-6:
        raise CompileError("character scene height is too small to calibrate")
    source_forward = "+z"
    explicit_calibration = manifest["schema"] in (
        probe.PACKAGE_SCHEMA, probe.PACKAGE_SCHEMA_V3,
        probe.PACKAGE_SCHEMA_V4, probe.PACKAGE_SCHEMA_V5,
    )
    if explicit_calibration:
        source_forward = presentation["source_forward"]
        target_height = float(presentation["target_height_m"])
        uniform_scale = 1.0 / source_height
        definition_scale = (uniform_scale, uniform_scale, uniform_scale)
        definition_translation = (0.0, 0.0, 0.0)
        definition_rotation = SOURCE_FORWARD_ROTATIONS[source_forward]
        context_manifest = presentation["contexts"]
    else:
        definition_scale = tuple(map(float, presentation["scale"]))
        definition_translation = tuple(map(float, presentation["translation_m"]))
        definition_rotation = tuple(map(float, presentation["rotation_xyzw"]))
        target_height = source_height * definition_scale[1]
        context_manifest = {
            "select": {
                "anchor": "ground", "translation_m": [0.0, 0.0, 0.0],
                "rotation_xyzw": [0.0, 0.0, 0.0, 1.0], "scale": 1.0,
            },
            **{
                vehicle: {
                    "anchor": "seat", "translation_m": [0.0, 0.0, 0.0],
                    "rotation_xyzw": [0.0, 0.0, 0.0, 1.0], "scale": 1.0,
                }
                for vehicle in gameplay["vehicles"]
            },
        }
    character_record = (
        strings.add(manifest["id"]), strings.add(manifest["display_name"]),
        DONOR_IDS[gameplay["donor"]], strings.add(manifest["renderer_profile"]),
        *definition_scale, *definition_translation,
        *definition_rotation, float(presentation.get("lod_bias", 0.0)),
        vehicle_mask,
    )
    semantic_records = []
    animations_manifest = manifest["animations"]
    fallback_flags, fallback_blend = _semantic_policy("fallback")
    fallback_clip = (
        bind_fallback_clip
        if animations_manifest["fallback"] == probe.BIND_POSE_FALLBACK
        else animations_manifest["fallback"]
    )
    semantic_records.append((strings.add("fallback"), strings.add(fallback_clip),
                             fallback_flags, fallback_blend))
    disabled_semantics = set(animations_manifest.get("disabled_states", []))
    semantic_mask = SEMANTIC_MASK_BITS["fallback"]
    disabled_semantic_mask = 0
    for semantic, clip in sorted(animations_manifest.get("states", {}).items()):
        flags, blend = _semantic_policy(semantic)
        if semantic in disabled_semantics:
            flags |= SEMANTIC_DISABLED
            disabled_semantic_mask |= SEMANTIC_MASK_BITS.get(semantic, 0)
        else:
            semantic_mask |= SEMANTIC_MASK_BITS.get(semantic, 0)
        semantic_records.append((strings.add(semantic), strings.add(clip), flags, blend))
    socket_records = []
    for semantic, node_name in sorted(manifest["sockets"].items()):
        if node_name not in node_names:
            raise CompileError(f"socket {semantic!r} names missing node {node_name!r}")
        socket_records.append((strings.add(semantic), node_names[node_name]))
    attachment_records = []
    for context in sorted(context_manifest, key=lambda name: CONTEXT_IDS[name]):
        adjustment = context_manifest[context]
        attachment_records.append((
            CONTEXT_IDS[context], strings.add(adjustment["anchor"]),
            *map(float, adjustment["translation_m"]),
            *map(float, adjustment["rotation_xyzw"]),
            float(adjustment.get("scale", 1.0)),
            1 if adjustment["anchor"] == "ground" else 0,
            0,
        ))
    ground = (
        (float(bounds_min[0]) + float(bounds_max[0])) * 0.5,
        float(bounds_min[1]),
        (float(bounds_min[2]) + float(bounds_max[2])) * 0.5,
    )
    calibration_record = (
        *map(float, bounds_min), *map(float, bounds_max), *ground,
        source_height, SOURCE_FORWARD_IDS[source_forward],
        1 if explicit_calibration else 0,
        source_height * definition_scale[1], target_height, 0.0, 0.0,
    )

    rig_records = []
    rig_role_records = []
    rig_manifest = manifest.get("rig")
    rig_role_mask = 0
    joint_constraint_records = []
    secondary_chain_records = []
    secondary_joint_records = []
    if manifest["schema"] in probe.RIG_SCHEMAS:
        if not isinstance(rig_manifest, dict):
            raise CompileError("source-v4/v5 package omits rig metadata")
        role_nodes: dict[str, int] = {}
        for role in probe.HUMANOID_ROLES:
            mapping = rig_manifest["roles"].get(role)
            if mapping is None:
                continue
            node_name = mapping["node"]
            if node_name not in node_names:
                raise CompileError(
                    f"rig role {role!r} names missing node {node_name!r}"
                )
            node_index = node_names[node_name]
            if node_index not in joint_node_set:
                raise CompileError(
                    f"rig role {role!r} node {node_name!r} is not a skin joint"
                )
            role_nodes[role] = node_index
            rig_role_mask |= 1 << RIG_ROLE_IDS[role]
            rig_role_records.append((
                strings.add(role), node_index,
                1 if mapping["inferred"] else 0,
                int(round(float(mapping["confidence"]) * 1000.0)),
                *map(float, mapping.get(
                    "rest_rotation_xyzw", (0.0, 0.0, 0.0, 1.0)
                )),
                *map(float, mapping.get("bend_axis", (0.0, 0.0, 0.0))),
            ))
            constraint = mapping.get("constraint")
            if constraint is not None:
                if rig_manifest["mode"] != "humanoid-retarget-v1":
                    raise CompileError(
                        f"rig role {role!r} constraint requires "
                        "humanoid-retarget-v1 mode"
                    )
                joint_constraint_records.append((
                    RIG_ROLE_IDS[role], node_index,
                    *map(float, constraint["twist_axis"]),
                    float(constraint["swing_limit_degrees"]),
                    float(constraint["twist_min_degrees"]),
                    float(constraint["twist_max_degrees"]),
                ))
        for ancestor_role, descendant_role in RIG_HIERARCHY:
            if ancestor_role not in role_nodes or descendant_role not in role_nodes:
                continue
            if not _is_ancestor(
                parents, role_nodes[ancestor_role], role_nodes[descendant_role]
            ):
                raise CompileError(
                    f"rig hierarchy requires {descendant_role!r} below "
                    f"{ancestor_role!r}"
                )
        rig_records.append((
            RIG_MODE_IDS[rig_manifest["mode"]],
            1 if rig_manifest["reviewed"] else 0,
            len(rig_role_records), rig_role_mask,
        ))

    secondary_manifest = manifest.get("secondary_motion")
    if isinstance(secondary_manifest, dict):
        used_dynamic_nodes: set[int] = set()
        role_node_set = {
            record[1] for record in rig_role_records
        }
        for chain in secondary_manifest["chains"]:
            root_name = chain["root"]
            if root_name not in node_names:
                raise CompileError(
                    f"secondary chain {chain['name']!r} names missing root "
                    f"node {root_name!r}"
                )
            root_node = node_names[root_name]
            if not _orientation_preserving(parents, node_scales, root_node):
                raise CompileError(
                    f"secondary chain {chain['name']!r} root has a mirrored "
                    "or singular world transform"
                )
            previous = root_node
            first_joint = len(secondary_joint_records)
            for order, node_name in enumerate(chain["joints"]):
                if node_name not in node_names:
                    raise CompileError(
                        f"secondary chain {chain['name']!r} names missing "
                        f"joint node {node_name!r}"
                    )
                node_index = node_names[node_name]
                if node_index not in joint_node_set:
                    raise CompileError(
                        f"secondary chain {chain['name']!r} node "
                        f"{node_name!r} is not a skin joint"
                    )
                if not _orientation_preserving(
                        parents, node_scales, node_index):
                    raise CompileError(
                        f"secondary chain {chain['name']!r} node "
                        f"{node_name!r} has a mirrored or singular world transform"
                    )
                if parents[node_index] != previous:
                    raise CompileError(
                        f"secondary chain {chain['name']!r} must be a direct "
                        "parent-to-child node path"
                    )
                if node_index in used_dynamic_nodes:
                    raise CompileError(
                        f"secondary chain {chain['name']!r} reuses dynamic "
                        f"node {node_name!r}"
                    )
                if node_index in role_node_set:
                    raise CompileError(
                        f"secondary chain {chain['name']!r} overlaps humanoid "
                        f"role node {node_name!r}"
                    )
                used_dynamic_nodes.add(node_index)
                secondary_joint_records.append((
                    node_index, len(secondary_chain_records), order, 0,
                ))
                previous = node_index
            secondary_chain_records.append((
                strings.add(chain["name"]), root_node, first_joint,
                len(chain["joints"]),
                float(chain["stiffness_hz"]),
                float(chain["damping_ratio"]),
                float(chain["inertia"]),
                float(chain["max_angle_degrees"]),
                *map(float, chain["bend_axis"]),
            ))

    identity_records = []
    identity_name_records = []
    identity_data = b""
    identity_manifest = manifest.get("identity")
    if manifest["schema"] in probe.IDENTITY_SCHEMAS:
        if portrait is None:
            raise CompileError("source-v3/v4/v5 package omits portrait.png")
        try:
            probe.inspect_portrait_png(portrait)
        except probe.ProbeError as exc:
            raise CompileError(str(exc)) from exc
        if not isinstance(identity_manifest, dict):
            raise CompileError("source-v3/v4/v5 package omits identity metadata")
        if hashlib.sha256(portrait).hexdigest() != identity_manifest["portrait_sha256"]:
            raise CompileError("portrait.png digest does not match the manifest")
        red, green, blue = identity_manifest["minimap_rgb"]
        minimap_rgba = red | (green << 8) | (blue << 16) | (255 << 24)
        identity_data = portrait
        short_name = identity_manifest.get("short_name")
        narration_name = identity_manifest.get("narration_name")
        sort_label = identity_manifest.get("sort_label")
        identity_records.append((
            1, 1, 0, len(portrait), minimap_rgba,
            strings.add(short_name) if short_name is not None else 0,
        ))
        identity_name_records.append((
            strings.add(narration_name) if narration_name is not None else 0,
            strings.add(sort_label) if sort_label is not None else 0,
            0,
        ))

    license_manifest = manifest["license"]
    provenance_record = (
        strings.add(license_manifest["spdx"]),
        strings.add(license_manifest["attribution"]),
        strings.add(license_manifest["source_url"]),
        1,  # exact LICENSE.txt bytes are included in the source digest
    )

    sections = [
        Section(SECTION_STRINGS, len(strings.data), 1, bytes(strings.data)),
        Section(SECTION_VERTICES, len(vertex_records), struct.calcsize(VERTEX_FORMAT), _pack_records(VERTEX_FORMAT, vertex_records)),
        Section(SECTION_INDICES, len(indices_output), 4, _pack_records("<I", ((value,) for value in indices_output))),
        Section(SECTION_PRIMITIVES, len(primitive_records), struct.calcsize(PRIMITIVE_FORMAT), _pack_records(PRIMITIVE_FORMAT, primitive_records)),
        Section(SECTION_MATERIALS, len(material_records), struct.calcsize(MATERIAL_FORMAT), _pack_records(MATERIAL_FORMAT, material_records)),
        Section(SECTION_TEXTURES, len(texture_records), struct.calcsize(TEXTURE_FORMAT), _pack_records(TEXTURE_FORMAT, texture_records)),
        Section(SECTION_TEXTURE_DATA, len(texture_data), 1, bytes(texture_data)),
        Section(SECTION_NODES, len(node_records), struct.calcsize(NODE_FORMAT), _pack_records(NODE_FORMAT, node_records)),
        Section(SECTION_SKINS, len(skin_records), struct.calcsize(SKIN_FORMAT), _pack_records(SKIN_FORMAT, skin_records)),
        Section(SECTION_JOINTS, len(joint_records), struct.calcsize(JOINT_FORMAT), _pack_records(JOINT_FORMAT, joint_records)),
        Section(SECTION_ANIMATIONS, len(animation_records), struct.calcsize(ANIMATION_FORMAT), _pack_records(ANIMATION_FORMAT, animation_records)),
        Section(SECTION_CHANNELS, len(channel_records), struct.calcsize(CHANNEL_FORMAT), _pack_records(CHANNEL_FORMAT, channel_records)),
        Section(SECTION_KEYS, len(key_records), struct.calcsize(KEY_FORMAT), _pack_records(KEY_FORMAT, key_records)),
        Section(SECTION_CHARACTER, 1, struct.calcsize(CHARACTER_FORMAT), _pack_records(CHARACTER_FORMAT, (character_record,))),
        Section(SECTION_SEMANTICS, len(semantic_records), struct.calcsize(SEMANTIC_FORMAT), _pack_records(SEMANTIC_FORMAT, semantic_records)),
        Section(SECTION_SOCKETS, len(socket_records), struct.calcsize(SOCKET_FORMAT), _pack_records(SOCKET_FORMAT, socket_records)),
        Section(SECTION_ATTACHMENTS, len(attachment_records), struct.calcsize(ATTACHMENT_FORMAT), _pack_records(ATTACHMENT_FORMAT, attachment_records)),
        Section(SECTION_CALIBRATION, 1, struct.calcsize(CALIBRATION_FORMAT), _pack_records(CALIBRATION_FORMAT, (calibration_record,))),
    ]
    if identity_records:
        sections.extend((
            Section(SECTION_IDENTITY, 1, struct.calcsize(IDENTITY_FORMAT),
                    _pack_records(IDENTITY_FORMAT, identity_records)),
            Section(SECTION_IDENTITY_DATA, len(identity_data), 1, identity_data),
            Section(
                SECTION_IDENTITY_NAMES, 1,
                struct.calcsize(IDENTITY_NAMES_FORMAT),
                _pack_records(IDENTITY_NAMES_FORMAT, identity_name_records),
            ),
        ))
    if rig_records:
        sections.extend((
            Section(SECTION_RIG, 1, struct.calcsize(RIG_FORMAT),
                    _pack_records(RIG_FORMAT, rig_records)),
            Section(SECTION_RIG_ROLES, len(rig_role_records),
                    struct.calcsize(RIG_ROLE_FORMAT),
                    _pack_records(RIG_ROLE_FORMAT, rig_role_records)),
        ))
    if joint_constraint_records:
        sections.append(Section(
            SECTION_JOINT_CONSTRAINTS, len(joint_constraint_records),
            struct.calcsize(JOINT_CONSTRAINT_FORMAT),
            _pack_records(JOINT_CONSTRAINT_FORMAT, joint_constraint_records),
        ))
    if secondary_chain_records:
        sections.extend((
            Section(
                SECTION_SECONDARY_CHAINS, len(secondary_chain_records),
                struct.calcsize(SECONDARY_CHAIN_FORMAT),
                _pack_records(SECONDARY_CHAIN_FORMAT, secondary_chain_records),
            ),
            Section(
                SECTION_SECONDARY_JOINTS, len(secondary_joint_records),
                struct.calcsize(SECONDARY_JOINT_FORMAT),
                _pack_records(SECONDARY_JOINT_FORMAT, secondary_joint_records),
            ),
        ))
    sections.append(Section(
        SECTION_PROVENANCE, 1, struct.calcsize(PROVENANCE_FORMAT),
        _pack_records(PROVENANCE_FORMAT, (provenance_record,)),
    ))
    compiled = _assemble(sections, source_digest)
    lod_vertices = [0, 0, 0, 0]
    lod_triangles = [0, 0, 0, 0]
    lod_primitives = [0, 0, 0, 0]
    for primitive in primitive_records:
        lod = primitive[7]
        lod_vertices[lod] += primitive[1]
        lod_triangles[lod] += primitive[3] // 3
        lod_primitives[lod] += 1
    report = {
        "compiler": COMPILER_ID,
        "format": "mdkc-v2",
        "bytes": len(compiled),
        "sha256": hashlib.sha256(compiled).hexdigest(),
        "source_sha256": source_digest.hex(),
        "character_id": manifest["id"],
        "display_name": manifest["display_name"],
        "donor": gameplay["donor"],
        "vehicle_mask": vehicle_mask,
        "vertices": len(vertex_records),
        "indices": len(indices_output),
        "triangles": len(indices_output) // 3,
        "primitives": len(primitive_records),
        "lod_levels": max(node_lods, default=0) + 1,
        "materials": len(material_records),
        "textures": len(texture_records),
        "encoded_texture_bytes": len(texture_data),
        "decoded_texture_bytes": decoded_texture_bytes,
        "ktx2_texture_count": ktx2_texture_count,
        "ktx2_source_bytes": ktx2_source_bytes,
        "ktx2_etc1s_count": ktx2_etc1s_count,
        "ktx2_uastc_count": ktx2_uastc_count,
        "ktx2_mip_levels_min": min(ktx2_mip_levels, default=0),
        "ktx2_mip_levels_max": max(ktx2_mip_levels, default=0),
        "authored_tangent_primitives": authored_tangent_primitives,
        "generated_tangent_primitives": generated_tangent_primitives,
        "authored_tangent_repaired_vertices": (
            authored_tangent_repaired_vertices
        ),
        "generated_tangent_degenerate_uv_triangles": (
            generated_tangent_degenerate_uv_triangles
        ),
        "tangent_fallback_vertices": tangent_fallback_vertices,
        "normal_map_tangent_fallback_vertices": (
            normal_map_tangent_fallback_vertices
        ),
        "nodes": len(node_records),
        "skins": len(skin_records),
        "joints": len(joint_records),
        "animations": len(animation_records),
        "animation_channels": len(channel_records),
        "motion_channels": motion_channels,
        "static_animations": static_animations,
        "disabled_semantics": sorted(disabled_semantics),
        "semantic_mask": semantic_mask,
        "disabled_semantic_mask": disabled_semantic_mask,
        "animation_keys": len(key_records),
        "lod_vertices": lod_vertices,
        "lod_triangles": lod_triangles,
        "lod_primitives": lod_primitives,
        "semantics": len(semantic_records),
        "sockets": len(socket_records),
        "source_schema": manifest["schema"],
        "source_forward": source_forward,
        "source_height_m": source_height,
        "target_height_m": target_height,
        "ground_anchor_m": list(ground),
        "attachment_contexts": sorted(context_manifest, key=lambda name: CONTEXT_IDS[name]),
        "calibration_explicit": explicit_calibration,
        "identity_portrait": bool(identity_records),
        "identity_short_name": (
            identity_manifest.get("short_name", manifest["display_name"])
            if isinstance(identity_manifest, dict) else None
        ),
        "identity_narration_name": (
            identity_manifest.get("narration_name", manifest["display_name"])
            if isinstance(identity_manifest, dict) else None
        ),
        "identity_sort_label": (
            identity_manifest.get("sort_label", manifest["display_name"])
            if isinstance(identity_manifest, dict) else None
        ),
        "identity_portrait_bytes": len(identity_data),
        "minimap_rgb": (
            identity_manifest.get("minimap_rgb")
            if isinstance(identity_manifest, dict) else None
        ),
        "rig_mode": (
            rig_manifest.get("mode") if isinstance(rig_manifest, dict) else None
        ),
        "rig_reviewed": (
            rig_manifest.get("reviewed")
            if isinstance(rig_manifest, dict) else False
        ),
        "rig_roles": len(rig_role_records),
        "joint_constraints": len(joint_constraint_records),
        "secondary_chains": len(secondary_chain_records),
        "secondary_joints": len(secondary_joint_records),
        "rig_role_mask": rig_role_mask,
    }
    return compiled, report


def compile_package(package_path: Path, output_path: Path) -> dict[str, Any]:
    verification = probe.verify_package(package_path)
    if not verification["valid"]:
        raise CompileError("invalid source package: " + "; ".join(verification["errors"]))
    with zipfile.ZipFile(package_path) as archive:
        manifest = probe.json_loads_strict(
            archive.read("manifest.json"), "manifest"
        )
        model = archive.read("model.glb")
        portrait = (
            archive.read("portrait.png")
            if manifest.get("schema") in probe.IDENTITY_SCHEMAS else None
        )
        digest = source_digest(
            (name, archive.read(name))
            for name in probe.package_members_for_schema(manifest.get("schema"))
        )
    compiled, report = compile_character(
        model, manifest, digest, portrait
    )
    output_path.parent.mkdir(parents=True, exist_ok=True)
    temporary = output_path.with_name(output_path.name + ".tmp")
    temporary.write_bytes(compiled)
    temporary.replace(output_path)
    report["output"] = str(output_path)
    return report


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path, help="validated .mdkrchar source package")
    parser.add_argument("--output", required=True, type=Path, help="private .mdkc cache path")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    try:
        report = compile_package(args.input, args.output)
        json.dump(report, sys.stdout, indent=2, sort_keys=True)
        sys.stdout.write("\n")
        return 0
    except (OSError, zipfile.BadZipFile, json.JSONDecodeError, probe.ProbeError, CompileError) as exc:
        json.dump({"error": str(exc)}, sys.stdout, indent=2, sort_keys=True)
        sys.stdout.write("\n")
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
