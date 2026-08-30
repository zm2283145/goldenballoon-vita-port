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
#include "textures_sprites.h" /* rendermode_reset (panel fill-state restore) */
#include "online/online_portraits.h" /* sOnlineToPortrait, sOnlineNames,
                                        MDKR_ONLINE_PORTRAIT_COUNT */
#include "online/online_screen_constants.h" /* MDKR_ONLINE_SCREEN_W (strip width) */
#include "fast3d/gfx_pc_dkr.h" /* gfx_dkr_font_display_hd_set (HD display-face
                                  derivation latch; same header font.c already
                                  uses for gfx_dkr_font_texture_register) */

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

/* ======================================================================== *
 * High-definition text (see online_screen_util.h for the player-facing
 * contract). A refcount rather than a boolean: RESULTS -> CEREMONY (and any
 * future screen chain) may overlap _enter/_exit in either order, and the
 * renderer latch must only drop when NO online screen is up. The count lives
 * here (one TU) so every screen shares the same latch discipline.
 * ======================================================================== */
static u32 sHdTextRefs = 0u;

void mdkr_online_screen_hd_text_ref(void) {
    sHdTextRefs++;
    gfx_dkr_font_display_hd_set(true);
}

void mdkr_online_screen_hd_text_unref(void) {
    if (sHdTextRefs > 0u) {
        sHdTextRefs--;
    }
    gfx_dkr_font_display_hd_set(sHdTextRefs != 0u);
}

/* BELT: force the latch OFF and zero the refcount for a FRESH session. A
 * watchdog exit (platform_request_exit -- the resident return-to-room fired by
 * the RESULTS rematch-hold / FINISH wrap-hold watchdogs in online_session.c) can
 * bypass a screen's _exit; the per-break _exit calls are the buckle for that, but
 * if any future exit path ever slips one, the refcount would strand at >0 and the
 * SDF display-face latch would stay ON for every subsequent same-process render.
 * mdkr_online_session_begin calls this at session start so a new session can NEVER
 * inherit a stale latch, whatever happened to the previous one. */
void mdkr_online_screen_hd_text_reset(void) {
    sHdTextRefs = 0u;
    gfx_dkr_font_display_hd_set(false);
}

/* Draw text into the engine frame's display list with the given font + colour.
 *
 * Presentation contract (the retail-menu discipline): body text NEVER floats
 * naked over the bright scrolling skies -- it sits on a dark panel/strip drawn
 * by mdkr_online_screen_panel/_strip below (figure-ground comes from LAYOUT,
 * exactly like the retail options/pause boards). The old per-string treatment
 * (dark band + 8-direction halo + face = 10 draw_text passes) smeared the small
 * ROM glyphs into unreadable blobs AND overran the frame display list on the
 * dense chooser (8354 of 7000 Gfx commands); it is deliberately gone.
 *
 * What remains per string: ONE 1px drop shadow (crisp, retail-style depth cue)
 * + ONE face pass. BIGFONT carries its own authored thick outline (the GAME
 * SELECT face), so it skips even the shadow and keeps its exact retail look.
 * Glyphs render at the authored size only (draw_text has no scale parameter;
 * scale is always 1.0) -- integer-crisp, never fractionally resampled.
 *
 * COLOUR (root-cause note): set_text_colour's 4th argument is the ENV-ALPHA
 * blend factor of the text combiner (G_CC_BLENDT_ENV_ALPHA_A_TxP: colour =
 * lerp(TEXEL, ENV, envA)) -- NOT an unused alpha. The old halo stack passed 0
 * there, so every one of its 9 "dark" passes actually drew the RAW WHITE
 * glyph texel: nine overlapping white copies WAS the owner-reported blur, and
 * none of the screens' colour vocabulary ever reached the frame. 255 applies
 * the requested colour fully; the shadow uses retail's own translucent-black
 * shadow recipe (menu.c draws its shadows at opacity 180). BIGFONT and
 * FUNFONT are AUTHORED-COLOUR faces (the gold title art / the rainbow trophy
 * digits): they keep envA 0 so their authored art shows untinted, exactly as
 * retail renders them. */
