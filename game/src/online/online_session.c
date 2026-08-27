/* SEPARATED-BOOT-PATH (Strategy D) online session -- PD-T1 skeleton + fork.
 *
 * The whole translation unit is #if MDKR_ENABLE_ONLINE_BETA and the file is
 * added to the build ONLY inside the beta CMake gate (game/src/online/ is NOT
 * auto-globbed), so a normal (beta OFF) build never compiles a byte of it and
 * the release engine object is untouched. See game/src/online/online_session.h
 * for the architecture rationale.
 *
 * PD-T1 implements ONLY: the session-owned state, LOBBY_WAIT (read the
 * party_link forward feed and wait until the room leaves selection), and the
 * RACE hand-off (call the EXISTING mdkr_online_boot_direct_race, which sets
 * GAMEMODE_INGAME -- no race-setup logic is duplicated here; extraction is
 * PD-T4). The remaining phases (CHARSELECT / TRACKSELECT / RESULTS / CEREMONY)
 * are placeholders PD-T2..T6 fill in.
 */
#include "online/online_session.h"

#if MDKR_ENABLE_ONLINE_BETA

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "types.h"
#include "thread3_main.h"
#include "net/party_link.h"
#include "online/online_charselect.h" /* PD-T2 native CHARSELECT phase */

/* The engine's live game-mode selector. Defined (external linkage) in
 * thread3_main.c; no shared header declares it, so the session declares the
 * exact global here to drive the mode transitions on the online path. */
extern s32 gGameMode;

/* The offline current-menu id (menu.c). It stays at its power-on default 0
 * until load_menu_with_level_background() loads a menu -- the offline boot loads
 * MENU_BOOT first. On the online path that call never runs, so reading 0 here is
 * concrete evidence the offline menu state machine was never entered. Used only
 * for the isolation witness below. */
extern s32 gCurrentMenuId;

/* The party_link snapshot carries the launcher lobby MdkrOnlinePhase as a byte
 * (platform/net/party_link.h). The room is still in selection while the phase is
 * MDKR_ONLINE_LOBBY; the host's START_RACE advances it to MDKR_ONLINE_LOADING.
 * These mirror platform/online/lobby_core.h MdkrOnlinePhase (LOBBY=1,
 * LOADING=2); kept as local constants so this engine TU does not pull the
 * launcher lobby headers in (party_link.h is deliberately dependency-free). */
#define MDKR_ONLINE_SESSION_LOBBY_PHASE 1u   /* MDKR_ONLINE_LOBBY */
#define MDKR_ONLINE_SESSION_LOADING_PHASE 2u /* MDKR_ONLINE_LOADING */

typedef struct MdkrOnlineSessionState {
    MdkrOnlineSessionPhase phase;
    const MdkrMatchLaunchDescriptorV1 *launch;
    u32 lobbyWaitTicks;
    u8 active;
} MdkrOnlineSessionState;

/* Session-owned state -- deliberately NOT any offline global. */
static MdkrOnlineSessionState sOnlineSession;

/* Warn-once latch for the CHARSELECT leave stub (PD-T6): the engine->launcher
 * return handshake is not wired yet, so a browse-B is logged exactly once, not
 * per-frame. Reset when CHARSELECT is entered. */
static u8 sCharselectLeaveWarned;

/* ---- Headless test seam (beta + env gated; inert in normal runs) ----------
 *
 * Ordinary runs never set MDKR_TEST_ONLINE_SESSION_SCRIPT, so this is dormant.
 * When it IS set (a headless gate), the session installs the REAL party_link
 * bridge and publishes a scripted forward-feed snapshot each LOBBY_WAIT tick:
 * MDKR_ONLINE_LOBBY for the first <hold> ticks, then MDKR_ONLINE_LOADING. That
 * lets the session genuinely exercise mdkr_party_link_read() and its
 * phase-leaves-lobby gate before booting -- the same publish-then-read pattern
 * OnlineRoom_runTestPartyLinkFake uses. Beta-only by construction. */
