// Actual patched serial RTC dispatcher plus the production ownership helper.
// Only ordinary throwing new/new[] are intercepted, not malloc, over-aligned
// allocations or exception-runtime storage. No network or app session is opened.
#include "impl/processor.hpp"
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
thread_local int refuseAfter = -1; // Calling-thread preparation ordinal only.
void require(bool value, const char *message) {
    if (!value) {
        std::fprintf(stderr, "RTC transport retirement: %s\n", message);
        std::abort();
    }
}
}

void *operator new(std::size_t size) {
    if (refuseAllocation.load() || refuseAfter == 0) {
        ++refusedAllocations;
        refuseAfter = -1;
        throw std::bad_alloc();
    }
    if (refuseAfter > 0) --refuseAfter;
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
using rtc::impl::TearDownProcessor;
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
                "worker reaches held body");
    }
    void release() {
        std::lock_guard<std::mutex> lock(mutex);
        released = true;
        condition.notify_all();
    }
};

void awaitFlag(const std::atomic<bool> &flag, const char *message) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!flag.load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    require(flag.load(), message);
}

struct Workers {
    ThreadPool &pool = ThreadPool::Instance();
    Processor &retirement = TearDownProcessor::Instance();
    Gate first, second;
    Workers() {
        pool.spawn(2);
        pool.enqueue([this] { first.hold(); });
        pool.enqueue([this] { second.hold(); });
        // Both workers must have completed optional naming before refusal.
        first.awaitEntry();
        second.awaitEntry();
        first.release();
        second.release();
    }
    void drain() { retirement.join(); }
    ~Workers() {
        refuseAllocation = false;
        drain();
        pool.join();
        pool.clear();
    }
};

struct Epoch {
    std::atomic<bool> &retired;
    explicit Epoch(std::atomic<bool> &value) : retired(value) {}
    ~Epoch() { retired = true; }
};
struct Probe {
    const std::thread::id caller = std::this_thread::get_id();
    std::atomic<unsigned> constructed{0}, stopped{0}, destroyed{0};
    std::atomic<bool> destructionStarted{false};
    Gate *destructionGate = nullptr;
    bool throwOnStop = false;
};
struct ResourceBase {
    virtual ~ResourceBase() = default;
    virtual void stop() = 0;
};
struct Resource final : ResourceBase, std::enable_shared_from_this<Resource> {
    Probe &probe;
    explicit Resource(Probe &value, bool refuseControlBlock = false) : probe(value) {
        ++probe.constructed;
        // Constructor completion precedes shared_ptr's separate control block.
        if (refuseControlBlock) refuseAllocation = true;
    }
    ~Resource() override {
        require(std::this_thread::get_id() != probe.caller,
                "complete object deletion is off the originating caller thread");
        probe.destructionStarted = true;
        if (probe.destructionGate) probe.destructionGate->hold();
        ++probe.destroyed;
    }
    void stop() override {
        require(std::this_thread::get_id() != probe.caller,
                "reserved stop is off the originating caller thread");
        ++probe.stopped;
        if (probe.throwOnStop) throw 19;
    }
};
using Retirement = MdkrTransportRetirement<Processor, std::shared_ptr<Epoch>, ResourceBase>;

void reservationRefusalPrecedesConstruction() {
    Workers workers;
    std::atomic<bool> epochRetired{false};
    auto epoch = std::make_shared<Epoch>(epochRetired);
    Probe probe;
    // Current preparation creates State, deletion node, stop node, then T.
    // Refuse each of those ordinary allocations before a resource constructor
    // can run; the separate case below refuses the final shared control block.
    for (int ordinal = 0; ordinal < 4; ++ordinal) {
        const unsigned before = refusedAllocations.load();
        refuseAfter = ordinal;
        bool refused = false;
        try { (void)Retirement::make<Resource>(workers.retirement, epoch, probe); }
        catch (const std::bad_alloc &) { refused = true; }
        refuseAfter = -1;
        require(refused && refusedAllocations == before + 1u && probe.constructed == 0u,
                "state/node/object allocation refusal acquires no transport resource");
    }
    auto retry = Retirement::make<Resource>(workers.retirement, epoch, probe);
    retry.reset();
    workers.drain();
    require(probe.constructed == 1u && probe.destroyed == 1u,
            "retry after reservation refusal retires exactly once");
}

