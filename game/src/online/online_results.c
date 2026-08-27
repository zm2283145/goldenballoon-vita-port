/* SEPARATED-BOOT-PATH (Strategy D2) native online RESULTS / STANDINGS screen.
 *
 * ============================ THE D2 REUSE BOUNDARY ========================
 * The post-race SCREEN of the separated online flow, between the race and the
 * next race (tournament) or the session end. Strategy D2 means: RE-IMPLEMENT the
 * presentation here using the GAME'S OWN decoded assets rather than calling the
 * offline menu's MENU_RESULTS loop.
 *
 * WHY IT DOES NOT REPLICATE MENU_RESULTS: the offline results/trophy menus drive
 * the GAMEMODE_MENU state machine (gCurrentMenuId, its own transitions) AND carry
 * a settings->racers[].placements side-effect (a wins-per-placement scoreboard).
 * The online session must never enter the offline state machine and must never
 * mutate the trophy/points settings -- the launcher's reducer is the ONLY points
 * authority. So this screen borrows the DATA + the DRAW/SFX primitives, exactly
 * like online_charselect.c / online_trackselect.c, and owns all of its own state.
 *
 * WHAT IT BORROWS (read-only reuse of already-compiled game code/data):
 *   - gRacerPortraits[10] + the portrait-only asset group (menu.c) -- the same
 *     ten faces the charselect screen borrows (portrait-ONLY id list so no menu
 *     OBJECTS spawn, unlike gRaceResultsObjectIndices which does).
 *   - gRacePlacementsArray[8] "1ST".."8TH" (menu.c) -- the real place labels.
 *   - draw_text / set_text_* (font.h), texrect_draw / bgdraw_* (rcp_dkr.h),
 *     sound_play + SOUND_* (audio.h / sound_ids.h), input_pressed (joypad.h).
 *
 * WHAT IT READS (never mutates): THIS race's finishing order from
 * mdkr_online_race_results_poll() (the engine-captured placements, canonical slot
 * -> placement, 0xFF absent) and the cup points table from the party_link
 * forward-feed snapshot (points[]/last_placements/race_index/mode). It renders
 * snapshot.points[] DIRECTLY -- the reducer already did the trophy-weight math
 * (tournament only); this screen never accumulates points or touches
 * settings->racers[].trophy_points.
 *
 * The ENTIRE TU is #if MDKR_ENABLE_ONLINE_BETA and it is added to the build only
 * inside the beta CMake gate (game/src/online/ is not globbed), so a normal (beta
 * OFF) build never compiles a byte of it and the release engine object is
 * untouched.
 * ==========================================================================
 */
#include "online/online_results.h"

#if MDKR_ENABLE_ONLINE_BETA

/* Game/PR headers FIRST (same sprintf-ordering rationale as online_charselect.c). */
#include "types.h"
#include "thread3_main.h"
#include "enums.h"      /* AlignmentFlags */
#include "menu.h"       /* gRacerPortraits, menu_assetgroup_load/free,
                           menu_racer_portraits, gRacePlacementsArray,
                           TEXTURE_ICON_PORTRAIT_*, font.h (draw_text, ...) */
#include "rcp_dkr.h"    /* texrect_draw, bgdraw_fillcolour */
#include "audio.h"      /* sound_play */
#include "sound_ids.h"  /* SOUND_SELECT2 / SOUND_MENU_PICK2 / ... */
#include "joypad.h"     /* input_pressed */
#include "PR/os_cont.h" /* A_BUTTON / START_BUTTON */
#include "net/party_link.h"
#include "net/online_race_results.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Screen space (mirrored, like online_charselect.c). */
#define RES_SCREEN_W 320
#define RES_SCREEN_H 240
#define RES_SCREEN_W_HALF 160

/* The engine's live 2D display list + the decoded portraits (declared here, not
 * exposed by menu.h -- exactly how online_charselect.c reaches them). */
extern Gfx *gCurrDisplayList;
extern DrawTexture *gRacerPortraits[10];
/* The real "1ST".."8TH" place labels (menu.c gRacePlacementsArray). Declared
 * here (menu.h does not export it) rather than by editing menu.c, exactly how
 * gRacerPortraits is reached above. */
