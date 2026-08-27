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
#include "online/online_track_table.h"
#include "net/net_roster_runtime.h"
#include "net/party_link.h"

#include "app_version.h"
#include "online/compatibility_identity.h"

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

/* Field-wise (never memcmp-with-padding) equality, mirroring the lobby
 * reducer's own JOIN comparator (lobby_core.c compatible()). */
bool sameCompatibility(const MdkrOnlineCompatibilityV1 &left,
                       const MdkrOnlineCompatibilityV1 &right) {
    return left.protocol_version == right.protocol_version &&
           left.rom_revision == right.rom_revision &&
           left.cadence_hz == right.cadence_hz &&
           std::memcmp(left.build_id, right.build_id,
                       sizeof(left.build_id)) == 0 &&
           std::memcmp(left.gameplay_digest, right.gameplay_digest,
                       sizeof(left.gameplay_digest)) == 0;
}

}  // namespace

/* Gameplay-determinism developer seams (platform/math_util_native.c): each of
 * these environment variables changes gameplay math ON THIS MACHINE ONLY (RNG
 * boot seeds, arctan table rounding, sine evaluation). None of them is part of
 * the compatibility identity -- provenance hashes version+commit, not runtime
 * env -- so a one-sided setting passes the JOIN byte-compare and then GUARANTEES
 * a silent mid-race desync. Live online therefore refuses to construct while
 * any is set; offline/dev use of the seams stays untouched. Returns the first
 * offending variable name, or nullptr when none is set. */
const char *OnlineRoom_liveBlockedByDeterminismEnv(void) {
    static const char *const kSeams[] = {"MDKR_RNGSEED", "MDKR_ARCTAN",
                                         "MDKR_TRIG"};
    for (const char *seam : kSeams) {
        if (std::getenv(seam) != nullptr) return seam;
    }
    return nullptr;
}

/* The LIVE compatibility identity for THIS binary: the real provenance
 * generator (platform/online/compatibility_identity.c, SHA-256 over
 * version+commit+ROM revision) over the same compiled-in values the About
 * panel and `mdkr64 --version` print -- MDKR_VERSION via AppVersion() and the
 * release CI's MDKR_BUILD_STAMP commit via AppBuildStamp(). Two copies of the
 * same DMG with the same accepted ROM derive identical bytes; any other
 * version, commit or ROM revision derives different bytes, which is exactly
 * the property the lobby's JOIN byte-compare needs.
 *
 * source_dirty is false because MDKR_BUILD_STAMP is only ever threaded by
 * release CI from the clean commit tools/release/stamp_provenance.sh binds the
 * artifact to. A plain dev build has an EMPTY stamp, which fails the strict
 * 40-hex commit grammar and this returns false: an unstamped build has no
 * provable gameplay digest, so live online correctly stays unavailable in it
 * (configure -DMDKR_BUILD_STAMP=<commit> on a clean tree to test locally). */
bool OnlineRoom_liveCompatibilityFromProvenance(
    uint8_t romRevision, MdkrOnlineCompatibilityV1 *out) {
    return mdkr_online_compatibility_from_provenance(
        AppVersion(), AppBuildStamp(), /*source_dirty=*/false, romRevision,
        out);
}

