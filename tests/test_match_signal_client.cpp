/*
 * MdkrMatchSignalClient: the native launcher-side match-signaling client,
 * pinned rule-for-rule against the dormant browser reference client
 * (dist/web/online/match-signal-client.js) and the wire contract in
 * docs/ref/match-signaling-v1.md. Every case runs against a scriptable
 * loopback fake of the signal endpoint (match_signal_test_server.h), so
 * every property asserted here is a wire-level fact: what the client offers
 * on the upgrade, what it refuses, which close code it sends, and which
 * failure-code string reaches the launcher.
 */
#include "online/match_signal_client.h"

#include "match_signal_test_server.h"
#include "party/party_webrtc_signaling.h"

/* Assert-driven test: NDEBUG would compile every check away. */
#undef NDEBUG

#include <nlohmann/json.hpp>

#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

using Json = nlohmann::json;

const char kRoomId[] = "Rm0123456789abcdefABCD";              /* 22 chars */
const char kCredential[] =
    "cred_cred_cred_cred_cred_cred_cred_cred_012";            /* 43 chars */
const char kSelfEndpoint[] = "101";

std::string validPublicKey(uint8_t seed = 1u) {
    uint8_t raw[65];
    raw[0] = 0x04u;
    for (unsigned index = 1u; index < 65u; index++) {
        raw[index] = static_cast<uint8_t>(seed + index);
    }
    return mdkr_party::base64Url(raw, sizeof(raw));
}

MdkrMatchSignalClientOptions baseOptions(uint16_t port,
                                         unsigned timeoutMs = 10000u) {
    MdkrMatchSignalClientOptions options;
    options.serviceOrigin = "ws://127.0.0.1:" + std::to_string(port);
    options.roomId = kRoomId;
    options.endpointId = kSelfEndpoint;
    options.credential = kCredential;
    options.timeoutMs = timeoutMs;
    return options;
}

Json welcomeMessage(uint32_t generation,
                    const std::vector<std::pair<std::string, uint32_t>> &peers) {
    Json peerList = Json::array();
    for (const auto &peer : peers) {
        peerList.push_back(Json{{"endpointId", peer.first},
                                {"connectionGeneration", peer.second}});
    }
    return Json{{"protocolVersion", 1},
                {"type", "signal_welcome"},
                {"endpointId", kSelfEndpoint},
                {"connectionGeneration", generation},
                {"peers", peerList}};
}

Json presenceMessage(const std::string &endpointId, uint32_t generation,
                     bool present) {
    return Json{{"protocolVersion", 1},
                {"type", "peer_presence"},
                {"endpointId", endpointId},
                {"connectionGeneration", generation},
                {"present", present}};
}

Json directedCommon(const std::string &type, uint32_t sequence,
                    uint32_t toGeneration, const std::string &fromEndpoint,
                    uint32_t fromGeneration) {
    return Json{{"protocolVersion", 1},
                {"type", type},
                {"sequence", sequence},
                {"toEndpointId", kSelfEndpoint},
                {"toConnectionGeneration", toGeneration},
                {"fromEndpointId", fromEndpoint},
                {"fromConnectionGeneration", fromGeneration}};
}

