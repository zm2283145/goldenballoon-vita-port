#include "startup_stage_ownership.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <system_error>

namespace {
void expect(bool condition, const char *message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL datachannel_startup_stages: %s\n", message);
        std::abort();
    }
}

struct Resource {
    explicit Resource(int &count) : live(count) { ++live; }
    ~Resource() { --live; }
    int &live;
};

struct PollFixture {
    int mapLive = 0, interrupterLive = 0;
    unsigned maps = 0, interrupters = 0, starts = 0;
    std::unique_ptr<Resource> map, interrupter;
    bool stopped = true;
    std::thread thread;
    std::atomic<bool> workerSawPublishedState{false};

    // Test fails loudly instead of hiding a lost joining obligation.
    ~PollFixture() { expect(!thread.joinable(), "fixture leaves no joinable worker"); }

    void start(int failStage = -1) {
        mdkrStartOwnedPolling(map, interrupter, stopped, thread,
            [&] {
                ++maps;
                if (failStage == 0) throw std::bad_alloc();
                if (failStage == 3) return std::unique_ptr<Resource>{};
                return std::make_unique<Resource>(mapLive);
            },
            [&] {
                ++interrupters;
                if (failStage == 1) throw std::bad_alloc();
                if (failStage == 4) return std::unique_ptr<Resource>{};
                return std::make_unique<Resource>(interrupterLive);
            },
            [&] {
                ++starts;
                if (failStage == 2) {
                    throw std::system_error(std::make_error_code(
                        std::errc::resource_unavailable_try_again));
                }
                if (failStage == 5) return std::thread{};
                return std::thread([this] {
                    workerSawPublishedState.store(map && interrupter && !stopped);
                });
            });
    }

    void stop() {
        // Corresponds to the successful service's ordinary joining owner.
        if (thread.joinable()) thread.join();
        stopped = true;
        interrupter.reset();
        map.reset();
    }
};

void pollingFailuresRestoreEmptyStoppedStateAndAllowRetry() {
    for (int stage = 0; stage <= 5; ++stage) {
        PollFixture fixture;
        bool refused = false;
        try { fixture.start(stage); }
        catch (...) { refused = true; }
        expect(refused, "injected polling startup refusal propagates");
        expect(fixture.stopped && !fixture.thread.joinable(), "refusal leaves stopped state and no worker");
        expect(!fixture.map && !fixture.interrupter, "refusal retracts both resources");
        expect(fixture.mapLive == 0 && fixture.interrupterLive == 0, "refusal releases staged resources");
        const unsigned expectedStarts = stage == 2 || stage == 5 ? 1u : 0u;
        expect(fixture.starts == expectedStarts, "allocation refusal never launches a worker");
        fixture.start();
        expect(fixture.thread.joinable() && !fixture.stopped, "retry creates a real owned worker");
        fixture.stop();
        expect(fixture.workerSawPublishedState.load(), "worker sees resources published before launch");
        expect(fixture.mapLive == 0 && fixture.interrupterLive == 0, "ordinary stop releases successful retry");
    }
}

void pollingRejectsExistingOwnershipWithoutReplacingIt() {
    PollFixture fixture;
    fixture.start();
    Resource *map = fixture.map.get();
    Resource *interrupter = fixture.interrupter.get();
    const unsigned starts = fixture.starts;
    bool refused = false;
    try { fixture.start(); }
    catch (const std::logic_error &) { refused = true; }
    expect(refused && fixture.starts == starts, "duplicate startup is rejected before factories or launch");
    expect(fixture.map.get() == map && fixture.interrupter.get() == interrupter,
           "duplicate startup preserves live resources");
    fixture.stop();
}

void registryAllocationPrecedesExternalStartup() {
    Resource *registry = nullptr;
    int live = 0;
    unsigned starts = 0;
    bool refused = false;
    try {
        mdkrStartOwnedRegistry(registry,
            []() -> std::unique_ptr<Resource> { throw std::bad_alloc(); },
            [&] { ++starts; });
    } catch (const std::bad_alloc &) { refused = true; }
    expect(refused && registry == nullptr && starts == 0,
           "registry allocation refusal never starts the external subsystem");

    mdkrStartOwnedRegistry(registry,
        [&] { return std::make_unique<Resource>(live); },
        [&] {
            expect(live == 1 && registry == nullptr, "registry is reserved before C startup and published afterward");
            ++starts;
        });
    expect(registry != nullptr && live == 1 && starts == 1, "successful external startup publishes sole registry ownership");
    Resource *original = registry;
    refused = false;
    try {
        mdkrStartOwnedRegistry(registry,
            [&] { return std::make_unique<Resource>(live); },
            [&] { ++starts; });
    } catch (const std::logic_error &) { refused = true; }
    expect(refused && registry == original && live == 1 && starts == 1,
           "duplicate registry startup preserves acquired state");

    // Settings are deliberately later than the helper's ownership commit,
    // matching SctpTransport::Init -> transaction mark -> SetSettings.
    bool settingsFailed = false;
    try { throw std::invalid_argument("settings refusal"); }
    catch (const std::invalid_argument &) { settingsFailed = true; }
    expect(settingsFailed && registry == original && live == 1,
           "settings failure leaves ownership available to transaction rollback");
    delete registry;
    registry = nullptr;
    expect(live == 0, "caller cleanup releases the published registry once");
}

void refusedExternalStartDoesNotLeakCppRegistry() {
    Resource *registry = nullptr;
    int live = 0;
    bool refused = false;
    try {
        mdkrStartOwnedRegistry(registry,
            [&] { return std::make_unique<Resource>(live); },
            [] { throw std::runtime_error("refused before external acquisition"); });
    } catch (const std::runtime_error &) { refused = true; }
    expect(refused && registry == nullptr && live == 0,
           "external refusal before acquisition releases reserved C++ registry");
    // This is NOT an oracle for partially started usrsctp, whose void C API
    // does not expose that failure contract and may terminate the process.
}
} // namespace

int main() {
    pollingFailuresRestoreEmptyStoppedStateAndAllowRetry();
    pollingRejectsExistingOwnershipWithoutReplacingIt();
    registryAllocationPrecedesExternalStartup();
    refusedExternalStartDoesNotLeakCppRegistry();
    return 0;
}
