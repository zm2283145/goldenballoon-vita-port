#!/usr/bin/env python3
"""ROM-free transactional install tests for private custom characters."""

from __future__ import annotations

import json
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "tests"))

import character_asset_probe as probe  # noqa: E402
import character_package_manager as manager  # noqa: E402
from test_character_asset_probe import (  # noqa: E402
    make_animated_glb,
    make_manifest,
    make_portrait_png,
)


class CharacterPackageManagerTests(unittest.TestCase):
    def make_package(self, root: Path,
                     manifest_data: dict[str, object] | None = None) -> Path:
        model = root / "model.glb"
        manifest = root / "manifest.json"
        license_file = root / "LICENSE.txt"
        package = root / "fixture.mdkrchar"
        model.write_bytes(make_animated_glb())
        manifest.write_text(
            json.dumps(manifest_data or make_manifest()), encoding="utf-8"
        )
        license_file.write_text("CC0-1.0 test fixture\n", encoding="utf-8")
        probe.build_package(model, manifest, license_file, package)
        return package

    def active_source(self, installed: Path) -> Path:
        active = [entry for entry in manager.list_installed(installed)["entries"]
                  if entry["active"]]
        self.assertGreaterEqual(len(active), 1)
        report = json.loads(
            (installed / active[0]["report_file"]).read_text(encoding="utf-8")
        )
        return installed / report["source_file"]

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

    def test_identity_revision_is_deterministic_and_retains_history(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            installed = root / "characters"
            original = manager.install(self.make_package(root), installed)
            portrait = root / "portrait.png"
            portrait.write_bytes(make_portrait_png(24))
            first = manager.revise_identity(
                original["id"], portrait, (12, 34, 56), installed
            )
            second = manager.revise_identity(
                original["id"], portrait, (12, 34, 56), installed
            )
            self.assertEqual(first["source_sha256"], second["source_sha256"])
            self.assertEqual(first["compiled_sha256"], second["compiled_sha256"])
            self.assertEqual([12, 34, 56], second["report"]["minimap_rgb"])
            self.assertTrue(second["report"]["identity_portrait"])
            self.assertTrue(
                (installed / original["source_file"]).is_file(),
                "the pre-edit source remains available as revision history",
            )
            self.assertEqual(2, len(list(installed.glob("*.mdkrchar"))))
            with zipfile.ZipFile(self.active_source(installed)) as archive:
                manifest = probe.json_loads_strict(archive.read("manifest.json"))
                self.assertEqual(probe.PACKAGE_SCHEMA_V3, manifest["schema"])
                self.assertEqual([12, 34, 56], manifest["identity"]["minimap_rgb"])
                self.assertEqual(portrait.read_bytes(), archive.read("portrait.png"))

    def test_identity_revision_losslessly_migrates_uniform_v1_transform(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            legacy = make_manifest()
            legacy["schema"] = probe.PACKAGE_SCHEMA_V1
            legacy["presentation"] = {
                "scale": [1.25, 1.25, 1.25],
                "translation_m": [0.1, -0.2, 0.3],
                "rotation_xyzw": [0.0, 1.0, 0.0, 0.0],
                "lod_bias": 0.5,
            }
            installed = root / "characters"
            report = manager.install(self.make_package(root, legacy), installed)
            portrait = root / "portrait.png"
            portrait.write_bytes(make_portrait_png())
            revised = manager.revise_identity(
                report["id"], portrait, (200, 100, 50), installed
            )
            self.assertIn("losslessly", revised["migration"])
            with zipfile.ZipFile(self.active_source(installed)) as archive:
                manifest = probe.json_loads_strict(archive.read("manifest.json"))
            presentation = manifest["presentation"]
            self.assertEqual("+z", presentation["source_forward"])
            self.assertAlmostEqual(1.25, presentation["target_height_m"])
            for context in ("select", "car", "hovercraft", "plane"):
                self.assertEqual(
                    [0.1, -0.2, 0.3],
                    presentation["contexts"][context]["translation_m"],
                )
                self.assertEqual(
                    [0.0, 1.0, 0.0, 0.0],
                    presentation["contexts"][context]["rotation_xyzw"],
                )

    def test_invalid_identity_edits_never_replace_active_cache(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            installed = root / "characters"
            report = manager.install(self.make_package(root), installed)
            cache = installed / f"{report['id']}.mdkc"
            before = cache.read_bytes()
            corrupt = root / "portrait.png"
            corrupt.write_bytes(b"not a PNG")
            with self.assertRaises(probe.ProbeError):
                manager.revise_identity(
                    report["id"], corrupt, (1, 2, 3), installed
                )
            valid = root / "valid.png"
            valid.write_bytes(make_portrait_png())
            with self.assertRaises(manager.ManagerError):
                manager.revise_identity(
                    report["id"], valid, (1, -1, 3), installed
                )
            with self.assertRaises(manager.ManagerError):
                manager.install(
                    self.make_package(root), installed,
                    expected_active_digest="0" * 64,
                )
            self.assertEqual(before, cache.read_bytes())

    def test_nonuniform_v1_identity_migration_fails_visibly(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            legacy = make_manifest()
            legacy["schema"] = probe.PACKAGE_SCHEMA_V1
            legacy["presentation"] = {
                "scale": [1.0, 1.5, 1.0],
                "translation_m": [0.0, 0.0, 0.0],
                "rotation_xyzw": [0.0, 0.0, 0.0, 1.0],
                "lod_bias": 0.0,
            }
            installed = root / "characters"
            report = manager.install(self.make_package(root, legacy), installed)
            before = (installed / f"{report['id']}.mdkc").read_bytes()
            portrait = root / "portrait.png"
            portrait.write_bytes(make_portrait_png())
            with self.assertRaisesRegex(manager.ManagerError, "non-uniform"):
                manager.revise_identity(
                    report["id"], portrait, (1, 2, 3), installed
                )
            self.assertEqual(before, (installed / f"{report['id']}.mdkc").read_bytes())

    def test_stale_provenance_cannot_impersonate_active_source(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            installed = root / "characters"
            original = manager.install(self.make_package(root), installed)
            portrait = root / "portrait.png"
            portrait.write_bytes(make_portrait_png())
            revised = manager.revise_identity(
                original["id"], portrait, (10, 20, 30), installed
            )
            active_report = installed / (
                f"{original['id']}.{revised['source_sha256']}.json"
            )
            active_report.unlink()
            stale_report_path = installed / (
                f"{original['id']}.{original['source_sha256']}.json"
            )
            stale = json.loads(stale_report_path.read_text(encoding="utf-8"))
            stale["cache_source_digest"] = revised["cache_source_digest"]
            stale_report_path.write_text(json.dumps(stale), encoding="utf-8")
            cache = installed / f"{original['id']}.mdkc"
            before = cache.read_bytes()
            with self.assertRaisesRegex(manager.ManagerError, "provenance"):
                manager.revise_identity(
                    original["id"], portrait, (40, 50, 60), installed
                )
            self.assertEqual(before, cache.read_bytes())

    def test_profile_revision_changes_donor_and_expands_vehicle_contexts(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = make_manifest()
            source["gameplay"]["vehicles"] = ["car"]
            source["presentation"]["contexts"] = {
                "select": source["presentation"]["contexts"]["select"],
                "car": source["presentation"]["contexts"]["car"],
            }
            installed = root / "characters"
            original = manager.install(self.make_package(root, source), installed)
            revised = manager.revise_profile(
                original["id"], "banjo",
                ("car", "hovercraft", "plane"), installed,
            )
            self.assertEqual("banjo", revised["report"]["donor"])
            self.assertEqual(7, revised["report"]["vehicle_mask"])
            with zipfile.ZipFile(self.active_source(installed)) as archive:
                manifest = probe.json_loads_strict(archive.read("manifest.json"))
            self.assertEqual("banjo", manifest["gameplay"]["donor"])
            self.assertEqual(
                ["car", "hovercraft", "plane"],
                manifest["gameplay"]["vehicles"],
            )
            self.assertEqual(
                {"select", "car", "hovercraft", "plane"},
                set(manifest["presentation"]["contexts"]),
            )
            self.assertEqual(
                "seat",
                manifest["presentation"]["contexts"]["hovercraft"]["anchor"],
            )

    def test_profile_revision_preserves_authored_identity(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            installed = root / "characters"
            original = manager.install(self.make_package(root), installed)
            portrait = root / "portrait.png"
            portrait.write_bytes(make_portrait_png(32))
            identity = manager.revise_identity(
                original["id"], portrait, (90, 80, 70), installed
            )
            revised = manager.revise_profile(
                original["id"], "tiptup", ("hovercraft",), installed
            )
            self.assertNotEqual(identity["source_sha256"], revised["source_sha256"])
            self.assertTrue(revised["report"]["identity_portrait"])
            self.assertEqual([90, 80, 70], revised["report"]["minimap_rgb"])
            with zipfile.ZipFile(self.active_source(installed)) as archive:
                self.assertEqual(portrait.read_bytes(), archive.read("portrait.png"))

    def test_invalid_profile_revision_does_not_replace_cache(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            installed = root / "characters"
            original = manager.install(self.make_package(root), installed)
            cache = installed / f"{original['id']}.mdkc"
            before = cache.read_bytes()
            with self.assertRaises(manager.ManagerError):
                manager.revise_profile(
                    original["id"], "not-a-racer", ("car",), installed
                )
            with self.assertRaises(manager.ManagerError):
                manager.revise_profile(
                    original["id"], "diddy", (), installed
                )
            self.assertEqual(before, cache.read_bytes())


if __name__ == "__main__":
    unittest.main()
