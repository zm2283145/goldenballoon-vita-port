/* Launcher-owned native Phone Party model and replaceable transport seam. */
#ifndef MDKR_NATIVE_PARTY_HOST_H
#define MDKR_NATIVE_PARTY_HOST_H

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

enum class MdkrNativePartyPhase {
    Closed,
    Opening,
    Open,
    InviteRevoked,
    Recovering,
    Error,
    /* I4: the service says this room can never come back (deleted, expired,
     * or refusing every resume for good). Terminal: the transport is shut
     * down and never reopened for this room; the only way forward is a
     * fresh open() into a brand-new room, which the UI offers as Create New
     * Invite. Distinct from Error so the surface reads as "this session is
     * over", not "something broke". */
    RoomEnded,
};

enum class MdkrNativePartyControllerPhase {
    Pending,
    Approved,
    Leased,
    Connected,
};

struct MdkrNativePartyController {
    std::string id;
    std::string name;
    /* Base64url-encoded uncompressed P-256 key used for the verified SAS. */
    std::string publicKey;
    std::string pairingPhrase;
    MdkrNativePartyControllerPhase phase =
        MdkrNativePartyControllerPhase::Pending;
    unsigned seat = 0u;  // 1..4 when approved; zero while pending.
    uint32_t leaseGeneration = 0u;
    uint32_t connectionSequence = 0u;
    bool direct = false;
    bool haptics = false;
    bool commandPending = false;
    /* P2.1 compare-then-trust: the human pressed Words Match for this phone's
     * channel-bound pairing phrase, granting seat CUSTODY. Approval alone is
     * now a provisional connection only -- the WebRTC/DTLS comes up and the
     * phrase is computed and shown, but this stays false, so applyRoomState
     * binds NO ingress and applyEvent discards every pad packet at the
     * ingress boundary (fail-neutral). confirmPairing() flips it, binds the
     * seat, and tells the phone; Words Differ takes the ordinary remove path
     * so a differing phone is confirmed=false forever and never held a seat.
     * Launcher-owned (never carried on the wire); carried across room updates
     * of the same phone + key so a trusted phone auto-resumes its seat on
     * reconnect with no re-compare, exactly like everConnected. */
    bool confirmed = false;
    /* F2: whether this lease has EVER reached Connected. A lease that never
     * has is "Connecting…" on every surface; only a lease that connected and
     * then dropped may honestly read as "Reconnecting". Set by
     * ControllerConnected (or a room update that itself says Connected) and
     * carried across room updates of the same phone + key. */
    bool everConnected = false;
    /* F1: the phone renamed itself over its live control channel
     * (controller_rename -> ControllerRenamed). Room updates still carry the
     * redeem-time name, so the applied rename is carried across them while
     * the id + key still describe the same phone. */
    bool renamed = false;
    /* I2: the phone's controller page completed the WebRTC handshake but
     * spoke a different pairing-protocol version, so its input can never be
     * trusted. The seat keeps its lease (the room is not torn down) but must
     * show this honestly instead of an indistinguishable "Reconnecting".
     * Cleared by a genuine ControllerConnected -- the phone reloading into a
     * matching page version is the recovery path. */
    bool protocolMismatch = false;
    /* C1 self-heal: set when an ingress push failed (queue overflow revoked
     * the seat's custody) while otherwise healthy. service() clears it once
     * the seat has a fresh bind; lastRebindMs rate-limits repeated attempts
     * so a wedged phone cannot spin the rebind every tick. */
    bool needsRebind = false;
    uint64_t lastRebindMs = 0u;
    /* RTT: the newest control-channel ping round trip for THIS channel, in
     * milliseconds; 0 means no sample. Set per matched pong
     * (ControllerRtt), carried across room updates of the same
     * connectionSequence only, cleared when the channel ends or demotes
     * (disconnect, protocol mismatch, new sequence) -- an old channel's
     * number never vouches for a new one. */
    unsigned rttMs = 0u;
};

