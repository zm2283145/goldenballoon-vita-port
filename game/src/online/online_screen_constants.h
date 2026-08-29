#ifndef MDKR_ONLINE_SCREEN_CONSTANTS_H
#define MDKR_ONLINE_SCREEN_CONSTANTS_H

/* SEPARATED-BOOT-PATH (Strategy D2) shared native-screen constants.
 *
 * The native online SCREENS (charselect / trackselect / vehicleselect / results)
 * each used to re-#define the same handful of values with their own CS_/TS_/VS_/
 * RES_ prefix -- the launcher lobby id space, the virtual screen size, and the two
 * retail 2-player-narrowing track ids -- so the same number lived in up to four
 * places and could silently drift. They live here ONCE now, consumed by every
 * screen, so the shared vocabulary is guaranteed identical.
 *
 * The id-space values MIRROR platform/online/lobby_core.h (named in each comment):
 * net/party_link.h is deliberately dependency-free, so the engine screen TUs mirror
 * the few lobby_core.h constants they need rather than pull the launcher headers
 * into an engine TU. Kept in lock-step by the comments below.
 *
 * The ENTIRE header is #if MDKR_ENABLE_ONLINE_BETA so a normal (beta OFF) build
 * sees nothing here, and it is only ever included by the beta-gated online screen
 * TUs (game/src/online/ is NOT auto-globbed). Include is safe without the macro:
 * the body just vanishes.
 */
#if MDKR_ENABLE_ONLINE_BETA

/* Virtual screen space (SCREEN_WIDTH/HEIGHT live in camera.h/video.h; mirrored here
 * so the screen TUs do not pull those in just for two constants). */
#define MDKR_ONLINE_SCREEN_W 320
#define MDKR_ONLINE_SCREEN_W_HALF 160

/* Launcher lobby id-space mirrors (platform/online/lobby_core.h). */
#define MDKR_ONLINE_SCREEN_CHAR_COUNT 10u        /* MDKR_ONLINE_CHARACTER_COUNT */
#define MDKR_ONLINE_SCREEN_NO_CHARACTER 0xFFu    /* MDKR_ONLINE_NO_CHARACTER */
#define MDKR_ONLINE_SCREEN_NO_VEHICLE 0xFFu      /* MDKR_ONLINE_NO_VEHICLE */
#define MDKR_ONLINE_SCREEN_VEHICLE_COUNT 3u      /* car / hovercraft / plane (0x07 mask) */
#define MDKR_ONLINE_SCREEN_LOBBY_PHASE 1u        /* MDKR_ONLINE_LOBBY */
#define MDKR_ONLINE_SCREEN_RESULTS_PHASE 4u      /* MDKR_ONLINE_RESULTS */
#define MDKR_ONLINE_SCREEN_MODE_SINGLE 0u        /* MDKR_ONLINE_MODE_SINGLE_RACE */
#define MDKR_ONLINE_SCREEN_MODE_TOURNAMENT 1u    /* MDKR_ONLINE_MODE_TOURNAMENT */
#define MDKR_ONLINE_SCREEN_LOCAL_PAD 0           /* PLAYER_ONE */

/* Retail 2-player picker narrowing (menu.c menu_track_select V79+): the two tracks
 * whose usable-vehicle set shrinks at 2+ players. */
#define MDKR_ONLINE_SCREEN_TRACK_SPACEPORT_ALPHA 15u
#define MDKR_ONLINE_SCREEN_TRACK_FROSTY_VILLAGE 28u

#endif /* MDKR_ENABLE_ONLINE_BETA */
#endif /* MDKR_ONLINE_SCREEN_CONSTANTS_H */
