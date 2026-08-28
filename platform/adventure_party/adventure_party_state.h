/* adventure_party_state.h — Adventure Party session state machine (AP-02).
 *
 * The one authoritative record of an Adventure Party session: who is in it,
 * what state it is in, and which generation of level the party currently
 * occupies. Everything here is a value struct plus pure functions over it —
 * no Object/Settings pointers, no renderer, no saves, no controller globals,
 * no ROM.
 * The game talks to this module only through the narrow adapter seams listed
 * in docs/architecture/adventure-party.md; that is what keeps every edge case
 * below testable in milliseconds without booting anything.
 *
 * Deliberate boundaries (enforced by tests/check_adventure_party_boundaries.py):
 *
 *   - This is NOT the retail two-player-adventure protocol. Nothing here reads
 *     or writes gIsInTwoPlayerAdventure/gTwoPlayerAdvRace, and no caller may
 *     infer any of this session's facts from gNumberOfActivePlayers.
 *   - "player count" is not one fact. adventure_party_participant_count() is
 *     the stable 2-4 session roster; live human racers and viewports for one
 *     activity come from adventure_party_policy.h and are NOT substitutes.
 *
 * The reducer contract: adventure_party_session_apply() either performs one
 * legal transition and returns ADVENTURE_PARTY_OK, or returns a typed error
 * and leaves the session bit-for-bit untouched. There is no partially applied
 * event, so a caller that ignores a result cannot half-advance a session.
 *
 * Generations, because "which level are we in" is otherwise a race:
 *
 *   - session_generation increments once per formed session and never resets,
 *     so a token or trace from a previous party can never be mistaken for
 *     this one's.
 *   - level_generation increments on every level entry (scene, lobby, race,
 *     solo activity, and each return to the lobby) and never resets. The one
 *     latched shared action and the consumed completion tokens are keyed to
 *     it, which is what makes "a second door press" and "a second award"
 *     mechanical impossibilities rather than timing questions.
 */
#ifndef MDKR64_ADVENTURE_PARTY_STATE_H
#define MDKR64_ADVENTURE_PARTY_STATE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ADVENTURE_PARTY_MIN_PARTICIPANTS 2
#define ADVENTURE_PARTY_MAX_PARTICIPANTS 4
#define ADVENTURE_PARTY_MAX_SEATS        4

/* v1 product rule: the host is always seat 0 and never migrates. A query
 * exists (adventure_party_host_seat) so call sites read the rule, not the
 * literal, and so the "host never changes" invariant has one place to test. */
#define ADVENTURE_PARTY_HOST_SEAT 0

/* Sentinel seat: "no human" (e.g. a race the CPUs won). Never a valid seat. */
#define ADVENTURE_PARTY_NO_SEAT 0xFF

/* Distinct completion tokens consumable within ONE level generation. A level
 * awards a handful of progression commits at most (course flag, balloon, key,
 * trophy); the list resets on every generation bump, so this is a defensive
 * ceiling, not a campaign-length budget. */
#define ADVENTURE_PARTY_MAX_CONSUMED_TOKENS 8

/* Session states, exactly the plan's diagram. OFF is the zero value so a
 * zeroed struct is a valid, inert session. */
typedef enum AdventurePartySessionState {
    ADVENTURE_PARTY_STATE_OFF = 0,
    ADVENTURE_PARTY_STATE_FORMING,
    ADVENTURE_PARTY_STATE_SHARED_SCENE,
    ADVENTURE_PARTY_STATE_ACTIVE_LOBBY,
    ADVENTURE_PARTY_STATE_SHARED_DIALOGUE,
    ADVENTURE_PARTY_STATE_ACTIVE_RACE,
    ADVENTURE_PARTY_STATE_SOLO_ACTIVITY,
    ADVENTURE_PARTY_STATE_RESTORING_PARTY,
    ADVENTURE_PARTY_STATE_EXITING,
    ADVENTURE_PARTY_SESSION_STATE_COUNT
} AdventurePartySessionState;

