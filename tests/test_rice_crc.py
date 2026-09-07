#!/usr/bin/env python3
"""Coverage for the UNVALIDATED Rice CRC candidate.

Nothing here can show that the algorithm is the one Rice uses -- that needs a
ROM, and the experiment is written out in tools/ricepack/rice_crc.py. What
these tests can do, and do, is fix everything the module is allowed to be wrong
about *independently* of that question:

  * the CRC-32 primitive is the standard one, checked against zlib rather than
    against a constant somebody remembered;
  * the span each variant reads is exactly the rows the geometry names, and
    never the padding between them;
  * the four variants are genuinely different from one another, so a hit-rate
    sweep across them is a real measurement and not four names for one answer.

The pinned vectors at the end are change detectors, not oracles. They will fail
if a refactor alters the arithmetic, which is their whole job; they say nothing
about whether the arithmetic was right to begin with, and a future commit that
learns the true algorithm is expected to replace them outright.
"""

from __future__ import annotations

from pathlib import Path
import sys
import unittest
import zlib


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools" / "ricepack"))

import rice_crc  # noqa: E402
import rice_names  # noqa: E402


class Crc32Primitive(unittest.TestCase):
    def test_table_is_the_standard_reflected_crc32_table(self):
        self.assertEqual(len(rice_crc.CRC32_TABLE), 256)
        self.assertEqual(rice_crc.CRC32_TABLE[0], 0x00000000)
        self.assertEqual(rice_crc.CRC32_TABLE[1], 0x77073096)
        self.assertEqual(rice_crc.CRC32_TABLE[255], 0x2D02EF8D)

    def test_one_row_agrees_with_zlib(self):
        """The strongest available check on table and loop together.

        zlib.crc32 seeds 0xFFFFFFFF and inverts at the end, so a single-row
        `row-sum-final` must equal it exactly and `row-sum-raw` must equal it
        inverted. If the loop had the operands transposed or the shift the
        wrong way, this is where it shows.
        """
        row = bytes(range(256)) * 3
        width = len(row) // 2  # 16bpp: row bytes == width * 2
        self.assertEqual(
            rice_crc.rice_crc32(row, width, 1, 2,
                                variant=rice_crc.VARIANT_ROW_SUM_FINAL),
            zlib.crc32(row) & 0xFFFFFFFF)
        self.assertEqual(
            rice_crc.rice_crc32(row, width, 1, 2,
                                variant=rice_crc.VARIANT_ROW_SUM_RAW),
            zlib.crc32(row) ^ 0xFFFFFFFF)

    def test_streaming_variant_hashes_rows_as_one_message(self):
        rows = bytes(range(64))
        self.assertEqual(
            rice_crc.rice_crc32(rows, 16, 2, 2,
                                variant=rice_crc.VARIANT_STREAM_FINAL),
            zlib.crc32(rows) & 0xFFFFFFFF)


class HashedSpan(unittest.TestCase):
    def test_row_length_follows_the_size_code(self):
        for siz, expected in ((0, 32), (1, 64), (2, 128), (3, 256)):
            self.assertEqual(rice_names.rice_row_bytes(64, siz), expected)

    def test_padding_between_rows_is_not_hashed(self):
        """A wider pitch must not change the answer.

        This is the property that makes the CRC a name for a picture rather
        than a name for how the picture happened to be laid out, and it is the
        one most likely to be broken by a well-meaning simplification.
        """
        payload = bytes(range(32)) * 4
        packed = rice_crc.rice_crc32(payload, 16, 4, 1)
        padded_bytes = bytearray()
        for row in range(4):
            padded_bytes += payload[row * 16:(row + 1) * 16]
            padded_bytes += b"\xAB" * 48
        padded = rice_crc.rice_crc32(bytes(padded_bytes), 16, 4, 1, pitch=64)
        self.assertEqual(packed, padded)

    def test_a_changed_texel_changes_the_crc(self):
        payload = bytearray(bytes(range(64)))
        before = rice_crc.rice_crc32(bytes(payload), 32, 2, 1)
        payload[33] ^= 0x01
        self.assertNotEqual(before, rice_crc.rice_crc32(bytes(payload), 32, 2, 1))

    def test_a_short_span_is_refused_rather_than_hashed_short(self):
        with self.assertRaises(ValueError):
            rice_crc.rice_crc32(b"\x00" * 32, 32, 4, 1)

    def test_a_pitch_narrower_than_a_row_is_refused(self):
        with self.assertRaises(ValueError):
            rice_crc.rice_crc32(b"\x00" * 256, 32, 4, 1, pitch=16)

    def test_an_undefined_size_code_is_refused(self):
        with self.assertRaises(ValueError):
            rice_crc.rice_crc32(b"\x00" * 256, 32, 4, 7)


