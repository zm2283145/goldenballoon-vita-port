/* SEPARATED-BOOT-PATH native online VEHICLE stage of the track
 * screen.
 *
 * ============================ THE REUSE BOUNDARY ========================
 * Retail 2P picks vehicles AFTER the track, as a STAGE of the track-select
 * screen (menu.c trackmenu_setup_render / func_80092188 case 0): the live
 * scene continues behind, the track name stays up top, and each player's
 * column -- a PLAYER n label, a framed vehicle icon and the CAR/HOVER/PLANE
 * word list -- confirms independently; all-confirmed plays SOUND_CAR_REV2 and
 * moves to the racer-count / OK beat. This TU is that stage for the native
 * online flow: the session enters it from TRACKSELECT once the host's pick is
 * locked (browse -> lock -> THIS -> OK -> race), and the composition mirrors
 * the retail one at the retail coordinates. The racer-count stage is skipped
 * (online v1 is a fixed 2 racers) and the OK beat is the host's A/START here.
 *
 * THE CRUX (why this is a small, plumbing-free screen): the party_link reverse
 * feed ALREADY carries vehicle_id and the reducer ALREADY validates
 * CHOOSE_VEHICLE + refuses START (BEGIN_LOADING) with ILLEGAL_VEHICLE when a
 * seat's vehicle is outside the resolved track's mask. So this stage only
 * drives intent.vehicle_id from the player's pick, maps the retail per-player
 * CONFIRM onto intent.ready, and republishes the host's locked config +
 * start_requested. No new transport, no new reducer command, no new sync
 * protocol -- ONLY the local sequencing/presentation moved (the vote kinds and
 * every published intent field are the ones the launcher already plans).
 *
 * WHAT IT BORROWS (read-only reuse of already-compiled game code/data; NO edit
 * to menu.c -- every symbol below already has external linkage):
 *   - gMenuAssets[] + menu_assetgroup_load/free (menu.c): the retail setup
 *     stage's own art, by RAW TEXTURE id (the ids retail binds in
 *     trackmenu_assets, menu.c:10810+): TEXTURE_ICON_PLAYER_1/2 label art,
 *     TEXTURE_ICON_VEHICLE_SELECT_{CAR,HOVERCRAFT,PLANE}[_HIGHLIGHT] word art,
 *     TEXTURE_ICON_VEHICLE_*_TOP/BOTTOM icon pairs, and the wood tile for the
 *     icon frames. NOTE: gMenuImages / menu_imagegroup_load / menu_element_render
 *     (retail's own frame path) are OFFLINE-MENU-ONLY -- NULL allocations on the
 *     separated boot path -- so the frames are texrects of the same wood texture
 *     (the proven W2 pattern), never menu elements.
 *   - leveltable_vehicle_usable(id) (game.c): the REAL per-track vehicle mask
 *     (+ the shared retail v79 2-player narrowing / whole-cup intersection via
 *     online_trackselect.h) -- engine truth for legality, zero drift.
 *   - level_name(id) (game.c): the REAL track name for the header.
 *   - draw_text / texrect_draw / bgdraw_* / sound_play / input_* -- the real
 *     font, 2D blit, scrolling sky, SFX and pad.
 *
 * WHAT IT OWNS (all state lives HERE, never an offline global): the per-seat
 * local pick (retail gPlayerSelectVehicle analog), the confirm latch (retail
 * gPlayerSelectConfirm analog -> intent.ready), the host OK latch
 * (start_requested), and the continuous reverse-feed intent. The remote seat's
 * column renders LIVE from the snapshot (its pick + ready), exactly like
 * retail's second player column follows the second pad.
 *
 * Legality: the pick CYCLES ONLY WITHIN the resolved mask, exactly like retail
 * (menu.c:12066-12086 skips unavailable vehicles and clamps at the ends), so
 * the published vehicle is legal on every frame by construction; the reducer's
 * ILLEGAL_VEHICLE gate stays the final authority at START.
 *
 * The ENTIRE TU is #if MDKR_ENABLE_ONLINE_BETA and it is added to the build only
 * inside the beta CMake gate (game/src/online/ is not globbed), so a normal (beta
 * OFF) build never compiles a byte of it and the release engine object is
 * untouched.
 * ==========================================================================
 */
#include "online/online_vehicleselect.h"

#if MDKR_ENABLE_ONLINE_BETA

/* Game/PR headers FIRST (same sprintf-ordering rationale as online_charselect.c). */
#include "types.h"
#include "macros.h"     /* COLOUR_RGBA32 (texrect_draw_scaled packed colour) */
#include "thread3_main.h"
#include "enums.h"      /* VEHICLE_CAR / HOVERCRAFT / PLANE, AlignmentFlags */
#include "game.h"       /* level_name, leveltable_vehicle_usable */
#include "menu.h"       /* gMenuAssets, menu_assetgroup_load/free,
                           get_player_selected_vehicle, TEXTURE_ICON_*,
                           font.h (draw_text, ...) */
#include "rcp_dkr.h"    /* texrect_draw, texrect_draw_scaled */
#include "audio.h"      /* sound_play */
#include "sound_ids.h"  /* SOUND_MENU_PICK2 / SOUND_SELECT2 / SOUND_CAR_REV2 ... */
#include "joypad.h"     /* input_pressed, input_clamp_stick_y */
#include "PR/os_cont.h" /* A_BUTTON / B_BUTTON / U_JPAD / D_JPAD / START_BUTTON */
#include "net/party_link.h"
#include "online/online_trackselect.h" /* cup_track resolver + 2P narrowing +
                                          cup intersection + locked-config +
                                          the seam's vehicle-stage pump */
#include "online/online_screen_constants.h" /* shared screen size + lobby id-space
                                               mirrors (DRY across screens) */
#include "online/online_screen_util.h" /* shared local_seat / text / blink /
                                          card + scrolling-sky backdrop + retire */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The screen size + shared launcher lobby id-space mirrors (and the 2-player-
 * narrowing track ids) live in online_screen_constants.h. These three are used
 * only by this screen. */
#define VS_ALL_VEHICLES 0x07u        /* MDKR_ONLINE_PLAYER_VEHICLE_MASK */
#define VS_CUP_COUNT 5u              /* 5 cups (== 5 worlds) */
#define VS_TRACK_NONE 0xFFFFu        /* configured_track "none" sentinel */

/* Menu SFX -- the retail setup stage's own set (menu.c:12200-12207):
 * change PICK2, confirm/OK SELECT2, back BACK3, all-confirmed CAR_REV2. */
#define VS_SFX_MOVE SOUND_MENU_PICK2
#define VS_SFX_CONFIRM SOUND_SELECT2
#define VS_SFX_BACK SOUND_MENU_BACK3
#define VS_SFX_REJECT SOUND_UNK_6A   /* retail unavailable-cell blip */
#define VS_SFX_ALL_READY SOUND_CAR_REV2

/* ---- Layout geometry (320x240): the RETAIL 2P setup coordinates ----------- */
/* trackmenu_setup_render (menu.c): track name BIGFONT y43; PLAYER labels at
 * gTracksMenuPlayerNamePositions 2P = (68,114)/(204,114) with the pulsing
 * dialogue-box highlight at (x-2,y-2)-(x+50,y+23) behind the un-confirmed seat;
 * vehicle icons at x=79 (P1) / x=176 (P2), y=139 (+2 for plane); word columns at
 * gTracksMenuVehicleNamePositions 2P = x 33 / 251, y=139 step 24 per AVAILABLE
 * vehicle (a disallowed vehicle's row is NOT drawn -- retail omits it). */
#define VS_TRACKNAME_Y 43
#define VS_OK_Y 124              /* "OK?" cue, centred between the PLAYER labels */
#define VS_LABEL_Y 114
#define VS_ICON_Y 139
#define VS_WORD_Y0 139
#define VS_WORD_STEP 24
#define VS_ICON_W 64             /* the TOP+BOTTOM icon pair is 64x64 */
#define VS_FRAME_BORDER 5        /* wood frame border around each icon */
/* Online-only footer board (connection status/help; retail has no footer --
 * kept for continuity with the browse stage's board). Sits below the icon
 * frames (icon bottom 203 + border). */
#define VS_FOOT_Y0 210
#define VS_FOOT_Y1 238
#define VS_SEAT_Y 218
#define VS_HELP_Y 230

/* Per-seat retail column anchors (seat 0 == PLAYER 1, seat 1 == PLAYER 2). */
static const s16 sVsLabelX[2] = { 68, 204 };
static const s16 sVsIconX[2] = { 79, 176 };
static const s16 sVsWordX[2] = { 33, 251 };

/* the retail setup art, by RAW texture id (the exact ids retail binds in
 * trackmenu_assets, menu.c:10810+), + the -1 terminator menu_assetgroup_load/free
 * stop on. Borrowed READ-ONLY -- menu_asset_load routes each id to load_texture,
 * so this loads the tiles into gMenuAssets[] and spawns NO menu objects.
 * NON-const because the loader takes s16* (the sPortraitAssetIds discipline). */
