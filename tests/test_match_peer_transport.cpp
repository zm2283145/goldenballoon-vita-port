/*
 * MdkrMatchPeerMesh (O-T2): the launcher-owned <=4-endpoint full mesh of
 * WebRTC DataChannels, driven end to end over loopback with REAL
 * libdatachannel peers (real DTLS, the test_lan_party_transport discipline)
 * and an in-process fake of the match-signaling relay.
 *
 * The fake hub implements the MdkrMatchPeerSignalFeed seam: every outbound
 * signal message a mesh sends is captured, translated exactly the way
 * services/party/src/match relays it (authenticated from-fields injected,
 * delivery to one exact endpoint/generation) and delivered to the target
 * feed's inbox. Everything runs on the test thread except libdatachannel's
 * own peer threads, whose callbacks only enqueue.
 */
#include "online/match_peer_transport.h"

#include "net/match_peer_crypto.h"
#include "net/match_peer_transcript.h"
#include "party/party_webrtc_signaling.h"

/* Assert-driven test: NDEBUG would compile every check away. */
#undef NDEBUG

#include <nlohmann/json.hpp>
#include <rtc/rtc.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace {

using Json = nlohmann::json;

constexpr uint32_t kEpoch = 7u;
constexpr unsigned kPhaseMs = 20000u;

/* ---- Deterministic fixtures --------------------------------------------- */

std::string roomIdFixture() {
    uint8_t raw[16];
    for (unsigned index = 0u; index < 16u; index++) {
        raw[index] = static_cast<uint8_t>(index + 1u);
    }
    return mdkr_party::base64Url(raw, sizeof(raw));
}

MdkrOnlineCompatibilityV1 compatibilityFixture() {
    MdkrOnlineCompatibilityV1 value{};
    value.protocol_version = 1u;
    for (unsigned index = 0u; index < sizeof(value.build_id); index++) {
        value.build_id[index] = static_cast<uint8_t>(index + 1u);
    }
    for (unsigned index = 0u; index < sizeof(value.gameplay_digest); index++) {
        value.gameplay_digest[index] = static_cast<uint8_t>(0x80u + index);
    }
    value.rom_revision = 1u;
    value.cadence_hz = 30u;
    return value;
}

/* ---- Shared fake clock (the mesh's nowMs seam) --------------------------- */

struct FakeClock {
    uint64_t nowMs = 1'000'000u; /* nonzero: 0 means "no offer sent" to the
                                  * retry ladder, exactly like the party
                                  * policy tests. */
    std::function<uint64_t()> fn() {
        return [this]() { return nowMs; };
    }
};

/* ---- In-process signaling hub (the loopback relay) ----------------------- */

struct SentRecord {
    uint64_t from = 0u;
    MdkrMatchSignalOutbound message;
};

class FakeHub;

class FakeFeed final : public MdkrMatchPeerSignalFeed {
public:
    FakeFeed(FakeHub *hub, uint64_t endpointId) : hub_(hub), self_(endpointId) {}
    MdkrMatchSignalSendResult send(const MdkrMatchSignalOutbound &m) override;
    void drainEvents(std::vector<MdkrMatchSignalEvent> &out) override;

    std::deque<MdkrMatchSignalEvent> inbox;

private:
    FakeHub *hub_;
    uint64_t self_;
};

/* Thread-safe: RawPeer's libdatachannel callbacks route answers/candidates
 * from peer threads while the test thread pumps the mesh feeds. */
class FakeHub {
public:
    struct Endpoint {
        uint32_t generation = 0u;
        std::unique_ptr<FakeFeed> feed;
        bool blackholed = false; /* outbound to this endpoint is dropped */
    };

    FakeFeed *addEndpoint(uint64_t id, uint32_t generation) {
        std::lock_guard<std::mutex> lock(mutex_);
        Endpoint &endpoint = endpoints_[id];
        endpoint.generation = generation;
        endpoint.feed = std::make_unique<FakeFeed>(this, id);
        return endpoint.feed.get();
    }

    void blackhole(uint64_t id, bool value) {
        std::lock_guard<std::mutex> lock(mutex_);
        endpoints_[id].blackholed = value;
    }

    /* A signaling reconnect: the relay tracks a strictly higher generation
     * for this endpoint from now on. */
    void setGeneration(uint64_t id, uint32_t generation) {
        std::lock_guard<std::mutex> lock(mutex_);
        endpoints_[id].generation = generation;
    }

    /* M1 seam: fail the Nth peer_hello send (1-based, counted per sender)
     * with the relay's peer_unavailable refusal instead of delivering. */
    void failHelloSend(uint64_t from, unsigned index) {
        std::lock_guard<std::mutex> lock(mutex_);
        helloFailPlan_[from].insert(index);
    }

    /* Deliver a welcome to `target` naming every OTHER endpoint present. */
    void welcome(uint64_t target) {
        std::lock_guard<std::mutex> lock(mutex_);
        Endpoint &endpoint = endpoints_.at(target);
        MdkrMatchSignalEvent event;
        event.type = MdkrMatchSignalEventType::Welcome;
        event.endpointId = std::to_string(target);
        event.connectionGeneration = endpoint.generation;
        for (const auto &entry : endpoints_) {
            if (entry.first == target) continue;
            MdkrMatchSignalPeerRef ref;
            ref.endpointId = std::to_string(entry.first);
            ref.connectionGeneration = entry.second.generation;
            event.peers.push_back(std::move(ref));
        }
        endpoint.feed->inbox.push_back(std::move(event));
    }

    /* Inject one raw event into `target`'s inbox (stale-answer cases). */
    void inject(uint64_t target, MdkrMatchSignalEvent event) {
        std::lock_guard<std::mutex> lock(mutex_);
        endpoints_.at(target).feed->inbox.push_back(std::move(event));
    }

    MdkrMatchSignalSendResult route(uint64_t from,
                                    const MdkrMatchSignalOutbound &m) {
        std::lock_guard<std::mutex> lock(mutex_);
        sentLog.push_back(SentRecord{from, m});
        MdkrMatchSignalSendResult result;
        result.sequence = ++sequence_;
        result.ok = true;
        if (m.type == "peer_hello") {
            const unsigned index = ++helloSendCount_[from];
            const auto plan = helloFailPlan_.find(from);
            if (plan != helloFailPlan_.end() &&
                plan->second.count(index) != 0u) {
                result.ok = false;
                result.error = kMdkrMatchSignalPeerUnavailable;
                return result;
            }
        }
        uint64_t to = 0u;
        try { to = std::stoull(m.toEndpointId); } catch (...) { to = 0u; }
        const auto found = endpoints_.find(to);
        if (found == endpoints_.end() ||
            found->second.generation != m.toConnectionGeneration) {
            /* The real relay answers with a bounded signal_error; delivery
             * simply does not happen. */
            return result;
        }
        if (found->second.blackholed) return result;
        MdkrMatchSignalEvent event;
        event.fromEndpointId = std::to_string(from);
        event.fromConnectionGeneration = endpoints_.at(from).generation;
        if (m.type == "peer_hello") {
            event.type = MdkrMatchSignalEventType::PeerHello;
            event.publicKey = m.publicKey;
        } else if (m.type == "webrtc_offer") {
            event.type = MdkrMatchSignalEventType::WebrtcOffer;
            event.sdp = m.sdp;
        } else if (m.type == "webrtc_answer") {
            event.type = MdkrMatchSignalEventType::WebrtcAnswer;
            event.sdp = m.sdp;
        } else if (m.type == "webrtc_ice") {
            event.type = MdkrMatchSignalEventType::WebrtcIce;
            event.candidate = m.candidate;
            event.hasSdpMid = m.hasSdpMid;
            event.sdpMid = m.sdpMid;
            event.hasSdpMLineIndex = m.hasSdpMLineIndex;
            event.sdpMLineIndex = m.sdpMLineIndex;
            event.hasUsernameFragment = m.hasUsernameFragment;
            event.usernameFragment = m.usernameFragment;
        } else if (m.type == "peer_end") {
            event.type = MdkrMatchSignalEventType::PeerEnd;
            event.reason = m.reason;
        } else {
            result.ok = false;
            result.error = kMdkrMatchSignalInvalidClientMessage;
            return result;
        }
        found->second.feed->inbox.push_back(std::move(event));
        return result;
    }

    unsigned countSent(uint64_t from, const std::string &type,
                       const std::string &reason = std::string()) {
        std::lock_guard<std::mutex> lock(mutex_);
        unsigned count = 0u;
        for (const SentRecord &record : sentLog) {
            if (record.from == from && record.message.type == type &&
                (reason.empty() || record.message.reason == reason)) {
                count++;
            }
        }
        return count;
    }

    void drainInbox(FakeFeed *feed, std::vector<MdkrMatchSignalEvent> &out) {
        std::lock_guard<std::mutex> lock(mutex_);
        out.clear();
        while (!feed->inbox.empty()) {
            out.push_back(std::move(feed->inbox.front()));
            feed->inbox.pop_front();
        }
    }

