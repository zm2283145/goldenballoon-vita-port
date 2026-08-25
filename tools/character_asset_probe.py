#!/usr/bin/env python3
"""Inspect and package user-supplied modern character assets.

The stable input contract is a self-contained glTF 2.0 binary (GLB); this tool
adds MDKR policy checks and builds a deterministic, data-only source package
around that GLB. A bounded, dependency-free COLLADA convenience adapter lives
in collada_to_glb.py. Richer DAE/FBX authoring still belongs in Blender or an
equivalent DCC exporter, with GLB as the handoff boundary.
"""

from __future__ import annotations

import argparse
import hashlib
import io
import json
import math
import re
import struct
import sys
import unicodedata
import zipfile
import zlib
from pathlib import Path, PurePosixPath
from typing import Any
from xml.etree import ElementTree


GLB_MAGIC = b"glTF"
GLB_JSON_CHUNK = 0x4E4F534A
GLB_BIN_CHUNK = 0x004E4942
MAX_INPUT_BYTES = 512 * 1024 * 1024
MAX_MANIFEST_BYTES = 1024 * 1024
MAX_LICENSE_BYTES = 1024 * 1024
MAX_ARCHIVE_MEMBERS = 4096
MAX_NESTED_ARCHIVE_BYTES = 64 * 1024 * 1024
MAX_ARCHIVE_DEPTH = 2
MAX_JOINTS = 128
MAX_VERTICES = 100_000
MAX_TRIANGLES = 100_000
MAX_MATERIALS = 16
PACKAGE_SCHEMA = "mdkr-character-source-v1"
PACKAGE_MEMBERS = ("manifest.json", "model.glb", "LICENSE.txt")
PORTABLE_PACKAGE_MEMBERS = PACKAGE_MEMBERS + ("compiled.mdkc",)
PACKAGE_EPOCH = (1980, 1, 1, 0, 0, 0)
MODEL_SUFFIXES = {".glb", ".gltf", ".dae", ".fbx", ".obj"}
LICENSE_NAMES = {
    "license",
    "license.txt",
    "license.md",
    "copying",
    "copying.txt",
    "copyright",
    "copyright.txt",
}
SUPPORTED_REQUIRED_EXTENSIONS = {
    "KHR_materials_emissive_strength",
    "KHR_materials_specular",
    "KHR_mesh_quantization",
    "MSFT_lod",
}
GAMEPLAY_DONORS = {
    "banjo", "bumper", "conker", "diddy", "drumstick",
    "krunch", "pipsy", "timber", "tiptup", "tt",
}
VEHICLE_NAMES = {"car", "hovercraft", "plane"}
RECOMMENDED_RACE_SEMANTICS = (
    "race.steer", "race.reverse", "race.boost", "race.damage", "race.item",
    "race.spin", "race.airborne", "race.land", "race.finish_win",
    "race.finish_lose",
)
RECOMMENDED_SELECT_SEMANTICS = (
    "select.idle", "select.hover", "select.confirm",
)
REQUIRED_PRESENTATION_SOCKETS = {"seat", "head"}
ID_RE = re.compile(r"^[a-z0-9][a-z0-9._-]{1,63}$")
SEMANTIC_RE = re.compile(r"^[a-z][a-z0-9_.-]{0,63}$")


class ProbeError(ValueError):
    """A bounded, user-facing asset validation failure."""


def _sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _json_bytes(value: Any) -> bytes:
    return (json.dumps(value, sort_keys=True, separators=(",", ":")) + "\n").encode("utf-8")


