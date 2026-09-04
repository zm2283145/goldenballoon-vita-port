/* SEPARATED-BOOT-PATH native online champion CEREMONY screen.
 *
 * ============================ THE REUSE BOUNDARY ========================
 * The post-final-standings SCREEN of the separated online flow: a native,
 * decomp-authentic 2D celebration of the cup champion, between the final
 * STANDINGS and the session's hand-back to the launcher.
 * RE-IMPLEMENT the presentation here using the GAME'S OWN decoded assets rather
 * than calling the offline trophy-ceremony cinematic.
 *
 * WHY IT DOES NOT REUSE THE OFFLINE TROPHY CEREMONY: selecting the offline
 * ASSET_MENU_TEXT_TROPHYCEREMONY plays get_misc_asset(ASSET_MISC_CINEMATIC_TROPHY)
 * via cinematic_start + menu_init(MENU_NEWGAME_CINEMATIC) -- i.e. it RE-ENTERS the
 * offline GAMEMODE_MENU state machine, the one thing the online session
 * categorically forbids (online_session.h). It is also single-player-progression
 * coupled (settings->trophies, world-unlock cinematics). So this screen borrows
 * the DATA + the DRAW/SFX primitives, exactly like online_results.c /
 * online_charselect.c, and owns all of its own state.
 *
 * WHAT IT BORROWS (read-only reuse of already-compiled game code/data):
 *   - gRacerPortraits[10] + the portrait-only asset group (menu.c) -- the same
 *     ten faces charselect/results borrow (portrait-ONLY id list, no menu OBJECTS).
 *   - gRacePlacementsArray[8] "1ST".."8TH" (menu.c) -- the real place labels.
 *   - draw_text / set_text_* (font.h), texrect_draw / bgdraw_* (rcp_dkr.h),
 *     sound_play + SOUND_* (audio.h / sound_ids.h), input_pressed (joypad.h).
 *
 * WHAT IT READS (never mutates): the champion identity from the party_link
 * forward-feed snapshot (points[]/last_placements/seats[]), ranked by the SHARED
 * mdkr_online_standings_compute() the RESULTS screen also runs (online_standings.h)
 * so the ceremony's champion is byte-for-byte the seat STANDINGS crowned #1.
 *
 * ADVANCE MODEL (the safety contract): a bounded TIMED auto-advance that fires for
 * EVERY endpoint on a fixed frame budget -- NO cross-endpoint / snapshot
 * convergence gate, so it is impossible to hang the session on. A host press may
 * SKIP the hold early; a joiner never blocks anyone; a mid-ceremony remote vacate
 * ends it promptly. It adds NO second required "press A" (the RESULTS terminal
 * already took the one human FINISH confirm).
 *
 * The ENTIRE TU is #if MDKR_ENABLE_ONLINE_BETA and it is added to the build only
 * inside the beta CMake gate (game/src/online/ is not globbed), so a normal (beta
 * OFF) build never compiles a byte of it and the release engine object is
 * untouched.
 * ==========================================================================
 */
#include "online/online_ceremony.h"

#if MDKR_ENABLE_ONLINE_BETA

/* Game/PR headers FIRST (same include set + ordering as online_results.c). */
#include "types.h"
#include "thread3_main.h"
#include "enums.h"      /* AlignmentFlags */
#include "menu.h"       /* gRacerPortraits, menu_assetgroup_load/free,
                           menu_racer_portraits, gRacePlacementsArray,
                           TEXTURE_ICON_PORTRAIT_*, font.h (draw_text, ...) */
#include "rcp_dkr.h"    /* texrect_draw, bgdraw_fillcolour */
#include "audio.h"      /* sound_play */
#include "sound_ids.h"  /* SOUND_CROWD / SOUND_SELECT3 */
#include "joypad.h"     /* input_pressed */
#include "PR/os_cont.h" /* A_BUTTON / START_BUTTON */
#include "net/party_link.h"
#include "online/online_screen_constants.h" /* shared id-space mirrors (phase bytes) */
#include "online/online_standings.h" /* the shared champion sort (DRY with results) */
#include "online/online_portraits.h" /* the shared portrait/name/asset
                                        tables (DRY with charselect/results) */
