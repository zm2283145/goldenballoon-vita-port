/*
 * O-T6 production transport backends for the native LIVE lobby adapter.
 *
 * Two seams the O-T3 adapter injects, here backed by the real MatchRoom service
 * instead of the in-process doubles the unit test uses:
 *
 *  - MdkrOnlineRoomTransport: a real HTTP client for the MatchRoom lobby routes
 *    (POST create / join / code / command / rotate) plus the authenticated
 *    /connect state WebSocket (credential carried only in the
 *    `gb-match.{credential}` subprotocol, never the URL), parsing each pushed
 *    room-state frame into the MdkrOnlineLobby snapshots the adapter pumps. The
 *    iceServers, 22-char roomId, endpoint credential and endpoint id are taken
 *    from the create/join response and the first /connect frame.
 *
 *  - MdkrOnlineMeshSignalBackend: wraps the O-T1 MdkrMatchSignalClient behind
 *    the MdkrMatchSignalClientFeed adapter, so the O-T2 peer mesh runs real
 *    offer/answer/ICE over /api/match/{roomId}/signal.
 *
 * Same-origin/TLS posture mirrors the party transport and the O-T1 signal
 * client: https/wss speak TLS against the embedded Mozilla CA bundle with
 * hostname verification; http/ws plaintext is accepted ONLY behind the loopback
 * test token (mdkr_party_loopback_test_url_allowed). A production build with no
 * token can never reach a plaintext origin.
 *
 * Threading follows the MdkrPartyTransport / MdkrMatchSignalClient contract:
 * every method here is a launcher-thread call; one owned worker thread does the
 * blocking HTTP/WS I/O and copies observations into a bounded event queue the
 * launcher drains through pump().
 */
#ifndef MDKR_MATCH_LIVE_TRANSPORT_H
#define MDKR_MATCH_LIVE_TRANSPORT_H

#include "online/match_live_adapter.h"

#include <memory>
#include <string>

/* The MatchRoom HTTP + /connect client. Construction never touches the network:
 * the first begin* call issues the create/join POST. Returns nullptr with
 * *error set if the origin is refused (cross-origin plaintext without the
 * loopback token). */
std::unique_ptr<MdkrOnlineRoomTransport> mdkr_online_room_http_transport_create(
    const std::string &origin, std::string *error = nullptr);

/* The invite + identity the transport learned at create/join, so the O-T6 race
 * driver can narrate the room code the second process joins by. Thread-safe;
 * returns false until the Ready snapshot has been produced. fallbackCode and
 * inviteUrl are populated for a creator only. */
struct MdkrOnlineRoomHttpInvite {
    bool ready = false;
    std::string roomId;      /* 22-char base64url */
    std::string credential;  /* 43-char base64url endpoint bearer */
    std::string endpointId;  /* decimal u64 */
    std::string fallbackCode; /* 6 digits (creator only) */
    std::string inviteUrl;   /* /room/#match=<43> capability (creator only) */
};
bool mdkr_online_room_http_transport_invite(MdkrOnlineRoomTransport *transport,
                                            MdkrOnlineRoomHttpInvite *out);

/* The real-signal-client mesh backend. `origin` is the same service origin the
 * room transport uses; each beginSignaling() opens a fresh authenticated
 * /signal socket for the local endpoint and adopts the service-assigned
 * connection generation from the welcome. */
std::unique_ptr<MdkrOnlineMeshSignalBackend>
mdkr_online_mesh_signal_backend_create(const std::string &origin);

#endif /* MDKR_MATCH_LIVE_TRANSPORT_H */
