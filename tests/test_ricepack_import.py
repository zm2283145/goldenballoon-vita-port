#!/usr/bin/env python3
"""Coverage for the Rice pack importer: every decision, and the manifest.

No pack content is read here. Every fixture is a handful of tiny PNGs written
by the test, which is also what keeps this suite honest about the caps: a
header that lies about its dimensions is the only cheap way to prove a
decompression bomb is refused before anything expands it.
"""

from __future__ import annotations

import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest
import zlib
from contextlib import redirect_stderr, redirect_stdout
from io import StringIO


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools" / "ricepack"))

import import_rice_pack as importer  # noqa: E402
import png_probe                     # noqa: E402
import rice_names                    # noqa: E402


ROM = "Some Racing Game"


def name_for(crc: str, fmt: int, siz: int, part: str) -> str:
    return f"{ROM}#{crc}#{fmt}#{siz}_{part}.png"


def solid_png(width: int = 4, height: int = 4,
              colour: tuple[int, int, int, int] = (10, 20, 30, 40)) -> bytes:
    return png_probe.encode_rgba(width, height, bytes(colour) * width * height)


def lying_png(width: int, height: int, colour_type: int = 6,
              bit_depth: int = 8, interlace: int = 0,
              filler: int = 0) -> bytes:
    """A structurally valid header over a payload that does not match it.

    The scanning pass reads IHDR and stops, so this is exactly what a
    decompression bomb looks like to it: a small file claiming an enormous
    image. `filler` pads the file so the expansion ratio can be steered without
    changing the declared picture.
    """
    def chunk(kind: bytes, payload: bytes) -> bytes:
        return (struct.pack(">I", len(payload)) + kind + payload
                + struct.pack(">I", zlib.crc32(kind + payload) & 0xFFFFFFFF))

    header = struct.pack(">IIBBBBB", width, height, bit_depth, colour_type,
                         0, 0, interlace)
    return (png_probe.PNG_SIGNATURE + chunk(b"IHDR", header)
            + chunk(b"IDAT", b"\x00" * filler) + chunk(b"IEND", b""))


class PackFixture:
    """A throwaway Rice pack directory plus the paths a run needs."""

    def __init__(self, stack: tempfile.TemporaryDirectory):
        self.root = Path(stack.name)
        self.pack = self.root / "Some Pack"
        self.pack.mkdir()

    def add(self, relative: str, payload: bytes) -> Path:
        target = self.pack / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(payload)
        return target

    def plan(self, *extra: str) -> dict:
        manifest = self.root / "manifest.json"
        with redirect_stdout(StringIO()):
            code = importer.main(
                ["plan", str(self.pack), "--manifest", str(manifest), *extra])
        assert code == 0, f"plan exited {code}"
        return json.loads(manifest.read_text(encoding="utf-8"))


class RicepackTestCase(unittest.TestCase):
    def setUp(self):
        self._stack = tempfile.TemporaryDirectory(prefix="ricepack_test_")
        self.addCleanup(self._stack.cleanup)
        self.fixture = PackFixture(self._stack)

    def files_by_path(self, manifest: dict) -> dict[str, dict]:
        return {entry["path"]: entry for entry in manifest["files"]}

    def keys_by_key(self, manifest: dict) -> dict[str, dict]:
        return {entry["key"]: entry for entry in manifest["keys"]}


