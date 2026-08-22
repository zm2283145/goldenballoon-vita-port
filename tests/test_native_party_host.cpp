#include "party/native_party_host.h"
#include "party/native_remote_pad_ingress.h"
#include "party/party_protocol.h"

/* Assert-driven test: NDEBUG would compile every check away (see
 * test_native_remote_pad_ingress.cpp). */
#undef NDEBUG

#include <array>
#include <cassert>
#include <deque>
#include <string>
#include <utility>
#include <vector>

namespace {

class FakeTransport final : public MdkrPartyTransport {
public:
    bool availableValue = true;
    bool commandResult = true;
    /* M4 fix round 1 seam: models the real transport's no-open-socket gate
     * (libdatachannel_party_transport.cpp command(): shuttingDown_ ||
     * !socket_ || !socket_->isOpen() refuses the send synchronously, and
     * closeRoom() then returns before flushCloseCommand() ever runs).
     * Consulted by closeRoom() only -- the one command whose refusal shape
     * the destructor boundary tests must see. Defaults to open so every
     * pre-existing test keeps its exact behavior. closeFlushWaits counts
     * entries into the bounded goodbye flush, mirroring the real
     * closeRoom()'s flush-only-after-a-successful-send order; the no-socket
     * tests pin it at zero (quit never waits on a goodbye it cannot send). */
    bool socketOpen = true;
    size_t closeFlushWaits = 0u;
    std::string reason;
    std::deque<MdkrPartyTransportEvent> events;
    std::vector<std::string> calls;