/* I2: the one sentence every mismatch surface shows -- the room message
 * MdkrNativePartyHost::applyEvent sets and the seat row ui_phone_party.cpp
 * derives from protocolMismatch. Both screens were designed to give the
 * same remedy, so the copy lives once, here, where the flag it narrates is
 * declared; tests/test_native_party_host.cpp pins the sentence itself. */
inline constexpr char kMdkrPartyProtocolMismatchCopy[] =
    "This phone's controller page is a different version. "
    "Refresh the page on the phone.";

/* I4: the one terminal sentence for a room that is gone for good -- set by
 * MdkrNativePartyHost::applyEvent on RoomGone and carried on the transport's
 * event for any other listener. Its remedy must stay in lockstep with the
 * UI's RoomEnded surface (ui_phone_party.cpp offers Create New Invite, a
 * fresh open() into a brand-new room); tests/test_native_party_host.cpp
 * pins the sentence itself. */
inline constexpr char kMdkrPartyRoomEndedCopy[] =
    "This controller room has ended. Create a new invite to keep playing.";

/* F4: the one diagnosis sentence for a phone the C3 offer ladder gave up on
 * WHILE the room socket was healthy -- signaling delivered every offer, so
 * what failed is the phone-to-display path itself. All three surfaces (this
 * launcher, the browser host, the phone page) speak these exact bytes. TURN
 * has been server-delivered since wave 0, so this fires rarely; that is
 * precisely why the honest, specific copy matters when it does. */
inline constexpr char kMdkrPartyIceBlockedCopy[] =
    "This network blocks phone-to-display connections. "
    "Try another Wi-Fi network or a phone hotspot.";

/* F4 selection, used by the cloud transport's give-up site (tick()): only a
 * give-up whose whole ladder ran against a healthy room socket may claim
 * the network-blocked diagnosis; with the socket down, nothing about the
 * direct path was proven and the generic remedy stays. Inline so the
 * host-model test binary pins both branches without linking the
 * socket-owning transport. */
inline const char *mdkr_party_give_up_copy(bool roomSocketHealthy) {
    return roomSocketHealthy
        ? kMdkrPartyIceBlockedCopy
        : "This phone could not connect. Remove it and pair again.";
}

struct MdkrNativePartyView {
    MdkrNativePartyPhase phase = MdkrNativePartyPhase::Closed;
    std::string controllerUrl;
    std::string fallbackCode;
    std::string message;
    uint64_t inviteExpiresAtMs = 0u;
    uint64_t transitionId = 0u;
    unsigned inviteGeneration = 0u;
    std::vector<MdkrNativePartyController> controllers;
    bool inviteVisible = false;
    bool busy = false;
};

enum class MdkrPartyTransportEventType {
    RoomState,
    ControllerConnected,
    ControllerDisconnected,
    ControllerPacket,
    ControllerPhrase,
    /* F1: the phone's controller_rename from its authenticated control
     * channel; `controllerId` names the seat and `message` carries the new
     * name verbatim. The host model is the validation boundary
     * (native_party_host.cpp validRenameName). */
    ControllerRenamed,
    ControllerProtocolMismatch,
    /* RTT: one matched control-channel pong's round trip (event.rttMs).
     * Droppable in the shared queue -- a lost sample is replaced by the
     * next pong and mutates nothing but a cosmetic number. */
    ControllerRtt,
    CommandRejected,
    Recovering,
    Error,
    /* I4: the service refused to resume this room for good (see
     * mdkr_party_resume_decide in party_retry_policy.h for the exact
     * classification). Emitted at most once per room; the transport opens
     * no further sockets after it. */
    RoomGone,
    Closed,
};

struct MdkrPartyTransportRoomState {
    uint64_t transitionId = 0u;
    unsigned inviteGeneration = 0u;
    /* Relative milliseconds remaining as of when the transport parsed this
     * room update, NOT an absolute instant. The transport and the host each
     * run their own clock (transport: std::chrono::steady_clock since boot;
     * host: SDL_GetTicks64 since SDL init) -- an absolute value here would
     * cross domains and be meaningless once compared against the host's own
     * nowMs. The host latches nowMs + inviteExpiresInMs in its own clock at
     * the event-application site instead. */
    uint64_t inviteExpiresInMs = 0u;
    std::string controllerUrl;
    std::string fallbackCode;
    bool inviteActive = false;
    std::vector<MdkrNativePartyController> controllers;
};

