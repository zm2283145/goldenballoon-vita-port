#!/usr/bin/env python3
"""Positive and adversarial controls for the Workshop acceptance receipt."""

from __future__ import annotations

import contextlib
import copy
import hashlib
import importlib.util
import io
import json
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "check_character_release_evidence",
    ROOT / "tools" / "check_character_release_evidence.py",
)
assert SPEC is not None and SPEC.loader is not None
evidence = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = evidence
SPEC.loader.exec_module(evidence)


def digest(label: str) -> str:
    return hashlib.sha256(label.encode("utf-8")).hexdigest()


def pass_status(note: str = "") -> dict[str, str]:
    return {"status": "pass", "note": note}


def valid_record() -> dict:
    version = "1.6.0"
    artifacts = [
        {
            "role": "macos",
            "filename": f"Golden-Balloon-{version}-macos-arm64-unsigned.dmg",
            "sha256": digest("macos artifact"),
            "provenance_sha256": digest("macos provenance"),
        },
        {
            "role": "windows",
            "filename": f"Golden-Balloon-{version}-windows-x64.zip",
            "sha256": digest("windows artifact"),
            "provenance_sha256": digest("windows provenance"),
        },
        {
            "role": "linux_appimage",
            "filename": f"Golden-Balloon-{version}-linux-x86_64.AppImage",
            "sha256": digest("linux appimage"),
            "provenance_sha256": digest("linux appimage provenance"),
        },
        {
            "role": "linux_tarball",
            "filename": f"Golden-Balloon-{version}-linux-x86_64.tar.gz",
            "sha256": digest("linux tarball"),
            "provenance_sha256": digest("linux tarball provenance"),
        },
    ]
    runs = []
    for index, platform in enumerate(evidence.PLATFORMS):
        platform_digests = [
            artifact["sha256"]
            for artifact in artifacts
            if evidence.ROLE_PLATFORM[artifact["role"]] == platform
        ]
        contexts = {}
        for context in evidence.CONTEXTS:
            contexts[context] = {
                "status": "pass",
                "note": "",
                "views": {
                    view: pass_status() for view in evidence.VIEWS
                },
            }
        runs.append(
            {
                "platform": platform,
                "artifact_sha256s": platform_digests,
                "observed_at": f"2026-08-{20 + index:02d}T12:30:00Z",
                "os": f"{platform} test OS",
                "gpu": f"physical {platform} GPU",
                "driver": f"{platform} driver 1.0",
                "display": "2560x1440 at 100% and 200%",
                "modalities": {
                    name: pass_status(f"Observed {name} device or tool.")
                    for name in evidence.MODALITIES
                },
                "layouts": {
                    name: pass_status() for name in evidence.LAYOUTS
                },
                "accessibility": {
                    name: pass_status() for name in evidence.ACCESSIBILITY
                },
                "journeys": {
                    name: pass_status() for name in evidence.JOURNEYS
                },
                "exact_contexts": contexts,
                "player_layouts": {
                    name: pass_status() for name in evidence.PLAYER_LAYOUTS
                },
                "identity_surfaces": {
                    name: pass_status()
                    for name in evidence.IDENTITY_SURFACES
                },
                "notes": "Evidence is normalized; private captures retained separately.",
            }
        )
    # Touch may be genuinely unavailable on two hosts, but the release must
    # still contain one observed passing touch run and a reason for each N/A.
    for run in (runs[0], runs[2]):
        run["modalities"]["touch"] = {
            "status": "not_available",
            "note": "This physical host has no touch digitizer.",
        }
    return {
        "schema": evidence.SCHEMA,
        "candidate": {
            "version": version,
            "commit": "0123456789abcdef0123456789abcdef01234567",
        },
        "package": {
            "id": "org.mdkr.release-fixture",
            "source_sha256": digest("source"),
            "package_sha256": digest("package"),
            "license_sha256": digest("license"),
            "evidence_report_sha256": digest("report"),
        },
        "artifacts": artifacts,
        "runs": runs,
        "device_profiles": [
            {
                "tier": tier,
                "platform": evidence.PLATFORMS[index],
                "profile_sha256": digest(f"device {tier}"),
                "result": "pass",
                "note": "",
            }
            for index, tier in enumerate(evidence.DEVICE_TIERS)
        ],
        "notes": "No ROM, model, screenshot, local path, or observer identity.",
    }