static s16 sOnlineVehicleAssetIds[] = {
    TEXTURE_ICON_VEHICLE_CAR_TOP,        TEXTURE_ICON_VEHICLE_CAR_BOTTOM,
    TEXTURE_ICON_VEHICLE_HOVERCRAFT_TOP, TEXTURE_ICON_VEHICLE_HOVERCRAFT_BOTTOM,
    TEXTURE_ICON_VEHICLE_PLANE_TOP,      TEXTURE_ICON_VEHICLE_PLANE_BOTTOM,
    TEXTURE_ICON_PLAYER_1,               TEXTURE_ICON_PLAYER_2,
    TEXTURE_ICON_VEHICLE_SELECT_CAR,
    TEXTURE_ICON_VEHICLE_SELECT_CAR_HIGHLIGHT,
    TEXTURE_ICON_VEHICLE_SELECT_HOVERCRAFT,
    TEXTURE_ICON_VEHICLE_SELECT_HOVERCRAFT_HIGHLIGHT,
    TEXTURE_ICON_VEHICLE_SELECT_PLANE,
    TEXTURE_ICON_VEHICLE_SELECT_PLANE_HIGHLIGHT,
    TEXTURE_SURFACE_BUTTON_WOOD,
    -1,
};

/* Resolved word-art tiles: [v][0] = highlighted (the seat's pick), [v][1] = dim.
 * Bound from gMenuAssets after the group load; every blit fails safe to a text
 * label if a tile is not resident (never trust a symbol name -- the pairing
 * below is VERIFIED BY FRAME DUMP against retail-tracksetup-vehicles-2p.png). */
static TextureHeader *sVsWordTex[3][2];
static TextureHeader *sVsPlayerTex[2]; /* PLAYER 1 / PLAYER 2 label art */
static TextureHeader *sVsWoodTex;      /* icon frame wood tile */

/* ---- Session-owned screen state (never an offline global) ------------------ */
typedef struct MdkrOnlineVehicleselectState {
    u8 vehicle;        /* the LOCAL seat's live pick (retail gPlayerSelectVehicle
                        * analog); always mask-legal, published every frame */
    u8 confirmed;      /* local confirm latch (retail gPlayerSelectConfirm ->
                        * intent.ready) */
    u8 host;           /* the local seat is the room leader (owns the OK beat) */
    u8 startReq;       /* host pressed A/START at the OK beat (latched,
                        * republished -- async-reducer-safe) */
    u8 character;      /* local seat's chosen racer (carried in the intent) */
    u8 mask;           /* the resolved track/cup's legal-vehicle mask this frame */
    u16 track;         /* the resolved track id, or VS_TRACK_NONE */
    u8 assets;         /* art + sky + fonts loaded */
    u8 leave;          /* B: back to the track browse stage (edge; see tick) */
    u8 seeded;         /* first-snapshot seed applied */
    u8 bothReadyPrev;  /* CAR_REV2 rising-edge latch */
    u8 deferNoted;     /* deferred-START witness edge (startReq && !bothReady) */
    u32 ticks;         /* stage ticks elapsed (also drives test input) */
    u32 blinkTimer;    /* retail gOptionBlinkTimer mirror ((t + rate) & 0x3F) */
    s8 stickLatchY;
} MdkrOnlineVehicleselectState;

static MdkrOnlineVehicleselectState sVs;

/* persists ACROSS entries (NOT reset by _enter's memset) so a re-entered stage
 * restarts on the vehicle you last committed. */
static u8 sLastVehicle = (u8) VEHICLE_CAR;

/* PER-ROUND stage-confirm latch: the LOCAL seat has A-confirmed a (legal)
 * vehicle on THIS round's stage and has not B-un-confirmed since. Ready may
 * latch ONLY through this confirm, per round: the track BROWSE publishes this
 * latch as its ready (see trackselect_publish_intent), so a rematch re-front
 * whose host re-locks the IDENTICAL track -- which the reducer does NOT
 * ready-clear (no config change) -- can never carry a stale
 * ready into BEGIN_LOADING while a seat is still browsing. Deliberately NOT
 * reset by _enter's memset (it must outlive the screen for the browse to
 * read); cleared by the B un-confirm and by the session's round reset
 * (mdkr_online_vehicleselect_round_reset: session begin + every race boot --
 * the engine-side analog of the reducer's clear_round). */
static u8 sVsStageConfirmedRound;

/* witness change-detect (file scope so _enter() resets it for a clean re-entry). */
static u32 sWitnessKey = 0xFFFFFFFFu;
static s32 sWitnessRemoteSeat = -2;

/* Resolved (display-only) view of the remote seat (bounded, NUL-forced name). */
typedef struct VsRemoteView {
    s8 seat;
    u8 present;
    u8 vehicle;   /* MDKR_ONLINE_SCREEN_NO_VEHICLE when none */
    u8 ready;
    char name[MDKR_PARTY_LINK_NAME_BYTES + 1u];
} VsRemoteView;

/* ---- forward decls (test seam defined at the bottom) ---------------------- */
static void vehicleselect_test_resolve(void);
static void vehicleselect_test_reset(void);
static void vehicleselect_test_reduce_and_script(void);

/* ======================================================================== *
 * Legality (engine truth: leveltable_vehicle_usable + the 2-player narrowing)
 * ======================================================================== */

static unsigned vehicleselect_occupied_seats(const MdkrPartyLinkSnapshot *snap,
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

/* Which track the vehicle legality resolves against, from the forward feed:
 *   tournament  -> the locked cup's round-0 track (engine-truth cup schedule);
 *   single race -> the host's configured_track (locked before this stage runs);
 *   nothing resolved (defensive; the browse stage normally locks first) ->
 *   VS_TRACK_NONE (no constraint). */
static u16 vehicleselect_resolve_track(const MdkrPartyLinkSnapshot *snap,
                                       bool haveSnap) {
    if (!haveSnap) {
        return VS_TRACK_NONE;
    }
    if (snap->mode == MDKR_ONLINE_SCREEN_MODE_TOURNAMENT) {
        if (snap->cup_id < VS_CUP_COUNT) {
            return mdkr_online_trackselect_cup_track(snap->cup_id, 0u);
        }
        return VS_TRACK_NONE;
    }
    /* Single race: configured_track 0xFFFF (or a defensive 0) == none. */
    if (snap->configured_track != VS_TRACK_NONE && snap->configured_track != 0u) {
        return snap->configured_track;
    }
    return VS_TRACK_NONE;
}

/* The usable-vehicle mask for the resolved track at this player count. Engine
 * truth from leveltable_vehicle_usable(), then the retail 2-player narrowing. FAIL
 * CLOSED: VS_TRACK_NONE (nothing locked yet) is the ONLY permissive (all three)
 * case. A resolved track returns its engine-truth base mask (leveltable itself
 * fail-closes an out-of-range id to CAR-only), NEVER the permissive ALL; if that
 * base is empty, or the 2-player narrowing empties it, the mask stays EMPTY so the
 * caller refuses + surfaces it rather than silently substituting CAR (which might
 * itself be illegal on that track). Every REAL track yields >=1 usable vehicle and
 * the narrowing drops at most one, so a resolved real track never empties. */
static u8 vehicleselect_track_mask(u16 trackId, unsigned occupied) {
    u8 base;
    if (trackId == VS_TRACK_NONE) {
        return VS_ALL_VEHICLES;
    }
    base = (u8) (leveltable_vehicle_usable((s32) trackId) & VS_ALL_VEHICLES);
    return (u8) (mdkr_online_trackselect_narrow_2p(base, trackId, occupied) &
                 VS_ALL_VEHICLES);
}

/* The legality mask for the local seat's pick this frame. For a TOURNAMENT with a
 * known cup this is the whole cup's INTERSECTION (the pick persists across every
 * round, so it must be legal for all of them). Single race keeps the resolved
 * (locked) track's mask. */
static u8 vehicleselect_resolve_mask(const MdkrPartyLinkSnapshot *snap,
                                     bool haveSnap, unsigned occupied) {
    if (haveSnap && snap->mode == MDKR_ONLINE_SCREEN_MODE_TOURNAMENT &&
        snap->cup_id < VS_CUP_COUNT) {
        return (u8) (mdkr_online_trackselect_cup_vehicle_mask(snap->cup_id,
                                                              occupied) &
                     VS_ALL_VEHICLES);
    }
    return vehicleselect_track_mask(vehicleselect_resolve_track(snap, haveSnap),
                                    occupied);
}

static bool vehicleselect_vehicle_legal(u8 vehicle, u8 mask) {
    return vehicle < MDKR_ONLINE_SCREEN_VEHICLE_COUNT &&
           (mask & (u8) (1u << vehicle)) != 0u;
}

/* Clamp the pick into the mask (lowest legal bit when illegal). When the mask is
 * EMPTY (only a malformed/unknown track reaches that) there is nothing legal to
 * clamp to, so the pick is left as-is and stays illegal (fail closed) rather
 * than picking CAR. */
static void vehicleselect_autonarrow(u8 mask) {
    u8 v;
    if (vehicleselect_vehicle_legal(sVs.vehicle, mask)) {
        return;
    }
    for (v = 0u; v < MDKR_ONLINE_SCREEN_VEHICLE_COUNT; v++) {
        if (mask & (u8) (1u << v)) {
            sVs.vehicle = v;
            return;
        }
    }
    /* Empty mask -- no vehicle is legal for this track. The pick stays illegal,
     * A refuses, and the reducer refuses START -- fail closed. */
}

static void vehicleselect_resolve_remote(const MdkrPartyLinkSnapshot *snap,
                                         bool haveSnap, s32 localSeat,
                                         VsRemoteView *out) {
    unsigned i;
    memset(out, 0, sizeof(*out));
    out->seat = -1;
    out->vehicle = MDKR_ONLINE_SCREEN_NO_VEHICLE;
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
        if (seat->vehicle_id < MDKR_ONLINE_SCREEN_VEHICLE_COUNT) {
            out->vehicle = seat->vehicle_id;
        }
        out->ready = seat->ready ? 1u : 0u;
        memcpy(out->name, seat->name, MDKR_PARTY_LINK_NAME_BYTES); /* untrusted */
        out->name[MDKR_PARTY_LINK_NAME_BYTES] = '\0';
        break;
    }
}

