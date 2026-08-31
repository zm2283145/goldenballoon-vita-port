#!/usr/bin/env python3
"""Convert a bounded COLLADA 1.4 skinned-triangle subset to self-contained GLB.

This adapter is intentionally narrower than Blender or Assimp. It exists to
make the standard handoff reproducible for common exported DAE fixtures:
triangles, one skin controller, a visual-scene joint hierarchy, up to four
weights, and PNG diffuse/normal textures. It converts COLLADA units and Z-up
scenes at one shared root. Unsupported constructs fail closed.
"""

from __future__ import annotations

import argparse
import json
import math
import struct
import sys
from pathlib import Path
from typing import Any
from xml.etree import ElementTree as ET


MAX_DAE_BYTES = 512 * 1024 * 1024
MAX_TEXTURE_BYTES = 128 * 1024 * 1024
MAX_VERTICES = 100_000
MAX_TRIANGLES = 100_000
MAX_JOINTS = 128


class ConversionError(ValueError):
    pass


def _numbers(text: str | None, cast=float) -> list[Any]:
    if text is None:
        return []
    try:
        return [cast(value) for value in text.split()]
    except ValueError as exc:
        raise ConversionError("COLLADA numeric array is malformed") from exc


def _transpose_matrix(values: list[float]) -> list[float]:
    if len(values) != 16 or not all(math.isfinite(value) for value in values):
        raise ConversionError("COLLADA matrix must contain 16 finite values")
    return [values[row * 4 + column] for column in range(4) for row in range(4)]


def _transform_point(matrix: list[float], value: tuple[float, float, float]) -> tuple[float, float, float]:
    x, y, z = value
    return (
        matrix[0] * x + matrix[1] * y + matrix[2] * z + matrix[3],
        matrix[4] * x + matrix[5] * y + matrix[6] * z + matrix[7],
        matrix[8] * x + matrix[9] * y + matrix[10] * z + matrix[11],
    )


def _transform_direction(matrix: list[float], value: tuple[float, float, float]) -> tuple[float, float, float]:
    x, y, z = value
    output = (
        matrix[0] * x + matrix[1] * y + matrix[2] * z,
        matrix[4] * x + matrix[5] * y + matrix[6] * z,
        matrix[8] * x + matrix[9] * y + matrix[10] * z,
    )
    length = math.sqrt(sum(component * component for component in output))
    if length < 1.0e-12:
        return (0.0, 1.0, 0.0)
    return tuple(component / length for component in output)  # type: ignore[return-value]


class GlbBuilder:
    def __init__(self) -> None:
        self.binary = bytearray()
        self.views: list[dict[str, int]] = []
        self.accessors: list[dict[str, Any]] = []

    def add(self, payload: bytes, component: int, kind: str, count: int,
            *, target: int | None = None, minimum=None, maximum=None) -> int:
        self.binary.extend(b"\0" * ((-len(self.binary)) % 4))
        offset = len(self.binary)
        self.binary.extend(payload)
        view: dict[str, int] = {
            "buffer": 0, "byteOffset": offset, "byteLength": len(payload)
        }
        if target is not None:
            view["target"] = target
        view_index = len(self.views)
        self.views.append(view)
        accessor: dict[str, Any] = {
            "bufferView": view_index, "componentType": component,
            "count": count, "type": kind,
        }
        if minimum is not None:
            accessor["min"] = minimum
        if maximum is not None:
            accessor["max"] = maximum
        self.accessors.append(accessor)
        return len(self.accessors) - 1

    def add_blob(self, payload: bytes) -> int:
        self.binary.extend(b"\0" * ((-len(self.binary)) % 4))
        offset = len(self.binary)
        self.binary.extend(payload)
        self.views.append({
            "buffer": 0, "byteOffset": offset, "byteLength": len(payload)
        })
        return len(self.views) - 1

    def finish(self, document: dict[str, Any]) -> bytes:
        self.binary.extend(b"\0" * ((-len(self.binary)) % 4))
        document["bufferViews"] = self.views
        document["accessors"] = self.accessors
        document["buffers"] = [{"byteLength": len(self.binary)}]
        encoded = json.dumps(document, separators=(",", ":"), sort_keys=True).encode("utf-8")
        encoded += b" " * ((-len(encoded)) % 4)
        output = bytearray(struct.pack("<4sII", b"glTF", 2, 0))
        output += struct.pack("<II", len(encoded), 0x4E4F534A) + encoded
        output += struct.pack("<II", len(self.binary), 0x004E4942) + self.binary
        struct.pack_into("<I", output, 8, len(output))
        return bytes(output)


