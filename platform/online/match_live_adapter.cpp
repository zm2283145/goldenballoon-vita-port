/*
 * O-T3 native LIVE lobby adapter.
 *
 * Composes the now-complete online transport into a launcher-owned adapter that
 * drives a real online match, reusing (never re-implementing) every proven
 * layer:
 *   - lobby control over the MatchRoom transport seam (create/join/state/
 *     command), projected through the SHARED lobby_view_model reducer -- the
 *     same reducer the fake adapter uses, so the 43-case view-model oracle
 *     still describes every live view;
 *   - the O-T2 peer mesh (match_peer_transport) over an injected signal feed,
 *     brought up when Check Setup enters the room preflight phase so the
 *     transcript phrase can be compared before selections (the view-model
 *     shows the phrase only in that phase). The mesh is keyed with the stable
 *     nonzero lobby leader_generation because the lobby match_epoch is 0 until
 *     the leader begins loading; that keying epoch is independent of the
 *     descriptor's match_epoch, which is only carried inside the attestation;
 *   - at the Loading barrier: the O-T5 launch builder (retail-identity clamp)
 *     builds the descriptor from the frozen lobby snapshot -- the ONLY seam
 *     that produces a launch descriptor, and no roster mutation is possible
 *     between the authoritative LOADING snapshot and this build;
 *   - the preflight consensus barrier (match_preflight) over the descriptor,
 *     transcript and graph digests carried as sealed fragments on the mesh's
 *     reliable control channel, with all three flags (ROM_VERIFIED,
 *     PHRASE_CONFIRMED, CHANNELS_READY);
 *   - installation into the engine roster runtime (net_roster_runtime).
 *
 * Transport/lobby failures are mapped to the view-model's typed failure
 * vocabulary (docs/ref/match-signaling-v1.md, docs/ref/match-preflight-v1.md);
 * raw transport codes never become UI. Retry/backoff is launcher-owned; game
 * code is untouched.
 *
 * The transcript_digest bound into each attestation is the mesh's RAW 32-byte
 * transcript digest (mdkr_match_peer_transcript_digest, exposed by the mesh),
 * the canonical commitment-verified fingerprint the human phrase is itself
 * derived from. Binding the raw digest rather than SHA-256(phrase) closes the
 * O-T3 note: consensus is over the full transcript, not the phrase's lossy word
 * mapping. Every honest peer derives the identical digest, so consensus holds.
 *
 * Post-install (O-T6) the adapter owns a launcher-side match transport bound to
 * a headless session bridge: opened INPUT envelopes from the mesh drive
 * mdkr_match_transport_receive per covered tick, and race_advance() seals the
 * local endpoint's bundle onto the mesh and drains one authored tick -- the
 * live replacement for main_app.cpp's loopback MatchInputProviderContext.
 */
#include "match_live_adapter.h"

#include "online_track_table.h"
#include "net/match_input_bundle.h"
#include "net/match_preflight.h"
#include "net/match_transport.h"
#include "net/net_roster.h"
#include "net/net_roster_runtime.h"
#include "session/session_bridge.h"
#include "session/session_core.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <vector>

// Always-on (beta) diagnostics for the Start Race -> race-ready chain. Every
// tagged line ([ROOM-PHASE] [ONLINE] [START] [MESH] [PREFLIGHT] [LOADING]) is
// grep-able so the owner's two-machine capture pinpoints exactly which gate a
// stall sits behind. Compiled out of any non-beta build of this TU.
#if MDKR_ENABLE_ONLINE_BETA
#define MDKR_ONLINE_LOG(...) std::fprintf(stderr, __VA_ARGS__)
#else
/* Compiled-out logging that references every argument (and any log-only static
 * helper) in a POTENTIALLY-evaluated but dead branch, so -Wunused-parameter and
 * -Wunneeded-internal-declaration stay quiet at the call sites -- e.g. the
 * [PREFLIGHT]/[START] gate helpers whose only use is the log line -- while the
 * false condition folds the fprintf away to nothing (byte-identical no-op). The
 * test target compiles this TU with beta off under -Werror. */
#define MDKR_ONLINE_LOG(...) \
    ((void)(false ? std::fprintf(stderr, __VA_ARGS__) : 0))
#endif

namespace {

const char *roomPhaseName(MdkrRoomPhase room) {
    switch (room) {
        case MDKR_ROOM_NONE: return "NONE";
        case MDKR_ROOM_OPEN: return "OPEN";
        case MDKR_ROOM_PREFLIGHT: return "PREFLIGHT";
        case MDKR_ROOM_SELECTING: return "SELECTING";
        case MDKR_ROOM_LOADING: return "LOADING";
        case MDKR_ROOM_COUNTDOWN: return "COUNTDOWN";
        case MDKR_ROOM_RACING: return "RACING";
        case MDKR_ROOM_RESULTS: return "RESULTS";
        case MDKR_ROOM_CLOSED: return "CLOSED";
    }
    return "?";
}

const char *lobbyPhaseName(MdkrOnlinePhase phase) {
    switch (phase) {
        case MDKR_ONLINE_LOBBY: return "LOBBY";
        case MDKR_ONLINE_LOADING: return "LOADING";
        case MDKR_ONLINE_RACING: return "RACING";
        case MDKR_ONLINE_RESULTS: return "RESULTS";
        case MDKR_ONLINE_CLOSED: return "CLOSED";
    }
    return "?";
}

const char *lobbyErrorName(MdkrOnlineError e) {
    switch (e) {
        case MDKR_ONLINE_OK: return "OK";
        case MDKR_ONLINE_ERROR_PROTOCOL: return "PROTOCOL";
        case MDKR_ONLINE_ERROR_STALE_REVISION: return "STALE_REVISION";
        case MDKR_ONLINE_ERROR_STALE_COMMAND: return "STALE_COMMAND";
        case MDKR_ONLINE_ERROR_COMMAND_CONFLICT: return "COMMAND_CONFLICT";
        case MDKR_ONLINE_ERROR_INVALID_STATE: return "INVALID_STATE";
        case MDKR_ONLINE_ERROR_UNAUTHORIZED: return "UNAUTHORIZED";
        case MDKR_ONLINE_ERROR_NOT_FOUND: return "NOT_FOUND";
        case MDKR_ONLINE_ERROR_ALREADY_JOINED: return "ALREADY_JOINED";
        case MDKR_ONLINE_ERROR_INCOMPATIBLE: return "INCOMPATIBLE";
        case MDKR_ONLINE_ERROR_CAPACITY: return "CAPACITY";
        case MDKR_ONLINE_ERROR_NOT_READY: return "NOT_READY";
        case MDKR_ONLINE_ERROR_DISCONNECTED: return "DISCONNECTED";
        case MDKR_ONLINE_ERROR_SELECTION_CONFLICT: return "SELECTION_CONFLICT";
        case MDKR_ONLINE_ERROR_ILLEGAL_VEHICLE: return "ILLEGAL_VEHICLE";
    }
    return "?";
}

const char *lobbyCommandName(MdkrOnlineCommandType t) {
    switch (t) {
        case MDKR_ONLINE_JOIN: return "JOIN";
        case MDKR_ONLINE_LEAVE: return "LEAVE";
        case MDKR_ONLINE_DISCONNECT: return "DISCONNECT";
        case MDKR_ONLINE_RECONNECT: return "RECONNECT";
        case MDKR_ONLINE_SET_READY: return "SET_READY";
        case MDKR_ONLINE_SET_VOTE: return "SET_VOTE";
        case MDKR_ONLINE_BEGIN_LOADING: return "BEGIN_LOADING";
        case MDKR_ONLINE_ACK_LOADED: return "ACK_LOADED";
        case MDKR_ONLINE_BEGIN_RACE: return "BEGIN_RACE";
        case MDKR_ONLINE_PUBLISH_RESULTS: return "PUBLISH_RESULTS";
        case MDKR_ONLINE_REMATCH: return "REMATCH";
        case MDKR_ONLINE_TRANSFER_LEADER: return "TRANSFER_LEADER";
        case MDKR_ONLINE_CLOSE: return "CLOSE";
        case MDKR_ONLINE_SET_CHARACTER: return "SET_CHARACTER";
        case MDKR_ONLINE_SET_VEHICLE: return "SET_VEHICLE";
        case MDKR_ONLINE_CANCEL_LOADING: return "CANCEL_LOADING";
        case MDKR_ONLINE_SET_MODE: return "SET_MODE";
        case MDKR_ONLINE_SET_CONFIG_TRACK: return "SET_CONFIG_TRACK";
        case MDKR_ONLINE_SET_CUP: return "SET_CUP";
    }
    return "?";
}

uint64_t steadyNowMs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

#if MDKR_ENABLE_ONLINE_BETA
/* TEST-ONLY (beta loopback rigs): override the synthetic fixture's BUTTONS for
 * ONE canonical slot over scripted authored-tick windows, so a lane can drive
 * the REAL in-race button paths (a START press = the real pause path) through
 * the sealed canonical input stream. Env (both required; inert otherwise):
 *   MDKR_APP_TEST_ONLINE_SYNTH_PAD_SLOT=<canonical slot>
 *   MDKR_APP_TEST_ONLINE_SYNTH_PAD_SCRIPT="tick[-endtick]:0xNNNN,..."
 * Within a window the fixture's buttons are REPLACED by the scripted value
 * (sticks untouched); later entries override earlier ones, so a window can
 * carve precise edges (e.g. "600-699:0x0,610-699:0x1000" = release everything
 * at 600, press START at 610 and hold it). Because BOTH in-process endpoints
 * compute the identical override for the slot, seal-history convergence -- the
 * transport invariant every loopback lane rests on -- is preserved. */
struct SynthPadScriptEntry {
    uint32_t first;
    uint32_t last;
    uint16_t buttons;
};

const std::vector<SynthPadScriptEntry> &synthPadScript(int *slotOut) {
    static std::vector<SynthPadScriptEntry> entries;
    static int slot = -1;
    static bool parsed = false;
    if (!parsed) {
        parsed = true;
        const char *slotEnv =
            std::getenv("MDKR_APP_TEST_ONLINE_SYNTH_PAD_SLOT");
        const char *script =
            std::getenv("MDKR_APP_TEST_ONLINE_SYNTH_PAD_SCRIPT");
        if (slotEnv != nullptr && script != nullptr) {
            char *end = nullptr;
            const long parsedSlot = std::strtol(slotEnv, &end, 10);
            if (end != slotEnv && parsedSlot >= 0 &&
                parsedSlot < MDKR_SESSION_MAX_PLAYERS) {
                slot = static_cast<int>(parsedSlot);
                const char *cursor = script;
                while (*cursor != '\0') {
                    SynthPadScriptEntry entry{};
                    char *stop = nullptr;
                    entry.first = static_cast<uint32_t>(
                        std::strtoul(cursor, &stop, 10));
                    entry.last = entry.first;
                    if (stop == cursor) break;
                    cursor = stop;
                    if (*cursor == '-') {
                        ++cursor;
                        entry.last = static_cast<uint32_t>(
                            std::strtoul(cursor, &stop, 10));
                        if (stop == cursor) break;
                        cursor = stop;
                    }
                    if (*cursor != ':') break;
                    ++cursor;
                    entry.buttons = static_cast<uint16_t>(
                        std::strtoul(cursor, &stop, 0));
                    if (stop == cursor) break;
                    cursor = stop;
                    entries.push_back(entry);
                    if (*cursor == ',') ++cursor;
                }
                if (!entries.empty()) {
                    std::fprintf(stderr,
                                 "[online-live] TEST: synthetic pad script "
                                 "armed slot=%d windows=%zu\n",
                                 slot, entries.size());
                }
            }
        }
    }
    *slotOut = slot;
    return entries;
}
#endif

/* Deterministic per-slot input for the O-T6 race. Both endpoints compute the
 * SAME sample for a given (canonical slot, authored tick), so the frame this
 * endpoint seals for a future tick equals the frame the peer later drains for
 * that slot, and the committed canonical inputs converge byte-for-byte. Sticks
 * stay in the +/-80 bundle bound; a distinct pattern per slot keeps the race
 * non-trivial (both endpoints drive different inputs). */
MdkrPadSample raceLocalSample(uint8_t canonicalSlot, uint32_t tick) {
    uint32_t h = tick * UINT32_C(2654435761) +
                 static_cast<uint32_t>(canonicalSlot) * UINT32_C(40503) +
                 UINT32_C(0x9e3779b9);
    h ^= h >> 15;
    h *= UINT32_C(2246822519);
    h ^= h >> 13;
    MdkrPadSample s;
    /* Hold ACCELERATE (N64 A == 0x8000) every tick and add only deterministic,
     * race-safe extra bits (triggers + C-buttons == 0x003f). Deliberately never
     * press START (0x1000) or the d-pad (0x0f00): the VISIBLE engine consumes
     * these as real controller input, and a stray START/d-pad opens the pause
     * menu and quits the race after a handful of ticks. Both endpoints derive
     * the identical value from (slot, tick), so canonical convergence -- the
     * only property the transport-level tests assert -- is unchanged. */
    s.buttons = static_cast<uint16_t>(0x8000u | (h & 0x003fu));
    s.stick_x = static_cast<int8_t>(static_cast<int>((h >> 16) % 161u) - 80);
    s.stick_y = static_cast<int8_t>(static_cast<int>((h >> 8) % 161u) - 80);
    s.present = 1u;
#if MDKR_ENABLE_ONLINE_BETA
    /* Test-only scripted button override (see synthPadScript above). The LAST
     * matching window wins, so scripts can layer precise edges. Inert without
     * the env pair. */
    {
        int scriptSlot = -1;
        const std::vector<SynthPadScriptEntry> &script =
            synthPadScript(&scriptSlot);
        if (scriptSlot == static_cast<int>(canonicalSlot)) {
            for (const SynthPadScriptEntry &entry : script) {
                if (tick >= entry.first && tick <= entry.last) {
                    s.buttons = entry.buttons;
                }
            }
        }
    }
#endif
    return s;
}

/* Deterministic, peer-agreed race seed from the shared lobby snapshot. */
uint64_t deriveRngSeed(const MdkrOnlineLobby &lobby) {
    uint64_t h = UINT64_C(1469598103934665603);
    const uint64_t fields[] = {lobby.room_id, lobby.match_epoch,
                               lobby.leader_generation,
                               lobby.leader_endpoint_id};
    for (uint64_t f : fields) {
        for (unsigned b = 0u; b < 8u; ++b) {
            h ^= (f >> (b * 8u)) & 0xffu;
            h *= UINT64_C(1099511628211);
        }
    }
    return h;
}

/* Build the frozen wire manifest from the authoritative Loading snapshot. All
 * peers derive byte-identical fields from the same snapshot. */
bool manifestFromLobby(const MdkrOnlineLobby &lobby,
                       const MdkrOnlineCompatibilityV1 &compat,
                       uint8_t inputDelay, MdkrMatchManifestV1 *out) {
    MdkrMatchManifestV1 m;
    std::memset(&m, 0, sizeof(m));
    unsigned target = 0u;
    for (unsigned i = 0u; i < MDKR_ONLINE_MAX_SEATS; ++i) {
        if (!lobby.seats[i].occupied) continue;
        if (target >= MDKR_MATCH_SLOTS) return false;
        m.slot_owner[target++] = lobby.seats[i].endpoint_id;
    }
    m.match_epoch = lobby.match_epoch;
    m.protocol_version = MDKR_MATCH_MANIFEST_VERSION;
    std::memcpy(m.build_id, compat.build_id, sizeof(m.build_id));
    std::memcpy(m.gameplay_digest, compat.gameplay_digest,
                sizeof(m.gameplay_digest));
    m.slot_count = static_cast<uint8_t>(target);
    m.rng_seed = deriveRngSeed(lobby);
    m.track_id = lobby.selected_track;
    m.rom_revision = compat.rom_revision;
    m.cadence_hz = compat.cadence_hz;
    m.slot_count = lobby.seat_count;
    m.rules = MDKR_MATCH_RULES_STANDARD_RACE;
    m.vehicle_mask = lobby.selected_vehicle_mask;
    m.input_delay = inputDelay;
    if (!mdkr_match_manifest_validate(&m)) return false;
    *out = m;
    return true;
}

}  // namespace

/* LiveAdapter has EXTERNAL linkage (outside the anonymous namespace of helpers
 * above) so match_live_adapter.h can forward-declare it for the mdkrResolveLive()
 * downcast hook -- the header's `::LiveAdapter` and this definition are then the
 * SAME type, which a covariant override and the C accessors both require. Its
 * inline methods still freely use the internal-linkage helpers above (visible at
 * file scope in this translation unit). */
class LiveAdapter final : public IMdkrOnlineAdapter {
public:
    explicit LiveAdapter(const MdkrOnlineLiveAdapterOptions &opts)
        : opts_(opts) {
        mdkr_session_core_init(&session_, opts.sessionId);
        journey_ = opts.journey;
        nowMs_ = opts.nowMs ? opts.nowMs : &steadyNowMs;
        if (const char *t = std::getenv("MDKR_ONLINE_VIEW_TIMEOUT_MS")) {
            char *end = nullptr;
            const unsigned long v = std::strtoul(t, &end, 10);
            if (end != t && *end == '\0' && v >= 1000u) {
                viewTimeoutMs_ = static_cast<uint64_t>(v);
            }
        }
    }

    ~LiveAdapter() override {
        /* The race-boot AND room-ready registry
         * pointers are retracted synchronously on the LAUNCHER THREAD by
         * teardownAdapterAsync (ui_online_room.cpp) BEFORE the adapter is moved
         * to this detached teardown thread -- race-boot via the resolved-raw
         * pointer, room-ready likewise. The
         * live adapter is only ever destroyed via teardownAdapterAsync, so the
         * launcher-thread retract always runs first and nothing can publish a
         * handoff after the owner moves the adapter out. A destructor backstop
         * here would read/write the launcher-owned sPendingEngineRaceBoot /
         * sPendingEngineRoomReady globals from this thread -- a formally-UB
         * racing access of pointers the launcher may already be writing for the
         * NEXT session, contrary to the registries' "launcher-thread only, no
         * lock" contract (online_live_wiring.cpp). It covers no real case now, so
         * it is deliberately DROPPED to keep that contract honest. */
        /* A clean teardown tells the room goodbye. Best-effort and
         * fire-and-forget: the reducer refuses LEAVE outside LOBBY/RESULTS
         * and a closed transport refuses the submit; both are fine. */
        if (haveLobby_ && localEndpointId_ != 0u) {
            (void)sendLobbyCommandRaw(MDKR_ONLINE_LEAVE, 0u, 0u);
        }
        /* The mesh-level goodbye, same discipline: adapter destruction is the
         * one DELIBERATE session end (room leave / new code / app teardown),
         * so the announcing close lets each survivor resolve this endpoint as
         * the immediate typed PeerLost(PeerEnded) instead of its loss ladders.
         * Internal mesh rebuilds (forcePhraseRekey) stay silent. */
        if (mesh_) mesh_->close(/*announcePeerEnd=*/true);
        mesh_.reset(); /* mesh borrows the backend's feed: kill it first */
        if (opts_.meshBackend) opts_.meshBackend->reset();
    }

    /* ---- IMdkrOnlineAdapter ------------------------------------------- */

    /* Cross-cast-free downcast hook (see IMdkrOnlineAdapter): the concrete live
     * adapter resolves to itself, so every C accessor reaches it through the
     * owning wrapper without a sibling cast. */
    LiveAdapter *mdkrResolveLive() override { return this; }
    const LiveAdapter *mdkrResolveLive() const override { return this; }

    MdkrOnlineAdapterStep submit(
        const MdkrOnlineAdapterCommand &command) override {
        if (command.expectedRevision != revision_) {
            return step(false, 1u); /* stale view */
        }
        if (pending_ != Pending::None &&
            command.action != MDKR_ONLINE_VIEW_ACTION_RETRY) {
            return step(false, 2u); /* an async op is in flight */
        }
        if (!actionAllowed(command.action)) {
            return step(false, 3u);
        }
        if (!apply(command)) {
            return step(false, 4u);
        }
        bump();
        /* An accepted step may carry a one-shot advisory note (currently only
         * kMdkrOnlineLiveStepEnterAnotherCode -- the rebuild-me contract the
         * header documents for the panel). */
        const uint32_t note = stepNote_;
        stepNote_ = 0u;
        return step(true, note);
    }

