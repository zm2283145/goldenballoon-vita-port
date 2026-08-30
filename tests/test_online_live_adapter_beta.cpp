/* Beta-ON unit test for the live adapter's race-end-card truthfulness decisions.
 *
 * P1-T2 F4: no other test compiles platform/online/match_live_adapter.cpp WITH
 * MDKR_ENABLE_ONLINE_BETA (the app target does; mdkr_online_live_adapter_test
 * deliberately does NOT -- defining the macro there breaks
 * test_token_gate_required). That test hole is exactly why the F1 demotion bug
 * shipped. This SEPARATE, beta-ON target pins the two pure decisions that only
 * exist under the beta macro:
 *   - mapLostReason's in-race truthfulness branches (a peer that leaves after
 *     the race is racing is OPPONENT_LEFT / a mid-race transport breakdown is
 *     CONNECTION_UNPLAYABLE; pre-connection losses keep the old copy), and
 *   - the F1 no-demotion rule (the drain's reason-blind OPPONENT_LEFT must not
 *     overwrite an already-latched, more-specific CONNECTION_UNPLAYABLE).
 *
 * Both are exposed as pure test-only wrappers so this stays a fast, mesh-free
 * unit test. The two engine-boot-handoff hooks the beta build of the adapter
 * references live in the heavy wiring TU; stub them here so we need not link it.
 */
#include "online/match_live_adapter.h"

#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>

/* Beta build references these (resetRaceLatches / setUpRace boot handoff +
 * retractRaceBoot); the decisions under test never invoke them, so no-op stubs
 * suffice. Exit-gate C2 dropped the ~LiveAdapter room-ready/race-boot backstop,
 * so the destructor no longer references the registry (the launcher thread
 * retracts both synchronously before teardown). */
void OnlineRoom_publishEngineRaceBoot(IMdkrOnlineAdapter *) {}
void OnlineRoom_retractEngineRaceBoot(IMdkrOnlineAdapter *) {}

static int g_failures = 0;
static int g_checks = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        ++g_checks;                                                            \
        if (!(cond)) {                                                         \
            ++g_failures;                                                      \
            std::fprintf(stderr, "CHECK failed: %s (%s:%d)\n", #cond,          \
                         __FILE__, __LINE__);                                  \
        }                                                                      \
    } while (0)

static MdkrOnlineViewFailure mapLost(MdkrMatchPeerLostReason r, bool raceBegun) {
    return mdkr_online_live_adapter_test_map_lost_reason(r, raceBegun);
}

/* mapLostReason: in-race losses tell a truthful, DIFFERENT story than the
 * pre-connection copy, and a mid-race transport breakdown is distinct from an
 * opponent leaving. */
