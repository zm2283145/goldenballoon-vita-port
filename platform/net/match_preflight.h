/* Pure launcher-owned consensus gate between room Loading and engine launch. */
#ifndef MDKR_MATCH_PREFLIGHT_H
#define MDKR_MATCH_PREFLIGHT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "match_launch_descriptor.h"
#include "match_peer_crypto.h"
#include "match_peer_graph.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MDKR_MATCH_PREFLIGHT_VERSION           2u
#define MDKR_MATCH_PREFLIGHT_DIGEST_BYTES      32u
#define MDKR_MATCH_PREFLIGHT_ROM_VERIFIED      0x01u
#define MDKR_MATCH_PREFLIGHT_PHRASE_CONFIRMED  0x02u
#define MDKR_MATCH_PREFLIGHT_CHANNELS_READY    0x04u
#define MDKR_MATCH_PREFLIGHT_ROUTE_MEASURED    0x08u
/* The three checks READY requires. The route measurement is reported, not
 * required: a route that measures badly informs the player and widens this
 * endpoint's entry timing, it does not refuse the launch. */
#define MDKR_MATCH_PREFLIGHT_ALL_FLAGS         0x07u
#define MDKR_MATCH_PREFLIGHT_FLAG_MASK         0x0Fu
/* MPF1 was 124; MPF2 appends the 12-byte route-quality record. */
#define MDKR_MATCH_PREFLIGHT_ATTESTATION_BYTES 136u
#define MDKR_MATCH_PREFLIGHT_MEASUREMENT_OFFSET 124u
#define MDKR_MATCH_PREFLIGHT_FRAGMENT_HEADER_BYTES 6u
#define MDKR_MATCH_PREFLIGHT_FRAGMENT_DATA_BYTES 58u
#define MDKR_MATCH_PREFLIGHT_FRAGMENT_COUNT 3u

#if defined(__cplusplus)
static_assert(MDKR_MATCH_PREFLIGHT_FRAGMENT_HEADER_BYTES +
                      MDKR_MATCH_PREFLIGHT_FRAGMENT_DATA_BYTES ==
                  MDKR_MATCH_PEER_PAYLOAD_BYTES,
              "preflight fragment must fill one peer payload");
static_assert(MDKR_MATCH_PREFLIGHT_FRAGMENT_COUNT ==
                  (MDKR_MATCH_PREFLIGHT_ATTESTATION_BYTES +
                   MDKR_MATCH_PREFLIGHT_FRAGMENT_DATA_BYTES - 1u) /
                      MDKR_MATCH_PREFLIGHT_FRAGMENT_DATA_BYTES,
              "preflight fragment count must cover exactly one attestation");
#else
_Static_assert(MDKR_MATCH_PREFLIGHT_FRAGMENT_HEADER_BYTES +
                       MDKR_MATCH_PREFLIGHT_FRAGMENT_DATA_BYTES ==
                   MDKR_MATCH_PEER_PAYLOAD_BYTES,
               "preflight fragment must fill one peer payload");
_Static_assert(MDKR_MATCH_PREFLIGHT_FRAGMENT_COUNT ==
                   (MDKR_MATCH_PREFLIGHT_ATTESTATION_BYTES +
                    MDKR_MATCH_PREFLIGHT_FRAGMENT_DATA_BYTES - 1u) /
                       MDKR_MATCH_PREFLIGHT_FRAGMENT_DATA_BYTES,
               "preflight fragment count must cover exactly one attestation");
#endif

/* ---- Route-quality measurement (MPF2) ------------------------------------
 *
 * Before admission completes, each launcher replays the real match lanes at
 * their real cadence and payload size, then scores what came back. The
 * component below is pure: it owns no socket and reads no clock. The caller
 * hands it the host milliseconds it already samples for its own ladders, so
 * nothing on the simulation side of the authority boundary is involved.
 *
 * Both lanes carry one fixed 64-byte peer payload, which is exactly the input
 * bundle's size (MDKR_MATCH_INPUT_BUNDLE_BYTES) and exactly one sealed
 * control fragment. The bundle lane replays the authored tick cadence on the
 * unreliable state channel, so real datagram loss is visible; the control
 * lane replays 200 ms on the reliable ordered control channel, where SCTP
 * retransmission turns loss into latency instead. */
