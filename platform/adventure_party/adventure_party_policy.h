/* adventure_party_policy.h — Adventure Party pure decision core (AP-03).
 *
 * Every product decision that is a function of values: what a given activity
 * looks like for a party (the v1 capability table), which counts mean what,
 * which seat wins a simultaneous door press, when a progression token may be
 * issued at all, and which seat is allowed to take which shared action.
 * No state lives here — the session (adventure_party_state.h) owns storage;
 * this module only decides. Same purity rules: value structs in, value
 * structs out, ROM-free, deterministic.
 *
 * The load-bearing idea is the capability table. A race_type is NOT a
 * complete product rule: the capability row is resolved at level request time
 * from an explicit descriptor, and anything the table does not positively
 * recognise FAILS CLOSED to a one-viewport host-only presentation with a
 * diagnostic flag. Unknown activities never guess that four-player is safe.
 *
 * Count vocabulary, restated because it is the root cause this design exists
 * to kill: participant count (stable roster, state module), activity human
 * count and viewport count (this module, per capability row) are three
 * different facts. A host-solo boss for a four-player party is the canonical
 * proof: participants=4, humans=1, viewports=1. Never substitute one for
 * another.
 */
#ifndef MDKR64_ADVENTURE_PARTY_POLICY_H
#define MDKR64_ADVENTURE_PARTY_POLICY_H

#include <stdint.h>

#include "adventure_party/adventure_party_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Default and silver-coin Adventure races always field six racers: the
 * party's humans plus (six minus humans) CPUs, exactly the retail Adventure
 * field size. */
#define ADVENTURE_PARTY_RACE_FIELD_TOTAL 6

/* What kind of race/challenge the requested level runs. UNKNOWN is the zero
 * value on purpose: a descriptor someone forgot to classify fails closed. */
typedef enum AdventurePartyRaceKind {
    ADVENTURE_PARTY_RACE_KIND_UNKNOWN = 0,
    ADVENTURE_PARTY_RACE_KIND_NONE,             /* no race: lobby or cutscene */
    ADVENTURE_PARTY_RACE_KIND_DEFAULT,          /* default balloon race */
    ADVENTURE_PARTY_RACE_KIND_SILVER_COIN,
    ADVENTURE_PARTY_RACE_KIND_TAJ_CHALLENGE,
    ADVENTURE_PARTY_RACE_KIND_BATTLE_CHALLENGE,
    ADVENTURE_PARTY_RACE_KIND_EGG_CHALLENGE,
    ADVENTURE_PARTY_RACE_KIND_BANANA_CHALLENGE,
    ADVENTURE_PARTY_RACE_KIND_TROPHY,           /* series entered from Adventure */
    ADVENTURE_PARTY_RACE_KIND_BOSS,
    ADVENTURE_PARTY_RACE_KIND_COUNT
} AdventurePartyRaceKind;

/* What class of course hosts it. NON_ADVENTURE (Tracks menu, time trial,
 * demo) is where party policy is simply not active — retail behavior. */
typedef enum AdventurePartyCourseClass {
    ADVENTURE_PARTY_COURSE_UNKNOWN = 0,
    ADVENTURE_PARTY_COURSE_LOBBY,          /* central hub / world lobby */
    ADVENTURE_PARTY_COURSE_TRACK,          /* course entered from Adventure */
    ADVENTURE_PARTY_COURSE_CUTSCENE,       /* new-game / world cutscene */
    ADVENTURE_PARTY_COURSE_NON_ADVENTURE,  /* tracks / time trial / demo */
    ADVENTURE_PARTY_COURSE_CLASS_COUNT
} AdventurePartyCourseClass;

/* The level-request facts, as values. The adapter builds one of these at the
 * level-request seam; nothing here reads game structs to find out. */
typedef struct AdventurePartyActivityDescriptor {
    AdventurePartyRaceKind race_kind;
    AdventurePartyCourseClass course_class;
} AdventurePartyActivityDescriptor;

