#!/usr/bin/env python3
"""Contract tests for the redistributable adversarial character fixture."""

from __future__ import annotations

import hashlib
import json
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import character_asset_compiler as compiler  # noqa: E402
import character_asset_probe as probe  # noqa: E402
import character_spike_fixture as fixture  # noqa: E402


class CharacterSpikeFixtureTests(unittest.TestCase):
    def test_fixture_is_deterministic_auditable_and_adversarial(self) -> None:
        model = fixture.model_glb()
        portrait = fixture.portrait_png()
        self.assertEqual(model, fixture.model_glb())
        self.assertEqual(
            "6d7ac5c22ff57070e5febb08d460021ad2dea4283527e6650570d2d93d9f9681",
            hashlib.sha256(model).hexdigest(),
        )
        report = probe.inspect_glb_bytes(model, require_character=True)
        self.assertEqual([], report["errors"])
        self.assertTrue(report["self_contained"])
        self.assertEqual(504, report["vertex_count"])
        self.assertEqual(252, report["triangle_count"])
        self.assertEqual(20, report["max_joints"])
        self.assertEqual(3, report["material_count"])
        self.assertEqual(2, report["texture_count"])
        self.assertEqual([-126, -1, -23], report["mesh_local_bbox_min"])
        self.assertEqual([126, 194, 45], report["mesh_local_bbox_max"])

        document, binary = probe.parse_glb(model)
        self.assertIsNotNone(binary)
        nodes = document["nodes"]
        self.assertEqual("Skl_Root", nodes[0]["name"])
        self.assertEqual([1, 2], nodes[0]["children"])
        self.assertEqual("Hip", nodes[1]["name"])
        self.assertEqual("Spine1", nodes[2]["name"])
        self.assertEqual([0.01, 0.01, 0.01], nodes[0]["scale"])
        self.assertEqual([0.01, 0.01, 0.01], nodes[20]["scale"])
        self.assertEqual(
            ["Hair.Root", "Hair.Mid", "Hair.Tip"],
            [nodes[index]["name"] for index in (17, 18, 19)],
        )
        self.assertEqual(3, len(document["meshes"][0]["primitives"]))
        self.assertEqual("MASK", document["materials"][1]["alphaMode"])
        self.assertEqual("FaceDetails", document["materials"][2]["name"])
        self.assertEqual(0.01, document["asset"]["extras"]["declaredUnitMeters"])

        source = fixture.manifest(portrait)
        self.assertEqual([], probe.validate_manifest(source, report))
        self.assertEqual("+z", source["presentation"]["source_forward"])
        self.assertEqual("Skl_Root", source["rig"]["roles"]["hips"]["node"])
        self.assertEqual(
            probe.BIND_POSE_FALLBACK,
            source["animations"]["fallback"],
        )
        self.assertEqual({}, source["animations"]["states"])
        compiled, compile_report = compiler.compile_character(
            model, source, bytes(range(32)), portrait
        )
        self.assertGreater(len(compiled), 0)
        self.assertEqual(
            [probe.BIND_POSE_FALLBACK],
            compile_report["static_animations"],
        )
        self.assertEqual(0, compile_report["motion_channels"])
        self.assertEqual([], compile_report["disabled_semantics"])
        self.assertEqual(1, compile_report["semantic_mask"])
        self.assertEqual(0, compile_report["disabled_semantic_mask"])
        self.assertEqual(0xFFFF, compile_report["rig_role_mask"])

    def test_materialization_is_bounded_and_contains_no_absolute_paths(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            destination = Path(temporary) / "fixture"
            report = fixture.write_fixture(destination)
            self.assertEqual(
                {"LICENSE.txt", "manifest.json", "model.glb", "portrait.png"},
                set(report["files"]),
            )
            self.assertEqual(
                (ROOT / "tests" / "fixtures" /
                 "custom_character_adversarial" / "LICENSE.txt").read_text(
                    encoding="utf-8"
                ),
                (destination / "LICENSE.txt").read_text(encoding="utf-8"),
            )
            encoded = json.dumps(report, sort_keys=True)
            self.assertNotIn(str(destination), encoded)
            for name, record in report["files"].items():
                payload = (destination / name).read_bytes()
                self.assertEqual(len(payload), record["bytes"])
                self.assertEqual(hashlib.sha256(payload).hexdigest(),
                                 record["sha256"])


if __name__ == "__main__":
    unittest.main()
