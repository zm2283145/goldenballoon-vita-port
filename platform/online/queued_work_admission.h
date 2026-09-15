#ifndef MDKR64_QUEUED_WORK_ADMISSION_H
#define MDKR64_QUEUED_WORK_ADMISSION_H

#include <cstddef>
#include <functional>
#include <utility>

// The pinned RTC pool compares only time. Extracting the payload through a
// const priority_queue::top must not copy std::function or change that key.
// Caller holds the scheduler lock and pops this record immediately afterward.
// The empty record releases no captured owner under the lock; the returned
// callable is executed and destroyed by runOne after dequeue releases it.
template <typename Time>
struct MdkrScheduledWork {
    Time time;
    mutable std::function<void()> func;

    bool operator>(const MdkrScheduledWork &other) const { return time > other.time; }
    bool operator<(const MdkrScheduledWork &other) const { return time < other.time; }

    std::function<void()> takeCallable() const noexcept {
        std::function<void()> result;
        result.swap(func); // Leaves an explicitly empty node, not an unspecified moved-from value.
        return result;
    }
};

// Caller retains its existing queue lock and full/stopped admission checks.
// Measure before moving; publish accounting only after successful insertion.
// The queue's emplace must preserve existing entries on allocation refusal.
// This does not change the element type's own throwing-move contract or claim
// that general RTC submission/continuation/teardown is allocation-free.
template <typename Queue, typename Element, typename Measure>
void mdkrCommitQueueInsertion(Queue &queue, std::size_t &amount,
                              Element &&element, Measure &&measure) {
    const std::size_t added = std::forward<Measure>(measure)(element);
    queue.emplace(std::forward<Element>(element));
    amount += added;
}

#endif // MDKR64_QUEUED_WORK_ADMISSION_H
