#!/usr/bin/env python3
"""Inspect and package user-supplied modern character assets.

The stable input contract is a self-contained glTF 2.0 binary (GLB); this tool
adds MDKR policy checks and builds a deterministic, data-only source package
around that GLB. A bounded, dependency-free COLLADA convenience adapter lives
in collada_to_glb.py. Richer DAE/FBX authoring still belongs in Blender or an
equivalent DCC exporter, with GLB as the handoff boundary.
"""

from __future__ import annotations

import argparse
import hashlib
import io
import json
import math
import re
import struct
import sys
import unicodedata
import zipfile
import zlib
from pathlib import Path, PurePosixPath
from typing import Any
from xml.etree import ElementTree


GLB_MAGIC = b"glTF"
GLB_JSON_CHUNK = 0x4E4F534A
GLB_BIN_CHUNK = 0x004E4942
MAX_INPUT_BYTES = 512 * 1024 * 1024
MAX_MANIFEST_BYTES = 1024 * 1024
MAX_LICENSE_BYTES = 1024 * 1024
MAX_PORTRAIT_BYTES = 8 * 1024 * 1024
MAX_ARCHIVE_MEMBERS = 4096
MAX_NESTED_ARCHIVE_BYTES = 64 * 1024 * 1024
MAX_ARCHIVE_DEPTH = 2
MAX_ARCHIVE_EXPANSION_RATIO = 200
ARCHIVE_EXPANSION_SLACK_BYTES = 1024 * 1024
MAX_JOINTS = 256
MAX_VERTICES = 1_000_000
MAX_TRIANGLES = 2_000_000
MAX_MATERIALS = 256
MAX_TEXTURE_DIMENSION = 4096
MAX_DECODED_TEXTURE_BYTES = 512 * 1024 * 1024
MAX_GLTF_DIAGNOSTICS = 256
PACKAGE_SCHEMA_V1 = "mdkr-character-source-v1"
PACKAGE_SCHEMA = "mdkr-character-source-v2"
PACKAGE_SCHEMA_V3 = "mdkr-character-source-v3"
PACKAGE_SCHEMA_V4 = "mdkr-character-source-v4"
PACKAGE_SCHEMAS = {
    PACKAGE_SCHEMA_V1, PACKAGE_SCHEMA, PACKAGE_SCHEMA_V3, PACKAGE_SCHEMA_V4,
}
PACKAGE_MEMBERS = ("manifest.json", "model.glb", "LICENSE.txt")
PORTABLE_PACKAGE_MEMBERS = PACKAGE_MEMBERS + ("compiled.mdkc",)
PACKAGE_MEMBERS_V3 = (
    "manifest.json", "model.glb", "portrait.png", "LICENSE.txt"
)
PORTABLE_PACKAGE_MEMBERS_V3 = PACKAGE_MEMBERS_V3 + ("compiled.mdkc",)
PACKAGE_EPOCH = (1980, 1, 1, 0, 0, 0)
MODEL_SUFFIXES = {".glb", ".gltf", ".dae", ".fbx", ".obj"}
LICENSE_NAMES = {
    "license",
    "license.txt",
    "license.md",
    "copying",
    "copying.txt",
    "copyright",
    "copyright.txt",
}
SUPPORTED_REQUIRED_EXTENSIONS = {
    "MSFT_lod",
}
GAMEPLAY_DONORS = {
    "banjo", "bumper", "conker", "diddy", "drumstick",
    "krunch", "pipsy", "timber", "tiptup", "tt",
}
VEHICLE_NAMES = {"car", "hovercraft", "plane"}
PRESENTATION_CONTEXTS = {"select", *VEHICLE_NAMES}
SOURCE_FORWARD_AXES = {"+z", "-z", "+x", "-x"}
# Explicit manifest token for a compiler-owned, bind-pose fallback animation.
# It lets a valid skinned model enter Rig Studio without asking the artist to
# add a meaningless source clip. It is not an engine semantic or a GLB name.
BIND_POSE_FALLBACK = "$bind"
RECOMMENDED_RACE_SEMANTICS = (
    "race.steer", "race.reverse", "race.boost", "race.damage", "race.item",
    "race.spin", "race.airborne", "race.land", "race.finish_win",
    "race.finish_lose",
)
RECOMMENDED_SELECT_SEMANTICS = (
    "select.idle", "select.hover", "select.confirm",
)
DISABLEABLE_SEMANTICS = frozenset(
    (*RECOMMENDED_RACE_SEMANTICS, *RECOMMENDED_SELECT_SEMANTICS)
)
REQUIRED_PRESENTATION_SOCKETS = {"seat", "head"}
RIG_MODES = {"authored-clips-only", "humanoid-retarget-v1"}
HUMANOID_ROLES = (
    "hips", "spine", "chest", "head",
    "upper_arm.left", "lower_arm.left", "hand.left",
    "upper_arm.right", "lower_arm.right", "hand.right",
    "upper_leg.left", "lower_leg.left", "foot.left",
    "upper_leg.right", "lower_leg.right", "foot.right",
)
HUMANOID_ROLE_SET = set(HUMANOID_ROLES)
ID_RE = re.compile(r"^[a-z0-9][a-z0-9._-]{1,63}$")
SEMANTIC_RE = re.compile(r"^[a-z][a-z0-9_.-]{0,63}$")
PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"
SPDX_IDENTIFIER_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9.-]*$")
SPDX_REFERENCE_NAME_RE = re.compile(r"^[A-Za-z0-9.-]+$")
GLTF_COMPONENT_BYTES = {
    5120: 1, 5121: 1, 5122: 2, 5123: 2, 5125: 4, 5126: 4,
}
GLTF_COMPONENT_FORMAT = {
    5120: "b", 5121: "B", 5122: "h", 5123: "H", 5125: "I", 5126: "f",
}
GLTF_TYPE_COMPONENTS = {
    "SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4,
    "MAT2": 4, "MAT3": 9, "MAT4": 16,
}


class ProbeError(ValueError):
    """A bounded, user-facing asset validation failure."""


class _BoundedDiagnostics(list[str]):
    """Keep hostile documents from turning diagnostics into an allocation bomb."""

    def __init__(self) -> None:
        super().__init__()
        self.suppressed = 0

    def append(self, value: str) -> None:
        if len(self) < MAX_GLTF_DIAGNOSTICS:
            super().append(value)
        else:
            self.suppressed += 1

    def finish(self) -> None:
        if self.suppressed:
            super().append(
                f"{self.suppressed} additional GLB diagnostics were suppressed"
            )


def _spdx_reference_name(text: str) -> bool:
    return SPDX_REFERENCE_NAME_RE.fullmatch(text) is not None


def _spdx_ordinary_identifier(text: str) -> bool:
    return (
        _spdx_reference_name(text)
        and SPDX_IDENTIFIER_RE.fullmatch(text) is not None
        and text not in {
            "AND", "OR", "WITH", "and", "or", "with",
            "NONE", "NOASSERTION",
        }
    )


def _spdx_license_identifier(text: str) -> bool:
    license_prefix = "LicenseRef-"
    addition_prefix = "AdditionRef-"
    document_prefix = "DocumentRef-"
    if text.startswith(license_prefix):
        return _spdx_reference_name(text[len(license_prefix):])
    if text.startswith(document_prefix):
        document, separator, license_ref = text.partition(":")
        return (
            bool(separator)
            and ":" not in license_ref
            and _spdx_reference_name(document[len(document_prefix):])
            and license_ref.startswith(license_prefix)
            and _spdx_reference_name(license_ref[len(license_prefix):])
        )
    if text.startswith(addition_prefix):
        return False
    return _spdx_ordinary_identifier(text)


def _spdx_addition_identifier(text: str) -> bool:
    addition_prefix = "AdditionRef-"
    document_prefix = "DocumentRef-"
    if text.startswith(addition_prefix):
        return _spdx_reference_name(text[len(addition_prefix):])
    if text.startswith(document_prefix):
        document, separator, addition_ref = text.partition(":")
        return (
            bool(separator)
            and ":" not in addition_ref
            and _spdx_reference_name(document[len(document_prefix):])
            and addition_ref.startswith(addition_prefix)
            and _spdx_reference_name(addition_ref[len(addition_prefix):])
        )
    return _spdx_ordinary_identifier(text) and not text.startswith("LicenseRef-")


def validate_spdx_expression(expression: str) -> str | None:
    """Return an actionable syntax error, or None for a valid SPDX expression.

    This deliberately validates the stable expression grammar and identifier
    spelling without embedding a revision of the evolving SPDX License List.
    A syntactically valid declaration is not a rights or provenance verdict.
    """
    if not isinstance(expression, str) or not expression.strip():
        return "enter an SPDX license expression"
    try:
        encoded = expression.encode("ascii")
    except UnicodeEncodeError:
        return "SPDX expressions use ASCII identifiers and operators"
    if len(encoded) > 128:
        return "SPDX expression exceeds 128 bytes"

    tokens: list[tuple[str, int]] = []
    offset = 0
    while offset < len(expression):
        if expression[offset].isspace():
            offset += 1
            continue
        start = offset
        if expression[offset] in "()+":
            tokens.append((expression[offset], offset))
            offset += 1
            continue
        while (
            offset < len(expression)
            and not expression[offset].isspace()
            and expression[offset] not in "()+"
        ):
            offset += 1
        tokens.append((expression[start:offset], start))

    class Parser:
        def __init__(self) -> None:
            self.index = 0

        def current(self) -> tuple[str, int] | None:
            return tokens[self.index] if self.index < len(tokens) else None

        def take(self, *values: str) -> bool:
            current = self.current()
            if current is None or current[0] not in values:
                return False
            self.index += 1
            return True

        def parse_or(self) -> str | None:
            error = self.parse_and()
            if error is not None:
                return error
            while self.take("OR", "or"):
                error = self.parse_and()
                if error is not None:
                    return error
            return None

        def parse_and(self) -> str | None:
            error = self.parse_with()
            if error is not None:
                return error
            while self.take("AND", "and"):
                error = self.parse_with()
                if error is not None:
                    return error
            return None

        def parse_with(self) -> str | None:
            simple, error = self.parse_primary()
            if error is not None or not self.take("WITH", "with"):
                return error
            with_offset = tokens[self.index - 1][1]
            if not simple:
                return (
                    f"SPDX WITH at byte {with_offset} must follow one license "
                    "identifier, not a parenthesized expression"
                )
            current = self.current()
            if current is None or not _spdx_addition_identifier(current[0]):
                return (
                    "SPDX WITH must be followed by a license exception or "
                    "AdditionRef identifier"
                )
            self.index += 1
            return None

        def parse_primary(self) -> tuple[bool, str | None]:
            current = self.current()
            if current is None:
                return False, "expected an SPDX license identifier or '('"
            text, token_offset = current
            if text == "(":
                self.index += 1
                error = self.parse_or()
                if error is not None:
                    return False, error
                if not self.take(")"):
                    return False, "SPDX parenthesized expression is missing ')'"
                return False, None
            if not _spdx_license_identifier(text):
                return False, (
                    f"invalid SPDX license identifier {text!r} at byte "
                    f"{token_offset}"
                )
            self.index += 1
            current_index = self.index
            if self.take("+"):
                if text.startswith(("LicenseRef-", "DocumentRef-")):
                    return False, "SPDX '+' cannot qualify a custom LicenseRef"
                if tokens[current_index][1] != token_offset + len(text):
                    return False, (
                        "SPDX '+' must immediately follow its license identifier"
                    )
            return True, None

    parser = Parser()
    error = parser.parse_or()
    if error is not None:
        return error
    if parser.index != len(tokens):
        token, token_offset = tokens[parser.index]
        return f"unexpected SPDX token {token!r} at byte {token_offset}"
    return None


def _sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _json_bytes(value: Any) -> bytes:
    return (json.dumps(value, sort_keys=True, separators=(",", ":")) + "\n").encode("utf-8")


