/* adventure_party_state.c — see adventure_party_state.h.
 *
 * Implementation notes, in the order a reader will wonder about them:
 *
 *   - The reducer validates EVERYTHING before touching the session. There is
 *     no rollback path because there is nothing to roll back: the first
 *     write happens only after the last check has passed, which is how
 *     "illegal events do not mutate" is a structural property here rather
 *     than a discipline.
 *   - Check order is fixed and observable: arguments, then state legality
 *     (so an illegal state/event pair always reports ILLEGAL_EVENT, however
 *     malformed its payload also is), then payload guards.
 *   - enter_level() is the single place a level generation increments, so
 *     "the bump clears the latch and the consumed tokens" cannot be
 *     forgotten by one transition and remembered by another.
 */
#include "adventure_party/adventure_party_state.h"

#include <string.h>

void adventure_party_session_init(AdventurePartySession *session) {
    if (session)
        memset(session, 0, sizeof *session);
}

/* Validates a roster request into *out. Purely local: no session writes, so
 * a refusal here costs nothing. Seats must be exactly {0 .. count-1}; the
 * host seat is therefore always present by construction. */
static AdventurePartyResult validate_roster(
    const AdventurePartyRosterRequest *request, AdventurePartyRoster *out) {
    uint8_t mask = 0;

    if (request->participant_count < ADVENTURE_PARTY_MIN_PARTICIPANTS ||
        request->participant_count > ADVENTURE_PARTY_MAX_PARTICIPANTS)
        return ADVENTURE_PARTY_ERR_PARTICIPANT_COUNT;

    memset(out, 0, sizeof *out);
    out->participant_count = request->participant_count;
    for (int i = 0; i < request->participant_count; i++) {
        const uint8_t seat = request->seat[i];
        if (seat >= ADVENTURE_PARTY_MAX_SEATS)
            return ADVENTURE_PARTY_ERR_SEAT_INVALID;
        if (mask & (uint8_t)(1u << seat))
            return ADVENTURE_PARTY_ERR_SEAT_DUPLICATE;
        mask |= (uint8_t)(1u << seat);
        out->character_by_seat[seat] = request->character[i];
    }
    /* Dense from seat 0: a party of N occupies seats 0..N-1, nothing else.
     * (The adapter maps physical controller ports onto dense seats; a gap
     * here means it failed to, and guessing which seat is "really" which
     * is exactly the ambiguity this module exists to refuse.) */
    if (mask != (uint8_t)((1u << request->participant_count) - 1u))
        return ADVENTURE_PARTY_ERR_SEAT_SPARSE;

    out->seat_mask = mask;
    return ADVENTURE_PARTY_OK;
}

/* The one place a level generation advances. Every entry invalidates the
 * previous level's latched shared action and its consumed-token list — they
 * are keyed to the generation, so keeping them would only preserve stale
 * refusals (or worse, stale permissions). */
static void enter_level(AdventurePartySession *session,
                        AdventurePartySessionState state) {
    session->state = state;
    session->level_generation++;
    memset(&session->transition_latch, 0, sizeof session->transition_latch);
    session->consumed_count = 0;
    memset(session->consumed, 0, sizeof session->consumed);
}

/* The plan's diagram as one predicate, mirrored by the exhaustive pair test.
 * Kept beside the reducer so a new edge has exactly two places to appear:
 * here, and in the test's copy of the table. */
static int event_is_legal(AdventurePartySessionState state,
                          AdventurePartyEventKind kind) {
    switch (state) {
    case ADVENTURE_PARTY_STATE_OFF:
        return kind == ADVENTURE_PARTY_EVENT_FORM;
    case ADVENTURE_PARTY_STATE_FORMING:
        return kind == ADVENTURE_PARTY_EVENT_START_NEW_GAME ||
               kind == ADVENTURE_PARTY_EVENT_RESUME_SAVE;
    case ADVENTURE_PARTY_STATE_SHARED_SCENE:
        return kind == ADVENTURE_PARTY_EVENT_SCENE_COMPLETE;
    case ADVENTURE_PARTY_STATE_ACTIVE_LOBBY:
        return kind == ADVENTURE_PARTY_EVENT_DIALOGUE_START ||
               kind == ADVENTURE_PARTY_EVENT_RACE_START ||
               kind == ADVENTURE_PARTY_EVENT_SOLO_START ||
               kind == ADVENTURE_PARTY_EVENT_QUIT;
    case ADVENTURE_PARTY_STATE_SHARED_DIALOGUE:
        return kind == ADVENTURE_PARTY_EVENT_DIALOGUE_COMPLETE;
    case ADVENTURE_PARTY_STATE_ACTIVE_RACE:
        return kind == ADVENTURE_PARTY_EVENT_RACE_RESULT_COMMITTED ||
               kind == ADVENTURE_PARTY_EVENT_QUIT;
    case ADVENTURE_PARTY_STATE_SOLO_ACTIVITY:
        return kind == ADVENTURE_PARTY_EVENT_SOLO_EXIT;
    case ADVENTURE_PARTY_STATE_RESTORING_PARTY:
        return kind == ADVENTURE_PARTY_EVENT_RESTORE_COMMIT;
    case ADVENTURE_PARTY_STATE_EXITING:
        return kind == ADVENTURE_PARTY_EVENT_DESTROY;
    default:
        return 0;
    }
}