/* Reducer events. Each is legal from exactly the states the plan's diagram
 * draws; everything else is ADVENTURE_PARTY_ERR_ILLEGAL_EVENT. */
typedef enum AdventurePartyEventKind {
    /* OFF -> FORMING. Carries the roster plus the two activation facts the
     * adapter observed (feature enabled, Adventure selected). */
    ADVENTURE_PARTY_EVENT_FORM = 0,
    ADVENTURE_PARTY_EVENT_START_NEW_GAME,        /* FORMING -> SHARED_SCENE */
    ADVENTURE_PARTY_EVENT_RESUME_SAVE,           /* FORMING -> ACTIVE_LOBBY */
    ADVENTURE_PARTY_EVENT_SCENE_COMPLETE,        /* SHARED_SCENE -> ACTIVE_LOBBY */
    ADVENTURE_PARTY_EVENT_DIALOGUE_START,        /* ACTIVE_LOBBY -> SHARED_DIALOGUE */
    ADVENTURE_PARTY_EVENT_DIALOGUE_COMPLETE,     /* SHARED_DIALOGUE -> ACTIVE_LOBBY */
    ADVENTURE_PARTY_EVENT_RACE_START,            /* ACTIVE_LOBBY -> ACTIVE_RACE */
    ADVENTURE_PARTY_EVENT_RACE_RESULT_COMMITTED, /* ACTIVE_RACE -> ACTIVE_LOBBY */
    ADVENTURE_PARTY_EVENT_SOLO_START,            /* ACTIVE_LOBBY -> SOLO_ACTIVITY */
    ADVENTURE_PARTY_EVENT_SOLO_EXIT,             /* SOLO_ACTIVITY -> RESTORING_PARTY */
    ADVENTURE_PARTY_EVENT_RESTORE_COMMIT,        /* RESTORING_PARTY -> ACTIVE_LOBBY */
    ADVENTURE_PARTY_EVENT_QUIT,                  /* ACTIVE_LOBBY|ACTIVE_RACE -> EXITING */
    ADVENTURE_PARTY_EVENT_DESTROY,               /* EXITING -> OFF */
    ADVENTURE_PARTY_EVENT_KIND_COUNT
} AdventurePartyEventKind;

/* Non-zero, negative, and specific: an illegal event names WHY it was
 * refused, and a refusal never mutates the session. */
typedef enum AdventurePartyResult {
    ADVENTURE_PARTY_OK = 0,
    ADVENTURE_PARTY_ERR_ARGUMENT = -1,          /* NULL / out-of-range input */
    ADVENTURE_PARTY_ERR_ILLEGAL_EVENT = -2,     /* event not legal in this state */
    ADVENTURE_PARTY_ERR_NOT_ENABLED = -3,       /* FORM without the feature on */
    ADVENTURE_PARTY_ERR_NOT_ADVENTURE = -4,     /* FORM outside Adventure select */
    ADVENTURE_PARTY_ERR_PARTICIPANT_COUNT = -5, /* count outside 2..4 */
    ADVENTURE_PARTY_ERR_SEAT_INVALID = -6,      /* seat index outside 0..3 */
    ADVENTURE_PARTY_ERR_SEAT_DUPLICATE = -7,    /* same seat listed twice */
    ADVENTURE_PARTY_ERR_SEAT_SPARSE = -8,       /* seats not dense from seat 0 */
    ADVENTURE_PARTY_ERR_ROSTER_MISMATCH = -9,   /* restore differs from suspended */
    ADVENTURE_PARTY_ERR_WINNER_SEAT = -10,      /* winner not an occupied seat */
    ADVENTURE_PARTY_ERR_STALE_GENERATION = -11, /* token from another generation */
    ADVENTURE_PARTY_ERR_TOKEN_CONSUMED = -12,   /* exact-once: second award */
    ADVENTURE_PARTY_ERR_TOKEN_STATE = -13,      /* consume outside an activity */
    ADVENTURE_PARTY_ERR_TOKEN_CAPACITY = -14    /* consumed-token list is full */
} AdventurePartyResult;