extern char *gRacePlacementsArray[8];

/* ---- Local mirrors of the launcher lobby's id space (no launcher headers) --- */
#define RES_CHAR_COUNT 10u        /* MDKR_ONLINE_CHARACTER_COUNT */
#define RES_NO_CHARACTER 0xFFu    /* MDKR_ONLINE_NO_CHARACTER */
#define RES_NO_VEHICLE 0xFFu      /* MDKR_ONLINE_NO_VEHICLE */
#define RES_MODE_SINGLE 0u        /* MDKR_ONLINE_MODE_SINGLE_RACE */
#define RES_MODE_TOURNAMENT 1u    /* MDKR_ONLINE_MODE_TOURNAMENT */
#define RES_LOCAL_PAD 0           /* PLAYER_ONE */
#define RES_SLOTS 4u              /* MDKR_ONLINE_RACE_RESULT_SLOTS / seats */
#define RES_PLACE_NONE 0xFFu      /* MDKR_ONLINE_RACE_RESULT_NONE */
#define RES_CUP_ROUNDS 4u         /* a DKR cup is four rounds (RACE n/4 copy) */

/* Two visible stages of the post-race screen. A single race stops at RESULTS
 * (there is no cup standings); a tournament walks RESULTS -> STANDINGS. */
#define RES_STAGE_RESULTS 0u
#define RES_STAGE_STANDINGS 1u

/* Visible countdowns (R7): results 15s, standings 10s. updateRate accumulates in
 * 60ths of a second, so seconds*60. */
#define RES_RESULTS_UNITS 900u   /* 15s */
#define RES_STANDINGS_UNITS 600u /* 10s */

/* Menu SFX (the real DKR enums; same reuse as CHARSELECT). */
#define RES_SFX_ADVANCE SOUND_SELECT3
#define RES_SFX_TICK SOUND_MENU_PICK2

/* Trophy weights {9,7,5,3,1,0,0,0} == gTrophyRacePointsArray (menu.c) /
 * kTrophyPoints (lobby_core.c). Used ONLY by the headless stand-in reducer test
 * seam below; the SCREEN itself renders snapshot.points[] and never weights. */
static const u16 sTrophyPoints[8] = {9u, 7u, 5u, 3u, 1u, 0u, 0u, 0u};

/* Online character id -> gRacerPortraits index (identical mapping + names to
 * online_charselect.c -- the single source of the three-orderings truth). */
static const u8 sOnlineToPortrait[RES_CHAR_COUNT] = {
    1u, 9u, 8u, 6u, 5u, 3u, 4u, 0u, 2u, 7u,
};
static const char *const sOnlineNames[RES_CHAR_COUNT] = {
    "DIDDY", "TIMBER", "PIPSY", "TIPTUP", "CONKER",
    "BUMPER", "BANJO", "KRUNCH", "DRUMSTICK", "T.T.",
};

/* The portrait-ONLY texture group (KRUNCH..TIMBER), the same list charselect
 * loads: menu_asset_load routes each to load_texture, so no menu OBJECTS spawn. */
static s16 sPortraitAssetIds[] = {
    TEXTURE_ICON_PORTRAIT_KRUNCH, TEXTURE_ICON_PORTRAIT_DIDDY,
    TEXTURE_ICON_PORTRAIT_DRUMSTICK, TEXTURE_ICON_PORTRAIT_BUMPER,
    TEXTURE_ICON_PORTRAIT_BANJO, TEXTURE_ICON_PORTRAIT_CONKER,
    TEXTURE_ICON_PORTRAIT_TIPTUP, TEXTURE_ICON_PORTRAIT_TT,
    TEXTURE_ICON_PORTRAIT_PIPSY, TEXTURE_ICON_PORTRAIT_TIMBER,
    -1,
};

