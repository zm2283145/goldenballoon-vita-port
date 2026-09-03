#include "match_preflight.h"

#include <string.h>

#include "sha256.h"

static bool any_nonzero(const uint8_t *bytes, unsigned count) {
    unsigned index;
    uint8_t combined = 0u;
    for (index = 0u; index < count; index++)
        combined |= bytes[index];
    return combined != 0u;
}

static bool all_zero(const void *value, size_t count) {
    return !any_nonzero((const uint8_t *)value, (unsigned)count);
}

static void put32(uint8_t *output, uint32_t value) {
    output[0] = (uint8_t)(value >> 24u);
    output[1] = (uint8_t)(value >> 16u);
    output[2] = (uint8_t)(value >> 8u);
    output[3] = (uint8_t)value;
}

static void put16(uint8_t *output, uint16_t value) {
    output[0] = (uint8_t)(value >> 8u);
    output[1] = (uint8_t)value;
}

static void put64(uint8_t *output, uint64_t value) {
    unsigned index;
    for (index = 0u; index < 8u; index++)
        output[index] = (uint8_t)(value >> (56u - index * 8u));
}

static uint32_t get32(const uint8_t *input) {
    return (uint32_t)input[0] << 24u | (uint32_t)input[1] << 16u |
           (uint32_t)input[2] << 8u | (uint32_t)input[3];
}

static uint16_t get16(const uint8_t *input) {
    return (uint16_t)((uint16_t)input[0] << 8u | (uint16_t)input[1]);
}

static uint64_t get64(const uint8_t *input) {
    uint64_t value = 0u;
    unsigned index;
    for (index = 0u; index < 8u; index++)
        value = value << 8u | input[index];
    return value;
}

/* An attestation either carries a scored measurement and says so, or carries
 * an all-zero record. A band that disagrees with its own score is refused, so
 * a peer cannot show one number and band it as another. */
static bool measurement_consistent(
    uint8_t flags, const MdkrMatchRouteMeasurement *measurement) {
    MdkrMatchRouteMeasurement scored = *measurement;
    if ((flags & MDKR_MATCH_PREFLIGHT_ROUTE_MEASURED) == 0u)
        return all_zero(measurement, sizeof(*measurement));
    if (!mdkr_match_route_measurement_score(&scored)) return false;
    return scored.score == measurement->score &&
           scored.band == measurement->band;
}

static bool attestation_structural_valid(
    const MdkrMatchPreflightAttestationV1 *attestation) {
    return attestation != NULL &&
           measurement_consistent(attestation->flags,
                                  &attestation->measurement) &&
           attestation->protocol_version == MDKR_MATCH_PREFLIGHT_VERSION &&
           attestation->match_epoch != 0u &&
           attestation->connection_generation != 0u &&
           attestation->sequence != 0u && attestation->endpoint_id != 0u &&
           (attestation->flags & (uint8_t)~MDKR_MATCH_PREFLIGHT_FLAG_MASK) ==
               0u &&
           !any_nonzero(attestation->reserved, sizeof(attestation->reserved));
}

static bool key_context_valid(const MdkrMatchPeerKeyContext *context) {
    return context != NULL && context->match_epoch != 0u &&
           context->source_endpoint_id != 0u &&
           context->source_generation != 0u &&
           context->destination_endpoint_id != 0u &&
           context->destination_generation != 0u &&
           context->lane <= MDKR_MATCH_PEER_LANE_MAX &&
           context->source_endpoint_id != context->destination_endpoint_id;
}

static bool key_context_equal(const MdkrMatchPeerKeyContext *left,
                              const MdkrMatchPeerKeyContext *right) {
    return left->match_epoch == right->match_epoch &&
           left->source_endpoint_id == right->source_endpoint_id &&
           left->source_generation == right->source_generation &&
           left->destination_endpoint_id == right->destination_endpoint_id &&
           left->destination_generation == right->destination_generation &&
           left->lane == right->lane;
}

static bool fragment_context_valid(
    const MdkrMatchPeerEnvelopeContext *context) {
    return context != NULL && key_context_valid(&context->key) &&
           context->sequence != 0u &&
           context->payload_type ==
               MDKR_MATCH_PEER_PAYLOAD_PREFLIGHT_FRAGMENT &&
           (context->intermediate_endpoint_id == 0u ||
            (context->intermediate_endpoint_id !=
                 context->key.source_endpoint_id &&
             context->intermediate_endpoint_id !=
                 context->key.destination_endpoint_id));
}

static int endpoint_index(const MdkrMatchPeerGraph *graph,
                          uint64_t endpoint_id) {
    unsigned index;
    if (graph == NULL || endpoint_id == 0u)
        return -1;
    for (index = 0u; index < graph->endpoint_count; index++) {
        if (graph->endpoints[index].endpoint_id == endpoint_id)
            return (int)index;
    }
    return -1;
}

static bool graph_valid(const MdkrMatchPeerGraph *graph) {
    MdkrMatchPeerGraph copy;
    if (graph == NULL)
        return false;
    return mdkr_match_peer_graph_init(&copy, graph->match_epoch,
                                      graph->endpoints,
                                      graph->endpoint_count) &&
           memcmp(&copy, graph, sizeof(copy)) == 0;
}

static bool attestation_equal(const MdkrMatchPreflightAttestationV1 *left,
                              const MdkrMatchPreflightAttestationV1 *right) {
    return left->protocol_version == right->protocol_version &&
           left->match_epoch == right->match_epoch &&
           left->connection_generation == right->connection_generation &&
           left->sequence == right->sequence &&
           left->endpoint_id == right->endpoint_id &&
           left->flags == right->flags &&
           memcmp(left->reserved, right->reserved, sizeof(left->reserved)) ==
               0 &&
           memcmp(left->descriptor_digest, right->descriptor_digest,
                  sizeof(left->descriptor_digest)) == 0 &&
           memcmp(left->transcript_digest, right->transcript_digest,
                  sizeof(left->transcript_digest)) == 0 &&
           memcmp(left->graph_digest, right->graph_digest,
                  sizeof(left->graph_digest)) == 0 &&
           memcmp(&left->measurement, &right->measurement,
                  sizeof(left->measurement)) == 0;
}

