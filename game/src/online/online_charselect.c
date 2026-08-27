/* SEPARATED-BOOT-PATH (Strategy D2) native online CHARACTER/VEHICLE select.
 *
 * ============================ THE D2 REUSE BOUNDARY ========================
 * This screen is the first player-facing SCREEN of the separated online flow.
 * Strategy D2 means: RE-IMPLEMENT the presentation here using the GAME'S OWN
 * decoded assets, rather than calling the offline menu's character-select loop.
 * That is the whole point -- it lets a decomp do what a static recompilation
 * cannot: draw the real N64 racer portraits, play the real DKR menu SFX, and use
 * the real DKR font, natively, without any custom chrome.
 *
 * WHAT IT BORROWS (read-only reuse of already-compiled game code/data; NO edit
 * to menu.c is required -- every symbol below already has external linkage):
 *   - gRacerPortraits[10]        (menu.c) the decoded racer portrait textures.
 *   - menu_assetgroup_load/free  (menu.c) the same texture-group loader the
 *                                offline results screen uses; we hand it a
 *                                portrait-ONLY id list (TEXTURE_ICON_PORTRAIT_*)
 *                                so it loads exactly the ten portraits and
 *                                spawns no menu objects.
 *   - menu_racer_portraits       (menu.c) binds those loaded textures into
 *                                gRacerPortraits[] -- the exact call the results
 *                                screen makes right after loading the group.
 *   - draw_text / set_text_*     (font.h) the real DKR font.
 *   - texrect_draw / bgdraw_*    (rcp_dkr.h) the game's own 2D blit, drawing
 *                                straight into the engine frame's gCurrDisplayList.
 *   - sound_play + SOUND_*       (audio.h / sound_ids.h) the real menu SFX, the
 *                                same enums/API menu.c uses (e.g. menu.c:4564).
 *   - input_pressed / stick      (joypad.h) the real pad, local player only.
 *   - get_player_selected_vehicle(menu.c) the SAME default vehicle value
 *                                menu_online_versus_race_setup() applies at boot,
 *                                so the seat can legally READY here (the reducer
 *                                refuses READY without a vehicle) without our
 *                                inventing one.
 *
 * WHAT IT OWNS (all state lives HERE, never an offline global): the local
 * cursor, the confirm/ready latches, the continuous reverse-feed intent, and the
 * whole 2D layout. The forward feed (both seats' picks, ready and the remote
 * name) is read from platform/net/party_link; the remote seat is display-only.
 *
 * WHY IT DOES NOT CALL THE OFFLINE _loop: the offline charselect_* path drives
 * the offline GAMEMODE_MENU state machine (gCurrentMenuId, its own transitions,
 * local split-screen player assignment, AI fill, music state). Entering it would
 * defeat the separated-boot isolation guarantee that keeps the shipped offline
 * game provably unimpacted, and it has no notion of a remote seat or the
 * party_link feeds. So we reuse the DATA and the DRAW/SFX primitives, not the loop.
 *
 * The ENTIRE TU is #if MDKR_ENABLE_ONLINE_BETA and it is added to the build only
 * inside the beta CMake gate (game/src/online/ is not globbed), so a normal
 * (beta OFF) build never compiles a byte of it and the release engine object is
 * untouched.
 * ==========================================================================
 */
#include "online/online_charselect.h"

#if MDKR_ENABLE_ONLINE_BETA

/* Game/PR headers FIRST: rcp_dkr.h pulls ultra64.h -> PR/os_libc.h, which
 * declares sprintf() as a plain function. If the system <stdio.h> were included
 * before it, sprintf would already be a fortify macro and that declaration would
 * fail to compile -- the exact ordering thread3_main.c relies on. */
#include "types.h"
#include "thread3_main.h"
#include "enums.h"      /* AlignmentFlags, VEHICLE_CAR */
#include "menu.h"       /* gRacerPortraits, menu_assetgroup_load/free,
                           menu_racer_portraits, get_player_selected_vehicle,
                           TEXTURE_ICON_PORTRAIT_*, font.h (draw_text, ...) */
#include "rcp_dkr.h"    /* texrect_draw, bgdraw_fillcolour */
#include "audio.h"      /* sound_play */
#include "sound_ids.h"  /* SOUND_MENU_PICK2 / SOUND_SELECT2 / ... */
#include "joypad.h"     /* input_pressed, input_clamp_stick_x/y */
#include "PR/os_cont.h" /* A_BUTTON / B_BUTTON / *_JPAD / START_BUTTON */
#include "net/party_link.h"
#include "online/online_trackselect.h" /* PD-T3: defer self-start to TRACKSELECT */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Screen space (SCREEN_WIDTH/HEIGHT live in camera.h/video.h; mirrored here so
 * this TU does not pull those in just for two constants). */
#define CS_SCREEN_W 320
#define CS_SCREEN_H 240
#define CS_SCREEN_W_HALF 160

/* The engine's live 2D display list for the current frame. Set up by
 * main_game_loop() BEFORE the mode switch (rsp_init/rdp_init/bgdraw_render) and
 * closed after it (gDPFullSync/gSPEndDisplayList), so the GAMEMODE_ONLINE_SESSION
 * case can draw straight into it -- the same list menu_missing_controller() draws
 * into from the shared frame tail. Declared here (no shared header exposes it)
 * exactly as online_session.c declares gGameMode. */
extern Gfx *gCurrDisplayList;

