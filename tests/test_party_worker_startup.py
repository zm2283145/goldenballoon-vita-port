#!/usr/bin/env python3
"""Worker admission/ownership regressions; all process/network boundaries mocked."""

import contextlib
import io
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest import mock

import check_party_capacity as worker
import check_party_firewall_negative as firewall


class WorkerStartupTests(unittest.TestCase):
    def test_missing_dependency_refuses_before_node_or_process(self):
        with tempfile.TemporaryDirectory() as temp, \
                mock.patch.object(worker, "WRANGLER", Path(temp) / "missing.js"), \
                mock.patch.object(worker, "node_binary") as node, \
                mock.patch.object(worker.subprocess, "Popen") as popen:
            with self.assertRaisesRegex(worker.CheckFailure, "package-lock.json"):
                worker.start_worker("http://127.0.0.1:1", Path(temp), Path(temp), None, 20)
            with self.assertRaisesRegex(worker.CheckFailure, "npm ci"):
                firewall.start_worker("http://127.0.0.1:1", Path(temp), Path(temp), None)
            node.assert_not_called()
            popen.assert_not_called()

    def test_direct_command_helper_also_admits_dependency(self):
        with tempfile.TemporaryDirectory() as temp, \
                mock.patch.object(worker, "WRANGLER", Path(temp) / "missing.js"), \
                mock.patch.object(worker.subprocess, "Popen") as popen:
            with self.assertRaises(worker.CheckFailure):
                worker.start_worker_command(["must-not-spawn"], "unused", None)
            popen.assert_not_called()

    def test_success_transfers_process_without_changing_command_or_budget(self):
        process = mock.Mock()
        log = mock.Mock()
        command = ["pinned-node", "pinned-wrangler", "dev", "--local"]
        with mock.patch.object(worker, "require_worker_dependency"), \
                mock.patch.object(worker, "worker_log_size", return_value=12), \
                mock.patch.object(worker.subprocess, "Popen", return_value=process) as popen, \
                mock.patch.object(worker, "wait_worker") as wait, \
                mock.patch.object(worker, "stop_worker") as stop:
            self.assertIs(worker.start_worker_command(command, "origin", log), process)
            popen.assert_called_once_with(command, cwd=worker.SERVICE, stdout=log,
                                          stderr=subprocess.STDOUT)
            wait.assert_called_once_with("origin", process, 60)
            stop.assert_not_called()

    def test_readiness_failure_reclaims_untransferred_process_and_hides_error_text(self):
        for returncode, reason in ((None, "readiness_failed"), (1, "early_exit")):
            with self.subTest(returncode=returncode):
                process = mock.Mock()
                process.poll.return_value = returncode
                stop = mock.Mock()
                with mock.patch.object(worker, "require_worker_dependency"), \
                        mock.patch.object(worker, "worker_log_size", return_value=42), \
                        mock.patch.object(worker.subprocess, "Popen", return_value=process), \
                        mock.patch.object(worker, "wait_worker", side_effect=worker.CheckFailure("SECRET")), \
                        mock.patch.object(worker, "startup_diagnostic", return_value="safe metadata") as diagnostic:
                    with self.assertRaises(worker.CheckFailure) as caught:
                        worker.start_worker_command(["SECRET"], "origin", None, stop=stop)
                    self.assertIn(reason, str(caught.exception))
                    self.assertNotIn("SECRET", str(caught.exception))
                    self.assertTrue(caught.exception.__suppress_context__)
                    stop.assert_called_once_with(process)
                    diagnostic.assert_called_once_with(None, 42, returncode, reason)

    def test_spawn_failure_is_sanitized_and_does_not_claim_a_process(self):
        stop = mock.Mock()
        with mock.patch.object(worker, "require_worker_dependency"), \
                mock.patch.object(worker, "worker_log_size", return_value=0), \
                mock.patch.object(worker.subprocess, "Popen", side_effect=OSError("SECRET")), \
                mock.patch.object(worker, "wait_worker") as wait, \
                mock.patch.object(worker, "startup_diagnostic", return_value="safe metadata"):
            with self.assertRaisesRegex(worker.CheckFailure, "spawn_failed") as caught:
                worker.start_worker_command(["SECRET"], "origin", None, stop=stop)
            self.assertNotIn("SECRET", str(caught.exception))
            wait.assert_not_called()
            stop.assert_called_once_with(None)

    def test_interruption_and_unexpected_readiness_errors_also_reclaim_process(self):
        for error in (KeyboardInterrupt(), SystemExit(3), RuntimeError("unexpected")):
            with self.subTest(error=type(error).__name__):
                process = mock.Mock()
                process.poll.return_value = None
                stop = mock.Mock()
                with mock.patch.object(worker, "require_worker_dependency"), \
                        mock.patch.object(worker, "worker_log_size", return_value=0), \
                        mock.patch.object(worker.subprocess, "Popen", return_value=process), \
                        mock.patch.object(worker, "wait_worker", side_effect=error), \
                        mock.patch.object(worker, "startup_diagnostic") as diagnostic:
                    with self.assertRaises(type(error)) as caught:
                        worker.start_worker_command(["command"], "origin", None, stop=stop)
                    self.assertIs(caught.exception, error)
                    stop.assert_called_once_with(process)
                    diagnostic.assert_not_called()

    def test_secondary_cleanup_and_diagnostic_errors_do_not_mask_startup_failure(self):
        process = mock.Mock()
        process.poll.return_value = None
        process.pid = 123
        stop = mock.Mock(side_effect=RuntimeError("SECRET cleanup"))
        with mock.patch.object(worker, "require_worker_dependency"), \
                mock.patch.object(worker, "worker_log_size", return_value=0), \
                mock.patch.object(worker.subprocess, "Popen", return_value=process), \
                mock.patch.object(worker, "wait_worker", side_effect=worker.CheckFailure("SECRET readiness")), \
                mock.patch.object(worker, "startup_diagnostic", side_effect=RuntimeError("SECRET diagnostic")):
            with self.assertRaises(worker.CheckFailure) as caught:
                worker.start_worker_command(["command"], "origin", None, stop=stop)
        message = str(caught.exception)
        self.assertIn("readiness_failed", message)
        self.assertIn("diagnostic unavailable", message)
        self.assertIn("cleanup failed for owned PID 123", message)
        self.assertNotIn("SECRET", message)
        stop.assert_called_once_with(process)

    def test_interruption_with_cleanup_refusal_reports_owned_pid_without_masking(self):
        for error in (KeyboardInterrupt(), RuntimeError("SECRET readiness")):
            with self.subTest(error=type(error).__name__):
                process = mock.Mock(pid=456)
                process.poll.return_value = None
                stop = mock.Mock(side_effect=OSError("SECRET cleanup"))
                output = io.StringIO()
                with mock.patch.object(worker, "require_worker_dependency"), \
                        mock.patch.object(worker, "worker_log_size", return_value=0), \
                        mock.patch.object(worker.subprocess, "Popen", return_value=process), \
                        mock.patch.object(worker, "wait_worker", side_effect=error), \
                        contextlib.redirect_stderr(output):
                    with self.assertRaises(type(error)) as caught:
                        worker.start_worker_command(["command"], "origin", None, stop=stop)
                self.assertIs(caught.exception, error)
                self.assertIn("cleanup failed for owned PID 456", output.getvalue())
                self.assertNotIn("SECRET", output.getvalue())
                stop.assert_called_once_with(process)

    def test_firewall_preserves_its_command_and_cleanup_callback(self):
        command = ["firewall-command", "--local"]
        with mock.patch.object(firewall, "require_worker_dependency") as admit, \
                mock.patch.object(firewall, "worker_command", return_value=command), \
                mock.patch.object(firewall, "start_worker_command") as start:
            firewall.start_worker("origin", Path("shell"), Path("state"), "log")
            admit.assert_called_once_with()
            start.assert_called_once_with(command, "origin", "log", stop=firewall.stop_worker)

    def test_diagnostic_is_bounded_private_and_excludes_old_attempt_and_raw_secrets(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            original_mkstemp = tempfile.mkstemp
            def private_file(**kwargs):
                return original_mkstemp(dir=root, **kwargs)
            with (root / "worker.log").open("wb") as log:
                log.write(b"previous attempt: EADDRINUSE OLD_SECRET\n")
                offset = worker.worker_log_size(log)
                log.write(b"x" * (worker.STARTUP_DIAGNOSTIC_BYTES + 100))
                log.write(b"\nCannot find module NEW_SECRET PARTY_HMAC_KEY=SECRET\n")
                with mock.patch.object(worker.tempfile, "mkstemp", side_effect=private_file):
                    summary = worker.startup_diagnostic(log, offset, 1, "early_exit")
            evidence = list(root.glob("mdkr-worker-startup-*.json"))
            self.assertEqual(len(evidence), 1)
            text = evidence[0].read_text()
            detail = json.loads(text)
            self.assertEqual(detail["signals"], ["module_not_found"])
            self.assertTrue(detail["truncated"])
            self.assertEqual(detail["sampledBytes"], worker.STARTUP_DIAGNOSTIC_BYTES)
            self.assertFalse(detail["rawOutputRetained"])
            for forbidden in ("SECRET", "PARTY_HMAC_KEY", "EADDRINUSE"):
                self.assertNotIn(forbidden, summary + text)
            if os.name != "nt":
                self.assertEqual(evidence[0].stat().st_mode & 0o777, 0o600)

    def test_unknown_offset_never_scans_prior_output_and_write_failure_is_nonfatal(self):
        with tempfile.TemporaryDirectory() as temp:
            with (Path(temp) / "worker.log").open("wb") as log:
                log.write(b"previous attempt: EADDRINUSE SECRET\n")
                with mock.patch.object(worker.tempfile, "mkstemp", side_effect=OSError("SECRET")):
                    summary = worker.startup_diagnostic(log, None, 1, "early_exit")
            self.assertIn("signals=unclassified", summary)
            self.assertIn("could not be saved", summary)
            self.assertNotIn("SECRET", summary)


if __name__ == "__main__":
    unittest.main()
