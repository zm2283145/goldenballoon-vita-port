/* SEPARATED-BOOT-PATH (Strategy D2) native online CHARACTER select.
 *
 * (Vehicle choice is now its OWN dedicated native screen, online_vehicleselect.c,
 * fronted right after this one; this screen only SEEDS a legality-safe default
 * vehicle so the seat can READY -- see mdkr_online_charselect_enter.)
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
#include "sound_ids.h"  /* SOUND_MENU_PICK3 / SOUND_HORN_DRUMSTICK / voices ... */
#include "sequence_ids.h" /* SEQUENCE_CHOOSE_YOUR_RACER (retail PLAYER SELECT track) */
#include "joypad.h"     /* input_pressed, input_clamp_stick_x/y */
#include "PR/os_cont.h" /* A_BUTTON / B_BUTTON / *_JPAD / START_BUTTON */
#include "net/party_link.h"
#include "online/online_trackselect.h" /* defer self-start to TRACKSELECT */
#include "online/online_vehicleselect.h" /* defer self-start to VEHICLESELECT */
#include "online/online_portraits.h" /* the shared portrait/name/asset
                                        tables (DRY with results/ceremony) */
#include "online/online_screen_constants.h" /* shared screen size + lobby id-space
                                               mirrors (DRY across screens) */
#include "online/online_screen_util.h" /* shared local_seat / text / pulse /
                                          draw_portrait helpers (DRY across screens) */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* gCurrDisplayList (the engine's live 2D frame list) and gRacerPortraits[] (the
 * decoded racer faces) are declared in online_screen_util.h, shared with the other
 * native screens. Both are drawn into the same frame-tail display list
 * menu_missing_controller() uses. The screen size + launcher lobby id-space mirrors
 * this screen shares with the others live in online_screen_constants.h. */

/* ---- Grid geometry (5x2, screen space; SCREEN_WIDTH 320 x SCREEN_HEIGHT 240) */
#define CS_COLS 5
#define CS_ROWS 2
#define CS_CELL_W 60
#define CS_CELL_H 70
#define CS_GRID_X 22 /* top-left x of column 0's portrait */
#define CS_GRID_Y 46 /* top-left y of row 0's portrait */
#define CS_PORTRAIT_HALF 22 /* ~half a portrait, for centering labels */
/* All seats ready + no host-start yet: after this many ticks the online-only
 * "waiting for host" footer appears under the retail "OK?" (host-start latency). */
#define CS_OK_WAIT_TICKS 150u
/* Portrait luminance for a racer the rival has LOCKED (confirmed). ~0.28 of full:
 * an unmistakable greyed/"unavailable" drop. Prim-colour modulation cannot
 * desaturate a decoded portrait, so the hard luminance drop + the remote's seat
 * block on that tile are the retail "claimed" cues (retail shows no TAKEN/RIVAL
 * word pair -- the block + the blocked cursor communicate it). The render applies
 * this drop; the witness reports the luminance it ACTUALLY handed the blit (see
 * sTakenTileDrawLum), NOT this constant, so the headless taken-tile coverage
 * observes the drawn cue instead of re-deriving it from the same condition. */
#define CS_TAKEN_DIM 72u

/* Retail PLAYER SELECT SFX set (the real DKR enums; verified against menu.c's own
 * charselect loop: move menu.c:9253, blocked menu.c:9230, confirm/deselect voice
 * menu.c:9241/9145). Confirm/B-unconfirm play the racer's per-character VOICE line
 * (SOUND_VOICE_CHARACTER_SELECT / _DESELECTED + the Character-enum id), so the
 * confirm cue is "My name's Krunch" etc., exactly like retail. */
#define CS_SFX_MOVE SOUND_MENU_PICK3     /* legal cursor move */
#define CS_SFX_BLOCKED SOUND_HORN_DRUMSTICK /* confirm on a claimed racer */
#define CS_SFX_READY SOUND_SELECT2       /* the join/ready beat */
#define CS_SFX_BACK SOUND_MENU_BACK3     /* step back a level (unready) */
/* The per-character voice-line base ids. The offset added is the racer's
 * Character-enum id (== sOnlineToPortrait[onlineId] -- one mapping serves both the
 * portrait blit and the voice), matching CHARSELECT_DATA(idx).voiceID in menu.c. */
#define CS_SFX_VOICE_SELECT SOUND_VOICE_CHARACTER_SELECT   /* confirm ("I'm X") */
#define CS_SFX_VOICE_DESELECT SOUND_VOICE_CHARACTER_DESELECTED /* B-unconfirm */

/* ---- Retail grid ORDER (menu.c adjacency table 1046-1067) ------------------
 * The PLAYER SELECT roster reads in a fixed VISUAL order that differs from the
 * online id space. This is a pure cell<->online-id relabel: sCs.cursor stays the
 * ONLINE id (the published hover_character the reducer validates); only the grid
 * POSITION each id occupies moves, so navigation and drawing convert between the
 * two spaces via these inverse tables. Published / reducer ids are unchanged.
 *   row0: Krunch, Diddy, Drumstick, Bumper, Banjo
 *   row1: Conker, Tiptup, T.T.,   Pipsy,  Timber                             */
static const u8 kCsCellToOnline[MDKR_ONLINE_SCREEN_CHAR_COUNT] = {
    7u, 0u, 8u, 5u, 6u, /* row0: Krunch Diddy Drumstick Bumper Banjo */
    4u, 3u, 9u, 2u, 1u, /* row1: Conker Tiptup T.T. Pipsy Timber */
};
static const u8 kCsOnlineToCell[MDKR_ONLINE_SCREEN_CHAR_COUNT] = {
    1u, 9u, 8u, 6u, 5u, /* Diddy Timber Pipsy Tiptup Conker */
    3u, 4u, 0u, 2u, 7u, /* Bumper Banjo Krunch Drumstick T.T. */
};

