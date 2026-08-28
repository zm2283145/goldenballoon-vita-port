#ifndef MDKR_ONLINE_SCREEN_UTIL_H
#define MDKR_ONLINE_SCREEN_UTIL_H

/* SEPARATED-BOOT-PATH (Strategy D2) shared native-screen draw/state helpers.
 *
 * The small helper families every native online SCREEN (charselect / trackselect
 * / results / ceremony) needs, lifted DRY so the four screens can never drift:
 *   - mdkr_online_screen_local_seat   which snapshot seat is the local player
 *   - mdkr_online_screen_text         font + colour + draw_text into the frame list
 *   - mdkr_online_screen_pulse        the 0..16 triangle-wave cursor/heartbeat
 *   - mdkr_online_screen_seat_name    one seat's short name (untrusted snapshot
 *                                     name, else the character name, else Pn)
 *   - mdkr_online_screen_seconds_left ceil of a 60ths-of-a-second countdown
 *   - mdkr_online_screen_draw_portrait the guarded racer-portrait blit
 * Each was previously copy-pasted per screen; a single source guarantees the
 * shared visual/state vocabulary is byte-for-byte the same on every screen.
 *
 * gCurrDisplayList (the engine's live 2D frame list) and gRacerPortraits[] (the
 * decoded racer faces) are declared here ONCE, so the text/portrait inlines can
 * reference them without each screen re-declaring the externs. Neither symbol is
 * exposed by menu.h; both already have external linkage in the engine, so this
 * reaches them without editing menu.c. gRacerPortraits[k] is a DrawTexture[2]:
 * element[0] the portrait, element[1] the NULL terminator texrect_draw stops on;
 * the array is in its OWN portrait order (see sOnlineToPortrait[]).
 *
 * The ENTIRE header is #if MDKR_ENABLE_ONLINE_BETA so a normal (beta OFF) build
 * sees nothing here, and it is only ever included by the beta-gated online screen
 * TUs (game/src/online/ is NOT auto-globbed). The helpers are static inline (each
 * TU gets its own copy -- no linkage change, no new object, no ODR risk), the
 * same discipline online_portraits.h / online_standings.h model.
 */
#if MDKR_ENABLE_ONLINE_BETA

/* Game/PR headers FIRST: rcp_dkr.h pulls ultra64.h -> PR/os_libc.h, which declares
 * sprintf/snprintf as plain functions; the system <stdio.h> below MUST follow it
 * (otherwise sprintf would already be a fortify macro and os_libc's declaration
 * would fail) -- the exact ordering the screen TUs rely on. */
#include "types.h"
#include "enums.h"      /* AlignmentFlags */
#include "menu.h"       /* font.h: set_text_font/colour, draw_text; DrawTexture */
#include "rcp_dkr.h"    /* texrect_draw */
#include "audio.h"      /* music_play / music_current_sequence -- primitive borrow */
#include "sequence_ids.h"    /* SEQUENCE_MAIN_MENU */
#include "fade_transition.h" /* transition_begin + FADE_TRANSITION -- primitive borrow */
#include "net/party_link.h"
#include "online/online_portraits.h" /* sOnlineToPortrait, sOnlineNames,
                                        MDKR_ONLINE_PORTRAIT_COUNT */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The engine's live 2D display list for the current frame, and the decoded racer
 * portraits -- see the header note above for why they are declared here. */
extern Gfx *gCurrDisplayList;
extern DrawTexture *gRacerPortraits[10];

/* The menu texture table (menu.h does not export it) and the engine-truth
 * track->world lookup, borrowed READ-ONLY for the shared scrolling-sky backdrop
 * below. Both already have external linkage in the engine, so this reaches them
 * without editing menu.c / game.c. gMenuAssets[k] holds a TextureHeader* for a
 * loaded TEXTURE_* id; leveltable_world(mapId) returns the 1-based world of a
 * track (0 == none). */
extern void *gMenuAssets[128];
extern s8 leveltable_world(s32 mapId);

