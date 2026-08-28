/* test_adventure_party_policy.c — AP-03 policy core invariants.
 *
 * ROM-free: links adventure_party_policy.c plus adventure_party_state.c (the
 * exact-once and count-vocabulary tests exercise a real session, because the
 * point of those invariants is how the two modules compose). Covers the v1
 * capability table row by row, fail-closed classification of everything the
 * table does not positively recognise, the three-way count vocabulary, the
 * deterministic transition arbitration reducer, exact-once token issuance
 * across win/loss/quit/retry sequences, and host/action authority. */
#include "adventure_party/adventure_party_policy.h"

#include <stdio.h>
#include <string.h>

static int failures;
static void expect(int c, const char *w) {
    if (!c) { printf("FAIL %s\n", w); failures++; } else printf("ok   %s\n", w);
}

/* --- helpers ---------------------------------------------------------- */

static AdventurePartyActivityDescriptor desc(AdventurePartyRaceKind race,
                                             AdventurePartyCourseClass course) {
    AdventurePartyActivityDescriptor d;
    memset(&d, 0, sizeof d);
    d.race_kind = race;
    d.course_class = course;
    return d;
}

static void expect_fail_closed(const AdventurePartyCapability *cap,
                               const char *label) {
    char what[128];
    snprintf(what, sizeof what, "%s: fails closed (diagnostic flag)", label);
    expect(cap->fail_closed == 1, what);
    snprintf(what, sizeof what, "%s: host-solo presentation", label);
    expect(cap->presentation == ADVENTURE_PARTY_PRESENT_HOST_SOLO, what);
    snprintf(what, sizeof what, "%s: one human", label);
    expect(cap->human_count == 1, what);
    snprintf(what, sizeof what, "%s: one viewport", label);
    expect(cap->viewport_count == 1, what);
    snprintf(what, sizeof what, "%s: no progression guessed", label);
    expect(cap->progress == ADVENTURE_PARTY_PROGRESS_NONE, what);
    snprintf(what, sizeof what, "%s: policy stays active (host-only floor)",
             label);
    expect(cap->policy_active == 1, what);
}

/* Builds a real session with `count` participants, parked in ACTIVE_LOBBY. */
static void live_session(AdventurePartySession *s, int count) {
    AdventurePartyEvent e;
    memset(&e, 0, sizeof e);
    e.kind = ADVENTURE_PARTY_EVENT_FORM;
    e.enabled = 1;
    e.adventure_selected = 1;
    e.winner_seat = ADVENTURE_PARTY_NO_SEAT;
    e.roster.participant_count = (uint8_t)count;
    for (int i = 0; i < count; i++) {
        e.roster.seat[i] = (uint8_t)i;
        e.roster.character[i] = (uint8_t)(20 + i);
    }
    adventure_party_session_init(s);
    expect(adventure_party_session_apply(s, &e) == ADVENTURE_PARTY_OK,
           "helper: session formed");
    memset(&e, 0, sizeof e);
    e.kind = ADVENTURE_PARTY_EVENT_RESUME_SAVE;
    expect(adventure_party_session_apply(s, &e) == ADVENTURE_PARTY_OK,
           "helper: session in lobby");
}

static AdventurePartyTransitionRequest req(uint32_t generation, uint32_t tick,
                                           uint8_t seat, uint16_t destination) {
    AdventurePartyTransitionRequest r;
    memset(&r, 0, sizeof r);
    r.level_generation = generation;
    r.simulation_tick = tick;
    r.trigger_kind = 1;
    r.destination = destination;
    r.entrance = 2;
    r.object_id = 40;
    r.initiating_seat = seat;
    return r;
}

/* --- capability table -------------------------------------------------- */

