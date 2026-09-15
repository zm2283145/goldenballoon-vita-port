#!/usr/bin/env python3
"""Canonical, data-only handoff for third-party character source adapters.

An adapter runs in the artist's chosen DCC or external tool.  Golden Balloon
never loads or executes it.  The only accepted boundary is a deterministic
``.mdkrsource`` ZIP containing a self-contained GLB, a strict provenance
manifest, and optionally the exact license/notice bytes.

The hashes in this format provide integrity and source binding.  They are not
a signature and do not establish the adapter author's identity.
"""

from __future__ import annotations

import argparse
import hashlib
import io
import json
import unicodedata
import zipfile
from pathlib import Path
from typing import Any
from urllib.parse import urlsplit

import character_asset_probe as probe


SCHEMA = "mdkr-character-adapter-output-v1"
MEMBERS = ("adapter.json", "model.glb")
LICENSE_MEMBER = "LICENSE.txt"
MAX_MANIFEST_BYTES = 64 * 1024
MAX_NAME_BYTES = 128
MAX_VERSION_BYTES = 64
MAX_FORMAT_BYTES = 64
MAX_PROFILE_BYTES = 128
MAX_URL_BYTES = 2048
MAX_ATTRIBUTION_BYTES = 256


class AdapterOutputError(ValueError):
    """A source-adapter artifact violates the frozen handoff contract."""


def _digest(payload: bytes) -> str:
    return hashlib.sha256(payload).hexdigest()


def _digest_valid(value: object) -> bool:
    return (
        isinstance(value, str)
        and len(value) == 64
        and all(byte in "0123456789abcdef" for byte in value)
    )


def _text(value: object, maximum: int, label: str,
          *, allow_empty: bool = False) -> str:
    valid = isinstance(value, str)
    if valid:
        try:
            valid = (
                (allow_empty or bool(value.strip()))
                and unicodedata.normalize("NFC", value) == value
                and probe._bounded_printable_text(value, maximum)
            )
        except UnicodeError:
            valid = False
    if not valid:
        raise AdapterOutputError(
            f"{label} must be bounded, printable, NFC-normalized text"
        )
    assert isinstance(value, str)
    return value


def _url(value: object, label: str, *, allow_empty: bool = False) -> str:
    text = _text(value, MAX_URL_BYTES, label, allow_empty=allow_empty)
    if not text and allow_empty:
        return text
    try:
        parsed = urlsplit(text)
    except ValueError as exc:
        raise AdapterOutputError(f"{label} is not a valid URL: {exc}") from exc
    if (
        parsed.scheme not in ("https", "http")
        or not parsed.netloc
        or parsed.username is not None
        or parsed.password is not None
        or parsed.fragment
    ):
        raise AdapterOutputError(
            f"{label} must be an HTTP(S) URL without credentials or a fragment"
        )
    return text