#define MDKR_MATCH_ROUTE_MEASURE_MS        6000u
#define MDKR_MATCH_ROUTE_DRAIN_MS          1000u
#define MDKR_MATCH_ROUTE_CONTROL_CADENCE_MS 200u
/* A sample slower than this counts as late, matching the depth past which a
 * 30 Hz authored tick can no longer absorb the arrival without a correction. */
#define MDKR_MATCH_ROUTE_LATE_SAMPLE_MS    100u
/* 6 s of the 30 Hz bundle lane (182) plus the 200 ms control lane (30). */
#define MDKR_MATCH_ROUTE_MAX_PROBES        256u
/* Every endpoint leads by at least the manifest's agreed input_delay and by
 * at most this many authored ticks, however bad its own route measures. */
#define MDKR_MATCH_ROUTE_INPUT_DELAY_CAP   4u

typedef enum MdkrMatchRouteLane {
    MDKR_MATCH_ROUTE_LANE_BUNDLE = 0,
    MDKR_MATCH_ROUTE_LANE_CONTROL,
    MDKR_MATCH_ROUTE_LANE_COUNT
} MdkrMatchRouteLane;

typedef enum MdkrMatchRouteProbeKind {
    MDKR_MATCH_ROUTE_PROBE = 0,
    MDKR_MATCH_ROUTE_ECHO
} MdkrMatchRouteProbeKind;

/* Named quality bands over the 1-10 score. Their copy is the one the room
 * chip shows; the ladder that produces the score lives in match_preflight.c
 * and is specified in docs/ref/match-preflight-v1.md's v2 section. */
typedef enum MdkrMatchRouteBand {
    MDKR_MATCH_ROUTE_BAND_NONE = 0,
    MDKR_MATCH_ROUTE_BAND_ROUGH,
    MDKR_MATCH_ROUTE_BAND_UNEVEN,
    MDKR_MATCH_ROUTE_BAND_STEADY
} MdkrMatchRouteBand;

/* The 12 wire bytes MPF2 appends to MPF1's report. */
typedef struct MdkrMatchRouteMeasurement {
    uint16_t p95_rtt_ms;
    uint16_t jitter_ms;
    uint16_t loss_per_thousand;
    uint16_t late_per_thousand;
    /* Inbound pump-drain drops across the window: the carrier's bounded
     * callback->pump and pump->drainEvents queues overflowed, so the local
     * pump did not keep up. Supplied by the caller at finish. */
    uint16_t undrained;
    uint8_t  score; /* 1..10 */
    uint8_t  band;  /* MdkrMatchRouteBand */
} MdkrMatchRouteMeasurement;

typedef struct MdkrMatchRouteProbe {
    uint64_t origin_endpoint_id;
    uint32_t sequence;
    uint8_t  lane;
    uint8_t  kind;
} MdkrMatchRouteProbe;

typedef struct MdkrMatchRouteMeasureState {
    uint64_t origin_endpoint_id;
    uint32_t begin_ms;
    uint32_t tick_ms;
    uint32_t next_send_ms[MDKR_MATCH_ROUTE_LANE_COUNT];
    uint32_t next_sequence;
    uint32_t sent_ms[MDKR_MATCH_ROUTE_MAX_PROBES];
    uint16_t rtt_ms[MDKR_MATCH_ROUTE_MAX_PROBES];
    uint8_t  lane[MDKR_MATCH_ROUTE_MAX_PROBES];
    uint8_t  echoed[MDKR_MATCH_ROUTE_MAX_PROBES];
} MdkrMatchRouteMeasureState;

/* Fills `measurement`'s score and band from its five metric fields. Rejects a
 * record whose per-thousand fields exceed 1000, leaving output unchanged. */
bool mdkr_match_route_measurement_score(MdkrMatchRouteMeasurement *measurement);
MdkrMatchRouteBand mdkr_match_route_band(uint8_t score);
/* "steady" / "uneven" / "rough"; NULL for a score outside 1-10. */
const char *mdkr_match_route_band_name(MdkrMatchRouteBand band);

/* The entry-timing widen. The manifest's input_delay is the agreed floor every
 * endpoint admitted on; measured p95 RTT may lead by further whole authored
 * ticks up to MDKR_MATCH_ROUTE_INPUT_DELAY_CAP. Returns the floor unchanged
 * for a zero tick period or an already-capped floor. */
