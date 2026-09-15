// Exercise the actual patched Transport registration/retirement implementation.
// RTC global services start, but no peer, socket connection, app or ROM is used.
// This is distinct from the synthetic-resource allocation-refusal fixture.
#include "impl/transport.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace {
using rtc::impl::Init;
using rtc::impl::TearDownProcessor;
using rtc::impl::Transport;
using rtc::impl::TransportRetirement;
using rtc::impl::makeRetiredTransport;

void require(bool value, const char *message) {
    if (!value) {
        std::fprintf(stderr, "RTC transport edges: %s\n", message);
        std::abort();
    }
}

struct Counts {
    std::atomic<unsigned> stops{0}, received{0}, destroyed{0};
};

struct ProbeTransport : Transport {
    Counts &counts;
    explicit ProbeTransport(Counts &value, std::shared_ptr<Transport> lower = nullptr)
        : Transport(std::move(lower)), counts(value) {}
    ~ProbeTransport() override { ++counts.destroyed; }
    void stop() override {
        ++counts.stops;
        Transport::stop();
    }
    void deliver() { recv(std::make_shared<rtc::Message>(0)); }
    void incoming(rtc::message_ptr message) override {
        ++counts.received;
        Transport::incoming(std::move(message));
    }
};

struct ConstructorFailure final : Transport {
    explicit ConstructorFailure(std::shared_ptr<Transport> lower)
        : Transport(std::move(lower)) { throw std::runtime_error("construction refused"); }
};

struct OffThreadTransport final : ProbeTransport {
    const std::thread::id caller = std::this_thread::get_id();
    using ProbeTransport::ProbeTransport;
    ~OffThreadTransport() override {
        require(std::this_thread::get_id() != caller, "foreign final deletion remains off caller");
    }
    void stop() override {
        require(std::this_thread::get_id() != caller, "foreign stop remains off caller");
        ProbeTransport::stop();
    }
};

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
        require(condition.wait_for(lock, std::chrono::seconds(5), [&] { return entered; }),
                "held callback/retirement body is entered");
    }
    void release() {
        std::lock_guard<std::mutex> lock(mutex);
        released = true;
        condition.notify_all();
    }
};

void unstartedLoserDoesNotRetireWinner() {
    Counts lowerCounts, winnerCounts, loserCounts;
    auto lower = std::make_shared<ProbeTransport>(lowerCounts);
    auto winner = std::make_shared<ProbeTransport>(winnerCounts, lower);
    winner->start();
    {
        auto loser = makeRetiredTransport<ProbeTransport>(loserCounts, lower);
        require(TransportRetirement::requestStop(loser), "loser's stop was reserved");
    }
    TearDownProcessor::Instance().join();
    lower->deliver();
    require(loserCounts.stops == 1u && loserCounts.destroyed == 1u,
            "unstarted loser is stopped and retired exactly once");
    require(lowerCounts.stops == 0u && winnerCounts.received == 1u,
            "loser cannot clear winner callback or stop shared lower");
}

void occupiedEdgeRefusalPreservesWinner() {
    Counts lowerCounts, winnerCounts, loserCounts;
    auto lower = std::make_shared<ProbeTransport>(lowerCounts);
    auto winner = std::make_shared<ProbeTransport>(winnerCounts, lower);
    winner->start();
    bool rejected = false;
    {
        auto loser = std::make_shared<ProbeTransport>(loserCounts, lower);
        try { loser->start(); }
        catch (const std::logic_error &) { rejected = true; }
        loser->stop();
    }
    lower->deliver();
    require(rejected && lowerCounts.stops == 0u && winnerCounts.received == 1u,
            "occupied-edge rejection and unwind preserve the admitted owner");
}

void constructorUnwindHasNoChainAuthority() {
    Counts lowerCounts, winnerCounts;
    auto lower = std::make_shared<ProbeTransport>(lowerCounts);
    auto winner = std::make_shared<ProbeTransport>(winnerCounts, lower);
    winner->start();
    bool rejected = false;
    try { (void)makeRetiredTransport<ConstructorFailure>(lower); }
    catch (const std::runtime_error &) { rejected = true; }
    lower->deliver();
    require(rejected && lowerCounts.stops == 0u && winnerCounts.received == 1u,
            "incomplete upper construction cannot clear or stop another chain");
}