/* The decoded racer portraits. Declared here (menu.h does not expose the array)
 * rather than by editing menu.c. gRacerPortraits[k] is a DrawTexture[2]:
 * element[0] is the portrait, element[1] the NULL terminator texrect_draw stops
 * on. NOTE the array is in its OWN portrait order, NOT the Character enum order
 * and NOT the online id order -- see sOnlineToPortrait[] below. */
extern DrawTexture *gRacerPortraits[10];

/* ---- Local mirrors of the launcher lobby's id space -----------------------
 * party_link.h is deliberately dependency-free, so (exactly like online_session.c)
 * we mirror the handful of lobby_core.h constants we need rather than pull the
 * launcher headers into an engine TU. Kept in lock-step by the comment. */
#define CS_CHAR_COUNT 10u            /* MDKR_ONLINE_CHARACTER_COUNT */
#define CS_NO_CHARACTER 0xFFu        /* MDKR_ONLINE_NO_CHARACTER */
#define CS_NO_VEHICLE 0xFFu          /* MDKR_ONLINE_NO_VEHICLE */
#define CS_PLAYER_VEHICLE_COUNT 3u   /* car / hovercraft / plane (0x07 mask) */
#define CS_LOBBY_PHASE 1u            /* MDKR_ONLINE_LOBBY */
#define CS_LOCAL_PAD 0               /* PLAYER_ONE */

/* ---- Grid geometry (5x2, screen space; SCREEN_WIDTH 320 x SCREEN_HEIGHT 240) */
#define CS_COLS 5
#define CS_ROWS 2
#define CS_CELL_W 60
#define CS_CELL_H 70
#define CS_GRID_X 22 /* top-left x of column 0's portrait */
#define CS_GRID_Y 44 /* top-left y of row 0's portrait */
#define CS_PORTRAIT_HALF 22 /* ~half a portrait, for centering labels */
#define CS_TAKEN_FLASH_TICKS 45u /* "TAKEN BY x" flash duration (~1.5s @ 30Hz) */

/* Menu SFX (the real DKR enums -- same reuse as the portraits; verified against
 * menu.c:4564/4577/4778). */
#define CS_SFX_MOVE SOUND_MENU_PICK2
#define CS_SFX_CONFIRM SOUND_SELECT2
#define CS_SFX_READY SOUND_SELECT3
#define CS_SFX_BACK SOUND_MENU_BACK3
#define CS_SFX_REJECT SOUND_ELECTRIC_BUZZ

/* ---- Online character id -> gRacerPortraits index -------------------------
 * THREE different orderings exist for the ten racers and mixing them silently
 * draws the wrong face, so the mapping is explicit and named:
 *
 *   online id space   (party_link seats[].character_id; ui_online_room kCharacters):
 *     0 Diddy 1 Timber 2 Pipsy 3 Tiptup 4 Conker 5 Bumper 6 Banjo 7 Krunch
 *     8 Drumstick 9 T.T.
 *   gRacerPortraits[] order (menu.c literal):
 *     0 Krunch 1 Diddy 2 Drumstick 3 Bumper 4 Banjo 5 Conker 6 Tiptup 7 T.T.
 *     8 Pipsy 9 Timber
 *
 * The screen is laid out in ONLINE id order (grid cell index == online char id
 * == what we publish as hover_character and what the reducer validates), so this
 * table maps that id to the portrait slot to blit. The headless lane emits the
 * resolved slot (witness `portrait=`) and asserts the mapping, so a swapped entry
 * is caught. */
static const u8 sOnlineToPortrait[CS_CHAR_COUNT] = {
    1u, /* 0 Diddy     -> gRacerPortraits[1] */
    9u, /* 1 Timber    -> [9] */
    8u, /* 2 Pipsy     -> [8] */
    6u, /* 3 Tiptup    -> [6] */
    5u, /* 4 Conker    -> [5] */
    3u, /* 5 Bumper    -> [3] */
    4u, /* 6 Banjo     -> [4] */
    0u, /* 7 Krunch    -> [0] */
    2u, /* 8 Drumstick -> [2] */
    7u, /* 9 T.T.      -> [7] */
};

/* Short display names, in online id order (matches the launcher's kCharacters). */
static const char *const sOnlineNames[CS_CHAR_COUNT] = {
    "DIDDY", "TIMBER", "PIPSY", "TIPTUP", "CONKER",
    "BUMPER", "BANJO", "KRUNCH", "DRUMSTICK", "T.T.",
};

/* ---- Session-owned screen state (never an offline global) ------------------ */
typedef struct MdkrOnlineCharselectState {
    u8 cursor;         /* online char id under the cursor (grid cell) */
    u8 confirmed;      /* local player has locked a character */
    u8 ready;          /* local player is ready (the single ready-truth: P3) */
    u8 vehicle;        /* mask-legal default vehicle id published with the pick */
    u8 assets;         /* portrait group + fonts are loaded */
    u8 leave;          /* B-in-browse: leave request (edge; see tick) */
    u8 seeded;         /* P1: first-snapshot cursor seed applied */
    u32 ticks;         /* CHARSELECT ticks elapsed (also drives the test input) */
    u32 takenFlashEnd; /* F3: "TAKEN BY x" flash deadline, in ticks */
    s8 stickLatchX;
    s8 stickLatchY;
} MdkrOnlineCharselectState;

static MdkrOnlineCharselectState sCs;

/* P1: persists ACROSS entries (deliberately NOT reset by _enter's memset) so a
 * tournament group's next race restarts on the racer you last confirmed, not
 * always Diddy. */