    std::vector<SentRecord> sentLog;

private:
    std::map<uint64_t, Endpoint> endpoints_;
    std::map<uint64_t, std::set<unsigned>> helloFailPlan_;
    std::map<uint64_t, unsigned> helloSendCount_;
    std::mutex mutex_;
    uint32_t sequence_ = 0u;
};

MdkrMatchSignalSendResult FakeFeed::send(const MdkrMatchSignalOutbound &m) {
    return hub_->route(self_, m);
}

void FakeFeed::drainEvents(std::vector<MdkrMatchSignalEvent> &out) {
    hub_->drainInbox(this, out);
}

/* ---- Mesh harness -------------------------------------------------------- */

struct MeshHarness {
    FakeHub hub;
    FakeClock clock;
    std::vector<std::unique_ptr<MdkrMatchPeerMesh>> meshes;
    std::map<uint64_t, std::vector<MdkrMatchPeerMeshEvent>> events;
    std::vector<uint64_t> ids;

    ~MeshHarness() { meshes.clear(); /* before the hub's feeds */ }

    MdkrMatchPeerMesh *mesh(uint64_t id) {
        for (size_t index = 0u; index < ids.size(); index++) {
            if (ids[index] == id) return meshes[index].get();
        }
        return nullptr;
    }

    MdkrMatchPeerMesh *add(uint64_t id, uint32_t generation,
                           const std::vector<MdkrMatchPeerSlotOwner> &roster) {
        FakeFeed *feed = hub.addEndpoint(id, generation);
        MdkrMatchPeerMeshOptions options;
        options.signal = feed;
        options.roomId = roomIdFixture();
        options.localEndpointId = id;
        options.localGeneration = generation;
        options.matchEpoch = kEpoch;
        options.compatibility = compatibilityFixture();
        options.roster = roster;
        options.nowMs = clock.fn();
        std::string error;
        std::unique_ptr<MdkrMatchPeerMesh> mesh =
            MdkrMatchPeerMesh::create(options, &error);
        if (!mesh) {
            std::fprintf(stderr, "mesh create refused: %s\n", error.c_str());
            assert(mesh);
        }
        ids.push_back(id);
        meshes.push_back(std::move(mesh));
        return meshes.back().get();
    }

    void pumpOnce() {
        std::vector<MdkrMatchPeerMeshEvent> drained;
        for (size_t index = 0u; index < meshes.size(); index++) {
            meshes[index]->pump();
            meshes[index]->drainEvents(drained);
            for (MdkrMatchPeerMeshEvent &event : drained) {
                events[ids[index]].push_back(std::move(event));
            }
        }
    }

    bool pumpUntil(const std::function<bool()> &done,
                   unsigned maxMillis = kPhaseMs, uint64_t fakeStepMs = 0u) {
        for (unsigned elapsed = 0u; elapsed <= maxMillis; elapsed += 10u) {
            pumpOnce();
            if (done()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            clock.nowMs += fakeStepMs;
        }
        pumpOnce();
        return done();
    }

    unsigned countEvents(uint64_t owner, MdkrMatchPeerMeshEventType type,
                         uint64_t aboutEndpoint = 0u) const {
        unsigned count = 0u;
        const auto found = events.find(owner);
        if (found == events.end()) return 0u;
        for (const MdkrMatchPeerMeshEvent &event : found->second) {
            if (event.type != type) continue;
            if (aboutEndpoint != 0u && event.endpointId != aboutEndpoint)
                continue;
            count++;
        }
        return count;
    }

    const MdkrMatchPeerMeshEvent *lastEvent(
        uint64_t owner, MdkrMatchPeerMeshEventType type,
        uint64_t aboutEndpoint = 0u) const {
        const auto found = events.find(owner);
        if (found == events.end()) return nullptr;
        for (auto it = found->second.rbegin(); it != found->second.rend();
             ++it) {
            if (it->type != type) continue;
            if (aboutEndpoint != 0u && it->endpointId != aboutEndpoint)
                continue;
            return &*it;
        }
        return nullptr;
    }
};

std::vector<MdkrMatchPeerSlotOwner> rosterOf(
    std::initializer_list<uint64_t> ids) {
    std::vector<MdkrMatchPeerSlotOwner> roster;
    uint8_t slot = 1u;
    for (uint64_t id : ids) {
        MdkrMatchPeerSlotOwner owner;
        owner.endpointId = id;
        owner.slotMask = slot;
        slot = static_cast<uint8_t>(slot << 1u);
        roster.push_back(owner);
    }
    return roster;
}

std::array<uint8_t, MDKR_MATCH_PEER_PAYLOAD_BYTES> payloadFixture(uint8_t seed) {
    std::array<uint8_t, MDKR_MATCH_PEER_PAYLOAD_BYTES> payload{};
    for (unsigned index = 0u; index < payload.size(); index++) {
        payload[index] = static_cast<uint8_t>(seed + index * 3u);
    }
    return payload;
}

/* ---- A raw protocol peer: real libdatachannel + the crypto C API --------- */

/* Answers the mesh's offer with a real PeerConnection, performs the
 * three-hello commitment dance by hand, and can seal/open envelopes with
 * its own identity -- full control over every byte the mesh receives, which
 * the misbehavior cases need. Channel callbacks only enqueue. */
struct RawPeer {
    uint64_t selfId;
    uint32_t selfGeneration;
    uint64_t meshId;
    uint32_t meshGeneration;
    FakeHub *hub;
    bool sendHellos = true;
    bool answerPings = true;

    MdkrMatchPeerIdentity *identity = nullptr;
    std::array<uint8_t, MDKR_MATCH_PEER_PUBLIC_KEY_BYTES> publicKey{};
    std::array<uint8_t, MDKR_MATCH_PEER_COMMIT_NONCE_BYTES> nonce{};
    std::array<uint8_t, MDKR_MATCH_PEER_COMMIT_BYTES> commitment{};

    unsigned meshHellos = 0u;
    std::array<uint8_t, MDKR_MATCH_PEER_COMMIT_BYTES> meshCommitment{};
    std::array<uint8_t, MDKR_MATCH_PEER_PUBLIC_KEY_BYTES> meshKey{};
    std::array<uint8_t, MDKR_MATCH_PEER_COMMIT_NONCE_BYTES> meshNonce{};
    bool sentReveal = false;
    bool sentCommit = false;

    std::shared_ptr<rtc::PeerConnection> pc;
    std::shared_ptr<rtc::DataChannel> state;
    std::shared_ptr<rtc::DataChannel> control;
    std::mutex mutex;
    std::deque<std::string> controlText;

    MdkrMatchPeerKeyring ring{};
    MdkrMatchPeerSealingKey *sealKey = nullptr;
    bool keysDerived = false;

    RawPeer(uint64_t self, uint32_t generation, uint64_t mesh,
            uint32_t meshGen, FakeHub *owner)
        : selfId(self), selfGeneration(generation), meshId(mesh),
          meshGeneration(meshGen), hub(owner) {
        identity = mdkr_match_peer_identity_create();
        assert(identity != nullptr);
        assert(mdkr_match_peer_identity_public_key(identity, publicKey.data()));
        for (unsigned index = 0u; index < nonce.size(); index++) {
            nonce[index] = static_cast<uint8_t>(0x21u + index);
        }
        assert(mdkr_match_peer_commitment(kEpoch, selfId, selfGeneration,
                                          nonce.data(), publicKey.data(),
                                          commitment.data()));
    }

    ~RawPeer() {
        if (pc) pc->close();
        mdkr_match_peer_keyring_forget(&ring);
        mdkr_match_peer_identity_destroy(identity);
    }

    void sendSignal(const std::string &type,
                    const std::function<void(MdkrMatchSignalOutbound &)> &fill) {
        MdkrMatchSignalOutbound message;
        message.type = type;
        message.toEndpointId = std::to_string(meshId);
        message.toConnectionGeneration = meshGeneration;
        fill(message);
        hub->route(selfId, message);
    }

    std::string helloBlob(const uint8_t body[32]) const {
        uint8_t raw[MDKR_MATCH_PEER_PUBLIC_KEY_BYTES] = {};
        raw[0] = 0x04u;
        std::memcpy(raw + 1u, body, 32u);
        return mdkr_party::base64Url(raw, sizeof(raw));
    }

    void sendCommitHello() {
        sendSignal("peer_hello", [this](MdkrMatchSignalOutbound &m) {
            m.publicKey = helloBlob(commitment.data());
        });
        sentCommit = true;
    }

    void sendRevealHellos() {
        sendSignal("peer_hello", [this](MdkrMatchSignalOutbound &m) {
            m.publicKey = mdkr_party::base64Url(publicKey.data(),
                                                publicKey.size());
        });
        sendSignal("peer_hello", [this](MdkrMatchSignalOutbound &m) {
            m.publicKey = helloBlob(nonce.data());
        });
        sentReveal = true;
    }