static void test_map_lost_reason_in_race_branches() {
    /* Once the race is up, a peer that pings-out or ends "disconnected". */
    CHECK(mapLost(MdkrMatchPeerLostReason::PingTimeout, true) ==
          MDKR_ONLINE_VIEW_FAILURE_OPPONENT_LEFT);
    CHECK(mapLost(MdkrMatchPeerLostReason::PeerEnded, true) ==
          MDKR_ONLINE_VIEW_FAILURE_OPPONENT_LEFT);
    /* A VANISHED peer (signal presence dropped + transport down, the
     * mid-race kill/quit signature) DEPARTED: truthful OPPONENT_LEFT, never
     * a connection-establishment demotion. */
    CHECK(mapLost(MdkrMatchPeerLostReason::PeerVanished, true) ==
          MDKR_ONLINE_VIEW_FAILURE_OPPONENT_LEFT);
    /* A seal-window exhaustion mid-race is a transport breakdown, NOT "opponent
     * left" and NOT "could not establish". */
    CHECK(mapLost(MdkrMatchPeerLostReason::SealWindowExhausted, true) ==
          MDKR_ONLINE_VIEW_FAILURE_CONNECTION_UNPLAYABLE);

    /* Pre-connection: unchanged establishment copy. */
    CHECK(mapLost(MdkrMatchPeerLostReason::PingTimeout, false) ==
          MDKR_ONLINE_VIEW_FAILURE_CONNECTION_CHECK);
    CHECK(mapLost(MdkrMatchPeerLostReason::PeerEnded, false) ==
          MDKR_ONLINE_VIEW_FAILURE_CONNECTION_CHECK);
    CHECK(mapLost(MdkrMatchPeerLostReason::PeerVanished, false) ==
          MDKR_ONLINE_VIEW_FAILURE_CONNECTION_CHECK);
    CHECK(mapLost(MdkrMatchPeerLostReason::SealWindowExhausted, false) ==
          MDKR_ONLINE_VIEW_FAILURE_CONNECTION_CHECK);

    /* LINGERING-PRESENCE mid-race loss (the audit's defect #1): ICE tears
     * down while the peer's signal presence is still asserted (Worker slow to
     * drop it, or the Worker itself down), so the survivor runs the restart
     * ladder to ConnectTimeout -- or TransportFailed when signaling is also
     * gone. A playable race EXISTED, so the truthful card is OPPONENT_LEFT,
     * exactly like the ping/vanish reasons; only a pre-race loss keeps the
     * establishment copy. */
    CHECK(mapLost(MdkrMatchPeerLostReason::ConnectTimeout, true) ==
          MDKR_ONLINE_VIEW_FAILURE_OPPONENT_LEFT);
    CHECK(mapLost(MdkrMatchPeerLostReason::ConnectTimeout, false) ==
          MDKR_ONLINE_VIEW_FAILURE_NETWORKS_CANNOT_CONNECT);
    CHECK(mapLost(MdkrMatchPeerLostReason::TransportFailed, true) ==
          MDKR_ONLINE_VIEW_FAILURE_OPPONENT_LEFT);
    CHECK(mapLost(MdkrMatchPeerLostReason::TransportFailed, false) ==
          MDKR_ONLINE_VIEW_FAILURE_NETWORKS_CANNOT_CONNECT);
    CHECK(mapLost(MdkrMatchPeerLostReason::CommitmentMismatch, true) ==
          MDKR_ONLINE_VIEW_FAILURE_VERIFICATION_MISMATCH);
    CHECK(mapLost(MdkrMatchPeerLostReason::ControlChannelViolation, false) ==
          MDKR_ONLINE_VIEW_FAILURE_VERIFICATION_MISMATCH);
}

/* F1: the drain reports a reason-blind OpponentLeft (it only polls
 * race_peer_lost()), so setRaceEndFailure(OPPONENT_LEFT) must NOT demote a
 * more-specific CONNECTION_UNPLAYABLE the mesh already latched -- otherwise a
 * transport breakdown is mislabeled "your opponent lost connection". */
static void test_race_end_no_demotion_rule() {
    /* The one case that must be kept. */
    CHECK(mdkr_online_live_adapter_test_race_end_demotes(
        MDKR_ONLINE_VIEW_FAILURE_OPPONENT_LEFT,
        MDKR_ONLINE_VIEW_FAILURE_CONNECTION_UNPLAYABLE));
    /* OPPONENT_LEFT over a plain in-race latch (or none) still applies. */
    CHECK(!mdkr_online_live_adapter_test_race_end_demotes(
        MDKR_ONLINE_VIEW_FAILURE_OPPONENT_LEFT,
        MDKR_ONLINE_VIEW_FAILURE_OPPONENT_LEFT));
    CHECK(!mdkr_online_live_adapter_test_race_end_demotes(
        MDKR_ONLINE_VIEW_FAILURE_OPPONENT_LEFT,
        MDKR_ONLINE_VIEW_FAILURE_NONE));
    /* The barrier-abort card (OPPONENT_NEVER_STARTED) is not the reason-blind
     * one and never demotes -- it always sets its own copy. */
    CHECK(!mdkr_online_live_adapter_test_race_end_demotes(
        MDKR_ONLINE_VIEW_FAILURE_OPPONENT_NEVER_STARTED,
        MDKR_ONLINE_VIEW_FAILURE_CONNECTION_UNPLAYABLE));
    /* Applying the specific card over a generic one is never a demotion. */
    CHECK(!mdkr_online_live_adapter_test_race_end_demotes(
        MDKR_ONLINE_VIEW_FAILURE_CONNECTION_UNPLAYABLE,
        MDKR_ONLINE_VIEW_FAILURE_OPPONENT_LEFT));
}