void sameOwnerReregistrationAndReplacement() {
    Counts lowerCounts, oldCounts, nextCounts;
    auto lower = std::make_shared<ProbeTransport>(lowerCounts);
    auto old = std::make_shared<ProbeTransport>(oldCounts, lower);
    old->start();
    old->unregisterIncoming();
    old->registerIncoming();
    lower->deliver();
    require(oldCounts.received == 1u, "same owner may re-register an unsealed edge");
    old->unregisterIncoming();
    auto next = std::make_shared<ProbeTransport>(nextCounts, lower);
    next->start();
    old->unregisterIncoming(); // Repeated stale cleanup is also inert.
    old.reset();
    lower->deliver();
    require(lowerCounts.stops == 0u && nextCounts.received == 1u,
            "replacement registration revokes the previous chain-stop authority");
}

void retirementSealsBeforeQueuedStop() {
    Counts lowerCounts, oldCounts, lateCounts;
    auto lower = makeRetiredTransport<ProbeTransport>(lowerCounts);
    auto old = std::make_shared<ProbeTransport>(oldCounts, lower);
    old->start();
    Gate gate;
    auto &retirement = TearDownProcessor::Instance();
    retirement.enqueue([&] { gate.hold(); });
    gate.awaitEntry();
    old.reset(); // Foreign upper destroys synchronously; lower stop is reserved.
    require(lowerCounts.stops == 0u, "lower stop has not run behind held retirement");
    bool rejected = false;
    {
        auto late = std::make_shared<ProbeTransport>(lateCounts, lower);
        try { late->start(); }
        catch (const std::logic_error &) { rejected = true; }
        lower->onRecv(nullptr); // An explicit clear must not reopen a sealed edge.
        bool stillRejected = false;
        try { late->registerIncoming(); }
        catch (const std::logic_error &) { stillRejected = true; }
        require(rejected && stillRejected, "retirement seal precedes deferred stop and is permanent");
    }
    gate.release();
    retirement.join();
    require(lowerCounts.stops == 1u, "sealed lower is stopped exactly once");
    lower.reset();
    retirement.join();
    require(lowerCounts.destroyed == 1u, "reserved lower deletion completes");
}

void explicitExternalReplacementSurvivesStaleUpper() {
    Counts lowerCounts, upperCounts;
    auto lower = std::make_shared<ProbeTransport>(lowerCounts);
    auto upper = std::make_shared<ProbeTransport>(upperCounts, lower);
    upper->start();
    unsigned externalCalls = 0;
    lower->onRecv([&](rtc::message_ptr) { ++externalCalls; });
    upper.reset();
    lower->deliver();
    require(externalCalls == 1u && lowerCounts.stops == 0u,
            "external callback replacement revokes stale upper ownership");
    lower->onRecv(nullptr);
}

void reservedCascadeRetiresEveryStageOnce() {
    Counts lowerCounts, middleCounts, upperCounts;
    auto lower = makeRetiredTransport<ProbeTransport>(lowerCounts);
    auto middle = makeRetiredTransport<ProbeTransport>(middleCounts, lower);
    auto upper = makeRetiredTransport<ProbeTransport>(upperCounts, middle);
    middle->start();
    upper->start();
    lower->deliver();
    require(middleCounts.received == 1u && upperCounts.received == 1u,
            "actual transport delivery traverses the admitted chain");
    require(TransportRetirement::hasReservation(upper), "top-stage stop is reserved");
    rtc::impl::retireTransportChain(std::array<std::shared_ptr<Transport>, 3>{
        std::move(upper), std::move(middle), std::move(lower)});
    TearDownProcessor::Instance().join();
    require(upperCounts.stops == 1u && middleCounts.stops == 1u && lowerCounts.stops == 1u,
            "top retirement cascades one stop per owned lower edge");
    require(upperCounts.destroyed == 1u && middleCounts.destroyed == 1u && lowerCounts.destroyed == 1u,
            "all actual transport stages complete reserved deletion");
}

