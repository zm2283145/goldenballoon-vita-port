#include "rtc_initialization_transaction.h"
#include "reserved_cleanup_worker.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>

namespace {
using Transaction = MdkrRtcInitializationTransaction;
using Stage = Transaction::Stage;
using Phase = Transaction::Phase;
using namespace std::chrono_literals;

void expect(bool value, const char *message) {
    if (!value) {
        std::fprintf(stderr, "FAIL rtc_initialization_transaction: %s\n", message);
        std::abort();
    }
}

struct Fixture {
    std::mutex mutex;
    Transaction transaction;
    std::shared_future<void> completion;
    std::shared_future<void> retirementGate;
    std::array<unsigned, 4> live{};
    std::array<unsigned, 4> released{};
    std::array<unsigned, 4> stopCalls{};
    int failStart = -1;
    int failStop = -1;

    Fixture() {
        std::promise<void> ready;
        ready.set_value();
        completion = ready.get_future().share();
    }

    void start() {
        const auto fail = [&](int point) { if (failStart == point) throw point; };
        fail(0); // Before a successful platform-socket acquisition.
        live[0] = 1;
        transaction.acquired(Stage::Sockets);
        fail(1); // ThreadPool singleton acquisition failed; only sockets are owned.
        transaction.acquired(Stage::Pool); // A partial spawn is owned too.
        fail(2); // First worker refused: an attempted pool can contain zero workers.
        live[1] = 1;
        fail(3); // A later worker refused: earlier workers must still retire.
        live[1] = 2;
        fail(4); // Poll start's strong failure contract leaves no resources.
        live[2] = 1;
        transaction.acquired(Stage::Poll);
        fail(5); // SCTP registry allocation precedes C startup.
        live[3] = 1;
        transaction.acquired(Stage::Sctp);
        fail(6); // Settings conversion after SCTP has started.
    }

