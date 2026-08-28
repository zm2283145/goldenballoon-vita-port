#!/usr/bin/env python3
"""Validate the complete, privacy-bounded Character Workshop release receipt.

The receipt deliberately contains digests and normalized observations, not ROM
bytes, character assets, screenshots, local paths, account names, or free-form
operator identity.  It complements automated gates: a green source-tree test
cannot prove that the exact packaged artifacts were usable with real input,
assistive technology, and representative physical GPUs.
"""

from __future__ import annotations

import argparse
import datetime
import hashlib
import json
import os
import re
import sys
from collections.abc import Mapping, Sequence
from pathlib import Path, PurePath
from typing import Any


SCHEMA = "mdkr-character-release-acceptance/1"
PLATFORMS = ("macos", "windows", "linux")
ARTIFACT_ROLES = (
    "macos",
    "windows",
    "linux_appimage",
    "linux_tarball",
)
ROLE_PLATFORM = {
    "macos": "macos",
    "windows": "windows",
    "linux_appimage": "linux",
    "linux_tarball": "linux",
}
MODALITIES = ("mouse", "keyboard", "controller", "touch", "screen_reader")
LAYOUTS = ("ordinary", "scale_200_percent", "narrow_640x480")
ACCESSIBILITY = ("reduced_motion", "colour_vision")
JOURNEYS = (
    "rom_free_intake",
    "failure_recovery",
    "offset_studio",
    "rig_and_motion",
    "portrait_identity",
    "unicode_identity",
    "exact_test_matrix",
    "package_lifecycle",
    "authority_and_fallback",
)
CONTEXTS = ("select", "car", "hovercraft", "plane")
VIEWS = ("front", "side", "top", "underside")
PLAYER_LAYOUTS = ("1p", "2p", "3p", "4p")
IDENTITY_SURFACES = (
    "character_select",
    "hud",
    "results_rankings",
    "minimap",
    "collection_flag",
    "custom_roster",
)
DEVICE_TIERS = ("low", "mid", "high")
MAX_RECEIPT_BYTES = 1024 * 1024
MAX_PROVENANCE_BYTES = 64 * 1024
TEMPLATE_PLACEHOLDERS = {
    "Replace with observed evidence.",
    "Record exact OS version",
    "Record physical GPU",
    "Record driver/runtime version",
    "Record display and scale",
    "Replace with an observed physical-device result.",
}

HEX40_RE = re.compile(r"^[0-9a-f]{40}$")
HEX64_RE = re.compile(r"^[0-9a-f]{64}$")
VERSION_RE = re.compile(r"^(?:dev|[0-9]+(?:\.[0-9]+){1,2})$")
UTC_RE = re.compile(
    r"^[0-9]{4}-(?:0[1-9]|1[0-2])-(?:0[1-9]|[12][0-9]|3[01])"
    r"T(?:[01][0-9]|2[0-3]):[0-5][0-9]:[0-5][0-9]Z$"
)
PRIVATE_PATH_RE = re.compile(
    r"(?:^|[^A-Za-z0-9])/(?:Users|home)/[^\s]+|[A-Za-z]:\\Users\\[^\s]+",
    re.IGNORECASE,
)
EMAIL_RE = re.compile(
    r"(?<![A-Za-z0-9.!#$%&'*+/=?^_`{|}~-])"
    r"[A-Za-z0-9.!#$%&'*+/=?^_`{|}~-]+@"
    r"[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?"
    r"(?:\.[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?)+"
)


class EvidenceError(RuntimeError):
    """Malformed command input, distinct from an incomplete receipt."""


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _is_object(value: Any) -> bool:
    return isinstance(value, Mapping)


def _is_array(value: Any) -> bool:
    return isinstance(value, Sequence) and not isinstance(
        value, (str, bytes, bytearray)
    )


def _keys(
    value: Any,
    path: str,
    required: Sequence[str],
    optional: Sequence[str],
    problems: list[str],
) -> Mapping[str, Any] | None:
    if not _is_object(value):
        problems.append(f"{path}: expected an object")
        return None
    actual = set(value)
    missing = set(required) - actual
    extra = actual - set(required) - set(optional)
    for key in sorted(missing):
        problems.append(f"{path}: missing {key!r}")
    for key in sorted(extra):
        problems.append(f"{path}: unknown field {key!r}")
    return value


