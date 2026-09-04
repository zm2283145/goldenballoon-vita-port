/* NON-BLOCKING online race pause overlay -- see online_race_pause.h for the
 * crash mechanism this replaces and the isolation contract.
 *
 * Rules this TU lives by:
 *   - PRESENTATION ONLY. No state here is registered rollback authority, no
 *     sim global is written, no gameplay event is traced, and no sound is
 *     played (a UI sound emitted during the authored pass would enter the
 *     side-effect journal and be cancelled/restarted by every correction that
 *     rewinds across it -- the overlay stays silent by design).
 *   - LOCAL ONLY. Open/navigate keys off the LOCAL seat's canonical pad edges
 *     (the local player's own sealed input, ~inputDelay ticks behind the
 *     physical press). The remote seat's START is deliberately ignored: each
 *     machine owns its own overlay, the peer sees nothing.
 *   - The kart keeps driving. Nothing is captured away from the sim; A/B and
 *     the stick still steer while the menu is up (the MK8 model). Navigation
 *     therefore uses edges the racer does not consume for driving (D-pad
 *     up/down, A/B edges, START).
 */
#include "online/online_race_pause.h"

#if MDKR_ENABLE_ONLINE_BETA

/* Game/PR headers FIRST, system <stdio.h> LAST: online_screen_util.h pulls
 * rcp_dkr.h -> ultra64.h -> PR/os_libc.h, which declares sprintf as a plain
 * function; the system <stdio.h> must follow it (otherwise sprintf is already
 * a fortify macro and os_libc's declaration fails) -- the exact ordering every
 * online screen TU relies on. */
#include "PR/os_cont.h"     /* START_BUTTON / A_BUTTON / B_BUTTON / U_JPAD / D_JPAD */
#include "joypad.h"         /* input_pressed */
#include "thread3_main.h"   /* GameMode enum + is_postrace_viewport_active */
#include "net/match_input_runtime.h" /* the online canonical-input runtime gate */
#include "net/net_roster_runtime.h"  /* local seat -> canonical slot (= pad port) */
#include "online/online_session.h"   /* the clean LEAVE return path */
#include "online/online_screen_util.h" /* panel + text draw vocabulary */
#include "online/online_screen_constants.h" /* MDKR_ONLINE_SCREEN_W_HALF */
#include "asset_enums.h"    /* ASSET_FONTS_SMALLFONT */
#include "enums.h"          /* ALIGN_* */

#include <stdio.h>
#include <stdlib.h>

/* The engine's live game-mode selector (thread3_main.c, external linkage; the
 * same declaration online_session.c uses). */
extern s32 gGameMode;
/* The presentation scheduler's simulation-tick ordinal (present_sched.c) --
 * witness-line observability only, never authority. */
extern int g_simTickCounter;

enum {
    MDKR_ONLINE_PAUSE_ROW_CONTINUE = 0,
    MDKR_ONLINE_PAUSE_ROW_LEAVE = 1
};

static struct {
    u8 open;
    u8 row;
} sOnlineRacePause;

s32 mdkr_online_race_pause_suppressed(void) {
    /* TEST-ONLY (belt lane): keep the LEGACY retail engage reachable so the
     * registered lane can forever pin the rollback belt -- the recoverable
     * routing of a correction replay the paused sim refuses
     * (rollback_game_runtime.c reconcile_network_inputs) -- through the REAL
     * mechanism rather than a synthetic refusal. Read once; inert unless the
     * env is set, and never set outside the lane. */
    static int sAllowRetailForTest = -1;
    if (sAllowRetailForTest < 0) {
        const char *env = getenv("MDKR_APP_TEST_ONLINE_ALLOW_RETAIL_PAUSE");
        sAllowRetailForTest =
            (env != NULL && env[0] != '\0' && env[0] != '0') ? 1 : 0;
        if (sAllowRetailForTest) {
            fprintf(stderr,
                    "[online-pause] TEST: retail pause engage ALLOWED "
                    "(belt lane)\n");
        }
    }
    if (sAllowRetailForTest) {
        return 0;
    }
    /* Keyed ONLY on the online canonical-input runtime: constant for the whole
     * race, identical on every endpoint and identical live vs. resimulated, so
     * the suppression is deterministic sim behaviour (both machines skip the
     * retail pause for the same canonical START edge). False in every offline
     * mode of the beta build, so local Play pauses exactly as retail. */
    return mdkr_match_input_runtime_active() ? 1 : 0;
}

static void online_race_pause_reset(void) {
    sOnlineRacePause.open = 0u;
    sOnlineRacePause.row = MDKR_ONLINE_PAUSE_ROW_CONTINUE;
}

