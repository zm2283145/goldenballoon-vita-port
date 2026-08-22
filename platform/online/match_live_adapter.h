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
    const char *token = std::getenv("MDKR_INTERNAL_TEST_TOKEN");
    return token != nullptr &&
           std::strcmp(token, kMdkrOnlineLiveLobbyTestToken) == 0;
}

/* Launcher-side hook for the gated live adapter. The production MatchRoom HTTP
 * transport + real signal-client mesh backend are owned by the O-T6 two-process
 * race lane, which constructs the live adapter directly through
 * mdkr_online_live_adapter_create with those production transports (mirroring
 * how the Party e2e driver owns the real party transport). Until O-T6 lands
 * this returns nullptr, so the launcher panel -- even with the token gate open
 * -- keeps constructing the fail-closed fake adapter. It is a header-inline
 * stub so the app never links the heavy live-adapter/transport translation
 * units, and no build configuration can start an online race from the panel. */
inline std::unique_ptr<IMdkrOnlineAdapter> OnlineRoom_makeGatedLiveAdapter(
    const MdkrOnlineCompatibilityV1 & /*compatibility*/) {
    return nullptr;
}

#endif /* MDKR_MATCH_LIVE_ADAPTER_H */
