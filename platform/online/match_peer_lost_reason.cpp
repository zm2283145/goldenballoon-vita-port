/* The peer-loss reason name table, kept out of the mesh TU so a test or
 * tool can read a recorded reason without linking the WebRTC transport. */
#include "online/match_peer_transport.h"

const char *mdkr_match_peer_lost_reason_name(MdkrMatchPeerLostReason reason) {
    switch (reason) {
        case MdkrMatchPeerLostReason::ConnectTimeout: return "connect_timeout";
        case MdkrMatchPeerLostReason::PingTimeout: return "ping_timeout";
        case MdkrMatchPeerLostReason::SealWindowExhausted:
            return "seal_window_exhausted";
        case MdkrMatchPeerLostReason::ControlChannelViolation:
            return "control_channel_violation";
        case MdkrMatchPeerLostReason::CommitmentMismatch:
            return "commitment_mismatch";
        case MdkrMatchPeerLostReason::HelloViolation: return "hello_violation";
        case MdkrMatchPeerLostReason::PeerEnded: return "peer_ended";
        case MdkrMatchPeerLostReason::TransportFailed:
            return "transport_failed";
        case MdkrMatchPeerLostReason::PeerVanished: return "peer_vanished";
    }
    return "unknown";
}
