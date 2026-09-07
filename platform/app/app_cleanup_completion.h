// Observe a process-wide asynchronous cleanup after its callers retire every
// owner. The caller must forbid new owner creation once retirement is declared.
// Completion includes failure: failed() MUST be checked before claiming that
// resources were successfully shut down. This helper imposes no time deadline.
#ifndef MDKR64_APP_CLEANUP_COMPLETION_H
#define MDKR64_APP_CLEANUP_COMPLETION_H

#include <chrono>
#include <exception>
#include <future>
#include <stdexcept>
#include <utility>

class AppCleanupCompletion {
public:
    // Launcher-thread only. Start is called once, and only after ownersRetired.
    // Polling never waits for asynchronous work. Start itself must return its
    // shared_future promptly; a deferred future is a contract failure, not work
    // that may be executed by get() on the launcher thread.
    template <typename Start>
    bool poll(bool ownersRetired, Start &&start) {
        if (finished_) return true;
        try {
            if (!started_) {
                if (!ownersRetired) return false;
                started_ = true;
                future_ = std::forward<Start>(start)();
                if (!future_.valid()) {
                    throw std::logic_error("Cleanup returned an invalid future");
                }
            }
            const std::future_status status =
                future_.wait_for(std::chrono::seconds(0));
            if (status == std::future_status::timeout) return false;
            if (status == std::future_status::deferred) {
                throw std::logic_error("Cleanup returned a deferred future");
            }
            future_.get();
        } catch (...) {
            error_ = std::current_exception();
        }
        finished_ = true;
        return true;
    }

    bool failed() const noexcept { return error_ != nullptr; }
    std::exception_ptr error() const noexcept { return error_; }

private:
    std::shared_future<void> future_;
    std::exception_ptr error_;
    bool started_ = false;
    bool finished_ = false;
};

#endif // MDKR64_APP_CLEANUP_COMPLETION_H