/* Which snapshot seat is the local one (display-only lookups). -1 when none. */
static inline s32 mdkr_online_screen_local_seat(
    const MdkrPartyLinkSnapshot *snap) {
    unsigned i;
    for (i = 0u; i < MDKR_PARTY_LINK_SEATS; i++) {
        if (snap->seats[i].occupied && snap->seats[i].is_local) {
            return (s32) i;
        }
    }
    return -1;
}

/* Draw text into the engine frame's display list with the given font + colour,
 * wrapped in a legibility SCRIM (T7b).
 *
 * The scrim is an 8-direction near-black halo drawn behind every glyph -- a
 * per-glyph dark backing that keeps body text crisp over even the brightest
 * world sky. T7 flagged that the authentic scrolling skies washed out light body
 * text on trackselect/results even WITH the former single 1px drop shadow; a full
 * halo fixes it. This is the retail "outline the font" technique, NOT a heavy
 * opaque box, and it is deliberately built from proven draw_text calls only -- no
 * raw fill-rect microcode (the RDP-state hazard online_results.c's chooser text
 * documented) and, crucially, the halo lives in the SAME virtual coordinate space
 * as the glyphs, so it can never mis-register the way a framebuffer-space
 * fill-rect panel would on the aspect-scaled widescreen host. Applied here once so
 * EVERY native online screen's text (charselect / vehicle / track / results /
 * ceremony) pops uniformly over the busy sky (DRY -- the screens cannot drift). */
static inline void mdkr_online_screen_text(s32 x, s32 y, s32 fontId, char *text,
                                           AlignmentFlags align, s32 r, s32 g,
                                           s32 b) {
    /* +-1 virtual unit == a clean thin halo at the menu font scale (the same
     * offsets online_results.c's chooser proved legible over the brightest sky). */
    static const s32 ox[8] = {-1, 0, 1, -1, 1, -1, 0, 1};
    static const s32 oy[8] = {-1, -1, -1, 0, 0, 1, 1, 1};
    unsigned i;
    set_text_font(fontId);
    set_text_background_colour(0, 0, 0, 0);
    set_text_colour(0, 0, 0, 0, 255);
    for (i = 0u; i < 8u; i++) {
        draw_text(&gCurrDisplayList, x + ox[i], y + oy[i], text, align);
    }
    set_text_colour(r, g, b, 0, 255);
    draw_text(&gCurrDisplayList, x, y, text, align);
}

/* Triangle-wave pulse 0..16 off a tick counter -- the shared native highlight /
 * heartbeat feel with zero assets. */
static inline s32 mdkr_online_screen_pulse(u32 ticks) {
    s32 tri = (s32) (ticks & 31u);
    if (tri > 16) {
        tri = 32 - tri;
    }
    return tri;
}

/* Resolve one seat's short name: the untrusted snapshot name if present, else the
 * character's canonical name, else a "Pn" slot fallback. */
static inline void mdkr_online_screen_seat_name(
    const MdkrPartyLinkSnapshot *snap, bool haveSnap, unsigned slot, char *out,
    size_t cap) {
    if (haveSnap && slot < MDKR_PARTY_LINK_SEATS &&
        snap->seats[slot].occupied && snap->seats[slot].name[0] != '\0') {
        (void) snprintf(out, cap, "%.*s", (int) MDKR_PARTY_LINK_NAME_BYTES,
                        snap->seats[slot].name);
        return;
    }
    if (haveSnap && slot < MDKR_PARTY_LINK_SEATS &&
        snap->seats[slot].character_id < MDKR_ONLINE_PORTRAIT_COUNT) {
        (void) snprintf(out, cap, "%s",
                        sOnlineNames[snap->seats[slot].character_id]);
        return;
    }
    (void) snprintf(out, cap, "P%u", slot + 1u);
}

/* Countdown seconds still on the clock (ceil), 0 when elapsed. `done`/`limit` are
 * in 60ths of a second (updateRate accumulates at 60Hz). */
static inline u32 mdkr_online_screen_seconds_left(u32 done, u32 limit) {
    if (done >= limit) {
        return 0u;
    }
    return (limit - done + 59u) / 60u;
}

/* Blit a racer portrait (guarded: out-of-range id and unloaded texture are no-ops,
 * matching the faithful DrawTexture[] NULL-terminator scan). */
