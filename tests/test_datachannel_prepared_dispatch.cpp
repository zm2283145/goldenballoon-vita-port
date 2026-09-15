// Compile/link against the actual patched RTC ThreadPool and Processor, not
// a second dispatcher model. This fixture starts ordinary worker threads only.
// Refusal intercepts ordinary throwing new/new[], not malloc, over-aligned
// allocation or exception-runtime storage. Explicit bad_alloc throws below
// exercise continuation, not a task-body allocator's failure branch.
#include "impl/threadpool.hpp"
#include "impl/processor.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <new>
#include <thread>

namespace {
std::atomic<bool> refuseAllocation{false};
std::atomic<unsigned> refusedAllocations{0u};
void require(bool condition, const char *message) {
    if (!condition) {
        std::fprintf(stderr, "prepared RTC dispatch: %s\n", message);
        std::abort();
    }
}
}

void *operator new(std::size_t size) {
    if (refuseAllocation.load()) {
        ++refusedAllocations;
        throw std::bad_alloc();
    }
    if (void *value = std::malloc(size ? size : 1u)) return value;
    throw std::bad_alloc();
}
void *operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void *value) noexcept { std::free(value); }
void operator delete[](void *value) noexcept { std::free(value); }
void operator delete(void *value, std::size_t) noexcept { std::free(value); }
void operator delete[](void *value, std::size_t) noexcept { std::free(value); }

namespace {
using rtc::impl::Processor;
using rtc::impl::ThreadPool;

struct Gate {
    std::mutex mutex;
    std::condition_variable condition;
    bool entered = false;
    bool released = false;
    void hold() {
        std::unique_lock<std::mutex> lock(mutex);
        entered = true;
        condition.notify_all();
        condition.wait(lock, [&] { return released; });
    }
    void awaitEntry() {
        std::unique_lock<std::mutex> lock(mutex);
        require(condition.wait_for(lock, std::chrono::seconds(5), [&] { return entered; }),
                "worker entered held task");
    }
    void release() {
        std::lock_guard<std::mutex> lock(mutex);
        released = true;
        condition.notify_all();
    }
};

void awaitFlag(const std::atomic<bool> &flag, const char *message) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!flag.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    require(flag.load(), message);
}

void acceptedWorkAndDueTimerKeepOrderWithoutAllocating() {
    auto &pool = ThreadPool::Instance();
    pool.spawn(1);
    Gate gate;
    pool.enqueue([&] { gate.hold(); });
    gate.awaitEntry();
    int order[3] = {};
    unsigned next = 0u; // One worker; examined only after its join.
    const auto record = [&](int value) {
        require(next < 3u, "each accepted callable runs at most once");
        order[next++] = value;
    };
    pool.schedule(ThreadPool::clock::now() - std::chrono::seconds(1),
                  [&] { record(1); });
    auto first = ThreadPool::prepare([&] { record(2); });
    auto second = ThreadPool::prepare([&] { record(3); });
    refuseAllocation = true;
    pool.enqueuePrepared(std::move(first));
    pool.enqueuePrepared(std::move(second));
    gate.release();
    pool.join();
    pool.clear();
    refuseAllocation = false;
    require(refusedAllocations == 0u, "accepted publication/execution/join does not allocate");
    require(next == 3u && order[0] == 1 && order[1] == 2 && order[2] == 3,
            "overdue timer precedes later immediate work and immediate FIFO is preserved");
}

void processorContinuesAfterThrowWithoutAllocating() {
    auto &pool = ThreadPool::Instance();
    pool.spawn(1);
    Processor processor;
    Gate gate;
    processor.enqueue([&] { gate.hold(); });
    gate.awaitEntry();
    int order[4] = {};
    unsigned next = 0u;
    const auto record = [&](int value) {
        require(next < 4u, "each accepted Processor task runs at most once");
        order[next++] = value;
    };
    const auto failuresBefore = pool.preparedTaskFailures();
    processor.enqueue([&] { record(1); });
    processor.enqueue([&] { record(2); throw 7; });
    processor.enqueue([&] { record(3); throw std::bad_alloc(); });
    processor.enqueue([&] { record(4); });
    refuseAllocation = true;
    gate.release();
    processor.join();
    pool.join();
    pool.clear();
    refuseAllocation = false;
    require(refusedAllocations == 0u, "accepted Processor continuation has no allocation path");
    require(next == 4u && order[0] == 1 && order[1] == 2 && order[2] == 3 && order[3] == 4,
            "non-standard and bad_alloc task exceptions cannot strand later accepted work or join");
    require(pool.preparedTaskFailures() == failuresBefore + 2u,
            "contained task failure remains observable without allocating a log message");
}

void preparedBeforeDueTimerKeepsItsEarlierPublicationOrder() {
    auto &pool = ThreadPool::Instance();
    pool.spawn(1);
    Gate gate;
    pool.enqueue([&] { gate.hold(); });
    gate.awaitEntry();
    int order[3] = {};
    unsigned next = 0u;
    const auto record = [&](int value) {
        require(next < 3u, "mixed-lane task executes at most once");
        order[next++] = value;
    };
    pool.enqueuePrepared(ThreadPool::prepare([&] { record(1); }));
    // The timer key is strictly later than the first node's publication key,
    // independent of scheduler speed or the clock's tick resolution.
    const auto due = ThreadPool::clock::now() + std::chrono::milliseconds(1);
    pool.schedule(due, [&] { record(2); });
    auto later = ThreadPool::prepare([&] { record(3); });
    {
        std::mutex mutex;
        std::condition_variable condition;
        std::unique_lock<std::mutex> lock(mutex);
        (void)condition.wait_until(lock, due, [] { return false; });
    }
    refuseAllocation = true;
    pool.enqueuePrepared(std::move(later));
    gate.release();
    pool.join();
    pool.clear();
    refuseAllocation = false;
    require(refusedAllocations == 0u, "mixed prepared/timer accepted dispatch does not allocate");
    require(next == 3u && order[0] == 1 && order[1] == 2 && order[2] == 3,
            "earlier prepared work precedes due timer, which precedes later prepared work");
}

