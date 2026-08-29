/* SEPARATED-BOOT-PATH (Strategy D2) shared native-screen draw/state helpers --
 * the implementation half of online_screen_util.h. See that header for the family
 * overview and the isolation rationale. Split into a real .c/.h pair (from the
 * former header-only grab-bag) so there is ONE definition of each helper and the
 * shared sky-asset table, not a per-TU copy.
 *
 * The ENTIRE TU is #if MDKR_ENABLE_ONLINE_BETA and it is added to the build only
 * inside the beta CMake gate (game/src/online/ is not globbed), so a normal (beta
 * OFF) build never compiles a byte of it and the release engine object is untouched.
 */
#include "online/online_screen_util.h"

#if MDKR_ENABLE_ONLINE_BETA

/* Game/PR headers FIRST: the header above pulls rcp_dkr.h -> ultra64.h ->
 * PR/os_libc.h, which declares sprintf/snprintf as plain functions; the system
 * <stdio.h> below MUST follow it (otherwise sprintf would already be a fortify
 * macro and os_libc's declaration would fail) -- the exact ordering the screen TUs
 * rely on. */
#include "audio.h"            /* music_play / music_current_sequence */
#include "sequence_ids.h"     /* SEQUENCE_MAIN_MENU */
#include "fade_transition.h"  /* transition_begin + FADE_TRANSITION */
#include "online/online_portraits.h" /* sOnlineToPortrait, sOnlineNames,
                                        MDKR_ONLINE_PORTRAIT_COUNT */

#include <stdio.h>
#include <string.h>

/* Which snapshot seat is the local one (display-only lookups). -1 when none. */
s32 mdkr_online_screen_local_seat(const MdkrPartyLinkSnapshot *snap) {
    unsigned i;
    for (i = 0u; i < MDKR_PARTY_LINK_SEATS; i++) {
        if (snap->seats[i].occupied && snap->seats[i].is_local) {
            return (s32) i;
        }
    }
    return -1;
}

/* Alpha of the body-text dark backing band. ~0.75 opaque near-black: strong enough
 * to read the light body faces over the BRIGHTEST world sky (Dino gold), still
 * translucent so the scrolling sky reads through it (a nameplate, not an opaque
 * box). Tuned against the bright/dark frame dumps. */
#define MDKR_ONLINE_TEXT_BAND_ALPHA 190

/* Draw text into the engine frame's display list with the given font + colour,
 * wrapped in a legibility SCRIM + a dark backing band for body text.
 *
 * The scrim is an 8-direction near-black halo drawn behind every glyph -- a
 * per-glyph dark backing that keeps body text crisp over even the brightest world
 * sky. The authentic scrolling skies washed out light body text on
 * trackselect/results even with a single 1px drop shadow; a full halo fixes it.
 * This is the retail "outline the font" technique, NOT a heavy opaque box, and the
 * halo lives in the SAME virtual coordinate space as the glyphs, so it can never
 * mis-register the way a framebuffer-space fill-rect panel would on the
 * aspect-scaled widescreen host.
 *
 * The compact body faces over the BRIGHT skies (Dino gold / Sherbet / Snowflake)
 * still washed out with the halo alone, worst on the pure-text MORE RACES chooser +
 * the charselect labels. BIGFONT (ASSET_FONTS_BIGFONT) carries its own thick dark
 * outline (the GAME SELECT face) and stays crisp over any sky -- online already uses
 * it for TITLES -- but it is ~24px tall and does not fit the dense option lists /
 * grid nameplates, so for the compact faces we add a genuine dark backing BAND: the
 * engine's OWN text-background fillrect (set_text_background_colour), which
 * render_text_string emits in the SAME glyph coordinate space (co-registers with the
 * text on the widescreen host, unlike a raw framebuffer fill-rect). BIGFONT
 * (self-outlined) skips the band so titles keep their exact retail look; every other
 * face (SMALLFONT / FUNFONT) gets it. The band is drawn ONCE (with the first halo
 * pass) then disabled, so it is a single flat plate, not nine stacked ones. */
