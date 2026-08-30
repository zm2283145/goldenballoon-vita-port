#ifndef MDKR_ONLINE_SESSION_H
#define MDKR_ONLINE_SESSION_H

/* SEPARATED-BOOT-PATH (Strategy D) online session skeleton.
 *
 * This is the whole reason the online campaign can promise the battle-tested
 * OFFLINE game is provably unimpacted: online is a FULLY SEPARATE boot path.
 * mode_intro forks straight into GAMEMODE_ONLINE_SESSION (never the offline
 * menu state machine), the top-level dispatch in main_game_loop() routes that
 * mode to mdkr_online_session_tick(), and the session -- holding ALL of its own
 * state, never any offline global -- drives the flow to the race. The offline
 * GAMEMODE_MENU / gCurrentMenuId path is never entered on this route.
 *
 * The ENTIRE header is #if MDKR_ENABLE_ONLINE_BETA so a normal (beta OFF) build
 * sees nothing here, and the file is compiled into the engine ONLY under the
 * beta CMake gate (game/src/online/ is NOT auto-globbed). Include is safe
 * without the macro: the body just vanishes.
 */
#if MDKR_ENABLE_ONLINE_BETA

#include "types.h"
#include "thread3_main.h"                 /* GameMode, GAMEMODE_UNUSED_2 */
#include "net/match_launch_descriptor.h"  /* MdkrMatchLaunchDescriptorV1 */

#ifdef __cplusplus
extern "C" {
#endif

/* Beta alias for the online session mode. It REUSES the dead offline slot value
 * 2 (GAMEMODE_UNUSED_2) WITHOUT editing the shared GameMode enum in
 * thread3_main.h. The offline state machine never produces this value (verified:
 * nothing in game/src assigns GAMEMODE_UNUSED_2), so the online session mode is
 * ONLY ever reachable through the beta mode_intro fork -- the isolation
 * cornerstone. Defined here, in the beta-only header, not in the shared enum. */
#define GAMEMODE_ONLINE_SESSION GAMEMODE_UNUSED_2

/* The online session flow, held in session-owned state (never an offline
 * global). Each phase drives one native screen or the race hand-off. */
typedef enum MdkrOnlineSessionPhase {
    MDKR_ONLINE_SESSION_LOBBY_WAIT = 0, /* read party_link; wait for boot signal */
    MDKR_ONLINE_SESSION_CHARSELECT,     /* native character select */
    MDKR_ONLINE_SESSION_TRACKSELECT,    /* native track select */
    MDKR_ONLINE_SESSION_RACE,           /* hand off to the in-game race boot */
    MDKR_ONLINE_SESSION_RESULTS,        /* results */
    MDKR_ONLINE_SESSION_CEREMONY,       /* tournament ceremony */
    /* Appended (NOT inserted) so no live enumerator value shifts: the native
     * VEHICLE stage of the track screen (game/src/online/online_vehicleselect.c).
     * The flow order is the RETAIL one -- CHARSELECT -> TRACKSELECT (browse +
     * lock) -> VEHICLESELECT (the track screen's setup stage) -> race -- driven
     * by the switch + transitions in online_session.c, NOT by this enum's
     * numeric order. Beta-only by construction (the whole header is
     * #if MDKR_ENABLE_ONLINE_BETA). */
    MDKR_ONLINE_SESSION_VEHICLESELECT   /* native vehicle select (car/hover/plane) */
} MdkrOnlineSessionPhase;

/* Enter the separated online boot path. Called from the (already beta-gated)
 * mode_intro fork with the validated launch descriptor: stashes it in
 * session-owned state and sets gGameMode = GAMEMODE_ONLINE_SESSION. It does NOT
 * touch any offline menu state. */
void mdkr_online_session_begin(const MdkrMatchLaunchDescriptorV1 *launch);

/* Per-frame tick for GAMEMODE_ONLINE_SESSION, dispatched from main_game_loop().
 * LOBBY_WAIT idles on the party_link forward feed until the room leaves
 * selection, then hands off to the race boot (which sets GAMEMODE_INGAME). */
void mdkr_online_session_tick(s32 updateRate);

/* The direct online race boot (mdkr_online_boot_direct_race) was extracted in
 * into game/src/online/online_race_boot.{c,h}; online_session.c includes
 * that header and calls it for the RACE hand-off. */

/* post-race RE-ENTRY. Called from the online
 * post-race hook (menu.c) when the grace period elapses. Re-arms the session
 * into its RESULTS phase in THIS engine process and returns true ONLY when
 * resident mode is on (env MDKR_TEST_ONLINE_RESIDENT, set only by the scripted
 * soak) AND this race captured a finish order. Returns false for every live lane
 * (resident OFF) and every abnormal end (no captured results), so the caller
 * keeps calling platform_request_exit(0) exactly as today -- zero live-lane
 * behaviour change. */
bool mdkr_online_session_resume_results(void);

/* True when the current session began DESCRIPTOR-LESS in SINGLE-ENDPOINT mode
 * (the production room-ready takeover over a real 2-process room). The
 * post-race hook (menu.c) uses it to pick the resume discipline: there the
 * RESULTS resume signal is the reducer snapshot, whose arrival needs the
 * leader's PUBLISH_RESULTS to round-trip the real network (observed slower
 * than the 2.5 s post-race grace), so the hook RETRIES the resume up to a
 * bounded window instead of deciding once. Every other path -- descriptor-first
 * boots, the scripted soaks, and the two-adapter loopback descriptor-less
 * lanes (whose resume signal is synchronous/process-local) -- keeps the
 * historical one-shot decision, byte-for-byte. Persists across the in-session
 * race (begin() set the latches; nothing clears them until the next begin). */
bool mdkr_online_session_postrace_results_retry(void);

/* PEER-LOSS CRASH FIX: route a RECOVERABLE race-start (or mid-race) peer loss to a clean
 * return-to-room instead of abort()ing the app. Called from the engine tick loop
 * (thread3_main.c, beta-gated) ONLY when the boundary validator reports the
 * recoverable online-input starvation (mdkr_rollback_game_runtime_online_input_-
 * recoverable()): notes the session end (LEFT), tears the rollback runtime down
 * cleanly, and requests platform_request_exit(0) so the launcher resumes the room. */
void mdkr_online_session_return_to_room_on_peer_loss(void);

/* THE LOCAL LEAVE: the non-blocking online pause overlay's "LEAVE RACE" action
 * (game/src/online/online_race_pause.c). Notes the session end (LEFT -- the
 * truthful reason: this player left), tears the rollback runtime down cleanly
 * and requests platform_request_exit(0), exactly the peer-loss return above. */
void mdkr_online_session_leave_race(void);

#ifdef __cplusplus
}
#endif

#endif /* MDKR_ENABLE_ONLINE_BETA */
#endif /* MDKR_ONLINE_SESSION_H */
