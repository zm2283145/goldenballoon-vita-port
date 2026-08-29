/* SEPARATED-BOOT-PATH (Strategy D2) native online VEHICLE select.
 *
 * ============================ THE D2 REUSE BOUNDARY ========================
 * The player-facing SCREEN inserted between the native CHARSELECT
 * (online_charselect.c) and the native TRACKSELECT (online_trackselect.c). Before
 * it existed the native flow only auto-narrowed a DEFAULT vehicle to the resolved
 * track's legal mask -- the player never chose car / hovercraft / plane. Strategy
 * D2 means: RE-IMPLEMENT the presentation here using the GAME'S OWN decoded assets
 * (real racer portrait for context, real DKR font + menu SFX, real per-track
 * vehicle mask) rather than calling any offline menu _loop.
 *
 * THE CRUX (why this is a small, plumbing-free screen): the party_link reverse
 * feed ALREADY carries vehicle_id and the reducer ALREADY validates CHOOSE_VEHICLE
 * (any of the three player vehicles) + refuses START (BEGIN_LOADING) with
 * ILLEGAL_VEHICLE when a seat's vehicle is outside the resolved track's mask. So
 * this screen only drives intent.vehicle_id from a player CURSOR instead of the
 * auto-narrow default -- a new screen writing an EXISTING field. No new transport,
 * no new reducer command, no new sync protocol.
 *
 * WHAT IT BORROWS (read-only reuse of already-compiled game code/data; NO edit to
 * menu.c is required -- every symbol below already has external linkage):
 *   - gRacerPortraits[10] / menu_assetgroup_load/free / menu_racer_portraits
 *                                (menu.c) the decoded racer portrait, for context.
 *   - leveltable_vehicle_usable(id) (game.c) the REAL per-track vehicle mask --
 *                                engine truth for the legality gate, zero drift.
 *   - level_name(id)             (game.c) the REAL track name for the resolved
 *                                track line.
 *   - get_player_selected_vehicle(menu.c) the same default seed CHARSELECT applies.
 *   - draw_text / set_text_* / texrect_draw / bgdraw_* / sound_play / SOUND_* /
 *     input_pressed / stick   -- the real font, 2D blit, scrolling sky + SFX + pad.
 *   - mdkr_online_trackselect_cup_track (online_trackselect.c) resolve a tournament
 *     cup's round-0 track from the SAME authoritative table the lane pins -- so the
 *     legality mask for a tournament room is engine truth without a launcher header.
 *
 * WHAT IT OWNS (all state lives HERE, never an offline global): the browse cursor,
 * the committed (always mask-legal) vehicle, the confirm latch, and the continuous
 * reverse-feed intent. The forward feed (both seats + the host's resolved track/cup)
 * is read from platform/net/party_link; the remote seat is display-only.
 *
 * R3 (legality): the PUBLISHED vehicle is ALWAYS inside the resolved track's mask.
 * It is seeded from the CHARSELECT default clamped to a legal bit, auto-narrowed
 * every tick against the resolved track (single -> snapshot configured_track;
 * tournament -> cup round-0 track; none resolved yet -> all three legal), and only
 * ever changed to another LEGAL vehicle on confirm. An A press on an ILLEGAL slot
 * is REJECTED (buzz, no change). So a seat can never READY / START with an illegal
 * vehicle even before TRACKSELECT locks the track -- TRACKSELECT's own auto-narrow
 * remains the final clamp if the host later locks a track that outlaws the pick.
 *
 * The ENTIRE TU is #if MDKR_ENABLE_ONLINE_BETA and it is added to the build only
 * inside the beta CMake gate (game/src/online/ is not globbed), so a normal (beta
 * OFF) build never compiles a byte of it and the release engine object is untouched.
 * ==========================================================================
 */
#include "online/online_vehicleselect.h"

#if MDKR_ENABLE_ONLINE_BETA

/* Game/PR headers FIRST (same sprintf-ordering rationale as online_charselect.c). */
#include "types.h"
#include "thread3_main.h"
#include "enums.h"      /* VEHICLE_CAR / HOVERCRAFT / PLANE, AlignmentFlags */
#include "game.h"       /* level_name, leveltable_vehicle_usable */
#include "menu.h"       /* gRacerPortraits, menu_assetgroup_load/free,
                           menu_racer_portraits, get_player_selected_vehicle,
                           TEXTURE_ICON_PORTRAIT_*, font.h (draw_text, ...) */
#include "rcp_dkr.h"    /* texrect_draw, bgdraw_fillcolour */
#include "audio.h"      /* sound_play */
#include "sound_ids.h"  /* SOUND_MENU_PICK2 / SOUND_SELECT2 / ... */
#include "joypad.h"     /* input_pressed, input_clamp_stick_x/y */
#include "PR/os_cont.h" /* A_BUTTON / B_BUTTON / *_JPAD / START_BUTTON */
#include "net/party_link.h"
#include "online/online_trackselect.h" /* cup_track resolver (engine-truth table) */
#include "online/online_portraits.h" /* sOnlineToPortrait / sOnlineNames /
                                        sPortraitAssetIds (DRY with the other screens) */
#include "online/online_screen_util.h" /* shared local_seat / text / pulse /
                                          draw_portrait + scrolling-sky backdrop */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Screen space (mirrored, like online_charselect.c). */
#define VS_SCREEN_W 320
#define VS_SCREEN_W_HALF 160

