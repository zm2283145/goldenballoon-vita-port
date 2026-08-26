/*
 * O-T3 native LIVE lobby adapter seam.
 *
 * This header carves the launcher-side adapter interface that BOTH the
 * deterministic fake adapter (platform/online/lobby_fake_adapter.*, the
 * view-model oracle) and the real live adapter satisfy, so the launcher panel
 * (platform/app/ui_online_room.cpp) drives one interface instead of embedding
 * a concrete struct. The interface is deliberately narrow: submit a command
 * and get a step, poll the current view/journey projection, and service the
 * adapter's async work each frame (the fake completes its deterministic
 * scheduled outcome; the live adapter drains its MatchRoom transport + peer
 * mesh and advances the state machine).
 *
 * The live adapter itself (declaration at the bottom, definition in
 * match_live_adapter.cpp) is reachable ONLY behind an internal-test-token gate
 * (mdkr_online_live_lobby_gate_open, mirroring the party loopback gate) AND the
 * pre-existing online release locks. A normal build never constructs it.
 */
#ifndef MDKR_MATCH_LIVE_ADAPTER_H
#define MDKR_MATCH_LIVE_ADAPTER_H

#include "online/lobby_core.h"
#include "online/lobby_fake_adapter.h"
#include "online/lobby_view_model.h"
#include "online/match_launch_builder.h"
#include "online/match_peer_transport.h"
#include "session/session_bridge.h"
#include "session/session_types.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>

/* ---- Neutral seam vocabulary -------------------------------------------- */

/* One launcher command. Structurally the fake command, renamed so the seam is
 * not tied to the fake adapter's identity. */
struct MdkrOnlineAdapterCommand {
    uint32_t expectedRevision = 0u;
    uint64_t requestId = 0u;
    MdkrOnlineViewAction action = MDKR_ONLINE_VIEW_ACTION_NONE;
    uint32_t seat = 0u;
    uint32_t value = 0u;
};

/* One command outcome. `error` is an adapter-defined diagnostic code (0 == ok)
 * used only for logging; the launcher branches on `accepted`/`pendingToken`. */
struct MdkrOnlineAdapterStep {
    bool accepted = false;
    bool duplicate = false;
    uint32_t error = 0u;
    uint32_t revision = 0u;
    uint32_t pendingToken = 0u;
};

/* ---- The adapter seam ---------------------------------------------------- */

class IMdkrOnlineAdapter {
public:
    virtual ~IMdkrOnlineAdapter() = default;

    /* command submit -> step */
    virtual MdkrOnlineAdapterStep submit(
        const MdkrOnlineAdapterCommand &command) = 0;

    /* current view projection (kind / failure / controls). Fail-atomic:
     * false leaves *out untouched. */
    virtual bool view(MdkrOnlineViewModel *out) const = 0;

    /* current journey projection (Create / Join / Rematch). */
    virtual MdkrOnlineJourney journey() const = 0;

    /* pending-outcome poll: advance in-flight async work one step. Called
     * every frame. The fake fires its deterministic scheduled completion; the
     * live adapter drains its transport + mesh and advances. */
    virtual void service() = 0;

    /* projections the launcher panel reads directly. */
    virtual uint32_t revision() const = 0;
    virtual MdkrPlayIntent sessionIntent() const = 0;
    virtual bool raceAdmissionEnabled() const = 0;
    virtual bool timeoutExpired() const = 0;

    /* Escape hatch for the fake-only design/evidence gallery and the dev
     * "Finish Preview Race" result stub. The live adapter returns nullptr:
     * those are development tools that never run against a real match. */
    virtual MdkrOnlineFakeAdapter *fakeAdapter() { return nullptr; }
};

/* ---- Fake adapter, adapted onto the seam (header-only, light) ------------ */

/* Wraps the untouched C fake adapter. The 43-case gallery/conformance tests
 * still exercise the C fake directly, so this wrapper cannot disturb them. */
class MdkrOnlineFakeAdapterSeam final : public IMdkrOnlineAdapter {
public:
    bool init(uint64_t sessionId, const MdkrOnlineCompatibilityV1 *compatibility,
              bool raceAdmissionEnabled) {
        scheduledToken_ = 0u;
        scheduledFrames_ = 0u;
        return mdkr_online_fake_init(&adapter_, sessionId, compatibility,
                                     raceAdmissionEnabled);
    }