def json_loads_strict(data: bytes | str, label: str = "JSON") -> Any:
    """Decode hostile JSON without duplicate keys or non-finite numbers."""
    def object_from_pairs(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
        value: dict[str, Any] = {}
        for key, member in pairs:
            if key in value:
                raise ProbeError(f"{label} contains duplicate key {key!r}")
            value[key] = member
        return value

    def reject_constant(value: str) -> Any:
        raise ProbeError(f"{label} contains non-finite number {value}")

    def bounded_integer(value: str) -> int:
        digits = value[1:] if value.startswith("-") else value
        if len(digits) > 128:
            raise ProbeError(f"{label} contains an oversized integer")
        return int(value)

    def finite_float(value: str) -> float:
        parsed = float(value)
        if not math.isfinite(parsed):
            raise ProbeError(f"{label} contains non-finite number {value}")
        return parsed

    try:
        text = data.decode("utf-8") if isinstance(data, bytes) else data
        return json.loads(
            text, object_pairs_hook=object_from_pairs,
            parse_constant=reject_constant,
            parse_int=bounded_integer,
            parse_float=finite_float,
        )
    except UnicodeDecodeError as exc:
        raise ProbeError(f"{label} is not valid UTF-8: {exc}") from exc


def _manifest_unicode_errors(value: Any, path: str = "manifest") -> list[str]:
    errors: list[str] = []
    if isinstance(value, str):
        if unicodedata.normalize("NFC", value) != value:
            errors.append(f"{path} must use NFC-normalized Unicode")
    elif isinstance(value, dict):
        for key, member in value.items():
            if unicodedata.normalize("NFC", key) != key:
                errors.append(f"{path} contains a non-NFC key")
            errors.extend(_manifest_unicode_errors(member, f"{path}.{key}"))
    elif isinstance(value, list):
        for index, member in enumerate(value):
            errors.extend(_manifest_unicode_errors(member, f"{path}[{index}]"))
    return errors


def _bounded_printable_text(value: str, maximum_bytes: int) -> bool:
    """Match the native cache/UI text profile without restricting scripts."""
    return (
        len(value.encode("utf-8")) <= maximum_bytes
        and all(
            not (
                ord(character) < 0x20
                or 0x7F <= ord(character) <= 0x9F
                or 0x200B <= ord(character) <= 0x200F
                or 0x2028 <= ord(character) <= 0x202E
                or 0x2060 <= ord(character) <= 0x206F
                or ord(character) == 0xFEFF
            )
            for character in value
        )
    )


def _read_bounded(path: Path, maximum: int, label: str) -> bytes:
    size = path.stat().st_size
    if size > maximum:
        raise ProbeError(f"{label} exceeds {maximum} bytes")
    data = path.read_bytes()
    if len(data) > maximum:
        raise ProbeError(f"{label} exceeds {maximum} bytes")
    return data


def inspect_portrait_png(data: bytes) -> dict[str, int]:
    """Validate the deliberately narrow, portable identity-image profile.

    Portrait Studio exports this profile directly. Restricting the package
    boundary to non-interlaced 8-bit RGB/RGBA avoids decoder-dependent first
    frames, palettes, colour-key transparency, and high-bit-depth conversion.
    The runtime still decodes independently and fails closed.
    """
    if len(data) > MAX_PORTRAIT_BYTES:
        raise ProbeError(f"portrait.png exceeds {MAX_PORTRAIT_BYTES} bytes")
    if len(data) < 8 or data[:8] != PNG_SIGNATURE:
        raise ProbeError("portrait.png is not a PNG")
    offset = 8
    chunks = 0
    saw_ihdr = False
    saw_idat = False
    width = height = colour_type = 0
    while offset < len(data):
        if len(data) - offset < 12:
            raise ProbeError("portrait.png has a truncated chunk")
        length = struct.unpack_from(">I", data, offset)[0]
        kind = data[offset + 4:offset + 8]
        end = offset + 12 + length
        if end > len(data):
            raise ProbeError("portrait.png chunk exceeds the file")
        payload = data[offset + 8:offset + 8 + length]
        expected_crc = struct.unpack_from(">I", data, offset + 8 + length)[0]
        if (zlib.crc32(kind + payload) & 0xFFFFFFFF) != expected_crc:
            raise ProbeError("portrait.png has a bad chunk checksum")
        chunks += 1
        if chunks > 4096:
            raise ProbeError("portrait.png has too many chunks")
        if kind == b"IHDR":
            if saw_ihdr or offset != 8 or length != 13:
                raise ProbeError("portrait.png has an invalid IHDR")
            width, height, bit_depth, colour_type, compression, filtering, interlace = (
                struct.unpack(">IIBBBBB", payload)
            )
            if not 16 <= width <= 1024 or not 16 <= height <= 1024:
                raise ProbeError("portrait.png dimensions must be between 16 and 1024")
            if width != height:
                raise ProbeError("portrait.png must be square")
            if bit_depth != 8 or colour_type not in (2, 6):
                raise ProbeError("portrait.png must use 8-bit RGB or RGBA pixels")
            if compression != 0 or filtering != 0 or interlace != 0:
                raise ProbeError("portrait.png must use standard non-interlaced PNG encoding")
            saw_ihdr = True
        elif kind == b"acTL":
            raise ProbeError("animated portrait PNGs are unsupported")
        elif kind == b"IDAT":
            if not saw_ihdr:
                raise ProbeError("portrait.png IDAT precedes IHDR")
            saw_idat = True
        elif kind == b"IEND":
            if length != 0 or not saw_ihdr or not saw_idat or end != len(data):
                raise ProbeError("portrait.png has an invalid IEND")
            return {
                "width": width,
                "height": height,
                "colour_type": colour_type,
                "bytes": len(data),
            }
        offset = end
    raise ProbeError("portrait.png is missing IEND")


def _inspect_texture_png(
    data: bytes | memoryview, image_index: int
) -> tuple[int, int]:
    """Validate one embedded PNG without allocating beyond its pixel budget."""
    label = f"images[{image_index}]"
    source = memoryview(data)
    if len(source) < 8 or bytes(source[:8]) != PNG_SIGNATURE:
        raise ProbeError(f"{label} is not a PNG")
    offset = 8
    chunks = 0
    saw_ihdr = False
    saw_plte = False
    saw_idat = False
    ended_idat = False
    width = height = bit_depth = colour_type = interlace = 0
    expected_decoded = 0
    pass_rows: list[tuple[int, int]] = []
    decoded = bytearray()
    decompressor: Any = None
    valid_depths = {
        0: {1, 2, 4, 8, 16},
        2: {8, 16},
        3: {1, 2, 4, 8},
        4: {8, 16},
        6: {8, 16},
    }
    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}
    adam7 = (
        (0, 0, 8, 8), (4, 0, 8, 8), (0, 4, 4, 8),
        (2, 0, 4, 4), (0, 2, 2, 4), (1, 0, 2, 2),
        (0, 1, 1, 2),
    )
    while offset < len(source):
        if len(source) - offset < 12:
            raise ProbeError(f"{label} has a truncated PNG chunk")
        length = struct.unpack_from(">I", source, offset)[0]
        kind = bytes(source[offset + 4:offset + 8])
        end = offset + 12 + length
        if end > len(source):
            raise ProbeError(f"{label} PNG chunk exceeds its bufferView")
        payload = source[offset + 8:offset + 8 + length]
        expected_crc = struct.unpack_from(">I", source, offset + 8 + length)[0]
        actual_crc = zlib.crc32(payload, zlib.crc32(kind)) & 0xFFFFFFFF
        if actual_crc != expected_crc:
            raise ProbeError(f"{label} PNG has a bad chunk checksum")
        chunks += 1
        if chunks > 4096:
            raise ProbeError(f"{label} PNG has too many chunks")
        if kind == b"IHDR":
            if saw_ihdr or offset != 8 or length != 13:
                raise ProbeError(f"{label} PNG has an invalid IHDR")
            (width, height, bit_depth, colour_type, compression, filtering,
             interlace) = struct.unpack(">IIBBBBB", payload)
            if (
                width == 0 or height == 0
                or width > MAX_TEXTURE_DIMENSION
                or height > MAX_TEXTURE_DIMENSION
            ):
                raise ProbeError(
                    f"{label} PNG dimensions {width}x{height} exceed the "
                    f"{MAX_TEXTURE_DIMENSION}x{MAX_TEXTURE_DIMENSION} profile"
                )
            if (
                colour_type not in valid_depths
                or bit_depth not in valid_depths[colour_type]
                or compression != 0 or filtering != 0
                or interlace not in (0, 1)
            ):
                raise ProbeError(f"{label} PNG has an unsupported IHDR profile")
            passes = ((0, 0, 1, 1),) if interlace == 0 else adam7
            for x0, y0, dx, dy in passes:
                pass_width = (
                    (width - x0 + dx - 1) // dx if width > x0 else 0
                )
                pass_height = (
                    (height - y0 + dy - 1) // dy if height > y0 else 0
                )
                if pass_width == 0 or pass_height == 0:
                    continue
                row_bytes = (
                    pass_width * channels[colour_type] * bit_depth + 7
                ) // 8
                pass_rows.append((pass_height, row_bytes))
                expected_decoded += pass_height * (row_bytes + 1)
            if expected_decoded > MAX_DECODED_TEXTURE_BYTES:
                raise ProbeError(f"{label} PNG exceeds the decoded texture budget")
            saw_ihdr = True
        elif kind == b"PLTE":
            if (
                not saw_ihdr or saw_idat or saw_plte
                or length == 0 or length > 768 or length % 3 != 0
                or colour_type in (0, 4)
                or (colour_type == 3 and length // 3 > (1 << bit_depth))
            ):
                raise ProbeError(f"{label} PNG has an invalid PLTE")
            saw_plte = True
        elif kind == b"acTL":
            raise ProbeError(f"{label} animated PNG textures are unsupported")
        elif kind == b"IDAT":
            if not saw_ihdr or ended_idat or (colour_type == 3 and not saw_plte):
                raise ProbeError(f"{label} PNG has invalid IDAT ordering")
            if decompressor is None:
                decompressor = zlib.decompressobj()
            try:
                remaining = expected_decoded + 1 - len(decoded)
                if remaining <= 0:
                    raise ProbeError(
                        f"{label} PNG expands beyond its declared dimensions"
                    )
                decoded.extend(decompressor.decompress(payload, remaining))
            except zlib.error as exc:
                raise ProbeError(f"{label} PNG has invalid compressed pixels") from exc
            if decompressor.unconsumed_tail or len(decoded) > expected_decoded:
                raise ProbeError(
                    f"{label} PNG expands beyond its declared dimensions"
                )
            saw_idat = True
        elif kind == b"IEND":
            if (
                length != 0 or not saw_ihdr or not saw_idat
                or end != len(source) or decompressor is None
            ):
                raise ProbeError(f"{label} PNG has an invalid IEND")
            try:
                remaining = expected_decoded + 1 - len(decoded)
                if remaining > 0:
                    decoded.extend(decompressor.flush(remaining))
            except zlib.error as exc:
                raise ProbeError(f"{label} PNG has invalid compressed pixels") from exc
            if (
                not decompressor.eof or decompressor.unused_data
                or decompressor.unconsumed_tail
                or len(decoded) != expected_decoded
            ):
                raise ProbeError(
                    f"{label} PNG pixels do not match its declared dimensions"
                )
            decoded_offset = 0
            for row_count, row_bytes in pass_rows:
                for _ in range(row_count):
                    if decoded[decoded_offset] > 4:
                        raise ProbeError(f"{label} PNG uses an invalid row filter")
                    decoded_offset += row_bytes + 1
            return width, height
        elif saw_idat:
            ended_idat = True
        if kind not in (b"IHDR", b"PLTE", b"IDAT", b"IEND") and not (kind[0] & 0x20):
            raise ProbeError(f"{label} PNG has an unknown critical chunk")
        offset = end
    raise ProbeError(f"{label} PNG is missing IEND")


def package_members_for_schema(schema: object, portable: bool = False) -> tuple[str, ...]:
    if schema in (PACKAGE_SCHEMA_V3, PACKAGE_SCHEMA_V4):
        return PORTABLE_PACKAGE_MEMBERS_V3 if portable else PACKAGE_MEMBERS_V3
    return PORTABLE_PACKAGE_MEMBERS if portable else PACKAGE_MEMBERS


def _safe_archive_name(raw_name: str) -> str:
    name = raw_name.replace("\\", "/")
    path = PurePosixPath(name)
    if not name or name.startswith("/") or path.is_absolute() or ".." in path.parts:
        raise ProbeError(f"unsafe archive member path: {raw_name!r}")
    if path.parts and ":" in path.parts[0]:
        raise ProbeError(f"drive-qualified archive member path: {raw_name!r}")
    return path.as_posix()


def _zip_member_is_symlink(info: zipfile.ZipInfo) -> bool:
    return ((info.external_attr >> 16) & 0o170000) == 0o120000


def _archive_report_has_license(report: dict[str, Any]) -> bool:
    return bool(report.get("license_files")) or any(
        _archive_report_has_license(nested)
        for nested in report.get("nested_archives", [])
    )


def _archive_report_has_model(report: dict[str, Any]) -> bool:
    return bool(report.get("models")) or any(
        _archive_report_has_model(nested)
        for nested in report.get("nested_archives", [])
    )


def _validate_general_archive_budget(
    infos: list[zipfile.ZipInfo], name: str
) -> tuple[int, int]:
    """Reject costly ZIP payloads from central-directory facts before reads."""
    total_expanded = 0
    total_compressed = 0
    for info in infos:
        if info.is_dir():
            continue
        safe_name = _safe_archive_name(info.filename)
        if info.compress_type not in (zipfile.ZIP_STORED, zipfile.ZIP_DEFLATED):
            raise ProbeError(
                f"unsupported ZIP compression method for archive member: {safe_name}"
            )
        if info.file_size < 0 or info.compress_size < 0:
            raise ProbeError(f"invalid ZIP member sizes: {safe_name}")
        allowed = (
            info.compress_size * MAX_ARCHIVE_EXPANSION_RATIO
            + ARCHIVE_EXPANSION_SLACK_BYTES
        )
        if info.file_size > allowed:
            raise ProbeError(
                "archive member compression ratio exceeds the bounded intake "
                f"policy: {safe_name}"
            )
        total_expanded += info.file_size
        total_compressed += info.compress_size
    aggregate_allowed = (
        total_compressed * MAX_ARCHIVE_EXPANSION_RATIO
        + ARCHIVE_EXPANSION_SLACK_BYTES
    )
    if total_expanded > aggregate_allowed:
        raise ProbeError(
            f"archive aggregate compression ratio exceeds the bounded intake policy: {name}"
        )
    return total_compressed, total_expanded


def _local_name(tag: str) -> str:
    return tag.rsplit("}", 1)[-1]


def inspect_dae(data: bytes, name: str = "model.dae") -> dict[str, Any]:
    if len(data) > MAX_INPUT_BYTES:
        raise ProbeError(f"COLLADA input exceeds {MAX_INPUT_BYTES} bytes")
    try:
        root = ElementTree.fromstring(data)
    except ElementTree.ParseError as exc:
        raise ProbeError(f"invalid COLLADA XML in {name}: {exc}") from exc

    def elements(local: str):
        return (elem for elem in root.iter() if _local_name(elem.tag) == local)

    author = next((elem.text or "" for elem in elements("author")), "").strip()
    authoring_tool = next((elem.text or "" for elem in elements("authoring_tool")), "").strip()
    unit = next(elements("unit"), None)
    up_axis = next((elem.text or "" for elem in elements("up_axis")), "").strip()
    triangle_count = sum(int(elem.attrib.get("count", "0")) for elem in elements("triangles"))
    triangle_count += sum(int(elem.attrib.get("count", "0")) for elem in elements("polylist"))

    joint_names: set[str] = set()
    for array_name in ("Name_array", "IDREF_array"):
        for elem in elements(array_name):
            joint_names.update((elem.text or "").split())
    influence_max = 0
    for elem in elements("vcount"):
        counts = [int(value) for value in (elem.text or "").split()]
        influence_max = max(influence_max, max(counts, default=0))

    channels = sum(1 for _ in elements("channel"))
    return {
        "format": "collada-1.x",
        "name": name,
        "author": author or None,
        "authoring_tool": authoring_tool or None,
        "unit_meter": float(unit.attrib.get("meter", "1")) if unit is not None else 1.0,
        "up_axis": up_axis or None,
        "geometry_count": sum(1 for _ in elements("geometry")),
        "triangle_count": triangle_count,
        "material_count": sum(1 for _ in elements("material")),
        "image_count": sum(1 for _ in elements("image")),
        "skin_count": sum(1 for _ in elements("skin")),
        "joint_name_count_upper_bound": len(joint_names),
        "max_influences": influence_max,
        "animation_channel_count": channels,
    }


def inspect_archive_bytes(data: bytes, name: str, depth: int = 0) -> dict[str, Any]:
    if len(data) > MAX_INPUT_BYTES:
        raise ProbeError(f"archive exceeds {MAX_INPUT_BYTES} bytes")
    if depth > MAX_ARCHIVE_DEPTH:
        raise ProbeError(f"archive nesting exceeds {MAX_ARCHIVE_DEPTH}")
    try:
        archive = zipfile.ZipFile(io.BytesIO(data))
    except zipfile.BadZipFile as exc:
        raise ProbeError(f"invalid ZIP archive {name}: {exc}") from exc

    infos = archive.infolist()
    if len(infos) > MAX_ARCHIVE_MEMBERS:
        raise ProbeError(f"archive contains more than {MAX_ARCHIVE_MEMBERS} members")
    compressed_total, declared_expanded_total = (
        _validate_general_archive_budget(infos, name)
    )
    total = 0
    files: list[dict[str, Any]] = []
    models: list[dict[str, Any]] = []
    licenses: list[str] = []
    nested: list[dict[str, Any]] = []
    for info in infos:
        safe_name = _safe_archive_name(info.filename)
        if info.is_dir():
            continue
        if info.flag_bits & 1:
            raise ProbeError(f"encrypted archive member is unsupported: {safe_name}")
        if _zip_member_is_symlink(info):
            raise ProbeError(f"archive symlink is unsupported: {safe_name}")
        total += info.file_size
        if total > MAX_INPUT_BYTES:
            raise ProbeError(f"expanded archive exceeds {MAX_INPUT_BYTES} bytes")
        suffix = PurePosixPath(safe_name).suffix.lower()
        files.append({"path": safe_name, "bytes": info.file_size, "sha256": None})
        basename = PurePosixPath(safe_name).name.lower()
        if basename in LICENSE_NAMES or basename.startswith("license."):
            licenses.append(safe_name)
        if suffix in MODEL_SUFFIXES:
            entry: dict[str, Any] = {"path": safe_name, "format": suffix[1:]}
            if suffix == ".dae":
                entry["inspection"] = inspect_dae(archive.read(info), safe_name)
            models.append(entry)
        if suffix == ".zip":
            if info.file_size > MAX_NESTED_ARCHIVE_BYTES:
                raise ProbeError(f"nested archive is too large: {safe_name}")
            nested.append(inspect_archive_bytes(archive.read(info), safe_name, depth + 1))

    blockers: list[str] = []
    if not licenses and not any(
            _archive_report_has_license(item) for item in nested):
        blockers.append("no embedded license or copyright file")
    if not models and not any(
            _archive_report_has_model(item) for item in nested):
        blockers.append("no supported model candidate")
    return {
        "format": "zip",
        "name": name,
        "archive_depth": depth,
        "member_count": len(files),
        "expanded_bytes": total,
        "compressed_member_bytes": compressed_total,
        "declared_expanded_bytes": declared_expanded_total,
        "expansion_ratio_limit": MAX_ARCHIVE_EXPANSION_RATIO,
        "expansion_slack_bytes": ARCHIVE_EXPANSION_SLACK_BYTES,
        "license_files": licenses,
        "models": models,
        "nested_archives": nested,
        "blockers": blockers,
    }


def inspect_archive(path: Path) -> dict[str, Any]:
    return inspect_archive_bytes(_read_bounded(path, MAX_INPUT_BYTES, "archive"), path.name)


def parse_glb(data: bytes) -> tuple[dict[str, Any], bytes | None]:
    if len(data) > MAX_INPUT_BYTES:
        raise ProbeError(f"GLB input exceeds {MAX_INPUT_BYTES} bytes")
    if len(data) < 20:
        raise ProbeError("GLB is shorter than its header and JSON chunk")
    magic, version, declared_length = struct.unpack_from("<4sII", data, 0)
    if magic != GLB_MAGIC:
        raise ProbeError("GLB magic is not 'glTF'")
    if version != 2:
        raise ProbeError(f"unsupported GLB version {version}; expected 2")
    if declared_length != len(data):
        raise ProbeError(f"GLB declares {declared_length} bytes but contains {len(data)}")

    offset = 12
    json_chunk: bytes | None = None
    bin_chunk: bytes | None = None
    chunk_index = 0
    while offset < len(data):
        if offset + 8 > len(data):
            raise ProbeError("truncated GLB chunk header")
        chunk_length, chunk_type = struct.unpack_from("<II", data, offset)
        offset += 8
        end = offset + chunk_length
        if chunk_length % 4 or end > len(data):
            raise ProbeError("invalid GLB chunk size")
        payload = data[offset:end]
        offset = end
        if chunk_index == 0 and chunk_type != GLB_JSON_CHUNK:
            raise ProbeError("first GLB chunk is not JSON")
        if chunk_type == GLB_JSON_CHUNK:
            if json_chunk is not None:
                raise ProbeError("GLB contains multiple JSON chunks")
            json_chunk = payload
        elif chunk_type == GLB_BIN_CHUNK:
            if bin_chunk is not None:
                raise ProbeError("GLB contains multiple BIN chunks")
            bin_chunk = payload
        chunk_index += 1
    if json_chunk is None:
        raise ProbeError("GLB has no JSON chunk")
    try:
        document = json_loads_strict(
            json_chunk.rstrip(b" \t\r\n\0"), "GLB JSON"
        )
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise ProbeError(f"invalid GLB JSON: {exc}") from exc
    if not isinstance(document, dict):
        raise ProbeError("GLB JSON root must be an object")
    return document, bin_chunk


def _array(document: dict[str, Any], name: str) -> list[Any]:
    value = document.get(name, [])
    return value if isinstance(value, list) else []


def _accessor(document: dict[str, Any], index: Any) -> dict[str, Any] | None:
    accessors = _array(document, "accessors")
    return (
        accessors[index]
        if isinstance(index, int) and not isinstance(index, bool)
        and 0 <= index < len(accessors)
        and isinstance(accessors[index], dict)
        else None
    )


def _gltf_integer(value: Any, minimum: int = 0) -> bool:
    return isinstance(value, int) and not isinstance(value, bool) and value >= minimum


def _gltf_number(value: Any) -> bool:
    if not isinstance(value, (int, float)) or isinstance(value, bool):
        return False
    try:
        return math.isfinite(float(value))
    except OverflowError:
        return False


def _gltf_float32(value: Any) -> float | None:
    if not _gltf_number(value):
        return None
    try:
        return struct.unpack("<f", struct.pack("<f", float(value)))[0]
    except (OverflowError, struct.error):
        return None


def _validate_glb_scene_and_materials(
    document: dict[str, Any], errors: list[str]
) -> None:
    """Validate the JSON contracts consumed directly by cache version 1."""
    array_names = (
        "scenes", "nodes", "meshes", "materials", "images", "textures",
        "samplers", "skins", "animations",
    )
    for name in array_names:
        if name in document and not isinstance(document[name], list):
            errors.append(f"glTF {name} must be an array")

    scenes = _array(document, "scenes")
    nodes = _array(document, "nodes")
    meshes = _array(document, "meshes")
    materials = _array(document, "materials")
    images = _array(document, "images")
    textures = _array(document, "textures")
    samplers = _array(document, "samplers")
    skins = _array(document, "skins")
    used_extensions = document.get("extensionsUsed", [])
    used_extension_set = (
        set(used_extensions)
        if isinstance(used_extensions, list)
        and all(isinstance(value, str) for value in used_extensions)
        else set()
    )

    def optional_name(item: dict[str, Any], path: str) -> None:
        if "name" in item and not isinstance(item["name"], str):
            errors.append(f"{path}.name must be a string")

    def vector(
        value: Any, length: int, path: str, *, minimum: float | None = None,
        maximum: float | None = None,
    ) -> bool:
        valid = (
            isinstance(value, list) and len(value) == length
            and all(_gltf_float32(component) is not None for component in value)
        )
        if valid and minimum is not None:
            valid = all(component >= minimum for component in value)
        if valid and maximum is not None:
            valid = all(component <= maximum for component in value)
        if not valid:
            errors.append(f"{path} must contain {length} finite profile values")
        return valid

    scene_index = document.get("scene", 0)
    if (
        not _gltf_integer(scene_index) or scene_index >= len(scenes)
    ):
        errors.append("default scene index is invalid")
    for index, scene in enumerate(scenes):
        prefix = f"scenes[{index}]"
        if not isinstance(scene, dict):
            errors.append(f"{prefix} must be an object")
            continue
        optional_name(scene, prefix)
        roots = scene.get("nodes", [])
        if (
            not isinstance(roots, list)
            or any(not _gltf_integer(node) or node >= len(nodes) for node in roots)
            or len(set(roots)) != len(roots)
        ):
            errors.append(f"{prefix}.nodes must contain unique valid node indices")

    parents = [-1] * len(nodes)
    node_names: set[str] = set()
    lod_targets: set[int] = set()
    for index, node in enumerate(nodes):
        prefix = f"nodes[{index}]"
        if not isinstance(node, dict):
            errors.append(f"{prefix} must be an object")
            continue
        optional_name(node, prefix)
        name = node.get("name")
        if isinstance(name, str) and name:
            if name in node_names:
                errors.append(
                    f"{prefix}.name duplicates another node; node names must be unique"
                )
            node_names.add(name)
        children = node.get("children", [])
        if not isinstance(children, list):
            errors.append(f"{prefix}.children must be an array")
        else:
            seen_children: set[int] = set()
            for child in children:
                if not _gltf_integer(child) or child >= len(nodes):
                    errors.append(f"{prefix}.children contains an invalid node index")
                    continue
                if child in seen_children:
                    errors.append(f"{prefix}.children contains a duplicate node")
                    continue
                seen_children.add(child)
                if parents[child] != -1:
                    errors.append(f"nodes[{child}] has multiple parents")
                else:
                    parents[child] = index
        if "mesh" in node and (
            not _gltf_integer(node["mesh"]) or node["mesh"] >= len(meshes)
        ):
            errors.append(f"{prefix}.mesh is invalid")
        if "skin" in node:
            if (
                not _gltf_integer(node["skin"]) or node["skin"] >= len(skins)
            ):
                errors.append(f"{prefix}.skin is invalid")
            if "mesh" not in node:
                errors.append(f"{prefix}.skin requires a mesh")
        if "weights" in node:
            errors.append(f"{prefix}.weights requires morph targets unsupported by cache v1")
        transform_fields = {"translation", "rotation", "scale"}
        if "matrix" in node and transform_fields.intersection(node):
            errors.append(f"{prefix} cannot define both matrix and TRS")
        if "matrix" in node:
            matrix = node["matrix"]
            if vector(matrix, 16, f"{prefix}.matrix"):
                floats = [float(value) for value in matrix]
                if any(abs(floats[item] - expected) > 1.0e-6
                       for item, expected in ((3, 0.0), (7, 0.0),
                                              (11, 0.0), (15, 1.0))):
                    errors.append(f"{prefix}.matrix must be affine")
                columns = [
                    floats[0:3], floats[4:7], floats[8:11],
                ]
                lengths = [
                    math.sqrt(sum(component * component for component in column))
                    for column in columns
                ]
                if min(lengths) < 1.0e-12:
                    errors.append(f"{prefix}.matrix has a singular scale")
                elif any(
                    abs(sum(
                        columns[left][axis] * columns[right][axis]
                        for axis in range(3)
                    ) / (lengths[left] * lengths[right])) > 1.0e-5
                    for left, right in ((0, 1), (0, 2), (1, 2))
                ):
                    errors.append(
                        f"{prefix}.matrix contains shear unsupported by cache v1"
                    )
        else:
            if "translation" in node:
                vector(node["translation"], 3, f"{prefix}.translation")
            if "scale" in node:
                if vector(node["scale"], 3, f"{prefix}.scale") and any(
                    abs(float(component)) < 1.0e-12
                    for component in node["scale"]
                ):
                    errors.append(f"{prefix}.scale must be non-singular")
            if "rotation" in node and vector(
                node["rotation"], 4, f"{prefix}.rotation"
            ):
                length_squared = sum(
                    float(component) * float(component)
                    for component in node["rotation"]
                )
                if abs(length_squared - 1.0) > 2.0e-5:
                    errors.append(f"{prefix}.rotation must be a unit quaternion")
        extensions = node.get("extensions", {})
        if not isinstance(extensions, dict):
            errors.append(f"{prefix}.extensions must be an object")
        elif "MSFT_lod" in extensions:
            lod = extensions["MSFT_lod"]
            ids = lod.get("ids") if isinstance(lod, dict) else None
            if (
                not isinstance(ids, list) or not 1 <= len(ids) <= 3
                or any(not _gltf_integer(value) or value >= len(nodes)
                       or value == index for value in ids)
                or len(set(ids)) != len(ids)
            ):
                errors.append(f"{prefix}.extensions.MSFT_lod is invalid")
            elif any(value in lod_targets for value in ids):
                errors.append(
                    f"{prefix}.extensions.MSFT_lod reuses a LOD target"
                )
            else:
                lod_targets.update(ids)
            if "MSFT_lod" not in used_extension_set:
                errors.append(f"{prefix} uses MSFT_lod without extensionsUsed")

    cycle_reported = False
    for start in range(len(nodes)):
        current = start
        visited: set[int] = set()
        while current >= 0:
            if current in visited:
                if not cycle_reported:
                    errors.append("node hierarchy contains a cycle")
                    cycle_reported = True
                break
            visited.add(current)
            current = parents[current]
    if _gltf_integer(scene_index) and scene_index < len(scenes):
        scene = scenes[scene_index]
        roots = scene.get("nodes", []) if isinstance(scene, dict) else []
        if isinstance(roots, list):
            for root in roots:
                if _gltf_integer(root) and root < len(parents) and parents[root] >= 0:
                    errors.append(
                        f"default scene root nodes[{root}] is also a child node"
                    )

    for mesh_index, mesh in enumerate(meshes):
        if not isinstance(mesh, dict):
            continue
        optional_name(mesh, f"meshes[{mesh_index}]")
        primitives = mesh.get("primitives", [])
        if not isinstance(primitives, list):
            continue
        for primitive_index, primitive in enumerate(primitives):
            if not isinstance(primitive, dict):
                continue
            prefix = f"meshes[{mesh_index}].primitives[{primitive_index}]"
            if "material" in primitive and (
                not _gltf_integer(primitive["material"])
                or primitive["material"] >= len(materials)
            ):
                errors.append(f"{prefix}.material is invalid")

    def texture_info(info: Any, path: str, *, scalar: str | None = None) -> None:
        if not isinstance(info, dict):
            errors.append(f"{path} must be an object")
            return
        reference = info.get("index")
        if not _gltf_integer(reference) or reference >= len(textures):
            errors.append(f"{path}.index is invalid")
        if "texCoord" in info and info["texCoord"] != 0:
            errors.append(f"{path}.texCoord must be 0 for modern-skeletal-v1")
        elif "texCoord" in info and not _gltf_integer(info["texCoord"]):
            errors.append(f"{path}.texCoord must be 0 for modern-skeletal-v1")
        if scalar is not None and scalar in info and _gltf_float32(info[scalar]) is None:
            errors.append(f"{path}.{scalar} must be finite")

    for index, material in enumerate(materials):
        prefix = f"materials[{index}]"
        if not isinstance(material, dict):
            errors.append(f"{prefix} must be an object")
            continue
        optional_name(material, prefix)
        pbr = material.get("pbrMetallicRoughness", {})
        if not isinstance(pbr, dict):
            errors.append(f"{prefix}.pbrMetallicRoughness must be an object")
            pbr = {}
        if "baseColorFactor" in pbr:
            vector(pbr["baseColorFactor"], 4, f"{prefix}.pbrMetallicRoughness.baseColorFactor", minimum=0.0, maximum=1.0)
        for field in ("metallicFactor", "roughnessFactor"):
            if field in pbr and (
                _gltf_float32(pbr[field]) is None or not 0.0 <= pbr[field] <= 1.0
            ):
                errors.append(f"{prefix}.pbrMetallicRoughness.{field} must be from 0 to 1")
        for field in ("baseColorTexture", "metallicRoughnessTexture"):
            if field in pbr:
                texture_info(pbr[field], f"{prefix}.pbrMetallicRoughness.{field}")
        if "emissiveFactor" in material:
            vector(material["emissiveFactor"], 3, f"{prefix}.emissiveFactor", minimum=0.0)
        if "alphaMode" in material and material["alphaMode"] not in (
            "OPAQUE", "MASK", "BLEND",
        ):
            errors.append(f"{prefix}.alphaMode is unsupported")
        if "alphaCutoff" in material and (
            _gltf_float32(material["alphaCutoff"]) is None
            or not 0.0 <= material["alphaCutoff"] <= 1.0
        ):
            errors.append(f"{prefix}.alphaCutoff must be from 0 to 1")
        if "doubleSided" in material and not isinstance(material["doubleSided"], bool):
            errors.append(f"{prefix}.doubleSided must be Boolean")
        for field, scalar in (
            ("normalTexture", "scale"),
            ("occlusionTexture", "strength"),
            ("emissiveTexture", None),
        ):
            if field in material:
                texture_info(material[field], f"{prefix}.{field}", scalar=scalar)
        occlusion = material.get("occlusionTexture")
        if (
            isinstance(occlusion, dict) and "strength" in occlusion
            and _gltf_float32(occlusion["strength"]) is not None
            and not 0.0 <= occlusion["strength"] <= 1.0
        ):
            errors.append(f"{prefix}.occlusionTexture.strength must be from 0 to 1")

    for index, image in enumerate(images):
        if isinstance(image, dict):
            optional_name(image, f"images[{index}]")
    for index, texture in enumerate(textures):
        prefix = f"textures[{index}]"
        if not isinstance(texture, dict):
            errors.append(f"{prefix} must be an object")
            continue
        optional_name(texture, prefix)
        source = texture.get("source")
        if not _gltf_integer(source) or source >= len(images):
            errors.append(f"{prefix}.source is invalid")
        if "sampler" in texture and (
            not _gltf_integer(texture["sampler"])
            or texture["sampler"] >= len(samplers)
        ):
            errors.append(f"{prefix}.sampler is invalid")
        if "extensions" in texture and not isinstance(texture["extensions"], dict):
            errors.append(f"{prefix}.extensions must be an object")
    for index, sampler in enumerate(samplers):
        prefix = f"samplers[{index}]"
        if not isinstance(sampler, dict):
            errors.append(f"{prefix} must be an object")
            continue
        optional_name(sampler, prefix)
        allowed = {
            "magFilter": {9728, 9729},
            "minFilter": {9728, 9729, 9984, 9985, 9986, 9987},
            "wrapS": {33071, 33648, 10497},
            "wrapT": {33071, 33648, 10497},
        }
        for field, choices in allowed.items():
            if field in sampler and (
                not _gltf_integer(sampler[field]) or sampler[field] not in choices
            ):
                errors.append(f"{prefix}.{field} is unsupported")
    for index, skin in enumerate(skins):
        if not isinstance(skin, dict):
            continue
        optional_name(skin, f"skins[{index}]")
        if "skeleton" in skin and (
            not _gltf_integer(skin["skeleton"]) or skin["skeleton"] >= len(nodes)
        ):
            errors.append(f"skins[{index}].skeleton is invalid")
    animation_names: set[str] = set()
    for index, animation in enumerate(_array(document, "animations")):
        if isinstance(animation, dict):
            optional_name(animation, f"animations[{index}]")
            name = animation.get("name") or f"animation_{index}"
            if isinstance(name, str):
                if name in animation_names:
                    errors.append(
                        f"animations[{index}].name duplicates another animation; "
                        "animation names must be unique"
                    )
                animation_names.add(name)


def _validate_glb_storage(
    document: dict[str, Any], bin_chunk: bytes | None, errors: list[str]
) -> list[dict[str, Any] | None]:
    """Validate every buffer view/accessor before inventory indexes metadata."""
    for name in ("buffers", "bufferViews", "accessors"):
        if name in document and not isinstance(document[name], list):
            errors.append(f"glTF {name} must be an array")
    buffers = _array(document, "buffers")
    views = _array(document, "bufferViews")
    accessors = _array(document, "accessors")
    declared_buffer_bytes: int | None = None
    if len(buffers) != 1:
        errors.append("source GLB must declare exactly one embedded buffer")
    elif not isinstance(buffers[0], dict):
        errors.append("buffers[0] must be an object")
    else:
        declared = buffers[0].get("byteLength")
        if not _gltf_integer(declared, 1):
            errors.append("buffers[0].byteLength must be a positive integer")
        elif bin_chunk is None:
            errors.append("source GLB declares a buffer but has no BIN chunk")
        elif declared > len(bin_chunk):
            errors.append("buffers[0].byteLength exceeds the GLB BIN chunk")
        elif len(bin_chunk) - declared > 3:
            errors.append("GLB BIN chunk has more than three padding bytes")
        else:
            declared_buffer_bytes = declared

    valid_views: list[dict[str, Any] | None] = [None] * len(views)
    for index, view in enumerate(views):
        prefix = f"bufferViews[{index}]"
        if not isinstance(view, dict):
            errors.append(f"{prefix} must be an object")
            continue
        buffer_index = view.get("buffer")
        offset = view.get("byteOffset", 0)
        length = view.get("byteLength")
        stride = view.get("byteStride")
        target = view.get("target")
        valid = True
        if not _gltf_integer(buffer_index) or buffer_index >= len(buffers):
            errors.append(f"{prefix}.buffer is invalid")
            valid = False
        if not _gltf_integer(offset):
            errors.append(f"{prefix}.byteOffset must be a non-negative integer")
            valid = False
        if not _gltf_integer(length, 1):
            errors.append(f"{prefix}.byteLength must be a positive integer")
            valid = False
        if "byteStride" in view and (
            not _gltf_integer(stride, 4) or stride > 252 or stride % 4 != 0
        ):
            errors.append(
                f"{prefix}.byteStride must be a 4-byte multiple from 4 to 252"
            )
            valid = False
        if "target" in view and target not in (34962, 34963):
            errors.append(f"{prefix}.target is unsupported")
            valid = False
        if (
            valid
            and declared_buffer_bytes is not None
            and offset + length > declared_buffer_bytes
        ):
            errors.append(f"{prefix} exceeds buffers[0].byteLength")
            valid = False
        if valid and buffer_index != 0:
            errors.append(f"{prefix} references a non-GLB buffer")
            valid = False
        if valid:
            valid_views[index] = {
                "offset": offset,
                "length": length,
                "stride": stride,
                "target": target,
            }

    descriptors: list[dict[str, Any] | None] = [None] * len(accessors)
    for index, accessor in enumerate(accessors):
        prefix = f"accessors[{index}]"
        if not isinstance(accessor, dict):
            errors.append(f"{prefix} must be an object")
            continue
        component_type = accessor.get("componentType")
        value_type = accessor.get("type")
        count = accessor.get("count")
        view_index = accessor.get("bufferView")
        accessor_offset = accessor.get("byteOffset", 0)
        normalized = accessor.get("normalized", False)
        valid = True
        if (
            not _gltf_integer(component_type)
            or component_type not in GLTF_COMPONENT_BYTES
        ):
            errors.append(f"{prefix}.componentType is unsupported")
            valid = False
        if not isinstance(value_type, str) or value_type not in GLTF_TYPE_COMPONENTS:
            errors.append(f"{prefix}.type is unsupported")
            valid = False
        if not _gltf_integer(count, 1):
            errors.append(f"{prefix}.count must be a positive integer")
            valid = False
        if not _gltf_integer(accessor_offset):
            errors.append(f"{prefix}.byteOffset must be a non-negative integer")
            valid = False
        if not isinstance(normalized, bool):
            errors.append(f"{prefix}.normalized must be Boolean")
            valid = False
        if "sparse" in accessor:
            errors.append(
                f"{prefix} uses sparse storage; normalize it before import"
            )
            valid = False
        if not _gltf_integer(view_index) or view_index >= len(views):
            errors.append(
                f"{prefix}.bufferView is required and must be valid for "
                "modern-skeletal-v1"
            )
            valid = False
        elif valid_views[view_index] is None:
            valid = False
        if (
            component_type == 5126 and normalized is True
        ):
            errors.append(f"{prefix} cannot normalize FLOAT components")
            valid = False
        if (
            isinstance(value_type, str)
            and value_type.startswith("MAT")
            and component_type != 5126
        ):
            errors.append(
                f"{prefix} uses a packed integer matrix unsupported by "
                "modern-skeletal-v1"
            )
            valid = False
        for bound_name in ("min", "max"):
            if bound_name not in accessor:
                continue
            bound = accessor[bound_name]
            components = (
                GLTF_TYPE_COMPONENTS.get(value_type)
                if isinstance(value_type, str) else None
            )
            if (
                not isinstance(bound, list)
                or components is None
                or len(bound) != components
                or not all(_gltf_number(value) for value in bound)
            ):
                errors.append(
                    f"{prefix}.{bound_name} must contain one finite number "
                    "per component"
                )
                valid = False
            elif component_type == 5126 and any(
                _gltf_float32(value) is None for value in bound
            ):
                errors.append(
                    f"{prefix}.{bound_name} contains a value outside FLOAT range"
                )
                valid = False
        minimum = accessor.get("min")
        maximum = accessor.get("max")
        if (
            isinstance(minimum, list)
            and isinstance(maximum, list)
            and len(minimum) == len(maximum)
            and all(_gltf_number(value) for value in minimum + maximum)
            and any(low > high for low, high in zip(minimum, maximum))
        ):
            errors.append(f"{prefix}.min exceeds max")
            valid = False
        if not valid:
            continue
        assert isinstance(component_type, int)
        assert isinstance(value_type, str)
        assert isinstance(count, int)
        assert isinstance(view_index, int)
        assert isinstance(accessor_offset, int)
        view = valid_views[view_index]
        assert view is not None
        component_bytes = GLTF_COMPONENT_BYTES[component_type]
        element_bytes = component_bytes * GLTF_TYPE_COMPONENTS[value_type]
        stride = view["stride"] if view["stride"] is not None else element_bytes
        absolute_offset = view["offset"] + accessor_offset
        if accessor_offset % component_bytes != 0 or absolute_offset % component_bytes != 0:
            errors.append(f"{prefix} is not component-size aligned")
            continue
        if stride < element_bytes or stride % component_bytes != 0:
            errors.append(f"{prefix} has an invalid effective byte stride")
            continue
        end = accessor_offset + stride * (count - 1) + element_bytes
        if end > view["length"]:
            errors.append(f"{prefix} exceeds its bufferView")
            continue
        if (
            declared_buffer_bytes is not None
            and absolute_offset + stride * (count - 1) + element_bytes
                > declared_buffer_bytes
        ):
            errors.append(f"{prefix} exceeds buffers[0].byteLength")
            continue
        descriptors[index] = {
            "componentType": component_type,
            "type": value_type,
            "count": count,
            "normalized": normalized,
            "view": view_index,
            "offset": accessor_offset,
            "absoluteOffset": absolute_offset,
            "elementBytes": element_bytes,
            "stride": stride,
        }
    return descriptors


def _validate_glb_accessor_usage(
    document: dict[str, Any], descriptors: list[dict[str, Any] | None],
    bin_chunk: bytes | None, errors: list[str]
) -> None:
    accessors = _array(document, "accessors")
    views = _array(document, "bufferViews")
    binary = bin_chunk or b""
    finite_accessors: set[int] = set()
    position_bounds: dict[int, tuple[list[float], list[float]] | None] = {}
    animation_inputs: set[int] = set()
    joint_maxima: dict[int, int] = {}
    rotation_outputs: set[tuple[int, str]] = set()
    direction_accessors: set[tuple[int, str]] = set()
    weight_accessors: set[int] = set()

    def values(reference: int, item: dict[str, Any]):
        component_count = GLTF_TYPE_COMPONENTS[item["type"]]
        unpack = struct.Struct(
            "<" + GLTF_COMPONENT_FORMAT[item["componentType"]] * component_count
        )
        for item_index in range(item["count"]):
            yield unpack.unpack_from(
                binary, item["absoluteOffset"] + item_index * item["stride"]
            )

    def require_finite(reference: int, item: dict[str, Any], path: str) -> None:
        if item["componentType"] != 5126 or reference in finite_accessors:
            return
        finite_accessors.add(reference)
        if any(
            not math.isfinite(component)
            for value in values(reference, item)
            for component in value
        ):
            errors.append(f"{path} contains NaN or infinity")

    def require_position_bounds(
        reference: int, item: dict[str, Any], path: str
    ) -> None:
        if reference in position_bounds:
            return
        raw = raw_accessor(reference)
        if raw is None or "min" not in raw or "max" not in raw:
            position_bounds[reference] = None
            return
        minimum = [math.inf, math.inf, math.inf]
        maximum = [-math.inf, -math.inf, -math.inf]
        for value in values(reference, item):
            for component in range(3):
                minimum[component] = min(minimum[component], value[component])
                maximum[component] = max(maximum[component], value[component])
        declared_minimum = raw["min"]
        declared_maximum = raw["max"]
        rounded_minimum = [_gltf_float32(value) for value in declared_minimum]
        rounded_maximum = [_gltf_float32(value) for value in declared_maximum]
        if minimum != rounded_minimum or maximum != rounded_maximum:
            errors.append(
                f"{path} declared min/max do not match its binary values"
            )
            position_bounds[reference] = None
            return
        position_bounds[reference] = (minimum, maximum)

    def require_animation_input(
        reference: int, item: dict[str, Any], path: str
    ) -> None:
        if reference in animation_inputs:
            return
        animation_inputs.add(reference)
        raw = raw_accessor(reference)
        previous = -math.inf
        minimum = math.inf
        maximum = -math.inf
        for value in values(reference, item):
            time = value[0]
            if not math.isfinite(time) or time < 0.0 or time <= previous:
                errors.append(
                    f"{path} values must be finite, non-negative, and strictly increasing"
                )
                return
            previous = time
            minimum = min(minimum, time)
            maximum = max(maximum, time)
        if raw is not None and "min" in raw and "max" in raw:
            declared_minimum = _gltf_float32(raw["min"][0])
            declared_maximum = _gltf_float32(raw["max"][0])
            if minimum != declared_minimum or maximum != declared_maximum:
                errors.append(
                    f"{path} declared min/max do not match its binary values"
                )

    def require_direction_vectors(
        reference: int, item: dict[str, Any], path: str, *, tangent: bool
    ) -> None:
        key = (reference, "tangent" if tangent else "normal")
        if key in direction_accessors:
            return
        direction_accessors.add(key)
        for value in values(reference, item):
            length_squared = sum(component * component for component in value[:3])
            if not math.isfinite(length_squared) or length_squared < 1.0e-12:
                errors.append(f"{path} contains a zero or non-finite direction")
                return
            if tangent and abs(abs(value[3]) - 1.0) > 1.0e-6:
                errors.append(f"{path} tangent handedness must be -1 or 1")
                return

    def require_positive_weights(
        reference: int, item: dict[str, Any], path: str
    ) -> None:
        if reference in weight_accessors:
            return
        weight_accessors.add(reference)
        divisor = {
            5121: 255.0,
            5123: 65535.0,
            5126: 1.0,
        }[item["componentType"]]
        for value in values(reference, item):
            decoded = [component / divisor for component in value]
            if any(component < 0.0 for component in decoded) or sum(decoded) < 1.0e-12:
                errors.append(
                    f"{path} must contain non-negative influences with positive total weight"
                )
                return

    def descriptor(reference: Any, path: str) -> dict[str, Any] | None:
        if not _gltf_integer(reference) or reference >= len(descriptors):
            errors.append(f"{path} is not a valid accessor index")
            return None
        result = descriptors[reference]
        if result is None:
            errors.append(f"{path} references an invalid accessor")
        return result

    def raw_accessor(reference: Any) -> dict[str, Any] | None:
        if not _gltf_integer(reference) or reference >= len(accessors):
            return None
        value = accessors[reference]
        return value if isinstance(value, dict) else None

    vertex_view_accessors: dict[int, set[int]] = {}
    nodes = _array(document, "nodes")
    skins = _array(document, "skins")
    skin_joint_counts = [
        len(skin.get("joints", []))
        if isinstance(skin, dict) and isinstance(skin.get("joints"), list)
        else 0
        for skin in skins
    ]
    mesh_skin_counts: dict[int, set[int | None]] = {}
    for node in nodes:
        if not isinstance(node, dict) or not _gltf_integer(node.get("mesh")):
            continue
        mesh_index = node["mesh"]
        skin_index = node.get("skin")
        skin_count = (
            skin_joint_counts[skin_index]
            if _gltf_integer(skin_index) and skin_index < len(skin_joint_counts)
            and skin_joint_counts[skin_index] > 0
            else None
        )
        mesh_skin_counts.setdefault(mesh_index, set()).add(skin_count)
    attribute_contracts: dict[str, set[tuple[int, str, bool]]] = {
        "POSITION": {(5126, "VEC3", False)},
        "NORMAL": {(5126, "VEC3", False)},
        "TANGENT": {(5126, "VEC4", False)},
        "TEXCOORD_0": {
            (5126, "VEC2", False),
            (5121, "VEC2", True),
            (5123, "VEC2", True),
        },
        "JOINTS_0": {
            (5121, "VEC4", False),
            (5123, "VEC4", False),
        },
        "WEIGHTS_0": {
            (5126, "VEC4", False),
            (5121, "VEC4", True),
            (5123, "VEC4", True),
        },
    }
    for mesh_index, mesh in enumerate(_array(document, "meshes")):
        if not isinstance(mesh, dict):
            continue
        primitives = mesh.get("primitives", [])
        if not isinstance(primitives, list) or not primitives:
            errors.append(f"meshes[{mesh_index}].primitives must be a non-empty array")
            continue
        for primitive_index, primitive in enumerate(primitives):
            prefix = f"meshes[{mesh_index}].primitives[{primitive_index}]"
            if not isinstance(primitive, dict):
                errors.append(f"{prefix} must be an object")
                continue
            index_descriptor = descriptor(primitive.get("indices"), f"{prefix}.indices")
            if index_descriptor is not None:
                if (
                    index_descriptor["type"] != "SCALAR"
                    or index_descriptor["componentType"] not in (5121, 5123, 5125)
                    or index_descriptor["normalized"]
                ):
                    errors.append(
                        f"{prefix}.indices must use an unnormalized unsigned "
                        "SCALAR accessor"
                    )
                index_view = views[index_descriptor["view"]]
                if isinstance(index_view, dict):
                    if "byteStride" in index_view:
                        errors.append(f"{prefix}.indices must be tightly packed")
                    if index_view.get("target") not in (None, 34963):
                        errors.append(f"{prefix}.indices has the wrong bufferView target")
            attributes = primitive.get("attributes")
            if not isinstance(attributes, dict):
                continue
            expected_count: int | None = None
            joints_reference: int | None = None
            joints_descriptor: dict[str, Any] | None = None
            for semantic, reference in attributes.items():
                path = f"{prefix}.attributes.{semantic}"
                attribute = descriptor(reference, path)
                if attribute is None:
                    continue
                if expected_count is None:
                    expected_count = attribute["count"]
                elif attribute["count"] != expected_count:
                    errors.append(
                        f"{prefix} vertex attribute accessor counts do not match"
                    )
                if attribute["offset"] % 4 != 0 or attribute["stride"] % 4 != 0:
                    errors.append(f"{path} is not 4-byte vertex aligned")
                vertex_view_accessors.setdefault(
                    attribute["view"], set()
                ).add(reference)
                view = views[attribute["view"]]
                if isinstance(view, dict) and view.get("target") not in (None, 34962):
                    errors.append(f"{path} has the wrong bufferView target")
                contract = attribute_contracts.get(semantic)
                actual = (
                    attribute["componentType"], attribute["type"],
                    attribute["normalized"],
                )
                if contract is not None and actual not in contract:
                    errors.append(
                        f"{path} has a component/type/normalized combination "
                        "unsupported by modern-skeletal-v1"
                    )
                elif contract is not None and _gltf_integer(reference):
                    require_finite(reference, attribute, path)
                    if semantic == "POSITION":
                        require_position_bounds(reference, attribute, path)
                    elif semantic == "NORMAL":
                        require_direction_vectors(
                            reference, attribute, path, tangent=False
                        )
                    elif semantic == "TANGENT":
                        require_direction_vectors(
                            reference, attribute, path, tangent=True
                        )
                    elif semantic == "JOINTS_0":
                        joints_reference = reference
                        joints_descriptor = attribute
                    elif semantic == "WEIGHTS_0":
                        require_positive_weights(reference, attribute, path)
            position_reference = attributes.get("POSITION")
            position = raw_accessor(position_reference)
            if position is not None and (
                "min" not in position or "max" not in position
            ):
                errors.append(f"{prefix}.attributes.POSITION requires min and max")
            if primitive.get("targets"):
                errors.append(
                    f"{prefix} uses morph targets; cache v1 requires a "
                    "non-morphed export"
                )
            instance_skin_counts = mesh_skin_counts.get(mesh_index, set())
            if joints_reference is not None and joints_descriptor is not None:
                if joints_reference not in joint_maxima:
                    joint_maxima[joints_reference] = max(
                        component
                        for value in values(joints_reference, joints_descriptor)
                        for component in value
                    )
                maximum_joint = joint_maxima[joints_reference]
                if not instance_skin_counts or None in instance_skin_counts:
                    errors.append(
                        f"{prefix} has JOINTS_0 but is instantiated without a valid skin"
                    )
                for joint_count in instance_skin_counts:
                    if joint_count is not None and maximum_joint >= joint_count:
                        errors.append(
                            f"{prefix}.attributes.JOINTS_0 exceeds an "
                            "instanced skin palette"
                        )
                        break
            elif any(
                joint_count is not None for joint_count in instance_skin_counts
            ):
                errors.append(
                    f"{prefix} is instantiated with a skin but has no valid JOINTS_0"
                )
            if (
                index_descriptor is not None
                and expected_count is not None
                and _gltf_integer(primitive.get("indices"))
                and index_descriptor["type"] == "SCALAR"
                and index_descriptor["componentType"] in (5121, 5123, 5125)
            ):
                restart = (1 << (
                    GLTF_COMPONENT_BYTES[index_descriptor["componentType"]] * 8
                )) - 1
                invalid_index = False
                for value in values(primitive["indices"], index_descriptor):
                    if value[0] == restart or value[0] >= expected_count:
                        invalid_index = True
                        break
                if invalid_index:
                    errors.append(
                        f"{prefix}.indices contains primitive restart or an "
                        "out-of-range vertex index"
                    )
    for view_index, references in vertex_view_accessors.items():
        view = views[view_index]
        if (
            len(references) > 1
            and isinstance(view, dict)
            and "byteStride" not in view
        ):
            errors.append(
                f"bufferViews[{view_index}] is shared by vertex attributes "
                "but has no byteStride"
            )

    for image_index, image in enumerate(_array(document, "images")):
        if not isinstance(image, dict):
            errors.append(f"images[{image_index}] must be an object")
            continue
        if "uri" in image:
            continue
        view_index = image.get("bufferView")
        if not _gltf_integer(view_index) or view_index >= len(views):
            errors.append(f"images[{image_index}].bufferView is invalid")
            continue
        view = views[view_index]
        if not isinstance(view, dict):
            continue
        if "byteStride" in view or "target" in view:
            errors.append(
                f"images[{image_index}] bufferView must not define byteStride "
                "or target"
            )
        if image.get("mimeType") != "image/png":
            errors.append(
                f"images[{image_index}].mimeType must be 'image/png' for "
                "modern-skeletal-v1"
            )
            continue
        offset = view.get("byteOffset", 0)
        length = view.get("byteLength")
        if (
            _gltf_integer(offset) and _gltf_integer(length, 1)
            and offset + length <= len(binary)
        ):
            try:
                _inspect_texture_png(
                    memoryview(binary)[offset:offset + length], image_index
                )
            except ProbeError as exc:
                errors.append(str(exc))

    for skin_index, skin in enumerate(skins):
        if not isinstance(skin, dict):
            errors.append(f"skins[{skin_index}] must be an object")
            continue
        joints = skin.get("joints")
        if (
            not isinstance(joints, list) or not joints
            or any(not _gltf_integer(joint) or joint >= len(nodes) for joint in joints)
            or len(set(joints)) != len(joints)
        ):
            errors.append(f"skins[{skin_index}].joints must be unique valid nodes")
            continue
        if "inverseBindMatrices" in skin:
            inverse = descriptor(
                skin.get("inverseBindMatrices"),
                f"skins[{skin_index}].inverseBindMatrices",
            )
            if inverse is not None:
                if (
                    inverse["componentType"] != 5126
                    or inverse["type"] != "MAT4"
                    or inverse["normalized"]
                    or inverse["count"] != len(joints)
                ):
                    errors.append(
                        f"skins[{skin_index}].inverseBindMatrices must be a "
                        "FLOAT MAT4 accessor matching the joint count"
                    )
                view = views[inverse["view"]]
                if isinstance(view, dict) and "byteStride" in view:
                    errors.append(
                        f"skins[{skin_index}].inverseBindMatrices must be tightly packed"
                    )
                if _gltf_integer(skin.get("inverseBindMatrices")):
                    require_finite(
                        skin["inverseBindMatrices"], inverse,
                        f"skins[{skin_index}].inverseBindMatrices",
                    )

    for animation_index, animation in enumerate(_array(document, "animations")):
        if not isinstance(animation, dict):
            errors.append(f"animations[{animation_index}] must be an object")
            continue
        samplers = animation.get("samplers")
        channels = animation.get("channels")
        if not isinstance(samplers, list) or not samplers:
            errors.append(f"animations[{animation_index}].samplers must be non-empty")
            continue
        if not isinstance(channels, list) or not channels:
            errors.append(f"animations[{animation_index}].channels must be non-empty")
            continue
        validated_samplers: list[tuple[dict[str, Any], dict[str, Any], str] | None] = []
        for sampler_index, sampler in enumerate(samplers):
            prefix = f"animations[{animation_index}].samplers[{sampler_index}]"
            if not isinstance(sampler, dict):
                errors.append(f"{prefix} must be an object")
                validated_samplers.append(None)
                continue
            input_descriptor = descriptor(sampler.get("input"), f"{prefix}.input")
            output_descriptor = descriptor(sampler.get("output"), f"{prefix}.output")
            interpolation = sampler.get("interpolation", "LINEAR")
            if interpolation not in ("LINEAR", "STEP", "CUBICSPLINE"):
                errors.append(f"{prefix}.interpolation is unsupported")
            if input_descriptor is not None:
                if (
                    input_descriptor["componentType"] != 5126
                    or input_descriptor["type"] != "SCALAR"
                    or input_descriptor["normalized"]
                ):
                    errors.append(f"{prefix}.input must be an unnormalized FLOAT SCALAR")
                raw_input = raw_accessor(sampler.get("input"))
                if raw_input is not None and (
                    "min" not in raw_input or "max" not in raw_input
                ):
                    errors.append(f"{prefix}.input requires min and max")
                if _gltf_integer(sampler.get("input")):
                    require_animation_input(
                        sampler["input"], input_descriptor, f"{prefix}.input"
                    )
                view = views[input_descriptor["view"]]
                if isinstance(view, dict) and "byteStride" in view:
                    errors.append(f"{prefix}.input must be tightly packed")
            if output_descriptor is not None:
                if output_descriptor["componentType"] != 5126 or output_descriptor["normalized"]:
                    errors.append(f"{prefix}.output must use unnormalized FLOAT components")
                elif _gltf_integer(sampler.get("output")):
                    require_finite(
                        sampler["output"], output_descriptor, f"{prefix}.output"
                    )
                view = views[output_descriptor["view"]]
                if isinstance(view, dict) and "byteStride" in view:
                    errors.append(f"{prefix}.output must be tightly packed")
            validated_samplers.append(
                (input_descriptor, output_descriptor, interpolation)
                if input_descriptor is not None and output_descriptor is not None
                else None
            )
        seen_targets: set[tuple[int, str]] = set()
        for channel_index, channel in enumerate(channels):
            prefix = f"animations[{animation_index}].channels[{channel_index}]"
            if not isinstance(channel, dict):
                errors.append(f"{prefix} must be an object")
                continue
            sampler_index = channel.get("sampler")
            target = channel.get("target")
            if (
                not _gltf_integer(sampler_index)
                or sampler_index >= len(validated_samplers)
                or validated_samplers[sampler_index] is None
            ):
                errors.append(f"{prefix}.sampler is invalid")
                continue
            if not isinstance(target, dict):
                errors.append(f"{prefix}.target must be an object")
                continue
            node = target.get("node")
            path = target.get("path")
            if not _gltf_integer(node) or node >= len(nodes):
                errors.append(f"{prefix}.target.node is invalid")
                continue
            if path not in ("translation", "rotation", "scale"):
                errors.append(
                    f"{prefix}.target.path is unsupported by modern-skeletal-v1"
                )
                continue
            if (node, path) in seen_targets:
                errors.append(f"{prefix} duplicates an animation target")
            seen_targets.add((node, path))
            input_descriptor, output_descriptor, interpolation = (
                validated_samplers[sampler_index]
            )
            expected_type = "VEC4" if path == "rotation" else "VEC3"
            if output_descriptor["type"] != expected_type:
                errors.append(
                    f"{prefix} output accessor must use {expected_type} for {path}"
                )
            multiplier = 3 if interpolation == "CUBICSPLINE" else 1
            if output_descriptor["count"] != input_descriptor["count"] * multiplier:
                errors.append(f"{prefix} input/output accessor counts do not match")
            output_reference = samplers[sampler_index].get("output")
            if (
                path == "rotation" and _gltf_integer(output_reference)
                and output_descriptor["type"] == "VEC4"
                and (output_reference, interpolation) not in rotation_outputs
            ):
                rotation_outputs.add((output_reference, interpolation))
                for value_index, value in enumerate(
                    values(output_reference, output_descriptor)
                ):
                    if interpolation == "CUBICSPLINE" and value_index % 3 != 1:
                        continue
                    length_squared = sum(component * component for component in value)
                    if (
                        not math.isfinite(length_squared)
                        or abs(length_squared - 1.0) > 2.0e-5
                    ):
                        errors.append(
                            f"{prefix} rotation key is not a unit quaternion"
                        )
                        break


def _matrix_multiply(left: list[float], right: list[float]) -> list[float]:
    return [sum(left[inner * 4 + row] * right[column * 4 + inner]
                for inner in range(4))
            for column in range(4) for row in range(4)]


def _node_matrix(node: dict[str, Any]) -> list[float]:
    if "matrix" in node:
        values = node["matrix"]
        if (not isinstance(values, list) or len(values) != 16 or
                not all(_gltf_number(value) for value in values)):
            raise ProbeError("node matrix must contain 16 finite numbers")
        return [float(value) for value in values]
    translation = node.get("translation", [0.0, 0.0, 0.0])
    rotation = node.get("rotation", [0.0, 0.0, 0.0, 1.0])
    scale = node.get("scale", [1.0, 1.0, 1.0])
    if (not all(isinstance(value, list) for value in (translation, rotation, scale)) or
            len(translation) != 3 or len(rotation) != 4 or len(scale) != 3 or
            not all(_gltf_number(component)
                    for value in (translation, rotation, scale) for component in value)):
        raise ProbeError("node TRS is malformed or non-finite")
    x, y, z, w = (float(value) for value in rotation)
    length = math.sqrt(x * x + y * y + z * z + w * w)
    if length < 1.0e-12:
        raise ProbeError("node quaternion has zero length")
    x, y, z, w = x / length, y / length, z / length, w / length
    sx, sy, sz = (float(value) for value in scale)
    return [
        (1 - 2 * (y * y + z * z)) * sx,
        (2 * (x * y + z * w)) * sx,
        (2 * (x * z - y * w)) * sx, 0.0,
        (2 * (x * y - z * w)) * sy,
        (1 - 2 * (x * x + z * z)) * sy,
        (2 * (y * z + x * w)) * sy, 0.0,
        (2 * (x * z + y * w)) * sz,
        (2 * (y * z - x * w)) * sz,
        (1 - 2 * (x * x + y * y)) * sz, 0.0,
        float(translation[0]), float(translation[1]), float(translation[2]), 1.0,
    ]


def _world_bounds(document: dict[str, Any],
                  mesh_bounds: list[tuple[list[float], list[float]] | None],
                  errors: list[str]) -> tuple[list[float] | None, list[float] | None]:
    nodes = _array(document, "nodes")
    scenes = _array(document, "scenes")
    scene_index = document.get("scene", 0)
    if not isinstance(scene_index, int) or not 0 <= scene_index < len(scenes):
        errors.append("default scene index is invalid")
        return None, None
    scene = scenes[scene_index]
    roots = scene.get("nodes", []) if isinstance(scene, dict) else []
    if not isinstance(roots, list):
        errors.append("default scene node list is invalid")
        return None, None
    identity = [1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0,
                0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0]
    output_min: list[float] | None = None
    output_max: list[float] | None = None

    def visit(index: Any, parent: list[float], ancestry: set[int]) -> None:
        nonlocal output_min, output_max
        if not isinstance(index, int) or not 0 <= index < len(nodes):
            errors.append("scene graph references an invalid node")
            return
        if index in ancestry:
            errors.append("scene graph contains a node cycle")
            return
        node = nodes[index]
        if not isinstance(node, dict):
            errors.append(f"nodes[{index}] must be an object")
            return
        try:
            world = _matrix_multiply(parent, _node_matrix(node))
        except ProbeError as exc:
            errors.append(f"nodes[{index}]: {exc}")
            return
        if any(_gltf_float32(component) is None for component in world):
            errors.append(
                f"nodes[{index}] world transform exceeds finite FLOAT range"
            )
            return
        mesh_index = node.get("mesh")
        if isinstance(mesh_index, int) and 0 <= mesh_index < len(mesh_bounds):
            bounds = mesh_bounds[mesh_index]
            if bounds is not None:
                local_min, local_max = bounds
                for x in (local_min[0], local_max[0]):
                    for y in (local_min[1], local_max[1]):
                        for z in (local_min[2], local_max[2]):
                            point = [
                                world[0] * x + world[4] * y + world[8] * z + world[12],
                                world[1] * x + world[5] * y + world[9] * z + world[13],
                                world[2] * x + world[6] * y + world[10] * z + world[14],
                            ]
                            if any(
                                _gltf_float32(component) is None
                                for component in point
                            ):
                                errors.append(
                                    f"nodes[{index}] world bounds exceed finite FLOAT range"
                                )
                                return
                            if output_min is None:
                                output_min, output_max = list(point), list(point)
                            else:
                                output_min = [min(output_min[i], point[i]) for i in range(3)]
                                output_max = [max(output_max[i], point[i]) for i in range(3)]
        next_ancestry = ancestry | {index}
        children = node.get("children", [])
        if not isinstance(children, list):
            errors.append(f"nodes[{index}].children must be an array")
            return
        for child in children:
            visit(child, world, next_ancestry)

    for root_index in roots:
        visit(root_index, identity, set())
    return output_min, output_max


def inspect_glb_bytes(data: bytes, require_character: bool = False) -> dict[str, Any]:
    document, bin_chunk = parse_glb(data)
    errors = _BoundedDiagnostics()
    warnings: list[str] = []
    asset = document.get("asset")
    if not isinstance(asset, dict) or asset.get("version") != "2.0":
        errors.append("asset.version must be exactly '2.0'")

    buffers = _array(document, "buffers")
    for index, buffer in enumerate(buffers):
        if isinstance(buffer, dict) and "uri" in buffer:
            errors.append(f"buffers[{index}] is external; source GLB must be self-contained")
    for index, image in enumerate(_array(document, "images")):
        if isinstance(image, dict) and "uri" in image:
            errors.append(f"images[{index}] is external; source GLB must be self-contained")

    accessor_descriptors = _validate_glb_storage(document, bin_chunk, errors)
    _validate_glb_accessor_usage(
        document, accessor_descriptors, bin_chunk, errors
    )
    _validate_glb_scene_and_materials(document, errors)

    required_extension_list = document.get("extensionsRequired", [])
    if not isinstance(required_extension_list, list) or not all(
        isinstance(extension, str) for extension in required_extension_list
    ):
        errors.append("extensionsRequired must be an array of strings")
        required_extension_list = []
    elif len(set(required_extension_list)) != len(required_extension_list):
        errors.append("extensionsRequired contains duplicates")
    used_extension_list = document.get("extensionsUsed", [])
    if not isinstance(used_extension_list, list) or not all(
        isinstance(extension, str) for extension in used_extension_list
    ):
        errors.append("extensionsUsed must be an array of strings")
        used_extension_list = []
    elif len(set(used_extension_list)) != len(used_extension_list):
        errors.append("extensionsUsed contains duplicates")
    required_extensions = set(required_extension_list)
    if not required_extensions.issubset(set(used_extension_list)):
        errors.append("extensionsRequired must be a subset of extensionsUsed")
    unknown_required = sorted(required_extensions - SUPPORTED_REQUIRED_EXTENSIONS)
    if unknown_required:
        errors.append("unsupported required extensions: " + ", ".join(unknown_required))

    vertex_count = 0
    triangle_count = 0
    primitive_count = 0
    primitive_materials: set[int] = set()
    bbox_min: list[float] | None = None
    bbox_max: list[float] | None = None
    meshes = _array(document, "meshes")
    mesh_bounds: list[tuple[list[float], list[float]] | None] = [None] * len(meshes)
    skinned_primitive_count = 0
    for mesh_index, mesh in enumerate(meshes):
        mesh_min: list[float] | None = None
        mesh_max: list[float] | None = None
        if not isinstance(mesh, dict):
            errors.append(f"meshes[{mesh_index}] must be an object")
            continue
        primitives = mesh.get("primitives", [])
        if not isinstance(primitives, list):
            continue
        for primitive_index, primitive in enumerate(primitives):
            prefix = f"meshes[{mesh_index}].primitives[{primitive_index}]"
            if not isinstance(primitive, dict):
                continue
            primitive_count += 1
            if primitive.get("mode", 4) != 4:
                errors.append(f"{prefix} must use TRIANGLES mode")
            if "indices" not in primitive:
                errors.append(f"{prefix} must be indexed")
            index_accessor = _accessor(document, primitive.get("indices"))
            if index_accessor is not None:
                raw_index_count = index_accessor.get("count")
                index_count = (
                    raw_index_count if _gltf_integer(raw_index_count) else 0
                )
                if index_count % 3:
                    errors.append(f"{prefix} index count is not divisible by three")
                triangle_count += index_count // 3
            attributes = primitive.get("attributes", {})
            if not isinstance(attributes, dict):
                errors.append(f"{prefix}.attributes must be an object")
                continue
            for semantic in ("POSITION", "NORMAL", "TEXCOORD_0"):
                if semantic not in attributes:
                    errors.append(f"{prefix} is missing {semantic}")
            joints = "JOINTS_0" in attributes
            weights = "WEIGHTS_0" in attributes
            if joints != weights:
                errors.append(f"{prefix} must provide JOINTS_0 and WEIGHTS_0 together")
            if joints:
                skinned_primitive_count += 1
            if "JOINTS_1" in attributes or "WEIGHTS_1" in attributes:
                errors.append(f"{prefix} exceeds the v1 four-influence skinning contract")
            position = _accessor(document, attributes.get("POSITION"))
            if position is not None:
                raw_position_count = position.get("count")
                vertex_count += (
                    raw_position_count if _gltf_integer(raw_position_count) else 0
                )
                pmin, pmax = position.get("min"), position.get("max")
                if (isinstance(pmin, list) and isinstance(pmax, list) and
                        len(pmin) == len(pmax) == 3 and
                        all(_gltf_number(value) for value in pmin + pmax)):
                    if mesh_min is None:
                        mesh_min, mesh_max = list(pmin), list(pmax)
                    else:
                        mesh_min = [min(mesh_min[i], pmin[i]) for i in range(3)]
                        mesh_max = [max(mesh_max[i], pmax[i]) for i in range(3)]
                    if bbox_min is None:
                        bbox_min, bbox_max = list(pmin), list(pmax)
                    else:
                        bbox_min = [min(bbox_min[i], pmin[i]) for i in range(3)]
                        bbox_max = [max(bbox_max[i], pmax[i]) for i in range(3)]
            if isinstance(primitive.get("material"), int):
                primitive_materials.add(primitive["material"])
        mesh_bounds[mesh_index] = ((mesh_min, mesh_max)
                                   if mesh_min is not None and mesh_max is not None
                                   else None)

    world_bbox_min, world_bbox_max = _world_bounds(document, mesh_bounds, errors)

    skins = _array(document, "skins")
    max_joints = max((
        len(skin.get("joints", []))
        for skin in skins
        if isinstance(skin, dict) and isinstance(skin.get("joints", []), list)
    ), default=0)
    animations: list[dict[str, Any]] = []
    for index, animation in enumerate(_array(document, "animations")):
        if not isinstance(animation, dict):
            continue
        duration = 0.0
        samplers = animation.get("samplers", [])
        if not isinstance(samplers, list):
            samplers = []
        for sampler in samplers:
            accessor = _accessor(document, sampler.get("input") if isinstance(sampler, dict) else None)
            if (
                accessor and isinstance(accessor.get("max"), list)
                and accessor["max"] and _gltf_number(accessor["max"][0])
            ):
                duration = max(duration, float(accessor["max"][0]))
        channels = animation.get("channels", [])
        source_name = animation.get("name")
        animations.append({
            "index": index,
            "name": (
                source_name if isinstance(source_name, str) and source_name
                else f"animation_{index}"
            ),
            "channels": len(channels) if isinstance(channels, list) else 0,
            "duration_seconds": duration,
        })

    if vertex_count > MAX_VERTICES:
        errors.append(f"vertex count {vertex_count} exceeds the runtime ceiling {MAX_VERTICES}")
    if triangle_count > MAX_TRIANGLES:
        errors.append(f"triangle count {triangle_count} exceeds the runtime ceiling {MAX_TRIANGLES}")
    if len(_array(document, "materials")) > MAX_MATERIALS:
        errors.append(f"material count exceeds the runtime ceiling {MAX_MATERIALS}")
    if max_joints > MAX_JOINTS:
        errors.append(f"joint count {max_joints} exceeds the renderer ceiling {MAX_JOINTS}")
    if world_bbox_min is not None and world_bbox_max is not None:
        height = world_bbox_max[1] - world_bbox_min[1]
        if height < 0.25 or height > 4.0:
            warnings.append(
                f"scene-world Y extent is {height:.6g} meters; verify root transforms, scale, and seat placement"
            )

    if require_character:
        if not skins or not skinned_primitive_count:
            errors.append("character package requires a skin and skinned primitives")
        if not animations:
            warnings.append(
                "character has no source animation; use the explicit bind-pose fallback and review humanoid reference motion before enabling"
            )
        elif not any(animation["duration_seconds"] > 0.0 for animation in animations):
            warnings.append(
                "character source animations have no positive duration; review the explicit bind/reference-motion path"
            )
        if world_bbox_min is None or world_bbox_max is None:
            errors.append("character package requires finite scene-world bounds")
        else:
            height = world_bbox_max[1] - world_bbox_min[1]
            if not math.isfinite(height) or height <= 1.0e-6:
                errors.append(
                    "character scene-world height is too small to calibrate"
                )

    errors.finish()

    return {
        "format": "glb-2.0",
        "sha256": _sha256(data),
        "bytes": len(data),
        "self_contained": not any("external" in error for error in errors),
        "generator": asset.get("generator") if isinstance(asset, dict) else None,
        "extensions_used": used_extension_list,
        "extensions_required": required_extension_list,
        "scene_count": len(_array(document, "scenes")),
        "mesh_count": len(_array(document, "meshes")),
        "primitive_count": primitive_count,
        "vertex_count": vertex_count,
        "triangle_count": triangle_count,
        "material_count": len(_array(document, "materials")),
        "texture_count": len(_array(document, "textures")),
        "skin_count": len(skins),
        "max_joints": max_joints,
        "animations": animations,
        "bbox_min": world_bbox_min,
        "bbox_max": world_bbox_max,
        "mesh_local_bbox_min": bbox_min,
        "mesh_local_bbox_max": bbox_max,
        "binary_bytes": len(bin_chunk or b""),
        "errors": errors,
        "warnings": warnings,
        "character_ready": require_character and not errors,
    }


def inspect_glb(path: Path, require_character: bool = False) -> dict[str, Any]:
    return inspect_glb_bytes(
        _read_bounded(path, MAX_INPUT_BYTES, "GLB input"),
        require_character=require_character,
    )


def validate_manifest(manifest: dict[str, Any], glb_report: dict[str, Any]) -> list[str]:
    errors = _manifest_unicode_errors(manifest)
    allowed = {
        "schema", "id", "display_name", "renderer_profile", "license",
        "animations", "gameplay", "presentation", "sockets", "model",
        "model_sha256", "license_file", "identity", "rig",
    }
    for field in sorted(set(manifest) - allowed):
        errors.append(f"manifest contains unknown field {field!r}")
    schema = manifest.get("schema")
    if schema not in PACKAGE_SCHEMAS:
        errors.append(
            "manifest.schema must be one of: " + ", ".join(sorted(PACKAGE_SCHEMAS))
        )
    package_id = manifest.get("id")
    if not isinstance(package_id, str) or not ID_RE.fullmatch(package_id):
        errors.append("manifest.id must be a 2-64 character lowercase slug")
    if (not isinstance(manifest.get("display_name"), str) or
            not manifest["display_name"].strip() or
            not _bounded_printable_text(manifest["display_name"], 96)):
        errors.append(
            "manifest.display_name must be 1-96 printable UTF-8 bytes"
        )
    if manifest.get("renderer_profile") != "modern-skeletal-v1":
        errors.append("manifest.renderer_profile must be 'modern-skeletal-v1'")
    identity = manifest.get("identity")
    if schema in (PACKAGE_SCHEMA_V3, PACKAGE_SCHEMA_V4):
        if not isinstance(identity, dict):
            errors.append("manifest.identity object is required for v3/v4")
        else:
            for field in sorted(set(identity) - {
                "portrait_file", "portrait_sha256", "minimap_rgb",
                "short_name", "narration_name", "sort_label",
            }):
                errors.append(f"manifest.identity contains unknown field {field!r}")
            if identity.get("portrait_file") != "portrait.png":
                errors.append("manifest.identity.portrait_file must be 'portrait.png'")
            portrait_digest = identity.get("portrait_sha256")
            if (
                not isinstance(portrait_digest, str)
                or re.fullmatch(r"[0-9a-f]{64}", portrait_digest) is None
            ):
                errors.append("manifest.identity.portrait_sha256 must be lowercase SHA-256")
            minimap_rgb = identity.get("minimap_rgb")
            if (
                not isinstance(minimap_rgb, list)
                or len(minimap_rgb) != 3
                or any(
                    isinstance(component, bool)
                    or not isinstance(component, int)
                    or not 0 <= component <= 255
                    for component in minimap_rgb
                )
            ):
                errors.append("manifest.identity.minimap_rgb must contain three bytes")
            for field, maximum in (
                ("short_name", 96),
                ("narration_name", 96),
                ("sort_label", 96),
            ):
                value = identity.get(field, manifest.get("display_name"))
                if (not isinstance(value, str) or not value.strip() or
                        not _bounded_printable_text(value, maximum)):
                    errors.append(
                        f"manifest.identity.{field} must be 1-{maximum} "
                        "printable UTF-8 bytes when present"
                    )
    elif identity is not None:
        errors.append("manifest.identity requires mdkr-character-source-v3 or v4")
    rig = manifest.get("rig")
    if schema == PACKAGE_SCHEMA_V4:
        if not isinstance(rig, dict):
            errors.append("manifest.rig object is required for v4")
        else:
            for field in sorted(set(rig) - {"mode", "reviewed", "roles"}):
                errors.append(f"manifest.rig contains unknown field {field!r}")
            mode = rig.get("mode")
            if mode not in RIG_MODES:
                errors.append(
                    "manifest.rig.mode must be authored-clips-only or humanoid-retarget-v1"
                )
            if not isinstance(rig.get("reviewed"), bool):
                errors.append("manifest.rig.reviewed must be a boolean")
            roles = rig.get("roles")
            if not isinstance(roles, dict):
                errors.append("manifest.rig.roles must be an object")
            else:
                unknown_roles = sorted(set(roles) - HUMANOID_ROLE_SET)
                for role in unknown_roles:
                    errors.append(f"manifest.rig.roles contains unknown role {role!r}")
                if len(roles) > len(HUMANOID_ROLES):
                    errors.append("manifest.rig.roles exceeds the humanoid role contract")
                mapped_nodes: list[str] = []
                for role, mapping in roles.items():
                    if role not in HUMANOID_ROLE_SET:
                        continue
                    if not isinstance(mapping, dict):
                        errors.append(f"manifest.rig.roles.{role} must be an object")
                        continue
                    for field in sorted(set(mapping) - {
                        "node", "inferred", "confidence", "rest_rotation_xyzw",
                        "bend_axis",
                    }):
                        errors.append(
                            f"manifest.rig.roles.{role} contains unknown field {field!r}"
                        )
                    node = mapping.get("node")
                    if not isinstance(node, str) or not node.strip():
                        errors.append(f"manifest.rig.roles.{role}.node is required")
                    else:
                        mapped_nodes.append(node)
                    if not isinstance(mapping.get("inferred"), bool):
                        errors.append(
                            f"manifest.rig.roles.{role}.inferred must be a boolean"
                        )
                    confidence = mapping.get("confidence")
                    if (
                        isinstance(confidence, bool)
                        or not isinstance(confidence, (int, float))
                        or not math.isfinite(float(confidence))
                        or not 0.0 <= float(confidence) <= 1.0
                    ):
                        errors.append(
                            f"manifest.rig.roles.{role}.confidence must be between 0 and 1"
                        )
                    rotation = mapping.get(
                        "rest_rotation_xyzw", [0.0, 0.0, 0.0, 1.0]
                    )
                    if (
                        not isinstance(rotation, list)
                        or len(rotation) != 4
                        or any(
                            isinstance(value, bool)
                            or not isinstance(value, (int, float))
                            or not math.isfinite(float(value))
                            for value in rotation
                        )
                        or not 0.999 <= sum(float(value) ** 2 for value in rotation) <= 1.001
                    ):
                        errors.append(
                            f"manifest.rig.roles.{role}.rest_rotation_xyzw must be a normalized quaternion"
                        )
                    bend = mapping.get("bend_axis", [0.0, 0.0, 0.0])
                    if (
                        not isinstance(bend, list)
                        or len(bend) != 3
                        or any(
                            isinstance(value, bool)
                            or not isinstance(value, (int, float))
                            or not math.isfinite(float(value))
                            for value in bend
                        )
                    ):
                        errors.append(
                            f"manifest.rig.roles.{role}.bend_axis must contain three finite values"
                        )
                    else:
                        bend_length = math.sqrt(sum(float(value) ** 2 for value in bend))
                        if bend_length != 0.0 and not 0.999 <= bend_length <= 1.001:
                            errors.append(
                                f"manifest.rig.roles.{role}.bend_axis must be zero or normalized"
                            )
                if len(mapped_nodes) != len(set(mapped_nodes)):
                    errors.append("manifest.rig.roles must map to distinct nodes")
                if mode == "humanoid-retarget-v1":
                    missing_roles = sorted(HUMANOID_ROLE_SET - set(roles))
                    if missing_roles:
                        errors.append(
                            "manifest.rig.roles is missing required humanoid roles: "
                            + ", ".join(missing_roles)
                        )
    elif rig is not None:
        errors.append("manifest.rig requires mdkr-character-source-v4")
    license_info = manifest.get("license")
    if not isinstance(license_info, dict):
        errors.append("manifest.license object is required")
    else:
        for field in sorted(set(license_info) - {"spdx", "attribution", "source_url"}):
            errors.append(f"manifest.license contains unknown field {field!r}")
        for field in ("spdx", "attribution", "source_url"):
            if not isinstance(license_info.get(field), str) or not license_info[field].strip():
                errors.append(f"manifest.license.{field} is required")
        limits = {"spdx": 128, "attribution": 256, "source_url": 2048}
        for field, maximum in limits.items():
            value = license_info.get(field)
            if isinstance(value, str) and not _bounded_printable_text(
                    value, maximum):
                errors.append(
                    f"manifest.license.{field} exceeds its bounded printable profile"
                )
        spdx = license_info.get("spdx")
        if (
            isinstance(spdx, str)
            and spdx.strip()
            and _bounded_printable_text(spdx, limits["spdx"])
        ):
            spdx_error = validate_spdx_expression(spdx)
            if spdx_error is not None:
                errors.append(
                    "manifest.license.spdx is not a structurally valid SPDX expression: "
                    + spdx_error
                )
    animation_info = manifest.get("animations")
    clip_names = {animation["name"] for animation in glb_report.get("animations", [])}
    if not isinstance(animation_info, dict) or not isinstance(animation_info.get("fallback"), str):
        errors.append("manifest.animations.fallback is required")
    elif (animation_info["fallback"] != BIND_POSE_FALLBACK and
          animation_info["fallback"] not in clip_names):
        errors.append("manifest.animations.fallback does not name a GLB animation")
    states = animation_info.get("states", {}) if isinstance(animation_info, dict) else {}
    disabled_states = (
        animation_info.get("disabled_states", [])
        if isinstance(animation_info, dict) else []
    )
    if isinstance(animation_info, dict):
        for field in sorted(
                set(animation_info) - {"fallback", "states", "disabled_states"}):
            errors.append(f"manifest.animations contains unknown field {field!r}")
    if not isinstance(states, dict):
        errors.append("manifest.animations.states must be an object")
    else:
        for semantic, clip in states.items():
            if semantic == "fallback":
                errors.append(
                    "animation mapping 'fallback' is reserved; use "
                    "manifest.animations.fallback instead"
                )
            elif (not isinstance(semantic, str) or not SEMANTIC_RE.fullmatch(semantic) or
                    not isinstance(clip, str) or clip not in clip_names):
                errors.append(f"animation mapping {semantic!r} does not name a GLB animation")
        if len(states) > 64:
            errors.append("manifest.animations.states exceeds 64 mappings")
    if (
        not isinstance(disabled_states, list)
        or any(not isinstance(semantic, str) for semantic in disabled_states)
        or len(set(disabled_states)) != len(disabled_states)
        or len(disabled_states) > 64
    ):
        errors.append(
            "manifest.animations.disabled_states must contain at most 64 unique semantic names"
        )
    elif isinstance(states, dict):
        unsupported_disabled = [
            semantic for semantic in disabled_states
            if semantic not in DISABLEABLE_SEMANTICS
        ]
        if unsupported_disabled:
            errors.append(
                "manifest.animations.disabled_states contains unsupported "
                "engine semantics: " + ", ".join(unsupported_disabled)
            )
        unknown_disabled = [
            semantic for semantic in disabled_states if semantic not in states
        ]
        if unknown_disabled:
            errors.append(
                "manifest.animations.disabled_states names unmapped states: "
                + ", ".join(unknown_disabled)
            )

    gameplay = manifest.get("gameplay")
    if not isinstance(gameplay, dict):
        errors.append("manifest.gameplay object is required")
    else:
        for field in sorted(set(gameplay) - {"donor", "vehicles"}):
            errors.append(f"manifest.gameplay contains unknown field {field!r}")
        donor = gameplay.get("donor")
        if donor not in GAMEPLAY_DONORS:
            errors.append(
                "manifest.gameplay.donor must name a built-in racer: "
                + ", ".join(sorted(GAMEPLAY_DONORS))
            )
        vehicles = gameplay.get("vehicles")
        if not isinstance(vehicles, list) or not vehicles:
            errors.append("manifest.gameplay.vehicles must be a non-empty array")
        elif any(vehicle not in VEHICLE_NAMES for vehicle in vehicles):
            errors.append("manifest.gameplay.vehicles contains an unsupported vehicle")
        elif len(set(vehicles)) != len(vehicles):
            errors.append("manifest.gameplay.vehicles must not contain duplicates")

    presentation = manifest.get("presentation")
    if not isinstance(presentation, dict):
        errors.append("manifest.presentation object is required")
    elif schema in (PACKAGE_SCHEMA, PACKAGE_SCHEMA_V3, PACKAGE_SCHEMA_V4):
        for field in sorted(set(presentation) - {
            "source_forward", "target_height_m", "contexts", "lod_bias"
        }):
            errors.append(f"manifest.presentation contains unknown field {field!r}")
        if presentation.get("source_forward") not in SOURCE_FORWARD_AXES:
            errors.append(
                "manifest.presentation.source_forward must be +z, -z, +x, or -x"
            )
        target_height = presentation.get("target_height_m")
        if (
            isinstance(target_height, bool)
            or not isinstance(target_height, (int, float))
            or not math.isfinite(float(target_height))
            or not 0.1 <= float(target_height) <= 10.0
        ):
            errors.append(
                "manifest.presentation.target_height_m must be between 0.1 and 10"
            )
        contexts = presentation.get("contexts")
        required_contexts = {"select"}
        if isinstance(gameplay, dict) and isinstance(gameplay.get("vehicles"), list):
            required_contexts.update(
                vehicle for vehicle in gameplay["vehicles"] if vehicle in VEHICLE_NAMES
            )
        if not isinstance(contexts, dict):
            errors.append("manifest.presentation.contexts object is required")
        else:
            for context in sorted(set(contexts) - PRESENTATION_CONTEXTS):
                errors.append(
                    f"manifest.presentation.contexts contains unknown context {context!r}"
                )
            missing_contexts = sorted(required_contexts - set(contexts))
            if missing_contexts:
                errors.append(
                    "manifest.presentation.contexts is missing: "
                    + ", ".join(missing_contexts)
                )
            for context, adjustment in contexts.items():
                if context not in PRESENTATION_CONTEXTS:
                    continue
                if not isinstance(adjustment, dict):
                    errors.append(
                        f"manifest.presentation.contexts.{context} must be an object"
                    )
                    continue
                for field in sorted(set(adjustment) - {
                    "anchor", "translation_m", "rotation_xyzw", "scale"
                }):
                    errors.append(
                        f"manifest.presentation.contexts.{context} contains unknown field {field!r}"
                    )
                expected_anchor = "ground" if context == "select" else "seat"
                if adjustment.get("anchor") != expected_anchor:
                    errors.append(
                        f"manifest.presentation.contexts.{context}.anchor must be {expected_anchor!r}"
                    )
                translation = adjustment.get("translation_m")
                if (
                    not isinstance(translation, list)
                    or len(translation) != 3
                    or any(
                        isinstance(component, bool)
                        or not isinstance(component, (int, float))
                        or not math.isfinite(float(component))
                        or not -10.0 <= float(component) <= 10.0
                        for component in translation
                    )
                ):
                    errors.append(
                        f"manifest.presentation.contexts.{context}.translation_m must contain three finite values between -10 and 10"
                    )
                rotation = adjustment.get("rotation_xyzw")
                if (
                    not isinstance(rotation, list)
                    or len(rotation) != 4
                    or any(
                        isinstance(component, bool)
                        or not isinstance(component, (int, float))
                        or not math.isfinite(float(component))
                        or not -1.0 <= float(component) <= 1.0
                        for component in rotation
                    )
                ):
                    errors.append(
                        f"manifest.presentation.contexts.{context}.rotation_xyzw must be a bounded quaternion"
                    )
                else:
                    length_squared = sum(float(component) ** 2 for component in rotation)
                    if not 0.999 <= length_squared <= 1.001:
                        errors.append(
                            f"manifest.presentation.contexts.{context}.rotation_xyzw must be normalized"
                        )
                context_scale = adjustment.get("scale", 1.0)
                if (
                    isinstance(context_scale, bool)
                    or not isinstance(context_scale, (int, float))
                    or not math.isfinite(float(context_scale))
                    or not 0.1 <= float(context_scale) <= 5.0
                ):
                    errors.append(
                        f"manifest.presentation.contexts.{context}.scale must be between 0.1 and 5"
                    )
        lod_bias = presentation.get("lod_bias", 0.0)
        if (
            isinstance(lod_bias, bool)
            or not isinstance(lod_bias, (int, float))
            or not math.isfinite(float(lod_bias))
            or not -4.0 <= float(lod_bias) <= 4.0
        ):
            errors.append("manifest.presentation.lod_bias must be between -4 and 4")
    elif schema == PACKAGE_SCHEMA_V1:
        for field in sorted(set(presentation) - {
            "scale", "translation_m", "rotation_xyzw", "lod_bias"
        }):
            errors.append(f"manifest.presentation contains unknown field {field!r}")
        vector_fields = {
            "scale": (3, 0.001, 1000.0),
            "translation_m": (3, -1000.0, 1000.0),
            "rotation_xyzw": (4, -1.0, 1.0),
        }
        for field, (length, minimum, maximum) in vector_fields.items():
            value = presentation.get(field)
            if (
                not isinstance(value, list)
                or len(value) != length
                or any(
                    isinstance(component, bool)
                    or not isinstance(component, (int, float))
                    or not minimum <= float(component) <= maximum
                    for component in value
                )
            ):
                errors.append(
                    f"manifest.presentation.{field} must contain {length} finite bounded numbers"
                )
        rotation = presentation.get("rotation_xyzw")
        if isinstance(rotation, list) and len(rotation) == 4 and all(
            isinstance(component, (int, float)) and not isinstance(component, bool)
            for component in rotation
        ):
            length_squared = sum(float(component) ** 2 for component in rotation)
            if not 0.999 <= length_squared <= 1.001:
                errors.append("manifest.presentation.rotation_xyzw must be normalized")
        lod_bias = presentation.get("lod_bias", 0.0)
        if (
            isinstance(lod_bias, bool)
            or not isinstance(lod_bias, (int, float))
            or not -4.0 <= float(lod_bias) <= 4.0
        ):
            errors.append("manifest.presentation.lod_bias must be between -4 and 4")

    sockets = manifest.get("sockets")
    if not isinstance(sockets, dict):
        errors.append("manifest.sockets object is required")
    else:
        missing_sockets = sorted(REQUIRED_PRESENTATION_SOCKETS - set(sockets))
        if missing_sockets:
            errors.append("manifest.sockets is missing: " + ", ".join(missing_sockets))
        for semantic, node_name in sockets.items():
            if (
                not isinstance(semantic, str)
                or not SEMANTIC_RE.fullmatch(semantic)
                or not isinstance(node_name, str)
                or not node_name.strip()
            ):
                errors.append("manifest.sockets must map non-empty semantic names to node names")
        if len(sockets) > 32:
            errors.append("manifest.sockets exceeds 32 mappings")
    return errors


def _zip_entry(name: str, data: bytes) -> tuple[zipfile.ZipInfo, bytes]:
    info = zipfile.ZipInfo(name, PACKAGE_EPOCH)
    info.compress_type = zipfile.ZIP_STORED
    info.external_attr = 0o100644 << 16
    info.create_system = 3
    return info, data


def build_package(model_path: Path, manifest_path: Path, license_path: Path,
                  output_path: Path, compiled_cache_path: Path | None = None,
                  portrait_path: Path | None = None) -> dict[str, Any]:
    model = _read_bounded(model_path, MAX_INPUT_BYTES, "GLB input")
    report = inspect_glb_bytes(model, require_character=True)
    if report["errors"]:
        raise ProbeError("model is not character-ready: " + "; ".join(report["errors"]))
    try:
        manifest = json_loads_strict(
            _read_bounded(manifest_path, MAX_MANIFEST_BYTES, "manifest"),
            "manifest",
        )
    except (OSError, ProbeError, json.JSONDecodeError) as exc:
        raise ProbeError(f"cannot read manifest: {exc}") from exc
    if not isinstance(manifest, dict):
        raise ProbeError("manifest root must be an object")
    portrait = None
    portrait_report = None
    if manifest.get("schema") in (PACKAGE_SCHEMA_V3, PACKAGE_SCHEMA_V4):
        if portrait_path is None:
            raise ProbeError("v3/v4 packages require --portrait")
        portrait = _read_bounded(portrait_path, MAX_PORTRAIT_BYTES, "portrait.png")
        portrait_report = inspect_portrait_png(portrait)
        identity = manifest.get("identity")
        if not isinstance(identity, dict):
            identity = {}
        identity = dict(identity)
        identity["portrait_file"] = "portrait.png"
        identity["portrait_sha256"] = _sha256(portrait)
        manifest = dict(manifest)
        manifest["identity"] = identity
    elif portrait_path is not None:
        raise ProbeError("--portrait requires mdkr-character-source-v3 or v4")
    manifest_errors = validate_manifest(manifest, report)
    if manifest_errors:
        raise ProbeError("invalid manifest: " + "; ".join(manifest_errors))
    license_text = _read_bounded(license_path, MAX_LICENSE_BYTES, "license text")
    if not license_text.strip():
        raise ProbeError("license text is empty")

    canonical = dict(manifest)
    canonical["model"] = "model.glb"
    canonical["model_sha256"] = report["sha256"]
    canonical["license_file"] = "LICENSE.txt"
    members = {
        "manifest.json": _json_bytes(canonical),
        "model.glb": model,
        "LICENSE.txt": license_text,
    }
    if portrait is not None:
        members["portrait.png"] = portrait
    package_members = package_members_for_schema(manifest["schema"])
    if compiled_cache_path is not None:
        compiled = _read_bounded(compiled_cache_path, MAX_INPUT_BYTES,
                                 "compiled character cache")
        if not _compiled_cache_valid(compiled):
            raise ProbeError("compiled character cache is invalid")
        members["compiled.mdkc"] = compiled
        package_members = package_members_for_schema(manifest["schema"], portable=True)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(output_path, "w", allowZip64=False) as archive:
        for name in package_members:
            info, payload = _zip_entry(name, members[name])
            archive.writestr(info, payload)
    package_bytes = output_path.read_bytes()
    return {
        "output": str(output_path),
        "bytes": len(package_bytes),
        "sha256": _sha256(package_bytes),
        "model": report,
        "portrait": portrait_report,
        "portable": compiled_cache_path is not None,
    }


def _compiled_cache_valid(data: bytes) -> bool:
    if len(data) < 832:
        return False
    magic, version, header_bytes, file_bytes = struct.unpack_from("<4sIIQ", data, 0)
    if magic != b"MDKC" or version != 1 or header_bytes != 832 or file_bytes != len(data):
        return False
    expected_crc = struct.unpack_from("<I", data, 52)[0]
    return (zlib.crc32(data[header_bytes:]) & 0xFFFFFFFF) == expected_crc


def add_compiled_cache(package_path: Path, compiled: bytes,
                       output_path: Path) -> dict[str, Any]:
    verified = verify_package(package_path)
    if not verified["valid"]:
        raise ProbeError("source package is invalid: " + "; ".join(verified["errors"]))
    if not _compiled_cache_valid(compiled):
        raise ProbeError("compiled character cache is invalid")
    with zipfile.ZipFile(package_path) as source:
        manifest = json_loads_strict(source.read("manifest.json"), "manifest")
        source_members = package_members_for_schema(manifest.get("schema"))
        members = {name: source.read(name) for name in source_members}
    members["compiled.mdkc"] = compiled
    output_path.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(output_path, "w", allowZip64=False) as archive:
        for name in package_members_for_schema(manifest.get("schema"), portable=True):
            info, payload = _zip_entry(name, members[name])
            archive.writestr(info, payload)
    package_bytes = output_path.read_bytes()
    return {
        "output": str(output_path),
        "bytes": len(package_bytes),
        "sha256": _sha256(package_bytes),
        "portable": True,
        "model": verified["model"],
    }


def verify_package(path: Path) -> dict[str, Any]:
    data = _read_bounded(path, MAX_INPUT_BYTES, "character package")
    try:
        archive = zipfile.ZipFile(io.BytesIO(data))
    except zipfile.BadZipFile as exc:
        raise ProbeError(f"invalid character package: {exc}") from exc
    infos = archive.infolist()
    names = tuple(info.filename for info in infos)
    valid_member_sets = (
        PACKAGE_MEMBERS, PORTABLE_PACKAGE_MEMBERS,
        PACKAGE_MEMBERS_V3, PORTABLE_PACKAGE_MEMBERS_V3,
    )
    if names not in valid_member_sets:
        raise ProbeError(
            "package members do not match a canonical source or portable layout"
        )
    member_caps = {
        "manifest.json": MAX_MANIFEST_BYTES,
        "model.glb": MAX_INPUT_BYTES,
        "LICENSE.txt": MAX_LICENSE_BYTES,
        "portrait.png": MAX_PORTRAIT_BYTES,
        "compiled.mdkc": MAX_INPUT_BYTES,
    }
    for info in infos:
        safe_name = _safe_archive_name(info.filename)
        if info.is_dir() or _zip_member_is_symlink(info):
            raise ProbeError(f"package member must be a regular file: {safe_name}")
        if info.flag_bits & 1:
            raise ProbeError(f"encrypted package member is unsupported: {safe_name}")
        if info.compress_type != zipfile.ZIP_STORED:
            raise ProbeError(f"package member must use deterministic stored encoding: {safe_name}")
        if info.file_size > member_caps[safe_name]:
            raise ProbeError(f"package member exceeds its size cap: {safe_name}")
    manifest = json_loads_strict(archive.read("manifest.json"), "manifest")
    if not isinstance(manifest, dict):
        raise ProbeError("manifest root must be an object")
    expected_members = package_members_for_schema(
        manifest.get("schema"), portable="compiled.mdkc" in names
    )
    if names != expected_members:
        raise ProbeError("package members do not match manifest.schema")
    model = archive.read("model.glb")
    license_text = archive.read("LICENSE.txt")
    report = inspect_glb_bytes(model, require_character=True)
    errors = list(report["errors"])
    errors.extend(validate_manifest(manifest, report))
    if manifest.get("model_sha256") != _sha256(model):
        errors.append("manifest model_sha256 does not match model.glb")
    portrait_report = None
    if manifest.get("schema") in (PACKAGE_SCHEMA_V3, PACKAGE_SCHEMA_V4):
        portrait = archive.read("portrait.png")
        try:
            portrait_report = inspect_portrait_png(portrait)
        except ProbeError as exc:
            errors.append(str(exc))
        identity = manifest.get("identity", {})
        if (
            isinstance(identity, dict)
            and identity.get("portrait_sha256") != _sha256(portrait)
        ):
            errors.append("manifest portrait_sha256 does not match portrait.png")
    if not license_text.strip():
        errors.append("LICENSE.txt is empty")
    portable = names in (PORTABLE_PACKAGE_MEMBERS, PORTABLE_PACKAGE_MEMBERS_V3)
    if portable and not _compiled_cache_valid(archive.read("compiled.mdkc")):
        errors.append("compiled.mdkc is invalid")
    animation_info = manifest.get("animations", {})
    states = animation_info.get("states", {}) if isinstance(animation_info, dict) else {}
    disabled_states = set(
        animation_info.get("disabled_states", [])
        if isinstance(animation_info, dict)
        and isinstance(animation_info.get("disabled_states", []), list)
        else []
    )
    active_states = {
        semantic: clip for semantic, clip in states.items()
        if semantic not in disabled_states
    } if isinstance(states, dict) else {}
    sockets = manifest.get("sockets", {})
    missing_states = [semantic for semantic in RECOMMENDED_RACE_SEMANTICS
                      if semantic not in active_states]
    missing_select_states = [
        semantic for semantic in RECOMMENDED_SELECT_SEMANTICS
        if semantic not in active_states
    ]
    author_warnings = []
    if manifest.get("schema") == PACKAGE_SCHEMA_V1:
        author_warnings.append(
            "legacy v1 transform has no explicit forward/height/context calibration; regenerate with the v2 manifest wizard"
        )
    if missing_states:
        author_warnings.append(
            f"{len(missing_states)} recommended race animation states use fallback"
        )
    if missing_select_states:
        author_warnings.append(
            f"{len(missing_select_states)} recommended select animation states use fallback"
        )
    if isinstance(sockets, dict) and "hand" not in sockets:
        author_warnings.append("optional hand socket is not authored")
    if not portable:
        author_warnings.append(
            "source-only package needs the developer compiler; prepare a portable package for launcher-only import"
        )
    return {
        "format": manifest.get("schema"),
        "bytes": len(data),
        "sha256": _sha256(data),
        "id": manifest.get("id"),
        "model": report,
        "portrait": portrait_report,
        "errors": errors,
        "valid": not errors,
        "portable": portable,
        "authoring": {
            "calibration_schema": manifest.get("schema"),
            "source_forward": (
                manifest.get("presentation", {}).get("source_forward")
                if isinstance(manifest.get("presentation"), dict) else None
            ),
            "target_height_m": (
                manifest.get("presentation", {}).get("target_height_m")
                if isinstance(manifest.get("presentation"), dict) else None
            ),
            "attachment_contexts": sorted(
                manifest.get("presentation", {}).get("contexts", {})
                if isinstance(manifest.get("presentation"), dict) and
                   isinstance(manifest.get("presentation", {}).get("contexts"), dict)
                else {}
            ),
            "fallback_clip": animation_info.get("fallback")
                if isinstance(animation_info, dict) else None,
            "mapped_recommended_states": [semantic for semantic in RECOMMENDED_RACE_SEMANTICS
                                           if semantic in active_states],
            "missing_recommended_states": missing_states,
            "disabled_states": sorted(disabled_states),
            "mapped_select_states": [
                semantic for semantic in RECOMMENDED_SELECT_SEMANTICS
                if semantic in active_states
            ],
            "missing_select_states": missing_select_states,
            "sockets": sorted(sockets) if isinstance(sockets, dict) else [],
            "warnings": author_warnings,
        },
    }


def _write_report(report: dict[str, Any]) -> None:
    json.dump(report, sys.stdout, indent=2, sort_keys=True)
    sys.stdout.write("\n")


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    probe = sub.add_parser("probe", help="inspect a ZIP, DAE, GLB, or character package")
    probe.add_argument("input", type=Path)
    probe.add_argument("--require-character", action="store_true")
    pack = sub.add_parser("pack", help="build a deterministic .mdkrchar source package")
    pack.add_argument("--model", required=True, type=Path)
    pack.add_argument("--manifest", required=True, type=Path)
    pack.add_argument("--license", required=True, type=Path)
    pack.add_argument("--output", required=True, type=Path)
    pack.add_argument("--compiled-cache", type=Path)
    pack.add_argument(
        "--portrait", type=Path,
        help="square 8-bit RGB/RGBA PNG required by source-v3/v4",
    )
    verify = sub.add_parser("verify", help="verify an existing .mdkrchar source package")
    verify.add_argument("input", type=Path)
    return parser


def main(argv: list[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    try:
        if args.command == "pack":
            report = build_package(
                args.model, args.manifest, args.license, args.output,
                args.compiled_cache, args.portrait,
            )
        elif args.command == "verify":
            report = verify_package(args.input)
        else:
            suffix = args.input.suffix.lower()
            if suffix == ".zip":
                report = inspect_archive(args.input)
            elif suffix == ".dae":
                report = inspect_dae(
                    _read_bounded(args.input, MAX_INPUT_BYTES, "COLLADA input"),
                    args.input.name,
                )
            elif suffix == ".glb":
                report = inspect_glb(args.input, require_character=args.require_character)
            elif suffix == ".mdkrchar":
                report = verify_package(args.input)
            else:
                raise ProbeError(f"unsupported input suffix: {suffix or '<none>'}")
        _write_report(report)
        if report.get("errors") or report.get("blockers") or report.get("valid") is False:
            return 2
        return 0
    except (OSError, ProbeError, zipfile.BadZipFile, json.JSONDecodeError) as exc:
        _write_report({"error": str(exc)})
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
