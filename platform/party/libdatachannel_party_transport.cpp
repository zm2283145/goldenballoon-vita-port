#include "libdatachannel_party_transport.h"
#include "mozilla_ca_bundle.h"
#include "party_event_queue.h"
#include "party_callback_identity.h"
#include "party_peer_setup_retry.h"
#include "party_retry_policy.h"
#include "party_webrtc_signaling.h"

#include <nlohmann/json.hpp>
#include <rtc/rtc.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace {

using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;
constexpr size_t kMaxQueuedEvents = 128u;
constexpr size_t kMaxSignalBytes = 64u * 1024u;
constexpr size_t kMaxControlBytes = 4096u;
/*
 * Channel protocol: the version tag inside the direct data-channel messages
 * (controller_ready/host_ready/ping/pong/rumble). This is a SEPARATE
 * namespace from the HTTP redemption "pairing protocol" (room-model.ts,
 * now 2 = SAS v2) and from the Worker-internal x-mdkr-internal-api header
 * version. The pairing protocol and the internal header gate share the
 * protocol_update_required refusal token; a channel mismatch instead sets
 * the protocolMismatch latch and shows kMdkrPartyProtocolMismatchCopy
 * (native_party_host.h). SAS v2 changed no channel message shape, so this
 * stays 1.
 */
constexpr unsigned kProtocol = 1u;

/*
 * Shared signaling primitives, extracted to party_webrtc_signaling.{h,cpp} so
 * the cloud and LAN transports derive the SAS phrase, capture DTLS
 * fingerprints, decode keys and read signaling JSON byte-for-byte alike. The
 * using-declarations keep this file's call sites unchanged; the definitions
 * now live once. kProtocol above stays local (host_ready/ping/pong/rumble
 * sends) and equals mdkr_party::kChannelProtocol by construction.
 */
using PartyIdentity = mdkr_party::Identity;
using mdkr_party::canonicalSdpFingerprint;
using mdkr_party::commandRejectionFromSignal;
using mdkr_party::controllerReadyEventFromControl;
using mdkr_party::decodePublicKey;
using mdkr_party::safeString;
using mdkr_party::uintValue;

/*
 * Signaling URL construction, shared by the create and reconnect sockets and
 * pinned by tests/test_native_party_sas.cpp through
 * mdkr_party_signaling_url_for_test. An https origin becomes wss and the
 * loopback test origin (gated in initialize() below) becomes plain ws. The
 * previous in-place rewrite here replaced the first five characters with
 * "wss:", which turned every https origin into "wss:://host/..." — a URL
 * libdatachannel's RFC 3986 parser rejects outright, so the production
 * socket could never even begin its handshake. Splitting on the exact
 * scheme keeps both forms honest.
 */
std::string signalingUrl(const std::string &origin, const std::string &path) {
    std::string url = origin;
    if (url.rfind("https://", 0u) == 0u) {
        url.replace(0u, 8u, "wss://");
    } else {
        url.replace(0u, 7u, "ws://");
    }
    return url + path;
}

uint64_t steadyNowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        Clock::now().time_since_epoch()).count());
}

/* M4 goodbye flush. closeRoom()'s send only QUEUES the close command:
 * rtc::WebSocket::send hands the frame to libdatachannel's writer and
 * returns, and the caller (the host model's closeRoom() and destructor
 * both) tears the socket down right after. Completion signal, verified
 * against the pinned dep (mdkr_libdatachannel-src): rtc::Channel::
 * bufferedAmount() is the "total size buffered to send"
 * (include/rtc/channel.hpp:34); for a WebSocket it is wired to the TCP
 * layer (src/impl/websocket.cpp:235, onBufferedAmount ->
 * triggerBufferedAmount), which adds each message on send
 * (src/impl/tcptransport.cpp:134) and subtracts only as bytes actually
 * reach the OS socket (src/impl/tcptransport.cpp:315,320). So
 * bufferedAmount()==0 after our send means every queued byte -- the close
 * frame included -- has left the process; there is no per-send completion
 * callback in this API, so a short poll against that counter is the whole
 * mechanism. The loop is bounded twice over: by the total sleep it
 * requests AND by the observed wall clock (an oversleeping OS cannot
 * stretch it), because quit must never block past the deadline.
 * tests/test_native_party_sas.cpp drives this exact loop through
 * mdkr_party_close_flush_wait_for_test with a fake clock. */
constexpr uint64_t kCloseFlushPollMs = 5u;

uint64_t closeFlushWait(const std::function<uint64_t()> &nowMs,
                        const std::function<size_t()> &bufferedBytes,
                        const std::function<void(uint64_t)> &sleepMs) {
    constexpr uint64_t deadline = kMdkrPartyCloseFlushDeadlineMs;
    uint64_t slept = 0u;
    const uint64_t start = nowMs();
    while (bufferedBytes() > 0u && slept < deadline) {
        const uint64_t elapsed = nowMs() - start;
        if (elapsed >= deadline) break;
        const uint64_t nap = std::min(
            kCloseFlushPollMs, std::min(deadline - slept, deadline - elapsed));
        sleepMs(nap);
        slept += nap;
    }
    return slept;
}

uint64_t wallExpiryToSteady(uint64_t expiresAtMs) {
    const uint64_t wall = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    const uint64_t now = steadyNowMs();
    return expiresAtMs > wall ? now + (expiresAtMs - wall) : now;
}

/*
 * Server-delivered iceServers (services/party/src/turn.ts). The bootstrap
 * may carry the room's iceServers -- STUN always, TURN credentials when the
 * service minted them -- and createPeer prefers a fully valid list over the
 * baked-in STUN below. Strictly validated and rebuilt here; any shortfall
 * discards the whole list, because connectivity config is best effort and
 * must never fail the bootstrap that carried it.
 */
constexpr size_t kMaxIceServerEntries = 8u;
constexpr size_t kMaxIceServersTotal = 16u;
constexpr size_t kMaxIceUrlBytes = 256u;
constexpr size_t kMaxIceSecretBytes = 512u;
const char kFallbackStunUrl[] = "stun:stun.cloudflare.com:3478";

bool validIceUrl(const std::string &url) {
    if (url.size() > kMaxIceUrlBytes) return false;
    if (url.rfind("stun:", 0u) != 0u && url.rfind("turn:", 0u) != 0u &&
        url.rfind("turns:", 0u) != 0u) {
        return false;
    }
    for (const char byte : url) {
        if (byte <= ' ' || byte > '~') return false;
    }
    return true;
}

std::vector<MdkrPartyIceServer> iceServersFromSignal(const Json &value) {
    const auto found = value.find("iceServers");
    if (found == value.end() || !found->is_array() || found->empty() ||
        found->size() > kMaxIceServerEntries) {
        return {};
    }
    std::vector<MdkrPartyIceServer> servers;
    for (const Json &entry : *found) {
        if (!entry.is_object() || !entry.contains("urls")) return {};
        std::vector<std::string> urls;
        if (entry["urls"].is_array()) {
            if (entry["urls"].empty() ||
                entry["urls"].size() > kMaxIceServerEntries) return {};
            for (const Json &url : entry["urls"]) {
                if (!url.is_string()) return {};
                urls.push_back(url.get<std::string>());
            }
        } else {
            std::string url;
            if (!safeString(entry, "urls", url, kMaxIceUrlBytes) ||
                url.empty()) return {};
            urls.push_back(std::move(url));
        }
        std::string username;
        std::string credential;
        if (!safeString(entry, "username", username, kMaxIceSecretBytes, false) ||
            !safeString(entry, "credential", credential, kMaxIceSecretBytes,
                false) ||
            username.empty() != credential.empty()) {
            return {};
        }
        for (const std::string &url : urls) {
            if (!validIceUrl(url) || servers.size() >= kMaxIceServersTotal) {
                return {};
            }
            /* Credentials are TURN-scoped: a credentialed entry naming any
             * non-turn/turns url refuses the whole list, the same rejection
             * the page validators apply. */
            if (!username.empty() && url.rfind("turn:", 0u) != 0u &&
                url.rfind("turns:", 0u) != 0u) {
                return {};
            }
            MdkrPartyIceServer server;
            server.url = url;
            server.username = username;
            server.credential = credential;
            servers.push_back(std::move(server));
        }
    }
    return servers;
}

std::vector<MdkrPartyIceServer> resolvedIceServers(
        std::vector<MdkrPartyIceServer> servers) {
    if (servers.empty()) {
        MdkrPartyIceServer fallback;
        fallback.url = kFallbackStunUrl;
        servers.push_back(std::move(fallback));
    }
    return servers;
}

struct Peer {
    std::string id;
    unsigned seat = 0u;
    uint32_t leaseGeneration = 0u;
    uint32_t connectionSequence = 0u;
    uint32_t peerGeneration = 0u;
    bool initializing = true;
    uint64_t admissionGeneration = 0u;
    bool failed = false;
    bool authenticated = false;
    /* F1 flood guard state, one per peer (native_party_host.h). */
    MdkrPartyRenameGate renameGate;
    /* I2: this peer's controller_ready declared a different channel-protocol
     * version. It is connected-but-wrong-version, not stranded: the C3
     * unanswered-offer ladder must leave it alone (no recreate, no resend,
     * no give-up -- none of those can close a version gap). Cleared only by
     * a later controller_ready that matches on this same channel; the usual
     * recovery -- the phone reloading into a matching page -- arrives as a
     * fresh peer (createPeer / room-state retirement), flag clear. */
    bool protocolMismatch = false;
    uint32_t pingNonce = 0u;
    Clock::time_point nextPingAt{};
    Clock::time_point pingOutstandingAt{};
    /* C3 signaling retry (party_retry_policy.h): steadyNowMs() the current
     * offer was last (re)sent, 0 meaning none sent yet; how many distinct
     * offers (not resends) have been sent so far; and whether this peer has
     * already been reported connect_timeout so tick() stops re-evaluating
     * and re-emitting it every pass. */
    uint64_t offerSentMs = 0u;
    unsigned offerAttempts = 0u;
    bool gaveUp = false;
    std::shared_ptr<rtc::PeerConnection> connection;
    std::shared_ptr<rtc::DataChannel> state;
    std::shared_ptr<rtc::DataChannel> control;
};

struct ControllerAdmission {
    MdkrNativePartyController controller;
    MdkrPartyPeerSetupRetry setup;
    uint64_t setupGeneration = 0u;
};

