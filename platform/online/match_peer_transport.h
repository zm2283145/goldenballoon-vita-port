/*
 * Native match peer transport (O-T2): the launcher-owned <=4-endpoint
 * full-mesh of WebRTC DataChannels behind one online race.
 *
 * Pure composition of proven layers -- this file adds NO crypto and NO game
 * logic:
 *  - signaling rides the validated match-signal surface
 *    (platform/online/match_signal_client.h, docs/ref/match-signaling-v1.md),
 *    consumed through the injectable MdkrMatchPeerSignalFeed seam below;
 *  - the committed key exchange is platform/net/match_peer_transcript.h +
 *    match_peer_crypto.h (ephemeral P-256, commit-then-reveal, HKDF into a
 *    direction/epoch/generation-bound keyring slot);
 *  - gameplay/preflight bytes cross the wire ONLY as sealed 132-byte
 *    envelopes (mdkr_match_peer_seal/open);
 *  - connection discipline mirrors the Phone Party transport
 *    (platform/party/libdatachannel_party_transport.cpp): callbacks -> one
 *    bounded queue -> launcher pump, generation/attempt-guarded stale
 *    rejection, the 5 s control ping / 15 s stale ladder, and the
 *    mdkr_party_retry_decide unanswered-offer ladder reused verbatim
 *    (party_retry_policy.h is transport-agnostic).
 *
 * Commitment transport. match-signaling-v1 has no commitment message: the
 * only key-shaped field on the wire is peer_hello's 65-byte publicKey
 * (validated as canonical 87-char base64url, 0x04-prefixed, nonzero body --
 * shape, not curve membership). The transcript layer's commitment functions
 * therefore define the payload, and the commitment rides INSIDE the
 * peer_hello publicKey exchange as a fixed three-hello sequence per sender
 * per (epoch, connection generation), positional under the wire's strictly
 * increasing per-sender sequences:
 *   hello 1 (round-1 commit):  0x04 || commitment[32] || zero[32]
 *   hello 2 (round-2 reveal):  the real uncompressed P-256 public key
 *   hello 3 (round-2 opening): 0x04 || commit_nonce[32] || zero[32]
 * An honest endpoint sends hello 1 as soon as the peer's generation is
 * known and withholds hellos 2 and 3 until that peer's hello 1 has arrived,
 * so no endpoint can choose its key after seeing another's -- the exact
 * property of docs/ref/match-peer-carrier-v1.md's key commitment round.
 * Reserved bytes must be zero; a fourth hello, malformed blob or failed
 * commitment opening is terminal for that peer. The transcript digest
 * re-verifies every commitment, so a phrase can never be produced from
 * uncommitted key material even if this layer forgot a check.
 *
 * Glare rule: the numerically LOWER endpoint id is the sole offerer for a
 * pair; the higher id only answers. The wire has no polite/impolite
 * convention (services/party/src/match relays symmetrically), so this
 * mirrors the house lowest-id tie-break already pinned in
 * match_peer_graph.h route selection. Glare is impossible by construction.
 *
 * Channels per peer:
 *   gb-match-state-v1   -- unordered, maxRetransmits 0 (lossy by design);
 *                          carries ONLY sealed INPUT envelopes. A datagram
 *                          that fails to open is counted and dropped, never
 *                          terminal.
 *   gb-match-control-v1 -- reliable ordered; sealed PREFLIGHT fragments +
 *                          the bounded ping. A reliable channel never
 *                          delivers garbage, so an open failure here IS
 *                          terminal for the peer.
 *
 * One-hop forwarding (match_peer_graph.h / match_peer_forward.h) is OUT of
 * scope for this transport revision: only the direct mesh is wired. The
 * seam is marked in match_peer_transport.cpp where a non-direct route would
 * consult the graph and the forwarder admission window.
 *
 * Threading: create()/pump()/drainEvents()/sendInput()/
 * sendPreflightFragment()/phrase()/stats()/close() are launcher-thread
 * calls. libdatachannel callbacks only copy into the bounded internal
 * queue; every signal-feed send happens inside pump() on the launcher
 * thread (the real client's send() is launcher-thread-only). No lock is
 * ever held across a user callback.
 */
#ifndef MDKR_MATCH_PEER_TRANSPORT_H
#define MDKR_MATCH_PEER_TRANSPORT_H

#include "online/lobby_core.h"
#include "online/match_signal_client.h"
#include "net/match_peer_crypto.h"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

/* Channel labels (versioned, mirror mdkr-pad-*-v1's naming discipline). */
inline constexpr char kMdkrMatchStateChannelLabel[] = "gb-match-state-v1";
inline constexpr char kMdkrMatchControlChannelLabel[] = "gb-match-control-v1";

/* Control ping ladder, contractual like the party transport's: one ping
 * every 5 s of quiet, a peer is stale after 15 s without its pong. */
inline constexpr unsigned kMdkrMatchControlPingIntervalMs = 5000u;
inline constexpr unsigned kMdkrMatchControlPingTimeoutMs = 15000u;

