/*
 * Native online BETA: launcher-side live-adapter wiring.
 *
 * Compiled ONLY under MDKR_ENABLE_ONLINE_BETA (the CMake beta gate; never in a
 * shipping build -- the option defaults OFF and release.yml/build_app_bundle.sh
 * never set it). Provides the real OnlineRoom_makeGatedLiveAdapter the Online
 * Room panel calls: it composes the production MatchRoom HTTP transport, the
 * real signal-client mesh backend and the O-T3 live adapter EXACTLY as the O-T6
 * e2e driver does (tests/test_online_live_transport_e2e_driver.cpp), and returns
 * an owning wrapper that keeps the two borrowed transports alive for the
 * adapter's lifetime.
 *
 * Fences enforced here (v1, do not weaken):
 *   - 2 endpoints: localSeatCount = 1 (solo endpoint; couch-pair = 2 comes later
 *     and >=3 needs the full-mesh-rekey protocol that is NOT closed).
 *   - retail identities: every local roster slot is clamped to
 *     MDKR_MATCH_IDENTITY_RETAIL, feeding the O-T5 clamp inside the adapter.
 *   - STUN-only: a client-side belt drops any turn:/turns: ICE server the service
 *     delivers, so a mis-provisioned zone can never silently start using relay.
 *
 * The security-audited parsers/crypto/SAS live in the composed translation units
 * and are NOT touched here.
 */
#if MDKR_ENABLE_ONLINE_BETA

#include "online/match_live_adapter.h"
#include "online/match_live_transport.h"
#include "net/net_roster_runtime.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

/* STUN-only belt: wrap the production room transport and drop any relay
 * (turn:/turns:) ICE server the service delivers on every drained event. The
 * mesh already skips credentialed non-turn entries; this makes the beta refuse
 * relay outright even if a zone is mis-provisioned. Every other call delegates
 * straight through. */
class StunOnlyRoomTransport final : public MdkrOnlineRoomTransport {
public:
    explicit StunOnlyRoomTransport(
        std::unique_ptr<MdkrOnlineRoomTransport> inner)
        : inner_(std::move(inner)) {}

    bool beginCreate(const MdkrOnlineCompatibilityV1 &compatibility,
                     unsigned seatCount) override {
        return inner_->beginCreate(compatibility, seatCount);
    }
    bool beginJoin(const std::string &capability,
                   const MdkrOnlineCompatibilityV1 &compatibility,
                   unsigned seatCount) override {
        return inner_->beginJoin(capability, compatibility, seatCount);
    }
    bool beginJoinByCode(const std::string &code,
                         const MdkrOnlineCompatibilityV1 &compatibility,
                         unsigned seatCount) override {
        return inner_->beginJoinByCode(code, compatibility, seatCount);
    }
    bool submitCommand(const MdkrOnlineCommand &command) override {
        return inner_->submitCommand(command);
    }
    void pump(std::vector<MdkrOnlineRoomEvent> &out) override {
        inner_->pump(out);
        for (MdkrOnlineRoomEvent &event : out) {
            event.iceServers.erase(
                std::remove_if(event.iceServers.begin(), event.iceServers.end(),
                               [](const MdkrMatchPeerIceServer &server) {
                                   return isRelay(server.url);
                               }),
                event.iceServers.end());
        }
    }
    void close() override { inner_->close(); }

    /* The wrapped production HTTP transport. mdkr_online_room_http_transport_invite
     * dynamic_casts to the concrete RoomHttpTransport, so the invite accessor
     * must reach past this belt to the real transport underneath. */
    MdkrOnlineRoomTransport *inner() const { return inner_.get(); }

private:
    static bool isRelay(const std::string &url) {
        std::string scheme = url.substr(0, url.find(':'));
        std::transform(scheme.begin(), scheme.end(), scheme.begin(),
                       [](unsigned char c) {
                           return static_cast<char>(std::tolower(c));
                       });
        return scheme == "turn" || scheme == "turns";
    }

    std::unique_ptr<MdkrOnlineRoomTransport> inner_;
};

/* Owning wrapper. MdkrOnlineLiveAdapterOptions.room and .meshBackend are
 * borrowed and must outlive the adapter, so the launcher-facing adapter owns all
 * three. Member declaration order makes inner_ destruct FIRST (it borrows the
 * other two), then mesh_, then room_. Every IMdkrOnlineAdapter call delegates to
 * inner_; the panel drives only this narrow seam. */
class OwningLiveAdapter final : public IMdkrOnlineAdapter {
public:
    OwningLiveAdapter(std::unique_ptr<MdkrOnlineRoomTransport> room,
                      std::unique_ptr<MdkrOnlineMeshSignalBackend> mesh)
        : room_(std::move(room)), mesh_(std::move(mesh)) {}

    bool build(const MdkrOnlineLiveAdapterOptions &options, std::string *error) {
        inner_ = mdkr_online_live_adapter_create(options, error);
        return inner_ != nullptr;
    }

    MdkrOnlineAdapterStep submit(
        const MdkrOnlineAdapterCommand &command) override {
        return inner_->submit(command);
    }
    bool view(MdkrOnlineViewModel *out) const override {
        return inner_->view(out);
    }
    MdkrOnlineJourney journey() const override { return inner_->journey(); }
    void service() override { inner_->service(); }
    uint32_t revision() const override { return inner_->revision(); }
    MdkrPlayIntent sessionIntent() const override {
        return inner_->sessionIntent();
    }
    bool raceAdmissionEnabled() const override {
        return inner_->raceAdmissionEnabled();
    }
    bool timeoutExpired() const override { return inner_->timeoutExpired(); }
    MdkrOnlineFakeAdapter *fakeAdapter() override {
        return inner_->fakeAdapter();
    }

