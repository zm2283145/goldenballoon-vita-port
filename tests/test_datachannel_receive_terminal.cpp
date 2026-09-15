// Actual selected MbedTLS DTLS/TLS lifecycle code. Global RTC services and
// cryptographic contexts are initialized, but no peer/socket connection is
// started. Forced transport states isolate terminal ownership; they do not
// stand in for a real handshake, encrypted-data drain or network acceptance.
#include "impl/dtlstransport.hpp"
#include "impl/icetransport.hpp"
#include "impl/tcptransport.hpp"
#include "impl/tlstransport.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <new>
#include <thread>

namespace {
std::atomic<bool> refuseAllocation{false};
std::atomic<unsigned> refusedAllocations{0};
thread_local bool refuseNextAllocation = false;

void require(bool value, const char *message) {
    if (!value) {
        std::fprintf(stderr, "RTC receive terminal: %s\n", message);
        std::abort();
    }
}
}

void *operator new(std::size_t size) {
    if (refuseAllocation.load() || refuseNextAllocation) {
        refuseNextAllocation = false;
        ++refusedAllocations;
        throw std::bad_alloc();
    }
    if (void *value = std::malloc(size ? size : 1u)) return value;
    throw std::bad_alloc();
}
void *operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void *value) noexcept { std::free(value); }
void operator delete[](void *value) noexcept { std::free(value); }
void operator delete(void *value, std::size_t) noexcept { std::free(value); }
void operator delete[](void *value, std::size_t) noexcept { std::free(value); }

namespace {
using rtc::impl::Certificate;
using rtc::impl::DtlsTransport;
using rtc::impl::IceTransport;
using rtc::impl::Init;
using rtc::impl::TcpTransport;
using rtc::impl::ThreadPool;
using rtc::impl::TlsTransport;
using rtc::impl::Transport;
using rtc::impl::TearDownProcessor;
using rtc::impl::makeRetiredTransport;
using State = Transport::State;

struct Trace {
    std::atomic<unsigned> failed{0}, disconnected{0}, eof{0};
    std::atomic<bool> destructionStarted{false};
    bool throwNotification = false;
};

template <typename Predicate>
void await(Predicate predicate, const char *message) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!predicate() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    require(predicate(), message);
}

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
                "RTC worker reaches held body");
    }
    void release() {
        std::lock_guard<std::mutex> lock(mutex);
        released = true;
        condition.notify_all();
    }
};

struct DtlsProbe final : DtlsTransport {
    Trace &trace;
    DtlsProbe(Trace &value, rtc::impl::certificate_ptr certificate)
        : DtlsTransport(makeRetiredTransport<IceTransport>(rtc::Configuration{}, nullptr,
                            nullptr, nullptr), std::move(certificate), std::nullopt,
                        rtc::CertificateFingerprint::Algorithm::Sha256,
                        [](const std::string &) { return true; }, nullptr), trace(value) {}
    ~DtlsProbe() override { trace.destructionStarted = true; }
    using DtlsTransport::enqueueRecv;
    using DtlsTransport::incoming;
    void forceState(State value) { changeState(value); }
    bool outgoing(rtc::message_ptr) override { return true; } // No lower I/O.
};

struct TlsProbe final : TlsTransport {
    Trace &trace;
    TlsProbe(Trace &value, rtc::impl::certificate_ptr certificate)
        : TlsTransport(makeRetiredTransport<TcpTransport>("localhost", "0", nullptr),
                       std::nullopt, std::move(certificate), nullptr), trace(value) {}
    ~TlsProbe() override { trace.destructionStarted = true; }
    using TlsTransport::enqueueRecv;
    using TlsTransport::incoming;
    void forceState(State value) { changeState(value); }
    bool outgoing(rtc::message_ptr) override { return true; } // No lower I/O.
};

template <typename Probe>
auto makeProbe(Trace &trace, const rtc::impl::certificate_ptr &certificate, State state) {
    auto probe = makeRetiredTransport<Probe>(trace, certificate);
    probe->forceState(state);
    probe->onStateChange([&trace](State value) {
        if (value == State::Failed) ++trace.failed;
        if (value == State::Disconnected) ++trace.disconnected;
        if (trace.throwNotification) throw 17;
    });
    probe->onRecv([&trace](rtc::message_ptr message) {
        if (!message) ++trace.eof;
    });
    return probe;
}

template <typename Probe>
void retire(std::shared_ptr<Probe> &probe, Trace &trace) {
    probe.reset();
    // A normal/terminal pool task may still retain its owner after publishing
    // state. Observe deletion admission before joining the serial deletion lane;
    // joining an empty lane too early would not prove the owner has retired.
    await([&] { return trace.destructionStarted.load(); }, "last receive owner reaches reserved deletion");
    TearDownProcessor::Instance().join();
}

template <typename Probe>
void idleEofSettles(State initial, const rtc::impl::certificate_ptr &certificate) {
    Trace trace;
    auto probe = makeProbe<Probe>(trace, certificate, initial);
    probe->incoming(nullptr);
    await([&] { return initial == State::Connecting ? trace.failed == 1u : trace.eof == 1u; },
          "idle EOF schedules terminal completion without a later packet");
    retire(probe, trace);
    require(initial == State::Connecting
                ? trace.failed == 1u && trace.disconnected == 0u && trace.eof == 0u
                : trace.failed == 0u && trace.disconnected == 1u && trace.eof == 1u,
            "terminal state preserves connecting/connected distinction");
}