static bool preflight_valid(const MdkrMatchPreflightV1 *preflight) {
    uint8_t canonical_graph_digest[MDKR_MATCH_PREFLIGHT_DIGEST_BYTES];
    int local;
    unsigned index;
    if (preflight == NULL ||
        preflight->protocol_version != MDKR_MATCH_PREFLIGHT_VERSION ||
        preflight->match_epoch == 0u ||
        preflight->match_epoch != preflight->graph.match_epoch ||
        !graph_valid(&preflight->graph) ||
        !any_nonzero(preflight->descriptor_digest,
                     sizeof(preflight->descriptor_digest)) ||
        !any_nonzero(preflight->transcript_digest,
                     sizeof(preflight->transcript_digest)) ||
        !any_nonzero(preflight->graph_digest,
                     sizeof(preflight->graph_digest)) ||
        (preflight->present_mask &
         (uint8_t)~((1u << preflight->graph.endpoint_count) - 1u)) != 0u)
        return false;
    if (!mdkr_match_preflight_graph_digest(&preflight->graph,
                                            canonical_graph_digest) ||
        memcmp(canonical_graph_digest, preflight->graph_digest,
               sizeof(canonical_graph_digest)) != 0)
        return false;
    for (index = 0u; index < preflight->graph.endpoint_count; index++) {
        const MdkrMatchPreflightAttestationV1 *attestation =
            &preflight->attestations[index];
        if ((preflight->present_mask & (uint8_t)(1u << index)) == 0u) {
            if (!all_zero(attestation, sizeof(*attestation)))
                return false;
            continue;
        }
        if (attestation->protocol_version != MDKR_MATCH_PREFLIGHT_VERSION ||
            attestation->match_epoch != preflight->match_epoch ||
            attestation->endpoint_id !=
                preflight->graph.endpoints[index].endpoint_id ||
            attestation->connection_generation !=
                preflight->graph.endpoints[index].generation ||
            attestation->sequence == 0u ||
            (attestation->flags & (uint8_t)~MDKR_MATCH_PREFLIGHT_FLAG_MASK) !=
                0u ||
            !measurement_consistent(attestation->flags,
                                    &attestation->measurement) ||
            any_nonzero(attestation->reserved, sizeof(attestation->reserved)))
            return false;
    }
    for (; index < MDKR_MATCH_PEER_GRAPH_MAX_ENDPOINTS; index++)
        if (!all_zero(&preflight->attestations[index],
                      sizeof(preflight->attestations[index])))
            return false;
    local = endpoint_index(&preflight->graph, preflight->local_endpoint_id);
    return local >= 0 && preflight->local_connection_generation != 0u &&
           preflight->graph.endpoints[local].generation ==
               preflight->local_connection_generation;
}

bool mdkr_match_preflight_descriptor_digest(
    const MdkrMatchLaunchDescriptorV1 *descriptor,
    uint8_t output[MDKR_MATCH_PREFLIGHT_DIGEST_BYTES]) {
    uint8_t encoded[MDKR_MATCH_LAUNCH_DESCRIPTOR_BYTES];
    uint8_t next[MDKR_MATCH_PREFLIGHT_DIGEST_BYTES];
    MdkrSha256 hash;
    if (output == NULL || !mdkr_match_launch_descriptor_encode(
                              descriptor, encoded, sizeof(encoded)))
        return false;
    mdkr_sha256_init(&hash);
    mdkr_sha256_update(&hash, encoded, sizeof(encoded));
    mdkr_sha256_final(&hash, next);
    memcpy(output, next, sizeof(next));
    return true;
}

bool mdkr_match_preflight_graph_digest(
    const MdkrMatchPeerGraph *graph,
    uint8_t output[MDKR_MATCH_PREFLIGHT_DIGEST_BYTES]) {
    static const char domain[] = "golden-balloon-match-peer-graph-v1";
    unsigned order[MDKR_MATCH_PEER_GRAPH_MAX_ENDPOINTS];
    uint8_t encoded[8];
    uint8_t next[MDKR_MATCH_PREFLIGHT_DIGEST_BYTES];
    MdkrSha256 hash;
    unsigned source;
    if (!graph_valid(graph) || output == NULL)
        return false;
    for (source = 0u; source < graph->endpoint_count; source++)
        order[source] = source;
    for (source = 0u; source < graph->endpoint_count; source++) {
        unsigned other;
        for (other = source + 1u; other < graph->endpoint_count; other++) {
            if (graph->endpoints[order[source]].endpoint_id >
                graph->endpoints[order[other]].endpoint_id) {
                unsigned swap = order[source];
                order[source] = order[other];
                order[other] = swap;
            }
        }
    }
    mdkr_sha256_init(&hash);
    mdkr_sha256_update(&hash, domain, sizeof(domain) - 1u);
    put32(encoded, graph->protocol_version);
    mdkr_sha256_update(&hash, encoded, 4u);
    put32(encoded, graph->match_epoch);
    mdkr_sha256_update(&hash, encoded, 4u);
    encoded[0] = graph->endpoint_count;
    mdkr_sha256_update(&hash, encoded, 1u);
    for (source = 0u; source < graph->endpoint_count; source++) {
        const unsigned original_source = order[source];
        const MdkrMatchPeerEndpoint *endpoint =
            &graph->endpoints[original_source];
        uint8_t canonical_mask = 0u;
        unsigned destination;
        put64(encoded, endpoint->endpoint_id);
        mdkr_sha256_update(&hash, encoded, 8u);
        put32(encoded, endpoint->generation);
        mdkr_sha256_update(&hash, encoded, 4u);
        for (destination = 0u; destination < graph->endpoint_count;
             destination++) {
            if ((endpoint->reachable_mask &
                 (uint8_t)(1u << order[destination])) != 0u)
                canonical_mask |= (uint8_t)(1u << destination);
        }
        mdkr_sha256_update(&hash, &canonical_mask, 1u);
    }
    mdkr_sha256_final(&hash, next);
    memcpy(output, next, sizeof(next));
    memset(next, 0, sizeof(next));
    return true;
}

/* ---- Route-quality measurement ------------------------------------------
 *
 * The score ladder, in one place. Each metric walks its rungs top to bottom
 * and takes the deduction of the first rung whose threshold it does not
 * exceed; a metric past every rung takes the row's worst deduction. The rungs
 * and their deductions are the table documented in
 * docs/ref/match-preflight-v1.md's v2 section. */
typedef struct RouteLadderRung {
    uint32_t threshold;
    uint8_t  deduction;
} RouteLadderRung;

static const RouteLadderRung route_rtt_rungs[] = {
    {40u, 0u}, {70u, 1u}, {110u, 2u}, {160u, 3u}, {220u, 4u}};
