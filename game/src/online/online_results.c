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
#include "online/online_standings.h" /* PD-T6f: the ONE seat-ranking sort, shared
                                        DRY with online_ceremony.c so the champion
                                        the CEREMONY crowns is byte-for-byte the
                                        seat this screen ranks #1. */
#include "online/online_portraits.h" /* screens I-3: the ONE portrait/name/asset-id
                                        set, shared DRY across the three screens. */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Screen space (mirrored, like online_charselect.c). */
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
#define RES_MODE_SINGLE 0u        /* MDKR_ONLINE_MODE_SINGLE_RACE */
#define RES_MODE_TOURNAMENT 1u    /* MDKR_ONLINE_MODE_TOURNAMENT */
#define RES_LOCAL_PAD 0           /* PLAYER_ONE */
#define RES_SLOTS 4u              /* MDKR_ONLINE_RACE_RESULT_SLOTS / seats */
#define RES_PLACE_NONE 0xFFu      /* MDKR_ONLINE_RACE_RESULT_NONE */
#define RES_CUP_ROUNDS 4u         /* a DKR cup is four rounds (RACE n/4 copy) */
#define RES_PHASE_RESULTS 4u      /* MDKR_ONLINE_RESULTS (party_link phase byte;
                                   * LOBBY=1 LOADING=2 RACING=3 RESULTS=4) -- the
                                   * joiner's authoritative ADVANCE is this phase
                                   * leaving RESULTS (R-D convergence model). */
#define RES_INPUT_GRACE 30u       /* F3: manual-advance lockout at each stage entry
                                   * (~0.5s), so a host still mashing at the finish
                                   * line cannot skip the results on frame one. */

/* Two visible stages of the post-race screen. A single race stops at RESULTS
 * (there is no cup standings); a tournament walks RESULTS -> STANDINGS. */
#define RES_STAGE_RESULTS 0u
#define RES_STAGE_STANDINGS 1u

/* Visible countdowns (R7): results 15s, standings 10s. updateRate accumulates in
 * 60ths of a second, so seconds*60. */
#define RES_RESULTS_UNITS 900u   /* 15s */
#define RES_STANDINGS_UNITS 600u /* 10s */

/* Exit-gate C1: the FINAL-standings JOINER terminal self-advance backstop. The
 * HOST holds interactively on "A: FINISH", but a non-host joiner's forward feed
 * PARKS in RESULTS after the host finishes (the reducer never leaves RESULTS on a
 * final race -- no REMATCH, no CLOSE), so the joiner's old "wait for the phase to
 * leave RESULTS" exit could never fire in real play and it hung forever on the
 * last screen of every tournament. The joiner now gets a bound INDEPENDENT of the
 * host: a local A/B press advances immediately, and absent any input this generous
 * dwell (the same 10s sensibility as the standings countdown -- long enough for a
 * human to read the final table) self-advances -> LEAVE -> the joiner's OWN
 * per-endpoint CEREMONY -> FINISHED. The host is never auto-bounded here. */
#define RES_JOINER_TERMINAL_UNITS RES_STANDINGS_UNITS /* 10s */

/* Menu SFX (the real DKR enums; same reuse as CHARSELECT). */
#define RES_SFX_ADVANCE SOUND_SELECT3
#define RES_SFX_TICK SOUND_MENU_PICK2

/* Trophy weights {9,7,5,3,1,0,0,0} == gTrophyRacePointsArray (menu.c) /
 * kTrophyPoints (lobby_core.c). Two honest uses (M-2): (1) the headless stand-in
 * reducer test seam ACCRUES with them, and (2) the STANDINGS stage derives this
 * race's "+delta" purely for DISPLAY from snap.last_placements[]. The running
 * TOTAL is always read straight from snap.points[] -- the reducer's authority; the
 * screen never accumulates into or mutates the trophy/points state (R-E permits
 * the read-only weight mirror). */
static const u16 sTrophyPoints[8] = {9u, 7u, 5u, 3u, 1u, 0u, 0u, 0u};

/* Online id -> portrait / name / asset-id tables: screens I-3 DRY lift into the
 * shared online_portraits.h (byte-identical across charselect/results/ceremony;
 * sOnlineToPortrait[], sOnlineNames[], sPortraitAssetIds[] now live there). */

/* ---- Session-owned screen state (never an offline global) ------------------ */
typedef struct MdkrOnlineResultsState {
    u8 stage;         /* RES_STAGE_RESULTS / RES_STAGE_STANDINGS */
    u8 isFinal;       /* the last race of the soak/cup: standings HOLD (no advance) */
    u8 raceIndex;     /* 0-based cup round, for the "RACE n/4" copy */
    u8 assets;        /* portrait group + fonts loaded */
    u8 host;          /* local seat is the room leader (advance authority) */
    u8 leave;         /* B: back-out request (edge; PD-T6 return) */
    u8 haveResults;   /* THIS race's placements were read from the reducer feed */
    u8 advanced;      /* an ADVANCE was already returned for this stage (edge) */
    u8 advanceCommitted; /* T6ac: host committed the rematch advance -- republish
                          * rematch_requested EVERY tick until the room leaves
                          * RESULTS, THEN return ADVANCE (convergence, not a
                          * one-shot edge a dropped reverse-feed pump could lose) */
    u8 advanceAuto;      /* how the committed advance fired: 1 == countdown auto,
                          * 0 == host press (preserved across the convergence wait
                          * so the witness still reads "(auto)"/"(host)") */
    u8 prevSecs;      /* last countdown second drawn (for the hurry-up SFX edge) */
    u32 stageTicks;   /* countdown accumulator for the current stage */
    u32 pulseTicks;   /* free-running (drives the terminal-hold pulse, never reset) */
    u8 placements[RES_SLOTS]; /* THIS race's canonical-slot -> placement (poll) */
} MdkrOnlineResultsState;

