#!/usr/bin/env python3
"""ROM-free transactional install tests for private custom characters."""

from __future__ import annotations

import json
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "tests"))

import character_asset_probe as probe  # noqa: E402
import character_package_manager as manager  # noqa: E402
from test_character_asset_probe import make_animated_glb, make_manifest  # noqa: E402


class CharacterPackageManagerTests(unittest.TestCase):
    def make_package(self, root: Path) -> Path:
        model = root / "model.glb"
        manifest = root / "manifest.json"
        license_file = root / "LICENSE.txt"
        package = root / "fixture.mdkrchar"
        model.write_bytes(make_animated_glb())
        manifest.write_text(json.dumps(make_manifest()), encoding="utf-8")
        license_file.write_text("CC0-1.0 test fixture\n", encoding="utf-8")
        probe.build_package(model, manifest, license_file, package)
        return package

    def test_install_is_atomic_idempotent_and_removable(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            package = self.make_package(root)
            installed = root / "characters"
            first = manager.install(package, installed)
            second = manager.install(package, installed)
            self.assertEqual(first["compiled_sha256"], second["compiled_sha256"])
            self.assertEqual(manager.COMPILER_ID, second["compiler"])
            self.assertTrue((installed / "org.example.pipeline-proof.mdkc").is_file())
            listing = manager.list_installed(installed)
            active = [entry for entry in listing["entries"] if entry["active"]]
            self.assertEqual(1, len(active))
            self.assertTrue(active[0]["source_present"])
            collision = installed / (
                "org.example.pipeline-proof.other." + "a" * 64 + ".json"
            )
            collision.write_text("unrelated prefix package\n", encoding="utf-8")
            removed = manager.remove("org.example.pipeline-proof", installed)
            self.assertEqual(3, len(removed["removed"]))
            self.assertFalse((installed / "org.example.pipeline-proof.mdkc").exists())
            self.assertTrue(collision.is_file())

    def test_invalid_package_never_publishes_cache(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            bad = root / "bad.mdkrchar"
            bad.write_bytes(b"not a package")
            installed = root / "characters"
            with self.assertRaises(Exception):
                manager.install(bad, installed)
            self.assertEqual([], list(installed.glob("*.mdkc")))

    def test_prepare_builds_a_self_contained_portable_package(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = self.make_package(root)
            portable = root / "portable.mdkrchar"
            prepared = manager.prepare(source, portable)
            self.assertEqual("org.example.pipeline-proof", prepared["id"])
            verified = probe.verify_package(portable)
            self.assertTrue(verified["valid"], verified["errors"])
            self.assertTrue(verified["portable"])
            installed = root / "installed"
            report = manager.install(portable, installed)
            self.assertEqual(prepared["compiled_sha256"], report["compiled_sha256"])


if __name__ == "__main__":
    unittest.main()