def json_loads_strict(data: bytes | str, label: str = "JSON") -> Any:
    """Decode hostile JSON without duplicate keys or non-finite numbers."""
    def object_from_pairs(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
        value: dict[str, Any] = {}
        for key, member in pairs:
            if key in value:
                raise ProbeError(f"{label} contains duplicate key {key!r}")
            value[key] = member
        return value

    def reject_constant(value: str) -> Any:
        raise ProbeError(f"{label} contains non-finite number {value}")

    try:
        text = data.decode("utf-8") if isinstance(data, bytes) else data
        return json.loads(
            text, object_pairs_hook=object_from_pairs,
            parse_constant=reject_constant,
        )
    except UnicodeDecodeError as exc:
        raise ProbeError(f"{label} is not valid UTF-8: {exc}") from exc


def _manifest_unicode_errors(value: Any, path: str = "manifest") -> list[str]:
    errors: list[str] = []
    if isinstance(value, str):
        if unicodedata.normalize("NFC", value) != value:
            errors.append(f"{path} must use NFC-normalized Unicode")
    elif isinstance(value, dict):
        for key, member in value.items():
            if unicodedata.normalize("NFC", key) != key:
                errors.append(f"{path} contains a non-NFC key")
            errors.extend(_manifest_unicode_errors(member, f"{path}.{key}"))
    elif isinstance(value, list):
        for index, member in enumerate(value):
            errors.extend(_manifest_unicode_errors(member, f"{path}[{index}]"))
    return errors


def _read_bounded(path: Path, maximum: int, label: str) -> bytes:
    size = path.stat().st_size
    if size > maximum:
        raise ProbeError(f"{label} exceeds {maximum} bytes")
    data = path.read_bytes()
    if len(data) > maximum:
        raise ProbeError(f"{label} exceeds {maximum} bytes")
    return data


def _safe_archive_name(raw_name: str) -> str:
    name = raw_name.replace("\\", "/")
    path = PurePosixPath(name)
    if not name or name.startswith("/") or path.is_absolute() or ".." in path.parts:
        raise ProbeError(f"unsafe archive member path: {raw_name!r}")
    if path.parts and ":" in path.parts[0]:
        raise ProbeError(f"drive-qualified archive member path: {raw_name!r}")
    return path.as_posix()


def _zip_member_is_symlink(info: zipfile.ZipInfo) -> bool:
    return ((info.external_attr >> 16) & 0o170000) == 0o120000


def _local_name(tag: str) -> str:
    return tag.rsplit("}", 1)[-1]


def inspect_dae(data: bytes, name: str = "model.dae") -> dict[str, Any]:
    if len(data) > MAX_INPUT_BYTES:
        raise ProbeError(f"COLLADA input exceeds {MAX_INPUT_BYTES} bytes")
    try:
        root = ElementTree.fromstring(data)
    except ElementTree.ParseError as exc:
        raise ProbeError(f"invalid COLLADA XML in {name}: {exc}") from exc

    def elements(local: str):
        return (elem for elem in root.iter() if _local_name(elem.tag) == local)

    author = next((elem.text or "" for elem in elements("author")), "").strip()
    authoring_tool = next((elem.text or "" for elem in elements("authoring_tool")), "").strip()
    unit = next(elements("unit"), None)
    up_axis = next((elem.text or "" for elem in elements("up_axis")), "").strip()
    triangle_count = sum(int(elem.attrib.get("count", "0")) for elem in elements("triangles"))
    triangle_count += sum(int(elem.attrib.get("count", "0")) for elem in elements("polylist"))

    joint_names: set[str] = set()
    for array_name in ("Name_array", "IDREF_array"):
        for elem in elements(array_name):
            joint_names.update((elem.text or "").split())
    influence_max = 0
    for elem in elements("vcount"):
        counts = [int(value) for value in (elem.text or "").split()]
        influence_max = max(influence_max, max(counts, default=0))

    channels = sum(1 for _ in elements("channel"))
    return {
        "format": "collada-1.x",
        "name": name,
        "author": author or None,
        "authoring_tool": authoring_tool or None,
        "unit_meter": float(unit.attrib.get("meter", "1")) if unit is not None else 1.0,
        "up_axis": up_axis or None,
        "geometry_count": sum(1 for _ in elements("geometry")),
        "triangle_count": triangle_count,
        "material_count": sum(1 for _ in elements("material")),
        "image_count": sum(1 for _ in elements("image")),
        "skin_count": sum(1 for _ in elements("skin")),
        "joint_name_count_upper_bound": len(joint_names),
        "max_influences": influence_max,
        "animation_channel_count": channels,
    }


def inspect_archive_bytes(data: bytes, name: str, depth: int = 0) -> dict[str, Any]:
    if len(data) > MAX_INPUT_BYTES:
        raise ProbeError(f"archive exceeds {MAX_INPUT_BYTES} bytes")
    if depth > MAX_ARCHIVE_DEPTH:
        raise ProbeError(f"archive nesting exceeds {MAX_ARCHIVE_DEPTH}")
    try:
        archive = zipfile.ZipFile(io.BytesIO(data))
    except zipfile.BadZipFile as exc:
        raise ProbeError(f"invalid ZIP archive {name}: {exc}") from exc

    infos = archive.infolist()
    if len(infos) > MAX_ARCHIVE_MEMBERS:
        raise ProbeError(f"archive contains more than {MAX_ARCHIVE_MEMBERS} members")
    total = 0
    files: list[dict[str, Any]] = []
    models: list[dict[str, Any]] = []
    licenses: list[str] = []
    nested: list[dict[str, Any]] = []
    for info in infos:
        safe_name = _safe_archive_name(info.filename)
        if info.is_dir():
            continue
        if info.flag_bits & 1:
            raise ProbeError(f"encrypted archive member is unsupported: {safe_name}")
        if _zip_member_is_symlink(info):
            raise ProbeError(f"archive symlink is unsupported: {safe_name}")
        total += info.file_size
        if total > MAX_INPUT_BYTES:
            raise ProbeError(f"expanded archive exceeds {MAX_INPUT_BYTES} bytes")
        suffix = PurePosixPath(safe_name).suffix.lower()
        files.append({"path": safe_name, "bytes": info.file_size, "sha256": None})
        basename = PurePosixPath(safe_name).name.lower()
        if basename in LICENSE_NAMES or basename.startswith("license."):
            licenses.append(safe_name)
        if suffix in MODEL_SUFFIXES:
            entry: dict[str, Any] = {"path": safe_name, "format": suffix[1:]}
            if suffix == ".dae":
                entry["inspection"] = inspect_dae(archive.read(info), safe_name)
            models.append(entry)
        if suffix == ".zip":
            if info.file_size > MAX_NESTED_ARCHIVE_BYTES:
                raise ProbeError(f"nested archive is too large: {safe_name}")
            nested.append(inspect_archive_bytes(archive.read(info), safe_name, depth + 1))

    blockers: list[str] = []
    if not licenses and not any(item["license_files"] for item in nested):
        blockers.append("no embedded license or copyright file")
    if not models and not any(item["models"] or item["nested_archives"] for item in nested):
        blockers.append("no supported model candidate")
    return {
        "format": "zip",
        "name": name,
        "archive_depth": depth,
        "member_count": len(files),
        "expanded_bytes": total,
        "license_files": licenses,
        "models": models,
        "nested_archives": nested,
        "blockers": blockers,
    }


def inspect_archive(path: Path) -> dict[str, Any]:
    return inspect_archive_bytes(_read_bounded(path, MAX_INPUT_BYTES, "archive"), path.name)


def parse_glb(data: bytes) -> tuple[dict[str, Any], bytes | None]:
    if len(data) > MAX_INPUT_BYTES:
        raise ProbeError(f"GLB input exceeds {MAX_INPUT_BYTES} bytes")
    if len(data) < 20:
        raise ProbeError("GLB is shorter than its header and JSON chunk")
    magic, version, declared_length = struct.unpack_from("<4sII", data, 0)
    if magic != GLB_MAGIC:
        raise ProbeError("GLB magic is not 'glTF'")
    if version != 2:
        raise ProbeError(f"unsupported GLB version {version}; expected 2")
    if declared_length != len(data):
        raise ProbeError(f"GLB declares {declared_length} bytes but contains {len(data)}")

    offset = 12
    json_chunk: bytes | None = None
    bin_chunk: bytes | None = None
    chunk_index = 0
    while offset < len(data):
        if offset + 8 > len(data):
            raise ProbeError("truncated GLB chunk header")
        chunk_length, chunk_type = struct.unpack_from("<II", data, offset)
        offset += 8
        end = offset + chunk_length
        if chunk_length % 4 or end > len(data):
            raise ProbeError("invalid GLB chunk size")
        payload = data[offset:end]
        offset = end
        if chunk_index == 0 and chunk_type != GLB_JSON_CHUNK:
            raise ProbeError("first GLB chunk is not JSON")
        if chunk_type == GLB_JSON_CHUNK:
            if json_chunk is not None:
                raise ProbeError("GLB contains multiple JSON chunks")
            json_chunk = payload
        elif chunk_type == GLB_BIN_CHUNK:
            if bin_chunk is not None:
                raise ProbeError("GLB contains multiple BIN chunks")
            bin_chunk = payload
        chunk_index += 1
    if json_chunk is None:
        raise ProbeError("GLB has no JSON chunk")
    try:
        document = json_loads_strict(
            json_chunk.rstrip(b" \t\r\n\0"), "GLB JSON"
        )
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise ProbeError(f"invalid GLB JSON: {exc}") from exc
    if not isinstance(document, dict):
        raise ProbeError("GLB JSON root must be an object")
    return document, bin_chunk


def _array(document: dict[str, Any], name: str) -> list[Any]:
    value = document.get(name, [])
    return value if isinstance(value, list) else []


def _accessor(document: dict[str, Any], index: Any) -> dict[str, Any] | None:
    accessors = _array(document, "accessors")
    return accessors[index] if isinstance(index, int) and 0 <= index < len(accessors) else None


def _matrix_multiply(left: list[float], right: list[float]) -> list[float]:
    return [sum(left[inner * 4 + row] * right[column * 4 + inner]
                for inner in range(4))
            for column in range(4) for row in range(4)]


def _node_matrix(node: dict[str, Any]) -> list[float]:
    if "matrix" in node:
        values = node["matrix"]
        if (not isinstance(values, list) or len(values) != 16 or
                not all(isinstance(value, (int, float)) and math.isfinite(value)
                        for value in values)):
            raise ProbeError("node matrix must contain 16 finite numbers")
        return [float(value) for value in values]
    translation = node.get("translation", [0.0, 0.0, 0.0])
    rotation = node.get("rotation", [0.0, 0.0, 0.0, 1.0])
    scale = node.get("scale", [1.0, 1.0, 1.0])
    if (not all(isinstance(value, list) for value in (translation, rotation, scale)) or
            len(translation) != 3 or len(rotation) != 4 or len(scale) != 3 or
            not all(isinstance(component, (int, float)) and math.isfinite(component)
                    for value in (translation, rotation, scale) for component in value)):
        raise ProbeError("node TRS is malformed or non-finite")
    x, y, z, w = (float(value) for value in rotation)
    length = math.sqrt(x * x + y * y + z * z + w * w)
    if length < 1.0e-12:
        raise ProbeError("node quaternion has zero length")
    x, y, z, w = x / length, y / length, z / length, w / length
    sx, sy, sz = (float(value) for value in scale)
    return [
        (1 - 2 * (y * y + z * z)) * sx,
        (2 * (x * y + z * w)) * sx,
        (2 * (x * z - y * w)) * sx, 0.0,
        (2 * (x * y - z * w)) * sy,
        (1 - 2 * (x * x + z * z)) * sy,
        (2 * (y * z + x * w)) * sy, 0.0,
        (2 * (x * z + y * w)) * sz,
        (2 * (y * z - x * w)) * sz,
        (1 - 2 * (x * x + y * y)) * sz, 0.0,
        float(translation[0]), float(translation[1]), float(translation[2]), 1.0,
    ]


def _world_bounds(document: dict[str, Any],
                  mesh_bounds: list[tuple[list[float], list[float]] | None],
                  errors: list[str]) -> tuple[list[float] | None, list[float] | None]:
    nodes = _array(document, "nodes")
    scenes = _array(document, "scenes")
    scene_index = document.get("scene", 0)
    if not isinstance(scene_index, int) or not 0 <= scene_index < len(scenes):
        errors.append("default scene index is invalid")
        return None, None
    scene = scenes[scene_index]
    roots = scene.get("nodes", []) if isinstance(scene, dict) else []
    if not isinstance(roots, list):
        errors.append("default scene node list is invalid")
        return None, None
    identity = [1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0,
                0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0]
    output_min: list[float] | None = None
    output_max: list[float] | None = None

    def visit(index: Any, parent: list[float], ancestry: set[int]) -> None:
        nonlocal output_min, output_max
        if not isinstance(index, int) or not 0 <= index < len(nodes):
            errors.append("scene graph references an invalid node")
            return
        if index in ancestry:
            errors.append("scene graph contains a node cycle")
            return
        node = nodes[index]
        if not isinstance(node, dict):
            errors.append(f"nodes[{index}] must be an object")
            return
        try:
            world = _matrix_multiply(parent, _node_matrix(node))
        except ProbeError as exc:
            errors.append(f"nodes[{index}]: {exc}")
            return
        mesh_index = node.get("mesh")
        if isinstance(mesh_index, int) and 0 <= mesh_index < len(mesh_bounds):
            bounds = mesh_bounds[mesh_index]
            if bounds is not None:
                local_min, local_max = bounds
                for x in (local_min[0], local_max[0]):
                    for y in (local_min[1], local_max[1]):
                        for z in (local_min[2], local_max[2]):
                            point = [
                                world[0] * x + world[4] * y + world[8] * z + world[12],
                                world[1] * x + world[5] * y + world[9] * z + world[13],
                                world[2] * x + world[6] * y + world[10] * z + world[14],
                            ]
                            if output_min is None:
                                output_min, output_max = list(point), list(point)
                            else:
                                output_min = [min(output_min[i], point[i]) for i in range(3)]
                                output_max = [max(output_max[i], point[i]) for i in range(3)]
        next_ancestry = ancestry | {index}
        children = node.get("children", [])
        if not isinstance(children, list):
            errors.append(f"nodes[{index}].children must be an array")
            return
        for child in children:
            visit(child, world, next_ancestry)

    for root_index in roots:
        visit(root_index, identity, set())
    return output_min, output_max


def inspect_glb_bytes(data: bytes, require_character: bool = False) -> dict[str, Any]:
    document, bin_chunk = parse_glb(data)
    errors: list[str] = []
    warnings: list[str] = []
    if document.get("asset", {}).get("version") != "2.0":
        errors.append("asset.version must be exactly '2.0'")

    buffers = _array(document, "buffers")
    for index, buffer in enumerate(buffers):
        if isinstance(buffer, dict) and "uri" in buffer:
            errors.append(f"buffers[{index}] is external; source GLB must be self-contained")
    for index, image in enumerate(_array(document, "images")):
        if isinstance(image, dict) and "uri" in image:
            errors.append(f"images[{index}] is external; source GLB must be self-contained")

    required_extensions = set(document.get("extensionsRequired", []))
    unknown_required = sorted(required_extensions - SUPPORTED_REQUIRED_EXTENSIONS)
    if unknown_required:
        errors.append("unsupported required extensions: " + ", ".join(unknown_required))

    vertex_count = 0
    triangle_count = 0
    primitive_count = 0
    primitive_materials: set[int] = set()
    bbox_min: list[float] | None = None
    bbox_max: list[float] | None = None
    meshes = _array(document, "meshes")
    mesh_bounds: list[tuple[list[float], list[float]] | None] = [None] * len(meshes)
    skinned_primitive_count = 0
    for mesh_index, mesh in enumerate(meshes):
        mesh_min: list[float] | None = None
        mesh_max: list[float] | None = None
        if not isinstance(mesh, dict):
            errors.append(f"meshes[{mesh_index}] must be an object")
            continue
        for primitive_index, primitive in enumerate(mesh.get("primitives", [])):
            prefix = f"meshes[{mesh_index}].primitives[{primitive_index}]"
            primitive_count += 1
            if primitive.get("mode", 4) != 4:
                errors.append(f"{prefix} must use TRIANGLES mode")
            if "indices" not in primitive:
                errors.append(f"{prefix} must be indexed")
            index_accessor = _accessor(document, primitive.get("indices"))
            if index_accessor is not None:
                index_count = int(index_accessor.get("count", 0))
                if index_count % 3:
                    errors.append(f"{prefix} index count is not divisible by three")
                triangle_count += index_count // 3
            attributes = primitive.get("attributes", {})
            if not isinstance(attributes, dict):
                errors.append(f"{prefix}.attributes must be an object")
                continue
            for semantic in ("POSITION", "NORMAL", "TEXCOORD_0"):
                if semantic not in attributes:
                    errors.append(f"{prefix} is missing {semantic}")
            joints = "JOINTS_0" in attributes
            weights = "WEIGHTS_0" in attributes
            if joints != weights:
                errors.append(f"{prefix} must provide JOINTS_0 and WEIGHTS_0 together")
            if joints:
                skinned_primitive_count += 1
            if "JOINTS_1" in attributes or "WEIGHTS_1" in attributes:
                errors.append(f"{prefix} exceeds the v1 four-influence skinning contract")
            position = _accessor(document, attributes.get("POSITION"))
            if position is not None:
                vertex_count += int(position.get("count", 0))
                pmin, pmax = position.get("min"), position.get("max")
                if (isinstance(pmin, list) and isinstance(pmax, list) and
                        len(pmin) == len(pmax) == 3 and
                        all(isinstance(value, (int, float)) and math.isfinite(value)
                            for value in pmin + pmax)):
                    if mesh_min is None:
                        mesh_min, mesh_max = list(pmin), list(pmax)
                    else:
                        mesh_min = [min(mesh_min[i], pmin[i]) for i in range(3)]
                        mesh_max = [max(mesh_max[i], pmax[i]) for i in range(3)]
                    if bbox_min is None:
                        bbox_min, bbox_max = list(pmin), list(pmax)
                    else:
                        bbox_min = [min(bbox_min[i], pmin[i]) for i in range(3)]
                        bbox_max = [max(bbox_max[i], pmax[i]) for i in range(3)]
            if isinstance(primitive.get("material"), int):
                primitive_materials.add(primitive["material"])
        mesh_bounds[mesh_index] = ((mesh_min, mesh_max)
                                   if mesh_min is not None and mesh_max is not None
                                   else None)

    world_bbox_min, world_bbox_max = _world_bounds(document, mesh_bounds, errors)

    skins = _array(document, "skins")
    max_joints = max((len(skin.get("joints", [])) for skin in skins if isinstance(skin, dict)), default=0)
    animations: list[dict[str, Any]] = []
    for index, animation in enumerate(_array(document, "animations")):
        if not isinstance(animation, dict):
            continue
        duration = 0.0
        for sampler in animation.get("samplers", []):
            accessor = _accessor(document, sampler.get("input") if isinstance(sampler, dict) else None)
            if accessor and isinstance(accessor.get("max"), list) and accessor["max"]:
                duration = max(duration, float(accessor["max"][0]))
        animations.append({
            "index": index,
            "name": animation.get("name") or f"animation_{index}",
            "channels": len(animation.get("channels", [])),
            "duration_seconds": duration,
        })

    if vertex_count > MAX_VERTICES:
        errors.append(f"vertex count {vertex_count} exceeds the v1 budget {MAX_VERTICES}")
    if triangle_count > MAX_TRIANGLES:
        errors.append(f"triangle count {triangle_count} exceeds the v1 budget {MAX_TRIANGLES}")
    if len(_array(document, "materials")) > MAX_MATERIALS:
        errors.append(f"material count exceeds the v1 budget {MAX_MATERIALS}")
    if max_joints > MAX_JOINTS:
        errors.append(f"joint count {max_joints} exceeds the v1 budget {MAX_JOINTS}")
    if world_bbox_min is not None and world_bbox_max is not None:
        height = world_bbox_max[1] - world_bbox_min[1]
        if height < 0.25 or height > 4.0:
            warnings.append(
                f"scene-world Y extent is {height:.6g} meters; verify root transforms, scale, and seat placement"
            )

    if require_character:
        if not skins or not skinned_primitive_count:
            errors.append("character package requires a skin and skinned primitives")
        if not animations:
            errors.append("character package requires at least one named animation")
        elif not any(animation["duration_seconds"] > 0.0 for animation in animations):
            errors.append("character package has no animation with positive duration")

    return {
        "format": "glb-2.0",
        "sha256": _sha256(data),
        "bytes": len(data),
        "self_contained": not any("external" in error for error in errors),
        "generator": document.get("asset", {}).get("generator"),
        "extensions_used": document.get("extensionsUsed", []),
        "extensions_required": document.get("extensionsRequired", []),
        "scene_count": len(_array(document, "scenes")),
        "mesh_count": len(_array(document, "meshes")),
        "primitive_count": primitive_count,
        "vertex_count": vertex_count,
        "triangle_count": triangle_count,
        "material_count": len(_array(document, "materials")),
        "texture_count": len(_array(document, "textures")),
        "skin_count": len(skins),
        "max_joints": max_joints,
        "animations": animations,
        "bbox_min": world_bbox_min,
        "bbox_max": world_bbox_max,
        "mesh_local_bbox_min": bbox_min,
        "mesh_local_bbox_max": bbox_max,
        "binary_bytes": len(bin_chunk or b""),
        "errors": errors,
        "warnings": warnings,
        "character_ready": require_character and not errors,
    }


def inspect_glb(path: Path, require_character: bool = False) -> dict[str, Any]:
    return inspect_glb_bytes(
        _read_bounded(path, MAX_INPUT_BYTES, "GLB input"),
        require_character=require_character,
    )


def validate_manifest(manifest: dict[str, Any], glb_report: dict[str, Any]) -> list[str]:
    errors = _manifest_unicode_errors(manifest)
    allowed = {
        "schema", "id", "display_name", "renderer_profile", "license",
        "animations", "gameplay", "presentation", "sockets", "model",
        "model_sha256", "license_file",
    }
    for field in sorted(set(manifest) - allowed):
        errors.append(f"manifest contains unknown field {field!r}")
    if manifest.get("schema") != PACKAGE_SCHEMA:
        errors.append(f"manifest.schema must be {PACKAGE_SCHEMA!r}")
    package_id = manifest.get("id")
    if not isinstance(package_id, str) or not ID_RE.fullmatch(package_id):
        errors.append("manifest.id must be a 2-64 character lowercase slug")
    if (not isinstance(manifest.get("display_name"), str) or
            not manifest["display_name"].strip() or
            len(manifest["display_name"]) > 96):
        errors.append("manifest.display_name is required")
    if manifest.get("renderer_profile") != "modern-skeletal-v1":
        errors.append("manifest.renderer_profile must be 'modern-skeletal-v1'")
    license_info = manifest.get("license")
    if not isinstance(license_info, dict):
        errors.append("manifest.license object is required")
    else:
        for field in sorted(set(license_info) - {"spdx", "attribution", "source_url"}):
            errors.append(f"manifest.license contains unknown field {field!r}")
        for field in ("spdx", "attribution", "source_url"):
            if not isinstance(license_info.get(field), str) or not license_info[field].strip():
                errors.append(f"manifest.license.{field} is required")
    animation_info = manifest.get("animations")
    clip_names = {animation["name"] for animation in glb_report.get("animations", [])}
    if not isinstance(animation_info, dict) or not isinstance(animation_info.get("fallback"), str):
        errors.append("manifest.animations.fallback is required")
    elif animation_info["fallback"] not in clip_names:
        errors.append("manifest.animations.fallback does not name a GLB animation")
    states = animation_info.get("states", {}) if isinstance(animation_info, dict) else {}
    if isinstance(animation_info, dict):
        for field in sorted(set(animation_info) - {"fallback", "states"}):
            errors.append(f"manifest.animations contains unknown field {field!r}")
    if not isinstance(states, dict):
        errors.append("manifest.animations.states must be an object")
    else:
        for semantic, clip in states.items():
            if (not isinstance(semantic, str) or not SEMANTIC_RE.fullmatch(semantic) or
                    not isinstance(clip, str) or clip not in clip_names):
                errors.append(f"animation mapping {semantic!r} does not name a GLB animation")
        if len(states) > 64:
            errors.append("manifest.animations.states exceeds 64 mappings")

    gameplay = manifest.get("gameplay")
    if not isinstance(gameplay, dict):
        errors.append("manifest.gameplay object is required")
    else:
        for field in sorted(set(gameplay) - {"donor", "vehicles"}):
            errors.append(f"manifest.gameplay contains unknown field {field!r}")
        donor = gameplay.get("donor")
        if donor not in GAMEPLAY_DONORS:
            errors.append(
                "manifest.gameplay.donor must name a built-in racer: "
                + ", ".join(sorted(GAMEPLAY_DONORS))
            )
        vehicles = gameplay.get("vehicles")
        if not isinstance(vehicles, list) or not vehicles:
            errors.append("manifest.gameplay.vehicles must be a non-empty array")
        elif any(vehicle not in VEHICLE_NAMES for vehicle in vehicles):
            errors.append("manifest.gameplay.vehicles contains an unsupported vehicle")
        elif len(set(vehicles)) != len(vehicles):
            errors.append("manifest.gameplay.vehicles must not contain duplicates")

    presentation = manifest.get("presentation")
    if not isinstance(presentation, dict):
        errors.append("manifest.presentation object is required")
    else:
        for field in sorted(set(presentation) - {
            "scale", "translation_m", "rotation_xyzw", "lod_bias"
        }):
            errors.append(f"manifest.presentation contains unknown field {field!r}")
        vector_fields = {
            "scale": (3, 0.001, 1000.0),
            "translation_m": (3, -1000.0, 1000.0),
            "rotation_xyzw": (4, -1.0, 1.0),
        }
        for field, (length, minimum, maximum) in vector_fields.items():
            value = presentation.get(field)
            if (
                not isinstance(value, list)
                or len(value) != length
                or any(
                    isinstance(component, bool)
                    or not isinstance(component, (int, float))
                    or not minimum <= float(component) <= maximum
                    for component in value
                )
            ):
                errors.append(
                    f"manifest.presentation.{field} must contain {length} finite bounded numbers"
                )
        rotation = presentation.get("rotation_xyzw")
        if isinstance(rotation, list) and len(rotation) == 4 and all(
            isinstance(component, (int, float)) and not isinstance(component, bool)
            for component in rotation
        ):
            length_squared = sum(float(component) ** 2 for component in rotation)
            if not 0.999 <= length_squared <= 1.001:
                errors.append("manifest.presentation.rotation_xyzw must be normalized")
        lod_bias = presentation.get("lod_bias", 0.0)
        if (
            isinstance(lod_bias, bool)
            or not isinstance(lod_bias, (int, float))
            or not -4.0 <= float(lod_bias) <= 4.0
        ):
            errors.append("manifest.presentation.lod_bias must be between -4 and 4")

    sockets = manifest.get("sockets")
    if not isinstance(sockets, dict):
        errors.append("manifest.sockets object is required")
    else:
        missing_sockets = sorted(REQUIRED_PRESENTATION_SOCKETS - set(sockets))
        if missing_sockets:
            errors.append("manifest.sockets is missing: " + ", ".join(missing_sockets))
        for semantic, node_name in sockets.items():
            if (
                not isinstance(semantic, str)
                or not SEMANTIC_RE.fullmatch(semantic)
                or not isinstance(node_name, str)
                or not node_name.strip()
            ):
                errors.append("manifest.sockets must map non-empty semantic names to node names")
        if len(sockets) > 32:
            errors.append("manifest.sockets exceeds 32 mappings")
    return errors


def _zip_entry(name: str, data: bytes) -> tuple[zipfile.ZipInfo, bytes]:
    info = zipfile.ZipInfo(name, PACKAGE_EPOCH)
    info.compress_type = zipfile.ZIP_STORED
    info.external_attr = 0o100644 << 16
    info.create_system = 3
    return info, data


def build_package(model_path: Path, manifest_path: Path, license_path: Path,
                  output_path: Path, compiled_cache_path: Path | None = None) -> dict[str, Any]:
    model = _read_bounded(model_path, MAX_INPUT_BYTES, "GLB input")
    report = inspect_glb_bytes(model, require_character=True)
    if report["errors"]:
        raise ProbeError("model is not character-ready: " + "; ".join(report["errors"]))
    try:
        manifest = json_loads_strict(
            _read_bounded(manifest_path, MAX_MANIFEST_BYTES, "manifest"),
            "manifest",
        )
    except (OSError, ProbeError, json.JSONDecodeError) as exc:
        raise ProbeError(f"cannot read manifest: {exc}") from exc
    if not isinstance(manifest, dict):
        raise ProbeError("manifest root must be an object")
    manifest_errors = validate_manifest(manifest, report)
    if manifest_errors:
        raise ProbeError("invalid manifest: " + "; ".join(manifest_errors))
    license_text = _read_bounded(license_path, MAX_LICENSE_BYTES, "license text")
    if not license_text.strip():
        raise ProbeError("license text is empty")

    canonical = dict(manifest)
    canonical["model"] = "model.glb"
    canonical["model_sha256"] = report["sha256"]
    canonical["license_file"] = "LICENSE.txt"
    members = {
        "manifest.json": _json_bytes(canonical),
        "model.glb": model,
        "LICENSE.txt": license_text,
    }
    package_members = PACKAGE_MEMBERS
    if compiled_cache_path is not None:
        compiled = _read_bounded(compiled_cache_path, MAX_INPUT_BYTES,
                                 "compiled character cache")
        if not _compiled_cache_valid(compiled):
            raise ProbeError("compiled character cache is invalid")
        members["compiled.mdkc"] = compiled
        package_members = PORTABLE_PACKAGE_MEMBERS
    output_path.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(output_path, "w", allowZip64=False) as archive:
        for name in package_members:
            info, payload = _zip_entry(name, members[name])
            archive.writestr(info, payload)
    package_bytes = output_path.read_bytes()
    return {
        "output": str(output_path),
        "bytes": len(package_bytes),
        "sha256": _sha256(package_bytes),
        "model": report,
        "portable": compiled_cache_path is not None,
    }


def _compiled_cache_valid(data: bytes) -> bool:
    if len(data) < 832:
        return False
    magic, version, header_bytes, file_bytes = struct.unpack_from("<4sIIQ", data, 0)
    if magic != b"MDKC" or version != 1 or header_bytes != 832 or file_bytes != len(data):
        return False
    expected_crc = struct.unpack_from("<I", data, 52)[0]
    return (zlib.crc32(data[header_bytes:]) & 0xFFFFFFFF) == expected_crc


def add_compiled_cache(package_path: Path, compiled: bytes,
                       output_path: Path) -> dict[str, Any]:
    verified = verify_package(package_path)
    if not verified["valid"]:
        raise ProbeError("source package is invalid: " + "; ".join(verified["errors"]))
    if not _compiled_cache_valid(compiled):
        raise ProbeError("compiled character cache is invalid")
    with zipfile.ZipFile(package_path) as source:
        members = {name: source.read(name) for name in PACKAGE_MEMBERS}
    members["compiled.mdkc"] = compiled
    output_path.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(output_path, "w", allowZip64=False) as archive:
        for name in PORTABLE_PACKAGE_MEMBERS:
            info, payload = _zip_entry(name, members[name])
            archive.writestr(info, payload)
    package_bytes = output_path.read_bytes()
    return {
        "output": str(output_path),
        "bytes": len(package_bytes),
        "sha256": _sha256(package_bytes),
        "portable": True,
        "model": verified["model"],
    }


def verify_package(path: Path) -> dict[str, Any]:
    data = _read_bounded(path, MAX_INPUT_BYTES, "character package")
    try:
        archive = zipfile.ZipFile(io.BytesIO(data))
    except zipfile.BadZipFile as exc:
        raise ProbeError(f"invalid character package: {exc}") from exc
    infos = archive.infolist()
    names = tuple(info.filename for info in infos)
    if names not in (PACKAGE_MEMBERS, PORTABLE_PACKAGE_MEMBERS):
        raise ProbeError(
            f"package members must be exactly {PACKAGE_MEMBERS!r} or "
            f"{PORTABLE_PACKAGE_MEMBERS!r}, in order"
        )
    member_caps = {
        "manifest.json": MAX_MANIFEST_BYTES,
        "model.glb": MAX_INPUT_BYTES,
        "LICENSE.txt": MAX_LICENSE_BYTES,
        "compiled.mdkc": MAX_INPUT_BYTES,
    }
    for info in infos:
        safe_name = _safe_archive_name(info.filename)
        if info.is_dir() or _zip_member_is_symlink(info):
            raise ProbeError(f"package member must be a regular file: {safe_name}")
        if info.flag_bits & 1:
            raise ProbeError(f"encrypted package member is unsupported: {safe_name}")
        if info.compress_type != zipfile.ZIP_STORED:
            raise ProbeError(f"package member must use deterministic stored encoding: {safe_name}")
        if info.file_size > member_caps[safe_name]:
            raise ProbeError(f"package member exceeds its size cap: {safe_name}")
    manifest = json_loads_strict(archive.read("manifest.json"), "manifest")
    if not isinstance(manifest, dict):
        raise ProbeError("manifest root must be an object")
    model = archive.read("model.glb")
    license_text = archive.read("LICENSE.txt")
    report = inspect_glb_bytes(model, require_character=True)
    errors = list(report["errors"])
    errors.extend(validate_manifest(manifest, report))
    if manifest.get("model_sha256") != _sha256(model):
        errors.append("manifest model_sha256 does not match model.glb")
    if not license_text.strip():
        errors.append("LICENSE.txt is empty")
    portable = names == PORTABLE_PACKAGE_MEMBERS
    if portable and not _compiled_cache_valid(archive.read("compiled.mdkc")):
        errors.append("compiled.mdkc is invalid")
    animation_info = manifest.get("animations", {})
    states = animation_info.get("states", {}) if isinstance(animation_info, dict) else {}
    sockets = manifest.get("sockets", {})
    missing_states = [semantic for semantic in RECOMMENDED_RACE_SEMANTICS
                      if semantic not in states]
    missing_select_states = [
        semantic for semantic in RECOMMENDED_SELECT_SEMANTICS
        if semantic not in states
    ]
    author_warnings = []
    if missing_states:
        author_warnings.append(
            f"{len(missing_states)} recommended race animation states use fallback"
        )
    if missing_select_states:
        author_warnings.append(
            f"{len(missing_select_states)} recommended select animation states use fallback"
        )
    if isinstance(sockets, dict) and "hand" not in sockets:
        author_warnings.append("optional hand socket is not authored")
    if not portable:
        author_warnings.append(
            "source-only package needs the developer compiler; prepare a portable package for launcher-only import"
        )
    return {
        "format": PACKAGE_SCHEMA,
        "bytes": len(data),
        "sha256": _sha256(data),
        "id": manifest.get("id"),
        "model": report,
        "errors": errors,
        "valid": not errors,
        "portable": portable,
        "authoring": {
            "fallback_clip": animation_info.get("fallback")
                if isinstance(animation_info, dict) else None,
            "mapped_recommended_states": [semantic for semantic in RECOMMENDED_RACE_SEMANTICS
                                           if semantic in states],
            "missing_recommended_states": missing_states,
            "mapped_select_states": [
                semantic for semantic in RECOMMENDED_SELECT_SEMANTICS
                if semantic in states
            ],
            "missing_select_states": missing_select_states,
            "sockets": sorted(sockets) if isinstance(sockets, dict) else [],
            "warnings": author_warnings,
        },
    }


def _write_report(report: dict[str, Any]) -> None:
    json.dump(report, sys.stdout, indent=2, sort_keys=True)
    sys.stdout.write("\n")


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    probe = sub.add_parser("probe", help="inspect a ZIP, DAE, GLB, or character package")
    probe.add_argument("input", type=Path)
    probe.add_argument("--require-character", action="store_true")
    pack = sub.add_parser("pack", help="build a deterministic .mdkrchar source package")
    pack.add_argument("--model", required=True, type=Path)
    pack.add_argument("--manifest", required=True, type=Path)
    pack.add_argument("--license", required=True, type=Path)
    pack.add_argument("--output", required=True, type=Path)
    pack.add_argument("--compiled-cache", type=Path)
    verify = sub.add_parser("verify", help="verify an existing .mdkrchar source package")
    verify.add_argument("input", type=Path)
    return parser


def main(argv: list[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    try:
        if args.command == "pack":
            report = build_package(args.model, args.manifest, args.license,
                                   args.output, args.compiled_cache)
        elif args.command == "verify":
            report = verify_package(args.input)
        else:
            suffix = args.input.suffix.lower()
            if suffix == ".zip":
                report = inspect_archive(args.input)
            elif suffix == ".dae":
                report = inspect_dae(
                    _read_bounded(args.input, MAX_INPUT_BYTES, "COLLADA input"),
                    args.input.name,
                )
            elif suffix == ".glb":
                report = inspect_glb(args.input, require_character=args.require_character)
            elif suffix == ".mdkrchar":
                report = verify_package(args.input)
            else:
                raise ProbeError(f"unsupported input suffix: {suffix or '<none>'}")
        _write_report(report)
        if report.get("errors") or report.get("blockers") or report.get("valid") is False:
            return 2
        return 0
    except (OSError, ProbeError, zipfile.BadZipFile, json.JSONDecodeError) as exc:
        _write_report({"error": str(exc)})
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