void mdkr_online_screen_text(s32 x, s32 y, s32 fontId, char *text,
                             AlignmentFlags align, s32 r, s32 g, s32 b) {
    bool authoredFace = (fontId == (s32) ASSET_FONTS_BIGFONT) ||
                        (fontId == (s32) ASSET_FONTS_FUNFONT);
    set_text_font(fontId);
    set_text_background_colour(0, 0, 0, 0);
    /* KERNING (owner "above reproach" polish): the ROM font's authored per-glyph
     * advances read airy at the aspect-scaled host size ("DRA GON", "BE GIN").
     * set_kerning(TRUE) tightens body/list glyphs by 1px each (the exact public
     * API menu.c uses for the Snowflake hub name + the pak menu). Applied ONLY to
     * the ROM body faces (SMALLFONT / SUBTITLEFONT) -- the authored-art BIGFONT /
     * FUNFONT headers keep their own designed spacing (they are excluded from the
     * authoredFace kern just as they are from the tint/shadow). The shadow and
     * face passes share the same kerning so they stay registered; state is
     * restored to the module default (FALSE) before returning. */
    if (!authoredFace) {
        set_kerning(TRUE);
    }
    if (fontId != (s32) ASSET_FONTS_BIGFONT) {
        set_text_colour(0, 0, 0, 255, 180);
        draw_text(&gCurrDisplayList, x + 1, y + 1, text, align);
    }
    if (authoredFace) {
        set_text_colour(r, g, b, 0, 255); /* authored art, untinted */
    } else {
        set_text_colour(r, g, b, 255, 255);
    }
    draw_text(&gCurrDisplayList, x, y, text, align);
    if (!authoredFace) {
        set_kerning(FALSE);
    }
}

/* ======================================================================== *
 * Panels (retail menu-board style: dark rounded quad + subtle border)
 * ------------------------------------------------------------------------
 * The screens group their text blocks on these boards so light text always has
 * a solid dark ground, whatever the sky behind. Drawn with the FONT module's
 * own flat-fill vocabulary -- dDialogueBoxBegin + dDialogueBoxDrawModes[1]
 * (env-colour XLU fill) + fillrects -- i.e. the exact command sequence
 * render_dialogue_box() uses for the retail dialogue boards, so the quads live
 * in the same logical 320x240 space as the glyph texrects and co-register with
 * the text on the aspect-scaled host. State is reset afterwards the same way
 * the font module resets it (pipesync + rendermode_reset).
 * ======================================================================== */

/* Board palette: near-black navy fill (dark enough for white body text over
 * the brightest Dino sand, still translucent so the scrolling sky reads
 * through) + a muted tan 1px border (subtle, NOT the selection gold). */
#define MDKR_ONLINE_PANEL_FILL_R 0
#define MDKR_ONLINE_PANEL_FILL_G 0
#define MDKR_ONLINE_PANEL_FILL_B 20
#define MDKR_ONLINE_PANEL_FILL_A 208
#define MDKR_ONLINE_PANEL_EDGE_R 132
#define MDKR_ONLINE_PANEL_EDGE_G 112
#define MDKR_ONLINE_PANEL_EDGE_B 64
#define MDKR_ONLINE_PANEL_EDGE_A 176
#define MDKR_ONLINE_STRIP_FILL_A 168

/* Engine-owned draw-mode lists the flat fills borrow (authoritative
 * definitions: font.c; external linkage, same borrow discipline as the
 * gRacerPortraits/gMenuAssets decls above). */
extern Gfx dDialogueBoxBegin[];
extern Gfx dDialogueBoxDrawModes[][2];

/* Arm the font module's flat env-colour fill (dialogue-board vocabulary). */
static void online_screen_fill_begin(s32 r, s32 g, s32 b, s32 a) {
    gSPDisplayList(gCurrDisplayList++, dDialogueBoxBegin);
    gDkrDmaDisplayList(gCurrDisplayList++,
                       OS_K0_TO_PHYSICAL(dDialogueBoxDrawModes[1]), 2);
    gDPSetEnvColor(gCurrDisplayList++, r, g, b, a);
}

/* Restore neutral render state exactly the way the font module does. */
static void online_screen_fill_end(void) {
    gDPPipeSync(gCurrDisplayList++);
    rendermode_reset(&gCurrDisplayList);
    gDPPipeSync(gCurrDisplayList++);
}

/* One rounded (2px-notched) dialogue-box quad + subtle 1px border, fill AND edge
 * colours parameterised. (x1,y1)-(x2,y2) inclusive-ish logical 320x240 coords.
 * ~14 Gfx commands total. This is the ONE box vocabulary the screens share: the
 * navy menu-board (mdkr_online_screen_panel below) and the retail RANKINGS blue
 * dialogue box (online_results.c results_blue_box) are the SAME command sequence,
 * differing only in colour -- previously copy-pasted per TU (the byte-for-byte
 * results_blue_box duplicate + a second dDialogueBox* re-extern), now one source. */