/* ---- Session-owned screen state (never an offline global) ------------------ */
typedef struct MdkrOnlineResultsState {
    u8 stage;         /* RES_STAGE_RESULTS / RES_STAGE_STANDINGS */
    u8 isFinal;       /* the last race of the soak/cup: standings HOLD (no advance) */
    u8 raceIndex;     /* 0-based cup round, for the "RACE n/4" copy */
    u8 assets;        /* portrait group + fonts loaded */
    u8 host;          /* local seat is the room leader (advance authority) */
    u8 leave;         /* B: back-out request (edge; PD-T6 return) */
    u8 haveResults;   /* the engine-captured placements were polled in */
    u8 advanced;      /* an ADVANCE was already returned for this stage (edge) */
    u32 stageTicks;   /* countdown accumulator for the current stage */
    u8 placements[RES_SLOTS]; /* THIS race's canonical-slot -> placement (poll) */
} MdkrOnlineResultsState;

static MdkrOnlineResultsState sRes;

/* Witness change-detect (file scope so _enter can reset for a clean re-entry). */
static u32 sWitnessKey = 0xFFFFFFFFu;

/* ---- forward decls (test seam at the bottom) ------------------------------ */
static void results_test_resolve(void);
static void results_test_accrue(void);
static void results_test_pump(void);

/* ======================================================================== *
 * Small helpers
 * ======================================================================== */
static s32 results_local_seat(const MdkrPartyLinkSnapshot *snap) {
    unsigned i;
    for (i = 0u; i < MDKR_PARTY_LINK_SEATS; i++) {
        if (snap->seats[i].occupied && snap->seats[i].is_local) {
            return (s32) i;
        }
    }
    return -1;
}

/* Countdown seconds still on the clock for the current stage (ceil), 0 when
 * elapsed. */
static u32 results_seconds_left(u32 limit) {
    u32 done = sRes.stageTicks;
    if (done >= limit) {
        return 0u;
    }
    return (limit - done + 59u) / 60u;
}

/* Resolve one seat's short name (untrusted snapshot name, else the character's
 * canonical name, else a slot fallback). */
static void results_seat_name(const MdkrPartyLinkSnapshot *snap, bool haveSnap,
                              unsigned slot, char *out, size_t cap) {
    if (haveSnap && slot < MDKR_PARTY_LINK_SEATS &&
        snap->seats[slot].occupied && snap->seats[slot].name[0] != '\0') {
        (void) snprintf(out, cap, "%.*s", (int) MDKR_PARTY_LINK_NAME_BYTES,
                        snap->seats[slot].name);
        return;
    }
    if (haveSnap && slot < MDKR_PARTY_LINK_SEATS &&
        snap->seats[slot].character_id < RES_CHAR_COUNT) {
        (void) snprintf(out, cap, "%s",
                        sOnlineNames[snap->seats[slot].character_id]);
        return;
    }
    (void) snprintf(out, cap, "P%u", slot + 1u);
}

/* ======================================================================== *
 * Render (native: real portraits + real font, into the engine frame list)
 * ======================================================================== */
static void results_text(s32 x, s32 y, s32 fontId, char *text,
                         AlignmentFlags align, s32 r, s32 g, s32 b) {
    set_text_font(fontId);
    set_text_background_colour(0, 0, 0, 0);
    set_text_colour(r, g, b, 0, 255);
    draw_text(&gCurrDisplayList, x, y, text, align);
}

static void results_draw_portrait(u8 character, s32 x, s32 y, u8 r, u8 g, u8 b) {
    DrawTexture *portrait;
    if (character >= RES_CHAR_COUNT) {
        return;
    }
    portrait = gRacerPortraits[sOnlineToPortrait[character]];
    if (portrait != NULL && portrait[0].texture != NULL) {
        texrect_draw(&gCurrDisplayList, portrait, x, y, r, g, b, 255);
    }
}

/* RESULTS stage: THIS race's finishing order (portrait + place label + name),
 * sorted by placement, local seat highlighted, remote labelled. */