void boundedProcessorAdmissionWakesAfterQueueSpaceIsReleased() {
    auto &pool = ThreadPool::Instance();
    pool.spawn(1);
    Processor processor(1u);
    Gate first, second;
    int order[3] = {};
    unsigned next = 0u;
    const auto record = [&](int value) {
        require(next < 3u, "bounded Processor executes each task once");
        order[next++] = value;
    };
    processor.enqueue([&] { record(1); first.hold(); });
    first.awaitEntry();
    processor.enqueue([&] { record(2); second.hold(); }); // fills the one waiting slot
    std::atomic<bool> attempting{false};
    std::mutex admissionMutex;
    std::condition_variable admissionCondition;
    bool admitted = false;
    std::thread submitter([&] {
        attempting = true;
        processor.enqueue([&] { record(3); });
        std::lock_guard<std::mutex> lock(admissionMutex);
        admitted = true;
        admissionCondition.notify_all();
    });
    awaitFlag(attempting, "bounded producer reaches admission call");
    {
        std::unique_lock<std::mutex> lock(admissionMutex);
        // Black-box admission check: while the first task is held and the
        // waiting slot is full, successful third admission must not return.
        // The attempting flag precedes enqueue: this bounded observation is
        // not a deterministic witness that the submitter entered its CV wait.
        require(!admissionCondition.wait_for(lock, std::chrono::milliseconds(100), [&] { return admitted; }),
                "Processor(limit=1) applies waiting-queue backpressure");
    }
    first.release();
    second.awaitEntry(); // schedule() must acquire its mutex and pop the waiting slot
    {
        std::unique_lock<std::mutex> lock(admissionMutex);
        require(admissionCondition.wait_for(lock, std::chrono::seconds(5), [&] { return admitted; }),
                "blocked admission wakes after continuation releases queue space");
    }
    submitter.join();
    second.release();
    processor.join();
    pool.join();
    pool.clear();
    require(next == 3u && order[0] == 1 && order[1] == 2 && order[2] == 3,
            "bounded admission preserves FIFO and join completes without mutex deadlock");
}

void finalTaskOwnerCanJoinItsProcessorDuringDestruction() {
    auto &pool = ThreadPool::Instance();
    pool.spawn(1);
    std::atomic<bool> destroyed{false};
    struct Owner {
        Processor processor;
        std::atomic<bool> &destroyed;
        explicit Owner(std::atomic<bool> &flag) : destroyed(flag) {}
        ~Owner() {
            processor.join();
            destroyed = true;
        }
    };
    auto owner = std::make_shared<Owner>(destroyed);
    Gate gate;
    owner->processor.enqueue([keep = owner, &gate] { (void)keep; gate.hold(); });
    gate.awaitEntry();
    owner.reset(); // The held production task is now the final owner.
    gate.release();
    awaitFlag(destroyed, "last task releases owner after marking its Processor idle");
    pool.join();
    pool.clear();
}

void clearReleasesTimedOwnersOutsideLockAndKeepsPreparedWork() {
    auto &pool = ThreadPool::Instance();
    pool.spawn(1);
    Gate gate;
    pool.enqueue([&] { gate.hold(); });
    gate.awaitEntry();
    std::atomic<bool> published{false}, ran{false};
    auto prepared = ThreadPool::prepare([&] { ran = true; });
    struct PublishOnRelease {
        ThreadPool::PreparedWork &prepared;
        std::atomic<bool> &published;
        PublishOnRelease(ThreadPool::PreparedWork &work, std::atomic<bool> &flag)
            : prepared(work), published(flag) {}
        ~PublishOnRelease() {
            ThreadPool::Instance().enqueuePrepared(std::move(prepared));
            published = true;
        }
    };
    auto owner = std::make_shared<PublishOnRelease>(prepared, published);
    // Discard the future so it cannot extend packaged-task capture lifetime.
    pool.schedule(std::chrono::hours(1), [keep = owner] { (void)keep; });
    owner.reset();
    pool.clear();
    require(published, "timed cancellation releases callable owner outside pool mutex");
    gate.release();
    awaitFlag(ran, "clear does not cancel accepted prepared work");
    pool.join();
    pool.clear();
}

void idlePoolJoinDoesNotDiscardAcceptedWork() {
    auto &pool = ThreadPool::Instance();
    for (unsigned iteration = 0; iteration < 16u; ++iteration) {
        pool.spawn(1);
        std::atomic<bool> ran{false};
        auto prepared = ThreadPool::prepare([&] { ran = true; });
        pool.enqueuePrepared(std::move(prepared));
        pool.join();
        require(ran, "join must account for accepted queue as well as busy workers");
        pool.clear();
    }
}
}

int main() {
    acceptedWorkAndDueTimerKeepOrderWithoutAllocating();
    preparedBeforeDueTimerKeepsItsEarlierPublicationOrder();
    processorContinuesAfterThrowWithoutAllocating();
    boundedProcessorAdmissionWakesAfterQueueSpaceIsReleased();
    finalTaskOwnerCanJoinItsProcessorDuringDestruction();
    clearReleasesTimedOwnersOutsideLockAndKeepsPreparedWork();
    idlePoolJoinDoesNotDiscardAcceptedWork();
    return 0;
}
