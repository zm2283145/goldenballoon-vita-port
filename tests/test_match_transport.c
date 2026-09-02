/* Assert-driven test: NDEBUG (the Release default) would compile every
 * check away — and delete the registration calls the asserts wrap. */
#undef NDEBUG

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "platform/net/match_transport.h"

static MdkrSessionLaunchV2 launch(void) {
    MdkrSessionLaunchV2 value;
    memset(&value, 0, sizeof(value));
    value.version = MDKR_SESSION_LAUNCH_VERSION;
    value.size = sizeof(value);
    value.match.match_epoch = 7u;
    value.match.protocol_version = MDKR_SESSION_PROTOCOL_VERSION;
    value.match.build_id[0] = 0x42u;
    value.match.gameplay_digest[0] = 0x91u;
    value.match.slot_owner[0] = 11u;
    value.match.slot_owner[1] = 22u;
    value.match.slot_owner[2] = 33u;
    value.match.slot_owner[3] = 44u;
    value.match.rng_seed = UINT64_C(0x0123456789abcdef);
    value.match.track_id = 5u;
    value.match.rom_revision = MDKR_ROM_US_11;
    value.match.cadence_hz = 30u;
    value.match.slot_count = 4u;
    value.match.rules = 1u;
    value.match.vehicle_mask = 7u;
    value.local_slot_mask = 0x5u;
    value.viewport_slot_mask = 0x5u;
    return value;
}

static MdkrPadSample sample(uint16_t buttons, int x) {
    const MdkrPadSample value = {buttons, (int8_t)x, 0, 1u};
    return value;
}