static const RouteLadderRung route_jitter_rungs[] = {
    {5u, 0u}, {12u, 1u}, {25u, 2u}, {45u, 3u}};
static const RouteLadderRung route_loss_rungs[] = {
    {5u, 0u}, {20u, 2u}, {50u, 4u}};
static const RouteLadderRung route_late_rungs[] = {
    {10u, 0u}, {50u, 1u}, {150u, 2u}};
static const uint8_t route_rtt_worst = 5u;
static const uint8_t route_jitter_worst = 4u;
static const uint8_t route_loss_worst = 6u;
static const uint8_t route_late_worst = 3u;
static const uint8_t route_undrained_deduction = 3u;
static const uint8_t route_perfect_score = 10u;
static const uint8_t route_steady_floor = 8u;
static const uint8_t route_uneven_floor = 5u;
static const uint16_t route_per_thousand_max = 1000u;

static uint8_t ladder_deduction(const RouteLadderRung *rungs, unsigned count,
                                uint8_t worst, uint32_t value) {
    unsigned index;
    for (index = 0u; index < count; index++)
        if (value <= rungs[index].threshold)
            return rungs[index].deduction;
    return worst;
}

MdkrMatchRouteBand mdkr_match_route_band(uint8_t score) {
    if (score > route_perfect_score || score == 0u)
        return MDKR_MATCH_ROUTE_BAND_NONE;
    if (score >= route_steady_floor)
        return MDKR_MATCH_ROUTE_BAND_STEADY;
    if (score >= route_uneven_floor)
        return MDKR_MATCH_ROUTE_BAND_UNEVEN;
    return MDKR_MATCH_ROUTE_BAND_ROUGH;
}

const char *mdkr_match_route_band_name(MdkrMatchRouteBand band) {
    switch (band) {
        case MDKR_MATCH_ROUTE_BAND_STEADY: return "steady";
        case MDKR_MATCH_ROUTE_BAND_UNEVEN: return "uneven";
        case MDKR_MATCH_ROUTE_BAND_ROUGH:  return "rough";
        case MDKR_MATCH_ROUTE_BAND_NONE:   break;
    }
    return NULL;
}

bool mdkr_match_route_measurement_score(
    MdkrMatchRouteMeasurement *measurement) {
    unsigned deducted;
    uint8_t score;
    if (measurement == NULL ||
        measurement->loss_per_thousand > route_per_thousand_max ||
        measurement->late_per_thousand > route_per_thousand_max)
        return false;
    deducted = ladder_deduction(route_rtt_rungs,
                                (unsigned)(sizeof(route_rtt_rungs) /
                                           sizeof(route_rtt_rungs[0])),
                                route_rtt_worst, measurement->p95_rtt_ms);
    deducted += ladder_deduction(route_jitter_rungs,
                                 (unsigned)(sizeof(route_jitter_rungs) /
                                            sizeof(route_jitter_rungs[0])),
                                 route_jitter_worst, measurement->jitter_ms);
    deducted += ladder_deduction(route_loss_rungs,
                                 (unsigned)(sizeof(route_loss_rungs) /
                                            sizeof(route_loss_rungs[0])),
                                 route_loss_worst,
                                 measurement->loss_per_thousand);
    deducted += ladder_deduction(route_late_rungs,
                                 (unsigned)(sizeof(route_late_rungs) /
                                            sizeof(route_late_rungs[0])),
                                 route_late_worst,
                                 measurement->late_per_thousand);
    if (measurement->undrained != 0u) deducted += route_undrained_deduction;
    score = deducted >= route_perfect_score
                ? 1u
                : (uint8_t)(route_perfect_score - deducted);
    measurement->score = score;
    measurement->band = (uint8_t)mdkr_match_route_band(score);
    return true;
}

uint8_t mdkr_match_route_input_delay(uint16_t p95_rtt_ms, uint32_t tick_ms,
                                     uint8_t manifest_input_delay) {
    uint32_t needed;
    uint32_t headroom;
    if (tick_ms == 0u ||
        manifest_input_delay >= MDKR_MATCH_ROUTE_INPUT_DELAY_CAP)
        return manifest_input_delay;
    needed = ((uint32_t)p95_rtt_ms + tick_ms - 1u) / tick_ms;
    if (needed <= manifest_input_delay) return manifest_input_delay;
    headroom = (uint32_t)MDKR_MATCH_ROUTE_INPUT_DELAY_CAP -
               manifest_input_delay;
    needed -= manifest_input_delay;
    if (needed > headroom) needed = headroom;
    return (uint8_t)(manifest_input_delay + needed);
}

/* Probe payload: a fixed magic, the lane and kind, the sequence the sender
 * matches its echo against, and the origin id so a broadcast echo is only
 * counted by the endpoint that asked for it. The filler carries the payload to
 * the lane's real size and must be zero, so no other traffic can decode here. */
#define MDKR_MATCH_ROUTE_PROBE_HEADER_BYTES 18u

bool mdkr_match_route_probe_encode(
    const MdkrMatchRouteProbe *probe,
    uint8_t payload[MDKR_MATCH_PEER_PAYLOAD_BYTES]) {
    uint8_t next[MDKR_MATCH_PEER_PAYLOAD_BYTES];
    if (probe == NULL || payload == NULL || probe->sequence == 0u ||
        probe->origin_endpoint_id == 0u ||
        probe->lane >= MDKR_MATCH_ROUTE_LANE_COUNT ||
        probe->kind > MDKR_MATCH_ROUTE_ECHO)
        return false;
    memset(next, 0, sizeof(next));
    next[0] = 'M';
    next[1] = 'R';
    next[2] = 'Q';
    next[3] = '1';
    next[4] = probe->lane;
    next[5] = probe->kind;
    put32(next + 6u, probe->sequence);
    put64(next + 10u, probe->origin_endpoint_id);
    memcpy(payload, next, sizeof(next));
    return true;
}

bool mdkr_match_route_probe_decode(
    const uint8_t payload[MDKR_MATCH_PEER_PAYLOAD_BYTES],
    MdkrMatchRouteProbe *output) {
    MdkrMatchRouteProbe next;
    if (payload == NULL || output == NULL || payload[0] != 'M' ||
        payload[1] != 'R' || payload[2] != 'Q' || payload[3] != '1' ||
        payload[4] >= MDKR_MATCH_ROUTE_LANE_COUNT ||
        payload[5] > MDKR_MATCH_ROUTE_ECHO ||
        any_nonzero(payload + MDKR_MATCH_ROUTE_PROBE_HEADER_BYTES,
                    MDKR_MATCH_PEER_PAYLOAD_BYTES -
                        MDKR_MATCH_ROUTE_PROBE_HEADER_BYTES))
        return false;
    memset(&next, 0, sizeof(next));
    next.lane = payload[4];
    next.kind = payload[5];
    next.sequence = get32(payload + 6u);
    next.origin_endpoint_id = get64(payload + 10u);
    if (next.sequence == 0u || next.origin_endpoint_id == 0u) return false;
    *output = next;
    return true;
}