class TransportState final : public std::enable_shared_from_this<TransportState> {
public:
    bool initialize(const std::string &serviceOrigin) {
        if (!identity_.generate()) return false;
        origin_ = serviceOrigin;
        while (!origin_.empty() && origin_.back() == '/') origin_.pop_back();
        /* The host has already refused anything that is not HTTPS or the
         * token-gated loopback test origin; re-checking here keeps this
         * transport fail-closed even if it is ever driven directly. */
        if (origin_.rfind("https://", 0u) != 0u &&
            !mdkr_party_loopback_test_url_allowed(origin_)) {
            return false;
        }
        return connect(true);
    }

    bool command(const Json &message, uint64_t generation = 0u,
                 const std::shared_ptr<Peer> &peer = {}) {
        std::shared_ptr<rtc::WebSocket> socket;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!callbackCurrentLocked(generation, peer) ||
                (peer && peer->failed) || !socket_ || !socket_->isOpen()) return false;
            socket = socket_;
        }
        const std::string encoded = message.dump();
        if (encoded.size() > kMaxControlBytes) return false;
        bool sent = false;
        try { sent = socket->send(encoded); }
        catch (...) { return false; }
        if (!sent) return false;
        /* M7 socket-cap headroom: every message the Worker receives on this
         * socket counts toward its 512-lifetime-message hard close
         * (party-room.ts SIGNAL_LIFETIME_MESSAGES), and this is the one
         * choke point where the host sends any. Count per socket and, 32
         * messages short of the cap, ask tick() to recycle the socket
         * through the same resume ladder a network drop uses. Only latched
         * for a resumable socket (credential_ set): the short-lived create
         * socket has no resume path to cycle through. The latch is consumed
         * on the launcher thread, not here -- command() may be running
         * inside a libdatachannel callback, where closing the very socket
         * that is calling back invites a deadlock. */
        std::lock_guard<std::mutex> lock(mutex_);
        if (socket == socket_ && !shuttingDown_) {
            socketSentMessages_++;
            if (!credential_.empty() &&
                mdkr_party_socket_cycle_due(socketSentMessages_)) {
                socketCyclePending_ = true;
            }
        }
        return true;
    }

    /* M4: bounded flush of the just-queued close command -- see
     * closeFlushWait above for the completion signal and both bounds. Runs
     * on the launcher thread, only on the explicit close/quit path. */
    void flushCloseCommand() {
        std::shared_ptr<rtc::WebSocket> socket;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (shuttingDown_ || !socket_) return;
            socket = socket_;
        }
        closeFlushWait(
            []() { return steadyNowMs(); },
            [&socket]() -> size_t {
                /* A socket that dies mid-wait can never drain; report empty
                 * so quit stops paying for it. */
                try {
                    return socket->isOpen() ? socket->bufferedAmount() : 0u;
                } catch (...) {
                    return 0u;
                }
            },
            [](uint64_t ms) {
                std::this_thread::sleep_for(std::chrono::milliseconds(ms));
            });
    }

    bool sendRumble(const std::string &id, uint16_t strength) {
        std::shared_ptr<rtc::DataChannel> channel;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto found = peers_.find(id);
            if (found == peers_.end() || !found->second->control ||
                !found->second->control->isOpen()) return false;
            channel = found->second->control;
        }
        try {
            return channel->send(Json{{"type", "rumble"}, {"protocol", kProtocol},
                {"strength", strength},
                {"durationMs", strength > 0u ? 250u : 0u}}.dump());
        } catch (...) { return false; }
    }

    /* P2.2 in-race feedback: a bounded, change-driven race_state over the same
     * reliable control channel rumble uses. One-way like rumble/seat_confirmed
     * -- the phone updates its own readout and never answers. */
    bool sendRaceState(const std::string &id, const MdkrNativeRaceState &state) {
        std::shared_ptr<rtc::DataChannel> channel;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto found = peers_.find(id);
            if (found == peers_.end() || !found->second->control ||
                !found->second->control->isOpen()) return false;
            channel = found->second->control;
        }
        try {
            return channel->send(Json{{"type", "race_state"},
                {"protocol", kProtocol}, {"racing", state.racing},
                {"item", state.item_type}, {"itemLevel", state.item_level},
                {"itemQty", state.item_quantity},
                {"lap", state.lap}, {"laps", state.lap_total},
                {"pos", state.position}, {"field", state.field_size},
                {"countdown", state.countdown}, {"finished", state.finished},
                {"finishPos", state.finish_position}}.dump());
        } catch (...) { return false; }
    }

    /* P2.1 compare-then-trust: the host confirmed this controller (Words
     * Match). Remember it -- so a reconnecting peer is trusted again with no
     * second human step -- and, when its control channel is already live, tell
     * the phone now; controlOpened re-sends on any later (re)open. */
    bool confirm(const std::string &id) {
        std::shared_ptr<rtc::DataChannel> channel;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            confirmed_.insert(id);
            const auto found = peers_.find(id);
            if (found != peers_.end() && found->second->control &&
                found->second->control->isOpen()) {
                channel = found->second->control;
            }
        }
        if (channel) {
            try {
                channel->send(Json{{"type", "seat_confirmed"},
                    {"protocol", kProtocol}}.dump());
            } catch (...) { /* controlOpened re-sends; best effort */ }
        }
        return true;
    }

    bool poll(MdkrPartyTransportEvent &event) {
        tick();
        std::lock_guard<std::mutex> lock(mutex_);
        if (drainedIndex_ >= drained_.size()) {
            queue_.drainInto(drained_);
            drainedIndex_ = 0u;
            /* Log a burst only when its size changes, not once per drained
             * event: a sustained flood must not itself become a stderr
             * flood. */
            const uint64_t dropped = queue_.droppedPadPackets();
            if (dropped != loggedDroppedPadPackets_) {
                std::fprintf(stderr, "[PARTY-QUEUE] dropped=%llu\n",
                    static_cast<unsigned long long>(dropped));
                loggedDroppedPadPackets_ = dropped;
            }
        }
        if (drainedIndex_ >= drained_.size()) return false;
        event = std::move(drained_[drainedIndex_++]);
        return true;
    }

    void shutdown() {
        std::shared_ptr<rtc::WebSocket> socket;
        std::map<std::string, std::shared_ptr<Peer>> peers;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (shuttingDown_) return;
            shuttingDown_ = true;
            generation_++;
            socket = std::move(socket_);
            peers.swap(peers_);
            signaled_.clear();
            confirmed_.clear();
        }
        if (socket) socket->close();
        for (auto &entry : peers) {
            if (entry.second->connection) entry.second->connection->close();
        }
    }

