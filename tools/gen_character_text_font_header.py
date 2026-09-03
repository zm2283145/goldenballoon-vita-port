#!/usr/bin/env python3
"""Wrap the pinned custom-character text face as a generated C header.

The actual compression is intentionally delegated to Dear ImGui's audited
``binary_to_compressed_c`` tool. This wrapper rejects an unexpected payload,
rejects a ``--unicodes`` range the subset does not actually carry, and adds the
project provenance/contract banner and include guard.
"""

from __future__ import annotations

import argparse
import hashlib
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import character_text_subset  # noqa: E402


SYMBOL = "MdkrCharacterText_compressed_data_base85"
PAYLOAD_SHA256 = "3bb210c02c70ab0860f8ad2d9694853212ebac66e62181a04901067f0d9b4482"
# The head.modified inside the shipped face. FontTools recalculates that
# field on save, so without this the documented commands cannot reproduce
# the recorded subset digest.
SOURCE_DATE_EPOCH = 1787935697

# The repertoire the subset step asks for. Every entry is verified below to
# contribute at least one glyph, because pyftsubset accepts a range the source
# font has never carried and drops it in silence.
SUBSET_RANGES = (
    "U+0020-024F,U+0300-052F,U+1E00-1FFF,U+2000-206F,U+20A0-20CF,"
    "U+2100-214F,U+FFFD"
)


def banner(unicodes: str) -> str:
    return f"""/*
 * gfx_character_text_face.h -- GENERATED FILE, DO NOT EDIT.
 *
 * Project-owned native glyph source for custom-character display/short names.
 * It is independent of every ROM and is never replaced by a host-system font.
 *
 * Upstream: Roboto variable font at google/fonts commit
 *   ade3d1533e06b2b1462ffcde8e08b129627ca360
 *   ofl/roboto/Roboto[wdth,wght].ttf
 *   sha256 d7598e12c5dbef095ff8272cfc55da0250bd07fbdecbac8a530b9b277872a134
 * Modification: FontTools 4.63.0, wght=600/wdth=100 static instance, no
 * hinting, subset to the bounded primary repertoire documented below while
 * retaining all applicable OpenType layout features for the pinned shaper.
 * Subset sha256:
 *   d8a1237268dcec64c5b0007c927431adf8f58a114e79cc04fc9d238e82f71642
 * Compression: Dear ImGui binary_to_compressed_c at project pin
 *   776bf2ab0d61e719e58f2c6d27d109ab5dcf2af1
 *   tool sha256 8f39e8890cd03c9374be72973f6d95b335d31c8b6862f7b249e9d45ff0daf691
 *   compressed declaration sha256
 *   3bb210c02c70ab0860f8ad2d9694853212ebac66e62181a04901067f0d9b4482
 *
 * Reproduction (in a throwaway directory):
 *   python3 -m venv venv
 *   venv/bin/python -m pip install fonttools==4.63.0
 *   # Download and sha256-check the exact upstream paths/pins above.
 *   export SOURCE_DATE_EPOCH={SOURCE_DATE_EPOCH}
 *   venv/bin/python -c "from fontTools import ttLib; from fontTools.varLib import instancer; f=ttLib.TTFont('Roboto.ttf'); instancer.instantiateVariableFont(f,{{'wght':600,'wdth':100}},updateFontNames=True).save('Roboto-SemiBold.ttf')"
 *   venv/bin/pyftsubset Roboto-SemiBold.ttf \\
 *     --unicodes={unicodes} \\
 *     --ignore-missing-unicodes --output-file=Roboto-MDKR-Character-Text.ttf \\
 *     --no-hinting --layout-features='*' --glyph-names \\
 *     --name-IDs='*' --name-languages='*'
 *   binary_to_compressed_c -base85 Roboto-MDKR-Character-Text.ttf \\
 *     MdkrCharacterText > compressed.inc
 *   tools/gen_character_text_font_header.py compressed.inc \\
 *     platform/fast3d/gfx_character_text_face.h
 *
 * SOURCE_DATE_EPOCH is what makes the subset digest above checkable. Without
 * it FontTools stamps the run's own clock into head.modified, and the result
 * differs from the shipped face in exactly 12 bytes -- head.modified, the
 * checkSumAdjustment derived from it, and head's entry in the sfnt table
 * directory. Every glyph, name and layout byte is identical either way; the
 * pinned value is the timestamp inside the face this header carries.
 *
 * The face is SIL Open Font License 1.1. See lib/fonts/LICENSE.txt and
 * THIRD_PARTY.md. Roboto reserves no font name.
 *
 * This primary face covers Latin, Greek, Cyrillic, their extensions, combining
 * marks, and neutral punctuation. The runtime uses it with pinned HarfBuzz and
 * SheenBidi plus reviewed Noto script faces; missing glyphs and unsafe invisible
 * controls fail closed instead of consulting host fonts.
 *
 * The subset list holds only ranges this face actually carries. Upstream Roboto
 * has no arrow (U+2190-21FF), Cyrillic Extended-A/B (U+2DE0-2DFF, U+A640-A69F)
 * or fullwidth-form (U+FF01-FF5E) glyph, so asking for those produced the same
 * bytes while implying a coverage the launcher did not have. The generator now
 * refuses a range that contributes nothing.
 */
#ifndef MDKR_GFX_CHARACTER_TEXT_FACE_H
#define MDKR_GFX_CHARACTER_TEXT_FACE_H
"""


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("compressed", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument(
        "--unicodes", default=SUBSET_RANGES,
        help="the pyftsubset --unicodes list the payload was built from",
    )
    args = parser.parse_args()

    source = args.compressed.read_text(encoding="utf-8")
    marker = f"static const char {SYMBOL}"
    start = source.find(marker)
    if start < 0 or "Roboto-MDKR-Character-Text.ttf" not in source[:start]:
        raise SystemExit("unexpected binary_to_compressed_c payload")
    payload = source[start:].split("\n\n#endif", 1)[0].rstrip()
    if payload.count(marker) != 1 or not payload.endswith(";"):
        raise SystemExit("compressed font declaration is malformed")
    digest = hashlib.sha256(payload.encode("utf-8")).hexdigest()
    if digest != PAYLOAD_SHA256:
        raise SystemExit(
            f"compressed font declaration digest mismatch: {digest}"
        )
    inert = character_text_subset.empty_ranges(
        character_text_subset.font_codepoints(
            character_text_subset.subset_from_declaration(payload)
        ),
        character_text_subset.parse_ranges(args.unicodes),
    )
    if inert:
        raise SystemExit(
            "subset contributed no glyph for " + ", ".join(inert) +
            " -- the source font does not carry that range, so the header "
            "would advertise coverage the launcher cannot draw"
        )
    rendered = banner(args.unicodes) + "\n" + payload + "\n\n#endif\n"
    args.output.write_text(rendered, encoding="utf-8", newline="\n")
    print(f"wrote {args.output} ({len(rendered.encode('utf-8'))} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
