/* Unit tests for the native online BETA live selection bridge
 * (platform/net/party_link.{h,c}): the forward-feed snapshot runtime, the
 * one-shot reverse-feed intent channel, and the pure view-model+lobby ->
 * snapshot projection. ROM-, GPU- and network-free. */
#include "net/party_link.h"

#include "online/lobby_core.h"
#include "online/lobby_view_model.h"

#include <stdio.h>
#include <string.h>

static int g_failures;
static int g_checks;
#define CHECK(cond)                                                            \
    do {                                                                       \
        ++g_checks;                                                            \
        if (!(cond)) {                                                         \
            ++g_failures;                                                      \
            fprintf(stderr, "CHECK failed: %s (%s:%d)\n", #cond, __FILE__,     \
                    __LINE__);                                                 \
        }                                                                      \
    } while (0)

/* A representative snapshot with distinctive field values so a round-trip can
 * prove every byte survived (generation is owned by publish, so it is set 0). */
static void sample_snapshot(MdkrPartyLinkSnapshot *out) {
    memset(out, 0, sizeof(*out));
    out->generation = 0u;
    out->phase = (uint8_t)MDKR_ONLINE_RESULTS;
    out->mode = MDKR_ONLINE_MODE_TOURNAMENT;
    out->configured_track = 0xFFFFu;
    out->cup_id = 0u;
    out->race_index = 2u;
    out->seats[0].occupied = 1u;
    out->seats[0].is_local = 1u;
    out->seats[0].is_host = 1u;
    out->seats[0].ready = 1u;
    out->seats[0].connected = 1u;
    out->seats[0].character_id = 3u;
    out->seats[0].vehicle_id = 1u;
    out->seats[1].occupied = 1u;
    out->seats[1].connected = 1u;
    out->seats[1].character_id = 4u;
    out->points[0] = 18u;
    out->points[1] = 14u;
    out->last_placements[0] = 0u;
    out->last_placements[1] = 1u;
}

static void test_round_trip(void) {
    MdkrPartyLinkSnapshot in, out;
    /* Inactive before install: read fails and leaves *out untouched. */
    mdkr_party_link_clear();
    CHECK(!mdkr_party_link_active());
    memset(&out, 0xAB, sizeof(out));
    CHECK(!mdkr_party_link_read(&out));
    CHECK(((const unsigned char *)&out)[0] == 0xABu); /* untouched */

    CHECK(mdkr_party_link_install());
    CHECK(mdkr_party_link_active());
    /* install-once: a second install without clear is refused. */
    CHECK(!mdkr_party_link_install());

    sample_snapshot(&in);
    mdkr_party_link_publish(&in);
    CHECK(mdkr_party_link_read(&out));
    /* Publish bumped the generation from 0; every other field round-trips. */
    CHECK(out.generation == 1u);
    CHECK(out.phase == (uint8_t)MDKR_ONLINE_RESULTS);
    CHECK(out.mode == MDKR_ONLINE_MODE_TOURNAMENT);
    CHECK(out.configured_track == 0xFFFFu);
    CHECK(out.cup_id == 0u);
    CHECK(out.race_index == 2u);
    CHECK(out.seats[0].character_id == 3u && out.seats[0].vehicle_id == 1u);
    CHECK(out.seats[0].is_local && out.seats[0].is_host && out.seats[0].ready);
    CHECK(out.seats[1].character_id == 4u && out.seats[1].connected);
    CHECK(out.points[0] == 18u && out.points[1] == 14u);
    CHECK(out.last_placements[0] == 0u && out.last_placements[1] == 1u);

    mdkr_party_link_clear();
    CHECK(!mdkr_party_link_active());
    /* clear() drops the snapshot: read fails again. */
    CHECK(!mdkr_party_link_read(&out));
}

static void test_generation_monotonicity(void) {
    MdkrPartyLinkSnapshot in, out;
    mdkr_party_link_clear();
    CHECK(mdkr_party_link_install());
    sample_snapshot(&in);

    in.generation = 0u;
    mdkr_party_link_publish(&in);
    CHECK(mdkr_party_link_read(&out) && out.generation == 1u);
    /* Caller left generation stale (still 0): publish bumps it again. */
    in.generation = 0u;
    mdkr_party_link_publish(&in);
    CHECK(mdkr_party_link_read(&out) && out.generation == 2u);
    /* Caller advanced generation itself: publish respects the higher value. */
    in.generation = 100u;
    mdkr_party_link_publish(&in);
    CHECK(mdkr_party_link_read(&out) && out.generation == 100u);
    /* Back to stale: never regresses, always bumps past the stored value. */
    in.generation = 5u;
    mdkr_party_link_publish(&in);
    CHECK(mdkr_party_link_read(&out) && out.generation == 101u);

    mdkr_party_link_clear();
}

