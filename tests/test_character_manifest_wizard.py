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
from test_character_asset_probe import make_animated_glb, make_portrait_png  # noqa: E402


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


if __name__ == "__main__":
    unittest.main()
