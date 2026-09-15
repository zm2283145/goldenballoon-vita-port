#ifndef MDKR64_ONLINE_TEARDOWN_TRACKER_H
#define MDKR64_ONLINE_TEARDOWN_TRACKER_H

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

// Launcher-thread-owned handles; retiring workers publish completion under
// mutex. A diagnostic deadline is not permission to let a worker outlive the
// process globals its adapter may still use.
struct OnlineRoomTeardownTracker {
    struct Worker {
        std::thread thread;
        bool complete = false; // guarded by mutex, including launcher reads
    };
    struct StartThread {
        template <typename Work>
        std::thread operator()(Work &&work) const {
            return std::thread(std::forward<Work>(work));
        }
    };
    std::mutex mutex;
    std::condition_variable done;
    unsigned live = 0u;
    std::vector<std::unique_ptr<Worker>> threads;

    // Launcher thread only. Reap each completed handle even when a different
    // transport remains stalled. Completion is published AFTER adapter cleanup;
    // joining here waits only for that worker's bookkeeping tail.
    void reapCompleted() {
        for (std::size_t index = 0; index < threads.size();) {
            bool complete;
            {
                std::lock_guard<std::mutex> lock(mutex);
                complete = threads[index]->complete;
            }
            if (!complete) {
                ++index;
                continue;
            }
            std::thread &worker = threads[index]->thread;
            if (worker.joinable()) worker.join();
            threads.erase(threads.begin() + index);
        }
    }

    // Launcher-thread poll: never wait for an adapter destructor that has not
    // completed. Ready means its bookkeeping tail has also been joined.
    bool pollReady() {
        reapCompleted();
        return threads.empty();
    }

    // Registry retraction must precede this call on the launcher thread.
    // Allocate/store the stable record BEFORE launching, and keep sole adapter
    // ownership here until thread construction succeeds. Allocation or launch
    // refusal falls back to synchronous cleanup; it must not leak ownership,
    // strand the live count, or destroy an unrecorded joinable std::thread.
    // The injectable starter must either return a joinable worker or throw
    // without starting work, exactly like std::thread construction.
    template <typename Adapter, typename Starter = StartThread>
    bool retire(std::unique_ptr<Adapter> adapter, Starter start = {}) {
        if (!adapter) return true;
        reapCompleted();
        Worker *record;
        try {
            auto prepared = std::make_unique<Worker>();
            record = prepared.get();
            threads.push_back(std::move(prepared));
        } catch (...) {
            adapter.reset();
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(mutex);
            ++live;
        }
        try {
            record->thread = start([this, record, owned = adapter.get()] {
                delete owned;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    record->complete = true;
                    --live;
                }
                done.notify_all();
            });
        } catch (...) {
            {
                std::lock_guard<std::mutex> lock(mutex);
                --live;
            }
            threads.pop_back();
            done.notify_all();
            adapter.reset();
            return false;
        }
        (void)adapter.release();
        return true;
    }

    // Call only after no more adapters can be retired on the launcher thread.
    // Returns whether the diagnostic deadline was met, but ALWAYS joins before
    // returning. onDelayed runs outside the tracker lock and must not throw.
    template <typename Rep, typename Period, typename Warning>
    bool drain(const std::chrono::duration<Rep, Period> &deadline,
               Warning &&onDelayed) {
        static_assert(noexcept(onDelayed()), "teardown warning must not throw");
        std::vector<std::unique_ptr<Worker>> retiring;
        bool finishedWithinDeadline;
        {
            std::unique_lock<std::mutex> lock(mutex);
            finishedWithinDeadline = done.wait_for(
                lock, deadline, [this] { return live == 0u; });
            retiring.swap(threads);
        }
        if (!finishedWithinDeadline) onDelayed();
        for (const auto &record : retiring) {
            std::thread &worker = record->thread;
            if (worker.joinable()) worker.join();
        }
        return finishedWithinDeadline;
    }
};

#endif