    /* The concrete HTTP transport behind the STUN-only belt, for the invite
     * accessor. Returns the room transport directly if no belt is present. */
    MdkrOnlineRoomTransport *httpTransportForInvite() const {
        StunOnlyRoomTransport *belt =
            dynamic_cast<StunOnlyRoomTransport *>(room_.get());
        return belt != nullptr ? belt->inner() : room_.get();
    }

private:
    std::unique_ptr<MdkrOnlineRoomTransport> room_;
    std::unique_ptr<MdkrOnlineMeshSignalBackend> mesh_;
    std::unique_ptr<IMdkrOnlineAdapter> inner_;
};

/* Stable per-launch session id: distinct across launches, constant within one,
 * derived once from the steady clock. The adapter uses it as the local identity
 * nonce; convergence does not depend on its value (the e2e driver uses a fixed
 * constant). */
uint64_t launchSessionId() {
    static const uint64_t id = []() {
        return static_cast<uint64_t>(
                   std::chrono::steady_clock::now().time_since_epoch().count()) ^
               UINT64_C(0x4f4e4c494e450000); /* "ONLINE" tag */
    }();
    return id;
}

}  // namespace

std::unique_ptr<IMdkrOnlineAdapter> OnlineRoom_makeGatedLiveAdapter(
    const MdkrOnlineCompatibilityV1 &compatibility, MdkrOnlineJourney journey,
    const std::string &joinCode) {
    std::string error;

    /* Construction never touches the network: the room transport begins on the
     * first submit(CREATE_ROOM/JOIN_ROOM). MDKR_PARTY_ORIGIN is the compiled
     * https origin (empty -> the transport refuses and we fall back to the
     * fake). Native transports refuse non-https/wss off-machine. */
    std::unique_ptr<MdkrOnlineRoomTransport> http =
        mdkr_online_room_http_transport_create(MDKR_PARTY_ORIGIN, &error);
    if (!http) return nullptr;
    auto room = std::make_unique<StunOnlyRoomTransport>(std::move(http));
    std::unique_ptr<MdkrOnlineMeshSignalBackend> mesh =
        mdkr_online_mesh_signal_backend_create(MDKR_PARTY_ORIGIN);
    if (!mesh) return nullptr;

    MdkrOnlineRoomTransport *roomPtr = room.get();
    MdkrOnlineMeshSignalBackend *meshPtr = mesh.get();
    auto adapter = std::make_unique<OwningLiveAdapter>(std::move(room),
                                                       std::move(mesh));

    MdkrOnlineLiveAdapterOptions options;
    options.sessionId = launchSessionId();
    options.compatibility = compatibility;
    options.journey = journey;                    /* CREATE or JOIN-by-code */
    options.localSeatCount = 1u;                  /* FENCE: solo endpoint (2P) */
    options.joinCode = joinCode;                  /* JOIN by 6-digit fallback */
    for (unsigned i = 0u; i < MDKR_MATCH_LOCAL_PLAYER_SLOTS; ++i) {
        options.localRoster.player_identity[i] = MDKR_MATCH_IDENTITY_RETAIL;
    }                                             /* FENCE: retail-only clamp */
    options.raceAdmissionEnabled = true;          /* beta build only */
    options.romVerified = true;                   /* beta: local ROM verified;
                                                     never sourced from service */
    options.inputDelay = 2u;
    options.room = roomPtr;
    options.meshBackend = meshPtr;

    if (!adapter->build(options, &error)) return nullptr;
    return adapter;
}

/* Beta-only invite accessor for the creator's invite card. Reaches past the
 * owning wrapper + STUN-only belt to the concrete HTTP transport and returns
 * its learned 6-digit fallback code and invite URL, but only once the room has
 * reached Ready (creator only). Returns false for a non-live adapter, a joiner,
 * or before the room is Ready -- so the panel simply shows nothing yet. */
bool OnlineRoom_liveInvite(IMdkrOnlineAdapter *adapter, std::string *code,
                           std::string *inviteUrl) {
    OwningLiveAdapter *owning = dynamic_cast<OwningLiveAdapter *>(adapter);
    if (owning == nullptr) return false;
    MdkrOnlineRoomTransport *http = owning->httpTransportForInvite();
    if (http == nullptr) return false;
    MdkrOnlineRoomHttpInvite invite;
    if (!mdkr_online_room_http_transport_invite(http, &invite) || !invite.ready) {
        return false;
    }
    if (code != nullptr) *code = invite.fallbackCode;
    if (inviteUrl != nullptr) *inviteUrl = invite.inviteUrl;
    return true;
}

/* ======================================================================== *
 * O-T6b: visible-engine race-boot handoff registry
 *
 * The live adapter publishes itself here the instant its race transport becomes
 * ready (LiveAdapter::setUpRace); a teardown / SAS re-verify retracts it. The
 * launcher's interactive loop polls it and boots the visible engine on the live
 * transport. Launcher-thread only (published from service() inside the panel
 * draw, polled from the same loop), so a plain pointer needs no lock.
 * ======================================================================== */
namespace {
IMdkrOnlineAdapter *sPendingEngineRaceBoot = nullptr;
/* Explicit owner of the process-global engine roster. It lives HERE, in the
 * beta-only wiring TU, rather than in platform/net/net_roster_runtime.c: that
 * file is compiled into every build without the MDKR_ENABLE_ONLINE_BETA macro,
 * so state or state-mutating functions added there would leak into the
 * OFF/release binary. Only online boots install a roster, so the token belongs
 * with the online code and the release build stays byte-identical. */
uint64_t sRosterOwnerToken = 0u;
}  // namespace

void OnlineRoom_publishEngineRaceBoot(IMdkrOnlineAdapter *adapter) {
    sPendingEngineRaceBoot = adapter;
}

void OnlineRoom_retractEngineRaceBoot(IMdkrOnlineAdapter *adapter) {
    if (sPendingEngineRaceBoot == adapter) sPendingEngineRaceBoot = nullptr;
}

IMdkrOnlineAdapter *OnlineRoom_pollEngineRaceBoot(void) {
    IMdkrOnlineAdapter *pending = sPendingEngineRaceBoot;
    sPendingEngineRaceBoot = nullptr; /* consume once: boot exactly one race */
    return pending;
}