struct MdkrPartyTransportEvent {
    MdkrPartyTransportEventType type = MdkrPartyTransportEventType::Error;
    MdkrPartyTransportRoomState room;
    std::string controllerId;
    std::string message;
    /* CommandRejected only: the worker's typed host_command_result error
     * code, verbatim (services/party/src/party-room.ts commandError).
     * Empty means unknown; the host then keeps its generic copy. */
    std::string errorCode;
    /* CommandRejected only: the worker's echoed name of the command that
     * failed ("approve", "rotate", ...). Together with controllerId this is
     * the failed command's identity (party-room.ts commandIdentity): it lets
     * the host clear exactly the state that was waiting on that command --
     * one controller's pending flag, or the room-level busy flag for
     * rotate/revoke/close -- instead of clearing the whole room. Empty means
     * the sender predates the identity echo; the host then falls back to its
     * conservative room-wide cleanup. */
    std::string command;
    /* ControllerProtocolMismatch only: the protocol version the phone's
     * controller_ready declared. Zero means it declared none at all. */
    unsigned theirProtocol = 0u;
    /* ControllerRtt only: the matched pong's round trip in milliseconds,
     * floored at 1 so "measured, just fast" is distinguishable from the
     * model's 0 = no sample. */
    unsigned rttMs = 0u;
    std::vector<uint8_t> packet;
    bool haptics = false;
};

/*
 * Implementations own HTTPS/WSS/WebRTC and their callback queue. Every method
 * below is called on the launcher thread. poll() copies one bounded event out;
 * no implementation callback may call the host model directly.
 */
class MdkrPartyTransport {
public:
    virtual ~MdkrPartyTransport() = default;
    virtual bool available() const = 0;
    virtual const char *unavailableReason() const = 0;
    /*
     * Transport-selection seam. Whether this transport's invite origin must be
     * the compiled secure (https) Party origin -- the fail-closed default, so
     * the cloud transport and every test fake keep the M2 canonical-origin
     * policy unchanged (a non-https CLOUD open still refuses, and a room_state
     * whose controllerUrl is not https is still rejected). The LAN transport
     * overrides this to false: its invite is an http://<lan-ip>:<port> the
     * phones reach with no internet at all, legitimately outside the
     * compiled-origin trust model. The host consults this at open() and when
     * validating a room update's controllerUrl, and nowhere else.
     */
    virtual bool requiresSecureOrigin() const { return true; }
    virtual bool open(const std::string &serviceOrigin) = 0;
    virtual bool approve(const std::string &controllerId, unsigned seat) = 0;
    virtual bool reject(const std::string &controllerId) = 0;
    virtual bool remove(const std::string &controllerId) = 0;
    /*
     * P2.1 compare-then-trust: the human confirmed this phone's pairing phrase
     * matches on both screens (Words Match). Tell the phone it is trusted over
     * its own reliable control channel (the seat_confirmed message) so it may
     * leave the compare screen and run the auto input test, and start honoring
     * its input_test round trips. Best-effort and idempotent: the ingress seat
     * custody is granted by the host model, not by this call, so a transport
     * that cannot deliver the message never leaves a seat wrongly held. The
     * default no-op keeps every test fake and the unavailable stub building;
     * the two shipping transports override it and stay byte-alike (twin rule).
     */
    virtual bool confirm(const std::string &controllerId) {
        (void)controllerId;
        return true;
    }
    virtual bool rotateInvite(unsigned expectedGeneration) = 0;
    virtual bool revokeInvite() = 0;
    virtual bool closeRoom() = 0;
    virtual bool sendRumble(
        const std::string &controllerId, uint16_t strength) = 0;
    virtual bool poll(MdkrPartyTransportEvent &event) = 0;
    virtual void shutdown() = 0;
};