def _exact_keys(value: object, required: set[str], optional: set[str],
                label: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise AdapterOutputError(f"{label} must be an object")
    keys = set(value)
    missing = required - keys
    unknown = keys - required - optional
    if missing:
        raise AdapterOutputError(
            f"{label} is missing: {', '.join(sorted(missing))}"
        )
    if unknown:
        raise AdapterOutputError(
            f"{label} has unsupported fields: {', '.join(sorted(unknown))}"
        )
    return value


def _read_member(archive: zipfile.ZipFile, info: zipfile.ZipInfo,
                 maximum: int, label: str) -> bytes:
    if info.file_size > maximum:
        raise AdapterOutputError(f"{label} exceeds {maximum} bytes")
    try:
        payload = archive.read(info)
    except (zipfile.BadZipFile, RuntimeError, NotImplementedError) as exc:
        raise AdapterOutputError(f"cannot read {label}: {exc}") from exc
    if len(payload) != info.file_size or len(payload) > maximum:
        raise AdapterOutputError(f"{label} expanded beyond its declared bound")
    return payload


def inspect_bytes(payload: bytes) -> dict[str, Any]:
    """Validate a complete artifact and return bounded facts plus payloads."""
    if len(payload) > probe.MAX_INPUT_BYTES:
        raise AdapterOutputError(
            f"adapter output exceeds {probe.MAX_INPUT_BYTES} bytes"
        )
    try:
        archive = zipfile.ZipFile(io.BytesIO(payload))
    except zipfile.BadZipFile as exc:
        raise AdapterOutputError(f"adapter output is not a valid ZIP: {exc}") from exc
    infos = archive.infolist()
    if archive.comment:
        raise AdapterOutputError("adapter output ZIP comments are unsupported")
    names = [info.filename for info in infos]
    if len(names) != len(set(names)):
        raise AdapterOutputError("adapter output contains duplicate members")
    expected = set(MEMBERS)
    if LICENSE_MEMBER in names:
        expected.add(LICENSE_MEMBER)
    if set(names) != expected:
        missing = expected - set(names)
        unknown = set(names) - expected
        detail = []
        if missing:
            detail.append("missing " + ", ".join(sorted(missing)))
        if unknown:
            detail.append("unexpected " + ", ".join(sorted(unknown)))
        raise AdapterOutputError(
            "adapter output must contain only its canonical members (" +
            "; ".join(detail) + ")"
        )
    try:
        probe._validate_general_archive_budget(infos, "adapter output")
    except probe.ProbeError as exc:
        raise AdapterOutputError(str(exc)) from exc
    by_name = {info.filename: info for info in infos}
    for name, info in by_name.items():
        if (
            info.is_dir()
            or info.flag_bits & 1
            or probe._zip_member_is_symlink(info)
            or bool(info.extra)
            or bool(info.comment)
            or info.compress_type not in (zipfile.ZIP_STORED, zipfile.ZIP_DEFLATED)
        ):
            raise AdapterOutputError(
                f"adapter output member {name!r} uses an unsafe or unsupported ZIP form, extra field, or comment"
            )

    manifest_payload = _read_member(
        archive, by_name["adapter.json"], MAX_MANIFEST_BYTES,
        "adapter manifest",
    )
    try:
        manifest = probe.json_loads_strict(manifest_payload, "adapter manifest")
    except (probe.ProbeError, json.JSONDecodeError) as exc:
        raise AdapterOutputError(f"invalid adapter manifest: {exc}") from exc
    root = _exact_keys(
        manifest,
        {"schema", "adapter", "source", "conversion", "model"},
        {"license"},
        "adapter manifest",
    )
    if root["schema"] != SCHEMA:
        raise AdapterOutputError(f"adapter manifest schema must be {SCHEMA!r}")

    adapter = _exact_keys(
        root["adapter"], {"name", "version"}, {"homepage"},
        "adapter identity",
    )
    adapter_name = _text(adapter["name"], MAX_NAME_BYTES, "adapter name")
    adapter_version = _text(
        adapter["version"], MAX_VERSION_BYTES, "adapter version"
    )
    adapter_homepage = (
        _url(adapter["homepage"], "adapter homepage")
        if "homepage" in adapter else ""
    )

    source = _exact_keys(
        root["source"], {"format", "sha256"}, set(), "source identity"
    )
    source_format = _text(
        source["format"], MAX_FORMAT_BYTES, "source format"
    )
    if not _digest_valid(source["sha256"]):
        raise AdapterOutputError("source sha256 must be lowercase hexadecimal")

    conversion = _exact_keys(
        root["conversion"], {"profile", "settings_sha256"}, set(),
        "conversion identity",
    )
    conversion_profile = _text(
        conversion["profile"], MAX_PROFILE_BYTES, "conversion profile"
    )
    if not _digest_valid(conversion["settings_sha256"]):
        raise AdapterOutputError(
            "conversion settings_sha256 must be lowercase hexadecimal"
        )

    model = _exact_keys(
        root["model"], {"file", "sha256"}, set(), "model identity"
    )
    if model["file"] != "model.glb":
        raise AdapterOutputError("model.file must be 'model.glb'")
    if not _digest_valid(model["sha256"]):
        raise AdapterOutputError("model sha256 must be lowercase hexadecimal")
    model_payload = _read_member(
        archive, by_name["model.glb"], probe.MAX_INPUT_BYTES, "adapter model"
    )
    if _digest(model_payload) != model["sha256"]:
        raise AdapterOutputError("adapter model digest does not match its bytes")

    license_payload: bytes | None = None
    spdx = attribution = source_url = license_sha256 = ""
    if "license" in root:
        license_info = _exact_keys(
            root["license"],
            {"file", "sha256", "spdx", "attribution", "source_url"},
            set(), "license identity",
        )
        if license_info["file"] != LICENSE_MEMBER:
            raise AdapterOutputError("license.file must be 'LICENSE.txt'")
        if LICENSE_MEMBER not in by_name:
            raise AdapterOutputError("license metadata requires LICENSE.txt")
        if not _digest_valid(license_info["sha256"]):
            raise AdapterOutputError("license sha256 must be lowercase hexadecimal")
        spdx = _text(license_info["spdx"], 128, "license SPDX")
        spdx_error = probe.validate_spdx_expression(spdx)
        if spdx_error is not None:
            raise AdapterOutputError(f"invalid license SPDX: {spdx_error}")
        attribution = _text(
            license_info["attribution"], MAX_ATTRIBUTION_BYTES,
            "license attribution",
        )
        source_url = _url(license_info["source_url"], "license source URL")
        license_payload = _read_member(
            archive, by_name[LICENSE_MEMBER], probe.MAX_LICENSE_BYTES,
            "adapter license",
        )
        if not license_payload.strip():
            raise AdapterOutputError("adapter license is empty")
        license_sha256 = _digest(license_payload)
        if license_sha256 != license_info["sha256"]:
            raise AdapterOutputError("adapter license digest does not match its bytes")
    elif LICENSE_MEMBER in by_name:
        raise AdapterOutputError("LICENSE.txt requires matching license metadata")

    return {
        "schema": SCHEMA,
        "artifact_sha256": _digest(payload),
        "artifact_bytes": len(payload),
        "adapter_name": adapter_name,
        "adapter_version": adapter_version,
        "adapter_homepage": adapter_homepage,
        "source_format": source_format,
        "source_sha256": source["sha256"],
        "conversion_profile": conversion_profile,
        "conversion_settings_sha256": conversion["settings_sha256"],
        "model_sha256": model["sha256"],
        "model_bytes": len(model_payload),
        "model_payload": model_payload,
        "license_present": license_payload is not None,
        "license_sha256": license_sha256,
        "license_payload": license_payload,
        "license_spdx": spdx,
        "license_attribution": attribution,
        "license_source_url": source_url,
    }


def inspect_path(path: Path) -> dict[str, Any]:
    if path.suffix.lower() != ".mdkrsource":
        raise AdapterOutputError("adapter output must use a .mdkrsource suffix")
    try:
        if path.is_symlink() or not path.is_file():
            raise AdapterOutputError("adapter output must be a regular file")
        if path.stat().st_size > probe.MAX_INPUT_BYTES:
            raise AdapterOutputError(
                f"adapter output exceeds {probe.MAX_INPUT_BYTES} bytes"
            )
        payload = path.read_bytes()
    except OSError as exc:
        raise AdapterOutputError(f"cannot read adapter output: {exc}") from exc
    return inspect_bytes(payload)


def canonical_manifest(*, adapter_name: str, adapter_version: str,
                       source_format: str, source_sha256: str,
                       conversion_profile: str,
                       conversion_settings_sha256: str,
                       model_payload: bytes, adapter_homepage: str = "",
                       license_payload: bytes | None = None,
                       license_spdx: str = "", attribution: str = "",
                       source_url: str = "") -> dict[str, Any]:
    """Build and self-validate the canonical manifest used by adapter authors."""
    adapter_name = _text(adapter_name, MAX_NAME_BYTES, "adapter name")
    adapter_version = _text(
        adapter_version, MAX_VERSION_BYTES, "adapter version"
    )
    source_format = _text(source_format, MAX_FORMAT_BYTES, "source format")
    conversion_profile = _text(
        conversion_profile, MAX_PROFILE_BYTES, "conversion profile"
    )
    if not _digest_valid(source_sha256):
        raise AdapterOutputError("source sha256 must be lowercase hexadecimal")
    if not _digest_valid(conversion_settings_sha256):
        raise AdapterOutputError(
            "conversion settings_sha256 must be lowercase hexadecimal"
        )
    if adapter_homepage:
        adapter_homepage = _url(adapter_homepage, "adapter homepage")
    if license_payload is not None:
        if not license_payload.strip():
            raise AdapterOutputError("adapter license is empty")
        license_spdx = _text(license_spdx, 128, "license SPDX")
        spdx_error = probe.validate_spdx_expression(license_spdx)
        if spdx_error is not None:
            raise AdapterOutputError(f"invalid license SPDX: {spdx_error}")
        attribution = _text(
            attribution, MAX_ATTRIBUTION_BYTES, "license attribution"
        )
        source_url = _url(source_url, "license source URL")
    elif license_spdx or attribution or source_url:
        raise AdapterOutputError(
            "license metadata requires exact license bytes"
        )
    manifest: dict[str, Any] = {
        "schema": SCHEMA,
        "adapter": {"name": adapter_name, "version": adapter_version},
        "source": {"format": source_format, "sha256": source_sha256},
        "conversion": {
            "profile": conversion_profile,
            "settings_sha256": conversion_settings_sha256,
        },
        "model": {"file": "model.glb", "sha256": _digest(model_payload)},
    }
    if adapter_homepage:
        manifest["adapter"]["homepage"] = adapter_homepage
    if license_payload is not None:
        manifest["license"] = {
            "file": LICENSE_MEMBER,
            "sha256": _digest(license_payload),
            "spdx": license_spdx,
            "attribution": attribution,
            "source_url": source_url,
        }
    return manifest


def pack(*, model_path: Path, output_path: Path, adapter_name: str,
         adapter_version: str, source_format: str, source_sha256: str,
         conversion_profile: str, conversion_settings_sha256: str,
         adapter_homepage: str = "", license_path: Path | None = None,
         license_spdx: str = "", attribution: str = "",
         source_url: str = "") -> dict[str, Any]:
    """Create a deterministic artifact without overwriting an existing path."""
    if output_path.suffix.lower() != ".mdkrsource":
        raise AdapterOutputError("output must use a .mdkrsource suffix")
    if output_path.exists() or output_path.is_symlink():
        raise AdapterOutputError("output already exists")
    try:
        model_payload = probe._read_bounded(
            model_path, probe.MAX_INPUT_BYTES, "adapter GLB"
        )
        license_payload = (
            probe._read_bounded(
                license_path, probe.MAX_LICENSE_BYTES, "adapter license"
            ) if license_path is not None else None
        )
    except (OSError, probe.ProbeError) as exc:
        raise AdapterOutputError(str(exc)) from exc
    try:
        report = probe.inspect_glb_bytes(model_payload, require_character=True)
    except probe.ProbeError as exc:
        raise AdapterOutputError(f"adapter GLB is invalid: {exc}") from exc
    if report["errors"]:
        raise AdapterOutputError(
            "adapter GLB is not character-ready: " + "; ".join(report["errors"])
        )
    manifest = canonical_manifest(
        adapter_name=adapter_name, adapter_version=adapter_version,
        adapter_homepage=adapter_homepage, source_format=source_format,
        source_sha256=source_sha256, model_payload=model_payload,
        conversion_profile=conversion_profile,
        conversion_settings_sha256=conversion_settings_sha256,
        license_payload=license_payload, license_spdx=license_spdx,
        attribution=attribution, source_url=source_url,
    )
    manifest_payload = (
        json.dumps(manifest, ensure_ascii=False, sort_keys=True,
                   separators=(",", ":")) + "\n"
    ).encode("utf-8")
    output_path.parent.mkdir(parents=True, exist_ok=True)
    try:
        with output_path.open("xb") as stream:
            with zipfile.ZipFile(stream, "w", allowZip64=False) as archive:
                members = {
                    "adapter.json": manifest_payload,
                    "model.glb": model_payload,
                }
                if license_payload is not None:
                    members[LICENSE_MEMBER] = license_payload
                for name in (*MEMBERS, *((LICENSE_MEMBER,)
                                         if license_payload is not None else ())):
                    info, member_payload = probe._zip_entry(name, members[name])
                    archive.writestr(info, member_payload)
    except FileExistsError as exc:
        raise AdapterOutputError("output already exists") from exc
    except Exception:
        if output_path.is_file() and not output_path.is_symlink():
            output_path.unlink()
        raise
    try:
        inspected = inspect_path(output_path)
    except Exception:
        if output_path.is_file() and not output_path.is_symlink():
            output_path.unlink()
        raise
    return {key: value for key, value in inspected.items()
            if not key.endswith("_payload")}


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    inspect_parser = sub.add_parser(
        "inspect", help="validate and report a data-only adapter artifact"
    )
    inspect_parser.add_argument("input", type=Path)
    pack_parser = sub.add_parser(
        "pack", help="create a deterministic adapter artifact without overwrite"
    )
    pack_parser.add_argument("model", type=Path)
    pack_parser.add_argument("output", type=Path)
    pack_parser.add_argument("--adapter-name", required=True)
    pack_parser.add_argument("--adapter-version", required=True)
    pack_parser.add_argument("--adapter-homepage", default="")
    pack_parser.add_argument("--source-format", required=True)
    pack_parser.add_argument("--source-sha256", required=True)
    pack_parser.add_argument("--conversion-profile", required=True)
    pack_parser.add_argument("--conversion-settings-sha256", required=True)
    pack_parser.add_argument("--license", type=Path)
    pack_parser.add_argument("--license-spdx", default="")
    pack_parser.add_argument("--attribution", default="")
    pack_parser.add_argument("--source-url", default="")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    try:
        if args.command == "inspect":
            report = inspect_path(args.input)
            report = {key: value for key, value in report.items()
                      if not key.endswith("_payload")}
        else:
            rights = (
                args.license is not None,
                bool(args.license_spdx), bool(args.attribution),
                bool(args.source_url),
            )
            if any(rights) and not all(rights):
                raise AdapterOutputError(
                    "--license, --license-spdx, --attribution, and "
                    "--source-url must be supplied together"
                )
            report = pack(
                model_path=args.model, output_path=args.output,
                adapter_name=args.adapter_name,
                adapter_version=args.adapter_version,
                adapter_homepage=args.adapter_homepage,
                source_format=args.source_format,
                source_sha256=args.source_sha256,
                conversion_profile=args.conversion_profile,
                conversion_settings_sha256=args.conversion_settings_sha256,
                license_path=args.license,
                license_spdx=args.license_spdx,
                attribution=args.attribution,
                source_url=args.source_url,
            )
        print(json.dumps({"ok": True, **report}, ensure_ascii=False,
                         indent=2, sort_keys=True))
        return 0
    except (OSError, probe.ProbeError, AdapterOutputError) as exc:
        print(json.dumps({"ok": False, "error": str(exc)},
                         ensure_ascii=False, sort_keys=True))
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