def _text(
    value: Any,
    path: str,
    problems: list[str],
    *,
    maximum: int = 500,
    allow_empty: bool = False,
) -> str | None:
    if not isinstance(value, str):
        problems.append(f"{path}: expected text")
        return None
    if (not allow_empty and not value.strip()) or len(value) > maximum:
        qualifier = "non-empty " if not allow_empty else ""
        problems.append(f"{path}: expected {qualifier}text up to {maximum} characters")
        return None
    if any(ord(character) < 0x20 and character not in "\t" for character in value):
        problems.append(f"{path}: contains a control character")
    if value.strip() in TEMPLATE_PLACEHOLDERS:
        problems.append(f"{path}: still contains a template placeholder")
    if PRIVATE_PATH_RE.search(value):
        problems.append(f"{path}: contains a private machine path")
    if EMAIL_RE.search(value):
        problems.append(f"{path}: contains an email address")
    return value


def _digest(value: Any, path: str, problems: list[str]) -> str | None:
    text = _text(value, path, problems, maximum=64)
    if text is not None and (
        HEX64_RE.fullmatch(text) is None or len(set(text)) == 1
    ):
        problems.append(f"{path}: expected a non-placeholder lowercase SHA-256")
        return None
    return text


def _exact_key_object(
    value: Any,
    path: str,
    names: Sequence[str],
    problems: list[str],
) -> Mapping[str, Any] | None:
    return _keys(value, path, names, (), problems)


def _status(
    value: Any,
    path: str,
    problems: list[str],
    *,
    allowed: Sequence[str],
    must_pass: bool,
) -> str | None:
    record = _keys(value, path, ("status", "note"), (), problems)
    if record is None:
        return None
    state = record.get("status")
    note = _text(
        record.get("note"),
        f"{path}.note",
        problems,
        maximum=500,
        allow_empty=True,
    )
    if state not in allowed:
        problems.append(f"{path}.status: expected one of {', '.join(allowed)}")
        return None
    if state != "pass" and (note is None or not note.strip()):
        problems.append(f"{path}.note: {state!r} requires a corrective explanation")
    if must_pass and state != "pass":
        problems.append(f"{path}: required release observation did not pass")
    return state


def _expected_artifact_name(role: str, version: str, filename: str) -> bool:
    if role == "windows":
        return filename == f"Golden-Balloon-{version}-windows-x64.zip"
    if role == "linux_appimage":
        return filename == f"Golden-Balloon-{version}-linux-x86_64.AppImage"
    if role == "linux_tarball":
        return filename == f"Golden-Balloon-{version}-linux-x86_64.tar.gz"
    return filename in {
        f"Golden-Balloon-{version}-macos-arm64-unsigned.dmg",
        f"Golden-Balloon-{version}-macos-arm64-signed-notarized.dmg",
    }


