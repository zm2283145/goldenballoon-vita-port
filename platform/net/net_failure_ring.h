/* In-process forensics ring for online play.
 *
 * Every record is fixed-width and lands in a statically sized ring, so
 * recording allocates nothing, touches no file, and is safe to call from the
 * tick thread. Only a failure or abort path dumps the retained tail.
 *
 * Timestamps obey the authority boundary: the simulation side stamps records
 * with its own tick counter, and the transport side stamps them with the host
 * clock it already samples for its ladders. Neither side reads a clock the
 * other cannot see, so recording can never fork a deterministic timeline.
 */
#ifndef MDKR_NET_FAILURE_RING_H
#define MDKR_NET_FAILURE_RING_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MDKR_NET_FAILURE_CAPACITY 2048u
#define MDKR_NET_FAILURE_PEERS 4u
/* Fixed-width typed code (a reason/failure name), redaction-filtered on the
 * way in. 23 characters plus the terminator. */
#define MDKR_NET_FAILURE_CODE_BYTES 24u
/* Records that belong to no single canonical slot. */
#define MDKR_NET_FAILURE_NO_SLOT 0xffu

/* The dump's file name when no evidence artifact is configured; it sits in the
 * directory the app shell keeps mdkr64.log in. */
#define MDKR_NET_FAILURE_DUMP_LEAF "mdkr64-online-failure.txt"

/* The text a redaction-filtered code becomes when its source carries material
 * this ring must never retain. */
#define MDKR_NET_FAILURE_REDACTED "redacted"

typedef enum MdkrNetFailureKind {
    MDKR_NET_FAILURE_ROLLBACK_SAVE = 0,
    MDKR_NET_FAILURE_ROLLBACK_LOAD,
    MDKR_NET_FAILURE_DESYNC_TICK,
    MDKR_NET_FAILURE_RECOVERY,
    MDKR_NET_FAILURE_QUEUE_PRESSURE,
    MDKR_NET_FAILURE_QUEUE_OVERFLOW,
    MDKR_NET_FAILURE_FRAME_COMMIT,
    MDKR_NET_FAILURE_INPUT_PREDICTED,
    MDKR_NET_FAILURE_LATE_INPUT_DISCARDED,
    MDKR_NET_FAILURE_PEER_LOST,
    MDKR_NET_FAILURE_SIMHASH_DIVERGENCE,
    MDKR_NET_FAILURE_LIFECYCLE,
    MDKR_NET_FAILURE_PROGRESS_WATCHDOG,
    MDKR_NET_FAILURE_STALL_BEGIN,
    MDKR_NET_FAILURE_STALL_ONGOING,
    MDKR_NET_FAILURE_STALL_END,
    MDKR_NET_FAILURE_SESSION_FAILURE,
    MDKR_NET_FAILURE_KIND_COUNT
} MdkrNetFailureKind;

/* Session boundaries a forensic reader needs to place every other record. */
typedef enum MdkrNetFailureLifecycle {
    MDKR_NET_LIFECYCLE_MESH_UP = 0,
    MDKR_NET_LIFECYCLE_RACE_ARMED,
    MDKR_NET_LIFECYCLE_RACE_FIRST_TICK,
    MDKR_NET_LIFECYCLE_RACE_ENDED,
    MDKR_NET_LIFECYCLE_MESH_CLOSED,
    /* A room-reported departure finalised a seat: value_a carries the
     * MdkrMatchTakeoverResult, so a one-sided finalisation (a CONFLICT or
     * TOO_LATE on one survivor and an ACCEPTED on another) is visible in a
     * dump instead of only in a log line. Appended so the prior codes' values
     * never shift. */
    MDKR_NET_LIFECYCLE_DEPARTURE_FINALISED,
    /* A peer's finalisation proposal was refused before it could take effect
     * -- the wrong sender for the surviving roster, another race's epoch, or
     * no room verdict of our own to intersect it with. */
    MDKR_NET_LIFECYCLE_DEPARTURE_REFUSED,
    /* A room departure verdict was HELD rather than acted on: an
     * authenticated packet arrived from the departed endpoint inside the
     * peer-silence grace, so the peer was plainly still racing and the
     * transport ladders were left to decide. value_a carries the authenticated
     * packets counted in the grace, value_b the tick the grace opened at. */
    MDKR_NET_LIFECYCLE_DEPARTURE_HELD
} MdkrNetFailureLifecycle;

/* Per-peer link snapshot carried by every stall record. Widths are fixed and
 * saturating: a snapshot never grows the record or allocates. */