static inline void mdkr_online_screen_draw_portrait(u8 character, s32 x, s32 y,
                                                    u8 r, u8 g, u8 b) {
    DrawTexture *portrait;
    if (character >= MDKR_ONLINE_PORTRAIT_COUNT) {
        return;
    }
    portrait = gRacerPortraits[sOnlineToPortrait[character]];
    if (portrait != NULL && portrait[0].texture != NULL) {
        texrect_draw(&gCurrDisplayList, portrait, x, y, r, g, b, 255);
    }
}

/* ======================================================================== *
 * T7b: seamless phase transitions + menu ambiance (isolation-safe primitive
 * borrows -- the SAME discipline the scrolling-sky backdrop uses for bgdraw).
 * ------------------------------------------------------------------------
 * The native online screens deliberately bypass the offline gCurrentMenuId
 * menu/transition state machine for isolation, so historically they HARD-CUT
 * between phases and the launcher->engine hand-off could show a black-frame jump.
 * These two helpers restore the retail FEEL without re-entering any offline loop:
 *
 *   - mdkr_online_screen_fade_in_from_black(): arms the engine's OWN transition
 *     system (transition_begin) to REVEAL the screen from black over ~0.3s. The
 *     engine's per-frame driver -- transition_update() + transition_render() at
 *     thread3_main.c:536, run every frame AFTER the gamemode tick REGARDLESS of
 *     mode -- then draws the receding black veil on top of whatever the screen
 *     drew. This is the exact "already in the loop" primitive-borrow the backdrop
 *     leans on for bgdraw_render(): we only ADD a transition_begin() call from the
 *     beta-gated screens; the vanilla driver is untouched (so the OFF build is
 *     byte-identical). Each screen calls this from its _enter(), so EVERY phase
 *     change -- and the very first native screen after the launcher hand-off --
 *     fades up from black instead of snapping in. Cadence matches the retail menu
 *     (menu.c sMenuTransitionFadeOut: FADE_FULLSCREEN black, 18 frames).
 *     FADE_FULLSCREEN is allocation-free, so firing it on every entry is cheap and
 *     needs no transition workspace.
 *
 *   - mdkr_online_screen_menu_music(): starts (idempotently) the retail menu music
 *     sequence via the self-contained music_play() primitive -- the exact call
 *     menu.c makes (menu.c:4642) -- NOT the offline menu music state machine. The
 *     select / results screens were SFX-only; this gives them the retail menu
 *     ambiance. The race level loader replaces the sequence on the RACE hand-off.
 * ======================================================================== */

/* REVEAL the screen from black (retail menu cadence). FADE_FLAG_OUT drives the
 * black veil opacity 255 -> 0, i.e. the veil RECEDES to expose the screen -- the
 * engine's flag naming is inverted from the visual sense; this is menu.c's "fade
 * the freshly-entered screen in" transition (sMenuTransitionFadeOut). endTimer 0
 * so the transition auto-clears once the screen is fully revealed. */
static inline void mdkr_online_screen_fade_in_from_black(void) {
    static FadeTransition reveal =
        FADE_TRANSITION(FADE_FULLSCREEN, FADE_FLAG_OUT, FADE_COLOR_BLACK, 18, 0);
    transition_begin(&reveal);
}

/* Start / keep the retail menu music on the native menu-family screens. Idempotent
 * (music_current_sequence guards against restarting it every entry). */
static inline void mdkr_online_screen_menu_music(void) {
    if (music_current_sequence() != (u8) SEQUENCE_MAIN_MENU) {
        music_play((u8) SEQUENCE_MAIN_MENU);
    }
}

