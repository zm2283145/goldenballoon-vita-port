#include "online/match_peer_transport.h"

#include "net/match_peer_transcript.h"
#include "net/net_failure_ring.h"
#include "party/party_retry_policy.h"
#include "party/party_webrtc_signaling.h"

#include <nlohmann/json.hpp>
#include <rtc/rtc.hpp>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <utility>
#include <variant>

// Always-on (beta) [MESH] diagnostics for the WebRTC bring-up: ICE connection
// state, STATE/control DataChannel open, channelsReady and peer-lost reasons.
// The classic real-two-machine stall is the unordered STATE channel never
// opening across NATs, so channelsReady never reaches the roster and the race
// never boots -- these lines make that visible in a stderr capture. Compiled out
// of any non-beta build of this TU (e.g. the standalone transport test).
#if MDKR_ENABLE_ONLINE_BETA
#define MDKR_MESH_LOG(...) std::fprintf(stderr, __VA_ARGS__)
#else
#define MDKR_MESH_LOG(...) ((void)0)
#endif

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
 * touching the crypto layers. Out of scope by design.
 */

namespace {

using Json = nlohmann::json;

constexpr size_t kMaxInternalEvents = 512u;
constexpr size_t kMaxPublicEvents = 256u;
constexpr size_t kMaxControlTextBytes = 4096u;
constexpr size_t kMaxSdpBytes = 60u * 1024u;
constexpr size_t kMaxCandidateBytes = 4096u;
/* Channel-message protocol version for the plaintext control-channel messages
 * (the party discipline). v2 adds race_drop; the set does not negotiate, so a
 * v1 peer's ping is refused rather than half-understood. */
constexpr unsigned kChannelProtocol = 2u;
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

/* The three channel labels, in lane order, so a lane indexes its own label
 * and nothing has to keep two orderings in step. */
const char *const kMdkrMatchChannelLabels[MDKR_MATCH_PEER_LANE_COUNT] = {
    kMdkrMatchStateChannelLabel, kMdkrMatchControlChannelLabel,
    kMdkrMatchAuthorityChannelLabel};

bool laneForLabel(const std::string &label, uint8_t &lane) {
    for (unsigned index = 0u; index < MDKR_MATCH_PEER_LANE_COUNT; ++index) {
        if (label == kMdkrMatchChannelLabels[index]) {
            lane = static_cast<uint8_t>(index);
            return true;
        }
    }
    return false;
}

struct InternalEvent {
    enum class Kind {
        LocalDescription,
        LocalCandidate,
        ConnectionDown,
        ChannelAdopted,
        ChannelOpen,
        ChannelBinary,
        ChannelText,
    };
    Kind kind = Kind::ConnectionDown;
    uint64_t peer = 0u;
    uint32_t attempt = 0u;
    /* Which of the peer connection's channels the event came from. */
    uint8_t lane = MDKR_MATCH_PEER_LANE_STATE;
    std::string text; /* sdp / candidate / control text */
    std::string extra; /* description type / candidate mid */
    std::vector<uint8_t> bytes;
    std::shared_ptr<rtc::DataChannel> channel;
};

struct PeerRuntime {
    uint64_t endpointId = 0u;
    uint8_t slotMask = 0u;
    /* Fixed position among the remote roster peers; the forensics ring indexes
     * its per-peer stall snapshot by it. */
    unsigned rosterIndex = 0u;
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
    /* Indexed by MDKR_MATCH_PEER_LANE_*. */
    std::shared_ptr<rtc::DataChannel> channels[MDKR_MATCH_PEER_LANE_COUNT];
    bool channelOpen[MDKR_MATCH_PEER_LANE_COUNT] = {};
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

    /* Link health for the forensics ring. One RTT sample per answered ping;
     * jitter is the RFC 3550 smoothing of its absolute change, so a single
     * late pong cannot dominate the figure. Bytes cover both channels. */
    uint32_t rttMs = 0u;
    uint32_t jitterMs = 0u;
    bool haveRtt = false;
    uint64_t bytesSent = 0u;
    uint64_t bytesReceived = 0u;

    /* Envelopes from this peer that OPENED under its own derived lane key --
     * an input bundle, a preflight fragment or an input repair. Monotonic
     * across the mesh's lifetime and never reset, so a caller can compare two
     * readings and know whether anything authenticated arrived between them
     * (D1's peer-silence grace does exactly that). Bytes and rejected
     * counters cannot answer that question: they also move for garbage, and
     * garbage is not evidence that the peer is still racing. */
    uint64_t authenticatedPackets = 0u;

    /* Vanish dwell (0 = disarmed): first tick at which this peer was BOTH
     * absent from signaling AND without ready channels. Armed/disarmed by
     * tick() from those two live facts each pump, so a presence re-assert or
     * a channel recovery disarms it; expiry is PeerVanished. Deliberately
     * NOT cleared by silentTeardown -- a teardown while absent is exactly
     * the state the dwell bounds. */
    uint64_t vanishedSinceMs = 0u;

    /* Derived directional keys (slots in the mesh keyring) + replay, one set
     * per lane. Each channel owns a separate key and therefore a separate
     * sequence space, so a reliable fragment delayed behind a burst of lossy
     * state envelopes cannot be retired as a REPLAY of them, and one
     * channel's traffic can never advance another channel's nonce. */
    MdkrMatchPeerSealingKey *sealKeys[MDKR_MATCH_PEER_LANE_COUNT] = {};
    MdkrMatchPeerSealingKey *openKeys[MDKR_MATCH_PEER_LANE_COUNT] = {};
    MdkrMatchPeerReplayWindow replay[MDKR_MATCH_PEER_LANE_COUNT] = {};
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
    /* F3: a peer sent a race_abort on the reliable control channel. Consumed
     * (read-and-cleared) by the launcher through consumeRaceAbort(), so a fresh
     * abort in a later race is observed independently. */
    bool raceAbortReceived = false;
    /* A3: finalisation ticks peers proposed, one per departed endpoint, held
     * the same read-and-clear way as the abort latch. Keyed by endpoint rather
     * than held one at a time so a second departure's proposal cannot be lost
     * behind an unconsumed first; first proposal for an endpoint wins. The
     * SENDER rides along because only the launcher can tell whether that
     * endpoint was entitled to propose, and the epoch because a proposal is
     * about one race and must not be applied to the next one. */
    struct RaceDropProposal {
        uint64_t senderEndpointId = 0u;
        uint32_t matchEpoch = 0u;
        uint32_t tick = 0u;
    };
    std::map<uint64_t, RaceDropProposal> raceDrops;
    std::deque<MdkrMatchPeerMeshEvent> events;
    MdkrMatchPeerMeshStats counters;