void OnlineRoom_setRosterOwner(uint64_t token) {
    /* Ownership is meaningful only while a roster is installed. */
    sRosterOwnerToken = mdkr_net_roster_runtime_active() ? token : 0u;
}

uint64_t OnlineRoom_rosterOwner(void) {
    return mdkr_net_roster_runtime_active() ? sRosterOwnerToken : 0u;
}

bool OnlineRoom_guardRosterOwner(uint64_t token) {
    if (mdkr_net_roster_guard_decides_clear(mdkr_net_roster_runtime_active(),
                                            sRosterOwnerToken, token)) {
        mdkr_net_roster_runtime_clear();
        sRosterOwnerToken = 0u;
        return true;
    }
    return false;
}

/* ======================================================================== *
 * Test-only in-process loopback race pair (MDKR_APP_TEST_ONLINE_LIVE)
 *
 * Builds two REAL live adapters over the O-T2 loopback signal hub feeding a real
 * MdkrMatchPeerMesh (real libdatachannel DTLS on 127.0.0.1) plus an in-process
 * MatchRoom double running the REAL lobby reducer -- the same shape
 * tests/test_online_live_adapter.cpp uses -- and drives them through
 * create/join/preflight/loading to a READY race transport. Endpoint A is the
 * VISIBLE endpoint whose live transport main_app feeds into mdkr64_engine_boot;
 * endpoint B is the peer that seals real input over the mesh so the visible
 * engine runs an actual networked race. Ordinary play never reaches this: it is
 * gated by MDKR_APP_TEST_ONLINE_LIVE and only proves the make-or-break engine
 * wiring headlessly. Deliberately NOT the production OnlineRoom_makeGatedLive
 * Adapter path (that needs a live MatchRoom Worker + second process); this is the
 * loopback equivalent of the O-T6 e2e driver, in one process.
 * ======================================================================== */
namespace {

MdkrOnlineCompatibilityV1 loopbackCompatibility() {
    MdkrOnlineCompatibilityV1 c;
    std::memset(&c, 0, sizeof(c));
    c.protocol_version = MDKR_ONLINE_PROTOCOL_VERSION;
    for (unsigned i = 0u; i < sizeof(c.build_id); ++i)
        c.build_id[i] = static_cast<uint8_t>(i + 1u);
    for (unsigned i = 0u; i < sizeof(c.gameplay_digest); ++i)
        c.gameplay_digest[i] = static_cast<uint8_t>(0x80u + i);
    c.rom_revision = 1u; /* MDKR_ROM_US_11 */
    c.cadence_hz = 30u;
    return c;
}

/* Canonical 22-char base64url room id (the mesh transcript binding requires a
 * canonical decode round-trip). */
std::string loopbackRoomId() {
    static const char alpha[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    uint8_t raw[16];
    for (unsigned i = 0u; i < 16u; ++i) raw[i] = static_cast<uint8_t>(i + 1u);
    std::string out;
    unsigned bits = 0u;
    uint32_t acc = 0u;
    for (unsigned i = 0u; i < 16u; ++i) {
        acc = (acc << 8) | raw[i];
        bits += 8u;
        while (bits >= 6u) {
            bits -= 6u;
            out.push_back(alpha[(acc >> bits) & 0x3fu]);
        }
    }
    if (bits > 0u) out.push_back(alpha[(acc << (6u - bits)) & 0x3fu]);
    return out;
}

const char kLoopbackCredential[] =
    "cred0123456789abcdefABCDEF0123456789abcdefX"; /* 43 chars */

}  // namespace

/* The doubles below are members of the externally-visible
 * MdkrOnlineTestLoopbackRace, so they must have external linkage (a struct with
 * external linkage may not have anonymous-namespace-typed members --
 * -Wsubobject-linkage). Their names are unique to this TU. */
struct LoopbackMatchRoom {
    MdkrOnlineLobby lobby{};
    bool created = false;
    uint64_t nextEndpoint = UINT64_C(0x101);

    uint64_t create(const MdkrOnlineCompatibilityV1 &compat, unsigned seats) {
        const uint64_t leader = nextEndpoint++;
        if (!mdkr_online_lobby_init(&lobby, UINT64_C(0xABCDEF), leader, &compat,
                                    seats)) {
            return 0u;
        }
        created = true;
        return leader;
    }

    uint64_t join(const MdkrOnlineCompatibilityV1 &compat, unsigned seats) {
        if (!created) return 0u;
        const uint64_t ep = nextEndpoint++;
        MdkrOnlineCommand cmd;
        std::memset(&cmd, 0, sizeof(cmd));
        cmd.protocol_version = MDKR_ONLINE_PROTOCOL_VERSION;
        cmd.expected_revision = lobby.revision;
        cmd.command_id = 1u;
        cmd.actor_endpoint_id = ep;
        cmd.type = MDKR_ONLINE_JOIN;
        cmd.value = seats;
        cmd.compatibility = compat;
        const MdkrOnlineStep step = mdkr_online_lobby_dispatch(&lobby, &cmd);
        if (!step.accepted) {
            --nextEndpoint;
            return 0u;
        }
        return ep;
    }

    MdkrOnlineStep command(const MdkrOnlineCommand &in) {
        MdkrOnlineCommand cmd = in;
        return mdkr_online_lobby_dispatch(&lobby, &cmd);
    }
};

class LoopbackRoomTransport final : public MdkrOnlineRoomTransport {
public:
    explicit LoopbackRoomTransport(LoopbackMatchRoom *room) : room_(room) {}