/* ---- Local mirrors of the launcher lobby's id space (no launcher headers) --- */
#define VS_CHAR_COUNT 10u            /* MDKR_ONLINE_CHARACTER_COUNT */
#define VS_NO_CHARACTER 0xFFu        /* MDKR_ONLINE_NO_CHARACTER */
#define VS_NO_VEHICLE 0xFFu          /* MDKR_ONLINE_NO_VEHICLE */
#define VS_PLAYER_VEHICLE_COUNT 3u   /* car / hovercraft / plane (0x07 mask) */
#define VS_ALL_VEHICLES 0x07u        /* MDKR_ONLINE_PLAYER_VEHICLE_MASK */
#define VS_CUP_COUNT 5u              /* 5 cups (== 5 worlds) */
#define VS_LOBBY_PHASE 1u            /* MDKR_ONLINE_LOBBY */
#define VS_MODE_TOURNAMENT 1u        /* MDKR_ONLINE_MODE_TOURNAMENT */
#define VS_TRACK_NONE 0xFFFFu        /* configured_track "none" sentinel */
#define VS_LOCAL_PAD 0               /* PLAYER_ONE */

/* Retail 2-player narrowing (mirrors online_trackselect.c: menu_track_select V79+
 * -- engine truth for the two tracks whose usable set shrinks at 2 players). */
#define VS_TRACK_SPACEPORT_ALPHA 15u
#define VS_TRACK_FROSTY_VILLAGE 28u

/* Menu SFX (the real DKR enums, same reuse as the other native screens). */
#define VS_SFX_MOVE SOUND_MENU_PICK2
#define VS_SFX_CONFIRM SOUND_SELECT2
#define VS_SFX_BACK SOUND_MENU_BACK3
#define VS_SFX_REJECT SOUND_ELECTRIC_BUZZ

#define VS_REJECT_FLASH_TICKS 45u /* "NOT ON THIS TRACK" flash (~1.5s @ 30Hz) */

/* ---- Layout geometry (320x240) -------------------------------------------- */
#define VS_TITLE_Y 18
#define VS_PORTRAIT_X (VS_SCREEN_W_HALF - 22) /* centered ~44px portrait */
#define VS_PORTRAIT_Y 34
#define VS_CHARNAME_Y 82
#define VS_ART_Y 98            /* top edge of the real car/hover/plane art (T9) */
#define VS_CARD_Y 150          /* vehicle name row (caption below the art) */
#define VS_CARD_STATE_Y 164    /* per-vehicle state label */
#define VS_TRACK_Y 182
#define VS_STATUS_Y 204        /* YOU / rival pair (charselect parity) */
#define VS_HELP_Y 226
/* Three vehicle cards centered across the width. */
#define VS_CARD_X0 64
#define VS_CARD_DX 96

/* Vehicle names + a per-vehicle accent colour (display chrome only). */
static const char *const sVehicleNames[VS_PLAYER_VEHICLE_COUNT] = {
    "CAR", "HOVERCRAFT", "PLANE",
};
static const u8 sVehicleAccent[VS_PLAYER_VEHICLE_COUNT][3] = {
    {230u, 110u, 110u}, /* CAR       -- warm red */
    {110u, 190u, 230u}, /* HOVERCRAFT-- cool blue */
    {235u, 205u, 110u}, /* PLANE     -- gold */
};

/* T9 NIT-2: the real vehicle art tile group (three vehicles x TOP+BOTTOM) + the
 * -1 terminator menu_assetgroup_load/free stop on. Borrowed READ-ONLY the same way
 * sPortraitAssetIds borrows the racer faces -- menu_asset_load routes each texture
 * id to load_texture, so this loads the six vehicle tiles into gMenuAssets[] and
 * spawns NO menu objects. NON-const because the loader takes s16* and this TU owns
 * its own copy (the sPortraitAssetIds discipline). Defined HERE (not the shared
 * header) because only the vehicle screen uses it. */
static s16 sOnlineVehicleAssetIds[] = {
    TEXTURE_ICON_VEHICLE_CAR_TOP,        TEXTURE_ICON_VEHICLE_CAR_BOTTOM,
    TEXTURE_ICON_VEHICLE_HOVERCRAFT_TOP, TEXTURE_ICON_VEHICLE_HOVERCRAFT_BOTTOM,
    TEXTURE_ICON_VEHICLE_PLANE_TOP,      TEXTURE_ICON_VEHICLE_PLANE_BOTTOM,
    -1,
};

/* ---- Session-owned screen state (never an offline global) ------------------ */
typedef struct MdkrOnlineVehicleselectState {
    u8 cursor;         /* browse position 0..2 (may sit on an illegal slot) */
    u8 vehicle;        /* COMMITTED, always mask-legal, published every frame */
    u8 confirmed;      /* local player confirmed a legal vehicle (advance latch) */
    u8 character;      /* local seat's chosen racer (for the portrait), or NONE */
    u8 mask;           /* the resolved track's legal-vehicle mask this frame */
    u16 track;         /* the resolved track id, or VS_TRACK_NONE */
    u8 assets;         /* portraits + sky + fonts loaded */
    u8 leave;          /* B: back-to-charselect request (edge; see tick) */
    u8 seeded;         /* first-snapshot seed applied */
    u32 ticks;         /* VEHICLESELECT ticks elapsed (also drives test input) */
    u32 rejectFlashEnd;/* "NOT ON THIS TRACK" flash deadline, in ticks */
    s8 stickLatchX;
} MdkrOnlineVehicleselectState;

static MdkrOnlineVehicleselectState sVs;

/* persists ACROSS entries (NOT reset by _enter's memset) so a re-entered screen
 * restarts on the vehicle you last committed. */
static u8 sLastVehicle = (u8) VEHICLE_CAR;

/* witness change-detect (file scope so _enter() resets it for a clean re-entry). */
static u32 sWitnessKey = 0xFFFFFFFFu;
static s32 sWitnessRemoteSeat = -2;