class FilenameAllowlist(RicepackTestCase):
    def test_every_non_conforming_file_is_refused_with_a_reason(self):
        self.fixture.add(name_for("AABBCCDD", 0, 2, "all"), solid_png())
        junk = {
            "Thumbs.db": b"\x00\x01\x02",
            "Generated Image 29 ago 2026, 01_48_54.png": solid_png(),
            f"{ROM}#AABBCCDE#0#2_all .png": solid_png(),
            f"{ROM}#AABBCC#0#2_all.png": solid_png(),
            f"{ROM}#AABBCCE0#0#2_all.PNG": solid_png(),
            "notes.txt": b"working notes",
        }
        for relative, payload in junk.items():
            self.fixture.add(relative, payload)

        entries = self.files_by_path(self.fixture.plan())
        for relative in junk:
            with self.subTest(relative):
                entry = entries[relative]
                self.assertEqual(entry["decision"], importer.VERDICT_REJECTED)
                self.assertEqual(entry["reason"],
                                 rice_names.REASON_NOT_RICE_NAME)
                self.assertTrue(entry["detail"])
        self.assertEqual(
            entries[name_for("AABBCCDD", 0, 2, "all")]["decision"],
            importer.VERDICT_SELECTED)

    def test_the_allowlist_names_no_junk_file(self):
        """The junk above must fall out of the pattern, not out of a list.

        A special case for `Thumbs.db` works exactly once: the next pack
        arrives with `.DS_Store`, an editor backup, or a renamed export.
        """
        source = (ROOT / "tools" / "ricepack" / "rice_names.py").read_text(
            encoding="utf-8")
        for literal in ("Thumbs", "DS_Store", "desktop.ini", "Generated Image"):
            self.assertNotIn(literal, source)

    def test_a_recognised_but_unimplemented_rice_part_says_so(self):
        """A Rice-shaped name with a part this importer cannot use.

        Kept distinct from "not a Rice filename" because the two need different
        answers from a pack author: one is a file to delete, the other is a
        format nobody has implemented yet. A wrong-case suffix lands here too,
        which is the most useful thing to tell someone who typed `_ALL`.
        """
        shaped = {
            f"{ROM}#AABBCCDD#2#1#11223344_ciByRGBA.png": solid_png(),
            f"{ROM}#AABBCCDE#0#2_ALL.png": solid_png(),
            f"{ROM}#AABBCCDF#0#2_rgba.png": solid_png(),
        }
        for relative, payload in shaped.items():
            self.fixture.add(relative, payload)
        entries = self.files_by_path(self.fixture.plan())
        for relative in shaped:
            with self.subTest(relative):
                entry = entries[relative]
                self.assertEqual(entry["decision"], importer.VERDICT_REJECTED)
                self.assertEqual(entry["reason"],
                                 rice_names.REASON_UNSUPPORTED_PART)
                self.assertTrue(entry["detail"])

    def test_a_foreign_rom_name_is_refused_when_one_is_required(self):
        self.fixture.add(name_for("AABBCCDD", 0, 2, "all"), solid_png())
        self.fixture.add("Other Game#AABBCCDE#0#2_all.png", solid_png())
        entries = self.files_by_path(
            self.fixture.plan("--rom-name", ROM))
        self.assertEqual(entries["Other Game#AABBCCDE#0#2_all.png"]["reason"],
                         importer.REASON_ROM_MISMATCH)

    def test_an_undefined_rdp_format_is_refused(self):
        self.fixture.add(f"{ROM}#AABBCCDD#7#3_all.png", solid_png())
        entry = self.files_by_path(self.fixture.plan())[
            f"{ROM}#AABBCCDD#7#3_all.png"]
        self.assertEqual(entry["reason"], importer.REASON_UNDEFINED_FORMAT)


class Precedence(RicepackTestCase):
    def build_ambiguous_pack(self, all_dir: str, split_dir: str) -> dict:
        self.fixture.add(f"{all_dir}/{name_for('AABBCCDD', 0, 3, 'all')}",
                         solid_png())
        self.fixture.add(f"{split_dir}/{name_for('AABBCCDD', 0, 3, 'rgb')}",
                         solid_png())
        self.fixture.add(f"{split_dir}/{name_for('AABBCCDD', 0, 3, 'a')}",
                         solid_png())
        return self.fixture.plan()

    def test_all_wins_and_the_shadowed_halves_are_skipped_with_a_reason(self):
        manifest = self.build_ambiguous_pack("World", "Characters")
        key = self.keys_by_key(manifest)["AABBCCDD#0#3#"]
        self.assertEqual(key["decision"], importer.VERDICT_SELECTED)
        self.assertEqual(key["reason"], importer.REASON_SELECTED_ALL)

        entries = self.files_by_path(manifest)
        for part in ("rgb", "a"):
            entry = entries[f"Characters/{name_for('AABBCCDD', 0, 3, part)}"]
            self.assertEqual(entry["decision"], importer.VERDICT_SKIPPED)
            self.assertEqual(entry["reason"], importer.REASON_SHADOWED)
            self.assertIn("World/", entry["detail"])

    def test_directory_position_does_not_decide(self):
        """The `_all` wins whether it sorts before or after the halves.

        In a real pack the halves sit several directories away from the file
        that beats them, so a per-directory or first-seen rule would give two
        different answers for one pack.
        """
        first = self.build_ambiguous_pack("AAA", "ZZZ")
        self._stack.cleanup()
        self._stack = tempfile.TemporaryDirectory(prefix="ricepack_test_")
        self.addCleanup(self._stack.cleanup)
        self.fixture = PackFixture(self._stack)
        second = self.build_ambiguous_pack("ZZZ", "AAA")
        self.assertEqual(self.keys_by_key(first)["AABBCCDD#0#3#"]["reason"],
                         self.keys_by_key(second)["AABBCCDD#0#3#"]["reason"])
        self.assertEqual(first["coverage"]["keysMappable"],
                         second["coverage"]["keysMappable"])

    def test_two_files_claiming_one_part_are_both_refused(self):
        self.fixture.add(f"A/{name_for('AABBCCDD', 0, 3, 'all')}", solid_png())
        self.fixture.add(f"B/{name_for('AABBCCDD', 0, 3, 'all')}",
                         solid_png(colour=(1, 2, 3, 4)))
        manifest = self.fixture.plan()
        key = self.keys_by_key(manifest)["AABBCCDD#0#3#"]
        self.assertEqual(key["decision"], importer.VERDICT_REJECTED)
        self.assertEqual(key["reason"], importer.REASON_DUPLICATE)
        for entry in manifest["files"]:
            self.assertEqual(entry["decision"], importer.VERDICT_REJECTED)
            self.assertIn("A/", entry["detail"])
            self.assertIn("B/", entry["detail"])


