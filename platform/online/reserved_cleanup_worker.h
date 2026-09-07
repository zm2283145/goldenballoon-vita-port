#ifndef MDKR64_RESERVED_CLEANUP_WORKER_H
#define MDKR64_RESERVED_CLEANUP_WORKER_H

#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <stdexcept>
#include <thread>
#include <utility>

// Reserve two workers and all job/future state BEFORE initializing the resources
// they will eventually retire. The cleanup worker waits for Run/Cancel; the
// publication worker joins it before settling the completion promise. The
// patched TokenPayload owns one helper: two reserved threads per live RTC epoch.
// The final owner may be destroyed while holding an initialization mutex or on
// a worker that cleanup must join:
// that destructor must neither create a thread nor run cleanup inline.
// Single owner; arm() and destruction must not race. The callable must not
// borrow this helper or its enclosing owner, which can die before cleanup runs.
class MdkrReservedCleanupWorker {
    struct State {
        template <typename Work>
        explicit State(Work &&work)
            : cleanup(std::forward<Work>(work)),
              decision(),
              decisionReady(decision.get_future().share()),
              completion(),
              completionReady(completion.get_future().share()) {}

        std::function<void()> cleanup;
        std::promise<bool> decision;
        std::shared_future<bool> decisionReady;
        std::promise<void> completion;
        std::shared_future<void> completionReady;
        std::exception_ptr error;
        std::thread cleanupThread; // joined only by the publisher, if started
    };

public:
    struct StartThread {
        template <typename Work>
        std::thread operator()(Work &&work) const {
            return std::thread(std::forward<Work>(work));
        }
    };
    struct DetachThread {
        void operator()(std::thread &worker) const { worker.detach(); }
    };

    // Injected start must return a joinable worker, or throw without starting
    // work. Injected detach has std::thread::detach semantics. Both operations
    // occur before callers can begin initialization, not in a noexcept dtor.
    template <typename Work, typename Starter = StartThread,
              typename Detacher = DetachThread>
    explicit MdkrReservedCleanupWorker(Work &&work, Starter start = {},
                                       Detacher detach = {})
        : state_(std::make_shared<State>(std::forward<Work>(work))) {
        std::thread publisher;
        bool publisherStarted = false;
        try {
            state_->cleanupThread = start([state = state_] {
                // Cancellation touches job-owned state only. No RTC/logger/
                // initialization lock may be acquired before the Run decision.
                try {
                    if (state->decisionReady.get()) state->cleanup();
                } catch (...) {
                    state->error = std::current_exception();
                }
            });
            if (!state_->cleanupThread.joinable()) {
                throw std::logic_error("Reserved cleanup starter returned no worker");
            }
            publisher = start([state = state_] {
                // This dedicated thread is the sole joining owner after its
                // construction succeeds. Joining includes cleanup-thread TLS
                // destruction, without allocating a late at-thread-exit hook.
                state->cleanupThread.join();
                state->cleanup = nullptr;
                const auto error = std::move(state->error);
                if (error) state->completion.set_exception(error);
                else state->completion.set_value();
                // After publication: job-owned bookkeeping only. No RTC,
                // logger, initialization lock or borrowed application owner.
            });
            if (!publisher.joinable()) {
                throw std::logic_error("Reserved cleanup starter returned no publisher");
            }
            publisherStarted = true;
            detach(publisher);
            if (publisher.joinable()) {
                throw std::logic_error("Reserved cleanup detacher retained the publisher");
            }
        } catch (...) {
            state_->decision.set_value(false);
            if (publisher.joinable()) {
                publisher.join(); // publisher alone joins cleanupThread
            } else if (publisherStarted) {
                // A custom detacher may already have transferred the handle.
                // Observe cancellation completion rather than abandon its job.
                state_->completionReady.wait();
            } else if (state_->cleanupThread.joinable()) {
                // Publisher construction failed: ownership never transferred.
                state_->cleanupThread.join();
            }
            throw;
        }
    }

    MdkrReservedCleanupWorker(const MdkrReservedCleanupWorker &) = delete;
    MdkrReservedCleanupWorker &operator=(const MdkrReservedCleanupWorker &) = delete;

    ~MdkrReservedCleanupWorker() noexcept {
        state_->decision.set_value(armed_);
        if (!armed_) {
            // Initialization threw before arm(): the publisher was detached
            // already. This is acknowledged cancellation, NOT joining that
            // detached handle. It guarantees cleanupThread was joined,
            // including TLS destruction; the publisher may still be releasing
            // private job bookkeeping. It is not an all-OS-thread-exit oracle.
            state_->completionReady.wait();
        }
    }

    std::shared_future<void> future() const noexcept {
        return state_->completionReady;
    }

    // Publish the completion future to its observer before arming. Arm before
    // fallible acquisition only when cleanup explicitly owns partial rollback;
    // otherwise leave unarmed until initialization succeeds. Armed destruction
    // signals the preallocated job without waiting under the owner's mutex.
    void arm() noexcept { armed_ = true; }

private:
    std::shared_ptr<State> state_;
    bool armed_ = false;
};

#endif // MDKR64_RESERVED_CLEANUP_WORKER_H
