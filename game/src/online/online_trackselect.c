/* SEPARATED-BOOT-PATH (Strategy D2) native online HOST TRACK / CUP select.
 *
 * ============================ THE D2 REUSE BOUNDARY ========================
 * The second player-facing SCREEN of the separated online flow, between the
 * native CHARSELECT (online_charselect.c) and the race. Strategy D2 means:
 * RE-IMPLEMENT the presentation here using the GAME'S OWN decoded assets rather
 * than calling the offline menu's track-select loop. The offline track select is
 * a live 3D world preview driven by the GAMEMODE_MENU state machine -- entering
 * it would defeat the separated-boot isolation guarantee and it has no notion of
 * the party_link feeds. So we reuse the DATA and the DRAW/SFX primitives, not the
 * loop.
 *
 * WHAT IT BORROWS (read-only reuse of already-compiled game code/data; NO edit to
 * menu.c is required -- every symbol below already has external linkage):
 *   - gMenuAssets[] + menu_assetgroup_load/free (menu.c) the same texture-group
 *     loader the offline track select uses; we hand it the per-world BACKGROUND
 *     texture ids (TEXTURE_BACKGROUND_*_TOP/BOTTOM) so it loads exactly the ten
 *     world-sky tiles and spawns no menu objects. We bind them into local
 *     DrawTextures the charselect way and blit with texrect_draw.
 *   - level_name(id)             (game.c) the REAL, language-aware track names --
 *                                the engine truth, so the on-screen list can
 *                                never drift from the ROM.
 *   - leveltable_vehicle_usable(id) (game.c) the REAL per-track vehicle mask --
 *                                engine truth for the R-A auto-narrow, zero drift.
 *   - draw_text / set_text_*     (font.h) the real DKR font.
 *   - texrect_draw / bgdraw_*    (rcp_dkr.h) the game's own 2D blit.
 *   - sound_play + SOUND_*       (audio.h / sound_ids.h) the real menu SFX.
 *   - input_pressed / stick      (joypad.h) the real pad, local player only.
 *   - get_player_selected_vehicle(menu.c) the seed vehicle, same as CHARSELECT.
 *
 * WHAT IT OWNS (all state lives HERE, never an offline global): the cursor, the
 * mode toggle, the track/cup lock latch, the auto-narrowed local vehicle, the
 * continuous reverse-feed intent and the whole 2D layout. The forward feed (both
 * seats + the host's config + the host_cursor) is read from platform/net/party_link.
 *
 * ISOLATION (R-D): this engine TU pulls NO launcher/platform headers -- only the
 * decomp game headers and net/party_link.h, exactly like CHARSELECT. Track NAMES
 * come from level_name(); vehicle MASKS from leveltable_vehicle_usable(). The list
 * of 20 selectable track ids is mirrored locally (sTrackIds[]) rather than
 * #including online/online_track_table.h; the headless lane asserts that list
 * equals the reducer-accepted set (kCupTracks in lobby_core.c) so it can never
 * drift silently.
 *
 * The ENTIRE TU is #if MDKR_ENABLE_ONLINE_BETA and it is added to the build only
 * inside the beta CMake gate (game/src/online/ is not globbed), so a normal (beta
 * OFF) build never compiles a byte of it and the release engine object is
 * untouched.
 * ==========================================================================
 */
#include "online/online_trackselect.h"

#if MDKR_ENABLE_ONLINE_BETA

/* Game/PR headers FIRST (same sprintf-ordering rationale as online_charselect.c). */
#include "types.h"
#include "thread3_main.h"
#include "enums.h"      /* VEHICLE_CAR / HOVERCRAFT / PLANE, AlignmentFlags */
#include "game.h"       /* level_name, leveltable_vehicle_usable */
#include "menu.h"       /* gMenuAssets, menu_assetgroup_load/free,
                           get_player_selected_vehicle, TEXTURE_BACKGROUND_*,
                           font.h (draw_text, ...) */
#include "rcp_dkr.h"    /* texrect_draw, bgdraw_fillcolour */
#include "audio.h"      /* sound_play */
#include "sound_ids.h"  /* SOUND_MENU_PICK2 / SOUND_SELECT2 / ... */
#include "joypad.h"     /* input_pressed, input_clamp_stick_x/y */
#include "PR/os_cont.h" /* A_BUTTON / B_BUTTON / *_JPAD / START_BUTTON / Z_TRIG */
#include "net/party_link.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Screen space (mirrored, like online_charselect.c). */
#define TS_SCREEN_W 320
#define TS_SCREEN_H 240
#define TS_SCREEN_W_HALF 160

/* The engine's live 2D display list + menu asset table. Declared here (no shared
 * header exposes gCurrDisplayList; menu.h does not export gMenuAssets) rather
 * than by editing menu.c -- exactly how online_charselect.c reaches gCurrDisplayList
 * and gRacerPortraits. gMenuAssets[k] holds a TextureHeader* for a loaded
 * TEXTURE_* id (see menu_asset_load). */
extern Gfx *gCurrDisplayList;
extern void *gMenuAssets[128];

/* ---- Local mirrors of the launcher lobby's id space (R-D: no launcher headers) */
#define TS_CHAR_COUNT 10u            /* MDKR_ONLINE_CHARACTER_COUNT */
#define TS_NO_CHARACTER 0xFFu        /* MDKR_ONLINE_NO_CHARACTER */
#define TS_NO_VEHICLE 0xFFu          /* MDKR_ONLINE_NO_VEHICLE */
#define TS_PLAYER_VEHICLE_COUNT 3u   /* car / hovercraft / plane (0x07 mask) */
#define TS_LOBBY_PHASE 1u            /* MDKR_ONLINE_LOBBY */
#define TS_LOADING_PHASE 2u          /* MDKR_ONLINE_LOADING */
#define TS_MODE_SINGLE 0u            /* MDKR_ONLINE_MODE_SINGLE_RACE */
#define TS_MODE_TOURNAMENT 1u        /* MDKR_ONLINE_MODE_TOURNAMENT */
#define TS_LOCAL_PAD 0               /* PLAYER_ONE */