typedef enum AdventurePartyPresentation {
    ADVENTURE_PARTY_PRESENT_RETAIL = 0,     /* policy not active: stock screen */
    ADVENTURE_PARTY_PRESENT_SPLIT,          /* 2/3/4-way split, all humans */
    ADVENTURE_PARTY_PRESENT_HOST_SOLO,      /* one viewport, host plays */
    ADVENTURE_PARTY_PRESENT_ONE_VIEWPORT_NO_INPUT /* cutscene: nobody drives */
} AdventurePartyPresentation;

/* Which existing retail progression a completed activity is allowed to feed,
 * exactly once. NONE covers both "no progression here" and "fail-closed
 * unknown" — an unproven activity must not guess at a commit either. */
typedef enum AdventurePartyProgressPolicy {
    ADVENTURE_PARTY_PROGRESS_NONE = 0,
    ADVENTURE_PARTY_PROGRESS_SHARED_LOBBY,            /* shared lobby state */
    ADVENTURE_PARTY_PROGRESS_ANY_HUMAN_FIRST,         /* default race */
    ADVENTURE_PARTY_PROGRESS_TEAM_COINS_ANY_HUMAN_FIRST, /* silver coin */
    ADVENTURE_PARTY_PROGRESS_HOST_EXISTING_ONCE,      /* challenges, boss */
    ADVENTURE_PARTY_PROGRESS_SHARED_SERIES_RESULT,    /* trophy series */
    ADVENTURE_PARTY_PROGRESS_CUTSCENE_FLAGS_ONCE
} AdventurePartyProgressPolicy;

/* One resolved capability row. total_racer_count is 0 where the retail field
 * is unchanged or the size is not this table's to decide; the six-racer rows
 * say six explicitly. */
typedef struct AdventurePartyCapability {
    uint8_t policy_active;        /* 0: tracks/time-trial/demo — stock game */
    AdventurePartyPresentation presentation;
    uint8_t human_count;          /* live human racers in THIS activity */
    uint8_t viewport_count;       /* cameras rendered for THIS activity */
    uint8_t total_racer_count;    /* humans + CPUs; 0 = retail/undecided */
    uint8_t field_size_undecided; /* trophy only: AP-16 owns the field size */
    uint8_t fail_closed;          /* diagnostic: unclassified, forced host-only */
    AdventurePartyProgressPolicy progress;
} AdventurePartyCapability;

/*
 * The v1 capability table, resolved for one descriptor and one party size.
 *
 *   lobbies                 split, all humans, shared lobby state
 *   default / silver races  split, all humans, SIX total racers
 *   Taj/battle/egg/banana   host-solo, one viewport, existing progression once
 *   trophy series           split, all humans; field size DEFERRED
 *                           (field_size_undecided=1 — AP-16 decides, not this)
 *   boss                    host-solo, one viewport
 *   cutscenes               one viewport, no live party input
 *   tracks/time-trial/demo  policy_active=0 — party policy not active
 *
 * Anything else — UNKNOWN kinds, out-of-range values, incoherent pairs like a
 * boss in a lobby, a participant count outside 2..4, a NULL descriptor —
 * returns the fail-closed row: host-solo, one viewport, one human, progress
 * NONE, fail_closed=1 so development builds can surface the misclassification
 * loudly while release builds stay merely safe.
 */
AdventurePartyCapability adventure_party_classify_activity(
    const AdventurePartyActivityDescriptor *descriptor,
    int participant_count);

/* Live human racers for the classified activity. NOT the session participant
 * count (adventure_party_participant_count) and NOT the viewport count.
 * Returns 0 for NULL. */
int adventure_party_activity_human_count(
    const AdventurePartyCapability *capability);

/* Cameras rendered for the classified activity. A cutscene has one viewport
 * and zero live humans, which is why this is not the human count either.
 * Returns 0 for NULL. */
int adventure_party_viewport_count(const AdventurePartyCapability *capability);

/* Arbitration verdicts. LATCHED means "this request is now the winner" —
 * either it was first, or it beats the current winner under the deterministic
 * rule below. Everything else is a typed rejection that left the latch
 * untouched. */
