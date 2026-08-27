#ifndef MDKR_ONLINE_SESSION_H
#define MDKR_ONLINE_SESSION_H

/* SEPARATED-BOOT-PATH (Strategy D) online session skeleton.
 *
 * This is the whole reason the online campaign can promise the battle-tested
 * OFFLINE game is provably unimpacted: online is a FULLY SEPARATE boot path.
 * mode_intro forks straight into GAMEMODE_ONLINE_SESSION (never the offline
 * menu state machine), the top-level dispatch in main_game_loop() routes that
 * mode to mdkr_online_session_tick(), and the session -- holding ALL of its own
 * state, never any offline global -- drives the flow to the race. The offline
 * GAMEMODE_MENU / gCurrentMenuId path is never entered on this route.
 *
 * The ENTIRE header is #if MDKR_ENABLE_ONLINE_BETA so a normal (beta OFF) build
 * sees nothing here, and the file is compiled into the engine ONLY under the
 * beta CMake gate (game/src/online/ is NOT auto-globbed). Include is safe
 * without the macro: the body just vanishes.
 */
#if MDKR_ENABLE_ONLINE_BETA

#include "types.h"
#include "thread3_main.h"                 /* GameMode, GAMEMODE_UNUSED_2 */
#include "net/match_launch_descriptor.h"  /* MdkrMatchLaunchDescriptorV1 */

#ifdef __cplusplus
extern "C" {
#endif

/* Beta alias for the online session mode. It REUSES the dead offline slot value
 * 2 (GAMEMODE_UNUSED_2) WITHOUT editing the shared GameMode enum in
 * thread3_main.h. The offline state machine never produces this value (verified:
 * nothing in game/src assigns GAMEMODE_UNUSED_2), so the online session mode is
 * ONLY ever reachable through the beta mode_intro fork -- the isolation
 * cornerstone. Defined here, in the beta-only header, not in the shared enum. */
#define GAMEMODE_ONLINE_SESSION GAMEMODE_UNUSED_2

/* The online session flow, held in session-owned state (never an offline
 * global). PD-T1 implements LOBBY_WAIT and the RACE hand-off only; the
 * remaining phases are the contract PD-T2..T6 build their screens on. */
typedef enum MdkrOnlineSessionPhase {
    MDKR_ONLINE_SESSION_LOBBY_WAIT = 0, /* read party_link; wait for boot signal */
    MDKR_ONLINE_SESSION_CHARSELECT,     /* PD-T2/T3: native character select */
    MDKR_ONLINE_SESSION_TRACKSELECT,    /* PD-T3: native track select */
    MDKR_ONLINE_SESSION_RACE,           /* hand off to the in-game race boot */
    MDKR_ONLINE_SESSION_RESULTS,        /* PD-T5: results */
    MDKR_ONLINE_SESSION_CEREMONY        /* PD-T6: tournament ceremony */
} MdkrOnlineSessionPhase;

/* Enter the separated online boot path. Called from the (already beta-gated)
 * mode_intro fork with the validated launch descriptor: stashes it in
 * session-owned state and sets gGameMode = GAMEMODE_ONLINE_SESSION. It does NOT
 * touch any offline menu state. */
void mdkr_online_session_begin(const MdkrMatchLaunchDescriptorV1 *launch);

/* Per-frame tick for GAMEMODE_ONLINE_SESSION, dispatched from main_game_loop().
 * PD-T1: LOBBY_WAIT idles on the party_link forward feed until the room leaves
 * selection, then hands off to the race boot (which sets GAMEMODE_INGAME). */
void mdkr_online_session_tick(s32 updateRate);

/* The direct online race boot (mdkr_online_boot_direct_race) was extracted in
 * PD-T4 into game/src/online/online_race_boot.{c,h}; online_session.c includes
 * that header and calls it for the RACE hand-off. */

#ifdef __cplusplus
}
#endif

#endif /* MDKR_ENABLE_ONLINE_BETA */
#endif /* MDKR_ONLINE_SESSION_H */