static uint32_t route_lane_cadence_ms(
    const MdkrMatchRouteMeasureState *state, unsigned lane) {
    return lane == (unsigned)MDKR_MATCH_ROUTE_LANE_BUNDLE
               ? state->tick_ms
               : MDKR_MATCH_ROUTE_CONTROL_CADENCE_MS;
}

static bool route_measure_valid(const MdkrMatchRouteMeasureState *state) {
    return state != NULL && state->tick_ms != 0u &&
           state->origin_endpoint_id != 0u &&
           state->next_sequence >= 1u &&
           state->next_sequence <= MDKR_MATCH_ROUTE_MAX_PROBES + 1u;
}

bool mdkr_match_route_measure_begin(MdkrMatchRouteMeasureState *state,
                                    uint32_t now_ms, uint32_t tick_ms,
                                    uint64_t origin_endpoint_id) {
    MdkrMatchRouteMeasureState next;
    unsigned lane;
    if (state == NULL || tick_ms == 0u || origin_endpoint_id == 0u)
        return false;
    memset(&next, 0, sizeof(next));
    next.origin_endpoint_id = origin_endpoint_id;
    next.begin_ms = now_ms;
    next.tick_ms = tick_ms;
    next.next_sequence = 1u;
    for (lane = 0u; lane < (unsigned)MDKR_MATCH_ROUTE_LANE_COUNT; lane++)
        next.next_send_ms[lane] = now_ms;
    *state = next;
    return true;
}

bool mdkr_match_route_measure_due(MdkrMatchRouteMeasureState *state,
                                  uint32_t now_ms,
                                  MdkrMatchRouteProbe *output) {
    unsigned lane;
    if (!route_measure_valid(state) || output == NULL) return false;
    for (lane = 0u; lane < (unsigned)MDKR_MATCH_ROUTE_LANE_COUNT; lane++) {
        const uint32_t due_ms = state->next_send_ms[lane];
        uint32_t slot;
        /* Emission is scheduled off the cadence, not off now_ms, so a coarse
         * pump neither drops nor duplicates a lane's slots: it replays the
         * ones it owes on the next call and stops at the window's edge. */
        if (due_ms - state->begin_ms >= MDKR_MATCH_ROUTE_MEASURE_MS ||
            now_ms < due_ms)
            continue;
        state->next_send_ms[lane] =
            due_ms + route_lane_cadence_ms(state, lane);
        /* The table holds both lanes' whole windows with room to spare; a full
         * one can only mean a corrupted state, so stop emitting on it. */
        if (state->next_sequence > MDKR_MATCH_ROUTE_MAX_PROBES) continue;
        slot = state->next_sequence - 1u;
        state->sent_ms[slot] = now_ms;
        state->lane[slot] = (uint8_t)lane;
        output->origin_endpoint_id = state->origin_endpoint_id;
        output->sequence = state->next_sequence;
        output->lane = (uint8_t)lane;
        output->kind = (uint8_t)MDKR_MATCH_ROUTE_PROBE;
        state->next_sequence++;
        return true;
    }
    return false;
}

void mdkr_match_route_measure_echo(MdkrMatchRouteMeasureState *state,
                                   uint32_t sequence, uint32_t now_ms) {
    uint32_t slot;
    uint32_t elapsed;
    if (!route_measure_valid(state) || sequence == 0u ||
        sequence >= state->next_sequence)
        return;
    slot = sequence - 1u;
    if (state->echoed[slot] != 0u) return;
    elapsed = now_ms - state->sent_ms[slot];
    state->rtt_ms[slot] = elapsed > UINT16_MAX ? UINT16_MAX : (uint16_t)elapsed;
    state->echoed[slot] = 1u;
}

/* 95th percentile of the round trips answered so far, zero with none. The
 * cut's margin scales with it, so a slow route is not charged for its own
 * round trip. Separate from finish()'s reduction on purpose: this one runs
 * mid-window, over the samples that exist at the cut. */
static uint16_t route_measure_p95(const MdkrMatchRouteMeasureState *state) {
    uint16_t sorted[MDKR_MATCH_ROUTE_MAX_PROBES];
    unsigned answered = 0u;
    unsigned sent;
    unsigned index;
    unsigned rank;
    sent = state->next_sequence - 1u;
    for (index = 0u; index < sent; index++) {
        if (state->echoed[index] == 0u) continue;
        sorted[answered++] = state->rtt_ms[index];
    }
    if (answered == 0u) return 0u;
    for (index = 1u; index < answered; index++) {
        const uint16_t value = sorted[index];
        unsigned position = index;
        while (position > 0u && sorted[position - 1u] > value) {
            sorted[position] = sorted[position - 1u];
            position--;
        }
        sorted[position] = value;
    }
    rank = (answered * 95u + 99u) / 100u;
    return sorted[rank - 1u];
}

unsigned mdkr_match_route_measure_cut(MdkrMatchRouteMeasureState *state,
                                      uint32_t now_ms) {
    unsigned lane;
    unsigned sent;
    unsigned dropped = 0u;
    uint32_t margin;
    uint32_t twice_p95;
    if (!route_measure_valid(state)) return 0u;
    sent = state->next_sequence - 1u;
    twice_p95 = (uint32_t)route_measure_p95(state) * 2u;
    margin = twice_p95 > MDKR_MATCH_ROUTE_CUT_MARGIN_MS
                 ? twice_p95
                 : MDKR_MATCH_ROUTE_CUT_MARGIN_MS;
    /* Only the TRAILING run: the scan stops at the first probe that was
     * answered, or that has been unanswered for longer than the margin, so
     * real loss inside the measured stretch is never erased. */
    while (sent > 0u && state->echoed[sent - 1u] == 0u &&
           now_ms - state->sent_ms[sent - 1u] < margin) {
        sent--;
        dropped++;
    }
    state->next_sequence = sent + 1u;
    /* Park both lanes at the window's edge: due() emits nothing past it, so a
     * caller that keeps pumping cannot reopen a cut window. */
    for (lane = 0u; lane < (unsigned)MDKR_MATCH_ROUTE_LANE_COUNT; lane++)
        state->next_send_ms[lane] =
            state->begin_ms + MDKR_MATCH_ROUTE_MEASURE_MS;
    return dropped;
}