static void test_intent_one_shot(void) {
    MdkrPartyLinkLocalIntent in, out;
    mdkr_party_link_clear();
    /* Inactive: publish is a no-op and poll fails. */
    memset(&in, 0, sizeof(in));
    in.confirmed = 1u;
    mdkr_party_link_intent_publish(&in);
    CHECK(!mdkr_party_link_intent_poll(&out));

    CHECK(mdkr_party_link_install());
    /* No publish yet: poll returns false. */
    CHECK(!mdkr_party_link_intent_poll(&out));

    memset(&in, 0, sizeof(in));
    in.hover_character = 7u;
    in.vehicle_id = 2u;
    in.confirmed = 1u;
    in.ready = 1u;
    in.start_requested = 1u;
    mdkr_party_link_intent_publish(&in);
    /* One-shot: true exactly once, matching the published intent. */
    memset(&out, 0, sizeof(out));
    CHECK(mdkr_party_link_intent_poll(&out));
    CHECK(out.hover_character == 7u && out.vehicle_id == 2u &&
          out.confirmed == 1u && out.ready == 1u &&
          out.start_requested == 1u && out.backout == 0u);
    /* Second poll without a new publish returns false. */
    CHECK(!mdkr_party_link_intent_poll(&out));

    /* A fresh publish re-arms the poll exactly once more. */
    memset(&in, 0, sizeof(in));
    in.backout = 1u;
    mdkr_party_link_intent_publish(&in);
    CHECK(mdkr_party_link_intent_poll(&out) && out.backout == 1u &&
          out.confirmed == 0u);
    CHECK(!mdkr_party_link_intent_poll(&out));

    /* clear() drops the intent channel too. */
    mdkr_party_link_clear();
    CHECK(!mdkr_party_link_intent_poll(&out));
}

/* Build a populated 2-seat tournament lobby (host seat 0 = leader/crown, joiner
 * seat 1), with accrued points, last placements and cup config -- the shape
 * tests/test_online_live_adapter.cpp reaches through the live reducer, built
 * here directly since the mapper is pure. */
static void build_tournament_lobby(MdkrOnlineLobby *lobby) {
    memset(lobby, 0, sizeof(*lobby));
    lobby->protocol_version = MDKR_ONLINE_PROTOCOL_VERSION;
    lobby->phase = MDKR_ONLINE_RESULTS;
    lobby->leader_endpoint_id = 0x101u;
    lobby->member_count = 2u;
    lobby->members[0].endpoint_id = 0x101u;
    lobby->members[0].occupied = true;
    lobby->members[0].connected = true;
    lobby->members[0].ready = true;
    lobby->members[1].endpoint_id = 0x102u;
    lobby->members[1].occupied = true;
    lobby->members[1].connected = true;
    lobby->members[1].ready = false;
    lobby->seat_count = 2u;
    lobby->seats[0].endpoint_id = 0x101u;
    lobby->seats[0].occupied = true;
    lobby->seats[0].character_id = 1u;
    lobby->seats[0].vehicle_id = 0u;
    lobby->seats[1].endpoint_id = 0x102u;
    lobby->seats[1].occupied = true;
    lobby->seats[1].character_id = 2u;
    lobby->seats[1].vehicle_id = 0u;
    lobby->mode = MDKR_ONLINE_MODE_TOURNAMENT;
    lobby->cup_id = 0u;
    lobby->configured_track = 0xFFFFu; /* tournament: cup schedule owns track */
    lobby->race_index = 1u;
    lobby->points[0] = 9u;
    lobby->points[1] = 7u;
    lobby->last_placements[0] = 0u;
    lobby->last_placements[1] = 1u;
    lobby->last_placements[2] = MDKR_ONLINE_NO_PLACEMENT;
    lobby->last_placements[3] = MDKR_ONLINE_NO_PLACEMENT;
}

