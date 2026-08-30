/*
 * O-T6 production MatchRoom transport (platform/online/match_live_transport):
 * connection-robustness cases from the W3 native connection review, driven
 * wire-level against a scriptable loopback fake of the MatchRoom service
 * (HTTP create/command + the /connect RFC 6455 state socket), the same
 * harness discipline as tests/match_signal_test_server.cpp.
 *
 * Pinned here:
 *  - N1: a transport-shaped /connect drop reconnects on a bounded ladder,
 *    re-subscribes with the same credential, and the lastRevision_ dedupe
 *    makes redelivered state idempotent; application closes (the worker
 *    contract's 4000-class host_closed / room_expired) stay terminal
 *    immediately and stop the ladder.
 *  - N3: a blackholed first address (TEST-NET-1) costs one per-address
 *    slice, never the whole connect budget.
 *  - N6b: the mesh signal backend replaces a dead /signal socket on the
 *    same bounded ladder and delivers the replacement's fresh welcome
 *    (a higher connection generation) through the SAME borrowed feed.
 *  - N6c: close() during a resolver stalled by a DNS outage returns
 *    promptly (deadline-bounded, abandoned resolve).
 */
#include "online/match_live_transport.h"

#include "match_signal_test_server.h"

/* Assert-driven test: NDEBUG would compile every check away. */
#undef NDEBUG

#include <nlohmann/json.hpp>

#include <mbedtls/base64.h>
#include <mbedtls/sha1.h>

#include <atomic>
#include <cassert>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using RoomSocket = SOCKET;
static const RoomSocket kBadRoomSocket = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
using RoomSocket = int;
static const RoomSocket kBadRoomSocket = -1;
#endif

namespace {

using Json = nlohmann::json;

uint64_t nowMs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

void closeRoomSocket(RoomSocket fd) {
    if (fd == kBadRoomSocket) return;
#ifdef _WIN32
    ::closesocket(fd);
#else
    ::close(fd);
#endif
}

void setRoomRecvTimeout(RoomSocket fd, unsigned ms) {
#ifdef _WIN32
    DWORD timeout = ms;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO,
                 reinterpret_cast<const char *>(&timeout), sizeof(timeout));
#else
    struct timeval timeout;
    timeout.tv_sec = ms / 1000u;
    timeout.tv_usec = static_cast<int>((ms % 1000u) * 1000u);
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO,
                 reinterpret_cast<const char *>(&timeout), sizeof(timeout));
#endif
}

bool sendAllRoom(RoomSocket fd, const std::string &bytes) {
    size_t sent = 0u;
    while (sent < bytes.size()) {
        const int wrote = static_cast<int>(::send(
            fd, bytes.data() + sent,
#ifdef _WIN32
            static_cast<int>(bytes.size() - sent),
#else
            bytes.size() - sent,
#endif
            0));
        if (wrote <= 0) return false;
        sent += static_cast<size_t>(wrote);
    }
    return true;
}

std::string wsAcceptKeyFor(const std::string &clientKey) {
    static const char kGuid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    const std::string joined = clientKey + kGuid;
    unsigned char digest[20];
    if (mbedtls_sha1(reinterpret_cast<const unsigned char *>(joined.data()),
                     joined.size(), digest) != 0) {
        return std::string{};
    }
    unsigned char encoded[64];
    size_t written = 0u;
    if (mbedtls_base64_encode(encoded, sizeof(encoded), &written, digest,
                              sizeof(digest)) != 0) {
        return std::string{};
    }
    return std::string(reinterpret_cast<const char *>(encoded), written);
}

std::string headerValueOf(const std::string &head, const std::string &name) {
    size_t start = head.find("\r\n");
    while (start != std::string::npos && start + 2u < head.size()) {
        start += 2u;
        size_t end = head.find("\r\n", start);
        if (end == std::string::npos) end = head.size();
        std::string line = head.substr(start, end - start);
        const size_t colon = line.find(':');
        if (colon != std::string::npos) {
            std::string key = line.substr(0u, colon);
            for (char &byte : key)
                byte = static_cast<char>(
                    std::tolower(static_cast<unsigned char>(byte)));
            if (key == name) {
                std::string value = line.substr(colon + 1u);
                while (!value.empty() &&
                       (value.front() == ' ' || value.front() == '\t'))
                    value.erase(0u, 1u);
                while (!value.empty() &&
                       (value.back() == ' ' || value.back() == '\t'))
                    value.pop_back();
                return value;
            }
        }
        start = end;
    }
    return std::string();
}