    /* Drive from the test thread with events drained from the hub inbox. */
    void onSignalEvent(const MdkrMatchSignalEvent &event) {
        if (event.type == MdkrMatchSignalEventType::PeerHello) {
            std::array<uint8_t, 65> raw{};
            assert(mdkr_party::decodePublicKey(event.publicKey, raw));
            if (meshHellos == 0u) {
                std::memcpy(meshCommitment.data(), raw.data() + 1u, 32u);
            } else if (meshHellos == 1u) {
                std::memcpy(meshKey.data(), raw.data(), raw.size());
            } else if (meshHellos == 2u) {
                std::memcpy(meshNonce.data(), raw.data() + 1u, 32u);
                assert(mdkr_match_peer_commitment_verify(
                    kEpoch, meshId, meshGeneration, meshNonce.data(),
                    meshKey.data(), meshCommitment.data()));
            }
            meshHellos++;
            if (sendHellos && !sentReveal && meshHellos >= 1u) {
                if (!sentCommit) sendCommitHello();
                sendRevealHellos();
            }
        } else if (event.type == MdkrMatchSignalEventType::WebrtcOffer) {
            startPeer();
            try {
                pc->setRemoteDescription(rtc::Description(event.sdp, "offer"));
            } catch (...) { assert(false && "offer rejected"); }
        } else if (event.type == MdkrMatchSignalEventType::WebrtcIce) {
            if (!pc) return;
            try {
                pc->addRemoteCandidate(
                    rtc::Candidate(event.candidate, event.sdpMid));
            } catch (...) {}
        }
    }

    void startPeer() {
        if (pc) return;
        rtc::Configuration configuration; /* loopback: no ICE servers */
        pc = std::make_shared<rtc::PeerConnection>(configuration);
        pc->onLocalDescription([this](rtc::Description description) {
            sendSignal("webrtc_answer", [&](MdkrMatchSignalOutbound &m) {
                m.sdp = std::string(description);
            });
        });
        pc->onLocalCandidate([this](rtc::Candidate candidate) {
            sendSignal("webrtc_ice", [&](MdkrMatchSignalOutbound &m) {
                m.candidate = std::string(candidate);
                m.hasSdpMid = true;
                m.sdpMid = candidate.mid();
            });
        });
        pc->onDataChannel([this](std::shared_ptr<rtc::DataChannel> channel) {
            const std::string label = channel->label();
            std::lock_guard<std::mutex> lock(mutex);
            if (label == kMdkrMatchControlChannelLabel) {
                control = channel;
                channel->onMessage([this](rtc::message_variant message) {
                    if (!std::holds_alternative<std::string>(message)) return;
                    std::lock_guard<std::mutex> inner(mutex);
                    controlText.push_back(std::get<std::string>(message));
                });
            } else if (label == kMdkrMatchStateChannelLabel) {
                state = channel;
            }
        });
    }

    bool channelsOpen() {
        std::lock_guard<std::mutex> lock(mutex);
        return state && state->isOpen() && control && control->isOpen();
    }

    /* Answer mesh pings so the stale ladder stays quiet where wanted. */
    void serviceControl() {
        std::deque<std::string> pending;
        std::shared_ptr<rtc::DataChannel> channel;
        {
            std::lock_guard<std::mutex> lock(mutex);
            pending.swap(controlText);
            channel = control;
        }
        for (const std::string &text : pending) {
            Json value = Json::parse(text, nullptr, false);
            if (value.is_discarded() || !value.is_object()) continue;
            if (answerPings && value.value("type", std::string{}) == "ping" &&
                channel && channel->isOpen()) {
                try {
                    channel->send(Json{{"type", "pong"}, {"protocol", 1u},
                        {"nonce", value.value("nonce", 0u)}}.dump());
                } catch (...) {}
            }
        }
    }

    void deriveKeys() {
        assert(meshHellos >= 3u);
        MdkrMatchPeerTranscript transcript{};
        uint8_t roomRaw[16];
        for (unsigned index = 0u; index < 16u; index++) {
            roomRaw[index] = static_cast<uint8_t>(index + 1u);
        }
        std::memcpy(transcript.room_id, roomRaw, sizeof(roomRaw));
        transcript.match_epoch = kEpoch;
        transcript.compatibility = compatibilityFixture();
        transcript.entry_count = 2u;
        transcript.entries[0].endpoint_id = meshId;
        transcript.entries[0].generation = meshGeneration;
        std::memcpy(transcript.entries[0].public_key, meshKey.data(),
                    meshKey.size());
        std::memcpy(transcript.entries[0].commitment, meshCommitment.data(),
                    meshCommitment.size());
        std::memcpy(transcript.entries[0].commit_nonce, meshNonce.data(),
                    meshNonce.size());
        transcript.entries[1].endpoint_id = selfId;
        transcript.entries[1].generation = selfGeneration;
        std::memcpy(transcript.entries[1].public_key, publicKey.data(),
                    publicKey.size());
        std::memcpy(transcript.entries[1].commitment, commitment.data(),
                    commitment.size());
        std::memcpy(transcript.entries[1].commit_nonce, nonce.data(),
                    nonce.size());
        uint8_t digest[MDKR_MATCH_PEER_TRANSCRIPT_DIGEST_BYTES];
        assert(mdkr_match_peer_transcript_digest(&transcript, digest));
        MdkrMatchPeerKeyContext outbound{};
        outbound.match_epoch = kEpoch;
        outbound.source_endpoint_id = selfId;
        outbound.source_generation = selfGeneration;
        outbound.destination_endpoint_id = meshId;
        outbound.destination_generation = meshGeneration;
        sealKey = mdkr_match_peer_identity_derive_key(
            &ring, identity, meshKey.data(), digest, &outbound);
        assert(sealKey != nullptr);
        keysDerived = true;
    }

    bool sendStateBytes(const std::vector<uint8_t> &bytes) {
        std::shared_ptr<rtc::DataChannel> channel;
        {
            std::lock_guard<std::mutex> lock(mutex);
            channel = state;
        }
        if (!channel || !channel->isOpen()) return false;
        std::vector<std::byte> raw(bytes.size());
        std::memcpy(raw.data(), bytes.data(), bytes.size());
        try { return channel->send(raw.data(), raw.size()); }
        catch (...) { return false; }
    }

    bool sendControlBytes(const std::vector<uint8_t> &bytes) {
        std::shared_ptr<rtc::DataChannel> channel;
        {
            std::lock_guard<std::mutex> lock(mutex);
            channel = control;
        }
        if (!channel || !channel->isOpen()) return false;
        std::vector<std::byte> raw(bytes.size());
        std::memcpy(raw.data(), bytes.data(), bytes.size());
        try { return channel->send(raw.data(), raw.size()); }
        catch (...) { return false; }
    }

    bool sendControlText(const std::string &text) {
        std::shared_ptr<rtc::DataChannel> channel;
        {
            std::lock_guard<std::mutex> lock(mutex);
            channel = control;
        }
        if (!channel || !channel->isOpen()) return false;
        try { return channel->send(text); }
        catch (...) { return false; }
    }

    std::vector<uint8_t> sealed(
        const std::array<uint8_t, MDKR_MATCH_PEER_PAYLOAD_BYTES> &payload,
        uint8_t payloadType) {
        assert(keysDerived);
        MdkrMatchPeerSendContext context{};
        context.key = sealKey->direction;
        context.intermediate_endpoint_id = 0u;
        context.payload_type = payloadType;
        std::vector<uint8_t> envelope(MDKR_MATCH_PEER_ENVELOPE_BYTES);
        assert(mdkr_match_peer_seal(sealKey, &context, payload.data(),
                                    envelope.data()));
        return envelope;
    }

    std::vector<uint8_t> sealedInput(
        const std::array<uint8_t, MDKR_MATCH_PEER_PAYLOAD_BYTES> &payload) {
        return sealed(payload, MDKR_MATCH_PEER_PAYLOAD_INPUT);
    }

    std::vector<uint8_t> sealedFragment(
        const std::array<uint8_t, MDKR_MATCH_PEER_PAYLOAD_BYTES> &payload) {
        return sealed(payload, MDKR_MATCH_PEER_PAYLOAD_PREFLIGHT_FRAGMENT);
    }
};

/* Connect a 2-mesh pair to PeerChannelsReady + PhraseReady on both sides. */
struct PairHarness {
    MeshHarness harness;
    MdkrMatchPeerMesh *low = nullptr;
    MdkrMatchPeerMesh *high = nullptr;

