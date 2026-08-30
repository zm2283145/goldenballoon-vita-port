/* SEPARATED-BOOT-PATH (Strategy D) online session state machine.
 *
 * The whole translation unit is #if MDKR_ENABLE_ONLINE_BETA and the file is
 * added to the build ONLY inside the beta CMake gate (game/src/online/ is NOT
 * auto-globbed), so a normal (beta OFF) build never compiles a byte of it and
 * the release engine object is untouched. See game/src/online/online_session.h
 * for the architecture rationale.
 *
 * The session owns its own state and drives the phase machine: LOBBY_WAIT (read
 * the party_link forward feed and wait until the room leaves selection), the
 * native CHARSELECT / TRACKSELECT screens, the RACE hand-off (call the existing
 * mdkr_online_boot_direct_race, which sets GAMEMODE_INGAME -- no race-setup logic
 * is duplicated here; the boot lives in online/online_race_boot.c), and the
 * post-race RESULTS / CEREMONY screens.
 */
#include "online/online_session.h"

#if MDKR_ENABLE_ONLINE_BETA

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "types.h"
#include "thread3_main.h"
#include "platform_os.h"               /* watchdog: platform_request_exit
                                          (the SAME clean engine-exit menu.c uses on
                                          the online post-race path) */
#include "net/party_link.h"
#include "net/online_race_results.h"   /* captured-placements availability */
#include "net/net_roster_runtime.h"    /* per-round launch descriptor re-fetch */
#include "net/match_input_runtime.h"   /* live re-wait: match-input epoch gate */
#include "online/online_charselect.h"  /* native CHARSELECT phase */
#include "online/online_vehicleselect.h" /* native VEHICLESELECT phase */
#include "online/online_trackselect.h" /* native TRACKSELECT phase +
                                          mdkr_online_trackselect_cup_track */
#include "online/online_race_boot.h"   /* direct race boot (extracted) */
#include "online/online_results.h"     /* native RESULTS/STANDINGS phase */
#include "online/online_ceremony.h"    /* native champion CEREMONY phase */
#include "online/online_standings.h"   /* capture the final ranking (shared sort) */

/* fade-skip primitive (online_screen_util.c). Declared directly rather than
 * pulling online_screen_util.h in: that header drags rcp_dkr.h -> ultra64.h,
 * whose os_libc sprintf declarations must PRECEDE the system <stdio.h> this TU
 * includes first (the screen TUs' include-ordering rationale, inverted). */
void mdkr_online_screen_fade_skip_once(void);
#include "rollback/rollback_game_runtime.h" /* clean rollback teardown on the
                                              recoverable peer-loss unwind */

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
#define MDKR_ONLINE_SESSION_RESULTS_PHASE 4u /* MDKR_ONLINE_RESULTS */

/* "No track resolved yet" sentinel for intendedTrack below -- matches the
 * forward-feed / lobby "none" width (configured_track 0xFFFF). */
#define MDKR_ONLINE_SESSION_TRACK_NONE 0xFFFFu

typedef struct MdkrOnlineSessionState {
    MdkrOnlineSessionPhase phase;
    const MdkrMatchLaunchDescriptorV1 *launch;
    u32 lobbyWaitTicks;
    /* the last-seen resolved host-INTENDED race track from the forward
     * feed (single race -> configured_track; tournament -> the cup's scheduled
     * round track), or MDKR_ONLINE_SESSION_TRACK_NONE. This is an OBSERVABILITY
     * value ONLY -- the boot always loads launch->manifest.track_id (the peer
     * rollback-admission authority); intendedTrack is logged against it so "the
     * host's locked track is what loads" is explicit and testable. */
    u16 intendedTrack;
    u8 active;
    /* number of engine races booted this process (>=2 in the resident
     * soak; the ADVANCE off the last race's STANDINGS does NOT re-boot). */
    u32 raceCount;
    /* the RESULTS phase must call the screen's _enter on its next tick
     * (set by mdkr_online_session_resume_results, cleared once entered). */
    u8 resultsPending;
    /* the last observed forward-feed mode, so a tournament->single interlude
     * clears the sticky intendedTrack stash instead of carrying a stale pick into
     * the boot's divergence check. 0xFF == none observed yet. */
    u8 lastModeSeen;
    /* LIVE residency (a real two-adapter session, resumed on the reducer
     * snapshot showing RESULTS -- NOT the scripted MDKR_TEST_ONLINE_RESIDENT
     * soak). It changes the RESULTS->next-race path: instead of re-booting inline
     * (the scripted soak, no transport), the session re-enters LOBBY_WAIT and
     * waits for the launcher to re-cycle the room to a fresh race-ready transport
     * (a new match_epoch) before booting. */
    u8 liveResident;
    /* the match_epoch of the race this session last booted. In a LIVE
     * resident re-wait, the next boot is gated on the runtime descriptor's epoch
     * having advanced past this (the launcher's fresh per-round install). */
    u32 bootedEpoch;
    /* throttle the per-tick live re-wait witness. Now that the
     * launcher-side round advance is FRAME-DRIVEN the re-wait spans MANY engine
     * ticks, so logging every tick would flood stderr. Log only the FIRST re-wait
     * tick of each round and every time `ready` changes. 0xFF == not yet logged
     * this round (reset in online_session_boot_race). */
    u8 liveReWaitLastReady;
    /* the session BEGAN without a launch descriptor (the party_link
     * lobby-start path -- the native online screens own race 1). Every new
     * behaviour (the descriptor-less begin witness, the race-1 readiness gate in
     * online_session_boot_race, the descriptor-less LOBBY_WAIT re-wait, and the
     * feed-derived isFinal) is gated on this latch, so a descriptor-first begin
     * (every existing lane + real play today) is byte-behaviour-unchanged. */
    u8 beganWithoutDescriptor;
    /* set once the descriptor-less room has committed to loading (a
     * boot was requested while the real descriptor was not yet live -- the native
     * ADVANCE / LOBBY_WAIT phase-left-LOBBY fires frames before the launcher
     * builds it). While set, LOBBY_WAIT routes through the descriptor-less re-wait
     * (below) instead of re-fronting CHARSELECT, and boots once the descriptor +
     * roster + match-input are ready. Cleared implicitly at the (single) boot. */
    u8 desclessBootPending;
    /* throttle the descriptor-less re-wait witness (mirrors
     * liveReWaitLastReady). 0xFF == not yet logged. */
    u8 desclessReWaitLastReady;
    /* the feed-derived finality of the CURRENT RESULTS race, latched at
     * RESULTS ENTER (when the snapshot's race_index still names the just-finished
     * race). The RESULTS->next-race decision for a descriptor-less session MUST read
     * THIS, not a fresh online_session_feed_isfinal() at ADVANCE time -- by the time
     * the RESULTS screen returns ADVANCE the host's REMATCH has ALREADY advanced the
     * reducer's race_index, so a fresh read would see the NEXT race's index and
     * wrongly treat the non-final race N (race_index N-1) as final once N-1 reached
     * CUP_ROUNDS-1 via the just-landed rematch. Unused for a descriptor-first begin. */
    u8 resultsIsFinal;
    /* WALL-CLOCK WATCHDOG: frames spent in the CURRENT
     * descriptor-less wait (the race-1 re-wait OR, for a descriptor-less session,
     * the per-round live re-wait). Reset to 0 at every boot, so each round's wait
     * gets a fresh budget; incremented only on the descriptor-less waits. When it
     * exceeds MDKR_ONLINE_SESSION_DESCLESS_WAIT_FRAME_BUDGET the session logs a
     * clear diagnostic and routes to the platform exit -- never hangs. Inert for a
     * descriptor-first begin (beganWithoutDescriptor == 0), so the resident lane's
     * shared live re-wait is byte-behaviour-unchanged. */
    u32 desclessWaitTicks;
    /* this descriptor-less session drives a SINGLE local endpoint (a real
     * 2-process room -- the remote readies itself over the transport), latched from
     * mdkr_party_link_is_single_endpoint() at begin. When set, the descriptor-less
     * waits use the WALL-CLOCK deadline below (a real cross-process DTLS handshake +
     * preflight is seconds and variable; a frame count is wrong when the frame rate
     * varies) and route a stuck wait to an ERROR exit, and the per-round
     * re-wait gains a mid-tournament CANCEL unwind. The two-endpoint
     * loopback lanes leave this 0 -> the existing frame-count + clean-exit path is
     * byte-behaviour-unchanged. */
    u8 singleEndpoint;
    /* WALL-CLOCK deadline (ns, platform_perf_monotonic_ns). Absolute
     * deadline of the CURRENT descriptor-less wait; 0 == not armed. Re-armed at each
     * boot, at RESULTS enter (the REMATCH-convergence hold), and when a
     * per-round re-wait begins, so each of the THREE descriptor-less waits gets its
     * own fresh budget. Used only when singleEndpoint. */
    u64 desclessWaitDeadlineNs;
    /* in the per-round re-wait, set once the room has LEFT LOBBY
     * this round (the launcher advance drove it to LOADING). Only AFTER that does a
     * return to LOBBY unambiguously mean a mid-tournament CANCEL_LOADING (vs. the
     * initial next-round SELECTING the re-wait first observes). Reset at each boot.
     * Single-endpoint only. */
    u8 desclessRoundLeftLobby;
    /* debounce counter for the pre-START remote-seat-vacated
     * detector. Incremented each CHARSELECT/TRACKSELECT tick the forward feed shows
     * a LOBBY-phase room seating the LOCAL player but NO remote seat; reset the
     * instant a remote reappears (or the room leaves LOBBY). On a sustained absence
     * the session notes LEFT + exits. Inert for a descriptor-first begin. */
    u16 remoteAbsentTicks;
    /* SINGLE-RACE "Race Again" auto-start. The host committed RACE AGAIN
     * (same config) on the RESULTS chooser, so the session re-entered LOBBY_WAIT
     * with NO native selection screen to re-Ready + host-START the next race. While
     * set, the live re-wait auto-publishes a reverse-feed intent each tick -- keep
     * the persisted local character/vehicle (so CHOOSE_* stay converged no-ops),
     * re-assert READY (REMATCH cleared it), and host-START -- so the launcher's
     * reverse pump re-cycles the room to a fresh race-ready transport with NO human
     * input ("straight to a fresh RACE"). The CHANGE-picks options do NOT set this:
     * their re-fronted native screen drives the re-selection instead, so the two are
     * mutually exclusive and the auto-start never fights a live screen. Cleared once
     * the room leaves LOBBY (loading committed) and defensively at each RESULTS
     * enter. Meaningful only for a descriptor-less LIVE resident single race. */
    u8 replayAutoStart;
    /* the COMPLETE final ranking captured at the final standings while BOTH
     * seats were present (via the shared mdkr_online_standings_compute), and a
     * latch that a capture happened. Re-captured every final-standings tick that
     * still seats both players, so it always holds the last-known-good full
     * ordering. Handed to mdkr_online_ceremony_enter on the RESULTS->CEREMONY
     * transition so a host that disconnects during the dwell cannot make the
     * ceremony re-crown the surviving joiner from a 1-seat live snapshot. */
    MdkrOnlineStandings finalRanking;
    u8 finalRankingCaptured;
    /* the FOURTH single-endpoint descriptor-less wait: the committed-FINISH
     * WRAP hold (tournament final AND the single-race chooser). Once the host
     * commits FINISH, the chooser republishes the REMATCH wrap intent and holds
     * STAY until the room leaves RESULTS -- over a real WAN a link drop there
     * would park the host in the chooser forever. Set when the committed FINISH
     * hold is first observed; arms a FRESH wall-clock deadline at that moment
     * (the host's interactive deliberation before the commit must never be
     * bounded), after which the shared watchdog bounds only the post-commit
     * convergence and surfaces a genuine ERROR (never a fake FINISHED).
     * Single-endpoint only. */
    u8 finishWrapHoldArmed;
    /* the committed FINISH was the SINGLE-RACE wrap (chooser mode SINGLE): the
     * CEREMONY/FINISHED witnesses name the single-race road truthfully instead
     * of "final standings" (which a single race never shows). Latched at the
     * LEAVE routing, read by the CEREMONY case's FINISHED note. */
    u8 ceremonySingle;
} MdkrOnlineSessionState;

