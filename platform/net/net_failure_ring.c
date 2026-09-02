#include "net_failure_ring.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A build defining MDKR_NET_FAILURE_RING_DISABLED keeps the whole API and
 * records nothing: it is the positive control the impairment lane runs, and
 * the shape a shipped build would take if forensics were ever cut. */
#if defined(MDKR_NET_FAILURE_RING_DISABLED)
#define MDKR_NET_FAILURE_RECORDING 0
#else
#define MDKR_NET_FAILURE_RECORDING 1
#endif

/* Confirmed input must stop advancing for this long before the ring calls it a
 * stall; a repeat lands every REPEAT_MS while it lasts, so a long stall costs a
 * bounded share of the ring instead of one record per tick. The watchdog fires
 * once per stall, at the point a race can no longer be expected to recover. */
#define MDKR_NET_FAILURE_STALL_BEGIN_MS 250u
#define MDKR_NET_FAILURE_STALL_REPEAT_MS 500u
#define MDKR_NET_FAILURE_WATCHDOG_MS 5000u

/* Single-writer: the thread that owns the online session records, and the
 * failure/abort path on that same thread dumps. No lock, no allocation, no
 * I/O on the recording path. */
static MdkrNetFailureRecord g_records[MDKR_NET_FAILURE_CAPACITY];
static uint64_t g_recorded;
/* The newest authored tick the simulation side has stamped, so a transport-side
 * record can be placed on the same timeline without reading it. */
static uint32_t g_authored_tick;
/* Install-scoped, not race-scoped: cleared only by an explicit set. */
static char g_log_directory[1024];

/* Stall tracker state (see mdkr_net_failure_ring_progress). */
static uint32_t g_confirmed_through;
static uint32_t g_confirmed_at_ms;
static uint32_t g_stall_noted_ms;
static bool g_stall_open;
static bool g_stall_watchdogged;
static bool g_progress_seen;

static const char *const kKindNames[MDKR_NET_FAILURE_KIND_COUNT] = {
    "rollback_save",
    "rollback_load",
    "desync_tick",
    "recovery",
    "queue_pressure",
    "queue_overflow",
    "frame_commit",
    "input_predicted",
    "late_input_discarded",
    "peer_lost",
    "simhash_divergence",
    "lifecycle",
    "progress_watchdog",
    "stall_begin",
    "stall_ongoing",
    "stall_end",
    "session_failure",
};

bool mdkr_net_failure_ring_recording(void) {
    return MDKR_NET_FAILURE_RECORDING != 0;
}

const char *mdkr_net_failure_kind_name(MdkrNetFailureKind kind) {
    if ((unsigned)kind >= (unsigned)MDKR_NET_FAILURE_KIND_COUNT) return "unknown";
    return kKindNames[(unsigned)kind];
}

static bool redaction_safe(char character) {
    return (character >= 'a' && character <= 'z') ||
           (character >= 'A' && character <= 'Z') ||
           (character >= '0' && character <= '9') ||
           character == '_' || character == '-';
}

void mdkr_net_failure_ring_redact(
    char out[MDKR_NET_FAILURE_CODE_BYTES], const char *source) {
    size_t index;
    if (out == NULL) return;
    memset(out, 0, MDKR_NET_FAILURE_CODE_BYTES);
    if (source == NULL) return;
    for (index = 0u; source[index] != '\0'; index++) {
        if (index + 1u >= (size_t)MDKR_NET_FAILURE_CODE_BYTES ||
            !redaction_safe(source[index])) {
            /* Refuse the whole source rather than clip it: half of a host:port
             * pair or an ICE candidate line is still endpoint material. */
            memcpy(out, MDKR_NET_FAILURE_REDACTED,
                   sizeof(MDKR_NET_FAILURE_REDACTED));
            return;
        }
        out[index] = source[index];
    }
}

#if MDKR_NET_FAILURE_RECORDING
/* A re-simulated tick is behind the authored front by construction, so those
 * kinds must not drag the tick a transport-side record inherits backwards. */
static bool kind_replays(MdkrNetFailureKind kind) {
    return kind == MDKR_NET_FAILURE_ROLLBACK_SAVE ||
           kind == MDKR_NET_FAILURE_ROLLBACK_LOAD ||
           kind == MDKR_NET_FAILURE_DESYNC_TICK;
}

static MdkrNetFailureRecord *push(MdkrNetFailureKind kind, unsigned slot,
                                  unsigned detail) {
    MdkrNetFailureRecord *record =
        &g_records[(size_t)(g_recorded % MDKR_NET_FAILURE_CAPACITY)];
    memset(record, 0, sizeof(*record));
    record->sequence = g_recorded++;
    record->kind = (uint16_t)kind;
    record->slot = (uint8_t)slot;
    record->detail = (uint8_t)detail;
    return record;
}
#endif