void mdkr_online_screen_text(s32 x, s32 y, s32 fontId, char *text,
                             AlignmentFlags align, s32 r, s32 g, s32 b) {
    /* +-1 virtual unit == a clean thin halo at the menu font scale. */
    static const s32 ox[8] = {-1, 0, 1, -1, 1, -1, 0, 1};
    static const s32 oy[8] = {-1, -1, -1, 0, 0, 1, 1, 1};
    unsigned i;
    set_text_font(fontId);
    if (fontId != (s32) ASSET_FONTS_BIGFONT) {
        /* One dark backing band (a throwaway near-black glyph carries the fillrect
         * behind the whole string), then disable it for the halo/main passes. */
        set_text_background_colour(0, 0, 0, MDKR_ONLINE_TEXT_BAND_ALPHA);
        set_text_colour(0, 0, 0, 0, 255);
        draw_text(&gCurrDisplayList, x, y, text, align);
        set_text_background_colour(0, 0, 0, 0);
    } else {
        set_text_background_colour(0, 0, 0, 0);
        set_text_colour(0, 0, 0, 0, 255);
    }
    for (i = 0u; i < 8u; i++) {
        draw_text(&gCurrDisplayList, x + ox[i], y + oy[i], text, align);
    }
    set_text_colour(r, g, b, 0, 255);
    draw_text(&gCurrDisplayList, x, y, text, align);
}

/* Triangle-wave pulse 0..16 off a tick counter -- the shared native highlight /
 * heartbeat feel with zero assets. */
s32 mdkr_online_screen_pulse(u32 ticks) {
    s32 tri = (s32) (ticks & 31u);
    if (tri > 16) {
        tri = 32 - tri;
    }
    return tri;
}

/* Resolve one seat's short name: the untrusted snapshot name if present, else the
 * character's canonical name, else a "Pn" slot fallback. */
