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
 * The transcript_digest bound into each attestation is SHA-256 of the mesh's
 * human verification phrase. The phrase is the canonical, human-verified
 * fingerprint of the committed transcript (the mesh re-verifies every
 * commitment before it will emit a phrase), and every honest peer derives the
 * same phrase, so this achieves attestation consensus over the transcript. A
 * future revision may bind the raw transcript digest if the mesh exposes it.
 */
#include "match_live_adapter.h"

#include "net/match_preflight.h"
#include "net/net_roster.h"
#include "net/net_roster_runtime.h"
#include "session/session_core.h"

extern "C" {
#include "sha256.h"
}

#include <chrono>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace {

uint64_t steadyNowMs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
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

class LiveAdapter final : public IMdkrOnlineAdapter {
public:
    explicit LiveAdapter(const MdkrOnlineLiveAdapterOptions &opts)
        : opts_(opts) {
        mdkr_session_core_init(&session_, opts.sessionId);
        journey_ = opts.journey;
        nowMs_ = opts.nowMs ? opts.nowMs : &steadyNowMs;
    }

    ~LiveAdapter() override {
        mesh_.reset(); /* mesh borrows the backend's feed: kill it first */
        if (opts_.meshBackend) opts_.meshBackend->reset();
    }

    /* ---- IMdkrOnlineAdapter ------------------------------------------- */

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
        return step(true, 0u);
    }

    bool view(MdkrOnlineViewModel *out) const override {
        MdkrOnlineViewInput in;
        std::memset(&in, 0, sizeof(in));
        in.session = &session_.state;
        in.lobby = haveLobby_ ? &lobby_ : nullptr;
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
        pumpMesh();
        runLoadingBarrier();
        runPreflight();
    }

    uint32_t revision() const override { return revision_; }
    MdkrPlayIntent sessionIntent() const override {
        return session_.state.intent;
    }
    bool raceAdmissionEnabled() const override {
        return opts_.raceAdmissionEnabled;
    }
    bool timeoutExpired() const override { return false; }

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
        staleRetries_ = 0u;
        return sendLobbyCommandRaw(type, seat, value);
    }

    bool sendLobbyCommandRaw(MdkrOnlineCommandType type, uint32_t seat,
                             uint32_t value) {
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
        return opts_.room && opts_.room->submitCommand(c);
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
            case MDKR_ONLINE_VIEW_ACTION_CONFIRM_PHRASE:
                if (!havePhrase_ ||
                    session_.state.room != MDKR_ROOM_PREFLIGHT) return false;
                if (!sessionDispatch(MDKR_SESSION_COMMAND_SET_ROOM_PHASE,
                                     MDKR_ROOM_SELECTING)) return false;
                phraseConfirmed_ = true;
                return true;
            case MDKR_ONLINE_VIEW_ACTION_REPORT_PHRASE_MISMATCH:
                failure_ = MDKR_ONLINE_VIEW_FAILURE_VERIFICATION_MISMATCH;
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
            case MDKR_ONLINE_VIEW_ACTION_START_RACE:
                /* Leader begins loading; both peers follow lobby.phase LOADING
                 * (syncPhase) into the Loading barrier. */
                return sendLobbyCommand(MDKR_ONLINE_BEGIN_LOADING, 0u, value);
            case MDKR_ONLINE_VIEW_ACTION_RETURN_TO_LOBBY:
                if (!sendLobbyCommand(MDKR_ONLINE_CANCEL_LOADING, 0u, 0u))
                    return false;
                (void)sessionDispatch(MDKR_SESSION_COMMAND_RETURN_TO_LOBBY, 0u);
                journey_ = MDKR_ONLINE_JOURNEY_REMATCH;
                return true;
            case MDKR_ONLINE_VIEW_ACTION_RETRY:
                if (failure_ == MDKR_ONLINE_VIEW_FAILURE_NONE) return false;
                failure_ = MDKR_ONLINE_VIEW_FAILURE_NONE;
                return true;
            case MDKR_ONLINE_VIEW_ACTION_LEAVE_ROOM:
            case MDKR_ONLINE_VIEW_ACTION_PLAY_HERE:
            case MDKR_ONLINE_VIEW_ACTION_CHOOSE_ROM:
            case MDKR_ONLINE_VIEW_ACTION_RETURN_HOME:
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
                    bump();
                    break;
                case MdkrOnlineRoomEvent::Type::State:
                    if (ev.haveLobby) {
                        lobby_ = ev.lobby;
                        haveLobby_ = true;
                        syncPhase();
                        bump();
                    }
                    break;
                case MdkrOnlineRoomEvent::Type::CommandResult:
                    if (ev.step.accepted) {
                        staleRetries_ = 0u;
                    } else if (ev.step.error == MDKR_ONLINE_ERROR_INCOMPATIBLE) {
                        failure_ = MDKR_ONLINE_VIEW_FAILURE_DIFFERENT_BUILD;
                        bump();
                    } else if ((ev.step.error == MDKR_ONLINE_ERROR_STALE_REVISION ||
                                ev.step.error == MDKR_ONLINE_ERROR_STALE_COMMAND) &&
                               haveLast_ && staleRetries_ < 8u) {
                        /* Launcher-owned optimistic-concurrency retry: the room
                         * advanced under us (a concurrent peer). The State
                         * carrying that advance was applied earlier in this same
                         * drain, so re-send against the fresh revision. */
                        ++staleRetries_;
                        (void)sendLobbyCommandRaw(lastType_, lastSeat_, lastValue_);
                    }
                    break;
                case MdkrOnlineRoomEvent::Type::Failure:
                    /* Pre-mapped stable failure; lobby state is retained so a
                     * Retry recovers without dismissing the room. */
                    failure_ = ev.failure != MDKR_ONLINE_VIEW_FAILURE_NONE
                                   ? ev.failure
                                   : MDKR_ONLINE_VIEW_FAILURE_SERVICE_UNAVAILABLE;
                    pending_ = Pending::None;
                    bump();
                    break;
            }
        }
    }

    /* Follow the authoritative lobby phase for phases the room/leader drives.
     * Local sub-phases (PREFLIGHT/SELECTING while lobby is LOBBY) are never
     * overwritten here. */
    void syncPhase() {
        if (lobby_.phase == MDKR_ONLINE_LOADING &&
            session_.state.room != MDKR_ROOM_LOADING) {
            (void)sessionDispatch(MDKR_SESSION_COMMAND_SET_ROOM_PHASE,
                                  MDKR_ROOM_LOADING);
            (void)sessionDispatch(MDKR_SESSION_COMMAND_REQUEST_RACE, 0u);
        }
    }

    /* ---- Peer mesh ---------------------------------------------------- */

    void bringUpMesh() {
        if (meshUp_ || !opts_.meshBackend || !haveLobby_) return;
        buildMeshRoster();
        meshEpoch_ = lobby_.leader_generation; /* stable nonzero keying epoch */
        MdkrMatchPeerSignalFeed *feed = opts_.meshBackend->beginSignaling(
            localEndpointId_, meshGeneration_, roomIdStr_, credential_,
            iceServers_);
        if (feed == nullptr) {
            failure_ = MDKR_ONLINE_VIEW_FAILURE_CONNECTION_CHECK;
            return;
        }
        MdkrMatchPeerMeshOptions o;
        o.signal = feed;
        o.roomId = roomIdStr_;
        o.localEndpointId = localEndpointId_;
        o.localGeneration = meshGeneration_;
        o.matchEpoch = meshEpoch_;
        o.compatibility = opts_.compatibility;
        o.roster = meshRoster_;
        o.iceServers = iceServers_;
        o.nowMs = nowMs_;
        std::string err;
        mesh_ = MdkrMatchPeerMesh::create(o, &err);
        if (!mesh_) {
            failure_ = MDKR_ONLINE_VIEW_FAILURE_CONNECTION_CHECK;
            return;
        }
        meshUp_ = true;
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

    void pumpMesh() {
        if (!meshUp_ || !mesh_) return;
        mesh_->pump();
        mesh_->drainEvents(meshEvents_);
        for (const MdkrMatchPeerMeshEvent &ev : meshEvents_) {
            switch (ev.type) {
                case MdkrMatchPeerMeshEventType::PhraseReady: {
                    std::string p;
                    if (mesh_->phrase(p) &&
                        p.size() + 1u <= sizeof(phrase_)) {
                        std::memcpy(phrase_, p.c_str(), p.size() + 1u);
                        havePhrase_ = true;
                        bump();
                    }
                    break;
                }
                case MdkrMatchPeerMeshEventType::PeerChannelsReady:
                    channelsReady_.insert(ev.endpointId);
                    bump();
                    break;
                case MdkrMatchPeerMeshEventType::PreflightFragment:
                    onPreflightFragment(ev);
                    break;
                case MdkrMatchPeerMeshEventType::PeerLost:
                    failure_ = mapLostReason(ev.lostReason);
                    bump();
                    break;
                case MdkrMatchPeerMeshEventType::Failure:
                    /* SignalLost with healthy channels is a status, not a
                     * failure (docs: "Room updates reconnecting"). */
                    if (ev.failure != MdkrMatchPeerMeshFailure::SignalLost) {
                        failure_ = MDKR_ONLINE_VIEW_FAILURE_VERIFICATION_MISMATCH;
                        bump();
                    }
                    break;
                case MdkrMatchPeerMeshEventType::InputEnvelope:
                    /* Post-install per-tick input feed into match_transport is
                     * driven by the two-process race lane (O-T6); counted here
                     * for diagnosability. */
                    ++inputEnvelopes_;
                    break;
            }
        }
    }

    static MdkrOnlineViewFailure mapLostReason(MdkrMatchPeerLostReason r) {
        switch (r) {
            case MdkrMatchPeerLostReason::CommitmentMismatch:
            case MdkrMatchPeerLostReason::HelloViolation:
            case MdkrMatchPeerLostReason::ControlChannelViolation:
                return MDKR_ONLINE_VIEW_FAILURE_VERIFICATION_MISMATCH;
            case MdkrMatchPeerLostReason::ConnectTimeout:
            case MdkrMatchPeerLostReason::TransportFailed:
                return MDKR_ONLINE_VIEW_FAILURE_NETWORKS_CANNOT_CONNECT;
            case MdkrMatchPeerLostReason::PingTimeout:
            case MdkrMatchPeerLostReason::PeerEnded:
            case MdkrMatchPeerLostReason::SealWindowExhausted:
            default:
                return MDKR_ONLINE_VIEW_FAILURE_CONNECTION_CHECK;
        }
    }

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
        MdkrMatchManifestV1 manifest;
        if (!manifestFromLobby(lobby_, opts_.compatibility, opts_.inputDelay,
                               &manifest)) {
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
            descriptorBuilt_ = false;
            failure_ = MDKR_ONLINE_VIEW_FAILURE_ENGINE_FAILED;
            bump();
            return;
        }
        descriptorBuilt_ = true;
        bump();
    }

    /* ---- Preflight consensus + install -------------------------------- */

    void runPreflight() {
        if (!descriptorBuilt_ || installed_ || preflightReady_) return;
        if (!meshUp_ || !mesh_) return;
        /* Every roster peer's channels must be ready before consensus. */
        if (channelsReady_.size() + 1u < meshRoster_.size()) return;
        if (!phraseConfirmed_ || !havePhrase_) return;

        if (!preflightInit_) {
            buildGraph();
            uint8_t transcriptDigest[MDKR_MATCH_PREFLIGHT_DIGEST_BYTES];
            phraseDigest(transcriptDigest);
            if (!mdkr_match_preflight_init(&preflight_, &descriptor_, &graph_,
                                           transcriptDigest, localEndpointId_,
                                           meshGeneration_)) {
                return;
            }
            preflightInit_ = true;
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
            }
        }

        const MdkrMatchPreflightStatus status =
            mdkr_match_preflight_evaluate(&preflight_);
        if (status.state == MDKR_MATCH_PREFLIGHT_READY) {
            preflightReady_ = true;
            install();
            bump();
        }
    }

    void phraseDigest(uint8_t out[MDKR_MATCH_PREFLIGHT_DIGEST_BYTES]) const {
        MdkrSha256 ctx;
        mdkr_sha256_init(&ctx);
        mdkr_sha256_update(&ctx, phrase_, std::strlen(phrase_));
        mdkr_sha256_final(&ctx, out);
    }

    void buildGraph() {
        std::vector<MdkrMatchPeerEndpoint> eps;
        std::map<uint64_t, unsigned> index;
        for (const MdkrMatchPeerSlotOwner &o : meshRoster_) {
            MdkrMatchPeerEndpoint e;
            std::memset(&e, 0, sizeof(e));
            e.endpoint_id = o.endpointId;
            e.generation = meshGeneration_;
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
        att->sequence = 1u;
        att->endpoint_id = localEndpointId_;
        (void)mdkr_match_preflight_descriptor_digest(&descriptor_,
                                                     att->descriptor_digest);
        phraseDigest(att->transcript_digest);
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

    void onPreflightFragment(const MdkrMatchPeerMeshEvent &ev) {
        const uint64_t peer = ev.context.key.source_endpoint_id;
        auto it = fragStates_.find(peer);
        if (it == fragStates_.end()) {
            MdkrMatchPreflightFragmentState st;
            if (!mdkr_match_preflight_fragment_state_init(&st,
                                                          &ev.context.key)) {
                return;
            }
            it = fragStates_.emplace(peer, st).first;
        }
        MdkrMatchPreflightAttestationV1 att;
        const MdkrMatchPreflightFragmentResult r =
            mdkr_match_preflight_fragment_submit(&it->second, &ev.context,
                                                 ev.payload.data(), &att);
        if (r != MDKR_MATCH_PREFLIGHT_FRAGMENT_COMPLETE) return;
        if (preflightInit_) {
            (void)mdkr_match_preflight_submit(
                &preflight_, peer, ev.context.key.source_generation, &att);
        } else {
            pendingPeerAtts_[peer] = att;
        }
    }

    void install() {
        if (installed_) return;
        MdkrNetRoster roster;
        if (!mdkr_net_roster_init(&roster, &descriptor_.manifest)) return;
        uint8_t localSlots[MDKR_MATCH_SLOTS];
        unsigned localCount = 0u;
        for (unsigned i = 0u; i < descriptor_.manifest.slot_count; ++i) {
            if (descriptor_.manifest.slot_owner[i] == localEndpointId_) {
                localSlots[localCount++] = static_cast<uint8_t>(i);
            }
        }
        if (!mdkr_net_roster_configure_local(&roster, localSlots, localCount) ||
            !mdkr_net_roster_set_viewports(&roster, localSlots, localCount)) {
            return;
        }
        if (mdkr_net_roster_runtime_install_launch(&descriptor_, &roster)) {
            installed_ = true;
        }
    }

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
    char phrase_[MDKR_ONLINE_VERIFICATION_PHRASE_BYTES] = {};
    uint32_t revision_ = 1u;
    uint64_t nextCommandId_ = 1u;
    Pending pending_ = Pending::None;
    /* Last lobby command, for launcher-owned stale-revision re-send. */
    MdkrOnlineCommandType lastType_ = MDKR_ONLINE_JOIN;
    uint32_t lastSeat_ = 0u;
    uint32_t lastValue_ = 0u;
    bool haveLast_ = false;
    unsigned staleRetries_ = 0u;
    std::function<uint64_t()> nowMs_;
    std::vector<MdkrMatchPeerIceServer> iceServers_;
    std::string roomIdStr_;
    std::string credential_;
    std::vector<MdkrOnlineRoomEvent> roomEvents_;

    /* Mesh */
    static constexpr uint32_t meshGeneration_ = 1u;
    uint32_t meshEpoch_ = 1u;
    std::vector<MdkrMatchPeerSlotOwner> meshRoster_;
    std::unique_ptr<MdkrMatchPeerMesh> mesh_;
    bool meshUp_ = false;
    std::set<uint64_t> channelsReady_;
    std::vector<MdkrMatchPeerMeshEvent> meshEvents_;
    uint64_t inputEnvelopes_ = 0u;

    /* Loading barrier + preflight */
    bool loadingBuildDone_ = false;
    bool descriptorBuilt_ = false;
    MdkrMatchLaunchRefusal refusal_ = MDKR_MATCH_LAUNCH_ADMITTED;
    MdkrMatchLaunchDescriptorV1 descriptor_{};
    bool preflightInit_ = false;
    MdkrMatchPreflightV1 preflight_{};
    bool ownSubmitted_ = false;
    bool preflightReady_ = false;
    bool installed_ = false;
    MdkrMatchPeerGraph graph_{};
    std::map<uint64_t, MdkrMatchPreflightFragmentState> fragStates_;
    std::map<uint64_t, MdkrMatchPreflightAttestationV1> pendingPeerAtts_;
};

}  // namespace

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
    const LiveAdapter *live = dynamic_cast<const LiveAdapter *>(adapter);
    if (live == nullptr) return false;
    live->fillProbe(out);
    return true;
}

std::unique_ptr<IMdkrOnlineAdapter> OnlineRoom_makeGatedLiveAdapter(
    const MdkrOnlineCompatibilityV1 &) {
    /* Production wiring (real HTTP MatchRoom transport + real signal-client
     * mesh backend from the join payload) is owned by the O-T6 two-process
     * race lane, which constructs the live adapter through
     * mdkr_online_live_adapter_create with production transports. In the
     * launcher panel this returns nullptr, so even with the token gate open the
     * panel falls back to the fail-closed fake path until O-T6 lands. */
    return nullptr;
}
