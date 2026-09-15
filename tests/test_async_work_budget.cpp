/* Assert-driven test: release builds must not compile its checks away. */
#undef NDEBUG

#include "online/async_work_budget.h"

#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <vector>

AsyncWorkBudget &resolverBudgetFromOtherTranslationUnit();
AsyncWorkBudget::Permit acquireResolverPermitFromOtherTranslationUnit();
std::size_t observeResolverWorkFromOtherTranslationUnit() noexcept;

namespace {
using Permit = AsyncWorkBudget::Permit;
static_assert(!std::is_copy_constructible_v<Permit>);
static_assert(!std::is_copy_assignable_v<Permit>);
static_assert(std::is_nothrow_move_constructible_v<Permit>);
static_assert(std::is_nothrow_move_assignable_v<Permit>);
static_assert(!std::is_convertible_v<Permit, bool>);
static_assert(noexcept(onlineResolverWorkInUse()));

void expect(bool condition, const char *message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL async_work_budget: %s\n", message);
        std::abort();
    }
}

void observationWithoutInitialization() {
    // This must run before any call that constructs the process pool.
    expect(mdkr_async_work_detail::resolverBudgetSlot().load() == nullptr,
           "process pool starts unpublished");
    expect(onlineResolverWorkInUse() == 0 &&
               observeResolverWorkFromOtherTranslationUnit() == 0,
           "both translation units can observe an unused pool without constructing it");
    expect(mdkr_async_work_detail::resolverBudgetSlot().load() == nullptr,
           "observation did not initialize or publish the process pool");

    // Exercise the singleton owner's publication/destruction ordering without
    // relying on process-exit callbacks or replacing the global allocator.
    std::atomic<AsyncWorkBudget *> slot{nullptr};
    Permit survivor;
    {
        mdkr_async_work_detail::ResolverBudgetOwner owner(slot);
        expect(slot.load() == &owner.budget(), "completed owner publishes its budget");
        survivor = owner.budget().tryAcquire();
        expect(slot.load()->inUse() == 1, "published observation sees owned work");
    }
    expect(slot.load() == nullptr, "owner retracts publication before destroying its budget");
    expect(bool(survivor), "permit survives retraction using independent shared state");
    survivor = {};
}

void capacityAndMoves() {
    AsyncWorkBudget disabled(0);
    expect(!disabled.tryAcquire() && disabled.inUse() == 0, "zero capacity rejects work");
    AsyncWorkBudget budget(2);
    expect(budget.limit() == 2 && budget.inUse() == 0, "new budget is empty");
    auto first = budget.tryAcquire();
    auto second = budget.tryAcquire();
    expect(first && second && budget.inUse() == 2, "capacity permits acquired");
    expect(!budget.tryAcquire() && budget.inUse() == 2, "refusal does not consume capacity");
    Permit moved(std::move(first));
    expect(moved && !first && budget.inUse() == 2, "move construction transfers one permit");
    moved = std::move(second);
    expect(moved && !second && budget.inUse() == 1, "move assignment releases replaced permit");
    Permit *alias = &moved;
    moved = std::move(*alias);
    expect(moved && budget.inUse() == 1, "self move retains ownership");
    first = {};
    second = {};
    expect(budget.inUse() == 1, "moved-from permits cannot release twice");
    moved = {};
    expect(budget.inUse() == 0, "empty assignment releases final permit");
    { auto released = budget.tryAcquire(); expect(bool(released), "reacquire after release"); }
    expect(budget.inUse() == 0, "scope exit releases permit");

    AsyncWorkBudget another(1);
    auto left = budget.tryAcquire();
    auto right = another.tryAcquire();
    left = std::move(right);
    expect(budget.inUse() == 0 && another.inUse() == 1 && !right,
           "cross-budget reassignment releases the old budget only");
    left = {};
    expect(another.inUse() == 0, "cross-budget replacement releases its new owner");
}