void foreignChainRetainsAsynchronousRetirement() {
    Counts lowerCounts, upperCounts;
    auto lower = std::make_shared<OffThreadTransport>(lowerCounts);
    auto upper = std::make_shared<OffThreadTransport>(upperCounts, lower);
    upper->start();
    require(!TransportRetirement::hasReservation(upper), "foreign controlblock is identified");
    rtc::impl::retireTransportChain(std::array<std::shared_ptr<Transport>, 2>{
        std::move(upper), std::move(lower)});
    TearDownProcessor::Instance().join();
    require(upperCounts.stops == 1u && lowerCounts.stops == 1u &&
            upperCounts.destroyed == 1u && lowerCounts.destroyed == 1u,
            "legacy grouped fallback retires every foreign stage exactly once");
}

void callbackCanRemoveItsOwnEdge() {
    Counts lowerCounts, upperCounts;
    auto lower = std::make_shared<ProbeTransport>(lowerCounts);
    auto upper = std::make_shared<ProbeTransport>(upperCounts, lower);
    upper->start();
    bool returned = false;
    upper->onRecv([&](rtc::message_ptr) {
        upper->unregisterIncoming();
        returned = true;
    });
    lower->deliver();
    require(returned && upperCounts.received == 1u, "callback may reenter its own edge removal");
    upper->onRecv(nullptr);
}

void retiredCallbackCaptureCanReenterReplacement() {
    Counts lowerCounts;
    auto lower = std::make_shared<ProbeTransport>(lowerCounts);
    struct Capture {
        std::weak_ptr<ProbeTransport> transport;
        unsigned &destroyed;
        Capture(std::weak_ptr<ProbeTransport> value, unsigned &count)
            : transport(std::move(value)), destroyed(count) {}
        ~Capture() {
            if (auto owner = transport.lock()) owner->onRecv(nullptr);
            ++destroyed;
        }
    };
    unsigned destroyed = 0;
    auto capture = std::make_shared<Capture>(lower, destroyed);
    lower->onRecv([capture](rtc::message_ptr) { (void)capture; });
    capture.reset();
    lower->onRecv(nullptr);
    require(destroyed == 1u,
            "retired callback capture is destroyed outside the pending/edge mutex");
}

void concurrentRemovalAndCallbackReentryComplete() {
    // Bounded interleaving stress, not a deterministic old-code deadlock control:
    // the public API does not expose the exact removal lock-acquisition point.
    for (unsigned round = 0; round < 16u; ++round) {
        Counts lowerCounts, upperCounts;
        auto lower = std::make_shared<ProbeTransport>(lowerCounts);
        auto upper = std::make_shared<ProbeTransport>(upperCounts, lower);
        upper->start();
        Gate callback;
        std::atomic<bool> removalStarted{false};
        std::atomic<bool> reentered{false};
        upper->onRecv([&](rtc::message_ptr) {
            callback.hold();
            upper->unregisterIncoming();
            reentered = true;
        });
        std::thread receiving([&] { lower->deliver(); });
        callback.awaitEntry();
        std::thread removing([&] {
            removalStarted = true;
            upper->unregisterIncoming();
        });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!removalStarted && std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
        require(removalStarted, "concurrent removal thread starts");
        callback.release();
        receiving.join();
        removing.join();
        require(reentered, "concurrent removal does not prevent callback reentry");
        upper->onRecv(nullptr);
    }
}
} // namespace

int main() {
    Init::Instance().setThreadPoolSize(2);
    auto epoch = Init::Instance().token();
    unstartedLoserDoesNotRetireWinner();
    occupiedEdgeRefusalPreservesWinner();
    constructorUnwindHasNoChainAuthority();
    sameOwnerReregistrationAndReplacement();
    retirementSealsBeforeQueuedStop();
    explicitExternalReplacementSurvivesStaleUpper();
    reservedCascadeRetiresEveryStageOnce();
    foreignChainRetainsAsynchronousRetirement();
    callbackCanRemoveItsOwnEdge();
    retiredCallbackCaptureCanReenterReplacement();
    concurrentRemovalAndCallbackReentryComplete();
    TearDownProcessor::Instance().join();
    epoch.reset();
    auto cleanup = Init::Instance().cleanup();
    require(cleanup.wait_for(std::chrono::seconds(10)) == std::future_status::ready,
            "global services retire after all actual transport owners");
    cleanup.get();
    std::puts("PASS RTC transport edges: 11 groups");
}