def _validate_artifacts(
    value: Any,
    version: str,
    commit: str,
    artifact_dir: Path | None,
    problems: list[str],
) -> dict[str, dict[str, Any]]:
    found: dict[str, dict[str, Any]] = {}
    if not _is_array(value):
        problems.append("artifacts: expected an array")
        return found
    for index, item in enumerate(value):
        path = f"artifacts[{index}]"
        record = _keys(
            item,
            path,
            ("role", "filename", "sha256", "provenance_sha256"),
            (),
            problems,
        )
        if record is None:
            continue
        role = record.get("role")
        if role not in ARTIFACT_ROLES:
            problems.append(f"{path}.role: expected one of {', '.join(ARTIFACT_ROLES)}")
            continue
        if role in found:
            problems.append(f"{path}.role: duplicate {role!r}")
            continue
        filename = _text(record.get("filename"), f"{path}.filename", problems, maximum=180)
        digest = _digest(record.get("sha256"), f"{path}.sha256", problems)
        provenance_digest = _digest(
            record.get("provenance_sha256"),
            f"{path}.provenance_sha256",
            problems,
        )
        if filename is not None:
            if PurePath(filename).name != filename or "/" in filename or "\\" in filename:
                problems.append(f"{path}.filename: must be a basename, not a path")
            elif not _expected_artifact_name(role, version, filename):
                problems.append(
                    f"{path}.filename: does not match role {role!r} and version {version!r}"
                )
        found[role] = {
            "role": role,
            "filename": filename,
            "sha256": digest,
            "provenance_sha256": provenance_digest,
        }
        if artifact_dir is not None and filename is not None:
            artifact = artifact_dir / filename
            provenance = artifact_dir / f"{filename}.provenance.json"
            if not artifact.is_file():
                problems.append(f"{path}: artifact is absent from --artifact-dir")
            elif digest is not None and _sha256(artifact) != digest:
                problems.append(f"{path}.sha256: artifact bytes do not match")
            if not provenance.is_file():
                problems.append(f"{path}: provenance sidecar is absent from --artifact-dir")
            else:
                if provenance_digest is not None and _sha256(provenance) != provenance_digest:
                    problems.append(f"{path}.provenance_sha256: sidecar bytes do not match")
                try:
                    provenance_size = provenance.stat().st_size
                except OSError as error:
                    problems.append(f"{path}: cannot inspect provenance sidecar: {error}")
                    continue
                if provenance_size > MAX_PROVENANCE_BYTES:
                    problems.append(
                        f"{path}: provenance sidecar exceeds {MAX_PROVENANCE_BYTES} bytes"
                    )
                else:
                    try:
                        with provenance.open("r", encoding="utf-8") as stream:
                            provenance_record = json.load(stream)
                    except (OSError, UnicodeError, json.JSONDecodeError, RecursionError) as error:
                        problems.append(f"{path}: cannot parse provenance sidecar: {error}")
                    else:
                        if not _is_object(provenance_record):
                            problems.append(f"{path}: provenance sidecar must be an object")
                        else:
                            expected = {
                                "artifact": filename,
                                "sha256": digest,
                                "version": version,
                                "commit": commit,
                                "platform": ROLE_PLATFORM[role],
                                "source_dirty": False,
                            }
                            for field, expected_value in expected.items():
                                if provenance_record.get(field) != expected_value:
                                    problems.append(
                                        f"{path}: provenance {field!r} does not bind "
                                        "the accepted candidate"
                                    )
    for role in ARTIFACT_ROLES:
        if role not in found:
            problems.append(f"artifacts: missing required role {role!r}")
    for field in ("sha256", "provenance_sha256"):
        digests = [
            artifact[field]
            for artifact in found.values()
            if isinstance(artifact.get(field), str)
        ]
        if len(digests) > 1 and len(set(digests)) != len(digests):
            problems.append(f"artifacts: {field} values must be distinct")
    return found


