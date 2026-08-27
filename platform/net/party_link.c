#include "party_link.h"

/* The projection helper reaches into the launcher's lobby + view-model structs
 * (read-only, field copies only). These are dependency-light launcher C headers
 * (stdint/stdbool plus each other); pulling them in HERE -- not in party_link.h
 * -- keeps the header safe to include from an engine TU that only reads the
 * feed. */
#include "online/lobby_core.h"
#include "online/lobby_view_model.h"

#include <string.h>

/* Single-threaded process-global state (see party_link.h): the launcher service
 * callback and the engine loop alternate on one thread, so no lock is needed --
 * the same discipline net_roster_runtime.c and online_race_results.c use. */
static MdkrPartyLinkSnapshot sSnapshot;
static bool sActive;
static MdkrPartyLinkLocalIntent sIntent;
/* Monotonic per-publish intent epoch vs. the last epoch a poll handed out.
 * Equal means "nothing new": the initial 0 == 0 state is the same guard that
 * later stops an already-read intent from being re-read (online_race_results). */
static uint32_t sIntentEpoch;
static uint32_t sIntentPolledEpoch;

bool mdkr_party_link_install(void) {
    if (sActive) return false;
    memset(&sSnapshot, 0, sizeof(sSnapshot));
    memset(&sIntent, 0, sizeof(sIntent));
    sIntentEpoch = 0u;
    sIntentPolledEpoch = 0u;
    sActive = true;
    return true;
}

void mdkr_party_link_clear(void) {
    memset(&sSnapshot, 0, sizeof(sSnapshot));
    memset(&sIntent, 0, sizeof(sIntent));
    sIntentEpoch = 0u;
    sIntentPolledEpoch = 0u;
    sActive = false;
}

bool mdkr_party_link_active(void) {
    return sActive;
}

void mdkr_party_link_publish(const MdkrPartyLinkSnapshot *snapshot) {
    uint32_t previous;
    if (!sActive || snapshot == NULL) return;
    previous = sSnapshot.generation;
    sSnapshot = *snapshot;
    /* Bump if the caller did not advance it, so readers always see progress. */
    if (sSnapshot.generation <= previous) {
        sSnapshot.generation = previous + 1u;
    }
}

bool mdkr_party_link_read(MdkrPartyLinkSnapshot *out) {
    if (!sActive || out == NULL) return false;
    *out = sSnapshot;
    return true;
}

void mdkr_party_link_intent_publish(const MdkrPartyLinkLocalIntent *intent) {
    if (!sActive || intent == NULL) return;
    sIntent = *intent;
    sIntentEpoch++;
}

bool mdkr_party_link_intent_poll(MdkrPartyLinkLocalIntent *out) {
    if (!sActive || out == NULL || sIntentPolledEpoch == sIntentEpoch) {
        return false;
    }
    *out = sIntent;
    sIntentPolledEpoch = sIntentEpoch;
    return true;
}

/* ---- Launcher-side projection ------------------------------------------- */

static const MdkrOnlineMember *party_link_member_for(
    const MdkrOnlineLobby *lobby, uint64_t endpoint_id) {
    unsigned i;
    if (endpoint_id == 0u) return NULL;
    for (i = 0u; i < MDKR_ONLINE_MAX_ENDPOINTS; i++) {
        if (lobby->members[i].occupied &&
            lobby->members[i].endpoint_id == endpoint_id) {
            return &lobby->members[i];
        }
    }
    return NULL;
}

/* Resolve which endpoint is "local". An explicit nonzero id wins. Otherwise, in
 * the fenced 2-endpoint beta, derive it from the view model's leader flag: the
 * leader endpoint when this endpoint leads, else the unique occupied non-leader
 * endpoint. Returns 0 when it cannot be resolved unambiguously. */
static uint64_t party_link_resolve_local(const MdkrOnlineViewModel *view,
                                         const MdkrOnlineLobby *lobby,
                                         uint64_t local_endpoint_id) {
    unsigned i;
    uint64_t candidate;
    unsigned candidates;
    if (local_endpoint_id != 0u) return local_endpoint_id;
    if (view == NULL) return 0u;
    if (view->local_member_is_leader) return lobby->leader_endpoint_id;
    candidate = 0u;
    candidates = 0u;
    for (i = 0u; i < MDKR_ONLINE_MAX_ENDPOINTS; i++) {
        if (lobby->members[i].occupied &&
            lobby->members[i].endpoint_id != lobby->leader_endpoint_id) {
            candidate = lobby->members[i].endpoint_id;
            candidates++;
        }
    }
    return candidates == 1u ? candidate : 0u;
}