/* Drain the client's queue until `count` events have been collected. */
bool waitForEvents(MdkrMatchSignalClient &client,
                   std::vector<MdkrMatchSignalEvent> &collected, size_t count,
                   unsigned budgetMs = 5000u) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(budgetMs);
    std::vector<MdkrMatchSignalEvent> batch;
    for (;;) {
        client.drainEvents(batch);
        collected.insert(collected.end(), batch.begin(), batch.end());
        if (collected.size() >= count) return true;
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

/* One server + one client, wired together. */
struct Rig {
    MdkrMatchSignalTestServer server;
    std::unique_ptr<MdkrMatchSignalClient> client;
    std::vector<MdkrMatchSignalEvent> events;

    explicit Rig(MdkrMatchSignalTestServer::ProtocolMode mode =
                     MdkrMatchSignalTestServer::ProtocolMode::SelectV1,
                 unsigned timeoutMs = 10000u, bool respond = true,
                 const std::string &extra101Header = std::string()) {
        server.protocolMode = mode;
        server.respondToUpgrade = respond;
        server.extra101Header = extra101Header;
        assert(server.start());
        std::string error;
        client = MdkrMatchSignalClient::create(
            baseOptions(server.port(), timeoutMs), &error);
        assert(client != nullptr);
        assert(client->connect());
    }

    ~Rig() {
        if (client) client->close();
        server.stop();
    }

    /* Complete the welcome and swallow its event. */
    void welcome(uint32_t generation,
                 const std::vector<std::pair<std::string, uint32_t>> &peers) {
        assert(server.waitForOpen());
        assert(server.sendText(welcomeMessage(generation, peers).dump()));
        assert(waitForEvents(*client, events, 1u));
        assert(events.back().type == MdkrMatchSignalEventType::Welcome);
        const MdkrMatchSignalSnapshot snapshot = client->snapshot();
        assert(snapshot.phase == MdkrMatchSignalPhase::Open);
        assert(snapshot.connected);
        assert(snapshot.connectionGeneration == generation);
    }

    /* The terminal-failure contract: the code arrives as a Failure event,
     * the phase latches Failed, and every later call refuses with the JS
     * codes. */
    void expectTerminalFailure(const char *code, unsigned budgetMs = 5000u) {
        const size_t before = events.size();
        bool sawFailure = false;
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(budgetMs);
        while (!sawFailure && std::chrono::steady_clock::now() < deadline) {
            (void)waitForEvents(*client, events, events.size() + 1u, 200u);
            for (size_t index = before; index < events.size(); index++) {
                if (events[index].type == MdkrMatchSignalEventType::Failure) {
                    assert(events[index].failureCode == code);
                    sawFailure = true;
                }
            }
        }
        assert(sawFailure);
        assert(client->snapshot().phase == MdkrMatchSignalPhase::Failed);
        assert(!client->snapshot().connected);
        MdkrMatchSignalOutbound hello;
        hello.type = "peer_hello";
        hello.toEndpointId = "202";
        hello.toConnectionGeneration = 1u;
        hello.publicKey = validPublicKey();
        const MdkrMatchSignalSendResult refused = client->send(hello);
        assert(!refused.ok);
        assert(refused.error == kMdkrMatchSignalNotConnected);
        std::string connectError;
        assert(!client->connect(&connectError));
        assert(connectError == kMdkrMatchSignalClientClosed);
    }
};

/* A bad welcome (or a bad first message) must be terminal with
 * invalid_signal_message and the 4003 close the JS client sends. */
void expectFirstMessageRejected(const Json &message, const char *label) {
    Rig rig;
    assert(rig.server.waitForOpen());
    assert(rig.server.sendText(message.dump()));
    rig.expectTerminalFailure(kMdkrMatchSignalInvalidMessage);
    assert(rig.server.waitForClientClose());
    assert(rig.server.clientCloseCode() == 4003u);
    assert(rig.server.clientCloseReason() == "invalid_server_message");
    std::fprintf(stderr, "  rejected: %s\n", label);
}

/* ---- Cases --------------------------------------------------------------- */

void createRefusesInvalidIdentityAndForeignOrigins() {
    const MdkrMatchSignalClientOptions good = baseOptions(1u);
    std::string error;

    { /* Valid identity constructs (no connect). */
        auto client = MdkrMatchSignalClient::create(good, &error);
        assert(client != nullptr);
        assert(client->snapshot().phase == MdkrMatchSignalPhase::Idle);
    }
    { /* endpoint at the u64 ceiling is still valid. */
        MdkrMatchSignalClientOptions options = good;
        options.endpointId = "18446744073709551615";
        assert(MdkrMatchSignalClient::create(options, &error) != nullptr);
    }

    const auto refusedIdentity = [&](MdkrMatchSignalClientOptions options) {
        error.clear();
        assert(MdkrMatchSignalClient::create(options, &error) == nullptr);
        assert(error == kMdkrMatchSignalInvalidIdentity);
    };
    { MdkrMatchSignalClientOptions o = good; o.roomId = "tooShortRoom";
      refusedIdentity(o); }
    { MdkrMatchSignalClientOptions o = good;
      o.roomId = std::string(kRoomId) + "x"; refusedIdentity(o); }
    { MdkrMatchSignalClientOptions o = good; o.roomId[3] = '!';
      refusedIdentity(o); }
    { MdkrMatchSignalClientOptions o = good; o.endpointId = "0";
      refusedIdentity(o); }
    { MdkrMatchSignalClientOptions o = good; o.endpointId = "018";
      refusedIdentity(o); }
    { MdkrMatchSignalClientOptions o = good; o.endpointId = "abc";
      refusedIdentity(o); }
    { MdkrMatchSignalClientOptions o = good;
      o.endpointId = "18446744073709551616"; /* 2^64 */ refusedIdentity(o); }
    { MdkrMatchSignalClientOptions o = good;
      o.endpointId = "111111111111111111111"; /* 21 digits */
      refusedIdentity(o); }
    { MdkrMatchSignalClientOptions o = good;
      o.credential = std::string(42u, 'c'); refusedIdentity(o); }
    { MdkrMatchSignalClientOptions o = good;
      o.credential = std::string(42u, 'c') + "!"; refusedIdentity(o); }

    const auto refusedOrigin = [&](const std::string &origin) {
        MdkrMatchSignalClientOptions options = good;
        options.serviceOrigin = origin;
        error.clear();
        assert(MdkrMatchSignalClient::create(options, &error) == nullptr);
        assert(error == kMdkrMatchSignalCrossOriginRefused);
    };
    refusedOrigin("");
    refusedOrigin("ftp://127.0.0.1");
    refusedOrigin("ws://example.com");           /* plaintext off-machine */
    refusedOrigin("http://example.com:8080");
    refusedOrigin("ws://127.0.0.1.evil.example:1"); /* host-boundary smuggle */
    refusedOrigin("ws://127.0.0.1:@evil.example");  /* userinfo smuggle */
}

void happyPathRoundTripPinsTheFullContract() {
    Rig rig;

    /* connect() is idempotent while connecting/open. */
    assert(rig.client->connect());

    /* The upgrade itself: exact path, credential ONLY in the subprotocol,
     * no Origin header (native is originless). */
    assert(rig.server.waitForUpgrade());
    assert(rig.server.requestPath() ==
           std::string("/api/match/") + kRoomId + "/signal");
    assert(rig.server.requestPath().find(kCredential) == std::string::npos);
    assert(rig.server.requestPath().find('?') == std::string::npos);
    const std::vector<std::string> offered = rig.server.offeredProtocols();
    assert(offered.size() == 2u);
    assert(offered[0] == "gb-match-signal-v1");
    assert(offered[1] == std::string("gb-match.") + kCredential);
    assert(!rig.server.sawHeader("origin"));
    assert(rig.server.sawHeader("host"));
    /* The credential appears exactly once in the whole head: the
     * subprotocol offer, nowhere else. */
    const std::string head = rig.server.requestHeadRaw();
    const size_t first = head.find(kCredential);
    assert(first != std::string::npos);
    assert(head.find(kCredential, first + 1u) == std::string::npos);

    /* Welcome with two sorted unique peers. */
    rig.welcome(7u, {{"202", 5u}, {"303", 2u}});
    const MdkrMatchSignalEvent &welcome = rig.events.back();
    assert(welcome.endpointId == kSelfEndpoint);
    assert(welcome.connectionGeneration == 7u);
    assert(welcome.peers.size() == 2u);
    assert(welcome.peers[0].endpointId == "202");
    assert(welcome.peers[0].connectionGeneration == 5u);
    assert(welcome.peers[1].endpointId == "303");
    assert(welcome.peers[1].connectionGeneration == 2u);

    /* Presence for a fresh peer. */
    assert(rig.server.sendText(presenceMessage("404", 1u, true).dump()));
    assert(waitForEvents(*rig.client, rig.events, rig.events.size() + 1u));
    {
        const MdkrMatchSignalEvent &presence = rig.events.back();
        assert(presence.type == MdkrMatchSignalEventType::PeerPresence);
        assert(presence.endpointId == "404");
        assert(presence.connectionGeneration == 1u);
        assert(presence.present);
    }

    /* Directed inbound: hello / offer / ice / answer / peer_end, strictly
     * increasing per-sender sequences (gaps allowed). */
    const std::string peerKey = validPublicKey(9u);
    Json hello = directedCommon("peer_hello", 1u, 7u, "202", 5u);
    hello["publicKey"] = peerKey;
    assert(rig.server.sendText(hello.dump()));
    Json offer = directedCommon("webrtc_offer", 2u, 7u, "202", 5u);
    offer["sdp"] = "v=0\r\no=offer";
    assert(rig.server.sendText(offer.dump()));
    Json ice = directedCommon("webrtc_ice", 3u, 7u, "202", 5u);
    ice["candidate"] = "candidate:1 1 udp 1 10.0.0.1 9 typ host";
    ice["sdpMid"] = "0";
    ice["sdpMLineIndex"] = 0;
    ice["usernameFragment"] = nullptr;
    assert(rig.server.sendText(ice.dump()));
    Json answer = directedCommon("webrtc_answer", 5u, 7u, "202", 5u);
    answer["sdp"] = "v=0\r\no=answer";
    assert(rig.server.sendText(answer.dump()));
    Json end = directedCommon("peer_end", 6u, 7u, "202", 5u);
    end["reason"] = "close";
    assert(rig.server.sendText(end.dump()));
    assert(waitForEvents(*rig.client, rig.events, rig.events.size() + 5u));
    {
        const size_t base = rig.events.size() - 5u;
        assert(rig.events[base].type == MdkrMatchSignalEventType::PeerHello);
        assert(rig.events[base].fromEndpointId == "202");
        assert(rig.events[base].fromConnectionGeneration == 5u);
        assert(rig.events[base].sequence == 1u);
        assert(rig.events[base].publicKey == peerKey);
        assert(rig.events[base + 1u].type ==
               MdkrMatchSignalEventType::WebrtcOffer);
        assert(rig.events[base + 1u].sdp == "v=0\r\no=offer");
        assert(rig.events[base + 2u].type ==
               MdkrMatchSignalEventType::WebrtcIce);
        assert(rig.events[base + 2u].candidate ==
               "candidate:1 1 udp 1 10.0.0.1 9 typ host");
        assert(rig.events[base + 2u].hasSdpMid);
        assert(rig.events[base + 2u].sdpMid == "0");
        assert(rig.events[base + 2u].hasSdpMLineIndex);
        assert(rig.events[base + 2u].sdpMLineIndex == 0u);
        assert(!rig.events[base + 2u].hasUsernameFragment);
        assert(rig.events[base + 3u].type ==
               MdkrMatchSignalEventType::WebrtcAnswer);
        assert(rig.events[base + 3u].sdp == "v=0\r\no=answer");
        assert(rig.events[base + 4u].type ==
               MdkrMatchSignalEventType::PeerEnd);
        assert(rig.events[base + 4u].reason == "close");
    }

    /* Outbound: every type, monotonic sequences, exact wire shape. The
     * 60 KiB sdp pins the upper bound and the 16-bit frame length. */
    MdkrMatchSignalOutbound outHello;
    outHello.type = "peer_hello";
    outHello.toEndpointId = "202";
    outHello.toConnectionGeneration = 5u;
    outHello.publicKey = validPublicKey(3u);
    MdkrMatchSignalSendResult sent = rig.client->send(outHello);
    assert(sent.ok);
    assert(sent.sequence == 1u);

    MdkrMatchSignalOutbound outOffer;
    outOffer.type = "webrtc_offer";
    outOffer.toEndpointId = "303";
    outOffer.toConnectionGeneration = 2u;
    outOffer.sdp = std::string(60u * 1024u, 's');
    sent = rig.client->send(outOffer);
    assert(sent.ok);
    assert(sent.sequence == 2u);

    MdkrMatchSignalOutbound outIce;
    outIce.type = "webrtc_ice";
    outIce.toEndpointId = "202";
    outIce.toConnectionGeneration = 5u;
    outIce.candidate = "candidate:9 1 udp 2 10.0.0.2 9 typ host";
    outIce.hasSdpMLineIndex = true;
    outIce.sdpMLineIndex = 1u;
    outIce.hasUsernameFragment = true;
    outIce.usernameFragment = "uf";
    sent = rig.client->send(outIce);
    assert(sent.ok);
    assert(sent.sequence == 3u);

    MdkrMatchSignalOutbound outEnd;
    outEnd.type = "peer_end";
    outEnd.toEndpointId = "202";
    outEnd.toConnectionGeneration = 5u;
    outEnd.reason = "restart";
    sent = rig.client->send(outEnd);
    assert(sent.ok);
    assert(sent.sequence == 4u);
    assert(rig.client->snapshot().nextSequence == 5u);

    assert(rig.server.waitForTextMessages(4u));
    assert(rig.server.allClientFramesMasked());
    {
        const std::vector<std::string> wire = rig.server.textMessages();
        const Json first = Json::parse(wire[0]);
        assert(first.size() == 6u);
        assert(first["protocolVersion"] == 1);
        assert(first["type"] == "peer_hello");
        assert(first["sequence"] == 1u);
        assert(first["toEndpointId"] == "202");
        assert(first["toConnectionGeneration"] == 5u);
        assert(first["publicKey"] == outHello.publicKey);
        const Json second = Json::parse(wire[1]);
        assert(second.size() == 6u);
        assert(second["type"] == "webrtc_offer");
        assert(second["sequence"] == 2u);
        assert(second["toEndpointId"] == "303");
        assert(second["toConnectionGeneration"] == 2u);
        assert(second["sdp"] == outOffer.sdp);
        const Json third = Json::parse(wire[2]);
        assert(third.size() == 9u);
        assert(third["type"] == "webrtc_ice");
        assert(third["candidate"] == outIce.candidate);
        assert(third["sdpMid"].is_null());
        assert(third["sdpMLineIndex"] == 1u);
        assert(third["usernameFragment"] == "uf");
        const Json fourth = Json::parse(wire[3]);
        assert(fourth.size() == 6u);
        assert(fourth["type"] == "peer_end");
        assert(fourth["reason"] == "restart");
    }

    /* signal_error correlates against the recorded sent target. */
    assert(rig.server.sendText(Json{{"protocolVersion", 1},
                                    {"type", "signal_error"},
                                    {"sequence", 1u},
                                    {"toEndpointId", "202"},
                                    {"toConnectionGeneration", 5u},
                                    {"error", "peer_unavailable"}}.dump()));
    assert(waitForEvents(*rig.client, rig.events, rig.events.size() + 1u));
    {
        const MdkrMatchSignalEvent &errorEvent = rig.events.back();
        assert(errorEvent.type == MdkrMatchSignalEventType::SignalError);
        assert(errorEvent.sequence == 1u);
        assert(errorEvent.toEndpointId == "202");
        assert(errorEvent.toConnectionGeneration == 5u);
        assert(errorEvent.error == "peer_unavailable");
    }

    /* Departure: absence must name the exact current generation; a departed
     * peer refuses sends; a rejoin above the high-water mark is accepted and
     * resets the forwarded-sequence floor. */
    assert(rig.server.sendText(presenceMessage("202", 5u, false).dump()));
    assert(waitForEvents(*rig.client, rig.events, rig.events.size() + 1u));
    assert(!rig.events.back().present);
    MdkrMatchSignalOutbound toDeparted = outHello;
    const MdkrMatchSignalSendResult departed = rig.client->send(toDeparted);
    assert(!departed.ok);
    assert(departed.error == kMdkrMatchSignalPeerUnavailable);
    assert(rig.server.sendText(presenceMessage("202", 6u, true).dump()));
    assert(waitForEvents(*rig.client, rig.events, rig.events.size() + 1u));
    Json rejoinHello = directedCommon("peer_hello", 1u, 7u, "202", 6u);
    rejoinHello["publicKey"] = peerKey;
    assert(rig.server.sendText(rejoinHello.dump()));
    assert(waitForEvents(*rig.client, rig.events, rig.events.size() + 1u));
    assert(rig.events.back().type == MdkrMatchSignalEventType::PeerHello);
    assert(rig.events.back().fromConnectionGeneration == 6u);

    /* Clean close: 1000 launcher_route_changed, then everything refuses. */
    rig.client->close();
    assert(rig.server.waitForClientClose());
    assert(rig.server.clientCloseCode() == 1000u);
    assert(rig.server.clientCloseReason() == "launcher_route_changed");
    assert(rig.client->snapshot().phase == MdkrMatchSignalPhase::Closed);
    const MdkrMatchSignalSendResult afterClose = rig.client->send(outHello);
    assert(!afterClose.ok);
    assert(afterClose.error == kMdkrMatchSignalNotConnected);
    std::string connectError;
    assert(!rig.client->connect(&connectError));
    assert(connectError == kMdkrMatchSignalClientClosed);
}

void serverSelectingTheCredentialSubprotocolIsRefused() {
    Rig rig(MdkrMatchSignalTestServer::ProtocolMode::SelectSecond);
    rig.expectTerminalFailure(kMdkrMatchSignalInvalidSubprotocol);
    assert(rig.server.waitForClientClose());
    assert(rig.server.clientCloseCode() == 4003u);
    assert(rig.server.clientCloseReason() == "invalid_server_message");
}

void serverSelectingNoSubprotocolIsTransportLost() {
    /* The browser composite: an offered-protocols handshake the server
     * answers without a selection never opens -- the reference client sees
     * only the transport failure. */
    Rig rig(MdkrMatchSignalTestServer::ProtocolMode::SelectNone);
    rig.expectTerminalFailure(kMdkrMatchSignalTransportLost);
}

void serverSelectingAnUnofferedSubprotocolIsTransportLost() {
    Rig rig(MdkrMatchSignalTestServer::ProtocolMode::SelectUnoffered);
    rig.expectTerminalFailure(kMdkrMatchSignalTransportLost);
}

void welcomeWithUnsortedPeersIsTerminal() {
    expectFirstMessageRejected(welcomeMessage(7u, {{"303", 1u}, {"202", 1u}}),
                               "unsorted peers");
    /* NUMERIC order, not lexicographic: "1000" < "999" as strings. */
    expectFirstMessageRejected(welcomeMessage(7u, {{"1000", 1u}, {"999", 1u}}),
                               "lexicographic-only ordering");
}

void welcomeWithDuplicatePeersIsTerminal() {
    expectFirstMessageRejected(welcomeMessage(7u, {{"202", 1u}, {"202", 2u}}),
                               "duplicate peer");
}

void welcomeContainingSelfIsTerminal() {
    expectFirstMessageRejected(welcomeMessage(7u, {{kSelfEndpoint, 1u}}),
                               "self in peers");
}

void welcomeWithFourPeersIsTerminal() {
    expectFirstMessageRejected(
        welcomeMessage(7u, {{"202", 1u}, {"303", 1u}, {"404", 1u}, {"505", 1u}}),
        "four peers");
}

void welcomeWithExtraKeyIsTerminal() {
    Json bad = welcomeMessage(7u, {{"202", 1u}});
    bad["extra"] = 1;
    expectFirstMessageRejected(bad, "extra key");
}

void welcomeWithZeroGenerationIsTerminal() {
    expectFirstMessageRejected(welcomeMessage(0u, {}), "zero generation");
}

void welcomeForAnotherEndpointIsTerminal() {
    Json bad = welcomeMessage(7u, {});
    bad["endpointId"] = "999";
    expectFirstMessageRejected(bad, "welcome not addressed to self");
}

void nothingButWelcomeIsValidAtGenerationZero() {
    expectFirstMessageRejected(presenceMessage("202", 1u, true),
                               "presence before welcome");
}

void secondWelcomeIsTerminal() {
    Rig rig;
    rig.welcome(7u, {});
    assert(rig.server.sendText(welcomeMessage(8u, {}).dump()));
    rig.expectTerminalFailure(kMdkrMatchSignalInvalidMessage);
}

void presenceGenerationRollbackIsTerminal() {
    { /* Equal to the high-water mark. */
        Rig rig;
        rig.welcome(7u, {{"202", 5u}});
        assert(rig.server.sendText(presenceMessage("202", 5u, true).dump()));
        rig.expectTerminalFailure(kMdkrMatchSignalInvalidMessage);
    }
    { /* Below it -- and the mark survives the peer's absence. */
        Rig rig;
        rig.welcome(7u, {{"202", 5u}});
        assert(rig.server.sendText(presenceMessage("202", 5u, false).dump()));
        assert(waitForEvents(*rig.client, rig.events, rig.events.size() + 1u));
        assert(rig.server.sendText(presenceMessage("202", 4u, true).dump()));
        rig.expectTerminalFailure(kMdkrMatchSignalInvalidMessage);
    }
}

void presenceAbsentWithWrongGenerationIsTerminal() {
    {
        Rig rig;
        rig.welcome(7u, {{"202", 5u}});
        assert(rig.server.sendText(presenceMessage("202", 4u, false).dump()));
        rig.expectTerminalFailure(kMdkrMatchSignalInvalidMessage);
    }
    { /* Absence for a peer that is not present at all. */
        Rig rig;
        rig.welcome(7u, {});
        assert(rig.server.sendText(presenceMessage("303", 1u, false).dump()));
        rig.expectTerminalFailure(kMdkrMatchSignalInvalidMessage);
    }
}

void staleSenderGenerationIsTerminal() {
    {
        Rig rig;
        rig.welcome(7u, {{"202", 5u}});
        Json hello = directedCommon("peer_hello", 1u, 7u, "202", 4u);
        hello["publicKey"] = validPublicKey();
        assert(rig.server.sendText(hello.dump()));
        rig.expectTerminalFailure(kMdkrMatchSignalInvalidMessage);
    }
    { /* Unknown sender entirely. */
        Rig rig;
        rig.welcome(7u, {});
        Json hello = directedCommon("peer_hello", 1u, 7u, "202", 1u);
        hello["publicKey"] = validPublicKey();
        assert(rig.server.sendText(hello.dump()));
        rig.expectTerminalFailure(kMdkrMatchSignalInvalidMessage);
    }
}

void sequenceReplayIsTerminal() {
    Rig rig;
    rig.welcome(7u, {{"202", 5u}});
    Json hello = directedCommon("peer_hello", 7u, 7u, "202", 5u);
    hello["publicKey"] = validPublicKey();
    assert(rig.server.sendText(hello.dump()));
    assert(waitForEvents(*rig.client, rig.events, rig.events.size() + 1u));
    Json replay = directedCommon("webrtc_offer", 7u, 7u, "202", 5u);
    replay["sdp"] = "v=0";
    assert(rig.server.sendText(replay.dump()));
    rig.expectTerminalFailure(kMdkrMatchSignalInvalidMessage);
}

void directedMessageMustNameSelfAndCurrentGeneration() {
    { /* Addressed to another endpoint. */
        Rig rig;
        rig.welcome(7u, {{"202", 5u}});
        Json hello = directedCommon("peer_hello", 1u, 7u, "202", 5u);
        hello["toEndpointId"] = "303";
        hello["publicKey"] = validPublicKey();
        assert(rig.server.sendText(hello.dump()));
        rig.expectTerminalFailure(kMdkrMatchSignalInvalidMessage);
    }
    { /* Addressed to a stale own generation. */
        Rig rig;
        rig.welcome(7u, {{"202", 5u}});
        Json hello = directedCommon("peer_hello", 1u, 6u, "202", 5u);
        hello["publicKey"] = validPublicKey();
        assert(rig.server.sendText(hello.dump()));
        rig.expectTerminalFailure(kMdkrMatchSignalInvalidMessage);
    }
}

void signalErrorWithoutARecordedTargetIsTerminal() {
    Rig rig;
    rig.welcome(7u, {{"202", 5u}});
    assert(rig.server.sendText(Json{{"protocolVersion", 1},
                                    {"type", "signal_error"},
                                    {"sequence", 1u},
                                    {"toEndpointId", "202"},
                                    {"toConnectionGeneration", 5u},
                                    {"error", "peer_unavailable"}}.dump()));
    rig.expectTerminalFailure(kMdkrMatchSignalInvalidMessage);
}

void signalErrorTargetMismatchIsTerminal() {
    Rig rig;
    rig.welcome(7u, {{"202", 5u}});
    MdkrMatchSignalOutbound hello;
    hello.type = "peer_hello";
    hello.toEndpointId = "202";
    hello.toConnectionGeneration = 5u;
    hello.publicKey = validPublicKey();
    const MdkrMatchSignalSendResult sent = rig.client->send(hello);
    assert(sent.ok && sent.sequence == 1u);
    /* Right sequence, wrong recorded generation. */
    assert(rig.server.sendText(Json{{"protocolVersion", 1},
                                    {"type", "signal_error"},
                                    {"sequence", 1u},
                                    {"toEndpointId", "202"},
                                    {"toConnectionGeneration", 6u},
                                    {"error", "peer_unavailable"}}.dump()));
    rig.expectTerminalFailure(kMdkrMatchSignalInvalidMessage);
}

void oversizeFrameIsTerminalBeforeParse() {
    Rig rig;
    rig.welcome(7u, {});
    /* 64 KiB + 1 of bytes that would ALSO be malformed JSON -- but the
     * bound must trip first, from the byte length alone. */
    assert(rig.server.sendText(std::string(64u * 1024u + 1u, 'a')));
    rig.expectTerminalFailure(kMdkrMatchSignalInvalidMessage);
}

void malformedJsonIsTerminal() {
    Rig rig;
    rig.welcome(7u, {});
    assert(rig.server.sendText("{\"protocolVersion\":1,"));
    rig.expectTerminalFailure(kMdkrMatchSignalInvalidMessage);
    assert(rig.server.waitForClientClose());
    assert(rig.server.clientCloseCode() == 4003u);
    assert(rig.server.clientCloseReason() == "invalid_server_message");
}

void binaryFrameIsTerminal() {
    Rig rig;
    rig.welcome(7u, {});
    assert(rig.server.sendBinary(presenceMessage("202", 1u, true).dump()));
    rig.expectTerminalFailure(kMdkrMatchSignalInvalidMessage);
}

void peerGenerationOverflowFailsClosed() {
    Rig rig;
    rig.welcome(7u, {{"202", 1u}}); /* 1 tracked id */
    /* 63 more distinct ids fill the 64-entry map exactly. */
    for (unsigned index = 0u; index < 63u; index++) {
        const std::string id = std::to_string(1000u + index);
        assert(rig.server.sendText(presenceMessage(id, 1u, true).dump()));
    }
    assert(waitForEvents(*rig.client, rig.events, rig.events.size() + 63u));
    assert(rig.client->snapshot().phase == MdkrMatchSignalPhase::Open);
    /* The 65th distinct id must fail CLOSED, not evict. */
    assert(rig.server.sendText(presenceMessage("2000", 1u, true).dump()));
    rig.expectTerminalFailure(kMdkrMatchSignalPeerGenerationOverflow);
}

void silentServerTimesOutWithSignalTimeout() {
    /* timeoutMs 1 clamps to the JS floor of 2000. */
    const auto start = std::chrono::steady_clock::now();
    Rig rig(MdkrMatchSignalTestServer::ProtocolMode::SelectV1, 1u);
    assert(rig.server.waitForOpen());
    rig.expectTerminalFailure(kMdkrMatchSignalTimeout, 6000u);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - start)
                             .count();
    assert(elapsed >= 1900 && elapsed < 5000);
}

