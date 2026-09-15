#!/usr/bin/env python3
"""ROM-free event-context reporting regressions; never launches the game."""

import contextlib
import io
import json
import unittest
from unittest import mock

import check_camera_motion_quality as motion


class MotionReportingTests(unittest.TestCase):
    def test_only_exact_event_rows_are_retained(self):
        event = "camera_motion reengagement tick=42 viewport=0 gap=11"
        output = ("camera_motion summary slot_ticks=100\n"
                  f"prefix {event}\n{event}\n"
                  "camera_motion detail tick=42\n")
        self.assertEqual(motion.reengagement_evidence(output), [event])

    def test_report_keeps_arm_identity_and_event_context(self):
        output = "camera_motion reengagement tick=42 viewport=1 gap=11\n"
        captured = io.StringIO()
        with contextlib.redirect_stdout(captured):
            motion.report_reengagements("route@240", output, 1)
        report = captured.getvalue()
        self.assertIn("[route@240]", report)
        self.assertIn("1 event rows, 1 census events", report)
        self.assertIn(output.strip(), report)
        self.assertNotIn("incomplete", report)

    def test_missing_or_extra_context_does_not_claim_complete_evidence(self):
        for output, count in (("", 1),
                              ("camera_motion reengagement tick=42\n", 0)):
            with self.subTest(output=output, count=count):
                captured = io.StringIO()
                with contextlib.redirect_stdout(captured):
                    motion.report_reengagements("route", output, count)
                self.assertIn("aggregate hard verdict is unchanged",
                              captured.getvalue())

    def test_console_is_bounded_without_dropping_retained_rows(self):
        output = "\n".join(f"camera_motion reengagement tick={tick}"
                           for tick in range(20))
        self.assertEqual(len(motion.reengagement_evidence(output)), 20)
        captured = io.StringIO()
        with contextlib.redirect_stdout(captured):
            motion.report_reengagements("route", output, 20)
        self.assertEqual(captured.getvalue().count("camera_motion reengagement"), 16)
        self.assertIn("4 additional rows omitted from console", captured.getvalue())
        self.assertIn("--baseline-out", captured.getvalue())

    def test_empty_clean_run_adds_no_failure_context(self):
        captured = io.StringIO()
        with contextlib.redirect_stdout(captured):
            motion.report_reengagements("route", "", 0)
        self.assertEqual(captured.getvalue(), "")

    def test_failed_authored_and_high_rate_arms_keep_separate_evidence(self):
        arm = motion.HIGH_RATE_ARMS[0]
        authored = "camera_motion reengagement tick=42 viewport=0 gap=11"
        high_rate = "camera_motion reengagement tick=84 viewport=1 gap=10"
        summary = {"correction_reengagements": 1}
        captured = io.StringIO()
        # Exercise main's real report/serialization path, replacing only the
        # process/measurement boundaries. No product or ROM process is launched.
        with mock.patch("sys.argv", ["check_camera_motion_quality.py",
                                     "--only", arm.route,
                                     "--baseline-out", "unused.json"]), \
                mock.patch.object(motion, "resolve_binary", return_value="unused-game"), \
                mock.patch.object(motion.Path, "is_file", return_value=True), \
                mock.patch.object(motion.Path, "write_text") as write, \
                mock.patch.object(motion, "run_route", side_effect=[authored, high_rate]), \
                mock.patch.object(motion, "inspect",
                                  return_value=(summary, {}, ["authored hard failure"])), \
                mock.patch.object(motion, "inspect_high_rate",
                                  return_value=({"summary": summary},
                                                ["high-rate hard failure"])), \
                mock.patch.object(motion, "report_baseline"), \
                contextlib.redirect_stdout(captured), \
                contextlib.redirect_stderr(captured):
            self.assertEqual(motion.main(), 1)
        saved = json.loads(write.call_args.args[0])
        self.assertEqual(saved["routes"][arm.route]["reengagement_events"], [authored])
        label = f"{arm.route}@{arm.present_rate}"
        self.assertEqual(saved["high_rate"][label]["reengagement_events"], [high_rate])
        report = captured.getvalue()
        self.assertIn(f"[{label}] reengagement evidence", report)
        self.assertIn("authored hard failure", report)
        self.assertIn("high-rate hard failure", report)


if __name__ == "__main__":
    unittest.main()
