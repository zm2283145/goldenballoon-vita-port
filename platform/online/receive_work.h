#ifndef MDKR64_RECEIVE_WORK_H
#define MDKR64_RECEIVE_WORK_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <utility>

// One queued receive wake, distinct from an already executing receive body.
// Preparation coalesces demand, but remains transactional until its owned node
// is ready. Terminal sealing supplies the mandatory fallback after refusal.
class MdkrReceiveAdmission {
public:
    enum class TerminalMode { None, GracefulEof, Abort };

    class Reservation {
    public:
        Reservation() = default;
        Reservation(const Reservation &) = delete;
        Reservation &operator=(const Reservation &) = delete;
        Reservation(Reservation &&other) noexcept
            : owner_(std::exchange(other.owner_, nullptr)), ticket_(other.ticket_) {}
        Reservation &operator=(Reservation &&other) noexcept {
            if (this != &other) {
                rollback();
                owner_ = std::exchange(other.owner_, nullptr);
                ticket_ = other.ticket_;
            }
            return *this;
        }
        ~Reservation() { rollback(); }
        explicit operator bool() const noexcept { return owner_ != nullptr; }
        bool commit() noexcept {
            auto *owner = std::exchange(owner_, nullptr);
            return owner && owner->commit(ticket_);
        }
    private:
        friend class MdkrReceiveAdmission;
        Reservation(MdkrReceiveAdmission *owner, std::uint64_t ticket) noexcept
            : owner_(owner), ticket_(ticket) {}
        void rollback() noexcept {
            if (auto *owner = std::exchange(owner_, nullptr)) owner->rollback(ticket_);
        }
        MdkrReceiveAdmission *owner_ = nullptr;
        std::uint64_t ticket_ = 0;
    };

    // timerGeneration==0 denotes ordinary input/start, not a timer. Valid timer
    // demand is rechecked at commit; ordinary demand coalesced during an older
    // timer's preparation must not disappear when that timer becomes stale.
    Reservation reserve(std::uint64_t timerGeneration = 0) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        if (terminal_ != TerminalMode::None ||
            (timerGeneration && timerGeneration != timerGeneration_)) return {};
        if (phase_ == Phase::Queued) return {};
        if (phase_ == Phase::Idle) {
            phase_ = Phase::Preparing;
            ordinaryWanted_ = false;
            timerWanted_ = 0;
            if (++ticket_ == 0) ++ticket_;
        } else {
            if (timerGeneration) timerWanted_ = timerGeneration;
            else ordinaryWanted_ = true;
            return {};
        }
        if (timerGeneration) timerWanted_ = timerGeneration;
        else ordinaryWanted_ = true;
        return Reservation(this, ticket_);
    }

    // Call only for an accepted normal node, AFTER the transport receive mutex
    // is acquired. Releasing at task completion would lose a late input wake.
    bool beginQueued() noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        if (phase_ != Phase::Queued) return false;
        resetPending();
        return true;
    }

    // Priority only increases: graceful EOF may upgrade to abort. This does not
    // run callbacks or acquire transport/SSL/queue locks. The caller must stop
    // intake and request its independently pre-reserved terminal node.
    bool seal(TerminalMode mode) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        if (static_cast<int>(mode) <= static_cast<int>(terminal_)) return false;
        terminal_ = mode;
        advanceTimer();
        return true;
    }

    TerminalMode terminalMode() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return terminal_;
    }
    std::uint64_t nextTimerGeneration() noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return advanceTimer();
    }
    std::uint64_t timerGeneration() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return timerGeneration_;
    }
    std::size_t pending() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return phase_ == Phase::Idle ? 0u : 1u;
    }

private:
    enum class Phase { Idle, Preparing, Queued };
    bool commit(std::uint64_t ticket) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        if (phase_ != Phase::Preparing || ticket_ != ticket) return false;
        if (terminal_ != TerminalMode::None ||
            (!ordinaryWanted_ && timerWanted_ != timerGeneration_)) {
            resetPending();
            return false;
        }
        phase_ = Phase::Queued;
        return true;
    }
    void rollback(std::uint64_t ticket) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        if (phase_ == Phase::Preparing && ticket_ == ticket) resetPending();
    }
    void resetPending() noexcept {
        phase_ = Phase::Idle;
        ordinaryWanted_ = false;
        timerWanted_ = 0;
    }
    std::uint64_t advanceTimer() noexcept {
        if (++timerGeneration_ == 0) ++timerGeneration_;
        return timerGeneration_;
    }
    mutable std::mutex mutex_;
    Phase phase_ = Phase::Idle;
    TerminalMode terminal_ = TerminalMode::None;
    std::uint64_t ticket_ = 0;
    std::uint64_t timerGeneration_ = 1;
    bool ordinaryWanted_ = false;
    std::uint64_t timerWanted_ = 0;
};

// Reserve before transport resources are published. Dispatcher is a live
// prepared-work pool, and Owner MUST have reserved off-thread final deletion
// (the selected TLS/DTLS factories do). Do not use this for an owner whose
// destructor joins the currently executing dispatcher: final local release
// precedes its dispatcher's completion bookkeeping.
template <typename Dispatcher, typename Owner>
class MdkrReservedTerminal {
public:
    using Complete = void (*)(Owner &);
    MdkrReservedTerminal(Dispatcher &dispatcher, Complete complete)
        : dispatcher_(dispatcher), complete_(complete),
          work_(dispatcher.prepare([this]() {
              auto owner = std::move(owner_);
              auto complete = complete_;
              complete(*owner);
              // No helper access after local owner release, including unwind.
          })) {}
    MdkrReservedTerminal(const MdkrReservedTerminal &) = delete;
    MdkrReservedTerminal &operator=(const MdkrReservedTerminal &) = delete;

    void request(std::shared_ptr<Owner> owner) noexcept {
        if (!owner || requested_.exchange(true)) return;
        owner_ = std::move(owner);
        auto *dispatcher = &dispatcher_;
        auto work = std::move(work_);
        dispatcher->enqueuePrepared(std::move(work));
    }

private:
    Dispatcher &dispatcher_;
    Complete complete_;
    typename Dispatcher::PreparedWork work_;
    std::atomic<bool> requested_{false};
    std::shared_ptr<Owner> owner_;
};

#endif
