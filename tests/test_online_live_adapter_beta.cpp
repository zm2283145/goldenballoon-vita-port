/* Beta-ON unit test for the live adapter's race-end-card truthfulness decisions.
 *
 * P1-T2 F4: no other test compiles platform/online/match_live_adapter.cpp WITH
 * MDKR_ENABLE_ONLINE_BETA (the app target does; mdkr_online_live_adapter_test
 * deliberately does NOT -- defining the macro there breaks
 * test_token_gate_required). That test hole is exactly why the F1 demotion bug
 * shipped. This SEPARATE, beta-ON target pins the two pure decisions that only
 * exist under the beta macro:
 *   - mapLostReason's in-race truthfulness branches (a peer that leaves after
 *     the race is racing is OPPONENT_LEFT / a mid-race transport breakdown is
 *     CONNECTION_UNPLAYABLE; pre-connection losses keep the old copy), and
 *   - the F1 no-demotion rule (the drain's reason-blind OPPONENT_LEFT must not
 *     overwrite an already-latched, more-specific CONNECTION_UNPLAYABLE).
 *
 * Both are exposed as pure test-only wrappers so this stays a fast, mesh-free
 * unit test. The two engine-boot-handoff hooks the beta build of the adapter
 * references live in the heavy wiring TU; stub them here so we need not link it.
 */
#include "online/match_live_adapter.h"

#include <cstdio>

/* Beta build references these (resetRaceLatches / setUpRace boot handoff; the
 * PD-T6h2c ~LiveAdapter destructor also retracts the room-ready handoff); the
 * decisions under test never invoke them, so no-op stubs suffice. */
void OnlineRoom_publishEngineRaceBoot(IMdkrOnlineAdapter *) {}
void OnlineRoom_retractEngineRaceBoot(IMdkrOnlineAdapter *) {}
void OnlineRoom_retractEngineRoomReady(IMdkrOnlineAdapter *) {}

static int g_failures = 0;
static int g_checks = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        ++g_checks;                                                            \
        if (!(cond)) {                                                         \
            ++g_failures;                                                      \
            std::fprintf(stderr, "CHECK failed: %s (%s:%d)\n", #cond,          \
                         __FILE__, __LINE__);                                  \
        }                                                                      \
    } while (0)

static MdkrOnlineViewFailure mapLost(MdkrMatchPeerLostReason r, bool raceBegun) {
    return mdkr_online_live_adapter_test_map_lost_reason(r, raceBegun);
}

/* mapLostReason: in-race losses tell a truthful, DIFFERENT story than the
 * pre-connection copy, and a mid-race transport breakdown is distinct from an
 * opponent leaving. */
static void test_map_lost_reason_in_race_branches() {
    /* Once the race is up, a peer that pings-out or ends "disconnected". */
    CHECK(mapLost(MdkrMatchPeerLostReason::PingTimeout, true) ==
          MDKR_ONLINE_VIEW_FAILURE_OPPONENT_LEFT);
    CHECK(mapLost(MdkrMatchPeerLostReason::PeerEnded, true) ==
          MDKR_ONLINE_VIEW_FAILURE_OPPONENT_LEFT);
    /* A seal-window exhaustion mid-race is a transport breakdown, NOT "opponent
     * left" and NOT "could not establish". */
    CHECK(mapLost(MdkrMatchPeerLostReason::SealWindowExhausted, true) ==
          MDKR_ONLINE_VIEW_FAILURE_CONNECTION_UNPLAYABLE);

    /* Pre-connection: unchanged establishment copy. */
    CHECK(mapLost(MdkrMatchPeerLostReason::PingTimeout, false) ==
          MDKR_ONLINE_VIEW_FAILURE_CONNECTION_CHECK);
    CHECK(mapLost(MdkrMatchPeerLostReason::PeerEnded, false) ==
          MDKR_ONLINE_VIEW_FAILURE_CONNECTION_CHECK);
    CHECK(mapLost(MdkrMatchPeerLostReason::SealWindowExhausted, false) ==
          MDKR_ONLINE_VIEW_FAILURE_CONNECTION_CHECK);

    /* Reason classes independent of raceBegun stay put. */
    CHECK(mapLost(MdkrMatchPeerLostReason::ConnectTimeout, true) ==
          MDKR_ONLINE_VIEW_FAILURE_NETWORKS_CANNOT_CONNECT);
    CHECK(mapLost(MdkrMatchPeerLostReason::ConnectTimeout, false) ==
          MDKR_ONLINE_VIEW_FAILURE_NETWORKS_CANNOT_CONNECT);
    CHECK(mapLost(MdkrMatchPeerLostReason::TransportFailed, true) ==
          MDKR_ONLINE_VIEW_FAILURE_NETWORKS_CANNOT_CONNECT);
    CHECK(mapLost(MdkrMatchPeerLostReason::CommitmentMismatch, true) ==
          MDKR_ONLINE_VIEW_FAILURE_VERIFICATION_MISMATCH);
    CHECK(mapLost(MdkrMatchPeerLostReason::ControlChannelViolation, false) ==
          MDKR_ONLINE_VIEW_FAILURE_VERIFICATION_MISMATCH);
}