/* Grid: 5 world columns x 4 round rows (== the 5 cups x 4 rounds). */
#define TS_COLS 5
#define TS_ROWS 4
#define TS_TRACK_COUNT (TS_COLS * TS_ROWS)
#define TS_COL_W 64
#define TS_BANNER_Y 40
#define TS_BANNER_H 32
#define TS_ROW_Y0 72
#define TS_ROW_DY 14
#define TS_NONE 0xFFu

/* 2-player picker narrowing (retail parity, menu.c menu_track_select V79+). */
#define TS_TRACK_SPACEPORT_ALPHA 15u
#define TS_TRACK_FROSTY_VILLAGE 28u

/* Menu SFX (the real DKR enums, same reuse as CHARSELECT). */
#define TS_SFX_MOVE SOUND_MENU_PICK2
#define TS_SFX_LOCK SOUND_SELECT2
#define TS_SFX_START SOUND_SELECT3
#define TS_SFX_BACK SOUND_MENU_BACK3
#define TS_SFX_REJECT SOUND_ELECTRIC_BUZZ

/* The 20 selectable track ids, cup-major then round order -- MIRRORS kCupTracks
 * in platform/online/lobby_core.c (the reducer's authoritative accepted set) and
 * online_track_table.c. Column c == cup c (world display order); row r == round r;
 * track index == c*4 + r. R-D: the headless lane asserts this equals the reducer
 * set, so a drift is caught. (Every id is <= 33, so it fits the u8 dispatch
 * value the reverse feed carries.) */
static const u8 sTrackIds[TS_TRACK_COUNT] = {
    5u,  3u,  29u, 7u,  /* cup0 Dino Domain:        Ancient Lake / Fossil Canyon / Jungle Falls / Hot Top Volcano */
    13u, 6u,  9u,  28u, /* cup1 Snowflake Mountain: EverFrost / Walrus Cove / Snowball Valley / Frosty Village */
    8u,  4u,  10u, 30u, /* cup2 Sherbet Island:     Whale Bay / Pirate Lagoon / Crescent Island / Treasure Caves */
    19u, 18u, 20u, 31u, /* cup3 Dragon Forest:      Boulder Canyon / Greenwood / Windmill Plains / Haunted Woods */
    17u, 32u, 33u, 15u, /* cup4 Future Fun Land:    Spacedust Alley / DarkMoon / Star City / Spaceport Alpha */
};

/* Per-cup (== per-column) world background TOP/BOTTOM texture ids, in cup display
 * order. These are the real per-world sky tiles the offline track select uses
 * (menu.c gTracksMenuBgTextureIndices). */
static const s16 sCupBgTop[TS_COLS] = {
    TEXTURE_BACKGROUND_DINO_DOMAIN_TOP,       /* cup0 Dino */
    TEXTURE_BACKGROUND_SNOWFLAKE_MOUNTAIN_TOP,/* cup1 Snowflake */
    TEXTURE_BACKGROUND_SHERBERT_ISLAND_TOP,   /* cup2 Sherbet */
    TEXTURE_BACKGROUND_DRAGON_FOREST_TOP,     /* cup3 Dragon */
    TEXTURE_BACKGROUND_FUTURE_FUN_LAND_TOP,   /* cup4 FFL */
};

/* Short, plain world/cup labels (display chrome only -- NOT the drift-sensitive
 * track set, so a static label is allowed by R-C; track names themselves come
 * from level_name()). */
static const char *const sWorldLabels[TS_COLS] = {
    "DINO", "SNOW", "SHERBET", "DRAGON", "FUTURE",
};
static const char *const sCupLabels[TS_COLS] = {
    "DINO CUP", "SNOW CUP", "SHERBET CUP", "DRAGON CUP", "FUTURE CUP",
};

/* The -1-terminated asset group: the five worlds' TOP+BOTTOM sky tiles. */
static s16 sWorldBgAssetIds[] = {
    TEXTURE_BACKGROUND_DINO_DOMAIN_TOP, TEXTURE_BACKGROUND_DINO_DOMAIN_BOTTOM,
    TEXTURE_BACKGROUND_SNOWFLAKE_MOUNTAIN_TOP,
    TEXTURE_BACKGROUND_SNOWFLAKE_MOUNTAIN_BOTTOM,
    TEXTURE_BACKGROUND_SHERBERT_ISLAND_TOP,
    TEXTURE_BACKGROUND_SHERBERT_ISLAND_BOTTOM,
    TEXTURE_BACKGROUND_DRAGON_FOREST_TOP, TEXTURE_BACKGROUND_DRAGON_FOREST_BOTTOM,
    TEXTURE_BACKGROUND_FUTURE_FUN_LAND_TOP,
    TEXTURE_BACKGROUND_FUTURE_FUN_LAND_BOTTOM,
    -1,
};

/* ---- Session-owned screen state (never an offline global) ------------------ */
typedef struct MdkrOnlineTrackselectState {
    u8 mode;         /* TS_MODE_SINGLE / TS_MODE_TOURNAMENT (host-chosen) */
    u8 cursorCol;    /* world column (0..4) == cup id */
    u8 cursorRow;    /* round row (0..3), single-race only */
    u8 lockedTrack;  /* locked track INDEX (0..19), or TS_NONE */
    u8 lockedCup;    /* locked cup (0..4), or TS_NONE */
    u8 vehicle;      /* auto-narrowed, mask-legal local vehicle id */
    u8 host;         /* the local seat is the room leader */
    u8 startReq;     /* host pressed Start with something locked (latched) */
    u8 assets;       /* world bg group + fonts loaded */
    u8 leave;        /* B: back-to-charselect request (edge; see tick) */
    u32 ticks;       /* TRACKSELECT ticks elapsed (also drives the test input) */
    u32 lockFlashEnd;/* "LOCKING IN..." transient deadline, in ticks */
    s8 stickLatchX;
    s8 stickLatchY;
} MdkrOnlineTrackselectState;

static MdkrOnlineTrackselectState sTs;

/* Resolved per-column background tiles (bound from gMenuAssets after load). */
static TextureHeader *sCupBgTopTex[TS_COLS];
static TextureHeader *sCupBgBotTex[TS_COLS];

