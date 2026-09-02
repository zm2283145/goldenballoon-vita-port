#!/usr/bin/env python3
"""Regenerate the deterministic custom-character binary fuzz seeds."""

from __future__ import annotations

import struct
import sys
import zlib
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "tests"))

import character_asset_compiler as compiler  # noqa: E402
from test_character_asset_probe import (  # noqa: E402
    KTX2_ETC1S_SRGB,
    KTX2_UASTC_LINEAR_ZSTD,
    make_v5_character,
)


CORPUS = ROOT / "tests" / "fuzz_corpus" / "modern_character_asset"


def with_valid_crc(cache: bytearray) -> bytes:
    struct.pack_into(
        "<I", cache, 52,
        zlib.crc32(cache[compiler.MDKC_HEADER_BYTES:]) & 0xFFFFFFFF,
    )
    return bytes(cache)


def main() -> int:
    CORPUS.mkdir(parents=True, exist_ok=True)
    model, portrait, manifest = make_v5_character()
    compiled, _ = compiler.compile_character(
        model, manifest, bytes(range(32)), portrait,
    )

    # Two valid texture encodings cross both inspect and transcode paths.
    (CORPUS / "valid-etc1s-srgb.ktx2").write_bytes(KTX2_ETC1S_SRGB)
    (CORPUS / "valid-uastc-zstd.ktx2").write_bytes(
        KTX2_UASTC_LINEAR_ZSTD
    )
    (CORPUS / "truncated-level-table.ktx2").write_bytes(
        KTX2_ETC1S_SRGB[:96]
    )

    # The same PNG-only stb_image configuration decodes package textures and
    # portraits. A valid generated portrait reaches full RGBA decode; its
    # truncation anchors the PNG error paths without importing external art.
    (CORPUS / "valid-portrait.png").write_bytes(portrait)
    (CORPUS / "truncated-portrait.png").write_bytes(portrait[:-13])

    # A fully valid compiled character reaches every section parser and its
    # embedded portrait PNG. Truncated and checksum-valid hostile-offset
    # variants keep structural rejection paths in the initial corpus instead
    # of relying on chance mutations.
    (CORPUS / "valid-character.mdkc").write_bytes(compiled)
    (CORPUS / "truncated-character.mdkc").write_bytes(compiled[:-17])
    hostile = bytearray(compiled)
    struct.pack_into("<Q", hostile, 64 + 8, 0xFFFFFFFFFFFFFFF0)
    (CORPUS / "hostile-section-offset.mdkc").write_bytes(
        with_valid_crc(hostile)
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
