#include "native_party_host.h"

#include "native_remote_pad_ingress.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <set>

namespace {

constexpr size_t kMaxControllers = 8u;
constexpr size_t kMaxEventsPerService = 64u;
constexpr size_t kMaxControllerId = 64u;
constexpr size_t kMaxName = 48u;
constexpr size_t kMaxPhrase = 48u;
constexpr size_t kPublicKeyLength = 87u;
constexpr size_t kMaxUrl = 2048u;
constexpr size_t kMaxMessage = 240u;
/* C1 self-heal: at most one rebind attempt per controller per this window,
 * so a phone that keeps overflowing the queue cannot make service() spin. */
constexpr uint64_t kRebindRateLimitMs = 500u;
/* M5 sustained rumble: re-send cadence while the engine mailbox holds a
 * strength above zero. Under the transport's 250 ms one-shot on purpose --
 * a healthy channel refreshes before the previous pulse expires, a broken
 * one goes silent within 250 ms. */
constexpr uint64_t kRumbleRefreshMs = 200u;
/* F6 auto-rotate: how recent the UI's last noteInviteDisplayed must be for
 * the invite card to count as ON SCREEN right now. The UI reports once per
 * drawn frame, so anything past a second means the card left the screen
 * and the invite must simply expire at its ordinary TTL. */
constexpr uint64_t kInviteDisplayedFreshMs = 1000u;

bool printable(const std::string &value, size_t maximum) {
    if (value.size() > maximum) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char byte) {
        return byte >= 0x20u && byte != 0x7fu;
    });
}

bool validPublicKey(const std::string &value) {
    return value.size() == kPublicKeyLength &&
        std::all_of(value.begin(), value.end(), [](unsigned char byte) {
            return std::isalnum(byte) != 0 || byte == '-' || byte == '_';
        });
}

uint64_t ownerFor(const MdkrNativePartyController &controller) {
    if (controller.seat < 1u || controller.seat > 4u ||
        controller.leaseGeneration == 0u) {
        return 0u;
    }
    /* Same stable derivation as the browser bridge, widened before shifting. */
    const uint64_t owner =
        (static_cast<uint64_t>(controller.leaseGeneration) << 3u) |
        static_cast<uint64_t>(controller.seat);
    return owner == 0u ? 1u : owner;
}

bool occupiesSeat(const MdkrNativePartyController &controller) {
    return controller.phase != MdkrNativePartyControllerPhase::Pending &&
        controller.seat >= 1u && controller.seat <= 4u;
}

std::string safeMessage(const std::string &value, const char *fallback) {
    return printable(value, kMaxMessage) && !value.empty() ? value : fallback;
}

/* F8: whether any controller currently holds a seat lease. The terminal /
 * recoverable fork for transport errors hangs on this: a room with no
 * seated lease has nothing to protect and may fail closed, while a room
 * with one must never trade a live direct channel for an error screen. */
bool anySeatHeld(const MdkrNativePartyView &view) {
    return std::any_of(view.controllers.begin(), view.controllers.end(),
        [](const MdkrNativePartyController &candidate) {
            return candidate.phase != MdkrNativePartyControllerPhase::Pending &&
                candidate.seat >= 1u && candidate.seat <= 4u;
        });
}

/* F1 rename validation: the same bounds the redeem-time name field already
 * enforces (services/party/src/security.ts normalizeName and its native
 * twin in lan_party_room.cpp) -- 24 code points, none of the control /
 * zero-width / bidi-formatting set, no edge whitespace, kMaxName bytes --
 * applied as REFUSAL rather than repair. The phone page normalizes before
 * sending, so anything that arrives outside these bounds is a client this
 * host does not trust to relabel a seat row. Empty is valid: the name field
 * is optional and a rename may clear it. */
bool validRenameName(const std::string &value) {
    if (value.size() > kMaxName) return false;
    std::vector<uint32_t> codePoints;
    for (size_t index = 0u; index < value.size();) {
        const auto byte = static_cast<unsigned char>(value[index]);
        uint32_t code = 0u;
        size_t length = 0u;
        if (byte < 0x80u) {
            code = byte;
            length = 1u;
        } else if ((byte & 0xe0u) == 0xc0u) {
            code = byte & 0x1fu;
            length = 2u;
        } else if ((byte & 0xf0u) == 0xe0u) {
            code = byte & 0x0fu;
            length = 3u;
        } else if ((byte & 0xf8u) == 0xf0u) {
            code = byte & 0x07u;
            length = 4u;
        } else {
            return false;
        }
        if (index + length > value.size()) return false;
        for (size_t offset = 1u; offset < length; offset++) {
            const auto continuation =
                static_cast<unsigned char>(value[index + offset]);
            if ((continuation & 0xc0u) != 0x80u) return false;
            code = (code << 6u) | (continuation & 0x3fu);
        }
        if ((length == 2u && code < 0x80u) ||
            (length == 3u && code < 0x800u) ||
            (length == 4u && code < 0x10000u) || code > 0x10ffffu ||
            (code >= 0xd800u && code <= 0xdfffu)) {
            return false;
        }
        codePoints.push_back(code);
        index += length;
    }
    if (codePoints.size() > 24u) return false;
    const auto stripped = [](uint32_t code) {
        return code <= 0x1fu || (code >= 0x7fu && code <= 0x9fu) ||
            (code >= 0x200bu && code <= 0x200fu) || code == 0x2028u ||
            code == 0x2029u || (code >= 0x202au && code <= 0x202eu) ||
            code == 0x2060u || (code >= 0x2066u && code <= 0x2069u) ||
            code == 0xfeffu;
    };
    const auto spaceLike = [](uint32_t code) {
        return code == 0x20u || code == 0xa0u || code == 0x1680u ||
            (code >= 0x2000u && code <= 0x200au) || code == 0x202fu ||
            code == 0x205fu || code == 0x3000u;
    };
    for (uint32_t code : codePoints) {
        if (stripped(code)) return false;
    }
    if (!codePoints.empty() &&
        (spaceLike(codePoints.front()) || spaceLike(codePoints.back()))) {
        return false;
    }
    return true;
}

