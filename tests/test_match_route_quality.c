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
           "inbound pump-drain drops deduct three");
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
    (void)mdkr_match_route_measure_finish(&state, 0u, &measurement);
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
    expect(!mdkr_match_route_measure_finish(&state, 0u, &empty) &&
               memcmp(&empty, &untouched, sizeof(empty)) == 0,
           "a phase that sent no probe produces no record");
    expect(!mdkr_match_route_measure_begin(&state, 1000u, 0u, 42u) &&
               !mdkr_match_route_measure_begin(&state, 1000u, 33u, 0u),
           "a zero tick period or origin id refuses to begin");

    {
        /* The queue-drain term is the carrier's own count of inbound
         * pump-drain drops, which only the caller can see; finish carries it
         * into the record and the ladder deducts for it. */
        MdkrMatchRouteMeasureState pressured;
        MdkrMatchRouteMeasurement drained;
        MdkrMatchRouteMeasurement pressed;
        MdkrMatchRouteProbe emitted;
        expect(mdkr_match_route_measure_begin(&pressured, 1000u, 1000u / 30u,
                                              42u),
               "the queue-pressure phase begins");
        expect(mdkr_match_route_measure_due(&pressured, 1000u, &emitted),
               "one probe is emitted");
        mdkr_match_route_measure_echo(&pressured, emitted.sequence, 1000u);
        expect(mdkr_match_route_measure_finish(&pressured, 0u, &drained) &&
                   drained.undrained == 0u && drained.score == 10u,
               "a carrier whose queues drained scores clean");
        expect(mdkr_match_route_measure_finish(&pressured, 70000u, &pressed) &&
                   pressed.undrained == 65535u &&
                   pressed.score == drained.score - 3u,
               "carrier queue drops saturate the field and deduct three");
    }
}

/* A window can be cut short: Start is never blocked, so a race that begins
 * before the measurement settles takes both lanes back and the launcher scores
 * what came back rather than throwing the window away. The probes that were
 * still inside their answer window at the cut are dropped from the sample set
 * -- they were lost to the cut, not to the route -- while everything older,
 * including real loss, stands. */
static void test_cut_window(void) {
    MdkrMatchRouteMeasureState state;
    MdkrMatchRouteMeasureState uncut;
    MdkrMatchRouteMeasurement whole;
    MdkrMatchRouteMeasurement scored;
    MdkrMatchRouteProbe probe;
    uint32_t now;
    unsigned dropped;
    memset(&whole, 0, sizeof(whole));
    memset(&scored, 0, sizeof(scored));
    expect(mdkr_match_route_measure_begin(&state, 1000u, 1000u / 30u, 42u),
           "the cut phase begins");
    /* One second of both lanes against a 10 ms carrier. Sequence 3's echo is
     * genuinely lost; nothing sent in the last 100 ms is answered, which is
     * what an in-flight tail looks like when the race latch closes the lanes. */
    for (now = 1000u; now < 2000u; now += 5u) {
        while (mdkr_match_route_measure_due(&state, now, &probe)) {
            if (probe.sequence == 3u || now >= 1900u) continue;
            mdkr_match_route_measure_echo(&state, probe.sequence, now + 10u);
        }
    }
    uncut = state;
    expect(mdkr_match_route_measure_finish(&uncut, 0u, &whole),
           "the uncut window still scores");

    dropped = mdkr_match_route_measure_cut(&state, 2000u);
    expect(dropped > 0u, "the cut drops the probes still in flight");
    expect(mdkr_match_route_measure_cut(&state, 2000u) == 0u,
           "a second cut has nothing left to drop");
    expect(!mdkr_match_route_measure_due(&state, 2500u, &probe),
           "a cut window emits no further probe");
    expect(mdkr_match_route_measure_cut(NULL, 2000u) == 0u,
           "cutting nothing drops nothing");

    expect(mdkr_match_route_measure_finish(&state, 0u, &scored),
           "the cut window still scores");
    expect(scored.loss_per_thousand < whole.loss_per_thousand,
           "the in-flight tail is not counted as loss");
    expect(scored.loss_per_thousand > 0u,
           "an echo lost before the cut is still loss");
    expect(scored.p95_rtt_ms == 10u,
           "the cut window reports the round trip it did measure");
    expect(!mdkr_match_route_measure_adoptable(&state),
           "one second of window does not span enough to band a route");
}

/* The margin that drops the in-flight tail scales with the route: at a 240 ms
 * round trip a fixed 100 ms margin would score a third of a second of
 * perfectly healthy probes as loss, and the drain that would have answered
 * them never runs, because the race owns the lanes now. */