void mdkr_party_link_snapshot_from_lobby(
    MdkrPartyLinkSnapshot *out,
    const MdkrOnlineViewModel *view,
    const MdkrOnlineLobby *lobby,
    uint64_t local_endpoint_id) {
    unsigned i;
    uint64_t local_endpoint;
    if (out == NULL || lobby == NULL) return;
    memset(out, 0, sizeof(*out));
    /* generation intentionally left 0: mdkr_party_link_publish owns it. */
    out->phase = (uint8_t)lobby->phase;
    out->mode = lobby->mode;
    out->configured_track = lobby->configured_track;
    out->cup_id = lobby->cup_id;
    out->race_index = lobby->race_index;
    local_endpoint = party_link_resolve_local(view, lobby, local_endpoint_id);
    for (i = 0u; i < MDKR_PARTY_LINK_SEATS && i < MDKR_ONLINE_MAX_SEATS; i++) {
        const MdkrOnlineSeat *seat = &lobby->seats[i];
        MdkrPartyLinkSeat *dst = &out->seats[i];
        const MdkrOnlineMember *member;
        dst->occupied = seat->occupied ? 1u : 0u;
        if (!seat->occupied) {
            /* Never publish a real racer id (0) for an empty seat: force the
             * unset sentinels so P2-T2 readers can trust the fields (F4). */
            dst->character_id = MDKR_ONLINE_NO_CHARACTER;
            dst->vehicle_id = MDKR_ONLINE_NO_VEHICLE;
            continue;
        }
        dst->character_id = seat->character_id;
        dst->vehicle_id = seat->vehicle_id;
        dst->is_host =
            (seat->endpoint_id == lobby->leader_endpoint_id) ? 1u : 0u;
        dst->is_local = (local_endpoint != 0u &&
                         seat->endpoint_id == local_endpoint) ? 1u : 0u;
        member = party_link_member_for(lobby, seat->endpoint_id);
        if (member != NULL) {
            dst->ready = member->ready ? 1u : 0u;
            dst->connected = member->connected ? 1u : 0u;
        }
        /* name[] has no source in the lobby/view model yet; left empty. */
    }
    for (i = 0u; i < MDKR_PARTY_LINK_SEATS && i < MDKR_ONLINE_MAX_SEATS; i++) {
        out->points[i] = lobby->points[i];
        out->last_placements[i] = lobby->last_placements[i];
    }
    /* host_cursor stays zero/invalid (P2-T3). */
}

/* ---- Reverse-feed dispatch plan (pure, convergence-driven) -------------- */

void mdkr_party_link_dispatch_state_reset(MdkrPartyLinkDispatchState *state) {
    if (state == NULL) return;
    memset(state, 0, sizeof(*state));
}

/* What value would this kind's command carry for `intent`, and is the local
 * seat ALREADY converged to it (so nothing needs sending)? Returns 1 when the
 * intent WANTS this kind (regardless of convergence); *value / *converged are
 * only meaningful then. */
static uint8_t party_link_kind_state(uint8_t kind,
                                     const MdkrPartyLinkLocalIntent *intent,
                                     const MdkrPartyLinkLocalView *local,
                                     uint8_t *value, uint8_t *converged) {
    uint8_t have = local != NULL && local->have_seat;
    switch (kind) {
    case MDKR_PARTY_LINK_DISPATCH_CHOOSE_CHARACTER:
        if (!intent->confirmed) return 0u;
        *value = intent->hover_character;
        *converged = have && local->character_id == intent->hover_character;
        return 1u;
    case MDKR_PARTY_LINK_DISPATCH_CHOOSE_VEHICLE:
        /* Mask-legal value handling: only a real vehicle id is ever sent; the
         * reducer's own mask gate rejects a track-illegal pick (refusal ->
         * re-fire), never a silent launcher default. */
        if (intent->vehicle_id == MDKR_ONLINE_NO_VEHICLE ||
            intent->vehicle_id >= MDKR_ONLINE_PLAYER_VEHICLE_COUNT) return 0u;
        *value = intent->vehicle_id;
        *converged = have && local->vehicle_id == intent->vehicle_id;
        return 1u;
    case MDKR_PARTY_LINK_DISPATCH_CHANGE_SELECTION:
        /* Back out == un-ready (SET_READY 0): converged once not ready. */
        if (!intent->backout) return 0u;
        *value = 0u;
        *converged = have && !local->ready;
        return 1u;
    case MDKR_PARTY_LINK_DISPATCH_READY:
        if (!intent->ready) return 0u;
        *value = 1u;
        *converged = have && local->ready;
        return 1u;
    case MDKR_PARTY_LINK_DISPATCH_START_RACE:
        /* Converged once the room left the lobby phase (loading started). */
        if (!intent->start_requested) return 0u;
        *value = 0u; /* mask filled by the wiring */
        *converged = local != NULL && local->phase != (uint8_t)MDKR_ONLINE_LOBBY;
        return 1u;
    /* Host-only session config (PD-T3). Wanted only for the leader seat and only
     * when the intent carries a non-sentinel value; the joiner always publishes
     * the UNSET sentinels, so it never wants these regardless of the is_host
     * gate. The reducer clears every member's ready on any of these, which is why
     * kOrder[] runs them before READY (the same plan re-asserts ready after). */
    case MDKR_PARTY_LINK_DISPATCH_SET_MODE:
        if (!have || !local->is_host) return 0u;
        if (intent->mode == MDKR_PARTY_LINK_MODE_UNSET) return 0u;
        *value = intent->mode;
        *converged = local->mode == intent->mode;
        return 1u;
    case MDKR_PARTY_LINK_DISPATCH_SET_CONFIG_TRACK:
        if (!have || !local->is_host) return 0u;
        if (intent->config_track == MDKR_PARTY_LINK_TRACK_UNSET) return 0u;
        /* Track ids are all <= 33, so the u8 action value never truncates. */
        *value = (uint8_t)intent->config_track;
        *converged = local->configured_track == intent->config_track;
        return 1u;
    case MDKR_PARTY_LINK_DISPATCH_SET_CUP:
        if (!have || !local->is_host) return 0u;
        if (intent->cup_id == MDKR_PARTY_LINK_CUP_UNSET) return 0u;
        *value = intent->cup_id;
        *converged = local->cup_id == intent->cup_id;
        return 1u;
    default:
        return 0u;
    }
}

