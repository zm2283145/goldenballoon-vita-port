#!/usr/bin/env python3
"""License-clean, ROM-free coverage for the bounded COLLADA adapter."""

from __future__ import annotations

import json
import struct
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "tests"))

import character_asset_compiler as compiler  # noqa: E402
import character_asset_probe as probe  # noqa: E402
import collada_to_glb as adapter  # noqa: E402
from test_character_asset_probe import make_manifest  # noqa: E402


DAE = """<?xml version="1.0" encoding="utf-8"?>
<COLLADA xmlns="http://www.collada.org/2005/11/COLLADASchema" version="1.4.1">
  <asset><unit name="centimeter" meter="0.01"/><up_axis>Z_UP</up_axis></asset>
  <library_effects><effect id="body-fx"><profile_COMMON><technique sid="common">
    <lambert><diffuse><color>1 1 1 1</color></diffuse></lambert>
  </technique></profile_COMMON></effect></library_effects>
  <library_materials><material id="body-material" name="body">
    <instance_effect url="#body-fx"/>
  </material></library_materials>
  <library_geometries><geometry id="body-geometry"><mesh>
    <source id="positions"><float_array id="positions-array" count="9">0 0 0 100 0 0 0 0 100</float_array>
      <technique_common><accessor source="#positions-array" count="3" stride="3"/></technique_common></source>
    <source id="normals"><float_array id="normals-array" count="9">0 1 0 0 1 0 0 1 0</float_array>
      <technique_common><accessor source="#normals-array" count="3" stride="3"/></technique_common></source>
    <source id="uvs"><float_array id="uvs-array" count="6">0 0 1 0 0 1</float_array>
      <technique_common><accessor source="#uvs-array" count="3" stride="2"/></technique_common></source>
    <vertices id="vertices"><input semantic="POSITION" source="#positions"/></vertices>
    <triangles material="body-symbol" count="1">
      <input semantic="VERTEX" source="#vertices" offset="0"/>
      <input semantic="NORMAL" source="#normals" offset="1"/>
      <input semantic="TEXCOORD" source="#uvs" offset="2" set="0"/>
      <p>0 0 0 1 1 1 2 2 2</p>
    </triangles>
  </mesh></geometry></library_geometries>
  <library_controllers><controller id="body-controller"><skin source="#body-geometry">
    <bind_shape_matrix>1 0 0 0 0 1 0 0 0 0 1 0 0 0 0 1</bind_shape_matrix>
    <source id="joint-names"><Name_array id="joint-names-array" count="1">Root</Name_array>
      <technique_common><accessor source="#joint-names-array" count="1" stride="1"/></technique_common></source>
    <source id="inverse-binds"><float_array id="inverse-binds-array" count="16">1 0 0 0 0 1 0 0 0 0 1 0 0 0 0 1</float_array>
      <technique_common><accessor source="#inverse-binds-array" count="1" stride="16"/></technique_common></source>
    <source id="weights"><float_array id="weights-array" count="1">1</float_array>
      <technique_common><accessor source="#weights-array" count="1" stride="1"/></technique_common></source>
    <joints><input semantic="JOINT" source="#joint-names"/><input semantic="INV_BIND_MATRIX" source="#inverse-binds"/></joints>
    <vertex_weights count="3"><input semantic="JOINT" source="#joint-names" offset="0"/>
      <input semantic="WEIGHT" source="#weights" offset="1"/><vcount>1 1 1</vcount><v>0 0 0 0 0 0</v>
    </vertex_weights>
  </skin></controller></library_controllers>
  <library_visual_scenes><visual_scene id="scene">
    <node id="Root" sid="Root" name="root" type="JOINT"><matrix>1 0 0 0 0 1 0 0 0 0 1 0 0 0 0 1</matrix>
      <node id="Head" sid="Head" name="head" type="JOINT"><matrix>1 0 0 0 0 1 0 0 0 0 1 100 0 0 0 1</matrix></node>
    </node>
    <node id="mesh"><instance_controller url="#body-controller"><skeleton>#Root</skeleton>
      <bind_material><technique_common><instance_material symbol="body-symbol" target="#body-material"/></technique_common></bind_material>
    </instance_controller></node>
  </visual_scene></library_visual_scenes>
  <scene><instance_visual_scene url="#scene"/></scene>
</COLLADA>
"""


def glb_document(data: bytes) -> dict[str, object]:
    magic, version, length = struct.unpack_from("<4sII", data, 0)
    if magic != b"glTF" or version != 2 or length != len(data):
        raise AssertionError("adapter did not emit a GLB 2.0 container")
    chunk_length, chunk_kind = struct.unpack_from("<II", data, 12)
    if chunk_kind != probe.GLB_JSON_CHUNK:
        raise AssertionError("first GLB chunk is not JSON")
    return json.loads(data[20:20 + chunk_length])


class ColladaAdapterTests(unittest.TestCase):
    def test_dae_converts_to_compiler_ready_glb(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary) / "fixture.dae"
            source.write_text(DAE, encoding="utf-8")
            glb, report = adapter.convert(source)

        self.assertEqual(1, report["triangles"])
        self.assertEqual(1, report["joints"])
        self.assertEqual("Z_UP", report["source_up_axis"])
        self.assertEqual(0.01, report["source_unit_meter"])
        document = glb_document(glb)
        conversion = document["nodes"][-1]
        self.assertEqual([0.01, 0.01, 0.01], conversion["scale"])
        self.assertIn("rotation", conversion)

        inspected = probe.inspect_glb_bytes(glb, require_character=True)
        self.assertEqual([], inspected["errors"])
        self.assertEqual([], inspected["warnings"])
        self.assertAlmostEqual(
            1.0, inspected["bbox_max"][1] - inspected["bbox_min"][1]
        )
        manifest = make_manifest()
        compiled, compiled_report = compiler.compile_character(
            glb, manifest, bytes(range(32))
        )
        self.assertTrue(compiled.startswith(compiler.MDKC_MAGIC))
        self.assertEqual(1, compiled_report["triangles"])
        self.assertEqual(0, compiled_report["motion_channels"])
        self.assertEqual(["idle"], compiled_report["static_animations"])

    def test_authored_dae_animation_fails_closed(self) -> None:
        animated = DAE.replace(
            "<scene><instance_visual_scene",
            "<library_animations><animation><channel source=\"#sampler\" target=\"Root/matrix\"/></animation></library_animations><scene><instance_visual_scene",
        )
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary) / "animated.dae"
            source.write_text(animated, encoding="utf-8")
            with self.assertRaisesRegex(adapter.ConversionError, "animation channels"):
                adapter.convert(source)

    def test_polylist_fails_closed(self) -> None:
        unsupported = DAE.replace("<triangles material=", "<polylist material=").replace(
            "</triangles>", "</polylist>"
        )
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary) / "polylist.dae"
            source.write_text(unsupported, encoding="utf-8")
            with self.assertRaisesRegex(adapter.ConversionError, "polylist"):
                adapter.convert(source)


if __name__ == "__main__":
    unittest.main()
