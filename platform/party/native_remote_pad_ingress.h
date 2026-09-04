/*
 * Native Phone Party transport -> engine handoff.
 *
 * WebRTC callbacks run outside the engine thread.  This bounded process-owned
 * queue is the only native crossing: transport code publishes already-framed
 * Party v1 packets and the SDL input boundary drains them.  It deliberately
 * owns no sockets, room state or UI.
 */
#ifndef MDKR_NATIVE_REMOTE_PAD_INGRESS_H
#define MDKR_NATIVE_REMOTE_PAD_INGRESS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "party_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MDKR_NATIVE_REMOTE_PAD_PORTS 4u
#define MDKR_NATIVE_REMOTE_PAD_QUEUE_CAPACITY 32u

typedef struct MdkrNativeRemotePadIngressStats {
    uint64_t binds;
    uint64_t releases;
    uint64_t packets;
    uint64_t malformed;
    uint64_t stale;
    uint64_t overflows;
    uint64_t rumble_requests;
    uint64_t race_state_changes;
} MdkrNativeRemotePadIngressStats;

/*
 * P2.2 in-race phone feedback. The launcher-owned twin of the rumble mailbox,
 * but ENGINE -> phone in the other direction: the HUD forwards the per-racer
 * quantities a confirmed phone renders (held item, lap, position, countdown,
 * finish). Every field is a value the HUD already computed for this frame --
 * this carries presentation state, never simulation authority, and is never
 * hashed. All int so the platform bridge (C) fills it without a cast.
 */
typedef struct MdkrNativeRaceState {
    int racing;         /* 1 while a race/challenge/time-trial HUD is live */
    int item_type;      /* BalloonType: 0 boost 1 missiles 2 traps 3 shield 4 magnet */
    int item_level;     /* power level of the held item */
    int item_quantity;  /* how many are held; 0 means no item */
    int lap;            /* zero-based, as the racer stores it */
    int lap_total;      /* laps in this race */
    int position;       /* 1-based race position */
    int field_size;     /* racers in the field */
    int countdown;      /* raw get_race_countdown(); >0 while the grid holds */
    int finished;       /* 1 once this racer has crossed the line */
    int finish_position;/* final placing, valid when finished */
} MdkrNativeRaceState;

/*
 * Bind/rebind is a reliable-control-plane event.  owner and
 * connection_sequence are non-zero. A new binding clears buffered data from
 * the old epoch before becoming visible to the engine.
 */
bool mdkr_native_remote_pad_bind(
    unsigned port, uint64_t owner, uint32_t connection_sequence);

/* A delayed release cannot revoke a replacement: both identity fields match. */
bool mdkr_native_remote_pad_release(
    unsigned port, uint64_t owner, uint32_t connection_sequence);
bool mdkr_native_remote_pad_set_haptics(
    unsigned port, uint64_t owner, uint32_t connection_sequence,
    bool supported);
bool mdkr_native_remote_pad_haptics_supported(unsigned port);

/*
 * Validates the complete Party v1 frame before enqueueing it. Queue exhaustion
 * revokes the binding and clears every queued packet, forcing neutral rather
 * than allowing an old accelerator sample to survive congestion.
 */
bool mdkr_native_remote_pad_push(
    unsigned port, uint64_t owner, uint32_t connection_sequence,
    const uint8_t *bytes, size_t length);

/* Engine-thread read side. Identity is a snapshot, not a borrowed pointer. */
bool mdkr_native_remote_pad_info(
    unsigned port, uint64_t *out_owner, uint32_t *out_connection_sequence);
size_t mdkr_native_remote_pad_pop(
    unsigned port, uint64_t owner, uint32_t connection_sequence,
    uint8_t *output, size_t capacity);

/* Engine -> launcher transport control. Only the newest magnitude is useful. */
bool mdkr_native_remote_pad_request_rumble(
    unsigned port, uint16_t strength);
bool mdkr_native_remote_pad_take_rumble(
    unsigned port, uint64_t owner, uint32_t connection_sequence,
    uint16_t *out_strength);
/*
 * Read-only twin of take (M5 sustained rumble): reports the mailbox's
 * current magnitude without consuming the pending flag. The magnitude
 * persists after a take because the engine only posts CHANGES (start /
 * stop), while a phone's rumble command is a deliberate 250 ms one-shot --
 * so this is how the launcher's refresh loop asks "does the engine still
 * want this motor on?" between posts. Identity-checked like every other
 * crossing, and a rebind, release, or haptics loss zeroes the magnitude
 * (clearPayload / set_haptics), so a stale channel can never keep
 * reporting strength.
 */
bool mdkr_native_remote_pad_peek_rumble(
    unsigned port, uint64_t owner, uint32_t connection_sequence,
    uint16_t *out_strength);

/*
 * P2.2 engine -> launcher in-race feedback. publish stores the newest HUD
 * snapshot for a reserved seat and flags it pending ONLY when a field changed,
 * so the reliable control channel stays change-driven (a few Hz), never
 * per-frame -- the HUD may call this every frame and interpolation replay may
 * call it several times a frame; unchanged calls cost one bounded compare and
 * send nothing. A rebind/release clears the snapshot, so the first publish
 * after a phone (re)connects always resends the current state. take is the
 * launcher-thread read side, identity-checked like every other crossing; it
 * returns the pending snapshot once and clears the flag.
 */
bool mdkr_native_remote_pad_publish_race_state(
    unsigned port, const MdkrNativeRaceState *state);
bool mdkr_native_remote_pad_take_race_state(
    unsigned port, uint64_t owner, uint32_t connection_sequence,
    MdkrNativeRaceState *out_state);

void mdkr_native_remote_pad_stats(
    unsigned port, MdkrNativeRemotePadIngressStats *out_stats);

/* Process/test teardown. Ordinary engine-session teardown must not call this:
 * an approved phone keeps its launcher-owned seat across a rematch. */
void mdkr_native_remote_pad_reset_all(void);

#ifdef __cplusplus
}
#endif

#endif /* MDKR_NATIVE_REMOTE_PAD_INGRESS_H */