void mdkr_net_failure_ring_set_log_directory(const char *directory) {
    size_t length;
    if (directory == NULL || directory[0] == '\0' ||
        (length = strlen(directory)) >= sizeof(g_log_directory)) {
        g_log_directory[0] = '\0';
        return;
    }
    memcpy(g_log_directory, directory, length + 1u);
}

void mdkr_net_failure_ring_reset(void) {
    memset(g_records, 0, sizeof(g_records));
    g_recorded = 0u;
    g_authored_tick = 0u;
    g_confirmed_through = 0u;
    g_confirmed_at_ms = 0u;
    g_stall_noted_ms = 0u;
    g_stall_open = false;
    g_stall_watchdogged = false;
    g_progress_seen = false;
}

void mdkr_net_failure_ring_record_tick(
    MdkrNetFailureKind kind, uint32_t tick, unsigned slot, unsigned detail,
    uint32_t value_a, uint32_t value_b) {
#if MDKR_NET_FAILURE_RECORDING
    MdkrNetFailureRecord *record = push(kind, slot, detail);
    record->tick = tick;
    record->value_a = value_a;
    record->value_b = value_b;
    if (!kind_replays(kind)) g_authored_tick = tick;
#else
    (void)kind; (void)tick; (void)slot; (void)detail;
    (void)value_a; (void)value_b;
#endif
}

void mdkr_net_failure_ring_record_host(
    MdkrNetFailureKind kind, uint32_t host_ms, unsigned slot, unsigned detail,
    const char *code) {
#if MDKR_NET_FAILURE_RECORDING
    MdkrNetFailureRecord *record = push(kind, slot, detail);
    record->tick = g_authored_tick;
    record->host_ms = host_ms;
    mdkr_net_failure_ring_redact(record->code, code);
#else
    (void)kind; (void)host_ms; (void)slot; (void)detail; (void)code;
#endif
}

#if MDKR_NET_FAILURE_RECORDING
static void push_stall(MdkrNetFailureKind kind, uint32_t tick, uint32_t host_ms,
                       uint32_t stalled_ms, const MdkrNetFailureStall *peers) {
    MdkrNetFailureRecord *record =
        push(kind, MDKR_NET_FAILURE_NO_SLOT, 0u);
    record->tick = tick;
    record->host_ms = host_ms;
    record->value_a = stalled_ms;
    record->value_b = g_confirmed_through;
    if (peers != NULL) record->stall = *peers;
}
#endif

void mdkr_net_failure_ring_progress(
    uint32_t tick, uint32_t host_ms, uint32_t confirmed_through,
    const MdkrNetFailureStall *peers) {
#if MDKR_NET_FAILURE_RECORDING
    uint32_t stalled_ms;
    g_authored_tick = tick;
    if (!g_progress_seen || confirmed_through != g_confirmed_through) {
        g_progress_seen = true;
        g_confirmed_through = confirmed_through;
        if (g_stall_open) {
            /* Measured from the LAST CONFIRMATION, the same origin stall_begin
             * and stall_ongoing use, so every stall record reads on one scale.
             * Computed before the new confirmation moves that origin. */
            push_stall(MDKR_NET_FAILURE_STALL_END, tick, host_ms,
                       host_ms - g_confirmed_at_ms, peers);
            g_stall_open = false;
            g_stall_watchdogged = false;
        }
        g_confirmed_at_ms = host_ms;
        return;
    }
    stalled_ms = host_ms - g_confirmed_at_ms;
    if (!g_stall_open) {
        if (stalled_ms < MDKR_NET_FAILURE_STALL_BEGIN_MS) return;
        g_stall_open = true;
        g_stall_noted_ms = host_ms;
        push_stall(MDKR_NET_FAILURE_STALL_BEGIN, tick, host_ms, stalled_ms,
                   peers);
        return;
    }
    if (!g_stall_watchdogged &&
        stalled_ms >= MDKR_NET_FAILURE_WATCHDOG_MS) {
        g_stall_watchdogged = true;
        push_stall(MDKR_NET_FAILURE_PROGRESS_WATCHDOG, tick, host_ms,
                   stalled_ms, peers);
        return;
    }
    if (host_ms - g_stall_noted_ms >= MDKR_NET_FAILURE_STALL_REPEAT_MS) {
        g_stall_noted_ms = host_ms;
        push_stall(MDKR_NET_FAILURE_STALL_ONGOING, tick, host_ms, stalled_ms,
                   peers);
    }
#else
    (void)tick; (void)host_ms; (void)confirmed_through; (void)peers;
#endif
}