static void results_render_results(const MdkrPartyLinkSnapshot *snap,
                                   bool haveSnap, s32 localSeat) {
    u32 secs = results_seconds_left(RES_RESULTS_UNITS);
    s32 rowY = 60;
    u8 place;
    char line[64];

    results_text(RES_SCREEN_W_HALF, 20, ASSET_FONTS_BIGFONT, "RACE RESULTS",
                 ALIGN_MIDDLE_CENTER, 255, 224, 96);

    /* Walk placements in finishing order (place 0 == 1st). Each canonical slot
     * carries its own placement, so scan for the slot at each place. */
    for (place = 0u; place < RES_SLOTS; place++) {
        unsigned slot;
        for (slot = 0u; slot < RES_SLOTS; slot++) {
            bool isLocal;
            u8 character = RES_NO_CHARACTER;
            char name[32];
            const char *placeLabel;
            s32 nr, ng, nb;

            if (sRes.placements[slot] != place) {
                continue;
            }
            isLocal = ((s32) slot == localSeat);
            if (haveSnap && snap->seats[slot].character_id < RES_CHAR_COUNT) {
                character = snap->seats[slot].character_id;
            }
            results_seat_name(snap, haveSnap, slot, name, sizeof(name));
            placeLabel = (place < 8u) ? (const char *) gRacePlacementsArray[place]
                                      : "-";

            /* Colour + a text tag carry the "this is you" state (no colour-only
             * meaning): local = bright gold + [YOU], remote = plain white. */
            if (isLocal) {
                nr = 255; ng = 224; nb = 96;
            } else {
                nr = 220; ng = 220; nb = 220;
            }

            /* Row: place label (right of a gap) | portrait | name -- laid out so
             * the ~44px portrait never overprints the label or the name. */
            results_text(52, rowY + 4, ASSET_FONTS_BIGFONT,
                         (char *) placeLabel, ALIGN_MIDDLE_RIGHT, nr, ng, nb);
            results_draw_portrait(character, 64, rowY - 8, (u8) nr, (u8) ng,
                                  (u8) nb);
            (void) snprintf(line, sizeof(line), "%.12s%s", name,
                            isLocal ? "  [YOU]" : "");
            results_text(118, rowY + 4, ASSET_FONTS_SMALLFONT, line,
                         ALIGN_MIDDLE_LEFT, nr, ng, nb);
            rowY += 40;
            break;
        }
    }

    /* Countdown line (always legible; names the next step). */
    if (haveSnap && snap->mode == RES_MODE_TOURNAMENT) {
        (void) snprintf(line, sizeof(line), "STANDINGS IN %us", secs);
    } else {
        (void) snprintf(line, sizeof(line), "CONTINUING IN %us", secs);
    }
    results_text(RES_SCREEN_W_HALF, 214, ASSET_FONTS_SMALLFONT, line,
                 ALIGN_MIDDLE_CENTER, 200, 200, 255);
    results_text(RES_SCREEN_W_HALF, 228, ASSET_FONTS_SMALLFONT,
                 sRes.host ? "A: CONTINUE" : "WAITING FOR HOST...",
                 ALIGN_MIDDLE_CENTER, sRes.host ? 255 : 150,
                 sRes.host ? 255 : 150, sRes.host ? 255 : 150);
}

/* STANDINGS stage: the cup points table (portraits + accumulated points, sorted
 * descending), this race's delta shown, "RACE n/4". */