/* ======================================================================== *
 * Input
 * ======================================================================== */
typedef struct VsInput {
    s8 step;      /* -1 (up) / 0 / +1 (down) pick cycle (edge) */
    u8 aEdge;     /* A/START: confirm the pick; host OK once both confirmed */
    u8 bEdge;     /* B: un-confirm, or (host, un-confirmed) back to the browse */
} VsInput;

/* Headless scenario, resolved from the env VALUE (see vehicleselect_test_resolve):
 *   SKIP (default "1") -- the combined lane pins Whale Bay (hovercraft-only 0x2):
 *     the pick seeds on the only legal vehicle; the script tries to cycle BOTH
 *     ways (both must skip-clamp inside the mask, retail menu.c:12066-12086 --
 *     the pick never leaves HOVERCRAFT), confirms, and the host OKs. Proves the
 *     published vehicle can never leave the mask.
 *   DIVERGE -- no track pinned (all three legal); the pick cycles CAR -> PLANE
 *     and confirms, so the local seat converges to a vehicle DIFFERENT from the
 *     scripted remote's (car) -- the per-seat divergence + convergence proof.
 *   HOLD -- frame-dump only: park the stage un-confirmed so a shot can be taken.
 *   UNKNOWN -- pin an out-of-range track: prove the mask fails CLOSED. */
#define VS_SCN_SKIP 0
#define VS_SCN_DIVERGE 1
#define VS_SCN_HOLD 2
#define VS_SCN_UNKNOWN 3
/* DEFER -- the deferred/refused START window: confirm + host OK while both
 * seats read ready, then the seam's scripted remote UN-readies (standing in
 * for a real rival's B / a reducer ready-clear landing after the OK), so the
 * latched startReq sits refused (NOT_READY) and the room never leaves LOBBY.
 * Proves the footer flips to the truthful WAITING line (never a frozen
 * "STARTING...") and that B backs the request out. */
#define VS_SCN_DEFER 4
/* JOINER_HOLD -- frame-dump only: the HOLD park with the seats' roles flipped
 * (the REMOTE seat is the host), so a still shot can show the JOINER's
 * un-confirmed footer (the "HOST CAN UNLOCK TRACK WITH B" locked-door line). */
#define VS_SCN_JOINER_HOLD 5
static s8 sVsScenario = -1;

static void vehicleselect_input_scripted(VsInput *in) {
    memset(in, 0, sizeof(*in));
    if (sVsScenario == VS_SCN_HOLD || sVsScenario == VS_SCN_JOINER_HOLD) {
        /* Dump seam: nudge the pick once (a legal move when the mask allows it;
         * a skip-clamp no-op otherwise) and park un-confirmed, so a single frame
         * dump shows the un-confirmed pulsing highlight + the word states. */
        if (sVs.ticks == 2u) {
            in->step = 1;
        }
        return;
    }
    if (sVsScenario == VS_SCN_DIVERGE) {
        switch (sVs.ticks) {
        case 2u:
            in->step = 1; /* CAR -> HOVERCRAFT */
            break;
        case 3u:
            in->step = 1; /* HOVERCRAFT -> PLANE (id 2) */
            break;
        case 6u:
            in->aEdge = 1u; /* confirm PLANE (legal; differs from remote CAR) */
            break;
        default:
            break;
        }
        return;
    }
    if (sVsScenario == VS_SCN_UNKNOWN) {
        if (sVs.ticks == 3u) {
            in->aEdge = 1u; /* confirm the fail-closed CAR-only pick; park */
        }
        return;
    }
    if (sVsScenario == VS_SCN_DEFER) {
        switch (sVs.ticks) {
        case 2u:
            in->aEdge = 1u; /* confirm the seeded (mask-legal) vehicle */
            break;
        case 8u:
            in->aEdge = 1u; /* host OK at bothReady -> startReq latches; the
                             * seam then un-readies the remote (deferred) */
            break;
        case 40u:
            in->bEdge = 1u; /* back the deferred start out (B: CHANGE) */
            break;
        case 50u:
            in->aEdge = 1u; /* re-confirm against the still-un-ready rival:
                             * parks the CONFIRMED-AND-WAITING footer state
                             * ("READY! WAITING FOR ...") for the lane pin +
                             * the capture */
            break;
        default:
            break;
        }
        return;
    }
    /* SKIP (default): both cycle directions must skip-clamp inside the
     * hovercraft-only mask, then confirm + host OK. The OK press repeats each
     * tick: a reducer ready-clear (the host's config republish landing over a
     * latency-carrying room) can hollow out any single OK press, and the apply
     * is idempotent (confirm re-latch is a no-op; the OK fires only at
     * bothReady && !startReq). */
    switch (sVs.ticks) {
    case 2u:
        in->step = -1; /* toward CAR: must clamp (stays HOVERCRAFT) */
        break;
    case 3u:
        in->step = 1; /* toward PLANE: must clamp (stays HOVERCRAFT) */
        break;
    case 6u:
        in->aEdge = 1u; /* confirm the (only) legal vehicle */
        break;
    default:
        if (sVs.ticks >= 12u) {
            in->aEdge = 1u; /* host OK once both seats read ready */
        }
        break;
    }
}

/* Live pad: U/D edges plus a latched analog stick (retail cycles the pick with
 * the stick, menu.c:12066), local player only. A and START both confirm/OK
 * (retail accepts either, menu.c:12059). */
static void vehicleselect_input_live(VsInput *in) {
    u32 pressed = input_pressed(MDKR_ONLINE_SCREEN_LOCAL_PAD);
    s32 sy = input_clamp_stick_y(MDKR_ONLINE_SCREEN_LOCAL_PAD);
    s8 wantY = 0;

    memset(in, 0, sizeof(*in));

    if (pressed & D_JPAD) {
        in->step = 1;
    } else if (pressed & U_JPAD) {
        in->step = -1;
    }
    if (sy > 40) {
        wantY = -1;
    } else if (sy < -40) {
        wantY = 1;
    }
    if (in->step == 0 && wantY != 0 && sVs.stickLatchY == 0) {
        in->step = wantY;
    }
    sVs.stickLatchY = wantY;

    in->aEdge = (pressed & (A_BUTTON | START_BUTTON)) ? 1u : 0u;
    in->bEdge = (pressed & B_BUTTON) ? 1u : 0u;
}

/* the TRACKSELECT lane's stage choreography (env MDKR_TEST_ONLINE_TRACKSELECT
 * armed WITHOUT the dedicated VEHICLESELECT seam). Keyed on the stage ENTRY
 * count: entry 1 proves the stage back-stack (B -> back to the browse, which
 * re-locks); entry 2+ confirms the seeded (mask-legal) vehicle and, for the
 * single-host scenario, presses the host OK. The joiner/hold scenario values
 * only confirm (the seam's remote host owns the OK). */
static u8 sVsTsLaneEntries; /* incremented each _enter while the flavor is armed */
static s8 sVsTsLaneFlavor = -1; /* -1 unresolved, 0 off, 1 single-host, 2 confirm-only */
static u8 vehicleselect_tslane_flavor(void) {
    if (sVsTsLaneFlavor < 0) {
        const char *e = getenv("MDKR_TEST_ONLINE_TRACKSELECT");
        if (e == NULL) {
            sVsTsLaneFlavor = 0;
        } else if (strstr(e, "joiner") != NULL || strstr(e, "hold") != NULL ||
                   strstr(e, "rematch") != NULL) {
            /* joiner-side scenarios (incl. the same-track REMATCH arm): confirm
             * only -- the seam's scripted remote host owns the OK. */
            sVsTsLaneFlavor = 2;
        } else {
            sVsTsLaneFlavor = 1;
        }
    }
    return (u8) sVsTsLaneFlavor;
}

static void vehicleselect_input_tslane(VsInput *in) {
    memset(in, 0, sizeof(*in));
    if (vehicleselect_tslane_flavor() == 1u && sVsTsLaneEntries <= 1u) {
        if (sVs.ticks == 3u) {
            in->bEdge = 1u; /* stage back-stack: vehicle stage -> browse */
        }
        return;
    }
    if (sVs.ticks == 2u) {
        in->aEdge = 1u; /* confirm the seeded (mask-legal) vehicle */
    } else if (vehicleselect_tslane_flavor() == 1u && sVs.ticks >= 10u) {
        in->aEdge = 1u; /* host OK; repeats so a ready-clear window cannot
                         * swallow the single press (idempotent apply) */
    }
}

/* minimal scripted input for the headless lanes that drive the native flow
 * through this stage but arm neither the dedicated VEHICLESELECT choreography
 * nor the TRACKSELECT-lane flavor above: the LOBBY-START / LOBBY-TOURNAMENT
 * loopback lanes (a REAL launcher reducer) and the self-contained CHARSELECT
 * lane. It stays trivial: the pick seeds on the committed, always-mask-legal
 * vehicle, so a single A confirms it and -- when the local seat is the host --
 * a later A presses the OK (start latches + republishes, so an ASYNC reducer
 * converges regardless of timing; a joiner's publish never carries start).
 * Inert (unresolved -> off) in a run that arms none of these seams, so live
 * play uses the real pad. Resolved once. */