class SplitHalves(RicepackTestCase):
    def test_a_complete_pair_is_selected(self):
        self.fixture.add(name_for("AABBCCDD", 0, 3, "rgb"), solid_png())
        self.fixture.add(name_for("AABBCCDD", 0, 3, "a"), solid_png())
        key = self.keys_by_key(self.fixture.plan())["AABBCCDD#0#3#"]
        self.assertEqual(key["decision"], importer.VERDICT_SELECTED)
        self.assertEqual(key["reason"], importer.REASON_SELECTED_PAIR)

    def test_a_lone_colour_half_is_refused_when_the_format_carries_alpha(self):
        self.fixture.add(name_for("AABBCCDD", 0, 2, "rgb"), solid_png())
        key = self.keys_by_key(self.fixture.plan())["AABBCCDD#0#2#"]
        self.assertEqual(key["decision"], importer.VERDICT_REJECTED)
        self.assertEqual(key["reason"],
                         importer.REASON_ORPHAN_RGB_ALPHA_FORMAT)

    def test_a_lone_colour_half_is_refused_when_alpha_is_the_intensity(self):
        """I4/I8 have no alpha channel, but the port's decoder derives one.

        Accepting a colour-only half here would silently make every intensity
        texture fully opaque, which is why `luminance` is its own class rather
        than being folded into "no alpha".
        """
        self.fixture.add(name_for("AABBCCDD", 4, 0, "rgb"), solid_png())
        key = self.keys_by_key(self.fixture.plan())["AABBCCDD#4#0#"]
        self.assertEqual(key["decision"], importer.VERDICT_REJECTED)
        self.assertEqual(key["reason"], importer.REASON_ORPHAN_RGB_LUMINANCE)

    def test_a_lone_colour_half_is_accepted_when_the_format_has_no_alpha(self):
        self.fixture.add(name_for("AABBCCDD", 1, 2, "rgb"), solid_png())
        key = self.keys_by_key(self.fixture.plan())["AABBCCDD#1#2#"]
        self.assertEqual(key["decision"], importer.VERDICT_SELECTED)
        self.assertEqual(key["reason"], importer.REASON_SELECTED_OPAQUE)

    def test_a_lone_coverage_half_is_always_refused(self):
        for fmt, siz in ((0, 2), (0, 3), (4, 0), (1, 2)):
            with self.subTest(fmt=fmt, siz=siz):
                self.setUp()
                self.fixture.add(name_for("AABBCCDD", fmt, siz, "a"),
                                 solid_png())
                key = self.keys_by_key(
                    self.fixture.plan())[f"AABBCCDD#{fmt}#{siz}#"]
                self.assertEqual(key["decision"], importer.VERDICT_REJECTED)
                self.assertEqual(key["reason"], importer.REASON_ORPHAN_ALPHA)

    def test_an_undecodable_half_is_refused_but_an_all_image_is_not(self):
        """`_all` is copied through; only halves are decompressed here.

        A 16-bit or palettised `_all` still works, because the game's own
        decoder reads it. Applying this tool's narrower decoder to it would
        refuse files that are fine.
        """
        self.fixture.add(name_for("AABBCCDD", 0, 3, "all"),
                         lying_png(4, 4, colour_type=3, bit_depth=4))
        self.fixture.add(name_for("AABBCCDE", 0, 3, "rgb"),
                         lying_png(4, 4, colour_type=3, bit_depth=4))
        entries = self.files_by_path(self.fixture.plan())
        self.assertEqual(entries[name_for("AABBCCDD", 0, 3, "all")]["decision"],
                         importer.VERDICT_SELECTED)
        self.assertEqual(entries[name_for("AABBCCDE", 0, 3, "rgb")]["reason"],
                         png_probe.REASON_BIT_DEPTH)


