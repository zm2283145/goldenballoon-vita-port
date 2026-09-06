#!/usr/bin/env python3
"""ROM-free coverage for arm-owned pacing failure diagnostics; launches no app."""

import ast
import contextlib
import inspect
import io
import unittest

import check_pacing_quality as pacing


def sample_run(label: str, anchors: int) -> pacing.Run:
    return pacing.Run(
        label=label,
        output="[PRESENT-DISCIPLINE] active=1\n",
        summary={"slotanchors": anchors},
        hist={
            "displayed-interval": {"p50": 16000, "p95": 18000, "p99": 21000,
                                   "max": 23000, "var": 1000000, "n": 1795},
            "alpha-delta": {"p50": 500000, "p95": 500000, "p99": 1000000,
                            "var": 2000000, "gridppm": 500000,
                            "displayed": 1795, "stalls": 0, "regressions": 0,
                            "binwidth": 1000},
        },
        latency={"meandepthmilli": 1000, "maxdepth": 1,
                 "meanlatencyus": 16666, "maxlatencyus": 16666, "refreshhz": 60},
        audio={}, pressure={}, arm="display/smoothing/realtime",
    )


class PacingReportingTests(unittest.TestCase):
    def test_slot_failure_keeps_its_own_distribution_and_counter(self):
        strict = sample_run("strict-attempt", 0)
        companion = sample_run("companion-retry2", 43)
        failures = pacing.check_slot_quality(companion)
        self.assertEqual(len(failures), 1)
        self.assertIn("43 slot re-anchors exceed 3", failures[0])
        self.assertEqual(pacing.check_slot_quality(strict), [])
        measured = pacing.baseline_note(companion)
        for expected in ("companion-retry2", "p95=500000ppm", "grid=500000ppm",
                         "displayed=1795", "stalls=0", "regressions=0",
                         "slotanchors=43"):
            self.assertIn(expected, measured)
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            status = pacing.report_failure(
                failures, [pacing.baseline_note(strict)],
                [f"slot measurement: {measured}", "retry classifier evidence"])
        self.assertEqual(status, 1)
        self.assertIn("check_pacing_quality: FAIL", output.getvalue())
        self.assertNotIn("PASS", output.getvalue())
        self.assertIn("measured: strict-attempt", output.getvalue())
        self.assertIn("slot measurement: companion-retry2", output.getvalue())
        self.assertIn("slotanchors=43", output.getvalue())
        self.assertIn("retry classifier evidence", output.getvalue())

    def test_main_routes_companion_evidence_without_promoting_strict_baseline(self):
        calls = [node for node in ast.walk(ast.parse(inspect.getsource(pacing.main)))
                 if isinstance(node, ast.Call)]
        destinations = []
        for call in calls:
            if not (isinstance(call.func, ast.Attribute) and
                    call.func.attr == "append" and
                    isinstance(call.func.value, ast.Name)):
                continue
            if any(isinstance(node, ast.Call) and
                   isinstance(node.func, ast.Name) and node.func.id == "baseline_note" and
                   len(node.args) == 1 and isinstance(node.args[0], ast.Name) and
                   node.args[0].id == "honest_result" for node in ast.walk(call)):
                destinations.append(call.func.value.id)
        self.assertEqual(destinations, ["notes"])
        reports = [call for call in calls if isinstance(call.func, ast.Name) and
                   call.func.id == "report_failure"]
        self.assertEqual(len(reports), 1)
        self.assertEqual([arg.id for arg in reports[0].args if isinstance(arg, ast.Name)],
                         ["failures", "baselines", "notes"])


if __name__ == "__main__":
    unittest.main()
