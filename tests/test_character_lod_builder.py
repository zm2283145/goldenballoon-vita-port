#!/usr/bin/env python3
"""Executable and hostile contracts for recorded offline LOD generation."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import character_asset_compiler as compiler
import character_asset_probe as probe
import character_lod_builder as lod

from test_high_fidelity_character import make_grid_glb, make_manifest


HELPER: Path


def rewrite_document(model: bytes, update) -> bytes:
    document, binary = probe.parse_glb(model)
    update(document)
    encoded = json.dumps(document, separators=(",", ":")).encode("utf-8")
    encoded += b" " * ((-len(encoded)) % 4)
    payload = binary or b""
    output = bytearray(struct.pack("<4sII", b"glTF", 2, 0))
    output += struct.pack("<II", len(encoded), probe.GLB_JSON_CHUNK) + encoded
    output += struct.pack("<II", len(payload), probe.GLB_BIN_CHUNK) + payload
    struct.pack_into("<I", output, 8, len(output))
    return bytes(output)


class CharacterLodBuilderTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.source = make_grid_glb(side=40, authored_tangent=(1.0, 0.0, 0.0, 1.0))

    def test_deterministic_compact_chain_compiles(self) -> None:
        source_digest = hashlib.sha256(self.source).hexdigest()
        first, report = lod.build_lods(self.source, HELPER)
        second, second_report = lod.build_lods(self.source, HELPER)
        self.assertEqual(first, second)
        self.assertEqual(report, second_report)
        self.assertEqual(source_digest, hashlib.sha256(self.source).hexdigest())
        self.assertEqual(lod.SCHEMA, report["schema"])
        self.assertEqual(lod.MESHOPTIMIZER_COMMIT, report["algorithm_commit"])
        self.assertEqual(3, len(report["levels"]))

        inspected = probe.inspect_glb_bytes(first, require_character=True)
        self.assertEqual([], inspected["errors"])
        document, _ = probe.parse_glb(first)
        self.assertIn("MSFT_lod", document["extensionsRequired"])
        provenance = document["asset"]["extras"][
            "org.goldenballoon.character_lod_v1"
        ]
        self.assertEqual(lod.MESHOPTIMIZER_COMMIT, provenance["commit"])
        self.assertEqual(3, len(provenance["levels"]))
        self.assertEqual(
            [4.0, 4.0, 4.0, 4.0],
            provenance["attribute_metric"]["JOINTS_0_if_present"],
        )

        compiled, compile_report = compiler.compile_character(
            first, make_manifest(), bytes(32)
        )
        self.assertGreater(len(compiled), 0)
        self.assertEqual(4, compile_report["lod_levels"])
        triangles = compile_report["lod_triangles"]
        vertices = compile_report["lod_vertices"]
        self.assertTrue(all(
            triangles[index] > triangles[index + 1]
            for index in range(3)
        ), triangles)
        self.assertTrue(all(
            vertices[index] > vertices[index + 1]
            for index in range(3)
        ), vertices)
        for level, facts in enumerate(report["levels"], start=1):
            self.assertEqual(triangles[level], facts["triangles"])
            self.assertEqual(vertices[level], facts["vertices"])
            self.assertLessEqual(
                facts["maximum_accumulated_error"],
                sum(lod.DEFAULT_ERROR_LIMITS[:level]) + 1.0e-6,
            )

    def test_mesh_node_animation_is_duplicated_for_each_lod(self) -> None:
        animated = rewrite_document(
            self.source,
            lambda document: document["animations"][0]["channels"][0][
                "target"
            ].update({"node": 2}),
        )
        output, _ = lod.build_lods(
            animated, HELPER, ratios=(0.5,), error_limits=(0.01,)
        )
        document, _ = probe.parse_glb(output)
        ids = document["nodes"][2]["extensions"]["MSFT_lod"]["ids"]
        targets = [
            channel["target"]["node"]
            for channel in document["animations"][0]["channels"]
        ]
        self.assertEqual([2, *ids], targets)
        self.assertEqual([], probe.inspect_glb_bytes(
            output, require_character=True
        )["errors"])

    def test_existing_lods_and_hostile_settings_fail_closed(self) -> None:
        generated, _ = lod.build_lods(
            self.source, HELPER, ratios=(0.5,), error_limits=(0.01,)
        )
        with self.assertRaisesRegex(lod.LodBuildError, "already has authored LODs"):
            lod.build_lods(generated, HELPER)
        for ratios, errors in (
            ((), ()),
            ((0.5, 0.6), (0.01, 0.02)),
            ((0.5,), (float("nan"),)),
            ((0.5,), (0.3,)),
        ):
            with self.assertRaises(lod.LodBuildError):
                lod.build_lods(
                    self.source, HELPER, ratios=ratios, error_limits=errors
                )
        with self.assertRaisesRegex(lod.LodBuildError, "must be absolute"):
            lod.build_lods(self.source, Path("mdkr-character-lod"))

    def test_helper_protocol_rejects_noncanonical_and_out_of_range_input(self) -> None:
        import subprocess

        malformed = subprocess.run(
            [str(HELPER)], input=b"MDKRLOD1", capture_output=True, check=False
        )
        self.assertNotEqual(0, malformed.returncode)
        self.assertIn(b"header is invalid", malformed.stderr)

        positions = struct.pack("<9f", 0.0, 0.0, 0.0,
                                1.0, 0.0, 0.0, 0.0, 1.0, 0.0)
        attributes = struct.pack("<3f", 0.0, 1.0, 2.0)
        request = (
            lod.PROTOCOL_MAGIC
            + struct.pack("<6If", 1, 3, 3, 1, 3, 0, 0.01)
            + positions + attributes + struct.pack("<f", 1.0)
            + struct.pack("<3I", 0, 1, 3)
        )
        hostile = subprocess.run(
            [str(HELPER)], input=request, capture_output=True, check=False
        )
        self.assertNotEqual(0, hostile.returncode)
        self.assertIn(b"index exceeds", hostile.stderr)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--helper", type=Path, required=True)
    args, remaining = parser.parse_known_args()
    HELPER = args.helper.resolve()
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(
        CharacterLodBuilderTests
    )
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    raise SystemExit(0 if result.wasSuccessful() else 1)
