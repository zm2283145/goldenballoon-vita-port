/* Pre-flight route-quality measurement: the score ladder, its named bands, the
 * entry-timing widen, the probe payload codec, the caller-clocked measurement
 * phase, and the impairment lane that pins 8% injected loss into the degraded
 * band. */
#include "platform/net/match_preflight.h"

#include <stdio.h>
#include <string.h>

#include "platform/net/net_impairment.h"

static int failures;

static void expect(int condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        failures++;
    }
}

static MdkrMatchRouteMeasurement metrics(uint16_t p95, uint16_t jitter,
                                         uint16_t loss, uint16_t late,
                                         uint16_t undrained) {
    MdkrMatchRouteMeasurement value;
    memset(&value, 0, sizeof(value));
    value.p95_rtt_ms = p95;
    value.jitter_ms = jitter;
    value.loss_per_thousand = loss;
    value.late_per_thousand = late;
    value.undrained = undrained;
    return value;
}

static uint8_t score_of(uint16_t p95, uint16_t jitter, uint16_t loss,
                        uint16_t late, uint16_t undrained) {
    MdkrMatchRouteMeasurement value =
        metrics(p95, jitter, loss, late, undrained);
    return mdkr_match_route_measurement_score(&value) ? value.score : 0u;
}

static MdkrMatchRouteBand band_of(uint16_t p95, uint16_t jitter, uint16_t loss,
                                  uint16_t late, uint16_t undrained) {
    MdkrMatchRouteMeasurement value =
        metrics(p95, jitter, loss, late, undrained);
    return mdkr_match_route_measurement_score(&value)
               ? (MdkrMatchRouteBand)value.band
               : MDKR_MATCH_ROUTE_BAND_NONE;
}

static void test_score_ladder(void) {
    /* Each rung is pinned at its own boundary and one millisecond past it, so
     * a moved threshold cannot pass by landing in a neighbouring rung. */
    expect(score_of(0u, 0u, 0u, 0u, 0u) == 10u, "a clean route scores 10");
    expect(score_of(40u, 5u, 5u, 10u, 0u) == 10u,
           "every metric at its first threshold still scores 10");
    expect(score_of(41u, 0u, 0u, 0u, 0u) == 9u, "p95 past 40 ms deducts one");
    expect(score_of(70u, 0u, 0u, 0u, 0u) == 9u, "p95 at 70 ms deducts one");
    expect(score_of(71u, 0u, 0u, 0u, 0u) == 8u, "p95 past 70 ms deducts two");
    expect(score_of(111u, 0u, 0u, 0u, 0u) == 7u, "p95 past 110 ms deducts three");
    expect(score_of(161u, 0u, 0u, 0u, 0u) == 6u, "p95 past 160 ms deducts four");
    expect(score_of(221u, 0u, 0u, 0u, 0u) == 5u, "p95 past 220 ms deducts five");
    expect(score_of(65535u, 0u, 0u, 0u, 0u) == 5u,
           "the p95 rung saturates rather than growing");

    expect(score_of(0u, 6u, 0u, 0u, 0u) == 9u, "jitter past 5 ms deducts one");
    expect(score_of(0u, 13u, 0u, 0u, 0u) == 8u, "jitter past 12 ms deducts two");
    expect(score_of(0u, 26u, 0u, 0u, 0u) == 7u, "jitter past 25 ms deducts three");
    expect(score_of(0u, 46u, 0u, 0u, 0u) == 6u, "jitter past 45 ms deducts four");

    expect(score_of(0u, 0u, 6u, 0u, 0u) == 8u, "loss past 0.5% deducts two");
    expect(score_of(0u, 0u, 21u, 0u, 0u) == 6u, "loss past 2% deducts four");
    expect(score_of(0u, 0u, 51u, 0u, 0u) == 4u, "loss past 5% deducts six");

    expect(score_of(0u, 0u, 0u, 11u, 0u) == 9u,
           "late samples past 1% deduct one");
    expect(score_of(0u, 0u, 0u, 51u, 0u) == 8u,
           "late samples past 5% deduct two");
    expect(score_of(0u, 0u, 0u, 151u, 0u) == 7u,
           "late samples past 15% deduct three");

    expect(score_of(0u, 0u, 0u, 0u, 1u) == 7u,
           "an undrained outbound queue deducts three");
    expect(score_of(65535u, 65535u, 1000u, 1000u, 1u) == 1u,
           "the ladder floors at one rather than underflowing");
}