static void online_race_pause_draw(void) {
    const s32 cx = MDKR_ONLINE_SCREEN_W_HALF;
    /* The online screens' panel style: dark board, white body text, the
     * selected row in the screens' selection gold. Small and centred so the
     * still-running race stays readable around it. */
    mdkr_online_screen_panel(cx - 70, 74, cx + 70, 166);
    mdkr_online_screen_text(cx, 88, ASSET_FONTS_SMALLFONT, "ONLINE RACE",
                            ALIGN_MIDDLE_CENTER, 255, 255, 255);
    mdkr_online_screen_text(cx, 102, ASSET_FONTS_SMALLFONT,
                            "THE RACE KEEPS GOING", ALIGN_MIDDLE_CENTER,
                            180, 180, 180);
    if (sOnlineRacePause.row == MDKR_ONLINE_PAUSE_ROW_CONTINUE) {
        mdkr_online_screen_text(cx, 122, ASSET_FONTS_SMALLFONT, "CONTINUE",
                                ALIGN_MIDDLE_CENTER, 255, 200, 60);
        mdkr_online_screen_text(cx, 138, ASSET_FONTS_SMALLFONT, "LEAVE RACE",
                                ALIGN_MIDDLE_CENTER, 255, 255, 255);
    } else {
        mdkr_online_screen_text(cx, 122, ASSET_FONTS_SMALLFONT, "CONTINUE",
                                ALIGN_MIDDLE_CENTER, 255, 255, 255);
        mdkr_online_screen_text(cx, 138, ASSET_FONTS_SMALLFONT, "LEAVE RACE",
                                ALIGN_MIDDLE_CENTER, 255, 200, 60);
    }
    mdkr_online_screen_text(cx, 156, ASSET_FONTS_SMALLFONT,
                            "A: SELECT   START: BACK", ALIGN_MIDDLE_CENTER,
                            180, 180, 180);
}

void mdkr_online_race_overlay_frame(s32 updateRate) {
    u8 localSlot = 0u;
    u32 pressed;
    (void)updateRate;
    /* Only a LIVE online rollback race, in its racing shape. Any other state
     * (offline, online session screens, post-race viewport, a race that just
     * ended) closes and resets the overlay. */
    if (!mdkr_match_input_runtime_active() || gGameMode != GAMEMODE_INGAME ||
        is_postrace_viewport_active()) {
        online_race_pause_reset();
        return;
    }
    if (!mdkr_net_roster_runtime_local_to_canonical(0u, &localSlot)) {
        online_race_pause_reset();
        return;
    }
    /* The LOCAL seat's canonical edges (the sealed copy of the local player's
     * own pad). Never the remote seat: the peer's START must not open this
     * machine's overlay. */
    pressed = input_pressed((s32)localSlot);

    if (!sOnlineRacePause.open) {
        if (pressed & START_BUTTON) {
            sOnlineRacePause.open = 1u;
            sOnlineRacePause.row = MDKR_ONLINE_PAUSE_ROW_CONTINUE;
            fprintf(stderr,
                    "[online-pause] overlay OPEN tick=%d (sim keeps running)\n",
                    g_simTickCounter);
        }
        return;
    }

    /* Open: navigate / act. START or B backs out; the D-pad flips between the
     * two rows; an A EDGE confirms the highlighted row. */
    if (pressed & (START_BUTTON | B_BUTTON)) {
        fprintf(stderr, "[online-pause] overlay CLOSED tick=%d (resume)\n",
                g_simTickCounter);
        online_race_pause_reset();
        return;
    }
    if (pressed & (U_JPAD | D_JPAD)) {
        sOnlineRacePause.row =
            sOnlineRacePause.row == MDKR_ONLINE_PAUSE_ROW_CONTINUE
                ? MDKR_ONLINE_PAUSE_ROW_LEAVE
                : MDKR_ONLINE_PAUSE_ROW_CONTINUE;
    }
    if (pressed & A_BUTTON) {
        if (sOnlineRacePause.row == MDKR_ONLINE_PAUSE_ROW_LEAVE) {
            fprintf(stderr,
                    "[online-pause] LEAVE RACE selected tick=%d -> clean LEFT "
                    "return\n",
                    g_simTickCounter);
            online_race_pause_reset();
            mdkr_online_session_leave_race();
            return;
        }
        fprintf(stderr, "[online-pause] overlay CLOSED tick=%d (resume)\n",
                g_simTickCounter);
        online_race_pause_reset();
        return;
    }
    online_race_pause_draw();
}

#endif /* MDKR_ENABLE_ONLINE_BETA */
