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
    /* CommandResult correlation: the command_id the server echoed for this
     * step, or 0 when the response carried none. The adapter keys its
     * in-flight command_id->type map on this so a refusal is attributed to the
     * command it actually answered, not merely the most recently SENT one (two
     * commands can be in flight). Lives on the event rather than MdkrOnlineStep
     * so the shared lobby_core.h struct -- compiled into the OFF/release build
     * -- stays byte-identical. */
    uint64_t commandId = 0u;
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
    /* A roster peer was lost (typed PeerLost from the mesh) since the race
     * transport came up. The launcher's engine-session loop polls this to end
     * the visible race instead of predicting against a dead peer forever; the
     * adapter has already latched the matching lobby-facing failure so the
     * post-race panel shows the connection-lost recovery. Cleared when the
     * room returns to the lobby phase. */
    bool peerLost = false;
    /* Non-fatal connection quality: the launcher-side match transport latched
     * a typed recovery (INPUT_GAP / LATE_INPUT) mid-race. Detail lives in
     * MdkrOnlineLiveRaceStats.recovery*. Never terminal by itself. */
    bool connectionDegraded = false;
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
    /* In-race resend sweep (W4 C4): every ~30 service() calls of a
     * race_advance-driven (production loop shape) race, the adapter re-fans
     * the trailing sealed window (newest .. newest-60, stepping 3) so one
     * dropped datagram on the lossy state channel can never wedge a tick
     * unconfirmed forever. resendSweeps counts sweeps, resendBundles the
     * bundles re-sent. The sweep deliberately stays OFF while the driver
     * drains via race_drain_local: that seam's contract is that EVERY mesh
     * transmission is routed through the driver's own (impairment) carrier. */
    uint32_t resendSweeps = 0u;
    uint32_t resendBundles = 0u;
};
bool mdkr_online_live_adapter_race_stats(const IMdkrOnlineAdapter *adapter,
                                         MdkrOnlineLiveRaceStats *out);

/* ---- O-T7 race lifecycle + session-config C APIs (launcher/UI seams) ----- *
 *
 * These free functions resolve the gated LIVE adapter only (false for the
 * fake adapter / nullptr), exactly like the race accessors above. They are
 * the surface the launcher's engine-session loop and the Online Room panel
 * call for the multi-race lifecycle; none of them exists on the view-action
 * vocabulary because lobby_view_model.c does not define leader-config
 * actions yet (that surface is owned by the UI task). */

/* Race results handoff (launcher -> room). Called once after the visible
 * engine session ends, with the per-CANONICAL-SLOT placements from
 * mdkr_online_race_results_poll (0 == first place, 0xFF == no human racer in
 * that slot). The adapter maps canonical slots onto the lobby's SEAT indices
 * from the authoritative roster (canonical slot k == k-th occupied seat, the
 * exact order the manifest froze), packs value = p0|p1<<8|p2<<16|p3<<24 with
 * 0xFF per unoccupied seat, and -- when this endpoint is the room LEADER --
 * sends PUBLISH_RESULTS. A joiner records nothing: the RESULTS lobby phase
 * arrives via snapshot and the adapter walks its local session to the
 * results scene from it. Returns false for a non-live adapter, before the
 * race transport was ready, or when an occupied seat carries no placement.
 * Idempotent per race (a second call after RESULTS is a no-op true). */
bool mdkr_online_live_adapter_report_results(IMdkrOnlineAdapter *adapter,
                                             const uint8_t placements[4]);

/* Leader-only session configuration (UI -> room). Each sends the matching
 * lobby command (SET_MODE=17 / SET_CONFIG_TRACK=18 / SET_CUP=19) through the
 * adapter's command path (stale-revision retry included) and returns whether
 * the command was submitted: false for a non-live adapter, a non-leader, a
 * lobby not in the LOBBY phase, or an out-of-range value (mode > 1, track >
 * 255, cup >= MDKR_ONLINE_CUP_COUNT -- mirroring the reducer's own gates).
 * The authoritative acceptance lands asynchronously in the next lobby
 * snapshot; render from mdkr_online_live_adapter_lobby below. */
bool mdkr_online_live_adapter_set_mode(IMdkrOnlineAdapter *adapter,
                                       unsigned mode);
bool mdkr_online_live_adapter_set_config_track(IMdkrOnlineAdapter *adapter,
                                               unsigned trackId);
bool mdkr_online_live_adapter_set_cup(IMdkrOnlineAdapter *adapter,
                                      unsigned cupId);

/* The current authoritative lobby snapshot (mode / configured_track / cup_id
 * / race_index / points / last_placements included), for panel rendering of
 * the new session-config and results/standings surfaces the shared view
 * model does not carry yet. Fail-atomic: false (non-live adapter, or no
 * snapshot received yet) leaves *out untouched. */