static void test_bands(void) {
    unsigned score;
    static const MdkrMatchRouteBand expected[11] = {
        MDKR_MATCH_ROUTE_BAND_NONE,   MDKR_MATCH_ROUTE_BAND_ROUGH,
        MDKR_MATCH_ROUTE_BAND_ROUGH,  MDKR_MATCH_ROUTE_BAND_ROUGH,
        MDKR_MATCH_ROUTE_BAND_ROUGH,  MDKR_MATCH_ROUTE_BAND_UNEVEN,
        MDKR_MATCH_ROUTE_BAND_UNEVEN, MDKR_MATCH_ROUTE_BAND_UNEVEN,
        MDKR_MATCH_ROUTE_BAND_STEADY, MDKR_MATCH_ROUTE_BAND_STEADY,
        MDKR_MATCH_ROUTE_BAND_STEADY};
    for (score = 0u; score <= 10u; score++)
        expect(mdkr_match_route_band((uint8_t)score) == expected[score],
               "every score maps to its documented band");
    expect(mdkr_match_route_band(11u) == MDKR_MATCH_ROUTE_BAND_NONE,
           "a score outside the ladder has no band");
    expect(strcmp(mdkr_match_route_band_name(MDKR_MATCH_ROUTE_BAND_STEADY),
                  "steady") == 0 &&
               strcmp(mdkr_match_route_band_name(MDKR_MATCH_ROUTE_BAND_UNEVEN),
                      "uneven") == 0 &&
               strcmp(mdkr_match_route_band_name(MDKR_MATCH_ROUTE_BAND_ROUGH),
                      "rough") == 0,
           "band names are the copy the room chip shows");
    expect(mdkr_match_route_band_name(MDKR_MATCH_ROUTE_BAND_NONE) == NULL,
           "an unscored record has no band name");
    expect(band_of(0u, 0u, 0u, 0u, 0u) == MDKR_MATCH_ROUTE_BAND_STEADY,
           "a clean route bands steady");
    expect(band_of(100u, 10u, 0u, 30u, 0u) == MDKR_MATCH_ROUTE_BAND_UNEVEN,
           "a middling route bands uneven");
    expect(band_of(0u, 0u, 80u, 0u, 0u) == MDKR_MATCH_ROUTE_BAND_ROUGH,
           "8% loss alone bands rough");
}

static void test_degenerate_input(void) {
    MdkrMatchRouteMeasurement value = metrics(10u, 1u, 1001u, 0u, 0u);
    MdkrMatchRouteMeasurement untouched = value;
    expect(!mdkr_match_route_measurement_score(&value) &&
               memcmp(&value, &untouched, sizeof(value)) == 0,
           "a loss rate above 100% rejects without mutating the record");
    value = metrics(10u, 1u, 0u, 1001u, 0u);
    untouched = value;
    expect(!mdkr_match_route_measurement_score(&value) &&
               memcmp(&value, &untouched, sizeof(value)) == 0,
           "a late rate above 100% rejects without mutating the record");
    expect(!mdkr_match_route_measurement_score(NULL),
           "a null record rejects");
    expect(score_of(0u, 0u, 1000u, 1000u, 0u) == 1u,
           "a route that lost everything still produces a floor score");
}

static void test_entry_timing_widen(void) {
    const uint32_t tick_ms = 1000u / 30u;
    expect(mdkr_match_route_input_delay(45u, tick_ms, 2u) == 2u,
           "a route inside the agreed floor does not widen");
    expect(mdkr_match_route_input_delay(70u, tick_ms, 2u) == 3u,
           "a route needing three ticks widens by one");
    expect(mdkr_match_route_input_delay(240u, tick_ms, 2u) == 4u,
           "a slow route widens only to the documented cap");
    expect(mdkr_match_route_input_delay(0u, tick_ms, 2u) == 2u,
           "a zero measurement leaves the agreed floor alone");
    expect(mdkr_match_route_input_delay(65535u, tick_ms, 5u) == 5u,
           "a floor already past the cap is never lowered");
    expect(mdkr_match_route_input_delay(240u, 0u, 2u) == 2u,
           "a zero tick period cannot widen");
}