std::string serverFrame(uint8_t opcode, const std::string &payload) {
    std::string frame;
    frame.push_back(static_cast<char>(0x80u | opcode));
    if (payload.size() < 126u) {
        frame.push_back(static_cast<char>(payload.size()));
    } else if (payload.size() <= 0xffffu) {
        frame.push_back(static_cast<char>(126u));
        frame.push_back(static_cast<char>((payload.size() >> 8u) & 0xffu));
        frame.push_back(static_cast<char>(payload.size() & 0xffu));
    } else {
        frame.push_back(static_cast<char>(127u));
        for (int shift = 56; shift >= 0; shift -= 8) {
            frame.push_back(static_cast<char>(
                (static_cast<uint64_t>(payload.size()) >> shift) & 0xffu));
        }
    }
    frame += payload;
    return frame;
}

/* ---- Fixtures ------------------------------------------------------------ */

const char kCredential[] = "cred0123456789abcdefABCDEF0123456789abcdefX"; /* 43 */
const char kRoomId22[] = "AQIDBAUGBwgJCgsMDQ4PEA";                        /* 22 */

MdkrOnlineCompatibilityV1 compatibilityFixture() {
    MdkrOnlineCompatibilityV1 c;
    std::memset(&c, 0, sizeof(c));
    c.protocol_version = MDKR_ONLINE_PROTOCOL_VERSION;
    for (unsigned i = 0u; i < sizeof(c.build_id); ++i)
        c.build_id[i] = static_cast<uint8_t>(i + 1u);
    for (unsigned i = 0u; i < sizeof(c.gameplay_digest); ++i)
        c.gameplay_digest[i] = static_cast<uint8_t>(0x80u + i);
    c.rom_revision = 1u;
    c.cadence_hz = 30u;
    return c;
}

/* Serialize the worker's exact create/state shapes over a real reducer
 * lobby, so parseLobby + mdkr_online_lobby_valid accept every frame. */
Json lobbyJson(const MdkrOnlineLobby &l) {
    Json lobby;
    lobby["protocolVersion"] = l.protocol_version;
    lobby["revision"] = l.revision;
    lobby["matchEpoch"] = l.match_epoch;
    lobby["leaderGeneration"] = l.leader_generation;
    lobby["roomId"] = std::to_string(l.room_id);
    lobby["leaderEndpointId"] = std::to_string(l.leader_endpoint_id);
    lobby["phase"] = "lobby";
    Json compat;
    compat["protocolVersion"] = l.compatibility.protocol_version;
    compat["buildId"] = Json::array();
    for (unsigned i = 0u; i < sizeof(l.compatibility.build_id); ++i)
        compat["buildId"].push_back(l.compatibility.build_id[i]);
    compat["gameplayDigest"] = Json::array();
    for (unsigned i = 0u; i < sizeof(l.compatibility.gameplay_digest); ++i)
        compat["gameplayDigest"].push_back(l.compatibility.gameplay_digest[i]);
    compat["romRevision"] = l.compatibility.rom_revision;
    compat["cadenceHz"] = l.compatibility.cadence_hz;
    lobby["compatibility"] = compat;
    Json members = Json::array();
    for (unsigned i = 0u; i < MDKR_ONLINE_MAX_ENDPOINTS; ++i) {
        if (!l.members[i].occupied) continue;
        Json m;
        m["endpointId"] = std::to_string(l.members[i].endpoint_id);
        m["seatCount"] = l.members[i].seat_count;
        m["connected"] = l.members[i].connected;
        m["ready"] = l.members[i].ready;
        m["loaded"] = l.members[i].loaded;
        members.push_back(m);
    }
    lobby["members"] = members;
    Json seats = Json::array();
    for (unsigned i = 0u; i < MDKR_ONLINE_MAX_SEATS; ++i) {
        if (!l.seats[i].occupied) continue;
        Json s;
        s["endpointId"] = std::to_string(l.seats[i].endpoint_id);
        s["selectionRevision"] = l.seats[i].selection_revision;
        s["localIndex"] = l.seats[i].local_index;
        if (l.seats[i].vote_track == MDKR_ONLINE_NO_VOTE) s["voteTrack"] = nullptr;
        else s["voteTrack"] = l.seats[i].vote_track;
        if (l.seats[i].character_id == MDKR_ONLINE_NO_CHARACTER)
            s["characterId"] = nullptr;
        else s["characterId"] = l.seats[i].character_id;
        if (l.seats[i].vehicle_id == MDKR_ONLINE_NO_VEHICLE)
            s["vehicleId"] = nullptr;
        else s["vehicleId"] = l.seats[i].vehicle_id;
        seats.push_back(s);
    }
    lobby["seats"] = seats;
    lobby["selectedTrack"] = nullptr;
    lobby["selectedVehicleMask"] = l.selected_vehicle_mask;
    Json root;
    root["lobby"] = lobby;
    return root;
}

/* ---- Scriptable loopback MatchRoom fake ----------------------------------- */

