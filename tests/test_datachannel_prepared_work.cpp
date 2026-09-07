/* Assert-driven test: release builds must not compile its checks away. */
#undef NDEBUG

#include "prepared_work_queue.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <new>
#include <type_traits>

namespace {
// No threads in this helper fixture. Refusal surrounds accepted queue/guard
// operations only; the separate vendor fixture exercises the real dispatcher.
// Only ordinary throwing new/new[] are intercepted, not malloc, over-aligned
// allocation or exception-runtime storage. Explicit bad_alloc throws test
// continuation rather than the body allocator's refusal behavior.
bool refuseAllocation = false;
std::size_t allocations = 0;
void expect(bool value, const char *message) {
    if (!value) {
        std::fprintf(stderr, "FAIL datachannel_prepared_work: %s\n", message);
        std::abort();
    }
}
}

void *operator new(std::size_t size) {
    ++allocations;
    if (refuseAllocation) throw std::bad_alloc();
    if (void *value = std::malloc(size ? size : 1u)) return value;
    throw std::bad_alloc();
}
void *operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void *value) noexcept { std::free(value); }
void operator delete[](void *value) noexcept { std::free(value); }
void operator delete(void *value, std::size_t) noexcept { std::free(value); }
void operator delete[](void *value, std::size_t) noexcept { std::free(value); }

