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

#include "net/net_impairment.h"
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

    /* A signaling reconnect: the relay tracks a strictly higher generation
     * for this endpoint from now on (does NOT touch the borrowed feed). */
    void setGeneration(uint64_t id, uint32_t generation) {
        std::lock_guard<std::mutex> lock(mutex_);
        endpoints_.at(id).generation = generation;
    }

    /* Inject one raw event into `target`'s inbox (re-welcome / stale-welcome
     * cases). */
    void inject(uint64_t target, MdkrMatchSignalEvent event) {
        std::lock_guard<std::mutex> lock(mutex_);
        endpoints_.at(target).feed->inbox.push_back(std::move(event));
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
        /* The live adapter now passes 0 to ADOPT the service-assigned
         * generation (O-T6 forward-fix). Mirror the real signal service, which
         * assigns each endpoint's first signal socket generation 1. */
        return hub_->addEndpoint(localEndpointId,
                                 generation != 0u ? generation : 1u);
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
    bool raceRun = false;
    bool raceConverged = false;
    uint32_t racedTicks = 0u;
    uint64_t hashA = 0u;
    uint64_t hashB = 0u;
    /* O2.2-sim impaired-matrix witnesses (unset on the unimpaired path). */
    bool bothReachedTarget = false;        /* both drained + confirmed target */
    uint32_t recoveryReason = 0u;          /* max over endpoints: 1 gap, 2 late */
    uint32_t recoveryFirstTick = 0u;
    uint32_t recoveryObservedTick = 0u;
    uint8_t recoverySlot = 0u;
    uint64_t inputEnvelopesA = 0u;         /* real opened INPUT envelopes A saw */
    uint64_t inputEnvelopesB = 0u;
    uint32_t transportAcceptedA = 0u;      /* remote frames folded through mesh */
    uint32_t transportAcceptedB = 0u;
    /* Seeded net_impairment carrier counters, summed over both directions. */
    uint64_t impSent = 0u;
    uint64_t impDropped = 0u;
    uint64_t impDuplicated = 0u;
    uint64_t impReordered = 0u;
    uint64_t impCorrupted = 0u;
    uint64_t impOutageDropped = 0u;
    uint64_t impThrottled = 0u;
    uint64_t impOverflow = 0u;
    uint64_t impCorruptDropped = 0u;       /* corrupt tokens the driver discarded */
    /* Honest real-input path capture (driveTwoAdapters realInput=true): a
     * mid-race committed canonical frame from each endpoint, plus each
     * endpoint's local canonical slot, so the caller can prove the injected
     * pads (not raceLocalSample, not neutral) were committed and converged. */
    bool realInputCaptured = false;
    uint32_t sampleTick = 0u;
    uint8_t localSlotA = 0u;
    uint8_t localSlotB = 0u;
    MdkrInputSet sampleFrameA{};
    MdkrInputSet sampleFrameB{};
};

/* One net_impairment matrix cell: a named carrier profile + a deterministic
 * seed and its expected honest outcome. */
enum MatrixExpect { MATRIX_CONVERGE, MATRIX_RECOVER, MATRIX_EITHER };
struct ImpairmentSpec {
    MdkrNetImpairmentProfileName profile;
    const char *name;
    MatrixExpect expect;
    uint64_t seed;
};

/* A tiny checksummed tick token carried through the impairment carrier. The
 * carrier's malformed path flips bit 0x80 of the LAST byte, so a trailing XOR
 * checksum over the tick bytes detects every corruption deterministically. The
 * token itself never crosses the mesh -- it only tells the driver WHICH tick's
 * real sealed bundle to transmit once the carrier says the datagram survived. */
constexpr size_t kTokBytes = 6u;
void encodeTok(uint8_t *b, uint32_t tick) {
    b[0] = 0x5au;
    b[1] = static_cast<uint8_t>(tick & 0xffu);
    b[2] = static_cast<uint8_t>((tick >> 8) & 0xffu);
    b[3] = static_cast<uint8_t>((tick >> 16) & 0xffu);
    b[4] = static_cast<uint8_t>((tick >> 24) & 0xffu);
    b[5] = static_cast<uint8_t>(b[1] ^ b[2] ^ b[3] ^ b[4] ^ 0xa5u);
}
bool decodeTok(const uint8_t *b, size_t len, uint32_t *tick) {
    if (len < kTokBytes || b[0] != 0x5au) return false;
    if (b[5] != static_cast<uint8_t>(b[1] ^ b[2] ^ b[3] ^ b[4] ^ 0xa5u))
        return false; /* corrupted datagram: the receiver discards it */
    *tick = static_cast<uint32_t>(b[1]) |
            (static_cast<uint32_t>(b[2]) << 8) |
            (static_cast<uint32_t>(b[3]) << 16) |
            (static_cast<uint32_t>(b[4]) << 24);
    return true;
}

