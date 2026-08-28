/* test_adventure_party_state.c — AP-02 session state machine invariants.
 *
 * ROM-free: links only platform/adventure_party/adventure_party_state.c.
 * Every named invariant from the plan's Tier-1 list for the state module is
 * a named test here; the exhaustive table covers every legal AND illegal
 * state/event pair, and the seeded property test replays long random event
 * sequences (reset/suspend/restore/exit mixed in) asserting the session
 * never becomes internally inconsistent and refusals never mutate it. */
#include "adventure_party/adventure_party_state.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
static void expect(int c, const char *w) {
    if (!c) { printf("FAIL %s\n", w); failures++; } else printf("ok   %s\n", w);
}

/* Property-loop assertion: silent on success so 20k iterations do not drown
 * the log; a violation prints once with the iteration number. */
static int quiet_failures;
static void check(int c, const char *w, unsigned long iter) {
    if (!c) {
        printf("FAIL %s (iteration %lu)\n", w, iter);
        quiet_failures++;
        failures++;
    }
}

/* --- helpers ---------------------------------------------------------- */

static uint8_t character_for(uint8_t seat) { return (uint8_t)(10 + seat); }

static AdventurePartyRosterRequest dense_roster(int count) {
    AdventurePartyRosterRequest r;
    memset(&r, 0, sizeof r);
    r.participant_count = (uint8_t)count;
    for (int i = 0; i < count && i < ADVENTURE_PARTY_MAX_SEATS; i++) {
        r.seat[i] = (uint8_t)i;
        r.character[i] = character_for((uint8_t)i);
    }
    return r;
}

static AdventurePartyRosterRequest request_from(const AdventurePartyRoster *r) {
    AdventurePartyRosterRequest q;
    int n = 0;
    memset(&q, 0, sizeof q);
    q.participant_count = r->participant_count;
    for (int s = 0; s < ADVENTURE_PARTY_MAX_SEATS; s++) {
        if (r->seat_mask & (1u << s)) {
            q.seat[n] = (uint8_t)s;
            q.character[n] = r->character_by_seat[s];
            n++;
        }
    }
    return q;
}

/* A zeroed event with valid defaults for whatever the kind needs. */
static AdventurePartyEvent make_event(AdventurePartyEventKind kind, int count) {
    AdventurePartyEvent e;
    memset(&e, 0, sizeof e);
    e.kind = kind;
    e.enabled = 1;
    e.adventure_selected = 1;
    e.winner_seat = ADVENTURE_PARTY_NO_SEAT;
    e.roster = dense_roster(count);
    return e;
}

static AdventurePartyResult apply(AdventurePartySession *s,
                                  AdventurePartyEventKind kind, int count) {
    AdventurePartyEvent e = make_event(kind, count);
    return adventure_party_session_apply(s, &e);
}

/* Drives a fresh session to `target` through legal events only, asserting
 * every step, so table rows start from a state reached the honest way. */
static void drive(AdventurePartySession *s, AdventurePartySessionState target,
                  int count) {
    adventure_party_session_init(s);
    if (target == ADVENTURE_PARTY_STATE_OFF) return;
    expect(apply(s, ADVENTURE_PARTY_EVENT_FORM, count) == ADVENTURE_PARTY_OK,
           "drive: FORM");
    if (target == ADVENTURE_PARTY_STATE_FORMING) return;
    if (target == ADVENTURE_PARTY_STATE_SHARED_SCENE) {
        expect(apply(s, ADVENTURE_PARTY_EVENT_START_NEW_GAME, count) ==
               ADVENTURE_PARTY_OK, "drive: START_NEW_GAME");
        return;
    }
    expect(apply(s, ADVENTURE_PARTY_EVENT_RESUME_SAVE, count) ==
           ADVENTURE_PARTY_OK, "drive: RESUME_SAVE");
    switch (target) {
    case ADVENTURE_PARTY_STATE_ACTIVE_LOBBY:
        return;
    case ADVENTURE_PARTY_STATE_SHARED_DIALOGUE:
        expect(apply(s, ADVENTURE_PARTY_EVENT_DIALOGUE_START, count) ==
               ADVENTURE_PARTY_OK, "drive: DIALOGUE_START");
        return;
    case ADVENTURE_PARTY_STATE_ACTIVE_RACE:
        expect(apply(s, ADVENTURE_PARTY_EVENT_RACE_START, count) ==
               ADVENTURE_PARTY_OK, "drive: RACE_START");
        return;
    case ADVENTURE_PARTY_STATE_SOLO_ACTIVITY:
        expect(apply(s, ADVENTURE_PARTY_EVENT_SOLO_START, count) ==
               ADVENTURE_PARTY_OK, "drive: SOLO_START");
        return;
    case ADVENTURE_PARTY_STATE_RESTORING_PARTY:
        expect(apply(s, ADVENTURE_PARTY_EVENT_SOLO_START, count) ==
               ADVENTURE_PARTY_OK, "drive: SOLO_START");
        expect(apply(s, ADVENTURE_PARTY_EVENT_SOLO_EXIT, count) ==
               ADVENTURE_PARTY_OK, "drive: SOLO_EXIT");
        return;
    case ADVENTURE_PARTY_STATE_EXITING:
        expect(apply(s, ADVENTURE_PARTY_EVENT_QUIT, count) ==
               ADVENTURE_PARTY_OK, "drive: QUIT");
        return;
    default:
        expect(0, "drive: unreachable target");
        return;
    }
}

/* The plan's transition diagram as a predicate. The exhaustive test below
 * holds the implementation to exactly this table — nothing more permissive,
 * nothing less. */
