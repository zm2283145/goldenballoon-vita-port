#ifndef MDKR_ONLINE_SCREEN_UTIL_H
#define MDKR_ONLINE_SCREEN_UTIL_H

/* SEPARATED-BOOT-PATH (Strategy D2) shared native-screen draw/state helpers.
 *
 * The small helper families every native online SCREEN (charselect / trackselect /
 * vehicleselect / results / ceremony) needs, lifted DRY so the four screens can
 * never drift:
 *   - mdkr_online_screen_local_seat    which snapshot seat is the local player
 *   - mdkr_online_screen_text          font + colour + 1px drop-shadow text draw
 *   - mdkr_online_screen_panel/strip   the dark menu-board card / full-width band
 *                                      the text blocks sit on (retail figure-ground)
 *   - mdkr_online_screen_pulse         the 0..16 triangle-wave cursor/heartbeat
 *   - mdkr_online_screen_seat_name     one seat's short name (snapshot / char / Pn)
 *   - mdkr_online_screen_seconds_left  ceil of a 60ths-of-a-second countdown
 *   - mdkr_online_screen_draw_portrait the guarded racer-portrait blit
 *   - mdkr_online_screen_draw_vehicle  the guarded car/hover/plane art blit
 *   - fade_in_from_black / menu_music  seamless transitions + retail menu ambiance
 *   - backdrop / backdrop_clear        the shared scrolling-sky background
 *   - sky_world_for_cup / _for_snapshot cup/snapshot -> sky world index
 * Each was previously copy-pasted per screen; a single source (online_screen_util.c)
 * guarantees the shared visual/state vocabulary is byte-for-byte the same on every
 * screen. This header is the declarations; the definitions live in the .c.
 *
 * The ENTIRE header is #if MDKR_ENABLE_ONLINE_BETA so a normal (beta OFF) build sees
 * nothing here, and it is only ever included by the beta-gated online screen TUs
 * (game/src/online/ is NOT auto-globbed). online_screen_util.c is likewise added to
 * the build only inside the beta CMake gate.
 */
#if MDKR_ENABLE_ONLINE_BETA

#include "types.h"
#include "enums.h"          /* AlignmentFlags */
#include "menu.h"           /* DrawTexture (gRacerPortraits) */
#include "rcp_dkr.h"        /* Gfx (gCurrDisplayList) */
#include "net/party_link.h" /* MdkrPartyLinkSnapshot */

#include <stdbool.h>
#include <stddef.h> /* size_t */

#ifdef __cplusplus
extern "C" {
#endif

/* Engine globals the helpers reach without editing menu.c / game.c (all already have
 * external linkage). Declared here ONCE so the screen TUs and online_screen_util.c
 * share one set of decls. Authoritative definitions:
 *   gCurrDisplayList  thread3_main.c (the engine's live 2D frame list; also the list
 *                     menu_missing_controller() draws into)
 *   gRacerPortraits   menu.c (decoded racer portraits; gRacerPortraits[k] is a
 *                     DrawTexture[2] -- [0] the portrait, [1] the NULL terminator
 *                     texrect_draw stops on -- in its OWN order, see sOnlineToPortrait)
 *   gMenuAssets       menu.c (TextureHeader* per loaded TEXTURE_* id)
 *   leveltable_world  game.c (1-based world of a track; 0 == none) */
extern Gfx *gCurrDisplayList;
extern DrawTexture *gRacerPortraits[10];
extern void *gMenuAssets[128];
extern s8 leveltable_world(s32 mapId);

/* The ten sky tiles (five worlds x TOP+BOTTOM) + the -1 terminator that
 * menu_assetgroup_load/free stop on. ONE definition (online_screen_util.c); every
 * screen loads/frees this shared group. Non-const because the loader takes s16*. */
extern s16 sOnlineSkyAssetIds[];

/* Charselect's neutral/hub backdrop: Dino Domain's bright sky (world 0). */
#define MDKR_ONLINE_SKY_WORLD_NEUTRAL 0u

s32 mdkr_online_screen_local_seat(const MdkrPartyLinkSnapshot *snap);
void mdkr_online_screen_text(s32 x, s32 y, s32 fontId, char *text,
                             AlignmentFlags align, s32 r, s32 g, s32 b);
void mdkr_online_screen_panel(s32 x1, s32 y1, s32 x2, s32 y2);
void mdkr_online_screen_strip(s32 y1, s32 y2);
s32 mdkr_online_screen_pulse(u32 ticks);
void mdkr_online_screen_seat_name(const MdkrPartyLinkSnapshot *snap, bool haveSnap,
                                  unsigned slot, char *out, size_t cap);
u32 mdkr_online_screen_seconds_left(u32 done, u32 limit);
void mdkr_online_screen_draw_portrait(u8 character, s32 x, s32 y, u8 r, u8 g, u8 b);
bool mdkr_online_screen_draw_vehicle(u8 vehicle, s32 cx, s32 topY, u8 r, u8 g, u8 b,
                                     u8 a);
void mdkr_online_screen_fade_in_from_black(void);
void mdkr_online_screen_menu_music(void);
void mdkr_online_screen_backdrop(u8 skyWorld);
void mdkr_online_screen_backdrop_clear(void);
u8 mdkr_online_screen_sky_world_for_cup(u8 cupId);
u8 mdkr_online_screen_sky_world_for_snapshot(const MdkrPartyLinkSnapshot *snap,
                                             bool haveSnap);

#ifdef __cplusplus
}
#endif

#endif /* MDKR_ENABLE_ONLINE_BETA */
#endif /* MDKR_ONLINE_SCREEN_UTIL_H */