/* I3: the worker's typed host_command_result codes, mapped to honest
 * player-facing copy (services/party/src/party-room.ts commandError and
 * room-model.ts supply the codes). An unmapped code returns nullptr and the
 * caller keeps the generic copy exactly as before -- a future code can
 * never crash the launcher or lie to the player, it is just not yet
 * specific. */
const char *typedCommandErrorCopy(const std::string &code) {
    if (code == "service_budget_safe") {
        return "The controller service has reached today's limit. "
               "Try again after midnight UTC.";
    }
    if (code == "invite_rotated") {
        return "That invite was replaced. Use the newest code.";
    }
    if (code == "room_full") return "No free phone slot.";
    return nullptr;
}

}  // namespace

MdkrNativePartyHost::MdkrNativePartyHost(MdkrPartyTransport &transport)
    : transport_(transport) {}

MdkrNativePartyHost::~MdkrNativePartyHost() {
    /* M4: quitting the app with a live room must tell the phones goodbye.
     * The transport's closeRoom sends the worker the `close` command --
     * relayed to every controller as host_closed
     * (services/party/src/party-room.ts) -- and flushes it, bounded by
     * kMdkrPartyCloseFlushDeadlineMs, before the shutdown below hangs up,
     * so the phones read "the host ended the session" instead of misreading
     * a silent socket drop as a network fault. Closed and the terminal
     * phases are excluded: their transports are already shut down
     * (closeRoom / setTerminal), so there is no live socket to say goodbye
     * on and no room left listening for one. */
    if (view_.phase != MdkrNativePartyPhase::Closed &&
        view_.phase != MdkrNativePartyPhase::Error &&
        view_.phase != MdkrNativePartyPhase::RoomEnded) {
        (void)transport_.closeRoom();
    }
    releaseAll();
    transport_.shutdown();
}

bool MdkrNativePartyHost::open(const std::string &serviceOrigin) {
    /* I4: RoomEnded joins Closed and Error as a from-scratch start. The
     * ended room's copy names exactly this call as the way forward; the
     * fresh open creates a brand-new room, never resumes the dead one. */
    if (view_.phase != MdkrNativePartyPhase::Closed &&
        view_.phase != MdkrNativePartyPhase::Error &&
        view_.phase != MdkrNativePartyPhase::RoomEnded) {
        return false;
    }
    releaseAll();
    view_ = MdkrNativePartyView{};
    inviteDisplayedAtMs_ = 0u;
    inviteTtlMs_ = 0u;
    if (!transport_.available()) {
        setError(safeMessage(
            transport_.unavailableReason(),
            "Phone controllers are not included in this build."));
        return false;
    }
    /* M2: not merely "starts with https://" -- the compiled origin must be
     * the one canonical scheme+host[:port] shape. A value carrying a path,
     * query, fragment, userinfo or trailing slash is a misconfigured build
     * and fails closed here, before any transport bootstrap. The
     * canonical-origin gate is the CLOUD arm's; a LAN transport
     * (requiresSecureOrigin()==false) legitimately bypasses it because its
     * origin is an in-process http://<lan-ip>:<port>, not the compiled Party
     * service. The size cap and the transport's own open() still apply. */
    if (serviceOrigin.size() > kMaxUrl) {
        setError("Phone controllers require the configured secure Party service.");
        return false;
    }
    if (transport_.requiresSecureOrigin() &&
        !mdkr_party_canonical_https_origin(serviceOrigin) &&
        !mdkr_party_loopback_test_url_allowed(serviceOrigin)) {
        setError("Phone controllers require the configured secure Party service.");
        return false;
    }
    if (!transport_.open(serviceOrigin)) {
        setError("Could not open Phone Party. Local controllers still work.");
        return false;
    }
    view_.phase = MdkrNativePartyPhase::Opening;
    view_.busy = true;
    view_.message = "Creating a private controller room…";
    return true;
}

MdkrNativePartyController *MdkrNativePartyHost::controller(
    const std::string &id) {
    const auto found = std::find_if(
        view_.controllers.begin(), view_.controllers.end(),
        [&id](const MdkrNativePartyController &candidate) {
            return candidate.id == id;
        });
    return found == view_.controllers.end() ? nullptr : &*found;
}

const MdkrNativePartyController *MdkrNativePartyHost::controller(
    const std::string &id) const {
    const auto found = std::find_if(
        view_.controllers.begin(), view_.controllers.end(),
        [&id](const MdkrNativePartyController &candidate) {
            return candidate.id == id;
        });
    return found == view_.controllers.end() ? nullptr : &*found;
}

bool MdkrNativePartyHost::approve(
    const std::string &controllerId, unsigned seat) {
    /* SAS v2: no phrase gate here. The phrase commits to both DTLS
     * fingerprints, so it cannot exist until after approval starts the
     * WebRTC exchange -- it arrives as a ControllerPhrase event once the
     * phone connects, and the player verifies it on the seat row then. */
    MdkrNativePartyController *candidate = controller(controllerId);
    if (candidate == nullptr || candidate->commandPending ||
        candidate->phase != MdkrNativePartyControllerPhase::Pending ||
        seat < 1u || seat > 4u) {
        return false;
    }
    const bool occupied = std::any_of(
        view_.controllers.begin(), view_.controllers.end(),
        [seat](const MdkrNativePartyController &other) {
            return occupiesSeat(other) && other.seat == seat;
        });
    if (occupied || !transport_.approve(controllerId, seat)) return false;
    candidate->commandPending = true;
    view_.message = "Approving this phone…";
    return true;
}