static void test_capability_table_split_rows(void) {
    /* Lobbies and the two split race rows, at every party size. */
    for (int n = 2; n <= 4; n++) {
        char what[128];
        AdventurePartyActivityDescriptor d;
        AdventurePartyCapability cap;

        d = desc(ADVENTURE_PARTY_RACE_KIND_NONE, ADVENTURE_PARTY_COURSE_LOBBY);
        cap = adventure_party_classify_activity(&d, n);
        snprintf(what, sizeof what, "lobby(%d): split, all humans", n);
        expect(cap.policy_active == 1 && cap.fail_closed == 0 &&
               cap.presentation == ADVENTURE_PARTY_PRESENT_SPLIT &&
               cap.human_count == n && cap.viewport_count == n, what);
        snprintf(what, sizeof what, "lobby(%d): humans only, no CPU racers", n);
        expect(cap.total_racer_count == n, what);
        snprintf(what, sizeof what, "lobby(%d): shared lobby progress", n);
        expect(cap.progress == ADVENTURE_PARTY_PROGRESS_SHARED_LOBBY, what);

        d = desc(ADVENTURE_PARTY_RACE_KIND_DEFAULT,
                 ADVENTURE_PARTY_COURSE_TRACK);
        cap = adventure_party_classify_activity(&d, n);
        snprintf(what, sizeof what, "default race(%d): split, all humans", n);
        expect(cap.policy_active == 1 && cap.fail_closed == 0 &&
               cap.presentation == ADVENTURE_PARTY_PRESENT_SPLIT &&
               cap.human_count == n && cap.viewport_count == n, what);
        snprintf(what, sizeof what,
                 "default race(%d): six total racers (humans + %d CPUs)",
                 n, 6 - n);
        expect(cap.total_racer_count == ADVENTURE_PARTY_RACE_FIELD_TOTAL, what);
        snprintf(what, sizeof what, "default race(%d): any human first", n);
        expect(cap.progress == ADVENTURE_PARTY_PROGRESS_ANY_HUMAN_FIRST, what);

        d = desc(ADVENTURE_PARTY_RACE_KIND_SILVER_COIN,
                 ADVENTURE_PARTY_COURSE_TRACK);
        cap = adventure_party_classify_activity(&d, n);
        snprintf(what, sizeof what, "silver coin(%d): split, six racers", n);
        expect(cap.presentation == ADVENTURE_PARTY_PRESENT_SPLIT &&
               cap.human_count == n && cap.viewport_count == n &&
               cap.total_racer_count == ADVENTURE_PARTY_RACE_FIELD_TOTAL &&
               cap.fail_closed == 0, what);
        snprintf(what, sizeof what,
                 "silver coin(%d): team coins + any human first", n);
        expect(cap.progress ==
               ADVENTURE_PARTY_PROGRESS_TEAM_COINS_ANY_HUMAN_FIRST, what);
    }
}

static void test_capability_table_host_solo_rows(void) {
    static const struct {
        AdventurePartyRaceKind kind;
        const char *label;
    } rows[] = {
        { ADVENTURE_PARTY_RACE_KIND_TAJ_CHALLENGE,    "taj challenge" },
        { ADVENTURE_PARTY_RACE_KIND_BATTLE_CHALLENGE, "battle challenge" },
        { ADVENTURE_PARTY_RACE_KIND_EGG_CHALLENGE,    "egg challenge" },
        { ADVENTURE_PARTY_RACE_KIND_BANANA_CHALLENGE, "banana challenge" },
        { ADVENTURE_PARTY_RACE_KIND_BOSS,             "boss" },
    };
    for (size_t i = 0; i < sizeof rows / sizeof rows[0]; i++) {
        for (int n = 2; n <= 4; n++) {
            char what[128];
            AdventurePartyActivityDescriptor d =
                desc(rows[i].kind, ADVENTURE_PARTY_COURSE_TRACK);
            AdventurePartyCapability cap =
                adventure_party_classify_activity(&d, n);
            snprintf(what, sizeof what, "%s(%d): host-solo one viewport",
                     rows[i].label, n);
            expect(cap.policy_active == 1 && cap.fail_closed == 0 &&
                   cap.presentation == ADVENTURE_PARTY_PRESENT_HOST_SOLO &&
                   cap.human_count == 1 && cap.viewport_count == 1, what);
            snprintf(what, sizeof what, "%s(%d): retail field untouched",
                     rows[i].label, n);
            expect(cap.total_racer_count == 0, what);
            snprintf(what, sizeof what,
                     "%s(%d): existing progression exactly once",
                     rows[i].label, n);
            expect(cap.progress ==
                   ADVENTURE_PARTY_PROGRESS_HOST_EXISTING_ONCE, what);
        }
    }
}