static int pair_is_legal(AdventurePartySessionState st,
                         AdventurePartyEventKind k) {
    switch (st) {
    case ADVENTURE_PARTY_STATE_OFF:
        return k == ADVENTURE_PARTY_EVENT_FORM;
    case ADVENTURE_PARTY_STATE_FORMING:
        return k == ADVENTURE_PARTY_EVENT_START_NEW_GAME ||
               k == ADVENTURE_PARTY_EVENT_RESUME_SAVE;
    case ADVENTURE_PARTY_STATE_SHARED_SCENE:
        return k == ADVENTURE_PARTY_EVENT_SCENE_COMPLETE;
    case ADVENTURE_PARTY_STATE_ACTIVE_LOBBY:
        return k == ADVENTURE_PARTY_EVENT_DIALOGUE_START ||
               k == ADVENTURE_PARTY_EVENT_RACE_START ||
               k == ADVENTURE_PARTY_EVENT_SOLO_START ||
               k == ADVENTURE_PARTY_EVENT_LOBBY_TRANSITION ||
               k == ADVENTURE_PARTY_EVENT_QUIT;
    case ADVENTURE_PARTY_STATE_SHARED_DIALOGUE:
        return k == ADVENTURE_PARTY_EVENT_DIALOGUE_COMPLETE;
    case ADVENTURE_PARTY_STATE_ACTIVE_RACE:
        return k == ADVENTURE_PARTY_EVENT_RACE_RESULT_COMMITTED ||
               k == ADVENTURE_PARTY_EVENT_QUIT;
    case ADVENTURE_PARTY_STATE_SOLO_ACTIVITY:
        return k == ADVENTURE_PARTY_EVENT_SOLO_EXIT;
    case ADVENTURE_PARTY_STATE_RESTORING_PARTY:
        return k == ADVENTURE_PARTY_EVENT_RESTORE_COMMIT;
    case ADVENTURE_PARTY_STATE_EXITING:
        return k == ADVENTURE_PARTY_EVENT_DESTROY;
    default:
        return 0;
    }
}

static AdventurePartyCompletionToken token_for(const AdventurePartySession *s,
                                               uint16_t course,
                                               uint8_t activity,
                                               uint8_t kind) {
    AdventurePartyCompletionToken t;
    memset(&t, 0, sizeof t);
    t.session_generation = s->session_generation;
    t.level_generation = s->level_generation;
    t.course = course;
    t.activity = activity;
    t.completion_kind = kind;
    return t;
}

/* --- tests ------------------------------------------------------------ */

static void test_init_is_off(void) {
    AdventurePartySession s;
    adventure_party_session_init(&s);
    expect(s.state == ADVENTURE_PARTY_STATE_OFF, "init: state is OFF");
    expect(!adventure_party_is_active(&s), "init: not active");
    expect(adventure_party_participant_count(&s) == 0,
           "init: participant count 0");
    expect(adventure_party_host_seat(&s) == ADVENTURE_PARTY_HOST_SEAT,
           "init: host seat is 0");
    expect(adventure_party_character_for_seat(&s, 0) == -1,
           "init: no character at seat 0");
}

static void test_form_happy_paths(void) {
    for (int count = 2; count <= 4; count++) {
        AdventurePartySession s;
        char what[96];
        adventure_party_session_init(&s);
        snprintf(what, sizeof what, "form(%d): accepted", count);
        expect(apply(&s, ADVENTURE_PARTY_EVENT_FORM, count) ==
               ADVENTURE_PARTY_OK, what);
        snprintf(what, sizeof what, "form(%d): state FORMING", count);
        expect(s.state == ADVENTURE_PARTY_STATE_FORMING, what);
        snprintf(what, sizeof what, "form(%d): now active", count);
        expect(adventure_party_is_active(&s), what);
        snprintf(what, sizeof what, "form(%d): participant count", count);
        expect(adventure_party_participant_count(&s) == count, what);
        snprintf(what, sizeof what, "form(%d): session generation bumped",
                 count);
        expect(s.session_generation == 1, what);
        snprintf(what, sizeof what, "form(%d): dense seat mask", count);
        expect(s.roster.seat_mask == (uint8_t)((1u << count) - 1u), what);
        for (int seat = 0; seat < count; seat++) {
            snprintf(what, sizeof what, "form(%d): character at seat %d",
                     count, seat);
            expect(adventure_party_character_for_seat(&s, seat) ==
                   character_for((uint8_t)seat), what);
        }
        snprintf(what, sizeof what, "form(%d): unused seat has no character",
                 count);
        expect(count == 4 ||
               adventure_party_character_for_seat(&s, count) == -1, what);
    }
}

static void test_form_requires_enabled_and_adventure(void) {
    AdventurePartySession s, before;
    AdventurePartyEvent e;

    adventure_party_session_init(&s);
    before = s;
    e = make_event(ADVENTURE_PARTY_EVENT_FORM, 2);
    e.enabled = 0;
    expect(adventure_party_session_apply(&s, &e) ==
           ADVENTURE_PARTY_ERR_NOT_ENABLED,
           "form: refused while feature disabled");
    expect(memcmp(&before, &s, sizeof s) == 0,
           "form: disabled refusal did not mutate");

    e = make_event(ADVENTURE_PARTY_EVENT_FORM, 2);
    e.adventure_selected = 0;
    expect(adventure_party_session_apply(&s, &e) ==
           ADVENTURE_PARTY_ERR_NOT_ADVENTURE,
           "form: refused outside Adventure select");
    expect(memcmp(&before, &s, sizeof s) == 0,
           "form: non-Adventure refusal did not mutate");
}

