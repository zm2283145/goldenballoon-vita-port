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
from test_character_asset_probe import make_animated_glb  # noqa: E402


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
            self.assertEqual("idle", decisions["fallback"])
            self.assertEqual("idle", manifest["animations"]["states"]["select.idle"])
            self.assertEqual(
                ["select.hover", "select.confirm"],
                decisions["missing_select_states"],
            )
            self.assertEqual({"seat": "root", "head": "head"}, manifest["sockets"])


if __name__ == "__main__":
    unittest.main()