static u8 sLastConfirmedChar;

/* M3: witness change-detect state at file scope so _enter() can reset it for a
 * clean second entry. */
static u32 sWitnessKey = 0xFFFFFFFFu;
static s32 sWitnessRemoteSeat = -2;

/* The exact portrait texture ids (TEXTURE_ICON_PORTRAIT_KRUNCH .. _TIMBER, all
 * ASSET_MASK_TEXTURE assets) plus the -1 terminator menu_assetgroup_load stops
 * on. A portrait-only list: menu_asset_load routes every entry to load_texture,
 * so this loads the ten faces and spawns NO menu objects (unlike the results
 * screen's mixed group). menu_racer_portraits() then binds them. */
static s16 sPortraitAssetIds[] = {
    TEXTURE_ICON_PORTRAIT_KRUNCH, TEXTURE_ICON_PORTRAIT_DIDDY,
    TEXTURE_ICON_PORTRAIT_DRUMSTICK, TEXTURE_ICON_PORTRAIT_BUMPER,
    TEXTURE_ICON_PORTRAIT_BANJO, TEXTURE_ICON_PORTRAIT_CONKER,
    TEXTURE_ICON_PORTRAIT_TIPTUP, TEXTURE_ICON_PORTRAIT_TT,
    TEXTURE_ICON_PORTRAIT_PIPSY, TEXTURE_ICON_PORTRAIT_TIMBER,
    -1,
};

/* Resolved (display-only) view of the remote seat, with a bounded, NUL-forced
 * name copy -- seat->name is untrusted (T6 makes it remote-controlled). */
typedef struct CsRemoteView {
    s8 seat;      /* remote seat index, -1 when none */
    u8 present;   /* an occupied remote seat exists */
    u8 character; /* CS_NO_CHARACTER when no pick yet */
    u8 ready;
    char name[MDKR_PARTY_LINK_NAME_BYTES + 1u];
} CsRemoteView;

/* ---- forward decls (test seam defined at the bottom) ---------------------- */
static void charselect_test_resolve(void);
static void charselect_test_reset(void);
static void charselect_test_reduce_and_script(void);

/* ======================================================================== *
 * Input
 * ======================================================================== */
typedef struct CsInput {
    s8 dx;      /* -1 / 0 / +1 column step (edge) */
    s8 dy;      /* -1 / 0 / +1 row step (edge) */
    u8 aEdge;   /* A/Start pressed this frame */
    u8 bEdge;   /* B pressed this frame */
} CsInput;

/* Scripted headless input (env MDKR_TEST_ONLINE_CHARSELECT): move the cursor two
 * cells right (to Pipsy, online id 2), press B once WHILE BROWSING (the I1
 * no-wedge coverage), confirm, then ready. Deterministic and inert in a normal
 * run. Exercises the SAME cursor/confirm/ready logic the live pad drives. */
static void charselect_input_scripted(CsInput *in) {
    memset(in, 0, sizeof(*in));
    switch (sCs.ticks) {
    case 2u:
        in->dx = 1; /* 0 -> 1 */
        break;
    case 3u:
        in->bEdge = 1u; /* B while browsing: must NOT wedge the session */
        break;
    case 4u:
        in->dx = 1; /* 1 -> 2 (Pipsy) */
        break;
    case 6u:
        in->aEdge = 1u; /* confirm */
        break;
    case 12u:
        in->aEdge = 1u; /* ready */
        break;
    default:
        break;
    }
}

/* Live pad: D-pad edges plus a latched analog stick, local player only. */
static void charselect_input_live(CsInput *in) {
    u32 pressed = input_pressed(CS_LOCAL_PAD);
    s32 sx = input_clamp_stick_x(CS_LOCAL_PAD);
    s32 sy = input_clamp_stick_y(CS_LOCAL_PAD);
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

    /* Analog stick, edge-latched so one push moves one cell. Deliberate move
     * threshold (well past JOYSTICK_DEADZONE) avoids drift. Screen y is down, so
     * a stick pushed up (+sy) steps to the row above (-1). */
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
    if (in->dx == 0 && wantX != 0 && sCs.stickLatchX == 0) {
        in->dx = wantX;
    }
    if (in->dy == 0 && wantY != 0 && sCs.stickLatchY == 0) {
        in->dy = wantY;
    }
    sCs.stickLatchX = wantX;
    sCs.stickLatchY = wantY;

    in->aEdge = (pressed & (A_BUTTON | START_BUTTON)) ? 1u : 0u;
    in->bEdge = (pressed & B_BUTTON) ? 1u : 0u;
}

static void charselect_gather_input(CsInput *in) {
    if (mdkr_online_charselect_test_active()) {
        charselect_input_scripted(in);
    } else {
        charselect_input_live(in);
    }
}

/* Apply one input step to the local cursor/confirm/ready latches. `remoteChar`
 * is the remote seat's taken racer (CS_NO_CHARACTER when none) so a confirm on a
 * taken cell can be REJECTED (ruling: DISALLOW -- matches offline DKR and avoids
 * the SELECTION_CONFLICT divergence). */