AdventurePartyResult adventure_party_session_apply(
    AdventurePartySession *session, const AdventurePartyEvent *event) {
    AdventurePartyRoster roster;
    AdventurePartyResult r;

    if (!session || !event)
        return ADVENTURE_PARTY_ERR_ARGUMENT;
    if ((int)event->kind < 0 ||
        event->kind >= ADVENTURE_PARTY_EVENT_KIND_COUNT)
        return ADVENTURE_PARTY_ERR_ARGUMENT;
    if (!event_is_legal(session->state, event->kind))
        return ADVENTURE_PARTY_ERR_ILLEGAL_EVENT;

    switch (event->kind) {
    case ADVENTURE_PARTY_EVENT_FORM:
        /* The activation facts the adapter observed, then the roster. The
         * order is part of the contract: a disabled feature is reported as
         * disabled even if the roster is also nonsense. */
        if (!event->enabled)
            return ADVENTURE_PARTY_ERR_NOT_ENABLED;
        if (!event->adventure_selected)
            return ADVENTURE_PARTY_ERR_NOT_ADVENTURE;
        r = validate_roster(&event->roster, &roster);
        if (r != ADVENTURE_PARTY_OK)
            return r;
        session->state = ADVENTURE_PARTY_STATE_FORMING;
        session->roster = roster;
        session->host_seat = ADVENTURE_PARTY_HOST_SEAT;
        session->session_generation++;
        return ADVENTURE_PARTY_OK;

    case ADVENTURE_PARTY_EVENT_START_NEW_GAME:
        enter_level(session, ADVENTURE_PARTY_STATE_SHARED_SCENE);
        return ADVENTURE_PARTY_OK;

    case ADVENTURE_PARTY_EVENT_RESUME_SAVE:
    case ADVENTURE_PARTY_EVENT_SCENE_COMPLETE:
        enter_level(session, ADVENTURE_PARTY_STATE_ACTIVE_LOBBY);
        return ADVENTURE_PARTY_OK;

    case ADVENTURE_PARTY_EVENT_DIALOGUE_START:
        /* Same lobby, same generation: the dialogue IS this generation's
         * latched shared action, so the latch survives it starting. */
        session->state = ADVENTURE_PARTY_STATE_SHARED_DIALOGUE;
        return ADVENTURE_PARTY_OK;

    case ADVENTURE_PARTY_EVENT_DIALOGUE_COMPLETE:
        /* The shared action finished and control returned; releasing the
         * latch is what lets the party use a door afterwards. A level
         * transition, by contrast, is only ever released by the arrival
         * generation bump in enter_level(). */
        session->state = ADVENTURE_PARTY_STATE_ACTIVE_LOBBY;
        memset(&session->transition_latch, 0,
               sizeof session->transition_latch);
        return ADVENTURE_PARTY_OK;

    case ADVENTURE_PARTY_EVENT_RACE_START:
        enter_level(session, ADVENTURE_PARTY_STATE_ACTIVE_RACE);
        return ADVENTURE_PARTY_OK;

    case ADVENTURE_PARTY_EVENT_RACE_RESULT_COMMITTED:
        /* The winner is a fact about the finished race, never an instruction
         * to this session: seats and characters do not move. Validating it
         * still matters — a "winner" this roster cannot contain means the
         * adapter miscounted something upstream. */
        if (event->winner_seat != ADVENTURE_PARTY_NO_SEAT) {
            if (event->winner_seat >= ADVENTURE_PARTY_MAX_SEATS ||
                !(session->roster.seat_mask &
                  (uint8_t)(1u << event->winner_seat)))
                return ADVENTURE_PARTY_ERR_WINNER_SEAT;
        }
        enter_level(session, ADVENTURE_PARTY_STATE_ACTIVE_LOBBY);
        return ADVENTURE_PARTY_OK;

    case ADVENTURE_PARTY_EVENT_SOLO_START:
        /* Suspend the roster facts the restore transaction must reproduce.
         * The live roster stays — the host is still playing. */
        session->suspended_roster = session->roster;
        session->has_suspended_roster = 1;
        enter_level(session, ADVENTURE_PARTY_STATE_SOLO_ACTIVITY);
        return ADVENTURE_PARTY_OK;

    case ADVENTURE_PARTY_EVENT_SOLO_EXIT:
        /* Not a level entry: the party is between levels until the restore
         * transaction commits. */
        session->state = ADVENTURE_PARTY_STATE_RESTORING_PARTY;
        return ADVENTURE_PARTY_OK;

    case ADVENTURE_PARTY_EVENT_RESTORE_COMMIT:
        /* A solo activity returns EXACTLY the party it borrowed: same count,
         * same seats, same characters. Shape errors first (they are typed),
         * then the field-for-field comparison against the suspended facts. */
        r = validate_roster(&event->roster, &roster);
        if (r != ADVENTURE_PARTY_OK)
            return r;
        if (memcmp(&roster, &session->suspended_roster, sizeof roster) != 0)
            return ADVENTURE_PARTY_ERR_ROSTER_MISMATCH;
        session->has_suspended_roster = 0;
        memset(&session->suspended_roster, 0,
               sizeof session->suspended_roster);
        enter_level(session, ADVENTURE_PARTY_STATE_ACTIVE_LOBBY);
        return ADVENTURE_PARTY_OK;

    case ADVENTURE_PARTY_EVENT_QUIT:
        session->state = ADVENTURE_PARTY_STATE_EXITING;
        memset(&session->transition_latch, 0,
               sizeof session->transition_latch);
        return ADVENTURE_PARTY_OK;

    case ADVENTURE_PARTY_EVENT_DESTROY:
        /* Back to OFF with the roster gone; both generation counters
         * survive so nothing minted by this session can ever match a
         * later one. */
        session->state = ADVENTURE_PARTY_STATE_OFF;
        memset(&session->roster, 0, sizeof session->roster);
        memset(&session->suspended_roster, 0,
               sizeof session->suspended_roster);
        session->has_suspended_roster = 0;
        session->host_seat = ADVENTURE_PARTY_HOST_SEAT;
        memset(&session->transition_latch, 0,
               sizeof session->transition_latch);
        session->consumed_count = 0;
        memset(session->consumed, 0, sizeof session->consumed);
        return ADVENTURE_PARTY_OK;

    default:
        /* Unreachable: the range check above already refused it. */
        return ADVENTURE_PARTY_ERR_ARGUMENT;
    }
}