    bool view(MdkrOnlineViewModel *out) const override {
        MdkrOnlineViewInput in;
        MdkrOnlineLobby reVerifyLobby;
        std::memset(&in, 0, sizeof(in));
        in.session = &session_.state;
        if (haveLobby_ && reVerify_) {
            /* Re-verify projection (SAS barrier armed): the race is
             * suspended and the room presents at its lobby/preflight
             * surface so the shared view-model oracle -- which refuses an
             * inconsistent (session, lobby) pair by design -- renders the
             * recovery and fresh-phrase views. The projection is the real
             * snapshot scrubbed to the oracle's LOBBY-phase invariants
             * (no selected track/vehicle-mask, no loaded flags, the
             * session's epoch); the AUTHORITATIVE snapshot in lobby_ is
             * untouched and returns the moment the second confirmation
             * clears the barrier. */
            reVerifyLobby = lobby_;
            reVerifyLobby.phase = MDKR_ONLINE_LOBBY;
            reVerifyLobby.match_epoch = session_.state.match_epoch;
            reVerifyLobby.selected_track = MDKR_ONLINE_NO_VOTE;
            reVerifyLobby.selected_vehicle_mask = 0u;
            for (unsigned i = 0u; i < MDKR_ONLINE_MAX_ENDPOINTS; ++i) {
                reVerifyLobby.members[i].loaded = false;
            }
            in.lobby = &reVerifyLobby;
        } else if (raceEndFailureLatched_ &&
                   failure_ != MDKR_ONLINE_VIEW_FAILURE_NONE) {
            /* A race-end recovery card is latched. Do NOT hand the builder a
             * stale/late lobby snapshot -- the shared view model fails ATOMIC
             * (returns false) when the walked session's phase disagrees with the
             * snapshot's, BEFORE its failure-precedence path runs, so a late
             * State re-latching haveLobby_ would demote the tailored card to the
             * generic "Online Room Unavailable" box. The recovery model builds
             * from the failure alone, so suppress the lobby input persistently
             * until the failure clears (resetRaceLatches / clearRaceLossFailure /
             * a RETURN_HOME leave). */
            in.lobby = nullptr;
        } else {
            in.lobby = haveLobby_ ? &lobby_ : nullptr;
        }
        in.local_endpoint_id = localEndpointId_;
        in.journey = journey_;
        in.failure = failure_;
        in.invite_state =
            inviteReady_ ? MDKR_ONLINE_INVITE_READY : MDKR_ONLINE_INVITE_PREPARING;
        in.verification_phrase =
            (havePhrase_ && !phraseConfirmed_ &&
             session_.state.room == MDKR_ROOM_PREFLIGHT)
                ? phrase_
                : nullptr;
        in.race_admission_enabled = opts_.raceAdmissionEnabled;
        return mdkr_online_view_model_build(&in, out);
    }

    MdkrOnlineJourney journey() const override { return journey_; }

    void service() override {
        pumpRoom();
        ageStaleParked();
        followLobbyPhase();
        pumpMesh();
        runLoadingBarrier();
        runPreflight();
        runRaceLobbyPhase();
        raceServiceWork();
        logPhaseAndTimeoutAnchor();
    }

    uint32_t revision() const override { return revision_; }
    MdkrPlayIntent sessionIntent() const override {
        return session_.state.intent;
    }
    bool raceAdmissionEnabled() const override {
        return opts_.raceAdmissionEnabled;
    }
    /* Non-silent: once the current view surface has not progressed within
     * viewTimeoutMs_, report the timeout so the view model's already-present
     * timeout card ("Room Took Too Long", "Setup Check Took Too Long", "Race Did
     * Not Load", "Selection Took Too Long" -> Return to Lobby / Try Again /
     * Leave Room) surfaces instead of an endless spinner. The anchor resets on
     * every surface change AND on selection progress -- any pick/ready/
     * settings bumps lobby.revision, which re-anchors while SELECTING so the
     * "Selection Took Too Long" card fronts only a genuinely dead room, never
     * active picking. */
    bool timeoutExpired() const override {
        return viewAnchorMs_ != 0u && nowMs_() - viewAnchorMs_ >= viewTimeoutMs_;
    }

    /* ---- Test probe --------------------------------------------------- */
    void fillProbe(MdkrOnlineLiveLaunchProbe *p) const {
        p->descriptorBuilt = descriptorBuilt_;
        p->refusal = refusal_;
        p->installed = installed_;
        p->phraseConfirmed = phraseConfirmed_;
        p->preflightReady = preflightReady_;
        p->descriptor = descriptor_;
    }

private:
    enum class Pending { None, Create, Join };

    /* A fully-recorded sent lobby command: enough to (a) attribute a refusal
     * to the exact command the server answered, (b) re-send THAT command (not
     * merely the most recent one -- with two in flight the old lastType_-only
     * re-send duplicated the newer command and silently dropped the refused
     * one; every redundant accepted SET_CHARACTER/SET_VEHICLE then reset the
     * seat's ready on a live room), and (c) re-EVALUATE against the current
     * lobby whether the command is still needed at all before re-sending. */
    struct SentCommand {
        MdkrOnlineCommandType type = MDKR_ONLINE_JOIN;
        uint32_t seat = 0u;
        uint32_t value = 0u;
        uint32_t expectedRevision = 0u; /* the revision the send carried */
        /* Stale re-sends already burned on THIS command, carried across every
         * park/re-send cycle so the retry cap is a PER-COMMAND bound. A shared
         * counter here was reviewed out: it was reset by ANY fresh send of ANY
         * type, so under a continuous cross-type intent stream a persistently
         * stale-refused command could retry forever. Pinned by the
         * cross-type-traffic unit test in tests/test_online_live_adapter.cpp. */
        unsigned retries = 0u;
    };

    MdkrOnlineAdapterStep step(bool accepted, uint32_t error) {
        MdkrOnlineAdapterStep s;
        s.accepted = accepted;
        s.duplicate = false;
        s.error = error;
        s.revision = revision_;
        s.pendingToken = 0u;
        return s;
    }

    void bump() {
        ++revision_;
        if (revision_ == 0u) revision_ = 1u;
    }

    unsigned readyCount() const {
        if (!haveLobby_) return 0u;
        unsigned n = 0u;
        for (unsigned i = 0u; i < MDKR_ONLINE_MAX_ENDPOINTS; ++i) {
            if (lobby_.members[i].occupied && lobby_.members[i].ready) ++n;
        }
        return n;
    }

    /* Emit one [ROOM-PHASE] line whenever the presented surface changes -- the
     * spine the owner greps to see OPEN -> PREFLIGHT -> SELECTING -> LOADING ->
     * RACING and where it stops -- and re-anchor the non-silent view timeout. */
    void logPhaseAndTimeoutAnchor() {
        const uint64_t key =
            (static_cast<uint64_t>(session_.state.room) << 8) |
            (haveLobby_ ? static_cast<uint64_t>(lobby_.phase) : 0u) |
            (haveLobby_ ? 0u : UINT64_C(0x10000)) |
            (failure_ != MDKR_ONLINE_VIEW_FAILURE_NONE ? UINT64_C(0x20000) : 0u) |
            (reVerify_ ? UINT64_C(0x40000) : 0u);
        const bool selecting = haveLobby_ &&
            session_.state.room == MDKR_ROOM_SELECTING &&
            lobby_.phase == MDKR_ONLINE_LOBBY;
        if (key == lastPhaseKey_) {
            /* The phase key is constant across an entire selection screen, so
             * without this a human taking >30 s to pick would permanently front
             * the "Selection Took Too Long" card even while picks are actively
             * landing. Selection progress bumps lobby.revision (any pick / ready
             * / settings change), so re-anchor the timeout on it: only a room
             * with NO revision for the full window (a genuinely stuck peer) still
             * cards. Other surfaces keep the phase-key-only anchor -- their
             * timeouts are true "no transition" spinners. */
            if (selecting && lobby_.revision != lastAnchorRevision_) {
                lastAnchorRevision_ = lobby_.revision;
                viewAnchorMs_ = nowMs_();
            }
            return;
        }
        lastPhaseKey_ = key;
        lastAnchorRevision_ = haveLobby_ ? lobby_.revision : 0u;
        viewAnchorMs_ = nowMs_();
        MDKR_ONLINE_LOG(
            "[ROOM-PHASE] role=%s room=%s lobbyPhase=%s rev=%u epoch=%u "
            "members=%u ready=%u track=%u vmask=0x%02x failure=%u reverify=%u\n",
            journey_ == MDKR_ONLINE_JOURNEY_JOIN ? "join" : "create",
            roomPhaseName(session_.state.room),
            haveLobby_ ? lobbyPhaseName(lobby_.phase) : "(none)",
            haveLobby_ ? lobby_.revision : 0u,
            haveLobby_ ? lobby_.match_epoch : 0u,
            haveLobby_ ? lobby_.member_count : 0u, readyCount(),
            haveLobby_ ? lobby_.selected_track : 0u,
            haveLobby_ ? lobby_.selected_vehicle_mask : 0u,
            static_cast<unsigned>(failure_), reVerify_ ? 1u : 0u);
    }

    bool sessionDispatch(MdkrSessionCommandType type, uint32_t value) {
        MdkrSessionCommand c;
        std::memset(&c, 0, sizeof(c));
        c.protocol_version = MDKR_SESSION_PROTOCOL_VERSION;
        c.expected_generation = session_.state.generation;
        c.type = type;
        c.value = value;
        return mdkr_session_core_dispatch(&session_, &c).accepted;
    }

    bool sendLobbyCommand(MdkrOnlineCommandType type, uint32_t seat,
                          uint32_t value) {
        lastType_ = type;
        lastSeat_ = seat;
        lastValue_ = value;
        haveLast_ = true;
        /* A FRESH command supersedes any parked stale re-send of the same
         * type: the caller's newest intent wins, and re-sending the stale
         * older value AFTER it would silently revert the newer one. */
        for (auto it = staleParked_.begin(); it != staleParked_.end();) {
            if (it->type == type) {
                it = staleParked_.erase(it);
            } else {
                ++it;
            }
        }
        return sendLobbyCommandRaw(type, seat, value);
    }

    bool sendLobbyCommandRaw(MdkrOnlineCommandType type, uint32_t seat,
                             uint32_t value, unsigned retries = 0u) {
        MdkrOnlineCommand c;
        std::memset(&c, 0, sizeof(c));
        c.protocol_version = MDKR_ONLINE_PROTOCOL_VERSION;
        c.expected_revision = haveLobby_ ? lobby_.revision : 0u;
        c.command_id = nextCommandId_++;
        c.actor_endpoint_id = localEndpointId_;
        c.type = type;
        c.target_endpoint_id = seat;
        c.value = value;
        c.compatibility = opts_.compatibility;
        /* Correlation: remember what each command_id carried, so the
         * CommandResult drain can attribute a refusal to the command the
         * server answered even with two in flight -- and re-send / re-evaluate
         * THAT command, never merely the most recent one. Bounded (a server
         * that echoes no id never lets us consume entries): drop the oldest
         * once the window is comfortably past any realistic in-flight depth. */
        SentCommand sent;
        sent.type = type;
        sent.seat = seat;
        sent.value = value;
        sent.expectedRevision = c.expected_revision;
        sent.retries = retries; /* re-sends inherit + advance their count */
        inFlight_[c.command_id] = sent;
        if (inFlight_.size() > 64u) inFlight_.erase(inFlight_.begin());
        return opts_.room && opts_.room->submitCommand(c);
    }

    /* Does the CURRENT authoritative lobby already reflect `cmd`'s effect?
     * The stale-refusal path uses this to drop a re-send whose outcome has
     * already landed (typically via the concurrent peer's interleaving):
     * re-applying it anyway is never free -- an accepted redundant
     * SET_CHARACTER / SET_VEHICLE / SET_VOTE resets the seat's ready
     * (lobby_core.c), which is exactly how the retry churn kept knocking a
     * latched READY back down on a real two-peer room. Conservative: an
     * unknown type reports false (keep the retry). */
    bool commandEffectApplied(const SentCommand &cmd) const {
        if (!haveLobby_) return false;
        const MdkrOnlineMember *self = nullptr;
        for (unsigned i = 0u; i < MDKR_ONLINE_MAX_ENDPOINTS; ++i) {
            if (lobby_.members[i].occupied &&
                lobby_.members[i].endpoint_id == localEndpointId_) {
                self = &lobby_.members[i];
                break;
            }
        }
        switch (cmd.type) {
        case MDKR_ONLINE_SET_CHARACTER:
        case MDKR_ONLINE_SET_VEHICLE:
        case MDKR_ONLINE_SET_VOTE: {
            if (cmd.seat >= MDKR_ONLINE_MAX_SEATS) return false;
            const MdkrOnlineSeat &s = lobby_.seats[cmd.seat];
            if (!s.occupied || s.endpoint_id != localEndpointId_) return false;
            if (cmd.type == MDKR_ONLINE_SET_CHARACTER) {
                return s.character_id == static_cast<uint8_t>(cmd.value);
            }
            if (cmd.type == MDKR_ONLINE_SET_VEHICLE) {
                return s.vehicle_id == static_cast<uint8_t>(cmd.value);
            }
            return s.vote_track == static_cast<uint16_t>(cmd.value);
        }
        case MDKR_ONLINE_SET_READY:
            return self != nullptr && self->ready == (cmd.value != 0u);
        case MDKR_ONLINE_ACK_LOADED:
            return self != nullptr && self->loaded;
        case MDKR_ONLINE_SET_MODE:
            return lobby_.mode == static_cast<uint8_t>(cmd.value);
        case MDKR_ONLINE_SET_CONFIG_TRACK:
            return lobby_.configured_track == static_cast<uint16_t>(cmd.value);
        case MDKR_ONLINE_SET_CUP:
            return lobby_.cup_id == static_cast<uint8_t>(cmd.value);
        case MDKR_ONLINE_BEGIN_LOADING:
            return lobby_.phase != MDKR_ONLINE_LOBBY;
        case MDKR_ONLINE_CANCEL_LOADING:
            return lobby_.phase == MDKR_ONLINE_LOBBY;
        case MDKR_ONLINE_BEGIN_RACE:
            return lobby_.phase != MDKR_ONLINE_LOADING;
        case MDKR_ONLINE_PUBLISH_RESULTS:
            return lobby_.phase == MDKR_ONLINE_RESULTS;
        case MDKR_ONLINE_REMATCH:
            return lobby_.phase != MDKR_ONLINE_RESULTS;
        default:
            return false;
        }
    }

    /* Launcher-owned optimistic-concurrency recovery for a STALE_REVISION /
     * STALE_COMMAND refusal of `cmd` (the ATTRIBUTED command, by echoed id).
     * Order of business:
     *   1. already applied?           -> drop (nothing to re-send);
     *   2. our lobby view has moved
     *      past the refused revision
     *      (or only the command id
     *      was stale)                 -> re-send NOW against the fresh view;
     *   3. our view IS the refused
     *      revision                   -> PARK until the next State resyncs
     *                                    (an immediate re-send would carry the
     *                                    same stale revision -- guaranteed
     *                                    refusal, the old retry churn).
     * Bounded PER COMMAND (SentCommand::retries, kStaleRetryCap): exhaustion
     * surfaces the refusal (one-shot) so the reverse-feed planner re-drives.
     * The count rides the command through every park/re-send cycle, so
     * unrelated fresh traffic can never reset it (the cross-type-traffic
     * unit pin). */
    void handleStaleRefusal(const SentCommand &cmd, uint32_t error) {
        if (commandEffectApplied(cmd)) {
            MDKR_ONLINE_LOG(
                "[ONLINE] command stale type=%s -- effect already in lobby "
                "rev=%u (dropped, no re-send)\n",
                lobbyCommandName(cmd.type), haveLobby_ ? lobby_.revision : 0u);
            return;
        }
        if (cmd.retries >= kStaleRetryCap) {
            surfaceStaleExhaustion(cmd, error);
            return;
        }
        if (!haveLobby_ || lobby_.revision != cmd.expectedRevision ||
            error == MDKR_ONLINE_ERROR_STALE_COMMAND) {
            MDKR_ONLINE_LOG(
                "[ONLINE] command stale type=%s error=%s retry=%u "
                "(re-sending against fresh revision %u)\n",
                lobbyCommandName(cmd.type),
                lobbyErrorName(static_cast<MdkrOnlineError>(error)),
                cmd.retries + 1u, haveLobby_ ? lobby_.revision : 0u);
            (void)sendLobbyCommandRaw(cmd.type, cmd.seat, cmd.value,
                                      cmd.retries + 1u);
            return;
        }
        /* Park (latest per type wins; the entry KEEPS its retry count). The
         * age counter is deliberately NOT reset here: it measures how long
         * ANY entry has sat parked without the queue emptying, so a
         * park/re-park cycle cannot hold the age-out off forever. */
        for (auto it = staleParked_.begin(); it != staleParked_.end();) {
            if (it->type == cmd.type) {
                it = staleParked_.erase(it);
            } else {
                ++it;
            }
        }
        staleParked_.push_back(cmd);
        MDKR_ONLINE_LOG(
            "[ONLINE] command stale type=%s error=%s at rev=%u -- parked "
            "until the next authoritative state (no blind re-send)\n",
            lobbyCommandName(cmd.type),
            lobbyErrorName(static_cast<MdkrOnlineError>(error)),
            cmd.expectedRevision);
    }

    /* A command burned its per-command stale-retry budget: surface the
     * refusal (one-shot) so the reverse-feed planner's guard clears and it
     * re-drives from the current intent. */
    void surfaceStaleExhaustion(const SentCommand &cmd, uint32_t error) {
        MDKR_ONLINE_LOG(
            "[ONLINE] command REJECTED type=%s error=%s "
            "(stale retries exhausted at %u)\n",
            lobbyCommandName(cmd.type),
            lobbyErrorName(static_cast<MdkrOnlineError>(error)), cmd.retries);
        refusalType_ = static_cast<uint32_t>(cmd.type);
        refusalError_ = error;
        haveRefusal_ = true;
        bump();
    }

    /* Belt-and-braces bound: a parked stale re-send waits for a State that --
     * on a genuinely dead room -- may never come. After ~10 s of service()
     * calls with entries still parked, surface the head as a refusal (the
     * reverse-feed planner's guard clears and it re-drives from the intent)
     * and drop the rest; never a silent forever-park. */
    void ageStaleParked() {
        if (staleParked_.empty()) {
            staleParkedAgeServices_ = 0u;
            return;
        }
        if (++staleParkedAgeServices_ < kStaleParkedMaxServices) return;
        const SentCommand head = staleParked_.front();
        MDKR_ONLINE_LOG(
            "[ONLINE] parked stale type=%s TIMED OUT waiting for a state "
            "resync -- surfacing refusal (dropping %u parked)\n",
            lobbyCommandName(head.type),
            static_cast<unsigned>(staleParked_.size()));
        staleParked_.clear();
        staleParkedAgeServices_ = 0u;
        refusalType_ = static_cast<uint32_t>(head.type);
        refusalError_ = MDKR_ONLINE_ERROR_STALE_REVISION;
        haveRefusal_ = true;
        bump();
    }