/* Bounded ICE-restart episodes per peer (a channel that dies immediately
 * after every restart must not loop forever), matching the offer ladder's
 * 3-attempt bound in party_retry_policy.h. */
inline constexpr unsigned kMdkrMatchMaxRestartEpisodes = 3u;

/*
 * Injectable signaling seam. The mesh consumes validated match-signal
 * events and produces outbound messages through this interface only, so
 * tests drive a whole mesh with an in-process loopback bridge and the
 * launcher wraps the real MdkrMatchSignalClient with the adapter below.
 * While a mesh is live it owns the feed's drainEvents(); the launcher must
 * not drain the same client concurrently.
 */
class MdkrMatchPeerSignalFeed {
public:
    virtual ~MdkrMatchPeerSignalFeed() = default;
    virtual MdkrMatchSignalSendResult send(
        const MdkrMatchSignalOutbound &message) = 0;
    virtual void drainEvents(std::vector<MdkrMatchSignalEvent> &out) = 0;
};

/* The production adapter: borrows the launcher-owned signal client. */
class MdkrMatchSignalClientFeed final : public MdkrMatchPeerSignalFeed {
public:
    explicit MdkrMatchSignalClientFeed(MdkrMatchSignalClient *client)
        : client_(client) {}
    MdkrMatchSignalSendResult send(
        const MdkrMatchSignalOutbound &message) override {
        if (client_ == nullptr) return MdkrMatchSignalSendResult{};
        return client_->send(message);
    }
    void drainEvents(std::vector<MdkrMatchSignalEvent> &out) override {
        if (client_ == nullptr) { out.clear(); return; }
        client_->drainEvents(out);
    }

private:
    MdkrMatchSignalClient *client_ = nullptr;
};

/* Server-delivered ICE servers, validated with the wave-0 TURN-scoped
 * credential rule (a credentialed entry naming a non-turn/turns url is
 * skipped, never handed to the ICE agent). An empty list means host
 * candidates only -- the mesh never invents connectivity config; the
 * service delivers it through the room bootstrap. */
struct MdkrMatchPeerIceServer {
    std::string url;
    std::string username;
    std::string credential;
};

struct MdkrMatchPeerSlotOwner {
    uint64_t endpointId = 0u;
    uint8_t slotMask = 0u;
};

enum class MdkrMatchPeerMeshEventType {
    /* Both channels to this endpoint are open. */
    PeerChannelsReady,
    /* This endpoint is gone for this mesh, with a typed reason. */
    PeerLost,
    /* One opened INPUT payload (64 bytes) from this endpoint. */
    InputEnvelope,
    /* One opened PREFLIGHT fragment + its authenticated envelope context
     * (mdkr_match_preflight_fragment_submit consumes both). */
    PreflightFragment,
    /* Every peer's key is committed, opened and derived; phrase() answers. */
    PhraseReady,
    /* Mesh-level failure with a typed reason. */
    Failure,
};

enum class MdkrMatchPeerLostReason {
    /* Offer ladder exhausted (mdkr_party_retry_decide gave up), or the
     * bounded restart-episode budget ran out. */
    ConnectTimeout,
    /* 15 s without the peer's pong on the reliable control channel. */
    PingTimeout,
    /* This direction's seal window is exhausted: the direction requires a
     * reconnect and a fresh generation-bound key. */
    SealWindowExhausted,
    /* Garbage on the reliable control channel (bad envelope, wrong payload
     * type, malformed ping JSON). */
    ControlChannelViolation,
    /* The peer's round-1 commitment did not open over its revealed key. */
    CommitmentMismatch,
    /* Structural hello violation (bad blob, extra hello, invalid key). */
    HelloViolation,
    /* The peer sent peer_end reason "close". */
    PeerEnded,
    /* The channel died and no recovery path remains (signaling lost). */
    TransportFailed,
};

enum class MdkrMatchPeerMeshFailure {
    /* The signal feed reported its terminal failure. Healthy direct
     * channels keep carrying gameplay; only recovery paths are gone. */
    SignalLost,
    /* The welcome named a different identity than options promised. */
    IdentityMismatch,
    /* Transcript digest or key derivation refused (roster inconsistency). */
    KeyScheduleFailed,
};

struct MdkrMatchPeerMeshEvent {
    MdkrMatchPeerMeshEventType type = MdkrMatchPeerMeshEventType::Failure;
    uint64_t endpointId = 0u;
    MdkrMatchPeerLostReason lostReason =
        MdkrMatchPeerLostReason::TransportFailed;
    MdkrMatchPeerMeshFailure failure = MdkrMatchPeerMeshFailure::SignalLost;
    /* SignalLost carries the feed's failure-code string verbatim. */
    std::string failureCode;
    /* InputEnvelope / PreflightFragment: the opened 64-byte payload and the
     * authenticated envelope context (sequence, direction, payload type). */
    std::array<uint8_t, MDKR_MATCH_PEER_PAYLOAD_BYTES> payload{};
    MdkrMatchPeerEnvelopeContext context{};
};