bool mdkr_match_route_measure_adoptable(
    const MdkrMatchRouteMeasureState *state) {
    unsigned answered = 0u;
    unsigned sent;
    unsigned index;
    if (!route_measure_valid(state)) return false;
    sent = state->next_sequence - 1u;
    if (sent == 0u) return false;
    for (index = 0u; index < sent; index++)
        if (state->echoed[index] != 0u) answered++;
    if (answered < MDKR_MATCH_ROUTE_CUT_MIN_SAMPLES) return false;
    /* The span the surviving samples actually cover, measured from the
     * window's own start to the last probe still in the set. */
    return state->sent_ms[sent - 1u] - state->begin_ms >=
           MDKR_MATCH_ROUTE_CUT_MIN_SPAN_MS;
}

bool mdkr_match_route_measure_settled(const MdkrMatchRouteMeasureState *state,
                                      uint32_t now_ms) {
    if (!route_measure_valid(state)) return true;
    return now_ms - state->begin_ms >=
           MDKR_MATCH_ROUTE_MEASURE_MS + MDKR_MATCH_ROUTE_DRAIN_MS;
}

bool mdkr_match_route_measure_finish(const MdkrMatchRouteMeasureState *state,
                                     uint32_t undrained,
                                     MdkrMatchRouteMeasurement *output) {
    MdkrMatchRouteMeasurement next;
    uint16_t sorted[MDKR_MATCH_ROUTE_MAX_PROBES];
    unsigned sent;
    unsigned answered = 0u;
    unsigned late = 0u;
    unsigned gaps = 0u;
    uint32_t deviation = 0u;
    unsigned have_previous = 0u;
    uint16_t previous = 0u;
    unsigned index;
    if (!route_measure_valid(state) || output == NULL) return false;
    sent = state->next_sequence - 1u;
    if (sent == 0u) return false;
    for (index = 0u; index < sent; index++) {
        uint16_t rtt;
        if (state->echoed[index] == 0u) continue;
        rtt = state->rtt_ms[index];
        sorted[answered++] = rtt;
        if (rtt > MDKR_MATCH_ROUTE_LATE_SAMPLE_MS) late++;
        if (have_previous != 0u) {
            deviation += rtt > previous ? (uint32_t)(rtt - previous)
                                        : (uint32_t)(previous - rtt);
            gaps++;
        }
        previous = rtt;
        have_previous = 1u;
    }
    for (index = 1u; index < answered; index++) {
        const uint16_t value = sorted[index];
        unsigned position = index;
        while (position > 0u && sorted[position - 1u] > value) {
            sorted[position] = sorted[position - 1u];
            position--;
        }
        sorted[position] = value;
    }
    memset(&next, 0, sizeof(next));
    if (answered != 0u) {
        const unsigned rank = (answered * 95u + 99u) / 100u;
        next.p95_rtt_ms = sorted[rank - 1u];
        next.late_per_thousand =
            (uint16_t)((uint32_t)late * route_per_thousand_max / answered);
    }
    if (gaps != 0u)
        next.jitter_ms = (uint16_t)(deviation / gaps);
    next.loss_per_thousand = (uint16_t)((uint32_t)(sent - answered) *
                                        route_per_thousand_max / sent);
    next.undrained =
        undrained > UINT16_MAX ? UINT16_MAX : (uint16_t)undrained;
    if (!mdkr_match_route_measurement_score(&next)) return false;
    *output = next;
    return true;
}

bool mdkr_match_preflight_attestation_encode(
    const MdkrMatchPreflightAttestationV1 *attestation, uint8_t *output,
    size_t capacity) {
    uint8_t next[MDKR_MATCH_PREFLIGHT_ATTESTATION_BYTES];
    if (!attestation_structural_valid(attestation) || output == NULL ||
        capacity < sizeof(next))
        return false;
    memset(next, 0, sizeof(next));
    next[0] = 'M';
    next[1] = 'P';
    next[2] = 'F';
    next[3] = '2';
    next[4] = MDKR_MATCH_PREFLIGHT_VERSION;
    next[5] = attestation->flags;
    put32(next + 8u, attestation->match_epoch);
    put32(next + 12u, attestation->connection_generation);
    put32(next + 16u, attestation->sequence);
    put64(next + 20u, attestation->endpoint_id);
    memcpy(next + 28u, attestation->descriptor_digest,
           sizeof(attestation->descriptor_digest));
    memcpy(next + 60u, attestation->transcript_digest,
           sizeof(attestation->transcript_digest));
    memcpy(next + 92u, attestation->graph_digest,
           sizeof(attestation->graph_digest));
    put16(next + MDKR_MATCH_PREFLIGHT_MEASUREMENT_OFFSET,
          attestation->measurement.p95_rtt_ms);
    put16(next + MDKR_MATCH_PREFLIGHT_MEASUREMENT_OFFSET + 2u,
          attestation->measurement.jitter_ms);
    put16(next + MDKR_MATCH_PREFLIGHT_MEASUREMENT_OFFSET + 4u,
          attestation->measurement.loss_per_thousand);
    put16(next + MDKR_MATCH_PREFLIGHT_MEASUREMENT_OFFSET + 6u,
          attestation->measurement.late_per_thousand);
    put16(next + MDKR_MATCH_PREFLIGHT_MEASUREMENT_OFFSET + 8u,
          attestation->measurement.undrained);
    next[MDKR_MATCH_PREFLIGHT_MEASUREMENT_OFFSET + 10u] =
        attestation->measurement.score;
    next[MDKR_MATCH_PREFLIGHT_MEASUREMENT_OFFSET + 11u] =
        attestation->measurement.band;
    memcpy(output, next, sizeof(next));
    return true;
}

const char *mdkr_match_preflight_decode_reason_name(
    MdkrMatchPreflightDecodeReason reason) {
    switch (reason) {
        case MDKR_MATCH_PREFLIGHT_DECODE_OK:             return "ok";
        case MDKR_MATCH_PREFLIGHT_DECODE_LEGACY_MPF1:    return "legacy_mpf1";
        case MDKR_MATCH_PREFLIGHT_DECODE_UNKNOWN_FORMAT: return "unknown_format";
        case MDKR_MATCH_PREFLIGHT_DECODE_LENGTH:         return "length";
        case MDKR_MATCH_PREFLIGHT_DECODE_MALFORMED:      break;
    }
    return "malformed";
}