private:
    // State/event admission shares mutex_ with retirement. Check at the actual
    // commit, not just callback entry before parsing or other out-of-lock work.
    bool callbackCurrentLocked(uint64_t generation = 0u,
                               const std::shared_ptr<Peer> &peer = {}) const {
        bool peerMatches = true;
        if (peer) {
            const auto found = peers_.find(peer->id);
            peerMatches = found != peers_.end() && found->second == peer;
            peerMatches = peerMatches && mdkr_party_peer_initialization_current(
                peer->initializing, generation_, peer->admissionGeneration);
        }
        return mdkr_party_callback_current(shuttingDown_, generation_, generation, peerMatches);
    }

    bool controllerCurrentLocked(const MdkrNativePartyController &controller) const {
        const auto found = controllers_.find(controller.id);
        return found != controllers_.end() &&
            sameControllerLifecycle(found->second.controller, controller);
    }

    static bool sameControllerLifecycle(const MdkrNativePartyController &current,
                                        const MdkrNativePartyController &observed) {
        return current.phase != MdkrNativePartyControllerPhase::Pending &&
            observed.phase != MdkrNativePartyControllerPhase::Pending &&
            current.id == observed.id && current.seat == observed.seat &&
            current.leaseGeneration == observed.leaseGeneration &&
            current.connectionSequence == observed.connectionSequence &&
            current.publicKey == observed.publicKey;
    }

    bool setupCurrentLocked(const MdkrNativePartyController &controller,
                            uint64_t setupGeneration, uint64_t revision) const {
        const auto found = controllers_.find(controller.id);
        return !roomGone_ && callbackCurrentLocked(setupGeneration) &&
            found != controllers_.end() &&
            sameControllerLifecycle(found->second.controller, controller) &&
            found->second.setupGeneration == setupGeneration &&
            mdkr_party_setup_owned(found->second.setup, revision);
    }

    void reportSetupExhaustedLocked(ControllerAdmission &admission) noexcept {
        if (!admission.setup.exhausted || admission.setup.exhaustionReported) return;
        try {
            MdkrPartyTransportEvent event;
            event.type = MdkrPartyTransportEventType::CommandRejected;
            event.controllerId = admission.controller.id;
            event.message = kMdkrPartySetupExhaustedCopy;
            queue_.push(std::move(event));
            admission.setup.exhaustionReported = true;
        } catch (...) { /* Keep the report pending if allocation failed. */ }
    }

    void enqueue(MdkrPartyTransportEvent event, uint64_t generation = 0u,
                 const std::shared_ptr<Peer> &peer = {}) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!callbackCurrentLocked(generation, peer) || (peer && peer->failed)) return;
        queue_.push(std::move(event));
    }

    // Called on the connect caller, never from a socket callback. Retire only
    // this attempt and invalidate its callbacks before closing outside mutex_.
    // Error reporting is best effort: allocation failure must not hide the
    // false return or strand the existing resume ladder.
    void connectionAttemptFailed(uint64_t generation,
                                 const std::shared_ptr<rtc::WebSocket> &socket,
                                 bool published) noexcept {
        std::shared_ptr<rtc::WebSocket> retired;
        bool report = false;
        bool recover = false;
        uint64_t reportGeneration = 0u;
        try {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (generation != 0u && generation == generation_ && !shuttingDown_) {
                    // open() can synchronously close before throwing. Preserve
                    // that callback's verdict rather than advancing twice.
                    const bool alreadyClosed = published && socket_ != socket;
                    ++generation_;
                    reportGeneration = generation_;
                    if (!alreadyClosed) {
                        retired = std::move(socket_);
                        creating_ = false;
                        socketSentMessages_ = 0u;
                        socketCyclePending_ = false;
                        resumeRejected_ = false;
                        report = !roomGone_;
                        if (!roomGone_ && !credential_.empty()) {
                            reconnectAttempt_ = std::min(reconnectAttempt_ + 1u, 6u);
                            const auto decision = mdkr_party_resume_decide(
                                reconnectAttempt_, false, resumeRejections_,
                                steadyNowMs(), firstResumeRejectedMs_);
                            reconnectAt_ = Clock::now() +
                                std::chrono::milliseconds(decision.delayMs);
                            recover = true;
                        }
                    }
                }
            }
            if (report) {
                MdkrPartyTransportEvent event;
                event.type = recover ? MdkrPartyTransportEventType::Recovering
                                     : MdkrPartyTransportEventType::Error;
                event.message = recover ? "Controller room reconnecting."
                    : "Could not create a secure phone controller room.";
                enqueue(std::move(event), reportGeneration);
            }
        } catch (...) { /* Recovery state precedes optional event allocation. */ }
        // Callback registration itself may have failed halfway. Generation
        // invalidation rejects callbacks that enter after this retirement.
        if (socket) {
            try { socket->close(); } catch (...) { /* destructor also retires */ }
        }
        retired.reset();
    }

    bool connect(bool create) noexcept {
        std::shared_ptr<rtc::WebSocket> socket;
        uint64_t generation = 0u;
        bool published = false;
        try {
            rtc::WebSocket::Configuration configuration;
            std::string url;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (shuttingDown_ || roomGone_ || socket_) return false;
                generation = ++generation_;
                // Snapshot callback-owned credentials/room data under the lock.
                configuration.protocols = {"gb-native-host-v1", "gb-control-v1",
                    create ? "gb-key." + identity_.publicKey() : "gb-host." + credential_};
                url = signalingUrl(origin_, create ? "/api/party/native-create"
                                                  : "/api/party/" + roomId_ + "/connect");
            }
            configuration.disableTlsVerification = false;
            configuration.caCertificatePemFile = std::string(
                reinterpret_cast<const char *>(kMdkrMozillaCaBundle),
                static_cast<size_t>(kMdkrMozillaCaBundleLength));
            configuration.connectionTimeout = std::chrono::seconds(10);
            configuration.pingInterval = std::chrono::seconds(15);
            configuration.maxOutstandingPings = 2;
            configuration.maxMessageSize = kMaxSignalBytes;
            socket = std::make_shared<rtc::WebSocket>(configuration);
            const std::weak_ptr<TransportState> weak = shared_from_this();
            socket->onOpen([weak, generation]() {
                if (auto state = weak.lock()) state->socketOpened(generation);
            });
            socket->onError([weak, generation](const std::string &reason) {
                if (auto state = weak.lock()) state->socketError(generation, reason);
            });
            socket->onClosed([weak, generation]() {
                if (auto state = weak.lock()) state->socketClosed(generation);
            });
            socket->onMessage([weak, generation](rtc::message_variant message) {
                if (auto state = weak.lock()) state->socketMessage(generation, message);
            });
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (shuttingDown_ || roomGone_ || generation != generation_ || socket_) return false;
                socket_ = socket;
                published = true;
                creating_ = create;
                // M7 headroom belongs to the newly published socket only.
                socketSentMessages_ = 0u;
                socketCyclePending_ = false;
            }
            socket->open(url);
            bool stillOwned = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                stillOwned = !shuttingDown_ && !roomGone_ &&
                    generation == generation_ && socket_ == socket;
            }
            if (!stillOwned) {
                // Public callers serialize open/shutdown. Keep this lower-level
                // transaction defensive too: a pre-open close of a Closed RTC
                // socket does not prevent its subsequent open() from starting.
                // Preserve any onClosed verdict and retire outside the lock.
                connectionAttemptFailed(generation, socket, published);
                return false;
            }
            return true;
        } catch (...) {
            connectionAttemptFailed(generation, socket, published);
            return false;
        }
    }

    void socketOpened(uint64_t generation) {
        std::vector<std::shared_ptr<Peer>> pendingOffers;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (generation != generation_ || shuttingDown_) return;
            reconnectAttempt_ = 0u;
            reconnectAt_ = Clock::time_point{};
            /* I4: a completed upgrade is the service saying yes -- the only
             * thing that ever resets the consecutive-refusal streak (and
             * its first-refusal timestamp) the resume policy weighs toward
             * its terminal verdict. */
            resumeRejected_ = false;
            resumeRejections_ = 0u;
            firstResumeRejectedMs_ = 0u;
            /* C3: the socket just (re)opened. Any still-unauthenticated
             * peer with an outstanding offer may have had that offer
             * dropped by the Worker while the socket was down -- resend it
             * now instead of waiting out the 20 s deadline. socketOpen=true
             * is edge-triggered by construction here: this runs once per
             * onOpen callback, never once per tick. */
            for (const auto &entry : peers_) {
                const std::shared_ptr<Peer> &peer = entry.second;
                if (!callbackCurrentLocked(generation, peer) || peer->failed || peer->gaveUp) continue;
                const MdkrPartyRetryDecision decision = mdkr_party_retry_decide(
                    steadyNowMs(), peer->offerSentMs, peer->offerAttempts,
                    peer->authenticated, peer->protocolMismatch,
                    /*socketOpen=*/true);
                if (decision.resendOffer) pendingOffers.push_back(peer);
            }
        }
        for (const auto &peer : pendingOffers) resendOffer(peer, generation);
    }

    void socketError(uint64_t generation, const std::string &reason) {
        bool fatal = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (generation != generation_ || shuttingDown_) return;
            fatal = creating_ && credential_.empty();
            /* I4 classification. libdatachannel folds EVERY HTTP-refused
             * WebSocket upgrade -- the Worker/room's 404 not_found for a
             * deleted or expired room, its 401 for a credential that can
             * never re-validate, alike -- into this one onError string; the
             * actual status code is logged and swallowed (wshandshake.cpp
             * throws on any non-101, wstransport.cpp catches it and fails
             * the handshake, websocket.cpp maps that state to this exact
             * text). Network-level failures arrive as different strings
             * ("TCP connection failed", "TLS connection failed",
             * "Connection timed out") and typed close frames lose their
             * code and reason entirely, so "the service itself refused this
             * resume handshake" is precisely -- and only -- this string on
             * a non-create socket. socketClosed(), which libdatachannel
             * fires right after this callback, feeds the latch to
             * mdkr_party_resume_decide. The load-bearing literals -- this
             * one and the network-level trio -- are pinned at configure
             * time against the dependency's own source
             * (cmake/datachannel.cmake), so a libdatachannel bump that
             * rewords them fails the build instead of silently reverting
             * gone-room detection to always-retry. */
            if (!creating_ && reason == "WebSocket connection failed") {
                resumeRejected_ = true;
            }
        }
        if (fatal) {
            MdkrPartyTransportEvent event;
            event.type = MdkrPartyTransportEventType::Error;
            event.message = "Could not create a secure phone controller room.";
            enqueue(std::move(event), generation);
        }
    }

    void socketClosed(uint64_t generation) {
        bool recover = false;
        bool gone = false;
        std::shared_ptr<rtc::WebSocket> retired;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (generation != generation_ || shuttingDown_) return;
            retired = std::move(socket_);
            ++generation_; // also revokes callbacks already parsing this socket
            /* I4: once the terminal verdict is in, later stray closes have
             * nothing to add -- and must not enqueue a Recovering/Error
             * that would overwrite the host's RoomEnded surface. */
            if (roomGone_) return;
            /* Consume the per-attempt refusal latch socketError set just
             * before this callback, and let the pure resume policy
             * (party_retry_policy.h) decide between the existing bounded
             * ladder and the terminal verdict. A network-level failure
             * (rejected == false) neither advances nor resets the streak:
             * it proved nothing about the room. roomGone_ latches so a
             * terminal room can never re-enter the ladder, whatever late
             * callbacks still fire. */
            const bool rejected = resumeRejected_;
            resumeRejected_ = false;
            if (!credential_.empty()) {
                const uint64_t nowSteadyMs = steadyNowMs();
                if (rejected) {
                    resumeRejections_++;
                    /* I-1: the streak's first refusal starts the 30 s
                     * wall-clock floor the terminal verdict must also
                     * clear. Same steady clock as everything else here. */
                    if (firstResumeRejectedMs_ == 0u) {
                        firstResumeRejectedMs_ = nowSteadyMs;
                    }
                }
                reconnectAttempt_ = std::min(reconnectAttempt_ + 1u, 6u);
                const MdkrPartyResumeDecision decision = mdkr_party_resume_decide(
                    reconnectAttempt_, rejected, resumeRejections_,
                    nowSteadyMs, firstResumeRejectedMs_);
                if (decision.terminal) {
                    roomGone_ = true;
                    reconnectAt_ = Clock::time_point{};
                    gone = true;
                } else if (decision.retry) {
                    reconnectAt_ = Clock::now() +
                        std::chrono::milliseconds(decision.delayMs);
                    recover = true;
                }
            }
            MdkrPartyTransportEvent event;
            if (gone) {
                event.type = MdkrPartyTransportEventType::RoomGone;
                event.message = kMdkrPartyRoomEndedCopy;
            } else {
                event.type = recover ? MdkrPartyTransportEventType::Recovering
                                     : MdkrPartyTransportEventType::Error;
                event.message = recover
                    ? "Controller room reconnecting."
                    : "Phone controller room closed before it was ready.";
            }
            // Already locked: do not call enqueue(), which acquires mutex_.
            queue_.push(std::move(event));
        }
    }

    void tick() {
        bool reconnect = false;
        /* F4: whether the room socket was healthy at the moment a give-up
         * verdict landed -- captured under the same lock, because the copy
         * fork (network-blocked diagnosis vs generic remedy) is only honest
         * about the socket state the ladder actually ran against. */
        bool giveUpSocketHealthy = false;
        std::shared_ptr<rtc::WebSocket> cycleSocket;
        std::vector<std::shared_ptr<Peer>> ping;
        struct ExpiredPing {
            std::shared_ptr<Peer> peer;
            Clock::time_point sentAt;
            uint32_t nonce;
        };
        std::vector<ExpiredPing> expired;
        struct PeerRecreation {
            MdkrNativePartyController controller;
            unsigned attempts;
            uint64_t offerSentMs;
            std::shared_ptr<Peer> expectedPeer;
        };
        std::vector<PeerRecreation> recreations;
        struct SetupRetry {
            MdkrNativePartyController controller;
            uint64_t generation;
            uint64_t revision;
        };
        std::vector<SetupRetry> setupRetries;
        const Clock::time_point now = Clock::now();
        const uint64_t nowMs = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                now.time_since_epoch()).count());
        {
            std::lock_guard<std::mutex> lock(mutex_);
            /* I4: !roomGone_ is belt-and-suspenders -- the terminal verdict
             * already cleared reconnectAt_ -- so a gone room can never open
             * another socket from here no matter what else runs. */
            reconnect = !shuttingDown_ && !roomGone_ && !socket_ &&
                !credential_.empty() &&
                reconnectAt_ != Clock::time_point{} && now >= reconnectAt_;
            if (reconnect) reconnectAt_ = Clock::time_point{};
            /* M7: consume the proactive-recycle latch command() set at the
             * headroom threshold. Consumed exactly once (the flag drops
             * here whatever happens next), on this thread, outside any
             * libdatachannel callback. */
            if (socketCyclePending_) {
                socketCyclePending_ = false;
                if (!shuttingDown_ && !roomGone_ && socket_) {
                    cycleSocket = socket_;
                }
            }
            for (const auto &entry : peers_) {
                const std::shared_ptr<Peer> &peer = entry.second;
                if (!callbackCurrentLocked(0u, peer) || peer->failed) continue;
                if (!peer->authenticated) {
                    /* C3: an unanswered offer never makes the PeerConnection
                     * reach Failed (no ICE start without a remote
                     * description), so this is the only place a silently
                     * stranded peer is ever noticed. gaveUp latches once the
                     * host has been told, so a given-up peer is never
                     * re-evaluated or re-reported every tick. */
                    if (!peer->gaveUp) {
                        const MdkrPartyRetryDecision decision = mdkr_party_retry_decide(
                            nowMs, peer->offerSentMs, peer->offerAttempts,
                            /*authenticated=*/false, peer->protocolMismatch,
                            /*socketOpen=*/false);
                        if (decision.giveUp) {
                            peer->gaveUp = true;
                            try {
                                giveUpSocketHealthy =
                                    socket_ && socket_->isOpen();
                            } catch (...) {
                                giveUpSocketHealthy = false;
                            }
                            // The verdict and publication are one locked commit:
                            // a ready callback cannot authenticate this same peer
                            // between the decision and an obsolete timeout event.
                            MdkrPartyTransportEvent event;
                            event.type = MdkrPartyTransportEventType::CommandRejected;
                            event.controllerId = peer->id;
                            event.message = mdkr_party_give_up_copy(giveUpSocketHealthy);
                            queue_.push(std::move(event));
                        } else if (decision.recreatePeer) {
                            const auto known = controllers_.find(peer->id);
                            if (known != controllers_.end()) {
                                recreations.push_back({known->second.controller, peer->offerAttempts,
                                                       peer->offerSentMs, peer});
                            }
                        }
                    }
                    continue;
                }
                if (!peer->control) continue;
                if (peer->pingOutstandingAt != Clock::time_point{} &&
                    now - peer->pingOutstandingAt >= std::chrono::seconds(15)) {
                    expired.push_back({peer, peer->pingOutstandingAt, peer->pingNonce});
                } else if (peer->pingOutstandingAt == Clock::time_point{} &&
                           peer->nextPingAt != Clock::time_point{} &&
                           now >= peer->nextPingAt) {
                    peer->pingNonce++;
                    peer->pingOutstandingAt = now;
                    peer->nextPingAt = now + std::chrono::seconds(5);
                    ping.push_back(peer);
                }
            }
            for (auto &entry : controllers_) {
                auto &admission = entry.second;
                if (roomGone_ || !callbackCurrentLocked(admission.setupGeneration) ||
                    admission.setupGeneration != generation_) continue;
                reportSetupExhaustedLocked(admission);
                if (mdkr_party_setup_due(admission.setup, nowMs)) {
                    setupRetries.push_back({admission.controller, admission.setupGeneration,
                                            admission.setup.revision});
                }
            }
        }
        /* M7: the proactive recycle IS the network-drop path from here on:
         * this close fires the socket's own onClosed, and socketClosed()'s
         * existing ladder schedules the resume that opens the replacement.
         * A clean close is not a refusal -- socketError never ran, so the
         * I4 refusal streak (resumeRejected_/resumeRejections_) is exactly
         * as untouched as it is for any live-socket drop; the policy side
         * of that is pinned in test_party_transport_retry.cpp. */
        if (cycleSocket) {
            try { cycleSocket->close(); } catch (...) {}
        }
        if (reconnect) (void)connect(false);
        for (const auto &recreation : recreations) {
            createPeer(recreation.controller, /*forceRecreate=*/true, recreation.attempts,
                       0u, recreation.expectedPeer, recreation.offerSentMs);
        }
        for (const auto &retry : setupRetries) {
            createPeer(retry.controller, false, 0u, retry.generation, {}, 0u, retry.revision);
        }
        for (const auto &expiration : expired) {
            peerDisconnected(expiration.peer, true, 0u, expiration.sentAt, expiration.nonce);
        }
        for (const auto &peer : ping) {
            try {
                if (!peer->control->isOpen() || !peer->control->send(
                        Json{{"type", "ping"}, {"protocol", kProtocol},
                            {"nonce", peer->pingNonce}}.dump())) {
                    peerDisconnected(peer, true);
                }
            } catch (...) { peerDisconnected(peer, true); }
        }
    }

    void socketMessage(uint64_t generation, const rtc::message_variant &message) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (generation != generation_ || shuttingDown_) return;
        }
        if (!std::holds_alternative<std::string>(message)) return;
        const std::string &text = std::get<std::string>(message);
        if (text.size() > kMaxSignalBytes) return;
        Json value = Json::parse(text, nullptr, false);
        if (value.is_discarded() || !value.is_object()) return;
        try {
            const std::string type = value.value("type", std::string{});
            if (type == "native_bootstrap") handleBootstrap(value, generation);
            else if (type == "room_state") handleRoomState(value, generation);
            else if (type == "controller_hello") handleHello(value, generation);
            else if (type == "webrtc_answer") handleAnswer(value, generation);
            else if (type == "webrtc_ice") handleIce(value, generation);
            else if (type == "host_command_result") {
                MdkrPartyTransportEvent event;
                if (commandRejectionFromSignal(value, event)) {
                    enqueue(std::move(event), generation);
                }
            }
        } catch (...) {
            MdkrPartyTransportEvent event;
            event.type = MdkrPartyTransportEventType::Error;
            event.message = "Controller service sent an invalid room update.";
            enqueue(std::move(event), generation);
        }
    }

    void handleBootstrap(const Json &value, uint64_t signalGeneration) {
        std::string room;
        std::string credential;
        std::string url;
        std::string code;
        uint64_t generation = 0u;
        uint64_t expiresIn = 0u;
        if (!safeString(value, "roomId", room, 22u) || room.size() != 22u ||
            !safeString(value, "hostCredential", credential, 43u) ||
            credential.size() != 43u || !safeString(value, "controllerUrl", url, 2048u) ||
            (url.rfind("https://", 0u) != 0u &&
             !mdkr_party_loopback_test_url_allowed(url)) ||
            !safeString(value, "fallbackCode", code, 6u) ||
            code.size() != 6u || !uintValue(value, "inviteGeneration", generation,
                std::numeric_limits<unsigned>::max()) || generation == 0u ||
            !uintValue(value, "inviteExpiresInMs", expiresIn, 300000u) || expiresIn == 0u) {
            MdkrPartyTransportEvent event;
            event.type = MdkrPartyTransportEventType::Error;
            event.message = "Controller room bootstrap was invalid.";
            enqueue(std::move(event), signalGeneration);
            return;
        }
        /* Optional field, validated separately: a missing or malformed list
         * resolves to the baked-in STUN in createPeer, never a refused
         * bootstrap (the service's own TURN degradation contract). */
        std::vector<MdkrPartyIceServer> iceServers = iceServersFromSignal(value);
        std::lock_guard<std::mutex> lock(mutex_);
        if (!callbackCurrentLocked(signalGeneration) || !credential_.empty()) return;
        roomId_ = std::move(room);
        credential_ = std::move(credential);
        inviteUrl_ = std::move(url);
        fallbackCode_ = std::move(code);
        inviteGeneration_ = static_cast<unsigned>(generation);
        inviteExpiresAtMs_ = steadyNowMs() + expiresIn;
        iceServers_ = std::move(iceServers);
    }

    struct ParsedInviteCache {
        bool replace = false;
        std::string url;
        std::string code;
        uint64_t expiresAtMs = 0u;
    };

    bool parseRoom(const Json &value, MdkrPartyTransportRoomState &room,
                   ParsedInviteCache &cache, uint64_t signalGeneration) {
        uint64_t transition = 0u;
        uint64_t generation = 0u;
        uint64_t wallExpiry = 0u;
        if (!uintValue(value, "transitionId", transition) || transition == 0u ||
            !uintValue(value, "inviteGeneration", generation,
                std::numeric_limits<unsigned>::max()) || generation == 0u ||
            !uintValue(value, "inviteExpiresAt", wallExpiry) ||
            !value.contains("controllers") || !value["controllers"].is_array() ||
            value["controllers"].size() > 8u) return false;
        room.transitionId = transition;
        room.inviteGeneration = static_cast<unsigned>(generation);
        /* I1: kept in this function's own steady-clock domain
         * (wallExpiryToSteady/steadyNowMs) throughout parsing -- only
         * translated to a cross-domain-safe *relative* value
         * (room.inviteExpiresInMs) once, right before returning, so the host
         * can anchor it in its own clock instead of this transport's. */
        uint64_t expiresAtSteadyMs = wallExpiryToSteady(wallExpiry);
        room.inviteActive = value.value("phase", std::string{}) == "open" &&
            expiresAtSteadyMs > steadyNowMs();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!callbackCurrentLocked(signalGeneration)) return false;
            if (room.inviteGeneration == inviteGeneration_) {
                room.controllerUrl = inviteUrl_;
                room.fallbackCode = fallbackCode_;
                expiresAtSteadyMs = inviteExpiresAtMs_;
            }
        }
        if (safeString(value, "controllerUrl", room.controllerUrl, 2048u, false) &&
            safeString(value, "fallbackCode", room.fallbackCode, 6u, false)) {
            if (value.contains("controllerUrl") && value.contains("fallbackCode")) {
                cache.replace = true;
                cache.url = room.controllerUrl;
                cache.code = room.fallbackCode;
                cache.expiresAtMs = expiresAtSteadyMs;
            }
        } else return false;
        for (const Json &item : value["controllers"]) {
            if (!item.is_object()) return false;
            MdkrNativePartyController controller;
            if (!safeString(item, "controllerId", controller.id, 64u) ||
                controller.id.empty() || !safeString(item, "name", controller.name, 48u) ||
                !safeString(item, "controllerPublicKey", controller.publicKey, 87u) ||
                controller.publicKey.size() != 87u) return false;
            const std::string phase = item.value("phase", std::string{});
            if (phase == "pending") controller.phase = MdkrNativePartyControllerPhase::Pending;
            else if (phase == "approved") controller.phase = MdkrNativePartyControllerPhase::Approved;
            else if (phase == "leased") controller.phase = MdkrNativePartyControllerPhase::Leased;
            else if (phase == "connected") controller.phase = MdkrNativePartyControllerPhase::Connected;
            else return false;
            uint64_t seat = 0u;
            /* I5: a pending controller with no seat assigned yet may arrive
             * with the "seat" key absent entirely, not merely null. nlohmann's
             * const operator[] on a missing key is undefined behaviour under
             * NDEBUG (an end-iterator dereference, not a catchable
             * exception) -- contains() first, same as every other field in
             * this function. */
            if (item.contains("seat") && !item["seat"].is_null() &&
                !uintValue(item, "seat", seat, 4u)) return false;
            uint64_t lease = 0u;
            uint64_t connection = 0u;
            if (!uintValue(item, "leaseGeneration", lease,
                    std::numeric_limits<uint32_t>::max()) ||
                !uintValue(item, "connectionSequence", connection,
                    std::numeric_limits<uint32_t>::max())) return false;
            controller.seat = static_cast<unsigned>(seat);
            controller.leaseGeneration = static_cast<uint32_t>(lease);
            controller.connectionSequence = static_cast<uint32_t>(connection);
            /* SAS v2: no phrase here. It binds both DTLS fingerprints,
             * which do not exist until this controller's WebRTC
             * descriptions are exchanged -- emitPhrase (from handleAnswer)
             * is the one derivation site. The key itself is still validated
             * as a curve point so a malformed room update fails closed
             * exactly as it always has. */
            std::array<uint8_t, 65> keyBytes{};
            if (!decodePublicKey(controller.publicKey, keyBytes)) return false;
            room.controllers.push_back(std::move(controller));
        }
        const uint64_t nowSteady = steadyNowMs();
        room.inviteExpiresInMs =
            expiresAtSteadyMs > nowSteady ? expiresAtSteadyMs - nowSteady : 0u;
        return true;
    }

    void handleRoomState(const Json &value, uint64_t signalGeneration) {
        MdkrPartyTransportEvent event;
        event.type = MdkrPartyTransportEventType::RoomState;
        ParsedInviteCache cache;
        if (!parseRoom(value, event.room, cache, signalGeneration)) {
            event.type = MdkrPartyTransportEventType::Error;
            event.message = "Controller service sent an invalid signed room update.";
            enqueue(std::move(event), signalGeneration);
            return;
        }
        std::vector<std::shared_ptr<Peer>> retired;
        const std::vector<MdkrNativePartyController> controllers = event.room.controllers;
        std::map<std::string, ControllerAdmission> roster;
        std::map<std::string, MdkrNativePartyController> rosterControllers;
        for (const auto &controller : controllers) {
            roster[controller.id].controller = controller;
            rosterControllers[controller.id] = controller;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!callbackCurrentLocked(signalGeneration)) return;
            if (cache.replace) {
                inviteUrl_.swap(cache.url);
                fallbackCode_.swap(cache.code);
                inviteGeneration_ = event.room.inviteGeneration;
                inviteExpiresAtMs_ = cache.expiresAtMs;
            }
            for (auto &entry : roster) {
                auto &admission = entry.second;
                const auto old = controllers_.find(entry.first);
                if (old != controllers_.end() &&
                    sameControllerLifecycle(old->second.controller, admission.controller)) {
                    admission.setup = old->second.setup;
                    if (old->second.setupGeneration != signalGeneration) {
                        mdkr_party_setup_reauthorize(admission.setup, steadyNowMs());
                    }
                }
                admission.setupGeneration = signalGeneration;
            }
            controllers_.swap(roster);
            for (const auto &controller : event.room.controllers) {
                const auto found = peers_.find(controller.id);
                const auto previous = roster.find(controller.id);
                const bool freshLifecycle = previous == roster.end() ||
                    !sameControllerLifecycle(previous->second.controller, controller);
                if (found != peers_.end()) {
                    if (freshLifecycle || found->second->seat != controller.seat ||
                        found->second->leaseGeneration != controller.leaseGeneration ||
                        found->second->connectionSequence != controller.connectionSequence ||
                        !mdkr_party_peer_initialization_current(found->second->initializing,
                            generation_, found->second->admissionGeneration)) {
                        retired.push_back(found->second);
                        peers_.erase(found);
                    }
                }
            }
            for (auto iterator = peers_.begin(); iterator != peers_.end();) {
                if (controllers_.count(iterator->first) == 0u) {
                    retired.push_back(iterator->second);
                    iterator = peers_.erase(iterator);
                } else {
                    ++iterator;
                }
            }
            /* M3: retire this roster's hellos along with its peers --
             * signaled_ otherwise grows by one id per phone for the room's
             * whole life (it was cleared only at shutdown). An id outside
             * the roster has nothing left to vouch for: a phone that
             * returns says controller_hello again. */
            (void)mdkr_party_prune_signaled_ids(signaled_, rosterControllers);
            /* P2.1: a confirmation is scoped to a live seat; an id that left
             * the roster has nothing left to trust, so drop it the same way. */
            (void)mdkr_party_prune_signaled_ids(confirmed_, rosterControllers);
            // Publish the roster with its state commit, before another socket
            // epoch or peer callback can interleave an event from newer state.
            queue_.push(std::move(event));
        }
        for (const auto &peer : retired) {
            if (peer->connection) peer->connection->close();
        }
        for (const auto &controller : controllers) {
            bool shouldCreate = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!callbackCurrentLocked(signalGeneration)) return;
                shouldCreate = signaled_.count(controller.id) != 0u &&
                    controller.phase != MdkrNativePartyControllerPhase::Pending &&
                    peers_.count(controller.id) == 0u;
            }
            if (shouldCreate) createPeer(controller, false, 0u, signalGeneration);
        }
    }

    void handleHello(const Json &value, uint64_t signalGeneration) {
        std::string id;
        if (!safeString(value, "controllerId", id, 64u) || id.empty()) return;
        MdkrNativePartyController controller;
        bool foundController = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!callbackCurrentLocked(signalGeneration)) return;
            signaled_.insert(id);
            const auto found = controllers_.find(id);
            if (found != controllers_.end()) {
                controller = found->second.controller;
                if (found->second.setupGeneration != signalGeneration) {
                    mdkr_party_setup_reauthorize(found->second.setup, steadyNowMs());
                    found->second.setupGeneration = signalGeneration;
                }
                foundController = true;
            }
        }
        if (foundController && controller.phase != MdkrNativePartyControllerPhase::Pending) {
            createPeer(controller, false, 0u, signalGeneration);
        }
    }

    /* forceRecreate/carriedOfferAttempts serve the C3 retry ladder only:
     * tick() sets forceRecreate when mdkr_party_retry_decide() says an
     * unauthenticated peer's offer has gone unanswered past the deadline,
     * and carries its offerAttempts forward so a peer does not get an
     * unbounded number of attempts just because each one recreates a brand
     * new Peer object. Every other call site (handleRoomState, handleHello,
     * peerDisconnected's existing reconnect-after-drop path) leaves both at
     * their defaults, exactly as before this policy existed. */
    void retirePeerCandidate(const std::shared_ptr<Peer> &peer,
                             const std::shared_ptr<rtc::PeerConnection> &connection) noexcept {
        try {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto                  found = peer ? peers_.find(peer->id) : peers_.end();
            if (found != peers_.end() && found->second == peer) peers_.erase(found);
        } catch (...) {
        }
        if (connection) {
            try {
                connection->close();
            } catch (...) {
            }
        }
    }

    void peerSetupFailed(const MdkrNativePartyController &controller,
                         uint64_t setupGeneration, uint64_t revision,
                         const std::shared_ptr<Peer> &peer,
                         const std::shared_ptr<rtc::PeerConnection> &connection) noexcept {
        try {
            std::lock_guard<std::mutex> lock(mutex_);
            if (setupCurrentLocked(controller, setupGeneration, revision)) {
                auto &admission = controllers_.find(controller.id)->second;
                if (peer) {
                    peer->failed = true;
                    admission.setup.offerAttempts = std::max(admission.setup.offerAttempts,
                                                              peer->offerAttempts);
                }
                mdkr_party_setup_failed(admission.setup, revision, steadyNowMs());
                reportSetupExhaustedLocked(admission);
            }
        } catch (...) { /* Retirement still runs if optional reporting fails. */ }
        // No RTC close/destruction while holding the state mutex.
        retirePeerCandidate(peer, connection);
    }

    void createPeer(const MdkrNativePartyController &controller,
                    bool                             forceRecreate        = false,
                    unsigned                         carriedOfferAttempts = 0u,
                    uint64_t                         signalGeneration     = 0u,
                    const std::shared_ptr<Peer>     &expectedPeer         = {},
                    uint64_t                         expectedOfferSentMs  = 0u,
                    uint64_t                         expectedSetupRevision = 0u) noexcept {
        std::shared_ptr<Peer>                peer;
        std::shared_ptr<rtc::PeerConnection> connection;
        std::shared_ptr<Peer>                retired;
        std::shared_ptr<Peer>                priorPeer;
        uint64_t setupGeneration = 0u;
        uint64_t setupRevision = 0u;
        try {
            std::vector<MdkrPartyIceServer> iceServers;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!callbackCurrentLocked(signalGeneration) || roomGone_ ||
                    !controllerCurrentLocked(controller)) return;
                const auto found = peers_.find(controller.id);
                if (!mdkr_party_retry_owner_current(forceRecreate,
                                                    expectedPeer && found != peers_.end() && found->second == expectedPeer,
                                                    expectedPeer && !expectedPeer->authenticated && !expectedPeer->failed &&
                                                        !expectedPeer->protocolMismatch && !expectedPeer->gaveUp,
                                                    expectedPeer && expectedPeer->offerAttempts == carriedOfferAttempts &&
                                                        expectedPeer->offerSentMs == expectedOfferSentMs)) return;
                if (found != peers_.end() && !found->second->failed && !forceRecreate &&
                    mdkr_party_peer_initialization_current(found->second->initializing,
                                                           generation_,
                                                           found->second->admissionGeneration)) return;
                auto &admission = controllers_.find(controller.id)->second;
                if (expectedSetupRevision != 0u &&
                    (admission.setup.revision != expectedSetupRevision ||
                     !mdkr_party_setup_due(admission.setup, steadyNowMs()))) return;
                if (admission.setupGeneration != generation_) {
                    mdkr_party_setup_reauthorize(admission.setup, steadyNowMs());
                    admission.setupGeneration = generation_;
                }
                if (found != peers_.end()) priorPeer = found->second;
                if (priorPeer && priorPeer->authenticated) admission.setup.offerAttempts = 0u;
                carriedOfferAttempts = std::max(carriedOfferAttempts, admission.setup.offerAttempts);
                if (priorPeer && !priorPeer->authenticated) {
                    carriedOfferAttempts = std::max(carriedOfferAttempts, priorPeer->offerAttempts);
                }
                admission.setup.offerAttempts = carriedOfferAttempts;
                if (++setupRevision_ == 0u) ++setupRevision_;
                setupRevision = mdkr_party_setup_begin(admission.setup, steadyNowMs(), setupRevision_);
                if (setupRevision == 0u) {
                    reportSetupExhaustedLocked(admission);
                    return;
                }
                setupGeneration = generation_;
                if (found != peers_.end()) {
                    // Retire the retry's exact owner in its admission lock.
                    // It cannot authenticate between that decision and later
                    // replacement; its RTC object remains owned outside lock.
                    retired = found->second;
                    peers_.erase(found);
                }
                // Reservation precedes even the first fallible Peer/config copy.
                iceServers = iceServers_;
            }
            if (retired && retired->connection) {
                try { retired->connection->close(); } catch (...) {}
            }
            peer = std::make_shared<Peer>();
            peer->id = controller.id;
            peer->seat = controller.seat;
            peer->leaseGeneration = controller.leaseGeneration;
            peer->connectionSequence = controller.connectionSequence;
            peer->offerAttempts = carriedOfferAttempts;
            peer->admissionGeneration = setupGeneration;
            bool reservationCurrent = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                const auto found = peers_.find(controller.id);
                reservationCurrent = setupCurrentLocked(controller, setupGeneration, setupRevision) &&
                    found == peers_.end();
                if (reservationCurrent) {
                    peers_.emplace(peer->id, peer);
                    peer->peerGeneration = ++peerGeneration_;
                    if (peer->peerGeneration == 0u) peer->peerGeneration = ++peerGeneration_;
                }
            }
            if (!reservationCurrent) {
                peerSetupFailed(controller, setupGeneration, setupRevision, peer, connection);
                return;
            }
            rtc::Configuration configuration;
            for (const MdkrPartyIceServer &server : resolvedIceServers(iceServers)) {
                try {
                    /* Credentials are TURN-scoped even here at the last hop: a
                     * non-turn/turns url carrying them is skipped, never handed
                     * to the ICE agent (iceServersFromSignal already refuses
                     * such lists, matching the page validators). */
                    const bool relay = server.url.rfind("turn:", 0u) == 0u ||
                                       server.url.rfind("turns:", 0u) == 0u;
                    if (!server.username.empty() && !relay) continue;
                    rtc::IceServer resolved(server.url);
                    if (!server.username.empty()) {
                        resolved.username = server.username;
                        resolved.password = server.credential;
                    }
                    configuration.iceServers.push_back(std::move(resolved));
                } catch (...) {
                    /* One URL libdatachannel refuses must not cost the rest. */
                }
            }
            if (configuration.iceServers.empty()) {
                configuration.iceServers.emplace_back(kFallbackStunUrl);
            }
            configuration.maxMessageSize = kMaxSignalBytes;
            connection                   = std::make_shared<rtc::PeerConnection>(configuration);
            bool admitted                = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                admitted = callbackCurrentLocked(signalGeneration, peer) &&
                           setupCurrentLocked(controller, setupGeneration, setupRevision) && !peer->failed;
                if (admitted) peer->connection = connection;
            }
            if (!admitted) {
                peerSetupFailed(controller, setupGeneration, setupRevision, peer, connection);
                return;
            }
            const std::weak_ptr<TransportState> weak     = shared_from_this();
            const std::weak_ptr<Peer>           weakPeer = peer;
            peer->connection->onLocalDescription([weak, weakPeer](rtc::Description description) {
                if (auto state = weak.lock()) {
                    if (auto current = weakPeer.lock()) {
                        /* This is the offer going out for real (the first one,
                         * or a fresh one from a retry-driven recreation) -- a
                         * new attempt, so mark it as one. Resends of this same
                         * description on socket reopen go through resendOffer()
                         * instead and do not call onLocalDescription again. */
                        state->markOfferSent(current, /*freshAttempt=*/true);
                        state->sendPeerSignal(current,
                                              Json{{"type", "webrtc_offer"}, {"to", current->id}, {"peerGeneration", current->peerGeneration}, {"sdp", {{"type", description.typeString()}, {"sdp", std::string(description)}}}});
                    }
                }
            });
            peer->connection->onLocalCandidate([weak, weakPeer](rtc::Candidate candidate) {
                if (auto state = weak.lock()) {
                    if (auto current = weakPeer.lock()) state->sendPeerSignal(current,
                                                                              Json{{"type", "webrtc_ice"}, {"to", current->id}, {"peerGeneration", current->peerGeneration}, {"candidate", {{"candidate", std::string(candidate)}, {"sdpMid", candidate.mid()}}}});
                }
            });
            peer->connection->onStateChange([weak, weakPeer](rtc::PeerConnection::State state) {
                if (state == rtc::PeerConnection::State::Disconnected ||
                    state == rtc::PeerConnection::State::Failed ||
                    state == rtc::PeerConnection::State::Closed) {
                    if (auto owner = weak.lock()) {
                        if (auto current = weakPeer.lock()) owner->peerDisconnected(current,
                                                                                    state == rtc::PeerConnection::State::Failed);
                    }
                }
            });
            /* The peer must be registered BEFORE the first createDataChannel:
             * creating the channel is what makes libdatachannel set the local
             * description, and onLocalDescription fires as soon as it does.
             * markOfferSent and sendPeerSignal both look this peer up in peers_
             * and silently return when it is absent, so registering only after
             * the channels exist (as this function originally did) raced the
             * callback -- the first offer was discarded unsent, offerSentMs
             * stayed 0, and the retry ladder (which reads offerSentMs == 0 as
             * "no offer to retry yet") could never rescue the stranded phone. */
            rtc::DataChannelInit stateConfiguration;
            stateConfiguration.reliability.unordered      = true;
            stateConfiguration.reliability.maxRetransmits = 0u;
            auto stateChannel                             = connection->createDataChannel("mdkr-pad-state-v1", stateConfiguration);
            auto controlChannel                           = connection->createDataChannel("mdkr-pad-control-v1");
            {
                std::lock_guard<std::mutex> lock(mutex_);
                admitted = callbackCurrentLocked(signalGeneration, peer) &&
                           setupCurrentLocked(controller, setupGeneration, setupRevision) && !peer->failed;
                if (admitted) {
                    peer->state   = stateChannel;
                    peer->control = controlChannel;
                }
            }
            if (!admitted) {
                peerSetupFailed(controller, setupGeneration, setupRevision, peer, connection);
                return;
            }
            peer->state->onMessage([weak, weakPeer](rtc::message_variant message) {
                if (auto owner = weak.lock()) {
                    if (auto current = weakPeer.lock()) owner->stateMessage(current, message);
                }
            });
            peer->state->onClosed([weak, weakPeer]() {
                if (auto owner = weak.lock()) {
                    if (auto current = weakPeer.lock()) owner->peerDisconnected(current, true);
                }
            });
            peer->control->onOpen([weak, weakPeer]() {
                if (auto owner = weak.lock()) {
                    if (auto current = weakPeer.lock()) owner->controlOpened(current);
                }
            });
            peer->control->onMessage([weak, weakPeer](rtc::message_variant message) {
                if (auto owner = weak.lock()) {
                    if (auto current = weakPeer.lock()) owner->controlMessage(current, message);
                }
            });
            peer->control->onClosed([weak, weakPeer]() {
                if (auto owner = weak.lock()) {
                    if (auto current = weakPeer.lock()) owner->peerDisconnected(current, true);
                }
            });
            {
                std::lock_guard<std::mutex> lock(mutex_);
                admitted = callbackCurrentLocked(signalGeneration, peer) &&
                           setupCurrentLocked(controller, setupGeneration, setupRevision) && !peer->failed;
                if (admitted) {
                    peer->initializing = false;
                    mdkr_party_setup_succeeded(controllers_.find(controller.id)->second.setup, setupRevision);
                }
            }
            if (!admitted) peerSetupFailed(controller, setupGeneration, setupRevision, peer, connection);
        } catch (...) {
            peerSetupFailed(controller, setupGeneration, setupRevision, peer, connection);
            if (retired && retired->connection) {
                try { retired->connection->close(); } catch (...) {}
            }
        }
    }

    void peerDisconnected(const std::shared_ptr<Peer> &peer, bool failed,
                          uint64_t signalGeneration = 0u,
                          Clock::time_point expectedPingAt = {}, uint32_t expectedPingNonce = 0u) {
        MdkrNativePartyController controller;
        bool recover = false;
        MdkrPartyTransportEvent event;
        event.type = MdkrPartyTransportEventType::ControllerDisconnected;
        event.controllerId = peer->id;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!callbackCurrentLocked(signalGeneration, peer) || peer->failed) return;
            if (!mdkr_party_ping_timeout_current(expectedPingAt != Clock::time_point{},
                    peer->pingOutstandingAt == expectedPingAt &&
                        peer->pingNonce == expectedPingNonce)) return;
            peer->failed = peer->failed || failed;
            const auto known = controllers_.find(peer->id);
            if (failed && !peer->initializing && known != controllers_.end() &&
                known->second.controller.phase != MdkrNativePartyControllerPhase::Pending) {
                controller = known->second.controller;
                recover = true;
            }
            queue_.push(std::move(event));
        }
        if (recover) createPeer(controller, false, 0u, signalGeneration);
    }

    void stateMessage(const std::shared_ptr<Peer> &peer,
                      const rtc::message_variant &message) {
        if (!std::holds_alternative<rtc::binary>(message)) return;
        const rtc::binary &bytes = std::get<rtc::binary>(message);
        if (bytes.size() < 24u || bytes.size() > 64u) return;
        MdkrPartyTransportEvent event;
        event.type = MdkrPartyTransportEventType::ControllerPacket;
        event.controllerId = peer->id;
        const auto *first = reinterpret_cast<const uint8_t *>(bytes.data());
        event.packet.assign(first, first + bytes.size());
        enqueue(std::move(event), 0u, peer);
    }

    void controlOpened(const std::shared_ptr<Peer> &peer) {
        try {
            std::shared_ptr<rtc::DataChannel> control;
            bool trusted;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!callbackCurrentLocked(0u, peer) || peer->failed || !peer->control) return;
                control = peer->control;
                trusted = confirmed_.count(peer->id) != 0u;
            }
            control->send(Json{{"type", "host_ready"}, {"protocol", kProtocol},
                {"seat", peer->seat}, {"leaseGeneration", peer->leaseGeneration},
                {"connectionSequence", peer->connectionSequence}}.dump());
            /* P2.1: a phone reconnecting to an already-confirmed seat is told
             * it is trusted the moment its control channel is back, so it
             * resumes without a second human step (confirm() also sends this
             * the instant the human presses Words Match on a live channel). */
            if (trusted) {
                control->send(Json{{"type", "seat_confirmed"},
                    {"protocol", kProtocol}}.dump());
            }
        } catch (...) { peerDisconnected(peer, true); }
    }

    void controlMessage(const std::shared_ptr<Peer> &peer,
                        const rtc::message_variant &message) {
        if (!std::holds_alternative<std::string>(message)) return;
        const std::string &text = std::get<std::string>(message);
        if (text.size() > kMaxControlBytes) return;
        Json value = Json::parse(text, nullptr, false);
        if (value.is_discarded() || !value.is_object()) return;
        try {
            MdkrPartyTransportEvent ready;
            if (controllerReadyEventFromControl(
                    value, peer->id, peer->connectionSequence, ready)) {
                const bool matched = ready.type ==
                    MdkrPartyTransportEventType::ControllerConnected;
                std::shared_ptr<rtc::DataChannel> control;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (!callbackCurrentLocked(0u, peer) || peer->failed) return;
                    peer->protocolMismatch = !matched;
                    if (matched) {
                        peer->authenticated = true;
                        peer->pingOutstandingAt = Clock::time_point{};
                        peer->nextPingAt = Clock::now() + std::chrono::seconds(5);
                    }
                    control = peer->control;
                    queue_.push(std::move(ready));
                }
                if (matched && control) {
                    control->send(Json{{"type", "controller_ready_ack"}}.dump());
                }
                /* I2 mismatch: no ack and no authentication, but the peer is
                 * left standing for the mismatch's whole lifetime -- the
                 * protocolMismatch latch keeps the C3 unanswered-offer
                 * ladder (tick()/socketOpened via mdkr_party_retry_decide)
                 * from recreating or giving up on it, since its offer WAS
                 * answered and no retry can close a version gap. The phone
                 * may simply reload into a page version that matches and
                 * complete controller_ready then, arriving as a fresh
                 * peer. */
            } else if (value.value("type", std::string{}) == "input_test" &&
                       value.contains("nonce") &&
                       value["nonce"].is_number_unsigned()) {
                /* P2.1: hold the connection-test ack until the human has
                 * confirmed this phone (Words Match). An unconfirmed phone
                 * that taps Go anyway never passes the test, so it cannot
                 * advance past the compare screen; the auto test the phone
                 * runs on seat_confirmed then completes at once. */
                bool trusted;
                std::shared_ptr<rtc::DataChannel> control;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (!callbackCurrentLocked(0u, peer) || peer->failed) return;
                    trusted = confirmed_.count(peer->id) != 0u;
                    control = peer->control;
                }
                if (trusted && control) {
                    control->send(Json{{"type", "input_test_ack"},
                        {"nonce", value["nonce"]}}.dump());
                }
            } else if (value.value("type", std::string{}) == "controller_rename" &&
                       value.value("protocol", 0u) == kProtocol) {
                /* F1 session-alive names: only a peer that completed
                 * controller_ready may relabel its own seat row, and only
                 * within the redeem-time name budget; the host model
                 * (native_party_host.cpp validRenameName) is the strict
                 * validation boundary behind this size gate. The per-peer
                 * mdkr_party_rename_admit gate (dedupe + humane rate) runs
                 * BEFORE any event exists, so a rename flood can never
                 * crowd pad packets out of the shared queue. */
                std::string name;
                bool admitted = false;
                if (safeString(value, "name", name, 48u)) {
                    const uint64_t nowMs = static_cast<uint64_t>(
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            Clock::now().time_since_epoch()).count());
                    std::lock_guard<std::mutex> lock(mutex_);
                    admitted = callbackCurrentLocked(0u, peer) &&
                        !peer->failed && peer->authenticated &&
                        mdkr_party_rename_admit(peer->renameGate, name, nowMs);
                }
                if (admitted) {
                    MdkrPartyTransportEvent renamed;
                    renamed.type = MdkrPartyTransportEventType::ControllerRenamed;
                    renamed.controllerId = peer->id;
                    renamed.message = std::move(name);
                    enqueue(std::move(renamed), 0u, peer);
                }
            } else if (value.value("type", std::string{}) == "pong" &&
                       value.value("protocol", 0u) == kProtocol &&
                       value.contains("nonce") &&
                       value["nonce"].is_number_unsigned()) {
                const uint64_t nonce = value["nonce"].get<uint64_t>();
                unsigned rttMs = 0u;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (nonce <= std::numeric_limits<uint32_t>::max() &&
                        callbackCurrentLocked(0u, peer) && !peer->failed &&
                        peer->pingOutstandingAt != Clock::time_point{} &&
                        static_cast<uint32_t>(nonce) == peer->pingNonce) {
                        /* RTT: this pong closes the outstanding ping.
                         * Floored at 1 ms so "measured, just fast" never
                         * reads as the host model's no-sample zero. */
                        const auto elapsed = std::chrono::duration_cast<
                            std::chrono::milliseconds>(
                                Clock::now() - peer->pingOutstandingAt).count();
                        rttMs = elapsed < 1 ? 1u : static_cast<unsigned>(elapsed);
                        peer->pingOutstandingAt = Clock::time_point{};
                        peer->nextPingAt = Clock::now() + std::chrono::seconds(5);
                    }
                }
                /* Outside the lock: enqueue() takes mutex_ itself. */
                if (rttMs != 0u) {
                    MdkrPartyTransportEvent sample;
                    sample.type = MdkrPartyTransportEventType::ControllerRtt;
                    sample.controllerId = peer->id;
                    sample.rttMs = rttMs;
                    enqueue(std::move(sample), 0u, peer);
                }
            }
        } catch (...) { /* Malformed peer control cannot escape its callback. */ }
    }

    void handleAnswer(const Json &value, uint64_t signalGeneration) {
        std::string id;
        uint64_t peerGeneration = 0u;
        if (!safeString(value, "controllerId", id, 64u) ||
            !uintValue(value, "peerGeneration", peerGeneration,
                std::numeric_limits<uint32_t>::max()) || peerGeneration == 0u ||
            !value.contains("sdp") || !value["sdp"].is_object()) return;
        std::string sdp;
        std::string type;
        if (!safeString(value["sdp"], "sdp", sdp, 60u * 1024u) ||
            !safeString(value["sdp"], "type", type, 16u) || type != "answer") return;
        std::shared_ptr<Peer> peer;
        std::shared_ptr<rtc::PeerConnection> connection;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!callbackCurrentLocked(signalGeneration)) return;
            const auto found = peers_.find(id);
            if (found == peers_.end() ||
                found->second->peerGeneration != static_cast<uint32_t>(peerGeneration) ||
                !callbackCurrentLocked(signalGeneration, found->second) ||
                found->second->failed || !found->second->connection) return;
            peer = found->second;
            connection = peer->connection;
        }
        try { connection->setRemoteDescription(rtc::Description(sdp, type)); }
        catch (...) {
            peerDisconnected(peer, true, signalGeneration);
            return;
        }
        emitPhrase(peer, sdp, signalGeneration);
    }

    /*
     * SAS v2 derivation site -- the capture point. Both descriptions are set
     * exactly here: the local offer existed before this answer could arrive
     * (createDataChannel set it, onLocalDescription sent it), and
     * handleAnswer just applied the remote one, so this is the earliest
     * moment both DTLS fingerprints exist. The controller fingerprint is
     * read from the answer's own SDP text (the very bytes just applied),
     * the host fingerprint from the connection's local description. Any
     * missing or ambiguous piece means NO ControllerPhrase event at all:
     * the seat simply never shows a phrase (fail closed -- the v1
     * derivation no longer exists in this build to fall back to). A
     * reconnect re-enters through a fresh answer and re-emits with the new
     * channel's fingerprints.
     */
    void emitPhrase(const std::shared_ptr<Peer> &peer,
                    const std::string &answerSdp, uint64_t signalGeneration) {
        const std::string controllerFingerprint =
            canonicalSdpFingerprint(answerSdp);
        std::shared_ptr<rtc::PeerConnection> connection;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!callbackCurrentLocked(signalGeneration, peer) || peer->failed) return;
            connection = peer->connection;
        }
        if (!connection) return;
        std::string hostSdp;
        try {
            const std::optional<rtc::Description> description =
                connection->localDescription();
            if (description) hostSdp = std::string(*description);
        } catch (...) {
            return;
        }
        const std::string hostFingerprint = canonicalSdpFingerprint(hostSdp);
        std::string controllerKey;
        std::string room;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!callbackCurrentLocked(signalGeneration, peer) || peer->failed) return;
            const auto found = controllers_.find(peer->id);
            if (found != controllers_.end()) {
                controllerKey = found->second.controller.publicKey;
            }
            room = roomId_;
        }
        std::string phrase;
        /* room.empty() is unreachable today -- an answer presupposes the
         * bootstrap that set roomId_ -- but a transcript with a vacant
         * field must stay impossible by construction, not by call order. */
        if (controllerFingerprint.empty() || hostFingerprint.empty() ||
            controllerKey.empty() || room.empty() ||
            !identity_.phrase(controllerKey, room, hostFingerprint,
                              controllerFingerprint, phrase)) {
            return;
        }
        MdkrPartyTransportEvent event;
        event.type = MdkrPartyTransportEventType::ControllerPhrase;
        event.controllerId = peer->id;
        event.message = std::move(phrase);
        enqueue(std::move(event), signalGeneration, peer);
    }

    void handleIce(const Json &value, uint64_t signalGeneration) {
        std::string id;
        uint64_t peerGeneration = 0u;
        if (!safeString(value, "controllerId", id, 64u) ||
            !uintValue(value, "peerGeneration", peerGeneration,
                std::numeric_limits<uint32_t>::max()) || peerGeneration == 0u ||
            !value.contains("candidate") || !value["candidate"].is_object()) return;
        std::string candidate;
        std::string mid;
        if (!safeString(value["candidate"], "candidate", candidate, 4096u) ||
            !safeString(value["candidate"], "sdpMid", mid, 64u)) return;
        std::shared_ptr<rtc::PeerConnection> connection;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!callbackCurrentLocked(signalGeneration)) return;
            const auto found = peers_.find(id);
            if (found == peers_.end() ||
                found->second->peerGeneration != static_cast<uint32_t>(peerGeneration) ||
                !callbackCurrentLocked(signalGeneration, found->second) ||
                found->second->failed || !found->second->connection) return;
            connection = found->second->connection;
        }
        try { connection->addRemoteCandidate(rtc::Candidate(candidate, mid)); }
        catch (...) { /* One malformed candidate cannot tear down a healthy peer. */ }
    }

    void sendPeerSignal(const std::shared_ptr<Peer> &peer, const Json &value,
                        uint64_t signalGeneration = 0u) {
        // Capture the socket and admit this peer in command's SAME lock. A
        // separate precheck could otherwise forward an old peer to a new room.
        (void)command(value, signalGeneration, peer);
    }

    /* C3 retry bookkeeping. freshAttempt=true is a genuinely new offer (the
     * first one, or one from a retry-driven recreation); false is a resend
     * of the same description after the socket reopened, which restarts
     * the 20 s deadline (it just went out again) but must not consume one
     * of the 3 attempts. offerSentMs/offerAttempts are set unconditionally,
     * even though command() inside sendPeerSignal may still fail if the
     * socket happens to be down right now -- that is exactly the case
     * resendOffer() exists to recover from once the socket comes back, so
     * the record must survive a send that never reached the wire. */
    void markOfferSent(const std::shared_ptr<Peer> &peer, bool freshAttempt,
                       uint64_t signalGeneration = 0u) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!callbackCurrentLocked(signalGeneration, peer) || peer->failed) return;
        peer->offerSentMs = steadyNowMs();
        if (freshAttempt) peer->offerAttempts++;
        const auto admission = controllers_.find(peer->id);
        if (admission != controllers_.end()) {
            admission->second.setup.offerAttempts = std::max(
                admission->second.setup.offerAttempts, peer->offerAttempts);
        }
    }

    /* Resends the peer's existing local description verbatim -- no
     * createOffer(), no renegotiation, same peerGeneration -- because
     * C3 is a Worker-relay drop, not anything wrong with the offer. */
    void resendOffer(const std::shared_ptr<Peer> &peer, uint64_t signalGeneration) {
        std::shared_ptr<rtc::PeerConnection> connection;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!callbackCurrentLocked(signalGeneration, peer) || peer->failed) return;
            connection = peer->connection;
        }
        if (!connection) return;
        std::optional<rtc::Description> description;
        try { description = connection->localDescription(); }
        catch (...) { return; }
        if (!description) return;
        markOfferSent(peer, /*freshAttempt=*/false, signalGeneration);
        sendPeerSignal(peer, Json{{"type", "webrtc_offer"}, {"to", peer->id},
            {"peerGeneration", peer->peerGeneration},
            {"sdp", {{"type", description->typeString()},
                {"sdp", std::string(*description)}}}}, signalGeneration);
    }

    std::mutex mutex_;
    MdkrPartyEventQueue queue_{kMaxQueuedEvents};
    std::vector<MdkrPartyTransportEvent> drained_;
    size_t drainedIndex_ = 0u;
    uint64_t loggedDroppedPadPackets_ = 0u;
    std::shared_ptr<rtc::WebSocket> socket_;
    std::map<std::string, std::shared_ptr<Peer>> peers_;
    std::map<std::string, ControllerAdmission> controllers_;
    std::set<std::string> signaled_;
    /* P2.1 compare-then-trust: controllerIds the host has confirmed (Words
     * Match). Only a confirmed controller is told seat_confirmed and has its
     * input_test acked, so a provisional phone cannot advance past the compare
     * screen. Keyed by id so it survives a peer recreation on reconnect;
     * pruned with signaled_ as controllers leave, cleared on shutdown. */
    std::set<std::string> confirmed_;
    PartyIdentity identity_;
    std::string origin_;
    std::string roomId_;
    std::string credential_;
    std::string inviteUrl_;
    std::string fallbackCode_;
    /* The room's server-delivered ICE servers, set once by the bootstrap;
     * empty means createPeer uses the baked-in STUN fallback. */
    std::vector<MdkrPartyIceServer> iceServers_;
    unsigned inviteGeneration_ = 0u;
    uint64_t inviteExpiresAtMs_ = 0u;
    uint64_t generation_ = 0u;
    uint32_t peerGeneration_ = 0u;
    uint64_t setupRevision_ = 0u;
    unsigned reconnectAttempt_ = 0u;
    Clock::time_point reconnectAt_{};
    /* I4 resume classification (mdkr_party_resume_decide): the per-attempt
     * "the service refused this upgrade" latch socketError sets and
     * socketClosed consumes; the consecutive-refusal streak and its
     * first-refusal timestamp (steadyNowMs domain, 0 = none standing) that
     * only a successful open resets; and the terminal latch after which
     * this transport never opens another socket. */
    bool resumeRejected_ = false;
    unsigned resumeRejections_ = 0u;
    uint64_t firstResumeRejectedMs_ = 0u;
    bool roomGone_ = false;
    /* M7 socket-cap headroom: outbound messages sent on the CURRENT host
     * socket (the Worker hard-closes a socket at 512 lifetime messages,
     * party-room.ts SIGNAL_LIFETIME_MESSAGES), and the recycle latch
     * command() sets at mdkr_party_socket_cycle_due's 480 threshold.
     * Consumed by tick() on the launcher thread; both reset with each new
     * socket in connect(). */
    unsigned socketSentMessages_ = 0u;
    bool socketCyclePending_ = false;
    bool creating_ = false;
    bool shuttingDown_ = false;
};

