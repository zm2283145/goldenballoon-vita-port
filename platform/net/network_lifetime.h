#ifndef MDKR_NETWORK_LIFETIME_H
#define MDKR_NETWORK_LIFETIME_H

#include <atomic>
#include <cstdint>
#include <memory>
#include <utility>

// Sticky failure evidence without an allocating logger. A failed OS cleanup
// prevents fresh native acquisition and a successful launcher shutdown verdict.
inline std::atomic<bool> mdkrFirstPartyNetworkCleanupFailed{false};

// The Windows adapter supplies only OS calls; this shared policy is also used
// by the refusal/version/cleanup-failure fixture without loading Winsock.
template <class Api>
struct MdkrWinsockReferencePolicy {
    static constexpr bool required = true;
    static bool start() noexcept {
        if (mdkrFirstPartyNetworkCleanupFailed.load()) return false;
        uint16_t version = 0;
        if (!Api::startup(version)) return false;
        if (version != 0x0202u) {
            stop();
            return false;
        }
        return true;
    }
    static void stop() noexcept {
        if (!Api::cleanup()) mdkrFirstPartyNetworkCleanupFailed.store(true);
    }
};

// Operations::start() acquires one reference or none and does not throw;
// Operations::stop() releases that reference and reports failure independently.
// Copies share an acquired reference without allocating. A resolver copies its
// caller's lease and keeps it until after address-result cleanup. There is no
// static owner, per-client unbalanced increment or static destruction ordering.
// Independent copies may be used on different threads; mutation of the same
// lease object requires caller serialization, like std::shared_ptr itself.
template <class Operations>
class MdkrSharedNetworkLease {
    struct Token {
        bool owned = Operations::start();
        ~Token() noexcept { if (owned) Operations::stop(); }
    };
public:
    bool acquire() noexcept {
        if constexpr (!Operations::required) return true;
        if (token_) return true;
        try {
            auto candidate = std::make_shared<Token>();
            if (!candidate->owned) return false;
            token_ = std::move(candidate);
            return true;
        } catch (...) {
            return false; // Allocation precedes acquisition of the OS reference.
        }
    }
    explicit operator bool() const noexcept {
        if constexpr (!Operations::required) return true;
        return bool(token_);
    }
    void reset() noexcept { token_.reset(); }
private:
    std::shared_ptr<Token> token_;
};

#endif
