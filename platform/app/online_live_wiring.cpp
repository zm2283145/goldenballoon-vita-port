/*
 * Native online BETA: launcher-side live-adapter wiring.
 *
 * Compiled ONLY under MDKR_ENABLE_ONLINE_BETA (the CMake beta gate; never in a
 * shipping build -- the option defaults OFF and release.yml/build_app_bundle.sh
 * never set it). Provides the real OnlineRoom_makeGatedLiveAdapter the Online
 * Room panel calls: it composes the production MatchRoom HTTP transport, the
 * real signal-client mesh backend and the O-T3 live adapter EXACTLY as the O-T6
 * e2e driver does (tests/test_online_live_transport_e2e_driver.cpp), and returns
 * an owning wrapper that keeps the two borrowed transports alive for the
 * adapter's lifetime.
 *
 * Fences enforced here (v1, do not weaken):
 *   - 2 endpoints: localSeatCount = 1 (solo endpoint; couch-pair = 2 comes later
 *     and >=3 needs the full-mesh-rekey protocol that is NOT closed).
 *   - retail identities: every local roster slot is clamped to
 *     MDKR_MATCH_IDENTITY_RETAIL, feeding the O-T5 clamp inside the adapter.
 *   - STUN-only: a client-side belt drops any turn:/turns: ICE server the service
 *     delivers, so a mis-provisioned zone can never silently start using relay.
 *
 * The security-audited parsers/crypto/SAS live in the composed translation units
 * and are NOT touched here.
 */
#if MDKR_ENABLE_ONLINE_BETA

#include "online/match_live_adapter.h"
#include "online/match_live_transport.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

/* STUN-only belt: wrap the production room transport and drop any relay
 * (turn:/turns:) ICE server the service delivers on every drained event. The
 * mesh already skips credentialed non-turn entries; this makes the beta refuse
 * relay outright even if a zone is mis-provisioned. Every other call delegates
 * straight through. */
class StunOnlyRoomTransport final : public MdkrOnlineRoomTransport {
public:
    explicit StunOnlyRoomTransport(
        std::unique_ptr<MdkrOnlineRoomTransport> inner)
        : inner_(std::move(inner)) {}

    bool beginCreate(const MdkrOnlineCompatibilityV1 &compatibility,
                     unsigned seatCount) override {
        return inner_->beginCreate(compatibility, seatCount);
    }
    bool beginJoin(const std::string &capability,
                   const MdkrOnlineCompatibilityV1 &compatibility,
                   unsigned seatCount) override {
        return inner_->beginJoin(capability, compatibility, seatCount);
    }
    bool beginJoinByCode(const std::string &code,
                         const MdkrOnlineCompatibilityV1 &compatibility,
                         unsigned seatCount) override {
        return inner_->beginJoinByCode(code, compatibility, seatCount);
    }
    bool submitCommand(const MdkrOnlineCommand &command) override {
        return inner_->submitCommand(command);
    }
    void pump(std::vector<MdkrOnlineRoomEvent> &out) override {
        inner_->pump(out);
        for (MdkrOnlineRoomEvent &event : out) {
            event.iceServers.erase(
                std::remove_if(event.iceServers.begin(), event.iceServers.end(),
                               [](const MdkrMatchPeerIceServer &server) {
                                   return isRelay(server.url);
                               }),
                event.iceServers.end());
        }
    }
    void close() override { inner_->close(); }

    /* The wrapped production HTTP transport. mdkr_online_room_http_transport_invite
     * dynamic_casts to the concrete RoomHttpTransport, so the invite accessor
     * must reach past this belt to the real transport underneath. */
    MdkrOnlineRoomTransport *inner() const { return inner_.get(); }

private:
    static bool isRelay(const std::string &url) {
        std::string scheme = url.substr(0, url.find(':'));
        std::transform(scheme.begin(), scheme.end(), scheme.begin(),
                       [](unsigned char c) {
                           return static_cast<char>(std::tolower(c));
                       });
        return scheme == "turn" || scheme == "turns";
    }

    std::unique_ptr<MdkrOnlineRoomTransport> inner_;
};

/* Owning wrapper. MdkrOnlineLiveAdapterOptions.room and .meshBackend are
 * borrowed and must outlive the adapter, so the launcher-facing adapter owns all
 * three. Member declaration order makes inner_ destruct FIRST (it borrows the
 * other two), then mesh_, then room_. Every IMdkrOnlineAdapter call delegates to
 * inner_; the panel drives only this narrow seam. */
class OwningLiveAdapter final : public IMdkrOnlineAdapter {
public:
    OwningLiveAdapter(std::unique_ptr<MdkrOnlineRoomTransport> room,
                      std::unique_ptr<MdkrOnlineMeshSignalBackend> mesh)
        : room_(std::move(room)), mesh_(std::move(mesh)) {}

    bool build(const MdkrOnlineLiveAdapterOptions &options, std::string *error) {
        inner_ = mdkr_online_live_adapter_create(options, error);
        return inner_ != nullptr;
    }

