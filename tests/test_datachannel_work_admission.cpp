/* Assert-driven test: release builds must not compile its checks away. */
#undef NDEBUG

#include "queued_work_admission.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <memory>
#include <mutex>
#include <new>
#include <queue>
#include <type_traits>

namespace {
// This fixture creates no threads. Allocation refusal is enabled only around
// extraction of work that has already been admitted, never around its setup.
bool refuseAllocation = false;
std::size_t allocations = 0;
void expect(bool value, const char *message) {
    if (!value) {
        std::fprintf(stderr, "FAIL datachannel_work_admission: %s\n", message);
        std::abort();
    }
}
}

void *operator new(std::size_t size) {
    ++allocations;
    if (refuseAllocation) throw std::bad_alloc();
    if (void *value = std::malloc(size ? size : 1)) return value;
    throw std::bad_alloc();
}
void *operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void *value) noexcept { std::free(value); }
void operator delete[](void *value) noexcept { std::free(value); }
void operator delete(void *value, std::size_t) noexcept { std::free(value); }
void operator delete[](void *value, std::size_t) noexcept { std::free(value); }

namespace {
using Clock = std::chrono::steady_clock;
using Work = MdkrScheduledWork<Clock::time_point>;
static_assert(noexcept(std::declval<const Work &>().takeCallable()));
static_assert(std::is_nothrow_move_constructible_v<Work>);

struct Owner {
    Owner(bool &locked, unsigned &destroyed) : locked(locked), destroyed(destroyed) {}
    ~Owner() {
        expect(!locked, "captured owner is destroyed outside scheduler lock");
        ++destroyed;
    }
    bool &locked;
    unsigned &destroyed;
};

struct LargeCallable {
    LargeCallable(std::shared_ptr<Owner> owner, unsigned &copies, unsigned &calls,
                  bool &refuseCopy)
        : owner(std::move(owner)), copies(copies), calls(calls), refuseCopy(refuseCopy) {}
    LargeCallable(const LargeCallable &other)
        : owner(other.owner), copies(other.copies), calls(other.calls),
          refuseCopy(other.refuseCopy), padding(other.padding) {
        ++copies;
        if (refuseCopy) throw 19;
    }
    LargeCallable(LargeCallable &&) noexcept = default;
    void operator()() { ++calls; }
    std::shared_ptr<Owner> owner;
    unsigned &copies;
    unsigned &calls;
    bool &refuseCopy;
    std::array<unsigned char, 2048> padding{};
};

void acceptedWorkExtractionDoesNotAllocateOrCopy() {
    // This is the production Task and the production container/comparator, not
    // a replacement extraction implementation. The patch binds dequeue to it.
    std::priority_queue<Work, std::deque<Work>, std::greater<Work>> queue;
    std::mutex mutex;
    bool locked = false, refuseCopy = false;
    unsigned copies = 0, calls = 0, destroyed = 0;
    for (int time : {90, 10, 40}) {
        auto owner = std::make_shared<Owner>(locked, destroyed);
        queue.push({Clock::time_point(std::chrono::seconds(time)),
                    LargeCallable(std::move(owner), copies, calls, refuseCopy)});
    }
    const unsigned admittedCopies = copies;
    refuseCopy = true;
    for (int time : {10, 40, 90}) {
        std::function<void()> callable;
        const unsigned beforeDestroyed = destroyed;
        {
            std::lock_guard<std::mutex> lock(mutex);
            locked = true;
            const auto priority = queue.top().time;
            expect(priority == Clock::time_point(std::chrono::seconds(time)),
                   "deadline ordering is unchanged");
            const auto beforeAllocations = allocations;
            refuseAllocation = true;
            callable = queue.top().takeCallable();
            expect(!queue.top().func && queue.top().time == priority,
                   "extraction empties only the payload, not the heap key");
            queue.pop();
            refuseAllocation = false;
            expect(allocations == beforeAllocations && copies == admittedCopies,
                   "accepted extraction and heap pop do not allocate or copy");
            expect(destroyed == beforeDestroyed, "pop retains the extracted owner");
            locked = false;
        }
        callable();
        callable = nullptr;
        expect(destroyed == beforeDestroyed + 1, "owner retires exactly once after extraction");
    }
    expect(queue.empty() && calls == 3 && destroyed == 3, "all admitted work executes once");
    const Work empty{Clock::time_point{}, {}};
    expect(!empty.takeCallable(), "empty payload extraction remains empty");
}

struct AllocationState { bool refuse = false; unsigned attempts = 0; };
template <typename T> struct RefusingAllocator {
    using value_type = T;
    AllocationState *state;
    explicit RefusingAllocator(AllocationState &value) noexcept : state(&value) {}
    template <typename U>
    RefusingAllocator(const RefusingAllocator<U> &other) noexcept : state(other.state) {}
    T *allocate(std::size_t count) {
        ++state->attempts;
        if (state->refuse) throw std::bad_alloc();
        return std::allocator<T>{}.allocate(count);
    }
    void deallocate(T *value, std::size_t count) noexcept {
        std::allocator<T>{}.deallocate(value, count);
    }
    template <typename U> bool operator==(const RefusingAllocator<U> &other) const noexcept {
        return state == other.state;
    }
    template <typename U> bool operator!=(const RefusingAllocator<U> &other) const noexcept {
        return !(*this == other);
    }
};

struct Item {
    explicit Item(std::size_t value, bool &refuseMove) : weight(value), refuseMove(refuseMove) {}
    Item(Item &&other) : weight(other.weight), refuseMove(other.refuseMove) {
        if (refuseMove) throw 23;
    }
    std::size_t weight;
    bool &refuseMove;
    std::array<unsigned char, 2048> padding{};
};

void accountingCommitsOnlyAfterRealInsertion() {
    AllocationState allocation;
    using Storage = std::deque<Item, RefusingAllocator<Item>>;
    std::queue<Item, Storage> queue{Storage{RefusingAllocator<Item>(allocation)}};
    std::size_t amount = 0;
    bool refuseMove = false;
    const auto measure = [](const Item &item) { return item.weight; };
    Item first(7, refuseMove);
    mdkrCommitQueueInsertion(queue, amount, std::move(first), measure);
    expect(queue.size() == 1 && amount == 7, "successful admission accounts once");

    // Deques may reserve slots in blocks. Refuse their next actual allocation,
    // checking every accepted insertion before it; no guessed block size.
    allocation.refuse = true;
    bool rejected = false;
    for (unsigned index = 0; index < 4096 && !rejected; ++index) {
        const auto oldSize = queue.size(), oldAmount = amount;
        Item next(3, refuseMove);
        try { mdkrCommitQueueInsertion(queue, amount, std::move(next), measure); }
        catch (const std::bad_alloc &) {
            rejected = true;
            expect(queue.size() == oldSize && amount == oldAmount,
                   "real container allocation refusal leaves accounting unchanged");
        }
        if (!rejected) expect(queue.size() == oldSize + 1 && amount == oldAmount + 3,
                              "already reserved slots still account correctly");
    }
    expect(rejected && queue.front().weight == 7, "refusal preserves existing queue entries");
    allocation.refuse = false;
    const auto oldSize = queue.size(), oldAmount = amount;
    const auto oldAttempts = allocation.attempts;
    Item next(5, refuseMove);
    try {
        mdkrCommitQueueInsertion(queue, amount, std::move(next),
            [](const Item &) -> std::size_t { throw 29; });
        expect(false, "measure refusal propagates");
    } catch (int error) { expect(error == 29, "original measure error preserved"); }
    expect(queue.size() == oldSize && amount == oldAmount && allocation.attempts == oldAttempts,
           "measure failure precedes container mutation");
    refuseMove = true;
    try {
        mdkrCommitQueueInsertion(queue, amount, std::move(next), measure);
        expect(false, "move refusal propagates");
    } catch (int error) { expect(error == 23, "original element move error preserved"); }
    expect(queue.size() == oldSize && amount == oldAmount,
           "unsuccessful element construction does not commit amount");
    refuseMove = false;
    mdkrCommitQueueInsertion(queue, amount, std::move(next), measure);
    expect(queue.size() == oldSize + 1 && amount == oldAmount + 5,
           "retry after refusal commits once");
}
}

int main() {
    acceptedWorkExtractionDoesNotAllocateOrCopy();
    accountingCommitsOnlyAfterRealInsertion();
    return 0;
}