static void test_form_rejects_bad_participant_counts(void) {
    const int bad[] = { 0, 1, 5 };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        AdventurePartySession s, before;
        char what[96];
        adventure_party_session_init(&s);
        before = s;
        snprintf(what, sizeof what, "form: count %d refused", bad[i]);
        expect(apply(&s, ADVENTURE_PARTY_EVENT_FORM, bad[i]) ==
               ADVENTURE_PARTY_ERR_PARTICIPANT_COUNT, what);
        snprintf(what, sizeof what, "form: count %d did not mutate", bad[i]);
        expect(memcmp(&before, &s, sizeof s) == 0, what);
    }
}

static void test_form_rejects_bad_seat_sets(void) {
    AdventurePartySession s, before;
    AdventurePartyEvent e;

    adventure_party_session_init(&s);
    before = s;

    /* Seat index out of range. */
    e = make_event(ADVENTURE_PARTY_EVENT_FORM, 2);
    e.roster.seat[1] = 4;
    expect(adventure_party_session_apply(&s, &e) ==
           ADVENTURE_PARTY_ERR_SEAT_INVALID, "form: seat 4 refused");
    expect(memcmp(&before, &s, sizeof s) == 0,
           "form: invalid seat did not mutate");

    /* Same seat twice. */
    e = make_event(ADVENTURE_PARTY_EVENT_FORM, 2);
    e.roster.seat[1] = 0;
    expect(adventure_party_session_apply(&s, &e) ==
           ADVENTURE_PARTY_ERR_SEAT_DUPLICATE, "form: duplicate seat refused");
    expect(memcmp(&before, &s, sizeof s) == 0,
           "form: duplicate seat did not mutate");

    /* Sparse: seats {0,2} for a two-player party. */
    e = make_event(ADVENTURE_PARTY_EVENT_FORM, 2);
    e.roster.seat[1] = 2;
    expect(adventure_party_session_apply(&s, &e) ==
           ADVENTURE_PARTY_ERR_SEAT_SPARSE, "form: sparse seats refused");
    expect(memcmp(&before, &s, sizeof s) == 0,
           "form: sparse seats did not mutate");

    /* Host seat absent: seats {1,2}. */
    e = make_event(ADVENTURE_PARTY_EVENT_FORM, 2);
    e.roster.seat[0] = 1;
    e.roster.seat[1] = 2;
    expect(adventure_party_session_apply(&s, &e) ==
           ADVENTURE_PARTY_ERR_SEAT_SPARSE, "form: missing host seat refused");
    expect(memcmp(&before, &s, sizeof s) == 0,
           "form: missing host seat did not mutate");
}

static void test_null_and_out_of_range_arguments(void) {
    AdventurePartySession s;
    AdventurePartyEvent e = make_event(ADVENTURE_PARTY_EVENT_FORM, 2);
    adventure_party_session_init(&s);
    expect(adventure_party_session_apply(NULL, &e) ==
           ADVENTURE_PARTY_ERR_ARGUMENT, "apply: NULL session refused");
    expect(adventure_party_session_apply(&s, NULL) ==
           ADVENTURE_PARTY_ERR_ARGUMENT, "apply: NULL event refused");
    e.kind = (AdventurePartyEventKind)ADVENTURE_PARTY_EVENT_KIND_COUNT;
    expect(adventure_party_session_apply(&s, &e) ==
           ADVENTURE_PARTY_ERR_ARGUMENT, "apply: out-of-range kind refused");
    expect(adventure_party_consume_completion_token(&s, NULL) ==
           ADVENTURE_PARTY_ERR_ARGUMENT, "consume: NULL token refused");
    expect(adventure_party_consume_completion_token(NULL, NULL) ==
           ADVENTURE_PARTY_ERR_ARGUMENT, "consume: NULL session refused");
    expect(adventure_party_is_active(NULL) == 0, "is_active(NULL) is 0");
    expect(adventure_party_participant_count(NULL) == 0,
           "participant_count(NULL) is 0");
    expect(adventure_party_character_for_seat(&s, -1) == -1,
           "character_for_seat: negative seat is -1");
    expect(adventure_party_character_for_seat(&s, 4) == -1,
           "character_for_seat: seat 4 is -1");
}

/* Every legal AND illegal state/event pair. Illegal pairs must return the
 * ILLEGAL_EVENT error specifically (payloads are valid, so nothing else may
 * fire first) and must leave the session bit-for-bit untouched. */
static void test_every_state_event_pair(void) {
    for (int st = 0; st < ADVENTURE_PARTY_SESSION_STATE_COUNT; st++) {
        for (int k = 0; k < ADVENTURE_PARTY_EVENT_KIND_COUNT; k++) {
            AdventurePartySession s, before;
            AdventurePartyEvent e;
            AdventurePartyResult r;
            char what[128];

            drive(&s, (AdventurePartySessionState)st, 3);
            e = make_event((AdventurePartyEventKind)k, 3);
            /* A legal RESTORE_COMMIT must present the suspended roster. */
            if (k == ADVENTURE_PARTY_EVENT_RESTORE_COMMIT &&
                s.has_suspended_roster)
                e.roster = request_from(&s.suspended_roster);
            before = s;
            r = adventure_party_session_apply(&s, &e);

            if (pair_is_legal((AdventurePartySessionState)st,
                              (AdventurePartyEventKind)k)) {
                snprintf(what, sizeof what, "legal: %s + event %d accepted",
                         adventure_party_state_name(
                             (AdventurePartySessionState)st), k);
                expect(r == ADVENTURE_PARTY_OK, what);
            } else {
                snprintf(what, sizeof what,
                         "illegal: %s + event %d refused as ILLEGAL_EVENT",
                         adventure_party_state_name(
                             (AdventurePartySessionState)st), k);
                expect(r == ADVENTURE_PARTY_ERR_ILLEGAL_EVENT, what);
                snprintf(what, sizeof what,
                         "illegal: %s + event %d did not mutate",
                         adventure_party_state_name(
                             (AdventurePartySessionState)st), k);
                expect(memcmp(&before, &s, sizeof s) == 0, what);
            }
        }
    }
}