    bool available() const override { return availableValue; }
    const char *unavailableReason() const override { return reason.c_str(); }
    bool open(const std::string &origin) override {
        calls.push_back("open:" + origin); return commandResult;
    }
    bool approve(const std::string &id, unsigned seat) override {
        calls.push_back("approve:" + id + ":" + std::to_string(seat));
        return commandResult;
    }
    bool reject(const std::string &id) override {
        calls.push_back("reject:" + id); return commandResult;
    }
    bool remove(const std::string &id) override {
        calls.push_back("remove:" + id); return commandResult;
    }
    bool rotateInvite(unsigned generation) override {
        calls.push_back("rotate:" + std::to_string(generation));
        return commandResult;
    }
    bool revokeInvite() override {
        calls.push_back("revoke"); return commandResult;
    }
    bool closeRoom() override {
        if (!socketOpen) {
            calls.push_back("close-refused:no-socket");
            return false;
        }
        calls.push_back("close");
        if (commandResult) closeFlushWaits++;
        return commandResult;
    }
    bool sendRumble(const std::string &id, uint16_t strength) override {
        calls.push_back("rumble:" + id + ":" + std::to_string(strength));
        return commandResult;
    }
    bool poll(MdkrPartyTransportEvent &event) override {
        if (events.empty()) return false;
        event = std::move(events.front());
        events.pop_front();
        return true;
    }
    void shutdown() override { calls.push_back("shutdown"); }
};

MdkrNativePartyController pending(std::string id) {
    MdkrNativePartyController value;
    value.id = std::move(id);
    value.name = "A friend's phone";
    value.publicKey = std::string(87u, 'C');
    /* SAS v2: the phrase commits to both DTLS fingerprints, so it cannot
     * exist before the WebRTC descriptions are exchanged -- a pending phone
     * has no phrase, and approval must not wait for one. */
    value.pairingPhrase.clear();
    /* The service allocates the first signaling generation at redemption. */
    value.connectionSequence = 1u;
    return value;
}

MdkrNativePartyController approved(
    std::string id, unsigned seat, uint32_t lease, uint32_t connection) {
    MdkrNativePartyController value = pending(std::move(id));
    value.phase = MdkrNativePartyControllerPhase::Leased;
    value.seat = seat;
    value.leaseGeneration = lease;
    value.connectionSequence = connection;
    return value;
}

MdkrPartyTransportEvent roomEvent(
    uint64_t transition, unsigned generation, uint64_t expiresInMs,
    std::vector<MdkrNativePartyController> controllers = {}) {
    MdkrPartyTransportEvent event;
    event.type = MdkrPartyTransportEventType::RoomState;
    event.room.transitionId = transition;
    event.room.inviteGeneration = generation;
    event.room.inviteExpiresInMs = expiresInMs;
    event.room.controllerUrl = "https://party.example/controller/#secret";
    event.room.fallbackCode = "123456";
    event.room.inviteActive = true;
    event.room.controllers = std::move(controllers);
    return event;
}

std::vector<uint8_t> padPacket(uint32_t connection, uint32_t sequence) {
    MdkrPartyPadPacket source{};
    source.flags = MDKR_PARTY_PAD_FLAG_PRESENT;
    source.connection_sequence = connection;
    source.sample_sequence = sequence;
    source.sender_time_ms = sequence;
    source.buttons = 0x8000u;
    std::array<uint8_t, MDKR_PARTY_PAD_MAX_BYTES> output{};
    size_t length = 0u;
    assert(mdkr_party_pad_encode(
        &source, output.data(), output.size(), &length));
    return std::vector<uint8_t>(output.begin(), output.begin() + length);
}

void unavailableAndSecureOrigin() {
    mdkr_native_remote_pad_reset_all();
    FakeTransport transport;
    transport.availableValue = false;
    transport.reason = "This package has no native WebRTC adapter.";
    MdkrNativePartyHost host(transport);
    assert(!host.open("https://party.example"));
    assert(host.view().phase == MdkrNativePartyPhase::Error);
    assert(host.view().message == transport.reason);

    transport.availableValue = true;
    assert(!host.open("http://party.example"));
    assert(host.view().phase == MdkrNativePartyPhase::Error);
}

void lifecycleAndCustody() {
    mdkr_native_remote_pad_reset_all();
    FakeTransport transport;
    MdkrNativePartyHost host(transport);
    assert(host.open("https://party.example"));
    assert(host.view().phase == MdkrNativePartyPhase::Opening);
    transport.events.push_back(roomEvent(1u, 1u, 121000u, {pending("phone-a")}));
    host.service(1000u);
    assert(host.view().phase == MdkrNativePartyPhase::Open);
    assert(host.view().inviteVisible);
    assert(host.approve("phone-a", 2u));
    assert(!host.approve("phone-a", 3u));

    auto phone = approved("phone-a", 2u, 4u, 9u);
    transport.events.push_back(roomEvent(2u, 1u, 121000u, {phone}));
    host.service(1001u);
    uint64_t owner = 0u;
    uint32_t connection = 0u;
    assert(mdkr_native_remote_pad_info(1u, &owner, &connection));
    assert(owner == ((4u << 3u) | 2u) && connection == 9u);

    MdkrPartyTransportEvent connected;
    connected.type = MdkrPartyTransportEventType::ControllerConnected;
    connected.controllerId = "phone-a";
    connected.haptics = true;
    transport.events.push_back(connected);
    MdkrPartyTransportEvent packet;
    packet.type = MdkrPartyTransportEventType::ControllerPacket;
    packet.controllerId = "phone-a";
    packet.packet = padPacket(9u, 1u);
    transport.events.push_back(packet);
    host.service(1002u);
    assert(host.view().controllers[0].direct);
    std::array<uint8_t, MDKR_PARTY_PAD_MAX_BYTES> output{};
    assert(mdkr_native_remote_pad_pop(
        1u, owner, connection, output.data(), output.size()) > 0u);

    assert(mdkr_native_remote_pad_request_rumble(1u, 1234u));
    host.service(1003u);
    assert(transport.calls.back() == "rumble:phone-a:1234");

    MdkrPartyTransportEvent disconnected;
    disconnected.type = MdkrPartyTransportEventType::ControllerDisconnected;
    disconnected.controllerId = "phone-a";
    transport.events.push_back(disconnected);
    host.service(1004u);
    assert(!host.view().controllers[0].direct);
    assert(mdkr_native_remote_pad_info(1u, &owner, &connection));

    assert(host.dismissInvite());
    auto revoked = roomEvent(3u, 1u, 0u, {phone});
    revoked.room.inviteActive = false;
    revoked.room.controllerUrl.clear();
    revoked.room.fallbackCode.clear();
    transport.events.push_back(std::move(revoked));
    host.service(1005u);
    assert(host.view().phase == MdkrNativePartyPhase::InviteRevoked);
    assert(!host.view().inviteVisible);
    assert(mdkr_native_remote_pad_info(1u, &owner, &connection));

    assert(host.closeRoom());
    assert(host.view().phase == MdkrNativePartyPhase::Closed);
    assert(!mdkr_native_remote_pad_info(1u, &owner, &connection));
}

void invalidAndStaleUpdatesFailClosed() {
    mdkr_native_remote_pad_reset_all();
    FakeTransport transport;
    MdkrNativePartyHost host(transport);
    assert(host.open("https://party.example"));
    transport.events.push_back(roomEvent(2u, 2u, 5000u));
    host.service(1u);
    assert(host.view().transitionId == 2u);
    transport.events.push_back(roomEvent(1u, 1u, 5000u));
    host.service(2u);
    assert(host.view().transitionId == 2u);

    auto first = approved("one", 1u, 1u, 1u);
    auto second = approved("two", 1u, 2u, 2u);
    transport.events.push_back(roomEvent(3u, 2u, 5000u, {first, second}));
    host.service(3u);
    assert(host.view().phase == MdkrNativePartyPhase::Error);
    assert(!host.view().inviteVisible);
}

void expiryPreservesApprovedSeat() {
    mdkr_native_remote_pad_reset_all();
    FakeTransport transport;
    MdkrNativePartyHost host(transport);
    assert(host.open("https://party.example"));
    /* Relative expiresInMs=9 latched against nowMs=1 (host's own clock)
     * lands the deadline at exactly 10, matching the original absolute
     * fixture value this test was written against. */
    transport.events.push_back(roomEvent(
        1u, 1u, 9u, {approved("phone", 4u, 9u, 12u)}));
    host.service(1u);
    host.service(10u);
    assert(host.view().phase == MdkrNativePartyPhase::InviteRevoked);
    assert(host.view().controllers.size() == 1u);
    uint64_t owner = 0u;
    uint32_t connection = 0u;
    assert(mdkr_native_remote_pad_info(3u, &owner, &connection));
    assert(host.rotateInvite());
    assert(transport.calls.back() == "rotate:1");
}

void commandRejectionAndRemovalStayRecoverable() {
    mdkr_native_remote_pad_reset_all();
    FakeTransport transport;
    MdkrNativePartyHost host(transport);
    assert(host.open("https://party.example"));
    transport.events.push_back(roomEvent(1u, 1u, 5000u, {pending("phone")}));
    host.service(1u);
    assert(host.approve("phone", 1u));
    MdkrPartyTransportEvent rejected;
    rejected.type = MdkrPartyTransportEventType::CommandRejected;
    rejected.message = "That controller slot was just taken. Choose another.";
    transport.events.push_back(rejected);
    host.service(2u);
    assert(host.view().phase == MdkrNativePartyPhase::Open);
    assert(!host.view().controllers[0].commandPending);
    assert(host.view().inviteVisible);

    transport.events.push_back(roomEvent(
        2u, 1u, 5000u, {approved("phone", 1u, 2u, 1u)}));
    host.service(3u);
    assert(host.reject("phone"));
    assert(transport.calls.back() == "remove:phone");
}

/* C3 give-up review fix: a CommandRejected-shaped event that carries a
 * controller identity (the connect_timeout give-up path) must only clear
 * that one controller's commandPending. Before this fix, any CommandRejected
 * -- including this new autonomous 20-60 s timeout ladder, which can land at
 * any time, not just reactively right after a host-issued command -- cleared
 * every controller's commandPending, silently unblocking an unrelated
 * controller's genuinely in-flight command. An event with no controller
 * identity (a true host-command rejection) keeps the old room-wide
 * behavior. */
void giveUpClearsOnlyItsOwnControllersCommandPending() {
    mdkr_native_remote_pad_reset_all();
    FakeTransport transport;
    MdkrNativePartyHost host(transport);
    assert(host.open("https://party.example"));
    transport.events.push_back(roomEvent(
        1u, 1u, 5000u, {pending("phone-a"), pending("phone-b")}));
    host.service(1u);
    assert(host.approve("phone-a", 1u));
    assert(host.approve("phone-b", 2u));
    assert(host.view().controllers[0].commandPending);
    assert(host.view().controllers[1].commandPending);

    /* A give-up event scoped to phone-b only, with no intervening RoomState
     * (which would otherwise reset every commandPending on its own). */
    MdkrPartyTransportEvent timedOut;
    timedOut.type = MdkrPartyTransportEventType::CommandRejected;
    timedOut.controllerId = "phone-b";
    timedOut.message = "This phone could not connect. Remove it and pair again.";
    transport.events.push_back(timedOut);
    host.service(2u);

    assert(host.view().controllers[0].commandPending);   // phone-a: untouched
    assert(!host.view().controllers[1].commandPending);  // phone-b: its own
    assert(host.view().message ==
        "This phone could not connect. Remove it and pair again.");
}

/* I3: the worker's typed command errors reach the player honestly, per
 * controller. host_command_result{ok:false,error:"<code>"} arrives as a
 * CommandRejected carrying the code verbatim in errorCode; the host maps
 * the known codes to their exact copy and -- when the event names a
 * controller -- clears only that controller's commandPending, leaving an
 * unrelated controller's genuinely in-flight command pending. This is the
 * give-up scoping generalized to every command rejection that names a
 * controller, not just connect_timeout. An unknown code keeps the existing
 * generic copy exactly as before. */
void typedCommandErrorSurfacesHonestCopyPerController() {
    mdkr_native_remote_pad_reset_all();
    FakeTransport transport;
    MdkrNativePartyHost host(transport);
    assert(host.open("https://party.example"));
    transport.events.push_back(roomEvent(
        1u, 1u, 5000u, {pending("phone-a"), pending("phone-b")}));
    host.service(1u);
    assert(host.approve("phone-a", 1u));
    assert(host.approve("phone-b", 2u));
    assert(host.view().controllers[0].commandPending);
    assert(host.view().controllers[1].commandPending);

    /* The real transport shape: the generic prose is still present on the
     * event, but the typed code must win the copy decision. */
    MdkrPartyTransportEvent budget;
    budget.type = MdkrPartyTransportEventType::CommandRejected;
    budget.controllerId = "phone-b";
    budget.errorCode = "service_budget_safe";
    budget.message = "That controller action did not complete. Try again.";
    transport.events.push_back(budget);
    host.service(2u);

    /* The behavioral half: phone-a's own in-flight approve stays pending;
     * only phone-b's command was rejected. */
    assert(host.view().controllers[0].commandPending);
    assert(!host.view().controllers[1].commandPending);
    assert(host.view().message ==
        "The controller service has reached today's limit. "
        "Try again after midnight UTC.");

    MdkrPartyTransportEvent rotated;
    rotated.type = MdkrPartyTransportEventType::CommandRejected;
    rotated.errorCode = "invite_rotated";
    transport.events.push_back(rotated);
    host.service(3u);
    assert(host.view().message ==
        "That invite was replaced. Use the newest code.");

    MdkrPartyTransportEvent full;
    full.type = MdkrPartyTransportEventType::CommandRejected;
    full.controllerId = "phone-b";
    full.errorCode = "room_full";
    transport.events.push_back(full);
    host.service(4u);
    assert(host.view().message == "No free phone slot.");

    /* Unknown code: the existing copy, exactly as before this task. */
    MdkrPartyTransportEvent unknown;
    unknown.type = MdkrPartyTransportEventType::CommandRejected;
    unknown.errorCode = "invalid_state";
    transport.events.push_back(unknown);
    host.service(5u);
    assert(host.view().message ==
        "That controller action did not complete. Try again.");
}

/* Task-1 residual CLOSED: the worker now echoes the failed command's
 * identity on every host_command_result failure, so no rejection has to be
 * treated as room-wide any more. Two in-flight commands on different
 * controllers plus a room-level rotate in flight: a rotate failure (command
 * name, no controller) clears only the room-level busy flag and leaves both
 * controllers' genuinely in-flight commands pending; a failure that names
 * its controller clears only that controller's pending flag; and only an
 * identity-less event (a sender older than the echo contract) still falls
 * back to the conservative room-wide clear. */
void echoedIdentityScopesRejectionCleanupToItsOwnCommand() {
    mdkr_native_remote_pad_reset_all();
    FakeTransport transport;
    MdkrNativePartyHost host(transport);
    assert(host.open("https://party.example"));
    transport.events.push_back(roomEvent(
        1u, 1u, 5000u, {pending("phone-a"), pending("phone-b")}));
    host.service(1u);
    assert(host.approve("phone-a", 1u));
    assert(host.approve("phone-b", 2u));
    assert(host.rotateInvite());
    assert(host.view().controllers[0].commandPending);
    assert(host.view().controllers[1].commandPending);
    assert(host.view().busy);

    /* The rotate fails. Rotation was never any one phone's action: only the
     * busy flag was waiting on it. */
    MdkrPartyTransportEvent staleRotate;
    staleRotate.type = MdkrPartyTransportEventType::CommandRejected;
    staleRotate.command = "rotate";
    staleRotate.errorCode = "invalid_state";
    staleRotate.message = "That controller action did not complete. Try again.";
    transport.events.push_back(staleRotate);
    host.service(2u);
    assert(!host.view().busy);
    assert(host.view().controllers[0].commandPending);  // untouched
    assert(host.view().controllers[1].commandPending);  // untouched

    /* One of the two in-flight commands fails with its identity: only that
     * controller's pending clears. */
    MdkrPartyTransportEvent full;
    full.type = MdkrPartyTransportEventType::CommandRejected;
    full.command = "approve";
    full.controllerId = "phone-b";
    full.errorCode = "room_full";
    transport.events.push_back(full);
    host.service(3u);
    assert(host.view().controllers[0].commandPending);   // phone-a: untouched
    assert(!host.view().controllers[1].commandPending);  // phone-b: its own
    assert(host.view().message == "No free phone slot.");

    /* An event with no identity at all keeps the pre-echo conservative
     * room-wide behavior, so a stale worker can still never wedge the UI. */
    MdkrPartyTransportEvent legacy;
    legacy.type = MdkrPartyTransportEventType::CommandRejected;
    transport.events.push_back(legacy);
    host.service(4u);
    assert(!host.view().controllers[0].commandPending);
}

/* I5 sweep: a room_state controller entry with no seat assigned yet (the
 * wire's "seat" key absent entirely, not merely null -- a phone that has
 * paired but has not been approved to a seat) must reach the host as an
 * ordinary no-seat pending controller and never crash the launcher.
 *
 * The actual undefined-behaviour seam this guards (nlohmann's const
 * operator[] on a JSON object missing the key, guarded in
 * libdatachannel_party_transport.cpp's parseRoom with a contains() check)
 * lives entirely in JSON parsing that this host-only test binary never
 * touches -- mdkr_native_party_host_test links native_party_host.cpp and
 * native_remote_pad_ingress.cpp, not the transport, exactly like every
 * other test in this file. This is the downstream contract this file CAN
 * prove: once the transport hands the host a controller with no seat
 * (seat=0, phase=Pending -- what the fixed parser produces for a missing
 * key), the host applies it cleanly, keeps servicing it without incident,
 * and can still approve it onto a seat later. */
void seatlessRoomEntryAppliedAsNoSeatPendingWithoutCrash() {
    mdkr_native_remote_pad_reset_all();
    FakeTransport transport;
    MdkrNativePartyHost host(transport);
    assert(host.open("https://party.example"));
    transport.events.push_back(roomEvent(1u, 1u, 121000u, {pending("phone-a")}));
    host.service(1000u);

    assert(host.view().phase == MdkrNativePartyPhase::Open);
    assert(host.view().controllers.size() == 1u);
    assert(host.view().controllers[0].phase == MdkrNativePartyControllerPhase::Pending);
    assert(host.view().controllers[0].seat == 0u);

    /* Servicing again, as the launcher does every frame, must stay stable:
     * no seat means no owner derived, no ingress bind attempted, no crash. */
    host.service(1001u);
    assert(host.view().phase == MdkrNativePartyPhase::Open);
    assert(host.view().controllers[0].seat == 0u);

    /* Still a live, well-formed pending controller: approvable once a seat
     * is chosen, proving it was accepted rather than silently malformed. */
    assert(host.approve("phone-a", 3u));
}

/* I1 fix: the transport reports invite expiry as a RELATIVE duration
 * (inviteExpiresInMs, computed at parse time); the host must latch
 * nowMs + expiresInMs in its OWN service clock at the event-application
 * site (applyRoomState). Before this fix the host copied whatever number
 * the transport sent as if it were already an absolute instant in the
 * host's own clock -- correct only by coincidence when both clocks happen
 * to start near zero together. Real launches never satisfy that: the
 * transport's std::chrono::steady_clock runs since boot, the host's
 * SDL_GetTicks64 since SDL init. Simulate that gap with nowMs starting
 * near uptime scale (999,999,999 ms) while the invite is a fresh 2-minute
 * window. */
void expiryLatchesInHostsOwnClockDomain() {
    mdkr_native_remote_pad_reset_all();
    FakeTransport transport;
    MdkrNativePartyHost host(transport);
    assert(host.open("https://party.example"));

    constexpr uint64_t kHostNowMs = 999999999u;  // uptime far exceeds session runtime
    constexpr uint64_t kExpiresInMs = 120000u;   // the transport's relative report
    transport.events.push_back(roomEvent(1u, 1u, kExpiresInMs, {}));
    host.service(kHostNowMs);

    assert(host.view().phase == MdkrNativePartyPhase::Open);
    assert(host.view().inviteVisible);
    /* The deadline must live in the HOST's own clock domain: nowMs plus the
     * relative duration, not the raw 120000 the transport reported (which,
     * compared directly against a nowMs of ~10^9, would have looked expired
     * for eleven straight days under the pre-fix cross-domain compare). */
    assert(host.view().inviteExpiresAtMs == kHostNowMs + kExpiresInMs);

    /* ui_phone_party.cpp:184-185's countdown formula, asserted on host state
     * rather than the drawn ImGui text (no headless render seam here):
     * ceil((expiry - now) / 1000) seconds must read as 2:00. */
    const uint64_t secondsLeft =
        (host.view().inviteExpiresAtMs - kHostNowMs + 999u) / 1000u;
    assert(secondsLeft == 120u);
    assert(secondsLeft / 60u == 2u && secondsLeft % 60u == 0u);

    /* Nowhere near the latched deadline yet: still visible. */
    host.service(kHostNowMs + 1u);
    assert(host.view().inviteVisible);

    /* Exactly at the latched deadline in the HOST's clock -- proving the
     * deadline really tracks nowMs's domain, not the raw 120000. */
    host.service(kHostNowMs + kExpiresInMs);
    assert(!host.view().inviteVisible);
    assert(host.view().phase == MdkrNativePartyPhase::InviteRevoked);
}

/* Review fix: setTerminal() releases every seat and shuts the transport
 * down, but before this fix left a controller's needsRebind flag standing.
 * If a push failure (queue overflow revokes ingress custody, same shape as
 * the stall-recovery coverage in test_party_session_lifecycle.cpp) and a
 * terminal event land in the same drain cycle, service()'s top-of-loop heal
 * on the very next tick re-bound the already-released seat, flipped the
 * controller back to Connected and overwrote the terminal message -- all
 * inside a room the host had already declared over. F8 update: a generic
 * Error with a seated lease is recoverable now, so the terminal event for
 * a seated room is the typed RoomGone; the invariant under test -- a
 * terminal room never re-binds -- is unchanged. Prove the controller stays
 * put, the room stays RoomEnded, and the terminal message is not
 * clobbered. */
void terminalRoomGoneAfterPushFailureStaysEndedAndNeverRebinds() {
    mdkr_native_remote_pad_reset_all();
    FakeTransport transport;
    MdkrNativePartyHost host(transport);
    assert(host.open("https://party.example"));
    transport.events.push_back(roomEvent(1u, 1u, 121000u, {pending("phone-a")}));
    host.service(1000u);
    assert(host.approve("phone-a", 1u));

    auto phone = approved("phone-a", 1u, 4u, 9u);
    transport.events.push_back(roomEvent(2u, 1u, 121000u, {phone}));
    host.service(1001u);

    MdkrPartyTransportEvent connected;
    connected.type = MdkrPartyTransportEventType::ControllerConnected;
    connected.controllerId = "phone-a";
    connected.haptics = true;
    transport.events.push_back(connected);
    host.service(1002u);
    assert(host.view().controllers[0].direct);
    assert(host.view().controllers[0].phase ==
           MdkrNativePartyControllerPhase::Connected);

    /* Flood the bounded ingress queue with live packets for this same
     * controller -- the same overflow shape
     * stall_with_live_packets_recovers_input() in
     * test_party_session_lifecycle.cpp drives -- so that, part-way through,
     * a push fails and the host marks the controller needsRebind. Queue a
     * terminal Error right behind it: both land in the ONE drain cycle
     * below. */
    const uint32_t flood = MDKR_NATIVE_REMOTE_PAD_QUEUE_CAPACITY + 8u;
    uint32_t sequence = 1u;
    for (uint32_t index = 0u; index < flood; ++index) {
        MdkrPartyTransportEvent packet;
        packet.type = MdkrPartyTransportEventType::ControllerPacket;
        packet.controllerId = "phone-a";
        packet.packet = padPacket(9u, ++sequence);
        transport.events.push_back(packet);
    }
    MdkrPartyTransportEvent fatal;
    fatal.type = MdkrPartyTransportEventType::RoomGone;
    transport.events.push_back(fatal);
    host.service(2000u);

    assert(host.view().phase == MdkrNativePartyPhase::RoomEnded);
    assert(host.view().message == kMdkrPartyRoomEndedCopy);
    assert(host.view().controllers[0].phase ==
           MdkrNativePartyControllerPhase::Leased);

    /* The next tick, with no new events at all, is exactly where the bug
     * lived: the top-of-service heal loop must not resurrect the released
     * seat inside an ended room. */
    host.service(2100u);
    assert(host.view().phase == MdkrNativePartyPhase::RoomEnded);
    assert(host.view().message == kMdkrPartyRoomEndedCopy);
    assert(host.view().controllers[0].phase ==
           MdkrNativePartyControllerPhase::Leased);
    assert(!host.view().controllers[0].needsRebind);
    uint64_t owner = 0u;
    uint32_t connection = 0u;
    assert(!mdkr_native_remote_pad_info(0u, &owner, &connection));
}

/* F8: only credential-invalid and room-closed are terminal -- on this model
 * those arrive as the typed RoomGone (and the host's own Closed) -- so every
 * OTHER transport error while seats hold leases must keep them and show a
 * recovery state instead of tearing the room down. The pinned offender: an
 * invalid room update mid-session (both transports emit it as a generic
 * Error event, and the host's own roomStateValid refusal took the same
 * setError path) used to release every seat and land phase Error; the
 * connected phone's direct channel never dropped, so the teardown was pure
 * self-harm. Now: phase Recovering, seats and ingress custody intact, pad
 * packets still flowing, and the next valid room update recovers to Open. */
void recoverableTransportErrorKeepsLeasesAndShowsRecovery() {
    mdkr_native_remote_pad_reset_all();
    FakeTransport transport;
    MdkrNativePartyHost host(transport);
    assert(host.open("https://party.example"));
    transport.events.push_back(roomEvent(
        1u, 1u, 121000u, {approved("phone-a", 1u, 4u, 9u)}));
    host.service(1000u);
    MdkrPartyTransportEvent connected;
    connected.type = MdkrPartyTransportEventType::ControllerConnected;
    connected.controllerId = "phone-a";
    connected.haptics = true;
    transport.events.push_back(connected);
    host.service(1001u);
    assert(host.view().controllers[0].direct);

    /* The invalid-update shape both transports emit as a plain Error. */
    MdkrPartyTransportEvent fault;
    fault.type = MdkrPartyTransportEventType::Error;
    fault.message = "Controller service sent an invalid room update.";
    transport.events.push_back(fault);
    host.service(2000u);
    assert(host.view().phase == MdkrNativePartyPhase::Recovering);
    assert(host.view().message ==
           "Controller service sent an invalid room update.");
    assert(host.view().controllers.size() == 1u);
    assert(host.view().controllers[0].seat == 1u);
    assert(host.view().controllers[0].direct);
    uint64_t owner = 0u;
    uint32_t connection = 0u;
    assert(mdkr_native_remote_pad_info(0u, &owner, &connection));
    assert(owner == ((4u << 3u) | 1u) && connection == 9u);

    /* The direct channel never dropped: its input still reaches the sim. */
    MdkrPartyTransportEvent packet;
    packet.type = MdkrPartyTransportEventType::ControllerPacket;
    packet.controllerId = "phone-a";
    packet.packet = padPacket(9u, 1u);
    transport.events.push_back(packet);
    host.service(2001u);
    std::array<uint8_t, MDKR_PARTY_PAD_MAX_BYTES> output{};
    assert(mdkr_native_remote_pad_pop(
        0u, owner, connection, output.data(), output.size()) > 0u);

    /* The host's own roomStateValid refusal takes the same recovery path
     * while seats are held: two controllers on one seat is invalid. */
    auto first = approved("one", 2u, 1u, 1u);
    auto second = approved("two", 2u, 2u, 2u);
    transport.events.push_back(roomEvent(5u, 1u, 121000u, {first, second}));
    host.service(2002u);
    assert(host.view().phase == MdkrNativePartyPhase::Recovering);
    assert(host.view().controllers.size() == 1u);
    assert(mdkr_native_remote_pad_info(0u, &owner, &connection));

    /* The next valid room update recovers the surface to Open. */
    transport.events.push_back(roomEvent(
        6u, 1u, 121000u, {approved("phone-a", 1u, 4u, 9u)}));
    host.service(3000u);
    assert(host.view().phase == MdkrNativePartyPhase::Open);
    assert(mdkr_native_remote_pad_info(0u, &owner, &connection));

    /* Boundary unchanged: with no seat held (a room that never got past
     * bootstrap), the same Error event is still terminal -- there is no
     * lease to protect and the retry button is the honest way forward. */
    assert(host.closeRoom());
    assert(host.open("https://party.example"));
    MdkrPartyTransportEvent createFault;
    createFault.type = MdkrPartyTransportEventType::Error;
    createFault.message = "Could not create a secure phone controller room.";
    transport.events.push_back(createFault);
    host.service(4000u);
    assert(host.view().phase == MdkrNativePartyPhase::Error);
}

/* I2: a phone whose controller page speaks a different pairing-protocol
 * version must be loud on the host screen, not an indistinguishable
 * "Reconnecting". The transport emits ControllerProtocolMismatch (it keeps
 * the peer up -- the phone may reload into a matching page); the host must
 * mark the seat with its own visible state and honest copy, demote the seat
 * to neutral input, keep the room and the seat's lease intact, and never
 * spin the C1 rebind heal loop over it (a rebind cannot fix a version
 * gap and its "Phone input reconnected." copy would be a lie). A genuine
 * ControllerConnected -- the phone reloaded into a matching version -- is
 * the recovery that clears the state. */
void protocolMismatchMarksSeatLoudlyWithoutRebindLoop() {
    mdkr_native_remote_pad_reset_all();
    FakeTransport transport;
    MdkrNativePartyHost host(transport);
    assert(host.open("https://party.example"));
    auto phone = approved("phone-a", 1u, 4u, 9u);
    transport.events.push_back(roomEvent(1u, 1u, 121000u, {phone}));
    host.service(1000u);

    MdkrPartyTransportEvent connected;
    connected.type = MdkrPartyTransportEventType::ControllerConnected;
    connected.controllerId = "phone-a";
    connected.haptics = true;
    transport.events.push_back(connected);
    host.service(1001u);
    assert(host.view().controllers[0].direct);

    /* The phone reloads into a page speaking a fake future protocol. */
    MdkrPartyTransportEvent mismatch;
    mismatch.type = MdkrPartyTransportEventType::ControllerProtocolMismatch;
    mismatch.controllerId = "phone-a";
    mismatch.theirProtocol = 3u;
    transport.events.push_back(mismatch);
    const size_t callsBefore = transport.calls.size();
    host.service(1002u);

    assert(host.view().controllers[0].protocolMismatch);
    assert(!host.view().controllers[0].direct);
    assert(!host.view().controllers[0].haptics);
    assert(host.view().controllers[0].phase ==
           MdkrNativePartyControllerPhase::Leased);
    assert(host.view().message ==
        "This phone's controller page is a different version. "
        "Refresh the page on the phone.");
    /* Not torn down: the room stays open, the seat keeps its lease, and the
     * host issued no remove/reject command of its own. */
    assert(host.view().phase == MdkrNativePartyPhase::Open);
    assert(transport.calls.size() == callsBefore);
    uint64_t owner = 0u;
    uint32_t connection = 0u;
    assert(mdkr_native_remote_pad_info(0u, &owner, &connection));

    /* Ticks pass with no events: the heal loop must not resurrect the seat
     * into a lying "Connected"/"Phone input reconnected." surface. */
    host.service(1600u);
    host.service(2200u);
    assert(host.view().controllers[0].protocolMismatch);
    assert(!host.view().controllers[0].needsRebind);
    assert(!host.view().controllers[0].direct);
    assert(host.view().controllers[0].phase ==
           MdkrNativePartyControllerPhase::Leased);
    assert(host.view().message ==
        "This phone's controller page is a different version. "
        "Refresh the page on the phone.");

    /* Its packets stay out of the sim while mismatched (direct is false, so
     * the ControllerPacket arm refuses them; nothing reaches ingress). */
    MdkrPartyTransportEvent packet;
    packet.type = MdkrPartyTransportEventType::ControllerPacket;
    packet.controllerId = "phone-a";
    packet.packet = padPacket(9u, 1u);
    transport.events.push_back(packet);
    host.service(2300u);
    std::array<uint8_t, MDKR_PARTY_PAD_MAX_BYTES> output{};
    assert(mdkr_native_remote_pad_pop(
        0u, (4u << 3u) | 1u, 9u, output.data(), output.size()) == 0u);

    /* Recovery: the phone reloads into a matching page version and
     * completes controller_ready for real. The honest state clears. */
    MdkrPartyTransportEvent recovered;
    recovered.type = MdkrPartyTransportEventType::ControllerConnected;
    recovered.controllerId = "phone-a";
    recovered.haptics = true;
    transport.events.push_back(recovered);
    host.service(2400u);
    assert(!host.view().controllers[0].protocolMismatch);
    assert(host.view().controllers[0].direct);
    assert(host.view().controllers[0].phase ==
           MdkrNativePartyControllerPhase::Connected);
}

/* I2 review fix: a controller-scoped CommandRejected naming a
 * version-mismatched seat must not overwrite the room message. The one
 * such rejection that arrives on its own schedule is the transport's C3
 * give-up ("This phone could not connect. Remove it and pair again.") --
 * for a protocol gap that remedy is simply wrong (removing and re-pairing
 * cannot fix a page version), and it would sit in the room banner
 * contradicting the seat row's correct "refresh the phone" copy. The
 * rejection still does its bookkeeping (commandPending clears); only the
 * message overwrite is suppressed, and only for the mismatched seat --
 * rejections naming a healthy controller, and room-wide rejections, keep
 * their existing surfaces exactly. */
void giveUpForMismatchedSeatKeepsTheHonestRoomCopy() {
    mdkr_native_remote_pad_reset_all();
    FakeTransport transport;
    MdkrNativePartyHost host(transport);
    assert(host.open("https://party.example"));
    transport.events.push_back(roomEvent(
        1u, 1u, 121000u, {approved("phone-a", 1u, 4u, 9u), pending("phone-b")}));
    host.service(1000u);

    MdkrPartyTransportEvent mismatch;
    mismatch.type = MdkrPartyTransportEventType::ControllerProtocolMismatch;
    mismatch.controllerId = "phone-a";
    mismatch.theirProtocol = 3u;
    transport.events.push_back(mismatch);
    host.service(1001u);
    assert(host.view().message ==
        "This phone's controller page is a different version. "
        "Refresh the page on the phone.");

    /* The C3 ladder's give-up shape, ~60 s later on the pre-fix transport.
     * The honest copy must survive it. */
    MdkrPartyTransportEvent gaveUp;
    gaveUp.type = MdkrPartyTransportEventType::CommandRejected;
    gaveUp.controllerId = "phone-a";
    gaveUp.message = "This phone could not connect. Remove it and pair again.";
    transport.events.push_back(gaveUp);
    host.service(61000u);
    assert(host.view().message ==
        "This phone's controller page is a different version. "
        "Refresh the page on the phone.");
    assert(!host.view().controllers[0].commandPending);

    /* Scoped suppression only: a rejection naming a healthy controller
     * still surfaces its copy (typed code and generic prose alike). */
    assert(host.approve("phone-b", 2u));
    MdkrPartyTransportEvent full;
    full.type = MdkrPartyTransportEventType::CommandRejected;
    full.controllerId = "phone-b";
    full.errorCode = "room_full";
    transport.events.push_back(full);
    host.service(61001u);
    assert(host.view().message == "No free phone slot.");
    assert(!host.view().controllers[1].commandPending);

    /* Room-wide rejections (no controller identity) are untouched too. */
    MdkrPartyTransportEvent roomWide;
    roomWide.type = MdkrPartyTransportEventType::CommandRejected;
    roomWide.message = "That controller slot was just taken. Choose another.";
    transport.events.push_back(roomWide);
    host.service(61002u);
    assert(host.view().message ==
        "That controller slot was just taken. Choose another.");
}

/* I4: a room the service will never bring back (deleted by its 24 h alarm,
 * expired, or otherwise refusing every resume for good) must end in an
 * explicit terminal state, not today's silent forever-ladder. The transport
 * classifies the refusals and emits one RoomGone event; the host must land
 * in phase RoomEnded with the exact terminal sentence, release every seat,
 * shut the transport down, and -- terminal means terminal -- never open the
 * transport again on its own. The named way forward, creating a new invite,
 * is a fresh open() into a brand-new room, and must still work. */
void roomGoneForGoodEndsTheRoomInsteadOfRetryingForever() {
    mdkr_native_remote_pad_reset_all();
    FakeTransport transport;
    MdkrNativePartyHost host(transport);
    const auto openCalls = [&transport]() {
        size_t opens = 0u;
        for (const std::string &call : transport.calls) {
            if (call.rfind("open:", 0u) == 0u) opens++;
        }
        return opens;
    };
    assert(host.open("https://party.example"));
    transport.events.push_back(roomEvent(
        1u, 1u, 121000u, {approved("phone-a", 1u, 4u, 9u)}));
    host.service(1000u);
    MdkrPartyTransportEvent connected;
    connected.type = MdkrPartyTransportEventType::ControllerConnected;
    connected.controllerId = "phone-a";
    connected.haptics = true;
    transport.events.push_back(connected);
    host.service(1001u);
    assert(host.view().controllers[0].direct);

    MdkrPartyTransportEvent gone;
    gone.type = MdkrPartyTransportEventType::RoomGone;
    transport.events.push_back(gone);
    host.service(2000u);

    assert(host.view().phase == MdkrNativePartyPhase::RoomEnded);
    assert(host.view().message ==
        "This controller room has ended. Create a new invite to keep playing.");
    assert(!host.view().busy);
    assert(!host.view().inviteVisible);
    assert(host.view().controllerUrl.empty());
    assert(host.view().fallbackCode.empty());
    assert(transport.calls.back() == "shutdown");
    /* The seat went back to local play, fail-neutral. */
    uint64_t owner = 0u;
    uint32_t connection = 0u;
    assert(!mdkr_native_remote_pad_info(0u, &owner, &connection));

    /* Zero further transport opens -- and no other traffic on the dead
     * room -- no matter how many ticks pass. */
    assert(openCalls() == 1u);
    const size_t callsBefore = transport.calls.size();
    for (int tick = 0; tick < 32; ++tick) {
        host.service(2100u + static_cast<uint64_t>(tick));
    }
    assert(transport.calls.size() == callsBefore);
    assert(openCalls() == 1u);
    assert(host.view().phase == MdkrNativePartyPhase::RoomEnded);
    assert(host.view().message ==
        "This controller room has ended. Create a new invite to keep playing.");

    /* A dead room cannot rotate an invite back into existence. */
    assert(!host.rotateInvite());
    assert(openCalls() == 1u);

    /* The copy's remedy must actually work: a fresh open() starts a brand
     * new room. That is the ONE way another open ever happens. */
    assert(host.open("https://party.example"));
    assert(host.view().phase == MdkrNativePartyPhase::Opening);
    assert(openCalls() == 2u);
}

/* M4: quitting the app while a room is live must tell the phones goodbye.
 * Destroying the host is the quit path; it must issue the transport's close
 * command -- the worker relays it to every controller as host_closed
 * (services/party/src/party-room.ts) -- BEFORE shutting the transport down.
 * Before this fix the destructor only shut the socket, and the phones
 * misread the silent drop as a network fault they should wait out. The
 * bounded (250 ms cap) flush that keeps quit from blocking lives in the
 * real transport and is proven with a fake clock in
 * tests/test_native_party_sas.cpp; this fake transport is instantaneous,
 * so the order is the whole contract here. */
void destructionWithLiveRoomSaysGoodbyeBeforeHangingUp() {
    mdkr_native_remote_pad_reset_all();
    FakeTransport transport;
    {
        MdkrNativePartyHost host(transport);
        assert(host.open("https://party.example"));
        transport.events.push_back(roomEvent(
            1u, 1u, 121000u, {approved("phone-a", 1u, 4u, 9u)}));
        host.service(1000u);
        assert(host.view().phase == MdkrNativePartyPhase::Open);
    }
    /* The goodbye precedes the hangup, and the hangup still happens. */
    assert(transport.calls.size() >= 2u);
    assert(transport.calls[transport.calls.size() - 2u] == "close");
    assert(transport.calls.back() == "shutdown");
    /* And the seat still went back to local play on the way out. */
    uint64_t owner = 0u;
    uint32_t connection = 0u;
    assert(!mdkr_native_remote_pad_info(0u, &owner, &connection));
}

/* M4 boundary: with no live room there is no one to say goodbye to. A
 * never-opened, an explicitly closed, and a terminal (RoomEnded) host must
 * all destroy with a plain hangup and no close command -- the closed and
 * terminal transports are already shut down (closeRoom / setTerminal), so
 * a goodbye there would be a command fired into a dead socket. */
void destructionWithoutLiveRoomHangsUpWithoutAGoodbye() {
    mdkr_native_remote_pad_reset_all();
    {   /* Never opened: the destructor's own shutdown and nothing else. */
        FakeTransport transport;
        { MdkrNativePartyHost host(transport); }
        assert((transport.calls == std::vector<std::string>{"shutdown"}));
    }
    {   /* Terminal: the room is gone for good, transport already down. */
        FakeTransport transport;
        size_t callsAtDestruction = 0u;
        {
            MdkrNativePartyHost host(transport);
            assert(host.open("https://party.example"));
            transport.events.push_back(roomEvent(1u, 1u, 121000u, {}));
            host.service(1000u);
            MdkrPartyTransportEvent gone;
            gone.type = MdkrPartyTransportEventType::RoomGone;
            transport.events.push_back(gone);
            host.service(1001u);
            assert(host.view().phase == MdkrNativePartyPhase::RoomEnded);
            callsAtDestruction = transport.calls.size();
        }
        assert(transport.calls.size() == callsAtDestruction + 1u);
        assert(transport.calls.back() == "shutdown");
    }
    {   /* Explicitly closed: closeRoom() already said the goodbye itself. */
        FakeTransport transport;
        size_t callsAtDestruction = 0u;
        {
            MdkrNativePartyHost host(transport);
            assert(host.open("https://party.example"));
            transport.events.push_back(roomEvent(1u, 1u, 121000u, {}));
            host.service(1000u);
            assert(host.closeRoom());
            assert(host.view().phase == MdkrNativePartyPhase::Closed);
            callsAtDestruction = transport.calls.size();
        }
        assert(transport.calls.size() == callsAtDestruction + 1u);
        assert(transport.calls.back() == "shutdown");
    }
}

/* M4 fix round 1: the review's uncovered boundary -- destruction while the
 * room exists but NO socket is open. Opening (open() called, no room_state
 * yet) and Recovering (socket down mid-reconnect) are live phases, so the
 * destructor still attempts the goodbye; but the real transport has no
 * open socket to carry it, refuses the send synchronously at command()'s
 * gate, and skips the bounded flush entirely
 * (libdatachannel_party_transport.cpp closeRoom() returns before
 * flushCloseCommand()) -- quit pays zero wait for a goodbye it cannot
 * send. The fake models exactly that gate via socketOpen, and its
 * closeFlushWaits counter is the fake clock here: it must stay at zero. */
void destructionDuringOpeningAttemptsGoodbyeButNeverWaits() {
    mdkr_native_remote_pad_reset_all();
    FakeTransport transport;
    {
        MdkrNativePartyHost host(transport);
        assert(host.open("https://party.example"));
        assert(host.view().phase == MdkrNativePartyPhase::Opening);
        /* The signaling socket has not completed its handshake yet. */
        transport.socketOpen = false;
    }
    /* The attempt was made, refused for want of a socket, no flush wait
     * was consumed -- and the hangup still happened, in order. */
    assert(transport.calls.size() >= 2u);
    assert(transport.calls[transport.calls.size() - 2u] ==
           "close-refused:no-socket");
    assert(transport.calls.back() == "shutdown");
    assert(transport.closeFlushWaits == 0u);
}

void destructionDuringRecoveringAttemptsGoodbyeButNeverWaits() {
    mdkr_native_remote_pad_reset_all();
    FakeTransport transport;
    {
        MdkrNativePartyHost host(transport);
        assert(host.open("https://party.example"));
        transport.events.push_back(roomEvent(
            1u, 1u, 121000u, {approved("phone-a", 1u, 4u, 9u)}));
        host.service(1000u);
        assert(host.view().phase == MdkrNativePartyPhase::Open);
        MdkrPartyTransportEvent recovering;
        recovering.type = MdkrPartyTransportEventType::Recovering;
        transport.events.push_back(recovering);
        host.service(2000u);
        assert(host.view().phase == MdkrNativePartyPhase::Recovering);
        /* Mid-reconnect: the old socket is gone, the new one is not up. */
        transport.socketOpen = false;
    }
    assert(transport.calls.size() >= 2u);
    assert(transport.calls[transport.calls.size() - 2u] ==
           "close-refused:no-socket");
    assert(transport.calls.back() == "shutdown");
    assert(transport.closeFlushWaits == 0u);
    /* The seat still went back to local play on the way out. */
    uint64_t owner = 0u;
    uint32_t connection = 0u;
    assert(!mdkr_native_remote_pad_info(0u, &owner, &connection));
}

/* SAS v2 phrase lifecycle. The transport can only derive the phrase once
 * both WebRTC descriptions are set, which is after approval, so: a pending
 * phone with NO phrase must still be approvable; the late ControllerPhrase
 * event attaches the phrase to its seat; and a later room update for the
 * SAME connection (which carries no phrase -- the service never knows one)
 * must not wipe it. A new connectionSequence is a new channel whose phrase
 * has not arrived yet, so the old phrase must go rather than vouch for a
 * channel it never described. */
void phraseArrivesAtConnectionAndSurvivesRoomUpdates() {
    mdkr_native_remote_pad_reset_all();
    FakeTransport transport;
    MdkrNativePartyHost host(transport);
    assert(host.open("https://party.example"));
    transport.events.push_back(roomEvent(1u, 1u, 121000u, {pending("phone-a")}));
    host.service(1000u);
    assert(host.view().controllers[0].pairingPhrase.empty());
    /* Approval no longer waits for a phrase that cannot exist yet. */
    assert(host.approve("phone-a", 1u));

    transport.events.push_back(roomEvent(
        2u, 1u, 121000u, {approved("phone-a", 1u, 4u, 9u)}));
    host.service(1001u);
    MdkrPartyTransportEvent phrase;
    phrase.type = MdkrPartyTransportEventType::ControllerPhrase;
    phrase.controllerId = "phone-a";
    phrase.message = "Gentle-Star Royal-Pilot";
    transport.events.push_back(phrase);
    host.service(1002u);
    assert(host.view().controllers[0].pairingPhrase ==
           "Gentle-Star Royal-Pilot");

    /* The connected-transition room update carries no phrase; the seat's
     * verified phrase stays put. */
    auto phone = approved("phone-a", 1u, 4u, 9u);
    phone.phase = MdkrNativePartyControllerPhase::Connected;
    transport.events.push_back(roomEvent(3u, 1u, 121000u, {phone}));
    host.service(1003u);
    assert(host.view().controllers[0].phase ==
           MdkrNativePartyControllerPhase::Connected);
    assert(host.view().controllers[0].pairingPhrase ==
           "Gentle-Star Royal-Pilot");

    /* A fresh connectionSequence is a different channel: the stale phrase
     * must not be shown for it. */
    transport.events.push_back(roomEvent(
        4u, 1u, 121000u, {approved("phone-a", 1u, 4u, 10u)}));
    host.service(1004u);
    assert(host.view().controllers[0].pairingPhrase.empty());

    /* A disconnect ends the channel the words vouched for, so the words go
     * with it. The refused-reconnect shape matters most: if the fresh
     * answer's fingerprints are refused, the transport derives NO new
     * phrase, yet the channel still completes -- the seat must then show no
     * words at all rather than the previous channel's. */
    MdkrPartyTransportEvent rejoined;
    rejoined.type = MdkrPartyTransportEventType::ControllerPhrase;
    rejoined.controllerId = "phone-a";
    rejoined.message = "Mighty-Kite Wild-Kite";
    transport.events.push_back(rejoined);
    host.service(1005u);
    assert(host.view().controllers[0].pairingPhrase ==
           "Mighty-Kite Wild-Kite");
    MdkrPartyTransportEvent dropped;
    dropped.type = MdkrPartyTransportEventType::ControllerDisconnected;
    dropped.controllerId = "phone-a";
    transport.events.push_back(dropped);
    host.service(1006u);
    assert(host.view().controllers[0].pairingPhrase.empty());
    /* Reconnect completes WITHOUT a ControllerPhrase event (the transport
     * refused the new answer's fingerprints): still no words. */
    MdkrPartyTransportEvent reconnected;
    reconnected.type = MdkrPartyTransportEventType::ControllerConnected;
    reconnected.controllerId = "phone-a";
    transport.events.push_back(reconnected);
    host.service(1007u);
    assert(host.view().controllers[0].phase ==
           MdkrNativePartyControllerPhase::Connected);
    assert(host.view().controllers[0].pairingPhrase.empty());
}

/* F11: the invite code renders as two groups of three, display only --
 * grouping is the UI's concern and must never invent structure for a value
 * that is not exactly the six digits the room minted. */
void groupedFallbackCodeIsDisplayOnly() {
    assert(mdkr_party_grouped_fallback_code("123456") == "123 456");
    assert(mdkr_party_grouped_fallback_code("000000") == "000 000");
    assert(mdkr_party_grouped_fallback_code("12345") == "12345");
    assert(mdkr_party_grouped_fallback_code("1234567") == "1234567");
    assert(mdkr_party_grouped_fallback_code("12a456") == "12a456");
    assert(mdkr_party_grouped_fallback_code("") == "");
}

/* Item 4: the join-request attention cue fires once per NEW pending phone --
 * never on the first (priming) observation, a repeat render, or an approval --
 * and a phone that leaves and later re-requests fires again. Pure detection
 * twin the launcher surface drives; the ImGui cue itself has no UI harness. */
void newPendingAttentionFiresOncePerFreshPending() {
    MdkrPartyPendingAttention attention;
    const auto pending = [](const char *id) {
        MdkrNativePartyController controller;
        controller.id = id;
        controller.phase = MdkrNativePartyControllerPhase::Pending;
        return controller;
    };
    /* First observation primes -- a room already holding a waiting phone must
     * not flash when the overlay opens onto it. */
    assert(mdkr_party_note_new_pending(attention, {pending("a")}) == 0u);
    /* A repeat render of the same pending phone does not re-fire. */
    assert(mdkr_party_note_new_pending(attention, {pending("a")}) == 0u);
    /* A genuinely new pending phone fires exactly once. */
    assert(mdkr_party_note_new_pending(attention,
        {pending("a"), pending("b")}) == 1u);
    /* Two brand-new phones arriving together fire twice. */
    assert(mdkr_party_note_new_pending(attention,
        {pending("a"), pending("b"), pending("c"), pending("d")}) == 2u);
    /* Approving one (it leaves the pending set) is not a new pending. */
    MdkrNativePartyController approved;
    approved.id = "a";
    approved.phase = MdkrNativePartyControllerPhase::Approved;
    approved.seat = 1u;
    assert(mdkr_party_note_new_pending(attention,
        {approved, pending("b"), pending("c"), pending("d")}) == 0u);
    /* A phone that left and later re-requests fires again. */
    assert(mdkr_party_note_new_pending(attention, {pending("b")}) == 0u);
    assert(mdkr_party_note_new_pending(attention,
        {pending("b"), pending("a")}) == 1u);
    /* An empty room clears the set; the next arrival is fresh once more. */
    assert(mdkr_party_note_new_pending(attention, {}) == 0u);
    assert(mdkr_party_note_new_pending(attention, {pending("b")}) == 1u);
}

/* F4: the C3 give-up's copy forks on what actually failed. With the room
 * socket healthy the whole ladder, signaling delivered every offer and the
 * phone still never connected -- the network between the devices is the
 * diagnosis, and all three surfaces speak this exact sentence. With the
 * socket down the generic remedy stays: signaling itself was broken, so
 * nothing about the direct path was proven. TURN has been server-delivered
 * since wave 0, which is exactly why the rare healthy-socket give-up must
 * name the real cause instead of a remedy that cannot help. */
void giveUpCopyNamesTheNetworkOnlyWhenSignalingWasHealthy() {
    assert(std::string(mdkr_party_give_up_copy(true)) ==
           kMdkrPartyIceBlockedCopy);
    assert(std::string(mdkr_party_give_up_copy(false)) ==
           "This phone could not connect. Remove it and pair again.");
    assert(std::string(kMdkrPartyIceBlockedCopy) ==
           "This network blocks phone-to-display connections. "
           "Try another Wi-Fi network or a phone hotspot.");
}

size_t rotateCallCount(const FakeTransport &transport) {
    size_t count = 0u;
    for (const std::string &call : transport.calls) {
        if (call.rfind("rotate:", 0u) == 0u) count++;
    }
    return count;
}

/* F6: while the invite card is actually on screen (the UI reports each
 * frame it draws the QR via noteInviteDisplayed), the host rotates the
 * invite on its own once ~75% of the TTL has elapsed, so a displayed
 * QR/code is always redeemable. Binding security decision: the TTL itself
 * never lengthens -- perceived permanence comes from rotation, and an
 * invite nobody is displaying still dies at its ordinary TTL without a
 * single rotate. */
void displayedInviteAutoRotatesBeforeItsTtlLapses() {
    mdkr_native_remote_pad_reset_all();
    FakeTransport transport;
    MdkrNativePartyHost host(transport);
    assert(host.open("https://party.example"));
    transport.events.push_back(roomEvent(1u, 1u, 120000u, {}));
    host.service(1000u);
    assert(host.view().phase == MdkrNativePartyPhase::Open);
    assert(host.view().inviteExpiresAtMs == 121000u);

    /* Displayed, but under 75% elapsed: no rotation. */
    host.noteInviteDisplayed(90999u);
    host.service(90999u);
    assert(rotateCallCount(transport) == 0u);

    /* Displayed at 75% elapsed (30 s remaining of 120 s): rotate now. */
    host.noteInviteDisplayed(91000u);
    host.service(91000u);
    assert(rotateCallCount(transport) == 1u);
    assert(transport.calls.back() == "rotate:1");
    assert(host.view().busy);
    /* The old invite's deadline is untouched until the rotated room state
     * arrives: rotation, never TTL extension. */
    assert(host.view().inviteExpiresAtMs == 121000u);
    /* busy gates a duplicate rotate while the command is in flight. */
    host.noteInviteDisplayed(91100u);
    host.service(91100u);
    assert(rotateCallCount(transport) == 1u);

    /* The rotated invite lands with a fresh generation and full TTL; kept
     * on screen, it auto-rotates again at ITS 75% mark. */
    transport.events.push_back(roomEvent(2u, 2u, 120000u, {}));
    host.service(92000u);
    assert(host.view().phase == MdkrNativePartyPhase::Open);
    host.noteInviteDisplayed(181999u);
    host.service(181999u);
    assert(rotateCallCount(transport) == 1u);
    host.noteInviteDisplayed(182000u);
    host.service(182000u);
    assert(rotateCallCount(transport) == 2u);
    assert(transport.calls.back() == "rotate:2");

    /* An invite nobody displays never rotates: it expires at its ordinary
     * TTL exactly as before this feature existed. */
    transport.events.push_back(roomEvent(3u, 3u, 120000u, {}));
    host.service(183000u);
    assert(host.view().phase == MdkrNativePartyPhase::Open);
    for (uint64_t at = 213000u; at <= 303000u; at += 30000u) {
        host.service(at);
    }
    assert(rotateCallCount(transport) == 2u);
    assert(host.view().phase == MdkrNativePartyPhase::InviteRevoked);

    /* A stale display report (the card left the screen a while ago) does
     * not count as displayed. */
    assert(host.rotateInvite());
    transport.events.push_back(roomEvent(4u, 4u, 120000u, {}));
    host.service(304000u);
    assert(host.view().phase == MdkrNativePartyPhase::Open);
    host.noteInviteDisplayed(304000u);
    host.service(304000u + 119000u);
    assert(rotateCallCount(transport) == 3u);  // only the manual one above
}

/* F2: a lease that has never reached Connected is CONNECTING, not
 * reconnecting -- the model carries the distinction so both host surfaces
 * (ui_phone_party.cpp statusText, the browser host's seat tile) can show
 * honest copy. The flag survives room updates and disconnects. */
void neverConnectedLeaseReadsAsConnecting() {
    mdkr_native_remote_pad_reset_all();
    FakeTransport transport;
    MdkrNativePartyHost host(transport);
    assert(host.open("https://party.example"));
    auto phone = approved("phone-a", 1u, 2u, 3u);
    transport.events.push_back(roomEvent(1u, 1u, 121000u, {phone}));
    host.service(1000u);
    assert(!host.view().controllers[0].everConnected);

    /* Another room update of the same never-connected lease keeps it so. */
    transport.events.push_back(roomEvent(2u, 1u, 121000u, {phone}));
    host.service(1001u);
    assert(!host.view().controllers[0].everConnected);

    MdkrPartyTransportEvent connected;
    connected.type = MdkrPartyTransportEventType::ControllerConnected;
    connected.controllerId = "phone-a";
    transport.events.push_back(connected);
    host.service(1002u);
    assert(host.view().controllers[0].everConnected);

    /* A drop demotes the phase but never the history: this lease HAS
     * connected, so its surface may honestly say "reconnecting". */
    MdkrPartyTransportEvent dropped;
    dropped.type = MdkrPartyTransportEventType::ControllerDisconnected;
    dropped.controllerId = "phone-a";
    transport.events.push_back(dropped);
    host.service(1003u);
    assert(host.view().controllers[0].phase ==
           MdkrNativePartyControllerPhase::Leased);
    assert(host.view().controllers[0].everConnected);

    /* Carried across a room update of the same phone + key. */
    transport.events.push_back(roomEvent(3u, 1u, 121000u, {phone}));
    host.service(1004u);
    assert(host.view().controllers[0].everConnected);

    /* A room update that itself says Connected marks the history even when
     * no ControllerConnected event ever reached this host instance. */
    auto phoneConnected = approved("phone-b", 2u, 2u, 3u);
    phoneConnected.phase = MdkrNativePartyControllerPhase::Connected;
    transport.events.push_back(
        roomEvent(4u, 1u, 121000u, {phone, phoneConnected}));
    host.service(1005u);
    assert(host.view().controllers[1].everConnected);
}

/* RTT surfacing: each matched pong on a seat's control channel arrives as a
 * ControllerRtt event; the model keeps the newest sample per seat so both
 * host surfaces can show "NN ms · direct". The sample describes ONE live
 * channel: it survives room updates of the same connection, and dies with
 * the channel -- a disconnect, a protocol mismatch, or a fresh
 * connectionSequence all clear it rather than letting an old channel's
 * number vouch for a new one. */
void rttSampleTracksItsOwnChannelOnly() {
    mdkr_native_remote_pad_reset_all();
    FakeTransport transport;
    MdkrNativePartyHost host(transport);
    assert(host.open("https://party.example"));
    auto phone = approved("phone-a", 1u, 4u, 9u);
    transport.events.push_back(roomEvent(1u, 1u, 121000u, {phone}));
    MdkrPartyTransportEvent connected;
    connected.type = MdkrPartyTransportEventType::ControllerConnected;
    connected.controllerId = "phone-a";
    transport.events.push_back(connected);
    host.service(1000u);
    assert(host.view().controllers[0].rttMs == 0u);  // no sample yet

    MdkrPartyTransportEvent rtt;
    rtt.type = MdkrPartyTransportEventType::ControllerRtt;
    rtt.controllerId = "phone-a";
    rtt.rttMs = 23u;
    transport.events.push_back(rtt);
    host.service(1001u);
    assert(host.view().controllers[0].rttMs == 23u);

    /* Newest sample wins; an unknown controller's sample is dropped. */
    rtt.rttMs = 41u;
    transport.events.push_back(rtt);
    MdkrPartyTransportEvent ghost = rtt;
    ghost.controllerId = "phone-zz";
    transport.events.push_back(ghost);
    host.service(1002u);
    assert(host.view().controllers[0].rttMs == 41u);

    /* Survives a room update of the same connection... */
    transport.events.push_back(roomEvent(2u, 1u, 121000u, {phone}));
    host.service(1003u);
    assert(host.view().controllers[0].rttMs == 41u);

    /* ...but a fresh connectionSequence is a new channel with no sample. */
    transport.events.push_back(roomEvent(
        3u, 1u, 121000u, {approved("phone-a", 1u, 4u, 10u)}));
    host.service(1004u);
    assert(host.view().controllers[0].rttMs == 0u);

    /* A disconnect ends the measured channel: the sample goes with it. */
    rtt.rttMs = 17u;
    transport.events.push_back(rtt);
    host.service(1005u);
    assert(host.view().controllers[0].rttMs == 17u);
    MdkrPartyTransportEvent dropped;
    dropped.type = MdkrPartyTransportEventType::ControllerDisconnected;
    dropped.controllerId = "phone-a";
    transport.events.push_back(dropped);
    host.service(1006u);
    assert(host.view().controllers[0].rttMs == 0u);

    /* A protocol mismatch demotes the channel: no number may vouch for it. */
    transport.events.push_back(connected);
    rtt.rttMs = 12u;
    transport.events.push_back(rtt);
    host.service(1007u);
    assert(host.view().controllers[0].rttMs == 12u);
    MdkrPartyTransportEvent mismatch;
    mismatch.type = MdkrPartyTransportEventType::ControllerProtocolMismatch;
    mismatch.controllerId = "phone-a";
    transport.events.push_back(mismatch);
    host.service(1008u);
    assert(host.view().controllers[0].rttMs == 0u);
}

MdkrPartyTransportEvent renameEvent(std::string id, std::string name) {
    MdkrPartyTransportEvent event;
    event.type = MdkrPartyTransportEventType::ControllerRenamed;
    event.controllerId = std::move(id);
    event.message = std::move(name);
    return event;
}

/* F1 session-alive names: controller_rename arrives over the direct control
 * channel as a ControllerRenamed event. The model applies it under the same
 * bounds the redeem-time name field enforces (24 code points, no control /
 * zero-width / bidi code points, no edge whitespace, 48 bytes) -- refusing,
 * not repairing, anything else -- and the applied name survives room updates
 * that still carry the redeem-time name. */
void controllerRenameIsStrictAndSurvivesRoomUpdates() {
    mdkr_native_remote_pad_reset_all();
    FakeTransport transport;
    MdkrNativePartyHost host(transport);
    assert(host.open("https://party.example"));
    auto phone = approved("phone-a", 1u, 2u, 3u);
    transport.events.push_back(roomEvent(1u, 1u, 121000u, {phone}));
    host.service(1000u);
    assert(host.view().controllers[0].name == "A friend's phone");

    transport.events.push_back(renameEvent("phone-a", "Blue Racer"));
    host.service(1001u);
    assert(host.view().controllers[0].name == "Blue Racer");

    /* The next room update still carries the redeem-time name; the live
     * rename must not be clobbered by it. */
    transport.events.push_back(roomEvent(2u, 1u, 121000u, {phone}));
    host.service(1002u);
    assert(host.view().controllers[0].name == "Blue Racer");

    /* Refusals: a control byte, a bidi override, edge whitespace, over the
     * 24-code-point budget, malformed UTF-8, an unknown controller. */
    transport.events.push_back(renameEvent("phone-a", "Bad\tName"));
    transport.events.push_back(renameEvent("phone-a", "Bad\xE2\x80\xAEName"));
    transport.events.push_back(renameEvent("phone-a", " Padded"));
    transport.events.push_back(renameEvent("phone-a", "Padded "));
    transport.events.push_back(renameEvent("phone-a", std::string(25u, 'x')));
    transport.events.push_back(renameEvent("phone-a", "Bad\xFFName"));
    transport.events.push_back(renameEvent("phone-zz", "Ghost"));
    host.service(1003u);
    assert(host.view().controllers[0].name == "Blue Racer");

    /* An empty rename clears the cosmetic name -- the field is optional. */
    transport.events.push_back(renameEvent("phone-a", ""));
    host.service(1004u);
    assert(host.view().controllers[0].name.empty());

    /* A pending phone has no authenticated channel; a rename naming one is
     * refused outright. */
    transport.events.push_back(
        roomEvent(3u, 1u, 121000u, {phone, pending("phone-p")}));
    transport.events.push_back(renameEvent("phone-p", "Sneaky"));
    host.service(1005u);
    assert(host.view().controllers[1].name == "A friend's phone");

    /* A rename never follows an id whose key changed: a different phone
     * under a reused id gets the room's own name, not the old rename. */
    transport.events.push_back(renameEvent("phone-a", "Blue Racer"));
    host.service(1006u);
    auto swapped = phone;
    swapped.publicKey = std::string(87u, 'D');
    transport.events.push_back(roomEvent(4u, 1u, 121000u, {swapped}));
    host.service(1007u);
    assert(host.view().controllers[0].name == "A friend's phone");
}

/* F1 flood guard semantics: dedupe against the last admitted name (the
 * browser host's exact behavior), a humane fresh-name budget per fixed
 * window (a rename is a human act), and recovery in the next window. */
void renameGateDedupesAndRateLimits() {
    MdkrPartyRenameGate gate;
    assert(mdkr_party_rename_admit(gate, "Blue Racer", 10000u));
    assert(!mdkr_party_rename_admit(gate, "Blue Racer", 10001u));
    assert(mdkr_party_rename_admit(gate, "Red Racer", 10002u));
    assert(mdkr_party_rename_admit(gate, "Green Racer", 10003u));
    /* Burst spent: nothing fresh for the rest of the window... */
    assert(!mdkr_party_rename_admit(gate, "Gold Racer", 10004u));
    assert(!mdkr_party_rename_admit(gate, "Gold Racer",
                                    10000u + kMdkrPartyRenameWindowMs - 1u));
    /* ...and the next window admits a human's next rename. */
    assert(mdkr_party_rename_admit(gate, "Gold Racer",
                                   10000u + kMdkrPartyRenameWindowMs));
    /* The dedupe outlives windows: the same name never re-enqueues. */
    assert(!mdkr_party_rename_admit(gate, "Gold Racer",
                                    10000u + 10u * kMdkrPartyRenameWindowMs));
}

size_t rumbleSendCount(const FakeTransport &transport) {
    size_t count = 0u;
    for (const std::string &call : transport.calls) {
        if (call.rfind("rumble:", 0u) == 0u) count++;
    }
    return count;
}

/* One connected, haptics-capable phone on seat 2 (ingress port 1), ready
 * for the M5 sustained-rumble scenarios below. */
void connectHapticPhone(FakeTransport &transport, MdkrNativePartyHost &host) {
    assert(host.open("https://party.example"));
    transport.events.push_back(roomEvent(
        1u, 1u, 300000u, {approved("phone-a", 2u, 4u, 9u)}));
    MdkrPartyTransportEvent connected;
    connected.type = MdkrPartyTransportEventType::ControllerConnected;
    connected.controllerId = "phone-a";
    connected.haptics = true;
    transport.events.push_back(connected);
    host.service(1000u);
    assert(host.view().controllers[0].direct);
    assert(host.view().controllers[0].haptics);
}

/* M5 sustained rumble. The engine's SDL path sustains an effect by letting
 * the motor run on a 60 s safety duration (platform_sdl_min.c), but the
 * phone path's rumble command is a deliberate 250 ms one-shot -- so a
 * sustained effect must be KEPT alive by the host re-sending the command
 * every 200 ms for as long as the engine mailbox still holds a strength
 * above zero. The one-shot stays: a lost refresh fails silent-off, never
 * stuck-on. */
void sustainedRumbleRefreshesWhileTheMailboxHoldsStrength() {
    mdkr_native_remote_pad_reset_all();
    FakeTransport transport;
    MdkrNativePartyHost host(transport);
    connectHapticPhone(transport, host);

    /* The engine starts a sustained effect: sent immediately, once. */
    assert(mdkr_native_remote_pad_request_rumble(1u, 1234u));
    host.service(2000u);
    assert(rumbleSendCount(transport) == 1u);
    assert(transport.calls.back() == "rumble:phone-a:1234");

    /* Inside the 200 ms refresh interval: silence -- the phone's own
     * 250 ms one-shot is still running. */
    host.service(2100u);
    assert(rumbleSendCount(transport) == 1u);

    /* Held to 200 ms: the host re-sends unprompted, same strength -- the
     * engine posted nothing new, the mailbox simply still says "on". */
    host.service(2200u);
    assert(rumbleSendCount(transport) == 2u);
    assert(transport.calls.back() == "rumble:phone-a:1234");

    /* And keeps that cadence while the effect holds. */
    host.service(2350u);
    assert(rumbleSendCount(transport) == 2u);
    host.service(2450u);
    assert(rumbleSendCount(transport) == 3u);

    /* The engine stops the effect: the stop goes out immediately (never
     * rate-limited -- a stop must not wait out a refresh window), and then
     * nothing, ever -- strength zero is not a sustained effect. */
    assert(mdkr_native_remote_pad_request_rumble(1u, 0u));
    host.service(2500u);
    assert(rumbleSendCount(transport) == 4u);
    assert(transport.calls.back() == "rumble:phone-a:0");
    host.service(2750u);
    host.service(3000u);
    host.service(60000u);
    assert(rumbleSendCount(transport) == 4u);
}

/* M5 guard rail: a seat that stops being a live direct channel -- the
 * phone dropped, or its page turned out to speak the wrong protocol
 * version -- gets NO refreshes, however long the engine's last "on" would
 * otherwise have been sustained. The transport's 250 ms one-shot then ends
 * the motor on its own: fail silent-off. */
void mismatchedOrDisconnectedSeatsGetNoRumbleRefreshes() {
    const MdkrPartyTransportEventType interruptions[] = {
        MdkrPartyTransportEventType::ControllerProtocolMismatch,
        MdkrPartyTransportEventType::ControllerDisconnected,
    };
    for (const MdkrPartyTransportEventType type : interruptions) {
        mdkr_native_remote_pad_reset_all();
        FakeTransport transport;
        MdkrNativePartyHost host(transport);
        connectHapticPhone(transport, host);

        assert(mdkr_native_remote_pad_request_rumble(1u, 900u));
        host.service(2000u);
        assert(rumbleSendCount(transport) == 1u);

        MdkrPartyTransportEvent interruption;
        interruption.type = type;
        interruption.controllerId = "phone-a";
        transport.events.push_back(interruption);
        host.service(2010u);
        assert(!host.view().controllers[0].direct);

        /* Past one refresh window, past many: not one more send. */
        host.service(2300u);
        host.service(2600u);
        host.service(60000u);
        assert(rumbleSendCount(transport) == 1u);
    }
}

}  // namespace