    MdkrOnlineAdapterStep submit(
        const MdkrOnlineAdapterCommand &command) override {
        MdkrOnlineFakeCommand c;
        std::memset(&c, 0, sizeof(c));
        c.expected_revision = command.expectedRevision;
        c.request_id = command.requestId;
        c.action = command.action;
        c.seat = command.seat;
        c.value = command.value;
        const MdkrOnlineFakeStep s = mdkr_online_fake_dispatch(&adapter_, &c);
        /* Preserve the launcher's previous one-frame-delayed callback: an
         * accepted command that opens a pending outcome completes on the
         * service() two frames later. */
        if (s.accepted && s.pending_token != 0u) {
            scheduledToken_ = s.pending_token;
            scheduledFrames_ = 1u;
        }
        return toStep(s);
    }

    bool view(MdkrOnlineViewModel *out) const override {
        return mdkr_online_fake_view(&adapter_, out);
    }

    MdkrOnlineJourney journey() const override { return adapter_.journey; }

    void service() override {
        if (scheduledToken_ == 0u) return;
        if (scheduledFrames_ != 0u) {
            --scheduledFrames_;
            return;
        }
        const uint32_t token = scheduledToken_;
        scheduledToken_ = 0u;
        (void)mdkr_online_fake_complete(&adapter_, token,
                                        MDKR_ONLINE_VIEW_FAILURE_NONE);
    }

    uint32_t revision() const override { return adapter_.revision; }
    MdkrPlayIntent sessionIntent() const override {
        return adapter_.session.state.intent;
    }
    bool raceAdmissionEnabled() const override {
        return adapter_.race_admission_enabled;
    }
    bool timeoutExpired() const override { return adapter_.timeout_expired; }

    MdkrOnlineFakeAdapter *fakeAdapter() override { return &adapter_; }

private:
    static MdkrOnlineAdapterStep toStep(const MdkrOnlineFakeStep &s) {
        MdkrOnlineAdapterStep out;
        out.accepted = s.accepted;
        out.duplicate = s.duplicate;
        out.error = static_cast<uint32_t>(s.error);
        out.revision = s.revision;
        out.pendingToken = s.pending_token;
        return out;
    }

    MdkrOnlineFakeAdapter adapter_{};
    uint32_t scheduledToken_ = 0u;
    unsigned scheduledFrames_ = 0u;
};

/* ---- Live adapter: MatchRoom transport seam ----------------------------- *
 *
 * The launcher-side native client of the MatchRoom routes (create / join /
 * code / state / command over same-origin HTTP, with the state subscription).
 * The live adapter owns exactly one; tests inject a small in-process double
 * driving the real lobby reducer, production injects the HTTP client. All calls
 * are launcher-thread; the client copies observations into events drained by
 * pump(), the same discipline the signal client and peer mesh use.
 */
struct MdkrOnlineRoomEvent {
    enum class Type { Ready, State, CommandResult, Failure };
    Type type = Type::Failure;
    /* Ready: authenticated identity + delivered ICE servers + first snapshot. */
    uint64_t localEndpointId = 0u;
    std::string roomId;     /* 22-char base64url (production signal binding). */
    std::string credential; /* 43-char base64url (production subprotocol). */
    std::vector<MdkrMatchPeerIceServer> iceServers;
    /* Ready / State: the authoritative lobby snapshot. */
    MdkrOnlineLobby lobby{};
    bool haveLobby = false;
    /* CommandResult: the server's step for a submitted command. */
    MdkrOnlineStep step{};
    /* Failure: a pre-mapped stable launcher failure -- never a raw wire code. */
    MdkrOnlineViewFailure failure = MDKR_ONLINE_VIEW_FAILURE_NONE;
};

class MdkrOnlineRoomTransport {
public:
    virtual ~MdkrOnlineRoomTransport() = default;
    /* Exactly one of the begin* calls is made once, per the journey. */
    virtual bool beginCreate(const MdkrOnlineCompatibilityV1 &compatibility,
                             unsigned seatCount) = 0;
    virtual bool beginJoin(const std::string &capability,
                           const MdkrOnlineCompatibilityV1 &compatibility,
                           unsigned seatCount) = 0;
    virtual bool beginJoinByCode(const std::string &code,
                                 const MdkrOnlineCompatibilityV1 &compatibility,
                                 unsigned seatCount) = 0;
    /* Submit one authenticated lobby command (async). */
    virtual bool submitCommand(const MdkrOnlineCommand &command) = 0;
    /* Drive I/O and drain events, oldest first, into `out` (cleared first). */
    virtual void pump(std::vector<MdkrOnlineRoomEvent> &out) = 0;
    virtual void close() = 0;
};