static void test_level_generation_bumps_exactly_on_level_entry(void) {
    AdventurePartySession s;
    uint32_t g;

    adventure_party_session_init(&s);
    expect(s.level_generation == 0, "gen: starts at 0");
    apply(&s, ADVENTURE_PARTY_EVENT_FORM, 2);
    expect(s.level_generation == 0, "gen: FORM is not a level entry");

    apply(&s, ADVENTURE_PARTY_EVENT_START_NEW_GAME, 2);
    expect(s.level_generation == 1, "gen: new-game scene bumps");
    apply(&s, ADVENTURE_PARTY_EVENT_SCENE_COMPLETE, 2);
    expect(s.level_generation == 2, "gen: lobby after scene bumps");

    g = s.level_generation;
    apply(&s, ADVENTURE_PARTY_EVENT_DIALOGUE_START, 2);
    expect(s.level_generation == g, "gen: dialogue start does not bump");
    apply(&s, ADVENTURE_PARTY_EVENT_DIALOGUE_COMPLETE, 2);
    expect(s.level_generation == g, "gen: dialogue complete does not bump");

    apply(&s, ADVENTURE_PARTY_EVENT_RACE_START, 2);
    expect(s.level_generation == g + 1, "gen: race start bumps");
    apply(&s, ADVENTURE_PARTY_EVENT_RACE_RESULT_COMMITTED, 2);
    expect(s.level_generation == g + 2, "gen: lobby return bumps");

    apply(&s, ADVENTURE_PARTY_EVENT_SOLO_START, 2);
    expect(s.level_generation == g + 3, "gen: solo start bumps");
    apply(&s, ADVENTURE_PARTY_EVENT_SOLO_EXIT, 2);
    expect(s.level_generation == g + 3, "gen: solo exit does not bump");
    {
        AdventurePartyEvent e =
            make_event(ADVENTURE_PARTY_EVENT_RESTORE_COMMIT, 2);
        e.roster = request_from(&s.suspended_roster);
        adventure_party_session_apply(&s, &e);
    }
    expect(s.level_generation == g + 4, "gen: restore commit bumps");

    apply(&s, ADVENTURE_PARTY_EVENT_QUIT, 2);
    expect(s.level_generation == g + 4, "gen: quit does not bump");
    apply(&s, ADVENTURE_PARTY_EVENT_DESTROY, 2);
    expect(s.level_generation == g + 4, "gen: destroy does not bump");
    expect(s.session_generation == 1, "gen: session generation survives destroy");

    /* A second party in the same struct keeps both counters monotonic. */
    apply(&s, ADVENTURE_PARTY_EVENT_FORM, 3);
    expect(s.session_generation == 2, "gen: second session bumps generation");
    expect(s.level_generation == g + 4, "gen: level counter never resets");
}

/* "Starting a second transition in one level generation fails": once the
 * session has left the lobby for a race/solo/dialogue, another departure
 * request in that generation is a typed refusal, not a second transition. */
static void test_second_transition_same_generation_fails(void) {
    AdventurePartySession s, before;

    drive(&s, ADVENTURE_PARTY_STATE_ACTIVE_RACE, 2);
    before = s;
    expect(apply(&s, ADVENTURE_PARTY_EVENT_RACE_START, 2) ==
           ADVENTURE_PARTY_ERR_ILLEGAL_EVENT,
           "second transition: race-within-race refused");
    expect(apply(&s, ADVENTURE_PARTY_EVENT_SOLO_START, 2) ==
           ADVENTURE_PARTY_ERR_ILLEGAL_EVENT,
           "second transition: solo-within-race refused");
    expect(apply(&s, ADVENTURE_PARTY_EVENT_DIALOGUE_START, 2) ==
           ADVENTURE_PARTY_ERR_ILLEGAL_EVENT,
           "second transition: dialogue-within-race refused");
    expect(memcmp(&before, &s, sizeof s) == 0,
           "second transition: refusals did not mutate");
}

static void test_latch_clears_on_bump_and_dialogue_complete(void) {
    AdventurePartySession s;

    drive(&s, ADVENTURE_PARTY_STATE_ACTIVE_LOBBY, 2);
    s.transition_latch.latched = 1;
    s.transition_latch.winner.level_generation = s.level_generation;
    s.transition_latch.winner.initiating_seat = 1;
    apply(&s, ADVENTURE_PARTY_EVENT_RACE_START, 2);
    expect(s.transition_latch.latched == 0,
           "latch: cleared by the generation bump");

    apply(&s, ADVENTURE_PARTY_EVENT_RACE_RESULT_COMMITTED, 2);
    s.transition_latch.latched = 1;
    apply(&s, ADVENTURE_PARTY_EVENT_DIALOGUE_START, 2);
    expect(s.transition_latch.latched == 1,
           "latch: dialogue start leaves the latch (it IS the action)");
    apply(&s, ADVENTURE_PARTY_EVENT_DIALOGUE_COMPLETE, 2);
    expect(s.transition_latch.latched == 0,
           "latch: released when the dialogue completes");
}

