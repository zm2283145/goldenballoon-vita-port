#ifndef MDKR64_CHARACTER_ASYNC_JOB_H
#define MDKR64_CHARACTER_ASYNC_JOB_H

#include <exception>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

/*
 * One small, auditable lifecycle for Workshop work that must not block the UI.
 *
 * A job remains busy until its result has been polled, not merely until the
 * worker function returns. This prevents callers from starting a second
 * operation before the first operation's result has been published. Worker
 * exceptions cross the thread boundary as exception_ptr instead of invoking
 * std::terminate. Destruction joins as the final safety net; the launcher
 * services and visibly settles jobs before normal shutdown, so that join is
 * not the user-facing shutdown path.
 */
template <typename Result>
class CharacterAsyncJob final {
public:
    CharacterAsyncJob() = default;
    CharacterAsyncJob(const CharacterAsyncJob &) = delete;
    CharacterAsyncJob &operator=(const CharacterAsyncJob &) = delete;

    ~CharacterAsyncJob() {
        if (thread_.joinable()) thread_.join();
    }

    template <typename Work>
    bool start(Work &&work) {
        std::lock_guard<std::mutex> guard(mutex_);
        if (running_ || ready_) return false;
        running_ = true;
        try {
            thread_ = std::thread(
                [this, work = std::forward<Work>(work)]() mutable {
                    std::unique_ptr<Result> completed;
                    std::exception_ptr error;
                    try {
                        completed = std::make_unique<Result>(work());
                    } catch (...) {
                        error = std::current_exception();
                    }
                    std::lock_guard<std::mutex> finished(mutex_);
                    completed_ = std::move(completed);
                    error_ = std::move(error);
                    running_ = false;
                    ready_ = true;
                });
        } catch (...) {
            running_ = false;
            return false;
        }
        return true;
    }

    bool busy() const {
        std::lock_guard<std::mutex> guard(mutex_);
        return running_ || ready_;
    }

    bool poll(std::unique_ptr<Result> &completed,
              std::exception_ptr &error) {
        std::thread finishedThread;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            if (!ready_) return false;
            completed = std::move(completed_);
            error = std::move(error_);
            ready_ = false;
            finishedThread = std::move(thread_);
        }
        if (finishedThread.joinable()) finishedThread.join();
        return true;
    }

private:
    mutable std::mutex mutex_;
    std::thread thread_;
    bool running_ = false;
    bool ready_ = false;
    std::unique_ptr<Result> completed_;
    std::exception_ptr error_;
};

#endif