    bool connect() {
        const std::vector<MdkrMatchPeerSlotOwner> roster = rosterOf({100u, 200u});
        low = harness.add(100u, 1u, roster);
        high = harness.add(200u, 2u, roster);
        harness.hub.welcome(100u);
        harness.hub.welcome(200u);
        return harness.pumpUntil([this]() {
            return harness.countEvents(
                       100u, MdkrMatchPeerMeshEventType::PeerChannelsReady,
                       200u) >= 1u &&
                   harness.countEvents(
                       200u, MdkrMatchPeerMeshEventType::PeerChannelsReady,
                       100u) >= 1u &&
                   harness.countEvents(
                       100u, MdkrMatchPeerMeshEventType::PhraseReady) >= 1u &&
                   harness.countEvents(
                       200u, MdkrMatchPeerMeshEventType::PhraseReady) >= 1u;
        });
    }
};

/* ======================================================================== */

void twoPeerHappyPath() {
    PairHarness pair;
    assert(pair.connect());

    /* Phrase equality across both sides -- the whole point of the committed
     * transcript. */
    std::string phraseLow;
    std::string phraseHigh;
    assert(pair.low->phrase(phraseLow));
    assert(pair.high->phrase(phraseHigh));
    assert(!phraseLow.empty() && phraseLow == phraseHigh);

    /* Input envelope round trip, both directions, bundle bytes identical. */
    const auto payloadLow = payloadFixture(0x11u);
    const auto payloadHigh = payloadFixture(0x77u);
    assert(pair.low->sendInput(payloadLow.data()) == 1u);
    assert(pair.high->sendInput(payloadHigh.data()) == 1u);
    assert(pair.harness.pumpUntil([&]() {
        return pair.harness.countEvents(
                   200u, MdkrMatchPeerMeshEventType::InputEnvelope, 100u) >= 1u &&
               pair.harness.countEvents(
                   100u, MdkrMatchPeerMeshEventType::InputEnvelope, 200u) >= 1u;
    }));
    const MdkrMatchPeerMeshEvent *toHigh = pair.harness.lastEvent(
        200u, MdkrMatchPeerMeshEventType::InputEnvelope, 100u);
    const MdkrMatchPeerMeshEvent *toLow = pair.harness.lastEvent(
        100u, MdkrMatchPeerMeshEventType::InputEnvelope, 200u);
    assert(toHigh != nullptr && toHigh->payload == payloadLow);
    assert(toLow != nullptr && toLow->payload == payloadHigh);
    assert(toHigh->context.payload_type == MDKR_MATCH_PEER_PAYLOAD_INPUT);

    /* Preflight fragment on the reliable control channel. */
    const auto fragment = payloadFixture(0x40u);
    assert(pair.low->sendPreflightFragment(200u, fragment.data()));
    assert(pair.harness.pumpUntil([&]() {
        return pair.harness.countEvents(
                   200u, MdkrMatchPeerMeshEventType::PreflightFragment,
                   100u) >= 1u;
    }));
    const MdkrMatchPeerMeshEvent *preflight = pair.harness.lastEvent(
        200u, MdkrMatchPeerMeshEventType::PreflightFragment, 100u);
    assert(preflight != nullptr && preflight->payload == fragment);
    assert(preflight->context.payload_type ==
           MDKR_MATCH_PEER_PAYLOAD_PREFLIGHT_FRAGMENT);
    std::printf("twoPeerHappyPath: ok (phrase='%s')\n", phraseLow.c_str());
}

void threePeerMeshEveryoneReachesEveryone() {
    MeshHarness harness;
    const std::vector<MdkrMatchPeerSlotOwner> roster =
        rosterOf({100u, 200u, 300u});
    harness.add(100u, 1u, roster);
    harness.add(200u, 2u, roster);
    harness.add(300u, 3u, roster);
    harness.hub.welcome(100u);
    harness.hub.welcome(200u);
    harness.hub.welcome(300u);
    assert(harness.pumpUntil([&]() {
        return harness.countEvents(100u,
                   MdkrMatchPeerMeshEventType::PeerChannelsReady, 200u) >= 1u &&
               harness.countEvents(100u,
                   MdkrMatchPeerMeshEventType::PeerChannelsReady, 300u) >= 1u &&
               harness.countEvents(200u,
                   MdkrMatchPeerMeshEventType::PeerChannelsReady, 100u) >= 1u &&
               harness.countEvents(200u,
                   MdkrMatchPeerMeshEventType::PeerChannelsReady, 300u) >= 1u &&
               harness.countEvents(300u,
                   MdkrMatchPeerMeshEventType::PeerChannelsReady, 100u) >= 1u &&
               harness.countEvents(300u,
                   MdkrMatchPeerMeshEventType::PeerChannelsReady, 200u) >= 1u &&
               harness.countEvents(100u,
                   MdkrMatchPeerMeshEventType::PhraseReady) >= 1u &&
               harness.countEvents(200u,
                   MdkrMatchPeerMeshEventType::PhraseReady) >= 1u &&
               harness.countEvents(300u,
                   MdkrMatchPeerMeshEventType::PhraseReady) >= 1u;
    }, 30000u));

    std::string phraseA;
    std::string phraseB;
    std::string phraseC;
    assert(harness.mesh(100u)->phrase(phraseA));
    assert(harness.mesh(200u)->phrase(phraseB));
    assert(harness.mesh(300u)->phrase(phraseC));
    assert(!phraseA.empty() && phraseA == phraseB && phraseB == phraseC);

    /* One input from the lowest endpoint reaches both others. */
    const auto payload = payloadFixture(0x05u);
    assert(harness.mesh(100u)->sendInput(payload.data()) == 2u);
    assert(harness.pumpUntil([&]() {
        return harness.countEvents(200u,
                   MdkrMatchPeerMeshEventType::InputEnvelope, 100u) >= 1u &&
               harness.countEvents(300u,
                   MdkrMatchPeerMeshEventType::InputEnvelope, 100u) >= 1u;
    }));
    const MdkrMatchPeerMeshEvent *atB = harness.lastEvent(
        200u, MdkrMatchPeerMeshEventType::InputEnvelope, 100u);
    const MdkrMatchPeerMeshEvent *atC = harness.lastEvent(
        300u, MdkrMatchPeerMeshEventType::InputEnvelope, 100u);
    assert(atB != nullptr && atB->payload == payload);
    assert(atC != nullptr && atC->payload == payload);
    std::printf("threePeerMeshEveryoneReachesEveryone: ok (phrase='%s')\n",
                phraseA.c_str());
}

void staleGenerationAnswerIgnored() {
    MeshHarness harness;
    const std::vector<MdkrMatchPeerSlotOwner> roster = rosterOf({100u, 200u});
    MdkrMatchPeerMesh *mesh = harness.add(100u, 1u, roster);
    harness.hub.addEndpoint(200u, 7u); /* feed unused: 200 never answers */
    harness.hub.welcome(100u);
    /* Let the offer go out. */
    assert(harness.pumpUntil([&]() {
        return harness.hub.countSent(100u, "webrtc_offer") >= 1u;
    }));

    /* An answer from a RETIRED generation of 200 must be ignored: no state
     * change, no PeerLost, only the stale counter moves. */
    MdkrMatchSignalEvent stale;
    stale.type = MdkrMatchSignalEventType::WebrtcAnswer;
    stale.fromEndpointId = "200";
    stale.fromConnectionGeneration = 6u; /* tracked current is 7 */
    stale.sdp = "v=0\r\no=- 0 0 IN IP4 127.0.0.1\r\ns=-\r\n";
    harness.hub.inject(100u, stale);
    /* And an offer from an endpoint outside the roster is ignored too. */
    MdkrMatchSignalEvent unknown;
    unknown.type = MdkrMatchSignalEventType::WebrtcOffer;
    unknown.fromEndpointId = "999";
    unknown.fromConnectionGeneration = 1u;
    unknown.sdp = "v=0\r\n";
    harness.hub.inject(100u, unknown);
    harness.pumpOnce();
    const MdkrMatchPeerMeshStats stats = mesh->stats();
    assert(stats.ignoredStaleSignals >= 2u);
    assert(harness.countEvents(100u, MdkrMatchPeerMeshEventType::PeerLost) ==
           0u);
    assert(harness.countEvents(100u, MdkrMatchPeerMeshEventType::Failure) ==
           0u);
    std::printf("staleGenerationAnswerIgnored: ok (ignored=%llu)\n",
                static_cast<unsigned long long>(stats.ignoredStaleSignals));
}

void phraseRefusedBeforeCommitmentCompletes() {
    MeshHarness harness;
    const std::vector<MdkrMatchPeerSlotOwner> roster = rosterOf({100u, 200u});
    MdkrMatchPeerMesh *mesh = harness.add(100u, 1u, roster);
    harness.hub.addEndpoint(200u, 2u);
    RawPeer raw(200u, 2u, 100u, 1u, &harness.hub);
    raw.sendHellos = false; /* hand-drive each round */
    harness.hub.welcome(100u);

    std::string phrase;
    harness.pumpOnce();
    assert(!mesh->phrase(phrase)); /* nothing yet */

    /* Round 1 only: the peer's commitment. Still no phrase. */
    raw.sendCommitHello();
    harness.pumpOnce();
    assert(!mesh->phrase(phrase));
    assert(harness.countEvents(100u,
               MdkrMatchPeerMeshEventType::PhraseReady) == 0u);

    /* Key revealed but the commitment not yet OPENED: still refused. */
    raw.sendSignal("peer_hello", [&](MdkrMatchSignalOutbound &m) {
        m.publicKey = mdkr_party::base64Url(raw.publicKey.data(),
                                            raw.publicKey.size());
    });
    harness.pumpOnce();
    assert(!mesh->phrase(phrase));
    assert(harness.countEvents(100u,
               MdkrMatchPeerMeshEventType::PhraseReady) == 0u);

    /* The opening nonce completes the two-round handshake: phrase appears. */
    raw.sendSignal("peer_hello", [&](MdkrMatchSignalOutbound &m) {
        m.publicKey = raw.helloBlob(raw.nonce.data());
    });
    assert(harness.pumpUntil([&]() {
        return harness.countEvents(100u,
                   MdkrMatchPeerMeshEventType::PhraseReady) >= 1u;
    }, 2000u));
    assert(mesh->phrase(phrase));
    assert(!phrase.empty());
    std::printf("phraseRefusedBeforeCommitmentCompletes: ok\n");
}

void sealWindowExhaustionIsTypedPeerLoss() {
    PairHarness pair;
    assert(pair.connect());
    /* The seam leaves the outbound window exactly as one UINT64_MAX seal
     * would: exhausted, requiring reconnect + fresh generation-bound key. */
    assert(mdkr_match_peer_mesh_exhaust_seal_for_test(*pair.low, 200u));
    const auto payload = payloadFixture(0x0Au);
    assert(pair.low->sendInput(payload.data()) == 0u);
    assert(pair.harness.pumpUntil([&]() {
        return pair.harness.countEvents(
                   100u, MdkrMatchPeerMeshEventType::PeerLost, 200u) >= 1u;
    }, 2000u));
    const MdkrMatchPeerMeshEvent *lost = pair.harness.lastEvent(
        100u, MdkrMatchPeerMeshEventType::PeerLost, 200u);
    assert(lost != nullptr &&
           lost->lostReason == MdkrMatchPeerLostReason::SealWindowExhausted);
    /* Terminal for that peer: later sends refuse rather than re-reporting. */
    assert(pair.low->sendInput(payload.data()) == 0u);
    std::printf("sealWindowExhaustionIsTypedPeerLoss: ok\n");
}

/* One mesh + one RawPeer wired through the hub. */
struct RawHarness {
    MeshHarness harness;
    MdkrMatchPeerMesh *mesh = nullptr;
    std::unique_ptr<RawPeer> raw;
    FakeFeed *rawFeed = nullptr;