static void charselect_apply_input(const CsInput *in, u8 remoteChar) {
    if (!sCs.confirmed) {
        /* Browsing: move the cursor over the grid. Columns wrap (native DKR 2D
         * menus wrap); rows clamp. */
        s32 col = (s32) (sCs.cursor % CS_COLS);
        s32 row = (s32) (sCs.cursor / CS_COLS);
        u8 previous = sCs.cursor;
        col = (col + in->dx + CS_COLS) % CS_COLS;
        row += in->dy;
        if (row < 0) {
            row = 0;
        }
        if (row >= CS_ROWS) {
            row = CS_ROWS - 1;
        }
        sCs.cursor = (u8) (row * CS_COLS + col);
        if (sCs.cursor != previous) {
            sound_play(CS_SFX_MOVE, NULL);
            /* PD-T2 nit: end the "TAKEN BY x" flash as soon as the cursor leaves
             * the taken cell, so it cannot linger ~1.5s while hovering elsewhere. */
            sCs.takenFlashEnd = 0u;
        }
        if (in->aEdge) {
            if (remoteChar != CS_NO_CHARACTER && sCs.cursor == remoteChar) {
                /* DISALLOW: do not publish a confirm for a taken racer; flash a
                 * TAKEN notice and play a negative cue. */
                sCs.takenFlashEnd = sCs.ticks + CS_TAKEN_FLASH_TICKS;
                sound_play(CS_SFX_REJECT, NULL);
            } else {
                sCs.confirmed = 1u;
                sLastConfirmedChar = sCs.cursor; /* P1 persistence */
                sound_play(CS_SFX_CONFIRM, NULL);
            }
        } else if (in->bEdge) {
            /* Leave-to-launcher PLUMBING only (kept for PD-T6); it is NOT wired
             * yet, so the browse help never advertises B. Edge-handled in the
             * tick (cleared after one return) so it can never wedge the session. */
            sCs.leave = 1u;
        }
        return;
    }
    /* Confirmed: A readies, B steps back one level (unready, then unconfirm). */
    if (in->aEdge) {
        sCs.ready = 1u;
        sound_play(CS_SFX_READY, NULL);
    } else if (in->bEdge) {
        if (sCs.ready) {
            sCs.ready = 0u;
        } else {
            sCs.confirmed = 0u;
        }
        sound_play(CS_SFX_BACK, NULL);
    }
}

/* Publish the FULL local intent every frame (continuous republish is required:
 * the launcher-side reducer dedupes against lobby convergence, never against a
 * one-shot send -- see platform/net/party_link.h). Field semantics match the
 * reverse-feed contract:
 *   confirmed  -> CHOOSE_CHARACTER(hover_character)   [only sent once confirmed]
 *   vehicle_id -> CHOOSE_VEHICLE(vehicle)             [always a mask-legal id]
 *   ready      -> SET_READY 1
 *   backout    -> SET_READY 0 (converges when not ready; a no-op while browsing)
 * start_requested stays 0 -- host-start belongs to the track screen (PD-T3). */
static void charselect_publish_intent(void) {
    MdkrPartyLinkLocalIntent intent;
    /* C1: start from the shared baseline so the host-only session-config fields
     * (mode/config_track/cup_id) carry their UNSET sentinels, NOT a bare zero --
     * charselect is never a host-config screen, so it must never dispatch
     * SET_MODE(0)/SET_CONFIG_TRACK(0)/SET_CUP(0) once the reverse pump goes live. */
    mdkr_party_link_intent_init(&intent);
    intent.hover_character = sCs.cursor;
    intent.vehicle_id = sCs.vehicle;
    intent.confirmed = sCs.confirmed;
    intent.ready = sCs.ready;
    intent.backout = sCs.ready ? 0u : 1u;
    intent.start_requested = 0u;
    mdkr_party_link_intent_publish(&intent);
}

/* ======================================================================== *
 * Render (native: real portraits + real font, into the engine frame list)
 * ======================================================================== */
static void charselect_draw_portrait(u8 onlineId, u8 r, u8 g, u8 b) {
    s32 col = (s32) (onlineId % CS_COLS);
    s32 row = (s32) (onlineId / CS_COLS);
    s32 x = CS_GRID_X + col * CS_CELL_W;
    s32 y = CS_GRID_Y + row * CS_CELL_H;
    DrawTexture *portrait = gRacerPortraits[sOnlineToPortrait[onlineId]];
    if (portrait != NULL && portrait[0].texture != NULL) {
        texrect_draw(&gCurrDisplayList, portrait, x, y, r, g, b, 255);
    }
}

static void charselect_draw_label(u8 onlineId, s32 dy, s32 fontId, char *text,
                                   s32 r, s32 g, s32 b) {
    s32 col = (s32) (onlineId % CS_COLS);
    s32 row = (s32) (onlineId / CS_COLS);
    s32 cx = CS_GRID_X + col * CS_CELL_W + CS_PORTRAIT_HALF;
    s32 y = CS_GRID_Y + row * CS_CELL_H + dy;
    set_text_font(fontId);
    set_text_background_colour(0, 0, 0, 0);
    set_text_colour(r, g, b, 0, 255);
    draw_text(&gCurrDisplayList, cx, y, text, ALIGN_MIDDLE_CENTER);
}

static void charselect_draw_text_at(s32 x, s32 y, s32 fontId, char *text,
                                    AlignmentFlags align, s32 r, s32 g, s32 b) {
    set_text_font(fontId);
    set_text_background_colour(0, 0, 0, 0);
    set_text_colour(r, g, b, 0, 255);
    draw_text(&gCurrDisplayList, x, y, text, align);
}

/* Which snapshot seat is the local one (display-only lookups). */
static s32 charselect_local_seat(const MdkrPartyLinkSnapshot *snap) {
    unsigned i;
    for (i = 0u; i < MDKR_PARTY_LINK_SEATS; i++) {
        if (snap->seats[i].occupied && snap->seats[i].is_local) {
            return (s32) i;
        }
    }
    return -1;
}