class Caps(RicepackTestCase):
    def test_an_oversized_image_is_refused(self):
        self.fixture.add(name_for("AABBCCDD", 0, 3, "all"),
                         lying_png(importer.MAX_TEXTURE_DIMENSION + 1, 8))
        entry = self.files_by_path(self.fixture.plan())[
            name_for("AABBCCDD", 0, 3, "all")]
        self.assertEqual(entry["reason"], importer.REASON_DIMENSION_CAP)

    def test_an_oversized_file_is_refused(self):
        payload = lying_png(8, 8, filler=importer.MAX_TEXTURE_SOURCE_BYTES + 1)
        self.fixture.add(name_for("AABBCCDD", 0, 3, "all"), payload)
        entry = self.files_by_path(self.fixture.plan())[
            name_for("AABBCCDD", 0, 3, "all")]
        self.assertEqual(entry["reason"], importer.REASON_SOURCE_BYTES_CAP)

    def test_a_decompression_bomb_is_refused_from_its_header(self):
        """4096x4096 RGBA is 64 MiB expanded from a file of a few hundred bytes.

        Nothing inflates it: the refusal comes from IHDR, which is the only
        point at which refusing is still cheap.
        """
        bomb = lying_png(4096, 4096)
        self.assertLess(len(bomb), 1024)
        self.fixture.add(name_for("AABBCCDD", 0, 3, "all"), bomb)
        entry = self.files_by_path(self.fixture.plan())[
            name_for("AABBCCDD", 0, 3, "all")]
        self.assertEqual(entry["reason"], importer.REASON_EXPANSION_CAP)
        self.assertEqual(entry["decodedBytes"], 4096 * 4096 * 4)

    def test_a_large_but_proportionate_image_is_accepted(self):
        """The bomb rule must not be a second, secret size limit."""
        honest = lying_png(1024, 1024, filler=1024 * 128)
        self.fixture.add(name_for("AABBCCDD", 0, 3, "all"), honest)
        entry = self.files_by_path(self.fixture.plan())[
            name_for("AABBCCDD", 0, 3, "all")]
        self.assertEqual(entry["decision"], importer.VERDICT_SELECTED)

    def test_the_whole_pack_total_is_capped(self):
        original = importer.MAX_PACK_DECODED_BYTES
        importer.MAX_PACK_DECODED_BYTES = 4 * 1024 * 1024
        self.addCleanup(setattr, importer, "MAX_PACK_DECODED_BYTES", original)
        for index in range(4):
            self.fixture.add(
                f"{index}/" + name_for(f"AABBCC0{index}", 0, 3, "all"),
                lying_png(1024, 512, filler=64 * 1024))
        manifest = self.fixture.plan()
        decisions = [entry["decision"] for entry in manifest["files"]]
        self.assertEqual(decisions.count(importer.VERDICT_SELECTED), 2)
        refused = [entry for entry in manifest["files"]
                   if entry["decision"] == importer.VERDICT_REJECTED]
        self.assertEqual({entry["reason"] for entry in refused},
                         {importer.REASON_PACK_DECODED_CAP})
        # Deterministic, and by path rather than by whatever the walk returned
        # last: the two that survive are always the two that sort first.
        self.assertEqual(
            sorted(entry["path"] for entry in manifest["files"]
                   if entry["decision"] == importer.VERDICT_SELECTED),
            sorted(entry["path"] for entry in manifest["files"])[:2])


