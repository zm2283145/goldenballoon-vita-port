/* Assert-driven test: NDEBUG (the Release default) would compile every
 * check away — and delete the registration calls the asserts wrap. */
#undef NDEBUG

/* Two real match-transport endpoints exchanging input across a seeded
 * net_impairment carrier whose profile carries a two-second outage. The outage
 * is long enough that the receiving endpoint's confirmed input falls further
 * behind than the authored rollback window can ever replay, which is the exact
 * condition the launcher must resolve as a peer loss.
 *
 * The run drives the forensics ring the way production does: the transport and
 * the ring's own stall tracker record on the authored timeline, and the
 * launcher-side decision records the typed loss with the mesh's own reason
 * name. It ends by dumping the tail beside the state-hash evidence artifact.
 * The lane (tests/check_net_failure_ring_impairment.py) then reads that dump.
 *
 * Everything here is deterministic: one seed, one profile, no clock. A build
 * compiled with MDKR_NET_FAILURE_RING_DISABLED runs the identical scenario and
 * writes no dump -- the lane's positive control.
 */

#include <cassert>
#include <cstdio>
#include <cstring>

#include "net/match_transport.h"
#include "net/net_failure_ring.h"
#include "net/net_impairment.h"
#include "online/match_peer_transport.h"

namespace {

constexpr uint32_t kMatchEpoch = 7u;
constexpr uint16_t kCadenceHz = 30u;
constexpr uint32_t kTickMs = 1000u / kCadenceHz;
/* Three seconds of ticks: the profile's outage covers ticks 30..89. */
constexpr uint32_t kTicks = 90u + kCadenceHz;

struct Wire {
    uint32_t tick;
    uint8_t slot;
    MdkrPadSample sample;
};

struct Endpoint {
    MdkrSessionBridge bridge;
    MdkrMatchTransport transport;
    uint8_t slot;
};

MdkrSessionLaunchV2 launch(uint8_t localSlot) {
    MdkrSessionLaunchV2 value;
    std::memset(&value, 0, sizeof(value));
    value.version = MDKR_SESSION_LAUNCH_VERSION;
    value.size = sizeof(value);
    value.match.match_epoch = kMatchEpoch;
    value.match.protocol_version = MDKR_SESSION_PROTOCOL_VERSION;
    value.match.build_id[0] = 0x42u;
    value.match.gameplay_digest[0] = 0x91u;
    value.match.slot_owner[0] = 11u;
    value.match.slot_owner[1] = 22u;
    value.match.rng_seed = UINT64_C(0x0123456789abcdef);
    value.match.track_id = 5u;
    value.match.rom_revision = MDKR_ROM_US_11;
    value.match.cadence_hz = kCadenceHz;
    value.match.slot_count = 2u;
    value.match.rules = 1u;
    value.match.vehicle_mask = 7u;
    value.local_slot_mask = (uint8_t)(1u << localSlot);
    value.viewport_slot_mask = value.local_slot_mask;
    return value;
}

/* Per-tick variation so a repeated (predicted) frame is never accidentally
 * equal to the frame it stands in for. */
MdkrPadSample pad(uint8_t slot, uint32_t tick) {
    MdkrPadSample value;
    value.buttons = (uint16_t)(0x1000u * (slot + 1u) + (tick & 0xffu));
    value.stick_x = (int8_t)((int)(tick % 41u) - 20);
    value.stick_y = (int8_t)((int)(slot * 7u) - 3);
    value.present = 1u;
    return value;
}

void open_endpoint(Endpoint &endpoint, uint8_t slot) {
    MdkrSessionLaunchV2 admitted = launch(slot);
    endpoint.slot = slot;
    mdkr_session_bridge_init(&endpoint.bridge);
    assert(mdkr_session_bridge_apply_launch(&endpoint.bridge, &admitted));
    assert(mdkr_session_bridge_set_engine_phase(&endpoint.bridge,
                                                MDKR_ENGINE_BOOTING));
    assert(mdkr_session_bridge_set_engine_phase(&endpoint.bridge,
                                                MDKR_ENGINE_READY));
    assert(mdkr_match_transport_init(&endpoint.transport, &endpoint.bridge, 0u));
}

/* Both endpoints must retain byte-identical canonical input for a settled
 * tick. Comparing the RETAINED history (not the frame just drained) is the
 * only fair test: at drain time neither endpoint has yet received the other's
 * input for that tick, so a difference there is ordinary prediction. */
constexpr uint32_t kSettleTicks = 4u;

uint64_t frame_hash(const MdkrInputSet &frame) {
    uint64_t hash = UINT64_C(0xcbf29ce484222325);
    const unsigned char *bytes = (const unsigned char *)&frame;
    for (size_t index = 0u; index < sizeof(frame); index++) {
        hash = (hash ^ bytes[index]) * UINT64_C(0x100000001b3);
    }
    return hash;
}

/* The mesh's per-peer link health has no counterpart in a carrier-only run, so
 * the snapshot is built from what this harness genuinely knows: the profile's
 * modelled one-way delay and the carrier's byte accounting. */
MdkrNetFailureStall snapshot(const MdkrNetImpairmentProfile &profile,
                             const MdkrNetImpairment &outbound,
                             const MdkrNetImpairment &inbound) {
    MdkrNetFailureStall stall;
    std::memset(&stall, 0, sizeof(stall));
    stall.peers[1].rtt_ms = (uint16_t)(profile.latency_ticks * 2u * kTickMs);
    stall.peers[1].jitter_ms = (uint16_t)(profile.jitter_ticks * kTickMs);
    stall.peers[1].bytes_sent = (uint32_t)(outbound.sent * sizeof(Wire));
    stall.peers[1].bytes_received = (uint32_t)(inbound.sent * sizeof(Wire));
    return stall;
}

}  // namespace