/* R16: ACTIVE_LOBBY may re-enter ACTIVE_LOBBY (a lobby->lobby door), through the
 * SAME single enter_level bump every other level entry uses. Two hops must give
 * two generations, each clearing the previous generation's latch, with the party
 * itself untouched — this is what unblocks a second door in a freshly entered
 * lobby (the arbiter latch is otherwise terminal for its departing generation). */
static void test_lobby_transition_two_hop(void) {
    AdventurePartySession s, before;
    uint32_t g0, g1;

    drive(&s, ADVENTURE_PARTY_STATE_ACTIVE_LOBBY, 3);
    g0 = s.level_generation;
    before = s;

    /* A latched door in the departing lobby generation (the first hop's cause). */
    s.transition_latch.latched = 1;
    s.transition_latch.winner.level_generation = s.level_generation;
    s.transition_latch.winner.initiating_seat = 2;

    /* First hop: lobby -> lobby. */
    expect(apply(&s, ADVENTURE_PARTY_EVENT_LOBBY_TRANSITION, 3) ==
           ADVENTURE_PARTY_OK, "lobby-hop: first hop accepted");
    expect(s.state == ADVENTURE_PARTY_STATE_ACTIVE_LOBBY,
           "lobby-hop: still ACTIVE_LOBBY after the hop");
    g1 = s.level_generation;
    expect(g1 == g0 + 1, "lobby-hop: level generation advanced by one");
    expect(s.transition_latch.latched == 0,
           "lobby-hop: latch cleared in the new generation");
    expect(adventure_party_participant_count(&s) == 3,
           "lobby-hop: party count intact across the hop");
    expect(memcmp(&s.roster, &before.roster, sizeof s.roster) == 0,
           "lobby-hop: roster (seats + characters) intact across the hop");

    /* Second hop: a fresh latch in the new generation, then hop again — two
     * distinct latches resolved in two distinct generations. */
    s.transition_latch.latched = 1;
    s.transition_latch.winner.level_generation = s.level_generation;
    s.transition_latch.winner.initiating_seat = 0;
    expect(apply(&s, ADVENTURE_PARTY_EVENT_LOBBY_TRANSITION, 3) ==
           ADVENTURE_PARTY_OK, "lobby-hop: second hop accepted");
    expect(s.level_generation == g1 + 1,
           "lobby-hop: second hop advanced the generation again");
    expect(s.transition_latch.latched == 0,
           "lobby-hop: latch cleared again (two latches, two generations)");
    expect(s.session_generation == 1,
           "lobby-hop: session generation unchanged across the hops");

    /* A lobby transition is a level entry, so it also clears consumed tokens. */
    {
        AdventurePartyCompletionToken t = token_for(&s, 3, 2, 0);
        expect(adventure_party_consume_completion_token(&s, &t) ==
               ADVENTURE_PARTY_OK, "lobby-hop: token consumable after the hop");
        apply(&s, ADVENTURE_PARTY_EVENT_LOBBY_TRANSITION, 3);
        expect(s.consumed_count == 0,
               "lobby-hop: consumed-token list cleared by the hop's bump");
    }
}

/* "Awarding twice fails." */
static void test_awarding_twice_fails(void) {
    AdventurePartySession s;
    AdventurePartyCompletionToken t;

    drive(&s, ADVENTURE_PARTY_STATE_ACTIVE_RACE, 2);
    t = token_for(&s, 7, 2, 0);
    expect(adventure_party_consume_completion_token(&s, &t) ==
           ADVENTURE_PARTY_OK, "award: first consume succeeds");
    expect(adventure_party_consume_completion_token(&s, &t) ==
           ADVENTURE_PARTY_ERR_TOKEN_CONSUMED,
           "award: second consume of the same key fails");

    /* A different key in the same generation is a different award. */
    t.completion_kind = 1;
    expect(adventure_party_consume_completion_token(&s, &t) ==
           ADVENTURE_PARTY_OK, "award: distinct key still consumable");
}

/* "Committing a stale generation fails." */
static void test_stale_generation_commit_fails(void) {
    AdventurePartySession s, before;
    AdventurePartyCompletionToken t;

    drive(&s, ADVENTURE_PARTY_STATE_ACTIVE_RACE, 2);
    t = token_for(&s, 7, 2, 0);

    /* The race ends and the party returns to the lobby: the token's level
     * generation is now stale, so the late commit is refused. */
    apply(&s, ADVENTURE_PARTY_EVENT_RACE_RESULT_COMMITTED, 2);
    before = s;
    expect(adventure_party_consume_completion_token(&s, &t) ==
           ADVENTURE_PARTY_ERR_STALE_GENERATION,
           "stale: old level generation refused");
    expect(memcmp(&before, &s, sizeof s) == 0, "stale: refusal did not mutate");

    /* A token from another session generation is just as dead. */
    t = token_for(&s, 7, 2, 0);
    t.session_generation += 1;
    expect(adventure_party_consume_completion_token(&s, &t) ==
           ADVENTURE_PARTY_ERR_STALE_GENERATION,
           "stale: wrong session generation refused");
}

static void test_token_needs_an_activity_state(void) {
    static const AdventurePartySessionState dead[] = {
        ADVENTURE_PARTY_STATE_OFF,
        ADVENTURE_PARTY_STATE_FORMING,
        ADVENTURE_PARTY_STATE_EXITING,
    };
    for (size_t i = 0; i < sizeof dead / sizeof dead[0]; i++) {
        AdventurePartySession s;
        AdventurePartyCompletionToken t;
        char what[96];
        drive(&s, dead[i], 2);
        t = token_for(&s, 1, 1, 0);
        snprintf(what, sizeof what, "token: refused in %s",
                 adventure_party_state_name(dead[i]));
        expect(adventure_party_consume_completion_token(&s, &t) ==
               ADVENTURE_PARTY_ERR_TOKEN_STATE, what);
    }
}