class Manifest(RicepackTestCase):
    def populated_pack(self) -> None:
        self.fixture.add("World/" + name_for("AABBCCDD", 0, 2, "all"), solid_png())
        self.fixture.add("World/" + name_for("AABBCCDE", 0, 3, "rgb"), solid_png())
        self.fixture.add("World/" + name_for("AABBCCDE", 0, 3, "a"), solid_png())
        self.fixture.add("Menus/" + name_for("AABBCCDF", 0, 2, "rgb"), solid_png())
        self.fixture.add("Thumbs.db", b"\x00")

    def test_a_repeat_run_is_byte_identical(self):
        self.populated_pack()
        first = self.fixture.root / "one.json"
        second = self.fixture.root / "two.json"
        for target in (first, second):
            with redirect_stdout(StringIO()):
                importer.main(["plan", str(self.fixture.pack),
                               "--manifest", str(target)])
        self.assertEqual(first.read_bytes(), second.read_bytes())

    def test_every_input_carries_a_decision_and_a_source_digest(self):
        self.populated_pack()
        manifest = self.fixture.plan()
        self.assertEqual(len(manifest["files"]), 5)
        for entry in manifest["files"]:
            self.assertIn(entry["decision"],
                          (importer.VERDICT_SELECTED, importer.VERDICT_SKIPPED,
                           importer.VERDICT_REJECTED))
            self.assertRegex(entry["sha256"], r"^[0-9a-f]{64}$")
            self.assertGreater(entry["sourceBytes"], 0)
            if entry["decision"] != importer.VERDICT_SELECTED:
                self.assertTrue(entry["reason"])
                self.assertTrue(entry["detail"])
        for entry in manifest["files"]:
            if entry["path"].endswith(".png"):
                self.assertIn("width", entry)
                self.assertIn("height", entry)

    def test_a_misnamed_image_still_records_its_dimensions(self):
        """A refusal is more useful when it says what was refused.

        The dimensions are read before the name is judged, but they never
        change the verdict: this file is still refused for its name.
        """
        self.fixture.add("Generated Image.png", lying_png(320, 240))
        entry = self.files_by_path(self.fixture.plan())["Generated Image.png"]
        self.assertEqual(entry["reason"], rice_names.REASON_NOT_RICE_NAME)
        self.assertEqual((entry["width"], entry["height"]), (320, 240))

    def test_a_half_lost_to_a_pack_limit_takes_its_partner_with_it(self):
        original = importer.MAX_PACK_DECODED_BYTES
        importer.MAX_PACK_DECODED_BYTES = 3 * 1024 * 1024
        self.addCleanup(setattr, importer, "MAX_PACK_DECODED_BYTES", original)
        for part in ("a", "rgb"):
            self.fixture.add(name_for("AABBCCDD", 0, 3, part),
                             lying_png(1024, 512, filler=64 * 1024))
        manifest = self.fixture.plan()
        self.assertEqual(
            self.keys_by_key(manifest)["AABBCCDD#0#3#"]["decision"],
            importer.VERDICT_REJECTED)
        decisions = {entry["path"]: entry["decision"]
                     for entry in manifest["files"]}
        self.assertNotIn(importer.VERDICT_SELECTED, decisions.values())
        self.assertEqual(manifest["coverage"]["keysMappable"], 0)

    def test_no_absolute_path_reaches_the_manifest(self):
        """Relative paths are not tidiness.

        An absolute path would make the record machine-specific -- so no longer
        byte-identical anywhere else -- and would also carry a home directory
        into a file somebody may well commit.
        """
        self.populated_pack()
        text = json.dumps(self.fixture.plan())
        self.assertNotIn(str(self.fixture.root), text)
        self.assertNotIn(str(Path.home()), text)
        for entry in self.fixture.plan()["files"]:
            self.assertFalse(entry["path"].startswith("/"))

    def test_the_version_fields_are_present_and_read_from_the_contract(self):
        self.populated_pack()
        manifest = self.fixture.plan("--source-pack-version", "2026-08-29")
        self.assertEqual(manifest["importer"]["version"],
                         importer.IMPORTER_VERSION)
        self.assertEqual(manifest["sourcePack"]["declaredVersion"], "2026-08-29")
        self.assertEqual(manifest["digestContract"]["definedBy"],
                         "platform/mod_texture_key.h")

        # Parsed a second way, from the header itself, so the manifest cannot
        # simply be repeating a number this tool hard-coded.
        header = (ROOT / "platform" / "mod_texture_key.h").read_text(
            encoding="utf-8")
        declared = [line.split()[2].rstrip("u")
                    for line in header.splitlines()
                    if line.startswith("#define MDKR_MOD_TEXTURE_DIGEST_VERSION")]
        self.assertEqual([str(manifest["digestContract"]["version"])], declared)

    def test_a_missing_source_version_is_recorded_as_missing(self):
        self.populated_pack()
        manifest = self.fixture.plan()
        self.assertIsNone(manifest["sourcePack"]["declaredVersion"])
        self.assertIn("no version field",
                      manifest["sourcePack"]["declaredVersionNote"])

    def test_the_pack_identity_covers_files_the_importer_refused(self):
        self.populated_pack()
        before = self.fixture.plan()["sourcePack"]["contentDigest"]
        self.fixture.add("Characters/Thumbs.db", b"\x01")
        self.assertNotEqual(before,
                            self.fixture.plan()["sourcePack"]["contentDigest"])

    def test_the_crc_variant_is_recorded_as_unvalidated(self):
        self.populated_pack()
        manifest = self.fixture.plan()
        self.assertEqual(manifest["riceCrc"]["status"], "unvalidated")
        self.assertIn(manifest["riceCrc"]["variant"],
                      importer.rice_crc.VARIANTS)