/*
 * Loopback test gate for the end-to-end lane (tests/check_party_native_e2e.py).
 * True only when BOTH hold: `url` is a plain-HTTP loopback URL — it starts
 * with `http://127.0.0.1` or `http://localhost` followed by nothing, `:` or
 * `/` (so `http://127.0.0.1.evil.example` never matches) — AND the process
 * carries MDKR_INTERNAL_TEST_TOKEN=mdkr64-party-e2e-v1. Without the token
 * every HTTP origin, loopback included, stays refused exactly as before, so
 * the shipped fail-closed HTTPS-only posture is unchanged. This mirrors the
 * Party service itself, which accepts plain HTTP solely for
 * standards-defined loopback development (services/party/src/security.ts).
 * Inline because the host model and the transport check it independently
 * and are linked in different combinations by different test binaries.
 */
inline bool mdkr_party_loopback_test_url_allowed(const std::string &url) {
    const char *token = std::getenv("MDKR_INTERNAL_TEST_TOKEN");
    if (token == nullptr ||
        std::strcmp(token, "mdkr64-party-e2e-v1") != 0) {
        return false;
    }
    for (const char *prefix : {"http://127.0.0.1", "http://localhost"}) {
        const size_t length = std::strlen(prefix);
        if (url.compare(0u, length, prefix) != 0) continue;
        /* Host boundary: the loopback name must end here, at a port, or at
         * a path — never continue into a longer hostname. */
        if (url.size() == length) return true;
        const char next = url[length];
        if (next != ':' && next != '/') continue;
        /* The rest of the authority must not smuggle a real host behind
         * userinfo: RFC 3986 reads "http://127.0.0.1:@evil.example/..." as
         * userinfo "127.0.0.1:" at host evil.example. Allow the one port
         * colon consumed above and refuse any '@' or further ':' before
         * the path begins. */
        for (size_t index = length + (next == ':' ? 1u : 0u);
             index < url.size(); index++) {
            const char byte = url[index];
            if (byte == '/') break;
            if (byte == '@' || byte == ':') return false;
        }
        return true;
    }
    return false;
}

/*
 * M2 canonical-origin gate for the compiled Party service origin: exactly
 * `https://` + lowercase host + one optional explicit `:port`, nothing else.
 * The value is interpolated into invite URLs and compared byte-for-byte
 * against the service's PARTY_ORIGIN, so a path, query, fragment, trailing
 * slash or userinfo is a misconfiguration, not a softer origin. The
 * character-by-character walk matches the loopback gate above: the allowed
 * host alphabet excludes '@', so RFC 3986's
 * "https://host:@evil.example/..." userinfo reading (userinfo "host:" at
 * host evil.example) can never smuggle a different authority past the
 * check, and the digits-only port rule refuses a second ':' the same way.
 * Host labels follow DNS shape -- non-empty, [a-z0-9-], no leading or
 * trailing '-', joined by single dots, no trailing dot -- which keeps
 * IDN punycode ("xn--...") accepted while "..", uppercase and bracketed
 * IPv6 literals stay refused. CMakeLists.txt enforces the same rule at
 * configure time (MDKR_PARTY_ORIGIN); this is the runtime twin for the
 * value the build actually compiled in. Inline for the same reason as the
 * loopback gate: the host model and the transport link in different
 * combinations across test binaries.
 */
inline bool mdkr_party_canonical_https_origin(const std::string &origin) {
    static const char kScheme[] = "https://";
    const size_t schemeLength = sizeof(kScheme) - 1u;
    if (origin.size() <= schemeLength ||
        origin.compare(0u, schemeLength, kScheme) != 0) {
        return false;
    }
    size_t index = schemeLength;
    size_t labelLength = 0u;
    char previous = '\0';
    while (index < origin.size() && origin[index] != ':') {
        const char byte = origin[index];
        if (byte >= 'a' && byte <= 'z') {
            labelLength++;
        } else if (byte >= '0' && byte <= '9') {
            labelLength++;
        } else if (byte == '-') {
            if (labelLength == 0u) return false;  /* label starts with '-' */
            labelLength++;
        } else if (byte == '.') {
            if (labelLength == 0u || previous == '-') return false;
            labelLength = 0u;
        } else {
            return false;  /* '/', '?', '#', '@', uppercase, anything else */
        }
        previous = byte;
        index++;
    }
    if (labelLength == 0u || previous == '-') return false;
    if (index == origin.size()) return true;  /* https://host */
    /* One explicit port: 1-65535, digits only, no leading zero. */
    index++;  /* the ':' */
    if (index == origin.size() || origin[index] == '0') return false;
    unsigned port = 0u;
    for (; index < origin.size(); index++) {
        const char byte = origin[index];
        if (byte < '0' || byte > '9') return false;
        port = port * 10u + static_cast<unsigned>(byte - '0');
        if (port > 65535u) return false;
    }
    return true;
}