void stalledHandshakeTimesOutWithSignalTimeout() {
    Rig rig(MdkrMatchSignalTestServer::ProtocolMode::SelectV1, 1u,
            /*respond=*/false);
    assert(rig.server.waitForUpgrade());
    rig.expectTerminalFailure(kMdkrMatchSignalTimeout, 6000u);
}

void sendToAnUnknownGenerationIsRefusedLocally() {
    Rig rig;
    rig.welcome(7u, {{"202", 5u}});
    MdkrMatchSignalOutbound offer;
    offer.type = "webrtc_offer";
    offer.toEndpointId = "202";
    offer.toConnectionGeneration = 6u; /* not the tracked generation */
    offer.sdp = "v=0";
    MdkrMatchSignalSendResult result = rig.client->send(offer);
    assert(!result.ok);
    assert(result.error == kMdkrMatchSignalPeerUnavailable);
    offer.toEndpointId = "999"; /* never tracked at all */
    offer.toConnectionGeneration = 1u;
    result = rig.client->send(offer);
    assert(!result.ok);
    assert(result.error == kMdkrMatchSignalPeerUnavailable);
    /* Local refusals are not terminal and consume no sequence. */
    assert(rig.client->snapshot().phase == MdkrMatchSignalPhase::Open);
    assert(rig.client->snapshot().nextSequence == 1u);
    offer.toEndpointId = "202";
    offer.toConnectionGeneration = 5u;
    result = rig.client->send(offer);
    assert(result.ok);
    assert(result.sequence == 1u);
}