#include "online/online_screen_util.h" /* shared local_seat / text / pulse /
                                          seat_name / seconds_left / draw_portrait */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Screen space (mirrored, like online_results.c). */
#define CER_SCREEN_W_HALF 160

/* ---- Local mirrors of the launcher lobby's id space (no launcher headers) --- */
#define CER_CHAR_COUNT 10u        /* MDKR_ONLINE_CHARACTER_COUNT */
#define CER_LOCAL_PAD 0           /* PLAYER_ONE */

/* The bounded celebration hold: a fixed FRAME budget (updateRate accumulates in
 * 60ths of a second, so seconds*60 -- the RES_STANDINGS_UNITS vocabulary). ~6s is
 * long enough to land as a real "you won the cup" moment but short enough to never
 * read as a flash or a stall; it auto-advances for EVERY endpoint, so the session
 * can never hang here. */
#define CER_HOLD_UNITS 360u /* 6s */

/* A host may SKIP the hold early with A/START, but only after this entry lockout
 * (~0.5s) -- so a host still mashing "A: FINISH" at the final standings cannot
 * skip past the champion on frame one (mirrors RES_INPUT_GRACE). */
#define CER_ENTRY_GRACE 30u

/* A mid-ceremony remote vacate must persist this many frames before it ends the
 * screen, so a one-frame feed blip never trips it (mirrors the pre-START
 * remote-vacate debounce). The forced test seam bypasses the debounce. */
#define CER_VACATE_DEBOUNCE 15u

/* Menu SFX (the real DKR enums; same reuse discipline as RESULTS). */
#define CER_SFX_CELEBRATE SOUND_CROWD   /* a crowd cheer for the champion (on enter) */
#define CER_SFX_ADVANCE SOUND_SELECT3   /* the skip/advance blip */

/* Online id -> portrait / name / asset-id tables: the DRY lift into the
 * shared online_portraits.h (byte-identical across charselect/results/ceremony;
 * sOnlineToPortrait[], sOnlineNames[], sPortraitAssetIds[] now live there). */

/* gCurrDisplayList + gRacerPortraits[] are declared in online_screen_util.h,
 * shared with the other native screens; the place labels (menu.c
 * gRacePlacementsArray, not exposed by menu.h) are declared here. */
extern char *gRacePlacementsArray[8];

/* ---- Session-owned screen state (never an offline global) ------------------ */
typedef struct MdkrOnlineCeremonyState {
    u8 assets;        /* portrait group + fonts loaded */
    u8 host;          /* local seat is the room leader (may skip early) */
    u8 single;        /* the room is SINGLE-race (snapshot mode at enter; the mode
                       * survives the REMATCH wrap): the copy reads "RACE WINNER"/
                       * "RACE COMPLETE" and the meaningless zero POINTS figures
                       * are dropped. Tournament copy is byte-identical. */
    MdkrOnlineStandings st; /* the ranked seats (order[0] == champion), from enter */
    u8 champSeat;     /* champion canonical seat slot, or 0xFF if unresolved */
    u8 champChar;     /* champion character id, or 0xFF */
    u16 champPoints;  /* champion final cup total */
    u8 champLocal;    /* the champion seat is THIS endpoint's local seat */
    u8 done;          /* an ADVANCE/LEAVE was already returned (edge guard) */
    u32 stageTicks;   /* the hold accumulator */
    u32 pulseTicks;   /* free-running (drives the footer pulse, never reset) */
    u16 vacateTicks;  /* remote-absent debounce */
} MdkrOnlineCeremonyState;

static MdkrOnlineCeremonyState sCer;

/* Witness change-detect (file scope so _enter can reset for a clean re-entry). */
static u32 sWitnessKey = 0xFFFFFFFFu;

/* ---- forward decls (test seams at the bottom) ----------------------------- */
static u8 ceremony_skip_active(void);
static u8 ceremony_vacate_forced(void);
static u8 ceremony_remote_absent_forced(void);