static void test_snapshot_field_mapping(void) {
    MdkrOnlineLobby lobby;
    MdkrOnlineViewModel view;
    MdkrPartyLinkSnapshot snap;
    build_tournament_lobby(&lobby);

    /* Local endpoint is the leader (host). */
    memset(&view, 0, sizeof(view));
    view.local_member_is_leader = true;
    mdkr_party_link_snapshot_from_lobby(&snap, &view, &lobby, 0u);

    CHECK(snap.generation == 0u); /* mapper never sets generation */
    CHECK(snap.phase == (uint8_t)MDKR_ONLINE_RESULTS);
    CHECK(snap.mode == MDKR_ONLINE_MODE_TOURNAMENT);
    CHECK(snap.cup_id == 0u);
    CHECK(snap.configured_track == 0xFFFFu);
    CHECK(snap.race_index == 1u);
    /* Seat 0: host crown + local, ready, connected, its selection. */
    CHECK(snap.seats[0].occupied && snap.seats[0].is_host &&
          snap.seats[0].is_local);
    CHECK(snap.seats[0].ready && snap.seats[0].connected);
    CHECK(snap.seats[0].character_id == 1u && snap.seats[0].vehicle_id == 0u);
    CHECK(snap.seats[0].name[0] == '\0'); /* no name source yet */
    /* Seat 1: joiner, not host, not local, not ready but connected. */
    CHECK(snap.seats[1].occupied && !snap.seats[1].is_host &&
          !snap.seats[1].is_local);
    CHECK(!snap.seats[1].ready && snap.seats[1].connected);
    CHECK(snap.seats[1].character_id == 2u);
    /* Empty seats stay clear AND publish the unset sentinels, never racer 0 (F4). */
    CHECK(!snap.seats[2].occupied && !snap.seats[3].occupied);
    CHECK(snap.seats[2].character_id == MDKR_ONLINE_NO_CHARACTER &&
          snap.seats[2].vehicle_id == MDKR_ONLINE_NO_VEHICLE);
    CHECK(snap.seats[3].character_id == MDKR_ONLINE_NO_CHARACTER &&
          snap.seats[3].vehicle_id == MDKR_ONLINE_NO_VEHICLE);
    /* Points + standings copied verbatim. */
    CHECK(snap.points[0] == 9u && snap.points[1] == 7u);
    CHECK(snap.last_placements[0] == 0u && snap.last_placements[1] == 1u);
    CHECK(snap.last_placements[2] == MDKR_ONLINE_NO_PLACEMENT);
    /* host_cursor stays zero/invalid (P2-T3 fills it). */
    CHECK(!snap.host_cursor.valid);

    /* Local endpoint is the joiner (not leader): the unique non-leader endpoint
     * (seat 1) becomes local via the 2-endpoint leader fallback. */
    memset(&view, 0, sizeof(view));
    view.local_member_is_leader = false;
    mdkr_party_link_snapshot_from_lobby(&snap, &view, &lobby, 0u);
    CHECK(!snap.seats[0].is_local && snap.seats[1].is_local);
    CHECK(snap.seats[0].is_host && !snap.seats[1].is_host);

    /* Explicit local endpoint id wins over the view fallback. */
    mdkr_party_link_snapshot_from_lobby(&snap, NULL, &lobby, 0x102u);
    CHECK(!snap.seats[0].is_local && snap.seats[1].is_local);

    /* A single-race lobby's configured_track passes through verbatim. */
    lobby.mode = MDKR_ONLINE_MODE_SINGLE_RACE;
    lobby.configured_track = 7u;
    mdkr_party_link_snapshot_from_lobby(&snap, &view, &lobby, 0u);
    CHECK(snap.mode == MDKR_ONLINE_MODE_SINGLE_RACE &&
          snap.configured_track == 7u);
}

static MdkrPartyLinkLocalIntent intent_new(void) {
    MdkrPartyLinkLocalIntent in;
    /* C1: go through the shared init helper so every test starts from the SAME
     * "want nothing extra" baseline every real publisher must use (vehicle_id +
     * the host-only mode/config_track/cup_id fields all at their UNSET sentinels).
     * A bare memset(0) here would be the exact bug C1 pins. */
    mdkr_party_link_intent_init(&in);
    return in;
}

/* A resolved local seat with the given lobby values. */
static MdkrPartyLinkLocalView lv(uint8_t character, uint8_t vehicle,
                                 uint8_t ready, uint8_t phase) {
    MdkrPartyLinkLocalView v;
    memset(&v, 0, sizeof(v));
    v.have_seat = 1u;
    v.character_id = character;
    v.vehicle_id = vehicle;
    v.ready = ready;
    v.phase = phase;
    v.configured_track = MDKR_PARTY_LINK_TRACK_UNSET;
    v.cup_id = MDKR_PARTY_LINK_CUP_UNSET;
    return v;
}

/* A resolved local HOST seat (leader) with the given lobby session config. */
static MdkrPartyLinkLocalView lv_host(uint8_t mode, uint16_t configured_track,
                                      uint8_t cup_id, uint8_t ready,
                                      uint8_t phase) {
    MdkrPartyLinkLocalView v;
    memset(&v, 0, sizeof(v));
    v.have_seat = 1u;
    v.is_host = 1u;
    v.character_id = MDKR_ONLINE_NO_CHARACTER;
    v.vehicle_id = MDKR_ONLINE_NO_VEHICLE;
    v.ready = ready;
    v.phase = phase;
    v.mode = mode;
    v.configured_track = configured_track;
    v.cup_id = cup_id;
    return v;
}