def convert(path: Path) -> tuple[bytes, dict[str, Any]]:
    if path.stat().st_size > MAX_DAE_BYTES:
        raise ConversionError("COLLADA input exceeds 512 MiB")
    try:
        root = ET.fromstring(path.read_bytes())
    except ET.ParseError as exc:
        raise ConversionError(f"invalid COLLADA XML: {exc}") from exc
    namespace = root.tag.partition("}")[0].lstrip("{")
    if not namespace or not root.tag.endswith("COLLADA"):
        raise ConversionError("input is not namespaced COLLADA")
    ns = {"c": namespace}
    unit = root.find("c:asset/c:unit", ns)
    unit_meter = float(unit.get("meter", "1")) if unit is not None else 1.0
    if not math.isfinite(unit_meter) or unit_meter <= 0.0:
        raise ConversionError("COLLADA unit scale is invalid")
    up_axis = (root.findtext("c:asset/c:up_axis", default="Y_UP", namespaces=ns)
               or "Y_UP").strip()
    if up_axis not in ("Y_UP", "Z_UP"):
        raise ConversionError(f"COLLADA up axis {up_axis!r} is unsupported")

    source_animations = root.findall("c:library_animations//c:channel", ns)
    if source_animations:
        raise ConversionError(
            "COLLADA animation channels are not supported by this bounded adapter; "
            "export GLB from Blender for authored animation"
        )

    sources: dict[str, list[tuple[Any, ...]]] = {}
    for source in root.findall(".//c:source", ns):
        source_id = source.get("id")
        accessor = source.find("c:technique_common/c:accessor", ns)
        if not source_id or accessor is None:
            continue
        if source_id in sources:
            raise ConversionError(f"duplicate COLLADA source id {source_id!r}")
        stride = int(accessor.get("stride", "1"))
        count = int(accessor.get("count", "0"))
        float_array = source.find("c:float_array", ns)
        name_array = source.find("c:Name_array", ns)
        if name_array is None:
            name_array = source.find("c:IDREF_array", ns)
        values = _numbers(float_array.text) if float_array is not None else (
            name_array.text.split() if name_array is not None and name_array.text else []
        )
        if stride <= 0 or count < 0 or len(values) < count * stride:
            raise ConversionError(f"source {source_id!r} accessor exceeds its array")
        sources[source_id] = [
            tuple(values[index * stride:(index + 1) * stride])
            for index in range(count)
        ]

    controllers = root.findall("c:library_controllers/c:controller/c:skin", ns)
    if len(controllers) != 1:
        raise ConversionError("exactly one skin controller is required")
    controller = controllers[0]
    bind_shape = _numbers(controller.findtext("c:bind_shape_matrix", namespaces=ns))
    if not bind_shape:
        bind_shape = [1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0,
                      0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0]
    if len(bind_shape) != 16:
        raise ConversionError("bind-shape matrix is invalid")
    joints_element = controller.find("c:joints", ns)
    weights_element = controller.find("c:vertex_weights", ns)
    if joints_element is None or weights_element is None:
        raise ConversionError("skin controller is missing joints or weights")
    joint_source = next((item.get("source", "").lstrip("#") for item in
                         joints_element.findall("c:input", ns)
                         if item.get("semantic") == "JOINT"), "")
    bind_source = next((item.get("source", "").lstrip("#") for item in
                        joints_element.findall("c:input", ns)
                        if item.get("semantic") == "INV_BIND_MATRIX"), "")
    joint_names = [str(value[0]) for value in sources.get(joint_source, [])]
    inverse_binds = sources.get(bind_source, [])
    if not joint_names or len(joint_names) > MAX_JOINTS or len(inverse_binds) != len(joint_names):
        raise ConversionError("skin joint and inverse-bind arrays do not match")

    weight_inputs = weights_element.findall("c:input", ns)
    stride = max((int(item.get("offset", "0")) for item in weight_inputs), default=-1) + 1
    joint_offset = next((int(item.get("offset", "0")) for item in weight_inputs
                         if item.get("semantic") == "JOINT"), -1)
    weight_input = next((item for item in weight_inputs if item.get("semantic") == "WEIGHT"), None)
    if stride <= 0 or joint_offset < 0 or weight_input is None:
        raise ConversionError("skin vertex-weight inputs are unsupported")
    weight_offset = int(weight_input.get("offset", "0"))
    weight_values = sources.get(weight_input.get("source", "").lstrip("#"), [])
    vcount = _numbers(weights_element.findtext("c:vcount", namespaces=ns), int)
    packed_weights = _numbers(weights_element.findtext("c:v", namespaces=ns), int)
    cursor = 0
    skin_vertices: list[tuple[tuple[int, int, int, int], tuple[float, float, float, float]]] = []
    for influence_count in vcount:
        influences: list[tuple[int, float]] = []
        for _ in range(influence_count):
            record = packed_weights[cursor:cursor + stride]
            cursor += stride
            if len(record) != stride:
                raise ConversionError("skin vertex-weight list is truncated")
            joint = record[joint_offset]
            weight_index = record[weight_offset]
            if not 0 <= joint < len(joint_names) or not 0 <= weight_index < len(weight_values):
                raise ConversionError("skin vertex weight references an invalid value")
            influences.append((joint, float(weight_values[weight_index][0])))
        influences.sort(key=lambda value: value[1], reverse=True)
        influences = influences[:4]
        total = sum(max(0.0, value[1]) for value in influences)
        if total <= 1.0e-12:
            influences = [(0, 1.0)]
            total = 1.0
        joints = tuple([value[0] for value in influences] + [0] * (4 - len(influences)))
        weights = tuple([max(0.0, value[1]) / total for value in influences] +
                        [0.0] * (4 - len(influences)))
        skin_vertices.append((joints, weights))  # type: ignore[arg-type]
    if cursor != len(packed_weights):
        raise ConversionError("skin vertex-weight list has trailing values")

    visual_scene = root.find("c:library_visual_scenes/c:visual_scene", ns)
    if visual_scene is None:
        raise ConversionError("COLLADA visual scene is missing")
    nodes: list[dict[str, Any]] = []
    sid_to_node: dict[str, int] = {}
    root_nodes: list[int] = []

    def add_node(element: ET.Element, parent: int | None) -> None:
        if element.get("type") != "JOINT":
            return
        index = len(nodes)
        name = element.get("name") or element.get("sid") or element.get("id") or f"joint_{index}"
        matrix_values = _numbers(element.findtext("c:matrix", namespaces=ns))
        node: dict[str, Any] = {"name": name}
        if matrix_values:
            node["matrix"] = _transpose_matrix(matrix_values)
        nodes.append(node)
        sid = element.get("sid")
        if sid:
            sid_to_node[sid] = index
        if parent is None:
            root_nodes.append(index)
        else:
            nodes[parent].setdefault("children", []).append(index)
        for child in element.findall("c:node", ns):
            add_node(child, index)

    for element in visual_scene.findall("c:node", ns):
        add_node(element, None)
    try:
        joint_nodes = [sid_to_node[name] for name in joint_names]
    except KeyError as exc:
        raise ConversionError(f"joint {exc.args[0]!r} is missing from the visual scene") from exc

    geometry_id = controller.get("source", "").lstrip("#")
    geometry = None
    for candidate in root.findall("c:library_geometries/c:geometry", ns):
        if candidate.get("id") == geometry_id:
            geometry = candidate.find("c:mesh", ns)
            break
    if geometry is None:
        raise ConversionError("skin controller geometry is missing")
    unsupported_primitives = [
        child.tag.rsplit("}", 1)[-1]
        for child in geometry
        if child.tag.rsplit("}", 1)[-1] in
        {"lines", "linestrips", "polygons", "polylist", "trifans", "tristrips"}
    ]
    if unsupported_primitives:
        raise ConversionError(
            "unsupported COLLADA mesh primitives: " +
            ", ".join(sorted(set(unsupported_primitives)))
        )
    vertices_bindings: dict[str, str] = {}
    vertices = geometry.find("c:vertices", ns)
    if vertices is not None:
        for item in vertices.findall("c:input", ns):
            vertices_bindings[item.get("semantic", "")] = item.get("source", "").lstrip("#")

    image_paths: dict[str, Path] = {}
    for image in root.findall("c:library_images/c:image", ns):
        image_id = image.get("id")
        relative = image.findtext("c:init_from", namespaces=ns)
        if not image_id or not relative:
            continue
        candidate = (path.parent / relative.replace("\\", "/")).resolve()
        try:
            candidate.relative_to(path.parent.resolve())
        except ValueError as exc:
            raise ConversionError("texture path escapes the COLLADA directory") from exc
        image_paths[image_id] = candidate

    effect_images: dict[str, tuple[str | None, str | None]] = {}
    for effect in root.findall("c:library_effects/c:effect", ns):
        params: dict[str, str] = {}
        for param in effect.findall("c:profile_COMMON/c:newparam", ns):
            surface = param.find("c:surface/c:init_from", ns)
            sampler = param.find("c:sampler2D/c:source", ns)
            if surface is not None and surface.text:
                params[param.get("sid", "")] = surface.text
            elif sampler is not None and sampler.text:
                params[param.get("sid", "")] = sampler.text
        diffuse = effect.find(".//c:diffuse/c:texture", ns)
        bump = effect.find(".//c:bump/c:texture", ns)
        def resolve(texture: ET.Element | None) -> str | None:
            key = texture.get("texture", "") if texture is not None else ""
            surface = params.get(key, key)
            return params.get(surface, surface) or None
        effect_images[effect.get("id", "")] = (resolve(diffuse), resolve(bump))

    material_effects: dict[str, str] = {}
    material_names: dict[str, str] = {}
    for material in root.findall("c:library_materials/c:material", ns):
        material_id = material.get("id", "")
        instance = material.find("c:instance_effect", ns)
        material_effects[material_id] = instance.get("url", "").lstrip("#") if instance is not None else ""
        material_names[material_id] = material.get("name") or material_id

    symbol_materials: dict[str, str] = {}
    controller_id = ""
    for candidate in root.findall("c:library_controllers/c:controller", ns):
        if candidate.find("c:skin", ns) is controller:
            controller_id = candidate.get("id", "")
            break
    for instance in visual_scene.findall(".//c:instance_controller", ns):
        if instance.get("url", "").lstrip("#") != controller_id:
            continue
        for binding in instance.findall(
                "c:bind_material/c:technique_common/c:instance_material", ns):
            symbol = binding.get("symbol", "")
            target = binding.get("target", "").lstrip("#")
            if symbol and target:
                symbol_materials[symbol] = target

    builder = GlbBuilder()
    images: list[dict[str, Any]] = []
    textures: list[dict[str, Any]] = []
    texture_by_image: dict[str, int] = {}
    def texture_for(image_id: str | None) -> int | None:
        if not image_id:
            return None
        if image_id in texture_by_image:
            return texture_by_image[image_id]
        image_path = image_paths.get(image_id)
        if image_path is None or not image_path.is_file() or image_path.stat().st_size > MAX_TEXTURE_BYTES:
            raise ConversionError(f"PNG texture {image_id!r} is missing or too large")
        payload = image_path.read_bytes()
        if not payload.startswith(b"\x89PNG\r\n\x1a\n"):
            raise ConversionError(f"texture {image_path.name!r} is not PNG")
        image_index = len(images)
        images.append({"name": image_id, "mimeType": "image/png",
                       "bufferView": builder.add_blob(payload)})
        texture_index = len(textures)
        textures.append({"name": image_id, "source": image_index, "sampler": 0})
        texture_by_image[image_id] = texture_index
        return texture_index

    gltf_materials: list[dict[str, Any]] = []
    material_by_symbol: dict[str, int] = {}
    for symbol in sorted({primitive.get("material", "") for primitive in
                          geometry.findall("c:triangles", ns)}):
        material_id = symbol_materials.get(symbol, symbol)
        material_id = material_id if material_id in material_effects else next(
            (key for key in material_effects if key == symbol + "-material"), "")
        if material_id not in material_effects:
            raise ConversionError(f"material symbol {symbol!r} has no bound material")
        base_image, normal_image = effect_images.get(material_effects.get(material_id, ""), (None, None))
        material: dict[str, Any] = {
            "name": material_names.get(material_id, symbol or "material"),
            "pbrMetallicRoughness": {"metallicFactor": 0.0, "roughnessFactor": 0.8},
        }
        base_texture = texture_for(base_image)
        normal_texture = texture_for(normal_image)
        if base_texture is not None:
            material["pbrMetallicRoughness"]["baseColorTexture"] = {"index": base_texture}
        if normal_texture is not None:
            material["normalTexture"] = {"index": normal_texture}
        material_by_symbol[symbol] = len(gltf_materials)
        gltf_materials.append(material)

    primitives: list[dict[str, Any]] = []
    total_vertices = 0
    total_triangles = 0
    for primitive in geometry.findall("c:triangles", ns):
        triangle_count = int(primitive.get("count", "0"))
        if triangle_count <= 0 or total_triangles + triangle_count > MAX_TRIANGLES:
            raise ConversionError("COLLADA triangle budget is invalid or exceeded")
        inputs = primitive.findall("c:input", ns)
        input_stride = max((int(item.get("offset", "0")) for item in inputs), default=-1) + 1
        semantic_inputs: dict[str, tuple[str, int]] = {}
        for item in inputs:
            semantic = item.get("semantic", "")
            source_id = item.get("source", "").lstrip("#")
            if semantic == "VERTEX":
                source_id = vertices_bindings.get("POSITION", source_id)
                semantic = "POSITION"
            if semantic in ("POSITION", "NORMAL") or (semantic == "TEXCOORD" and item.get("set", "0") == "0"):
                semantic_inputs[semantic] = (source_id, int(item.get("offset", "0")))
        if not all(key in semantic_inputs for key in ("POSITION", "NORMAL", "TEXCOORD")):
            raise ConversionError("triangles require position, normal, and TEXCOORD set 0")
        for semantic, (source_id, _) in semantic_inputs.items():
            if source_id not in sources:
                raise ConversionError(
                    f"{semantic} references missing COLLADA source {source_id!r}"
                )
        packed = _numbers(primitive.findtext("c:p", namespaces=ns), int)
        if len(packed) != triangle_count * 3 * input_stride:
            raise ConversionError("triangle index list length does not match its declaration")
        unique: dict[tuple[int, int, int], int] = {}
        positions: list[tuple[float, float, float]] = []
        normals: list[tuple[float, float, float]] = []
        uvs: list[tuple[float, float]] = []
        joints_out: list[tuple[int, int, int, int]] = []
        weights_out: list[tuple[float, float, float, float]] = []
        indices: list[int] = []
        for offset in range(0, len(packed), input_stride):
            pidx = packed[offset + semantic_inputs["POSITION"][1]]
            nidx = packed[offset + semantic_inputs["NORMAL"][1]]
            tidx = packed[offset + semantic_inputs["TEXCOORD"][1]]
            key = (pidx, nidx, tidx)
            vertex = unique.get(key)
            if vertex is None:
                position_values = sources[semantic_inputs["POSITION"][0]]
                normal_values = sources[semantic_inputs["NORMAL"][0]]
                uv_values = sources[semantic_inputs["TEXCOORD"][0]]
                if (pidx < 0 or pidx >= len(position_values) or
                        nidx < 0 or nidx >= len(normal_values) or
                        tidx < 0 or tidx >= len(uv_values)):
                    raise ConversionError("triangle references an out-of-range vertex attribute")
                position_source = position_values[pidx]
                normal_source = normal_values[nidx]
                uv_source = uv_values[tidx]
                if pidx < 0 or pidx >= len(skin_vertices):
                    raise ConversionError("geometry position lacks skin weights")
                position = _transform_point(bind_shape, tuple(float(v) for v in position_source[:3]))
                normal = _transform_direction(bind_shape, tuple(float(v) for v in normal_source[:3]))
                vertex = len(positions)
                unique[key] = vertex
                positions.append(position)
                normals.append(normal)
                uvs.append((float(uv_source[0]), 1.0 - float(uv_source[1])))
                joints_out.append(skin_vertices[pidx][0])
                weights_out.append(skin_vertices[pidx][1])
            indices.append(vertex)
        if total_vertices + len(positions) > MAX_VERTICES:
            raise ConversionError("unified COLLADA vertices exceed the 100,000 cap")
        flat_positions = [component for value in positions for component in value]
        attributes = {
            "POSITION": builder.add(
                struct.pack(f"<{len(flat_positions)}f", *flat_positions), 5126, "VEC3", len(positions),
                target=34962,
                minimum=[min(value[axis] for value in positions) for axis in range(3)],
                maximum=[max(value[axis] for value in positions) for axis in range(3)]),
            "NORMAL": builder.add(struct.pack(f"<{len(normals) * 3}f", *(c for v in normals for c in v)),
                                  5126, "VEC3", len(normals), target=34962),
            "TEXCOORD_0": builder.add(struct.pack(f"<{len(uvs) * 2}f", *(c for v in uvs for c in v)),
                                      5126, "VEC2", len(uvs), target=34962),
            "JOINTS_0": builder.add(struct.pack(f"<{len(joints_out) * 4}H", *(c for v in joints_out for c in v)),
                                    5123, "VEC4", len(joints_out), target=34962),
            "WEIGHTS_0": builder.add(struct.pack(f"<{len(weights_out) * 4}f", *(c for v in weights_out for c in v)),
                                     5126, "VEC4", len(weights_out), target=34962),
        }
        primitives.append({
            "attributes": attributes,
            "indices": builder.add(struct.pack(f"<{len(indices)}I", *indices),
                                   5125, "SCALAR", len(indices), target=34963),
            "material": material_by_symbol[primitive.get("material", "")],
        })
        total_vertices += len(positions)
        total_triangles += triangle_count

    inverse_values = [float(value) for matrix in inverse_binds for value in _transpose_matrix([float(v) for v in matrix])]
    inverse_accessor = builder.add(struct.pack(f"<{len(inverse_values)}f", *inverse_values),
                                   5126, "MAT4", len(joint_names))
    mesh_node = len(nodes)
    nodes.append({"name": "character_mesh", "mesh": 0, "skin": 0})
    root_nodes.append(mesh_node)
    animation_node = len(nodes)
    nodes.append({"name": "animation_witness"})
    root_nodes.append(animation_node)
    conversion_root = len(nodes)
    conversion_node: dict[str, Any] = {
        "name": "collada_axis_and_unit_conversion",
        "children": root_nodes,
        "scale": [unit_meter, unit_meter, unit_meter],
    }
    if up_axis == "Z_UP":
        # -90 degrees around +X maps COLLADA +Z to glTF +Y.
        conversion_node["rotation"] = [-math.sqrt(0.5), 0.0, 0.0, math.sqrt(0.5)]
    nodes.append(conversion_node)
    idle_times = builder.add(struct.pack("<2f", 0.0, 1.0), 5126, "SCALAR", 2,
                             minimum=[0.0], maximum=[1.0])
    idle_values = builder.add(struct.pack("<6f", 0.0, 0.0, 0.0,
                                          0.0, 0.0, 0.0),
                              5126, "VEC3", 2)
    document = {
        "asset": {"version": "2.0", "generator": "mdkr-collada-adapter/1"},
        "scene": 0,
        "scenes": [{"nodes": [conversion_root]}],
        "nodes": nodes,
        "meshes": [{"name": geometry_id, "primitives": primitives}],
        "skins": [{"name": "rig", "joints": joint_nodes,
                   "skeleton": joint_nodes[0], "inverseBindMatrices": inverse_accessor}],
        "animations": [{"name": "idle", "samplers": [{
            "input": idle_times, "output": idle_values, "interpolation": "LINEAR"
        }], "channels": [{"sampler": 0, "target": {
            "node": animation_node, "path": "translation"
        }}]}],
        "materials": gltf_materials,
        "images": images,
        "textures": textures,
        "samplers": [{"magFilter": 9729, "minFilter": 9987,
                      "wrapS": 10497, "wrapT": 10497}],
    }
    output = builder.finish(document)
    return output, {
        "input": str(path), "output_bytes": len(output),
        "vertices": total_vertices, "triangles": total_triangles,
        "joints": len(joint_names), "materials": len(gltf_materials),
        "textures": len(textures), "source_unit_meter": unit_meter,
        "source_up_axis": up_axis,
        "converted_to_meters": True,
        "generated_animation": "idle",
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args(argv)
    try:
        output, report = convert(args.input)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        temporary = args.output.with_name(args.output.name + ".tmp")
        temporary.write_bytes(output)
        temporary.replace(args.output)
        report["output"] = str(args.output)
        print(json.dumps(report, indent=2, sort_keys=True))
        return 0
    except (OSError, ConversionError, KeyError, IndexError, struct.error, ValueError) as exc:
        print(json.dumps({"error": str(exc)}, indent=2, sort_keys=True))
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
