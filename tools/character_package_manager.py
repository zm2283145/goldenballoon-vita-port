#!/usr/bin/env python3
"""Install and manage private .mdkrchar packages and disposable MDKC caches.

The source package is retained for provenance. `<id>.mdkc` is the enabled
runtime record; `<id>.mdkc.disabled` is the same validated cache held outside
runtime discovery. Installs and revisions preserve that state and replace the
chosen cache atomically only after validation and compilation complete.
"""

from __future__ import annotations

import argparse
import hashlib
import io
import json
import math
import os
import stat
import struct
import sys
import tempfile
import time
import unicodedata
import zipfile
import zlib
from contextlib import contextmanager
from pathlib import Path
from typing import Any, Callable, Iterator

import character_asset_compiler as compiler
import character_asset_probe as probe
import character_manifest_wizard as wizard
import character_source_adapter as source_adapter
import collada_to_glb as collada
import gltf_validator_adapter as gltf_validator


MANAGER_SCHEMA = "mdkr-character-install-v1"
IMPORTER_INFO_SCHEMA = "mdkr-character-importer-info-v1"
COMPILER_ID = compiler.COMPILER_ID
LEGACY_COMPILER_IDS = tuple(
    f"mdkr-character-compiler/{version}" for version in range(7, 0, -1)
)
LOCK_NAME = ".character-import.lock"
MAX_REPORT_BYTES = 64 * 1024
MAX_UI_REVISIONS = 256
MAX_WORKSHOP_DRAFT_BYTES = 256 * 1024
RAW_INTAKE_INDEX_NAME = ".launcher-character-glb-intake.tsv"
RAW_INTAKE_CANDIDATE_NAME = ".launcher-character-raw-candidate.mdkrchar"
RAW_INTAKE_MAX_CLIPS = 256
RAW_INTAKE_MAX_NODES = 4096
RAW_INTAKE_MAX_CHOICE_BYTES = 256
ADAPTER_OUTPUT_INDEX_NAME = ".launcher-character-adapter-output.tsv"
FAILURE_SCHEMA = "mdkr-character-import-failure-v1"
FAILURE_INDEX_SCHEMA = "mdkr-character-import-failures-v1"
FAILURE_DIRECTORY_NAME = ".failed-character-imports"
FAILURE_INDEX_NAME = ".launcher-character-failures.tsv"
MAX_FAILURE_RECORDS = 4096
MAX_UI_FAILURES = 256
MAX_FAILURE_TEXT_BYTES = 8192
MAX_FAILURE_PATH_BYTES = 4096


class ManagerError(ValueError):
    pass


class ImportValidationError(ManagerError):
    """An author model failed with an optional complete bounded report."""

    def __init__(self, message: str,
                 validation: dict[str, Any] | None = None) -> None:
        super().__init__(message)
        self.validation = validation


def _validation_summary(validation: dict[str, Any]) -> dict[str, Any]:
    """Return bounded provenance and issue counts suitable for UI reports."""
    issues = validation["report"]["issues"]
    identity = validation["validator"]
    return {
        "schema": validation["schema"],
        "source_sha256": validation["source_sha256"],
        "validator_version": identity["version"],
        "validator_commit": identity["commit"],
        "validator_target": identity["target"],
        "validator_sha256": identity["executable_sha256"],
        "errors": issues["numErrors"],
        "warnings": issues["numWarnings"],
        "infos": issues["numInfos"],
        "hints": issues["numHints"],
        "diagnostics_truncated": issues["truncated"],
    }


def _validate_character_glb(payload: bytes, context: str) -> dict[str, Any]:
    """Require the pinned Khronos validator at every GLB trust boundary."""
    try:
        validation = gltf_validator.validate_glb_bytes(payload)
    except gltf_validator.ValidatorError as exc:
        raise ImportValidationError(
            f"{context} could not be checked by the pinned Khronos glTF "
            f"Validator: {exc}"
        ) from exc
    if validation["source_sha256"] != hashlib.sha256(payload).hexdigest():
        raise ManagerError(f"{context} validation digest disagrees with its bytes")
    if validation["valid"]:
        return validation
    issues = validation["report"]["issues"]
    errors = [
        f"{message['code']}: {message['message']}"
        for message in issues["messages"]
        if message["severity"] == 0
    ]
    detail = "; ".join(errors[:8])
    if len(errors) > 8 or issues["truncated"]:
        detail += ("; " if detail else "") + "additional diagnostics omitted"
    if not detail:
        detail = f"validator reported {issues['numErrors']} error(s)"
    raise ImportValidationError(
        f"{context} is not valid glTF 2.0: {detail}", validation
    )


def _archive_convertible_paths(
        report: dict[str, Any], prefix: tuple[str, ...] = ()
) -> list[tuple[tuple[str, ...], str]]:
    paths = [
        ((*prefix, str(model["path"])), str(model["format"]))
        for model in report.get("models", [])
        if model.get("format") in ("dae", "glb")
    ]
    for nested in report.get("nested_archives", []):
        paths.extend(_archive_convertible_paths(
            nested, (*prefix, str(nested.get("name", ""))))
        )
    return paths


def _selected_archive_payload(payload: bytes,
                              nested_paths: tuple[str, ...]) -> bytes:
    selected = payload
    for member in nested_paths:
        with zipfile.ZipFile(io.BytesIO(selected)) as archive:
            try:
                selected = archive.read(member)
            except KeyError as exc:
                raise ManagerError(
                    f"inspected nested archive member disappeared: {member}"
                ) from exc
    return selected


def convert_authoring_source(input_path: Path,
                             output_path: Path) -> dict[str, Any]:
    """Convert a DAE or extract one unambiguous DAE/GLB from a safe ZIP."""
    suffix = input_path.suffix.lower()
    if suffix not in (".dae", ".zip"):
        raise ManagerError("authoring conversion requires a .dae or .zip source")
    if output_path.suffix.lower() != ".glb":
        raise ManagerError("converted authoring output must use a .glb suffix")
    if output_path.parent.is_symlink() or not output_path.parent.is_dir():
        raise ManagerError("converted output directory must be a real existing directory")
    destination = output_path.parent.resolve() / output_path.name
    if destination.exists() or destination.is_symlink():
        raise ManagerError("converted output destination already exists")

    archive_report: dict[str, Any] | None = None
    source_label = input_path.name
    with tempfile.TemporaryDirectory(
            prefix="mdkr-character-authoring-convert-") as temporary:
        if suffix == ".dae":
            if input_path.is_symlink() or not input_path.is_file():
                raise ManagerError("COLLADA source must be a regular file")
            conversion_source = input_path
        else:
            archive_payload = _bounded_authoring_input(
                input_path, probe.MAX_INPUT_BYTES, "authoring ZIP")
            archive_report = probe.inspect_archive_bytes(
                archive_payload, input_path.name)
            candidates = _archive_convertible_paths(archive_report)
            if not candidates:
                raise ManagerError(
                    "archive has no convertible COLLADA (.dae) or GLB model; "
                    "export a self-contained GLB from Blender"
                )
            if len(candidates) != 1:
                labels = [
                    " > ".join(path) + f" ({kind})"
                    for path, kind in candidates[:8]
                ]
                raise ManagerError(
                    "archive has multiple convertible model candidates; extract and "
                    "choose one explicitly: " + "; ".join(labels)
                )
            candidate, candidate_kind = candidates[0]
            selected_payload = _selected_archive_payload(
                archive_payload, candidate[:-1])
            extraction_root = Path(temporary) / "source"
            extraction_root.mkdir()
            with zipfile.ZipFile(io.BytesIO(selected_payload)) as archive:
                for info in archive.infolist():
                    safe_name = probe._safe_archive_name(info.filename)
                    if info.is_dir():
                        continue
                    if info.flag_bits & 1 or probe._zip_member_is_symlink(info):
                        raise ManagerError(
                            f"unsafe archive member cannot be converted: {safe_name}"
                        )
                    target = extraction_root.joinpath(*Path(safe_name).parts)
                    target.parent.mkdir(parents=True, exist_ok=True)
                    _write_exclusive(target, archive.read(info))
            conversion_source = extraction_root.joinpath(
                *Path(candidate[-1]).parts)
            source_label = " > ".join(candidate)

        if suffix == ".zip" and candidate_kind == "glb":
            converted = _bounded_authoring_input(
                conversion_source, probe.MAX_INPUT_BYTES,
                "archived GLB model",
            )
            inspected = probe.inspect_glb_bytes(
                converted, require_character=True)
            if inspected["errors"]:
                raise ManagerError(
                    "archived GLB is not character-ready: " +
                    "; ".join(inspected["errors"])
                )
            report = {
                "vertices": inspected["vertex_count"],
                "triangles": inspected["triangle_count"],
                "joints": inspected["max_joints"],
                "materials": inspected["material_count"],
                "textures": inspected["texture_count"],
                "source_format": "glb",
            }
        else:
            converted, report = collada.convert(conversion_source)
        validation = _validate_character_glb(
            converted, f"converted model {source_label!r}"
        )
        try:
            _write_exclusive(destination, converted)
        except FileExistsError as exc:
            raise ManagerError(
                "converted output destination already exists"
            ) from exc
    return {
        "action": "convert-authoring-source",
        "input": str(input_path),
        "source_member": source_label,
        "converted_model": str(destination),
        "output_bytes": len(converted),
        "archive_license_present": bool(
            archive_report and not any(
                blocker == "no embedded license or copyright file"
                for blocker in archive_report.get("blockers", [])
            )
        ),
        "validation": _validation_summary(validation),
        **{
            key: value for key, value in report.items()
            if key not in ("input", "output")
        },
    }


def inspect_adapter_output(input_path: Path) -> dict[str, Any]:
    """Validate a data-only adapter handoff without extracting any member."""
    artifact = source_adapter.inspect_path(input_path)
    model_payload = artifact["model_payload"]
    validation = _validate_character_glb(
        model_payload, "third-party adapter output model"
    )
    report = probe.inspect_glb_bytes(model_payload, require_character=True)
    if report["errors"]:
        raise ManagerError(
            "adapter output GLB is not character-ready: " +
            "; ".join(report["errors"])
        )
    return {
        **{key: value for key, value in artifact.items()
           if not key.endswith("_payload")},
        "validation": _validation_summary(validation),
        "vertices": report["vertex_count"],
        "triangles": report["triangle_count"],
        "joints": report["max_joints"],
        "materials": report["material_count"],
        "textures": report["texture_count"],
    }


def write_adapter_output_index(input_path: Path, directory: Path,
                               index_path: Path) -> dict[str, Any]:
    """Write bounded review facts for the launcher's mutation-free review."""
    root = _prepare_directory(directory)
    if (index_path.parent.resolve() != root or
            index_path.name != ADAPTER_OUTPUT_INDEX_NAME):
        raise ManagerError(
            "adapter output index must use the launcher's exact file inside "
            "the character directory"
        )
    report = inspect_adapter_output(input_path)

    def encoded(value: str) -> str:
        return "-" if not value else value.encode("utf-8").hex()

    fields = (
        report["artifact_sha256"], report["model_sha256"],
        report["source_sha256"], str(report["artifact_bytes"]),
        str(report["model_bytes"]), str(report["vertices"]),
        str(report["triangles"]), str(report["joints"]),
        str(report["materials"]), str(report["textures"]),
        "1" if report["license_present"] else "0",
        report["license_sha256"] or "-",
        encoded(report["adapter_name"]),
        encoded(report["adapter_version"]),
        encoded(report["adapter_homepage"]),
        encoded(report["source_format"]),
        encoded(report["conversion_profile"]),
        report["conversion_settings_sha256"],
        encoded(report["license_spdx"]),
        encoded(report["license_attribution"]),
        encoded(report["license_source_url"]),
    )
    payload = (
        "mdkr-character-adapter-output-index-v1\n" +
        "\t".join(fields) + "\n"
    ).encode("ascii")
    _write_atomic(index_path, payload)
    return {**report, "index_file": index_path.name, "authenticated": False}


def _adapter_output_paths(model_output: Path, license_present: bool) -> tuple[
        Path, Path | None, Path]:
    if model_output.suffix.lower() != ".glb":
        raise ManagerError("adapter model output must use a .glb suffix")
    if model_output.parent.is_symlink() or not model_output.parent.is_dir():
        raise ManagerError("adapter output directory must be a real existing directory")
    model = model_output.parent.resolve() / model_output.name
    license_output = (
        model.with_name(model.stem + ".LICENSE.txt")
        if license_present else None
    )
    provenance = model.with_name(model.stem + ".mdkrsource.json")
    return model, license_output, provenance


def extract_reviewed_adapter_output(
        input_path: Path, output_path: Path, expected_artifact_sha256: str,
        expected_model_sha256: str) -> dict[str, Any]:
    """Revalidate and exclusively extract only the exact reviewed artifact."""
    for label, digest in (
            ("reviewed artifact", expected_artifact_sha256),
            ("reviewed model", expected_model_sha256)):
        if (len(digest) != 64 or
                any(character not in "0123456789abcdef" for character in digest)):
            raise ManagerError(f"{label} digest is invalid")
    artifact = source_adapter.inspect_path(input_path)
    if artifact["artifact_sha256"] != expected_artifact_sha256:
        raise ManagerError(
            "the adapter output changed after review; inspect the new bytes before extracting"
        )
    if artifact["model_sha256"] != expected_model_sha256:
        raise ManagerError(
            "the adapter model identity changed after review; inspect it again"
        )
    # Repeat the pinned validator at the committing boundary.  The review's
    # success is evidence, not permission to trust bytes re-read later.
    validation = _validate_character_glb(
        artifact["model_payload"], "reviewed adapter output model"
    )
    model, license_output, provenance_output = _adapter_output_paths(
        output_path, artifact["license_present"]
    )
    destinations = [model, provenance_output]
    if license_output is not None:
        destinations.append(license_output)
    for destination in destinations:
        if destination.exists() or destination.is_symlink():
            raise ManagerError(
                f"adapter extraction destination already exists: {destination.name}"
            )
    provenance = {
        "schema": source_adapter.SCHEMA,
        "integrity_only_not_signed": True,
        **{key: value for key, value in artifact.items()
           if not key.endswith("_payload")},
        "validation": _validation_summary(validation),
    }
    provenance_payload = (
        json.dumps(provenance, ensure_ascii=False, indent=2, sort_keys=True) + "\n"
    ).encode("utf-8")
    created: list[tuple[Path, os.stat_result, str]] = []
    try:
        model_payload = artifact["model_payload"]
        model_identity = _write_exclusive(model, model_payload)
        created.append((model, model_identity,
                        hashlib.sha256(model_payload).hexdigest()))
        if license_output is not None:
            license_payload = artifact["license_payload"]
            assert isinstance(license_payload, bytes)
            license_identity = _write_exclusive(license_output, license_payload)
            created.append((license_output, license_identity,
                            hashlib.sha256(license_payload).hexdigest()))
        provenance_identity = _write_exclusive(
            provenance_output, provenance_payload)
        created.append((provenance_output, provenance_identity,
                        hashlib.sha256(provenance_payload).hexdigest()))
    except Exception:
        for created_path, identity, digest in reversed(created):
            _unlink_created_exact(created_path, identity, digest)
        raise
    return {
        **provenance,
        "action": "extract-reviewed-adapter-output",
        "model_output": str(model),
        "license_output": str(license_output) if license_output else "",
        "provenance_output": str(provenance_output),
    }


