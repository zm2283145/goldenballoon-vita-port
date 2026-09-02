/* Assert-driven test: NDEBUG (the Release default) would compile every
 * check away — and delete the registration calls the asserts wrap. */
#undef NDEBUG

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform/net/net_failure_ring.h"

static const char kDumpPath[] = "net_failure_ring_dump.txt";
static const char kEvidencePath[] = "net_failure_ring_evidence.txt";

static unsigned long line_count(const char *path, const char *needle) {
    char line[512];
    unsigned long hits = 0u;
    FILE *file = fopen(path, "rb");
    assert(file != NULL);
    while (fgets(line, (int)sizeof(line), file) != NULL) {
        if (strstr(line, needle) != NULL) hits++;
    }
    fclose(file);
    return hits;
}

/* Read the dumped sequence numbers in file order. */
static unsigned read_sequences(const char *path, unsigned long long *out,
                               unsigned max) {
    char line[512];
    unsigned count = 0u;
    FILE *file = fopen(path, "rb");
    assert(file != NULL);
    while (fgets(line, (int)sizeof(line), file) != NULL && count < max) {
        unsigned long long sequence = 0ull;
        if (sscanf(line, "[NETFAIL] seq=%llu", &sequence) == 1) {
            out[count++] = sequence;
        }
    }
    fclose(file);
    return count;
}

static void test_typed_fields(void) {
    MdkrNetFailureRecord record;
    mdkr_net_failure_ring_reset();
    assert(mdkr_net_failure_ring_retained() == 0u);
    mdkr_net_failure_ring_record_tick(
        MDKR_NET_FAILURE_LATE_INPUT_DISCARDED, 4242u, 2u, 7u, 900u, 901u);
    assert(mdkr_net_failure_ring_retained() == 1u);
    assert(mdkr_net_failure_ring_at(0u, &record));
    assert(record.kind == (uint16_t)MDKR_NET_FAILURE_LATE_INPUT_DISCARDED);
    assert(record.tick == 4242u);
    assert(record.slot == 2u);
    assert(record.detail == 7u);
    assert(record.value_a == 900u && record.value_b == 901u);
    /* A simulation-side record reads no clock at all. */
    assert(record.host_ms == 0u);
    assert(record.code[0] == '\0');

    mdkr_net_failure_ring_record_host(
        MDKR_NET_FAILURE_PEER_LOST, 55000u, 1u, 3u, "ping_timeout");
    assert(mdkr_net_failure_ring_at(1u, &record));
    assert(record.kind == (uint16_t)MDKR_NET_FAILURE_PEER_LOST);
    assert(record.host_ms == 55000u);
    assert(record.slot == 1u && record.detail == 3u);
    assert(strcmp(record.code, "ping_timeout") == 0);
    /* The transport side stamps the last authored tick the ring observed, so
     * the two sides line up without the transport reading the simulation. */
    assert(record.tick == 4242u);
    assert(record.sequence > 0u);

    assert(strcmp(mdkr_net_failure_kind_name(MDKR_NET_FAILURE_PEER_LOST),
                  "peer_lost") == 0);
    assert(strcmp(mdkr_net_failure_kind_name(MDKR_NET_FAILURE_STALL_BEGIN),
                  "stall_begin") == 0);
}

static void test_wrap(void) {
    const unsigned overshoot = MDKR_NET_FAILURE_CAPACITY + 37u;
    MdkrNetFailureRecord oldest, newest;
    unsigned index;
    mdkr_net_failure_ring_reset();
    for (index = 0u; index < overshoot; index++) {
        mdkr_net_failure_ring_record_tick(
            MDKR_NET_FAILURE_FRAME_COMMIT, index, 0u, 0u, index, 0u);
    }
    assert(mdkr_net_failure_ring_recorded() == overshoot);
    assert(mdkr_net_failure_ring_retained() == MDKR_NET_FAILURE_CAPACITY);
    /* Wrap retires the oldest records, never the newest. */
    assert(mdkr_net_failure_ring_at(0u, &oldest));
    assert(oldest.tick == overshoot - MDKR_NET_FAILURE_CAPACITY);
    assert(mdkr_net_failure_ring_at(MDKR_NET_FAILURE_CAPACITY - 1u, &newest));
    assert(newest.tick == overshoot - 1u);
    /* Sequence numbers stay monotonic across the wrap. */
    assert(newest.sequence == oldest.sequence + MDKR_NET_FAILURE_CAPACITY - 1u);
    assert(!mdkr_net_failure_ring_at(MDKR_NET_FAILURE_CAPACITY, &newest));
}