class Coverage(RicepackTestCase):
    DIGEST_A = "0123456789abcdef0123456789abcdef"
    DIGEST_B = "fedcba9876543210fedcba9876543210"

    def crosswalk(self, table: dict[str, str]) -> Path:
        path = self.fixture.root / "crosswalk.json"
        path.write_text(json.dumps({
            "schemaVersion": importer.CROSSWALK_SCHEMA_VERSION,
            "variants": {name: dict(table)
                         for name in importer.rice_crc.VARIANTS},
        }), encoding="utf-8")
        return path

    def two_key_pack(self) -> None:
        self.fixture.add(name_for("AABBCCDD", 0, 2, "all"), solid_png())
        self.fixture.add(name_for("AABBCCDE", 0, 2, "all"), solid_png())

    def test_no_crosswalk_reports_zero_and_never_claims_completeness(self):
        self.two_key_pack()
        coverage = self.fixture.plan()["coverage"]
        self.assertEqual(coverage["keysMapped"], 0)
        self.assertEqual(coverage["keysMappable"], 2)
        self.assertFalse(coverage["complete"])
        self.assertIn("0 of 2", coverage["statement"])

    def test_partial_coverage_is_reported_as_partial(self):
        self.two_key_pack()
        path = self.crosswalk({"AABBCCDD#0#2#": self.DIGEST_A})
        coverage = self.fixture.plan("--crosswalk", str(path))["coverage"]
        self.assertEqual(coverage["keysMapped"], 1)
        self.assertEqual(coverage["keysUnmapped"], 1)
        self.assertEqual(coverage["percentOfMappable"], 50.0)
        self.assertFalse(coverage["complete"])

    def test_completeness_needs_every_key_and_no_refusal(self):
        self.two_key_pack()
        path = self.crosswalk({"AABBCCDD#0#2#": self.DIGEST_A,
                               "AABBCCDE#0#2#": self.DIGEST_B})
        coverage = self.fixture.plan("--crosswalk", str(path))["coverage"]
        self.assertTrue(coverage["complete"])

        self.fixture.add("Thumbs.db", b"\x00")
        coverage = self.fixture.plan("--crosswalk", str(path))["coverage"]
        self.assertFalse(coverage["complete"])
        self.assertEqual(coverage["filesRefused"], 1)

    def test_a_refused_key_keeps_completeness_false(self):
        self.two_key_pack()
        self.fixture.add(name_for("AABBCCE0", 0, 2, "a"), solid_png())
        path = self.crosswalk({"AABBCCDD#0#2#": self.DIGEST_A,
                               "AABBCCDE#0#2#": self.DIGEST_B})
        coverage = self.fixture.plan("--crosswalk", str(path))["coverage"]
        self.assertEqual(coverage["keysDiscovered"], 3)
        self.assertEqual(coverage["keysMappable"], 2)
        self.assertEqual(coverage["keysMapped"], 2)
        self.assertFalse(coverage["complete"])

    def test_a_crosswalk_with_a_bad_digest_is_refused(self):
        self.two_key_pack()
        path = self.crosswalk({"AABBCCDD#0#2#": "NOT-A-DIGEST"})
        with self.assertRaises(SystemExit):
            self.fixture.plan("--crosswalk", str(path))


class Materialising(RicepackTestCase):
    DIGEST_A = "0123456789abcdef0123456789abcdef"
    DIGEST_B = "fedcba9876543210fedcba9876543210"

    def build(self, *extra: str) -> tuple[Path, dict]:
        out = self.fixture.root / "content-pack"
        manifest = self.fixture.root / "manifest.json"
        with redirect_stdout(StringIO()):
            code = importer.main(["build", str(self.fixture.pack),
                                  "--manifest", str(manifest),
                                  "--out", str(out), "--name", "Test Pack",
                                  *extra])
        return out, {"code": code,
                     "manifest": json.loads(manifest.read_text("utf-8"))
                     if manifest.exists() else {}}

    def crosswalk(self, table: dict[str, str]) -> Path:
        path = self.fixture.root / "crosswalk.json"
        path.write_text(json.dumps({
            "schemaVersion": importer.CROSSWALK_SCHEMA_VERSION,
            "variants": {name: dict(table)
                         for name in importer.rice_crc.VARIANTS},
        }), encoding="utf-8")
        return path

    def test_an_all_image_is_copied_byte_for_byte(self):
        payload = solid_png(colour=(9, 8, 7, 6))
        self.fixture.add(name_for("AABBCCDD", 0, 2, "all"), payload)
        path = self.crosswalk({"AABBCCDD#0#2#": self.DIGEST_A})
        out, result = self.build("--crosswalk", str(path))
        self.assertEqual(result["code"], 0)
        self.assertEqual((out / "textures" / f"{self.DIGEST_A}.png").read_bytes(),
                         payload)
        self.assertIn("name = Test Pack",
                      (out / "pack.ini").read_text(encoding="utf-8"))

    def test_a_split_pair_takes_alpha_from_the_coverage_image_red_channel(self):
        """Measured, not assumed.

        In the sample pack the `_a` files are greyscale carried in RGB, and the
        ones that have an alpha channel of their own have it uniformly opaque.
        Reading that channel would produce a fully opaque texture.
        """
        colour = png_probe.encode_rgba(2, 1, bytes([10, 20, 30, 255,
                                                    40, 50, 60, 255]))
        coverage = png_probe.encode_rgba(2, 1, bytes([70, 70, 70, 255,
                                                      90, 90, 90, 255]))
        self.fixture.add(name_for("AABBCCDD", 0, 3, "rgb"), colour)
        self.fixture.add(name_for("AABBCCDD", 0, 3, "a"), coverage)
        path = self.crosswalk({"AABBCCDD#0#3#": self.DIGEST_A})
        out, _ = self.build("--crosswalk", str(path))
        _, pixels = png_probe.decode_rgba(
            (out / "textures" / f"{self.DIGEST_A}.png").read_bytes())
        self.assertEqual(bytes(pixels), bytes([10, 20, 30, 70, 40, 50, 60, 90]))

    def test_an_unmapped_key_writes_nothing_and_is_reported(self):
        self.fixture.add(name_for("AABBCCDD", 0, 2, "all"), solid_png())
        self.fixture.add(name_for("AABBCCDE", 0, 2, "all"), solid_png())
        path = self.crosswalk({"AABBCCDD#0#2#": self.DIGEST_A})
        out, result = self.build("--crosswalk", str(path))
        self.assertEqual(
            sorted(p.name for p in (out / "textures").iterdir()),
            [f"{self.DIGEST_A}.png"])
        self.assertEqual(result["manifest"]["coverage"]["keysMapped"], 1)
        self.assertFalse(result["manifest"]["coverage"]["complete"])

    def test_an_over_long_pack_name_is_refused_before_anything_is_written(self):
        self.fixture.add(name_for("AABBCCDD", 0, 2, "all"), solid_png())
        out = self.fixture.root / "content-pack"
        manifest = self.fixture.root / "manifest.json"
        with redirect_stdout(StringIO()), redirect_stderr(StringIO()):
            code = importer.main(
                ["build", str(self.fixture.pack), "--manifest", str(manifest),
                 "--out", str(out), "--name", "x" * 200])
        self.assertEqual(code, 1)
        self.assertFalse(out.exists())


