#!/usr/bin/env python3
"""Create a reviewed, deterministic MSFT_lod GLB without touching the source.

The geometry decision is delegated to the separately built, pinned
``mdkr-character-lod`` helper. This module owns glTF structure, exact attribute
copying, provenance, and post-build validation. The helper receives no path and
no ROM bytes: one bounded binary request enters stdin and one result leaves
stdout.
"""

from __future__ import annotations

import copy
import json
import math
import os
from pathlib import Path
import stat
import struct
import subprocess
from typing import Any, Iterable, Sequence

import character_asset_compiler as compiler
import character_asset_probe as probe


SCHEMA = "mdkr-character-lod-result-v1"
PROTOCOL_MAGIC = b"MDKRLOD1"
PROTOCOL_VERSION = 1
MESHOPTIMIZER_VERSION = "1.2"
MESHOPTIMIZER_COMMIT = "9d9890c73011d75920af614485296d1e03e95448"
MAX_HELPER_OUTPUT = 32 * 1024 * 1024
DEFAULT_RATIOS = (0.50, 0.25, 0.125)
DEFAULT_ERROR_LIMITS = (0.010, 0.025, 0.050)
SIMPLIFY_LOCK_BORDER = 1 << 0
SIMPLIFY_REGULARIZE_LIGHT = 1 << 6
SIMPLIFY_OPTIONS = SIMPLIFY_LOCK_BORDER | SIMPLIFY_REGULARIZE_LIGHT


class LodBuildError(ValueError):
    """Actionable refusal from the offline LOD authoring boundary."""


def _integer(value: object, minimum: int = 0) -> bool:
    return isinstance(value, int) and not isinstance(value, bool) and value >= minimum


def _array(document: dict[str, Any], name: str) -> list[Any]:
    value = document.get(name, [])
    if not isinstance(value, list):
        raise LodBuildError(f"glTF {name} must be an array")
    return value


def _validate_settings(
    ratios: Sequence[float], error_limits: Sequence[float]
) -> tuple[tuple[float, ...], tuple[float, ...]]:
    if not 1 <= len(ratios) <= 3 or len(ratios) != len(error_limits):
        raise LodBuildError("LOD generation requires one to three ratio/error pairs")
    normalized_ratios: list[float] = []
    normalized_errors: list[float] = []
    previous_ratio = 1.0
    previous_error = -1.0
    for level, (ratio, error) in enumerate(zip(ratios, error_limits), start=1):
        if (
            isinstance(ratio, bool) or not isinstance(ratio, (int, float))
            or not math.isfinite(float(ratio))
            or not 0.01 <= float(ratio) < previous_ratio
        ):
            raise LodBuildError(
                f"LOD{level} triangle ratio must be finite, at least 0.01, "
                "and lower than the preceding level"
            )
        if (
            isinstance(error, bool) or not isinstance(error, (int, float))
            or not math.isfinite(float(error))
            or not 0.0 <= float(error) <= 0.25
            or float(error) < previous_error
        ):
            raise LodBuildError(
                f"LOD{level} error limit must be finite, from 0 to 0.25, "
                "and no lower than the preceding level"
            )
        normalized_ratios.append(float(ratio))
        normalized_errors.append(float(error))
        previous_ratio = float(ratio)
        previous_error = float(error)
    return tuple(normalized_ratios), tuple(normalized_errors)


def _validate_helper(path: Path) -> Path:
    if not path.is_absolute():
        raise LodBuildError("the LOD helper path must be absolute")
    try:
        details = path.lstat()
    except OSError as exc:
        raise LodBuildError("the pinned LOD helper is missing") from exc
    if not stat.S_ISREG(details.st_mode) or path.is_symlink():
        raise LodBuildError("the LOD helper must be a regular, non-symlink file")
    if os.name != "nt" and details.st_mode & 0o111 == 0:
        raise LodBuildError("the LOD helper is not executable")
    return path


def _flatten(values: Iterable[Iterable[object]], label: str) -> list[float]:
    result: list[float] = []
    for row in values:
        for value in row:
            if isinstance(value, bool) or not isinstance(value, (int, float)):
                raise LodBuildError(f"{label} contains a non-numeric component")
            number = float(value)
            if not math.isfinite(number):
                raise LodBuildError(f"{label} contains a non-finite component")
            result.append(number)
    return result


