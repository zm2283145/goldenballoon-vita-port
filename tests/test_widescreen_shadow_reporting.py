#!/usr/bin/env python3
"""ROM-free shadow gate reporting regressions; process boundaries are mocked."""

import contextlib
from dataclasses import replace
import io
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest import mock

import check_widescreen_shadow as shadow


def sample(label="4:3", frames=4, traced=True, decal=True, overflow=0):
    pace = "".join(f"[PACE] frame={frame} R=2 dtms=1.0\n"
                   for frame in range(1, frames + 1)) if traced else ""
    output = (pace
              + f"[SHADOW] decal={'on' if decal else 'off'} "
                "dataPeak=1/2 triPeak=1/8 vtxPeak=1/16 "
              + f"overflowDrops={overflow} emptyMeshes=0 drawGroups=1 "
              + f"nonDecal={0 if decal else 1}\n"
              + f"[DEPTH] decalTriangles={2 if decal else 0} comparedTriangles=3\n"
              + f"[SDL] headless: reached {frames} frames, exiting cleanly.\n")
    return shadow.RunResult(label, ["unused-game"], 0, output,
                            shadow.normalized_pace(output),
                            shadow.parse_shadow(output), shadow.parse_depth(output),
                            frames, timeout=180)


class ShadowReportingTests(unittest.TestCase):
    def test_matching_complete_runs_pass_and_actual_difference_fails(self):
        reference = sample()
        candidate = sample("16:9")
        self.assertEqual(shadow.run_failures(reference), [])
        self.assertEqual(shadow.pace_comparison_failures(reference, candidate), [])
        candidate.pace[2] += " changed=1"
        self.assertIn("row 2 differs", shadow.pace_comparison_failures(
            reference, candidate)[0])

    def test_partial_lengths_or_values_never_claim_simulation_divergence(self):
        reference = replace(sample(), timed_out=True, returncode=124)
        for rows in (reference.pace[:-1], reference.pace,
                     ["[PACE] frame=1 changed=1"]):
            candidate = replace(sample("16:9"), timed_out=True,
                                returncode=124, pace=rows)
            failures = shadow.pace_comparison_failures(reference, candidate)
            self.assertTrue(failures)
            self.assertIn("comparison unavailable", failures[0])
            self.assertNotIn("simulation stream changed", failures[0])

    def test_successful_exit_requires_exact_single_completion(self):
        good = sample()
        marker = "[SDL] headless: reached 4 frames, exiting cleanly.\n"
        for replacement in ("", marker.replace("4 frames", "3 frames"), marker * 2):
            with self.subTest(replacement=replacement):
                result = replace(good, output=good.output.replace(marker, replacement))
                self.assertFalse(shadow.completed_route(result))
                self.assertTrue(shadow.run_failures(result))
                self.assertIn("comparison unavailable",
                              shadow.pace_comparison_failures(good, result)[0])

    def test_missing_duplicate_and_reordered_trace_rows_fail_closed(self):
        good = sample()
        for rows in (good.pace[:-1], good.pace + good.pace[-1:],
                     list(reversed(good.pace)), [good.pace[0]] * 4):
            with self.subTest(rows=rows):
                result = replace(good, pace=rows)
                self.assertFalse(shadow.complete_pace(result))
                self.assertTrue(shadow.run_failures(result))
                self.assertIn("comparison unavailable",
                              shadow.pace_comparison_failures(result, result)[0])

    def test_timeout_is_distinct_from_child_exit_124(self):
        timed = replace(sample(), timed_out=True, returncode=124)
        exited = replace(timed, timed_out=False)
        self.assertIn("timed out after 180s", shadow.run_failures(timed)[0])
        self.assertIn("exit code 124", shadow.run_failures(exited)[0])
        self.assertFalse(shadow.completed_route(timed))

    def test_both_missing_summaries_are_reported(self):
        result = replace(sample(), shadow=None, depth=None)
        failures = "\n".join(shadow.run_failures(result))
        self.assertIn("no parseable [SHADOW] report", failures)
        self.assertIn("no parseable [DEPTH] report", failures)

    def test_sanitizer_reports_fail_even_with_successful_exit(self):
        for marker in ("AddressSanitizer", "UndefinedBehaviorSanitizer",
                       "runtime error:"):
            with self.subTest(marker=marker):
                result = sample()
                result.output += marker + " diagnostic\n"
                self.assertIn("output contains " + marker,
                              "\n".join(shadow.run_failures(result)))

    def test_shipping_is_untraced_but_still_requires_completion(self):
        shipping = sample("4:3 shipping", traced=False)
        self.assertEqual(shadow.run_failures(shipping, traced=False), [])
        self.assertTrue(shadow.run_failures(replace(shipping, timed_out=True),
                                            traced=False))
        self.assertTrue(shadow.run_failures(sample(), traced=False))

    def test_timeout_bytes_preserve_summaries_without_accepting_completion(self):
        good = sample()
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            timeout = subprocess.TimeoutExpired(["unused"], 180,
                                                 output=good.output.encode(),
                                                 stderr=b"partial error \xff\n")
            with mock.patch.object(shadow.subprocess, "run", side_effect=timeout):
                result = shadow.run_case(Path("unused"), Path("unused-rom"), 4,
                                         "4:3", "640x480", {}, None, 180, False, root)
            self.assertTrue(result.timed_out)
            self.assertEqual(result.shadow, good.shadow)
            self.assertEqual(result.depth, good.depth)
            self.assertFalse(shadow.completed_route(result))
            self.assertIn("partial error \ufffd", result.output)
            self.assertEqual((root / "4_3/process.log").read_text(), result.output)
            record = json.loads((root / "4_3/process.json").read_text())
            self.assertTrue(record["timed_out"])
            self.assertEqual(record["requested_frames"], 4)
            self.assertEqual(record["timeout_seconds"], 180)
            with self.assertRaises(FileExistsError):
                shadow.write_evidence(root, result)

    def test_launch_failure_is_recorded_and_fails(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            with mock.patch.object(shadow.subprocess, "run",
                                   side_effect=OSError("launch unavailable")):
                result = shadow.run_case(Path("unused"), Path("unused-rom"), 4,
                                         "4:3 shipping", "640x480", {}, None,
                                         180, False, root)
            self.assertFalse(result.timed_out)
            self.assertEqual(result.returncode, 127)
            self.assertIn("launch unavailable", result.output)
            self.assertTrue(shadow.run_failures(result, traced=False))
            self.assertTrue((root / "4_3_shipping/process.json").is_file())

    def run_main(self, results):
        capture = io.StringIO()
        with mock.patch("sys.argv", ["check_widescreen_shadow.py"]), \
                mock.patch.object(shadow, "resolve_binary", return_value="unused-game"), \
                mock.patch.object(shadow.Path, "is_file", return_value=True), \
                mock.patch.object(shadow, "run_case", side_effect=results), \
                contextlib.redirect_stdout(capture), \
                contextlib.redirect_stderr(capture):
            code = shadow.main()
        return code, capture.getvalue()

    def arms(self):
        return [sample(), sample("16:9"), sample("21:9"),
                sample("tiny shadow heaps", overflow=1),
                sample("decal positive control", decal=False),
                sample("4:3 shipping", traced=False)]

    def test_main_passes_complete_sensitive_controls(self):
        code, output = self.run_main(self.arms())
        self.assertEqual(code, 0)
        self.assertIn("PASS:", output)

    def test_main_missing_baseline_depth_does_not_abort_other_reporting(self):
        results = self.arms()
        results[0].depth = None
        code, output = self.run_main(results)
        self.assertEqual(code, 1)
        self.assertIn("4:3: no parseable [DEPTH] report", output)
        self.assertIn("--- 4:3: last 50 output lines ---", output)

    def test_main_all_timeout_reports_include_shipping_and_no_false_divergence(self):
        results = [replace(result, returncode=124, timed_out=True,
                           pace=result.pace[:index], shadow=None, depth=None)
                   for index, result in enumerate(self.arms())]
        code, output = self.run_main(results)
        self.assertEqual(code, 1)
        self.assertEqual(output.count("timed out after 180s"), 6)
        for result in results:
            self.assertIn(f"--- {result.label}: last 50 output lines ---", output)
        self.assertNotIn("simulation stream changed", output)
        self.assertNotIn("PASS:", output)


if __name__ == "__main__":
    unittest.main()
