#!/usr/bin/env python3
"""Install and manage private .mdkrchar packages and disposable MDKC caches.

The source package is retained for provenance. `<id>.mdkc` is the sole active
runtime record and is replaced atomically only after validation and compilation
complete, so an interrupted import cannot publish a partial character.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import struct
import sys
import time
import zipfile
import zlib
from contextlib import contextmanager
from pathlib import Path
from typing import Any, Iterator

import character_asset_compiler as compiler
import character_asset_probe as probe


MANAGER_SCHEMA = "mdkr-character-install-v1"
COMPILER_ID = "mdkr-character-compiler/1"
LOCK_NAME = ".character-import.lock"


class ManagerError(ValueError):
    pass


def _write_exclusive(path: Path, payload: bytes) -> None:
    descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    try:
        with os.fdopen(descriptor, "wb") as output:
            output.write(payload)
            output.flush()
            os.fsync(output.fileno())
    except BaseException:
        path.unlink(missing_ok=True)
        raise


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


def install(package_path: Path, directory: Path) -> dict[str, Any]:
    root = _prepare_directory(directory)
    package = package_path.read_bytes()
    verification = probe.verify_package(package_path)
    if not verification["valid"]:
        raise ManagerError("invalid source package: " + "; ".join(verification["errors"]))
    package_id = verification["id"]
    if not isinstance(package_id, str) or probe.ID_RE.fullmatch(package_id) is None:
        raise ManagerError("validated package has an unsafe id")
    source_sha = hashlib.sha256(package).hexdigest()
    compiler_digest = hashlib.sha256(
        COMPILER_ID.encode("ascii") + b"\0" + package
    ).digest()
    with zipfile.ZipFile(package_path) as archive:
        manifest = probe.json_loads_strict(
            archive.read("manifest.json"), "manifest"
        )
        model = archive.read("model.glb")
    compiled, compile_report = compiler.compile_character(
        model, manifest, compiler_digest
    )
    if not _cache_valid(compiled):
        raise ManagerError("compiler produced an invalid cache")
    compiled_sha = hashlib.sha256(compiled).hexdigest()
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
        "cache_file": f"{package_id}.mdkc",
        "report": compile_report,
    }
    source_path = root / provenance["source_file"]
    report_path = root / f"{package_id}.{source_sha}.json"
    cache_path = root / provenance["cache_file"]
    with _locked(root):
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


def list_installed(directory: Path) -> dict[str, Any]:
    root = _prepare_directory(directory)
    entries: list[dict[str, Any]] = []
    for report_path in sorted(root.glob("*.json")):
        try:
            report = json.loads(report_path.read_text(encoding="utf-8"))
            if report.get("schema") != MANAGER_SCHEMA:
                continue
            package_id = report["id"]
            cache = root / f"{package_id}.mdkc"
            source = root / report["source_file"]
            cache_data = cache.read_bytes() if cache.is_file() else b""
            active = (
                _cache_valid(cache_data)
                and cache_data[20:52].hex() == report.get("cache_source_digest")
            )
            entries.append(
                {
                    "id": package_id,
                    "display_name": report.get("display_name", package_id),
                    "source_sha256": report.get("source_sha256", ""),
                    "compiler": report.get("compiler", ""),
                    "active": active,
                    "source_present": source.is_file(),
                    "report_file": report_path.name,
                }
            )
        except (OSError, KeyError, TypeError, ValueError, struct.error):
            continue
    # Keep only the provenance matching each active cache's embedded source
    # digest. Older reports remain visible as inactive history until clean.
    return {"schema": MANAGER_SCHEMA, "directory": str(root), "entries": entries}


def remove(package_id: str, directory: Path) -> dict[str, Any]:
    if probe.ID_RE.fullmatch(package_id) is None:
        raise ManagerError("invalid package id")
    root = _prepare_directory(directory)
    removed: list[str] = []
    with _locked(root):
        candidates = [root / f"{package_id}.mdkc"]
        candidates.extend(sorted(root.glob(f"{package_id}.*.mdkrchar")))
        candidates.extend(sorted(root.glob(f"{package_id}.*.json")))
        for path in candidates:
            if path.parent != root or not path.is_file() or path.is_symlink():
                continue
            path.unlink()
            removed.append(path.name)
    return {"id": package_id, "removed": removed}


def clean(directory: Path) -> dict[str, Any]:
    root = _prepare_directory(directory)
    removed: list[str] = []
    with _locked(root):
        for path in sorted(root.glob("*.json")):
            try:
                report = json.loads(path.read_text(encoding="utf-8"))
                if report.get("schema") != MANAGER_SCHEMA:
                    continue
                cache = root / f"{report.get('id', '')}.mdkc"
                cache_data = cache.read_bytes() if cache.is_file() else b""
                if (_cache_valid(cache_data) and
                        cache_data[20:52].hex() == report.get("cache_source_digest")):
                    continue
                source = root / report.get("source_file", "")
                if source.parent == root and source.is_file() and not source.is_symlink():
                    source.unlink()
                    removed.append(source.name)
                path.unlink()
                removed.append(path.name)
            except (OSError, TypeError, ValueError):
                continue
    return {"removed": removed}


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--directory", required=True, type=Path)
    sub = parser.add_subparsers(dest="command", required=True)
    install_parser = sub.add_parser("install")
    install_parser.add_argument("package", type=Path)
    sub.add_parser("list")
    remove_parser = sub.add_parser("remove")
    remove_parser.add_argument("id")
    sub.add_parser("clean")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    try:
        if args.command == "install":
            report = install(args.package, args.directory)
        elif args.command == "remove":
            report = remove(args.id, args.directory)
        elif args.command == "clean":
            report = clean(args.directory)
        else:
            report = list_installed(args.directory)
        print(json.dumps(report, indent=2, sort_keys=True))
        return 0
    except (OSError, zipfile.BadZipFile, json.JSONDecodeError, probe.ProbeError,
            compiler.CompileError, ManagerError) as exc:
        print(json.dumps({"error": str(exc)}, indent=2, sort_keys=True))
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