static u8 sTestScriptInstalled;
static u32 sTestHoldTicks;

static void online_session_test_maybe_script(void) {
    const char *env = getenv("MDKR_TEST_ONLINE_SESSION_SCRIPT");
    MdkrPartyLinkSnapshot snap;

    if (env == NULL) {
        return;
    }
    if (!sTestScriptInstalled) {
        sTestHoldTicks = (u32) strtoul(env, NULL, 10);
        mdkr_party_link_clear();
        (void) mdkr_party_link_install();
        sTestScriptInstalled = 1;
        fprintf(stderr, "[online-session] test-script install hold=%u\n",
                sTestHoldTicks);
    }
    memset(&snap, 0, sizeof(snap));
    snap.generation = sOnlineSession.lobbyWaitTicks + 1u;
    snap.phase = (sOnlineSession.lobbyWaitTicks < sTestHoldTicks)
                     ? (uint8_t) MDKR_ONLINE_SESSION_LOBBY_PHASE
                     : (uint8_t) MDKR_ONLINE_SESSION_LOADING_PHASE;
    mdkr_party_link_publish(&snap);
}

static void online_session_test_script_teardown(void) {
    if (sTestScriptInstalled) {
        mdkr_party_link_clear();
        sTestScriptInstalled = 0;
    }
}

/* ---- Session ------------------------------------------------------------- */

static void online_session_boot_race(void) {
    sOnlineSession.phase = MDKR_ONLINE_SESSION_RACE;
    /* Isolation witness: on the online path control reached the session
     * (gGameMode == GAMEMODE_ONLINE_SESSION) and the offline boot menu was
     * never loaded (gCurrentMenuId still 0, i.e. not MENU_BOOT). */
    fprintf(stderr,
            "[online-session] phase=RACE booting after %u LOBBY_WAIT tick(s); "
            "isolation gGameMode=%d gCurrentMenuId=%d (0 == offline MENU_BOOT "
            "never loaded)\n",
            sOnlineSession.lobbyWaitTicks, gGameMode, gCurrentMenuId);
    online_session_test_script_teardown();
    sOnlineSession.active = 0;
    /* Reuse the game's own race boot; it sets gGameMode = GAMEMODE_INGAME. No
     * race-setup logic is duplicated (PD-T4 owns any extraction). */
    mdkr_online_boot_direct_race(sOnlineSession.launch);
}

void mdkr_online_session_begin(const MdkrMatchLaunchDescriptorV1 *launch) {
    memset(&sOnlineSession, 0, sizeof(sOnlineSession));
    sOnlineSession.phase = MDKR_ONLINE_SESSION_LOBBY_WAIT;
    sOnlineSession.launch = launch;
    sOnlineSession.active = 1;
    /* Enter the SEPARATED mode. Offline code never produces this value, so the
     * offline menu state machine is never entered on this route. */
    gGameMode = GAMEMODE_ONLINE_SESSION;
    fprintf(stderr,
            "[online-session] begin: separated boot path entered "
            "(gGameMode=%d phase=LOBBY_WAIT); offline menu state machine "
            "bypassed\n",
            gGameMode);
}

/* True when the snapshot seats a resolvable LOCAL player -- the signal that the
 * room is a live selection room this endpoint is actually in, so the native
 * CHARSELECT screen (PD-T2) should be shown rather than idling. The PD-T1
 * session-boot lane publishes an empty room (no occupied seats), so it never
 * trips this and keeps its LOBBY_WAIT-then-boot behaviour unchanged. */
static bool online_session_snapshot_has_local_seat(
    const MdkrPartyLinkSnapshot *snap) {
    unsigned i;
    for (i = 0u; i < MDKR_PARTY_LINK_SEATS; i++) {
        if (snap->seats[i].occupied && snap->seats[i].is_local) {
            return true;
        }
    }
    return false;
}