static void test_redaction(void) {
    char field[MDKR_NET_FAILURE_CODE_BYTES];
    MdkrNetFailureRecord record;
    /* Anything that could carry an endpoint address or key material is
     * refused whole, never truncated into a partial address. */
    static const char *const secrets[] = {
        "turn:198.51.100.7:3478?transport=udp",
        "wss://relay.example.net/room",
        "198.51.100.7",
        "alpha bravo charlie",           /* an SAS phrase */
        "user@relay",
        "candidate:1 1 udp 2 10.0.0.4 5",
    };
    unsigned index;
    for (index = 0u; index < sizeof(secrets) / sizeof(secrets[0]); index++) {
        mdkr_net_failure_ring_redact(field, secrets[index]);
        assert(strcmp(field, MDKR_NET_FAILURE_REDACTED) == 0);
    }
    /* A plain typed code survives verbatim. */
    mdkr_net_failure_ring_redact(field, "signal_lost");
    assert(strcmp(field, "signal_lost") == 0);
    mdkr_net_failure_ring_redact(field, NULL);
    assert(field[0] == '\0');
    /* Over-long codes are refused rather than clipped. */
    mdkr_net_failure_ring_redact(field, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    assert(strcmp(field, MDKR_NET_FAILURE_REDACTED) == 0);

    /* The filter is on the recording path, not only the helper. */
    mdkr_net_failure_ring_reset();
    mdkr_net_failure_ring_record_host(
        MDKR_NET_FAILURE_SESSION_FAILURE, 10u, MDKR_NET_FAILURE_NO_SLOT, 0u,
        "wss://relay.example.net/room");
    assert(mdkr_net_failure_ring_at(0u, &record));
    assert(strcmp(record.code, MDKR_NET_FAILURE_REDACTED) == 0);
    assert(mdkr_net_failure_ring_dump(kDumpPath));
    assert(line_count(kDumpPath, "relay.example.net") == 0u);
    assert(line_count(kDumpPath, MDKR_NET_FAILURE_REDACTED) == 1u);
}

static void test_stall_records(void) {
    MdkrNetFailureStall snapshot;
    MdkrNetFailureRecord record;
    uint32_t tick;
    memset(&snapshot, 0, sizeof(snapshot));
    snapshot.peers[1].rtt_ms = 84u;
    snapshot.peers[1].jitter_ms = 19u;
    snapshot.peers[1].bytes_sent = 4096u;
    snapshot.peers[1].bytes_received = 2048u;
    mdkr_net_failure_ring_reset();
    /* Confirmed input keeps pace: no stall. */
    for (tick = 0u; tick < 30u; tick++) {
        mdkr_net_failure_ring_progress(tick, tick * 33u, tick, &snapshot);
    }
    assert(mdkr_net_failure_ring_retained() == 0u);
    /* Confirmation freezes: begin, then repeats, then the watchdog. */
    for (tick = 30u; tick < 300u; tick++) {
        mdkr_net_failure_ring_progress(tick, tick * 33u, 29u, &snapshot);
    }
    assert(mdkr_net_failure_ring_at(0u, &record));
    assert(record.kind == (uint16_t)MDKR_NET_FAILURE_STALL_BEGIN);
    assert(record.stall.peers[1].rtt_ms == 84u);
    assert(record.stall.peers[1].jitter_ms == 19u);
    assert(record.stall.peers[1].bytes_sent == 4096u);
    assert(record.stall.peers[1].bytes_received == 2048u);
    assert(mdkr_net_failure_ring_retained() > 2u);
    {
        unsigned index;
        unsigned ongoing = 0u;
        unsigned watchdog = 0u;
        for (index = 0u; index < mdkr_net_failure_ring_retained(); index++) {
            assert(mdkr_net_failure_ring_at(index, &record));
            if (record.kind == (uint16_t)MDKR_NET_FAILURE_STALL_ONGOING) ongoing++;
            if (record.kind == (uint16_t)MDKR_NET_FAILURE_PROGRESS_WATCHDOG) {
                watchdog++;
            }
        }
        assert(ongoing > 0u);
        /* The watchdog fires once per stall, not once per tick. */
        assert(watchdog == 1u);
    }
    /* Confirmation resumes: exactly one end record, carrying the duration. */
    mdkr_net_failure_ring_progress(300u, 300u * 33u, 300u, &snapshot);
    mdkr_net_failure_ring_progress(301u, 301u * 33u, 301u, &snapshot);
    assert(mdkr_net_failure_ring_at(
        mdkr_net_failure_ring_retained() - 1u, &record));
    assert(record.kind == (uint16_t)MDKR_NET_FAILURE_STALL_END);
    assert(record.value_a > 0u);
}

static void test_dump_order(void) {
    unsigned long long sequences[MDKR_NET_FAILURE_CAPACITY];
    unsigned count;
    unsigned index;
    mdkr_net_failure_ring_reset();
    for (index = 0u; index < MDKR_NET_FAILURE_CAPACITY + 5u; index++) {
        mdkr_net_failure_ring_record_tick(
            MDKR_NET_FAILURE_ROLLBACK_SAVE, index, 0u, 0u, index, 0u);
    }
    assert(mdkr_net_failure_ring_dump(kDumpPath));
    count = read_sequences(kDumpPath, sequences,
                           (unsigned)(sizeof(sequences) / sizeof(sequences[0])));
    assert(count == MDKR_NET_FAILURE_CAPACITY);
    for (index = 1u; index < count; index++) {
        assert(sequences[index] == sequences[index - 1u] + 1ull);
    }
    /* Oldest first: the dump opens on the oldest retained record. */
    {
        MdkrNetFailureRecord oldest;
        assert(mdkr_net_failure_ring_at(0u, &oldest));
        assert(sequences[0] == oldest.sequence);
    }
}

static void test_dump_beside_evidence(void) {
    char beside[256];
    FILE *artifact;
    mdkr_net_failure_ring_reset();
    mdkr_net_failure_ring_record_tick(
        MDKR_NET_FAILURE_LIFECYCLE, 1u, MDKR_NET_FAILURE_NO_SLOT, 0u, 0u, 0u);
#if defined(_WIN32)
    assert(_putenv_s("MDKR_STATE_HASH_FILE", kEvidencePath) == 0);
#else
    assert(setenv("MDKR_STATE_HASH_FILE", kEvidencePath, 1) == 0);
#endif
    artifact = fopen(kEvidencePath, "wb");
    assert(artifact != NULL);
    fclose(artifact);
    assert(mdkr_net_failure_ring_dump_beside_evidence());
    snprintf(beside, sizeof(beside), "%s.netfail", kEvidencePath);
    assert(line_count(beside, "lifecycle") == 1u);
#if defined(_WIN32)
    assert(_putenv_s("MDKR_STATE_HASH_FILE", "") == 0);
#else
    assert(unsetenv("MDKR_STATE_HASH_FILE") == 0);
#endif
    /* No configured artifact means nothing to sit beside. */
    assert(!mdkr_net_failure_ring_dump_beside_evidence());
    remove(beside);
    remove(kEvidencePath);
}

int main(void) {
    assert(mdkr_net_failure_ring_recording());
    test_typed_fields();
    test_wrap();
    test_redaction();
    test_stall_records();
    test_dump_order();
    test_dump_beside_evidence();
    remove(kDumpPath);
    puts("test_net_failure_ring: PASS");
    return 0;
}