class CrosswalkBuilder(RicepackTestCase):
    DIGEST = "0123456789abcdef0123456789abcdef"

    DIGEST_B = "fedcba9876543210fedcba9876543210"

    # An 8x4 RGBA16 tile: rows are 16 bytes of picture, 24 bytes apart, so the
    # last eight bytes of every row are addressing padding and must be outside
    # the CRC. Every fixture below is built from this one so that "the padding
    # is skipped" is asserted by the numbers rather than by a comment.
    RECORD_V2 = ("dump_format=2\nwidth=8\nheight=4\nfmt=0\nsiz=2\n"
                 "source_width=8\nsource_height=4\nsource_line_bytes=24\n"
                 "source_size_bytes=96\nsource_texel_bytes=96\n"
                 "first_seen=frame 10, texture unit 0\n")
    SPAN_V2 = bytes(range(96))

    def dump_dir(self) -> Path:
        dump = self.fixture.root / "dump"
        dump.mkdir(exist_ok=True)
        return dump

    def write_record(self, dump: Path, digest: str, record: str,
                     span: bytes | None) -> None:
        (dump / f"{digest}.txt").write_text(record, encoding="utf-8")
        if span is not None:
            (dump / f"{digest}.texels").write_bytes(span)

    def test_a_corpus_with_no_texel_span_is_reported_as_lacking_one(self):
        """A dump from a build predating format 2 -- readable, not usable."""
        dump = self.dump_dir()
        self.write_record(
            dump, self.DIGEST,
            "width=32\nheight=32\nfmt=0\nsiz=2\nfirst_seen=frame 10\n", None)
        result = importer.build_crosswalk(dump)
        self.assertEqual(result["texturesMapped"], 0)
        self.assertEqual(result["skipped"],
                         [{"digest": self.DIGEST,
                           "reason": "dump-lacks-texel-span"}])
        # An unversioned record IS version 1; that is what keeps it readable.
        self.assertEqual(result["dumpFormatsRead"],
                         [importer.DUMP_FORMAT_UNVERSIONED])

    def test_a_widened_dump_yields_one_key_per_variant(self):
        dump = self.dump_dir()
        self.write_record(dump, self.DIGEST, self.RECORD_V2, self.SPAN_V2)
        result = importer.build_crosswalk(dump)
        self.assertEqual(result["skipped"], [])
        self.assertEqual(result["texturesMapped"], 1)
        self.assertEqual(result["dumpFormatsRead"], [2])
        self.assertEqual(result["dumpFormatSupported"], 2)
        for variant in importer.rice_crc.VARIANTS:
            table = result["variants"][variant]
            self.assertEqual(len(table), 1)
            key = next(iter(table))
            self.assertRegex(key, r"^[0-9A-F]{8}#0#2#$")
            self.assertEqual(table[key], self.DIGEST)
        self.assertEqual(result["crcStatus"], "unvalidated")

    def test_the_source_geometry_is_read_not_the_dumped_buffers(self):
        """The two are the same picture only when no pack was installed.

        `width`/`height` describe the PNG that was written, which for a run
        with a pack installed is the replacement's own size. Hashing the ROM
        span against those dimensions would produce a confident wrong CRC, so
        the record's source_* fields have to win. The fixture makes them
        disagree in a way that cannot be missed: a 256x256 buffer over a 8x4
        tile does not even fit the span, so reading the wrong pair is a
        rejection rather than a subtly different number.
        """
        dump = self.dump_dir()
        record = self.RECORD_V2.replace("width=8\nheight=4\n",
                                        "width=256\nheight=256\n")
        self.assertIn("source_width=8", record)
        self.write_record(dump, self.DIGEST, record, self.SPAN_V2)
        result = importer.build_crosswalk(dump)
        self.assertEqual(result["skipped"], [])
        self.assertEqual(result["texturesMapped"], 1)

        # And the CRC is the same one the agreeing record produces, so this is
        # asserting which fields were read, not merely that nothing crashed.
        other = self.fixture.root / "dump-agreeing"
        other.mkdir()
        self.write_record(other, self.DIGEST, self.RECORD_V2, self.SPAN_V2)
        self.assertEqual(result["variants"],
                         importer.build_crosswalk(other)["variants"])

    def test_a_span_too_short_for_its_geometry_is_refused_not_hashed(self):
        """A CRC over fewer rows than the emulator hashed can only ever miss.

        This is what an arena-clamped span looks like: source_size_bytes says
        96, the file holds 72. Silently hashing three rows would put a
        plausible number in the crosswalk that no pack key can match, and the
        sweep would read as "the algorithm is wrong".
        """
        dump = self.dump_dir()
        self.write_record(dump, self.DIGEST, self.RECORD_V2, self.SPAN_V2[:72])
        result = importer.build_crosswalk(dump)
        self.assertEqual(result["texturesMapped"], 0)
        self.assertEqual(len(result["skipped"]), 1)
        self.assertTrue(
            result["skipped"][0]["reason"].startswith("crc-input-rejected"),
            result["skipped"][0])

    def test_a_tile_with_no_resolvable_geometry_is_refused(self):
        """Zero rows hash to a CONSTANT, which is worse than no answer.

        The engine records 0x0 when it could not resolve a tile's logical
        dimensions. Hashing that walks no rows and returns 00000000 in three of
        the four variants -- the same value for every such texture, colliding
        with each other and able to match a pack file literally named
        00000000. A hit like that measures nothing, so the record never enters
        the table.
        """
        dump = self.dump_dir()
        self.write_record(
            dump, self.DIGEST,
            self.RECORD_V2.replace("source_width=8\nsource_height=4\n",
                                   "source_width=0\nsource_height=0\n"),
            self.SPAN_V2)
        result = importer.build_crosswalk(dump)
        self.assertEqual(result["texturesMapped"], 0)
        self.assertEqual(result["skipped"],
                         [{"digest": self.DIGEST,
                           "reason": "dump-geometry-degenerate"}])

    def test_a_future_dump_format_is_refused_rather_than_read_hopefully(self):
        dump = self.dump_dir()
        self.write_record(dump, self.DIGEST,
                          self.RECORD_V2.replace("dump_format=2",
                                                 "dump_format=99"),
                          self.SPAN_V2)
        result = importer.build_crosswalk(dump)
        self.assertEqual(result["texturesMapped"], 0)
        self.assertEqual(result["skipped"],
                         [{"digest": self.DIGEST,
                           "reason": "dump-format-unsupported: 99"}])

    def test_two_textures_on_one_rice_key_are_counted_not_hidden(self):
        """Byte-identical spans in two records collide in every variant.

        A table that silently collapses textures onto keys could score hits
        without knowing anything, so the collision has to reach the report.
        """
        dump = self.dump_dir()
        self.write_record(dump, self.DIGEST, self.RECORD_V2, self.SPAN_V2)
        self.write_record(dump, self.DIGEST_B, self.RECORD_V2, self.SPAN_V2)
        result = importer.build_crosswalk(dump)
        self.assertEqual(result["texturesMapped"], 1)
        for variant in importer.rice_crc.VARIANTS:
            self.assertEqual(result["keyCollisions"][variant], 1)

    def test_a_file_that_is_not_a_content_digest_is_skipped(self):
        dump = self.dump_dir()
        (dump / "notes.txt").write_text("nothing", encoding="utf-8")
        self.assertEqual(importer.build_crosswalk(dump)["skipped"],
                         [{"digest": "notes", "reason": "not-a-content-digest"}])


if __name__ == "__main__":
    unittest.main()
