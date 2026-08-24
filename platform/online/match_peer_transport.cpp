#include "online/match_peer_transport.h"

#include "net/match_peer_transcript.h"
#include "party/party_retry_policy.h"
#include "party/party_webrtc_signaling.h"

#include <nlohmann/json.hpp>
#include <rtc/rtc.hpp>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <utility>
#include <variant>

/*
 * Composition-only: every byte of key schedule, sealing and phrase logic
 * lives in platform/net/{match_peer_crypto,match_peer_transcript}; every
 * signaling byte in the launcher's validated signal client behind the
 * MdkrMatchPeerSignalFeed seam; connection discipline mirrors
 * libdatachannel_party_transport.cpp (bounded callback queue, launcher
 * pump, attempt-guarded stale rejection, mdkr_party_retry_decide ladder).
 *
 * ONE-HOP FORWARDING SEAM: this revision wires the direct mesh only. When a
 * pair's direct path is not admissible, the graph planner
 * (match_peer_graph.h) selects the deterministic one-hop intermediate and
 * the forwarder admission window (match_peer_forward.h) bounds relayed
 * ciphertext; both would plug into stateData()/routing below without
 * touching the crypto layers. Out of scope for O-T2 by design.
 */

namespace {

using Json = nlohmann::json;

constexpr size_t kMaxInternalEvents = 512u;
constexpr size_t kMaxPublicEvents = 256u;
constexpr size_t kMaxControlTextBytes = 4096u;
constexpr size_t kMaxSdpBytes = 60u * 1024u;
constexpr size_t kMaxCandidateBytes = 4096u;
/* Channel-message protocol version for ping/pong (the party discipline). */
constexpr unsigned kChannelProtocol = 1u;
constexpr size_t kMaxPeerMessageBytes = 4096u;

uint64_t steadyNowMs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

bool parseEndpointId(const std::string &value, uint64_t &out) {
    if (value.empty() || value.size() > 20u || value[0] == '0') return false;
    uint64_t result = 0u;
    for (const char byte : value) {
        if (byte < '0' || byte > '9') return false;
        const uint64_t digit = static_cast<uint64_t>(byte - '0');
        if (result > (UINT64_MAX - digit) / 10u) return false;
        result = result * 10u + digit;
    }
    if (result == 0u) return false;
    out = result;
    return true;
}

/* 22-char canonical base64url room id -> 16 bytes (the transcript's room
 * binding). Canonical means re-encoding reproduces the input exactly, the
 * same rule mdkr_party::decodePublicKey applies to keys. */
bool decodeRoomId(const std::string &value, uint8_t out[16]) {
    if (value.size() != 22u) return false;
    uint32_t accumulator = 0u;
    unsigned bits = 0u;
    size_t produced = 0u;
    for (const char byte : value) {
        int digit = -1;
        if (byte >= 'A' && byte <= 'Z') digit = byte - 'A';
        else if (byte >= 'a' && byte <= 'z') digit = byte - 'a' + 26;
        else if (byte >= '0' && byte <= '9') digit = byte - '0' + 52;
        else if (byte == '-') digit = 62;
        else if (byte == '_') digit = 63;
        else return false;
        accumulator = (accumulator << 6u) | static_cast<uint32_t>(digit);
        bits += 6u;
        while (bits >= 8u && produced < 16u) {
            bits -= 8u;
            out[produced++] = static_cast<uint8_t>(accumulator >> bits);
        }
    }
    if (produced != 16u) return false;
    return mdkr_party::base64Url(out, 16u) == value;
}

bool secureRandom(uint8_t *out, size_t length) {
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&drbg);
    const unsigned char personalization[] = "mdkr-match-peer-mesh";
    const bool ok =
        mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy,
                              personalization,
                              sizeof(personalization) - 1u) == 0 &&
        mbedtls_ctr_drbg_random(&drbg, out, length) == 0;
    mbedtls_ctr_drbg_free(&drbg);
    mbedtls_entropy_free(&entropy);
    return ok;
}

/* Hello blobs: 0x04 || body[32] || zero[32], canonical base64url. See the
 * header's commitment-transport contract. */
std::string encodeHelloBody(const uint8_t body[32]) {
    uint8_t raw[MDKR_MATCH_PEER_PUBLIC_KEY_BYTES] = {};
    raw[0] = 0x04u;
    std::memcpy(raw + 1u, body, 32u);
    return mdkr_party::base64Url(raw, sizeof(raw));
}

struct InternalEvent {
    enum class Kind {
        LocalDescription,
        LocalCandidate,
        ConnectionDown,
        ChannelAdopted,
        ChannelOpen,
        StateData,
        ControlBinary,
        ControlText,
    };
    Kind kind = Kind::ConnectionDown;
    uint64_t peer = 0u;
    uint32_t attempt = 0u;
    bool isState = false;
    std::string text; /* sdp / candidate / control text */
    std::string extra; /* description type / candidate mid */
    std::vector<uint8_t> bytes;
    std::shared_ptr<rtc::DataChannel> channel;
};

struct PeerRuntime {
    uint64_t endpointId = 0u;
    uint8_t slotMask = 0u;
    uint32_t generation = 0u; /* 0 until welcome/presence names it */
    bool present = false;
    bool offerer = false;

    /* Three-hello commitment exchange, both directions. */
    unsigned hellosSent = 0u;
    unsigned hellosReceived = 0u;
    uint8_t commitment[MDKR_MATCH_PEER_COMMIT_BYTES] = {};
    uint8_t publicKey[MDKR_MATCH_PEER_PUBLIC_KEY_BYTES] = {};
    uint8_t commitNonce[MDKR_MATCH_PEER_COMMIT_NONCE_BYTES] = {};
    bool helloComplete = false;

    /* WebRTC episode state. `attempt` rotates on every teardown so stale
     * callbacks and stale answers can never touch a fresh connection. */
    uint32_t attempt = 0u;
    std::shared_ptr<rtc::PeerConnection> connection;
    std::shared_ptr<rtc::DataChannel> state;
    std::shared_ptr<rtc::DataChannel> control;
    bool stateOpen = false;
    bool controlOpen = false;
    bool channelsReady = false;
    bool answerApplied = false;
    uint64_t offerSentMs = 0u;
    unsigned offerAttempts = 0u;
    bool gaveUp = false;
    unsigned restartEpisodes = 0u;
    /* M4 bounds: consecutive PeerConnection construction failures (an
     * offerer whose ctor throws must not recreate unboundedly every tick),
     * and the answerer's setup-deadline anchor (0 = not armed). */
    unsigned buildFailures = 0u;
    uint64_t setupStartedMs = 0u;
    bool lost = false;

    /* Control ping ladder. */
    uint32_t pingNonce = 0u;
    uint64_t nextPingAtMs = 0u;
    uint64_t pingOutstandingSinceMs = 0u;

    /* Derived directional keys (slots in the mesh keyring) + replay. The
     * two channels are independent streams over one sender sequence space:
     * a reliable control fragment delayed behind >64 lossy state envelopes
     * is honest traffic, so each channel keeps its own 64-deep window (its
     * own subsequence stays monotonic-enough) instead of one shared window
     * retiring the laggard as REPLAY. */
    MdkrMatchPeerSealingKey *sealKey = nullptr;
    MdkrMatchPeerSealingKey *openKey = nullptr;
    MdkrMatchPeerReplayWindow replayState{};
    MdkrMatchPeerReplayWindow replayControl{};
};

}  // namespace

