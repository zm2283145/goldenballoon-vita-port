#ifndef MDKR_PARTY_PEER_SETUP_RETRY_H
#define MDKR_PARTY_PEER_SETUP_RETRY_H

#include <cstdint>
#include <limits>

// Per admitted controller lifecycle, separate from offers actually sent. Three
// setup failures exhaust recovery; the first two wait 300ms and 600ms. A normal
// roster refresh, signaling reconnect or duplicate hello never resets this.
// Consuming the existing three-offer budget can exhaust earlier: an offer
// emitted before later setup failure must not authorize a fourth fresh offer.
struct MdkrPartyPeerSetupRetry {
    unsigned failures = 0u;
    unsigned offerAttempts = 0u;
    uint64_t revision = 0u;
    uint64_t retryAtMs = 0u;
    bool active = false;
    bool exhausted = false;
    bool exhaustionReported = false;
};

inline uint64_t mdkr_party_setup_after(uint64_t nowMs, uint64_t delayMs) noexcept {
    const auto limit = std::numeric_limits<uint64_t>::max();
    return nowMs > limit - delayMs ? limit : nowMs + delayMs;
}

inline bool mdkr_party_setup_due(const MdkrPartyPeerSetupRetry &state, uint64_t nowMs) noexcept {
    return !state.active && !state.exhausted && state.retryAtMs != 0u && nowMs >= state.retryAtMs;
}

inline uint64_t mdkr_party_setup_begin(MdkrPartyPeerSetupRetry &state, uint64_t nowMs,
                                      uint64_t uniqueRevision) noexcept {
    if (uniqueRevision == 0u || state.active || state.exhausted ||
        (state.retryAtMs != 0u && nowMs < state.retryAtMs)) return 0u;
    if (state.offerAttempts >= 3u) {
        state.exhausted = true;
        state.retryAtMs = 0u;
        return 0u;
    }
    // Caller supplies a transport-wide serial, so removal and re-addition of
    // an otherwise identical controller tuple cannot recreate an old token.
    state.revision = uniqueRevision;
    state.active = true;
    state.retryAtMs = 0u;
    return state.revision;
}

inline bool mdkr_party_setup_owned(const MdkrPartyPeerSetupRetry &state, uint64_t revision) noexcept {
    return revision != 0u && state.active && state.revision == revision;
}

inline void mdkr_party_setup_failed(MdkrPartyPeerSetupRetry &state, uint64_t revision,
                                   uint64_t nowMs) noexcept {
    if (!mdkr_party_setup_owned(state, revision)) return;
    state.active = false;
    if (state.failures < 3u) ++state.failures;
    state.exhausted = state.failures >= 3u || state.offerAttempts >= 3u;
    state.retryAtMs = state.exhausted ? 0u : mdkr_party_setup_after(nowMs, 300u << (state.failures - 1u));
}

inline void mdkr_party_setup_succeeded(MdkrPartyPeerSetupRetry &state, uint64_t revision) noexcept {
    if (!mdkr_party_setup_owned(state, revision)) return;
    state.active = false;
    state.retryAtMs = 0u;
    // Keep the failure budget and offer counter for this controller lifecycle.
}

inline void mdkr_party_setup_reauthorize(MdkrPartyPeerSetupRetry &state, uint64_t nowMs) noexcept {
    if (!state.active) return;
    state.active = false;
    if (++state.revision == 0u) ++state.revision;
    // A retired epoch is not a setup failure. Still avoid immediate churn when
    // a current room snapshot reauthorizes its interrupted construction.
    if (!state.exhausted) state.retryAtMs = mdkr_party_setup_after(nowMs, 300u);
}

inline constexpr const char *kMdkrPartySetupExhaustedCopy =
    "Could not initialize this phone connection within the retry limit. Remove this phone and pair it again.";

#endif