static int plan_index_of(const MdkrPartyLinkDispatchPlan *plan, uint8_t kind) {
    unsigned i;
    for (i = 0u; i < plan->count; i++) {
        if (plan->actions[i].kind == kind) return (int)i;
    }
    return -1;
}

/* Mark the planned action of `kind` as sent (simulating a successful transport
 * send -- NOT a reduction). */
static void mark_sent_kind(MdkrPartyLinkDispatchState *st,
                           const MdkrPartyLinkDispatchPlan *plan, uint8_t kind) {
    const int idx = plan_index_of(plan, kind);
    CHECK(idx >= 0);
    if (idx >= 0) mdkr_party_link_dispatch_mark_sent(st, &plan->actions[idx]);
}

static void test_dispatch_plan(void) {
    MdkrPartyLinkDispatchState st;
    MdkrPartyLinkDispatchPlan plan;
    MdkrPartyLinkLocalIntent in;
    MdkrPartyLinkLocalView none = lv(MDKR_ONLINE_NO_CHARACTER,
                                     MDKR_ONLINE_NO_VEHICLE, 0u,
                                     (uint8_t)MDKR_ONLINE_LOBBY);
    unsigned pump;

    /* LIVE PATH (the keystone): optimistic send that does NOT converge must NOT
     * latch. The lobby snapshot -- not the send result -- is authoritative. */
    mdkr_party_link_dispatch_state_reset(&st);
    in = intent_new();
    in.confirmed = 1u;
    in.hover_character = 5u;
    /* Seat has no character yet -> plan CHOOSE_CHARACTER(5). */
    mdkr_party_link_plan_dispatch(&st, &in, &none, &plan);
    CHECK(plan_index_of(&plan, MDKR_PARTY_LINK_DISPATCH_CHOOSE_CHARACTER) >= 0);
    /* Simulate an optimistic SEND (transport accepted) but NO convergence yet. */
    mark_sent_kind(&st, &plan, MDKR_PARTY_LINK_DISPATCH_CHOOSE_CHARACTER);
    /* Still not converged, no refusal -> the in-flight guard suppresses a
     * per-frame re-send (but the command is NOT permanently latched). */
    mdkr_party_link_plan_dispatch(&st, &in, &none, &plan);
    CHECK(plan_index_of(&plan, MDKR_PARTY_LINK_DISPATCH_CHOOSE_CHARACTER) < 0);
    /* Async SELECTION_CONFLICT observed -> clears the guard -> re-fires (the old
     * optimistic-latch bug would have swallowed this forever). */
    mdkr_party_link_dispatch_note_refusal(&st,
                                          MDKR_PARTY_LINK_DISPATCH_CHOOSE_CHARACTER);
    mdkr_party_link_plan_dispatch(&st, &in, &none, &plan);
    CHECK(plan_index_of(&plan, MDKR_PARTY_LINK_DISPATCH_CHOOSE_CHARACTER) >= 0);
    /* The opponent frees the racer: our re-send lands and the lobby CONVERGES.
     * Now (and only now) it is done -- never re-sent again. */
    mark_sent_kind(&st, &plan, MDKR_PARTY_LINK_DISPATCH_CHOOSE_CHARACTER);
    {
        MdkrPartyLinkLocalView got = lv(5u, MDKR_ONLINE_NO_VEHICLE, 0u,
                                        (uint8_t)MDKR_ONLINE_LOBBY);
        mdkr_party_link_plan_dispatch(&st, &in, &got, &plan);
        CHECK(plan_index_of(&plan, MDKR_PARTY_LINK_DISPATCH_CHOOSE_CHARACTER) < 0);
        /* Repeated pumps at convergence never re-send. */
        mdkr_party_link_plan_dispatch(&st, &in, &got, &plan);
        CHECK(plan_index_of(&plan, MDKR_PARTY_LINK_DISPATCH_CHOOSE_CHARACTER) < 0);
    }

    /* SYNCHRONOUS (fake) PATH: convergence is same-/next-pump, so each command
     * sends exactly once. */
    mdkr_party_link_dispatch_state_reset(&st);
    in = intent_new();
    in.confirmed = 1u;
    in.hover_character = 2u;
    mdkr_party_link_plan_dispatch(&st, &in, &none, &plan);
    CHECK(plan_index_of(&plan, MDKR_PARTY_LINK_DISPATCH_CHOOSE_CHARACTER) >= 0);
    mark_sent_kind(&st, &plan, MDKR_PARTY_LINK_DISPATCH_CHOOSE_CHARACTER);
    {
        /* Fake reduced immediately: the next snapshot already shows character 2. */
        MdkrPartyLinkLocalView got = lv(2u, MDKR_ONLINE_NO_VEHICLE, 0u,
                                        (uint8_t)MDKR_ONLINE_LOBBY);
        mdkr_party_link_plan_dispatch(&st, &in, &got, &plan);
        CHECK(plan_index_of(&plan, MDKR_PARTY_LINK_DISPATCH_CHOOSE_CHARACTER) < 0);
    }

    /* ORDER: {character, vehicle, ready} on a fresh seat plans CHOOSE_CHARACTER,
     * then CHOOSE_VEHICLE, then READY -- vehicle strictly before ready. */
    mdkr_party_link_dispatch_state_reset(&st);
    in = intent_new();
    in.confirmed = 1u;
    in.hover_character = 3u;
    in.vehicle_id = 1u;
    in.ready = 1u;
    mdkr_party_link_plan_dispatch(&st, &in, &none, &plan);
    CHECK(plan.count == 3u);
    CHECK(plan.actions[0].kind == MDKR_PARTY_LINK_DISPATCH_CHOOSE_CHARACTER &&
          plan.actions[0].value == 3u);
    CHECK(plan.actions[1].kind == MDKR_PARTY_LINK_DISPATCH_CHOOSE_VEHICLE &&
          plan.actions[1].value == 1u);
    CHECK(plan.actions[2].kind == MDKR_PARTY_LINK_DISPATCH_READY);
    /* Character + vehicle converged, ready not yet -> only READY remains. */
    {
        MdkrPartyLinkLocalView got = lv(3u, 1u, 0u, (uint8_t)MDKR_ONLINE_LOBBY);
        mdkr_party_link_plan_dispatch(&st, &in, &got, &plan);
        CHECK(plan.count == 1u &&
              plan.actions[0].kind == MDKR_PARTY_LINK_DISPATCH_READY);
    }
    /* Everything converged -> empty plan. */
    {
        MdkrPartyLinkLocalView got = lv(3u, 1u, 1u, (uint8_t)MDKR_ONLINE_LOBBY);
        mdkr_party_link_plan_dispatch(&st, &in, &got, &plan);
        CHECK(plan.count == 0u);
    }

    /* VEHICLE mask-legality: an out-of-range vehicle is never planned. */
    mdkr_party_link_dispatch_state_reset(&st);
    in = intent_new();
    in.vehicle_id = 99u; /* >= MDKR_ONLINE_PLAYER_VEHICLE_COUNT */
    mdkr_party_link_plan_dispatch(&st, &in, &none, &plan);
    CHECK(plan.count == 0u);

    /* CHANGE_SELECTION (back out): converged once the seat is not ready. */
    mdkr_party_link_dispatch_state_reset(&st);
    in = intent_new();
    in.backout = 1u;
    {
        MdkrPartyLinkLocalView ready = lv(4u, 0u, 1u, (uint8_t)MDKR_ONLINE_LOBBY);
        mdkr_party_link_plan_dispatch(&st, &in, &ready, &plan);
        CHECK(plan_index_of(&plan,
                            MDKR_PARTY_LINK_DISPATCH_CHANGE_SELECTION) >= 0);
        mark_sent_kind(&st, &plan, MDKR_PARTY_LINK_DISPATCH_CHANGE_SELECTION);
    }
    {
        MdkrPartyLinkLocalView unready =
            lv(4u, 0u, 0u, (uint8_t)MDKR_ONLINE_LOBBY);
        mdkr_party_link_plan_dispatch(&st, &in, &unready, &plan);
        CHECK(plan_index_of(&plan,
                            MDKR_PARTY_LINK_DISPATCH_CHANGE_SELECTION) < 0);
    }

    /* START_RACE: converged once the room leaves the lobby phase. */
    mdkr_party_link_dispatch_state_reset(&st);
    in = intent_new();
    in.start_requested = 1u;
    mdkr_party_link_plan_dispatch(&st, &in, &none, &plan);
    CHECK(plan_index_of(&plan, MDKR_PARTY_LINK_DISPATCH_START_RACE) >= 0);
    mark_sent_kind(&st, &plan, MDKR_PARTY_LINK_DISPATCH_START_RACE);
    {
        MdkrPartyLinkLocalView loading = lv(MDKR_ONLINE_NO_CHARACTER,
                                            MDKR_ONLINE_NO_VEHICLE, 0u,
                                            (uint8_t)MDKR_ONLINE_LOADING);
        mdkr_party_link_plan_dispatch(&st, &in, &loading, &plan);
        CHECK(plan_index_of(&plan, MDKR_PARTY_LINK_DISPATCH_START_RACE) < 0);
    }

    /* SAFETY-NET BOUND: an in-flight command that never converges and gets no
     * refusal re-fires after MDKR_PARTY_LINK_INFLIGHT_MAX_PUMPS. */
    mdkr_party_link_dispatch_state_reset(&st);
    in = intent_new();
    in.ready = 1u;
    mdkr_party_link_plan_dispatch(&st, &in, &none, &plan);
    CHECK(plan_index_of(&plan, MDKR_PARTY_LINK_DISPATCH_READY) >= 0);
    mark_sent_kind(&st, &plan, MDKR_PARTY_LINK_DISPATCH_READY);
    for (pump = 0u; pump < MDKR_PARTY_LINK_INFLIGHT_MAX_PUMPS - 1u; pump++) {
        mdkr_party_link_plan_dispatch(&st, &in, &none, &plan);
        CHECK(plan_index_of(&plan, MDKR_PARTY_LINK_DISPATCH_READY) < 0);
    }
    mdkr_party_link_plan_dispatch(&st, &in, &none, &plan); /* bound elapsed */
    CHECK(plan_index_of(&plan, MDKR_PARTY_LINK_DISPATCH_READY) >= 0);
}

