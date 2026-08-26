#!/usr/bin/env python3
"""ROM-free transactional install tests for private custom characters."""

from __future__ import annotations

import json
import hashlib
import sys
import tempfile
import unittest
import zipfile
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "tests"))

import character_asset_probe as probe  # noqa: E402
import character_package_manager as manager  # noqa: E402
from test_character_asset_probe import (  # noqa: E402
    make_animated_glb,
    make_humanoid_glb,
    make_manifest,
    make_portrait_png,
    make_v4_manifest,
)


class CharacterPackageManagerTests(unittest.TestCase):
    def make_package(self, root: Path,
                     manifest_data: dict[str, object] | None = None,
                     model_data: bytes | None = None) -> Path:
        model = root / "model.glb"
        manifest = root / "manifest.json"
        license_file = root / "LICENSE.txt"
        package = root / "fixture.mdkrchar"
        portrait = root / "portrait.png"
        portrait_path = None
        model.write_bytes(model_data or make_animated_glb())
        manifest.write_text(
            json.dumps(manifest_data or make_manifest()), encoding="utf-8"
        )
        license_file.write_text("CC0-1.0 test fixture\n", encoding="utf-8")
        if (manifest_data or {}).get("schema") in (
            probe.PACKAGE_SCHEMA_V3, probe.PACKAGE_SCHEMA_V4
        ):
            portrait.write_bytes(make_portrait_png())
            portrait_path = portrait
        probe.build_package(
            model, manifest, license_file, package,
            portrait_path=portrait_path,
        )
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

    def test_disable_preserves_history_updates_and_workshop_revisions(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            package = self.make_package(root)
            installed = root / "characters"
            first = manager.install(package, installed)
            package_id = first["id"]

            disabled = manager.set_enabled(package_id, installed, False)
            self.assertFalse(disabled["enabled"])
            self.assertFalse((installed / f"{package_id}.mdkc").exists())
            disabled_cache = installed / f"{package_id}.mdkc.disabled"
            self.assertTrue(disabled_cache.is_file())
            current = [
                entry for entry in manager.list_installed(installed)["entries"]
                if entry["active"]
            ]
            self.assertEqual(1, len(current))
            self.assertFalse(current[0]["enabled"])

            updated = manager.install(package, installed)
            self.assertFalse(updated["enabled"])
            self.assertEqual(disabled_cache.name, updated["cache_file"])
            with self.assertRaisesRegex(manager.ManagerError, "already disabled"):
                manager.set_enabled(package_id, installed, False)

            portrait = root / "replacement.png"
            portrait.write_bytes(make_portrait_png(17))
            revised = manager.revise_identity(
                package_id, portrait, (17, 34, 51), installed
            )
            self.assertFalse(revised["enabled"])
            self.assertTrue(disabled_cache.is_file())
            self.assertEqual(2, len(list(installed.glob("*.mdkrchar"))))
            self.assertEqual(2, len(list(installed.glob("*.json"))))

            enabled = manager.set_enabled(package_id, installed, True)
            self.assertTrue(enabled["enabled"])
            self.assertTrue((installed / f"{package_id}.mdkc").is_file())
            self.assertFalse(disabled_cache.exists())
            current = [
                entry for entry in manager.list_installed(installed)["entries"]
                if entry["active"]
            ]
            self.assertEqual(1, len(current))
            self.assertTrue(current[0]["enabled"])
            manager.set_enabled(package_id, installed, False)
            removed = manager.remove(package_id, installed)
            self.assertEqual(5, len(removed["removed"]))
            self.assertFalse(disabled_cache.exists())
            self.assertEqual([], list(installed.glob("*.mdkrchar")))
            self.assertEqual([], list(installed.glob("*.json")))

    def test_ambiguous_cache_state_never_updates_or_changes_state(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            package = self.make_package(root)
            installed = root / "characters"
            report = manager.install(package, installed)
            package_id = report["id"]
            active = installed / f"{package_id}.mdkc"
            disabled = installed / f"{package_id}.mdkc.disabled"
            disabled.write_bytes(active.read_bytes())
            before = active.read_bytes()
            with self.assertRaisesRegex(manager.ManagerError, "both enabled and disabled"):
                manager.install(package, installed)
            with self.assertRaisesRegex(manager.ManagerError, "both enabled and disabled"):
                manager.set_enabled(package_id, installed, False)
            self.assertEqual(before, active.read_bytes())
            self.assertEqual(before, disabled.read_bytes())

    def test_partial_deletion_fails_visible_with_completed_scope(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            installed = root / "characters"
            report = manager.install(self.make_package(root), installed)
            witness = installed / (report["id"] + "." + "b" * 64 + ".json")
            witness.mkdir()
            with self.assertRaisesRegex(
                    manager.ManagerError,
                    r"partial deletion removed 3 owned file\(s\).+1 could not"):
                manager.remove(report["id"], installed)
            self.assertTrue(witness.is_dir())
            self.assertFalse((installed / f"{report['id']}.mdkc").exists())

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

    def test_candidate_inspection_is_exact_and_mutation_free(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = self.make_package(root)
            before = {path.name for path in root.iterdir()}
            inspected = manager.inspect(source)
            self.assertEqual(before, {path.name for path in root.iterdir()})
            self.assertEqual("inspect", inspected["action"])
            self.assertEqual("org.example.pipeline-proof", inspected["id"])
            self.assertEqual("Pipeline Proof", inspected["display_name"])
            self.assertEqual(3, inspected["report"]["vertices"])
            self.assertEqual(1, inspected["report"]["triangles"])
            self.assertEqual(2, inspected["report"]["joints"])
            self.assertEqual(7, inspected["report"]["vehicle_mask"])
            self.assertFalse(inspected["portable"])
            character_dir = root / "characters"
            index_path = character_dir / ".launcher-character-candidate.tsv"
            manager.write_candidate_index(
                source, character_dir, index_path
            )
            index_lines = index_path.read_text(encoding="ascii").splitlines()
            self.assertEqual("mdkr-character-candidate-v1", index_lines[0])
            fields = index_lines[1].split("\t")
            self.assertEqual(36, len(fields))
            self.assertEqual(inspected["id"], fields[0])
            self.assertEqual(inspected["display_name"], bytes.fromhex(
                fields[1]
            ).decode("utf-8"))
            self.assertEqual(inspected["source_sha256"], fields[2])
            self.assertEqual(inspected["cache_source_digest"], fields[3])
            self.assertEqual("9", fields[4])
            self.assertEqual("7", fields[5])
            self.assertEqual(
                inspected["report"]["animation_channels"], int(fields[16])
            )
            self.assertEqual(
                inspected["report"]["animation_keys"], int(fields[17])
            )
            self.assertEqual(
                inspected["report"]["lod_vertices"],
                [int(value) for value in fields[24:28]],
            )
            self.assertEqual(
                inspected["report"]["lod_triangles"],
                [int(value) for value in fields[28:32]],
            )
            self.assertEqual(
                inspected["report"]["lod_primitives"],
                [int(value) for value in fields[32:36]],
            )
            with self.assertRaisesRegex(manager.ManagerError, "exact file"):
                manager.write_candidate_index(
                    source, character_dir, root / "candidate.tsv"
                )

            portable = root / "portable.mdkrchar"
            manager.prepare(source, portable)
            portable_inspection = manager.inspect(portable)
            self.assertTrue(portable_inspection["portable"])
            self.assertEqual(
                inspected["cache_source_digest"],
                portable_inspection["cache_source_digest"],
            )
            self.assertEqual(
                inspected["compiled_sha256"],
                portable_inspection["compiled_sha256"],
            )

    def test_candidate_inspection_rejects_oversized_source_before_read(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            package = Path(temporary) / "oversized.mdkrchar"
            with package.open("wb") as output:
                output.truncate(probe.MAX_INPUT_BYTES + 1)
            with self.assertRaisesRegex(manager.ManagerError, "exceeds"):
                manager.inspect(package)

    def test_reviewed_install_binds_candidate_and_installed_base(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            first_dir = root / "first"
            first_dir.mkdir()
            first_package = self.make_package(first_dir)
            first_review = manager.inspect(first_package)
            installed = root / "characters"
            first = manager.install_reviewed(
                first_package, installed, first_review["source_sha256"], "absent"
            )
            self.assertEqual("install-reviewed", first["action"])

            update_dir = root / "update"
            update_dir.mkdir()
            update_manifest = make_manifest()
            update_manifest["display_name"] = "Reviewed Update"
            update_package = self.make_package(update_dir, update_manifest)
            update_review = manager.inspect(update_package)
            cache = installed / f"{first['id']}.mdkc"
            before = cache.read_bytes()

            changed_dir = root / "changed"
            changed_dir.mkdir()
            changed_manifest = make_manifest()
            changed_manifest["display_name"] = "Changed After Review"
            changed_package = self.make_package(changed_dir, changed_manifest)
            reviewed_bytes = update_package.read_bytes()
            update_package.write_bytes(changed_package.read_bytes())
            with self.assertRaisesRegex(manager.ManagerError, "changed after review"):
                manager.install_reviewed(
                    update_package, installed,
                    update_review["source_sha256"],
                    first["cache_source_digest"],
                )
            self.assertEqual(before, cache.read_bytes())

            update_package.write_bytes(reviewed_bytes)
            concurrent_dir = root / "concurrent"
            concurrent_dir.mkdir()
            concurrent_manifest = make_manifest()
            concurrent_manifest["display_name"] = "Concurrent Revision"
            concurrent = manager.install(
                self.make_package(concurrent_dir, concurrent_manifest), installed
            )
            concurrent_bytes = cache.read_bytes()
            with self.assertRaisesRegex(manager.ManagerError, "changed after review"):
                manager.install_reviewed(
                    update_package, installed,
                    update_review["source_sha256"],
                    first["cache_source_digest"],
                )
            self.assertEqual(concurrent_bytes, cache.read_bytes())

            committed = manager.install_reviewed(
                update_package, installed,
                update_review["source_sha256"],
                concurrent["cache_source_digest"],
            )
            self.assertEqual("Reviewed Update", committed["display_name"])

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

    def test_arbitrary_revision_restore_and_no_overwrite_export(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            installed = root / "characters"
            original = manager.install(self.make_package(root), installed)
            portrait = root / "portrait-new.png"
            portrait.write_bytes(make_portrait_png(23))
            revised = manager.revise_identity(
                original["id"], portrait, (23, 46, 69), installed
            )
            history = manager.list_revisions(original["id"], installed)
            self.assertEqual(2, len(history["revisions"]))
            self.assertEqual(
                revised["source_sha256"],
                history["revisions"][0]["source_sha256"],
            )
            self.assertTrue(history["revisions"][0]["active"])
            index_path = installed / ".launcher-character-revisions.tsv"
            index_report = manager.write_revision_index(
                original["id"], installed, index_path
            )
            self.assertEqual(2, index_report["indexed_revisions"])
            index_lines = index_path.read_text(encoding="ascii").splitlines()
            self.assertEqual(
                "mdkr-character-revisions-v1\t2\t2", index_lines[0]
            )
            self.assertTrue(index_lines[1].startswith(
                revised["source_sha256"] + "\t1\t1\t"
            ))
            with self.assertRaisesRegex(manager.ManagerError, "exact file"):
                manager.write_revision_index(
                    original["id"], installed, root / "unsafe.tsv"
                )

            manager.set_enabled(original["id"], installed, False)
            restored = manager.restore_revision(
                original["id"], original["source_sha256"], installed
            )
            self.assertEqual("restore-revision", restored["action"])
            self.assertFalse(restored["enabled"])
            current = [
                row for row in manager.list_revisions(
                    original["id"], installed
                )["revisions"] if row["active"]
            ]
            self.assertEqual(
                original["source_sha256"], current[0]["source_sha256"]
            )

            exported = root / "exported-revision.mdkrchar"
            export_report = manager.export_revision(
                original["id"], revised["source_sha256"], installed, exported
            )
            self.assertEqual(
                revised["source_sha256"], export_report["source_sha256"]
            )
            self.assertEqual(
                revised["source_sha256"],
                hashlib.sha256(exported.read_bytes()).hexdigest(),
            )
            with self.assertRaisesRegex(manager.ManagerError, "already exists"):
                manager.export_revision(
                    original["id"], revised["source_sha256"], installed,
                    exported,
                )
            retained = installed / revised["source_file"]
            retained.write_bytes(b"tampered retained source")
            with self.assertRaisesRegex(manager.ManagerError, "provenance"):
                manager.export_revision(
                    original["id"], revised["source_sha256"], installed,
                    root / "must-not-exist.mdkrchar",
                )
            self.assertFalse((root / "must-not-exist.mdkrchar").exists())
            with self.assertRaisesRegex(manager.ManagerError, "64 lowercase hex"):
                manager.restore_revision(
                    original["id"], "NOT-A-DIGEST", installed
                )

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

    def test_revisions_preserve_source_v4_rig_contract(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            installed = root / "characters"
            source = make_v4_manifest(make_portrait_png())
            original = manager.install(self.make_package(root, source), installed)
            replacement = root / "replacement.png"
            replacement.write_bytes(make_portrait_png(24))
            manager.revise_identity(
                original["id"], replacement, (11, 22, 33), installed
            )
            manager.revise_profile(
                original["id"], "banjo", ("car",), installed
            )
            with zipfile.ZipFile(self.active_source(installed)) as archive:
                manifest = probe.json_loads_strict(archive.read("manifest.json"))
            self.assertEqual(probe.PACKAGE_SCHEMA_V4, manifest["schema"])
            self.assertEqual("authored-clips-only", manifest["rig"]["mode"])
            self.assertFalse(manifest["rig"]["reviewed"])
            self.assertEqual({}, manifest["rig"]["roles"])

    def test_rig_revision_upgrades_v3_and_preserves_all_source_media(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            installed = root / "characters"
            portrait = make_portrait_png()
            source = make_v4_manifest(portrait, humanoid=True)
            rig = source.pop("rig")
            source["schema"] = probe.PACKAGE_SCHEMA_V3
            original = manager.install(
                self.make_package(root, source, make_humanoid_glb()), installed
            )
            draft = root / "rig-draft.json"
            rig["reviewed"] = False
            rig["roles"]["upper_arm.left"]["inferred"] = True
            rig["roles"]["upper_arm.left"]["confidence"] = 0.875
            draft.write_text(json.dumps({
                "schema": "mdkr-character-rig-draft-v1",
                **rig,
            }), encoding="utf-8")
            revised = manager.revise_rig(original["id"], draft, installed)
            self.assertEqual("revise-rig", revised["action"])
            self.assertEqual(16, revised["rig_roles"])
            self.assertFalse(revised["rig_reviewed"])
            self.assertEqual(0xFFFF, revised["report"]["rig_role_mask"])
            with zipfile.ZipFile(self.active_source(installed)) as archive:
                manifest = probe.json_loads_strict(archive.read("manifest.json"))
                self.assertEqual(probe.PACKAGE_SCHEMA_V4, manifest["schema"])
                self.assertEqual(rig, manifest["rig"])
                self.assertEqual(portrait, archive.read("portrait.png"))
            rig["reviewed"] = True
            draft.write_text(json.dumps({
                "schema": "mdkr-character-rig-draft-v1",
                **rig,
            }), encoding="utf-8")
            reviewed = manager.revise_rig(original["id"], draft, installed)
            self.assertTrue(reviewed["rig_reviewed"])
            self.assertTrue(reviewed["report"]["rig_reviewed"])
            self.assertEqual(3, len(list(installed.glob("*.mdkrchar"))))

    def test_invalid_rig_revision_never_replaces_active_cache(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            installed = root / "characters"
            portrait = make_portrait_png()
            source = make_v4_manifest(portrait, humanoid=True)
            original = manager.install(
                self.make_package(root, source, make_humanoid_glb()), installed
            )
            cache = installed / f"{original['id']}.mdkc"
            before = cache.read_bytes()
            rig = source["rig"]
            rig["roles"]["spine"]["node"] = rig["roles"]["hips"]["node"]
            draft = root / "bad-rig-draft.json"
            draft.write_text(json.dumps({
                "schema": "mdkr-character-rig-draft-v1",
                **rig,
            }), encoding="utf-8")
            with self.assertRaises(probe.ProbeError):
                manager.revise_rig(original["id"], draft, installed)
            self.assertEqual(before, cache.read_bytes())
            draft.write_text(json.dumps({
                "schema": "mdkr-character-rig-draft-v1",
                "mode": "authored-clips-only",
                "reviewed": False,
                "roles": {},
                "surprise": True,
            }), encoding="utf-8")
            with self.assertRaisesRegex(manager.ManagerError, "unknown"):
                manager.revise_rig(original["id"], draft, installed)
            self.assertEqual(before, cache.read_bytes())

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

    def test_exact_rgba_canvas_round_trips_through_identity_revision(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            installed = root / "characters"
            original = manager.install(self.make_package(root), installed)
            rgba = bytes(
                component
                for y in range(40)
                for x in range(40)
                for component in (x * 6, y * 6, (x + y) * 3, 255)
            )
            first_png = manager._portrait_png_from_rgba(rgba)
            self.assertEqual(first_png, manager._portrait_png_from_rgba(rgba))
            report = manager.revise_identity_rgba(
                original["id"], rgba, (4, 5, 6), installed
            )
            self.assertEqual("revise-identity-rgba", report["action"])
            self.assertEqual([4, 5, 6], report["report"]["minimap_rgb"])
            with zipfile.ZipFile(self.active_source(installed)) as archive:
                portrait = archive.read("portrait.png")
            self.assertEqual(first_png, portrait)
            self.assertEqual(
                {"width": 40, "height": 40, "colour_type": 6,
                 "bytes": len(portrait)},
                probe.inspect_portrait_png(portrait),
            )
            offset = len(probe.PNG_SIGNATURE)
            compressed = bytearray()
            while offset < len(portrait):
                length = int.from_bytes(portrait[offset:offset + 4], "big")
                kind = portrait[offset + 4:offset + 8]
                payload = portrait[offset + 8:offset + 8 + length]
                if kind == b"IDAT":
                    compressed.extend(payload)
                offset += 12 + length
            scanlines = zlib.decompress(bytes(compressed))
            decoded = b"".join(
                scanlines[y * 161 + 1:(y + 1) * 161]
                for y in range(40)
            )
            self.assertEqual(rgba, decoded)

    def test_exact_rgba_canvas_rejects_wrong_size(self) -> None:
        with self.assertRaisesRegex(manager.ManagerError, "40×40"):
            manager._portrait_png_from_rgba(bytes(40 * 40 * 4 - 1))


if __name__ == "__main__":
    unittest.main()