/* Drop/rejection counters, diagnosability only (never terminal). */
struct MdkrMatchPeerMeshStats {
    /* State-channel datagrams dropped: wrong size, no key yet, or
     * open/replay rejection. The state channel is lossy by design. */
    uint64_t rejectedStateEnvelopes = 0u;
    /* Signaling messages ignored for stale generation / wrong role /
     * unknown endpoint / unexpected timing. */
    uint64_t ignoredStaleSignals = 0u;
    /* Internal bounded-queue overflow drops (callback -> pump). */
    uint64_t droppedInternalEvents = 0u;
    /* Public event-queue overflow drops (pump -> drainEvents). */
    uint64_t droppedEvents = 0u;
};

struct MdkrMatchPeerMeshOptions {
    /* Borrowed; the launcher owns the client and its lifetime. Must outlive
     * the mesh. */
    MdkrMatchPeerSignalFeed *signal = nullptr;
    /* 22-char base64url (16 bytes) -- the transcript's room binding. */
    std::string roomId;
    uint64_t localEndpointId = 0u;
    /* 0 adopts the welcome's generation; nonzero must match the welcome. */
    uint32_t localGeneration = 0u;
    uint32_t matchEpoch = 0u;
    /* Bound into the transcript digest (protocol/build/gameplay/ROM). */
    MdkrOnlineCompatibilityV1 compatibility{};
    /* Slot-ownership map: 2..4 unique endpoints including the local one. */
    std::vector<MdkrMatchPeerSlotOwner> roster;
    std::vector<MdkrMatchPeerIceServer> iceServers;
    /* Steady-clock seam for the retry/ping ladders (tests inject a fake;
     * production leaves it empty for the real steady clock). Never 0. */
    std::function<uint64_t()> nowMs;
};

class MdkrMatchPeerMesh {
public:
    /* Validates options (roster shape, room id, epoch, identity material);
     * returns nullptr with *errorMessage set on refusal. Never touches the
     * network: signaling starts flowing on the first pump(). */
    static std::unique_ptr<MdkrMatchPeerMesh> create(
        const MdkrMatchPeerMeshOptions &options,
        std::string *errorMessage = nullptr);

    ~MdkrMatchPeerMesh(); /* Implies close(). */

    MdkrMatchPeerMesh(const MdkrMatchPeerMesh &) = delete;
    MdkrMatchPeerMesh &operator=(const MdkrMatchPeerMesh &) = delete;

    /* Drain the signal feed + internal callback queue, drive the hello /
     * offer / ping ladders, perform every outbound signal send. Launcher
     * thread only. */
    void pump();

    /* Drain pending mesh events, oldest first, into `out` (cleared first). */
    void drainEvents(std::vector<MdkrMatchPeerMeshEvent> &out);

    /* Seal one 64-byte input bundle per reachable peer and fan it out on
     * the state channels. Returns the number of peers it reached. A peer
     * whose seal window is exhausted becomes PeerLost(SealWindowExhausted). */
    unsigned sendInput(const uint8_t bundle[MDKR_MATCH_PEER_PAYLOAD_BYTES]);

    /* Seal one 64-byte preflight fragment to one peer on its reliable
     * control channel. */
    bool sendPreflightFragment(
        uint64_t peerEndpointId,
        const uint8_t fragment[MDKR_MATCH_PEER_PAYLOAD_BYTES]);

    /* The transcript verification phrase. Available ONLY once every roster
     * peer's key is committed, opened and derived (mirrors the transcript
     * layer: no phrase from uncommitted key material); refuses otherwise. */
    bool phrase(std::string &out) const;

    MdkrMatchPeerMeshStats stats() const;

    /* Terminal, idempotent, bounded: closes every peer connection and
     * zeroizes the keyring. Never blocks on a remote peer. */
    void close();

private:
    struct State;
    explicit MdkrMatchPeerMesh(std::shared_ptr<State> state);
    std::shared_ptr<State> state_;

    friend bool mdkr_match_peer_mesh_exhaust_seal_for_test(
        MdkrMatchPeerMesh &mesh, uint64_t peerEndpointId);
    friend bool mdkr_match_peer_mesh_kill_channels_for_test(
        MdkrMatchPeerMesh &mesh, uint64_t peerEndpointId);
};

/* ---- Test seams (the *_for_test convention of the party transport) ------ */

/* Marks the outbound seal window toward `peerEndpointId` exhausted, exactly
 * the state one UINT64_MAX seal leaves behind, so the typed
 * PeerLost(SealWindowExhausted) path is testable without 2^64 seals. */
bool mdkr_match_peer_mesh_exhaust_seal_for_test(
    MdkrMatchPeerMesh &mesh, uint64_t peerEndpointId);

/* Closes the live PeerConnection toward `peerEndpointId` out from under the
 * mesh -- the observable shape of a mid-race network death -- to drive the
 * ICE-restart path. */
bool mdkr_match_peer_mesh_kill_channels_for_test(
    MdkrMatchPeerMesh &mesh, uint64_t peerEndpointId);

#endif /* MDKR_MATCH_PEER_TRANSPORT_H */