/* Witness change-detect (file scope so _enter() can reset for a clean re-entry). */
static u32 sWitnessKey = 0xFFFFFFFFu;
static u8 sTracksWitnessed;

/* ---- forward decls (test seam defined at the bottom) ---------------------- */
static void trackselect_test_resolve(void);
static void trackselect_test_reset(void);
static void trackselect_test_reduce_and_script(void);

#define TS_LOCK_FLASH_TICKS 30u /* "LOCKING IN..." flash (~1s @ 30Hz) */

/* ======================================================================== *
 * Track / vehicle helpers (engine truth)
 * ======================================================================== */

/* Which snapshot seat is the local one. */
static s32 trackselect_local_seat(const MdkrPartyLinkSnapshot *snap) {
    unsigned i;
    for (i = 0u; i < MDKR_PARTY_LINK_SEATS; i++) {
        if (snap->seats[i].occupied && snap->seats[i].is_local) {
            return (s32) i;
        }
    }
    return -1;
}

static unsigned trackselect_occupied_seats(const MdkrPartyLinkSnapshot *snap,
                                           bool haveSnap) {
    unsigned i;
    unsigned n = 0u;
    if (!haveSnap) {
        return 1u;
    }
    for (i = 0u; i < MDKR_PARTY_LINK_SEATS; i++) {
        if (snap->seats[i].occupied) {
            n++;
        }
    }
    return n == 0u ? 1u : n;
}

/* The usable-vehicle mask for a track at this player count: engine truth from
 * leveltable_vehicle_usable(), then the retail 2-player narrowing (R-A). */
static u8 trackselect_track_mask(u8 trackId, unsigned occupied) {
    u8 mask = (u8) leveltable_vehicle_usable((s32) trackId);
    if (mask == 0u) {
        mask = (u8) (1u << VEHICLE_CAR); /* fail-safe: never empty */
    }
    if (occupied >= 2u) {
        if (trackId == TS_TRACK_SPACEPORT_ALPHA) {
            mask &= (u8) ~(1u << VEHICLE_HOVERCRAFT);
        }
        if (trackId == TS_TRACK_FROSTY_VILLAGE) {
            mask &= (u8) ~(1u << VEHICLE_PLANE);
        }
    }
    return mask;
}

/* The track the vehicle auto-narrow and the vehicle preview resolve against:
 * the hovered track in single race, the chosen cup's first round in tournament. */
static u8 trackselect_resolved_track(void) {
    if (sTs.mode == TS_MODE_TOURNAMENT) {
        return sTrackIds[(sTs.cursorCol * TS_ROWS) + 0u];
    }
    return sTrackIds[(sTs.cursorCol * TS_ROWS) + sTs.cursorRow];
}

/* R-A: keep the local seat's vehicle inside the resolved track's mask so
 * BEGIN_LOADING can never be refused for an illegal vehicle. Picks the lowest
 * legal bit when the current one is not allowed. */
static void trackselect_autonarrow_vehicle(u8 trackId, unsigned occupied) {
    u8 mask = trackselect_track_mask(trackId, occupied);
    u8 v;
    if (mask & (u8) (1u << sTs.vehicle)) {
        return; /* already legal */
    }
    for (v = 0u; v < TS_PLAYER_VEHICLE_COUNT; v++) {
        if (mask & (u8) (1u << v)) {
            sTs.vehicle = v;
            return;
        }
    }
}

/* ======================================================================== *
 * Input
 * ======================================================================== */
typedef struct TsInput {
    s8 dx;        /* -1 / 0 / +1 column step (edge) */
    s8 dy;        /* -1 / 0 / +1 row step (edge) */
    u8 aEdge;     /* A: lock track/cup */
    u8 bEdge;     /* B: back to charselect */
    u8 startEdge; /* Start: begin the race (host, once locked) */
    u8 modeEdge;  /* Z: toggle single/tournament (host) */
} TsInput;

/* Scripted headless input (env MDKR_TEST_ONLINE_TRACKSELECT). Deterministic and
 * inert in a normal run; exercises the SAME cursor/lock/start logic the live pad
 * drives. Keyed on the TRACKSELECT ENTRY count so the first entry proves the
 * B->CHARSELECT back path (no wedge) and the second entry locks a track + starts:
 *
 *   entry 1: press B early (while browsing) -> session returns to CHARSELECT.
 *   entry 2: walk to Whale Bay (cup2/round0, track 8, hovercraft-only 0x2) so the
 *            R-A auto-narrow moves the seat off Car, LOCK it (SET_CONFIG_TRACK),
 *            then press Start (host-start -> BEGIN_LOADING -> boot). */
static u8 sTsEntryCount; /* incremented each _enter (test), persists across a run */

static void trackselect_input_scripted(TsInput *in) {
    memset(in, 0, sizeof(*in));
    if (sTsEntryCount <= 1u) {
        /* First entry: exercise the back-to-charselect path once, then stop. */
        if (sTs.ticks == 3u) {
            in->bEdge = 1u;
        }
        return;
    }
    /* Second (and later) entry: lock a narrow-mask track, then start. */
    switch (sTs.ticks) {
    case 2u:
        in->dx = 1; /* col 0 -> 1 */
        break;
    case 3u:
        in->dx = 1; /* col 1 -> 2 (Sherbet); row 0 == Whale Bay (track 8) */
        break;
    case 6u:
        in->aEdge = 1u; /* lock track 8 -> SET_CONFIG_TRACK(8) (clears ready) */
        break;
    case 20u:
        in->startEdge = 1u; /* host start once reconverged to all-ready */
        break;
    default:
        break;
    }
}

