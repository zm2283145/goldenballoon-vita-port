// Shared receive admission plus actual patched pool/retirement dispatch.
// Ordinary throwing new/new[] only; no C allocator or network session coverage.
#include "impl/processor.hpp"
#include "receive_work.h"
#include "transport_retirement.h"

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
std::atomic<unsigned> refusedAllocations{0};
void require(bool value, const char *message) {
    if (!value) {
        std::fprintf(stderr, "RTC receive work: %s\n", message);
        std::abort();
    }
}
}
void *operator new(std::size_t size) {
    if (refuseAllocation.load()) { ++refusedAllocations; throw std::bad_alloc(); }
    if (void *value = std::malloc(size ? size : 1u)) return value;
    throw std::bad_alloc();
}
void *operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void *value) noexcept { std::free(value); }
void operator delete[](void *value) noexcept { std::free(value); }
void operator delete(void *value, std::size_t) noexcept { std::free(value); }
void operator delete[](void *value, std::size_t) noexcept { std::free(value); }

namespace {
using Admission = MdkrReceiveAdmission;
using Mode = Admission::TerminalMode;
using rtc::impl::Processor;
using rtc::impl::ThreadPool;
using rtc::impl::TearDownProcessor;

void normalWakeOwnership() {
    Admission admission;
    auto first = admission.reserve();
    require(bool(first) && admission.pending() == 1u, "preparing wake owns one pending claim");
    require(!admission.reserve(), "ordinary preparation coalesces concurrent demand");
    auto transferred = std::move(first);
    require(!first && transferred.commit() && !transferred, "reservation moves and commits once");
    require(!transferred.commit() && !admission.reserve(), "queued wake cannot commit or admit twice");
    require(admission.beginQueued() && admission.pending() == 0u, "locked task entry releases its claim");
    require(!admission.beginQueued(), "unaccounted entry never decrements below zero");
    auto successor = admission.reserve();
    require(successor.commit() && admission.pending() == 1u, "executing body permits a follow-up wake");
    require(admission.beginQueued(), "follow-up owns its own entry");
}

void preparationRollbackAndMoveAssignment() {
    Admission first, second;
    {
        auto a = first.reserve();
        auto b = second.reserve();
        a = std::move(b);
        require(first.pending() == 0u && second.pending() == 1u,
                "move assignment rolls back only the replaced reservation");
    }
    require(first.pending() == 0u && second.pending() == 0u, "unused reservations roll back");
    auto retry = first.reserve();
    require(retry.commit() && first.beginQueued(), "ordinary retry after rollback succeeds");
}

void obsoleteTimerDoesNotConsumeCount() {
    Admission admission;
    const auto old = admission.timerGeneration();
    auto pending = admission.reserve(old);
    admission.nextTimerGeneration();
    require(!pending.commit() && admission.pending() == 0u,
            "timer becoming stale during preparation has no accepted count");
    require(!admission.reserve(old) && !admission.beginQueued(), "stale timer is not an unaccounted doRecv");
    auto current = admission.reserve(admission.timerGeneration());
    require(current.commit() && admission.beginQueued(), "current timer follows normal accounting");
}

void coalescedDemandSurvivesStaleTimer() {
    for (bool ordinary : {false, true}) {
        Admission admission;
        auto pending = admission.reserve(admission.timerGeneration());
        const auto current = admission.nextTimerGeneration();
        require(!admission.reserve(ordinary ? 0 : current), "new demand coalesces into preparing wake");
        require(pending.commit() && admission.beginQueued(),
                "new ordinary/current-timer demand is not lost with original stale timer");
    }
}

void terminalSealAndPriority() {
    Admission preparing;
    auto reservation = preparing.reserve();
    const auto generation = preparing.timerGeneration();
    require(preparing.seal(Mode::GracefulEof), "first EOF seals new intake");
    require(preparing.timerGeneration() != generation && !reservation.commit() && preparing.pending() == 0u,
            "seal invalidates in-flight preparation and timer generation");
    require(!preparing.reserve() && !preparing.seal(Mode::GracefulEof), "terminal seal is idempotent");
    require(preparing.seal(Mode::Abort) && !preparing.seal(Mode::GracefulEof) &&
            preparing.terminalMode() == Mode::Abort, "abort can upgrade EOF but never be downgraded");

    Admission queued;
    auto accepted = queued.reserve();
    require(accepted.commit(), "normal wake was accepted before terminal seal");
    queued.seal(Mode::Abort);
    require(queued.pending() == 1u && queued.beginQueued() && queued.pending() == 0u,
            "terminal seal preserves accepted-node accounting until actual entry");
}

void admissionOperationsDoNotAllocate() {
    Admission admission;
    const unsigned before = refusedAllocations.load();
    refuseAllocation = true;
    auto first = admission.reserve();
    require(first.commit() && admission.beginQueued(), "normal admission under ordinary-new refusal");
    const auto generation = admission.nextTimerGeneration();
    auto second = admission.reserve(generation);
    admission.seal(Mode::Abort);
    require(!second.commit(), "terminal rollback under ordinary-new refusal");
    refuseAllocation = false;
    require(refusedAllocations == before, "receive accounting itself attempts no ordinary allocation");
}

struct Gate {
    std::mutex mutex;
    std::condition_variable condition;
    bool entered = false, released = false;
    void hold() {
        std::unique_lock<std::mutex> lock(mutex);
        entered = true;
        condition.notify_all();
        condition.wait(lock, [&] { return released; });
    }
    void awaitEntry() {
        std::unique_lock<std::mutex> lock(mutex);
        require(condition.wait_for(lock, std::chrono::seconds(5), [&] { return entered; }), "worker enters hold");
    }
    void release() {
        std::lock_guard<std::mutex> lock(mutex);
        released = true;
        condition.notify_all();
    }
};

struct Workers {
    ThreadPool &pool = ThreadPool::Instance();
    Processor &retirement = TearDownProcessor::Instance();
    Gate first, second;
    Workers() {
        pool.spawn(2);
        pool.enqueue([&] { first.hold(); });
        pool.enqueue([&] { second.hold(); });
        first.awaitEntry();
        second.awaitEntry();
        first.release();
        second.release();
    }
    ~Workers() {
        refuseAllocation = false;
        retirement.join();
        pool.join();
        pool.clear();
    }
};

struct Trace {
    std::atomic<unsigned> completed{0};
    std::atomic<bool> destroyed{false};
    bool throwCompletion = false;
    const std::thread::id caller = std::this_thread::get_id();
};
struct Base { virtual ~Base() = default; virtual void stop() {} };
struct Resource final : Base {
    Trace &trace;
    MdkrReservedTerminal<ThreadPool, Resource> terminal;
    explicit Resource(Trace &value)
        : trace(value), terminal(ThreadPool::Instance(), [](Resource &owner) {
            ++owner.trace.completed;
            if (owner.trace.throwCompletion) throw 31;
        }) {}
    ~Resource() override {
        require(std::this_thread::get_id() != trace.caller, "transport owner deletion remains off caller");
        trace.destroyed = true;
    }
};
using Retirement = MdkrTransportRetirement<Processor, std::shared_ptr<int>, Base>;

void reservedTerminalOwnsThroughCompletion(bool throws) {
    Workers workers;
    Trace trace;
    trace.throwCompletion = throws;
    auto token = std::make_shared<int>(0);
    auto resource = Retirement::make<Resource>(workers.retirement, token, trace);
    const auto before = refusedAllocations.load();
    const auto failures = workers.pool.preparedTaskFailures();
    refuseAllocation = true;
    resource->terminal.request(resource);
    resource->terminal.request(resource);
    resource.reset();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!trace.destroyed && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
    require(trace.destroyed, "terminal strong owner releases to reserved deletion");
    workers.retirement.join();
    workers.pool.join(); // Also waits for outer task-failure bookkeeping.
    refuseAllocation = false;
    require(trace.completed == 1u && refusedAllocations == before, "one-shot terminal dispatch does not allocate");
    require(workers.pool.preparedTaskFailures() == failures + unsigned(throws),
            "terminal task exception is contained without suppressing owner retirement");
}

void unusedTerminalAndRefusedPreparationHaveNoCycle() {
    Workers workers;
    Trace trace;
    auto token = std::make_shared<int>(0);
    {
        auto resource = Retirement::make<Resource>(workers.retirement, token, trace);
    }
    workers.retirement.join();
    require(trace.destroyed && trace.completed == 0u, "unused terminal reservation does not retain its owner");
    bool rejected = false;
    refuseAllocation = true;
    try {
        MdkrReservedTerminal<ThreadPool, Resource> terminal(workers.pool, [](Resource &) {});
    } catch (const std::bad_alloc &) { rejected = true; }
    refuseAllocation = false;
    require(rejected, "terminal record preparation fails before admitting any owner");
}

void checkedTimerRefusalAndRetry() {
    Workers workers;
    bool rejected = false;
    const auto before = refusedAllocations.load();
    refuseAllocation = true;
    try {
        (void)workers.pool.scheduleChecked(ThreadPool::clock::now(), [] {});
    } catch (const std::bad_alloc &) { rejected = true; }
    refuseAllocation = false;
    require(rejected && refusedAllocations == before + 1u,
            "checked timer preparation refusal is catchable before publication");
    auto retry = workers.pool.scheduleChecked(ThreadPool::clock::now(), [] { return 23; });
    require(retry.wait_for(std::chrono::seconds(5)) == std::future_status::ready && retry.get() == 23,
            "checked timer retry runs on the actual scheduler");
}

void futureTimerDoesNotRetainTransportEpoch() {
    Workers workers;
    Trace trace;
    auto token = std::make_shared<int>(0);
    std::weak_ptr<int> weakToken = token;
    auto resource = Retirement::make<Resource>(workers.retirement, token, trace);
    std::weak_ptr<Resource> weakOwner = resource;
    auto future = workers.pool.scheduleChecked(ThreadPool::clock::now() + std::chrono::hours(1),
                                              [weakOwner] { (void)weakOwner.lock(); });
    token.reset();
    resource.reset();
    workers.retirement.join();
    require(trace.destroyed && weakOwner.expired() && weakToken.expired(),
            "pending weak timer/controlblock does not retain retired resource or epoch");
    require(future.wait_for(std::chrono::seconds(0)) == std::future_status::timeout,
            "future deadline is not executed as immediate prepared work");
}
} // namespace

int main() {
    normalWakeOwnership();
    preparationRollbackAndMoveAssignment();
    obsoleteTimerDoesNotConsumeCount();
    coalescedDemandSurvivesStaleTimer();
    terminalSealAndPriority();
    admissionOperationsDoNotAllocate();
    reservedTerminalOwnsThroughCompletion(false);
    reservedTerminalOwnsThroughCompletion(true);
    unusedTerminalAndRefusedPreparationHaveNoCycle();
    checkedTimerRefusalAndRetry();
    futureTimerDoesNotRetainTransportEpoch();
    std::puts("PASS RTC receive work: 11 groups");
}