struct MdkrMatchPeerMesh::State
    : public std::enable_shared_from_this<MdkrMatchPeerMesh::State> {
    /* ---- Options, fixed at create ------------------------------------- */
    MdkrMatchPeerSignalFeed *feed = nullptr;
    uint64_t localEndpointId = 0u;
    uint32_t expectedLocalGeneration = 0u;
    uint32_t matchEpoch = 0u;
    MdkrOnlineCompatibilityV1 compatibility{};
    std::vector<MdkrMatchPeerIceServer> iceServers;
    std::function<uint64_t()> clock;
    uint8_t roomId[MDKR_MATCH_PEER_ROOM_ID_BYTES] = {};

    /* ---- Identity + key schedule --------------------------------------- */
    MdkrMatchPeerIdentity *identity = nullptr;
    uint8_t ownPublicKey[MDKR_MATCH_PEER_PUBLIC_KEY_BYTES] = {};
    uint8_t ownNonce[MDKR_MATCH_PEER_COMMIT_NONCE_BYTES] = {};
    uint8_t ownCommitment[MDKR_MATCH_PEER_COMMIT_BYTES] = {};
    bool ownCommitmentReady = false;
    MdkrMatchPeerKeyring keyring{};
    bool keysDerived = false;
    std::string verificationPhrase;
    uint8_t transcriptDigestBytes[MDKR_MATCH_PEER_TRANSCRIPT_DIGEST_BYTES] = {};

    /* ---- Launcher-thread state ----------------------------------------- */
    std::map<uint64_t, PeerRuntime> peers;
    uint32_t localGeneration = 0u;
    bool welcomed = false;
    bool signalHealthy = true;
    bool signalLossReported = false;
    bool failed = false;
    bool closed = false;
    std::deque<MdkrMatchPeerMeshEvent> events;
    MdkrMatchPeerMeshStats counters;

    /* ---- Callback -> pump bounded queue --------------------------------- */
    std::mutex queueMutex;
    std::deque<InternalEvent> internalQueue;
    uint64_t droppedInternal = 0u;

    ~State() { teardown(); }

    uint64_t now() const {
        const uint64_t value = clock ? clock() : steadyNowMs();
        return value == 0u ? 1u : value;
    }

    /* ---- Event emission -------------------------------------------------*/
    void emit(MdkrMatchPeerMeshEvent event) {
        if (events.size() >= kMaxPublicEvents) {
            events.pop_front();
            counters.droppedEvents++;
        }
        events.push_back(std::move(event));
    }

    void emitFailure(MdkrMatchPeerMeshFailure failure,
                     std::string failureCode = std::string()) {
        MdkrMatchPeerMeshEvent event;
        event.type = MdkrMatchPeerMeshEventType::Failure;
        event.failure = failure;
        event.failureCode = std::move(failureCode);
        emit(std::move(event));
    }

    void enqueueInternal(InternalEvent event) {
        std::lock_guard<std::mutex> lock(queueMutex);
        if (internalQueue.size() >= kMaxInternalEvents) {
            internalQueue.pop_front();
            droppedInternal++;
        }
        internalQueue.push_back(std::move(event));
    }

    /* ---- Signaling sends (launcher thread only) ------------------------- */
    bool sendSignal(PeerRuntime &peer, MdkrMatchSignalOutbound message) {
        message.toEndpointId = std::to_string(peer.endpointId);
        message.toConnectionGeneration = peer.generation;
        const MdkrMatchSignalSendResult result = feed->send(message);
        if (!result.ok && result.error == kMdkrMatchSignalNotConnected) {
            noteSignalLost(result.error);
        }
        return result.ok;
    }

    void noteSignalLost(const std::string &code) {
        signalHealthy = false;
        if (!signalLossReported) {
            signalLossReported = true;
            emitFailure(MdkrMatchPeerMeshFailure::SignalLost, code);
        }
    }

    /* ---- Peer lifecycle -------------------------------------------------*/

    /* Rotate the attempt (orphaning every callback of the old connection)
     * and drop the connection WITHOUT reporting anything. */
    void silentTeardown(PeerRuntime &peer) {
        peer.attempt++;
        std::shared_ptr<rtc::PeerConnection> stale = std::move(peer.connection);
        peer.connection.reset();
        peer.state.reset();
        peer.control.reset();
        peer.stateOpen = false;
        peer.controlOpen = false;
        peer.channelsReady = false;
        peer.answerApplied = false;
        peer.offerSentMs = 0u;
        peer.pingOutstandingSinceMs = 0u;
        peer.nextPingAtMs = 0u;
        peer.setupStartedMs = 0u; /* the answerer deadline re-arms fresh */
        if (stale) {
            try { stale->close(); } catch (...) {}
        }
    }

    void peerLost(PeerRuntime &peer, MdkrMatchPeerLostReason reason) {
        if (peer.lost) return;
        peer.lost = true;
        silentTeardown(peer);
        MdkrMatchPeerMeshEvent event;
        event.type = MdkrMatchPeerMeshEventType::PeerLost;
        event.endpointId = peer.endpointId;
        event.lostReason = reason;
        emit(std::move(event));
    }

    /* A generation change is a replaced peer: retire keys AND channels,
     * restart the hello exchange, compare a new phrase later. */
    void rekeyPeer(PeerRuntime &peer, uint32_t generation) {
        peer.generation = generation;
        peer.hellosSent = 0u;
        peer.hellosReceived = 0u;
        peer.helloComplete = false;
        std::memset(peer.commitment, 0, sizeof(peer.commitment));
        std::memset(peer.publicKey, 0, sizeof(peer.publicKey));
        std::memset(peer.commitNonce, 0, sizeof(peer.commitNonce));
        peer.offerAttempts = 0u;
        peer.gaveUp = false;
        peer.restartEpisodes = 0u;
        peer.lost = false;
        silentTeardown(peer);
        if (keysDerived) {
            /* Every pairwise key was salted with the old transcript digest;
             * all of them retire together and re-derive when the exchange
             * completes again. */
            keysDerived = false;
            verificationPhrase.clear();
            mdkr_match_peer_keyring_forget(&keyring);
            for (auto &entry : peers) {
                entry.second.sealKey = nullptr;
                entry.second.openKey = nullptr;
                entry.second.replayState = MdkrMatchPeerReplayWindow{};
                entry.second.replayControl = MdkrMatchPeerReplayWindow{};
            }
        }
    }

    /* ---- WebRTC construction --------------------------------------------*/

    rtc::Configuration rtcConfiguration() const {
        rtc::Configuration configuration;
        for (const MdkrMatchPeerIceServer &server : iceServers) {
            /* TURN-scoped credential rule (wave 0): a credentialed entry
             * naming a non-turn/turns url is never handed to the agent. */
            const bool relay = server.url.rfind("turn:", 0u) == 0u ||
                server.url.rfind("turns:", 0u) == 0u;
            if (!server.username.empty() && !relay) continue;
            try {
                rtc::IceServer resolved(server.url);
                if (!server.username.empty()) {
                    resolved.username = server.username;
                    resolved.password = server.credential;
                }
                configuration.iceServers.push_back(std::move(resolved));
            } catch (...) {
                /* One refused URL must not cost the rest. */
            }
        }
        configuration.maxMessageSize = kMaxPeerMessageBytes;
        return configuration;
    }

    void attachConnectionCallbacks(PeerRuntime &peer) {
        const std::weak_ptr<State> weak = weak_from_this();
        const uint64_t endpointId = peer.endpointId;
        const uint32_t attempt = peer.attempt;
        peer.connection->onLocalDescription(
            [weak, endpointId, attempt](rtc::Description description) {
                if (auto state = weak.lock()) {
                    InternalEvent event;
                    event.kind = InternalEvent::Kind::LocalDescription;
                    event.peer = endpointId;
                    event.attempt = attempt;
                    event.text = std::string(description);
                    event.extra = description.typeString();
                    state->enqueueInternal(std::move(event));
                }
            });
        peer.connection->onLocalCandidate(
            [weak, endpointId, attempt](rtc::Candidate candidate) {
                if (auto state = weak.lock()) {
                    InternalEvent event;
                    event.kind = InternalEvent::Kind::LocalCandidate;
                    event.peer = endpointId;
                    event.attempt = attempt;
                    /* mDNS `.local` candidates flow through opaquely: the
                     * mesh never parses candidate hostnames. */
                    event.text = std::string(candidate);
                    event.extra = candidate.mid();
                    state->enqueueInternal(std::move(event));
                }
            });
        peer.connection->onStateChange(
            [weak, endpointId, attempt](rtc::PeerConnection::State value) {
                if (value != rtc::PeerConnection::State::Disconnected &&
                    value != rtc::PeerConnection::State::Failed &&
                    value != rtc::PeerConnection::State::Closed) {
                    return;
                }
                if (auto state = weak.lock()) {
                    InternalEvent event;
                    event.kind = InternalEvent::Kind::ConnectionDown;
                    event.peer = endpointId;
                    event.attempt = attempt;
                    state->enqueueInternal(std::move(event));
                }
            });
        peer.connection->onDataChannel(
            [weak, endpointId, attempt](
                std::shared_ptr<rtc::DataChannel> channel) {
                if (auto state = weak.lock()) {
                    const std::string label = channel->label();
                    const bool isState = label == kMdkrMatchStateChannelLabel;
                    if (!isState && label != kMdkrMatchControlChannelLabel) {
                        return; /* unknown labels are ignored */
                    }
                    state->attachChannelCallbacks(channel, endpointId, attempt,
                                                  isState);
                    InternalEvent event;
                    event.kind = InternalEvent::Kind::ChannelAdopted;
                    event.peer = endpointId;
                    event.attempt = attempt;
                    event.isState = isState;
                    event.channel = std::move(channel);
                    state->enqueueInternal(std::move(event));
                }
            });
    }

    void attachChannelCallbacks(const std::shared_ptr<rtc::DataChannel> &channel,
                                uint64_t endpointId, uint32_t attempt,
                                bool isState) {
        const std::weak_ptr<State> weak = weak_from_this();
        channel->onOpen([weak, endpointId, attempt, isState]() {
            if (auto state = weak.lock()) {
                InternalEvent event;
                event.kind = InternalEvent::Kind::ChannelOpen;
                event.peer = endpointId;
                event.attempt = attempt;
                event.isState = isState;
                state->enqueueInternal(std::move(event));
            }
        });
        channel->onClosed([weak, endpointId, attempt]() {
            if (auto state = weak.lock()) {
                InternalEvent event;
                event.kind = InternalEvent::Kind::ConnectionDown;
                event.peer = endpointId;
                event.attempt = attempt;
                state->enqueueInternal(std::move(event));
            }
        });
        channel->onMessage(
            [weak, endpointId, attempt, isState](rtc::message_variant message) {
                auto state = weak.lock();
                if (!state) return;
                InternalEvent event;
                event.peer = endpointId;
                event.attempt = attempt;
                event.isState = isState;
                if (std::holds_alternative<rtc::binary>(message)) {
                    const rtc::binary &raw = std::get<rtc::binary>(message);
                    if (raw.size() > kMaxPeerMessageBytes) return;
                    event.kind = isState ? InternalEvent::Kind::StateData
                                         : InternalEvent::Kind::ControlBinary;
                    event.bytes.resize(raw.size());
                    if (!raw.empty()) {
                        std::memcpy(event.bytes.data(), raw.data(), raw.size());
                    }
                } else {
                    const std::string &text = std::get<std::string>(message);
                    if (isState) {
                        /* Text on the lossy state channel is just another
                         * bad datagram: counted and dropped in stateData. */
                        event.kind = InternalEvent::Kind::StateData;
                        event.bytes.assign(text.begin(), text.end());
                    } else {
                        /* Oversize control text is garbage on the reliable
                         * channel; the empty-text event fails JSON parsing
                         * in controlText and retires the peer, terminal. */
                        event.kind = InternalEvent::Kind::ControlText;
                        if (text.size() <= kMaxControlTextBytes) {
                            event.text = text;
                        }
                    }
                }
                state->enqueueInternal(std::move(event));
            });
    }

    /* Offerer path: register the peer's fresh attempt, create both channels
     * (which triggers the local offer). The peer entry exists before
     * createDataChannel, the party transport's callback-race discipline. */
    void createConnection(PeerRuntime &peer) {
        peer.attempt++;
        peer.answerApplied = false;
        /* M4: a machine where construction itself keeps throwing must not
         * recreate every tick forever -- bounded like everything else. */
        const auto buildFailed = [this, &peer]() {
            peer.buildFailures++;
            if (peer.buildFailures >= kMdkrMatchMaxRestartEpisodes) {
                peerLost(peer, MdkrMatchPeerLostReason::ConnectTimeout);
            }
        };
        try {
            peer.connection =
                std::make_shared<rtc::PeerConnection>(rtcConfiguration());
        } catch (...) {
            peer.connection.reset();
            buildFailed();
            return;
        }
        attachConnectionCallbacks(peer);
        rtc::DataChannelInit stateConfiguration;
        stateConfiguration.reliability.unordered = true;
        stateConfiguration.reliability.maxRetransmits = 0u;
        try {
            peer.state = peer.connection->createDataChannel(
                kMdkrMatchStateChannelLabel, stateConfiguration);
            attachChannelCallbacks(peer.state, peer.endpointId, peer.attempt,
                                   /*isState=*/true);
            peer.control = peer.connection->createDataChannel(
                kMdkrMatchControlChannelLabel);
            attachChannelCallbacks(peer.control, peer.endpointId, peer.attempt,
                                   /*isState=*/false);
        } catch (...) {
            silentTeardown(peer);
            buildFailed();
            return;
        }
        peer.buildFailures = 0u;
    }

    /* ---- Feed event handling ---------------------------------------------*/

    PeerRuntime *rosterPeer(const std::string &fromEndpointId,
                            uint32_t fromGeneration) {
        uint64_t id = 0u;
        if (!parseEndpointId(fromEndpointId, id)) {
            counters.ignoredStaleSignals++;
            return nullptr;
        }
        const auto found = peers.find(id);
        if (found == peers.end() || found->second.generation == 0u ||
            found->second.generation != fromGeneration) {
            counters.ignoredStaleSignals++;
            return nullptr;
        }
        return &found->second;
    }

    /* W3 N6b: the LOCAL endpoint's replacement signal socket. Replacement
     * sockets are first-class in the wire contract (the service assigns the
     * fresh socket a strictly higher connection generation and announces it
     * to peers as a presence bump), and the peer-side half of this already
     * existed (applyPresence -> rekeyPeer). This is the missing local half:
     * a second welcome after a signal loss means WE are the replaced
     * endpoint -- adopt the new generation, recommit the (unchanged) mesh
     * key under it with a fresh nonce, restart every pairwise exchange, and
     * un-latch the signal-health flag so connectionDown() can run the
     * peer_end/restart ladder again instead of PeerLost(TransportFailed).
     * The launcher's expectedLocalGeneration pin applies to the FIRST
     * welcome only: a replacement's generation is service-assigned and
     * necessarily different. */
    void handleReWelcome(const MdkrMatchSignalEvent &event) {
        uint64_t id = 0u;
        if (!parseEndpointId(event.endpointId, id) || id != localEndpointId ||
            event.connectionGeneration <= localGeneration) {
            /* A replayed or stale welcome can never move the mesh. */
            counters.ignoredStaleSignals++;
            return;
        }
        localGeneration = event.connectionGeneration;
        if (!secureRandom(ownNonce, sizeof(ownNonce)) ||
            !mdkr_match_peer_commitment(matchEpoch, localEndpointId,
                                        localGeneration, ownNonce,
                                        ownPublicKey, ownCommitment)) {
            failed = true;
            emitFailure(MdkrMatchPeerMeshFailure::KeyScheduleFailed);
            return;
        }
        ownCommitmentReady = true;
        signalHealthy = true;
        signalLossReported = false;
        /* Every pairwise exchange restarts: peers rekey us on the presence
         * bump, and rekeyPeer here retires all transcript-salted keys
         * (keysDerived drops until the exchange completes again). */
        for (auto &entry : peers) {
            PeerRuntime &peer = entry.second;
            rekeyPeer(peer, peer.generation);
            peer.present = false; /* the welcome's peer list re-asserts */
        }
        for (const MdkrMatchSignalPeerRef &ref : event.peers) {
            applyPresence(ref.endpointId, ref.connectionGeneration, true);
        }
    }

    void handleWelcome(const MdkrMatchSignalEvent &event) {
        if (welcomed) {
            handleReWelcome(event);
            return;
        }
        uint64_t id = 0u;
        if (!parseEndpointId(event.endpointId, id) || id != localEndpointId ||
            event.connectionGeneration == 0u ||
            (expectedLocalGeneration != 0u &&
             event.connectionGeneration != expectedLocalGeneration)) {
            failed = true;
            emitFailure(MdkrMatchPeerMeshFailure::IdentityMismatch);
            return;
        }
        welcomed = true;
        localGeneration = event.connectionGeneration;
        if (!mdkr_match_peer_commitment(matchEpoch, localEndpointId,
                                        localGeneration, ownNonce,
                                        ownPublicKey, ownCommitment)) {
            failed = true;
            emitFailure(MdkrMatchPeerMeshFailure::KeyScheduleFailed);
            return;
        }
        ownCommitmentReady = true;
        for (const MdkrMatchSignalPeerRef &ref : event.peers) {
            applyPresence(ref.endpointId, ref.connectionGeneration, true);
        }
    }

    void applyPresence(const std::string &endpointText, uint32_t generation,
                       bool present) {
        uint64_t id = 0u;
        if (!parseEndpointId(endpointText, id) || generation == 0u) {
            counters.ignoredStaleSignals++;
            return;
        }
        const auto found = peers.find(id);
        if (found == peers.end()) {
            counters.ignoredStaleSignals++; /* outside the fixed roster */
            return;
        }
        PeerRuntime &peer = found->second;
        if (!present) {
            if (peer.generation == generation) peer.present = false;
            return;
        }
        if (peer.generation == 0u) {
            peer.generation = generation;
        } else if (generation > peer.generation) {
            rekeyPeer(peer, generation);
        } else if (generation < peer.generation) {
            counters.ignoredStaleSignals++; /* high-water-mark discipline */
            return;
        }
        peer.present = true;
    }

    void handleHello(const MdkrMatchSignalEvent &event) {
        PeerRuntime *peer = rosterPeer(event.fromEndpointId,
                                       event.fromConnectionGeneration);
        if (peer == nullptr || peer->lost) return;
        std::array<uint8_t, MDKR_MATCH_PEER_PUBLIC_KEY_BYTES> raw{};
        if (!mdkr_party::decodePublicKey(event.publicKey, raw)) {
            peerLost(*peer, MdkrMatchPeerLostReason::HelloViolation);
            return;
        }
        const auto zeroTail = [&raw]() {
            for (size_t index = 33u; index < raw.size(); index++) {
                if (raw[index] != 0u) return false;
            }
            return true;
        };
        if (peer->hellosReceived == 0u) {
            if (!zeroTail()) {
                peerLost(*peer, MdkrMatchPeerLostReason::HelloViolation);
                return;
            }
            std::memcpy(peer->commitment, raw.data() + 1u,
                        MDKR_MATCH_PEER_COMMIT_BYTES);
        } else if (peer->hellosReceived == 1u) {
            std::memcpy(peer->publicKey, raw.data(), raw.size());
        } else if (peer->hellosReceived == 2u) {
            if (!zeroTail()) {
                peerLost(*peer, MdkrMatchPeerLostReason::HelloViolation);
                return;
            }
            std::memcpy(peer->commitNonce, raw.data() + 1u,
                        MDKR_MATCH_PEER_COMMIT_NONCE_BYTES);
            if (!mdkr_match_peer_commitment_verify(
                    matchEpoch, peer->endpointId, peer->generation,
                    peer->commitNonce, peer->publicKey, peer->commitment)) {
                peerLost(*peer, MdkrMatchPeerLostReason::CommitmentMismatch);
                return;
            }
            peer->helloComplete = true;
        } else {
            peerLost(*peer, MdkrMatchPeerLostReason::HelloViolation);
            return;
        }
        peer->hellosReceived++;
    }

    void handleOffer(const MdkrMatchSignalEvent &event) {
        PeerRuntime *peer = rosterPeer(event.fromEndpointId,
                                       event.fromConnectionGeneration);
        if (peer == nullptr || peer->lost) return;
        if (peer->offerer || event.sdp.empty() ||
            event.sdp.size() > kMaxSdpBytes) {
            /* Only the numerically lower endpoint may offer. */
            counters.ignoredStaleSignals++;
            return;
        }
        /* A fresh offer replaces the pair's connection: the offerer is
         * authoritative after its restart episode. */
        if (peer->connection) silentTeardown(*peer);
        peer->attempt++;
        try {
            peer->connection =
                std::make_shared<rtc::PeerConnection>(rtcConfiguration());
        } catch (...) {
            peer->connection.reset();
            return;
        }
        attachConnectionCallbacks(*peer);
        try {
            peer->connection->setRemoteDescription(
                rtc::Description(event.sdp, "offer"));
        } catch (...) {
            counters.ignoredStaleSignals++;
            silentTeardown(*peer);
        }
    }

    void handleAnswer(const MdkrMatchSignalEvent &event) {
        PeerRuntime *peer = rosterPeer(event.fromEndpointId,
                                       event.fromConnectionGeneration);
        if (peer == nullptr || peer->lost) return;
        /* M2 attempt guard: offerSentMs is zeroed by every teardown and set
         * only when the CURRENT attempt's offer actually went to the wire,
         * so an answer that arrives while it is 0 can only belong to a
         * retired attempt -- applying it to the fresh PeerConnection would
         * poison the DTLS handshake and burn a restart episode. */
        if (!peer->offerer || !peer->connection || peer->answerApplied ||
            peer->offerSentMs == 0u || event.sdp.empty() ||
            event.sdp.size() > kMaxSdpBytes) {
            counters.ignoredStaleSignals++;
            return;
        }
        try {
            peer->connection->setRemoteDescription(
                rtc::Description(event.sdp, "answer"));
            peer->answerApplied = true;
        } catch (...) {
            /* A late answer from a retired attempt must not tear down the
             * fresh one; the offer ladder still bounds the episode. */
            counters.ignoredStaleSignals++;
        }
    }

    void handleIce(const MdkrMatchSignalEvent &event) {
        PeerRuntime *peer = rosterPeer(event.fromEndpointId,
                                       event.fromConnectionGeneration);
        if (peer == nullptr || peer->lost) return;
        if (!peer->connection || event.candidate.empty() ||
            event.candidate.size() > kMaxCandidateBytes) {
            counters.ignoredStaleSignals++;
            return;
        }
        try {
            /* `.local` mDNS candidates pass through opaquely; libdatachannel
             * owns any resolution. Never parsed as hostnames here. */
            peer->connection->addRemoteCandidate(rtc::Candidate(
                event.candidate, event.hasSdpMid ? event.sdpMid : ""));
        } catch (...) {
            /* One malformed candidate cannot tear down a healthy peer. */
        }
    }

    void handlePeerEnd(const MdkrMatchSignalEvent &event) {
        PeerRuntime *peer = rosterPeer(event.fromEndpointId,
                                       event.fromConnectionGeneration);
        if (peer == nullptr || peer->lost) return;
        if (event.reason == "close") {
            peerLost(*peer, MdkrMatchPeerLostReason::PeerEnded);
            return;
        }
        /* "restart": retire this generation's connection quietly; the
         * offerer rebuilds, the answerer awaits the fresh offer. */
        silentTeardown(*peer);
        if (peer->offerer) {
            peer->offerAttempts = 0u;
            peer->gaveUp = false;
            createConnection(*peer);
        }
    }

    void handleFeedEvent(const MdkrMatchSignalEvent &event) {
        switch (event.type) {
        case MdkrMatchSignalEventType::Welcome:
            handleWelcome(event);
            break;
        case MdkrMatchSignalEventType::PeerPresence:
            applyPresence(event.endpointId, event.connectionGeneration,
                          event.present);
            break;
        case MdkrMatchSignalEventType::PeerHello:
            handleHello(event);
            break;
        case MdkrMatchSignalEventType::WebrtcOffer:
            handleOffer(event);
            break;
        case MdkrMatchSignalEventType::WebrtcAnswer:
            handleAnswer(event);
            break;
        case MdkrMatchSignalEventType::WebrtcIce:
            handleIce(event);
            break;
        case MdkrMatchSignalEventType::PeerEnd:
            handlePeerEnd(event);
            break;
        case MdkrMatchSignalEventType::SignalError:
            /* peer_unavailable: the presence feed follows; nothing to do. */
            break;
        case MdkrMatchSignalEventType::Failure:
            noteSignalLost(event.failureCode);
            break;
        }
    }

    /* ---- Internal (callback) event handling ------------------------------*/

    void handleInternal(const InternalEvent &event) {
        const auto found = peers.find(event.peer);
        if (found == peers.end()) return;
        PeerRuntime &peer = found->second;
        if (peer.lost || event.attempt != peer.attempt) return;
        switch (event.kind) {
        case InternalEvent::Kind::LocalDescription: {
            MdkrMatchSignalOutbound message;
            message.type = event.extra == "offer" ? "webrtc_offer"
                                                  : "webrtc_answer";
            message.sdp = event.text;
            if (event.extra == "offer") {
                peer.offerSentMs = now();
                peer.offerAttempts++;
            }
            (void)sendSignal(peer, std::move(message));
            break;
        }
        case InternalEvent::Kind::LocalCandidate: {
            MdkrMatchSignalOutbound message;
            message.type = "webrtc_ice";
            message.candidate = event.text;
            message.hasSdpMid = true;
            message.sdpMid = event.extra;
            message.hasSdpMLineIndex = false;
            message.hasUsernameFragment = false;
            (void)sendSignal(peer, std::move(message));
            break;
        }
        case InternalEvent::Kind::ConnectionDown:
            connectionDown(peer);
            break;
        case InternalEvent::Kind::ChannelAdopted:
            if (event.isState) peer.state = event.channel;
            else peer.control = event.channel;
            if (event.channel && event.channel->isOpen()) {
                channelOpened(peer, event.isState);
            }
            break;
        case InternalEvent::Kind::ChannelOpen:
            channelOpened(peer, event.isState);
            break;
        case InternalEvent::Kind::StateData:
            stateData(peer, event.bytes);
            break;
        case InternalEvent::Kind::ControlBinary:
            controlBinary(peer, event.bytes);
            break;
        case InternalEvent::Kind::ControlText:
            controlText(peer, event.text);
            break;
        }
    }

    void channelOpened(PeerRuntime &peer, bool isState) {
        if (isState) peer.stateOpen = true;
        else peer.controlOpen = true;
        if (!peer.channelsReady && peer.stateOpen && peer.controlOpen) {
            peer.channelsReady = true;
            peer.pingOutstandingSinceMs = 0u;
            peer.nextPingAtMs = now() + kMdkrMatchControlPingIntervalMs;
            MdkrMatchPeerMeshEvent event;
            event.type = MdkrMatchPeerMeshEventType::PeerChannelsReady;
            event.endpointId = peer.endpointId;
            emit(std::move(event));
        }
    }

    void connectionDown(PeerRuntime &peer) {
        if (closed || peer.lost) return;
        if (!signalHealthy) {
            /* No signaling, no restart offer: the peer is gone for good. */
            peerLost(peer, MdkrMatchPeerLostReason::TransportFailed);
            return;
        }
        if (peer.restartEpisodes >= kMdkrMatchMaxRestartEpisodes) {
            peerLost(peer, MdkrMatchPeerLostReason::ConnectTimeout);
            return;
        }
        peer.restartEpisodes++;
        silentTeardown(peer);
        MdkrMatchSignalOutbound message;
        message.type = "peer_end";
        message.reason = "restart";
        (void)sendSignal(peer, std::move(message));
        if (peer.offerer) {
            peer.offerAttempts = 0u;
            peer.gaveUp = false;
            createConnection(peer);
        }
    }

    void stateData(PeerRuntime &peer, const std::vector<uint8_t> &bytes) {
        /* Lossy by design: any rejection here is a counted drop, never
         * terminal -- a single bad datagram cannot end a race.
         *
         * FORWARDING SEAM: a one-hop route would first
         * mdkr_match_peer_inspect() the envelope and, when the header names
         * this endpoint as intermediate, run mdkr_match_peer_forwarder_admit
         * against the current graph route and relay the unchanged bytes. */
        if (bytes.size() != MDKR_MATCH_PEER_ENVELOPE_BYTES || !keysDerived ||
            peer.openKey == nullptr) {
            counters.rejectedStateEnvelopes++;
            return;
        }
        MdkrMatchPeerEnvelopeContext context{};
        MdkrMatchPeerMeshEvent event;
        if (mdkr_match_peer_open(peer.openKey, &peer.openKey->direction,
                                 &peer.replayState, bytes.data(), &context,
                                 event.payload.data()) !=
                MDKR_MATCH_PEER_CRYPTO_OK ||
            context.payload_type != MDKR_MATCH_PEER_PAYLOAD_INPUT) {
            counters.rejectedStateEnvelopes++;
            return;
        }
        event.type = MdkrMatchPeerMeshEventType::InputEnvelope;
        event.endpointId = peer.endpointId;
        event.context = context;
        emit(std::move(event));
    }

    void controlBinary(PeerRuntime &peer, const std::vector<uint8_t> &bytes) {
        /* The reliable ordered channel never delivers STRUCTURAL garbage:
         * a conforming peer only ever sends 132-byte envelopes here, so a
         * wrong-size frame is terminal. An envelope that fails to OPEN is
         * not: any roster peer's generation bump retires every
         * transcript-salted key mesh-wide, and an honest third peer's
         * in-flight fragment sealed under the old digest -- or one that
         * lands before this side finished (re-)deriving -- authenticates
         * as garbage while being nothing of the sort. Those are counted
         * drops; the preflight layer's own retry discipline re-carries
         * fragments once both sides re-derive. */
        if (bytes.size() != MDKR_MATCH_PEER_ENVELOPE_BYTES) {
            peerLost(peer, MdkrMatchPeerLostReason::ControlChannelViolation);
            return;
        }
        if (!keysDerived || peer.openKey == nullptr) {
            counters.rejectedControlEnvelopes++;
            return;
        }
        MdkrMatchPeerEnvelopeContext context{};
        MdkrMatchPeerMeshEvent event;
        if (mdkr_match_peer_open(peer.openKey, &peer.openKey->direction,
                                 &peer.replayControl, bytes.data(), &context,
                                 event.payload.data()) !=
                MDKR_MATCH_PEER_CRYPTO_OK) {
            counters.rejectedControlEnvelopes++;
            return;
        }
        if (context.payload_type != MDKR_MATCH_PEER_PAYLOAD_PREFLIGHT_FRAGMENT) {
            /* Authenticated under the peer's own key with the wrong type:
             * proven misbehavior, not a race. */
            peerLost(peer, MdkrMatchPeerLostReason::ControlChannelViolation);
            return;
        }
        event.type = MdkrMatchPeerMeshEventType::PreflightFragment;
        event.endpointId = peer.endpointId;
        event.context = context;
        emit(std::move(event));
    }

    void controlText(PeerRuntime &peer, const std::string &text) {
        /* C1: every extraction is shape-checked BEFORE it is typed --
         * nlohmann's value() throws json::type_error on a key that exists
         * with a mismatched type ({"type":123}), and this runs inside
         * pump() on the launcher thread, where a roster peer must never be
         * able to raise anything. The outer catch is the same guard the
         * party transport gives its control parser; here any escape is
         * garbage on the reliable channel and terminal for the peer. */
        try {
            const Json value = Json::parse(text, nullptr, false);
            if (value.is_discarded() || !value.is_object() ||
                !value.contains("type") || !value["type"].is_string() ||
                !value.contains("protocol") ||
                !value["protocol"].is_number_unsigned() ||
                value["protocol"].get<uint64_t>() != kChannelProtocol ||
                !value.contains("nonce") ||
                !value["nonce"].is_number_unsigned() ||
                value["nonce"].get<uint64_t>() > UINT32_MAX) {
                peerLost(peer,
                         MdkrMatchPeerLostReason::ControlChannelViolation);
                return;
            }
            const std::string type = value["type"].get<std::string>();
            const uint32_t nonce =
                static_cast<uint32_t>(value["nonce"].get<uint64_t>());
            if (type == "ping") {
                if (peer.control && peer.control->isOpen()) {
                    try {
                        peer.control->send(Json{{"type", "pong"},
                            {"protocol", kChannelProtocol},
                            {"nonce", nonce}}.dump());
                    } catch (...) {
                        connectionDown(peer);
                    }
                }
                return;
            }
            if (type == "pong") {
                if (peer.pingOutstandingSinceMs != 0u &&
                    nonce == peer.pingNonce) {
                    peer.pingOutstandingSinceMs = 0u;
                    peer.nextPingAtMs =
                        now() + kMdkrMatchControlPingIntervalMs;
                }
                return;
            }
            peerLost(peer, MdkrMatchPeerLostReason::ControlChannelViolation);
        } catch (...) {
            peerLost(peer, MdkrMatchPeerLostReason::ControlChannelViolation);
        }
    }

    /* ---- Key schedule -----------------------------------------------------*/

    void deriveAll() {
        MdkrMatchPeerTranscript transcript{};
        std::memcpy(transcript.room_id, roomId, sizeof(roomId));
        transcript.match_epoch = matchEpoch;
        transcript.compatibility = compatibility;
        unsigned count = 0u;
        MdkrMatchPeerTranscriptEntry &self = transcript.entries[count++];
        self.endpoint_id = localEndpointId;
        self.generation = localGeneration;
        std::memcpy(self.public_key, ownPublicKey, sizeof(ownPublicKey));
        std::memcpy(self.commitment, ownCommitment, sizeof(ownCommitment));
        std::memcpy(self.commit_nonce, ownNonce, sizeof(ownNonce));
        for (const auto &entry : peers) {
            MdkrMatchPeerTranscriptEntry &record = transcript.entries[count++];
            record.endpoint_id = entry.second.endpointId;
            record.generation = entry.second.generation;
            std::memcpy(record.public_key, entry.second.publicKey,
                        sizeof(record.public_key));
            std::memcpy(record.commitment, entry.second.commitment,
                        sizeof(record.commitment));
            std::memcpy(record.commit_nonce, entry.second.commitNonce,
                        sizeof(record.commit_nonce));
        }
        transcript.entry_count = static_cast<uint8_t>(count);
        uint8_t digest[MDKR_MATCH_PEER_TRANSCRIPT_DIGEST_BYTES];
        if (!mdkr_match_peer_transcript_digest(&transcript, digest)) {
            failed = true;
            emitFailure(MdkrMatchPeerMeshFailure::KeyScheduleFailed);
            return;
        }
        mdkr_match_peer_keyring_forget(&keyring);
        for (auto &entry : peers) {
            PeerRuntime &peer = entry.second;
            MdkrMatchPeerKeyContext outbound{};
            outbound.match_epoch = matchEpoch;
            outbound.source_endpoint_id = localEndpointId;
            outbound.source_generation = localGeneration;
            outbound.destination_endpoint_id = peer.endpointId;
            outbound.destination_generation = peer.generation;
            MdkrMatchPeerKeyContext inbound{};
            inbound.match_epoch = matchEpoch;
            inbound.source_endpoint_id = peer.endpointId;
            inbound.source_generation = peer.generation;
            inbound.destination_endpoint_id = localEndpointId;
            inbound.destination_generation = localGeneration;
            peer.sealKey = mdkr_match_peer_identity_derive_key(
                &keyring, identity, peer.publicKey, digest, &outbound);
            peer.openKey = mdkr_match_peer_identity_derive_key(
                &keyring, identity, peer.publicKey, digest, &inbound);
            peer.replayState = MdkrMatchPeerReplayWindow{};
            peer.replayControl = MdkrMatchPeerReplayWindow{};
            if (peer.sealKey == nullptr || peer.openKey == nullptr) {
                /* An invalid revealed key surfaces here (derive validates
                 * the curve point): that is THIS peer's typed loss, not a
                 * mesh failure -- the commitment round passed over bytes
                 * ECDH cannot consume. The lost peer blocks completion, so
                 * no phrase can ever be produced from the broken roster,
                 * while the mesh itself stays alive and typed. */
                peerLost(peer, MdkrMatchPeerLostReason::HelloViolation);
                mdkr_match_peer_keyring_forget(&keyring);
                for (auto &reset : peers) {
                    reset.second.sealKey = nullptr;
                    reset.second.openKey = nullptr;
                }
                return;
            }
        }
        char phrase[MDKR_MATCH_PEER_PHRASE_BYTES] = {};
        if (!mdkr_match_peer_verification_phrase(digest, phrase)) {
            failed = true;
            emitFailure(MdkrMatchPeerMeshFailure::KeyScheduleFailed);
            return;
        }
        verificationPhrase = phrase;
        std::memcpy(transcriptDigestBytes, digest, sizeof(transcriptDigestBytes));
        keysDerived = true;
        MdkrMatchPeerMeshEvent event;
        event.type = MdkrMatchPeerMeshEventType::PhraseReady;
        emit(std::move(event));
    }

    /* ---- Tick: hello / key / offer / ping ladders -------------------------*/

    void sendHello(PeerRuntime &peer, const std::string &blob) {
        MdkrMatchSignalOutbound message;
        message.type = "peer_hello";
        message.publicKey = blob;
        if (sendSignal(peer, std::move(message))) peer.hellosSent++;
    }

    void tick() {
        if (!welcomed || failed) return;
        const uint64_t nowMs = now();
        for (auto &entry : peers) {
            PeerRuntime &peer = entry.second;
            if (peer.lost || !peer.present || peer.generation == 0u) continue;
            /* Hellos: commit eagerly; reveal only after the peer commits.
             * Each of the three sends is guarded independently, so a
             * refused send (relay said peer_unavailable mid-blip) is
             * retried on the next pump rather than dead-ending the
             * exchange -- hellosSent only advances on an accepted send. */
            if (ownCommitmentReady) {
                if (peer.hellosSent == 0u) {
                    sendHello(peer, encodeHelloBody(ownCommitment));
                }
                if (peer.hellosSent == 1u && peer.hellosReceived >= 1u) {
                    sendHello(peer, mdkr_party::base64Url(
                                        ownPublicKey, sizeof(ownPublicKey)));
                }
                if (peer.hellosSent == 2u && peer.hellosReceived >= 1u) {
                    sendHello(peer, encodeHelloBody(ownNonce));
                }
            }
            /* M4: the answerer's bounded setup verdict -- it has no offer
             * ladder, so a peer whose offer never arrives (or never
             * completes) must still resolve in bounded time. */
            if (!peer.offerer && !peer.channelsReady && !peer.gaveUp) {
                if (peer.setupStartedMs == 0u) {
                    peer.setupStartedMs = nowMs;
                } else if (nowMs - peer.setupStartedMs >=
                           kMdkrMatchAnswererSetupDeadlineMs) {
                    peer.gaveUp = true;
                    peerLost(peer, MdkrMatchPeerLostReason::ConnectTimeout);
                    continue;
                }
            }
            /* Offer ladder (glare-free: lower id offers, higher answers). */
            if (peer.offerer && !peer.gaveUp) {
                if (!peer.connection) {
                    createConnection(peer);
                } else if (!peer.channelsReady) {
                    /* W3 N4: the shared 3-attempt ladder on the MESH
                     * deadline -- the phones keep their 20 s default. */
                    const MdkrPartyRetryDecision decision =
                        mdkr_party_retry_decide(
                            nowMs, peer.offerSentMs, peer.offerAttempts,
                            /*authenticated=*/false,
                            /*protocolMismatched=*/false,
                            /*socketOpen=*/false,
                            kMdkrMatchOfferRetryDeadlineMs);
                    if (decision.giveUp) {
                        peer.gaveUp = true;
                        peerLost(peer, MdkrMatchPeerLostReason::ConnectTimeout);
                        continue;
                    }
                    if (decision.recreatePeer) {
                        const unsigned attempts = peer.offerAttempts;
                        silentTeardown(peer);
                        peer.offerAttempts = attempts;
                        createConnection(peer);
                    }
                }
            }
            /* Control ping ladder (5 s cadence, 15 s stale). */
            if (peer.channelsReady) {
                if (peer.pingOutstandingSinceMs != 0u &&
                    nowMs - peer.pingOutstandingSinceMs >=
                        kMdkrMatchControlPingTimeoutMs) {
                    peerLost(peer, MdkrMatchPeerLostReason::PingTimeout);
                    continue;
                }
                if (peer.pingOutstandingSinceMs == 0u &&
                    peer.nextPingAtMs != 0u && nowMs >= peer.nextPingAtMs &&
                    peer.control && peer.control->isOpen()) {
                    peer.pingNonce++;
                    peer.pingOutstandingSinceMs = nowMs;
                    peer.nextPingAtMs = nowMs + kMdkrMatchControlPingIntervalMs;
                    try {
                        peer.control->send(Json{{"type", "ping"},
                            {"protocol", kChannelProtocol},
                            {"nonce", peer.pingNonce}}.dump());
                    } catch (...) {
                        connectionDown(peer);
                        continue;
                    }
                }
            }
        }
        if (!keysDerived) {
            bool complete = true;
            for (const auto &entry : peers) {
                if (entry.second.lost || !entry.second.helloComplete) {
                    complete = false;
                    break;
                }
            }
            if (complete) deriveAll();
        }
    }

    /* ---- Pump -------------------------------------------------------------*/

    void pump() {
        if (closed) return;
        std::vector<MdkrMatchSignalEvent> feedEvents;
        feed->drainEvents(feedEvents);
        for (const MdkrMatchSignalEvent &event : feedEvents) {
            if (failed) break;
            handleFeedEvent(event);
        }
        std::deque<InternalEvent> pending;
        {
            std::lock_guard<std::mutex> lock(queueMutex);
            pending.swap(internalQueue);
        }
        for (const InternalEvent &event : pending) {
            handleInternal(event);
        }
        tick();
    }

    /* ---- Data plane ---------------------------------------------------------*/

    unsigned sendInput(const uint8_t bundle[MDKR_MATCH_PEER_PAYLOAD_BYTES]) {
        if (closed || failed || !keysDerived) return 0u;
        unsigned reached = 0u;
        for (auto &entry : peers) {
            PeerRuntime &peer = entry.second;
            if (peer.lost || !peer.channelsReady || peer.sealKey == nullptr ||
                !peer.state || !peer.state->isOpen()) {
                continue;
            }
            MdkrMatchPeerSendContext context{};
            context.key = peer.sealKey->direction;
            context.intermediate_endpoint_id = 0u; /* direct mesh only */
            context.payload_type = MDKR_MATCH_PEER_PAYLOAD_INPUT;
            uint8_t envelope[MDKR_MATCH_PEER_ENVELOPE_BYTES];
            if (!mdkr_match_peer_seal(peer.sealKey, &context, bundle,
                                      envelope)) {
                if (!peer.sealKey->window.ready) {
                    /* Nonce space spent: this direction requires a reconnect
                     * and a fresh generation-bound key. Typed and terminal. */
                    peerLost(peer,
                             MdkrMatchPeerLostReason::SealWindowExhausted);
                }
                continue;
            }
            try {
                if (peer.state->send(
                        reinterpret_cast<const std::byte *>(envelope),
                        sizeof(envelope))) {
                    reached++;
                }
            } catch (...) {
                /* The channel-down event will drive recovery. */
            }
        }
        return reached;
    }

    bool sendPreflightFragment(
        uint64_t peerEndpointId,
        const uint8_t fragment[MDKR_MATCH_PEER_PAYLOAD_BYTES]) {
        if (closed || failed || !keysDerived) return false;
        const auto found = peers.find(peerEndpointId);
        if (found == peers.end()) return false;
        PeerRuntime &peer = found->second;
        if (peer.lost || !peer.channelsReady || peer.sealKey == nullptr ||
            !peer.control || !peer.control->isOpen()) {
            return false;
        }
        MdkrMatchPeerSendContext context{};
        context.key = peer.sealKey->direction;
        context.intermediate_endpoint_id = 0u;
        context.payload_type = MDKR_MATCH_PEER_PAYLOAD_PREFLIGHT_FRAGMENT;
        uint8_t envelope[MDKR_MATCH_PEER_ENVELOPE_BYTES];
        if (!mdkr_match_peer_seal(peer.sealKey, &context, fragment, envelope)) {
            if (!peer.sealKey->window.ready) {
                peerLost(peer, MdkrMatchPeerLostReason::SealWindowExhausted);
            }
            return false;
        }
        try {
            return peer.control->send(
                reinterpret_cast<const std::byte *>(envelope),
                sizeof(envelope));
        } catch (...) {
            return false;
        }
    }

    /* ---- Teardown ------------------------------------------------------------*/

    void teardown() {
        if (closed) return;
        closed = true;
        std::vector<std::shared_ptr<rtc::PeerConnection>> connections;
        for (auto &entry : peers) {
            PeerRuntime &peer = entry.second;
            peer.attempt++; /* orphan every callback first */
            if (peer.connection) {
                connections.push_back(std::move(peer.connection));
            }
            peer.connection.reset();
            peer.state.reset();
            peer.control.reset();
            peer.sealKey = nullptr;
            peer.openKey = nullptr;
        }
        for (const auto &connection : connections) {
            try { connection->close(); } catch (...) {}
        }
        mdkr_match_peer_keyring_forget(&keyring);
        if (identity != nullptr) {
            mdkr_match_peer_identity_destroy(identity);
            identity = nullptr;
        }
        std::memset(ownNonce, 0, sizeof(ownNonce));
        verificationPhrase.clear();
        keysDerived = false;
        {
            std::lock_guard<std::mutex> lock(queueMutex);
            internalQueue.clear();
        }
        events.clear();
    }
};

