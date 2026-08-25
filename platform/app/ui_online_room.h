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
#endif  // MDKR_ENABLE_ONLINE_BETA

#endif
