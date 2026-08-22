/*
 * O-T3 live lobby adapter tests.
 *
 * The lobby side is driven through an in-process MatchRoom double that runs the
 * REAL lobby reducer (the "small in-process double" option in the task brief).
 * The transport side is driven through the O-T2 loopback signal hub feeding a
 * real MdkrMatchPeerMesh (real libdatachannel DTLS on 127.0.0.1) -- the same
 * loopback mesh test_match_peer_transport.cpp exercises. No wrangler, no
 * network, no engine process: preflight-consensus + install are proven with
 * two live adapters in one process.
 */
#include "online/match_live_adapter.h"

#include "net/net_roster_runtime.h"

#include <cassert>
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
#include <vector>

namespace {

int g_failures = 0;
int g_checks = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        ++g_checks;                                                            \
        if (!(cond)) {                                                         \
            ++g_failures;                                                      \
            std::fprintf(stderr, "CHECK failed: %s (%s:%d)\n", #cond,          \
                         __FILE__, __LINE__);                                  \
        }                                                                      \
    } while (0)

MdkrOnlineCompatibilityV1 compatibilityFixture() {
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

/* Canonical 22-char base64url room id (16 bytes) -- the mesh's transcript room
 * binding requires canonical encoding (decode(value) round-trips). */
std::string canonicalRoomId() {
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
    return out; /* 22 chars, canonical (trailing bits zero-padded) */
}

const char kCredential[] = "cred0123456789abcdefABCDEF0123456789abcdefX"; /* 43 */

/* ---- In-process MatchRoom double (authoritative lobby reducer) ----------- */

struct FakeMatchRoom {
    MdkrOnlineLobby lobby{};
    bool created = false;
    uint64_t nextEndpoint = UINT64_C(0x101);

    uint64_t create(const MdkrOnlineCompatibilityV1 &compat, unsigned seats) {
        const uint64_t leader = nextEndpoint++;
        if (!mdkr_online_lobby_init(&lobby, UINT64_C(0xABCDEF), leader,
                                    &compat, seats)) {
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

class FakeRoomTransport final : public MdkrOnlineRoomTransport {
public:
    FakeRoomTransport(FakeMatchRoom *room, unsigned) : room_(room) {}

    /* Inject a mapped failure the adapter must surface without losing state. */
    void injectFailure(MdkrOnlineViewFailure failure) {
        MdkrOnlineRoomEvent ev;
        ev.type = MdkrOnlineRoomEvent::Type::Failure;
        ev.failure = failure;
        queue_.push_back(ev);
    }

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
            ev.roomId = canonicalRoomId();
            ev.credential = kCredential;
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
    FakeMatchRoom *room_;
    Kind kind_ = Kind::None;
    MdkrOnlineCompatibilityV1 compat_{};
    unsigned seats_ = 1u;
    bool ready_ = false;
    uint32_t lastRevision_ = 0u;
    std::deque<MdkrOnlineRoomEvent> queue_;
};

/* ---- O-T2 loopback signal hub (adapted from test_match_peer_transport) --- */

class FakeHub;

class FakeFeed final : public MdkrMatchPeerSignalFeed {
public:
    FakeFeed(FakeHub *hub, uint64_t endpointId) : hub_(hub), self_(endpointId) {}
    MdkrMatchSignalSendResult send(const MdkrMatchSignalOutbound &m) override;
    void drainEvents(std::vector<MdkrMatchSignalEvent> &out) override;
    std::deque<MdkrMatchSignalEvent> inbox;

private:
    FakeHub *hub_;
    uint64_t self_;
};

class FakeHub {
public:
    struct Endpoint {
        uint32_t generation = 0u;
        std::unique_ptr<FakeFeed> feed;
    };

    FakeFeed *addEndpoint(uint64_t id, uint32_t generation) {
        std::lock_guard<std::mutex> lock(mutex_);
        Endpoint &endpoint = endpoints_[id];
        endpoint.generation = generation;
        endpoint.feed = std::make_unique<FakeFeed>(this, id);
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
        try { to = std::stoull(m.toEndpointId); } catch (...) { to = 0u; }
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

    void drainInbox(FakeFeed *feed, std::vector<MdkrMatchSignalEvent> &out) {
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

MdkrMatchSignalSendResult FakeFeed::send(const MdkrMatchSignalOutbound &m) {
    return hub_->route(self_, m);
}
void FakeFeed::drainEvents(std::vector<MdkrMatchSignalEvent> &out) {
    hub_->drainInbox(this, out);
}

/* Mesh backend over the shared hub. Records the ids it registered so the test
 * can deliver welcomes once every adapter has begun signaling. */
class HubMeshBackend final : public MdkrOnlineMeshSignalBackend {
public:
    explicit HubMeshBackend(FakeHub *hub) : hub_(hub) {}
    MdkrMatchPeerSignalFeed *beginSignaling(
        uint64_t localEndpointId, uint32_t generation, const std::string &,
        const std::string &,
        const std::vector<MdkrMatchPeerIceServer> &) override {
        began = localEndpointId;
        return hub_->addEndpoint(localEndpointId, generation);
    }
    void reset() override {}
    uint64_t began = 0u;

private:
    FakeHub *hub_;
};

struct FakeClock {
    uint64_t nowMs = 1u;
    std::function<uint64_t()> fn() {
        return [this]() { return nowMs; };
    }
};

/* ---- Drive helpers ------------------------------------------------------- */

MdkrOnlineAdapterCommand cmd(IMdkrOnlineAdapter *a, MdkrOnlineViewAction action,
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

MdkrOnlineViewModel viewOf(IMdkrOnlineAdapter *a) {
    MdkrOnlineViewModel m{};
    a->view(&m);
    return m;
}

/* Pump a set of adapters (service()) with real 10 ms steps + a fake-clock step,
 * until `done` holds or the budget elapses. Real sleeps let the loopback DTLS
 * threads make progress, exactly like the O-T2 mesh harness. */
bool pumpUntil(std::vector<IMdkrOnlineAdapter *> adapters, FakeClock &clock,
               const std::function<bool()> &done, unsigned maxMs = 20000u) {
    for (unsigned elapsed = 0u; elapsed <= maxMs; elapsed += 10u) {
        for (IMdkrOnlineAdapter *a : adapters) a->service();
        if (done()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        clock.nowMs += 10u;
    }
    for (IMdkrOnlineAdapter *a : adapters) a->service();
    return done();
}

/* ---- Tests --------------------------------------------------------------- */

void test_token_gate_required() {
#if defined(_WIN32)
    _putenv_s("MDKR_INTERNAL_TEST_TOKEN", "");
#else
    unsetenv("MDKR_INTERNAL_TEST_TOKEN");
#endif
    CHECK(!mdkr_online_live_lobby_gate_open());
#if defined(_WIN32)
    _putenv_s("MDKR_INTERNAL_TEST_TOKEN", "mdkr64-online-live-v1");
#else
    setenv("MDKR_INTERNAL_TEST_TOKEN", "mdkr64-online-live-v1", 1);
#endif
    CHECK(mdkr_online_live_lobby_gate_open());
#if defined(_WIN32)
    _putenv_s("MDKR_INTERNAL_TEST_TOKEN", "wrong-token");
#else
    setenv("MDKR_INTERNAL_TEST_TOKEN", "wrong-token", 1);
#endif
    CHECK(!mdkr_online_live_lobby_gate_open());
}

MdkrOnlineLiveAdapterOptions baseOptions(FakeRoomTransport *room,
                                         HubMeshBackend *backend,
                                         FakeClock *clock,
                                         MdkrOnlineJourney journey) {
    MdkrOnlineLiveAdapterOptions o;
    o.sessionId = UINT64_C(0x4c495645);
    o.compatibility = compatibilityFixture();
    o.journey = journey;
    o.localSeatCount = 1u;
    o.joinCapability = "capabilitycapabilitycapabilitycapabilityXYZ0";
    for (unsigned i = 0u; i < MDKR_MATCH_LOCAL_PLAYER_SLOTS; ++i)
        o.localRoster.player_identity[i] = MDKR_MATCH_IDENTITY_RETAIL;
    o.raceAdmissionEnabled = true;
    o.romVerified = true;
    o.inputDelay = 2u;
    o.nowMs = clock->fn();
    o.room = room;
    o.meshBackend = backend;
    return o;
}

void test_create_room_view() {
    FakeMatchRoom room;
    FakeHub hub;
    FakeClock clock;
    FakeRoomTransport transport(&room, 1u);
    HubMeshBackend backend(&hub);
    std::string err;
    auto adapter = mdkr_online_live_adapter_create(
        baseOptions(&transport, &backend, &clock, MDKR_ONLINE_JOURNEY_CREATE),
        &err);
    CHECK(adapter != nullptr);
    if (!adapter) { std::fprintf(stderr, "create err: %s\n", err.c_str()); return; }

    const MdkrOnlineAdapterStep s = adapter->submit(
        cmd(adapter.get(), MDKR_ONLINE_VIEW_ACTION_CREATE_ROOM));
    CHECK(s.accepted);
    /* Connecting until the room replies. */
    CHECK(viewOf(adapter.get()).kind == MDKR_ONLINE_VIEW_CONNECTING);
    (void)pumpUntil({adapter.get()}, clock, [&]() {
        return viewOf(adapter.get()).kind == MDKR_ONLINE_VIEW_ROOM;
    }, 2000u);
    const MdkrOnlineViewModel m = viewOf(adapter.get());
    CHECK(m.kind == MDKR_ONLINE_VIEW_ROOM);
    CHECK(m.member_count == 1u);
    CHECK(adapter->sessionIntent() == MDKR_INTENT_ONLINE_PRIVATE);
    CHECK(adapter->journey() == MDKR_ONLINE_JOURNEY_CREATE);
}

void test_transport_failure_preserves_lobby() {
    FakeMatchRoom room;
    FakeHub hub;
    FakeClock clock;
    FakeRoomTransport transport(&room, 1u);
    HubMeshBackend backend(&hub);
    auto adapter = mdkr_online_live_adapter_create(
        baseOptions(&transport, &backend, &clock, MDKR_ONLINE_JOURNEY_CREATE));
    CHECK(adapter != nullptr);
    if (!adapter) return;
    adapter->submit(cmd(adapter.get(), MDKR_ONLINE_VIEW_ACTION_CREATE_ROOM));
    (void)pumpUntil({adapter.get()}, clock, [&]() {
        return viewOf(adapter.get()).kind == MDKR_ONLINE_VIEW_ROOM;
    }, 2000u);
    CHECK(viewOf(adapter.get()).member_count == 1u);

    transport.injectFailure(MDKR_ONLINE_VIEW_FAILURE_CONNECTION_CHECK);
    adapter->service();
    const MdkrOnlineViewModel m = viewOf(adapter.get());
    CHECK(m.kind == MDKR_ONLINE_VIEW_RECOVERY);
    CHECK(m.failure == MDKR_ONLINE_VIEW_FAILURE_CONNECTION_CHECK);
    /* Lobby state is retained: a Retry can recover without re-creating. */
    CHECK(m.member_count == 1u);
}

/* Drive two adapters from create/join through the loopback mesh to the Loading
 * barrier. `bonusIdentityOnA` seeds a non-retail local identity on A to force
 * the retail-identity clamp. Returns the two probes. */
struct FullRunResult {
    bool reachedLoading = false;
    MdkrOnlineLiveLaunchProbe probeA{};
    MdkrOnlineLiveLaunchProbe probeB{};
};

FullRunResult driveTwoAdapters(bool bonusIdentityOnA) {
    FullRunResult result;
    mdkr_net_roster_runtime_clear();

    FakeMatchRoom room;
    FakeHub hub;
    FakeClock clock;
    FakeRoomTransport transportA(&room, 1u);
    FakeRoomTransport transportB(&room, 1u);
    HubMeshBackend backendA(&hub);
    HubMeshBackend backendB(&hub);

    MdkrOnlineLiveAdapterOptions optA =
        baseOptions(&transportA, &backendA, &clock, MDKR_ONLINE_JOURNEY_CREATE);
    if (bonusIdentityOnA) optA.localRoster.player_identity[0] = 3u; /* bonus */
    MdkrOnlineLiveAdapterOptions optB =
        baseOptions(&transportB, &backendB, &clock, MDKR_ONLINE_JOURNEY_JOIN);

    auto A = mdkr_online_live_adapter_create(optA);
    auto B = mdkr_online_live_adapter_create(optB);
    CHECK(A != nullptr);
    CHECK(B != nullptr);
    if (!A || !B) return result;
    std::vector<IMdkrOnlineAdapter *> both{A.get(), B.get()};

    A->submit(cmd(A.get(), MDKR_ONLINE_VIEW_ACTION_CREATE_ROOM));
    if (!pumpUntil({A.get()}, clock, [&]() {
            return viewOf(A.get()).kind == MDKR_ONLINE_VIEW_ROOM;
        }, 3000u)) return result;

    B->submit(cmd(B.get(), MDKR_ONLINE_VIEW_ACTION_JOIN_ROOM));
    if (!pumpUntil(both, clock, [&]() {
            return viewOf(A.get()).member_count == 2u &&
                   viewOf(B.get()).member_count == 2u;
        }, 3000u)) return result;

    /* Check Setup -> preflight; brings up both meshes over the loopback hub. */
    A->submit(cmd(A.get(), MDKR_ONLINE_VIEW_ACTION_CHECK_SETUP));
    B->submit(cmd(B.get(), MDKR_ONLINE_VIEW_ACTION_CHECK_SETUP));
    hub.welcome(backendA.began);
    hub.welcome(backendB.began);

    if (!pumpUntil(both, clock, [&]() {
            return viewOf(A.get()).verification_phrase[0] != '\0' &&
                   viewOf(B.get()).verification_phrase[0] != '\0';
        }, 30000u)) {
        return result;
    }
    /* Both displays show the SAME phrase (transcript agreement). */
    CHECK(std::strcmp(viewOf(A.get()).verification_phrase,
                      viewOf(B.get()).verification_phrase) == 0);

    A->submit(cmd(A.get(), MDKR_ONLINE_VIEW_ACTION_CONFIRM_PHRASE));
    B->submit(cmd(B.get(), MDKR_ONLINE_VIEW_ACTION_CONFIRM_PHRASE));
    if (!pumpUntil(both, clock, [&]() {
            return viewOf(A.get()).kind == MDKR_ONLINE_VIEW_SELECTING &&
                   viewOf(B.get()).kind == MDKR_ONLINE_VIEW_SELECTING;
        }, 3000u)) {
        return result;
    }

    /* Selections + Ready for both endpoints (one seat each). */
    auto selectReady = [&](IMdkrOnlineAdapter *self, unsigned character) {
        auto until = [&](MdkrOnlineViewAction next) {
            return pumpUntil(both, clock, [&]() {
                return viewOf(self).primary.action == next;
            }, 5000u);
        };
        self->submit(
            cmd(self, MDKR_ONLINE_VIEW_ACTION_CHOOSE_CHARACTER, 0u, character));
        until(MDKR_ONLINE_VIEW_ACTION_CHOOSE_VEHICLE);
        self->submit(cmd(self, MDKR_ONLINE_VIEW_ACTION_CHOOSE_VEHICLE, 0u, 0u));
        until(MDKR_ONLINE_VIEW_ACTION_VOTE_TRACK);
        self->submit(cmd(self, MDKR_ONLINE_VIEW_ACTION_VOTE_TRACK, 0u, 5u));
        until(MDKR_ONLINE_VIEW_ACTION_READY);
        self->submit(cmd(self, MDKR_ONLINE_VIEW_ACTION_READY, 0u, 1u));
        (void)pumpUntil(both, clock, [&]() {
            return viewOf(self).primary.action !=
                   MDKR_ONLINE_VIEW_ACTION_READY;
        }, 5000u);
    };
    selectReady(A.get(), 1u); /* distinct characters: characters are unique */
    selectReady(B.get(), 2u);
    if (!pumpUntil(both, clock, [&]() {
            return viewOf(A.get()).ready_count == 2u &&
                   viewOf(B.get()).ready_count == 2u;
        }, 3000u)) {
        return result;
    }

    /* Leader starts the race -> BEGIN_LOADING; both follow the lobby phase. */
    A->submit(cmd(A.get(), MDKR_ONLINE_VIEW_ACTION_START_RACE, 0u, 1u));
    result.reachedLoading = pumpUntil(both, clock, [&]() {
        MdkrOnlineLiveLaunchProbe pa{}, pb{};
        mdkr_online_live_adapter_probe(A.get(), &pa);
        mdkr_online_live_adapter_probe(B.get(), &pb);
        if (bonusIdentityOnA) {
            /* A refuses at the clamp; B never reaches consensus without A. */
            return pa.descriptorBuilt || pa.refusal != MDKR_MATCH_LAUNCH_ADMITTED;
        }
        return pa.installed && pb.preflightReady;
    }, 30000u);

    mdkr_online_live_adapter_probe(A.get(), &result.probeA);
    mdkr_online_live_adapter_probe(B.get(), &result.probeB);
    return result;
}

void test_full_flow_installs_through_builder() {
    const FullRunResult r = driveTwoAdapters(/*bonusIdentityOnA=*/false);
    CHECK(r.reachedLoading);
    CHECK(r.probeA.phraseConfirmed);
    CHECK(r.probeA.descriptorBuilt);
    CHECK(r.probeA.refusal == MDKR_MATCH_LAUNCH_ADMITTED);
    CHECK(r.probeA.preflightReady);
    CHECK(r.probeA.installed);
    /* The descriptor was installed into the engine runtime through the builder. */
    CHECK(mdkr_net_roster_runtime_active());
    const MdkrMatchLaunchDescriptorV1 *installed =
        mdkr_net_roster_runtime_launch_descriptor();
    CHECK(installed != nullptr);
    if (installed) {
        CHECK(mdkr_match_launch_descriptor_validate(installed));
        /* Both peers froze the byte-identical descriptor (consensus). */
        CHECK(std::memcmp(installed, &r.probeA.descriptor,
                          sizeof(*installed)) == 0);
        CHECK(std::memcmp(&r.probeA.descriptor, &r.probeB.descriptor,
                          sizeof(r.probeA.descriptor)) == 0);
    }
    CHECK(r.probeB.preflightReady);
    CHECK(r.probeB.descriptorBuilt);
    mdkr_net_roster_runtime_clear();
}

void test_clamp_refuses_bonus_identity() {
    const FullRunResult r = driveTwoAdapters(/*bonusIdentityOnA=*/true);
    /* A reached the Loading barrier but the retail clamp refused the build. */
    CHECK(!r.probeA.descriptorBuilt);
    CHECK(r.probeA.refusal == MDKR_MATCH_LAUNCH_REFUSE_NON_RETAIL_IDENTITY);
    CHECK(!r.probeA.installed);
    /* No descriptor was installed for A. */
    CHECK(!r.probeA.preflightReady);
    mdkr_net_roster_runtime_clear();
}

}  // namespace

int main() {
    test_token_gate_required();
    test_create_room_view();
    test_transport_failure_preserves_lobby();
    test_full_flow_installs_through_builder();
    test_clamp_refuses_bonus_identity();
    std::fprintf(stderr, "online_live_adapter: %d checks, %d failures\n",
                 g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