static void test_probe_codec(void) {
    MdkrMatchRouteProbe probe;
    MdkrMatchRouteProbe decoded;
    uint8_t payload[MDKR_MATCH_PEER_PAYLOAD_BYTES];
    unsigned index;
    memset(&probe, 0, sizeof(probe));
    probe.origin_endpoint_id = UINT64_C(0x1122334455667788);
    probe.sequence = 9u;
    probe.lane = MDKR_MATCH_ROUTE_LANE_CONTROL;
    probe.kind = MDKR_MATCH_ROUTE_ECHO;
    memset(payload, 0xa5, sizeof(payload));
    expect(mdkr_match_route_probe_encode(&probe, payload),
           "a probe encodes into one fixed carrier payload");
    expect(payload[0] == 'M' && payload[1] == 'R' && payload[2] == 'Q' &&
               payload[3] == '1' && payload[4] == MDKR_MATCH_ROUTE_LANE_CONTROL &&
               payload[5] == MDKR_MATCH_ROUTE_ECHO && payload[9] == 9u,
           "the probe header binds format, lane, kind and sequence");
    for (index = 18u; index < MDKR_MATCH_PEER_PAYLOAD_BYTES; index++)
        if (payload[index] != 0u) {
            expect(0, "probe filler is zero to the lane's real payload size");
            break;
        }
    memset(&decoded, 0xa5, sizeof(decoded));
    expect(mdkr_match_route_probe_decode(payload, &decoded) &&
               memcmp(&decoded, &probe, sizeof(decoded)) == 0,
           "a probe round-trips exactly");
    payload[3] = '2';
    expect(!mdkr_match_route_probe_decode(payload, &decoded),
           "a foreign payload is not mistaken for a probe");
    payload[3] = '1';
    payload[63] = 1u;
    expect(!mdkr_match_route_probe_decode(payload, &decoded),
           "non-zero filler rejects");
    payload[63] = 0u;
    payload[4] = MDKR_MATCH_ROUTE_LANE_COUNT;
    expect(!mdkr_match_route_probe_decode(payload, &decoded),
           "an unknown lane rejects");
    probe.sequence = 0u;
    expect(!mdkr_match_route_probe_encode(&probe, payload),
           "a zero sequence never encodes");
}

/* Replays both lanes for the full window against a carrier that answers every
 * probe after `one_way_ms`, dropping one echo in `drop_every` when set. */
static MdkrMatchRouteMeasurement run_measurement(uint32_t one_way_ms,
                                                 unsigned drop_every,
                                                 unsigned *sent_out,
                                                 unsigned *bundle_out) {
    MdkrMatchRouteMeasureState state;
    MdkrMatchRouteMeasurement measurement;
    MdkrMatchRouteProbe probe;
    uint32_t pending_at[MDKR_MATCH_ROUTE_MAX_PROBES];
    uint32_t pending_seq[MDKR_MATCH_ROUTE_MAX_PROBES];
    unsigned pending = 0u;
    unsigned sent = 0u;
    unsigned bundle = 0u;
    uint32_t now;
    memset(&measurement, 0, sizeof(measurement));
    memset(pending_at, 0, sizeof(pending_at));
    memset(pending_seq, 0, sizeof(pending_seq));
    if (!mdkr_match_route_measure_begin(&state, 1000u, 1000u / 30u, 42u))
        return measurement;
    for (now = 1000u; !mdkr_match_route_measure_settled(&state, now);
         now += 5u) {
        unsigned index = 0u;
        while (index < pending) {
            if (pending_at[index] <= now) {
                mdkr_match_route_measure_echo(&state, pending_seq[index], now);
                pending_at[index] = pending_at[pending - 1u];
                pending_seq[index] = pending_seq[pending - 1u];
                pending--;
                continue;
            }
            index++;
        }
        while (mdkr_match_route_measure_due(&state, now, &probe)) {
            sent++;
            if (probe.lane == MDKR_MATCH_ROUTE_LANE_BUNDLE) bundle++;
            if (drop_every != 0u && sent % drop_every == 0u) continue;
            if (pending >= MDKR_MATCH_ROUTE_MAX_PROBES) continue;
            pending_at[pending] = now + one_way_ms * 2u;
            pending_seq[pending] = probe.sequence;
            pending++;
        }
    }
    (void)mdkr_match_route_measure_finish(&state, &measurement);
    if (sent_out != NULL) *sent_out = sent;
    if (bundle_out != NULL) *bundle_out = bundle;
    return measurement;
}

