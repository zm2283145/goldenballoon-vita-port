/*
 * Native match-signaling client: the launcher-side mirror of the dormant
 * browser reference client (dist/web/online/match-signal-client.js) for the
 * authenticated WebSocket at /api/match/{roomId}/signal
 * (docs/ref/match-signaling-v1.md).
 *
 * Conformance source: the JS client's validation surface, mirrored rule for
 * rule and failure-code string for failure-code string. Exact-key JSON
 * objects, u32/endpoint/publicKey validators, welcome only at generation 0
 * with at most 3 sorted unique non-self peers, peer-presence high-water-mark
 * rejection, signal_error correlation against recorded sent targets, directed
 * messages checked against self endpoint + current generation + the sender's
 * tracked generation + strictly increasing per-sender sequences, and the
 * 64-entry peer-generation map that fails CLOSED (peer_generation_overflow).
 * Any invalid server message is terminal: close 4003, code
 * invalid_signal_message, and every later call refuses.
 *
 * Transport is a hand-rolled RFC 6455 client (mbedtls TLS + the embedded
 * Mozilla bundle for wss, plain TCP for the token-gated loopback ws lane),
 * NOT rtc::WebSocket: the pinned libdatachannel client handshake neither
 * verifies nor exposes the server-selected subprotocol
 * (src/impl/wshandshake.cpp parses only Upgrade and Sec-WebSocket-Accept) and
 * its close() cannot carry the 4003 close code the reference client sends.
 * Both are load-bearing here -- the server MUST select `gb-match-signal-v1`
 * or the client refuses -- so the socket is implemented locally, the same
 * way lan_party_server.cpp hand-rolls the server half of RFC 6455.
 *
 * Threading contract (the MdkrPartyTransport rules): create()/connect()/
 * send()/close()/snapshot()/drainEvents() are launcher-thread calls. One
 * client-owned socket thread does connect/handshake/read and never calls
 * back into launcher code: every observation is copied into a bounded event
 * queue the launcher drains. The credential rides ONLY in the
 * `gb-match.{credential}` subprotocol -- never in the URL -- and close()
 * zeroizes it in memory.
 */
#ifndef MDKR_MATCH_SIGNAL_CLIENT_H
#define MDKR_MATCH_SIGNAL_CLIENT_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

/* The public version subprotocol the server must select on the 101. */
inline constexpr char kMdkrMatchSignalSubprotocol[] = "gb-match-signal-v1";

/*
 * Failure / refusal codes, byte-for-byte the reference client's strings.
 * The first eight arrive as Failure events or typed refusals; the last is
 * the JS TypeError message for an outbound message that fails validation
 * before anything touches the wire.
 */
inline constexpr char kMdkrMatchSignalTimeout[] = "signal_timeout";
inline constexpr char kMdkrMatchSignalTransportLost[] = "signal_transport_lost";
inline constexpr char kMdkrMatchSignalClientClosed[] = "signal_client_closed";
inline constexpr char kMdkrMatchSignalNotConnected[] = "signal_not_connected";
inline constexpr char kMdkrMatchSignalPeerUnavailable[] =
    "signal_peer_unavailable";
inline constexpr char kMdkrMatchSignalInvalidSubprotocol[] =
    "invalid_signal_subprotocol";
inline constexpr char kMdkrMatchSignalInvalidMessage[] =
    "invalid_signal_message";
inline constexpr char kMdkrMatchSignalPeerGenerationOverflow[] =
    "peer_generation_overflow";
inline constexpr char kMdkrMatchSignalInvalidClientMessage[] =
    "invalid match signal message";

/* create() refusal messages (the JS TypeError messages, verbatim). */
inline constexpr char kMdkrMatchSignalInvalidIdentity[] =
    "invalid match signal identity";
inline constexpr char kMdkrMatchSignalCrossOriginRefused[] =
    "cross-origin signaling refused";

enum class MdkrMatchSignalPhase { Idle, Connecting, Open, Failed, Closed };

/* Mirrors the JS snapshot(): {phase, connectionGeneration, nextSequence,
 * connected}, plus the bounded-queue drop counter for diagnosability. */