class LibDatachannelPartyTransport final : public MdkrPartyTransport {
public:
    bool available() const override { return true; }
    const char *unavailableReason() const override { return ""; }

    bool open(const std::string &serviceOrigin) noexcept override {
        if (state_) return false;
        std::shared_ptr<TransportState> candidate;
        try {
            candidate = std::make_shared<TransportState>();
            if (candidate->initialize(serviceOrigin)) {
                state_ = std::move(candidate);
                return true;
            }
        } catch (...) {
            // Construction, identity/config allocation and callback setup are
            // fallible. The UI receives the same ordinary unavailable result.
        }
        if (candidate) {
            try { candidate->shutdown(); } catch (...) { /* release local owner */ }
        }
        return false;
    }

    bool approve(const std::string &id, unsigned seat) override {
        return sendHostCommand("approve", id, Json{{"seat", seat}});
    }
    bool reject(const std::string &id) override {
        return sendHostCommand("reject", id);
    }
    bool remove(const std::string &id) override {
        return sendHostCommand("remove", id);
    }
    bool confirm(const std::string &id) override {
        return state_ && state_->confirm(id);
    }
    bool rotateInvite(unsigned generation) override {
        return state_ && state_->command(Json{{"type", "host_command"},
            {"action", "rotate"}, {"expectedInviteGeneration", generation}});
    }
    bool revokeInvite() override {
        return sendHostCommand("revoke", "");
    }
    bool closeRoom() override {
        /* M4: the send only queued the goodbye; give it its bounded
         * (kMdkrPartyCloseFlushDeadlineMs) chance to reach the wire before
         * the caller hangs up, so the worker can relay host_closed to the
         * phones instead of them meeting a silent socket drop. */
        if (!sendHostCommand("close", "")) return false;
        if (state_) state_->flushCloseCommand();
        return true;
    }
    bool sendRumble(const std::string &id, uint16_t strength) override {
        return state_ && state_->sendRumble(id, strength);
    }
    bool sendRaceState(const std::string &id,
                       const MdkrNativeRaceState &state) override {
        return state_ && state_->sendRaceState(id, state);
    }
    bool poll(MdkrPartyTransportEvent &event) override {
        return state_ && state_->poll(event);
    }
    void shutdown() override {
        if (state_) state_->shutdown();
        state_.reset();
    }

private:
    bool sendHostCommand(const char *action, const std::string &id,
                         Json extra = Json::object()) {
        if (!state_) return false;
        Json message = {{"type", "host_command"}, {"action", action}};
        if (!id.empty()) message["controllerId"] = id;
        message.update(extra);
        return state_->command(message);
    }