/* Live pad: D-pad edges plus a latched analog stick, local player only. */
static void trackselect_input_live(TsInput *in) {
    u32 pressed = input_pressed(TS_LOCAL_PAD);
    s32 sx = input_clamp_stick_x(TS_LOCAL_PAD);
    s32 sy = input_clamp_stick_y(TS_LOCAL_PAD);
    s8 wantX = 0;
    s8 wantY = 0;

    memset(in, 0, sizeof(*in));

    if (pressed & R_JPAD) {
        in->dx = 1;
    } else if (pressed & L_JPAD) {
        in->dx = -1;
    }
    if (pressed & D_JPAD) {
        in->dy = 1;
    } else if (pressed & U_JPAD) {
        in->dy = -1;
    }
    if (sx > 40) {
        wantX = 1;
    } else if (sx < -40) {
        wantX = -1;
    }
    if (sy > 40) {
        wantY = -1;
    } else if (sy < -40) {
        wantY = 1;
    }
    if (in->dx == 0 && wantX != 0 && sTs.stickLatchX == 0) {
        in->dx = wantX;
    }
    if (in->dy == 0 && wantY != 0 && sTs.stickLatchY == 0) {
        in->dy = wantY;
    }
    sTs.stickLatchX = wantX;
    sTs.stickLatchY = wantY;

    in->aEdge = (pressed & A_BUTTON) ? 1u : 0u;
    in->bEdge = (pressed & B_BUTTON) ? 1u : 0u;
    in->startEdge = (pressed & START_BUTTON) ? 1u : 0u;
    in->modeEdge = (pressed & Z_TRIG) ? 1u : 0u;
}

static void trackselect_gather_input(TsInput *in) {
    if (mdkr_online_trackselect_test_active()) {
        trackselect_input_scripted(in);
    } else {
        trackselect_input_live(in);
    }
}

/* Apply one input step. Track/cup/mode/start are HOST-ONLY (R-C: the joiner's
 * input is suppressed and it just watches). B backs out for everyone. */
static void trackselect_apply_input(const TsInput *in) {
    if (in->bEdge) {
        sTs.leave = 1u;
        sound_play(TS_SFX_BACK, NULL);
        return;
    }
    if (!sTs.host) {
        return; /* joiner: watch only */
    }

    if (in->modeEdge) {
        sTs.mode = (sTs.mode == TS_MODE_SINGLE) ? TS_MODE_TOURNAMENT
                                                : TS_MODE_SINGLE;
        /* Changing mode drops the pending lock (the reducer clears ready too; the
         * continuous republish reconverges -- R-B). */
        sTs.lockedTrack = TS_NONE;
        sTs.lockedCup = TS_NONE;
        sTs.startReq = 0u;
        sound_play(TS_SFX_LOCK, NULL);
        return;
    }

    if (in->dx != 0) {
        s32 col = (s32) sTs.cursorCol + in->dx;
        col = (col + TS_COLS) % TS_COLS; /* columns wrap (native DKR 2D menus) */
        if ((u8) col != sTs.cursorCol) {
            sTs.cursorCol = (u8) col;
            sound_play(TS_SFX_MOVE, NULL);
        }
    }
    if (in->dy != 0 && sTs.mode == TS_MODE_SINGLE) {
        s32 row = (s32) sTs.cursorRow + in->dy;
        if (row < 0) {
            row = 0;
        }
        if (row >= TS_ROWS) {
            row = TS_ROWS - 1;
        }
        if ((u8) row != sTs.cursorRow) {
            sTs.cursorRow = (u8) row;
            sound_play(TS_SFX_MOVE, NULL);
        }
    }

    if (in->aEdge) {
        if (sTs.mode == TS_MODE_SINGLE) {
            sTs.lockedTrack =
                (u8) ((sTs.cursorCol * TS_ROWS) + sTs.cursorRow);
        } else {
            sTs.lockedCup = sTs.cursorCol;
        }
        sTs.startReq = 0u; /* a fresh lock re-arms Start */
        sTs.lockFlashEnd = sTs.ticks + TS_LOCK_FLASH_TICKS;
        sound_play(TS_SFX_LOCK, NULL);
    } else if (in->startEdge) {
        bool locked = (sTs.mode == TS_MODE_SINGLE)
                          ? (sTs.lockedTrack != TS_NONE)
                          : (sTs.lockedCup != TS_NONE);
        if (locked) {
            sTs.startReq = 1u;
            sound_play(TS_SFX_START, NULL);
        } else {
            sound_play(TS_SFX_REJECT, NULL); /* nothing to start yet */
        }
    }
}

/* Publish the FULL local intent every frame (R-B: continuous republish so the
 * reducer's ready-clear on any config change reconverges within a pump). The host
 * carries mode + the locked track/cup + start; a joiner carries only its
 * (already locked) character/vehicle/ready and leaves the config at UNSET. */
static void trackselect_publish_intent(u8 localSeatChar) {
    MdkrPartyLinkLocalIntent intent;
    memset(&intent, 0, sizeof(intent));

    if (localSeatChar < TS_CHAR_COUNT) {
        intent.hover_character = localSeatChar;
        intent.confirmed = 1u;
    } else {
        intent.hover_character = 0u;
        intent.confirmed = 0u;
    }
    intent.vehicle_id = sTs.vehicle;
    intent.ready = 1u;
    intent.backout = 0u;
    intent.start_requested = 0u;
    intent.mode = MDKR_PARTY_LINK_MODE_UNSET;
    intent.config_track = MDKR_PARTY_LINK_TRACK_UNSET;
    intent.cup_id = MDKR_PARTY_LINK_CUP_UNSET;

    if (sTs.host) {
        intent.mode = sTs.mode; /* host always drives the mode */
        if (sTs.mode == TS_MODE_SINGLE && sTs.lockedTrack != TS_NONE) {
            intent.config_track = sTrackIds[sTs.lockedTrack];
        } else if (sTs.mode == TS_MODE_TOURNAMENT && sTs.lockedCup != TS_NONE) {
            intent.cup_id = sTs.lockedCup;
        }
        intent.start_requested = sTs.startReq ? 1u : 0u;
    }
    mdkr_party_link_intent_publish(&intent);
}

/* ======================================================================== *
 * Render (native: real world backgrounds + real font, into the frame list)
 * ======================================================================== */
static void trackselect_draw_text_at(s32 x, s32 y, s32 fontId, char *text,
                                     AlignmentFlags align, s32 r, s32 g, s32 b) {
    set_text_font(fontId);
    set_text_background_colour(0, 0, 0, 0);
    set_text_colour(r, g, b, 0, 255);
    draw_text(&gCurrDisplayList, x, y, text, align);
}