    void stop() {
        if (retirementGate.valid()) retirementGate.wait();
        for (const Stage stage : {Stage::Pool, Stage::Poll, Stage::Sctp, Stage::Sockets}) {
            transaction.release(stage, [&] {
                const auto index = static_cast<std::size_t>(stage);
                ++stopCalls[index];
                if (failStop == static_cast<int>(index)) throw 77;
                released[index] += live[index];
                live[index] = 0;
            });
        }
    }
};

// Uses the actual production state/worker helpers in the patched token order.
// Resource counters stand in for subsystems; their real start helpers have a
// separate fixture and patch-order source bindings. This does not run RTC.
struct Epoch {
    explicit Epoch(Fixture &fixture)
        : cleanup([&fixture] {
            std::lock_guard<std::mutex> lock(fixture.mutex);
            fixture.transaction.retire([&] { fixture.stop(); });
        }) {
        fixture.completion = cleanup.future();
        cleanup.arm();
        fixture.transaction.initialize([&] { fixture.start(); });
    }
    MdkrReservedCleanupWorker cleanup;
};

void await(const std::shared_future<void> &future) {
    expect(future.wait_for(2s) == std::future_status::ready, "retirement becomes ready");
}

void everyPartialStartRetiresAndAllowsFreshInitialization() {
    for (int failure = 0; failure <= 6; ++failure) {
        Fixture fixture;
        std::promise<void> allowRetirement;
        fixture.retirementGate = allowRetirement.get_future().share();
        fixture.failStart = failure;
        bool originalPreserved = false;
        std::array<unsigned, 4> ownedBeforeRollback{};
        {
            std::lock_guard<std::mutex> lock(fixture.mutex);
            fixture.transaction.admit(fixture.completion);
            try { Epoch epoch(fixture); }
            catch (int point) { originalPreserved = point == failure; }
            expect(originalPreserved, "constructor preserves the original start exception");
            expect(fixture.transaction.phase() == Phase::RollbackPending, "failed start is not ready");
            for (const Stage stage : {Stage::Sockets, Stage::Pool, Stage::Poll, Stage::Sctp})
                ownedBeforeRollback[static_cast<std::size_t>(stage)] =
                    fixture.transaction.owns(stage) ? 1u : 0u;
            expect(fixture.completion.wait_for(0s) == std::future_status::timeout,
                   "failure publishes this pending retirement, not the previous ready future");
            bool refused = false;
            try { fixture.transaction.admit(fixture.completion); }
            catch (const std::runtime_error &) { refused = true; }
            expect(refused, "new admission is refused without waiting under the initialization mutex");
        }
        allowRetirement.set_value();
        await(fixture.completion);
        fixture.completion.get();
        expect(fixture.transaction.phase() == Phase::Dormant, "successful rollback restores dormant state");
        for (unsigned count : fixture.live) expect(count == 0, "no partial stage left live");
        expect(fixture.stopCalls == ownedBeforeRollback,
               "each owned stage retires exactly once, including a pool with zero started workers");
        fixture.retirementGate = {};
        fixture.failStart = -1;
        {
            std::lock_guard<std::mutex> lock(fixture.mutex);
            fixture.transaction.admit(fixture.completion);
            Epoch epoch(fixture);
            expect(fixture.transaction.phase() == Phase::Ready, "fresh initialization reruns all stages");
            expect(fixture.live == std::array<unsigned, 4>{1, 2, 1, 1}, "retry does not use poisoned state");
        }
        await(fixture.completion);
        fixture.completion.get();
        const auto released = fixture.released;
        fixture.transaction.retire([&] { fixture.stop(); });
        expect(fixture.released == released, "repeated retirement cannot double release");
    }
}

void failedRetirementRetainsOwnershipAndRefusesRetry() {
    for (const Stage failedStage : {Stage::Pool, Stage::Poll, Stage::Sctp, Stage::Sockets}) {
        Fixture fixture;
        fixture.failStop = static_cast<int>(failedStage);
        {
            std::lock_guard<std::mutex> lock(fixture.mutex);
            fixture.transaction.admit(fixture.completion);
            Epoch epoch(fixture);
        }
        await(fixture.completion);
        bool preserved = false;
        try { fixture.completion.get(); } catch (int error) { preserved = error == 77; }
        expect(preserved && fixture.transaction.phase() == Phase::Failed, "retirement failure is explicit");
        const std::array<unsigned, 4> initial{1, 2, 1, 1};
        auto retained = initial;
        for (const Stage stage : {Stage::Pool, Stage::Poll, Stage::Sctp, Stage::Sockets}) {
            if (stage == failedStage) break;
            retained[static_cast<std::size_t>(stage)] = 0;
        }
        expect(fixture.live == retained, "failure retains only unreleased supporting resources");
        for (const Stage stage : {Stage::Pool, Stage::Poll, Stage::Sctp, Stage::Sockets}) {
            const auto index = static_cast<std::size_t>(stage);
            expect(fixture.transaction.owns(stage) == (retained[index] != 0),
                   "released stages remain clear and unreleased ownership survives failure");
            expect(fixture.released[index] == initial[index] - retained[index],
                   "already completed retirement is counted exactly once");
        }
        preserved = false;
        try { fixture.transaction.admit(fixture.completion); } catch (int error) { preserved = error == 77; }
        expect(preserved, "failed future never authorizes retry");
    }
}

void pendingPublicationAndInvalidFutureCannotAdmit() {
    Transaction transaction;
    std::promise<void> publication;
    const auto pending = publication.get_future().share();
    bool refused = false;
    try { transaction.admit(pending); } catch (const std::runtime_error &) { refused = true; }
    expect(refused, "even dormant state waits for successful publication");
    refused = false;
    try { transaction.admit({}); } catch (const std::logic_error &) { refused = true; }
    expect(refused, "invalid previous future fails closed");
    publication.set_value();
    transaction.admit(pending);
    transaction.initialize([] {});
    refused = false;
    try { transaction.admit(pending); } catch (const std::logic_error &) { refused = true; }
    expect(refused, "a ready future cannot hide an unretired epoch");
}

void deferredCompletionCannotRunOnTheAdmissionThread() {
    Transaction transaction;
    bool invoked = false;
    const auto deferred = std::async(std::launch::deferred, [&] { invoked = true; }).share();
    bool refused = false;
    try { transaction.admit(deferred); }
    catch (const std::runtime_error &) { refused = true; }
    expect(refused && !invoked,
           "admission must not execute deferred cleanup while holding the initialization mutex");
    expect(transaction.phase() == Phase::Dormant, "refused admission leaves state untouched");
}

void incompleteRetirementCannotPublishReusableState() {
    Transaction transaction;
    transaction.initialize([&] { transaction.acquired(Stage::Sctp); });
    bool refused = false;
    try { transaction.initialize([] {}); }
    catch (const std::logic_error &) { refused = true; }
    expect(refused && transaction.phase() == Phase::Ready && transaction.owns(Stage::Sctp),
           "duplicate initialization preserves the existing ready owner");
    refused = false;
    try { transaction.retire([] {}); }
    catch (const std::logic_error &) { refused = true; }
    expect(refused && transaction.phase() == Phase::Failed && transaction.owns(Stage::Sctp),
           "a retirement callback that omits an owned stage cannot publish dormant state");
    bool stopCalled = false;
    refused = false;
    try { transaction.retire([&] { stopCalled = true; }); }
    catch (const std::logic_error &) { refused = true; }
    expect(refused && !stopCalled && transaction.owns(Stage::Sctp),
           "failed retirement cannot retry cleanup implicitly or lose remaining ownership");
}
}

int main() {
    everyPartialStartRetiresAndAllowsFreshInitialization();
    failedRetirementRetainsOwnershipAndRefusesRetry();
    pendingPublicationAndInvalidFutureCannotAdmit();
    deferredCompletionCannotRunOnTheAdmissionThread();
    incompleteRetirementCannotPublishReusableState();
    return 0;
}
