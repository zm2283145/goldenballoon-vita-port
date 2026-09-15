#ifndef MDKR64_TRANSPORT_RETIREMENT_H
#define MDKR64_TRANSPORT_RETIREMENT_H

#include <memory>
#include <mutex>
#include <utility>

// Dispatcher must be a live, serial, unbounded prepared-work dispatcher. Its
// prepare() wraps a task with nonallocating continuation; enqueuePrepared() must
// accept that exact dispatcher's record without allocation. The vendor uses its
// immortal TearDownProcessor and an RTC pool with at least two workers: a retired
// object may join its own different Processor while retirement is serialized.
//
// This reserves dispatch, not allocation-free task bodies or destructors. The
// token is retained through destruction, never in a surviving weak controlblock.
template <typename Dispatcher, typename Token, typename Base>
class MdkrTransportRetirement {
    using Work = typename Dispatcher::PreparedWork;

    struct State {
        // First member is destroyed last, after unused records and ownership.
        Token token;
        Dispatcher *dispatcher;
        void *object = nullptr;
        void (*destroy)(void *) noexcept = nullptr;
        Work deletion;
        Work stopping;
        std::mutex mutex;
        bool starting = false;
        bool stopRequested = false;
        std::shared_ptr<Base> stopOwner;

        State(Dispatcher &target, Token keep)
            : token(std::move(keep)), dispatcher(&target) {}
    };

    // Standard shared_ptr(raw, deleter) initializes enable_shared_from_this.
    // A weak_ptr retains this pointer-only deleter after last-strong release;
    // the pointer is then inert, and holds neither a token nor a work record.
    struct Deleter {
        State *state;
        void operator()(void *) const noexcept {
            auto *dispatcher = state->dispatcher;
            auto work = std::move(state->deletion);
            dispatcher->enqueuePrepared(std::move(work));
            // Do not touch state: a worker may already have destroyed it.
        }
    };

public:
    // Construct is invoked only after BOTH retirement records are reserved.
    // It returns a completed T*. An accepted-socket factory can mark its caller's
    // descriptor transferred here, before shared_ptr controlblock allocation.
    // Incomplete-T cleanup remains the constructor's responsibility.
    template <typename T, typename Construct>
    static std::shared_ptr<T> makeWith(Dispatcher &dispatcher, Token token,
                                      Construct &&construct) {
        auto state = std::make_unique<State>(dispatcher, std::move(token));
        auto *rawState = state.get();
        state->destroy = [](void *object) noexcept { delete static_cast<T *>(object); };
        state->deletion = dispatcher.prepare([rawState]() {
            auto owned = std::unique_ptr<State>(rawState);
            auto *object = std::exchange(owned->object, nullptr);
            // Delete INSIDE the serialized body, not capture destruction after
            // its continuation has made the next retirement runnable.
            owned->destroy(object);
        });
        state->stopping = dispatcher.prepare([rawState]() {
            // Retain independently before calling user/library code. On throw,
            // local ownership also retires before the typed continuation runs.
            auto owner = std::move(rawState->stopOwner);
            owner->stop();
            // No state access after stop: dropping owner may queue finaldelete.
        });
        T *object = std::forward<Construct>(construct)();
        state->object = object;
        state.release();
        // On controlblock refusal, standard shared_ptr invokes Deleter exactly
        // once. It publishes the already-reserved deletion record off-thread.
        return std::shared_ptr<T>(object, Deleter{rawState});
    }

    template <typename T, typename... Args>
    static std::shared_ptr<T> make(Dispatcher &dispatcher, Token token, Args &&...args) {
        return makeWith<T>(dispatcher, std::move(token), [&]() {
            return new T(std::forward<Args>(args)...);
        });
    }

    static bool hasReservation(const std::shared_ptr<Base> &owner) noexcept {
        return !owner || std::get_deleter<Deleter>(owner) != nullptr;
    }

    // false means a foreign controlblock, not a silently dropped stop request.
    // Production factories all use make/makeWith; callers retain the legacy
    // synchronous behavior explicitly for foreign internal owners if needed.
    static bool requestStop(const std::shared_ptr<Base> &owner) noexcept {
        if (!owner) return true;
        auto *deleter = std::get_deleter<Deleter>(owner);
        if (!deleter) return false;
        auto *state = deleter->state;
        auto *dispatcher = state->dispatcher;
        Work work;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (state->stopRequested) return true;
            state->stopRequested = true;
            state->stopOwner = owner;
            if (!state->starting) work = std::move(state->stopping);
        }
        if (work) dispatcher->enqueuePrepared(std::move(work));
        // The strong caller owner and queued owner protect State until handoff.
        return true;
    }

    // Close can race publication immediately before start(). Do not consume the
    // one-shot stop before an already-admitted start completes, and do not admit
    // start after a stop request. Neither user code nor dispatch runs under this
    // record lock, so a synchronous start callback may request its own close.
    static bool beginStart(const std::shared_ptr<Base> &owner) noexcept {
        auto *deleter = std::get_deleter<Deleter>(owner);
        if (!deleter) return true; // foreign owners retain the legacy contract
        auto *state = deleter->state;
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->stopRequested || state->starting) return false;
        state->starting = true;
        return true;
    }

    // Every successful beginStart requires this on success AND exception exit.
    // A typed nonallocating scope guard must be installed before invoking start.
    static void finishStart(const std::shared_ptr<Base> &owner) noexcept {
        auto *deleter = std::get_deleter<Deleter>(owner);
        if (!deleter) return;
        auto *state = deleter->state;
        auto *dispatcher = state->dispatcher;
        Work work;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->starting = false;
            if (state->stopRequested) work = std::move(state->stopping);
        }
        if (work) dispatcher->enqueuePrepared(std::move(work));
    }
};

#endif