static void test_token_capacity_is_a_typed_refusal(void) {
    AdventurePartySession s;
    AdventurePartyCompletionToken t;
    drive(&s, ADVENTURE_PARTY_STATE_ACTIVE_RACE, 2);
    for (int i = 0; i < ADVENTURE_PARTY_MAX_CONSUMED_TOKENS; i++) {
        t = token_for(&s, (uint16_t)i, 2, 0);
        char what[96];
        snprintf(what, sizeof what, "capacity: token %d consumable", i);
        expect(adventure_party_consume_completion_token(&s, &t) ==
               ADVENTURE_PARTY_OK, what);
    }
    t = token_for(&s, 999, 2, 0);
    expect(adventure_party_consume_completion_token(&s, &t) ==
           ADVENTURE_PARTY_ERR_TOKEN_CAPACITY,
           "capacity: overflow refused, not silently dropped");
}

static void test_solo_suspend_and_restore_roundtrip(void) {
    AdventurePartySession s;
    AdventurePartyRoster lobby_roster;
    AdventurePartyEvent e;

    drive(&s, ADVENTURE_PARTY_STATE_ACTIVE_LOBBY, 3);
    lobby_roster = s.roster;

    apply(&s, ADVENTURE_PARTY_EVENT_SOLO_START, 3);
    expect(s.has_suspended_roster == 1, "restore: solo start suspends");
    expect(memcmp(&s.suspended_roster, &lobby_roster,
                  sizeof lobby_roster) == 0,
           "restore: suspended facts equal the lobby roster");

    apply(&s, ADVENTURE_PARTY_EVENT_SOLO_EXIT, 3);
    e = make_event(ADVENTURE_PARTY_EVENT_RESTORE_COMMIT, 3);
    e.roster = request_from(&s.suspended_roster);
    expect(adventure_party_session_apply(&s, &e) == ADVENTURE_PARTY_OK,
           "restore: matching roster commits");
    expect(s.state == ADVENTURE_PARTY_STATE_ACTIVE_LOBBY,
           "restore: back in the lobby");
    expect(s.has_suspended_roster == 0, "restore: suspension cleared");
    expect(memcmp(&s.roster, &lobby_roster, sizeof lobby_roster) == 0,
           "restore: roster identical to what was borrowed");
}

/* "Restoring a roster that differs from the suspended one fails" — count,
 * seat and character legs each get a named refusal. */
static void test_restore_mismatches_fail(void) {
    AdventurePartySession s, before;
    AdventurePartyEvent e;

    /* Count differs. */
    drive(&s, ADVENTURE_PARTY_STATE_RESTORING_PARTY, 2);
    before = s;
    e = make_event(ADVENTURE_PARTY_EVENT_RESTORE_COMMIT, 3);
    expect(adventure_party_session_apply(&s, &e) ==
           ADVENTURE_PARTY_ERR_ROSTER_MISMATCH,
           "restore: different count refused");
    expect(memcmp(&before, &s, sizeof s) == 0,
           "restore: count mismatch did not mutate");

    /* Seat set differs. Dense-seat validation makes {0,2} unrepresentable as
     * a healthy roster, so the refusal is the seat-shape error — still a
     * typed refusal, still no mutation, which is the invariant. */
    e = make_event(ADVENTURE_PARTY_EVENT_RESTORE_COMMIT, 2);
    e.roster.seat[1] = 2;
    expect(adventure_party_session_apply(&s, &e) ==
           ADVENTURE_PARTY_ERR_SEAT_SPARSE,
           "restore: different seat set refused");
    expect(memcmp(&before, &s, sizeof s) == 0,
           "restore: seat mismatch did not mutate");

    /* Character differs on one seat. */
    e = make_event(ADVENTURE_PARTY_EVENT_RESTORE_COMMIT, 2);
    e.roster.character[1] = (uint8_t)(e.roster.character[1] + 1);
    expect(adventure_party_session_apply(&s, &e) ==
           ADVENTURE_PARTY_ERR_ROSTER_MISMATCH,
           "restore: swapped character refused");
    expect(memcmp(&before, &s, sizeof s) == 0,
           "restore: character mismatch did not mutate");
}

/* "Changing participant count mid-session fails": there is no event that can
 * do it — re-FORMing anywhere but OFF is illegal, and the count is untouched
 * by every legal event. */
static void test_participant_count_fixed_mid_session(void) {
    AdventurePartySession s;
    drive(&s, ADVENTURE_PARTY_STATE_ACTIVE_LOBBY, 2);
    expect(apply(&s, ADVENTURE_PARTY_EVENT_FORM, 4) ==
           ADVENTURE_PARTY_ERR_ILLEGAL_EVENT,
           "count: re-form mid-session refused");
    expect(adventure_party_participant_count(&s) == 2,
           "count: still the formed party of two");
}

/* "Host seat never changes": across a full campaign walk the host is seat 0
 * after every single accepted event. */
