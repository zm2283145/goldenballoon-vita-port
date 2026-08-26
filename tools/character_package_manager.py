#!/usr/bin/env python3
"""Install and manage private .mdkrchar packages and disposable MDKC caches.

The source package is retained for provenance. `<id>.mdkc` is the sole active
runtime record and is replaced atomically only after validation and compilation
complete, so an interrupted import cannot publish a partial character.
"""

from __future__ import annotations

import argparse
import hashlib
import io
import json
import os
import struct
import sys
import tempfile
import time
import zipfile
import zlib
from contextlib import contextmanager
from pathlib import Path
from typing import Any, Iterator

import character_asset_compiler as compiler
import character_asset_probe as probe


MANAGER_SCHEMA = "mdkr-character-install-v1"
COMPILER_ID = compiler.COMPILER_ID
LOCK_NAME = ".character-import.lock"


class ManagerError(ValueError):
    pass


def _compiler_source_digest(archive: zipfile.ZipFile) -> bytes:
    manifest = probe.json_loads_strict(archive.read("manifest.json"), "manifest")
    return compiler.source_digest(
        (name, archive.read(name))
        for name in probe.package_members_for_schema(manifest.get("schema"))
    )


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


def _active_cache_digest(root: Path, package_id: str) -> str:
    cache_path = root / f"{package_id}.mdkc"
    try:
        cache = cache_path.read_bytes()
    except FileNotFoundError as exc:
        raise ManagerError(f"character {package_id!r} is not installed") from exc
    if not _cache_valid(cache):
        raise ManagerError(f"installed cache for {package_id!r} is invalid")
    return cache[20:52].hex()


