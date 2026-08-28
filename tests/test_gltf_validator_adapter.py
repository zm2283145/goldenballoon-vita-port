#!/usr/bin/env python3
"""ROM-free tests for the pinned Khronos Validator boundary."""

from __future__ import annotations

import hashlib
import json
import os
import stat
import struct
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import build_gltf_validator as builder  # noqa: E402
import gltf_validator_adapter as adapter  # noqa: E402


def report_bytes(errors: int = 0, *, truncated: bool = False) -> bytes:
    messages = ([{
        "code": "TEST_ISSUE", "message": "fixture diagnostic",
        "severity": 0 if errors else 3, "pointer": "/asset",
    }] if errors or truncated else [])
    return json.dumps({
        "uri": "model.glb",
        "mimeType": "model/gltf-binary",
        "validatorVersion": adapter.VALIDATOR_VERSION,
        "issues": {
            "numErrors": errors, "numWarnings": 0, "numInfos": 0,
            "numHints": 0, "messages": messages, "truncated": truncated,
        },
        "info": {"version": "2.0"},
    }).encode("utf-8")


class GltfValidatorAdapterTests(unittest.TestCase):
    def _elf_fixture(self, root: Path) -> Path:
        executable = root / "gltf_validator"
        payload = bytearray(64)
        payload[:4] = b"\x7fELF"
        payload[4] = 2
        payload[5] = 1
        struct.pack_into("<H", payload, 18, 62)
        executable.write_bytes(payload)
        executable.chmod(executable.stat().st_mode | stat.S_IXUSR)
        return executable

    def _official_manifest(self, executable: Path) -> dict[str, object]:
        digest = hashlib.sha256(executable.read_bytes()).hexdigest()
        return {
            "schema": adapter.MANIFEST_SCHEMA,
            "version": adapter.VALIDATOR_VERSION,
            "commit": adapter.VALIDATOR_COMMIT,
            "target": "linux-x86_64",
            "distribution": "official-release",
            "archive_sha256": adapter.OFFICIAL_ARCHIVE_SHA256["linux-x86_64"],
            "source_archive_sha256": None,
            "dart_version": None,
            "dart_sdk_archive_sha256": None,
            "pubspec_lock_sha256": None,
            "executable": "gltf_validator",
            "executable_bytes": executable.stat().st_size,
            "build_executable_sha256": digest,
            "executable_sha256": digest,
        }

    def test_binary_target_reads_native_headers_without_external_tools(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            temporary = Path(raw)
            self.assertEqual(
                "linux-x86_64", adapter._binary_target(self._elf_fixture(temporary))
            )
            macho = temporary / "macho"
            macho.write_bytes(
                b"\xcf\xfa\xed\xfe" + struct.pack("<I", 0x0100000C) + b"\0" * 8
            )
            self.assertEqual("darwin-arm64", adapter._binary_target(macho))
            pe = bytearray(128)
            pe[:2] = b"MZ"
            struct.pack_into("<I", pe, 0x3C, 80)
            pe[80:84] = b"PE\0\0"
            struct.pack_into("<H", pe, 84, 0x8664)
            windows = temporary / "validator.exe"
            windows.write_bytes(pe)
            self.assertEqual("windows-x86_64", adapter._binary_target(windows))

    def test_installation_manifest_binds_target_bytes_and_distribution(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            temporary = Path(raw)
            executable = self._elf_fixture(temporary)
            manifest = self._official_manifest(executable)
            manifest_path = temporary / "gltf_validator.manifest.json"
            manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
            digest = manifest["executable_sha256"]
            with mock.patch.dict(
                    adapter.PINNED_BUILD_SHA256,
                    {"linux-x86_64": digest}):
                verified = adapter.verify_installation(
                    executable, manifest_path, "linux-x86_64"
                )
                self.assertEqual("official-release", verified["distribution"])
                executable.write_bytes(executable.read_bytes() + b"drift")
                with self.assertRaisesRegex(adapter.ValidatorError, "bytes mismatch"):
                    adapter.verify_installation(
                        executable, manifest_path, "linux-x86_64"
                    )

    def test_official_install_cannot_attest_arbitrary_bytes_as_pinned(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            temporary = Path(raw)
            executable = self._elf_fixture(temporary)
            manifest = self._official_manifest(executable)
            manifest["build_executable_sha256"] = "0" * 64
            manifest_path = temporary / "gltf_validator.manifest.json"
            manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
            with mock.patch.dict(
                    adapter.PINNED_BUILD_SHA256,
                    {"linux-x86_64": "0" * 64}):
                with self.assertRaisesRegex(
                        adapter.ValidatorError, "bytes differ from the pin"):
                    adapter.verify_installation(
                        executable, manifest_path, "linux-x86_64"
                    )

    def test_manifest_rejects_duplicate_keys(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            path = Path(raw) / "manifest.json"
            path.write_text(
                '{"schema":"one","schema":"two"}', encoding="utf-8"
            )
            with self.assertRaisesRegex(adapter.ValidatorError, "duplicate JSON key"):
                adapter._load_manifest(path)

    def test_resolver_never_searches_path_or_accepts_relative_override(self) -> None:
        with mock.patch.dict(os.environ, {"MDKR_GLTF_VALIDATOR": "validator"}):
            with self.assertRaisesRegex(adapter.ValidatorError, "absolute path"):
                adapter.resolve_installation()
        with mock.patch.dict(os.environ, {}, clear=True):
            with mock.patch.object(sys, "frozen", False, create=True):
                with self.assertRaisesRegex(adapter.ValidatorError, "requires"):
                    adapter.resolve_installation()

    def test_frozen_macos_resolver_uses_sealed_resource_manifest(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            contents = Path(raw) / "GoldenBalloon.app" / "Contents"
            importer = contents / "MacOS" / "tools" / "character_importer"
            manifest = (
                contents
                / "Resources"
                / "ThirdParty"
                / "GltfValidator-MANIFEST.json"
            )
            importer.parent.mkdir(parents=True)
            manifest.parent.mkdir(parents=True)
            manifest.write_text("{}", encoding="utf-8")
            with mock.patch.dict(os.environ, {}, clear=True):
                with mock.patch.object(sys, "frozen", True, create=True):
                    with mock.patch.object(sys, "executable", str(importer)):
                        with mock.patch.object(sys, "platform", "darwin"):
                            executable, resolved_manifest = adapter.resolve_installation()
            self.assertEqual(
                (
                    contents
                    / "MacOS"
                    / "tools"
                    / "validators"
                    / "gltf_validator"
                ).resolve(),
                executable,
            )
            self.assertEqual(manifest.resolve(), resolved_manifest)

    def test_validation_uses_private_canonical_model_and_preserves_report(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            temporary = Path(raw)
            executable = temporary / "gltf_validator"
            manifest = temporary / "gltf_validator.manifest.json"
            executable.write_bytes(b"fixture")
            manifest.write_text("{}", encoding="ascii")

            def run(command: list[str], cwd: Path) -> tuple[int, bytes, bytes]:
                self.assertEqual("model.glb", command[-1])
                self.assertEqual("validator-config.yaml", command[-2])
                self.assertEqual(b"glb fixture", (cwd / "model.glb").read_bytes())
                self.assertEqual(
                    f"max-issues: {adapter.MAX_ISSUES}\n",
                    (cwd / "validator-config.yaml").read_text(encoding="ascii"),
                )
                return 0, report_bytes(), b"absolute temp path is ignored"

            identity = {
                "target": "darwin-arm64",
                "distribution": "pinned-source-build",
                "build_executable_sha256": "1" * 64,
                "executable_sha256": "2" * 64,
            }
            with mock.patch.object(adapter, "verify_installation", return_value=identity):
                with mock.patch.object(adapter, "_run_bounded", side_effect=run):
                    result = adapter.validate_glb_bytes(
                        b"glb fixture", validator=executable.resolve(),
                        manifest=manifest.resolve(),
                    )
            self.assertTrue(result["valid"])
            self.assertEqual(adapter.ADAPTER_SCHEMA, result["schema"])
            self.assertEqual(
                hashlib.sha256(b"glb fixture").hexdigest(),
                result["source_sha256"],
            )

    def test_validation_errors_are_not_misclassified_as_tool_failure(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            temporary = Path(raw)
            executable = temporary / "gltf_validator"
            manifest = temporary / "gltf_validator.manifest.json"
            executable.write_bytes(b"fixture")
            manifest.write_text("{}", encoding="ascii")
            identity = {
                "target": "linux-x86_64", "distribution": "official-release",
                "build_executable_sha256": "1" * 64,
                "executable_sha256": "1" * 64,
            }
            with mock.patch.object(adapter, "verify_installation", return_value=identity):
                with mock.patch.object(
                        adapter, "_run_bounded",
                        return_value=(1, report_bytes(errors=1), b"Errors: 1")):
                    result = adapter.validate_glb_bytes(
                        b"bad", validator=executable.resolve(),
                        manifest=manifest.resolve(),
                    )
            self.assertFalse(result["valid"])
            self.assertEqual(1, result["report"]["issues"]["numErrors"])

    def test_report_rejects_noninteger_severity_and_ambiguous_location(self) -> None:
        report = json.loads(report_bytes(errors=1))
        report["issues"]["messages"][0]["severity"] = 1.0
        with self.assertRaisesRegex(adapter.ValidatorError, "severity"):
            adapter._validate_report(report)
        report["issues"]["messages"][0]["severity"] = 0
        report["issues"]["messages"][0]["offset"] = 4
        with self.assertRaisesRegex(adapter.ValidatorError, "ambiguous"):
            adapter._validate_report(report)

    def test_bounded_runner_kills_oversized_output(self) -> None:
        command = [
            sys.executable, "-c",
            "import sys; sys.stdout.buffer.write(b'x' * 4096)",
        ]
        with mock.patch.object(adapter, "MAX_REPORT_BYTES", 1024):
            with self.assertRaisesRegex(adapter.ValidatorError, "oversized"):
                adapter._run_bounded(command, ROOT)

    def test_report_write_is_atomic_and_replaces_exact_destination(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            path = root / "report.json"
            path.write_text("old", encoding="ascii")
            adapter.write_report(path, {"schema": "fixture", "valid": False})
            self.assertEqual(
                {"schema": "fixture", "valid": False},
                json.loads(path.read_text(encoding="utf-8")),
            )
            self.assertFalse((root / "report.json.tmp").exists())

    def test_byte_api_rejects_empty_nonbytes_and_oversized_inputs_early(self) -> None:
        with self.assertRaisesRegex(adapter.ValidatorError, "non-empty bytes"):
            adapter.validate_glb_bytes(b"")
        with self.assertRaisesRegex(adapter.ValidatorError, "non-empty bytes"):
            adapter.validate_glb_bytes(bytearray(b"glb"))  # type: ignore[arg-type]
        with mock.patch.object(adapter, "MAX_GLB_BYTES", 3):
            with self.assertRaisesRegex(adapter.ValidatorError, "exceeds"):
                adapter.validate_glb_bytes(b"four")

    def test_archive_extractor_rejects_traversal_and_links(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            archive = root / "unsafe.zip"
            with zipfile.ZipFile(archive, "w") as output:
                output.writestr("../escape", b"bad")
            with self.assertRaisesRegex(builder.BuildError, "unsafe path"):
                builder._extract_zip(archive, root / "out")
            archive = root / "link.zip"
            info = zipfile.ZipInfo("gltf_validator")
            info.create_system = 3
            info.external_attr = (stat.S_IFLNK | 0o777) << 16
            with zipfile.ZipFile(archive, "w") as output:
                output.writestr(info, "target")
            with self.assertRaisesRegex(builder.BuildError, "linked"):
                builder._extract_zip(archive, root / "out2")


if __name__ == "__main__":
    unittest.main()