/* ---- Online character id -> gRacerPortraits index -------------------------
 * TWO different orderings exist for the ten racers and mixing them silently
 * draws the wrong face, so the mapping is explicit and named:
 *
 *   online id space   (party_link seats[].character_id; ui_online_room kCharacters):
 *     0 Diddy 1 Timber 2 Pipsy 3 Tiptup 4 Conker 5 Bumper 6 Banjo 7 Krunch
 *     8 Drumstick 9 T.T.
 *   gRacerPortraits[] index == the engine Character enum (enums.h) -- NOT the
 *   order the symbols appear in menu.c's array initializer:
 *     0 Krunch 1 Bumper 2 Tiptup 3 Conker 4 Timber 5 Banjo 6 Drumstick 7 Pipsy
 *     8 T.T. 9 Diddy
 *
 * The screen is laid out in ONLINE id order (grid cell index == online char id
 * == what we publish as hover_character and what the reducer validates), so the
 * sOnlineToPortrait[] table maps that id to the racer's Character-enum slot to
 * blit. The headless lane emits the resolved slot (witness `portrait=`) and
 * asserts the mapping, so a swapped entry is caught.
 *
 * that table + sOnlineNames[] + sPortraitAssetIds[] are the DRY lift
 * into the shared online_portraits.h (byte-identical across charselect / results /
 * ceremony -- the single truth). */

/* ---- Session-owned screen state (never an offline global) ------------------ */
typedef struct MdkrOnlineCharselectState {
    u8 cursor;         /* online char id under the cursor (grid cell) */
    u8 confirmed;      /* local player has locked a character */
    u8 ready;          /* local player is ready (the single ready-truth: P3) */
    u8 vehicle;        /* mask-legal default vehicle id published with the pick */
    u8 assets;         /* portrait group + fonts are loaded */
    u8 leave;          /* B-in-browse: leave request (edge; see tick) */
    u8 seeded;         /* first-snapshot cursor seed applied */
    u32 ticks;         /* CHARSELECT ticks elapsed (also drives the test input) */
    u32 blink;         /* retail (t + updateRate) & 0x3F selected-pulse timer */
    u32 bothReadyTicks; /* consecutive ticks all seats have been ready (OK? state) */
    s8 stickLatchX;
    s8 stickLatchY;
} MdkrOnlineCharselectState;

static MdkrOnlineCharselectState sCs;

/* persists ACROSS entries (deliberately NOT reset by _enter's memset) so a
 * tournament group's next race restarts on the racer you last confirmed, not
 * always Diddy. */
static u8 sLastConfirmedChar;

/* witness change-detect state at file scope so _enter() can reset it for a
 * clean second entry. */
static u32 sWitnessKey = 0xFFFFFFFFu;
static s32 sWitnessRemoteSeat = -2;

/* The luminance the render ACTUALLY handed the rival's locked (taken) tile's
 * portrait blit on the frame just drawn -- captured at the draw site, then reported
 * as the witness 'dim' so the taken-dim coverage observes what reached the screen
 * rather than re-deriving CS_TAKEN_DIM from the same taken condition. -1 = no taken
 * tile was drawn this frame (or the witness seam is unarmed -> witness prints 255). */
static s32 sTakenTileDrawLum = -1;

/* Resolved (display-only) view of the remote seat, with a bounded, NUL-forced
 * name copy -- seat->name is untrusted (remote-controlled). */
