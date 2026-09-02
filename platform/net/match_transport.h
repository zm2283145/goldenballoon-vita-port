/* Launcher-owned canonical input adapter. Networking/authentication stays out
 * of the engine; this boundary accepts only already-authenticated peer input. */
#ifndef MDKR_MATCH_TRANSPORT_H
#define MDKR_MATCH_TRANSPORT_H

#include "net_input.h"
#include "rollback/rollback_limits.h"
#include "session/session_bridge.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Must match the real-game snapshot retention contract: a packet outside this
 * authored depth cannot be reconciled and is rejected before it can dirty the
 * engine timeline. */
#define MDKR_MATCH_TRANSPORT_ROLLBACK_TICKS \
    MDKR_ROLLBACK_MAX_INPUT_AGE_TICKS

typedef enum MdkrMatchRecoveryReason {
    MDKR_MATCH_RECOVERY_NONE = 0,
    MDKR_MATCH_RECOVERY_INPUT_GAP,
    MDKR_MATCH_RECOVERY_LATE_INPUT
} MdkrMatchRecoveryReason;

typedef struct MdkrMatchRecovery {
    MdkrMatchRecoveryReason reason;
    uint32_t first_unrecoverable_tick;
    uint32_t observed_at_tick;
    uint8_t canonical_slot;
} MdkrMatchRecovery;

typedef enum MdkrMatchTransportIngressResult {
    MDKR_MATCH_INGRESS_ACCEPTED = 0,
    MDKR_MATCH_INGRESS_CORRECTED,
    MDKR_MATCH_INGRESS_DUPLICATE,
    MDKR_MATCH_INGRESS_INVALID,
    MDKR_MATCH_INGRESS_STALE_EPOCH,
    MDKR_MATCH_INGRESS_UNAUTHORIZED,
    MDKR_MATCH_INGRESS_CONFLICT,
    MDKR_MATCH_INGRESS_OUT_OF_WINDOW,
    MDKR_MATCH_INGRESS_TAKEN_OVER
} MdkrMatchTransportIngressResult;

typedef enum MdkrMatchTakeoverResult {
    MDKR_MATCH_TAKEOVER_ACCEPTED = 0,
    MDKR_MATCH_TAKEOVER_DUPLICATE,
    MDKR_MATCH_TAKEOVER_INVALID,
    MDKR_MATCH_TAKEOVER_STALE_EPOCH,
    MDKR_MATCH_TAKEOVER_TOO_LATE,
    MDKR_MATCH_TAKEOVER_CONFLICT
} MdkrMatchTakeoverResult;

typedef struct MdkrMatchTransportStats {
    uint32_t accepted;
    uint32_t corrected;
    uint32_t duplicates;
    uint32_t invalid;
    uint32_t stale_epoch;
    uint32_t unauthorized;
    uint32_t conflicts;
    uint32_t out_of_window;
    uint32_t drained;
    uint32_t drain_rejected;
    uint32_t takeover_started;
    uint32_t takeover_ignored_inputs;
} MdkrMatchTransportStats;

/* What the last committed frame looked like, for the forensics ring alone.
 * Recording a frame-commit every tick would spend the whole ring on steady
 * state, so only a CHANGE in the committed masks is worth a record, and the
 * run length says how many ticks the state it replaces had held. */
typedef struct MdkrMatchCommitTrace {
    uint32_t run_ticks;
    uint8_t confirmed_mask;
    uint8_t present_mask;
    /* Remote slots whose committed frame came from prediction, not a received
     * packet: a change here is the input-prediction transition. */
    uint8_t predicted_slot_mask;
    bool started;
} MdkrMatchCommitTrace;

typedef struct MdkrMatchTransport {
    MdkrNetInputHistory history;
    MdkrSessionBridge *bridge;
    uint32_t match_epoch;
    uint8_t active_slot_mask;
    uint8_t local_slot_mask;
    uint8_t remote_slot_mask;
    uint8_t reserved;
    uint32_t remote_confirmed_through[MDKR_NET_INPUT_SLOTS];
    uint32_t ai_takeover_tick[MDKR_NET_INPUT_SLOTS];
    bool remote_have_confirmed[MDKR_NET_INPUT_SLOTS];
    uint8_t ai_takeover_scheduled_mask;
    uint8_t ai_takeover_started_mask;
    MdkrMatchRecovery recovery;
    MdkrMatchCommitTrace commit_trace;
    MdkrMatchTransportStats stats;
    bool ready;
} MdkrMatchTransport;

/* Binds exactly one admitted bridge epoch. The caller owns both objects and
 * must keep the bridge alive until shutdown. */
bool mdkr_match_transport_init(
    MdkrMatchTransport *transport, MdkrSessionBridge *bridge,
    uint32_t first_tick);