def _run_helper(
    helper: Path, positions: list[tuple[Any, ...]],
    attributes: list[tuple[Any, ...]], attribute_weights: list[float],
    indices: list[int], target_count: int, target_error: float,
) -> tuple[list[int], float]:
    vertex_count = len(positions)
    if len(attributes) != vertex_count or not attributes:
        raise LodBuildError("simplifier attribute rows do not match POSITION")
    attribute_count = len(attributes[0])
    if not 1 <= attribute_count <= 32 or any(
        len(row) != attribute_count for row in attributes
    ) or len(attribute_weights) != attribute_count:
        raise LodBuildError("simplifier attributes are not a bounded matrix")
    request = bytearray(PROTOCOL_MAGIC)
    request += struct.pack(
        "<6If", PROTOCOL_VERSION, vertex_count, len(indices),
        attribute_count, target_count, SIMPLIFY_OPTIONS, target_error,
    )
    flat_positions = _flatten(positions, "POSITION")
    flat_attributes = _flatten(attributes, "simplification attributes")
    request += struct.pack(f"<{len(flat_positions)}f", *flat_positions)
    request += struct.pack(f"<{len(flat_attributes)}f", *flat_attributes)
    request += struct.pack(f"<{attribute_count}f", *attribute_weights)
    request += struct.pack(f"<{len(indices)}I", *indices)
    try:
        completed = subprocess.run(
            [str(helper)], input=request, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, timeout=120, check=False,
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise LodBuildError(
            "the pinned LOD helper could not complete within 120 seconds"
        ) from exc
    diagnostic = completed.stderr[:4096].decode("utf-8", "replace").strip()
    if completed.returncode != 0:
        raise LodBuildError(
            "LOD simplification failed" + (f": {diagnostic}" if diagnostic else "")
        )
    output = completed.stdout
    if len(output) < 20 or len(output) > MAX_HELPER_OUTPUT:
        raise LodBuildError("the LOD helper returned an invalid result size")
    if output[:8] != PROTOCOL_MAGIC:
        raise LodBuildError("the LOD helper returned an invalid result header")
    version, result_count, result_error = struct.unpack_from("<IIf", output, 8)
    if (
        version != PROTOCOL_VERSION or result_count < 3
        or result_count > len(indices) or result_count % 3
        or len(output) != 20 + result_count * 4
        or not math.isfinite(result_error) or result_error < 0.0
    ):
        raise LodBuildError("the LOD helper returned invalid bounded metadata")
    result = list(struct.unpack_from(f"<{result_count}I", output, 20))
    if any(index >= vertex_count for index in result):
        raise LodBuildError("the LOD helper returned an out-of-range index")
    return result, float(result_error)


def _accessor_element_bytes(
    document: dict[str, Any], binary: bytes, accessor_index: int,
    selected: Sequence[int],
) -> bytes:
    accessors = _array(document, "accessors")
    views = _array(document, "bufferViews")
    if not _integer(accessor_index) or accessor_index >= len(accessors):
        raise LodBuildError("a primitive attribute accessor is invalid")
    accessor = accessors[accessor_index]
    if not isinstance(accessor, dict):
        raise LodBuildError("a primitive attribute accessor is invalid")
    view_index = accessor.get("bufferView")
    if not _integer(view_index) or view_index >= len(views):
        raise LodBuildError("a primitive attribute buffer view is invalid")
    view = views[view_index]
    if not isinstance(view, dict):
        raise LodBuildError("a primitive attribute buffer view is invalid")
    component = accessor.get("componentType")
    kind = accessor.get("type")
    if component not in probe.GLTF_COMPONENT_BYTES or kind not in probe.GLTF_TYPE_COMPONENTS:
        raise LodBuildError("a primitive attribute format is unsupported")
    element_size = (
        probe.GLTF_COMPONENT_BYTES[component] * probe.GLTF_TYPE_COMPONENTS[kind]
    )
    stride = view.get("byteStride", element_size)
    count = accessor.get("count")
    if not _integer(stride, element_size) or not _integer(count, 1):
        raise LodBuildError("a primitive attribute layout is invalid")
    start = view.get("byteOffset", 0) + accessor.get("byteOffset", 0)
    if not _integer(start):
        raise LodBuildError("a primitive attribute offset is invalid")
    output = bytearray()
    for index in selected:
        if not _integer(index) or index >= count:
            raise LodBuildError("a simplified vertex exceeds its accessor")
        offset = start + index * stride
        end = offset + element_size
        if end > len(binary):
            raise LodBuildError("a primitive attribute range exceeds the GLB")
        output.extend(binary[offset:end])
    return bytes(output)


def _append_bytes(
    binary: bytearray, views: list[Any], payload: bytes, target: int,
) -> int:
    binary.extend(b"\0" * ((-len(binary)) % 4))
    offset = len(binary)
    binary.extend(payload)
    views.append({
        "buffer": 0, "byteOffset": offset,
        "byteLength": len(payload), "target": target,
    })
    return len(views) - 1


def _compact_primitive(
    document: dict[str, Any], source_binary: bytes, output_binary: bytearray,
    reader: Any, primitive: dict[str, Any], simplified: Sequence[int],
    level: int,
) -> tuple[dict[str, Any], int]:
    views = _array(document, "bufferViews")
    accessors = _array(document, "accessors")
    remap: dict[int, int] = {}
    selected: list[int] = []
    compact_indices: list[int] = []
    for old_index in simplified:
        if old_index not in remap:
            remap[old_index] = len(selected)
            selected.append(old_index)
        compact_indices.append(remap[old_index])
    attributes = primitive.get("attributes")
    if not isinstance(attributes, dict):
        raise LodBuildError("a primitive attributes object is invalid")
    compact_attributes: dict[str, int] = {}
    for semantic, accessor_index in sorted(attributes.items()):
        if not isinstance(semantic, str):
            raise LodBuildError("a primitive attribute semantic is invalid")
        source_accessor = accessors[accessor_index]
        payload = _accessor_element_bytes(
            document, source_binary, accessor_index, selected
        )
        view_index = _append_bytes(output_binary, views, payload, 34962)
        compact_accessor: dict[str, Any] = {
            "bufferView": view_index,
            "componentType": source_accessor["componentType"],
            "count": len(selected),
            "type": source_accessor["type"],
        }
        if source_accessor.get("normalized") is True:
            compact_accessor["normalized"] = True
        if semantic == "POSITION":
            position_values = reader.values(accessor_index)
            retained = [position_values[index] for index in selected]
            compact_accessor["min"] = [
                min(float(row[axis]) for row in retained) for axis in range(3)
            ]
            compact_accessor["max"] = [
                max(float(row[axis]) for row in retained) for axis in range(3)
            ]
        accessors.append(compact_accessor)
        compact_attributes[semantic] = len(accessors) - 1
    index_payload = struct.pack(f"<{len(compact_indices)}I", *compact_indices)
    index_view = _append_bytes(output_binary, views, index_payload, 34963)
    accessors.append({
        "bufferView": index_view, "componentType": 5125,
        "count": len(compact_indices), "type": "SCALAR",
        "min": [min(compact_indices)], "max": [max(compact_indices)],
    })
    compact: dict[str, Any] = {
        "attributes": compact_attributes,
        "indices": len(accessors) - 1,
        "mode": 4,
    }
    if "material" in primitive:
        compact["material"] = primitive["material"]
    if "extras" in primitive:
        compact["extras"] = copy.deepcopy(primitive["extras"])
    compact["extras"] = (
        compact["extras"] if isinstance(compact.get("extras"), dict) else {}
    )
    compact["extras"]["org.goldenballoon.lod_level"] = level
    return compact, len(selected)


def _unique_name(existing: set[str], preferred: str) -> str:
    candidate = preferred
    suffix = 2
    while candidate in existing:
        candidate = f"{preferred}_{suffix}"
        suffix += 1
    existing.add(candidate)
    return candidate


def build_lods(
    model: bytes, helper_path: Path, *,
    ratios: Sequence[float] = DEFAULT_RATIOS,
    error_limits: Sequence[float] = DEFAULT_ERROR_LIMITS,
) -> tuple[bytes, dict[str, Any]]:
    """Return a new validated GLB and exact generation report."""
    if len(model) > probe.MAX_INPUT_BYTES:
        raise LodBuildError("GLB input exceeds the 512 MiB authoring limit")
    helper = _validate_helper(helper_path)
    normalized_ratios, normalized_errors = _validate_settings(
        ratios, error_limits
    )
    try:
        initial = probe.inspect_glb_bytes(model, require_character=True)
    except probe.ProbeError as exc:
        raise LodBuildError(str(exc)) from exc
    if initial["errors"]:
        raise LodBuildError(
            "source GLB is not character-ready: " + "; ".join(initial["errors"])
        )
    document, binary_chunk = probe.parse_glb(model)
    document = copy.deepcopy(document)
    source_binary = bytes(binary_chunk or b"")
    output_binary = bytearray(source_binary)
    nodes = _array(document, "nodes")
    meshes = _array(document, "meshes")
    if any(
        isinstance(node, dict)
        and isinstance(node.get("extensions"), dict)
        and "MSFT_lod" in node["extensions"]
        for node in nodes
    ):
        raise LodBuildError(
            "this GLB already has authored LODs; preserve them instead of replacing them"
        )
    source_node_count = len(nodes)
    source_mesh_nodes = [
        index for index, node in enumerate(nodes[:source_node_count])
        if isinstance(node, dict) and "mesh" in node
    ]
    if not source_mesh_nodes:
        raise LodBuildError("the GLB has no mesh nodes to simplify")
    used_meshes = sorted({int(nodes[index]["mesh"]) for index in source_mesh_nodes})
    reader = compiler.AccessorReader(document, source_binary)

    primitive_state: dict[tuple[int, int], dict[str, Any]] = {}
    source_triangles = 0
    source_vertices = 0
    for mesh_index in used_meshes:
        mesh = meshes[mesh_index]
        primitives = mesh.get("primitives", []) if isinstance(mesh, dict) else []
        if not isinstance(primitives, list) or not primitives:
            raise LodBuildError(f"mesh[{mesh_index}] has no triangle primitives")
        for primitive_index, primitive in enumerate(primitives):
            if not isinstance(primitive, dict):
                raise LodBuildError("a mesh primitive is invalid")
            attrs = primitive.get("attributes")
            if not isinstance(attrs, dict):
                raise LodBuildError("a mesh primitive has no attributes")
            positions = reader.values(attrs.get("POSITION"))
            normals = reader.values(attrs.get("NORMAL"))
            uvs = reader.values(attrs.get("TEXCOORD_0"))
            if not positions or len(normals) != len(positions) or len(uvs) != len(positions):
                raise LodBuildError("POSITION, NORMAL, and TEXCOORD_0 must align")
            attribute_rows: list[tuple[Any, ...]] = [
                tuple(normals[index]) + tuple(uvs[index])
                for index in range(len(positions))
            ]
            attribute_weights = [1.0, 1.0, 1.0, 10.0, 10.0]
            if "TANGENT" in attrs:
                tangents = reader.values(attrs["TANGENT"])
                if len(tangents) != len(positions):
                    raise LodBuildError("TANGENT does not align with POSITION")
                attribute_rows = [
                    attribute_rows[index] + tuple(tangents[index])
                    for index in range(len(positions))
                ]
                attribute_weights.extend((0.5, 0.5, 0.5, 0.25))
            if "WEIGHTS_0" in attrs:
                weights = reader.values(attrs["WEIGHTS_0"])
                if len(weights) != len(positions):
                    raise LodBuildError("WEIGHTS_0 does not align with POSITION")
                attribute_rows = [
                    attribute_rows[index] + tuple(weights[index])
                    for index in range(len(positions))
                ]
                attribute_weights.extend((2.0, 2.0, 2.0, 2.0))
            if "JOINTS_0" in attrs:
                joints = reader.values(attrs["JOINTS_0"], as_float=False)
                if len(joints) != len(positions):
                    raise LodBuildError("JOINTS_0 does not align with POSITION")
                attribute_rows = [
                    attribute_rows[index] + tuple(joints[index])
                    for index in range(len(positions))
                ]
                # Joint identifiers are categorical rather than spatial. A
                # strong penalty prevents collapses across influence-set
                # changes; the retained vertices still preserve the exact
                # authored integer bytes below.
                attribute_weights.extend((4.0, 4.0, 4.0, 4.0))
            if "COLOR_0" in attrs:
                colors = reader.values(attrs["COLOR_0"])
                if (
                    len(colors) != len(positions) or not colors
                    or not 3 <= len(colors[0]) <= 4
                ):
                    raise LodBuildError("COLOR_0 does not align with POSITION")
                color_components = len(colors[0])
                if any(len(row) != color_components for row in colors):
                    raise LodBuildError("COLOR_0 rows do not share one format")
                attribute_rows = [
                    attribute_rows[index] + tuple(colors[index])
                    for index in range(len(positions))
                ]
                attribute_weights.extend((1.0,) * color_components)
            indices = [
                int(row[0]) for row in reader.values(
                    primitive.get("indices"), as_float=False
                )
            ]
            source_triangles += len(indices) // 3
            source_vertices += len(positions)
            primitive_state[(mesh_index, primitive_index)] = {
                "primitive": primitive,
                "positions": positions,
                "attributes": attribute_rows,
                "attribute_weights": attribute_weights,
                "source_indices": list(indices),
                "indices": list(indices),
                "cumulative_error": 0.0,
            }

    generated_meshes_by_level: list[dict[int, int]] = []
    level_reports: list[dict[str, Any]] = []
    for level, (ratio, error_limit) in enumerate(
        zip(normalized_ratios, normalized_errors), start=1
    ):
        proposed: dict[tuple[int, int], tuple[list[int], float]] = {}
        level_triangles = 0
        previous_triangles = 0
        for key, state in primitive_state.items():
            current = state["indices"]
            previous_triangles += len(current) // 3
            source_count = len(state["source_indices"])
            target = max(3, int(source_count * ratio) // 3 * 3)
            target = min(target, len(current))
            simplified, error = _run_helper(
                helper, state["positions"], state["attributes"],
                state["attribute_weights"], current, target, error_limit,
            )
            proposed[key] = (simplified, error)
            level_triangles += len(simplified) // 3
        if level_triangles >= previous_triangles:
            break

        level_meshes: dict[int, int] = {}
        level_vertices = 0
        level_max_error = 0.0
        for mesh_index in used_meshes:
            source_mesh = meshes[mesh_index]
            output_primitives: list[dict[str, Any]] = []
            for primitive_index, primitive in enumerate(source_mesh["primitives"]):
                key = (mesh_index, primitive_index)
                simplified, error = proposed[key]
                state = primitive_state[key]
                state["indices"] = simplified
                state["cumulative_error"] += error
                level_max_error = max(level_max_error, state["cumulative_error"])
                compact, compact_vertices = _compact_primitive(
                    document, source_binary, output_binary, reader,
                    primitive, simplified, level,
                )
                output_primitives.append(compact)
                level_vertices += compact_vertices
            meshes.append({
                "name": f"{source_mesh.get('name', f'mesh_{mesh_index}')}__mdkr_lod{level}",
                "primitives": output_primitives,
                "extras": {
                    "org.goldenballoon.generated_lod": {
                        "level": level, "ratio": ratio,
                        "error_limit": error_limit,
                    },
                },
            })
            level_meshes[mesh_index] = len(meshes) - 1
        generated_meshes_by_level.append(level_meshes)
        level_reports.append({
            "level": level, "requested_triangle_ratio": ratio,
            "error_limit": error_limit,
            "triangles": level_triangles, "vertices": level_vertices,
            "triangle_ratio": level_triangles / source_triangles,
            "maximum_accumulated_error": level_max_error,
        })

    if not generated_meshes_by_level:
        raise LodBuildError(
            "the conservative simplifier could not reduce this topology; "
            "preserve authored seams or export authored LODs from the DCC tool"
        )

    parents = [-1] * source_node_count
    for parent_index, node in enumerate(nodes[:source_node_count]):
        for child in node.get("children", []) if isinstance(node, dict) else []:
            if _integer(child) and child < source_node_count:
                parents[child] = parent_index
    existing_names = {
        node.get("name") for node in nodes
        if isinstance(node, dict) and isinstance(node.get("name"), str)
    }
    node_clones: dict[int, list[int]] = {}
    for node_index in source_mesh_nodes:
        source_node = nodes[node_index]
        clone_ids: list[int] = []
        for level, level_meshes in enumerate(generated_meshes_by_level, start=1):
            clone: dict[str, Any] = {
                "name": _unique_name(
                    existing_names,
                    f"{source_node.get('name', f'node_{node_index}')}__mdkr_lod{level}",
                ),
                "mesh": level_meshes[int(source_node["mesh"])],
            }
            for field in ("skin", "translation", "rotation", "scale", "matrix"):
                if field in source_node:
                    clone[field] = copy.deepcopy(source_node[field])
            nodes.append(clone)
            clone_ids.append(len(nodes) - 1)
        extensions = source_node.setdefault("extensions", {})
        if not isinstance(extensions, dict):
            raise LodBuildError(f"nodes[{node_index}].extensions must be an object")
        extensions["MSFT_lod"] = {"ids": clone_ids}
        node_clones[node_index] = clone_ids
        parent = parents[node_index]
        if parent >= 0:
            nodes[parent].setdefault("children", []).extend(clone_ids)
        else:
            for scene in _array(document, "scenes"):
                roots = scene.get("nodes", []) if isinstance(scene, dict) else []
                if isinstance(roots, list) and node_index in roots:
                    roots.extend(clone_ids)

    for animation in _array(document, "animations"):
        channels = animation.get("channels", []) if isinstance(animation, dict) else []
        if not isinstance(channels, list):
            continue
        additions: list[dict[str, Any]] = []
        for channel in channels:
            target = channel.get("target") if isinstance(channel, dict) else None
            target_node = target.get("node") if isinstance(target, dict) else None
            if target_node not in node_clones:
                continue
            for clone_id in node_clones[target_node]:
                duplicate = copy.deepcopy(channel)
                duplicate["target"]["node"] = clone_id
                additions.append(duplicate)
        channels.extend(additions)

    for field in ("extensionsUsed", "extensionsRequired"):
        extensions = document.setdefault(field, [])
        if not isinstance(extensions, list):
            raise LodBuildError(f"{field} must be an array")
        if "MSFT_lod" not in extensions:
            extensions.append("MSFT_lod")
    asset = document.get("asset")
    if not isinstance(asset, dict):
        raise LodBuildError("asset metadata is missing")
    extras = asset.setdefault("extras", {})
    if not isinstance(extras, dict):
        raise LodBuildError(
            "asset.extras must be an object before recorded LOD generation"
        )
    extras["org.goldenballoon.character_lod_v1"] = {
        "algorithm": "meshoptimizer",
        "version": MESHOPTIMIZER_VERSION,
        "commit": MESHOPTIMIZER_COMMIT,
        "options": ["lock-border", "regularize-light"],
        "attribute_metric": {
            "NORMAL": [1.0, 1.0, 1.0],
            "TEXCOORD_0": [10.0, 10.0],
            "TANGENT_if_present": [0.5, 0.5, 0.5, 0.25],
            "WEIGHTS_0_if_present": [2.0, 2.0, 2.0, 2.0],
            "JOINTS_0_if_present": [4.0, 4.0, 4.0, 4.0],
            "COLOR_0_if_present": 1.0,
        },
        "source_triangles": source_triangles,
        "source_vertices": source_vertices,
        "levels": level_reports,
    }
    document["buffers"][0]["byteLength"] = len(output_binary)
    output_binary.extend(b"\0" * ((-len(output_binary)) % 4))
    document["buffers"][0]["byteLength"] = len(output_binary)
    encoded_json = json.dumps(
        document, ensure_ascii=False, separators=(",", ":"), sort_keys=True,
    ).encode("utf-8")
    encoded_json += b" " * ((-len(encoded_json)) % 4)
    output = bytearray(struct.pack("<4sII", b"glTF", 2, 0))
    output += struct.pack("<II", len(encoded_json), probe.GLB_JSON_CHUNK)
    output += encoded_json
    output += struct.pack("<II", len(output_binary), probe.GLB_BIN_CHUNK)
    output += output_binary
    struct.pack_into("<I", output, 8, len(output))
    if len(output) > probe.MAX_INPUT_BYTES:
        raise LodBuildError("generated LOD GLB exceeds the 512 MiB authoring limit")
    verified = probe.inspect_glb_bytes(bytes(output), require_character=True)
    if verified["errors"]:
        raise LodBuildError(
            "generated LOD GLB failed independent validation: "
            + "; ".join(verified["errors"])
        )
    report = {
        "schema": SCHEMA,
        "algorithm": "meshoptimizer",
        "algorithm_version": MESHOPTIMIZER_VERSION,
        "algorithm_commit": MESHOPTIMIZER_COMMIT,
        "options": ["lock-border", "regularize-light"],
        "attribute_metric": extras[
            "org.goldenballoon.character_lod_v1"
        ]["attribute_metric"],
        "source_vertices": source_vertices,
        "source_triangles": source_triangles,
        "levels": level_reports,
        "output_bytes": len(output),
    }
    return bytes(output), report
