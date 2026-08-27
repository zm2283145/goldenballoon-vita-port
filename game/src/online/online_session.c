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
#include "online/online_charselect.h"  /* PD-T2 native CHARSELECT phase */
#include "online/online_trackselect.h" /* PD-T3 native TRACKSELECT phase +
                                          mdkr_online_trackselect_cup_track */
#include "online/online_race_boot.h"   /* PD-T4 direct race boot (extracted) */

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

/* "No track resolved yet" sentinel for intendedTrack below -- matches the
 * forward-feed / lobby "none" width (configured_track 0xFFFF). */
#define MDKR_ONLINE_SESSION_TRACK_NONE 0xFFFFu

typedef struct MdkrOnlineSessionState {
    MdkrOnlineSessionPhase phase;
    const MdkrMatchLaunchDescriptorV1 *launch;
    u32 lobbyWaitTicks;
    /* PD-T4: the last-seen resolved host-INTENDED race track from the forward
     * feed (single race -> configured_track; tournament -> the cup's scheduled
     * round track), or MDKR_ONLINE_SESSION_TRACK_NONE. This is an OBSERVABILITY
     * value ONLY -- the boot always loads launch->manifest.track_id (the peer
     * rollback-admission authority); intendedTrack is logged against it so "the
     * host's locked track is what loads" is explicit and testable. */
    u16 intendedTrack;
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
    /* PD-T4: publish the config "none" sentinels, not the bare memset zeros. This
     * lane scripts an EMPTY room with no host pick -- it boots the vote-derived
     * manifest -- so the session's intended-track resolution must read NONE. A
     * plain memset(0) would read as mode=SINGLE + configured_track=0 == "intended
     * track 0" (garbage) and log a spurious divergence; the sentinels give the
     * silent "track honored" path this lane expects. (mode stays 0 == SINGLE.) */
    snap.configured_track = MDKR_PARTY_LINK_TRACK_UNSET; /* 0xFFFF == none */
    snap.cup_id = MDKR_PARTY_LINK_CUP_UNSET;             /* 0xFF == none */
    mdkr_party_link_publish(&snap);
}

static void online_session_test_script_teardown(void) {
    if (sTestScriptInstalled) {
        mdkr_party_link_clear();
        sTestScriptInstalled = 0;
    }
}

/* ---- Session ------------------------------------------------------------- */

/* PD-T4: resolve the host-intended race track from a forward-feed snapshot,
 * mirroring the launcher reducer's own resolution: tournament -> the cup's
 * scheduled round track (mdkr_online_cup_track / kCupTracks, re-exposed engine-
 * side as mdkr_online_trackselect_cup_track over the same accepted set); single
 * race -> configured_track. Returns MDKR_ONLINE_SESSION_TRACK_NONE when nothing
 * is configured yet. Pure observation -- never drives the boot. */
static u16 online_session_resolve_intended(const MdkrPartyLinkSnapshot *snap) {
    if (snap->mode == MDKR_PARTY_LINK_MODE_TOURNAMENT &&
        snap->cup_id != MDKR_PARTY_LINK_CUP_UNSET) {
        u16 track =
            mdkr_online_trackselect_cup_track(snap->cup_id, snap->race_index);
        return (track != 0u) ? track : MDKR_ONLINE_SESSION_TRACK_NONE;
    }
    if (snap->configured_track != MDKR_PARTY_LINK_TRACK_UNSET) {
        return snap->configured_track;
    }
    return MDKR_ONLINE_SESSION_TRACK_NONE;
}

/* Stash the last-seen resolved host-intended track (only when one is actually
 * configured, so a transient "none" -- e.g. the reducer clearing the pick on a
 * mode toggle before the host re-locks -- never clobbers a real pick right before
 * boot). Observability only; the boot still loads launch->manifest.track_id. */
static void online_session_stash_intended(const MdkrPartyLinkSnapshot *snap) {
    u16 intended = online_session_resolve_intended(snap);
    if (intended != MDKR_ONLINE_SESSION_TRACK_NONE) {
        sOnlineSession.intendedTrack = intended;
    }
}

