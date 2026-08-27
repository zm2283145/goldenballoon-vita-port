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
#include "net/online_race_results.h"   /* PD-T5 captured-placements availability */
#include "net/net_roster_runtime.h"    /* PD-T6ac per-round launch descriptor re-fetch */
#include "net/match_input_runtime.h"   /* PD-T6ac live re-wait: match-input epoch gate */
#include "online/online_charselect.h"  /* PD-T2 native CHARSELECT phase */
#include "online/online_trackselect.h" /* PD-T3 native TRACKSELECT phase +
                                          mdkr_online_trackselect_cup_track */
#include "online/online_race_boot.h"   /* PD-T4 direct race boot (extracted) */
#include "online/online_results.h"     /* PD-T5 native RESULTS/STANDINGS phase */

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
    /* PD-T5: number of engine races booted this process (>=2 in the resident
     * soak; the ADVANCE off the last race's STANDINGS does NOT re-boot). */
    u32 raceCount;
    /* PD-T5: the RESULTS phase must call the screen's _enter on its next tick
     * (set by mdkr_online_session_resume_results, cleared once entered). */
    u8 resultsPending;
    /* M1: the last observed forward-feed mode, so a tournament->single interlude
     * clears the sticky intendedTrack stash instead of carrying a stale pick into
     * the boot's divergence check. 0xFF == none observed yet. */
    u8 lastModeSeen;
    /* PD-T6ac: LIVE residency (a real two-adapter session, resumed on the reducer
     * snapshot showing RESULTS -- NOT the scripted MDKR_TEST_ONLINE_RESIDENT
     * soak). It changes the RESULTS->next-race path: instead of re-booting inline
     * (the scripted soak, no transport), the session re-enters LOBBY_WAIT and
     * waits for the launcher to re-cycle the room to a fresh race-ready transport
     * (a new match_epoch) before booting. */
    u8 liveResident;
    /* PD-T6ac: the match_epoch of the race this session last booted. In a LIVE
     * resident re-wait, the next boot is gated on the runtime descriptor's epoch
     * having advanced past this (the launcher's fresh per-round install). */
    u32 bootedEpoch;
    /* PD-T6h1 (M4): throttle the per-tick live re-wait witness. Now that the
     * launcher-side round advance is FRAME-DRIVEN the re-wait spans MANY engine
     * ticks, so logging every tick would flood stderr. Log only the FIRST re-wait
     * tick of each round and every time `ready` changes. 0xFF == not yet logged
     * this round (reset in online_session_boot_race). */
    u8 liveReWaitLastReady;
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
/* M-3: optional tournament->single MODE INTERLUDE (env
 * MDKR_TEST_ONLINE_SESSION_MODE_INTERLUDE = the cup id to stash under tournament).
 * -1 unresolved, -2 off. When on, the first half of the hold publishes
 * mode=TOURNAMENT + that cup (so the session STASHES the cup's round-0 track),
 * then the second half flips to mode=SINGLE (no track). M1 must clear the sticky
 * stash on that mode change, so the boot logs "track honored" on the SINGLE
 * manifest; if the M1 clear is reverted the stale cup track lingers and the boot
 * logs a "track divergence" -- which the session-boot lane's interlude scenario
 * asserts against, making this a NON-vacuous M1 regression guard. */
static s8 sInterludeCup = -1;

static void online_session_interlude_resolve(void) {
    if (sInterludeCup == -1) {
        const char *e = getenv("MDKR_TEST_ONLINE_SESSION_MODE_INTERLUDE");
        sInterludeCup = (e != NULL) ? (s8) strtoul(e, NULL, 10) : (s8) -2;
    }
}