/* Resolve the (first occupied, non-local) remote seat into a bounded view.
 * NOTE fenced 2-endpoint beta: first remote seat only (code M6). T6 revisits for
 * true 4-player rendering. */
static void charselect_resolve_remote(const MdkrPartyLinkSnapshot *snap,
                                      bool haveSnap, s32 localSeat,
                                      CsRemoteView *out) {
    unsigned i;
    memset(out, 0, sizeof(*out));
    out->seat = -1;
    out->character = CS_NO_CHARACTER;
    if (!haveSnap) {
        return;
    }
    for (i = 0u; i < MDKR_PARTY_LINK_SEATS; i++) {
        const MdkrPartyLinkSeat *seat = &snap->seats[i];
        if (!seat->occupied || (s32) i == localSeat) {
            continue;
        }
        out->seat = (s8) i;
        out->present = 1u;
        if (seat->character_id < CS_CHAR_COUNT) {
            out->character = seat->character_id;
        }
        out->ready = seat->ready ? 1u : 0u;
        /* Untrusted: bounded copy, never assume NUL termination (M5 / F6). */
        memcpy(out->name, seat->name, MDKR_PARTY_LINK_NAME_BYTES);
        out->name[MDKR_PARTY_LINK_NAME_BYTES] = '\0';
        break;
    }
}

static void charselect_render(const MdkrPartyLinkSnapshot *snap, bool haveSnap,
                              s32 localSeat, const CsRemoteView *rv) {
    const char *rname = rv->name[0] != '\0' ? rv->name : "RIVAL";
    /* P4: triangle-wave pulse (0..16) off the tick counter for a native cursor
     * highlight feel with zero assets. */
    s32 tri = (s32) (sCs.ticks & 31u);
    u8 id;

    (void) snap;
    (void) localSeat;
    if (tri > 16) {
        tri = 32 - tri;
    }

    /* Title. */
    charselect_draw_text_at(CS_SCREEN_W_HALF, 18, ASSET_FONTS_BIGFONT,
                            "CHOOSE YOUR RACER", ALIGN_MIDDLE_CENTER, 255, 224,
                            96);

    /* The grid: every racer's portrait + name. Colour + shape carry the state so
     * a colorblind player still reads it: cursor = pulsing gold + >NAME< brackets
     * (F7), local confirmed pick = green + YOU tag, remote's taken pick = big
     * luminance drop + name tag. */
    for (id = 0u; id < CS_CHAR_COUNT; id++) {
        u8 pr = 210u, pg = 210u, pb = 210u;
        s32 nr = 200, ng = 200, nb = 200;
        bool taken = (rv->character != CS_NO_CHARACTER && id == rv->character);
        bool onCursor = (id == sCs.cursor);
        bool localPick = (sCs.confirmed && id == sCs.cursor);
        char label[24];

        if (taken) {
            pr = pg = pb = 80u;
            nr = ng = nb = 90;
        }
        if (onCursor && !localPick) {
            if (taken) {
                /* Dimmed gold: keep BOTH "this is my cursor" and "this is taken"
                 * legible (also aids colorblind players) -- F3.2. */
                pr = 200u;
                pg = 170u;
                pb = 80u;
                nr = 200;
                ng = 170;
                nb = 80;
            } else {
                pr = 255u;
                pg = (u8) (190 + tri * 4);
                pb = (u8) (60 + tri * 3);
                nr = 255;
                ng = 190 + tri * 4;
                nb = 60 + tri * 3;
            }
        }
        if (localPick) {
            pr = 120u;
            pg = 255u;
            pb = 120u;
            nr = 120;
            ng = 255;
            nb = 120;
        }

        charselect_draw_portrait(id, pr, pg, pb);
        /* Shape redundancy for the hover cursor (F7). */
        if (onCursor) {
            (void) snprintf(label, sizeof(label), ">%s<", sOnlineNames[id]);
        } else {
            (void) snprintf(label, sizeof(label), "%s", sOnlineNames[id]);
        }
        charselect_draw_label(id, 46, ASSET_FONTS_SMALLFONT, label, nr, ng, nb);

        /* Seat markers on separate rows so they never overprint during the brief
         * same-character latency window (F3.3). */
        if (localPick) {
            charselect_draw_label(id, 56, ASSET_FONTS_SMALLFONT, "YOU", 120, 255,
                                  120);
        }
        if (taken) {
            char tag[16];
            (void) snprintf(tag, sizeof(tag), "%.7s", rname); /* F6 */
            charselect_draw_label(id, 66, ASSET_FONTS_SMALLFONT, tag, 255, 160,
                                  160);
        }
    }

    /* Status lines: drawn edge-anchored (F1 -- render_text_string subtracts half
     * the width from x, so a CENTER-aligned edge string clips off-screen). Local
     * status is driven by the ready LATCH (single ready-truth, P3). */
    {
        char line[64];
        const char *you = sCs.ready ? "READY" : (sCs.confirmed ? "PICKED"
                                                              : "CHOOSING");
        (void) snprintf(line, sizeof(line), "YOU: %s", you);
        charselect_draw_text_at(24, 196, ASSET_FONTS_SMALLFONT, line,
                                ALIGN_MIDDLE_LEFT, sCs.ready ? 120 : 220,
                                sCs.ready ? 255 : 220, sCs.ready ? 120 : 220);

        /* Right status is ALWAYS drawn (F2): a first-time host must see the
         * remote's presence/waiting state, not an empty half. */
        if (!rv->present) {
            charselect_draw_text_at(CS_SCREEN_W - 24, 196, ASSET_FONTS_SMALLFONT,
                                    "WAITING FOR PLAYER...", ALIGN_MIDDLE_RIGHT,
                                    150, 150, 150);
        } else {
            (void) snprintf(line, sizeof(line), "%.12s: %s", rname,
                            rv->ready ? "READY" : "CHOOSING"); /* F6 */
            charselect_draw_text_at(CS_SCREEN_W - 24, 196, ASSET_FONTS_SMALLFONT,
                                    line, ALIGN_MIDDLE_RIGHT,
                                    rv->ready ? 120 : 220, rv->ready ? 255 : 220,
                                    rv->ready ? 120 : 220);
        }
    }

    /* Context help / transient TAKEN flash (object-complete copy, F5). Browse
     * never advertises B (leave-to-launcher is not wired). */
    if (sCs.ticks < sCs.takenFlashEnd) {
        char msg[32];
        (void) snprintf(msg, sizeof(msg), "TAKEN BY %.7s", rname);
        charselect_draw_text_at(CS_SCREEN_W_HALF, 224, ASSET_FONTS_SMALLFONT, msg,
                                ALIGN_MIDDLE_CENTER, 255, 80, 80);
    } else if (!sCs.confirmed) {
        charselect_draw_text_at(CS_SCREEN_W_HALF, 224, ASSET_FONTS_SMALLFONT,
                                "A: SELECT", ALIGN_MIDDLE_CENTER, 255, 255, 255);
    } else if (!sCs.ready) {
        charselect_draw_text_at(CS_SCREEN_W_HALF, 224, ASSET_FONTS_SMALLFONT,
                                "A: READY   B: CHANGE PICK", ALIGN_MIDDLE_CENTER,
                                255, 255, 255);
    } else {
        char msg[64];
        if (rv->ready) {
            (void) snprintf(msg, sizeof(msg),
                            "WAITING FOR HOST TO START...   B: UNREADY");
        } else {
            (void) snprintf(msg, sizeof(msg),
                            "READY! WAITING FOR %.12s...   B: UNREADY", rname);
        }
        charselect_draw_text_at(CS_SCREEN_W_HALF, 224, ASSET_FONTS_SMALLFONT, msg,
                                ALIGN_MIDDLE_CENTER, 120, 255, 120);
    }
}

