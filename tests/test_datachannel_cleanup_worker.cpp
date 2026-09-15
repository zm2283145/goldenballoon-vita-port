#include "reserved_cleanup_worker.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <mutex>
#include <system_error>

namespace {
using namespace std::chrono_literals;

void expect(bool condition, const char *message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL datachannel_cleanup_worker: %s\n", message);
        std::abort();
    }
}

struct RefuseStart {
    template <typename Work>
    std::thread operator()(Work &&) const {
        throw std::system_error(std::make_error_code(
            std::errc::resource_unavailable_try_again));
    }
};

struct RefuseDetach {
    void operator()(std::thread &) const {
        throw std::system_error(std::make_error_code(
            std::errc::resource_unavailable_try_again));
    }
};

struct RefuseSecondStart {
    unsigned *attempts;
    template <typename Work>
    std::thread operator()(Work &&work) const {
        if (++*attempts == 2) {
            throw std::system_error(std::make_error_code(
                std::errc::resource_unavailable_try_again));
        }
        return std::thread(std::forward<Work>(work));
    }
};

// Mirrors the patched TokenPayload's actual helper/initialization/arm ordering.
// The helper's real destructor, not an outer cleanup callback mock, runs when
// this enclosing constructor throws or its final owner dies.
struct Token {
    template <typename Start = MdkrReservedCleanupWorker::StartThread,
              typename Detach = MdkrReservedCleanupWorker::DetachThread>
    Token(std::atomic<unsigned> &initialized, std::atomic<unsigned> &cleaned,
          bool refuseInit, Start start = {}, Detach detach = {})
        : cleanup([&cleaned] { ++cleaned; }, start, detach) {
        ++initialized;
        if (refuseInit) throw std::runtime_error("initialization refused");
        result = cleanup.future();
        cleanup.arm();
    }
    MdkrReservedCleanupWorker cleanup;
    std::shared_future<void> result;
};

void launchRefusalPrecedesInitialization() {
    std::atomic<unsigned> initialized{0}, cleaned{0};
    bool caught = false;
    try { Token token(initialized, cleaned, false, RefuseStart{}); }
    catch (const std::system_error &) { caught = true; }
    expect(caught, "launch refusal is catchable before a token is admitted");
    expect(initialized == 0 && cleaned == 0, "launch refusal neither initializes nor cleans RTC");
}

void detachRefusalCancelsAndJoinsOwnedWorker() {
    std::atomic<unsigned> initialized{0}, cleaned{0};
    bool caught = false;
    try {
        Token token(initialized, cleaned, false,
                    MdkrReservedCleanupWorker::StartThread{}, RefuseDetach{});
    } catch (const std::system_error &) { caught = true; }
    expect(caught, "detach refusal remains catchable with no lost joinable handle");
    expect(initialized == 0 && cleaned == 0, "detach refusal cancels before initialization");
}

void publisherLaunchRefusalJoinsCleanupWorker() {
    std::atomic<unsigned> initialized{0}, cleaned{0};
    unsigned attempts = 0;
    bool caught = false;
    try { Token token(initialized, cleaned, false, RefuseSecondStart{&attempts}); }
    catch (const std::system_error &) { caught = true; }
    expect(caught && attempts == 2, "publisher launch refusal is catchable");
    expect(initialized == 0 && cleaned == 0, "publisher launch refusal cancels and joins without cleanup");
}

void initializationRefusalCancelsReservedWorker() {
    std::atomic<unsigned> initialized{0}, cleaned{0};
    std::atomic<bool> workerReturned{false};
    unsigned attempts = 0;
    const auto start = [&workerReturned, &attempts](auto work) {
        const bool cleanupThread = ++attempts == 1;
        return std::thread([&workerReturned, cleanupThread, work = std::move(work)] {
            work();
            if (cleanupThread) workerReturned.store(true);
        });
    };
    bool caught = false;
    try { Token token(initialized, cleaned, true, start); }
    catch (const std::runtime_error &) { caught = true; }
    expect(caught && initialized == 1, "initialization failure propagates");
    expect(cleaned == 0, "unarmed destruction never runs cleanup under initialization lock");
    expect(workerReturned.load(), "initialization cancellation waits for cleanup worker join");
}

void lastOwnerOnlySignalsWhileInitMutexHeld() {
    std::mutex initMutex;
    std::atomic<unsigned> cleaned{0}, starts{0};
    std::shared_future<void> result;
    {
        std::lock_guard<std::mutex> lock(initMutex);
        MdkrReservedCleanupWorker worker([&] {
            std::lock_guard<std::mutex> cleanupLock(initMutex);
            ++cleaned;
        }, [&starts](auto work) {
            ++starts;
            return std::thread(std::move(work));
        });
        result = worker.future();
        worker.arm();
        expect(starts == 2 && cleaned == 0, "both workers reserve before resource cleanup");
        // Destruction precedes lock release, exactly like mGlobal.reset() in
        // Init::cleanup(). It must signal, not join or run cleanup inline.
    }
    expect(result.wait_for(2s) == std::future_status::ready, "cleanup completes after initialization lock release");
    result.get();
    expect(starts == 2 && cleaned == 1, "final destruction starts no new thread and cleans once");
}

void cleanupMayJoinTheFinalOwnersThread() {
    std::thread lastOwner;
    std::promise<void> mayRelease;
    auto releaseReady = mayRelease.get_future().share();
    auto worker = std::make_unique<MdkrReservedCleanupWorker>([&lastOwner] {
        lastOwner.join();
    });
    const auto result = worker->future();
    worker->arm();
    lastOwner = std::thread([worker = std::move(worker), releaseReady]() mutable {
        releaseReady.wait();
        worker.reset();
    });
    // Do not allow cleanup to join until assignment of lastOwner has finished.
    mayRelease.set_value();
    expect(result.wait_for(2s) == std::future_status::ready, "cleanup can join the final owner's worker");
    result.get();
    expect(!lastOwner.joinable(), "reserved cleanup worker joined the final owner without self-join");
}

struct ExitProbe {
    std::atomic<bool> *destroyed;
    ~ExitProbe() { destroyed->store(true); }
};

void completionIncludesThreadLocalDestructionAndExceptions() {
    for (const bool fail : {false, true}) {
        std::atomic<bool> threadLocalDestroyed{false};
        std::shared_future<void> result;
        {
            MdkrReservedCleanupWorker worker([&] {
                thread_local std::unique_ptr<ExitProbe> probe;
                probe = std::make_unique<ExitProbe>();
                probe->destroyed = &threadLocalDestroyed;
                if (fail) throw std::runtime_error("cleanup failed");
            });
            result = worker.future();
            worker.arm();
        }
        expect(result.wait_for(2s) == std::future_status::ready, "cleanup result becomes ready after cleanup worker join");
        expect(threadLocalDestroyed.load(), "completion follows thread-local destruction");
        bool threw = false;
        try { result.get(); }
        catch (const std::runtime_error &) { threw = true; }
        expect(threw == fail, "cleanup exception is preserved, not process termination");
        expect(result.wait_for(0s) == std::future_status::ready, "shared completion remains terminal");
    }
}
} // namespace

int main() {
    launchRefusalPrecedesInitialization();
    publisherLaunchRefusalJoinsCleanupWorker();
    detachRefusalCancelsAndJoinsOwnedWorker();
    initializationRefusalCancelsReservedWorker();
    lastOwnerOnlySignalsWhileInitMutexHeld();
    cleanupMayJoinTheFinalOwnersThread();
    completionIncludesThreadLocalDestructionAndExceptions();
    return 0;
}