/* Blit one 64x32 world-sky tile (dimmed) as a column header banner. */
static void trackselect_draw_banner(u8 col, u8 dim) {
    DrawTexture tile[2];
    s32 x = (s32) col * TS_COL_W;
    if (sCupBgTopTex[col] == NULL) {
        return;
    }
    tile[0].texture = sCupBgTopTex[col];
    tile[0].xOffset = 0;
    tile[0].yOffset = 0;
    tile[1].texture = NULL;
    tile[1].xOffset = 0;
    tile[1].yOffset = 0;
    texrect_draw(&gCurrDisplayList, tile, x, TS_BANNER_Y, dim, dim, dim, 255);
}

static void trackselect_render(const MdkrPartyLinkSnapshot *snap, bool haveSnap,
                               u8 hostCursorValid, u8 hostCol, u8 hostRow) {
    s32 tri = (s32) (sTs.ticks & 31u);
    u8 c;
    char line[64];

    (void) snap;
    (void) haveSnap;
    if (tri > 16) {
        tri = 32 - tri;
    }

    /* Title + mode line. */
    trackselect_draw_text_at(TS_SCREEN_W_HALF, 14, ASSET_FONTS_BIGFONT,
                             "SELECT TRACK", ALIGN_MIDDLE_CENTER, 255, 224, 96);
    (void) snprintf(line, sizeof(line), "%s%s",
                    sTs.mode == TS_MODE_SINGLE ? "SINGLE RACE" : "TOURNAMENT",
                    sTs.host ? "   Z: MODE" : "");
    trackselect_draw_text_at(TS_SCREEN_W_HALF, 28, ASSET_FONTS_SMALLFONT, line,
                             ALIGN_MIDDLE_CENTER, 200, 200, 255);

    /* Five world columns: a real per-world sky banner, then the world/cup label,
     * then the four track names (single) or the four rounds (tournament). */
    for (c = 0u; c < TS_COLS; c++) {
        s32 cx = (s32) c * TS_COL_W + (TS_COL_W / 2);
        bool colCursor = (c == sTs.cursorCol);
        u8 dim = colCursor ? 200u : 120u;
        u8 r;

        trackselect_draw_banner(c, dim);

        /* World / cup label on the banner. */
        if (sTs.mode == TS_MODE_SINGLE) {
            trackselect_draw_text_at(cx, TS_BANNER_Y + 12, ASSET_FONTS_SMALLFONT,
                                     (char *) sWorldLabels[c],
                                     ALIGN_MIDDLE_CENTER,
                                     colCursor ? 255 : 210,
                                     colCursor ? 240 : 210,
                                     colCursor ? 160 : 210);
        } else {
            bool cupLocked = (sTs.lockedCup == c);
            trackselect_draw_text_at(
                cx, TS_BANNER_Y + 12, ASSET_FONTS_SMALLFONT,
                (char *) sCupLabels[c], ALIGN_MIDDLE_CENTER,
                cupLocked ? 120 : (colCursor ? 255 : 210),
                cupLocked ? 255 : (colCursor ? 240 : 210),
                cupLocked ? 120 : (colCursor ? 60 : 210));
        }

        for (r = 0u; r < TS_ROWS; r++) {
            u8 idx = (u8) (c * TS_ROWS + r);
            char *name = level_name((s32) sTrackIds[idx]);
            s32 y = TS_ROW_Y0 + (s32) r * TS_ROW_DY;
            bool onCursor = (sTs.mode == TS_MODE_SINGLE && colCursor &&
                             r == sTs.cursorRow);
            bool locked = (sTs.mode == TS_MODE_SINGLE &&
                           sTs.lockedTrack == idx) ||
                          (sTs.mode == TS_MODE_TOURNAMENT &&
                           sTs.lockedCup == c);
            s32 nr = 190, ng = 190, nb = 190;
            char label[28];

            if (name == NULL) {
                name = (char *) "?";
            }
            if (locked) {
                nr = 120;
                ng = 255;
                nb = 120;
            } else if (onCursor) {
                nr = 255;
                ng = 190 + tri * 4;
                nb = 60 + tri * 3;
            }
            /* Shape redundancy for the hovered track (F7 parity w/ charselect). */
            if (onCursor) {
                (void) snprintf(label, sizeof(label), ">%.10s<", name);
            } else {
                (void) snprintf(label, sizeof(label), "%.12s", name);
            }
            trackselect_draw_text_at(cx, y, ASSET_FONTS_SMALLFONT, label,
                                     ALIGN_MIDDLE_CENTER, nr, ng, nb);
        }
    }

    /* Joiner: mirror the host cursor when the snapshot marks it valid (never
     * today -- driven by the test pump; the live host_cursor transport is PD-T6). */
    if (!sTs.host && hostCursorValid) {
        s32 hx = (s32) hostCol * TS_COL_W + (TS_COL_W / 2);
        s32 hy = TS_ROW_Y0 + (s32) hostRow * TS_ROW_DY;
        trackselect_draw_text_at(hx, hy, ASSET_FONTS_SMALLFONT, "<>",
                                 ALIGN_MIDDLE_CENTER, 255, 210, 90);
    }

    /* Status line. */
    {
        bool locked = (sTs.mode == TS_MODE_SINGLE)
                          ? (sTs.lockedTrack != TS_NONE)
                          : (sTs.lockedCup != TS_NONE);
        if (!sTs.host) {
            trackselect_draw_text_at(TS_SCREEN_W_HALF, 140,
                                     ASSET_FONTS_SMALLFONT, "HOST IS CHOOSING...",
                                     ALIGN_MIDDLE_CENTER, 200, 200, 120);
        } else if (sTs.ticks < sTs.lockFlashEnd) {
            trackselect_draw_text_at(TS_SCREEN_W_HALF, 140,
                                     ASSET_FONTS_SMALLFONT, "LOCKING IN...",
                                     ALIGN_MIDDLE_CENTER, 255, 240, 120);
        } else if (locked) {
            trackselect_draw_text_at(TS_SCREEN_W_HALF, 140,
                                     ASSET_FONTS_SMALLFONT,
                                     "PRESS START TO BEGIN", ALIGN_MIDDLE_CENTER,
                                     120, 255, 120);
        } else {
            trackselect_draw_text_at(
                TS_SCREEN_W_HALF, 140, ASSET_FONTS_SMALLFONT,
                sTs.mode == TS_MODE_SINGLE ? "CHOOSE A TRACK" : "CHOOSE A CUP",
                ALIGN_MIDDLE_CENTER, 220, 220, 220);
        }
    }

    /* Help line. */
    if (sTs.host) {
        trackselect_draw_text_at(TS_SCREEN_W_HALF, 224, ASSET_FONTS_SMALLFONT,
                                 "A: LOCK   START: BEGIN   B: BACK",
                                 ALIGN_MIDDLE_CENTER, 255, 255, 255);
    } else {
        trackselect_draw_text_at(TS_SCREEN_W_HALF, 224, ASSET_FONTS_SMALLFONT,
                                 "B: BACK", ALIGN_MIDDLE_CENTER, 255, 255, 255);
    }
}