/* `authenticated_slot_mask` comes from the launcher's authenticated peer
 * identity, never from packet bytes. It may authorize several couch seats;
 * the requested slot must be remote, active, and included in that mask. */
MdkrMatchTransportIngressResult mdkr_match_transport_receive(
    MdkrMatchTransport *transport, uint32_t match_epoch,
    uint8_t authenticated_slot_mask, unsigned slot, uint32_t tick,
    const MdkrPadSample *sample);

/* Samples local seats once, combines them with received/repeat-last remote
 * input, and commits atomically through MdkrSessionBridge. */
bool mdkr_match_transport_drain_tick(
    MdkrMatchTransport *transport, uint32_t match_epoch, uint32_t tick,
    const MdkrPadSample *local_samples, unsigned local_sample_count);

bool mdkr_match_transport_take_dirty(
    MdkrMatchTransport *transport, uint32_t *tick);
/* Apply an already-authorized room control-log decision. Activation must be a
 * future authored tick. It is immutable/idempotent and has no mid-race
 * handback: a different tick for the same slot is a conflict. */
MdkrMatchTakeoverResult mdkr_match_transport_schedule_ai_takeover(
    MdkrMatchTransport *transport, uint32_t match_epoch, unsigned slot,
    uint32_t activation_tick);
bool mdkr_match_transport_ai_takeover_mask_for_tick(
    const MdkrMatchTransport *transport, uint32_t match_epoch, uint32_t tick,
    uint8_t *slot_mask);
/* Copy the canonical frame retained for an authored/replay tick. This is the
 * only history view exported to the engine provider; internal status cells and
 * mutable transport state never cross the launcher boundary. */
bool mdkr_match_transport_inputs_for_tick(
    MdkrMatchTransport *transport, uint32_t match_epoch, uint32_t tick,
    MdkrInputSet *out);
const MdkrMatchTransportStats *mdkr_match_transport_stats(
    const MdkrMatchTransport *transport);
/* The contiguous run of authored ticks a REMOTE slot is still missing at the
 * drain frontier, capped at MDKR_MATCH_TRANSPORT_ROLLBACK_TICKS (a longer run
 * could not be reconciled even if it arrived). False when the slot is not
 * remote, the frontier has not reached the gap, or nothing is missing. The
 * caller decides whether the run has outlived its carrier's redundancy and is
 * worth repairing; this reports only what the history knows.
 * Read-only: it never advances confirmation or latches recovery. */
bool mdkr_match_transport_input_gap(
    const MdkrMatchTransport *transport, unsigned slot,
    uint32_t *first_tick, uint32_t *count);

/* Sticky, exact-epoch recovery request. Once a gap can no longer be replayed,
 * the launcher must stop admission/gameplay and choose resync or disconnect. */
bool mdkr_match_transport_recovery(
    const MdkrMatchTransport *transport, MdkrMatchRecovery *recovery);

/* ---- Lobby-authoritative peer drop --------------------------------------
 *
 * When the room reports that a member left, the seat it owned stops consuming
 * that peer's input at an agreed tick and consumes neutral frames from then
 * on. The commitment itself is the takeover schedule above -- it already
 * authors one neutral received sample per tick for the seat, refuses later
 * input for it, and treats a second, different tick for the same seat as a
 * conflict. These two rules are what decides WHICH tick, and WHO gets to say
 * so, without which two endpoints could finalise the same seat differently and
 * author different races.
 *
 * The tick every endpoint must agree on. `confirmed_through` is the floor:
 * nothing past the confirmed frontier is common knowledge, so finalising
 * earlier would replace a frame already committed with the departed peer's
 * real input. `current_tick` raises it, because an authored tick's inputs are
 * already spent (schedule_ai_takeover refuses a tick that is not strictly
 * ahead of the head). `lead_ticks` -- the match's agreed input delay -- raises
 * it again, so a proposal is still ahead of a survivor whose own head runs that
 * far in front of this one by the time the proposal lands. Half-range tick
 * ordering throughout, so the result wraps with the tick space. */
uint32_t mdkr_match_drop_finalisation_tick(
    uint32_t confirmed_through, bool have_confirmed, uint32_t current_tick,
    uint8_t lead_ticks);

/* Whether this endpoint is the one that proposes the tick for a departure, out
 * of the `count` endpoints still in the room. The lowest surviving endpoint id
 * proposes and every other survivor adopts what it sends: endpoint ids are
 * unique within a room, so the rule is total and every survivor evaluates it
 * identically from the same surviving roster. False for an endpoint the roster
 * does not name, and for an empty roster. A sole survivor -- 2P, the shipping
 * case -- is trivially the proposer, which is why a 2P drop needs no round
 * trip: there is nobody left to agree with, and the room already confirmed the
 * leave. */
bool mdkr_match_drop_is_proposer(
    uint64_t local_endpoint_id, const uint64_t *surviving, unsigned count);

#ifdef __cplusplus
}
#endif
#endif
