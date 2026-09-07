#ifndef MDKR64_ASYNC_WORK_BUDGET_H
#define MDKR64_ASYNC_WORK_BUDGET_H

#include <atomic>
#include <cstddef>
#include <memory>
#include <utility>

// Bounds actual outstanding work, not just callers still awaiting a result.
// A worker keeps its permit until its owned lookup work and result cleanup
// finish, even if its caller has abandoned the result. The shared state
// outlives the budget object while permits exist;
// releasing a permit never accesses the budget object or process globals.
class AsyncWorkBudget {
    struct State {
        explicit State(std::size_t maximum) : limit(maximum) {}
        const std::size_t limit;
        std::atomic<std::size_t> used{0};
    };

public:
    class Permit {
    public:
        Permit() noexcept = default;
        Permit(const Permit &) = delete;
        Permit &operator=(const Permit &) = delete;
        Permit(Permit &&other) noexcept : state_(std::move(other.state_)) {}
        Permit &operator=(Permit &&other) noexcept {
            if (this != &other) {
                release();
                state_ = std::move(other.state_);
            }
            return *this;
        }
        ~Permit() { release(); }

        explicit operator bool() const noexcept { return state_ != nullptr; }

    private:
        friend class AsyncWorkBudget;
        explicit Permit(std::shared_ptr<State> state) noexcept
            : state_(std::move(state)) {}
        void release() noexcept {
            if (state_) {
                state_->used.fetch_sub(1, std::memory_order_release);
                state_.reset();
            }
        }
        std::shared_ptr<State> state_;
    };

    explicit AsyncWorkBudget(std::size_t limit)
        : state_(std::make_shared<State>(limit)) {}
    AsyncWorkBudget(const AsyncWorkBudget &) = delete;
    AsyncWorkBudget &operator=(const AsyncWorkBudget &) = delete;

    // No wait, thread creation or allocation. A rejected acquisition has no
    // effect on the budget. The budget object must remain alive during this call.
    Permit tryAcquire() noexcept {
        std::size_t used = state_->used.load(std::memory_order_relaxed);
        while (used < state_->limit) {
            if (state_->used.compare_exchange_weak(
                    used, used + 1, std::memory_order_acquire,
                    std::memory_order_relaxed)) {
                return Permit(state_);
            }
        }
        return {};
    }

    std::size_t limit() const noexcept { return state_->limit; }
    std::size_t inUse() const noexcept {
        return state_->used.load(std::memory_order_acquire);
    }

private:
    std::shared_ptr<State> state_;
};

namespace mdkr_async_work_detail {
// Observing an unused pool must not allocate merely to close the application.
// The accessor initializes this slot BEFORE its owner, so the slot also outlives
// the owner during ordered static destruction.
inline std::atomic<AsyncWorkBudget *> &resolverBudgetSlot() noexcept {
    static std::atomic<AsyncWorkBudget *> slot{nullptr};
    return slot;
}

class ResolverBudgetOwner {
public:
    explicit ResolverBudgetOwner(std::atomic<AsyncWorkBudget *> &slot)
        : budget_(8), slot_(slot) {
        // Construction may throw before publication; a failed construction
        // leaves no observable budget and cannot have issued any permits.
        slot_.store(&budget_, std::memory_order_release);
    }
    ~ResolverBudgetOwner() {
        slot_.store(nullptr, std::memory_order_release);
    }
    AsyncWorkBudget &budget() noexcept { return budget_; }

private:
    AsyncWorkBudget budget_;
    std::atomic<AsyncWorkBudget *> &slot_;
};
} // namespace mdkr_async_work_detail

// External-linkage inline function: its local static is ONE process budget
// shared by room and signal translation units, not a separate per-TU pool.
// This limits first-party resolver jobs only; it does not cancel OS resolution
// or account for libdatachannel/libjuice's separately owned resolver work.
// Eight is an outstanding-lookup limit, not an active-connection/player limit.
inline AsyncWorkBudget &onlineResolverWorkBudget() {
    auto &slot = mdkr_async_work_detail::resolverBudgetSlot();
    static mdkr_async_work_detail::ResolverBudgetOwner owner(slot);
    return owner.budget();
}

// Nonallocating snapshot, including before the first resolver and after a failed
// initial allocation. Zero is an exit barrier ONLY after all producer owners
// retire: before then another producer may be constructing/acquiring the pool.
// Observe during application lifetime, not concurrently with static destruction.
// The owner retracts publication before destruction; surviving permits retain
// their own shared state and never consult this slot when releasing.
inline std::size_t onlineResolverWorkInUse() noexcept {
    const auto *budget = mdkr_async_work_detail::resolverBudgetSlot().load(
        std::memory_order_acquire);
    return budget != nullptr ? budget->inUse() : 0u;
}

#endif // MDKR64_ASYNC_WORK_BUDGET_H