static void online_session_test_maybe_script(void) {
    const char *env = getenv("MDKR_TEST_ONLINE_SESSION_SCRIPT");
    MdkrPartyLinkSnapshot snap;

    if (env == NULL) {
        return;
    }
    online_session_interlude_resolve();
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
    snap.mode = (uint8_t) MDKR_PARTY_LINK_MODE_SINGLE;
    /* M-3 interlude: TOURNAMENT (stash the cup's round-0 track) for the first half
     * of the hold, then SINGLE (M1 must clear the stale stash). */
    if (sInterludeCup >= 0 &&
        sOnlineSession.lobbyWaitTicks < (sTestHoldTicks / 2u)) {
        snap.mode = (uint8_t) MDKR_PARTY_LINK_MODE_TOURNAMENT;
        snap.cup_id = (uint8_t) sInterludeCup;
    }
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
    u16 intended;
    /* M1: a mode interlude (e.g. tournament -> single before the host re-locks)
     * must not leave the previous mode's resolved track stuck in intendedTrack --
     * that stale pick would then log a spurious "[online-boot] track divergence"
     * against the freshly-booted manifest. When the observed forward-feed mode
     * changes, drop the stash back to NONE so the honored/divergence check
     * re-resolves from the new mode's feed (or reads NONE and logs honored). */
    if (sOnlineSession.lastModeSeen != snap->mode) {
        sOnlineSession.lastModeSeen = snap->mode;
        sOnlineSession.intendedTrack = MDKR_ONLINE_SESSION_TRACK_NONE;
    }
    intended = online_session_resolve_intended(snap);
    if (intended != MDKR_ONLINE_SESSION_TRACK_NONE) {
        sOnlineSession.intendedTrack = intended;
    }
}

/* ---- PD-T5 resident-mode flag (scoping ruling R-A) ------------------------ *
 * The post-race fork re-enters the session (RESULTS -> next race in ONE engine
 * process) ONLY when this flag is on. It is set ONLY by the scripted resident
 * soak (env MDKR_TEST_ONLINE_RESIDENT = the number of engine races to drive).
 * With it OFF -- every existing live lane and real play today -- the menu.c
 * post-race hook keeps calling platform_request_exit(0) exactly as now, so no
 * live lane changes behaviour. Making LIVE play resident is PD-T6. */
static s8 sResidentResolved = -1; /* -1 unresolved, 0 off, 1 on */
static u32 sResidentRaces;        /* total engine races the soak drives (0 == off) */

static void online_session_resident_resolve(void) {
    if (sResidentResolved < 0) {
        const char *e = getenv("MDKR_TEST_ONLINE_RESIDENT");
        unsigned long n = (e != NULL) ? strtoul(e, NULL, 10) : 0ul;
        if (n == 0ul) {
            /* PD-T6ac: the LIVE-loopback resident lane's TARGET race count (the
             * scripted-soak env is unset there). Residency itself latches off the
             * reducer snapshot at resume -- this only sizes the RESULTS screen's
             * isFinal decision. Non-resident live lanes set neither env, so this
             * stays 0 (isFinal path irrelevant -- they exit at postrace). */
            const char *le = getenv("MDKR_APP_TEST_ONLINE_LIVE_RESIDENT");
            n = (le != NULL) ? strtoul(le, NULL, 10) : 0ul;
        }
        sResidentRaces = (u32) n;
        sResidentResolved = (n > 0ul) ? 1 : 0;
    }
}