/* Session-owned state -- deliberately NOT any offline global. */
static MdkrOnlineSessionState sOnlineSession;

/* Warn-once latch for the CHARSELECT leave stub: the engine->launcher
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
/* optional tournament->single MODE INTERLUDE (env
 * MDKR_TEST_ONLINE_SESSION_MODE_INTERLUDE = the cup id to stash under tournament).
 * -1 unresolved, -2 off. When on, the first half of the hold publishes
 * mode=TOURNAMENT + that cup (so the session STASHES the cup's round-0 track),
 * then the second half flips to mode=SINGLE (no track). The session must clear the
 * sticky stash on that mode change, so the boot logs "track honored" on the SINGLE
 * manifest; if that clear is reverted the stale cup track lingers and the boot
 * logs a "track divergence" -- which the session-boot lane's interlude scenario
 * asserts against, making this a NON-vacuous regression guard. */
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
    /* publish the config "none" sentinels, not the bare memset zeros. This
     * lane scripts an EMPTY room with no host pick -- it boots the vote-derived
     * manifest -- so the session's intended-track resolution must read NONE. A
     * plain memset(0) would read as mode=SINGLE + configured_track=0 == "intended
     * track 0" (garbage) and log a spurious divergence; the sentinels give the
     * silent "track honored" path this lane expects. (mode stays 0 == SINGLE.) */
    snap.configured_track = MDKR_PARTY_LINK_TRACK_UNSET; /* 0xFFFF == none */
    snap.cup_id = MDKR_PARTY_LINK_CUP_UNSET;             /* 0xFF == none */
    snap.mode = (uint8_t) MDKR_PARTY_LINK_MODE_SINGLE;
    /* interlude: TOURNAMENT (stash the cup's round-0 track) for the first half
     * of the hold, then SINGLE (the session must clear the stale stash). */
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

/* resolve the host-intended race track from a forward-feed snapshot,
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
    /* a mode interlude (e.g. tournament -> single before the host re-locks)
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

/* ---- resident-mode flag ------------------------------------------------- *
 * The post-race fork re-enters the session (RESULTS -> next race in ONE engine
 * process) ONLY when this flag is on. It is set ONLY by the scripted resident
 * soak (env MDKR_TEST_ONLINE_RESIDENT = the number of engine races to drive).
 * With it OFF -- every existing live lane and real play today -- the menu.c
 * post-race hook keeps calling platform_request_exit(0) exactly as now, so no
 * live lane changes behaviour. */
static s8 sResidentResolved = -1; /* -1 unresolved, 0 off, 1 on */
static u32 sResidentRaces;        /* total engine races the soak drives (0 == off) */