/* Whole-phase regression: a FIRST-race SAS mismatch drives re-verify ->
 * re-confirm -> SELECTING -> BEGIN_LOADING WITHOUT ever running
 * resetRaceLatches (the belt-and-braces reset only fires once race latches are
 * armed, which they are not before any loading). So a peer-loss latched during
 * the mismatch teardown must be cleared by the re-verify entry points
 * themselves; otherwise the re-confirmed race's start barrier reads a stale
 * race_peer_lost(), aborts, and kicks the HEALTHY peer with a false "Your
 * Opponent Couldn't Start" card. Cover both entry points and both latch
 * sources (racePeerLost_ and the folded-in received-abort latch). */
static void test_reverify_paths_clear_stale_peer_loss() {
    CHECK(!mdkr_online_live_adapter_test_rekey_clears_peer_loss(false));
    CHECK(!mdkr_online_live_adapter_test_rekey_clears_peer_loss(true));
    CHECK(!mdkr_online_live_adapter_test_reverify_clears_peer_loss(false));
    CHECK(!mdkr_online_live_adapter_test_reverify_clears_peer_loss(true));
}

/* RETRY must genuinely retry (audit story gap #2). Pre-Ready -- the create/
 * join round trip failed or stalled, the transport worker is gone -- a RETRY
 * must hand the panel the rebuild sentinel (kMdkrOnlineLiveStepRetryRebuild)
 * so a FRESH adapter re-runs the same create/join journey, never merely clear
 * the failure back to a spinner over a dead transport. On a live room the
 * retained-room clear stays; a PREFLIGHT timeout with no latched failure
 * re-attempts the secure setup (accepted, no sentinel) instead of refusing. */
static void test_retry_genuinely_retries() {
    bool accepted = false;
    /* Tier 0: pre-Ready with a latched failure -> rebuild sentinel. */
    CHECK(mdkr_online_live_adapter_test_retry_step(0u, &accepted) ==
          kMdkrOnlineLiveStepRetryRebuild);
    CHECK(accepted);
    /* Tier 1: pre-Ready with nothing to retry -> refused (no false hope). */
    CHECK(mdkr_online_live_adapter_test_retry_step(1u, &accepted) == 0u);
    CHECK(!accepted);
    /* Tier 2: pre-Ready, no failure, view timeout expired (the "Room Took
     * Too Long" card's Try Again) -> rebuild sentinel. */
    CHECK(mdkr_online_live_adapter_test_retry_step(2u, &accepted) ==
          kMdkrOnlineLiveStepRetryRebuild);
    CHECK(accepted);
    /* Tier 3: live room with a latched failure -> the retained-room clear
     * (accepted, NO sentinel) -- the soft-recovery contract is unchanged. */
    CHECK(mdkr_online_live_adapter_test_retry_step(3u, &accepted) == 0u);
    CHECK(accepted);
    /* Tier 4: live room at PREFLIGHT, no failure, timeout expired (the
     * "Setup Check Took Too Long" card's Retry Checks) -> accepted genuine
     * secure-setup re-attempt, NO sentinel. */
    CHECK(mdkr_online_live_adapter_test_retry_step(4u, &accepted) == 0u);
    CHECK(accepted);
}

/* ---- Owning-wrapper accessor regression --------------------------------- *
 *
 * Production holds the live adapter as an OwningLiveAdapter wrapper
 * (platform/app/online_live_wiring.cpp), a SIBLING of the concrete LiveAdapter
 * under IMdkrOnlineAdapter. The mdkr_online_live_adapter_* C accessors used to
 * dynamic_cast their argument to the concrete LiveAdapter -- a sibling
 * cross-cast that yields nullptr on the wrapper -- so every call failed closed
 * in production with no error, making the whole native online flow dead code.
 * This test builds the adapter the way production does (a wrapper around a real
 * LiveAdapter) and pins that the accessors resolve THROUGH the wrapper. The
 * wrapper below is a faithful, minimal stand-in for OwningLiveAdapter: it links
 * neither the heavy wiring TU nor its production transport, so it constructs the
 * inner LiveAdapter directly over an in-process MatchRoom double, exactly as the
 * loopback lane (tests/test_online_live_adapter.cpp) does. */