/* Read the party_link snapshot for the ceremony. Normally a straight
 * mdkr_party_link_read; with the remote-absent seam armed it additionally DROPS
 * every occupied non-local seat, so the LIVE snapshot the ceremony reads is
 * genuinely seat-absent -- standing in for a real host disconnect at ceremony
 * enter, which no loopback rig can cheaply stage. This exercises the REAL
 * downstream condition (the champion resolution over a snapshot that has lost the
 * winner's seat), unlike a seam that only feeds a predicate the shipped code never
 * reaches. Inert unless the env is set. */
static bool ceremony_read_snapshot(MdkrPartyLinkSnapshot *snap) {
    bool haveSnap = mdkr_party_link_read(snap);
    if (haveSnap && ceremony_remote_absent_forced()) {
        unsigned i;
        for (i = 0u; i < MDKR_PARTY_LINK_SEATS; i++) {
            if (snap->seats[i].occupied && !snap->seats[i].is_local) {
                snap->seats[i].occupied = 0u;
                snap->seats[i].connected = 0u;
                snap->seats[i].character_id = 0xFFu; /* no name / no portrait */
                snap->seats[i].name[0] = '\0';
            }
        }
    }
    return haveSnap;
}

/* ======================================================================== *
 * Small helpers
 * ======================================================================== */
/* Any occupied seat that is NOT the local player -- the remote(s) still present in
 * the room. Mirrors online_session_snapshot_has_remote_seat. */
static bool ceremony_has_remote_seat(const MdkrPartyLinkSnapshot *snap) {
    unsigned i;
    for (i = 0u; i < MDKR_PARTY_LINK_SEATS; i++) {
        if (snap->seats[i].occupied && !snap->seats[i].is_local) {
            return true;
        }
    }
    return false;
}
static bool ceremony_has_local_seat(const MdkrPartyLinkSnapshot *snap) {
    unsigned i;
    for (i = 0u; i < MDKR_PARTY_LINK_SEATS; i++) {
        if (snap->seats[i].occupied && snap->seats[i].is_local) {
            return true;
        }
    }
    return false;
}

/* The champion's display name, resolved from the CAPTURED character id (latched at
 * the final standings, sCer.champChar) rather than the live seat -- so a
 * disconnected winner still shows their canonical racer name (sOnlineNames)
 * instead of a "Pn" slot fallback. It is the exact sibling of the portrait, which
 * blits from the same sCer.champChar. On the happy path char_id[0] == the live
 * champion's character and the real flow leaves seat names empty (party_link.c), so
 * this returns the same canonical name the prior live-seat lookup produced. */
static void ceremony_champ_name(char *out, size_t cap) {
    if (sCer.champSeat == 0xFFu) {
        (void) snprintf(out, cap, "(none)");
    } else if (sCer.champChar < MDKR_ONLINE_PORTRAIT_COUNT) {
        (void) snprintf(out, cap, "%s", sOnlineNames[sCer.champChar]);
    } else {
        /* Nameless/invalid-character champion: the generic remote-player fallback the
         * native screens use everywhere (RIVAL), not a "Pn" slot label. Only reached
         * by an absent remote winner (a captured local champion always has a valid
         * character); the local-winner " - YOU!" tag disambiguates regardless. */
        (void) snprintf(out, cap, "RIVAL");
    }
}

/* ======================================================================== *
 * Render (native: real portraits + real font, into the engine frame list)
 * ======================================================================== */
