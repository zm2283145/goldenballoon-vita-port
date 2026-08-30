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
#include "textures_sprites.h" /* rendermode_reset (TU-local blue-box fill restore) */
#include "audio.h"      /* sound_play */
#include "sound_ids.h"  /* SOUND_SELECT2 / SOUND_MENU_PICK2 / ... */
#include "joypad.h"     /* input_pressed */
#include "PR/os_cont.h" /* A_BUTTON / START_BUTTON */
#include "net/party_link.h"
#include "net/online_race_results.h"
#include "online/online_standings.h" /* the ONE seat-ranking sort, shared
                                        DRY with online_ceremony.c so the champion
                                        the CEREMONY crowns is byte-for-byte the
                                        seat this screen ranks #1. */
#include "online/online_portraits.h" /* the ONE portrait/name/asset-id
                                        set, shared DRY across the three screens. */
#include "online/online_screen_constants.h" /* shared screen size + lobby id-space
                                               mirrors (DRY across screens) */
#include "online/online_screen_util.h" /* shared local_seat / text / pulse /
                                          seat_name / seconds_left / draw_portrait */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* gCurrDisplayList + gRacerPortraits[] are declared in online_screen_util.h,
 * shared with the other native screens. The real "1ST".."8TH" place labels
 * (menu.c gRacePlacementsArray) are declared here (menu.h does not export it)
 * rather than by editing menu.c. The screen size + shared launcher lobby id-space
 * mirrors live in online_screen_constants.h. */
extern char *gRacePlacementsArray[8];

/* Engine flat-fill vocabulary for the TU-LOCAL translucent BLUE dialogue box (the
 * retail RANKINGS options ground -- set_current_dialogue_background_colour(7,
 * 64,64,255,...)). The SAME read-only borrow of the font module's draw-mode lists
 * that online_screen_util.c makes for its navy panel, declared HERE too so the blue
 * box is a TU-local helper: the shared panel is a navy board other screens depend
 * on and this wave must not change it (results_blue_box below). */
extern Gfx dDialogueBoxBegin[];
extern Gfx dDialogueBoxDrawModes[][2];

/* ---- Local mirrors of the launcher lobby's id space (no launcher headers) --- */
#define RES_SLOTS 4u              /* MDKR_ONLINE_RACE_RESULT_SLOTS / seats */
#define RES_PLACE_COUNT 8u        /* MDKR_ONLINE_PLACEMENT_COUNT -- a placement is a
                                   * finish position among the FULL grid (up to 8
                                   * racers once AI fill / >4 players ship), so the
                                   * finishing-order scan must range 0..7, not 0..3. */
#define RES_PLACE_NONE 0xFFu      /* MDKR_ONLINE_RACE_RESULT_NONE */
#define RES_CUP_ROUNDS 4u         /* a DKR cup is four rounds (RACE n/4 copy) */
#define RES_PHASE_RESULTS 4u      /* MDKR_ONLINE_RESULTS (party_link phase byte;
                                   * LOBBY=1 LOADING=2 RACING=3 RESULTS=4) -- the
                                   * joiner's authoritative ADVANCE is this phase
                                   * leaving RESULTS (the convergence model). */
#define RES_INPUT_GRACE 30u       /* manual-advance lockout at each stage entry
                                   * (~0.5s), so a host still mashing at the finish
                                   * line cannot skip the results on frame one. */

/* Session-scoped per-seat placement tally (DISPLAY-ONLY): how many times each
 * canonical seat finished 1st/2nd/3rd/... across every race of this session -- the
 * native mirror of retail's settings->racers[].placements the RANKINGS screen
 * shows. NOT part of sRes (which _enter memsets each race): it survives the per-race
 * re-enter, and the engine process lifetime IS the session lifetime in the
 * separated-boot online flow, so this accrues across TRY AGAIN replays exactly as
 * the retail session standings do. The reducer never reads it (presentation only);
 * _enter accrues this race's placements into it (once per RESULTS entry). */
static u16 sSessionTally[RES_SLOTS][RES_PLACE_COUNT];

/* Visible stages of the post-race screen. A single race shows RESULTS; a
 * tournament walks RESULTS -> STANDINGS. When the native "more races" chooser is
 * armed (real play / the chooser lane) the SESSION DECISION POINT (a single race's
 * RESULTS, or a tournament's FINAL standings) hands to the CHOOSER stage instead
 * of the old terminal hold -- the full retail replay menu (online_results.h). */
#define RES_STAGE_RESULTS 0u
#define RES_STAGE_STANDINGS 1u
#define RES_STAGE_CHOOSER 2u

/* Dwell before the "more races" chooser fronts -- a single race's RESULTS OR a
 * tournament's FINAL standings, so the finishing order / standings is readable first
 * (host A skips it early); ~2s at 60Hz. */
#define RES_CHOOSER_FRONT_DWELL 120u

/* The joiner-mirror test seam departs the feed (-> LOBBY) after this many
 * CHOOSER-stage ticks, standing in for the host's authoritative REMATCH landing so
 * the joiner follows into re-selection. Test seam only (inert in a normal run). */
#define RES_CHOOSER_JOINER_STANDIN_DWELL 200u

/* One entry in a mode's "more races" option list: the on-screen label + the choice
 * the session routes on. The list is per-mode (single offers CHANGE TRACK, a
 * tournament offers CHANGE CUP -- the retail-faithful pick), and every distinct
 * MdkrOnlineResultsChoice is reachable across the two. */
typedef struct MdkrResChooserOption {
    const char *label;
    u8 choice; /* MdkrOnlineResultsChoice */
} MdkrResChooserOption;

#define RES_CHOOSER_MAX 6u
#define RES_CHOOSER_ROW_CHANGE 1u /* the sole per-mode row (CHANGE TRACK / CHANGE CUP) */

/* The "more races" option list. Both modes share every row except
 * RES_CHOOSER_ROW_CHANGE, so the base list is the SINGLE-race one and a tournament
 * substitutes only that one row (results_chooser_option()) -- one table, one
 * per-mode row. Every distinct MdkrOnlineResultsChoice is reachable across the two. */
static const MdkrResChooserOption sChooser[RES_CHOOSER_MAX] = {
    {"RACE AGAIN", (u8) MDKR_ONLINE_RESULTS_CHOICE_RACE_AGAIN},
    {"CHANGE TRACK", (u8) MDKR_ONLINE_RESULTS_CHOICE_CHANGE_TRACK}, /* tournament -> CHANGE CUP */
    {"CHANGE MODE", (u8) MDKR_ONLINE_RESULTS_CHOICE_CHANGE_MODE},
    {"NEW TOURNAMENT", (u8) MDKR_ONLINE_RESULTS_CHOICE_NEW_TOURNAMENT},
    {"CHANGE CHARACTER", (u8) MDKR_ONLINE_RESULTS_CHOICE_CHANGE_CHAR},
    {"FINISH", (u8) MDKR_ONLINE_RESULTS_CHOICE_FINISH},
};
/* The one tournament override, substituted at row RES_CHOOSER_ROW_CHANGE. */
static const MdkrResChooserOption sChooserTournamentChange = {
    "CHANGE CUP", (u8) MDKR_ONLINE_RESULTS_CHOICE_CHANGE_CUP
};

/* FINISH is the LAST row (index RES_CHOOSER_MAX-1 == 5) in BOTH modes: the headless
 * chooser lanes script cursor "5" to pick it, and the only per-mode substitution
 * touches row RES_CHOOSER_ROW_CHANGE (1), never this one -- so FINISH's index never
 * moves. Keep FINISH last. */
_Static_assert(RES_CHOOSER_MAX == 6u,
               "the chooser lanes script cursor 5 == FINISH, the last of 6 rows");

/* Visible countdowns: results 15s, standings 10s. updateRate accumulates in
 * 60ths of a second, so seconds*60. */
#define RES_RESULTS_UNITS 900u   /* 15s */
#define RES_STANDINGS_UNITS 600u /* 10s */

/* the FINAL-standings JOINER terminal self-advance backstop. The
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

/* Joiner chooser mirror: sustained-absence debounce before the mirror ends the
 * session on a vanished host (the room down to just this seat). Mirrors the
 * online_session RESULTS remote-vacate detector's 45-tick debounce; the mirror
 * needs its own because that detector is gated to !resultsIsFinal and the "MORE
 * RACES?" chooser fronts at the tournament FINAL standings (resultsIsFinal).
 * SEMANTIC DELTA (intentional): the session detector resets its debounce unless
 * the LOCAL seat still occupies the room, so a fully-empty snapshot is NOT a
 * vacate there; this mirror keys only on "no remote seat present", so a fully-
 * empty room also counts as vanished -- which is still the correct end for the
 * mirror (a room with no remote to wait on cannot continue). */
#define RES_CHOOSER_VACATE_DEBOUNCE 45u

/* Menu SFX (the real DKR enums; same reuse as CHARSELECT). The RANKINGS options
 * use the retail post-race vocabulary: nav PICK2 (RES_SFX_TICK), select SELECT2
 * (RES_SFX_SELECT), back BACK3 (RES_SFX_BACK) -- menu.c menu_results_loop. */
#define RES_SFX_ADVANCE SOUND_SELECT3
#define RES_SFX_TICK SOUND_MENU_PICK2
#define RES_SFX_SELECT SOUND_SELECT2
#define RES_SFX_BACK SOUND_MENU_BACK3

/* The single-race RANKINGS options reveal: the retail gOpacityDecayTimer>=60 cue
 * (options appear ~1s after the board slides in). Single-race only, so the
 * tournament FINAL standings keep their longer RES_CHOOSER_FRONT_DWELL read. */
#define RES_RANK_OPTIONS_DWELL 60u

/* RANKINGS board layout (320x240, the retail results_render coordinates lifted up
 * so our SIX online options -- vs retail's three -- fit the blue box beneath).
 * Title top, one portrait column per seat, then the 1ST..4TH session-tally rows.
 * The tally pitch is retail's own 17 (menu.c results_render: y2 += 17): the
 * FUNFONT place labels/digits are ~15px of ink, so the previous compressed 13
 * stacked them into an illegible jumble at HD (hd-rankings-after.png). The
 * room for the retail pitch comes from the portraits (up 6) and a tighter
 * option list in the blue box (results_chooser_render). */
#define RES_RANK_TITLE_Y 30
#define RES_RANK_PORTRAIT_Y 40
#define RES_RANK_TALLY_Y0 92
#define RES_RANK_TALLY_DY 17
#define RES_RANK_TALLY_ROWS 4
#define RES_RANK_ANIM_UNITS 40 /* retail slide: ~40 units, portraits drop + board in */

/* Trophy weights {9,7,5,3,1,0,0,0} == gTrophyRacePointsArray (menu.c) /
 * kTrophyPoints (lobby_core.c). Two honest uses: (1) the headless stand-in
 * reducer test seam ACCRUES with them, and (2) the STANDINGS stage derives this
 * race's "+delta" purely for DISPLAY from snap.last_placements[]. The running
 * TOTAL is always read straight from snap.points[] -- the reducer's authority; the
 * screen never accumulates into or mutates the trophy/points state (a
 * read-only weight mirror). */
static const u16 sTrophyPoints[8] = {9u, 7u, 5u, 3u, 1u, 0u, 0u, 0u};

/* Online id -> portrait / name / asset-id tables: the DRY lift into the
 * shared online_portraits.h (byte-identical across charselect/results/ceremony;
 * sOnlineToPortrait[], sOnlineNames[], sPortraitAssetIds[] now live there). */

