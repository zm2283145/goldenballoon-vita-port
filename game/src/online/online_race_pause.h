#ifndef MDKR_ONLINE_RACE_PAUSE_H
#define MDKR_ONLINE_RACE_PAUSE_H

/* NON-BLOCKING online race pause (the MK8 model).
 *
 * In an online rollback race the sim NEVER stops. The retail START pause
 * (gIsPaused + menu_pause_init) runs INSIDE the networked simulation: a START
 * press reaches the canonical input stream of BOTH machines, and the paused
 * state makes mdkr_game_resimulate_tick refuse every later rollback-correction
 * replay (admission rejects a paused game; completion rejects a replay that
 * ENGAGES the pause) -- the two-machine beta crash: the non-pausing peer
 * abort()ed the instant the host's START edge landed as a correction.
 *
 * So, online:
 *   1. the retail pause ENGAGE is suppressed deterministically on every
 *      endpoint (mdkr_online_race_pause_suppressed, keyed only on the online
 *      canonical-input runtime being active -- identical live and in resim,
 *      identical on both machines, constant for the whole race);
 *   2. START instead opens a LOCAL, presentation-only overlay menu
 *      (CONTINUE / LEAVE RACE) rendered over the still-running race. Desktop
 *      keeps driving on live pad input; Vita publishes neutral local input
 *      while its overlay is visible so menu navigation cannot steer the kart.
 *      The overlay writes nothing the rollback authority snapshots, so
 *      corrections replay identically whether the overlay is open or not.
 *
 * The ENTIRE header is #if MDKR_ENABLE_ONLINE_BETA and the TU is compiled only
 * inside the beta CMake gate, so a normal (beta OFF) build sees none of this
 * and the offline pause behaviour is byte-identical (thread3_main.c's hooks are
 * likewise #if-gated).
 */
#if MDKR_ENABLE_ONLINE_BETA || \
    (defined(__vita__) && defined(MDKR_ENABLE_VITA_LAN))

#include "types.h"
#if defined(__vita__) && defined(MDKR_ENABLE_VITA_LAN)
#include "session/session_bridge.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* True while the online canonical-input runtime drives the race: the retail
 * START pause must not engage (mode_game's pause branch, live AND resim). */
s32 mdkr_online_race_pause_suppressed(void);

/* Per-frame overlay service, called from main_game_loop AFTER the mode
 * dispatch (never during resimulation -- resim re-enters mode_game directly),
 * so it reads this frame's canonical local-seat edges and draws over the
 * completed HUD. Inert unless an online race is live. */
void mdkr_online_race_overlay_frame(s32 updateRate);
#if defined(__vita__) && defined(MDKR_ENABLE_VITA_LAN)
void mdkr_online_race_pause_filter_local_input(MdkrPadSample *input);
void mdkr_online_race_pause_close(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* online beta or Vita LAN */
#endif /* MDKR_ONLINE_RACE_PAUSE_H */