static s8 sVsScriptedConfirm = -1; /* -1 unresolved, 0 off, 1 on */
static u8 vehicleselect_scripted_confirm_active(void) {
    if (sVsScriptedConfirm < 0) {
        sVsScriptedConfirm = (getenv("MDKR_TEST_ONLINE_LOBBY_START") != NULL ||
                              getenv("MDKR_TEST_ONLINE_LOBBY_TOURNAMENT") != NULL ||
                              getenv("MDKR_TEST_ONLINE_CHARSELECT") != NULL)
                                 ? 1
                                 : 0;
    }
    return (u8) (sVsScriptedConfirm > 0 ? 1 : 0);
}

static void vehicleselect_input_scripted_confirm(VsInput *in) {
    memset(in, 0, sizeof(*in));
    if (sVs.ticks == 2u) {
        in->aEdge = 1u; /* confirm the seeded (mask-legal) vehicle */
    } else if (sVs.ticks >= 8u) {
        in->aEdge = 1u; /* host OK (a joiner's publish drops it); repeats so a
                         * reducer ready-clear window -- the host's config
                         * republish landing over a latency-carrying room --
                         * cannot swallow the single press (idempotent apply) */
    }
}

static void vehicleselect_gather_input(VsInput *in) {
    if (mdkr_online_vehicleselect_test_active()) {
        vehicleselect_input_scripted(in);
    } else if (vehicleselect_tslane_flavor() != 0u) {
        vehicleselect_input_tslane(in);
    } else if (vehicleselect_scripted_confirm_active()) {
        vehicleselect_input_scripted_confirm(in);
    } else {
        vehicleselect_input_live(in);
    }
}

/* Apply one input step -- a line-for-line mirror of retail's stage-0 handler
 * (menu.c func_80092188 case 0) for the LOCAL seat:
 *   stick/pad step: cycle the pick, SKIPPING vehicles outside the mask and
 *     CLAMPING at the ends (retail's do/while walk; no wrap);
 *   A/START: confirm the pick (SELECT2); once BOTH seats are confirmed, the
 *     host's A/START is the OK beat (start_requested latches);
 *   B: un-confirm if confirmed (BACK3); a host's un-confirmed B backs the
 *     stage out to the track browse. (Retail backs the whole 2P group out only
 *     when NObody is confirmed; online the remote republishes ready
 *     continuously, so the local-latch analog keeps the host un-strandable. A
 *     joiner's un-confirmed B is a no-op -- it has no track cursor to return
 *     to, and its exit remains the charselect back-stack.) */
static void vehicleselect_apply_input(const VsInput *in, u8 bothReady) {
    if (in->bEdge) {
        if (sVs.confirmed) {
            sVs.confirmed = 0u;
            sVsStageConfirmedRound = 0u; /* un-confirm drops the round latch */
            sVs.startReq = 0u;
            sound_play(VS_SFX_BACK, NULL);
        } else if (sVs.host) {
            sVs.leave = 1u;
            sound_play(VS_SFX_BACK, NULL);
        }
        return;
    }
    if (in->step != 0 && !sVs.confirmed) {
        s32 v = (s32) sVs.vehicle;
        do {
            v += in->step;
        } while (v >= 0 && v < (s32) MDKR_ONLINE_SCREEN_VEHICLE_COUNT &&
                 (sVs.mask & (u8) (1u << v)) == 0u);
        if (v >= 0 && v < (s32) MDKR_ONLINE_SCREEN_VEHICLE_COUNT &&
            (u8) v != sVs.vehicle) {
            sVs.vehicle = (u8) v;
            sLastVehicle = sVs.vehicle; /* persistence */
            sound_play(VS_SFX_MOVE, NULL);
        }
        /* out of range: clamp -- keep the current pick (retail restores orig) */
    }
    if (in->aEdge) {
        if (!sVs.confirmed) {
            if (vehicleselect_vehicle_legal(sVs.vehicle, sVs.mask)) {
                sVs.confirmed = 1u;
                sVsStageConfirmedRound = 1u; /* THE per-round ready latch */
                sLastVehicle = sVs.vehicle;
                sound_play(VS_SFX_CONFIRM, NULL);
            } else {
                /* only an EMPTY mask (malformed track) reaches this: nothing is
                 * legal to confirm -- refuse loudly, never a silent commit. */
                sound_play(VS_SFX_REJECT, NULL);
                fprintf(stderr,
                        "[online-vehicleselect] reject vehicle=%u (illegal for "
                        "track=%u mask=0x%x)\n",
                        (unsigned) sVs.vehicle, (unsigned) sVs.track,
                        (unsigned) sVs.mask);
            }
        } else if (sVs.host && bothReady && !sVs.startReq) {
            /* the OK beat: retail's post-CAR_REV2 confirm (racer count is
             * skipped -- online v1 is a fixed 2 racers). */
            sVs.startReq = 1u;
            sound_play(VS_SFX_CONFIRM, NULL);
            fprintf(stderr, "[online-vehicleselect] host OK -> start requested\n");
        }
    }
}

/* Publish the FULL local intent every frame (continuous republish: the reducer
 * clears ready on any selection change, so republishing reconverges within a
 * pump). The pick is always mask-legal; ready mirrors the retail per-player
 * CONFIRM. The HOST also republishes its locked session config (the browse
 * stage's pick -- a single lock-tick publish could be lost on a real transport)
 * and the latched OK (start_requested). A joiner leaves every host-only field
 * at the UNSET sentinels. */
static void vehicleselect_publish_intent(void) {
    MdkrPartyLinkLocalIntent intent;
    mdkr_party_link_intent_init(&intent);
    if (sVs.character < MDKR_ONLINE_SCREEN_CHAR_COUNT) {
        intent.hover_character = sVs.character;
        intent.confirmed = 1u;
    }
    intent.vehicle_id = sVs.vehicle;
    intent.ready = sVs.confirmed ? 1u : 0u;
    /* an un-confirmed stage emits the UN-ready (the ready-XOR-backout pair
     * charselect publishes): the planner converges un-ready only through
     * backout -> CHANGE_SELECTION (SET_READY 0) -- ready=0 alone plans nothing
     * (party_link.c) and would leave a stale room ready standing after a B
     * un-confirm on an unchanged config. */
    intent.backout = sVs.confirmed ? 0u : 1u;
    if (sVs.host) {
        mdkr_online_trackselect_locked_config(&intent.mode, &intent.config_track,
                                              &intent.cup_id);
        intent.start_requested = sVs.startReq ? 1u : 0u;
    }
    mdkr_party_link_intent_publish(&intent);
}

/* ======================================================================== *
 * Render (the retail 2P track-setup composition, at the retail coordinates)
 * ======================================================================== */

/* Upper-case copy (the authored-art BIGFONT is an all-caps face). */
static void vehicleselect_upper(const char *src, char *dst, u32 cap) {
    u32 i;
    if (src == NULL) {
        src = "?";
    }
    for (i = 0u; src[i] != '\0' && i + 1u < cap; i++) {
        char c = src[i];
        if (c >= 'a' && c <= 'z') {
            c = (char) (c - 'a' + 'A');
        }
        dst[i] = c;
    }
    dst[i] = '\0';
}

/* Tournament cup display names for the header (cup display order). */
static const char *const sVsCupNames[VS_CUP_COUNT] = {
    "DINO DOMAIN CUP", "SNOWFLAKE CUP", "SHERBET CUP", "DRAGON FOREST CUP",
    "FUTURE FUN CUP",
};

/* One word-art cell: the highlighted variant for the seat's pick, the dim
 * variant otherwise (retail [v*3+1] / [v*3+2]). Falls back to a SMALLFONT
 * label styled to the same read if the tile is not resident. */
static void vehicleselect_draw_word(u8 vehicle, u8 seatCol, bool picked, s32 y) {
    TextureHeader *tex =
        (vehicle < 3u) ? sVsWordTex[vehicle][picked ? 0 : 1] : NULL;
    s32 x = (s32) sVsWordX[seatCol];
    if (tex != NULL) {
        mdkr_online_screen_blit(tex, x, y, 255, 255, 255, 255);
        return;
    }
    /* fallback (art not resident): text styled to the retail read. */
    mdkr_online_screen_text(x, y + 8, ASSET_FONTS_SMALLFONT,
                            (char *) mdkr_online_vehicle_names[vehicle],
                            ALIGN_MIDDLE_LEFT, picked ? 255 : 140,
                            picked ? 224 : 140, picked ? 96 : 140);
}

/* One seat column: the pulsing highlight card behind an un-confirmed seat
 * (retail's dialogue box 7, colour (255, blink, 0) -- menu.c:11659+11764), the
 * PLAYER n label art, the framed vehicle icon and the word list states. */