MdkrMatchPreflightDecodeReason mdkr_match_preflight_attestation_decode_reason(
    const uint8_t *bytes, size_t length,
    MdkrMatchPreflightAttestationV1 *output) {
    MdkrMatchPreflightAttestationV1 next;
    if (bytes == NULL || output == NULL)
        return MDKR_MATCH_PREFLIGHT_DECODE_MALFORMED;
    /* The format tag is read before the length so a peer still sending the
     * 124-byte MPF1 report is named rather than reported as a short buffer. */
    if (length >= 4u && bytes[0] == 'M' && bytes[1] == 'P' && bytes[2] == 'F' &&
        bytes[3] == '1')
        return MDKR_MATCH_PREFLIGHT_DECODE_LEGACY_MPF1;
    if (length != MDKR_MATCH_PREFLIGHT_ATTESTATION_BYTES)
        return MDKR_MATCH_PREFLIGHT_DECODE_LENGTH;
    if (bytes[0] != 'M' || bytes[1] != 'P' || bytes[2] != 'F' ||
        bytes[3] != '2' || bytes[4] != MDKR_MATCH_PREFLIGHT_VERSION)
        return MDKR_MATCH_PREFLIGHT_DECODE_UNKNOWN_FORMAT;
    if (bytes[6] != 0u || bytes[7] != 0u)
        return MDKR_MATCH_PREFLIGHT_DECODE_MALFORMED;
    memset(&next, 0, sizeof(next));
    next.protocol_version = bytes[4];
    next.flags = bytes[5];
    next.match_epoch = get32(bytes + 8u);
    next.connection_generation = get32(bytes + 12u);
    next.sequence = get32(bytes + 16u);
    next.endpoint_id = get64(bytes + 20u);
    memcpy(next.descriptor_digest, bytes + 28u, sizeof(next.descriptor_digest));
    memcpy(next.transcript_digest, bytes + 60u, sizeof(next.transcript_digest));
    memcpy(next.graph_digest, bytes + 92u, sizeof(next.graph_digest));
    next.measurement.p95_rtt_ms =
        get16(bytes + MDKR_MATCH_PREFLIGHT_MEASUREMENT_OFFSET);
    next.measurement.jitter_ms =
        get16(bytes + MDKR_MATCH_PREFLIGHT_MEASUREMENT_OFFSET + 2u);
    next.measurement.loss_per_thousand =
        get16(bytes + MDKR_MATCH_PREFLIGHT_MEASUREMENT_OFFSET + 4u);
    next.measurement.late_per_thousand =
        get16(bytes + MDKR_MATCH_PREFLIGHT_MEASUREMENT_OFFSET + 6u);
    next.measurement.undrained =
        get16(bytes + MDKR_MATCH_PREFLIGHT_MEASUREMENT_OFFSET + 8u);
    next.measurement.score = bytes[MDKR_MATCH_PREFLIGHT_MEASUREMENT_OFFSET + 10u];
    next.measurement.band = bytes[MDKR_MATCH_PREFLIGHT_MEASUREMENT_OFFSET + 11u];
    if (!attestation_structural_valid(&next))
        return MDKR_MATCH_PREFLIGHT_DECODE_MALFORMED;
    *output = next;
    return MDKR_MATCH_PREFLIGHT_DECODE_OK;
}

bool mdkr_match_preflight_attestation_decode(
    const uint8_t *bytes, size_t length,
    MdkrMatchPreflightAttestationV1 *output) {
    return mdkr_match_preflight_attestation_decode_reason(bytes, length,
                                                          output) ==
           MDKR_MATCH_PREFLIGHT_DECODE_OK;
}

static size_t fragment_data_size(unsigned fragment_index) {
    const size_t offset =
        (size_t)fragment_index * MDKR_MATCH_PREFLIGHT_FRAGMENT_DATA_BYTES;
    const size_t remaining = MDKR_MATCH_PREFLIGHT_ATTESTATION_BYTES - offset;
    return remaining < MDKR_MATCH_PREFLIGHT_FRAGMENT_DATA_BYTES
               ? remaining
               : MDKR_MATCH_PREFLIGHT_FRAGMENT_DATA_BYTES;
}

static bool fragment_state_valid(
    const MdkrMatchPreflightFragmentState *state) {
    unsigned index;
    if (state == NULL || !key_context_valid(&state->direction) ||
        (state->present_mask &
         (uint8_t)~((1u << MDKR_MATCH_PREFLIGHT_FRAGMENT_COUNT) - 1u)) != 0u)
        return false;
    if (state->sequence == 0u)
        return state->present_mask == 0u &&
               all_zero(state->encoded, sizeof(state->encoded));
    if (state->present_mask == 0u)
        return false;
    for (index = 0u; index < MDKR_MATCH_PREFLIGHT_FRAGMENT_COUNT; index++) {
        const size_t offset =
            (size_t)index * MDKR_MATCH_PREFLIGHT_FRAGMENT_DATA_BYTES;
        if ((state->present_mask & (uint8_t)(1u << index)) == 0u &&
            !all_zero(state->encoded + offset, fragment_data_size(index)))
            return false;
    }
    if (state->present_mask ==
        (uint8_t)((1u << MDKR_MATCH_PREFLIGHT_FRAGMENT_COUNT) - 1u)) {
        MdkrMatchPreflightAttestationV1 decoded;
        if (!mdkr_match_preflight_attestation_decode(
                state->encoded, sizeof(state->encoded), &decoded) ||
            decoded.sequence != state->sequence)
            return false;
    }
    return true;
}

bool mdkr_match_preflight_fragment_encode(
    const MdkrMatchPreflightAttestationV1 *attestation,
    unsigned fragment_index,
    uint8_t payload[MDKR_MATCH_PEER_PAYLOAD_BYTES]) {
    uint8_t encoded[MDKR_MATCH_PREFLIGHT_ATTESTATION_BYTES];
    uint8_t next[MDKR_MATCH_PEER_PAYLOAD_BYTES];
    const size_t offset =
        (size_t)fragment_index * MDKR_MATCH_PREFLIGHT_FRAGMENT_DATA_BYTES;
    size_t count;
    if (fragment_index >= MDKR_MATCH_PREFLIGHT_FRAGMENT_COUNT ||
        payload == NULL ||
        !mdkr_match_preflight_attestation_encode(attestation, encoded,
                                                  sizeof(encoded)))
        return false;
    count = fragment_data_size(fragment_index);
    memset(next, 0, sizeof(next));
    put32(next, attestation->sequence);
    next[4] = (uint8_t)fragment_index;
    next[5] = MDKR_MATCH_PREFLIGHT_FRAGMENT_COUNT;
    memcpy(next + MDKR_MATCH_PREFLIGHT_FRAGMENT_HEADER_BYTES,
           encoded + offset, count);
    memcpy(payload, next, sizeof(next));
    return true;
}

