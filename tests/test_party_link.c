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
    memset(&in, 0, sizeof(in));
    in.vehicle_id = MDKR_ONLINE_NO_VEHICLE; /* "unset" unless a test sets it */
    return in;
}

static int plan_index_of(const MdkrPartyLinkDispatchPlan *plan, uint8_t kind) {
    unsigned i;
    for (i = 0u; i < plan->count; i++) {
        if (plan->actions[i].kind == kind) return (int)i;
    }
    return -1;
}

/* Latch whichever planned action of `kind` is present (simulating the reducer
 * accepting that submit). */
static void latch_kind(MdkrPartyLinkDispatchState *st,
                       const MdkrPartyLinkDispatchPlan *plan, uint8_t kind) {
    const int idx = plan_index_of(plan, kind);
    CHECK(idx >= 0);
    if (idx >= 0) mdkr_party_link_dispatch_latch(st, &plan->actions[idx]);
}

static void test_dispatch_plan(void) {
    MdkrPartyLinkDispatchState st;
    MdkrPartyLinkDispatchPlan plan;
    MdkrPartyLinkLocalIntent in;

    /* F1: a REFUSED dispatch does not latch, so it re-fires; an ACCEPTED one
     * latches and dedupes; a CHANGED pick re-fires. */
    mdkr_party_link_dispatch_state_reset(&st);
    in = intent_new();
    in.confirmed = 1u;
    in.hover_character = 5u;
    mdkr_party_link_plan_dispatch(&st, &in, &plan);
    CHECK(plan_index_of(&plan, MDKR_PARTY_LINK_DISPATCH_CHOOSE_CHARACTER) >= 0);
    /* refusal: DON'T latch -> the very next intent re-plans it. */
    mdkr_party_link_plan_dispatch(&st, &in, &plan);
    CHECK(plan_index_of(&plan, MDKR_PARTY_LINK_DISPATCH_CHOOSE_CHARACTER) >= 0);
    /* acceptance: latch -> deduped. */
    latch_kind(&st, &plan, MDKR_PARTY_LINK_DISPATCH_CHOOSE_CHARACTER);
    mdkr_party_link_plan_dispatch(&st, &in, &plan);
    CHECK(plan_index_of(&plan, MDKR_PARTY_LINK_DISPATCH_CHOOSE_CHARACTER) < 0);
    /* a changed racer re-fires. */
    in.hover_character = 6u;
    mdkr_party_link_plan_dispatch(&st, &in, &plan);
    CHECK(plan_index_of(&plan, MDKR_PARTY_LINK_DISPATCH_CHOOSE_CHARACTER) >= 0);

    /* F2: {character, vehicle, ready} plans CHOOSE_CHARACTER, then
     * CHOOSE_VEHICLE, then READY -- vehicle strictly before ready. */
    mdkr_party_link_dispatch_state_reset(&st);
    in = intent_new();
    in.confirmed = 1u;
    in.hover_character = 3u;
    in.vehicle_id = 1u;
    in.ready = 1u;
    mdkr_party_link_plan_dispatch(&st, &in, &plan);
    CHECK(plan.count == 3u);
    CHECK(plan.actions[0].kind == MDKR_PARTY_LINK_DISPATCH_CHOOSE_CHARACTER &&
          plan.actions[0].value == 3u);
    CHECK(plan.actions[1].kind == MDKR_PARTY_LINK_DISPATCH_CHOOSE_VEHICLE &&
          plan.actions[1].value == 1u);
    CHECK(plan.actions[2].kind == MDKR_PARTY_LINK_DISPATCH_READY);

    /* READY: rising edge, F1 retry, and re-arm on release. */
    mdkr_party_link_dispatch_state_reset(&st);
    in = intent_new();
    in.ready = 1u;
    mdkr_party_link_plan_dispatch(&st, &in, &plan);
    CHECK(plan_index_of(&plan, MDKR_PARTY_LINK_DISPATCH_READY) >= 0);
    mdkr_party_link_plan_dispatch(&st, &in, &plan); /* refused -> re-fires */
    CHECK(plan_index_of(&plan, MDKR_PARTY_LINK_DISPATCH_READY) >= 0);
    latch_kind(&st, &plan, MDKR_PARTY_LINK_DISPATCH_READY); /* accepted */
    mdkr_party_link_plan_dispatch(&st, &in, &plan);
    CHECK(plan_index_of(&plan, MDKR_PARTY_LINK_DISPATCH_READY) < 0);
    in.ready = 0u; /* release re-arms */
    mdkr_party_link_plan_dispatch(&st, &in, &plan);
    in.ready = 1u;
    mdkr_party_link_plan_dispatch(&st, &in, &plan);
    CHECK(plan_index_of(&plan, MDKR_PARTY_LINK_DISPATCH_READY) >= 0);

    /* VEHICLE dedupe: accepted -> deduped; changed -> re-fires; unset -> never. */
    mdkr_party_link_dispatch_state_reset(&st);
    in = intent_new();
    in.vehicle_id = 1u;
    mdkr_party_link_plan_dispatch(&st, &in, &plan);
    latch_kind(&st, &plan, MDKR_PARTY_LINK_DISPATCH_CHOOSE_VEHICLE);
    mdkr_party_link_plan_dispatch(&st, &in, &plan);
    CHECK(plan_index_of(&plan, MDKR_PARTY_LINK_DISPATCH_CHOOSE_VEHICLE) < 0);
    in.vehicle_id = 2u;
    mdkr_party_link_plan_dispatch(&st, &in, &plan);
    CHECK(plan_index_of(&plan, MDKR_PARTY_LINK_DISPATCH_CHOOSE_VEHICLE) >= 0);
    in.vehicle_id = MDKR_ONLINE_NO_VEHICLE;
    mdkr_party_link_plan_dispatch(&st, &in, &plan);
    CHECK(plan_index_of(&plan, MDKR_PARTY_LINK_DISPATCH_CHOOSE_VEHICLE) < 0);

    /* BACK OUT latches once and re-arms the character + vehicle picks. */
    mdkr_party_link_dispatch_state_reset(&st);
    in = intent_new();
    in.confirmed = 1u;
    in.hover_character = 4u;
    mdkr_party_link_plan_dispatch(&st, &in, &plan);
    latch_kind(&st, &plan, MDKR_PARTY_LINK_DISPATCH_CHOOSE_CHARACTER);
    mdkr_party_link_plan_dispatch(&st, &in, &plan); /* same racer -> deduped */
    CHECK(plan_index_of(&plan, MDKR_PARTY_LINK_DISPATCH_CHOOSE_CHARACTER) < 0);
    in.backout = 1u;
    mdkr_party_link_plan_dispatch(&st, &in, &plan);
    latch_kind(&st, &plan, MDKR_PARTY_LINK_DISPATCH_CHANGE_SELECTION);
    in.backout = 0u; /* the same confirm now re-fires (re-armed) */
    mdkr_party_link_plan_dispatch(&st, &in, &plan);
    CHECK(plan_index_of(&plan, MDKR_PARTY_LINK_DISPATCH_CHOOSE_CHARACTER) >= 0);

    /* reset drops all dedupe. */
    mdkr_party_link_dispatch_state_reset(&st);
    in = intent_new();
    in.confirmed = 1u;
    in.hover_character = 4u; /* same id as before reset -> fires because cleared */
    mdkr_party_link_plan_dispatch(&st, &in, &plan);
    CHECK(plan_index_of(&plan, MDKR_PARTY_LINK_DISPATCH_CHOOSE_CHARACTER) >= 0);
}

int main(void) {
    test_round_trip();
    test_generation_monotonicity();
    test_intent_one_shot();
    test_snapshot_field_mapping();
    test_dispatch_plan();
    fprintf(stderr, "party_link: %d checks, %d failures\n", g_checks,
            g_failures);
    return g_failures == 0 ? 0 : 1;
}
