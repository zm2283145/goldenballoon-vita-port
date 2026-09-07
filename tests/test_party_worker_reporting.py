#!/usr/bin/env python3
"""Privacy-safe Worker reporting regressions; browser/process boundaries mocked."""

import argparse
import contextlib
import io
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest import mock

import check_browser_online_two_person as browser
import check_party_service_chaos as chaos
import party_worker_reporting as reporting


class WorkerReportingTests(unittest.TestCase):
    def test_untrusted_exception_messages_are_never_the_summary(self):
        for error, category in (
            (browser.CheckFailure("SECRET invite/body"), "check_failed"),
            (subprocess.CalledProcessError(1, ["SECRET"], output="SECRET"), "process_failed"),
            (OSError("SECRET"), "local_io_failed"),
            (ValueError("SECRET"), "unexpected_failure"),
        ):
            with self.subTest(category=category):
                self.assertEqual(reporting.safe_failure_summary(error), category)

    def test_source_callsite_is_kept_instead_of_browser_state(self):
        cdp = mock.Mock()
        cdp.evaluate.return_value = {"clicked": False, "credential": "SECRET"}
        try:
            browser.click_action(cdp, 1)
        except browser.CheckFailure as error:
            summary = reporting.safe_failure_summary(error)
        else:
            self.fail("fixture did not fail the action assertion")
        self.assertIn("tests/check_browser_online_two_person.py:", summary)
        self.assertNotIn("check_browser_runtime.py", summary)
        self.assertNotIn("SECRET", summary)

    def test_private_evidence_never_copies_raw_log_or_failure(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            original_mkstemp = tempfile.mkstemp
            def private_file(**kwargs):
                return original_mkstemp(dir=root, **kwargs)
            with (root / "worker.log").open("wb") as log:
                log.write(b"ERROR Cannot find module SECRET binding=value\n")
                with mock.patch.object(reporting.tempfile, "mkstemp", side_effect=private_file):
                    error = reporting.private_worker_failure(
                        browser.CheckFailure("SECRET response"), log,
                        reason="worker_log_error")
            evidence = list(root.glob("mdkr-worker-startup-*.json"))
            self.assertEqual(len(evidence), 1)
            text = evidence[0].read_text()
            detail = json.loads(text)
            self.assertEqual(detail["reason"], "worker_log_error")
            self.assertEqual(detail["signals"], ["module_not_found"])
            self.assertFalse(detail["rawOutputRetained"])
            summary = reporting.safe_failure_summary(error)
            self.assertIn("worker_log_error", summary)
            self.assertIn(str(evidence[0]), summary)
            for forbidden in ("SECRET", "binding=value", "Cannot find module"):
                self.assertNotIn(forbidden, text + summary)

    def test_evidence_failure_does_not_mask_original_category(self):
        with mock.patch.object(reporting, "startup_diagnostic", side_effect=OSError("SECRET")):
            error = reporting.private_worker_failure(ValueError("SECRET"), None)
        self.assertIn("unexpected_failure", str(error))
        self.assertIn("diagnostic unavailable", str(error))
        self.assertNotIn("SECRET", str(error))

    def test_already_sanitized_failure_keeps_diagnostic_identity_without_rescanning(self):
        original = reporting.SanitizedWorkerFailure("early_exit; private diagnostic: fixture.json")
        with mock.patch.object(reporting, "startup_diagnostic") as scan:
            self.assertIs(reporting.private_worker_failure(original, None), original)
            self.assertEqual(reporting.safe_failure_summary(original), str(original))
            scan.assert_not_called()

    def test_both_cli_failure_boundaries_sanitize_and_remain_nonzero(self):
        for module in (browser, chaos):
            for error, category in ((browser.CheckFailure("SECRET"), "check_failed"),
                                    (RuntimeError("SECRET"), "unexpected_failure")):
                with self.subTest(module=module.__name__, category=category):
                    output = io.StringIO()
                    with mock.patch.object(module.sys, "argv", [module.__file__]), \
                            mock.patch.object(module, "run", side_effect=error), \
                            contextlib.redirect_stderr(output):
                        result = module.main()
                    self.assertEqual(result, 1)
                    self.assertIn("FAIL", output.getvalue())
                    self.assertIn(category, output.getvalue())
                    self.assertNotIn("SECRET", output.getvalue())

    def test_whole_log_error_gate_keeps_chunk_boundary_and_case_semantics(self):
        with tempfile.TemporaryDirectory() as temp:
            log = Path(temp) / "worker.log"
            log.write_bytes(b"x" * (64 * 1024 - 2) + b"eRrOr SECRET" + b"x" * 100)
            with mock.patch.object(reporting, "startup_diagnostic", return_value="safe evidence"):
                with self.assertRaises(reporting.SanitizedWorkerFailure) as caught:
                    chaos.require_clean_worker_log(log)
            self.assertIn("worker_log_error", str(caught.exception))
            self.assertNotIn("SECRET", str(caught.exception))
            log.write_bytes(b"ordinary Worker output\n" * 10000)
            with mock.patch.object(reporting, "startup_diagnostic") as scan:
                chaos.require_clean_worker_log(log)
                scan.assert_not_called()

    def test_two_person_startup_failure_still_closes_owned_worker(self):
        process = mock.Mock(returncode=None)
        process.poll.return_value = None
        args = argparse.Namespace(shell_dir="unused", timeout=1)
        with mock.patch.object(Path, "is_file", return_value=True), \
                mock.patch.object(browser, "free_port", return_value=12345), \
                mock.patch.object(browser, "node_binary", return_value=Path("node")), \
                mock.patch.object(browser.subprocess, "Popen", return_value=process), \
                mock.patch.object(browser, "wait_worker", side_effect=ValueError("SECRET")), \
                mock.patch.object(reporting, "startup_diagnostic", return_value="safe evidence"):
            with self.assertRaises(reporting.SanitizedWorkerFailure) as caught:
                browser.run(args)
        self.assertIn("unexpected_failure", str(caught.exception))
        self.assertNotIn("SECRET", str(caught.exception))
        self.assertTrue(caught.exception.__suppress_context__)
        process.terminate.assert_called_once_with()
        process.wait.assert_called_once_with(timeout=5)

    def test_chaos_assertion_failure_still_halts_owned_worker(self):
        process = mock.Mock()
        args = argparse.Namespace(shell_dir="unused", verbose=False)
        with mock.patch.object(Path, "is_file", return_value=True), \
                mock.patch.object(chaos, "free_port", return_value=12345), \
                mock.patch.object(chaos, "start_worker", return_value=process), \
                mock.patch.object(chaos, "stop_worker") as stop, \
                mock.patch.object(chaos, "establish", side_effect=browser.CheckFailure("SECRET")), \
                mock.patch.object(reporting, "startup_diagnostic", return_value="safe evidence"):
            with self.assertRaises(reporting.SanitizedWorkerFailure) as caught:
                chaos.run(args)
        self.assertIn("check_failed", str(caught.exception))
        self.assertNotIn("SECRET", str(caught.exception))
        stop.assert_called_once_with(process)


if __name__ == "__main__":
    unittest.main()