struct MiniRoomState {
    std::mutex mutex;
    std::atomic<bool> stopping{false};
    RoomSocket listenFd = kBadRoomSocket;
    uint16_t port = 0u;
    std::thread acceptThread;
    std::vector<std::thread> workers;

    MdkrOnlineLobby lobby{};

    /* Current /connect socket (one at a time; a reconnect replaces it). */
    RoomSocket wsFd = kBadRoomSocket;
    unsigned upgradeCount = 0u;
    std::string lastWsHead;
    std::vector<std::string> commandBodies;
    /* When nonempty, served verbatim as every /command response body (the
     * misbehaving-service arm). */
    std::string commandResponseOverride;
};

class MiniRoomServer {
public:
    MiniRoomServer() : state_(std::make_shared<MiniRoomState>()) {
#ifdef _WIN32
        WSADATA data;
        WSAStartup(MAKEWORD(2, 2), &data);
#endif
        const MdkrOnlineCompatibilityV1 compat = compatibilityFixture();
        assert(mdkr_online_lobby_init(&state_->lobby, UINT64_C(0xABCDEF),
                                      UINT64_C(101), &compat, 1u));
    }
    ~MiniRoomServer() { stop(); }

    uint16_t port() const { return state_->port; }
    uint32_t baseRevision() const { return state_->lobby.revision; }