/* ---- Live adapter: peer-mesh signaling backend -------------------------- *
 *
 * Supplies the MdkrMatchPeerSignalFeed the peer mesh borrows, brought up at the
 * Loading barrier once the authenticated identity is known. The backend OWNS
 * the returned feed (and any signal client behind it) until reset(). Tests
 * return a loopback-hub feed; production wraps a real MdkrMatchSignalClient.
 */
class MdkrOnlineMeshSignalBackend {
public:
    virtual ~MdkrOnlineMeshSignalBackend() = default;
    virtual MdkrMatchPeerSignalFeed *beginSignaling(
        uint64_t localEndpointId, uint32_t generation,
        const std::string &roomId, const std::string &credential,
        const std::vector<MdkrMatchPeerIceServer> &iceServers) = 0;
    virtual void reset() = 0;
};

/* ---- Live adapter: construction options --------------------------------- */
struct MdkrOnlineLiveAdapterOptions {
    uint64_t sessionId = 0u;
    MdkrOnlineCompatibilityV1 compatibility{};
    MdkrOnlineJourney journey = MDKR_ONLINE_JOURNEY_CREATE;
    unsigned localSeatCount = 1u;
    std::string joinCapability; /* journey == JOIN via invite link */
    std::string joinCode;       /* journey == JOIN via fallback code */
    /* Retail-identity clamp input: what the LOCAL mod roster resolves for each
     * local player slot. Read once, frozen at the Loading barrier. */
    MdkrMatchLocalRosterV1 localRoster{};
    bool raceAdmissionEnabled = false;
    /* Launcher-local ROM verification result (the ROM_VERIFIED preflight flag).
     * Never sourced from room/service data. */
    bool romVerified = false;
    uint8_t inputDelay = 2u; /* manifest input delay, <= 8 */
    std::function<uint64_t()> nowMs; /* clock seam; empty -> steady clock */
    /* Borrowed; both must outlive the adapter. */
    MdkrOnlineRoomTransport *room = nullptr;
    MdkrOnlineMeshSignalBackend *meshBackend = nullptr;
};

/* Validates options and returns the composed live adapter, or nullptr with
 * *error set. Never touches the network at construction: the room transport
 * begins on the first submit(CREATE_ROOM/JOIN_ROOM). */
std::unique_ptr<IMdkrOnlineAdapter> mdkr_online_live_adapter_create(
    const MdkrOnlineLiveAdapterOptions &options, std::string *error = nullptr);

/* Test-only introspection of the live adapter's Loading-barrier launch build,
 * so a test can prove the descriptor was built through the O-T5 clamp and
 * installed without a second engine process. Returns false for a non-live
 * adapter or before the build has run. */
struct MdkrOnlineLiveLaunchProbe {
    bool descriptorBuilt = false;
    MdkrMatchLaunchRefusal refusal = MDKR_MATCH_LAUNCH_ADMITTED;
    bool installed = false;
    bool phraseConfirmed = false;
    bool preflightReady = false;
    MdkrMatchLaunchDescriptorV1 descriptor{};
};
bool mdkr_online_live_adapter_probe(const IMdkrOnlineAdapter *adapter,
                                    MdkrOnlineLiveLaunchProbe *out);

/* ---- O-T6 post-install per-tick race feed ------------------------------- *
 *
 * Once the descriptor installs, the adapter owns a launcher-side match
 * transport bound to a headless session bridge -- the live replacement for the
 * loopback simulator in main_app.cpp's MatchInputProviderContext. Opened INPUT
 * envelopes from the mesh are fed to mdkr_match_transport_receive per covered
 * tick inside service(); race_advance() seals the local endpoint's 3-frame
 * bundle, fans it out on the mesh and drains one authored tick through the
 * transport. The engine model that advances over the resulting canonical input
 * frames -- and the state hash the two processes compare -- is owned by the
 * O-T6 race driver, which reads confirmed frames back through
 * race_inputs_for_tick(). These functions return false for a non-live adapter
 * or before install; they never run on the launcher's fake-adapter path.
 *
 * INTEGRATION CONTRACT -- pump ordering (W3 N5, review A5). The send side is
 * immediate (race_advance seals and hands the bundle to the mesh
 * synchronously), but the RECEIVE side is deferred: remote input only moves
 * from the mesh's bounded callback queue into mdkr_match_transport_receive
 * inside service(). Every integration loop MUST therefore call service()
 * immediately BEFORE the tick drain (race_advance / race_drain_local) in the
 * same frame, so remote input entering the fold is at most one service()
 * old. Draining first and servicing after adds a full frame interval (33 ms
 * at 30 Hz -- one extra resim tick) to every remote input's effective age;
 * it never changes local feel (there is no input delay on the local drain)
 * but it deepens every remote-kart correction for free. Both shipped
 * drivers (tests/test_online_live_transport_e2e_driver.cpp and the
 * in-process race in tests/test_online_live_adapter.cpp) order
 * service()-then-advance and are the reference loop shapes. */
