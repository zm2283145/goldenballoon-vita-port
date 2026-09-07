#ifndef MDKR64_RESERVED_OWNER_WORK_H
#define MDKR64_RESERVED_OWNER_WORK_H

#include <memory>
#include <mutex>
#include <type_traits>
#include <utility>

// Reserve before the owner's resources are admitted. Dispatcher must be a
// live, unbounded serial Processor whose prepared continuation settles before
// destroying its callable. Payload assignment must not allocate or throw.
// This reserves dispatch, not arbitrary callback bodies or OS synchronization.
template <typename Owner, typename Dispatcher, typename Payload>
class MdkrReservedOwnerWork {
    static_assert(std::is_nothrow_copy_assignable<Payload>::value,
                  "Reserved payload publication must not throw");
    struct State {
        std::shared_ptr<Owner> owner;
        Payload payload{};
    };
    Dispatcher &dispatcher_;
    std::shared_ptr<State> state_;
    typename Dispatcher::PreparedWork work_;
    std::mutex mutex_;

public:
    template <typename Function>
    MdkrReservedOwnerWork(Dispatcher &dispatcher, Function &&function)
        : dispatcher_(dispatcher), state_(std::make_shared<State>()),
          work_(dispatcher.prepare(
              [state = state_, function = std::forward<Function>(function)]() mutable {
                  // Do not move owner into a body-local: its destructor could
                  // join this Processor before the prepared continuation runs.
                  function(*state->owner, state->payload);
              })) {}

    bool publish(const std::shared_ptr<Owner> &owner, Payload payload) {
        typename Dispatcher::PreparedWork work;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!owner || !work_) return false;
            state_->owner = owner;
            state_->payload = payload;
            work = std::move(work_);
            // Only the callable keeps State now. No Owner -> slot -> Owner
            // cycle survives publication, including callback exception exit.
            state_.reset();
        }
        dispatcher_.enqueuePrepared(std::move(work));
        return true;
    }
};

#endif