/* PD-T3: the host-only session-config dispatch kinds (SET_MODE /
 * SET_CONFIG_TRACK / SET_CUP): want/converged/ordering/host-gating/refusal. */
static void test_dispatch_session_config(void) {
    MdkrPartyLinkDispatchState st;
    MdkrPartyLinkDispatchPlan plan;
    MdkrPartyLinkLocalIntent in;

    /* HOST-ONLY GATE: a joiner (is_host==0) never plans the config kinds even
     * with a fully populated host-style intent. */
    mdkr_party_link_dispatch_state_reset(&st);
    in = intent_new();
    in.mode = MDKR_ONLINE_MODE_SINGLE_RACE;
    in.config_track = 7u; /* Hot Top Volcano */
    {
        MdkrPartyLinkLocalView joiner = lv(MDKR_ONLINE_NO_CHARACTER,
                                           MDKR_ONLINE_NO_VEHICLE, 0u,
                                           (uint8_t)MDKR_ONLINE_LOBBY);
        mdkr_party_link_plan_dispatch(&st, &in, &joiner, &plan);
        CHECK(plan_index_of(&plan, MDKR_PARTY_LINK_DISPATCH_SET_MODE) < 0);
        CHECK(plan_index_of(&plan,
                            MDKR_PARTY_LINK_DISPATCH_SET_CONFIG_TRACK) < 0);
    }

    /* UNSET SENTINELS: a host whose intent leaves the config unset plans nothing
     * (the joiner/idle-host default; SET_MODE not wanted when mode==UNSET). */
    mdkr_party_link_dispatch_state_reset(&st);
    in = intent_new(); /* all config fields at UNSET */
    {
        MdkrPartyLinkLocalView host = lv_host(MDKR_ONLINE_MODE_SINGLE_RACE,
                                              MDKR_PARTY_LINK_TRACK_UNSET,
                                              MDKR_PARTY_LINK_CUP_UNSET, 0u,
                                              (uint8_t)MDKR_ONLINE_LOBBY);
        mdkr_party_link_plan_dispatch(&st, &in, &host, &plan);
        CHECK(plan.count == 0u);
    }

    /* SINGLE-RACE HOST: mode already single, track not yet set -> plan only
     * SET_CONFIG_TRACK (SET_MODE(0) is already converged). */
    mdkr_party_link_dispatch_state_reset(&st);
    in = intent_new();
    in.mode = MDKR_ONLINE_MODE_SINGLE_RACE;
    in.config_track = 5u; /* Ancient Lake */
    {
        MdkrPartyLinkLocalView host = lv_host(MDKR_ONLINE_MODE_SINGLE_RACE,
                                              MDKR_PARTY_LINK_TRACK_UNSET,
                                              MDKR_PARTY_LINK_CUP_UNSET, 0u,
                                              (uint8_t)MDKR_ONLINE_LOBBY);
        mdkr_party_link_plan_dispatch(&st, &in, &host, &plan);
        CHECK(plan_index_of(&plan, MDKR_PARTY_LINK_DISPATCH_SET_MODE) < 0);
        CHECK(plan_index_of(&plan,
                            MDKR_PARTY_LINK_DISPATCH_SET_CONFIG_TRACK) >= 0);
        {
            const int idx = plan_index_of(
                &plan, MDKR_PARTY_LINK_DISPATCH_SET_CONFIG_TRACK);
            CHECK(idx >= 0 && plan.actions[idx].value == 5u);
        }
    }
    /* Once configured_track converges, the kind drops. */
    {
        MdkrPartyLinkLocalView host = lv_host(MDKR_ONLINE_MODE_SINGLE_RACE, 5u,
                                              MDKR_PARTY_LINK_CUP_UNSET, 0u,
                                              (uint8_t)MDKR_ONLINE_LOBBY);
        mdkr_party_link_plan_dispatch(&st, &in, &host, &plan);
        CHECK(plan_index_of(&plan,
                            MDKR_PARTY_LINK_DISPATCH_SET_CONFIG_TRACK) < 0);
    }

    /* TOURNAMENT HOST: lobby still single, host wants tournament + a cup. Plan
     * SET_MODE(1) then SET_CUP -- and SET_MODE strictly precedes SET_CUP. */
    mdkr_party_link_dispatch_state_reset(&st);
    in = intent_new();
    in.mode = MDKR_ONLINE_MODE_TOURNAMENT;
    in.cup_id = 3u; /* Dragon Forest cup */
    {
        MdkrPartyLinkLocalView host = lv_host(MDKR_ONLINE_MODE_SINGLE_RACE,
                                              MDKR_PARTY_LINK_TRACK_UNSET,
                                              MDKR_PARTY_LINK_CUP_UNSET, 0u,
                                              (uint8_t)MDKR_ONLINE_LOBBY);
        mdkr_party_link_plan_dispatch(&st, &in, &host, &plan);
        CHECK(plan_index_of(&plan, MDKR_PARTY_LINK_DISPATCH_SET_MODE) >= 0);
        CHECK(plan_index_of(&plan, MDKR_PARTY_LINK_DISPATCH_SET_CUP) >= 0);
        CHECK(plan_index_of(&plan, MDKR_PARTY_LINK_DISPATCH_SET_MODE) <
              plan_index_of(&plan, MDKR_PARTY_LINK_DISPATCH_SET_CUP));
        {
            const int idx = plan_index_of(&plan, MDKR_PARTY_LINK_DISPATCH_SET_CUP);
            CHECK(idx >= 0 && plan.actions[idx].value == 3u);
        }
    }

    /* ORDER vs READY: on a config change the reducer clears all ready, so a host
     * plan carrying BOTH SET_CONFIG_TRACK and READY must send config FIRST so the
     * re-asserted ready sticks. */
    mdkr_party_link_dispatch_state_reset(&st);
    in = intent_new();
    in.mode = MDKR_ONLINE_MODE_SINGLE_RACE;
    in.config_track = 8u; /* Whale Bay */
    in.ready = 1u;
    {
        /* Host lobby: single mode already, no track, not ready. */
        MdkrPartyLinkLocalView host = lv_host(MDKR_ONLINE_MODE_SINGLE_RACE,
                                              MDKR_PARTY_LINK_TRACK_UNSET,
                                              MDKR_PARTY_LINK_CUP_UNSET, 0u,
                                              (uint8_t)MDKR_ONLINE_LOBBY);
        mdkr_party_link_plan_dispatch(&st, &in, &host, &plan);
        CHECK(plan_index_of(&plan,
                            MDKR_PARTY_LINK_DISPATCH_SET_CONFIG_TRACK) >= 0);
        CHECK(plan_index_of(&plan, MDKR_PARTY_LINK_DISPATCH_READY) >= 0);
        CHECK(plan_index_of(&plan, MDKR_PARTY_LINK_DISPATCH_SET_CONFIG_TRACK) <
              plan_index_of(&plan, MDKR_PARTY_LINK_DISPATCH_READY));
    }

    /* REFUSAL re-fire: an optimistic SET_CONFIG_TRACK that does not converge is
     * suppressed until a refusal clears the guard, then re-fires. */
    mdkr_party_link_dispatch_state_reset(&st);
    in = intent_new();
    in.mode = MDKR_ONLINE_MODE_SINGLE_RACE;
    in.config_track = 9u; /* Snowball Valley */
    {
        MdkrPartyLinkLocalView host = lv_host(MDKR_ONLINE_MODE_SINGLE_RACE,
                                              MDKR_PARTY_LINK_TRACK_UNSET,
                                              MDKR_PARTY_LINK_CUP_UNSET, 0u,
                                              (uint8_t)MDKR_ONLINE_LOBBY);
        mdkr_party_link_plan_dispatch(&st, &in, &host, &plan);
        CHECK(plan_index_of(&plan,
                            MDKR_PARTY_LINK_DISPATCH_SET_CONFIG_TRACK) >= 0);
        mark_sent_kind(&st, &plan, MDKR_PARTY_LINK_DISPATCH_SET_CONFIG_TRACK);
        /* Still in flight (no convergence, no refusal): suppressed. */
        mdkr_party_link_plan_dispatch(&st, &in, &host, &plan);
        CHECK(plan_index_of(&plan,
                            MDKR_PARTY_LINK_DISPATCH_SET_CONFIG_TRACK) < 0);
        /* Refusal observed -> re-fires next pump. */
        mdkr_party_link_dispatch_note_refusal(
            &st, MDKR_PARTY_LINK_DISPATCH_SET_CONFIG_TRACK);
        mdkr_party_link_plan_dispatch(&st, &in, &host, &plan);
        CHECK(plan_index_of(&plan,
                            MDKR_PARTY_LINK_DISPATCH_SET_CONFIG_TRACK) >= 0);
    }
}