def install(package_path: Path, directory: Path, *,
            expected_active_digest: str | None = None) -> dict[str, Any]:
    root = _prepare_directory(directory)
    package = package_path.read_bytes()
    verification = probe.verify_package(package_path)
    if not verification["valid"]:
        raise ManagerError("invalid source package: " + "; ".join(verification["errors"]))
    package_id = verification["id"]
    if not isinstance(package_id, str) or probe.ID_RE.fullmatch(package_id) is None:
        raise ManagerError("validated package has an unsafe id")
    source_sha = hashlib.sha256(package).hexdigest()
    with zipfile.ZipFile(package_path) as archive:
        manifest = probe.json_loads_strict(
            archive.read("manifest.json"), "manifest"
        )
        model = archive.read("model.glb")
        portrait = (
            archive.read("portrait.png")
            if manifest.get("schema") in (
                probe.PACKAGE_SCHEMA_V3, probe.PACKAGE_SCHEMA_V4
            ) else None
        )
        compiler_digest = _compiler_source_digest(archive)
        embedded = archive.read("compiled.mdkc") if verification.get("portable") else None
    compiled, compile_report = compiler.compile_character(
        model, manifest, compiler_digest, portrait
    )
    if embedded is not None and embedded != compiled:
        raise ManagerError(
            "portable package cache does not match its source model and manifest"
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
        if (expected_active_digest is not None and
                _active_cache_digest(root, package_id) != expected_active_digest):
            raise ManagerError(
                "the installed character changed while this revision was being built; "
                "review the latest revision and try again"
            )
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


def _active_source_snapshot(package_id: str, root: Path) -> tuple[bytes, str, str]:
    """Read the exact source behind the active cache under the import lock."""
    with _locked(root):
        active_digest = _active_cache_digest(root, package_id)
        candidates: list[tuple[bool, str, bytes]] = []
        for report_path in sorted(root.glob(f"{package_id}.*.json")):
            try:
                report = json.loads(report_path.read_text(encoding="utf-8"))
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
                    if _compiler_source_digest(archive).hex() != active_digest:
                        continue
                    portable = "compiled.mdkc" in archive.namelist()
                candidates.append((portable, source_sha, package))
            except (OSError, KeyError, TypeError, ValueError,
                    zipfile.BadZipFile, json.JSONDecodeError, probe.ProbeError):
                continue
        if not candidates:
            raise ManagerError(
                f"active source provenance for {package_id!r} is missing or invalid"
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
    elif original_schema not in (
        probe.PACKAGE_SCHEMA, probe.PACKAGE_SCHEMA_V3, probe.PACKAGE_SCHEMA_V4
    ):
        raise ManagerError("installed package uses an unsupported source schema")
    upgraded["schema"] = (
        probe.PACKAGE_SCHEMA_V4
        if original_schema == probe.PACKAGE_SCHEMA_V4
        else probe.PACKAGE_SCHEMA_V3
    )
    upgraded["identity"] = {
        "portrait_file": "portrait.png",
        # build_package replaces this placeholder with the canonical digest.
        "portrait_sha256": "0" * 64,
        "minimap_rgb": list(minimap_rgb),
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


def revise_profile(package_id: str, donor: str, vehicles: tuple[str, ...],
                   directory: Path) -> dict[str, Any]:
    """Create and atomically activate a donor/vehicle compatibility revision."""
    if probe.ID_RE.fullmatch(package_id) is None:
        raise ManagerError("invalid package id")
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
                if manifest.get("schema") in (
                    probe.PACKAGE_SCHEMA_V3, probe.PACKAGE_SCHEMA_V4
                ) else None
            )
        if not isinstance(manifest, dict):
            raise ManagerError("active source manifest is not an object")
        revised = dict(manifest)
        revised["gameplay"] = {
            "donor": donor,
            "vehicles": list(vehicles),
        }
        if revised.get("schema") in (
            probe.PACKAGE_SCHEMA, probe.PACKAGE_SCHEMA_V3,
            probe.PACKAGE_SCHEMA_V4,
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
    return {
        **installed,
        "action": "revise-profile",
        "based_on_source_sha256": based_on_sha,
        "donor": donor,
        "vehicles": list(vehicles),
    }


def list_installed(directory: Path) -> dict[str, Any]:
    root = _prepare_directory(directory)
    entries: list[dict[str, Any]] = []
    for report_path in sorted(root.glob("*.json")):
        try:
            report = json.loads(report_path.read_text(encoding="utf-8"))
            if report.get("schema") != MANAGER_SCHEMA:
                continue
            package_id = report["id"]
            source_sha = report.get("source_sha256", "")
            if report_path.name != f"{package_id}.{source_sha}.json":
                continue
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
                    "source_sha256": source_sha,
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


def prepare(package_path: Path, output_path: Path) -> dict[str, Any]:
    verification = probe.verify_package(package_path)
    if not verification["valid"]:
        raise ManagerError("invalid source package: " + "; ".join(verification["errors"]))
    with zipfile.ZipFile(package_path) as archive:
        manifest = probe.json_loads_strict(archive.read("manifest.json"), "manifest")
        model = archive.read("model.glb")
        portrait = (
            archive.read("portrait.png")
            if manifest.get("schema") in (
                probe.PACKAGE_SCHEMA_V3, probe.PACKAGE_SCHEMA_V4
            ) else None
        )
        digest = _compiler_source_digest(archive)
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
    with _locked(root):
        candidates = [root / f"{package_id}.mdkc"]
        candidates.extend(sorted(root.glob(f"{package_id}.*.mdkrchar")))
        candidates.extend(sorted(root.glob(f"{package_id}.*.json")))
        for path in candidates:
            if path.parent != root or not path.is_file() or path.is_symlink():
                continue
            if path.name != f"{package_id}.mdkc" and not (
                _owned_provenance_name(path.name, package_id, ".mdkrchar") or
                _owned_provenance_name(path.name, package_id, ".json")
            ):
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
    parser.add_argument("--directory", type=Path)
    parser.add_argument(
        "--result-file",
        type=Path,
        help="write the same bounded JSON result for a native launcher caller",
    )
    sub = parser.add_subparsers(dest="command", required=True)
    install_parser = sub.add_parser("install")
    install_parser.add_argument("package", type=Path)
    sub.add_parser("list")
    remove_parser = sub.add_parser("remove")
    remove_parser.add_argument("id")
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
    sub.add_parser("clean")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    try:
        if args.command != "prepare" and args.directory is None:
            raise ManagerError("--directory is required for this command")
        if args.command == "install":
            report = install(args.package, args.directory)
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
        elif args.command == "remove":
            report = remove(args.id, args.directory)
        elif args.command == "clean":
            report = clean(args.directory)
        else:
            report = list_installed(args.directory)
        report = {"ok": True, **report}
        status = 0
    except (OSError, zipfile.BadZipFile, json.JSONDecodeError, probe.ProbeError,
            compiler.CompileError, ManagerError) as exc:
        report = {"ok": False, "error": str(exc)}
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
