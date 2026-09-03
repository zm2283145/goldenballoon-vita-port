#!/usr/bin/env python3
"""Reproducibility guard for the embedded custom-character text face."""

from __future__ import annotations

import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import character_text_subset  # noqa: E402
import gen_character_text_font_header as latin_generator  # noqa: E402
import gen_character_text_script_font_header as script_generator  # noqa: E402

GENERATED = ROOT / "platform" / "fast3d" / "gfx_character_text_face.h"
GENERATOR = ROOT / "tools" / "gen_character_text_font_header.py"
MARKER = "static const char MdkrCharacterText_compressed_data_base85"
SCRIPT_GENERATOR = ROOT / "tools" / "gen_character_text_script_font_header.py"
SCRIPT_FACES = {
    "arabic": (
        ROOT / "platform" / "fast3d" / "gfx_character_text_arabic_face.h",
        "static const char MdkrCharacterTextArabic_compressed_data_base85",
        "NotoSansArabic-MDKR.ttf",
    ),
    "hebrew": (
        ROOT / "platform" / "fast3d" / "gfx_character_text_hebrew_face.h",
        "static const char MdkrCharacterTextHebrew_compressed_data_base85",
        "NotoSansHebrew-MDKR.ttf",
    ),
}


class CharacterTextFontGeneratorTests(unittest.TestCase):
    def test_checked_payload_reproduces_tracked_header(self) -> None:
        tracked = GENERATED.read_text(encoding="utf-8")
        payload = tracked[tracked.index(MARKER):].split("\n\n#endif", 1)[0]
        source = (
            "// File: 'Roboto-MDKR-Character-Text.ttf' (126448 bytes)\n"
            "// Exported using the pinned tool\n" + payload + "\n"
        )
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            compressed = root / "compressed.inc"
            output = root / "generated.h"
            compressed.write_text(source, encoding="utf-8", newline="\n")
            process = subprocess.run(
                [sys.executable, str(GENERATOR), str(compressed), str(output)],
                cwd=ROOT, text=True, capture_output=True, check=False,
            )
            self.assertEqual(process.returncode, 0, process.stderr)
            self.assertEqual(output.read_bytes(), GENERATED.read_bytes())

    def test_one_byte_payload_drift_fails_closed(self) -> None:
        tracked = GENERATED.read_text(encoding="utf-8")
        payload = tracked[tracked.index(MARKER):].split("\n\n#endif", 1)[0]
        payload = payload.replace("7])#######", "8])#######", 1)
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            compressed = root / "compressed.inc"
            output = root / "generated.h"
            compressed.write_text(
                "// File: 'Roboto-MDKR-Character-Text.ttf'\n" + payload,
                encoding="utf-8", newline="\n",
            )
            process = subprocess.run(
                [sys.executable, str(GENERATOR), str(compressed), str(output)],
                cwd=ROOT, text=True, capture_output=True, check=False,
            )
            self.assertNotEqual(process.returncode, 0)
            self.assertIn("digest mismatch", process.stderr)
            self.assertFalse(output.exists())

    def test_checked_script_faces_reproduce_tracked_headers(self) -> None:
        for face, (generated, marker, filename) in SCRIPT_FACES.items():
            with self.subTest(face=face), tempfile.TemporaryDirectory() as raw:
                tracked = generated.read_text(encoding="utf-8")
                payload = tracked[tracked.index(marker):].split(
                    "\n\n#endif", 1
                )[0]
                root = Path(raw)
                compressed = root / "compressed.inc"
                output = root / "generated.h"
                compressed.write_text(
                    f"// File: '{filename}'\n" + payload + "\n",
                    encoding="utf-8", newline="\n",
                )
                process = subprocess.run(
                    [sys.executable, str(SCRIPT_GENERATOR), face,
                     str(compressed), str(output)],
                    cwd=ROOT, text=True, capture_output=True, check=False,
                )
                self.assertEqual(process.returncode, 0, process.stderr)
                self.assertEqual(output.read_bytes(), generated.read_bytes())


    def _tracked_payload(self) -> str:
        tracked = GENERATED.read_text(encoding="utf-8")
        return tracked[tracked.index(MARKER):].split("\n\n#endif", 1)[0]

    def _generate(self, payload: str, *arguments: str):
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            compressed = root / "compressed.inc"
            output = root / "generated.h"
            compressed.write_text(
                "// File: 'Roboto-MDKR-Character-Text.ttf' (126448 bytes)\n"
                "// Exported using the pinned tool\n" + payload + "\n",
                encoding="utf-8", newline="\n",
            )
            process = subprocess.run(
                [sys.executable, str(GENERATOR), str(compressed), str(output),
                 *arguments],
                cwd=ROOT, text=True, capture_output=True, check=False,
            )
            return process, output.read_bytes() if output.exists() else None

    def test_pinned_ranges_all_contribute_glyphs(self) -> None:
        """Every range the pipeline asks for must reach the shipped face.

        pyftsubset drops an unavailable range without a word, so a range that
        contributes nothing is indistinguishable from one that works until a
        player sees the box. This reads the repertoire back out of the payload
        the header actually carries.
        """
        subset = character_text_subset.subset_from_declaration(
            self._tracked_payload()
        )
        codepoints = character_text_subset.font_codepoints(subset)
        self.assertEqual(
            character_text_subset.empty_ranges(
                codepoints,
                character_text_subset.parse_ranges(
                    latin_generator.SUBSET_RANGES
                ),
            ),
            [],
        )
        for face in script_generator.FACES.values():
            with self.subTest(face=face["file"]):
                generated, marker, _ = SCRIPT_FACES[
                    "arabic" if "Arabic" in face["label"] else "hebrew"
                ]
                tracked = generated.read_text(encoding="utf-8")
                declaration = tracked[tracked.index(marker):].split(
                    "\n\n#endif", 1
                )[0]
                script_subset = (
                    character_text_subset.subset_from_declaration(declaration)
                )
                self.assertEqual(
                    character_text_subset.empty_ranges(
                        character_text_subset.font_codepoints(script_subset),
                        character_text_subset.parse_ranges(face["ranges"]),
                    ),
                    [],
                )

    def test_inert_unicode_range_fails_closed(self) -> None:
        """Positive control: the arrow block is not in the pinned Roboto.

        U+2190-21FF is the range that shipped inert for the life of this file.
        Asking for it must name it and refuse, or this gate proves nothing.
        """
        process, written = self._generate(
            self._tracked_payload(),
            "--unicodes", latin_generator.SUBSET_RANGES + ",U+2190-21FF",
        )
        self.assertNotEqual(process.returncode, 0)
        self.assertIn("U+2190-21FF", process.stderr)
        self.assertIn("contributed no glyph", process.stderr)
        self.assertIsNone(written)

    def test_subset_reader_agrees_with_the_recorded_digest(self) -> None:
        import hashlib
        subset = character_text_subset.subset_from_declaration(
            self._tracked_payload()
        )
        self.assertIn(
            hashlib.sha256(subset).hexdigest(),
            GENERATED.read_text(encoding="utf-8"),
        )


if __name__ == "__main__":
    unittest.main()