    void drainRawInbox() {
        /* RawPeer consumes its own hub inbox on the test thread. */
        if (rawFeed == nullptr) return;
        std::vector<MdkrMatchSignalEvent> drained;
        rawFeed->drainEvents(drained);
        for (const MdkrMatchSignalEvent &event : drained) {
            raw->onSignalEvent(event);
        }
    }
};

void badStateEnvelopeDroppedAndCounted() {
    RawHarness rig;
    const std::vector<MdkrMatchPeerSlotOwner> roster = rosterOf({100u, 200u});
    rig.mesh = rig.harness.add(100u, 1u, roster);
    rig.rawFeed = rig.harness.hub.addEndpoint(200u, 2u);
    rig.raw = std::make_unique<RawPeer>(200u, 2u, 100u, 1u, &rig.harness.hub);
    rig.harness.hub.welcome(100u);
    assert(rig.harness.pumpUntil([&]() {
        rig.drainRawInbox();
        rig.raw->serviceControl();
        return rig.harness.countEvents(100u,
                   MdkrMatchPeerMeshEventType::PeerChannelsReady, 200u) >= 1u &&
               rig.raw->channelsOpen() && rig.raw->meshHellos >= 3u;
    }));
    rig.raw->deriveKeys();

    /* Garbage on the LOSSY state channel: dropped + counted, never fatal. */
    std::vector<uint8_t> junk(MDKR_MATCH_PEER_ENVELOPE_BYTES, 0xEEu);
    assert(rig.raw->sendStateBytes(junk));
    std::vector<uint8_t> runt(20u, 0x55u);
    assert(rig.raw->sendStateBytes(runt));
    assert(rig.harness.pumpUntil([&]() {
        rig.raw->serviceControl();
        return rig.mesh->stats().rejectedStateEnvelopes >= 2u;
    }, 5000u));
    assert(rig.harness.countEvents(100u,
               MdkrMatchPeerMeshEventType::PeerLost) == 0u);

    /* The mesh is still healthy: a genuine sealed envelope right after the
     * garbage opens and surfaces normally. */
    const auto payload = payloadFixture(0x33u);
    assert(rig.raw->sendStateBytes(rig.raw->sealedInput(payload)));
    assert(rig.harness.pumpUntil([&]() {
        rig.raw->serviceControl();
        return rig.harness.countEvents(100u,
                   MdkrMatchPeerMeshEventType::InputEnvelope, 200u) >= 1u;
    }, 5000u));
    const MdkrMatchPeerMeshEvent *input = rig.harness.lastEvent(
        100u, MdkrMatchPeerMeshEventType::InputEnvelope, 200u);
    assert(input != nullptr && input->payload == payload);

    /* A correctly sized control envelope that fails to open is the
     * rekey-window race shape: counted, dropped, NOT terminal (I2). */
    assert(rig.raw->sendControlBytes(junk));
    assert(rig.harness.pumpUntil([&]() {
        rig.raw->serviceControl();
        return rig.mesh->stats().rejectedControlEnvelopes >= 1u;
    }, 5000u));
    assert(rig.harness.countEvents(100u,
               MdkrMatchPeerMeshEventType::PeerLost) == 0u);

    /* STRUCTURAL garbage on the reliable channel -- a frame no conforming
     * peer can ever produce -- is terminal. */
    assert(rig.raw->sendControlBytes(runt));
    assert(rig.harness.pumpUntil([&]() {
        return rig.harness.countEvents(100u,
                   MdkrMatchPeerMeshEventType::PeerLost, 200u) >= 1u;
    }, 5000u));
    const MdkrMatchPeerMeshEvent *lost = rig.harness.lastEvent(
        100u, MdkrMatchPeerMeshEventType::PeerLost, 200u);
    assert(lost != nullptr && lost->lostReason ==
           MdkrMatchPeerLostReason::ControlChannelViolation);
    std::printf("badStateEnvelopeDroppedAndCounted: ok (state=%llu "
                "control=%llu)\n",
                static_cast<unsigned long long>(
                    rig.mesh->stats().rejectedStateEnvelopes),
                static_cast<unsigned long long>(
                    rig.mesh->stats().rejectedControlEnvelopes));
}

void controlPingTimeoutIsTypedPeerLoss() {
    RawHarness rig;
    const std::vector<MdkrMatchPeerSlotOwner> roster = rosterOf({100u, 200u});
    rig.mesh = rig.harness.add(100u, 1u, roster);
    rig.rawFeed = rig.harness.hub.addEndpoint(200u, 2u);
    rig.raw = std::make_unique<RawPeer>(200u, 2u, 100u, 1u, &rig.harness.hub);
    rig.raw->sendHellos = false;   /* keys are irrelevant to the ping ladder */
    rig.raw->answerPings = false;  /* the peer goes silent */
    rig.harness.hub.welcome(100u);
    assert(rig.harness.pumpUntil([&]() {
        rig.drainRawInbox();
        return rig.harness.countEvents(100u,
                   MdkrMatchPeerMeshEventType::PeerChannelsReady, 200u) >= 1u &&
               rig.raw->channelsOpen();
    }));

    /* Advance the fake clock past one ping interval, then past the stale
     * deadline with the pong never arriving. */
    rig.harness.clock.nowMs += kMdkrMatchControlPingIntervalMs + 1u;
    rig.harness.pumpOnce();
    rig.harness.clock.nowMs += kMdkrMatchControlPingTimeoutMs + 1u;
    assert(rig.harness.pumpUntil([&]() {
        return rig.harness.countEvents(100u,
                   MdkrMatchPeerMeshEventType::PeerLost, 200u) >= 1u;
    }, 5000u));
    const MdkrMatchPeerMeshEvent *lost = rig.harness.lastEvent(
        100u, MdkrMatchPeerMeshEventType::PeerLost, 200u);
    assert(lost != nullptr &&
           lost->lostReason == MdkrMatchPeerLostReason::PingTimeout);
    std::printf("controlPingTimeoutIsTypedPeerLoss: ok\n");
}

void iceRestartRecoversAfterChannelDeath() {
    PairHarness pair;
    assert(pair.connect());
    const unsigned readyLowBefore = pair.harness.countEvents(
        100u, MdkrMatchPeerMeshEventType::PeerChannelsReady, 200u);
    const unsigned readyHighBefore = pair.harness.countEvents(
        200u, MdkrMatchPeerMeshEventType::PeerChannelsReady, 100u);

    /* Kill the pair's connection out from under the offerer. */
    assert(mdkr_match_peer_mesh_kill_channels_for_test(*pair.low, 200u));
    assert(pair.harness.pumpUntil([&]() {
        return pair.harness.countEvents(100u,
                   MdkrMatchPeerMeshEventType::PeerChannelsReady, 200u) >
                   readyLowBefore &&
               pair.harness.countEvents(200u,
                   MdkrMatchPeerMeshEventType::PeerChannelsReady, 100u) >
                   readyHighBefore;
    }, 30000u));
    /* The restart was announced on the wire, the way the signaling doc's
     * peer_end contract requires. */
    assert(pair.harness.hub.countSent(100u, "peer_end", "restart") >= 1u);

    /* And the same generation-bound keys still carry input after recovery. */
    const auto payload = payloadFixture(0x59u);
    assert(pair.low->sendInput(payload.data()) == 1u);
    assert(pair.harness.pumpUntil([&]() {
        return pair.harness.countEvents(200u,
                   MdkrMatchPeerMeshEventType::InputEnvelope, 100u) >= 1u;
    }));
    std::printf("iceRestartRecoversAfterChannelDeath: ok\n");
}

void offerLadderGivesUpBounded() {
    MeshHarness harness;
    const std::vector<MdkrMatchPeerSlotOwner> roster = rosterOf({100u, 200u});
    harness.add(100u, 1u, roster);
    harness.hub.addEndpoint(200u, 7u);
    harness.hub.blackhole(200u, true); /* every offer vanishes, like a peer
                                        * whose socket dropped mid-relay */
    harness.hub.welcome(100u);
    /* Fake time marches; the mdkr_party_retry_decide ladder must recreate
     * on the 20 s deadline and give up after 3 attempts -- typed, bounded,
     * never a silent hang. */
    assert(harness.pumpUntil([&]() {
        return harness.countEvents(100u,
                   MdkrMatchPeerMeshEventType::PeerLost, 200u) >= 1u;
    }, 30000u, 1000u));
    const MdkrMatchPeerMeshEvent *lost = harness.lastEvent(
        100u, MdkrMatchPeerMeshEventType::PeerLost, 200u);
    assert(lost != nullptr &&
           lost->lostReason == MdkrMatchPeerLostReason::ConnectTimeout);
    assert(harness.hub.countSent(100u, "webrtc_offer") >= 3u);
    /* Given up means given up: no more offers after the verdict. */
    const unsigned offersAtVerdict = harness.hub.countSent(100u, "webrtc_offer");
    harness.clock.nowMs += 60000u;
    for (unsigned index = 0u; index < 5u; index++) harness.pumpOnce();
    assert(harness.hub.countSent(100u, "webrtc_offer") == offersAtVerdict);
    std::printf("offerLadderGivesUpBounded: ok (offers=%u)\n", offersAtVerdict);
}

void closeTearsDownBounded() {
    PairHarness pair;
    assert(pair.connect());
    const auto begin = std::chrono::steady_clock::now();
    pair.low->close();
    pair.low->close(); /* idempotent */
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - begin);
    assert(elapsed.count() < 5000);
    /* A closed mesh refuses work instead of crashing or hanging. */
    const auto payload = payloadFixture(0x01u);
    assert(pair.low->sendInput(payload.data()) == 0u);
    std::string phrase;
    assert(!pair.low->phrase(phrase));
    pair.low->pump(); /* harmless */
    /* The survivor keeps running (its own pump never hangs on the corpse). */
    for (unsigned index = 0u; index < 20u; index++) {
        pair.harness.pumpOnce();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    std::printf("closeTearsDownBounded: ok (%lld ms)\n",
                static_cast<long long>(elapsed.count()));
}

/* ---- Fix round 1 ---------------------------------------------------------*/

/* C1: a roster peer must never be able to throw through pump(). Every one
 * of these frames used to reach nlohmann typed extraction with a mismatched
 * type and terminate the launcher via json::type_error. */
void malformedControlJsonIsTypedLossNotCrash() {
    RawHarness rig;
    const std::vector<MdkrMatchPeerSlotOwner> roster = rosterOf({100u, 200u});
    rig.mesh = rig.harness.add(100u, 1u, roster);
    rig.rawFeed = rig.harness.hub.addEndpoint(200u, 2u);
    rig.raw = std::make_unique<RawPeer>(200u, 2u, 100u, 1u, &rig.harness.hub);
    rig.raw->sendHellos = false; /* the control plane predates the keys */
    rig.harness.hub.welcome(100u);
    assert(rig.harness.pumpUntil([&]() {
        rig.drainRawInbox();
        return rig.harness.countEvents(100u,
                   MdkrMatchPeerMeshEventType::PeerChannelsReady, 200u) >= 1u &&
               rig.raw->channelsOpen();
    }));
    assert(rig.raw->sendControlText("{\"type\":123}"));
    assert(rig.raw->sendControlText(
        "{\"type\":\"ping\",\"protocol\":true,\"nonce\":1}"));
    assert(rig.raw->sendControlText(
        "{\"type\":\"pong\",\"protocol\":1,\"nonce\":\"x\"}"));
    assert(rig.harness.pumpUntil([&]() {
        return rig.harness.countEvents(100u,
                   MdkrMatchPeerMeshEventType::PeerLost, 200u) >= 1u;
    }, 5000u));
    const MdkrMatchPeerMeshEvent *lost = rig.harness.lastEvent(
        100u, MdkrMatchPeerMeshEventType::PeerLost, 200u);
    assert(lost != nullptr && lost->lostReason ==
           MdkrMatchPeerLostReason::ControlChannelViolation);
    /* The mesh survives its peer's garbage: still pumpable, still typed. */
    (void)rig.mesh->stats();
    rig.harness.pumpOnce();
    std::printf("malformedControlJsonIsTypedLossNotCrash: ok\n");
}

/* I1: state and control are independent streams; one shared 64-deep replay
 * window let >64 state envelopes retire a delayed-but-honest control
 * fragment as REPLAY -- a terminal kill of a healthy peer. */
void delayedControlFragmentSurvivesInputBurst() {
    RawHarness rig;
    const std::vector<MdkrMatchPeerSlotOwner> roster = rosterOf({100u, 200u});
    rig.mesh = rig.harness.add(100u, 1u, roster);
    rig.rawFeed = rig.harness.hub.addEndpoint(200u, 2u);
    rig.raw = std::make_unique<RawPeer>(200u, 2u, 100u, 1u, &rig.harness.hub);
    rig.harness.hub.welcome(100u);
    assert(rig.harness.pumpUntil([&]() {
        rig.drainRawInbox();
        rig.raw->serviceControl();
        return rig.harness.countEvents(100u,
                   MdkrMatchPeerMeshEventType::PeerChannelsReady, 200u) >= 1u &&
               rig.raw->channelsOpen() && rig.raw->meshHellos >= 3u;
    }));
    rig.raw->deriveKeys();

    /* The fragment is sealed FIRST (sequence 1), then delayed while 70
     * inputs (sequences 2..71) advance the direction. */
    const auto fragment = payloadFixture(0x66u);
    const std::vector<uint8_t> delayed = rig.raw->sealedFragment(fragment);
    for (unsigned index = 0u; index < 70u; index++) {
        assert(rig.raw->sendStateBytes(rig.raw->sealedInput(
            payloadFixture(static_cast<uint8_t>(index)))));
    }
    assert(rig.harness.pumpUntil([&]() {
        rig.raw->serviceControl();
        return rig.harness.countEvents(100u,
                   MdkrMatchPeerMeshEventType::InputEnvelope, 200u) >= 65u;
    }));
    assert(rig.raw->sendControlBytes(delayed));
    assert(rig.harness.pumpUntil([&]() {
        rig.raw->serviceControl();
        return rig.harness.countEvents(100u,
                   MdkrMatchPeerMeshEventType::PreflightFragment, 200u) >= 1u;
    }, 5000u));
    const MdkrMatchPeerMeshEvent *preflight = rig.harness.lastEvent(
        100u, MdkrMatchPeerMeshEventType::PreflightFragment, 200u);
    assert(preflight != nullptr && preflight->payload == fragment);
    assert(rig.harness.countEvents(100u,
               MdkrMatchPeerMeshEventType::PeerLost) == 0u);
    std::printf("delayedControlFragmentSurvivesInputBurst: ok\n");
}

/* Hello-only stand-in for a reconnected endpoint: one identity, the
 * three-hello dance with several targets, no WebRTC (the phrase never
 * needed channels). */
struct HelloDriver {
    struct Target {
        uint64_t id = 0u;
        uint32_t generation = 0u;
        unsigned received = 0u;
        bool committed = false;
        bool revealed = false;
    };