void invalidOutboundMessagesAreRefusedBeforeTheWire() {
    Rig rig;
    rig.welcome(7u, {{"202", 5u}});
    const auto refused = [&](const MdkrMatchSignalOutbound &message) {
        const MdkrMatchSignalSendResult result = rig.client->send(message);
        assert(!result.ok);
        assert(result.error == kMdkrMatchSignalInvalidClientMessage);
    };
    MdkrMatchSignalOutbound base;
    base.toEndpointId = "202";
    base.toConnectionGeneration = 5u;

    { MdkrMatchSignalOutbound m = base; m.type = "bogus_type"; refused(m); }
    { MdkrMatchSignalOutbound m = base; m.type = "peer_hello";
      m.publicKey = validPublicKey().substr(0u, 86u); refused(m); }
    { /* 87 valid chars but noncanonical trailing bits. */
      MdkrMatchSignalOutbound m = base; m.type = "peer_hello";
      m.publicKey = validPublicKey();
      m.publicKey[86] = 'B'; /* low bits no longer zero */
      refused(m); }
    { /* Valid shape but an all-zero point body. */
      MdkrMatchSignalOutbound m = base; m.type = "peer_hello";
      uint8_t raw[65] = {0x04u};
      m.publicKey = mdkr_party::base64Url(raw, sizeof(raw));
      refused(m); }
    { MdkrMatchSignalOutbound m = base; m.type = "webrtc_offer";
      m.sdp = ""; refused(m); }
    { MdkrMatchSignalOutbound m = base; m.type = "webrtc_offer";
      m.sdp = std::string(60u * 1024u + 1u, 's'); refused(m); }
    { MdkrMatchSignalOutbound m = base; m.type = "webrtc_answer";
      m.sdp = std::string("v=0\0x", 5u); /* embedded NUL */ refused(m); }
    { MdkrMatchSignalOutbound m = base; m.type = "webrtc_ice";
      m.candidate = std::string(4097u, 'c'); refused(m); }
    { MdkrMatchSignalOutbound m = base; m.type = "webrtc_ice";
      m.candidate = "candidate:1"; m.hasSdpMid = true;
      m.sdpMid = std::string(257u, 'm'); refused(m); }
    { MdkrMatchSignalOutbound m = base; m.type = "webrtc_ice";
      m.candidate = "candidate:1"; m.hasSdpMLineIndex = true;
      m.sdpMLineIndex = 256u; refused(m); }
    { MdkrMatchSignalOutbound m = base; m.type = "peer_end";
      m.reason = "quit"; refused(m); }
    { /* Self-targeting. */
      MdkrMatchSignalOutbound m; m.type = "peer_end"; m.reason = "close";
      m.toEndpointId = kSelfEndpoint; m.toConnectionGeneration = 5u;
      refused(m); }
    { /* Zero target generation. */
      MdkrMatchSignalOutbound m; m.type = "peer_end"; m.reason = "close";
      m.toEndpointId = "202"; m.toConnectionGeneration = 0u; refused(m); }
    { /* Cross-type payload: a hello carrying an sdp (exact-key mirror). */
      MdkrMatchSignalOutbound m = base; m.type = "peer_hello";
      m.publicKey = validPublicKey(); m.sdp = "v=0"; refused(m); }
    { /* peer_end carrying ICE metadata. */
      MdkrMatchSignalOutbound m = base; m.type = "peer_end";
      m.reason = "close"; m.hasSdpMid = true; m.sdpMid = "0"; refused(m); }

    /* No sequence consumed and nothing reached the wire. */
    assert(rig.client->snapshot().nextSequence == 1u);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    assert(rig.server.textMessages().empty());
    /* And the client is still healthy. */
    MdkrMatchSignalOutbound good = base;
    good.type = "peer_end";
    good.reason = "close";
    const MdkrMatchSignalSendResult sent = rig.client->send(good);
    assert(sent.ok && sent.sequence == 1u);
}