    /* A fresh authoritative State landed: re-evaluate the parked stale
     * re-sends against it. Drop everything already applied; re-send AT MOST
     * ONE (the head) -- its accept broadcasts the next State, which drains the
     * next entry, so a chain replays in original order with exactly one
     * speculative expected_revision outstanding at a time. */
    void drainStaleParked() {
        while (!staleParked_.empty()) {
            const SentCommand cmd = staleParked_.front();
            if (commandEffectApplied(cmd)) {
                staleParked_.pop_front();
                MDKR_ONLINE_LOG(
                    "[ONLINE] parked stale type=%s resolved by state rev=%u "
                    "(dropped, no re-send)\n",
                    lobbyCommandName(cmd.type), lobby_.revision);
                continue;
            }
            staleParked_.pop_front();
            if (cmd.retries >= kStaleRetryCap) {
                /* Its budget is gone: surface instead of burning another
                 * round trip; keep draining the rest against this state. */
                surfaceStaleExhaustion(cmd, MDKR_ONLINE_ERROR_STALE_REVISION);
                continue;
            }
            MDKR_ONLINE_LOG(
                "[ONLINE] parked stale type=%s re-sent against fresh state "
                "rev=%u (retry=%u)\n",
                lobbyCommandName(cmd.type), lobby_.revision, cmd.retries + 1u);
            (void)sendLobbyCommandRaw(cmd.type, cmd.seat, cmd.value,
                                      cmd.retries + 1u);
            break;
        }
        /* The age counter is NOT reset here: entries still parked behind the
         * head keep aging toward the age-out, and a head that re-parks (the
         * semi-live-room cycle) must not restart the clock -- ageStaleParked
         * resets it only once the queue is genuinely empty. Both bounds are
         * pinned by unit tests (dead-room age-out; cross-type termination). */
    }

    /* The launcher panel addresses a LOCAL seat index (0-based within this
     * endpoint, exactly like the fake adapter). Map it to the canonical seat
     * index the room reducer/manifest use; -1 if this endpoint owns no such
     * seat. */
    int canonicalSeat(uint32_t localIndex) const {
        if (!haveLobby_) return -1;
        for (unsigned i = 0u; i < MDKR_ONLINE_MAX_SEATS; ++i) {
            const MdkrOnlineSeat &s = lobby_.seats[i];
            if (s.occupied && s.endpoint_id == localEndpointId_ &&
                s.local_index == localIndex) {
                return static_cast<int>(i);
            }
        }
        return -1;
    }

    bool actionAllowed(MdkrOnlineViewAction action) const {
        /* Re-picking a racer, vehicle or track vote is legal any time the room
         * is back in its selection phase, even when the view model's primary
         * control has already moved on to Ready/Start (the reducer accepts a
         * re-pick and clears ready). Without this the pickers are one-shot:
         * "Change Selection" could never actually change anything.
         *
         * READY / CHANGE_SELECTION (SET_READY 1/0) are reducer-authoritative in
         * the LOBBY phase too. The view model's SELECTING surface gates its
         * Ready button behind a TRACK VOTE for an unconfigured single-race room
         * -- a step that exists only for the legacy ImGui picker flow. The
         * native descriptor-less flow never casts a vote (its TRACKSELECT
         * screen SET_CONFIG_TRACKs later), so on a FRESH production room the
         * reverse-feed READY was refused HERE, silently, forever: the seat
         * never readied and both real peers wedged at CHARSELECT. The reducer
         * itself requires only member_selection_complete (character + vehicle)
         * for SET_READY 1 -- exactly the gate that should decide -- and any
         * genuinely early READY comes back as a surfaced NOT_READY refusal. */
        if ((action == MDKR_ONLINE_VIEW_ACTION_CHOOSE_CHARACTER ||
             action == MDKR_ONLINE_VIEW_ACTION_CHOOSE_VEHICLE ||
             action == MDKR_ONLINE_VIEW_ACTION_VOTE_TRACK ||
             action == MDKR_ONLINE_VIEW_ACTION_READY ||
             action == MDKR_ONLINE_VIEW_ACTION_CHANGE_SELECTION) &&
            haveLobby_ && lobby_.phase == MDKR_ONLINE_LOBBY) {
            return true;
        }
        MdkrOnlineViewModel m;
        if (!view(&m)) return false;
        return (m.primary.visible && m.primary.enabled &&
                m.primary.action == action) ||
               (m.secondary.visible && m.secondary.enabled &&
                m.secondary.action == action) ||
               (m.cancel.visible && m.cancel.enabled &&
                m.cancel.action == action);
    }

    bool apply(MdkrOnlineViewAction action, uint32_t seat, uint32_t value) {
        switch (action) {
            case MDKR_ONLINE_VIEW_ACTION_CREATE_ROOM:
                if (!sessionDispatch(MDKR_SESSION_COMMAND_BEGIN_ONLINE, 0u))
                    return false;
                journey_ = MDKR_ONLINE_JOURNEY_CREATE;
                pending_ = Pending::Create;
                return opts_.room &&
                       opts_.room->beginCreate(opts_.compatibility,
                                               opts_.localSeatCount);
            case MDKR_ONLINE_VIEW_ACTION_JOIN_ROOM:
                if (!sessionDispatch(MDKR_SESSION_COMMAND_BEGIN_ONLINE, 0u))
                    return false;
                journey_ = MDKR_ONLINE_JOURNEY_JOIN;
                pending_ = Pending::Join;
                if (!opts_.room) return false;
                if (!opts_.joinCode.empty()) {
                    return opts_.room->beginJoinByCode(
                        opts_.joinCode, opts_.compatibility,
                        opts_.localSeatCount);
                }
                return opts_.room->beginJoin(opts_.joinCapability,
                                             opts_.compatibility,
                                             opts_.localSeatCount);
            case MDKR_ONLINE_VIEW_ACTION_SHARE_INVITE:
                /* The invite is already shareable from the Ready payload; the
                 * real peer joins out-of-band via its own transport. */
                return journey_ == MDKR_ONLINE_JOURNEY_CREATE && haveLobby_;
            case MDKR_ONLINE_VIEW_ACTION_CHECK_SETUP:
                if (!sessionDispatch(MDKR_SESSION_COMMAND_SET_ROOM_PHASE,
                                     MDKR_ROOM_PREFLIGHT)) return false;
                bringUpMesh();
                return true;
            case MDKR_ONLINE_VIEW_ACTION_CONFIRM_PHRASE: {
                if (!havePhrase_ ||
                    session_.state.room != MDKR_ROOM_PREFLIGHT) return false;
                const bool resync = reVerify_;
                if (!sessionDispatch(MDKR_SESSION_COMMAND_SET_ROOM_PHASE,
                                     MDKR_ROOM_SELECTING)) return false;
                phraseConfirmed_ = true;
                reVerify_ = false; /* the fresh SAS has been compared */
                /* Pin the exact transcript the human just compared: any
                 * later digest change re-arms the barrier. */
                haveConfirmedDigest_ =
                    mesh_ && mesh_->transcriptDigest(confirmedDigest_);
                /* After a RE-verify confirm, return the room to the
                 * authoritative lobby phase (the barrier had parked it at
                 * the preflight surface mid-LOADING). */
                if (resync) followLobbyPhase();
                return true;
            }
            case MDKR_ONLINE_VIEW_ACTION_REPORT_PHRASE_MISMATCH:
                /* The mismatch RETIRES the compared keys instead of
                 * just latching a failure the RETRY would clear back onto the
                 * SAME phrase. Tell every peer over the sealed control
                 * channel (so their "confirm the phrase" surface leaves too),
                 * then -- after a couple of service() pumps so the reliable
                 * notice flushes -- tear the mesh session down and bring it
                 * back up on fresh ephemeral keys: the re-derived transcript
                 * surfaces a NEW phrase after "Reconnect Securely". */
                failure_ = MDKR_ONLINE_VIEW_FAILURE_VERIFICATION_MISMATCH;
                phraseMismatchActive_ = true;
                if (sendPhraseMismatchNotice()) {
                    phraseRekeyCountdown_ = kPhraseRekeyDelayServices;
                } else {
                    /* The phrase can be ready before the reliable control
                     * channel opens (hellos ride signaling); keep retrying
                     * the notice from service() -- bounded -- before tearing
                     * the mesh down, so the peer is TOLD, not just cut off. */
                    phraseNoticePending_ = true;
                    phraseNoticeTries_ = 0u;
                }
                return true;
            case MDKR_ONLINE_VIEW_ACTION_CHOOSE_CHARACTER: {
                const int cs = canonicalSeat(seat);
                return cs >= 0 && sendLobbyCommand(MDKR_ONLINE_SET_CHARACTER,
                                                   (uint32_t)cs, value);
            }
            case MDKR_ONLINE_VIEW_ACTION_CHOOSE_VEHICLE: {
                const int cs = canonicalSeat(seat);
                return cs >= 0 && sendLobbyCommand(MDKR_ONLINE_SET_VEHICLE,
                                                   (uint32_t)cs, value);
            }
            case MDKR_ONLINE_VIEW_ACTION_VOTE_TRACK: {
                const int cs = canonicalSeat(seat);
                return cs >= 0 && sendLobbyCommand(MDKR_ONLINE_SET_VOTE,
                                                   (uint32_t)cs, value);
            }
            case MDKR_ONLINE_VIEW_ACTION_READY:
                return sendLobbyCommand(MDKR_ONLINE_SET_READY, 0u, 1u);
            case MDKR_ONLINE_VIEW_ACTION_CHANGE_SELECTION:
                return sendLobbyCommand(MDKR_ONLINE_SET_READY, 0u, 0u);
            case MDKR_ONLINE_VIEW_ACTION_START_RACE: {
                /* Leader begins loading; both peers follow lobby.phase LOADING
                 * (followLobbyPhase) into the Loading barrier. The value is the usable
                 * vehicle mask for the voted track (see the panel note); it must
                 * equal leveltable_vehicle_usable(track) or the engine admission
                 * rejects the boot. */
                const bool sent =
                    sendLobbyCommand(MDKR_ONLINE_BEGIN_LOADING, 0u, value);
                MDKR_ONLINE_LOG(
                    "[START] START_RACE -> BEGIN_LOADING vehicleMask=0x%02x "
                    "leader=%u sentToCloud=%u rev=%u\n",
                    value,
                    (haveLobby_ &&
                     lobby_.leader_endpoint_id == localEndpointId_) ? 1u : 0u,
                    sent ? 1u : 0u, haveLobby_ ? lobby_.revision : 0u);
                return sent;
            }
            case MDKR_ONLINE_VIEW_ACTION_RETURN_TO_LOBBY:
                /* Never move the local session ahead of the room reducer.
                 * CANCEL_LOADING is leader-only: only the LEADER sends the
                 * cancel and NOBODY transitions locally, because a joiner that
                 * yanked its own session to SELECTING while the lobby stayed
                 * LOADING would create the (session, lobby) mismatch that bricks
                 * the view ("Online Room Unavailable"). The accepted cancel's
                 * LOBBY-phase snapshot walks every endpoint's session back
                 * through followLobbyPhase(). */
                journey_ = MDKR_ONLINE_JOURNEY_REMATCH;
                if (haveLobby_ && lobby_.phase == MDKR_ONLINE_LOADING &&
                    lobby_.leader_endpoint_id == localEndpointId_) {
                    return sendLobbyCommand(MDKR_ONLINE_CANCEL_LOADING, 0u, 0u);
                }
                return true;
            case MDKR_ONLINE_VIEW_ACTION_RACE_AGAIN:
            case MDKR_ONLINE_VIEW_ACTION_CHANGE_TRACK:
                /* REMATCH is leader-only in the reducer; the shared
                 * results view offers Race Again to everyone, so a joiner's
                 * dispatch is a clean refusal here rather than a doomed
                 * command. CHANGE_TRACK maps onto the same REMATCH the panel
                 * already routes it to: the lobby returns to selections where
                 * the track is re-chosen. The session walks back to the lobby
                 * scene from the REMATCH snapshot (followLobbyPhase), never
                 * locally. */
                if (!haveLobby_ || lobby_.phase != MDKR_ONLINE_RESULTS ||
                    lobby_.leader_endpoint_id != localEndpointId_) {
                    return false;
                }
                journey_ = MDKR_ONLINE_JOURNEY_REMATCH;
                return sendLobbyCommand(MDKR_ONLINE_REMATCH, 0u, 0u);
            case MDKR_ONLINE_VIEW_ACTION_ENTER_ANOTHER_CODE:
                /* The live adapter's journey/join-code are fixed at
                 * construction and the room transport begins exactly once, so
                 * re-joining in place is impossible. Leave cleanly, park the
                 * session at HOME, and hand the panel the documented rebuild
                 * sentinel (kMdkrOnlineLiveStepEnterAnotherCode) so it
                 * destroys this adapter and constructs a fresh JOIN adapter
                 * with the newly entered code. */
                if (haveLobby_ && localEndpointId_ != 0u) {
                    (void)sendLobbyCommandRaw(MDKR_ONLINE_LEAVE, 0u, 0u);
                }
                if (!sessionDispatch(MDKR_SESSION_COMMAND_CANCEL, 0u))
                    return false;
                haveLobby_ = false;
                inviteReady_ = false;
                pending_ = Pending::None;
                failure_ = MDKR_ONLINE_VIEW_FAILURE_NONE;
                stepNote_ = kMdkrOnlineLiveStepEnterAnotherCode;
                return true;
            case MDKR_ONLINE_VIEW_ACTION_RETRY:
                if (failure_ == MDKR_ONLINE_VIEW_FAILURE_NONE) return false;
                failure_ = MDKR_ONLINE_VIEW_FAILURE_NONE;
                return true;
            case MDKR_ONLINE_VIEW_ACTION_LEAVE_ROOM:
            case MDKR_ONLINE_VIEW_ACTION_PLAY_HERE:
            case MDKR_ONLINE_VIEW_ACTION_CHOOSE_ROM:
            case MDKR_ONLINE_VIEW_ACTION_RETURN_HOME:
                /* A clean local leave/abandon tells the room goodbye
                 * (best-effort; the reducer refuses it outside LOBBY/RESULTS
                 * and nothing depends on acceptance). */
                if (action == MDKR_ONLINE_VIEW_ACTION_LEAVE_ROOM &&
                    haveLobby_ && localEndpointId_ != 0u) {
                    (void)sendLobbyCommandRaw(MDKR_ONLINE_LEAVE, 0u, 0u);
                }
                if (!sessionDispatch(
                        action == MDKR_ONLINE_VIEW_ACTION_LEAVE_ROOM
                            ? MDKR_SESSION_COMMAND_CANCEL
                            : MDKR_SESSION_COMMAND_RETURN_HOME,
                        0u)) return false;
                haveLobby_ = false;
                inviteReady_ = false;
                failure_ = MDKR_ONLINE_VIEW_FAILURE_NONE;
                return true;
            case MDKR_ONLINE_VIEW_ACTION_LEAVE_RACE:
                if (!sessionDispatch(MDKR_SESSION_COMMAND_LEAVE_ONLINE_RACE, 0u))
                    return false;
                haveLobby_ = false;
                failure_ = MDKR_ONLINE_VIEW_FAILURE_NONE;
                return true;
            default:
                return false;
        }
    }

    bool apply(const MdkrOnlineAdapterCommand &c) {
        return apply(c.action, c.seat, c.value);
    }

    /* ---- Room transport ---------------------------------------------- */

    void pumpRoom() {
        if (!opts_.room) return;
        opts_.room->pump(roomEvents_);
        for (const MdkrOnlineRoomEvent &ev : roomEvents_) {
            switch (ev.type) {
                case MdkrOnlineRoomEvent::Type::Ready:
                    localEndpointId_ = ev.localEndpointId;
                    lobby_ = ev.lobby;
                    haveLobby_ = ev.haveLobby;
                    iceServers_ = ev.iceServers;
                    roomIdStr_ = ev.roomId;
                    credential_ = ev.credential;
                    (void)sessionDispatch(MDKR_SESSION_COMMAND_SET_CONNECTIVITY,
                                          MDKR_CONNECTIVITY_DIRECT);
                    (void)sessionDispatch(MDKR_SESSION_COMMAND_SET_ROOM_PHASE,
                                          MDKR_ROOM_OPEN);
                    inviteReady_ = true;
                    pending_ = Pending::None;
                    /* The service issues the JOIN itself with command id 1, so
                     * a joiner's own commands must start after it; a creator's
                     * seat needs no join command. */
                    nextCommandId_ =
                        journey_ == MDKR_ONLINE_JOURNEY_JOIN ? 2u : 1u;
                    MDKR_ONLINE_LOG(
                        "[ONLINE] room ready role=%s localEp=%llu members=%u\n",
                        journey_ == MDKR_ONLINE_JOURNEY_JOIN ? "join" : "create",
                        (unsigned long long)localEndpointId_,
                        haveLobby_ ? lobby_.member_count : 0u);
                    bump();
                    break;
                case MdkrOnlineRoomEvent::Type::State:
                    if (ev.haveLobby) {
                        lobby_ = ev.lobby;
                        haveLobby_ = true;
                        MDKR_ONLINE_LOG(
                            "[ONLINE] room state rev=%u lobbyPhase=%s members=%u "
                            "ready=%u track=%u vmask=0x%02x epoch=%u\n",
                            lobby_.revision, lobbyPhaseName(lobby_.phase),
                            lobby_.member_count, readyCount(),
                            lobby_.selected_track, lobby_.selected_vehicle_mask,
                            lobby_.match_epoch);
                        followLobbyPhase();
                        bump();
                        /* The resync the parked stale re-sends were waiting
                         * for: re-evaluate them against THIS revision. */
                        drainStaleParked();
                    }
                    break;
                case MdkrOnlineRoomEvent::Type::CommandResult: {
                    /* Attribute this result to the command it actually answers.
                     * Two commands can be in flight, so keying on the most
                     * recently SENT type (lastType_) misattributes the first's
                     * refusal to the second. Prefer the server-echoed command_id
                     * (see MdkrOnlineRoomEvent::commandId); fall back to lastType_
                     * only when the server echoed no id. Consume the map entry so
                     * it cannot grow or be matched twice. */
                    SentCommand attrCmd;
                    attrCmd.type = lastType_;
                    attrCmd.seat = lastSeat_;
                    attrCmd.value = lastValue_;
                    /* Fallback (no echoed id): the expected revision the send
                     * carried is unknown -- an impossible sentinel keeps the
                     * stale path on its immediate-re-send arm, and the
                     * consecutive-no-echo counter stands in for the
                     * per-command budget (a transport that never echoes ids
                     * must still terminate at the cap). */
                    attrCmd.expectedRevision = UINT32_MAX;
                    attrCmd.retries = noEchoStaleRetries_;
                    MdkrOnlineCommandType attrType = lastType_;
                    bool attrValid = haveLast_;
                    bool attrFromMap = false;
                    if (ev.commandId != 0u) {
                        const auto found = inFlight_.find(ev.commandId);
                        if (found != inFlight_.end()) {
                            attrCmd = found->second;
                            attrType = attrCmd.type;
                            attrValid = true;
                            attrFromMap = true;
                            inFlight_.erase(found);
                        }
                    }
                    /* The BEGIN_LOADING result is the make-or-break Start Race
                     * signal: whether the cloud MatchRoom accepted the leader's
                     * start, and with what error if not. Always logged. */
                    if (attrValid && attrType == MDKR_ONLINE_BEGIN_LOADING) {
                        MDKR_ONLINE_LOG(
                            "[START] BEGIN_LOADING result accepted=%u error=%s "
                            "rev=%u\n",
                            ev.step.accepted ? 1u : 0u,
                            lobbyErrorName(ev.step.error), ev.step.revision);
                    }
                    if (ev.step.accepted) {
                        /* Per-command retry budgets retire with their
                         * inFlight_ entries; only the no-echo fallback's
                         * consecutive counter needs the reset. */
                        noEchoStaleRetries_ = 0u;
                    } else if (ev.step.error == MDKR_ONLINE_ERROR_INCOMPATIBLE) {
                        MDKR_ONLINE_LOG(
                            "[ONLINE] command REJECTED type=%s error=%s "
                            "-> DIFFERENT_BUILD\n",
                            attrValid ? lobbyCommandName(attrType) : "?",
                            lobbyErrorName(ev.step.error));
                        failure_ = MDKR_ONLINE_VIEW_FAILURE_DIFFERENT_BUILD;
                        bump();
                    } else if ((ev.step.error == MDKR_ONLINE_ERROR_STALE_REVISION ||
                                ev.step.error == MDKR_ONLINE_ERROR_STALE_COMMAND) &&
                               attrValid) {
                        /* Launcher-owned optimistic-concurrency recovery: the
                         * room advanced under us (a concurrent peer). Recover
                         * the ATTRIBUTED command -- drop it when its effect
                         * already landed, re-send when our view has already
                         * resynced past the refused revision, otherwise park
                         * it for the next State (a blind immediate re-send
                         * would carry the SAME stale revision: over the real
                         * transport the CommandResult routinely arrives
                         * BEFORE the State that explains the staleness). */
                        if (!attrFromMap) ++noEchoStaleRetries_;
                        handleStaleRefusal(attrCmd, ev.step.error);
                    } else {
                        /* Non-silent: any other refusal (e.g. NOT_READY,
                         * ILLEGAL_VEHICLE, exhausted stale retries) is logged so a
                         * refused command never vanishes without a trace. */
                        MDKR_ONLINE_LOG(
                            "[ONLINE] command REJECTED type=%s error=%s "
                            "(no auto-recovery)\n",
                            attrValid ? lobbyCommandName(attrType) : "?",
                            lobbyErrorName(ev.step.error));
                        /* Surface the refusal to the panel (one-shot): the
                         * command was accepted locally but refused by the room
                         * (e.g. SELECTION_CONFLICT when both players tap the
                         * same racer), so the UI must un-stage its optimistic
                         * pick and show why. Attributed to the command the server
                         * answered, not merely the last one sent. */
                        if (attrValid) {
                            refusalType_ =
                                static_cast<uint32_t>(attrType);
                            refusalError_ = ev.step.error;
                            haveRefusal_ = true;
                            bump();
                        }
                    }
                    break;
                }
                case MdkrOnlineRoomEvent::Type::Failure:
                    /* Pre-mapped stable failure; lobby state is retained so a
                     * Retry recovers without dismissing the room. */
                    failure_ = ev.failure != MDKR_ONLINE_VIEW_FAILURE_NONE
                                   ? ev.failure
                                   : MDKR_ONLINE_VIEW_FAILURE_SERVICE_UNAVAILABLE;
                    MDKR_ONLINE_LOG("[ONLINE] room failure=%u\n",
                                    static_cast<unsigned>(failure_));
                    pending_ = Pending::None;
                    bump();
                    break;
            }
        }
    }