def _bounded_authoring_input(path: Path, maximum: int, label: str) -> bytes:
    try:
        flags = os.O_RDONLY
        for flag in ("O_BINARY", "O_CLOEXEC", "O_NONBLOCK"):
            flags |= getattr(os, flag, 0)
        descriptor = os.open(path, flags)
        with os.fdopen(descriptor, "rb") as stream:
            information = os.fstat(stream.fileno())
            if not stat.S_ISREG(information.st_mode):
                raise ManagerError(f"{label} must resolve to a regular file")
            if information.st_size > maximum:
                raise ManagerError(f"{label} exceeds {maximum} bytes")
            payload = stream.read(maximum + 1)
    except OSError as exc:
        raise ManagerError(f"cannot read {label}: {exc}") from exc
    if len(payload) > maximum:
        raise ManagerError(f"{label} exceeds {maximum} bytes")
    return payload


def _decode_launcher_hex(value: str, maximum: int, label: str) -> str:
    if (len(value) > maximum * 2 or len(value) % 2 != 0 or
            any(character not in "0123456789abcdef" for character in value)):
        raise ManagerError(f"{label} has invalid bounded hex encoding")
    try:
        decoded = bytes.fromhex(value).decode("utf-8")
    except UnicodeDecodeError as exc:
        raise ManagerError(f"{label} is not valid UTF-8") from exc
    if not decoded or "\x00" in decoded:
        raise ManagerError(f"{label} must not be empty or contain NUL")
    return decoded


def _workshop_identity_name(value: Any, label: str,
                            maximum_bytes: int) -> str:
    """Validate user-facing draft text before touching retained source state."""
    if not isinstance(value, str) or not value or not value.strip():
        raise ManagerError(f"{label} must be non-empty printable UTF-8")
    if unicodedata.normalize("NFC", value) != value:
        raise ManagerError(f"{label} must use NFC-normalized Unicode")
    try:
        encoded = value.encode("utf-8")
    except UnicodeEncodeError as exc:
        raise ManagerError(f"{label} is not valid UTF-8") from exc
    if len(encoded) > maximum_bytes or any(
        ord(character) < 0x20
        or 0x7F <= ord(character) <= 0x9F
        or 0x200B <= ord(character) <= 0x200F
        or 0x2028 <= ord(character) <= 0x202E
        or 0x2060 <= ord(character) <= 0x206F
        or ord(character) == 0xFEFF
        for character in value
    ):
        raise ManagerError(f"{label} must be bounded printable UTF-8")
    return value


def _compiler_source_digest(archive: zipfile.ZipFile,
                            compiler_id: str = COMPILER_ID) -> bytes:
    manifest = probe.json_loads_strict(archive.read("manifest.json"), "manifest")
    members = (
        (name, archive.read(name))
        for name in probe.package_members_for_schema(manifest.get("schema"))
    )
    return compiler.source_digest(members, compiler_id)


def _compatible_source_digests(archive: zipfile.ZipFile) -> set[str]:
    """Authenticate retained caches emitted by every supported compiler."""
    return {
        _compiler_source_digest(archive, compiler_id).hex()
        for compiler_id in (COMPILER_ID, *LEGACY_COMPILER_IDS)
    }


def _same_file_identity(left: os.stat_result, right: os.stat_result) -> bool:
    return left.st_dev == right.st_dev and left.st_ino == right.st_ino


def _unlink_created_exact(path: Path, identity: os.stat_result,
                          digest: str | None = None) -> None:
    """Remove only the same regular file, optionally with the same full bytes."""
    descriptor = -1
    try:
        flags = os.O_RDONLY
        for flag in ("O_BINARY", "O_CLOEXEC", "O_NONBLOCK", "O_NOFOLLOW"):
            flags |= getattr(os, flag, 0)
        descriptor = os.open(path, flags)
        current = os.fstat(descriptor)
        if not stat.S_ISREG(current.st_mode) or not _same_file_identity(
                current, identity):
            return
        if digest is not None:
            observed = hashlib.sha256()
            with os.fdopen(descriptor, "rb") as stream:
                descriptor = -1
                while chunk := stream.read(1024 * 1024):
                    observed.update(chunk)
            if observed.hexdigest() != digest:
                return
        latest = path.lstat()
        if (stat.S_ISREG(latest.st_mode) and
                _same_file_identity(latest, identity)):
            path.unlink()
    except OSError:
        return
    finally:
        if descriptor >= 0:
            os.close(descriptor)


def _write_exclusive(path: Path, payload: bytes) -> os.stat_result:
    descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    identity = os.fstat(descriptor)
    try:
        output = os.fdopen(descriptor, "wb")
        descriptor = -1
        with output:
            output.write(payload)
            output.flush()
            os.fsync(output.fileno())
    except BaseException:
        if descriptor >= 0:
            os.close(descriptor)
        # The file may contain only a prefix, so identity—not the intended
        # final digest—is the safe cleanup predicate at this point.
        _unlink_created_exact(path, identity)
        raise
    return identity


def _write_atomic(path: Path, payload: bytes) -> None:
    temporary = path.with_name(f".{path.name}.{os.getpid()}.tmp")
    temporary.unlink(missing_ok=True)
    try:
        _write_exclusive(temporary, payload)
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


def _prepare_directory(directory: Path) -> Path:
    directory.mkdir(mode=0o700, parents=True, exist_ok=True)
    if directory.is_symlink() or not directory.is_dir():
        raise ManagerError("character directory must be a real directory")
    return directory.resolve()


def _failure_directory(root: Path, *, create: bool = True) -> Path:
    directory = root / FAILURE_DIRECTORY_NAME
    if create:
        directory.mkdir(mode=0o700, exist_ok=True)
    elif not directory.exists() and not directory.is_symlink():
        raise ManagerError("failed-import recovery record does not exist")
    if directory.is_symlink() or not directory.is_dir():
        raise ManagerError("failed-import recovery directory must be a real directory")
    resolved = directory.resolve(strict=True)
    if resolved.parent != root:
        raise ManagerError("failed-import recovery directory escaped character storage")
    return resolved


def _bounded_failure_text(value: str, maximum: int, label: str) -> str:
    if not isinstance(value, str):
        raise ManagerError(f"{label} must be text")
    encoded = value.encode("utf-8", errors="replace")
    if len(encoded) <= maximum:
        return encoded.decode("utf-8")
    suffix = "\n[diagnostic truncated]".encode("utf-8")
    clipped = encoded[:maximum - len(suffix)]
    while True:
        try:
            return clipped.decode("utf-8") + suffix.decode("utf-8")
        except UnicodeDecodeError:
            clipped = clipped[:-1]


def _failure_source_for_command(args: argparse.Namespace) -> tuple[
        Path | None, str | None, Path | None]:
    command = args.command
    if command == "write-raw-glb-index":
        return args.model, "raw-inspect", None
    if command == "convert-authoring-source":
        return args.input, "convert", args.output
    if command == "write-adapter-output-index":
        return args.input, "adapter-inspect", None
    if command == "extract-reviewed-adapter-output":
        # A failed committing extraction must return through a fresh review;
        # recovery never remembers consent or tries to recreate output files.
        return args.input, "adapter-inspect", None
    if command in ("install", "inspect", "write-candidate-index",
                   "install-reviewed", "prepare"):
        return args.package, "package-inspect", None
    if command == "build-raw-glb":
        try:
            source = Path(_decode_launcher_hex(
                args.model_hex, MAX_FAILURE_PATH_BYTES, "model path"
            ))
        except ManagerError:
            return None, None, None
        return source, "raw-inspect", None
    return None, None, None


def _read_failure_record(path: Path) -> dict[str, Any]:
    if path.is_symlink() or not path.is_file() or path.stat().st_size > MAX_REPORT_BYTES:
        raise ManagerError("failed-import recovery record is unsafe or oversized")
    record = probe.json_loads_strict(path.read_bytes(), "failed-import recovery record")
    keys = {
        "schema", "record_id", "command", "retry_kind", "source_path",
        "source_sha256", "source_bytes", "output_path", "error",
        "attempts", "first_failed_unix", "last_failed_unix",
        "validation_report_file",
    }
    if not isinstance(record, dict) or set(record) != keys:
        raise ManagerError("failed-import recovery record has an unexpected schema")
    record_id = record["record_id"]
    if (record["schema"] != FAILURE_SCHEMA or
            not isinstance(record_id, str) or len(record_id) != 64 or
            any(character not in "0123456789abcdef" for character in record_id) or
            path.name != f"{record_id}.json"):
        raise ManagerError("failed-import recovery record identity is invalid")
    for key in ("command", "retry_kind", "source_path", "error"):
        if not isinstance(record[key], str):
            raise ManagerError(f"failed-import recovery {key} is invalid")
    if record["retry_kind"] not in (
            "raw-inspect", "package-inspect", "convert", "adapter-inspect"):
        raise ManagerError("failed-import recovery retry kind is invalid")
    if (not record["source_path"] or
            len(record["source_path"].encode("utf-8")) > MAX_FAILURE_PATH_BYTES):
        raise ManagerError("failed-import recovery source path is invalid")
    source_sha = record["source_sha256"]
    if source_sha is not None and (
            not isinstance(source_sha, str) or len(source_sha) != 64 or
            any(character not in "0123456789abcdef" for character in source_sha)):
        raise ManagerError("failed-import recovery source digest is invalid")
    for key in ("source_bytes", "attempts", "first_failed_unix", "last_failed_unix"):
        value = record[key]
        if isinstance(value, bool) or not isinstance(value, int) or value < 0:
            raise ManagerError(f"failed-import recovery {key} is invalid")
    if record["attempts"] < 1 or record["attempts"] > 0xFFFFFFFF:
        raise ManagerError("failed-import recovery attempt count is invalid")
    if (record["source_bytes"] > 0xFFFFFFFFFFFFFFFF or
            record["first_failed_unix"] > 0xFFFFFFFFFFFFFFFF or
            record["last_failed_unix"] > 0xFFFFFFFFFFFFFFFF):
        raise ManagerError("failed-import recovery numeric metadata is invalid")
    output = record["output_path"]
    if output is not None and (not isinstance(output, str) or
                               len(output.encode("utf-8")) > MAX_FAILURE_PATH_BYTES):
        raise ManagerError("failed-import recovery output path is invalid")
    report_file = record["validation_report_file"]
    if report_file not in (None, f"{record_id}.validation.json"):
        raise ManagerError("failed-import recovery report name is invalid")
    return record


def _failure_source_state(record: dict[str, Any], *,
                          exact: bool = False) -> tuple[bool, bool]:
    source = Path(record["source_path"])
    if source.is_symlink() or not source.is_file():
        return False, False
    expected = record["source_sha256"]
    if expected is None:
        return True, True
    try:
        if source.stat().st_size != record["source_bytes"]:
            return True, True
    except OSError:
        return False, False
    # Inventory refresh must stay O(record count), not O(total source bytes).
    # A same-size edit is still caught by the mandatory digest check below
    # when the author explicitly retries the source.
    if not exact:
        return True, False
    try:
        payload = _bounded_authoring_input(
            source, probe.MAX_INPUT_BYTES, "failed-import source"
        )
    except ManagerError:
        return True, True
    return True, hashlib.sha256(payload).hexdigest() != expected


def _record_import_failure(root: Path, args: argparse.Namespace,
                           error: BaseException) -> dict[str, Any] | None:
    source, retry_kind, output = _failure_source_for_command(args)
    if source is None or retry_kind is None:
        return None
    source = source.absolute()
    source_text = _bounded_failure_text(
        str(source), MAX_FAILURE_PATH_BYTES, "failed-import source path"
    )
    payload: bytes | None = None
    try:
        payload = _bounded_authoring_input(
            source, probe.MAX_INPUT_BYTES, "failed-import source"
        )
    except ManagerError:
        pass
    validation = (
        error.validation if isinstance(error, ImportValidationError) else None
    )
    source_sha = (
        hashlib.sha256(payload).hexdigest() if payload is not None else
        validation.get("source_sha256") if isinstance(validation, dict) else None
    )
    source_size = len(payload) if payload is not None else 0
    if payload is None:
        try:
            if not source.is_symlink() and source.is_file():
                source_size = min(source.stat().st_size, 0xFFFFFFFFFFFFFFFF)
        except OSError:
            # The source can disappear between the bounded read attempt and
            # metadata capture. Recovery must still preserve the diagnostic.
            source_size = 0
    identity = "\0".join((args.command, source_sha or source_text)).encode("utf-8")
    record_id = hashlib.sha256(identity).hexdigest()
    directory = _failure_directory(root)
    record_path = directory / f"{record_id}.json"
    report_path = directory / f"{record_id}.validation.json"
    now = int(time.time())
    attempts = 1
    first_failed = now
    if record_path.exists() or record_path.is_symlink():
        prior = _read_failure_record(record_path)
        attempts = min(prior["attempts"] + 1, 0xFFFFFFFF)
        first_failed = prior["first_failed_unix"]
    report_file: str | None = None
    if isinstance(validation, dict):
        gltf_validator.write_report(report_path, validation)
        report_file = report_path.name
    else:
        report_path.unlink(missing_ok=True)
    output_text = None if output is None else _bounded_failure_text(
        str(output.absolute()), MAX_FAILURE_PATH_BYTES,
        "failed-import output path",
    )
    record = {
        "schema": FAILURE_SCHEMA,
        "record_id": record_id,
        "command": args.command,
        "retry_kind": retry_kind,
        "source_path": source_text,
        "source_sha256": source_sha,
        "source_bytes": source_size,
        "output_path": output_text,
        "error": _bounded_failure_text(
            str(error), MAX_FAILURE_TEXT_BYTES, "failed-import error"
        ),
        "attempts": attempts,
        "first_failed_unix": first_failed,
        "last_failed_unix": now,
        "validation_report_file": report_file,
    }
    encoded = (json.dumps(record, indent=2, sort_keys=True) + "\n").encode("utf-8")
    if len(encoded) > MAX_REPORT_BYTES:
        raise ManagerError("failed-import recovery metadata exceeds its bound")
    _write_atomic(record_path, encoded)
    return {
        "record_id": record_id,
        "attempts": attempts,
        "source_sha256": source_sha,
        "report_available": report_file is not None,
    }


