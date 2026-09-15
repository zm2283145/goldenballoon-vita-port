#!/usr/bin/env python3
"""ROM-free tests for the frozen Character Workshop importer contract."""

from __future__ import annotations

import json
import stat
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import build_character_importer as builder  # noqa: E402
import character_package_manager as manager  # noqa: E402
import verify_character_importer as verifier  # noqa: E402


class CharacterImporterBuildTests(unittest.TestCase):
    def _attested_fixture(
            self, temporary: Path,
            *, build_hash: str | None = None) -> tuple[Path, Path]:
        executable = temporary / "character_importer"
        executable.write_bytes(b"frozen importer fixture\n")
        executable.chmod(executable.stat().st_mode | stat.S_IXUSR)
        executable_hash = builder.sha256_file(executable)
        manifest = {
            "schema": builder.BUILD_SCHEMA,
            "target": "darwin-arm64",
            "python": ".".join(map(str, builder.PINNED_PYTHON)),
            "pyinstaller": builder.PYINSTALLER_VERSION,
            "source_members": list(builder.SOURCE_MODULES),
            "source_bundle_sha256": builder.source_bundle_digest(ROOT),
            "executable": executable.name,
            "executable_bytes": executable.stat().st_size,
            "build_executable_sha256": build_hash or executable_hash,
            "executable_sha256": executable_hash,
            "manager_schema": manager.MANAGER_SCHEMA,
            "compiler_id": manager.COMPILER_ID,
        }
        manifest_path = temporary / "manifest.json"
        manifest_path.write_text(
            json.dumps(manifest), encoding="utf-8", newline="\n"
        )
        return executable, manifest_path

    def test_source_digest_binds_names_lengths_and_bytes(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            (root / "one").write_bytes(b"ab")
            (root / "two").write_bytes(b"c")
            first = builder.source_bundle_digest(root, ("one", "two"))
            second = builder.source_bundle_digest(root, ("two", "one"))
            self.assertNotEqual(first, second)
            (root / "one").write_bytes(b"a")
            (root / "two").write_bytes(b"bc")
            self.assertNotEqual(
                first, builder.source_bundle_digest(root, ("one", "two"))
            )

    def test_source_digest_rejects_missing_and_symlink_members(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            with self.assertRaisesRegex(builder.BuildError, "missing regular"):
                builder.source_bundle_digest(root, ("missing",))
            target = root / "target"
            target.write_text("source", encoding="ascii")
            link = root / "link"
            try:
                link.symlink_to(target)
            except OSError:
                self.skipTest("symlinks are unavailable")
            with self.assertRaisesRegex(builder.BuildError, "missing regular"):
                builder.source_bundle_digest(root, ("link",))

    def test_manager_tool_info_is_side_effect_free(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            before = list(root.iterdir())
            with mock.patch.object(sys, "frozen", True, create=True):
                with mock.patch("builtins.print") as output:
                    status = manager.main(["tool-info"])
            self.assertEqual(0, status)
            self.assertEqual(before, list(root.iterdir()))
            payload = output.call_args.args[0]
            report = json.loads(payload)
            self.assertTrue(report["ok"])
            self.assertTrue(report["frozen"])
            self.assertEqual(builder.INFO_SCHEMA, report["schema"])
            self.assertEqual(manager.COMPILER_ID, report["compiler_id"])

    def test_tool_info_verifier_rejects_nonfrozen_helper(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            helper = Path(raw) / "helper"
            helper.write_text(
                "#!/bin/sh\nprintf '%s\\n' "
                "'{\"ok\":true,\"schema\":\"mdkr-character-importer-info-v1\","
                "\"frozen\":false}'\n",
                encoding="ascii",
            )
            helper.chmod(helper.stat().st_mode | stat.S_IXUSR)
            with self.assertRaisesRegex(builder.BuildError, "non-frozen"):
                builder._run_tool_info(helper)

    def test_tool_info_checks_allow_bounded_first_launch_security_scan(self) -> None:
        report = json.dumps({
            "ok": True,
            "schema": builder.INFO_SCHEMA,
            "frozen": True,
            "python": ".".join(map(str, builder.PINNED_PYTHON)),
        }).encode("utf-8")
        completed = mock.Mock(returncode=0, stdout=report, stderr=b"")
        with mock.patch.object(
                subprocess, "run", return_value=completed) as run:
            builder._run_tool_info(Path("character_importer"))
            self.assertEqual(
                builder.TOOL_INFO_TIMEOUT_SECONDS,
                run.call_args.kwargs["timeout"],
            )
        with mock.patch.object(
                subprocess, "run", return_value=completed) as run:
            verifier._tool_info(Path("character_importer"))
            self.assertEqual(
                builder.TOOL_INFO_TIMEOUT_SECONDS,
                run.call_args.kwargs["timeout"],
            )

    def test_build_refuses_wrong_python_before_pyinstaller(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            output = Path(raw) / "character_importer"
            with mock.patch.object(builder, "PINNED_PYTHON", (0, 0, 0)):
                with self.assertRaisesRegex(builder.BuildError, "Python 0.0.0"):
                    builder.build(ROOT, output)
            self.assertFalse(output.exists())

    def test_exclusive_copy_cleans_partial_destination(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            temporary = Path(raw)
            source = temporary / "source"
            output = temporary / "output"
            source.write_bytes(b"complete")
            builder._copy_executable_exclusive(source, output)
            self.assertEqual(b"complete", output.read_bytes())
            self.assertTrue(output.stat().st_mode & stat.S_IXUSR)
            with self.assertRaises(FileExistsError):
                builder._copy_executable_exclusive(source, output)
            self.assertEqual(b"complete", output.read_bytes())

    def test_manifest_loader_rejects_duplicate_and_extra_keys(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            path = Path(raw) / "manifest.json"
            path.write_text('{"schema":"one","schema":"two"}\n')
            with self.assertRaisesRegex(
                    verifier.VerificationError, "duplicate manifest key"):
                verifier._load_manifest(path)
            path.write_text('{"unexpected":true}\n')
            manifest = verifier._load_manifest(path)
            self.assertEqual({"unexpected": True}, manifest)

    def test_verifier_rejects_source_or_executable_drift(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            temporary = Path(raw)
            executable = temporary / "character_importer"
            executable.write_text("#!/bin/sh\nexit 0\n", encoding="ascii")
            executable.chmod(executable.stat().st_mode | stat.S_IXUSR)
            manifest = {
                key: "invalid" for key in verifier.EXPECTED_KEYS
            }
            manifest["source_members"] = list(builder.SOURCE_MODULES)
            manifest["executable_bytes"] = executable.stat().st_size
            manifest_path = temporary / "manifest.json"
            manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
            with self.assertRaisesRegex(
                    verifier.VerificationError, "manifest schema mismatch"):
                verifier.verify(ROOT, executable, manifest_path, "darwin-arm64")

    def test_signed_verifier_preserves_distinct_canonical_build_hash(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            executable, manifest_path = self._attested_fixture(
                Path(raw), build_hash="0" * 64
            )
            info = {
                "ok": True,
                "schema": builder.INFO_SCHEMA,
                "frozen": True,
                "python": ".".join(map(str, builder.PINNED_PYTHON)),
                "manager_schema": manager.MANAGER_SCHEMA,
                "compiler_id": manager.COMPILER_ID,
            }
            with mock.patch.object(verifier, "_tool_info", return_value=info):
                verified = verifier.verify(
                    ROOT, executable, manifest_path, "darwin-arm64",
                    allow_signed=True,
                )
                self.assertEqual("0" * 64, verified["build_executable_sha256"])
                with self.assertRaisesRegex(
                        verifier.VerificationError,
                        "build_executable_sha256 mismatch"):
                    verifier.verify(
                        ROOT, executable, manifest_path, "darwin-arm64"
                    )

    def test_signed_verifier_rejects_noncanonical_build_hash(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            executable, manifest_path = self._attested_fixture(
                Path(raw), build_hash="signed"
            )
            with self.assertRaisesRegex(
                    verifier.VerificationError, "canonical build hash"):
                verifier.verify(
                    ROOT, executable, manifest_path, "darwin-arm64",
                    allow_signed=True,
                )


if __name__ == "__main__":
    unittest.main()
