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
#include "macros.h"    /* COLOUR_RGBA32 (the packed-colour texrect_draw_scaled takes) */
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
#include "online/online_vehicleselect.h" /* the per-round stage-confirm latch:
                                            the browse's ready publication */
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
#define TS_NONE 0xFFu

/* Framed-cell layout (retail wooden picture-frame browser, retail-trackselect-
 * browse.png). ONE big cell at a time: the hovered world name in BIGFONT up top,
 * a wooden picture frame around a per-world sky "postcard" in the middle, the
 * hovered track name in BIGFONT along the bottom, and one footer board holding
 * seats / status / controls. The old world-banner strip + 4-row text list are
 * gone -- the moving cursor now updates this single framed cell (the retail
 * one-big-cell read), keeping the world->track two-stage shape underneath. */
#define TS_WORLD_Y 24        /* world / cup name BIGFONT, top-centre */
#define TS_MODE_Y 42         /* small SINGLE RACE / TOURNAMENT line under it */
#define TS_FRAME_X0 86       /* wooden picture frame, outer rect */
#define TS_FRAME_Y0 52
#define TS_FRAME_X1 234
#define TS_FRAME_Y1 166
#define TS_FRAME_BORDER 12   /* wood border thickness (postcard inset) */
#define TS_TRACK_Y 182       /* hovered track name BIGFONT, bottom-centre */
#define TS_FOOT_X0 10        /* bottom board: seats / status / controls */
#define TS_FOOT_X1 310
#define TS_FOOT_Y0 190
#define TS_FOOT_Y1 236
#define TS_SEAT_Y 199        /* YOU / rival pair (charselect parity) */
#define TS_STATUS_Y 214
#define TS_HELP_Y 228

/* Menu SFX (the real DKR enums, same reuse as CHARSELECT). */
#define TS_SFX_MOVE SOUND_MENU_PICK2
#define TS_SFX_LOCK SOUND_SELECT2
#define TS_SFX_BACK SOUND_MENU_BACK3
/* Retail's rejected/unavailable-cell blip (menu.c menu_track_select uses
 * SOUND_UNK_6A for picking an unavailable track), replacing the non-retail
 * electric buzz for a blocked move. */
#define TS_SFX_REJECT SOUND_UNK_6A
#define TS_SFX_MODE SOUND_MENU_PICK2 /* distinct from the lock sound */

/* Retail T.T. track-name announcer debounce: mirrors menu.c's gTrackNameVoiceDelay
 * (counts UP once armed on a hover change; the voice fires at >= 7 then resets to
 * 0), so rapid browsing never stacks voice lines. */
#define TS_TT_VOICE_DELAY 7

#define TS_LOCK_FLASH_TICKS 15u /* short flash on the lock frame */
#define TS_VEH_FLASH_TICKS 20u  /* auto-narrow "vehicle changed" highlight */
/* JOINER browse dwell before following a lock that was ALREADY configured when
 * the screen entered (a pre-configured room / a re-front's stale pick). MUST
 * exceed the session's remote-vacate debounce (45 ticks) so a joiner whose host
 * vanished at the browse trips LEFT there instead of warping into the vehicle
 * stage on the stale pick. A FRESH lock is followed immediately (retail). */
#define TS_JOINER_STALE_FOLLOW_TICKS 70u

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

/* The hovered world / hub name drawn in BIGFONT along the top of the framed cell
 * (retail draws the hub name here, e.g. "DINO DOMAIN"). Display chrome only -- NOT
 * the drift-sensitive track set (track names come from level_name()); kept short so
 * the authored-art BIGFONT never overruns the 320px-wide logical frame. */
static const char *const sWorldBigNames[TS_COLS] = {
    "DINO DOMAIN", "SNOWFLAKE", "SHERBET ISLE", "DRAGON FOREST", "FUTURE FUN",
};
static const char *const sCupNames[TS_COLS] = {
    "DINO DOMAIN CUP", "SNOWFLAKE CUP", "SHERBET CUP", "DRAGON FOREST CUP",
    "FUTURE FUN CUP",
};

/* T.T. announcer voice table (menu.c gTTVoiceLines, indexed by track id; -1 == no
 * line). Non-static engine data, read-only borrow -- not declared in menu.h, so
 * mirror the extern here (same discipline as the gMenuAssets/gRacerPortraits
 * externs in online_screen_util.h). */
extern s16 gTTVoiceLines[53];

/* The wooden picture frame + directional arrows are the offline track select's own
 * menu textures, borrowed READ-ONLY from gMenuAssets exactly as the sky tiles and
 * vehicle icons are (menu_assetgroup_load routes each id through load_texture; every
 * id verified in menu.h: TEXTURE_SURFACE_BUTTON_WOOD 0x43, the four arrow icons
 * 0x3C..0x3F). A screen-owned group, freed on _exit -- kept SEPARATE from the shared
 * sOnlineSkyAssetIds so the other native screens never load the frame art. The real
 * offline frame (gMenuImages[7] via menu_element_render / func_80080580) is NOT
 * borrowable here: both need gMenuImages / gMenuGeometry, which only the offline menu
 * init allocates (they are NULL in the separated-boot path), so we texrect the same
 * wood + arrow textures directly -- the fallback the spec calls out. */
static s16 sTrackselectFrameAssetIds[] = {
    TEXTURE_SURFACE_BUTTON_WOOD,
    TEXTURE_ICON_ARROW_UP,
    TEXTURE_ICON_ARROW_DOWN,
    TEXTURE_ICON_ARROW_LEFT,
    TEXTURE_ICON_ARROW_RIGHT,
    -1,
};
static TextureHeader *sWoodTex;      /* frame wood-grain tile */
static TextureHeader *sArrowTex[4];  /* 0 up, 1 down, 2 left, 3 right */

/* Vehicle names for the always-on VEHICLE line. Defined here and shared with
 * VEHICLESELECT via online_trackselect.h (one table -- both screens label
 * identically). */
