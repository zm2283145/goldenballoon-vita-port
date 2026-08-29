#!/usr/bin/env python3
"""Bounded, provenance-pinned adapter for the Khronos glTF Validator."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
import struct
import subprocess
import sys
import tempfile
import threading
from pathlib import Path
from typing import Any, BinaryIO


ADAPTER_SCHEMA = "mdkr-gltf-validation-v1"
MANIFEST_SCHEMA = "mdkr-gltf-validator-build-v1"
VALIDATOR_VERSION = "2.0.0-dev.3.10"
VALIDATOR_COMMIT = "bcd52cc4ba5f333b2999a58f67cc05ddf28b4fb1"
SOURCE_ARCHIVE_SHA256 = (
    "b17f302eb0fa9fecc9874c47118f9beea72aff87e7fabc7f5c8c11417fd1c9f1"
)
DART_VERSION = "2.19.6"
DART_MACOS_ARM64_ARCHIVE_SHA256 = (
    "3c6b54b6f44bca38bdc7858ea45734f297951eba5fb10c8fa7b86b4a3f43edb6"
)
PUBSPEC_LOCK_SHA256 = "9fec69b760a6789506e1d03881e48c0b9024779559b67ba427a1b0309c841749"
OFFICIAL_ARCHIVE_SHA256 = {
    "linux-x86_64":
        "168eba887964125abe17ae97899b38d0b3cfd73c266c78424c194929ddcbc522",
    "windows-x86_64":
        "c5068f51205deedc28acc3529ee7e11ee60e853454f673093398eba80142202c",
}
PINNED_BUILD_SHA256 = {
    "linux-x86_64":
        "5cba1e097935c9efd929d24a49610824259b030f0ae45672273bfbcbb5b87a62",
    "windows-x86_64":
        "4388a152ff90b68c6430ae03862e05e257a9d50a500ed7d0eb1cd420dc75ff96",
    "darwin-arm64":
        "cdee231f253b3c369222354d036016c159c713c994e990d08412da2870d8c440",
}
MAX_MANIFEST_BYTES = 64 * 1024
MAX_REPORT_BYTES = 8 * 1024 * 1024
MAX_STDERR_BYTES = 128 * 1024
MAX_GLB_BYTES = 512 * 1024 * 1024
VALIDATION_TIMEOUT_SECONDS = 120
MAX_ISSUES = 256
MANIFEST_KEYS = {
    "schema", "version", "commit", "target", "distribution",
    "archive_sha256", "source_archive_sha256", "dart_version",
    "dart_sdk_archive_sha256", "pubspec_lock_sha256", "executable",
    "executable_bytes", "build_executable_sha256", "executable_sha256",
}


class ValidatorError(ValueError):
    """The validator executable, attestation, or report contract failed."""


def sha256_bytes(payload: bytes) -> str:
    return hashlib.sha256(payload).hexdigest()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while chunk := stream.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def target_id() -> str:
    system = platform.system().lower()
    machine = platform.machine().lower()
    machine = {
        "amd64": "x86_64", "x86-64": "x86_64", "aarch64": "arm64",
    }.get(machine, machine)
    target = f"{system}-{machine}"
    if target not in PINNED_BUILD_SHA256:
        raise ValidatorError(f"unsupported glTF Validator target: {target}")
    return target


def _strict_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise ValidatorError(f"duplicate JSON key: {key}")
        result[key] = value
    return result


def _reject_constant(value: str) -> None:
    raise ValidatorError(f"non-finite JSON number: {value}")


def _load_json_bytes(payload: bytes, label: str) -> dict[str, Any]:
    try:
        result = json.loads(
            payload.decode("utf-8"), object_pairs_hook=_strict_object,
            parse_constant=_reject_constant,
        )
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise ValidatorError(f"{label} is not strict UTF-8 JSON") from exc
    if not isinstance(result, dict):
        raise ValidatorError(f"{label} root must be an object")
    return result


def _canonical_sha(value: Any, label: str) -> str:
    if (not isinstance(value, str) or len(value) != 64 or
            any(character not in "0123456789abcdef" for character in value)):
        raise ValidatorError(f"{label} is not a canonical SHA-256")
    return value


def _binary_target(path: Path) -> str:
    with path.open("rb") as stream:
        header = stream.read(4096)
    if len(header) >= 12 and header[:4] == b"\xcf\xfa\xed\xfe":
        cpu_type = struct.unpack_from("<I", header, 4)[0]
        machine = {0x01000007: "x86_64", 0x0100000C: "arm64"}.get(cpu_type)
        if machine is not None:
            return f"darwin-{machine}"
    if len(header) >= 20 and header[:4] == b"\x7fELF" and header[4] == 2:
        endian = "<" if header[5] == 1 else ">" if header[5] == 2 else None
        if endian is not None:
            machine = {62: "x86_64", 183: "arm64"}.get(
                struct.unpack_from(endian + "H", header, 18)[0]
            )
            if machine is not None:
                return f"linux-{machine}"
    if len(header) >= 64 and header[:2] == b"MZ":
        pe_offset = struct.unpack_from("<I", header, 0x3C)[0]
        if pe_offset + 6 <= len(header) and header[pe_offset:pe_offset + 4] == b"PE\0\0":
            machine = {0x8664: "x86_64", 0xAA64: "arm64"}.get(
                struct.unpack_from("<H", header, pe_offset + 4)[0]
            )
            if machine is not None:
                return f"windows-{machine}"
    raise ValidatorError("glTF Validator executable has an unknown binary format")


def _load_manifest(path: Path) -> dict[str, Any]:
    if path.is_symlink() or not path.is_file():
        raise ValidatorError("glTF Validator manifest must be a regular file")
    payload = path.read_bytes()
    if not payload or len(payload) > MAX_MANIFEST_BYTES:
        raise ValidatorError("glTF Validator manifest size is invalid")
    return _load_json_bytes(payload, "glTF Validator manifest")


def verify_installation(executable: Path, manifest_path: Path,
                        expected_target: str | None = None) -> dict[str, Any]:
    expected_target = expected_target or target_id()
    if expected_target not in PINNED_BUILD_SHA256:
        raise ValidatorError(f"unsupported expected validator target: {expected_target}")
    if executable.is_symlink() or not executable.is_file():
        raise ValidatorError("glTF Validator executable must be a regular file")
    if not os.access(executable, os.X_OK):
        raise ValidatorError("glTF Validator executable is not executable")
    if _binary_target(executable) != expected_target:
        raise ValidatorError("glTF Validator executable architecture is wrong")
    manifest = _load_manifest(manifest_path)
    if set(manifest) != MANIFEST_KEYS:
        missing = sorted(MANIFEST_KEYS - set(manifest))
        extra = sorted(set(manifest) - MANIFEST_KEYS)
        raise ValidatorError(
            f"glTF Validator manifest keys differ (missing={missing}, extra={extra})"
        )
    executable_name = "gltf_validator.exe" if expected_target.startswith("windows-") else "gltf_validator"
    common = {
        "schema": MANIFEST_SCHEMA,
        "version": VALIDATOR_VERSION,
        "commit": VALIDATOR_COMMIT,
        "target": expected_target,
        "executable": executable_name,
        "executable_bytes": executable.stat().st_size,
        "executable_sha256": sha256_file(executable),
        "build_executable_sha256": PINNED_BUILD_SHA256[expected_target],
    }
    for key, expected in common.items():
        if manifest.get(key) != expected:
            raise ValidatorError(
                f"glTF Validator manifest {key} mismatch: expected {expected!r}, "
                f"found {manifest.get(key)!r}"
            )
    _canonical_sha(manifest["executable_sha256"], "validator executable hash")
    if expected_target in OFFICIAL_ARCHIVE_SHA256:
        if manifest["executable_sha256"] != PINNED_BUILD_SHA256[expected_target]:
            raise ValidatorError(
                "official glTF Validator executable bytes differ from the pin"
            )
        expected_distribution = {
            "distribution": "official-release",
            "archive_sha256": OFFICIAL_ARCHIVE_SHA256[expected_target],
            "source_archive_sha256": None,
            "dart_version": None,
            "dart_sdk_archive_sha256": None,
            "pubspec_lock_sha256": None,
        }
    else:
        expected_distribution = {
            "distribution": "pinned-source-build",
            "archive_sha256": None,
            "source_archive_sha256": SOURCE_ARCHIVE_SHA256,
            "dart_version": DART_VERSION,
            "dart_sdk_archive_sha256": DART_MACOS_ARM64_ARCHIVE_SHA256,
            "pubspec_lock_sha256": PUBSPEC_LOCK_SHA256,
        }
    for key, expected in expected_distribution.items():
        if manifest.get(key) != expected:
            raise ValidatorError(
                f"glTF Validator manifest {key} mismatch: expected {expected!r}, "
                f"found {manifest.get(key)!r}"
            )
    return manifest


def resolve_installation(validator: Path | None = None,
                         manifest: Path | None = None) -> tuple[Path, Path]:
    if (validator is None) != (manifest is None):
        raise ValidatorError("validator executable and manifest must be supplied together")
    if validator is not None and manifest is not None:
        if not validator.is_absolute() or not manifest.is_absolute():
            raise ValidatorError("explicit validator paths must be absolute")
        return validator, manifest
    override = os.environ.get("MDKR_GLTF_VALIDATOR")
    if override:
        executable = Path(override)
        if not executable.is_absolute():
            raise ValidatorError("MDKR_GLTF_VALIDATOR must be an absolute path")
    elif getattr(sys, "frozen", False):
        tool_directory = Path(sys.executable).resolve().parent
        executable = tool_directory / "validators" / (
            "gltf_validator.exe" if os.name == "nt" else "gltf_validator"
        )
    else:
        raise ValidatorError(
            "the source importer requires MDKR_GLTF_VALIDATOR to name the "
            "absolute pinned validator executable"
        )
    adjacent_manifest = executable.with_name(executable.name + ".manifest.json")
    if sys.platform == "darwin" and not adjacent_manifest.is_file():
        # Apple bundle sealing treats every payload below Contents/MacOS as
        # nested code. Keep the signed executable there, but place its JSON
        # attestation in the canonical resource area. Standalone frozen tools
        # and the Windows/Linux packages retain the adjacent-manifest layout.
        # An explicit absolute override can legitimately point at the same
        # sealed validator inside an app bundle. Do not derive Contents from
        # the frozen importer's directory: that local does not exist on the
        # override path (and, more importantly, it is not the validator's
        # authority). Walk only the executable's resolved ancestors.
        contents_directory = next(
            (parent for parent in executable.resolve().parents
             if parent.name == "Contents"),
            None,
        )
        if contents_directory is not None:
            resource_manifest = (
                contents_directory
                / "Resources"
                / "ThirdParty"
                / "GltfValidator-MANIFEST.json"
            )
            if resource_manifest.is_file():
                return executable, resource_manifest
    return executable, adjacent_manifest


def _read_bounded(stream: BinaryIO, limit: int, destination: list[bytes],
                  overflow: threading.Event, process: subprocess.Popen[bytes]) -> None:
    size = 0
    while chunk := stream.read(64 * 1024):
        size += len(chunk)
        if size > limit:
            overflow.set()
            try:
                process.kill()
            except OSError:
                pass
            return
        destination.append(chunk)


def _run_bounded(command: list[str], cwd: Path) -> tuple[int, bytes, bytes]:
    try:
        process = subprocess.Popen(
            command, cwd=cwd, stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        )
    except OSError as exc:
        raise ValidatorError(f"could not start glTF Validator: {exc}") from exc
    assert process.stdout is not None and process.stderr is not None
    stdout_chunks: list[bytes] = []
    stderr_chunks: list[bytes] = []
    overflow = threading.Event()
    threads = [
        threading.Thread(
            target=_read_bounded,
            args=(process.stdout, MAX_REPORT_BYTES, stdout_chunks, overflow, process),
            daemon=True,
        ),
        threading.Thread(
            target=_read_bounded,
            args=(process.stderr, MAX_STDERR_BYTES, stderr_chunks, overflow, process),
            daemon=True,
        ),
    ]
    try:
        for thread in threads:
            thread.start()
        try:
            return_code = process.wait(timeout=VALIDATION_TIMEOUT_SECONDS)
        except subprocess.TimeoutExpired as exc:
            process.kill()
            process.wait()
            for thread in threads:
                thread.join()
            raise ValidatorError(
                f"glTF Validator exceeded {VALIDATION_TIMEOUT_SECONDS} seconds"
            ) from exc
        for thread in threads:
            thread.join()
    finally:
        # Popen does not close PIPE handles when communicate() is not used.
        # The reader threads are complete (or the process was killed) before
        # reaching this block, so both handles can be closed deterministically.
        process.stdout.close()
        process.stderr.close()
    if overflow.is_set():
        raise ValidatorError("glTF Validator emitted an oversized report or diagnostic")
    return return_code, b"".join(stdout_chunks), b"".join(stderr_chunks)


def _validate_report(report: dict[str, Any]) -> dict[str, Any]:
    required = {"uri", "mimeType", "validatorVersion", "issues"}
    if not required.issubset(report) or set(report) - (required | {"info"}):
        raise ValidatorError("glTF Validator report has an unexpected top-level schema")
    if report["uri"] != "model.glb":
        raise ValidatorError("glTF Validator report identifies the wrong input")
    if report["mimeType"] != "model/gltf-binary":
        raise ValidatorError("glTF Validator did not recognize a GLB input")
    if report["validatorVersion"] != VALIDATOR_VERSION:
        raise ValidatorError("glTF Validator report version changed")
    issues = report["issues"]
    issue_keys = {
        "numErrors", "numWarnings", "numInfos", "numHints", "messages",
        "truncated",
    }
    if not isinstance(issues, dict) or set(issues) != issue_keys:
        raise ValidatorError("glTF Validator issues object has an unexpected schema")
    for key in ("numErrors", "numWarnings", "numInfos", "numHints"):
        value = issues[key]
        if isinstance(value, bool) or not isinstance(value, int) or value < 0:
            raise ValidatorError(f"glTF Validator {key} is invalid")
    messages = issues["messages"]
    if not isinstance(messages, list) or len(messages) > MAX_ISSUES:
        raise ValidatorError("glTF Validator message list exceeds its configured bound")
    if not isinstance(issues["truncated"], bool):
        raise ValidatorError("glTF Validator truncated flag is invalid")
    for index, message in enumerate(messages):
        allowed = {"code", "severity", "message", "pointer", "offset"}
        if (not isinstance(message, dict) or set(message) - allowed or
                not {"code", "severity", "message"}.issubset(message)):
            raise ValidatorError(f"glTF Validator message {index} is malformed")
        if not isinstance(message["code"], str) or not isinstance(message["message"], str):
            raise ValidatorError(f"glTF Validator message {index} text is malformed")
        if (isinstance(message["severity"], bool) or
                not isinstance(message["severity"], int) or
                message["severity"] not in range(4)):
            raise ValidatorError(f"glTF Validator message {index} severity is invalid")
        location_count = int("pointer" in message) + int("offset" in message)
        if location_count != 1:
            raise ValidatorError(f"glTF Validator message {index} location is ambiguous")
        if "pointer" in message and not isinstance(message["pointer"], str):
            raise ValidatorError(f"glTF Validator message {index} pointer is invalid")
        if "offset" in message and (
                isinstance(message["offset"], bool) or
                not isinstance(message["offset"], int) or message["offset"] < 0):
            raise ValidatorError(f"glTF Validator message {index} offset is invalid")
    return issues


def validate_glb_bytes(payload: bytes, *, validator: Path | None = None,
                       manifest: Path | None = None) -> dict[str, Any]:
    if not isinstance(payload, bytes) or not payload:
        raise ValidatorError("GLB input must be non-empty bytes")
    if len(payload) > MAX_GLB_BYTES:
        raise ValidatorError(f"GLB input exceeds {MAX_GLB_BYTES} bytes")
    executable, manifest_path = resolve_installation(validator, manifest)
    identity = verify_installation(executable, manifest_path)
    with tempfile.TemporaryDirectory(prefix="mdkr-gltf-validation.") as raw:
        temporary = Path(raw)
        model = temporary / "model.glb"
        config = temporary / "validator-config.yaml"
        model.write_bytes(payload)
        config.write_text(f"max-issues: {MAX_ISSUES}\n", encoding="ascii")
        command = [
            str(executable), "--stdout", "--validate-resources",
            "--no-write-timestamp", "--no-absolute-path", "--no-messages",
            "--config", config.name, model.name,
        ]
        return_code, stdout, stderr = _run_bounded(command, temporary)
    report = _load_json_bytes(stdout, "glTF Validator report")
    issues = _validate_report(report)
    num_errors = issues["numErrors"]
    if return_code not in (0, 1):
        detail = stderr.decode("utf-8", errors="replace").strip().splitlines()
        suffix = f": {detail[-1]}" if detail else ""
        raise ValidatorError(f"glTF Validator exited {return_code}{suffix}")
    if (return_code == 0) != (num_errors == 0):
        raise ValidatorError("glTF Validator exit status disagrees with its report")
    canonical_report = json.loads(json.dumps(report, sort_keys=True))
    envelope = {
        "schema": ADAPTER_SCHEMA,
        "source_sha256": sha256_bytes(payload),
        "valid": num_errors == 0,
        "validator": {
            "version": VALIDATOR_VERSION,
            "commit": VALIDATOR_COMMIT,
            "target": identity["target"],
            "distribution": identity["distribution"],
            "build_executable_sha256": identity["build_executable_sha256"],
            "executable_sha256": identity["executable_sha256"],
            "max_issues": MAX_ISSUES,
        },
        "report": canonical_report,
    }
    encoded = (json.dumps(envelope, indent=2, sort_keys=True) + "\n").encode("utf-8")
    if len(encoded) > MAX_REPORT_BYTES:
        raise ValidatorError("canonical glTF Validator report exceeds its bound")
    return envelope


def write_report(path: Path, report: dict[str, Any]) -> None:
    if path.name in ("", ".", ".."):
        raise ValidatorError("glTF Validator report filename is invalid")
    if path.is_symlink():
        raise ValidatorError("glTF Validator report destination must not be a symlink")
    parent = path.parent.resolve(strict=True)
    if not parent.is_dir():
        raise ValidatorError("glTF Validator report parent must be a directory")
    destination = parent / path.name
    payload = (json.dumps(report, indent=2, sort_keys=True) + "\n").encode("utf-8")
    if len(payload) > MAX_REPORT_BYTES:
        raise ValidatorError("glTF Validator report exceeds its storage bound")
    temporary = destination.with_name(destination.name + ".tmp")
    if temporary.exists() or temporary.is_symlink():
        raise ValidatorError("stale glTF Validator report transaction exists")
    try:
        with temporary.open("xb") as stream:
            stream.write(payload)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, destination)
    except BaseException:
        try:
            temporary.unlink()
        except OSError:
            pass
        raise


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path)
    parser.add_argument("--report", required=True, type=Path)
    parser.add_argument("--validator", type=Path)
    parser.add_argument("--manifest", type=Path)
    return parser


def main(argv: list[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    try:
        if args.input.is_symlink() or not args.input.is_file():
            raise ValidatorError("GLB input must be a regular file")
        if args.input.stat().st_size > MAX_GLB_BYTES:
            raise ValidatorError(f"GLB input exceeds {MAX_GLB_BYTES} bytes")
        payload = args.input.read_bytes()
        if len(payload) > MAX_GLB_BYTES:
            raise ValidatorError(f"GLB input exceeds {MAX_GLB_BYTES} bytes")
        report = validate_glb_bytes(
            payload, validator=args.validator, manifest=args.manifest,
        )
        write_report(args.report, report)
    except (OSError, ValidatorError) as exc:
        print(f"glTF validation failed: {exc}", file=sys.stderr)
        return 2
    issues = report["report"]["issues"]
    print(json.dumps({
        "ok": report["valid"],
        "schema": ADAPTER_SCHEMA,
        "source_sha256": report["source_sha256"],
        "errors": issues["numErrors"],
        "warnings": issues["numWarnings"],
        "infos": issues["numInfos"],
        "hints": issues["numHints"],
        "truncated": issues["truncated"],
        "report": str(args.report),
    }, indent=2, sort_keys=True))
    return 0 if report["valid"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