/* Bounded stderr witness: one line only when the visible/config state changes, so
 * the headless lane reads the drawn cursor + resolved track/mask/vehicle + both
 * seats' ready + the snapshot config + the published intent without flooding. */
static void trackselect_witness(const MdkrPartyLinkSnapshot *snap, bool haveSnap,
                                u8 resolvedTrack, u8 mask, s32 lockedTrackId,
                                s32 lockedCup) {
    u8 seat0Ready = 0u;
    u8 seat1Ready = 0u;
    u16 snapCfg = 0xFFFFu;
    u8 snapCup = 0xFFu;
    u8 snapPhase = 0u;
    u32 key;

    if (haveSnap) {
        seat0Ready = snap->seats[0].ready;
        seat1Ready = snap->seats[1].ready;
        snapCfg = snap->configured_track;
        snapCup = snap->cup_id;
        snapPhase = snap->phase;
    }

    key = ((u32) sTs.mode) | ((u32) sTs.cursorCol << 1) |
          ((u32) sTs.cursorRow << 4) | ((u32) sTs.vehicle << 6) |
          ((u32) (seat0Ready ? 1u : 0u) << 8) |
          ((u32) (seat1Ready ? 1u : 0u) << 9) | ((u32) snapPhase << 10) |
          ((u32) resolvedTrack << 13) |
          ((u32) ((snapCfg == 0xFFFFu) ? 0x3Fu : (snapCfg & 0x3Fu)) << 20) |
          ((u32) sTs.startReq << 26);
    if (key == sWitnessKey) {
        return;
    }
    sWitnessKey = key;

    fprintf(stderr,
            "[online-trackselect] render mode=%u col=%u row=%u host=%u "
            "track=%u mask=0x%x vehicle=%u locked{track=%d cup=%d} "
            "seat{r0=%u r1=%u} snap{cfgTrack=%u cup=%u phase=%u} start=%u\n",
            (unsigned) sTs.mode, (unsigned) sTs.cursorCol,
            (unsigned) sTs.cursorRow, (unsigned) sTs.host,
            (unsigned) resolvedTrack, (unsigned) mask, (unsigned) sTs.vehicle,
            lockedTrackId, lockedCup, (unsigned) seat0Ready,
            (unsigned) seat1Ready, (unsigned) snapCfg, (unsigned) snapCup,
            (unsigned) snapPhase, (unsigned) sTs.startReq);
}

/* Emit the offered track-id list ONCE so the lane can assert it equals the
 * reducer-accepted set (R-D). */
static void trackselect_witness_tracks(void) {
    char buf[128];
    int off = 0;
    unsigned i;
    if (sTracksWitnessed) {
        return;
    }
    sTracksWitnessed = 1u;
    for (i = 0u; i < TS_TRACK_COUNT; i++) {
        off += snprintf(buf + off, sizeof(buf) - (size_t) off, "%s%u",
                        i == 0u ? "" : " ", (unsigned) sTrackIds[i]);
        if (off >= (int) sizeof(buf) - 4) {
            break;
        }
    }
    fprintf(stderr, "[online-trackselect] tracks: %s\n", buf);
}

/* ======================================================================== *
 * Lifecycle
 * ======================================================================== */
void mdkr_online_trackselect_enter(void) {
    s8 defaultVehicle;
    u8 c;

    memset(&sTs, 0, sizeof(sTs));
    sTs.mode = TS_MODE_SINGLE;
    sTs.lockedTrack = TS_NONE;
    sTs.lockedCup = TS_NONE;

    /* Seed the vehicle exactly as CHARSELECT does; the per-tick auto-narrow then
     * keeps it inside the resolved track's mask. */
    defaultVehicle = get_player_selected_vehicle(TS_LOCAL_PAD);
    if (defaultVehicle < 0 || (u8) defaultVehicle >= TS_PLAYER_VEHICLE_COUNT) {
        defaultVehicle = (s8) VEHICLE_CAR;
    }
    sTs.vehicle = (u8) defaultVehicle;

    /* M3: reset the witness change-detect + the headless seam for a clean entry. */
    sWitnessKey = 0xFFFFFFFFu;
    trackselect_test_reset();

    /* Borrow the real per-world backgrounds: load the sky-tile group and bind the
     * TOP/BOTTOM tiles into local DrawTextures (the charselect asset-borrow
     * discipline; menu_asset_load routes each id to load_texture). */
    menu_assetgroup_load(sWorldBgAssetIds);
    for (c = 0u; c < TS_COLS; c++) {
        sCupBgTopTex[c] = (TextureHeader *) gMenuAssets[sCupBgTop[c]];
        sCupBgBotTex[c] = (TextureHeader *) gMenuAssets[sCupBgTop[c] + 1];
    }

    load_font(ASSET_FONTS_BIGFONT);
    load_font(ASSET_FONTS_SMALLFONT);

    sTs.assets = 1u;
    bgdraw_fillcolour(12, 16, 32);

    if (mdkr_online_trackselect_test_active()) {
        sTsEntryCount++;
    }
    trackselect_witness_tracks();

    fprintf(stderr,
            "[online-trackselect] enter: native track select up entry=%u "
            "defaultVehicle=%u (world backgrounds loaded, offline _loop "
            "bypassed)\n",
            (unsigned) sTsEntryCount, (unsigned) sTs.vehicle);
}