uint8_t mdkr_match_route_input_delay(uint16_t p95_rtt_ms, uint32_t tick_ms,
                                     uint8_t manifest_input_delay);

/* Fixed 64-byte probe payload. It rides the lanes' existing payload types, so
 * the sealed envelope's type space is unchanged; the launcher only accepts
 * probes before its race transport exists, which is the only window in which
 * this component runs. */
bool mdkr_match_route_probe_encode(
    const MdkrMatchRouteProbe *probe,
    uint8_t payload[MDKR_MATCH_PEER_PAYLOAD_BYTES]);
bool mdkr_match_route_probe_decode(
    const uint8_t payload[MDKR_MATCH_PEER_PAYLOAD_BYTES],
    MdkrMatchRouteProbe *output);

bool mdkr_match_route_measure_begin(MdkrMatchRouteMeasureState *state,
                                    uint32_t now_ms, uint32_t tick_ms,
                                    uint64_t origin_endpoint_id);
/* Emits at most one probe per call; the caller loops until it returns false.
 * Slots are scheduled off each lane's cadence rather than off now_ms, so a
 * coarse pump replays the slots it owes as one burst on its next call instead
 * of dropping them, and no lane emits past MDKR_MATCH_ROUTE_MEASURE_MS. */
bool mdkr_match_route_measure_due(MdkrMatchRouteMeasureState *state,
                                  uint32_t now_ms, MdkrMatchRouteProbe *output);
/* One returned echo. Unknown, duplicate and unsent sequences are ignored. */
void mdkr_match_route_measure_echo(MdkrMatchRouteMeasureState *state,
                                   uint32_t sequence, uint32_t now_ms);
/* True once the send window and the drain have both elapsed. A corrupt state
 * settles immediately, so a caller loop always terminates. */
bool mdkr_match_route_measure_settled(const MdkrMatchRouteMeasureState *state,
                                      uint32_t now_ms);
/* Reduces the samples to a scored record. `undrained` is the carrier's inbound
 * pump-drain drop count across the window, which only the caller can see.
 * False, output unchanged, when no probe was ever sent. */
bool mdkr_match_route_measure_finish(const MdkrMatchRouteMeasureState *state,
                                     uint32_t undrained,
                                     MdkrMatchRouteMeasurement *output);

typedef struct MdkrMatchPreflightAttestationV1 {
    uint32_t protocol_version;
    uint32_t match_epoch;
    uint32_t connection_generation;
    uint32_t sequence;
    uint64_t endpoint_id;
    uint8_t  descriptor_digest[MDKR_MATCH_PREFLIGHT_DIGEST_BYTES];
    uint8_t  transcript_digest[MDKR_MATCH_PREFLIGHT_DIGEST_BYTES];
    uint8_t  graph_digest[MDKR_MATCH_PREFLIGHT_DIGEST_BYTES];
    uint8_t  flags;
    uint8_t  reserved[7];
    /* All zero unless MDKR_MATCH_PREFLIGHT_ROUTE_MEASURED is set. */
    MdkrMatchRouteMeasurement measurement;
} MdkrMatchPreflightAttestationV1;

typedef struct MdkrMatchPreflightFragmentState {
    /* Canonical authenticated sender -> recipient direction for every
     * fragment in this reassembly. It is never inferred from report bytes. */
    MdkrMatchPeerKeyContext direction;
    uint32_t sequence;
    uint8_t  present_mask;
    uint8_t  encoded[MDKR_MATCH_PREFLIGHT_ATTESTATION_BYTES];
} MdkrMatchPreflightFragmentState;

typedef enum MdkrMatchPreflightFragmentResult {
    MDKR_MATCH_PREFLIGHT_FRAGMENT_ACCEPTED = 0,
    MDKR_MATCH_PREFLIGHT_FRAGMENT_COMPLETE,
    MDKR_MATCH_PREFLIGHT_FRAGMENT_DUPLICATE,
    MDKR_MATCH_PREFLIGHT_FRAGMENT_INVALID,
    MDKR_MATCH_PREFLIGHT_FRAGMENT_CONTEXT_MISMATCH,
    MDKR_MATCH_PREFLIGHT_FRAGMENT_STALE_SEQUENCE,
    MDKR_MATCH_PREFLIGHT_FRAGMENT_CONFLICT
} MdkrMatchPreflightFragmentResult;