typedef struct MdkrNetFailurePeerSnapshot {
    uint16_t rtt_ms;
    uint16_t jitter_ms;
    uint32_t bytes_sent;
    uint32_t bytes_received;
} MdkrNetFailurePeerSnapshot;

typedef struct MdkrNetFailureStall {
    MdkrNetFailurePeerSnapshot peers[MDKR_NET_FAILURE_PEERS];
} MdkrNetFailureStall;

typedef struct MdkrNetFailureRecord {
    /* Monotonic across wrap, so the dump can order the retained tail. */
    uint64_t sequence;
    uint32_t tick;
    uint32_t host_ms;
    uint32_t value_a;
    uint32_t value_b;
    uint16_t kind;
    uint8_t slot;
    /* Kind-specific typed sub-code (recovery reason, ingress result,
     * lifecycle boundary, peer-loss reason ordinal). */
    uint8_t detail;
    MdkrNetFailureStall stall;
    char code[MDKR_NET_FAILURE_CODE_BYTES];
} MdkrNetFailureRecord;

/* False in a build compiled with MDKR_NET_FAILURE_RING_DISABLED: every
 * recorder is a no-op and every dump writes nothing. */
bool mdkr_net_failure_ring_recording(void);

void mdkr_net_failure_ring_reset(void);

/* Where a dump goes when no evidence artifact is configured -- every shipped
 * build. The app shell resolves the directory the same way it names mdkr64.log
 * (mdkr_user_log_directory) and hands it over once, so this module stays free
 * of the path policy's SDL and filesystem dependencies. NULL or "" clears it.
 * Survives mdkr_net_failure_ring_reset(): it is a property of the install, not
 * of a race. */
void mdkr_net_failure_ring_set_log_directory(const char *directory);

/* Simulation-side record: the caller owns the tick, and no clock is read. */
void mdkr_net_failure_ring_record_tick(
    MdkrNetFailureKind kind, uint32_t tick, unsigned slot, unsigned detail,
    uint32_t value_a, uint32_t value_b);

/* Transport-side record: the caller owns the host clock it already samples,
 * and the ring stamps the last authored tick it observed so the two sides
 * line up in the dump without the transport reading the simulation. `code`
 * may be NULL; otherwise it is copied through the redaction filter. */
void mdkr_net_failure_ring_record_host(
    MdkrNetFailureKind kind, uint32_t host_ms, unsigned slot, unsigned detail,
    const char *code);

/* One authored-tick progress observation, from the tick thread. Emits
 * STALL_BEGIN / STALL_ONGOING / STALL_END and PROGRESS_WATCHDOG on its own as
 * `confirmed_through` stops and resumes advancing. `peers` may be NULL. */
void mdkr_net_failure_ring_progress(
    uint32_t tick, uint32_t host_ms, uint32_t confirmed_through,
    const MdkrNetFailureStall *peers);

/* Copy a source string into a fixed-width code field, dropping anything that
 * could carry an endpoint address or SAS/TURN material. A source holding any
 * character outside [A-Za-z0-9_-], or longer than the field, becomes
 * MDKR_NET_FAILURE_REDACTED in full: URLs, host:port pairs, ICE candidate
 * lines and spaced SAS phrases all fail that test, so none can reach a dump. */
void mdkr_net_failure_ring_redact(
    char out[MDKR_NET_FAILURE_CODE_BYTES], const char *source);

/* Retained-tail readers, oldest first. `index` runs [0, retained). */
unsigned mdkr_net_failure_ring_retained(void);
uint64_t mdkr_net_failure_ring_recorded(void);
bool mdkr_net_failure_ring_at(unsigned index, MdkrNetFailureRecord *out);

const char *mdkr_net_failure_kind_name(MdkrNetFailureKind kind);

/* Write the retained tail, oldest first, to `path`. Failure/abort paths only:
 * this is the ring's one I/O seam. */
bool mdkr_net_failure_ring_dump(const char *path);

/* Dump beside the per-tick state-hash evidence artifact the online lanes
 * already write (MDKR_STATE_HASH_FILE), as "<artifact>.netfail". With no
 * artifact configured -- every shipped build -- it falls back to
 * MDKR_NET_FAILURE_DUMP_LEAF in the log directory set above, so a player's
 * loss lands beside the mdkr64.log support already asks for. Overwritten per
 * loss: the race that just ended is what a report is about. */
bool mdkr_net_failure_ring_dump_beside_evidence(void);

#ifdef __cplusplus
}
#endif
#endif