    /* Follow the authoritative lobby phase for phases the room/leader drives:
     * the full LOBBY -> LOADING -> RACING -> RESULTS -> LOBBY loop, not just the
     * first LOADING. Local sub-phases (PREFLIGHT/SELECTING while lobby is LOBBY)
     * are never overwritten here. While the SAS re-verify barrier is armed the
     * room presentation stays at the re-verify surface -- a lobby snapshot must
     * not yank the phase back and hide the fresh phrase; the authoritative phase
     * re-syncs after the second confirmation. Bounded multi-step: one snapshot
     * may require a short walk (e.g. RESULTS observed while still RACING ->
     * engine FINISHED -> results scene), and a skipped intermediate snapshot (the
     * transport delivers latest-state, not every revision) may require the
     * return-to-lobby leg before the next loading leg. */
    void followLobbyPhase() {
        if (!haveLobby_ || reVerify_) return;
        for (unsigned guard = 0u; guard < 6u; ++guard) {
            if (!stepLobbyPhase()) break;
        }
    }

    bool isLeader() const {
        return haveLobby_ && lobby_.leader_endpoint_id == localEndpointId_;
    }

    /* One session transition toward the authoritative lobby phase; true when
     * progress was made (the caller loops, bounded). */
    bool stepLobbyPhase() {
        const MdkrRoomPhase room = session_.state.room;
        const MdkrEnginePhase engine = session_.state.engine;
        const bool sessionInRacePhase =
            room == MDKR_ROOM_LOADING || room == MDKR_ROOM_COUNTDOWN ||
            room == MDKR_ROOM_RACING || room == MDKR_ROOM_RESULTS;
        const bool sessionInLobbyScene =
            room == MDKR_ROOM_OPEN || room == MDKR_ROOM_PREFLIGHT ||
            room == MDKR_ROOM_SELECTING;
        switch (lobby_.phase) {
            case MDKR_ONLINE_LOBBY:
                /* REMATCH / CANCEL_LOADING observed: the round is over.
                 * Walk the session home (engine RACING must pass through
                 * FINISHED; the reducer refuses everything else) and re-arm
                 * every one-race latch so the NEXT BEGIN_LOADING rebuilds
                 * manifest + descriptor for the new epoch/track. */
                if (engine == MDKR_ENGINE_RACING) {
                    return sessionDispatch(MDKR_SESSION_COMMAND_SET_ENGINE_PHASE,
                                           MDKR_ENGINE_FINISHED);
                }
                if (sessionInRacePhase) {
                    if (room == MDKR_ROOM_RESULTS) {
                        journey_ = MDKR_ONLINE_JOURNEY_REMATCH;
                    }
                    if (!sessionDispatch(MDKR_SESSION_COMMAND_RETURN_TO_LOBBY,
                                         0u)) {
                        return false;
                    }
                    resetRaceLatches("lobby returned to LOBBY");
                    return true;
                }
                /* Belt-and-braces: latches armed but the session never made
                 * it into a race phase (e.g. a refused local transition). */
                if (raceLatchesArmed()) resetRaceLatches("lobby at LOBBY");
                return false;
            case MDKR_ONLINE_LOADING:
            case MDKR_ONLINE_RACING:
            case MDKR_ONLINE_RESULTS:
                /* A stale RESULTS session first walks home (a REMATCH +
                 * BEGIN_LOADING pair can land inside one snapshot gap). */
                if (lobby_.phase == MDKR_ONLINE_LOADING &&
                    room == MDKR_ROOM_RESULTS &&
                    engine == MDKR_ENGINE_FINISHED) {
                    if (!sessionDispatch(MDKR_SESSION_COMMAND_RETURN_TO_LOBBY,
                                         0u)) {
                        return false;
                    }
                    resetRaceLatches("REMATCH+BEGIN_LOADING snapshot gap");
                    return true;
                }
                if (sessionInLobbyScene &&
                    lobby_.phase != MDKR_ONLINE_RESULTS) {
                    MDKR_ONLINE_LOG(
                        "[START] room entered LOADING (following leader) rev=%u "
                        "epoch=%u track=%u vmask=0x%02x\n",
                        lobby_.revision, lobby_.match_epoch,
                        lobby_.selected_track, lobby_.selected_vehicle_mask);
                    (void)sessionDispatch(MDKR_SESSION_COMMAND_SET_ROOM_PHASE,
                                          MDKR_ROOM_LOADING);
                    /* Exactly one REQUEST_RACE per lobby BEGIN_LOADING keeps
                     * session and lobby match epochs in lock-step (the shared
                     * view model refuses a mismatched pair). An equal epoch
                     * means this session already requested this race (e.g.
                     * the post-re-verify resync) -- never bump past it. */
                    if (session_.state.match_epoch != lobby_.match_epoch) {
                        (void)sessionDispatch(MDKR_SESSION_COMMAND_REQUEST_RACE,
                                              0u);
                    }
                    return true;
                }
                if (lobby_.phase == MDKR_ONLINE_RACING &&
                    (room == MDKR_ROOM_LOADING ||
                     room == MDKR_ROOM_COUNTDOWN)) {
                    /* all_loaded + leader BEGIN_RACE landed: drive the
                     * lobby-facing session through the engine phases so
                     * lobby_phase_matches holds (LOADING -> RACING). */
                    if (engine == MDKR_ENGINE_BOOTING) {
                        return sessionDispatch(
                            MDKR_SESSION_COMMAND_SET_ENGINE_PHASE,
                            MDKR_ENGINE_READY);
                    }
                    if (engine == MDKR_ENGINE_READY) {
                        MDKR_ONLINE_LOG(
                            "[START] lobby RACING -> session RACE_CHROME "
                            "epoch=%u\n", lobby_.match_epoch);
                        return sessionDispatch(
                            MDKR_SESSION_COMMAND_SET_ENGINE_PHASE,
                            MDKR_ENGINE_RACING);
                    }
                    return false;
                }
                if (lobby_.phase == MDKR_ONLINE_RESULTS) {
                    if (engine == MDKR_ENGINE_RACING) {
                        MDKR_ONLINE_LOG(
                            "[START] lobby RESULTS -> session results scene "
                            "epoch=%u\n", lobby_.match_epoch);
                        return sessionDispatch(
                            MDKR_SESSION_COMMAND_SET_ENGINE_PHASE,
                            MDKR_ENGINE_FINISHED);
                    }
                    /* A joiner that never observed the RACING snapshot still
                     * walks BOOTING -> READY -> RACING -> FINISHED. */
                    if (engine == MDKR_ENGINE_BOOTING) {
                        return sessionDispatch(
                            MDKR_SESSION_COMMAND_SET_ENGINE_PHASE,
                            MDKR_ENGINE_READY);
                    }
                    if (engine == MDKR_ENGINE_READY) {
                        return sessionDispatch(
                            MDKR_SESSION_COMMAND_SET_ENGINE_PHASE,
                            MDKR_ENGINE_RACING);
                    }
                }
                return false;
            case MDKR_ONLINE_CLOSED:
                return false;
        }
        return false;
    }

    bool raceLatchesArmed() const {
        return loadingBuildDone_ || descriptorBuilt_ || raceReady_ ||
               ackLoadedSent_ || beginRaceSent_ || resultsReported_;
    }

    /* Re-arm every once-per-race latch when the room returns to the
     * lobby phase, so the NEXT BEGIN_LOADING (new match_epoch, possibly a new
     * track/mode) rebuilds the manifest + descriptor through the O-T5 clamp,
     * re-runs preflight consensus (attestations bind the new epoch), re-runs
     * setUpRace from its first tick and re-publishes the engine boot handoff.
     * The mesh deliberately stays up: it is keyed on leader_generation,
     * epoch-independent, and its channels/phrase survive rounds. */
    void resetRaceLatches(const char *why) {
        MDKR_ONLINE_LOG(
            "[START] race latches reset (%s): loading barrier re-armed for the "
            "next epoch (last epoch=%u)\n",
            why, haveLobby_ ? lobby_.match_epoch : 0u);
        loadingBuildDone_ = false;
        descriptorBuilt_ = false;
        refusal_ = MDKR_MATCH_LAUNCH_ADMITTED;
        preflightInit_ = false;
        preflightInitLogged_ = false;
        ownSubmitted_ = false;
        preflightReady_ = false;
        fragStates_.clear();
        /* Keep only FUTURE-epoch attestations a fast peer may already have
         * delivered; anything bound to the finished epoch is stale. */
        for (auto it = pendingPeerAtts_.begin();
             it != pendingPeerAtts_.end();) {
            it = (!haveLobby_ || it->second.match_epoch <= lobby_.match_epoch)
                     ? pendingPeerAtts_.erase(it)
                     : ++it;
        }
        installed_ = false; /* the launcher clears the process-global roster
                             * between races; re-arm the install path */
        raceReady_ = false;
        raceReadyLogged_ = false;
        ackLoadedSent_ = false;
        beginRaceSent_ = false;
        resultsReported_ = false;
        raceSendOwned_ = false;
        raceSweepServiceCalls_ = 0u;
        racePeerLost_ = false;
        raceAbortReceived_ = false;
        raceLossFailureLatched_ = false;
        raceEndFailureLatched_ = false;
        raceDegraded_ = false;
        lastPreflightGate_ = -1;
        if (failure_ == MDKR_ONLINE_VIEW_FAILURE_ENGINE_FAILED
#if MDKR_ENABLE_ONLINE_BETA
            || failure_ == MDKR_ONLINE_VIEW_FAILURE_OPPONENT_LEFT
            || failure_ == MDKR_ONLINE_VIEW_FAILURE_OPPONENT_NEVER_STARTED
            || failure_ == MDKR_ONLINE_VIEW_FAILURE_CONNECTION_UNPLAYABLE
#endif
        ) {
            failure_ = MDKR_ONLINE_VIEW_FAILURE_NONE; /* race-scoped failure */
        }
#if MDKR_ENABLE_ONLINE_BETA
        OnlineRoom_retractEngineRaceBoot(this);
#endif
        bump();
    }

    /* Drive the lobby's own loading handshake. After setUpRace
     * succeeded locally the endpoint acknowledges ACK_LOADED; once the
     * snapshot shows every member loaded the LEADER sends BEGIN_RACE. Both
     * are once-per-epoch (re-armed by resetRaceLatches). */
    void runRaceLobbyPhase() {
        if (!haveLobby_ || reVerify_ ||
            lobby_.phase != MDKR_ONLINE_LOADING) return;
        if (raceReady_ && !ackLoadedSent_) {
            ackLoadedSent_ = sendLobbyCommand(MDKR_ONLINE_ACK_LOADED, 0u, 0u);
            MDKR_ONLINE_LOG("[START] ACK_LOADED sent=%u epoch=%u\n",
                            ackLoadedSent_ ? 1u : 0u, lobby_.match_epoch);
        }
        if (!beginRaceSent_ && isLeader() && allMembersLoaded()) {
            beginRaceSent_ = sendLobbyCommand(MDKR_ONLINE_BEGIN_RACE, 0u, 0u);
            MDKR_ONLINE_LOG("[START] all loaded -> BEGIN_RACE sent=%u epoch=%u\n",
                            beginRaceSent_ ? 1u : 0u, lobby_.match_epoch);
        }
    }

    bool allMembersLoaded() const {
        if (!haveLobby_) return false;
        for (unsigned i = 0u; i < MDKR_ONLINE_MAX_ENDPOINTS; ++i) {
            const MdkrOnlineMember &m = lobby_.members[i];
            if (m.occupied && (!m.connected || !m.loaded)) return false;
        }
        return true;
    }

    /* Per-service race housekeeping: the deferred SAS-mismatch rekey, the
     * in-race transport-recovery poll and the resend sweep. */
    void raceServiceWork() {
        if (phraseNoticePending_) {
            if (sendPhraseMismatchNotice() ||
                ++phraseNoticeTries_ >= kPhraseNoticeMaxTries) {
                phraseNoticePending_ = false;
                phraseRekeyCountdown_ = kPhraseRekeyDelayServices;
            }
        }
        if (phraseRekeyCountdown_ > 0u && --phraseRekeyCountdown_ == 0u) {
            forcePhraseRekey();
        }
        if (!raceReady_) return;
        if (!raceDegraded_) {
            MdkrMatchRecovery rec;
            if (mdkr_match_transport_recovery(&raceTransport_, &rec)) {
                raceDegraded_ = true;
                MDKR_ONLINE_LOG(
                    "[ONLINE] race connection degraded: recovery reason=%d "
                    "firstTick=%u observed=%u slot=%u\n",
                    static_cast<int>(rec.reason), rec.first_unrecoverable_tick,
                    rec.observed_at_tick, rec.canonical_slot);
                bump();
            }
        }
        /* Resend sweep: only while race_advance owns the send side (the
         * production loop shape). race_drain_local's contract routes EVERY
         * transmission through the driver's own carrier (the impairment
         * matrix), so the sweep stays out of its way there. */
        if (!raceSendOwned_ || !mesh_) return;
        if (++raceSweepServiceCalls_ % kRaceSweepServicePeriod != 0u) return;
        if (raceNextTick_ <= raceFirstTick_) return; /* nothing sealed yet */
        const uint32_t newest = (raceNextTick_ - 1u) + raceInputDelay_;
        const uint32_t floor = newest > raceFirstTick_ + kRaceSweepWindow
                                   ? newest - kRaceSweepWindow
                                   : raceFirstTick_;
        for (uint32_t tick = newest;;) {
            sendLocalBundle(tick);
            ++raceResendBundles_;
            if (tick < floor + 3u) break;
            tick -= 3u;
        }
        ++raceResendSweeps_;
    }

    /* Retire the WHOLE mesh session after a reported SAS mismatch --
     * keys, channels and signaling -- and bring it back up through the
     * backend so fresh ephemeral keys derive a fresh transcript (and thus a
     * fresh phrase). Without this, RETRY would re-present the identical phrase
     * the humans just refused. The preflight barrier resets with it; the
     * room stays. */
    void forcePhraseRekey() {
        MDKR_ONLINE_LOG(
            "[MESH] SAS mismatch: retiring mesh keys/session and re-keying\n");
        mesh_.reset(); /* borrows the backend's feed: kill it first */
        if (opts_.meshBackend) opts_.meshBackend->reset();
        meshUp_ = false;
        meshGeneration_ = 0u;
        havePhrase_ = false;
        phraseConfirmed_ = false;
        haveConfirmedDigest_ = false;
        channelsReady_.clear();
        preflightInit_ = false;
        preflightInitLogged_ = false;
        ownSubmitted_ = false;
        preflightReady_ = false;
        fragStates_.clear();
        pendingPeerAtts_.clear();
        raceReady_ = false;
        raceSendOwned_ = false;
        /* Clear the race-scoped peer-loss latches here too: on a FIRST-race SAS
         * mismatch the re-verify -> re-confirm -> BEGIN_LOADING path never runs
         * resetRaceLatches (raceLatchesArmed() is false before any loading), so
         * a peer-loss latched during the mismatch teardown would otherwise
         * persist into the re-confirmed race's start barrier -- aborting and
         * kicking a HEALTHY peer with a false "couldn't start" card. */
        racePeerLost_ = false;
        raceAbortReceived_ = false;
        phraseNoticePending_ = false;
#if MDKR_ENABLE_ONLINE_BETA
        OnlineRoom_retractEngineRaceBoot(this);
#endif
        /* The mismatch is only reportable at the preflight surface, but a
         * peer-sent notice can land anywhere in the lobby sub-phases. */
        if (session_.state.room != MDKR_ROOM_PREFLIGHT) {
            (void)sessionDispatch(MDKR_SESSION_COMMAND_SET_ROOM_PHASE,
                                  MDKR_ROOM_PREFLIGHT);
        }
        bringUpMesh(); /* fresh ephemeral keys -> fresh transcript/phrase */
        bump();
    }

    /* ---- Peer mesh ---------------------------------------------------- */

    void bringUpMesh() {
        if (meshUp_ || !opts_.meshBackend || !haveLobby_) return;
        buildMeshRoster();
        meshEpoch_ = lobby_.leader_generation; /* stable nonzero keying epoch */
        MDKR_ONLINE_LOG(
            "[MESH] bringUpMesh roster=%u epoch=%u iceServers=%u localEp=%llu\n",
            static_cast<unsigned>(meshRoster_.size()), meshEpoch_,
            static_cast<unsigned>(iceServers_.size()),
            (unsigned long long)localEndpointId_);
        /* localGeneration 0 == adopt the welcome's assigned generation: against
         * the real signal service the server owns the per-endpoint monotonic
         * generation, so the launcher must not hardcode 1. */
        MdkrMatchPeerSignalFeed *feed = opts_.meshBackend->beginSignaling(
            localEndpointId_, 0u, roomIdStr_, credential_, iceServers_);
        if (feed == nullptr) {
            MDKR_ONLINE_LOG(
                "[MESH] beginSignaling FAILED -> CONNECTION_CHECK\n");
            failure_ = MDKR_ONLINE_VIEW_FAILURE_CONNECTION_CHECK;
            return;
        }
        MdkrMatchPeerMeshOptions o;
        o.signal = feed;
        o.roomId = roomIdStr_;
        o.localEndpointId = localEndpointId_;
        o.localGeneration = 0u;
        o.matchEpoch = meshEpoch_;
        o.compatibility = opts_.compatibility;
        o.roster = meshRoster_;
        o.iceServers = iceServers_;
        o.nowMs = nowMs_;
        std::string err;
        mesh_ = MdkrMatchPeerMesh::create(o, &err);
        if (!mesh_) {
            MDKR_ONLINE_LOG("[MESH] mesh create FAILED err=%s -> CONNECTION_CHECK\n",
                            err.c_str());
            failure_ = MDKR_ONLINE_VIEW_FAILURE_CONNECTION_CHECK;
            return;
        }
        meshUp_ = true;
        MDKR_ONLINE_LOG("[MESH] mesh up (peer connections starting)\n");
    }