/* Roster as the adapter proposes it: a LIST of (seat, character) pairs, not a
 * mask, precisely so duplicate and sparse seat sets are representable and can
 * be refused with a named error instead of being silently unrepresentable.
 * Seats are session seats, dense from 0 (the adapter maps physical controller
 * ports to dense seats); characters are stable identity values this module
 * never interprets. Only the first participant_count entries are read. */
typedef struct AdventurePartyRosterRequest {
    uint8_t participant_count;                     /* must be 2..4 */
    uint8_t seat[ADVENTURE_PARTY_MAX_SEATS];       /* must be dense from 0 */
    uint8_t character[ADVENTURE_PARTY_MAX_SEATS];  /* parallel to seat[] */
} AdventurePartyRosterRequest;

/* Validated roster as the session stores it. character_by_seat[] is only
 * meaningful where the seat_mask bit is set. */
typedef struct AdventurePartyRoster {
    uint8_t participant_count;                          /* 2..4 */
    uint8_t seat_mask;                                  /* bit s = seat s occupied */
    uint8_t character_by_seat[ADVENTURE_PARTY_MAX_SEATS];
} AdventurePartyRoster;

/* One reducer event. Fields beyond `kind` are read only by the events named
 * in their comments; zero-initialise and fill what the event needs. */
typedef struct AdventurePartyEvent {
    AdventurePartyEventKind kind;
    uint8_t enabled;            /* FORM: the feature toggle the adapter read */
    uint8_t adventure_selected; /* FORM: Adventure (not Tracks etc.) chosen */
    uint8_t winner_seat;        /* RACE_RESULT_COMMITTED: first human home, or
                                 * ADVENTURE_PARTY_NO_SEAT when no human won.
                                 * Informational — it never remaps seats. */
    AdventurePartyRosterRequest roster; /* FORM, RESTORE_COMMIT */
} AdventurePartyEvent;

/* One authored trigger firing, as a value: everything the arbitration
 * reducer (adventure_party_policy.h) needs to pick exactly one winner without
 * consulting object iteration order. */
typedef struct AdventurePartyTransitionRequest {
    uint32_t level_generation; /* generation the initiator believes it is in */
    uint32_t simulation_tick;  /* tick the trigger fired on */
    uint16_t trigger_kind;     /* adapter vocabulary: door/exit/balloon/... */
    uint16_t destination;
    uint16_t entrance;
    uint16_t object_id;
    uint8_t  initiating_seat;
} AdventurePartyTransitionRequest;

/* The ONE latched shared action per level generation. Owned by the session
 * (cleared on every level-generation bump and on dialogue completion),
 * decided by adventure_party_arbitrate_transition() in the policy module. */
typedef struct AdventurePartyTransitionLatch {
    uint8_t latched;
    AdventurePartyTransitionRequest winner; /* meaningful only when latched */
} AdventurePartyTransitionLatch;

/* Exact-once progression token. The five fields ARE the key; this module
 * treats course/activity/completion_kind as opaque key components (the policy
 * module gives them meaning and decides when one may be issued at all). */
typedef struct AdventurePartyCompletionToken {
    uint32_t session_generation;
    uint32_t level_generation;
    uint16_t course;
    uint8_t  activity;        /* an AdventurePartyRaceKind value */
    uint8_t  completion_kind; /* an AdventurePartyCompletionKind value */
} AdventurePartyCompletionToken;

/* The whole session. A transparent value struct the caller owns — embed it,
 * copy it, snapshot it. All mutation goes through the functions below; the
 * fields are visible so tests and read-only instrumentation (AP-05) need no
 * accessor zoo. */