static void test_capability_table_trophy_row(void) {
    for (int n = 2; n <= 4; n++) {
        char what[128];
        AdventurePartyActivityDescriptor d =
            desc(ADVENTURE_PARTY_RACE_KIND_TROPHY,
                 ADVENTURE_PARTY_COURSE_TRACK);
        AdventurePartyCapability cap = adventure_party_classify_activity(&d, n);
        snprintf(what, sizeof what, "trophy(%d): split, all humans", n);
        expect(cap.policy_active == 1 && cap.fail_closed == 0 &&
               cap.presentation == ADVENTURE_PARTY_PRESENT_SPLIT &&
               cap.human_count == n && cap.viewport_count == n, what);
        snprintf(what, sizeof what,
                 "trophy(%d): field size deferred to AP-16, not asserted", n);
        expect(cap.field_size_undecided == 1 && cap.total_racer_count == 0,
               what);
        snprintf(what, sizeof what, "trophy(%d): one shared series result", n);
        expect(cap.progress == ADVENTURE_PARTY_PROGRESS_SHARED_SERIES_RESULT,
               what);
    }
    /* No other row defers its field size. */
    {
        AdventurePartyActivityDescriptor d =
            desc(ADVENTURE_PARTY_RACE_KIND_DEFAULT,
                 ADVENTURE_PARTY_COURSE_TRACK);
        AdventurePartyCapability cap = adventure_party_classify_activity(&d, 2);
        expect(cap.field_size_undecided == 0,
               "default race: field size decided (six)");
    }
}

static void test_capability_table_cutscene_row(void) {
    for (int n = 2; n <= 4; n++) {
        char what[128];
        AdventurePartyActivityDescriptor d =
            desc(ADVENTURE_PARTY_RACE_KIND_NONE,
                 ADVENTURE_PARTY_COURSE_CUTSCENE);
        AdventurePartyCapability cap = adventure_party_classify_activity(&d, n);
        snprintf(what, sizeof what,
                 "cutscene(%d): one viewport, no live party input", n);
        expect(cap.policy_active == 1 && cap.fail_closed == 0 &&
               cap.presentation ==
                   ADVENTURE_PARTY_PRESENT_ONE_VIEWPORT_NO_INPUT &&
               cap.human_count == 0 && cap.viewport_count == 1, what);
        snprintf(what, sizeof what, "cutscene(%d): flags exactly once", n);
        expect(cap.progress == ADVENTURE_PARTY_PROGRESS_CUTSCENE_FLAGS_ONCE,
               what);
    }
}

static void test_capability_table_policy_not_active(void) {
    /* Tracks menu / time trial / demo: the stock game, whatever the race
     * kind claims — party policy simply is not running there. */
    static const AdventurePartyRaceKind kinds[] = {
        ADVENTURE_PARTY_RACE_KIND_NONE,
        ADVENTURE_PARTY_RACE_KIND_DEFAULT,
        ADVENTURE_PARTY_RACE_KIND_BOSS,
    };
    for (size_t i = 0; i < sizeof kinds / sizeof kinds[0]; i++) {
        char what[128];
        AdventurePartyActivityDescriptor d =
            desc(kinds[i], ADVENTURE_PARTY_COURSE_NON_ADVENTURE);
        AdventurePartyCapability cap = adventure_party_classify_activity(&d, 3);
        snprintf(what, sizeof what,
                 "non-adventure kind %d: policy not active", (int)kinds[i]);
        expect(cap.policy_active == 0 &&
               cap.presentation == ADVENTURE_PARTY_PRESENT_RETAIL &&
               cap.fail_closed == 0 &&
               cap.progress == ADVENTURE_PARTY_PROGRESS_NONE, what);
    }
}