/* Fix round 1, Important 1: an outbound text field that is not well-formed
 * UTF-8 must be the same "invalid match signal message" value-refusal as
 * every other bad shape -- never an exception escaping send() (nlohmann's
 * dump() throws type_error.316 on invalid UTF-8). */
void invalidUtf8OutboundIsRefusedNotThrown() {
    Rig rig;
    rig.welcome(7u, {{"202", 5u}});
    const auto refused = [&](const MdkrMatchSignalOutbound &message) {
        const MdkrMatchSignalSendResult result = rig.client->send(message);
        assert(!result.ok);
        assert(result.error == kMdkrMatchSignalInvalidClientMessage);
    };
    MdkrMatchSignalOutbound base;
    base.toEndpointId = "202";
    base.toConnectionGeneration = 5u;
    { /* Stray continuation byte in an sdp. */
        MdkrMatchSignalOutbound m = base;
        m.type = "webrtc_offer";
        m.sdp = std::string("v=0\x80stray");
        refused(m);
    }
    { /* Truncated multi-byte sequence at the end of a candidate. */
        MdkrMatchSignalOutbound m = base;
        m.type = "webrtc_ice";
        m.candidate = std::string("candidate:1\xc3");
        refused(m);
    }
    { /* Overlong encoding (0xC0 0xAF is an overlong '/'). */
        MdkrMatchSignalOutbound m = base;
        m.type = "webrtc_ice";
        m.candidate = "candidate:1";
        m.hasSdpMid = true;
        m.sdpMid = std::string("\xc0\xaf");
        refused(m);
    }
    { /* CESU-8 style surrogate half (ED A0 80). */
        MdkrMatchSignalOutbound m = base;
        m.type = "webrtc_answer";
        m.sdp = std::string("v=0\xed\xa0\x80");
        refused(m);
    }
    /* No sequence consumed, nothing on the wire, and the client still
     * accepts a valid (multi-byte UTF-8) payload afterwards. */
    assert(rig.client->snapshot().nextSequence == 1u);
    MdkrMatchSignalOutbound good = base;
    good.type = "webrtc_offer";
    good.sdp = std::string("v=0 caf\xc3\xa9 \xe2\x9c\x93");
    const MdkrMatchSignalSendResult sent = rig.client->send(good);
    assert(sent.ok && sent.sequence == 1u);
    assert(rig.server.waitForTextMessages(1u));
}