static void test_host_seat_never_changes(void) {
    static const AdventurePartyEventKind walk[] = {
        ADVENTURE_PARTY_EVENT_FORM,
        ADVENTURE_PARTY_EVENT_START_NEW_GAME,
        ADVENTURE_PARTY_EVENT_SCENE_COMPLETE,
        ADVENTURE_PARTY_EVENT_DIALOGUE_START,
        ADVENTURE_PARTY_EVENT_DIALOGUE_COMPLETE,
        ADVENTURE_PARTY_EVENT_RACE_START,
        ADVENTURE_PARTY_EVENT_RACE_RESULT_COMMITTED,
        ADVENTURE_PARTY_EVENT_SOLO_START,
        ADVENTURE_PARTY_EVENT_SOLO_EXIT,
        ADVENTURE_PARTY_EVENT_RESTORE_COMMIT,
        ADVENTURE_PARTY_EVENT_QUIT,
        ADVENTURE_PARTY_EVENT_DESTROY,
    };
    AdventurePartySession s;
    adventure_party_session_init(&s);
    for (size_t i = 0; i < sizeof walk / sizeof walk[0]; i++) {
        AdventurePartyEvent e = make_event(walk[i], 4);
        char what[96];
        if (walk[i] == ADVENTURE_PARTY_EVENT_RESTORE_COMMIT)
            e.roster = request_from(&s.suspended_roster);
        snprintf(what, sizeof what, "host: walk step %zu accepted", i);
        expect(adventure_party_session_apply(&s, &e) == ADVENTURE_PARTY_OK,
               what);
        snprintf(what, sizeof what, "host: still seat 0 after step %zu", i);
        expect(adventure_party_host_seat(&s) == ADVENTURE_PARTY_HOST_SEAT,
               what);
        /* The STORED field too, not just the accessor: the two must never
         * be separately right, or a trace reading the struct directly could
         * disagree with every caller of the query. */
        snprintf(what, sizeof what,
                 "host: stored field still seat 0 after step %zu", i);
        expect(s.host_seat == ADVENTURE_PARTY_HOST_SEAT, what);
    }
}

/* "A race winner never changes seat mappings." */
static void test_race_winner_never_changes_seats(void) {
    AdventurePartySession s, before;
    AdventurePartyEvent e;

    drive(&s, ADVENTURE_PARTY_STATE_ACTIVE_RACE, 2);
    before = s;

    /* An out-of-range winner and an unoccupied seat are typed refusals. */
    e = make_event(ADVENTURE_PARTY_EVENT_RACE_RESULT_COMMITTED, 2);
    e.winner_seat = 7;
    expect(adventure_party_session_apply(&s, &e) ==
           ADVENTURE_PARTY_ERR_WINNER_SEAT, "winner: seat 7 refused");
    e.winner_seat = 3; /* not occupied in a party of two */
    expect(adventure_party_session_apply(&s, &e) ==
           ADVENTURE_PARTY_ERR_WINNER_SEAT, "winner: empty seat refused");
    expect(memcmp(&before, &s, sizeof s) == 0,
           "winner: refusals did not mutate");

    /* A non-host winner commits fine and remaps nothing. */
    e.winner_seat = 1;
    expect(adventure_party_session_apply(&s, &e) == ADVENTURE_PARTY_OK,
           "winner: seat 1 result committed");
    expect(memcmp(&s.roster, &before.roster, sizeof s.roster) == 0,
           "winner: roster identical after a non-host win");
    expect(adventure_party_host_seat(&s) == ADVENTURE_PARTY_HOST_SEAT,
           "winner: host unchanged after a non-host win");
    expect(s.host_seat == ADVENTURE_PARTY_HOST_SEAT,
           "winner: stored host seat field untouched by a non-host win");

    /* No human winner at all (CPUs took it) is a legal result too. */
    apply(&s, ADVENTURE_PARTY_EVENT_RACE_START, 2);
    e = make_event(ADVENTURE_PARTY_EVENT_RACE_RESULT_COMMITTED, 2);
    e.winner_seat = ADVENTURE_PARTY_NO_SEAT;
    expect(adventure_party_session_apply(&s, &e) == ADVENTURE_PARTY_OK,
           "winner: NO_SEAT (CPU win) committed");
    expect(memcmp(&s.roster, &before.roster, sizeof s.roster) == 0,
           "winner: roster identical after a CPU win");
    expect(s.host_seat == ADVENTURE_PARTY_HOST_SEAT,
           "winner: stored host seat field untouched by a CPU win");
}

/* --- seeded property test --------------------------------------------- */

static uint64_t prng_state;
static uint64_t prng_next(void) {
    /* xorshift64*: deterministic, seedable, good enough to shuffle events. */
    uint64_t x = prng_state;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    prng_state = x;
    return x * 0x2545F4914F6CDD1DULL;
}