bool mdkr_match_preflight_fragment_state_init(
    MdkrMatchPreflightFragmentState *state,
    const MdkrMatchPeerKeyContext *authenticated_direction) {
    MdkrMatchPreflightFragmentState next;
    if (state == NULL || !key_context_valid(authenticated_direction))
        return false;
    memset(&next, 0, sizeof(next));
    /* Whole-struct: the direction is one identity, and copying it field by
     * field silently drops any field the context later grows. */
    next.direction = *authenticated_direction;
    *state = next;
    return true;
}

MdkrMatchPreflightFragmentResult mdkr_match_preflight_fragment_submit(
    MdkrMatchPreflightFragmentState *state,
    const MdkrMatchPeerEnvelopeContext *authenticated_context,
    const uint8_t payload[MDKR_MATCH_PEER_PAYLOAD_BYTES],
    MdkrMatchPreflightAttestationV1 *output) {
    MdkrMatchPreflightFragmentState next;
    MdkrMatchPreflightAttestationV1 decoded;
    uint32_t sequence;
    unsigned index;
    size_t offset;
    size_t count;
    if (!fragment_state_valid(state) ||
        !fragment_context_valid(authenticated_context) || payload == NULL ||
        output == NULL)
        return MDKR_MATCH_PREFLIGHT_FRAGMENT_INVALID;
    if (!key_context_equal(&state->direction, &authenticated_context->key))
        return MDKR_MATCH_PREFLIGHT_FRAGMENT_CONTEXT_MISMATCH;
    sequence = get32(payload);
    index = payload[4];
    if (sequence == 0u || index >= MDKR_MATCH_PREFLIGHT_FRAGMENT_COUNT ||
        payload[5] != MDKR_MATCH_PREFLIGHT_FRAGMENT_COUNT)
        return MDKR_MATCH_PREFLIGHT_FRAGMENT_INVALID;
    offset = (size_t)index * MDKR_MATCH_PREFLIGHT_FRAGMENT_DATA_BYTES;
    count = fragment_data_size(index);
    if (!all_zero(payload + MDKR_MATCH_PREFLIGHT_FRAGMENT_HEADER_BYTES + count,
                  MDKR_MATCH_PEER_PAYLOAD_BYTES -
                      MDKR_MATCH_PREFLIGHT_FRAGMENT_HEADER_BYTES - count))
        return MDKR_MATCH_PREFLIGHT_FRAGMENT_INVALID;
    if (state->sequence != 0u && sequence < state->sequence)
        return MDKR_MATCH_PREFLIGHT_FRAGMENT_STALE_SEQUENCE;
    next = *state;
    if (sequence > next.sequence) {
        memset(&next, 0, sizeof(next));
        next.direction = state->direction;
        next.sequence = sequence;
    }
    if ((next.present_mask & (uint8_t)(1u << index)) != 0u) {
        return memcmp(next.encoded + offset,
                      payload + MDKR_MATCH_PREFLIGHT_FRAGMENT_HEADER_BYTES,
                      count) == 0
                   ? MDKR_MATCH_PREFLIGHT_FRAGMENT_DUPLICATE
                   : MDKR_MATCH_PREFLIGHT_FRAGMENT_CONFLICT;
    }
    memcpy(next.encoded + offset,
           payload + MDKR_MATCH_PREFLIGHT_FRAGMENT_HEADER_BYTES, count);
    next.present_mask |= (uint8_t)(1u << index);
    if (next.present_mask !=
        (uint8_t)((1u << MDKR_MATCH_PREFLIGHT_FRAGMENT_COUNT) - 1u)) {
        *state = next;
        return MDKR_MATCH_PREFLIGHT_FRAGMENT_ACCEPTED;
    }
    if (!mdkr_match_preflight_attestation_decode(
            next.encoded, sizeof(next.encoded), &decoded) ||
        decoded.sequence != sequence)
        return MDKR_MATCH_PREFLIGHT_FRAGMENT_INVALID;
    if (decoded.match_epoch != state->direction.match_epoch ||
        decoded.endpoint_id != state->direction.source_endpoint_id ||
        decoded.connection_generation != state->direction.source_generation)
        return MDKR_MATCH_PREFLIGHT_FRAGMENT_CONTEXT_MISMATCH;
    *state = next;
    *output = decoded;
    return MDKR_MATCH_PREFLIGHT_FRAGMENT_COMPLETE;
}

bool mdkr_match_preflight_init(
    MdkrMatchPreflightV1 *preflight,
    const MdkrMatchLaunchDescriptorV1 *descriptor,
    const MdkrMatchPeerGraph *graph,
    const uint8_t transcript_digest[MDKR_MATCH_PREFLIGHT_DIGEST_BYTES],
    uint64_t local_endpoint_id, uint32_t local_connection_generation) {
    MdkrMatchPreflightV1 next;
    int local;
    if (preflight == NULL || descriptor == NULL || !graph_valid(graph) ||
        transcript_digest == NULL ||
        !any_nonzero(transcript_digest, MDKR_MATCH_PREFLIGHT_DIGEST_BYTES) ||
        descriptor->manifest.match_epoch != graph->match_epoch)
        return false;
    local = endpoint_index(graph, local_endpoint_id);
    if (local < 0 || local_connection_generation == 0u ||
        graph->endpoints[local].generation != local_connection_generation)
        return false;
    memset(&next, 0, sizeof(next));
    next.protocol_version = MDKR_MATCH_PREFLIGHT_VERSION;
    next.match_epoch = graph->match_epoch;
    next.local_endpoint_id = local_endpoint_id;
    next.local_connection_generation = local_connection_generation;
    next.graph = *graph;
    memcpy(next.transcript_digest, transcript_digest,
           sizeof(next.transcript_digest));
    if (!mdkr_match_preflight_graph_digest(graph, next.graph_digest))
        return false;
    if (!mdkr_match_preflight_descriptor_digest(descriptor,
                                                next.descriptor_digest))
        return false;
    *preflight = next;
    return true;
}

