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

#elif defined(__vita__) && defined(MDKR_ENABLE_VITA_LAN)

#include "PR/os_cont.h"
#include "asset_enums.h"
#include "enums.h"
#include "font.h"
#include "game.h"
#include "joypad.h"
#include "macros.h"
#include "rcp_dkr.h"
#include "thread3_main.h"
#include "net/match_input_runtime.h"
#include "net/net_roster_runtime.h"
#include "vita_lan.h"

#include <stdio.h>

extern s32 gGameMode;
extern Gfx *gCurrDisplayList;
extern int g_simTickCounter;

enum {
    VITA_PAUSE_CONTINUE = 0,
    VITA_PAUSE_RESTART,
    VITA_PAUSE_SELECT_TRACK,
    VITA_PAUSE_QUIT
};

static const u8 sVitaLanTrackIds[] = {
    5u, 3u, 29u, 7u, 13u, 6u, 9u, 28u, 8u, 4u,
    10u, 30u, 19u, 18u, 20u, 31u, 17u, 32u, 33u, 15u
};

static struct {
    u8 open;
    u8 row;
    u8 track_picker;
    u8 track_index;
    u8 dialogue_open;
    u8 suppress_until_release;
    u16 raw_buttons;
    u16 raw_pressed;
    u16 blink;
} sVitaRacePause;

s32 mdkr_online_race_pause_suppressed(void) {
    return mdkr_match_input_runtime_active() ? 1 : 0;
}

static void vita_pause_dialogue_begin(s32 optionCount, s32 halfWidth) {
    s32 halfHeight = ((optionCount * 16) + 28) >> 1;
    clear_dialogue_box_open_flag(7);
    dialogue_clear(7);
    set_current_dialogue_box_coords(
        7, 160 - halfWidth, 120 - halfHeight,
        160 + halfWidth, 120 + halfHeight);
    set_current_dialogue_background_colour(7, 64, 64, 255, 160);
    set_dialogue_font(7, ASSET_FONTS_FUNFONT);
    set_current_text_background_colour(7, 128, 128, 255, 0);
    set_current_text_colour(7, 0, 255, 0, 128, 255);
    sVitaRacePause.dialogue_open = 1;
}

static void vita_pause_dialogue_row(
    s32 y, const char *text, int selected) {
    s32 alpha = (sVitaRacePause.blink & 0x3F) * 8;
    if (alpha > 255) {
        alpha = 511 - alpha;
    }
    set_current_text_colour(7, 255, 255, 255,
                            selected ? alpha : 0, 255);
    render_dialogue_text(7, POS_CENTRED, y, (char *)text, 1,
                         ALIGN_MIDDLE_CENTER);
}

static unsigned int vita_track_index(unsigned int trackId) {
    unsigned int index;
    for (index = 0; index < ARRAY_COUNT(sVitaLanTrackIds); index++) {
        if (sVitaLanTrackIds[index] == trackId) {
            return index;
        }
    }
    return 0;
}

static unsigned int vita_track_vehicle_mask(unsigned int trackId) {
    unsigned int mask =
        (unsigned int)leveltable_vehicle_usable((s32)trackId) & 0x07u;
    if (trackId == 15u) {
        mask &= ~(1u << VEHICLE_HOVERCRAFT);
    } else if (trackId == 28u) {
        mask &= ~(1u << VEHICLE_PLANE);
    }
    return mask;
}

static void vita_pause_draw_root(int host) {
    vita_pause_dialogue_begin(host ? 4 : 2, 105);
    set_current_text_colour(7, 0, 255, 0, 128, 255);
    render_dialogue_text(7, POS_CENTRED, 12, "PAUSE OPTIONS", 1,
                         ALIGN_MIDDLE_CENTER);
    vita_pause_dialogue_row(
        32, "CONTINUE", sVitaRacePause.row == VITA_PAUSE_CONTINUE);
    if (host) {
        vita_pause_dialogue_row(
            48, "RESTART RACE",
            sVitaRacePause.row == VITA_PAUSE_RESTART);
        vita_pause_dialogue_row(
            64, "SELECT TRACK",
            sVitaRacePause.row == VITA_PAUSE_SELECT_TRACK);
        vita_pause_dialogue_row(
            80, "QUIT GAME", sVitaRacePause.row == VITA_PAUSE_QUIT);
    } else {
        vita_pause_dialogue_row(
            48, "QUIT GAME", sVitaRacePause.row == VITA_PAUSE_QUIT);
    }
    open_dialogue_box(7);
}