/* ---- Session-owned screen state (never an offline global) ------------------ */
typedef struct MdkrOnlineResultsState {
    u8 stage;         /* RES_STAGE_RESULTS / RES_STAGE_STANDINGS */
    u8 isFinal;       /* the last race of the soak/cup: standings HOLD (no advance) */
    u8 raceIndex;     /* 0-based cup round, for the "RACE n/4" copy */
    u8 assets;        /* portrait group + fonts loaded */
    u8 host;          /* local seat is the room leader (advance authority) */
    u8 leave;         /* B: back-out request (edge; the LEFT return) */
    u8 haveResults;   /* THIS race's placements were read from the reducer feed */
    u8 advanced;      /* an ADVANCE was already returned for this stage (edge) */
    u8 advanceCommitted; /* host committed the rematch advance -- republish
                          * rematch_requested EVERY tick until the room leaves
                          * RESULTS, THEN return ADVANCE (convergence, not a
                          * one-shot edge a dropped reverse-feed pump could lose) */
    u8 advanceAuto;      /* how the committed advance fired: 1 == countdown auto,
                          * 0 == host press (preserved across the convergence wait
                          * so the witness still reads "(auto)"/"(host)") */
    u8 prevSecs;      /* last countdown second drawn (for the hurry-up SFX edge) */
    u8 whooshed;      /* the single-race RANKINGS slide-in SOUND_WHOOSH1 fired (edge) */
    u32 stageTicks;   /* countdown accumulator for the current stage */
    u32 pulseTicks;   /* free-running (drives the terminal-hold pulse, never reset) */
    u8 placements[RES_SLOTS]; /* THIS race's canonical-slot -> placement (poll) */
    /* "more races" chooser (host-driven; joiner mirror). */
    u8 chooserEnabled;   /* the chooser arms at the session decision point */
    u8 chooserMode;      /* forward-feed mode cached at chooser entry (option list) */
    u8 chooserFeed;      /* a forward-feed snapshot was PRESENT at chooser entry.
                          * The FINISH wrap requires an observable room to converge
                          * against; a feed-less legacy boot (chooserMode reads
                          * SINGLE there too) keeps the historical direct leave,
                          * else its convergence hold could never end. */
    u8 chooserCursor;    /* the host's highlighted option index */
    u8 chooserCount;     /* number of options in the current mode's list */
    u8 chooserPrevCursor;/* last cursor drawn (nav SFX edge) */
    u8 chooserChoice;    /* the committed MdkrOnlineResultsChoice (0 == none yet) */
    u8 chooserCommitted; /* host committed -> republish the intent to convergence */
    u8 chooserJoiner;    /* render as the joiner mirror (host-only drives) */
    u8 chooserLeaveArm;  /* joiner mirror: B armed a confirm-to-leave (2nd B commits) */
    u8 chooserVacateTicks; /* joiner mirror: debounce for a vanished-host exit */
    u8 chooserHeldNoted;   /* joiner mirror: one-shot "survived past the old dwell" */
} MdkrOnlineResultsState;

static MdkrOnlineResultsState sRes;

/* Witness change-detect (file scope so _enter can reset for a clean re-entry). */
static u32 sWitnessKey = 0xFFFFFFFFu;

/* ---- forward decls (test seam at the bottom) ------------------------------ */
static void results_test_resolve(void);
static void results_test_capture(void);
static void results_test_pump(void);
static void results_test_reduce(void);
static void results_chooser_maybe_vacate_joiner(void); /* B2 scenario 4 seam */
static u8 results_host_press_active(void);
static u8 results_joiner_finish_seam(void);          /* test seam */
static u8 results_joiner_finish_departed(u32 stageTicks);
static u8 results_joiner_terminal_seam(void);        /* no-seam proof */
static u8 results_remote_vacate_final_probe(void);   /* probe */
static u8 results_resident_remote_wins(void);        /* champion-on-disconnect */
/* chooser seam + stage (test seam resolves at the bottom). */
static s8 results_chooser_seam_select(void);         /* -1 none / 0..N option idx */
static void results_chooser_seam_mark_fired(void);   /* one-shot scripted select */
static u8 results_chooser_seam_show(void);           /* display-only (PNG capture) */
static u8 results_chooser_seam_joiner(void);         /* force the joiner mirror */
static u8 results_chooser_seam_joiner_hold(void);    /* mirror + feed HELD in RESULTS */
static u8 results_chooser_seam_joiner_vacate(void);  /* mirror + host seat vacates */
static u8 results_chooser_seam_single(void);         /* single-race stand-in room */
static const MdkrResChooserOption *results_chooser_option(unsigned i);
static MdkrOnlineResultsResult results_chooser_tick(const MdkrPartyLinkSnapshot *snap,
                                                    bool haveSnap, s32 localSeat,
                                                    s32 updateRate);

/* ======================================================================== *
 * Small helpers
 * ======================================================================== */
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

/* TRUE while any seat other than the local player is still occupied. The joiner
 * chooser mirror waits on the host through this; a sustained FALSE (every remote
 * gone) is the vanished-host signal the mirror ends the session on. */
static u8 results_chooser_remote_present(const MdkrPartyLinkSnapshot *snap) {
    unsigned i;
    for (i = 0u; i < MDKR_PARTY_LINK_SEATS; i++) {
        if (snap->seats[i].occupied && !snap->seats[i].is_local) {
            return 1u;
        }
    }
    return 0u;
}

/* ======================================================================== *
 * Render (native: real portraits + real font, into the engine frame list)
 * ======================================================================== */

/* Retail selected-option blink (menu.c: spA0 = gOptionBlinkTimer*8; if(spA0>255)
 * spA0 = 511-spA0) -- a 0..255 triangle over the 0x3F (64-tick) period, the slow
 * retail cadence. TU-LOCAL on purpose: the shared mdkr_online_screen_pulse is a
 * faster/dimmer 0..16 wave the OTHER screens depend on and this wave must not
 * change it; a later wave unifies (online_screen_util). */
static s32 results_retail_blink(u32 ticks) {
    s32 v = (s32) ((ticks & 0x3Fu) * 8u); /* 0..504 over the 64-tick period */
    if (v > 255) {
        v = 511 - v; /* fold to a 0..255 triangle */
    }
    return v;
}

/* Retail RANKINGS winner-portrait pulse (menu.c: if(blink<32) spA0=blink*4+128;
 * else spA0=0x17F-blink*4) -- brightness 128..255 over the same 64-tick period,
 * so the race winner's portrait breathes bright/dim while the rest stay full. */
static s32 results_winner_pulse(u32 ticks) {
    s32 b = (s32) (ticks & 0x3Fu);
    return (b < 32) ? (b * 4 + 128) : (0x17F - b * 4);
}

/* One translucent BLUE dialogue box (the retail RANKINGS option ground). Byte-for-
 * byte the flat-fill command sequence online_screen_util.c's navy panel emits
 * (dDialogueBoxBegin + dDialogueBoxDrawModes[1] env-colour XLU fill + rim), only
 * the colour differs -- retail's dialogue blue instead of the near-black navy. It
 * is a TU-LOCAL copy (not a call into the shared panel) so this wave changes no
 * shared helper. (x1,y1)-(x2,y2) logical 320x240 coords. */
#define RES_BOX_FILL_R 40
#define RES_BOX_FILL_G 52
#define RES_BOX_FILL_B 200
#define RES_BOX_FILL_A 184
#define RES_BOX_EDGE_R 120
#define RES_BOX_EDGE_G 150
#define RES_BOX_EDGE_B 255
#define RES_BOX_EDGE_A 208
static void results_blue_box(s32 x1, s32 y1, s32 x2, s32 y2) {
    gSPDisplayList(gCurrDisplayList++, dDialogueBoxBegin);
    gDkrDmaDisplayList(gCurrDisplayList++,
                       OS_K0_TO_PHYSICAL(dDialogueBoxDrawModes[1]), 2);
    gDPSetEnvColor(gCurrDisplayList++, RES_BOX_FILL_R, RES_BOX_FILL_G,
                   RES_BOX_FILL_B, RES_BOX_FILL_A);
    render_fill_rectangle(&gCurrDisplayList, x1 + 2, y1, x2 - 2, y1 + 2);
    render_fill_rectangle(&gCurrDisplayList, x1, y1 + 2, x2, y2 - 2);
    render_fill_rectangle(&gCurrDisplayList, x1 + 2, y2 - 2, x2 - 2, y2);
    gDPPipeSync(gCurrDisplayList++);
    gDPSetEnvColor(gCurrDisplayList++, RES_BOX_EDGE_R, RES_BOX_EDGE_G,
                   RES_BOX_EDGE_B, RES_BOX_EDGE_A);
    render_fill_rectangle(&gCurrDisplayList, x1 + 2, y1, x2 - 2, y1 + 1);
    render_fill_rectangle(&gCurrDisplayList, x1 + 2, y2 - 1, x2 - 2, y2);
    render_fill_rectangle(&gCurrDisplayList, x1, y1 + 2, x1 + 1, y2 - 2);
    render_fill_rectangle(&gCurrDisplayList, x2 - 1, y1 + 2, x2, y2 - 2);
    gDPPipeSync(gCurrDisplayList++);
    rendermode_reset(&gCurrDisplayList);
    gDPPipeSync(gCurrDisplayList++);
}

/* FUNFONT drawn with a REAL tint (envA 255) -- the shared text helper forces
 * FUNFONT to authored-untinted (envA 0), but the RANKINGS "1ST..4TH" labels carry
 * retail's per-row dimming ramp, so this TU-local draw applies the colour. One
 * black drop shadow + the tinted face. */
static void results_label_tinted(s32 x, s32 y, char *text, AlignmentFlags align,
                                 s32 r, s32 g, s32 b) {
    set_text_font(ASSET_FONTS_FUNFONT);
    set_text_background_colour(0, 0, 0, 0);
    set_text_colour(0, 0, 0, 255, 180);
    draw_text(&gCurrDisplayList, x + 1, y + 1, text, align);
    set_text_colour(r, g, b, 255, 255);
    draw_text(&gCurrDisplayList, x, y, text, align);
}

/* On-screen (retail-worded) label for a chooser option. The sChooser table keeps
 * its ORIGINAL labels (the witness + routing + every headless lane index/regex are
 * keyed on them, and the option->intent mapping must not move); this maps them to
 * the retail RANKINGS vocabulary for DISPLAY only. SELECT TRACK/CUP (change track/
 * cup), TRY AGAIN (race again), QUIT (finish); the online-only options are shown
 * verbatim. Rendering-only -- the stderr witness still prints the canonical label. */
static const char *results_chooser_display_label(const char *canonical) {
    if (strcmp(canonical, "RACE AGAIN") == 0) {
        return "TRY AGAIN";
    }
    if (strcmp(canonical, "CHANGE TRACK") == 0) {
        return "SELECT TRACK";
    }
    if (strcmp(canonical, "CHANGE CUP") == 0) {
        return "SELECT CUP";
    }
    if (strcmp(canonical, "FINISH") == 0) {
        return "QUIT";
    }
    return canonical; /* CHANGE MODE / NEW TOURNAMENT / CHANGE CHARACTER */
}

/* The retail RANKINGS board: "RANKINGS" title, one portrait column per seat (the
 * race winner's portrait pulsing bright/dim), and the 1ST..4TH session-tally rows
 * beneath -- the exact read of menu.c's results_render, from our snapshot + the
 * DISPLAY-ONLY sSessionTally. dropOffset/slideOffset are the entry-motion layout
 * offsets (portraits drop from the top, the board slides in); both 0 once settled.
 * No new assets: real portraits (already borrowed) + FUNFONT digits. */
static void results_render_rankings_board(const MdkrPartyLinkSnapshot *snap,
                                          bool haveSnap, s32 localSeat,
                                          s32 dropOffset, s32 slideOffset) {
    unsigned cols[RES_SLOTS];
    unsigned ncols = 0u;
    unsigned c;
    s32 pitch, startX;
    s32 winnerPulse = results_winner_pulse(sRes.pulseTicks);
    s32 row;
    char line[16];

    (void) localSeat;

    /* One column per occupied seat (finishing order is not needed here: retail
     * lays columns out in seat order and shows each seat's whole-session tally). */
    for (c = 0u; c < RES_SLOTS; c++) {
        bool present = haveSnap ? (snap->seats[c].occupied != 0u)
                                : (sRes.placements[c] != RES_PLACE_NONE);
        if (present) {
            cols[ncols++] = c;
        }
    }
    if (ncols == 0u) {
        return;
    }
    pitch = (ncols <= 2u) ? 64 : 56; /* retail offsetX2: 64 for <=2 seats, 56 for 4 */
    startX = 160 - (s32) (ncols - 1u) * (pitch / 2);

    /* Title (BIGFONT authored art; white == untinted, exactly retail's RANKINGS). */
    mdkr_online_screen_text(MDKR_ONLINE_SCREEN_W_HALF, RES_RANK_TITLE_Y,
                            ASSET_FONTS_BIGFONT, "RANKINGS", ALIGN_MIDDLE_CENTER,
                            255, 255, 255);

    /* Portrait row: the race winner (placement 0) pulses bright/dim; others full. */
    for (c = 0u; c < ncols; c++) {
        unsigned slot = cols[c];
        s32 cx = startX + (s32) c * pitch + slideOffset;
        u8 character = MDKR_ONLINE_SCREEN_NO_CHARACTER;
        u8 bright;
        if (haveSnap &&
            snap->seats[slot].character_id < MDKR_ONLINE_SCREEN_CHAR_COUNT) {
            character = snap->seats[slot].character_id;
        }
        bright = (sRes.placements[slot] == 0u) ? (u8) winnerPulse : 255u;
        mdkr_online_screen_draw_portrait(character, cx - 22,
                                         RES_RANK_PORTRAIT_Y + dropOffset, bright,
                                         bright, bright);
    }

    /* Tally: 1ST..4TH labels (retail gold dim ramp G=255-64*row-ish) on the left,
     * each column's whole-session count in FUNFONT (authored colourful digits). */
    for (row = 0; row < RES_RANK_TALLY_ROWS; row++) {
        s32 ry = RES_RANK_TALLY_Y0 + row * RES_RANK_TALLY_DY;
        s32 g = 224 - 48 * row;
        s32 b = 96 - 22 * row;
        if (g < 64) {
            g = 64;
        }
        if (b < 16) {
            b = 16;
        }
        results_label_tinted(startX - 34 + slideOffset, ry,
                             (char *) gRacePlacementsArray[row], ALIGN_MIDDLE_CENTER,
                             255, g, b);
        for (c = 0u; c < ncols; c++) {
            unsigned slot = cols[c];
            s32 cx = startX + (s32) c * pitch + slideOffset;
            (void) snprintf(line, sizeof(line), "%u",
                            (unsigned) sSessionTally[slot][row]);
            mdkr_online_screen_text(cx, ry, ASSET_FONTS_FUNFONT, line,
                                    ALIGN_MIDDLE_CENTER, 255, 255, 255);
        }
    }
}