/* ---- Public surface -------------------------------------------------------*/

MdkrMatchPeerMesh::MdkrMatchPeerMesh(std::shared_ptr<State> state)
    : state_(std::move(state)) {}

MdkrMatchPeerMesh::~MdkrMatchPeerMesh() { close(); }

std::unique_ptr<MdkrMatchPeerMesh> MdkrMatchPeerMesh::create(
    const MdkrMatchPeerMeshOptions &options, std::string *errorMessage) {
    const auto refuse = [errorMessage](const char *reason) {
        if (errorMessage != nullptr) *errorMessage = reason;
        return std::unique_ptr<MdkrMatchPeerMesh>();
    };
    if (options.signal == nullptr) return refuse("missing signal feed");
    if (options.localEndpointId == 0u) return refuse("invalid endpoint id");
    if (options.matchEpoch == 0u) return refuse("invalid match epoch");
    if (options.roster.size() < 2u ||
        options.roster.size() > MDKR_MATCH_PEER_GRAPH_MAX_ENDPOINTS) {
        return refuse("invalid roster size");
    }
    bool sawSelf = false;
    for (size_t index = 0u; index < options.roster.size(); index++) {
        const MdkrMatchPeerSlotOwner &owner = options.roster[index];
        if (owner.endpointId == 0u) return refuse("invalid roster endpoint");
        for (size_t other = index + 1u; other < options.roster.size();
             other++) {
            if (options.roster[other].endpointId == owner.endpointId) {
                return refuse("duplicate roster endpoint");
            }
        }
        if (owner.endpointId == options.localEndpointId) sawSelf = true;
    }
    if (!sawSelf) return refuse("roster missing local endpoint");

    auto state = std::make_shared<State>();
    if (!decodeRoomId(options.roomId, state->roomId)) {
        return refuse("invalid room id");
    }
    state->feed = options.signal;
    state->localEndpointId = options.localEndpointId;
    state->expectedLocalGeneration = options.localGeneration;
    state->matchEpoch = options.matchEpoch;
    state->compatibility = options.compatibility;
    state->iceServers = options.iceServers;
    state->clock = options.nowMs;
    state->identity = mdkr_match_peer_identity_create();
    if (state->identity == nullptr ||
        !mdkr_match_peer_identity_public_key(state->identity,
                                             state->ownPublicKey) ||
        !secureRandom(state->ownNonce, sizeof(state->ownNonce))) {
        return refuse("identity material unavailable");
    }
    for (const MdkrMatchPeerSlotOwner &owner : options.roster) {
        if (owner.endpointId == options.localEndpointId) continue;
        PeerRuntime peer;
        peer.endpointId = owner.endpointId;
        peer.slotMask = owner.slotMask;
        /* Glare-free by construction: the lower id offers. */
        peer.offerer = options.localEndpointId < owner.endpointId;
        state->peers.emplace(owner.endpointId, std::move(peer));
    }
    return std::unique_ptr<MdkrMatchPeerMesh>(
        new MdkrMatchPeerMesh(std::move(state)));
}

