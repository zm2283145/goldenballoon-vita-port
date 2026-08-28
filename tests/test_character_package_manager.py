#!/usr/bin/env python3
"""ROM-free transactional install tests for private custom characters."""

from __future__ import annotations

import json
import hashlib
import io
import sys
import tempfile
import unittest
import zipfile
import zlib
from contextlib import redirect_stdout
from pathlib import Path
from unittest import mock

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
    make_v5_character,
    rewrite_glb_document,
)
from character_validation_fixture import accepted_validation  # noqa: E402


class CharacterPackageManagerTests(unittest.TestCase):
    def setUp(self) -> None:
        patcher = mock.patch.object(
            manager, "_validate_character_glb", side_effect=accepted_validation
        )
        self.validation = patcher.start()
        self.addCleanup(patcher.stop)

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
        if (manifest_data or {}).get("schema") in probe.IDENTITY_SCHEMAS:
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

    def test_source_v5_installs_and_prepares_as_a_portable_package(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            model, _, manifest = make_v5_character()
            package = self.make_package(root, manifest, model)
            installed = manager.install(package, root / "characters")
            self.assertEqual("mdkc-v2", installed["report"]["format"])
            self.assertEqual(1, installed["report"]["joint_constraints"])
            self.assertEqual(1, installed["report"]["secondary_chains"])
            self.assertEqual(2, installed["report"]["secondary_joints"])
            portable = root / "portable.mdkrchar"
            prepared = manager.prepare(package, portable)
            self.assertEqual("mdkc-v2", prepared["report"]["format"])
            self.assertTrue(portable.is_file())
            verified = probe.verify_package(portable)
            self.assertTrue(verified["valid"], verified["errors"])
            self.assertEqual(probe.PACKAGE_SCHEMA_V5, verified["format"])

    def test_source_v5_rig_edits_retain_only_still_bound_constraints(self) -> None:
        _, _, manifest = make_v5_character()
        original_roles = manifest["rig"]["roles"]
        revised_roles = {
            role: dict(mapping) for role, mapping in original_roles.items()
        }
        revised_roles["lower_arm.left"].pop("constraint")
        revised = {
            "mode": "humanoid-retarget-v1",
            "reviewed": False,
            "roles": revised_roles,
        }
        retained = manager._retain_v5_constraints(manifest, revised)
        self.assertIn("constraint", retained["roles"]["lower_arm.left"])
        revised_roles["lower_arm.left"]["node"] = "mixamorig:LeftHand"
        invalidated = manager._retain_v5_constraints(manifest, revised)
        self.assertNotIn("constraint", invalidated["roles"]["lower_arm.left"])
        self.assertNotIn("constraint", revised_roles["lower_arm.left"])
        revised_roles = {
            role: {
                key: value for key, value in mapping.items()
                if key != "constraint"
            }
            for role, mapping in original_roles.items()
        }
        authored_only = manager._retain_v5_constraints(manifest, {
            "mode": "authored-clips-only",
            "reviewed": False,
            "roles": revised_roles,
        })
        self.assertNotIn(
            "constraint", authored_only["roles"]["lower_arm.left"]
        )

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

    def test_raw_glb_intake_inventory_and_candidate_are_reviewable(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            model = root / "author-model.glb"
            license_file = root / "LICENSE.txt"
            characters = root / "characters"
            model.write_bytes(make_animated_glb())
            license_file.write_text(
                "Creative Commons test license\n", encoding="utf-8"
            )
            inventory = manager.inspect_raw_glb(model)
            self.assertEqual("mdkr-character-glb-intake-v3", inventory["schema"])
            self.assertEqual(1, inventory["lod_levels"])
            self.assertEqual("idle", inventory["fallback"])
            self.assertEqual("root", inventory["seat"])
            self.assertEqual("head", inventory["head"])
            self.assertEqual(["idle"], inventory["clips"])
            self.assertIn("root", inventory["nodes"])

            index = characters / manager.RAW_INTAKE_INDEX_NAME
            indexed = manager.write_raw_glb_index(model, characters, index)
            self.assertEqual(inventory["model_sha256"], indexed["model_sha256"])
            lines = index.read_text(encoding="ascii").splitlines()
            self.assertTrue(lines[0].startswith(
                "mdkr-character-glb-intake-v3\t" + inventory["model_sha256"]
            ))
            self.assertTrue(lines[0].endswith("\t1"))
            self.assertEqual("defaults\t69646c65\t726f6f74\t68656164", lines[1])
            with self.assertRaisesRegex(manager.ManagerError, "exact file"):
                manager.write_raw_glb_index(
                    model, characters, root / "outside.tsv"
                )

            report = manager.build_raw_glb_candidate(
                model, license_file, "org.example.raw-intake", "Raw Intake",
                "CC-BY-4.0", "Fixture Artist",
                "https://example.invalid/raw-intake", "diddy",
                ("car", "plane"), "-z", 1.4, "idle", "root", "head",
                characters, expected_model_sha256=inventory["model_sha256"],
            )
            candidate = Path(report["candidate"])
            self.assertEqual(manager.RAW_INTAKE_CANDIDATE_NAME, candidate.name)
            verification = probe.verify_package(candidate)
            self.assertTrue(verification["valid"], verification["errors"])
            with zipfile.ZipFile(candidate) as archive:
                manifest = probe.json_loads_strict(archive.read("manifest.json"))
                self.assertEqual("org.example.raw-intake", manifest["id"])
                self.assertEqual("-z", manifest["presentation"]["source_forward"])

                self.assertEqual(1.4, manifest["presentation"]["target_height_m"])
                self.assertEqual(["car", "plane"], manifest["gameplay"]["vehicles"])
                self.assertEqual(
                    license_file.read_bytes(), archive.read("LICENSE.txt")
                )
            inspected = manager.inspect(candidate)
            self.assertEqual("Fixture Artist", inspected["attribution"])
            self.assertFalse(inspected["portable"])
            installed = manager.install_reviewed(
                candidate, characters, inspected["source_sha256"], "absent"
            )
            self.assertEqual("install-reviewed", installed["action"])
            self.assertTrue(
                (characters / "org.example.raw-intake.mdkc").is_file()
            )
            with self.assertRaisesRegex(
                    manager.ManagerError, "changed after inspection"):
                manager.build_raw_glb_candidate(
                    model, license_file, "org.example.raw-intake", "Raw Intake",
                    "CC-BY-4.0", "Fixture Artist",
                    "https://example.invalid/raw-intake", "diddy", ("car",),
                    "+z", 1.25, "idle", "root", "head", characters,
                    expected_model_sha256="0" * 64,
                )

            def encode(value: str) -> str:
                return value.encode("utf-8").hex()
            with redirect_stdout(io.StringIO()):
                status = manager.main([
                    "--directory", str(characters), "build-raw-glb",
                    encode(str(model)), encode(str(license_file)),
                    encode("org.example.raw-cli"), encode("Raw CLI"),
                    encode("CC-BY-4.0"), encode("Fixture Artist"),
                    encode("https://example.invalid/raw-cli"),
                    encode("diddy"), encode("+z"), encode("idle"),
                    encode("root"), encode("head"),
                    inventory["model_sha256"], "7", "1.25",
                ])
            self.assertEqual(0, status)
            with zipfile.ZipFile(
                    characters / manager.RAW_INTAKE_CANDIDATE_NAME) as archive:
                manifest = probe.json_loads_strict(archive.read("manifest.json"))
            self.assertEqual("org.example.raw-cli", manifest["id"])

    def test_lod_generation_is_digest_bound_exclusive_and_profile_recorded(
            self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "source.glb"
            output = root / "source-lod.glb"
            helper = root / "mdkr-character-lod"
            payload = make_animated_glb()
            source.write_bytes(payload)
            helper.write_bytes(b"fixture helper")
            helper.chmod(0o700)
            digest = hashlib.sha256(payload).hexdigest()
            generation = {
                "schema": "mdkr-character-lod-result-v1",
                "levels": [],
            }
            with mock.patch.object(
                    manager.lod_builder, "build_lods",
                    return_value=(payload, generation)) as build:
                report = manager.generate_lod_glb(
                    source, output, digest, "balanced", helper.resolve()
                )
            self.assertEqual(payload, source.read_bytes())
            self.assertEqual(payload, output.read_bytes())
            self.assertEqual("balanced", report["profile"])
            self.assertEqual(generation, report["generation"])
            build.assert_called_once_with(
                payload, helper.resolve(),
                ratios=manager.LOD_PROFILES["balanced"][0],
                error_limits=manager.LOD_PROFILES["balanced"][1],
            )
            with self.assertRaisesRegex(manager.ManagerError, "already exists"):
                manager.generate_lod_glb(
                    source, output, digest, "balanced", helper.resolve()
                )
            with self.assertRaisesRegex(manager.ManagerError, "changed after"):
                manager.generate_lod_glb(
                    source, root / "other.glb", "0" * 64,
                    "quality", helper.resolve(),
                )

    def test_animationless_raw_glb_builds_without_a_fabricated_source_clip(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            model = root / "static-rig.glb"
            license_file = root / "LICENSE.txt"
            characters = root / "characters"
            model.write_bytes(rewrite_glb_document(
                make_animated_glb(),
                lambda document: document.pop("animations", None),
            ))
            license_file.write_text("CC0 fixture\n", encoding="utf-8")
            inventory = manager.inspect_raw_glb(model)
            self.assertEqual(0, inventory["source_animation_count"])
            self.assertEqual([probe.BIND_POSE_FALLBACK], inventory["clips"])
            self.assertEqual(probe.BIND_POSE_FALLBACK, inventory["fallback"])
            candidate_report = manager.build_raw_glb_candidate(
                model, license_file,
                "org.example.static-raw", "Static Raw", "CC0-1.0",
                "Generated fixture", "https://example.invalid/static-raw",
                "diddy", ("car",), "+z", 1.25,
                probe.BIND_POSE_FALLBACK, "root", "head", characters,
                expected_model_sha256=inventory["model_sha256"],
            )
            candidate = Path(candidate_report["candidate"])
            verification = probe.verify_package(candidate)
            self.assertTrue(verification["valid"], verification["errors"])
            portable = root / "static-portable.mdkrchar"
            prepared = manager.prepare(candidate, portable)
            self.assertEqual(1, prepared["report"]["animations"])
            self.assertEqual(0, prepared["report"]["motion_channels"])
            self.assertEqual(
                [probe.BIND_POSE_FALLBACK],
                prepared["report"]["static_animations"],
            )
            with zipfile.ZipFile(candidate) as archive:
                packaged_model = archive.read("model.glb")
                document, _ = probe.parse_glb(packaged_model)
                self.assertNotIn("animations", document)

    def test_raw_glb_intake_rejects_ambiguous_or_uncalibrated_sources(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            original = make_animated_glb()

            def set_malformed_animation_name(document: dict) -> None:
                document["animations"][0]["name"] = 7

            malformed_name = root / "malformed-name.glb"
            malformed_name.write_bytes(rewrite_glb_document(
                original, set_malformed_animation_name
            ))
            with self.assertRaisesRegex(
                    manager.ManagerError, "name must be a string"):
                manager.inspect_raw_glb(malformed_name)

            def duplicate_root_node_name(document: dict) -> None:
                document["nodes"][1]["name"] = "root"

            duplicate_node = root / "duplicate-node.glb"
            duplicate_node.write_bytes(rewrite_glb_document(
                original, duplicate_root_node_name
            ))
            with self.assertRaisesRegex(
                    manager.ManagerError, "must be unique"):
                manager.inspect_raw_glb(duplicate_node)

            def remove_position_bounds(document: dict) -> None:
                primitive = document["meshes"][0]["primitives"][0]
                position = primitive["attributes"]["POSITION"]
                accessors = document["accessors"]
                accessors[position].pop("min", None)
                accessors[position].pop("max", None)

            unbounded = root / "unbounded.glb"
            unbounded.write_bytes(rewrite_glb_document(
                original, remove_position_bounds
            ))
            with self.assertRaisesRegex(
                    manager.ManagerError,
                    "attributes.POSITION requires min and max"):
                manager.inspect_raw_glb(unbounded)

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
            self.assertEqual("CC0-1.0", inspected["license_spdx"])
            self.assertEqual(
                "Generated MDKR test fixture", inspected["attribution"]
            )
            self.assertEqual(
                "https://example.invalid/pipeline-proof",
                inspected["source_url"],
            )
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
            self.assertEqual("mdkr-character-candidate-v7", index_lines[0])
            fields = index_lines[1].split("\t")
            self.assertEqual(60, len(fields))
            self.assertEqual(inspected["id"], fields[0])
            self.assertEqual(inspected["display_name"], bytes.fromhex(
                fields[1]
            ).decode("utf-8"))
            self.assertEqual(inspected["short_name"], bytes.fromhex(
                fields[2]
            ).decode("utf-8"))
            self.assertEqual(inspected["narration_name"], bytes.fromhex(
                fields[3]
            ).decode("utf-8"))
            self.assertEqual(inspected["sort_label"], bytes.fromhex(
                fields[4]
            ).decode("utf-8"))
            self.assertEqual(inspected["source_sha256"], fields[5])
            self.assertEqual(inspected["cache_source_digest"], fields[6])
            self.assertEqual("9", fields[7])
            self.assertEqual("7", fields[8])
            self.assertEqual(
                inspected["report"]["animation_channels"], int(fields[19])
            )
            self.assertEqual(
                inspected["report"]["animation_keys"], int(fields[20])
            )
            self.assertEqual(
                inspected["report"]["joint_constraints"], int(fields[25])
            )
            self.assertEqual(
                inspected["report"]["secondary_chains"], int(fields[26])
            )
            self.assertEqual(
                inspected["report"]["secondary_joints"], int(fields[27])
            )
            self.assertEqual(
                inspected["report"]["semantic_mask"], int(fields[42])
            )
            self.assertEqual(
                inspected["report"]["disabled_semantic_mask"],
                int(fields[43]),
            )
            self.assertEqual(
                inspected["report"]["authored_tangent_primitives"],
                int(fields[44]),
            )
            self.assertEqual(
                inspected["report"]["generated_tangent_primitives"],
                int(fields[45]),
            )
            self.assertEqual(
                inspected["report"]["authored_tangent_repaired_vertices"],
                int(fields[46]),
            )
            self.assertEqual(
                inspected["report"][
                    "generated_tangent_degenerate_uv_triangles"
                ],
                int(fields[47]),
            )
            self.assertEqual(
                inspected["report"]["tangent_fallback_vertices"],
                int(fields[48]),
            )
            self.assertEqual(
                inspected["report"][
                    "normal_map_tangent_fallback_vertices"
                ],
                int(fields[49]),
            )
            self.assertEqual(
                inspected["report"]["lod_vertices"],
                [int(value) for value in fields[30:34]],
            )
            self.assertEqual(
                inspected["report"]["lod_triangles"],
                [int(value) for value in fields[34:38]],
            )
            self.assertEqual(
                inspected["report"]["lod_primitives"],
                [int(value) for value in fields[38:42]],
            )
            for offset, name in enumerate((
                "ktx2_texture_count",
                "ktx2_source_bytes",
                "ktx2_etc1s_count",
                "ktx2_uastc_count",
                "ktx2_mip_levels_min",
                "ktx2_mip_levels_max",
            )):
                self.assertEqual(inspected["report"][name], int(fields[50 + offset]))
            self.assertEqual("1", fields[56])
            self.assertEqual(
                inspected["license_spdx"],
                bytes.fromhex(fields[57]).decode("utf-8"),
            )
            self.assertEqual(
                inspected["attribution"],
                bytes.fromhex(fields[58]).decode("utf-8"),
            )
            self.assertEqual(
                inspected["source_url"],
                bytes.fromhex(fields[59]).decode("utf-8"),
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

    def test_portable_export_uses_exact_retained_revision_without_mutation(
            self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            installed = root / "characters"
            original = manager.install(self.make_package(root), installed)
            portrait = root / "portrait-new.png"
            portrait.write_bytes(make_portrait_png(29))
            revised = manager.revise_identity(
                original["id"], portrait, (29, 58, 87), installed
            )
            manager.restore_revision(
                original["id"], original["source_sha256"], installed
            )
            before = {
                path.name: path.read_bytes()
                for path in installed.iterdir() if path.is_file()
            }

            portable = root / "portable-revision.mdkrchar"
            report = manager.export_portable_revision(
                original["id"], revised["source_sha256"], installed, portable
            )
            self.assertEqual("export-portable-revision", report["action"])
            self.assertEqual(revised["source_sha256"], report["source_sha256"])
            self.assertEqual(str(portable.resolve()), report["exported_file"])
            verification = probe.verify_package(portable)
            self.assertTrue(verification["valid"], verification["errors"])
            self.assertTrue(verification["portable"])
            with zipfile.ZipFile(portable) as archive:
                self.assertEqual(
                    portrait.read_bytes(), archive.read("portrait.png")
                )
            self.assertEqual(
                before,
                {
                    path.name: path.read_bytes()
                    for path in installed.iterdir() if path.is_file()
                },
            )
            with self.assertRaisesRegex(manager.ManagerError, "already exists"):
                manager.export_portable_revision(
                    original["id"], revised["source_sha256"], installed,
                    portable,
                )

    def test_rebuild_current_is_transactional_and_preserves_disabled_state(
            self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            installed = root / "characters"
            original = manager.install(self.make_package(root), installed)
            manager.set_enabled(original["id"], installed, False)
            cache = installed / f"{original['id']}.mdkc.disabled"
            before_cache = cache.read_bytes()

            rebuilt = manager.rebuild_current(original["id"], installed)
            self.assertEqual("rebuild-current", rebuilt["action"])
            self.assertEqual(
                original["source_sha256"], rebuilt["rebuilt_source_sha256"]
            )
            self.assertFalse(rebuilt["enabled"])
            self.assertEqual(before_cache, cache.read_bytes())
            self.assertFalse((installed / f"{original['id']}.mdkc").exists())

            before_files = {
                path.name: path.read_bytes()
                for path in installed.iterdir() if path.is_file()
            }
            with mock.patch.object(
                    manager.compiler, "compile_character",
                    side_effect=manager.compiler.CompileError("forced failure")):
                with self.assertRaisesRegex(
                        manager.compiler.CompileError, "forced failure"):
                    manager.rebuild_current(original["id"], installed)
            self.assertEqual(
                before_files,
                {
                    path.name: path.read_bytes()
                    for path in installed.iterdir() if path.is_file()
                },
            )

    def test_native_launcher_cli_dispatches_portable_export_and_rebuild(
            self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            installed = root / "characters"
            original = manager.install(self.make_package(root), installed)
            result = installed / ".launcher-character-result.json"
            portable = root / "portable-cli.mdkrchar"

            with redirect_stdout(io.StringIO()):
                status = manager.main([
                    "--directory", str(installed),
                    "--result-file", str(result),
                    "export-portable", original["id"],
                    original["source_sha256"], str(portable),
                ])
            self.assertEqual(0, status)
            exported = json.loads(result.read_text(encoding="utf-8"))
            self.assertTrue(exported["ok"])
            self.assertEqual("export-portable-revision", exported["action"])
            self.assertTrue(probe.verify_package(portable)["portable"])

            with redirect_stdout(io.StringIO()):
                status = manager.main([
                    "--directory", str(installed),
                    "--result-file", str(result),
                    "rebuild", original["id"],
                ])
            self.assertEqual(0, status)
            rebuilt = json.loads(result.read_text(encoding="utf-8"))
            self.assertTrue(rebuilt["ok"])
            self.assertEqual("rebuild-current", rebuilt["action"])

    def test_legacy_compiler_source_remains_editable_and_upgrades(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            installed = root / "characters"
            original = manager.install(self.make_package(root), installed)
            source_path = installed / original["source_file"]
            with zipfile.ZipFile(source_path) as archive:
                legacy_digest = manager._compiler_source_digest(
                    archive, "mdkr-character-compiler/4"
                ).hex()
            cache_path = installed / f"{original['id']}.mdkc"
            legacy_cache = bytearray(cache_path.read_bytes())
            legacy_cache[20:52] = bytes.fromhex(legacy_digest)
            cache_path.write_bytes(legacy_cache)
            report_path = installed / (
                f"{original['id']}.{original['source_sha256']}.json"
            )
            report = json.loads(report_path.read_text(encoding="utf-8"))
            report["cache_source_digest"] = legacy_digest
            report_path.write_text(
                json.dumps(report, indent=2, sort_keys=True) + "\n",
                encoding="utf-8",
            )
            portrait = root / "legacy-upgrade.png"
            portrait.write_bytes(make_portrait_png(17))
            revised = manager.revise_identity(
                original["id"], portrait, (11, 22, 33), installed
            )
            self.assertNotEqual(legacy_digest, revised["cache_source_digest"])
            self.assertEqual(manager.COMPILER_ID, revised["compiler"])

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

    def test_rig_revision_preserves_reversible_animation_intent(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            installed = root / "characters"
            portrait = make_portrait_png()
            source = make_v4_manifest(portrait, humanoid=True)
            source["rig"]["reviewed"] = True
            source["animations"]["states"]["select.idle"] = "idle"
            original = manager.install(
                self.make_package(root, source, make_humanoid_glb()), installed
            )
            draft = root / "motion-draft.json"
            draft.write_text(json.dumps({
                "schema": "mdkr-character-rig-draft-v2",
                **source["rig"],
                "disabled_semantics": ["select.idle"],
            }), encoding="utf-8")
            disabled = manager.revise_rig(original["id"], draft, installed)
            self.assertEqual(["select.idle"], disabled["disabled_semantics"])
            self.assertEqual(
                ["select.idle"],
                disabled["report"]["disabled_semantics"],
            )
            with zipfile.ZipFile(self.active_source(installed)) as archive:
                manifest = probe.json_loads_strict(archive.read("manifest.json"))
            self.assertEqual(
                "idle", manifest["animations"]["states"]["select.idle"]
            )
            self.assertEqual(
                ["select.idle"], manifest["animations"]["disabled_states"]
            )

            draft.write_text(json.dumps({
                "schema": "mdkr-character-rig-draft-v1",
                **source["rig"],
            }), encoding="utf-8")
            preserved = manager.revise_rig(
                original["id"], draft, installed
            )
            self.assertEqual(
                ["select.idle"], preserved["disabled_semantics"]
            )
            with zipfile.ZipFile(self.active_source(installed)) as archive:
                manifest = probe.json_loads_strict(archive.read("manifest.json"))
            self.assertEqual(
                ["select.idle"], manifest["animations"]["disabled_states"]
            )

            draft.write_text(json.dumps({
                "schema": "mdkr-character-rig-draft-v2",
                **source["rig"],
                "disabled_semantics": [],
            }), encoding="utf-8")
            restored = manager.revise_rig(original["id"], draft, installed)
            self.assertEqual([], restored["disabled_semantics"])
            with zipfile.ZipFile(self.active_source(installed)) as archive:
                manifest = probe.json_loads_strict(archive.read("manifest.json"))
            self.assertNotIn("disabled_states", manifest["animations"])
            self.assertEqual(
                "idle", manifest["animations"]["states"]["select.idle"]
            )

    def test_rig_v3_draft_authors_and_removes_source_v5_motion(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            installed = root / "characters"
            model, portrait, motion_source = make_v5_character()
            source = json.loads(json.dumps(motion_source))
            constraint = source["rig"]["roles"]["lower_arm.left"].pop(
                "constraint"
            )
            secondary = source.pop("secondary_motion")
            source["schema"] = probe.PACKAGE_SCHEMA_V4
            original = manager.install(
                self.make_package(root, source, model), installed
            )
            draft = root / "motion-authoring-draft.json"
            source["rig"]["roles"]["lower_arm.left"]["constraint"] = (
                constraint
            )
            draft.write_text(json.dumps({
                "schema": "mdkr-character-rig-draft-v3",
                **source["rig"],
                "disabled_semantics": [],
                "secondary_motion": secondary,
            }), encoding="utf-8")
            authored = manager.revise_rig(original["id"], draft, installed)
            self.assertEqual(1, authored["report"]["joint_constraints"])
            self.assertEqual(1, authored["report"]["secondary_chains"])
            self.assertEqual(2, authored["report"]["secondary_joints"])
            with zipfile.ZipFile(self.active_source(installed)) as archive:
                manifest = probe.json_loads_strict(archive.read("manifest.json"))
            self.assertEqual(probe.PACKAGE_SCHEMA_V5, manifest["schema"])
            self.assertEqual(
                constraint,
                manifest["rig"]["roles"]["lower_arm.left"]["constraint"],
            )
            self.assertEqual(secondary, manifest["secondary_motion"])

            clean_rig = json.loads(json.dumps(manifest["rig"]))
            clean_rig["roles"]["lower_arm.left"].pop("constraint")
            draft.write_text(json.dumps({
                "schema": "mdkr-character-rig-draft-v3",
                **clean_rig,
                "disabled_semantics": [],
                "secondary_motion": None,
            }), encoding="utf-8")
            removed = manager.revise_rig(original["id"], draft, installed)
            self.assertEqual(0, removed["report"]["joint_constraints"])
            self.assertEqual(0, removed["report"]["secondary_chains"])
            self.assertEqual(0, removed["report"]["secondary_joints"])
            with zipfile.ZipFile(self.active_source(installed)) as archive:
                manifest = probe.json_loads_strict(archive.read("manifest.json"))
            self.assertNotIn("secondary_motion", manifest)
            self.assertNotIn(
                "constraint",
                manifest["rig"]["roles"]["lower_arm.left"],
            )

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

    def test_workshop_draft_builds_all_source_edits_as_one_revision(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            installed = root / "characters"
            source = make_v4_manifest(make_portrait_png(), humanoid=True)
            original = manager.install(
                self.make_package(root, source, make_humanoid_glb()), installed
            )
            rgba = bytes(
                (index * 37 + 11) & 0xFF for index in range(40 * 40 * 4)
            )
            rig = json.loads(json.dumps(source["rig"]))
            rig["reviewed"] = True
            rig["roles"]["lower_arm.left"]["constraint"] = {
                "twist_axis": [1.0, 0.0, 0.0],
                "swing_limit_degrees": 60.0,
                "twist_min_degrees": -30.0,
                "twist_max_degrees": 30.0,
            }
            draft = root / "workshop-draft.json"
            draft.write_text(json.dumps({
                "schema": "mdkr-workshop-build-v1",
                "base_cache_source_digest": original["cache_source_digest"],
                "display_name": "Dixie Kong",
                "short_name": "Dixie",
                "narration_name": "Dixie Kong",
                "sort_label": "Kong, Dixie",
                "donor": "banjo",
                "vehicles": ["car", "plane"],
                "portrait_rgba_hex": rgba.hex(),
                "minimap_rgb": [19, 83, 211],
                "rig_draft": {
                    "schema": "mdkr-character-rig-draft-v3",
                    **rig,
                    "disabled_semantics": [],
                    "secondary_motion": None,
                },
            }), encoding="utf-8")

            built = manager.build_workshop_draft(
                original["id"], draft, installed
            )
            self.assertEqual("build-workshop-draft", built["action"])
            self.assertEqual(
                original["cache_source_digest"],
                built["based_on_cache_source_digest"],
            )
            self.assertEqual("banjo", built["report"]["donor"])
            self.assertEqual(5, built["report"]["vehicle_mask"])
            self.assertEqual([19, 83, 211], built["report"]["minimap_rgb"])
            self.assertEqual("Dixie Kong", built["report"]["display_name"])
            self.assertEqual(
                "Dixie", built["report"]["identity_short_name"]
            )
            self.assertEqual(
                "Dixie Kong", built["report"]["identity_narration_name"]
            )
            self.assertEqual(
                "Kong, Dixie", built["report"]["identity_sort_label"]
            )
            self.assertTrue(built["report"]["rig_reviewed"])
            self.assertEqual(2, len(list(installed.glob("*.mdkrchar"))))
            self.assertEqual(2, len(manager.list_revisions(
                original["id"], installed
            )["revisions"]))
            with zipfile.ZipFile(self.active_source(installed)) as archive:
                manifest = probe.json_loads_strict(
                    archive.read("manifest.json"), "manifest"
                )
                self.assertEqual(probe.PACKAGE_SCHEMA_V5, manifest["schema"])
                self.assertEqual("Dixie Kong", manifest["display_name"])
                self.assertEqual("Dixie", manifest["identity"]["short_name"])
                self.assertEqual(
                    "Dixie Kong", manifest["identity"]["narration_name"]
                )
                self.assertEqual(
                    "Kong, Dixie", manifest["identity"]["sort_label"]
                )
                self.assertEqual("banjo", manifest["gameplay"]["donor"])
                self.assertEqual(
                    ["car", "plane"], manifest["gameplay"]["vehicles"]
                )
                self.assertEqual(rig, manifest["rig"])
                self.assertEqual(
                    manager._portrait_png_from_rgba(rgba),
                    archive.read("portrait.png"),
                )

    def test_invalid_or_stale_workshop_draft_never_publishes(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            installed = root / "characters"
            source = make_v4_manifest(make_portrait_png(), humanoid=True)
            original = manager.install(
                self.make_package(root, source, make_humanoid_glb()), installed
            )
            cache = installed / f"{original['id']}.mdkc"
            before = cache.read_bytes()
            before_files = sorted(path.name for path in installed.iterdir())
            rig = json.loads(json.dumps(source["rig"]))
            payload = {
                "schema": "mdkr-workshop-build-v1",
                "base_cache_source_digest": "0" * 64,
                "display_name": "Dixie Kong",
                "short_name": "Dixie",
                "narration_name": "Dixie Kong",
                "sort_label": "Kong, Dixie",
                "donor": "diddy",
                "vehicles": ["car"],
                "portrait_rgba_hex": bytes(40 * 40 * 4).hex(),
                "minimap_rgb": [1, 2, 3],
                "rig_draft": {
                    "schema": "mdkr-character-rig-draft-v1",
                    **rig,
                },
            }
            draft = root / "workshop-draft.json"
            draft.write_text(json.dumps(payload), encoding="utf-8")
            with self.assertRaisesRegex(manager.ManagerError, "changed"):
                manager.build_workshop_draft(original["id"], draft, installed)
            self.assertEqual(before, cache.read_bytes())
            self.assertEqual(
                before_files, sorted(path.name for path in installed.iterdir())
            )

            payload["base_cache_source_digest"] = original[
                "cache_source_digest"
            ]
            payload["rig_draft"]["surprise"] = True
            draft.write_text(json.dumps(payload), encoding="utf-8")
            with self.assertRaisesRegex(manager.ManagerError, "unknown"):
                manager.build_workshop_draft(original["id"], draft, installed)
            self.assertEqual(before, cache.read_bytes())
            self.assertEqual(
                before_files, sorted(path.name for path in installed.iterdir())
            )
            self.assertEqual(before, cache.read_bytes())

            payload["rig_draft"].pop("surprise")
            payload["sort_label"] = "Kong\u202e, Dixie"
            draft.write_text(json.dumps(payload), encoding="utf-8")
            with self.assertRaisesRegex(
                manager.ManagerError, "sort label.*printable UTF-8"
            ):
                manager.build_workshop_draft(original["id"], draft, installed)
            self.assertEqual(before, cache.read_bytes())
            self.assertEqual(
                before_files, sorted(path.name for path in installed.iterdir())
            )

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


class CharacterValidationBoundaryTests(unittest.TestCase):
    def test_standards_errors_fail_closed_with_bounded_author_diagnostics(self) -> None:
        validation = accepted_validation(b"fixture", "fixture")
        validation["valid"] = False
        validation["report"]["issues"].update({
            "numErrors": 1,
            "messages": [{
                "code": "ACCESSOR_INVALID",
                "message": "accessor is outside its buffer view",
                "severity": 0,
                "pointer": "/accessors/0",
            }],
        })
        with mock.patch.object(
                manager.gltf_validator, "validate_glb_bytes",
                return_value=validation):
            with self.assertRaisesRegex(
                    manager.ManagerError,
                    "ACCESSOR_INVALID: accessor is outside"):
                manager._validate_character_glb(b"fixture", "raw model")

    def test_validator_tool_failure_is_distinct_from_invalid_author_content(self) -> None:
        failure = manager.gltf_validator.ValidatorError("attestation mismatch")
        with mock.patch.object(
                manager.gltf_validator, "validate_glb_bytes",
                side_effect=failure):
            with self.assertRaisesRegex(
                    manager.ManagerError,
                    "could not be checked by the pinned Khronos"):
                manager._validate_character_glb(b"fixture", "package model")

    def test_failed_import_recovery_is_private_metadata_only_and_retryable(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            characters = root / "characters"
            model = root / "author.glb"
            model_payload = make_animated_glb()
            model.write_bytes(model_payload)
            invalid = accepted_validation(model_payload, "raw")
            invalid["valid"] = False
            invalid["report"]["issues"].update({
                "numErrors": 1,
                "messages": [{
                    "code": "TEST_INVALID",
                    "message": "author fixture rejected",
                    "severity": 0,
                    "pointer": "/asset",
                }],
            })
            failure = manager.ImportValidationError(
                "raw character model is not valid glTF 2.0", invalid
            )
            index = characters / manager.RAW_INTAKE_INDEX_NAME
            with mock.patch.object(
                    manager, "_validate_character_glb", side_effect=failure):
                with redirect_stdout(io.StringIO()) as output:
                    status = manager.main([
                        "--directory", str(characters),
                        "write-raw-glb-index", str(model), str(index),
                    ])
            self.assertEqual(2, status)
            result = json.loads(output.getvalue())
            record_id = result["failed_import"]["record_id"]
            failures = manager.list_failed_imports(characters)
            self.assertEqual(1, failures["count"])
            entry = failures["entries"][0]
            self.assertEqual(record_id, entry["record_id"])
            self.assertTrue(entry["source_available"])
            self.assertFalse(entry["source_changed"])
            self.assertTrue(entry["validation_report_available"])
            recovery_dir = characters / manager.FAILURE_DIRECTORY_NAME
            self.assertEqual(
                [f"{record_id}.json", f"{record_id}.validation.json"],
                sorted(path.name for path in recovery_dir.iterdir()),
            )
            self.assertTrue(all(
                model_payload not in path.read_bytes()
                for path in recovery_dir.iterdir()
            ))

            failure_index = characters / manager.FAILURE_INDEX_NAME
            indexed = manager.write_failure_index(characters, failure_index)
            self.assertEqual(1, indexed["indexed_failures"])
            self.assertEqual(10, len(
                failure_index.read_text(encoding="ascii").splitlines()[1].split("\t")
            ))
            exported = root / "diagnostic.json"
            export = manager.export_failed_import_report(
                record_id, characters, exported
            )
            self.assertTrue(export["validation_included"])
            diagnostic = json.loads(exported.read_text(encoding="utf-8"))
            self.assertEqual(record_id, diagnostic["failure"]["record_id"])
            self.assertEqual("TEST_INVALID", diagnostic["validation"][
                "report"]["issues"]["messages"][0]["code"])

            model.write_bytes(model_payload + b"changed")
            changed = manager.list_failed_imports(characters)["entries"][0]
            self.assertTrue(changed["source_available"])
            self.assertTrue(changed["source_changed"])
            with self.assertRaisesRegex(manager.ManagerError, "source changed"):
                manager.retry_failed_import(record_id, characters)
            model.unlink()
            missing = manager.list_failed_imports(characters)["entries"][0]
            self.assertFalse(missing["source_available"])
            self.assertFalse(missing["source_changed"])
            with self.assertRaisesRegex(manager.ManagerError, "source is missing"):
                manager.retry_failed_import(record_id, characters)
            model.write_bytes(model_payload)

            same_size_change = bytearray(model_payload)
            same_size_change[-1] ^= 1
            model.write_bytes(same_size_change)
            quick = manager.list_failed_imports(characters)["entries"][0]
            self.assertFalse(quick["source_changed"])
            with self.assertRaisesRegex(manager.ManagerError, "source changed"):
                manager.retry_failed_import(record_id, characters)
            model.write_bytes(model_payload)

            with mock.patch.object(
                    manager, "_validate_character_glb", side_effect=failure):
                with self.assertRaises(manager.ImportValidationError):
                    manager.retry_failed_import(record_id, characters)
            retained = manager.list_failed_imports(characters)["entries"][0]
            self.assertEqual(2, retained["attempts"])
            self.assertTrue(retained["validation_report_available"])

            with mock.patch.object(
                    manager, "_validate_character_glb",
                    side_effect=accepted_validation):
                retried = manager.retry_failed_import(record_id, characters)
            self.assertTrue(retried["resolved"])
            self.assertEqual(0, manager.list_failed_imports(characters)["count"])
            self.assertTrue(model.is_file())

            with mock.patch.object(
                    manager, "_validate_character_glb", side_effect=failure):
                with redirect_stdout(io.StringIO()) as output:
                    self.assertEqual(2, manager.main([
                        "--directory", str(characters),
                        "write-raw-glb-index", str(model), str(index),
                    ]))
            forgotten_id = json.loads(output.getvalue())[
                "failed_import"
            ]["record_id"]
            forgotten = manager.forget_failed_import(
                forgotten_id, characters
            )
            self.assertEqual(2, len(forgotten["removed"]))
            self.assertTrue(model.is_file())
            self.assertEqual(0, manager.list_failed_imports(characters)["count"])


if __name__ == "__main__":
    unittest.main()