/* F1: the drain reports a reason-blind OpponentLeft (it only polls
 * race_peer_lost()), so setRaceEndFailure(OPPONENT_LEFT) must NOT demote a
 * more-specific CONNECTION_UNPLAYABLE the mesh already latched -- otherwise a
 * transport breakdown is mislabeled "your opponent lost connection". */
static void test_race_end_no_demotion_rule() {
    /* The one case that must be kept. */
    CHECK(mdkr_online_live_adapter_test_race_end_demotes(
        MDKR_ONLINE_VIEW_FAILURE_OPPONENT_LEFT,
        MDKR_ONLINE_VIEW_FAILURE_CONNECTION_UNPLAYABLE));
    /* OPPONENT_LEFT over a plain in-race latch (or none) still applies. */
    CHECK(!mdkr_online_live_adapter_test_race_end_demotes(
        MDKR_ONLINE_VIEW_FAILURE_OPPONENT_LEFT,
        MDKR_ONLINE_VIEW_FAILURE_OPPONENT_LEFT));
    CHECK(!mdkr_online_live_adapter_test_race_end_demotes(
        MDKR_ONLINE_VIEW_FAILURE_OPPONENT_LEFT,
        MDKR_ONLINE_VIEW_FAILURE_NONE));
    /* The barrier-abort card (OPPONENT_NEVER_STARTED) is not the reason-blind
     * one and never demotes -- it always sets its own copy. */
    CHECK(!mdkr_online_live_adapter_test_race_end_demotes(
        MDKR_ONLINE_VIEW_FAILURE_OPPONENT_NEVER_STARTED,
        MDKR_ONLINE_VIEW_FAILURE_CONNECTION_UNPLAYABLE));
    /* Applying the specific card over a generic one is never a demotion. */
    CHECK(!mdkr_online_live_adapter_test_race_end_demotes(
        MDKR_ONLINE_VIEW_FAILURE_CONNECTION_UNPLAYABLE,
        MDKR_ONLINE_VIEW_FAILURE_OPPONENT_LEFT));
}

/* Whole-phase regression: a FIRST-race SAS mismatch drives re-verify ->
 * re-confirm -> SELECTING -> BEGIN_LOADING WITHOUT ever running
 * resetRaceLatches (the belt-and-braces reset only fires once race latches are
 * armed, which they are not before any loading). So a peer-loss latched during
 * the mismatch teardown must be cleared by the re-verify entry points
 * themselves; otherwise the re-confirmed race's start barrier reads a stale
 * race_peer_lost(), aborts, and kicks the HEALTHY peer with a false "Your
 * Opponent Couldn't Start" card. Cover both entry points and both latch
 * sources (racePeerLost_ and the folded-in received-abort latch). */
static void test_reverify_paths_clear_stale_peer_loss() {
    CHECK(!mdkr_online_live_adapter_test_rekey_clears_peer_loss(false));
    CHECK(!mdkr_online_live_adapter_test_rekey_clears_peer_loss(true));
    CHECK(!mdkr_online_live_adapter_test_reverify_clears_peer_loss(false));
    CHECK(!mdkr_online_live_adapter_test_reverify_clears_peer_loss(true));
}

int main() {
    test_map_lost_reason_in_race_branches();
    test_race_end_no_demotion_rule();
    test_reverify_paths_clear_stale_peer_loss();
    std::fprintf(stderr, "online_live_adapter_beta: %d checks, %d failures\n",
                 g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