/*
 * F1 flood guard: per-peer admission for controller_rename at the transport
 * boundary, BEFORE any event exists. ControllerRenamed is a non-droppable
 * event in the shared transport queue (a rename mutates the host's model and
 * must never vanish silently), which is only safe if a hostile phone cannot
 * mint them at wire speed -- otherwise a rename flood evicts and then
 * refuses the droppable pad packets every other seat depends on. A rename
 * is a human act, so the budget is humane and tiny: a repeat of the last
 * admitted name is dropped outright (the browser host dedupes on value the
 * same way), and at most kMdkrPartyRenameWindowMessages fresh names are
 * admitted per kMdkrPartyRenameWindowMs fixed window -- the same
 * fixed-window shape as the room model's per-socket signal throttle
 * (kMdkrLanPartySignalWindow* / worker admitSignalMessage), scaled down to
 * this one message class. Refusal costs no queue slot. Inline because both
 * transports keep one gate per peer and must stay byte-alike (twin rule);
 * tests/test_native_party_host.cpp pins the semantics and
 * tests/test_party_event_queue.cpp pins the no-starvation outcome.
 */
inline constexpr uint64_t kMdkrPartyRenameWindowMs = 3000u;
inline constexpr unsigned kMdkrPartyRenameWindowMessages = 3u;

struct MdkrPartyRenameGate {
    std::string lastName;
    bool named = false;
    uint64_t windowStartedAtMs = 0u;
    unsigned windowMessages = 0u;
};

inline bool mdkr_party_rename_admit(MdkrPartyRenameGate &gate,
                                    const std::string &name,
                                    uint64_t nowMs) {
    if (gate.named && gate.lastName == name) return false;
    if (nowMs - gate.windowStartedAtMs >= kMdkrPartyRenameWindowMs) {
        gate.windowStartedAtMs = nowMs;
        gate.windowMessages = 0u;
    }
    if (gate.windowMessages >= kMdkrPartyRenameWindowMessages) return false;
    gate.windowMessages++;
    gate.named = true;
    gate.lastName = name;
    return true;
}

/*
 * F11: the six-digit invite code, grouped for DISPLAY as two groups of three
 * ("123 456"). Grouping is presentation only -- every input path still takes
 * the raw six digits -- so anything that is not exactly six digits is
 * returned unchanged rather than inventing structure for it. Inline because
 * the launcher surface (ui_phone_party.cpp) is the caller and
 * tests/test_native_party_host.cpp pins the shape.
 */
inline std::string mdkr_party_grouped_fallback_code(const std::string &code) {
    if (code.size() != 6u) return code;
    for (char byte : code) {
        if (byte < '0' || byte > '9') return code;
    }
    return code.substr(0u, 3u) + " " + code.substr(3u);
}

/*
 * Item 4: a phone that redeems appears as a new Pending controller. The
 * launcher has no audio path (a grep of platform/app + platform/party finds no
 * sound hook), so the surface flashes a VISUAL attention cue -- a pulsing
 * pending card and a brief in-game indicator line -- once per NEW pending id:
 * never re-fired for an id already noticed, never on approval or any other
 * transition. This is the pure detection twin the UI (ui_phone_party.cpp)
 * drives. The first observation primes -- it adopts the current pending set
 * without firing -- so opening the manage overlay onto an existing room never
 * flashes a stale cue. Returns the count of newly-pending ids this observation
 * and updates `seen` to exactly the pending ids present now, so a phone that
 * leaves and later re-requests fires again. Whether native UI cues should ever
 * become audible is an owner decision (no launcher audio subsystem exists to
 * reuse, and inventing one is out of scope). Inline for the same reason as the
 * other host-model helpers; tests/test_native_party_host.cpp pins the semantics.
 */
