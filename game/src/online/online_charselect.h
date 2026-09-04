#ifndef MDKR_ONLINE_CHARSELECT_H
#define MDKR_ONLINE_CHARSELECT_H

/* SEPARATED-BOOT-PATH native online character-select screen.
 *
 * This is the FIRST player-facing screen of the separated online flow. It is
 * driven by the online session (game/src/online/online_session.c) as its
 * MDKR_ONLINE_SESSION_CHARSELECT phase, NOT by the offline menu state machine:
 * it deliberately does NOT call menu.c's charselect _loop. See the file header
 * in online_charselect.c for the full reuse boundary (what game assets it
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

/* What one CHARSELECT tick tells the session to do next. */
typedef enum MdkrOnlineCharselectResult {
    MDKR_ONLINE_CHARSELECT_STAY = 0, /* keep showing the screen */
    MDKR_ONLINE_CHARSELECT_ADVANCE,  /* authoritative lobby left LOBBY: move on */
    MDKR_ONLINE_CHARSELECT_LEAVE     /* local player backed all the way out */
} MdkrOnlineCharselectResult;

/* Load the screen's borrowed game assets (portraits + fonts are process-global)
 * and reset the local cursor/confirm/ready state. Symmetric with _exit(). */
void mdkr_online_charselect_enter(void);

/* Free the borrowed portrait assets (mirrors how menu.c frees a results-style
 * portrait group). Safe to call more than once. */
void mdkr_online_charselect_exit(void);

/* One per-frame step: read the party_link snapshot, apply local pad input to the
 * cursor/confirm/ready, publish the FULL local intent (continuously), and render
 * the native screen. Returns whether the session should stay, advance or leave. */
MdkrOnlineCharselectResult mdkr_online_charselect_tick(s32 updateRate);

/* True when the headless CHARSELECT test seam is armed (env
 * MDKR_TEST_ONLINE_CHARSELECT). Ordinary runs always return false. */
u8 mdkr_online_charselect_test_active(void);

/* true when the CHARSELECT cursor/confirm/ready is driven by SCRIPTED
 * input (env MDKR_TEST_ONLINE_CHARSELECT or MDKR_TEST_ONLINE_LOBBY_START) rather
 * than the live pad. The scripted script presses a browse-B at tick 3 as the I1
 * no-wedge coverage, which must NOT leave-to-room, so the session honors a
 * charselect backout as a genuine LEFT only for LIVE input (a real human) --
 * unless the dedicated backout seam overrides it. Ordinary runs return false. */
u8 mdkr_online_charselect_scripted_input_active(void);

/* True when the LOCAL player has locked a character AND readied on THIS screen
 * (the screen's own latch, not the lagging lobby snapshot). The session gates the
 * CHARSELECT -> TRACKSELECT hand-off on this so a live B-back (whose snapshot
 * ready flag lags un-ready by >=1 pump) cannot one-frame bounce straight back to
 * TRACKSELECT before the player can re-pick. Resets to false on _enter(). */
u8 mdkr_online_charselect_local_ready(void);

/* Headless test seam only (inert unless the env above is set): from LOBBY_WAIT,
 * install the party_link forward feed and publish a scripted 2-seat LOBBY room
 * whose local seat is occupied, so the session enters CHARSELECT. No-op in a
 * normal run, so it never disturbs the direct-boot / session-boot lanes. */
void mdkr_online_charselect_test_lobby_pump(void);

#ifdef __cplusplus
}
#endif

#endif /* MDKR_ENABLE_ONLINE_BETA */
#endif /* MDKR_ONLINE_CHARSELECT_H */
