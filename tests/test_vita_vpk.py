#!/usr/bin/env python3
"""ROM-free checks for the Vita CI artifact verifier."""

import importlib.util
from pathlib import Path
import struct
import tempfile
import unittest
import zipfile


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("verify_vita_vpk", ROOT / "tools" / "verify_vita_vpk.py")
verifier = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(verifier)


def make_sfo(title="GBLN00001", version="01.72"):
    keys = b"TITLE_ID\0APP_VER\0"
    values = title.encode() + b"\0" + version.encode() + b"\0"
    start = 20 + 2 * 16
    header = struct.pack("<5I", 0x46535000, 0x101, start, start + len(keys), 2)
    first = struct.pack("<HHIII", 0, 0x0204, len(title) + 1, len(title) + 1, 0)
    second = struct.pack("<HHIII", 9, 0x0204, len(version) + 1, len(version) + 1, len(title) + 1)
    return header + first + second + keys + values


class VitaVpkTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.vpk = Path(self.temp.name) / "test.vpk"

    def package(self, *, title="GBLN00001", version="01.72", omit=None, extra=None):
        with zipfile.ZipFile(self.vpk, "w") as archive:
            for name in sorted(verifier.FILES - {omit}):
                data = make_sfo(title, version) if name.endswith("param.sfo") else b"test fixture"
                archive.writestr(name, data)
            if extra:
                archive.writestr(extra, b"not allowed")

    def test_full_package(self):
        self.package()
        result = verifier.verify(self.vpk, "1.7.2")
        self.assertEqual(result["app_ver"], "01.72")
        self.assertEqual(len(result["files"]), 7)
        self.assertEqual(len(result["sha256"]), 64)

    def test_wrong_title(self):
        self.package(title="OTHER0001")
        with self.assertRaisesRegex(ValueError, "TITLE_ID"):
            verifier.verify(self.vpk, "1.7.2")

    def test_wrong_version(self):
        self.package(version="01.71")
        with self.assertRaisesRegex(ValueError, "APP_VER"):
            verifier.verify(self.vpk, "1.7.2")

    def test_missing_file(self):
        self.package(omit="eboot.bin")
        with self.assertRaises(ValueError):
            verifier.verify(self.vpk, "1.7.2")

    def test_extra_game_data_rejected(self):
        self.package(extra="baserom.z64")
        with self.assertRaises(ValueError):
            verifier.verify(self.vpk, "1.7.2")

    def test_unrepresentable_version(self):
        for version in ("1.7.10", "1.10.0", "100.0.0", "dev", "1.7.2-extra"):
            with self.subTest(version=version), self.assertRaises(ValueError):
                verifier.app_version(version)

    def test_truncated_sfo(self):
        with self.assertRaises(struct.error):
            verifier.sfo_strings(b"\0PSF")

    def test_sfo_bad_bounds(self):
        bad = bytearray(make_sfo())
        struct.pack_into("<I", bad, 12, len(bad) + 4)
        with self.assertRaises(ValueError):
            verifier.sfo_strings(bad)


if __name__ == "__main__":
    unittest.main()