static void vehicleselect_draw_column(u8 seatCol, u8 pick, u8 confirmed,
                                      u8 present, s32 blink) {
    s32 labelX = (s32) sVsLabelX[seatCol];
    s32 iconX = (s32) sVsIconX[seatCol];
    s32 y;
    u8 v;

    /* the retail pulsing box behind the seat that has NOT yet confirmed
     * (set_current_dialogue_box_coords(7, x-2, y-2, x+50, y+23) with background
     * (255, blink, 0); drawn with the same fill vocabulary render_dialogue_box
     * uses -- the dialogue-slot state machine itself is offline-owned). */
    if (present && !confirmed) {
        mdkr_online_screen_card(labelX - 2, VS_LABEL_Y - 2, labelX + 50,
                                VS_LABEL_Y + 23, 255, blink, 0, 176);
    }

    /* PLAYER n label art (fallback: text). Dim the label when the seat is
     * absent (online-only state; retail always has both). */
    if (sVsPlayerTex[seatCol] != NULL) {
        mdkr_online_screen_blit(sVsPlayerTex[seatCol], labelX, VS_LABEL_Y,
                                present ? 255 : 120, present ? 255 : 120,
                                present ? 255 : 120, 255);
    } else {
        mdkr_online_screen_text(labelX + 25, VS_LABEL_Y + 10, ASSET_FONTS_FUNFONT,
                                seatCol == 0u ? "PLAYER 1" : "PLAYER 2",
                                ALIGN_MIDDLE_CENTER, present ? 255 : 120,
                                present ? 255 : 120, present ? 255 : 120);
    }

    if (!present) {
        return; /* no pick/words/icon for an empty seat */
    }

    /* Word list: one row per AVAILABLE vehicle (retail omits a disallowed
     * vehicle's row entirely -- menu.c:11777), highlighted for this seat's
     * pick, dim otherwise. */
    y = VS_WORD_Y0;
    for (v = 0u; v < MDKR_ONLINE_SCREEN_VEHICLE_COUNT; v++) {
        if (sVs.mask & (u8) (1u << v)) {
            vehicleselect_draw_word(v, seatCol, pick == v, y);
            y += VS_WORD_STEP;
        }
    }

    /* Framed vehicle icon: wood tile under the 64x64 icon pair (the retail
     * frame element gMenuImages[7] is offline-only, so the frame is a texrect
     * of the same wood texture -- the W2 pattern), +2px for the plane (retail
     * menu.c:11803). */
    if (pick < MDKR_ONLINE_SCREEN_VEHICLE_COUNT) {
        s32 iconY = VS_ICON_Y + ((pick == (u8) VEHICLE_PLANE) ? 2 : 0);
        if (sVsWoodTex != NULL && sVsWoodTex->width != 0 &&
            sVsWoodTex->height != 0) {
            mdkr_online_screen_blit_scaled(
                sVsWoodTex, (f32) (iconX - VS_FRAME_BORDER),
                (f32) (iconY - VS_FRAME_BORDER),
                (f32) (VS_ICON_W + 2 * VS_FRAME_BORDER) / (f32) sVsWoodTex->width,
                (f32) (VS_ICON_W + 2 * VS_FRAME_BORDER) / (f32) sVsWoodTex->height,
                COLOUR_RGBA32(255, 255, 255, 255));
        }
        (void) mdkr_online_screen_draw_vehicle(pick, iconX + VS_ICON_W / 2,
                                               iconY, 255u, 255u, 255u, 255u);
    }
}

static void vehicleselect_render(const MdkrPartyLinkSnapshot *snap, bool haveSnap,
                                 s32 localSeat, const VsRemoteView *rv,
                                 u8 bothReady) {
    const char *rname = rv->name[0] != '\0' ? rv->name : "RIVAL";
    s32 blink = mdkr_online_screen_blink(sVs.blinkTimer);
    char nameBuf[32];
    char line[64];
    u8 seatCol;

    /* Header: the locked track's name (single) / the locked cup (tournament),
     * BIGFONT at the retail y (menu.c:11648 draws it at y=43 -- authored art,
     * untinted, exactly how mdkr_online_screen_text renders BIGFONT). */
    if (haveSnap && snap->mode == MDKR_ONLINE_SCREEN_MODE_TOURNAMENT &&
        snap->cup_id < VS_CUP_COUNT) {
        (void) snprintf(nameBuf, sizeof(nameBuf), "%s", sVsCupNames[snap->cup_id]);
    } else if (sVs.track != VS_TRACK_NONE) {
        vehicleselect_upper(level_name((s32) sVs.track), nameBuf, sizeof(nameBuf));
    } else {
        (void) snprintf(nameBuf, sizeof(nameBuf), "ANY TRACK");
    }
    mdkr_online_screen_text(MDKR_ONLINE_SCREEN_W_HALF, VS_TRACKNAME_Y,
                            ASSET_FONTS_BIGFONT, nameBuf, ALIGN_MIDDLE_CENTER,
                            255, 224, 96);

    /* The two seat columns, SEAT-ordered (seat 0 == PLAYER 1, seat 1 == PLAYER 2
     * -- retail's columns are pad-ordered, not local/remote-ordered). */
    for (seatCol = 0u; seatCol < 2u; seatCol++) {
        u8 pick = MDKR_ONLINE_SCREEN_NO_VEHICLE;
        u8 confirmed = 0u;
        u8 present = 0u;
        if (haveSnap && seatCol < MDKR_PARTY_LINK_SEATS &&
            snap->seats[seatCol].occupied) {
            present = 1u;
            if ((s32) seatCol == localSeat) {
                pick = sVs.vehicle;
                confirmed = sVs.confirmed;
            } else {
                /* the REMOTE pick, live from the snapshot (retail's second
                 * column follows the second pad; ours follows the feed). */
                pick = (snap->seats[seatCol].vehicle_id <
                        MDKR_ONLINE_SCREEN_VEHICLE_COUNT)
                           ? snap->seats[seatCol].vehicle_id
                           : (u8) MDKR_ONLINE_SCREEN_NO_VEHICLE;
                confirmed = snap->seats[seatCol].ready ? 1u : 0u;
            }
        } else if (!haveSnap && seatCol == 0u) {
            present = 1u; /* defensive solo render */
            pick = sVs.vehicle;
            confirmed = sVs.confirmed;
        }
        vehicleselect_draw_column(seatCol, pick, confirmed, present, blink);
    }

    /* The OK beat: both seats confirmed -> retail's "OK?" read (racer count is
     * skipped online). BIGFONT authored art, centred between the labels. */
    if (bothReady) {
        mdkr_online_screen_text(MDKR_ONLINE_SCREEN_W_HALF, VS_OK_Y,
                                ASSET_FONTS_BIGFONT, "OK?", ALIGN_MIDDLE_CENTER,
                                255, 224, 96);
    }

    /* Online-only footer (connection truth + verbs; retail has no footer). */
    mdkr_online_screen_panel(10, VS_FOOT_Y0, 310, VS_FOOT_Y1);
    (void) snprintf(line, sizeof(line), "YOU: %s",
                    sVs.confirmed ? "READY" : "CHOOSING");
    mdkr_online_screen_text(24, VS_SEAT_Y, ASSET_FONTS_SMALLFONT, line,
                            ALIGN_MIDDLE_LEFT, sVs.confirmed ? 120 : 220,
                            sVs.confirmed ? 255 : 220, sVs.confirmed ? 120 : 220);
    if (!rv->present) {
        mdkr_online_screen_text(MDKR_ONLINE_SCREEN_W - 24, VS_SEAT_Y,
                                ASSET_FONTS_SMALLFONT, "WAITING FOR PLAYER...",
                                ALIGN_MIDDLE_RIGHT, 150, 150, 150);
    } else {
        (void) snprintf(line, sizeof(line), "%.12s: %s", rname,
                        rv->ready ? "READY" : "CHOOSING");
        mdkr_online_screen_text(MDKR_ONLINE_SCREEN_W - 24, VS_SEAT_Y,
                                ASSET_FONTS_SMALLFONT, line, ALIGN_MIDDLE_RIGHT,
                                rv->ready ? 120 : 220, rv->ready ? 255 : 220,
                                rv->ready ? 120 : 220);
    }
    if (sVs.startReq && bothReady) {
        /* the start request is latched AND both seats still read ready --
         * BEGIN_LOADING is genuinely in flight (acceptance flips the phase and
         * the stage advances off this screen within a pump). */
        mdkr_online_screen_text(MDKR_ONLINE_SCREEN_W_HALF, VS_HELP_Y,
                                ASSET_FONTS_SMALLFONT, "STARTING...",
                                ALIGN_MIDDLE_CENTER, 120, 255, 120);
    } else if (sVs.startReq) {
        /* the DEFERRED/REFUSED window: the OK is latched but the rival is not
         * (or no longer) ready, so the reducer refuses BEGIN_LOADING
         * (NOT_READY) and the room stays in LOBBY. Truthful status instead of
         * a frozen "STARTING...", and the B escape stays advertised -- B here
         * un-confirms AND drops the latched start request
         * (vehicleselect_apply_input), so the hint matches reality. */
        (void) snprintf(line, sizeof(line),
                        "WAITING FOR %.12s TO CHOOSE...   B: CHANGE", rname);
        mdkr_online_screen_text(MDKR_ONLINE_SCREEN_W_HALF, VS_HELP_Y,
                                ASSET_FONTS_SMALLFONT, line,
                                ALIGN_MIDDLE_CENTER, 200, 200, 200);
    } else if (bothReady) {
        if (sVs.host) {
            mdkr_online_screen_text(MDKR_ONLINE_SCREEN_W_HALF, VS_HELP_Y,
                                    ASSET_FONTS_SMALLFONT, "A: GO   B: CHANGE",
                                    ALIGN_MIDDLE_CENTER, 255, 255, 255);
        } else {
            /* both seats confirmed but the GO is the HOST's -- the joiner's A
             * is inert here, so never advertise it; name the actual wait
             * (charselect's "WAITING FOR HOST TO START..." family) and keep
             * the truthful B (un-confirm). */
            (void) snprintf(line, sizeof(line),
                            "WAITING FOR %.12s TO START...   B: CHANGE", rname);
            mdkr_online_screen_text(MDKR_ONLINE_SCREEN_W_HALF, VS_HELP_Y,
                                    ASSET_FONTS_SMALLFONT, line,
                                    ALIGN_MIDDLE_CENTER, 200, 200, 200);
        }
    } else if (sVs.confirmed) {
        /* confirmed-and-waiting: A is inert until the rival confirms too, so
         * "A: SELECT" would be a lie -- mirror charselect's confirmed-seat
         * read ("READY! WAITING FOR <name>...") with the truthful B verb
         * (un-confirm to change the pick). */
        (void) snprintf(line, sizeof(line),
                        "READY! WAITING FOR %.12s...   B: CHANGE", rname);
        mdkr_online_screen_text(MDKR_ONLINE_SCREEN_W_HALF, VS_HELP_Y,
                                ASSET_FONTS_SMALLFONT, line,
                                ALIGN_MIDDLE_CENTER, 120, 255, 120);
    } else {
        /* (no "VEHICLE" literal here: the kerned SMALLFONT swallows the narrow
         * I between H and C -- it rendered as "VEHCLE" on capture.)
         * The JOINER's line also names the locked door: after the host's lock
         * the joiner has NO back path of its own (its un-confirmed B is a
         * no-op), so a joiner wanting a different track/racer must ask the
         * HOST to B out of the stage -- without this sentence the limitation
         * reads as a broken button (stories audit gap #7). */
        mdkr_online_screen_text(MDKR_ONLINE_SCREEN_W_HALF, VS_HELP_Y,
                                ASSET_FONTS_SMALLFONT,
                                sVs.host ? "A: SELECT   B: BACK"
                                         : "A: SELECT   HOST CAN UNLOCK "
                                           "TRACK WITH B",
                                ALIGN_MIDDLE_CENTER, 255, 255, 255);
    }

    /* Ground: keep the LOCKED world's scrolling sky armed (the same ground the
     * browse stage showed -- the stage flip keeps the track screen's ground). */
    mdkr_online_screen_backdrop(
        mdkr_online_screen_sky_world_for_snapshot(snap, haveSnap));
}

