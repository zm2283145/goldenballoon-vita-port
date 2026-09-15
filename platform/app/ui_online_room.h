#ifndef MDKR64_UI_ONLINE_ROOM_H
#define MDKR64_UI_ONLINE_ROOM_H

struct LauncherAction;
struct LauncherState;

void OnlineRoomPanel_draw(LauncherState &state, LauncherAction &action);

// Machine-readable, windowless scenario inventory for render-test discovery.
// Returns zero after writing one versioned row per deterministic gallery case.
int OnlineRoom_dumpGalleryContract();

// Exact-action witness for token-gated production-input smoke scripts.
bool OnlineRoom_smokeActionResult(unsigned action, bool *accepted);

#if MDKR_ENABLE_ONLINE_BETA
// Native online beta only. True while a live online session has progressed past
// the entry/chooser (connecting, room, preflight, selecting, loading, countdown,
// racing, results or recovery). The launcher shell uses this to render ONLY the
// full-screen lobby and suppress the nav rail, top tabs and the generic offline
// Play button, so an offline launch is unreachable during an online session.
// Retains takeover when an adapter is owned but its view cannot be composed;
// presentation failure is not teardown, and the safe Leave Room stays usable.
bool OnlineRoom_isLobbyTakeoverActive();

// Current MdkrOnlineViewKind (0 when absent or unavailable), for the probe.
int OnlineRoom_lobbyProbeViewKind();

// Player-slot / status snapshot for the persistent lobby header. Fills the
// current member/ready/seat counts, whether this display leads the room, and a
// plain-language status line derived from the view kind ("Waiting for the other
// player…", "Racing", …) -- never a raw wire code. False when no live session.
struct OnlineLobbyHeaderInfo {
    int memberCount;
    int readyCount;
    int seatCount;
    bool localIsLeader;
    const char *statusLine;
};
bool OnlineRoom_lobbyHeaderInfo(OnlineLobbyHeaderInfo *out);

// Request the single, clean "Leave Race" exit (deferred). The actual teardown
// runs in OnlineRoom_serviceLobbyLeave after the frame's lobby body has drawn.
void OnlineRoom_requestLeave();

// Called by the takeover AFTER the lobby body draws. If a leave was requested or
// the body navigated home, it hands the live adapter to a background thread for
// teardown, resets the session UI to the chooser and returns home. Allocation
// or worker-launch failure instead cleans up synchronously to retain ownership
// safety; it is not a guarantee of non-blocking transport close in that case.
void OnlineRoom_serviceLobbyLeave(LauncherState &state);

// Irreversible normal-Quit phase, begun only after Workshop work settles.
// Retracts engine handoffs and normally retires the panel adapter off-thread.
// Poll while pumping/drawing closing progress; true means all tracked handles
// are joined. Scheduling refusal retains the safe synchronous fallback.
void OnlineRoom_beginAppExit();
bool OnlineRoom_pollAppExit();

// ORDERED app-exit teardown of the live room adapter, called by the launcher
// once its main loop has ended (before main returns). The adapter owns mesh /
// signal-client owners; leaving their destruction to static teardown lets workers race
// destroyed globals (observed: an uncaught "mutex lock failed" SIGABRT when the
// app was quit while a live room was up). Normal Quit uses begin/poll above;
// this final backstop also covers renderer-failure exits that cannot draw.
// Destroys any remaining panel-owned adapter INLINE on the calling thread
// after retracting both engine registries, exactly like the async leave path,
// AND drains the tracked background teardown threads an in-session Leave may
// have spawned moments earlier (teardownAdapterAsync). A slow drain is reported
// after 10 seconds, but workers are always joined before returning; the deadline
// does not guarantee bounded exit or permit detached access to destroyed globals.
// This joins tracked adapter retirement, not all asynchronous libdatachannel
// cleanup or OS resolver work; those require their own lifecycle qualification.
// No-op with neither
// a live adapter nor an in-flight teardown.
void OnlineRoom_shutdownForAppExit();
#endif  // MDKR_ENABLE_ONLINE_BETA

#endif
