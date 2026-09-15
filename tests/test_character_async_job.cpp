#include "character_async_job.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

void check(bool condition) {
    if (!condition) std::abort();
}

template <typename Result>
bool pollUntilReady(CharacterAsyncJob<Result> &job,
                    std::unique_ptr<Result> &result,
                    std::exception_ptr &error) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
        if (job.poll(result, error)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

void testResultRemainsBusyUntilPublished() {
    CharacterAsyncJob<std::string> job;
    std::promise<void> release;
    std::shared_future<void> gate(release.get_future());
    check(job.start([gate]() {
        gate.wait();
        return std::string("finished");
    }));
    check(job.busy());
    check(!job.start([] { return std::string("overlap"); }));

    release.set_value();
    std::unique_ptr<std::string> result;
    std::exception_ptr error;
    check(pollUntilReady(job, result, error));
    check(error == nullptr);
    check(result && *result == "finished");
    check(!job.busy());
    check(!job.poll(result, error));
}

void testWorkerExceptionCrossesThreadBoundary() {
    CharacterAsyncJob<int> job;
    check(job.start([]() -> int {
        throw std::runtime_error("fixture failure");
    }));

    std::unique_ptr<int> result;
    std::exception_ptr error;
    check(pollUntilReady(job, result, error));
    check(!result);
    check(error != nullptr);
    try {
        std::rethrow_exception(error);
        check(false);
    } catch (const std::runtime_error &failure) {
        check(std::string(failure.what()) == "fixture failure");
    }
    check(!job.busy());
}

void testDestructorWaitsForWorker() {
    std::atomic<bool> completed{false};
    {
        CharacterAsyncJob<int> job;
        check(job.start([&completed] {
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
            completed.store(true);
            return 1;
        }));
    }
    check(completed.load());
}

} // namespace

int main() {
    testResultRemainsBusyUntilPublished();
    testWorkerExceptionCrossesThreadBoundary();
    testDestructorWaitsForWorker();
    return 0;
}