static void online_session_resident_resolve(void) {
    if (sResidentResolved < 0) {
        const char *e = getenv("MDKR_TEST_ONLINE_RESIDENT");
        unsigned long n = (e != NULL) ? strtoul(e, NULL, 10) : 0ul;
        if (n == 0ul) {
            /* the LIVE-loopback resident lane's TARGET race count (the
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

/* race-1 READINESS GATE. For a descriptor-less
 * (lobby-start) session the boot must NOT fire until the launcher has built +
 * installed the REAL launch descriptor and armed the match-input source on that
 * epoch -- otherwise online_session_boot_race would deref a NULL/stale launch
 * (online_session.c:277). This is the live resident re-wait predicate (:504-507
 * below) GENERALIZED to race 1: bootedEpoch is 0 for the first boot, so any real
 * (nonzero) epoch satisfies the freshness check. It is used ONLY under the
 * beganWithoutDescriptor latch, so the !haveSnap direct-boot branch and the
 * seam-armed CHARSELECT hand-off are untouched. */
static bool online_session_descless_boot_ready(void) {
    const MdkrMatchLaunchDescriptorV1 *launch =
        mdkr_net_roster_runtime_launch_descriptor();
    return launch != NULL && mdkr_net_roster_runtime_active() &&
           launch->manifest.match_epoch != sOnlineSession.bootedEpoch &&
           mdkr_match_input_runtime_active() &&
           mdkr_match_input_runtime_epoch() == launch->manifest.match_epoch;
}

/* FEED-DERIVED finality. A descriptor-less (interactive) session
 * has no MDKR_APP_TEST_ONLINE_LIVE_RESIDENT env to size the run, so finality is
 * read from the SAME party_link forward feed the screens render: a tournament is
 * final on its last cup round (race_index >= CUP_ROUNDS-1 -- mirrors the
 * launcher's lobby_view_model.c:713 predicate and ui_online_room.cpp:2149); a
 * single race NEVER auto-finals (the host advances via REMATCH / leaves). The
 * env path stays FIRST in the RESULTS enter below, so the two resident lanes are
 * byte-behaviour-unchanged; this applies only when beganWithoutDescriptor. */
#define MDKR_ONLINE_SESSION_CUP_ROUNDS 4u /* == MDKR_ONLINE_CUP_ROUNDS (lobby_core.h) */

/* WALL-CLOCK WATCHDOG budget. The max engine frames one
 * DESCRIPTOR-LESS wait may spend before it gives up: the race-1 re-wait (host
 * STARTed but the launcher's descriptor/roster/match-input never went live), and
 * the per-round live re-wait (the launcher's REMATCH re-cycle never re-armed the
 * next race). On exceed the session logs a diagnostic and platform_request_exit(0)s
 * (the SAME clean exit the online post-race path uses) -- it NEVER hangs. This is a
 * FRAME COUNT (session tick == one engine frame), never a sleep, so exceeding it
 * degrades to a clean diagnostic. It is checked ONLY under beganWithoutDescriptor,
 * so the resident lane's shared live re-wait is byte-behaviour-unchanged.
 *
 * Sizing: on warm loopback the race-1 arm converges in ~2 frames and a per-round
 * re-cycle in ~7 (the launcher-side kResidentAdvanceFrameBudget is 900). 2700 is
 * 3x that launcher budget -- ample headroom for a cold DTLS re-handshake / loaded
 * CI host at high headless frame rates -- while at a vsynced 30-60 fps it is ~45-90
 * s of wall clock, which is generous for a real WAN/DTLS re-cycle. NOTE: a real
 * WAN is far slower than warm loopback; a follow-up should convert this
 * to an actual wall-clock deadline (this engine TU has no cheap clock, so a frame
 * budget stands in here) and retune against measured WAN convergence. */
#define MDKR_ONLINE_SESSION_DESCLESS_WAIT_FRAME_BUDGET 2700u

/* WALL-CLOCK watchdog. The SINGLE-ENDPOINT (real 2-process) path bounds
 * every descriptor-less wait by REAL wall clock (platform_perf_monotonic_ns), not a
 * frame count: a cross-process DTLS handshake + preflight is seconds and variable,
 * and a frame count over-/under-shoots as the interactive frame rate varies. The
 * deadline is armed (absolute ns) when a wait BEGINS -- see arm below -- so each of
 * the FOUR waits (race-1 re-wait, per-round re-wait, RESULTS REMATCH-hold, and the
 * final-FINISH wrap-hold) gets a fresh budget. Default is generous for a real WAN; MDKR_ONLINE_SESSION_DESCLESS_
 * WAIT_DEADLINE_MS overrides it (the single-endpoint test lanes pin a short one). */
#define MDKR_ONLINE_SESSION_DESCLESS_WAIT_DEADLINE_MS_DEFAULT 45000u
/* 10-minute sanity ceiling on the env override: a value above this (or 0) is
 * treated as unset and falls back to the default, so a fat-fingered env can never
 * stall a descriptor-less wait for an absurd duration. */
#define MDKR_ONLINE_SESSION_DESCLESS_WAIT_DEADLINE_MS_MAX 600000u

static u64 online_session_descless_deadline_ns(void) {
    static s64 sBudgetMs = -1; /* resolved once; -1 == unresolved */
    if (sBudgetMs < 0) {
        const char *e = getenv("MDKR_ONLINE_SESSION_DESCLESS_WAIT_DEADLINE_MS");
        unsigned long ms = (e != NULL) ? strtoul(e, NULL, 10) : 0ul;
        if (ms == 0ul ||
            ms > MDKR_ONLINE_SESSION_DESCLESS_WAIT_DEADLINE_MS_MAX) {
            ms = MDKR_ONLINE_SESSION_DESCLESS_WAIT_DEADLINE_MS_DEFAULT;
        }
        sBudgetMs = (s64) ms;
    }
    return (u64) sBudgetMs * UINT64_C(1000000); /* ms -> ns */
}

/* Arm the wall-clock deadline for a NEW descriptor-less wait (single-endpoint only;
 * inert otherwise). Idempotent-per-wait: call once when a wait begins. */
static void online_session_descless_wallclock_arm(void) {
    if (!sOnlineSession.singleEndpoint) return;
    sOnlineSession.desclessWaitDeadlineNs =
        platform_perf_monotonic_ns() + online_session_descless_deadline_ns();
}

/* Advance the descriptor-less wait watchdog; return true (after logging + routing to
 * the platform exit) if the current wait has exceeded its budget. Called only from
 * the descriptor-less waits, only under beganWithoutDescriptor.
 *
 * two modes. SINGLE-ENDPOINT (real 2-process) uses the WALL-CLOCK deadline
 * and requests a NONZERO exit (a genuine stuck trip is DISTINGUISHABLE from
 * a normal finish, so the launcher routes back to the room / surfaces the failure to
 * the human). The two-endpoint LOOPBACK path keeps the exact frame-count budget +
 * clean exit(0) it had, so those lanes are byte-behaviour-unchanged. */
static bool online_session_descless_watchdog_tick(const char *where) {
    if (sOnlineSession.singleEndpoint) {
        const u64 now = platform_perf_monotonic_ns();
        if (sOnlineSession.desclessWaitDeadlineNs == 0u) {
            /* Not armed yet (first tick of this wait): arm and let it run. */
            online_session_descless_wallclock_arm();
            return false;
        }
        if (now <= sOnlineSession.desclessWaitDeadlineNs) return false;
        fprintf(stderr,
                "[online-session] descless wait TIMEOUT: exceeded %ums wall-clock "
                "deadline at %s (raceCount=%u) -- ERROR: routing to return-to-room "
                "(no hang, no silent finish)\n",
                (unsigned) (online_session_descless_deadline_ns() /
                            UINT64_C(1000000)),
                where, sOnlineSession.raceCount);
        /* Nonzero code: strengthens the clean exit into a FAILURE the launcher's
         * online engine-session caller sees (liveResult != 0 -> stay in the Online
         * Room, panel surfaces recovery) rather than a normal finish. The thread3
         * loop honors the flag, the engine returns, teardown runs. Note the
         * ERROR reason so the launcher's session-end read surfaces it as ERROR (one
         * uniform channel + witness alongside the nonzero rc). */
        mdkr_party_link_note_session_end(MDKR_PARTY_LINK_SESSION_END_ERROR);
        platform_request_exit(2);
        return true;
    }
    sOnlineSession.desclessWaitTicks++;
    if (sOnlineSession.desclessWaitTicks > MDKR_ONLINE_SESSION_DESCLESS_WAIT_FRAME_BUDGET) {
        fprintf(stderr,
                "[online-session] descless wait TIMEOUT: exceeded %u-frame budget "
                "at %s (raceCount=%u) -- routing to abnormal exit (no hang)\n",
                (unsigned) MDKR_ONLINE_SESSION_DESCLESS_WAIT_FRAME_BUDGET, where,
                sOnlineSession.raceCount);
        /* The SAME platform-owned exit flag the online post-race path + the autoplay
         * tick budget use: the thread3 loop honors it, the engine returns, and
         * control comes back to the launcher's engine-session call (clean teardown /
         * residency exit). */
        platform_request_exit(0);
        return true;
    }
    return false;
}

/* Read ONE feed snapshot at RESULTS ENTER and derive BOTH the final-round flag
 * and the current 0-based cup round from it. Two separate mdkr_party_link_read()
 * calls would be TORN: a tournament wrap publishing BETWEEN them at a genuine
 * final makes isFinal read the pre-wrap race_index (final) while the round reads
 * the post-wrap 0 -- exactly the torn state that re-opens the strand item B
 * closes (finalWrap needs isFinal AND raceIndex+1>=CUP_ROUNDS) -- so both come
 * from the SAME snapshot. At RESULTS enter race_index still names the
 * just-finished race (before the host's REMATCH advances it).
 *
 *   isFinal  = race_index >= CUP_ROUNDS-1.
 *   raceIndex = race_index itself -- the TRUE cup round, NOT sRes.raceCount (the
 *     session's own boot count, which under-counts for a leader whose session
 *     joined a cup mid-way -- leader migration -- so raceCount-1 would read a
 *     genuine final as a non-final and take the purely-local FINISH leave that
 *     strands a second peer).
 *
 * Outside a configured tournament both are 0 (the chooser's TOURNAMENT gate
 * excludes the wrap anyway). */
static void online_session_feed_final_and_round(u8 *isFinal, u8 *raceIndex) {
    MdkrPartyLinkSnapshot snap;
    *isFinal = 0u;
    *raceIndex = 0u;
    if (mdkr_party_link_read(&snap) &&
        snap.mode == (uint8_t) MDKR_PARTY_LINK_MODE_TOURNAMENT &&
        snap.cup_id != MDKR_PARTY_LINK_CUP_UNSET) {
        *raceIndex = snap.race_index;
        *isFinal = (snap.race_index >= (u8) (MDKR_ONLINE_SESSION_CUP_ROUNDS - 1u))
                       ? 1u
                       : 0u;
    }
}

/* SINGLE-RACE "Race Again" auto-start publish. Publish a reverse-feed
 * intent, from the LOBBY_WAIT re-wait, that re-cycles the room to a fresh race
 * with NO human input: keep the local seat's persisted character + vehicle (so
 * the launcher planner's CHOOSE_CHARACTER / CHOOSE_VEHICLE stay CONVERGED no-ops),
 * re-assert READY (the REMATCH clear_round dropped it), and -- for the host seat --
 * request START. The host-only session-config fields stay at their UNSET sentinels
 * (via intent_init) so configured_track / mode / cup PERSIST -> race N+1 is the
 * SAME track. This is the engine half of the single-race re-cycle; the launcher's
 * resident coordinator is OBSERVE-ONLY for a single race (it just re-arms the
 * match-input on the fresh epoch), so this drive never collides with an auto-drive.
 * Only the HOST reaches this LOBBY_WAIT path on RACE AGAIN (a joiner follows to
 * CHARSELECT), but start_requested is gated on is_host anyway. */
static void online_session_publish_replay_autostart(void) {
    MdkrPartyLinkSnapshot snap;
    MdkrPartyLinkLocalIntent intent;
    unsigned i;
    if (!mdkr_party_link_read(&snap)) {
        return;
    }
    mdkr_party_link_intent_init(&intent);
    for (i = 0u; i < MDKR_PARTY_LINK_SEATS; i++) {
        if (snap.seats[i].occupied && snap.seats[i].is_local) {
            intent.hover_character = snap.seats[i].character_id;
            intent.vehicle_id = snap.seats[i].vehicle_id;
            intent.confirmed = 1u;
            intent.ready = 1u;
            intent.start_requested = snap.seats[i].is_host ? 1u : 0u;
            break;
        }
    }
    mdkr_party_link_intent_publish(&intent);
}

static void online_session_boot_race(void) {
    u16 intended;
    s32 manifestTrack;

    /* RE-FETCH the launch descriptor each boot. mdkr_online_session_begin
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

    /* RACE-1 READINESS GATE. When the session began descriptor-less, the
     * native CHARSELECT/TRACKSELECT ADVANCE (and LOBBY_WAIT's phase-left-LOBBY)
     * fire the instant the host START drives the room out of LOBBY -- several
     * frames-to-seconds before the launcher builds the descriptor, installs the
     * roster and arms the match-input source. Booting now would deref a NULL/stale
     * launch (:277). Instead: mark the room as committed to loading and re-enter
     * LOBBY_WAIT, whose descriptor-less re-wait boots exactly once the real
     * descriptor + match-input are live. This branch is inert for a descriptor-
     * first begin (beganWithoutDescriptor == 0), so every existing lane -- and the
     * !haveSnap direct-boot branch that reaches here -- is unchanged. */
    if (sOnlineSession.beganWithoutDescriptor &&
        !online_session_descless_boot_ready()) {
        if (!sOnlineSession.desclessBootPending) {
            sOnlineSession.desclessBootPending = 1u;
            /* the race-1 re-wait begins now -- arm its wall-clock deadline
             * fresh (single-endpoint only; inert otherwise). */
            sOnlineSession.desclessWaitTicks = 0u;
            online_session_descless_wallclock_arm();
            fprintf(stderr,
                    "[online-session] race-1 boot deferred: descriptor not ready "
                    "yet (lobby-start gate) -> LOBBY_WAIT re-wait\n");
        }
        sOnlineSession.phase = MDKR_ONLINE_SESSION_LOBBY_WAIT;
        return;
    }
    /* defensive NULL guard: never deref a NULL launch. Unreachable for a
     * descriptor-first begin (mode_intro validated non-NULL) and the gate above
     * guarantees non-NULL for the descriptor-less path, but the brief requires
     * every launch-> deref be guarded. */
    if (sOnlineSession.launch == NULL) {
        sOnlineSession.phase = MDKR_ONLINE_SESSION_LOBBY_WAIT;
        return;
    }

    intended = sOnlineSession.intendedTrack;
    manifestTrack = (s32) sOnlineSession.launch->manifest.track_id;
    sOnlineSession.bootedEpoch = sOnlineSession.launch->manifest.match_epoch;
    /* arm the live re-wait witness throttle so the NEXT round's re-wait logs
     * its first tick + ready changes afresh (0xFF != any real ready value). */
    sOnlineSession.liveReWaitLastReady = 0xFFu;
    /* a boot just fired, so the next descriptor-less wait (the per-round
     * live re-wait) starts its watchdog budget fresh. */
    sOnlineSession.desclessWaitTicks = 0u;
    /* also disarm the wall-clock deadline; the next descriptor-less wait
     * (RESULTS REMATCH-hold, then the per-round re-wait) re-arms it fresh. And clear
     * the per-round "left LOBBY" latch so the NEXT round's cancel detection starts
     * fresh. */
    sOnlineSession.desclessWaitDeadlineNs = 0u;
    sOnlineSession.desclessRoundLeftLobby = 0u;
    sOnlineSession.remoteAbsentTicks = 0u; /* fresh vacate debounce */

    sOnlineSession.raceCount++; /* count engine races booted this process */
    sOnlineSession.phase = MDKR_ONLINE_SESSION_RACE;
    /* Isolation witness: on the online path control reached the session
     * (gGameMode == GAMEMODE_ONLINE_SESSION) and the offline boot menu was
     * never loaded (gCurrentMenuId still 0, i.e. not MENU_BOOT). */
    /* the LOBBY_WAIT tick count is meaningful only for the FIRST boot; a
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

    /* observable agreement check. The race ALWAYS boots the manifest's
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
    /* Reuse the game's own race boot (extracted to online/online_race_boot.c);
     * it loads launch->manifest.track_id and sets gGameMode =
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
    sOnlineSession.lastModeSeen = 0xFFu; /* no forward-feed mode observed yet */
    sOnlineSession.liveReWaitLastReady = 0xFFu; /* re-wait witness unthrottled */
    sOnlineSession.desclessReWaitLastReady = 0xFFu; /* ditto */
    sOnlineSession.active = 1;
    /* Enter the SEPARATED mode. Offline code never produces this value, so the
     * offline menu state machine is never entered on this route. */
    gGameMode = GAMEMODE_ONLINE_SESSION;
    if (launch == NULL) {
        /* DESCRIPTOR-LESS begin (the party_link lobby-start path): no
         * launch descriptor exists yet -- the native online screens front
         * CHARSELECT -> TRACKSELECT and the host START builds it. The race-1
         * readiness gate (online_session_boot_race) holds the boot until the real
         * descriptor + roster + match-input are live, so the NULL launch stashed
         * here is never dereferenced. */
        sOnlineSession.beganWithoutDescriptor = 1u;
        /* latch whether the launcher drives a SINGLE local endpoint (a
         * real 2-process room) vs the two-adapter loopback. Selects the WALL-CLOCK
         * watchdog + error-signal + mid-tournament-cancel unwind path below. */
        sOnlineSession.singleEndpoint =
            mdkr_party_link_is_single_endpoint() ? 1u : 0u;
        fprintf(stderr,
                "[online-session] begin: lobby-start (no descriptor) "
                "(gGameMode=%d phase=LOBBY_WAIT singleEndpoint=%d); offline menu "
                "state machine bypassed\n",
                gGameMode, (int) sOnlineSession.singleEndpoint);
        return;
    }
    fprintf(stderr,
            "[online-session] begin: separated boot path entered "
            "(gGameMode=%d phase=LOBBY_WAIT); offline menu state machine "
            "bypassed\n",
            gGameMode);
}

/* True when the snapshot seats a resolvable LOCAL player -- the signal that the
 * room is a live selection room this endpoint is actually in, so the native
 * CHARSELECT screen should be shown rather than idling. The
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

/* the native VEHICLE stage of the track screen (AFTER the track lock -- the
 * retail order: charselect -> track browse -> lock -> vehicles -> OK -> race)
 * is ALWAYS part of the flow -- the player picks car/hovercraft/plane. Env
 * presence never removes it: a production run can no longer silently lose the
 * stage just because a test var happens to be set in the environment. */
static bool online_session_vehicleselect_enabled(void) {
    return true; /* the vehicle stage is always on */
}

/* the native "more races" chooser ALWAYS owns the RESULTS terminal. Env presence
 * never demotes it to the legacy terminal behaviour: a production run always gets
 * the replay menu, regardless of which test vars are set in the environment. */
static bool online_session_results_chooser_enabled(void) {
    return true; /* the results chooser is always on */
}

/* Short name of a committed "more races" choice, for the routing witness. */
static const char *online_session_chooser_name(MdkrOnlineResultsChoice choice) {
    switch (choice) {
    case MDKR_ONLINE_RESULTS_CHOICE_RACE_AGAIN:     return "race again";
    case MDKR_ONLINE_RESULTS_CHOICE_CHANGE_TRACK:   return "change track";
    case MDKR_ONLINE_RESULTS_CHOICE_CHANGE_CUP:     return "change cup";
    case MDKR_ONLINE_RESULTS_CHOICE_CHANGE_MODE:    return "change mode";
    case MDKR_ONLINE_RESULTS_CHOICE_NEW_TOURNAMENT: return "new tournament";
    case MDKR_ONLINE_RESULTS_CHOICE_CHANGE_CHAR:    return "change character";
    case MDKR_ONLINE_RESULTS_CHOICE_JOINER_FOLLOW:  return "joiner follow";
    default:                                        return "?";
    }
}

/* True once the LOCAL seat has both a locked character and its ready flag while
 * the room is still in LOBBY -- the signal that CHARSELECT is done and the
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

/* the CHARSELECT browse-B backout leave-to-room seam (env, resolved
 * once). Off in every normal run. */
static s8 sCharselectBackoutResolved = -1; /* -1 unresolved, 0 off, 1 on */
static bool online_session_charselect_backout_seam(void) {
    if (sCharselectBackoutResolved < 0) {
        sCharselectBackoutResolved =
            (getenv("MDKR_TEST_ONLINE_CHARSELECT_BACKOUT") != NULL) ? 1 : 0;
    }
    return sCharselectBackoutResolved > 0;
}

/* set-point B gate: honor a CHARSELECT browse-B as a genuine leave-to-room
 * ONLY for a live human (unscripted pad). The scripted lobby-start lanes press a
 * browse-B at tick 3 as the I1 no-wedge coverage and must keep the warn-once stub
 * (STAY) so they still confirm+ready+start race 1; the seam-armed charselect lane
 * is descriptor-first and already excluded by beganWithoutDescriptor. The
 * dedicated backout seam overrides the scripted-input suppression for the headless
 * proof of this LEFT path. */
static bool online_session_charselect_backout_honored(void) {
    if (online_session_charselect_backout_seam()) return true;
    return !mdkr_online_charselect_scripted_input_active();
}

/* force the remote-seat-vacated predicate for the headless proof
 * -- a real transport departure cannot be cheaply staged on the loopback rig, so
 * this seam makes the detector READ the remote as gone while the debounce + note
 * LEFT + exit(0) action all run genuinely. Off in every normal run. */
static s8 sRemoteVacateResolved = -1;
static bool online_session_remote_vacate_forced(void) {
    if (sRemoteVacateResolved < 0) {
        sRemoteVacateResolved =
            (getenv("MDKR_TEST_ONLINE_REMOTE_VACATE") != NULL) ? 1 : 0;
    }
    return sRemoteVacateResolved > 0;
}

/* Remote-vacate probe (env MDKR_TEST_ONLINE_REMOTE_VACATE_AT_RESULTS_FINAL):
 * force the remote-seat-vacated predicate ONLY once the FINAL standings are latched
 * (resultsIsFinal). This is scoped to resultsIsFinal so it stays inert during the
 * non-final results screens (the cup proceeds normally) and only reads the remote
 * as gone at the terminal -- exactly the condition the gate now guards. Without
 * the gate the detector would trip -> LEFT; with the gate the detector is never
 * called at the final standings, so the dwell -> FINISHED wins. Off in every normal
 * run (resolved once). */
static s8 sRemoteVacateFinalResolved = -1;
static bool online_session_remote_vacate_final_forced(void) {
    if (sRemoteVacateFinalResolved < 0) {
        sRemoteVacateFinalResolved =
            (getenv("MDKR_TEST_ONLINE_REMOTE_VACATE_AT_RESULTS_FINAL") != NULL)
                ? 1 : 0;
    }
    return sRemoteVacateFinalResolved > 0 && sOnlineSession.resultsIsFinal;
}

/* SCREEN-SCOPED remote-vacate probe (env MDKR_TEST_ONLINE_REMOTE_VACATE_AT=<screen>,
 * e.g. "trackselect"): force the remote-seat-vacated predicate ONLY on the named
 * native screen, so the session can REACH that screen (e.g. a joiner past
 * CHARSELECT/VEHICLESELECT to TRACKSELECT) before the host is read as gone. The
 * unscoped MDKR_TEST_ONLINE_REMOTE_VACATE trips at the FIRST detector call
 * (charselect); this scopes it to the target screen for the per-screen vacate
 * proofs. Off in every normal run (the env compares against the caller's `where`).
 * A real transport departure cannot be cheaply staged on the loopback rig; the
 * debounce + note LEFT + exit(0) action all run genuinely. */
static const char *online_session_remote_vacate_at_screen(void) {
    static s8 resolved = -1;
    static const char *screen = NULL;
    if (resolved < 0) {
        screen = getenv("MDKR_TEST_ONLINE_REMOTE_VACATE_AT");
        resolved = 1;
    }
    return screen;
}
static bool online_session_remote_vacate_forced_at(const char *where) {
    const char *screen = online_session_remote_vacate_at_screen();
    return screen != NULL && screen[0] != '\0' && where != NULL &&
           strcmp(screen, where) == 0;
}

/* UNCAPTURED-latch seam (env MDKR_TEST_ONLINE_CEREMONY_UNCAPTURED): hand the
 * ceremony NULL even though a final ranking WAS captured -- standing in for the
 * real WAN ordering window where the host's wrap State lands before this
 * endpoint's FIRST RESULTS-phase capture tick (finalRankingCaptured genuinely 0
 * at ceremony enter). The downstream condition is exercised for REAL: the
 * ceremony's fallback live-compute runs over the already-wrapped feed (the
 * faithful stand-in wrap zeroes the points and departs RESULTS), which is
 * exactly what it must refuse to crown. Off in every normal run. */
static s8 sCeremonyUncapturedResolved = -1;
static bool online_session_ceremony_uncaptured_seam(void) {
    if (sCeremonyUncapturedResolved < 0) {
        sCeremonyUncapturedResolved =
            (getenv("MDKR_TEST_ONLINE_CEREMONY_UNCAPTURED") != NULL) ? 1 : 0;
    }
    return sCeremonyUncapturedResolved > 0;
}

/* Any occupied seat that is NOT the local player -- the remote(s) still present in
 * the room. Mirrors online_session_snapshot_has_local_seat. */
static bool online_session_snapshot_has_remote_seat(
    const MdkrPartyLinkSnapshot *snap) {
    unsigned i;
    for (i = 0u; i < MDKR_PARTY_LINK_SEATS; i++) {
        if (snap->seats[i].occupied && !snap->seats[i].is_local) {
            return true;
        }
    }
    return false;
}

/* remote-vacated detector. While a DESCRIPTOR-LESS session waits
 * on a native screen, a remote seat that VACATES (the peer left / the room
 * dissolved) means the round can never proceed -- parking would be indefinite
 * (only app-quit exits otherwise). Read the forward feed: if a room that is still
 * in an ACTIVE lobby-side phase (LOBBY, pre-START, or RESULTS, the post-race hold)
 * still seats the LOCAL player but NO remote seat is occupied, DEBOUNCE it (a
 * one-frame transient must never trip), and on a sustained absence note LEFT +
 * platform_request_exit(0) so the launcher returns cleanly to the room. Gated on
 * beganWithoutDescriptor, so a descriptor-first lane (whose remote stays seated
 * through selection) never trips. Returns true (exit requested) on a trip.
 *
 * extended to accept the RESULTS phase too, so the four native
 * screens are symmetric (CHARSELECT/TRACKSELECT/CEREMONY already catch a remote
 * departure; RESULTS was the lone screen with no graceful vacate exit). In the
 * NORMAL tournament end the host parks in RESULTS with its seat OCCUPIED and keeps
 * pumping, so this stays inert there and the joiner's INDEPENDENT terminal
 * self-advance (online_results.c) drives it to CEREMONY -> FINISHED; only a
 * genuine sustained host/remote departure during RESULTS trips this -> LEFT. */
#define MDKR_ONLINE_SESSION_REMOTE_VACATE_DEBOUNCE 45u
static bool online_session_detect_remote_vacated(const char *where) {
    MdkrPartyLinkSnapshot snap;
    if (!sOnlineSession.beganWithoutDescriptor) {
        return false;
    }
    if (!mdkr_party_link_read(&snap) ||
        (snap.phase != (uint8_t) MDKR_ONLINE_SESSION_LOBBY_PHASE &&
         snap.phase != (uint8_t) MDKR_ONLINE_SESSION_RESULTS_PHASE) ||
        !online_session_snapshot_has_local_seat(&snap)) {
        sOnlineSession.remoteAbsentTicks = 0u;
        return false;
    }
    if (online_session_snapshot_has_remote_seat(&snap) &&
        !online_session_remote_vacate_forced() &&
        !online_session_remote_vacate_final_forced() &&
        !online_session_remote_vacate_forced_at(where)) {
        sOnlineSession.remoteAbsentTicks = 0u; /* remote back / present: reset */
        return false;
    }
    sOnlineSession.remoteAbsentTicks++;
    if (sOnlineSession.remoteAbsentTicks <
        MDKR_ONLINE_SESSION_REMOTE_VACATE_DEBOUNCE) {
        return false; /* still within the debounce window */
    }
    mdkr_party_link_note_session_end(MDKR_PARTY_LINK_SESSION_END_LEFT);
    fprintf(stderr,
            "[online-session] LEFT: remote seat vacated at %s "
            "(debounced %u ticks) -> return to room (exit 0)\n",
            where, (unsigned) sOnlineSession.remoteAbsentTicks);
    platform_request_exit(0);
    return true;
}

/* Session-end + residency contract (the seams are explicit here):
 *   - LIVE play is resident: OnlineRoom_pumpPartyLink / the reverse intent feed
 *     pump from the overlay-service hook during residency, results are published
 *     mid-residency, the roster re-freezes/cycles per round, and the live-path
 *     platform_request_exit(0) becomes the resident return. Residency is gated
 *     behind MDKR_TEST_ONLINE_RESIDENT so only the scripted soak re-enters and
 *     every live lane still exits unchanged.
 *   - The champion CEREMONY: a native 2D celebration of the cup champion
 *     (online_ceremony.c) that the final-standings FINISH detours into before the
 *     FINISHED handshake -- a bounded, auto-advancing screen that falls into the
 *     SAME FINISHED note + exit(0) below (fired exactly once, after the ceremony).
 *     No 3D cutscene / offline trophy cinematic is entered.
 *   - The engine->launcher return handshake: a RESULTS FINISH, a CHARSELECT
 *     backout, a pre-START remote-vacate, or a mid-tournament cancel notes an end
 *     reason (mdkr_party_link_note_session_end) + platform_request_exit, which the
 *     launcher takes (mdkr_party_link_take_session_end) after the boot returns to
 *     resume the Online Room. The warn-once CHARSELECT stub remains for the
 *     descriptor-first / scripted lanes (byte-behaviour-unchanged).
 *   - ABANDON_RACE is NOT needed: abnormal ends keep platform_request_exit(0),
 *     which resume_results below preserves by returning false when no finish was
 *     captured. */

/* THE PEER-LOSS CLEAN RETURN -- the crash fix (race-start AND mid-race).
 *
 * The engine's rollback runtime (rollback_game_runtime.c) reports a RECOVERABLE
 * online-input starvation from TWO tick-loop sites:
 *   - validate_boundary at the FIRST authored boundary, when the peer/bootstrap
 *     input never arrived (peer LOST at race start / ICE failed and the launcher's
 *     race-start barrier aborted the tick-1 drain); and
 *   - prepare_tick MID-RACE, when the launcher input provider cannot supply an
 *     authored tick (a peer/console that dropped cleanly mid-race), or a rewound
 *     correction's peer input is gone.
 * Either is a RECOVERABLE peer loss, NOT rollback invariant corruption; the engine
 * tick loop (thread3_main.c, beta-gated) used to abort() the whole app on both and
 * crash BOTH machines. Route them instead to the SAME clean return-to-room every
 * other abnormal online end uses (see the remote-vacate + watchdog paths above):
 *   1. note the session end -- LEFT, because the peer left (the launcher's
 *      onlineTakeSessionEndWitness then logs [online-session-end] reason=LEFT and
 *      resumes the Online Room, exactly as the remote-vacate LEFT path does);
 *   2. tear the pinned rollback runtime down cleanly here -- mirror
 *      unload_level_game()'s mdkr_rollback_game_runtime_level_end() so the
 *      recoverable unwind strands no authority allocation / snapshot ring (this is
 *      idempotent with the engine-shutdown level_end in main_pc.c, which also runs
 *      when a level is still loaded at exit);
 *   3. request the platform exit(0) -- the SAME clean engine-exit the online
 *      post-race path uses; the thread3 loop honors it and returns.
 * Called ONLY from the engine prepare_tick / validate_boundary failure handlers,
 * ONLY when the failure was the recoverable online-input kind
 * (mdkr_rollback_game_runtime_online_input_recoverable()). Both roles reach the
 * identical path: the host (peer=joiner lost) and the joiner (peer=host
 * lost/crashed) both starve an authored boundary. */
void mdkr_online_session_return_to_room_on_peer_loss(void) {
    mdkr_party_link_note_session_end(MDKR_PARTY_LINK_SESSION_END_LEFT);
    fprintf(stderr,
            "[online-session] LEFT: online peer/input lost (recoverable boundary "
            "starvation) -> return to room (exit 0)\n");
    /* Release the pinned rollback assets/authority + snapshot ring now, so the
     * recoverable unwind leaks nothing (idempotent with engine-shutdown teardown). */
    mdkr_rollback_game_runtime_level_end();
    platform_request_exit(0);
}

/* post-race RE-ENTRY. Called from the online
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
    /* LIVE residency (the real behaviour now): the launcher pump PUBLISH_-
     * RESULTS mid-residency, so the reducer snapshot shows RESULTS for this race.
     * Resume on that REAL signal (not an env), reading placements/points from the
     * snapshot. This is what makes a live session resident across races. */
    {
        MdkrPartyLinkSnapshot snap;
        if (mdkr_party_link_read(&snap) &&
            snap.phase == (uint8_t) MDKR_ONLINE_SESSION_RESULTS_PHASE) {
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
        return false; /* abnormal end / nothing captured: caller exits */
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

bool mdkr_online_session_postrace_results_retry(void) {
    /* Read by the post-race hook (menu.c) AFTER the race ended -- the session
     * struct persists across the in-session race (boot_race clears only
     * `active`), so begin()'s latches are still authoritative here. TRUE only
     * for the descriptor-less SINGLE-ENDPOINT session (a real 2-process room):
     * there the RESULTS resume signal is the reducer snapshot, which needs the
     * LEADER's PUBLISH_RESULTS to round-trip the real network. Every loopback
     * descriptor-less lane is two-adapter (singleEndpoint 0) or publishes the
     * reducer RESULTS synchronously before the post-race grace elapses, so the
     * historical one-shot decision stays byte-timed everywhere else. */
    return sOnlineSession.beganWithoutDescriptor != 0u &&
           sOnlineSession.singleEndpoint != 0u;
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

        /* DESCRIPTOR-LESS race-1 re-wait. The native CHARSELECT/
         * TRACKSELECT fronted and the host START drove the room out of LOBBY, so
         * boot_race requested a boot the readiness gate refused (descriptor not
         * live yet) -- it set desclessBootPending and parked us here. Boot race 1
         * ONLY once the launcher's real descriptor + roster + match-input are live
         * on a fresh epoch (bootedEpoch == 0), mirroring the resident re-wait
         * below. Because it is gated on desclessBootPending it NEVER preempts the
         * initial LOBBY_WAIT -> CHARSELECT hand-off (which runs while pending == 0
         * -- see the !haveSnap / snapPhase logic further down). Disjoint from the
         * resident re-wait (raceCount > 0) and inert without the latch. */
        if (sOnlineSession.beganWithoutDescriptor &&
            sOnlineSession.desclessBootPending &&
            sOnlineSession.raceCount == 0u) {
            const MdkrMatchLaunchDescriptorV1 *launch =
                mdkr_net_roster_runtime_launch_descriptor();
            bool ready = online_session_descless_boot_ready();
            MdkrPartyLinkSnapshot rsnap;
            bool haveReSnap = mdkr_party_link_read(&rsnap);
            if (haveReSnap) {
                online_session_stash_intended(&rsnap);
            }
            /* UNWIND. The boot was deferred because the host
             * STARTed (room left LOBBY) but the descriptor was not live yet. If the
             * LEADER now CANCELs loading (RETURN_TO_LOBBY -> CANCEL_LOADING,
             * lobby_core.c:660 / match_live_adapter.cpp:670) the room returns to
             * LOBBY -- so the descriptor will NEVER become ready on this epoch and
             * parking here forever would brick the session. Detect the room back in
             * LOBBY (with our seat still present) while not ready, CLEAR the pending
             * boot, and re-front the native CHARSELECT so the player can re-select /
             * re-START. (The descless re-wait fires only after the room left LOBBY,
             * so a LOBBY snapshot here is unambiguously a cancel, never the initial
             * hand-off.) */
            if (!ready && haveReSnap &&
                rsnap.phase == (uint8_t) MDKR_ONLINE_SESSION_LOBBY_PHASE &&
                online_session_snapshot_has_local_seat(&rsnap)) {
                sOnlineSession.desclessBootPending = 0u;
                sOnlineSession.desclessReWaitLastReady = 0xFFu;
                sOnlineSession.desclessWaitTicks = 0u;
                sOnlineSession.desclessWaitDeadlineNs = 0u;
                sOnlineSession.phase = MDKR_ONLINE_SESSION_CHARSELECT;
                sCharselectLeaveWarned = 0u;
                mdkr_online_charselect_enter();
                fprintf(stderr,
                        "[online-session] lobby-start UNWIND: room returned to LOBBY "
                        "while boot pending (leader CANCEL_LOADING) -> re-fronting "
                        "CHARSELECT (tick=%u)\n",
                        sOnlineSession.lobbyWaitTicks);
                break;
            }
            if (sOnlineSession.desclessReWaitLastReady != (u8) ready) {
                fprintf(stderr,
                        "[online-session] phase=LOBBY_WAIT (lobby-start re-wait) "
                        "tick=%u epoch=%u bootedEpoch=%u ready=%d\n",
                        sOnlineSession.lobbyWaitTicks,
                        (unsigned) (launch != NULL ? launch->manifest.match_epoch
                                                   : 0u),
                        (unsigned) sOnlineSession.bootedEpoch, (int) ready);
                sOnlineSession.desclessReWaitLastReady = (u8) ready;
            }
            sOnlineSession.lobbyWaitTicks++;
            if (ready) {
                online_session_boot_race();
            } else if (online_session_descless_watchdog_tick("race-1 re-wait")) {
                /* Watchdog tripped (descriptor never built): exit requested. */
                break;
            }
            break;
        }

        /* LIVE residency re-wait: after a RESULTS advance we re-entered
         * LOBBY_WAIT and the launcher is re-cycling the room (REMATCH -> re-Ready
         * -> START) to a fresh race-ready transport. Boot the next race ONLY once
         * a NEW roster/descriptor is installed (match_epoch advanced past the
         * just-raced one) AND the match-input runtime is live on that epoch. Idle
         * (keeping intended-track observability fresh) otherwise. This is disjoint
         * from the first boot (raceCount 0) and from the scripted soak (which
         * re-boots inline off the RESULTS advance, never through here). */
        if (sOnlineSession.liveResident && sOnlineSession.raceCount > 0u) {
            /* The readiness predicate is the shared helper (the same one the race-1
             * re-wait uses); the local launch fetch is kept only for the witness
             * log below. */
            const MdkrMatchLaunchDescriptorV1 *launch =
                mdkr_net_roster_runtime_launch_descriptor();
            bool ready = online_session_descless_boot_ready();
            {
                MdkrPartyLinkSnapshot rsnap;
                if (mdkr_party_link_read(&rsnap)) {
                    online_session_stash_intended(&rsnap);
                    /* MID-TOURNAMENT CANCEL unwind.
                     * SINGLE-ENDPOINT only: rounds 2..N re-cycle the room from the
                     * launcher (LOBBY -> LOADING -> race-ready). If the LEADER cancels
                     * mid-tournament (RETURN_TO_LOBBY -> CANCEL_LOADING) the room
                     * regresses LOADING -> LOBBY and the next race's descriptor/epoch
                     * never arms -- parking would leave only watchdog-bounded death.
                     * Latch "left LOBBY this round" first (so the initial next-round
                     * SELECTING the re-wait observes is NOT mistaken for a cancel),
                     * then, on a return to LOBBY with our seat still present while not
                     * ready, note LEFT + platform_request_exit(0): a CLEAN engine->
                     * launcher return-to-room, replacing the earlier re-front
                     * that dropped the human into the doomed 900-frame advance budget
                     * (a bounded ERROR exit(2)). Gated on singleEndpoint, so the
                     * loopback resident/lobby-tournament lanes are byte-unchanged. */
                    if (sOnlineSession.singleEndpoint) {
                        if (rsnap.phase !=
                            (uint8_t) MDKR_ONLINE_SESSION_LOBBY_PHASE) {
                            sOnlineSession.desclessRoundLeftLobby = 1u;
                        } else if (sOnlineSession.desclessRoundLeftLobby && !ready &&
                                   online_session_snapshot_has_local_seat(&rsnap)) {
                            mdkr_party_link_note_session_end(
                                MDKR_PARTY_LINK_SESSION_END_LEFT);
                            fprintf(stderr,
                                    "[online-session] mid-tournament UNWIND: room "
                                    "regressed to LOBBY during the per-round re-wait "
                                    "(leader CANCEL_LOADING) -> LEFT: return to room "
                                    "(exit 0) (race=%u tick=%u)\n",
                                    sOnlineSession.raceCount,
                                    sOnlineSession.lobbyWaitTicks);
                            platform_request_exit(0);
                            break;
                        }
                    }
                    /* SINGLE-RACE "Race Again" auto-start. No native screen
                     * re-fronted for RACE AGAIN, so drive the re-Ready + host-START
                     * from here: while the room is still in LOBBY, publish the
                     * keep-selection/ready/start intent each tick (the launcher's
                     * reverse pump lands READY then START -> BEGIN_LOADING on the
                     * SAME configured track); once the room has LEFT LOBBY (loading
                     * committed) stop + clear the latch so it never leaks into a
                     * later CHANGE-picks re-cycle. Inert unless RACE AGAIN armed it. */
                    if (sOnlineSession.replayAutoStart) {
                        if (rsnap.phase ==
                            (uint8_t) MDKR_ONLINE_SESSION_LOBBY_PHASE) {
                            online_session_publish_replay_autostart();
                        } else {
                            sOnlineSession.replayAutoStart = 0u;
                        }
                    }
                }
            }
            /* throttle to the first re-wait tick + every ready-state change,
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
            } else if (sOnlineSession.beganWithoutDescriptor &&
                       online_session_descless_watchdog_tick("per-round re-wait")) {
                /* a DESCRIPTOR-LESS session's per-round re-cycle never
                 * re-armed the next race (the launcher's REMATCH re-cycle wedged) --
                 * bound it + exit cleanly. Gated on beganWithoutDescriptor, so the
                 * resident lane (descriptor-first) is byte-behaviour-unchanged. */
                break;
            }
            break;
        }

        /* Dormant unless a headless test seam is enabled. */
        online_session_test_maybe_script();
        /* inert unless the CHARSELECT headless seam is armed; then it
         * installs the forward feed and publishes a scripted LOBBY room. */
        mdkr_online_charselect_test_lobby_pump();
        /* inert unless the TRACKSELECT headless seam is armed AND nothing
         * else installed the feed (the CHARSELECT seam owns install in the
         * combined lane); a defensive standalone install otherwise. */
        mdkr_online_trackselect_test_lobby_pump();
        /* inert unless the VEHICLESELECT headless seam is armed AND nothing else
         * installed the feed (the CHARSELECT seam owns install in the combined
         * vehicle lane); a defensive standalone install otherwise. */
        mdkr_online_vehicleselect_test_lobby_pump();

        haveSnap = mdkr_party_link_read(&snap);
        if (haveSnap) {
            online_session_stash_intended(&snap); /* observability */
        }
        if (!haveSnap) {
            /* No live forward feed installed (legacy direct-boot / the launcher
             * is not yet publishing): the validated launch descriptor IS the
             * room's agreement to start, so boot now. This keeps
             * check_online_engine_boot_direct green. */
            readyToBoot = true;
        } else if (snap.phase == (uint8_t) MDKR_ONLINE_SESSION_LOBBY_PHASE) {
            /* Room is still selecting. Show the native CHARSELECT once a local
             * seat is resolvable; otherwise keep idling (the session-boot
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
        /* pre-START remote-vacated detector. If the remote seat
         * leaves the LOBBY-phase room while we wait on CHARSELECT, note LEFT + exit
         * (debounced). Inert for a descriptor-first begin. Free the screen assets on
         * the trip (symmetry with the backout / FINISH exit-free paths). */
        if (online_session_detect_remote_vacated("charselect")) {
            mdkr_online_charselect_exit();
            break;
        }
        {
            /* track the host-intended pick as the room converges.
             * Stashing PRE-tick is correct here (unlike TRACKSELECT, which reads
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
             * is the safety path and the historical CHARSELECT-lane
             * hand-off: boot the race directly. */
            mdkr_online_charselect_exit();
            online_session_boot_race();
        } else if (r == MDKR_ONLINE_CHARSELECT_STAY &&
                   mdkr_online_charselect_local_ready()) {
            /* once the local seat is confirmed+ready while the room
             * is still in LOBBY, hand off to the native TRACKSELECT screen --
             * the RETAIL order: the track browse comes right after PLAYER
             * SELECT, and the vehicle pick is a later STAGE of that track
             * screen (after the lock). Gate on the SCREEN's own confirmed+ready
             * latch AS WELL AS the snapshot's ready flag. The snapshot ready lags
             * un-ready by >=1 pump after a back-out, so relying on it alone would
             * one-frame flash charselect and re-advance; the screen latch resets
             * immediately on _enter(), so requiring it keeps the player on charselect
             * until they genuinely re-confirm+re-ready. */
            MdkrPartyLinkSnapshot snap;
            if (mdkr_party_link_read(&snap) &&
                online_session_local_seat_ready_in_lobby(&snap)) {
                mdkr_online_charselect_exit();
                sOnlineSession.phase = MDKR_ONLINE_SESSION_TRACKSELECT;
                mdkr_online_trackselect_enter();
                fprintf(stderr,
                        "[online-session] charselect -> trackselect (local "
                        "seat ready in LOBBY)\n");
            }
        } else if (r == MDKR_ONLINE_CHARSELECT_LEAVE) {
            if (sOnlineSession.beganWithoutDescriptor &&
                online_session_charselect_backout_honored()) {
                /* LEFT handshake: a genuine browse-B backout on the
                 * descriptor-less path returns to the launcher room. mdkr_online_
                 * charselect_exit frees the screen assets; note LEFT + request the
                 * clean platform exit so the launcher reads the reason + resumes. */
                mdkr_online_charselect_exit();
                mdkr_party_link_note_session_end(
                    MDKR_PARTY_LINK_SESSION_END_LEFT);
                fprintf(stderr,
                        "[online-session] LEFT: charselect backout -> return to "
                        "room (exit 0)\n");
                platform_request_exit(0);
            } else if (!sCharselectLeaveWarned) {
                /* Non-descriptor-less begin, or the scripted lobby-start lanes'
                 * tick-3 I1 no-wedge browse-B: keep the warn-once stub (STAY) so
                 * the seam-armed charselect lane and the loopback lobby-start lanes
                 * are byte-behaviour-unchanged. The tick returns LEAVE as an edge
                 * (ADVANCE always wins), so this can never wedge the session -- a
                 * host-start still boots this endpoint. */
                sCharselectLeaveWarned = 1u;
                fprintf(stderr,
                        "[online-charselect] leave requested; engine->launcher "
                        "return handled by the launcher, staying on screen\n");
            }
        }
        break;
    }
    case MDKR_ONLINE_SESSION_VEHICLESELECT: {
        MdkrOnlineVehicleselectResult r;
        /* pre-START remote-vacated detector (also covers the VEHICLESELECT wait).
         * Inert for a descriptor-first begin. Free the screen assets on the trip
         * (symmetry with the other exit-free paths). */
        if (online_session_detect_remote_vacated("vehicleselect")) {
            mdkr_online_vehicleselect_exit();
            break;
        }
        {
            /* track the host-intended pick as the room converges. Stashing
             * PRE-tick is correct (like CHARSELECT, unlike TRACKSELECT): the
             * VEHICLE screen reduces NO host session config, so its tick cannot
             * change the intended track -- the pre-tick snapshot is the freshest. */
            MdkrPartyLinkSnapshot vsSnap;
            if (mdkr_party_link_read(&vsSnap)) {
                online_session_stash_intended(&vsSnap);
            }
        }
        r = mdkr_online_vehicleselect_tick(updateRate);
        if (r == MDKR_ONLINE_VEHICLESELECT_ADVANCE) {
            /* The authoritative lobby left LOBBY (the host's OK started loading)
             * -- the vehicle stage is the LAST selection stop in the retail
             * order, so this is the normal race hand-off now. */
            mdkr_online_vehicleselect_exit();
            online_session_boot_race();
        } else if (r == MDKR_ONLINE_VEHICLESELECT_LEAVE) {
            /* B on the vehicle stage is a clean "back one stage" to the track
             * BROWSE (retail: backing out of the setup stage returns to the
             * browse frame; the lock latch resets on _enter, so re-advancing
             * requires a fresh lock). Same intra-screen presentation: skip the
             * reveal fade. Do NOT reset sCharselectLeaveWarned (continuation of
             * the same session). */
            mdkr_online_vehicleselect_exit();
            sOnlineSession.phase = MDKR_ONLINE_SESSION_TRACKSELECT;
            mdkr_online_screen_fade_skip_once();
            mdkr_online_trackselect_enter();
            fprintf(stderr,
                    "[online-session] vehicleselect -> trackselect (back one "
                    "stage)\n");
        }
        break;
    }
    case MDKR_ONLINE_SESSION_TRACKSELECT: {
        MdkrOnlineTrackselectResult r;
        /* pre-START remote-vacated detector (also covers the
         * TRACKSELECT wait). Inert for a descriptor-first begin. Free the screen
         * assets on the trip (symmetry with the other exit-free paths). */
        if (online_session_detect_remote_vacated("trackselect")) {
            mdkr_online_trackselect_exit();
            break;
        }
        r = mdkr_online_trackselect_tick(updateRate);
        {
            /* read AFTER the tick so the host's just-reduced config
             * (SET_CONFIG_TRACK / SET_CUP) is captured before any boot. */
            MdkrPartyLinkSnapshot tsSnap;
            if (mdkr_party_link_read(&tsSnap)) {
                online_session_stash_intended(&tsSnap);
            }
        }
        if (r == MDKR_ONLINE_TRACKSELECT_ADVANCE) {
            /* The room left LOBBY under the browse stage (a safety path: the
             * OK normally lands while the vehicle stage fronts) -> boot. */
            mdkr_online_trackselect_exit();
            online_session_boot_race();
        } else if (r == MDKR_ONLINE_TRACKSELECT_STAY &&
                   online_session_vehicleselect_enabled() &&
                   mdkr_online_trackselect_setup_ready()) {
            /* The browse stage is done -- the host LOCKED a track/cup (or the
             * joiner observed the host's lock): flip to the VEHICLE stage of
             * the track screen, the RETAIL order (track first, vehicles after,
             * on the same screen -- so the reveal fade is skipped; the ground
             * carries over). The setup latch resets on trackselect _enter, so a
             * B-back from the vehicle stage re-requires a fresh lock (no
             * one-frame bounce). */
            mdkr_online_trackselect_exit();
            sOnlineSession.phase = MDKR_ONLINE_SESSION_VEHICLESELECT;
            mdkr_online_screen_fade_skip_once();
            mdkr_online_vehicleselect_enter();
            fprintf(stderr,
                    "[online-session] trackselect -> vehicleselect (track "
                    "locked: retail vehicle stage)\n");
        } else if (r == MDKR_ONLINE_TRACKSELECT_LEAVE) {
            /* B on the track BROWSE is a clean "back one level" to CHARSELECT
             * (the retail back-stack: track screen -> PLAYER SELECT).
             * Deliberately do NOT reset sCharselectLeaveWarned: this is a
             * continuation of the same session, so the leave-to-launcher
             * warn-once latch is preserved. */
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
        /* the native RESULTS/STANDINGS screen, re-entered by the resident
         * post-race fork (mdkr_online_session_resume_results). Enter it once on
         * the first tick after the resume (the enter polls this race's captured
         * placements); the screen renders placements from the poll + the cup
         * points from the party_link snapshot. */
        MdkrOnlineResultsResult r;
        /* RESULTS remote-vacate detector (mirrors charselect :1069 /
         * trackselect :1147), so a genuine host/remote departure or a stale feed
         * DURING a NON-FINAL results screen is caught gracefully (debounced, notes
         * LEFT + exits) instead of parking -- there is still a next round to
         * coordinate, so a vanished remote means the round can never proceed.
         *
         * GATE this on !resultsIsFinal. At
         * the FINAL standings the match is COMPLETE -- nothing remains to coordinate
         * -- and the joiner has EARNED its FINISHED + champion ceremony. Running the
         * detector there let a 45-tick (0.75s) remote-vacate PRE-EMPT the joiner's
         * ~10s self-advance dwell (online_results.c) if the host lingered/dropped
         * during the dwell, mis-reporting an earned FINISHED as LEFT. The final
         * standings are owned solely by that already-bounded dwell -> CEREMONY ->
         * FINISHED (no hang), so the detector is confined to the non-final screens
         * where a vacate is genuinely unrecoverable. */
        if (!sOnlineSession.resultsIsFinal &&
            online_session_detect_remote_vacated("results")) {
            mdkr_online_results_exit();
            break;
        }
        if (sOnlineSession.resultsPending) {
            online_session_resident_resolve();
            /* A fresh RESULTS decision: drop any stale single-race auto-start latch
             * (defensive -- it is normally cleared when the prior re-cycle left
             * LOBBY) so this terminal's chooser choice, not the last one, decides. */
            sOnlineSession.replayAutoStart = 0u;
            /* Drop the PREVIOUS race's ranking latch too. Single-race sessions
             * capture at EVERY race (each single race is its own decision point),
             * so without this a race-N wrap landing before race N's first
             * RESULTS-phase capture tick would hand the ceremony race N-1's STALE
             * table (captured=1 from the previous race) and silently crown the
             * wrong winner -- the ceremony's refusal only guards the UNCAPTURED
             * window. A tournament captures only at its final, where this clear
             * is a no-op. */
            sOnlineSession.finalRankingCaptured = 0u;
            /* LIVE residency: FREE the just-finished race level NOW, on
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
                 * on screen and the autoplay tick budget ends the process. A
                 * descriptor-less (interactive) session has no resident env, so
                 * size finality + the round from the FEED (ONE snapshot -> both,
                 * torn-read safe); the ENV path stays FIRST so the two resident
                 * lanes are byte-unchanged. The round must be the FEED's true cup
                 * round, not raceCount-1 (the session's own boot count, which
                 * under-counts for a mid-cup-joined leader -- see the helper). */
                u8 isFinal;
                u8 raceIndex;
                if (sOnlineSession.beganWithoutDescriptor) {
                    online_session_feed_final_and_round(&isFinal, &raceIndex);
                } else {
                    isFinal = (sOnlineSession.raceCount >= sResidentRaces) ? 1u
                                                                          : 0u;
                    raceIndex = (sOnlineSession.raceCount > 0u)
                                    ? (u8) (sOnlineSession.raceCount - 1u)
                                    : 0u;
                }
                /* latch finality NOW (race_index still names this race) so
                 * the ADVANCE decision below is not fooled by the REMATCH advancing
                 * race_index before the screen returns ADVANCE. */
                sOnlineSession.resultsIsFinal = isFinal;
                mdkr_online_results_enter(
                    isFinal, raceIndex,
                    online_session_results_chooser_enabled() ? 1u : 0u);
            }
            sOnlineSession.resultsPending = 0u;
            /* the RESULTS-phase REMATCH-convergence hold is the
             * THIRD descriptor-less wait. After the host commits the advance the
             * screen republishes REMATCH and HOLDS "STARTING NEXT RACE..." until the
             * snapshot leaves RESULTS -- on a real WAN a link drop mid-RESULTS (after
             * PUBLISH_RESULTS, before the REMATCH lands) would park the engine in
             * RESULTS forever. Arm the wall-clock deadline at RESULTS enter so this
             * hold is bounded too (single-endpoint only; the frame-count loopback
             * lanes never enter this branch -> unchanged). */
            sOnlineSession.desclessWaitDeadlineNs = 0u;
            online_session_descless_wallclock_arm();
        }
        r = mdkr_online_results_tick(updateRate);
        /* while a SESSION DECISION POINT is shown with BOTH seats still present,
         * (re)capture the COMPLETE ranking with the SAME shared sort the STANDINGS
         * render runs (DRY). Two decision points capture: the tournament FINAL
         * standings (resultsIsFinal) and EVERY single-race RESULTS (each single
         * race fronts its own chooser whose FINISH ceremonies from this latch --
         * points are structurally 0 there, so the shared sort ranks by this
         * race's placements). The champion CEREMONY celebrates THIS captured
         * winner rather than recomputing from a live snapshot that may have lost
         * the host seat to a genuine disconnect during the dwell -- so a departed
         * host can never re-crown the surviving (possibly losing) joiner from a
         * 1-seat snapshot. Recomputing each 2-seat tick is cheap and always keeps
         * the freshest last-known-good ordering; the LAST such capture (both
         * present) is what the RESULTS->CEREMONY transition below hands to the
         * ceremony. */
        {
            MdkrPartyLinkSnapshot fsnap;
            /* phase gate: capture ONLY while the room is still authoritatively
             * in RESULTS. A committed FINISH now dispatches the REMATCH wrap
             * (RESULTS -> LOBBY; at the tournament final also
             * reset_tournament_series, in single-race a placement clear) BEFORE
             * the screen returns LEAVE, so on the convergence tick the live
             * snapshot already carries the WRAPPED table -- an ungated recapture
             * here would overwrite the true ranking with it one tick before the
             * ceremony reads it (crowning by seat order). Both seats remain
             * present across the wrap, so the seat-presence checks alone cannot
             * stop that. */
            if (mdkr_party_link_read(&fsnap) &&
                fsnap.phase == (uint8_t) MDKR_ONLINE_SESSION_RESULTS_PHASE &&
                (sOnlineSession.resultsIsFinal ||
                 fsnap.mode == (uint8_t) MDKR_PARTY_LINK_MODE_SINGLE) &&
                online_session_snapshot_has_local_seat(&fsnap) &&
                online_session_snapshot_has_remote_seat(&fsnap)) {
                MdkrOnlineStandings ranked;
                mdkr_online_standings_compute(&fsnap, true, &ranked);
                if (ranked.count >= 2u) {
                    sOnlineSession.finalRanking = ranked;
                    sOnlineSession.finalRankingCaptured = 1u;
                }
            }
        }
        /* bound the RESULTS hold. The results screen returns STAY
         * during its countdown and the post-commit REMATCH-convergence hold; the
         * wall-clock deadline (armed at enter) covers both. On a non-final race a
         * wedge after PUBLISH_RESULTS (REMATCH never lands) trips it -> route to
         * a clean ERROR exit rather than a silent RESULTS hang. The final
         * STANDINGS legitimately hold (resultsIsFinal), so do NOT bound that.
         * The SINGLE-RACE chooser DECISION POINT (fronted, undecided) holds
         * interactively too -- the host may deliberate forever and the joiner
         * mirror deliberately waits on the host (its own exits: the observed
         * wrap, a vanished host, a confirmed B) -- so it gets the SAME unbounded
         * discipline the final standings get; each deciding tick REFRESHES the
         * wall-clock budget so a later committed choice still gets its full
         * convergence window (the deadline is wall-clock, so merely skipping the
         * watchdog would let the deliberation eat the commit's budget). A
         * committed non-FINISH choice (deciding false) is a genuine convergence
         * hold and stays bounded; a committed FINISH takes its own wrap hold
         * below. Single-endpoint only. */
        if (sOnlineSession.singleEndpoint && !sOnlineSession.resultsIsFinal &&
            r == MDKR_ONLINE_RESULTS_STAY) {
            if (mdkr_online_results_chooser_deciding()) {
                online_session_descless_wallclock_arm();
            } else if (mdkr_online_results_choice() !=
                           MDKR_ONLINE_RESULTS_CHOICE_FINISH &&
                       online_session_descless_watchdog_tick(
                           "results rematch-hold")) {
                break;
            }
        }
        /* the committed-FINISH WRAP hold gets its own bound (tournament final
         * AND the single-race chooser). The decision point itself legitimately
         * holds un-bounded (above) -- but once the HOST has COMMITTED FINISH the
         * screen is only waiting for its own REMATCH wrap to land, and a WAN
         * drop there must surface as a bounded ERROR, not a silent park. Arm a
         * FRESH wall-clock budget on the first committed-FINISH STAY tick (the
         * pre-commit deliberation is never counted), then run the shared
         * watchdog. A STAY with the FINISH choice latched happens ONLY on the
         * wrap roads (the excluded direct-leave shapes return LEAVE on the
         * commit tick), so no resultsIsFinal gate is needed. Single-endpoint
         * only; the loopback lanes converge the wrap synchronously and never
         * accumulate this hold. */
        if (sOnlineSession.singleEndpoint && r == MDKR_ONLINE_RESULTS_STAY &&
            mdkr_online_results_choice() == MDKR_ONLINE_RESULTS_CHOICE_FINISH) {
            if (!sOnlineSession.finishWrapHoldArmed) {
                sOnlineSession.finishWrapHoldArmed = 1u;
                sOnlineSession.desclessWaitDeadlineNs = 0u;
                online_session_descless_wallclock_arm();
            } else if (online_session_descless_watchdog_tick(
                           "FINISH wrap-hold")) {
                break;
            }
        }
        if (r == MDKR_ONLINE_RESULTS_ADVANCE) {
            /* Exit-symmetry: free the RESULTS screen on the ADVANCE
             * return ITSELF, unconditionally -- every other screen exits on its
             * non-STAY return, and this was the one _exit call gated on a second
             * predicate (shouldAdvance). Idempotent (sRes.assets latch), so a
             * later re-exit is harmless; closes the (currently unreachable)
             * ADVANCE-while-!shouldAdvance busy-return that would re-tick a loaded
             * screen forever. */
            mdkr_online_results_exit();
            {
                /* "more races" chooser routing. When the host committed a replay
                 * option (or a joiner followed the host), the screen returned ADVANCE
                 * only once its REMATCH drove the room out of RESULTS -> LOBBY. Route
                 * back to the right native screen so the host re-locks the new config
                 * over the existing SET_* feed / re-races the same config. choice ==
                 * NONE for every pre-existing ADVANCE (the tournament round re-cycle /
                 * scripted soak), which falls through to the historical path below
                 * BYTE-FOR-BYTE unchanged. */
                MdkrOnlineResultsChoice choice = mdkr_online_results_choice();
                if (choice != MDKR_ONLINE_RESULTS_CHOICE_NONE) {
                    switch (mdkr_online_results_choice_refront(choice)) {
                    case MDKR_ONLINE_RESULTS_REFRONT_TRACKSELECT:
                        /* re-front TRACKSELECT: the host re-locks the new track/cup/
                         * mode over SET_CONFIG_TRACK / SET_CUP / SET_MODE (SET_CUP +
                         * SET_MODE reset the tournament series to round 1). */
                        if (!sOnlineSession.liveResident) {
                            unload_level_game();
                        }
                        sOnlineSession.phase = MDKR_ONLINE_SESSION_TRACKSELECT;
                        mdkr_online_trackselect_enter();
                        fprintf(stderr,
                                "[online-session] results -> trackselect (chooser: "
                                "%s)\n",
                                online_session_chooser_name(choice));
                        break;
                    case MDKR_ONLINE_RESULTS_REFRONT_CHARSELECT:
                        /* re-front CHARSELECT (-> TRACKSELECT -> vehicle stage):
                         * change character + vehicle between races. The display-only joiner
                         * follows here too -- CHARSELECT is the safe universal
                         * re-selection entry that mirrors the host's config downstream. */
                        if (!sOnlineSession.liveResident) {
                            unload_level_game();
                        }
                        sOnlineSession.phase = MDKR_ONLINE_SESSION_CHARSELECT;
                        sCharselectLeaveWarned = 0u;
                        mdkr_online_charselect_enter();
                        fprintf(stderr,
                                "[online-session] results -> charselect (chooser: "
                                "%s)\n",
                                online_session_chooser_name(choice));
                        break;
                    case MDKR_ONLINE_RESULTS_REFRONT_SAME:
                    default:
                        /* re-race the SAME config. The in-process single-race
                         * re-cycle re-enters LOBBY_WAIT and, because there is NO
                         * native selection screen to drive the re-Ready + host-START,
                         * arms replayAutoStart so the live re-wait auto-publishes that
                         * intent each tick (the launcher's reverse pump re-cycles the
                         * room to a fresh race-ready transport, its resident
                         * coordinator observe-only re-arms the match-input, and the
                         * re-wait boots race N+1). The scripted soak (no transport)
                         * re-boots inline off the frozen descriptor. */
                        if (sOnlineSession.liveResident) {
                            sOnlineSession.phase = MDKR_ONLINE_SESSION_LOBBY_WAIT;
                            sOnlineSession.replayAutoStart = 1u;
                            sOnlineSession.desclessWaitDeadlineNs = 0u;
                            online_session_descless_wallclock_arm();
                            fprintf(stderr,
                                    "[online-session] results -> re-race same config "
                                    "(chooser: race again; LIVE single-race re-cycle: "
                                    "LOBBY_WAIT auto-start armed)\n");
                        } else {
                            unload_level_game();
                            fprintf(stderr,
                                    "[online-session] results -> re-race same config "
                                    "(chooser: race again)\n");
                            online_session_boot_race();
                        }
                        break;
                    }
                    break; /* chooser routing handled -- skip the pre-existing path */
                }
            }
            /* the RESULTS->next-race decision. A DESCRIPTOR-LESS
             * (lobby-start) session has no MDKR_APP_TEST_ONLINE_LIVE_RESIDENT env
             * (sResidentRaces == 0), so `raceCount < sResidentRaces` is always false
             * and the session would DEAD-END at RESULTS -- even though
             * online_session_feed_isfinal() promises "the host advances via REMATCH".
             * Drive finality from the FEED instead: advance while NOT feed-final
             * (tournament race_index < CUP_ROUNDS-1). The ENV-sized predicate stays
             * EXACTLY as-is for a descriptor-first begin, so the two resident lanes
             * are byte-behaviour-unchanged. (At feed-final the RESULTS screen already
             * holds -- returns no ADVANCE -- so this is also a belt-and-braces gate.) */
            u8 shouldAdvance =
                sOnlineSession.beganWithoutDescriptor
                    ? (u8) (sOnlineSession.resultsIsFinal ? 0u : 1u)
                    : ((sOnlineSession.raceCount < sResidentRaces) ? 1u : 0u);
            if (shouldAdvance) {
                /* Re-boot the NEXT race IN THIS SAME ENGINE PROCESS -- the
                 * load-bearing residency proof (>=2 direct boots). */
                if (!sOnlineSession.liveResident) {
                    /* Scripted soak held the level resident through RESULTS, so
                     * free it now before the next boot's load_level_game -- the
                     * same "leave the current race level" call the offline
                     * race->race path makes. (The LIVE lane already freed it on
                     * RESULTS entry, above.) */
                    unload_level_game();
                }
                if (sOnlineSession.liveResident) {
                    /* LIVE residency: do NOT boot inline -- the RESULTS
                     * host-advance drove the reducer's REMATCH via the reverse
                     * feed, but the launcher must still re-Ready + START the room
                     * and re-install a fresh roster + match-input source (new
                     * match_epoch) before the next race can run on the real
                     * transport. Re-enter LOBBY_WAIT, whose live re-wait gate boots
                     * once that fresh descriptor + match-input are live. */
                    sOnlineSession.phase = MDKR_ONLINE_SESSION_LOBBY_WAIT;
                    /* the per-round re-wait begins now -- arm its own fresh
                     * wall-clock budget (single-endpoint only), separate from the
                     * RESULTS hold's budget just consumed. */
                    sOnlineSession.desclessWaitDeadlineNs = 0u;
                    online_session_descless_wallclock_arm();
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
            /* else: the cup/soak is complete -- feed-final (descriptor-less) or
             * raceCount == sResidentRaces (env-sized). The screen returned isFinal,
             * so it holds the final standings; nothing to boot. */
        } else if (r == MDKR_ONLINE_RESULTS_LEAVE) {
            /* engine->launcher FINISH/RETURN handshake. The results screen
             * returns LEAVE for the host's
             * "A: FINISH" on the FINAL standings (resultsIsFinal), the converged
             * SINGLE-RACE chooser FINISH wrap (resultsIsFinal is 0 there -- a
             * single race is never the feed-final -- so the screen's own latched
             * verdict routes it), or a non-final B-back. Free the screen assets,
             * note the reason, and request the clean platform exit so the
             * launcher reads the reason + resumes the Online Room. */
            u8 singleFinish = mdkr_online_results_single_finish();
            mdkr_online_results_exit();
            if (sOnlineSession.resultsIsFinal || singleFinish) {
                /* DETOUR: the match is over (the tournament's final standings,
                 * or a single race whose host chose FINISH), so instead of the
                 * FINISHED handshake firing HERE, run the native CEREMONY first.
                 * The FINISHED note + platform_request_exit(0) below moved INTACT
                 * into the CEREMONY case, so it still fires EXACTLY ONCE (with the
                 * same reason/result the launcher reads) once the celebration ends.
                 * The ceremony crowns the ranking CAPTURED above (the SAME shared
                 * sort the standings/results showed, while both seats were
                 * present), so its champion agrees with that screen even if the
                 * winner's seat then departs. */
                sOnlineSession.ceremonySingle = singleFinish;
                sOnlineSession.phase = MDKR_ONLINE_SESSION_CEREMONY;
                /* hand the ceremony the ranking captured while both seats
                 * were present. If nothing was captured (a disconnect so early the
                 * final standings never latched with two present -- or, over the
                 * WAN, the host's wrap State landing before this endpoint's FIRST
                 * RESULTS-phase capture tick), pass NULL and the ceremony falls
                 * back to a live compute, which refuses to crown a lone survivor
                 * AND refuses to crown once the room has already left RESULTS
                 * (the wrap resets the live points -- online_ceremony.c). The
                 * UNCAPTURED seam below stages exactly that ordering window
                 * headlessly (a NULL handoff over an already-wrapped feed);
                 * inert in every normal run. */
                mdkr_online_ceremony_enter(
                    (sOnlineSession.finalRankingCaptured &&
                     !online_session_ceremony_uncaptured_seam())
                        ? &sOnlineSession.finalRanking
                        : NULL);
                /* Mode-truthful witness: the tournament line is byte-identical
                 * to the pre-existing one (lanes pin it); the single-race road
                 * names itself (a single race has no "final standings"). */
                fprintf(stderr, "%s",
                        singleFinish
                            ? "[online-session] phase=CEREMONY: single-race "
                              "FINISH -> race-winner celebration (FINISHED "
                              "deferred until it ends)\n"
                            : "[online-session] phase=CEREMONY: final standings "
                              "A:FINISH -> champion celebration (FINISHED "
                              "deferred until it ends)\n");
            } else {
                /* Non-final local back-out = a mid-tournament LEFT. (The scripted
                 * resident lanes never press B, so this fires only for a live
                 * human / a descriptor-less back-out.) The ceremony is FINAL-only,
                 * so this path is unchanged: it exits immediately. */
                mdkr_party_link_note_session_end(
                    MDKR_PARTY_LINK_SESSION_END_LEFT);
                fprintf(stderr,
                        "[online-session] LEFT: non-final results back-out -> "
                        "return to room (exit 0)\n");
                platform_request_exit(0);
            }
        }
        break;
    }
    case MDKR_ONLINE_SESSION_CEREMONY: {
        /* The native champion celebration, entered from the final-standings
         * FINISH above. It is a bounded, auto-advancing screen (timer fires for
         * every endpoint; a host may skip early; a remote vacate ends it promptly),
         * so it can NEVER hang the session. When it ends (ADVANCE/LEAVE), fall into
         * the FINISHED handshake -- freed screen assets, one FINISHED note, one
         * platform_request_exit(0). */
        MdkrOnlineCeremonyResult r = mdkr_online_ceremony_tick(updateRate);
        if (r != MDKR_ONLINE_CEREMONY_STAY) {
            mdkr_online_ceremony_exit();
            mdkr_party_link_note_session_end(
                MDKR_PARTY_LINK_SESSION_END_FINISHED);
            /* Mode-truthful witness (see the CEREMONY-enter note above): the
             * tournament line stays byte-identical for the lanes that pin it. */
            fprintf(stderr, "%s",
                    sOnlineSession.ceremonySingle
                        ? "[online-session] FINISHED: single-race FINISH -> "
                          "return to room (exit 0)\n"
                        : "[online-session] FINISHED: final standings A:FINISH -> "
                          "return to room (exit 0)\n");
            platform_request_exit(0);
        }
        break;
    }
    default:
        /* Every phase is handled above; this stays a defensive no-op. */
        break;
    }
}

#endif /* MDKR_ENABLE_ONLINE_BETA */
