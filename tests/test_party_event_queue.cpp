/*
 * MdkrPartyEventQueue overflow policy.
 *
 * This is the direct regression test for the transport's former ad-hoc
 * bounded deque: at capacity it used to clear itself, push one synthetic
 * Error and latch overflowed_ forever, so ~1.6 s of unserviced host with
 * four phones' pad heartbeats destroyed the whole room. The fixed policy
 * must survive a sustained burst by dropping only droppable (pad-packet)
 * events, keep every room/control event, and never latch: a second burst
 * after a drain must deliver exactly as the first one did.
 */
#include "party/party_event_queue.h"

/* Assert-driven test: NDEBUG would compile every check away. */
#undef NDEBUG

#include <cassert>
#include <cstdint>
#include <vector>

namespace {

MdkrPartyTransportEvent padPacketEvent(uint32_t sequence) {
    MdkrPartyTransportEvent event;
    event.type = MdkrPartyTransportEventType::ControllerPacket;
    event.controllerId = "phone-a";
    /* The queue never inspects packet bytes; the sequence just lets the
     * test tell surviving events apart after a drain. */
    event.packet = {static_cast<uint8_t>(sequence & 0xffu),
                     static_cast<uint8_t>((sequence >> 8u) & 0xffu)};
    return event;
}

MdkrPartyTransportEvent roomStateEvent(uint64_t transitionId) {
    MdkrPartyTransportEvent event;
    event.type = MdkrPartyTransportEventType::RoomState;
    event.room.transitionId = transitionId;
    return event;
}

/* (a) A pad-packet flood past capacity drops the oldest packets, keeps the
 * queue at exactly its bound, counts every drop, and never manufactures an
 * Error the way the old overflow latch did. */
void floodOfDroppablePacketsDropsOldestAndCounts() {
    constexpr size_t kCapacity = 128u;
    MdkrPartyEventQueue queue(kCapacity);
    constexpr uint32_t kPushed = 200u;
    for (uint32_t sequence = 0u; sequence < kPushed; ++sequence) {
        queue.push(padPacketEvent(sequence));
    }

    const std::vector<MdkrPartyTransportEvent> drained = queue.drain();
    assert(drained.size() == kCapacity);
    assert(queue.droppedPadPackets() == kPushed - kCapacity);

    for (const MdkrPartyTransportEvent &event : drained) {
        assert(event.type != MdkrPartyTransportEventType::Error);
    }

    /* The surviving 128 must be the newest 128 (sequences 72..199), in
     * order: dropping the oldest, not a random or newest one. */
    for (size_t index = 0u; index < drained.size(); ++index) {
        const uint32_t expected =
            static_cast<uint32_t>(kPushed - kCapacity + index);
        const uint32_t actual =
            static_cast<uint32_t>(drained[index].packet[0]) |
            (static_cast<uint32_t>(drained[index].packet[1]) << 8u);
        assert(actual == expected);
    }
}

/* (b) Room/control events are never droppable: interleaved among a much
 * larger packet flood, all of them survive a capacity-128 queue, in order.
 * The CommandRejected among them proves the I3 payload extension rides the
 * queue intact: its typed errorCode survives verbatim next to its prose. */
void roomStateSurvivesAPacketFloodInOrder() {
    constexpr size_t kCapacity = 128u;
    MdkrPartyEventQueue queue(kCapacity);
    uint32_t sequence = 0u;
    for (uint64_t transition = 1u; transition <= 4u; ++transition) {
        for (int i = 0; i < 75; ++i) queue.push(padPacketEvent(sequence++));
        queue.push(roomStateEvent(transition));
    }
    MdkrPartyTransportEvent rejected;
    rejected.type = MdkrPartyTransportEventType::CommandRejected;
    rejected.errorCode = "room_full";
    rejected.message = "That controller action did not complete. Try again.";
    queue.push(rejected);
    for (int i = 0; i < 100; ++i) queue.push(padPacketEvent(sequence++));

    const std::vector<MdkrPartyTransportEvent> drained = queue.drain();
    assert(drained.size() == kCapacity);

    std::vector<uint64_t> roomStateOrder;
    size_t rejections = 0u;
    for (const MdkrPartyTransportEvent &event : drained) {
        if (event.type == MdkrPartyTransportEventType::RoomState) {
            roomStateOrder.push_back(event.room.transitionId);
        }
        if (event.type == MdkrPartyTransportEventType::CommandRejected) {
            rejections++;
            assert(event.errorCode == "room_full");
            assert(event.message ==
                "That controller action did not complete. Try again.");
        }
    }
    assert((roomStateOrder == std::vector<uint64_t>{1u, 2u, 3u, 4u}));
    assert(rejections == 1u);
}

/* (c) No latch: a drain must not leave the queue unable to accept or
 * deliver a second burst, unlike the old overflowed_ flag that was set once
 * and never reset. */
void drainThenPushDeliversTheNextBurstToo() {
    constexpr size_t kCapacity = 8u;
    MdkrPartyEventQueue queue(kCapacity);
    for (uint32_t sequence = 0u; sequence < 20u; ++sequence) {
        queue.push(padPacketEvent(sequence));
    }
    const std::vector<MdkrPartyTransportEvent> firstDrain = queue.drain();
    assert(firstDrain.size() == kCapacity);
    assert(queue.droppedPadPackets() == 20u - kCapacity);

    /* The queue must be empty and fully usable right after a drain. */
    const std::vector<MdkrPartyTransportEvent> emptyDrain = queue.drain();
    assert(emptyDrain.empty());

    for (uint32_t sequence = 100u; sequence < 105u; ++sequence) {
        queue.push(padPacketEvent(sequence));
    }
    const std::vector<MdkrPartyTransportEvent> secondDrain = queue.drain();
    assert(secondDrain.size() == 5u);
    assert(queue.droppedPadPackets() == 20u - kCapacity);
    for (size_t index = 0u; index < secondDrain.size(); ++index) {
        const uint32_t expected = 100u + static_cast<uint32_t>(index);
        const uint32_t actual =
            static_cast<uint32_t>(secondDrain[index].packet[0]) |
            (static_cast<uint32_t>(secondDrain[index].packet[1]) << 8u);
        assert(actual == expected);
    }
}

/* F1 flood guard. ControllerRenamed is non-droppable (a rename mutates the
 * host's model and must never vanish silently), which is only safe because
 * the transports admit controller_rename through the per-peer
 * mdkr_party_rename_admit gate BEFORE any event exists: repeats of the
 * admitted name are dropped (the browser host dedupes the same way) and
 * fresh names are capped per fixed window, so a phone blasting ~8k
 * renames/s can never fill the shared queue and evict or refuse other
 * seats' pad packets. */
void renameFloodCannotStarvePadInput() {
    MdkrPartyRenameGate gate;
    MdkrPartyEventQueue queue(128u);

    unsigned admitted = 0u;
    for (unsigned index = 0u; index < 8000u; ++index) {
        /* ~8 messages per simulated millisecond = one second of flood. */
        const uint64_t nowMs = 10000u + index / 8u;
        if (mdkr_party_rename_admit(
                gate, "name-" + std::to_string(index), nowMs)) {
            MdkrPartyTransportEvent renamed;
            renamed.type = MdkrPartyTransportEventType::ControllerRenamed;
            renamed.controllerId = "phone-a";
            renamed.message = "name-" + std::to_string(index);
            queue.push(std::move(renamed));
            admitted++;
        }
    }
    assert(admitted <= kMdkrPartyRenameWindowMessages);

    /* Another seat's pad heartbeats arrive during and after the flood and
     * every one of them must still cross the queue. */
    for (uint32_t sequence = 0u; sequence < 16u; ++sequence) {
        queue.push(padPacketEvent(sequence));
    }
    const std::vector<MdkrPartyTransportEvent> drained = queue.drain();
    size_t pads = 0u;
    for (const MdkrPartyTransportEvent &event : drained) {
        if (event.type == MdkrPartyTransportEventType::ControllerPacket) {
            pads++;
        }
    }
    assert(pads == 16u);
    assert(queue.droppedPadPackets() == 0u);
}

}  // namespace

int main() {
    floodOfDroppablePacketsDropsOldestAndCounts();
    roomStateSurvivesAPacketFloodInOrder();
    drainThenPushDeliversTheNextBurstToo();
    renameFloodCannotStarvePadInput();
    return 0;
}