static void test_property_random_sequences(uint64_t seed,
                                           unsigned long iterations) {
    AdventurePartySession s;
    AdventurePartyRoster shadow;
    int shadow_valid = 0;
    uint32_t prev_session_gen = 0, prev_level_gen = 0;

    printf("property: seed 0x%llx, %lu iterations\n",
           (unsigned long long)seed, iterations);
    prng_state = seed ? seed : 1;
    adventure_party_session_init(&s);
    memset(&shadow, 0, sizeof shadow);

    for (unsigned long i = 0; i < iterations; i++) {
        AdventurePartySession before = s;
        uint64_t r = prng_next();

        if ((r & 7u) == 7u) {
            /* Try a token consume: sometimes current, sometimes stale. */
            AdventurePartyCompletionToken t;
            memset(&t, 0, sizeof t);
            t.session_generation = (r & 16u) ? s.session_generation
                                             : s.session_generation + 1u;
            t.level_generation = (r & 32u) ? s.level_generation
                                           : s.level_generation + 9u;
            t.course = (uint16_t)((r >> 8) & 3u);
            t.activity = (uint8_t)((r >> 10) & 3u);
            t.completion_kind = (uint8_t)((r >> 12) & 1u);
            AdventurePartyResult tr =
                adventure_party_consume_completion_token(&s, &t);
            if (tr != ADVENTURE_PARTY_OK)
                check(memcmp(&before, &s, sizeof s) == 0,
                      "property: refused consume did not mutate", i);
        } else {
            AdventurePartyEvent e;
            uint64_t kind_roll = prng_next();
            memset(&e, 0, sizeof e);
            /* Mostly real kinds; occasionally garbage to hit ARGUMENT. */
            e.kind = (AdventurePartyEventKind)(kind_roll %
                     (ADVENTURE_PARTY_EVENT_KIND_COUNT + 1));
            e.enabled = (prng_next() & 15u) != 0;
            e.adventure_selected = (prng_next() & 15u) != 0;
            {
                uint64_t w = prng_next();
                e.winner_seat = (w & 3u) == 0 ? ADVENTURE_PARTY_NO_SEAT
                                              : (uint8_t)(w % 6u);
            }
            if (e.kind == ADVENTURE_PARTY_EVENT_RESTORE_COMMIT &&
                s.has_suspended_roster && (prng_next() & 1u)) {
                e.roster = request_from(&s.suspended_roster);
            } else {
                uint64_t q = prng_next();
                e.roster.participant_count = (uint8_t)(q % 6u);
                for (int j = 0; j < ADVENTURE_PARTY_MAX_SEATS; j++) {
                    /* 3 in 4 rosters are dense; the rest are hostile. */
                    e.roster.seat[j] = ((q >> 8) & 3u) != 0
                        ? (uint8_t)j
                        : (uint8_t)((q >> (10 + 2 * j)) % 5u);
                    e.roster.character[j] = character_for((uint8_t)j);
                }
            }

            AdventurePartyResult ar = adventure_party_session_apply(&s, &e);
            if (ar != ADVENTURE_PARTY_OK) {
                check(ar < 0, "property: refusal is a typed error", i);
                check(memcmp(&before, &s, sizeof s) == 0,
                      "property: refused event did not mutate", i);
            } else if (e.kind == ADVENTURE_PARTY_EVENT_FORM) {
                shadow = s.roster;
                shadow_valid = 1;
            } else if (e.kind == ADVENTURE_PARTY_EVENT_DESTROY) {
                shadow_valid = 0;
            }
        }

        /* Structural invariants, every iteration. */
        check(s.state < ADVENTURE_PARTY_SESSION_STATE_COUNT,
              "property: state within range", i);
        check(s.session_generation >= prev_session_gen,
              "property: session generation monotonic", i);
        check(s.level_generation >= prev_level_gen,
              "property: level generation monotonic", i);
        prev_session_gen = s.session_generation;
        prev_level_gen = s.level_generation;

        check(adventure_party_host_seat(&s) == ADVENTURE_PARTY_HOST_SEAT,
              "property: host is always seat 0", i);
        check(s.host_seat == ADVENTURE_PARTY_HOST_SEAT,
              "property: stored host seat field is always seat 0", i);
        check(s.has_suspended_roster ==
              (s.state == ADVENTURE_PARTY_STATE_SOLO_ACTIVITY ||
               s.state == ADVENTURE_PARTY_STATE_RESTORING_PARTY),
              "property: suspension tracks solo/restore states", i);
        check(s.consumed_count <= ADVENTURE_PARTY_MAX_CONSUMED_TOKENS,
              "property: consumed list within capacity", i);
        for (int c = 0; c < s.consumed_count; c++)
            check(s.consumed[c].session_generation == s.session_generation &&
                  s.consumed[c].level_generation == s.level_generation,
                  "property: consumed tokens all current", i);

        if (s.state == ADVENTURE_PARTY_STATE_OFF) {
            check(adventure_party_participant_count(&s) == 0,
                  "property: OFF has no participants", i);
        } else {
            int n = adventure_party_participant_count(&s);
            check(n >= ADVENTURE_PARTY_MIN_PARTICIPANTS &&
                  n <= ADVENTURE_PARTY_MAX_PARTICIPANTS,
                  "property: active count within 2..4", i);
            check(s.roster.seat_mask == (uint8_t)((1u << n) - 1u),
                  "property: active seat mask dense", i);
            if (shadow_valid) {
                check(memcmp(&s.roster, &shadow, sizeof shadow) == 0,
                      "property: roster never drifts from the formed one", i);
                for (int seat = 0; seat < n; seat++)
                    check(adventure_party_character_for_seat(&s, seat) ==
                          shadow.character_by_seat[seat],
                          "property: character identity stable", i);
            }
        }
    }
    expect(quiet_failures == 0, "property: no invariant violations");
}

int main(int argc, char **argv) {
    uint64_t seed = 0xC0FFEE64D0C5ULL; /* fixed: reproducible by default */
    if (argc > 1) seed = strtoull(argv[1], NULL, 0);

    test_init_is_off();
    test_form_happy_paths();
    test_form_requires_enabled_and_adventure();
    test_form_rejects_bad_participant_counts();
    test_form_rejects_bad_seat_sets();
    test_null_and_out_of_range_arguments();
    test_every_state_event_pair();
    test_level_generation_bumps_exactly_on_level_entry();
    test_second_transition_same_generation_fails();
    test_lobby_transition_two_hop();
    test_latch_clears_on_bump_and_dialogue_complete();
    test_awarding_twice_fails();
    test_stale_generation_commit_fails();
    test_token_needs_an_activity_state();
    test_token_capacity_is_a_typed_refusal();
    test_solo_suspend_and_restore_roundtrip();
    test_restore_mismatches_fail();
    test_participant_count_fixed_mid_session();
    test_host_seat_never_changes();
    test_race_winner_never_changes_seats();
    test_property_random_sequences(seed, 20000);

    printf("test_adventure_party_state: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
