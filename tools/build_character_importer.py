#!/usr/bin/env python3
"""Build and attest the self-contained Character Workshop importer.

The game never invokes an ambient Python installation in a packaged build.
Release jobs freeze the project-owned standard-library importer on each target
OS, then pass this executable and its manifest to the platform packager. The
manifest binds the exact first-party source modules, Python/PyInstaller
toolchain, target, and produced bytes without claiming cross-OS byte identity.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any, Iterable


BUILD_SCHEMA = "mdkr-character-importer-build-v1"
INFO_SCHEMA = "mdkr-character-importer-info-v1"
PYINSTALLER_VERSION = "6.22.2"
PINNED_PYTHON = (3, 13, 13)
MAX_TOOL_INFO_BYTES = 16 * 1024
# A newly signed/frozen executable can spend tens of seconds in the operating
# system's first-launch security scan on a loaded release host. Keep this
# bounded, but do not misclassify that scan as a broken importer.
TOOL_INFO_TIMEOUT_SECONDS = 120
SOURCE_MODULES = (
    "tools/character_package_manager.py",
    "tools/gltf_validator_adapter.py",
    "tools/character_asset_compiler.py",
    "tools/character_asset_probe.py",
    "tools/character_lod_builder.py",
    "tools/character_manifest_wizard.py",
    "tools/character_source_adapter.py",
    "tools/collada_to_glb.py",
)


class BuildError(ValueError):
    """A deterministic importer build-contract failure."""


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while chunk := stream.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def source_bundle_digest(root: Path,
                         members: Iterable[str] = SOURCE_MODULES) -> str:
    """Hash names, lengths, and bytes so concatenation cannot be ambiguous."""
    digest = hashlib.sha256()
    for name in members:
        try:
            encoded = name.encode("ascii")
        except UnicodeEncodeError as exc:
            raise BuildError("importer source member names must be ASCII") from exc
        path = root / name
        if path.is_symlink() or not path.is_file():
            raise BuildError(f"missing regular importer source member: {name}")
        payload = path.read_bytes()
        digest.update(len(encoded).to_bytes(4, "little"))
        digest.update(encoded)
        digest.update(len(payload).to_bytes(8, "little"))
        digest.update(payload)
    return digest.hexdigest()


def _platform_id() -> str:
    system = platform.system().lower()
    machine = platform.machine().lower()
    aliases = {"amd64": "x86_64", "x86-64": "x86_64", "aarch64": "arm64"}
    machine = aliases.get(machine, machine)
    if system not in ("darwin", "linux", "windows"):
        raise BuildError(f"unsupported importer build operating system: {system}")
    if machine not in ("x86_64", "arm64"):
        raise BuildError(f"unsupported importer build architecture: {machine}")
    return f"{system}-{machine}"


def _output_path(raw: Path) -> Path:
    output = raw.absolute()
    if output.name in ("", ".", ".."):
        raise BuildError("--output must name the importer executable")
    if output.exists() or output.is_symlink():
        raise BuildError("--output must not already exist")
    parent = output.parent
    if parent.is_symlink() or not parent.is_dir():
        raise BuildError("--output parent must be a real existing directory")
    return output


def _load_pyinstaller() -> str:
    try:
        import PyInstaller  # type: ignore[import-not-found]
    except ImportError as exc:
        raise BuildError(
            "PyInstaller is unavailable; install the exact hashed build "
            "requirements first"
        ) from exc
    version = str(PyInstaller.__version__)
    if version != PYINSTALLER_VERSION:
        raise BuildError(
            f"PyInstaller {PYINSTALLER_VERSION} is required, found {version}"
        )
    return version


def _run_tool_info(executable: Path) -> dict[str, Any]:
    try:
        completed = subprocess.run(
            [str(executable), "tool-info"],
            check=False,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=TOOL_INFO_TIMEOUT_SECONDS,
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise BuildError(f"built importer could not run tool-info: {exc}") from exc
    if (len(completed.stdout) > MAX_TOOL_INFO_BYTES or
            len(completed.stderr) > MAX_TOOL_INFO_BYTES):
        raise BuildError("built importer emitted an oversized tool-info result")
    if completed.returncode != 0:
        detail = completed.stderr.decode("utf-8", errors="replace").strip()
        raise BuildError(
            f"built importer rejected tool-info ({completed.returncode}): {detail}"
        )
    try:
        report = json.loads(completed.stdout)
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise BuildError("built importer returned malformed tool-info JSON") from exc
    if not isinstance(report, dict) or not report.get("ok"):
        raise BuildError("built importer tool-info did not report success")
    if report.get("schema") != INFO_SCHEMA or report.get("frozen") is not True:
        raise BuildError("built importer has the wrong or non-frozen contract")
    if report.get("python") != ".".join(map(str, PINNED_PYTHON)):
        raise BuildError("built importer does not contain the pinned Python runtime")
    return report


def _write_manifest(path: Path, manifest: dict[str, Any]) -> None:
    if path.exists() or path.is_symlink():
        raise BuildError("importer build manifest destination already exists")
    payload = (json.dumps(manifest, indent=2, sort_keys=True) + "\n").encode("utf-8")
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL
    descriptor = os.open(path, flags, 0o600)
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


def _copy_executable_exclusive(source: Path, destination: Path) -> None:
    """Publish one complete executable without following or replacing a path."""
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


def build(root: Path, raw_output: Path) -> dict[str, Any]:
    root = root.resolve(strict=True)
    output = _output_path(raw_output)
    if tuple(sys.version_info[:3]) != PINNED_PYTHON:
        expected = ".".join(map(str, PINNED_PYTHON))
        actual = ".".join(map(str, sys.version_info[:3]))
        raise BuildError(f"Python {expected} is required, found {actual}")
    pyinstaller_version = _load_pyinstaller()
    source_digest = source_bundle_digest(root)
    entrypoint = root / SOURCE_MODULES[0]
    tools_dir = root / "tools"
    with tempfile.TemporaryDirectory(prefix="mdkr-character-importer-build.") as raw:
        temporary = Path(raw)
        dist = temporary / "dist"
        work = temporary / "work"
        spec = temporary / "spec"
        env = os.environ.copy()
        env["PYTHONHASHSEED"] = "0"
        env["SOURCE_DATE_EPOCH"] = "946684800"
        command = [
            sys.executable, "-m", "PyInstaller",
            "--onefile", "--console", "--clean", "--noconfirm", "--noupx",
            "--log-level", "WARN", "--name", "character_importer",
            "--distpath", str(dist), "--workpath", str(work),
            "--specpath", str(spec), "--paths", str(tools_dir),
            str(entrypoint),
        ]
        completed = subprocess.run(
            command, cwd=root, env=env, check=False,
            stdin=subprocess.DEVNULL,
        )
        if completed.returncode != 0:
            raise BuildError(
                f"PyInstaller failed with exit status {completed.returncode}"
            )
        built = dist / (
            "character_importer.exe" if os.name == "nt"
            else "character_importer"
        )
        if built.is_symlink() or not built.is_file():
            raise BuildError("PyInstaller did not produce one regular executable")
        _copy_executable_exclusive(built, output)

    try:
        tool_info = _run_tool_info(output)
        manifest = {
            "schema": BUILD_SCHEMA,
            "target": _platform_id(),
            "python": ".".join(map(str, PINNED_PYTHON)),
            "pyinstaller": pyinstaller_version,
            "source_members": list(SOURCE_MODULES),
            "source_bundle_sha256": source_digest,
            "executable": output.name,
            "executable_bytes": output.stat().st_size,
            "build_executable_sha256": sha256_file(output),
            "executable_sha256": sha256_file(output),
            "manager_schema": tool_info.get("manager_schema"),
            "compiler_id": tool_info.get("compiler_id"),
        }
        _write_manifest(output.with_name(output.name + ".manifest.json"), manifest)
        return manifest
    except BaseException:
        try:
            output.unlink()
        except OSError:
            pass
        raise


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument(
        "--repo-root", type=Path,
        default=Path(__file__).resolve().parents[1],
    )
    return parser


def main(argv: list[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    try:
        report = build(args.repo_root, args.output)
    except (BuildError, OSError) as exc:
        print(f"character importer build failed: {exc}", file=sys.stderr)
        return 2
    print(json.dumps(report, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