/* Bounded stderr witness: one line only when the visible state changes, so a
 * headless lane can read the drawn picks/names + published intent without
 * flooding. The change-detect key folds the remote pick/ready too (M2), so a
 * remote-only change still emits a row. */
static void charselect_witness(const MdkrPartyLinkSnapshot *snap, bool haveSnap,
                               s32 localSeat, const CsRemoteView *rv) {
    u8 localSeatChar = CS_NO_CHARACTER;
    u8 localSeatReady = 0u;
    u8 remoteNibble;
    u32 key;

    if (haveSnap && localSeat >= 0) {
        localSeatChar = snap->seats[localSeat].character_id;
        localSeatReady = snap->seats[localSeat].ready;
    }

    remoteNibble = (rv->character < CS_CHAR_COUNT) ? rv->character : 0xFu;
    key = ((u32) sCs.cursor) | ((u32) sCs.confirmed << 8) |
          ((u32) sCs.ready << 9) | ((u32) remoteNibble << 10) |
          ((u32) (rv->ready ? 1u : 0u) << 14) | ((u32) rv->present << 15) |
          ((u32) localSeatChar << 16) | ((u32) localSeatReady << 24);
    if (key == sWitnessKey && rv->seat == sWitnessRemoteSeat) {
        return;
    }
    sWitnessKey = key;
    sWitnessRemoteSeat = rv->seat;

    fprintf(stderr,
            "[online-charselect] render cursor=%u name=%s portrait=%u "
            "local{conf=%u ready=%u seatChar=%u seatReady=%u} "
            "remote{seat=%d char=%u ready=%u name=%.*s} "
            "intent{hover=%u vehicle=%u confirmed=%u ready=%u}\n",
            sCs.cursor, sOnlineNames[sCs.cursor],
            (unsigned) sOnlineToPortrait[sCs.cursor], sCs.confirmed, sCs.ready,
            (unsigned) localSeatChar, (unsigned) localSeatReady, (int) rv->seat,
            (unsigned) rv->character, (unsigned) rv->ready,
            (int) MDKR_PARTY_LINK_NAME_BYTES,
            rv->name[0] != '\0' ? rv->name : "-", sCs.cursor,
            (unsigned) sCs.vehicle, sCs.confirmed, sCs.ready);
}

/* ======================================================================== *
 * Lifecycle
 * ======================================================================== */