/* FNV-1a fold of one confirmed canonical frame, over exactly the active slots
 * -- the ROM-free "engine advance" both endpoints agree on. */
void foldFrame(uint64_t &hash, uint32_t tick, uint8_t activeMask,
               const MdkrInputSet &frame) {
    auto mix = [&hash](uint64_t v) {
        for (unsigned b = 0u; b < 8u; ++b) {
            hash ^= (v >> (b * 8u)) & 0xffu;
            hash *= UINT64_C(1099511628211);
        }
    };
    mix(tick);
    for (unsigned slot = 0u; slot < MDKR_SESSION_MAX_PLAYERS; ++slot) {
        if ((activeMask & (1u << slot)) == 0u) continue;
        mix(frame.slots[slot].buttons);
        mix(static_cast<uint64_t>(static_cast<uint8_t>(frame.slots[slot].stick_x)));
        mix(static_cast<uint64_t>(static_cast<uint8_t>(frame.slots[slot].stick_y)));
    }
}

FullRunResult driveTwoAdapters(bool bonusIdentityOnA, unsigned raceTicks = 0u,
                               const ImpairmentSpec *imp = nullptr,
                               bool realInput = false) {
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

    /* O-T6 race: with both descriptors installed, feed real sealed input
     * bundles over the loopback mesh for `raceTicks` authored ticks and fold
     * every confirmed canonical frame into a per-endpoint FNV state hash. The
     * two independent endpoints must converge on the identical hash. */
    MdkrOnlineLiveRaceInfo ia{}, ib{};
    mdkr_online_live_adapter_race_info(A.get(), &ia);
    mdkr_online_live_adapter_race_info(B.get(), &ib);
    /* Both endpoints bring up their per-endpoint race transport even though the
     * process-global engine roster can hold only one (see install()). */
    if (raceTicks > 0u && ia.ready && ib.ready) {
        result.raceRun = true;
        auto lowestBit = [](uint8_t m) -> uint8_t {
            for (uint8_t b = 0u; b < 8u; ++b)
                if ((m >> b) & 1u) return b;
            return 0u;
        };
        result.localSlotA = lowestBit(ia.localSlotMask);
        result.localSlotB = lowestBit(ib.localSlotMask);
        if (realInput) {
            /* Honest path: drive each endpoint from a DISTINCT real controller
             * frame (neither raceLocalSample, which always holds 0x8000|hash bits
             * and a hashed stick, nor neutral) so the committed canonical inputs
             * must reflect the injected pads iff real local input reaches the sim
             * on both endpoints and crosses the mesh. Constant per endpoint, so
             * with the input delay every committed tick past the ramp equals it. */
            MdkrPadSample padA;
            padA.buttons = 0x8000u; padA.stick_x = 50; padA.stick_y = -20;
            padA.present = 1u;
            MdkrPadSample padB;
            padB.buttons = 0x4000u; padB.stick_x = -60; padB.stick_y = 30;
            padB.present = 1u;
            mdkr_online_live_adapter_race_set_local_input(A.get(), &padA, 1u);
            mdkr_online_live_adapter_race_set_local_input(B.get(), &padB, 1u);
        } else {
            /* Transport/rollback matrix: the deterministic fixture, which varies
             * per tick to force genuine corrections. */
            mdkr_online_live_adapter_race_set_synthetic_input(A.get(), true);
            mdkr_online_live_adapter_race_set_synthetic_input(B.get(), true);
        }
        const uint32_t target = ia.firstTick + raceTicks - 1u;
        const uint32_t kThrottle = 16u;
        uint32_t cursorA = ia.firstTick, cursorB = ib.firstTick;
        uint64_t hA = UINT64_C(1469598103934665603);
        uint64_t hB = UINT64_C(1469598103934665603);
        auto foldReady = [&](IMdkrOnlineAdapter *self, uint32_t &cursor,
                             uint8_t active, uint64_t &h) {
            MdkrInputSet frame;
            while (mdkr_online_live_adapter_race_inputs_for_tick(self, cursor,
                                                                 &frame)) {
                if ((frame.confirmed_mask & active) != active) break;
                foldFrame(h, cursor, active, frame);
                ++cursor;
            }
        };
        if (imp == nullptr) {
            for (unsigned step = 0u; step < 20000u; ++step) {
                /* service() before the tick drain -- the load-bearing pump
                 * ordering (match_live_adapter.h integration contract). */
                A->service();
                B->service();
                MdkrOnlineLiveRaceInfo na{}, nb{};
                mdkr_online_live_adapter_race_info(A.get(), &na);
                mdkr_online_live_adapter_race_info(B.get(), &nb);
                if (na.nextTick <= target && na.nextTick - cursorA < kThrottle) {
                    mdkr_online_live_adapter_race_advance(A.get());
                }
                if (nb.nextTick <= target && nb.nextTick - cursorB < kThrottle) {
                    mdkr_online_live_adapter_race_advance(B.get());
                }
                foldReady(A.get(), cursorA, ia.activeSlotMask, hA);
                foldReady(B.get(), cursorB, ib.activeSlotMask, hB);
                if (cursorA > target && cursorB > target) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                clock.nowMs += 2u;
            }
            result.racedTicks =
                (cursorA <= cursorB ? cursorA : cursorB) - ia.firstTick;
            result.hashA = hA;
            result.hashB = hB;
            result.raceConverged =
                cursorA > target && cursorB > target && hA == hB;
            if (realInput) {
                result.sampleTick = ia.firstTick + ia.inputDelay + 5u;
                result.realInputCaptured =
                    mdkr_online_live_adapter_race_inputs_for_tick(
                        A.get(), result.sampleTick, &result.sampleFrameA) &&
                    mdkr_online_live_adapter_race_inputs_for_tick(
                        B.get(), result.sampleTick, &result.sampleFrameB);
            }
            return result;
        }

        /* ---- O2.2-sim impaired path -------------------------------------- *
         *
         * The engine advances in real time (one drain per loop, predicting
         * through stalls via race_drain_local). Every mesh transmission is
         * gated by a seeded net_impairment carrier: the fresh 3-frame bundle
         * for `nextTick + inputDelay` is offered to the carrier each tick, and
         * ONLY the datagrams the carrier says survived trigger the genuine
         * race_resend (real seal -> real libdatachannel DTLS -> real fold), so
         * remote input still crosses the mesh -- impairment never shortcuts it.
         * Two carriers model the two directions; each is seeded per endpoint so
         * the whole matrix is reproducible with no wall-clock dependence. */
        MdkrNetImpairmentProfile prof;
        CHECK(mdkr_net_impairment_named_profile(imp->profile, 30u, &prof));
        MdkrNetImpairment simA; /* A -> B */
        MdkrNetImpairment simB; /* B -> A */
        mdkr_net_impairment_init(&simA, imp->seed ^ UINT64_C(0xA11CE),
                                 prof);
        mdkr_net_impairment_init(&simB, imp->seed ^ UINT64_C(0xB0B),
                                 prof);
        const uint8_t delay = ia.inputDelay;
        uint32_t simTick = ia.firstTick;
        for (unsigned step = 0u; step < 20000u; ++step) {
            A->service();
            B->service();
            MdkrOnlineLiveRaceInfo na{}, nb{};
            mdkr_online_live_adapter_race_info(A.get(), &na);
            mdkr_online_live_adapter_race_info(B.get(), &nb);
            /* Offer this tick's fresh bundle to each carrier (may drop / delay /
             * duplicate / corrupt / throttle it). */
            if (na.nextTick <= target) {
                uint8_t tok[kTokBytes];
                encodeTok(tok, na.nextTick + delay);
                (void)mdkr_net_impairment_send(&simA, simTick, 0u, 1u, tok,
                                               kTokBytes);
            }
            if (nb.nextTick <= target) {
                uint8_t tok[kTokBytes];
                encodeTok(tok, nb.nextTick + delay);
                (void)mdkr_net_impairment_send(&simB, simTick, 1u, 0u, tok,
                                               kTokBytes);
            }
            /* Real-time drain: keep predicting even while the carrier starves,
             * so a stall genuinely outruns the confirmed frontier. */
            if (na.nextTick <= target) {
                (void)mdkr_online_live_adapter_race_drain_local(A.get());
            }
            if (nb.nextTick <= target) {
                (void)mdkr_online_live_adapter_race_drain_local(B.get());
            }
            /* Deliver whatever survived the carrier THIS tick as a genuine mesh
             * send of that tick's real sealed bundle. */
            MdkrNetSimPacket pkt;
            while (mdkr_net_impairment_receive(&simA, simTick, 1u, &pkt)) {
                uint32_t t;
                if (decodeTok(pkt.bytes, pkt.length, &t)) {
                    (void)mdkr_online_live_adapter_race_resend(A.get(), t);
                } else {
                    ++result.impCorruptDropped;
                }
            }
            while (mdkr_net_impairment_receive(&simB, simTick, 0u, &pkt)) {
                uint32_t t;
                if (decodeTok(pkt.bytes, pkt.length, &t)) {
                    (void)mdkr_online_live_adapter_race_resend(B.get(), t);
                } else {
                    ++result.impCorruptDropped;
                }
            }
            foldReady(A.get(), cursorA, ia.activeSlotMask, hA);
            foldReady(B.get(), cursorB, ib.activeSlotMask, hB);
            MdkrOnlineLiveRaceStats sa{}, sb{};
            mdkr_online_live_adapter_race_stats(A.get(), &sa);
            mdkr_online_live_adapter_race_stats(B.get(), &sb);
            if (sa.recoveryReason != 0u || sb.recoveryReason != 0u) {
                const MdkrOnlineLiveRaceStats &s =
                    sa.recoveryReason != 0u ? sa : sb;
                result.recoveryReason = s.recoveryReason;
                result.recoveryFirstTick = s.recoveryFirstTick;
                result.recoveryObservedTick = s.recoveryObservedTick;
                result.recoverySlot = s.recoverySlot;
                break;
            }
            if (cursorA > target && cursorB > target) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            clock.nowMs += 2u;
            ++simTick;
        }
        result.racedTicks =
            (cursorA <= cursorB ? cursorA : cursorB) - ia.firstTick;
        result.hashA = hA;
        result.hashB = hB;
        result.bothReachedTarget = cursorA > target && cursorB > target;
        result.raceConverged = result.bothReachedTarget && hA == hB;
        MdkrOnlineLiveRaceStats sa{}, sb{};
        mdkr_online_live_adapter_race_stats(A.get(), &sa);
        mdkr_online_live_adapter_race_stats(B.get(), &sb);
        result.inputEnvelopesA = sa.inputEnvelopesReceived;
        result.inputEnvelopesB = sb.inputEnvelopesReceived;
        result.transportAcceptedA = sa.transportAccepted;
        result.transportAcceptedB = sb.transportAccepted;
        result.impSent = simA.sent + simB.sent;
        result.impDropped = simA.dropped + simB.dropped;
        result.impDuplicated = simA.duplicated + simB.duplicated;
        result.impReordered = simA.reordered + simB.reordered;
        result.impCorrupted = simA.corrupted + simB.corrupted;
        result.impOutageDropped = simA.outage_dropped + simB.outage_dropped;
        result.impThrottled = simA.throttled + simB.throttled;
        result.impOverflow = simA.overflow + simB.overflow;
    }
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

void test_two_endpoint_race_converges() {
    const FullRunResult r = driveTwoAdapters(/*bonusIdentityOnA=*/false,
                                             /*raceTicks=*/240u);
    CHECK(r.probeA.installed);  /* the process-global roster holder */
    CHECK(r.probeA.preflightReady && r.probeB.preflightReady);
    CHECK(r.raceRun);
    CHECK(r.racedTicks >= 240u);
    /* The headline: two independent endpoints, real sealed input over the mesh,
     * byte-identical converged state hash. */
    CHECK(r.raceConverged);
    CHECK(r.hashA == r.hashB);
    if (!r.raceConverged) {
        std::fprintf(stderr,
                     "race did not converge: ticks=%u hashA=%016llx "
                     "hashB=%016llx\n",
                     r.racedTicks, (unsigned long long)r.hashA,
                     (unsigned long long)r.hashB);
    }
    mdkr_net_roster_runtime_clear();
}

/* HONEST online-input gate. Every other online race test drives the deterministic
 * raceLocalSample fixture and asserts only convergence -- which two endpoints fed
 * identical canned input satisfy trivially, and which stayed green while the
 * shipped game ignored the controller entirely (both karts self-drove on the
 * fixture: 0x8000 accelerate + hashed steering every tick). This drives each
 * endpoint from a DISTINCT real controller frame through the production
 * race_set_local_input path and proves the committed canonical inputs ARE the
 * injected pads -- so real local input reaches the sim, crosses the mesh to the
 * peer, and still converges byte-for-byte. This is the regression guard for the
 * "cars self-drive / input ignored on both machines" defect. */
void test_two_endpoint_race_respects_real_input() {
    const FullRunResult r =
        driveTwoAdapters(/*bonusIdentityOnA=*/false, /*raceTicks=*/120u,
                         /*imp=*/nullptr, /*realInput=*/true);
    CHECK(r.raceRun);
    CHECK(r.racedTicks >= 120u);
    /* Still converges with real, distinct per-endpoint input. */
    CHECK(r.raceConverged);
    CHECK(r.hashA == r.hashB);
    CHECK(r.realInputCaptured);

    const MdkrPadSample &a = r.sampleFrameA.slots[r.localSlotA];
    const MdkrPadSample &b = r.sampleFrameA.slots[r.localSlotB];
    /* A's injected pad committed at A's canonical slot (not 0x8000|hash, not 0). */
    CHECK(a.buttons == 0x8000u);
    CHECK(a.stick_x == 50);
    CHECK(a.stick_y == -20);
    /* B's injected pad reached A over the mesh and committed at B's slot. */
    CHECK(b.buttons == 0x4000u);
    CHECK(b.stick_x == -60);
    CHECK(b.stick_y == 30);
    /* Both endpoints committed the identical local-slot pads (frame-level
     * convergence, beyond the fold hash). */
    const MdkrPadSample &a2 = r.sampleFrameB.slots[r.localSlotA];
    const MdkrPadSample &b2 = r.sampleFrameB.slots[r.localSlotB];
    CHECK(a.buttons == a2.buttons && a.stick_x == a2.stick_x &&
          a.stick_y == a2.stick_y);
    CHECK(b.buttons == b2.buttons && b.stick_x == b2.stick_x &&
          b.stick_y == b2.stick_y);
    if (r.realInputCaptured &&
        (a.buttons != 0x8000u || b.buttons != 0x4000u)) {
        std::fprintf(stderr,
                     "real input NOT respected: slotA(%u) buttons=%04x "
                     "stick=(%d,%d) slotB(%u) buttons=%04x stick=(%d,%d)\n",
                     r.localSlotA, a.buttons, a.stick_x, a.stick_y,
                     r.localSlotB, b.buttons, b.stick_x, b.stick_y);
    }
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

/* W3 fix round (Critical): a POST-CONFIRMATION re-welcome must force a fresh
 * SAS compare. docs/ref/match-signaling-v1.md:114 ("Secure connection
 * changed"): retire keys/channels, reconnect, compare a NEW phrase; never
 * reuse Ready. Pre-fix the adapter latched phraseConfirmed_ forever: a
 * replacement /signal socket after confirmation re-keyed the whole mesh and
 * play continued on channels the humans never re-verified. Pinned here end
 * to end: install both adapters, force A's re-welcome (higher generation),
 * assert BOTH sides drop race-Ready, reset confirmation, surface the
 * VERIFICATION_MISMATCH recovery ("Reconnect Securely"), re-surface a fresh
 * phrase (equal across sides, different from the confirmed one), and only a
 * SECOND confirm re-arms the preflight barrier and restores play on the
 * re-derived keys. Negative arm: a stale (equal-generation) welcome changes
 * nothing. */
void test_reverify_after_post_confirmation_rewelcome() {
    mdkr_net_roster_runtime_clear();
    FakeMatchRoom room;
    FakeHub hub;
    FakeClock clock;
    FakeRoomTransport transportA(&room, 1u);
    FakeRoomTransport transportB(&room, 1u);
    HubMeshBackend backendA(&hub);
    HubMeshBackend backendB(&hub);
    auto A = mdkr_online_live_adapter_create(
        baseOptions(&transportA, &backendA, &clock, MDKR_ONLINE_JOURNEY_CREATE));
    auto B = mdkr_online_live_adapter_create(
        baseOptions(&transportB, &backendB, &clock, MDKR_ONLINE_JOURNEY_JOIN));
    CHECK(A != nullptr && B != nullptr);
    if (!A || !B) return;
    std::vector<IMdkrOnlineAdapter *> both{A.get(), B.get()};

    A->submit(cmd(A.get(), MDKR_ONLINE_VIEW_ACTION_CREATE_ROOM));
    CHECK(pumpUntil({A.get()}, clock, [&]() {
        return viewOf(A.get()).kind == MDKR_ONLINE_VIEW_ROOM;
    }, 3000u));
    B->submit(cmd(B.get(), MDKR_ONLINE_VIEW_ACTION_JOIN_ROOM));
    CHECK(pumpUntil(both, clock, [&]() {
        return viewOf(A.get()).member_count == 2u &&
               viewOf(B.get()).member_count == 2u;
    }, 3000u));
    A->submit(cmd(A.get(), MDKR_ONLINE_VIEW_ACTION_CHECK_SETUP));
    B->submit(cmd(B.get(), MDKR_ONLINE_VIEW_ACTION_CHECK_SETUP));
    hub.welcome(backendA.began);
    hub.welcome(backendB.began);
    CHECK(pumpUntil(both, clock, [&]() {
        return viewOf(A.get()).verification_phrase[0] != '\0' &&
               viewOf(B.get()).verification_phrase[0] != '\0';
    }, 30000u));
    const std::string phraseBefore = viewOf(A.get()).verification_phrase;
    A->submit(cmd(A.get(), MDKR_ONLINE_VIEW_ACTION_CONFIRM_PHRASE));
    B->submit(cmd(B.get(), MDKR_ONLINE_VIEW_ACTION_CONFIRM_PHRASE));
    CHECK(pumpUntil(both, clock, [&]() {
        return viewOf(A.get()).kind == MDKR_ONLINE_VIEW_SELECTING &&
               viewOf(B.get()).kind == MDKR_ONLINE_VIEW_SELECTING;
    }, 3000u));
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
            return viewOf(self).primary.action != MDKR_ONLINE_VIEW_ACTION_READY;
        }, 5000u);
    };
    selectReady(A.get(), 1u);
    selectReady(B.get(), 2u);
    CHECK(pumpUntil(both, clock, [&]() {
        return viewOf(A.get()).ready_count == 2u &&
               viewOf(B.get()).ready_count == 2u;
    }, 3000u));
    A->submit(cmd(A.get(), MDKR_ONLINE_VIEW_ACTION_START_RACE, 0u, 1u));
    MdkrOnlineLiveRaceInfo ia{}, ib{};
    CHECK(pumpUntil(both, clock, [&]() {
        return mdkr_online_live_adapter_race_info(A.get(), &ia) && ia.ready &&
               mdkr_online_live_adapter_race_info(B.get(), &ib) && ib.ready;
    }, 30000u));

    /* Negative arm: a STALE (equal-generation) welcome changes nothing. */
    {
        MdkrMatchSignalEvent stale;
        stale.type = MdkrMatchSignalEventType::Welcome;
        stale.endpointId = std::to_string(backendA.began);
        stale.connectionGeneration = 1u; /* the current generation */
        MdkrMatchSignalPeerRef ref;
        ref.endpointId = std::to_string(backendB.began);
        ref.connectionGeneration = 1u;
        stale.peers.push_back(ref);
        hub.inject(backendA.began, stale);
    }
    for (unsigned index = 0u; index < 10u; index++) {
        for (IMdkrOnlineAdapter *a : both) a->service();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        clock.nowMs += 5u;
    }
    MdkrOnlineLiveLaunchProbe probeA{};
    mdkr_online_live_adapter_probe(A.get(), &probeA);
    CHECK(mdkr_online_live_adapter_race_info(A.get(), &ia) && ia.ready);
    CHECK(probeA.phraseConfirmed);
    CHECK(viewOf(A.get()).kind != MDKR_ONLINE_VIEW_RECOVERY);

    /* The replacement socket: the relay assigns A a strictly higher
     * generation and announces it to B as a presence bump. */
    hub.setGeneration(backendA.began, 4u);
    MdkrMatchSignalEvent bump;
    bump.type = MdkrMatchSignalEventType::PeerPresence;
    bump.endpointId = std::to_string(backendA.began);
    bump.connectionGeneration = 4u;
    bump.present = true;
    hub.inject(backendB.began, bump);
    hub.welcome(backendA.began); /* the fresh welcome, generation 4 */

    /* Both sides must drop race-Ready and reset the confirmation... */
    CHECK(pumpUntil(both, clock, [&]() {
        MdkrOnlineLiveRaceInfo na{}, nb{};
        return mdkr_online_live_adapter_race_info(A.get(), &na) &&
               mdkr_online_live_adapter_race_info(B.get(), &nb) &&
               !na.ready && !nb.ready;
    }, 15000u));
    MdkrOnlineLiveLaunchProbe probeB{};
    mdkr_online_live_adapter_probe(A.get(), &probeA);
    mdkr_online_live_adapter_probe(B.get(), &probeB);
    CHECK(!probeA.phraseConfirmed);
    CHECK(!probeB.phraseConfirmed);
    /* ...on the documented "Secure connection changed" recovery surface. */
    CHECK(viewOf(A.get()).kind == MDKR_ONLINE_VIEW_RECOVERY);
    CHECK(viewOf(A.get()).failure ==
          MDKR_ONLINE_VIEW_FAILURE_VERIFICATION_MISMATCH);
    CHECK(viewOf(B.get()).kind == MDKR_ONLINE_VIEW_RECOVERY);
    CHECK(viewOf(B.get()).failure ==
          MDKR_ONLINE_VIEW_FAILURE_VERIFICATION_MISMATCH);

    /* MINOR: a SECOND re-welcome DURING the re-verify window (before the
     * human retries) must be absorbed -- the barrier stays armed, the
     * confirmation stays reset, neither side escapes to play, and RETRY then
     * surfaces the LATEST transcript's phrase. */
    hub.setGeneration(backendA.began, 6u);
    MdkrMatchSignalEvent bump2;
    bump2.type = MdkrMatchSignalEventType::PeerPresence;
    bump2.endpointId = std::to_string(backendA.began);
    bump2.connectionGeneration = 6u;
    bump2.present = true;
    hub.inject(backendB.began, bump2);
    hub.welcome(backendA.began); /* second fresh welcome, generation 6 */
    for (unsigned index = 0u; index < 40u; index++) {
        for (IMdkrOnlineAdapter *a : both) a->service();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        clock.nowMs += 5u;
    }
    mdkr_online_live_adapter_probe(A.get(), &probeA);
    mdkr_online_live_adapter_probe(B.get(), &probeB);
    CHECK(!probeA.phraseConfirmed);
    CHECK(!probeB.phraseConfirmed);
    CHECK(viewOf(A.get()).kind == MDKR_ONLINE_VIEW_RECOVERY);
    CHECK(viewOf(B.get()).kind == MDKR_ONLINE_VIEW_RECOVERY);
    {
        MdkrOnlineLiveRaceInfo na{}, nb{};
        CHECK(mdkr_online_live_adapter_race_info(A.get(), &na) && !na.ready);
        CHECK(mdkr_online_live_adapter_race_info(B.get(), &nb) && !nb.ready);
    }

    /* "Reconnect Securely" -> a FRESH phrase surfaces on both sides. */
    CHECK(A->submit(cmd(A.get(), MDKR_ONLINE_VIEW_ACTION_RETRY)).accepted);
    CHECK(B->submit(cmd(B.get(), MDKR_ONLINE_VIEW_ACTION_RETRY)).accepted);
    CHECK(pumpUntil(both, clock, [&]() {
        return viewOf(A.get()).verification_phrase[0] != '\0' &&
               viewOf(B.get()).verification_phrase[0] != '\0';
    }, 30000u));
    const std::string phraseAfterA = viewOf(A.get()).verification_phrase;
    const std::string phraseAfterB = viewOf(B.get()).verification_phrase;
    CHECK(phraseAfterA == phraseAfterB);
    CHECK(phraseAfterA != phraseBefore); /* never reuse the retired SAS */

    /* Only the SECOND confirm re-arms the barrier and restores play. */
    A->submit(cmd(A.get(), MDKR_ONLINE_VIEW_ACTION_CONFIRM_PHRASE));
    B->submit(cmd(B.get(), MDKR_ONLINE_VIEW_ACTION_CONFIRM_PHRASE));
    CHECK(pumpUntil(both, clock, [&]() {
        return mdkr_online_live_adapter_race_info(A.get(), &ia) && ia.ready &&
               mdkr_online_live_adapter_race_info(B.get(), &ib) && ib.ready;
    }, 30000u));
    mdkr_online_live_adapter_probe(A.get(), &probeA);
    mdkr_online_live_adapter_probe(B.get(), &probeB);
    CHECK(probeA.phraseConfirmed);
    CHECK(probeB.phraseConfirmed);

    /* And the re-derived keys genuinely carry input: the rebuilt race
     * transport confirms its first tick across both slots. */
    bool confirmed = false;
    for (unsigned step = 0u; step < 2000u && !confirmed; ++step) {
        A->service();
        B->service();
        MdkrOnlineLiveRaceInfo na{}, nb{};
        mdkr_online_live_adapter_race_info(A.get(), &na);
        mdkr_online_live_adapter_race_info(B.get(), &nb);
        if (na.nextTick < na.firstTick + 8u) {
            mdkr_online_live_adapter_race_advance(A.get());
        }
        if (nb.nextTick < nb.firstTick + 8u) {
            mdkr_online_live_adapter_race_advance(B.get());
        }
        MdkrInputSet frame;
        if (mdkr_online_live_adapter_race_inputs_for_tick(A.get(),
                                                          na.firstTick,
                                                          &frame) &&
            (frame.confirmed_mask & na.activeSlotMask) == na.activeSlotMask) {
            confirmed = true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        clock.nowMs += 2u;
    }
    CHECK(confirmed);
    mdkr_net_roster_runtime_clear();
}