    void buildMeshRoster() {
        std::map<uint64_t, uint8_t> masks;
        for (unsigned i = 0u; i < MDKR_ONLINE_MAX_SEATS; ++i) {
            const MdkrOnlineSeat &s = lobby_.seats[i];
            if (!s.occupied) continue;
            masks[s.endpoint_id] |= static_cast<uint8_t>(1u << i);
        }
        meshRoster_.clear();
        for (const auto &kv : masks) {
            MdkrMatchPeerSlotOwner owner;
            owner.endpointId = kv.first;
            owner.slotMask = kv.second;
            meshRoster_.push_back(owner);
        }
    }

    /* The SAS re-verification barrier.
     *
     * A mesh rekey after the humans confirmed the phrase -- the local
     * endpoint's replacement /signal socket (re-welcome, higher generation)
     * or a PEER's replacement (presence bump -> rekeyPeer) -- derives fresh
     * keys and a fresh phrase. docs/ref/match-signaling-v1.md:114 ("Secure
     * connection changed"): retire keys/channels, reconnect, and compare a
     * NEW phrase; NEVER reuse Ready. Without this barrier, phraseConfirmed_
     * would latch forever and play would silently continue on channels the
     * SAS never validated.
     *
     * Detection keys on the 256-bit TRANSCRIPT DIGEST, never the 20-bit
     * phrase (a phrase strcmp collides ~2^-20 and is grindable). Two
     * symmetric paths, both compared against the confirmed digest:
     * (a) mesh_->phrase() REFUSES while a rekey is in flight (keys retired)
     *     -- polled every pumpMesh(); catches a rekey that spans pumps; and
     * (b) a PhraseReady whose fresh transcript digest differs from the
     *     confirmed one -- catches a retire-and-rederive that completes
     *     inside ONE pump (the batched [presence,hello1,hello2,hello3] feed
     *     drain a malicious relay can force: keysDerived flips false->true
     *     within a single mesh pump(), so path (a) never observes the
     *     window, but the new digest still differs and arms the barrier).
     *
     * What the re-verify does: drop race-Ready IMMEDIATELY (the race feed
     * reports not-ready, so integrators stop advancing; the in-progress
     * race presentation is ABANDONED -- resuming it would be reusing
     * Ready), reset the confirmation and the whole preflight barrier
     * (attestations/fragments were bound to the retired generations),
     * surface the typed VERIFICATION_MISMATCH recovery (the launcher
     * model's "secure connection may have changed... compare a new phrase"
     * copy, primary "Reconnect Securely" = RETRY), and return the room to
     * PREFLIGHT so the fresh phrase surfaces after RETRY. A second
     * CONFIRM_PHRASE re-runs the barrier over the re-derived transcript;
     * install() then rebuilds the race transport from its FIRST tick (the
     * engine roster install itself is process-global and stays). Stale
     * (lower/equal-generation) welcomes never reach here: the mesh ignores
     * them, the phrase never retires, and nothing changes -- pinned by the
     * adapter test's negative arm. */
    void beginReVerify() {
        const bool confirmed = phraseConfirmed_;
        havePhrase_ = false;      /* the retired phrase must never redisplay */
        phraseConfirmed_ = false; /* never reuse Ready */
        haveConfirmedDigest_ = false; /* the confirmed transcript is retired */
        preflightInit_ = false;
        ownSubmitted_ = false;
        preflightReady_ = false;
        fragStates_.clear();
        pendingPeerAtts_.clear();
        /* Rebuild channel bookkeeping from LIVE mesh truth rather than
         * clearing blindly: a fresh PeerChannelsReady that raced the
         * detection must survive, and stale pre-rekey entries must go. */
        channelsReady_.clear();
        for (const MdkrMatchPeerSlotOwner &o : meshRoster_) {
            if (o.endpointId == localEndpointId_) continue;
            if (mesh_ && mesh_->peerChannelsReady(o.endpointId)) {
                channelsReady_.insert(o.endpointId);
            }
        }
        if (confirmed) {
            reVerify_ = true;
            raceReady_ = false;
            raceSendOwned_ = false;
            /* Same stale-latch shape as forcePhraseRekey: clear the race-scoped
             * peer-loss latches so a mismatch detected on a confirmed phrase
             * cannot carry a peer-loss into the re-confirmed race's barrier. */
            racePeerLost_ = false;
            raceAbortReceived_ = false;
#if MDKR_ENABLE_ONLINE_BETA
            /* The interrupted race's Ready is never reused: retract the boot
             * handoff so main_app cannot boot on the abandoned transport. */
            OnlineRoom_retractEngineRaceBoot(this);
#endif
            failure_ = MDKR_ONLINE_VIEW_FAILURE_VERIFICATION_MISMATCH;
            /* The session may be mid-race (RACE_CHROME) when the rekey lands.
             * The reducer refuses a room-phase hop out of an active race, so
             * walk the abandoned race out first (FINISHED -> back to lobby); the
             * in-progress presentation is abandoned by design. */
            if (session_.state.engine == MDKR_ENGINE_RACING) {
                (void)sessionDispatch(MDKR_SESSION_COMMAND_SET_ENGINE_PHASE,
                                      MDKR_ENGINE_FINISHED);
            }
            if (session_.state.room == MDKR_ROOM_RESULTS ||
                session_.state.room == MDKR_ROOM_LOADING ||
                session_.state.room == MDKR_ROOM_COUNTDOWN) {
                (void)sessionDispatch(MDKR_SESSION_COMMAND_RETURN_TO_LOBBY, 0u);
            }
            (void)sessionDispatch(MDKR_SESSION_COMMAND_SET_ROOM_PHASE,
                                  MDKR_ROOM_PREFLIGHT);
        }
        bump();
    }

    void pumpMesh() {
        if (!meshUp_ || !mesh_) return;
        mesh_->pump();
        mesh_->drainEvents(meshEvents_);
        for (const MdkrMatchPeerMeshEvent &ev : meshEvents_) {
            switch (ev.type) {
                case MdkrMatchPeerMeshEventType::PhraseReady: {
                    std::string p;
                    uint8_t digest[MDKR_MATCH_PEER_TRANSCRIPT_DIGEST_BYTES];
                    if (mesh_->phrase(p) &&
                        p.size() + 1u <= sizeof(phrase_) &&
                        mesh_->transcriptDigest(digest)) {
                        /* Path (b): a fresh transcript digest that differs
                         * from the confirmed one is a re-key to re-verify,
                         * even if it collided to the same 20-bit phrase and
                         * even if it landed inside a single pump. */
                        if (haveConfirmedDigest_ &&
                            std::memcmp(digest, confirmedDigest_,
                                        sizeof(digest)) != 0) {
                            beginReVerify();
                        }
                        std::memcpy(phrase_, p.c_str(), p.size() + 1u);
                        havePhrase_ = true;
                        /* The post-mismatch rekey completed: fresh keys, fresh
                         * phrase; peer-lost suppression window closes. */
                        phraseMismatchActive_ = false;
                        /* Adopt the service-assigned local generation now that
                         * the welcome has been processed; it is bound into the
                         * graph and the local attestation. */
                        meshGeneration_ = mesh_->connectionGeneration();
                        MDKR_ONLINE_LOG(
                            "[MESH] phrase ready gen=%u (SAS phrase available for "
                            "comparison)\n",
                            meshGeneration_);
                        bump();
                    }
                    break;
                }
                case MdkrMatchPeerMeshEventType::PeerChannelsReady:
                    channelsReady_.insert(ev.endpointId);
                    MDKR_ONLINE_LOG(
                        "[MESH] peer channels ready ep=%llu channelsReady=%u/%u "
                        "(state+control DataChannels open)\n",
                        (unsigned long long)ev.endpointId,
                        static_cast<unsigned>(channelsReady_.size()),
                        meshRoster_.empty()
                            ? 0u
                            : static_cast<unsigned>(meshRoster_.size() - 1u));
                    bump();
                    break;
                case MdkrMatchPeerMeshEventType::PreflightFragment:
                    onPreflightFragment(ev);
                    break;
                case MdkrMatchPeerMeshEventType::PeerLost:
                    /* Expose the mid-race condition on the race info
                     * feed too -- during the race nobody renders the lobby
                     * failure surface, so the launcher's engine loop polls
                     * peerLost to end the session; the failure below then
                     * fronts the post-race recovery copy. */
                    racePeerLost_ = true;
                    if (phraseMismatchActive_ || phraseRekeyCountdown_ > 0u) {
                        /* The deliberate mismatch teardown races both sides'
                         * channel closes; keep the VERIFICATION_MISMATCH
                         * surface instead of a misleading network failure. */
                        MDKR_ONLINE_LOG(
                            "[MESH] peer lost during SAS-mismatch rekey "
                            "ep=%llu (suppressed)\n",
                            (unsigned long long)ev.endpointId);
                        break;
                    }
                    failure_ = mapLostReason(ev.lostReason, raceReady_);
                    /* Mark this as the IN-RACE loss-mapped failure so the
                     * capture path can clear exactly it (and nothing else, e.g.
                     * a genuine VERIFICATION_MISMATCH) when a finish order was
                     * committed before the peer dropped. */
                    raceLossFailureLatched_ = true;
                    /* While a race-END card (OPPONENT_LEFT /
                     * CONNECTION_UNPLAYABLE, or the beta-OFF CONNECTION_CHECK
                     * fallback) is latched, view() must front it even if a late
                     * State snapshot re-latches a lobby whose phase disagrees
                     * with the walked session. Only in-race losses arm this;
                     * pre-connection losses keep the ordinary lobby input. */
                    if (raceReady_) raceEndFailureLatched_ = true;
                    MDKR_ONLINE_LOG(
                        "[MESH] peer LOST ep=%llu reason=%d -> failure=%u\n",
                        (unsigned long long)ev.endpointId,
                        static_cast<int>(ev.lostReason),
                        static_cast<unsigned>(failure_));
                    bump();
                    break;
                case MdkrMatchPeerMeshEventType::Failure:
                    /* SignalLost with healthy channels is a status, not a
                     * failure (docs: "Room updates reconnecting"). */
                    if (ev.failure != MdkrMatchPeerMeshFailure::SignalLost) {
                        MDKR_ONLINE_LOG(
                            "[MESH] mesh failure=%d -> VERIFICATION_MISMATCH\n",
                            static_cast<int>(ev.failure));
                        failure_ = MDKR_ONLINE_VIEW_FAILURE_VERIFICATION_MISMATCH;
                        bump();
                    } else {
                        MDKR_ONLINE_LOG(
                            "[MESH] signal lost (channels healthy; reconnecting)\n");
                    }
                    break;
                case MdkrMatchPeerMeshEventType::InputEnvelope:
                    if (inputEnvelopes_ == 0u) {
                        MDKR_ONLINE_LOG(
                            "[MESH] first race input envelope received\n");
                    }
                    ++inputEnvelopes_;
                    feedInputEnvelope(ev);
                    break;
            }
        }
        /* A peer that aborted its race-start barrier (or ended mid-race)
         * signals us over the reliable control channel. Consume-once from the
         * mesh and latch it locally; the drain's barrier AND mid-race polls both
         * key on racePeerLost(), which folds this in, so a received abort ends
         * our race exactly like a lost peer. */
        if (mesh_ && mesh_->consumeRaceAbort() && !raceAbortReceived_) {
            raceAbortReceived_ = true;
            MDKR_ONLINE_LOG(
                "[MESH] race-abort received from peer -> ending local race\n");
            bump();
        }
        /* Rekey-in-flight detection (see beginReVerify): the mesh refuses
         * phrase() the moment any generation bump retires the keys. Checked
         * AFTER the event drain so channel bookkeeping rebuilt inside
         * beginReVerify sees this pump's events. Self-limiting: the reset
         * clears havePhrase_, and it only re-arms once the NEW phrase has
         * been adopted. */
        if (havePhrase_) {
            std::string current;
            if (!mesh_->phrase(current)) beginReVerify();
        }
    }

public:
    /* Map a mesh peer-loss reason to a lobby-facing failure. `raceBegun` says a
     * playable race connection had actually come up (raceReady_) before the
     * loss, which changes what is TRUTHFUL: a peer that pings-out or ends after
     * the race is racing has "disconnected" (OPPONENT_LEFT), not "could not
     * establish a connection"; a seal-window exhaustion mid-race is a genuine
     * transport breakdown (CONNECTION_UNPLAYABLE), not a failed handshake. A
     * pre-connection loss keeps the original establishment copy. The OPPONENT_*
     * / CONNECTION_UNPLAYABLE enums are beta-only, so a beta-OFF compile of this
     * TU falls back to the pre-existing CONNECTION_CHECK. */
    static MdkrOnlineViewFailure mapLostReason(MdkrMatchPeerLostReason r,
                                               bool raceBegun) {
        switch (r) {
            case MdkrMatchPeerLostReason::CommitmentMismatch:
            case MdkrMatchPeerLostReason::HelloViolation:
            case MdkrMatchPeerLostReason::ControlChannelViolation:
                return MDKR_ONLINE_VIEW_FAILURE_VERIFICATION_MISMATCH;
            case MdkrMatchPeerLostReason::ConnectTimeout:
            case MdkrMatchPeerLostReason::TransportFailed:
                /* Always a handshake-time failure by nature. */
                return MDKR_ONLINE_VIEW_FAILURE_NETWORKS_CANNOT_CONNECT;
            case MdkrMatchPeerLostReason::SealWindowExhausted:
#if MDKR_ENABLE_ONLINE_BETA
                if (raceBegun)
                    return MDKR_ONLINE_VIEW_FAILURE_CONNECTION_UNPLAYABLE;
#endif
                (void)raceBegun;
                return MDKR_ONLINE_VIEW_FAILURE_CONNECTION_CHECK;
            case MdkrMatchPeerLostReason::PingTimeout:
            case MdkrMatchPeerLostReason::PeerEnded:
            /* PeerVanished: the signal service saw the peer's socket die AND
             * its transport went down (the mid-race kill/quit signature) --
             * the peer DEPARTED. Same truthful attribution as a ping-out:
             * mid-race it is OPPONENT_LEFT, never a connection-establishment
             * demotion. */
            case MdkrMatchPeerLostReason::PeerVanished:
            default:
#if MDKR_ENABLE_ONLINE_BETA
                if (raceBegun)
                    return MDKR_ONLINE_VIEW_FAILURE_OPPONENT_LEFT;
#endif
                (void)raceBegun;
                return MDKR_ONLINE_VIEW_FAILURE_CONNECTION_CHECK;
        }
    }

private:

    /* ---- Loading barrier: build descriptor through the O-T5 clamp ------ */

    void runLoadingBarrier() {
        if (loadingBuildDone_ || !haveLobby_ ||
            lobby_.phase != MDKR_ONLINE_LOADING) {
            return;
        }
        /* The snapshot is authoritative and frozen at LOADING; the descriptor
         * is built directly from it with no opportunity for roster mutation
         * between the barrier and the build. */
        loadingBuildDone_ = true;
        MDKR_ONLINE_LOG(
            "[LOADING] barrier: building descriptor track=%u vmask=0x%02x "
            "seats=%u epoch=%u\n",
            lobby_.selected_track, lobby_.selected_vehicle_mask,
            lobby_.seat_count, lobby_.match_epoch);
        MdkrMatchManifestV1 manifest;
        if (!manifestFromLobby(lobby_, opts_.compatibility, opts_.inputDelay,
                               &manifest)) {
            MDKR_ONLINE_LOG(
                "[LOADING] manifest build FAILED -> ENGINE_FAILED (race blocked)\n");
            refusal_ = MDKR_MATCH_LAUNCH_REFUSE_SNAPSHOT;
            failure_ = MDKR_ONLINE_VIEW_FAILURE_ENGINE_FAILED;
            bump();
            return;
        }
        if (!mdkr_match_launch_descriptor_from_lobby(
                &lobby_, &manifest, &opts_.localRoster, &descriptor_,
                &refusal_)) {
            /* Retail-identity clamp (or a snapshot/selection refusal) fired:
             * fail closed, install nothing, keep the room. */
            MDKR_ONLINE_LOG(
                "[LOADING] descriptor REFUSED refusal=%d -> ENGINE_FAILED "
                "(race blocked)\n",
                static_cast<int>(refusal_));
            descriptorBuilt_ = false;
            failure_ = MDKR_ONLINE_VIEW_FAILURE_ENGINE_FAILED;
            bump();
            return;
        }
        descriptorBuilt_ = true;
        MDKR_ONLINE_LOG("[LOADING] descriptor built (advancing to preflight)\n");
        bump();
    }

    /* ---- Preflight consensus + install -------------------------------- */

    void runPreflight() {
        /* Gate on preflightReady_ alone (not installed_): the SAS re-verify
         * barrier clears preflightReady_ so consensus re-runs over the
         * re-derived transcript even though the process-global engine
         * roster stays installed. First-run behavior is unchanged --
         * preflightReady_ latches true at READY and only the barrier ever
         * clears it. */
        if (!descriptorBuilt_ || preflightReady_) return;
        /* Past the descriptor barrier we are actively trying to reach consensus;
         * log the first blocking gate on change so a LOADING stall's cause is
         * obvious in the capture (the classic 2-machine failure is the STATE/
         * control channel never opening -> channelsReady stuck below roster). */
        if (!meshUp_ || !mesh_) { logPreflightGate(2, "peer mesh not up yet"); return; }
        /* Every roster peer's channels must be ready before consensus. */
        if (channelsReady_.size() + 1u < meshRoster_.size()) {
            logPreflightGate(3, "waiting for peer STATE+control channels");
            return;
        }
        if (!phraseConfirmed_ || !havePhrase_) {
            logPreflightGate(4, "waiting for safety-phrase confirmation");
            return;
        }
        logPreflightGate(0, "all gates open; running consensus");

        if (!preflightInit_) {
            buildGraph();
            uint8_t transcriptDigest[MDKR_MATCH_PREFLIGHT_DIGEST_BYTES];
            if (!mesh_ || !mesh_->transcriptDigest(transcriptDigest)) {
                logPreflightGate(5, "waiting for transcript digest");
                return; /* phrase/digest not ready yet; retry next service() */
            }
            if (!mdkr_match_preflight_init(&preflight_, &descriptor_, &graph_,
                                           transcriptDigest, localEndpointId_,
                                           meshGeneration_)) {
                logPreflightGate(6, "preflight init refused");
                return;
            }
            preflightInit_ = true;
            if (!preflightInitLogged_) {
                preflightInitLogged_ = true;
                MDKR_ONLINE_LOG(
                    "[PREFLIGHT] init epoch=%u gen=%u romVerified=%u "
                    "phraseConfirmed=%u\n",
                    descriptor_.manifest.match_epoch, meshGeneration_,
                    opts_.romVerified ? 1u : 0u, phraseConfirmed_ ? 1u : 0u);
            }
            /* Any peer attestations that arrived before init are now applied. */
            for (const auto &kv : pendingPeerAtts_) {
                (void)mdkr_match_preflight_submit(&preflight_, kv.first,
                                                  meshGeneration_, &kv.second);
            }
            pendingPeerAtts_.clear();
        }

        if (!ownSubmitted_) {
            MdkrMatchPreflightAttestationV1 att;
            buildOwnAttestation(&att);
            if (mdkr_match_preflight_submit(&preflight_, localEndpointId_,
                                            meshGeneration_, &att) ==
                MDKR_MATCH_PREFLIGHT_SUBMIT_ACCEPTED) {
                sendOwnFragments(att);
                ownSubmitted_ = true;
                lastFragmentSendMs_ = nowMs_();
                MDKR_ONLINE_LOG(
                    "[PREFLIGHT] own attestation submitted "
                    "(flags rom=%u phrase=%u channels=1); fragments sent\n",
                    opts_.romVerified ? 1u : 0u, phraseConfirmed_ ? 1u : 0u);
            }
        } else if (nowMs_() - lastFragmentSendMs_ >= 1000u) {
            /* Idempotent 1 Hz fragment re-send while consensus is pending: a
             * peer whose reassembly raced a round boundary (or lost a control
             * message) recovers instead of stalling forever -- fragments are
             * otherwise sent exactly once. Duplicates decode as DUPLICATE and
             * change nothing. */
            lastFragmentSendMs_ = nowMs_();
            MdkrMatchPreflightAttestationV1 att;
            buildOwnAttestation(&att);
            sendOwnFragments(att);
        }

        const MdkrMatchPreflightStatus status =
            mdkr_match_preflight_evaluate(&preflight_);
        if (status.state == MDKR_MATCH_PREFLIGHT_READY) {
            MDKR_ONLINE_LOG(
                "[PREFLIGHT] consensus READY (descriptor+transcript+graph agree; "
                "installing race)\n");
            preflightReady_ = true;
            install();
            bump();
        }
    }

