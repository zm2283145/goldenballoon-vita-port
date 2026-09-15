/*
 * Pure soft-fail/hard-fail liveness policy for a peer's control-ping ladder
 * (A7).
 *
 * match_peer_transport.h's control ping ladder (kMdkrMatchControlPingIntervalMs
 * / kMdkrMatchControlPingTimeoutMs) already answers "is this peer connected at
 * all": one missed pong, and 15 s later peerLost(PingTimeout) ends the peer.
 * That verdict is correct and this file changes NOTHING about it -- it is
 * about what the player sees in the seconds BEFORE that verdict lands. Before
 * this policy existed the live status line showed nothing but "Direct
 * Connection" right up to the moment the race ended, so a real network wobble
 * that recovers on its own read to the player as no warning at all, and one
 * that does not recover read as a sudden, unexplained loss.
 *
 * This is the pure decision half: given a stream of "probe answered" /
 * "probe interval elapsed unanswered" observations for one peer, decide what
 * state the live status line should be in right now. It touches no clock, no
 * transport state and no UI, and it has no opinion on player copy either --
 * lobby_view_model.c owns that single source of truth (its RACING status
 * switch), keyed off the session connectivity code the caller maps this
 * state onto. The last real measurement a hit produced needs no bookkeeping
 * here: the transport's own peer.rttMs is untouched by a miss (it only
 * updates on an answered pong), so it is already "the last known
 * measurement" for free.
 *
 * Policy (values are contractual -- update this comment and its test
 * together with any change):
 *  - A peer that has never yet answered a probe is not this policy's
 *    business (its setup ladder owns that case): a miss observed before the
 *    first hit leaves the tracker untouched at Good.
 *  - A previously-good peer's FIRST missed probe interval does not announce
 *    a loss: it moves to Transient (soft-fail).
 *  - Only missesToUnreachable CONSECUTIVE missed intervals from a
 *    previously-good peer escalate to Unreachable (hard-fail). The caller
 *    derives missesToUnreachable from the same ladder the mesh's own
 *    timeout uses (kMdkrMatchControlPingTimeoutMs /
 *    kMdkrMatchControlPingIntervalMs = 3, see match_live_adapter.cpp's
 *    static_assert against those constants) so the presenter's "Connection
 *    lost" lands at the same wall-clock boundary peerLost(PingTimeout) does,
 *    never earlier.
 *  - Any answered probe -- from Transient or Unreachable alike -- clears the
 *    streak and returns to Good. In practice the mesh has usually already
 *    declared the peer lost for real by the time Unreachable would be
 *    reached, so that transition is exercised directly by the unit test
 *    rather than routinely observed live.
 */
#ifndef MDKR_MATCH_PEER_LIVENESS_H
#define MDKR_MATCH_PEER_LIVENESS_H

enum class MdkrPeerLivenessState {
    /* No streak: the most recent probe (if any) was answered. */
    Good = 0,
    /* 1..missesToUnreachable-1 consecutive misses: soft-fail. */
    Transient,
    /* missesToUnreachable consecutive misses: hard-fail. */
    Unreachable,
};

struct MdkrPeerLivenessTracker {
    MdkrPeerLivenessState state = MdkrPeerLivenessState::Good;
    unsigned consecutiveMisses = 0u;
    bool haveMeasurement = false;
};

/* One answered probe. Pure value transform: returns the tracker's next
 * state rather than mutating in place, matching this module's neighbours
 * (party_retry_policy.h's decision functions, session_core's next=*core). */
MdkrPeerLivenessTracker mdkr_peer_liveness_on_hit(
    MdkrPeerLivenessTracker tracker);

/* One probe interval elapsed with no answer. missesToUnreachable must be >=
 * 1; the caller derives it from the mesh's own ping/timeout ladder (see the
 * header comment above). Idempotent once saturated: calling this more than
 * missesToUnreachable times in a row is safe and leaves the tracker at
 * Unreachable with consecutiveMisses == missesToUnreachable. */
MdkrPeerLivenessTracker mdkr_peer_liveness_on_miss(
    MdkrPeerLivenessTracker tracker, unsigned missesToUnreachable);

#endif /* MDKR_MATCH_PEER_LIVENESS_H */
