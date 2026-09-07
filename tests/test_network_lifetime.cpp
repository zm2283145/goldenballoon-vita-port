#include "net/network_lifetime.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <thread>
#include <type_traits>

namespace {
std::atomic<bool> refuseAllocation{false};
std::atomic<unsigned> allocations{0};
void expect(bool value, const char *message) {
    if (!value) {
        std::fprintf(stderr, "network lifetime: %s\n", message);
        std::abort();
    }
}
}

void *operator new(std::size_t size) {
    ++allocations;
    if (refuseAllocation.load()) throw std::bad_alloc();
    if (void *value = std::malloc(size ? size : 1)) return value;
    throw std::bad_alloc();
}
void *operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void *value) noexcept { std::free(value); }
void operator delete[](void *value) noexcept { std::free(value); }
void operator delete(void *value, std::size_t) noexcept { std::free(value); }
void operator delete[](void *value, std::size_t) noexcept { std::free(value); }

namespace {
struct Operations {
    static constexpr bool required = true;
    inline static std::atomic<unsigned> starts{0}, stops{0}, live{0};
    inline static bool allowStart = true;
    static bool start() noexcept {
        ++starts;
        if (!allowStart) return false;
        ++live;
        return true;
    }
    static void stop() noexcept {
        expect(live.fetch_sub(1) != 0, "release always owns a successful acquisition");
        ++stops;
    }
};
using Lease = MdkrSharedNetworkLease<Operations>;
static_assert(std::is_nothrow_copy_constructible_v<Lease>);
static_assert(std::is_nothrow_move_constructible_v<Lease>);

struct NoOperations {
    static constexpr bool required = false;
    static bool start() noexcept { expect(false, "no-op path cannot start"); return false; }
    static void stop() noexcept { expect(false, "no-op path cannot stop"); }
};

struct FakeSocketApi {
    inline static bool allowStart = false, allowCleanup = true;
    inline static uint16_t version = 0x0202u;
    inline static unsigned starts = 0, cleanups = 0;
    static bool startup(uint16_t &actual) noexcept {
        ++starts;
        actual = version;
        return allowStart;
    }
    static bool cleanup() noexcept { ++cleanups; return allowCleanup; }
};

void nativePolicyRefusalVersionAndStickyCleanupFailure() {
    using NativeLease = MdkrSharedNetworkLease<MdkrWinsockReferencePolicy<FakeSocketApi>>;
    NativeLease owner;
    expect(!owner.acquire() && FakeSocketApi::cleanups == 0,
           "OS startup refusal cannot call cleanup");
    FakeSocketApi::allowStart = true;
    FakeSocketApi::version = 0x0101u;
    expect(!owner.acquire() && FakeSocketApi::cleanups == 1,
           "unsupported negotiated API balances its successful acquisition once");
    FakeSocketApi::version = 0x0202u;
    expect(owner.acquire(), "supported version can retry after prior refusal");
    auto retained = owner;
    owner.reset();
    FakeSocketApi::allowCleanup = false;
    retained.reset();
    expect(FakeSocketApi::cleanups == 2 && mdkrFirstPartyNetworkCleanupFailed.load(),
           "last-owner cleanup failure becomes sticky terminal evidence");
    const auto starts = FakeSocketApi::starts;
    expect(!owner.acquire() && FakeSocketApi::starts == starts,
           "failed cleanup prevents new OS admission, not just successful reporting");
    // Test isolation only: production has no reset/retry of a failed cleanup.
    mdkrFirstPartyNetworkCleanupFailed.store(false);
}

void acquisitionRefusalAndRetry() {
    Lease owner;
    refuseAllocation = true;
    expect(!owner.acquire() && !owner, "allocation refusal does not publish ownership");
    refuseAllocation = false;
    expect(Operations::starts == 0 && Operations::stops == 0,
           "allocation precedes any native acquisition");
    Operations::allowStart = false;
    expect(!owner.acquire() && !owner, "refused startup remains retryable");
    expect(Operations::starts == 1 && Operations::stops == 0,
           "failed startup has no balancing release");
    Operations::allowStart = true;
    expect(owner.acquire() && owner.acquire(), "retry succeeds and repeat acquire is idempotent");
    expect(Operations::starts == 2 && Operations::live == 1, "one owned acquisition");
    const auto before = allocations.load();
    refuseAllocation = true;
    Lease copy = owner;
    Lease moved = std::move(copy);
    expect(!copy && moved && moved.acquire(), "copied/moved ownership requires no new acquisition");
    owner.reset();
    owner.reset();
    expect(Operations::live == 1 && Operations::stops == 0, "retained work outlives its caller");
    moved.reset();
    refuseAllocation = false;
    expect(allocations == before && Operations::stops == 1 && Operations::live == 0,
           "last copy releases exactly once without allocating");
}

void resolverAndIndependentCopies() {
    Lease owner;
    expect(owner.acquire(), "caller owns native prerequisites");
    struct Result {
        ~Result() { expect(Operations::live != 0, "address result frees before prerequisites"); }
    };
    struct Resolver {
        Lease network;
        Result result;
    };
    auto task = std::make_shared<Resolver>(Resolver{owner, {}});
    owner.reset();
    expect(Operations::live == 1, "abandoned resolver keeps prerequisite alive");
    std::array<std::thread, 8> workers;
    for (auto &worker : workers) {
        worker = std::thread([copy = task->network]() mutable {
            for (unsigned index = 0; index < 1000; ++index) {
                Lease nested = copy;
                expect(nested.acquire(), "independent copied owner remains admitted");
            }
            copy.reset();
        });
    }
    for (auto &worker : workers) worker.join();
    expect(Operations::live == 1, "joining copies does not release retained resolver");
    task.reset();
    expect(Operations::live == 0, "result retirement releases final resolver lease");
    for (unsigned index = 0; index < 100; ++index) {
        Lease session;
        expect(session.acquire(), "new session can acquire after previous retirement");
    }
    expect(Operations::starts == Operations::stops + 1,
           "only the deliberately refused startup is unmatched, not successful sessions");
}
}

int main() {
    acquisitionRefusalAndRetry();
    resolverAndIndependentCopies();
    nativePolicyRefusalVersionAndStickyCleanupFailure();
    MdkrSharedNetworkLease<NoOperations> noop;
    const auto before = allocations.load();
    refuseAllocation = true;
    expect(noop.acquire() && noop, "non-Windows policy requires no acquisition");
    auto copy = noop;
    copy.reset();
    noop.reset();
    refuseAllocation = false;
    expect(allocations == before, "no-op platform adds no allocation");
    return 0;
}