static void test_unknown_activities_fail_closed(void) {
    AdventurePartyActivityDescriptor d;
    AdventurePartyCapability cap;

    d = desc(ADVENTURE_PARTY_RACE_KIND_UNKNOWN, ADVENTURE_PARTY_COURSE_TRACK);
    cap = adventure_party_classify_activity(&d, 4);
    expect_fail_closed(&cap, "unknown race kind");

    d = desc(ADVENTURE_PARTY_RACE_KIND_DEFAULT,
             ADVENTURE_PARTY_COURSE_UNKNOWN);
    cap = adventure_party_classify_activity(&d, 4);
    expect_fail_closed(&cap, "unknown course class");

    /* Incoherent pairs the table never authored. */
    d = desc(ADVENTURE_PARTY_RACE_KIND_BOSS, ADVENTURE_PARTY_COURSE_LOBBY);
    cap = adventure_party_classify_activity(&d, 4);
    expect_fail_closed(&cap, "boss-in-a-lobby");

    d = desc(ADVENTURE_PARTY_RACE_KIND_NONE, ADVENTURE_PARTY_COURSE_TRACK);
    cap = adventure_party_classify_activity(&d, 4);
    expect_fail_closed(&cap, "track with no race kind");

    d = desc(ADVENTURE_PARTY_RACE_KIND_DEFAULT,
             ADVENTURE_PARTY_COURSE_CUTSCENE);
    cap = adventure_party_classify_activity(&d, 4);
    expect_fail_closed(&cap, "race-in-a-cutscene");

    /* Out-of-range enum values from a future or corrupt caller. */
    d = desc((AdventurePartyRaceKind)99, ADVENTURE_PARTY_COURSE_TRACK);
    cap = adventure_party_classify_activity(&d, 4);
    expect_fail_closed(&cap, "out-of-range race kind");

    d = desc(ADVENTURE_PARTY_RACE_KIND_DEFAULT,
             (AdventurePartyCourseClass)99);
    cap = adventure_party_classify_activity(&d, 4);
    expect_fail_closed(&cap, "out-of-range course class");

    /* Broken party sizes and a missing descriptor fail closed too. */
    d = desc(ADVENTURE_PARTY_RACE_KIND_DEFAULT, ADVENTURE_PARTY_COURSE_TRACK);
    cap = adventure_party_classify_activity(&d, 0);
    expect_fail_closed(&cap, "participant count 0");
    cap = adventure_party_classify_activity(&d, 1);
    expect_fail_closed(&cap, "participant count 1");
    cap = adventure_party_classify_activity(&d, 5);
    expect_fail_closed(&cap, "participant count 5");
    cap = adventure_party_classify_activity(NULL, 3);
    expect_fail_closed(&cap, "NULL descriptor");
}

/* --- count vocabulary --------------------------------------------------- */

/* The canonical proof the three counts are not substitutes: a host-solo boss
 * for a party of four is participants=4, humans=1, viewports=1 — three
 * different questions, three different answers. */