static void ceremony_render(const MdkrPartyLinkSnapshot *snap, bool haveSnap) {
    /* Retail selected-item cadence (menu.c gOptionBlinkTimer: 0x3F wrap, *8
     * triangle, 0..255) via the shared helper -- the SAME blink the other native
     * screens use, so the "RETURNING TO ROOM..." heartbeat breathes at the
     * authentic DKR rate (was the faster/dimmer 0..16 mdkr_online_screen_pulse). */
    s32 blink = mdkr_online_screen_blink(sCer.pulseTicks);
    u8 pg = (u8) (170 + blink / 3); /* 170..255 pulse */
    char line[64];
    char name[32];
    s32 rowY;
    unsigned i;

    /* Grounds first: title strip, the champion board, the runners-up card (only
     * when there are runners-up) and the footer strip -- retail figure-ground,
     * no naked body text over the sky. */
    mdkr_online_screen_strip(8, 48);
    /* Champion board width sized for the WIDEST retail roster name at BIGFONT
     * scale: DRUMSTICK measures 142px, and the no-champion "CUP COMPLETE" 183px
     * -- both overspilled the former 136px (92..228) board. 60..260 (200px)
     * clears the widest content and shares its edges with the runners-up card
     * below, so the two stack as one tidy column. */
    if (sCer.champSeat != 0xFFu) {
        mdkr_online_screen_panel(60, 64, 260, 186);
    } else {
        mdkr_online_screen_panel(60, 104, 260, 136);
    }
    if (sCer.st.count > 1u) {
        s32 nRunners = (s32) (sCer.st.count - 1u);
        if (nRunners > 2) {
            nRunners = 2; /* two podium rows fit above the footer strip */
        }
        mdkr_online_screen_panel(60, 188, 260, 192 + nRunners * 12);
    }
    mdkr_online_screen_strip(218, 240);

    /* Headline: a big gold "CHAMPION" + a smaller congratulations line. */
    mdkr_online_screen_text(CER_SCREEN_W_HALF, 24, ASSET_FONTS_BIGFONT, "CHAMPION",
                  ALIGN_MIDDLE_CENTER, 255, 224, 96);
    mdkr_online_screen_text(CER_SCREEN_W_HALF, 40, ASSET_FONTS_SMALLFONT, "CONGRATULATIONS!",
                  ALIGN_MIDDLE_CENTER, 210, 210, 210);

    /* The champion: a prominent centred portrait + name + point total on the
     * board. The winner's name is BIGFONT gold (unmistakable); the total is
     * FUNFONT because BIGFONT has no digit glyphs (the RESULTS rank/points
     * reason). BIGFONT also has no '['/']' glyphs, so the local-winner tag
     * rides the small CUP CHAMPION line, never the BIGFONT name. */
    if (sCer.champSeat != 0xFFu) {
        ceremony_champ_name(name, sizeof(name));
        mdkr_online_screen_draw_portrait(sCer.champChar, CER_SCREEN_W_HALF - 22, 74,
                               255, 224, 96);
        (void) snprintf(line, sizeof(line), "%.12s", name);
        mdkr_online_screen_text(CER_SCREEN_W_HALF, 138, ASSET_FONTS_BIGFONT, line,
                      ALIGN_MIDDLE_CENTER, 255, 224, 96);
        (void) snprintf(line, sizeof(line), "%s%s",
                        sCer.single ? "RACE WINNER" : "CUP CHAMPION",
                        sCer.champLocal ? " - YOU!" : "");
        mdkr_online_screen_text(CER_SCREEN_W_HALF, 158, ASSET_FONTS_SMALLFONT, line,
                      ALIGN_MIDDLE_CENTER, 255, 255, 255);
        /* Points: the FUNFONT total is no longer an orphan number -- pair it with
         * a SMALLFONT "POINTS" caption (the runner rows read as "PLACE name pts",
         * so the lone champion figure needs the same "these are points" cue). The
         * FUNFONT figure + the caption are centred as one unit via get_text_width.
         * A SINGLE race has no cup total (points are structurally 0), so the
         * figure is dropped there -- a "0 POINTS" crown would read as broken. */
        if (!sCer.single) {
            s32 numW, lblW, leftX;
            (void) snprintf(line, sizeof(line), "%u", (unsigned) sCer.champPoints);
            numW = get_text_width(line, 0, ASSET_FONTS_FUNFONT);
            lblW = get_text_width((char *) "POINTS", 0, ASSET_FONTS_SMALLFONT);
            leftX = CER_SCREEN_W_HALF - (numW + 5 + lblW) / 2;
            mdkr_online_screen_text(leftX, 176, ASSET_FONTS_FUNFONT, line,
                          ALIGN_MIDDLE_LEFT, 255, 224, 96);
            mdkr_online_screen_text(leftX + numW + 5, 176, ASSET_FONTS_SMALLFONT,
                          "POINTS", ALIGN_MIDDLE_LEFT, 210, 210, 210);
        }
    } else {
        mdkr_online_screen_text(CER_SCREEN_W_HALF, 120, ASSET_FONTS_BIGFONT,
                      sCer.single ? (char *) "RACE COMPLETE"
                                  : (char *) "CUP COMPLETE",
                      ALIGN_MIDDLE_CENTER, 255, 224, 96);
    }

    /* Runners-up: the rest of the podium in small rows below (2ND, 3RD, ...),
     * reusing the real place labels -- so the standings are still legible under
     * the champion, not just the winner in isolation. */
    rowY = 196;
    for (i = 1u; i < sCer.st.count && i < 3u; i++) {
        unsigned slot = sCer.st.order[i];
        mdkr_online_screen_seat_name(snap, haveSnap, slot, name, sizeof(name));
        if (sCer.single) {
            /* single race: place + name only (a zero points column would read
             * as broken -- there is no cup total to show). */
            (void) snprintf(line, sizeof(line), "%s  %.10s",
                            (i < 8u) ? gRacePlacementsArray[i] : "-", name);
        } else {
            (void) snprintf(line, sizeof(line), "%s  %.10s  %u",
                            (i < 8u) ? gRacePlacementsArray[i] : "-", name,
                            (unsigned) sCer.st.points[i]);
        }
        mdkr_online_screen_text(CER_SCREEN_W_HALF, rowY, ASSET_FONTS_SMALLFONT, line,
                      ALIGN_MIDDLE_CENTER, 200, 200, 200);
        rowY += 12;
    }

    /* Footer: a pulsed "returning" heartbeat (never a dead hold) + the host's
     * optional early-skip affordance. No countdown number, no required button. */
    mdkr_online_screen_text(CER_SCREEN_W_HALF, 224, ASSET_FONTS_SMALLFONT,
                  "RETURNING TO ROOM...", ALIGN_MIDDLE_CENTER, 120, pg, 120);
    if (sCer.host) {
        mdkr_online_screen_text(CER_SCREEN_W_HALF, 234, ASSET_FONTS_SMALLFONT, "A: CONTINUE",
                      ALIGN_MIDDLE_CENTER, 255, 255, 255);
    }
}