    uint64_t selfId;
    FakeHub *hub;
    FakeFeed *feed;
    MdkrMatchPeerIdentity *identity = nullptr;
    std::array<uint8_t, MDKR_MATCH_PEER_PUBLIC_KEY_BYTES> publicKey{};
    std::array<uint8_t, MDKR_MATCH_PEER_COMMIT_NONCE_BYTES> nonce{};
    std::array<uint8_t, MDKR_MATCH_PEER_COMMIT_BYTES> commitment{};
    std::vector<Target> targets;

    HelloDriver(uint64_t self, uint32_t generation, FakeHub *owner,
                FakeFeed *ownFeed,
                const std::vector<std::pair<uint64_t, uint32_t>> &list)
        : selfId(self), hub(owner), feed(ownFeed) {
        identity = mdkr_match_peer_identity_create();
        assert(identity != nullptr);
        assert(mdkr_match_peer_identity_public_key(identity, publicKey.data()));
        for (unsigned index = 0u; index < nonce.size(); index++) {
            nonce[index] = static_cast<uint8_t>(0x51u + index);
        }
        assert(mdkr_match_peer_commitment(kEpoch, selfId, generation,
                                          nonce.data(), publicKey.data(),
                                          commitment.data()));
        for (const auto &entry : list) {
            Target target;
            target.id = entry.first;
            target.generation = entry.second;
            targets.push_back(target);
        }
    }

