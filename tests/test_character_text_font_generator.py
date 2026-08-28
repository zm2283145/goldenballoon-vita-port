#!/usr/bin/env python3
"""Reproducibility guard for the embedded custom-character text face."""

from __future__ import annotations

import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
GENERATED = ROOT / "platform" / "fast3d" / "gfx_character_text_face.h"
GENERATOR = ROOT / "tools" / "gen_character_text_font_header.py"
MARKER = "static const char MdkrCharacterText_compressed_data_base85"


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


if __name__ == "__main__":
    unittest.main()
