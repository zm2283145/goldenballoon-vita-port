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

/* T4 -- the native "more races" chooser. After a race the terminal RESULTS/
 * STANDINGS screen no longer offers a binary continue/leave: the HOST picks one of
 * the full retail replay options and the session routes back to the right native
 * screen. Each option maps to the EXISTING party_link reverse-feed intents (REMATCH
 * + SET_MODE) already reduced by the launcher lobby -- no new reducer command:
 *   RACE_AGAIN     -> REMATCH (same config: single re-races the same track; a
 *                     finished tournament REMATCH resets the cup series to round 1)
 *   CHANGE_TRACK   -> REMATCH, then re-front native TRACKSELECT (host locks a new
 *                     single-race track over the SET_CONFIG_TRACK feed)
 *   CHANGE_CUP     -> REMATCH, then re-front native TRACKSELECT (host locks a new
 *                     cup over the SET_CUP feed -- SET_CUP resets the series)
 *   CHANGE_MODE    -> REMATCH + SET_MODE(toggled single<->tournament), re-front
 *                     TRACKSELECT
 *   NEW_TOURNAMENT -> REMATCH + SET_MODE(tournament), re-front TRACKSELECT (the
 *                     host locks a cup -> the series resets from round 1)
 *   CHANGE_CHAR    -> REMATCH, then re-front native CHARSELECT (-> VEHICLE ->
 *                     TRACKSELECT: change character + vehicle between races)
 *   FINISH         -> the LEAVE return (the session detours to the champion
 *                     CEREMONY for a finished tournament, else returns to the room)
 * JOINER_FOLLOW is not a host option: the joiner is display-only (it renders the
 * "more races" mirror + "waiting for host"), and once the host's authoritative
 * choice drives the room out of RESULTS the joiner reports this so the session
 * re-fronts its own native selection. */
typedef enum MdkrOnlineResultsChoice {
    MDKR_ONLINE_RESULTS_CHOICE_NONE = 0, /* not the chooser (the pre-existing paths) */
    MDKR_ONLINE_RESULTS_CHOICE_RACE_AGAIN,
    MDKR_ONLINE_RESULTS_CHOICE_CHANGE_TRACK,
    MDKR_ONLINE_RESULTS_CHOICE_CHANGE_CUP,
    MDKR_ONLINE_RESULTS_CHOICE_CHANGE_MODE,
    MDKR_ONLINE_RESULTS_CHOICE_NEW_TOURNAMENT,
    MDKR_ONLINE_RESULTS_CHOICE_CHANGE_CHAR,
    MDKR_ONLINE_RESULTS_CHOICE_FINISH,
    MDKR_ONLINE_RESULTS_CHOICE_JOINER_FOLLOW
} MdkrOnlineResultsChoice;

/* Load the screen's borrowed game assets (portraits + fonts) and reset the local
 * screen state. `isFinalRace` is the session's knowledge that no further race
 * will boot (the last cup round in the resident soak); it makes the STANDINGS
 * stage present the FINAL standings and hold (never auto-advance) instead of
 * counting down to the next race. `raceIndex` is this race's 0-based cup round,
 * used for the "RACE n/N" copy. Polls this race's captured placements exactly
 * once here. Symmetric with _exit().
 *
 * `chooserEnabled` arms the native "more races" chooser at the session decision
 * point (a single race's RESULTS, or a tournament's FINAL standings). The session
 * computes it: ON for real interactive play (and the dedicated chooser lane), OFF
 * for every pre-existing scripted/loopback lane -- so the terminal keeps its exact
 * historical behaviour (A: FINISH / joiner self-advance) whenever the chooser is
 * off, and only real play (and the T4 lane) gets the full replay menu. */
void mdkr_online_results_enter(u8 isFinalRace, u8 raceIndex, u8 chooserEnabled);

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

/* True when the dedicated "more races" chooser lane is armed (env
 * MDKR_TEST_ONLINE_RESULTS_CHOOSER). The session's chooser gate consults it so the
 * dedicated lane forces the chooser on even though it rides the RESIDENT soak.
 * Ordinary runs always return false. */
u8 mdkr_online_results_chooser_test_active(void);

/* After a RESULTS tick returns ADVANCE, the session reads which "more races"
 * option the host committed (or JOINER_FOLLOW for a joiner following the host's
 * authoritative choice) so it can route back to the correct native screen. Returns
 * MDKR_ONLINE_RESULTS_CHOICE_NONE for the pre-existing (non-chooser) ADVANCE paths
 * (the tournament round re-cycle / scripted soak), which the session must handle
 * exactly as before. */
MdkrOnlineResultsChoice mdkr_online_results_choice(void);

#ifdef __cplusplus
}
#endif

#endif /* MDKR_ENABLE_ONLINE_BETA */
#endif /* MDKR_ONLINE_RESULTS_H */