def _validate_run(
    value: Any,
    index: int,
    artifacts: Mapping[str, Mapping[str, Any]],
    modality_passes: dict[str, int],
    problems: list[str],
) -> str | None:
    path = f"runs[{index}]"
    record = _keys(
        value,
        path,
        (
            "platform",
            "artifact_sha256s",
            "observed_at",
            "os",
            "gpu",
            "driver",
            "display",
            "modalities",
            "layouts",
            "accessibility",
            "journeys",
            "exact_contexts",
            "player_layouts",
            "identity_surfaces",
        ),
        ("notes",),
        problems,
    )
    if record is None:
        return None
    platform = record.get("platform")
    if platform not in PLATFORMS:
        problems.append(f"{path}.platform: expected one of {', '.join(PLATFORMS)}")
        return None
    for field in ("os", "gpu", "driver", "display"):
        _text(record.get(field), f"{path}.{field}", problems, maximum=300)
    observed = _text(record.get("observed_at"), f"{path}.observed_at", problems, maximum=20)
    if observed is not None:
        if UTC_RE.fullmatch(observed) is None:
            problems.append(f"{path}.observed_at: expected UTC YYYY-MM-DDTHH:MM:SSZ")
        else:
            try:
                datetime.datetime.strptime(observed, "%Y-%m-%dT%H:%M:%SZ")
            except ValueError:
                problems.append(f"{path}.observed_at: expected a real calendar date")
    if "notes" in record:
        _text(record.get("notes"), f"{path}.notes", problems, maximum=1000, allow_empty=True)

    expected_digests = sorted(
        digest
        for role, artifact in artifacts.items()
        if ROLE_PLATFORM.get(role) == platform
        for digest in (artifact.get("sha256"),)
        if isinstance(digest, str)
    )
    actual_digests = record.get("artifact_sha256s")
    if not _is_array(actual_digests):
        problems.append(f"{path}.artifact_sha256s: expected an array")
    else:
        checked = [
            _digest(value, f"{path}.artifact_sha256s[{item}]", problems)
            for item, value in enumerate(actual_digests)
        ]
        if sorted(value for value in checked if value is not None) != expected_digests:
            problems.append(
                f"{path}.artifact_sha256s: must name every {platform} artifact exactly once"
            )

    modalities = _exact_key_object(
        record.get("modalities"), f"{path}.modalities", MODALITIES, problems
    )
    if modalities is not None:
        for name in MODALITIES:
            state = _status(
                modalities.get(name),
                f"{path}.modalities.{name}",
                problems,
                allowed=("pass", "fail", "not_available"),
                must_pass=False,
            )
            if state == "fail":
                problems.append(f"{path}.modalities.{name}: failed modality blocks release")
            elif state == "pass":
                modality_passes[name] += 1
                modality = modalities.get(name)
                note = modality.get("note") if _is_object(modality) else None
                if not isinstance(note, str) or not note.strip():
                    problems.append(
                        f"{path}.modalities.{name}.note: a passing modality requires "
                        "the normalized device or assistive-tool description"
                    )

    for field, names in (
        ("layouts", LAYOUTS),
        ("accessibility", ACCESSIBILITY),
        ("journeys", JOURNEYS),
        ("player_layouts", PLAYER_LAYOUTS),
        ("identity_surfaces", IDENTITY_SURFACES),
    ):
        group = _exact_key_object(record.get(field), f"{path}.{field}", names, problems)
        if group is not None:
            for name in names:
                _status(
                    group.get(name),
                    f"{path}.{field}.{name}",
                    problems,
                    allowed=("pass", "fail"),
                    must_pass=True,
                )

    contexts = _exact_key_object(
        record.get("exact_contexts"),
        f"{path}.exact_contexts",
        CONTEXTS,
        problems,
    )
    if contexts is not None:
        for context in CONTEXTS:
            context_path = f"{path}.exact_contexts.{context}"
            context_record = _keys(
                contexts.get(context), context_path, ("status", "note", "views"), (), problems
            )
            if context_record is None:
                continue
            _status(
                {"status": context_record.get("status"), "note": context_record.get("note")},
                context_path,
                problems,
                allowed=("pass", "fail"),
                must_pass=True,
            )
            views = _exact_key_object(
                context_record.get("views"), f"{context_path}.views", VIEWS, problems
            )
            if views is not None:
                for view in VIEWS:
                    _status(
                        views.get(view),
                        f"{context_path}.views.{view}",
                        problems,
                        allowed=("pass", "fail"),
                        must_pass=True,
                    )
    return platform


def _validate_devices(value: Any, problems: list[str]) -> None:
    if not _is_array(value):
        problems.append("device_profiles: expected an array")
        return
    tiers: set[str] = set()
    for index, item in enumerate(value):
        path = f"device_profiles[{index}]"
        record = _keys(
            item,
            path,
            ("tier", "platform", "profile_sha256", "result", "note"),
            (),
            problems,
        )
        if record is None:
            continue
        tier = record.get("tier")
        if tier not in DEVICE_TIERS:
            problems.append(f"{path}.tier: expected one of {', '.join(DEVICE_TIERS)}")
        elif tier in tiers:
            problems.append(f"{path}.tier: duplicate {tier!r}")
        else:
            tiers.add(tier)
        if record.get("platform") not in PLATFORMS:
            problems.append(f"{path}.platform: expected one of {', '.join(PLATFORMS)}")
        _digest(record.get("profile_sha256"), f"{path}.profile_sha256", problems)
        result = record.get("result")
        note = _text(
            record.get("note"), f"{path}.note", problems, maximum=500, allow_empty=True
        )
        if result not in ("pass", "exception"):
            problems.append(f"{path}.result: expected 'pass' or 'exception'")
        elif result == "exception" and (note is None or not note.strip()):
            problems.append(f"{path}.note: a performance exception requires justification")
    for tier in DEVICE_TIERS:
        if tier not in tiers:
            problems.append(f"device_profiles: missing {tier!r} physical-device tier")