bool MdkrNativePartyHost::confirmPairing(const std::string &controllerId) {
    /* P2.1 compare-then-trust: approval brought up a provisional connection
     * whose phrase is now on both screens; this is the human's Words Match.
     * Only a phone that holds a seat AND has a channel-bound phrase to compare
     * can be confirmed -- a phone that never connected, or whose channel was
     * refused (no phrase) or dropped (phrase cleared), offers nothing to
     * match, so Words Match is refused there. Idempotent: an already-confirmed
     * seat is left exactly as it is. Words Differ is reject()/remove, so a
     * mismatch is never routed through this method. */
    MdkrNativePartyController *candidate = controller(controllerId);
    if (candidate == nullptr || candidate->commandPending ||
        candidate->confirmed || !occupiesSeat(*candidate) ||
        candidate->pairingPhrase.empty()) {
        return false;
    }
    candidate->confirmed = true;
    /* Seat custody begins HERE, not at approval: bind the ingress now so the
     * next pad packet lands on a live seat. A fresh bind clears the ingress
     * haptics bit, so reassert the phone's known capability when the channel
     * is already live (the same call ControllerConnected makes). */
    const uint64_t owner = ownerFor(*candidate);
    if (owner != 0u && candidate->connectionSequence != 0u) {
        if (!mdkr_native_remote_pad_bind(
                candidate->seat - 1u, owner, candidate->connectionSequence)) {
            candidate->confirmed = false;
            setError("A phone controller could not reserve its seat safely.");
            return false;
        }
        if (candidate->direct) {
            (void)mdkr_native_remote_pad_set_haptics(
                candidate->seat - 1u, owner, candidate->connectionSequence,
                candidate->haptics);
        }
    }
    /* Tell the phone it is trusted so it leaves the compare screen and runs
     * the auto input test. Best-effort: the seat custody above already holds
     * regardless of whether the message reaches the phone this instant (the
     * transport re-sends it when the control channel opens). */
    (void)transport_.confirm(controllerId);
    view_.message = "Phone confirmed. Its controls are live.";
    return true;
}

bool MdkrNativePartyHost::reject(const std::string &controllerId) {
    MdkrNativePartyController *candidate = controller(controllerId);
    if (candidate == nullptr || candidate->commandPending ||
        !(candidate->phase == MdkrNativePartyControllerPhase::Pending
            ? transport_.reject(controllerId)
            : transport_.remove(controllerId))) {
        return false;
    }
    candidate->commandPending = true;
    view_.message = "Removing this phone…";
    return true;
}

bool MdkrNativePartyHost::rotateInvite() {
    if ((view_.phase != MdkrNativePartyPhase::Open &&
         view_.phase != MdkrNativePartyPhase::InviteRevoked) || view_.busy ||
        view_.inviteGeneration == 0u ||
        !transport_.rotateInvite(view_.inviteGeneration)) {
        return false;
    }
    view_.busy = true;
    view_.message = "Making a fresh controller code…";
    return true;
}

void MdkrNativePartyHost::noteInviteDisplayed(uint64_t nowMs) {
    inviteDisplayedAtMs_ = nowMs;
}

bool MdkrNativePartyHost::dismissInvite() {
    if (!view_.inviteVisible || view_.busy || !transport_.revokeInvite()) {
        return false;
    }
    view_.busy = true;
    view_.message = "Closing this invite…";
    return true;
}

bool MdkrNativePartyHost::closeRoom() {
    if (view_.phase == MdkrNativePartyPhase::Closed) return true;
    const bool requested = transport_.closeRoom();
    transport_.shutdown();
    releaseAll();
    view_ = MdkrNativePartyView{};
    inviteDisplayedAtMs_ = 0u;
    inviteTtlMs_ = 0u;
    view_.message = "Phone controllers closed.";
    return requested;
}

bool MdkrNativePartyHost::roomStateValid(
    const MdkrPartyTransportRoomState &room) const {
    /* The invite URL's scheme trust model follows the same transport-selection
     * seam as open(): the CLOUD arm demands https (or the token-gated loopback
     * test origin); a LAN transport advertises a plain http://<lan-ip>:<port>
     * the phones reach with no internet, so it is validated as a well-formed
     * http(s) URL rather than a secure one. Everything else -- the printable
     * caps, the six-digit code, the roster shape -- is identical either way. */
    const bool secureOrigin = transport_.requiresSecureOrigin();
    const bool inviteUrlUntrusted = secureOrigin
        ? (room.controllerUrl.rfind("https://", 0u) != 0u &&
           !mdkr_party_loopback_test_url_allowed(room.controllerUrl))
        : (room.controllerUrl.rfind("http://", 0u) != 0u &&
           room.controllerUrl.rfind("https://", 0u) != 0u);
    if (room.transitionId == 0u || room.inviteGeneration == 0u ||
        room.controllers.size() > kMaxControllers ||
        !printable(room.controllerUrl, kMaxUrl) ||
        !printable(room.fallbackCode, 12u) ||
        (room.inviteActive &&
         (inviteUrlUntrusted ||
          room.fallbackCode.size() != 6u ||
          !std::all_of(room.fallbackCode.begin(), room.fallbackCode.end(),
                       [](unsigned char byte) { return std::isdigit(byte) != 0; })))) {
        return false;
    }
    std::set<std::string> ids;
    std::array<bool, 4> seats{};
    for (const MdkrNativePartyController &candidate : room.controllers) {
        if (candidate.id.empty() || !printable(candidate.id, kMaxControllerId) ||
            !printable(candidate.name, kMaxName) ||
            !validPublicKey(candidate.publicKey) ||
            !printable(candidate.pairingPhrase, kMaxPhrase)) {
            return false;
        }
        if (!ids.insert(candidate.id).second) return false;
        if (candidate.phase == MdkrNativePartyControllerPhase::Pending) {
            if (candidate.seat != 0u || candidate.leaseGeneration != 0u ||
                candidate.connectionSequence == 0u || candidate.direct) {
                return false;
            }
            continue;
        }
        if (candidate.seat < 1u || candidate.seat > 4u ||
            candidate.leaseGeneration == 0u ||
            candidate.connectionSequence == 0u ||
            seats[candidate.seat - 1u]) {
            return false;
        }
        seats[candidate.seat - 1u] = true;
        if (candidate.direct &&
            candidate.phase != MdkrNativePartyControllerPhase::Connected) {
            return false;
        }
    }
    return true;
}