    /* De-duplicated blocking-gate note for the preflight/loading stall. */
    void logPreflightGate(int gate, const char *why) {
        if (gate == lastPreflightGate_) return;
        lastPreflightGate_ = gate;
        MDKR_ONLINE_LOG(
            "[PREFLIGHT] %s (channelsReady=%u/%u phraseConfirmed=%u havePhrase=%u "
            "meshUp=%u descriptorBuilt=%u)\n",
            why, static_cast<unsigned>(channelsReady_.size()),
            meshRoster_.empty()
                ? 0u
                : static_cast<unsigned>(meshRoster_.size() - 1u),
            phraseConfirmed_ ? 1u : 0u, havePhrase_ ? 1u : 0u, meshUp_ ? 1u : 0u,
            descriptorBuilt_ ? 1u : 0u);
    }

    void buildGraph() {
        std::vector<MdkrMatchPeerEndpoint> eps;
        std::map<uint64_t, unsigned> index;
        for (const MdkrMatchPeerSlotOwner &o : meshRoster_) {
            MdkrMatchPeerEndpoint e;
            std::memset(&e, 0, sizeof(e));
            e.endpoint_id = o.endpointId;
            /* Each endpoint's own service-assigned generation, so both peers
             * hash a byte-identical graph. Local uses the adopted generation;
             * peers use the value the mesh learned from the welcome. */
            if (o.endpointId == localEndpointId_) {
                e.generation = meshGeneration_;
            } else if (uint32_t g = 0u; mesh_ && mesh_->peerGeneration(
                                            o.endpointId, &g)) {
                e.generation = g;
            } else {
                e.generation = meshGeneration_;
            }
            e.reachable_mask = 0u;
            index[o.endpointId] = static_cast<unsigned>(eps.size());
            eps.push_back(e);
        }
        /* Mutual edges: local <-> every peer whose channels are ready. Both
         * peers derive the same topology, and the digest is order-independent. */
        const unsigned self = index.count(localEndpointId_)
                                  ? index[localEndpointId_]
                                  : 0u;
        for (uint64_t peer : channelsReady_) {
            const auto it = index.find(peer);
            if (it == index.end()) continue;
            eps[self].reachable_mask |=
                static_cast<uint8_t>(1u << it->second);
            eps[it->second].reachable_mask |=
                static_cast<uint8_t>(1u << self);
        }
        std::memset(&graph_, 0, sizeof(graph_));
        (void)mdkr_match_peer_graph_init(&graph_,
                                         descriptor_.manifest.match_epoch,
                                         eps.data(),
                                         static_cast<unsigned>(eps.size()));
    }

    void buildOwnAttestation(MdkrMatchPreflightAttestationV1 *att) const {
        std::memset(att, 0, sizeof(*att));
        att->protocol_version = MDKR_MATCH_PREFLIGHT_VERSION;
        att->match_epoch = descriptor_.manifest.match_epoch;
        att->connection_generation = meshGeneration_;
        /* Monotonic per round (the descriptor epoch is >= 1 in LOADING and
         * strictly grows), so a round-2 fragment reaching a peer whose
         * reassembly still holds round 1 replaces it through the codec's own
         * newer-sequence path instead of colliding on a constant 1. */
        att->sequence = descriptor_.manifest.match_epoch;
        att->endpoint_id = localEndpointId_;
        (void)mdkr_match_preflight_descriptor_digest(&descriptor_,
                                                     att->descriptor_digest);
        if (mesh_) (void)mesh_->transcriptDigest(att->transcript_digest);
        (void)mdkr_match_preflight_graph_digest(&graph_, att->graph_digest);
        att->flags = 0u;
        if (opts_.romVerified) att->flags |= MDKR_MATCH_PREFLIGHT_ROM_VERIFIED;
        if (phraseConfirmed_) att->flags |= MDKR_MATCH_PREFLIGHT_PHRASE_CONFIRMED;
        att->flags |= MDKR_MATCH_PREFLIGHT_CHANNELS_READY;
    }

    void sendOwnFragments(const MdkrMatchPreflightAttestationV1 &att) {
        for (const MdkrMatchPeerSlotOwner &o : meshRoster_) {
            if (o.endpointId == localEndpointId_) continue;
            for (unsigned i = 0u; i < MDKR_MATCH_PREFLIGHT_FRAGMENT_COUNT; ++i) {
                uint8_t payload[MDKR_MATCH_PEER_PAYLOAD_BYTES];
                if (mdkr_match_preflight_fragment_encode(&att, i, payload)) {
                    (void)mesh_->sendPreflightFragment(o.endpointId, payload);
                }
            }
        }
    }

    /* The sealed control channel carries exactly one payload type the mesh
     * surfaces to us (PREFLIGHT). The SAS-mismatch notice rides the
     * same sealed 64-byte contract, distinguished by a header no legitimate
     * fragment can produce: byte 4 is the fragment index and the codec only
     * ever emits 0..2, so 0xFF is unreachable. */
    static bool isPhraseMismatchNotice(const uint8_t *payload) {
        return payload[0] == 'G' && payload[1] == 'B' && payload[2] == 'M' &&
               payload[3] == 'M' && payload[4] == 0xFFu && payload[5] == 1u;
    }

    bool sendPhraseMismatchNotice() {
        if (!meshUp_ || !mesh_) return false;
        uint8_t payload[MDKR_MATCH_PEER_PAYLOAD_BYTES];
        std::memset(payload, 0, sizeof(payload));
        payload[0] = 'G'; payload[1] = 'B'; payload[2] = 'M'; payload[3] = 'M';
        payload[4] = 0xFFu; /* invalid fragment index: never a real fragment */
        payload[5] = 1u;    /* notice version */
        bool any = false;
        for (const MdkrMatchPeerSlotOwner &o : meshRoster_) {
            if (o.endpointId == localEndpointId_) continue;
            any = mesh_->sendPreflightFragment(o.endpointId, payload) || any;
        }
        if (any) MDKR_ONLINE_LOG("[MESH] SAS mismatch notice sent\n");
        return any;
    }

    void onPreflightFragment(const MdkrMatchPeerMeshEvent &ev) {
        const uint64_t peer = ev.context.key.source_endpoint_id;
        if (isPhraseMismatchNotice(ev.payload.data())) {
            /* A human on the peer display reported "Words Differ": leave the
             * confirm surface, present the mismatch recovery, and retire the
             * keys on the next service() (never mid event drain -- the rekey
             * destroys the mesh this loop is iterating). */
            MDKR_ONLINE_LOG(
                "[MESH] peer ep=%llu reported SAS mismatch -> retiring keys\n",
                (unsigned long long)peer);
            failure_ = MDKR_ONLINE_VIEW_FAILURE_VERIFICATION_MISMATCH;
            phraseMismatchActive_ = true;
            if (phraseRekeyCountdown_ == 0u) phraseRekeyCountdown_ = 1u;
            bump();
            return;
        }
        /* The sealed carrier keys its context on the MESH epoch (the stable
         * leader_generation -- deliberately round-independent), while the
         * attestation inside binds the DESCRIPTOR match_epoch, which
         * advances every round. The fragment codec cross-checks the
         * two, so translate the carrier context onto the current round's
         * epoch at this boundary; authentication already happened in the
         * mesh, and the attestation's own epoch is still validated against
         * the preflight instance on submit. */
        MdkrMatchPeerEnvelopeContext ctx = ev.context;
        ctx.key.match_epoch =
            descriptorBuilt_ ? descriptor_.manifest.match_epoch
            : (haveLobby_ && lobby_.match_epoch != 0u) ? lobby_.match_epoch
                                                       : ctx.key.match_epoch;
        auto it = fragStates_.find(peer);
        if (it == fragStates_.end()) {
            MdkrMatchPreflightFragmentState st;
            if (!mdkr_match_preflight_fragment_state_init(&st, &ctx.key)) {
                return;
            }
            it = fragStates_.emplace(peer, st).first;
        }
        MdkrMatchPreflightAttestationV1 att;
        const MdkrMatchPreflightFragmentResult r =
            mdkr_match_preflight_fragment_submit(&it->second, &ctx,
                                                 ev.payload.data(), &att);
        if (r == MDKR_MATCH_PREFLIGHT_FRAGMENT_CONTEXT_MISMATCH ||
            r == MDKR_MATCH_PREFLIGHT_FRAGMENT_CONFLICT ||
            r == MDKR_MATCH_PREFLIGHT_FRAGMENT_INVALID) {
            /* A reassembly pinned to a stale epoch (a round boundary raced
             * this fragment) can never complete; drop it so the peer's
             * periodic fragment re-send re-initializes it against the
             * current round. Only the authentic peer can reach this channel
             * (sealed + authenticated), so this cannot be used to reset an
             * honest reassembly from outside. */
            fragStates_.erase(peer);
            return;
        }
        if (r != MDKR_MATCH_PREFLIGHT_FRAGMENT_COMPLETE) return;
        MDKR_ONLINE_LOG(
            "[PREFLIGHT] peer attestation complete ep=%llu epoch=%u (%s)\n",
            (unsigned long long)peer, att.match_epoch,
            preflightInit_ ? "submitted" : "queued until init");
        if (preflightInit_ &&
            att.match_epoch == descriptor_.manifest.match_epoch) {
            (void)mdkr_match_preflight_submit(
                &preflight_, peer, ev.context.key.source_generation, &att);
        } else {
            /* Not for the current preflight instance (or none yet): queue it.
             * init drains the queue; resetRaceLatches prunes stale epochs. */
            pendingPeerAtts_[peer] = att;
        }
    }

    void install() {
        if (raceReady_) return;
        uint8_t localSlots[MDKR_MATCH_SLOTS];
        unsigned localCount = 0u;
        for (unsigned i = 0u; i < descriptor_.manifest.slot_count; ++i) {
            if (descriptor_.manifest.slot_owner[i] == localEndpointId_) {
                localSlots[localCount++] = static_cast<uint8_t>(i);
            }
        }
        /* The engine roster install is process-global and once-only; after
         * an SAS re-verify barrier it is skipped and only the race
         * transport below is rebuilt -- from its FIRST tick, because the
         * interrupted race's Ready is never reused (see beginReVerify). */
        if (!installed_) {
            MdkrNetRoster roster;
            if (mdkr_net_roster_init(&roster, &descriptor_.manifest) &&
                mdkr_net_roster_configure_local(&roster, localSlots,
                                                localCount) &&
                mdkr_net_roster_set_viewports(&roster, localSlots,
                                              localCount) &&
                mdkr_net_roster_runtime_install_launch(&descriptor_,
                                                       &roster)) {
                installed_ = true;
                MDKR_ONLINE_LOG(
                    "[START] engine roster installed localSlots=%u\n", localCount);
            } else {
                MDKR_ONLINE_LOG(
                    "[START] engine roster install FAILED (race cannot boot)\n");
            }
        }
        /* The race transport is per-endpoint (its own session bridge), so it is
         * brought up independently of the process-global engine roster: in a
         * real online race each process holds its own runtime AND its race
         * transport, while two endpoints sharing one process (the in-process
         * convergence test) can only publish one global roster but must still
         * both race. */
        setUpRace(localSlots, localCount);
    }

    /* Post-install: bring up the launcher-side match transport bound to a
     * headless session bridge, so opened INPUT envelopes advance a real engine
     * timeline. Best-effort: a race-setup failure never unwinds the roster
     * install (which is the launcher's actual contract). */
    void setUpRace(const uint8_t *localSlots, unsigned localCount) {
        uint8_t localMask = 0u;
        for (unsigned i = 0u; i < localCount; ++i) {
            localMask |= static_cast<uint8_t>(1u << localSlots[i]);
        }
        MdkrSessionLaunchV3 launch;
        std::memset(&launch, 0, sizeof(launch));
        launch.version = MDKR_SESSION_LAUNCH_V3_VERSION;
        launch.size = sizeof(launch);
        launch.match = descriptor_;
        launch.local_slot_mask = localMask;
        launch.viewport_slot_mask = localMask;
        mdkr_session_bridge_init(&raceBridge_);
        if (!mdkr_session_bridge_apply_launch_v3(&raceBridge_, &launch) ||
            !mdkr_session_bridge_set_engine_phase(&raceBridge_,
                                                  MDKR_ENGINE_BOOTING) ||
            !mdkr_session_bridge_set_engine_phase(&raceBridge_,
                                                  MDKR_ENGINE_READY)) {
            MDKR_ONLINE_LOG(
                "[START] setUpRace FAILED: session bridge launch/phase rejected "
                "(race transport not ready, no boot handoff)\n");
            return;
        }
        raceEpoch_ = descriptor_.manifest.match_epoch;
        raceFirstTick_ = 1u;
        raceNextTick_ = raceFirstTick_;
        /* Fresh race, fresh seal history: authored ticks restart at 1 every
         * race, and the first-write-wins seal rule would otherwise replay the
         * PREVIOUS race's recorded pads for the same tick numbers. */
        std::memset(localHistTick_, 0, sizeof(localHistTick_));
        std::memset(localHist_, 0, sizeof(localHist_));
        localPendingCount_ = 0u;
        if (!mdkr_match_transport_init(&raceTransport_, &raceBridge_,
                                       raceFirstTick_)) {
            MDKR_ONLINE_LOG(
                "[START] setUpRace FAILED: match transport init rejected "
                "(no boot handoff)\n");
            return;
        }
        peerSlotMask_.clear();
        for (const MdkrMatchPeerSlotOwner &o : meshRoster_) {
            if (o.endpointId == localEndpointId_) continue;
            peerSlotMask_[o.endpointId] = o.slotMask;
        }
        raceInputDelay_ = opts_.inputDelay != 0u ? opts_.inputDelay : 2u;
        raceSendOwned_ = false;
        raceDegraded_ = false;
        raceSweepServiceCalls_ = 0u;
        raceReady_ = true;
        if (!raceReadyLogged_) {
            raceReadyLogged_ = true;
            MDKR_ONLINE_LOG(
                "[START] race transport READY epoch=%u firstTick=%u local=0x%02x "
                "remote=0x%02x -> publishing engine race-boot handoff\n",
                raceEpoch_, raceFirstTick_, raceTransport_.local_slot_mask,
                raceTransport_.remote_slot_mask);
        }
#if MDKR_ENABLE_ONLINE_BETA
        /* Make-or-break handoff: the visual race-start point has been reached.
         * Publish THIS adapter so main_app's interactive loop can boot the
         * visible engine on the live transport. Driven off adapter state, never
         * a UI callback. */
        OnlineRoom_publishEngineRaceBoot(this);
#endif
    }

    /* One opened INPUT envelope -> the launcher transport. The authenticated
     * slot mask is the SENDER's owned canonical slots (from the roster, never
     * the packet bytes); only covered ticks/slots are admitted. */
    void feedInputEnvelope(const MdkrMatchPeerMeshEvent &ev) {
        if (!raceReady_) return;
        const auto it = peerSlotMask_.find(ev.context.key.source_endpoint_id);
        if (it == peerSlotMask_.end()) return;
        const uint8_t authMask = it->second;
        MdkrMatchInputBundle bundle;
        if (!mdkr_match_input_bundle_decode(ev.payload.data(),
                                            ev.payload.size(), &bundle)) {
            return;
        }
        for (unsigned age = 0u; age < bundle.frame_count &&
                                age < MDKR_MATCH_INPUT_BUNDLE_FRAMES; ++age) {
            const uint32_t authoredTick = bundle.newest_tick - age;
            for (unsigned slot = 0u; slot < MDKR_SESSION_MAX_PLAYERS; ++slot) {
                const uint8_t bit = static_cast<uint8_t>(1u << slot);
                if ((bundle.slot_mask & bit) == 0u ||
                    (authMask & bit) == 0u) {
                    continue;
                }
                (void)mdkr_match_transport_receive(
                    &raceTransport_, bundle.match_epoch, authMask, slot,
                    authoredTick, &bundle.frames[age][slot]);
            }
        }
    }

public:
    /* ---- O-T6 race API (reached through the free accessors) ------------- */
    bool raceReady() const { return raceReady_; }

    void raceInfo(MdkrOnlineLiveRaceInfo *out) const {
        out->ready = raceReady_;
        out->matchEpoch = raceEpoch_;
        out->firstTick = raceFirstTick_;
        out->nextTick = raceNextTick_;
        out->activeSlotMask = raceTransport_.active_slot_mask;
        out->localSlotMask = raceTransport_.local_slot_mask;
        out->remoteSlotMask = raceTransport_.remote_slot_mask;
        out->inputDelay = raceInputDelay_;
        /* A peer that ABORTED its start barrier is, for the drain's
         * purposes, indistinguishable from a mesh-lost peer: either way we must
         * not keep racing its frozen input. Fold the received-abort latch into
         * the same signal. */
        out->peerLost = racePeerLost_ || raceAbortReceived_;
        MdkrMatchRecovery rec;
        out->connectionDegraded =
            raceDegraded_ ||
            (raceReady_ && mdkr_match_transport_recovery(&raceTransport_, &rec));
    }

    /* Cheap drain-facing peer-loss latch (see the header): the engine-session
     * drain polls this every service iteration to end the visible race the
     * moment the opponent vanishes -- or the moment the opponent tells us it
     * aborted its own start barrier. */
    bool racePeerLost() const { return racePeerLost_ || raceAbortReceived_; }

    /* Applying `incoming` over the already-latched `current` would DEMOTE a
     * more-specific mid-race breakdown to the drain's reason-blind OPPONENT_LEFT.
     * The drain sees only race_peer_lost() and reports OpponentLeft, so a
     * SealWindowExhausted (which the mesh already diagnosed as
     * CONNECTION_UNPLAYABLE) would be overwritten with the wrong "opponent
     * disconnected" copy. Pure + public so the beta test can pin it. */
    static bool raceEndFailureDemotes(MdkrOnlineViewFailure incoming,
                                      MdkrOnlineViewFailure current) {
#if MDKR_ENABLE_ONLINE_BETA
        return incoming == MDKR_ONLINE_VIEW_FAILURE_OPPONENT_LEFT &&
               current == MDKR_ONLINE_VIEW_FAILURE_CONNECTION_UNPLAYABLE;
#else
        (void)incoming;
        (void)current;
        return false;
#endif
    }

#if MDKR_ENABLE_ONLINE_BETA
    /* Test-only (beta): pin that the two SAS re-verify entry points clear the
     * race-scoped peer-loss latches. Static so it can arm the private latch and
     * drive the private entry point on a mesh-free, lobby-free local adapter,
     * then report the drain-facing race_peer_lost() the next race's start
     * barrier would read. `viaAbort` arms raceAbortReceived_ instead of
     * racePeerLost_ (both fold into race_peer_lost()). Arming the latch to true
     * first is what makes this a real regression test: without the clear, the
     * entry point leaves it set and race_peer_lost() stays true. */
    static bool testRekeyClearsPeerLoss(bool viaAbort) {
        MdkrOnlineLiveAdapterOptions o;
        o.sessionId = 1u;
        LiveAdapter a(o);
        if (viaAbort) a.raceAbortReceived_ = true; else a.racePeerLost_ = true;
        a.forcePhraseRekey();
        return a.racePeerLost();
    }
    static bool testReVerifyClearsPeerLoss(bool viaAbort) {
        MdkrOnlineLiveAdapterOptions o;
        o.sessionId = 1u;
        LiveAdapter a(o);
        a.phraseConfirmed_ = true; /* beginReVerify only re-verifies a CONFIRMED
                                    * phrase -- the bug's first-race scenario. */
        if (viaAbort) a.raceAbortReceived_ = true; else a.racePeerLost_ = true;
        a.beginReVerify();
        return a.racePeerLost();
    }
#endif