/* Bounded stderr witness: one line only when the visible state changes, so the
 * headless lane can read the drawn champion + countdown (mirrors results_witness). */
static void ceremony_witness(void) {
    u32 secs = mdkr_online_screen_seconds_left(sCer.stageTicks, CER_HOLD_UNITS);
    u32 key = ((u32) sCer.champSeat & 0xFu) |
              ((u32) (sCer.champPoints & 0xFFFu) << 4) |
              ((u32) (secs & 0x3Fu) << 16) |
              ((u32) (sCer.host ? 1u : 0u) << 22);
    if (key == sWitnessKey) {
        return;
    }
    sWitnessKey = key;
    fprintf(stderr,
            "[online-ceremony] render champion=%u points=%u secs=%u host=%u "
            "seats=%u\n",
            (unsigned) sCer.champSeat, (unsigned) sCer.champPoints,
            (unsigned) secs, (unsigned) sCer.host, (unsigned) sCer.st.count);
}

/* ======================================================================== *
 * Lifecycle
 * ======================================================================== */
void mdkr_online_ceremony_enter(const MdkrOnlineStandings *finalRanking) {
    MdkrPartyLinkSnapshot snap;
    bool haveSnap;
    bool useCaptured;
    bool crown;
    s32 localSeat;
    char name[32];

    memset(&sCer, 0, sizeof(sCer));
    sCer.champSeat = 0xFFu;
    sCer.champChar = 0xFFu;
    sWitnessKey = 0xFFFFFFFFu;

    /* The local seat identity is stable even after the remote leaves (this
     * endpoint has not departed), so resolve it from the live snapshot for the
     * champLocal / host affordance either way. */
    haveSnap = ceremony_read_snapshot(&snap);
    localSeat = haveSnap ? mdkr_online_screen_local_seat(&snap) : -1;
    /* SINGLE-race copy gate: read the mode once at enter. The REMATCH wrap that
     * preceded this ceremony never touches the mode (lobby_core.c), so the
     * post-wrap snapshot still names the raced mode; a feed-less endpoint keeps
     * the historical tournament copy. */
    sCer.single = (haveSnap &&
                   snap.mode == (uint8_t) MDKR_ONLINE_SCREEN_MODE_SINGLE)
                      ? 1u
                      : 0u;

    /* Prefer the ranking the session CAPTURED at the final standings while BOTH
     * seats were present (the SAME sort the STANDINGS screen ran, so the two
     * screens can never disagree). Only when nothing was captured -- a disconnect
     * so early the final standings never latched with two present -- fall back to
     * a live compute from the current snapshot. */
    useCaptured = (finalRanking != NULL && finalRanking->count > 0u);
    if (useCaptured) {
        sCer.st = *finalRanking;
    } else {
        mdkr_online_standings_compute(&snap, haveSnap, &sCer.st);
    }

    /* Crown the captured winner directly. In the FALLBACK live path only, refuse
     * to crown when the live table cannot be trusted:
     *   - a lone survivor (fewer than two seats remain): the winner the human
     *     saw was never latched, so declaring whoever is left would mis-crown;
     *   - the room has ALREADY LEFT RESULTS (or there is no snapshot at all):
     *     the tournament-final wrap RESETS the live points to a fresh series
     *     (lobby_core.c reset_tournament_series), so a live compute over a
     *     wrapped/absent feed ranks an all-zero table and crowns by seat order.
     *     This window is real over the WAN: if the wrap State lands before the
     *     joiner's FIRST RESULTS-phase capture, the session hands the ceremony
     *     NULL and only this refusal stands between the screen and a zero-point
     *     champion.
     * The honest degradation is the neutral "CUP COMPLETE" screen (no crown);
     * FINISHED still fires exactly once downstream either way. */
    crown = (sCer.st.count > 0u) &&
            (useCaptured ||
             (sCer.st.count >= 2u && haveSnap &&
              snap.phase == (uint8_t) MDKR_ONLINE_SCREEN_RESULTS_PHASE));
    if (crown) {
        sCer.champSeat = sCer.st.order[0];
        sCer.champPoints = sCer.st.points[0];
        /* champChar (and, via it, the champion's PORTRAIT + NAME) comes from the
         * CAPTURED character id, not the live seat: on a real disconnect the
         * winner's seat is gone from the live snapshot, but char_id[0] was latched
         * at the final standings while both seats were present, so the true winner
         * still shows their real face + canonical name instead of degrading to a
         * "Pn"/no-portrait fallback. On the happy path char_id[0] == the live
         * champion's character (the capture read the same published snapshot), so
         * this is byte-identical to the prior live-seat resolution. */
        sCer.champChar =
            (sCer.st.char_id[0] < CER_CHAR_COUNT) ? sCer.st.char_id[0] : 0xFFu;
        sCer.champLocal = (localSeat >= 0 && (u8) localSeat == sCer.champSeat)
                              ? 1u
                              : 0u;
    }

    /* Borrow the real portraits + fonts + per-world sky tiles (the charselect/
     * results asset-borrow discipline), set the retail scrolling sky of the raced
     * world (from the forward-feed snapshot read above), and cue a crowd cheer. */
    menu_assetgroup_load(sPortraitAssetIds);
    menu_racer_portraits();
    menu_assetgroup_load(sOnlineSkyAssetIds);
    load_font(ASSET_FONTS_BIGFONT);
    load_font(ASSET_FONTS_SMALLFONT);
    load_font(ASSET_FONTS_FUNFONT);
    /* High-definition text while this screen is up (retail letterforms at
     * native-window resolution; see online_screen_util.h). Balanced by the
     * unref in _exit(), inside the same sCer.assets guard as the fonts. */
    mdkr_online_screen_hd_text_ref();
    sCer.assets = 1u;
    mdkr_online_screen_backdrop(
        mdkr_online_screen_sky_world_for_snapshot(&snap, haveSnap));
    sound_play(CER_SFX_CELEBRATE, NULL);

    /* reveal the champion celebration from black (retail fade cadence) + keep
     * the retail menu music underneath the crowd cheer (isolation-safe primitive
     * borrows -- see online_screen_util.h). */
    mdkr_online_screen_fade_in_from_black();
    mdkr_online_screen_menu_music();

    /* Resolve the champion name from the CAPTURED character id (ceremony_champ_name):
     * a real champion shows their canonical racer name even after their seat has
     * disconnected, a feed-less endpoint (no champion) logs "(none)". Witness
     * cosmetics only -- the render path already guards the no-champion case
     * ("CUP COMPLETE"). */
    ceremony_champ_name(name, sizeof(name));
    fprintf(stderr,
            "[online-ceremony] enter: champion seat=%u name=%.12s points=%u "
            "seats=%u local=%u (native cup celebration; offline trophy cinematic "
            "bypassed)\n",
            (unsigned) sCer.champSeat, name, (unsigned) sCer.champPoints,
            (unsigned) sCer.st.count, (unsigned) sCer.champLocal);
}