void MdkrNativePartyHost::releaseController(
    const MdkrNativePartyController &candidate) {
    const uint64_t owner = ownerFor(candidate);
    if (owner != 0u && candidate.connectionSequence != 0u) {
        (void)mdkr_native_remote_pad_release(
            candidate.seat - 1u, owner, candidate.connectionSequence);
    }
}

void MdkrNativePartyHost::releaseAll() {
    for (const MdkrNativePartyController &candidate : view_.controllers) {
        releaseController(candidate);
    }
    lastRumbleSentMs_.fill(0u);
}

void MdkrNativePartyHost::applyRoomState(
    const MdkrPartyTransportRoomState &room, uint64_t nowMs) {
    if (!roomStateValid(room)) {
        /* F8: an invalid update is refused either way, but only a room with
         * no seated lease may fail closed over it. Seated leases ride out
         * the fault in Recovering -- their direct channels never depended
         * on this update, and the next valid room update recovers the
         * surface (applyRoomState below runs on Recovering explicitly). */
        if (anySeatHeld(view_)) {
            view_.phase = MdkrNativePartyPhase::Recovering;
            view_.busy = true;
            view_.message =
                "The controller service sent an invalid room update. "
                "Connected phones keep working.";
            return;
        }
        setError("The controller service returned an invalid room update.");
        return;
    }
    if (room.transitionId < view_.transitionId) return;
    if (room.transitionId == view_.transitionId &&
        view_.phase != MdkrNativePartyPhase::Opening &&
        view_.phase != MdkrNativePartyPhase::Recovering) {
        return;
    }

    for (const MdkrNativePartyController &existing : view_.controllers) {
        const auto replacement = std::find_if(
            room.controllers.begin(), room.controllers.end(),
            [&existing](const MdkrNativePartyController &candidate) {
                return candidate.id == existing.id;
            });
        if (replacement == room.controllers.end() ||
            ownerFor(*replacement) != ownerFor(existing) ||
            replacement->connectionSequence != existing.connectionSequence) {
            releaseController(existing);
        }
    }

    /* SAS v2: room updates never carry a phrase -- it arrives on its own
     * ControllerPhrase event once the phone's WebRTC descriptions are set.
     * Carry a seat's verified phrase across a room transition only while
     * the update still describes the same channel: same phone, same key,
     * same connectionSequence. A new sequence is a new channel whose phrase
     * has not arrived yet, and the old words must not vouch for it. */
    const std::vector<MdkrNativePartyController> previous =
        std::move(view_.controllers);
    view_.controllers = room.controllers;
    for (MdkrNativePartyController &candidate : view_.controllers) {
        candidate.commandPending = false;
        const auto former = std::find_if(
            previous.begin(), previous.end(),
            [&candidate](const MdkrNativePartyController &other) {
                return other.id == candidate.id;
            });
        if (former != previous.end() &&
            former->publicKey == candidate.publicKey) {
            if (candidate.pairingPhrase.empty() &&
                former->connectionSequence == candidate.connectionSequence) {
                candidate.pairingPhrase = former->pairingPhrase;
            }
            /* RTT rides the same rule as the phrase: it measured this exact
             * channel, so a new connectionSequence starts sampleless. */
            if (former->connectionSequence == candidate.connectionSequence) {
                candidate.rttMs = former->rttMs;
            }
            /* F2: connection history belongs to the phone (id + key), not
             * to any one room transition -- carry it. */
            candidate.everConnected = candidate.everConnected ||
                former->everConnected;
            /* P2.1: seat custody, once the human granted it (Words Match),
             * belongs to the phone (id + key) too -- carry it so a trusted
             * phone auto-resumes its seat on reconnect with no re-compare,
             * exactly as everConnected rides across room updates. The wire
             * never carries confirmed, so a different phone reusing an id
             * (key changed) does not inherit it: this block only runs when
             * the former entry's key matches. */
            candidate.confirmed = candidate.confirmed || former->confirmed;
            /* F1: the room update still carries the redeem-time name; a
             * live rename outlives it while this is the same phone. */
            if (former->renamed && occupiesSeat(candidate)) {
                candidate.name = former->name;
                candidate.renamed = true;
            }
        }
        if (candidate.phase == MdkrNativePartyControllerPhase::Connected) {
            candidate.everConnected = true;
        }
        /* P2.1: bind a seat's ingress only once the human confirmed it.
         * Before Words Match a seated phone is a provisional connection --
         * the room reserves the seat number, but no ingress custody exists,
         * so its pad packets are discarded at the boundary (fail-neutral).
         * A confirmed phone re-binds here on every room transition, which is
         * exactly how a trusted phone auto-resumes its seat across a
         * reconnect (confirmed rode across the update above). */
        const uint64_t owner = ownerFor(candidate);
        if (candidate.confirmed && owner != 0u &&
            candidate.connectionSequence != 0u) {
            if (!mdkr_native_remote_pad_bind(
                    candidate.seat - 1u, owner,
                    candidate.connectionSequence)) {
                setError("A phone controller could not reserve its seat safely.");
                return;
            }
        }
    }

    /* F6: latch the generation's ORIGINAL TTL once, at the transition that
     * introduces the generation -- the 75% auto-rotate mark is measured
     * against it, and a later same-generation update (which arrives with
     * only the remaining time) must not shrink the baseline. */
    if (room.inviteGeneration != view_.inviteGeneration) {
        inviteTtlMs_ = room.inviteActive ? room.inviteExpiresInMs : 0u;
    }
    view_.transitionId = room.transitionId;
    view_.inviteGeneration = room.inviteGeneration;
    /* I1 fix: room.inviteExpiresInMs is relative (ms remaining as of the
     * transport's parse), specifically so it can be anchored here in the
     * HOST's own service clock (nowMs) rather than compared as if it were
     * already an absolute instant in some other clock's domain. Every
     * downstream read of view_.inviteExpiresAtMs (the tail of service()
     * below, ui_phone_party.cpp's countdown) already compares it against
     * this same nowMs domain, so latching it here is the entire fix. */
    view_.inviteExpiresAtMs = nowMs + room.inviteExpiresInMs;
    view_.controllerUrl = room.inviteActive ? room.controllerUrl : std::string{};
    view_.fallbackCode = room.inviteActive ? room.fallbackCode : std::string{};
    view_.inviteVisible = room.inviteActive && view_.inviteExpiresAtMs > nowMs;
    view_.phase = view_.inviteVisible ? MdkrNativePartyPhase::Open
                                     : MdkrNativePartyPhase::InviteRevoked;
    view_.busy = false;
    view_.message = view_.inviteVisible
        ? "Scan to add a phone controller."
        : "The invite is closed. Connected phones keep their seats.";
}

