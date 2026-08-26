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
import os
import struct
import sys
import tempfile
import time
import zipfile
import zlib
from contextlib import contextmanager
from pathlib import Path
from typing import Any, Callable, Iterator

import character_asset_compiler as compiler
import character_asset_probe as probe


MANAGER_SCHEMA = "mdkr-character-install-v1"
COMPILER_ID = compiler.COMPILER_ID
LOCK_NAME = ".character-import.lock"
MAX_REPORT_BYTES = 64 * 1024
MAX_UI_REVISIONS = 256


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
    return {
        "package": package,
        "package_id": package_id,
        "source_sha256": hashlib.sha256(package).hexdigest(),
        "manifest": manifest,
        "compiler_digest": compiler_digest,
        "compiled": compiled,
        "compiled_sha256": hashlib.sha256(compiled).hexdigest(),
        "compile_report": compile_report,
        "portable": embedded is not None,
    }


def inspect(package_path: Path) -> dict[str, Any]:
    """Publish exact compiled candidate facts without installing any bytes."""
    candidate = _compile_candidate(package_path)
    report = candidate["compile_report"]
    return {
        "schema": MANAGER_SCHEMA,
        "action": "inspect",
        "id": candidate["package_id"],
        "display_name": candidate["manifest"]["display_name"],
        "source_sha256": candidate["source_sha256"],
        "cache_source_digest": candidate["compiler_digest"].hex(),
        "compiled_sha256": candidate["compiled_sha256"],
        "compiler": COMPILER_ID,
        "portable": candidate["portable"],
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
    display_hex = candidate["display_name"].encode("utf-8").hex()
    rig_mode = report.get("rig_mode")
    rig_mode_value = {
        None: 0,
        "authored-clips-only": 1,
        "humanoid-retarget-v1": 2,
    }.get(rig_mode)
    if rig_mode_value is None:
        raise ManagerError("candidate compiler reported an unknown rig mode")
    fields = [
        candidate["id"], display_hex, candidate["source_sha256"],
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
        str(report["rig_roles"]), str(report["encoded_texture_bytes"]),
        str(report["decoded_texture_bytes"]),
        *(str(value) for value in report["lod_vertices"]),
        *(str(value) for value in report["lod_triangles"]),
        *(str(value) for value in report["lod_primitives"]),
    ]
    payload = (
        "mdkr-character-candidate-v1\n" + "\t".join(fields) + "\n"
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
                    if _compiler_source_digest(archive).hex() != active_digest:
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
                if manifest.get("schema") in (
                    probe.PACKAGE_SCHEMA_V3, probe.PACKAGE_SCHEMA_V4
                ) else None
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


def revise_rig(package_id: str, rig_draft_path: Path,
               directory: Path) -> dict[str, Any]:
    """Create and atomically activate a reviewed skeleton-map revision."""
    if rig_draft_path.is_symlink() or not rig_draft_path.is_file():
        raise ManagerError("rig draft must be a regular JSON file")
    if rig_draft_path.stat().st_size > 128 * 1024:
        raise ManagerError("rig draft exceeds 128 KiB")
    draft_payload = rig_draft_path.read_bytes()
    if len(draft_payload) > 128 * 1024:
        raise ManagerError("rig draft exceeds 128 KiB")
    draft = probe.json_loads_strict(
        draft_payload, "rig draft"
    )
    if not isinstance(draft, dict):
        raise ManagerError("rig draft must be an object")
    expected = {"schema", "mode", "reviewed", "roles"}
    unknown = set(draft) - expected
    missing = expected - set(draft)
    if unknown or missing or draft.get("schema") != "mdkr-character-rig-draft-v1":
        detail = []
        if unknown:
            detail.append("unknown: " + ", ".join(sorted(unknown)))
        if missing:
            detail.append("missing: " + ", ".join(sorted(missing)))
        if draft.get("schema") != "mdkr-character-rig-draft-v1":
            detail.append("unsupported schema")
        raise ManagerError("invalid rig draft (" + "; ".join(detail) + ")")
    rig = {
        "mode": draft["mode"],
        "reviewed": draft["reviewed"],
        "roles": draft["roles"],
    }

    def transform(manifest: dict[str, Any], _: dict[str, Any]) -> dict[str, Any]:
        schema = manifest.get("schema")
        if schema not in (probe.PACKAGE_SCHEMA_V3, probe.PACKAGE_SCHEMA_V4):
            raise ManagerError(
                "rig authoring requires an identity-capable source-v3/v4 package"
            )
        revised = dict(manifest)
        revised["schema"] = probe.PACKAGE_SCHEMA_V4
        revised["rig"] = rig
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
    restore_parser = sub.add_parser("restore")
    restore_parser.add_argument("id")
    restore_parser.add_argument("source_sha256")
    export_parser = sub.add_parser("export")
    export_parser.add_argument("id")
    export_parser.add_argument("source_sha256")
    export_parser.add_argument("output", type=Path)
    index_parser = sub.add_parser("write-revision-index")
    index_parser.add_argument("id")
    index_parser.add_argument("output", type=Path)
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
        help="create and install a source-v4 skeleton role-map revision",
    )
    rig_parser.add_argument("id")
    rig_parser.add_argument("draft", type=Path)
    sub.add_parser("clean")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    try:
        if args.command not in ("prepare", "inspect") and args.directory is None:
            raise ManagerError("--directory is required for this command")
        if args.command == "install":
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
        elif args.command == "remove":
            report = remove(args.id, args.directory)
        elif args.command == "revisions":
            report = list_revisions(args.id, args.directory)
        elif args.command == "restore":
            report = restore_revision(
                args.id, args.source_sha256, args.directory
            )
        elif args.command == "export":
            report = export_revision(
                args.id, args.source_sha256, args.directory, args.output
            )
        elif args.command == "write-revision-index":
            report = write_revision_index(
                args.id, args.directory, args.output
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