void mdkr_online_charselect_enter(void) {
    s8 defaultVehicle;

    memset(&sCs, 0, sizeof(sCs));
    /* P1: start on the last confirmed racer (or Diddy the first time). A valid
     * first snapshot may re-seed this (see tick). sLastConfirmedChar persists. */
    sCs.cursor = sLastConfirmedChar < CS_CHAR_COUNT ? sLastConfirmedChar : 0u;

    /* M3: reset the witness change-detect + the headless seam so a second entry
     * is clean (a re-entered CHARSELECT re-scripts from scratch). */
    sWitnessKey = 0xFFFFFFFFu;
    sWitnessRemoteSeat = -2;
    charselect_test_reset();

    /* Reuse the EXACT default vehicle menu_online_versus_race_setup() applies at
     * boot (get_player_selected_vehicle(PLAYER_ONE)); clamp to a base vehicle so
     * it is always inside the 0x07 player mask and the seat can legally READY
     * here (the reducer refuses READY without a vehicle). Real vehicle choice is
     * the track screen (PD-T3). */
    defaultVehicle = get_player_selected_vehicle(CS_LOCAL_PAD);
    if (defaultVehicle < 0 || (u8) defaultVehicle >= CS_PLAYER_VEHICLE_COUNT) {
        defaultVehicle = (s8) VEHICLE_CAR;
    }
    sCs.vehicle = (u8) defaultVehicle;

    /* Borrow the real portraits: load the portrait-only texture group and bind
     * them into gRacerPortraits[] -- the same two calls the offline results
     * screen makes. A clean solid backdrop reads as native without a 3D scene. */
    menu_assetgroup_load(sPortraitAssetIds);
    menu_racer_portraits();

    /* load_fonts() at boot only builds the font TABLE; each screen must load the
     * glyph textures for the fonts it draws with (refcounted; unload_font() on
     * exit). The offline charselect does exactly this for BIGFONT. */
    load_font(ASSET_FONTS_BIGFONT);
    load_font(ASSET_FONTS_SMALLFONT);

    sCs.assets = 1u;
    bgdraw_fillcolour(16, 24, 48);

    fprintf(stderr,
            "[online-charselect] enter: native screen up defaultVehicle=%u "
            "cursor=%u (portraits loaded, offline _loop bypassed)\n",
            (unsigned) sCs.vehicle, (unsigned) sCs.cursor);
}

void mdkr_online_charselect_exit(void) {
    if (sCs.assets) {
        /* Symmetric free: the same group loader's free path releases the ten
         * portrait textures (and, once the menu asset count returns to zero, the
         * shared portrait bookkeeping) before the race loader reuses the pool.
         * Balance the two load_font() refs taken in _enter(). */
        unload_font(ASSET_FONTS_SMALLFONT);
        unload_font(ASSET_FONTS_BIGFONT);
        menu_assetgroup_free(sPortraitAssetIds);
        sCs.assets = 0u;
        fprintf(stderr, "[online-charselect] exit: freed portrait assets\n");
    }
}

MdkrOnlineCharselectResult mdkr_online_charselect_tick(s32 updateRate) {
    MdkrPartyLinkSnapshot snap;
    bool haveSnap;
    s32 localSeat;
    CsRemoteView rv;
    CsInput in;

    (void) updateRate;

    /* Read the authoritative forward feed the launcher publishes (both seats'
     * picks/ready + the remote name). Display-only for the remote seat. */
    haveSnap = mdkr_party_link_read(&snap);
    localSeat = haveSnap ? charselect_local_seat(&snap) : -1;
    charselect_resolve_remote(&snap, haveSnap, localSeat, &rv);

    /* P1: seed the cursor from the first snapshot's local pick (e.g. a reconnect
     * lands on your existing racer), once, before input. */
    if (!sCs.seeded && haveSnap) {
        if (localSeat >= 0 &&
            snap.seats[localSeat].character_id < CS_CHAR_COUNT) {
            sCs.cursor = snap.seats[localSeat].character_id;
        }
        sCs.seeded = 1u;
    }

    /* Local pad drives the cursor / confirm / ready (taken racer disallowed). */
    charselect_gather_input(&in);
    charselect_apply_input(&in, rv.character);

    /* Continuous reverse-feed publish (see charselect_publish_intent). */
    charselect_publish_intent();

    /* Native render into the engine frame's display list. */
    charselect_render(&snap, haveSnap, localSeat, &rv);
    charselect_witness(&snap, haveSnap, localSeat, &rv);

    /* Headless test seam: reflect the intent into the scripted room + script the
     * host-start. Inert (and installs nothing) in a normal run. */
    charselect_test_reduce_and_script();

    sCs.ticks++;

    /* F4: the authoritative lobby leaving LOBBY (host started / loading) is the
     * signal to move on, and it WINS over a pending leave -- otherwise a stray B
     * would keep this endpoint from ever booting while the room raced on. */
    if (haveSnap && snap.phase != (uint8_t) CS_LOBBY_PHASE) {
        fprintf(stderr,
                "[online-charselect] advance: lobby left LOBBY (phase=%u) -> hand "
                "off\n",
                (unsigned) snap.phase);
        return MDKR_ONLINE_CHARSELECT_ADVANCE;
    }
    if (sCs.leave) {
        sCs.leave = 0u; /* edge: return LEAVE once, never shadow ADVANCE */
        return MDKR_ONLINE_CHARSELECT_LEAVE;
    }
    return MDKR_ONLINE_CHARSELECT_STAY;
}

/* ======================================================================== *
 * Headless test seam (beta + env gated; entirely inert in a normal run)
 *
 * This stands in for the launcher during a headless CHARSELECT lane: it installs
 * the REAL party_link forward feed, publishes a scripted 2-seat LOBBY room, then
 * each tick acts as a minimal reducer (reflect the polled intent into the local
 * seat) and scripts the host-start (flip the lobby to LOADING once both seats are
 * ready). It is the CHARSELECT analogue of online_session.c's LOBBY_WAIT seam.
 * Nothing here runs unless MDKR_TEST_ONLINE_CHARSELECT is set.
 * ======================================================================== */