struct MdkrOnlineLiveRaceInfo {
    bool ready = false;
    uint32_t matchEpoch = 0u;
    uint32_t firstTick = 0u;
    uint32_t nextTick = 0u;      /* the next authored tick race_advance drains */
    uint8_t activeSlotMask = 0u; /* every canonical slot in the manifest */
    uint8_t localSlotMask = 0u;  /* this endpoint's owned canonical slots */
    uint8_t remoteSlotMask = 0u; /* peers' canonical slots (fed from the mesh) */
    uint8_t inputDelay = 0u;     /* ticks the sealed input leads the drain */
};
bool mdkr_online_live_adapter_race_info(const IMdkrOnlineAdapter *adapter,
                                        MdkrOnlineLiveRaceInfo *out);
/* Seal + fan out this endpoint's local input for the delayed future tick and
 * drain the current authored tick. This endpoint records the exact frame it
 * seals, so the copy the peer receives, this endpoint's own later drain of that
 * tick, and any retransmit all commit byte-identical canonical inputs -- both
 * endpoints converge because each contributes only its own local seats and
 * every other seat is confirmed from the peer's fanned-out bundle. (Local input
 * is the real controller in production; the transport/rollback test seams select
 * the deterministic fixture via race_set_synthetic_input.) */
bool mdkr_online_live_adapter_race_advance(IMdkrOnlineAdapter *adapter);
/* Select the deterministic raceLocalSample fixture as this endpoint's local
 * input source (on=true), instead of the real physical pad. The transport and
 * rollback test seams enable it because they need per-tick input variation to
 * force genuine corrections; the shipped interactive boot leaves it off so the
 * race is driven by the player's controller. */
bool mdkr_online_live_adapter_race_set_synthetic_input(
    IMdkrOnlineAdapter *adapter, bool on);
/* Stage the real local controller pads for the next sealed tick. `local` is in
 * local-seat order (seat i reads controller port i), matching the physical[]
 * the engine hands the match-input drain callback. Ignored in synthetic mode. */
bool mdkr_online_live_adapter_race_set_local_input(
    IMdkrOnlineAdapter *adapter, const MdkrPadSample *local, unsigned count);
/* Retransmit the local input covering `newestTick` (and the two ticks before
 * it) without draining, so a datagram dropped on the lossy state channel cannot
 * permanently wedge the peer's contiguous confirmation. */
bool mdkr_online_live_adapter_race_resend(IMdkrOnlineAdapter *adapter,
                                          uint32_t newestTick);
/* Drain the current authored tick WITHOUT sealing/fanning out any local bundle
 * -- the "advance-minus-send" half of race_advance. Test-lane seam (O2.2-sim):
 * lets a deterministic impairment matrix separate the launcher-side engine's
 * real-time drain (which keeps predicting through a network stall) from the
 * network send, so the driver can route every mesh transmission through a
 * seeded net_impairment carrier while the engine keeps advancing. Every
 * surviving transmission still crosses the real mesh via race_resend; this only
 * decouples local progress from that send. Additive, reachable only through the
 * token-gated live adapter (the whole race API is test-only; the production
 * launcher never constructs this adapter), so it changes no shipped netcode. */
bool mdkr_online_live_adapter_race_drain_local(IMdkrOnlineAdapter *adapter);
/* The canonical frame retained for an authored tick (confirmed_mask tells the
 * driver when every active slot's input has actually arrived). */
bool mdkr_online_live_adapter_race_inputs_for_tick(
    IMdkrOnlineAdapter *adapter, uint32_t tick, MdkrInputSet *out);