static void online_session_boot_race(void) {
    u16 intended = sOnlineSession.intendedTrack;
    s32 manifestTrack = (s32) sOnlineSession.launch->manifest.track_id;

    sOnlineSession.phase = MDKR_ONLINE_SESSION_RACE;
    /* Isolation witness: on the online path control reached the session
     * (gGameMode == GAMEMODE_ONLINE_SESSION) and the offline boot menu was
     * never loaded (gCurrentMenuId still 0, i.e. not MENU_BOOT). */
    fprintf(stderr,
            "[online-session] phase=RACE booting after %u LOBBY_WAIT tick(s); "
            "isolation gGameMode=%d gCurrentMenuId=%d (0 == offline MENU_BOOT "
            "never loaded)\n",
            sOnlineSession.lobbyWaitTicks, gGameMode, gCurrentMenuId);

    /* PD-T4 observable agreement check. The race ALWAYS boots the manifest's
     * track (the peer rollback-admission authority: the launcher froze the
     * manifest from the CONVERGED lobby at the LOADING barrier, so in live play
     * it already equals the host's locked pick -- rollback_game_runtime.c ->
     * match_manifest.c require manifest.track_id == loaded_track_id or the peer
     * rejects the race). We compare the last-seen host-intended track from the
     * forward feed against it so "the host's locked track is what loads" is
     * explicit and testable, but we NEVER substitute the intended track for the
     * manifest -- doing so would fail peer admission. */
    if (intended != MDKR_ONLINE_SESSION_TRACK_NONE &&
        (s32) intended != manifestTrack) {
        fprintf(stderr,
                "[online-boot] track divergence: snapshot=%d manifest=%d "
                "(booting manifest -- admission authority)\n",
                (int) intended, manifestTrack);
    } else {
        fprintf(stderr, "[online-boot] track honored: %d\n", manifestTrack);
    }

    online_session_test_script_teardown();
    sOnlineSession.active = 0;
    /* Reuse the game's own race boot (extracted to online/online_race_boot.c in
     * PD-T4); it loads launch->manifest.track_id and sets gGameMode =
     * GAMEMODE_INGAME. No race-setup logic is duplicated. */
    mdkr_online_boot_direct_race(sOnlineSession.launch);
}