namespace {
using Queue = MdkrPreparedWorkQueue<uint64_t>;
using Dispatch = MdkrPreparedDispatch<uint64_t>;
static_assert(noexcept(std::declval<Queue &>().pop()));
static_assert(noexcept(std::declval<Queue &>().push(std::declval<Queue::Pointer>(), 0u)));
static_assert(std::is_nothrow_move_constructible_v<Dispatch>);

struct Owner {
    Owner(bool &schedulerLocked, bool &processorLocked, bool &pending, unsigned &retired)
        : schedulerLocked(schedulerLocked), processorLocked(processorLocked),
          pending(pending), retired(retired) {}
    ~Owner() {
        expect(!schedulerLocked && !processorLocked, "owner release is outside both locks");
        expect(!pending, "final owner observes settled pending state before joining");
        ++retired;
    }
    bool &schedulerLocked;
    bool &processorLocked;
    bool &pending;
    unsigned &retired;
};

void acceptedNodesPublishPopAndDestroyWithoutAllocation() {
    Queue queue;
    std::array<Queue::Pointer, 3> prepared;
    std::array<unsigned, 3> observed{};
    unsigned calls = 0;
    bool schedulerLocked = false, processorLocked = false, pending = false;
    unsigned retired = 0;
    for (unsigned index = 0; index < prepared.size(); ++index) {
        auto owner = std::make_unique<Owner>(schedulerLocked, processorLocked, pending, retired);
        prepared[index] = Queue::prepare([owner = std::move(owner), index, &calls, &observed]() {
            (void)owner;
            expect(calls < observed.size(), "accepted callable runs at most once");
            observed[calls++] = index;
        });
    }
    const auto before = allocations;
    refuseAllocation = true;
    schedulerLocked = true;
    for (unsigned index = 0; index < prepared.size(); ++index) {
        queue.push(std::move(prepared[index]), 10u + index);
    }
    schedulerLocked = false;
    expect(queue.size() == 3u, "all prepared records are admitted");
    for (unsigned index = 0; index < prepared.size(); ++index) {
        Dispatch dispatch;
        schedulerLocked = true;
        expect(queue.front().time == 10u + index, "FIFO retains publication timestamp");
        dispatch.prepared = queue.pop();
        expect(!dispatch.prepared->next, "pop detaches successor ownership");
        expect(retired == index, "dequeue has not destroyed current or successor owner");
        schedulerLocked = false;
        dispatch();
        dispatch.prepared.reset();
        expect(retired == index + 1u, "each move-only owner retires exactly once");
    }
    expect(!queue.pop(), "empty pop is allocation free");
    refuseAllocation = false;
    expect(allocations == before, "publish/dequeue/dispatch/destruction allocate nothing");
    expect(calls == 3u && observed == std::array<unsigned, 3>{0u, 1u, 2u}, "accepted FIFO is exact");
}

void refusedPreparationLeavesAcceptedQueueUnchanged() {
    Queue queue;
    unsigned calls = 0;
    queue.push(Queue::prepare([&calls]() { ++calls; }), 1u);
    const auto beforeSize = queue.size();
    refuseAllocation = true;
    bool refused = false;
    try { (void)Queue::prepare([&calls]() { ++calls; }); }
    catch (const std::bad_alloc &) { refused = true; }
    refuseAllocation = false;
    expect(refused && queue.size() == beforeSize, "preparation refusal changes no accepted state");
    auto retry = Queue::prepare([&calls]() { ++calls; });
    queue.push(std::move(retry), 2u);
    while (auto node = queue.pop()) node->invoke();
    expect(calls == 2u, "accepted work and later successful retry both execute once");
}

void typedContinuationProgressesAfterEveryTaskException() {
    Queue waiting, ready;
    bool schedulerLocked = false, processorLocked = false, pending = true;
    unsigned retired = 0, completed = 0, exceptions = 0;
    std::array<unsigned, 3> observed{};
    auto owner = std::make_shared<Owner>(schedulerLocked, processorLocked, pending, retired);
    // This fixture composes the exact production queue/typed guard. It does not
    // stand in for the actual Processor's mutex/join implementation: the vendor
    // fixture separately binds those behaviors and concurrent lifetime paths.
    auto continuation = [&]() noexcept {
        processorLocked = true;
        auto next = waiting.pop();
        if (next) {
            schedulerLocked = true;
            ready.push(std::move(next), 100u + completed);
            schedulerLocked = false;
        } else {
            pending = false;
        }
        processorLocked = false;
    };
    for (unsigned index = 0; index < 3u; ++index) {
        auto node = Queue::prepare([owner, index, &completed, &observed, &continuation]() {
            (void)owner;
            MdkrNoAllocScopeExit guard(continuation);
            expect(completed < observed.size(), "continuation runs each callable at most once");
            observed[completed++] = index;
            if (index == 0u) throw 31; // non-std exception still chains next work
            if (index == 1u) throw std::bad_alloc();
        });
        if (index == 0u) ready.push(std::move(node), 100u);
        else waiting.push(std::move(node), 100u + index);
    }
    owner.reset(); // accepted callbacks now hold the only strong owners
    const auto before = allocations;
    refuseAllocation = true;
    while (!ready.empty()) {
        Dispatch dispatch;
        schedulerLocked = true;
        dispatch.prepared = ready.pop();
        schedulerLocked = false;
        try { dispatch(); }
        catch (...) { ++exceptions; }
        dispatch.prepared.reset();
    }
    refuseAllocation = false;
    expect(allocations == before, "typed guard and accepted continuation have no new allocation");
    expect(exceptions == 2u && completed == 3u, "std and non-std throws cannot strand progress");
    expect(observed == std::array<unsigned, 3>{0u, 1u, 2u}, "continuation preserves accepted FIFO");
    expect(!pending && waiting.empty() && retired == 1u, "final owner release follows pending settlement");
}

void queueDestructionIsIterativeAndOutsideCallerLock() {
    bool schedulerLocked = false, processorLocked = false, pending = false;
    unsigned retired = 0;
    {
        Queue queue;
        for (unsigned index = 0; index < 128u; ++index) {
            auto owner = std::make_unique<Owner>(schedulerLocked, processorLocked, pending, retired);
            queue.push(Queue::prepare([owner = std::move(owner)]() { (void)owner; }), index);
        }
        refuseAllocation = true;
    }
    refuseAllocation = false;
    expect(retired == 128u, "iterative queue destruction retires every node without allocation");
}
} // namespace

int main() {
    acceptedNodesPublishPopAndDestroyWithoutAllocation();
    refusedPreparationLeavesAcceptedQueueUnchanged();
    typedContinuationProgressesAfterEveryTaskException();
    queueDestructionIsIterativeAndOutsideCallerLock();
    return 0;
}