struct MdkrPartyPendingAttention {
    std::vector<std::string> seen;
    bool primed = false;
};

inline unsigned mdkr_party_note_new_pending(
        MdkrPartyPendingAttention &state,
        const std::vector<MdkrNativePartyController> &controllers) {
    std::vector<std::string> current;
    for (const auto &controller : controllers) {
        if (controller.phase == MdkrNativePartyControllerPhase::Pending) {
            current.push_back(controller.id);
        }
    }
    unsigned fresh = 0u;
    if (state.primed) {
        for (const auto &id : current) {
            bool known = false;
            for (const auto &prior : state.seen) {
                if (prior == id) { known = true; break; }
            }
            if (!known) fresh++;
        }
    }
    state.seen = current;
    state.primed = true;
    return fresh;
}

class MdkrNativePartyHost {
public:
    explicit MdkrNativePartyHost(MdkrPartyTransport &transport);
    ~MdkrNativePartyHost();

    MdkrNativePartyHost(const MdkrNativePartyHost &) = delete;
    MdkrNativePartyHost &operator=(const MdkrNativePartyHost &) = delete;

    bool open(const std::string &serviceOrigin);
    bool approve(const std::string &controllerId, unsigned seat);
    /* P2.1: the human's Words Match decision for a provisionally-connected
     * phone. Grants seat custody (binds the ingress now) and tells the phone
     * it is trusted. Refused unless the phone holds a seat and its
     * channel-bound phrase is actually on screen to compare -- Words Differ is
     * the ordinary reject()/remove path, so a mismatch never reaches here. */
    bool confirmPairing(const std::string &controllerId);
    bool reject(const std::string &controllerId);
    bool rotateInvite();
    bool dismissInvite();
    bool closeRoom();

    /* F6: the UI calls this on every frame it actually draws the invite
     * card (QR + code). While the card is on screen, service() rotates the
     * invite on its own once ~75% of its TTL has elapsed, so a displayed
     * code is always redeemable. The TTL itself never lengthens (binding
     * security decision): an invite nobody displays expires exactly as
     * before, and perceived permanence comes only from rotation. */
    void noteInviteDisplayed(uint64_t nowMs);

    /* Drain bounded network events and newest engine rumble requests. */
    void service(uint64_t nowMs);
    const MdkrNativePartyView &view() const { return view_; }
    bool transportAvailable() const { return transport_.available(); }

private:
    MdkrNativePartyController *controller(const std::string &id);
    const MdkrNativePartyController *controller(const std::string &id) const;
    bool roomStateValid(const MdkrPartyTransportRoomState &room) const;
    void applyRoomState(const MdkrPartyTransportRoomState &room, uint64_t nowMs);
    void applyEvent(const MdkrPartyTransportEvent &event, uint64_t nowMs);
    void releaseController(const MdkrNativePartyController &controller);
    void releaseAll();
    void setError(const std::string &message);
    void setTerminal(MdkrNativePartyPhase phase, const std::string &message);

    MdkrPartyTransport &transport_;
    MdkrNativePartyView view_;
    /* F6 auto-rotate state: when the UI last reported the invite card on
     * screen (host service clock; 0 = never), and the full TTL the current
     * invite generation arrived with -- the 75% mark is measured against
     * the generation's ORIGINAL TTL, not whatever remained when some later
     * same-generation room update happened to arrive. */
    uint64_t inviteDisplayedAtMs_ = 0u;
    uint64_t inviteTtlMs_ = 0u;
    /* M5 sustained rumble: per-seat timestamp (service()'s own nowMs clock)
     * of the last rumble command actually sent, rate-limiting the 200 ms
     * refresh loop. Timestamps only, never strengths -- each refresh
     * re-reads the engine mailbox, so nothing the host remembers can keep a
     * motor running that the engine has stopped. */
    std::array<uint64_t, 4> lastRumbleSentMs_{};
};

#endif /* MDKR_NATIVE_PARTY_HOST_H */