int adventure_party_is_active(const AdventurePartySession *session) {
    return session != NULL && session->state != ADVENTURE_PARTY_STATE_OFF;
}

int adventure_party_participant_count(const AdventurePartySession *session) {
    if (!adventure_party_is_active(session))
        return 0;
    return session->roster.participant_count;
}

int adventure_party_host_seat(const AdventurePartySession *session) {
    (void)session; /* read for symmetry; v1 has exactly one answer */
    return ADVENTURE_PARTY_HOST_SEAT;
}

int adventure_party_character_for_seat(const AdventurePartySession *session,
                                       int seat) {
    if (!adventure_party_is_active(session))
        return -1;
    if (seat < 0 || seat >= ADVENTURE_PARTY_MAX_SEATS)
        return -1;
    if (!(session->roster.seat_mask & (uint8_t)(1u << seat)))
        return -1;
    return session->roster.character_by_seat[seat];
}

/* An activity a completion can legitimately be committed from. FORMING has
 * no level yet, EXITING is teardown, OFF has no session; a token surfacing
 * then is a replay or a bug, and either way it does not award. */
static int state_can_commit(AdventurePartySessionState state) {
    return state == ADVENTURE_PARTY_STATE_SHARED_SCENE ||
           state == ADVENTURE_PARTY_STATE_ACTIVE_LOBBY ||
           state == ADVENTURE_PARTY_STATE_SHARED_DIALOGUE ||
           state == ADVENTURE_PARTY_STATE_ACTIVE_RACE ||
           state == ADVENTURE_PARTY_STATE_SOLO_ACTIVITY;
}