def validate_record(
    value: Any, *, artifact_dir: Path | None = None
) -> list[str]:
    problems: list[str] = []
    record = _keys(
        value,
        "$",
        ("schema", "candidate", "package", "artifacts", "runs", "device_profiles"),
        ("notes",),
        problems,
    )
    if record is None:
        return problems
    if record.get("schema") != SCHEMA:
        problems.append(f"schema: expected {SCHEMA!r}")
    candidate = _keys(
        record.get("candidate"), "candidate", ("version", "commit"), (), problems
    )
    version = ""
    commit = ""
    if candidate is not None:
        version_value = _text(candidate.get("version"), "candidate.version", problems, maximum=32)
        if version_value is not None:
            version = version_value
            if VERSION_RE.fullmatch(version) is None:
                problems.append("candidate.version: expected dev or bare semantic version")
        commit_value = _text(candidate.get("commit"), "candidate.commit", problems, maximum=40)
        if commit_value is not None:
            commit = commit_value
        if commit_value is not None and (
            HEX40_RE.fullmatch(commit_value) is None or len(set(commit_value)) == 1
        ):
            problems.append("candidate.commit: expected a non-placeholder lowercase Git commit")

    package = _keys(
        record.get("package"),
        "package",
        (
            "id",
            "source_sha256",
            "package_sha256",
            "license_sha256",
            "evidence_report_sha256",
        ),
        (),
        problems,
    )
    if package is not None:
        package_id = _text(package.get("id"), "package.id", problems, maximum=64)
        if package_id is not None and re.fullmatch(
            r"[a-z0-9]+(?:[.-][a-z0-9]+)+", package_id
        ) is None:
            problems.append("package.id: expected a stable reverse-domain package id")
        elif package_id == "org.example.license-clean-character":
            problems.append("package.id: still contains the template package id")
        for field in (
            "source_sha256",
            "package_sha256",
            "license_sha256",
            "evidence_report_sha256",
        ):
            _digest(package.get(field), f"package.{field}", problems)

    artifacts = _validate_artifacts(
        record.get("artifacts"), version, commit, artifact_dir, problems
    )
    modality_passes = {name: 0 for name in MODALITIES}
    runs = record.get("runs")
    run_platforms: set[str] = set()
    if not _is_array(runs):
        problems.append("runs: expected an array")
    else:
        for index, run in enumerate(runs):
            platform = _validate_run(
                run, index, artifacts, modality_passes, problems
            )
            if platform is not None:
                if platform in run_platforms:
                    problems.append(f"runs[{index}].platform: duplicate {platform!r}")
                run_platforms.add(platform)
    for platform in PLATFORMS:
        if platform not in run_platforms:
            problems.append(f"runs: missing {platform!r} packaged-candidate observation")
    for modality, passes in modality_passes.items():
        if passes == 0:
            problems.append(
                f"runs: modality {modality!r} was not observed passing on any platform"
            )
    _validate_devices(record.get("device_profiles"), problems)
    if "notes" in record:
        _text(record.get("notes"), "notes", problems, maximum=1000, allow_empty=True)
    return problems


def _status_template() -> dict[str, str]:
    return {"status": "fail", "note": "Replace with observed evidence."}


def template_record() -> dict[str, Any]:
    placeholder = "0" * 64
    commit = "0" * 40
    version = "dev"
    artifacts = []
    names = {
        "macos": f"Golden-Balloon-{version}-macos-arm64-unsigned.dmg",
        "windows": f"Golden-Balloon-{version}-windows-x64.zip",
        "linux_appimage": f"Golden-Balloon-{version}-linux-x86_64.AppImage",
        "linux_tarball": f"Golden-Balloon-{version}-linux-x86_64.tar.gz",
    }
    for role in ARTIFACT_ROLES:
        artifacts.append(
            {
                "role": role,
                "filename": names[role],
                "sha256": placeholder,
                "provenance_sha256": placeholder,
            }
        )
    runs = []
    for platform in PLATFORMS:
        contexts: dict[str, Any] = {}
        for context in CONTEXTS:
            contexts[context] = {
                **_status_template(),
                "views": {view: _status_template() for view in VIEWS},
            }
        runs.append(
            {
                "platform": platform,
                "artifact_sha256s": [placeholder],
                "observed_at": "YYYY-MM-DDTHH:MM:SSZ",
                "os": "Record exact OS version",
                "gpu": "Record physical GPU",
                "driver": "Record driver/runtime version",
                "display": "Record display and scale",
                "modalities": {name: _status_template() for name in MODALITIES},
                "layouts": {name: _status_template() for name in LAYOUTS},
                "accessibility": {
                    name: _status_template() for name in ACCESSIBILITY
                },
                "journeys": {name: _status_template() for name in JOURNEYS},
                "exact_contexts": contexts,
                "player_layouts": {
                    name: _status_template() for name in PLAYER_LAYOUTS
                },
                "identity_surfaces": {
                    name: _status_template() for name in IDENTITY_SURFACES
                },
                "notes": "No paths, ROM data, screenshots, or personal identity.",
            }
        )
    return {
        "schema": SCHEMA,
        "candidate": {"version": version, "commit": commit},
        "package": {
            "id": "org.example.license-clean-character",
            "source_sha256": placeholder,
            "package_sha256": placeholder,
            "license_sha256": placeholder,
            "evidence_report_sha256": placeholder,
        },
        "artifacts": artifacts,
        "runs": runs,
        "device_profiles": [
            {
                "tier": tier,
                "platform": "macos",
                "profile_sha256": placeholder,
                "result": "exception",
                "note": "Replace with an observed physical-device result.",
            }
            for tier in DEVICE_TIERS
        ],
        "notes": "Private captures remain outside this receipt.",
    }