static MdkrOnlineResultsState sRes;

/* Witness change-detect (file scope so _enter can reset for a clean re-entry). */
static u32 sWitnessKey = 0xFFFFFFFFu;

/* ---- forward decls (test seam at the bottom) ------------------------------ */
static void results_test_resolve(void);
static void results_test_capture(void);
static void results_test_pump(void);
static void results_test_reduce(void);
static u8 results_host_press_active(void);
static u8 results_joiner_finish_seam(void);          /* PD-T6d test seam */
static u8 results_joiner_finish_departed(u32 stageTicks);
static u8 results_joiner_terminal_seam(void);        /* exit-gate C1 no-seam proof */
static u8 results_remote_vacate_final_probe(void);   /* final-review P2 probe */

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

/* The host seat's short name (for the joiner's "WAITING FOR <host>..." line). */
static void results_host_name(const MdkrPartyLinkSnapshot *snap, bool haveSnap,
                              char *out, size_t cap) {
    unsigned i;
    (void) snprintf(out, cap, "HOST");
    if (!haveSnap) {
        return;
    }
    for (i = 0u; i < MDKR_PARTY_LINK_SEATS; i++) {
        if (snap->seats[i].occupied && snap->seats[i].is_host &&
            snap->seats[i].name[0] != '\0') {
            (void) snprintf(out, cap, "%.12s", snap->seats[i].name);
            return;
        }
    }
}

/* Triangle-wave pulse 0..16 off the free-running counter (the trackselect P4
 * vocabulary) -- gives the otherwise-static terminal "COMPLETE" hold a visible
 * heartbeat so it never reads as a hang (F4/F6). */
