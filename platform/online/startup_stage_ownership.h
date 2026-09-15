#ifndef MDKR64_STARTUP_STAGE_OWNERSHIP_H
#define MDKR64_STARTUP_STAGE_OWNERSHIP_H

#include <memory>
#include <new>
#include <stdexcept>
#include <thread>
#include <utility>

// Called under the subsystem's existing exclusive startup/admission boundary.
// Factories return sole ownership or throw. The starter has std::thread's
// contract: it returns a joinable worker, or throws without starting work.
// Resource publication MUST precede launch because the worker immediately
// reads these members. Failed construction then has no worker to race rollback.
template <typename Map, typename Interrupter, typename MakeMap,
          typename MakeInterrupter, typename Start>
void mdkrStartOwnedPolling(std::unique_ptr<Map> &map,
                          std::unique_ptr<Interrupter> &interrupter,
                          bool &stopped, std::thread &thread,
                          MakeMap makeMap, MakeInterrupter makeInterrupter,
                          Start start) {
    if (!stopped || thread.joinable() || map || interrupter) {
        throw std::logic_error("Polling startup requires an empty stopped owner");
    }
    auto stagedMap = makeMap();
    if (!stagedMap) throw std::bad_alloc();
    auto stagedInterrupter = makeInterrupter();
    if (!stagedInterrupter) throw std::bad_alloc();

    map = std::move(stagedMap);
    interrupter = std::move(stagedInterrupter);
    stopped = false;
    try {
        thread = start();
        if (!thread.joinable()) {
            throw std::logic_error("Polling startup returned no worker");
        }
    } catch (...) {
        stopped = true;
        interrupter.reset();
        map.reset();
        throw;
    }
}

// Allocate the C++ registry before starting its external C subsystem. The
// caller owns any external effects of start(), whose C API must return before
// ownership can be committed here. This does NOT turn void C initialization,
// internal process exits, or arbitrary partial C startup into a rollback API.
// After success the published registry stays owned through later settings
// application; its caller must record the acquired subsystem before settings
// can throw and release it via that subsystem's ordinary cleanup.
template <typename Registry, typename MakeRegistry, typename Start>
void mdkrStartOwnedRegistry(Registry *&registry, MakeRegistry makeRegistry,
                           Start start) {
    if (registry != nullptr) {
        throw std::logic_error("Registry startup requires an empty owner");
    }
    auto staged = makeRegistry();
    if (!staged) throw std::bad_alloc();
    start();
    registry = staged.release();
}

#endif // MDKR64_STARTUP_STAGE_OWNERSHIP_H
