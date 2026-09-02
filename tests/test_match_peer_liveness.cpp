/*
 * mdkr_peer_liveness_on_hit / mdkr_peer_liveness_on_miss -- the pure A7
 * soft-fail/hard-fail liveness policy. See platform/online/match_peer_liveness.h
 * for the full policy writeup; this drives it through a scripted miss/hit
 * sequence and pins the player-facing copy for each failing state.
 *
 * Positive control: naiveImmediateEscalation() reproduces the pre-A7 shape
 * (any single miss from a previously-good peer escalates straight to
 * Unreachable, with no intermediate soft-fail step). Applied to the exact
 * same first-miss observation the real policy is asserted against, it fails
 * the "stays Transient" property outright -- proving that assertion is not
 * vacuous.
 */
#include "online/match_peer_liveness.h"

/* Assert-driven test: NDEBUG would compile every check away. */
#undef NDEBUG

#include <cassert>
#include <cstring>

namespace {

/* The pre-A7 shape this task replaces: immediate, binary escalation. A
 * peer that never had a good measurement is still not this policy's
 * business (mirrors the real policy's own carve-out); any miss from one
 * that did is Unreachable at once, with nothing in between. */
MdkrPeerLivenessState naiveImmediateEscalation(bool hadMeasurement, bool missed) {
    if (!hadMeasurement) return MdkrPeerLivenessState::Good;
    return missed ? MdkrPeerLivenessState::Unreachable
                  : MdkrPeerLivenessState::Good;
}

/* A miss observed before any peer has ever answered a probe is the setup
 * ladder's business, not this policy's: the tracker stays untouched. */
void missBeforeFirstHitLeavesTrackerAtGood() {
    MdkrPeerLivenessTracker tracker;
    tracker = mdkr_peer_liveness_on_miss(tracker, /*missesToUnreachable=*/3u);
    assert(tracker.state == MdkrPeerLivenessState::Good);
    assert(tracker.consecutiveMisses == 0u);
    assert(!tracker.haveMeasurement);
}

/* A hit always lands Good with the fresh measurement retained. */
void hitLandsGoodWithMeasurement() {
    MdkrPeerLivenessTracker tracker;
    tracker = mdkr_peer_liveness_on_hit(tracker, 42u);
    assert(tracker.state == MdkrPeerLivenessState::Good);
    assert(tracker.consecutiveMisses == 0u);
    assert(tracker.haveMeasurement);
    assert(tracker.lastRttMs == 42u);
}

/* The scripted miss/hit sequence: a previously-good peer's first missed
 * probe keeps showing the last measurement, labelled Transient; only the
 * Nth consecutive miss escalates to Unreachable; a later hit recovers to
 * Good with the fresh measurement, clearing the streak. */
void scriptedMissHitSequenceMatchesPolicy() {
    constexpr unsigned kN = 3u;  /* matches match_live_adapter.cpp's derived
                                  * kMdkrPeerLivenessMissesToUnreachable for
                                  * the shipped 5000/15000 ms ladder. */
    MdkrPeerLivenessTracker tracker;

    /* Peer answers its first probe: Good, 40 ms retained. */
    tracker = mdkr_peer_liveness_on_hit(tracker, 40u);
    assert(tracker.state == MdkrPeerLivenessState::Good);

    /* Miss 1: soft-fail. Last measurement (40 ms) is still there. */
    tracker = mdkr_peer_liveness_on_miss(tracker, kN);
    assert(tracker.state == MdkrPeerLivenessState::Transient);
    assert(tracker.consecutiveMisses == 1u);
    assert(tracker.lastRttMs == 40u);

    /* Positive control: the pre-A7 immediate-escalation shape does NOT
     * agree with the assertion just above -- applied to this exact first
     * miss, it lands Unreachable, not Transient, which proves the
     * assertion above actually discriminates the fixed shape from the old
     * one instead of passing regardless. */
    const MdkrPeerLivenessState oldShapeAfterFirstMiss =
        naiveImmediateEscalation(/*hadMeasurement=*/true, /*missed=*/true);
    assert(oldShapeAfterFirstMiss == MdkrPeerLivenessState::Unreachable);
    assert(oldShapeAfterFirstMiss != MdkrPeerLivenessState::Transient);

    /* Miss 2: still soft-fail, still 40 ms, still one policy away from
     * Unreachable. */
    tracker = mdkr_peer_liveness_on_miss(tracker, kN);
    assert(tracker.state == MdkrPeerLivenessState::Transient);
    assert(tracker.consecutiveMisses == 2u);
    assert(tracker.lastRttMs == 40u);

    /* A hit recovers mid-streak instead of only from Good. */
    MdkrPeerLivenessTracker recovered = mdkr_peer_liveness_on_hit(tracker, 48u);
    assert(recovered.state == MdkrPeerLivenessState::Good);
    assert(recovered.consecutiveMisses == 0u);
    assert(recovered.lastRttMs == 48u);

    /* Miss 3 (the Nth): escalates to Unreachable -- the same wall-clock
     * budget (N * interval == timeout) the mesh's own PingTimeout uses. */
    tracker = mdkr_peer_liveness_on_miss(tracker, kN);
    assert(tracker.state == MdkrPeerLivenessState::Unreachable);
    assert(tracker.consecutiveMisses == 3u);
    assert(tracker.lastRttMs == 40u);

    /* Further misses saturate rather than overflowing the streak. */
    tracker = mdkr_peer_liveness_on_miss(tracker, kN);
    assert(tracker.state == MdkrPeerLivenessState::Unreachable);
    assert(tracker.consecutiveMisses == 3u);

    /* Recovery even from Unreachable: a later hit is still a hit. */
    tracker = mdkr_peer_liveness_on_hit(tracker, 55u);
    assert(tracker.state == MdkrPeerLivenessState::Good);
    assert(tracker.consecutiveMisses == 0u);
    assert(tracker.lastRttMs == 55u);
}

/* missesToUnreachable == 1 is a legal, degenerate ladder (soft-fail is
 * skipped entirely): the very first miss is already the Nth. */
void singleMissLadderEscalatesImmediately() {
    MdkrPeerLivenessTracker tracker;
    tracker = mdkr_peer_liveness_on_hit(tracker, 10u);
    tracker = mdkr_peer_liveness_on_miss(tracker, /*missesToUnreachable=*/1u);
    assert(tracker.state == MdkrPeerLivenessState::Unreachable);
    assert(tracker.consecutiveMisses == 1u);
}

/* Player copy: pinned strings for the two failing states; Good has none of
 * its own (the caller keeps its existing healthy-state text). */
void playerCopyMatchesTheBrief() {
    assert(mdkr_peer_liveness_status_text(MdkrPeerLivenessState::Good) == nullptr);
    assert(std::strcmp(
        mdkr_peer_liveness_status_text(MdkrPeerLivenessState::Transient),
        "Connection hiccup — retrying") == 0);
    assert(std::strcmp(
        mdkr_peer_liveness_status_text(MdkrPeerLivenessState::Unreachable),
        "Connection lost") == 0);
}

}  // namespace

int main() {
    missBeforeFirstHitLeavesTrackerAtGood();
    hitLandsGoodWithMeasurement();
    scriptedMissHitSequenceMatchesPolicy();
    singleMissLadderEscalatesImmediately();
    playerCopyMatchesTheBrief();
    return 0;
}
