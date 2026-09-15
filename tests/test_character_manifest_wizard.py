#!/usr/bin/env python3
"""Deterministic author-manifest inference tests."""

from __future__ import annotations

import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "tests"))

import character_asset_probe as probe  # noqa: E402
import character_manifest_wizard as wizard  # noqa: E402
from test_character_asset_probe import (  # noqa: E402
    make_animated_glb, make_humanoid_glb, make_portrait_png,
    rewrite_glb_document,
)


class CharacterManifestWizardTests(unittest.TestCase):
    def test_named_fixture_gets_reviewable_semantics_and_sockets(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            model = Path(temporary) / "model.glb"
            model.write_bytes(make_animated_glb())
            manifest, decisions = wizard.build_manifest(
                model, "org.example.wizard", "Wizard Character", "CC0-1.0",
                "Generated fixture", "https://example.invalid/wizard",
                "diddy", ["car", "hovercraft", "plane"]
            )
            report = probe.inspect_glb(model, require_character=True)
            self.assertEqual([], probe.validate_manifest(manifest, report))
            self.assertEqual(probe.PACKAGE_SCHEMA, manifest["schema"])
            self.assertEqual("+z", manifest["presentation"]["source_forward"])
            self.assertEqual(1.25, manifest["presentation"]["target_height_m"])
            self.assertEqual(
                "ground", manifest["presentation"]["contexts"]["select"]["anchor"]
            )
            self.assertEqual(
                "seat", manifest["presentation"]["contexts"]["car"]["anchor"]
            )
            self.assertEqual("idle", decisions["fallback"])
            self.assertEqual("idle", manifest["animations"]["states"]["select.idle"])
            self.assertEqual(
                ["select.hover", "select.confirm"],
                decisions["missing_select_states"],
            )
            self.assertEqual({"seat": "root", "head": "head"}, manifest["sockets"])

    def test_forward_and_height_are_explicit_author_decisions(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            model = Path(temporary) / "model.glb"
            model.write_bytes(make_animated_glb())
            manifest, decisions = wizard.build_manifest(
                model, "org.example.backward", "Backward Character", "CC0-1.0",
                "Generated fixture", "https://example.invalid/backward",
                "diddy", ["car"], source_forward="-z", target_height_m=1.4
            )
            self.assertEqual("-z", manifest["presentation"]["source_forward"])
            self.assertEqual(1.4, manifest["presentation"]["target_height_m"])
            self.assertEqual("-z", decisions["source_forward"])

    def test_animationless_skin_enters_review_with_bind_fallback(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            model = Path(temporary) / "static-rig.glb"
            model.write_bytes(rewrite_glb_document(
                make_animated_glb(),
                lambda document: document.pop("animations", None),
            ))
            manifest, decisions = wizard.build_manifest(
                model, "org.example.static-rig", "Static Rig", "CC0-1.0",
                "Generated fixture", "https://example.invalid/static-rig",
                "diddy", ["car"],
            )
            self.assertEqual(
                probe.BIND_POSE_FALLBACK,
                manifest["animations"]["fallback"],
            )
            self.assertEqual({}, manifest["animations"]["states"])
            self.assertEqual(probe.BIND_POSE_FALLBACK, decisions["fallback"])
            self.assertEqual([], probe.validate_manifest(
                manifest, probe.inspect_glb(model, require_character=True)
            ))

    def test_author_can_override_required_clip_and_socket_inference(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            model = Path(temporary) / "model.glb"
            model.write_bytes(make_animated_glb())
            manifest, decisions = wizard.build_manifest(
                model, "org.example.manual", "Manual Mapping", "CC0-1.0",
                "Generated fixture", "https://example.invalid/manual",
                "diddy", ["car"], fallback_clip="idle",
                socket_overrides={"seat": "head", "head": "root"},
            )
            self.assertEqual("idle", decisions["fallback"])
            self.assertEqual(
                {"seat": "head", "head": "root"}, manifest["sockets"]
            )
            with self.assertRaisesRegex(
                    probe.ProbeError, "does not exist in the GLB"):
                wizard.build_manifest(
                    model, "org.example.invalid", "Invalid Mapping",
                    "CC0-1.0", "Generated fixture",
                    "https://example.invalid/invalid", "diddy", ["car"],
                    fallback_clip="missing",
                    socket_overrides={"seat": "root", "head": "head"},
                )

    def test_portrait_promotes_manifest_to_v3_identity(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            model = Path(temporary) / "model.glb"
            portrait = Path(temporary) / "portrait.png"
            model.write_bytes(make_animated_glb())
            portrait.write_bytes(make_portrait_png())
            manifest, decisions = wizard.build_manifest(
                model, "org.example.identity", "Identity Character", "CC0-1.0",
                "Generated fixture", "https://example.invalid/identity",
                "diddy", ["car"], portrait=portrait,
                minimap_rgb=[12, 34, 56],
            )
            self.assertEqual(probe.PACKAGE_SCHEMA_V3, manifest["schema"])
            self.assertEqual([12, 34, 56], manifest["identity"]["minimap_rgb"])
            self.assertEqual(16, decisions["identity_portrait"]["width"])
            self.assertEqual(
                [], probe.validate_manifest(
                    manifest, probe.inspect_glb(model, require_character=True)
                )
            )

    def test_humanoid_inference_emits_unreviewed_v4_roles(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            model = Path(temporary) / "model.glb"
            portrait = Path(temporary) / "portrait.png"
            model.write_bytes(make_humanoid_glb())
            portrait.write_bytes(make_portrait_png())
            manifest, decisions = wizard.build_manifest(
                model, "org.example.humanoid", "Humanoid", "CC0-1.0",
                "Generated fixture", "https://example.invalid/humanoid",
                "diddy", ["car"], portrait=portrait,
                minimap_rgb=[12, 34, 56], rig_mode="humanoid-retarget-v1",
            )
            self.assertEqual(probe.PACKAGE_SCHEMA_V4, manifest["schema"])
            self.assertFalse(manifest["rig"]["reviewed"])
            self.assertEqual(16, len(manifest["rig"]["roles"]))
            self.assertTrue(all(
                mapping["inferred"]
                for mapping in manifest["rig"]["roles"].values()
            ))
            self.assertTrue(decisions["rig"]["inference_requires_review"])
            self.assertEqual(
                [], probe.validate_manifest(
                    manifest, probe.inspect_glb(model, require_character=True)
                )
            )

    def test_humanoid_inference_repairs_sibling_pelvis_by_structure(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            model = Path(temporary) / "sibling-pelvis.glb"
            portrait = Path(temporary) / "portrait.png"

            def sibling_pelvis(document: dict) -> None:
                document["nodes"][0]["name"] = "Skl_Root"
                document["nodes"].append({
                    "name": "Hip", "translation": [0.0, 0.0, 0.0],
                })
                hip_index = len(document["nodes"]) - 1
                document["nodes"][0]["children"].append(hip_index)
                document["skins"][0]["joints"].append(hip_index)

            model.write_bytes(rewrite_glb_document(
                make_humanoid_glb(), sibling_pelvis,
            ))
            portrait.write_bytes(make_portrait_png())
            manifest, decisions = wizard.build_manifest(
                model, "org.example.sibling-pelvis", "Sibling Pelvis",
                "CC0-1.0", "Generated fixture",
                "https://example.invalid/sibling-pelvis", "diddy", ["car"],
                portrait=portrait, minimap_rgb=[12, 34, 56],
                rig_mode="humanoid-retarget-v1",
            )
            self.assertEqual(
                "Skl_Root", manifest["rig"]["roles"]["hips"]["node"]
            )
            self.assertEqual(
                1, decisions["rig"]["common_ancestor_repairs"]
            )
            self.assertIn(
                "lowest common", decisions["rig"]["role_provenance"]["hips"]
            )
            self.assertEqual([], probe.validate_manifest(
                manifest, probe.inspect_glb(model, require_character=True)
            ))


if __name__ == "__main__":
    unittest.main()