const char *const mdkr_online_vehicle_names[MDKR_ONLINE_SCREEN_VEHICLE_COUNT] = {
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
    u8 setupReady;   /* the browse stage is done: host locked a pick (A), or the
                      * joiner observed the host's lock in the snapshot. The
                      * session reads this to advance to the VEHICLE stage of the
                      * track screen (retail order: vehicles AFTER the track). */
    u8 entryLockMode;   /* joiner stale-lock latch: the lock already visible at
                         * _enter (mode + track/cup), so a PRE-EXISTING config
                         * (a pre-configured room / a results re-front's stale
                         * pick) is followed only after a short browse dwell,
                         * while a FRESH host lock is followed immediately
                         * (retail's zoom-into-setup). 0xFF == none at entry. */
    u16 entryLockTrack;
    u8 entryLockCup;
    u8 assets;       /* world bg group + fonts loaded */
    u8 leave;        /* B: back-to-charselect request (edge; see tick) */
    u32 ticks;       /* TRACKSELECT ticks elapsed (also drives the test input) */
    u32 lockFlashEnd;/* "LOCKED" flash + remote-ready hold deadline, in ticks */
    u32 vehFlashEnd; /* auto-narrow "vehicle changed" highlight deadline */
    s32 ttVoiceDelay;/* retail T.T. announcer debounce (gTrackNameVoiceDelay): 0
                      * idle, else counts up until the hovered track's voice fires */
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

/* The retail 2-player picker narrowing table (menu.c menu_track_select V79+): at
 * 2+ players each listed track drops one vehicle from the usable mask. Shared by
 * TRACKSELECT + VEHICLESELECT (declared in online_trackselect.h) so both narrow
 * from the SAME data -- extend here in one place, never per screen. */
u8 mdkr_online_trackselect_narrow_2p(u8 mask, u16 trackId, unsigned occupied) {
    static const struct {
        u16 track;  /* track whose usable set shrinks at 2+ players */
        u8 dropBit; /* the vehicle bit removed from the mask */
    } narrow[] = {
        {MDKR_ONLINE_SCREEN_TRACK_SPACEPORT_ALPHA, (u8) (1u << VEHICLE_HOVERCRAFT)},
        {MDKR_ONLINE_SCREEN_TRACK_FROSTY_VILLAGE, (u8) (1u << VEHICLE_PLANE)},
    };
    unsigned i;
    if (occupied < 2u) {
        return mask;
    }
    for (i = 0u; i < sizeof(narrow) / sizeof(narrow[0]); i++) {
        if (trackId == narrow[i].track) {
            mask = (u8) (mask & (u8) ~narrow[i].dropBit);
        }
    }
    return mask;
}

/* The usable-vehicle mask for a track at this player count: engine truth from
 * leveltable_vehicle_usable(), then the retail 2-player narrowing. */
static u8 trackselect_track_mask(u8 trackId, unsigned occupied) {
    u8 mask = (u8) leveltable_vehicle_usable((s32) trackId);
    if (mask == 0u) {
        mask = (u8) (1u << VEHICLE_CAR); /* fail-safe: never empty */
    }
    return mdkr_online_trackselect_narrow_2p(mask, trackId, occupied);
}

/* The vehicle mask legal for EVERY round of a TOURNAMENT cup: the AND of all
 * TS_ROWS rounds' engine-truth masks (leveltable_vehicle_usable + the retail
 * 2-player narrowing). A tournament persists ONE vehicle across all rounds (the
 * resident coordinator re-Readies + STARTs each round with the SAME seat vehicle;
 * there is no per-round re-selection), so the picked vehicle MUST be legal for the
 * whole cup. Narrowing the pick against only the round-0 track let a car chosen
 * for cup-0 round-0 (Ancient Lake, all vehicles) survive to round-3 Hot Top
 * Volcano (hovercraft/plane only, mask 0x6), where the reducer's all_vehicles_legal
 * gate REJECTS BEGIN_LOADING (ILLEGAL_VEHICLE) -> the round never boots and the
 * tournament stalls at raceCount=3. Every cup's 2-player intersection is non-empty
 * (cup0 0x6, cup1 0x3, cup2 0x2, cup3 0x2, cup4 0x1), so a legal pick always
 * exists. */
u8 mdkr_online_trackselect_cup_vehicle_mask(unsigned cup, unsigned occupied) {
    const u8 all = (u8) ((1u << MDKR_ONLINE_SCREEN_VEHICLE_COUNT) - 1u);
    u8 mask = all;
    unsigned r;
    if (cup >= TS_COLS) {
        return all; /* unknown cup: nothing cup-wide to clamp to yet */
    }
    for (r = 0u; r < TS_ROWS; r++) {
        mask &= trackselect_track_mask(sTrackIds[(cup * TS_ROWS) + r], occupied);
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

/* The vehicle mask the local seat's pick must stay inside this frame. A
 * TOURNAMENT clamps to the whole locked/hovered cup's INTERSECTION (the pick
 * persists across every round, so it must be legal for all of them -- see
 * mdkr_online_trackselect_cup_vehicle_mask); a single race clamps to the resolved
 * track. When nothing is resolved yet return ALL (keep the seed vehicle). */
static u8 trackselect_resolve_narrow_mask(const MdkrPartyLinkSnapshot *snap,
                                          bool haveSnap, unsigned occupied) {
    const u8 all = (u8) ((1u << MDKR_ONLINE_SCREEN_VEHICLE_COUNT) - 1u);
    if (sTs.mode == MDKR_ONLINE_SCREEN_MODE_TOURNAMENT) {
        u8 cup = TS_NONE;
        if (sTs.host) {
            /* locked cup once locked, else the hovered cup preview. */
            cup = (sTs.lockedCup != TS_NONE) ? sTs.lockedCup : (u8) sTs.cursorCol;
        } else if (haveSnap && snap->cup_id < TS_COLS) {
            cup = (u8) snap->cup_id; /* joiner follows the host's locked cup */
        }
        if (cup >= TS_COLS) {
            return all; /* cup not known yet: keep the seed */
        }
        return mdkr_online_trackselect_cup_vehicle_mask(cup, occupied);
    }
    {
        u8 trackId = trackselect_resolve_narrow_track(snap, haveSnap);
        if (trackId == TS_NONE) {
            return all; /* nothing to narrow against yet */
        }
        return trackselect_track_mask(trackId, occupied);
    }
}

/* keep the local seat's vehicle inside `mask` (lowest legal bit when the current
 * one is illegal). Flags a change so the VEHICLE line can flash. An ALL mask (no
 * lock resolved) leaves any valid seed vehicle untouched. */
static void trackselect_autonarrow_vehicle(u8 mask) {
    u8 v;
    if (mask == 0u) {
        return; /* nothing legal to clamp to (malformed table): keep as-is */
    }
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
    u8 aEdge;     /* A: lock track/cup -> the vehicle stage (retail track pick) */
    u8 bEdge;     /* B: back one level (to the native CHARSELECT) */
    u8 modeEdge;  /* Z: toggle single/tournament (host) */
} TsInput;

/* Headless scenario, from the env VALUE (see trackselect_test_resolve):
 *   0 SINGLE_HOST (default) -- host locks a single track; also the B-back lane.
 *   1 JOINER          -- local seat is a JOINER; the seam scripts a remote HOST
 *                        locking a tournament cup, to prove the joiner renders the
 *                        room and narrows to the cup's round-0 track.
 *   2 HOLD            -- frame-dump only (the VS_SCN_HOLD sibling): lock a track,
 *                        browse away, then park with no START so a shot can be
 *                        taken of the fully revealed screen.
 *   3 REMATCH         -- local seat is a JOINER on a SAME-TRACK rematch re-front:
 *                        the room enters carrying LAST round's config + persisted
 *                        seats (REMATCH's clear_round drops only ready), and the
 *                        scripted remote HOST presses OK while the joiner is
 *                        still inside its stale-lock browse dwell. Proves the
 *                        per-player CONFIRM cannot be bypassed: the OK must be
 *                        refused NOT_READY until the joiner's vehicle stage
 *                        fronts and confirms. */
#define TS_SCN_SINGLE_HOST 0
#define TS_SCN_JOINER 1
#define TS_SCN_HOLD 2
#define TS_SCN_REMATCH 3
/* LOCK-IN-FADE (regression arm for the exit-fade-hold strand): host, single mode,
 * IDENTICAL reducer/vehicle-stage machinery to SINGLE_HOST -- only the entry-1
 * input differs. Entry 1 arms the retail exit fade with a browse-B, then presses A
 * to LOCK the track while the 18-tick veil is still held. The session's STAY+lock
 * branch must cancel the armed veil (else it strands black over the vehicle stage
 * and poisons the next vehicle->trackselect B-back). */
#define TS_SCN_LOCKFADE 4
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
    if (sTsScenario == TS_SCN_JOINER || sTsScenario == TS_SCN_REMATCH) {
        return; /* joiner: watch only, the seam drives the host */
    }
    if (sTsScenario == TS_SCN_HOLD) {
        /* Dump seam: walk to Whale Bay (col2/row0) and PARK there browsing (a
         * lock now flips straight into the vehicle stage, so the browse dump
         * must NOT lock; the vehicle-stage dumps come from the VEHICLESELECT
         * hold seam instead), then browse away to the FFL column so a later dump
         * catches a second world's postcard too. */
        switch (sTs.ticks) {
        case 2u:
        case 3u:
            in->dx = 1;
            break;
        case 40u:
        case 41u:
            in->dx = 1;
            break;
        default:
            break;
        }
        return;
    }
    if (sTsScenario == TS_SCN_LOCKFADE) {
        /* LOCK-IN-FADE regression arm. Entry 1: walk to Whale Bay (col2/row0),
         * ARM the retail exit fade with a browse-B (tick 5 -> the session holds
         * the 18-tick veil), then LOCK the track with A (tick 10) WHILE the veil
         * is still held -- the session's STAY+lock branch must cancel the armed
         * veil rather than strand it. The A lands ~5 ticks into the 18-tick hold,
         * comfortably inside it. Entry 2+ (back from the vehicle stage's B):
         * re-lock the restored Whale Bay cell so the flow confirms + boots (proving
         * the veil never poisoned the vehicle->trackselect B-back either). */
        if (sTsEntryCount <= 1u) {
            switch (sTs.ticks) {
            case 2u:
                in->dx = 1; /* col 0 -> 1 */
                break;
            case 3u:
                in->dx = 1; /* col 1 -> 2 (Sherbet); row 0 == Whale Bay (track 8) */
                break;
            case 5u:
                in->bEdge = 1u; /* arm the exit fade (browse-B -> 18-tick veil hold) */
                break;
            case 10u:
                in->aEdge = 1u; /* LOCK inside the hold -> STAY+lock w/ veil armed */
                break;
            default:
                break;
            }
            return;
        }
        if (sTs.ticks == 2u) {
            in->aEdge = 1u; /* re-lock the restored cell -> vehicle stage -> boot */
        }
        return;
    }
    /* Retail-order choreography (the lane walks the whole track screen):
     *   entry 1: B at tick 3 -- the browse-stage back-out steps to CHARSELECT
     *            (whose seam re-confirms + re-readies and hands back here);
     *   entry 2: walk to Whale Bay (col2/row0, track 8, hovercraft-only) and
     *            LOCK it -- the session flips to the vehicle stage, whose own
     *            script Bs back once (the stage back-stack proof) ...
     *   entry 3: ... so re-lock the restored cursor cell (Whale Bay again) and
     *            hand the flow forward for good (vehicle confirm + host OK). */
    if (sTsEntryCount <= 1u) {
        if (sTs.ticks == 3u) {
            in->bEdge = 1u;
        }
        return;
    }
    if (sTsEntryCount == 2u) {
        switch (sTs.ticks) {
        case 2u:
            in->dx = 1; /* col 0 -> 1 */
            break;
        case 3u:
            in->dx = 1; /* col 1 -> 2 (Sherbet); row 0 == Whale Bay (track 8) */
            break;
        case 6u:
            in->aEdge = 1u; /* lock track 8 -> SET_CONFIG_TRACK(8) + vehicle stage */
            break;
        default:
            break;
        }
        return;
    }
    /* entry 3+ (back from the vehicle stage's B): cursor restored to the last
     * locked cell -- re-lock it. */
    if (sTs.ticks == 2u) {
        in->aEdge = 1u;
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

    /* START picks like A (retail lets either commit the track cell). */
    in->aEdge = (pressed & (A_BUTTON | START_BUTTON)) ? 1u : 0u;
    in->bEdge = (pressed & B_BUTTON) ? 1u : 0u;
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
        in->aEdge = 1u; /* lock track index 1 -> id 3 (SET_CONFIG_TRACK clears
                         * ready) -> the vehicle stage; the host START moved to
                         * that stage's script (retail order) */
        break;
    default:
        break;
    }
}

/* minimal scripted input for the VEHICLESELECT-seam lanes that do NOT arm this
 * screen's own seam (diverge/unknown scenarios): the retail-order flow passes
 * the track browse BEFORE the vehicle stage now, so those lanes need a lock to
 * reach it -- a single A on the entry cell. The vehicle seam then ADOPTS the
 * room and pins its own scenario track, so which cell is locked is immaterial.
 * Resolved once; inert without the env. */
static s8 sTsVsLaneInput = -1; /* -1 unresolved, 0 off, 1 on */
static u8 trackselect_vslane_input_active(void) {
    if (sTsVsLaneInput < 0) {
        sTsVsLaneInput =
            (getenv("MDKR_TEST_ONLINE_VEHICLESELECT") != NULL) ? 1 : 0;
    }
    return (u8) (sTsVsLaneInput > 0 ? 1 : 0);
}

static void trackselect_input_vslane(TsInput *in) {
    memset(in, 0, sizeof(*in));
    if (sTs.ticks == 2u) {
        in->aEdge = 1u; /* lock the entry cell -> the vehicle stage */
    }
}

static void trackselect_gather_input(TsInput *in) {
    if (mdkr_online_trackselect_test_active()) {
        trackselect_input_scripted(in);
    } else if (trackselect_lobby_input_active()) {
        trackselect_input_lobby_start(in);
    } else if (trackselect_vslane_input_active()) {
        trackselect_input_vslane(in);
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
        sTs.cursorRow = 0u;
        sound_play(TS_SFX_MODE, NULL); /* distinct from the lock sound */
        return;
    }

    if (in->dx != 0) {
        s32 col = (s32) sTs.cursorCol + in->dx;
        col = (col + TS_COLS) % TS_COLS; /* columns wrap (native DKR 2D menus) */
        if ((u8) col != sTs.cursorCol) {
            sTs.cursorCol = (u8) col;
            sTs.ttVoiceDelay = 1; /* re-arm the T.T. announcer on the hover change */
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
            sTs.ttVoiceDelay = 1; /* re-arm the T.T. announcer on the hover change */
            sound_play(TS_SFX_MOVE, NULL);
        } else {
            /* A row move blocked at the grid edge (no up/down arrow was shown for
             * it): the retail rejected-cell blip, paired with the missing arrow. */
            sound_play(TS_SFX_REJECT, NULL);
        }
    }

    if (in->aEdge) {
        /* Retail order: picking the track/cup IS the stage advance -- the same A
         * that locks the pick hands the screen to the vehicle stage (the session
         * reads the setupReady latch below). No separate host START lives here
         * any more; the race start moved to the vehicle stage's OK beat, exactly
         * where retail puts it (after every seat's vehicle is confirmed). */
        if (sTs.mode == MDKR_ONLINE_SCREEN_MODE_SINGLE) {
            sTs.lockedTrack = (u8) ((sTs.cursorCol * TS_ROWS) + sTs.cursorRow);
            sLastLockedTrack = sTs.lockedTrack; /* persistence */
        } else {
            sTs.lockedCup = sTs.cursorCol;
            sLastLockedCup = sTs.lockedCup; /* persistence */
        }
        sLastMode = sTs.mode;
        sTs.setupReady = 1u; /* the session advances to the vehicle stage */
        sTs.lockFlashEnd = sTs.ticks + TS_LOCK_FLASH_TICKS;
        sound_play(TS_SFX_LOCK, NULL);
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
    /* Ready latches ONLY via the vehicle stage's A-confirm, per round (the
     * retail per-player CONFIRM). The browse publishes the stage's per-round
     * confirm latch -- 0 until the LOCAL seat has confirmed on THIS round's
     * stage -- and emits the un-ready otherwise (backout -> CHANGE_SELECTION;
     * ready=0 alone plans nothing, party_link.c). An unconditional ready=1
     * here let a rematch re-front re-latch a stale ready: a re-lock of the
     * IDENTICAL track is no config change, so the reducer never ready-clears
     * (lobby_core.c:756) and the host's OK could BEGIN_LOADING while the other
     * seat was still browsing -- race-booted without its stage ever fronting.
     * The planner absorbs the interim: an OK before every stage confirm is
     * refused NOT_READY and re-fired off the refusal note once the confirm
     * lands (party_link.h:229-241). */
    {
        u8 stageReady = mdkr_online_vehicleselect_stage_confirmed_round();
        intent.ready = stageReady;
        intent.backout = stageReady ? 0u : 1u;
    }

    if (sTs.host) {
        intent.mode = sTs.mode; /* host always drives the mode */
        if (sTs.mode == MDKR_ONLINE_SCREEN_MODE_SINGLE && sTs.lockedTrack != TS_NONE) {
            intent.config_track = sTrackIds[sTs.lockedTrack];
        } else if (sTs.mode == MDKR_ONLINE_SCREEN_MODE_TOURNAMENT && sTs.lockedCup != TS_NONE) {
            intent.cup_id = sTs.lockedCup;
        }
        /* start_requested stays 0 here: the race start belongs to the VEHICLE
         * stage's OK beat now (retail order -- START_RACE fires only after both
         * seats confirmed their vehicles). */
    }
    mdkr_party_link_intent_publish(&intent);
}

/* ======================================================================== *
 * Render (retail wooden picture-frame browser: one big framed cell)
 * ======================================================================== */

/* The hovered track id (engine track id, not the 0..19 index): the cursor's
 * cell in single race, the cup's round-0 track in tournament. Used by both the
 * bottom BIGFONT name and the T.T. announcer. */
static u16 trackselect_hovered_track_id(void) {
    unsigned round = (sTs.mode == MDKR_ONLINE_SCREEN_MODE_SINGLE) ? sTs.cursorRow : 0u;
    return (u16) sTrackIds[(sTs.cursorCol * TS_ROWS) + round];
}

/* Copy `src` into `dst` upper-cased (A..Z), bounded. The authored-art BIGFONT is
 * an all-caps face; level_name() returns mixed case ("Ancient Lake"), so the
 * bottom track name is upper-cased to render (and to read as retail's all-caps
 * name art). */
static void trackselect_upper(const char *src, char *dst, u32 cap) {
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

/* Draw one directional arrow icon centred on (cx, cy) (retail blue triangles on
 * the frame edges). Modulated white so the authored blue shows; a no-op if the
 * icon is not resident (same fail-safe as the portrait / vehicle blits). */
static void trackselect_draw_arrow(u8 dir, s32 cx, s32 cy) {
    TextureHeader *tex;
    if (dir >= 4u) {
        return;
    }
    tex = sArrowTex[dir];
    if (tex == NULL) {
        return;
    }
    mdkr_online_screen_blit(tex, cx - (s32) tex->width / 2,
                            cy - (s32) tex->height / 2, 255, 255, 255, 255);
}

/* Draw the wooden picture frame around a per-world sky "postcard". The wood tile
 * is stretched over the whole outer rect, then the focused world's sky tile is
 * stretched over the inset preview area on top, leaving a wood border -- so the
 * cell reads as a framed window into that world (the honest static-postcard
 * mimicry of retail's live fly-through, which is the forbidden level-load class).
 * Fails safe: no wood -> just the postcard; no sky -> a dark inner fill. */
static void trackselect_draw_frame(u8 world) {
    TextureHeader *sky = (world < TS_COLS) ? sCupBgTopTex[world] : NULL;
    s32 ix0 = TS_FRAME_X0 + TS_FRAME_BORDER;
    s32 iy0 = TS_FRAME_Y0 + TS_FRAME_BORDER;
    s32 ix1 = TS_FRAME_X1 - TS_FRAME_BORDER;
    s32 iy1 = TS_FRAME_Y1 - TS_FRAME_BORDER;

    if (sWoodTex != NULL && sWoodTex->width != 0 && sWoodTex->height != 0) {
        mdkr_online_screen_blit_scaled(
            sWoodTex, (f32) TS_FRAME_X0, (f32) TS_FRAME_Y0,
            (f32) (TS_FRAME_X1 - TS_FRAME_X0) / (f32) sWoodTex->width,
            (f32) (TS_FRAME_Y1 - TS_FRAME_Y0) / (f32) sWoodTex->height,
            COLOUR_RGBA32(255, 255, 255, 255));
    }

    if (sky != NULL && sky->width != 0 && sky->height != 0) {
        mdkr_online_screen_blit_scaled(sky, (f32) ix0, (f32) iy0,
                                       (f32) (ix1 - ix0) / (f32) sky->width,
                                       (f32) (iy1 - iy0) / (f32) sky->height,
                                       COLOUR_RGBA32(255, 255, 255, 255));
    } else {
        mdkr_online_screen_card(ix0, iy0, ix1, iy1, 8, 12, 32, 255);
    }
}

/* Retail T.T. track-name announcer (menu.c gTrackNameVoiceDelay, host only -- the
 * joiner has no cursor). Armed to 1 on every hover change; here it counts up by
 * the update rate and, once it reaches TS_TT_VOICE_DELAY, plays the hovered
 * track's voice line (guarding the -1 no-line entries) and resets to 0 so a
 * settled cell announces exactly once and rapid browsing never stacks lines. */
static void trackselect_tt_announce(s32 rate) {
    u16 tid;
    s16 voice;
    /* SINGLE mode only: a tournament cell picks a CUP, and announcing the cup's
     * round-0 track name over it misnames the selection (retail's announcer only
     * ever voices a hovered TRACK). */
    if (!sTs.host || sTs.mode != MDKR_ONLINE_SCREEN_MODE_SINGLE ||
        sTs.ttVoiceDelay == 0) {
        return;
    }
    sTs.ttVoiceDelay += (rate > 0) ? rate : 1;
    if (sTs.ttVoiceDelay < TS_TT_VOICE_DELAY) {
        return;
    }
    tid = trackselect_hovered_track_id();
    voice = (tid < 53u) ? gTTVoiceLines[tid] : (s16) -1;
    if (voice != (s16) -1) {
        sound_play((u16) voice, NULL);
    }
    sTs.ttVoiceDelay = 0;
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
    /* Retail selected-item cadence (menu.c gOptionBlinkTimer: 0x3F wrap, *8
     * triangle, 0..255) via the shared helper -- the SAME blink charselect /
     * vehicleselect / results use, so the "<host> IS CHOOSING..." status pulses at
     * the authentic DKR rate (was the faster/dimmer 0..16 mdkr_online_screen_pulse).
     * Folded /16 back into the same 0..15 amplitude the old pulse fed the status
     * colour (150 + tri*4), so only the CADENCE changes, not the status line. */
    s32 tri = mdkr_online_screen_blink(sTs.ticks) / 16;
    u8 focusWorld;
    u8 lockedTrackIdx = TS_NONE; /* which of the 20 is the effective lock */
    u8 lockedCup = TS_NONE;
    u8 localReady = 0u;
    u8 cellTrackIdx;            /* the framed cell's track INDEX (0..19) */
    char nameBuf[32];          /* upper-cased track name for the bottom BIGFONT */
    const char *worldName;
    const char *bottomName;
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

    /* Resolve the framed cell's track INDEX + the two BIGFONT names. Host: the
     * cursor's cell (single) / the focused cup (tournament). Joiner (no cursor):
     * the host's locked cell, else the focused world's round 0 as a preview. */
    if (host) {
        cellTrackIdx = (u8) (focusWorld * TS_ROWS +
                             (effMode == MDKR_ONLINE_SCREEN_MODE_SINGLE ? sTs.cursorRow : 0u));
    } else {
        cellTrackIdx = (lockedTrackIdx != TS_NONE) ? lockedTrackIdx
                                                   : (u8) (focusWorld * TS_ROWS);
    }
    worldName = sWorldBigNames[focusWorld];
    if (effMode == MDKR_ONLINE_SCREEN_MODE_SINGLE) {
        trackselect_upper(level_name((s32) sTrackIds[cellTrackIdx]), nameBuf,
                          sizeof(nameBuf));
        bottomName = nameBuf;
    } else {
        /* Tournament: name the hovered/locked CUP itself (e.g. "SHERBET CUP"),
         * not a generic literal -- the cup IS the pick on this screen. */
        bottomName = sCupNames[focusWorld];
    }

    /* Ground: ONLY the bottom seats/status/controls board now. The framed cell +
     * its two BIGFONT names carry the retail figure-ground themselves (BIGFONT is
     * authored outlined art, legible straight over the scrolling sky, exactly like
     * retail's "DINO DOMAIN" / "ANCIENT LAKE" names float on the paper). */
    mdkr_online_screen_panel(TS_FOOT_X0, TS_FOOT_Y0, TS_FOOT_X1, TS_FOOT_Y1);

    /* World / hub name BIGFONT top + a small mode line under it (Z: MODE feedback --
     * retail has no mode here, but the online single/tournament toggle needs a
     * readable state). BIGFONT is authored gold/blue art (the colour args are
     * untinted -- see mdkr_online_screen_text), so lock/hover accent lives on the
     * status line below, not on these names. */
    mdkr_online_screen_text(MDKR_ONLINE_SCREEN_W_HALF, TS_WORLD_Y, ASSET_FONTS_BIGFONT,
                     (char *) worldName, ALIGN_MIDDLE_CENTER, 255, 224, 96);
    mdkr_online_screen_text(MDKR_ONLINE_SCREEN_W_HALF, TS_MODE_Y, ASSET_FONTS_SMALLFONT,
                     effMode == MDKR_ONLINE_SCREEN_MODE_SINGLE ? "SINGLE RACE" : "TOURNAMENT",
                     ALIGN_MIDDLE_CENTER, 210, 210, 210);

    /* The framed cell: retail wooden picture frame around the focused world's sky
     * postcard -- the one-big-cell read that replaces the banner strip + text list.
     * The moving cursor updates THIS cell (its names + postcard), keeping the
     * world->track two-stage shape + 4-track paging underneath. */
    trackselect_draw_frame(focusWorld);

    /* Directional arrows on the frame edges for the LEGAL moves only (host only --
     * the joiner has no cursor). Columns wrap so L/R are always available; single-
     * race rows clamp 0..3 so up/down show only when that move exists; tournament
     * has no per-round move. */
    if (host) {
        s32 midX = (TS_FRAME_X0 + TS_FRAME_X1) / 2;
        s32 midY = (TS_FRAME_Y0 + TS_FRAME_Y1) / 2;
        trackselect_draw_arrow(2u, TS_FRAME_X0 + TS_FRAME_BORDER / 2, midY); /* left */
        trackselect_draw_arrow(3u, TS_FRAME_X1 - TS_FRAME_BORDER / 2, midY); /* right */
        if (effMode == MDKR_ONLINE_SCREEN_MODE_SINGLE) {
            if (sTs.cursorRow > 0u) {
                trackselect_draw_arrow(0u, midX, TS_FRAME_Y0 + TS_FRAME_BORDER / 2); /* up */
            }
            if (sTs.cursorRow < TS_ROWS - 1u) {
                trackselect_draw_arrow(1u, midX, TS_FRAME_Y1 - TS_FRAME_BORDER / 2); /* down */
            }
        }
    }

    /* Hovered track / cup name BIGFONT along the bottom (retail's big name art). */
    mdkr_online_screen_text(MDKR_ONLINE_SCREEN_W_HALF, TS_TRACK_Y, ASSET_FONTS_BIGFONT,
                     (char *) bottomName, ALIGN_MIDDLE_CENTER, 255, 224, 96);

    /* Status line: names the pick + reflects the START / host-choosing state so a
     * deferred/refused start is never a dead screen. The old always-on
     * "VEHICLE: X" line is gone (the vehicle screen already owns that fact --
     * one status line suffices); the auto-narrow "your vehicle changed" moment
     * still surfaces here as a transient gold flash below. */
    {
        bool vehFlash = sTs.ticks < sTs.vehFlashEnd;
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
            } else if (vehFlash) {
                const char *vn = sTs.vehicle < MDKR_ONLINE_SCREEN_VEHICLE_COUNT
                                     ? mdkr_online_vehicle_names[sTs.vehicle]
                                     : "CAR";
                (void) snprintf(line, sizeof(line), "VEHICLE CHANGED: %s", vn);
                mdkr_online_screen_text(MDKR_ONLINE_SCREEN_W_HALF, TS_STATUS_Y,
                                 ASSET_FONTS_SMALLFONT, line, ALIGN_MIDDLE_CENTER,
                                 255, 224, 96);
            } else if (haveLock) {
                (void) snprintf(line, sizeof(line), "HOST PICKED: %s", pick);
                mdkr_online_screen_text(MDKR_ONLINE_SCREEN_W_HALF, TS_STATUS_Y,
                                 ASSET_FONTS_SMALLFONT, line, ALIGN_MIDDLE_CENTER,
                                 255, 224, 96);
            } else {
                (void) snprintf(line, sizeof(line), "%s IS CHOOSING...",
                                hostName);
                mdkr_online_screen_text(MDKR_ONLINE_SCREEN_W_HALF, TS_STATUS_Y,
                                 ASSET_FONTS_SMALLFONT, line, ALIGN_MIDDLE_CENTER,
                                 150 + tri * 4, 150 + tri * 4, 150 + tri * 4);
            }
        } else if (vehFlash) {
            const char *vn = sTs.vehicle < MDKR_ONLINE_SCREEN_VEHICLE_COUNT
                                 ? mdkr_online_vehicle_names[sTs.vehicle]
                                 : "CAR";
            (void) snprintf(line, sizeof(line), "VEHICLE CHANGED: %s", vn);
            mdkr_online_screen_text(MDKR_ONLINE_SCREEN_W_HALF, TS_STATUS_Y,
                             ASSET_FONTS_SMALLFONT, line, ALIGN_MIDDLE_CENTER,
                             255, 224, 96);
        } else if (haveLock) {
            /* Gold, not green: a LOCK is the committed selection accent, not a
             * go/READY cue (green stays reserved for READY/STARTING). Only
             * visible for the flip frame(s) now -- the lock advances straight to
             * the vehicle stage (retail order). */
            (void) snprintf(line, sizeof(line), "%s LOCKED", pick);
            mdkr_online_screen_text(MDKR_ONLINE_SCREEN_W_HALF, TS_STATUS_Y, ASSET_FONTS_SMALLFONT,
                             line, ALIGN_MIDDLE_CENTER, 255, 224, 96);
        } else {
            mdkr_online_screen_text(MDKR_ONLINE_SCREEN_W_HALF, TS_STATUS_Y, ASSET_FONTS_SMALLFONT,
                             effMode == MDKR_ONLINE_SCREEN_MODE_SINGLE ? "CHOOSE A TRACK"
                                                       : "CHOOSE A CUP",
                             ALIGN_MIDDLE_CENTER, 220, 220, 220);
        }
    }

    /* always-visible seat presence/ready pair (charselect family). Hold the
     * displayed READY through the lock-flash window so the reducer's 1-2-frame
     * ready-clear does not flicker CHOOSING. The local un-ready label is
     * CHOOSING (the family vocabulary): un-ready IS the browse's steady state
     * -- ready latches only at the vehicle stage's confirm, per round. */
    {
        bool holdReady = sTs.ticks < sTs.lockFlashEnd;
        bool youReady = localReady || holdReady;
        (void) snprintf(line, sizeof(line), "YOU: %s",
                        youReady ? "READY" : "CHOOSING");
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
        /* A picks the track/cup and moves straight into the vehicle stage
         * (retail order); the start prompt lives there now. */
        mdkr_online_screen_text(MDKR_ONLINE_SCREEN_W_HALF, TS_HELP_Y, ASSET_FONTS_SMALLFONT,
                         "A: SELECT   Z: MODE   B: BACK",
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
          ((u32) sTs.setupReady << 29) | ((u32) snapMode << 30);
    if (key == sWitnessKey) {
        return;
    }
    sWitnessKey = key;

    fprintf(stderr,
            "[online-trackselect] render mode=%u col=%u row=%u host=%u "
            "track=%u mask=0x%x vehicle=%u locked{track=%d cup=%d} "
            "seat{r0=%u r1=%u} snap{mode=%u cfgTrack=%u cup=%u phase=%u} "
            "setup=%u\n",
            (unsigned) sTs.mode, (unsigned) sTs.cursorCol,
            (unsigned) sTs.cursorRow, (unsigned) sTs.host,
            (unsigned) resolvedTrack, (unsigned) mask, (unsigned) sTs.vehicle,
            sTs.lockedTrack != TS_NONE ? (int) sTrackIds[sTs.lockedTrack] : -1,
            sTs.lockedCup != TS_NONE ? (int) sTs.lockedCup : -1,
            (unsigned) seat0Ready, (unsigned) seat1Ready, (unsigned) snapMode,
            (unsigned) snapCfg, (unsigned) snapCup, (unsigned) snapPhase,
            (unsigned) sTs.setupReady);
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

    /* latch the lock already visible at entry (a pre-configured room / a
     * re-front's stale pick) so the JOINER stage-follow can tell a FRESH host
     * lock (follow immediately) from a stale one (browse dwell first). */
    sTs.entryLockMode = 0xFFu;
    sTs.entryLockTrack = 0xFFFFu;
    sTs.entryLockCup = 0xFFu;
    {
        MdkrPartyLinkSnapshot esnap;
        if (mdkr_party_link_read(&esnap)) {
            if (esnap.mode == MDKR_ONLINE_SCREEN_MODE_TOURNAMENT) {
                if (esnap.cup_id < TS_COLS) {
                    sTs.entryLockMode = MDKR_ONLINE_SCREEN_MODE_TOURNAMENT;
                    sTs.entryLockCup = esnap.cup_id;
                }
            } else if (esnap.configured_track != 0xFFFFu &&
                       trackselect_index_of(esnap.configured_track) != TS_NONE) {
                sTs.entryLockMode = MDKR_ONLINE_SCREEN_MODE_SINGLE;
                sTs.entryLockTrack = esnap.configured_track;
            }
        }
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
    sTs.ttVoiceDelay = 1; /* arm the T.T. announcer so the entry cell announces once
                           * it settles (retail arms gTrackNameVoiceDelay on enter) */

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

    /* Frame wood + directional arrow art: a screen-owned group (deliberately kept
     * out of the shared sky group so the other native screens never load it). Bind
     * the resolved TextureHeader*s from gMenuAssets after the load; each blit fails
     * safe if a tile is not resident. */
    menu_assetgroup_load(sTrackselectFrameAssetIds);
    sWoodTex = (TextureHeader *) gMenuAssets[TEXTURE_SURFACE_BUTTON_WOOD];
    sArrowTex[0] = (TextureHeader *) gMenuAssets[TEXTURE_ICON_ARROW_UP];
    sArrowTex[1] = (TextureHeader *) gMenuAssets[TEXTURE_ICON_ARROW_DOWN];
    sArrowTex[2] = (TextureHeader *) gMenuAssets[TEXTURE_ICON_ARROW_LEFT];
    sArrowTex[3] = (TextureHeader *) gMenuAssets[TEXTURE_ICON_ARROW_RIGHT];

    load_font(ASSET_FONTS_BIGFONT);
    load_font(ASSET_FONTS_SMALLFONT);
    mdkr_online_screen_hd_text_ref();

    sTs.assets = 1u;
    /* Retail scrolling sky of the initially-focused world (cursorCol is in cup
     * display order); the per-frame render re-arms it as the host browses. */
    mdkr_online_screen_backdrop(
        mdkr_online_screen_sky_world_for_cup(sTs.cursorCol));

    if (mdkr_online_trackselect_test_active()) {
        sTsEntryCount++;
    }
    trackselect_witness_tracks();

    /* reveal from black (retail fade cadence) + keep the retail menu music
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
        /* Retire the frame's authored display list FIRST (this frame's frame/
         * arrow/postcard texrects reference the tiles freed below -- the
         * freed-texture DL corruption fix, see mdkr_online_screen_dl_retire). */
        mdkr_online_screen_dl_retire();
        /* Disarm the borrowed sky before freeing its tiles (bgdraw_render lifetime). */
        mdkr_online_screen_backdrop_clear();
        mdkr_online_screen_hd_text_unref();
        unload_font(ASSET_FONTS_SMALLFONT);
        unload_font(ASSET_FONTS_BIGFONT);
        menu_assetgroup_free(sOnlineSkyAssetIds);
        for (c = 0u; c < TS_COLS; c++) {
            sCupBgTopTex[c] = NULL;
        }
        /* Free the screen-owned frame + arrow group and drop the resolved pointers
         * (the sky backdrop never referenced them, so no disarm ordering needed). */
        menu_assetgroup_free(sTrackselectFrameAssetIds);
        sWoodTex = NULL;
        sArrowTex[0] = NULL;
        sArrowTex[1] = NULL;
        sArrowTex[2] = NULL;
        sArrowTex[3] = NULL;
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
    trackselect_autonarrow_vehicle(
        trackselect_resolve_narrow_mask(&snap, haveSnap, occupied));

    trackselect_gather_input(&in);
    trackselect_apply_input(&in);

    /* JOINER stage-follow: the joiner has no cursor -- its browse stage is done
     * once the HOST's lock is visible in the authoritative snapshot (single: a
     * known configured_track; tournament: a locked cup). Retail's track pick
     * moves every player into the vehicle stage at once, so a FRESH lock (one
     * that differs from whatever was already configured when this screen
     * entered) is followed IMMEDIATELY. A lock that was ALREADY PRESENT at
     * entry (a pre-configured room, or a results re-front's stale pick while
     * the host re-browses) is followed only after a short browse dwell -- the
     * joiner gets the "HOST PICKED" beat, and the session's remote-vacate
     * detector (45-tick debounce) keeps its trackselect arm: a vanished host
     * trips LEFT during the dwell instead of the joiner warping into the
     * vehicle stage on a stale pick. */
    if (!sTs.host && haveSnap && !sTs.setupReady) {
        u8 lockMode = 0xFFu;
        u16 lockTrack = 0xFFFFu;
        u8 lockCup = 0xFFu;
        if (snap.mode == MDKR_ONLINE_SCREEN_MODE_TOURNAMENT) {
            if (snap.cup_id < TS_COLS) {
                lockMode = MDKR_ONLINE_SCREEN_MODE_TOURNAMENT;
                lockCup = snap.cup_id;
            }
        } else if (snap.configured_track != 0xFFFFu &&
                   trackselect_index_of(snap.configured_track) != TS_NONE) {
            lockMode = MDKR_ONLINE_SCREEN_MODE_SINGLE;
            lockTrack = snap.configured_track;
        }
        if (lockMode != 0xFFu) {
            bool fresh = lockMode != sTs.entryLockMode ||
                         lockTrack != sTs.entryLockTrack ||
                         lockCup != sTs.entryLockCup;
            if (fresh || sTs.ticks >= TS_JOINER_STALE_FOLLOW_TICKS) {
                sTs.setupReady = 1u;
            }
        }
    }

    /* Re-resolve after input (the lock/cursor may have moved) so the published
     * vehicle + the witness reflect this frame. */
    narrowTrack = trackselect_resolve_narrow_track(&snap, haveSnap);
    trackselect_autonarrow_vehicle(
        trackselect_resolve_narrow_mask(&snap, haveSnap, occupied));
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
    trackselect_tt_announce(updateRate); /* retail T.T. hover announcer (host only) */
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

/* REMATCH-scenario refusal bookkeeping: the scripted host's OK before the
 * joiner's stage confirm is refused NOT_READY (mirroring BEGIN_LOADING's
 * all_ready gate) and re-fires each tick (the planner's refusal-note cadence);
 * the first refusal and the eventual convergence are witnessed once each. */
#define TS_REMATCH_OK_TICK 16u /* well inside the joiner's 70-tick browse dwell */
static u8 sTsRematchRefused;        /* first-refusal witness fired */
static u32 sTsRematchRefusedTicks;  /* how many ticks the OK stayed refused */
static u8 sTsRematchConvergedLogged;

static void trackselect_test_resolve(void) {
    if (sTsTestActive < 0) {
        const char *e = getenv("MDKR_TEST_ONLINE_TRACKSELECT");
        sTsTestActive = (e != NULL) ? 1 : 0;
        /* Scenario from the env VALUE: "joiner" selects the joiner-render lane,
         * anything else is the default single-race host lane. */
        if (e != NULL && strstr(e, "rematch") != NULL) {
            sTsScenario = (s8) TS_SCN_REMATCH;
        } else if (e != NULL && strstr(e, "joiner") != NULL) {
            sTsScenario = (s8) TS_SCN_JOINER;
        } else if (e != NULL && strstr(e, "lockfade") != NULL) {
            sTsScenario = (s8) TS_SCN_LOCKFADE;
        } else if (e != NULL && strstr(e, "hold") != NULL) {
            sTsScenario = (s8) TS_SCN_HOLD;
        } else {
            sTsScenario = (s8) TS_SCN_SINGLE_HOST;
        }
    }
}

static void trackselect_test_reset(void) {
    trackselect_test_resolve();
    if (!sTsTestActive) {
        return;
    }
    sTsAdopted = 0u;
    sTsStartArmed = 0u;
    sTsRematchRefused = 0u;
    sTsRematchRefusedTicks = 0u;
    sTsRematchConvergedLogged = 0u;
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
        } else if (sTsScenario == TS_SCN_REMATCH) {
            /* SAME-TRACK rematch re-front: the CHARSELECT seam already
             * pre-seeded the stale config + joiner roles (it must be in the
             * snapshot BEFORE _enter latches the stale-lock dwell); enforce
             * them defensively here so the scenario cannot silently degrade
             * into the host shape. */
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
     * render the room and narrow its own vehicle to the cup's round-0 track.
     * The local joiner publishes only its char/vehicle/ready (no config), which
     * we converge below; the host's OK (start) fires from the VEHICLE-stage pump
     * once both seats confirmed there (retail order). */
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
        mdkr_party_link_publish(&sTsRoom);
        return;
    }

    /* SAME-TRACK REMATCH scenario: the seam plays the REMOTE HOST of a
     * post-race re-front whose room still carries LAST round's config (the
     * reducer's REMATCH clear_round drops only ready + votes, lobby_core.c:407)
     * -- so re-locking the IDENTICAL track never ready-clears (no config
     * change, lobby_core.c:756). The host is fast on its own endpoint: it
     * holds ready and, from TS_REMATCH_OK_TICK (well inside the local joiner's
     * 70-tick stale-lock browse dwell), presses OK every tick. The reduce
     * models the real machinery: seat ready latches from intent.ready only
     * with char+vehicle set (SET_READY's member_selection_complete), DROPS on
     * !ready/backout (the planner's CHANGE_SELECTION), and the OK is
     * BEGIN_LOADING's all_ready gate -- refused NOT_READY while any seat is
     * un-ready, re-fired each tick (the refusal-note cadence), converging the
     * first tick both seats are ready. */
    if (sTsScenario == TS_SCN_REMATCH) {
        if (mdkr_party_link_intent_poll(&intent)) {
            if (intent.confirmed &&
                intent.hover_character < MDKR_ONLINE_SCREEN_CHAR_COUNT) {
                sTsRoom.seats[0].character_id = intent.hover_character;
            }
            if (intent.vehicle_id < MDKR_ONLINE_SCREEN_VEHICLE_COUNT) {
                sTsRoom.seats[0].vehicle_id = intent.vehicle_id;
            }
            if (intent.ready &&
                sTsRoom.seats[0].character_id != MDKR_ONLINE_SCREEN_NO_CHARACTER &&
                sTsRoom.seats[0].vehicle_id != MDKR_ONLINE_SCREEN_NO_VEHICLE) {
                sTsRoom.seats[0].ready = 1u;
            } else if (intent.backout || !intent.ready) {
                sTsRoom.seats[0].ready = 0u;
            }
        }
        sTsRoom.seats[1].ready = 1u; /* the host re-asserts ready */
        if (sTs.ticks >= TS_REMATCH_OK_TICK &&
            sTsRoom.phase == (uint8_t) MDKR_ONLINE_SCREEN_LOBBY_PHASE) {
            if (sTsRoom.seats[0].ready && sTsRoom.seats[1].ready) {
                sTsStartArmed++;
                if (sTsStartArmed >= 2u) {
                    sTsRoom.phase = (uint8_t) TS_LOADING_PHASE;
                }
            } else {
                sTsStartArmed = 0u;
                sTsRematchRefusedTicks++;
                if (!sTsRematchRefused) {
                    sTsRematchRefused = 1u;
                    fprintf(stderr,
                            "[online-trackselect] test-script host OK refused "
                            "NOT_READY (joiner not stage-confirmed; re-fire "
                            "armed)\n");
                }
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
        /* No start handling here: the host's START moved to the vehicle stage
         * (retail order), where mdkr_online_trackselect_test_vehicle_pump owns
         * the LOADING flip for this seam's lanes. */
    }

    mdkr_party_link_publish(&sTsRoom);
}

/* VEHICLE-stage reducer pump for the TRACKSELECT seam's lanes. The flow now ENDS
 * at the vehicle stage (retail order: browse -> lock -> vehicles -> OK), so the
 * seam's minimal reducer must keep converging the room -- and own the LOADING
 * flip -- while the VEHICLESELECT screen ticks (this TU's own reduce runs only
 * from trackselect_tick). Called from vehicleselect_tick; inert unless this seam
 * is armed and has adopted a room. SINGLE-HOST: converge the local seat's
 * vehicle/ready + the host's republished config, flip to LOADING on the polled
 * start_requested once both seats are ready. JOINER: the scripted remote HOST
 * presses OK once both seats are ready (the local joiner never starts). */
void mdkr_online_trackselect_test_vehicle_pump(void) {
    MdkrPartyLinkLocalIntent intent;
    bool haveIntent;

    trackselect_test_resolve();
    if (!sTsTestActive || !mdkr_party_link_active() || !sTsAdopted) {
        return;
    }

    haveIntent = mdkr_party_link_intent_poll(&intent);
    if (haveIntent) {
        if (intent.confirmed && intent.hover_character < MDKR_ONLINE_SCREEN_CHAR_COUNT) {
            sTsRoom.seats[0].character_id = intent.hover_character;
        }
        if (intent.vehicle_id < MDKR_ONLINE_SCREEN_VEHICLE_COUNT) {
            sTsRoom.seats[0].vehicle_id = intent.vehicle_id;
        }
        if (sTsScenario != TS_SCN_JOINER && sTsScenario != TS_SCN_REMATCH) {
            /* the host's vehicle-stage republish of its locked config (same
             * accept rules as the browse-stage reduce; a re-publish of the SAME
             * value is change-detected and never re-clears ready). */
            if (intent.mode != MDKR_PARTY_LINK_MODE_UNSET &&
                sTsRoom.mode != intent.mode) {
                sTsRoom.mode = intent.mode;
            }
            if (intent.config_track != MDKR_PARTY_LINK_TRACK_UNSET &&
                sTsRoom.configured_track != intent.config_track &&
                trackselect_test_known_track(intent.config_track)) {
                sTsRoom.configured_track = intent.config_track;
            }
            if (intent.cup_id != MDKR_PARTY_LINK_CUP_UNSET &&
                sTsRoom.cup_id != intent.cup_id) {
                sTsRoom.cup_id = intent.cup_id;
            }
        }
        sTsRoom.seats[1].ready = 1u; /* the scripted remote republishes ready */
        if (intent.ready &&
            sTsRoom.seats[0].character_id != MDKR_ONLINE_SCREEN_NO_CHARACTER &&
            sTsRoom.seats[0].vehicle_id != MDKR_ONLINE_SCREEN_NO_VEHICLE) {
            sTsRoom.seats[0].ready = 1u;
        } else if (!intent.ready) {
            sTsRoom.seats[0].ready = 0u; /* vehicle un-confirmed (B) */
        }
    }

    {
        bool configReady =
            (sTsRoom.mode == MDKR_ONLINE_SCREEN_MODE_TOURNAMENT)
                ? (sTsRoom.cup_id != 0xFFu)
                : (sTsRoom.configured_track != 0xFFFFu);
        bool bothReady = sTsRoom.seats[0].ready && sTsRoom.seats[1].ready;
        bool wantStart = (sTsScenario == TS_SCN_JOINER ||
                          sTsScenario == TS_SCN_REMATCH)
                             ? bothReady /* remote host OKs the converged room */
                             : (haveIntent && intent.start_requested &&
                                bothReady);
        if (wantStart && configReady) {
            sTsStartArmed++;
            if (sTsStartArmed >= 6u) {
                if (sTsScenario == TS_SCN_REMATCH && sTsRematchRefused &&
                    !sTsRematchConvergedLogged &&
                    sTsRoom.phase == (uint8_t) MDKR_ONLINE_SCREEN_LOBBY_PHASE) {
                    /* the re-fired OK finally converged: the joiner's stage
                     * confirm landed ready=1, so the all_ready gate opened. */
                    sTsRematchConvergedLogged = 1u;
                    fprintf(stderr,
                            "[online-trackselect] test-script host OK converged "
                            "after %u refused tick(s) (joiner stage-confirmed) "
                            "-> LOADING\n",
                            (unsigned) sTsRematchRefusedTicks);
                }
                sTsRoom.phase = (uint8_t) TS_LOADING_PHASE;
            }
        } else if (sTsScenario == TS_SCN_REMATCH &&
                   sTs.ticks >= TS_REMATCH_OK_TICK &&
                   sTsRoom.phase == (uint8_t) MDKR_ONLINE_SCREEN_LOBBY_PHASE) {
            /* the host's OK keeps re-firing against a not-yet-ready room while
             * the stage fronts too (the refusal-note cadence spans screens). */
            sTsStartArmed = 0u;
            sTsRematchRefusedTicks++;
        }
    }

    mdkr_party_link_publish(&sTsRoom);
}

u8 mdkr_online_trackselect_test_active(void) {
    trackselect_test_resolve();
    return (u8) (sTsTestActive > 0 ? 1 : 0);
}

/* the seam's SAME-TRACK REMATCH scenario is armed (env value "rematch").
 * The CHARSELECT seam consults this to pre-seed its scripted room with LAST
 * round's persisted config + joiner roles BEFORE trackselect _enter latches
 * the stale-lock browse dwell from the live snapshot (pre-seeding at this
 * seam's own adoption would be one screen too late). Test-only; inert in a
 * normal run. */
u8 mdkr_online_trackselect_test_scenario_rematch(void) {
    trackselect_test_resolve();
    return (u8) ((sTsTestActive > 0 && sTsScenario == (s8) TS_SCN_REMATCH) ? 1
                                                                           : 0);
}

/* the screen's OWN browse-stage-done latch (reset by _enter's memset): host set
 * it by locking a pick (A); a joiner set it on observing the host's lock in the
 * snapshot. The session gates the TRACKSELECT -> vehicle-stage hand-off on this,
 * so a B-back from the vehicle stage re-requires a fresh lock before
 * re-advancing (no one-frame bounce). */
u8 mdkr_online_trackselect_setup_ready(void) {
    return (u8) (sTs.setupReady ? 1 : 0);
}

/* The host's LAST LOCKED session config, for the vehicle stage's publisher: the
 * stage must keep republishing the locked mode + track/cup every frame (the
 * reverse-feed planner re-fires an un-converged SET_* only while an intent still
 * carries it -- a single lock-tick publish could be lost on a real transport).
 * Reads the file-scope persistence latches, which the A-lock refreshes. Fields
 * with no lock carry the party_link UNSET sentinels. */
void mdkr_online_trackselect_locked_config(u8 *mode, u16 *configTrack, u8 *cupId) {
    *mode = (u8) MDKR_PARTY_LINK_MODE_UNSET;
    *configTrack = (u16) MDKR_PARTY_LINK_TRACK_UNSET;
    *cupId = (u8) MDKR_PARTY_LINK_CUP_UNSET;
    if (sLastMode == MDKR_ONLINE_SCREEN_MODE_SINGLE) {
        if (sLastLockedTrack != TS_NONE) {
            *mode = sLastMode;
            *configTrack = (u16) sTrackIds[sLastLockedTrack];
        }
    } else if (sLastLockedCup != TS_NONE) {
        *mode = sLastMode;
        *cupId = sLastLockedCup;
    }
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