/* The "this screen is over, what does a button do now" footer, shared by the
 * single-race RESULTS terminal ("RACE COMPLETE") and the tournament STANDINGS
 * terminal ("CUP COMPLETE"): a pulsed label + an explicit host affordance
 * ("A: FINISH", wired to the LEAVE return) / the joiner's own self-advance
 * countdown to the champion celebration. No dead button. */
static void results_render_complete(const MdkrPartyLinkSnapshot *snap,
                                    bool haveSnap, const char *label) {
    s32 tri = mdkr_online_screen_pulse(sRes.pulseTicks);
    u8 pg = (u8) (170 + tri * 5); /* 170..255 pulse */
    char line[48];

    mdkr_online_screen_strip(200, 234); /* footer ground */
    mdkr_online_screen_text(MDKR_ONLINE_SCREEN_W_HALF, 208, ASSET_FONTS_SMALLFONT, (char *) label,
                 ALIGN_MIDDLE_CENTER, 120, pg, 120);
    if (sRes.host) {
        mdkr_online_screen_text(MDKR_ONLINE_SCREEN_W_HALF, 224, ASSET_FONTS_SMALLFONT, "A: FINISH",
                     ALIGN_MIDDLE_CENTER, 255, 255, 255);
    } else {
        /* the JOINER self-advances off this terminal (its feed parks
         * in RESULTS, so it must not wait on the host). Show its own visible
         * countdown to the champion celebration -- never the old "WAITING FOR
         * HOST..." (misleading now, and the host may already be gone). Advertise
         * BOTH honored buttons -- A OR B advances immediately to the
         * celebration (the joiner terminal honors in.bEdge too, online_results.c
         * joinerPress), so surface the navigation input rather than leaving B silent. */
        u32 secs = mdkr_online_screen_seconds_left(sRes.stageTicks,RES_JOINER_TERMINAL_UNITS);
        s32 c = 130 + tri * 5;
        (void) snprintf(line, sizeof(line), "CONTINUE IN %us  (A/B)", secs);
        mdkr_online_screen_text(MDKR_ONLINE_SCREEN_W_HALF, 224, ASSET_FONTS_SMALLFONT, line,
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
    s32 tri = mdkr_online_screen_pulse(sRes.pulseTicks);
    char line[48];

    mdkr_online_screen_strip(200, 234); /* footer ground */
    (void) snprintf(line, sizeof(line), "%s IN %us", nextLabel, secs);
    mdkr_online_screen_text(MDKR_ONLINE_SCREEN_W_HALF, 208, ASSET_FONTS_SMALLFONT, line,
                 ALIGN_MIDDLE_CENTER, 210, 210, 210);
    if (sRes.host) {
        /* Advertise the non-final back-out. B on a
         * non-final results/standings is a mid-tournament LEAVE-to-room, so surface
         * it beside the host's advance affordance. */
        char hv[40];
        (void) snprintf(hv, sizeof(hv), "%s   B: LEAVE", hostVerb);
        mdkr_online_screen_text(MDKR_ONLINE_SCREEN_W_HALF, 224, ASSET_FONTS_SMALLFONT, hv,
                     ALIGN_MIDDLE_CENTER, 255, 255, 255);
    } else {
        char host[16];
        s32 c = 130 + tri * 5;
        results_host_name(snap, haveSnap, host, sizeof(host));
        /* Advertise the joiner's B affordance. A joiner's B on a
         * NON-final results/standings is a genuine mid-tournament LEAVE-to-room
         * (in.bEdge -> sRes.leave -> LEFT), symmetric with the host footer's
         * "B: LEAVE" (results_render_countdown) -- it was previously silent. */
        (void) snprintf(line, sizeof(line), "WAITING FOR %.12s...   B: LEAVE", host);
        mdkr_online_screen_text(MDKR_ONLINE_SCREEN_W_HALF, 224, ASSET_FONTS_SMALLFONT, line,
                     ALIGN_MIDDLE_CENTER, c, c, c);
    }
}

/* RESULTS stage: THIS race's finishing order (portrait + place label + name),
 * sorted by placement, local seat highlighted, remote labelled. */
static void results_render_results(const MdkrPartyLinkSnapshot *snap,
                                   bool haveSnap, s32 localSeat) {
    bool tournament = haveSnap && snap->mode == MDKR_ONLINE_SCREEN_MODE_TOURNAMENT;
    u32 secs = mdkr_online_screen_seconds_left(sRes.stageTicks,RES_RESULTS_UNITS);
    s32 rowY = 60;
    u8 place;
    unsigned nrows = 0u;
    char line[64];

    /* SINGLE RACE: the retail RANKINGS read. The old "RACE RESULTS" place-row
     * card and the chooser "MORE RACES?" board are now ONE screen -- this stage
     * is the rankings sliding in (portraits drop from the top, the board slides
     * in over ~40 units, SOUND_WHOOSH1), and the option list arrives when the
     * chooser fronts (results_chooser_render). Pure layout offsets, zero assets. */
    if (!tournament) {
        u32 t = sRes.stageTicks;
        s32 rem = (t < (u32) RES_RANK_ANIM_UNITS)
                      ? (s32) ((u32) RES_RANK_ANIM_UNITS - t)
                      : 0;
        s32 dropOffset = -(rem * 120 / RES_RANK_ANIM_UNITS);  /* -120..0 */
        s32 slideOffset = -(rem * 90 / RES_RANK_ANIM_UNITS);  /* -90..0 (from left) */
        if (!sRes.whooshed) {
            sound_play(SOUND_WHOOSH1, NULL); /* the retail board slide-in cue */
            sRes.whooshed = 1u;
        }
        results_render_rankings_board(snap, haveSnap, localSeat, dropOffset,
                                      slideOffset);
        /* Legacy terminal (chooser OFF -- the historical scripted/loopback lanes):
         * keep the host "A: FINISH" affordance / countdown so their behaviour is
         * byte-unchanged. With the chooser ON the options appear on the chooser
         * stage, so this stage shows just the rankings board entering. */
        if (!sRes.chooserEnabled) {
            if (sRes.isFinal) {
                results_render_complete(snap, haveSnap, "RACE COMPLETE");
            } else {
                results_render_countdown(snap, haveSnap, "NEXT RACE", "A: CONTINUE",
                                         secs);
            }
        }
        return;
    }

    /* Grounds: title strip + the finishing-order board (sized to the rows
     * actually present, so two seats get a snug card, not a hollow one). */
    {
        unsigned slot;
        for (slot = 0u; slot < RES_SLOTS; slot++) {
            if (sRes.placements[slot] != RES_PLACE_NONE) {
                nrows++;
            }
        }
    }
    mdkr_online_screen_strip(6, 46);
    if (nrows > 0u) {
        mdkr_online_screen_panel(30, 52, 290, 66 + (s32) nrows * 40);
    }
    mdkr_online_screen_text(MDKR_ONLINE_SCREEN_W_HALF, 20, ASSET_FONTS_BIGFONT, "RACE RESULTS",
                 ALIGN_MIDDLE_CENTER, 255, 224, 96);
    /* Tournament: name the round on RESULTS too (parity with STANDINGS). */
    if (tournament) {
        (void) snprintf(line, sizeof(line), "RACE %u/%u",
                        (unsigned) (snap->race_index + 1u), RES_CUP_ROUNDS);
        mdkr_online_screen_text(MDKR_ONLINE_SCREEN_W_HALF, 38, ASSET_FONTS_SMALLFONT, line,
                     ALIGN_MIDDLE_CENTER, 210, 210, 210);
    }
    rowY = 64;

    /* Walk placements in finishing order (place 0 == 1st). Each canonical slot
     * carries its own placement, so scan for the slot at each place. Range over the
     * full placement space (0..7): with AI fill / >4-player rooms a human can finish
     * 5th-8th, and bounding the scan at RES_SLOTS(4) would silently DROP that seat's
     * row. */
    for (place = 0u; place < RES_PLACE_COUNT; place++) {
        unsigned slot;
        for (slot = 0u; slot < RES_SLOTS; slot++) {
            bool isLocal;
            u8 character = MDKR_ONLINE_SCREEN_NO_CHARACTER;
            char name[32];
            char *placeLabel;
            s32 nr, ng, nb;

            if (sRes.placements[slot] != place) {
                continue;
            }
            isLocal = ((s32) slot == localSeat);
            if (haveSnap && snap->seats[slot].character_id < MDKR_ONLINE_SCREEN_CHAR_COUNT) {
                character = snap->seats[slot].character_id;
            }
            mdkr_online_screen_seat_name(snap, haveSnap, slot, name, sizeof(name));
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
             * draws these exact strings in FUNFONT for the same reason. Row
             * text sits at the portrait's optical centre (rowY+12). */
            mdkr_online_screen_text(52, rowY + 12, ASSET_FONTS_FUNFONT, placeLabel,
                         ALIGN_MIDDLE_RIGHT, nr, ng, nb);
            mdkr_online_screen_draw_portrait(character, 64, rowY - 8, (u8) nr, (u8) ng,
                                  (u8) nb);
            (void) snprintf(line, sizeof(line), "%.12s%s", name,
                            isLocal ? " [YOU]" : "");
            mdkr_online_screen_text(118, rowY + 12, ASSET_FONTS_SMALLFONT, line,
                         ALIGN_MIDDLE_LEFT, nr, ng, nb);
            rowY += 40;
            break;
        }
    }

    /* Footer: a single race's FINAL result is a terminal hold (no
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
    u32 secs = mdkr_online_screen_seconds_left(sRes.stageTicks,RES_STANDINGS_UNITS);
    u16 points[RES_SLOTS];
    u8 order[RES_SLOTS];
    u8 lastpl[RES_SLOTS];
    unsigned nseats = 0u;
    unsigned i;
    s32 rowY = 64;
    char line[64];

    /* Collect the occupied seats + their (reducer-accrued) points from the
     * snapshot, then selection-sort by points descending, tie-broken on THIS
     * race's finish (lower last_placement wins) so equal totals are not
     * host-biased by seat order. The collect+sort is the shared
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

    /* Grounds: title strip + the points-table board (sized to the seats). */
    mdkr_online_screen_strip(6, 46);
    if (nseats > 0u) {
        mdkr_online_screen_panel(24, 52, 308, 66 + (s32) nseats * 40);
    }
    mdkr_online_screen_text(MDKR_ONLINE_SCREEN_W_HALF, 20, ASSET_FONTS_BIGFONT,
                 sRes.isFinal ? "FINAL STANDINGS" : "STANDINGS",
                 ALIGN_MIDDLE_CENTER, 255, 224, 96);
    /* never show "RACE n/N" alongside the FINAL banner (the cup is over). */
    if (haveSnap && !sRes.isFinal) {
        (void) snprintf(line, sizeof(line), "RACE %u/%u",
                        (unsigned) (snap->race_index + 1u), RES_CUP_ROUNDS);
        mdkr_online_screen_text(MDKR_ONLINE_SCREEN_W_HALF, 38, ASSET_FONTS_SMALLFONT, line,
                     ALIGN_MIDDLE_CENTER, 210, 210, 210);
    }

    for (i = 0u; i < nseats; i++) {
        unsigned slot = order[i];
        bool isLocal = ((s32) slot == localSeat);
        u8 character = MDKR_ONLINE_SCREEN_NO_CHARACTER;
        char name[32];
        s32 nr, ng, nb;
        /* This race's +delta, DISPLAY-only, from last_placement's trophy weight
         * (the running total below is read straight from snap.points[]). */
        u16 delta = (lastpl[i] < 8u) ? sTrophyPoints[lastpl[i]] : 0u;

        if (haveSnap && snap->seats[slot].character_id < MDKR_ONLINE_SCREEN_CHAR_COUNT) {
            character = snap->seats[slot].character_id;
        }
        mdkr_online_screen_seat_name(snap, haveSnap, slot, name, sizeof(name));
        if (isLocal) {
            nr = 255; ng = 224; nb = 96;
        } else {
            nr = 220; ng = 220; nb = 220;
        }

        /* Row: rank | portrait | name (left) | points (FUNFONT, right) | delta.
         * The rank is FUNFONT: BIGFONT has no digit glyphs, so "%u." would render
         * a bare ".". Row text at the portrait's optical centre (rowY+12). */
        (void) snprintf(line, sizeof(line), "%u.", i + 1u);
        mdkr_online_screen_text(44, rowY + 12, ASSET_FONTS_FUNFONT, line,
                     ALIGN_MIDDLE_RIGHT, nr, ng, nb);
        mdkr_online_screen_draw_portrait(character, 52, rowY - 8, (u8) nr, (u8) ng,
                              (u8) nb);
        (void) snprintf(line, sizeof(line), "%.10s%s", name,
                        isLocal ? " [YOU]" : "");
        mdkr_online_screen_text(104, rowY + 12, ASSET_FONTS_SMALLFONT, line,
                     ALIGN_MIDDLE_LEFT, nr, ng, nb);
        /* Points in FUNFONT (the trophy-rankings vocabulary), with this race's
         * delta so a newcomer sees WHY the total moved. */
        (void) snprintf(line, sizeof(line), "%u", (unsigned) points[i]);
        mdkr_online_screen_text(252, rowY + 12, ASSET_FONTS_FUNFONT, line,
                     ALIGN_MIDDLE_RIGHT, nr, ng, nb);
        if (delta > 0u) {
            (void) snprintf(line, sizeof(line), "+%u", (unsigned) delta);
            mdkr_online_screen_text(300, rowY + 12, ASSET_FONTS_SMALLFONT, line,
                         ALIGN_MIDDLE_RIGHT, 120, 255, 120);
        }
        rowY += 40;
    }

    /* Footer: the FINAL standings is a terminal hold (pulsed CUP COMPLETE +
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
    u32 secs = mdkr_online_screen_seconds_left(sRes.stageTicks,sRes.stage == RES_STAGE_RESULTS
                                        ? RES_RESULTS_UNITS
                                        : RES_STANDINGS_UNITS);
    u8 mode = haveSnap ? snap->mode : MDKR_ONLINE_SCREEN_MODE_SINGLE;
    u16 p0 = haveSnap ? snap->points[0] : 0u;
    u16 p1 = haveSnap ? snap->points[1] : 0u;
    /* placements are 8-bit (0xFF == absent), but they only get 4 bits each
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
void mdkr_online_results_enter(u8 isFinalRace, u8 raceIndex, u8 chooserEnabled) {
    unsigned i;

    memset(&sRes, 0, sizeof(sRes));
    sRes.stage = RES_STAGE_RESULTS;
    sRes.isFinal = isFinalRace ? 1u : 0u;
    sRes.raceIndex = raceIndex;
    sRes.chooserEnabled = chooserEnabled ? 1u : 0u;
    sRes.chooserPrevCursor = 0xFFu; /* no chooser cursor drawn yet (nav SFX edge) */
    sRes.prevSecs = 0xFFu; /* no countdown second drawn yet (hurry-up SFX edge) */
    for (i = 0u; i < RES_SLOTS; i++) {
        sRes.placements[i] = RES_PLACE_NONE;
    }
    sWitnessKey = 0xFFFFFFFFu;

    /* POLL CONTENTION. This screen must NOT consume the one-shot engine results
     * poll -- that single owner is the LAUNCHER pump, which polls mid-residency
     * and PUBLISH_RESULTS to the reducer. THIS race's
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
            /* Accrue THIS race's finishing order into the session-scoped placement
             * tally (DISPLAY-ONLY -- the RANKINGS "1ST..4TH" counts). Once per
             * genuine RESULTS entry (this block runs once per race), so each TRY
             * AGAIN replay adds one, exactly like retail's session standings. */
            for (i = 0u; i < RES_SLOTS; i++) {
                u8 place = sRes.placements[i];
                if (place < RES_PLACE_COUNT) {
                    sSessionTally[i][place]++;
                }
            }
        }
    }

    /* Borrow the real portraits + fonts + per-world sky tiles (the charselect
     * asset-borrow discipline; the sky group is freed symmetrically in _exit()). */
    menu_assetgroup_load(sPortraitAssetIds);
    menu_racer_portraits();
    menu_assetgroup_load(sOnlineSkyAssetIds);
    load_font(ASSET_FONTS_BIGFONT);
    load_font(ASSET_FONTS_SMALLFONT);
    load_font(ASSET_FONTS_FUNFONT);
    /* High-definition text while this screen is up (retail letterforms at
     * native-window resolution; see online_screen_util.h). Balanced by the
     * unref in _exit(), inside the same sRes.assets guard as the fonts. */
    mdkr_online_screen_hd_text_ref();
    sRes.assets = 1u;
    /* Retail scrolling sky of the RACED world (from the forward-feed snapshot),
     * so the results screen wears the track the player just raced. */
    {
        MdkrPartyLinkSnapshot bg;
        bool haveBg = mdkr_party_link_read(&bg);
        mdkr_online_screen_backdrop(
            mdkr_online_screen_sky_world_for_snapshot(&bg, haveBg));
    }

    /* reveal from black (retail fade cadence) + restore the retail menu music
     * for the post-race screen (the race level -- and its track music -- was
     * unloaded on RESULTS entry). Isolation-safe primitive borrows -- see
     * online_screen_util.h. */
    mdkr_online_screen_fade_in_from_black();
    mdkr_online_screen_menu_music();

    fprintf(stderr,
            "[online-results] enter: native results up race=%u final=%u "
            "haveResults=%u placements=%u,%u,%u,%u (offline MENU_RESULTS "
            "bypassed)\n",
            (unsigned) raceIndex, (unsigned) sRes.isFinal,
            (unsigned) sRes.haveResults, (unsigned) sRes.placements[0],
            (unsigned) sRes.placements[1], (unsigned) sRes.placements[2],
            (unsigned) sRes.placements[3]);
    /* chooser status on its OWN line so existing lanes' enter-line regexes are
     * unaffected; the chooser lane reads this + the "chooser: fronted" line. */
    fprintf(stderr, "[online-results] enter: chooser=%u\n",
            (unsigned) sRes.chooserEnabled);
}