void mdkr_online_screen_box(s32 x1, s32 y1, s32 x2, s32 y2, s32 fr, s32 fg, s32 fb,
                           s32 fa, s32 er, s32 eg, s32 eb, s32 ea) {
    /* Fill: three non-overlapping strips (top cap / middle / bottom cap) so the
     * translucent fill never double-blends. */
    online_screen_fill_begin(fr, fg, fb, fa);
    render_fill_rectangle(&gCurrDisplayList, x1 + 2, y1, x2 - 2, y1 + 2);
    render_fill_rectangle(&gCurrDisplayList, x1, y1 + 2, x2, y2 - 2);
    render_fill_rectangle(&gCurrDisplayList, x1 + 2, y2 - 2, x2 - 2, y2);
    /* Border: four 1px edge lines over the fill's rim. */
    gDPPipeSync(gCurrDisplayList++);
    gDPSetEnvColor(gCurrDisplayList++, er, eg, eb, ea);
    render_fill_rectangle(&gCurrDisplayList, x1 + 2, y1, x2 - 2, y1 + 1);
    render_fill_rectangle(&gCurrDisplayList, x1 + 2, y2 - 1, x2 - 2, y2);
    render_fill_rectangle(&gCurrDisplayList, x1, y1 + 2, x1 + 1, y2 - 2);
    render_fill_rectangle(&gCurrDisplayList, x2 - 1, y1 + 2, x2, y2 - 2);
    online_screen_fill_end();
}

/* The near-black navy menu-board card (byte-identical to the former inline body --
 * now the shared box with the navy palette). */
void mdkr_online_screen_panel(s32 x1, s32 y1, s32 x2, s32 y2) {
    mdkr_online_screen_box(x1, y1, x2, y2, MDKR_ONLINE_PANEL_FILL_R,
                           MDKR_ONLINE_PANEL_FILL_G, MDKR_ONLINE_PANEL_FILL_B,
                           MDKR_ONLINE_PANEL_FILL_A, MDKR_ONLINE_PANEL_EDGE_R,
                           MDKR_ONLINE_PANEL_EDGE_G, MDKR_ONLINE_PANEL_EDGE_B,
                           MDKR_ONLINE_PANEL_EDGE_A);
}

/* Full-width borderless band (title strip / footer ground): the ONE place text
 * may overlay the scene directly, so it gets a lighter flat band rather than a
 * bordered board. */
void mdkr_online_screen_strip(s32 y1, s32 y2) {
    online_screen_fill_begin(MDKR_ONLINE_PANEL_FILL_R, MDKR_ONLINE_PANEL_FILL_G,
                             MDKR_ONLINE_PANEL_FILL_B, MDKR_ONLINE_STRIP_FILL_A);
    render_fill_rectangle(&gCurrDisplayList, 0, y1, MDKR_ONLINE_SCREEN_W, y2);
    online_screen_fill_end();
}

/* One small solid card (retail P1/P2 seat-number block): flat fill + a 1px darker
 * border for definition, drawn with the SAME font-module fill vocabulary the panels
 * use so it co-registers with the glyph texrects on the aspect-scaled host. */