static s32 results_pulse(void) {
    s32 tri = (s32) (sRes.pulseTicks & 31u);
    if (tri > 16) {
        tri = 32 - tri;
    }
    return tri;
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

/* The "this screen is over, what does a button do now" footer, shared by the
 * single-race RESULTS terminal (F4, "RACE COMPLETE") and the tournament STANDINGS
 * terminal (F6, "CUP COMPLETE"): a pulsed label + an explicit host affordance
 * ("A: FINISH", wired to the LEAVE return) / the joiner's own self-advance
 * countdown to the champion celebration (exit-gate C1). No dead button. */
static void results_render_complete(const MdkrPartyLinkSnapshot *snap,
                                    bool haveSnap, const char *label) {
    s32 tri = results_pulse();
    u8 pg = (u8) (170 + tri * 5); /* 170..255 pulse */
    char line[48];

    results_text(RES_SCREEN_W_HALF, 208, ASSET_FONTS_SMALLFONT, (char *) label,
                 ALIGN_MIDDLE_CENTER, 120, pg, 120);
    if (sRes.host) {
        results_text(RES_SCREEN_W_HALF, 224, ASSET_FONTS_SMALLFONT, "A: FINISH",
                     ALIGN_MIDDLE_CENTER, 255, 255, 255);
    } else {
        /* Exit-gate C1: the JOINER self-advances off this terminal (its feed parks
         * in RESULTS, so it must not wait on the host). Show its own visible
         * countdown to the champion celebration -- never the old "WAITING FOR
         * HOST..." (misleading now, and the host may already be gone). A/B advances
         * immediately (P2-c: the navigation input is advertised). */
        u32 secs = results_seconds_left(RES_JOINER_TERMINAL_UNITS);
        s32 c = 130 + tri * 5;
        (void) snprintf(line, sizeof(line), "CONTINUE IN %us  (A)", secs);
        results_text(RES_SCREEN_W_HALF, 224, ASSET_FONTS_SMALLFONT, line,
                     ALIGN_MIDDLE_CENTER, c, c, c);
        (void) snap;
        (void) haveSnap;
    }
}

/* The non-terminal countdown footer: "<label> IN Ns" + the host's advance
 * affordance / the joiner's live waiting line (grey pulse, host name). */
static void results_render_countdown(const MdkrPartyLinkSnapshot *snap,
                                     bool haveSnap, const char *nextLabel,
                                     const char *hostVerb, u32 secs) {
    s32 tri = results_pulse();
    char line[48];

    (void) snprintf(line, sizeof(line), "%s IN %us", nextLabel, secs);
    results_text(RES_SCREEN_W_HALF, 208, ASSET_FONTS_SMALLFONT, line,
                 ALIGN_MIDDLE_CENTER, 200, 200, 255);
    if (sRes.host) {
        /* Screens I-2: advertise the (previously silent) non-final back-out. B on a
         * non-final results/standings is a mid-tournament LEAVE-to-room, so surface
         * it beside the host's advance affordance. */
        char hv[40];
        (void) snprintf(hv, sizeof(hv), "%s   B: LEAVE", hostVerb);
        results_text(RES_SCREEN_W_HALF, 224, ASSET_FONTS_SMALLFONT, hv,
                     ALIGN_MIDDLE_CENTER, 255, 255, 255);
    } else {
        char host[16];
        s32 c = 130 + tri * 5;
        results_host_name(snap, haveSnap, host, sizeof(host));
        (void) snprintf(line, sizeof(line), "WAITING FOR %.12s...", host);
        results_text(RES_SCREEN_W_HALF, 224, ASSET_FONTS_SMALLFONT, line,
                     ALIGN_MIDDLE_CENTER, c, c, c);
    }
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
    bool tournament = haveSnap && snap->mode == RES_MODE_TOURNAMENT;
    u32 secs = results_seconds_left(RES_RESULTS_UNITS);
    s32 rowY = 60;
    u8 place;
    char line[64];

    results_text(RES_SCREEN_W_HALF, 18, ASSET_FONTS_BIGFONT, "RACE RESULTS",
                 ALIGN_MIDDLE_CENTER, 255, 224, 96);
    /* Tournament: name the round on RESULTS too (parity with STANDINGS). */
    if (tournament) {
        (void) snprintf(line, sizeof(line), "RACE %u/%u",
                        (unsigned) (snap->race_index + 1u), RES_CUP_ROUNDS);
        results_text(RES_SCREEN_W_HALF, 38, ASSET_FONTS_SMALLFONT, line,
                     ALIGN_MIDDLE_CENTER, 200, 200, 255);
    }

    /* Walk placements in finishing order (place 0 == 1st). Each canonical slot
     * carries its own placement, so scan for the slot at each place. */
    for (place = 0u; place < RES_SLOTS; place++) {
        unsigned slot;
        for (slot = 0u; slot < RES_SLOTS; slot++) {
            bool isLocal;
            u8 character = RES_NO_CHARACTER;
            char name[32];
            char *placeLabel;
            s32 nr, ng, nb;

            if (sRes.placements[slot] != place) {
                continue;
            }
            isLocal = ((s32) slot == localSeat);
            if (haveSnap && snap->seats[slot].character_id < RES_CHAR_COUNT) {
                character = snap->seats[slot].character_id;
            }
            results_seat_name(snap, haveSnap, slot, name, sizeof(name));
            placeLabel = (place < 8u) ? gRacePlacementsArray[place] : (char *) "-";

            /* Colour + a text tag carry the "this is you" state (no colour-only
             * meaning): local = bright gold + [YOU], remote = plain white. */
            if (isLocal) {
                nr = 255; ng = 224; nb = 96;
            } else {
                nr = 220; ng = 220; nb = 220;
            }

            /* Row: place label | portrait | name -- laid out so the ~44px
             * portrait never overprints the label or the name. The place label
             * is FUNFONT: BIGFONT has NO digit glyphs (its '0'..'9' textureIDs are
             * 0xFF), so "1ST" would render "ST" -- the offline results screen
             * draws these exact strings in FUNFONT for the same reason (F1). Row
             * text sits at the portrait's optical centre (rowY+12). */
            results_text(52, rowY + 12, ASSET_FONTS_FUNFONT, placeLabel,
                         ALIGN_MIDDLE_RIGHT, nr, ng, nb);
            results_draw_portrait(character, 64, rowY - 8, (u8) nr, (u8) ng,
                                  (u8) nb);
            (void) snprintf(line, sizeof(line), "%.12s%s", name,
                            isLocal ? " [YOU]" : "");
            results_text(118, rowY + 12, ASSET_FONTS_SMALLFONT, line,
                         ALIGN_MIDDLE_LEFT, nr, ng, nb);
            rowY += 40;
            break;
        }
    }

    /* Footer: a single race's FINAL result is a terminal hold (F4 -- no
     * countdown, no dead button); otherwise a live countdown to the next step. */
    if (sRes.isFinal && !tournament) {
        results_render_complete(snap, haveSnap, "RACE COMPLETE");
    } else if (tournament) {
        results_render_countdown(snap, haveSnap, "STANDINGS", "A: CONTINUE", secs);
    } else {
        results_render_countdown(snap, haveSnap, "NEXT RACE", "A: CONTINUE", secs);
    }
}

/* STANDINGS stage: the cup points table (portraits + accumulated points, sorted
 * descending), this race's delta shown, "RACE n/4". */
static void results_render_standings(const MdkrPartyLinkSnapshot *snap,
                                     bool haveSnap, s32 localSeat) {
    u32 secs = results_seconds_left(RES_STANDINGS_UNITS);
    u16 points[RES_SLOTS];
    u8 order[RES_SLOTS];
    u8 lastpl[RES_SLOTS];
    unsigned nseats = 0u;
    unsigned i;
    s32 rowY = 64;
    char line[64];

    results_text(RES_SCREEN_W_HALF, 18, ASSET_FONTS_BIGFONT,
                 sRes.isFinal ? "FINAL STANDINGS" : "STANDINGS",
                 ALIGN_MIDDLE_CENTER, 255, 224, 96);
    /* M-6: never show "RACE n/N" alongside the FINAL banner (the cup is over). */
    if (haveSnap && !sRes.isFinal) {
        (void) snprintf(line, sizeof(line), "RACE %u/%u",
                        (unsigned) (snap->race_index + 1u), RES_CUP_ROUNDS);
        results_text(RES_SCREEN_W_HALF, 38, ASSET_FONTS_SMALLFONT, line,
                     ALIGN_MIDDLE_CENTER, 200, 200, 255);
    }

    /* Collect the occupied seats + their (reducer-accrued) points from the
     * snapshot, then selection-sort by points descending, tie-broken on THIS
     * race's finish (lower last_placement wins) so equal totals are not
     * host-biased by seat order. PD-T6f: the collect+sort is the shared
     * mdkr_online_standings_compute() helper (byte-identical to the loop that
     * lived here) so the CEREMONY's champion always agrees with this #1. */
    {
        MdkrOnlineStandings st;
        mdkr_online_standings_compute(snap, haveSnap, &st);
        nseats = st.count;
        for (i = 0u; i < nseats; i++) {
            order[i] = st.order[i];
            points[i] = st.points[i];
            lastpl[i] = st.lastpl[i];
        }
    }

    for (i = 0u; i < nseats; i++) {
        unsigned slot = order[i];
        bool isLocal = ((s32) slot == localSeat);
        u8 character = RES_NO_CHARACTER;
        char name[32];
        s32 nr, ng, nb;
        /* This race's +delta, DISPLAY-only, from last_placement's trophy weight
         * (M-2: the running total below is read straight from snap.points[]). */
        u16 delta = (lastpl[i] < 8u) ? sTrophyPoints[lastpl[i]] : 0u;

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
         * The rank is FUNFONT: BIGFONT has no digit glyphs, so "%u." would render
         * a bare "." (F2). Row text at the portrait's optical centre (rowY+12). */
        (void) snprintf(line, sizeof(line), "%u.", i + 1u);
        results_text(44, rowY + 12, ASSET_FONTS_FUNFONT, line,
                     ALIGN_MIDDLE_RIGHT, nr, ng, nb);
        results_draw_portrait(character, 52, rowY - 8, (u8) nr, (u8) ng,
                              (u8) nb);
        (void) snprintf(line, sizeof(line), "%.10s%s", name,
                        isLocal ? " [YOU]" : "");
        results_text(104, rowY + 12, ASSET_FONTS_SMALLFONT, line,
                     ALIGN_MIDDLE_LEFT, nr, ng, nb);
        /* Points in FUNFONT (the trophy-rankings vocabulary), with this race's
         * delta so a newcomer sees WHY the total moved. */
        (void) snprintf(line, sizeof(line), "%u", (unsigned) points[i]);
        results_text(252, rowY + 12, ASSET_FONTS_FUNFONT, line,
                     ALIGN_MIDDLE_RIGHT, nr, ng, nb);
        if (delta > 0u) {
            (void) snprintf(line, sizeof(line), "+%u", (unsigned) delta);
            results_text(300, rowY + 12, ASSET_FONTS_SMALLFONT, line,
                         ALIGN_MIDDLE_RIGHT, 120, 255, 120);
        }
        rowY += 40;
    }

    /* Footer: the FINAL standings is a terminal hold (F6 -- pulsed CUP COMPLETE +
     * host A:FINISH / joiner waiting); otherwise a live countdown to the next
     * race. */
    if (sRes.isFinal) {
        results_render_complete(snap, haveSnap, "CUP COMPLETE");
    } else {
        results_render_countdown(snap, haveSnap, "NEXT RACE", "A: NEXT RACE",
                                 secs);
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
    /* M-5: placements are 8-bit (0xFF == absent), but they only get 4 bits each
     * in the key -- mask to a nibble (0xFF -> 0xF) so an absent seat cannot smear
     * the higher secs/mode/points fields and silently weaken the countdown
     * change-detect. */
    u8 pl0 = (sRes.placements[0] == RES_PLACE_NONE) ? 0xFu
                                                    : (u8) (sRes.placements[0] & 0xFu);
    u8 pl1 = (sRes.placements[1] == RES_PLACE_NONE) ? 0xFu
                                                    : (u8) (sRes.placements[1] & 0xFu);
    u32 key = ((u32) sRes.stage) | ((u32) pl0 << 1) | ((u32) pl1 << 5) |
              ((u32) (secs & 0x1Fu) << 9) | ((u32) mode << 14) |
              ((u32) (p0 & 0xFFu) << 15) | ((u32) (p1 & 0xFFu) << 23);
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
    sRes.prevSecs = 0xFFu; /* no countdown second drawn yet (hurry-up SFX edge) */
    for (i = 0u; i < RES_SLOTS; i++) {
        sRes.placements[i] = RES_PLACE_NONE;
    }
    sWitnessKey = 0xFFFFFFFFu;

    /* PD-T6ac POLL-CONTENTION RESOLUTION. This screen NO LONGER consumes the
     * one-shot engine results poll -- that single owner is the LAUNCHER pump,
     * which polls mid-residency and PUBLISH_RESULTS to the reducer. THIS race's
     * finishing order is read from the forward-feed snapshot's last_placements[]
     * (which the reducer records at PUBLISH_RESULTS), exactly as the cup points
     * are already read from snapshot.points[]. In the headless soak the launcher
     * is stood in by the RESULTS test seam, which OWNS the poll there too:
     * results_test_capture() polls + accrues + publishes the RESULTS snapshot
     * BELOW so we read it here (inert in a live run -- the real launcher pump has
     * already published). */
    results_test_capture();
    {
        MdkrPartyLinkSnapshot snap;
        /* Only a genuine RESULTS publication carries a finishing order; the
         * reducer reaches RESULTS ONLY via PUBLISH_RESULTS, so phase==RESULTS is
         * the authoritative "placements are present" signal (a bare/LOBBY snapshot
         * never sets haveResults). */
        if (mdkr_party_link_read(&snap) &&
            snap.phase == (uint8_t) RES_PHASE_RESULTS) {
            for (i = 0u; i < RES_SLOTS; i++) {
                sRes.placements[i] = snap.last_placements[i];
            }
            sRes.haveResults = 1u;
        }
    }

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

/* Scripted headless input (only under the test seam): the HOST presses ADVANCE
 * right after the F3 entry lockout on the RESULTS stage, proving host-press
 * advance. The STANDINGS stage is deliberately left un-pressed so BOTH the
 * host-advance (RESULTS) and the countdown-to-zero auto-advance (STANDINGS) paths
 * are exercised in one soak; the final STANDINGS then holds (F6). */
static void results_input_scripted(ResInput *in) {
    memset(in, 0, sizeof(*in));
    if (sRes.stageTicks < RES_INPUT_GRACE) {
        return;
    }
    if (sRes.stage == RES_STAGE_RESULTS) {
        in->advanceEdge = 1u; /* RESULTS -> STANDINGS (host press) */
    } else if (results_host_press_active()) {
        /* Final-review P2 probe: at the FINAL standings, "the host has vacated" ->
         * script NO terminal press, so the joiner leaves via its self-advance DWELL
         * (making the dwell-vs-vacate race unambiguous). Non-final standings still
         * press (the cup advances fast). */
        if (sRes.isFinal && results_remote_vacate_final_probe()) {
            return;
        }
        /* PD-T6ac live-resident lane: also press the STANDINGS stage so the host
         * advance (-> REMATCH) fires promptly rather than after the full ~10s
         * countdown, keeping the headless resident lane fast. The scripted soak
         * (host_press_both false) deliberately leaves STANDINGS on the auto path. */
        in->advanceEdge = 1u;
    }
}

static void results_gather_input(ResInput *in) {
    /* Scripted host input when EITHER the stand-in-reducer soak seam
     * (MDKR_TEST_ONLINE_RESIDENT) OR the live-resident host-press seam
     * (MDKR_TEST_ONLINE_RESULTS_HOST_PRESS) is armed; live pad otherwise. */
    if (mdkr_online_results_test_active() || results_host_press_active()) {
        results_input_scripted(in);
    } else {
        results_input_live(in);
    }
}

/* PD-T6b/T6ac: the HOST publishes its post-race REMATCH intent on the reverse
 * feed to "start the next race". The launcher pump polls it and drives the
 * reducer's leader-only MDKR_ONLINE_REMATCH (return to LOBBY + tournament
 * race_index++).
 *
 * CONVERGENCE MODEL (carried T6b Important finding): once the host commits the
 * advance off a NON-final tournament STANDINGS, this is republished EVERY tick
 * (silently) until the snapshot phase leaves RESULTS -- a one-shot EDGE could be
 * dropped by a missed/failed/overwritten reverse-feed pump and permanently wedge
 * the room. The commit is LOGGED once (by the caller) so the witness stays a
 * per-race event. It is NEVER published for single-race, the final tournament
 * race, or the host's "A: FINISH" on the final standings -- those are the
 * LEAVE/finish path. A joiner never publishes it (watch-only). Inert (no-op) when
 * the bridge is uninstalled. */
static void results_publish_rematch(void) {
    MdkrPartyLinkLocalIntent intent;
    mdkr_party_link_intent_init(&intent);
    intent.rematch_requested = 1u;
    mdkr_party_link_intent_publish(&intent);
}

MdkrOnlineResultsResult mdkr_online_results_tick(s32 updateRate) {
    MdkrPartyLinkSnapshot snap;
    bool haveSnap;
    s32 localSeat;
    bool tournament;
    bool terminal;
    u32 limit;
    u32 secs;
    ResInput in;
    u8 manualEdge;
    u8 autoFire;

    if (updateRate <= 0) {
        updateRate = 1;
    }

    /* Stand-in reducer (headless soak): install the forward feed + publish the
     * scripted snapshot (points/mode/race_index). Inert in a normal run. */
    results_test_pump();

    haveSnap = mdkr_party_link_read(&snap);
    localSeat = haveSnap ? results_local_seat(&snap) : -1;
    /* M-1 / F5: a feed-less endpoint (no snapshot at all -- legacy direct boot)
     * owns the progression; but when a feed IS present, be a JOINER unless the
     * resolved local seat is the host. The old "no local seat -> assume host"
     * fallback wrongly granted advance authority to a feed-having endpoint that
     * could not resolve its seat, which would split a live room. The resident
     * soak's local seat IS host, so it still drives via the host path below. */
    if (!haveSnap) {
        sRes.host = 1u;
    } else {
        sRes.host = (localSeat >= 0 && snap.seats[localSeat].is_host) ? 1u : 0u;
    }
    tournament = haveSnap && snap.mode == RES_MODE_TOURNAMENT;
    limit = (sRes.stage == RES_STAGE_RESULTS) ? RES_RESULTS_UNITS
                                              : RES_STANDINGS_UNITS;
    /* The terminal screen holds (no ADVANCE): the final race's last shown stage
     * -- tournament STANDINGS (F6), or a single race's RESULTS (F4). */
    terminal = sRes.isFinal && (sRes.stage == RES_STAGE_STANDINGS ||
                                (sRes.stage == RES_STAGE_RESULTS && !tournament));

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
    sRes.pulseTicks += (u32) updateRate;

    /* Hurry-up SFX on the last three countdown seconds (native feel), only while
     * a live countdown is shown -- never on the terminal hold. */
    secs = results_seconds_left(limit);
    if (!terminal && secs != (u32) sRes.prevSecs && secs >= 1u && secs <= 3u) {
        sound_play(RES_SFX_TICK, NULL);
    }
    sRes.prevSecs = (u8) (secs > 255u ? 255u : secs);

    /* Advance authority (R7 / R-D):
     *  - HOST: a manual A/START edge (after the F3 entry lockout) OR the countdown
     *    reaching zero -- host is the room authority.
     *  - JOINER: never returns the room-affecting ADVANCE on its own countdown;
     *    it FOLLOWS the authoritative snapshot phase leaving RESULTS. */
    manualEdge = (in.advanceEdge && sRes.stageTicks >= RES_INPUT_GRACE) ? 1u : 0u;
    autoFire = (sRes.stageTicks >= limit) ? 1u : 0u;

    if (terminal) {
        /* HOST: the interactive hold. "A: FINISH" -> LEAVE, which the session maps
         * to FINISHED via resultsIsFinal (PD-T6d handshake -> CEREMONY -> FINISHED).
         * The host is NEVER auto-bounded here (its hold is legitimate + interactive
         * -- do NOT give the host a countdown). The test seam suppresses this so the
         * JOINER path below can be exercised on a rig where the visible endpoint
         * drove the rounds as host. */
        if (sRes.host && manualEdge && !results_joiner_finish_seam() &&
            !results_joiner_terminal_seam()) {
            fprintf(stderr, "[online-results] finish: host A -> LEAVE\n");
            return MDKR_ONLINE_RESULTS_LEAVE;
        }
        /* JOINER (exit-gate C1): a bound INDEPENDENT of the host. The joiner's feed
         * PARKS in RESULTS after the host finishes (the phase never leaves RESULTS),
         * so the old "wait for the phase to depart RESULTS" exit could never fire in
         * real play -- the joiner hung forever. It now LEAVEs on ANY of:
         *   - a local A/START or B press (honored immediately -- the terminal no
         *     longer swallows the joiner's input before the leave check), OR
         *   - the generous self-advance dwell elapsing (advances with NO input), OR
         *   - the authoritative phase actually leaving RESULTS / the departed test
         *     seam (kept for symmetry + the pre-existing seam scenario).
         * All route to LEAVE -> CEREMONY -> FINISHED (the joiner's own per-endpoint
         * celebration, already no-hang). This never pre-empts the host: the host
         * runs the sRes.host branch above and holds until it presses A. */
        if (!sRes.host || results_joiner_finish_seam() ||
            results_joiner_terminal_seam()) {
            u8 joinerPress = ((in.advanceEdge || in.bEdge) &&
                              sRes.stageTicks >= RES_INPUT_GRACE)
                                 ? 1u
                                 : 0u;
            u8 joinerDwell =
                (sRes.stageTicks >= RES_JOINER_TERMINAL_UNITS) ? 1u : 0u;
            u8 feedDeparted =
                ((haveSnap && snap.phase != (uint8_t) RES_PHASE_RESULTS) ||
                 results_joiner_finish_departed(sRes.stageTicks))
                    ? 1u
                    : 0u;
            if (joinerPress || joinerDwell || feedDeparted) {
                fprintf(stderr,
                        "[online-results] finish: joiner terminal advance (%s) -> "
                        "LEAVE\n",
                        joinerPress ? "press"
                                    : (feedDeparted ? "feed-departed"
                                                    : "self-advance"));
                return MDKR_ONLINE_RESULTS_LEAVE;
            }
        }
        return MDKR_ONLINE_RESULTS_STAY;
    }

    if (sRes.stage == RES_STAGE_RESULTS && tournament) {
        /* Internal RESULTS -> STANDINGS (no room-phase change; a display step).
         * Host press/auto drives it; a joiner runs its own countdown so its view
         * keeps moving -- the room-affecting ADVANCE is snapshot-gated at the
         * STANDINGS stage below, so this cannot split the room. */
        u8 toStandings = sRes.host ? (manualEdge || autoFire) : autoFire;
        if (toStandings) {
            sound_play(RES_SFX_ADVANCE, NULL);
            sRes.stage = RES_STAGE_STANDINGS;
            sRes.stageTicks = 0u;
            sRes.prevSecs = 0xFFu;
            fprintf(stderr,
                    "[online-results] advance: results -> standings (%s)\n",
                    sRes.host ? (manualEdge ? "host" : "auto") : "joiner-auto");
            return MDKR_ONLINE_RESULTS_STAY;
        }
    } else if (sRes.host && tournament && sRes.stage == RES_STAGE_STANDINGS) {
        /* PD-T6ac CONVERGENCE-DRIVEN host advance off a NON-final tournament
         * STANDINGS ("start the next race" -> REMATCH). On the host's commit edge
         * (press/auto) latch committed + LOG the rematch once; then republish the
         * rematch intent EVERY tick until the snapshot phase leaves RESULTS, and
         * only THEN return the room-affecting ADVANCE. A one-shot edge here could
         * be lost to a dropped/overwritten reverse-feed pump and wedge the room;
         * republishing to convergence is robust (mirrors the char/trackselect
         * convergence model). */
        if (!sRes.advanceCommitted && (manualEdge || autoFire)) {
            sRes.advanceCommitted = 1u;
            sRes.advanceAuto = manualEdge ? 0u : 1u; /* host press vs countdown */
            sound_play(RES_SFX_ADVANCE, NULL);
            fprintf(stderr,
                    "[online-results] publish: rematch (host advance -> next "
                    "race)\n");
        }
        if (sRes.advanceCommitted) {
            results_publish_rematch();   /* republish EVERY tick until converged */
            results_test_reduce();       /* soak stand-in reducer (inert if off) */
            if (haveSnap && snap.phase != (uint8_t) RES_PHASE_RESULTS) {
                fprintf(stderr, "[online-results] advance: screen done (%s)\n",
                        sRes.advanceAuto ? "auto" : "host");
                return MDKR_ONLINE_RESULTS_ADVANCE;
            }
            /* Committed but the room is still in RESULTS: hold + keep republishing
             * ("STARTING NEXT RACE..."). */
            return MDKR_ONLINE_RESULTS_STAY;
        }
    } else {
        /* Screen done (single-race non-final RESULTS, or a joiner following the
         * authoritative phase). Host: press/auto (one-shot -- no REMATCH here).
         * Joiner: ONLY when the snapshot phase has left RESULTS. */
        u8 hostAdvance = (sRes.host && (manualEdge || autoFire)) ? 1u : 0u;
        u8 joinerFollow = (!sRes.host && haveSnap &&
                           snap.phase != (uint8_t) RES_PHASE_RESULTS) ? 1u : 0u;
        if ((hostAdvance || joinerFollow) && !sRes.advanced) {
            sound_play(RES_SFX_ADVANCE, NULL);
            sRes.advanced = 1u;
            fprintf(stderr, "[online-results] advance: screen done (%s)\n",
                    hostAdvance ? (manualEdge ? "host" : "auto")
                                : "joiner-follow");
            return MDKR_ONLINE_RESULTS_ADVANCE;
        }
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
static u8 sTestLastPlacements[RES_SLOTS]; /* THIS race's finish order (the poll) */
static u8 sTestSeatChar[RES_SLOTS] = {0u, 5u, 0xFFu, 0xFFu}; /* Diddy / Bumper */
/* PD-T6b: the stand-in reducer's own cup round counter -- advanced ONLY when it
 * observes the REMATCH reverse-feed intent (results_test_reduce), NEVER off the
 * session's boot count, so the soak proves the results screen drives the next
 * race via rematch (not the old start signal). It is what the scripted forward
 * feed publishes as race_index. */
static u8 sTestRaceIndex;

static void results_test_resolve(void) {
    if (sTestActive < 0) {
        /* M-4: uniform truthiness with online_session.c / main_app.cpp -- the flag
         * is the RACE COUNT, so "=0" means OFF (a half-armed seam proves nothing). */
        const char *e = getenv("MDKR_TEST_ONLINE_RESIDENT");
        sTestActive = (e != NULL && strtoul(e, NULL, 10) > 0ul) ? 1 : 0;
    }
}

/* PD-T6ac stand-in LAUNCHER (headless soak only): the single owner of the
 * one-shot engine results poll (mirroring the live launcher pump), called from
 * _enter BEFORE the screen reads the snapshot. It polls THIS race's captured
 * finishing order, accrues the trophy points into the running cup total, and
 * PUBLISHES a forward-feed snapshot (mode=TOURNAMENT, phase=RESULTS, race_index,
 * points[], last_placements[], two seats) -- so the RESULTS screen reads
 * placements from the snapshot (never the poll), exactly as on the live feed.
 * Inert in a normal/live run. */
static void results_test_capture(void) {
    unsigned i;
    u8 polled[MDKR_ONLINE_RACE_RESULT_SLOTS];
    results_test_resolve();
    if (!sTestActive) {
        return;
    }
    for (i = 0u; i < RES_SLOTS; i++) {
        sTestLastPlacements[i] = RES_PLACE_NONE;
    }
    /* OWN the consuming poll (stand-in for the launcher's mid-residency poll). */
    if (mdkr_online_race_results_poll(polled)) {
        for (i = 0u; i < RES_SLOTS; i++) {
            sTestLastPlacements[i] = polled[i];
        }
    }
    memset(&sTestRoom, 0, sizeof(sTestRoom));
    sTestRoom.mode = (uint8_t) RES_MODE_TOURNAMENT;
    sTestRoom.phase = (uint8_t) RES_PHASE_RESULTS; /* MDKR_ONLINE_RESULTS */
    /* PD-T6b: the round is what the stand-in reducer has advanced via observed
     * REMATCH intents (results_test_reduce), not the session boot count. */
    sTestRoom.race_index = sTestRaceIndex;
    sTestRoom.configured_track = 0xFFFFu;
    sTestRoom.cup_id = 2u; /* Sherbet cup (matches the trackselect joiner lane) */
    for (i = 0u; i < RES_SLOTS; i++) {
        u8 place = sTestLastPlacements[i];
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
    if (!sTestInstalled || !mdkr_party_link_active()) {
        mdkr_party_link_clear();
        (void) mdkr_party_link_install();
        sTestInstalled = 1u;
        fprintf(stderr,
                "[online-results] test-script install (scripted STANDINGS feed; "
                "points=%u,%u)\n",
                (unsigned) sTestRoom.points[0], (unsigned) sTestRoom.points[1]);
    }
    mdkr_party_link_publish(&sTestRoom); /* ready for _enter to read below */
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
    }
    mdkr_party_link_publish(&sTestRoom);
}

/* PD-T6b/T6ac stand-in reducer: polls the RESULTS screen's republished REMATCH
 * intent and, on rematch_requested, advances the scripted room's cup round AND
 * leaves the RESULTS phase (-> LOBBY) -- exactly what the launcher reducer's
 * leader-only, RESULTS-gated MDKR_ONLINE_REMATCH does. The phase gate makes the
 * every-tick republish idempotent (race_index advances exactly once), and the
 * phase-leaves-RESULTS is the screen's convergence signal to finally ADVANCE.
 * Inert in a normal run. */
static void results_test_reduce(void) {
    MdkrPartyLinkLocalIntent intent;
    results_test_resolve();
    if (!sTestActive) {
        return;
    }
    if (mdkr_party_link_intent_poll(&intent) && intent.rematch_requested) {
        if (sTestRoom.phase == (uint8_t) RES_PHASE_RESULTS) {
            if (sTestRaceIndex < 0xFFu) {
                sTestRaceIndex++;
            }
            sTestRoom.phase = 1u; /* MDKR_ONLINE_LOBBY -- convergence signal */
            fprintf(stderr,
                    "[online-results] test-reducer: rematch observed -> "
                    "race_index=%u\n",
                    (unsigned) sTestRaceIndex);
        }
    }
}

/* PD-T6ac live-resident host-press seam (env MDKR_TEST_ONLINE_RESULTS_HOST_PRESS):
 * make the RESULTS screen advance via a scripted HOST press (both stages) instead
 * of the ~25s auto countdown, so the LIVE-loopback resident lane runs fast and
 * proves the host-advance -> REMATCH path explicitly. This does NOT enable the
 * stand-in reducer/feed above (that stays MDKR_TEST_ONLINE_RESIDENT-only): the
 * live launcher pump owns the real feed. Inert unless the env is set. */
static s8 sHostPressActive = -1;
static u8 results_host_press_active(void) {
    if (sHostPressActive < 0) {
        const char *e = getenv("MDKR_TEST_ONLINE_RESULTS_HOST_PRESS");
        sHostPressActive = (e != NULL && e[0] != '\0') ? 1 : 0;
    }
    return (u8) (sHostPressActive > 0 ? 1 : 0);
}

/* PD-T6d IMPORTANT-1 proof seam (env MDKR_TEST_ONLINE_RESULTS_JOINER_FINISH): at
 * the FINAL standings only, act as a JOINER whose host has departed RESULTS so the
 * non-host terminal-follow return is exercised end-to-end (engine FINISHED note +
 * launcher reason=FINISHED). It suppresses the host "A: FINISH" at the terminal and
 * forces the joiner + host-departed inputs after the render grace. A real transport
 * host-departure can't be cheaply staged on the loopback rig; the follow DECISION,
 * FINISHED mapping and launcher read all run genuinely. Inert unless the env is set;
 * confined to the terminal branch, so rounds 1..N-1 advance normally (as host). */
static s8 sJoinerFinishActive = -1;
static u8 results_joiner_finish_seam(void) {
    if (sJoinerFinishActive < 0) {
        const char *e = getenv("MDKR_TEST_ONLINE_RESULTS_JOINER_FINISH");
        sJoinerFinishActive = (e != NULL && e[0] != '\0') ? 1 : 0;
    }
    return (u8) (sJoinerFinishActive > 0 ? 1 : 0);
}
static u8 results_joiner_finish_departed(u32 stageTicks) {
    return (u8) ((results_joiner_finish_seam() && stageTicks >= RES_INPUT_GRACE)
                     ? 1 : 0);
}

/* Exit-gate C1 NO-SEAM proof seam (env MDKR_TEST_ONLINE_RESULTS_JOINER_TERMINAL):
 * at the FINAL standings ONLY, route the terminal into the JOINER branch (suppress
 * the host "A: FINISH"), WITHOUT forcing results_joiner_finish_departed -- so the
 * forward feed genuinely stays in RESULTS (the loopback reducer parks there on the
 * final race, no REMATCH/CLOSE) and the joiner leaves via the REAL production
 * paths only: the self-advance DWELL, or an honored A/B press. This is the seam
 * that proves the C1 fix without the old MDKR_TEST_ONLINE_RESULTS_JOINER_FINISH
 * departure that MASKED the bug. It only flips the terminal ROLE (the visible
 * loopback endpoint is the host, so a real 2-process joiner cannot be cheaply
 * staged); the self-advance/press DECISION, the FINISHED mapping via CEREMONY and
 * the launcher read all run genuinely. Inert unless the env is set; confined to
 * the terminal branch, so rounds 1..N-1 advance normally (as host). */
static s8 sJoinerTerminalActive = -1;
static u8 results_joiner_terminal_seam(void) {
    if (sJoinerTerminalActive < 0) {
        const char *e = getenv("MDKR_TEST_ONLINE_RESULTS_JOINER_TERMINAL");
        sJoinerTerminalActive = (e != NULL && e[0] != '\0') ? 1 : 0;
    }
    return (u8) (sJoinerTerminalActive > 0 ? 1 : 0);
}

/* Final-review P2 probe (env MDKR_TEST_ONLINE_REMOTE_VACATE_AT_RESULTS_FINAL):
 * when armed, the paired online_session detector reads the remote as GONE at the
 * FINAL standings. To make the joiner's SELF-ADVANCE DWELL (not a scripted press)
 * the thing that leaves -- so the dwell (10s) vs the vacate detector (0.75s) race
 * is unambiguous -- suppress the scripted terminal press below when this is set.
 * Rounds 1..N-1 still press (fast); only the terminal waits out the dwell. Inert
 * (no suppression) in every normal run and in the other seam lanes. */
static s8 sRemoteVacateFinalProbe = -1;
static u8 results_remote_vacate_final_probe(void) {
    if (sRemoteVacateFinalProbe < 0) {
        const char *e = getenv("MDKR_TEST_ONLINE_REMOTE_VACATE_AT_RESULTS_FINAL");
        sRemoteVacateFinalProbe = (e != NULL && e[0] != '\0') ? 1 : 0;
    }
    return (u8) (sRemoteVacateFinalProbe > 0 ? 1 : 0);
}

u8 mdkr_online_results_test_active(void) {
    results_test_resolve();
    return (u8) (sTestActive > 0 ? 1 : 0);
}

#endif /* MDKR_ENABLE_ONLINE_BETA */