static void vita_pause_draw_track(unsigned int trackId, int host) {
    char *name = level_name((s32)trackId);
    vita_pause_dialogue_begin(host ? 3 : 2, 125);
    set_current_text_colour(7, 0, 255, 0, 128, 255);
    render_dialogue_text(7, POS_CENTRED, 12, "SELECT TRACK", 1,
                         ALIGN_MIDDLE_CENTER);
    vita_pause_dialogue_row(
        36, name != NULL ? name : "UNKNOWN TRACK", host);
    if (host) {
        vita_pause_dialogue_row(56, "A: START   B: BACK", 0);
    } else {
        vita_pause_dialogue_row(52, "HOST SELECTING", 0);
    }
    open_dialogue_box(7);
}

static void vita_pause_reset(void) {
    if (sVitaRacePause.open || sVitaRacePause.track_picker) {
        sVitaRacePause.suppress_until_release = 1;
    }
    if (sVitaRacePause.track_picker && mdkr_vita_lan_is_host()) {
        mdkr_vita_lan_set_track_picker(0, 0);
    }
    sVitaRacePause.open = 0;
    sVitaRacePause.row = VITA_PAUSE_CONTINUE;
    sVitaRacePause.track_picker = 0;
    if (sVitaRacePause.dialogue_open) {
        dialogue_close(7);
        dialogue_clear(7);
        sVitaRacePause.dialogue_open = 0;
    }
}

void mdkr_online_race_pause_close(void) {
    vita_pause_reset();
}

void mdkr_online_race_pause_filter_local_input(MdkrPadSample *input) {
    unsigned int remoteTrack;
    int capture;
    if (input == NULL) {
        return;
    }
    sVitaRacePause.raw_pressed =
        input->buttons & (u16)~sVitaRacePause.raw_buttons;
    sVitaRacePause.raw_buttons = input->buttons;
    if (sVitaRacePause.suppress_until_release &&
        input->buttons == 0 && input->stick_x == 0 &&
        input->stick_y == 0) {
        sVitaRacePause.suppress_until_release = 0;
    }
    capture = sVitaRacePause.open ||
              sVitaRacePause.suppress_until_release ||
              (!mdkr_vita_lan_is_host() &&
               mdkr_vita_lan_track_picker(&remoteTrack));
    if (capture) {
        input->buttons = 0;
        input->stick_x = 0;
        input->stick_y = 0;
        input->present = 1;
    }
}

