#ifndef MDKR_ONLINE_CEREMONY_H
#define MDKR_ONLINE_CEREMONY_H

/* SEPARATED-BOOT-PATH (Strategy D2) native online champion CEREMONY screen.
 *
 * The LAST player-facing screen of the separated online flow: a native,
 * decomp-authentic 2D celebration of the cup champion, shown AFTER the final
 * STANDINGS and BEFORE the session hands back to the launcher. It is driven by
 * the online session (game/src/online/online_session.c) as its
 * MDKR_ONLINE_SESSION_CEREMONY phase, NOT by the offline menu state machine: it
 * deliberately does NOT enter the offline trophy-ceremony cinematic (which
 * re-enters GAMEMODE_MENU and is single-player-progression coupled). Like
 * online_results.c / online_charselect.c it borrows the game's decoded assets
 * (portraits + fonts) and the draw/SFX primitives, and owns all of its own state.
 *
 * ADVANCE MODEL (the load-bearing safety contract): the ceremony is a bounded,
 * TIMED auto-advance -- it fires on a fixed frame budget for EVERY endpoint
 * (host and joiner alike), with NO cross-endpoint / snapshot convergence gate, so
 * it is impossible to hang the session on. A host press may SKIP the hold early;
 * a joiner never blocks anyone. If a remote seat VACATES mid-ceremony it ends
 * promptly. The RESULTS terminal already took the one human "A: FINISH" confirm
 * (PD-T6d), so the ceremony adds NO second required gate (which would reopen the
 * joiner-parks-forever class T6d Important-1 fixed).
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

/* What one CEREMONY tick tells the session to do next. */
typedef enum MdkrOnlineCeremonyResult {
    MDKR_ONLINE_CEREMONY_STAY = 0, /* keep showing the celebration */
    MDKR_ONLINE_CEREMONY_ADVANCE,  /* the bounded hold ended (timer/host-skip): done */
    MDKR_ONLINE_CEREMONY_LEAVE     /* a remote vacated mid-ceremony: end promptly */
} MdkrOnlineCeremonyResult;

/* Load the screen's borrowed game assets (portraits + fonts) and reset the local
 * screen state. Reads the party_link snapshot ONCE here + runs the shared
 * standings sort (online_standings.h) to resolve the champion (order[0]), so the
 * ceremony's winner always agrees with the STANDINGS the RESULTS screen showed.
 * Symmetric with _exit(). */
void mdkr_online_ceremony_enter(void);

/* Free the borrowed portrait assets and fonts (mirrors online_results_exit).
 * Safe to call more than once (guarded), so a vacate-trip exit + the normal exit
 * cannot double-free. */
void mdkr_online_ceremony_exit(void);

/* One per-frame step: read the snapshot (for the vacate check + host affordance),
 * render the celebration, run the bounded auto-advance timer, and return STAY
 * until the celebration ends (timer elapsed / host skip / remote vacated). */
MdkrOnlineCeremonyResult mdkr_online_ceremony_tick(s32 updateRate);

/* True when the headless CEREMONY skip seam is armed (env
 * MDKR_TEST_ONLINE_CEREMONY_SKIP). Ordinary runs always return false. */
u8 mdkr_online_ceremony_test_active(void);

#ifdef __cplusplus
}
#endif

#endif /* MDKR_ENABLE_ONLINE_BETA */
#endif /* MDKR_ONLINE_CEREMONY_H */