template <typename Probe>
void repeatedStopDoesNotAllocate(const rtc::impl::certificate_ptr &certificate) {
    Trace trace;
    auto probe = makeProbe<Probe>(trace, certificate, State::Connected);
    const auto before = refusedAllocations.load();
    refuseAllocation = true;
    probe->stop();
    probe->stop();
    await([&] { return trace.eof == 1u; }, "reserved stop reaches EOF under ordinary-new refusal");
    retire(probe, trace);
    refuseAllocation = false;
    require(trace.disconnected == 1u && trace.eof == 1u && refusedAllocations == before,
            "repeated stop and last-owner destruction use no new ordinary allocation");
}

template <typename Probe>
void receivePreparationRefusalSettles(const rtc::impl::certificate_ptr &certificate) {
    Trace trace;
    auto probe = makeProbe<Probe>(trace, certificate, State::Connecting);
    const auto before = refusedAllocations.load();
    refuseNextAllocation = true;
    probe->enqueueRecv();
    require(!refuseNextAllocation, "actual normal-receive preparation was refused");
    await([&] { return trace.failed == 1u; }, "preparation refusal uses reserved terminal completion");
    retire(probe, trace);
    require(refusedAllocations == before + 1u && trace.failed == 1u,
            "one refused normal admission produces one terminal failure");
}

template <typename Probe>
void throwingNotificationCannotSuppressEof(const rtc::impl::certificate_ptr &certificate) {
    Trace trace;
    trace.throwNotification = true;
    auto probe = makeProbe<Probe>(trace, certificate, State::Connected);
    probe->stop();
    await([&] { return trace.eof == 1u; }, "throwing state callback cannot suppress mandatory EOF");
    retire(probe, trace);
    require(trace.disconnected == 1u && trace.eof == 1u, "throwing callback does not repeat terminal state");
}

template <typename Probe>
void unusedTerminalReservationHasNoCycle(const rtc::impl::certificate_ptr &certificate) {
    Trace trace;
    auto probe = makeProbe<Probe>(trace, certificate, State::Disconnected);
    const auto before = refusedAllocations.load();
    refuseAllocation = true;
    retire(probe, trace);
    refuseAllocation = false;
    require(refusedAllocations == before && trace.failed == 0u && trace.eof == 0u,
            "unused reservation/destructor creates neither a self-cycle nor a weak-only wake");
}

template <typename Probe>
void runProtocol(const rtc::impl::certificate_ptr &certificate) {
    idleEofSettles<Probe>(State::Connecting, certificate);
    idleEofSettles<Probe>(State::Connected, certificate);
    repeatedStopDoesNotAllocate<Probe>(certificate);
    receivePreparationRefusalSettles<Probe>(certificate);
    throwingNotificationCannotSuppressEof<Probe>(certificate);
    unusedTerminalReservationHasNoCycle<Probe>(certificate);
}

void lateEofReplayNeedsNoAllocation() {
    struct ReceiveOnly final : Transport { using Transport::recv; };
    auto transport = makeRetiredTransport<ReceiveOnly>();
    auto first = std::make_shared<rtc::Message>(1);
    auto second = std::make_shared<rtc::Message>(1);
    (*first)[0] = std::byte{1};
    (*second)[0] = std::byte{2};
    transport->recv(first);
    transport->recv(second);
    int order[3] = {};
    unsigned count = 0;
    rtc::message_callback callback = [&](rtc::message_ptr message) {
        require(count < 3u, "pending EOF is replayed at most once");
        order[count++] = message ? std::to_integer<int>((*message)[0]) : -1;
    };
    const auto before = refusedAllocations.load();
    refuseAllocation = true;
    transport->recv(nullptr);
    transport->recv(nullptr);
    transport->onRecv(std::move(callback));
    refuseAllocation = false;
    require(count == 3u && order[0] == 1 && order[1] == 2 && order[2] == -1,
            "late callback receives queued messages followed by one retained EOF");
    require(refusedAllocations == before, "pending EOF admission and replay need no ordinary new");
    transport->onRecv(nullptr);
    transport.reset();
    TearDownProcessor::Instance().join();
}
} // namespace

int main() {
    Init::Instance().setThreadPoolSize(2);
    auto epoch = Init::Instance().token();
    auto certificate = std::make_shared<Certificate>(
        Certificate::Generate(rtc::CertificateType::Ecdsa, "receive-lifecycle-fixture"));
    Gate first, second;
    ThreadPool::Instance().enqueue([&] { first.hold(); });
    ThreadPool::Instance().enqueue([&] { second.hold(); });
    first.awaitEntry();
    second.awaitEntry();
    first.release();
    second.release();
    runProtocol<DtlsProbe>(certificate);
    runProtocol<TlsProbe>(certificate);
    lateEofReplayNeedsNoAllocation();
    certificate.reset();
    epoch.reset();
    auto cleanup = Init::Instance().cleanup();
    require(cleanup.wait_for(std::chrono::seconds(10)) == std::future_status::ready,
            "global RTC cleanup completes after every receive owner");
    cleanup.get();
    std::puts("PASS RTC receive terminal: 13 groups");
}