/* Seed derived from the profile index only -- never wall-clock -- so every
 * matrix cell is byte-reproducible across CI reruns. */
uint64_t matrixSeed(unsigned profileIndex) {
    return UINT64_C(0x9E3779B97F4A7C15) ^
           (static_cast<uint64_t>(profileIndex) * UINT64_C(0x100000001B3));
}

/* O2.2-sim headline: the O-T6 two-endpoint mesh race run under EACH of the six
 * named net_impairment profiles. Convergent profiles must still reach a
 * byte-identical confirmed state hash within the 30-tick rollback window;
 * profiles that exceed the window (the 2 s outage) must fire the TYPED recovery
 * path (INPUT_GAP / LATE_INPUT) instead of a silent desync. Impairment is a
 * pure test injection: it only decides which real sealed bundles cross the real
 * DTLS mesh and when -- the confirmed frames folded into the hash are always
 * genuine remote input that traversed the mesh. */
void test_impairment_matrix() {
    const ImpairmentSpec specs[] = {
        {MDKR_NET_PROFILE_LAN, "lan", MATRIX_CONVERGE, matrixSeed(0u)},
        {MDKR_NET_PROFILE_REGIONAL_GOOD, "regional-good", MATRIX_CONVERGE,
         matrixSeed(1u)},
        {MDKR_NET_PROFILE_REGIONAL_VARIABLE, "regional-variable",
         MATRIX_CONVERGE, matrixSeed(2u)},
        {MDKR_NET_PROFILE_POOR, "poor", MATRIX_CONVERGE, matrixSeed(3u)},
        {MDKR_NET_PROFILE_TWO_SECOND_OUTAGE, "two-second-outage",
         MATRIX_RECOVER, matrixSeed(4u)},
        {MDKR_NET_PROFILE_ADVERSARIAL, "adversarial", MATRIX_EITHER,
         matrixSeed(5u)},
    };
    for (const ImpairmentSpec &spec : specs) {
        const FullRunResult r =
            driveTwoAdapters(/*bonusIdentityOnA=*/false, /*raceTicks=*/240u,
                             &spec);
        /* The stack came up and installed under this profile. */
        CHECK(r.probeA.installed);
        CHECK(r.probeA.preflightReady && r.probeB.preflightReady);
        CHECK(r.raceRun);
        /* The carrier was genuinely in the path (non-vacuous) and never
         * overflowed its bounded schedule buffer. */
        CHECK(r.impSent > 0u);
        CHECK(r.impOverflow == 0u);
        /* Remote input really crossed the mesh -- impairment did not shortcut
         * the convergence proof. */
        CHECK(r.inputEnvelopesA > 0u && r.inputEnvelopesB > 0u);
        CHECK(r.transportAcceptedA > 0u && r.transportAcceptedB > 0u);

        const bool recovered = r.recoveryReason != 0u;
        const bool converged = r.raceConverged && !recovered;
        const bool silentDesync = r.bothReachedTarget && r.hashA != r.hashB;
        /* The one thing that must NEVER pass: both endpoints ran to the target
         * yet folded different state. */
        CHECK(!silentDesync);

        bool outcomeOk = false;
        const char *outcome = "none";
        switch (spec.expect) {
            case MATRIX_CONVERGE:
                outcomeOk = converged;
                outcome = "converged";
                break;
            case MATRIX_RECOVER:
                outcomeOk = recovered && !r.raceConverged;
                outcome = "recovered";
                break;
            case MATRIX_EITHER:
                outcomeOk = converged || recovered;
                outcome = converged ? "converged" : "recovered";
                break;
        }
        CHECK(outcomeOk);

        if (converged) {
            CHECK(r.hashA == r.hashB);
            CHECK(r.racedTicks >= 240u);
        }
        /* Profiles carrying loss must have dropped at least one datagram, so
         * the loss arm is not a no-op with this seed. */
        if (spec.profile == MDKR_NET_PROFILE_REGIONAL_VARIABLE ||
            spec.profile == MDKR_NET_PROFILE_POOR ||
            spec.profile == MDKR_NET_PROFILE_ADVERSARIAL) {
            CHECK(r.impDropped > 0u);
        }
        if (spec.profile == MDKR_NET_PROFILE_ADVERSARIAL) {
            /* The 1-delivery/tick throttle and the malformed arm both bite. */
            CHECK(r.impThrottled > 0u);
            CHECK(r.impCorrupted > 0u);
            CHECK(r.impCorruptDropped > 0u);
        }
        if (spec.profile == MDKR_NET_PROFILE_TWO_SECOND_OUTAGE) {
            /* The outage genuinely blacked the carrier out and the transport
             * latched INPUT_GAP once the stall outran the retained window. */
            CHECK(r.impOutageDropped > 0u);
            CHECK(r.recoveryReason == 1u); /* INPUT_GAP */
            CHECK(r.recoveryObservedTick - r.recoveryFirstTick >= 31u);
        }

        std::fprintf(
            stderr,
            "[MATRIX] profile=%s outcome=%s ticks=%u hashA=%016llx "
            "hashB=%016llx recovery=%u firstTick=%u observedTick=%u slot=%u "
            "sent=%llu dropped=%llu dup=%llu reorder=%llu corrupt=%llu "
            "outage=%llu throttled=%llu envsA=%llu envsB=%llu "
            "acceptedA=%u acceptedB=%u\n",
            spec.name, outcome, r.racedTicks,
            (unsigned long long)r.hashA, (unsigned long long)r.hashB,
            r.recoveryReason, r.recoveryFirstTick, r.recoveryObservedTick,
            (unsigned)r.recoverySlot,
            (unsigned long long)r.impSent, (unsigned long long)r.impDropped,
            (unsigned long long)r.impDuplicated,
            (unsigned long long)r.impReordered,
            (unsigned long long)r.impCorrupted,
            (unsigned long long)r.impOutageDropped,
            (unsigned long long)r.impThrottled,
            (unsigned long long)r.inputEnvelopesA,
            (unsigned long long)r.inputEnvelopesB,
            r.transportAcceptedA, r.transportAcceptedB);
        mdkr_net_roster_runtime_clear();
    }
}

}  // namespace

int main(int argc, char **argv) {
    bool matrixOnly = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--matrix") == 0) matrixOnly = true;
    }
    /* The token gate must be open for the live adapter to construct. The
     * matrix lane runs in its own process, so set it there too. */
#if defined(_WIN32)
    _putenv_s("MDKR_INTERNAL_TEST_TOKEN", "mdkr64-online-live-v1");
#else
    setenv("MDKR_INTERNAL_TEST_TOKEN", "mdkr64-online-live-v1", 1);
#endif
    if (matrixOnly) {
        test_impairment_matrix();
        std::fprintf(stderr, "online_live_matrix: %d checks, %d failures\n",
                     g_checks, g_failures);
        return g_failures == 0 ? 0 : 1;
    }
    test_token_gate_required();
    test_create_room_view();
    test_transport_failure_preserves_lobby();
    test_full_flow_installs_through_builder();
    test_two_endpoint_race_converges();
    test_two_endpoint_race_respects_real_input();
    test_clamp_refuses_bonus_identity();
    test_reverify_after_post_confirmation_rewelcome();
    std::fprintf(stderr, "online_live_adapter: %d checks, %d failures\n",
                 g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