void MdkrMatchPeerMesh::pump() { state_->pump(); }

void MdkrMatchPeerMesh::drainEvents(std::vector<MdkrMatchPeerMeshEvent> &out) {
    out.clear();
    out.reserve(state_->events.size());
    for (MdkrMatchPeerMeshEvent &event : state_->events) {
        out.push_back(std::move(event));
    }
    state_->events.clear();
}

unsigned MdkrMatchPeerMesh::sendInput(
    const uint8_t bundle[MDKR_MATCH_PEER_PAYLOAD_BYTES]) {
    return state_->sendInput(bundle);
}

bool MdkrMatchPeerMesh::sendPreflightFragment(
    uint64_t peerEndpointId,
    const uint8_t fragment[MDKR_MATCH_PEER_PAYLOAD_BYTES]) {
    return state_->sendPreflightFragment(peerEndpointId, fragment);
}

bool MdkrMatchPeerMesh::phrase(std::string &out) const {
    if (state_->closed || state_->failed || !state_->keysDerived) return false;
    out = state_->verificationPhrase;
    return true;
}

bool MdkrMatchPeerMesh::transcriptDigest(
    uint8_t out[MDKR_MATCH_PEER_TRANSCRIPT_DIGEST_BYTES]) const {
    if (out == nullptr || state_->closed || state_->failed ||
        !state_->keysDerived) {
        return false;
    }
    std::memcpy(out, state_->transcriptDigestBytes,
                MDKR_MATCH_PEER_TRANSCRIPT_DIGEST_BYTES);
    return true;
}

