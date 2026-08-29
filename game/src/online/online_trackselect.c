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
 * LAYOUT (two-stage, DKR's worlds->doors mental model): a horizontal strip of the
 * 5 world (single) / cup (tournament) sky banners on top -- L/R selects, wrap --
 * and BELOW it ONLY the hovered world's 4 tracks as a full-width, centered,
 * UNTRUNCATED level_name() list (U/D selects). This kills the 5x4 grid's text
 * collisions and gives the joiner a legible mirror.
 *
 * WHAT IT BORROWS (read-only reuse of already-compiled game code/data; NO edit to
 * menu.c is required -- every symbol below already has external linkage):
 *   - gMenuAssets[] + menu_assetgroup_load/free (menu.c) the same texture-group
 *     loader the offline track select uses; we hand it the per-world BACKGROUND
 *     sky tile ids (TEXTURE_BACKGROUND_*_TOP) and blit them via texrect_draw.
 *   - level_name(id)             (game.c) the REAL, language-aware track names --
 *                                engine truth, so the list can never drift.
 *   - leveltable_vehicle_usable(id) (game.c) the REAL per-track vehicle mask --
 *                                engine truth for the auto-narrow, zero drift.
 *   - draw_text / set_text_*     (font.h) the real DKR font.
 *   - texrect_draw / bgdraw_*    (rcp_dkr.h) the game's own 2D blit.
 *   - sound_play + SOUND_*       (audio.h / sound_ids.h) the real menu SFX.
 *   - input_pressed / stick      (joypad.h) the real pad, local player only.
 *   - get_player_selected_vehicle(menu.c) the seed vehicle, same as CHARSELECT.
 *
 * WHAT IT OWNS (all state lives HERE, never an offline global): the cursor, the
 * mode toggle, the track/cup lock latch, the auto-narrowed local vehicle, the
 * continuous reverse-feed intent and the 2D layout. The forward feed (both seats
 * + the host's config + the host_cursor) is read from platform/net/party_link;
 * the JOINER renders that snapshot, not local state.
 *
 * ISOLATION: this engine TU pulls NO launcher/platform headers -- only the
 * decomp game headers and net/party_link.h, exactly like CHARSELECT. Track NAMES
 * come from level_name(); vehicle MASKS from leveltable_vehicle_usable(). The 20
 * selectable track ids are mirrored locally (sTrackIds[]); the headless lane
 * asserts they equal the reducer-accepted set (kCupTracks / known_race_track) and
 * the test seam itself refuses any config_track NOT in sTrackIds so a drifted
 * screen id cannot converge.
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
#include "online/online_screen_constants.h" /* shared screen size + lobby id-space
                                               mirrors (DRY across the native screens) */
#include "online/online_screen_util.h" /* shared local_seat / text / pulse helpers
                                          (DRY across the native screens) */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* gMenuAssets[] (the menu texture table; menu.h does not export it) and
 * gCurrDisplayList (the live 2D frame list) are both declared in
 * online_screen_util.h, shared with the other native screens. gMenuAssets[k]
 * holds a TextureHeader* for a loaded TEXTURE_* id (see menu_asset_load). The
 * screen size + shared launcher lobby id-space mirrors live in
 * online_screen_constants.h. */

/* MDKR_ONLINE_LOADING -- used only by this screen (the others do not need it). */
#define TS_LOADING_PHASE 2u          /* MDKR_ONLINE_LOADING */

/* Grid: 5 world columns x 4 round rows (== the 5 cups x 4 rounds). */
#define TS_COLS 5
#define TS_ROWS 4
#define TS_TRACK_COUNT (TS_COLS * TS_ROWS)
#define TS_COL_W 64
#define TS_NONE 0xFFu

/* Two-stage layout geometry. */
#define TS_TITLE_Y 18
#define TS_MODE_Y 32
#define TS_BANNER_Y 42     /* top of the 64x32 sky-tile strip */
#define TS_BANNER_LABEL_Y 62
#define TS_TRACK_Y0 88     /* first track row of the hovered world */
#define TS_TRACK_DY 14
#define TS_VEHICLE_Y 150
#define TS_STATUS_Y 170
#define TS_SEAT_Y 196      /* YOU / rival pair (charselect parity) */
#define TS_HELP_Y 224

/* Menu SFX (the real DKR enums, same reuse as CHARSELECT). */
#define TS_SFX_MOVE SOUND_MENU_PICK2
#define TS_SFX_LOCK SOUND_SELECT2
#define TS_SFX_START SOUND_SELECT3
#define TS_SFX_BACK SOUND_MENU_BACK3
#define TS_SFX_REJECT SOUND_ELECTRIC_BUZZ
#define TS_SFX_MODE SOUND_MENU_PICK2 /* distinct from the lock sound */

#define TS_LOCK_FLASH_TICKS 15u /* short flash; START works during it */
#define TS_VEH_FLASH_TICKS 20u  /* auto-narrow "vehicle changed" highlight */

/* The 20 selectable track ids, cup-major then round order -- MIRRORS kCupTracks
 * in platform/online/lobby_core.c (the reducer's authoritative accepted set) and
 * online_track_table.c. Column c == cup c (world display order); row r == round r;
 * track index == c*4 + r. The headless lane asserts this equals the reducer
 * set (test_online_lobby_core.c pins the same literal against known_race_track),
 * and the test seam refuses a config_track NOT in this table. (Every id is <= 33,
 * so it fits the u8 dispatch value the reverse feed carries.) */
static const u8 sTrackIds[TS_TRACK_COUNT] = {
    5u,  3u,  29u, 7u,  /* cup0 Dino Domain:        Ancient Lake / Fossil Canyon / Jungle Falls / Hot Top Volcano */
    13u, 6u,  9u,  28u, /* cup1 Snowflake Mountain: EverFrost / Walrus Cove / Snowball Valley / Frosty Village */
    8u,  4u,  10u, 30u, /* cup2 Sherbet Island:     Whale Bay / Pirate Lagoon / Crescent Island / Treasure Caves */
    19u, 18u, 20u, 31u, /* cup3 Dragon Forest:      Boulder Canyon / Greenwood / Windmill Plains / Haunted Woods */
    17u, 32u, 33u, 15u, /* cup4 Future Fun Land:    Spacedust Alley / DarkMoon / Star City / Spaceport Alpha */
};

/* Per-cup (== per-column) world background TOP sky-tile ids, in cup display
 * order. The offline track select's real per-world sky (menu.c
 * gTracksMenuBgTextureIndices). P6 (deferred): verify the *_TOP tiles are 64px
 * wide on the first screenshot -- the offline menu renders them scaled, so if a
 * banner overpaints its neighbour lay it out scaled to TS_COL_W (the *_BOTTOM
 * tiles are intentionally NOT loaded here, so nothing is loaded-never-drawn). */
static const s16 sCupBgTop[TS_COLS] = {
    TEXTURE_BACKGROUND_DINO_DOMAIN_TOP,       /* cup0 Dino */
    TEXTURE_BACKGROUND_SNOWFLAKE_MOUNTAIN_TOP,/* cup1 Snowflake */
    TEXTURE_BACKGROUND_SHERBERT_ISLAND_TOP,   /* cup2 Sherbet */
    TEXTURE_BACKGROUND_DRAGON_FOREST_TOP,     /* cup3 Dragon */
    TEXTURE_BACKGROUND_FUTURE_FUN_LAND_TOP,   /* cup4 FFL */
};

/* Short, plain world labels (display chrome only -- NOT the drift-sensitive track
 * set, so a static label is fine; track names themselves come from
 * level_name()). The full cup name goes to the status line. */
static const char *const sWorldLabels[TS_COLS] = {
    "DINO", "SNOW", "SHERBET", "DRAGON", "FUTURE",
};
static const char *const sCupNames[TS_COLS] = {
    "DINO DOMAIN CUP", "SNOWFLAKE CUP", "SHERBET CUP", "DRAGON FOREST CUP",
    "FUTURE FUN CUP",
};

/* Vehicle names for the always-on VEHICLE line. */
static const char *const sVehicleNames[MDKR_ONLINE_SCREEN_VEHICLE_COUNT] = {
    "CAR", "HOVERCRAFT", "PLANE",
};

/* The world sky tiles the banner strip + full-screen backdrop draw are loaded
 * READ-ONLY as the shared ten-tile group sOnlineSkyAssetIds (five worlds x
 * TOP+BOTTOM, online_screen_util.h). The banner strip binds each world's TOP from
 * sCupBgTop[] below; the full-screen scrolling backdrop pairs TOP+BOTTOM via the
 * shared mdkr_online_screen_backdrop() helper. */

/* ---- Session-owned screen state (never an offline global) ------------------ */
typedef struct MdkrOnlineTrackselectState {
    u8 mode;         /* MDKR_ONLINE_SCREEN_MODE_SINGLE / MDKR_ONLINE_SCREEN_MODE_TOURNAMENT (host-chosen) */
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
    u32 lockFlashEnd;/* "LOCKED" flash + remote-ready hold deadline, in ticks */
    u32 vehFlashEnd; /* auto-narrow "vehicle changed" highlight deadline */
    s8 stickLatchX;
    s8 stickLatchY;
} MdkrOnlineTrackselectState;

static MdkrOnlineTrackselectState sTs;

/* persists ACROSS entries (NOT reset by _enter's memset) so a tournament
 * group's next race restarts on the mode + selection they last locked. */
static u8 sLastMode = MDKR_ONLINE_SCREEN_MODE_SINGLE;
static u8 sLastLockedTrack = TS_NONE; /* track INDEX 0..19 */
static u8 sLastLockedCup = TS_NONE;

/* Resolved per-column background tiles (bound from gMenuAssets after load). */
static TextureHeader *sCupBgTopTex[TS_COLS];

/* Witness change-detect (file scope so _enter() can reset for a clean re-entry). */
static u32 sWitnessKey = 0xFFFFFFFFu;
static u8 sTracksWitnessed;

/* Resolved (display-only) view of the remote seat (bounded, NUL-forced name). */
typedef struct TsRemoteView {
    s8 seat;
    u8 present;
    u8 ready;
    char name[MDKR_PARTY_LINK_NAME_BYTES + 1u];
} TsRemoteView;

/* ---- forward decls (test seam defined at the bottom) ---------------------- */
static void trackselect_test_resolve(void);
static void trackselect_test_reset(void);
static void trackselect_test_reduce_and_script(void);

/* ======================================================================== *
 * Track / vehicle helpers (engine truth)
 * ======================================================================== */

static s32 trackselect_host_seat(const MdkrPartyLinkSnapshot *snap) {
    unsigned i;
    for (i = 0u; i < MDKR_PARTY_LINK_SEATS; i++) {
        if (snap->seats[i].occupied && snap->seats[i].is_host) {
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

/* Track INDEX (0..19) of a track id, or TS_NONE. */
static u8 trackselect_index_of(u16 trackId) {
    unsigned i;
    for (i = 0u; i < TS_TRACK_COUNT; i++) {
        if (sTrackIds[i] == trackId) {
            return (u8) i;
        }
    }
    return TS_NONE;
}

/* The usable-vehicle mask for a track at this player count: engine truth from
 * leveltable_vehicle_usable(), then the retail 2-player narrowing. */
static u8 trackselect_track_mask(u8 trackId, unsigned occupied) {
    u8 mask = (u8) leveltable_vehicle_usable((s32) trackId);
    if (mask == 0u) {
        mask = (u8) (1u << VEHICLE_CAR); /* fail-safe: never empty */
    }
    if (occupied >= 2u) {
        if (trackId == MDKR_ONLINE_SCREEN_TRACK_SPACEPORT_ALPHA) {
            mask &= (u8) ~(1u << VEHICLE_HOVERCRAFT);
        }
        if (trackId == MDKR_ONLINE_SCREEN_TRACK_FROSTY_VILLAGE) {
            mask &= (u8) ~(1u << VEHICLE_PLANE);
        }
    }
    return mask;
}

/* The track the vehicle auto-narrow resolves against. Once the HOST has
 * LOCKED a selection, resolve against the LOCKED track (single) / the locked
 * cup's round-0 track (tournament), NOT the hovered one -- otherwise browsing a
 * no-hovercraft track after locking hovercraft-only Whale Bay would re-narrow to
 * an illegal-for-the-locked-track vehicle and silently wedge START. A joiner
 * resolves against the host's locked track/cup from the snapshot (which is
 * unset in tournament until the host locks -- so key off snap.cup_id too). Returns
 * TS_NONE when there is nothing to narrow against yet (keep the seed vehicle). */
static u8 trackselect_resolve_narrow_track(const MdkrPartyLinkSnapshot *snap,
                                           bool haveSnap) {
    if (sTs.host) {
        if (sTs.lockedTrack != TS_NONE) {
            return sTrackIds[sTs.lockedTrack];
        }
        if (sTs.mode == MDKR_ONLINE_SCREEN_MODE_TOURNAMENT && sTs.lockedCup != TS_NONE) {
            return sTrackIds[(sTs.lockedCup * TS_ROWS) + 0u];
        }
        /* Nothing locked: preview against the hovered selection. */
        if (sTs.mode == MDKR_ONLINE_SCREEN_MODE_TOURNAMENT) {
            return sTrackIds[(sTs.cursorCol * TS_ROWS) + 0u];
        }
        return sTrackIds[(sTs.cursorCol * TS_ROWS) + sTs.cursorRow];
    }
    /* Joiner: follow the host's locked selection from the snapshot. */
    if (!haveSnap) {
        return TS_NONE;
    }
    if (snap->mode == MDKR_ONLINE_SCREEN_MODE_TOURNAMENT) {
        if (snap->cup_id < TS_COLS) {
            return sTrackIds[(snap->cup_id * TS_ROWS) + 0u];
        }
        return TS_NONE;
    }
    if (snap->configured_track != 0xFFFFu &&
        trackselect_index_of(snap->configured_track) != TS_NONE) {
        return (u8) snap->configured_track;
    }
    return TS_NONE;
}

/* keep the local seat's vehicle inside a track's mask (lowest legal bit when
 * the current one is illegal). Flags a change so the VEHICLE line can flash. */
static void trackselect_autonarrow_vehicle(u8 trackId, unsigned occupied) {
    u8 mask;
    u8 v;
    if (trackId == TS_NONE) {
        return; /* nothing to narrow against yet */
    }
    mask = trackselect_track_mask(trackId, occupied);
    if (mask & (u8) (1u << sTs.vehicle)) {
        return; /* already legal */
    }
    for (v = 0u; v < MDKR_ONLINE_SCREEN_VEHICLE_COUNT; v++) {
        if (mask & (u8) (1u << v)) {
            if (sTs.vehicle != v) {
                sTs.vehicle = v;
                sTs.vehFlashEnd = sTs.ticks + TS_VEH_FLASH_TICKS;
            }
            return;
        }
    }
}

/* Resolve the first occupied non-local (remote) seat into a bounded view.
 * NOTE fenced 2-endpoint beta: first remote seat only. */
static void trackselect_resolve_remote(const MdkrPartyLinkSnapshot *snap,
                                       bool haveSnap, s32 localSeat,
                                       TsRemoteView *out) {
    unsigned i;
    memset(out, 0, sizeof(*out));
    out->seat = -1;
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
        out->ready = seat->ready ? 1u : 0u;
        memcpy(out->name, seat->name, MDKR_PARTY_LINK_NAME_BYTES); /* untrusted */
        out->name[MDKR_PARTY_LINK_NAME_BYTES] = '\0';
        break;
    }
}

/* ======================================================================== *
 * Input
 * ======================================================================== */
typedef struct TsInput {
    s8 dx;        /* -1 / 0 / +1 column step (edge) */
    s8 dy;        /* -1 / 0 / +1 row step (edge) */
    u8 aEdge;     /* A: lock track/cup */
    u8 bEdge;     /* B: back one level (to the native VEHICLE screen) */
    u8 startEdge; /* Start: begin the race (host, once locked) */
    u8 modeEdge;  /* Z: toggle single/tournament (host) */
} TsInput;

/* Headless scenario, from the env VALUE (see trackselect_test_resolve):
 *   0 SINGLE_HOST (default) -- host locks a single track; also the B-back lane.
 *   1 JOINER          -- local seat is a JOINER; the seam scripts a remote HOST
 *                        locking a tournament cup, to prove the joiner renders the
 *                        room and narrows to the cup's round-0 track. */
#define TS_SCN_SINGLE_HOST 0
#define TS_SCN_JOINER 1
static s8 sTsScenario = -1;

/* Scripted headless input (env MDKR_TEST_ONLINE_TRACKSELECT). Keyed on the
 * TRACKSELECT ENTRY count. In the JOINER scenario the local player watches (input
 * suppressed), so nothing is scripted here -- the seam drives the remote host. In
 * the SINGLE_HOST scenario: entry 1 proves the B->CHARSELECT back path (no wedge);
 * later entries walk to Whale Bay (cup2/round0, track 8, hovercraft-only 0x2) so
 * the auto-narrow moves off Car, LOCK it, then browse AWAY to Spaceport Alpha
 * (cup4/round3, track 15 -- 2P narrowing drops its hovercraft) to prove (the
 * publishable vehicle stays legal for the LOCKED track, not the hovered one), then
 * Start. */
static u8 sTsEntryCount; /* incremented each _enter (test), persists across a run */

static void trackselect_input_scripted(TsInput *in) {
    memset(in, 0, sizeof(*in));
    if (sTsScenario == TS_SCN_JOINER) {
        return; /* joiner: watch only, the seam drives the host */
    }
    if (sTsEntryCount <= 1u) {
        if (sTs.ticks == 3u) {
            in->bEdge = 1u;
        }
        return;
    }
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
    /* browse AWAY from the locked track to Spaceport Alpha (col4,row3). Its
     * 2P mask drops hovercraft, so the OLD (hovered-track) narrow would flip the
     * seat to Car -- illegal for the LOCKED Whale Bay -- wedging Start. The fix
     * narrows against the LOCKED track, so the vehicle must stay hovercraft. */
    case 9u:
        in->dx = 1; /* col 2 -> 3 */
        break;
    case 10u:
        in->dx = 1; /* col 3 -> 4 (Future Fun Land) */
        break;
    case 11u:
    case 12u:
    case 13u:
        in->dy = 1; /* row 0 -> 3 (Spaceport Alpha, track 15) */
        break;
    case 24u:
        in->startEdge = 1u; /* host start once reconverged to all-ready */
        break;
    default:
        break;
    }
}

static void trackselect_input_live(TsInput *in) {
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

/* minimal scripted input for the headless LOBBY-START lane (env
 * MDKR_TEST_ONLINE_LOBBY_START). Unlike the co-designed SINGLE_HOST scenario
 * (which walks specific tracks against the self-contained reducer), this drives a
 * REAL launcher adapter, so it stays trivial + convergence-robust: browse ONE row
 * down to track index 1 == Fossil Canyon (id 3, mask 0x07 -- Car-legal, so the
 * auto-narrow never flips the Car charselect defaults to), LOCK it, then request
 * START. Index 1 is chosen DELIBERATELY DIFFERENT from the room's READY-unlock
 * pre-config (track 5): the native SET_CONFIG_TRACK(3) is then a REAL, non-deduped
 * reverse-feed dispatch, so "the booted track is the one the native TRACKSELECT
 * chose" is genuinely proven (not vacuously honored by the pre-config). Both the
 * lock and START latch and are republished every frame, so an ASYNC reducer (with
 * its ready-clear-on-config-change + re-ready) converges regardless of timing. */
static s8 sTsLobbyStartInput = -1; /* -1 unresolved, 0 off, 1 on */
static u8 trackselect_lobby_input_active(void) {
    if (sTsLobbyStartInput < 0) {
        sTsLobbyStartInput =
            (getenv("MDKR_TEST_ONLINE_LOBBY_START") != NULL) ? 1 : 0;
    }
    return (u8) (sTsLobbyStartInput > 0 ? 1 : 0);
}

/* the lobby-start TOURNAMENT lane (the demo mode). When set, the native
 * TRACKSELECT ENTERS in TOURNAMENT mode focused on the room's pre-configured cup
 * (read from the forward feed), so it publishes mode=TOURNAMENT + that cup from the
 * FIRST frame -- a converged no-op with the room, never a stray SET_MODE(SINGLE)
 * that would flip the pre-configured tournament back to single. The scripted input
 * then LOCKs the focused cup (aEdge) and issues the real START. The cup schedule
 * owns every round's track, so no per-round native track pick is needed here (the
 * resident coordinator re-cycles rounds 2..4). */
static s8 sTsLobbyTournament = -1; /* -1 unresolved, 0 off, 1 on */
static u8 trackselect_lobby_tournament_active(void) {
    if (sTsLobbyTournament < 0) {
        sTsLobbyTournament =
            (getenv("MDKR_TEST_ONLINE_LOBBY_TOURNAMENT") != NULL) ? 1 : 0;
    }
    return (u8) (sTsLobbyTournament > 0 ? 1 : 0);
}

static void trackselect_input_lobby_start(TsInput *in) {
    memset(in, 0, sizeof(*in));
    switch (sTs.ticks) {
    case 2u:
        in->dy = 1; /* row 0 -> 1: track index 1 == Fossil Canyon (id 3) */
        break;
    case 5u:
        in->aEdge = 1u; /* lock track index 1 -> id 3 (SET_CONFIG_TRACK clears ready) */
        break;
    case 10u:
        in->startEdge = 1u; /* host start; startReq latches + re-fires each frame */
        break;
    default:
        break;
    }
}

static void trackselect_gather_input(TsInput *in) {
    if (mdkr_online_trackselect_test_active()) {
        trackselect_input_scripted(in);
    } else if (trackselect_lobby_input_active()) {
        trackselect_input_lobby_start(in);
    } else {
        trackselect_input_live(in);
    }
}

/* Track/cup/mode/start are HOST-ONLY (the joiner watches). B backs out for
 * everyone. */
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
        sTs.mode = (sTs.mode == MDKR_ONLINE_SCREEN_MODE_SINGLE) ? MDKR_ONLINE_SCREEN_MODE_TOURNAMENT
                                                : MDKR_ONLINE_SCREEN_MODE_SINGLE;
        sTs.lockedTrack = TS_NONE; /* the reducer clears ready too; republish
                                    * reconverges */
        sTs.lockedCup = TS_NONE;
        sTs.startReq = 0u;
        sTs.cursorRow = 0u;
        sound_play(TS_SFX_MODE, NULL); /* distinct from the lock sound */
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
    if (in->dy != 0 && sTs.mode == MDKR_ONLINE_SCREEN_MODE_SINGLE) {
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
        if (sTs.mode == MDKR_ONLINE_SCREEN_MODE_SINGLE) {
            sTs.lockedTrack = (u8) ((sTs.cursorCol * TS_ROWS) + sTs.cursorRow);
            sLastLockedTrack = sTs.lockedTrack; /* persistence */
        } else {
            sTs.lockedCup = sTs.cursorCol;
            sLastLockedCup = sTs.lockedCup; /* persistence */
        }
        sLastMode = sTs.mode;
        sTs.startReq = 0u; /* a fresh lock re-arms Start */
        sTs.lockFlashEnd = sTs.ticks + TS_LOCK_FLASH_TICKS;
        sound_play(TS_SFX_LOCK, NULL);
    } else if (in->startEdge) {
        bool locked = (sTs.mode == MDKR_ONLINE_SCREEN_MODE_SINGLE)
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

/* Publish the FULL local intent every frame (continuous republish so the
 * reducer's ready-clear on any config change reconverges within a pump). The
 * intent starts from the shared init helper so the host-only config fields carry
 * their UNSET sentinels unless THIS (host) screen sets them. */
static void trackselect_publish_intent(u8 localSeatChar) {
    MdkrPartyLinkLocalIntent intent;
    mdkr_party_link_intent_init(&intent);

    if (localSeatChar < MDKR_ONLINE_SCREEN_CHAR_COUNT) {
        intent.hover_character = localSeatChar;
        intent.confirmed = 1u;
    }
    intent.vehicle_id = sTs.vehicle;
    intent.ready = 1u;
    intent.backout = 0u;

    if (sTs.host) {
        intent.mode = sTs.mode; /* host always drives the mode */
        if (sTs.mode == MDKR_ONLINE_SCREEN_MODE_SINGLE && sTs.lockedTrack != TS_NONE) {
            intent.config_track = sTrackIds[sTs.lockedTrack];
        } else if (sTs.mode == MDKR_ONLINE_SCREEN_MODE_TOURNAMENT && sTs.lockedCup != TS_NONE) {
            intent.cup_id = sTs.lockedCup;
        }
        intent.start_requested = sTs.startReq ? 1u : 0u;
    }
    mdkr_party_link_intent_publish(&intent);
}

/* ======================================================================== *
 * Render (native two-stage: sky-tile banner strip + hovered world's tracks)
 * ======================================================================== */
/* Blit one 64x32 world-sky tile (dimmed) as a strip banner. */
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
                               s32 localSeat, const TsRemoteView *rv) {
    /* Everything on this screen is driven by the AUTHORITATIVE snapshot for a
     * joiner: the mode, the focused world, the locked cell and the status
     * all come from the room, and the local cursor is SUPPRESSED (a pulsing
     * cursor that ignores input reads as hung). The host renders its own live
     * selection. */
    u8 host = sTs.host;
    u8 effMode = (host || !haveSnap) ? sTs.mode : (u8) snap->mode;
    s32 tri = mdkr_online_screen_pulse(sTs.ticks);
    u8 focusWorld;
    u8 lockedTrackIdx = TS_NONE; /* which of the 20 is the effective lock */
    u8 lockedCup = TS_NONE;
    u8 localReady = 0u;
    bool bothReady;
    u8 c;
    u8 r;
    char line[64];
    const char *rname = (rv->name[0] != '\0') ? rv->name : "RIVAL";

    /* Resolve the effective lock + focus world from the right source. */
    if (host) {
        if (effMode == MDKR_ONLINE_SCREEN_MODE_SINGLE) {
            lockedTrackIdx = sTs.lockedTrack;
        } else {
            lockedCup = sTs.lockedCup;
        }
        focusWorld = sTs.cursorCol;
    } else {
        if (effMode == MDKR_ONLINE_SCREEN_MODE_SINGLE && haveSnap &&
            snap->configured_track != 0xFFFFu) {
            lockedTrackIdx = trackselect_index_of(snap->configured_track);
        } else if (effMode == MDKR_ONLINE_SCREEN_MODE_TOURNAMENT && haveSnap &&
                   snap->cup_id < TS_COLS) {
            lockedCup = snap->cup_id;
        }
        /* Follow the host's locked selection; default to world 0 before a lock. */
        if (lockedTrackIdx != TS_NONE) {
            focusWorld = (u8) (lockedTrackIdx / TS_ROWS);
        } else if (lockedCup != TS_NONE) {
            focusWorld = lockedCup;
        } else {
            focusWorld = 0u;
        }
    }

    if (haveSnap && localSeat >= 0) {
        localReady = snap->seats[localSeat].ready ? 1u : 0u;
    }
    bothReady = haveSnap && localReady && rv->present && rv->ready;

    /* Title + mode line (P3: title y/backdrop match charselect's family). */
    mdkr_online_screen_text(MDKR_ONLINE_SCREEN_W_HALF, TS_TITLE_Y, ASSET_FONTS_BIGFONT,
                     effMode == MDKR_ONLINE_SCREEN_MODE_SINGLE ? "SELECT TRACK" : "SELECT CUP",
                     ALIGN_MIDDLE_CENTER, 255, 224, 96);
    if (host) {
        (void) snprintf(line, sizeof(line), "%s   Z: MODE",
                        effMode == MDKR_ONLINE_SCREEN_MODE_SINGLE ? "SINGLE RACE" : "TOURNAMENT");
    } else {
        (void) snprintf(line, sizeof(line), "%s",
                        effMode == MDKR_ONLINE_SCREEN_MODE_SINGLE ? "SINGLE RACE" : "TOURNAMENT");
    }
    mdkr_online_screen_text(MDKR_ONLINE_SCREEN_W_HALF, TS_MODE_Y, ASSET_FONTS_SMALLFONT, line,
                     ALIGN_MIDDLE_CENTER, 200, 200, 255);

    /* STAGE 1: the 5 world/cup sky banners. Hovered bright, others dim. The
     * joiner has no cursor, so it brightens the FOCUSED (locked) world instead. */
    for (c = 0u; c < TS_COLS; c++) {
        s32 cx = (s32) c * TS_COL_W + (TS_COL_W / 2);
        bool onFocus = (c == focusWorld);
        bool cupLocked = (effMode == MDKR_ONLINE_SCREEN_MODE_TOURNAMENT && lockedCup == c);
        /* T9 NIT-3 (trackselect "LOCKED while browsing"): the SINGLE-race host can
         * lock a track then move the cursor to PREVIEW another world without locking
         * a new one (the lock is sticky -- START still uses it, which is truthful).
         * The T8 capture caught exactly that scripted state ("WHALE BAY LOCKED" while
         * the cursor browsed FFL) and it read as a bug because nothing on-screen tied
         * the status to a world. It is NOT a 2-endpoint bug (the joiner's focus ALWAYS
         * follows the lock, so its view is always coherent), but to make the host view
         * coherent too, mark the WORLD banner that holds the current single-race lock
         * green -- the same cue tournament already gives a locked cup. Now the status
         * "<track> LOCKED" always has a matching green banner, even mid-browse. */
        bool worldHoldsLock =
            (effMode == MDKR_ONLINE_SCREEN_MODE_SINGLE && lockedTrackIdx != TS_NONE &&
             (u8) (lockedTrackIdx / TS_ROWS) == c);
        u8 dim = (onFocus || cupLocked || worldHoldsLock) ? 210u : 110u;
        s32 lr, lg, lb;

        trackselect_draw_banner(c, dim);
        if (cupLocked || worldHoldsLock) {
            lr = 120; lg = 255; lb = 120;
        } else if (onFocus) {
            lr = 255; lg = 240; lb = 160;
        } else {
            lr = 190; lg = 190; lb = 190;
        }
        mdkr_online_screen_text(cx, TS_BANNER_LABEL_Y, ASSET_FONTS_SMALLFONT,
                         (char *) sWorldLabels[c], ALIGN_MIDDLE_CENTER, lr, lg,
                         lb);
    }

    /* STAGE 2: the focused world's 4 tracks, full-width + untruncated. In single
     * race they are selectable (host cursor); in tournament they are a read-only
     * round preview and A locks the whole cup. */
    for (r = 0u; r < TS_ROWS; r++) {
        u8 idx = (u8) (focusWorld * TS_ROWS + r);
        char *name = level_name((s32) sTrackIds[idx]);
        s32 y = TS_TRACK_Y0 + (s32) r * TS_TRACK_DY;
        bool onCursor = host && effMode == MDKR_ONLINE_SCREEN_MODE_SINGLE &&
                        r == sTs.cursorRow;
        bool locked = (effMode == MDKR_ONLINE_SCREEN_MODE_SINGLE && lockedTrackIdx == idx) ||
                      (effMode == MDKR_ONLINE_SCREEN_MODE_TOURNAMENT && lockedCup == focusWorld);
        s32 nr = 190, ng = 190, nb = 190;
        char label[32];

        if (name == NULL) {
            name = (char *) "?";
        }
        if (locked) {
            nr = 120; ng = 255; nb = 120;
            /* text redundancy, not colour alone. */
            (void) snprintf(label, sizeof(label), "*%s*", name);
        } else if (onCursor) {
            nr = 255; ng = 190 + tri * 4; nb = 60 + tri * 3;
            (void) snprintf(label, sizeof(label), ">%s<", name);
        } else {
            if (effMode == MDKR_ONLINE_SCREEN_MODE_TOURNAMENT) {
                nr = ng = nb = 150; /* dim read-only preview */
            }
            (void) snprintf(label, sizeof(label), "%s", name);
        }
        mdkr_online_screen_text(MDKR_ONLINE_SCREEN_W_HALF, y, ASSET_FONTS_SMALLFONT, label,
                         ALIGN_MIDDLE_CENTER, nr, ng, nb);
    }

    /* the always-on VEHICLE line (the screen visibly knows the track's
     * legal vehicles), with a brief highlight when the auto-narrow changes it. */
    {
        bool flash = sTs.ticks < sTs.vehFlashEnd;
        const char *vn = sTs.vehicle < MDKR_ONLINE_SCREEN_VEHICLE_COUNT
                             ? sVehicleNames[sTs.vehicle]
                             : "CAR";
        (void) snprintf(line, sizeof(line), "VEHICLE: %s", vn);
        mdkr_online_screen_text(MDKR_ONLINE_SCREEN_W_HALF, TS_VEHICLE_Y, ASSET_FONTS_SMALLFONT,
                         line, ALIGN_MIDDLE_CENTER, flash ? 255 : 180,
                         flash ? 240 : 180, flash ? 120 : 180);
    }

    /* Status line: names the pick + reflects the START / host-choosing state so a
     * deferred/refused start is never a dead screen. */
    {
        char pickName[24];
        const char *pick = "";
        bool haveLock = (effMode == MDKR_ONLINE_SCREEN_MODE_SINGLE) ? (lockedTrackIdx != TS_NONE)
                                                    : (lockedCup != TS_NONE);
        if (effMode == MDKR_ONLINE_SCREEN_MODE_SINGLE && lockedTrackIdx != TS_NONE) {
            char *nm = level_name((s32) sTrackIds[lockedTrackIdx]);
            (void) snprintf(pickName, sizeof(pickName), "%.20s",
                            nm != NULL ? nm : "?");
            pick = pickName;
        } else if (effMode == MDKR_ONLINE_SCREEN_MODE_TOURNAMENT && lockedCup != TS_NONE) {
            (void) snprintf(pickName, sizeof(pickName), "%.20s",
                            sCupNames[lockedCup]);
            pick = pickName;
        }

        if (!host) {
            /* Joiner: walk HOST IS CHOOSING -> HOST PICKED -> STARTING, alive via
             * the tri pulse. Use the host seat's name where available. */
            char hostName[16] = "HOST";
            s32 hostSeat = haveSnap ? trackselect_host_seat(snap) : -1;
            if (hostSeat >= 0 && snap->seats[hostSeat].name[0] != '\0') {
                (void) snprintf(hostName, sizeof(hostName), "%.12s",
                                snap->seats[hostSeat].name);
            }
            if (haveSnap && snap->phase != (uint8_t) MDKR_ONLINE_SCREEN_LOBBY_PHASE) {
                mdkr_online_screen_text(MDKR_ONLINE_SCREEN_W_HALF, TS_STATUS_Y,
                                 ASSET_FONTS_SMALLFONT, "STARTING...",
                                 ALIGN_MIDDLE_CENTER, 120, 255, 120);
            } else if (haveLock) {
                (void) snprintf(line, sizeof(line), "HOST PICKED: %s", pick);
                mdkr_online_screen_text(MDKR_ONLINE_SCREEN_W_HALF, TS_STATUS_Y,
                                 ASSET_FONTS_SMALLFONT, line, ALIGN_MIDDLE_CENTER,
                                 120, 255, 120);
            } else {
                (void) snprintf(line, sizeof(line), "%s IS CHOOSING...",
                                hostName);
                mdkr_online_screen_text(MDKR_ONLINE_SCREEN_W_HALF, TS_STATUS_Y,
                                 ASSET_FONTS_SMALLFONT, line, ALIGN_MIDDLE_CENTER,
                                 150 + tri * 4, 150 + tri * 4, 120);
            }
        } else if (sTs.startReq && haveLock) {
            if (bothReady) {
                mdkr_online_screen_text(MDKR_ONLINE_SCREEN_W_HALF, TS_STATUS_Y,
                                 ASSET_FONTS_SMALLFONT, "STARTING...",
                                 ALIGN_MIDDLE_CENTER, 120, 255, 120);
            } else {
                (void) snprintf(line, sizeof(line), "WAITING FOR %.12s...",
                                rname);
                mdkr_online_screen_text(MDKR_ONLINE_SCREEN_W_HALF, TS_STATUS_Y,
                                 ASSET_FONTS_SMALLFONT, line, ALIGN_MIDDLE_CENTER,
                                 255, 240, 120);
            }
        } else if (haveLock) {
            (void) snprintf(line, sizeof(line), "%s LOCKED - PRESS START", pick);
            mdkr_online_screen_text(MDKR_ONLINE_SCREEN_W_HALF, TS_STATUS_Y, ASSET_FONTS_SMALLFONT,
                             line, ALIGN_MIDDLE_CENTER, 120, 255, 120);
        } else {
            mdkr_online_screen_text(MDKR_ONLINE_SCREEN_W_HALF, TS_STATUS_Y, ASSET_FONTS_SMALLFONT,
                             effMode == MDKR_ONLINE_SCREEN_MODE_SINGLE ? "CHOOSE A TRACK"
                                                       : "CHOOSE A CUP",
                             ALIGN_MIDDLE_CENTER, 220, 220, 220);
        }
    }

    /* always-visible seat presence/ready pair (charselect family). Hold the
     * displayed READY through the lock-flash window so the reducer's 1-2-frame
     * ready-clear does not flicker CHOOSING. */
    {
        bool holdReady = sTs.ticks < sTs.lockFlashEnd;
        bool youReady = localReady || holdReady;
        (void) snprintf(line, sizeof(line), "YOU: %s",
                        youReady ? "READY" : "SYNCING");
        mdkr_online_screen_text(24, TS_SEAT_Y, ASSET_FONTS_SMALLFONT, line,
                         ALIGN_MIDDLE_LEFT, youReady ? 120 : 220,
                         youReady ? 255 : 220, youReady ? 120 : 220);
        if (!rv->present) {
            mdkr_online_screen_text(MDKR_ONLINE_SCREEN_W - 24, TS_SEAT_Y, ASSET_FONTS_SMALLFONT,
                             "WAITING FOR PLAYER...", ALIGN_MIDDLE_RIGHT, 150, 150,
                             150);
        } else {
            bool rready = rv->ready || holdReady;
            (void) snprintf(line, sizeof(line), "%.12s: %s", rname,
                            rready ? "READY" : "CHOOSING");
            mdkr_online_screen_text(MDKR_ONLINE_SCREEN_W - 24, TS_SEAT_Y, ASSET_FONTS_SMALLFONT,
                             line, ALIGN_MIDDLE_RIGHT, rready ? 120 : 220,
                             rready ? 255 : 220, rready ? 120 : 220);
        }
    }

    /* context-sensitive help in the charselect verb family. */
    if (!host) {
        mdkr_online_screen_text(MDKR_ONLINE_SCREEN_W_HALF, TS_HELP_Y, ASSET_FONTS_SMALLFONT,
                         "B: BACK", ALIGN_MIDDLE_CENTER, 255, 255, 255);
    } else {
        bool haveLock = (effMode == MDKR_ONLINE_SCREEN_MODE_SINGLE) ? (lockedTrackIdx != TS_NONE)
                                                    : (lockedCup != TS_NONE);
        mdkr_online_screen_text(MDKR_ONLINE_SCREEN_W_HALF, TS_HELP_Y, ASSET_FONTS_SMALLFONT,
                         haveLock ? "START: BEGIN   A: CHANGE PICK   B: BACK"
                                  : "A: SELECT   B: BACK",
                         ALIGN_MIDDLE_CENTER, 255, 255, 255);
    }

    /* Live retail preview: re-arm the scrolling sky to the FOCUSED world (the
     * host's hovered cup column, or the joiner's authoritative locked world).
     * focusWorld is in cup display order; the shared helper maps it to the sky
     * WORLD and the engine's next bgdraw_render() draws it. All tiles are already
     * resident (loaded once in _enter), so this is a cheap pointer re-bind. */
    mdkr_online_screen_backdrop(mdkr_online_screen_sky_world_for_cup(focusWorld));
}

/* Bounded stderr witness: one line only when the visible/config state changes. */
static void trackselect_witness(const MdkrPartyLinkSnapshot *snap, bool haveSnap,
                                u8 resolvedTrack, u8 mask) {
    u8 seat0Ready = 0u;
    u8 seat1Ready = 0u;
    u16 snapCfg = 0xFFFFu;
    u8 snapCup = 0xFFu;
    u8 snapMode = 0u;
    u8 snapPhase = 0u;
    u32 key;

    if (haveSnap) {
        seat0Ready = snap->seats[0].ready;
        seat1Ready = snap->seats[1].ready;
        snapCfg = snap->configured_track;
        snapCup = snap->cup_id;
        snapMode = snap->mode;
        snapPhase = snap->phase;
    }

    /* fold mode/cursor/vehicle/ready/phase/resolvedTrack AND the snapshot's
     * configured_track + cup_id + the locked selection into the change key so the
     * joiner + tournament assertions get their rows. */
    key = ((u32) sTs.mode) | ((u32) sTs.cursorCol << 1) |
          ((u32) sTs.cursorRow << 4) | ((u32) sTs.vehicle << 6) |
          ((u32) (seat0Ready ? 1u : 0u) << 8) |
          ((u32) (seat1Ready ? 1u : 0u) << 9) | ((u32) snapPhase << 10) |
          ((u32) resolvedTrack << 13) |
          ((u32) ((snapCfg == 0xFFFFu) ? 0x3Fu : (snapCfg & 0x3Fu)) << 20) |
          ((u32) ((snapCup >= TS_COLS) ? 0x7u : snapCup) << 26) |
          ((u32) sTs.startReq << 29) | ((u32) snapMode << 30);
    if (key == sWitnessKey) {
        return;
    }
    sWitnessKey = key;

    fprintf(stderr,
            "[online-trackselect] render mode=%u col=%u row=%u host=%u "
            "track=%u mask=0x%x vehicle=%u locked{track=%d cup=%d} "
            "seat{r0=%u r1=%u} snap{mode=%u cfgTrack=%u cup=%u phase=%u} "
            "start=%u\n",
            (unsigned) sTs.mode, (unsigned) sTs.cursorCol,
            (unsigned) sTs.cursorRow, (unsigned) sTs.host,
            (unsigned) resolvedTrack, (unsigned) mask, (unsigned) sTs.vehicle,
            sTs.lockedTrack != TS_NONE ? (int) sTrackIds[sTs.lockedTrack] : -1,
            sTs.lockedCup != TS_NONE ? (int) sTs.lockedCup : -1,
            (unsigned) seat0Ready, (unsigned) seat1Ready, (unsigned) snapMode,
            (unsigned) snapCfg, (unsigned) snapCup, (unsigned) snapPhase,
            (unsigned) sTs.startReq);
}

/* Emit the offered track-id list ONCE so the lane can assert it equals the
 * reducer-accepted set. */
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
    /* restore the last locked mode + cursor position (not the lock itself --
     * the host re-confirms, exactly as charselect re-confirms its cursor seed). */
    sTs.mode = sLastMode;
    sTs.lockedTrack = TS_NONE;
    sTs.lockedCup = TS_NONE;
    if (sTs.mode == MDKR_ONLINE_SCREEN_MODE_SINGLE && sLastLockedTrack != TS_NONE) {
        sTs.cursorCol = (u8) (sLastLockedTrack / TS_ROWS);
        sTs.cursorRow = (u8) (sLastLockedTrack % TS_ROWS);
    } else if (sTs.mode == MDKR_ONLINE_SCREEN_MODE_TOURNAMENT && sLastLockedCup != TS_NONE) {
        sTs.cursorCol = sLastLockedCup;
    }

    /* the lobby-start TOURNAMENT lane enters in TOURNAMENT mode focused on
     * the room's PRE-CONFIGURED cup (from the forward feed), so the very first
     * published intent carries mode=TOURNAMENT + that cup -- converged with the room,
     * never a stray SET_MODE(SINGLE) that would flip the pre-configured tournament.
     * The scripted input then LOCKs this focused cup + STARTs. */
    if (trackselect_lobby_tournament_active()) {
        MdkrPartyLinkSnapshot esnap;
        sTs.mode = MDKR_ONLINE_SCREEN_MODE_TOURNAMENT;
        sTs.cursorRow = 0u;
        if (mdkr_party_link_read(&esnap) && esnap.cup_id < TS_COLS) {
            sTs.cursorCol = esnap.cup_id;
        }
        sLastMode = MDKR_ONLINE_SCREEN_MODE_TOURNAMENT;
    }

    /* Seed the vehicle exactly as CHARSELECT does; the per-tick auto-narrow then
     * keeps it inside the resolved track's mask. */
    defaultVehicle = get_player_selected_vehicle(MDKR_ONLINE_SCREEN_LOCAL_PAD);
    if (defaultVehicle < 0 || (u8) defaultVehicle >= MDKR_ONLINE_SCREEN_VEHICLE_COUNT) {
        defaultVehicle = (s8) VEHICLE_CAR;
    }
    sTs.vehicle = (u8) defaultVehicle;

    sWitnessKey = 0xFFFFFFFFu;
    trackselect_test_reset();

    /* Borrow the real per-world sky tiles (the charselect asset-borrow discipline;
     * menu_asset_load routes each id to load_texture). The shared ten-tile group
     * carries every world's TOP+BOTTOM, so the banner strip binds each TOP here and
     * the full-screen backdrop can preview ANY hovered world without a reload. */
    menu_assetgroup_load(sOnlineSkyAssetIds);
    for (c = 0u; c < TS_COLS; c++) {
        sCupBgTopTex[c] = (TextureHeader *) gMenuAssets[sCupBgTop[c]];
    }

    load_font(ASSET_FONTS_BIGFONT);
    load_font(ASSET_FONTS_SMALLFONT);

    sTs.assets = 1u;
    /* Retail scrolling sky of the initially-focused world (cursorCol is in cup
     * display order); the per-frame render re-arms it as the host browses. */
    mdkr_online_screen_backdrop(
        mdkr_online_screen_sky_world_for_cup(sTs.cursorCol));

    if (mdkr_online_trackselect_test_active()) {
        sTsEntryCount++;
    }
    trackselect_witness_tracks();

    /* T7b: reveal from black (retail fade cadence) + keep the retail menu music
     * (isolation-safe primitive borrows -- see online_screen_util.h). */
    mdkr_online_screen_fade_in_from_black();
    mdkr_online_screen_menu_music();

    fprintf(stderr,
            "[online-trackselect] enter: native track select up entry=%u "
            "mode=%u defaultVehicle=%u (world backgrounds loaded, offline _loop "
            "bypassed)\n",
            (unsigned) sTsEntryCount, (unsigned) sTs.mode,
            (unsigned) sTs.vehicle);
}

void mdkr_online_trackselect_exit(void) {
    u8 c;
    if (sTs.assets) {
        /* Disarm the borrowed sky before freeing its tiles (bgdraw_render lifetime). */
        mdkr_online_screen_backdrop_clear();
        unload_font(ASSET_FONTS_SMALLFONT);
        unload_font(ASSET_FONTS_BIGFONT);
        menu_assetgroup_free(sOnlineSkyAssetIds);
        for (c = 0u; c < TS_COLS; c++) {
            sCupBgTopTex[c] = NULL;
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
    u8 localSeatChar = MDKR_ONLINE_SCREEN_NO_CHARACTER;
    u8 narrowTrack;
    u8 witnessTrack;
    u8 mask;
    TsRemoteView rv;
    TsInput in;

    (void) updateRate;

    haveSnap = mdkr_party_link_read(&snap);
    localSeat = haveSnap ? mdkr_online_screen_local_seat(&snap) : -1;
    occupied = trackselect_occupied_seats(&snap, haveSnap);

    if (localSeat >= 0) {
        sTs.host = snap.seats[localSeat].is_host ? 1u : 0u;
        localSeatChar = snap.seats[localSeat].character_id;
    } else {
        /* no resolvable local seat -> assume host so a real run still lets the
         * leader drive the screen. This is UI-only: the launcher-side dispatch is
         * separately gated on the local seat's is_host in the lobby view, so a
         * mislabelled joiner can never actually send a host-config command. */
        sTs.host = 1u;
    }

    /* narrow the local vehicle against the LOCKED track (host)
     * or the host's locked track/cup from the snapshot (joiner) -- never the
     * merely-hovered track once something is locked. */
    narrowTrack = trackselect_resolve_narrow_track(&snap, haveSnap);
    trackselect_autonarrow_vehicle(narrowTrack, occupied);

    trackselect_gather_input(&in);
    trackselect_apply_input(&in);

    /* Re-resolve after input (the lock/cursor may have moved) so the published
     * vehicle + the witness reflect this frame. */
    narrowTrack = trackselect_resolve_narrow_track(&snap, haveSnap);
    trackselect_autonarrow_vehicle(narrowTrack, occupied);
    witnessTrack = (narrowTrack != TS_NONE)
                       ? narrowTrack
                       : (u8) (sTs.host
                                   ? sTrackIds[(sTs.cursorCol * TS_ROWS) +
                                               (sTs.mode == MDKR_ONLINE_SCREEN_MODE_SINGLE
                                                    ? sTs.cursorRow
                                                    : 0u)]
                                   : 0u);
    mask = trackselect_track_mask(witnessTrack, occupied);

    trackselect_publish_intent(localSeatChar);

    trackselect_resolve_remote(&snap, haveSnap, localSeat, &rv);
    trackselect_render(&snap, haveSnap, localSeat, &rv);
    trackselect_witness(&snap, haveSnap, witnessTrack, mask);

    trackselect_test_reduce_and_script();

    sTs.ticks++;

    if (haveSnap && snap.phase != (uint8_t) MDKR_ONLINE_SCREEN_LOBBY_PHASE) {
        fprintf(stderr,
                "[online-trackselect] advance: lobby left LOBBY (phase=%u) -> "
                "boot race\n",
                (unsigned) snap.phase);
        return MDKR_ONLINE_TRACKSELECT_ADVANCE;
    }
    if (sTs.leave) {
        sTs.leave = 0u; /* edge: return LEAVE once, never shadow ADVANCE */
        /* B steps back ONE level -- to the native VEHICLE screen (the session owns
         * the back-stack), not straight to charselect. */
        fprintf(stderr, "[online-trackselect] back one level\n");
        return MDKR_ONLINE_TRACKSELECT_LEAVE;
    }
    return MDKR_ONLINE_TRACKSELECT_STAY;
}

/* ======================================================================== *
 * Headless test seam (beta + env gated; entirely inert in a normal run)
 *
 * Stands in for the launcher during a headless TRACKSELECT lane. It ADOPTS the
 * room the CHARSELECT seam converged (the lane runs both), then acts as a minimal
 * reducer that faithfully models the launcher: converge the local seat, apply the
 * host's SET_MODE/SET_CONFIG_TRACK/SET_CUP with the SAME change-detect the plan
 * dedupe uses AND the reducer's ready-clear (every member's ready drops on a
 * config change), re-assert the scripted joiner's ready, reconverge the host's
 * ready, and flip to LOADING on the host's start.
 *
 * it REFUSES a config_track that is not in sTrackIds (mirrors the reducer's
 * known_race_track), so a drifted screen id can never converge and the lane's
 * convergence assertion catches the drift.
 *
 * the CHARSELECT seam re-initialises ITS own room on a back-out re-entry, so
 * the snapshot this seam adopts on the next TRACKSELECT entry is the freshly
 * reconverged room -- there is no stale trackselect config carried across a
 * back-out (sTsAdopted resets in _enter -> trackselect_test_reset).
 * ======================================================================== */
static s8 sTsTestActive = -1; /* -1 unresolved, 0 off, 1 on */
static u8 sTsAdopted;
static MdkrPartyLinkSnapshot sTsRoom;
static u8 sTsStartArmed;

static void trackselect_test_resolve(void) {
    if (sTsTestActive < 0) {
        const char *e = getenv("MDKR_TEST_ONLINE_TRACKSELECT");
        sTsTestActive = (e != NULL) ? 1 : 0;
        /* Scenario from the env VALUE: "joiner" selects the joiner-render lane,
         * anything else is the default single-race host lane. */
        sTsScenario = (e != NULL && strstr(e, "joiner") != NULL)
                          ? (s8) TS_SCN_JOINER
                          : (s8) TS_SCN_SINGLE_HOST;
    }
}

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
        room.phase = (uint8_t) MDKR_ONLINE_SCREEN_LOBBY_PHASE;
        for (i = 0u; i < MDKR_PARTY_LINK_SEATS; i++) {
            room.seats[i].character_id = MDKR_ONLINE_SCREEN_NO_CHARACTER;
            room.seats[i].vehicle_id = MDKR_ONLINE_SCREEN_NO_VEHICLE;
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

/* does the offered screen accept this config_track? (mirrors the reducer's
 * known_race_track over sTrackIds). */
static u8 trackselect_test_known_track(u16 trackId) {
    return (u8) (trackselect_index_of(trackId) != TS_NONE ? 1u : 0u);
}

static void trackselect_test_reduce_and_script(void) {
    MdkrPartyLinkLocalIntent intent;
    u8 configChanged = 0u;

    trackselect_test_resolve();
    if (!sTsTestActive || !mdkr_party_link_active()) {
        return;
    }

    if (!sTsAdopted) {
        if (!mdkr_party_link_read(&sTsRoom)) {
            return;
        }
        if (sTsRoom.configured_track == 0u) {
            sTsRoom.configured_track = 0xFFFFu; /* normalise "none" */
        }
        if (sTsScenario == TS_SCN_JOINER) {
            /* Flip roles: the LOCAL seat becomes the JOINER and seat 1 the HOST,
             * so the screen exercises the joiner render/narrow paths. seat1's
             * vehicle is set legal for the cup we script (Whale Bay, hovercraft). */
            sTsRoom.seats[0].is_host = 0u;
            sTsRoom.seats[0].is_local = 1u;
            sTsRoom.seats[1].is_host = 1u;
            sTsRoom.seats[1].is_local = 0u;
            sTsRoom.seats[1].vehicle_id = (uint8_t) VEHICLE_HOVERCRAFT;
        }
        sTsAdopted = 1u;
    }

    /* JOINER scenario: the seam plays the REMOTE HOST -- it locks a tournament cup
     * (Sherbet, cup 2; round 0 == Whale Bay, hovercraft-only) so the joiner must
     * render the room and narrow its own vehicle to the cup's round-0 track
     *, then it starts. The local joiner publishes only its char/vehicle/
     * ready (no config), which we converge below. */
    if (sTsScenario == TS_SCN_JOINER) {
        u8 configChangedJ = 0u;
        if (mdkr_party_link_intent_poll(&intent)) {
            if (intent.confirmed && intent.hover_character < MDKR_ONLINE_SCREEN_CHAR_COUNT) {
                sTsRoom.seats[0].character_id = intent.hover_character;
            }
            if (intent.vehicle_id < MDKR_ONLINE_SCREEN_VEHICLE_COUNT) {
                sTsRoom.seats[0].vehicle_id = intent.vehicle_id;
            }
        }
        /* Scripted host action: lock cup 2 (tournament) once, mid-screen. */
        if (sTs.ticks == 8u &&
            (sTsRoom.mode != MDKR_ONLINE_SCREEN_MODE_TOURNAMENT || sTsRoom.cup_id != 2u)) {
            sTsRoom.mode = (uint8_t) MDKR_ONLINE_SCREEN_MODE_TOURNAMENT;
            sTsRoom.cup_id = 2u;
            sTsRoom.seats[0].ready = 0u; /* reducer clears all ready on config */
            sTsRoom.seats[1].ready = 0u;
            configChangedJ = 1u;
        }
        if (!configChangedJ) {
            sTsRoom.seats[1].ready = 1u; /* the host re-asserts ready */
            if (intent.ready &&
                sTsRoom.seats[0].character_id != MDKR_ONLINE_SCREEN_NO_CHARACTER &&
                sTsRoom.seats[0].vehicle_id != MDKR_ONLINE_SCREEN_NO_VEHICLE) {
                sTsRoom.seats[0].ready = 1u;
            }
        }
        /* Scripted host start once the cup is locked and both are ready. */
        if (sTs.ticks >= 24u && sTsRoom.cup_id == 2u &&
            sTsRoom.seats[0].ready && sTsRoom.seats[1].ready) {
            sTsStartArmed++;
            if (sTsStartArmed >= 6u) {
                sTsRoom.phase = (uint8_t) TS_LOADING_PHASE;
            }
        }
        mdkr_party_link_publish(&sTsRoom);
        return;
    }

    if (mdkr_party_link_intent_poll(&intent)) {
        if (intent.confirmed && intent.hover_character < MDKR_ONLINE_SCREEN_CHAR_COUNT) {
            sTsRoom.seats[0].character_id = intent.hover_character;
        }
        if (intent.vehicle_id < MDKR_ONLINE_SCREEN_VEHICLE_COUNT) {
            sTsRoom.seats[0].vehicle_id = intent.vehicle_id;
        }
        /* Host session config -- change-detected exactly like the plan dedupe;
         * each accepted change clears EVERY member's ready (lobby_core.c:756-757).
         * A config_track outside sTrackIds is REFUSED, never converged. */
        if (intent.mode != MDKR_PARTY_LINK_MODE_UNSET &&
            sTsRoom.mode != intent.mode) {
            sTsRoom.mode = intent.mode;
            configChanged = 1u;
        }
        if (intent.config_track != MDKR_PARTY_LINK_TRACK_UNSET &&
            sTsRoom.configured_track != intent.config_track &&
            trackselect_test_known_track(intent.config_track)) {
            sTsRoom.configured_track = intent.config_track;
            configChanged = 1u;
        }
        if (intent.cup_id != MDKR_PARTY_LINK_CUP_UNSET &&
            sTsRoom.cup_id != intent.cup_id) {
            sTsRoom.cup_id = intent.cup_id;
            configChanged = 1u;
        }
        if (configChanged) {
            sTsRoom.seats[0].ready = 0u;
            sTsRoom.seats[1].ready = 0u;
            sTsStartArmed = 0u;
        } else {
            sTsRoom.seats[1].ready = 1u; /* scripted joiner republishes ready */
            if (intent.ready &&
                sTsRoom.seats[0].character_id != MDKR_ONLINE_SCREEN_NO_CHARACTER &&
                sTsRoom.seats[0].vehicle_id != MDKR_ONLINE_SCREEN_NO_VEHICLE) {
                sTsRoom.seats[0].ready = 1u;
            } else if (intent.backout) {
                sTsRoom.seats[0].ready = 0u;
            }
        }

        {
            bool configReady =
                (sTsRoom.mode == MDKR_ONLINE_SCREEN_MODE_TOURNAMENT)
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

/* cup round -> track id from the authoritative sTrackIds mirror (track
 * index == cup*TS_ROWS + round). Kept here (not duplicated in online_session.c)
 * so there is a single engine-side copy of the accepted set. */
u16 mdkr_online_trackselect_cup_track(unsigned cup, unsigned round) {
    if (cup >= TS_COLS || round >= TS_ROWS) {
        return 0u;
    }
    return (u16) sTrackIds[(cup * TS_ROWS) + round];
}

#endif /* MDKR_ENABLE_ONLINE_BETA */