void mdkr_online_screen_card(s32 x1, s32 y1, s32 x2, s32 y2, s32 r, s32 g, s32 b,
                             s32 a) {
    online_screen_fill_begin(r, g, b, a);
    render_fill_rectangle(&gCurrDisplayList, x1, y1, x2, y2);
    /* border: a darker rim over the fill's edge (half the fill's channels). */
    gDPPipeSync(gCurrDisplayList++);
    gDPSetEnvColor(gCurrDisplayList++, r / 3, g / 3, b / 3, a);
    render_fill_rectangle(&gCurrDisplayList, x1, y1, x2, y1 + 1);
    render_fill_rectangle(&gCurrDisplayList, x1, y2 - 1, x2, y2);
    render_fill_rectangle(&gCurrDisplayList, x1, y1, x1 + 1, y2);
    render_fill_rectangle(&gCurrDisplayList, x2 - 1, y1, x2, y2);
    online_screen_fill_end();
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

/* Retail selected-item blink level 0..255. menu.c drives its selected-option text
 * blend and the racer-count red pulse from gOptionBlinkTimer = (t + updateRate) &
 * 0x3F, value*8 triangle-waved. `timer` is the caller's already-accumulated
 * (t + updateRate) & 0x3F value (0..63); fold it to a 0..32 triangle, *8, clamp. */
s32 mdkr_online_screen_blink(u32 timer) {
    s32 v = (s32) (timer & 0x3Fu);
    if (v > 32) {
        v = 64 - v;
    }
    v *= 8;
    if (v > 255) {
        v = 255;
    }
    return v;
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

/* ======================================================================== *
 * Borrowed-tile texrect blits (DRY across the screens' hand-rolled art draws)
 * ------------------------------------------------------------------------
 * trackselect (arrows / wood frame / sky postcard) and vehicleselect (PLAYER-n
 * label / word art / wood frame) each hand-rolled a DrawTexture dt[2] +
 * texrect_draw(_scaled) blit of a borrowed gMenuAssets tile. These two helpers
 * build the dt[2] (tile + NULL terminator) once, keeping the SAME null/dims
 * fail-safe guards those call sites carried, so a not-resident borrow is a no-op
 * (never a NULL-tile DMA) exactly as before. draw_portrait / draw_vehicle above
 * already wrap the two portrait/vehicle cases; these wrap the generic tile case.
 * ======================================================================== */

/* Plain 1:1 texrect blit of one borrowed tile with its top-left at (x,y),
 * rgba-modulated. No-op if the tile is not resident. */
void mdkr_online_screen_blit(TextureHeader *tex, s32 x, s32 y, u8 r, u8 g, u8 b,
                             u8 a) {
    DrawTexture dt[2];
    if (tex == NULL) {
        return;
    }
    dt[0].texture = tex;
    dt[0].xOffset = 0;
    dt[0].yOffset = 0;
    dt[1].texture = NULL;
    dt[1].xOffset = 0;
    dt[1].yOffset = 0;
    texrect_draw(&gCurrDisplayList, dt, x, y, r, g, b, a);
}

/* Scaled texrect blit of one borrowed tile at (x,y) with per-axis scale + a packed
 * colour (COLOUR_RGBA32, computed by the caller). No-op if the tile is not resident
 * OR reports zero dims (the callers divide by width/height to derive the scale, so
 * a zero-dim tile would divide-by-zero -- the exact guard they carried inline). */
void mdkr_online_screen_blit_scaled(TextureHeader *tex, f32 x, f32 y, f32 sx,
                                    f32 sy, u32 rgba) {
    DrawTexture dt[2];
    if (tex == NULL || tex->width == 0 || tex->height == 0) {
        return;
    }
    dt[0].texture = tex;
    dt[0].xOffset = 0;
    dt[0].yOffset = 0;
    dt[1].texture = NULL;
    dt[1].xOffset = 0;
    dt[1].yOffset = 0;
    texrect_draw_scaled(&gCurrDisplayList, dt, x, y, sx, sy, rgba, 0);
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

/* One-shot latch: skip the next reveal fade (armed by the session for the
 * intra-track-screen stage flips, which retail presents as ONE screen). */
static u8 sOnlineFadeSkipOnce;

void mdkr_online_screen_fade_skip_once(void) {
    sOnlineFadeSkipOnce = 1u;
}

/* REVEAL the screen from black. The engine's per-frame driver (transition_update()
 * + transition_render() at thread3_main.c:536, run every frame AFTER the gamemode
 * tick REGARDLESS of mode) then draws the receding black veil on top of whatever the
 * screen drew. This is the "already in the loop" primitive-borrow the backdrop leans
 * on for bgdraw_render(): we only ADD a transition_begin() call from the beta-gated
 * screens; the vanilla driver is untouched (so the OFF build is byte-identical). Each
 * screen calls this from its _enter(), so EVERY phase change -- and the first native
 * screen after the launcher hand-off -- fades up from black instead of snapping in. */
void mdkr_online_screen_fade_in_from_black(void) {
    if (sOnlineFadeSkipOnce) {
        sOnlineFadeSkipOnce = 0u;
        return;
    }
    transition_begin(&sOnlineRevealTransition);
}

/* The EXIT fade (retail sMenuTransitionFadeIn): FADE_FLAG_NONE drives the black veil
 * opacity 0 -> 255 (it GROWS to cover the outgoing screen) and FADE_STAY holds it
 * black until the incoming screen's reveal (fade_in_from_black, which transition_end
 * s this one first) takes over -- so a phase hand-off is fade-to-black-then-from-
 * black, exactly like a retail menu switch, instead of a hard cut. The session fires
 * this, then holds the phase switch MDKR_ONLINE_SCREEN_EXIT_FADE_TICKS ticks (the
 * duration) so the outgoing screen keeps rendering under the growing veil, THEN runs
 * _exit + switch + the incoming _enter's reveal. Same isolation-safe primitive borrow
 * as the reveal (the OFF build never compiles this TU). */
static FadeTransition sOnlineExitTransition =
    FADE_TRANSITION(FADE_FULLSCREEN, FADE_FLAG_NONE, FADE_COLOR_BLACK,
                    MDKR_ONLINE_SCREEN_EXIT_FADE_TICKS, FADE_STAY);

void mdkr_online_screen_fade_out_to_black(void) {
    transition_begin(&sOnlineExitTransition);
}

/* Cancel a still-black exit fade and reveal the current screen again -- the session
 * calls this if a hand-off it started fading toward is abandoned mid-fade (e.g. the
 * local seat un-readies), so the black veil is never stranded. Bypasses the
 * skip-once latch (an abort must always clear the veil). */
void mdkr_online_screen_fade_cancel_to_reveal(void) {
    transition_begin(&sOnlineRevealTransition);
}

/* ======================================================================== *
 * Display-list retire (the freed-texture-still-referenced crash fix)
 * ------------------------------------------------------------------------
 * texrect_draw() (rcp_dkr.c) references every blitted tile by EMBEDDING a
 * gDkrDmaDisplayList(tex->cmd, ...) pointer INTO the texture allocation, and the
 * authored frame list is consumed by the gfx task asynchronously (double-buffered
 * gSPTaskNum). A screen transition that frees its texture groups mid-tick --
 * AFTER the outgoing screen already drew this frame -- therefore leaves the
 * frame's list (and possibly the still-in-flight previous task) pointing at
 * freed memory, which the incoming screen's loads immediately reuse: the task
 * walker then interprets texture pixel bytes as commands ("[DL] unknown
 * display-list opcode" spew, intermittent SEGV -- reproduced on the
 * single-race-replay lane at the first screen transitions). The engine's own
 * unload path (unload_level_game, thread3_main.c) shows the required discipline:
 * wait out the in-flight task, then truncate the authored list to a trivial
 * FullSync+End so no consumer ever walks the stale references. Mirror it here;
 * the cost is ONE dropped frame per screen transition (invisible behind the
 * entry fades / the intra-screen stage flip). Beta-only borrow: these globals
 * all have external linkage in thread3_main.c; the OFF build never compiles
 * this TU, so the release engine object is untouched. */
extern s8 gSkipGfxTask;      /* thread3_main.c */
extern s8 gDrawFrameTimer;   /* thread3_main.c */
extern Gfx *gDisplayLists[2];/* thread3_main.c */
extern s32 gSPTaskNum;       /* thread3_main.c */

void mdkr_online_screen_dl_retire(void) {
    if (gSkipGfxTask == FALSE) {
        if (gDrawFrameTimer != 1) {
            (void) gfxtask_wait();
        }
        gSkipGfxTask = TRUE;
    }
    /* Truncate the current authored list: this frame's already-drawn commands
     * reference the tiles the caller is about to free, so they must never be
     * consumed. Identical to unload_level_game's own truncation. */
    gCurrDisplayList = gDisplayLists[gSPTaskNum];
    gDPFullSync(gCurrDisplayList++);
    gSPEndDisplayList(gCurrDisplayList++);
    /* Hold the LAST presented image over the retired frame(s): without this
     * the empty truncated task presents one BLACK frame -- a visible blink on
     * the fade-skipped intra-track-screen stage flips (captured on the joiner
     * flow dump). gDrawFrameTimer=2 is the engine's own loading-hold: the task
     * submit is skipped while it counts down (thread3_main.c:378) and the
     * previous framebuffer is copied over the current one (:598-608), so the
     * outgoing screen's last real frame persists until the incoming screen's
     * first frame is authored. */
    gDrawFrameTimer = 2;
}

/* Start / keep the retail menu music on the native menu-family screens via the
 * self-contained music_play() primitive (the exact call menu.c makes) -- NOT the
 * offline menu music state machine. Idempotent (music_current_sequence guards
 * against restarting it every entry). The race level loader replaces the sequence on
 * the RACE hand-off. */
void mdkr_online_screen_menu_music(void) {
    mdkr_online_screen_music((u8) SEQUENCE_MAIN_MENU);
}

/* Start / keep a specific menu-family sequence (idempotent -- music_current_sequence
 * guards against restarting it every entry). The race level loader replaces the
 * sequence on the RACE hand-off. */
void mdkr_online_screen_music(u8 sequence) {
    if (music_current_sequence() != sequence) {
        music_play(sequence);
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