/* Diagnostics for the race lane (never terminal). */
struct MdkrOnlineLiveRaceStats {
    uint64_t inputEnvelopesReceived = 0u; /* opened INPUT envelopes from peers */
    uint64_t meshRejectedState = 0u;
    uint64_t meshIgnoredStaleSignals = 0u;
    uint64_t meshDroppedEvents = 0u;
    uint32_t transportAccepted = 0u;
    uint32_t transportCorrected = 0u;
    uint32_t transportDuplicates = 0u;
    uint32_t transportOutOfWindow = 0u;
    uint32_t transportDrained = 0u;
    /* Sticky typed recovery latched by the launcher-side match transport: 0 ==
     * none, 1 == INPUT_GAP (a remote slot's confirmation fell irrecoverably
     * behind the drain frontier), 2 == LATE_INPUT (a delivered input arrived
     * out of the retained rollback window). The impairment matrix asserts this
     * fires -- rather than a silent desync -- when a profile exceeds the window. */
    uint32_t recoveryReason = 0u;
    uint32_t recoveryFirstTick = 0u;    /* first unrecoverable authored tick */
    uint32_t recoveryObservedTick = 0u; /* drain tick where it was observed */
    uint8_t recoverySlot = 0u;          /* the stalled canonical slot */
};
bool mdkr_online_live_adapter_race_stats(const IMdkrOnlineAdapter *adapter,
                                         MdkrOnlineLiveRaceStats *out);

/* ---- O-T6b engine match-input seam --------------------------------------- *
 *
 * The visible engine (mdkr64_engine_boot) drives its per-tick canonical input
 * through the process-global MdkrMatchInputSource (platform/net/match_input_
 * runtime.h): drain, inputs_for_tick, take_dirty and ai_mask_for_tick. The live
 * adapter's race transport already answers the first two through race_advance /
 * race_inputs_for_tick; these two accessors complete the seam so main_app.cpp
 * can back the engine input provider with the LIVE adapter instead of its
 * loopback simulator. Both return false for a non-live adapter or before the
 * race transport is ready. */

/* Oldest authored tick whose confirmed canonical input changed since the last
 * call (mirrors mdkr_match_transport_take_dirty): the engine reconciles/replays
 * from it. Returns false (and leaves *tick untouched) when nothing is dirty. */
bool mdkr_online_live_adapter_race_take_dirty(IMdkrOnlineAdapter *adapter,
                                              uint32_t *tick);
/* AI-takeover mask for an authored tick (mirrors the transport's schedule).
 * Returns true with *slot_mask == 0 when no takeover is scheduled, so the engine
 * provider's begin_tick contract is always satisfiable on the live path. */
bool mdkr_online_live_adapter_race_ai_mask(IMdkrOnlineAdapter *adapter,
                                           uint32_t tick, uint8_t *slot_mask);

/* True once every remote canonical slot's input for `tick` has been received
 * into the transport history (independent of the drain frontier). The in-process
 * loopback proof polls this to deliver peer input synchronously before draining,
 * so the visible engine commits confirmed frames and never rolls back into the
 * paused race countdown. Always true when the endpoint owns every slot. */
bool mdkr_online_live_adapter_race_remote_ready(IMdkrOnlineAdapter *adapter,
                                                uint32_t tick);

/* ---- Internal-test-token gate for the live adapter ---------------------- *
 *
 * Fail-closed, mirroring platform/party/native_party_host.h's loopback gate:
 * the live adapter is reachable only when MDKR_INTERNAL_TEST_TOKEN holds this
 * adapter's own versioned value. This is an ADDITIONAL required condition on
 * top of the compile-time Online Room preview gate and the shipped online
 * release locks (publisher config, presenter fixture check) -- never a
 * replacement for any of them. A production build leaves the token unset, so
 * the launcher keeps instantiating the fake adapter and the fail-closed "not
 * enabled" surface. `inline` because the launcher and the tests link it into
 * different binaries and each must evaluate it independently.
 */
inline constexpr char kMdkrOnlineLiveLobbyTestToken[] = "mdkr64-online-live-v1";