def _failure_records(root: Path) -> list[dict[str, Any]]:
    unresolved = root / FAILURE_DIRECTORY_NAME
    if not unresolved.exists() and not unresolved.is_symlink():
        return []
    directory = _failure_directory(root, create=False)
    paths = sorted(directory.glob("*.json"))
    record_paths = [
        path for path in paths if not path.name.endswith(".validation.json")
    ]
    if len(record_paths) > MAX_FAILURE_RECORDS:
        raise ManagerError(
            f"failed-import recovery contains more than {MAX_FAILURE_RECORDS} records"
        )
    records = [_read_failure_record(path) for path in record_paths]
    records.sort(
        key=lambda record: (record["last_failed_unix"], record["record_id"]),
        reverse=True,
    )
    return records


@contextmanager
def _locked(directory: Path) -> Iterator[None]:
    lock = directory / LOCK_NAME
    try:
        descriptor = os.open(lock, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    except FileExistsError as exc:
        raise ManagerError(
            "another character import is active (remove the lock only after "
            "confirming no importer is running)"
        ) from exc
    try:
        os.write(descriptor, f"pid={os.getpid()}\n".encode("ascii"))
        os.close(descriptor)
        yield
    finally:
        try:
            os.close(descriptor)
        except OSError:
            pass
        lock.unlink(missing_ok=True)


def _cache_valid(data: bytes) -> bool:
    if len(data) < compiler.MDKC_HEADER_BYTES:
        return False
    magic, version, header_bytes, file_bytes = struct.unpack_from("<4sIIQ", data, 0)
    if (
        magic != compiler.MDKC_MAGIC
        or version != compiler.MDKC_VERSION
        or header_bytes != compiler.MDKC_HEADER_BYTES
        or file_bytes != len(data)
    ):
        return False
    expected_crc = struct.unpack_from("<I", data, 52)[0]
    return (zlib.crc32(data[header_bytes:]) & 0xFFFFFFFF) == expected_crc


def _read_report(path: Path) -> dict[str, Any]:
    if path.is_symlink() or not path.is_file():
        raise ManagerError("provenance report must be a regular file")
    if path.stat().st_size > MAX_REPORT_BYTES:
        raise ManagerError("provenance report exceeds 64 KiB")
    payload = path.read_bytes()
    if len(payload) > MAX_REPORT_BYTES:
        raise ManagerError("provenance report exceeds 64 KiB")
    report = probe.json_loads_strict(payload, "provenance report")
    if not isinstance(report, dict):
        raise ManagerError("provenance report must be an object")
    return report


def _installed_cache_path(root: Path, package_id: str, *,
                          required: bool = True) -> tuple[Path | None, bool]:
    """Resolve the one exact cache state without following ambiguous links."""
    active = root / f"{package_id}.mdkc"
    disabled = root / f"{package_id}.mdkc.disabled"
    active_exists = active.exists() or active.is_symlink()
    disabled_exists = disabled.exists() or disabled.is_symlink()
    if active_exists and disabled_exists:
        raise ManagerError(
            f"character {package_id!r} has both enabled and disabled caches"
        )
    cache_path = active if active_exists else disabled if disabled_exists else None
    if cache_path is None:
        if required:
            raise ManagerError(f"character {package_id!r} is not installed")
        return None, True
    if cache_path.is_symlink() or not cache_path.is_file():
        raise ManagerError(f"installed cache for {package_id!r} is unsafe")
    return cache_path, cache_path == active


def _installed_cache_digest(root: Path, package_id: str) -> str:
    cache_path, _ = _installed_cache_path(root, package_id)
    assert cache_path is not None
    cache = cache_path.read_bytes()
    if not _cache_valid(cache):
        raise ManagerError(f"installed cache for {package_id!r} is invalid")
    return cache[20:52].hex()


def _compile_candidate(package_path: Path) -> dict[str, Any]:
    """Validate and compile a package without touching installed state."""
    if package_path.stat().st_size > probe.MAX_INPUT_BYTES:
        raise ManagerError(
            f"character package exceeds {probe.MAX_INPUT_BYTES} bytes"
        )
    package = package_path.read_bytes()
    if len(package) > probe.MAX_INPUT_BYTES:
        raise ManagerError(
            f"character package exceeds {probe.MAX_INPUT_BYTES} bytes"
        )
    with tempfile.TemporaryDirectory(
            prefix="mdkr-character-candidate-") as temporary:
        snapshot = Path(temporary) / "candidate.mdkrchar"
        snapshot.write_bytes(package)
        verification = probe.verify_package(snapshot)
    if not verification["valid"]:
        raise ManagerError("invalid source package: " + "; ".join(verification["errors"]))
    package_id = verification["id"]
    if not isinstance(package_id, str) or probe.ID_RE.fullmatch(package_id) is None:
        raise ManagerError("validated package has an unsafe id")
    with zipfile.ZipFile(io.BytesIO(package)) as archive:
        manifest = probe.json_loads_strict(
            archive.read("manifest.json"), "manifest"
        )
        model = archive.read("model.glb")
        portrait = (
            archive.read("portrait.png")
            if manifest.get("schema") in probe.IDENTITY_SCHEMAS else None
        )
        compiler_digest = _compiler_source_digest(archive)
        embedded = archive.read("compiled.mdkc") if verification.get("portable") else None
    validation = _validate_character_glb(
        model, f"character package {package_id!r} model"
    )
    compiled, compile_report = compiler.compile_character(
        model, manifest, compiler_digest, portrait
    )
    if embedded is not None and embedded != compiled:
        raise ManagerError(
            "portable package cache does not match its source model and manifest"
        )
    if not _cache_valid(compiled):
        raise ManagerError("compiler produced an invalid cache")
    return {
        "package": package,
        "package_id": package_id,
        "source_sha256": hashlib.sha256(package).hexdigest(),
        "manifest": manifest,
        "compiler_digest": compiler_digest,
        "compiled": compiled,
        "compiled_sha256": hashlib.sha256(compiled).hexdigest(),
        "compile_report": compile_report,
        "validation": validation,
        "portable": embedded is not None,
    }


def inspect(package_path: Path) -> dict[str, Any]:
    """Publish exact compiled candidate facts without installing any bytes."""
    candidate = _compile_candidate(package_path)
    report = candidate["compile_report"]
    license_info = candidate["manifest"]["license"]
    display_name = candidate["manifest"]["display_name"]
    return {
        "schema": MANAGER_SCHEMA,
        "action": "inspect",
        "id": candidate["package_id"],
        "display_name": display_name,
        "short_name": report.get("identity_short_name") or display_name,
        "narration_name": (
            report.get("identity_narration_name") or display_name
        ),
        "sort_label": report.get("identity_sort_label") or display_name,
        "source_sha256": candidate["source_sha256"],
        "cache_source_digest": candidate["compiler_digest"].hex(),
        "compiled_sha256": candidate["compiled_sha256"],
        "compiler": COMPILER_ID,
        "portable": candidate["portable"],
        "license_spdx": license_info["spdx"],
        "attribution": license_info["attribution"],
        "source_url": license_info["source_url"],
        "validation": _validation_summary(candidate["validation"]),
        "report": report,
    }


def write_candidate_index(package_path: Path, directory: Path,
                          index_path: Path) -> dict[str, Any]:
    """Write one validated compiled candidate in a strict launcher protocol."""
    root = _prepare_directory(directory)
    if (index_path.parent.resolve() != root or
            index_path.name != ".launcher-character-candidate.tsv"):
        raise ManagerError(
            "candidate index must use the launcher's exact file inside the "
            "character directory"
        )
    candidate = inspect(package_path)
    report = candidate["report"]
    identity_hex = [
        candidate[field].encode("utf-8").hex()
        for field in (
            "display_name", "short_name", "narration_name", "sort_label"
        )
    ]
    rig_mode = report.get("rig_mode")
    rig_mode_value = {
        None: 0,
        "authored-clips-only": 1,
        "humanoid-retarget-v1": 2,
    }.get(rig_mode)
    if rig_mode_value is None:
        raise ManagerError("candidate compiler reported an unknown rig mode")
    fields = [
        candidate["id"], *identity_hex, candidate["source_sha256"],
        candidate["cache_source_digest"],
        str(compiler.DONOR_IDS[report["donor"]]),
        str(report["vehicle_mask"]), str(report["vertices"]),
        str(report["triangles"]), str(report["primitives"]),
        str(report["lod_levels"]), str(report["materials"]),
        str(report["textures"]), str(report["nodes"]),
        str(report["skins"]), str(report["joints"]),
        str(report["animations"]),
        str(report["animation_channels"]), str(report["animation_keys"]),
        "1" if report["identity_portrait"] else "0",
        str(rig_mode_value), "1" if report["rig_reviewed"] else "0",
        str(report["rig_roles"]), str(report["joint_constraints"]),
        str(report["secondary_chains"]), str(report["secondary_joints"]),
        str(report["encoded_texture_bytes"]),
        str(report["decoded_texture_bytes"]),
        *(str(value) for value in report["lod_vertices"]),
        *(str(value) for value in report["lod_triangles"]),
        *(str(value) for value in report["lod_primitives"]),
        str(report["semantic_mask"]),
        str(report["disabled_semantic_mask"]),
        str(report["authored_tangent_primitives"]),
        str(report["generated_tangent_primitives"]),
        str(report["authored_tangent_repaired_vertices"]),
        str(report["generated_tangent_degenerate_uv_triangles"]),
        str(report["tangent_fallback_vertices"]),
        str(report["normal_map_tangent_fallback_vertices"]),
        str(report["ktx2_texture_count"]),
        str(report["ktx2_source_bytes"]),
        str(report["ktx2_etc1s_count"]),
        str(report["ktx2_uastc_count"]),
        str(report["ktx2_mip_levels_min"]),
        str(report["ktx2_mip_levels_max"]),
        "1",
        candidate["license_spdx"].encode("utf-8").hex(),
        candidate["attribution"].encode("utf-8").hex(),
        candidate["source_url"].encode("utf-8").hex(),
    ]
    payload = (
        "mdkr-character-candidate-v7\n" + "\t".join(fields) + "\n"
    ).encode("ascii")
    _write_atomic(index_path, payload)
    return {
        "id": candidate["id"],
        "source_sha256": candidate["source_sha256"],
        "cache_source_digest": candidate["cache_source_digest"],
        "candidate_index": index_path.name,
    }


def install(package_path: Path, directory: Path, *,
            expected_active_digest: str | None = None,
            expected_package_sha256: str | None = None) -> dict[str, Any]:
    candidate = _compile_candidate(package_path)
    if (expected_package_sha256 is not None and
            candidate["source_sha256"] != expected_package_sha256):
        raise ManagerError(
            "the package file changed after review; validate the new bytes "
            "before installing"
        )
    root = _prepare_directory(directory)
    package = candidate["package"]
    package_id = candidate["package_id"]
    source_sha = candidate["source_sha256"]
    manifest = candidate["manifest"]
    compiler_digest = candidate["compiler_digest"]
    compiled = candidate["compiled"]
    compiled_sha = candidate["compiled_sha256"]
    compile_report = candidate["compile_report"]
    provenance = {
        "schema": MANAGER_SCHEMA,
        "id": package_id,
        "display_name": manifest["display_name"],
        "source_sha256": source_sha,
        "cache_source_digest": compiler_digest.hex(),
        "compiled_sha256": compiled_sha,
        "compiler": COMPILER_ID,
        "installed_unix": int(time.time()),
        "source_file": f"{package_id}.{source_sha}.mdkrchar",
        "validation": _validation_summary(candidate["validation"]),
        "report": compile_report,
    }
    source_path = root / provenance["source_file"]
    report_path = root / f"{package_id}.{source_sha}.json"
    with _locked(root):
        cache_path, enabled = _installed_cache_path(
            root, package_id, required=False
        )
        if expected_active_digest is not None:
            if expected_active_digest == "":
                if cache_path is not None:
                    raise ManagerError(
                        "a character with this id was installed after review; "
                        "review the update before installing"
                    )
            elif (
                len(expected_active_digest) != 64
                or any(character not in "0123456789abcdef"
                       for character in expected_active_digest)
            ):
                raise ManagerError("expected installed digest is invalid")
            elif (cache_path is None or
                  _installed_cache_digest(root, package_id) !=
                  expected_active_digest):
                raise ManagerError(
                    "the installed character changed after review; review the "
                    "latest revision and try again"
                )
        if cache_path is None:
            cache_path = root / f"{package_id}.mdkc"
            enabled = True
        provenance["cache_file"] = cache_path.name
        provenance["enabled"] = enabled
        if not source_path.exists():
            _write_exclusive(source_path, package)
        elif source_path.read_bytes() != package:
            raise ManagerError("content-addressed source path contains different bytes")
        _write_atomic(
            report_path,
            (json.dumps(provenance, indent=2, sort_keys=True) + "\n").encode("utf-8"),
        )
        # Commit point. The engine scans only this stable filename.
        _write_atomic(cache_path, compiled)
    return provenance


def install_reviewed(package_path: Path, directory: Path,
                     expected_package_sha256: str,
                     expected_installed_digest: str) -> dict[str, Any]:
    """Commit only the candidate/base pair the user explicitly reviewed."""
    if (len(expected_package_sha256) != 64 or
            any(character not in "0123456789abcdef"
                for character in expected_package_sha256)):
        raise ManagerError("reviewed package digest is invalid")
    if expected_installed_digest == "absent":
        expected_installed_digest = ""
    return {
        **install(
            package_path, directory,
            expected_active_digest=expected_installed_digest,
            expected_package_sha256=expected_package_sha256,
        ),
        "action": "install-reviewed",
    }


def _active_source_snapshot(package_id: str, root: Path) -> tuple[bytes, str, str]:
    """Read the exact source behind the installed cache under the import lock."""
    with _locked(root):
        active_digest = _installed_cache_digest(root, package_id)
        candidates: list[tuple[bool, str, bytes]] = []
        for report_path in sorted(root.glob(f"{package_id}.*.json")):
            try:
                report = _read_report(report_path)
                source_sha = report["source_sha256"]
                expected_source = f"{package_id}.{source_sha}.mdkrchar"
                if (
                    report.get("schema") != MANAGER_SCHEMA
                    or report.get("id") != package_id
                    or report.get("cache_source_digest") != active_digest
                    or report_path.name != f"{package_id}.{source_sha}.json"
                    or report.get("source_file") != expected_source
                    or len(source_sha) != 64
                    or any(character not in "0123456789abcdef"
                           for character in source_sha)
                ):
                    continue
                source_path = root / expected_source
                if source_path.is_symlink() or not source_path.is_file():
                    continue
                package = source_path.read_bytes()
                if hashlib.sha256(package).hexdigest() != source_sha:
                    continue
                with zipfile.ZipFile(io.BytesIO(package)) as archive:
                    if active_digest not in _compatible_source_digests(archive):
                        continue
                    portable = "compiled.mdkc" in archive.namelist()
                candidates.append((portable, source_sha, package))
            except (OSError, KeyError, TypeError, ValueError,
                    zipfile.BadZipFile, json.JSONDecodeError, probe.ProbeError):
                continue
        if not candidates:
            raise ManagerError(
                f"installed source provenance for {package_id!r} is missing or invalid"
            )
        # Prefer the smaller source-only artifact when both it and a portable
        # package describe the same active source; the canonical source members
        # are protected by the digest comparison above.
        candidates.sort(key=lambda candidate: (candidate[0], candidate[1]))
        _, source_sha, package = candidates[0]
        return package, source_sha, active_digest


def _upgrade_identity_manifest(manifest: dict[str, Any],
                               model_report: dict[str, Any],
                               minimap_rgb: tuple[int, int, int]) -> tuple[dict[str, Any], str]:
    upgraded = dict(manifest)
    original_schema = upgraded.get("schema")
    migration = "identity updated"
    if original_schema == probe.PACKAGE_SCHEMA_V1:
        presentation = upgraded.get("presentation")
        if not isinstance(presentation, dict):
            raise ManagerError("legacy v1 package has no presentation transform")
        scale = presentation.get("scale")
        if (
            not isinstance(scale, list)
            or len(scale) != 3
            or any(isinstance(value, bool) or not isinstance(value, (int, float))
                   for value in scale)
        ):
            raise ManagerError("legacy v1 package has an invalid presentation scale")
        uniform = float(scale[1])
        tolerance = max(1.0, abs(uniform)) * 1.0e-6
        if any(abs(float(value) - uniform) > tolerance for value in scale):
            raise ManagerError(
                "legacy v1 package uses non-uniform scale, which cannot be migrated "
                "to the calibrated format without changing its appearance"
            )
        bounds_min = model_report.get("bbox_min")
        bounds_max = model_report.get("bbox_max")
        if (
            not isinstance(bounds_min, list) or len(bounds_min) != 3
            or not isinstance(bounds_max, list) or len(bounds_max) != 3
        ):
            raise ManagerError("legacy v1 package has no measurable model bounds")
        target_height = (float(bounds_max[1]) - float(bounds_min[1])) * uniform
        if not 0.1 <= target_height <= 10.0:
            raise ManagerError(
                "legacy v1 package's effective height is outside the calibrated "
                "0.1–10 metre range"
            )
        gameplay = upgraded.get("gameplay", {})
        vehicles = gameplay.get("vehicles", []) if isinstance(gameplay, dict) else []
        translation = list(presentation.get("translation_m", []))
        rotation = list(presentation.get("rotation_xyzw", []))
        contexts: dict[str, Any] = {
            "select": {
                "anchor": "ground", "translation_m": translation,
                "rotation_xyzw": rotation, "scale": 1.0,
            }
        }
        for vehicle in vehicles:
            contexts[vehicle] = {
                "anchor": "seat", "translation_m": translation,
                "rotation_xyzw": rotation, "scale": 1.0,
            }
        upgraded["presentation"] = {
            "source_forward": "+z",
            "target_height_m": target_height,
            "contexts": contexts,
            "lod_bias": presentation.get("lod_bias", 0.0),
        }
        migration = "legacy v1 transform migrated losslessly; identity added"
    elif original_schema not in ({probe.PACKAGE_SCHEMA} | probe.IDENTITY_SCHEMAS):
        raise ManagerError("installed package uses an unsupported source schema")
    upgraded["schema"] = (
        original_schema
        if original_schema in probe.RIG_SCHEMAS
        else probe.PACKAGE_SCHEMA_V3
    )
    prior_identity = manifest.get("identity")
    retained_names = {}
    if isinstance(prior_identity, dict):
        retained_names = {
            field: prior_identity[field]
            for field in ("short_name", "narration_name", "sort_label")
            if field in prior_identity
        }
    upgraded["identity"] = {
        "portrait_file": "portrait.png",
        # build_package replaces this placeholder with the canonical digest.
        "portrait_sha256": "0" * 64,
        "minimap_rgb": list(minimap_rgb),
        **retained_names,
    }
    return upgraded, migration


def revise_identity(package_id: str, portrait_path: Path,
                    minimap_rgb: tuple[int, int, int],
                    directory: Path) -> dict[str, Any]:
    """Create and atomically activate an identity-capable source revision."""
    if probe.ID_RE.fullmatch(package_id) is None:
        raise ManagerError("invalid package id")
    if (
        not isinstance(minimap_rgb, tuple)
        or len(minimap_rgb) != 3
        or any(isinstance(component, bool) or not isinstance(component, int)
               or not 0 <= component <= 255 for component in minimap_rgb)
    ):
        raise ManagerError("minimap RGB must contain three bytes")
    # Validate before taking the shared install lock so a bad image cannot
    # disturb the current cache or block other imports.
    portrait_size = portrait_path.stat().st_size
    if portrait_size > probe.MAX_PORTRAIT_BYTES:
        raise ManagerError(
            f"portrait.png exceeds {probe.MAX_PORTRAIT_BYTES} bytes"
        )
    portrait = portrait_path.read_bytes()
    if len(portrait) > probe.MAX_PORTRAIT_BYTES:
        raise ManagerError(
            f"portrait.png exceeds {probe.MAX_PORTRAIT_BYTES} bytes"
        )
    probe.inspect_portrait_png(portrait)
    root = _prepare_directory(directory)
    package, based_on_sha, based_on_digest = _active_source_snapshot(
        package_id, root
    )
    with tempfile.TemporaryDirectory(prefix="mdkr-character-revision-") as temporary:
        draft = Path(temporary)
        snapshot = draft / "source.mdkrchar"
        snapshot.write_bytes(package)
        verification = probe.verify_package(snapshot)
        if not verification["valid"] or verification.get("id") != package_id:
            raise ManagerError("active source package failed verification")
        with zipfile.ZipFile(io.BytesIO(package)) as archive:
            manifest = probe.json_loads_strict(
                archive.read("manifest.json"), "manifest"
            )
            model = archive.read("model.glb")
            license_text = archive.read("LICENSE.txt")
        if not isinstance(manifest, dict):
            raise ManagerError("active source manifest is not an object")
        upgraded, migration = _upgrade_identity_manifest(
            manifest, verification["model"], minimap_rgb
        )
        model_path = draft / "model.glb"
        manifest_path = draft / "manifest.json"
        license_path = draft / "LICENSE.txt"
        portrait_copy = draft / "portrait.png"
        revised_package = draft / "revision.mdkrchar"
        model_path.write_bytes(model)
        manifest_path.write_text(
            json.dumps(upgraded, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
        license_path.write_bytes(license_text)
        portrait_copy.write_bytes(portrait)
        probe.build_package(
            model_path, manifest_path, license_path, revised_package,
            portrait_path=portrait_copy,
        )
        installed = install(
            revised_package, root, expected_active_digest=based_on_digest
        )
    return {
        **installed,
        "action": "revise-identity",
        "based_on_source_sha256": based_on_sha,
        "migration": migration,
        "minimap_rgb": list(minimap_rgb),
    }


def _portrait_png_from_rgba(rgba: bytes) -> bytes:
    size = 40
    if len(rgba) != size * size * 4:
        raise ManagerError("portrait RGBA draft must contain exactly 40×40 pixels")

    def chunk(kind: bytes, payload: bytes) -> bytes:
        return (
            struct.pack(">I", len(payload)) + kind + payload
            + struct.pack(">I", zlib.crc32(kind + payload) & 0xFFFFFFFF)
        )

    scanlines = b"".join(
        b"\0" + rgba[y * size * 4:(y + 1) * size * 4]
        for y in range(size)
    )
    return (
        probe.PNG_SIGNATURE
        + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(scanlines, level=9))
        + chunk(b"IEND", b"")
    )


def revise_identity_rgba(package_id: str, rgba: bytes,
                         minimap_rgb: tuple[int, int, int],
                         directory: Path) -> dict[str, Any]:
    """Compile an exact 40x40 editor canvas through the normal identity path."""
    portrait = _portrait_png_from_rgba(rgba)
    with tempfile.TemporaryDirectory(prefix="mdkr-portrait-draft-") as temporary:
        portrait_path = Path(temporary) / "portrait.png"
        portrait_path.write_bytes(portrait)
        result = revise_identity(
            package_id, portrait_path, minimap_rgb, directory
        )
    result["action"] = "revise-identity-rgba"
    result["draft_rgba_sha256"] = hashlib.sha256(rgba).hexdigest()
    return result


def _revise_manifest(
    package_id: str,
    directory: Path,
    transform: Callable[[dict[str, Any], dict[str, Any]], dict[str, Any]],
) -> tuple[dict[str, Any], str]:
    """Rebuild one source revision while preserving model, media and license."""
    if probe.ID_RE.fullmatch(package_id) is None:
        raise ManagerError("invalid package id")
    root = _prepare_directory(directory)
    package, based_on_sha, based_on_digest = _active_source_snapshot(
        package_id, root
    )
    with tempfile.TemporaryDirectory(prefix="mdkr-character-revision-") as temporary:
        draft = Path(temporary)
        snapshot = draft / "source.mdkrchar"
        snapshot.write_bytes(package)
        verification = probe.verify_package(snapshot)
        if not verification["valid"] or verification.get("id") != package_id:
            raise ManagerError("active source package failed verification")
        with zipfile.ZipFile(io.BytesIO(package)) as archive:
            manifest = probe.json_loads_strict(
                archive.read("manifest.json"), "manifest"
            )
            model = archive.read("model.glb")
            license_text = archive.read("LICENSE.txt")
            portrait = (
                archive.read("portrait.png")
                if manifest.get("schema") in probe.IDENTITY_SCHEMAS else None
            )
        if not isinstance(manifest, dict):
            raise ManagerError("active source manifest is not an object")
        revised = transform(manifest, verification)
        if not isinstance(revised, dict):
            raise ManagerError("manifest revision did not produce an object")
        model_path = draft / "model.glb"
        manifest_path = draft / "manifest.json"
        license_path = draft / "LICENSE.txt"
        portrait_path = draft / "portrait.png"
        revised_package = draft / "revision.mdkrchar"
        model_path.write_bytes(model)
        manifest_path.write_text(
            json.dumps(revised, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
        license_path.write_bytes(license_text)
        if portrait is not None:
            portrait_path.write_bytes(portrait)
        probe.build_package(
            model_path, manifest_path, license_path, revised_package,
            portrait_path=portrait_path if portrait is not None else None,
        )
        installed = install(
            revised_package, root, expected_active_digest=based_on_digest
        )
    return installed, based_on_sha


def revise_profile(package_id: str, donor: str, vehicles: tuple[str, ...],
                   directory: Path) -> dict[str, Any]:
    """Create and atomically activate a donor/vehicle compatibility revision."""
    if donor not in probe.GAMEPLAY_DONORS:
        raise ManagerError("gameplay donor must name a built-in racer")
    if (
        not vehicles
        or len(set(vehicles)) != len(vehicles)
        or any(vehicle not in probe.VEHICLE_NAMES for vehicle in vehicles)
    ):
        raise ManagerError(
            "vehicle compatibility must contain one or more unique car, "
            "hovercraft, or plane entries"
        )

    def transform(manifest: dict[str, Any], _: dict[str, Any]) -> dict[str, Any]:
        revised = dict(manifest)
        revised["gameplay"] = {
            "donor": donor,
            "vehicles": list(vehicles),
        }
        if revised.get("schema") in (
            probe.PACKAGE_SCHEMA, probe.PACKAGE_SCHEMA_V3,
            probe.PACKAGE_SCHEMA_V4, probe.PACKAGE_SCHEMA_V5,
        ):
            presentation = revised.get("presentation")
            if not isinstance(presentation, dict):
                raise ManagerError("active source has no calibrated presentation")
            presentation = dict(presentation)
            contexts = presentation.get("contexts")
            if not isinstance(contexts, dict):
                raise ManagerError("active source has no presentation contexts")
            contexts = dict(contexts)
            for vehicle in vehicles:
                contexts.setdefault(vehicle, {
                    "anchor": "seat",
                    "translation_m": [0.0, 0.0, 0.0],
                    "rotation_xyzw": [0.0, 0.0, 0.0, 1.0],
                    "scale": 1.0,
                })
            presentation["contexts"] = contexts
            revised["presentation"] = presentation
        return revised

    installed, based_on_sha = _revise_manifest(
        package_id, directory, transform
    )
    return {
        **installed,
        "action": "revise-profile",
        "based_on_source_sha256": based_on_sha,
        "donor": donor,
        "vehicles": list(vehicles),
    }


def _read_bounded_json_object(path: Path, maximum: int,
                              description: str) -> dict[str, Any]:
    if path.is_symlink() or not path.is_file():
        raise ManagerError(f"{description} must be a regular JSON file")
    if path.stat().st_size > maximum:
        raise ManagerError(f"{description} exceeds {maximum} bytes")
    payload = path.read_bytes()
    if len(payload) > maximum:
        raise ManagerError(f"{description} exceeds {maximum} bytes")
    value = probe.json_loads_strict(payload, description)
    if not isinstance(value, dict):
        raise ManagerError(f"{description} must be an object")
    return value


def _validated_rig_draft(
        draft: dict[str, Any]
        ) -> tuple[dict[str, Any], list[str] | None,
                   dict[str, Any] | None, bool]:
    schema = draft.get("schema")
    expected = {"schema", "mode", "reviewed", "roles"}
    if schema in (
            "mdkr-character-rig-draft-v2",
            "mdkr-character-rig-draft-v3"):
        expected.add("disabled_semantics")
    if schema == "mdkr-character-rig-draft-v3":
        expected.add("secondary_motion")
    unknown = set(draft) - expected
    missing = expected - set(draft)
    if unknown or missing or schema not in (
            "mdkr-character-rig-draft-v1", "mdkr-character-rig-draft-v2",
            "mdkr-character-rig-draft-v3"):
        detail = []
        if unknown:
            detail.append("unknown: " + ", ".join(sorted(unknown)))
        if missing:
            detail.append("missing: " + ", ".join(sorted(missing)))
        if schema not in (
                "mdkr-character-rig-draft-v1",
                "mdkr-character-rig-draft-v2",
                "mdkr-character-rig-draft-v3"):
            detail.append("unsupported schema")
        raise ManagerError("invalid rig draft (" + "; ".join(detail) + ")")
    disabled = (
        draft.get("disabled_semantics")
        if schema in (
            "mdkr-character-rig-draft-v2",
            "mdkr-character-rig-draft-v3") else None
    )
    if disabled is None and schema == "mdkr-character-rig-draft-v1":
        return ({
            "mode": draft["mode"],
            "reviewed": draft["reviewed"],
            "roles": draft["roles"],
        }, None, None, False)
    if (
        not isinstance(disabled, list)
        or any(
            not isinstance(semantic, str)
            or probe.SEMANTIC_RE.fullmatch(semantic) is None
            or semantic not in probe.DISABLEABLE_SEMANTICS
            for semantic in disabled
        )
        or len(disabled) > 64
        or len(set(disabled)) != len(disabled)
    ):
        raise ManagerError(
            "rig draft disabled_semantics must contain at most 64 unique "
            "supported engine semantic names"
        )
    secondary = draft.get("secondary_motion")
    if schema == "mdkr-character-rig-draft-v3" and not (
            secondary is None or isinstance(secondary, dict)):
        raise ManagerError(
            "rig draft secondary_motion must be an object or null"
        )
    return ({
        "mode": draft["mode"],
        "reviewed": draft["reviewed"],
        "roles": draft["roles"],
    }, list(disabled), secondary, schema == "mdkr-character-rig-draft-v3")


def _apply_disabled_semantics(
        manifest: dict[str, Any], disabled: list[str] | None) -> None:
    if disabled is None:
        return
    animations = manifest.get("animations")
    if not isinstance(animations, dict):
        raise ManagerError("active source has no animation mapping object")
    states = animations.get("states", {})
    if not isinstance(states, dict):
        raise ManagerError("active source has no animation state mapping")
    unknown = [semantic for semantic in disabled if semantic not in states]
    if unknown:
        raise ManagerError(
            "disabled animation semantics are absent from the active source: "
            + ", ".join(unknown)
        )
    revised_animations = dict(animations)
    if disabled:
        revised_animations["disabled_states"] = list(disabled)
    else:
        revised_animations.pop("disabled_states", None)
    manifest["animations"] = revised_animations


def _retain_v5_constraints(
        manifest: dict[str, Any], revised_rig: dict[str, Any]) -> dict[str, Any]:
    """Carry limits across non-anatomical edits; remapped roles invalidate them."""
    if manifest.get("schema") != probe.PACKAGE_SCHEMA_V5:
        return revised_rig
    if revised_rig.get("mode") != "humanoid-retarget-v1":
        return revised_rig
    original_rig = manifest.get("rig")
    original_roles = (
        original_rig.get("roles") if isinstance(original_rig, dict) else None
    )
    revised_roles = revised_rig.get("roles")
    if not isinstance(original_roles, dict) or not isinstance(revised_roles, dict):
        return revised_rig
    retained_roles: dict[str, Any] = {}
    for role, revised_mapping in revised_roles.items():
        if not isinstance(revised_mapping, dict):
            retained_roles[role] = revised_mapping
            continue
        retained_mapping = dict(revised_mapping)
        original_mapping = original_roles.get(role)
        if (
            isinstance(original_mapping, dict)
            and original_mapping.get("node") == revised_mapping.get("node")
            and isinstance(original_mapping.get("constraint"), dict)
        ):
            retained_mapping["constraint"] = dict(
                original_mapping["constraint"]
            )
        retained_roles[role] = retained_mapping
    retained_rig = dict(revised_rig)
    retained_rig["roles"] = retained_roles
    return retained_rig


def revise_rig(package_id: str, rig_draft_path: Path,
               directory: Path) -> dict[str, Any]:
    """Create and atomically activate a reviewed skeleton-map revision."""
    draft = _read_bounded_json_object(
        rig_draft_path, 128 * 1024, "rig draft"
    )
    rig, disabled, secondary, motion_contract = _validated_rig_draft(draft)

    def transform(manifest: dict[str, Any], _: dict[str, Any]) -> dict[str, Any]:
        schema = manifest.get("schema")
        if schema not in probe.IDENTITY_SCHEMAS:
            raise ManagerError(
                "rig authoring requires an identity-capable source-v3/v4/v5 package"
            )
        revised = dict(manifest)
        revised["schema"] = (
            probe.PACKAGE_SCHEMA_V5
            if motion_contract or schema == probe.PACKAGE_SCHEMA_V5
            else probe.PACKAGE_SCHEMA_V4
        )
        revised["rig"] = (
            rig if motion_contract else _retain_v5_constraints(manifest, rig)
        )
        if motion_contract:
            if secondary is None:
                revised.pop("secondary_motion", None)
            else:
                revised["secondary_motion"] = secondary
        _apply_disabled_semantics(revised, disabled)
        return revised

    installed, based_on_sha = _revise_manifest(
        package_id, directory, transform
    )
    return {
        **installed,
        "action": "revise-rig",
        "based_on_source_sha256": based_on_sha,
        "rig_mode": rig["mode"],
        "rig_reviewed": rig["reviewed"],
        "rig_roles": len(rig["roles"]) if isinstance(rig["roles"], dict) else 0,
        "disabled_semantics": installed["report"].get(
            "disabled_semantics", []
        ),
    }


def build_workshop_draft(package_id: str, draft_path: Path,
                         directory: Path) -> dict[str, Any]:
    """Build identity, gameplay and rig edits as one optimistic revision."""
    if probe.ID_RE.fullmatch(package_id) is None:
        raise ManagerError("invalid package id")
    draft = _read_bounded_json_object(
        draft_path, MAX_WORKSHOP_DRAFT_BYTES, "Workshop build draft"
    )
    expected = {
        "schema", "base_cache_source_digest", "donor", "vehicles",
        "display_name", "short_name", "narration_name", "sort_label",
        "portrait_rgba_hex", "minimap_rgb", "rig_draft",
    }
    unknown = set(draft) - expected
    missing = expected - set(draft)
    if unknown or missing or draft.get("schema") != "mdkr-workshop-build-v1":
        detail = []
        if unknown:
            detail.append("unknown: " + ", ".join(sorted(unknown)))
        if missing:
            detail.append("missing: " + ", ".join(sorted(missing)))
        if draft.get("schema") != "mdkr-workshop-build-v1":
            detail.append("unsupported schema")
        raise ManagerError(
            "invalid Workshop build draft (" + "; ".join(detail) + ")"
        )
    base_digest = draft["base_cache_source_digest"]
    if (not isinstance(base_digest, str) or len(base_digest) != 64 or
            any(character not in "0123456789abcdef"
                for character in base_digest)):
        raise ManagerError("Workshop draft base digest is invalid")
    display_name = _workshop_identity_name(
        draft["display_name"], "display name", 96
    )
    short_name = _workshop_identity_name(
        draft["short_name"], "short name", 96
    )
    narration_name = _workshop_identity_name(
        draft["narration_name"], "narration name", 96
    )
    sort_label = _workshop_identity_name(
        draft["sort_label"], "sort label", 96
    )
    donor = draft["donor"]
    vehicles = draft["vehicles"]
    if not isinstance(donor, str) or donor not in probe.GAMEPLAY_DONORS:
        raise ManagerError("gameplay donor must name a built-in racer")
    if (not isinstance(vehicles, list) or not vehicles or
            any(not isinstance(vehicle, str) for vehicle in vehicles) or
            len(set(vehicles)) != len(vehicles) or
            any(vehicle not in probe.VEHICLE_NAMES for vehicle in vehicles)):
        raise ManagerError(
            "vehicle compatibility must contain one or more unique car, "
            "hovercraft, or plane entries"
        )
    minimap = draft["minimap_rgb"]
    if (not isinstance(minimap, list) or len(minimap) != 3 or
            any(isinstance(component, bool) or not isinstance(component, int)
                or not 0 <= component <= 255 for component in minimap)):
        raise ManagerError("minimap RGB must contain three bytes")
    rgba_hex = draft["portrait_rgba_hex"]
    if (not isinstance(rgba_hex, str) or len(rgba_hex) != 40 * 40 * 8 or
            any(character not in "0123456789abcdef"
                for character in rgba_hex)):
        raise ManagerError(
            "portrait RGBA must be exactly 12,800 lowercase hex characters"
        )
    rig_draft = draft["rig_draft"]
    if not isinstance(rig_draft, dict):
        raise ManagerError("rig_draft must be an object")
    rig, disabled, secondary, motion_contract = _validated_rig_draft(rig_draft)

    root = _prepare_directory(directory)
    package, based_on_sha, based_on_digest = _active_source_snapshot(
        package_id, root
    )
    if based_on_digest != base_digest:
        raise ManagerError(
            "the installed character changed after this draft was resumed; "
            "restore its exact base or review the latest source"
        )
    with tempfile.TemporaryDirectory(
            prefix="mdkr-workshop-build-") as temporary:
        work = Path(temporary)
        snapshot = work / "source.mdkrchar"
        snapshot.write_bytes(package)
        verification = probe.verify_package(snapshot)
        if not verification["valid"] or verification.get("id") != package_id:
            raise ManagerError("active source package failed verification")
        with zipfile.ZipFile(io.BytesIO(package)) as archive:
            manifest = probe.json_loads_strict(
                archive.read("manifest.json"), "manifest"
            )
            model = archive.read("model.glb")
            license_text = archive.read("LICENSE.txt")
        if not isinstance(manifest, dict):
            raise ManagerError("active source manifest is not an object")
        revised, migration = _upgrade_identity_manifest(
            manifest, verification["model"], tuple(minimap)
        )
        revised["display_name"] = display_name
        identity = revised.get("identity")
        if not isinstance(identity, dict):
            raise ManagerError("identity migration did not produce an object")
        identity = dict(identity)
        identity["short_name"] = short_name
        identity["narration_name"] = narration_name
        identity["sort_label"] = sort_label
        revised["identity"] = identity
        revised["gameplay"] = {
            "donor": donor,
            "vehicles": list(vehicles),
        }
        presentation = revised.get("presentation")
        if not isinstance(presentation, dict):
            raise ManagerError("active source has no calibrated presentation")
        presentation = dict(presentation)
        contexts = presentation.get("contexts")
        if not isinstance(contexts, dict):
            raise ManagerError("active source has no presentation contexts")
        contexts = dict(contexts)
        for vehicle in vehicles:
            contexts.setdefault(vehicle, {
                "anchor": "seat",
                "translation_m": [0.0, 0.0, 0.0],
                "rotation_xyzw": [0.0, 0.0, 0.0, 1.0],
                "scale": 1.0,
            })
        presentation["contexts"] = contexts
        revised["presentation"] = presentation
        revised["schema"] = (
            probe.PACKAGE_SCHEMA_V5
            if motion_contract or manifest.get("schema") == probe.PACKAGE_SCHEMA_V5
            else probe.PACKAGE_SCHEMA_V4
        )
        revised["rig"] = (
            rig if motion_contract else _retain_v5_constraints(manifest, rig)
        )
        if motion_contract:
            if secondary is None:
                revised.pop("secondary_motion", None)
            else:
                revised["secondary_motion"] = secondary
        _apply_disabled_semantics(revised, disabled)

        model_path = work / "model.glb"
        manifest_path = work / "manifest.json"
        license_path = work / "LICENSE.txt"
        portrait_path = work / "portrait.png"
        revised_package = work / "revision.mdkrchar"
        model_path.write_bytes(model)
        manifest_path.write_text(
            json.dumps(revised, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
        license_path.write_bytes(license_text)
        portrait_path.write_bytes(_portrait_png_from_rgba(bytes.fromhex(rgba_hex)))
        probe.build_package(
            model_path, manifest_path, license_path, revised_package,
            portrait_path=portrait_path,
        )
        installed = install(
            revised_package, root, expected_active_digest=based_on_digest
        )
    return {
        **installed,
        "action": "build-workshop-draft",
        "based_on_source_sha256": based_on_sha,
        "based_on_cache_source_digest": based_on_digest,
        "migration": migration,
        "donor": donor,
        "vehicles": list(vehicles),
        "rig_mode": rig["mode"],
        "rig_reviewed": rig["reviewed"],
        "rig_roles": len(rig["roles"]) if isinstance(rig["roles"], dict) else 0,
        "disabled_semantics": installed["report"].get(
            "disabled_semantics", []
        ),
    }


def list_installed(directory: Path) -> dict[str, Any]:
    root = _prepare_directory(directory)
    entries: list[dict[str, Any]] = []
    for report_path in sorted(root.glob("*.json")):
        try:
            report = _read_report(report_path)
            if report.get("schema") != MANAGER_SCHEMA:
                continue
            package_id = report["id"]
            source_sha = report.get("source_sha256", "")
            if report_path.name != f"{package_id}.{source_sha}.json":
                continue
            if probe.ID_RE.fullmatch(package_id) is None:
                continue
            cache, enabled = _installed_cache_path(root, package_id)
            assert cache is not None
            source = root / report["source_file"]
            cache_data = cache.read_bytes()
            active = (
                _cache_valid(cache_data)
                and cache_data[20:52].hex() == report.get("cache_source_digest")
            )
            entries.append(
                {
                    "id": package_id,
                    "display_name": report.get("display_name", package_id),
                    "source_sha256": source_sha,
                    "compiler": report.get("compiler", ""),
                    "active": active,
                    "enabled": enabled,
                    "installed_unix": report.get("installed_unix"),
                    "cache_source_digest": report.get(
                        "cache_source_digest", ""
                    ),
                    "source_file": report["source_file"],
                    "cache_file": cache.name,
                    "source_present": source.is_file() and not source.is_symlink(),
                    "report_file": report_path.name,
                }
            )
        except (OSError, KeyError, TypeError, ValueError, struct.error,
                ManagerError):
            continue
    # Keep only the provenance matching each active cache's embedded source
    # digest. Older reports remain visible as inactive history until clean.
    return {"schema": MANAGER_SCHEMA, "directory": str(root), "entries": entries}


def list_revisions(package_id: str, directory: Path) -> dict[str, Any]:
    """List retained revisions for one exact identity, current revision first."""
    if probe.ID_RE.fullmatch(package_id) is None:
        raise ManagerError("invalid package id")
    listing = list_installed(directory)
    revisions = [
        entry for entry in listing["entries"] if entry["id"] == package_id
    ]
    revisions.sort(
        key=lambda entry: (
            not entry["active"],
            -(entry["installed_unix"]
              if isinstance(entry["installed_unix"], int) and
              not isinstance(entry["installed_unix"], bool) else 0),
            entry["source_sha256"],
        )
    )
    if not revisions:
        raise ManagerError(
            f"no valid retained revisions exist for {package_id!r}"
        )
    return {
        "schema": MANAGER_SCHEMA,
        "id": package_id,
        "enabled": revisions[0]["enabled"],
        "revisions": revisions,
    }


def _retained_revision_source(package_id: str, source_sha: str,
                              root: Path) -> tuple[bytes, str]:
    """Resolve and authenticate one retained source under the shared lock."""
    if (len(source_sha) != 64 or
            any(character not in "0123456789abcdef" for character in source_sha)):
        raise ManagerError("revision digest must be 64 lowercase hex characters")
    with _locked(root):
        expected_current_digest = _installed_cache_digest(root, package_id)
        report_path = root / f"{package_id}.{source_sha}.json"
        source_path = root / f"{package_id}.{source_sha}.mdkrchar"
        if (report_path.is_symlink() or not report_path.is_file() or
                source_path.is_symlink() or not source_path.is_file()):
            raise ManagerError("retained revision source or provenance is missing")
        report = _read_report(report_path)
        payload = source_path.read_bytes()
        if (
            report.get("schema") != MANAGER_SCHEMA
            or report.get("id") != package_id
            or report.get("source_sha256") != source_sha
            or report.get("source_file") != source_path.name
            or hashlib.sha256(payload).hexdigest() != source_sha
        ):
            raise ManagerError("retained revision provenance is invalid")
        with tempfile.TemporaryDirectory(
                prefix="mdkr-retained-source-verify-") as temporary:
            snapshot = Path(temporary) / "source.mdkrchar"
            snapshot.write_bytes(payload)
            verification = probe.verify_package(snapshot)
            if (not verification["valid"] or
                    verification.get("id") != package_id):
                raise ManagerError("retained revision package failed verification")
        return payload, expected_current_digest


def restore_revision(package_id: str, source_sha: str,
                     directory: Path) -> dict[str, Any]:
    """Compile and atomically activate any exact retained source revision."""
    if probe.ID_RE.fullmatch(package_id) is None:
        raise ManagerError("invalid package id")
    root = _prepare_directory(directory)
    payload, expected_digest = _retained_revision_source(
        package_id, source_sha, root
    )
    with tempfile.TemporaryDirectory(
            prefix="mdkr-character-restore-") as temporary:
        snapshot = Path(temporary) / "revision.mdkrchar"
        snapshot.write_bytes(payload)
        restored = install(
            snapshot, root, expected_active_digest=expected_digest
        )
    return {
        **restored,
        "action": "restore-revision",
        "restored_source_sha256": source_sha,
    }


def rebuild_current(package_id: str, directory: Path) -> dict[str, Any]:
    """Recompile the authenticated active source without changing identity."""
    if probe.ID_RE.fullmatch(package_id) is None:
        raise ManagerError("invalid package id")
    root = _prepare_directory(directory)
    payload, source_sha, expected_digest = _active_source_snapshot(
        package_id, root
    )
    with tempfile.TemporaryDirectory(
            prefix="mdkr-character-rebuild-") as temporary:
        snapshot = Path(temporary) / "current.mdkrchar"
        snapshot.write_bytes(payload)
        rebuilt = install(
            snapshot, root, expected_active_digest=expected_digest
        )
    return {
        **rebuilt,
        "action": "rebuild-current",
        "rebuilt_source_sha256": source_sha,
        "replaced_cache_source_digest": expected_digest,
    }


def export_revision(package_id: str, source_sha: str, directory: Path,
                    output_path: Path) -> dict[str, Any]:
    """Export an authenticated retained source without overwriting any file."""
    if probe.ID_RE.fullmatch(package_id) is None:
        raise ManagerError("invalid package id")
    root = _prepare_directory(directory)
    payload, _ = _retained_revision_source(package_id, source_sha, root)
    if output_path.parent.is_symlink():
        raise ManagerError("export directory must be a real directory")
    parent = output_path.parent.resolve()
    if not parent.is_dir():
        raise ManagerError("export directory must already exist")
    destination = parent / output_path.name
    if destination.name in ("", ".", ".."):
        raise ManagerError("export filename is invalid")
    if destination.exists() or destination.is_symlink():
        raise ManagerError("export destination already exists")
    try:
        _write_exclusive(destination, payload)
    except FileExistsError as exc:
        raise ManagerError("export destination already exists") from exc
    return {
        "id": package_id,
        "source_sha256": source_sha,
        "exported_file": str(destination),
        "bytes": len(payload),
    }


def export_portable_revision(package_id: str, source_sha: str,
                             directory: Path, output_path: Path) -> dict[str, Any]:
    """Compile and exclusively export one authenticated retained revision."""
    if probe.ID_RE.fullmatch(package_id) is None:
        raise ManagerError("invalid package id")
    root = _prepare_directory(directory)
    payload, _ = _retained_revision_source(package_id, source_sha, root)
    if output_path.parent.is_symlink():
        raise ManagerError("export directory must be a real directory")
    parent = output_path.parent.resolve()
    if not parent.is_dir():
        raise ManagerError("export directory must already exist")
    destination = parent / output_path.name
    if destination.name in ("", ".", ".."):
        raise ManagerError("export filename is invalid")
    if destination.exists() or destination.is_symlink():
        raise ManagerError("export destination already exists")

    with tempfile.TemporaryDirectory(
            prefix="mdkr-character-portable-export-") as temporary:
        temporary_root = Path(temporary)
        snapshot = temporary_root / "source.mdkrchar"
        compiled_package = temporary_root / "portable.mdkrchar"
        snapshot.write_bytes(payload)
        prepared = prepare(snapshot, compiled_package)
        portable_payload = compiled_package.read_bytes()
    try:
        _write_exclusive(destination, portable_payload)
    except FileExistsError as exc:
        raise ManagerError("export destination already exists") from exc
    return {
        **prepared,
        "action": "export-portable-revision",
        "source_sha256": source_sha,
        "portable_package": str(destination),
        "exported_file": str(destination),
        "bytes": len(portable_payload),
    }


def inspect_raw_glb(model_path: Path) -> dict[str, Any]:
    """Return the bounded author-facing choices needed before packaging."""
    if model_path.suffix.lower() != ".glb":
        raise ManagerError("raw character intake requires a .glb file")
    payload = _bounded_authoring_input(
        model_path, probe.MAX_INPUT_BYTES, "GLB model"
    )
    validation = _validate_character_glb(payload, "raw character model")
    report = probe.inspect_glb_bytes(payload, require_character=True)
    if report["errors"]:
        raise ManagerError(
            "GLB is not character-ready: " + "; ".join(report["errors"])
        )
    document, _ = probe.parse_glb(payload)
    clips: list[str] = []
    for index, animation in enumerate(document.get("animations", [])):
        if not isinstance(animation, dict):
            continue
        source_name = animation.get("name")
        if source_name is not None and not isinstance(source_name, str):
            raise ManagerError(
                f"animation {index} name must be a string when present"
            )
        clips.append(source_name or f"animation_{index}")
    nodes: list[str] = []
    for index, node in enumerate(document.get("nodes", [])):
        if not isinstance(node, dict):
            continue
        source_name = node.get("name")
        if source_name is not None and not isinstance(source_name, str):
            raise ManagerError(
                f"node {index} name must be a string when present"
            )
        if source_name:
            nodes.append(source_name)
    if len(clips) > RAW_INTAKE_MAX_CLIPS:
        raise ManagerError(
            f"GLB exposes {len(clips)} animations; raw intake supports at "
            f"most {RAW_INTAKE_MAX_CLIPS} reviewable choices"
        )
    if len(nodes) > RAW_INTAKE_MAX_NODES:
        raise ManagerError(
            f"GLB exposes {len(nodes)} named nodes; raw intake supports at "
            f"most {RAW_INTAKE_MAX_NODES} reviewable choices"
        )
    for label, choices in (("animation", clips), ("node", nodes)):
        for choice in choices:
            if (not choice.strip() or not probe._bounded_printable_text(
                    choice, RAW_INTAKE_MAX_CHOICE_BYTES)):
                raise ManagerError(
                    f"{label} name {choice!r} exceeds the bounded printable "
                    "authoring profile"
                )

    def duplicates(values: list[str]) -> list[str]:
        seen: set[str] = set()
        repeated: set[str] = set()
        for value in values:
            if value in seen:
                repeated.add(value)
            seen.add(value)
        return sorted(repeated)

    duplicate_clips = duplicates(clips)
    duplicate_nodes = duplicates(nodes)
    if duplicate_clips or duplicate_nodes:
        details = []
        if duplicate_clips:
            details.append("animation names: " + ", ".join(duplicate_clips))
        if duplicate_nodes:
            details.append("node names: " + ", ".join(duplicate_nodes))
        raise ManagerError(
            "GLB names used for author mappings must be unique (" +
            "; ".join(details) + "); rename duplicates in the source model"
        )
    fallback = wizard.choose(
        clips, ("idle", "default", "fallback", "rest")
    )
    if fallback is None and clips:
        fallback = clips[0]
    if fallback is None:
        # The launcher presents this as an explicit bind/reference-motion path,
        # not as a source clip. Keeping it in the bounded choice protocol lets
        # old draft persistence remain deterministic without inventing GLB data.
        fallback = probe.BIND_POSE_FALLBACK
    fallback_choices = clips if clips else [probe.BIND_POSE_FALLBACK]
    inferred_sockets = {
        semantic: node
        for semantic, aliases in wizard.SOCKET_ALIASES.items()
        if (node := wizard.choose(nodes, aliases)) is not None
    }
    bounds_min = report["bbox_min"]
    bounds_max = report["bbox_max"]
    mesh_local_min = report["mesh_local_bbox_min"]
    mesh_local_max = report["mesh_local_bbox_max"]
    if (not isinstance(bounds_min, list) or len(bounds_min) != 3 or
            not isinstance(bounds_max, list) or len(bounds_max) != 3 or
            not isinstance(mesh_local_min, list) or len(mesh_local_min) != 3 or
            not isinstance(mesh_local_max, list) or len(mesh_local_max) != 3):
        raise ManagerError(
            "GLB has no finite mesh-local and scene-world bounds; export POSITION min/max "
            "metadata before authoring"
        )
    source_height_m = float(bounds_max[1]) - float(bounds_min[1])
    if not math.isfinite(source_height_m) or source_height_m <= 1.0e-6:
        raise ManagerError(
            "GLB scene-world height is too small to calibrate"
        )
    return {
        "schema": "mdkr-character-glb-intake-v2",
        "model": str(model_path.resolve()),
        "model_sha256": hashlib.sha256(payload).hexdigest(),
        "vertices": report["vertex_count"],
        "triangles": report["triangle_count"],
        "materials": report["material_count"],
        "textures": report["texture_count"],
        "skins": report["skin_count"],
        "joints": report["max_joints"],
        "source_height_m": source_height_m,
        "mesh_local_bounds": [mesh_local_min, mesh_local_max],
        "scene_world_bounds": [bounds_min, bounds_max],
        "clips": fallback_choices,
        "source_animation_count": len(clips),
        "nodes": nodes,
        "fallback": fallback,
        "seat": inferred_sockets.get("seat"),
        "head": inferred_sockets.get("head"),
        "warnings": report["warnings"],
        "validation": _validation_summary(validation),
    }


def write_raw_glb_index(model_path: Path, directory: Path,
                        index_path: Path) -> dict[str, Any]:
    """Write the launcher's fixed-field, hex-escaped raw-model inventory."""
    root = _prepare_directory(directory)
    if (index_path.parent.resolve() != root or
            index_path.name != RAW_INTAKE_INDEX_NAME):
        raise ManagerError(
            "raw GLB index must use the launcher's exact file inside the "
            "character directory"
        )
    inventory = inspect_raw_glb(model_path)

    def encoded(value: str | None) -> str:
        return "-" if value is None else value.encode("utf-8").hex()

    lines = [
        "\t".join((
            inventory["schema"], inventory["model_sha256"],
            str(inventory["vertices"]), str(inventory["triangles"]),
            str(inventory["materials"]), str(inventory["textures"]),
            str(inventory["skins"]), str(inventory["joints"]),
            format(inventory["source_height_m"], ".9g"),
            str(len(inventory["clips"])), str(len(inventory["nodes"])),
            *(format(float(value), ".9g")
              for bounds in (
                  inventory["mesh_local_bounds"],
                  inventory["scene_world_bounds"],
              ) for point in bounds for value in point),
        )) + "\n",
        "defaults\t" + "\t".join((
            encoded(inventory["fallback"]), encoded(inventory["seat"]),
            encoded(inventory["head"]),
        )) + "\n",
    ]
    lines.extend(
        f"clip\t{encoded(name)}\n" for name in inventory["clips"]
    )
    lines.extend(
        f"node\t{encoded(name)}\n" for name in inventory["nodes"]
    )
    _write_atomic(index_path, "".join(lines).encode("ascii"))
    return {
        **inventory,
        "index_file": index_path.name,
    }


def build_raw_glb_candidate(
        model_path: Path, license_path: Path, package_id: str,
        display_name: str, spdx: str, attribution: str, source_url: str,
        donor: str, vehicles: tuple[str, ...], source_forward: str,
        target_height_m: float, fallback_clip: str, seat_node: str,
        head_node: str, directory: Path, *,
        expected_model_sha256: str | None = None) -> dict[str, Any]:
    """Build one self-consistent source package for normal reviewed import."""
    root = _prepare_directory(directory)
    model_payload = _bounded_authoring_input(
        model_path, probe.MAX_INPUT_BYTES, "GLB model"
    )
    model_sha256 = hashlib.sha256(model_payload).hexdigest()
    if expected_model_sha256 is not None:
        if (len(expected_model_sha256) != 64 or any(
                character not in "0123456789abcdef"
                for character in expected_model_sha256)):
            raise ManagerError("inspected GLB digest is invalid")
        if model_sha256 != expected_model_sha256:
            raise ManagerError(
                "the GLB changed after inspection; inspect the new bytes before building"
            )
    validation = _validate_character_glb(
        model_payload, "reviewed raw character model"
    )
    license_payload = _bounded_authoring_input(
        license_path, probe.MAX_LICENSE_BYTES, "license file"
    )
    with tempfile.TemporaryDirectory(
            prefix="mdkr-character-raw-intake-") as temporary:
        temporary_root = Path(temporary)
        model = temporary_root / "model.glb"
        license_file = temporary_root / "LICENSE.txt"
        manifest_file = temporary_root / "manifest.json"
        candidate = temporary_root / "candidate.mdkrchar"
        model.write_bytes(model_payload)
        license_file.write_bytes(license_payload)
        manifest, decisions = wizard.build_manifest(
            model, package_id, display_name, spdx, attribution, source_url,
            donor, list(vehicles), source_forward=source_forward,
            target_height_m=target_height_m, fallback_clip=fallback_clip,
            socket_overrides={"seat": seat_node, "head": head_node},
        )
        manifest_file.write_text(
            json.dumps(manifest, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
        built = probe.build_package(
            model, manifest_file, license_file, candidate
        )
        candidate_payload = candidate.read_bytes()
        verification = probe.verify_package(candidate)
        if not verification["valid"]:
            raise ManagerError(
                "generated raw-GLB package failed verification: " +
                "; ".join(verification["errors"])
            )
    destination = root / RAW_INTAKE_CANDIDATE_NAME
    _write_atomic(destination, candidate_payload)
    return {
        "schema": MANAGER_SCHEMA,
        "action": "build-raw-glb-candidate",
        "id": package_id,
        "candidate": str(destination),
        "package_sha256": hashlib.sha256(candidate_payload).hexdigest(),
        "model_sha256": model_sha256,
        "license_sha256": hashlib.sha256(license_payload).hexdigest(),
        "bytes": len(candidate_payload),
        "decisions": decisions,
        "validation": _validation_summary(validation),
        "package": built,
    }


def write_revision_index(package_id: str, directory: Path,
                         index_path: Path) -> dict[str, Any]:
    """Write the launcher's bounded, fixed-field revision inventory."""
    root = _prepare_directory(directory)
    if (index_path.parent.resolve() != root or
            index_path.name != ".launcher-character-revisions.tsv"):
        raise ManagerError(
            "revision index must use the launcher's exact file inside the "
            "character directory"
        )
    revisions = list_revisions(package_id, root)["revisions"]
    visible = revisions[:MAX_UI_REVISIONS]
    lines = [
        f"mdkr-character-revisions-v1\t{len(revisions)}\t{len(visible)}\n"
    ]
    for revision in visible:
        installed_unix = revision.get("installed_unix")
        if (isinstance(installed_unix, bool) or
                not isinstance(installed_unix, int) or installed_unix < 0):
            installed_unix = 0
        lines.append(
            f"{revision['source_sha256']}\t"
            f"{1 if revision['active'] else 0}\t"
            f"{1 if revision['enabled'] else 0}\t{installed_unix}\n"
        )
    payload = "".join(lines).encode("ascii")
    _write_atomic(index_path, payload)
    return {
        "id": package_id,
        "total_revisions": len(revisions),
        "indexed_revisions": len(visible),
        "truncated": len(visible) != len(revisions),
    }


def _canonical_failure_id(record_id: str) -> str:
    if (not isinstance(record_id, str) or len(record_id) != 64 or
            any(character not in "0123456789abcdef" for character in record_id)):
        raise ManagerError(
            "failed-import recovery id must be 64 lowercase hexadecimal characters"
        )
    return record_id


def _list_failed_imports(root: Path) -> dict[str, Any]:
    entries = []
    for record in _failure_records(root):
        available, changed = _failure_source_state(record)
        report_available = False
        if record["validation_report_file"]:
            report_path = _failure_directory(root, create=False) / record[
                "validation_report_file"
            ]
            try:
                report_available = (
                    not report_path.is_symlink() and report_path.is_file() and
                    report_path.stat().st_size <=
                    gltf_validator.MAX_REPORT_BYTES
                )
            except OSError:
                report_available = False
        entries.append({
            **record,
            "source_available": available,
            "source_changed": changed,
            "validation_report_available": report_available,
        })
    return {
        "schema": FAILURE_INDEX_SCHEMA,
        "entries": entries,
        "count": len(entries),
    }


def list_failed_imports(directory: Path) -> dict[str, Any]:
    root = _prepare_directory(directory)
    with _locked(root):
        return _list_failed_imports(root)


def write_failure_index(directory: Path, index_path: Path) -> dict[str, Any]:
    """Write bounded fixed-field recovery inventory for the native launcher."""
    root = _prepare_directory(directory)
    if (index_path.parent.resolve() != root or
            index_path.name != FAILURE_INDEX_NAME):
        raise ManagerError(
            "failed-import index must use the launcher's exact file inside "
            "the character directory"
        )
    with _locked(root):
        entries = _list_failed_imports(root)["entries"]
        rows: list[str] = []
        row_bytes = 0
        for record in entries[:MAX_UI_FAILURES]:
            fields = (
                record["record_id"], str(record["last_failed_unix"]),
                str(record["attempts"]),
                "1" if record["source_available"] else "0",
                "1" if record["source_changed"] else "0",
                "1" if record["validation_report_available"] else "0",
                record["retry_kind"],
                record["source_path"].encode("utf-8").hex(),
                "-" if record["output_path"] is None else
                    record["output_path"].encode("utf-8").hex(),
                record["error"].encode("utf-8").hex(),
            )
            row = "\t".join(fields) + "\n"
            # readCharacterManagerResult deliberately has the same 64 KiB
            # bound. Keep each row complete instead of letting the native side
            # observe a truncated hex field; total remains explicit in header.
            if row_bytes + len(row) > MAX_REPORT_BYTES - 128:
                break
            rows.append(row)
            row_bytes += len(row)
        visible_count = len(rows)
        lines = [
            f"{FAILURE_INDEX_SCHEMA}\t{len(entries)}\t{visible_count}\n",
            *rows,
        ]
        _write_atomic(index_path, "".join(lines).encode("ascii"))
    return {
        "schema": FAILURE_INDEX_SCHEMA,
        "total_failures": len(entries),
        "indexed_failures": visible_count,
        "truncated": len(entries) != visible_count,
        "index_file": index_path.name,
    }


def _remove_failure_record(root: Path, record_id: str) -> list[str]:
    directory = _failure_directory(root, create=False)
    record_path = directory / f"{record_id}.json"
    record = _read_failure_record(record_path)
    removed = []
    report_file = record["validation_report_file"]
    if report_file is not None:
        report_path = directory / report_file
        if report_path.is_symlink():
            raise ManagerError("failed-import validation report is linked")
        if report_path.exists():
            report_path.unlink()
            removed.append(report_path.name)
    record_path.unlink()
    removed.append(record_path.name)
    return removed


def forget_failed_import(record_id: str, directory: Path) -> dict[str, Any]:
    record_id = _canonical_failure_id(record_id)
    root = _prepare_directory(directory)
    with _locked(root):
        removed = _remove_failure_record(root, record_id)
    return {
        "schema": FAILURE_INDEX_SCHEMA,
        "action": "forget-failed-import",
        "record_id": record_id,
        "removed": removed,
    }


def retry_failed_import(record_id: str, directory: Path) -> dict[str, Any]:
    """Recheck exact unchanged source bytes, then clear resolved recovery state."""
    record_id = _canonical_failure_id(record_id)
    root = _prepare_directory(directory)
    with _locked(root):
        failure_dir = _failure_directory(root, create=False)
        record_path = failure_dir / f"{record_id}.json"
        record = _read_failure_record(record_path)
        available, changed = _failure_source_state(record, exact=True)
        if not available:
            raise ManagerError(
                "failed-import source is missing; locate it and start a new import"
            )
        if record["source_sha256"] is None:
            raise ManagerError(
                "failed-import source was never safely hashable; start a new import"
            )
        if changed:
            raise ManagerError(
                "failed-import source changed; inspect the new bytes as a new import"
            )
        source = Path(record["source_path"])
        try:
            if record["retry_kind"] == "raw-inspect":
                result = inspect_raw_glb(source)
            elif record["retry_kind"] == "package-inspect":
                result = inspect(source)
            elif record["retry_kind"] == "adapter-inspect":
                result = inspect_adapter_output(source)
            else:
                output = record["output_path"]
                if output is None:
                    raise ManagerError(
                        "failed conversion recovery has no output path"
                    )
                result = convert_authoring_source(source, Path(output))
        except (OSError, zipfile.BadZipFile, json.JSONDecodeError, probe.ProbeError,
                source_adapter.AdapterOutputError,
                compiler.CompileError, ManagerError) as exc:
            record["attempts"] += 1
            record["last_failed_unix"] = int(time.time())
            record["error"] = _bounded_failure_text(
                str(exc), MAX_FAILURE_TEXT_BYTES, "failed-import retry error"
            )
            report_path = failure_dir / f"{record_id}.validation.json"
            if isinstance(exc, ImportValidationError) and isinstance(
                    exc.validation, dict):
                gltf_validator.write_report(report_path, exc.validation)
                record["validation_report_file"] = report_path.name
            encoded = (
                json.dumps(record, indent=2, sort_keys=True) + "\n"
            ).encode("utf-8")
            if len(encoded) > MAX_REPORT_BYTES:
                raise ManagerError(
                    "failed-import recovery metadata exceeds its bound"
                ) from exc
            _write_atomic(record_path, encoded)
            raise
        removed = _remove_failure_record(root, record_id)
    return {
        "schema": FAILURE_INDEX_SCHEMA,
        "action": "retry-failed-import",
        "record_id": record_id,
        "resolved": True,
        "removed": removed,
        "result": result,
    }


def export_failed_import_report(record_id: str, directory: Path,
                                output_path: Path) -> dict[str, Any]:
    record_id = _canonical_failure_id(record_id)
    root = _prepare_directory(directory)
    with _locked(root):
        failure_dir = _failure_directory(root, create=False)
        record = _read_failure_record(failure_dir / f"{record_id}.json")
        validation = None
        if record["validation_report_file"] is not None:
            report_path = failure_dir / record["validation_report_file"]
            if (report_path.is_symlink() or not report_path.is_file() or
                    report_path.stat().st_size >
                    gltf_validator.MAX_REPORT_BYTES):
                raise ManagerError(
                    "failed-import validation report is missing, linked, or oversized"
                )
            validation = probe.json_loads_strict(
                report_path.read_bytes(), "failed-import validation report"
            )
    destination = output_path.absolute()
    if destination.suffix.lower() != ".json":
        raise ManagerError("failed-import export must use a .json suffix")
    if destination.parent.is_symlink() or not destination.parent.is_dir():
        raise ManagerError("failed-import export directory must be a real directory")
    payload = (json.dumps({
        "schema": "mdkr-character-import-diagnostic-export-v1",
        "failure": record,
        "validation": validation,
    }, indent=2, sort_keys=True) + "\n").encode("utf-8")
    if len(payload) > gltf_validator.MAX_REPORT_BYTES + MAX_REPORT_BYTES:
        raise ManagerError("failed-import diagnostic export exceeds its bound")
    try:
        _write_exclusive(destination, payload)
    except FileExistsError as exc:
        raise ManagerError("failed-import export destination already exists") from exc
    return {
        "schema": FAILURE_INDEX_SCHEMA,
        "action": "export-failed-import-report",
        "record_id": record_id,
        "exported_file": str(destination),
        "bytes": len(payload),
        "validation_included": validation is not None,
    }


def prepare(package_path: Path, output_path: Path) -> dict[str, Any]:
    verification = probe.verify_package(package_path)
    if not verification["valid"]:
        raise ManagerError("invalid source package: " + "; ".join(verification["errors"]))
    with zipfile.ZipFile(package_path) as archive:
        manifest = probe.json_loads_strict(archive.read("manifest.json"), "manifest")
        model = archive.read("model.glb")
        portrait = (
            archive.read("portrait.png")
            if manifest.get("schema") in probe.IDENTITY_SCHEMAS else None
        )
        digest = _compiler_source_digest(archive)
    validation = _validate_character_glb(
        model, f"character package {manifest['id']!r} model"
    )
    compiled, compile_report = compiler.compile_character(
        model, manifest, digest, portrait
    )
    package_report = probe.add_compiled_cache(package_path, compiled, output_path)
    return {
        "schema": MANAGER_SCHEMA,
        "id": manifest["id"],
        "display_name": manifest["display_name"],
        "portable_package": str(output_path),
        "package_sha256": package_report["sha256"],
        "compiled_sha256": hashlib.sha256(compiled).hexdigest(),
        "validation": _validation_summary(validation),
        "report": compile_report,
    }


def _owned_provenance_name(name: str, package_id: str, suffix: str) -> bool:
    prefix = package_id + "."
    if not name.startswith(prefix) or not name.endswith(suffix):
        return False
    digest = name[len(prefix):-len(suffix)]
    return len(digest) == 64 and all(character in "0123456789abcdef"
                                     for character in digest)


def remove(package_id: str, directory: Path) -> dict[str, Any]:
    if probe.ID_RE.fullmatch(package_id) is None:
        raise ManagerError("invalid package id")
    root = _prepare_directory(directory)
    removed: list[str] = []
    failed: list[str] = []
    with _locked(root):
        candidates = [
            root / f"{package_id}.mdkc",
            root / f"{package_id}.mdkc.disabled",
        ]
        candidates.extend(sorted(root.glob(f"{package_id}.*.mdkrchar")))
        candidates.extend(sorted(root.glob(f"{package_id}.*.json")))
        for path in candidates:
            if path.parent != root:
                continue
            owned = path.name in (
                f"{package_id}.mdkc", f"{package_id}.mdkc.disabled"
            ) or (
                _owned_provenance_name(path.name, package_id, ".mdkrchar") or
                _owned_provenance_name(path.name, package_id, ".json")
            )
            if not owned or not (path.exists() or path.is_symlink()):
                continue
            if path.is_symlink() or not path.is_file():
                failed.append(path.name)
                continue
            try:
                path.unlink()
                removed.append(path.name)
            except OSError:
                failed.append(path.name)
    if failed:
        detail = ", ".join(failed[:8])
        if len(failed) > 8:
            detail += f", and {len(failed) - 8} more"
        raise ManagerError(
            f"partial deletion removed {len(removed)} owned file(s), but "
            f"{len(failed)} could not be removed: {detail}"
        )
    if not removed:
        raise ManagerError("no regular installed files matched that character id")
    return {"id": package_id, "removed": removed}


def set_enabled(package_id: str, directory: Path,
                enabled: bool) -> dict[str, Any]:
    """Move one validated cache across the runtime-discovery boundary."""
    if probe.ID_RE.fullmatch(package_id) is None:
        raise ManagerError("invalid package id")
    if not isinstance(enabled, bool):
        raise ManagerError("enabled state must be boolean")
    root = _prepare_directory(directory)
    with _locked(root):
        source, current_enabled = _installed_cache_path(root, package_id)
        assert source is not None
        if current_enabled == enabled:
            state = "enabled" if enabled else "disabled"
            raise ManagerError(f"character {package_id!r} is already {state}")
        payload = source.read_bytes()
        if not _cache_valid(payload):
            raise ManagerError(f"installed cache for {package_id!r} is invalid")
        destination = root / (
            f"{package_id}.mdkc" if enabled
            else f"{package_id}.mdkc.disabled"
        )
        if destination.exists() or destination.is_symlink():
            raise ManagerError("destination cache state already exists")
        # The shared lock makes this a single state transition for cooperating
        # tools. rename preserves the validated bytes and never exposes a
        # partially written cache.
        source.rename(destination)
    return {
        "id": package_id,
        "enabled": enabled,
        "cache_file": destination.name,
        "retained_source_history": True,
    }


def clean(directory: Path) -> dict[str, Any]:
    root = _prepare_directory(directory)
    removed: list[str] = []
    with _locked(root):
        for path in sorted(root.glob("*.json")):
            try:
                report = _read_report(path)
                if report.get("schema") != MANAGER_SCHEMA:
                    continue
                package_id = report.get("id", "")
                if (not isinstance(package_id, str) or
                        probe.ID_RE.fullmatch(package_id) is None):
                    continue
                cache, _ = _installed_cache_path(
                    root, package_id, required=False
                )
                cache_data = cache.read_bytes() if cache is not None else b""
                if (_cache_valid(cache_data) and
                        cache_data[20:52].hex() == report.get("cache_source_digest")):
                    continue
                source = root / report.get("source_file", "")
                if source.parent == root and source.is_file() and not source.is_symlink():
                    source.unlink()
                    removed.append(source.name)
                path.unlink()
                removed.append(path.name)
            except (OSError, TypeError, ValueError, ManagerError):
                continue
    return {"removed": removed}


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--directory", type=Path)
    parser.add_argument(
        "--result-file",
        type=Path,
        help="write the same bounded JSON result for a native launcher caller",
    )
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser(
        "tool-info",
        help="report the frozen/source importer contract without touching user data",
    )
    install_parser = sub.add_parser("install")
    install_parser.add_argument("package", type=Path)
    inspect_parser = sub.add_parser("inspect")
    inspect_parser.add_argument("package", type=Path)
    candidate_index_parser = sub.add_parser("write-candidate-index")
    candidate_index_parser.add_argument("package", type=Path)
    candidate_index_parser.add_argument("output", type=Path)
    reviewed_parser = sub.add_parser("install-reviewed")
    reviewed_parser.add_argument("package", type=Path)
    reviewed_parser.add_argument("expected_package_sha256")
    reviewed_parser.add_argument("expected_installed_digest")
    sub.add_parser("list")
    revisions_parser = sub.add_parser("revisions")
    revisions_parser.add_argument("id")
    raw_index_parser = sub.add_parser("write-raw-glb-index")
    raw_index_parser.add_argument("model", type=Path)
    raw_index_parser.add_argument("output", type=Path)
    convert_parser = sub.add_parser(
        "convert-authoring-source",
        help="convert one DAE or extract one unambiguous DAE/GLB from a safe ZIP",
    )
    convert_parser.add_argument("input", type=Path)
    convert_parser.add_argument("output", type=Path)
    adapter_index_parser = sub.add_parser(
        "write-adapter-output-index",
        help="validate a canonical data-only adapter result for review",
    )
    adapter_index_parser.add_argument("input", type=Path)
    adapter_index_parser.add_argument("output", type=Path)
    adapter_extract_parser = sub.add_parser(
        "extract-reviewed-adapter-output",
        help="revalidate and exclusively extract an exact reviewed adapter result",
    )
    adapter_extract_parser.add_argument("input", type=Path)
    adapter_extract_parser.add_argument("output", type=Path)
    adapter_extract_parser.add_argument("expected_artifact_sha256")
    adapter_extract_parser.add_argument("expected_model_sha256")
    raw_build_parser = sub.add_parser(
        "build-raw-glb",
        help="build a reviewed source-only candidate from launcher intake fields",
    )
    for field in (
        "model_hex", "license_hex", "id_hex", "display_name_hex",
        "spdx_hex", "attribution_hex", "source_url_hex", "donor_hex",
        "source_forward_hex", "fallback_hex", "seat_hex", "head_hex",
    ):
        raw_build_parser.add_argument(field)
    raw_build_parser.add_argument("expected_model_sha256")
    raw_build_parser.add_argument("vehicle_mask", type=int)
    raw_build_parser.add_argument("target_height", type=float)
    restore_parser = sub.add_parser("restore")
    restore_parser.add_argument("id")
    restore_parser.add_argument("source_sha256")
    rebuild_parser = sub.add_parser("rebuild")
    rebuild_parser.add_argument("id")
    export_parser = sub.add_parser("export")
    export_parser.add_argument("id")
    export_parser.add_argument("source_sha256")
    export_parser.add_argument("output", type=Path)
    portable_export_parser = sub.add_parser(
        "export-portable",
        help="compile and export an authenticated retained source revision",
    )
    portable_export_parser.add_argument("id")
    portable_export_parser.add_argument("source_sha256")
    portable_export_parser.add_argument("output", type=Path)
    index_parser = sub.add_parser("write-revision-index")
    index_parser.add_argument("id")
    index_parser.add_argument("output", type=Path)
    sub.add_parser(
        "failed-imports",
        help="list private metadata-only failed-import recovery records",
    )
    failure_index_parser = sub.add_parser("write-failure-index")
    failure_index_parser.add_argument("output", type=Path)
    failure_retry_parser = sub.add_parser("retry-failed-import")
    failure_retry_parser.add_argument("record_id")
    failure_forget_parser = sub.add_parser("forget-failed-import")
    failure_forget_parser.add_argument("record_id")
    failure_export_parser = sub.add_parser("export-failed-import-report")
    failure_export_parser.add_argument("record_id")
    failure_export_parser.add_argument("output", type=Path)
    remove_parser = sub.add_parser("remove")
    remove_parser.add_argument("id")
    enable_parser = sub.add_parser("enable")
    enable_parser.add_argument("id")
    disable_parser = sub.add_parser("disable")
    disable_parser.add_argument("id")
    prepare_parser = sub.add_parser(
        "prepare", help="embed a verified deterministic cache for player import"
    )
    prepare_parser.add_argument("package", type=Path)
    prepare_parser.add_argument("output", type=Path)
    identity_parser = sub.add_parser(
        "revise-identity",
        help="create and install a new portrait/minimap source revision",
    )
    identity_parser.add_argument("id")
    identity_parser.add_argument("portrait", type=Path)
    identity_parser.add_argument("red", type=int)
    identity_parser.add_argument("green", type=int)
    identity_parser.add_argument("blue", type=int)
    rgba_parser = sub.add_parser(
        "revise-identity-rgba",
        help="create identity media from an exact 40x40 RGBA editor canvas",
    )
    rgba_parser.add_argument("id")
    rgba_parser.add_argument("rgba_hex")
    rgba_parser.add_argument("red", type=int)
    rgba_parser.add_argument("green", type=int)
    rgba_parser.add_argument("blue", type=int)
    profile_parser = sub.add_parser(
        "revise-profile",
        help="create and install a donor/vehicle compatibility revision",
    )
    profile_parser.add_argument("id")
    profile_parser.add_argument("donor")
    profile_parser.add_argument("vehicles", nargs="+")
    rig_parser = sub.add_parser(
        "revise-rig",
        help="create and install a skeleton role-map revision",
    )
    rig_parser.add_argument("id")
    rig_parser.add_argument("draft", type=Path)
    build_draft_parser = sub.add_parser(
        "build-draft",
        help="compile one reviewed Workshop draft as a single source revision",
    )
    build_draft_parser.add_argument("id")
    build_draft_parser.add_argument("draft", type=Path)
    sub.add_parser("clean")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    try:
        if args.command not in ("prepare", "inspect", "tool-info") and args.directory is None:
            raise ManagerError("--directory is required for this command")
        if args.command == "tool-info":
            report = {
                "schema": IMPORTER_INFO_SCHEMA,
                "manager_schema": MANAGER_SCHEMA,
                "compiler_id": COMPILER_ID,
                "frozen": bool(getattr(sys, "frozen", False)),
                "python": ".".join(str(part) for part in sys.version_info[:3]),
            }
        elif args.command == "install":
            report = install(args.package, args.directory)
        elif args.command == "inspect":
            report = inspect(args.package)
        elif args.command == "write-candidate-index":
            report = write_candidate_index(
                args.package, args.directory, args.output
            )
        elif args.command == "install-reviewed":
            report = install_reviewed(
                args.package, args.directory,
                args.expected_package_sha256,
                args.expected_installed_digest,
            )
        elif args.command == "prepare":
            report = prepare(args.package, args.output)
        elif args.command == "revise-identity":
            report = revise_identity(
                args.id, args.portrait,
                (args.red, args.green, args.blue), args.directory,
            )
        elif args.command == "revise-identity-rgba":
            if (
                len(args.rgba_hex) != 40 * 40 * 8
                or any(character not in "0123456789abcdef"
                       for character in args.rgba_hex)
            ):
                raise ManagerError(
                    "RGBA draft must be exactly 12,800 lowercase hex characters"
                )
            report = revise_identity_rgba(
                args.id, bytes.fromhex(args.rgba_hex),
                (args.red, args.green, args.blue), args.directory,
            )
        elif args.command == "revise-profile":
            report = revise_profile(
                args.id, args.donor, tuple(args.vehicles), args.directory,
            )
        elif args.command == "revise-rig":
            report = revise_rig(args.id, args.draft, args.directory)
        elif args.command == "build-draft":
            report = build_workshop_draft(
                args.id, args.draft, args.directory
            )
        elif args.command == "remove":
            report = remove(args.id, args.directory)
        elif args.command == "revisions":
            report = list_revisions(args.id, args.directory)
        elif args.command == "write-raw-glb-index":
            report = write_raw_glb_index(
                args.model, args.directory, args.output
            )
        elif args.command == "convert-authoring-source":
            report = convert_authoring_source(args.input, args.output)
        elif args.command == "write-adapter-output-index":
            report = write_adapter_output_index(
                args.input, args.directory, args.output
            )
        elif args.command == "extract-reviewed-adapter-output":
            report = extract_reviewed_adapter_output(
                args.input, args.output, args.expected_artifact_sha256,
                args.expected_model_sha256,
            )
        elif args.command == "build-raw-glb":
            vehicle_mask = args.vehicle_mask
            if vehicle_mask < 1 or vehicle_mask > 7:
                raise ManagerError("raw intake vehicle mask must be 1-7")
            report = build_raw_glb_candidate(
                Path(_decode_launcher_hex(
                    args.model_hex, 4096, "model path")),
                Path(_decode_launcher_hex(
                    args.license_hex, 4096, "license path")),
                _decode_launcher_hex(args.id_hex, 64, "package id"),
                _decode_launcher_hex(
                    args.display_name_hex, 96, "display name"),
                _decode_launcher_hex(args.spdx_hex, 128, "SPDX"),
                _decode_launcher_hex(
                    args.attribution_hex, 256, "attribution"),
                _decode_launcher_hex(
                    args.source_url_hex, 2048, "source URL"),
                _decode_launcher_hex(args.donor_hex, 16, "donor"),
                tuple(
                    vehicle for bit, vehicle in enumerate(
                        ("car", "hovercraft", "plane")
                    ) if vehicle_mask & (1 << bit)
                ),
                _decode_launcher_hex(
                    args.source_forward_hex, 2, "source forward"),
                args.target_height,
                _decode_launcher_hex(
                    args.fallback_hex, 256, "fallback clip"),
                _decode_launcher_hex(args.seat_hex, 256, "seat node"),
                _decode_launcher_hex(args.head_hex, 256, "head node"),
                args.directory,
                expected_model_sha256=args.expected_model_sha256,
            )
        elif args.command == "restore":
            report = restore_revision(
                args.id, args.source_sha256, args.directory
            )
        elif args.command == "rebuild":
            report = rebuild_current(args.id, args.directory)
        elif args.command == "export":
            report = export_revision(
                args.id, args.source_sha256, args.directory, args.output
            )
        elif args.command == "export-portable":
            report = export_portable_revision(
                args.id, args.source_sha256, args.directory, args.output
            )
        elif args.command == "write-revision-index":
            report = write_revision_index(
                args.id, args.directory, args.output
            )
        elif args.command == "failed-imports":
            report = list_failed_imports(args.directory)
        elif args.command == "write-failure-index":
            report = write_failure_index(args.directory, args.output)
        elif args.command == "retry-failed-import":
            report = retry_failed_import(args.record_id, args.directory)
        elif args.command == "forget-failed-import":
            report = forget_failed_import(args.record_id, args.directory)
        elif args.command == "export-failed-import-report":
            report = export_failed_import_report(
                args.record_id, args.directory, args.output
            )
        elif args.command == "enable":
            report = set_enabled(args.id, args.directory, True)
        elif args.command == "disable":
            report = set_enabled(args.id, args.directory, False)
        elif args.command == "clean":
            report = clean(args.directory)
        else:
            report = list_installed(args.directory)
        report = {"ok": True, **report}
        status = 0
    except (OSError, zipfile.BadZipFile, json.JSONDecodeError, probe.ProbeError,
            source_adapter.AdapterOutputError,
            compiler.CompileError, ManagerError) as exc:
        report = {"ok": False, "error": str(exc)}
        if args.directory is not None:
            try:
                recovery_root = _prepare_directory(args.directory)
                with _locked(recovery_root):
                    recovery = _record_import_failure(
                        recovery_root, args, exc
                    )
                if recovery is not None:
                    report["failed_import"] = recovery
            except (OSError, probe.ProbeError,
                    source_adapter.AdapterOutputError, ManagerError,
                    gltf_validator.ValidatorError) as recovery_error:
                report["recovery_error"] = str(recovery_error)
        status = 2
    payload = (json.dumps(report, indent=2, sort_keys=True) + "\n").encode("utf-8")
    print(payload.decode("utf-8"), end="")
    if args.result_file is not None:
        try:
            if args.directory is None or args.result_file.parent != args.directory:
                raise ManagerError("result file must be directly inside the character directory")
            _write_atomic(args.result_file, payload)
        except (OSError, ManagerError) as exc:
            print(json.dumps({"ok": False, "error": f"could not write launcher result: {exc}"}))
            return 2
    return status


if __name__ == "__main__":
    raise SystemExit(main())