static void test_counts_are_not_substitutable(void) {
    AdventurePartySession s;
    AdventurePartyActivityDescriptor d;
    AdventurePartyCapability boss, scene;

    live_session(&s, 4);
    d = desc(ADVENTURE_PARTY_RACE_KIND_BOSS, ADVENTURE_PARTY_COURSE_TRACK);
    boss = adventure_party_classify_activity(&d,
               adventure_party_participant_count(&s));

    expect(adventure_party_participant_count(&s) == 4,
           "counts: participants stay 4 during a host-solo boss");
    expect(adventure_party_activity_human_count(&boss) == 1,
           "counts: boss has one live human");
    expect(adventure_party_viewport_count(&boss) == 1,
           "counts: boss has one viewport");
    expect(adventure_party_participant_count(&s) !=
           adventure_party_activity_human_count(&boss),
           "counts: participant count is not the human count");

    /* And a cutscene splits the other pair: one viewport, zero humans. */
    d = desc(ADVENTURE_PARTY_RACE_KIND_NONE, ADVENTURE_PARTY_COURSE_CUTSCENE);
    scene = adventure_party_classify_activity(&d,
                adventure_party_participant_count(&s));
    expect(adventure_party_activity_human_count(&scene) == 0 &&
           adventure_party_viewport_count(&scene) == 1,
           "counts: viewport count is not the human count");
    expect(adventure_party_activity_human_count(NULL) == 0 &&
           adventure_party_viewport_count(NULL) == 0,
           "counts: NULL capability answers 0");
}

/* --- transition arbitration --------------------------------------------- */

static void test_arbitration_first_request_latches(void) {
    AdventurePartyTransitionLatch latch;
    AdventurePartyTransitionRequest a = req(5, 100, 1, 7);
    memset(&latch, 0, sizeof latch);

    expect(adventure_party_arbitrate_transition(&latch, &a, 5, 0x0F) ==
           ADVENTURE_PARTY_ARBITRATE_LATCHED,
           "arbitration: first valid request latches");
    expect(latch.latched == 1 && latch.winner.initiating_seat == 1 &&
           latch.winner.destination == 7,
           "arbitration: latch records the winner");
}

static void test_arbitration_same_tick_lowest_seat_wins(void) {
    AdventurePartyTransitionLatch latch, other;
    AdventurePartyTransitionRequest seat2 = req(5, 100, 2, 7);
    AdventurePartyTransitionRequest seat0 = req(5, 100, 0, 9);

    /* Order one: seat 2 first, then seat 0 — seat 0 takes the latch. */
    memset(&latch, 0, sizeof latch);
    adventure_party_arbitrate_transition(&latch, &seat2, 5, 0x0F);
    expect(adventure_party_arbitrate_transition(&latch, &seat0, 5, 0x0F) ==
           ADVENTURE_PARTY_ARBITRATE_LATCHED,
           "arbitration: same-tick lower seat replaces the winner");
    expect(latch.winner.initiating_seat == 0 && latch.winner.destination == 9,
           "arbitration: lower seat's request is the published one");

    /* Order two: seat 0 first — seat 2 is refused, winner unchanged. */
    memset(&other, 0, sizeof other);
    adventure_party_arbitrate_transition(&other, &seat0, 5, 0x0F);
    expect(adventure_party_arbitrate_transition(&other, &seat2, 5, 0x0F) ==
           ADVENTURE_PARTY_ARBITRATE_REJECTED_LATCHED,
           "arbitration: same-tick higher seat rejected");

    /* Both orders converge on the identical winner: iteration order is not
     * an authority rule. */
    expect(memcmp(&latch.winner, &other.winner, sizeof latch.winner) == 0,
           "arbitration: winner independent of arrival order");
}

static void test_arbitration_rejects_after_latch(void) {
    AdventurePartyTransitionLatch latch, before;
    AdventurePartyTransitionRequest a = req(5, 100, 0, 7);
    AdventurePartyTransitionRequest late = req(5, 101, 1, 8);

    memset(&latch, 0, sizeof latch);
    adventure_party_arbitrate_transition(&latch, &a, 5, 0x0F);
    before = latch;
    expect(adventure_party_arbitrate_transition(&latch, &late, 5, 0x0F) ==
           ADVENTURE_PARTY_ARBITRATE_REJECTED_LATCHED,
           "arbitration: later tick refused for this generation");
    expect(memcmp(&before, &latch, sizeof latch) == 0,
           "arbitration: refusal did not disturb the latch");

    /* An earlier tick surfacing late still wins: determinism over arrival. */
    {
        AdventurePartyTransitionRequest early = req(5, 99, 3, 4);
        expect(adventure_party_arbitrate_transition(&latch, &early, 5, 0x0F) ==
               ADVENTURE_PARTY_ARBITRATE_LATCHED,
               "arbitration: earliest tick wins regardless of arrival");
        expect(latch.winner.simulation_tick == 99,
               "arbitration: latch follows the earliest tick");
    }
}