    ~HelloDriver() { mdkr_match_peer_identity_destroy(identity); }

    std::string blob(const uint8_t body[32]) const {
        uint8_t raw[MDKR_MATCH_PEER_PUBLIC_KEY_BYTES] = {};
        raw[0] = 0x04u;
        std::memcpy(raw + 1u, body, 32u);
        return mdkr_party::base64Url(raw, sizeof(raw));
    }

    void sendHello(const Target &target, const std::string &publicKeyBlob) {
        MdkrMatchSignalOutbound message;
        message.type = "peer_hello";
        message.toEndpointId = std::to_string(target.id);
        message.toConnectionGeneration = target.generation;
        message.publicKey = publicKeyBlob;
        hub->route(selfId, message);
    }

    void pump() {
        for (Target &target : targets) {
            if (!target.committed) {
                sendHello(target, blob(commitment.data()));
                target.committed = true;
            }
        }
        std::vector<MdkrMatchSignalEvent> drained;
        feed->drainEvents(drained);
        for (const MdkrMatchSignalEvent &event : drained) {
            if (event.type != MdkrMatchSignalEventType::PeerHello) continue;
            uint64_t from = 0u;
            try { from = std::stoull(event.fromEndpointId); } catch (...) {}
            for (Target &target : targets) {
                if (target.id != from) continue;
                target.received++;
                if (!target.revealed && target.received >= 1u) {
                    sendHello(target, mdkr_party::base64Url(publicKey.data(),
                                                            publicKey.size()));
                    sendHello(target, blob(nonce.data()));
                    target.revealed = true;
                }
            }
        }
    }
};

/* I2: a generation bump for one peer retires every transcript-salted key;
 * an honest third peer's in-flight old-digest control fragment during that
 * window must be a counted drop, never a peer kill -- and the whole roster
 * must re-derive to one fresh phrase afterwards. */
void rekeyGenerationBumpKeepsHonestPeers() {
    MeshHarness harness;
    const std::vector<MdkrMatchPeerSlotOwner> roster =
        rosterOf({100u, 200u, 300u});
    MdkrMatchPeerMesh *meshA = harness.add(100u, 1u, roster);
    MdkrMatchPeerMesh *meshB = harness.add(200u, 2u, roster);
    MdkrMatchPeerMesh *meshC = harness.add(300u, 3u, roster);
    harness.hub.welcome(100u);
    harness.hub.welcome(200u);
    harness.hub.welcome(300u);
    assert(harness.pumpUntil([&]() {
        return harness.countEvents(100u,
                   MdkrMatchPeerMeshEventType::PhraseReady) >= 1u &&
               harness.countEvents(200u,
                   MdkrMatchPeerMeshEventType::PhraseReady) >= 1u &&
               harness.countEvents(300u,
                   MdkrMatchPeerMeshEventType::PhraseReady) >= 1u &&
               harness.countEvents(100u,
                   MdkrMatchPeerMeshEventType::PeerChannelsReady, 300u) >= 1u &&
               harness.countEvents(300u,
                   MdkrMatchPeerMeshEventType::PeerChannelsReady, 100u) >= 1u;
    }, 30000u));
    std::string phraseBefore;
    assert(meshA->phrase(phraseBefore));

    /* B's signaling socket reconnects: gone at generation 2, back at 5. */
    meshB->close();
    harness.hub.setGeneration(200u, 5u);
    MdkrMatchSignalEvent bump;
    bump.type = MdkrMatchSignalEventType::PeerPresence;
    bump.endpointId = "200";
    bump.connectionGeneration = 5u;
    bump.present = true;
    harness.hub.inject(100u, bump); /* A learns first: presence skew */
    harness.pumpOnce();

    /* C, still keyed on the old digest, has a control fragment in flight
     * toward A. A must drop it non-terminally. */
    const auto fragment = payloadFixture(0x2Au);
    assert(meshC->sendPreflightFragment(100u, fragment.data()));
    for (unsigned index = 0u; index < 30u; index++) {
        harness.pumpOnce();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    assert(harness.countEvents(100u, MdkrMatchPeerMeshEventType::PeerLost,
                               300u) == 0u);

    /* C learns of the bump; a fresh endpoint 200 completes the exchange at
     * generation 5 (hello-only: the phrase never needed its channels). */
    harness.hub.inject(300u, bump);
    harness.pumpOnce();
    FakeFeed *replacementFeed = harness.hub.addEndpoint(200u, 5u);
    HelloDriver replacement(200u, 5u, &harness.hub, replacementFeed,
                            {{100u, 1u}, {300u, 3u}});
    assert(harness.pumpUntil([&]() {
        replacement.pump();
        return harness.countEvents(100u,
                   MdkrMatchPeerMeshEventType::PhraseReady) >= 2u &&
               harness.countEvents(300u,
                   MdkrMatchPeerMeshEventType::PhraseReady) >= 2u;
    }));
    std::string phraseA;
    std::string phraseC;
    assert(meshA->phrase(phraseA));
    assert(meshC->phrase(phraseC));
    assert(!phraseA.empty() && phraseA == phraseC && phraseA != phraseBefore);

    /* The A<->C lane rides the re-derived keys immediately. */
    const auto payload = payloadFixture(0x77u);
    assert(meshA->sendInput(payload.data()) >= 1u);
    assert(harness.pumpUntil([&]() {
        return harness.countEvents(300u,
                   MdkrMatchPeerMeshEventType::InputEnvelope, 100u) >= 1u;
    }));
    assert(harness.countEvents(100u,
               MdkrMatchPeerMeshEventType::PeerLost) == 0u);
    assert(harness.countEvents(300u,
               MdkrMatchPeerMeshEventType::PeerLost) == 0u);
    std::printf("rekeyGenerationBumpKeepsHonestPeers: ok\n");
}

/* I3: the invented three-hello convention, pinned case by case. Nothing
 * malformed, duplicated, reordered or replayed may ever yield a phrase. */
void helloProtocolPinning() {
    const auto runCase =
        [](const char *name,
           const std::function<void(MeshHarness &, RawPeer &)> &drive,
           bool expectLost, MdkrMatchPeerLostReason expected) {
            MeshHarness harness;
            const std::vector<MdkrMatchPeerSlotOwner> roster =
                rosterOf({100u, 200u});
            MdkrMatchPeerMesh *mesh = harness.add(100u, 1u, roster);
            harness.hub.addEndpoint(200u, 2u);
            RawPeer raw(200u, 2u, 100u, 1u, &harness.hub);
            raw.sendHellos = false;
            harness.hub.welcome(100u);
            harness.pumpOnce();
            drive(harness, raw);
            for (unsigned index = 0u; index < 5u; index++) {
                harness.pumpOnce();
            }
            if (expectLost) {
                const MdkrMatchPeerMeshEvent *lost = harness.lastEvent(
                    100u, MdkrMatchPeerMeshEventType::PeerLost, 200u);
                assert(lost != nullptr && lost->lostReason == expected);
            } else {
                assert(harness.countEvents(
                           100u, MdkrMatchPeerMeshEventType::PeerLost) == 0u);
            }
            /* Fails closed: no phrase from unverified material, ever. */
            std::string phrase;
            assert(!mesh->phrase(phrase));
            assert(harness.countEvents(
                       100u, MdkrMatchPeerMeshEventType::PhraseReady) == 0u);
            std::printf("  helloProtocolPinning[%s]: ok\n", name);
        };

    runCase("fourth-hello", [](MeshHarness &, RawPeer &raw) {
        raw.sendCommitHello();
        raw.sendRevealHellos();
        raw.sendSignal("peer_hello", [&](MdkrMatchSignalOutbound &m) {
            m.publicKey = raw.helloBlob(raw.commitment.data());
        });
    }, true, MdkrMatchPeerLostReason::HelloViolation);

    runCase("duplicate-commit", [](MeshHarness &, RawPeer &raw) {
        raw.sendCommitHello();
        raw.sendSignal("peer_hello", [&](MdkrMatchSignalOutbound &m) {
            m.publicKey = raw.helloBlob(raw.commitment.data());
        });
        raw.sendSignal("peer_hello", [&](MdkrMatchSignalOutbound &m) {
            m.publicKey = mdkr_party::base64Url(raw.publicKey.data(),
                                                raw.publicKey.size());
        });
    }, true, MdkrMatchPeerLostReason::HelloViolation);

    runCase("out-of-order", [](MeshHarness &, RawPeer &raw) {
        /* The real key where the commit belongs: its tail is not zero. */
        raw.sendSignal("peer_hello", [&](MdkrMatchSignalOutbound &m) {
            m.publicKey = mdkr_party::base64Url(raw.publicKey.data(),
                                                raw.publicKey.size());
        });
    }, true, MdkrMatchPeerLostReason::HelloViolation);

    runCase("commit-mismatch", [](MeshHarness &, RawPeer &raw) {
        /* Committed to one key, revealed another (the grinding shape). */
        MdkrMatchPeerIdentity *other = mdkr_match_peer_identity_create();
        assert(other != nullptr);
        std::array<uint8_t, MDKR_MATCH_PEER_PUBLIC_KEY_BYTES> otherKey{};
        assert(mdkr_match_peer_identity_public_key(other, otherKey.data()));
        raw.sendCommitHello();
        raw.sendSignal("peer_hello", [&](MdkrMatchSignalOutbound &m) {
            m.publicKey = mdkr_party::base64Url(otherKey.data(),
                                                otherKey.size());
        });
        raw.sendSignal("peer_hello", [&](MdkrMatchSignalOutbound &m) {
            m.publicKey = raw.helloBlob(raw.nonce.data());
        });
        mdkr_match_peer_identity_destroy(other);
    }, true, MdkrMatchPeerLostReason::CommitmentMismatch);

    runCase("dirty-tail", [](MeshHarness &, RawPeer &raw) {
        uint8_t blob[MDKR_MATCH_PEER_PUBLIC_KEY_BYTES] = {};
        blob[0] = 0x04u;
        std::memcpy(blob + 1u, raw.commitment.data(), 32u);
        blob[64] = 0x01u; /* reserved bytes must be zero */
        raw.sendSignal("peer_hello", [&](MdkrMatchSignalOutbound &m) {
            m.publicKey = mdkr_party::base64Url(blob, sizeof(blob));
        });
    }, true, MdkrMatchPeerLostReason::HelloViolation);

    runCase("stale-generation-replay", [](MeshHarness &harness, RawPeer &raw) {
        /* A hello claiming a generation other than the tracked one is
         * ignored, before AND after a generation bump. */
        MdkrMatchSignalEvent stale;
        stale.type = MdkrMatchSignalEventType::PeerHello;
        stale.fromEndpointId = "200";
        stale.fromConnectionGeneration = 99u;
        stale.publicKey = raw.helloBlob(raw.commitment.data());
        harness.hub.inject(100u, stale);
        harness.pumpOnce();
        MdkrMatchSignalEvent bump;
        bump.type = MdkrMatchSignalEventType::PeerPresence;
        bump.endpointId = "200";
        bump.connectionGeneration = 3u;
        bump.present = true;
        harness.hub.inject(100u, bump);
        harness.pumpOnce();
        MdkrMatchSignalEvent replay;
        replay.type = MdkrMatchSignalEventType::PeerHello;
        replay.fromEndpointId = "200";
        replay.fromConnectionGeneration = 2u; /* the retired generation */
        replay.publicKey = raw.helloBlob(raw.commitment.data());
        harness.hub.inject(100u, replay);
        harness.pumpOnce();
        assert(harness.mesh(100u)->stats().ignoredStaleSignals >= 2u);
    }, false, MdkrMatchPeerLostReason::HelloViolation);

    std::printf("helloProtocolPinning: ok\n");
}

/* M3: a peer whose committed-and-opened key is not a curve point is that
 * peer's typed loss, not a whole-mesh failure. */
void offCurveRevealIsPerPeerLoss() {
    MeshHarness harness;
    const std::vector<MdkrMatchPeerSlotOwner> roster = rosterOf({100u, 200u});
    MdkrMatchPeerMesh *mesh = harness.add(100u, 1u, roster);
    harness.hub.addEndpoint(200u, 2u);
    RawPeer raw(200u, 2u, 100u, 1u, &harness.hub);
    raw.sendHellos = false;
    harness.hub.welcome(100u);
    harness.pumpOnce();

    /* Commit to garbage bytes, open the commitment honestly over them: the
     * commitment round passes, ECDH cannot. */
    uint8_t garbage[MDKR_MATCH_PEER_PUBLIC_KEY_BYTES];
    garbage[0] = 0x04u;
    for (unsigned index = 1u; index < sizeof(garbage); index++) {
        garbage[index] = static_cast<uint8_t>(0xB0u + index * 7u);
    }
    uint8_t commitment[MDKR_MATCH_PEER_COMMIT_BYTES];
    assert(mdkr_match_peer_commitment(kEpoch, 200u, 2u, raw.nonce.data(),
                                      garbage, commitment));
    raw.sendSignal("peer_hello", [&](MdkrMatchSignalOutbound &m) {
        m.publicKey = raw.helloBlob(commitment);
    });
    raw.sendSignal("peer_hello", [&](MdkrMatchSignalOutbound &m) {
        m.publicKey = mdkr_party::base64Url(garbage, sizeof(garbage));
    });
    raw.sendSignal("peer_hello", [&](MdkrMatchSignalOutbound &m) {
        m.publicKey = raw.helloBlob(raw.nonce.data());
    });
    assert(harness.pumpUntil([&]() {
        return harness.countEvents(100u,
                   MdkrMatchPeerMeshEventType::PeerLost, 200u) >= 1u;
    }, 5000u));
    const MdkrMatchPeerMeshEvent *lost = harness.lastEvent(
        100u, MdkrMatchPeerMeshEventType::PeerLost, 200u);
    assert(lost != nullptr &&
           lost->lostReason == MdkrMatchPeerLostReason::HelloViolation);
    /* Per-peer, not mesh-fatal: no Failure event, mesh stays serviceable. */
    assert(harness.countEvents(100u,
               MdkrMatchPeerMeshEventType::Failure) == 0u);
    std::string phrase;
    assert(!mesh->phrase(phrase));
    (void)mesh->stats();
    harness.pumpOnce();
    std::printf("offCurveRevealIsPerPeerLoss: ok\n");
}

/* M1: a refused opening-nonce hello (relay said peer_unavailable) must be
 * retried like hellos 1 and 2, not dead-end the exchange forever. */
void helloNonceSendFailureRetries() {
    RawHarness rig;
    const std::vector<MdkrMatchPeerSlotOwner> roster = rosterOf({100u, 200u});
    rig.mesh = rig.harness.add(100u, 1u, roster);
    rig.rawFeed = rig.harness.hub.addEndpoint(200u, 2u);
    rig.raw = std::make_unique<RawPeer>(200u, 2u, 100u, 1u, &rig.harness.hub);
    rig.harness.hub.failHelloSend(100u, 3u); /* the opening nonce, once */
    rig.harness.hub.welcome(100u);
    assert(rig.harness.pumpUntil([&]() {
        rig.drainRawInbox();
        rig.raw->serviceControl();
        return rig.raw->meshHellos >= 3u; /* the peer got all three */
    }, 10000u));
    std::printf("helloNonceSendFailureRetries: ok\n");
}

/* M4: an answerer whose offer never arrives must reach a typed, bounded
 * verdict instead of waiting forever. */
void answererSetupDeadlineBounded() {
    MeshHarness harness;
    const std::vector<MdkrMatchPeerSlotOwner> roster = rosterOf({200u, 300u});
    harness.add(300u, 3u, roster); /* higher id: pure answerer */
    harness.hub.addEndpoint(200u, 2u);
    harness.hub.welcome(300u);
    assert(harness.pumpUntil([&]() {
        return harness.countEvents(300u,
                   MdkrMatchPeerMeshEventType::PeerLost, 200u) >= 1u;
    }, 20000u, 5000u));
    const MdkrMatchPeerMeshEvent *lost = harness.lastEvent(
        300u, MdkrMatchPeerMeshEventType::PeerLost, 200u);
    assert(lost != nullptr &&
           lost->lostReason == MdkrMatchPeerLostReason::ConnectTimeout);
    std::printf("answererSetupDeadlineBounded: ok\n");
}

}  // namespace

int main() {
    twoPeerHappyPath();
    threePeerMeshEveryoneReachesEveryone();
    staleGenerationAnswerIgnored();
    phraseRefusedBeforeCommitmentCompletes();
    sealWindowExhaustionIsTypedPeerLoss();
    badStateEnvelopeDroppedAndCounted();
    controlPingTimeoutIsTypedPeerLoss();
    iceRestartRecoversAfterChannelDeath();
    offerLadderGivesUpBounded();
    closeTearsDownBounded();
    /* Fix round 1: C1, I1, I2, I3, M1, M3, M4. */
    malformedControlJsonIsTypedLossNotCrash();
    delayedControlFragmentSurvivesInputBurst();
    rekeyGenerationBumpKeepsHonestPeers();
    helloProtocolPinning();
    offCurveRevealIsPerPeerLoss();
    helloNonceSendFailureRetries();
    answererSetupDeadlineBounded();
    std::printf("all match_peer_transport cases passed\n");
    return 0;
}