AdventurePartyResult adventure_party_consume_completion_token(
    AdventurePartySession *session,
    const AdventurePartyCompletionToken *token) {
    if (!session || !token)
        return ADVENTURE_PARTY_ERR_ARGUMENT;
    if (!state_can_commit(session->state))
        return ADVENTURE_PARTY_ERR_TOKEN_STATE;
    /* Generation staleness before the duplicate scan: a late commit from a
     * level the party already left is refused as STALE even if its key was
     * never consumed — the consumed list only spans the CURRENT generation,
     * so answering "already consumed" about an old one would be a guess. */
    if (token->session_generation != session->session_generation ||
        token->level_generation != session->level_generation)
        return ADVENTURE_PARTY_ERR_STALE_GENERATION;
    for (int i = 0; i < session->consumed_count; i++) {
        if (memcmp(&session->consumed[i], token, sizeof *token) == 0)
            return ADVENTURE_PARTY_ERR_TOKEN_CONSUMED;
    }
    if (session->consumed_count >= ADVENTURE_PARTY_MAX_CONSUMED_TOKENS)
        return ADVENTURE_PARTY_ERR_TOKEN_CAPACITY;
    session->consumed[session->consumed_count++] = *token;
    return ADVENTURE_PARTY_OK;
}

const char *adventure_party_state_name(AdventurePartySessionState state) {
    switch (state) {
    case ADVENTURE_PARTY_STATE_OFF:             return "OFF";
    case ADVENTURE_PARTY_STATE_FORMING:         return "FORMING";
    case ADVENTURE_PARTY_STATE_SHARED_SCENE:    return "SHARED_SCENE";
    case ADVENTURE_PARTY_STATE_ACTIVE_LOBBY:    return "ACTIVE_LOBBY";
    case ADVENTURE_PARTY_STATE_SHARED_DIALOGUE: return "SHARED_DIALOGUE";
    case ADVENTURE_PARTY_STATE_ACTIVE_RACE:     return "ACTIVE_RACE";
    case ADVENTURE_PARTY_STATE_SOLO_ACTIVITY:   return "SOLO_ACTIVITY";
    case ADVENTURE_PARTY_STATE_RESTORING_PARTY: return "RESTORING_PARTY";
    case ADVENTURE_PARTY_STATE_EXITING:         return "EXITING";
    default:                                    return "INVALID";
    }
}

const char *adventure_party_result_name(AdventurePartyResult result) {
    switch (result) {
    case ADVENTURE_PARTY_OK:                     return "OK";
    case ADVENTURE_PARTY_ERR_ARGUMENT:           return "ERR_ARGUMENT";
    case ADVENTURE_PARTY_ERR_ILLEGAL_EVENT:      return "ERR_ILLEGAL_EVENT";
    case ADVENTURE_PARTY_ERR_NOT_ENABLED:        return "ERR_NOT_ENABLED";
    case ADVENTURE_PARTY_ERR_NOT_ADVENTURE:      return "ERR_NOT_ADVENTURE";
    case ADVENTURE_PARTY_ERR_PARTICIPANT_COUNT:  return "ERR_PARTICIPANT_COUNT";
    case ADVENTURE_PARTY_ERR_SEAT_INVALID:       return "ERR_SEAT_INVALID";
    case ADVENTURE_PARTY_ERR_SEAT_DUPLICATE:     return "ERR_SEAT_DUPLICATE";
    case ADVENTURE_PARTY_ERR_SEAT_SPARSE:        return "ERR_SEAT_SPARSE";
    case ADVENTURE_PARTY_ERR_ROSTER_MISMATCH:    return "ERR_ROSTER_MISMATCH";
    case ADVENTURE_PARTY_ERR_WINNER_SEAT:        return "ERR_WINNER_SEAT";
    case ADVENTURE_PARTY_ERR_STALE_GENERATION:   return "ERR_STALE_GENERATION";
    case ADVENTURE_PARTY_ERR_TOKEN_CONSUMED:     return "ERR_TOKEN_CONSUMED";
    case ADVENTURE_PARTY_ERR_TOKEN_STATE:        return "ERR_TOKEN_STATE";
    case ADVENTURE_PARTY_ERR_TOKEN_CAPACITY:     return "ERR_TOKEN_CAPACITY";
    default:                                     return "INVALID";
    }
}