typedef struct AdventurePartySession {
    AdventurePartySessionState state;
    AdventurePartyRoster roster;        /* zeroed while OFF */
    uint8_t host_seat;                  /* ADVENTURE_PARTY_HOST_SEAT, always */
    uint8_t has_suspended_roster;       /* set across solo/shared activities */
    uint32_t session_generation;        /* ++ per FORM, never resets */
    uint32_t level_generation;          /* ++ per level entry, never resets */
    AdventurePartyTransitionLatch transition_latch;
    uint8_t consumed_count;
    AdventurePartyCompletionToken consumed[ADVENTURE_PARTY_MAX_CONSUMED_TOKENS];
    AdventurePartyRoster suspended_roster; /* facts RESTORE_COMMIT must match */
} AdventurePartySession;

/* Zeroes the session: OFF, no roster, generation counters at zero. */
void adventure_party_session_init(AdventurePartySession *session);

/*
 * The reducer. Applies one event, or refuses it.
 *
 * ADVENTURE_PARTY_OK means exactly one legal transition happened. Any other
 * result means the session was NOT modified — not its state, not its roster,
 * not a generation, nothing — so an illegal event observed mid-frame can
 * simply be dropped.
 *
 * Level generation increments on every accepted level entry: START_NEW_GAME,
 * RESUME_SAVE, SCENE_COMPLETE, RACE_START, RACE_RESULT_COMMITTED, SOLO_START
 * and RESTORE_COMMIT. The bump clears the transition latch and the consumed
 * token list; DIALOGUE_COMPLETE clears the latch without a bump (the dialogue
 * was that generation's shared action, and it has finished).
 *
 * SOLO_START copies the roster into suspended_roster. RESTORE_COMMIT compares
 * the event's roster against it — count, seats, characters — and refuses
 * ADVENTURE_PARTY_ERR_ROSTER_MISMATCH on any difference: a solo activity may
 * not hand back a different party than it borrowed.
 *
 * There is deliberately NO event that alters participant count, seats or host
 * mid-session. Re-FORMing requires reaching OFF through EXITING first.
 */
AdventurePartyResult adventure_party_session_apply(
    AdventurePartySession *session, const AdventurePartyEvent *event);

/* Non-zero once a session exists (state != OFF). The first question every
 * game adapter asks; the stock path is the immediate `else`. */
int adventure_party_is_active(const AdventurePartySession *session);

/* The stable 2-4 session roster size; 0 when OFF (or session is NULL). This
 * is NOT the live human count for an activity and NOT the viewport count —
 * those are adventure_party_policy.h queries and are not substitutes. */
int adventure_party_participant_count(const AdventurePartySession *session);

/* ADVENTURE_PARTY_HOST_SEAT, for as long as v1 stands; reads the session so
 * the invariant "the host never changes" is a checkable fact, not a define. */
int adventure_party_host_seat(const AdventurePartySession *session);

/* Stable character identity for a seat, or -1 if the seat is not occupied
 * (or the session is OFF/NULL). Identity never changes while a session is
 * active — a race result, a solo detour, nothing remaps it. */
int adventure_party_character_for_seat(const AdventurePartySession *session,
                                       int seat);

/*
 * Consumes an exact-once completion token around the smallest existing retail
 * progress commit. OK exactly once per key; after that:
 *
 *   ERR_TOKEN_CONSUMED   the same key again — the duplicate-award attempt.
 *   ERR_STALE_GENERATION token minted for another session/level generation
 *                        (e.g. a late commit after the party left the level).
 *   ERR_TOKEN_STATE      no activity is in progress (OFF/FORMING/EXITING).
 *
 * Whether a token exists at all is the policy module's decision
 * (adventure_party_completion_token_issue): quit, retry and loss paths are
 * never issued one, so there is nothing here for them to consume.
 */
AdventurePartyResult adventure_party_consume_completion_token(
    AdventurePartySession *session,
    const AdventurePartyCompletionToken *token);

/* Diagnostic names for traces and test output; never NULL. */
const char *adventure_party_state_name(AdventurePartySessionState state);
const char *adventure_party_result_name(AdventurePartyResult result);

#ifdef __cplusplus
}
#endif

#endif /* MDKR64_ADVENTURE_PARTY_STATE_H */