struct MdkrMatchSignalSnapshot {
    MdkrMatchSignalPhase phase = MdkrMatchSignalPhase::Idle;
    uint32_t connectionGeneration = 0u;
    uint64_t nextSequence = 1u;
    bool connected = false;
    uint64_t droppedEvents = 0u;
};

enum class MdkrMatchSignalEventType {
    Welcome,
    PeerPresence,
    SignalError,
    PeerHello,
    WebrtcOffer,
    WebrtcAnswer,
    WebrtcIce,
    PeerEnd,
    /* Terminal: the socket failed with failureCode (one of the strings
     * above). After this event the client refuses everything. */
    Failure,
};

struct MdkrMatchSignalPeerRef {
    std::string endpointId;
    uint32_t connectionGeneration = 0u;
};

/*
 * One validated server message (or the terminal failure). Only the fields
 * for the event's type are meaningful; the rest stay at their defaults, the
 * same way the JS client hands the launcher the validated JSON object.
 */
struct MdkrMatchSignalEvent {
    MdkrMatchSignalEventType type = MdkrMatchSignalEventType::Failure;

    /* Failure */
    std::string failureCode;

    /* Welcome: own identity + sorted unique peers. PeerPresence: the peer's
     * endpointId/connectionGeneration and `present`. */
    std::string endpointId;
    uint32_t connectionGeneration = 0u;
    std::vector<MdkrMatchSignalPeerRef> peers;
    bool present = false;

    /* SignalError (always error == "peer_unavailable"): the echoed sent
     * sequence and target. */
    uint32_t sequence = 0u;
    std::string toEndpointId;
    uint32_t toConnectionGeneration = 0u;
    std::string error;

    /* Directed messages: authenticated sender identity. */
    std::string fromEndpointId;
    uint32_t fromConnectionGeneration = 0u;

    /* PeerHello */
    std::string publicKey;
    /* WebrtcOffer / WebrtcAnswer */
    std::string sdp;
    /* WebrtcIce -- the has* flags carry the wire's explicit nulls. */
    std::string candidate;
    bool hasSdpMid = false;
    std::string sdpMid;
    bool hasSdpMLineIndex = false;
    uint32_t sdpMLineIndex = 0u;
    bool hasUsernameFragment = false;
    std::string usernameFragment;
    /* PeerEnd: "restart" | "close" */
    std::string reason;
};

/*
 * One outbound client message. `type` is one of peer_hello / webrtc_offer /
 * webrtc_answer / webrtc_ice / peer_end; only that type's payload fields may
 * be set (the typed mirror of the JS exact-key rule -- a peer_hello carrying
 * an sdp is invalid). For webrtc_ice the has* flags distinguish an explicit
 * null (false) from a value (true); all three ride the wire either way,
 * exactly like the JS message shape.
 */
struct MdkrMatchSignalOutbound {
    std::string type;
    std::string toEndpointId;
    uint32_t toConnectionGeneration = 0u;

    std::string publicKey;
    std::string sdp;
    std::string candidate;
    bool hasSdpMid = false;
    std::string sdpMid;
    bool hasSdpMLineIndex = false;
    uint32_t sdpMLineIndex = 0u;
    bool hasUsernameFragment = false;
    std::string usernameFragment;
    std::string reason;
};

/* send() outcome: ok with the consumed sequence, or a refusal whose error is
 * signal_not_connected, "invalid match signal message" or
 * signal_peer_unavailable -- the JS throw surface as a value. */
struct MdkrMatchSignalSendResult {
    bool ok = false;
    uint32_t sequence = 0u;
    std::string error;
};

