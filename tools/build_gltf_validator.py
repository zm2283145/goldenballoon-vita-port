#!/usr/bin/env python3
"""Prepare the exact native Khronos glTF Validator shipped with the Workshop."""

from __future__ import annotations

import argparse
import json
import os
import shutil
import stat
import subprocess
import sys
import tarfile
import zipfile
from pathlib import Path, PurePosixPath
from typing import Any

import gltf_validator_adapter as adapter


MAX_ARCHIVE_MEMBERS = 20_000
MAX_ARCHIVE_EXPANDED_BYTES = 1024 * 1024 * 1024
MAX_VALIDATOR_BYTES = 32 * 1024 * 1024
MAX_VERSION_OUTPUT_BYTES = 64 * 1024
MAC_BUILD_ROOT = Path(
    f"/private/tmp/mdkr-gltf-validator-{adapter.VALIDATOR_VERSION}-arm64-build"
)


class BuildError(ValueError):
    """A validator archive, toolchain, or deterministic build failed."""


def _safe_member_path(name: str) -> PurePosixPath:
    if not name or "\\" in name:
        raise BuildError("validator archive has an empty or backslash path")
    path = PurePosixPath(name)
    if path.is_absolute() or any(part in ("", ".", "..") for part in path.parts):
        raise BuildError(f"validator archive has an unsafe path: {name}")
    if path.parts[0].endswith(":"):
        raise BuildError(f"validator archive has a drive-qualified path: {name}")
    return path


def _write_archive_file(destination: Path, payload: bytes, mode: int) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    if destination.exists() or destination.is_symlink():
        raise BuildError(f"duplicate validator archive member: {destination.name}")
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL
    descriptor = os.open(destination, flags, mode)
    try:
        with os.fdopen(descriptor, "wb") as stream:
            stream.write(payload)
    except BaseException:
        try:
            destination.unlink()
        except OSError:
            pass
        raise


def _extract_tar(archive_path: Path, destination: Path) -> list[str]:
    names: list[str] = []
    expanded = 0
    with tarfile.open(archive_path, "r:*") as archive:
        members = archive.getmembers()
        if len(members) > MAX_ARCHIVE_MEMBERS:
            raise BuildError("validator archive has too many members")
        for member in members:
            relative = _safe_member_path(member.name.rstrip("/"))
            names.append(member.name)
            target = destination.joinpath(*relative.parts)
            if member.isdir():
                target.mkdir(parents=True, exist_ok=True)
                continue
            if not member.isfile() or member.issym() or member.islnk():
                raise BuildError(f"validator archive member is not regular: {member.name}")
            expanded += member.size
            if expanded > MAX_ARCHIVE_EXPANDED_BYTES:
                raise BuildError("validator archive expands beyond its bound")
            stream = archive.extractfile(member)
            if stream is None:
                raise BuildError(f"validator archive member cannot be read: {member.name}")
            payload = stream.read(member.size + 1)
            if len(payload) != member.size:
                raise BuildError(f"validator archive member size changed: {member.name}")
            _write_archive_file(target, payload, member.mode & 0o777 or 0o600)
    return names


def _extract_zip(archive_path: Path, destination: Path) -> list[str]:
    names: list[str] = []
    expanded = 0
    with zipfile.ZipFile(archive_path) as archive:
        members = archive.infolist()
        if len(members) > MAX_ARCHIVE_MEMBERS:
            raise BuildError("validator archive has too many members")
        for member in members:
            relative = _safe_member_path(member.filename.rstrip("/"))
            names.append(member.filename)
            target = destination.joinpath(*relative.parts)
            mode = member.external_attr >> 16
            if stat.S_ISLNK(mode):
                raise BuildError(f"validator archive member is linked: {member.filename}")
            if member.is_dir():
                target.mkdir(parents=True, exist_ok=True)
                continue
            if member.compress_type not in (zipfile.ZIP_STORED, zipfile.ZIP_DEFLATED):
                raise BuildError(
                    f"validator archive compression is unsupported: {member.filename}"
                )
            expanded += member.file_size
            if expanded > MAX_ARCHIVE_EXPANDED_BYTES:
                raise BuildError("validator archive expands beyond its bound")
            payload = archive.read(member)
            if len(payload) != member.file_size:
                raise BuildError(f"validator archive member size changed: {member.filename}")
            permissions = mode & 0o777
            _write_archive_file(target, payload, permissions or 0o600)
    return names


def _exclusive_output(raw: Path) -> Path:
    output = raw.absolute()
    if output.name not in ("gltf_validator", "gltf_validator.exe"):
        raise BuildError("--output must use the canonical validator filename")
    if output.exists() or output.is_symlink():
        raise BuildError("validator output must not already exist")
    if output.parent.is_symlink() or not output.parent.is_dir():
        raise BuildError("validator output parent must be a real directory")
    return output