/* Bounded stderr witness: one line only when the visible state changes. Folds the
 * local seat's converged vehicle/ready AND the remote's vehicle/ready so a
 * remote-only change still emits a row (the "both converge" proof). */
static void vehicleselect_witness(const MdkrPartyLinkSnapshot *snap, bool haveSnap,
                                  s32 localSeat, const VsRemoteView *rv) {
    u8 localSeatVeh = MDKR_ONLINE_SCREEN_NO_VEHICLE;
    u8 localSeatReady = 0u;
    u8 remoteNibble;
    u32 key;

    if (haveSnap && localSeat >= 0) {
        localSeatVeh = snap->seats[localSeat].vehicle_id;
        localSeatReady = snap->seats[localSeat].ready;
    }
    remoteNibble = (rv->vehicle < MDKR_ONLINE_SCREEN_VEHICLE_COUNT) ? rv->vehicle : 0xFu;

    key = ((u32) sVs.vehicle) | ((u32) sVs.vehicle << 2) |
          ((u32) sVs.confirmed << 4) | ((u32) sVs.mask << 5) |
          ((u32) remoteNibble << 9) | ((u32) (rv->ready ? 1u : 0u) << 13) |
          ((u32) rv->present << 14) |
          ((u32) ((localSeatVeh < MDKR_ONLINE_SCREEN_VEHICLE_COUNT) ? localSeatVeh : 7u)
           << 15) |
          ((u32) localSeatReady << 18) | ((u32) (sVs.track & 0x3Fu) << 19) |
          ((u32) sVs.startReq << 25);
    if (key == sWitnessKey && rv->seat == sWitnessRemoteSeat) {
        return;
    }
    sWitnessKey = key;
    sWitnessRemoteSeat = rv->seat;

    /* "cursor" is the live pick itself now (retail has no separate cursor:
     * moving the stick IS changing the pick); kept in the row so the lane
     * regex shape stays familiar. */
    fprintf(stderr,
            "[online-vehicleselect] render cursor=%u vehicle=%u legal=0x%x "
            "track=%u local{seatVeh=%u seatReady=%u conf=%u} "
            "remote{seat=%d veh=%u ready=%u name=%.*s} "
            "intent{vehicle=%u ready=%u}\n",
            (unsigned) sVs.vehicle, (unsigned) sVs.vehicle, (unsigned) sVs.mask,
            (unsigned) sVs.track, (unsigned) localSeatVeh,
            (unsigned) localSeatReady, (unsigned) sVs.confirmed, (int) rv->seat,
            (unsigned) rv->vehicle, (unsigned) rv->ready,
            (int) MDKR_PARTY_LINK_NAME_BYTES,
            rv->name[0] != '\0' ? rv->name : "-", (unsigned) sVs.vehicle,
            (unsigned) (sVs.confirmed ? 1u : 0u));
}

/* ======================================================================== *
 * Lifecycle
 * ======================================================================== */
void mdkr_online_vehicleselect_enter(void) {
    MdkrPartyLinkSnapshot snap;
    bool haveSnap;
    s32 localSeat;
    s8 defaultVehicle;
    unsigned occupied;

    memset(&sVs, 0, sizeof(sVs));
    sVs.character = MDKR_ONLINE_SCREEN_NO_CHARACTER;
    sVs.track = VS_TRACK_NONE;
    sVs.mask = VS_ALL_VEHICLES;

    /* reset the witness change-detect + the headless seam so a second entry
     * is clean (a re-entered stage re-scripts from scratch). */
    sWitnessKey = 0xFFFFFFFFu;
    sWitnessRemoteSeat = -2;
    vehicleselect_test_reset();
    if (vehicleselect_tslane_flavor() != 0u) {
        sVsTsLaneEntries++;
    }

    /* Seed the pick from the CHARSELECT default (same source), or the last
     * committed one, then clamp to the resolved track's mask. */
    defaultVehicle = get_player_selected_vehicle(MDKR_ONLINE_SCREEN_LOCAL_PAD);
    if (defaultVehicle < 0 || (u8) defaultVehicle >= MDKR_ONLINE_SCREEN_VEHICLE_COUNT) {
        defaultVehicle = (s8) VEHICLE_CAR;
    }
    sVs.vehicle = (sLastVehicle < MDKR_ONLINE_SCREEN_VEHICLE_COUNT) ? sLastVehicle
                                                           : (u8) defaultVehicle;

    /* Read the first snapshot so the initial mask/character/pick are resolved
     * before the first render (the local seat carries the CHARSELECT pick + the
     * host's locked track/cup). */
    haveSnap = mdkr_party_link_read(&snap);
    localSeat = haveSnap ? mdkr_online_screen_local_seat(&snap) : -1;
    occupied = vehicleselect_occupied_seats(&snap, haveSnap);
    if (haveSnap && localSeat >= 0) {
        sVs.host = snap.seats[localSeat].is_host ? 1u : 0u;
        if (snap.seats[localSeat].character_id < MDKR_ONLINE_SCREEN_CHAR_COUNT) {
            sVs.character = snap.seats[localSeat].character_id;
        }
        if (snap.seats[localSeat].vehicle_id < MDKR_ONLINE_SCREEN_VEHICLE_COUNT) {
            sVs.vehicle = snap.seats[localSeat].vehicle_id;
        }
    } else {
        sVs.host = 1u; /* UI-only default (launcher-side dispatch is host-gated) */
    }
    sVs.track = vehicleselect_resolve_track(&snap, haveSnap);
    sVs.mask = vehicleselect_resolve_mask(&snap, haveSnap, occupied);
    vehicleselect_autonarrow(sVs.mask);
    sVs.seeded = 1u;

    /* Borrow the retail setup art (labels + word art + icons + frame wood) and
     * the shared scrolling-sky group -- the same raw-texrect asset borrow the
     * other native screens make (menu_imagegroup_load / gMenuImages is
     * offline-only, so every id here is a plain texture id). */
    menu_assetgroup_load(sOnlineVehicleAssetIds);
    menu_assetgroup_load(sOnlineSkyAssetIds);
    sVsWordTex[VEHICLE_CAR][0] =
        (TextureHeader *) gMenuAssets[TEXTURE_ICON_VEHICLE_SELECT_CAR_HIGHLIGHT];
    sVsWordTex[VEHICLE_CAR][1] =
        (TextureHeader *) gMenuAssets[TEXTURE_ICON_VEHICLE_SELECT_CAR];
    sVsWordTex[VEHICLE_HOVERCRAFT][0] = (TextureHeader *)
        gMenuAssets[TEXTURE_ICON_VEHICLE_SELECT_HOVERCRAFT_HIGHLIGHT];
    sVsWordTex[VEHICLE_HOVERCRAFT][1] =
        (TextureHeader *) gMenuAssets[TEXTURE_ICON_VEHICLE_SELECT_HOVERCRAFT];
    sVsWordTex[VEHICLE_PLANE][0] =
        (TextureHeader *) gMenuAssets[TEXTURE_ICON_VEHICLE_SELECT_PLANE_HIGHLIGHT];
    sVsWordTex[VEHICLE_PLANE][1] =
        (TextureHeader *) gMenuAssets[TEXTURE_ICON_VEHICLE_SELECT_PLANE];
    sVsPlayerTex[0] = (TextureHeader *) gMenuAssets[TEXTURE_ICON_PLAYER_1];
    sVsPlayerTex[1] = (TextureHeader *) gMenuAssets[TEXTURE_ICON_PLAYER_2];
    sVsWoodTex = (TextureHeader *) gMenuAssets[TEXTURE_SURFACE_BUTTON_WOOD];

    load_font(ASSET_FONTS_BIGFONT);
    load_font(ASSET_FONTS_FUNFONT);
    load_font(ASSET_FONTS_SMALLFONT);
    mdkr_online_screen_hd_text_ref();

    sVs.assets = 1u;
    /* The LOCKED world's scrolling sky -- the SAME ground the browse stage
     * showed, so the stage flip reads as one track screen. */
    mdkr_online_screen_backdrop(
        mdkr_online_screen_sky_world_for_snapshot(&snap, haveSnap));

    /* Entry fade: the session arms a one-shot skip for the intra-screen stage
     * flip (browse -> vehicles); a fresh entry from anywhere else keeps the
     * retail reveal. Menu music continues either way (idempotent). */
    mdkr_online_screen_fade_in_from_black();
    mdkr_online_screen_menu_music();

    fprintf(stderr,
            "[online-vehicleselect] enter: native screen up character=%u "
            "vehicle=%u track=%u mask=0x%x (retail vehicle stage, offline _loop "
            "bypassed)\n",
            (unsigned) sVs.character, (unsigned) sVs.vehicle,
            (unsigned) sVs.track, (unsigned) sVs.mask);
}