void mdkr_online_session_tick(s32 updateRate) {
    (void) updateRate;

    if (!sOnlineSession.active) {
        /* Only reachable if something set GAMEMODE_ONLINE_SESSION without a
         * begin() -- which offline code never does. Fail safe to intro. */
        fprintf(stderr,
                "[online-session] WARNING: tick with no active session; "
                "returning to intro\n");
        gGameMode = GAMEMODE_INTRO;
        return;
    }

    switch (sOnlineSession.phase) {
    case MDKR_ONLINE_SESSION_LOBBY_WAIT: {
        MdkrPartyLinkSnapshot snap;
        bool haveSnap;
        bool readyToBoot = false;
        bool toCharselect = false;

        /* Dormant unless a headless test seam is enabled. */
        online_session_test_maybe_script();
        /* PD-T2: inert unless the CHARSELECT headless seam is armed; then it
         * installs the forward feed and publishes a scripted LOBBY room. */
        mdkr_online_charselect_test_lobby_pump();

        haveSnap = mdkr_party_link_read(&snap);
        if (!haveSnap) {
            /* No live forward feed installed (legacy direct-boot / the launcher
             * is not yet publishing): the validated launch descriptor IS the
             * room's agreement to start, so boot now. This keeps
             * check_online_engine_boot_direct green. */
            readyToBoot = true;
        } else if (snap.phase == (uint8_t) MDKR_ONLINE_SESSION_LOBBY_PHASE) {
            /* Room is still selecting. Show the native CHARSELECT once a local
             * seat is resolvable; otherwise keep idling (the PD-T1 session-boot
             * lane publishes an empty room and boots when the phase leaves
             * LOBBY). */
            toCharselect = online_session_snapshot_has_local_seat(&snap);
        } else {
            /* The room has left selection (host started / loading): boot. */
            readyToBoot = true;
        }

        fprintf(stderr,
                "[online-session] phase=LOBBY_WAIT tick=%u haveSnap=%d "
                "snapPhase=%u ready=%d\n",
                sOnlineSession.lobbyWaitTicks, (int) haveSnap,
                (unsigned) (haveSnap ? snap.phase : 0u), (int) readyToBoot);
        sOnlineSession.lobbyWaitTicks++;

        if (toCharselect) {
            sOnlineSession.phase = MDKR_ONLINE_SESSION_CHARSELECT;
            sCharselectLeaveWarned = 0u;
            mdkr_online_charselect_enter();
        } else if (readyToBoot) {
            online_session_boot_race();
        }
        break;
    }
    case MDKR_ONLINE_SESSION_CHARSELECT: {
        MdkrOnlineCharselectResult r = mdkr_online_charselect_tick(updateRate);
        if (r == MDKR_ONLINE_CHARSELECT_ADVANCE) {
            /* PD-T3 will route to TRACKSELECT; for now hand straight to the race
             * so the separated flow still completes end-to-end. */
            mdkr_online_charselect_exit();
            online_session_boot_race();
        } else if (r == MDKR_ONLINE_CHARSELECT_LEAVE) {
            /* Backing all the way out to the launcher room requires the
             * engine->launcher return handshake that is PD-T6 (the same wiring
             * that boots this session at LOBBY phase in the first place). For now
             * this is a documented stub: log it ONCE and remain on the screen
             * rather than half-tear-down into an unwired state. The tick returns
             * LEAVE as an edge (ADVANCE always wins), so this can never wedge the
             * session -- a host-start still boots this endpoint. */
            if (!sCharselectLeaveWarned) {
                sCharselectLeaveWarned = 1u;
                fprintf(stderr,
                        "[online-charselect] leave requested; engine->launcher "
                        "return is PD-T6, staying on screen\n");
            }
        }
        break;
    }
    case MDKR_ONLINE_SESSION_RACE:
        /* The boot normally fires inline from LOBBY_WAIT / CHARSELECT; boot here
         * too if the mode is somehow re-entered before the hand-off completed. */
        online_session_boot_race();
        break;
    default:
        /* TRACKSELECT / RESULTS / CEREMONY arrive in PD-T3..T6. */
        break;
    }
}

#endif /* MDKR_ENABLE_ONLINE_BETA */