void mdkr_online_trackselect_exit(void) {
    u8 c;
    if (sTs.assets) {
        unload_font(ASSET_FONTS_SMALLFONT);
        unload_font(ASSET_FONTS_BIGFONT);
        menu_assetgroup_free(sWorldBgAssetIds);
        for (c = 0u; c < TS_COLS; c++) {
            sCupBgTopTex[c] = NULL;
            sCupBgBotTex[c] = NULL;
        }
        sTs.assets = 0u;
        fprintf(stderr, "[online-trackselect] exit: freed world bg assets\n");
    }
}

MdkrOnlineTrackselectResult mdkr_online_trackselect_tick(s32 updateRate) {
    MdkrPartyLinkSnapshot snap;
    bool haveSnap;
    s32 localSeat;
    unsigned occupied;
    u8 localSeatChar = TS_NO_CHARACTER;
    u8 resolvedTrack;
    u8 mask;
    TsInput in;

    (void) updateRate;

    haveSnap = mdkr_party_link_read(&snap);
    localSeat = haveSnap ? trackselect_local_seat(&snap) : -1;
    occupied = trackselect_occupied_seats(&snap, haveSnap);

    if (localSeat >= 0) {
        sTs.host = snap.seats[localSeat].is_host ? 1u : 0u;
        localSeatChar = snap.seats[localSeat].character_id;
    } else {
        /* No resolvable local seat: assume host so a real (non-test) run still
         * lets the leader drive; a joiner is resolved once the feed seats it. */
        sTs.host = 1u;
    }

    /* R-A: auto-narrow the local vehicle to the resolved track's mask BEFORE
     * publishing, so the reverse feed always carries a mask-legal vehicle. A
     * joiner narrows against the host's locked track (configured_track) when the
     * host has set one; otherwise against its hovered column. */
    resolvedTrack = trackselect_resolved_track();
    if (!sTs.host && haveSnap && snap.configured_track != 0xFFFFu) {
        const MdkrPartyLinkSnapshot *sp = &snap;
        u16 cfg = sp->configured_track;
        if (cfg <= 0xFFu) {
            resolvedTrack = (u8) cfg;
        }
    }
    trackselect_autonarrow_vehicle(resolvedTrack, occupied);
    mask = trackselect_track_mask(resolvedTrack, occupied);

    /* Local pad drives the cursor / mode / lock / start (host); B backs out. */
    trackselect_gather_input(&in);
    trackselect_apply_input(&in);

    /* Recompute the resolved track after input (the cursor may have moved) so the
     * published vehicle and the witness reflect this frame's selection. */
    resolvedTrack = trackselect_resolved_track();
    if (!sTs.host && haveSnap && snap.configured_track != 0xFFFFu &&
        snap.configured_track <= 0xFFu) {
        resolvedTrack = (u8) snap.configured_track;
    }
    trackselect_autonarrow_vehicle(resolvedTrack, occupied);
    mask = trackselect_track_mask(resolvedTrack, occupied);

    /* Continuous reverse-feed publish (see trackselect_publish_intent / R-B). */
    trackselect_publish_intent(localSeatChar);

    /* Native render into the engine frame's display list. */
    trackselect_render(&snap, haveSnap,
                       haveSnap ? snap.host_cursor.valid : 0u,
                       haveSnap ? (u8) snap.host_cursor.x : 0u,
                       haveSnap ? (u8) snap.host_cursor.row : 0u);
    trackselect_witness(&snap, haveSnap, resolvedTrack, mask,
                        sTs.lockedTrack != TS_NONE
                            ? (s32) sTrackIds[sTs.lockedTrack]
                            : -1,
                        sTs.lockedCup != TS_NONE ? (s32) sTs.lockedCup : -1);

    /* Headless test seam: reduce the intent into the scripted room + script the
     * host-start. Inert (installs nothing) in a normal run. */
    trackselect_test_reduce_and_script();

    sTs.ticks++;

    /* The authoritative lobby leaving LOBBY (host started -> loading) is the
     * signal to boot, and it WINS over a pending back-out. */
    if (haveSnap && snap.phase != (uint8_t) TS_LOBBY_PHASE) {
        fprintf(stderr,
                "[online-trackselect] advance: lobby left LOBBY (phase=%u) -> "
                "boot race\n",
                (unsigned) snap.phase);
        return MDKR_ONLINE_TRACKSELECT_ADVANCE;
    }
    if (sTs.leave) {
        sTs.leave = 0u; /* edge: return LEAVE once, never shadow ADVANCE */
        fprintf(stderr, "[online-trackselect] back to charselect\n");
        return MDKR_ONLINE_TRACKSELECT_LEAVE;
    }
    return MDKR_ONLINE_TRACKSELECT_STAY;
}

/* ======================================================================== *
 * Headless test seam (beta + env gated; entirely inert in a normal run)
 *
 * Stands in for the launcher during a headless TRACKSELECT lane. Unlike the
 * CHARSELECT seam (which installs a fresh room), this one ADOPTS the room the
 * CHARSELECT seam converged (the lane runs both), then acts as a minimal reducer
 * that faithfully models the launcher: it converges the local seat, applies the
 * host's SET_MODE/SET_CONFIG_TRACK/SET_CUP with the SAME change-detect the plan
 * dedupe uses AND the reducer's ready-clear (every member's ready drops on a
 * config change), re-asserts the scripted joiner's ready (its continuous
 * republish), reconverges the host's ready from the intent, and flips the lobby
 * to LOADING on the host's start. Nothing here runs unless
 * MDKR_TEST_ONLINE_TRACKSELECT is set.
 * ======================================================================== */
static s8 sTsTestActive = -1; /* -1 unresolved, 0 off, 1 on */
static u8 sTsAdopted;
static MdkrPartyLinkSnapshot sTsRoom;
static u8 sTsStartArmed;