static void test_measurement_phase(void) {
    unsigned sent = 0u;
    unsigned bundle = 0u;
    MdkrMatchRouteMeasureState state;
    MdkrMatchRouteMeasurement empty;
    MdkrMatchRouteMeasurement untouched;
    const MdkrMatchRouteMeasurement clean =
        run_measurement(2u, 0u, &sent, &bundle);
    /* 6 s of the authored tick cadence plus 6 s of the 200 ms control lane. */
    expect(bundle >= 180u && bundle <= 182u,
           "the bundle lane replays the authored tick cadence for 6 s");
    expect(sent - bundle == MDKR_MATCH_ROUTE_MEASURE_MS /
                                MDKR_MATCH_ROUTE_CONTROL_CADENCE_MS,
           "the control lane replays its 200 ms cadence for 6 s");
    expect(clean.loss_per_thousand == 0u && clean.undrained == 0u,
           "an answering carrier loses nothing and drains");
    expect(clean.p95_rtt_ms <= 10u && clean.late_per_thousand == 0u,
           "a fast carrier measures a small p95 and no late samples");
    expect(clean.band == MDKR_MATCH_ROUTE_BAND_STEADY,
           "a fast lossless carrier bands steady");

    {
        const MdkrMatchRouteMeasurement slow = run_measurement(90u, 0u, NULL, NULL);
        expect(slow.p95_rtt_ms >= 180u,
               "a 90 ms one-way carrier measures its round trip");
        expect(slow.late_per_thousand == 1000u,
               "every sample past 100 ms counts as late");
        expect(slow.band != MDKR_MATCH_ROUTE_BAND_STEADY,
               "a slow route does not band steady");
    }
    {
        const MdkrMatchRouteMeasurement lossy = run_measurement(2u, 10u, NULL, NULL);
        expect(lossy.loss_per_thousand >= 95u && lossy.loss_per_thousand <= 105u,
               "one echo dropped in ten measures as 10% loss");
    }

    memset(&empty, 0xa5, sizeof(empty));
    untouched = empty;
    expect(mdkr_match_route_measure_begin(&state, 1000u, 1000u / 30u, 42u),
           "the measurement phase begins");
    expect(!mdkr_match_route_measure_finish(&state, &empty) &&
               memcmp(&empty, &untouched, sizeof(empty)) == 0,
           "a phase that sent no probe produces no record");
    expect(!mdkr_match_route_measure_begin(&state, 1000u, 0u, 42u) &&
               !mdkr_match_route_measure_begin(&state, 1000u, 33u, 0u),
           "a zero tick period or origin id refuses to begin");
}

/* Positive control for the degraded band: 8% injected loss on the unreliable
 * bundle lane must land in ROUGH. Neutering the band mapping (every score
 * banding steady, the shape a chip that always reassures would have) fails the
 * same assertion the real mapping passes. */
static MdkrMatchRouteBand neutered_band(uint8_t score) {
    (void)score;
    return MDKR_MATCH_ROUTE_BAND_STEADY;
}

static void test_impairment_eight_percent_loss(void) {
    MdkrNetImpairment carrier;
    MdkrNetImpairmentProfile profile;
    MdkrMatchRouteMeasureState state;
    MdkrMatchRouteMeasurement measurement;
    MdkrMatchRouteProbe probe;
    MdkrNetSimPacket packet;
    uint32_t now;
    memset(&profile, 0, sizeof(profile));
    profile.loss_per_thousand = 80u;
    profile.max_deliveries_per_tick = 64u;
    mdkr_net_impairment_init(&carrier, UINT64_C(0x524f555445), profile);
    memset(&measurement, 0, sizeof(measurement));
    expect(mdkr_match_route_measure_begin(&state, 1000u, 1000u / 30u, 42u),
           "the impaired measurement phase begins");
    /* The carrier's own tick is the probe index; only the bundle lane rides
     * it, which is exactly the unreliable state channel's behaviour. */
    for (now = 1000u; !mdkr_match_route_measure_settled(&state, now);
         now += 5u) {
        uint8_t payload[MDKR_MATCH_PEER_PAYLOAD_BYTES];
        while (mdkr_match_route_measure_due(&state, now, &probe)) {
            if (!mdkr_match_route_probe_encode(&probe, payload)) continue;
            if (probe.lane != MDKR_MATCH_ROUTE_LANE_BUNDLE) {
                mdkr_match_route_measure_echo(&state, probe.sequence, now);
                continue;
            }
            (void)mdkr_net_impairment_send(&carrier, now / 5u, 0u, 1u, payload,
                                           MDKR_MATCH_PEER_PAYLOAD_BYTES);
        }
        while (mdkr_net_impairment_receive(&carrier, now / 5u, 1u, &packet)) {
            MdkrMatchRouteProbe echoed;
            if (mdkr_match_route_probe_decode(packet.bytes, &echoed))
                mdkr_match_route_measure_echo(&state, echoed.sequence, now);
        }
    }
    expect(mdkr_match_route_measure_finish(&state, &measurement),
           "the impaired phase produces a record");
    expect(carrier.dropped > 0u, "the carrier really dropped datagrams");
    expect(measurement.loss_per_thousand >= 60u &&
               measurement.loss_per_thousand <= 110u,
           "8% injected loss measures as roughly 8% on the bundle lane");
    expect((MdkrMatchRouteBand)measurement.band ==
               MDKR_MATCH_ROUTE_BAND_ROUGH,
           "8% injected loss scores in the rough band");
    expect(neutered_band(measurement.score) != MDKR_MATCH_ROUTE_BAND_ROUGH,
           "a neutered band mapping fails the same assertion");
}

int main(void) {
    test_score_ladder();
    test_bands();
    test_degenerate_input();
    test_entry_timing_widen();
    test_probe_codec();
    test_measurement_phase();
    test_impairment_eight_percent_loss();
    if (failures != 0) return 1;
    puts("match route quality: PASS (ladder, bands, widen, measurement, 8% loss)");
    return 0;
}
