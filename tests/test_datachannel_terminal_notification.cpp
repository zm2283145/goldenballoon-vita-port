// Actual patched PeerConnection/DataChannel/Track/WebSocket terminal paths.
// No connection, app or ROM is opened. Ordinary RTC services and worker threads
// are used. Allocation refusal is thread-local throwing new/new[] only; it does
// not cover malloc, over-aligned storage or arbitrary user callback behavior.
#include "impl/peerconnection.hpp"
#include "impl/websocket.hpp"
#include "reserved_owner_work.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <new>
#include <thread>

namespace {
thread_local int allocationsBeforeRefusal = -1;
std::atomic<unsigned> refusedAllocations{0};
void require(bool value, const char *message) {
    if (!value) {
        std::fprintf(stderr, "RTC terminal notification: %s\n", message);
        std::abort();
    }
}
void await(const std::atomic<unsigned> &value, unsigned expected) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (value.load() != expected && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    require(value.load() == expected, "terminal completion reached its bounded fixture deadline");
}
}

void *operator new(std::size_t size) {
    if (allocationsBeforeRefusal == 0) {
        ++refusedAllocations;
        throw std::bad_alloc();
    }
    if (allocationsBeforeRefusal > 0) --allocationsBeforeRefusal;
    if (auto *value = std::malloc(size ? size : 1)) return value;
    throw std::bad_alloc();
}
void *operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void *value) noexcept { std::free(value); }
void operator delete[](void *value) noexcept { std::free(value); }
void operator delete(void *value, std::size_t) noexcept { std::free(value); }
void operator delete[](void *value, std::size_t) noexcept { std::free(value); }

namespace rtc::impl {
struct MdkrTerminalNotificationTestAccess {
    static void warm(PeerConnection &peer) { (void)peer.mCertificate.get(); }
    static void drain(PeerConnection &peer) { peer.mProcessor.join(); }
    static void addTrack(PeerConnection &peer, const std::shared_ptr<Track> &track) {
        // Media-off public creation closes removed tracks immediately. Retain
        // a real Track here to exercise the actual final traversal/close body.
        std::unique_lock lock(peer.mTracksMutex);
        peer.mTracks.emplace(track->mid(), track);
        peer.mTrackLines.push_back(track);
    }
};
}