/* Fix round 1, Important 2: a peer that stops draining the socket (zero
 * receive window) while the launcher queues large payloads must not turn
 * close() into a hang -- the blocked write is aborted and the socket thread
 * joins promptly. */
void closeReturnsPromptlyWithABlockedWrite() {
    Rig rig;
    rig.welcome(7u, {{"202", 5u}});
    rig.server.stopReading();
    MdkrMatchSignalOutbound offer;
    offer.type = "webrtc_offer";
    offer.toEndpointId = "202";
    offer.toConnectionGeneration = 5u;
    offer.sdp = std::string(60u * 1024u, 's');
    /* ~4.2 MB queued: far past what loopback send+receive buffers absorb,
     * so the socket thread ends up blocked inside a send. */
    for (unsigned index = 0u; index < 70u; index++) {
        const MdkrMatchSignalSendResult sent = rig.client->send(offer);
        assert(sent.ok);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    const auto start = std::chrono::steady_clock::now();
    std::atomic<bool> closed{false};
    std::thread closer([&]() {
        rig.client->close();
        closed = true;
    });
    const auto deadline = start + std::chrono::milliseconds(3000);
    while (!closed && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    assert(closed); /* pre-fix: the join blocks behind ::send forever */
    closer.join();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - start)
                             .count();
    assert(elapsed < 3000);
    assert(rig.client->snapshot().phase == MdkrMatchSignalPhase::Closed);
}

/* Fix round 1, minor: RFC 6455 4.1 -- a 101 carrying a Sec-WebSocket-
 * Extensions the client never requested must fail the connection. */
void unrequestedExtensionOnThe101IsRefused() {
    Rig rig(MdkrMatchSignalTestServer::ProtocolMode::SelectV1, 10000u, true,
            "Sec-WebSocket-Extensions: permessage-deflate");
    rig.expectTerminalFailure(kMdkrMatchSignalTransportLost);
}

void abruptTransportLossIsTerminal() {
    Rig rig;
    rig.welcome(7u, {});
    rig.server.dropConnection();
    rig.expectTerminalFailure(kMdkrMatchSignalTransportLost);
}

void serverCloseFrameIsTransportLost() {
    Rig rig;
    rig.welcome(7u, {});
    assert(rig.server.sendClose(1001u, "going_away"));
    rig.expectTerminalFailure(kMdkrMatchSignalTransportLost);
}

} // namespace