void controlBlockRefusalRetiresTransferredResource() {
    Workers workers;
    std::atomic<bool> epochRetired{false};
    auto epoch = std::make_shared<Epoch>(epochRetired);
    Probe probe;
    bool callerOwnsResource = true;
    const unsigned before = refusedAllocations.load();
    bool refused = false;
    try {
        (void)Retirement::makeWith<Resource>(workers.retirement, epoch, [&] {
            auto *completed = new Resource(probe, true);
            // Same handoff boundary as accepted sockets: the caller must not
            // close a resource now owned by a fully constructed transport.
            callerOwnsResource = false;
            return completed;
        });
    } catch (const std::bad_alloc &) { refused = true; }
    workers.drain();
    refuseAllocation = false;
    require(refused && !callerOwnsResource && probe.destroyed == 1u,
            "control-block refusal publishes the completed transferred resource for deletion");
    require(refusedAllocations == before + 1u,
            "the refused control block is the only ordinary-new refusal");
}

void weakControlBlockDoesNotRetainRetiredEpoch() {
    Workers workers;
    std::atomic<bool> epochRetired{false};
    auto epoch = std::make_shared<Epoch>(epochRetired);
    Probe probe;
    auto owner = Retirement::make<Resource>(workers.retirement, epoch, probe);
    require(owner->shared_from_this() == owner, "standard enable_shared_from_this is preserved");
    std::weak_ptr<Resource> weak = owner;
    epoch.reset();
    const unsigned before = refusedAllocations.load();
    refuseAllocation = true;
    owner.reset();
    require(weak.expired(), "weak owner expires at last strong release before delayed deletion");
    workers.drain();
    refuseAllocation = false;
    require(probe.destroyed == 1u && epochRetired.load(),
            "deletion releases epoch even while an expired weak control block survives");
    require(refusedAllocations == before, "reserved final deletion requires no ordinary new");
    weak.reset(); // Inert pointer-only deleter must not touch its retired state.
}

void stopReservationIsOneShotAndRetainsOwner() {
    Workers workers;
    std::atomic<bool> epochRetired{false};
    auto epoch = std::make_shared<Epoch>(epochRetired);
    Probe probe;
    auto owner = Retirement::make<Resource>(workers.retirement, epoch, probe);
    const unsigned before = refusedAllocations.load();
    refuseAllocation = true;
    require(Retirement::requestStop(owner), "owned transport admits its reserved stop");
    require(Retirement::requestStop(owner), "duplicate stop does not need another record");
    owner.reset();
    workers.drain();
    refuseAllocation = false;
    require(probe.stopped == 1u && probe.destroyed == 1u,
            "stop retains transport until completion then deletes exactly once");
    require(refusedAllocations == before, "stop and final-delete handoffs require no ordinary new");
}

void throwingStopStillReleasesOwnerAndAdvancesRetirement() {
    Workers workers;
    std::atomic<bool> epochRetired{false};
    auto epoch = std::make_shared<Epoch>(epochRetired);
    Probe probe;
    probe.throwOnStop = true;
    auto owner = Retirement::make<Resource>(workers.retirement, epoch, probe);
    const auto before = workers.pool.preparedTaskFailures();
    require(Retirement::requestStop(owner), "throwing stop is admitted before invocation");
    owner.reset();
    workers.drain();
    require(probe.stopped == 1u && probe.destroyed == 1u,
            "throwing stop releases task-local ownership and cannot strand deletion");
    // Processor continuation may publish its successor before runOne catches
    // the preceding body's exception; await pool epilogues before its counter.
    workers.pool.join();
    require(workers.pool.preparedTaskFailures() == before + 1u,
            "stop exception remains observable through the real worker failure counter");
}

void postOwnershipInitializationFailureRetiresOffThread() {
    Workers workers;
    std::atomic<bool> epochRetired{false};
    auto epoch = std::make_shared<Epoch>(epochRetired);
    Probe probe;
    bool refused = false;
    try {
        auto owner = Retirement::make<Resource>(workers.retirement, epoch, probe);
        (void)owner;
        throw 29; // A resource initialization step after standard shared ownership.
    } catch (int value) { refused = value == 29; }
    workers.drain();
    require(refused && probe.destroyed == 1u,
            "post-ownership initialization failure uses reserved off-thread retirement");
}