static s8 sTestActive = -1; /* -1 unresolved, 0 off, 1 on */
static u8 sTestInstalled;
static MdkrPartyLinkSnapshot sTestRoom;
static u8 sTestStartArmed;

#define CS_TEST_REMOTE_CHARACTER 5u /* Bumper, in online id space */

static void charselect_test_resolve(void) {
    if (sTestActive < 0) {
        sTestActive = (getenv("MDKR_TEST_ONLINE_CHARSELECT") != NULL) ? 1 : 0;
    }
}

static void charselect_test_init_room(void) {
    unsigned i;
    memset(&sTestRoom, 0, sizeof(sTestRoom));
    sTestRoom.phase = (uint8_t) CS_LOBBY_PHASE;
    for (i = 0u; i < MDKR_PARTY_LINK_SEATS; i++) {
        sTestRoom.seats[i].character_id = CS_NO_CHARACTER;
        sTestRoom.seats[i].vehicle_id = CS_NO_VEHICLE;
    }
    /* Seat 0: the local player (also host). Seat 1: a scripted remote who has
     * already locked a character + readied, so the screen has a live remote pick
     * + name to render. */
    sTestRoom.seats[0].occupied = 1u;
    sTestRoom.seats[0].is_local = 1u;
    sTestRoom.seats[0].is_host = 1u;
    sTestRoom.seats[0].connected = 1u;
    sTestRoom.seats[1].occupied = 1u;
    sTestRoom.seats[1].connected = 1u;
    sTestRoom.seats[1].character_id = CS_TEST_REMOTE_CHARACTER;
    sTestRoom.seats[1].vehicle_id = 0u;
    sTestRoom.seats[1].ready = 1u;
    memcpy(sTestRoom.seats[1].name, "RIVAL", sizeof("RIVAL"));
}

/* M3: called from _enter so a re-entered CHARSELECT re-scripts from scratch. The
 * reducer gates on mdkr_party_link_active() (not sTestInstalled), so clearing the
 * install latch here does not disturb the current session -- it only forces a
 * fresh install on a future session's LOBBY_WAIT pump. */
static void charselect_test_reset(void) {
    charselect_test_resolve();
    if (!sTestActive) {
        return;
    }
    sTestStartArmed = 0u;
    sTestInstalled = 0u;
    charselect_test_init_room();
}

void mdkr_online_charselect_test_lobby_pump(void) {
    charselect_test_resolve();
    if (!sTestActive) {
        return;
    }
    if (!sTestInstalled) {
        charselect_test_init_room();
        mdkr_party_link_clear();
        (void) mdkr_party_link_install();
        sTestInstalled = 1u;
        fprintf(stderr,
                "[online-charselect] test-script install (scripted 2-seat LOBBY "
                "room; remote=%s ready)\n",
                sTestRoom.seats[1].name);
    }
    mdkr_party_link_publish(&sTestRoom);
}

static void charselect_test_reduce_and_script(void) {
    MdkrPartyLinkLocalIntent intent;

    charselect_test_resolve();
    if (!sTestActive || !mdkr_party_link_active()) {
        return;
    }

    /* Minimal launcher reducer: converge the local seat to the polled intent. */
    if (mdkr_party_link_intent_poll(&intent)) {
        if (intent.confirmed && intent.hover_character < CS_CHAR_COUNT) {
            sTestRoom.seats[0].character_id = intent.hover_character;
        }
        if (intent.vehicle_id < CS_PLAYER_VEHICLE_COUNT) {
            sTestRoom.seats[0].vehicle_id = intent.vehicle_id;
        }
        if (intent.ready && sTestRoom.seats[0].character_id != CS_NO_CHARACTER &&
            sTestRoom.seats[0].vehicle_id != CS_NO_VEHICLE) {
            sTestRoom.seats[0].ready = 1u;
        } else if (intent.backout) {
            sTestRoom.seats[0].ready = 0u;
        }
    }

    /* Script the host-start: once both seats are ready, hold a few frames (so the
     * converged, both-ready screen is genuinely rendered) then advance the lobby
     * to LOADING -- the authoritative signal the screen reacts to.
     *
     * PD-T3: when the TRACKSELECT headless seam is ALSO armed (the combined
     * trackselect lane), do NOT self-start here -- leave the room in LOBBY so the
     * session hands off CHARSELECT -> TRACKSELECT on the local-ready signal and
     * the TRACKSELECT seam drives the eventual host-start. The standalone
     * CHARSELECT lane (trackselect seam off) keeps its historical self-start. */
    if (sTestRoom.seats[0].ready && sTestRoom.seats[1].ready &&
        !mdkr_online_trackselect_test_active()) {
        sTestStartArmed++;
        if (sTestStartArmed >= 6u) {
            sTestRoom.phase = (uint8_t) (CS_LOBBY_PHASE + 1u); /* LOADING */
        }
    }

    mdkr_party_link_publish(&sTestRoom);
}

u8 mdkr_online_charselect_test_active(void) {
    charselect_test_resolve();
    return (u8) (sTestActive > 0 ? 1 : 0);
}

/* F-I1: the screen's OWN locked+ready latch (reset by _enter's memset), used by
 * the session to gate the CHARSELECT -> TRACKSELECT hand-off so a live B-back
 * cannot bounce on the lagging snapshot ready flag. */
u8 mdkr_online_charselect_local_ready(void) {
    return (u8) ((sCs.confirmed && sCs.ready) ? 1 : 0);
}

#endif /* MDKR_ENABLE_ONLINE_BETA */
