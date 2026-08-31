#!/usr/bin/env python3
"""Verify a frozen Character Workshop importer and its build attestation."""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
from pathlib import Path
from typing import Any

import build_character_importer as builder


MAX_MANIFEST_BYTES = 64 * 1024
MAX_TOOL_OUTPUT_BYTES = 16 * 1024
EXPECTED_KEYS = {
    "schema", "target", "python", "pyinstaller", "source_members",
    "source_bundle_sha256", "executable", "executable_bytes",
    "build_executable_sha256", "executable_sha256", "manager_schema",
    "compiler_id",
}
LICENSES = {
    "third_party/character_importer/CPython-LICENSE.txt":
        "78b12c3a81360b357002334f0e70ea0e92eebf7a9b358805c03c48484945f3bb",
    "third_party/character_importer/PyInstaller-COPYING.txt":
        "dcf75fdb959db1e3b41c0f8505069d2ece781b5ec6b3d0a4d30975cfc6580245",
}


class VerificationError(ValueError):
    """An importer attestation or executable mismatch."""


def _strict_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise VerificationError(f"duplicate manifest key: {key}")
        result[key] = value
    return result


def _reject_json_constant(value: str) -> None:
    raise VerificationError(f"non-finite JSON number: {value}")


def _load_manifest(path: Path) -> dict[str, Any]:
    if path.is_symlink() or not path.is_file():
        raise VerificationError("importer manifest must be a regular file")
    payload = path.read_bytes()
    if not payload or len(payload) > MAX_MANIFEST_BYTES:
        raise VerificationError("importer manifest size is invalid")
    try:
        report = json.loads(
            payload.decode("utf-8"), object_pairs_hook=_strict_object,
            parse_constant=_reject_json_constant,
        )
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise VerificationError("importer manifest is not strict UTF-8 JSON") from exc
    if not isinstance(report, dict):
        raise VerificationError("importer manifest root must be an object")
    return report


def _tool_info(executable: Path) -> dict[str, Any]:
    try:
        completed = subprocess.run(
            [str(executable), "tool-info"], check=False,
            stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=builder.TOOL_INFO_TIMEOUT_SECONDS,
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise VerificationError(f"could not execute importer tool-info: {exc}") from exc
    if (len(completed.stdout) > MAX_TOOL_OUTPUT_BYTES or
            len(completed.stderr) > MAX_TOOL_OUTPUT_BYTES):
        raise VerificationError("importer emitted oversized tool-info output")
    if completed.returncode != 0:
        raise VerificationError(
            f"importer tool-info exited {completed.returncode}"
        )
    try:
        report = json.loads(
            completed.stdout.decode("utf-8"), object_pairs_hook=_strict_object,
        )
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise VerificationError("importer tool-info is malformed") from exc
    if not isinstance(report, dict):
        raise VerificationError("importer tool-info root must be an object")
    return report


def verify(root: Path, executable: Path, manifest_path: Path,
           expected_target: str, *, allow_signed: bool = False) -> dict[str, Any]:
    root = root.resolve(strict=True)
    if executable.is_symlink() or not executable.is_file():
        raise VerificationError("character importer must be a regular file")
    if not os.access(executable, os.X_OK):
        raise VerificationError("character importer is not executable")
    manifest = _load_manifest(manifest_path)
    if set(manifest) != EXPECTED_KEYS:
        missing = sorted(EXPECTED_KEYS - set(manifest))
        extra = sorted(set(manifest) - EXPECTED_KEYS)
        raise VerificationError(
            f"importer manifest keys differ (missing={missing}, extra={extra})"
        )
    expected_python = ".".join(map(str, builder.PINNED_PYTHON))
    checks = {
        "schema": builder.BUILD_SCHEMA,
        "target": expected_target,
        "python": expected_python,
        "pyinstaller": builder.PYINSTALLER_VERSION,
        "source_members": list(builder.SOURCE_MODULES),
        "source_bundle_sha256": builder.source_bundle_digest(root),
        "executable": executable.name,
        "executable_bytes": executable.stat().st_size,
        "executable_sha256": builder.sha256_file(executable),
    }
    if allow_signed:
        build_hash = manifest.get("build_executable_sha256")
        if (not isinstance(build_hash, str) or len(build_hash) != 64 or
                any(character not in "0123456789abcdef"
                    for character in build_hash)):
            raise VerificationError(
                "signed importer manifest has no canonical build hash"
            )
    else:
        checks["build_executable_sha256"] = builder.sha256_file(executable)
    for key, expected in checks.items():
        if manifest.get(key) != expected:
            raise VerificationError(
                f"importer manifest {key} mismatch: expected {expected!r}, "
                f"found {manifest.get(key)!r}"
            )
    info = _tool_info(executable)
    info_checks = {
        "ok": True,
        "schema": builder.INFO_SCHEMA,
        "frozen": True,
        "python": expected_python,
        "manager_schema": manifest["manager_schema"],
        "compiler_id": manifest["compiler_id"],
    }
    for key, expected in info_checks.items():
        if info.get(key) != expected:
            raise VerificationError(
                f"importer tool-info {key} mismatch: expected {expected!r}, "
                f"found {info.get(key)!r}"
            )
    if set(info) != set(info_checks):
        raise VerificationError("importer tool-info has an unexpected schema")
    for name, expected_hash in LICENSES.items():
        path = root / name
        if path.is_symlink() or not path.is_file():
            raise VerificationError(f"missing importer runtime notice: {name}")
        if builder.sha256_file(path) != expected_hash:
            raise VerificationError(f"importer runtime notice changed: {name}")
    return manifest


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable", required=True, type=Path)
    parser.add_argument("--manifest", required=True, type=Path)
    parser.add_argument("--target", required=True)
    parser.add_argument(
        "--allow-signed", action="store_true",
        help="accept a platform signature changing final bytes from build bytes",
    )
    parser.add_argument(
        "--repo-root", type=Path,
        default=Path(__file__).resolve().parents[1],
    )
    return parser


def main(argv: list[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    try:
        manifest = verify(
            args.repo_root, args.executable, args.manifest, args.target,
            allow_signed=args.allow_signed,
        )
    except (OSError, VerificationError, builder.BuildError) as exc:
        print(f"character importer verification failed: {exc}", file=sys.stderr)
        return 2
    print(
        "character importer verified: "
        f"{manifest['target']} {manifest['executable_sha256']}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