int main(void) {
    MdkrNetImpairmentProfile profile;
    MdkrNetImpairment toB, toA;
    Endpoint a, b;
    uint32_t lostAtTick = 0u;
    unsigned settledComparisons = 0u;
    bool divergenceRecorded = false;
    bool lost = false;

    assert(mdkr_net_impairment_named_profile(
        MDKR_NET_PROFILE_TWO_SECOND_OUTAGE, kCadenceHz, &profile));
    mdkr_net_impairment_init(&toB, UINT64_C(0x5eed0001), profile);
    mdkr_net_impairment_init(&toA, UINT64_C(0x5eed0002), profile);
    open_endpoint(a, 0u);
    open_endpoint(b, 1u);
    mdkr_net_failure_ring_reset();

    for (uint32_t tick = 0u; tick < kTicks && !lost; tick++) {
        const uint32_t hostMs = tick * kTickMs;
        MdkrNetSimPacket packet;
        MdkrMatchRecovery recovery;
        const MdkrNetFailureStall stall = snapshot(profile, toB, toA);
        MdkrPadSample localA = pad(a.slot, tick);
        MdkrPadSample localB = pad(b.slot, tick);
        Wire fromA = {tick, a.slot, localA};
        Wire fromB = {tick, b.slot, localB};
        assert(mdkr_net_impairment_send(&toB, tick, 0u, 1u, &fromA,
                                        sizeof(fromA)));
        assert(mdkr_net_impairment_send(&toA, tick, 1u, 0u, &fromB,
                                        sizeof(fromB)));
        while (mdkr_net_impairment_receive(&toA, tick, 0u, &packet)) {
            Wire wire;
            std::memcpy(&wire, packet.bytes, sizeof(wire));
            (void)mdkr_match_transport_receive(
                &a.transport, kMatchEpoch, (uint8_t)(1u << wire.slot),
                wire.slot, wire.tick, &wire.sample);
        }
        while (mdkr_net_impairment_receive(&toB, tick, 1u, &packet)) {
            Wire wire;
            std::memcpy(&wire, packet.bytes, sizeof(wire));
            (void)mdkr_match_transport_receive(
                &b.transport, kMatchEpoch, (uint8_t)(1u << wire.slot),
                wire.slot, wire.tick, &wire.sample);
        }
        assert(mdkr_match_transport_drain_tick(&a.transport, kMatchEpoch, tick,
                                               &localA, 1u));
        assert(mdkr_match_transport_drain_tick(&b.transport, kMatchEpoch, tick,
                                               &localB, 1u));

        /* Endpoint A is the one under forensics; its confirmed-through figure
         * freezing while the authored tick advances is the stall. */
        mdkr_net_failure_ring_progress(
            tick, hostMs,
            a.transport.remote_have_confirmed[b.slot]
                ? a.transport.remote_confirmed_through[b.slot]
                : 0u,
            &stall);

        /* Two endpoints in one process is the only place a divergence between
         * authoritative timelines is observable while it happens. */
        if (tick >= kSettleTicks) {
            const uint32_t settled = tick - kSettleTicks;
            MdkrInputSet retainedA;
            MdkrInputSet retainedB;
            if (mdkr_match_transport_inputs_for_tick(
                    &a.transport, kMatchEpoch, settled, &retainedA) &&
                mdkr_match_transport_inputs_for_tick(
                    &b.transport, kMatchEpoch, settled, &retainedB) &&
                retainedA.confirmed_mask == 0x3u &&
                retainedB.confirmed_mask == 0x3u) {
                settledComparisons++;
                if (frame_hash(retainedA) != frame_hash(retainedB)) {
                    divergenceRecorded = true;
                    mdkr_net_failure_ring_record_tick(
                        MDKR_NET_FAILURE_SIMHASH_DIVERGENCE, settled,
                        MDKR_NET_FAILURE_NO_SLOT, 0u,
                        (uint32_t)frame_hash(retainedA),
                        (uint32_t)frame_hash(retainedB));
                }
            }
        }

        if (mdkr_match_transport_recovery(&a.transport, &recovery)) {
            /* The transport's sticky recovery latch is terminal: the gap can
             * no longer be replayed, so the launcher resolves the peer as lost
             * on its liveness ladder rather than resuming gameplay. */
            const MdkrMatchPeerLostReason reason =
                MdkrMatchPeerLostReason::PingTimeout;
            lost = true;
            lostAtTick = tick;
            mdkr_net_failure_ring_record_host(
                MDKR_NET_FAILURE_PEER_LOST, hostMs, b.slot,
                (unsigned)reason, mdkr_match_peer_lost_reason_name(reason));
            (void)mdkr_net_failure_ring_dump_beside_evidence();
        }
    }

    /* Fail closed in BOTH builds: a run that never forced the loss proves
     * nothing about the dump. */
    assert(lost);
    assert(toB.outage_dropped > 0u && toA.outage_dropped > 0u);
    /* The divergence detector must have had settled ticks to compare, or its
     * clean verdict would mean nothing. */
    assert(settledComparisons > 0u);
    assert(!divergenceRecorded);
    std::printf("[IMPAIR] peer_lost reason=%s tick=%u outage_dropped=%llu "
                "settled=%u recording=%d\n",
                mdkr_match_peer_lost_reason_name(
                    MdkrMatchPeerLostReason::PingTimeout),
                lostAtTick, (unsigned long long)toB.outage_dropped,
                settledComparisons,
                mdkr_net_failure_ring_recording() ? 1 : 0);
    std::puts("test_net_failure_ring_impairment: PASS");
    return 0;
}