int main() {
    unavailableAndSecureOrigin();
    lifecycleAndCustody();
    invalidAndStaleUpdatesFailClosed();
    expiryPreservesApprovedSeat();
    commandRejectionAndRemovalStayRecoverable();
    giveUpClearsOnlyItsOwnControllersCommandPending();
    typedCommandErrorSurfacesHonestCopyPerController();
    echoedIdentityScopesRejectionCleanupToItsOwnCommand();
    seatlessRoomEntryAppliedAsNoSeatPendingWithoutCrash();
    recoverableTransportErrorKeepsLeasesAndShowsRecovery();
    expiryLatchesInHostsOwnClockDomain();
    terminalRoomGoneAfterPushFailureStaysEndedAndNeverRebinds();
    protocolMismatchMarksSeatLoudlyWithoutRebindLoop();
    giveUpForMismatchedSeatKeepsTheHonestRoomCopy();
    roomGoneForGoodEndsTheRoomInsteadOfRetryingForever();
    destructionWithLiveRoomSaysGoodbyeBeforeHangingUp();
    destructionWithoutLiveRoomHangsUpWithoutAGoodbye();
    destructionDuringOpeningAttemptsGoodbyeButNeverWaits();
    destructionDuringRecoveringAttemptsGoodbyeButNeverWaits();
    phraseArrivesAtConnectionAndSurvivesRoomUpdates();
    groupedFallbackCodeIsDisplayOnly();
    newPendingAttentionFiresOncePerFreshPending();
    displayedInviteAutoRotatesBeforeItsTtlLapses();
    giveUpCopyNamesTheNetworkOnlyWhenSignalingWasHealthy();
    neverConnectedLeaseReadsAsConnecting();
    rttSampleTracksItsOwnChannelOnly();
    controllerRenameIsStrictAndSurvivesRoomUpdates();
    renameGateDedupesAndRateLimits();
    sustainedRumbleRefreshesWhileTheMailboxHoldsStrength();
    mismatchedOrDisconnectedSeatsGetNoRumbleRefreshes();
    mdkr_native_remote_pad_reset_all();
    return 0;
}