    bool start() {
        auto state = state_;
        state->listenFd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (state->listenFd == kBadRoomSocket) return false;
        int one = 1;
        ::setsockopt(state->listenFd, SOL_SOCKET, SO_REUSEADDR,
                     reinterpret_cast<const char *>(&one), sizeof(one));
        struct sockaddr_in address;
        std::memset(&address, 0, sizeof(address));
        address.sin_family = AF_INET;
        address.sin_port = 0u;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::bind(state->listenFd,
                   reinterpret_cast<struct sockaddr *>(&address),
                   sizeof(address)) != 0 ||
            ::listen(state->listenFd, 8) != 0) {
            closeRoomSocket(state->listenFd);
            state->listenFd = kBadRoomSocket;
            return false;
        }
        socklen_t length = sizeof(address);
        if (::getsockname(state->listenFd,
                          reinterpret_cast<struct sockaddr *>(&address),
                          &length) != 0) {
            return false;
        }
        state->port = ntohs(address.sin_port);
        setRoomRecvTimeout(state->listenFd, 100u);
        state->acceptThread = std::thread([state]() {
            while (!state->stopping) {
                fd_set readable;
                FD_ZERO(&readable);
                FD_SET(state->listenFd, &readable);
                struct timeval poll;
                poll.tv_sec = 0;
                poll.tv_usec = 100000;
                const int ready = ::select(
                    static_cast<int>(state->listenFd) + 1, &readable, nullptr,
                    nullptr, &poll);
                if (state->stopping) break;
                if (ready <= 0) continue;
                RoomSocket accepted =
                    ::accept(state->listenFd, nullptr, nullptr);
                if (accepted == kBadRoomSocket) continue;
                setRoomRecvTimeout(accepted, 100u);
                std::lock_guard<std::mutex> lock(state->mutex);
                state->workers.emplace_back(
                    [state, accepted]() { serveOne(state, accepted); });
            }
        });
        return true;
    }

    void stop() {
        auto state = state_;
        if (state->stopping.exchange(true)) return;
        if (state->acceptThread.joinable()) state->acceptThread.join();
        std::vector<std::thread> workers;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            workers.swap(state->workers);
            if (state->wsFd != kBadRoomSocket) {
#ifdef _WIN32
                ::shutdown(state->wsFd, SD_BOTH);
#else
                ::shutdown(state->wsFd, SHUT_RDWR);
#endif
            }
        }
        for (std::thread &worker : workers)
            if (worker.joinable()) worker.join();
        closeRoomSocket(state->listenFd);
        state->listenFd = kBadRoomSocket;
    }

    bool waitForUpgrades(unsigned count, unsigned budgetMs = 5000u) {
        const uint64_t deadline = nowMs() + budgetMs;
        while (nowMs() < deadline) {
            {
                std::lock_guard<std::mutex> lock(state_->mutex);
                if (state_->upgradeCount >= count) return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        std::lock_guard<std::mutex> lock(state_->mutex);
        return state_->upgradeCount >= count;
    }

    unsigned upgradeCount() const {
        std::lock_guard<std::mutex> lock(state_->mutex);
        return state_->upgradeCount;
    }

    bool sendStateRevision(uint32_t revision) {
        Json root;
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            MdkrOnlineLobby lobby = state_->lobby;
            lobby.revision = revision;
            root = lobbyJson(lobby);
        }
        return sendWsFrame(0x1u, root.dump());
    }

    bool sendWsClose(uint16_t code, const std::string &reason) {
        std::string payload;
        payload.push_back(static_cast<char>((code >> 8u) & 0xffu));
        payload.push_back(static_cast<char>(code & 0xffu));
        payload += reason;
        return sendWsFrame(0x8u, payload);
    }

    void setCommandResponse(const std::string &raw) {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->commandResponseOverride = raw;
    }

    void dropWs() {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (state_->wsFd != kBadRoomSocket) {
#ifdef _WIN32
            ::shutdown(state_->wsFd, SD_BOTH);
#else
            ::shutdown(state_->wsFd, SHUT_RDWR);
#endif
        }
    }

private:
    bool sendWsFrame(uint8_t opcode, const std::string &payload) {
        RoomSocket fd;
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            fd = state_->wsFd;
        }
        if (fd == kBadRoomSocket) return false;
        return sendAllRoom(fd, serverFrame(opcode, payload));
    }

    static void serveOne(std::shared_ptr<MiniRoomState> state, RoomSocket fd) {
        std::string head;
        while (!state->stopping &&
               head.find("\r\n\r\n") == std::string::npos &&
               head.size() < 65536u) {
            char chunk[4096];
            const int got =
                static_cast<int>(::recv(fd, chunk, sizeof(chunk), 0));
            if (got > 0) {
                head.append(chunk, static_cast<size_t>(got));
            } else if (got == 0) {
                break;
            }
#ifdef _WIN32
            else if (WSAGetLastError() != WSAETIMEDOUT) break;
#else
            else if (errno != EAGAIN && errno != EWOULDBLOCK) break;
#endif
        }
        const size_t marker = head.find("\r\n\r\n");
        if (marker == std::string::npos) {
            closeRoomSocket(fd);
            return;
        }
        std::string body = head.substr(marker + 4u);
        const std::string headOnly = head.substr(0u, marker + 2u);
        const std::string requestLine =
            headOnly.substr(0u, headOnly.find("\r\n"));
        const bool upgrade =
            headerValueOf(headOnly, "upgrade").find("websocket") !=
            std::string::npos;

        if (upgrade) {
            const std::string key =
                headerValueOf(headOnly, "sec-websocket-key");
            std::string response =
                "HTTP/1.1 101 Switching Protocols\r\n"
                "Upgrade: websocket\r\n"
                "Connection: Upgrade\r\n"
                "Sec-WebSocket-Accept: " + wsAcceptKeyFor(key) + "\r\n"
                "Sec-WebSocket-Protocol: gb-match-v1\r\n\r\n";
            if (!sendAllRoom(fd, response)) {
                closeRoomSocket(fd);
                return;
            }
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                state->wsFd = fd;
                state->upgradeCount++;
                state->lastWsHead = headOnly;
            }
            /* Hold the socket, consuming client control frames, until the
             * peer goes away or the harness shuts it down. */
            while (!state->stopping) {
                char chunk[4096];
                const int got =
                    static_cast<int>(::recv(fd, chunk, sizeof(chunk), 0));
                if (got > 0) continue; /* pongs/close echoes: discard */
                if (got == 0) break;
#ifdef _WIN32
                if (WSAGetLastError() != WSAETIMEDOUT) break;
#else
                if (errno != EAGAIN && errno != EWOULDBLOCK) break;
#endif
            }
            std::lock_guard<std::mutex> lock(state->mutex);
            if (state->wsFd == fd) state->wsFd = kBadRoomSocket;
            closeRoomSocket(fd);
            return;
        }

        /* Plain HTTP request: read the declared body, then answer. */
        size_t contentLength = 0u;
        {
            const std::string declared =
                headerValueOf(headOnly, "content-length");
            if (!declared.empty())
                contentLength = static_cast<size_t>(
                    std::strtoul(declared.c_str(), nullptr, 10));
        }
        while (!state->stopping && body.size() < contentLength) {
            char chunk[4096];
            const int got =
                static_cast<int>(::recv(fd, chunk, sizeof(chunk), 0));
            if (got > 0) body.append(chunk, static_cast<size_t>(got));
            else if (got == 0) break;
#ifdef _WIN32
            else if (WSAGetLastError() != WSAETIMEDOUT) break;
#else
            else if (errno != EAGAIN && errno != EWOULDBLOCK) break;
#endif
        }

        std::string status = "HTTP/1.1 200 OK";
        Json responseBody;
        if (requestLine.find("/api/match/create") != std::string::npos) {
            status = "HTTP/1.1 201 Created";
            std::lock_guard<std::mutex> lock(state->mutex);
            responseBody = lobbyJson(state->lobby);
            responseBody["roomId"] = kRoomId22;
            responseBody["credential"] = kCredential;
            responseBody["endpointId"] = "101";
            responseBody["fallbackCode"] = "123456";
            responseBody["inviteUrl"] = "/room/#match=abc";
            responseBody["iceServers"] = Json::array();
        } else if (requestLine.find("/command") != std::string::npos) {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->commandBodies.push_back(body);
            if (!state->commandResponseOverride.empty()) {
                const std::string payload = state->commandResponseOverride;
                std::string response = std::string(status) + "\r\n";
                response += "Content-Type: application/json\r\n";
                response += "Content-Length: " +
                            std::to_string(payload.size()) + "\r\n";
                response += "Connection: close\r\n\r\n";
                response += payload;
                (void)sendAllRoom(fd, response);
                closeRoomSocket(fd);
                return;
            }
            responseBody["accepted"] = true;
            responseBody["duplicate"] = false;
            responseBody["error"] = "ok";
            responseBody["revision"] = state->lobby.revision;
            responseBody["matchEpoch"] = state->lobby.match_epoch;
        } else {
            status = "HTTP/1.1 404 Not Found";
            responseBody["error"] = "not_found";
        }
        const std::string payload = responseBody.dump();
        std::string response = status + "\r\n";
        response += "Content-Type: application/json\r\n";
        response += "Content-Length: " + std::to_string(payload.size()) +
                    "\r\n";
        response += "Connection: close\r\n\r\n";
        response += payload;
        (void)sendAllRoom(fd, response);
        closeRoomSocket(fd);
    }

    std::shared_ptr<MiniRoomState> state_;
};