static void test_cut_margin_follows_the_route(void) {
    MdkrMatchRouteMeasureState state;
    MdkrMatchRouteMeasureState uncut;
    MdkrMatchRouteMeasurement whole;
    MdkrMatchRouteMeasurement scored;
    MdkrMatchRouteProbe probe;
    uint32_t pending_at[MDKR_MATCH_ROUTE_MAX_PROBES];
    uint32_t pending_seq[MDKR_MATCH_ROUTE_MAX_PROBES];
    unsigned pending = 0u;
    uint32_t now;
    memset(&whole, 0, sizeof(whole));
    memset(&scored, 0, sizeof(scored));
    memset(pending_at, 0, sizeof(pending_at));
    memset(pending_seq, 0, sizeof(pending_seq));
    expect(mdkr_match_route_measure_begin(&state, 1000u, 1000u / 30u, 42u),
           "the 240 ms phase begins");
    /* A carrier that answers everything, 240 ms later. Nothing is lost; the
     * probes sent in the last 240 ms simply have not come back yet. */
    for (now = 1000u; now < 4000u; now += 5u) {
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
            if (pending >= MDKR_MATCH_ROUTE_MAX_PROBES) continue;
            pending_at[pending] = now + 240u;
            pending_seq[pending] = probe.sequence;
            pending++;
        }
    }
    uncut = state;
    expect(mdkr_match_route_measure_finish(&uncut, 0u, &whole) &&
               whole.loss_per_thousand > 0u,
           "scoring the whole window charges the unanswered tail as loss");
    expect(mdkr_match_route_measure_cut(&state, 4000u) > 0u,
           "the cut drops the tail a 240 ms route has not answered yet");
    expect(mdkr_match_route_measure_finish(&state, 0u, &scored),
           "the cut 240 ms window still scores");
    expect(scored.loss_per_thousand == 0u,
           "a route that lost nothing measures no loss when Start cuts it");
    expect(scored.p95_rtt_ms >= 240u,
           "and it still reports the round trip it measured");
    expect(mdkr_match_route_measure_adoptable(&state),
           "three seconds of answered samples is worth adopting");
}

/* A cut window is only adopted with evidence behind it. Both floors are real:
 * a handful of lucky echoes must not band a route nobody measured, and neither
 * must a burst of samples taken over a moment. */
static void test_cut_adoption_floor(void) {
    MdkrMatchRouteMeasureState state;
    MdkrMatchRouteProbe probe;
    uint32_t now;
    unsigned answered = 0u;
    const unsigned floor_samples = MDKR_MATCH_ROUTE_CUT_MIN_SAMPLES;

    /* One short of the sample floor, spread over a long enough span. */
    expect(mdkr_match_route_measure_begin(&state, 1000u, 1000u / 30u, 42u),
           "the sample-floor phase begins");
    for (now = 1000u; now <= 1000u + MDKR_MATCH_ROUTE_CUT_MIN_SPAN_MS;
         now += 5u) {
        while (mdkr_match_route_measure_due(&state, now, &probe)) {
            if (answered + 1u >= floor_samples) continue;
            mdkr_match_route_measure_echo(&state, probe.sequence, now + 5u);
            answered++;
        }
    }
    expect(answered == floor_samples - 1u, "exactly one sample short");
    expect(!mdkr_match_route_measure_adoptable(&state),
           "29 answered samples are not enough to band a route");

    /* The same window with the last sample answered clears the floor. */
    answered = 0u;
    expect(mdkr_match_route_measure_begin(&state, 1000u, 1000u / 30u, 42u),
           "the adopted phase begins");
    for (now = 1000u; now <= 1000u + MDKR_MATCH_ROUTE_CUT_MIN_SPAN_MS;
         now += 5u) {
        while (mdkr_match_route_measure_due(&state, now, &probe)) {
            if (answered >= floor_samples) continue;
            mdkr_match_route_measure_echo(&state, probe.sequence, now + 5u);
            answered++;
        }
    }
    expect(answered == floor_samples, "exactly the sample floor");
    expect(mdkr_match_route_measure_adoptable(&state),
           "30 answered samples over the span floor are worth adopting");

    /* Enough samples, but taken over too little window: the span floor
     * refuses it. The state lane alone reaches the sample floor in about a
     * second, which is why the two floors are separate rules. */
    answered = 0u;
    expect(mdkr_match_route_measure_begin(&state, 1000u, 1000u / 30u, 42u),
           "the span-floor phase begins");
    for (now = 1000u; now <= 2200u; now += 5u) {
        while (mdkr_match_route_measure_due(&state, now, &probe)) {
            mdkr_match_route_measure_echo(&state, probe.sequence, now + 5u);
            answered++;
        }
    }
    expect(answered >= floor_samples,
           "the short window cleared the sample floor");
    expect(!mdkr_match_route_measure_adoptable(&state),
           "samples that do not span two seconds are refused on span alone");
}

/* The degraded band's lane: 8% injected loss on the unreliable bundle lane must
 * land in ROUGH. Its positive control is a real mutation of the production
 * ladder -- route_steady_floor 8 -> 1, so every score bands steady -- which
 * fails the band assertion below (see tests/README.md). */
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
    expect(mdkr_match_route_measure_finish(&state, 0u, &measurement),
           "the impaired phase produces a record");
    expect(carrier.dropped > 0u, "the carrier really dropped datagrams");
    expect(measurement.loss_per_thousand >= 60u &&
               measurement.loss_per_thousand <= 110u,
           "8% injected loss measures as roughly 8% on the bundle lane");
    expect((MdkrMatchRouteBand)measurement.band ==
               MDKR_MATCH_ROUTE_BAND_ROUGH,
           "8% injected loss scores in the rough band");
}

int main(void) {
    test_score_ladder();
    test_bands();
    test_degenerate_input();
    test_entry_timing_widen();
    test_probe_codec();
    test_measurement_phase();
    test_cut_window();
    test_cut_margin_follows_the_route();
    test_cut_adoption_floor();
    test_impairment_eight_percent_loss();
    if (failures != 0) return 1;
    puts("match route quality: PASS (ladder, bands, widen, measurement, "
         "cut, floors, 8% loss)");
    return 0;
}