typedef enum MdkrMatchPreflightSubmitResult {
    MDKR_MATCH_PREFLIGHT_SUBMIT_ACCEPTED = 0,
    MDKR_MATCH_PREFLIGHT_SUBMIT_DUPLICATE,
    MDKR_MATCH_PREFLIGHT_SUBMIT_INVALID,
    MDKR_MATCH_PREFLIGHT_SUBMIT_AUTHENTICATED_SOURCE_MISMATCH,
    MDKR_MATCH_PREFLIGHT_SUBMIT_STALE_EPOCH,
    MDKR_MATCH_PREFLIGHT_SUBMIT_UNKNOWN_ENDPOINT,
    MDKR_MATCH_PREFLIGHT_SUBMIT_STALE_GENERATION,
    MDKR_MATCH_PREFLIGHT_SUBMIT_STALE_SEQUENCE,
    MDKR_MATCH_PREFLIGHT_SUBMIT_CONFLICT
} MdkrMatchPreflightSubmitResult;

typedef enum MdkrMatchPreflightState {
    MDKR_MATCH_PREFLIGHT_INVALID = 0,
    MDKR_MATCH_PREFLIGHT_DESCRIPTOR_MISMATCH,
    MDKR_MATCH_PREFLIGHT_TRANSCRIPT_MISMATCH,
    MDKR_MATCH_PREFLIGHT_GRAPH_MISMATCH,
    MDKR_MATCH_PREFLIGHT_ROUTE_UNAVAILABLE,
    MDKR_MATCH_PREFLIGHT_WAITING_FOR_PEERS,
    MDKR_MATCH_PREFLIGHT_ROM_UNVERIFIED,
    MDKR_MATCH_PREFLIGHT_VERIFY_PHRASE,
    MDKR_MATCH_PREFLIGHT_CHANNELS_NOT_READY,
    MDKR_MATCH_PREFLIGHT_READY
} MdkrMatchPreflightState;

typedef struct MdkrMatchPreflightStatus {
    MdkrMatchPreflightState state;
    /* Lowest authenticated endpoint id currently responsible for the state,
     * or zero for a room-wide route/validation failure. */
    uint64_t                endpoint_id;
    uint8_t                 received_count;
    uint8_t                 required_count;
} MdkrMatchPreflightStatus;

typedef struct MdkrMatchPreflightV1 {
    uint32_t           protocol_version;
    uint32_t           match_epoch;
    uint64_t           local_endpoint_id;
    uint32_t           local_connection_generation;
    MdkrMatchPeerGraph graph;
    uint8_t            descriptor_digest[MDKR_MATCH_PREFLIGHT_DIGEST_BYTES];
    uint8_t            transcript_digest[MDKR_MATCH_PREFLIGHT_DIGEST_BYTES];
    uint8_t            graph_digest[MDKR_MATCH_PREFLIGHT_DIGEST_BYTES];
    MdkrMatchPreflightAttestationV1
            attestations[MDKR_MATCH_PEER_GRAPH_MAX_ENDPOINTS];
    uint8_t present_mask;
} MdkrMatchPreflightV1;

/* SHA-256 of the canonical 148-byte descriptor. Output is unchanged on error. */
bool mdkr_match_preflight_descriptor_digest(
    const MdkrMatchLaunchDescriptorV1 *descriptor,
    uint8_t                            output[MDKR_MATCH_PREFLIGHT_DIGEST_BYTES]);

/* SHA-256 of an order-independent canonical graph encoding. Reachability bits
 * are remapped into ascending endpoint-id order, so equivalent topology
 * snapshots cannot disagree merely because their arrays use different orders.
 * Output is unchanged on error. */
bool mdkr_match_preflight_graph_digest(
    const MdkrMatchPeerGraph *graph,
    uint8_t                   output[MDKR_MATCH_PREFLIGHT_DIGEST_BYTES]);

/* Why a report was refused. A peer still speaking the 124-byte MPF1 format is
 * named rather than folded into the malformed case, so a version skew reads as
 * a version skew in a dump and can never be silently downgraded. */