/* ---- Drive helpers -------------------------------------------------------- */

struct RoomRig {
    MiniRoomServer server;
    std::unique_ptr<MdkrOnlineRoomTransport> transport;
    std::vector<MdkrOnlineRoomEvent> events;

    RoomRig() {
        assert(server.start());
        std::string error;
        transport = mdkr_online_room_http_transport_create(
            "http://127.0.0.1:" + std::to_string(server.port()), &error);
        assert(transport != nullptr);
    }

    ~RoomRig() {
        if (transport) transport->close();
        server.stop();
    }

    /* Pump until `done(events)` or the budget elapses; keeps every event. */
    bool pumpUntil(const std::function<bool()> &done, unsigned budgetMs) {
        const uint64_t deadline = nowMs() + budgetMs;
        std::vector<MdkrOnlineRoomEvent> batch;
        while (nowMs() < deadline) {
            transport->pump(batch);
            for (MdkrOnlineRoomEvent &ev : batch)
                events.push_back(std::move(ev));
            if (done()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        transport->pump(batch);
        for (MdkrOnlineRoomEvent &ev : batch) events.push_back(std::move(ev));
        return done();
    }

    unsigned count(MdkrOnlineRoomEvent::Type type) const {
        unsigned total = 0u;
        for (const MdkrOnlineRoomEvent &ev : events)
            if (ev.type == type) total++;
        return total;
    }

    const MdkrOnlineRoomEvent *last(MdkrOnlineRoomEvent::Type type) const {
        for (auto it = events.rbegin(); it != events.rend(); ++it)
            if (it->type == type) return &*it;
        return nullptr;
    }

    bool bringUp() {
        if (!transport->beginCreate(compatibilityFixture(), 1u)) return false;
        return pumpUntil(
            [&]() { return count(MdkrOnlineRoomEvent::Type::Ready) >= 1u; },
            8000u);
    }
};

/* ---- Cases ---------------------------------------------------------------- */

void createHappyPathDeliversReadyAndState() {
    RoomRig rig;
    assert(rig.bringUp());
    const MdkrOnlineRoomEvent *ready =
        rig.last(MdkrOnlineRoomEvent::Type::Ready);
    assert(ready != nullptr);
    assert(ready->localEndpointId == 101u);
    assert(ready->roomId == kRoomId22);
    assert(ready->credential == kCredential);
    assert(ready->haveLobby);
    MdkrOnlineRoomHttpInvite invite;
    assert(mdkr_online_room_http_transport_invite(rig.transport.get(),
                                                  &invite));
    assert(invite.fallbackCode == "123456");
    assert(rig.server.waitForUpgrades(1u));
    const uint32_t base = rig.server.baseRevision();
    assert(rig.server.sendStateRevision(base + 1u));
    assert(rig.pumpUntil(
        [&]() {
            const MdkrOnlineRoomEvent *state =
                rig.last(MdkrOnlineRoomEvent::Type::State);
            return state != nullptr && state->lobby.revision == base + 1u;
        },
        5000u));
    std::fprintf(stderr, "createHappyPathDeliversReadyAndState: ok\n");
}

/* W3 N1: the /connect socket is the room's push channel; a transport-shaped
 * drop (Wi-Fi roam, NAT rebind) must reconnect on the bounded ladder and
 * re-subscribe -- NOT surface HOST_CLOSED and kill the room's service loop.
 * Redelivered state after the reconnect is deduped by lastRevision_. */
void wsDropReconnectsResubscribesAndDedupes() {
    RoomRig rig;
    assert(rig.bringUp());
    assert(rig.server.waitForUpgrades(1u));
    const uint32_t base = rig.server.baseRevision();
    assert(rig.server.sendStateRevision(base + 1u));
    assert(rig.pumpUntil(
        [&]() {
            const MdkrOnlineRoomEvent *state =
                rig.last(MdkrOnlineRoomEvent::Type::State);
            return state != nullptr && state->lobby.revision == base + 1u;
        },
        5000u));

    rig.server.dropWs();
    /* The drop must NOT become a terminal failure... */
    (void)rig.pumpUntil([]() { return false; }, 700u);
    assert(rig.count(MdkrOnlineRoomEvent::Type::Failure) == 0u);
    /* ...and the ladder must produce a fresh authenticated subscription. */
    assert(rig.server.waitForUpgrades(2u, 5000u));

    /* Redelivery of the revision we already hold is idempotent. */
    const unsigned statesBefore = rig.count(MdkrOnlineRoomEvent::Type::State);
    assert(rig.server.sendStateRevision(base + 1u));
    (void)rig.pumpUntil([]() { return false; }, 400u);
    assert(rig.count(MdkrOnlineRoomEvent::Type::State) == statesBefore);

    /* Fresh revisions flow again on the replacement socket. */
    assert(rig.server.sendStateRevision(base + 2u));
    assert(rig.pumpUntil(
        [&]() {
            const MdkrOnlineRoomEvent *state =
                rig.last(MdkrOnlineRoomEvent::Type::State);
            return state != nullptr && state->lobby.revision == base + 2u;
        },
        5000u));
    assert(rig.count(MdkrOnlineRoomEvent::Type::Failure) == 0u);
    std::fprintf(stderr, "wsDropReconnectsResubscribesAndDedupes: ok\n");
}

/* W3 N1: application-level closes are NOT transport blips. The worker
 * contract closes /connect with 4000 room_expired / host_closed (services/
 * party/src/match/match-room.ts); those must stay terminal immediately and
 * the ladder must not reopen a room the service just declared gone. */
void applicationCloseCodeStaysTerminal() {
    RoomRig rig;
    assert(rig.bringUp());
    assert(rig.server.waitForUpgrades(1u));
    assert(rig.server.sendWsClose(4000u, "room_expired"));
    assert(rig.pumpUntil(
        [&]() { return rig.count(MdkrOnlineRoomEvent::Type::Failure) >= 1u; },
        5000u));
    const MdkrOnlineRoomEvent *failure =
        rig.last(MdkrOnlineRoomEvent::Type::Failure);
    assert(failure != nullptr &&
           failure->failure == MDKR_ONLINE_VIEW_FAILURE_ROOM_EXPIRED);
    /* Terminal means terminal: no reconnect attempts follow. */
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    assert(rig.server.upgradeCount() == 1u);
    std::fprintf(stderr, "applicationCloseCodeStaysTerminal: ok\n");
}

/* W3 N3: the same per-address budget the signal client got -- a blackholed
 * first address (TEST-NET-1) costs min(3.5 s, remaining/left), and the
 * create still lands well inside the old 8 s single-address burn. */
void blackholedFirstAddressCreatesWithinBudget() {
    mdkr_online_room_transport_prepend_address_for_test("192.0.2.1", 9u);
    const uint64_t start = nowMs();
    {
        RoomRig rig;
        assert(rig.bringUp());
        const uint64_t elapsed = nowMs() - start;
        /* Pre-fix the blackhole ate the full 8 s connect budget before the
         * loopback address was tried (if it was tried at all). */
        assert(elapsed < 6500u);
        std::fprintf(stderr,
                     "blackholedFirstAddressCreatesWithinBudget: ok (%llu ms)\n",
                     static_cast<unsigned long long>(elapsed));
    }
    mdkr_online_room_transport_prepend_address_for_test(nullptr, 0u);
}

/* W3 N6c: close() joins the worker thread; a resolver stalled by a DNS
 * outage must not hold that join (and the launcher) hostage. */
void closeDuringResolverStallReturnsPromptly() {
    mdkr_online_room_transport_stall_resolver_for_test(3000u);
    {
        RoomRig rig;
        assert(rig.transport->beginCreate(compatibilityFixture(), 1u));
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        const uint64_t start = nowMs();
        rig.transport->close();
        const uint64_t elapsed = nowMs() - start;
        /* Pre-fix: the join blocked ~2.9 s behind the stalled resolve. */
        assert(elapsed < 1500u);
        std::fprintf(stderr,
                     "closeDuringResolverStallReturnsPromptly: ok (%llu ms)\n",
                     static_cast<unsigned long long>(elapsed));
    }
    mdkr_online_room_transport_stall_resolver_for_test(0u);
    /* Let the abandoned resolve run out before process teardown. */
    std::this_thread::sleep_for(std::chrono::milliseconds(3100));
}

/* W3 N7 regression (found by the online-wire fuzzer, exec #352448 of the
 * first smoke): a /command response whose "error" field is present but the
 * WRONG JSON type ({"error": 0}) made nlohmann's value() throw
 * type_error.302 -- an uncaught exception on the worker thread, i.e. a
 * remote-triggerable std::terminate of the launcher. Every wrong-typed
 * field in a command result must instead surface as the typed PROTOCOL
 * command error, with the transport alive afterwards. */
void malformedCommandResultIsTypedProtocolError() {
    RoomRig rig;
    assert(rig.bringUp());
    rig.server.setCommandResponse(
        "{\"accepted\": true, \"duplicate\": false, \"error\": 0}");
    MdkrOnlineCommand command;
    std::memset(&command, 0, sizeof(command));
    command.protocol_version = MDKR_ONLINE_PROTOCOL_VERSION;
    command.command_id = 1u;
    command.type = MDKR_ONLINE_SET_READY;
    command.value = 1u;
    assert(rig.transport->submitCommand(command));
    assert(rig.pumpUntil(
        [&]() {
            return rig.count(MdkrOnlineRoomEvent::Type::CommandResult) >= 1u;
        },
        5000u));
    const MdkrOnlineRoomEvent *result =
        rig.last(MdkrOnlineRoomEvent::Type::CommandResult);
    assert(result != nullptr);
    assert(!result->step.accepted);
    assert(result->step.error == MDKR_ONLINE_ERROR_PROTOCOL);
    /* Alive: a well-formed result still flows afterwards. */
    rig.server.setCommandResponse(std::string());
    command.command_id = 2u;
    assert(rig.transport->submitCommand(command));
    assert(rig.pumpUntil(
        [&]() {
            return rig.count(MdkrOnlineRoomEvent::Type::CommandResult) >= 2u;
        },
        5000u));
    assert(rig.last(MdkrOnlineRoomEvent::Type::CommandResult)->step.accepted);
    std::fprintf(stderr, "malformedCommandResultIsTypedProtocolError: ok\n");
}

/* W3 N6b: after the /signal socket dies, the mesh backend must replace it
 * (replacement sockets are first-class, docs/ref/match-signaling-v1.md) on
 * the bounded ladder and deliver the replacement's fresh welcome -- a HIGHER
 * connection generation -- through the SAME borrowed feed, so the mesh can
 * re-arm its recovery ladders. */
void meshBackendReplacesADeadSignalSocket() {
    MdkrMatchSignalTestServer server;
    assert(server.start());
    std::unique_ptr<MdkrOnlineMeshSignalBackend> backend =
        mdkr_online_mesh_signal_backend_create(
            "ws://127.0.0.1:" + std::to_string(server.port()));
    MdkrMatchPeerSignalFeed *feed = backend->beginSignaling(
        101u, 0u, kRoomId22, kCredential, {});
    assert(feed != nullptr);
    assert(server.waitForOpen());
    Json welcome{{"protocolVersion", 1},
                 {"type", "signal_welcome"},
                 {"endpointId", "101"},
                 {"connectionGeneration", 1u},
                 {"peers", Json::array()}};
    assert(server.sendText(welcome.dump()));
    std::vector<MdkrMatchSignalEvent> events;
    const auto drainUntil = [&](MdkrMatchSignalEventType type,
                                unsigned budgetMs) -> bool {
        const uint64_t deadline = nowMs() + budgetMs;
        std::vector<MdkrMatchSignalEvent> batch;
        while (nowMs() < deadline) {
            feed->drainEvents(batch);
            for (MdkrMatchSignalEvent &ev : batch)
                events.push_back(std::move(ev));
            for (const MdkrMatchSignalEvent &ev : events)
                if (ev.type == type) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return false;
    };
    assert(drainUntil(MdkrMatchSignalEventType::Welcome, 5000u));
    assert(events.back().connectionGeneration == 1u);

    /* Silent-ish death: abrupt drop of the signal socket. */
    events.clear();
    server.dropConnection();
    assert(drainUntil(MdkrMatchSignalEventType::Failure, 5000u));

    /* The backend's ladder replaces the socket (second upgrade). The ladder
     * advances inside drainEvents -- the launcher pump -- so keep pumping,
     * exactly as the mesh does every service(). */
    {
        const uint64_t deadline = nowMs() + 5000u;
        std::vector<MdkrMatchSignalEvent> batch;
        bool replaced = false;
        while (nowMs() < deadline) {
            feed->drainEvents(batch);
            for (MdkrMatchSignalEvent &ev : batch)
                events.push_back(std::move(ev));
            if (server.waitForUpgrades(2u, 10u)) {
                replaced = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        assert(replaced);
    }
    Json rewelcome{{"protocolVersion", 1},
                   {"type", "signal_welcome"},
                   {"endpointId", "101"},
                   {"connectionGeneration", 2u},
                   {"peers", Json::array()}};
    assert(server.sendText(rewelcome.dump()));
    /* ...and the fresh generation reaches the SAME feed the mesh borrows. */
    events.clear();
    assert(drainUntil(MdkrMatchSignalEventType::Welcome, 5000u));
    bool sawFreshGeneration = false;
    for (const MdkrMatchSignalEvent &ev : events) {
        if (ev.type == MdkrMatchSignalEventType::Welcome &&
            ev.connectionGeneration == 2u) {
            sawFreshGeneration = true;
        }
    }
    assert(sawFreshGeneration);
    backend->reset();
    server.stop();
    std::fprintf(stderr, "meshBackendReplacesADeadSignalSocket: ok\n");
}

/* Story gap #8: a mistyped 6-digit code must be distinguishable from a
 * genuinely expired invite wherever the SERVICE response allows, so the
 * launcher can say "check the digits" instead of sending the player to nag
 * the host for a fresh code. The failure VIEW routing (both -> the pinned
 * INVITE_EXPIRED recovery) is unchanged; only the detail is classified.
 * Driven through the exact shipped classifier via the for_test seam. */
void joinRefusalDetailDistinguishesMistypeFromExpiry() {
    /* Genuine TTL expiry: only a fresh code can fix it. */
    assert(mdkr_online_room_transport_classify_refusal_for_test(
               409, "{\"error\":\"invite_expired\"}") ==
           MDKR_ONLINE_ROOM_JOIN_REFUSAL_INVITE_EXPIRED);
    /* The mistype family: a code that matched no live room (worker 404 /
     * not_found), or resolved to a room whose digest it does not match
     * (invalid_invite), or the legacy invalid_code shape. Re-typing fixes
     * these; "expired" copy sends the player to the wrong remedy. */
    assert(mdkr_online_room_transport_classify_refusal_for_test(
               409, "{\"error\":\"invalid_invite\"}") ==
           MDKR_ONLINE_ROOM_JOIN_REFUSAL_CODE_INVALID);
    assert(mdkr_online_room_transport_classify_refusal_for_test(
               409, "{\"error\":\"invalid_code\"}") ==
           MDKR_ONLINE_ROOM_JOIN_REFUSAL_CODE_INVALID);
    assert(mdkr_online_room_transport_classify_refusal_for_test(
               404, "{\"error\":\"not_found\"}") ==
           MDKR_ONLINE_ROOM_JOIN_REFUSAL_CODE_INVALID);
    /* Non-invite refusals carry no detail (their own views already word
     * themselves). */
    assert(mdkr_online_room_transport_classify_refusal_for_test(
               409, "{\"error\":\"incompatible\"}") ==
           MDKR_ONLINE_ROOM_JOIN_REFUSAL_NONE);
    assert(mdkr_online_room_transport_classify_refusal_for_test(
               503, "{\"error\":\"service_budget_safe\"}") ==
           MDKR_ONLINE_ROOM_JOIN_REFUSAL_NONE);
    /* Malformed body: fail closed to no detail unless the 404 status alone
     * names the mistype family. */
    assert(mdkr_online_room_transport_classify_refusal_for_test(500, "!") ==
           MDKR_ONLINE_ROOM_JOIN_REFUSAL_NONE);
    std::fprintf(stderr,
                 "joinRefusalDetailDistinguishesMistypeFromExpiry: ok\n");
}

}  // namespace

int main(int argc, char **argv) {
#ifdef _WIN32
    _putenv_s("MDKR_INTERNAL_TEST_TOKEN", "mdkr64-party-e2e-v1");
#else
    setenv("MDKR_INTERNAL_TEST_TOKEN", "mdkr64-party-e2e-v1", 1);
#endif
    const struct {
        const char *name;
        void (*run)();
    } cases[] = {
        {"happy", createHappyPathDeliversReadyAndState},
        {"reconnect", wsDropReconnectsResubscribesAndDedupes},
        {"terminal4000", applicationCloseCodeStaysTerminal},
        {"blackhole", blackholedFirstAddressCreatesWithinBudget},
        {"stall", closeDuringResolverStallReturnsPromptly},
        {"badcommand", malformedCommandResultIsTypedProtocolError},
        {"backend", meshBackendReplacesADeadSignalSocket},
        {"refusaldetail", joinRefusalDetailDistinguishesMistypeFromExpiry},
    };
    for (const auto &c : cases) {
        if (argc > 1 && std::strcmp(argv[1], c.name) != 0) continue;
        c.run();
    }
    std::fprintf(stderr, "test_match_live_transport: all cases passed\n");
    return 0;
}