/* ======================================================================== *
 * Retail scrolling-sky backdrop (shared, DRY across the four native screens)
 * ------------------------------------------------------------------------
 * Instead of a flat fill, the native online screens arm the engine's global
 * background to the retail two-texture horizontally-scrolling sky. The engine's
 * per-frame bgdraw_render() (thread3_main.c:411, run BEFORE the online tick)
 * draws whatever background mode is armed: once bgdraw_texture_init() has stashed
 * a TOP+BOTTOM tile pair, every subsequent frame renders the scrolling sky via
 * bgdraw_texture() -- the exact same primitive the offline front-end / post-race
 * menu use (rcp_dkr.c). We reuse:
 *   - the per-world sky tiles (TEXTURE_BACKGROUND_*_TOP/_BOTTOM), loaded READ-ONLY
 *     through the same menu_assetgroup_load/free borrow the screens already use
 *     (sOnlineSkyAssetIds below);
 *   - the offline TOP+BOTTOM pairing + per-row shift table (menu.c
 *     gTracksMenuBgTextureIndices), mirrored here in the SAME engine WORLD order
 *     (worldIdx == leveltable_world(mapId) - 1: Dino, Sherbet, Snowflake, Dragon,
 *     Future Fun Land).
 *
 * LIFETIME (R6): the armed sky points DIRECTLY at the borrowed TextureHeader*s.
 * They dangle the instant the group is freed, and bgdraw_render() would then DMA
 * freed memory. So every screen's _exit() MUST call mdkr_online_screen_backdrop_
 * clear() (disarm -> bgdraw_texture_init(NULL,...)) BEFORE menu_assetgroup_free()
 * -- load and free stay balanced and the next gamemode (RACE) re-arms its own
 * background. mdkr_online_screen_backdrop() itself fails safe to a flat fill if a
 * tile is not resident, so it never DMAs a NULL sky.
 *
 * The screen supplies a WORLD index; charselect uses the neutral hub sky, track-
 * select the hovered world (a live retail preview), results/ceremony the raced
 * world (resolved from the forward-feed snapshot). Every screen loads the SAME
 * ten-tile group so it can arm any world without a per-frame reload.
 * ======================================================================== */

/* Five worlds (Dino, Sherbet, Snowflake, Dragon, FFL) in engine WORLD order. */
#define MDKR_ONLINE_SKY_WORLD_COUNT 5u
/* Charselect's neutral/hub backdrop: Dino Domain's bright sky (world 0). */
#define MDKR_ONLINE_SKY_WORLD_NEUTRAL 0u

/* The ten sky tiles (five worlds x TOP+BOTTOM) + the -1 terminator that
 * menu_assetgroup_load/free stop on. NON-const because that loader takes s16* and
 * each TU owns its own copy (the sPortraitAssetIds discipline). A screen loads the
 * whole set even if it only shows one world, so switching worlds is a cheap
 * re-arm (no reload). Same read-only borrow menu.c makes for the track menu. */
static s16 sOnlineSkyAssetIds[] = {
    TEXTURE_BACKGROUND_DINO_DOMAIN_TOP,        TEXTURE_BACKGROUND_DINO_DOMAIN_BOTTOM,
    TEXTURE_BACKGROUND_SHERBERT_ISLAND_TOP,    TEXTURE_BACKGROUND_SHERBERT_ISLAND_BOTTOM,
    TEXTURE_BACKGROUND_SNOWFLAKE_MOUNTAIN_TOP, TEXTURE_BACKGROUND_SNOWFLAKE_MOUNTAIN_BOTTOM,
    TEXTURE_BACKGROUND_DRAGON_FOREST_TOP,      TEXTURE_BACKGROUND_DRAGON_FOREST_BOTTOM,
    TEXTURE_BACKGROUND_FUTURE_FUN_LAND_TOP,    TEXTURE_BACKGROUND_FUTURE_FUN_LAND_BOTTOM,
    -1,
};

/* Arm the engine background to the given world's scrolling sky. Requires the
 * sky group to be resident (menu_assetgroup_load(sOnlineSkyAssetIds)); binds the
 * borrowed TOP+BOTTOM tiles and the offline per-row shift. Fails SAFE to a flat
 * fill if the tiles are not resident, so it can never DMA a NULL sky. */
