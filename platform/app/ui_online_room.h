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
bool OnlineRoom_isLobbyTakeoverActive();

// Current MdkrOnlineViewKind (0 when no live session), for the takeover probe.
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
// teardown -- never blocking the UI thread on WebRTC/WebSocket close -- resets
// the session UI to the chooser and returns the shell to the launcher home.
void OnlineRoom_serviceLobbyLeave(LauncherState &state);

// ORDERED app-exit teardown of the live room adapter, called by the launcher
// once its main loop has ended (before main returns). The adapter owns mesh /
// signal-client worker threads whose callbacks keep firing until its destructor
// joins them; leaving destruction to static teardown lets those threads race
// destroyed globals (observed: an uncaught "mutex lock failed" SIGABRT when the
// app was quit while a live room was up). Destroys the adapter INLINE on the
// calling thread -- blocking on the join is fine at app exit -- after retracting
// both engine registries, exactly like the async leave path. No-op without a
// live adapter.
void OnlineRoom_shutdownForAppExit();
#endif  // MDKR_ENABLE_ONLINE_BETA

#endif