unsigned mdkr_net_failure_ring_retained(void) {
    return (g_recorded < (uint64_t)MDKR_NET_FAILURE_CAPACITY)
               ? (unsigned)g_recorded
               : MDKR_NET_FAILURE_CAPACITY;
}

uint64_t mdkr_net_failure_ring_recorded(void) { return g_recorded; }

bool mdkr_net_failure_ring_at(unsigned index, MdkrNetFailureRecord *out) {
    const unsigned retained = mdkr_net_failure_ring_retained();
    uint64_t sequence;
    if (out == NULL || index >= retained) return false;
    sequence = g_recorded - (uint64_t)retained + (uint64_t)index;
    *out = g_records[(size_t)(sequence % MDKR_NET_FAILURE_CAPACITY)];
    return true;
}

#if MDKR_NET_FAILURE_RECORDING
static bool is_stall_kind(uint16_t kind) {
    return kind == (uint16_t)MDKR_NET_FAILURE_STALL_BEGIN ||
           kind == (uint16_t)MDKR_NET_FAILURE_STALL_ONGOING ||
           kind == (uint16_t)MDKR_NET_FAILURE_STALL_END ||
           kind == (uint16_t)MDKR_NET_FAILURE_PROGRESS_WATCHDOG;
}

static void write_record(FILE *file, const MdkrNetFailureRecord *record) {
    fprintf(file,
            "[NETFAIL] seq=%llu tick=%u ms=%u kind=%s slot=%u detail=%u "
            "a=%u b=%u code=%s",
            (unsigned long long)record->sequence, record->tick,
            record->host_ms,
            mdkr_net_failure_kind_name((MdkrNetFailureKind)record->kind),
            (unsigned)record->slot, (unsigned)record->detail,
            record->value_a, record->value_b,
            record->code[0] != '\0' ? record->code : "-");
    if (is_stall_kind(record->kind)) {
        unsigned peer;
        for (peer = 0u; peer < MDKR_NET_FAILURE_PEERS; peer++) {
            const MdkrNetFailurePeerSnapshot *snapshot =
                &record->stall.peers[peer];
            fprintf(file, " peer%u=rtt%u/jit%u/tx%u/rx%u", peer,
                    (unsigned)snapshot->rtt_ms, (unsigned)snapshot->jitter_ms,
                    snapshot->bytes_sent, snapshot->bytes_received);
        }
    }
    fputc('\n', file);
}
#endif

bool mdkr_net_failure_ring_dump(const char *path) {
#if MDKR_NET_FAILURE_RECORDING
    const unsigned retained = mdkr_net_failure_ring_retained();
    unsigned index;
    FILE *file;
    if (path == NULL || path[0] == '\0') return false;
    file = fopen(path, "wb");
    if (file == NULL) return false;
    fprintf(file, "[NETFAIL] begin capacity=%u recorded=%llu retained=%u\n",
            (unsigned)MDKR_NET_FAILURE_CAPACITY,
            (unsigned long long)g_recorded, retained);
    for (index = 0u; index < retained; index++) {
        MdkrNetFailureRecord record;
        if (!mdkr_net_failure_ring_at(index, &record)) break;
        write_record(file, &record);
    }
    fprintf(file, "[NETFAIL] end\n");
    return fclose(file) == 0;
#else
    (void)path;
    return false;
#endif
}

bool mdkr_net_failure_ring_dump_beside_evidence(void) {
    /* The online lanes mirror their per-tick [SIMHASH] rows to
     * MDKR_STATE_HASH_FILE; when one is configured the tail lands next to that
     * artifact so a single capture carries both halves of the story.
     *
     * A player has no such artifact, and a loss they cannot show anyone is
     * worth nothing, so the fallback is the log directory the app shell handed
     * over at install (mdkr_user_log_directory -- where mdkr64.log lives).
     * Overwritten per loss: the race that just ended is what a report is
     * about. */
    const char *artifact = getenv("MDKR_STATE_HASH_FILE");
    char path[sizeof(g_log_directory) + 64];
    size_t length;
    int written;
    if (artifact != NULL && artifact[0] != '\0') {
        written = snprintf(path, sizeof(path), "%s.netfail", artifact);
        if (written < 0 || (size_t)written >= sizeof(path)) return false;
        return mdkr_net_failure_ring_dump(path);
    }
    if (g_log_directory[0] == '\0') return false;
    length = strlen(g_log_directory);
    written = snprintf(
        path, sizeof(path), "%s%s%s", g_log_directory,
        (g_log_directory[length - 1u] == '/' ||
         g_log_directory[length - 1u] == '\\') ? "" : "/",
        MDKR_NET_FAILURE_DUMP_LEAF);
    if (written < 0 || (size_t)written >= sizeof(path)) return false;
    return mdkr_net_failure_ring_dump(path);
}
