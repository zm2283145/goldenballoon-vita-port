#ifndef MDKR_ONLINE_RESULTS_H
#define MDKR_ONLINE_RESULTS_H

/* SEPARATED-BOOT-PATH (Strategy D2) native online RESULTS / STANDINGS screen.
 *
 * The screen shown AFTER an online race, driven by the online session
 * (game/src/online/online_session.c) as its MDKR_ONLINE_SESSION_RESULTS phase,
 * NOT by the offline menu state machine: it deliberately does NOT call menu.c's
 * MENU_RESULTS loop (which carries a settings->racers[].placements side-effect).
 * See the file header in online_results.c for the full D2 reuse boundary (what
 * game assets it borrows vs. what it owns).
 *
 * It reads THIS race's finishing order from mdkr_online_race_results_poll (the
 * engine-captured placements) and the cup points table from the party_link
 * forward-feed snapshot (points[]/last_placements/race_index/mode). It NEVER
 * mutates settings->racers[].trophy_points and never engine-accumulates points.
 *
 * The ENTIRE header is #if MDKR_ENABLE_ONLINE_BETA so a normal (beta OFF) build
 * sees nothing here, and the TU is compiled into the engine ONLY under the beta
 * CMake gate (game/src/online/ is NOT auto-globbed). Include is safe without the
 * macro: the body just vanishes.
 */
#if MDKR_ENABLE_ONLINE_BETA

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* What one RESULTS tick tells the session to do next. */
typedef enum MdkrOnlineResultsResult {
    MDKR_ONLINE_RESULTS_STAY = 0, /* keep showing the screen */
    MDKR_ONLINE_RESULTS_ADVANCE,  /* countdown fired / host advanced: move on */
    MDKR_ONLINE_RESULTS_LEAVE     /* local player backed out (the LEFT return) */
} MdkrOnlineResultsResult;

/* Load the screen's borrowed game assets (portraits + fonts) and reset the local
 * screen state. `isFinalRace` is the session's knowledge that no further race
 * will boot (the last cup round in the resident soak); it makes the STANDINGS
 * stage present the FINAL standings and hold (never auto-advance) instead of
 * counting down to the next race. `raceIndex` is this race's 0-based cup round,
 * used for the "RACE n/N" copy. Polls this race's captured placements exactly
 * once here. Symmetric with _exit(). */
void mdkr_online_results_enter(u8 isFinalRace, u8 raceIndex);

/* Free the borrowed portrait assets and fonts. Safe to call more than once. */
void mdkr_online_results_exit(void);

/* One per-frame step: read the party_link snapshot (points/mode), apply the
 * local pad (host advance / joiner watch-only), run the visible countdown, and
 * render the native screen. Returns whether the session should stay, advance or
 * leave. */
MdkrOnlineResultsResult mdkr_online_results_tick(s32 updateRate);

/* True when the headless RESULTS test seam is armed (env MDKR_TEST_ONLINE_RESIDENT).
 * Ordinary runs always return false. */
u8 mdkr_online_results_test_active(void);

#ifdef __cplusplus
}
#endif

#endif /* MDKR_ENABLE_ONLINE_BETA */
#endif /* MDKR_ONLINE_RESULTS_H */