void MdkrNativePartyHost::applyEvent(
    const MdkrPartyTransportEvent &event, uint64_t nowMs) {
    switch (event.type) {
        case MdkrPartyTransportEventType::RoomState:
            applyRoomState(event.room, nowMs);
            return;
        case MdkrPartyTransportEventType::ControllerConnected: {
            MdkrNativePartyController *candidate = controller(event.controllerId);
            if (candidate == nullptr || !occupiesSeat(*candidate)) return;
            candidate->phase = MdkrNativePartyControllerPhase::Connected;
            candidate->direct = true;
            candidate->haptics = event.haptics;
            /* F2: this lease has now connected once; a later drop reads as
             * "Reconnecting", never again as first-time "Connecting…". */
            candidate->everConnected = true;
            /* I2 recovery: a genuine controller_ready at this build's own
             * protocol means the phone reloaded into a matching page. */
            candidate->protocolMismatch = false;
            /* P2.1: only a confirmed seat holds an ingress binding to carry
             * the haptics bit -- a provisional seat has none, so touching it
             * here would only stale-count. confirmPairing() re-asserts haptics
             * when the human grants the seat. */
            if (candidate->confirmed) {
                (void)mdkr_native_remote_pad_set_haptics(
                    candidate->seat - 1u, ownerFor(*candidate),
                    candidate->connectionSequence, event.haptics);
            }
            view_.message = "Phone connected directly.";
            return;
        }
        case MdkrPartyTransportEventType::ControllerDisconnected: {
            MdkrNativePartyController *candidate = controller(event.controllerId);
            if (candidate == nullptr || !occupiesSeat(*candidate)) return;
            candidate->direct = false;
            candidate->haptics = false;
            /* SAS v2: the phrase vouched for the channel that just ended,
             * so it ends here too. A legitimate reconnect re-derives fresh
             * words from its own answer; a reconnect whose fingerprints the
             * transport REFUSES derives none -- and without this clear that
             * refusal would leave the old channel's words standing
             * indefinitely against a live channel they do not bind. */
            candidate->pairingPhrase.clear();
            /* The RTT sample measured the channel that just ended. */
            candidate->rttMs = 0u;
            /* P2.1: only a confirmed seat has an ingress binding to neutralize. */
            if (candidate->confirmed) {
                (void)mdkr_native_remote_pad_set_haptics(
                    candidate->seat - 1u, ownerFor(*candidate),
                    candidate->connectionSequence, false);
            }
            if (candidate->phase == MdkrNativePartyControllerPhase::Connected) {
                candidate->phase = MdkrNativePartyControllerPhase::Leased;
            }
            view_.message = "Phone reconnecting; its controls are neutral.";
            return;
        }
        case MdkrPartyTransportEventType::ControllerPacket: {
            MdkrNativePartyController *candidate = controller(event.controllerId);
            /* P2.1 ingress discard: a provisional (unconfirmed) phone moves
             * nothing. Its seat has no ingress binding until Words Match, so
             * even attempting the push would only fail sameBinding and demote
             * a healthy connection -- discard here instead, at the ingress
             * boundary, exactly as the hard constraint requires. */
            if (candidate == nullptr || !candidate->confirmed ||
                !candidate->direct ||
                candidate->phase != MdkrNativePartyControllerPhase::Connected) {
                return;
            }
            const uint64_t owner = ownerFor(*candidate);
            if (!mdkr_native_remote_pad_push(
                    candidate->seat - 1u, owner,
                    candidate->connectionSequence,
                    event.packet.data(), event.packet.size())) {
                candidate->direct = false;
                candidate->phase = MdkrNativePartyControllerPhase::Leased;
                /* Queue exhaustion revoked custody at the ingress crossing,
                 * but nothing here waits for a room transition to fix it --
                 * a stable mid-race room never sends one. Flag it so
                 * service() heals the seat on its own next tick. Leave
                 * candidate->haptics untouched: it is the phone's known
                 * hardware capability, not a liveness bit, and the healing
                 * loop needs it to re-assert ingress haptics support once the
                 * seat is rebound (a fresh bind always clears that bit). */
                candidate->needsRebind = true;
                view_.message =
                    "Phone input paused safely. Reconnecting…";
            }
            return;
        }
        case MdkrPartyTransportEventType::ControllerRtt: {
            /* RTT: newest matched pong wins; the sample is cosmetic and
             * scoped to the live channel (cleared wherever that channel
             * ends or demotes). */
            MdkrNativePartyController *candidate = controller(event.controllerId);
            if (candidate == nullptr || !occupiesSeat(*candidate)) return;
            candidate->rttMs = event.rttMs;
            return;
        }
        case MdkrPartyTransportEventType::ControllerPhrase: {
            MdkrNativePartyController *candidate = controller(event.controllerId);
            if (candidate != nullptr && printable(event.message, kMaxPhrase)) {
                candidate->pairingPhrase = event.message;
            }
            return;
        }
        case MdkrPartyTransportEventType::ControllerRenamed: {
            /* F1: a rename may only come from a seat's own authenticated
             * control channel (the transports enforce that side), may only
             * name a seated controller, and must pass the same bounds the
             * redeem-time name field enforces -- otherwise it is dropped
             * whole. The row updates silently; nothing else changes. */
            MdkrNativePartyController *candidate = controller(event.controllerId);
            if (candidate == nullptr || !occupiesSeat(*candidate) ||
                !validRenameName(event.message)) {
                return;
            }
            candidate->name = event.message;
            candidate->renamed = true;
            return;
        }
        case MdkrPartyTransportEventType::ControllerProtocolMismatch: {
            MdkrNativePartyController *candidate = controller(event.controllerId);
            if (candidate == nullptr || !occupiesSeat(*candidate)) return;
            /* I2: the phone's page completed the handshake but speaks a
             * different channel-protocol version. Say so, visibly distinct
             * from "Reconnecting", and demote the seat to neutral input.
             * The room and the seat's lease stay intact -- the transport
             * kept the peer up, and the phone reloading into a matching
             * page version (ControllerConnected) is the recovery. Clear
             * needsRebind: the C1 heal loop's fresh bind cannot fix a
             * version gap, and its "Phone input reconnected." copy would
             * paper over this honest state with a lie. */
            candidate->protocolMismatch = true;
            candidate->needsRebind = false;
            candidate->direct = false;
            candidate->haptics = false;
            /* No RTT number may vouch for a demoted channel. */
            candidate->rttMs = 0u;
            /* P2.1: only a confirmed seat has an ingress binding to neutralize. */
            if (candidate->confirmed) {
                (void)mdkr_native_remote_pad_set_haptics(
                    candidate->seat - 1u, ownerFor(*candidate),
                    candidate->connectionSequence, false);
            }
            if (candidate->phase == MdkrNativePartyControllerPhase::Connected) {
                candidate->phase = MdkrNativePartyControllerPhase::Leased;
            }
            view_.message = kMdkrPartyProtocolMismatchCopy;
            return;
        }
        case MdkrPartyTransportEventType::CommandRejected: {
            if (event.controllerId.empty() &&
                (event.command == "rotate" || event.command == "revoke" ||
                 event.command == "close")) {
                /* Residual CLOSED: the worker echoes the failed command's
                 * identity on every host_command_result failure it emits
                 * (party-room.ts commandIdentity), so a room-level command
                 * failure -- rotate/dismiss/close, named by the echo but
                 * naming no controller because it never targeted one --
                 * clears only the room-level busy flag that was waiting on
                 * it. No controller's genuinely in-flight command is
                 * touched any more. */
                view_.busy = false;
            } else if (event.controllerId.empty()) {
                /* No identity at all: a sender older than the identity
                 * echo (or not the worker). Keep the pre-echo conservative
                 * room-wide clear so a stale service can still never leave
                 * commandPending or busy wedged forever. */
                for (MdkrNativePartyController &candidate : view_.controllers) {
                    candidate.commandPending = false;
                }
                view_.busy = false;
            } else {
                /* A controller-scoped rejection (C3's connect_timeout
                 * give-up) must not clear an unrelated controller's
                 * genuinely in-flight command -- only the named
                 * controller's own commandPending is touched. busy tracks
                 * room-level invite actions, not any one controller, so it
                 * is left untouched here. */
                MdkrNativePartyController *candidate = controller(event.controllerId);
                if (candidate != nullptr) {
                    candidate->commandPending = false;
                    /* I2 review fix: a version-mismatched seat keeps the
                     * honest room copy. The transport's ladder no longer
                     * gives up on such a peer at the source, but any
                     * rejection that still names a mismatched controller
                     * -- the give-up shape included -- would recommend
                     * the wrong remedy ("remove it and pair again" cannot
                     * fix a page version) right beside the seat row's
                     * correct one. Bookkeeping above still happened; only
                     * the message overwrite is suppressed. */
                    if (candidate->protocolMismatch) return;
                }
            }
            /* I3: a recognized typed code wins over whatever prose the
             * transport attached; an unknown or absent code keeps the
             * pre-existing generic surface untouched. */
            const char *typedCopy = typedCommandErrorCopy(event.errorCode);
            view_.message = typedCopy != nullptr ? typedCopy : safeMessage(
                event.message, "That controller action did not complete. Try again.");
            return;
        }
        case MdkrPartyTransportEventType::Recovering:
            view_.phase = MdkrNativePartyPhase::Recovering;
            view_.busy = true;
            view_.message = "Reconnecting the controller room…";
            return;
        case MdkrPartyTransportEventType::Error:
            /* F8: only credential-invalid and room-closed are terminal, and
             * on this model both arrive typed (RoomGone below; Closed for
             * the host's own goodbye). Any other transport error while a
             * seat holds its lease keeps the lease and shows recovery --
             * the phones' direct channels do not depend on the faulted
             * signaling path, so tearing their seats down would trade live
             * controls for an error screen. With no seat held there is
             * nothing to protect and the error stays terminal, keeping the
             * retry button honest. */
            if (anySeatHeld(view_)) {
                view_.phase = MdkrNativePartyPhase::Recovering;
                view_.busy = true;
                view_.message = safeMessage(
                    event.message,
                    "Phone controller connection hit a fault. "
                    "Connected phones keep working.");
                return;
            }
            setError(safeMessage(
                event.message,
                "Phone controllers are unavailable. Local controllers still work."));
            return;
        case MdkrPartyTransportEventType::RoomGone:
            /* I4: the service refused to resume this room for good -- its
             * 24 h life ended or it was deleted outright; no ladder rung
             * can bring it back. Same fail-neutral teardown as a terminal
             * error, but a distinct phase and sentence: nothing broke, the
             * session is simply over, and a NEW invite (a fresh open())
             * is the one honest way forward. The copy is the host's own,
             * not the event's: this surface must never depend on prose
             * arriving over the wire. */
            setTerminal(MdkrNativePartyPhase::RoomEnded,
                        kMdkrPartyRoomEndedCopy);
            return;
        case MdkrPartyTransportEventType::Closed:
            releaseAll();
            view_ = MdkrNativePartyView{};
            inviteDisplayedAtMs_ = 0u;
            inviteTtlMs_ = 0u;
            view_.message = safeMessage(event.message, "Phone controllers closed.");
            return;
    }
}