int main() {
#ifdef _WIN32
    _putenv_s("MDKR_INTERNAL_TEST_TOKEN", "mdkr64-party-e2e-v1");
#else
    setenv("MDKR_INTERNAL_TEST_TOKEN", "mdkr64-party-e2e-v1", 1);
#endif

    createRefusesInvalidIdentityAndForeignOrigins();
    happyPathRoundTripPinsTheFullContract();

    serverSelectingTheCredentialSubprotocolIsRefused();
    serverSelectingNoSubprotocolIsTransportLost();
    serverSelectingAnUnofferedSubprotocolIsTransportLost();

    welcomeWithUnsortedPeersIsTerminal();
    welcomeWithDuplicatePeersIsTerminal();
    welcomeContainingSelfIsTerminal();
    welcomeWithFourPeersIsTerminal();
    welcomeWithExtraKeyIsTerminal();
    welcomeWithZeroGenerationIsTerminal();
    welcomeForAnotherEndpointIsTerminal();
    nothingButWelcomeIsValidAtGenerationZero();
    secondWelcomeIsTerminal();

    presenceGenerationRollbackIsTerminal();
    presenceAbsentWithWrongGenerationIsTerminal();
    staleSenderGenerationIsTerminal();
    sequenceReplayIsTerminal();
    directedMessageMustNameSelfAndCurrentGeneration();
    signalErrorWithoutARecordedTargetIsTerminal();
    signalErrorTargetMismatchIsTerminal();

    oversizeFrameIsTerminalBeforeParse();
    malformedJsonIsTerminal();
    binaryFrameIsTerminal();
    peerGenerationOverflowFailsClosed();

    silentServerTimesOutWithSignalTimeout();
    stalledHandshakeTimesOutWithSignalTimeout();

    sendToAnUnknownGenerationIsRefusedLocally();
    invalidOutboundMessagesAreRefusedBeforeTheWire();
    abruptTransportLossIsTerminal();
    serverCloseFrameIsTransportLost();

    /* Fix round 1. */
    invalidUtf8OutboundIsRefusedNotThrown();
    closeReturnsPromptlyWithABlockedWrite();
    unrequestedExtensionOnThe101IsRefused();

    std::fprintf(stderr, "test_match_signal_client: all cases passed\n");
    return 0;
}