static void results_render_standings(const MdkrPartyLinkSnapshot *snap,
                                     bool haveSnap, s32 localSeat) {
    u32 secs = results_seconds_left(RES_STANDINGS_UNITS);
    u16 points[RES_SLOTS];
    u8 order[RES_SLOTS];
    unsigned nseats = 0u;
    unsigned i, j;
    s32 rowY = 64;
    char line[64];

    results_text(RES_SCREEN_W_HALF, 20, ASSET_FONTS_BIGFONT,
                 sRes.isFinal ? "FINAL STANDINGS" : "STANDINGS",
                 ALIGN_MIDDLE_CENTER, 255, 224, 96);

    if (haveSnap) {
        (void) snprintf(line, sizeof(line), "RACE %u/%u",
                        (unsigned) (snap->race_index + 1u), RES_CUP_ROUNDS);
        results_text(RES_SCREEN_W_HALF, 40, ASSET_FONTS_SMALLFONT, line,
                     ALIGN_MIDDLE_CENTER, 200, 200, 255);
    }

    /* Collect the occupied seats + their (reducer-accrued) points from the
     * snapshot, then selection-sort by points descending. */
    for (i = 0u; i < RES_SLOTS; i++) {
        if (haveSnap && i < MDKR_PARTY_LINK_SEATS && snap->seats[i].occupied) {
            order[nseats] = (u8) i;
            points[nseats] = snap->points[i];
            nseats++;
        }
    }
    for (i = 0u; i + 1u < nseats; i++) {
        for (j = i + 1u; j < nseats; j++) {
            if (points[j] > points[i]) {
                u16 tp = points[i]; points[i] = points[j]; points[j] = tp;
                { u8 to = order[i]; order[i] = order[j]; order[j] = to; }
            }
        }
    }

    for (i = 0u; i < nseats; i++) {
        unsigned slot = order[i];
        bool isLocal = ((s32) slot == localSeat);
        u8 character = RES_NO_CHARACTER;
        char name[32];
        s32 nr, ng, nb;
        u16 delta = (haveSnap && slot < MDKR_PARTY_LINK_SEATS &&
                     snap->last_placements[slot] < 8u)
                        ? sTrophyPoints[snap->last_placements[slot]]
                        : 0u;

        if (haveSnap && snap->seats[slot].character_id < RES_CHAR_COUNT) {
            character = snap->seats[slot].character_id;
        }
        results_seat_name(snap, haveSnap, slot, name, sizeof(name));
        if (isLocal) {
            nr = 255; ng = 224; nb = 96;
        } else {
            nr = 220; ng = 220; nb = 220;
        }

        /* Row: rank | portrait | name (left) | points (FUNFONT, right) | delta.
         * Spaced so the ~44px portrait never overprints the rank or the name. */
        (void) snprintf(line, sizeof(line), "%u.", i + 1u);
        results_text(44, rowY + 4, ASSET_FONTS_BIGFONT, line,
                     ALIGN_MIDDLE_RIGHT, nr, ng, nb);
        results_draw_portrait(character, 52, rowY - 8, (u8) nr, (u8) ng,
                              (u8) nb);
        (void) snprintf(line, sizeof(line), "%.10s%s", name,
                        isLocal ? " [YOU]" : "");
        results_text(104, rowY + 4, ASSET_FONTS_SMALLFONT, line,
                     ALIGN_MIDDLE_LEFT, nr, ng, nb);
        /* Points in FUNFONT (the trophy-rankings vocabulary), with this race's
         * delta so a newcomer sees WHY the total moved. */
        (void) snprintf(line, sizeof(line), "%u", (unsigned) points[i]);
        results_text(252, rowY + 4, ASSET_FONTS_FUNFONT, line,
                     ALIGN_MIDDLE_RIGHT, nr, ng, nb);
        if (delta > 0u) {
            (void) snprintf(line, sizeof(line), "+%u", (unsigned) delta);
            results_text(300, rowY + 4, ASSET_FONTS_SMALLFONT, line,
                         ALIGN_MIDDLE_RIGHT, 120, 255, 120);
        }
        rowY += 40;
    }

    if (sRes.isFinal) {
        results_text(RES_SCREEN_W_HALF, 214, ASSET_FONTS_SMALLFONT,
                     "CUP COMPLETE", ALIGN_MIDDLE_CENTER, 120, 255, 120);
    } else {
        (void) snprintf(line, sizeof(line), "NEXT RACE IN %us", secs);
        results_text(RES_SCREEN_W_HALF, 214, ASSET_FONTS_SMALLFONT, line,
                     ALIGN_MIDDLE_CENTER, 200, 200, 255);
        results_text(RES_SCREEN_W_HALF, 228, ASSET_FONTS_SMALLFONT,
                     sRes.host ? "A: NEXT RACE" : "WAITING FOR HOST...",
                     ALIGN_MIDDLE_CENTER, sRes.host ? 255 : 150,
                     sRes.host ? 255 : 150, sRes.host ? 255 : 150);
    }
}

/* Bounded stderr witness: one line only when the visible state changes, so the
 * headless soak lane can read the drawn placements/points + stage/countdown. */