bool mdkr_online_live_adapter_lobby(const IMdkrOnlineAdapter *adapter,
                                    MdkrOnlineLobby *out);

/* Synchronous engine-race-boot handoff retract (W4 m3). The owner MUST call
 * this on the launcher thread BEFORE handing the adapter to any teardown
 * thread: the destructor's own retract still exists but only as a backstop,
 * because a detached-thread destruction races the launcher's
 * OnlineRoom_pollEngineRaceBoot() against a dying adapter. Returns true for
 * a live adapter (no-op true when no handoff is pending or the build carries
 * no boot-handoff seam). */
bool mdkr_online_live_adapter_retract_race_boot(IMdkrOnlineAdapter *adapter);

/* One-shot async command-refusal for the panel: when the room refused this
 * endpoint's last lobby command with no auto-recovery (e.g. SET_CHARACTER ->
 * SELECTION_CONFLICT after both players tapped the same racer), returns true
 * once with the refused MdkrOnlineCommandType and MdkrOnlineError so the UI
 * can un-stage its optimistic pick and explain why. */
bool mdkr_online_live_adapter_take_refusal(IMdkrOnlineAdapter *adapter,
                                           uint32_t *command_type,
                                           uint32_t *error);

#if MDKR_ENABLE_ONLINE_BETA
/* Test-only (beta build): the two pure decisions that govern race-end card
 * truthfulness, exposed so a beta-ON unit test can pin them without a full
 * loopback mesh. `map_lost_reason` is the mesh-reason -> failure mapping (its
 * in-race branches depend on raceBegun); `race_end_demotes` is the F1 rule that
 * keeps a more-specific CONNECTION_UNPLAYABLE from being overwritten by the
 * drain's reason-blind OPPONENT_LEFT. Never called by the launcher. */
MdkrOnlineViewFailure mdkr_online_live_adapter_test_map_lost_reason(
    MdkrMatchPeerLostReason lostReason, bool raceBegun);
bool mdkr_online_live_adapter_test_race_end_demotes(
    MdkrOnlineViewFailure incoming, MdkrOnlineViewFailure current);
/* Pin that the SAS re-verify entry points (forcePhraseRekey / beginReVerify)
 * clear the race-scoped peer-loss latches, so a FIRST-race SAS mismatch cannot
 * carry a stale peer-loss into the re-confirmed race's start barrier. Each
 * pre-arms the latch, runs the entry point on a mesh-free adapter, and returns
 * race_peer_lost() afterward -- both must report false. `via_abort` arms the
 * received-abort latch instead of the peer-lost one (both fold into the same
 * signal). Never called by the launcher. */
bool mdkr_online_live_adapter_test_rekey_clears_peer_loss(bool via_abort);
bool mdkr_online_live_adapter_test_reverify_clears_peer_loss(bool via_abort);
#endif

/* Seal + fan out the race's OPENING input window (firstTick..firstTick+
 * inputDelay) without draining. The launcher's race-start barrier calls this
 * before waiting for the peer's first bundle so the two machines never
 * deadlock each other's barriers; idempotent while waiting (first-write-wins
 * seal history keeps later drains/retransmits byte-identical). */
bool mdkr_online_live_adapter_race_prime_start(IMdkrOnlineAdapter *adapter);

/* ---- ENTER_ANOTHER_CODE step contract (W4 M5 -- for the UI task) ---------- *
 *
 * The live adapter is constructed with a FIXED journey + join code and its
 * room transport begins exactly once, so "Enter Another Code" cannot re-join
 * in place. submit(MDKR_ONLINE_VIEW_ACTION_ENTER_ANOTHER_CODE) therefore:
 *   - best-effort sends MDKR_ONLINE_LEAVE (if a lobby was ever joined),
 *   - resets the local session to HOME and clears lobby/failure state, and
 *   - returns accepted == true with step.error ==
 *     kMdkrOnlineLiveStepEnterAnotherCode.
 * The panel keys on THAT step value: destroy this adapter, present its code
 * entry, and construct a fresh JOIN adapter with the new code
 * (OnlineRoom_makeGatedLiveAdapter). No other accepted step ever carries a
 * nonzero error, so the sentinel cannot collide with the 1..4 refusal codes
 * (those arrive only with accepted == false). */
inline constexpr uint32_t kMdkrOnlineLiveStepEnterAnotherCode = 100u;

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