MdkrMatchPreflightSubmitResult mdkr_match_preflight_submit(
    MdkrMatchPreflightV1 *preflight,
    uint64_t authenticated_endpoint_id,
    uint32_t authenticated_connection_generation,
    const MdkrMatchPreflightAttestationV1 *attestation) {
    int index;
    MdkrMatchPreflightAttestationV1 *current;
    if (!preflight_valid(preflight) || authenticated_endpoint_id == 0u ||
        authenticated_connection_generation == 0u ||
        !attestation_structural_valid(attestation))
        return MDKR_MATCH_PREFLIGHT_SUBMIT_INVALID;
    if (attestation->endpoint_id != authenticated_endpoint_id ||
        attestation->connection_generation !=
            authenticated_connection_generation)
        return MDKR_MATCH_PREFLIGHT_SUBMIT_AUTHENTICATED_SOURCE_MISMATCH;
    if (attestation->match_epoch != preflight->match_epoch)
        return MDKR_MATCH_PREFLIGHT_SUBMIT_STALE_EPOCH;
    index = endpoint_index(&preflight->graph, attestation->endpoint_id);
    if (index < 0)
        return MDKR_MATCH_PREFLIGHT_SUBMIT_UNKNOWN_ENDPOINT;
    if (attestation->connection_generation !=
        preflight->graph.endpoints[index].generation)
        return MDKR_MATCH_PREFLIGHT_SUBMIT_STALE_GENERATION;
    current = &preflight->attestations[index];
    if ((preflight->present_mask & (uint8_t)(1u << index)) != 0u) {
        if (attestation->sequence < current->sequence)
            return MDKR_MATCH_PREFLIGHT_SUBMIT_STALE_SEQUENCE;
        if (attestation->sequence == current->sequence)
            return attestation_equal(current, attestation)
                       ? MDKR_MATCH_PREFLIGHT_SUBMIT_DUPLICATE
                       : MDKR_MATCH_PREFLIGHT_SUBMIT_CONFLICT;
    }
    *current = *attestation;
    preflight->present_mask |= (uint8_t)(1u << index);
    return MDKR_MATCH_PREFLIGHT_SUBMIT_ACCEPTED;
}

static MdkrMatchPreflightStatus status(const MdkrMatchPreflightV1 *preflight,
                                       MdkrMatchPreflightState state,
                                       uint64_t endpoint_id) {
    MdkrMatchPreflightStatus result = {state, endpoint_id, 0u, 0u};
    unsigned index;
    if (preflight == NULL ||
        preflight->graph.endpoint_count < 2u ||
        preflight->graph.endpoint_count > MDKR_MATCH_PEER_GRAPH_MAX_ENDPOINTS)
        return result;
    result.required_count = preflight->graph.endpoint_count;
    for (index = 0u; index < preflight->graph.endpoint_count; index++) {
        if ((preflight->present_mask & (uint8_t)(1u << index)) != 0u)
            result.received_count++;
    }
    return result;
}

static uint64_t lowest_endpoint_for(const MdkrMatchPreflightV1 *preflight,
                                    int condition) {
    uint64_t lowest = 0u;
    unsigned index;
    for (index = 0u; index < preflight->graph.endpoint_count; index++) {
        const MdkrMatchPreflightAttestationV1 *attestation =
            &preflight->attestations[index];
        uint64_t candidate;
        bool present = (preflight->present_mask & (uint8_t)(1u << index)) != 0u;
        bool matches = false;
        if (condition == 0) {
            matches = !present;
        } else if (present && condition == 1) {
            matches = memcmp(attestation->descriptor_digest,
                             preflight->descriptor_digest,
                             sizeof(preflight->descriptor_digest)) != 0;
        } else if (present && condition == 2) {
            matches = memcmp(attestation->transcript_digest,
                             preflight->transcript_digest,
                             sizeof(preflight->transcript_digest)) != 0;
        } else if (present && condition == 3) {
            matches = memcmp(attestation->graph_digest,
                             preflight->graph_digest,
                             sizeof(preflight->graph_digest)) != 0;
        } else if (present && condition >= 4) {
            matches =
                (attestation->flags & (uint8_t)(1u << (condition - 4))) == 0u;
        }
        candidate = present ? attestation->endpoint_id
                            : preflight->graph.endpoints[index].endpoint_id;
        if (matches && (lowest == 0u || candidate < lowest))
            lowest = candidate;
    }
    return lowest;
}

MdkrMatchPreflightStatus
mdkr_match_preflight_evaluate(const MdkrMatchPreflightV1 *preflight) {
    uint64_t endpoint;
    if (!preflight_valid(preflight))
        return status(preflight, MDKR_MATCH_PREFLIGHT_INVALID, 0u);
    endpoint = lowest_endpoint_for(preflight, 1);
    if (endpoint != 0u)
        return status(preflight, MDKR_MATCH_PREFLIGHT_DESCRIPTOR_MISMATCH,
                      endpoint);
    endpoint = lowest_endpoint_for(preflight, 2);
    if (endpoint != 0u)
        return status(preflight, MDKR_MATCH_PREFLIGHT_TRANSCRIPT_MISMATCH,
                      endpoint);
    endpoint = lowest_endpoint_for(preflight, 3);
    if (endpoint != 0u)
        return status(preflight, MDKR_MATCH_PREFLIGHT_GRAPH_MISMATCH, endpoint);
    if (!mdkr_match_peer_graph_admissible(&preflight->graph))
        return status(preflight, MDKR_MATCH_PREFLIGHT_ROUTE_UNAVAILABLE, 0u);
    endpoint = lowest_endpoint_for(preflight, 0);
    if (endpoint != 0u)
        return status(preflight, MDKR_MATCH_PREFLIGHT_WAITING_FOR_PEERS,
                      endpoint);
    endpoint = lowest_endpoint_for(preflight, 4);
    if (endpoint != 0u)
        return status(preflight, MDKR_MATCH_PREFLIGHT_ROM_UNVERIFIED, endpoint);
    endpoint = lowest_endpoint_for(preflight, 5);
    if (endpoint != 0u)
        return status(preflight, MDKR_MATCH_PREFLIGHT_VERIFY_PHRASE, endpoint);
    endpoint = lowest_endpoint_for(preflight, 6);
    if (endpoint != 0u)
        return status(preflight, MDKR_MATCH_PREFLIGHT_CHANNELS_NOT_READY,
                      endpoint);
    return status(preflight, MDKR_MATCH_PREFLIGHT_READY, 0u);
}