static void results_witness(const MdkrPartyLinkSnapshot *snap, bool haveSnap) {
    u32 secs = results_seconds_left(sRes.stage == RES_STAGE_RESULTS
                                        ? RES_RESULTS_UNITS
                                        : RES_STANDINGS_UNITS);
    u8 mode = haveSnap ? snap->mode : RES_MODE_SINGLE;
    u16 p0 = haveSnap ? snap->points[0] : 0u;
    u16 p1 = haveSnap ? snap->points[1] : 0u;
    u32 key = ((u32) sRes.stage) | ((u32) sRes.placements[0] << 1) |
              ((u32) sRes.placements[1] << 5) | ((u32) (secs & 0x1Fu) << 9) |
              ((u32) mode << 14) | ((u32) (p0 & 0xFFu) << 15) |
              ((u32) (p1 & 0xFFu) << 23);
    if (key == sWitnessKey) {
        return;
    }
    sWitnessKey = key;
    fprintf(stderr,
            "[online-results] render stage=%s mode=%u race=%u host=%u "
            "placements=%u,%u,%u,%u points=%u,%u,%u,%u secs=%u final=%u\n",
            sRes.stage == RES_STAGE_RESULTS ? "results" : "standings",
            (unsigned) mode, (unsigned) (haveSnap ? snap->race_index : 0u),
            (unsigned) sRes.host, (unsigned) sRes.placements[0],
            (unsigned) sRes.placements[1], (unsigned) sRes.placements[2],
            (unsigned) sRes.placements[3],
            (unsigned) (haveSnap ? snap->points[0] : 0u),
            (unsigned) (haveSnap ? snap->points[1] : 0u),
            (unsigned) (haveSnap ? snap->points[2] : 0u),
            (unsigned) (haveSnap ? snap->points[3] : 0u), (unsigned) secs,
            (unsigned) sRes.isFinal);
}

/* ======================================================================== *
 * Lifecycle
 * ======================================================================== */
void mdkr_online_results_enter(u8 isFinalRace, u8 raceIndex) {
    unsigned i;

    memset(&sRes, 0, sizeof(sRes));
    sRes.stage = RES_STAGE_RESULTS;
    sRes.isFinal = isFinalRace ? 1u : 0u;
    sRes.raceIndex = raceIndex;
    for (i = 0u; i < RES_SLOTS; i++) {
        sRes.placements[i] = RES_PLACE_NONE;
    }
    sWitnessKey = 0xFFFFFFFFu;

    /* Read THIS race's engine-captured finishing order ONCE (canonical slot ->
     * placement; 0xFF absent). The resident post-race fork already gated on a
     * captured result via the NON-consuming availability query, so this poll
     * hands out the placements to us (the reader that owns them). */
    {
        u8 out[MDKR_ONLINE_RACE_RESULT_SLOTS];
        if (mdkr_online_race_results_poll(out)) {
            for (i = 0u; i < RES_SLOTS; i++) {
                sRes.placements[i] = out[i];
            }
            sRes.haveResults = 1u;
        }
    }

    /* Headless stand-in reducer: accrue this race's trophy points into the
     * scripted forward feed (inert in a normal run). Must run AFTER the poll so
     * it sees this race's placements. */
    results_test_accrue();

    /* Borrow the real portraits + fonts (the charselect asset-borrow discipline). */
    menu_assetgroup_load(sPortraitAssetIds);
    menu_racer_portraits();
    load_font(ASSET_FONTS_BIGFONT);
    load_font(ASSET_FONTS_SMALLFONT);
    load_font(ASSET_FONTS_FUNFONT);
    sRes.assets = 1u;
    bgdraw_fillcolour(16, 24, 48); /* match the charselect/trackselect backdrop */

    fprintf(stderr,
            "[online-results] enter: native results up race=%u final=%u "
            "haveResults=%u placements=%u,%u,%u,%u (offline MENU_RESULTS "
            "bypassed)\n",
            (unsigned) raceIndex, (unsigned) sRes.isFinal,
            (unsigned) sRes.haveResults, (unsigned) sRes.placements[0],
            (unsigned) sRes.placements[1], (unsigned) sRes.placements[2],
            (unsigned) sRes.placements[3]);
}

void mdkr_online_results_exit(void) {
    if (sRes.assets) {
        unload_font(ASSET_FONTS_FUNFONT);
        unload_font(ASSET_FONTS_SMALLFONT);
        unload_font(ASSET_FONTS_BIGFONT);
        menu_assetgroup_free(sPortraitAssetIds);
        sRes.assets = 0u;
        fprintf(stderr, "[online-results] exit: freed portrait assets\n");
    }
}

