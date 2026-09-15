/* AP-19 deterministic formation/dissolution churn.
 *
 * This is compiled and run by check_adventure_party_performance.py against the
 * production reducer. It deliberately has no renderer, ROM, controller, or
 * allocator dependency: the fixed-size session value must survive twenty
 * thousand complete lifetimes without retaining roster, suspension, latch, or
 * completion-token state. */
#include "adventure_party/adventure_party_state.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CHURN_CYCLES 20000u

static unsigned failures;

static AdventurePartyRosterRequest dense_roster(unsigned count) {
    AdventurePartyRosterRequest roster;
    unsigned seat;
    memset(&roster, 0, sizeof(roster));
    roster.participant_count = (uint8_t)count;
    for (seat = 0; seat < count; seat++) {
        roster.seat[seat] = (uint8_t)seat;
        roster.character[seat] = (uint8_t)(seat + 1u);
    }
    return roster;
}

static AdventurePartyResult apply(AdventurePartySession *session,
                                  AdventurePartyEventKind kind,
                                  unsigned count) {
    AdventurePartyEvent event;
    memset(&event, 0, sizeof(event));
    event.kind = kind;
    event.enabled = 1;
    event.adventure_selected = 1;
    event.winner_seat = ADVENTURE_PARTY_NO_SEAT;
    event.roster = dense_roster(count);
    if (kind == ADVENTURE_PARTY_EVENT_RESTORE_COMMIT) {
        unsigned seat;
        event.roster.participant_count =
            session->suspended_roster.participant_count;
        for (seat = 0; seat < event.roster.participant_count; seat++) {
            event.roster.seat[seat] = (uint8_t)seat;
            event.roster.character[seat] =
                session->suspended_roster.character_by_seat[seat];
        }
    }
    return adventure_party_session_apply(session, &event);
}

static void require(int condition) {
    if (!condition) {
        failures++;
    }
}

int main(void) {
    AdventurePartySession session;
    uint32_t expected_level_generation = 0;
    unsigned cycle;

    adventure_party_session_init(&session);
    for (cycle = 0; cycle < CHURN_CYCLES; cycle++) {
        const unsigned count = 2u + cycle % 3u;
        AdventurePartyCompletionToken token;

        require(apply(&session, ADVENTURE_PARTY_EVENT_FORM, count) ==
                ADVENTURE_PARTY_OK);
        require(session.session_generation == cycle + 1u);
        require(session.roster.participant_count == count);
        require(session.roster.seat_mask == (uint8_t)((1u << count) - 1u));

        require(apply(&session, ADVENTURE_PARTY_EVENT_RESUME_SAVE, count) ==
                ADVENTURE_PARTY_OK);
        expected_level_generation++;
        require(apply(&session, ADVENTURE_PARTY_EVENT_RACE_START, count) ==
                ADVENTURE_PARTY_OK);
        expected_level_generation++;

        memset(&token, 0, sizeof(token));
        token.session_generation = session.session_generation;
        token.level_generation = session.level_generation;
        token.course = (uint16_t)(cycle & 0xFFFFu);
        token.activity = 2u;
        require(adventure_party_consume_completion_token(&session, &token) ==
                ADVENTURE_PARTY_OK);
        require(session.consumed_count == 1u);

        require(apply(&session,
                      ADVENTURE_PARTY_EVENT_RACE_RESULT_COMMITTED,
                      count) == ADVENTURE_PARTY_OK);
        expected_level_generation++;
        require(session.consumed_count == 0u);

        if ((cycle % 4u) == 0u) {
            require(apply(&session, ADVENTURE_PARTY_EVENT_SOLO_START, count) ==
                    ADVENTURE_PARTY_OK);
            expected_level_generation++;
            require(session.has_suspended_roster == 1u);
            require(apply(&session, ADVENTURE_PARTY_EVENT_SOLO_EXIT, count) ==
                    ADVENTURE_PARTY_OK);
            require(apply(&session,
                          ADVENTURE_PARTY_EVENT_RESTORE_COMMIT,
                          count) == ADVENTURE_PARTY_OK);
            expected_level_generation++;
        }

        require(apply(&session, ADVENTURE_PARTY_EVENT_QUIT, count) ==
                ADVENTURE_PARTY_OK);
        require(apply(&session, ADVENTURE_PARTY_EVENT_DESTROY, count) ==
                ADVENTURE_PARTY_OK);
        require(session.state == ADVENTURE_PARTY_STATE_OFF);
        require(session.session_generation == cycle + 1u);
        require(session.level_generation == expected_level_generation);
        require(session.roster.participant_count == 0u);
        require(session.roster.seat_mask == 0u);
        require(session.has_suspended_roster == 0u);
        require(session.suspended_roster.participant_count == 0u);
        require(session.transition_latch.latched == 0u);
        require(session.consumed_count == 0u);
    }

    printf("aparty_perf_churn: cycles=%u sgen=%u lgen=%u bytes=%u failures=%u\n",
           CHURN_CYCLES, (unsigned)session.session_generation,
           (unsigned)session.level_generation, (unsigned)sizeof(session),
           failures);
    return failures == 0u ? 0 : 1;
}