/* Resolved (display-only) view of the remote seat (bounded, NUL-forced name). */
typedef struct VsRemoteView {
    s8 seat;
    u8 present;
    u8 vehicle;   /* VS_NO_VEHICLE when none */
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
 *   single race -> the host's configured_track once locked;
 *   nothing locked yet (the common first-race case) -> VS_TRACK_NONE (no
 *   constraint: all three vehicles are pickable, and TRACKSELECT's own auto-narrow
 *   is the final clamp when the host later locks a track). */
static u16 vehicleselect_resolve_track(const MdkrPartyLinkSnapshot *snap,
                                       bool haveSnap) {
    if (!haveSnap) {
        return VS_TRACK_NONE;
    }
    if (snap->mode == VS_MODE_TOURNAMENT) {
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
 * truth from leveltable_vehicle_usable(), then the retail 2-player narrowing.
 * VS_TRACK_NONE (nothing locked) is PERMISSIVE (all three) -- the pick is a
 * preference until TRACKSELECT locks a track, and the reducer only enforces the
 * mask at START. Never returns an empty mask. */
static u8 vehicleselect_track_mask(u16 trackId, unsigned occupied) {
    u8 mask;
    if (trackId == VS_TRACK_NONE) {
        return VS_ALL_VEHICLES;
    }
    mask = (u8) leveltable_vehicle_usable((s32) trackId);
    mask &= (u8) VS_ALL_VEHICLES;
    if (mask == 0u) {
        return VS_ALL_VEHICLES; /* unknown id -> permissive, not car-only */
    }
    if (occupied >= 2u) {
        if (trackId == VS_TRACK_SPACEPORT_ALPHA) {
            mask &= (u8) ~(1u << VEHICLE_HOVERCRAFT);
        }
        if (trackId == VS_TRACK_FROSTY_VILLAGE) {
            mask &= (u8) ~(1u << VEHICLE_PLANE);
        }
    }
    if ((mask & VS_ALL_VEHICLES) == 0u) {
        mask = (u8) (1u << VEHICLE_CAR); /* fail-safe: never empty */
    }
    return (u8) (mask & VS_ALL_VEHICLES);
}

static bool vehicleselect_vehicle_legal(u8 vehicle, u8 mask) {
    return vehicle < VS_PLAYER_VEHICLE_COUNT &&
           (mask & (u8) (1u << vehicle)) != 0u;
}

/* Clamp the COMMITTED vehicle into the mask (lowest legal bit when illegal). This
 * is the R3 guarantee: the published vehicle is always legal, so the seat can
 * never READY / START with an illegal vehicle. */
static void vehicleselect_autonarrow(u8 mask) {
    u8 v;
    if (vehicleselect_vehicle_legal(sVs.vehicle, mask)) {
        return;
    }
    for (v = 0u; v < VS_PLAYER_VEHICLE_COUNT; v++) {
        if (mask & (u8) (1u << v)) {
            sVs.vehicle = v;
            return;
        }
    }
    sVs.vehicle = (u8) VEHICLE_CAR; /* mask never empty, but stay defined */
}

static void vehicleselect_resolve_remote(const MdkrPartyLinkSnapshot *snap,
                                         bool haveSnap, s32 localSeat,
                                         VsRemoteView *out) {
    unsigned i;
    memset(out, 0, sizeof(*out));
    out->seat = -1;
    out->vehicle = VS_NO_VEHICLE;
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
        if (seat->vehicle_id < VS_PLAYER_VEHICLE_COUNT) {
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
    s8 dx;    /* -1 / 0 / +1 column step (edge) */
    u8 aEdge; /* A: confirm the hovered vehicle */
    u8 bEdge; /* B: back to charselect */
} VsInput;

/* Headless scenario, resolved from the env VALUE (see vehicleselect_test_resolve):
 *   REJECT (default) -- the combined lane pins Whale Bay (hovercraft-only 0x2), so
 *     the cursor seeds on the only legal slot (hovercraft); move LEFT to CAR and
 *     press A to prove the ILLEGAL pick is REJECTED (buzz, no change), then move
 *     back and confirm. Hands off to TRACKSELECT (which boots the live race).
 *   DIVERGE -- no track pinned (all three legal); the cursor seeds on CAR (the
 *     CHARSELECT default), moves RIGHT to PLANE and confirms, so the local seat
 *     converges to a vehicle DIFFERENT from the scripted remote's (car) -- the
 *     two-endpoint per-seat vehicle divergence + convergence proof (no boot). */
#define VS_SCN_REJECT 0
#define VS_SCN_DIVERGE 1
#define VS_SCN_HOLD 2 /* frame-dump only: park the screen so a shot can be taken */
static s8 sVsScenario = -1;

static void vehicleselect_input_scripted(VsInput *in) {
    memset(in, 0, sizeof(*in));
    if (sVsScenario == VS_SCN_HOLD) {
        /* Dump seam: rest the cursor on CAR (illegal on the pinned hovercraft-only
         * track) at tick 2 and never confirm/leave, so a single frame dump shows
         * ALL card states at once: cursor-gold on the illegal CAR ("N/A"), the
         * committed HOVERCRAFT ("SET", green), and the illegal PLANE ("N/A"). */
        if (sVs.ticks == 2u) {
            in->dx = -1;
        }
        return;
    }
    if (sVsScenario == VS_SCN_DIVERGE) {
        switch (sVs.ticks) {
        case 2u:
            in->dx = 1; /* CAR -> HOVERCRAFT */
            break;
        case 3u:
            in->dx = 1; /* HOVERCRAFT -> PLANE (id 2) */
            break;
        case 6u:
            in->aEdge = 1u; /* confirm PLANE (legal; differs from remote CAR) */
            break;
        default:
            break;
        }
        return;
    }
    switch (sVs.ticks) {
    case 2u:
        in->dx = -1; /* toward CAR (id 0) */
        break;
    case 3u:
        in->aEdge = 1u; /* A on the illegal slot -> REJECT */
        break;
    case 5u:
        in->dx = 1; /* back toward the legal slot */
        break;
    case 7u:
        in->aEdge = 1u; /* confirm a legal vehicle -> advance */
        break;
    default:
        break;
    }
}

/* Live pad: L/R edges plus a latched analog stick, local player only. */
static void vehicleselect_input_live(VsInput *in) {
    u32 pressed = input_pressed(VS_LOCAL_PAD);
    s32 sx = input_clamp_stick_x(VS_LOCAL_PAD);
    s8 wantX = 0;

    memset(in, 0, sizeof(*in));

    if (pressed & R_JPAD) {
        in->dx = 1;
    } else if (pressed & L_JPAD) {
        in->dx = -1;
    }
    if (sx > 40) {
        wantX = 1;
    } else if (sx < -40) {
        wantX = -1;
    }
    if (in->dx == 0 && wantX != 0 && sVs.stickLatchX == 0) {
        in->dx = wantX;
    }
    sVs.stickLatchX = wantX;

    in->aEdge = (pressed & (A_BUTTON | START_BUTTON)) ? 1u : 0u;
    in->bEdge = (pressed & B_BUTTON) ? 1u : 0u;
}

/* minimal scripted input for every OTHER headless lane that drives the native flow
 * through this screen but does NOT arm the dedicated VEHICLESELECT choreography
 * seam: the LOBBY-START / LOBBY-TOURNAMENT loopback lanes (which ride a REAL
 * launcher reducer) and the self-contained CHARSELECT / TRACKSELECT screen lanes
 * (whose own reducer converged the room through CHARSELECT). It stays trivial: the
 * cursor seeds on the committed, always-mask-legal vehicle, so a single A confirms
 * it and the session hands VEHICLESELECT -> TRACKSELECT. It never moves the cursor
 * -- the auto-narrow keeps the committed vehicle legal for the resolved track, so
 * the confirm is never rejected. Inert (unresolved -> off) in a run that arms none
 * of these seams, so live play uses the real pad. Resolved once. */
static s8 sVsScriptedConfirm = -1; /* -1 unresolved, 0 off, 1 on */
static u8 vehicleselect_scripted_confirm_active(void) {
    if (sVsScriptedConfirm < 0) {
        sVsScriptedConfirm = (getenv("MDKR_TEST_ONLINE_LOBBY_START") != NULL ||
                              getenv("MDKR_TEST_ONLINE_LOBBY_TOURNAMENT") != NULL ||
                              getenv("MDKR_TEST_ONLINE_CHARSELECT") != NULL ||
                              getenv("MDKR_TEST_ONLINE_TRACKSELECT") != NULL)
                                 ? 1
                                 : 0;
    }
    return (u8) (sVsScriptedConfirm > 0 ? 1 : 0);
}

static void vehicleselect_input_scripted_confirm(VsInput *in) {
    memset(in, 0, sizeof(*in));
    if (sVs.ticks == 2u) {
        in->aEdge = 1u; /* confirm the seeded (mask-legal) vehicle -> advance */
    }
}

static void vehicleselect_gather_input(VsInput *in) {
    if (mdkr_online_vehicleselect_test_active()) {
        vehicleselect_input_scripted(in);
    } else if (vehicleselect_scripted_confirm_active()) {
        vehicleselect_input_scripted_confirm(in);
    } else {
        vehicleselect_input_live(in);
    }
}

/* Apply one input step. The cursor wraps across the three slots (native DKR 2D
 * menus wrap); A on a LEGAL slot commits it + latches confirm; A on an ILLEGAL
 * slot is REJECTED (buzz, flash, no change). B backs out one level. */
static void vehicleselect_apply_input(const VsInput *in) {
    if (in->bEdge) {
        sVs.leave = 1u;
        sound_play(VS_SFX_BACK, NULL);
        return;
    }
    if (in->dx != 0) {
        s32 col = (s32) sVs.cursor + in->dx;
        col = (col + (s32) VS_PLAYER_VEHICLE_COUNT) % (s32) VS_PLAYER_VEHICLE_COUNT;
        if ((u8) col != sVs.cursor) {
            sVs.cursor = (u8) col;
            sound_play(VS_SFX_MOVE, NULL);
            sVs.rejectFlashEnd = 0u; /* leaving the slot ends the reject flash */
        }
    }
    if (in->aEdge) {
        if (vehicleselect_vehicle_legal(sVs.cursor, sVs.mask)) {
            sVs.vehicle = sVs.cursor;
            sLastVehicle = sVs.vehicle; /* persistence */
            sVs.confirmed = 1u;
            sound_play(VS_SFX_CONFIRM, NULL);
        } else {
            /* R3: never commit an illegal vehicle; flash + negative cue. */
            sVs.rejectFlashEnd = sVs.ticks + VS_REJECT_FLASH_TICKS;
            sound_play(VS_SFX_REJECT, NULL);
            fprintf(stderr,
                    "[online-vehicleselect] reject vehicle=%u (illegal for "
                    "track=%u mask=0x%x)\n",
                    (unsigned) sVs.cursor, (unsigned) sVs.track,
                    (unsigned) sVs.mask);
        }
    }
}

/* Publish the FULL local intent every frame (continuous republish: the reducer
 * clears ready on any selection change, so republishing reconverges within a
 * pump). The COMMITTED vehicle is always mask-legal (R3). The character is carried
 * from the local seat snapshot so CHOOSE_CHARACTER stays converged; ready is held
 * (the player readied on CHARSELECT and refines the vehicle here). start_requested
 * stays 0 -- host-start belongs to TRACKSELECT. */
static void vehicleselect_publish_intent(void) {
    MdkrPartyLinkLocalIntent intent;
    mdkr_party_link_intent_init(&intent);
    if (sVs.character < VS_CHAR_COUNT) {
        intent.hover_character = sVs.character;
        intent.confirmed = 1u;
    }
    intent.vehicle_id = sVs.vehicle;
    intent.ready = 1u;
    intent.backout = 0u;
    intent.start_requested = 0u;
    mdkr_party_link_intent_publish(&intent);
}

/* ======================================================================== *
 * Render (native: real portrait + real font, into the engine frame list)
 * ======================================================================== */
static void vehicleselect_render(const VsRemoteView *rv) {
    const char *rname = rv->name[0] != '\0' ? rv->name : "RIVAL";
    s32 tri = mdkr_online_screen_pulse(sVs.ticks);
    u8 v;

    /* Title. */
    mdkr_online_screen_text(VS_SCREEN_W_HALF, VS_TITLE_Y, ASSET_FONTS_BIGFONT,
                            "CHOOSE YOUR VEHICLE", ALIGN_MIDDLE_CENTER, 255, 224,
                            96);

    /* The chosen racer's portrait for context (borrowed, guarded). */
    if (sVs.character < VS_CHAR_COUNT) {
        mdkr_online_screen_draw_portrait(sVs.character, VS_PORTRAIT_X,
                                         VS_PORTRAIT_Y, 220u, 220u, 220u);
        mdkr_online_screen_text(VS_SCREEN_W_HALF, VS_CHARNAME_Y,
                                ASSET_FONTS_SMALLFONT,
                                (char *) sOnlineNames[sVs.character],
                                ALIGN_MIDDLE_CENTER, 200, 200, 200);
    }

    /* The three vehicle cards. Colour + shape carry the state so a colourblind
     * player still reads it: cursor = pulsing gold >NAME<, committed = green,
     * illegal = big luminance drop + "N/A". */
    for (v = 0u; v < VS_PLAYER_VEHICLE_COUNT; v++) {
        s32 x = VS_CARD_X0 + (s32) v * VS_CARD_DX;
        bool legal = vehicleselect_vehicle_legal(v, sVs.mask);
        bool onCursor = (v == sVs.cursor);
        bool committed = (v == sVs.vehicle);
        s32 r = sVehicleAccent[v][0];
        s32 g = sVehicleAccent[v][1];
        s32 b = sVehicleAccent[v][2];
        char label[24];
        const char *state;

        /* T9 NIT-2: the REAL vehicle picture per card (like the charselect grid's
         * real portraits) -- full colour when legal, ghosted (dim + half alpha,
         * the offline race-select's own "not available" treatment) when not. Falls
         * back to the text caption below when the tiles are not resident. */
        if (legal) {
            mdkr_online_screen_draw_vehicle(v, x, VS_ART_Y, 255u, 255u, 255u, 255u);
        } else {
            mdkr_online_screen_draw_vehicle(v, x, VS_ART_Y, 150u, 150u, 150u, 128u);
        }

        if (!legal) {
            r = 96;
            g = 96;
            b = 96;
        }
        if (committed && legal) {
            r = 120;
            g = 255;
            b = 120;
        }
        if (onCursor) {
            if (legal && !committed) {
                r = 255;
                g = 190 + tri * 4;
                b = 60 + tri * 3;
            }
            (void) snprintf(label, sizeof(label), ">%s<", sVehicleNames[v]);
        } else {
            (void) snprintf(label, sizeof(label), "%s", sVehicleNames[v]);
        }
        mdkr_online_screen_text(x, VS_CARD_Y, ASSET_FONTS_SMALLFONT, label,
                                ALIGN_MIDDLE_CENTER, r, g, b);

        if (!legal) {
            state = "N/A";
        } else if (committed) {
            state = "SET";
        } else {
            state = "";
        }
        if (state[0] != '\0') {
            mdkr_online_screen_text(x, VS_CARD_STATE_Y, ASSET_FONTS_SMALLFONT,
                                    (char *) state, ALIGN_MIDDLE_CENTER,
                                    committed ? 120 : 200, committed ? 255 : 120,
                                    committed ? 120 : 120);
        }
    }

    /* The resolved track (or ANY when nothing is locked yet). */
    {
        char line[48];
        if (sVs.track != VS_TRACK_NONE) {
            (void) snprintf(line, sizeof(line), "TRACK: %s",
                            level_name((s32) sVs.track));
        } else {
            (void) snprintf(line, sizeof(line), "TRACK: ANY (CHOOSE NEXT)");
        }
        mdkr_online_screen_text(VS_SCREEN_W_HALF, VS_TRACK_Y,
                                ASSET_FONTS_SMALLFONT, line, ALIGN_MIDDLE_CENTER,
                                190, 190, 210);
    }

    /* Status lines (charselect parity): local committed vehicle + the rival. */
    {
        char line[64];
        (void) snprintf(line, sizeof(line), "YOU: %s",
                        sVs.vehicle < VS_PLAYER_VEHICLE_COUNT
                            ? sVehicleNames[sVs.vehicle]
                            : "-");
        mdkr_online_screen_text(24, VS_STATUS_Y, ASSET_FONTS_SMALLFONT, line,
                                ALIGN_MIDDLE_LEFT, 120, 255, 120);
        if (!rv->present) {
            mdkr_online_screen_text(VS_SCREEN_W - 24, VS_STATUS_Y,
                                    ASSET_FONTS_SMALLFONT,
                                    "WAITING FOR PLAYER...", ALIGN_MIDDLE_RIGHT,
                                    150, 150, 150);
        } else {
            (void) snprintf(line, sizeof(line), "%.12s: %s", rname,
                            rv->vehicle < VS_PLAYER_VEHICLE_COUNT
                                ? sVehicleNames[rv->vehicle]
                                : "CHOOSING");
            mdkr_online_screen_text(VS_SCREEN_W - 24, VS_STATUS_Y,
                                    ASSET_FONTS_SMALLFONT, line,
                                    ALIGN_MIDDLE_RIGHT,
                                    rv->ready ? 120 : 220, rv->ready ? 255 : 220,
                                    rv->ready ? 120 : 220);
        }
    }

    /* Context help / transient reject flash. */
    if (sVs.ticks < sVs.rejectFlashEnd) {
        mdkr_online_screen_text(VS_SCREEN_W_HALF, VS_HELP_Y, ASSET_FONTS_SMALLFONT,
                                "NOT ALLOWED ON THIS TRACK", ALIGN_MIDDLE_CENTER,
                                255, 80, 80);
    } else {
        mdkr_online_screen_text(VS_SCREEN_W_HALF, VS_HELP_Y, ASSET_FONTS_SMALLFONT,
                                "A: SELECT   B: BACK", ALIGN_MIDDLE_CENTER, 255,
                                255, 255);
    }
}

/* Bounded stderr witness: one line only when the visible state changes. Folds the
 * local seat's converged vehicle/ready AND the remote's vehicle/ready so a
 * remote-only change still emits a row (the "both converge" proof). */
static void vehicleselect_witness(const MdkrPartyLinkSnapshot *snap, bool haveSnap,
                                  s32 localSeat, const VsRemoteView *rv) {
    u8 localSeatVeh = VS_NO_VEHICLE;
    u8 localSeatReady = 0u;
    u8 remoteNibble;
    u32 key;

    if (haveSnap && localSeat >= 0) {
        localSeatVeh = snap->seats[localSeat].vehicle_id;
        localSeatReady = snap->seats[localSeat].ready;
    }
    remoteNibble = (rv->vehicle < VS_PLAYER_VEHICLE_COUNT) ? rv->vehicle : 0xFu;

    key = ((u32) sVs.cursor) | ((u32) sVs.vehicle << 2) |
          ((u32) sVs.confirmed << 4) | ((u32) sVs.mask << 5) |
          ((u32) remoteNibble << 9) | ((u32) (rv->ready ? 1u : 0u) << 13) |
          ((u32) rv->present << 14) |
          ((u32) ((localSeatVeh < VS_PLAYER_VEHICLE_COUNT) ? localSeatVeh : 7u)
           << 15) |
          ((u32) localSeatReady << 18) | ((u32) (sVs.track & 0x3Fu) << 19);
    if (key == sWitnessKey && rv->seat == sWitnessRemoteSeat) {
        return;
    }
    sWitnessKey = key;
    sWitnessRemoteSeat = rv->seat;

    fprintf(stderr,
            "[online-vehicleselect] render cursor=%u vehicle=%u legal=0x%x "
            "track=%u local{seatVeh=%u seatReady=%u conf=%u} "
            "remote{seat=%d veh=%u ready=%u name=%.*s} "
            "intent{vehicle=%u ready=1}\n",
            (unsigned) sVs.cursor, (unsigned) sVs.vehicle, (unsigned) sVs.mask,
            (unsigned) sVs.track, (unsigned) localSeatVeh,
            (unsigned) localSeatReady, (unsigned) sVs.confirmed, (int) rv->seat,
            (unsigned) rv->vehicle, (unsigned) rv->ready,
            (int) MDKR_PARTY_LINK_NAME_BYTES,
            rv->name[0] != '\0' ? rv->name : "-", (unsigned) sVs.vehicle);
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
    sVs.character = VS_NO_CHARACTER;
    sVs.track = VS_TRACK_NONE;
    sVs.mask = VS_ALL_VEHICLES;

    /* reset the witness change-detect + the headless seam so a second entry
     * is clean (a re-entered screen re-scripts from scratch). */
    sWitnessKey = 0xFFFFFFFFu;
    sWitnessRemoteSeat = -2;
    vehicleselect_test_reset();

    /* Seed the committed vehicle from the CHARSELECT default (same source), or the
     * last committed one, then clamp to the resolved track's mask (R3). */
    defaultVehicle = get_player_selected_vehicle(VS_LOCAL_PAD);
    if (defaultVehicle < 0 || (u8) defaultVehicle >= VS_PLAYER_VEHICLE_COUNT) {
        defaultVehicle = (s8) VEHICLE_CAR;
    }
    sVs.vehicle = (sLastVehicle < VS_PLAYER_VEHICLE_COUNT) ? sLastVehicle
                                                           : (u8) defaultVehicle;

    /* Read the first snapshot so the initial mask/character/committed vehicle are
     * resolved before the first render (the local seat carries the CHARSELECT
     * pick + the host's resolved track/cup if any). */
    haveSnap = mdkr_party_link_read(&snap);
    localSeat = haveSnap ? mdkr_online_screen_local_seat(&snap) : -1;
    occupied = vehicleselect_occupied_seats(&snap, haveSnap);
    if (haveSnap && localSeat >= 0) {
        if (snap.seats[localSeat].character_id < VS_CHAR_COUNT) {
            sVs.character = snap.seats[localSeat].character_id;
        }
        if (snap.seats[localSeat].vehicle_id < VS_PLAYER_VEHICLE_COUNT) {
            sVs.vehicle = snap.seats[localSeat].vehicle_id;
        }
    }
    sVs.track = vehicleselect_resolve_track(&snap, haveSnap);
    sVs.mask = vehicleselect_track_mask(sVs.track, occupied);
    vehicleselect_autonarrow(sVs.mask);
    sVs.cursor = sVs.vehicle; /* start the cursor on the committed legal vehicle */
    sVs.seeded = 1u;

    /* Borrow the real portraits (context) + the shared scrolling-sky group, the
     * same two-call asset borrow the other native screens make. */
    menu_assetgroup_load(sPortraitAssetIds);
    menu_racer_portraits();
    menu_assetgroup_load(sOnlineSkyAssetIds);
    /* T9 NIT-2: the real car/hovercraft/plane art (read-only borrow, freed in
     * _exit before the group is released -- balanced with the portrait/sky loads). */
    menu_assetgroup_load(sOnlineVehicleAssetIds);

    load_font(ASSET_FONTS_BIGFONT);
    load_font(ASSET_FONTS_SMALLFONT);

    sVs.assets = 1u;
    /* Neutral hub sky (Dino Domain) -- the vehicle screen is world-agnostic. */
    mdkr_online_screen_backdrop((u8) MDKR_ONLINE_SKY_WORLD_NEUTRAL);

    /* T7b: reveal from black (retail fade cadence) + keep the retail menu music
     * (isolation-safe primitive borrows -- see online_screen_util.h). */
    mdkr_online_screen_fade_in_from_black();
    mdkr_online_screen_menu_music();

    fprintf(stderr,
            "[online-vehicleselect] enter: native screen up character=%u "
            "vehicle=%u track=%u mask=0x%x (portraits loaded, offline _loop "
            "bypassed)\n",
            (unsigned) sVs.character, (unsigned) sVs.vehicle,
            (unsigned) sVs.track, (unsigned) sVs.mask);
}

void mdkr_online_vehicleselect_exit(void) {
    if (sVs.assets) {
        /* Disarm the borrowed sky BEFORE freeing its tiles (bgdraw_render
         * lifetime, R6), then balance the loads _enter() took. */
        mdkr_online_screen_backdrop_clear();
        unload_font(ASSET_FONTS_SMALLFONT);
        unload_font(ASSET_FONTS_BIGFONT);
        menu_assetgroup_free(sPortraitAssetIds);
        menu_assetgroup_free(sOnlineSkyAssetIds);
        menu_assetgroup_free(sOnlineVehicleAssetIds);
        sVs.assets = 0u;
        fprintf(stderr,
                "[online-vehicleselect] exit: freed portrait + vehicle assets\n");
    }
}

MdkrOnlineVehicleselectResult mdkr_online_vehicleselect_tick(s32 updateRate) {
    MdkrPartyLinkSnapshot snap;
    bool haveSnap;
    s32 localSeat;
    unsigned occupied;
    VsRemoteView rv;
    VsInput in;

    (void) updateRate;

    haveSnap = mdkr_party_link_read(&snap);
    localSeat = haveSnap ? mdkr_online_screen_local_seat(&snap) : -1;
    occupied = vehicleselect_occupied_seats(&snap, haveSnap);

    /* Track the local seat's character (portrait) + the resolved-track mask each
     * frame; auto-narrow the committed vehicle into the mask (R3) BEFORE input so
     * the cursor's legality reads this frame's mask. */
    if (haveSnap && localSeat >= 0 &&
        snap.seats[localSeat].character_id < VS_CHAR_COUNT) {
        sVs.character = snap.seats[localSeat].character_id;
    }
    sVs.track = vehicleselect_resolve_track(&snap, haveSnap);
    sVs.mask = vehicleselect_track_mask(sVs.track, occupied);
    vehicleselect_autonarrow(sVs.mask);

    vehicleselect_gather_input(&in);
    vehicleselect_apply_input(&in);

    /* Re-narrow after input (a confirm may have moved the committed vehicle). */
    vehicleselect_autonarrow(sVs.mask);

    vehicleselect_publish_intent();

    /* Headless test seam: reflect the intent into the scripted room + publish the
     * converged snapshot. Inert (and installs nothing) in a normal run. Run it
     * BEFORE the render so this frame's render/witness (and the advance check
     * below) reflect the just-converged room -- otherwise a confirm that both
     * converges the seat AND triggers the session's immediate hand-off would never
     * emit a render row showing the converged seat vehicle. */
    vehicleselect_test_reduce_and_script();

    /* Re-read so the render/witness/advance reflect the freshest forward feed
     * (post-reduce in the test; the launcher's live snapshot in a normal run). */
    haveSnap = mdkr_party_link_read(&snap);
    localSeat = haveSnap ? mdkr_online_screen_local_seat(&snap) : -1;

    vehicleselect_resolve_remote(&snap, haveSnap, localSeat, &rv);
    vehicleselect_render(&rv);
    vehicleselect_witness(&snap, haveSnap, localSeat, &rv);

    sVs.ticks++;

    /* The authoritative lobby leaving LOBBY (host started / loading) wins over a
     * pending leave -- otherwise a stray B would keep this endpoint from booting
     * while the room raced on (charselect parity). */
    if (haveSnap && snap.phase != (uint8_t) VS_LOBBY_PHASE) {
        fprintf(stderr,
                "[online-vehicleselect] advance: lobby left LOBBY (phase=%u) -> "
                "hand off\n",
                (unsigned) snap.phase);
        return MDKR_ONLINE_VEHICLESELECT_ADVANCE;
    }
    if (sVs.leave) {
        sVs.leave = 0u; /* edge: return LEAVE once, never shadow ADVANCE */
        fprintf(stderr, "[online-vehicleselect] back to charselect\n");
        return MDKR_ONLINE_VEHICLESELECT_LEAVE;
    }
    return MDKR_ONLINE_VEHICLESELECT_STAY;
}

/* ======================================================================== *
 * Headless test seam (beta + env gated; entirely inert in a normal run)
 *
 * Stands in for the launcher during a headless VEHICLESELECT lane: it ADOPTS the
 * room the CHARSELECT seam converged (the lane runs both), pins a resolved
 * configured_track so the legality mask is non-trivial (Whale Bay, hovercraft-only
 * -- so CAR/PLANE are illegal and the reject path is exercised), then each tick
 * acts as a minimal reducer: converge the local seat to the polled intent's
 * vehicle (only accepting a mask-legal value) + ready, and hold the scripted
 * remote seat's ready. It NEVER self-starts: the session hands VEHICLESELECT ->
 * TRACKSELECT on the confirm, and the TRACKSELECT seam owns the host-start. Nothing
 * here runs unless MDKR_TEST_ONLINE_VEHICLESELECT is set.
 * ======================================================================== */
static s8 sVsTestActive = -1; /* -1 unresolved, 0 off, 1 on */
static u8 sVsAdopted;
static MdkrPartyLinkSnapshot sVsRoom;

/* Whale Bay (cup 2 round 0): hovercraft-only 0x2 -- the SAME track the combined
 * lane's TRACKSELECT seam locks + boots, so the legality mask is consistent from
 * the vehicle screen through the race. */
#define VS_TEST_TRACK 8u
#define VS_TEST_REMOTE_VEHICLE 1u /* hovercraft -- legal for Whale Bay */

static void vehicleselect_test_resolve(void) {
    if (sVsTestActive < 0) {
        const char *e = getenv("MDKR_TEST_ONLINE_VEHICLESELECT");
        sVsTestActive = (e != NULL) ? 1 : 0;
        /* Scenario from the env VALUE: "diverge" -> divergent-pick lane, "hold" ->
         * the frame-dump hold, anything else -> the default reject+chain+boot lane. */
        if (e != NULL && strstr(e, "diverge") != NULL) {
            sVsScenario = (s8) VS_SCN_DIVERGE;
        } else if (e != NULL && strstr(e, "hold") != NULL) {
            sVsScenario = (s8) VS_SCN_HOLD;
        } else {
            sVsScenario = (s8) VS_SCN_REJECT;
        }
    }
}

static void vehicleselect_test_reset(void) {
    vehicleselect_test_resolve();
    if (!sVsTestActive) {
        return;
    }
    sVsAdopted = 0u;
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
        room.phase = (uint8_t) VS_LOBBY_PHASE;
        for (i = 0u; i < MDKR_PARTY_LINK_SEATS; i++) {
            room.seats[i].character_id = VS_NO_CHARACTER;
            room.seats[i].vehicle_id = VS_NO_VEHICLE;
        }
        room.configured_track = (uint16_t) VS_TEST_TRACK;
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
                (unsigned) VS_TEST_TRACK);
    }
}

static void vehicleselect_test_reduce_and_script(void) {
    MdkrPartyLinkLocalIntent intent;

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
        } else {
            /* Pin the resolved track so the legality mask is non-trivial (Whale
             * Bay, hovercraft-only), whichever seam installed the room, and give
             * the scripted remote a track-legal vehicle for a coherent display. */
            sVsRoom.configured_track = (uint16_t) VS_TEST_TRACK;
            sVsRoom.seats[1].vehicle_id = (uint8_t) VS_TEST_REMOTE_VEHICLE;
        }
        sVsRoom.seats[1].ready = 1u;
        sVsAdopted = 1u;
    }

    /* Minimal launcher reducer: converge the local seat to the polled intent's
     * character + (mask-legal) vehicle + ready. The screen only ever publishes a
     * legal vehicle, so this simply mirrors it. */
    if (mdkr_party_link_intent_poll(&intent)) {
        if (intent.confirmed && intent.hover_character < VS_CHAR_COUNT) {
            sVsRoom.seats[0].character_id = intent.hover_character;
        }
        if (intent.vehicle_id < VS_PLAYER_VEHICLE_COUNT) {
            sVsRoom.seats[0].vehicle_id = intent.vehicle_id;
        }
        sVsRoom.seats[1].ready = 1u; /* scripted remote republishes ready */
        if (intent.ready &&
            sVsRoom.seats[0].character_id != VS_NO_CHARACTER &&
            sVsRoom.seats[0].vehicle_id != VS_NO_VEHICLE) {
            sVsRoom.seats[0].ready = 1u;
        } else if (intent.backout) {
            sVsRoom.seats[0].ready = 0u;
        }
    }

    mdkr_party_link_publish(&sVsRoom);
}

u8 mdkr_online_vehicleselect_test_active(void) {
    vehicleselect_test_resolve();
    return (u8) (sVsTestActive > 0 ? 1 : 0);
}

/* the screen's OWN confirm latch (reset by _enter's memset), used by the session
 * to gate the VEHICLESELECT -> TRACKSELECT hand-off so a back-out cannot one-frame
 * bounce forward before the player re-confirms a legal vehicle. */
u8 mdkr_online_vehicleselect_local_confirmed(void) {
    return (u8) (sVs.confirmed ? 1 : 0);
}

#endif /* MDKR_ENABLE_ONLINE_BETA */