std::unique_ptr<IMdkrOnlineAdapter> OnlineRoom_makeGatedLiveAdapter(
    const MdkrOnlineCompatibilityV1 &compatibility, MdkrOnlineJourney journey,
    const std::string &joinCode) {
    std::string error;

    /* m2 fence: never let a machine with one-sided gameplay-determinism env
     * seams into a real session (see OnlineRoom_liveBlockedByDeterminismEnv).
     * Enforced here so EVERY live-adapter construction -- panel and the cloud
     * test driver alike -- fails closed, not just the interactive chooser. */
    if (const char *seam = OnlineRoom_liveBlockedByDeterminismEnv()) {
        std::fprintf(stderr,
                     "[online-live] refused: gameplay-determinism env %s is "
                     "set; a one-sided setting guarantees an online desync\n",
                     seam);
        return nullptr;
    }

    /* C1 fence: the live adapter only ever carries the REAL provenance
     * compatibility. Recompute what this binary derives for the caller's ROM
     * revision and refuse anything else, so no caller (present or future) can
     * build a live adapter from a canned fixture that every build shares --
     * that fixture let mismatched builds/ROMs pass the JOIN byte-compare
     * (lobby_core.c) and desync mid-race undetected. */
    MdkrOnlineCompatibilityV1 expected;
    if (!OnlineRoom_liveCompatibilityFromProvenance(compatibility.rom_revision,
                                                    &expected)) {
        std::fprintf(stderr,
                     "[online-live] refused: no release provenance for rom "
                     "revision %u (dev build without MDKR_BUILD_STAMP?)\n",
                     static_cast<unsigned>(compatibility.rom_revision));
        return nullptr;
    }
    if (!sameCompatibility(compatibility, expected)) {
        std::fprintf(stderr,
                     "[online-live] refused: compatibility is not this "
                     "build's provenance identity (canned fixture?)\n");
        return nullptr;
    }

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
    /* Local ROM verification result (never sourced from the service). True is
     * honest here by caller contract, enforced above: `compatibility` must
     * byte-match this build's provenance identity for a SUPPORTED rom_revision,
     * and the only interactive caller (buildBetaLiveAdapter in
     * ui_online_room.cpp) derives that revision from the launcher's completed
     * full-image ROM validation (RomInfo.valid && integrity_verified -- the
     * same rom_validation.c contract the engine re-checks at boot) and refuses
     * to call this factory otherwise. The env-gated cloud test driver below
     * documents its own ROM contract at its call site. */
    options.romVerified = true;
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
 * P2-T1: live selection bridge wiring (platform/net/party_link)
 *
 * FORWARD FEED: OnlineRoom_pumpPartyLink projects the adapter's live lobby +
 * view model into a pinned snapshot and publishes it for the native screens.
 * REVERSE FEED: OnlineRoom_pumpPartyLinkIntent one-shot-polls the local
 * player's in-menu intent and dispatches the SAME existing view actions the
 * Online Room panel (ui_online_room.cpp) does. Later tasks pump both from the
 * engine-loop service callback during MENUS; install/clear bookend the session.
 * ======================================================================== */
namespace {

/* Mirror ui_online_room.cpp's dispatch(): expectedRevision from the live
 * revision, a monotonic requestId, seat 0 (fenced solo local seat). */
MdkrOnlineAdapterStep partyLinkSubmit(IMdkrOnlineAdapter *adapter,
                                      MdkrOnlineViewAction action,
                                      unsigned value) {
    static uint64_t nextId = 1u;
    MdkrOnlineAdapterCommand c;
    c.expectedRevision = adapter->revision();
    c.requestId = nextId++;
    c.action = action;
    c.seat = 0u;
    c.value = value;
    return adapter->submit(c);
}

/* START_RACE's value must be the resolved race track's RAW usable-vehicle mask
 * -- exactly ui_online_room.cpp handleAction()'s START_RACE computation: the
 * cup schedule's next round in a tournament, else the host's configured track;
 * all-base (0x07) when no snapshot resolves a track. */
unsigned partyLinkStartVehicleMask(IMdkrOnlineAdapter *adapter) {
    unsigned mask = MDKR_ONLINE_PLAYER_VEHICLE_MASK;
    MdkrOnlineLobby lobby{};
    if (mdkr_online_live_adapter_lobby(adapter, &lobby)) {
        const uint16_t resolved =
            lobby.mode == MDKR_ONLINE_MODE_TOURNAMENT
                ? (lobby.cup_id != MDKR_ONLINE_NO_CUP
                       ? mdkr_online_cup_track_id(lobby.cup_id, lobby.race_index)
                       : static_cast<uint16_t>(MDKR_ONLINE_NO_VOTE))
                : lobby.configured_track;
        const MdkrOnlineTrackInfo *track =
            resolved != MDKR_ONLINE_NO_VOTE ? mdkr_online_track_by_id(resolved)
                                            : nullptr;
        if (track != nullptr) mask = track->vehicle_mask;
    }
    return mask;
}

/* Reverse-feed dedupe state (pure dedupe/ordering logic lives in party_link.c so
 * it is unit-testable). One instance per session; reset on install/clear. */
MdkrPartyLinkDispatchState sPartyLinkDispatch;

/* Deterministic, adapter-free scripted snapshot for step `step` of the
 * MDKR_APP_TEST_PARTY_LINK_FAKE sequence: a 2-seat tournament room (host seat 0
 * = local + crown, joiner seat 1), advancing selections then Ready. With no
 * fake env set this is never called. */
void partyLinkFakeSnapshot(unsigned step, MdkrPartyLinkSnapshot *out) {
    std::memset(out, 0, sizeof(*out));
    out->phase = static_cast<uint8_t>(MDKR_ONLINE_LOBBY);
    out->mode = MDKR_ONLINE_MODE_TOURNAMENT;
    out->configured_track = 0xFFFFu; /* tournament: cup schedule owns the track */
    out->cup_id = 0u;                /* Dino Domain cup */
    out->race_index = 0u;
    for (unsigned i = 0u; i < MDKR_PARTY_LINK_SEATS; ++i) {
        out->seats[i].character_id = MDKR_ONLINE_NO_CHARACTER;
        out->seats[i].vehicle_id = MDKR_ONLINE_NO_VEHICLE;
    }
    out->seats[0].occupied = 1u;
    out->seats[0].is_local = 1u;
    out->seats[0].is_host = 1u;
    out->seats[0].connected = 1u;
    out->seats[1].occupied = 1u;
    out->seats[1].connected = 1u;
    if (step >= 1u) {
        out->seats[0].character_id = 1u;
        out->seats[0].vehicle_id = 0u;
        out->seats[1].character_id = 2u;
        out->seats[1].vehicle_id = 0u;
    }
    if (step >= 2u) {
        out->seats[0].ready = 1u;
        out->seats[1].ready = 1u;
    }
    /* generation left 0: mdkr_party_link_publish assigns a monotonic value. */
}

}  // namespace

void OnlineRoom_installPartyLink(void) {
    /* Clear-then-install (F3): install() refuses when already active, which would
     * leave a stale prior snapshot live -- clearing first guarantees a fresh
     * session every time. */
    mdkr_party_link_clear();
    (void)mdkr_party_link_install();
    mdkr_party_link_dispatch_state_reset(&sPartyLinkDispatch);
}

void OnlineRoom_clearPartyLink(void) {
    mdkr_party_link_clear();
    mdkr_party_link_dispatch_state_reset(&sPartyLinkDispatch);
}

void OnlineRoom_pumpPartyLink(IMdkrOnlineAdapter *adapter) {
    if (adapter == nullptr || !mdkr_party_link_active()) return;
    MdkrOnlineLobby lobby{};
    /* No authoritative snapshot yet (fake adapter, or before the first room
     * state): leave the last published snapshot in place. */
    if (!mdkr_online_live_adapter_lobby(adapter, &lobby)) return;
    MdkrOnlineViewModel vm{};
    const bool haveView = adapter->view(&vm);
    MdkrPartyLinkSnapshot snap;
    /* local_endpoint_id 0: the mapper falls back to view->local_member_is_leader
     * for the fenced 2-endpoint beta (no adapter accessor exposes the raw id). */
    mdkr_party_link_snapshot_from_lobby(&snap, haveView ? &vm : nullptr, &lobby,
                                        0u);
    mdkr_party_link_publish(&snap);
}

/* The local seat's current vehicle in the live lobby, so a CHOOSE_VEHICLE that
 * merely re-states the lobby's existing pick is skipped. NO_VEHICLE when there
 * is no snapshot or no resolvable local seat. */
unsigned partyLinkLocalVehicle(IMdkrOnlineAdapter *adapter) {
    MdkrOnlineLobby lobby{};
    if (!mdkr_online_live_adapter_lobby(adapter, &lobby)) {
        return MDKR_ONLINE_NO_VEHICLE;
    }
    MdkrOnlineViewModel vm{};
    const bool haveView = adapter->view(&vm);
    MdkrPartyLinkSnapshot snap;
    mdkr_party_link_snapshot_from_lobby(&snap, haveView ? &vm : nullptr, &lobby,
                                        0u);
    for (unsigned i = 0u; i < MDKR_PARTY_LINK_SEATS; ++i) {
        if (snap.seats[i].occupied && snap.seats[i].is_local) {
            return snap.seats[i].vehicle_id;
        }
    }
    return MDKR_ONLINE_NO_VEHICLE;
}

void OnlineRoom_pumpPartyLinkIntent(IMdkrOnlineAdapter *adapter) {
    if (adapter == nullptr) return;
    MdkrPartyLinkLocalIntent intent;
    if (!mdkr_party_link_intent_poll(&intent)) return; /* one-shot per publish */

    MdkrPartyLinkDispatchPlan plan;
    mdkr_party_link_plan_dispatch(&sPartyLinkDispatch, &intent, &plan);
    if (plan.count == 0u) return;

    const unsigned currentVehicle = partyLinkLocalVehicle(adapter);

    /* Dispatch the planned actions IN ORDER, latching each ONLY on an accepted
     * step (F1): a refusal (SELECTION_CONFLICT / ILLEGAL_VEHICLE / stale-revision
     * / in-flight) re-fires on the next intent instead of being swallowed. */
    for (unsigned i = 0u; i < plan.count; ++i) {
        const MdkrPartyLinkDispatchAction &action = plan.actions[i];
        switch (action.kind) {
        case MDKR_PARTY_LINK_DISPATCH_CHOOSE_CHARACTER:
            if (partyLinkSubmit(adapter,
                                MDKR_ONLINE_VIEW_ACTION_CHOOSE_CHARACTER,
                                action.value).accepted) {
                mdkr_party_link_dispatch_latch(&sPartyLinkDispatch, &action);
            }
            break;
        case MDKR_PARTY_LINK_DISPATCH_CHOOSE_VEHICLE:
            /* Mask-legal value handling: only forward a real vehicle id; the
             * reducer's own mask gate (ILLEGAL_VEHICLE) rejects a track-illegal
             * pick, which -- unlatched -- lets the native screen (the source of
             * truth) re-pick, rather than the launcher silently defaulting. */
            if (action.value >= MDKR_ONLINE_PLAYER_VEHICLE_COUNT) break;
            if (action.value == currentVehicle) {
                /* The lobby already holds this vehicle: nothing to send, but
                 * latch so it is not re-planned every intent. */
                mdkr_party_link_dispatch_latch(&sPartyLinkDispatch, &action);
                break;
            }
            if (partyLinkSubmit(adapter, MDKR_ONLINE_VIEW_ACTION_CHOOSE_VEHICLE,
                                action.value).accepted) {
                mdkr_party_link_dispatch_latch(&sPartyLinkDispatch, &action);
            }
            break;
        case MDKR_PARTY_LINK_DISPATCH_CHANGE_SELECTION:
            if (partyLinkSubmit(adapter,
                                MDKR_ONLINE_VIEW_ACTION_CHANGE_SELECTION, 0u)
                    .accepted) {
                mdkr_party_link_dispatch_latch(&sPartyLinkDispatch, &action);
            }
            break;
        case MDKR_PARTY_LINK_DISPATCH_READY:
            if (partyLinkSubmit(adapter, MDKR_ONLINE_VIEW_ACTION_READY, 1u)
                    .accepted) {
                mdkr_party_link_dispatch_latch(&sPartyLinkDispatch, &action);
            }
            break;
        case MDKR_PARTY_LINK_DISPATCH_START_RACE:
            /* START_RACE value is the resolved track's RAW usable-vehicle mask
             * (partyLinkStartVehicleMask), not a party_link value. */
            if (partyLinkSubmit(adapter, MDKR_ONLINE_VIEW_ACTION_START_RACE,
                                partyLinkStartVehicleMask(adapter)).accepted) {
                mdkr_party_link_dispatch_latch(&sPartyLinkDispatch, &action);
            }
            break;
        default:
            break;
        }
    }
}

void OnlineRoom_runTestPartyLinkFake(void) {
    /* Adapter-free scripted proof of the forward feed for the P2 native-menu
     * tests to read. Reinstall fresh, then publish the deterministic sequence;
     * the last snapshot stays live for a reader. */
    OnlineRoom_clearPartyLink();
    OnlineRoom_installPartyLink();
    const unsigned steps = 3u;
    for (unsigned step = 0u; step < steps; ++step) {
        MdkrPartyLinkSnapshot snap;
        partyLinkFakeSnapshot(step, &snap);
        mdkr_party_link_publish(&snap);
        MdkrPartyLinkSnapshot readBack{};
        (void)mdkr_party_link_read(&readBack);
        std::fprintf(stderr,
                     "[party-link-fake] step=%u gen=%u phase=%u mode=%u cup=%u "
                     "chars=%u,%u ready=%u,%u host=%u,%u local=%u,%u\n",
                     step, readBack.generation, readBack.phase, readBack.mode,
                     readBack.cup_id, readBack.seats[0].character_id,
                     readBack.seats[1].character_id, readBack.seats[0].ready,
                     readBack.seats[1].ready, readBack.seats[0].is_host,
                     readBack.seats[1].is_host, readBack.seats[0].is_local,
                     readBack.seats[1].is_local);
    }
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

/* Fixture for the IN-PROCESS loopback race ONLY (both endpoints share this
 * one process, so identical bytes are trivially guaranteed and no provenance
 * stamp is needed to run it). The production factory above and the cloud test
 * driver below both use OnlineRoom_liveCompatibilityFromProvenance instead;
 * OnlineRoom_makeGatedLiveAdapter refuses this fixture outright. */
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
        ev.commandId = command.command_id; /* echo for the adapter's correlation */
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

/* ==== Test-only loopback session-config env seams ======================== *
 *
 * The loopback race historically drove ONE fixed configuration: vote track 5
 * (Ancient Lake), vehicle Car, START_RACE mask 0x7. The gates for the
 * all-tracks + tournament work need the same harness to drive DIFFERENT
 * lobby configurations, so these seams read:
 *
 *   MDKR_APP_TEST_ONLINE_TRACK=<id>       leader fixes the track with
 *                                         SET_CONFIG_TRACK (a configured
 *                                         session skips the vote step in the
 *                                         view, exactly like retail; the
 *                                         config, never a vote, drives the
 *                                         manifest)
 *   MDKR_APP_TEST_ONLINE_MODE=tournament  leader sends SET_MODE(1) and
 *   MDKR_APP_TEST_ONLINE_CUP=<id>         SET_CUP (default cup 0); the cup
 *                                         schedule, not the votes, picks
 *                                         every round's track
 *
 * With NO seam env set every value below reproduces the historical literals
 * exactly (vehicle Car == 0, START_RACE mask 0x7, no leader config commands),
 * so the registered default lanes stay byte-identical. A malformed seam fails
 * the whole loopback setup loudly rather than racing a wrong configuration.
 *
 * Vehicle + mask derivation: BEGIN_LOADING refuses any seat whose chosen
 * vehicle bit is outside the START_RACE mask, the launch-descriptor validator
 * re-checks the same bit against the frozen manifest mask, and the engine's
 * admission requires manifest mask == leveltable_vehicle_usable(track). So a
 * configured track uses ITS raw table mask and ITS ROM default vehicle; a cup
 * uses round 1's table mask and one vehicle that stays legal across EVERY
 * round (seats keep their vehicle across REMATCH -- clear_round only resets
 * votes and Ready). */
struct LoopbackSessionConfig {
    bool valid = true;
    const char *error = nullptr;
    bool tournament = false; /* MDKR_APP_TEST_ONLINE_MODE=tournament */
    unsigned cup = 0u;       /* MDKR_APP_TEST_ONLINE_CUP, tournament only */
    bool haveTrack = false;  /* MDKR_APP_TEST_ONLINE_TRACK set */
    unsigned track = 0u;
    unsigned vehicle = 0u;   /* both endpoints' CHOOSE_VEHICLE value */
    unsigned startMask = 7u; /* the leader's START_RACE vehicle mask */
    bool any() const { return tournament || haveTrack; }
};

LoopbackSessionConfig loopbackSessionConfig() {
    LoopbackSessionConfig cfg;
    auto refuse = [&cfg](const char *message) {
        cfg.valid = false;
        cfg.error = message;
        return cfg;
    };
    auto parseUnsigned = [](const char *text, unsigned *out) {
        char *end = nullptr;
        const unsigned long value = std::strtoul(text, &end, 10);
        if (end == text || *end != '\0' || value > 0xffffu) return false;
        *out = static_cast<unsigned>(value);
        return true;
    };
    if (const char *mode = std::getenv("MDKR_APP_TEST_ONLINE_MODE")) {
        if (std::strcmp(mode, "tournament") != 0) {
            return refuse("MDKR_APP_TEST_ONLINE_MODE must be 'tournament'");
        }
        cfg.tournament = true;
        if (const char *cup = std::getenv("MDKR_APP_TEST_ONLINE_CUP")) {
            if (!parseUnsigned(cup, &cfg.cup) ||
                cfg.cup >= MDKR_ONLINE_CUP_COUNT) {
                return refuse("invalid MDKR_APP_TEST_ONLINE_CUP");
            }
        }
    }
    if (const char *track = std::getenv("MDKR_APP_TEST_ONLINE_TRACK")) {
        if (cfg.tournament) {
            return refuse("MDKR_APP_TEST_ONLINE_TRACK cannot combine with "
                          "tournament mode (the cup schedules every track)");
        }
        if (!parseUnsigned(track, &cfg.track) ||
            mdkr_online_track_by_id(static_cast<uint16_t>(cfg.track)) ==
                nullptr) {
            return refuse("invalid MDKR_APP_TEST_ONLINE_TRACK (not one of "
                          "the 20 standard race tracks)");
        }
        cfg.haveTrack = true;
    }
    if (cfg.tournament) {
        uint8_t sharedMask = MDKR_ONLINE_VEHICLE_BIT_ALL;
        const MdkrOnlineTrackInfo *round1 = nullptr;
        for (unsigned round = 0u; round < MDKR_ONLINE_CUP_ROUNDS; ++round) {
            const MdkrOnlineTrackInfo *info = mdkr_online_track_by_id(
                mdkr_online_cup_track(cfg.cup, round));
            if (info == nullptr) {
                return refuse("cup round missing from the track table");
            }
            sharedMask = static_cast<uint8_t>(sharedMask & info->vehicle_mask);
            if (round == 0u) round1 = info;
        }
        if (sharedMask == 0u) {
            return refuse("cup rounds share no legal vehicle");
        }
        cfg.startMask = round1->vehicle_mask;
        if ((sharedMask & (1u << round1->default_vehicle)) != 0u) {
            cfg.vehicle = round1->default_vehicle;
        } else {
            unsigned bit = 0u;
            while ((sharedMask & (1u << bit)) == 0u) ++bit;
            cfg.vehicle = bit;
        }
    } else if (cfg.haveTrack) {
        const MdkrOnlineTrackInfo *info =
            mdkr_online_track_by_id(static_cast<uint16_t>(cfg.track));
        cfg.startMask = info->vehicle_mask;
        cfg.vehicle = info->default_vehicle;
    }
    return cfg;
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
    /* Test-only (MDKR_APP_TEST_ONLINE_LIVE_JOINER): when set, endpoint B (the
     * join journey, canonical slot 1) wins the once-only process-global roster
     * install and becomes the visible engine, so the visible endpoint renders
     * canonical slot 1 (the joiner) full-screen -- the exact topology the
     * production join process presents. Default false keeps A (host) visible. */
    bool joinerVisible = false;
};

MdkrOnlineTestLoopbackRace *OnlineRoom_makeTestLoopbackRace(std::string *error) {
    auto set_err = [&](const char *m) {
        if (error != nullptr) *error = m;
    };
    /* A fresh process-global roster: A will install it (endpoint order below
     * makes A the owner, so the visible engine renders A's viewport). */
    mdkr_net_roster_runtime_clear();

    auto race = std::make_unique<MdkrOnlineTestLoopbackRace>();
    race->joinerVisible =
        std::getenv("MDKR_APP_TEST_ONLINE_LIVE_JOINER") != nullptr;
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
    /* Test-only session-config seams (loopbackSessionConfig above). With no
     * seam env set this whole block is inert and the flow below is identical
     * to the historical fixed vote-track-5 / Car / mask-0x7 race. */
    const LoopbackSessionConfig sessionCfg = loopbackSessionConfig();
    if (!sessionCfg.valid) {
        set_err(sessionCfg.error);
        return nullptr;
    }
    if (sessionCfg.any()) {
        if (sessionCfg.tournament) {
            if (!mdkr_online_live_adapter_set_mode(A,
                                                   MDKR_ONLINE_MODE_TOURNAMENT) ||
                !mdkr_online_live_adapter_set_cup(A, sessionCfg.cup)) {
                set_err("leader session-config submit refused (mode/cup)");
                return nullptr;
            }
        } else if (!mdkr_online_live_adapter_set_config_track(
                       A, sessionCfg.track)) {
            set_err("leader session-config submit refused (track)");
            return nullptr;
        }
        if (!loopbackPumpUntil(both, [&]() {
                MdkrOnlineLobby la{}, lb{};
                if (!mdkr_online_live_adapter_lobby(A, &la) ||
                    !mdkr_online_live_adapter_lobby(B, &lb)) return false;
                if (sessionCfg.tournament) {
                    return la.mode == MDKR_ONLINE_MODE_TOURNAMENT &&
                           la.cup_id == sessionCfg.cup &&
                           lb.mode == MDKR_ONLINE_MODE_TOURNAMENT &&
                           lb.cup_id == sessionCfg.cup;
                }
                return la.configured_track == sessionCfg.track &&
                       lb.configured_track == sessionCfg.track;
            }, 5000u)) {
            set_err("session config did not reach both lobby snapshots");
            return nullptr;
        }
        if (sessionCfg.tournament) {
            std::fprintf(stderr,
                         "[online-live] loopback config mode=tournament "
                         "cup=%u round1Track=%u startMask=0x%02x vehicle=%u\n",
                         sessionCfg.cup,
                         static_cast<unsigned>(
                             mdkr_online_cup_track(sessionCfg.cup, 0u)),
                         sessionCfg.startMask, sessionCfg.vehicle);
        } else {
            std::fprintf(stderr,
                         "[online-live] loopback config mode=single track=%u "
                         "startMask=0x%02x vehicle=%u\n",
                         sessionCfg.track, sessionCfg.startMask,
                         sessionCfg.vehicle);
        }
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
        /* Car (0) historically; a session-config seam swaps in a vehicle that
         * is legal for the configured track / every cup round (see
         * loopbackSessionConfig). */
        self->submit(loopbackCmd(self, MDKR_ONLINE_VIEW_ACTION_CHOOSE_VEHICLE,
                                 0u, sessionCfg.vehicle));
        if (!sessionCfg.any()) {
            /* Track 5 == Ancient Lake, matching tests/input_scripts/
             * race_2p_split. A configured session (tournament cup or fixed
             * track) never offers the vote step -- the view model skips it,
             * exactly like the retail flow -- so the vote exists only on the
             * legacy single-race default path. */
            until(MDKR_ONLINE_VIEW_ACTION_VOTE_TRACK);
            self->submit(
                loopbackCmd(self, MDKR_ONLINE_VIEW_ACTION_VOTE_TRACK, 0u, 5u));
        }
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
     * the ROM's usable-vehicle mask for the race track or the engine's online
     * race admission (mdkr_match_manifest_accepts_loaded_race) rejects the boot.
     * Default (no seam env): Ancient Lake (track 5), car/hovercraft/plane ==
     * 0x07; a session-config seam supplies the configured track's / cup round
     * 1's raw table mask instead. */
    A->submit(loopbackCmd(A, MDKR_ONLINE_VIEW_ACTION_START_RACE, 0u,
                          sessionCfg.startMask));
    /* When the joiner must be visible, service B first here so it reaches
     * race-ready and wins the once-only roster install ahead of A -- installing
     * B's local=slot1 roster and making the visible engine render canonical
     * slot 1. Otherwise A (host, slot0) wins as before. */
    const std::vector<IMdkrOnlineAdapter *> raceReadyOrder =
        race->joinerVisible ? std::vector<IMdkrOnlineAdapter *>{B, A} : both;
    if (!loopbackPumpUntil(raceReadyOrder, [&]() {
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
    if (race == nullptr) return nullptr;
    return (race->joinerVisible ? race->b : race->a).get();
}

IMdkrOnlineAdapter *OnlineRoom_testLoopbackPeer(
    MdkrOnlineTestLoopbackRace *race) {
    if (race == nullptr) return nullptr;
    return (race->joinerVisible ? race->a : race->b).get();
}

/* ======================================================================== *
 * Test-only tournament continuation (MDKR_APP_TEST_ONLINE_MODE=tournament)
 *
 * The MDKR_APP_TEST_ONLINE_LIVE branch in main_app.cpp boots the visible
 * engine EXACTLY once per process and then destroys the loopback pair, so a
 * full 4-race cup cannot re-boot the engine here. Instead, after the launcher
 * has reported the ENGINE race's real placements (reportOnlineRaceResults ->
 * PUBLISH_RESULTS), this continuation drives rounds 2..4 at the TRANSPORT
 * level through the SAME live room, exactly the way
 * tests/test_online_live_adapter.cpp's lifecycle/tournament rig does: leader
 * REMATCH advances race_index, both endpoints re-Ready (selections survive
 * clear_round and a tournament room offers no track vote), the leader
 * starts the round with that round's raw table mask, both race transports
 * reach READY on a fresh epoch with byte-identical descriptors carrying the
 * cup schedule's track, a window of authored ticks is sealed/drained and
 * hash-compared across both endpoints, and the leader publishes fixed
 * placements (canonical slot 0 first, slot 1 second) so the reducer accrues
 * authentic trophy points. Every step prints an [online-tournament] witness;
 * a stall prints "[online-tournament] result=error step=..." and gives up.
 * Default env (no MDKR_APP_TEST_ONLINE_MODE) never enters this path.
 * ======================================================================== */
namespace {

/* FNV-1a fold of one confirmed canonical frame over the active slots --
 * mirrors tests/test_online_live_adapter.cpp foldFrame, the ROM-free "engine
 * advance" both endpoints must agree on. */
void loopbackFoldFrame(uint64_t &hash, uint32_t tick, uint8_t activeMask,
                       const MdkrInputSet &frame) {
    auto mix = [&hash](uint64_t value) {
        for (unsigned b = 0u; b < 8u; ++b) {
            hash ^= (value >> (b * 8u)) & 0xffu;
            hash *= UINT64_C(1099511628211);
        }
    };
    mix(tick);
    for (unsigned slot = 0u; slot < MDKR_SESSION_MAX_PLAYERS; ++slot) {
        if ((activeMask & (1u << slot)) == 0u) continue;
        mix(frame.slots[slot].buttons);
        mix(static_cast<uint64_t>(
            static_cast<uint8_t>(frame.slots[slot].stick_x)));
        mix(static_cast<uint64_t>(
            static_cast<uint8_t>(frame.slots[slot].stick_y)));
    }
}

/* Seal + drain `ticks` authored ticks on the deterministic synthetic-input
 * fixture and require both endpoints to fold the identical confirmed hash --
 * the transport-level convergence proof for a round the engine does not race
 * (tests/test_online_live_adapter.cpp driveConvergedTicks, real clock). */
bool loopbackDriveConvergedTicks(IMdkrOnlineAdapter *A, IMdkrOnlineAdapter *B,
                                 unsigned ticks, unsigned *outConverged) {
    if (outConverged != nullptr) *outConverged = 0u;
    MdkrOnlineLiveRaceInfo ia{}, ib{};
    if (!mdkr_online_live_adapter_race_info(A, &ia) || !ia.ready ||
        !mdkr_online_live_adapter_race_info(B, &ib) || !ib.ready) {
        return false;
    }
    mdkr_online_live_adapter_race_set_synthetic_input(A, true);
    mdkr_online_live_adapter_race_set_synthetic_input(B, true);
    const uint32_t target = ia.firstTick + ticks - 1u;
    const uint32_t kThrottle = 16u;
    uint32_t cursorA = ia.firstTick, cursorB = ib.firstTick;
    uint64_t hashA = UINT64_C(1469598103934665603);
    uint64_t hashB = UINT64_C(1469598103934665603);
    auto foldReady = [&](IMdkrOnlineAdapter *self, uint32_t &cursor,
                         uint8_t active, uint64_t &hash) {
        MdkrInputSet frame;
        while (mdkr_online_live_adapter_race_inputs_for_tick(self, cursor,
                                                             &frame)) {
            if ((frame.confirmed_mask & active) != active) break;
            loopbackFoldFrame(hash, cursor, active, frame);
            ++cursor;
        }
    };
    for (unsigned step = 0u; step < 20000u; ++step) {
        /* service() before the tick drain -- the load-bearing pump ordering
         * (match_live_adapter.h integration contract). */
        A->service();
        B->service();
        MdkrOnlineLiveRaceInfo na{}, nb{};
        mdkr_online_live_adapter_race_info(A, &na);
        mdkr_online_live_adapter_race_info(B, &nb);
        if (na.nextTick <= target && na.nextTick - cursorA < kThrottle) {
            mdkr_online_live_adapter_race_advance(A);
        }
        if (nb.nextTick <= target && nb.nextTick - cursorB < kThrottle) {
            mdkr_online_live_adapter_race_advance(B);
        }
        foldReady(A, cursorA, ia.activeSlotMask, hashA);
        foldReady(B, cursorB, ib.activeSlotMask, hashB);
        if (cursorA > target && cursorB > target) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    if (cursorA <= target || cursorB <= target || hashA != hashB) return false;
    if (outConverged != nullptr) {
        *outConverged = static_cast<unsigned>(cursorA - ia.firstTick);
    }
    return true;
}

void loopbackTournamentResultsWitness(IMdkrOnlineAdapter *leader,
                                      unsigned raceNumber) {
    MdkrOnlineLobby lobby{};
    (void)mdkr_online_live_adapter_lobby(leader, &lobby);
    std::fprintf(stderr,
                 "[online-tournament] results race=%u race_index=%u track=%u "
                 "points=%u,%u,%u,%u last_placements=%u,%u,%u,%u\n",
                 raceNumber, static_cast<unsigned>(lobby.race_index),
                 static_cast<unsigned>(lobby.selected_track),
                 static_cast<unsigned>(lobby.points[0]),
                 static_cast<unsigned>(lobby.points[1]),
                 static_cast<unsigned>(lobby.points[2]),
                 static_cast<unsigned>(lobby.points[3]),
                 static_cast<unsigned>(lobby.last_placements[0]),
                 static_cast<unsigned>(lobby.last_placements[1]),
                 static_cast<unsigned>(lobby.last_placements[2]),
                 static_cast<unsigned>(lobby.last_placements[3]));
}

bool loopbackTournamentContinuation(MdkrOnlineTestLoopbackRace *race) {
    IMdkrOnlineAdapter *A = race->a.get(); /* room leader */
    IMdkrOnlineAdapter *B = race->b.get();
    std::vector<IMdkrOnlineAdapter *> both{A, B};
    const LoopbackSessionConfig cfg = loopbackSessionConfig();
    auto failStep = [](const char *stepName) {
        std::fprintf(stderr, "[online-tournament] result=error step=%s\n",
                     stepName);
        return false;
    };
    auto lobbyOf = [](IMdkrOnlineAdapter *self) {
        MdkrOnlineLobby lobby{};
        (void)mdkr_online_live_adapter_lobby(self, &lobby);
        return lobby;
    };
    auto bothAtPhase = [&](MdkrOnlinePhase phase) {
        return lobbyOf(A).phase == phase && lobbyOf(B).phase == phase;
    };

    /* Race 1 (the ENGINE race): the launcher already polled the engine's real
     * placements and the leader published them (reportOnlineRaceResults runs
     * before the destroy). Pump both endpoints to the RESULTS phase. */
    if (!loopbackPumpUntil(both, [&]() {
            return bothAtPhase(MDKR_ONLINE_RESULTS) &&
                   loopbackView(A).kind == MDKR_ONLINE_VIEW_RESULTS;
        }, 15000u)) {
        return failStep("race1-results-phase");
    }
    loopbackTournamentResultsWitness(A, 1u);

    for (unsigned round = 1u; round < MDKR_ONLINE_CUP_ROUNDS; ++round) {
        /* Mirror runOnlineLiveEngineSession's post-race teardown: the roster
         * runtime install is once-only while installed, so multi-race rooms
         * clear it between rounds and the next BEGIN_LOADING re-installs. */
        mdkr_net_roster_runtime_clear();

        /* Leader REMATCH (RACE_AGAIN maps onto the reducer's leader-only
         * REMATCH) advances race_index and returns the room to selections. */
        if (!A->submit(loopbackCmd(A, MDKR_ONLINE_VIEW_ACTION_RACE_AGAIN))
                 .accepted) {
            return failStep("rematch-submit");
        }
        if (!loopbackPumpUntil(both, [&]() {
                return bothAtPhase(MDKR_ONLINE_LOBBY) &&
                       lobbyOf(A).race_index == round &&
                       loopbackView(A).kind == MDKR_ONLINE_VIEW_SELECTING &&
                       loopbackView(B).kind == MDKR_ONLINE_VIEW_SELECTING;
            }, 15000u)) {
            return failStep("rematch-advance");
        }
        {
            const MdkrOnlineLobby lobby = lobbyOf(A);
            std::fprintf(stderr,
                         "[online-tournament] rematch race_index=%u "
                         "points=%u,%u,%u,%u\n",
                         static_cast<unsigned>(lobby.race_index),
                         static_cast<unsigned>(lobby.points[0]),
                         static_cast<unsigned>(lobby.points[1]),
                         static_cast<unsigned>(lobby.points[2]),
                         static_cast<unsigned>(lobby.points[3]));
        }

        /* Re-Ready (characters/vehicles survive clear_round, and a
         * tournament room never offers a track vote -- the cup schedule owns
         * the track -- so the primary lands straight on Ready). */
        for (IMdkrOnlineAdapter *self : both) {
            if (!loopbackPumpUntil(both, [&]() {
                    return loopbackView(self).primary.action ==
                           MDKR_ONLINE_VIEW_ACTION_READY;
                }, 10000u)) {
                return failStep("ready-offer");
            }
            self->submit(
                loopbackCmd(self, MDKR_ONLINE_VIEW_ACTION_READY, 0u, 1u));
        }
        if (!loopbackPumpUntil(both, [&]() {
                return loopbackView(A).ready_count == 2u &&
                       loopbackView(B).ready_count == 2u;
            }, 10000u)) {
            return failStep("ready-both");
        }

        /* Leader starts this round with the round track's raw table mask (the
         * admission equality the engine enforces at boot; asserted against
         * the frozen manifest below since rounds 2..4 do not boot it). */
        const uint16_t roundTrack = mdkr_online_cup_track(cfg.cup, round);
        const MdkrOnlineTrackInfo *info = mdkr_online_track_by_id(roundTrack);
        if (info == nullptr) return failStep("round-track-table");
        if (!loopbackPumpUntil(both, [&]() {
                return loopbackView(A).primary.action ==
                       MDKR_ONLINE_VIEW_ACTION_START_RACE;
            }, 10000u)) {
            return failStep("start-offer");
        }
        A->submit(loopbackCmd(A, MDKR_ONLINE_VIEW_ACTION_START_RACE, 0u,
                              info->vehicle_mask));
        if (!loopbackPumpUntil(both, [&]() {
                MdkrOnlineLiveRaceInfo ia{}, ib{};
                return mdkr_online_live_adapter_race_info(A, &ia) && ia.ready &&
                       mdkr_online_live_adapter_race_info(B, &ib) && ib.ready;
            }, 30000u)) {
            return failStep("race-ready");
        }
        if (!mdkr_net_roster_runtime_active()) {
            return failStep("roster-reinstall");
        }
        MdkrOnlineLiveLaunchProbe pa{}, pb{};
        if (!mdkr_online_live_adapter_probe(A, &pa) || !pa.descriptorBuilt ||
            !mdkr_online_live_adapter_probe(B, &pb) || !pb.descriptorBuilt) {
            return failStep("descriptor-probe");
        }
        const bool identical =
            std::memcmp(&pa.descriptor, &pb.descriptor,
                        sizeof(pa.descriptor)) == 0;
        unsigned convergedTicks = 0u;
        const bool converged =
            loopbackDriveConvergedTicks(A, B, 60u, &convergedTicks);
        std::fprintf(stderr,
                     "[online-tournament] race-ready round=%u track=%u "
                     "mask=0x%02x epoch=%u descriptorsIdentical=%u "
                     "convergedTicks=%u hashEqual=%u\n",
                     round + 1u,
                     static_cast<unsigned>(pa.descriptor.manifest.track_id),
                     static_cast<unsigned>(pa.descriptor.manifest.vehicle_mask),
                     static_cast<unsigned>(pa.descriptor.manifest.match_epoch),
                     identical ? 1u : 0u, convergedTicks, converged ? 1u : 0u);
        if (!identical || pa.descriptor.manifest.track_id != roundTrack ||
            pa.descriptor.manifest.vehicle_mask != info->vehicle_mask ||
            pa.descriptor.manifest.match_epoch != round + 1u) {
            return failStep("manifest-mismatch");
        }
        if (!converged) return failStep("transport-convergence");

        /* Transport-level round result: the leader reports fixed placements
         * (canonical slot 0 first, slot 1 second) exactly through the same
         * report seam the launcher uses after a real engine race. */
        const uint8_t placements[4] = {0u, 1u, 0xffu, 0xffu};
        const bool reported =
            mdkr_online_live_adapter_report_results(A, placements);
        std::fprintf(stderr,
                     "[online-tournament] transport results reported round=%u "
                     "placements=%u,%u,%u,%u accepted=%u\n",
                     round + 1u, 0u, 1u, 255u, 255u, reported ? 1u : 0u);
        if (!reported) return failStep("report-results");
        if (!loopbackPumpUntil(both, [&]() {
                return bothAtPhase(MDKR_ONLINE_RESULTS);
            }, 15000u)) {
            return failStep("results-phase");
        }
        loopbackTournamentResultsWitness(A, round + 1u);
    }

    {
        const MdkrOnlineLobby lobby = lobbyOf(A);
        std::fprintf(stderr,
                     "[online-tournament] final cup=%u race_index=%u "
                     "points=%u,%u,%u,%u result=ok\n",
                     static_cast<unsigned>(lobby.cup_id),
                     static_cast<unsigned>(lobby.race_index),
                     static_cast<unsigned>(lobby.points[0]),
                     static_cast<unsigned>(lobby.points[1]),
                     static_cast<unsigned>(lobby.points[2]),
                     static_cast<unsigned>(lobby.points[3]));
    }
    /* Round 4's transport race installed a roster; leave the process clean. */
    mdkr_net_roster_runtime_clear();
    return true;
}

}  // namespace

void OnlineRoom_destroyTestLoopbackRace(MdkrOnlineTestLoopbackRace *race) {
    /* Test-only multi-race continuation: only under
     * MDKR_APP_TEST_ONLINE_MODE=tournament (see the block comment above).
     * Failures are witnessed on stderr -- the caller's exit code belongs to
     * the engine race, so the tournament gate asserts the witnesses. With the
     * default env this is a plain delete, exactly as before. */
    if (race != nullptr) {
        const char *mode = std::getenv("MDKR_APP_TEST_ONLINE_MODE");
        if (mode != nullptr && std::strcmp(mode, "tournament") == 0) {
            (void)loopbackTournamentContinuation(race);
        }
    }
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

    /* REAL provenance compatibility -- the exact bytes the production panel
     * derives -- so this two-process proof exercises the shipped path: both
     * processes run the SAME binary against the SAME US 1.1 ROM (the harness
     * pins race_2p_split's US fixtures; the engine boot re-validates the image
     * authoritatively), so both derive identical bytes by construction and the
     * lobby reducer's JOIN byte-compare accepts; a mismatched companion build
     * is now correctly refused instead of silently admitted. The in-process
     * loopback race above keeps its own fixture: it never leaves one process,
     * and stamping is not required to run it. */
    MdkrOnlineCompatibilityV1 cloudCompat;
    if (!OnlineRoom_liveCompatibilityFromProvenance(1u /* US 1.1 */,
                                                    &cloudCompat)) {
        set_err("no release provenance for this binary (build with "
                "-DMDKR_BUILD_STAMP=<clean commit>); the cloud proof must run "
                "the production compatibility path");
        return nullptr;
    }
    std::unique_ptr<IMdkrOnlineAdapter> adapter =
        OnlineRoom_makeGatedLiveAdapter(cloudCompat, journey, joinCode);
    if (!adapter) {
        set_err("gated live adapter construction refused (missing/invalid "
                "compiled-in MDKR_PARTY_ORIGIN, or a gameplay-determinism env "
                "seam is set -- see the [online-live] line above)");
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