class CharacterReleaseEvidenceTests(unittest.TestCase):
    def test_public_schema_tracks_verifier_enumerations(self) -> None:
        schema = json.loads(
            (
                ROOT
                / "docs"
                / "ref"
                / "mdkr-character-release-acceptance-v1.schema.json"
            ).read_text(encoding="utf-8")
        )
        self.assertEqual(schema["properties"]["schema"]["const"], evidence.SCHEMA)
        definitions = schema["$defs"]
        self.assertEqual(
            set(definitions["modalities"]["required"]), set(evidence.MODALITIES)
        )
        self.assertEqual(
            set(definitions["journeys"]["required"]), set(evidence.JOURNEYS)
        )
        self.assertEqual(
            set(definitions["exactContexts"]["required"]), set(evidence.CONTEXTS)
        )
        self.assertEqual(
            set(definitions["identitySurfaces"]["required"]),
            set(evidence.IDENTITY_SURFACES),
        )
        for definition, names in (
            ("layouts", evidence.LAYOUTS),
            ("accessibility", evidence.ACCESSIBILITY),
            ("views", evidence.VIEWS),
            ("playerLayouts", evidence.PLAYER_LAYOUTS),
        ):
            self.assertEqual(set(definitions[definition]["required"]), set(names))
        self.assertEqual(
            set(definitions["artifact"]["properties"]["role"]["enum"]),
            set(evidence.ARTIFACT_ROLES),
        )
        self.assertEqual(
            set(definitions["run"]["properties"]["platform"]["enum"]),
            set(evidence.PLATFORMS),
        )
        self.assertEqual(
            set(definitions["deviceProfile"]["properties"]["tier"]["enum"]),
            set(evidence.DEVICE_TIERS),
        )

    def test_complete_cross_platform_receipt_passes(self) -> None:
        self.assertEqual(evidence.validate_record(valid_record()), [])

    def test_every_platform_and_artifact_role_is_required(self) -> None:
        record = valid_record()
        record["runs"] = record["runs"][:-1]
        record["artifacts"] = [
            artifact
            for artifact in record["artifacts"]
            if artifact["role"] != "linux_tarball"
        ]
        problems = evidence.validate_record(record)
        self.assertTrue(any("missing 'linux'" in item for item in problems), problems)
        self.assertTrue(
            any("missing required role 'linux_tarball'" in item for item in problems),
            problems,
        )

    def test_failed_journey_or_view_blocks_release(self) -> None:
        record = valid_record()
        record["runs"][0]["journeys"]["failure_recovery"] = {
            "status": "fail",
            "note": "Recovery lost the selected draft.",
        }
        record["runs"][1]["exact_contexts"]["plane"]["views"]["underside"] = {
            "status": "fail",
            "note": "Plane underside clipped.",
        }
        problems = evidence.validate_record(record)
        self.assertTrue(any("failure_recovery" in item for item in problems), problems)
        self.assertTrue(any("underside" in item for item in problems), problems)

    def test_not_available_requires_reason_and_global_coverage(self) -> None:
        record = valid_record()
        for run in record["runs"]:
            run["modalities"]["controller"] = {
                "status": "not_available",
                "note": "",
            }
        problems = evidence.validate_record(record)
        self.assertTrue(
            any("controller" in item and "requires" in item for item in problems),
            problems,
        )
        self.assertTrue(
            any("controller" in item and "not observed passing" in item for item in problems),
            problems,
        )

    def test_passing_modality_names_observed_device_or_tool(self) -> None:
        record = valid_record()
        record["runs"][0]["modalities"]["screen_reader"]["note"] = ""
        problems = evidence.validate_record(record)
        self.assertTrue(
            any("screen_reader.note" in item and "description" in item for item in problems),
            problems,
        )

    def test_performance_exception_is_visible_and_justified(self) -> None:
        record = valid_record()
        record["device_profiles"][0]["result"] = "exception"
        record["device_profiles"][0]["note"] = "30 fps target retained for this tier."
        self.assertEqual(evidence.validate_record(record), [])
        record["device_profiles"][0]["note"] = ""
        self.assertTrue(
            any("exception requires" in item for item in evidence.validate_record(record))
        )

    def test_placeholders_paths_and_substituted_artifacts_fail(self) -> None:
        record = valid_record()
        record["package"]["source_sha256"] = "0" * 64
        record["package"]["id"] = "org.example.license-clean-character"
        record["runs"][0]["notes"] = "Capture at /Users/person/Desktop/private.png"
        record["runs"][0]["modalities"]["mouse"]["note"] = (
            "Replace with observed evidence."
        )
        record["artifacts"][0]["filename"] = "nested/candidate.dmg"
        record["runs"][1]["artifact_sha256s"] = [digest("substitute")]
        problems = evidence.validate_record(record)
        self.assertTrue(any("placeholder" in item for item in problems), problems)
        self.assertTrue(any("template package id" in item for item in problems), problems)
        self.assertTrue(any("private machine path" in item for item in problems), problems)
        self.assertTrue(any("basename" in item for item in problems), problems)
        self.assertTrue(any("must name every windows artifact" in item for item in problems), problems)

    def test_unknown_fields_and_duplicate_device_tiers_fail(self) -> None:
        record = valid_record()
        record["approved"] = True
        record["device_profiles"][1]["tier"] = "low"
        problems = evidence.validate_record(record)
        self.assertTrue(any("unknown field 'approved'" in item for item in problems), problems)
        self.assertTrue(any("duplicate 'low'" in item for item in problems), problems)

    def test_artifact_directory_hashes_exact_bytes(self) -> None:
        record = valid_record()
        with tempfile.TemporaryDirectory(
            prefix="mdkr-character-release-artifacts-"
        ) as temporary:
            root = Path(temporary)
            for artifact in record["artifacts"]:
                payload = f"artifact:{artifact['role']}".encode("utf-8")
                artifact["sha256"] = hashlib.sha256(payload).hexdigest()
                provenance = json.dumps(
                    {
                        "artifact": artifact["filename"],
                        "commit": record["candidate"]["commit"],
                        "platform": evidence.ROLE_PLATFORM[artifact["role"]],
                        "sha256": artifact["sha256"],
                        "source_dirty": False,
                        "version": record["candidate"]["version"],
                    },
                    sort_keys=True,
                ).encode("utf-8")
                (root / artifact["filename"]).write_bytes(payload)
                (root / f"{artifact['filename']}.provenance.json").write_bytes(
                    provenance
                )
                artifact["provenance_sha256"] = hashlib.sha256(
                    provenance
                ).hexdigest()
            for run in record["runs"]:
                run["artifact_sha256s"] = [
                    artifact["sha256"]
                    for artifact in record["artifacts"]
                    if evidence.ROLE_PLATFORM[artifact["role"]]
                    == run["platform"]
                ]
            self.assertEqual(
                evidence.validate_record(record, artifact_dir=root), []
            )
            receipt = root / "acceptance.json"
            receipt.write_text(json.dumps(record), encoding="utf-8")
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                self.assertEqual(
                    evidence.main(
                        [str(receipt), "--artifact-dir", str(root)]
                    ),
                    0,
                )
            self.assertIn("PASS --", output.getvalue())
            self.assertNotIn("NOT RELEASE APPROVAL", output.getvalue())
            first = record["artifacts"][0]
            (root / first["filename"]).write_bytes(b"changed")
            self.assertTrue(
                any(
                    "artifact bytes do not match" in item
                    for item in evidence.validate_record(record, artifact_dir=root)
                )
            )

    def test_provenance_sidecar_must_bind_candidate_and_artifact(self) -> None:
        record = valid_record()
        with tempfile.TemporaryDirectory(
            prefix="mdkr-character-release-provenance-"
        ) as temporary:
            root = Path(temporary)
            for artifact in record["artifacts"]:
                payload = f"artifact:{artifact['role']}".encode("utf-8")
                artifact["sha256"] = hashlib.sha256(payload).hexdigest()
                provenance_record = {
                    "artifact": artifact["filename"],
                    "commit": record["candidate"]["commit"],
                    "platform": evidence.ROLE_PLATFORM[artifact["role"]],
                    "sha256": artifact["sha256"],
                    "source_dirty": False,
                    "version": record["candidate"]["version"],
                }
                if artifact["role"] == "windows":
                    provenance_record["commit"] = "f" * 40
                provenance = json.dumps(provenance_record, sort_keys=True).encode()
                (root / artifact["filename"]).write_bytes(payload)
                (root / f"{artifact['filename']}.provenance.json").write_bytes(
                    provenance
                )
                artifact["provenance_sha256"] = hashlib.sha256(provenance).hexdigest()
            for run in record["runs"]:
                run["artifact_sha256s"] = [
                    artifact["sha256"]
                    for artifact in record["artifacts"]
                    if evidence.ROLE_PLATFORM[artifact["role"]] == run["platform"]
                ]
            problems = evidence.validate_record(record, artifact_dir=root)
            self.assertTrue(
                any("provenance 'commit' does not bind" in item for item in problems),
                problems,
            )

    def test_real_dates_and_private_identity_are_required(self) -> None:
        record = valid_record()
        record["runs"][0]["observed_at"] = "2026-02-31T12:30:00Z"
        record["runs"][1]["notes"] = "file:///home/person/private/capture.png"
        record["runs"][2]["notes"] = "Observed by person@example.com"
        problems = evidence.validate_record(record)
        self.assertTrue(any("real calendar date" in item for item in problems), problems)
        self.assertTrue(any("private machine path" in item for item in problems), problems)
        self.assertTrue(any("email address" in item for item in problems), problems)

    def test_cli_template_is_exclusive_and_intentionally_fails(self) -> None:
        with tempfile.TemporaryDirectory(
            prefix="mdkr-character-release-template-"
        ) as temporary:
            path = Path(temporary) / "acceptance.json"
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(
                    evidence.main(["--write-template", str(path)]), 0
                )
            template = json.loads(path.read_text(encoding="utf-8"))
            self.assertTrue(evidence.validate_record(template))
            with contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(
                    evidence.main(["--write-template", str(path)]), 2
                )

    def test_cli_reports_receipt_digest_without_mutation(self) -> None:
        with tempfile.TemporaryDirectory(
            prefix="mdkr-character-release-receipt-"
        ) as temporary:
            path = Path(temporary) / "acceptance.json"
            before = json.dumps(valid_record(), indent=2, sort_keys=True) + "\n"
            path.write_text(before, encoding="utf-8")
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                self.assertEqual(evidence.main([str(path), "--structure-only"]), 0)
            self.assertIn("platforms=3", output.getvalue())
            self.assertIn("NOT RELEASE APPROVAL", output.getvalue())
            self.assertIn(hashlib.sha256(before.encode()).hexdigest(), output.getvalue())
            self.assertEqual(path.read_text(encoding="utf-8"), before)

    def test_cli_requires_artifact_bytes_for_release_approval(self) -> None:
        with tempfile.TemporaryDirectory(
            prefix="mdkr-character-release-required-artifacts-"
        ) as temporary:
            path = Path(temporary) / "acceptance.json"
            path.write_text(json.dumps(valid_record()), encoding="utf-8")
            with self.assertRaises(SystemExit), contextlib.redirect_stderr(io.StringIO()):
                evidence.main([str(path)])

    def test_cli_rejects_oversized_receipt_before_json_decode(self) -> None:
        with tempfile.TemporaryDirectory(
            prefix="mdkr-character-release-oversized-"
        ) as temporary:
            path = Path(temporary) / "acceptance.json"
            path.write_bytes(b" " * (evidence.MAX_RECEIPT_BYTES + 1))
            with contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(
                    evidence.main([str(path), "--structure-only"]), 2
                )


if __name__ == "__main__":
    unittest.main()