struct MdkrMatchSignalClientOptions {
    /*
     * Service origin or explicit WebSocket base. https:// and wss:// speak
     * TLS against the embedded Mozilla bundle with hostname verification;
     * http:// and ws:// are accepted ONLY for the loopback test lane behind
     * the same MDKR_INTERNAL_TEST_TOKEN gate the Party transport uses
     * (mdkr_party_loopback_test_url_allowed) -- the credential never rides
     * plaintext off-machine. Native is originless: no Origin header is sent.
     */
    std::string serviceOrigin;
    std::string roomId;     /* 22-char base64url */
    std::string endpointId; /* decimal string, 1..2^64-1 */
    std::string credential; /* 43-char base64url; subprotocol-only */
    /* Welcome deadline; clamped to [2000, 30000] like the JS client
     * (0 selects the 10000 default). */
    unsigned timeoutMs = 10000u;
    /* Client-originated liveness (W3 N6a; a native-only extension BELOW the
     * mirrored JS validation surface -- a browser WebSocket cannot originate
     * pings, so the reference client has none, and RFC 6455 5.5.2/5.5.3
     * obliges the server to pong). After livenessIdleMs without ANY inbound
     * byte the socket thread sends a masked ping; when nothing inbound
     * follows within livenessTimeoutMs more, the transport is declared lost
     * through the existing terminal path (signal_transport_lost), so a
     * NAT-timed-out half-open socket can no longer report Open forever
     * while ICE-restart recovery is silently dead. 0 selects the defaults
     * (20000 / 10000 ms). */
    unsigned livenessIdleMs = 0u;
    unsigned livenessTimeoutMs = 0u;
};

/* ---- Test seams (the *_for_test convention of the party transport) -------
 *
 * Process-global, test-binary-only knobs over the client's resolver. The
 * production launcher never calls them; they exist so the connect loop's
 * address-budget and bounded-resolve behavior are pinned by wire-level tests
 * instead of only being reachable against a broken home network.
 */

/* Prepend one numeric address to every subsequent resolution, ahead of the
 * real getaddrinfo results -- the broken-AAAA-first household shape. An
 * unroutable TEST-NET address here must cost at most the per-address budget,
 * never the whole welcome deadline. Pass nullptr to clear. */
void mdkr_match_signal_client_prepend_address_for_test(const char *ip,
                                                       uint16_t port);

/* Stall every subsequent resolution by `ms` before it completes -- the DNS
 * outage shape. close() must still return promptly (the resolver is
 * deadline-bounded and abandoned, never joined). 0 clears. */
void mdkr_match_signal_client_stall_resolver_for_test(unsigned ms);

class MdkrMatchSignalClient {
public:
    /*
     * Validates identity and origin exactly like the JS factory; returns
     * nullptr with *errorMessage set to kMdkrMatchSignalInvalidIdentity or
     * kMdkrMatchSignalCrossOriginRefused on refusal. Never connects.
     */
    static std::unique_ptr<MdkrMatchSignalClient> create(
        const MdkrMatchSignalClientOptions &options,
        std::string *errorMessage = nullptr);

    ~MdkrMatchSignalClient(); /* Implies close(). */

    MdkrMatchSignalClient(const MdkrMatchSignalClient &) = delete;
    MdkrMatchSignalClient &operator=(const MdkrMatchSignalClient &) = delete;

    /*
     * Begin (or continue) connecting. True when the client is now
     * connecting or already open; the outcome arrives on the event queue as
     * Welcome or Failure. False with *errorCode = signal_client_closed once
     * the client has failed or closed (the JS rejected-promise path).
     */
    bool connect(std::string *errorCode = nullptr);

    /*
     * Validate and send one message. Mirrors the JS send(): refuses with
     * signal_not_connected unless open, validates the full outbound shape
     * ("invalid match signal message"), refuses a target whose generation is
     * not the tracked current one (signal_peer_unavailable), records the
     * sent target for signal_error correlation and consumes one monotonic
     * sequence number.
     */
    MdkrMatchSignalSendResult send(const MdkrMatchSignalOutbound &message);

    /* Terminal. Sends close 1000 "launcher_route_changed", wipes the
     * credential and every tracked map, and joins the socket thread. */
    void close();

    MdkrMatchSignalSnapshot snapshot() const;

    /* Drain the bounded event queue, oldest first, into `out` (cleared
     * first). Launcher-thread only. */
    void drainEvents(std::vector<MdkrMatchSignalEvent> &out);

private:
    struct State;
    explicit MdkrMatchSignalClient(std::shared_ptr<State> state);
    std::shared_ptr<State> state_;
};

#endif /* MDKR_MATCH_SIGNAL_CLIENT_H */