    MdkrOnlineAdapterStep submit(
        const MdkrOnlineAdapterCommand &command) override {
        return inner_->submit(command);
    }
    bool view(MdkrOnlineViewModel *out) const override {
        return inner_->view(out);
    }
    MdkrOnlineJourney journey() const override { return inner_->journey(); }
    void service() override { inner_->service(); }
    uint32_t revision() const override { return inner_->revision(); }
    MdkrPlayIntent sessionIntent() const override {
        return inner_->sessionIntent();
    }
    bool raceAdmissionEnabled() const override {
        return inner_->raceAdmissionEnabled();
    }
    bool timeoutExpired() const override { return inner_->timeoutExpired(); }
    MdkrOnlineFakeAdapter *fakeAdapter() override {
        return inner_->fakeAdapter();
    }

    /* The concrete HTTP transport behind the STUN-only belt, for the invite
     * accessor. Returns the room transport directly if no belt is present. */
    MdkrOnlineRoomTransport *httpTransportForInvite() const {
        StunOnlyRoomTransport *belt =
            dynamic_cast<StunOnlyRoomTransport *>(room_.get());
        return belt != nullptr ? belt->inner() : room_.get();
    }

private:
    std::unique_ptr<MdkrOnlineRoomTransport> room_;
    std::unique_ptr<MdkrOnlineMeshSignalBackend> mesh_;
    std::unique_ptr<IMdkrOnlineAdapter> inner_;
};

/* Stable per-launch session id: distinct across launches, constant within one,
 * derived once from the steady clock. The adapter uses it as the local identity
 * nonce; convergence does not depend on its value (the e2e driver uses a fixed
 * constant). */
uint64_t launchSessionId() {
    static const uint64_t id = []() {
        return static_cast<uint64_t>(
                   std::chrono::steady_clock::now().time_since_epoch().count()) ^
               UINT64_C(0x4f4e4c494e450000); /* "ONLINE" tag */
    }();
    return id;
}

}  // namespace

std::unique_ptr<IMdkrOnlineAdapter> OnlineRoom_makeGatedLiveAdapter(
    const MdkrOnlineCompatibilityV1 &compatibility, MdkrOnlineJourney journey,
    const std::string &joinCode) {
    std::string error;

    /* Construction never touches the network: the room transport begins on the
     * first submit(CREATE_ROOM/JOIN_ROOM). MDKR_PARTY_ORIGIN is the compiled
     * https origin (empty -> the transport refuses and we fall back to the
     * fake). Native transports refuse non-https/wss off-machine. */
    std::unique_ptr<MdkrOnlineRoomTransport> http =
        mdkr_online_room_http_transport_create(MDKR_PARTY_ORIGIN, &error);
    if (!http) return nullptr;
    auto room = std::make_unique<StunOnlyRoomTransport>(std::move(http));
    std::unique_ptr<MdkrOnlineMeshSignalBackend> mesh =
        mdkr_online_mesh_signal_backend_create(MDKR_PARTY_ORIGIN);
    if (!mesh) return nullptr;

    MdkrOnlineRoomTransport *roomPtr = room.get();
    MdkrOnlineMeshSignalBackend *meshPtr = mesh.get();
    auto adapter = std::make_unique<OwningLiveAdapter>(std::move(room),
                                                       std::move(mesh));

    MdkrOnlineLiveAdapterOptions options;
    options.sessionId = launchSessionId();
    options.compatibility = compatibility;
    options.journey = journey;                    /* CREATE or JOIN-by-code */
    options.localSeatCount = 1u;                  /* FENCE: solo endpoint (2P) */
    options.joinCode = joinCode;                  /* JOIN by 6-digit fallback */
    for (unsigned i = 0u; i < MDKR_MATCH_LOCAL_PLAYER_SLOTS; ++i) {
        options.localRoster.player_identity[i] = MDKR_MATCH_IDENTITY_RETAIL;
    }                                             /* FENCE: retail-only clamp */
    options.raceAdmissionEnabled = true;          /* beta build only */
    options.romVerified = true;                   /* beta: local ROM verified;
                                                     never sourced from service */
    options.inputDelay = 2u;
    options.room = roomPtr;
    options.meshBackend = meshPtr;

    if (!adapter->build(options, &error)) return nullptr;
    return adapter;
}

/* Beta-only invite accessor for the creator's invite card. Reaches past the
 * owning wrapper + STUN-only belt to the concrete HTTP transport and returns
 * its learned 6-digit fallback code and invite URL, but only once the room has
 * reached Ready (creator only). Returns false for a non-live adapter, a joiner,
 * or before the room is Ready -- so the panel simply shows nothing yet. */
bool OnlineRoom_liveInvite(IMdkrOnlineAdapter *adapter, std::string *code,
                           std::string *inviteUrl) {
    OwningLiveAdapter *owning = dynamic_cast<OwningLiveAdapter *>(adapter);
    if (owning == nullptr) return false;
    MdkrOnlineRoomTransport *http = owning->httpTransportForInvite();
    if (http == nullptr) return false;
    MdkrOnlineRoomHttpInvite invite;
    if (!mdkr_online_room_http_transport_invite(http, &invite) || !invite.ready) {
        return false;
    }
    if (code != nullptr) *code = invite.fallbackCode;
    if (inviteUrl != nullptr) *inviteUrl = invite.inviteUrl;
    return true;
}

#endif /* MDKR_ENABLE_ONLINE_BETA */