typedef enum AdventurePartyArbitration {
    ADVENTURE_PARTY_ARBITRATE_LATCHED = 0,
    ADVENTURE_PARTY_ARBITRATE_REJECTED_LATCHED = -1,  /* someone already won */
    ADVENTURE_PARTY_ARBITRATE_REJECTED_STALE = -2,    /* wrong level generation */
    ADVENTURE_PARTY_ARBITRATE_REJECTED_SEAT = -3,     /* seat invalid/not active */
    ADVENTURE_PARTY_ARBITRATE_REJECTED_ARGUMENT = -4  /* NULL input */
} AdventurePartyArbitration;

/*
 * The transition arbitration reducer: at most ONE shared action wins per
 * level generation, and which one wins does not depend on the order requests
 * are fed in. The winner is the request with the LOWEST simulation tick,
 * ties resolved to the LOWEST initiating seat — object iteration order is
 * not an authority rule, so a request that beats the current winner under
 * that ordering replaces it (LATCHED) and the displaced one is simply no
 * longer the winner. Requests that do not beat the winner get
 * REJECTED_LATCHED; requests for any other level generation get
 * REJECTED_STALE; a seat outside active_seat_mask gets REJECTED_SEAT.
 *
 * The latch itself lives in the session (the state module clears it on every
 * generation bump and on dialogue completion); this function only decides.
 */
AdventurePartyArbitration adventure_party_arbitrate_transition(
    AdventurePartyTransitionLatch *latch,
    const AdventurePartyTransitionRequest *request,
    uint32_t current_level_generation,
    uint8_t active_seat_mask);

/* How an activity ended. Only TEAM_WIN can mint a completion token. */
typedef enum AdventurePartyActivityOutcome {
    ADVENTURE_PARTY_OUTCOME_TEAM_WIN = 0,
    ADVENTURE_PARTY_OUTCOME_LOSS,
    ADVENTURE_PARTY_OUTCOME_QUIT,
    ADVENTURE_PARTY_OUTCOME_RETRY
} AdventurePartyActivityOutcome;

/* Which retail commit the token authorises — the last key component. */
typedef enum AdventurePartyCompletionKind {
    ADVENTURE_PARTY_COMPLETION_COURSE = 0,  /* balloon / course flag */
    ADVENTURE_PARTY_COMPLETION_CHALLENGE,   /* Taj/battle/egg/banana progress */
    ADVENTURE_PARTY_COMPLETION_BOSS,
    ADVENTURE_PARTY_COMPLETION_TROPHY,
    ADVENTURE_PARTY_COMPLETION_CUTSCENE     /* cutscene flags */
} AdventurePartyCompletionKind;

/*
 * Issues (or refuses to issue) the exact-once token for one completed
 * activity. Returns 1 and fills *out_token only for TEAM_WIN with sane
 * arguments; returns 0 — with *out_token untouched — for LOSS, QUIT and
 * RETRY, and for malformed input. Quit/retry/loss paths therefore never hold
 * a token, so they have nothing to consume and cannot award: the guarantee
 * is structural, not a flag check at commit time.
 */
int adventure_party_completion_token_issue(
    AdventurePartyActivityOutcome outcome,
    uint32_t session_generation,
    uint32_t level_generation,
    uint16_t course,
    AdventurePartyRaceKind activity,
    AdventurePartyCompletionKind completion_kind,
    AdventurePartyCompletionToken *out_token);

/* Shared actions a seat may attempt. Host-only where a wrong answer mutates
 * shared fate (dialogue choices, pause menu decisions); any occupied seat
 * where the act is the point of being in a party (doors, collecting). */
typedef enum AdventurePartyActionKind {
    ADVENTURE_PARTY_ACTION_DIALOGUE_CHOICE = 0, /* host only */
    ADVENTURE_PARTY_ACTION_PAUSE_DECISION,      /* host only */
    ADVENTURE_PARTY_ACTION_TRIGGER_TRANSITION,  /* any occupied seat */
    ADVENTURE_PARTY_ACTION_COLLECT              /* any occupied seat */
} AdventurePartyActionKind;

/* 1 if `seat` (which must be occupied in active_seat_mask) may take this
 * action; 0 otherwise. An unoccupied or out-of-range seat may do nothing. */
int adventure_party_seat_may_act(int seat, uint8_t active_seat_mask,
                                 AdventurePartyActionKind action);

#ifdef __cplusplus
}
#endif

#endif /* MDKR64_ADVENTURE_PARTY_POLICY_H */