void stopBeforeStartRefusesStartAdmission() {
    Workers workers;
    std::atomic<bool> epochRetired{false};
    auto epoch = std::make_shared<Epoch>(epochRetired);
    Probe probe;
    auto owner = Retirement::make<Resource>(workers.retirement, epoch, probe);
    require(Retirement::requestStop(owner), "early terminal stop is reserved");
    require(!Retirement::beginStart(owner), "a stopped resource cannot start afterward");
    owner.reset();
    workers.drain();
    require(probe.stopped == 1u && probe.destroyed == 1u,
            "start refusal preserves exactly-once terminal retirement");
}

void stopDuringStartWaitsForStartCompletion() {
    Workers workers;
    std::atomic<bool> epochRetired{false};
    auto epoch = std::make_shared<Epoch>(epochRetired);
    Probe probe;
    auto owner = Retirement::make<Resource>(workers.retirement, epoch, probe);
    require(Retirement::beginStart(owner), "new transport admits its start");
    require(Retirement::requestStop(owner), "recursive close during start retains its owner");
    workers.drain(); // Must not publish stop before finishStart; this is not a sleep heuristic.
    require(probe.stopped == 0u, "stop cannot overtake an admitted start");
    const unsigned before = refusedAllocations.load();
    refuseAllocation = true;
    Retirement::finishStart(owner);
    owner.reset();
    workers.drain();
    refuseAllocation = false;
    require(probe.stopped == 1u && probe.destroyed == 1u,
            "completed start hands pending stop to the serial lane exactly once");
    require(refusedAllocations == before, "start completion publishes reserved stop without ordinary new");
}

void throwingStartCompletesPendingStopThroughTypedGuard() {
    Workers workers;
    std::atomic<bool> epochRetired{false};
    auto epoch = std::make_shared<Epoch>(epochRetired);
    Probe probe;
    auto owner = Retirement::make<Resource>(workers.retirement, epoch, probe);
    require(Retirement::beginStart(owner), "throwing start is admitted");
    bool failed = false;
    try {
        MdkrNoAllocScopeExit guard([owner]() noexcept { Retirement::finishStart(owner); });
        require(Retirement::requestStop(owner), "close while starting retains its stop request");
        throw 37;
    } catch (int value) { failed = value == 37; }
    owner.reset();
    workers.drain();
    require(failed && probe.stopped == 1u && probe.destroyed == 1u,
            "start exception cannot strand pending stop or final ownership");
}

void blockingDestructorsRemainSerializedWhilePoolProgresses() {
    Workers workers;
    std::atomic<bool> epochRetired{false};
    auto epoch = std::make_shared<Epoch>(epochRetired);
    Probe first, second;
    Gate destruction;
    first.destructionGate = &destruction;
    auto a = Retirement::make<Resource>(workers.retirement, epoch, first);
    auto b = Retirement::make<Resource>(workers.retirement, epoch, second);
    a.reset();
    destruction.awaitEntry();
    b.reset();
    std::atomic<bool> sentinel{false};
    // A later pool task proves another worker can progress. If deletion had
    // escaped the serial lane, its earlier immediate publication would run
    // before this later timed task on that other worker.
    workers.pool.enqueue([&] { sentinel = true; });
    awaitFlag(sentinel, "ordinary pool work progresses while one destructor is held");
    require(!second.destructionStarted.load(),
            "second destructor cannot overlap the first serialized task body");
    destruction.release();
    workers.drain();
    require(first.destroyed == 1u && second.destroyed == 1u,
            "both resources retire after releasing the held destructor");
}
}

int main() {
    reservationRefusalPrecedesConstruction();
    controlBlockRefusalRetiresTransferredResource();
    weakControlBlockDoesNotRetainRetiredEpoch();
    stopReservationIsOneShotAndRetainsOwner();
    throwingStopStillReleasesOwnerAndAdvancesRetirement();
    postOwnershipInitializationFailureRetiresOffThread();
    stopBeforeStartRefusesStartAdmission();
    stopDuringStartWaitsForStartCompletion();
    throwingStartCompletesPendingStopThroughTypedGuard();
    blockingDestructorsRemainSerializedWhilePoolProgresses();
    return 0;
}
