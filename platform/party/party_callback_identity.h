#ifndef MDKR_PARTY_CALLBACK_IDENTITY_H
#define MDKR_PARTY_CALLBACK_IDENTITY_H

#include <cstdint>

inline bool mdkr_party_peer_initialization_current(bool initializing,
                                                  uint64_t liveGeneration,
                                                  uint64_t admissionGeneration) noexcept {
    return !initializing || admissionGeneration == 0u || admissionGeneration == liveGeneration;
}

// A forced retry is a delayed decision about one particular peer, never a
// license to replace whichever peer now happens to occupy the controller ID.
inline bool mdkr_party_retry_owner_current(bool forceRecreate, bool expectedPeerMatches,
                                          bool stillEligible, bool offerUnchanged) noexcept {
    return !forceRecreate || (expectedPeerMatches && stillEligible && offerUnchanged);
}

inline bool mdkr_party_ping_timeout_current(bool timeoutDecision, bool sameOutstandingPing) noexcept {
    return !timeoutDecision || sameOutstandingPing;
}

// A zero signaling generation deliberately means transport/peer lifetime:
// healthy direct controller channels survive a signaling socket reconnect.
// Callers compute peerMatches and apply this predicate under their state lock.
inline bool mdkr_party_callback_current(bool stopping, uint64_t liveGeneration,
                                       uint64_t observedGeneration,
                                       bool peerMatches) noexcept {
    return !stopping && peerMatches &&
        (observedGeneration == 0u || observedGeneration == liveGeneration);
}

#endif
