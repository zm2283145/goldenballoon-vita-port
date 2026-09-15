#!/usr/bin/env python3
"""Hostile and lifecycle coverage for the data-only adapter handoff."""

from __future__ import annotations

import hashlib
import io
import json
import os
import stat
import sys
import tempfile
import unittest
import warnings
import zipfile
from contextlib import redirect_stdout
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "tests"))

import character_package_manager as manager  # noqa: E402
import character_source_adapter as adapter  # noqa: E402
from character_validation_fixture import accepted_validation  # noqa: E402
from test_character_asset_probe import make_animated_glb  # noqa: E402


SOURCE_SHA = hashlib.sha256(b"fixture.blend exact source bytes").hexdigest()
SETTINGS_SHA = hashlib.sha256(b'{"animations":"baked"}').hexdigest()


class CharacterSourceAdapterTests(unittest.TestCase):
    def setUp(self) -> None:
        patcher = mock.patch.object(
            manager, "_validate_character_glb", side_effect=accepted_validation
        )
        patcher.start()
        self.addCleanup(patcher.stop)

    def pack(self, root: Path, name: str = "artist.mdkrsource", *,
             with_license: bool = True, version: str = "4.2.1") -> Path:
        model = root / f"{name}.glb"
        model.write_bytes(make_animated_glb())
        license_path = None
        if with_license:
            license_path = root / f"{name}.LICENSE"
            license_path.write_text("CC0 fixture license\n", encoding="utf-8")
        output = root / name
        adapter.pack(
            model_path=model, output_path=output,
            adapter_name="Example Blender Exporter", adapter_version=version,
            adapter_homepage="https://example.invalid/adapter",
            source_format="Blender scene", source_sha256=SOURCE_SHA,
            conversion_profile="Golden Balloon character-v1",
            conversion_settings_sha256=SETTINGS_SHA,
            license_path=license_path,
            license_spdx="CC0-1.0" if with_license else "",
            attribution="Example artist" if with_license else "",
            source_url="https://example.invalid/model" if with_license else "",
        )
        return output

    @staticmethod
    def rewrite(path: Path, transform) -> None:
        with zipfile.ZipFile(path) as source:
            members = [(info, source.read(info)) for info in source.infolist()]
        target = io.BytesIO()
        with zipfile.ZipFile(target, "w", allowZip64=False) as archive:
            for info, payload in transform(members):
                archive.writestr(info, payload)
        path.write_bytes(target.getvalue())

    def test_pack_is_deterministic_bounded_and_explicitly_unsigned(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            first = self.pack(root, "first.mdkrsource")
            second = self.pack(root, "second.mdkrsource")
            self.assertEqual(first.read_bytes(), second.read_bytes())
            report = adapter.inspect_path(first)
            self.assertEqual(adapter.SCHEMA, report["schema"])
            self.assertEqual("Example Blender Exporter", report["adapter_name"])
            self.assertEqual("Blender scene", report["source_format"])
            self.assertEqual(SOURCE_SHA, report["source_sha256"])
            self.assertEqual(SETTINGS_SHA,
                             report["conversion_settings_sha256"])
            self.assertTrue(report["license_present"])
            self.assertNotIn("signed", report)
            self.assertEqual(
                hashlib.sha256(report["model_payload"]).hexdigest(),
                report["model_sha256"],
            )

    def test_optional_license_is_exact_not_inferred(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            output = self.pack(root, with_license=False)
            report = adapter.inspect_path(output)
            self.assertFalse(report["license_present"])
            self.assertIsNone(report["license_payload"])
            self.assertEqual("", report["license_spdx"])

    def test_unknown_duplicate_and_symlink_members_fail_closed(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            unknown = self.pack(root, "unknown.mdkrsource")
            self.rewrite(unknown, lambda members: [
                *members,
                (zipfile.ZipInfo("run-me.py"), b"print('unsafe')\n"),
            ])
            with self.assertRaisesRegex(adapter.AdapterOutputError,
                                        "unexpected run-me.py"):
                adapter.inspect_path(unknown)

            duplicate = self.pack(root, "duplicate.mdkrsource")
            with warnings.catch_warnings():
                warnings.simplefilter("ignore", UserWarning)
                self.rewrite(duplicate, lambda members: [*members, members[0]])
            with self.assertRaisesRegex(adapter.AdapterOutputError,
                                        "duplicate members"):
                adapter.inspect_path(duplicate)

            symlink = self.pack(root, "symlink.mdkrsource")
            def mark_model(members):
                rewritten = []
                for info, payload in members:
                    if info.filename == "model.glb":
                        info = zipfile.ZipInfo("model.glb")
                        info.create_system = 3
                        info.external_attr = (stat.S_IFLNK | 0o777) << 16
                    rewritten.append((info, payload))
                return rewritten
            self.rewrite(symlink, mark_model)
            with self.assertRaisesRegex(adapter.AdapterOutputError,
                                        "unsafe or unsupported ZIP form"):
                adapter.inspect_path(symlink)

            extra = self.pack(root, "extra.mdkrsource")
            def add_extra(members):
                rewritten = []
                for info, payload in members:
                    if info.filename == "model.glb":
                        info = zipfile.ZipInfo("model.glb")
                        info.extra = b"\x01\x00\x00\x00"
                    rewritten.append((info, payload))
                return rewritten
            self.rewrite(extra, add_extra)
            with self.assertRaisesRegex(adapter.AdapterOutputError,
                                        "extra field"):
                adapter.inspect_path(extra)

    def test_manifest_is_strict_and_every_payload_is_digest_bound(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            changed_model = self.pack(root, "model-digest.mdkrsource")
            self.rewrite(changed_model, lambda members: [
                (info, payload + b"x" if info.filename == "model.glb" else payload)
                for info, payload in members
            ])
            with self.assertRaisesRegex(adapter.AdapterOutputError,
                                        "model digest does not match"):
                adapter.inspect_path(changed_model)

            duplicate_json = self.pack(root, "duplicate-json.mdkrsource")
            def duplicate_schema(members):
                result = []
                for info, payload in members:
                    if info.filename == "adapter.json":
                        text = payload.decode("utf-8")
                        payload = text.replace(
                            '{"adapter":',
                            '{"schema":"wrong","adapter":', 1,
                        ).encode("utf-8")
                    result.append((info, payload))
                return result
            self.rewrite(duplicate_json, duplicate_schema)
            with self.assertRaisesRegex(adapter.AdapterOutputError,
                                        "duplicate key 'schema'"):
                adapter.inspect_path(duplicate_json)

            surrogate = self.pack(root, "surrogate.mdkrsource")
            def inject_surrogate(members):
                result = []
                for info, payload in members:
                    if info.filename == "adapter.json":
                        manifest = json.loads(payload)
                        manifest["adapter"]["name"] = "\ud800"
                        payload = json.dumps(manifest).encode("utf-8")
                    result.append((info, payload))
                return result
            self.rewrite(surrogate, inject_surrogate)
            with self.assertRaisesRegex(adapter.AdapterOutputError,
                                        "adapter name must be bounded"):
                adapter.inspect_path(surrogate)

    def test_review_index_then_exact_revalidation_and_atomic_extraction(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            artifact = self.pack(root)
            characters = root / "characters"
            index = characters / manager.ADAPTER_OUTPUT_INDEX_NAME
            reviewed = manager.write_adapter_output_index(
                artifact, characters, index
            )
            self.assertTrue(index.is_file())
            self.assertIn(reviewed["artifact_sha256"], index.read_text("ascii"))
            self.assertFalse(reviewed["authenticated"])

            model_output = root / "converted.glb"
            extracted = manager.extract_reviewed_adapter_output(
                artifact, model_output, reviewed["artifact_sha256"],
                reviewed["model_sha256"],
            )
            self.assertEqual(make_animated_glb(), model_output.read_bytes())
            self.assertEqual(
                "CC0 fixture license\n",
                (root / "converted.LICENSE.txt").read_text("utf-8"),
            )
            provenance_path = root / "converted.mdkrsource.json"
            provenance = json.loads(provenance_path.read_text("utf-8"))
            self.assertTrue(provenance["integrity_only_not_signed"])
            self.assertEqual(SOURCE_SHA, provenance["source_sha256"])
            self.assertEqual(
                str(provenance_path.resolve()), extracted["provenance_output"]
            )

    def test_changed_review_and_any_destination_collision_are_fail_atomic(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            artifact = self.pack(root)
            reviewed = manager.inspect_adapter_output(artifact)
            replacement = root / "replacement.mdkrsource"
            self.pack(root, replacement.name, version="4.2.2")
            artifact.write_bytes(replacement.read_bytes())
            output = root / "changed.glb"
            with self.assertRaisesRegex(manager.ManagerError,
                                        "changed after review"):
                manager.extract_reviewed_adapter_output(
                    artifact, output, reviewed["artifact_sha256"],
                    reviewed["model_sha256"],
                )
            self.assertFalse(output.exists())

            artifact = self.pack(root, "collision.mdkrsource")
            reviewed = manager.inspect_adapter_output(artifact)
            collision = root / "collision.mdkrsource.json"
            collision.write_text("user-owned\n", encoding="utf-8")
            output = root / "collision.glb"
            with self.assertRaisesRegex(manager.ManagerError,
                                        "destination already exists"):
                manager.extract_reviewed_adapter_output(
                    artifact, output, reviewed["artifact_sha256"],
                    reviewed["model_sha256"],
                )
            self.assertFalse(output.exists())
            self.assertEqual("user-owned\n", collision.read_text("utf-8"))

    def test_extraction_failure_cleans_only_unchanged_created_outputs(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            artifact = self.pack(root)
            reviewed = manager.inspect_adapter_output(artifact)
            original_write = manager._write_exclusive
            calls = 0

            def fail_provenance(path: Path, payload: bytes) -> os.stat_result:
                nonlocal calls
                calls += 1
                if calls == 3:
                    raise OSError("injected provenance write failure")
                return original_write(path, payload)

            output = root / "rollback.glb"
            with mock.patch.object(manager, "_write_exclusive",
                                   side_effect=fail_provenance):
                with self.assertRaisesRegex(OSError, "injected provenance"):
                    manager.extract_reviewed_adapter_output(
                        artifact, output, reviewed["artifact_sha256"],
                        reviewed["model_sha256"],
                    )
            self.assertFalse(output.exists())
            self.assertFalse((root / "rollback.LICENSE.txt").exists())
            self.assertFalse((root / "rollback.mdkrsource.json").exists())

            calls = 0

            def replace_then_fail(path: Path, payload: bytes) -> os.stat_result:
                nonlocal calls
                calls += 1
                identity = original_write(path, payload)
                if calls == 1:
                    path.write_bytes(b"concurrent owner replacement")
                if calls == 2:
                    raise OSError("injected license write failure")
                return identity

            raced = root / "raced.glb"
            with mock.patch.object(manager, "_write_exclusive",
                                   side_effect=replace_then_fail):
                with self.assertRaisesRegex(OSError, "injected license"):
                    manager.extract_reviewed_adapter_output(
                        artifact, raced, reviewed["artifact_sha256"],
                        reviewed["model_sha256"],
                    )
            self.assertEqual(b"concurrent owner replacement", raced.read_bytes())
            self.assertTrue((root / "raced.LICENSE.txt").exists())
            self.assertFalse((root / "raced.mdkrsource.json").exists())

    def test_failed_adapter_review_recovers_only_as_fresh_inspection(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            artifact = self.pack(root)
            characters = root / "characters"
            index = characters / manager.ADAPTER_OUTPUT_INDEX_NAME
            with mock.patch.object(
                    manager, "_validate_character_glb",
                    side_effect=manager.ManagerError("injected validator outage")):
                with redirect_stdout(io.StringIO()):
                    status = manager.main([
                        "--directory", str(characters),
                        "write-adapter-output-index", str(artifact), str(index),
                    ])
            self.assertEqual(2, status)
            failures = manager.list_failed_imports(characters)["entries"]
            self.assertEqual(1, len(failures))
            self.assertEqual("adapter-inspect", failures[0]["retry_kind"])
            self.assertIsNone(failures[0]["output_path"])
            retried = manager.retry_failed_import(
                failures[0]["record_id"], characters
            )
            self.assertEqual(adapter.SCHEMA, retried["result"]["schema"])
            self.assertEqual([], manager.list_failed_imports(characters)["entries"])


if __name__ == "__main__":
    unittest.main()