static void test_arbitration_stale_generation_and_bad_seats(void) {
    AdventurePartyTransitionLatch latch, before;
    AdventurePartyTransitionRequest stale = req(4, 100, 0, 7);
    AdventurePartyTransitionRequest unseated = req(5, 100, 3, 7);
    AdventurePartyTransitionRequest silly = req(5, 100, 9, 7);

    memset(&latch, 0, sizeof latch);
    before = latch;
    expect(adventure_party_arbitrate_transition(&latch, &stale, 5, 0x0F) ==
           ADVENTURE_PARTY_ARBITRATE_REJECTED_STALE,
           "arbitration: stale level generation refused");
    expect(adventure_party_arbitrate_transition(&latch, &unseated, 5, 0x03) ==
           ADVENTURE_PARTY_ARBITRATE_REJECTED_SEAT,
           "arbitration: seat outside the active mask refused");
    expect(adventure_party_arbitrate_transition(&latch, &silly, 5, 0x0F) ==
           ADVENTURE_PARTY_ARBITRATE_REJECTED_SEAT,
           "arbitration: seat 9 refused");
    expect(adventure_party_arbitrate_transition(NULL, &stale, 5, 0x0F) ==
           ADVENTURE_PARTY_ARBITRATE_REJECTED_ARGUMENT,
           "arbitration: NULL latch refused");
    expect(adventure_party_arbitrate_transition(&latch, NULL, 5, 0x0F) ==
           ADVENTURE_PARTY_ARBITRATE_REJECTED_ARGUMENT,
           "arbitration: NULL request refused");
    expect(memcmp(&before, &latch, sizeof latch) == 0,
           "arbitration: every refusal left the latch untouched");
}

/* --- exact-once issuance ------------------------------------------------ */

static void test_token_issued_only_for_team_win(void) {
    AdventurePartyCompletionToken t;
    memset(&t, 0xAB, sizeof t);

    expect(adventure_party_completion_token_issue(
               ADVENTURE_PARTY_OUTCOME_LOSS, 1, 5, 3,
               ADVENTURE_PARTY_RACE_KIND_DEFAULT,
               ADVENTURE_PARTY_COMPLETION_COURSE, &t) == 0,
           "issue: loss never issues");
    expect(adventure_party_completion_token_issue(
               ADVENTURE_PARTY_OUTCOME_QUIT, 1, 5, 3,
               ADVENTURE_PARTY_RACE_KIND_DEFAULT,
               ADVENTURE_PARTY_COMPLETION_COURSE, &t) == 0,
           "issue: quit never issues");
    expect(adventure_party_completion_token_issue(
               ADVENTURE_PARTY_OUTCOME_RETRY, 1, 5, 3,
               ADVENTURE_PARTY_RACE_KIND_DEFAULT,
               ADVENTURE_PARTY_COMPLETION_COURSE, &t) == 0,
           "issue: retry never issues");
    {
        AdventurePartyCompletionToken untouched;
        memset(&untouched, 0xAB, sizeof untouched);
        expect(memcmp(&t, &untouched, sizeof t) == 0,
               "issue: refused outcomes left the token untouched");
    }

    expect(adventure_party_completion_token_issue(
               ADVENTURE_PARTY_OUTCOME_TEAM_WIN, 1, 5, 3,
               ADVENTURE_PARTY_RACE_KIND_DEFAULT,
               ADVENTURE_PARTY_COMPLETION_COURSE, &t) == 1,
           "issue: team win issues");
    expect(t.session_generation == 1 && t.level_generation == 5 &&
           t.course == 3 &&
           t.activity == (uint8_t)ADVENTURE_PARTY_RACE_KIND_DEFAULT &&
           t.completion_kind == (uint8_t)ADVENTURE_PARTY_COMPLETION_COURSE,
           "issue: token carries the full five-part key");
    expect(adventure_party_completion_token_issue(
               ADVENTURE_PARTY_OUTCOME_TEAM_WIN, 1, 5, 3,
               ADVENTURE_PARTY_RACE_KIND_DEFAULT,
               ADVENTURE_PARTY_COMPLETION_COURSE, NULL) == 0,
           "issue: NULL out refused");
    expect(adventure_party_completion_token_issue(
               ADVENTURE_PARTY_OUTCOME_TEAM_WIN, 1, 5, 3,
               (AdventurePartyRaceKind)99,
               ADVENTURE_PARTY_COMPLETION_COURSE, &t) == 0,
           "issue: out-of-range activity refused");
    expect(adventure_party_completion_token_issue(
               ADVENTURE_PARTY_OUTCOME_TEAM_WIN, 1, 5, 3,
               ADVENTURE_PARTY_RACE_KIND_DEFAULT,
               (AdventurePartyCompletionKind)99, &t) == 0,
           "issue: out-of-range completion kind refused");
}

