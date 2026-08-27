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

/* ---- Reverse-feed dispatch plan (pure) ---------------------------------- */

void mdkr_party_link_dispatch_state_reset(MdkrPartyLinkDispatchState *state) {
    if (state == NULL) return;
    memset(state, 0, sizeof(*state));
    state->last_character = MDKR_ONLINE_NO_CHARACTER;
    state->last_vehicle = MDKR_ONLINE_NO_VEHICLE;
}

static void party_link_plan_push(MdkrPartyLinkDispatchPlan *out, uint8_t kind,
                                 uint8_t value) {
    if (out->count >= MDKR_PARTY_LINK_MAX_DISPATCH) return;
    out->actions[out->count].kind = kind;
    out->actions[out->count].value = value;
    out->count++;
}

void mdkr_party_link_plan_dispatch(MdkrPartyLinkDispatchState *state,
                                   const MdkrPartyLinkLocalIntent *intent,
                                   MdkrPartyLinkDispatchPlan *out) {
    if (out == NULL) return;
    memset(out, 0, sizeof(*out));
    if (state == NULL || intent == NULL) return;

    /* Edge re-arms: a released flag clears its latch so its next assertion fires
     * (independent of acceptance). */
    if (!intent->ready) state->ready_dispatched = 0u;
    if (!intent->start_requested) state->start_dispatched = 0u;
    if (!intent->backout) state->backout_dispatched = 0u;

    /* CHOOSE_CHARACTER on a changed confirmed id. Hover alone is cursor motion
     * carried by the forward feed's host_cursor, never a reducer command. */
    if (intent->confirmed &&
        (!state->character_dispatched ||
         state->last_character != intent->hover_character)) {
        party_link_plan_push(out, MDKR_PARTY_LINK_DISPATCH_CHOOSE_CHARACTER,
                             intent->hover_character);
    }
    /* CHOOSE_VEHICLE (before READY) when a vehicle is set and changed. */
    if (intent->vehicle_id != MDKR_ONLINE_NO_VEHICLE &&
        (!state->vehicle_dispatched ||
         state->last_vehicle != intent->vehicle_id)) {
        party_link_plan_push(out, MDKR_PARTY_LINK_DISPATCH_CHOOSE_VEHICLE,
                             intent->vehicle_id);
    }
    /* CHANGE_SELECTION (back out) once per assertion. */
    if (intent->backout && !state->backout_dispatched) {
        party_link_plan_push(out, MDKR_PARTY_LINK_DISPATCH_CHANGE_SELECTION, 0u);
    }
    /* READY on the rising edge. */
    if (intent->ready && !state->ready_dispatched) {
        party_link_plan_push(out, MDKR_PARTY_LINK_DISPATCH_READY, 1u);
    }
    /* START_RACE once per press (mask filled by the wiring). */
    if (intent->start_requested && !state->start_dispatched) {
        party_link_plan_push(out, MDKR_PARTY_LINK_DISPATCH_START_RACE, 0u);
    }
}

void mdkr_party_link_dispatch_latch(MdkrPartyLinkDispatchState *state,
                                    const MdkrPartyLinkDispatchAction *action) {
    if (state == NULL || action == NULL) return;
    switch (action->kind) {
    case MDKR_PARTY_LINK_DISPATCH_CHOOSE_CHARACTER:
        state->character_dispatched = 1u;
        state->last_character = action->value;
        break;
    case MDKR_PARTY_LINK_DISPATCH_CHOOSE_VEHICLE:
        state->vehicle_dispatched = 1u;
        state->last_vehicle = action->value;
        break;
    case MDKR_PARTY_LINK_DISPATCH_CHANGE_SELECTION:
        state->backout_dispatched = 1u;
        /* A fresh selection may follow: re-arm the character + vehicle picks. */
        state->character_dispatched = 0u;
        state->vehicle_dispatched = 0u;
        break;
    case MDKR_PARTY_LINK_DISPATCH_READY:
        state->ready_dispatched = 1u;
        break;
    case MDKR_PARTY_LINK_DISPATCH_START_RACE:
        state->start_dispatched = 1u;
        break;
    default:
        break;
    }
}