    /* ---- Callback -> pump bounded queue --------------------------------- */
    std::mutex queueMutex;
    std::deque<InternalEvent> internalQueue;
    uint64_t droppedInternal = 0u;
    /* Edge detection for the ring: one record per crossing, not per pump. */
    bool queuePressured = false;
    uint64_t queueDroppedSeen = 0u;

    ~State() { teardown(/*announcePeerEnd=*/false); }

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
        /* The feed hands its failure code through verbatim and it can name a
         * relay host, so it reaches the ring through the redaction filter. */
        mdkr_net_failure_ring_record_host(
            MDKR_NET_FAILURE_SESSION_FAILURE, static_cast<uint32_t>(now()),
            MDKR_NET_FAILURE_NO_SLOT, static_cast<unsigned>(failure),
            event.failureCode.c_str());
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
        for (unsigned lane = 0u; lane < MDKR_MATCH_PEER_LANE_COUNT; ++lane) {
            peer.channels[lane].reset();
            peer.channelOpen[lane] = false;
        }
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

    /* One RTT sample from an answered ping. Jitter follows RFC 3550's
     * smoothing (a sixteenth of each deviation), so one late pong moves the
     * figure without owning it. */
    static void noteRoundTrip(PeerRuntime &peer, uint64_t elapsedMs) {
        const uint32_t sample = elapsedMs > UINT32_MAX
                                    ? UINT32_MAX
                                    : static_cast<uint32_t>(elapsedMs);
        if (peer.haveRtt) {
            const uint32_t deviation = sample > peer.rttMs
                                           ? sample - peer.rttMs
                                           : peer.rttMs - sample;
            peer.jitterMs += (deviation - peer.jitterMs) / 16u;
        }
        peer.rttMs = sample;
        peer.haveRtt = true;
    }

    /* `announce` false retires the peer silently: the ring record, the typed
     * log line and the teardown all still happen, but no PeerLost event is
     * queued. Used when the LAUNCHER asked for the retirement and already
     * knows -- telling it back would only cost a pump. */
    void peerLost(PeerRuntime &peer, MdkrMatchPeerLostReason reason,
                  bool announce = true) {
        if (peer.lost) return;
        MDKR_MESH_LOG(
            "[MESH] peer LOST ep=%llu reason=%d channelsReady=%u offerer=%u\n",
            (unsigned long long)peer.endpointId, static_cast<int>(reason),
            peer.channelsReady ? 1u : 0u, peer.offerer ? 1u : 0u);
        mdkr_net_failure_ring_record_host(
            MDKR_NET_FAILURE_PEER_LOST, static_cast<uint32_t>(now()),
            peer.rosterIndex, static_cast<unsigned>(reason),
            mdkr_match_peer_lost_reason_name(reason));
        peer.lost = true;
        silentTeardown(peer);
        if (!announce) return;
        MdkrMatchPeerMeshEvent event;
        event.type = MdkrMatchPeerMeshEventType::PeerLost;
        event.endpointId = peer.endpointId;
        event.lostReason = reason;
        emit(std::move(event));
    }

    /* Draw a fresh local commit nonce and recompute our round-1 commitment
     * for the current generation (the handleReWelcome discipline). Called on
     * EVERY transcript-retiring rekey so our contribution to the next
     * transcript carries fresh, unrevealed entropy. */
    void refreshOwnCommitment() {
        if (!secureRandom(ownNonce, sizeof(ownNonce)) ||
            !mdkr_match_peer_commitment(matchEpoch, localEndpointId,
                                        localGeneration, ownNonce,
                                        ownPublicKey, ownCommitment)) {
            ownCommitmentReady = false;
            failed = true;
            emitFailure(MdkrMatchPeerMeshFailure::KeyScheduleFailed);
            return;
        }
        ownCommitmentReady = true;
    }

    /* A generation change is a replaced peer: retire keys AND channels,
     * restart the hello exchange, compare a new phrase later.
     *
     * W3 CRITICAL-2 -- local entropy on a rekey. A rekey retires the whole
     * transcript, and our contribution to the NEXT one must be fresh: reusing
     * our already-revealed nonce leaves it fully known to the relay, which
     * could then offline-grind a colliding phrase past a human. We refresh
     * our nonce only when it propagates CONSISTENTLY to every endpoint, i.e.
     * when we re-exchange hellos with everyone that would carry our new
     * contribution:
     *   - OUR OWN reconnect (handleReWelcome): our generation bumped, so the
     *     relay announces us to EVERY peer, each resets its us-lane, and we
     *     re-exchange with all of them. handleReWelcome refreshes there and
     *     resets every lane, so this holds for any mesh size.
     *   - a PEER's reconnect (applyPresence -> here) in a TWO-endpoint mesh:
     *     the bumped peer is our only peer and it reset its us-lane when it
     *     re-welcomed, so re-exchanging just that lane propagates our fresh
     *     contribution to the whole (2-node) transcript. Refresh here.
     * In a 3-4P mesh a single peer's bump does NOT change our generation, so
     * bystanders never reset their us-lane and cannot accept a re-hello
     * (RFC-style hello-sequence restart = HelloViolation), and our refreshed
     * self-entry could not reach them without diverging the digest. So we
     * keep our nonce on a peer-bump there and rely on the reconnecting peer's
     * own fresh entropy plus the adapter's digest-keyed re-verify barrier;
     * full per-peer-bump grind resistance for 3-4P needs the post-flip
     * full-mesh-rekey protocol and is tracked with that work. 2P -- the
     * shipping config -- is fully closed. */
    void rekeyPeer(PeerRuntime &peer, uint32_t generation) {
        const bool retireTranscript = keysDerived;
        const bool soleOtherPeer = peers.size() == 1u;
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
        peer.vanishedSinceMs = 0u; /* a reconnected peer starts a fresh dwell */
        silentTeardown(peer);
        if (retireTranscript) {
            /* Every pairwise key was salted with the old transcript digest;
             * all of them retire together and re-derive when the exchange
             * completes again. */
            keysDerived = false;
            verificationPhrase.clear();
            mdkr_match_peer_keyring_forget(&keyring);
            if (soleOtherPeer) refreshOwnCommitment(); /* fresh local entropy */
            for (auto &entry : peers) {
                forgetLaneKeys(entry.second);
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
                MDKR_MESH_LOG(
                    "[MESH] ice/connection state ep=%llu attempt=%u state=%d\n",
                    (unsigned long long)endpointId, attempt,
                    static_cast<int>(value));
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
                    uint8_t lane = MDKR_MATCH_PEER_LANE_STATE;
                    if (!laneForLabel(channel->label(), lane)) {
                        return; /* unknown labels are ignored */
                    }
                    state->attachChannelCallbacks(channel, endpointId, attempt,
                                                  lane);
                    InternalEvent event;
                    event.kind = InternalEvent::Kind::ChannelAdopted;
                    event.peer = endpointId;
                    event.attempt = attempt;
                    event.lane = lane;
                    event.channel = std::move(channel);
                    state->enqueueInternal(std::move(event));
                }
            });
    }

