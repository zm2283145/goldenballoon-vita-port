#!/usr/bin/env python3
"""Production bindings for signaling admission/terminal-recovery fixtures."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]


def section(source, start, end):
    return source.split(start, 1)[1].split(end, 1)[0]


class MatchSignalAdmissionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.source = (ROOT / "platform/online/match_signal_client.cpp").read_text()
        cls.fixture = (ROOT / "tests/test_match_signal_client.cpp").read_text()

    def test_send_commits_tracking_and_queue_before_sequence(self):
        send = section(self.source, "MdkrMatchSignalSendResult MdkrMatchSignalClient::send(",
                       "\nvoid MdkrMatchSignalClient::close()")
        self.assertLess(send.index("wire.dump()"), send.index("state.sentTargets.emplace"))
        self.assertLess(send.index("state.sentTargets.emplace"), send.index("state.outbound.push_back"))
        self.assertLess(send.index("state.outbound.push_back"), send.index("state.nextSequence++"))
        self.assertLess(send.index("state.nextSequence++"), send.index("result.ok = true"))
        self.assertIn("state.sentTargets.erase(inserted.first)", send)
        self.assertIn("catch (const std::bad_alloc &)", send)
        self.assertNotIn("state.sentTargets[sequence]", send)
        for stage in (1, 2, 3):
            self.assertIn(f"refuseStage({stage}u)", send)

    def test_worker_failure_runs_after_transport_unwind(self):
        worker = section(self.source, "void MdkrMatchSignalClient::State::run()",
                         "/* ---- Public API")
        self.assertTrue(worker.startswith(" try {"))
        self.assertIn("Transport transport;", worker)
        handler = worker.rsplit("} catch (...)", 1)[1]
        self.assertIn("std::lock_guard<std::mutex> lock(mutex)", handler)
        self.assertIn("stopping = true", handler)
        self.assertIn("socketOpen = false", handler)
        self.assertIn("latchFailureLocked(kMdkrMatchSignalTransportLost)", handler)

    def test_terminal_fallback_does_not_allocate(self):
        fallback = section(self.source, "void retainFailureLocked(", "/* ---- Event queue")
        self.assertIn("noexcept", fallback)
        self.assertIn("pendingFailureCode = code", fallback)
        for forbidden in ("push_back", "emplace", "std::string", "new "):
            self.assertNotIn(forbidden, fallback)
        latch = section(self.source, "bool latchFailureLocked(", "bool rememberPeerGenerationLocked(")
        self.assertIn("noexcept", latch)
        self.assertIn("} catch (...)", latch)
        self.assertIn("retainFailureLocked(code)", latch)

    def test_drain_prepares_all_storage_before_consuming_events(self):
        drain = section(self.source, "void MdkrMatchSignalClient::drainEvents(", "/* ---- Fuzz seam")
        self.assertIn("std::is_nothrow_move_constructible_v<MdkrMatchSignalEvent>", drain)
        self.assertLess(drain.index("failure.failureCode = state.pendingFailureCode"),
                        drain.index("state.events.pop_front()"))
        self.assertLess(drain.index("out.reserve("), drain.index("state.events.pop_front()"))
        self.assertLess(drain.index("} catch (...)"), drain.index("state.events.pop_front()"))
        self.assertLess(drain.index("out.push_back(std::move(failure))"),
                        drain.index("state.pendingFailureCode = nullptr"))

    def test_fixture_invokes_each_actual_refusal_journey(self):
        main = self.fixture.split("int main()", 1)[1]
        for name in ("refusedCreationReturnsAnOrdinaryFailure",
                     "refusedWorkerStartLeavesRetryableClient",
                     "refusedCloseReportStillRetiresWorker",
                     "refusedWorkerPublicationRetainsTerminalRecovery",
                     "refusedSendAdmissionKeepsCorrelationAndSequenceAtomic"):
            self.assertIn(f"{name}();", main)

    def test_sensitive_local_buffers_are_guarded_before_secret_copy(self):
        worker = section(self.source, "void MdkrMatchSignalClient::State::run()", "/* ---- Public API")
        for buffer, guard in (("credentialOffer", "wipeCredentialOffer"),
                              ("request", "wipeRequest"),
                              ("storedOffer", "wipeStoredOffer")):
            self.assertIn(f"MdkrScopedStringWipe<mbedtls_platform_zeroize> {guard}({buffer})", worker)
            secret_append = f"{buffer}.append(" + ("credentialOffer)" if buffer == "request" else "credential)")
            self.assertLess(worker.index(f"{guard}({buffer})"), worker.index(secret_append))
            self.assertLess(worker.index(f"{buffer}.reserve("), worker.index(secret_append))
            self.assertIn(f"{guard}.wipe()", worker)


if __name__ == "__main__":
    unittest.main()
