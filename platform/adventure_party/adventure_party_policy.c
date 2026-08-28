/* adventure_party_policy.c — see adventure_party_policy.h.
 *
 * Everything here is a straight-line function of its arguments. The
 * capability classifier is written as one positive match per authored table
 * row with a single shared fail-closed fallthrough, so adding a row is
 * additive and forgetting one is safe: an unmatched descriptor cannot fall
 * into a neighbouring row, only into host-only.
 */
#include "adventure_party/adventure_party_policy.h"

#include <string.h>

/* The fail-closed floor: one viewport, the host, no progression, flagged.
 * Used for UNKNOWN kinds, incoherent pairs, broken party sizes and NULL —
 * anything the v1 table did not positively author. */
static AdventurePartyCapability fail_closed_row(void) {
    AdventurePartyCapability cap;
    memset(&cap, 0, sizeof cap);
    cap.policy_active = 1;
    cap.presentation = ADVENTURE_PARTY_PRESENT_HOST_SOLO;
    cap.human_count = 1;
    cap.viewport_count = 1;
    cap.total_racer_count = 0;
    cap.fail_closed = 1;
    cap.progress = ADVENTURE_PARTY_PROGRESS_NONE;
    return cap;
}

AdventurePartyCapability adventure_party_classify_activity(
    const AdventurePartyActivityDescriptor *descriptor,
    int participant_count) {
    AdventurePartyCapability cap;
    memset(&cap, 0, sizeof cap);

    if (!descriptor ||
        participant_count < ADVENTURE_PARTY_MIN_PARTICIPANTS ||
        participant_count > ADVENTURE_PARTY_MAX_PARTICIPANTS)
        return fail_closed_row();

    /* Outside Adventure (Tracks menu, time trial, demo) the party policy is
     * simply not running — whatever the race kind claims. This is the one
     * row that is not fail-closed AND not active: it is the stock game. */
    if (descriptor->course_class == ADVENTURE_PARTY_COURSE_NON_ADVENTURE) {
        cap.policy_active = 0;
        cap.presentation = ADVENTURE_PARTY_PRESENT_RETAIL;
        cap.progress = ADVENTURE_PARTY_PROGRESS_NONE;
        return cap;
    }

    cap.policy_active = 1;

    /* Lobbies: everyone is present and split; the only racers are the
     * humans, and what persists is the shared lobby state itself. */
    if (descriptor->course_class == ADVENTURE_PARTY_COURSE_LOBBY &&
        descriptor->race_kind == ADVENTURE_PARTY_RACE_KIND_NONE) {
        cap.presentation = ADVENTURE_PARTY_PRESENT_SPLIT;
        cap.human_count = (uint8_t)participant_count;
        cap.viewport_count = (uint8_t)participant_count;
        cap.total_racer_count = (uint8_t)participant_count;
        cap.progress = ADVENTURE_PARTY_PROGRESS_SHARED_LOBBY;
        return cap;
    }

    /* Cutscenes: one authored camera and nobody drives. Zero humans and one
     * viewport in the same row is the standing proof that those two counts
     * are different questions. */
    if (descriptor->course_class == ADVENTURE_PARTY_COURSE_CUTSCENE &&
        descriptor->race_kind == ADVENTURE_PARTY_RACE_KIND_NONE) {
        cap.presentation = ADVENTURE_PARTY_PRESENT_ONE_VIEWPORT_NO_INPUT;
        cap.human_count = 0;
        cap.viewport_count = 1;
        cap.total_racer_count = 0;
        cap.progress = ADVENTURE_PARTY_PROGRESS_CUTSCENE_FLAGS_ONCE;
        return cap;
    }

    if (descriptor->course_class == ADVENTURE_PARTY_COURSE_TRACK) {
        switch (descriptor->race_kind) {
        case ADVENTURE_PARTY_RACE_KIND_DEFAULT:
        case ADVENTURE_PARTY_RACE_KIND_SILVER_COIN:
            /* The retail Adventure field: six racers, always — the party's
             * humans plus enough CPUs to make six. */
            cap.presentation = ADVENTURE_PARTY_PRESENT_SPLIT;
            cap.human_count = (uint8_t)participant_count;
            cap.viewport_count = (uint8_t)participant_count;
            cap.total_racer_count = ADVENTURE_PARTY_RACE_FIELD_TOTAL;
            cap.progress =
                descriptor->race_kind == ADVENTURE_PARTY_RACE_KIND_DEFAULT
                    ? ADVENTURE_PARTY_PROGRESS_ANY_HUMAN_FIRST
                    : ADVENTURE_PARTY_PROGRESS_TEAM_COINS_ANY_HUMAN_FIRST;
            return cap;

        case ADVENTURE_PARTY_RACE_KIND_TAJ_CHALLENGE:
        case ADVENTURE_PARTY_RACE_KIND_BATTLE_CHALLENGE:
        case ADVENTURE_PARTY_RACE_KIND_EGG_CHALLENGE:
        case ADVENTURE_PARTY_RACE_KIND_BANANA_CHALLENGE:
        case ADVENTURE_PARTY_RACE_KIND_BOSS:
            /* Safe v1: the host plays these alone on one viewport, and the
             * retail field is whatever retail spawns (total 0 = unchanged).
             * Progression is the existing one, exactly once. */
            cap.presentation = ADVENTURE_PARTY_PRESENT_HOST_SOLO;
            cap.human_count = 1;
            cap.viewport_count = 1;
            cap.total_racer_count = 0;
            cap.progress = ADVENTURE_PARTY_PROGRESS_HOST_EXISTING_ONCE;
            return cap;

        case ADVENTURE_PARTY_RACE_KIND_TROPHY:
            /* Supported and split, but the field size is deliberately NOT
             * asserted here: AP-04 measures whether the retail trophy field
             * survives four viewports and AP-16 owns the decision. Total
             * stays 0 and the flag says why. */
            cap.presentation = ADVENTURE_PARTY_PRESENT_SPLIT;
            cap.human_count = (uint8_t)participant_count;
            cap.viewport_count = (uint8_t)participant_count;
            cap.total_racer_count = 0;
            cap.field_size_undecided = 1;
            cap.progress = ADVENTURE_PARTY_PROGRESS_SHARED_SERIES_RESULT;
            return cap;

        default:
            break; /* UNKNOWN, NONE, out-of-range: fall through, closed. */
        }
    }

    return fail_closed_row();
}