    bool beginCreate(const MdkrOnlineCompatibilityV1 &compat,
                     unsigned seatCount) override {
        kind_ = Kind::Create;
        compat_ = compat;
        seats_ = seatCount;
        return true;
    }
    bool beginJoin(const std::string &, const MdkrOnlineCompatibilityV1 &compat,
                   unsigned seatCount) override {
        kind_ = Kind::Join;
        compat_ = compat;
        seats_ = seatCount;
        return true;
    }
    bool beginJoinByCode(const std::string &,
                         const MdkrOnlineCompatibilityV1 &compat,
                         unsigned seatCount) override {
        kind_ = Kind::Join;
        compat_ = compat;
        seats_ = seatCount;
        return true;
    }
    bool submitCommand(const MdkrOnlineCommand &command) override {
        MdkrOnlineRoomEvent ev;
        ev.type = MdkrOnlineRoomEvent::Type::CommandResult;
        ev.step = room_->command(command);
        queue_.push_back(ev);
        return true;
    }
    void pump(std::vector<MdkrOnlineRoomEvent> &out) override {
        out.clear();
        if (kind_ != Kind::None && !ready_) {
            MdkrOnlineRoomEvent ev;
            const uint64_t ep = kind_ == Kind::Create
                                    ? room_->create(compat_, seats_)
                                    : room_->join(compat_, seats_);
            if (ep == 0u) {
                ev.type = MdkrOnlineRoomEvent::Type::Failure;
                ev.failure = MDKR_ONLINE_VIEW_FAILURE_SERVICE_UNAVAILABLE;
                out.push_back(ev);
                return;
            }
            ev.type = MdkrOnlineRoomEvent::Type::Ready;
            ev.localEndpointId = ep;
            ev.roomId = loopbackRoomId();
            ev.credential = kLoopbackCredential;
            ev.lobby = room_->lobby;
            ev.haveLobby = true;
            lastRevision_ = room_->lobby.revision;
            ready_ = true;
            out.push_back(ev);
            return;
        }
        if (ready_ && room_->lobby.revision != lastRevision_) {
            MdkrOnlineRoomEvent ev;
            ev.type = MdkrOnlineRoomEvent::Type::State;
            ev.lobby = room_->lobby;
            ev.haveLobby = true;
            lastRevision_ = room_->lobby.revision;
            out.push_back(ev);
        }
        for (auto &ev : queue_) out.push_back(ev);
        queue_.clear();
    }
    void close() override {}

private:
    enum class Kind { None, Create, Join };
    LoopbackMatchRoom *room_;
    Kind kind_ = Kind::None;
    MdkrOnlineCompatibilityV1 compat_{};
    unsigned seats_ = 1u;
    bool ready_ = false;
    uint32_t lastRevision_ = 0u;
    std::deque<MdkrOnlineRoomEvent> queue_;
};

class LoopbackHub;

class LoopbackFeed final : public MdkrMatchPeerSignalFeed {
public:
    LoopbackFeed(LoopbackHub *hub, uint64_t endpointId)
        : hub_(hub), self_(endpointId) {}
    MdkrMatchSignalSendResult send(const MdkrMatchSignalOutbound &m) override;
    void drainEvents(std::vector<MdkrMatchSignalEvent> &out) override;
    std::deque<MdkrMatchSignalEvent> inbox;

private:
    LoopbackHub *hub_;
    uint64_t self_;
};

class LoopbackHub {
public:
    struct Endpoint {
        uint32_t generation = 0u;
        std::unique_ptr<LoopbackFeed> feed;
    };

    LoopbackFeed *addEndpoint(uint64_t id, uint32_t generation) {
        std::lock_guard<std::mutex> lock(mutex_);
        Endpoint &endpoint = endpoints_[id];
        endpoint.generation = generation;
        endpoint.feed = std::make_unique<LoopbackFeed>(this, id);
        return endpoint.feed.get();
    }

    void welcome(uint64_t target) {
        std::lock_guard<std::mutex> lock(mutex_);
        Endpoint &endpoint = endpoints_.at(target);
        MdkrMatchSignalEvent event;
        event.type = MdkrMatchSignalEventType::Welcome;
        event.endpointId = std::to_string(target);
        event.connectionGeneration = endpoint.generation;
        for (const auto &entry : endpoints_) {
            if (entry.first == target) continue;
            MdkrMatchSignalPeerRef ref;
            ref.endpointId = std::to_string(entry.first);
            ref.connectionGeneration = entry.second.generation;
            event.peers.push_back(std::move(ref));
        }
        endpoint.feed->inbox.push_back(std::move(event));
    }

    MdkrMatchSignalSendResult route(uint64_t from,
                                    const MdkrMatchSignalOutbound &m) {
        std::lock_guard<std::mutex> lock(mutex_);
        MdkrMatchSignalSendResult result;
        result.sequence = ++sequence_;
        result.ok = true;
        uint64_t to = 0u;
        try {
            to = std::stoull(m.toEndpointId);
        } catch (...) {
            to = 0u;
        }
        const auto found = endpoints_.find(to);
        if (found == endpoints_.end() ||
            found->second.generation != m.toConnectionGeneration) {
            return result;
        }
        MdkrMatchSignalEvent event;
        event.fromEndpointId = std::to_string(from);
        event.fromConnectionGeneration = endpoints_.at(from).generation;
        if (m.type == "peer_hello") {
            event.type = MdkrMatchSignalEventType::PeerHello;
            event.publicKey = m.publicKey;
        } else if (m.type == "webrtc_offer") {
            event.type = MdkrMatchSignalEventType::WebrtcOffer;
            event.sdp = m.sdp;
        } else if (m.type == "webrtc_answer") {
            event.type = MdkrMatchSignalEventType::WebrtcAnswer;
            event.sdp = m.sdp;
        } else if (m.type == "webrtc_ice") {
            event.type = MdkrMatchSignalEventType::WebrtcIce;
            event.candidate = m.candidate;
            event.hasSdpMid = m.hasSdpMid;
            event.sdpMid = m.sdpMid;
            event.hasSdpMLineIndex = m.hasSdpMLineIndex;
            event.sdpMLineIndex = m.sdpMLineIndex;
            event.hasUsernameFragment = m.hasUsernameFragment;
            event.usernameFragment = m.usernameFragment;
        } else if (m.type == "peer_end") {
            event.type = MdkrMatchSignalEventType::PeerEnd;
            event.reason = m.reason;
        } else {
            result.ok = false;
            result.error = kMdkrMatchSignalInvalidClientMessage;
            return result;
        }
        found->second.feed->inbox.push_back(std::move(event));
        return result;
    }