typedef struct CsRemoteView {
    s8 seat;      /* remote seat index, -1 when none */
    u8 present;   /* an occupied remote seat exists */
    u8 character; /* MDKR_ONLINE_SCREEN_NO_CHARACTER when no pick yet */
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
static s8 sRemoteVacateInput = -1; /* -1 unresolved, 0 off, 1 on */
static u8 charselect_remote_vacate_active(void) {
    if (sRemoteVacateInput < 0) {
        sRemoteVacateInput =
            (getenv("MDKR_TEST_ONLINE_REMOTE_VACATE") != NULL) ? 1 : 0;
    }
    return (u8) (sRemoteVacateInput > 0 ? 1 : 0);
}

/* Test-only: which row-0 racer the scripted cursor picks. Two paired endpoints
 * MUST pick DIFFERENT racers or the second SET_CHARACTER is rejected forever with
 * SELECTION_CONFLICT (both claim the same seat). The default (env unset) keeps the
 * historical Pipsy(2) sequence byte-for-byte, so every existing lane is unchanged;
 * the two-process cloud capstone sets it per role (host=2, joiner=0). Input only:
 * it changes which cell the pad navigates to, never a phase/route. Column 0..4. */
static s16 sCsScriptedPick = -2; /* -2 unresolved, -1 default(Pipsy 2), >=0 col */
static s16 charselect_scripted_pick(void) {
    if (sCsScriptedPick == -2) {
        const char *e = getenv("MDKR_TEST_ONLINE_CHARSELECT_PICK");
        sCsScriptedPick = -1;
        if (e != NULL && e[0] != '\0') {
            long v = strtol(e, NULL, 10);
            if (v >= 0 && v < CS_COLS) {
                sCsScriptedPick = (s16) v;
            }
        }
    }
    return sCsScriptedPick;
}

static void charselect_input_scripted(CsInput *in) {
    memset(in, 0, sizeof(*in));
    /* proof: keep the local seat BROWSING (never confirm/ready) so
     * the room stays in LOBBY while the session's remote-vacated detector debounces
     * and returns LEFT. Without this the scripted confirm+ready would hand off and
     * eventually host-start, leaving LOBBY before the debounce elapses. */
    if (charselect_remote_vacate_active()) {
        return;
    }
    {
        const s16 pick = charselect_scripted_pick();
        if (pick >= 0) {
            /* Navigate to row-0 CELL `pick` (a screen column, layout-independent):
             * step toward it in cell space one edge per tick, then confirm + ready.
             * Two paired endpoints pass different `pick` columns so they claim
             * different racers (host=2, joiner=0) -- no SELECTION_CONFLICT. */
            if (sCs.ticks >= 2u) {
                s32 cell = (s32) kCsOnlineToCell[sCs.cursor];
                s32 col = cell % CS_COLS;
                s32 row = cell / CS_COLS;
                if (!sCs.confirmed) {
                    if (row != 0) {
                        in->dy = -1;
                    } else if (col != (s32) pick) {
                        in->dx = ((s32) pick > col) ? 1 : -1;
                    } else {
                        in->aEdge = 1u; /* arrived -> confirm */
                    }
                } else if (!sCs.ready) {
                    in->aEdge = 1u; /* ready */
                }
            }
            return;
        }
    }
    /* Default script: walk to Pipsy (online id 2 -- cell 8 in the retail layout),
     * pressing a browse-B at tick 3 (the I1 no-wedge coverage), then confirm +
     * ready. Cell path from Diddy (cell 1): dx,dx to Bumper (cell 3, hover only),
     * dy to Pipsy (cell 8). The grid-layout witness proves EVERY cell's name/face
     * pair, so this walk need only exercise the round-trip. */
    switch (sCs.ticks) {
    case 2u:
        in->dx = 1; /* Diddy(cell1) -> Drumstick(cell2) */
        break;
    case 3u:
        in->bEdge = 1u; /* B while browsing: must NOT wedge the session */
        break;
    case 4u:
        in->dx = 1; /* -> Bumper(cell3), hover only (claimed by the remote) */
        break;
    case 5u:
        in->dy = 1; /* -> Pipsy(cell8) */
        break;
    case 7u:
        in->aEdge = 1u; /* confirm Pipsy */
        break;
    case 13u:
        in->aEdge = 1u; /* ready */
        break;
    default:
        break;
    }
}

/* Live pad: D-pad edges plus a latched analog stick, local player only. */
static void charselect_input_live(CsInput *in) {
    u32 pressed = input_pressed(MDKR_ONLINE_SCREEN_LOCAL_PAD);
    s32 sx = input_clamp_stick_x(MDKR_ONLINE_SCREEN_LOCAL_PAD);
    s32 sy = input_clamp_stick_y(MDKR_ONLINE_SCREEN_LOCAL_PAD);
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

/* the headless LOBBY-START lane (env MDKR_TEST_ONLINE_LOBBY_START)
 * drives the native screens with scripted INPUT while the REAL launcher owns the
 * party_link forward feed (unlike MDKR_TEST_ONLINE_CHARSELECT, which ALSO installs
 * a self-contained scripted feed + minimal reducer). So this enables the scripted
 * cursor/confirm/ready ONLY -- it never touches the feed seams below, whose gating
 * (MDKR_TEST_ONLINE_CHARSELECT) is unchanged, so the two-endpoint loopback's real
 * reverse feed reaches the real adapter. Inert (unresolved -> off) in every run
 * that does not set the env, so the existing charselect/trackselect lanes are
 * behaviour-unchanged. Resolved once. */
static s8 sLobbyStartInput = -1; /* -1 unresolved, 0 off, 1 on */
static u8 charselect_lobby_input_active(void) {
    if (sLobbyStartInput < 0) {
        sLobbyStartInput = (getenv("MDKR_TEST_ONLINE_LOBBY_START") != NULL) ? 1 : 0;
    }
    return (u8) (sLobbyStartInput > 0 ? 1 : 0);
}

static void charselect_gather_input(CsInput *in) {
    if (mdkr_online_charselect_test_active() || charselect_lobby_input_active()) {
        charselect_input_scripted(in);
    } else {
        charselect_input_live(in);
    }
}

/* Apply one input step to the local cursor/confirm/ready latches. `remoteChar`
 * is the remote seat's taken racer (MDKR_ONLINE_SCREEN_NO_CHARACTER when none) so a confirm on a
 * taken cell can be REJECTED (ruling: DISALLOW -- matches offline DKR and avoids
 * the SELECTION_CONFLICT divergence). */
static void charselect_apply_input(const CsInput *in, u8 remoteChar) {
    if (!sCs.confirmed) {
        /* Browsing: move the cursor over the grid in CELL space (visual
         * adjacency), then map the landed cell back to the online id sCs.cursor
         * carries. Columns wrap (native DKR 2D menus wrap); rows clamp. */
        s32 cell = (s32) kCsOnlineToCell[sCs.cursor];
        s32 col = cell % CS_COLS;
        s32 row = cell / CS_COLS;
        u8 previous = sCs.cursor;
        col = (col + in->dx + CS_COLS) % CS_COLS;
        row += in->dy;
        if (row < 0) {
            row = 0;
        }
        if (row >= CS_ROWS) {
            row = CS_ROWS - 1;
        }
        sCs.cursor = kCsCellToOnline[row * CS_COLS + col];
        if (sCs.cursor != previous) {
            sound_play(CS_SFX_MOVE, NULL);
        }
        if (in->aEdge) {
            if (remoteChar != MDKR_ONLINE_SCREEN_NO_CHARACTER && sCs.cursor == remoteChar) {
                /* DISALLOW: do not publish a confirm for a claimed racer -- retail
                 * blocks it with the drumstick horn (menu.c:9230). */
                sound_play(CS_SFX_BLOCKED, NULL);
            } else {
                sCs.confirmed = 1u;
                sLastConfirmedChar = sCs.cursor; /* persistence */
                /* confirm cue = the racer's own voice line ("I'm X"), voiceID ==
                 * the Character-enum id (sOnlineToPortrait), same as menu.c:9241. */
                sound_play(
                    (s32) CS_SFX_VOICE_SELECT + (s32) sOnlineToPortrait[sCs.cursor],
                    NULL);
            }
        } else if (in->bEdge) {
            /* Browse-B = leave-to-room. WIRED: on the descriptor-less
             * (lobby-start) human path online_session honors this as a LEFT return
             * to the Online Room; on the descriptor-first / scripted lanes it is
             * the warn-once STAY stub. Edge-handled in the tick (cleared after one
             * return) so it can never wedge the session. The browse footer
             * advertises "B: LEAVE" so the destructive input is
             * visible on the first screen of the flow. */
            sCs.leave = 1u;
        }
        return;
    }
    /* Confirmed: A readies, B steps back one level (unready, then unconfirm). */
    if (in->aEdge) {
        sCs.ready = 1u;
        sound_play(CS_SFX_READY, NULL); /* the ready beat (retail SELECT2) */
    } else if (in->bEdge) {
        if (sCs.ready) {
            sCs.ready = 0u;
            sound_play(CS_SFX_BACK, NULL);
        } else {
            sCs.confirmed = 0u;
            /* B-unconfirm cue = the racer's DESELECT voice (menu.c:9145). */
            sound_play(
                (s32) CS_SFX_VOICE_DESELECT + (s32) sOnlineToPortrait[sCs.cursor],
                NULL);
        }
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
 * start_requested stays 0 -- host-start belongs to the track screen. */
static void charselect_publish_intent(void) {
    MdkrPartyLinkLocalIntent intent;
    /* start from the shared baseline so the host-only session-config fields
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
/* Grid CELL top-left for an online id (the retail-order relabel lives here). */
static void charselect_cell_xy(u8 onlineId, s32 *x, s32 *y) {
    s32 cell = (s32) kCsOnlineToCell[onlineId];
    *x = CS_GRID_X + (cell % CS_COLS) * CS_CELL_W;
    *y = CS_GRID_Y + (cell / CS_COLS) * CS_CELL_H;
}

static void charselect_draw_portrait(u8 onlineId, u8 r, u8 g, u8 b) {
    /* Grid-relative wrapper: derive the cell coords, then the shared guarded blit. */
    s32 x, y;
    charselect_cell_xy(onlineId, &x, &y);
    mdkr_online_screen_draw_portrait(onlineId, x, y, r, g, b);
}

/* PLAYER SELECT / OK? title face: BIGFONT with the retail 1px translucent-black
 * drop shadow. The shared text helper deliberately skips shadows for the authored
 * BIGFONT art, so the retail shadow recipe (menu.c draws titles black at +offset,
 * opacity 180, then the gold art) is drawn here. */
static void charselect_bigfont_shadowed(s32 x, s32 y, char *text) {
    set_text_font(ASSET_FONTS_BIGFONT);
    set_text_background_colour(0, 0, 0, 0);
    set_text_colour(0, 0, 0, 255, 180);
    draw_text(&gCurrDisplayList, x + 1, y + 3, text, ALIGN_MIDDLE_CENTER);
    set_text_colour(255, 255, 255, 0, 255); /* authored gold art, untinted */
    draw_text(&gCurrDisplayList, x, y, text, ALIGN_MIDDLE_CENTER);
}

/* Retail P1/P2 seat-number block, mimicked in 2D: a small gold card above the
 * tile with the seat digit (FUNFONT -- BIGFONT has no digit glyphs). `xoff` shifts
 * the card horizontally off the tile centre so two markers on the SAME tile (local
 * cursor hovering the remote's claimed racer) can sit side-by-side instead of
 * stacking; 0 keeps the single-marker case centred as before. */
static void charselect_draw_seat_marker(u8 onlineId, s32 number, s32 xoff) {
    s32 x, y;
    char digit[2];
    s32 cx;
    charselect_cell_xy(onlineId, &x, &y);
    cx = x + CS_PORTRAIT_HALF + xoff;
    mdkr_online_screen_card(cx - 9, y - 15, cx + 9, y - 1, 255, 200, 40, 235);
    digit[0] = (char) ('0' + ((number > 0 && number < 10) ? number : 0));
    digit[1] = '\0';
    mdkr_online_screen_text(cx, y - 8, ASSET_FONTS_FUNFONT, digit,
                            ALIGN_MIDDLE_CENTER, 255, 255, 255);
}

/* One portrait name label. EVERY name draws in the retail body face (FUNFONT) with
 * the shared helper's pink authored art + 1px drop shadow -- no name ever drops to
 * the plain white SMALLFONT. The widest name (DRUMSTICK) overruns its 60px cell in
 * the authored FUNFONT advance and crowds its neighbour, and the retail text path
 * has no per-glyph horizontal scale (draw_text is fixed 1.0). So EVERY name is drawn
 * with the letter-spacing squeeze uniformly (set_kerning(TRUE) -- one pixel tighter
 * per glyph, the exact kern menu.c uses for the hub names and the one that fixed the
 * "DRA GON"/"BE GIN" airiness on the other online screens): consistency over size.
 * The squeeze keeps every label in the identical pink FUNFONT treatment AND opens
 * the inter-name gaps enough that DRUMSTICK clears BUMPER. Both get_text_width and
 * draw_text honour gCompactKerning, so the centred alignment and the shadow/face
 * passes stay registered under the squeeze. FUNFONT is an authored-art face, so
 * mdkr_online_screen_text never touches gCompactKerning itself; we set it for the
 * label draw and restore the module default (FALSE) afterwards. */
static void charselect_draw_name(u8 onlineId) {
    s32 x, y;
    char *name = (char *) sOnlineNames[onlineId];
    charselect_cell_xy(onlineId, &x, &y);
    set_kerning(TRUE);
    mdkr_online_screen_text(x + CS_PORTRAIT_HALF, y + 46, (s32) ASSET_FONTS_FUNFONT,
                            name, ALIGN_MIDDLE_CENTER, 255, 255, 255);
    set_kerning(FALSE);
}

/* Resolve the (first occupied, non-local) remote seat into a bounded view.
 * NOTE fenced 2-endpoint beta: first remote seat only. Revisit for
 * true 4-player rendering. */
static void charselect_resolve_remote(const MdkrPartyLinkSnapshot *snap,
                                      bool haveSnap, s32 localSeat,
                                      CsRemoteView *out) {
    unsigned i;
    memset(out, 0, sizeof(*out));
    out->seat = -1;
    out->character = MDKR_ONLINE_SCREEN_NO_CHARACTER;
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
        if (seat->character_id < MDKR_ONLINE_SCREEN_CHAR_COUNT) {
            out->character = seat->character_id;
        }
        out->ready = seat->ready ? 1u : 0u;
        /* Untrusted: bounded copy, never assume NUL termination. */
        memcpy(out->name, seat->name, MDKR_PARTY_LINK_NAME_BYTES);
        out->name[MDKR_PARTY_LINK_NAME_BYTES] = '\0';
        break;
    }
}

static void charselect_render(const CsRemoteView *rv, s32 localSeat) {
    const char *rname = rv->name[0] != '\0' ? rv->name : "RIVAL";
    /* Retail selected-item pulse: (t + updateRate) & 0x3F accumulated in sCs.blink,
     * *8 triangle-folded to 0..255 (menu.c gOptionBlinkTimer cadence -- slower and
     * deeper than the old 0..16 pulse). */
    s32 blink = mdkr_online_screen_blink(sCs.blink);
    /* Retail seat blocks are numbered by SEAT INDEX: host (seat 0) = "1", joiner
     * (seat 1) = "2", whichever is local. */
    s32 localNum = (localSeat >= 0) ? localSeat + 1 : 1;
    s32 remoteNum = (rv->seat >= 0) ? (s32) rv->seat + 1 : 2;
    bool allReady = (sCs.confirmed && sCs.ready && rv->present && rv->ready);
    /* Capture the taken tile's ACTUAL drawn luminance for the witness only when the
     * headless test seam is armed -- reset every frame so no stale value can leak. */
    bool witnessArmed = mdkr_online_charselect_test_active() ? true : false;
    u8 id;

    sTakenTileDrawLum = -1;

    /* Retail grounds (§4.6 "plain scene for charselect"): PLAYER SELECT has no
     * menu-board chrome. The title and the real portraits float over the hub sky
     * (portraits are opaque; the title + names carry the retail drop shadow). Only
     * the online-only status/help footer keeps a subtle band for legibility -- there
     * is no such text on the retail screen. */
    mdkr_online_screen_strip(190, 216);

    /* Title: PLAYER SELECT (a literal -- gMenuText is offline-loaded), BIGFONT with
     * the retail 1px translucent-black drop shadow. */
    charselect_bigfont_shadowed(MDKR_ONLINE_SCREEN_W_HALF, 16, "PLAYER SELECT");

    /* The grid: every racer's real portrait + FUNFONT name, in the retail visual
     * order. The hover cursor is the retail-cadence gold pulse + the local seat
     * block; the local confirmed pick reads green; the remote's claimed pick is the
     * retail luminance-dropped tile (the block + the drop are the "claimed" cue --
     * no TAKEN/RIVAL word pair). */
    for (id = 0u; id < MDKR_ONLINE_SCREEN_CHAR_COUNT; id++) {
        u8 pr = 210u, pg = 210u, pb = 210u;
        bool taken = (rv->character != MDKR_ONLINE_SCREEN_NO_CHARACTER && id == rv->character);
        bool onCursor = (id == sCs.cursor);
        bool localPick = (sCs.confirmed && id == sCs.cursor);

        if (taken) {
            pr = pg = pb = CS_TAKEN_DIM;
        }
        if (onCursor && !localPick) {
            if (taken) {
                /* Dimmed gold: keep BOTH "this is my cursor" and "claimed" legible. */
                pr = 200u;
                pg = 170u;
                pb = 80u;
            } else {
                pr = 255u;
                pg = (u8) (150 + (105 * blink) / 255);
                pb = (u8) (40 + (80 * blink) / 255);
            }
        }
        if (localPick) {
            pr = 120u;
            pg = 255u;
            pb = 120u;
        }
        if (taken && witnessArmed) {
            /* The luminance actually handed to THIS frame's taken-tile blit
             * (r==g==b for the greyed state) -- reported as the witness 'dim'. */
            sTakenTileDrawLum = (s32) pr;
        }
        charselect_draw_portrait(id, pr, pg, pb);
        charselect_draw_name(id);
    }

    /* Retail P1/P2 seat blocks: the local seat's number on the hovered tile (the
     * cursor IS the block, retail-style), the remote seat's number on its claimed
     * tile. Drawn AFTER the grid so they sit on top. When the local cursor is parked
     * on the SAME tile the remote has claimed (moving onto a claimed tile is legal --
     * only confirm is blocked), both cards would otherwise draw at identical coords:
     * the remote card (drawn last) would occlude the local one and the two
     * translucent cards would double-blend. So on coincidence, split the pair
     * side-by-side above the tile (local "1" shifted left, remote "2" shifted right)
     * so BOTH read; the single-marker cases stay centred (xoff 0). */
    {
        bool coincide = (rv->character != MDKR_ONLINE_SCREEN_NO_CHARACTER &&
                         sCs.cursor == rv->character);
        charselect_draw_seat_marker(sCs.cursor, localNum, coincide ? -11 : 0);
        if (rv->character != MDKR_ONLINE_SCREEN_NO_CHARACTER) {
            charselect_draw_seat_marker(rv->character, remoteNum, coincide ? 11 : 0);
        }
    }

    /* Online-only status footer (no retail counterpart): each seat's ready state,
     * driven by the ready LATCH (single ready-truth). */
    {
        char line[64];
        const char *you = sCs.ready ? "READY" : (sCs.confirmed ? "PICKED"
                                                              : "CHOOSING");
        (void) snprintf(line, sizeof(line), "YOU: %s", you);
        mdkr_online_screen_text(24, 198, ASSET_FONTS_SMALLFONT, line,
                                ALIGN_MIDDLE_LEFT, sCs.ready ? 120 : 220,
                                sCs.ready ? 255 : 220, sCs.ready ? 120 : 220);
        if (!rv->present) {
            mdkr_online_screen_text(MDKR_ONLINE_SCREEN_W - 24, 198, ASSET_FONTS_SMALLFONT,
                                    "WAITING FOR PLAYER...", ALIGN_MIDDLE_RIGHT,
                                    150, 150, 150);
        } else {
            (void) snprintf(line, sizeof(line), "%.12s: %s", rname,
                            rv->ready ? "READY" : "CHOOSING");
            mdkr_online_screen_text(MDKR_ONLINE_SCREEN_W - 24, 198, ASSET_FONTS_SMALLFONT,
                                    line, ALIGN_MIDDLE_RIGHT,
                                    rv->ready ? 120 : 220, rv->ready ? 255 : 220,
                                    rv->ready ? 120 : 220);
        }
    }

    /* Centre cue: the retail "OK?" (BIGFONT, low) when ALL seats are ready, else the
     * context help. When all are ready the host-start is imminent; only after a
     * stretch of host-start latency is the online-only "waiting for host" footer
     * added under it (an online necessity retail has no analogue for). */
    if (allReady) {
        sCs.bothReadyTicks++;
        charselect_bigfont_shadowed(MDKR_ONLINE_SCREEN_W_HALF, 208, "OK?");
        if (sCs.bothReadyTicks > CS_OK_WAIT_TICKS) {
            /* This online-only line sits BELOW the footer band (which ends at
             * y=216), so extend the dark band down to cover it -- every footer
             * line must read on a band, never float over the sky. The extension
             * abuts the main strip (drawn contiguously from y=216) and reaches
             * y=238, as the pre-W1 panel did. */
            mdkr_online_screen_strip(216, 238);
            mdkr_online_screen_text(MDKR_ONLINE_SCREEN_W_HALF, 228,
                                    ASSET_FONTS_SMALLFONT,
                                    "WAITING FOR HOST TO START...   B: UNREADY",
                                    ALIGN_MIDDLE_CENTER, 200, 200, 200);
        }
    } else {
        sCs.bothReadyTicks = 0u;
        if (!sCs.confirmed) {
            mdkr_online_screen_text(MDKR_ONLINE_SCREEN_W_HALF, 208, ASSET_FONTS_SMALLFONT,
                                    "A: SELECT   B: LEAVE", ALIGN_MIDDLE_CENTER,
                                    255, 255, 255);
        } else if (!sCs.ready) {
            mdkr_online_screen_text(MDKR_ONLINE_SCREEN_W_HALF, 208, ASSET_FONTS_SMALLFONT,
                                    "A: READY   B: CHANGE PICK", ALIGN_MIDDLE_CENTER,
                                    255, 255, 255);
        } else {
            char msg[64];
            (void) snprintf(msg, sizeof(msg),
                            "READY! WAITING FOR %.12s...   B: UNREADY", rname);
            mdkr_online_screen_text(MDKR_ONLINE_SCREEN_W_HALF, 208, ASSET_FONTS_SMALLFONT,
                                    msg, ALIGN_MIDDLE_CENTER, 120, 255, 120);
        }
    }
}

/* Bounded stderr witness: one line only when the visible state changes, so a
 * headless lane can read the drawn picks/names + published intent without
 * flooding. The change-detect key folds the remote pick/ready too, so a
 * remote-only change still emits a row. */
static void charselect_witness(const MdkrPartyLinkSnapshot *snap, bool haveSnap,
                               s32 localSeat, const CsRemoteView *rv) {
    u8 localSeatChar = MDKR_ONLINE_SCREEN_NO_CHARACTER;
    u8 localSeatReady = 0u;
    u8 remoteNibble;
    u32 key;

    if (haveSnap && localSeat >= 0) {
        localSeatChar = snap->seats[localSeat].character_id;
        localSeatReady = snap->seats[localSeat].ready;
    }

    remoteNibble = (rv->character < MDKR_ONLINE_SCREEN_CHAR_COUNT) ? rv->character : 0xFu;
    key = ((u32) sCs.cursor) | ((u32) sCs.confirmed << 8) |
          ((u32) sCs.ready << 9) | ((u32) remoteNibble << 10) |
          ((u32) (rv->ready ? 1u : 0u) << 14) | ((u32) rv->present << 15) |
          ((u32) localSeatChar << 16) | ((u32) localSeatReady << 24);
    if (key == sWitnessKey && rv->seat == sWitnessRemoteSeat) {
        return;
    }
    sWitnessKey = key;
    sWitnessRemoteSeat = rv->seat;

    /* Coverage for the greyed/"TAKEN" tile cue: when the rival has LOCKED
     * (confirmed) a racer its tile is drawn at a hard luminance drop + a TAKEN/
     * RIVAL nameplate. Report WHICH tile is greyed (from the snapshot pick) and the
     * luminance the render ACTUALLY drew it at (sTakenTileDrawLum, captured at the
     * draw site above) so a headless lane can ASSERT the cue genuinely shows -- and
     * so DROPPING the render's dim line reports a normal-bright tile here rather than
     * a constant re-derived from the same taken condition. taken=-1 / dim=255 when
     * the rival holds no lock (or the witness seam is unarmed). */
    {
        s32 takenTile = (rv->character < MDKR_ONLINE_SCREEN_CHAR_COUNT) ? (s32) rv->character : -1;
        unsigned takenDim = (sTakenTileDrawLum >= 0)
                                ? (unsigned) sTakenTileDrawLum
                                : 255u;
        fprintf(stderr,
                "[online-charselect] render cursor=%u name=%s portrait=%u "
                "local{conf=%u ready=%u seatChar=%u seatReady=%u} "
                "remote{seat=%d char=%u ready=%u name=%.*s} taken=%d dim=%u "
                "intent{hover=%u vehicle=%u confirmed=%u ready=%u}\n",
                sCs.cursor, sOnlineNames[sCs.cursor],
                (unsigned) sOnlineToPortrait[sCs.cursor], sCs.confirmed, sCs.ready,
                (unsigned) localSeatChar, (unsigned) localSeatReady, (int) rv->seat,
                (unsigned) rv->character, (unsigned) rv->ready,
                (int) MDKR_PARTY_LINK_NAME_BYTES,
                rv->name[0] != '\0' ? rv->name : "-", takenTile, takenDim,
                sCs.cursor, (unsigned) sCs.vehicle, sCs.confirmed, sCs.ready);
    }
}

/* ======================================================================== *
 * Lifecycle
 * ======================================================================== */
void mdkr_online_charselect_enter(void) {
    s8 defaultVehicle;

    memset(&sCs, 0, sizeof(sCs));
    /* start on the last confirmed racer (or Diddy the first time). A valid
     * first snapshot may re-seed this (see tick). sLastConfirmedChar persists. */
    sCs.cursor = sLastConfirmedChar < MDKR_ONLINE_SCREEN_CHAR_COUNT ? sLastConfirmedChar : 0u;

    /* reset the witness change-detect + the headless seam so a second entry
     * is clean (a re-entered CHARSELECT re-scripts from scratch). */
    sWitnessKey = 0xFFFFFFFFu;
    sWitnessRemoteSeat = -2;
    charselect_test_reset();

    /* Reuse the EXACT default vehicle menu_online_versus_race_setup() applies at
     * boot (get_player_selected_vehicle(PLAYER_ONE)); clamp to a base vehicle so
     * it is always inside the 0x07 player mask and the seat can legally READY
     * here (the reducer refuses READY without a vehicle). This is only the SEED /
     * legality-safe default now: the player makes the REAL vehicle choice on the
     * dedicated native VEHICLE select screen (online_vehicleselect.c), which the
     * session fronts right after this one (CHARSELECT -> VEHICLESELECT ->
     * TRACKSELECT); TRACKSELECT's auto-narrow remains the final legality clamp. */
    defaultVehicle = get_player_selected_vehicle(MDKR_ONLINE_SCREEN_LOCAL_PAD);
    if (defaultVehicle < 0 || (u8) defaultVehicle >= MDKR_ONLINE_SCREEN_VEHICLE_COUNT) {
        defaultVehicle = (s8) VEHICLE_CAR;
    }
    sCs.vehicle = (u8) defaultVehicle;

    /* Borrow the real portraits: load the portrait-only texture group and bind
     * them into gRacerPortraits[] -- the same two calls the offline results
     * screen makes. */
    menu_assetgroup_load(sPortraitAssetIds);
    menu_racer_portraits();

    /* Borrow the real per-world sky tiles (read-only, the same asset-group borrow
     * as the portraits) so the backdrop is the retail scrolling sky, not a flat
     * fill. Balanced by the menu_assetgroup_free() in _exit(). */
    menu_assetgroup_load(sOnlineSkyAssetIds);

    /* load_fonts() at boot only builds the font TABLE; each screen must load the
     * glyph textures for the fonts it draws with (refcounted; unload_font() on
     * exit). BIGFONT = title / OK?; FUNFONT = the retail body face (ALL portrait
     * names + seat digits); SMALLFONT = online-only footers only. */
    load_font(ASSET_FONTS_BIGFONT);
    load_font(ASSET_FONTS_FUNFONT);
    load_font(ASSET_FONTS_SMALLFONT);

    sCs.assets = 1u;
    /* Neutral hub sky (Dino Domain) -- charselect is world-agnostic. */
    mdkr_online_screen_backdrop((u8) MDKR_ONLINE_SKY_WORLD_NEUTRAL);

    /* reveal this screen from black (retail fade cadence) and start the retail
     * PLAYER SELECT music (SEQUENCE_CHOOSE_YOUR_RACER -- the offline charselect's
     * own track, not the front-end SEQUENCE_MAIN_MENU). Charselect is the FIRST
     * native screen after the launcher hand-off, so its reveal is also what makes
     * that hand-off read as one continuous motion. Isolation-safe primitive borrows
     * (transition_begin / music_play) -- see online_screen_util.h. */
    mdkr_online_screen_fade_in_from_black();
    mdkr_online_screen_music((u8) SEQUENCE_CHOOSE_YOUR_RACER);

    /* Grid-layout witness (headless lanes only): dump each CELL's online id + the
     * name/portrait slot it draws, so the retail cell order + every visible
     * name->face pair is asserted in one place (check_online_charselect.py) rather
     * than only for the cells the scripted cursor happens to visit. */
    if (mdkr_online_charselect_test_active()) {
        unsigned c;
        for (c = 0u; c < MDKR_ONLINE_SCREEN_CHAR_COUNT; c++) {
            u8 oid = kCsCellToOnline[c];
            fprintf(stderr,
                    "[online-charselect] grid cell=%u id=%u name=%s portrait=%u\n",
                    c, (unsigned) oid, sOnlineNames[oid],
                    (unsigned) sOnlineToPortrait[oid]);
        }
    }

    fprintf(stderr,
            "[online-charselect] enter: native screen up defaultVehicle=%u "
            "cursor=%u (portraits loaded, offline _loop bypassed)\n",
            (unsigned) sCs.vehicle, (unsigned) sCs.cursor);
}

void mdkr_online_charselect_exit(void) {
    if (sCs.assets) {
        /* Disarm the borrowed scrolling sky BEFORE freeing its tiles, so the
         * engine's per-frame bgdraw_render() can never DMA a freed sky. */
        mdkr_online_screen_backdrop_clear();
        /* Symmetric free: the same group loader's free path releases the ten
         * portrait textures (and, once the menu asset count returns to zero, the
         * shared portrait bookkeeping) before the race loader reuses the pool.
         * Balance the three load_font() refs taken in _enter(). */
        unload_font(ASSET_FONTS_SMALLFONT);
        unload_font(ASSET_FONTS_FUNFONT);
        unload_font(ASSET_FONTS_BIGFONT);
        menu_assetgroup_free(sPortraitAssetIds);
        menu_assetgroup_free(sOnlineSkyAssetIds);
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

    /* Retail blink timer: accumulate the logic update rate into a 0x3F-wrapping
     * counter, exactly like menu.c's gOptionBlinkTimer, so the selected-item pulse
     * runs at the authentic DKR cadence regardless of the host frame rate. */
    sCs.blink = (sCs.blink + (u32) (updateRate > 0 ? updateRate : 0)) & 0x3Fu;

    /* Read the authoritative forward feed the launcher publishes (both seats'
     * picks/ready + the remote name). Display-only for the remote seat. */
    haveSnap = mdkr_party_link_read(&snap);
    localSeat = haveSnap ? mdkr_online_screen_local_seat(&snap) : -1;
    charselect_resolve_remote(&snap, haveSnap, localSeat, &rv);

    /* seed the cursor from the first snapshot's local pick (e.g. a reconnect
     * lands on your existing racer), once, before input. */
    if (!sCs.seeded && haveSnap) {
        if (localSeat >= 0 &&
            snap.seats[localSeat].character_id < MDKR_ONLINE_SCREEN_CHAR_COUNT) {
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
    charselect_render(&rv, localSeat);
    charselect_witness(&snap, haveSnap, localSeat, &rv);

    /* Headless test seam: reflect the intent into the scripted room + script the
     * host-start. Inert (and installs nothing) in a normal run. */
    charselect_test_reduce_and_script();

    sCs.ticks++;

    /* the authoritative lobby leaving LOBBY (host started / loading) is the
     * signal to move on, and it WINS over a pending leave -- otherwise a stray B
     * would keep this endpoint from ever booting while the room raced on. */
    if (haveSnap && snap.phase != (uint8_t) MDKR_ONLINE_SCREEN_LOBBY_PHASE) {
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

#define CS_TEST_REMOTE_CHARACTER 5u /* Bumper, in online id space */

static void charselect_test_resolve(void) {
    if (sTestActive < 0) {
        sTestActive = (getenv("MDKR_TEST_ONLINE_CHARSELECT") != NULL) ? 1 : 0;
    }
}

static void charselect_test_init_room(void) {
    unsigned i;
    memset(&sTestRoom, 0, sizeof(sTestRoom));
    sTestRoom.phase = (uint8_t) MDKR_ONLINE_SCREEN_LOBBY_PHASE;
    for (i = 0u; i < MDKR_PARTY_LINK_SEATS; i++) {
        sTestRoom.seats[i].character_id = MDKR_ONLINE_SCREEN_NO_CHARACTER;
        sTestRoom.seats[i].vehicle_id = MDKR_ONLINE_SCREEN_NO_VEHICLE;
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

/* called from _enter so a re-entered CHARSELECT re-scripts from scratch. The
 * reducer gates on mdkr_party_link_active() (not sTestInstalled), so clearing the
 * install latch here does not disturb the current session -- it only forces a
 * fresh install on a future session's LOBBY_WAIT pump. */
static void charselect_test_reset(void) {
    charselect_test_resolve();
    if (!sTestActive) {
        return;
    }
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
        if (intent.confirmed && intent.hover_character < MDKR_ONLINE_SCREEN_CHAR_COUNT) {
            sTestRoom.seats[0].character_id = intent.hover_character;
        }
        if (intent.vehicle_id < MDKR_ONLINE_SCREEN_VEHICLE_COUNT) {
            sTestRoom.seats[0].vehicle_id = intent.vehicle_id;
        }
        if (intent.ready && sTestRoom.seats[0].character_id != MDKR_ONLINE_SCREEN_NO_CHARACTER &&
            sTestRoom.seats[0].vehicle_id != MDKR_ONLINE_SCREEN_NO_VEHICLE) {
            sTestRoom.seats[0].ready = 1u;
        } else if (intent.backout) {
            sTestRoom.seats[0].ready = 0u;
        }
    }

    /* Script the host-start: once both seats are ready, advance the lobby to
     * LOADING -- the authoritative signal the screen reacts to (the next tick reads
     * the LEFT-LOBBY snapshot, renders the converged both-ready state, and returns
     * ADVANCE). The advance is IMMEDIATE (same reduce tick both seats first read
     * ready): the native VEHICLESELECT screen is now always in the flow, so the
     * session would otherwise hand CHARSELECT -> VEHICLESELECT on the local-ready
     * signal before a held self-start could fire. Advancing at once leaves the
     * session no LOBBY-phase local-ready tick to divert on, so the standalone
     * CHARSELECT lane keeps its historical CHARSELECT -> race ADVANCE hand-off.
     *
     * when the TRACKSELECT / VEHICLESELECT headless seam is ALSO armed (the combined
     * screen lanes), do NOT self-start here -- leave the room in LOBBY so the
     * session hands the flow forward one native screen at a time and the downstream
     * seam drives the eventual host-start. */
    if (sTestRoom.seats[0].ready && sTestRoom.seats[1].ready &&
        !mdkr_online_trackselect_test_active() &&
        !mdkr_online_vehicleselect_test_active()) {
        sTestRoom.phase = (uint8_t) (MDKR_ONLINE_SCREEN_LOBBY_PHASE + 1u); /* LOADING */
    }

    mdkr_party_link_publish(&sTestRoom);
}

u8 mdkr_online_charselect_test_active(void) {
    charselect_test_resolve();
    return (u8) (sTestActive > 0 ? 1 : 0);
}

/* true when EITHER scripted-input seam is armed (the self-contained
 * CHARSELECT seam or the lobby-start scripted cursor). Mirrors
 * charselect_gather_input's own selector so the session can tell scripted input
 * (whose tick-3 browse-B is the I1 no-wedge coverage) from a live human's B. */
u8 mdkr_online_charselect_scripted_input_active(void) {
    return (u8) ((mdkr_online_charselect_test_active() ||
                  charselect_lobby_input_active()) ? 1 : 0);
}

/* the screen's OWN locked+ready latch (reset by _enter's memset), used by
 * the session to gate the CHARSELECT -> TRACKSELECT hand-off so a live B-back
 * cannot bounce on the lagging snapshot ready flag. */
u8 mdkr_online_charselect_local_ready(void) {
    return (u8) ((sCs.confirmed && sCs.ready) ? 1 : 0);
}

#endif /* MDKR_ENABLE_ONLINE_BETA */
