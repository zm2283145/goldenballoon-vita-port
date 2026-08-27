#!/usr/bin/env python3
"""Pure boundary tests for the isolated character-spike evidence runner."""

from __future__ import annotations

import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import run_character_spike_evidence as evidence  # noqa: E402


class CharacterSpikeEvidenceTests(unittest.TestCase):
    def test_redaction_removes_every_private_path_and_bounds_logs(self) -> None:
        private = [
            "/private/source/model.glb",
            "/private/license.txt",
            "/private/rom.z64",
            "/private/temp-work",
        ]
        text = "\n".join(private) + "\nordinary diagnostic"
        redacted = evidence._redact(text, private)
        for path in private:
            self.assertNotIn(path, redacted)
        self.assertIn("ordinary diagnostic", redacted)

    def test_ppm_parser_does_not_consume_whitespace_valued_pixels(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            ppm = Path(temporary) / "capture.ppm"
            # The first red byte is LF. Consuming arbitrary whitespace after
            # maxval would silently shift and corrupt this valid binary image.
            pixels = bytes((10, 32, 13, 255, 0, 127))
            ppm.write_bytes(b"P6\n2 1\n255\n" + pixels)
            width, height, parsed = evidence._ppm(ppm)
            self.assertEqual((2, 1, pixels), (width, height, parsed))
            png = evidence._rgb_png(width, height, parsed)
            self.assertEqual(b"\x89PNG\r\n\x1a\n", png[:8])

    def test_result_parser_requires_warmed_fit_pose_and_renderer_evidence(self) -> None:
        output = (
            "[TRACE] character_workshop_result: warmup=1 realtime=0 "
            "samples=60 p50us=1000 p95us=2000 p99us=3000 maxus=4000 "
            "replacements=61 contacts=60 contactMaxUm=42000 "
            "contactWitness=f contactWitnessErrorUm=10000,20000,30000,40000 "
            "fit=1 fitAnchorUm=0,0,0 fitBoundsYUm=0,1250000 "
            "fitForwardMilli=0,0,1000 pose=2 phase=500 poseTicks=60 "
            "poseFallback=0 gpu=3/3 sceneGpuNs=60,100,200 "
            "characterGpuNs=60,30,50 gpuExcluded=0,0,0 "
            "backend=webgpu-metal adapter=Fixture GPU driver=test "
            "vendor=00000001 device=00000002 output=1280x960 render=1280x960\n"
            "[WGPU-MODERN-CHARACTER] assetUploads=1 draws=120 "
            "triangles=240 refusedDraws=0\n"
        )
        parsed = evidence._parse_result(output, "car-1p")
        self.assertEqual(2000, parsed["wall_microseconds"]["p95"])
        self.assertEqual(42000, parsed["contacts"]["maximum_micrometres"])
        self.assertEqual([0, 1250000], parsed["fit"]["bounds_y_micrometres"])
        self.assertEqual("Fixture GPU", parsed["environment"]["adapter"])
        with self.assertRaisesRegex(evidence.EvidenceError, "package fallback"):
            evidence._parse_result(output.replace("poseFallback=0",
                                                  "poseFallback=1"), "car-1p")

    def test_existing_evidence_is_never_overwritten(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            evidence._ensure_evidence_destination(root / "new")
            occupied = root / "occupied"
            occupied.mkdir()
            (occupied / "evidence.json").write_text("preserve", encoding="utf-8")
            with self.assertRaisesRegex(evidence.EvidenceError, "never overwritten"):
                evidence._ensure_evidence_destination(occupied)
            self.assertEqual("preserve",
                             (occupied / "evidence.json").read_text(encoding="utf-8"))


if __name__ == "__main__":
    unittest.main()