void MdkrNativePartyHost::setError(const std::string &message) {
    setTerminal(MdkrNativePartyPhase::Error, message);
}

/* One teardown for every terminal room state (Error, and I4's RoomEnded):
 * seats go back to local play, the transport is shut down and stays down,
 * and the surface shows exactly one sentence for the state it is in. */
void MdkrNativePartyHost::setTerminal(
    MdkrNativePartyPhase phase, const std::string &message) {
    releaseAll();
    transport_.shutdown();
    /* Review fix: a controller left needsRebind from a push failure earlier
     * in this same drain cycle must not survive into the terminal room --
     * releaseAll() above already revoked its seat's custody, so the next
     * service() heal loop must never see a reason to touch it again. */
    for (MdkrNativePartyController &candidate : view_.controllers) {
        candidate.needsRebind = false;
    }
    view_.phase = phase;
    view_.busy = false;
    view_.inviteVisible = false;
    view_.controllerUrl.clear();
    view_.fallbackCode.clear();
    view_.message = message;
}

void MdkrNativePartyHost::service(uint64_t nowMs) {
    /* C1 self-heal. Ingress queue exhaustion revokes a seat's custody at the
     * transport crossing (native_remote_pad_ingress.cpp) below the room state
     * machine entirely -- the WebRTC channel and its 5 s pings never notice.
     * The only other rebind site is applyRoomState, which runs solely on an
     * advancing room transitionId; a stable mid-race room sends none, so
     * without this loop "Reconnecting..." is a promise nothing fulfills. Heal
     * it ourselves: re-issuing mdkr_native_remote_pad_bind with a fresh lease
     * generation gives the seat a new owner identity (the same call
     * applyRoomState makes for a real reconnect), and the engine-side rebind
     * that a changed identity triggers publishes neutral before any packet
     * from the new epoch can reach the sim -- the same fail-neutral path a
     * genuine reconnect already relies on. Runs before events are drained so
     * a fresh packet queued for this very tick lands on a live seat. A fresh
     * bind always clears the ingress haptics bit (native_remote_pad_ingress.cpp
     * clearPayload), and in this exact scenario the WebRTC channel never
     * dropped, so no ControllerConnected event will ever come along to set it
     * again -- reassert it ourselves from the controller's own remembered
     * capability, the same call ControllerConnected makes.
     *
     * Review fix: also gate the whole loop on the room not being terminal
     * (Error, I4's RoomEnded) or Closed. setTerminal() already clears
     * needsRebind on its way out (belt), but a terminal or closed room must
     * never re-bind a seat regardless of how a flag got left standing
     * (suspenders).
     */
    if (view_.phase != MdkrNativePartyPhase::Error &&
        view_.phase != MdkrNativePartyPhase::RoomEnded &&
        view_.phase != MdkrNativePartyPhase::Closed) {
        for (MdkrNativePartyController &candidate : view_.controllers) {
            /* I2: a version-mismatched seat is excluded outright. A fresh
             * bind cannot fix a protocol gap, and this loop's "Phone input
             * reconnected." would overwrite the honest mismatch copy.
             * applyEvent already clears needsRebind at the mismatch site
             * (belt); this keeps the loop safe regardless (suspenders). */
            /* P2.1: only a confirmed seat can have had ingress custody to
             * lose, so only a confirmed seat can be healed. needsRebind is
             * already set solely on a confirmed seat's push overflow, so this
             * is a belt beside that suspenders. */
            if (candidate.protocolMismatch || !candidate.needsRebind ||
                !candidate.confirmed || !occupiesSeat(candidate) ||
                candidate.connectionSequence == 0u) {
                continue;
            }
            if (candidate.lastRebindMs != 0u &&
                nowMs >= candidate.lastRebindMs &&
                nowMs - candidate.lastRebindMs < kRebindRateLimitMs) {
                continue;
            }
            candidate.lastRebindMs = nowMs;
            candidate.leaseGeneration++;
            const uint64_t owner = ownerFor(candidate);
            if (owner == 0u ||
                !mdkr_native_remote_pad_bind(
                    candidate.seat - 1u, owner, candidate.connectionSequence)) {
                continue;
            }
            candidate.needsRebind = false;
            candidate.direct = true;
            candidate.phase = MdkrNativePartyControllerPhase::Connected;
            (void)mdkr_native_remote_pad_set_haptics(
                candidate.seat - 1u, owner, candidate.connectionSequence,
                candidate.haptics);
            view_.message = "Phone input reconnected.";
        }
    }

    MdkrPartyTransportEvent event;
    size_t count = 0u;
    while (count < kMaxEventsPerService && transport_.poll(event)) {
        applyEvent(event, nowMs);
        count++;
    }
    if (view_.inviteVisible && nowMs >= view_.inviteExpiresAtMs) {
        view_.inviteVisible = false;
        view_.controllerUrl.clear();
        view_.fallbackCode.clear();
        view_.phase = MdkrNativePartyPhase::InviteRevoked;
        view_.message = "Controller code expired. Connected phones keep their seats.";
    }

    /* F6: an invite the player is actually LOOKING at right now (the UI
     * reported the card drawn within the last frame or so) rotates itself
     * once ~75% of its TTL has elapsed, so the QR/code on screen is always
     * redeemable. The TTL itself never lengthens: an undisplayed invite
     * just expired above exactly as it always did, and this path mints a
     * replacement rather than stretching the old one. rotateInvite() keeps
     * every one of its own gates (phase, busy, generation), so an in-flight
     * rotation is never doubled. */
    if (view_.inviteVisible && !view_.busy &&
        view_.phase == MdkrNativePartyPhase::Open &&
        inviteTtlMs_ != 0u && inviteDisplayedAtMs_ != 0u &&
        nowMs >= inviteDisplayedAtMs_ &&
        nowMs - inviteDisplayedAtMs_ <= kInviteDisplayedFreshMs &&
        view_.inviteExpiresAtMs > nowMs &&
        view_.inviteExpiresAtMs - nowMs <= inviteTtlMs_ / 4u) {
        (void)rotateInvite();
    }

    for (MdkrNativePartyController &candidate : view_.controllers) {
        /* P2.1: a provisional (unconfirmed) seat holds no ingress reservation,
         * so neither engine mailbox is bound; the confirmed gate keeps this
         * loop from even reaching for them. */
        if (!candidate.direct || !candidate.confirmed ||
            !occupiesSeat(candidate)) {
            continue;
        }
        const uint64_t owner = ownerFor(candidate);
        const unsigned port = candidate.seat - 1u;
        /* P2.2 in-race feedback reaches EVERY confirmed phone, haptics or not,
         * because it is visual. The ingress only flags a real change, so this
         * is change-driven (a few Hz) over the reliable control channel, never
         * per engine frame. */
        MdkrNativeRaceState raceState{};
        if (mdkr_native_remote_pad_take_race_state(
                port, owner, candidate.connectionSequence, &raceState)) {
            (void)transport_.sendRaceState(candidate.id, raceState);
        }
        /* Rumble is haptic: only a phone that advertised vibration gets it. */
        if (!candidate.haptics) {
            continue;
        }
        uint16_t strength = 0u;
        /* A fresh engine post always goes out immediately -- stops
         * (strength zero) included, which must never wait out a refresh
         * window. */
        bool send = mdkr_native_remote_pad_take_rumble(
            port, owner, candidate.connectionSequence, &strength);
        /* M5 sustained rumble: the engine posts CHANGES (its SDL path then
         * sustains the motor on a 60 s safety duration), but the phone's
         * rumble command is a deliberate 250 ms one-shot -- kept that way
         * so a lost refresh fails silent-off, never stuck-on. Sustaining is
         * therefore this loop's job: while the mailbox still holds a
         * strength above zero, re-send it every kRumbleRefreshMs -- inside
         * the one-shot's 250 ms, so a healthy channel never gaps. The
         * strength is re-read from the mailbox every time, never from host
         * memory, so a rebind or disconnect (both zero the mailbox) ends
         * the refreshes on its own; the direct/haptics gate above is the
         * same cutoff one layer up. */
        if (!send &&
            mdkr_native_remote_pad_peek_rumble(
                port, owner, candidate.connectionSequence, &strength) &&
            strength > 0u && nowMs >= lastRumbleSentMs_[port] &&
            nowMs - lastRumbleSentMs_[port] >= kRumbleRefreshMs) {
            send = true;
        }
        if (send && transport_.sendRumble(candidate.id, strength)) {
            lastRumbleSentMs_[port] = nowMs;
        }
    }
}
