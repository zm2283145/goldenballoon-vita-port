#ifndef MDKR_ONLINE_TRACKSELECT_H
#define MDKR_ONLINE_TRACKSELECT_H

/* SEPARATED-BOOT-PATH (Strategy D2) native online HOST TRACK / CUP select.
 *
 * This is the SECOND player-facing screen of the separated online flow, inserted
 * between the native CHARSELECT (online_charselect.{c,h}, PD-T2) and the race. It
 * is driven by the online session (game/src/online/online_session.c) as its
 * MDKR_ONLINE_SESSION_TRACKSELECT phase, NOT by the offline menu state machine:
 * it deliberately does NOT call menu.c's track-select _loop. See the file header
 * in online_trackselect.c for the full D2 reuse boundary (what game assets it
 * borrows vs. what it owns, and why it re-implements the presentation).
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

/* What one TRACKSELECT tick tells the session to do next. */
typedef enum MdkrOnlineTrackselectResult {
    MDKR_ONLINE_TRACKSELECT_STAY = 0, /* keep showing the screen */
    MDKR_ONLINE_TRACKSELECT_ADVANCE,  /* authoritative lobby left LOBBY: boot */
    MDKR_ONLINE_TRACKSELECT_LEAVE     /* B: back one level, to CHARSELECT */
} MdkrOnlineTrackselectResult;

/* Load the screen's borrowed game assets (per-world background textures + fonts
 * are process-global) and reset the local cursor/lock state. Symmetric with
 * _exit(). Seeds the vehicle from the local seat snapshot. */
void mdkr_online_trackselect_enter(void);

/* Free the borrowed world-background assets (mirrors how menu.c frees the track
 * menu's background group). Balances the fonts _enter() loaded. Safe to call more
 * than once. */
void mdkr_online_trackselect_exit(void);

/* One per-frame step: read the party_link snapshot, apply local pad input to the
 * cursor / mode toggle / track|cup lock / host start (host only), auto-narrow the
 * local vehicle to the resolved track's mask, publish the FULL local intent
 * (continuously -- the reducer clears all ready on any config change, so the
 * republish reconverges the lobby), and render the native screen. Returns whether
 * the session should stay, advance (boot) or leave (back to CHARSELECT). */
MdkrOnlineTrackselectResult mdkr_online_trackselect_tick(s32 updateRate);

/* True when the headless TRACKSELECT test seam is armed (env
 * MDKR_TEST_ONLINE_TRACKSELECT). Ordinary runs always return false. */
u8 mdkr_online_trackselect_test_active(void);

/* Headless test seam only (inert unless the env above is set): from LOBBY_WAIT,
 * install the party_link forward feed if nothing else has (the CHARSELECT seam
 * normally owns install in this lane). No-op in a normal run, so it never
 * disturbs the direct-boot / session-boot lanes. */
void mdkr_online_trackselect_test_lobby_pump(void);

#ifdef __cplusplus
}
#endif

#endif /* MDKR_ENABLE_ONLINE_BETA */
#endif /* MDKR_ONLINE_TRACKSELECT_H */