/* Read the local pad: HOST advance (A/START) is immediate (host authority); a
 * JOINER press is a no-op (watch-only, R-D). B is a back-out edge (PD-T6). */
typedef struct ResInput {
    u8 advanceEdge;
    u8 bEdge;
} ResInput;

static void results_input_live(ResInput *in) {
    u32 pressed = input_pressed(RES_LOCAL_PAD);
    memset(in, 0, sizeof(*in));
    in->advanceEdge = (pressed & (A_BUTTON | START_BUTTON)) ? 1u : 0u;
    in->bEdge = (pressed & B_BUTTON) ? 1u : 0u;
}

/* Scripted headless input (only under the test seam): the HOST presses ADVANCE a
 * few ticks into the RESULTS stage, proving host-press-immediate. The STANDINGS
 * stage is deliberately left to auto-advance so BOTH the host-advance and the
 * countdown-to-zero paths are exercised in one soak. */
static void results_input_scripted(ResInput *in) {
    memset(in, 0, sizeof(*in));
    if (sRes.stage == RES_STAGE_RESULTS && sRes.stageTicks >= 8u) {
        in->advanceEdge = 1u;
    }
}

static void results_gather_input(ResInput *in) {
    if (mdkr_online_results_test_active()) {
        results_input_scripted(in);
    } else {
        results_input_live(in);
    }
}

MdkrOnlineResultsResult mdkr_online_results_tick(s32 updateRate) {
    MdkrPartyLinkSnapshot snap;
    bool haveSnap;
    s32 localSeat;
    bool tournament;
    u32 limit;
    ResInput in;
    u8 autoFire;
    u8 hostFire;

    if (updateRate <= 0) {
        updateRate = 1;
    }

    /* Stand-in reducer (headless soak): install the forward feed + publish the
     * scripted snapshot (points/mode/race_index). Inert in a normal run. */
    results_test_pump();

    haveSnap = mdkr_party_link_read(&snap);
    localSeat = haveSnap ? results_local_seat(&snap) : -1;
    sRes.host = (haveSnap && localSeat >= 0 && snap.seats[localSeat].is_host)
                    ? 1u
                    : (u8) (localSeat < 0 ? 1u : 0u); /* no seat -> assume host */
    tournament = haveSnap && snap.mode == RES_MODE_TOURNAMENT;
    limit = (sRes.stage == RES_STAGE_RESULTS) ? RES_RESULTS_UNITS
                                              : RES_STANDINGS_UNITS;

    results_gather_input(&in);
    if (in.bEdge) {
        sRes.leave = 1u;
    }

    /* Render + witness BEFORE the advance decision so the just-elapsed frame is
     * always drawn (a stage that advances still showed its final frame). */
    if (sRes.stage == RES_STAGE_RESULTS) {
        results_render_results(&snap, haveSnap, localSeat);
    } else {
        results_render_standings(&snap, haveSnap, localSeat);
    }
    results_witness(&snap, haveSnap);

    sRes.stageTicks += (u32) updateRate;

    /* Advance authority (R7/R-D): host press advances immediately; auto-advance
     * fires at 0. The FINAL standings HOLDS (the resident soak ends on the tick
     * budget while the final standings is on screen). */
    hostFire = (sRes.host && in.advanceEdge) ? 1u : 0u;
    autoFire = (sRes.stageTicks >= limit) ? 1u : 0u;
    if (sRes.stage == RES_STAGE_STANDINGS && sRes.isFinal) {
        autoFire = 0u; /* hold on the final standings */
        hostFire = 0u;
    }

    if ((hostFire || autoFire) && !sRes.advanced) {
        if (sRes.stage == RES_STAGE_RESULTS && tournament) {
            /* RESULTS -> STANDINGS (same screen, next stage). */
            sound_play(RES_SFX_ADVANCE, NULL);
            sRes.stage = RES_STAGE_STANDINGS;
            sRes.stageTicks = 0u;
            fprintf(stderr,
                    "[online-results] advance: results -> standings (%s)\n",
                    hostFire ? "host" : "auto");
            return MDKR_ONLINE_RESULTS_STAY;
        }
        /* RESULTS (single race) or STANDINGS (tournament): the screen is done. */
        sound_play(RES_SFX_ADVANCE, NULL);
        sRes.advanced = 1u;
        fprintf(stderr, "[online-results] advance: screen done (%s)\n",
                hostFire ? "host" : "auto");
        return MDKR_ONLINE_RESULTS_ADVANCE;
    }

    if (sRes.leave) {
        sRes.leave = 0u; /* edge: never shadow ADVANCE */
        return MDKR_ONLINE_RESULTS_LEAVE;
    }
    return MDKR_ONLINE_RESULTS_STAY;
}