void mdkr_online_ceremony_exit(void) {
    if (sCer.assets) {
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
        sCer.assets = 0u;
        fprintf(stderr, "[online-ceremony] exit: freed portrait assets\n");
    }
}

MdkrOnlineCeremonyResult mdkr_online_ceremony_tick(s32 updateRate) {
    MdkrPartyLinkSnapshot snap;
    bool haveSnap;
    s32 localSeat;
    u32 pressed;
    u8 hostSkip;

    if (updateRate <= 0) {
        updateRate = 1;
    }

    haveSnap = ceremony_read_snapshot(&snap);
    localSeat = haveSnap ? mdkr_online_screen_local_seat(&snap) : -1;
    /* A feed-less endpoint owns its own progression (host); with a feed present be
     * the host only if the resolved local seat is the leader. The host may skip
     * the hold early, but NOBODY is ever blocked on the host -- the timer below
     * fires for everyone. */
    if (!haveSnap) {
        sCer.host = 1u;
    } else {
        sCer.host = (localSeat >= 0 && snap.seats[localSeat].is_host) ? 1u : 0u;
    }

    /* Render + witness BEFORE the advance decision so the just-elapsed frame is
     * always drawn (and the headless lane always sees a render witness between the
     * final STANDINGS and FINISHED, even when a skip fires on frame one). */
    ceremony_render(&snap, haveSnap);
    ceremony_witness();

    sCer.stageTicks += (u32) updateRate;
    sCer.pulseTicks += (u32) updateRate;

    if (sCer.done) {
        return MDKR_ONLINE_CEREMONY_STAY; /* already resolved (defensive) */
    }

    /* Mid-ceremony remote VACATE: if the feed seats the local player but the
     * remote seat is gone (debounced, or forced by the test seam), end promptly ->
     * the session maps this to FINISHED. The tournament is already decided, so
     * ending a few frames early changes nothing -- it just never parks. */
    if (haveSnap && ceremony_has_local_seat(&snap) &&
        (!ceremony_has_remote_seat(&snap) || ceremony_vacate_forced())) {
        /* ONE debounce gate for BOTH the genuine (remote seat gone from the feed)
         * and the forced-test path: the test seam overrides ONLY the "remote is
         * gone" predicate (mirroring the pre-START vacate seam,
         * online_session_remote_vacate_forced), so the SAME
         * vacateTicks >= CER_VACATE_DEBOUNCE branch the shipped code takes is what
         * the vacate lane exercises -- not a substitute gate. */
        sCer.vacateTicks++;
        if (sCer.vacateTicks >= CER_VACATE_DEBOUNCE) {
            sCer.done = 1u;
            fprintf(stderr,
                    "[online-ceremony] done: champion celebration ended early "
                    "(remote vacated) -> FINISHED handshake\n");
            return MDKR_ONLINE_CEREMONY_LEAVE;
        }
    } else {
        sCer.vacateTicks = 0u;
    }

    /* Host early-SKIP: a live A/START edge after the entry lockout, OR the headless
     * skip seam. A joiner press does nothing (watch-only) -- but the timer still
     * fires for the joiner, so no one is ever stranded. */
    pressed = input_pressed(CER_LOCAL_PAD);
    hostSkip = (sCer.host && (pressed & (A_BUTTON | START_BUTTON)) &&
                sCer.stageTicks >= CER_ENTRY_GRACE)
                   ? 1u
                   : 0u;
    if (ceremony_skip_active() || hostSkip) {
        sound_play(CER_SFX_ADVANCE, NULL);
        sCer.done = 1u;
        fprintf(stderr,
                "[online-ceremony] done: champion celebration complete (%s) -> "
                "FINISHED handshake\n",
                ceremony_skip_active() ? "skip" : "host");
        return MDKR_ONLINE_CEREMONY_ADVANCE;
    }

    /* TIMED auto-advance: the bounded hold elapsed. Fires for EVERY endpoint on
     * the local clock -- the celebration can never hang the session. */
    if (sCer.stageTicks >= CER_HOLD_UNITS) {
        sound_play(CER_SFX_ADVANCE, NULL);
        sCer.done = 1u;
        fprintf(stderr,
                "[online-ceremony] done: champion celebration complete (auto) -> "
                "FINISHED handshake\n");
        return MDKR_ONLINE_CEREMONY_ADVANCE;
    }

    return MDKR_ONLINE_CEREMONY_STAY;
}