static void trackselect_test_resolve(void) {
    if (sTsTestActive < 0) {
        sTsTestActive = (getenv("MDKR_TEST_ONLINE_TRACKSELECT") != NULL) ? 1 : 0;
    }
}

/* Called from _enter so a re-entered TRACKSELECT re-adopts the current room. */
static void trackselect_test_reset(void) {
    trackselect_test_resolve();
    if (!sTsTestActive) {
        return;
    }
    sTsAdopted = 0u;
    sTsStartArmed = 0u;
}

void mdkr_online_trackselect_test_lobby_pump(void) {
    trackselect_test_resolve();
    if (!sTsTestActive) {
        return;
    }
    /* The CHARSELECT seam owns install + the LOBBY room in this lane; only stand
     * one up ourselves if nothing else has (defensive / standalone use). */
    if (!mdkr_party_link_active()) {
        MdkrPartyLinkSnapshot room;
        unsigned i;
        memset(&room, 0, sizeof(room));
        room.phase = (uint8_t) TS_LOBBY_PHASE;
        for (i = 0u; i < MDKR_PARTY_LINK_SEATS; i++) {
            room.seats[i].character_id = TS_NO_CHARACTER;
            room.seats[i].vehicle_id = TS_NO_VEHICLE;
        }
        room.configured_track = 0xFFFFu;
        room.cup_id = 0xFFu;
        room.seats[0].occupied = 1u;
        room.seats[0].is_local = 1u;
        room.seats[0].is_host = 1u;
        room.seats[0].connected = 1u;
        room.seats[0].character_id = 0u;
        room.seats[0].vehicle_id = 0u;
        room.seats[1].occupied = 1u;
        room.seats[1].connected = 1u;
        room.seats[1].character_id = 5u;
        room.seats[1].vehicle_id = 0u;
        room.seats[1].ready = 1u;
        memcpy(room.seats[1].name, "RIVAL", sizeof("RIVAL"));
        mdkr_party_link_clear();
        (void) mdkr_party_link_install();
        mdkr_party_link_publish(&room);
        fprintf(stderr,
                "[online-trackselect] test-script standalone install\n");
    }
}

static void trackselect_test_reduce_and_script(void) {
    MdkrPartyLinkLocalIntent intent;
    u8 configChanged = 0u;

    trackselect_test_resolve();
    if (!sTsTestActive || !mdkr_party_link_active()) {
        return;
    }

    if (!sTsAdopted) {
        /* Adopt the room the CHARSELECT seam converged (seat0 confirmed+ready,
         * seat1 RIVAL ready, phase LOBBY). */
        if (!mdkr_party_link_read(&sTsRoom)) {
            return;
        }
        if (sTsRoom.configured_track == 0u) {
            sTsRoom.configured_track = 0xFFFFu; /* normalise "none" */
        }
        sTsAdopted = 1u;
    }

    if (mdkr_party_link_intent_poll(&intent)) {
        /* Converge the local seat's character/vehicle. */
        if (intent.confirmed && intent.hover_character < TS_CHAR_COUNT) {
            sTsRoom.seats[0].character_id = intent.hover_character;
        }
        if (intent.vehicle_id < TS_PLAYER_VEHICLE_COUNT) {
            sTsRoom.seats[0].vehicle_id = intent.vehicle_id;
        }
        /* Host session config -- change-detected exactly like the plan dedupe,
         * and each accepted change clears EVERY member's ready (the reducer's
         * behaviour, lobby_core.c:756-757). */
        if (intent.mode != MDKR_PARTY_LINK_MODE_UNSET &&
            sTsRoom.mode != intent.mode) {
            sTsRoom.mode = intent.mode;
            configChanged = 1u;
        }
        if (intent.config_track != MDKR_PARTY_LINK_TRACK_UNSET &&
            sTsRoom.configured_track != intent.config_track) {
            sTsRoom.configured_track = intent.config_track;
            configChanged = 1u;
        }
        if (intent.cup_id != MDKR_PARTY_LINK_CUP_UNSET &&
            sTsRoom.cup_id != intent.cup_id) {
            sTsRoom.cup_id = intent.cup_id;
            configChanged = 1u;
        }
        if (configChanged) {
            /* Ready-clear: publish the un-ready state THIS tick so the lane can
             * witness the clear; the continuous republish reconverges next tick. */
            sTsRoom.seats[0].ready = 0u;
            sTsRoom.seats[1].ready = 0u;
            sTsStartArmed = 0u;
        } else {
            /* Reconverge: the scripted joiner always re-asserts ready (its own
             * continuous republish), and the host's ready follows its intent. */
            sTsRoom.seats[1].ready = 1u;
            if (intent.ready &&
                sTsRoom.seats[0].character_id != TS_NO_CHARACTER &&
                sTsRoom.seats[0].vehicle_id != TS_NO_VEHICLE) {
                sTsRoom.seats[0].ready = 1u;
            } else if (intent.backout) {
                sTsRoom.seats[0].ready = 0u;
            }
        }

        /* Host-start: once a track/cup is locked and both seats are ready, the
         * host's start_requested drives the lobby to LOADING after a short hold
         * (so the converged, both-ready screen is genuinely rendered). */
        {
            bool configReady =
                (sTsRoom.mode == TS_MODE_TOURNAMENT)
                    ? (sTsRoom.cup_id != 0xFFu)
                    : (sTsRoom.configured_track != 0xFFFFu);
            if (intent.start_requested && configReady &&
                sTsRoom.seats[0].ready && sTsRoom.seats[1].ready) {
                sTsStartArmed++;
                if (sTsStartArmed >= 6u) {
                    sTsRoom.phase = (uint8_t) TS_LOADING_PHASE;
                }
            }
        }
    }

    mdkr_party_link_publish(&sTsRoom);
}

u8 mdkr_online_trackselect_test_active(void) {
    trackselect_test_resolve();
    return (u8) (sTsTestActive > 0 ? 1 : 0);
}

#endif /* MDKR_ENABLE_ONLINE_BETA */