void mdkr_online_results_exit(void) {
    if (sRes.assets) {
        /* Retire the frame's authored display list FIRST (this frame's texrects
         * reference the tiles freed below -- the freed-texture DL corruption fix,
         * see mdkr_online_screen_dl_retire). */
        mdkr_online_screen_dl_retire();
        /* Disarm the borrowed sky before freeing its tiles (bgdraw_render lifetime). */
        mdkr_online_screen_backdrop_clear();
        mdkr_online_screen_hd_text_unref();
        unload_font(ASSET_FONTS_FUNFONT);
        unload_font(ASSET_FONTS_SMALLFONT);
        unload_font(ASSET_FONTS_BIGFONT);
        menu_assetgroup_free(sPortraitAssetIds);
        menu_assetgroup_free(sOnlineSkyAssetIds);
        sRes.assets = 0u;
        fprintf(stderr, "[online-results] exit: freed portrait assets\n");
    }
}

/* Read the local pad: HOST advance (A/START) is immediate (host authority); a
 * JOINER press is a no-op (watch-only). B is a back-out edge. */
typedef struct ResInput {
    u8 advanceEdge;
    u8 bEdge;
    u8 upEdge;   /* chooser navigation (dpad + stick) */
    u8 downEdge;
} ResInput;

static void results_input_live(ResInput *in) {
    u32 pressed = input_pressed(MDKR_ONLINE_SCREEN_LOCAL_PAD);
    memset(in, 0, sizeof(*in));
    in->advanceEdge = (pressed & (A_BUTTON | START_BUTTON)) ? 1u : 0u;
    in->bEdge = (pressed & B_BUTTON) ? 1u : 0u;
    in->upEdge = (pressed & U_JPAD) ? 1u : 0u;
    in->downEdge = (pressed & D_JPAD) ? 1u : 0u;
}

/* Scripted headless input (only under the test seam): the HOST presses ADVANCE
 * right after the entry lockout on the RESULTS stage, proving host-press
 * advance. The STANDINGS stage is deliberately left un-pressed so BOTH the
 * host-advance (RESULTS) and the countdown-to-zero auto-advance (STANDINGS) paths
 * are exercised in one soak; the final STANDINGS then holds. */