void mdkr_online_race_overlay_frame(s32 updateRate) {
    u32 pressed;
    unsigned int remoteTrack;
    int host;
    unsigned int rowCount;
    (void)updateRate;
    sVitaRacePause.blink =
        (u16)(sVitaRacePause.blink + (updateRate > 0 ? updateRate : 1));

    if (!mdkr_match_input_runtime_active() ||
        gGameMode != GAMEMODE_INGAME ||
        is_postrace_viewport_active()) {
        vita_pause_reset();
        return;
    }
    host = mdkr_vita_lan_is_host();
    pressed = sVitaRacePause.raw_pressed;
    sVitaRacePause.raw_pressed = 0;

    if (!host && mdkr_vita_lan_track_picker(&remoteTrack) &&
        !sVitaRacePause.open) {
        vita_pause_draw_track(remoteTrack, 0);
        if (pressed & START_BUTTON) {
            sVitaRacePause.open = 1;
            sVitaRacePause.row = VITA_PAUSE_CONTINUE;
        }
        return;
    }

    if (!sVitaRacePause.open) {
        if (pressed & START_BUTTON) {
            sVitaRacePause.open = 1;
            sVitaRacePause.row = VITA_PAUSE_CONTINUE;
        }
        return;
    }

    if (sVitaRacePause.track_picker) {
        if (pressed & (START_BUTTON | B_BUTTON)) {
            mdkr_vita_lan_set_track_picker(0, 0);
            sVitaRacePause.track_picker = 0;
            sVitaRacePause.row = VITA_PAUSE_SELECT_TRACK;
        } else {
            if (pressed & (L_JPAD | U_JPAD)) {
                sVitaRacePause.track_index =
                    sVitaRacePause.track_index == 0
                        ? ARRAY_COUNT(sVitaLanTrackIds) - 1
                        : sVitaRacePause.track_index - 1;
                mdkr_vita_lan_set_track_picker(
                    1, sVitaLanTrackIds[sVitaRacePause.track_index]);
            } else if (pressed & (R_JPAD | D_JPAD)) {
                sVitaRacePause.track_index =
                    (sVitaRacePause.track_index + 1) %
                    ARRAY_COUNT(sVitaLanTrackIds);
                mdkr_vita_lan_set_track_picker(
                    1, sVitaLanTrackIds[sVitaRacePause.track_index]);
            }
            if (pressed & A_BUTTON) {
                unsigned int trackId =
                    sVitaLanTrackIds[sVitaRacePause.track_index];
                unsigned int vehicleMask =
                    vita_track_vehicle_mask(trackId);
                if (mdkr_vita_lan_request_control(
                        MDKR_VITA_LAN_CONTROL_SELECT_TRACK,
                        trackId, vehicleMask)) {
                    vita_pause_reset();
                    return;
                }
            }
        }
        vita_pause_draw_track(
            sVitaLanTrackIds[sVitaRacePause.track_index], 1);
        return;
    }

    if (pressed & (START_BUTTON | B_BUTTON)) {
        vita_pause_reset();
        return;
    }
    rowCount = host ? 4u : 2u;
    if (!host && (pressed & (U_JPAD | D_JPAD))) {
        sVitaRacePause.row =
            sVitaRacePause.row == VITA_PAUSE_CONTINUE
                ? VITA_PAUSE_QUIT
                : VITA_PAUSE_CONTINUE;
    } else if (host && (pressed & U_JPAD)) {
        sVitaRacePause.row =
            sVitaRacePause.row == 0 ? (u8)(rowCount - 1)
                                    : sVitaRacePause.row - 1;
    } else if (host && (pressed & D_JPAD)) {
        sVitaRacePause.row =
            (u8)((sVitaRacePause.row + 1) % rowCount);
    }
    if (pressed & A_BUTTON) {
        if (sVitaRacePause.row == VITA_PAUSE_CONTINUE) {
            vita_pause_reset();
            return;
        }
        if (host && sVitaRacePause.row == VITA_PAUSE_RESTART) {
            if (mdkr_vita_lan_request_control(
                    MDKR_VITA_LAN_CONTROL_RESTART, 0, 0)) {
                vita_pause_reset();
                return;
            }
        } else if (host &&
                   sVitaRacePause.row == VITA_PAUSE_SELECT_TRACK) {
            unsigned int currentTrack = 0;
            const MdkrMatchLaunchDescriptorV1 *launch =
                mdkr_net_roster_runtime_launch_descriptor();
            if (launch != NULL) {
                currentTrack = launch->manifest.track_id;
            }
            sVitaRacePause.track_index =
                (u8)vita_track_index(currentTrack);
            sVitaRacePause.track_picker = 1;
            mdkr_vita_lan_set_track_picker(
                1, sVitaLanTrackIds[sVitaRacePause.track_index]);
        } else if (sVitaRacePause.row == VITA_PAUSE_QUIT) {
            if (mdkr_vita_lan_request_control(
                    MDKR_VITA_LAN_CONTROL_QUIT, 0, 0)) {
                vita_pause_reset();
                return;
            }
        }
    }
    vita_pause_draw_root(host);
}

#endif /* online beta / Vita LAN */