/* Cheap drain-facing latch: true once a roster peer was lost (typed PeerLost
 * from the mesh) since the race transport came up, cleared when the room
 * returns to the lobby phase. The launcher's engine-session drain
 * (platform/app/main_app.cpp liveDrainMatchInput) polls this every service
 * iteration to END the visible race the instant the opponent vanishes -- instead
 * of predicting against a frozen ghost to the finish line. Equivalent to
 * MdkrOnlineLiveRaceInfo.peerLost but without filling the whole struct. Returns
 * false for a non-live adapter. */
bool mdkr_online_live_adapter_race_peer_lost(const IMdkrOnlineAdapter *adapter);

/* Latch a race-scoped recovery failure onto the adapter's lobby-facing view
 * AFTER the visible engine session ends. The launcher's post-session handling
 * (platform/app/main_app.cpp) calls this to route a peer-loss / start-barrier
 * abort to its dedicated card (OPPONENT_LEFT / OPPONENT_NEVER_STARTED) rather
 * than leaving the generic connection-lost copy the in-race PeerLost latch
 * mapped. Race-scoped: cleared by the adapter when the room returns to the
 * lobby phase, exactly like the ENGINE_FAILED latch. Returns false for a
 * non-live adapter. */
bool mdkr_online_live_adapter_set_race_end_failure(IMdkrOnlineAdapter *adapter,
                                                   MdkrOnlineViewFailure failure);

/* R1: clear ONLY the in-race peer-loss-mapped failure latch (the CONNECTION_CHECK
 * / NETWORKS_CANNOT_CONNECT / etc. that the mid-race PeerLost set via
 * mapLostReason). The launcher calls this on the results-capture path so a
 * genuinely committed finish order -- e.g. the opponent quit during the ~2.5 s
 * post-race window -- fronts the RESULTS screen instead of a misleading
 * connection-lost card. A no-op when failure_ was not loss-mapped (an unrelated
 * VERIFICATION_MISMATCH is preserved). Returns false for a non-live adapter. */
bool mdkr_online_live_adapter_clear_race_loss_failure(
    IMdkrOnlineAdapter *adapter);

/* F3: walk an abandoned race's engine out of RACING (and keep the race-end card
 * fronting over any late lobby snapshot) WITHOUT changing which failure shows.
 * The launcher calls this on the publish-failed keep-the-card path -- a genuine
 * finish was captured but PUBLISH_RESULTS never landed, so the loss-mapped
 * recovery card is retained; without the engine walk that card's PLAY_HERE ->
 * RETURN_HOME would be refused by the still-RACING reducer. Returns false for a
 * non-live adapter. */
bool mdkr_online_live_adapter_walk_engine_out_of_race(
    IMdkrOnlineAdapter *adapter);

/* F3 one-sided-abort guard: broadcast a race-abort to every reachable peer on
 * the reliable control channel. The launcher's engine drain calls this when it
 * aborts the race-start barrier so a slow-but-alive opponent stops waiting on
 * our primed opening fan-out and never races our frozen input to the flag (nor,
 * if it is the leader, publishes fabricated placements). The receiving adapter
 * latches it and its own drain treats it exactly like peer loss. Best-effort
 * (a peer already gone is simply not reached). Returns false for a non-live
 * adapter. */
bool mdkr_online_live_adapter_race_send_abort(IMdkrOnlineAdapter *adapter);

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

/* ---- P2-T1 live selection bridge wiring (beta only) ---------------------- *
 *
 * FORWARD FEED: OnlineRoom_pumpPartyLink projects the adapter's live lobby +
 * view model into a party_link snapshot and publishes it (a no-op until the
 * link is installed and the adapter has an authoritative lobby snapshot).
 * REVERSE FEED: OnlineRoom_pumpPartyLinkIntent one-shot-polls the local
 * player's in-menu intent and dispatches the SAME existing view actions
 * ui_online_room.cpp does (CHOOSE_CHARACTER / CHANGE_SELECTION / READY /
 * START_RACE), deduped so a per-frame republish never spams the reducer.
 * install/clear bookend a session and reset the reverse-feed dedupe. Both pumps
 * are driven from the launcher-code-in-engine-loop service callback during
 * MENUS by later tasks. Defined in platform/app/online_live_wiring.cpp. */
void OnlineRoom_installPartyLink(void);
void OnlineRoom_clearPartyLink(void);
void OnlineRoom_pumpPartyLink(IMdkrOnlineAdapter *adapter);
void OnlineRoom_pumpPartyLinkIntent(IMdkrOnlineAdapter *adapter);
/* Test seam (MDKR_APP_TEST_PARTY_LINK_FAKE): install the link and publish a
 * deterministic scripted snapshot sequence with NO adapter, for the P2 native
 * menu tests to read through the forward feed. */
void OnlineRoom_runTestPartyLinkFake(void);

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