void mdkr_online_vehicleselect_exit(void) {
    if (sVs.assets) {
        /* Retire the frame's authored display list FIRST (this frame's label/
         * word/icon/wood texrects reference the tiles freed below -- the
         * freed-texture DL corruption fix, see mdkr_online_screen_dl_retire). */
        mdkr_online_screen_dl_retire();
        /* Disarm the borrowed sky BEFORE freeing its tiles (bgdraw_render
         * lifetime), then balance the loads _enter() took. */
        mdkr_online_screen_backdrop_clear();
        mdkr_online_screen_hd_text_unref();
        unload_font(ASSET_FONTS_SMALLFONT);
        unload_font(ASSET_FONTS_FUNFONT);
        unload_font(ASSET_FONTS_BIGFONT);
        menu_assetgroup_free(sOnlineSkyAssetIds);
        menu_assetgroup_free(sOnlineVehicleAssetIds);
        memset(sVsWordTex, 0, sizeof(sVsWordTex));
        sVsPlayerTex[0] = NULL;
        sVsPlayerTex[1] = NULL;
        sVsWoodTex = NULL;
        sVs.assets = 0u;
        fprintf(stderr,
                "[online-vehicleselect] exit: freed vehicle-stage assets\n");
    }
}

MdkrOnlineVehicleselectResult mdkr_online_vehicleselect_tick(s32 updateRate) {
    MdkrPartyLinkSnapshot snap;
    bool haveSnap;
    s32 localSeat;
    unsigned occupied;
    u8 bothReady;
    VsRemoteView rv;
    VsInput in;

    /* retail blink cadence for the un-confirmed highlight (gOptionBlinkTimer). */
    sVs.blinkTimer = (sVs.blinkTimer + (u32) ((updateRate > 0) ? updateRate : 1)) &
                     0x3Fu;

    haveSnap = mdkr_party_link_read(&snap);
    localSeat = haveSnap ? mdkr_online_screen_local_seat(&snap) : -1;
    occupied = vehicleselect_occupied_seats(&snap, haveSnap);

    /* Track the local seat's role/character + the resolved-track mask each
     * frame; auto-narrow the pick into the mask BEFORE input so the cycle walks
     * this frame's mask. */
    if (haveSnap && localSeat >= 0) {
        sVs.host = snap.seats[localSeat].is_host ? 1u : 0u;
        if (snap.seats[localSeat].character_id < MDKR_ONLINE_SCREEN_CHAR_COUNT) {
            sVs.character = snap.seats[localSeat].character_id;
        }
    }
    sVs.track = vehicleselect_resolve_track(&snap, haveSnap);
    sVs.mask = vehicleselect_resolve_mask(&snap, haveSnap, occupied);
    vehicleselect_autonarrow(sVs.mask);

    /* Both seats confirmed = the retail all-ready beat. Local truth is the
     * screen latch (the snapshot lags a pump); the remote's is its seat ready. */
    vehicleselect_resolve_remote(&snap, haveSnap, localSeat, &rv);
    bothReady = (u8) ((sVs.confirmed && rv.present && rv.ready) ? 1u : 0u);
    if (bothReady && !sVs.bothReadyPrev) {
        /* retail: the LAST confirm plays the rev (menu.c:12098). */
        sound_play(VS_SFX_ALL_READY, NULL);
        fprintf(stderr, "[online-vehicleselect] all vehicles confirmed "
                        "(CAR_REV2)\n");
    }
    sVs.bothReadyPrev = bothReady;

    vehicleselect_gather_input(&in);
    vehicleselect_apply_input(&in, bothReady);

    /* DEFERRED-START truthfulness witness (edge; bounded). The host's OK is
     * latched but the rival is not (or no longer) ready -- the reducer refuses
     * BEGIN_LOADING (NOT_READY) and the room stays in LOBBY, so the footer
     * flips to the truthful WAITING line with the B escape advertised (see
     * vehicleselect_render). One line per entry into the window. */
    if (sVs.startReq && !bothReady) {
        if (!sVs.deferNoted) {
            sVs.deferNoted = 1u;
            fprintf(stderr,
                    "[online-vehicleselect] start deferred: start requested but "
                    "the rival is not ready (footer: waiting + B backs out)\n");
        }
    } else {
        sVs.deferNoted = 0u;
    }

    /* Re-narrow after input (a cycle may have been applied against a mask that
     * a same-tick config change replaced). */
    vehicleselect_autonarrow(sVs.mask);

    vehicleselect_publish_intent();

    /* Headless test seams: reflect the intent into the scripted room + publish
     * the converged snapshot. Both are inert (and install nothing) in a normal
     * run. Run them BEFORE the render so this frame's render/witness (and the
     * advance check below) reflect the just-converged room. The intent poll is
     * one-shot, so exactly ONE seam may reduce: the dedicated VEHICLESELECT
     * seam owns its lanes; the TRACKSELECT seam's stage pump covers the lanes
     * that armed only MDKR_TEST_ONLINE_TRACKSELECT. */
    if (mdkr_online_vehicleselect_test_active()) {
        vehicleselect_test_reduce_and_script();
    } else {
        mdkr_online_trackselect_test_vehicle_pump();
    }

    /* Re-read so the render/witness/advance reflect the freshest forward feed
     * (post-reduce in the tests; the launcher's live snapshot in a normal run). */
    haveSnap = mdkr_party_link_read(&snap);
    localSeat = haveSnap ? mdkr_online_screen_local_seat(&snap) : -1;

    vehicleselect_resolve_remote(&snap, haveSnap, localSeat, &rv);
    vehicleselect_render(&snap, haveSnap, localSeat, &rv, bothReady);
    vehicleselect_witness(&snap, haveSnap, localSeat, &rv);

    sVs.ticks++;

    /* The authoritative lobby leaving LOBBY (start / loading) wins over a
     * pending leave -- otherwise a stray B would keep this endpoint from booting
     * while the room raced on (charselect parity). */
    if (haveSnap && snap.phase != (uint8_t) MDKR_ONLINE_SCREEN_LOBBY_PHASE) {
        fprintf(stderr,
                "[online-vehicleselect] advance: lobby left LOBBY (phase=%u) -> "
                "hand off\n",
                (unsigned) snap.phase);
        return MDKR_ONLINE_VEHICLESELECT_ADVANCE;
    }
    if (sVs.leave) {
        sVs.leave = 0u; /* edge: return LEAVE once, never shadow ADVANCE */
        fprintf(stderr, "[online-vehicleselect] back to track browse\n");
        return MDKR_ONLINE_VEHICLESELECT_LEAVE;
    }
    return MDKR_ONLINE_VEHICLESELECT_STAY;
}

/* ======================================================================== *
 * Headless test seam (beta + env gated; entirely inert in a normal run)
 *
 * Stands in for the launcher during a headless VEHICLESELECT lane: it ADOPTS the
 * room the CHARSELECT seam converged (the lane runs both), pins a resolved
 * configured_track so the legality mask is non-trivial (Whale Bay,
 * hovercraft-only -- so the cycle's skip-clamp is exercised), then each tick
 * acts as a minimal reducer: converge the local seat to the polled intent's
 * vehicle + ready, hold the scripted remote's ready, and flip to LOADING on the
 * host's OK (the stage owns the start beat now -- retail order). Nothing here
 * runs unless MDKR_TEST_ONLINE_VEHICLESELECT is set.
 * ======================================================================== */
static s8 sVsTestActive = -1; /* -1 unresolved, 0 off, 1 on */
static u8 sVsAdopted;
static u8 sVsStartArmed;
static u8 sVsDeferTripped; /* DEFER: the OK landed -- the scripted remote now
                            * un-readies each tick (deferred/refused START) */
static MdkrPartyLinkSnapshot sVsRoom;

/* Whale Bay (cup 2 round 0): hovercraft-only 0x2 -- the SAME track the combined
 * lane locks + boots, so the legality mask is consistent from the vehicle stage
 * through the race. */
