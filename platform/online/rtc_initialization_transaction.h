#ifndef MDKR64_RTC_INITIALIZATION_TRANSACTION_H
#define MDKR64_RTC_INITIALIZATION_TRANSACTION_H

#include <array>
#include <chrono>
#include <cstddef>
#include <future>
#include <stdexcept>
#include <utility>

// The pinned RTC Init mutex serializes this state. No callback below may
// reacquire that mutex. Actual retirement runs on the pre-reserved worker,
// after the failed token constructor has unwound and released the mutex.
class MdkrRtcInitializationTransaction {
public:
    enum class Stage : std::size_t { Sockets, Pool, Poll, Sctp, Count };
    enum class Phase { Dormant, Starting, Ready, RollbackPending, Retiring, Failed };

    Phase phase() const noexcept { return phase_; }
    bool owns(Stage stage) const noexcept { return owned_[index(stage)]; }

    // Use only when no weak token can be retained. A live token belongs to the
    // existing epoch and must not be rejected just because Cleanup dropped the
    // global strong reference. This never waits under the initialization lock.
    void admit(const std::shared_future<void> &previous) const {
        if (!previous.valid()) throw std::logic_error("RTC cleanup future is invalid");
        if (previous.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
            throw std::runtime_error("RTC cleanup is still in progress");
        previous.get(); // A failed retirement cannot authorize another epoch.
        if (phase_ != Phase::Dormant)
            throw std::logic_error("RTC initialization state is not retired");
    }

    template <typename Start>
    void initialize(Start &&start) {
        if (phase_ != Phase::Dormant)
            throw std::logic_error("RTC initialization already owns an epoch");
        phase_ = Phase::Starting;
        try {
            std::forward<Start>(start)();
            phase_ = Phase::Ready;
        } catch (...) {
            phase_ = Phase::RollbackPending;
            throw;
        }
    }

    // Mark successful atomic starts after return. Mark a partial-start stage
    // (the thread pool) before starting, but only after its singleton exists.
    void acquired(Stage stage) noexcept { owned_[index(stage)] = true; }

    template <typename Stop>
    void release(Stage stage, Stop &&stop) {
        if (!owns(stage)) return;
        std::forward<Stop>(stop)();
        owned_[index(stage)] = false; // Retain ownership if retirement throws.
    }

    template <typename Stop>
    void retire(Stop &&stop) {
        if (phase_ == Phase::Dormant) return;
        if (phase_ == Phase::Failed)
            throw std::logic_error("RTC retirement previously failed");
        phase_ = Phase::Retiring;
        try {
            std::forward<Stop>(stop)();
            for (bool owned : owned_)
                if (owned) throw std::logic_error("RTC retirement retained a stage");
            phase_ = Phase::Dormant;
        } catch (...) {
            phase_ = Phase::Failed;
            throw;
        }
    }

private:
    static constexpr std::size_t index(Stage stage) noexcept {
        return static_cast<std::size_t>(stage);
    }
    std::array<bool, static_cast<std::size_t>(Stage::Count)> owned_{};
    Phase phase_ = Phase::Dormant;
};

#endif // MDKR64_RTC_INITIALIZATION_TRANSACTION_H