static inline void mdkr_online_screen_backdrop(u8 skyWorld) {
    /* WORLD order (worldIdx == leveltable_world-1), mirroring menu.c
     * gTracksMenuBgTextureIndices {TOP, BOTTOM, per-row shift}. */
    static const s16 top[MDKR_ONLINE_SKY_WORLD_COUNT] = {
        TEXTURE_BACKGROUND_DINO_DOMAIN_TOP,
        TEXTURE_BACKGROUND_SHERBERT_ISLAND_TOP,
        TEXTURE_BACKGROUND_SNOWFLAKE_MOUNTAIN_TOP,
        TEXTURE_BACKGROUND_DRAGON_FOREST_TOP,
        TEXTURE_BACKGROUND_FUTURE_FUN_LAND_TOP,
    };
    static const s16 bottom[MDKR_ONLINE_SKY_WORLD_COUNT] = {
        TEXTURE_BACKGROUND_DINO_DOMAIN_BOTTOM,
        TEXTURE_BACKGROUND_SHERBERT_ISLAND_BOTTOM,
        TEXTURE_BACKGROUND_SNOWFLAKE_MOUNTAIN_BOTTOM,
        TEXTURE_BACKGROUND_DRAGON_FOREST_BOTTOM,
        TEXTURE_BACKGROUND_FUTURE_FUN_LAND_BOTTOM,
    };
    static const u32 shift[MDKR_ONLINE_SKY_WORLD_COUNT] = {
        0x00u, 0x20u, 0x00u, 0x20u, 0x20u,
    };
    TextureHeader *t1;
    TextureHeader *t2;

    if (skyWorld >= MDKR_ONLINE_SKY_WORLD_COUNT) {
        skyWorld = (u8) MDKR_ONLINE_SKY_WORLD_NEUTRAL;
    }
    t1 = (TextureHeader *) gMenuAssets[top[skyWorld]];
    t2 = (TextureHeader *) gMenuAssets[bottom[skyWorld]];
    if (t1 == NULL) {
        /* Borrow not resident: disarm the texture bg and fall back to the flat
         * fill rather than point bgdraw_render() at a NULL sky. */
        bgdraw_texture_init(NULL, NULL, 0u);
        bgdraw_fillcolour(16, 24, 48);
        return;
    }
    bgdraw_texture_init(t1, t2, shift[skyWorld]);
}

/* Disarm the scrolling sky (release the borrowed tile pointers). MUST run before
 * menu_assetgroup_free(sOnlineSkyAssetIds) so bgdraw_render() never DMAs a freed
 * tile. */
static inline void mdkr_online_screen_backdrop_clear(void) {
    bgdraw_texture_init(NULL, NULL, 0u);
}

/* Cup id (cup display order: Dino, Snowflake, Sherbet, Dragon, FFL -- the
 * trackselect column / snapshot.cup_id order) -> sky WORLD index. */
static inline u8 mdkr_online_screen_sky_world_for_cup(u8 cupId) {
    static const u8 cupToWorld[MDKR_ONLINE_SKY_WORLD_COUNT] = {
        0u, /* cup0 Dino      -> world 0 */
        2u, /* cup1 Snowflake -> world 2 */
        1u, /* cup2 Sherbet   -> world 1 */
        3u, /* cup3 Dragon    -> world 3 */
        4u, /* cup4 FFL       -> world 4 */
    };
    return (cupId < MDKR_ONLINE_SKY_WORLD_COUNT) ? cupToWorld[cupId]
                                                 : (u8) MDKR_ONLINE_SKY_WORLD_NEUTRAL;
}

/* Resolve the raced world's sky from the forward-feed snapshot (results /
 * ceremony): prefer the exact configured_track (engine-truth world), else the
 * tournament cup, else the neutral hub sky. */
static inline u8 mdkr_online_screen_sky_world_for_snapshot(
    const MdkrPartyLinkSnapshot *snap, bool haveSnap) {
    if (haveSnap) {
        if (snap->configured_track != 0xFFFFu) {
            s8 world = leveltable_world((s32) snap->configured_track);
            if (world >= 1 && world <= (s8) MDKR_ONLINE_SKY_WORLD_COUNT) {
                return (u8) (world - 1);
            }
        }
        if (snap->cup_id < MDKR_ONLINE_SKY_WORLD_COUNT) {
            return mdkr_online_screen_sky_world_for_cup(snap->cup_id);
        }
    }
    return (u8) MDKR_ONLINE_SKY_WORLD_NEUTRAL;
}

#ifdef __cplusplus
}
#endif

#endif /* MDKR_ENABLE_ONLINE_BETA */
#endif /* MDKR_ONLINE_SCREEN_UTIL_H */
