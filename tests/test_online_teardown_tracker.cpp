#include "online_teardown_tracker.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <system_error>

namespace {
void expect(bool condition, const char *message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL online teardown tracker: %s\n", message);
        std::abort();
    }
}

struct Gate {
    std::mutex mutex;
    std::condition_variable changed;
    bool released = false;

    void wait() {
        std::unique_lock<std::mutex> lock(mutex);
        changed.wait(lock, [this] { return released; });
    }
    void release() noexcept {
        {
            std::lock_guard<std::mutex> lock(mutex);
            released = true;
        }
        changed.notify_all();
    }
};

struct Adapter {
    std::atomic<unsigned> &destroyed;
    Gate *gate = nullptr;
    std::thread::id *destroyedOn = nullptr;

    ~Adapter() {
        if (gate) gate->wait();
        if (destroyedOn) *destroyedOn = std::this_thread::get_id();
        ++destroyed;
    }
};

void awaitLive(OnlineRoomTeardownTracker &tracker, unsigned expected) {
    std::unique_lock<std::mutex> lock(tracker.mutex);
    tracker.done.wait(lock, [&] { return tracker.live == expected; });
}

void testEmptyAndCompleted() {
    OnlineRoomTeardownTracker tracker;
    bool warned = false;
    const auto warning = [&]() noexcept { warned = true; };
    expect(tracker.retire(std::unique_ptr<Adapter>{}), "empty retirement succeeds");
    expect(tracker.pollReady(), "empty tracker is ready without a wait");
    expect(tracker.drain(std::chrono::milliseconds(0), warning),
           "empty tracker completes immediately");
    expect(!warned, "empty tracker does not warn");

    std::atomic<unsigned> destroyed{0u};
    std::thread::id destroyedOn;
    expect(tracker.retire(std::unique_ptr<Adapter>(
               new Adapter{destroyed, nullptr, &destroyedOn})),
           "normal retirement launches");
    awaitLive(tracker, 0u);
    expect(tracker.drain(std::chrono::milliseconds(0), warning),
           "completed worker meets deadline");
    expect(!warned && tracker.threads.empty() && destroyed.load() == 1u,
           "completed handle joined and removed without warning");
    expect(destroyedOn != std::this_thread::get_id(),
           "normal cleanup runs off the launcher thread");
}

void testOverdueWorkersStillJoin() {
    OnlineRoomTeardownTracker tracker;
    Gate release;
    unsigned warnings = 0u;
    std::atomic<unsigned> destroyed{0u};
    for (unsigned index = 0u; index < 2u; ++index) {
        expect(tracker.retire(std::unique_ptr<Adapter>(
                   new Adapter{destroyed, &release})),
               "held retirement launches");
    }

    // Both workers remain held until the deadline callback: deterministic
    // ordering with no sleeps, networking, graphics or process-global teardown.
    const bool onTime = tracker.drain(std::chrono::milliseconds(0), [&]() noexcept {
        ++warnings;
        {
            // The callback must be allowed to observe tracker state. A
            // regression holding this mutex is caught by the fixture timeout.
            std::lock_guard<std::mutex> lock(tracker.mutex);
            expect(tracker.live == 2u, "warning sees both pending workers");
        }
        expect(destroyed.load() == 0u, "destructors are pending at deadline");
        release.release();
    });
    expect(!onTime && warnings == 1u, "overdue drain reports exactly once");
    expect(destroyed.load() == 2u && tracker.live == 0u && tracker.threads.empty(),
           "overdue drain joins every worker before tracker can be destroyed");
    expect(tracker.drain(std::chrono::milliseconds(0), [&]() noexcept { ++warnings; }),
           "repeated drain is empty and complete");
    expect(warnings == 1u, "repeated drain adds no warning");
}

struct RefuseThread {
    template <typename Work>
    std::thread operator()(Work &&) const {
        throw std::system_error(std::make_error_code(
            std::errc::resource_unavailable_try_again));
    }
};

void testLaunchRefusalAndRecovery() {
    OnlineRoomTeardownTracker tracker;
    std::atomic<unsigned> destroyed{0u};
    std::thread::id destroyedOn;
    expect(!tracker.retire(std::unique_ptr<Adapter>(
                new Adapter{destroyed, nullptr, &destroyedOn}), RefuseThread{}),
           "thread refusal reports synchronous fallback");
    expect(destroyed.load() == 1u && destroyedOn == std::this_thread::get_id(),
           "refused launch retains ownership and cleans up exactly once inline");
    expect(tracker.live == 0u && tracker.threads.empty(),
           "refused launch leaves no stranded count or joinable handle");
    expect(tracker.retire(std::unique_ptr<Adapter>(new Adapter{destroyed})),
           "later launch still succeeds after refusal");
    tracker.drain(std::chrono::milliseconds(0), []() noexcept {});
    expect(destroyed.load() == 2u, "recovered launch also cleans up exactly once");
}

void testReapWhileAnotherWorkerIsHeld() {
    OnlineRoomTeardownTracker tracker;
    Gate held;
    std::atomic<unsigned> destroyed{0u};
    expect(tracker.retire(std::unique_ptr<Adapter>(new Adapter{destroyed, &held})),
           "long retirement launches");
    expect(!tracker.pollReady(), "closing poll returns while destructor is held");
    // A stalled transport must not retain completed handles from later rooms.
    for (unsigned index = 0u; index < 16u; ++index) {
        expect(tracker.retire(std::unique_ptr<Adapter>(new Adapter{destroyed})),
               "neighbor retirement launches");
        awaitLive(tracker, 1u);
        expect(tracker.threads.size() == 2u,
               "completed handles from earlier sessions were reclaimed");
    }
    expect(!tracker.pollReady(), "completed neighbors do not hide the held worker");
    expect(tracker.threads.size() == 1u && destroyed.load() == 16u,
           "reaping does not wait for the held destructor");
    held.release();
    awaitLive(tracker, 0u);
    expect(tracker.pollReady(), "closing becomes ready only after remaining join");
    expect(tracker.pollReady(), "ready polling is idempotent");
    expect(destroyed.load() == 17u && tracker.threads.empty(),
           "remaining held worker is still safely joined at exit");
}
} // namespace

int main() {
    testEmptyAndCompleted();
    testOverdueWorkersStillJoin();
    testLaunchRefusalAndRecovery();
    testReapWhileAnotherWorkerIsHeld();
    std::puts("online teardown tracker: PASS");
}
