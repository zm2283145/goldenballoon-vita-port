#ifndef MDKR64_PREPARED_WORK_QUEUE_H
#define MDKR64_PREPARED_WORK_QUEUE_H

#include <cstddef>
#include <functional>
#include <memory>
#include <type_traits>
#include <utility>

// Callable construction is the only allocating part of prepared admission.
// A move-only concrete callable lives directly in its owned node; accepted
// dispatch never converts it to std::function or creates another task/future.
template <typename Time>
struct MdkrPreparedWorkNode {
    Time time{};
    std::unique_ptr<MdkrPreparedWorkNode> next;
    virtual ~MdkrPreparedWorkNode() = default;
    virtual void invoke() = 0;
};

template <typename Time, typename Function>
struct MdkrPreparedCallable final : MdkrPreparedWorkNode<Time> {
    template <typename F>
    explicit MdkrPreparedCallable(F &&value) : function(std::forward<F>(value)) {}
    void invoke() override { (void)function(); }
    Function function;
};

// Intrusive FIFO with owning links. Caller supplies synchronization and a
// monotonically ordered publication timestamp (not the preparation timestamp).
// pop() detaches the node's next link so destruction cannot retire successors.
template <typename Time>
class MdkrPreparedWorkQueue {
public:
    using Node = MdkrPreparedWorkNode<Time>;
    using Pointer = std::unique_ptr<Node>;

    MdkrPreparedWorkQueue() = default;
    MdkrPreparedWorkQueue(const MdkrPreparedWorkQueue &) = delete;
    MdkrPreparedWorkQueue &operator=(const MdkrPreparedWorkQueue &) = delete;
    ~MdkrPreparedWorkQueue() {
        // Avoid recursive unique_ptr-chain destruction. Production Processor
        // drains before destruction; ThreadPool must never cancel this lane.
        while (!empty()) { auto retired = pop(); }
    }

    template <typename F>
    static Pointer prepare(F &&function) {
        return std::make_unique<MdkrPreparedCallable<Time, std::decay_t<F>>>(
            std::forward<F>(function));
    }

    void push(Pointer work, Time time) noexcept {
        // work is a detached node, prepared before acquiring the caller lock.
        work->time = time;
        auto *node = work.get();
        if (tail_) tail_->next = std::move(work);
        else head_ = std::move(work);
        tail_ = node;
        ++size_;
    }

    Pointer pop() noexcept {
        if (!head_) return {};
        Pointer work = std::move(head_);
        head_ = std::move(work->next);
        if (!head_) tail_ = nullptr;
        --size_;
        return work;
    }

    bool empty() const noexcept { return !head_; }
    std::size_t size() const noexcept { return size_; }
    const Node &front() const noexcept { return *head_; }

private:
    Pointer head_;
    Node *tail_ = nullptr;
    std::size_t size_ = 0;
};

// Returned by the scheduler after releasing its mutex. Timed work keeps its
// existing callable/future behavior; immediate work retains its prepared node.
template <typename Time>
struct MdkrPreparedDispatch {
    typename MdkrPreparedWorkQueue<Time>::Pointer prepared;
    std::function<void()> timed;

    explicit operator bool() const noexcept { return bool(prepared) || bool(timed); }
    void operator()() {
        if (prepared) prepared->invoke();
        else if (timed) timed();
    }
};

// Unlike the vendor scope_guard, this stores the callable's concrete type;
// no std::function conversion/bind/allocation occurs when entering a guard.
template <typename Function>
class MdkrNoAllocScopeExit {
public:
    explicit MdkrNoAllocScopeExit(Function function) noexcept
        : function_(std::move(function)) {
        static_assert(std::is_nothrow_move_constructible_v<Function>);
        static_assert(std::is_nothrow_invocable_v<Function &>);
    }
    MdkrNoAllocScopeExit(const MdkrNoAllocScopeExit &) = delete;
    MdkrNoAllocScopeExit &operator=(const MdkrNoAllocScopeExit &) = delete;
    ~MdkrNoAllocScopeExit() noexcept { function_(); }
private:
    Function function_;
};

template <typename Function>
MdkrNoAllocScopeExit(Function) -> MdkrNoAllocScopeExit<Function>;

#endif