#define VS_TEST_TRACK 8u
#define VS_TEST_REMOTE_VEHICLE 1u /* hovercraft -- legal for Whale Bay */
#define VS_TEST_UNKNOWN_TRACK 900u /* out of range (!= VS_TRACK_NONE): fail-closed probe */

static void vehicleselect_test_resolve(void) {
    if (sVsTestActive < 0) {
        const char *e = getenv("MDKR_TEST_ONLINE_VEHICLESELECT");
        sVsTestActive = (e != NULL) ? 1 : 0;
        /* Scenario from the env VALUE: "diverge" -> divergent-pick lane, "hold" ->
         * the frame-dump hold, "unknown" -> the fail-closed out-of-range probe,
         * anything else -> the default skip-clamp+OK+boot lane. */
        if (e != NULL && strstr(e, "diverge") != NULL) {
            sVsScenario = (s8) VS_SCN_DIVERGE;
        } else if (e != NULL && strstr(e, "hold") != NULL) {
            sVsScenario = (s8) VS_SCN_HOLD;
        } else if (e != NULL && strstr(e, "unknown") != NULL) {
            sVsScenario = (s8) VS_SCN_UNKNOWN;
        } else if (e != NULL && strstr(e, "defer") != NULL) {
            sVsScenario = (s8) VS_SCN_DEFER;
        } else if (e != NULL && strstr(e, "joiner") != NULL) {
            sVsScenario = (s8) VS_SCN_JOINER_HOLD;
        } else {
            sVsScenario = (s8) VS_SCN_SKIP;
        }
    }
}

static void vehicleselect_test_reset(void) {
    vehicleselect_test_resolve();
    if (!sVsTestActive) {
        return;
    }
    sVsAdopted = 0u;
    sVsStartArmed = 0u;
    sVsDeferTripped = 0u;
}

/* Optional dump-seam track override (frame captures of specific mask states,
 * e.g. Frosty Village's 2P no-plane row omission). Test-only. */
static u16 vehicleselect_test_track(void) {
    const char *e = getenv("MDKR_TEST_ONLINE_VEHICLESELECT_TRACK");
    if (e != NULL && e[0] != '\0') {
        return (u16) strtoul(e, NULL, 10);
    }
    return (u16) VS_TEST_TRACK;
}

void mdkr_online_vehicleselect_test_lobby_pump(void) {
    vehicleselect_test_resolve();
    if (!sVsTestActive) {
        return;
    }
    /* The CHARSELECT seam owns install + the LOBBY room in the combined lane; only
     * stand one up ourselves if nothing else has (defensive / standalone use). */
    if (!mdkr_party_link_active()) {
        MdkrPartyLinkSnapshot room;
        unsigned i;
        memset(&room, 0, sizeof(room));
        room.phase = (uint8_t) MDKR_ONLINE_SCREEN_LOBBY_PHASE;
        for (i = 0u; i < MDKR_PARTY_LINK_SEATS; i++) {
            room.seats[i].character_id = MDKR_ONLINE_SCREEN_NO_CHARACTER;
            room.seats[i].vehicle_id = MDKR_ONLINE_SCREEN_NO_VEHICLE;
        }
        room.configured_track = vehicleselect_test_track();
        room.cup_id = 0xFFu;
        room.seats[0].occupied = 1u;
        room.seats[0].is_local = 1u;
        room.seats[0].is_host = 1u;
        room.seats[0].connected = 1u;
        room.seats[0].character_id = 0u;
        room.seats[0].vehicle_id = 0u; /* CHARSELECT default: CAR */
        room.seats[1].occupied = 1u;
        room.seats[1].connected = 1u;
        room.seats[1].character_id = 5u;
        room.seats[1].vehicle_id = (uint8_t) VS_TEST_REMOTE_VEHICLE;
        room.seats[1].ready = 1u;
        memcpy(room.seats[1].name, "RIVAL", sizeof("RIVAL"));
        mdkr_party_link_clear();
        (void) mdkr_party_link_install();
        mdkr_party_link_publish(&room);
        fprintf(stderr,
                "[online-vehicleselect] test-script standalone install "
                "(track=%u)\n",
                (unsigned) room.configured_track);
    }
}

static void vehicleselect_test_reduce_and_script(void) {
    MdkrPartyLinkLocalIntent intent;
    bool haveIntent;

    vehicleselect_test_resolve();
    if (!sVsTestActive || !mdkr_party_link_active()) {
        return;
    }

    if (!sVsAdopted) {
        if (!mdkr_party_link_read(&sVsRoom)) {
            return;
        }
        sVsRoom.mode = MDKR_PARTY_LINK_MODE_SINGLE;
        if (sVsScenario == VS_SCN_DIVERGE) {
            /* No track pinned: all three vehicles legal, so the local seat can
             * pick a vehicle DIFFERENT from the scripted remote (which keeps its
             * CHARSELECT default, CAR) -- both converge over the reverse feed. */
            sVsRoom.configured_track = (uint16_t) VS_TRACK_NONE;
        } else if (sVsScenario == VS_SCN_UNKNOWN) {
            /* Pin an OUT-OF-RANGE resolved track: the mask must fail CLOSED (the
             * engine-truth base -- leveltable itself yields CAR-only for an unknown
             * id -- never the permissive ALL). Guards the fail-closed contract. */
            sVsRoom.configured_track = (uint16_t) VS_TEST_UNKNOWN_TRACK;
        } else {
            /* Pin the resolved track so the legality mask is non-trivial (Whale
             * Bay hovercraft-only by default; the HOLD dump seam may override),
             * whichever seam installed the room, and give the scripted remote a
             * track-legal vehicle for a coherent display. */
            sVsRoom.configured_track = vehicleselect_test_track();
            sVsRoom.seats[1].vehicle_id = (uint8_t) VS_TEST_REMOTE_VEHICLE;
        }
        sVsRoom.seats[1].ready = 1u;
        if (sVsScenario == VS_SCN_JOINER_HOLD) {
            /* dump seam: the REMOTE seat is the host (the joiner's view). */
            sVsRoom.seats[0].is_host = 0u;
            sVsRoom.seats[1].is_host = 1u;
        }
        sVsAdopted = 1u;
    }

    /* Minimal launcher reducer: converge the local seat to the polled intent's
     * character + (mask-legal) vehicle + ready. The screen only ever publishes a
     * legal vehicle, so this simply mirrors it. The stage owns the OK beat now,
     * so the host's start_requested flips the room to LOADING here (the boot
     * proof lane rides the REAL loopback reducer; this mirrors its shape). */
    haveIntent = mdkr_party_link_intent_poll(&intent);
    if (haveIntent) {
        if (intent.confirmed && intent.hover_character < MDKR_ONLINE_SCREEN_CHAR_COUNT) {
            sVsRoom.seats[0].character_id = intent.hover_character;
        }
        if (intent.vehicle_id < MDKR_ONLINE_SCREEN_VEHICLE_COUNT) {
            sVsRoom.seats[0].vehicle_id = intent.vehicle_id;
        }
        if (sVsScenario == VS_SCN_DEFER) {
            /* DEFER: once the host's OK lands, the scripted remote UN-readies
             * (a rival's B / a ready-clear landing after the OK) and STAYS
             * un-ready, so the latched start sits refused and the room never
             * leaves LOBBY -- the deferred-START truthfulness stage. */
            if (intent.start_requested) {
                sVsDeferTripped = 1u;
            }
            sVsRoom.seats[1].ready = sVsDeferTripped ? 0u : 1u;
        } else {
            sVsRoom.seats[1].ready = 1u; /* scripted remote republishes ready */
        }
        if (intent.ready &&
            sVsRoom.seats[0].character_id != MDKR_ONLINE_SCREEN_NO_CHARACTER &&
            sVsRoom.seats[0].vehicle_id != MDKR_ONLINE_SCREEN_NO_VEHICLE) {
            sVsRoom.seats[0].ready = 1u;
        } else if (!intent.ready || intent.backout) {
            sVsRoom.seats[0].ready = 0u;
        }
        if (sVsScenario != VS_SCN_DEFER && intent.start_requested &&
            sVsRoom.seats[0].ready &&
            sVsRoom.seats[1].ready) {
            sVsStartArmed++;
            if (sVsStartArmed >= 4u) {
                sVsRoom.phase = 2u; /* MDKR_ONLINE_LOADING */
            }
        }
    }

    mdkr_party_link_publish(&sVsRoom);
}

u8 mdkr_online_vehicleselect_test_active(void) {
    vehicleselect_test_resolve();
    return (u8) (sVsTestActive > 0 ? 1 : 0);
}

/* the PER-ROUND stage-confirm latch, for the track BROWSE's ready publication
 * (see the sVsStageConfirmedRound comment): true once the LOCAL seat has
 * A-confirmed a legal vehicle on THIS round's stage and has not un-confirmed
 * since. Survives the screen (never reset by _enter); cleared only by the B
 * un-confirm and by round_reset below. */
u8 mdkr_online_vehicleselect_stage_confirmed_round(void) {
    return (u8) (sVsStageConfirmedRound ? 1 : 0);
}

/* ROUND RESET: a new round's selection must re-confirm on the stage before any
 * screen may publish ready again. The session calls this at begin and at every
 * race boot -- the engine-side analog of the reducer's clear_round -- so a
 * post-race re-front (rematch / CANCEL_LOADING unwind) starts un-latched.
 * Idempotent. */
void mdkr_online_vehicleselect_round_reset(void) {
    sVsStageConfirmedRound = 0u;
}

#endif /* MDKR_ENABLE_ONLINE_BETA */