uint32_t MdkrMatchPeerMesh::connectionGeneration() const {
    return state_ ? state_->localGeneration : 0u;
}

bool MdkrMatchPeerMesh::peerGeneration(uint64_t peerEndpointId,
                                       uint32_t *out) const {
    if (out == nullptr || !state_) return false;
    const auto found = state_->peers.find(peerEndpointId);
    if (found == state_->peers.end() || found->second.generation == 0u) {
        return false;
    }
    *out = found->second.generation;
    return true;
}

bool MdkrMatchPeerMesh::peerChannelsReady(uint64_t peerEndpointId) const {
    if (!state_) return false;
    const auto found = state_->peers.find(peerEndpointId);
    return found != state_->peers.end() && !found->second.lost &&
           found->second.channelsReady;
}

MdkrMatchPeerMeshStats MdkrMatchPeerMesh::stats() const {
    MdkrMatchPeerMeshStats stats = state_->counters;
    std::lock_guard<std::mutex> lock(state_->queueMutex);
    stats.droppedInternalEvents = state_->droppedInternal;
    return stats;
}

void MdkrMatchPeerMesh::close() {
    if (state_) state_->teardown();
}

/* ---- Test seams -------------------------------------------------------------*/

bool mdkr_match_peer_mesh_exhaust_seal_for_test(MdkrMatchPeerMesh &mesh,
                                                uint64_t peerEndpointId) {
    const auto found = mesh.state_->peers.find(peerEndpointId);
    if (found == mesh.state_->peers.end() ||
        found->second.sealKey == nullptr) {
        return false;
    }
    /* Exactly the state one UINT64_MAX seal leaves behind
     * (test_match_peer_crypto.cpp pins it): not ready, sequence zeroed. */
    found->second.sealKey->window.ready = false;
    found->second.sealKey->window.next_sequence = 0u;
    return true;
}

bool mdkr_match_peer_mesh_kill_channels_for_test(MdkrMatchPeerMesh &mesh,
                                                 uint64_t peerEndpointId) {
    const auto found = mesh.state_->peers.find(peerEndpointId);
    if (found == mesh.state_->peers.end() || !found->second.connection) {
        return false;
    }
    try { found->second.connection->close(); } catch (...) {}
    return true;
}