class Variants(unittest.TestCase):
    def test_every_variant_is_distinct(self):
        payload = bytes((index * 37 + 11) & 0xFF for index in range(256))
        values = rice_crc.all_variants(payload, 32, 4, 1)
        self.assertEqual(sorted(values), sorted(rice_crc.VARIANTS))
        self.assertEqual(len(set(values.values())), len(rice_crc.VARIANTS))

    def test_summing_variants_cannot_see_row_order_and_streaming_ones_can(self):
        """The cheapest discriminator between the two families.

        Addition is commutative, so a per-row sum gives a vertically mirrored
        texture the same name. That is a real limitation of half the candidate
        space, recorded here so nobody has to rediscover it while reading a
        hit-rate table.
        """
        rows = [bytes([value] * 16) for value in (1, 2, 3, 4)]
        forward = b"".join(rows)
        reversed_rows = b"".join(reversed(rows))
        for variant in (rice_crc.VARIANT_ROW_SUM_RAW,
                        rice_crc.VARIANT_ROW_SUM_FINAL):
            self.assertEqual(
                rice_crc.rice_crc32(forward, 16, 4, 1, variant=variant),
                rice_crc.rice_crc32(reversed_rows, 16, 4, 1, variant=variant),
                variant)
        for variant in (rice_crc.VARIANT_STREAM_RAW,
                        rice_crc.VARIANT_STREAM_FINAL):
            self.assertNotEqual(
                rice_crc.rice_crc32(forward, 16, 4, 1, variant=variant),
                rice_crc.rice_crc32(reversed_rows, 16, 4, 1, variant=variant),
                variant)

    def test_unknown_variant_is_refused(self):
        with self.assertRaises(ValueError):
            rice_crc.rice_crc32(b"\x00" * 64, 32, 2, 1, variant="v1")

    def test_format_crc_is_eight_upper_case_hex_digits(self):
        self.assertEqual(rice_crc.format_crc(0), "00000000")
        self.assertEqual(rice_crc.format_crc(0xABCDEF01), "ABCDEF01")
        with self.assertRaises(ValueError):
            rice_crc.format_crc(0x1_0000_0000)

    def test_result_is_deterministic(self):
        payload = bytes(range(128))
        first = rice_crc.all_variants(payload, 32, 2, 2)
        self.assertEqual(first, rice_crc.all_variants(payload, 32, 2, 2))


class PinnedVectors(unittest.TestCase):
    """Change detectors. NOT evidence that any of these values is correct.

    A commit that establishes the real algorithm should delete this class and
    replace it with vectors taken from an emulator.
    """

    PAYLOAD = bytes((index * 31 + 7) & 0xFF for index in range(256))

    def test_pinned(self):
        self.assertEqual(
            rice_crc.all_variants(self.PAYLOAD, 32, 8, 1),
            {
                rice_crc.VARIANT_ROW_SUM_RAW: "81160008",
                rice_crc.VARIANT_ROW_SUM_FINAL: "7EE9FFF0",
                rice_crc.VARIANT_STREAM_RAW: "F3162C9C",
                rice_crc.VARIANT_STREAM_FINAL: "0CE9D363",
            })


if __name__ == "__main__":
    unittest.main()