namespace {
using rtc::impl::PeerConnection;
using rtc::impl::Processor;
using rtc::impl::MdkrTerminalNotificationTestAccess;

struct BoundChannel : rtc::impl::DataChannel {
    explicit BoundChannel(const std::shared_ptr<PeerConnection> &peer)
        : DataChannel(peer, "bound", "", rtc::Reliability{}) {}
    void bind(const std::shared_ptr<rtc::impl::SctpTransport> &transport) {
        mSctpTransport = transport;
        mStream = 1;
    }
};

void parentTerminalRetainsCloseDespiteCallbackFailure() {
    auto peer = std::make_shared<PeerConnection>(rtc::Configuration{});
    MdkrTerminalNotificationTestAccess::warm(*peer);
    auto unassigned = peer->emplaceDataChannel("unassigned", rtc::DataChannelInit{});
    rtc::DataChannelInit assignedInit;
    assignedInit.id = 2;
    auto assigned = peer->emplaceDataChannel("assigned", assignedInit);
    auto track = std::make_shared<rtc::impl::Track>(peer,
        rtc::Description::Audio("fixture", rtc::Description::Direction::SendRecv));
    MdkrTerminalNotificationTestAccess::addTrack(*peer, track);
    std::atomic<unsigned> terminal{0}, closed{0}, channelClosed{0}, trackClosed{0};
    peer->stateChangeCallback = [&](PeerConnection::State state) {
        if (state == PeerConnection::State::Failed) {
            ++terminal;
            throw 7;
        }
        if (state == PeerConnection::State::Closed) {
            ++closed;
            peer->remoteClose(); // Recursive close must not duplicate jobs.
            throw 8;
        }
    };
    unassigned->closedCallback = [&] { ++channelClosed; throw 9; };
    assigned->closedCallback = [&] { ++channelClosed; };
    track->closedCallback = [&] { ++trackClosed; throw 10; };
    const auto refusedBefore = refusedAllocations.load();
    allocationsBeforeRefusal = 0;
    peer->requestTerminalClose(PeerConnection::State::Failed);
    peer->requestTerminalClose(PeerConnection::State::Disconnected);
    allocationsBeforeRefusal = -1;
    MdkrTerminalNotificationTestAccess::drain(*peer);
    require(refusedAllocations.load() == refusedBefore,
            "terminal admission does not allocate on caller");
    require(terminal == 1 && closed == 1 && channelClosed == 2 && trackClosed == 1,
            "terminal reason and final Closed plus all local closures complete once");
    require(peer->state == PeerConnection::State::Closed && unassigned->isClosed() &&
            assigned->isClosed() && track->isClosed(), "terminal ownership reaches all sinks");
    require(!unassigned->closedCallback && !track->closedCallback,
            "throwing closed callbacks cannot skip callback clearing");
    bool rejected = false;
    try { (void)peer->emplaceDataChannel("late", rtc::DataChannelInit{}); }
    catch (const std::logic_error &) { rejected = true; }
    require(rejected, "closed peer rejects late channel admission");
}

void closingParentNeverResetsRetainedSctpStream() {
    auto peer = std::make_shared<PeerConnection>(rtc::Configuration{});
    MdkrTerminalNotificationTestAccess::warm(*peer);
    auto transport = rtc::impl::makeRetiredTransport<rtc::impl::SctpTransport>(
        nullptr, rtc::Configuration{}, rtc::impl::SctpTransport::Ports{},
        nullptr, nullptr, nullptr);
    // No initialize/start: closeStream would allocate its reset message before
    // touching a socket. The live strong transport proves weak.lock succeeds.
    auto channel = std::make_shared<BoundChannel>(peer);
    channel->bind(transport);
    unsigned closed = 0;
    channel->closedCallback = [&] { ++closed; throw 11; };
    peer->closing = true;
    const auto refusedBefore = refusedAllocations.load();
    allocationsBeforeRefusal = 0;
    channel->close();
    allocationsBeforeRefusal = -1;
    require(refusedAllocations.load() == refusedBefore && closed == 1 && channel->isClosed(),
            "parent closure performs local finalization without SCTP reset allocation");
    peer->remoteClose();
    MdkrTerminalNotificationTestAccess::drain(*peer);
}

void refusedLocalStreamResetStillCompletesLocalClose() {
    auto peer = std::make_shared<PeerConnection>(rtc::Configuration{});
    MdkrTerminalNotificationTestAccess::warm(*peer);
    auto transport = rtc::impl::makeRetiredTransport<rtc::impl::SctpTransport>(
        nullptr, rtc::Configuration{}, rtc::impl::SctpTransport::Ports{},
        nullptr, nullptr, nullptr);
    auto channel = std::make_shared<BoundChannel>(peer);
    channel->bind(transport);
    unsigned closed = 0;
    channel->closedCallback = [&] { ++closed; };
    const auto refusedBefore = refusedAllocations.load();
    allocationsBeforeRefusal = 0;
    channel->close(); // Refuses actual closeStream reset-message allocation.
    allocationsBeforeRefusal = -1;
    MdkrTerminalNotificationTestAccess::drain(*peer);
    require(refusedAllocations.load() > refusedBefore && closed == 1 &&
            channel->isClosed() && !channel->closedCallback,
            "reset allocation refusal preserves local callback and reset");
    require(peer->state == PeerConnection::State::Closed,
            "reset refusal requests reserved parent terminal cleanup");
}

struct JoiningOwner : std::enable_shared_from_this<JoiningOwner> {
    std::atomic<unsigned> &destroyed;
    Processor processor;
    MdkrReservedOwnerWork<JoiningOwner, Processor, bool> work;
    explicit JoiningOwner(std::atomic<unsigned> &count)
        : destroyed(count), work(processor, [](JoiningOwner &, bool fail) {
            if (fail) throw 12;
        }) {}
    ~JoiningOwner() { processor.join(); ++destroyed; }
};

void capturedOwnerSurvivesContinuationAndRefusedReservationUnwinds() {
    for (int allowance : {0, 1}) {
        Processor processor;
        bool refused = false;
        allocationsBeforeRefusal = allowance;
        try {
            MdkrReservedOwnerWork<JoiningOwner, Processor, bool> slot(
                processor, [](JoiningOwner &, bool) {});
        } catch (const std::bad_alloc &) { refused = true; }
        allocationsBeforeRefusal = -1;
        require(refused, "State and actual prepared-record allocation refusals unwind");
    }
    for (bool fail : {false, true}) {
        std::atomic<unsigned> destroyed{0};
        auto owner = std::make_shared<JoiningOwner>(destroyed);
        allocationsBeforeRefusal = 0;
        require(owner->work.publish(owner, fail), "first publication succeeds");
        require(!owner->work.publish(owner, fail), "repeated publication is idempotent");
        allocationsBeforeRefusal = -1;
        owner.reset();
        await(destroyed, 1); // Would stall if owner died before continuation.
    }
}

void websocketRemoteClosureDoesNotNeedGracefulAllocation() {
    auto ws = std::make_shared<rtc::impl::WebSocket>();
    ws->state = rtc::impl::WebSocket::State::Connecting;
    unsigned closed = 0;
    ws->closedCallback = [&] { ++closed; ws->remoteClose(); throw 13; };
    const auto refusedBefore = refusedAllocations.load();
    allocationsBeforeRefusal = 0;
    ws->remoteClose();
    ws->remoteClose();
    allocationsBeforeRefusal = -1;
    require(ws->isClosed() && closed == 1 && refusedAllocations.load() == refusedBefore,
            "forced close and recursive closed callback need no graceful frame/timer allocation");
}
}

int main() {
    auto token = rtc::impl::Init::Instance().token();
    capturedOwnerSurvivesContinuationAndRefusedReservationUnwinds();
    parentTerminalRetainsCloseDespiteCallbackFailure();
    closingParentNeverResetsRetainedSctpStream();
    refusedLocalStreamResetStillCompletesLocalClose();
    websocketRemoteClosureDoesNotNeedGracefulAllocation();
    rtc::impl::TearDownProcessor::Instance().join();
    token.reset();
    rtc::impl::Init::Instance().cleanup().get();
    return 0;
}