    /* A race-end card fronts while the local session is still mid-race
     * (RACE_CHROME / engine RACING), where the reducer refuses the card's
     * PLAY_HERE -> RETURN_HOME. Walk the abandoned race's engine out of RACING
     * (the beginReVerify precedent) so the card's primary is ACCEPTED, drop the
     * lobby, and keep view() fronting the card over any late snapshot -- these
     * are dead-ends (no RACING -> LOBBY path yet), so recovery builds from the
     * failure alone. Owns no failure_ write: the caller decides which card shows.
     * Shared by setRaceEndFailure and the publish-failed keep-the-card path. */
    void makeRaceEndCardActionable() {
        if (session_.state.engine == MDKR_ENGINE_RACING) {
            (void)sessionDispatch(MDKR_SESSION_COMMAND_SET_ENGINE_PHASE,
                                  MDKR_ENGINE_FINISHED);
        }
        haveLobby_ = false;
        /* A late State snapshot can re-latch haveLobby_ under this card;
         * keep view() suppressing that stale lobby until the room resets. */
        raceEndFailureLatched_ = true;
        bump();
    }

    /* Route a race-scoped recovery failure onto the lobby-facing view after the
     * engine session ends (see the header). Race-scoped, so resetRaceLatches
     * clears it when the room returns to LOBBY. */
    void setRaceEndFailure(MdkrOnlineViewFailure failure) {
        makeRaceEndCardActionable();
        raceLossFailureLatched_ = false; /* explicit card, not a loss-mapped one */
        /* Never let the drain's generic OPPONENT_LEFT overwrite a more
         * specific mid-race breakdown the mesh already latched. */
        if (!raceEndFailureDemotes(failure, failure_)) failure_ = failure;
        bump();
    }

    /* After a genuine finish is published despite a late peer drop, clear
     * ONLY the in-race loss-mapped failure latch (mapLostReason set failure_ on
     * the same PeerLost that ended the session), so the RESULTS phase fronts
     * instead of a misleading "Lost connection" card. Leaves any unrelated
     * failure (e.g. a genuine VERIFICATION_MISMATCH) untouched. */
    void clearRaceLossFailure() {
        if (!raceLossFailureLatched_) return;
        failure_ = MDKR_ONLINE_VIEW_FAILURE_NONE;
        raceLossFailureLatched_ = false;
        raceEndFailureLatched_ = false; /* card cleared: stop suppressing the lobby */
        bump();
    }

    /* Tell every reachable peer that we are aborting the race start, so a
     * slow-but-alive opponent stops waiting on our primed tick-1 fan-out and
     * never races our frozen input to the flag (nor publishes fabricated
     * placements). Best-effort broadcast on the reliable control channel; a
     * peer that is already gone simply is not reached. */
    void raceSendAbort() {
        if (mesh_) (void)mesh_->sendRaceAbort();
    }

    /* Seal + fan out this endpoint's local input for `newestTick` and the two
     * ticks before it (the 3-frame redundancy window). Deterministic from the
     * local script, so a retransmit re-derives byte-identical frames. */
    void sendLocalBundle(uint32_t newestTick) {
        if (!mesh_) return;
        const uint8_t localMask = raceTransport_.local_slot_mask;
        MdkrMatchInputBundle bundle;
        std::memset(&bundle, 0, sizeof(bundle));
        bundle.match_epoch = raceEpoch_;
        bundle.newest_tick = newestTick;
        bundle.frame_count = MDKR_MATCH_INPUT_BUNDLE_FRAMES;
        bundle.slot_mask = localMask;
        for (unsigned age = 0u; age < MDKR_MATCH_INPUT_BUNDLE_FRAMES; ++age) {
            if (age > bundle.newest_tick) break; /* no tick below 0 */
            const uint32_t frameTick = bundle.newest_tick - age;
            for (unsigned slot = 0u; slot < MDKR_SESSION_MAX_PLAYERS; ++slot) {
                if ((localMask & (1u << slot)) == 0u) continue;
                bundle.frames[age][slot] =
                    localInputForTick(static_cast<uint8_t>(slot), frameTick);
            }
        }
        uint8_t bytes[MDKR_MATCH_INPUT_BUNDLE_BYTES];
        if (mdkr_match_input_bundle_encode(&bundle, bytes, sizeof(bytes))) {
            (void)mesh_->sendInput(bytes);
        }
    }

    /* Retransmit the local input covering an authored tick, so the lossy state
     * channel (maxRetransmits 0) cannot permanently wedge the peer's contiguous
     * confirmation on a single dropped datagram. */
    bool raceSendInputForTick(uint32_t newestTick) {
        if (!raceReady_ || !mesh_) return false;
        sendLocalBundle(newestTick);
        return true;
    }

    /* Seal + fan out the OPENING input window (ticks firstTick..firstTick+
     * inputDelay) without draining anything. The race-start barrier calls this
     * before waiting for the peer's first bundle: without it, each endpoint's
     * first seal only happens inside its first drain — which the barrier
     * blocks — and the two machines deadlock into their timeouts. First-write-
     * wins seal history keeps the later real drains byte-identical to what
     * was primed here. Idempotent; safe to call repeatedly while waiting. */
    bool racePrimeStart() {
        if (!raceReady_ || !mesh_) return false;
        for (uint32_t t = raceNextTick_; t <= raceNextTick_ + raceInputDelay_;
             ++t) {
            recordLocalInput(t);
        }
        sendLocalBundle(raceNextTick_ + raceInputDelay_);
        return true;
    }

    bool raceAdvance() {
        if (!raceReady_ || !mesh_) return false;
        raceSendOwned_ = true; /* production loop shape: sweep may assist */
        /* Seal this endpoint's currently-staged local input for the future tick
         * before fanning it out, so the peer's copy and our own later drain of
         * that tick commit the identical frame. */
        recordLocalInput(raceNextTick_ + raceInputDelay_);
        sendLocalBundle(raceNextTick_ + raceInputDelay_);
        /* Drain the current authored tick with this endpoint's local seats in
         * ascending canonical-slot order (the bridge's frozen local order). The
         * committed local frame is the one sealed inputDelay ticks ago. */
        const uint8_t localMask = raceTransport_.local_slot_mask;
        MdkrPadSample local[MDKR_SESSION_MAX_PLAYERS];
        unsigned localCount = 0u;
        for (unsigned slot = 0u; slot < MDKR_SESSION_MAX_PLAYERS; ++slot) {
            if ((localMask & (1u << slot)) == 0u) continue;
            local[localCount++] =
                localInputForTick(static_cast<uint8_t>(slot), raceNextTick_);
        }
        if (!mdkr_match_transport_drain_tick(&raceTransport_, raceEpoch_,
                                             raceNextTick_, local, localCount)) {
            return false;
        }
        ++raceNextTick_;
        return true;
    }

    /* Drain the current authored tick with this endpoint's local seats but do
     * NOT seal/fan out any bundle (the send half of raceAdvance is skipped).
     * The O2.2-sim impairment matrix uses this to keep the launcher-side engine
     * advancing in real time -- predicting through a network stall -- while it
     * routes every mesh transmission through a seeded net_impairment carrier. */
    bool raceDrainLocal() {
        if (!raceReady_) return false;
        raceSendOwned_ = false; /* the driver owns every transmission: the
                                 * resend sweep must not bypass its carrier */
        /* Record (but do not fan out) this tick's staged local input so a later
         * raceSendInputForTick retransmit re-derives the identical frame. */
        recordLocalInput(raceNextTick_ + raceInputDelay_);
        const uint8_t localMask = raceTransport_.local_slot_mask;
        MdkrPadSample local[MDKR_SESSION_MAX_PLAYERS];
        unsigned localCount = 0u;
        for (unsigned slot = 0u; slot < MDKR_SESSION_MAX_PLAYERS; ++slot) {
            if ((localMask & (1u << slot)) == 0u) continue;
            local[localCount++] =
                localInputForTick(static_cast<uint8_t>(slot), raceNextTick_);
        }
        if (!mdkr_match_transport_drain_tick(&raceTransport_, raceEpoch_,
                                             raceNextTick_, local, localCount)) {
            return false;
        }
        ++raceNextTick_;
        return true;
    }

    /* ---- Local controller input for the race ------------------------------ */

    static MdkrPadSample neutralSample() {
        MdkrPadSample s;
        s.buttons = 0u;
        s.stick_x = 0;
        s.stick_y = 0;
        s.present = 1u;
        return s;
    }

    /* Transport/rollback test seams call this to drive the race from the
     * deterministic raceLocalSample fixture (per-tick variation that forces real
     * corrections). The shipped interactive boot never enables it. */
    void raceSetSyntheticInput(bool on) { raceSyntheticInput_ = on; }

    /* Stage the real local pads for the next seal. `local` is in local-seat
     * order (seat i reads controller port i), matching the physical[] the engine
     * hands the drain callback. No-op effect in synthetic mode. */
    void raceSetLocalInput(const MdkrPadSample *local, unsigned count) {
        localPendingCount_ = 0u;
        if (local == nullptr) return;
        for (unsigned i = 0u; i < count && i < MDKR_SESSION_MAX_PLAYERS; ++i) {
            localPending_[localPendingCount_++] = local[i];
        }
    }

    /* Record the currently-staged real local pads as the committed input for
     * `sealTick`, mapping each local canonical slot to its local-seat index.
     * No-op in synthetic mode (raceLocalSample is recomputed on demand). */
    void recordLocalInput(uint32_t sealTick) {
        if (raceSyntheticInput_) return;
        const uint8_t localMask = raceTransport_.local_slot_mask;
        const size_t idx = static_cast<size_t>(sealTick % kLocalInputRing);
        /* First write wins: once a tick's frame has been sealed (and possibly
         * fanned out — racePrimeStart seals the opening window before the
         * first drain), every later seal and retransmit of that tick must be
         * byte-identical, or the peer would commit a different frame than we
         * retain and the confirmed input histories diverge permanently. */
        if (localHistTick_[idx] == sealTick) return;
        localHistTick_[idx] = sealTick;
        unsigned localIndex = 0u;
        for (unsigned slot = 0u; slot < MDKR_SESSION_MAX_PLAYERS; ++slot) {
            if ((localMask & (1u << slot)) == 0u) continue;
            localHist_[idx][slot] =
                (localIndex < localPendingCount_) ? localPending_[localIndex]
                                                  : neutralSample();
            ++localIndex;
        }
    }

    /* The committed local input for `tick`, canonical-slot indexed. Synthetic
     * mode returns the deterministic fixture; production returns the recorded
     * real pad, or a neutral present frame for a tick with no recorded input
     * (the first inputDelay ticks, or a headless run that never injects). */
    MdkrPadSample localInputForTick(uint8_t canonicalSlot, uint32_t tick) {
        if (raceSyntheticInput_) return raceLocalSample(canonicalSlot, tick);
        const size_t idx = static_cast<size_t>(tick % kLocalInputRing);
        if (localHistTick_[idx] == tick) return localHist_[idx][canonicalSlot];
        return neutralSample();
    }

    bool raceInputsForTick(uint32_t tick, MdkrInputSet *out) {
        if (!raceReady_) return false;
        return mdkr_match_transport_inputs_for_tick(&raceTransport_, raceEpoch_,
                                                    tick, out);
    }

    /* Engine match-input seam (O-T6b): the two transport views the engine's
     * canonical input provider needs beyond drain/inputs_for_tick. */
    bool raceTakeDirty(uint32_t *tick) {
        if (!raceReady_ || tick == nullptr) return false;
        return mdkr_match_transport_take_dirty(&raceTransport_, tick);
    }

    bool raceAiMask(uint32_t tick, uint8_t *slotMask) const {
        if (!raceReady_ || slotMask == nullptr) return false;
        /* No mid-race AI handback exists on the 2-endpoint beta path, so this is
         * almost always mask 0; routing it through the transport keeps the seam
         * honest if a takeover is ever scheduled. */
        return mdkr_match_transport_ai_takeover_mask_for_tick(
            &raceTransport_, raceEpoch_, tick, slotMask);
    }

    /* True once every REMOTE canonical slot's input for `tick` has been received
     * into the transport history -- independent of the drain frontier, which is
     * why it reads the public net_input cell directly rather than
     * inputs_for_tick (that refuses ticks ahead of current_tick). The in-process
     * proof polls this before draining so the visible engine commits confirmed
     * input, exactly like main_app.cpp's loopback simulator did synchronously,
     * and never rolls back into the paused race countdown (which the engine
     * rejects). remote_slot_mask == 0 (no peers) trivially returns true. */
    bool raceRemoteReceivedForTick(uint32_t tick) const {
        if (!raceReady_) return false;
        const uint8_t remote = raceTransport_.remote_slot_mask;
        if (remote == 0u) return true;
        const MdkrNetInputCell *cell =
            &raceTransport_.history.cells[tick % MDKR_NET_INPUT_CAPACITY];
        if (!cell->occupied || cell->tick != tick) return false;
        for (unsigned slot = 0u; slot < MDKR_NET_INPUT_SLOTS; ++slot) {
            const uint8_t bit = static_cast<uint8_t>(1u << slot);
            if ((remote & bit) == 0u) continue;
            if (cell->status[slot] != MDKR_NET_INPUT_RECEIVED) return false;
        }
        return true;
    }

    void raceStats(MdkrOnlineLiveRaceStats *out) const {
        out->inputEnvelopesReceived = inputEnvelopes_;
        if (mesh_) {
            const MdkrMatchPeerMeshStats m = mesh_->stats();
            out->meshRejectedState = m.rejectedStateEnvelopes;
            out->meshIgnoredStaleSignals = m.ignoredStaleSignals;
            out->meshDroppedEvents = m.droppedEvents;
        }
        const MdkrMatchTransportStats *t =
            mdkr_match_transport_stats(&raceTransport_);
        if (t) {
            out->transportAccepted = t->accepted;
            out->transportCorrected = t->corrected;
            out->transportDuplicates = t->duplicates;
            out->transportOutOfWindow = t->out_of_window;
            out->transportDrained = t->drained;
        }
        MdkrMatchRecovery rec;
        if (mdkr_match_transport_recovery(&raceTransport_, &rec)) {
            out->recoveryReason = static_cast<uint32_t>(rec.reason);
            out->recoveryFirstTick = rec.first_unrecoverable_tick;
            out->recoveryObservedTick = rec.observed_at_tick;
            out->recoverySlot = rec.canonical_slot;
        }
        out->resendSweeps = raceResendSweeps_;
        out->resendBundles = raceResendBundles_;
    }

    /* ---- Race-results handoff from the launcher --------------------------- */
    bool reportResults(const uint8_t placements[4]) {
        if (placements == nullptr || !haveLobby_) return false;
        if (lobby_.phase == MDKR_ONLINE_RESULTS) return true; /* already in */
        if (lobby_.phase != MDKR_ONLINE_RACING) return false;
        if (resultsReported_) return true;
        /* Map canonical slots onto SEAT indices from the authoritative
         * roster: canonical slot k is the k-th OCCUPIED seat in seat order --
         * exactly the order manifestFromLobby froze slot_owner[] with. For
         * the 2-endpoint beta this is the identity map, but it is derived,
         * never assumed. */
        uint32_t packed = 0u;
        unsigned canonical = 0u;
        for (unsigned seat = 0u; seat < MDKR_ONLINE_MAX_SEATS; ++seat) {
            uint8_t placement = 0xFFu; /* MDKR_ONLINE_NO_PLACEMENT */
            if (lobby_.seats[seat].occupied) {
                if (canonical >= 4u) return false;
                placement = placements[canonical++];
                if (placement >= MDKR_ONLINE_PLACEMENT_COUNT) {
                    MDKR_ONLINE_LOG(
                        "[ONLINE] report_results refused: occupied seat %u "
                        "carries no placement (0x%02x)\n", seat, placement);
                    return false;
                }
            }
            packed |= static_cast<uint32_t>(placement) << (seat * 8u);
        }
        if (!isLeader()) {
            /* The RESULTS lobby phase arrives via snapshot from the leader's
             * PUBLISH_RESULTS; followLobbyPhase walks the local session to
             * the results scene from it. Nothing to send. */
            MDKR_ONLINE_LOG(
                "[ONLINE] report_results (joiner): awaiting leader's "
                "PUBLISH_RESULTS snapshot\n");
            resultsReported_ = true;
            return true;
        }
        resultsReported_ = sendLobbyCommand(MDKR_ONLINE_PUBLISH_RESULTS, 0u,
                                            packed);
        MDKR_ONLINE_LOG(
            "[ONLINE] report_results (leader): PUBLISH_RESULTS value=0x%08x "
            "sent=%u epoch=%u\n",
            packed, resultsReported_ ? 1u : 0u, lobby_.match_epoch);
        return resultsReported_;
    }

    /* ---- Leader-only session configuration -------------------------------- */
    bool sendSessionConfig(MdkrOnlineCommandType type, uint32_t value) {
        if (!haveLobby_ || lobby_.phase != MDKR_ONLINE_LOBBY || !isLeader()) {
            return false;
        }
        /* Mirror the reducer's acceptance gates so an unacceptable press is an
         * immediate local refusal, not an async one. Both reducers accept
         * SET_CONFIG_TRACK only for one of the 20 standard race ids (lobby_core's
         * known_race_track / the service's RACE_TRACK_IDS), so reject anything
         * outside that set here too -- an earlier refusal only, never a new
         * acceptance. The >255 guard both matches the reducers' range and keeps
         * the uint16 track-table lookup from truncating a large value into a
         * false hit; the id set (max 33) is well within it. */
        if ((type == MDKR_ONLINE_SET_MODE &&
             value > MDKR_ONLINE_MODE_TOURNAMENT) ||
            (type == MDKR_ONLINE_SET_CONFIG_TRACK &&
             (value > 255u ||
              mdkr_online_track_by_id(static_cast<uint16_t>(value)) == nullptr)) ||
            (type == MDKR_ONLINE_SET_CUP && value >= MDKR_ONLINE_CUP_COUNT)) {
            return false;
        }
        const bool sent = sendLobbyCommand(type, 0u, value);
        MDKR_ONLINE_LOG("[ONLINE] %s value=%u sent=%u\n",
                        lobbyCommandName(type), value, sent ? 1u : 0u);
        if (sent) bump();
        return sent;
    }

    bool lobbySnapshot(MdkrOnlineLobby *out) const {
        if (out == nullptr || !haveLobby_) return false;
        *out = lobby_;
        return true;
    }

    /* Synchronous handoff retract for the owner (launcher thread). */
    bool retractRaceBoot() {
#if MDKR_ENABLE_ONLINE_BETA
        OnlineRoom_retractEngineRaceBoot(this);
#endif
        return true;
    }

private:

    /* ---- State -------------------------------------------------------- */
    MdkrOnlineLiveAdapterOptions opts_;
    MdkrSessionCore session_{};
    MdkrOnlineLobby lobby_{};
    bool haveLobby_ = false;
    uint64_t localEndpointId_ = 0u;
    MdkrOnlineJourney journey_ = MDKR_ONLINE_JOURNEY_CREATE;
    MdkrOnlineViewFailure failure_ = MDKR_ONLINE_VIEW_FAILURE_NONE;
    bool inviteReady_ = false;
    bool havePhrase_ = false;
    bool phraseConfirmed_ = false;
    /* SAS re-verify barrier latch: armed by beginReVerify() after a
     * post-confirmation rekey, cleared by the second CONFIRM_PHRASE. While
     * armed, followLobbyPhase() leaves the room at the re-verify surface. */
    bool reVerify_ = false;
    /* The 256-bit transcript digest the human confirmed. Re-verify detection
     * keys on THIS, never the 20-bit phrase: two different transcripts
     * collide to the same phrase with probability ~2^-20, so a strcmp on the
     * SAS is grindable, but a full-digest compare arms the barrier on ANY
     * transcript change. Captured at CONFIRM_PHRASE; compared every time a
     * fresh phrase is derived. */
    bool haveConfirmedDigest_ = false;
    uint8_t confirmedDigest_[MDKR_MATCH_PEER_TRANSCRIPT_DIGEST_BYTES] = {};
    char phrase_[MDKR_ONLINE_VERIFICATION_PHRASE_BYTES] = {};
    uint32_t revision_ = 1u;
    uint64_t nextCommandId_ = 1u;
    Pending pending_ = Pending::None;
    /* One-shot advisory carried on the NEXT accepted step (currently only the
     * ENTER_ANOTHER_CODE rebuild sentinel the header documents). */
    uint32_t stepNote_ = 0u;
    /* SAS-mismatch rekey: countdown (in service() calls) between the
     * control-channel mismatch notice and the mesh teardown, so the sealed
     * notice flushes before its channel dies; the active flag suppresses the
     * teardown's own PeerLost from repainting the mismatch surface. */
    unsigned phraseRekeyCountdown_ = 0u;
    bool phraseMismatchActive_ = false;
    bool phraseNoticePending_ = false;
    unsigned phraseNoticeTries_ = 0u;
    static constexpr unsigned kPhraseRekeyDelayServices = 3u;
    static constexpr unsigned kPhraseNoticeMaxTries = 300u;
    /* Last lobby command, for launcher-owned stale-revision re-send. */
    MdkrOnlineCommandType lastType_ = MDKR_ONLINE_JOIN;
    uint32_t lastSeat_ = 0u;
    uint32_t lastValue_ = 0u;
    bool haveLast_ = false;
    /* In-flight command correlation: command_id -> the command sent under it,
     * so a CommandResult refusal is attributed to the command the server
     * actually answered (two can be in flight) rather than to lastType_.
     * Bounded in sendLobbyCommandRaw; entries consumed as their results
     * drain. */
    std::map<uint64_t, SentCommand> inFlight_;
    /* Stale-refused commands whose refusal arrived while lobby_ still holds
     * the very revision the send carried (the production ordering: the HTTP
     * CommandResult beats the WS State that explains the staleness). A blind
     * immediate re-send would carry the SAME stale revision and be refused
     * again -- the retry churn the first real two-peer run drowned in --
     * so these wait for the next authoritative State, get re-checked against
     * it (often the effect has landed via the concurrent peer's interleaving
     * and nothing needs sending), and re-send at most ONE per State so a
     * chain of parked commands drains in order, one accepted revision at a
     * time. Bounded two ways -- BOTH pinned by dedicated unit tests in
     * tests/test_online_live_adapter.cpp (keep the literals in lock-step with
     * those pins):
     *   - kStaleRetryCap: a PER-COMMAND re-send budget (SentCommand::retries)
     *     that unrelated fresh traffic can never reset -- a persistently
     *     stale-refused command terminates in a surfaced refusal even on a
     *     live room under continuous cross-type intents;
     *   - kStaleParkedMaxServices: the service-call age-out for a room that
     *     stops delivering States at all -- the counter accumulates while ANY
     *     entry remains parked (only an EMPTY queue resets it), so periodic
     *     states that never resolve the head cannot hold the age-out off. */
    std::deque<SentCommand> staleParked_;
    unsigned staleParkedAgeServices_ = 0u;
    /* Consecutive stale refusals attributed via the NO-ECHO fallback (the
     * server returned no command id): stands in for the per-command budget
     * on a transport that never echoes, reset by any accepted result. All
     * shipped transports echo ids; this is the belt-and-braces bound. */
    unsigned noEchoStaleRetries_ = 0u;
    static constexpr unsigned kStaleRetryCap = 8u;
    static constexpr unsigned kStaleParkedMaxServices = 300u; /* ~10 s @30Hz */
    std::function<uint64_t()> nowMs_;
    std::vector<MdkrMatchPeerIceServer> iceServers_;
    std::string roomIdStr_;
    std::string credential_;
    std::vector<MdkrOnlineRoomEvent> roomEvents_;

    /* Mesh. The connection generation is ADOPTED from the signal service's
     * welcome (localGeneration 0 to the mesh); the constant 1 is never assumed
     * against real signaling. It is read once the phrase is ready and bound
     * into the graph and the local attestation. */
    uint32_t meshGeneration_ = 0u;
    uint32_t meshEpoch_ = 1u;
    std::vector<MdkrMatchPeerSlotOwner> meshRoster_;
    std::unique_ptr<MdkrMatchPeerMesh> mesh_;
    bool meshUp_ = false;
    std::set<uint64_t> channelsReady_;
    std::vector<MdkrMatchPeerMeshEvent> meshEvents_;
    uint64_t inputEnvelopes_ = 0u;

    /* Race runtime (post-install): the launcher-side match transport bound to a
     * headless session bridge, plus the per-peer authenticated slot masks used
     * to authorize opened INPUT envelopes into mdkr_match_transport_receive. */
    MdkrSessionBridge raceBridge_{};
    MdkrMatchTransport raceTransport_{};
    bool raceReady_ = false;
    uint32_t raceEpoch_ = 0u;
    uint32_t raceFirstTick_ = 1u;
    uint32_t raceNextTick_ = 1u;
    uint8_t raceInputDelay_ = 2u;
    std::map<uint64_t, uint8_t> peerSlotMask_;
    /* Once-per-epoch lobby loading handshake latches. */
    bool ackLoadedSent_ = false;
    bool beginRaceSent_ = false;
    bool resultsReported_ = false;
    /* Resend sweep + connection quality; peer-lost flag. */
    bool raceSendOwned_ = false;   /* true while race_advance drives the send */
    bool raceDegraded_ = false;
    bool racePeerLost_ = false;
    bool raceAbortReceived_ = false; /* peer told us it aborted the race */
    bool raceLossFailureLatched_ = false; /* failure_ came from mapLostReason */
    bool raceEndFailureLatched_ = false;  /* suppress stale lobby under a
                                           * race-end recovery card */
    unsigned raceSweepServiceCalls_ = 0u;
    uint32_t raceResendSweeps_ = 0u;
    uint32_t raceResendBundles_ = 0u;
    static constexpr unsigned kRaceSweepServicePeriod = 30u;
    static constexpr uint32_t kRaceSweepWindow = 60u;

    /* Real local controller input for the race (see raceSetLocalInput /
     * localInputForTick / recordLocalInput). raceSyntheticInput_ selects the
     * deterministic raceLocalSample fixture instead -- used ONLY by the
     * transport/rollback test seams (which need per-tick input variation to
     * force real corrections); the shipped interactive boot leaves it false and
     * drives the race from the physical pad. The ring records each sealed tick's
     * committed local input so the bundle sent to the peer, this endpoint's own
     * later drain of that tick, and any retransmit all agree byte-for-byte. */
    bool raceSyntheticInput_ = false;
    static constexpr uint32_t kLocalInputRing = 512u;
    MdkrPadSample localPending_[MDKR_SESSION_MAX_PLAYERS] = {};
    unsigned localPendingCount_ = 0u;
    MdkrPadSample localHist_[kLocalInputRing][MDKR_SESSION_MAX_PLAYERS] = {};
    uint32_t localHistTick_[kLocalInputRing] = {};

    /* Loading barrier + preflight */
    bool loadingBuildDone_ = false;
    bool descriptorBuilt_ = false;
    MdkrMatchLaunchRefusal refusal_ = MDKR_MATCH_LAUNCH_ADMITTED;
    MdkrMatchLaunchDescriptorV1 descriptor_{};
    bool preflightInit_ = false;
    MdkrMatchPreflightV1 preflight_{};
    bool ownSubmitted_ = false;
    uint64_t lastFragmentSendMs_ = 0u; /* 1 Hz idempotent fragment re-send */
    bool preflightReady_ = false;
    bool installed_ = false;
    MdkrMatchPeerGraph graph_{};
    std::map<uint64_t, MdkrMatchPreflightFragmentState> fragStates_;
    std::map<uint64_t, MdkrMatchPreflightAttestationV1> pendingPeerAtts_;

    /* Diagnostics + non-silent view timeout (see logPhaseAndTimeoutAnchor /
     * timeoutExpired). lastPhaseKey_ de-dups the [ROOM-PHASE] spine;
     * lastPreflightGate_ de-dups the [PREFLIGHT] gate note; the *Logged_ flags
     * de-dup the one-shot [LOADING]/[START] milestones. */
    uint64_t lastPhaseKey_ = UINT64_MAX;
    uint64_t viewAnchorMs_ = 0u;
    uint64_t viewTimeoutMs_ = 30000u;
    /* Lobby revision the timeout anchor was last re-armed on while SELECTING,
     * so active picking (each pick/ready/settings change bumps it) never lets the
     * "Selection Took Too Long" card false-fire. */
    uint32_t lastAnchorRevision_ = 0u;
    int lastPreflightGate_ = -1;
    bool preflightInitLogged_ = false;
    bool raceReadyLogged_ = false;

    /* One-shot async command-refusal surface for the panel (e.g. the room
     * refused SET_CHARACTER with SELECTION_CONFLICT after the optimistic local
     * pick). Set by the CommandResult drain, consumed by takeRefusal(). */
    uint32_t refusalType_ = 0u;
    uint32_t refusalError_ = 0u;
    bool haveRefusal_ = false;

public:
    bool takeRefusal(uint32_t *type, uint32_t *error) {
        if (!haveRefusal_) return false;
        if (type != nullptr) *type = refusalType_;
        if (error != nullptr) *error = refusalError_;
        haveRefusal_ = false;
        return true;
    }

private:
};

std::unique_ptr<IMdkrOnlineAdapter> mdkr_online_live_adapter_create(
    const MdkrOnlineLiveAdapterOptions &options, std::string *error) {
    if (options.room == nullptr || options.meshBackend == nullptr) {
        if (error) *error = "live adapter requires a room transport and mesh backend";
        return nullptr;
    }
    if (options.journey < MDKR_ONLINE_JOURNEY_CREATE ||
        options.journey > MDKR_ONLINE_JOURNEY_REMATCH ||
        !mdkr_online_compatibility_valid(&options.compatibility)) {
        if (error) *error = "invalid live adapter options";
        return nullptr;
    }
    return std::unique_ptr<IMdkrOnlineAdapter>(new LiveAdapter(options));
}

bool mdkr_online_live_adapter_probe(const IMdkrOnlineAdapter *adapter,
                                    MdkrOnlineLiveLaunchProbe *out) {
    if (adapter == nullptr || out == nullptr) return false;
    const LiveAdapter *live = adapter->mdkrResolveLive();
    if (live == nullptr) return false;
    live->fillProbe(out);
    return true;
}

bool mdkr_online_live_adapter_race_info(const IMdkrOnlineAdapter *adapter,
                                        MdkrOnlineLiveRaceInfo *out) {
    if (adapter == nullptr || out == nullptr) return false;
    const LiveAdapter *live = adapter->mdkrResolveLive();
    if (live == nullptr) return false;
    live->raceInfo(out);
    return true;
}

bool mdkr_online_live_adapter_race_peer_lost(const IMdkrOnlineAdapter *adapter) {
    if (adapter == nullptr) return false;
    const LiveAdapter *live = adapter->mdkrResolveLive();
    return live != nullptr && live->racePeerLost();
}

bool mdkr_online_live_adapter_set_race_end_failure(
    IMdkrOnlineAdapter *adapter, MdkrOnlineViewFailure failure) {
    if (adapter == nullptr) return false;
    LiveAdapter *live = adapter->mdkrResolveLive();
    if (live == nullptr) return false;
    live->setRaceEndFailure(failure);
    return true;
}

bool mdkr_online_live_adapter_walk_engine_out_of_race(
    IMdkrOnlineAdapter *adapter) {
    if (adapter == nullptr) return false;
    LiveAdapter *live = adapter->mdkrResolveLive();
    if (live == nullptr) return false;
    live->makeRaceEndCardActionable();
    return true;
}

#if MDKR_ENABLE_ONLINE_BETA
/* Test-only (beta): expose the two pure decisions that govern race-end card
 * truthfulness -- the peer-loss -> failure mapping and the no-demotion rule
 * -- so the beta unit test pins them without driving a full loopback mesh. Not
 * part of the launcher API; only compiled in a beta build. */
MdkrOnlineViewFailure mdkr_online_live_adapter_test_map_lost_reason(
    MdkrMatchPeerLostReason lostReason, bool raceBegun) {
    return LiveAdapter::mapLostReason(lostReason, raceBegun);
}

bool mdkr_online_live_adapter_test_race_end_demotes(
    MdkrOnlineViewFailure incoming, MdkrOnlineViewFailure current) {
    return LiveAdapter::raceEndFailureDemotes(incoming, current);
}

bool mdkr_online_live_adapter_test_rekey_clears_peer_loss(bool via_abort) {
    return LiveAdapter::testRekeyClearsPeerLoss(via_abort);
}

bool mdkr_online_live_adapter_test_reverify_clears_peer_loss(bool via_abort) {
    return LiveAdapter::testReVerifyClearsPeerLoss(via_abort);
}
#endif

bool mdkr_online_live_adapter_race_send_abort(IMdkrOnlineAdapter *adapter) {
    if (adapter == nullptr) return false;
    LiveAdapter *live = adapter->mdkrResolveLive();
    if (live == nullptr) return false;
    live->raceSendAbort();
    return true;
}

bool mdkr_online_live_adapter_clear_race_loss_failure(
    IMdkrOnlineAdapter *adapter) {
    if (adapter == nullptr) return false;
    LiveAdapter *live = adapter->mdkrResolveLive();
    if (live == nullptr) return false;
    live->clearRaceLossFailure();
    return true;
}

bool mdkr_online_live_adapter_race_advance(IMdkrOnlineAdapter *adapter) {
    if (adapter == nullptr) return false;
    LiveAdapter *live = adapter->mdkrResolveLive();
    return live != nullptr && live->raceAdvance();
}

bool mdkr_online_live_adapter_race_set_synthetic_input(
    IMdkrOnlineAdapter *adapter, bool on) {
    if (adapter == nullptr) return false;
    LiveAdapter *live = adapter->mdkrResolveLive();
    if (live == nullptr) return false;
    live->raceSetSyntheticInput(on);
    return true;
}

bool mdkr_online_live_adapter_race_set_local_input(
    IMdkrOnlineAdapter *adapter, const MdkrPadSample *local, unsigned count) {
    if (adapter == nullptr) return false;
    LiveAdapter *live = adapter->mdkrResolveLive();
    if (live == nullptr) return false;
    live->raceSetLocalInput(local, count);
    return true;
}

bool mdkr_online_live_adapter_race_resend(IMdkrOnlineAdapter *adapter,
                                          uint32_t newestTick) {
    if (adapter == nullptr) return false;
    LiveAdapter *live = adapter->mdkrResolveLive();
    return live != nullptr && live->raceSendInputForTick(newestTick);
}

bool mdkr_online_live_adapter_race_drain_local(IMdkrOnlineAdapter *adapter) {
    if (adapter == nullptr) return false;
    LiveAdapter *live = adapter->mdkrResolveLive();
    return live != nullptr && live->raceDrainLocal();
}

bool mdkr_online_live_adapter_race_inputs_for_tick(IMdkrOnlineAdapter *adapter,
                                                   uint32_t tick,
                                                   MdkrInputSet *out) {
    if (adapter == nullptr || out == nullptr) return false;
    LiveAdapter *live = adapter->mdkrResolveLive();
    return live != nullptr && live->raceInputsForTick(tick, out);
}

bool mdkr_online_live_adapter_race_stats(const IMdkrOnlineAdapter *adapter,
                                         MdkrOnlineLiveRaceStats *out) {
    if (adapter == nullptr || out == nullptr) return false;
    const LiveAdapter *live = adapter->mdkrResolveLive();
    if (live == nullptr) return false;
    live->raceStats(out);
    return true;
}

bool mdkr_online_live_adapter_race_take_dirty(IMdkrOnlineAdapter *adapter,
                                              uint32_t *tick) {
    if (adapter == nullptr || tick == nullptr) return false;
    LiveAdapter *live = adapter->mdkrResolveLive();
    return live != nullptr && live->raceTakeDirty(tick);
}

bool mdkr_online_live_adapter_race_ai_mask(IMdkrOnlineAdapter *adapter,
                                           uint32_t tick, uint8_t *slot_mask) {
    if (adapter == nullptr || slot_mask == nullptr) return false;
    LiveAdapter *live = adapter->mdkrResolveLive();
    return live != nullptr && live->raceAiMask(tick, slot_mask);
}

bool mdkr_online_live_adapter_race_remote_ready(IMdkrOnlineAdapter *adapter,
                                                uint32_t tick) {
    if (adapter == nullptr) return false;
    const LiveAdapter *live = adapter->mdkrResolveLive();
    return live != nullptr && live->raceRemoteReceivedForTick(tick);
}

bool mdkr_online_live_adapter_report_results(IMdkrOnlineAdapter *adapter,
                                             const uint8_t placements[4]) {
    if (adapter == nullptr || placements == nullptr) return false;
    LiveAdapter *live = adapter->mdkrResolveLive();
    return live != nullptr && live->reportResults(placements);
}

bool mdkr_online_live_adapter_set_mode(IMdkrOnlineAdapter *adapter,
                                       unsigned mode) {
    if (adapter == nullptr) return false;
    LiveAdapter *live = adapter->mdkrResolveLive();
    return live != nullptr &&
           live->sendSessionConfig(MDKR_ONLINE_SET_MODE,
                                   static_cast<uint32_t>(mode));
}

bool mdkr_online_live_adapter_set_config_track(IMdkrOnlineAdapter *adapter,
                                               unsigned trackId) {
    if (adapter == nullptr) return false;
    LiveAdapter *live = adapter->mdkrResolveLive();
    return live != nullptr &&
           live->sendSessionConfig(MDKR_ONLINE_SET_CONFIG_TRACK,
                                   static_cast<uint32_t>(trackId));
}

bool mdkr_online_live_adapter_set_cup(IMdkrOnlineAdapter *adapter,
                                      unsigned cupId) {
    if (adapter == nullptr) return false;
    LiveAdapter *live = adapter->mdkrResolveLive();
    return live != nullptr &&
           live->sendSessionConfig(MDKR_ONLINE_SET_CUP,
                                   static_cast<uint32_t>(cupId));
}

bool mdkr_online_live_adapter_lobby(const IMdkrOnlineAdapter *adapter,
                                    MdkrOnlineLobby *out) {
    if (adapter == nullptr || out == nullptr) return false;
    const LiveAdapter *live = adapter->mdkrResolveLive();
    return live != nullptr && live->lobbySnapshot(out);
}

bool mdkr_online_live_adapter_retract_race_boot(IMdkrOnlineAdapter *adapter) {
    if (adapter == nullptr) return false;
    LiveAdapter *live = adapter->mdkrResolveLive();
    return live != nullptr && live->retractRaceBoot();
}

bool mdkr_online_live_adapter_take_refusal(IMdkrOnlineAdapter *adapter,
                                           uint32_t *command_type,
                                           uint32_t *error) {
    if (adapter == nullptr) return false;
    LiveAdapter *live = adapter->mdkrResolveLive();
    return live != nullptr && live->takeRefusal(command_type, error);
}

bool mdkr_online_live_adapter_race_prime_start(IMdkrOnlineAdapter *adapter) {
    if (adapter == nullptr) return false;
    LiveAdapter *live = adapter->mdkrResolveLive();
    return live != nullptr && live->racePrimeStart();
}

#if MDKR_ENABLE_ONLINE_BETA
/* Resolve the RAW concrete live adapter behind the panel's owning wrapper (a raw
 * adapter passes through unchanged; the fake resolves to nullptr) via the SAME
 * mdkrResolveLive hook the C accessors use, so the room-ready / race-boot
 * registries key on the identical raw pointer those accessors act on. It lives in
 * THIS translation unit -- not the wiring TU that owns the other OnlineRoom_ seams
 * -- because the LiveAdapter -> IMdkrOnlineAdapter upcast on the result needs
 * LiveAdapter's complete type, which only this TU has. */
IMdkrOnlineAdapter *OnlineRoom_resolveRawLiveAdapter(IMdkrOnlineAdapter *adapter) {
    return adapter != nullptr ? adapter->mdkrResolveLive() : nullptr;
}
#endif

/* OnlineRoom_makeGatedLiveAdapter is a header-inline stub (returns nullptr)
 * so the launcher panel never links this translation unit; the production
 * transport wiring is owned by the O-T6 race lane. */