/* Full loss -> retry -> win -> duplicate-commit sequence through a real
 * session: the losing and retried runs have nothing to consume, the winning
 * run commits exactly once. */
static void test_exact_once_across_loss_retry_win(void) {
    AdventurePartySession s;
    AdventurePartyEvent e;
    AdventurePartyCompletionToken t;
    const uint16_t course = 11;

    live_session(&s, 3);

    /* Attempt one: the party loses. No token exists, nothing to consume. */
    memset(&e, 0, sizeof e);
    e.kind = ADVENTURE_PARTY_EVENT_RACE_START;
    adventure_party_session_apply(&s, &e);
    expect(adventure_party_completion_token_issue(
               ADVENTURE_PARTY_OUTCOME_LOSS, s.session_generation,
               s.level_generation, course, ADVENTURE_PARTY_RACE_KIND_DEFAULT,
               ADVENTURE_PARTY_COMPLETION_COURSE, &t) == 0,
           "sequence: losing run issues no token");
    memset(&e, 0, sizeof e);
    e.kind = ADVENTURE_PARTY_EVENT_RACE_RESULT_COMMITTED;
    e.winner_seat = ADVENTURE_PARTY_NO_SEAT;
    adventure_party_session_apply(&s, &e);

    /* Attempt two: won. One token, one consume; the second consume and a
     * post-exit commit both refuse. */
    memset(&e, 0, sizeof e);
    e.kind = ADVENTURE_PARTY_EVENT_RACE_START;
    adventure_party_session_apply(&s, &e);
    expect(adventure_party_completion_token_issue(
               ADVENTURE_PARTY_OUTCOME_TEAM_WIN, s.session_generation,
               s.level_generation, course, ADVENTURE_PARTY_RACE_KIND_DEFAULT,
               ADVENTURE_PARTY_COMPLETION_COURSE, &t) == 1,
           "sequence: winning run issues the token");
    expect(adventure_party_consume_completion_token(&s, &t) ==
           ADVENTURE_PARTY_OK, "sequence: award commits once");
    expect(adventure_party_consume_completion_token(&s, &t) ==
           ADVENTURE_PARTY_ERR_TOKEN_CONSUMED,
           "sequence: simultaneous second finish cannot award again");

    memset(&e, 0, sizeof e);
    e.kind = ADVENTURE_PARTY_EVENT_RACE_RESULT_COMMITTED;
    e.winner_seat = 1;
    adventure_party_session_apply(&s, &e);
    expect(adventure_party_consume_completion_token(&s, &t) ==
           ADVENTURE_PARTY_ERR_STALE_GENERATION,
           "sequence: late replayed commit is stale after the level ends");
}

/* --- host / action authority -------------------------------------------- */

