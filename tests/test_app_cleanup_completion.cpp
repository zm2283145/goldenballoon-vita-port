#include "app_cleanup_completion.h"

#include <cstdio>
#include <cstdlib>
#include <future>
#include <stdexcept>

namespace {
void expect(bool condition, const char *message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL app_cleanup_completion: %s\n", message);
        std::abort();
    }
}

void completedFutureAndOwnerGate() {
    AppCleanupCompletion completion;
    std::promise<void> result;
    result.set_value();
    const std::shared_future<void> future = result.get_future().share();
    unsigned starts = 0;
    const auto start = [&] { ++starts; return future; };
    expect(!completion.failed() && !completion.error(), "initial state has no error");
    expect(!completion.poll(false, start), "owners gate a ready future");
    expect(!completion.poll(false, start), "repeated owner gate remains pending");
    expect(starts == 0, "cleanup not started while owners remain");
    expect(completion.poll(true, start), "ready future completes");
    expect(starts == 1 && !completion.failed(), "ready cleanup succeeds once");
    expect(completion.poll(true, start) && completion.poll(false, start),
           "completed observation remains terminal");
    expect(starts == 1, "completed cleanup never starts again");
}

void pendingFutureStartsOnce() {
    AppCleanupCompletion completion;
    std::promise<void> result;
    const std::shared_future<void> future = result.get_future().share();
    unsigned starts = 0;
    const auto start = [&] { ++starts; return future; };
    for (unsigned i = 0; i < 100; ++i) {
        expect(!completion.poll(true, start), "pending future stays pending without waiting");
    }
    expect(starts == 1, "pending polls start exactly once");
    expect(!completion.failed() && !completion.error(), "pending is not a failure");
    result.set_value();
    expect(completion.poll(true, start), "settled future completes next poll");
    expect(!completion.failed() && starts == 1, "settled future succeeds without restarting");
}

void exceptionalFutureIsTerminalFailure() {
    AppCleanupCompletion completion;
    std::promise<void> result;
    const auto future = result.get_future().share();
    unsigned starts = 0;
    const auto start = [&] { ++starts; return future; };
    expect(!completion.poll(true, start), "exceptional arm begins pending");
    result.set_exception(std::make_exception_ptr(std::runtime_error("cleanup failed")));
    expect(completion.poll(true, start) && completion.failed(),
           "exceptional future completes as failure, not success");
    bool preserved = false;
    try { std::rethrow_exception(completion.error()); }
    catch (const std::runtime_error &) { preserved = true; }
    expect(preserved, "future exception is retained");
    const auto error = completion.error();
    expect(completion.poll(true, start) && starts == 1 && completion.error() == error,
           "failed completion is idempotent and retains its error");
}

void throwingStartIsNotRetried() {
    AppCleanupCompletion completion;
    unsigned starts = 0;
    const auto start = [&]() -> std::shared_future<void> { ++starts; throw 42; };
    expect(!completion.poll(false, start), "throwing start is still owner-gated");
    expect(completion.poll(true, start) && completion.failed(), "start exception recorded");
    bool preserved = false;
    try { std::rethrow_exception(completion.error()); }
    catch (int value) { preserved = value == 42; }
    expect(preserved, "non-standard start exception retained");
    expect(completion.poll(true, start) && starts == 1, "throwing start never retried");
}

void invalidFutureFails() {
    AppCleanupCompletion completion;
    unsigned starts = 0;
    const auto start = [&] { ++starts; return std::shared_future<void>{}; };
    expect(completion.poll(true, start) && completion.failed(), "invalid future fails explicitly");
    bool classified = false;
    try { std::rethrow_exception(completion.error()); }
    catch (const std::logic_error &) { classified = true; }
    expect(classified, "invalid future has a contract error");
    expect(completion.poll(true, start) && starts == 1, "invalid future never restarted");
}

void deferredFutureNeverExecutes() {
    AppCleanupCompletion completion;
    unsigned starts = 0;
    unsigned executions = 0;
    const auto future = std::async(std::launch::deferred, [&] { ++executions; }).share();
    const auto start = [&] { ++starts; return future; };
    expect(completion.poll(true, start) && completion.failed(), "deferred future fails explicitly");
    expect(executions == 0, "deferred work never runs on polling thread");
    bool classified = false;
    try { std::rethrow_exception(completion.error()); }
    catch (const std::logic_error &) { classified = true; }
    expect(classified, "deferred future has a contract error");
    expect(completion.poll(true, start) && starts == 1 && executions == 0,
           "deferred failure stays terminal without execution");
}
}

int main() {
    completedFutureAndOwnerGate();
    pendingFutureStartsOnce();
    exceptionalFutureIsTerminalFailure();
    throwingStartIsNotRetried();
    invalidFutureFails();
    deferredFutureNeverExecutes();
    std::puts("PASS app_cleanup_completion: owner gate, once-only start, completion and failures");
    return 0;
}