void mdkr_online_session_begin(const MdkrMatchLaunchDescriptorV1 *launch) {
    memset(&sOnlineSession, 0, sizeof(sOnlineSession));
    sOnlineSession.phase = MDKR_ONLINE_SESSION_LOBBY_WAIT;
    sOnlineSession.launch = launch;
    /* NONE, not the memset 0 (a real track id) -- so a boot with no forward feed
     * (legacy direct boot) reads "intended none" and logs the honored path, never
     * a spurious "divergence snapshot=0". */
    sOnlineSession.intendedTrack = MDKR_ONLINE_SESSION_TRACK_NONE;
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

/* PD-T3: whether CHARSELECT should hand off to the native TRACKSELECT screen once
 * the local seat is confirmed+ready. It does in real play and in the TRACKSELECT
 * lane; the STANDALONE CHARSELECT lane (which scripts its own host-start straight
 * to LOADING and asserts the historical charselect->race hand-off) deliberately
 * keeps that behaviour, so we DON'T insert TRACKSELECT when the CHARSELECT seam is
 * armed WITHOUT the TRACKSELECT seam. This is the smallest wiring that satisfies
 * ruling R-B while keeping check_online_charselect.py green unchanged. */
static bool online_session_trackselect_enabled(void) {
    return !(mdkr_online_charselect_test_active() &&
             !mdkr_online_trackselect_test_active());
}

/* True once the LOCAL seat has both a locked character and its ready flag while
 * the room is still in LOBBY -- the R-B signal that CHARSELECT is done and the
 * host/joiner should move on to the native TRACKSELECT screen. */
static bool online_session_local_seat_ready_in_lobby(
    const MdkrPartyLinkSnapshot *snap) {
    unsigned i;
    if (snap->phase != (uint8_t) MDKR_ONLINE_SESSION_LOBBY_PHASE) {
        return false;
    }
    for (i = 0u; i < MDKR_PARTY_LINK_SEATS; i++) {
        const MdkrPartyLinkSeat *seat = &snap->seats[i];
        if (seat->occupied && seat->is_local && seat->ready &&
            seat->character_id != 0xFFu) {
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
        /* PD-T3: inert unless the TRACKSELECT headless seam is armed AND nothing
         * else installed the feed (the CHARSELECT seam owns install in the
         * combined lane); a defensive standalone install otherwise. */
        mdkr_online_trackselect_test_lobby_pump();

        haveSnap = mdkr_party_link_read(&snap);
        if (haveSnap) {
            online_session_stash_intended(&snap); /* PD-T4 observability */
        }
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
        MdkrOnlineCharselectResult r;
        {
            /* PD-T4: track the host-intended pick as the room converges. */
            MdkrPartyLinkSnapshot csSnap;
            if (mdkr_party_link_read(&csSnap)) {
                online_session_stash_intended(&csSnap);
            }
        }
        r = mdkr_online_charselect_tick(updateRate);
        if (r == MDKR_ONLINE_CHARSELECT_ADVANCE) {
            /* The authoritative lobby left LOBBY (host started / loading). This
             * is the safety path (R-B) and the historical CHARSELECT-lane
             * hand-off: boot the race directly. */
            mdkr_online_charselect_exit();
            online_session_boot_race();
        } else if (r == MDKR_ONLINE_CHARSELECT_STAY &&
                   online_session_trackselect_enabled() &&
                   mdkr_online_charselect_local_ready()) {
            /* PD-T3 (R-B): once the local seat is confirmed+ready while the room
             * is still in LOBBY, hand off to the native TRACKSELECT screen.
             * F-I1: gate on the SCREEN's own confirmed+ready latch AS WELL AS the
             * snapshot's ready flag. The snapshot ready lags un-ready by >=1 pump
             * after a TRACKSELECT->CHARSELECT back-out, so relying on it alone
             * would one-frame flash charselect and re-advance; the screen latch
             * resets immediately on _enter(), so requiring it keeps the player on
             * charselect until they genuinely re-confirm+re-ready. */
            MdkrPartyLinkSnapshot snap;
            if (mdkr_party_link_read(&snap) &&
                online_session_local_seat_ready_in_lobby(&snap)) {
                mdkr_online_charselect_exit();
                sOnlineSession.phase = MDKR_ONLINE_SESSION_TRACKSELECT;
                mdkr_online_trackselect_enter();
                fprintf(stderr,
                        "[online-session] charselect -> trackselect (local seat "
                        "ready in LOBBY)\n");
            }
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
    case MDKR_ONLINE_SESSION_TRACKSELECT: {
        MdkrOnlineTrackselectResult r =
            mdkr_online_trackselect_tick(updateRate);
        {
            /* PD-T4: read AFTER the tick so the host's just-reduced config
             * (SET_CONFIG_TRACK / SET_CUP) is captured before any boot. */
            MdkrPartyLinkSnapshot tsSnap;
            if (mdkr_party_link_read(&tsSnap)) {
                online_session_stash_intended(&tsSnap);
            }
        }
        if (r == MDKR_ONLINE_TRACKSELECT_ADVANCE) {
            /* Host started -> lobby left LOBBY -> boot the race. */
            mdkr_online_trackselect_exit();
            online_session_boot_race();
        } else if (r == MDKR_ONLINE_TRACKSELECT_LEAVE) {
            /* R-B: B on TRACKSELECT is a clean "back one level" to CHARSELECT
             * (NOT the leave-to-launcher stub). The local player re-readies on
             * CHARSELECT as a natural consequence. Deliberately do NOT reset
             * sCharselectLeaveWarned: this is a continuation of the same session,
             * so the PD-T6 leave-to-launcher warn-once latch is preserved (a
             * browse-B on the re-entered CHARSELECT never re-spams the stub). */
            mdkr_online_trackselect_exit();
            sOnlineSession.phase = MDKR_ONLINE_SESSION_CHARSELECT;
            mdkr_online_charselect_enter();
            fprintf(stderr,
                    "[online-session] trackselect -> charselect (back one "
                    "level)\n");
        }
        break;
    }
    case MDKR_ONLINE_SESSION_RACE:
        /* The boot normally fires inline from LOBBY_WAIT / CHARSELECT /
         * TRACKSELECT; boot here too if the mode is somehow re-entered before the
         * hand-off completed. */
        online_session_boot_race();
        break;
    default:
        /* RESULTS / CEREMONY arrive in PD-T5..T6. */
        break;
    }
}

#endif /* MDKR_ENABLE_ONLINE_BETA */