void mdkr_party_link_plan_dispatch(MdkrPartyLinkDispatchState *state,
                                   const MdkrPartyLinkLocalIntent *intent,
                                   const MdkrPartyLinkLocalView *local,
                                   MdkrPartyLinkDispatchPlan *out) {
    /* Ordered so the host's session config (which clears all ready) lands FIRST,
     * then vehicle before ready, then start last -- so a single plan that carries
     * both a config change and READY ends converged to all-ready. */
    static const uint8_t kOrder[] = {
        MDKR_PARTY_LINK_DISPATCH_SET_MODE,
        MDKR_PARTY_LINK_DISPATCH_SET_CONFIG_TRACK,
        MDKR_PARTY_LINK_DISPATCH_SET_CUP,
        MDKR_PARTY_LINK_DISPATCH_CHOOSE_CHARACTER,
        MDKR_PARTY_LINK_DISPATCH_CHOOSE_VEHICLE,
        MDKR_PARTY_LINK_DISPATCH_CHANGE_SELECTION,
        MDKR_PARTY_LINK_DISPATCH_READY,
        MDKR_PARTY_LINK_DISPATCH_START_RACE};
    unsigned i;
    if (out == NULL) return;
    memset(out, 0, sizeof(*out));
    if (state == NULL || intent == NULL) return;

    for (i = 0u; i < sizeof(kOrder) / sizeof(kOrder[0]); i++) {
        const uint8_t kind = kOrder[i];
        uint8_t value = 0u;
        uint8_t converged = 0u;
        const uint8_t wants =
            party_link_kind_state(kind, intent, local, &value, &converged);
        if (!wants || converged) {
            /* Not wanted, or the lobby already reflects it: DONE -- drop any
             * in-flight guard for this kind. Convergence is the only "latch". */
            state->inflight_active[kind] = 0u;
            state->inflight_age[kind] = 0u;
            continue;
        }
        /* Wanted and NOT converged. Suppress a re-send while the same value is
         * genuinely in flight, until the safety-net bound elapses. */
        if (state->inflight_active[kind] &&
            state->inflight_value[kind] == value) {
            if (state->inflight_age[kind] < 0xFFFFu) state->inflight_age[kind]++;
            if (state->inflight_age[kind] < MDKR_PARTY_LINK_INFLIGHT_MAX_PUMPS) {
                continue; /* still in flight: wait for convergence/refusal */
            }
            /* Bound elapsed with no convergence and no refusal: re-fire once. */
            state->inflight_active[kind] = 0u;
            state->inflight_age[kind] = 0u;
        }
        if (out->count < MDKR_PARTY_LINK_MAX_DISPATCH) {
            out->actions[out->count].kind = kind;
            out->actions[out->count].value = value;
            out->count++;
        }
    }
}

void mdkr_party_link_dispatch_mark_sent(MdkrPartyLinkDispatchState *state,
                                        const MdkrPartyLinkDispatchAction *action) {
    if (state == NULL || action == NULL ||
        action->kind >= MDKR_PARTY_LINK_DISPATCH_KIND_COUNT) return;
    state->inflight_active[action->kind] = 1u;
    state->inflight_value[action->kind] = action->value;
    state->inflight_age[action->kind] = 0u;
}

void mdkr_party_link_dispatch_note_refusal(MdkrPartyLinkDispatchState *state,
                                           uint8_t kind) {
    if (state == NULL || kind >= MDKR_PARTY_LINK_DISPATCH_KIND_COUNT) return;
    state->inflight_active[kind] = 0u;
    state->inflight_age[kind] = 0u;
}