static void results_input_scripted(ResInput *in) {
    memset(in, 0, sizeof(*in));
    if (sRes.stageTicks < RES_INPUT_GRACE) {
        return;
    }
    if (sRes.stage == RES_STAGE_RESULTS) {
        in->advanceEdge = 1u; /* RESULTS -> STANDINGS (host press) */
    } else if (results_host_press_active()) {
        /* Remote-vacate probe: at the FINAL standings, "the host has vacated" ->
         * script NO terminal press, so the joiner leaves via its self-advance DWELL
         * (making the dwell-vs-vacate race unambiguous). Non-final standings still
         * press (the cup advances fast). */
        if (sRes.isFinal && results_remote_vacate_final_probe()) {
            return;
        }
        /* live-resident lane: also press the STANDINGS stage so the host
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

/* The HOST publishes its post-race REMATCH intent on the reverse
 * feed to "start the next race". The launcher pump polls it and drives the
 * reducer's leader-only MDKR_ONLINE_REMATCH (return to LOBBY + tournament
 * race_index++).
 *
 * CONVERGENCE MODEL: once the host commits the
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

/* ======================================================================== *
 * native "more races" chooser (host-driven; joiner mirror)
 *
 * At the session decision point (a single race's RESULTS, or a tournament's FINAL
 * standings) the terminal hands to this menu instead of the old binary
 * continue/leave. The HOST navigates the full retail replay set; the joiner is
 * display-only (it renders the mirror + "waiting for host" and follows the host's
 * authoritative choice once the room leaves RESULTS). Every option maps to the
 * EXISTING party_link reverse-feed intents (REMATCH + SET_MODE); the launcher
 * reducer already handles them (no new command). See online_results.h for the
 * option -> intent -> routing table.
 * ======================================================================== */

/* One "more races" option for the current chooser mode + row. Both modes share
 * every row except RES_CHOOSER_ROW_CHANGE (single CHANGE TRACK / tournament CHANGE
 * CUP), so this substitutes that one row for a tournament and otherwise returns the
 * shared row. Callers bound `i` to < RES_CHOOSER_MAX (chooserCount). */
static const MdkrResChooserOption *results_chooser_option(unsigned i) {
    if (i == RES_CHOOSER_ROW_CHANGE &&
        sRes.chooserMode == (u8) MDKR_ONLINE_SCREEN_MODE_TOURNAMENT) {
        return &sChooserTournamentChange;
    }
    return &sChooser[i];
}

/* ------------------------------------------------------------------------- *
 * The ONE chooser-choice routing table: each committed "more races" choice ->
 * (a) the SET_MODE the host publishes with its REMATCH and (b) the native screen
 * the session re-fronts. results_publish_chooser_intent + the commit-log line read
 * the mode column (via results_chooser_intent_mode); the session routing switch
 * reads the refront column (via mdkr_online_results_choice_refront). Single source
 * of truth -- the MdkrOnlineResultsChoice prose in online_results.h mirrors it.
 * ------------------------------------------------------------------------- */
typedef enum {
    RES_CHOOSER_MODE_KEEP = 0,       /* REMATCH only, no SET_MODE (mode UNSET) */
    RES_CHOOSER_MODE_TOGGLE,         /* SET_MODE single <-> tournament */
    RES_CHOOSER_MODE_SET_TOURNAMENT  /* SET_MODE tournament */
} ResChooserModeAction;

typedef struct {
    u8 modeAction; /* ResChooserModeAction: the SET_MODE the REMATCH carries */
    u8 refront;    /* MdkrOnlineResultsRefront: the session re-front target */
} ResChooserRoute;

/* Indexed by MdkrOnlineResultsChoice. */
static const ResChooserRoute sChooserRoutes[] = {
    [MDKR_ONLINE_RESULTS_CHOICE_NONE] =
        {RES_CHOOSER_MODE_KEEP, MDKR_ONLINE_RESULTS_REFRONT_SAME},
    [MDKR_ONLINE_RESULTS_CHOICE_RACE_AGAIN] =
        {RES_CHOOSER_MODE_KEEP, MDKR_ONLINE_RESULTS_REFRONT_SAME},
    [MDKR_ONLINE_RESULTS_CHOICE_CHANGE_TRACK] =
        {RES_CHOOSER_MODE_KEEP, MDKR_ONLINE_RESULTS_REFRONT_TRACKSELECT},
    [MDKR_ONLINE_RESULTS_CHOICE_CHANGE_CUP] =
        {RES_CHOOSER_MODE_KEEP, MDKR_ONLINE_RESULTS_REFRONT_TRACKSELECT},
    [MDKR_ONLINE_RESULTS_CHOICE_CHANGE_MODE] =
        {RES_CHOOSER_MODE_TOGGLE, MDKR_ONLINE_RESULTS_REFRONT_TRACKSELECT},
    [MDKR_ONLINE_RESULTS_CHOICE_NEW_TOURNAMENT] =
        {RES_CHOOSER_MODE_SET_TOURNAMENT, MDKR_ONLINE_RESULTS_REFRONT_TRACKSELECT},
    [MDKR_ONLINE_RESULTS_CHOICE_CHANGE_CHAR] =
        {RES_CHOOSER_MODE_KEEP, MDKR_ONLINE_RESULTS_REFRONT_CHARSELECT},
    [MDKR_ONLINE_RESULTS_CHOICE_FINISH] =
        {RES_CHOOSER_MODE_KEEP, MDKR_ONLINE_RESULTS_REFRONT_LEAVE},
    [MDKR_ONLINE_RESULTS_CHOICE_JOINER_FOLLOW] =
        {RES_CHOOSER_MODE_KEEP, MDKR_ONLINE_RESULTS_REFRONT_CHARSELECT},
};

/* The SET_MODE value a committed choice publishes with its REMATCH
 * (MDKR_PARTY_LINK_MODE_UNSET == no SET_MODE). TOGGLE reads the current chooser
 * mode. Consumed by both the intent publish and the commit-log line. */
static u8 results_chooser_intent_mode(u8 choice) {
    u8 action =
        (choice < (u8) (sizeof(sChooserRoutes) / sizeof(sChooserRoutes[0])))
            ? sChooserRoutes[choice].modeAction
            : (u8) RES_CHOOSER_MODE_KEEP;
    switch (action) {
    case RES_CHOOSER_MODE_TOGGLE:
        return (sRes.chooserMode == (u8) MDKR_ONLINE_SCREEN_MODE_TOURNAMENT)
                   ? (u8) MDKR_ONLINE_SCREEN_MODE_SINGLE
                   : (u8) MDKR_ONLINE_SCREEN_MODE_TOURNAMENT;
    case RES_CHOOSER_MODE_SET_TOURNAMENT:
        return (u8) MDKR_ONLINE_SCREEN_MODE_TOURNAMENT;
    default:
        return (u8) MDKR_PARTY_LINK_MODE_UNSET;
    }
}

MdkrOnlineResultsRefront mdkr_online_results_choice_refront(
    MdkrOnlineResultsChoice choice) {
    if ((unsigned) choice >=
        sizeof(sChooserRoutes) / sizeof(sChooserRoutes[0])) {
        return MDKR_ONLINE_RESULTS_REFRONT_SAME;
    }
    return (MdkrOnlineResultsRefront) sChooserRoutes[choice].refront;
}

/* The host's committed intent for a more-races option: REMATCH always (return the
 * room to LOBBY), plus the table's SET_MODE for the mode-changing options.
 * Republished every tick until the room leaves RESULTS (the convergence model
 * results_publish_rematch uses). A joiner never calls this (watch-only). */
static void results_publish_chooser_intent(u8 choice) {
    MdkrPartyLinkLocalIntent intent;
    mdkr_party_link_intent_init(&intent);
    intent.rematch_requested = 1u;
    intent.mode = results_chooser_intent_mode(choice);
    mdkr_party_link_intent_publish(&intent);
}

/* Bounded chooser witness: one line only when the cursor / commit / role changes,
 * so the headless lane can read the drawn option + role without flooding. */
static void results_chooser_witness(void) {
    static u32 sChooserKey = 0xFFFFFFFFu;
    u32 key = ((u32) sRes.chooserCursor) | ((u32) sRes.chooserMode << 4) |
              ((u32) sRes.chooserJoiner << 5) | ((u32) sRes.host << 6) |
              ((u32) sRes.chooserCommitted << 7) |
              ((u32) sRes.chooserChoice << 8);
    if (key == sChooserKey) {
        return;
    }
    sChooserKey = key;
    fprintf(stderr,
            "[online-results] chooser render mode=%u host=%u joiner=%u cursor=%u "
            "option=%s committed=%u choice=%u\n",
            (unsigned) sRes.chooserMode, (unsigned) sRes.host,
            (unsigned) sRes.chooserJoiner, (unsigned) sRes.chooserCursor,
            results_chooser_option(sRes.chooserCursor)->label,
            (unsigned) sRes.chooserCommitted, (unsigned) sRes.chooserChoice);
}

/* Render the "more races" chooser as the retail RANKINGS options moment. Single
 * race: the same board this race entered on (title + portrait columns + session
 * tallies) with the option list in the translucent blue dialogue box beneath.
 * Tournament final: the MORE RACES? banner + the same blue-box option list. The
 * host's selected option blinks at the retail cadence (results_retail_blink); a
 * joiner shows the list dimmer (no cursor) + a "WAITING FOR <host>..." footer --
 * the display-only mirror. Option labels are the retail wording
 * (results_chooser_display_label); the stderr witness keeps the canonical labels. */
static void results_chooser_render(const MdkrPartyLinkSnapshot *snap,
                                   bool haveSnap, s32 localSeat) {
    s32 blink = results_retail_blink(sRes.pulseTicks);
    bool single = (sRes.chooserMode == (u8) MDKR_ONLINE_SCREEN_MODE_SINGLE);
    s32 boxTop, boxBot, rowDy, rowY, fy;
    unsigned i;
    char line[48];

    if (single) {
        /* The retail RANKINGS "options" moment: the SAME board this race entered
         * on (title + portrait columns + session tallies), now settled, with the
         * option list in the translucent blue dialogue box beneath it -- one
         * unified RANKINGS read (retail-rankings-options.png). */
        results_render_rankings_board(snap, haveSnap, localSeat, 0, 0);
        /* below the retail-pitch tally (last row centre 92 + 3*17 = 143,
         * FUNFONT ink to ~151); the tighter 11px option pitch keeps the
         * six-row box bottom at 226 with the footer at its usual 236. */
        boxTop = 152;
        rowDy = 11;
    } else {
        /* Tournament FINAL: keep the MORE RACES? banner (the cup STANDINGS precede
         * this, so no per-race rankings board), options in the same blue box. */
        (void) localSeat;
        mdkr_online_screen_strip(28, 60);
        mdkr_online_screen_text(MDKR_ONLINE_SCREEN_W_HALF, 40, ASSET_FONTS_BIGFONT,
                                "MORE RACES?", ALIGN_MIDDLE_CENTER, 255, 224, 96);
        mdkr_online_screen_text(MDKR_ONLINE_SCREEN_W_HALF, 56, ASSET_FONTS_SMALLFONT,
                                "TOURNAMENT COMPLETE", ALIGN_MIDDLE_CENTER, 210, 210,
                                210);
        boxTop = 72;
        rowDy = 16;
    }

    /* Option list in the translucent blue dialogue box. */
    boxBot = boxTop + (s32) sRes.chooserCount * rowDy + 8;
    results_blue_box(92, boxTop, 228, boxBot);
    rowY = boxTop + 10;
    for (i = 0u; i < sRes.chooserCount; i++) {
        bool selected = (!sRes.chooserJoiner && (u8) i == sRes.chooserCursor);
        /* Retail wording on screen; the witness/routing keep the canonical label. */
        const char *label =
            results_chooser_display_label(results_chooser_option(i)->label);
        if (selected) {
            /* retail selected blink: the option pulses bright<->gold on the 0..255
             * cadence (menu.c gOptionBlinkTimer*8). */
            s32 c = 150 + blink * 105 / 255; /* 150..255 */
            mdkr_online_screen_text(MDKR_ONLINE_SCREEN_W_HALF, rowY,
                                    ASSET_FONTS_SMALLFONT, (char *) label,
                                    ALIGN_MIDDLE_CENTER, 255, (u8) c,
                                    (u8) (60 + c / 3));
        } else {
            s32 g = sRes.chooserJoiner ? 180 : 224;
            mdkr_online_screen_text(MDKR_ONLINE_SCREEN_W_HALF, rowY,
                                    ASSET_FONTS_SMALLFONT, (char *) label,
                                    ALIGN_MIDDLE_CENTER, g, g, g);
        }
        rowY += rowDy;
    }

    /* Footer: committed hold / joiner waiting / host help. Below the box, clamped
     * to the last on-screen line. */
    fy = (boxBot + 12 <= 236) ? boxBot + 12 : 236;
    if (sRes.chooserCommitted) {
        (void) snprintf(line, sizeof(line), "%s...",
                        results_chooser_display_label(
                            results_chooser_option(sRes.chooserCursor)->label));
        mdkr_online_screen_text(MDKR_ONLINE_SCREEN_W_HALF, fy, ASSET_FONTS_SMALLFONT,
                                line, ALIGN_MIDDLE_CENTER, 140,
                                (u8) (150 + blink * 105 / 255), 140);
    } else if (sRes.chooserJoiner) {
        char host[16];
        if (sRes.chooserLeaveArm) {
            (void) snprintf(line, sizeof(line), "PRESS B AGAIN TO LEAVE");
        } else {
            results_host_name(snap, haveSnap, host, sizeof(host));
            (void) snprintf(line, sizeof(line), "WAITING FOR %.12s...   B: LEAVE",
                            host);
        }
        mdkr_online_screen_text(MDKR_ONLINE_SCREEN_W_HALF, fy, ASSET_FONTS_SMALLFONT,
                                line, ALIGN_MIDDLE_CENTER, 200, 200, 210);
    } else {
        mdkr_online_screen_text(MDKR_ONLINE_SCREEN_W_HALF, fy, ASSET_FONTS_SMALLFONT,
                                "A: SELECT   UP/DOWN: MOVE", ALIGN_MIDDLE_CENTER, 255,
                                255, 255);
    }
    results_chooser_witness();
}

/* One CHOOSER-stage tick: host navigation + commit (-> intent republished to
 * convergence -> ADVANCE), or the joiner mirror + follow. Returns STAY / ADVANCE
 * (the session reads mdkr_online_results_choice to route) / LEAVE (FINISH, or the
 * joiner terminal). */
static MdkrOnlineResultsResult results_chooser_tick(const MdkrPartyLinkSnapshot *snap,
                                                    bool haveSnap, s32 localSeat,
                                                    s32 updateRate) {
    ResInput in;

    if (updateRate <= 0) {
        updateRate = 1;
    }
    if (sRes.chooserCount == 0u) {
        sRes.chooserCount = RES_CHOOSER_MAX;
    }

    /* B2 scenario 4: if armed, the joiner drops while the host deliberates here
     * (the host must not be interrupted -- no host-side vacate detector). */
    results_chooser_maybe_vacate_joiner();

    results_gather_input(&in); /* zeroed on the CHOOSER stage under the seams */

    /* HOST navigation (live pad) + the dedicated-lane scripted select. */
    if (sRes.host && !sRes.chooserJoiner && !sRes.chooserCommitted) {
        s8 sel = results_chooser_seam_select();
        if (sel >= 0) {
            if ((u8) sel < sRes.chooserCount) {
                sRes.chooserCursor = (u8) sel;
            }
            if (sRes.stageTicks >= RES_INPUT_GRACE) {
                in.advanceEdge = 1u; /* scripted A press */
            }
        } else if (!results_chooser_seam_show()) {
            if (in.upEdge && sRes.chooserCursor > 0u) {
                sRes.chooserCursor--;
            }
            if (in.downEdge && sRes.chooserCursor + 1u < sRes.chooserCount) {
                sRes.chooserCursor++;
            }
        }
        if (sRes.chooserCursor != sRes.chooserPrevCursor &&
            sRes.chooserPrevCursor != 0xFFu) {
            sound_play(RES_SFX_TICK, NULL);
        }
    }

    results_chooser_render(snap, haveSnap, localSeat);
    sRes.chooserPrevCursor = sRes.chooserCursor;
    sRes.stageTicks += (u32) updateRate;
    sRes.pulseTicks += (u32) updateRate;

    /* HOST commit. */
    if (sRes.host && !sRes.chooserJoiner && !sRes.chooserCommitted &&
        !results_chooser_seam_show() && in.advanceEdge &&
        sRes.stageTicks >= RES_INPUT_GRACE) {
        u8 choice = results_chooser_option(sRes.chooserCursor)->choice;
        sRes.chooserChoice = choice;
        if (results_chooser_seam_select() >= 0) {
            results_chooser_seam_mark_fired(); /* one-shot: no soak re-race loop */
        }
        sound_play(RES_SFX_SELECT, NULL); /* retail RANKINGS option select = SELECT2 */
        if (choice == (u8) MDKR_ONLINE_RESULTS_CHOICE_FINISH) {
            /* A FINISH at a SESSION DECISION POINT must be REDUCER-OBSERVABLE. A
             * second REAL peer's chooser mirror can only observe reducer STATE,
             * and its only exits are room-left-RESULTS and vanished-host; a
             * purely local FINISH leave (the pre-fix behavior) parked the room in
             * RESULTS with the host still seated (the FINISHED re-arm keeps the
             * host in the room for the re-take), so a real joiner was stranded
             * and the room could never return to SELECTING for a re-take. The
             * fix is the EXISTING leader-only REMATCH wrap (no new reducer
             * command), committed first and republished to convergence exactly
             * like every other chooser option; LEAVE only once the room has left
             * RESULTS. Two decision points wrap:
             *   - the GENUINE tournament final (the ENTRY-LATCHED last cup
             *     round): lobby_core.c REMATCH there runs RESULTS -> LOBBY +
             *     reset_tournament_series -- a fresh series in the same room;
             *   - EVERY single-race chooser (each single race is its own
             *     decision point): single-race REMATCH is PHASE-ONLY --
             *     RESULTS -> LOBBY + placements/votes cleared, race_index,
             *     points, mode and configured_track untouched (lobby_core.c) --
             *     so there is no series to wrongly advance.
             * The ceremony then renders from the ranking the session LATCHED
             * while the phase was still RESULTS (online_session.c), never from
             * the wrapped table. The direct LEAVE remains for exactly two
             * shapes: an env-shortened resident soak whose feed is MID-CUP
             * (sRes.raceIndex is the session's own 0-based round, so an
             * env-final at reducer round 0/1 stays excluded -- a mid-cup REMATCH
             * would advance a series the room is still playing) and a FEED-LESS
             * legacy boot (chooserFeed 0 -- no room to observe, so a wrap hold
             * could never converge).
             *
             * The gate reads ONLY entry-latched state (isFinal / chooserMode /
             * raceIndex / chooserFeed, all fixed at enter/front): a live
             * snapshot read here could transiently fail on the commit tick, and
             * a haveSnap-gated wrap would then silently take the purely-local
             * leave at a genuine decision point -- re-opening the exact strand
             * this exists to close. */
            u8 finishWrap = ((sRes.isFinal &&
                              sRes.chooserMode == (u8) MDKR_ONLINE_SCREEN_MODE_TOURNAMENT &&
                              (u32) sRes.raceIndex + 1u >= RES_CUP_ROUNDS) ||
                             (sRes.chooserMode == (u8) MDKR_ONLINE_SCREEN_MODE_SINGLE &&
                              sRes.chooserFeed))
                                ? 1u
                                : 0u;
            if (!finishWrap) {
                fprintf(stderr,
                        "[online-results] chooser: committed option=FINISH -> "
                        "LEAVE\n");
                return MDKR_ONLINE_RESULTS_LEAVE;
            }
            /* fall through: latch committed below and converge the wrap. */
        }
        sRes.chooserCommitted = 1u;
        {
            u8 mode = results_chooser_intent_mode(choice);
            fprintf(stderr,
                    "[online-results] chooser: committed option=%s choice=%u "
                    "intent{rematch=1 mode=%u}\n",
                    results_chooser_option(sRes.chooserCursor)->label,
                    (unsigned) choice, (unsigned) mode);
        }
    }

    /* HOST committed: republish the intent to convergence; ADVANCE once the room
     * has left RESULTS (the reducer accepted the REMATCH). */
    if (sRes.chooserCommitted) {
        results_publish_chooser_intent(sRes.chooserChoice);
        results_test_reduce(); /* stand-in reducer (inert in a live run) */
        if (haveSnap && snap->phase != (uint8_t) RES_PHASE_RESULTS) {
            if (sRes.chooserChoice == (u8) MDKR_ONLINE_RESULTS_CHOICE_FINISH) {
                /* The final-FINISH wrap landed: the room is back in LOBBY with a
                 * fresh series (reducer-observable by the second real peer), so
                 * NOW the host may leave -- LEAVE routes to the champion
                 * CEREMONY (from the session's latched ranking) -> FINISHED. */
                fprintf(stderr,
                        "[online-results] chooser: FINISH wrap converged (room "
                        "left RESULTS) -> LEAVE (ceremony)\n");
                return MDKR_ONLINE_RESULTS_LEAVE;
            }
            fprintf(stderr,
                    "[online-results] chooser: room left RESULTS -> ADVANCE "
                    "(choice=%u)\n",
                    (unsigned) sRes.chooserChoice);
            return MDKR_ONLINE_RESULTS_ADVANCE;
        }
        return MDKR_ONLINE_RESULTS_STAY; /* "<option>..." hold until converged */
    }

    /* JOINER mirror: display-only. Follow the host's authoritative choice once the
     * room leaves RESULTS (re-select). While the host is still deliberating the
     * mirror WAITS -- it must not bail on a blind countdown (the host is entitled
     * to take longer than 10s deciding "more races"), and it must not quit on a
     * stray A/START. It ends the session on only two events: a vanished host (the
     * room down to just this seat, debounced) and a deliberate, confirmed B. */
    if (sRes.chooserJoiner) {
        if (haveSnap && snap->phase != (uint8_t) RES_PHASE_RESULTS) {
            if (sRes.isFinal && sRes.chooserMode == (u8) MDKR_ONLINE_SCREEN_MODE_TOURNAMENT) {
                /* The TOURNAMENT-FINAL mirror: the cup is COMPLETE, and the
                 * room's departure from RESULTS is the host's authoritative wrap
                 * (its FINISH, or a replay option -- the mirror cannot and need
                 * not distinguish: the joiner has EARNED its celebration either
                 * way). Exit to the joiner's OWN champion CEREMONY -> FINISHED,
                 * rendered from the ranking the session LATCHED while the phase
                 * was still RESULTS (the wrap has already reset the live
                 * points). Following into re-selection here -- the pre-fix
                 * behavior -- left the joiner waiting in CHARSELECT on a host
                 * that had FINISHed out of its session; after FINISHED the
                 * joiner's own re-arm re-takes natively into the freshly
                 * wrapped room, which is where a continuing host is heading
                 * too. NOTE the finality gate is the ENTRY-latched isFinal, not
                 * the live race_index: by observation time the wrap has already
                 * reset race_index to 0. */
                fprintf(stderr,
                        "[online-results] chooser: joiner mirror observed the "
                        "final wrap -> LEAVE (ceremony)\n");
                return MDKR_ONLINE_RESULTS_LEAVE;
            }
            /* The SINGLE-RACE mirror KEEPS THE FOLLOW on the observed wrap --
             * deliberately NOT the tournament-final ceremony exit. A single-race
             * wrap is reducer-INDISTINGUISHABLE between the host's FINISH and
             * RACE AGAIN (both are the same phase-only REMATCH), and ceremonying
             * on every wrap would tear the joiner's session down on every
             * replay. The follow lands the joiner in CHARSELECT of the freshly
             * wrapped room -- exactly where a FINISHing host's automatic
             * FINISHED re-take arrives, so the two peers re-converge whichever
             * option the host picked. */
            sRes.chooserChoice = (u8) MDKR_ONLINE_RESULTS_CHOICE_JOINER_FOLLOW;
            fprintf(stderr,
                    "[online-results] chooser: joiner follows host authoritative "
                    "choice -> ADVANCE (re-select)\n");
            return MDKR_ONLINE_RESULTS_ADVANCE;
        }
        /* Vanished-host exit. The online_session RESULTS remote-vacate detector is
         * gated to !resultsIsFinal, but this "MORE RACES?" mirror fronts at the
         * tournament FINAL standings (resultsIsFinal), so the mirror carries its
         * own vacated-host exit: a sustained absence of every remote seat means the
         * room dissolved and the session must end. Debounced against a transient. */
        if (haveSnap && !results_chooser_remote_present(snap)) {
            if (sRes.chooserVacateTicks < 0xFFu) {
                sRes.chooserVacateTicks++;
            }
            if (sRes.chooserVacateTicks >= RES_CHOOSER_VACATE_DEBOUNCE) {
                fprintf(stderr, "[online-results] chooser: joiner mirror host "
                                "vacated -> LEAVE\n");
                return MDKR_ONLINE_RESULTS_LEAVE;
            }
        } else {
            sRes.chooserVacateTicks = 0u;
        }
        /* Observability: the mirror survived past the old blind 10s dwell with the
         * host present -- exactly the case that used to end the joiner's session. */
        if (!sRes.chooserHeldNoted &&
            sRes.stageTicks >= RES_JOINER_TERMINAL_UNITS) {
            sRes.chooserHeldNoted = 1u;
            fprintf(stderr, "[online-results] chooser: joiner mirror still up past "
                            "dwell (units=%u)\n",
                    (unsigned) sRes.stageTicks);
        }
        /* B is an intentional leave with a confirm step (the charselect-backout
         * idiom): the first B arms it and the footer prompts, a second B commits.
         * A/START (advanceEdge) cancels an armed leave; every other button is inert
         * on the mirror -- only the host drives the option list. */
        if (sRes.stageTicks >= RES_INPUT_GRACE) {
            if (in.bEdge) {
                if (sRes.chooserLeaveArm) {
                    fprintf(stderr, "[online-results] chooser: joiner confirmed "
                                    "B -> LEAVE\n");
                    return MDKR_ONLINE_RESULTS_LEAVE;
                }
                sRes.chooserLeaveArm = 1u;
                sound_play(RES_SFX_BACK, NULL); /* retail RANKINGS back = BACK3 */
            } else if (in.advanceEdge && sRes.chooserLeaveArm) {
                sRes.chooserLeaveArm = 0u; /* A cancels an armed leave */
            }
        }
    }
    return MDKR_ONLINE_RESULTS_STAY;
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
    localSeat = haveSnap ? mdkr_online_screen_local_seat(&snap) : -1;
    /* A feed-less endpoint (no snapshot at all -- legacy direct boot) owns the
     * progression; but when a feed IS present, be a JOINER unless the resolved
     * local seat is the host. A "no local seat -> assume host" fallback would
     * wrongly grant advance authority to a feed-having endpoint that could not
     * resolve its seat, splitting a live room -- so a feed-having endpoint that
     * cannot resolve its seat is a joiner. The resident soak's local seat IS
     * host, so it still drives via the host path below. */
    if (!haveSnap) {
        sRes.host = 1u;
    } else {
        sRes.host = (localSeat >= 0 && snap.seats[localSeat].is_host) ? 1u : 0u;
    }
    tournament = haveSnap && snap.mode == MDKR_ONLINE_SCREEN_MODE_TOURNAMENT;

    /* once the native "more races" chooser has been fronted it OWNS the tick
     * (its own render + host list / joiner mirror + intent publish + routing). */
    if (sRes.stage == RES_STAGE_CHOOSER) {
        return results_chooser_tick(&snap, haveSnap, localSeat, updateRate);
    }

    limit = (sRes.stage == RES_STAGE_RESULTS) ? RES_RESULTS_UNITS
                                              : RES_STANDINGS_UNITS;
    /* The terminal screen holds (no ADVANCE): the final race's last shown stage
     * -- tournament STANDINGS, or a single race's RESULTS. */
    terminal = sRes.isFinal && (sRes.stage == RES_STAGE_STANDINGS ||
                                (sRes.stage == RES_STAGE_RESULTS && !tournament));

    results_gather_input(&in);
    /* latch a B-back LEAVE only on a NON-terminal screen, where the consumer
     * at the bottom (`if (sRes.leave)`) is actually reached. On the terminal FINAL
     * standings the host branch returns STAY before that consumer and the joiner
     * branch honors in.bEdge directly (joinerPress), so an unconditional write
     * there was dead -- and it wrongly implied B "does something" for a host that
     * reflexively presses B to go back. Guarding on !terminal removes the dead
     * write while leaving every reachable B-back (non-final host/joiner) unchanged.
     * (Host A:FINISH is advanceEdge, never bEdge, so it is untouched.) */
    if (in.bEdge && !terminal) {
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
    secs = mdkr_online_screen_seconds_left(sRes.stageTicks,limit);
    if (!terminal && secs != (u32) sRes.prevSecs && secs >= 1u && secs <= 3u) {
        sound_play(RES_SFX_TICK, NULL);
    }
    sRes.prevSecs = (u8) (secs > 255u ? 255u : secs);

    /* at the SESSION DECISION POINT hand off to the native "more races" chooser
     * instead of the old terminal hold / single-race auto-advance. Armed only for
     * real play + the dedicated chooser lane (sRes.chooserEnabled); every pre-existing
     * scripted/loopback lane has it OFF and reaches the historical terminal + stage
     * logic below byte-for-byte unchanged. A tournament reaches the chooser ONLY at
     * its FINAL standings (non-final rounds still auto-REMATCH to the next round
     * below); a single race after a short dwell so the finishing order is readable
     * first (host A skips the dwell). */
    if (sRes.chooserEnabled) {
        bool tournFinal = tournament && sRes.isFinal &&
                          sRes.stage == RES_STAGE_STANDINGS &&
                          (sRes.stageTicks >= RES_CHOOSER_FRONT_DWELL ||
                           (sRes.host && in.advanceEdge &&
                            sRes.stageTicks >= RES_INPUT_GRACE));
        /* Single-race: the RANKINGS options appear ~1s after the board slides in
         * (RES_RANK_OPTIONS_DWELL, the retail gOpacityDecayTimer>=60 cue), so the
         * unified read matches retail's reveal; the host A still skips the wait. */
        bool singleDone = !tournament && sRes.stage == RES_STAGE_RESULTS &&
                          (sRes.stageTicks >= RES_RANK_OPTIONS_DWELL ||
                           (sRes.host && in.advanceEdge &&
                            sRes.stageTicks >= RES_INPUT_GRACE));
        if (tournFinal || singleDone) {
            sRes.stage = RES_STAGE_CHOOSER;
            sRes.stageTicks = 0u;
            sRes.chooserMode =
                (u8) (tournament ? MDKR_ONLINE_SCREEN_MODE_TOURNAMENT : MDKR_ONLINE_SCREEN_MODE_SINGLE);
            sRes.chooserFeed = haveSnap ? 1u : 0u;
            sRes.chooserCursor = 0u;
            sRes.chooserPrevCursor = 0xFFu;
            sRes.chooserCount = RES_CHOOSER_MAX;
            /* A joiner (or the forced-joiner seam) renders the display-only mirror
             * and follows the host; only the host drives the option list. */
            sRes.chooserJoiner =
                (u8) ((!sRes.host || results_chooser_seam_joiner()) ? 1u : 0u);
            sound_play(RES_SFX_ADVANCE, NULL);
            fprintf(stderr,
                    "[online-results] chooser: fronted (%s mode=%u host=%u "
                    "joiner=%u)\n",
                    tournament ? "tournament-final" : "single-race",
                    (unsigned) sRes.chooserMode, (unsigned) sRes.host,
                    (unsigned) sRes.chooserJoiner);
            /* Visual proof of the native chooser is a PLATFORM facility: the
             * launcher arms the engine frame-dump from the shot env when it sets
             * up the resident soak (main_app.cpp). Game code no longer touches the
             * platform g_dumpFramesDir global. */
            return results_chooser_tick(&snap, haveSnap, localSeat, updateRate);
        }
    }

    /* Advance authority:
     *  - HOST: a manual A/START edge (after the entry lockout) OR the countdown
     *    reaching zero -- host is the room authority.
     *  - JOINER: never returns the room-affecting ADVANCE on its own countdown;
     *    it FOLLOWS the authoritative snapshot phase leaving RESULTS. */
    manualEdge = (in.advanceEdge && sRes.stageTicks >= RES_INPUT_GRACE) ? 1u : 0u;
    autoFire = (sRes.stageTicks >= limit) ? 1u : 0u;

    if (terminal) {
        /* HOST: the interactive hold. "A: FINISH" -> LEAVE, which the session maps
         * to FINISHED via resultsIsFinal (the handshake -> CEREMONY -> FINISHED).
         * The host is NEVER auto-bounded here (its hold is legitimate + interactive
         * -- do NOT give the host a countdown). The test seam suppresses this so the
         * JOINER path below can be exercised on a rig where the visible endpoint
         * drove the rounds as host. */
        if (sRes.host && manualEdge && !results_joiner_finish_seam() &&
            !results_joiner_terminal_seam()) {
            fprintf(stderr, "[online-results] finish: host A -> LEAVE\n");
            return MDKR_ONLINE_RESULTS_LEAVE;
        }
        /* JOINER: a bound INDEPENDENT of the host. The joiner's feed
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
        /* CONVERGENCE-DRIVEN host advance off a NON-final tournament
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
/* the stand-in reducer's own cup round counter -- advanced ONLY when it
 * observes the REMATCH reverse-feed intent (results_test_reduce), NEVER off the
 * session's boot count, so the soak proves the results screen drives the next
 * race via rematch (not the old start signal). It is what the scripted forward
 * feed publishes as race_index. */
static u8 sTestRaceIndex;

static void results_test_resolve(void) {
    if (sTestActive < 0) {
        /* uniform truthiness with online_session.c / main_app.cpp -- the flag
         * is the RACE COUNT, so "=0" means OFF (a half-armed seam proves nothing). */
        const char *e = getenv("MDKR_TEST_ONLINE_RESIDENT");
        sTestActive = (e != NULL && strtoul(e, NULL, 10) > 0ul) ? 1 : 0;
    }
}

/* stand-in LAUNCHER (headless soak only): the single owner of the
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
    /* test seam (env MDKR_TEST_ONLINE_RESIDENT_REMOTE_WINS): make the REMOTE
     * seat (slot 1) win the cup so the LOCAL seat (slot 0) is the LOSER -- the
     * configuration the ceremony-champion-on-disconnect scenario needs (the true
     * winner is the seat that then departs). Force slot 1 first, slot 0 second, so
     * the existing trophy accrual below gives slot 1 the higher total. Off in every
     * normal run; the other resident scenarios (local wins) are unchanged. */
    if (results_resident_remote_wins()) {
        sTestLastPlacements[0] = 1u; /* local: 2nd */
        sTestLastPlacements[1] = 0u; /* remote: 1st */
    }
    memset(&sTestRoom, 0, sizeof(sTestRoom));
    sTestRoom.mode = (uint8_t) MDKR_ONLINE_SCREEN_MODE_TOURNAMENT;
    sTestRoom.phase = (uint8_t) RES_PHASE_RESULTS; /* MDKR_ONLINE_RESULTS */
    /* the round is what the stand-in reducer has advanced via observed
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
    /* Two occupied seats: seat 0 local+host, seat 1 the remote rival. seat 0 is the
     * room HOST because the resident soak drives its own cup advancement from the
     * LOCAL endpoint (host-press) -- a rig requirement, NOT a claim about who wins.
     * It is immaterial to the ENDPOINT-AGNOSTIC champion crown: the ceremony crowns
     * by the CAPTURED points ranking, so in the champion-on-disconnect scenario
     * (where this local host is the LOSER and the winning remote joiner departs) the
     * crown + champLocal=0 hold regardless of seat 0 being host. */
    sTestRoom.seats[0].occupied = 1u;
    sTestRoom.seats[0].is_local = 1u;
    sTestRoom.seats[0].is_host = 1u;
    sTestRoom.seats[0].connected = 1u;
    sTestRoom.seats[0].character_id = sTestSeatChar[0];
    /* RESULTS "YOU/YOU": give the local seat a REALISTIC placeholder
     * name ("P1") -- NOT the literal "YOU". The render correctly tags the local
     * player's OWN row with a " [YOU]" marker (retail-authentic), so a seat literally
     * named "YOU" rendered the confusing "YOU [YOU]" doubling the acceptance gate flagged.
     * That doubling was ALWAYS a test-harness artifact of the placeholder name (the
     * results witness never emits names, so nothing asserts on it), NOT a 2-endpoint
     * bug: in real play the seat carries the launcher-provided player name and the
     * row reads "<name> [YOU]". Naming it "P1" makes future captures read that way
     * too, without changing any render logic (nothing is masked). */
    memcpy(sTestRoom.seats[0].name, "P1", sizeof("P1"));
    sTestRoom.seats[1].occupied = 1u;
    sTestRoom.seats[1].connected = 1u;
    sTestRoom.seats[1].character_id = sTestSeatChar[1];
    memcpy(sTestRoom.seats[1].name, "RIVAL", sizeof("RIVAL"));
    /* SINGLE-race chooser variant (env MDKR_TEST_ONLINE_RESULTS_CHOOSER=single*):
     * publish a SINGLE-race room (no cup, a configured track, no points) so the
     * chooser fronts its SINGLE list (CHANGE TRACK etc.). Inert otherwise, so the
     * tournament resident soak + every existing lane are byte-behaviour-unchanged. */
    if (results_chooser_seam_single()) {
        sTestRoom.mode = (uint8_t) MDKR_ONLINE_SCREEN_MODE_SINGLE;
        sTestRoom.cup_id = 0xFFu;             /* CUP_UNSET */
        sTestRoom.configured_track = 5u;      /* a single-race track */
        for (i = 0u; i < RES_SLOTS; i++) {
            sTestRoom.points[i] = 0u;         /* single race: no cup points */
        }
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
    /* joiner-mirror lane: the joiner never publishes REMATCH (watch-only), so
     * the stand-in reducer would never leave RESULTS on its own. After the joiner
     * mirror has been shown for a while, depart the feed (-> LOBBY) to STAND IN for
     * the host's authoritative REMATCH landing over the transport, so the joiner
     * follows into re-selection. Inert unless MDKR_TEST_ONLINE_RESULTS_CHOOSER=joiner. */
    if (results_chooser_seam_joiner() &&
        sTestRoom.phase == (uint8_t) RES_PHASE_RESULTS &&
        sRes.stage == RES_STAGE_CHOOSER && sRes.stageTicks >= RES_CHOOSER_JOINER_STANDIN_DWELL) {
        if (results_chooser_seam_joiner_vacate()) {
            /* Vanished-host control: the sole remote seat departs the room while
             * the feed stays in RESULTS. The mirror's own vacated-host exit must
             * then end the session (the online_session detector is gated off the
             * final-standings chooser). */
            if (sTestRoom.seats[1].occupied) {
                sTestRoom.seats[1].occupied = 0u;
                sTestRoom.seats[1].connected = 0u;
                fprintf(stderr,
                        "[online-results] test-reducer: joiner-mirror host seat "
                        "vacated (feed held in RESULTS)\n");
            }
        } else if (!results_chooser_seam_joiner_hold()) {
            /* Default joiner lane: the host's authoritative REMATCH lands, so the
             * feed leaves RESULTS and the mirror follows into re-selection. */
            sTestRoom.phase = 1u; /* MDKR_ONLINE_LOBBY -- host REMATCH landed */
            fprintf(stderr,
                    "[online-results] test-reducer: joiner-mirror feed departed "
                    "RESULTS -> LOBBY (host authoritative REMATCH stand-in)\n");
        }
        /* joiner-hold: do nothing -- the host stays present and deliberating in
         * RESULTS well past the old 10s dwell, so the mirror must keep waiting. */
    }
    mdkr_party_link_publish(&sTestRoom);
}

/* Stand-in reducer: polls the RESULTS screen's republished REMATCH
 * intent and, on rematch_requested, advances the scripted room's cup round AND
 * leaves the RESULTS phase (-> LOBBY) -- exactly what the launcher reducer's
 * leader-only, RESULTS-gated MDKR_ONLINE_REMATCH does. The phase gate makes the
 * every-tick republish idempotent (race_index advances exactly once), and the
 * phase-leaves-RESULTS is the screen's convergence signal to finally ADVANCE.
 * Inert in a normal run. */
/* HOST-side chooser joiner-vanish seam (env MDKR_TEST_ONLINE_RESULTS_JOINER_GONE):
 * once the HOST is on the "MORE RACES?" chooser, vacate the sole remote (joiner)
 * seat in the stand-in room -- the joiner drops while the host deliberates. The
 * host chooser has NO vacate detector by design (a host may deliberate freely),
 * so the ruled behavior is that the host is NOT interrupted and can still commit
 * FINISH -> the leader-only wrap converges with one seat -> CEREMONY (from the
 * ranking latched while both were present) -> FINISHED. Inert in every normal
 * run; the vacate happens after the final-standings ranking is already latched. */
static s8 sJoinerGoneActive = -1;
static u8 results_chooser_joiner_gone_seam(void) {
    if (sJoinerGoneActive < 0) {
        const char *e = getenv("MDKR_TEST_ONLINE_RESULTS_JOINER_GONE");
        sJoinerGoneActive = (e != NULL && e[0] != '\0') ? 1 : 0;
    }
    return (u8) (sJoinerGoneActive > 0 ? 1 : 0);
}

/* HOST-side chooser joiner-vanish (see results_chooser_joiner_gone_seam): drop
 * the remote seat in the stand-in room while the host is DELIBERATING on the
 * chooser -- called every chooser tick (before the FINISH commit), so the host's
 * next snapshot read sees the joiner gone. Fires once. Requires the stand-in room
 * (resident soak). The host chooser has no vacate detector, so the host must NOT
 * be interrupted: it still commits FINISH -> ceremony -> FINISHED, alone. */
static void results_chooser_maybe_vacate_joiner(void) {
    results_test_resolve();
    if (!sTestActive || !results_chooser_joiner_gone_seam()) {
        return;
    }
    if (sRes.host && !sRes.chooserJoiner && !sRes.chooserCommitted &&
        sTestRoom.seats[1].occupied) {
        sTestRoom.seats[1].occupied = 0u;
        sTestRoom.seats[1].connected = 0u;
        mdkr_party_link_publish(&sTestRoom);
        fprintf(stderr,
                "[online-results] test-reducer: HOST-chooser joiner seat vacated "
                "(joiner dropped mid-deliberation)\n");
    }
}

static void results_test_reduce(void) {
    MdkrPartyLinkLocalIntent intent;
    results_test_resolve();
    if (!sTestActive) {
        return;
    }
    if (mdkr_party_link_intent_poll(&intent) && intent.rematch_requested) {
        if (sTestRoom.phase == (uint8_t) RES_PHASE_RESULTS) {
            if (sTestRaceIndex >= RES_CUP_ROUNDS - 1u) {
                /* The FINAL wrap, mirrored from lobby_core.c's REMATCH at the
                 * last cup round: the next round starts a FRESH series --
                 * race_index back to 0 and the points/placements RESET. Keeping
                 * the stand-in faithful here is load-bearing: the ceremony must
                 * crown from the session's LATCHED ranking, and a stand-in that
                 * kept the points alive after the wrap would mask a latch that
                 * wrongly re-reads live points. */
                unsigned slot;
                sTestRaceIndex = 0u;
                for (slot = 0u; slot < RES_SLOTS; slot++) {
                    sTestPoints[slot] = 0u;
                    sTestLastPlacements[slot] = RES_PLACE_NONE;
                    sTestRoom.points[slot] = 0u;
                    sTestRoom.last_placements[slot] = RES_PLACE_NONE;
                }
                sTestRoom.race_index = 0u;
            } else if (sTestRaceIndex < 0xFFu) {
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

/* live-resident host-press seam (env MDKR_TEST_ONLINE_RESULTS_HOST_PRESS):
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

/* IMPORTANT-1 proof seam (env MDKR_TEST_ONLINE_RESULTS_JOINER_FINISH): at
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

/* NO-SEAM proof seam (env MDKR_TEST_ONLINE_RESULTS_JOINER_TERMINAL):
 * at the FINAL standings ONLY, route the terminal into the JOINER branch (suppress
 * the host "A: FINISH"), WITHOUT forcing results_joiner_finish_departed -- so the
 * forward feed genuinely stays in RESULTS (the loopback reducer parks there on the
 * final race, no REMATCH/CLOSE) and the joiner leaves via the REAL production
 * paths only: the self-advance DWELL, or an honored A/B press. This is the seam
 * that proves the fix without the masking MDKR_TEST_ONLINE_RESULTS_JOINER_FINISH
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

/* Remote-vacate probe (env MDKR_TEST_ONLINE_REMOTE_VACATE_AT_RESULTS_FINAL):
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

/* champion-on-disconnect seam (env MDKR_TEST_ONLINE_RESIDENT_REMOTE_WINS):
 * flip the resident soak's placements on EVERY race (results_test_resolve forces
 * slot 1 first, slot 0 second each round) so the REMOTE seat wins the cup and the
 * LOCAL seat is the loser. Paired with the ceremony's remote-absent seam, this
 * stages the exact production defect it guards: a losing local endpoint whose
 * winning remote departs at ceremony enter. Inert unless the env is set. */
static s8 sResidentRemoteWins = -1;
static u8 results_resident_remote_wins(void) {
    if (sResidentRemoteWins < 0) {
        const char *e = getenv("MDKR_TEST_ONLINE_RESIDENT_REMOTE_WINS");
        sResidentRemoteWins = (e != NULL && e[0] != '\0') ? 1 : 0;
    }
    return (u8) (sResidentRemoteWins > 0 ? 1 : 0);
}

u8 mdkr_online_results_test_active(void) {
    results_test_resolve();
    return (u8) (sTestActive > 0 ? 1 : 0);
}

/* ======================================================================== *
 * chooser seam (env MDKR_TEST_ONLINE_RESULTS_CHOOSER; inert in a normal run):
 *   "<n>"    the host auto-navigates to option index n and presses A (drives the
 *            intent + routing for that option, per scenario)
 *   "show"   the chooser is fronted and HELD (no auto-select) -- the frame-dump /
 *            "just render it" variant used for the visual (PNG) proof
 *   "joiner" the chooser renders as the display-only joiner mirror (role-flipped
 *            on the host-seat rig, the results_joiner_terminal_seam pattern), and
 *            the stand-in feed later departs RESULTS so the follow path runs
 *   "joiner-hold"   the joiner mirror with the feed HELD in RESULTS (host present,
 *            deliberating past the old 10s dwell) -- the mirror must keep waiting
 *   "joiner-vacate" the joiner mirror, then the sole remote (host) seat vacates
 *            while the feed stays in RESULTS -- the mirror must end the session
 * The env being set at all forces the chooser ON (mdkr_online_results_chooser_test_
 * active), so the dedicated lane can ride the RESIDENT soak to reach a terminal. */
static s8 sChooserSeamKind = -1; /* -1 unresolved; 0 off; 1 select; 2 show; 3 joiner;
                                  * 4 joiner-hold; 5 joiner-vacate */
static s8 sChooserSeamIndex = -1;
static u8 sChooserSeamSingle;    /* the "single:" prefix -> SINGLE-race chooser */
/* A scripted SEQUENCE of option indices: the Nth chooser commit selects
 * sChooserSeq[N]. A lone index is a sequence of length 1 (the historical
 * one-shot). The capstone needs "0,5" == RACE AGAIN then FINISH across two
 * RESULTS terminals; every prior lane passes a single index and is unchanged. */
#define CHOOSER_SEQ_MAX 8u
static s8 sChooserSeq[CHOOSER_SEQ_MAX];
static u8 sChooserSeqLen;
static u8 sChooserSeqPos;
static void results_chooser_seam_resolve(void) {
    if (sChooserSeamKind >= 0) {
        return;
    }
    {
        const char *e = getenv("MDKR_TEST_ONLINE_RESULTS_CHOOSER");
        if (e == NULL || e[0] == '\0') {
            sChooserSeamKind = 0;
            return;
        }
        if (strncmp(e, "single:", 7) == 0) {
            sChooserSeamSingle = 1u; /* SINGLE-race stand-in room */
            e += 7;
        }
        if (strcmp(e, "show") == 0) {
            sChooserSeamKind = 2;
        } else if (strcmp(e, "joiner") == 0) {
            sChooserSeamKind = 3;
        } else if (strcmp(e, "joiner-hold") == 0) {
            sChooserSeamKind = 4; /* mirror, but the feed HOLDS in RESULTS (host
                                   * present, deliberating past the old dwell) */
        } else if (strcmp(e, "joiner-vacate") == 0) {
            sChooserSeamKind = 5; /* mirror, then the host seat vacates while the
                                   * feed stays in RESULTS (vanished-host control) */
        } else if (e[0] >= '0' && e[0] <= '9') {
            /* One index, or a comma-separated SEQUENCE (e.g. "0,5"). Each commit
             * advances to the next; after the last, the chooser holds. */
            sChooserSeamKind = 1;
            sChooserSeqLen = 0u;
            while (*e >= '0' && *e <= '9' && sChooserSeqLen < CHOOSER_SEQ_MAX) {
                sChooserSeq[sChooserSeqLen++] = (s8) strtol(e, NULL, 10);
                while (*e >= '0' && *e <= '9') e++;
                if (*e == ',') e++;
            }
            sChooserSeamIndex = sChooserSeqLen > 0u ? sChooserSeq[0] : (s8) -1;
        } else {
            sChooserSeamKind = 2; /* any other non-empty value: show-only */
        }
    }
}
static u8 results_chooser_seam_single(void) {
    results_chooser_seam_resolve();
    return sChooserSeamSingle;
}
/* Fires the SEQUENCE in order: the Nth commit selects sChooserSeq[N]. After the
 * last element subsequent chooser entries HOLD (return -1) rather than auto-
 * selecting again, so a soak cannot loop re-races to the tick budget. A lone
 * index is a length-1 sequence -- the historical one-shot behaviour. */
static s8 results_chooser_seam_select(void) {
    results_chooser_seam_resolve();
    if (sChooserSeamKind != 1 || sChooserSeqPos >= sChooserSeqLen) {
        return (s8) -1;
    }
    return sChooserSeq[sChooserSeqPos];
}
static void results_chooser_seam_mark_fired(void) {
    if (sChooserSeqPos < sChooserSeqLen) sChooserSeqPos++;
}
static u8 results_chooser_seam_show(void) {
    results_chooser_seam_resolve();
    return (u8) (sChooserSeamKind == 2 ? 1 : 0);
}
static u8 results_chooser_seam_joiner(void) {
    results_chooser_seam_resolve();
    return (u8) ((sChooserSeamKind == 3 || sChooserSeamKind == 4 ||
                  sChooserSeamKind == 5)
                     ? 1
                     : 0);
}
static u8 results_chooser_seam_joiner_hold(void) {
    results_chooser_seam_resolve();
    return (u8) (sChooserSeamKind == 4 ? 1 : 0);
}
static u8 results_chooser_seam_joiner_vacate(void) {
    results_chooser_seam_resolve();
    return (u8) (sChooserSeamKind == 5 ? 1 : 0);
}

u8 mdkr_online_results_chooser_test_active(void) {
    results_chooser_seam_resolve();
    return (u8) (sChooserSeamKind > 0 ? 1 : 0);
}

MdkrOnlineResultsChoice mdkr_online_results_choice(void) {
    return (MdkrOnlineResultsChoice) sRes.chooserChoice;
}

u8 mdkr_online_results_chooser_deciding(void) {
    return (sRes.stage == RES_STAGE_CHOOSER && !sRes.chooserCommitted) ? 1u : 0u;
}

u8 mdkr_online_results_single_finish(void) {
    return (sRes.chooserChoice == (u8) MDKR_ONLINE_RESULTS_CHOICE_FINISH &&
            sRes.chooserMode == (u8) MDKR_ONLINE_SCREEN_MODE_SINGLE &&
            sRes.chooserFeed)
               ? 1u
               : 0u;
}

#endif /* MDKR_ENABLE_ONLINE_BETA */