int main(void) {
    MdkrSessionLaunchV2 admitted = launch();
    MdkrSessionBridge bridge;
    MdkrMatchTransport transport;
    MdkrMatchTransport before;
    MdkrSessionBridge bridge_before;
    MdkrPadSample remote1 = sample(0x1000u, -20);
    MdkrPadSample remote3 = sample(0x2000u, 30);
    MdkrPadSample local[2] = {sample(0x8000u, 10), sample(0x4000u, 40)};
    const MdkrInputSet *frame;
    const MdkrMatchTransportStats *stats;
    MdkrMatchRecovery recovery;
    uint32_t tick = 0u;

    mdkr_session_bridge_init(&bridge);
    assert(mdkr_session_bridge_apply_launch(&bridge, &admitted));
    assert(mdkr_session_bridge_set_engine_phase(&bridge, MDKR_ENGINE_BOOTING));
    assert(mdkr_session_bridge_set_engine_phase(&bridge, MDKR_ENGINE_READY));
    assert(mdkr_match_transport_init(&transport, &bridge, 100u));

    assert(mdkr_match_transport_receive(
        &transport, 7u, 0x2u, 1u, 100u, &remote1) ==
        MDKR_MATCH_INGRESS_ACCEPTED);
    assert(mdkr_match_transport_receive(
        &transport, 7u, 0x8u, 3u, 100u, &remote3) ==
        MDKR_MATCH_INGRESS_ACCEPTED);
    assert(mdkr_match_transport_receive(
        &transport, 7u, 0x2u, 1u, 100u, &remote1) ==
        MDKR_MATCH_INGRESS_DUPLICATE);
    assert(mdkr_match_transport_drain_tick(
        &transport, 7u, 100u, local, 2u));
    frame = mdkr_session_bridge_inputs(&bridge, &tick);
    assert(frame != NULL && tick == 100u);
    assert(frame->present_mask == 0x0fu && frame->confirmed_mask == 0x0fu);
    assert(frame->slots[0].buttons == local[0].buttons);
    assert(frame->slots[1].buttons == remote1.buttons);
    assert(frame->slots[2].buttons == local[1].buttons);
    assert(frame->slots[3].buttons == remote3.buttons);

    /* Missing remote input repeats the prior sample but is not confirmed. */
    assert(mdkr_match_transport_drain_tick(
        &transport, 7u, 101u, local, 2u));
    frame = mdkr_session_bridge_inputs(&bridge, &tick);
    assert(frame != NULL && tick == 101u && frame->present_mask == 0x0fu);
    assert(frame->confirmed_mask == 0x05u);
    assert(frame->slots[1].buttons == remote1.buttons &&
           frame->slots[3].buttons == remote3.buttons);

    /* Authenticated identity, not packet claims, owns ingress authorization. */
    assert(mdkr_match_transport_receive(
        &transport, 7u, 0x2u, 3u, 101u, &remote3) ==
        MDKR_MATCH_INGRESS_UNAUTHORIZED);
    assert(mdkr_match_transport_receive(
        &transport, 7u, 0x1u, 0u, 101u, &local[0]) ==
        MDKR_MATCH_INGRESS_UNAUTHORIZED);
    assert(mdkr_match_transport_receive(
        &transport, 7u, 0x3u, 1u, 101u, &remote1) ==
        MDKR_MATCH_INGRESS_UNAUTHORIZED);
    assert(mdkr_match_transport_receive(
        &transport, 6u, 0x2u, 1u, 101u, &remote1) ==
        MDKR_MATCH_INGRESS_STALE_EPOCH);

    /* A late correction marks the rollback boundary. */
    remote1.buttons = 0x0040u;
    assert(mdkr_match_transport_receive(
        &transport, 7u, 0x2u, 1u, 101u, &remote1) ==
        MDKR_MATCH_INGRESS_CORRECTED);
    assert(mdkr_match_transport_take_dirty(&transport, &tick) && tick == 101u);
    assert(mdkr_match_transport_inputs_for_tick(
        &transport, 7u, 101u, &bridge_before.latest_inputs));
    assert(bridge_before.latest_inputs.slots[1].buttons == remote1.buttons);
    assert((bridge_before.latest_inputs.confirmed_mask & 0x02u) != 0u);
    assert(!mdkr_match_transport_inputs_for_tick(
        &transport, 6u, 101u, &bridge_before.latest_inputs));
    before = transport;
    assert(!mdkr_match_transport_inputs_for_tick(
        &transport, 7u, 102u, &bridge_before.latest_inputs));
    assert(memcmp(&transport.history, &before.history,
                  sizeof(transport.history)) == 0);

    /* Drain rejection is atomic across history and bridge. */
    before = transport;
    bridge_before = bridge;
    local[1].stick_x = 81;
    assert(!mdkr_match_transport_drain_tick(
        &transport, 7u, 102u, local, 2u));
    assert(memcmp(&transport.history, &before.history,
                  sizeof(transport.history)) == 0);
    assert(memcmp(&bridge, &bridge_before, sizeof(bridge)) == 0);

    stats = mdkr_match_transport_stats(&transport);
    assert(stats != NULL && stats->accepted == 2u &&
           stats->corrected == 1u && stats->duplicates == 1u &&
           stats->unauthorized == 3u && stats->stale_epoch == 1u &&
           stats->out_of_window == 0u &&
           stats->drained == 2u && stats->drain_rejected == 1u);

    /* The repair lane reads the same bookkeeping through its own accessor:
     * the contiguous run a REMOTE slot is missing at the drain frontier,
     * without advancing confirmation or latching anything. */
    {
        MdkrMatchTransport gap_before = transport;
        uint32_t gap_first = 0u;
        uint32_t gap_count = 0u;
        assert(mdkr_match_transport_input_gap(&transport, 3u, &gap_first,
                                              &gap_count));
        assert(gap_first == 101u && gap_count == 1u);
        /* A LOCAL slot is never a repair target: this endpoint authors it. */
        assert(!mdkr_match_transport_input_gap(&transport, 0u, &gap_first,
                                               &gap_count));
        assert(memcmp(&transport, &gap_before, sizeof(transport)) == 0);
    }

    /* An authenticated timeline gap is allowed only while its predecessor is
     * still retained. Once exact replay is impossible, the request is sticky
     * and typed so launcher UX can recover instead of crashing the engine. */
    assert(!mdkr_match_transport_recovery(&transport, &recovery));
    local[1].stick_x = 40;
    for (uint32_t future = 102u;
         future <= 101u + MDKR_MATCH_TRANSPORT_ROLLBACK_TICKS + 1u;
         future++) {
        assert(mdkr_match_transport_drain_tick(
            &transport, 7u, future, local, 2u));
    }
    {
        /* The run the frontier has left behind is far longer than the batch a
         * repair may name, so the accessor reports exactly the cap. */
        uint32_t gap_first = 0u;
        uint32_t gap_count = 0u;
        assert(mdkr_match_transport_input_gap(&transport, 3u, &gap_first,
                                              &gap_count));
        assert(gap_first == 101u &&
               gap_count == MDKR_MATCH_TRANSPORT_ROLLBACK_TICKS);
    }
    assert(mdkr_match_transport_recovery(&transport, &recovery));
    assert(recovery.reason == MDKR_MATCH_RECOVERY_INPUT_GAP &&
           recovery.canonical_slot == 3u &&
           recovery.first_unrecoverable_tick == 101u &&
           recovery.observed_at_tick ==
               101u + MDKR_MATCH_TRANSPORT_ROLLBACK_TICKS + 1u);
    assert(mdkr_match_transport_receive(
        &transport, 7u, 0x2u, 1u, 70u, &remote1) ==
        MDKR_MATCH_INGRESS_OUT_OF_WINDOW);
    assert(mdkr_match_transport_recovery(&transport, &recovery) &&
           recovery.reason == MDKR_MATCH_RECOVERY_INPUT_GAP);

    /* A room-authorized future tick is the one-way ownership boundary for AI
     * takeover. Grace input remains legal, activation is neutral/confirmed in
     * history, and neither old packets nor a second owner can reopen the seat. */
    {
        MdkrSessionBridge takeover_bridge;
        MdkrMatchTransport takeover;
        MdkrPadSample takeover_local[2] = {
            sample(0x8000u, 10), sample(0x4000u, 40)};
        MdkrPadSample peer1 = sample(0x1000u, -20);
        MdkrPadSample peer3 = sample(0x2000u, 30);
        uint8_t ai_mask = 0xffu;
        mdkr_session_bridge_init(&takeover_bridge);
        assert(mdkr_session_bridge_apply_launch(&takeover_bridge, &admitted));
        assert(mdkr_session_bridge_set_engine_phase(
            &takeover_bridge, MDKR_ENGINE_BOOTING));
        assert(mdkr_session_bridge_set_engine_phase(
            &takeover_bridge, MDKR_ENGINE_READY));
        assert(mdkr_match_transport_init(&takeover, &takeover_bridge, 10u));
        assert(mdkr_match_transport_receive(
            &takeover, 7u, 0x2u, 1u, 10u, &peer1) ==
            MDKR_MATCH_INGRESS_ACCEPTED);
        assert(mdkr_match_transport_receive(
            &takeover, 7u, 0x8u, 3u, 10u, &peer3) ==
            MDKR_MATCH_INGRESS_ACCEPTED);
        assert(mdkr_match_transport_drain_tick(
            &takeover, 7u, 10u, takeover_local, 2u));
        assert(mdkr_match_transport_schedule_ai_takeover(
            &takeover, 6u, 1u, 13u) == MDKR_MATCH_TAKEOVER_STALE_EPOCH);
        assert(mdkr_match_transport_schedule_ai_takeover(
            &takeover, 7u, 4u, 13u) == MDKR_MATCH_TAKEOVER_INVALID);
        assert(mdkr_match_transport_schedule_ai_takeover(
            &takeover, 7u, 1u, 10u) == MDKR_MATCH_TAKEOVER_TOO_LATE);
        assert(mdkr_match_transport_schedule_ai_takeover(
            &takeover, 7u, 1u, 13u) == MDKR_MATCH_TAKEOVER_ACCEPTED);
        assert(mdkr_match_transport_schedule_ai_takeover(
            &takeover, 7u, 1u, 13u) == MDKR_MATCH_TAKEOVER_DUPLICATE);
        assert(mdkr_match_transport_schedule_ai_takeover(
            &takeover, 7u, 1u, 14u) == MDKR_MATCH_TAKEOVER_CONFLICT);
        assert(mdkr_match_transport_ai_takeover_mask_for_tick(
            &takeover, 7u, 12u, &ai_mask) && ai_mask == 0u);
        assert(mdkr_match_transport_ai_takeover_mask_for_tick(
            &takeover, 7u, 13u, &ai_mask) && ai_mask == 0x02u);
        assert(mdkr_match_transport_receive(
            &takeover, 7u, 0x2u, 1u, 12u, &peer1) ==
            MDKR_MATCH_INGRESS_ACCEPTED);
        assert(mdkr_match_transport_receive(
            &takeover, 7u, 0x2u, 1u, 13u, &peer1) ==
            MDKR_MATCH_INGRESS_TAKEN_OVER);
        for (uint32_t authored = 11u; authored <= 45u; authored++) {
            assert(mdkr_match_transport_receive(
                &takeover, 7u, 0x8u, 3u, authored, &peer3) ==
                MDKR_MATCH_INGRESS_ACCEPTED);
            if (authored == 15u) {
                assert(mdkr_match_transport_schedule_ai_takeover(
                    &takeover, 7u, 0u, authored) ==
                    MDKR_MATCH_TAKEOVER_ACCEPTED);
            }
            assert(mdkr_match_transport_drain_tick(
                &takeover, 7u, authored, takeover_local, 2u));
            if (authored >= 13u) {
                MdkrInputSet retained;
                assert(mdkr_match_transport_inputs_for_tick(
                    &takeover, 7u, authored, &retained));
                assert(retained.slots[1].buttons == 0u &&
                       (retained.confirmed_mask & 0x02u) != 0u);
                if (authored >= 15u) {
                    assert(retained.slots[0].buttons == 0u &&
                           (retained.confirmed_mask & 0x01u) != 0u);
                }
            }
        }
        assert(mdkr_match_transport_receive(
            &takeover, 7u, 0x2u, 1u, 12u, &peer1) ==
            MDKR_MATCH_INGRESS_TAKEN_OVER);
        assert(!mdkr_match_transport_recovery(&takeover, &recovery));
    }

    /* ---- Lobby-authoritative drop: the agreed finalisation tick ----------
     *
     * The tick a departed seat stops consuming peer input at. The floor is the
     * confirmed frontier + 1 (nothing past it is common knowledge), the
     * authored head raises it (an authored tick's inputs are already spent, and
     * schedule_ai_takeover refuses it), and the agreed input-delay lead raises
     * it again so a survivor whose head runs that far ahead can still adopt. */
    {
        /* No confirmed frontier yet: the head and the lead decide. */
        assert(mdkr_match_drop_finalisation_tick(0u, false, 100u, 0u) == 101u);
        assert(mdkr_match_drop_finalisation_tick(0u, false, 100u, 3u) == 104u);
        /* The ordinary shape -- confirmation trails the head, so the head
         * wins and the floor is already satisfied. */
        assert(mdkr_match_drop_finalisation_tick(95u, true, 100u, 2u) == 103u);
        /* A frontier at or past the head keeps the floor: finalising at 99
         * would rewrite a frame committed with the departed peer's real input. */
        assert(mdkr_match_drop_finalisation_tick(100u, true, 100u, 0u) == 101u);
        assert(mdkr_match_drop_finalisation_tick(120u, true, 100u, 2u) == 123u);
        /* Half-range ordering, not magnitude: the tick space wraps, and a
         * numerically huge frontier just below the head is still behind it. */
        assert(mdkr_match_drop_finalisation_tick(
                   0xfffffffeu, true, 0xffffffffu, 2u) == 2u);
        assert(mdkr_match_drop_finalisation_tick(
                   0xffffffffu, true, 0xfffffffdu, 1u) == 1u);
    }

    /* Exactly one survivor proposes, so two survivors cannot commit different
     * ticks for the same seat. Every survivor evaluates the rule identically
     * over the same surviving roster. */
    {
        const uint64_t survivors[3] = {77u, 12u, 40u};
        assert(mdkr_match_drop_is_proposer(12u, survivors, 3u));
        assert(!mdkr_match_drop_is_proposer(40u, survivors, 3u));
        assert(!mdkr_match_drop_is_proposer(77u, survivors, 3u));
        /* The 2P survivor is alone, so it proposes whatever its id is and
         * needs no round trip. */
        assert(mdkr_match_drop_is_proposer(77u, survivors, 1u));
        assert(mdkr_match_drop_is_proposer(40u, &survivors[2], 1u));
        assert(!mdkr_match_drop_is_proposer(77u, &survivors[2], 1u));
        /* An endpoint outside the surviving roster never proposes for it. */
        assert(!mdkr_match_drop_is_proposer(5u, survivors, 3u));
        assert(!mdkr_match_drop_is_proposer(12u, NULL, 3u));
        assert(!mdkr_match_drop_is_proposer(12u, survivors, 0u));
    }

    /* ---- The drop's determinism proof ------------------------------------
     *
     * A transport whose departed seat is finalised at T must commit exactly the
     * frames a reference run commits when that seat simply sends neutral input
     * from T. Same launch, same local input, same tick range; the only
     * difference is how slot 1's neutral frames arrive. Diverging here is the
     * whole failure mode the drop exists to avoid: a survivor that finalises a
     * seat its peers did not would author a different race. */
    {
        MdkrSessionLaunchV2 dropped_launch = launch();
        MdkrSessionLaunchV2 reference_launch = launch();
        MdkrSessionBridge dropped_bridge;
        MdkrSessionBridge reference_bridge;
        MdkrMatchTransport dropped;
        MdkrMatchTransport reference;
        MdkrPadSample seat_local[2];
        MdkrPadSample peer1 = sample(0x0010u, 5);
        MdkrPadSample peer3 = sample(0x0020u, -5);
        const MdkrPadSample neutral = {0u, 0, 0, 1u};
        const uint32_t first = 200u;
        const uint32_t finalise = 210u;
        uint32_t authored;
        unsigned compared = 0u;

        seat_local[0] = sample(0x0100u, 9);
        seat_local[1] = sample(0x0200u, -9);
        mdkr_session_bridge_init(&dropped_bridge);
        mdkr_session_bridge_init(&reference_bridge);
        assert(mdkr_session_bridge_apply_launch(
            &dropped_bridge, &dropped_launch));
        assert(mdkr_session_bridge_apply_launch(
            &reference_bridge, &reference_launch));
        assert(mdkr_session_bridge_set_engine_phase(
            &dropped_bridge, MDKR_ENGINE_BOOTING));
        assert(mdkr_session_bridge_set_engine_phase(
            &reference_bridge, MDKR_ENGINE_BOOTING));
        assert(mdkr_session_bridge_set_engine_phase(
            &dropped_bridge, MDKR_ENGINE_READY));
        assert(mdkr_session_bridge_set_engine_phase(
            &reference_bridge, MDKR_ENGINE_READY));
        assert(mdkr_match_transport_init(&dropped, &dropped_bridge, first));
        assert(mdkr_match_transport_init(
            &reference, &reference_bridge, first));

        for (authored = first; authored < first + 40u; authored++) {
            const bool before = authored < finalise;
            /* Slot 3 races on untouched in both runs. */
            assert(mdkr_match_transport_receive(
                &dropped, 7u, 0x8u, 3u, authored, &peer3) ==
                MDKR_MATCH_INGRESS_ACCEPTED);
            assert(mdkr_match_transport_receive(
                &reference, 7u, 0x8u, 3u, authored, &peer3) ==
                MDKR_MATCH_INGRESS_ACCEPTED);
            if (before) {
                assert(mdkr_match_transport_receive(
                    &dropped, 7u, 0x2u, 1u, authored, &peer1) ==
                    MDKR_MATCH_INGRESS_ACCEPTED);
                assert(mdkr_match_transport_receive(
                    &reference, 7u, 0x2u, 1u, authored, &peer1) ==
                    MDKR_MATCH_INGRESS_ACCEPTED);
            } else {
                /* The reference peer keeps sending, neutral. The dropped run
                 * hears nothing more from it -- and refuses it if it speaks. */
                assert(mdkr_match_transport_receive(
                    &reference, 7u, 0x2u, 1u, authored, &neutral) ==
                    MDKR_MATCH_INGRESS_ACCEPTED);
                assert(mdkr_match_transport_receive(
                    &dropped, 7u, 0x2u, 1u, authored, &peer1) ==
                    MDKR_MATCH_INGRESS_TAKEN_OVER);
            }
            assert(mdkr_match_transport_drain_tick(
                &dropped, 7u, authored, seat_local, 2u));
            assert(mdkr_match_transport_drain_tick(
                &reference, 7u, authored, seat_local, 2u));
            if (authored == finalise - 3u) {
                /* The room reports the departure three ticks early; the agreed
                 * tick lands on the authored head plus the match's
                 * input-delay lead. */
                assert(mdkr_match_drop_finalisation_tick(
                    dropped.history.confirmed_through,
                    dropped.history.have_confirmed,
                    dropped.history.current_tick, 2u) == finalise);
                assert(mdkr_match_transport_schedule_ai_takeover(
                    &dropped, 7u, 1u, finalise) ==
                    MDKR_MATCH_TAKEOVER_ACCEPTED);
            }
            {
                MdkrInputSet dropped_frame;
                MdkrInputSet reference_frame;
                assert(mdkr_match_transport_inputs_for_tick(
                    &dropped, 7u, authored, &dropped_frame));
                assert(mdkr_match_transport_inputs_for_tick(
                    &reference, 7u, authored, &reference_frame));
                assert(memcmp(&dropped_frame, &reference_frame,
                              sizeof(dropped_frame)) == 0);
                if (!before) {
                    /* Non-vacuous: the frames being compared really are the
                     * neutral ones the drop authored, confirmed, not predicted
                     * from the departed peer's last real sample. */
                    assert(dropped_frame.slots[1].buttons == 0u &&
                           dropped_frame.slots[1].stick_x == 0 &&
                           (dropped_frame.confirmed_mask & 0x02u) != 0u);
                    assert(peer1.buttons != 0u);
                    compared++;
                }
            }
        }
        assert(compared == 30u);
        /* A run that never finalised the seat diverges from the reference at
         * the very first tick past T -- the repeat-last predictor holds the
         * departed peer's last real sample instead of going neutral. */
        {
            MdkrSessionLaunchV2 stalled_launch = launch();
            MdkrSessionBridge stalled_bridge;
            MdkrMatchTransport stalled;
            MdkrInputSet stalled_frame;
            MdkrInputSet reference_frame;
            mdkr_session_bridge_init(&stalled_bridge);
            assert(mdkr_session_bridge_apply_launch(
                &stalled_bridge, &stalled_launch));
            assert(mdkr_session_bridge_set_engine_phase(
                &stalled_bridge, MDKR_ENGINE_BOOTING));
            assert(mdkr_session_bridge_set_engine_phase(
                &stalled_bridge, MDKR_ENGINE_READY));
            assert(mdkr_match_transport_init(
                &stalled, &stalled_bridge, first));
            for (authored = first; authored <= finalise; authored++) {
                assert(mdkr_match_transport_receive(
                    &stalled, 7u, 0x8u, 3u, authored, &peer3) ==
                    MDKR_MATCH_INGRESS_ACCEPTED);
                if (authored < finalise) {
                    assert(mdkr_match_transport_receive(
                        &stalled, 7u, 0x2u, 1u, authored, &peer1) ==
                        MDKR_MATCH_INGRESS_ACCEPTED);
                }
                assert(mdkr_match_transport_drain_tick(
                    &stalled, 7u, authored, seat_local, 2u));
            }
            assert(mdkr_match_transport_inputs_for_tick(
                &stalled, 7u, finalise, &stalled_frame));
            assert(mdkr_match_transport_inputs_for_tick(
                &reference, 7u, finalise, &reference_frame));
            assert(memcmp(&stalled_frame, &reference_frame,
                          sizeof(stalled_frame)) != 0);
            assert(stalled_frame.slots[1].buttons == peer1.buttons);
        }
    }

    puts("test_match_transport: PASS");
    return 0;
}