/* C1 + M1: the zeroed-intent contract. The UNSET sentinels are deliberately
 * nonzero, so a helper-initialised intent plans NO host config, while a raw
 * memset(0) intent WOULD want it (the exact bug C1 pins) -- and the M1 mode
 * cross-gate keeps a stray intent from dispatching cup+track together. */
static void test_dispatch_zeroed_intent_contract(void) {
    MdkrPartyLinkDispatchState st;
    MdkrPartyLinkDispatchPlan plan;
    MdkrPartyLinkLocalIntent in;
    /* Fresh single-race host lobby: nothing configured yet. */
    MdkrPartyLinkLocalView host = lv_host(MDKR_ONLINE_MODE_SINGLE_RACE,
                                          MDKR_PARTY_LINK_TRACK_UNSET,
                                          MDKR_PARTY_LINK_CUP_UNSET, 0u,
                                          (uint8_t)MDKR_ONLINE_LOBBY);

    /* Helper-initialised intent (what every publisher MUST use): the sentinels
     * mean "want nothing", so ZERO host-config kinds are planned. */
    mdkr_party_link_dispatch_state_reset(&st);
    mdkr_party_link_intent_init(&in);
    mdkr_party_link_plan_dispatch(&st, &in, &host, &plan);
    CHECK(plan_index_of(&plan, MDKR_PARTY_LINK_DISPATCH_SET_MODE) < 0);
    CHECK(plan_index_of(&plan, MDKR_PARTY_LINK_DISPATCH_SET_CONFIG_TRACK) < 0);
    CHECK(plan_index_of(&plan, MDKR_PARTY_LINK_DISPATCH_SET_CUP) < 0);

    /* The TRAP, pinned: a RAW memset(0) intent (NOT via the helper) reads
     * config_track 0 as a REAL wanted track, so it WOULD dispatch
     * SET_CONFIG_TRACK(0) -- proving the sentinels are load-bearing. (SET_MODE(0)
     * is already converged on this single lobby; SET_CUP is cross-gated off in
     * single mode by M1.) */
    mdkr_party_link_dispatch_state_reset(&st);
    memset(&in, 0, sizeof(in)); /* the bug: no sentinels */
    mdkr_party_link_plan_dispatch(&st, &in, &host, &plan);
    CHECK(plan_index_of(&plan, MDKR_PARTY_LINK_DISPATCH_SET_CONFIG_TRACK) >= 0);
    {
        const int idx =
            plan_index_of(&plan, MDKR_PARTY_LINK_DISPATCH_SET_CONFIG_TRACK);
        CHECK(idx >= 0 && plan.actions[idx].value == 0u);
    }
    /* M1 cross-gate: a raw-zero intent in single mode never plans SET_CUP. */
    CHECK(plan_index_of(&plan, MDKR_PARTY_LINK_DISPATCH_SET_CUP) < 0);

    /* M1 cross-gates, both directions: tournament intent never plans a track; a
     * single-mode intent never plans a cup, even with both fields set. */
    mdkr_party_link_dispatch_state_reset(&st);
    in = intent_new();
    in.mode = MDKR_ONLINE_MODE_TOURNAMENT;
    in.config_track = 5u; /* set, but tournament -> must NOT plan track */
    in.cup_id = 2u;
    mdkr_party_link_plan_dispatch(&st, &in, &host, &plan);
    CHECK(plan_index_of(&plan, MDKR_PARTY_LINK_DISPATCH_SET_CONFIG_TRACK) < 0);
    CHECK(plan_index_of(&plan, MDKR_PARTY_LINK_DISPATCH_SET_CUP) >= 0);

    mdkr_party_link_dispatch_state_reset(&st);
    in = intent_new();
    in.mode = MDKR_ONLINE_MODE_SINGLE_RACE;
    in.config_track = 5u;
    in.cup_id = 2u; /* set, but single -> must NOT plan cup */
    mdkr_party_link_plan_dispatch(&st, &in, &host, &plan);
    CHECK(plan_index_of(&plan, MDKR_PARTY_LINK_DISPATCH_SET_CUP) < 0);
    CHECK(plan_index_of(&plan, MDKR_PARTY_LINK_DISPATCH_SET_CONFIG_TRACK) >= 0);
}

int main(void) {
    test_round_trip();
    test_generation_monotonicity();
    test_intent_one_shot();
    test_snapshot_field_mapping();
    test_dispatch_plan();
    test_dispatch_session_config();
    test_dispatch_zeroed_intent_contract();
    fprintf(stderr, "party_link: %d checks, %d failures\n", g_checks,
            g_failures);
    return g_failures == 0 ? 0 : 1;
}