inline bool mdkr_online_live_lobby_gate_open() {
#if MDKR_ENABLE_ONLINE_BETA
    /* Native online BETA build: the compile-time beta gate replaces the
     * internal-test-token gate, so beta testers need no MDKR_INTERNAL_TEST_TOKEN.
     * The other in-code fences (2-endpoint, retail clamp, STUN-only, the SAS
     * verification phrase) are unchanged and still enforced downstream. */
    return true;
#else
    const char *token = std::getenv("MDKR_INTERNAL_TEST_TOKEN");
    return token != nullptr &&
           std::strcmp(token, kMdkrOnlineLiveLobbyTestToken) == 0;
#endif
}

/* Launcher-side hook for the gated live adapter.
 *
 * In a NATIVE ONLINE BETA build (MDKR_ENABLE_ONLINE_BETA) this is a forward
 * declaration; the real owning factory lives out-of-line in
 * platform/app/online_live_wiring.cpp, composing the production MatchRoom HTTP
 * transport + real signal-client mesh backend + O-T3 live adapter EXACTLY as the
 * O-T6 e2e driver does, behind the beta gate and fenced to one local seat,
 * retail identities and STUN-only. journey/joinCode select CREATE vs
 * JOIN-by-code before construction (the live adapter is built with a fixed
 * journey, unlike the fake).
 *
 * In every other build it stays a header-inline stub returning nullptr, so the
 * app never links the heavy live-adapter/transport translation units and the
 * launcher -- even with the token gate open -- keeps constructing the
 * fail-closed fake adapter; no shipping configuration can start an online race
 * from the panel. */
#if MDKR_ENABLE_ONLINE_BETA
std::unique_ptr<IMdkrOnlineAdapter> OnlineRoom_makeGatedLiveAdapter(
    const MdkrOnlineCompatibilityV1 &compatibility,
    MdkrOnlineJourney journey = MDKR_ONLINE_JOURNEY_CREATE,
    const std::string &joinCode = std::string());

/* Beta-only: the creator's invite (6-digit fallback code + invite URL) so the
 * Online Room panel can render the invite card (big code + Copy + QR). Returns
 * false for a non-live adapter, a joiner, or before the room is Ready; the
 * out-params are decoupled from the transport header so the panel need not pull
 * it in. Defined out-of-line in platform/app/online_live_wiring.cpp. */
bool OnlineRoom_liveInvite(IMdkrOnlineAdapter *adapter, std::string *code,
                           std::string *inviteUrl);

/* ---- O-T6b visible-engine race-boot handoff (beta only) ------------------ *
 *
 * The make-or-break seam: turning the headless online race into a VISIBLE 3D
 * race. It is deliberately driven off ADAPTER STATE, not a UI callback -- the
 * launcher's interactive loop (platform/app/main_app.cpp) polls it every frame
 * and never needs a change in the UX-owned Online Room panel.
 *
 * When a gated live adapter's race transport becomes ready (LiveAdapter::
 * setUpRace succeeded inside install()), the adapter publishes ITSELF here; a
 * teardown / SAS re-verify retracts it. OnlineRoom_pollEngineRaceBoot() hands
 * the pending adapter to main_app EXACTLY ONCE, which then boots the visible
 * engine with the live transport as the match-input source and, on engine exit,
 * tears down and returns to the launcher.
 *
 * The published pointer is the raw LiveAdapter (an IMdkrOnlineAdapter*), so the
 * mdkr_online_live_adapter_race_* accessors resolve it. Its lifetime is the
 * panel-owned adapter's lifetime; the blocking engine boot cannot outlive it
 * because the launcher loop (which owns the adapter) is suspended for the race.
 * Defined out-of-line in platform/app/online_live_wiring.cpp. */
void OnlineRoom_publishEngineRaceBoot(IMdkrOnlineAdapter *adapter);
void OnlineRoom_retractEngineRaceBoot(IMdkrOnlineAdapter *adapter);
IMdkrOnlineAdapter *OnlineRoom_pollEngineRaceBoot(void);

/* ---- Engine-roster ownership guard (local-Play beach-ball fix, beta only) -- *
 *
 * The process-global engine roster (platform/net/net_roster_runtime) is installed
 * by an online boot and must never be inherited by a later local-Play boot, which
 * would flip the engine into online-race mode and stall on network input a local
 * race never sends. The owner token lives in the beta wiring layer (not in the
 * always-compiled net_roster TU) so the OFF/release build stays byte-identical;
 * the pure decision is mdkr_net_roster_guard_decides_clear() in
 * net_roster_runtime.h. Defined in platform/app/online_live_wiring.cpp. */