void failureAndOwnerLifetime() {
    AsyncWorkBudget budget(1);
    try {
        auto permit = budget.tryAcquire();
        expect(bool(permit), "failure path acquired permit");
        // Model a worker closure that owns its permit when scheduling refuses.
        const auto refusedWorker = [held = std::move(permit)] { return bool(held); };
        expect(!permit && refusedWorker(), "worker closure exclusively owns permit");
        throw std::runtime_error("scheduling refused");
    } catch (const std::runtime_error &) {}
    expect(budget.inUse() == 0, "failed scheduling unwinds closure ownership");

    Permit survivor;
    {
        AsyncWorkBudget shortLivedOwner(1);
        survivor = shortLivedOwner.tryAcquire();
        expect(shortLivedOwner.inUse() == 1, "owner has a live permit");
    }
    expect(bool(survivor), "permit retains state after budget object destruction");
    survivor = {}; // Must not touch the dead stack owner or process globals.
    expect(!survivor, "owner-independent release completes");
}

void concurrentBoundAndAbandonedWaiter() {
    constexpr unsigned workerCount = 24;
    constexpr unsigned capacity = 8;
    AsyncWorkBudget budget(capacity);
    std::mutex mutex;
    std::condition_variable changed;
    unsigned waiting = 0;
    unsigned attempted = 0;
    unsigned accepted = 0;
    bool start = false;
    bool finish = false;
    std::vector<std::thread> workers;
    for (unsigned i = 0; i < workerCount; ++i) {
        workers.emplace_back([&] {
            {
                std::unique_lock<std::mutex> lock(mutex);
                ++waiting;
                changed.notify_all();
                changed.wait(lock, [&] { return start; });
            }
            auto permit = budget.tryAcquire();
            {
                std::unique_lock<std::mutex> lock(mutex);
                ++attempted;
                if (permit) ++accepted;
                changed.notify_all();
                // Held work cannot free capacity just because its caller has
                // stopped waiting for it. Only leaving this worker scope does.
                changed.wait(lock, [&] { return finish; });
            }
        });
    }
    {
        std::unique_lock<std::mutex> lock(mutex);
        changed.wait(lock, [&] { return waiting == workerCount; });
        start = true;
        changed.notify_all();
        changed.wait(lock, [&] { return attempted == workerCount; });
        expect(accepted == capacity && budget.inUse() == capacity,
               "concurrent successful acquisitions exactly fill but never exceed capacity");
        expect(!budget.tryAcquire(), "outstanding workers retain budget after caller abandonment");
        finish = true;
        changed.notify_all();
    }
    for (auto &worker : workers) worker.join();
    expect(budget.inUse() == 0, "all completed worker permits released");
    expect(bool(budget.tryAcquire()), "work can resume after held workers finish");
}

void sharedAcrossTranslationUnits() {
    auto &budget = onlineResolverWorkBudget();
    expect(&budget == &resolverBudgetFromOtherTranslationUnit(),
           "inline external accessor has one cross-translation-unit instance");
    expect(budget.limit() == 8 && budget.inUse() == 0, "resolver process capacity is eight");
    std::vector<Permit> permits;
    for (unsigned i = 0; i < 8; ++i) {
        permits.push_back(i % 2 == 0 ? budget.tryAcquire()
                                    : acquireResolverPermitFromOtherTranslationUnit());
        expect(bool(permits.back()), "both translation units acquire from shared capacity");
        expect(onlineResolverWorkInUse() == i + 1 &&
                   observeResolverWorkFromOtherTranslationUnit() == i + 1,
               "nonallocating observations share the live cross-translation-unit count");
    }
    expect(!budget.tryAcquire() && !acquireResolverPermitFromOtherTranslationUnit(),
           "both translation units observe shared exhaustion");
    permits.clear();
    expect(budget.inUse() == 0, "cross-translation-unit permits release the same state");
    expect(onlineResolverWorkInUse() == 0 &&
               observeResolverWorkFromOtherTranslationUnit() == 0,
           "observations see final release");
}
}

int main() {
    observationWithoutInitialization();
    capacityAndMoves();
    failureAndOwnerLifetime();
    concurrentBoundAndAbandonedWaiter();
    sharedAcrossTranslationUnits();
    std::puts("PASS async_work_budget: capacity, moves, lifetime, contention, shared resolver pool");
    return 0;
}
