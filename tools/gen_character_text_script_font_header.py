#!/usr/bin/env python3
"""Wrap a pinned Noto script face as a reproducible generated C header."""

from __future__ import annotations

import argparse
import hashlib
from pathlib import Path


GOOGLE_FONTS_COMMIT = "ade3d1533e06b2b1462ffcde8e08b129627ca360"
TOOL_COMMIT = "776bf2ab0d61e719e58f2c6d27d109ab5dcf2af1"
TOOL_SHA256 = "8f39e8890cd03c9374be72973f6d95b335d31c8b6862f7b249e9d45ff0daf691"

FACES = {
    "arabic": {
        "label": "Noto Sans Arabic",
        "path": "ofl/notosansarabic/NotoSansArabic[wdth,wght].ttf",
        "source_sha": "63111b5b2e074dd48cc67692e0a2726d86ee94c1c37fe8598257b7b4e87e869e",
        "subset_sha": "7edb9d96053b12dcb08342562419ba66968ba4a386cc76dd8c07073222cb37e7",
        "ranges": "U+0020-007E,U+0300-036F,U+0600-06FF,U+0750-077F,U+0870-089F,U+08A0-08FF,U+2000-206F,U+20A0-20CF,U+FB50-FDFF,U+FE70-FEFF",
        "file": "NotoSansArabic-MDKR.ttf",
        "symbol": "MdkrCharacterTextArabic_compressed_data_base85",
        "payload_sha": "597048882b7643ad9566c331cd62823251721c427e1e2f0c88c566772be291db",
        "guard": "MDKR_GFX_CHARACTER_TEXT_ARABIC_FACE_H",
    },
    "hebrew": {
        "label": "Noto Sans Hebrew",
        "path": "ofl/notosanshebrew/NotoSansHebrew[wdth,wght].ttf",
        "source_sha": "7ef36a2c3593758cdb622e1bdef4f84523e92fbc3ccc667438dd80ff54c2de88",
        "subset_sha": "85ad08e4d4f98be7f146adc504632b495bd3a0221736ca70d1019ba211f81abc",
        "ranges": "U+0020-007E,U+0300-036F,U+0590-05FF,U+FB1D-FB4F,U+2000-206F,U+20A0-20CF",
        "file": "NotoSansHebrew-MDKR.ttf",
        "symbol": "MdkrCharacterTextHebrew_compressed_data_base85",
        "payload_sha": "38d4ff98d1c4169f435a3ed6a770c63b89290566a4dbc503bd254ca689eba781",
        "guard": "MDKR_GFX_CHARACTER_TEXT_HEBREW_FACE_H",
    },
}


def banner(kind: str, face: dict[str, str]) -> str:
    return f"""/*
 * gfx_character_text_{kind}_face.h -- GENERATED FILE, DO NOT EDIT.
 *
 * ROM-independent {face['label']} SemiBold face for bounded custom-character
 * identity shaping. Upstream google/fonts commit:
 *   {GOOGLE_FONTS_COMMIT}
 *   {face['path']}
 *   sha256 {face['source_sha']}
 * Modification: FontTools 4.63.0, wght=600/wdth=100 static instance,
 * no hinting, retaining all applicable OpenType layout features; subset:
 *   {face['ranges']}
 *   sha256 {face['subset_sha']}
 * Compression: Dear ImGui binary_to_compressed_c at {TOOL_COMMIT}
 *   tool sha256 {TOOL_SHA256}
 *   compressed declaration sha256 {face['payload_sha']}
 *
 * Reproduce after downloading and checking the exact upstream file above:
 *   python3 -m venv venv
 *   venv/bin/python -m pip install fonttools==4.63.0
 *   venv/bin/python -c \"from fontTools import ttLib; from fontTools.varLib import instancer; f=ttLib.TTFont('source.ttf'); instancer.instantiateVariableFont(f,{{'wght':600,'wdth':100}},updateFontNames=True).save('static.ttf')\"
 *   venv/bin/pyftsubset static.ttf --unicodes={face['ranges']} \\
 *     --ignore-missing-unicodes --output-file={face['file']} \\
 *     --no-hinting --layout-features='*' --glyph-names \\
 *     --name-IDs='*' --name-languages='*'
 *   binary_to_compressed_c -base85 {face['file']} \\
 *     {face['symbol'].removesuffix('_compressed_data_base85')} > compressed.inc
 *   tools/gen_character_text_script_font_header.py {kind} compressed.inc output.h
 *
 * Licensed under SIL Open Font License 1.1; see lib/fonts/LICENSE.txt.
 */
#ifndef {face['guard']}
#define {face['guard']}
"""


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("face", choices=sorted(FACES))
    parser.add_argument("compressed", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    face = FACES[args.face]
    source = args.compressed.read_text(encoding="utf-8")
    marker = f"static const char {face['symbol']}"
    start = source.find(marker)
    if start < 0 or face["file"] not in source[:start]:
        raise SystemExit("unexpected binary_to_compressed_c payload")
    payload = source[start:].split("\n\n#endif", 1)[0].rstrip()
    if payload.count(marker) != 1 or not payload.endswith(";"):
        raise SystemExit("compressed font declaration is malformed")
    digest = hashlib.sha256(payload.encode("utf-8")).hexdigest()
    if digest != face["payload_sha"]:
        raise SystemExit(f"compressed font declaration digest mismatch: {digest}")
    rendered = banner(args.face, face) + "\n" + payload + "\n\n#endif\n"
    args.output.write_text(rendered, encoding="utf-8", newline="\n")
    print(f"wrote {args.output} ({len(rendered.encode('utf-8'))} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
