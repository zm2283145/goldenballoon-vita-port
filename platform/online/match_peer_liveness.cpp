#include "match_peer_liveness.h"

MdkrPeerLivenessTracker mdkr_peer_liveness_on_hit(
    MdkrPeerLivenessTracker tracker) {
    tracker.state = MdkrPeerLivenessState::Good;
    tracker.consecutiveMisses = 0u;
    tracker.haveMeasurement = true;
    return tracker;
}

MdkrPeerLivenessTracker mdkr_peer_liveness_on_miss(
    MdkrPeerLivenessTracker tracker, unsigned missesToUnreachable) {
    /* A peer that has never answered a probe has nothing to go stale --
     * that is the setup ladder's business, not this policy's. */
    if (!tracker.haveMeasurement) return tracker;
    if (tracker.consecutiveMisses < missesToUnreachable) {
        tracker.consecutiveMisses++;
    }
    tracker.state = tracker.consecutiveMisses >= missesToUnreachable
                         ? MdkrPeerLivenessState::Unreachable
                         : MdkrPeerLivenessState::Transient;
    return tracker;
}