static void test_action_authority(void) {
    const uint8_t mask3 = 0x07; /* party of three: seats 0,1,2 */

    expect(adventure_party_seat_may_act(0, mask3,
               ADVENTURE_PARTY_ACTION_DIALOGUE_CHOICE) == 1,
           "authority: host chooses dialogue");
    expect(adventure_party_seat_may_act(1, mask3,
               ADVENTURE_PARTY_ACTION_DIALOGUE_CHOICE) == 0,
           "authority: guest cannot choose dialogue");
    expect(adventure_party_seat_may_act(0, mask3,
               ADVENTURE_PARTY_ACTION_PAUSE_DECISION) == 1,
           "authority: host decides pause actions");
    expect(adventure_party_seat_may_act(2, mask3,
               ADVENTURE_PARTY_ACTION_PAUSE_DECISION) == 0,
           "authority: guest cannot decide pause actions");
    expect(adventure_party_seat_may_act(2, mask3,
               ADVENTURE_PARTY_ACTION_TRIGGER_TRANSITION) == 1,
           "authority: any participant triggers doors");
    expect(adventure_party_seat_may_act(1, mask3,
               ADVENTURE_PARTY_ACTION_COLLECT) == 1,
           "authority: any participant collects");
    expect(adventure_party_seat_may_act(3, mask3,
               ADVENTURE_PARTY_ACTION_COLLECT) == 0,
           "authority: an unoccupied seat may do nothing");
    expect(adventure_party_seat_may_act(-1, mask3,
               ADVENTURE_PARTY_ACTION_COLLECT) == 0,
           "authority: negative seat may do nothing");
    expect(adventure_party_seat_may_act(9, mask3,
               ADVENTURE_PARTY_ACTION_TRIGGER_TRANSITION) == 0,
           "authority: out-of-range seat may do nothing");
}

/* AP-10 controller-disconnect pause authority. Pure decision: the shared pause
 * is held iff any BOUND seat's pad is absent. Underpins the game adapter's
 * "force pause + block unpause until every bound pad returns". */
static void test_disconnect_should_pause(void) {
    const uint8_t mask3 = 0x7; /* seats 0,1,2 bound */
    expect(adventure_party_disconnect_should_pause(mask3, 0x7) == 0,
           "disconnect: every bound pad present -> no pause");
    expect(adventure_party_disconnect_should_pause(mask3, 0x3) == 1,
           "disconnect: a bound pad (seat 2) missing -> hold pause");
    expect(adventure_party_disconnect_should_pause(mask3, 0x0) == 1,
           "disconnect: all bound pads missing -> hold pause");
    expect(adventure_party_disconnect_should_pause(mask3, 0x6) == 1,
           "disconnect: host pad missing -> hold pause");
    /* A present bit for an UNBOUND seat never forces or clears a pause. */
    expect(adventure_party_disconnect_should_pause(0x3, 0xF) == 0,
           "disconnect: spare port present, all bound present -> no pause");
    expect(adventure_party_disconnect_should_pause(0x3, 0x8) == 1,
           "disconnect: bound seats absent despite a spare present -> hold pause");
    expect(adventure_party_disconnect_should_pause(0x0, 0x0) == 0,
           "disconnect: no bound seats -> never a pause");
}

int main(void) {
    test_capability_table_split_rows();
    test_capability_table_host_solo_rows();
    test_capability_table_trophy_row();
    test_capability_table_cutscene_row();
    test_capability_table_policy_not_active();
    test_unknown_activities_fail_closed();
    test_counts_are_not_substitutable();
    test_arbitration_first_request_latches();
    test_arbitration_same_tick_lowest_seat_wins();
    test_arbitration_rejects_after_latch();
    test_arbitration_stale_generation_and_bad_seats();
    test_token_issued_only_for_team_win();
    test_exact_once_across_loss_retry_win();
    test_action_authority();
    test_disconnect_should_pause();

    printf("test_adventure_party_policy: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
