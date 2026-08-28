/* SEPARATED-BOOT-PATH (Strategy D2) native online champion CEREMONY screen.
 *
 * ============================ THE D2 REUSE BOUNDARY ========================
 * The post-final-standings SCREEN of the separated online flow: a native,
 * decomp-authentic 2D celebration of the cup champion, between the final
 * STANDINGS and the session's hand-back to the launcher. Strategy D2 means:
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
 * already took the one human FINISH confirm, PD-T6d).
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
#include "online/online_standings.h" /* the shared champion sort (DRY with results) */
#include "online/online_portraits.h" /* screens I-3: the shared portrait/name/asset
                                        tables (DRY with charselect/results) */

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

/* Online id -> portrait / name / asset-id tables: screens I-3 DRY lift into the
 * shared online_portraits.h (byte-identical across charselect/results/ceremony;
 * sOnlineToPortrait[], sOnlineNames[], sPortraitAssetIds[] now live there). */

/* The engine's live 2D display list + the decoded portraits + place labels
 * (declared here, not exposed by menu.h -- exactly how online_results.c reaches
 * them). */
extern Gfx *gCurrDisplayList;
extern DrawTexture *gRacerPortraits[10];
extern char *gRacePlacementsArray[8];

/* ---- Session-owned screen state (never an offline global) ------------------ */
typedef struct MdkrOnlineCeremonyState {
    u8 assets;        /* portrait group + fonts loaded */
    u8 host;          /* local seat is the room leader (may skip early) */
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

/* ======================================================================== *
 * Small helpers
 * ======================================================================== */
static s32 ceremony_local_seat(const MdkrPartyLinkSnapshot *snap) {
    unsigned i;
    for (i = 0u; i < MDKR_PARTY_LINK_SEATS; i++) {
        if (snap->seats[i].occupied && snap->seats[i].is_local) {
            return (s32) i;
        }
    }
    return -1;
}

/* Resolve one seat's short name (untrusted snapshot name, else the character's
 * canonical name, else a slot fallback) -- identical policy to results_seat_name. */
static void ceremony_seat_name(const MdkrPartyLinkSnapshot *snap, bool haveSnap,
                               unsigned slot, char *out, size_t cap) {
    if (haveSnap && slot < MDKR_PARTY_LINK_SEATS &&
        snap->seats[slot].occupied && snap->seats[slot].name[0] != '\0') {
        (void) snprintf(out, cap, "%.*s", (int) MDKR_PARTY_LINK_NAME_BYTES,
                        snap->seats[slot].name);
        return;
    }
    if (haveSnap && slot < MDKR_PARTY_LINK_SEATS &&
        snap->seats[slot].character_id < CER_CHAR_COUNT) {
        (void) snprintf(out, cap, "%s",
                        sOnlineNames[snap->seats[slot].character_id]);
        return;
    }
    (void) snprintf(out, cap, "P%u", slot + 1u);
}

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

/* Triangle-wave pulse 0..16 off the free-running counter (the RESULTS terminal
 * vocabulary) -- gives the celebration footer a visible heartbeat so it never
 * reads as a hang. */
static s32 ceremony_pulse(void) {
    s32 tri = (s32) (sCer.pulseTicks & 31u);
    if (tri > 16) {
        tri = 32 - tri;
    }
    return tri;
}

/* Seconds still on the celebration clock (ceil), 0 when elapsed. */
static u32 ceremony_seconds_left(void) {
    u32 done = sCer.stageTicks;
    if (done >= CER_HOLD_UNITS) {
        return 0u;
    }
    return (CER_HOLD_UNITS - done + 59u) / 60u;
}

/* ======================================================================== *
 * Render (native: real portraits + real font, into the engine frame list)
 * ======================================================================== */
static void ceremony_text(s32 x, s32 y, s32 fontId, char *text,
                          AlignmentFlags align, s32 r, s32 g, s32 b) {
    set_text_font(fontId);
    set_text_background_colour(0, 0, 0, 0);
    set_text_colour(r, g, b, 0, 255);
    draw_text(&gCurrDisplayList, x, y, text, align);
}

static void ceremony_draw_portrait(u8 character, s32 x, s32 y, u8 r, u8 g, u8 b) {
    DrawTexture *portrait;
    if (character >= CER_CHAR_COUNT) {
        return;
    }
    portrait = gRacerPortraits[sOnlineToPortrait[character]];
    if (portrait != NULL && portrait[0].texture != NULL) {
        texrect_draw(&gCurrDisplayList, portrait, x, y, r, g, b, 255);
    }
}

static void ceremony_render(const MdkrPartyLinkSnapshot *snap, bool haveSnap) {
    s32 tri = ceremony_pulse();
    u8 pg = (u8) (170 + tri * 5); /* 170..255 pulse */
    char line[64];
    char name[32];
    s32 rowY;
    unsigned i;

    /* Headline: a big gold "CHAMPION" + a smaller congratulations line. */
    ceremony_text(CER_SCREEN_W_HALF, 34, ASSET_FONTS_BIGFONT, "CHAMPION",
                  ALIGN_MIDDLE_CENTER, 255, 224, 96);
    ceremony_text(CER_SCREEN_W_HALF, 56, ASSET_FONTS_SMALLFONT, "CONGRATULATIONS!",
                  ALIGN_MIDDLE_CENTER, 200, 200, 255);

    /* The champion: a prominent centred portrait + name + point total. The
     * winner's name is BIGFONT gold (unmistakable); the total is FUNFONT because
     * BIGFONT has no digit glyphs (the RESULTS rank/points reason). */
    if (sCer.champSeat != 0xFFu) {
        ceremony_seat_name(snap, haveSnap, sCer.champSeat, name, sizeof(name));
        ceremony_draw_portrait(sCer.champChar, CER_SCREEN_W_HALF - 22, 74,
                               255, 224, 96);
        (void) snprintf(line, sizeof(line), "%.12s%s", name,
                        sCer.champLocal ? " [YOU]" : "");
        ceremony_text(CER_SCREEN_W_HALF, 138, ASSET_FONTS_BIGFONT, line,
                      ALIGN_MIDDLE_CENTER, 255, 224, 96);
        (void) snprintf(line, sizeof(line), "CUP CHAMPION");
        ceremony_text(CER_SCREEN_W_HALF, 158, ASSET_FONTS_SMALLFONT, line,
                      ALIGN_MIDDLE_CENTER, 255, 255, 255);
        (void) snprintf(line, sizeof(line), "%u", (unsigned) sCer.champPoints);
        ceremony_text(CER_SCREEN_W_HALF, 176, ASSET_FONTS_FUNFONT, line,
                      ALIGN_MIDDLE_CENTER, 255, 224, 96);
    } else {
        ceremony_text(CER_SCREEN_W_HALF, 120, ASSET_FONTS_BIGFONT, "CUP COMPLETE",
                      ALIGN_MIDDLE_CENTER, 255, 224, 96);
    }

    /* Runners-up: the rest of the podium in small rows below (2ND, 3RD, ...),
     * reusing the real place labels -- so the standings are still legible under
     * the champion, not just the winner in isolation. */
    rowY = 194;
    for (i = 1u; i < sCer.st.count && i < 4u; i++) {
        unsigned slot = sCer.st.order[i];
        ceremony_seat_name(snap, haveSnap, slot, name, sizeof(name));
        (void) snprintf(line, sizeof(line), "%s  %.10s  %u",
                        (i < 8u) ? gRacePlacementsArray[i] : "-", name,
                        (unsigned) sCer.st.points[i]);
        ceremony_text(CER_SCREEN_W_HALF, rowY, ASSET_FONTS_SMALLFONT, line,
                      ALIGN_MIDDLE_CENTER, 200, 200, 200);
        rowY += 14;
    }

    /* Footer: a pulsed "returning" heartbeat (never a dead hold) + the host's
     * optional early-skip affordance. No countdown number, no required button. */
    ceremony_text(CER_SCREEN_W_HALF, 224, ASSET_FONTS_SMALLFONT,
                  "RETURNING TO ROOM...", ALIGN_MIDDLE_CENTER, 120, pg, 120);
    if (sCer.host) {
        ceremony_text(CER_SCREEN_W_HALF, 234, ASSET_FONTS_SMALLFONT, "A: CONTINUE",
                      ALIGN_MIDDLE_CENTER, 255, 255, 255);
    }
}

/* Bounded stderr witness: one line only when the visible state changes, so the
 * headless lane can read the drawn champion + countdown (mirrors results_witness). */
static void ceremony_witness(void) {
    u32 secs = ceremony_seconds_left();
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
void mdkr_online_ceremony_enter(void) {
    MdkrPartyLinkSnapshot snap;
    bool haveSnap;
    s32 localSeat;
    char name[32];

    memset(&sCer, 0, sizeof(sCer));
    sCer.champSeat = 0xFFu;
    sCer.champChar = 0xFFu;
    sWitnessKey = 0xFFFFFFFFu;

    /* Resolve the champion from the SAME snapshot + the SAME sort the RESULTS
     * STANDINGS ran, so the two screens can never disagree about who won. */
    haveSnap = mdkr_party_link_read(&snap);
    localSeat = haveSnap ? ceremony_local_seat(&snap) : -1;
    mdkr_online_standings_compute(&snap, haveSnap, &sCer.st);
    if (sCer.st.count > 0u) {
        sCer.champSeat = sCer.st.order[0];
        sCer.champPoints = sCer.st.points[0];
        if (haveSnap && sCer.champSeat < MDKR_PARTY_LINK_SEATS &&
            snap.seats[sCer.champSeat].character_id < CER_CHAR_COUNT) {
            sCer.champChar = snap.seats[sCer.champSeat].character_id;
        }
        sCer.champLocal = (localSeat >= 0 && (u8) localSeat == sCer.champSeat)
                              ? 1u
                              : 0u;
    }

    /* Borrow the real portraits + fonts (the charselect/results asset-borrow
     * discipline), set the shared online backdrop, and cue a crowd cheer. */
    menu_assetgroup_load(sPortraitAssetIds);
    menu_racer_portraits();
    load_font(ASSET_FONTS_BIGFONT);
    load_font(ASSET_FONTS_SMALLFONT);
    load_font(ASSET_FONTS_FUNFONT);
    sCer.assets = 1u;
    bgdraw_fillcolour(16, 24, 48); /* match the charselect/trackselect/results backdrop */
    sound_play(CER_SFX_CELEBRATE, NULL);

    /* Only resolve a name for a real champion seat; a feed-less endpoint (no
     * champion) logs "(none)" rather than the misleading "P256" a slot-255 name
     * fallback would print. Witness cosmetics only -- the render path already
     * guards the no-champion case ("CUP COMPLETE"). */
    if (sCer.champSeat != 0xFFu) {
        ceremony_seat_name(&snap, haveSnap, sCer.champSeat, name, sizeof(name));
    } else {
        (void) snprintf(name, sizeof(name), "(none)");
    }
    fprintf(stderr,
            "[online-ceremony] enter: champion seat=%u name=%.12s points=%u "
            "seats=%u (native cup celebration; offline trophy cinematic "
            "bypassed)\n",
            (unsigned) sCer.champSeat, name, (unsigned) sCer.champPoints,
            (unsigned) sCer.st.count);
}

void mdkr_online_ceremony_exit(void) {
    if (sCer.assets) {
        unload_font(ASSET_FONTS_FUNFONT);
        unload_font(ASSET_FONTS_SMALLFONT);
        unload_font(ASSET_FONTS_BIGFONT);
        menu_assetgroup_free(sPortraitAssetIds);
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

    haveSnap = mdkr_party_link_read(&snap);
    localSeat = haveSnap ? ceremony_local_seat(&snap) : -1;
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
         * gone" predicate (mirroring the T6d pre-START vacate seam,
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

u8 mdkr_online_ceremony_test_active(void) {
    return ceremony_skip_active();
}

#endif /* MDKR_ENABLE_ONLINE_BETA */