/* ======================================================================== *
 * Headless test seam (beta + env gated; entirely inert in a normal run)
 *
 * Stands in for the launcher reducer during the resident soak: it accrues this
 * race's trophy points (tournament weights) into a running cup total and
 * publishes a party_link forward-feed snapshot (mode=TOURNAMENT, race_index,
 * points[], last_placements[], two seats) so the STANDINGS screen renders a real
 * points table. Nothing here runs unless MDKR_TEST_ONLINE_RESIDENT is set.
 * ======================================================================== */
static s8 sTestActive = -1; /* -1 unresolved, 0 off, 1 on */
static u8 sTestInstalled;
static MdkrPartyLinkSnapshot sTestRoom;
static u16 sTestPoints[RES_SLOTS];   /* running cup totals (persist across races) */
static u8 sTestSeatChar[RES_SLOTS] = {0u, 5u, 0xFFu, 0xFFu}; /* Diddy / Bumper */

static void results_test_resolve(void) {
    if (sTestActive < 0) {
        sTestActive = (getenv("MDKR_TEST_ONLINE_RESIDENT") != NULL) ? 1 : 0;
    }
}

/* Accrue this race's trophy points (called from _enter, after the poll). */
static void results_test_accrue(void) {
    unsigned i;
    results_test_resolve();
    if (!sTestActive) {
        return;
    }
    memset(&sTestRoom, 0, sizeof(sTestRoom));
    sTestRoom.mode = (uint8_t) RES_MODE_TOURNAMENT;
    sTestRoom.phase = 4u; /* MDKR_ONLINE_RESULTS (display only) */
    sTestRoom.race_index = sRes.raceIndex;
    sTestRoom.configured_track = 0xFFFFu;
    sTestRoom.cup_id = 2u; /* Sherbet cup (matches the trackselect joiner lane) */
    for (i = 0u; i < RES_SLOTS; i++) {
        u8 place = sRes.placements[i];
        if (place < 8u) {
            sTestPoints[i] = (u16) (sTestPoints[i] + sTrophyPoints[place]);
        }
        sTestRoom.points[i] = sTestPoints[i];
        sTestRoom.last_placements[i] = place;
    }
    /* Two occupied seats: seat 0 local+host, seat 1 the remote rival. */
    sTestRoom.seats[0].occupied = 1u;
    sTestRoom.seats[0].is_local = 1u;
    sTestRoom.seats[0].is_host = 1u;
    sTestRoom.seats[0].connected = 1u;
    sTestRoom.seats[0].character_id = sTestSeatChar[0];
    memcpy(sTestRoom.seats[0].name, "YOU", sizeof("YOU"));
    sTestRoom.seats[1].occupied = 1u;
    sTestRoom.seats[1].connected = 1u;
    sTestRoom.seats[1].character_id = sTestSeatChar[1];
    memcpy(sTestRoom.seats[1].name, "RIVAL", sizeof("RIVAL"));
}

static void results_test_pump(void) {
    results_test_resolve();
    if (!sTestActive) {
        return;
    }
    if (!sTestInstalled || !mdkr_party_link_active()) {
        mdkr_party_link_clear();
        (void) mdkr_party_link_install();
        sTestInstalled = 1u;
        fprintf(stderr,
                "[online-results] test-script install (scripted STANDINGS feed; "
                "points=%u,%u)\n",
                (unsigned) sTestRoom.points[0], (unsigned) sTestRoom.points[1]);
    }
    mdkr_party_link_publish(&sTestRoom);
}

u8 mdkr_online_results_test_active(void) {
    results_test_resolve();
    return (u8) (sTestActive > 0 ? 1 : 0);
}

#endif /* MDKR_ENABLE_ONLINE_BETA */