def _copy_exclusive(source: Path, destination: Path) -> None:
    if source.stat().st_size > MAX_VALIDATOR_BYTES:
        raise BuildError("validator executable exceeds its build bound")
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL
    descriptor = os.open(destination, flags, 0o700)
    try:
        with source.open("rb") as input_stream:
            with os.fdopen(descriptor, "wb") as output_stream:
                while chunk := input_stream.read(1024 * 1024):
                    output_stream.write(chunk)
                output_stream.flush()
                os.fsync(output_stream.fileno())
        destination.chmod(0o755)
    except BaseException:
        try:
            os.close(descriptor)
        except OSError:
            pass
        try:
            destination.unlink()
        except OSError:
            pass
        raise


def _check_version(executable: Path) -> None:
    try:
        completed = subprocess.run(
            [str(executable), "--version"], check=False,
            stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, timeout=30,
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise BuildError(f"built validator could not report its version: {exc}") from exc
    output = completed.stdout + completed.stderr
    if len(output) > MAX_VERSION_OUTPUT_BYTES:
        raise BuildError("validator version output exceeds its bound")
    expected = f"glTF 2.0 Validator, version {adapter.VALIDATOR_VERSION}".encode("ascii")
    # Upstream has no dedicated version flag: an unknown/no-input invocation
    # prints the version banner and usage, then exits 1. Pin both behaviours.
    if completed.returncode != 1 or not output.startswith(expected):
        raise BuildError("validator executable reports the wrong version")


def _manifest(target: str, output: Path, distribution: str,
              archive_sha256: str | None) -> dict[str, Any]:
    source_build = distribution == "pinned-source-build"
    return {
        "schema": adapter.MANIFEST_SCHEMA,
        "version": adapter.VALIDATOR_VERSION,
        "commit": adapter.VALIDATOR_COMMIT,
        "target": target,
        "distribution": distribution,
        "archive_sha256": archive_sha256,
        "source_archive_sha256": adapter.SOURCE_ARCHIVE_SHA256 if source_build else None,
        "dart_version": adapter.DART_VERSION if source_build else None,
        "dart_sdk_archive_sha256": (
            adapter.DART_MACOS_ARM64_ARCHIVE_SHA256 if source_build else None
        ),
        "pubspec_lock_sha256": adapter.PUBSPEC_LOCK_SHA256 if source_build else None,
        "executable": output.name,
        "executable_bytes": output.stat().st_size,
        "build_executable_sha256": adapter.sha256_file(output),
        "executable_sha256": adapter.sha256_file(output),
    }


def _write_manifest(output: Path, report: dict[str, Any]) -> Path:
    path = output.with_name(output.name + ".manifest.json")
    payload = (json.dumps(report, indent=2, sort_keys=True) + "\n").encode("utf-8")
    descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    try:
        with os.fdopen(descriptor, "wb") as stream:
            stream.write(payload)
            stream.flush()
            os.fsync(stream.fileno())
    except BaseException:
        try:
            path.unlink()
        except OSError:
            pass
        raise
    return path


def _build_official(archive_path: Path, target: str, output: Path) -> dict[str, Any]:
    expected_archive = adapter.OFFICIAL_ARCHIVE_SHA256.get(target)
    if expected_archive is None:
        raise BuildError("this target has no official native validator archive")
    if archive_path.is_symlink() or not archive_path.is_file():
        raise BuildError("official validator archive must be a regular file")
    if adapter.sha256_file(archive_path) != expected_archive:
        raise BuildError("official validator archive hash mismatch")
    with tempfile.TemporaryDirectory(prefix="mdkr-validator-release.") as raw:
        temporary = Path(raw)
        names = (
            _extract_zip(archive_path, temporary)
            if target.startswith("windows-")
            else _extract_tar(archive_path, temporary)
        )
        executable_name = "gltf_validator.exe" if target.startswith("windows-") else "gltf_validator"
        expected_names = {
            "docs/", "docs/config-example.yaml", "docs/validation.schema.json",
            executable_name, "LICENSE", "NOTICES",
        }
        if set(names) != expected_names:
            raise BuildError("official validator archive member list changed")
        source = temporary / executable_name
        if adapter.sha256_file(source) != adapter.PINNED_BUILD_SHA256[target]:
            raise BuildError("official validator executable hash mismatch")
        _copy_exclusive(source, output)
    return _manifest(target, output, "official-release", expected_archive)


def _build_macos_arm64(source_archive: Path, dart_archive: Path,
                       lock_path: Path, output: Path) -> dict[str, Any]:
    if source_archive.is_symlink() or not source_archive.is_file():
        raise BuildError("validator source archive must be a regular file")
    if dart_archive.is_symlink() or not dart_archive.is_file():
        raise BuildError("Dart SDK archive must be a regular file")
    if adapter.sha256_file(source_archive) != adapter.SOURCE_ARCHIVE_SHA256:
        raise BuildError("validator source archive hash mismatch")
    if adapter.sha256_file(dart_archive) != adapter.DART_MACOS_ARM64_ARCHIVE_SHA256:
        raise BuildError("Dart SDK archive hash mismatch")
    if lock_path.is_symlink() or not lock_path.is_file():
        raise BuildError("validator pubspec lock must be a regular file")
    if adapter.sha256_file(lock_path) != adapter.PUBSPEC_LOCK_SHA256:
        raise BuildError("validator pubspec lock hash mismatch")
    if MAC_BUILD_ROOT.exists() or MAC_BUILD_ROOT.is_symlink():
        raise BuildError(f"canonical validator build root already exists: {MAC_BUILD_ROOT}")
    source_root = MAC_BUILD_ROOT / "source"
    sdk_root = MAC_BUILD_ROOT / "sdk"
    cache_root = MAC_BUILD_ROOT / "pub-cache"
    built = MAC_BUILD_ROOT / "out" / "gltf_validator"
    created_root = False
    try:
        MAC_BUILD_ROOT.mkdir(mode=0o700)
        created_root = True
        source_root.mkdir()
        sdk_root.mkdir()
        cache_root.mkdir()
        built.parent.mkdir()
        source_names = _extract_tar(source_archive, source_root)
        prefixes = {PurePosixPath(name.rstrip("/")).parts[0] for name in source_names}
        if len(prefixes) != 1:
            raise BuildError("validator source archive has an ambiguous root")
        extracted_source = source_root / next(iter(prefixes))
        _extract_zip(dart_archive, sdk_root)
        dart = sdk_root / "dart-sdk" / "bin" / "dart"
        if not dart.is_file():
            raise BuildError("Dart SDK archive has no compiler executable")
        dart.chmod(dart.stat().st_mode | stat.S_IXUSR)
        shutil.copyfile(lock_path, extracted_source / "pubspec.lock")
        environment = os.environ.copy()
        environment["PUB_CACHE"] = str(cache_root)
        get = subprocess.run(
            [str(dart), "pub", "get", "--enforce-lockfile"],
            cwd=extracted_source, env=environment, check=False,
            stdin=subprocess.DEVNULL,
        )
        if get.returncode != 0:
            raise BuildError(f"locked Dart dependency fetch exited {get.returncode}")
        compile_result = subprocess.run(
            [str(dart), "compile", "exe", "bin/gltf_validator.dart", "-o", str(built)],
            cwd=extracted_source, env=environment, check=False,
            stdin=subprocess.DEVNULL,
        )
        if compile_result.returncode != 0:
            raise BuildError(f"Dart validator compile exited {compile_result.returncode}")
        actual_build_sha = adapter.sha256_file(built)
        expected_build_sha = adapter.PINNED_BUILD_SHA256["darwin-arm64"]
        if actual_build_sha != expected_build_sha:
            raise BuildError(
                "arm64 validator build is not reproducible "
                f"(expected {expected_build_sha}, found {actual_build_sha})"
            )
        _copy_exclusive(built, output)
    finally:
        if created_root and MAC_BUILD_ROOT == Path(
                f"/private/tmp/mdkr-gltf-validator-{adapter.VALIDATOR_VERSION}-arm64-build"):
            shutil.rmtree(MAC_BUILD_ROOT, ignore_errors=True)
    return _manifest("darwin-arm64", output, "pinned-source-build", None)


def build(output_path: Path, *, release_archive: Path | None,
          source_archive: Path | None, dart_sdk_archive: Path | None,
          repo_root: Path) -> dict[str, Any]:
    output = _exclusive_output(output_path)
    target = adapter.target_id()
    lock_path = repo_root.resolve(strict=True) / "third_party/gltf_validator/pubspec.lock"
    try:
        if target == "darwin-arm64":
            if release_archive is not None or source_archive is None or dart_sdk_archive is None:
                raise BuildError(
                    "darwin-arm64 requires --source-archive and --dart-sdk-archive"
                )
            report = _build_macos_arm64(
                source_archive, dart_sdk_archive, lock_path, output,
            )
        else:
            if release_archive is None or source_archive is not None or dart_sdk_archive is not None:
                raise BuildError("official targets require only --release-archive")
            report = _build_official(release_archive, target, output)
        _check_version(output)
        manifest_path = _write_manifest(output, report)
        adapter.verify_installation(output, manifest_path, target)
        return report
    except BaseException:
        try:
            output.unlink()
        except OSError:
            pass
        try:
            output.with_name(output.name + ".manifest.json").unlink()
        except OSError:
            pass
        raise


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--release-archive", type=Path)
    parser.add_argument("--source-archive", type=Path)
    parser.add_argument("--dart-sdk-archive", type=Path)
    parser.add_argument(
        "--repo-root", type=Path,
        default=Path(__file__).resolve().parents[1],
    )
    return parser


def main(argv: list[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    try:
        report = build(
            args.output, release_archive=args.release_archive,
            source_archive=args.source_archive,
            dart_sdk_archive=args.dart_sdk_archive,
            repo_root=args.repo_root,
        )
    except (BuildError, OSError, adapter.ValidatorError,
            tarfile.TarError, zipfile.BadZipFile) as exc:
        print(f"glTF Validator build failed: {exc}", file=sys.stderr)
        return 2
    print(json.dumps(report, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
