#!/usr/bin/env python3
"""Source contracts for transactional Phone Party connection admission; no RTC I/O."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]


def section(source, start, end):
    return source.split(start, 1)[1].split(end, 1)[0]


class PartyOpenTransactionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.source = (ROOT / "platform/party/libdatachannel_party_transport.cpp").read_text()
        cls.connect = section(cls.source, "bool connect(bool create) noexcept {",
                              "\n    void socketOpened(")
        cls.rollback = section(cls.source, "void connectionAttemptFailed(",
                               "\n    bool connect(")
        cls.outer = section(cls.source, "class LibDatachannelPartyTransport final", "\n    bool approve(")

    def test_every_fallible_connection_step_is_inside_the_transaction(self):
        start = self.connect.index("try {")
        failure = self.connect.index("} catch (...)")
        for operation in ("rtc::WebSocket::Configuration configuration;", "configuration.protocols =",
                          "url = signalingUrl(", "configuration.caCertificatePemFile =",
                          "std::make_shared<rtc::WebSocket>", "shared_from_this()",
                          "socket->onOpen(", "socket->onError(", "socket->onClosed(",
                          "socket->onMessage(", "socket->open(url)"):
            with self.subTest(operation=operation):
                self.assertLess(start, self.connect.index(operation))
                self.assertLess(self.connect.index(operation), failure)
        self.assertIn("connectionAttemptFailed(generation, socket, published);", self.connect[failure:])
        self.assertIn("return false;", self.connect[failure:])

    def test_publication_follows_callbacks_and_precedes_unlocked_open(self):
        publication = self.connect.index("socket_ = socket;")
        self.assertLess(self.connect.index("socket->onMessage("), publication)
        self.assertLess(publication, self.connect.index("socket->open(url);"))
        self.assertIn("generation != generation_ || socket_) return false;", self.connect)
        self.assertIn("socketCyclePending_ = false;\n            }\n            socket->open(url);",
                      self.connect)
        for callback in ("socketOpened(generation)", "socketError(generation, reason)",
                         "socketClosed(generation)", "socketMessage(generation, message)"):
            self.assertIn(callback, self.connect)
        self.assertEqual(self.connect.count("[weak, generation]"), 4)

    def test_authentication_and_existing_connection_budgets_are_preserved(self):
        for required in ("configuration.disableTlsVerification = false;",
                         "kMdkrMozillaCaBundle", "kMdkrMozillaCaBundleLength",
                         "configuration.connectionTimeout = std::chrono::seconds(10);",
                         "configuration.pingInterval = std::chrono::seconds(15);",
                         "configuration.maxOutstandingPings = 2;",
                         "configuration.maxMessageSize = kMaxSignalBytes;",
                         '"gb-native-host-v1", "gb-control-v1"'):
            self.assertIn(required, self.connect)

    def test_open_rechecks_ownership_and_retires_lost_attempt_outside_lock(self):
        after_open = self.connect.split("socket->open(url);", 1)[1]
        self.assertIn("stillOwned = !shuttingDown_ && !roomGone_ &&", after_open)
        self.assertIn("generation == generation_ && socket_ == socket;", after_open)
        self.assertIn("socket_ == socket;\n            }\n            if (!stillOwned)", after_open)
        rejection = section(after_open, "if (!stillOwned) {", "\n            }")
        self.assertIn("connectionAttemptFailed(generation, socket, published);", rejection)
        self.assertIn("return false;", rejection)
        self.assertLess(after_open.index("if (!stillOwned)"), after_open.index("return true;"))

    def test_rollback_invalidates_before_unlocked_close_and_does_not_double_retry(self):
        self.assertIn("bool published) noexcept", self.rollback)
        self.assertIn("generation == generation_ && !shuttingDown_", self.rollback)
        self.assertIn("const bool alreadyClosed = published && socket_ != socket;", self.rollback)
        invalidation = self.rollback.index("++generation_;")
        retirement = self.rollback.index("retired = std::move(socket_);")
        close = self.rollback.index("socket->close();")
        self.assertLess(invalidation, retirement)
        self.assertLess(retirement, close)
        self.assertLess(self.rollback.index("} catch (...)"), close)
        self.assertIn("if (!alreadyClosed) {", self.rollback)
        self.assertNotIn("socket_.reset()", self.rollback)
        self.assertIn("retired.reset();", self.rollback)

    def test_failed_setup_restores_network_retry_without_manufacturing_service_refusal(self):
        self.assertIn("resumeRejected_ = false;", self.rollback)
        self.assertIn("!roomGone_ && !credential_.empty()", self.rollback)
        self.assertIn("reconnectAttempt_ = std::min(reconnectAttempt_ + 1u, 6u);", self.rollback)
        self.assertIn("reconnectAttempt_, false, resumeRejections_,", self.rollback)
        self.assertIn("std::chrono::milliseconds(decision.delayMs)", self.rollback)
        self.assertNotIn("resumeRejections_++", self.rollback)
        self.assertLess(self.rollback.index("reconnectAt_ ="), self.rollback.index("enqueue("))
        self.assertIn("} catch (...) { /* Recovery state precedes optional event allocation. */ }",
                      self.rollback)

    def test_outer_state_is_unpublished_until_initialize_succeeds_and_failure_is_retryable(self):
        self.assertIn("bool open(const std::string &serviceOrigin) noexcept override", self.outer)
        allocate = self.outer.index("candidate = std::make_shared<TransportState>();")
        initialize = self.outer.index("if (candidate->initialize(serviceOrigin))")
        publish = self.outer.index("state_ = std::move(candidate);")
        self.assertLess(self.outer.index("try {"), allocate)
        self.assertLess(allocate, initialize)
        self.assertLess(initialize, publish)
        self.assertNotIn("state_ = std::make_shared", self.outer)
        self.assertIn("try { candidate->shutdown(); } catch (...)", self.outer)
        self.assertIn("return false;", self.outer[self.outer.index("} catch (...)"):])

    def test_signaling_identity_reaches_parse_and_all_dispatch_handlers(self):
        dispatch = section(self.source, "void socketMessage(", "\n    void handleBootstrap(")
        for handler in ("handleBootstrap", "handleRoomState", "handleHello", "handleAnswer", "handleIce"):
            self.assertIn(f"{handler}(value, generation)", dispatch)
        self.assertIn("enqueue(std::move(event), generation);", dispatch)
        parser = section(self.source, "bool parseRoom(", "\n    void handleRoomState(")
        self.assertIn("callbackCurrentLocked(signalGeneration)", parser)
        for cache_field in ("inviteUrl_", "fallbackCode_", "inviteGeneration_", "inviteExpiresAtMs_"):
            self.assertNotRegex(parser, rf"\b{cache_field}\s*=(?!=)")
        room = section(self.source, "void handleRoomState(", "\n    void handleHello(")
        self.assertLess(room.index("callbackCurrentLocked(signalGeneration)"), room.index("inviteUrl_.swap"))
        self.assertLess(room.index("controllers_.swap(roster)"), room.index("queue_.push(std::move(event))"))
        self.assertIn("createPeer(controller, false, 0u, signalGeneration)", room)

    def test_retirement_and_event_commits_share_the_state_mutex(self):
        close = section(self.source, "void socketClosed(", "\n    void tick(")
        self.assertLess(close.index("lock(mutex_)"), close.index("++generation_"))
        self.assertLess(close.index("++generation_"), close.index("queue_.push(std::move(event))"))
        self.assertNotIn("enqueue(std::move(event))", close)
        enqueue = section(self.source, "void enqueue(", "\n    void connectionAttemptFailed(")
        self.assertLess(enqueue.index("lock(mutex_)"), enqueue.index("callbackCurrentLocked(generation, peer)"))
        self.assertLess(enqueue.index("callbackCurrentLocked(generation, peer)"), enqueue.index("queue_.push"))
        self.assertIn("enqueue(std::move(event), reportGeneration)", self.rollback)
        disconnect = section(self.source, "void peerDisconnected(", "\n    void stateMessage(")
        self.assertLess(disconnect.index("callbackCurrentLocked(signalGeneration, peer)"),
                        disconnect.index("peer->failed ="))
        self.assertLess(disconnect.index("peer->failed ="), disconnect.index("queue_.push"))

    def test_direct_callbacks_check_owner_at_state_and_event_commits(self):
        predicate = section(self.source, "bool callbackCurrentLocked(", "\n    bool controllerCurrentLocked(")
        self.assertIn("found->second == peer", predicate)
        self.assertIn("mdkr_party_peer_initialization_current(", predicate)
        self.assertIn("mdkr_party_callback_current(shuttingDown_, generation_, generation, peerMatches)", predicate)
        control = section(self.source, "void controlMessage(", "\n    void handleAnswer(")
        self.assertGreaterEqual(control.count("callbackCurrentLocked(0u, peer)"), 4)
        self.assertLess(control.index("callbackCurrentLocked(0u, peer)"), control.index("peer->authenticated = true"))
        self.assertLess(control.index("peer->authenticated = true"), control.index("queue_.push(std::move(ready))"))
        self.assertIn("enqueue(std::move(renamed), 0u, peer)", control)
        self.assertIn("enqueue(std::move(sample), 0u, peer)", control)
        packet = section(self.source, "void stateMessage(", "\n    void controlOpened(")
        self.assertIn("enqueue(std::move(event), 0u, peer)", packet)
        phrase = section(self.source, "void emitPhrase(", "\n    void handleIce(")
        self.assertEqual(phrase.count("callbackCurrentLocked(signalGeneration, peer)"), 2)
        self.assertIn("enqueue(std::move(event), signalGeneration, peer)", phrase)

    def test_candidate_publication_rechecks_identity_and_stale_pending_is_replaceable(self):
        create = section(self.source, "void createPeer(", "\n    void peerDisconnected(")
        self.assertIn("peer->admissionGeneration = setupGeneration", create)
        self.assertIn("mdkr_party_peer_initialization_current(found->second->initializing", create)
        self.assertLess(create.index("peers_.emplace"), create.index("std::make_shared<rtc::PeerConnection>"))
        self.assertEqual(create.count("admitted = callbackCurrentLocked(signalGeneration, peer)"), 3)
        self.assertLess(create.index("peer->control->onClosed("), create.index("peer->initializing = false"))
        self.assertIn("peerSetupFailed(controller, setupGeneration, setupRevision, peer, connection)",
                      create[create.index("        } catch (...) {"):])
        room = section(self.source, "void handleRoomState(", "\n    void handleHello(")
        self.assertIn("mdkr_party_peer_initialization_current(found->second->initializing", room)
        self.assertLess(room.index("mdkr_party_peer_initialization_current"), room.index("peers_.erase(found)"))
        retire = section(self.source, "void retirePeerCandidate(", "\n    void createPeer(")
        self.assertIn("found->second == peer", retire)
        self.assertLess(retire.index("} catch (...)"), retire.index("connection->close()"))

    def test_retry_snapshot_cannot_replace_new_owner_or_newly_authenticated_peer(self):
        tick = section(self.source, "void tick(", "\n    void socketMessage(")
        self.assertIn("std::shared_ptr<Peer> expectedPeer", tick)
        self.assertIn("recreation.expectedPeer, recreation.offerSentMs", tick)
        create = section(self.source, "void createPeer(", "\n    void peerDisconnected(")
        admission = section(create, "mdkr_party_retry_owner_current(", "return;")
        for required in ("found->second == expectedPeer", "!expectedPeer->authenticated",
                         "!expectedPeer->failed", "!expectedPeer->protocolMismatch", "!expectedPeer->gaveUp",
                         "expectedPeer->offerAttempts == carriedOfferAttempts",
                         "expectedPeer->offerSentMs == expectedOfferSentMs"):
            self.assertIn(required, admission)
        self.assertLess(create.index("mdkr_party_retry_owner_current("), create.index("peers_.erase(found)"))

    def test_rtc_signal_operations_capture_current_owner_under_same_lock(self):
        for name, end in (("handleAnswer", "/*\n     * SAS v2"), ("handleIce", "    void sendPeerSignal(")):
            handler = section(self.source, f"void {name}(", end)
            self.assertIn("callbackCurrentLocked(signalGeneration, found->second)", handler)
        command = section(self.source, "bool command(", "\n    /* M4:")
        self.assertLess(command.index("callbackCurrentLocked(generation, peer)"), command.index("socket = socket_"))
        forwarding = section(self.source, "void sendPeerSignal(", "\n    /* C3 retry bookkeeping")
        self.assertIn("command(value, signalGeneration, peer)", forwarding)

    def test_ping_expiry_and_offer_give_up_do_not_outlive_same_peer_progress(self):
        tick = section(self.source, "void tick(", "\n    void socketMessage(")
        self.assertIn("expired.push_back({peer, peer->pingOutstandingAt, peer->pingNonce})", tick)
        self.assertIn("peerDisconnected(expiration.peer, true, 0u, expiration.sentAt, expiration.nonce)", tick)
        give_up = section(tick, "if (decision.giveUp) {", "} else if (decision.recreatePeer)")
        self.assertIn("peer->gaveUp = true", give_up)
        self.assertIn("queue_.push(std::move(event))", give_up)
        self.assertNotIn("timedOut", tick)
        disconnect = section(self.source, "void peerDisconnected(", "\n    void stateMessage(")
        self.assertIn("peer->pingOutstandingAt == expectedPingAt", disconnect)
        self.assertIn("peer->pingNonce == expectedPingNonce", disconnect)
        self.assertLess(disconnect.index("mdkr_party_ping_timeout_current("), disconnect.index("peer->failed ="))

    def test_setup_reservation_precedes_allocations_and_failure_remains_owned(self):
        create = section(self.source, "void createPeer(", "\n    void peerDisconnected(")
        reserve = create.index("setupRevision = mdkr_party_setup_begin(")
        for allocation in ("iceServers = iceServers_", "std::make_shared<Peer>()",
                           "peer->id = controller.id", "std::make_shared<rtc::PeerConnection>"):
            self.assertLess(reserve, create.index(allocation))
        self.assertIn("setupCurrentLocked(controller, setupGeneration, setupRevision)", create)
        self.assertIn("mdkr_party_setup_succeeded(", create)
        self.assertIn("if (setupRevision == 0u) {", create)
        failure = section(self.source, "void peerSetupFailed(", "\n    void createPeer(")
        self.assertLess(failure.index("setupCurrentLocked(controller, setupGeneration, revision)"),
                        failure.index("mdkr_party_setup_failed("))
        self.assertIn("peer->failed = true", failure)
        self.assertLess(failure.index("} catch (...)"), failure.index("retirePeerCandidate(peer, connection)"))

    def test_setup_retry_is_due_scoped_and_cannot_reset_on_duplicate_room_or_hello(self):
        tick = section(self.source, "void tick(", "\n    void socketMessage(")
        self.assertIn("mdkr_party_setup_due(admission.setup, nowMs)", tick)
        self.assertIn("admission.setupGeneration != generation_", tick)
        self.assertIn("createPeer(retry.controller, false, 0u, retry.generation, {}, 0u, retry.revision)", tick)
        create = section(self.source, "void createPeer(", "\n    void peerDisconnected(")
        self.assertIn("admission.setup.revision != expectedSetupRevision", create)
        self.assertIn("!mdkr_party_setup_due(admission.setup, steadyNowMs())", create)
        self.assertIn("++setupRevision_", create)
        room = section(self.source, "void handleRoomState(", "\n    void handleHello(")
        self.assertIn("sameControllerLifecycle(old->second.controller, admission.controller)", room)
        self.assertIn("admission.setup = old->second.setup", room)
        self.assertIn("mdkr_party_setup_reauthorize(admission.setup", room)
        self.assertIn("if (freshLifecycle ||", room)
        hello = section(self.source, "void handleHello(", "\n    /* forceRecreate")
        self.assertIn("mdkr_party_setup_reauthorize(found->second.setup", hello)
        self.assertNotIn("setup =", hello)

    def test_setup_failure_preserves_offer_budget_and_exhaustion_is_reported_once(self):
        create = section(self.source, "void createPeer(", "\n    void peerDisconnected(")
        self.assertIn("std::max(carriedOfferAttempts, admission.setup.offerAttempts)", create)
        self.assertIn("std::max(carriedOfferAttempts, priorPeer->offerAttempts)", create)
        self.assertIn("if (priorPeer && priorPeer->authenticated) admission.setup.offerAttempts = 0u", create)
        self.assertNotIn("admission.setup.failures = 0", create)
        report = section(self.source, "void reportSetupExhaustedLocked(", "\n    void enqueue(")
        self.assertIn("!admission.setup.exhausted || admission.setup.exhaustionReported", report)
        self.assertIn("event.message = kMdkrPartySetupExhaustedCopy", report)
        self.assertLess(report.index("queue_.push"), report.index("admission.setup.exhaustionReported = true"))
        disconnect = section(self.source, "void peerDisconnected(", "\n    void stateMessage(")
        self.assertIn("failed && !peer->initializing", disconnect)


if __name__ == "__main__":
    unittest.main()