def _write_template(path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL
    descriptor = os.open(path, flags, 0o600)
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8") as stream:
            json.dump(template_record(), stream, indent=2, ensure_ascii=False)
            stream.write("\n")
    except BaseException:
        path.unlink(missing_ok=True)
        raise


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("receipt", type=Path, nargs="?")
    parser.add_argument(
        "--artifact-dir",
        type=Path,
        help="also hash every artifact and provenance sidecar in this directory",
    )
    parser.add_argument(
        "--structure-only",
        action="store_true",
        help="preflight receipt structure without granting release approval",
    )
    parser.add_argument(
        "--write-template",
        type=Path,
        help="exclusively create an intentionally failing receipt template",
    )
    args = parser.parse_args(argv)
    if args.write_template is not None:
        if args.receipt is not None or args.artifact_dir is not None or args.structure_only:
            parser.error("--write-template cannot be combined with receipt validation")
        try:
            _write_template(args.write_template)
        except FileExistsError:
            print(
                f"check_character_release_evidence: refusing to overwrite {args.write_template}",
                file=sys.stderr,
            )
            return 2
        except OSError as error:
            print(f"check_character_release_evidence: {error}", file=sys.stderr)
            return 2
        print(f"wrote incomplete acceptance template: {args.write_template}")
        return 0
    if args.receipt is None:
        parser.error("a receipt path or --write-template is required")
    if args.structure_only and args.artifact_dir is not None:
        parser.error("--structure-only cannot be combined with --artifact-dir")
    if not args.structure_only and args.artifact_dir is None:
        parser.error(
            "--artifact-dir is required for release approval; use --structure-only "
            "only for a non-approving preflight"
        )
    try:
        receipt_size = args.receipt.stat().st_size
        if receipt_size > MAX_RECEIPT_BYTES:
            raise EvidenceError(f"receipt exceeds {MAX_RECEIPT_BYTES} bytes")
        with args.receipt.open("r", encoding="utf-8") as stream:
            record = json.load(stream)
    except (OSError, UnicodeError, json.JSONDecodeError, RecursionError, EvidenceError) as error:
        print(f"check_character_release_evidence: cannot read receipt: {error}", file=sys.stderr)
        return 2
    artifact_dir = args.artifact_dir
    if artifact_dir is not None and not artifact_dir.is_dir():
        print(
            f"check_character_release_evidence: --artifact-dir is not a directory: {artifact_dir}",
            file=sys.stderr,
        )
        return 2
    problems = validate_record(record, artifact_dir=artifact_dir)
    if problems:
        print(
            f"check_character_release_evidence: FAIL -- {len(problems)} problem(s)",
            file=sys.stderr,
        )
        for problem in problems:
            print(f"  - {problem}", file=sys.stderr)
        return 1
    receipt_digest = _sha256(args.receipt)
    exception_count = sum(
        1
        for item in record["device_profiles"]
        if item["result"] == "exception"
    )
    verdict = "STRUCTURE PASS -- NOT RELEASE APPROVAL" if args.structure_only else "PASS"
    print(
        f"check_character_release_evidence: {verdict} -- "
        f"commit={record['candidate']['commit']} "
        f"artifacts={len(record['artifacts'])} platforms={len(record['runs'])} "
        f"device_tiers={len(record['device_profiles'])} "
        f"performance_exceptions={exception_count} receipt_sha256={receipt_digest}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