    std::shared_ptr<TransportState> state_;
};

}  // namespace

std::unique_ptr<MdkrPartyTransport> mdkr_create_native_party_transport() {
    return std::make_unique<LibDatachannelPartyTransport>();
}

std::shared_future<void> mdkr_native_party_cleanup() {
    return rtc::Cleanup();
}

std::string mdkr_party_signaling_url_for_test(
    const std::string &origin, const std::string &path) {
    return signalingUrl(origin, path);
}

bool mdkr_party_sas_phrase_for_test(
    const uint8_t privateScalar[32], const std::string &roomId,
    const std::string &controllerPublicKey,
    const std::string &hostFingerprint,
    const std::string &controllerFingerprint,
    std::string &hostPublicKey, std::string &phrase) {
    PartyIdentity identity;
    if (!identity.loadPrivateForTest(privateScalar)) return false;
    hostPublicKey = identity.publicKey();
    return identity.phrase(controllerPublicKey, roomId, hostFingerprint,
                           controllerFingerprint, phrase);
}

std::string mdkr_party_sdp_fingerprint_for_test(const std::string &sdp) {
    return canonicalSdpFingerprint(sdp);
}

uint64_t mdkr_party_close_flush_wait_for_test(
    const std::function<uint64_t()> &nowMs,
    const std::function<size_t()> &bufferedBytes,
    const std::function<void(uint64_t)> &sleepMs) {
    return closeFlushWait(nowMs, bufferedBytes, sleepMs);
}