typedef enum MdkrMatchPreflightDecodeReason {
    MDKR_MATCH_PREFLIGHT_DECODE_OK = 0,
    MDKR_MATCH_PREFLIGHT_DECODE_LEGACY_MPF1,
    MDKR_MATCH_PREFLIGHT_DECODE_UNKNOWN_FORMAT,
    MDKR_MATCH_PREFLIGHT_DECODE_LENGTH,
    MDKR_MATCH_PREFLIGHT_DECODE_MALFORMED
} MdkrMatchPreflightDecodeReason;

/* Fixed big-endian MPF2 report codec. Authentication and endpoint binding are
 * supplied by the pairwise carrier; the service must never originate these
 * bytes. Decode requires exactly 136 bytes and leaves output unchanged on any
 * malformed header, reserved bit or zero identity/generation/sequence. */
bool mdkr_match_preflight_attestation_encode(
    const MdkrMatchPreflightAttestationV1 *attestation,
    uint8_t                               *output,
    size_t                                 capacity);
bool mdkr_match_preflight_attestation_decode(
    const uint8_t                   *bytes,
    size_t                           length,
    MdkrMatchPreflightAttestationV1 *output);
/* The same decode, keeping the typed refusal instead of collapsing it. */
MdkrMatchPreflightDecodeReason mdkr_match_preflight_attestation_decode_reason(
    const uint8_t                   *bytes,
    size_t                           length,
    MdkrMatchPreflightAttestationV1 *output);
const char *mdkr_match_preflight_decode_reason_name(
    MdkrMatchPreflightDecodeReason reason);

/* The 136-byte report crosses the fixed 64-byte encrypted peer carrier as
 * three sequence-bound fragments on its reliable control path. Each returned
 * payload is sealed with payload type PREFLIGHT_FRAGMENT and a globally unique
 * AEAD sequence. Reassembly accepts reordering, exact duplicates and a newer
 * report replacing an incomplete older one; malformed/conflicting fragments
 * never change state or output. */
bool mdkr_match_preflight_fragment_encode(
    const MdkrMatchPreflightAttestationV1 *attestation,
    unsigned                               fragment_index,
    uint8_t payload[MDKR_MATCH_PEER_PAYLOAD_BYTES]);
bool mdkr_match_preflight_fragment_state_init(
    MdkrMatchPreflightFragmentState *state,
    const MdkrMatchPeerKeyContext   *authenticated_direction);
MdkrMatchPreflightFragmentResult mdkr_match_preflight_fragment_submit(
    MdkrMatchPreflightFragmentState       *state,
    const MdkrMatchPeerEnvelopeContext    *authenticated_context,
    const uint8_t payload[MDKR_MATCH_PEER_PAYLOAD_BYTES],
    MdkrMatchPreflightAttestationV1       *output);

/* Copies all immutable expectations. An inadmissible but structurally valid
 * graph is retained so evaluate() can present Route unavailable honestly. */
bool mdkr_match_preflight_init(
    MdkrMatchPreflightV1              *preflight,
    const MdkrMatchLaunchDescriptorV1 *descriptor,
    const MdkrMatchPeerGraph          *graph,
    const uint8_t                      transcript_digest[MDKR_MATCH_PREFLIGHT_DIGEST_BYTES],
    uint64_t                           local_endpoint_id,
    uint32_t                           local_connection_generation);

/* The caller must pass the identity authenticated by the carrier (or the
 * launcher's own trusted local identity). The embedded endpoint/generation
 * must match it exactly, so report bytes can never claim another member.
 * Higher sequences may advance local checks; exact retries are idempotent and
 * same-sequence changes conflict without mutation. */
MdkrMatchPreflightSubmitResult mdkr_match_preflight_submit(
    MdkrMatchPreflightV1                  *preflight,
    uint64_t                               authenticated_endpoint_id,
    uint32_t                               authenticated_connection_generation,
    const MdkrMatchPreflightAttestationV1 *attestation);

/* Pure status projection. READY is consensus evidence, not engine authority. */
MdkrMatchPreflightStatus mdkr_match_preflight_evaluate(
    const MdkrMatchPreflightV1 *preflight);

#ifdef __cplusplus
}
#endif
#endif