/* ======================================================================== *
 * Headless test seams (beta + env gated; entirely inert in a normal run)
 * ======================================================================== */

/* Scripted SKIP (env MDKR_TEST_ONLINE_CEREMONY_SKIP): fire the advance on the
 * first tick (after one render) so a CI lane does not wait out the real-time hold.
 * The existing hold-ending lanes set this so their frame budgets are preserved
 * with the ceremony in the path. Inert unless the env is set. */
static s8 sSkipActive = -1;
static u8 ceremony_skip_active(void) {
    if (sSkipActive < 0) {
        const char *e = getenv("MDKR_TEST_ONLINE_CEREMONY_SKIP");
        sSkipActive = (e != NULL && e[0] != '\0') ? 1 : 0;
    }
    return (u8) (sSkipActive > 0 ? 1 : 0);
}

/* Forced VACATE (env MDKR_TEST_ONLINE_CEREMONY_VACATE): make the vacate detector
 * READ the remote as gone during the ceremony -- a real transport departure cannot
 * be cheaply staged on the loopback rig, so this seam drives the debounce + LEAVE
 * + FINISHED action genuinely. Off in every normal run. */
static s8 sVacateForced = -1;
static u8 ceremony_vacate_forced(void) {
    if (sVacateForced < 0) {
        const char *e = getenv("MDKR_TEST_ONLINE_CEREMONY_VACATE");
        sVacateForced = (e != NULL && e[0] != '\0') ? 1 : 0;
    }
    return (u8) (sVacateForced > 0 ? 1 : 0);
}

/* Forced REMOTE-ABSENT (env MDKR_TEST_ONLINE_CEREMONY_REMOTE_ABSENT): drop the
 * remote (non-local) seats from the snapshot the CEREMONY reads (see
 * ceremony_read_snapshot), so the champion resolution runs over a genuinely
 * seat-absent live snapshot -- the real downstream effect of a host disconnect at
 * ceremony enter. Unlike the VACATE seam (which only forces the mid-ceremony
 * detector's "remote gone" predicate), this makes the seat ACTUALLY absent, so it
 * proves the champion is taken from the CAPTURED final ranking rather than a
 * degraded live recompute. Off in every normal run. */
static s8 sRemoteAbsentForced = -1;
static u8 ceremony_remote_absent_forced(void) {
    if (sRemoteAbsentForced < 0) {
        const char *e = getenv("MDKR_TEST_ONLINE_CEREMONY_REMOTE_ABSENT");
        sRemoteAbsentForced = (e != NULL && e[0] != '\0') ? 1 : 0;
    }
    return (u8) (sRemoteAbsentForced > 0 ? 1 : 0);
}

u8 mdkr_online_ceremony_test_active(void) {
    return ceremony_skip_active();
}

#endif /* MDKR_ENABLE_ONLINE_BETA */