    void attachChannelCallbacks(const std::shared_ptr<rtc::DataChannel> &channel,
                                uint64_t endpointId, uint32_t attempt,
                                uint8_t lane) {
        const std::weak_ptr<State> weak = weak_from_this();
        channel->onOpen([weak, endpointId, attempt, lane]() {
            if (auto state = weak.lock()) {
                InternalEvent event;
                event.kind = InternalEvent::Kind::ChannelOpen;
                event.peer = endpointId;
                event.attempt = attempt;
                event.lane = lane;
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
            [weak, endpointId, attempt, lane](rtc::message_variant message) {
                auto state = weak.lock();
                if (!state) return;
                InternalEvent event;
                event.peer = endpointId;
                event.attempt = attempt;
                event.lane = lane;
                if (std::holds_alternative<rtc::binary>(message)) {
                    const rtc::binary &raw = std::get<rtc::binary>(message);
                    if (raw.size() > kMaxPeerMessageBytes) return;
                    event.kind = InternalEvent::Kind::ChannelBinary;
                    event.bytes.resize(raw.size());
                    if (!raw.empty()) {
                        std::memcpy(event.bytes.data(), raw.data(), raw.size());
                    }
                } else {
                    const std::string &text = std::get<std::string>(message);
                    if (lane == MDKR_MATCH_PEER_LANE_STATE) {
                        /* Text on the lossy state channel is just another
                         * bad datagram: counted and dropped in stateData. */
                        event.kind = InternalEvent::Kind::ChannelBinary;
                        event.bytes.assign(text.begin(), text.end());
                    } else {
                        /* Oversize control text is garbage on the reliable
                         * channel; the empty-text event fails JSON parsing
                         * in controlText and retires the peer, terminal.
                         * Text on the authority channel is structural
                         * garbage outright -- see the pump's dispatch. */
                        event.kind = InternalEvent::Kind::ChannelText;
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
        MDKR_MESH_LOG(
            "[MESH] creating peer connection ep=%llu attempt=%u offerAttempts=%u "
            "(offerer, creating STATE+control channels)\n",
            (unsigned long long)peer.endpointId, peer.attempt,
            peer.offerAttempts);
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
        /* Retransmitted until delivered, but never head-of-line blocked: a
         * repair answer is useful the moment it lands, and holding it behind
         * an older message would spend the very ticks it exists to save. */
        rtc::DataChannelInit authorityConfiguration;
        authorityConfiguration.reliability.unordered = true;
        try {
            peer.channels[MDKR_MATCH_PEER_LANE_STATE] =
                peer.connection->createDataChannel(
                    kMdkrMatchStateChannelLabel, stateConfiguration);
            peer.channels[MDKR_MATCH_PEER_LANE_CONTROL] =
                peer.connection->createDataChannel(
                    kMdkrMatchControlChannelLabel);
            peer.channels[MDKR_MATCH_PEER_LANE_AUTHORITY] =
                peer.connection->createDataChannel(
                    kMdkrMatchAuthorityChannelLabel, authorityConfiguration);
            for (unsigned lane = 0u; lane < MDKR_MATCH_PEER_LANE_COUNT;
                 ++lane) {
                attachChannelCallbacks(peer.channels[lane], peer.endpointId,
                                       peer.attempt,
                                       static_cast<uint8_t>(lane));
            }
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
        if (found == peers.end() || found->second.generation == 0u) {
            counters.ignoredStaleSignals++;
            return nullptr;
        }
        /* A STRICTLY NEWER sender generation is the service's own attestation
         * that the peer's replacement socket is live: from/generation on every
         * delivered signal event are stamped by the relay from the
         * authenticated sending socket (match-room injects them; a client
         * cannot claim either), and generations are service-assigned,
         * strictly increasing. Adopt it exactly like the presence bump it
         * proves happened -- because that bump's broadcast is MISSABLE: when
         * BOTH endpoints blip near-simultaneously, the peer's presence bump
         * goes out while OUR socket is down, our own re-welcome then lists
         * the still-reconnecting peer as absent, and every message the peer
         * re-drives at us arrives under its new generation only to be dropped
         * here as stale -- the vanish dwell (or, slower, the setup ladders)
         * then declares a RETURNING peer gone. Not for a peer already
         * declared lost: past the dwell's expiry the verdict stands and only
         * the real presence bump re-admits, as before. */
        if (fromGeneration > found->second.generation && !found->second.lost) {
            rekeyPeer(found->second, fromGeneration);
            found->second.present = true;
            return &found->second;
        }
        if (found->second.generation != fromGeneration) {
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
            /* Not for a peer already declared lost -- the same rule rosterPeer
             * states at its own rekey: past the dwell's expiry the verdict
             * stands, and only a real presence bump re-admits. rekeyPeer()
             * clears `lost`, so calling it here resurrected every finalised
             * departure -- PeerEnded, PeerVanished, and the PeerDeparted the A3
             * drop path sets -- on nothing more than OUR socket being replaced,
             * without emitting a new verdict. The launcher had already stopped
             * consuming that endpoint while the mesh quietly resumed hellos,
             * key derivation and input fan-out to it. It also reset
             * restartEpisodes, which is the budget handlePeerEnd now spends. */
            if (peer.lost) continue;
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
            /* Edge-triggered: the room reports absence repeatedly while a
             * member stays gone, and only the crossing is a departure. A
             * generation the relay has already superseded says nothing about
             * the peer this mesh is talking to. */
            if (peer.generation != generation || !peer.present) return;
            peer.present = false;
            MdkrMatchPeerMeshEvent event;
            event.type = MdkrMatchPeerMeshEventType::PeerDeparted;
            event.endpointId = peer.endpointId;
            emit(std::move(event));
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
        /*
         * "restart": retire this generation's connection quietly; the offerer
         * rebuilds, the answerer awaits the fresh offer.
         *
         * Bounded by the same budget connectionDown() spends, and for the same
         * reason. silentTeardown() disarms every ladder that could later
         * produce a verdict -- it clears channelsReady, so the control-ping
         * ladder stops; it zeroes setupStartedMs, so the answerer deadline
         * re-arms from scratch; and the vanish dwell needs !channelsReady while
         * the signal socket is still up. Unbounded, a peer sending one of these
         * every few seconds kept its own loss detection permanently disarmed,
         * and mid-race the teardown stripped a live connection so sealAndSend()
         * failed forever -- input delivery to that peer stopped silently while
         * the mesh reported everything healthy. On the offerer path it also
         * reset offerAttempts and gaveUp, making the retry ladder's give-up
         * unreachable and buying a full PeerConnection rebuild each time.
         *
         * A remote-driven restart is still a restart episode, so it is charged
         * to the same counter a local one is.
         */
        if (peer->restartEpisodes >= kMdkrMatchMaxRestartEpisodes) {
            peerLost(*peer, MdkrMatchPeerLostReason::ConnectTimeout);
            return;
        }
        peer->restartEpisodes++;
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
            peer.channels[event.lane] = event.channel;
            if (event.channel && event.channel->isOpen()) {
                channelOpened(peer, event.lane);
            }
            break;
        case InternalEvent::Kind::ChannelOpen:
            channelOpened(peer, event.lane);
            break;
        case InternalEvent::Kind::ChannelBinary:
            if (event.lane == MDKR_MATCH_PEER_LANE_STATE) {
                stateData(peer, event.bytes);
            } else {
                reliableBinary(peer, event.lane, event.bytes);
            }
            break;
        case InternalEvent::Kind::ChannelText:
            /* Only the control channel speaks text (the ping ladder). Text on
             * the authority channel is structural garbage on a reliable
             * channel, exactly like a wrong-size frame, and terminal. */
            if (event.lane == MDKR_MATCH_PEER_LANE_CONTROL) {
                controlText(peer, event.text);
            } else {
                peerLost(peer,
                         MdkrMatchPeerLostReason::ControlChannelViolation);
            }
            break;
        }
    }

    void channelOpened(PeerRuntime &peer, uint8_t lane) {
        peer.channelOpen[lane] = true;
        MDKR_MESH_LOG(
            "[MESH] DataChannel open ep=%llu channel=%s open=%u/%u\n",
            (unsigned long long)peer.endpointId, kMdkrMatchChannelLabels[lane],
            openChannelCount(peer), MDKR_MATCH_PEER_LANE_COUNT);
        if (!peer.channelsReady && openChannelCount(peer) ==
                                       MDKR_MATCH_PEER_LANE_COUNT) {
            peer.channelsReady = true;
            peer.pingOutstandingSinceMs = 0u;
            peer.nextPingAtMs = now() + kMdkrMatchControlPingIntervalMs;
            MDKR_MESH_LOG(
                "[MESH] channels ready ep=%llu (all three channels open)\n",
                (unsigned long long)peer.endpointId);
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
        if (!peer.present) {
            /* An ABSENT peer can receive neither the peer_end "restart" nor
             * a fresh offer, so a restart episode here is pure waste (it
             * burns one of the three bounded episodes against the void).
             * Just retire the dead connection; recovery is either the
             * peer's presence returning (the offer ladder / a fresh offer
             * re-drive from a clean slate) or the vanish dwell resolving
             * the loss typed and bounded. */
            silentTeardown(peer);
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
        peer.bytesReceived += bytes.size();
        /* Lossy by design: any rejection here is a counted drop, never
         * terminal -- a single bad datagram cannot end a race.
         *
         * FORWARDING SEAM: a one-hop route would first
         * mdkr_match_peer_inspect() the envelope and, when the header names
         * this endpoint as intermediate, run mdkr_match_peer_forwarder_admit
         * against the current graph route and relay the unchanged bytes. */
        MdkrMatchPeerSealingKey *opening =
            peer.openKeys[MDKR_MATCH_PEER_LANE_STATE];
        if (bytes.size() != MDKR_MATCH_PEER_ENVELOPE_BYTES || !keysDerived ||
            opening == nullptr) {
            counters.rejectedStateEnvelopes++;
            return;
        }
        MdkrMatchPeerEnvelopeContext context{};
        MdkrMatchPeerMeshEvent event;
        if (mdkr_match_peer_open(
                opening, &opening->direction,
                &peer.replay[MDKR_MATCH_PEER_LANE_STATE], bytes.data(),
                &context, event.payload.data()) != MDKR_MATCH_PEER_CRYPTO_OK ||
            context.payload_type != MDKR_MATCH_PEER_PAYLOAD_INPUT) {
            counters.rejectedStateEnvelopes++;
            return;
        }
        peer.authenticatedPackets++;
        event.type = MdkrMatchPeerMeshEventType::InputEnvelope;
        event.endpointId = peer.endpointId;
        event.context = context;
        emit(std::move(event));
    }

    /* Both RELIABLE channels: control (preflight fragments) and authority
     * (input repair). Neither ever delivers STRUCTURAL garbage -- a
     * conforming peer only ever sends 132-byte envelopes here -- so a
     * wrong-size frame, or an envelope authenticated under the peer's own
     * key carrying a payload type the channel does not serve, is terminal.
     * An envelope that merely fails to OPEN is not: any roster peer's
     * generation bump retires every transcript-salted key mesh-wide, and an
     * honest third peer's in-flight message sealed under the old digest --
     * or one that lands before this side finished (re-)deriving --
     * authenticates as garbage while being nothing of the sort. Those are
     * counted drops; each layer's own retry discipline re-carries the
     * message once both sides re-derive. */
    void reliableBinary(PeerRuntime &peer, uint8_t lane,
                        const std::vector<uint8_t> &bytes) {
        const bool authority = lane == MDKR_MATCH_PEER_LANE_AUTHORITY;
        uint64_t &rejected = authority ? counters.rejectedAuthorityEnvelopes
                                       : counters.rejectedControlEnvelopes;
        peer.bytesReceived += bytes.size();
        if (bytes.size() != MDKR_MATCH_PEER_ENVELOPE_BYTES) {
            peerLost(peer, MdkrMatchPeerLostReason::ControlChannelViolation);
            return;
        }
        MdkrMatchPeerSealingKey *opening = peer.openKeys[lane];
        if (!keysDerived || opening == nullptr) {
            rejected++;
            return;
        }
        MdkrMatchPeerEnvelopeContext context{};
        MdkrMatchPeerMeshEvent event;
        if (mdkr_match_peer_open(opening, &opening->direction,
                                 &peer.replay[lane], bytes.data(), &context,
                                 event.payload.data()) !=
                MDKR_MATCH_PEER_CRYPTO_OK) {
            rejected++;
            return;
        }
        const bool typed = authority
            ? (context.payload_type ==
                   MDKR_MATCH_PEER_PAYLOAD_INPUT_REPAIR_REQUEST ||
               context.payload_type ==
                   MDKR_MATCH_PEER_PAYLOAD_INPUT_REPAIR_ANSWER)
            : context.payload_type == MDKR_MATCH_PEER_PAYLOAD_PREFLIGHT_FRAGMENT;
        if (!typed) {
            /* Authenticated under the peer's own key with the wrong type:
             * proven misbehavior, not a race. */
            peerLost(peer, MdkrMatchPeerLostReason::ControlChannelViolation);
            return;
        }
        peer.authenticatedPackets++;
        event.type = authority
            ? MdkrMatchPeerMeshEventType::InputRepairMessage
            : MdkrMatchPeerMeshEventType::PreflightFragment;
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
                const std::shared_ptr<rtc::DataChannel> &controlChannel =
                    peer.channels[MDKR_MATCH_PEER_LANE_CONTROL];
                if (controlChannel && controlChannel->isOpen()) {
                    try {
                        controlChannel->send(Json{{"type", "pong"},
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
                    noteRoundTrip(peer, now() - peer.pingOutstandingSinceMs);
                    peer.pingOutstandingSinceMs = 0u;
                    peer.nextPingAtMs =
                        now() + kMdkrMatchControlPingIntervalMs;
                }
                return;
            }
            if (type == "race_drop") {
                /* A3: the proposer names the departed endpoint, the race it is
                 * talking about, and the tick every survivor finalises that
                 * endpoint's seats at. The roster is fixed for the room, so a
                 * name outside it -- the recipient's own, or the SENDER's own,
                 * neither of which can have departed if it is talking -- is
                 * garbage on the reliable channel, like any other malformed
                 * control message. Whether the sender was ENTITLED to propose
                 * is not decidable here: it depends on which endpoints have
                 * already departed, which only the launcher tracks. */
                if (!value.contains("endpoint") ||
                    !value["endpoint"].is_number_unsigned() ||
                    !value.contains("epoch") ||
                    !value["epoch"].is_number_unsigned() ||
                    value["epoch"].get<uint64_t>() > UINT32_MAX ||
                    !value.contains("tick") ||
                    !value["tick"].is_number_unsigned() ||
                    value["tick"].get<uint64_t>() > UINT32_MAX) {
                    peerLost(peer,
                             MdkrMatchPeerLostReason::ControlChannelViolation);
                    return;
                }
                const uint64_t departed = value["endpoint"].get<uint64_t>();
                if (departed == localEndpointId ||
                    departed == peer.endpointId ||
                    peers.find(departed) == peers.end()) {
                    peerLost(peer,
                             MdkrMatchPeerLostReason::ControlChannelViolation);
                    return;
                }
                RaceDropProposal proposal;
                proposal.senderEndpointId = peer.endpointId;
                proposal.matchEpoch =
                    static_cast<uint32_t>(value["epoch"].get<uint64_t>());
                proposal.tick =
                    static_cast<uint32_t>(value["tick"].get<uint64_t>());
                /* First proposal for an endpoint wins; the channel is
                 * ordered, so every recipient sees the same first one. */
                (void)raceDrops.emplace(departed, proposal);
                return;
            }
            if (type == "race_abort") {
                /* F3: the peer is abandoning the race start (or ended
                 * mid-race). Latch it for the launcher; not a peer loss, so the
                 * channel stays open and this endpoint keeps answering pings. */
                raceAbortReceived = true;
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
            bool derived = true;
            for (unsigned lane = 0u; lane < MDKR_MATCH_PEER_LANE_COUNT;
                 ++lane) {
                outbound.lane = static_cast<uint8_t>(lane);
                inbound.lane = static_cast<uint8_t>(lane);
                peer.sealKeys[lane] = mdkr_match_peer_identity_derive_key(
                    &keyring, identity, peer.publicKey, digest, &outbound);
                peer.openKeys[lane] = mdkr_match_peer_identity_derive_key(
                    &keyring, identity, peer.publicKey, digest, &inbound);
                peer.replay[lane] = MdkrMatchPeerReplayWindow{};
                derived = derived && peer.sealKeys[lane] != nullptr &&
                    peer.openKeys[lane] != nullptr;
            }
            if (!derived) {
                /* An invalid revealed key surfaces here (derive validates
                 * the curve point): that is THIS peer's typed loss, not a
                 * mesh failure -- the commitment round passed over bytes
                 * ECDH cannot consume. The lost peer blocks completion, so
                 * no phrase can ever be produced from the broken roster,
                 * while the mesh itself stays alive and typed. */
                peerLost(peer, MdkrMatchPeerLostReason::HelloViolation);
                mdkr_match_peer_keyring_forget(&keyring);
                for (auto &reset : peers) forgetLaneKeys(reset.second);
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
            if (peer.lost || peer.generation == 0u) continue;
            /* Vanish dwell: ABSENT from signaling AND channels down. Neither
             * setup ladder can run against an endpoint the relay cannot
             * reach (no offer/answer/hello can be delivered), so without
             * this bound that state parks forever -- the real-cloud mid-race kill
             * left the survivor exactly there once ICE tore the connection
             * down. Both facts are transport state; either recovering
             * disarms the dwell (a signal blip with healthy channels never
             * arms it, and a reconnect bump re-admits the peer even after
             * expiry via rekeyPeer's lost=false). A double blip -- our own
             * re-welcome omitting a simultaneously-blipping peer whose bump
             * broadcast we missed -- is covered by rosterPeer's adoption of
             * a service-stamped newer sender generation: the returning
             * peer's re-driven traffic cancels this clock before expiry.
             * Residual: a reconnected peer that stays completely silent
             * through the whole dwell is still declared lost at expiry;
             * its later bump re-admits the mesh peer, only the
             * already-latched race end stands. */
            if (!peer.present && !peer.channelsReady) {
                if (peer.vanishedSinceMs == 0u) {
                    peer.vanishedSinceMs = nowMs;
                } else if (nowMs - peer.vanishedSinceMs >=
                           kMdkrMatchPeerVanishTimeoutMs) {
                    peerLost(peer, MdkrMatchPeerLostReason::PeerVanished);
                    continue;
                }
            } else {
                peer.vanishedSinceMs = 0u;
            }
            /* The SETUP ladders (hellos, offers, the answerer's deadline)
             * require a present peer: every step is a signaling delivery,
             * and a re-appearing peer resumes them where they stood. The
             * LIVENESS ladder (control ping, below) deliberately does NOT --
             * it probes the established channels themselves, and gating it
             * on presence starved mid-race loss detection whenever the
             * relay truthfully reported the killed peer's socket closing
             * (the shipped defect: presence dropped seconds after the SIGKILL,
             * freezing the very ladder that would have caught it). */
            if (peer.present) {
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
                    /* Channels DID open, just not all of them: the offer's
                     * channel set is short, which is what an endpoint that
                     * predates a channel looks like. Name it instead of
                     * reporting the ICE-never-completed verdict. */
                    peerLost(peer, missingChannel(peer) ?
                                       MdkrMatchPeerLostReason::
                                           ChannelSetMismatch
                                     : MdkrMatchPeerLostReason::ConnectTimeout);
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
                        /* Same diagnosis as the answerer's deadline: channels
                         * opened but not the whole set, so the peer's protocol
                         * is short a channel rather than unreachable. */
                        peerLost(peer, missingChannel(peer) ?
                                           MdkrMatchPeerLostReason::
                                               ChannelSetMismatch
                                         : MdkrMatchPeerLostReason::
                                               ConnectTimeout);
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
            } /* peer.present (setup ladders only) */
            /* Control ping ladder (5 s cadence, 15 s stale) -- runs on the
             * ESTABLISHED channels regardless of signal presence (see the
             * presence note above): kMdkrMatchControlPingIntervalMs +
             * kMdkrMatchControlPingTimeoutMs is the ping-path loss bound
             * (kMdkrMatchMidRaceLossPingBoundMs; the composite worst case
             * across orderings is kMdkrMatchMidRaceLossDetectBoundMs). */
            if (peer.channelsReady) {
                if (peer.pingOutstandingSinceMs != 0u &&
                    nowMs - peer.pingOutstandingSinceMs >=
                        kMdkrMatchControlPingTimeoutMs) {
                    peerLost(peer, MdkrMatchPeerLostReason::PingTimeout);
                    continue;
                }
                if (peer.pingOutstandingSinceMs == 0u &&
                    peer.nextPingAtMs != 0u && nowMs >= peer.nextPingAtMs &&
                    peer.channels[MDKR_MATCH_PEER_LANE_CONTROL] &&
                    peer.channels[MDKR_MATCH_PEER_LANE_CONTROL]->isOpen()) {
                    peer.pingNonce++;
                    peer.pingOutstandingSinceMs = nowMs;
                    peer.nextPingAtMs = nowMs + kMdkrMatchControlPingIntervalMs;
                    try {
                        peer.channels[MDKR_MATCH_PEER_LANE_CONTROL]->send(
                            Json{{"type", "ping"},
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
        uint64_t dropped;
        {
            std::lock_guard<std::mutex> lock(queueMutex);
            pending.swap(internalQueue);
            dropped = droppedInternal;
        }
        /* The callback threads fill the internal queue, so the drop counters
         * are read (never recorded) off-thread and only compared here, where
         * the ring has its single writer. */
        noteQueueDepth(pending.size(), dropped + counters.droppedEvents);
        for (const InternalEvent &event : pending) {
            handleInternal(event);
        }
        tick();
    }

    /* Pressure crosses at three quarters of the callback queue's bound, which
     * is the last pump before a burst starts costing events. */
    void noteQueueDepth(size_t depth, uint64_t dropped) {
        const size_t pressureAt = kMaxInternalEvents - kMaxInternalEvents / 4u;
        const bool pressured = depth >= pressureAt;
        if (pressured && !queuePressured) {
            mdkr_net_failure_ring_record_host(
                MDKR_NET_FAILURE_QUEUE_PRESSURE, static_cast<uint32_t>(now()),
                MDKR_NET_FAILURE_NO_SLOT,
                static_cast<unsigned>(depth * 100u / kMaxInternalEvents),
                nullptr);
        }
        queuePressured = pressured;
        if (dropped != queueDroppedSeen) {
            queueDroppedSeen = dropped;
            mdkr_net_failure_ring_record_host(
                MDKR_NET_FAILURE_QUEUE_OVERFLOW, static_cast<uint32_t>(now()),
                MDKR_NET_FAILURE_NO_SLOT, 0u, nullptr);
        }
    }

    /* ---- Data plane ---------------------------------------------------------*/

    /* True when the peer's connection produced SOME channels but never the
     * whole set: at the setup deadline that is a protocol-version mismatch,
     * not an unreachable endpoint. Logs the labels that never arrived, so a
     * stderr capture names the missing channel even where the reason code
     * alone would not. */
    static bool missingChannel(const PeerRuntime &peer) {
        const unsigned open = openChannelCount(peer);
        if (open == 0u || open == MDKR_MATCH_PEER_LANE_COUNT) return false;
        for (unsigned lane = 0u; lane < MDKR_MATCH_PEER_LANE_COUNT; ++lane) {
            if (!peer.channelOpen[lane]) {
                MDKR_MESH_LOG(
                    "[MESH] peer ep=%llu never opened channel=%s\n",
                    (unsigned long long)peer.endpointId,
                    kMdkrMatchChannelLabels[lane]);
            }
        }
        return true;
    }

    static unsigned openChannelCount(const PeerRuntime &peer) {
        unsigned open = 0u;
        for (unsigned lane = 0u; lane < MDKR_MATCH_PEER_LANE_COUNT; ++lane) {
            if (peer.channelOpen[lane]) open++;
        }
        return open;
    }

    static void forgetLaneKeys(PeerRuntime &peer) {
        for (unsigned lane = 0u; lane < MDKR_MATCH_PEER_LANE_COUNT; ++lane) {
            peer.sealKeys[lane] = nullptr;
            peer.openKeys[lane] = nullptr;
            peer.replay[lane] = MdkrMatchPeerReplayWindow{};
        }
    }

    /* Seal one 64-byte payload under `lane`'s own key and send it on that
     * lane's channel. Each lane's window owns its own nonce sequence, so a
     * send here can never advance another channel's. An exhausted window is
     * this direction's typed, terminal loss whichever lane spends the last
     * sequence. */
    bool sealAndSend(PeerRuntime &peer, uint8_t lane, uint8_t payloadType,
                     const uint8_t payload[MDKR_MATCH_PEER_PAYLOAD_BYTES]) {
        MdkrMatchPeerSealingKey *sealing = peer.sealKeys[lane];
        const std::shared_ptr<rtc::DataChannel> &channel = peer.channels[lane];
        if (peer.lost || !peer.channelsReady || sealing == nullptr ||
            !channel || !channel->isOpen()) {
            return false;
        }
        MdkrMatchPeerSendContext context{};
        context.key = sealing->direction;
        context.intermediate_endpoint_id = 0u; /* direct mesh only */
        context.payload_type = payloadType;
        uint8_t envelope[MDKR_MATCH_PEER_ENVELOPE_BYTES];
        if (!mdkr_match_peer_seal(sealing, &context, payload, envelope)) {
            if (!sealing->window.ready) {
                /* Nonce space spent: this direction requires a reconnect and
                 * a fresh generation-bound key. Typed and terminal. */
                peerLost(peer, MdkrMatchPeerLostReason::SealWindowExhausted);
            }
            return false;
        }
        try {
            if (!channel->send(reinterpret_cast<const std::byte *>(envelope),
                               sizeof(envelope))) {
                return false;
            }
        } catch (...) {
            /* The channel-down event will drive recovery. */
            return false;
        }
        peer.bytesSent += sizeof(envelope);
        return true;
    }

    unsigned sendInput(const uint8_t bundle[MDKR_MATCH_PEER_PAYLOAD_BYTES]) {
        if (closed || failed || !keysDerived) return 0u;
        unsigned reached = 0u;
        for (auto &entry : peers) {
            PeerRuntime &peer = entry.second;
            if (!sealAndSend(peer, MDKR_MATCH_PEER_LANE_STATE,
                             MDKR_MATCH_PEER_PAYLOAD_INPUT, bundle)) {
                continue;
            }
            reached++;
        }
        return reached;
    }

    /* F3: fan a plaintext race-abort out to every peer on its reliable control
     * channel (versioned/typed like ping; the receiver does not correlate the
     * nonce). Best-effort: a peer whose channel is not open is skipped. */
    unsigned sendRaceAbort() {
        if (closed || failed) return 0u;
        unsigned reached = 0u;
        for (auto &entry : peers) {
            PeerRuntime &peer = entry.second;
            const std::shared_ptr<rtc::DataChannel> &controlChannel =
                peer.channels[MDKR_MATCH_PEER_LANE_CONTROL];
            if (peer.lost || !controlChannel || !controlChannel->isOpen())
                continue;
            try {
                if (controlChannel->send(Json{{"type", "race_abort"},
                        {"protocol", kChannelProtocol},
                        {"nonce", 0u}}.dump())) {
                    reached++;
                }
            } catch (...) {
                connectionDown(peer);
            }
        }
        return reached;
    }

    /* A3: fan the agreed finalisation tick out on the reliable control
     * channel. `checkRoster` is the production path's refusal of an endpoint
     * this room never had; the test seam sends without it so the RECIPIENT's
     * validation is what a test observes. */
    unsigned sendRaceDrop(uint64_t departedEndpointId, uint32_t matchEpoch,
                          uint32_t tick, bool checkRoster) {
        if (closed || failed || departedEndpointId == 0u) return 0u;
        if (checkRoster && peers.find(departedEndpointId) == peers.end()) {
            return 0u;
        }
        unsigned reached = 0u;
        for (auto &entry : peers) {
            PeerRuntime &peer = entry.second;
            const std::shared_ptr<rtc::DataChannel> &controlChannel =
                peer.channels[MDKR_MATCH_PEER_LANE_CONTROL];
            if (peer.lost || peer.endpointId == departedEndpointId ||
                !controlChannel || !controlChannel->isOpen()) {
                continue;
            }
            try {
                if (controlChannel->send(Json{{"type", "race_drop"},
                        {"protocol", kChannelProtocol},
                        {"nonce", 0u},
                        {"endpoint", departedEndpointId},
                        {"epoch", matchEpoch},
                        {"tick", tick}}.dump())) {
                    reached++;
                }
            } catch (...) {
                connectionDown(peer);
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
        return sealAndSend(found->second, MDKR_MATCH_PEER_LANE_CONTROL,
                           MDKR_MATCH_PEER_PAYLOAD_PREFLIGHT_FRAGMENT,
                           fragment);
    }

    bool sendInputRepair(
        uint64_t peerEndpointId, uint8_t payloadType,
        const uint8_t message[MDKR_MATCH_PEER_PAYLOAD_BYTES]) {
        if (closed || failed || !keysDerived ||
            (payloadType != MDKR_MATCH_PEER_PAYLOAD_INPUT_REPAIR_REQUEST &&
             payloadType != MDKR_MATCH_PEER_PAYLOAD_INPUT_REPAIR_ANSWER)) {
            return false;
        }
        const auto found = peers.find(peerEndpointId);
        if (found == peers.end()) return false;
        return sealAndSend(found->second, MDKR_MATCH_PEER_LANE_AUTHORITY,
                           payloadType, message);
    }

    /* ---- Teardown ------------------------------------------------------------*/

    void teardown(bool announcePeerEnd) {
        if (closed) return;
        closed = true;
        /* DELIBERATE local end only (see close()): announce peer_end "close"
         * (the wire contract's goodbye -- handlePeerEnd on every conforming
         * endpoint, relayed and sender-stamped by the service) to each
         * still-viable peer BEFORE the connections come down, so a survivor
         * resolves this endpoint as the immediate typed PeerLost(PeerEnded)
         * instead of grinding the restart episodes / vanish dwell against a
         * connection that will never return. Best-effort by design: a crash
         * sends nothing (survivors keep the ladder-typed resolution), a send
         * into a dead feed is inert, and an internal rebuild (announce false)
         * says nothing at all. */
        if (announcePeerEnd) {
            for (auto &entry : peers) {
                PeerRuntime &peer = entry.second;
                if (peer.lost || peer.generation == 0u) continue;
                MdkrMatchSignalOutbound goodbye;
                goodbye.type = "peer_end";
                goodbye.reason = "close";
                (void)sendSignal(peer, std::move(goodbye));
            }
        }
        std::vector<std::shared_ptr<rtc::PeerConnection>> connections;
        for (auto &entry : peers) {
            PeerRuntime &peer = entry.second;
            peer.attempt++; /* orphan every callback first */
            if (peer.connection) {
                connections.push_back(std::move(peer.connection));
            }
            peer.connection.reset();
            for (unsigned lane = 0u; lane < MDKR_MATCH_PEER_LANE_COUNT;
                 ++lane) {
                peer.channels[lane].reset();
            }
            forgetLaneKeys(peer);
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
        peer.rosterIndex = static_cast<unsigned>(state->peers.size());
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

unsigned MdkrMatchPeerMesh::sendRaceAbort() {
    return state_ ? state_->sendRaceAbort() : 0u;
}

bool MdkrMatchPeerMesh::consumeRaceAbort() {
    if (!state_ || !state_->raceAbortReceived) return false;
    state_->raceAbortReceived = false;
    return true;
}

bool MdkrMatchPeerMesh::retireDepartedPeer(uint64_t endpointId) {
    if (!state_) return false;
    const auto found = state_->peers.find(endpointId);
    if (found == state_->peers.end() || found->second.lost) return false;
    state_->peerLost(found->second, MdkrMatchPeerLostReason::PeerDeparted,
                     /*announce=*/false);
    return true;
}

unsigned MdkrMatchPeerMesh::sendRaceDrop(uint64_t departedEndpointId,
                                         uint32_t matchEpoch, uint32_t tick) {
    return state_ ? state_->sendRaceDrop(departedEndpointId, matchEpoch, tick,
                                         /*checkRoster=*/true)
                  : 0u;
}

bool MdkrMatchPeerMesh::peekRaceDrop() const {
    return state_ && !state_->raceDrops.empty();
}

bool MdkrMatchPeerMesh::consumeRaceDrop(uint64_t *senderEndpointId,
                                        uint64_t *departedEndpointId,
                                        uint32_t *matchEpoch, uint32_t *tick) {
    if (!state_ || state_->raceDrops.empty() ||
        senderEndpointId == nullptr || departedEndpointId == nullptr ||
        matchEpoch == nullptr || tick == nullptr) {
        return false;
    }
    const auto oldest = state_->raceDrops.begin();
    *senderEndpointId = oldest->second.senderEndpointId;
    *departedEndpointId = oldest->first;
    *matchEpoch = oldest->second.matchEpoch;
    *tick = oldest->second.tick;
    state_->raceDrops.erase(oldest);
    return true;
}

void MdkrMatchPeerMesh::clearRaceDrops() {
    if (state_) state_->raceDrops.clear();
}

bool MdkrMatchPeerMesh::sendPreflightFragment(
    uint64_t peerEndpointId,
    const uint8_t fragment[MDKR_MATCH_PEER_PAYLOAD_BYTES]) {
    return state_->sendPreflightFragment(peerEndpointId, fragment);
}

bool MdkrMatchPeerMesh::sendInputRepair(
    uint64_t peerEndpointId, uint8_t payloadType,
    const uint8_t message[MDKR_MATCH_PEER_PAYLOAD_BYTES]) {
    if (message == nullptr) return false;
    return state_->sendInputRepair(peerEndpointId, payloadType, message);
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

bool MdkrMatchPeerMesh::authenticatedPacketCount(uint64_t peerEndpointId,
                                                 uint64_t *out) const {
    if (out == nullptr || !state_) return false;
    const auto found = state_->peers.find(peerEndpointId);
    if (found == state_->peers.end()) return false;
    *out = found->second.authenticatedPackets;
    return true;
}

bool MdkrMatchPeerMesh::peerChannelsReady(uint64_t peerEndpointId) const {
    if (!state_) return false;
    const auto found = state_->peers.find(peerEndpointId);
    return found != state_->peers.end() && !found->second.lost &&
           found->second.channelsReady;
}

unsigned MdkrMatchPeerMesh::linkStats(
    MdkrMatchPeerLinkStats *out, unsigned max) const {
    unsigned written = 0u;
    if (out == nullptr || !state_) return 0u;
    for (const auto &entry : state_->peers) {
        const PeerRuntime &peer = entry.second;
        if (written >= max) break;
        out[written].endpointId = peer.endpointId;
        out[written].rosterIndex = peer.rosterIndex;
        out[written].rttMs = peer.rttMs;
        out[written].jitterMs = peer.jitterMs;
        out[written].bytesSent = peer.bytesSent;
        out[written].bytesReceived = peer.bytesReceived;
        out[written].consecutivePingMisses = 0u;
        if (peer.channelsReady && !peer.lost &&
            peer.pingOutstandingSinceMs != 0u) {
            const uint64_t elapsed = state_->now() - peer.pingOutstandingSinceMs;
            out[written].consecutivePingMisses =
                (uint32_t)(elapsed / kMdkrMatchControlPingIntervalMs);
        }
        written++;
    }
    return written;
}

MdkrMatchPeerMeshStats MdkrMatchPeerMesh::stats() const {
    MdkrMatchPeerMeshStats stats = state_->counters;
    std::lock_guard<std::mutex> lock(state_->queueMutex);
    stats.droppedInternalEvents = state_->droppedInternal;
    return stats;
}

void MdkrMatchPeerMesh::close(bool announcePeerEnd) {
    if (state_) state_->teardown(announcePeerEnd);
}

/* ---- Test seams -------------------------------------------------------------*/

bool mdkr_match_peer_mesh_exhaust_seal_for_test(MdkrMatchPeerMesh &mesh,
                                                uint64_t peerEndpointId) {
    const auto found = mesh.state_->peers.find(peerEndpointId);
    if (found == mesh.state_->peers.end() ||
        found->second.sealKeys[MDKR_MATCH_PEER_LANE_STATE] == nullptr) {
        return false;
    }
    /* Exactly the state one UINT64_MAX seal leaves behind
     * (test_match_peer_crypto.cpp pins it): not ready, sequence zeroed. */
    MdkrMatchPeerSealingKey *sealing =
        found->second.sealKeys[MDKR_MATCH_PEER_LANE_STATE];
    sealing->window.ready = false;
    sealing->window.next_sequence = 0u;
    return true;
}

bool mdkr_match_peer_mesh_send_raw_race_drop_for_test(
    MdkrMatchPeerMesh &mesh, uint64_t departedEndpointId, uint32_t matchEpoch,
    uint32_t tick) {
    return mesh.state_ && mesh.state_->sendRaceDrop(
        departedEndpointId, matchEpoch, tick, /*checkRoster=*/false) > 0u;
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