void mdkr_online_screen_seat_name(const MdkrPartyLinkSnapshot *snap, bool haveSnap,
                                  unsigned slot, char *out, size_t cap) {
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
u32 mdkr_online_screen_seconds_left(u32 done, u32 limit) {
    if (done >= limit) {
        return 0u;
    }
    return (limit - done + 59u) / 60u;
}

/* Blit a racer portrait (guarded: out-of-range id and unloaded texture are no-ops,
 * matching the faithful DrawTexture[] NULL-terminator scan). */
void mdkr_online_screen_draw_portrait(u8 character, s32 x, s32 y, u8 r, u8 g,
                                      u8 b) {
    DrawTexture *portrait;
    if (character >= MDKR_ONLINE_PORTRAIT_COUNT) {
        return;
    }
    portrait = gRacerPortraits[sOnlineToPortrait[character]];
    if (portrait != NULL && portrait[0].texture != NULL) {
        texrect_draw(&gCurrDisplayList, portrait, x, y, r, g, b, 255);
    }
}

/* Blit the REAL vehicle art (car / hovercraft / plane) the same way the charselect
 * grid blits the real racer portraits -- so the native vehicle screen shows the
 * vehicle pictures, not just text names. The tiles are the offline race-select's own
 * TOP+BOTTOM vehicle-icon pair (TEXTURE_ICON_VEHICLE_*_TOP/_BOTTOM), borrowed
 * READ-ONLY from gMenuAssets exactly as the portraits are. The caller loads the
 * group with menu_assetgroup_load(sOnlineVehicleAssetIds) and frees it on exit.
 * Centred horizontally on cx, TOP edge at topY, modulated by (r,g,b,a) so the card
 * can dim an illegal vehicle / tint the picked one. Returns FALSE (drawing nothing)
 * when the tiles are not resident, so the caller keeps its text label and this never
 * DMAs a NULL tile -- the same fail-safe the portrait blit and the sky backdrop
 * use. */
bool mdkr_online_screen_draw_vehicle(u8 vehicle, s32 cx, s32 topY, u8 r, u8 g,
                                     u8 b, u8 a) {
    static const s16 topId[3] = {
        TEXTURE_ICON_VEHICLE_CAR_TOP,
        TEXTURE_ICON_VEHICLE_HOVERCRAFT_TOP,
        TEXTURE_ICON_VEHICLE_PLANE_TOP,
    };
    static const s16 botId[3] = {
        TEXTURE_ICON_VEHICLE_CAR_BOTTOM,
        TEXTURE_ICON_VEHICLE_HOVERCRAFT_BOTTOM,
        TEXTURE_ICON_VEHICLE_PLANE_BOTTOM,
    };
    TextureHeader *tTop;
    TextureHeader *tBot;
    DrawTexture art[3];
    s32 halfW;

    if (vehicle > 2u) {
        return false;
    }
    tTop = (TextureHeader *) gMenuAssets[topId[vehicle]];
    tBot = (TextureHeader *) gMenuAssets[botId[vehicle]];
    if (tTop == NULL || tBot == NULL) {
        return false;
    }
    /* The offline pair anchors TOP at (0,0) and BOTTOM one tile-height below;
     * centre it on cx by offsetting each tile left by half its width. */
    halfW = (s32) tTop->width / 2;
    art[0].texture = tTop;
    art[0].xOffset = (s16) -halfW;
    art[0].yOffset = 0;
    art[1].texture = tBot;
    art[1].xOffset = (s16) -halfW;
    art[1].yOffset = (s16) tTop->height;
    art[2].texture = NULL;
    art[2].xOffset = 0;
    art[2].yOffset = 0;
    texrect_draw(&gCurrDisplayList, art, cx, topY, r, g, b, a);
    return true;
}

/* ======================================================================== *
 * Seamless phase transitions + menu ambiance (isolation-safe primitive borrows --
 * the SAME discipline the scrolling-sky backdrop uses for bgdraw).
 * ------------------------------------------------------------------------
 * The native online screens deliberately bypass the offline gCurrentMenuId
 * menu/transition state machine for isolation, so historically they HARD-CUT
 * between phases and the launcher->engine hand-off could show a black-frame jump.
 * These two helpers restore the retail FEEL without re-entering any offline loop.
 * ======================================================================== */

/* The reveal-from-black transition (retail menu cadence). FADE_FLAG_OUT drives the
 * black veil opacity 255 -> 0, i.e. the veil RECEDES to expose the screen -- the
 * engine's flag naming is inverted from the visual sense; this is menu.c's "fade the
 * freshly-entered screen in" transition (sMenuTransitionFadeOut). endTimer 0 so the
 * transition auto-clears once the screen is fully revealed. FADE_FULLSCREEN is
 * allocation-free, so firing it on every entry is cheap and needs no workspace. One
 * file-scope instance (transition_begin takes a non-const pointer). */
static FadeTransition sOnlineRevealTransition =
    FADE_TRANSITION(FADE_FULLSCREEN, FADE_FLAG_OUT, FADE_COLOR_BLACK, 18, 0);

/* REVEAL the screen from black. The engine's per-frame driver (transition_update()
 * + transition_render() at thread3_main.c:536, run every frame AFTER the gamemode
 * tick REGARDLESS of mode) then draws the receding black veil on top of whatever the
 * screen drew. This is the "already in the loop" primitive-borrow the backdrop leans
 * on for bgdraw_render(): we only ADD a transition_begin() call from the beta-gated
 * screens; the vanilla driver is untouched (so the OFF build is byte-identical). Each
 * screen calls this from its _enter(), so EVERY phase change -- and the first native
 * screen after the launcher hand-off -- fades up from black instead of snapping in. */
void mdkr_online_screen_fade_in_from_black(void) {
    transition_begin(&sOnlineRevealTransition);
}

/* Start / keep the retail menu music on the native menu-family screens via the
 * self-contained music_play() primitive (the exact call menu.c makes) -- NOT the
 * offline menu music state machine. Idempotent (music_current_sequence guards
 * against restarting it every entry). The race level loader replaces the sequence on
 * the RACE hand-off. */
void mdkr_online_screen_menu_music(void) {
    if (music_current_sequence() != (u8) SEQUENCE_MAIN_MENU) {
        music_play((u8) SEQUENCE_MAIN_MENU);
    }
}

/* ======================================================================== *
 * Retail scrolling-sky backdrop (shared, DRY across the four native screens)
 * ------------------------------------------------------------------------
 * Instead of a flat fill, the native online screens arm the engine's global
 * background to the retail two-texture horizontally-scrolling sky. The engine's
 * per-frame bgdraw_render() (thread3_main.c:411, run BEFORE the online tick) draws
 * whatever background mode is armed: once bgdraw_texture_init() has stashed a
 * TOP+BOTTOM tile pair, every subsequent frame renders the scrolling sky via
 * bgdraw_texture() -- the exact same primitive the offline front-end / post-race menu
 * use (rcp_dkr.c). We reuse the per-world sky tiles (TEXTURE_BACKGROUND_*_TOP/
 * _BOTTOM), loaded READ-ONLY through the same menu_assetgroup_load/free borrow the
 * screens already use (sOnlineSkyAssetIds below), and the offline TOP+BOTTOM pairing
 * + per-row shift table (menu.c gTracksMenuBgTextureIndices), mirrored here in the
 * SAME engine WORLD order (worldIdx == leveltable_world(mapId) - 1: Dino, Sherbet,
 * Snowflake, Dragon, Future Fun Land).
 *
 * LIFETIME: the armed sky points DIRECTLY at the borrowed TextureHeader*s. They
 * dangle the instant the group is freed, and bgdraw_render() would then DMA freed
 * memory. So every screen's _exit() MUST call mdkr_online_screen_backdrop_clear()
 * (disarm -> bgdraw_texture_init(NULL,...)) BEFORE menu_assetgroup_free() -- load and
 * free stay balanced and the next gamemode (RACE) re-arms its own background.
 * mdkr_online_screen_backdrop() itself fails safe to a flat fill if a tile is not
 * resident, so it never DMAs a NULL sky.
 *
 * The screen supplies a WORLD index; charselect uses the neutral hub sky, trackselect
 * the hovered world (a live retail preview), results/ceremony the raced world. Every
 * screen loads the SAME ten-tile group so it can arm any world without a per-frame
 * reload.
 * ======================================================================== */

/* Five worlds (Dino, Sherbet, Snowflake, Dragon, FFL) in engine WORLD order. */
#define MDKR_ONLINE_SKY_WORLD_COUNT 5u

/* The ten sky tiles (five worlds x TOP+BOTTOM) + the -1 terminator that
 * menu_assetgroup_load/free stop on. NON-const because the loader takes s16*. ONE
 * definition, shared by every screen via the extern in online_screen_util.h: a screen
 * loads the whole set even if it shows one world, so switching worlds is a cheap
 * re-arm (no reload). Same read-only borrow menu.c makes for the track menu. */
s16 sOnlineSkyAssetIds[] = {
    TEXTURE_BACKGROUND_DINO_DOMAIN_TOP,        TEXTURE_BACKGROUND_DINO_DOMAIN_BOTTOM,
    TEXTURE_BACKGROUND_SHERBERT_ISLAND_TOP,    TEXTURE_BACKGROUND_SHERBERT_ISLAND_BOTTOM,
    TEXTURE_BACKGROUND_SNOWFLAKE_MOUNTAIN_TOP, TEXTURE_BACKGROUND_SNOWFLAKE_MOUNTAIN_BOTTOM,
    TEXTURE_BACKGROUND_DRAGON_FOREST_TOP,      TEXTURE_BACKGROUND_DRAGON_FOREST_BOTTOM,
    TEXTURE_BACKGROUND_FUTURE_FUN_LAND_TOP,    TEXTURE_BACKGROUND_FUTURE_FUN_LAND_BOTTOM,
    -1,
};

/* Arm the engine background to the given world's scrolling sky. Requires the sky
 * group to be resident (menu_assetgroup_load(sOnlineSkyAssetIds)); binds the borrowed
 * TOP+BOTTOM tiles and the offline per-row shift. Fails SAFE to a flat fill if the
 * tiles are not resident, so it can never DMA a NULL sky. */
void mdkr_online_screen_backdrop(u8 skyWorld) {
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
        /* Borrow not resident: disarm the texture bg and fall back to the flat fill
         * rather than point bgdraw_render() at a NULL sky. */
        bgdraw_texture_init(NULL, NULL, 0u);
        bgdraw_fillcolour(16, 24, 48);
        return;
    }
    bgdraw_texture_init(t1, t2, shift[skyWorld]);
}

/* Disarm the scrolling sky (release the borrowed tile pointers). MUST run before
 * menu_assetgroup_free(sOnlineSkyAssetIds) so bgdraw_render() never DMAs a freed
 * tile. */
void mdkr_online_screen_backdrop_clear(void) {
    bgdraw_texture_init(NULL, NULL, 0u);
}

/* Cup id (cup display order: Dino, Snowflake, Sherbet, Dragon, FFL -- the
 * trackselect column / snapshot.cup_id order) -> sky WORLD index. */
u8 mdkr_online_screen_sky_world_for_cup(u8 cupId) {
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

/* Resolve the raced world's sky from the forward-feed snapshot (results / ceremony):
 * prefer the exact configured_track (engine-truth world), else the tournament cup,
 * else the neutral hub sky. */
u8 mdkr_online_screen_sky_world_for_snapshot(const MdkrPartyLinkSnapshot *snap,
                                             bool haveSnap) {
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

#endif /* MDKR_ENABLE_ONLINE_BETA */