/* In-process MatchRoom double driving the REAL lobby reducer. */
struct FakeMatchRoom {
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
        MdkrOnlineCommand c;
        std::memset(&c, 0, sizeof(c));
        c.protocol_version = MDKR_ONLINE_PROTOCOL_VERSION;
        c.expected_revision = lobby.revision;
        c.command_id = 1u;
        c.actor_endpoint_id = ep;
        c.type = MDKR_ONLINE_JOIN;
        c.value = seats;
        c.compatibility = compat;
        const MdkrOnlineStep step = mdkr_online_lobby_dispatch(&lobby, &c);
        if (!step.accepted) {
            --nextEndpoint;
            return 0u;
        }
        return ep;
    }

    MdkrOnlineStep command(const MdkrOnlineCommand &in) {
        MdkrOnlineCommand c = in;
        return mdkr_online_lobby_dispatch(&lobby, &c);
    }
};

/* Minimal room transport over the double: CREATE -> Ready(with lobby), and any
 * submitted command echoes a synchronous CommandResult. */
class FakeRoomTransport final : public MdkrOnlineRoomTransport {
public:
    explicit FakeRoomTransport(FakeMatchRoom *room) : room_(room) {}

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
        ev.commandId = command.command_id;
        queue_.push_back(ev);
        return true;
    }

    void pump(std::vector<MdkrOnlineRoomEvent> &out) override {
        out.clear();
        if (kind_ != Kind::None && !ready_) {
            const uint64_t ep = kind_ == Kind::Create
                                    ? room_->create(compat_, seats_)
                                    : room_->join(compat_, seats_);
            MdkrOnlineRoomEvent ev;
            if (ep == 0u) {
                ev.type = MdkrOnlineRoomEvent::Type::Failure;
                ev.failure = MDKR_ONLINE_VIEW_FAILURE_SERVICE_UNAVAILABLE;
                out.push_back(ev);
                return;
            }
            ev.type = MdkrOnlineRoomEvent::Type::Ready;
            ev.localEndpointId = ep;
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

/* The LOBBY drive never reaches the Loading barrier, so the mesh backend is
 * never asked to begin signaling; a null-returning stub suffices. */
class StubMeshBackend final : public MdkrOnlineMeshSignalBackend {
public:
    MdkrMatchPeerSignalFeed *beginSignaling(
        uint64_t, uint32_t, const std::string &, const std::string &,
        const std::vector<MdkrMatchPeerIceServer> &) override {
        return nullptr;
    }
    void reset() override {}
};

struct FakeClock {
    uint64_t nowMs = 1u;
    std::function<uint64_t()> fn() {
        return [this]() { return nowMs; };
    }
};

/* Faithful minimal stand-in for OwningLiveAdapter: owns an inner adapter and
 * delegates every IMdkrOnlineAdapter call to it. (The mdkrResolveLive hook the
 * fix adds is overridden below during the GREEN step.) */
class TestOwningWrapper final : public IMdkrOnlineAdapter {
public:
    explicit TestOwningWrapper(std::unique_ptr<IMdkrOnlineAdapter> inner)
        : inner_(std::move(inner)) {}

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
    /* Mirror OwningLiveAdapter: forward the downcast hook to the inner adapter so
     * the C accessors resolve the concrete LiveAdapter through the wrapper. */
    LiveAdapter *mdkrResolveLive() override {
        return inner_ ? inner_->mdkrResolveLive() : nullptr;
    }
    const LiveAdapter *mdkrResolveLive() const override {
        const IMdkrOnlineAdapter *in = inner_.get();
        return in != nullptr ? in->mdkrResolveLive() : nullptr;
    }

private:
    std::unique_ptr<IMdkrOnlineAdapter> inner_;
};

static MdkrOnlineCompatibilityV1 compatibilityFixture() {
    MdkrOnlineCompatibilityV1 c;
    std::memset(&c, 0, sizeof(c));
    c.protocol_version = MDKR_ONLINE_PROTOCOL_VERSION;
    for (unsigned i = 0u; i < sizeof(c.build_id); ++i)
        c.build_id[i] = static_cast<uint8_t>(i + 1u);
    for (unsigned i = 0u; i < sizeof(c.gameplay_digest); ++i)
        c.gameplay_digest[i] = static_cast<uint8_t>(0x80u + i);
    c.rom_revision = 1u;
    c.cadence_hz = 30u;
    return c;
}

static MdkrOnlineLiveAdapterOptions baseOptions(FakeRoomTransport *room,
                                                StubMeshBackend *backend,
                                                FakeClock *clock,
                                                MdkrOnlineJourney journey) {
    MdkrOnlineLiveAdapterOptions o;
    o.sessionId = UINT64_C(0x4c495645);
    o.compatibility = compatibilityFixture();
    o.journey = journey;
    o.localSeatCount = 1u;
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

static MdkrOnlineAdapterCommand cmd(IMdkrOnlineAdapter *a,
                                    MdkrOnlineViewAction action) {
    static uint64_t nextId = 1u;
    MdkrOnlineAdapterCommand c;
    c.expectedRevision = a->revision();
    c.requestId = nextId++;
    c.action = action;
    c.seat = 0u;
    c.value = 0u;
    return c;
}

static MdkrOnlineViewModel viewOf(IMdkrOnlineAdapter *a) {
    MdkrOnlineViewModel m{};
    a->view(&m);
    return m;
}

/* The regression: build a real LiveAdapter, drive it to a LOBBY where the local
 * endpoint is the room leader, wrap it as production does, and require the C
 * accessors to resolve THROUGH the wrapper (not fail closed on it). */
static void test_owning_wrapper_accessors_resolve_through_wrapper() {
    FakeMatchRoom room;
    FakeClock clock;
    FakeRoomTransport transport(&room);
    StubMeshBackend mesh;
    std::string err;
    auto inner = mdkr_online_live_adapter_create(
        baseOptions(&transport, &mesh, &clock, MDKR_ONLINE_JOURNEY_CREATE), &err);
    CHECK(inner != nullptr);
    if (inner == nullptr) {
        std::fprintf(stderr, "create failed: %s\n", err.c_str());
        return;
    }

    /* CREATE_ROOM, then service the synchronous double to a real LOBBY. */
    inner->submit(cmd(inner.get(), MDKR_ONLINE_VIEW_ACTION_CREATE_ROOM));
    for (int i = 0;
         i < 200 && viewOf(inner.get()).kind != MDKR_ONLINE_VIEW_ROOM; ++i) {
        inner->service();
    }
    CHECK(viewOf(inner.get()).kind == MDKR_ONLINE_VIEW_ROOM);

    /* Baseline on the RAW adapter (holds at HEAD and after the fix): the leader in
     * LOBBY has a lobby view and can set the session mode. */
    IMdkrOnlineAdapter *raw = inner.get();
    MdkrOnlineLobby rawLobby{};
    CHECK(mdkr_online_live_adapter_lobby(raw, &rawLobby));
    const bool rawSetMode =
        mdkr_online_live_adapter_set_mode(raw, MDKR_ONLINE_MODE_TOURNAMENT);
    CHECK(rawSetMode);

    /* Wrap it exactly as production does. `raw` stays valid -- the wrapper now
     * owns the same underlying LiveAdapter. */
    TestOwningWrapper wrapper(std::move(inner));
    IMdkrOnlineAdapter *w = &wrapper;

    /* RED at HEAD: the wrapper is a sibling of LiveAdapter, so the accessors'
     * cross-cast yields nullptr and both fail closed. They must instead resolve
     * through the wrapper and answer as the raw adapter does. */
    MdkrOnlineLobby wrapLobby{};
    CHECK(mdkr_online_live_adapter_lobby(w, &wrapLobby));
    const bool wrapSetMode =
        mdkr_online_live_adapter_set_mode(w, MDKR_ONLINE_MODE_TOURNAMENT);
    CHECK(wrapSetMode == rawSetMode);
    CHECK(wrapSetMode);
}

int main() {
    test_map_lost_reason_in_race_branches();
    test_race_end_no_demotion_rule();
    test_reverify_paths_clear_stale_peer_loss();
    test_retry_genuinely_retries();
    test_owning_wrapper_accessors_resolve_through_wrapper();
    std::fprintf(stderr, "online_live_adapter_beta: %d checks, %d failures\n",
                 g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