static void online_session_boot_race(void) {
    u16 intended;
    s32 manifestTrack;

    /* PD-T6ac: RE-FETCH the launch descriptor each boot. mdkr_online_session_begin
     * stashed race 1's; but in a resident LIVE session the launcher clears +
     * re-installs the roster/descriptor per round (a fresh match_epoch and, in a
     * tournament, the next cup track), so a stale race-1 pointer would boot the
     * wrong race. Re-reading here picks up each round's fresh descriptor. Falls
     * back to the stashed pointer when the runtime has none (the scripted soak
     * keeps a single install, so this returns the same descriptor). */
    {
        const MdkrMatchLaunchDescriptorV1 *launch =
            mdkr_net_roster_runtime_launch_descriptor();
        if (launch != NULL) {
            sOnlineSession.launch = launch;
        }
    }
    intended = sOnlineSession.intendedTrack;
    manifestTrack = (s32) sOnlineSession.launch->manifest.track_id;
    sOnlineSession.bootedEpoch = sOnlineSession.launch->manifest.match_epoch;
    /* M4: arm the live re-wait witness throttle so the NEXT round's re-wait logs
     * its first tick + ready changes afresh (0xFF != any real ready value). */
    sOnlineSession.liveReWaitLastReady = 0xFFu;

    sOnlineSession.raceCount++; /* PD-T5: count engine races booted this process */
    sOnlineSession.phase = MDKR_ONLINE_SESSION_RACE;
    /* Isolation witness: on the online path control reached the session
     * (gGameMode == GAMEMODE_ONLINE_SESSION) and the offline boot menu was
     * never loaded (gCurrentMenuId still 0, i.e. not MENU_BOOT). */
    /* M-6: the LOBBY_WAIT tick count is meaningful only for the FIRST boot; a
     * RESULTS-origin re-boot (race >= 2) reuses that stale counter, so the trailing
     * [race=N] disambiguates which engine race this is (the re-boot itself is
     * announced separately at the RESULTS ADVANCE). The witness prefix through
     * gCurrentMenuId is unchanged, so every lane's regex still matches. */
    fprintf(stderr,
            "[online-session] phase=RACE booting after %u LOBBY_WAIT tick(s); "
            "isolation gGameMode=%d gCurrentMenuId=%d (0 == offline MENU_BOOT "
            "never loaded) [race=%u]\n",
            sOnlineSession.lobbyWaitTicks, gGameMode, gCurrentMenuId,
            sOnlineSession.raceCount);

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
    sOnlineSession.lastModeSeen = 0xFFu; /* M1: no forward-feed mode observed yet */
    sOnlineSession.liveReWaitLastReady = 0xFFu; /* M4: re-wait witness unthrottled */
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

/* DEFERRED to PD-T6 (do NOT build here -- documented so the seam is explicit):
 *   - Making LIVE play resident: pumping OnlineRoom_pumpPartyLink / the reverse
 *     intent feed from the overlay-service hook during residency, publishing
 *     results mid-residency, per-round roster re-freeze/cycling, and replacing
 *     the live-path platform_request_exit(0) with the resident return. PD-T5
 *     gates all of that behind MDKR_TEST_ONLINE_RESIDENT so only the scripted
 *     soak re-enters and every live lane still exits unchanged.
 *   - The champion CEREMONY 3D cutscene (the MDKR_ONLINE_SESSION_CEREMONY phase,
 *     still default:-swallowed below): the final STANDINGS holds in its place.
 *   - The real engine->launcher return handshake (the same wiring that boots this
 *     session at LOBBY phase): a RESULTS/CHARSELECT LEAVE is a documented stub.
 *   - ABANDON_RACE is NOT needed: abnormal ends keep platform_request_exit(0)
 *     (R-B), which resume_results below preserves by returning false when no
 *     finish was captured. */

/* PD-T5 post-race RE-ENTRY (scoping ruling R-A / R-B). Called from the online
 * post-race hook (menu.c) when the grace period elapses. Returns true -- and
 * re-arms the session into its RESULTS phase in THIS engine process -- ONLY when
 * BOTH: resident mode is on (the scripted soak) AND this race captured a finish
 * order (a NON-consuming availability peek, so the RESULTS screen still polls the
 * placements). Returns false for every live lane (resident OFF) and every
 * abnormal end (no captured results), so the caller keeps calling
 * platform_request_exit(0) exactly as today -- preserving the proven recovery
 * routing. A bare gGameMode write would hit the tick's active==0 fail-safe, so
 * this re-arms active=1 + phase=RESULTS; the launch descriptor pointer survived
 * (the roster stays installed while the engine is resident). */
bool mdkr_online_session_resume_results(void) {
    if (sOnlineSession.launch == NULL) {
        return false; /* no session ever began */
    }
    if (!mdkr_net_roster_runtime_active()) {
        /* Roster retired (abnormal end / no residency): caller exits, preserving
         * the P1 recovery routing. */
        return false;
    }
    /* PD-T6ac LIVE residency (the real behaviour now): the launcher pump PUBLISH_-
     * RESULTS mid-residency, so the reducer snapshot shows RESULTS for this race.
     * Resume on that REAL signal (not an env), reading placements/points from the
     * snapshot. This is what makes a live session resident across races. */
    {
        MdkrPartyLinkSnapshot snap;
        if (mdkr_party_link_read(&snap) &&
            snap.phase == (uint8_t) 4u /* MDKR_ONLINE_RESULTS */) {
            sOnlineSession.liveResident = 1u;
            online_session_resident_resolve();
            sOnlineSession.active = 1;
            sOnlineSession.phase = MDKR_ONLINE_SESSION_RESULTS;
            sOnlineSession.resultsPending = 1u;
            gGameMode = GAMEMODE_ONLINE_SESSION;
            fprintf(stderr,
                    "[online-session] resume: RESULTS phase (race %u of %u; live "
                    "reducer RESULTS) gGameMode=%d\n",
                    sOnlineSession.raceCount, sResidentRaces, gGameMode);
            return true;
        }
    }
    /* Scripted resident soak fallback (env MDKR_TEST_ONLINE_RESIDENT): no live
     * launcher publishes the RESULTS snapshot mid-grace, so gate on the engine-
     * captured availability peek (NON-consuming; the RESULTS test seam owns the
     * consuming poll). Unchanged behaviour, so the scripted soak stays green and
     * every non-resident live lane (env OFF, no RESULTS snapshot) still exits. */
    online_session_resident_resolve();
    if (!sResidentResolved) {
        return false; /* resident OFF: caller exits (zero live-lane change) */
    }
    if (!mdkr_online_race_results_available()) {
        return false; /* abnormal end / nothing captured (R-B): caller exits */
    }
    sOnlineSession.active = 1;
    sOnlineSession.phase = MDKR_ONLINE_SESSION_RESULTS;
    sOnlineSession.resultsPending = 1u;
    gGameMode = GAMEMODE_ONLINE_SESSION;
    fprintf(stderr,
            "[online-session] resume: RESULTS phase (race %u of %u; results "
            "captured) gGameMode=%d\n",
            sOnlineSession.raceCount, sResidentRaces, gGameMode);
    return true;
}

void mdkr_online_session_tick(s32 updateRate) {
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

        /* PD-T6ac LIVE residency re-wait: after a RESULTS advance we re-entered
         * LOBBY_WAIT and the launcher is re-cycling the room (REMATCH -> re-Ready
         * -> START) to a fresh race-ready transport. Boot the next race ONLY once
         * a NEW roster/descriptor is installed (match_epoch advanced past the
         * just-raced one) AND the match-input runtime is live on that epoch. Idle
         * (keeping intended-track observability fresh) otherwise. This is disjoint
         * from the first boot (raceCount 0) and from the scripted soak (which
         * re-boots inline off the RESULTS advance, never through here). */
        if (sOnlineSession.liveResident && sOnlineSession.raceCount > 0u) {
            const MdkrMatchLaunchDescriptorV1 *launch =
                mdkr_net_roster_runtime_launch_descriptor();
            bool ready =
                launch != NULL && mdkr_net_roster_runtime_active() &&
                launch->manifest.match_epoch != sOnlineSession.bootedEpoch &&
                mdkr_match_input_runtime_active() &&
                mdkr_match_input_runtime_epoch() == launch->manifest.match_epoch;
            {
                MdkrPartyLinkSnapshot rsnap;
                if (mdkr_party_link_read(&rsnap)) {
                    online_session_stash_intended(&rsnap);
                }
            }
            /* M4: throttle to the first re-wait tick + every ready-state change,
             * not every tick -- the frame-driven advance makes this wait span many
             * ticks and an unthrottled line would flood stderr. */
            if (sOnlineSession.liveReWaitLastReady != (u8) ready) {
                fprintf(stderr,
                        "[online-session] phase=LOBBY_WAIT (live re-wait) tick=%u "
                        "epoch=%u bootedEpoch=%u ready=%d\n",
                        sOnlineSession.lobbyWaitTicks,
                        (unsigned) (launch != NULL ? launch->manifest.match_epoch
                                                   : 0u),
                        (unsigned) sOnlineSession.bootedEpoch, (int) ready);
                sOnlineSession.liveReWaitLastReady = (u8) ready;
            }
            sOnlineSession.lobbyWaitTicks++;
            if (ready) {
                online_session_boot_race();
            }
            break;
        }

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
            /* PD-T4: track the host-intended pick as the room converges. M2:
             * stashing PRE-tick is correct here (unlike TRACKSELECT, which reads
             * POST-tick to capture the host's just-reduced SET_CONFIG_TRACK /
             * SET_CUP): CHARSELECT reduces NO host session config, so its tick
             * cannot change the intended track and there is nothing to read after
             * it -- the pre-tick snapshot already carries the freshest config. */
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
    case MDKR_ONLINE_SESSION_RESULTS: {
        /* PD-T5: the native RESULTS/STANDINGS screen, re-entered by the resident
         * post-race fork (mdkr_online_session_resume_results). Enter it once on
         * the first tick after the resume (the enter polls this race's captured
         * placements); the screen renders placements from the poll + the cup
         * points from the party_link snapshot. */
        MdkrOnlineResultsResult r;
        if (sOnlineSession.resultsPending) {
            online_session_resident_resolve();
            /* PD-T6ac LIVE residency: FREE the just-finished race level NOW, on
             * RESULTS entry, before showing the screen. Unlike the scripted soak
             * (a transport-less autopilot race, harmless to keep loaded), a LIVE
             * race's rollback runtime keeps DRAINING its transport every frame
             * while the level stays loaded -- and the host's imminent REMATCH
             * tears that transport down, so a held level would fault
             * (mdkr_rollback_game_runtime_prepare_tick). unload_level_game()
             * deactivates the rollback runtime (level_end), so the RESULTS screen
             * (which borrows only portraits/fonts, like charselect) renders with
             * no race resident and the launcher's per-round re-cycle can safely
             * swap the roster + match-input epoch. */
            if (sOnlineSession.liveResident) {
                unload_level_game();
            }
            {
                /* isFinal: no further race will boot (the ADVANCE off this
                 * screen would be the (N+1)th boot). The final STANDINGS holds
                 * on screen and the autoplay tick budget ends the process. */
                u8 isFinal = (sOnlineSession.raceCount >= sResidentRaces) ? 1u
                                                                          : 0u;
                u8 raceIndex = (sOnlineSession.raceCount > 0u)
                                   ? (u8) (sOnlineSession.raceCount - 1u)
                                   : 0u;
                mdkr_online_results_enter(isFinal, raceIndex);
            }
            sOnlineSession.resultsPending = 0u;
        }
        r = mdkr_online_results_tick(updateRate);
        if (r == MDKR_ONLINE_RESULTS_ADVANCE) {
            if (sOnlineSession.raceCount < sResidentRaces) {
                /* Re-boot the NEXT race IN THIS SAME ENGINE PROCESS -- the
                 * load-bearing residency proof (>=2 direct boots). */
                mdkr_online_results_exit();
                if (!sOnlineSession.liveResident) {
                    /* Scripted soak held the level resident through RESULTS, so
                     * free it now before the next boot's load_level_game -- the
                     * same "leave the current race level" call the offline
                     * race->race path makes. (The LIVE lane already freed it on
                     * RESULTS entry, above.) */
                    unload_level_game();
                }
                if (sOnlineSession.liveResident) {
                    /* PD-T6ac LIVE residency: do NOT boot inline -- the RESULTS
                     * host-advance drove the reducer's REMATCH via the reverse
                     * feed, but the launcher must still re-Ready + START the room
                     * and re-install a fresh roster + match-input source (new
                     * match_epoch) before the next race can run on the real
                     * transport. Re-enter LOBBY_WAIT, whose live re-wait gate boots
                     * once that fresh descriptor + match-input are live. */
                    sOnlineSession.phase = MDKR_ONLINE_SESSION_LOBBY_WAIT;
                    fprintf(stderr,
                            "[online-session] results -> awaiting next race (LIVE "
                            "residency; launcher re-cycling roster/epoch)\n");
                } else {
                    /* Scripted soak (no transport): the single install re-boots
                     * inline off the frozen descriptor. */
                    fprintf(stderr,
                            "[online-session] results -> re-boot race %u "
                            "(RESULTS-origin, same process)\n",
                            sOnlineSession.raceCount + 1u);
                    online_session_boot_race();
                }
            }
            /* else: soak complete -- the screen returned isFinal, so it holds
             * the final standings; nothing to do. */
        } else if (r == MDKR_ONLINE_RESULTS_LEAVE) {
            /* Backing out to the launcher room is the PD-T6 engine->launcher
             * return handshake (same wiring that boots this session). For now,
             * hold on the screen rather than half-tear-down into an unwired
             * state; the resident soak never presses B. */
        }
        break;
    }
    default:
        /* CEREMONY (the champion cutscene) arrives in PD-T6. */
        break;
    }
}

#endif /* MDKR_ENABLE_ONLINE_BETA */