int adventure_party_activity_human_count(
    const AdventurePartyCapability *capability) {
    return capability ? capability->human_count : 0;
}

int adventure_party_viewport_count(
    const AdventurePartyCapability *capability) {
    return capability ? capability->viewport_count : 0;
}

/* Deterministic ordering: does `a` beat `b` for the latch? Earliest
 * simulation tick first, lowest seat on a tie. Strict — an exact duplicate
 * does not "beat" the winner, it is already represented by it. */
static int request_wins(const AdventurePartyTransitionRequest *a,
                        const AdventurePartyTransitionRequest *b) {
    if (a->simulation_tick != b->simulation_tick)
        return a->simulation_tick < b->simulation_tick;
    return a->initiating_seat < b->initiating_seat;
}

AdventurePartyArbitration adventure_party_arbitrate_transition(
    AdventurePartyTransitionLatch *latch,
    const AdventurePartyTransitionRequest *request,
    uint32_t current_level_generation,
    uint8_t active_seat_mask) {
    if (!latch || !request)
        return ADVENTURE_PARTY_ARBITRATE_REJECTED_ARGUMENT;
    if (request->level_generation != current_level_generation)
        return ADVENTURE_PARTY_ARBITRATE_REJECTED_STALE;
    if (request->initiating_seat >= ADVENTURE_PARTY_MAX_SEATS ||
        !(active_seat_mask & (uint8_t)(1u << request->initiating_seat)))
        return ADVENTURE_PARTY_ARBITRATE_REJECTED_SEAT;

    /* First valid request takes the latch; after that a request wins only
     * by beating the standing winner under (tick, seat) ordering. The same
     * set of requests therefore converges on the same winner in ANY arrival
     * order — object iteration order is not an authority rule. */
    if (latch->latched && !request_wins(request, &latch->winner))
        return ADVENTURE_PARTY_ARBITRATE_REJECTED_LATCHED;

    latch->latched = 1;
    latch->winner = *request;
    return ADVENTURE_PARTY_ARBITRATE_LATCHED;
}

int adventure_party_completion_token_issue(
    AdventurePartyActivityOutcome outcome,
    uint32_t session_generation,
    uint32_t level_generation,
    uint16_t course,
    AdventurePartyRaceKind activity,
    AdventurePartyCompletionKind completion_kind,
    AdventurePartyCompletionToken *out_token) {
    if (!out_token)
        return 0;
    /* Only a team win mints a token — quit, retry and loss paths are handed
     * nothing, so their commit calls have nothing to consume. The refusal
     * leaves *out_token untouched so a caller cannot commit a half-built
     * key by ignoring the return value. */
    if (outcome != ADVENTURE_PARTY_OUTCOME_TEAM_WIN)
        return 0;
    if ((int)activity < 0 || activity >= ADVENTURE_PARTY_RACE_KIND_COUNT)
        return 0;
    if ((int)completion_kind < 0 ||
        completion_kind > ADVENTURE_PARTY_COMPLETION_CUTSCENE)
        return 0;

    memset(out_token, 0, sizeof *out_token);
    out_token->session_generation = session_generation;
    out_token->level_generation = level_generation;
    out_token->course = course;
    out_token->activity = (uint8_t)activity;
    out_token->completion_kind = (uint8_t)completion_kind;
    return 1;
}

int adventure_party_seat_may_act(int seat, uint8_t active_seat_mask,
                                 AdventurePartyActionKind action) {
    /* An unoccupied or out-of-range seat may do nothing at all. */
    if (seat < 0 || seat >= ADVENTURE_PARTY_MAX_SEATS)
        return 0;
    if (!(active_seat_mask & (uint8_t)(1u << seat)))
        return 0;

    switch (action) {
    case ADVENTURE_PARTY_ACTION_DIALOGUE_CHOICE:
    case ADVENTURE_PARTY_ACTION_PAUSE_DECISION:
        /* A wrong answer here mutates shared fate: host only. */
        return seat == ADVENTURE_PARTY_HOST_SEAT;
    case ADVENTURE_PARTY_ACTION_TRIGGER_TRANSITION:
    case ADVENTURE_PARTY_ACTION_COLLECT:
        /* Any participant — arbitration and shared counts make these safe
         * for everyone, and doing them is the point of being in a party. */
        return 1;
    default:
        return 0;
    }
}