    void drainInbox(LoopbackFeed *feed,
                    std::vector<MdkrMatchSignalEvent> &out) {
        std::lock_guard<std::mutex> lock(mutex_);
        out.clear();
        while (!feed->inbox.empty()) {
            out.push_back(std::move(feed->inbox.front()));
            feed->inbox.pop_front();
        }
    }

private:
    std::map<uint64_t, Endpoint> endpoints_;
    std::mutex mutex_;
    uint32_t sequence_ = 0u;
};

MdkrMatchSignalSendResult LoopbackFeed::send(const MdkrMatchSignalOutbound &m) {
    return hub_->route(self_, m);
}
void LoopbackFeed::drainEvents(std::vector<MdkrMatchSignalEvent> &out) {
    hub_->drainInbox(this, out);
}

class LoopbackMeshBackend final : public MdkrOnlineMeshSignalBackend {
public:
    explicit LoopbackMeshBackend(LoopbackHub *hub) : hub_(hub) {}
    MdkrMatchPeerSignalFeed *beginSignaling(
        uint64_t localEndpointId, uint32_t generation, const std::string &,
        const std::string &,
        const std::vector<MdkrMatchPeerIceServer> &) override {
        began = localEndpointId;
        /* The live adapter passes 0 to adopt the service-assigned generation;
         * mirror the real signal service that assigns the first socket gen 1. */
        return hub_->addEndpoint(localEndpointId,
                                 generation != 0u ? generation : 1u);
    }
    void reset() override {}
    uint64_t began = 0u;

private:
    LoopbackHub *hub_;
};

namespace {

MdkrOnlineAdapterCommand loopbackCmd(IMdkrOnlineAdapter *a,
                                     MdkrOnlineViewAction action,
                                     unsigned seat = 0u, unsigned value = 0u) {
    static uint64_t nextId = 1u;
    MdkrOnlineAdapterCommand c;
    c.expectedRevision = a->revision();
    c.requestId = nextId++;
    c.action = action;
    c.seat = seat;
    c.value = value;
    return c;
}

MdkrOnlineViewModel loopbackView(IMdkrOnlineAdapter *a) {
    MdkrOnlineViewModel m{};
    a->view(&m);
    return m;
}

bool loopbackPumpUntil(std::vector<IMdkrOnlineAdapter *> adapters,
                       const std::function<bool()> &done,
                       unsigned maxMs = 30000u) {
    for (unsigned elapsed = 0u; elapsed <= maxMs; elapsed += 10u) {
        for (IMdkrOnlineAdapter *a : adapters) a->service();
        if (done()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    for (IMdkrOnlineAdapter *a : adapters) a->service();
    return done();
}

MdkrOnlineLiveAdapterOptions loopbackOptions(LoopbackRoomTransport *room,
                                             LoopbackMeshBackend *backend,
                                             MdkrOnlineJourney journey) {
    MdkrOnlineLiveAdapterOptions o;
    o.sessionId = UINT64_C(0x4c495645); /* "LIVE" */
    o.compatibility = loopbackCompatibility();
    o.journey = journey;
    o.localSeatCount = 1u; /* FENCE: solo endpoint (2P total) */
    o.joinCapability = "capabilitycapabilitycapabilitycapabilityXYZ0";
    for (unsigned i = 0u; i < MDKR_MATCH_LOCAL_PLAYER_SLOTS; ++i)
        o.localRoster.player_identity[i] = MDKR_MATCH_IDENTITY_RETAIL;
    o.raceAdmissionEnabled = true;
    o.romVerified = true;
    o.inputDelay = 2u;
    /* nowMs left empty: real steady clock, serviced frequently enough (handshake
     * sleeps here, the overlay service hook during the engine race). */
    o.room = room;
    o.meshBackend = backend;
    return o;
}

}  // namespace

/* Opaque owner: keeps the doubles + both adapters alive for the engine boot.
 * Members are declared so the adapters (which borrow the transports/backends)
 * destruct FIRST (reverse declaration order). */
struct MdkrOnlineTestLoopbackRace {
    LoopbackMatchRoom room;
    LoopbackHub hub;
    LoopbackRoomTransport transportA{&room};
    LoopbackRoomTransport transportB{&room};
    LoopbackMeshBackend backendA{&hub};
    LoopbackMeshBackend backendB{&hub};
    std::unique_ptr<IMdkrOnlineAdapter> a;
    std::unique_ptr<IMdkrOnlineAdapter> b;
};

MdkrOnlineTestLoopbackRace *OnlineRoom_makeTestLoopbackRace(std::string *error) {
    auto set_err = [&](const char *m) {
        if (error != nullptr) *error = m;
    };
    /* A fresh process-global roster: A will install it (endpoint order below
     * makes A the owner, so the visible engine renders A's viewport). */
    mdkr_net_roster_runtime_clear();

    auto race = std::make_unique<MdkrOnlineTestLoopbackRace>();
    race->a = mdkr_online_live_adapter_create(
        loopbackOptions(&race->transportA, &race->backendA,
                        MDKR_ONLINE_JOURNEY_CREATE));
    race->b = mdkr_online_live_adapter_create(
        loopbackOptions(&race->transportB, &race->backendB,
                        MDKR_ONLINE_JOURNEY_JOIN));
    if (!race->a || !race->b) {
        set_err("live adapter construction failed");
        return nullptr;
    }
    IMdkrOnlineAdapter *A = race->a.get();
    IMdkrOnlineAdapter *B = race->b.get();
    std::vector<IMdkrOnlineAdapter *> both{A, B};

    A->submit(loopbackCmd(A, MDKR_ONLINE_VIEW_ACTION_CREATE_ROOM));
    if (!loopbackPumpUntil({A}, [&]() {
            return loopbackView(A).kind == MDKR_ONLINE_VIEW_ROOM;
        }, 5000u)) {
        set_err("create room did not reach ROOM");
        return nullptr;
    }
    B->submit(loopbackCmd(B, MDKR_ONLINE_VIEW_ACTION_JOIN_ROOM));
    if (!loopbackPumpUntil(both, [&]() {
            return loopbackView(A).member_count == 2u &&
                   loopbackView(B).member_count == 2u;
        }, 5000u)) {
        set_err("join did not reach two members");
        return nullptr;
    }
    A->submit(loopbackCmd(A, MDKR_ONLINE_VIEW_ACTION_CHECK_SETUP));
    B->submit(loopbackCmd(B, MDKR_ONLINE_VIEW_ACTION_CHECK_SETUP));
    race->hub.welcome(race->backendA.began);
    race->hub.welcome(race->backendB.began);
    if (!loopbackPumpUntil(both, [&]() {
            return loopbackView(A).verification_phrase[0] != '\0' &&
                   loopbackView(B).verification_phrase[0] != '\0';
        }, 30000u)) {
        set_err("mesh preflight did not surface the verification phrase");
        return nullptr;
    }
    A->submit(loopbackCmd(A, MDKR_ONLINE_VIEW_ACTION_CONFIRM_PHRASE));
    B->submit(loopbackCmd(B, MDKR_ONLINE_VIEW_ACTION_CONFIRM_PHRASE));
    if (!loopbackPumpUntil(both, [&]() {
            return loopbackView(A).kind == MDKR_ONLINE_VIEW_SELECTING &&
                   loopbackView(B).kind == MDKR_ONLINE_VIEW_SELECTING;
        }, 5000u)) {
        set_err("confirm phrase did not reach SELECTING");
        return nullptr;
    }
    auto selectReady = [&](IMdkrOnlineAdapter *self, unsigned character) {
        auto until = [&](MdkrOnlineViewAction next) {
            return loopbackPumpUntil(both, [&]() {
                return loopbackView(self).primary.action == next;
            }, 5000u);
        };
        self->submit(loopbackCmd(self, MDKR_ONLINE_VIEW_ACTION_CHOOSE_CHARACTER,
                                 0u, character));
        until(MDKR_ONLINE_VIEW_ACTION_CHOOSE_VEHICLE);
        self->submit(
            loopbackCmd(self, MDKR_ONLINE_VIEW_ACTION_CHOOSE_VEHICLE, 0u, 0u));
        until(MDKR_ONLINE_VIEW_ACTION_VOTE_TRACK);
        /* Track 5 == Ancient Lake, matching tests/input_scripts/race_2p_split. */
        self->submit(
            loopbackCmd(self, MDKR_ONLINE_VIEW_ACTION_VOTE_TRACK, 0u, 5u));
        until(MDKR_ONLINE_VIEW_ACTION_READY);
        self->submit(loopbackCmd(self, MDKR_ONLINE_VIEW_ACTION_READY, 0u, 1u));
        (void)loopbackPumpUntil(both, [&]() {
            return loopbackView(self).primary.action !=
                   MDKR_ONLINE_VIEW_ACTION_READY;
        }, 5000u);
    };
    selectReady(A, 1u); /* distinct characters (characters are unique) */
    selectReady(B, 2u);
    if (!loopbackPumpUntil(both, [&]() {
            return loopbackView(A).ready_count == 2u &&
                   loopbackView(B).ready_count == 2u;
        }, 5000u)) {
        set_err("both endpoints did not become ready");
        return nullptr;
    }
    /* Leader starts the race; both follow LOADING -> the O-T5 clamp build ->
     * preflight consensus -> engine roster install -> race transport ready. A is
     * serviced before B in `both`, so A wins the once-only process-global roster
     * install and the visible engine renders A's viewport.
     *
     * START_RACE's value becomes the lobby's selected_vehicle_mask (BEGIN_LOADING
     * carries it), which the O-T5 builder freezes into the manifest. It MUST equal
     * the ROM's usable-vehicle mask for the voted track or the engine's online
     * race admission (mdkr_match_manifest_accepts_loaded_race) rejects the boot.
     * Ancient Lake (track 5) is car/hovercraft/plane == 0x07. */
    A->submit(loopbackCmd(A, MDKR_ONLINE_VIEW_ACTION_START_RACE, 0u, 7u));
    if (!loopbackPumpUntil(both, [&]() {
            MdkrOnlineLiveRaceInfo ia{}, ib{};
            return mdkr_online_live_adapter_race_info(A, &ia) && ia.ready &&
                   mdkr_online_live_adapter_race_info(B, &ib) && ib.ready;
        }, 30000u)) {
        set_err("race transport did not become ready on both endpoints");
        return nullptr;
    }
    if (!mdkr_net_roster_runtime_active()) {
        set_err("engine roster was not installed at race-ready");
        return nullptr;
    }
    return race.release();
}

IMdkrOnlineAdapter *OnlineRoom_testLoopbackVisible(
    MdkrOnlineTestLoopbackRace *race) {
    return race != nullptr ? race->a.get() : nullptr;
}

IMdkrOnlineAdapter *OnlineRoom_testLoopbackPeer(
    MdkrOnlineTestLoopbackRace *race) {
    return race != nullptr ? race->b.get() : nullptr;
}

void OnlineRoom_destroyTestLoopbackRace(MdkrOnlineTestLoopbackRace *race) {
    delete race;
}

/* ======================================================================== *
 * Test-only single-adapter CLOUD race driver (MDKR_APP_TEST_ONLINE_LIVE_CLOUD)
 *
 * Drives ONE production-shaped live adapter -- OnlineRoom_makeGatedLiveAdapter
 * against the compiled-in MDKR_PARTY_ORIGIN, the exact factory the real Online
 * Room panel calls -- through create/join, secure setup, selection and loading
 * to a READY race transport. The state machine mirrors
 * tests/test_online_live_transport_e2e_driver.cpp's main() exactly (same
 * command sequence, same compatibility fixture); the difference is that this
 * copy hands the resulting adapter back to a caller that boots the VISIBLE
 * engine on it instead of running a headless tick loop.
 * ======================================================================== */
namespace {

uint64_t cloudNowMs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

MdkrOnlineViewModel cloudView(IMdkrOnlineAdapter *a) {
    MdkrOnlineViewModel m{};
    a->view(&m);
    return m;
}

MdkrOnlineAdapterCommand cloudCmd(IMdkrOnlineAdapter *a,
                                 MdkrOnlineViewAction action,
                                 unsigned seat = 0u, unsigned value = 0u) {
    static uint64_t nextId = 1u;
    MdkrOnlineAdapterCommand c;
    c.expectedRevision = a->revision();
    c.requestId = nextId++;
    c.action = action;
    c.seat = seat;
    c.value = value;
    return c;
}

/* Pump the adapter until `done` holds or the absolute deadline elapses --
 * mirrors the e2e driver's pumpUntil() but with an explicit deadline instead
 * of a global, so this can share a process with other timing concerns. */
bool cloudPumpUntil(IMdkrOnlineAdapter *a, uint64_t deadlineMs,
                    const std::function<bool()> &done) {
    while (cloudNowMs() < deadlineMs) {
        a->service();
        if (done()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    a->service();
    return done();
}

bool cloudSubmitWhenOffered(IMdkrOnlineAdapter *a, uint64_t deadlineMs,
                            MdkrOnlineViewAction action, unsigned seat,
                            unsigned value) {
    if (!cloudPumpUntil(a, deadlineMs, [&]() {
            return cloudView(a).primary.action == action;
        })) {
        return false;
    }
    return a->submit(cloudCmd(a, action, seat, value)).accepted;
}

}  // namespace

struct MdkrOnlineTestCloudLiveSession {
    /* Owns the wrapper (STUN-only belt + OwningLiveAdapter) for its lifetime;
     * never dereferenced through the race_* seam (see the comment at the
     * OnlineRoom_pollEngineRaceBoot() poll site). */
    std::unique_ptr<IMdkrOnlineAdapter> adapter;
    /* The raw LiveAdapter*, borrowed from OnlineRoom_pollEngineRaceBoot() once
     * the race transport is ready; this is what race_* calls and the engine
     * boot itself must use. Lifetime is owned by `adapter` above (the
     * wrapper's inner_). */
    IMdkrOnlineAdapter *raceAdapter = nullptr;
};

MdkrOnlineTestCloudLiveSession *OnlineRoom_makeTestCloudLiveSession(
    MdkrOnlineJourney journey, const std::string &joinCode, unsigned character,
    unsigned track, unsigned vehicleMask, uint64_t timeoutMs,
    std::string *error) {
    auto set_err = [&](const std::string &m) {
        if (error != nullptr) *error = m;
        std::fprintf(stderr, "[online-live-cloud] result=error message=%s\n",
                     m.c_str());
    };
    const uint64_t deadline = cloudNowMs() + timeoutMs;
    const bool isCreate = journey == MDKR_ONLINE_JOURNEY_CREATE;

    /* Same compatibility fixture as the loopback race and the e2e driver
     * (compatibilityFixture() there, loopbackCompatibility() here): both sides
     * must agree byte-for-byte or the lobby reducer refuses the join. */
    std::unique_ptr<IMdkrOnlineAdapter> adapter =
        OnlineRoom_makeGatedLiveAdapter(loopbackCompatibility(), journey,
                                        joinCode);
    if (!adapter) {
        set_err("gated live adapter construction refused (missing/invalid "
                "compiled-in MDKR_PARTY_ORIGIN?)");
        return nullptr;
    }
    IMdkrOnlineAdapter *a = adapter.get();
    std::fprintf(stderr, "[online-live-cloud] role=%s origin=%s\n",
                 isCreate ? "create" : "join", MDKR_PARTY_ORIGIN);

    /* 1. Create or join the room. */
    const MdkrOnlineViewAction entryAction =
        isCreate ? MDKR_ONLINE_VIEW_ACTION_CREATE_ROOM
                 : MDKR_ONLINE_VIEW_ACTION_JOIN_ROOM;
    if (!a->submit(cloudCmd(a, entryAction)).accepted) {
        set_err("entry action refused");
        return nullptr;
    }
    if (!cloudPumpUntil(a, deadline, [&]() {
            return cloudView(a).kind == MDKR_ONLINE_VIEW_ROOM ||
                   cloudView(a).kind == MDKR_ONLINE_VIEW_RECOVERY;
        })) {
        set_err("room never opened");
        return nullptr;
    }
    if (cloudView(a).kind == MDKR_ONLINE_VIEW_RECOVERY) {
        set_err("room entry failed failure=" +
                std::to_string(static_cast<int>(cloudView(a).failure)));
        return nullptr;
    }
    if (isCreate) {
        std::string code, inviteUrl;
        if (!cloudPumpUntil(a, deadline, [&]() {
                return OnlineRoom_liveInvite(a, &code, &inviteUrl);
            })) {
            set_err("create succeeded but the invite never became ready");
            return nullptr;
        }
        std::fprintf(stderr, "[online-live-cloud] code=%s\n", code.c_str());
    } else {
        std::fprintf(stderr, "[online-live-cloud] joined\n");
    }

    /* 2. Wait for both endpoints present. */
    if (!cloudPumpUntil(a, deadline, [&]() {
            return cloudView(a).member_count >= 2u;
        })) {
        set_err("second endpoint never joined");
        return nullptr;
    }
    std::fprintf(stderr, "[online-live-cloud] members=2\n");

    /* 3. Secure-setup: bring up the mesh, compute + confirm the phrase. */
    if (!a->submit(cloudCmd(a, MDKR_ONLINE_VIEW_ACTION_CHECK_SETUP)).accepted) {
        set_err("check setup refused");
        return nullptr;
    }
    if (!cloudPumpUntil(a, deadline, [&]() {
            return cloudView(a).verification_phrase[0] != '\0' ||
                   cloudView(a).kind == MDKR_ONLINE_VIEW_RECOVERY;
        })) {
        set_err("verification phrase never appeared");
        return nullptr;
    }
    if (cloudView(a).kind == MDKR_ONLINE_VIEW_RECOVERY) {
        set_err("secure setup failed failure=" +
                std::to_string(static_cast<int>(cloudView(a).failure)));
        return nullptr;
    }
    std::fprintf(stderr, "[online-live-cloud] phrase=%s\n",
                 cloudView(a).verification_phrase);

    if (!a->submit(cloudCmd(a, MDKR_ONLINE_VIEW_ACTION_CONFIRM_PHRASE))
             .accepted) {
        set_err("confirm phrase refused");
        return nullptr;
    }
    if (!cloudPumpUntil(a, deadline, [&]() {
            return cloudView(a).kind == MDKR_ONLINE_VIEW_SELECTING;
        })) {
        set_err("never reached selection");
        return nullptr;
    }
    std::fprintf(stderr, "[online-live-cloud] selecting\n");

    /* 4. Selections + ready. */
    if (!cloudSubmitWhenOffered(a, deadline,
                               MDKR_ONLINE_VIEW_ACTION_CHOOSE_CHARACTER, 0u,
                               character) ||
        !cloudSubmitWhenOffered(a, deadline,
                               MDKR_ONLINE_VIEW_ACTION_CHOOSE_VEHICLE, 0u,
                               0u) ||
        !cloudSubmitWhenOffered(a, deadline, MDKR_ONLINE_VIEW_ACTION_VOTE_TRACK,
                               0u, track) ||
        !cloudSubmitWhenOffered(a, deadline, MDKR_ONLINE_VIEW_ACTION_READY, 0u,
                               1u)) {
        set_err("selection/ready flow stalled");
        return nullptr;
    }
    if (!cloudPumpUntil(a, deadline, [&]() {
            return cloudView(a).ready_count >= 2u;
        })) {
        set_err("both endpoints never readied");
        return nullptr;
    }
    std::fprintf(stderr, "[online-live-cloud] ready=2\n");

    /* 5. The leader starts the race; both follow the LOADING phase. START_RACE's
     * value becomes the lobby's selected_vehicle_mask, which the O-T5 clamp
     * freezes into the manifest -- it MUST equal the ROM's usable-vehicle mask
     * for the voted track or the engine's online race admission rejects the
     * boot (see OnlineRoom_makeTestLoopbackRace's identical comment). */
    if (isCreate) {
        if (!cloudSubmitWhenOffered(a, deadline,
                                   MDKR_ONLINE_VIEW_ACTION_START_RACE, 0u,
                                   vehicleMask)) {
            set_err("start race refused");
            return nullptr;
        }
    }
    std::fprintf(stderr, "[online-live-cloud] loading\n");

    /* 6. Preflight consensus, install, and race-ready.
     *
     * mdkr_online_live_adapter_race_info/race_advance/race_inputs_for_tick
     * and friends all dynamic_cast their argument to the CONCRETE LiveAdapter;
     * `a` here is OwningLiveAdapter (the STUN-only belt + owning wrapper
     * OnlineRoom_makeGatedLiveAdapter returns) -- a DIFFERENT concrete type
     * that only delegates through the IMdkrOnlineAdapter virtual interface --
     * so calling them directly on `a` always fails closed (dynamic_cast
     * returns null). The real Online Room panel never hits this: the moment
     * the race transport becomes ready, LiveAdapter::setUpRace() publishes
     * ITSELF -- the raw LiveAdapter*, not the wrapper -- via
     * OnlineRoom_publishEngineRaceBoot(this), the exact O-T6b handoff
     * main_app.cpp's interactive loop polls (OnlineRoom_pollEngineRaceBoot())
     * before booting the visible engine. Poll the SAME handoff here instead
     * of race_info() on `a`, and use the polled raw pointer for every
     * subsequent race_* call and for the engine boot itself. */
    IMdkrOnlineAdapter *raceBoot = nullptr;
    if (!cloudPumpUntil(a, deadline, [&]() {
            raceBoot = OnlineRoom_pollEngineRaceBoot();
            return raceBoot != nullptr;
        })) {
        set_err("race transport never came up");
        return nullptr;
    }
    if (!mdkr_net_roster_runtime_active()) {
        set_err("engine roster was not installed at race-ready");
        return nullptr;
    }
    MdkrOnlineLiveRaceInfo info{};
    (void)mdkr_online_live_adapter_race_info(raceBoot, &info);
    std::fprintf(stderr,
                 "[online-live-cloud] race-ready epoch=%u firstTick=%u "
                 "active=0x%02x local=0x%02x remote=0x%02x\n",
                 static_cast<unsigned>(info.matchEpoch),
                 static_cast<unsigned>(info.firstTick),
                 static_cast<unsigned>(info.activeSlotMask),
                 static_cast<unsigned>(info.localSlotMask),
                 static_cast<unsigned>(info.remoteSlotMask));

    auto *session = new MdkrOnlineTestCloudLiveSession();
    session->adapter = std::move(adapter);
    session->raceAdapter = raceBoot;
    return session;
}

IMdkrOnlineAdapter *OnlineRoom_testCloudLiveAdapter(
    MdkrOnlineTestCloudLiveSession *session) {
    return session != nullptr ? session->raceAdapter : nullptr;
}

void OnlineRoom_destroyTestCloudLiveSession(
    MdkrOnlineTestCloudLiveSession *session) {
    delete session;
}

#endif /* MDKR_ENABLE_ONLINE_BETA */