void OnlineRoom_setRosterOwner(uint64_t token);
uint64_t OnlineRoom_rosterOwner(void);
/* Force-clear the installed roster BEFORE booting when this boot (owner `token`;
 * 0 == "installs no roster", ordinary local Play) does not own it. Returns true
 * if a foreign roster was discarded so the boot starts clean. */
bool OnlineRoom_guardRosterOwner(uint64_t token);

/* ---- Test-only in-process loopback race pair (MDKR_APP_TEST_ONLINE_LIVE) --- *
 *
 * Builds two real live adapters over the O-T2 loopback signal hub + an
 * in-process MatchRoom double and drives them to a READY race transport, so a
 * headless proof can boot the VISIBLE engine on endpoint A's live transport
 * while endpoint B seals real input over the mesh. Returns nullptr on failure
 * (*error set). Ordinary play never calls this. Defined in
 * platform/app/online_live_wiring.cpp. */
struct MdkrOnlineTestLoopbackRace; /* opaque owner of doubles + adapters */
MdkrOnlineTestLoopbackRace *OnlineRoom_makeTestLoopbackRace(std::string *error);
IMdkrOnlineAdapter *OnlineRoom_testLoopbackVisible(
    MdkrOnlineTestLoopbackRace *race);
IMdkrOnlineAdapter *OnlineRoom_testLoopbackPeer(
    MdkrOnlineTestLoopbackRace *race);
void OnlineRoom_destroyTestLoopbackRace(MdkrOnlineTestLoopbackRace *race);

/* ---- Test-only single-adapter CLOUD race driver ---------------------------
 * (MDKR_APP_TEST_ONLINE_LIVE_CLOUD) ------------------------------------------
 *
 * The loopback race above proves the engine-boot wiring with BOTH endpoints
 * (and both live adapters) inside one process over an in-process signal hub.
 * This one drives a SINGLE production-shaped live adapter -- built by the
 * SAME OnlineRoom_makeGatedLiveAdapter factory the real Online Room panel
 * uses, against the compiled-in MDKR_PARTY_ORIGIN -- through create/join,
 * secure setup, selection and loading to a READY race transport. It is
 * exactly the state machine tests/test_online_live_transport_e2e_driver.cpp
 * already proves against a real MatchRoom Worker (and, via
 * tools/online/cloud_two_session_smoke.py, against the real deployed cloud
 * origin), so a companion PROCESS running the peer role over the SAME real
 * cloud origin supplies the other endpoint's input over the mesh and
 * `runOnlineLiveEngineSession(host, config, adapter, nullptr)` can boot the
 * visible engine on a genuinely independent, real adapter instance -- the
 * `peer == nullptr` production path liveDrainMatchInput() already implements.
 *
 * Narrates each phase as `[online-live-cloud] key=value` lines on stderr for
 * a driving harness to scrape (the room code line lets a two-process launcher
 * hand the code to the joiner). `timeoutMs` bounds the ENTIRE setup dance (a
 * single steady-clock deadline, matching the e2e driver's own budget model);
 * it does NOT bound the race itself, which the caller's autoplay tick/frame
 * limit and the launching harness's process timeout already bound. Returns
 * nullptr on failure or timeout (*error set). Ordinary play never calls this.
 * Defined in platform/app/online_live_wiring.cpp. */
struct MdkrOnlineTestCloudLiveSession; /* opaque owner of the live adapter */
MdkrOnlineTestCloudLiveSession *OnlineRoom_makeTestCloudLiveSession(
    MdkrOnlineJourney journey, const std::string &joinCode, unsigned character,
    unsigned track, unsigned vehicleMask, uint64_t timeoutMs,
    std::string *error);
IMdkrOnlineAdapter *OnlineRoom_testCloudLiveAdapter(
    MdkrOnlineTestCloudLiveSession *session);
void OnlineRoom_destroyTestCloudLiveSession(
    MdkrOnlineTestCloudLiveSession *session);
#else
inline std::unique_ptr<IMdkrOnlineAdapter> OnlineRoom_makeGatedLiveAdapter(
    const MdkrOnlineCompatibilityV1 & /*compatibility*/,
    MdkrOnlineJourney /*journey*/ = MDKR_ONLINE_JOURNEY_CREATE,
    const std::string & /*joinCode*/ = std::string()) {
    return nullptr;
}
#endif

#endif /* MDKR_MATCH_LIVE_ADAPTER_H */
