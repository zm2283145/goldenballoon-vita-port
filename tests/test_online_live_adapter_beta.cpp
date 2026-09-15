/* Beta-ON unit test for the live adapter's race-end-card truthfulness decisions.
 *
 * No other test compiles platform/online/match_live_adapter.cpp WITH
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

/* N5: a rekey or a re-verify retires every peer key, so a route measurement in
 * flight is measuring sequences sealed under keys that no longer exist and a
 * SETTLED record describes a connection that is gone. Both entry points must
 * clear the whole route state -- the open window, the record, the publication
 * latch and the queue-drop baseline -- so the next connection measures itself
 * instead of inheriting the old one's numbers. */
static void test_reverify_paths_restart_route_measurement() {
    CHECK(mdkr_online_live_adapter_test_rekey_restarts_route_measurement(false));
    CHECK(mdkr_online_live_adapter_test_rekey_restarts_route_measurement(true));
    /* The other half of the rule (D-N5): the boundary BETWEEN two races of one
     * tournament retires the epoch, not the mesh, so a FULL window's record
     * survives and is exchanged again for the new round -- while a record
     * whose window a race start cut short is retired there, because the lanes
     * are idle between rounds and a whole window costs nobody a wait. */
    CHECK(mdkr_online_live_adapter_test_race_latch_reset_keeps_route(false));
    CHECK(mdkr_online_live_adapter_test_race_latch_reset_keeps_route(true));
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

/* Worker loss during preflight (audit matrix top-cell #2): a SignalLost while
 * the checking/phrase surfaces depend on the Worker must front the tailored
 * service card PROMPTLY through the existing failure plumbing -- never only
 * the generic 30 s timeout. Everywhere else SignalLost stays a pure status
 * (an established race rides the direct DataChannels; audit finding (f)). */
static void test_signal_lost_during_preflight_fronts_service_card() {
    CHECK(mdkr_online_live_adapter_test_signal_lost_card(true, false));
    CHECK(!mdkr_online_live_adapter_test_signal_lost_card(false, false));
    CHECK(!mdkr_online_live_adapter_test_signal_lost_card(true, true));
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

/* N8: the room owns membership, so a mid-race departure it reports finalises
 * the departed seat instead of waiting out a transport ladder. Every gate on
 * that decision, and the proposer rule that keeps two survivors from
 * finalising the same seat at different ticks.
 *
 * Bit 0 of the seam is "this departure finalises a seat now"; bit 1 is "this
 * endpoint proposes the tick" (the other survivors adopt what it sends). */
static void test_room_departure_gates_and_proposer() {
    enum { kFinalises = 1u, kProposes = 2u };
    /* 2P mid-race: the sole survivor finalises and proposes in one step --
     * there is nobody left to agree with and the room already confirmed the
     * leave. */
    CHECK(mdkr_online_live_adapter_test_room_departure(
              /*raceUp=*/true, /*enabled=*/true, /*known=*/true,
              /*thirdPeer=*/false) == (kFinalises | kProposes));
    /* A third survivor with a lower endpoint id owns the proposal; this one
     * still fronts the card, but adopts the tick it is sent. */
    CHECK(mdkr_online_live_adapter_test_room_departure(
              true, true, true, true) == kFinalises);
    /* No race is running: a departure is the lobby's business, and the
     * pre-race ladders are untouched by design. */
    CHECK(mdkr_online_live_adapter_test_room_departure(
              false, true, true, false) == 0u);
    /* The positive control the kill-drop lane arms: with the plumbing off the
     * departure decides nothing and the transport ladders resolve the loss at
     * their own pace. */
    CHECK(mdkr_online_live_adapter_test_room_departure(
              true, false, true, false) == 0u);
    /* An endpoint that owns no seat in this race cannot finalise one. */
    CHECK(mdkr_online_live_adapter_test_room_departure(
              true, true, false, false) == 0u);
}

/* N8 fix round 1 -- the critical one. A race_drop is a CLAIM by another
 * player, not a verdict. Adopting one unchecked let any peer name a third
 * party, finalise its seats and end this endpoint's race with no room
 * departure at all. Three independent things must hold before a proposal
 * means anything, and each is checked here on its own. */
static void test_drop_proposal_is_checked_before_it_counts() {
    /* The honest case: the lowest surviving id proposes, for this race, about
     * an endpoint the room has told US is gone. 400 is departing, so the
     * surviving roster is 100/200/300 and 100 owns the proposal. */
    CHECK(mdkr_online_live_adapter_test_drop_proposal_accepted(
        /*sender=*/100u, /*epoch=*/5u, /*room_verdict=*/true));
    /* THE ATTACK: a peer that is not the proposer tries to finalise a third
     * party. 300 is a survivor but not the lowest, so it has no standing. */
    CHECK(!mdkr_online_live_adapter_test_drop_proposal_accepted(
        300u, 5u, true));
    /* The departing endpoint proposing its own removal is equally unentitled. */
    CHECK(!mdkr_online_live_adapter_test_drop_proposal_accepted(
        400u, 5u, true));
    /* An endpoint outside the room cannot propose at all. */
    CHECK(!mdkr_online_live_adapter_test_drop_proposal_accepted(
        999u, 5u, true));
    /* Another race's proposal, arriving late, must not finalise a seat in
     * this one. */
    CHECK(!mdkr_online_live_adapter_test_drop_proposal_accepted(
        100u, 4u, true));
    CHECK(!mdkr_online_live_adapter_test_drop_proposal_accepted(
        100u, 6u, true));

    /* The intersection itself: a well-formed proposal from the right sender
     * is ACCEPTED as an agreed tick but must not finalise anything until this
     * endpoint has heard the room's own verdict -- otherwise the peer, not the
     * room, is deciding who is still racing. */
    CHECK(mdkr_online_live_adapter_test_drop_proposal_accepted(
        100u, 5u, /*room_verdict=*/false));
    CHECK(!mdkr_online_live_adapter_test_drop_proposal_applied(
        /*order=*/2u));
    /* Once both halves are present it applies, in either arrival order. */
    CHECK(mdkr_online_live_adapter_test_drop_proposal_applied(0u));
    CHECK(mdkr_online_live_adapter_test_drop_proposal_applied(1u));
}

/* D1: the peer-silence grace on the room's departure verdict. The room owns
 * membership, but a room-service wobble closes a member's socket exactly like
 * a quit does -- and only one of the two stops the peer racing. So the verdict
 * waits out a grace measured in AUTHORED ticks, and an authenticated packet
 * from the departed endpoint inside it drops the verdict for the race.
 *
 * Seam bits: 1 the grace is still open, 2 the verdict was held, 4 the seats
 * were finalised, 8 a finalisation tick was proposed, 16 the race ended. */
static void test_departure_grace_holds_a_live_peers_verdict() {
    enum {
        kOpen = 1u, kHeld = 2u, kFinalised = 4u, kProposed = 8u, kEnded = 16u
    };
    /* Silence, and the grace has not run out: nothing has happened yet -- and
     * in particular nothing has been PROPOSED. A proposal is a claim that a
     * peer stopped racing, and this endpoint has not finished listening. */
    CHECK(mdkr_online_live_adapter_test_departure_grace(
              /*grace_ticks=*/2u, /*ticks_elapsed=*/0u,
              /*peer_spoke=*/false) == kOpen);
    CHECK(mdkr_online_live_adapter_test_departure_grace(2u, 1u, false) ==
          kOpen);
    /* Silence for the whole grace: the room's verdict stands, and the sole
     * survivor proposes the tick, finalises the seat and ends its race. */
    CHECK(mdkr_online_live_adapter_test_departure_grace(2u, 2u, false) ==
          (kFinalised | kProposed | kEnded));
    /* THE DEFECT D1 REMOVES: the peer is still sending. The verdict is held,
     * no seat is finalised, no tick is proposed and the race carries on for
     * the transport's ladders to end. */
    CHECK(mdkr_online_live_adapter_test_departure_grace(2u, 2u, true) == kHeld);
    /* A hold does not wait out the grace: the first authenticated packet
     * settles it, whatever the authored head has reached. */
    CHECK(mdkr_online_live_adapter_test_departure_grace(2u, 0u, true) == kHeld);
    /* The lane's positive control (MDKR_ONLINE_LOBBY_DROP_GRACE=0) is the
     * pre-D1 code: it acts on the verdict in the pump it arrives in, and it
     * drops a peer that is still sending. */
    CHECK(mdkr_online_live_adapter_test_departure_grace(0u, 0u, false) ==
          (kFinalised | kProposed | kEnded));
    CHECK(mdkr_online_live_adapter_test_departure_grace(0u, 0u, true) ==
          (kFinalised | kProposed | kEnded));
}

/* N7-shaped bound on the route-probe echo. Every decoded probe used to be
 * echoed unconditionally, and in a 3-4P room one broadcast probe yields N-1
 * sealed echoes -- so a peer replaying probes at pump rate could make every
 * survivor seal for it. The budget is per SENDER and per PUMP: it must bound a
 * flood without touching the honest rate the measurement actually emits. */
static void test_route_echo_budget_bounds_a_flood() {
    /* Honest: one probe per pump from one peer, over a whole measurement's
     * worth of pumps, is echoed in full. */
    CHECK(mdkr_online_live_adapter_test_route_echoes_allowed(1u, 1u, 200u) ==
          200u);
    /* Still honest: a burst inside the budget echoes in full. */
    CHECK(mdkr_online_live_adapter_test_route_echoes_allowed(1u, 16u, 3u) ==
          48u);
    /* THE FLOOD: 1,000 probes inside one pump cost 16 echoes, not 1,000. */
    CHECK(mdkr_online_live_adapter_test_route_echoes_allowed(1u, 1000u, 1u) ==
          16u);
    /* And it does not refill inside the pump: the same flood over ten pumps
     * costs ten budgets, not ten thousand echoes. */
    CHECK(mdkr_online_live_adapter_test_route_echoes_allowed(1u, 1000u, 10u) ==
          160u);
    /* Per sender, so one flooding peer cannot starve an honest one: three
     * peers flooding cost three budgets per pump, and an honest peer among
     * them still gets its own. */
    CHECK(mdkr_online_live_adapter_test_route_echoes_allowed(3u, 1000u, 1u) ==
          48u);
}

/* The forensics ring is 2048 fixed-width slots and it is the only record of
 * what a lost race did. A peer sending a wrong-epoch race_drop every pump used
 * to write one record per message, evicting the whole ring in about ten
 * seconds. The refusal is a property of the sender's view, so it is recorded
 * once per (sender, reason) per race however long the flood runs. */
static void test_drop_refusal_flood_leaves_a_bounded_mark() {
    /* Three distinct (sender, reason) pairs: 100/epoch, 100/unknown,
     * 300/not-proposer. One round writes all three. */
    CHECK(mdkr_online_live_adapter_test_drop_refusal_records(1u) == 3u);
    /* 700 rounds is 2,100 refusals -- past the ring's 2048 slots, so if every
     * refusal were recorded the ring would now hold nothing else. It holds
     * exactly the same three. */
    CHECK(mdkr_online_live_adapter_test_drop_refusal_records(700u) == 3u);
    /* Not vacuous: the refusals themselves are still happening. Zero rounds
     * leave nothing, so the three above came from the flood, not from the
     * staging. */
    CHECK(mdkr_online_live_adapter_test_drop_refusal_records(0u) == 0u);
}

/* ---- S1: the preflight graph at three and four endpoints ---------------- *
 *
 * mdkr_match_preflight_evaluate compares every peer's attested graph digest
 * against the local one and reports a disagreement BEFORE it ever reaches the
 * admissibility check, so a room whose endpoints hash different graphs can
 * never reach READY however healthy its mesh is. The adapter used to set only
 * local<->peer edges: at two endpoints that star IS the complete graph and
 * everyone agrees, but at three each endpoint hashed a different star centred
 * on itself and the room stalled unconditionally. The consensus reducer itself
 * already reaches READY at three endpoints (tests/test_match_preflight.c), so
 * the graph the adapter built was the whole blocker. */

static const unsigned kEveryPeer = 0xfu; /* the local index's bit is ignored */

static bool sameDigest(const uint8_t *left, const uint8_t *right) {
    return std::memcmp(left, right, MDKR_MATCH_PREFLIGHT_DIGEST_BYTES) == 0;
}

static bool zeroDigest(const uint8_t *digest) {
    for (unsigned index = 0u; index < MDKR_MATCH_PREFLIGHT_DIGEST_BYTES;
         ++index) {
        if (digest[index] != 0u) return false;
    }
    return true;
}

/* Every endpoint in the room attests the same graph, at two, three and four --
 * and the star it replaced is shown disagreeing at three and four, so this is
 * not a fixture that would pass either way. */
static void test_preflight_graph_agrees_at_three_and_four() {
    uint8_t complete[MDKR_MATCH_PEER_GRAPH_MAX_ENDPOINTS]
                    [MDKR_MATCH_PREFLIGHT_DIGEST_BYTES];
    uint8_t star[MDKR_MATCH_PEER_GRAPH_MAX_ENDPOINTS]
                [MDKR_MATCH_PREFLIGHT_DIGEST_BYTES];
    uint8_t byCount[MDKR_MATCH_PEER_GRAPH_MAX_ENDPOINTS + 1u]
                   [MDKR_MATCH_PREFLIGHT_DIGEST_BYTES];
    std::memset(byCount, 0, sizeof(byCount));

    for (unsigned count = 2u; count <= MDKR_MATCH_PEER_GRAPH_MAX_ENDPOINTS;
         ++count) {
        for (unsigned local = 0u; local < count; ++local) {
            CHECK(mdkr_online_live_adapter_test_preflight_graph_digest(
                count, local, kEveryPeer, kEveryPeer, false, complete[local]));
            CHECK(mdkr_online_live_adapter_test_preflight_star_digest(
                count, local, star[local]));
            CHECK(!zeroDigest(complete[local]));
        }
        /* THE FIX. Whichever endpoint you stand on, the attested graph is the
         * same one, so the digests can agree at all. */
        for (unsigned local = 1u; local < count; ++local) {
            CHECK(sameDigest(complete[0], complete[local]));
        }
        /* Roster order is not load-bearing: the digest sorts endpoints by id
         * and remaps the reachability bits into that order before hashing, so
         * a roster built the other way round hashes the same. */
        for (unsigned local = 0u; local < count; ++local) {
            uint8_t reversed[MDKR_MATCH_PREFLIGHT_DIGEST_BYTES];
            CHECK(mdkr_online_live_adapter_test_preflight_graph_digest(
                count, local, kEveryPeer, kEveryPeer, true, reversed));
            CHECK(sameDigest(complete[0], reversed));
        }
        if (count == 2u) {
            /* Two endpoints are UNCHANGED. The old star was already the
             * complete graph there, so both endpoints attest the identical
             * digest they attested before -- this change moves nothing that a
             * two-player room can observe. */
            CHECK(sameDigest(complete[0], star[0]));
            CHECK(sameDigest(complete[1], star[1]));
        } else {
            /* THE BUG, reproduced. At three and four the star gave every
             * endpoint a different digest, and none of them is the graph now
             * attested. */
            for (unsigned local = 0u; local < count; ++local) {
                CHECK(!sameDigest(complete[0], star[local]));
                for (unsigned other = local + 1u; other < count; ++other) {
                    CHECK(!sameDigest(star[local], star[other]));
                }
            }
        }
        std::memcpy(byCount[count], complete[0],
                    MDKR_MATCH_PREFLIGHT_DIGEST_BYTES);
    }
    /* Not vacuous: a room of a different size is a different graph. */
    CHECK(!sameDigest(byCount[2], byCount[3]));
    CHECK(!sameDigest(byCount[3], byCount[4]));
    CHECK(!sameDigest(byCount[2], byCount[4]));
}

/* The assertion's other half, and the only reason it is sound: an endpoint
 * that cannot speak for the whole roster declines to build a graph at all. It
 * then never attests, and preflight leaves the room at WAITING_FOR_PEERS
 * rather than agreeing a topology nobody can route -- the refusal direction. */
static void test_preflight_graph_refuses_what_it_cannot_speak_for() {
    uint8_t digest[MDKR_MATCH_PREFLIGHT_DIGEST_BYTES];
    /* Control: the whole star seen, every generation known, three endpoints
     * build. Local is index 0, so its peers are bits 1 and 2. */
    CHECK(mdkr_online_live_adapter_test_preflight_graph_digest(
        3u, 0u, 0x6u, 0x6u, false, digest));
    CHECK(!zeroDigest(digest));

    /* One peer's channels are not open here, so this endpoint has no evidence
     * for the pair and refuses. Both endpoints of a broken pair refuse, so a
     * genuinely incomplete mesh stalls the room instead of reaching READY. */
    CHECK(!mdkr_online_live_adapter_test_preflight_graph_digest(
        3u, 0u, 0x2u, 0x6u, false, digest));
    CHECK(zeroDigest(digest));
    CHECK(!mdkr_online_live_adapter_test_preflight_graph_digest(
        4u, 1u, 0x5u, 0xdu, false, digest));
    CHECK(zeroDigest(digest));

    /* One peer's service-assigned generation is unknown. Standing the local
     * generation in for it -- what the adapter used to do -- hashes a graph
     * that peer cannot reproduce, and the graph is built once and latched, so
     * that is a PERMANENT disagreement rather than something a later service()
     * repairs. Refuse and wait instead. */
    CHECK(!mdkr_online_live_adapter_test_preflight_graph_digest(
        3u, 0u, 0x6u, 0x4u, false, digest));
    CHECK(zeroDigest(digest));
    CHECK(!mdkr_online_live_adapter_test_preflight_graph_digest(
        4u, 3u, 0x7u, 0x3u, false, digest));
    CHECK(zeroDigest(digest));
    /* Including at two endpoints, where the substitute was equally wrong and
     * equally permanent. */
    CHECK(!mdkr_online_live_adapter_test_preflight_graph_digest(
        2u, 0u, 0x2u, 0x0u, false, digest));
    CHECK(zeroDigest(digest));

    /* A room this seam cannot describe is refused rather than half-built. */
    CHECK(!mdkr_online_live_adapter_test_preflight_graph_digest(
        1u, 0u, kEveryPeer, kEveryPeer, false, digest));
    CHECK(zeroDigest(digest));
}

int main() {
    test_map_lost_reason_in_race_branches();
    test_race_end_no_demotion_rule();
    test_reverify_paths_clear_stale_peer_loss();
    test_reverify_paths_restart_route_measurement();
    test_retry_genuinely_retries();
    test_signal_lost_during_preflight_fronts_service_card();
    test_owning_wrapper_accessors_resolve_through_wrapper();
    test_room_departure_gates_and_proposer();
    test_drop_proposal_is_checked_before_it_counts();
    test_departure_grace_holds_a_live_peers_verdict();
    test_route_echo_budget_bounds_a_flood();
    test_drop_refusal_flood_leaves_a_bounded_mark();
    test_preflight_graph_agrees_at_three_and_four();
    test_preflight_graph_refuses_what_it_cannot_speak_for();
    std::fprintf(stderr, "online_live_adapter_beta: %d checks, %d failures\n",
                 g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