bool mdkr_party_host_command_rejection_for_test(
    const std::string &text, MdkrPartyTransportEvent &event) {
    const Json value = Json::parse(text, nullptr, false);
    if (value.is_discarded() || !value.is_object()) return false;
    /* Same guard the socket path's outer try gives the shared parser. */
    try { return commandRejectionFromSignal(value, event); }
    catch (...) { return false; }
}

bool mdkr_party_controller_ready_event_for_test(
    const std::string &text, const std::string &controllerId,
    uint32_t connectionSequence, MdkrPartyTransportEvent &event) {
    const Json value = Json::parse(text, nullptr, false);
    if (value.is_discarded() || !value.is_object()) return false;
    /* Same guard the control channel's outer try gives the shared parser. */
    try {
        return controllerReadyEventFromControl(
            value, controllerId, connectionSequence, event);
    } catch (...) { return false; }
}

bool mdkr_party_ice_servers_for_test(
    const std::string &text, std::vector<MdkrPartyIceServer> &servers) {
    servers.clear();
    const Json value = Json::parse(text, nullptr, false);
    std::vector<MdkrPartyIceServer> parsed;
    if (!value.is_discarded() && value.is_object()) {
        /* Same guard the socket path's outer try gives the shared parser. */
        try { parsed = iceServersFromSignal(value); }
        catch (...) { parsed.clear(); }
    }
    const bool serverProvided = !parsed.empty();
    servers = resolvedIceServers(std::move(parsed));
    return serverProvided;
}

size_t mdkr_party_prune_signaled_ids(
    std::set<std::string> &signaled,
    const std::map<std::string, MdkrNativePartyController> &roster) {
    size_t pruned = 0u;
    for (auto iterator = signaled.begin(); iterator != signaled.end();) {
        if (roster.count(*iterator) == 0u) {
            iterator = signaled.erase(iterator);
            pruned++;
        } else {
            ++iterator;
        }
    }
    return pruned;
}
