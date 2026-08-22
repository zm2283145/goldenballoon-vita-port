#include "match_signal_test_server.h"

#include <mbedtls/base64.h>
#include <mbedtls/sha1.h>

#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <mutex>
#include <thread>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using HarnessSocket = SOCKET;
static const HarnessSocket kBadHarnessSocket = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
using HarnessSocket = int;
static const HarnessSocket kBadHarnessSocket = -1;
#endif

namespace {

void closeHarnessSocket(HarnessSocket fd) {
    if (fd == kBadHarnessSocket) return;
#ifdef _WIN32
    ::closesocket(fd);
#else
    ::close(fd);
#endif
}

void setRecvTimeout(HarnessSocket fd, unsigned ms) {
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

std::string acceptKeyFor(const std::string &clientKey) {
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

std::string loweredCopy(std::string value) {
    for (char &byte : value) {
        byte = static_cast<char>(std::tolower(static_cast<unsigned char>(byte)));
    }
    return value;
}

/* Header lookup in a recorded request head; lowercase name, trimmed value. */
bool headerValue(const std::string &head, const std::string &lowercaseName,
                 std::string &value) {
    size_t start = head.find("\r\n");
    while (start != std::string::npos && start + 2u < head.size()) {
        start += 2u;
        size_t end = head.find("\r\n", start);
        if (end == std::string::npos) end = head.size();
        const std::string line = head.substr(start, end - start);
        const size_t colon = line.find(':');
        if (colon != std::string::npos &&
            loweredCopy(line.substr(0u, colon)) == lowercaseName) {
            value = line.substr(colon + 1u);
            while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
                value.erase(0u, 1u);
            }
            while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
                value.pop_back();
            }
            return true;
        }
        start = end;
    }
    return false;
}

std::string buildServerFrame(uint8_t firstByte, bool maskBit,
                             const std::string &payload) {
    std::string frame;
    frame.push_back(static_cast<char>(firstByte));
    const uint8_t mask = maskBit ? 0x80u : 0x00u;
    if (payload.size() < 126u) {
        frame.push_back(static_cast<char>(mask | static_cast<uint8_t>(payload.size())));
    } else if (payload.size() <= 0xffffu) {
        frame.push_back(static_cast<char>(mask | 126u));
        frame.push_back(static_cast<char>((payload.size() >> 8u) & 0xffu));
        frame.push_back(static_cast<char>(payload.size() & 0xffu));
    } else {
        frame.push_back(static_cast<char>(mask | 127u));
        for (int shift = 56; shift >= 0; shift -= 8) {
            frame.push_back(static_cast<char>(
                (static_cast<uint64_t>(payload.size()) >> shift) & 0xffu));
        }
    }
    frame += payload;
    return frame;
}

} // namespace

struct MdkrMatchSignalTestServerState {
    std::mutex mutex;
    std::condition_variable condition;
    std::atomic<bool> stopping{false};

    HarnessSocket listenFd = kBadHarnessSocket;
    uint16_t boundPort = 0u;
    std::thread thread;

    /* Live connection (one at a time). Guarded by mutex. */
    HarnessSocket clientFd = kBadHarnessSocket;
    std::mutex writeMutex;

    bool upgraded = false;
    std::string requestHead;
    std::string requestPath;
    std::vector<std::string> offered;

    std::vector<std::string> texts;
    std::vector<std::string> pongs;
    std::atomic<bool> pauseReading{false};
    bool clientClosed = false;
    uint16_t closeCode = 0u;
    std::string closeReason;
    bool disconnected = false;
    bool sawUnmaskedClientFrame = false;

    bool waitUntil(const std::function<bool()> &done, unsigned budgetMs) {
        std::unique_lock<std::mutex> lock(mutex);
        return condition.wait_for(lock, std::chrono::milliseconds(budgetMs),
                                  [&]() { return done(); });
    }
};

MdkrMatchSignalTestServer::MdkrMatchSignalTestServer()
    : state_(std::make_shared<MdkrMatchSignalTestServerState>()) {
#ifdef _WIN32
    WSADATA data;
    WSAStartup(MAKEWORD(2, 2), &data);
#endif
}

MdkrMatchSignalTestServer::~MdkrMatchSignalTestServer() { stop(); }

uint16_t MdkrMatchSignalTestServer::port() const { return state_->boundPort; }

bool MdkrMatchSignalTestServer::start() {
    auto state = state_;
    state->stopping = false;
    state->listenFd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (state->listenFd == kBadHarnessSocket) return false;
    int one = 1;
    ::setsockopt(state->listenFd, SOL_SOCKET, SO_REUSEADDR,
                 reinterpret_cast<const char *>(&one), sizeof(one));
    struct sockaddr_in address;
    std::memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = 0u;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(state->listenFd, reinterpret_cast<struct sockaddr *>(&address),
               sizeof(address)) != 0 ||
        ::listen(state->listenFd, 4) != 0) {
        closeHarnessSocket(state->listenFd);
        state->listenFd = kBadHarnessSocket;
        return false;
    }
    socklen_t length = sizeof(address);
    if (::getsockname(state->listenFd,
                      reinterpret_cast<struct sockaddr *>(&address),
                      &length) != 0) {
        closeHarnessSocket(state->listenFd);
        state->listenFd = kBadHarnessSocket;
        return false;
    }
    state->boundPort = ntohs(address.sin_port);
    setRecvTimeout(state->listenFd, 100u);

    const ProtocolMode mode = protocolMode;
    const bool respond = respondToUpgrade;
    const std::string extraHeader = extra101Header;
    state->thread = std::thread([state, mode, respond, extraHeader]() {
        while (!state->stopping) {
            struct sockaddr_in peer;
            socklen_t peerLength = sizeof(peer);
            /* Bounded accept: poll via the listen socket's recv timeout. */
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
            HarnessSocket accepted = ::accept(
                state->listenFd, reinterpret_cast<struct sockaddr *>(&peer),
                &peerLength);
            if (accepted == kBadHarnessSocket) continue;
            setRecvTimeout(accepted, 100u);
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                state->clientFd = accepted;
            }

            /* ---- Read the upgrade request head ---- */
            std::string head;
            while (!state->stopping &&
                   head.find("\r\n\r\n") == std::string::npos &&
                   head.size() < 16384u) {
                char chunk[2048];
                const int got = static_cast<int>(
                    ::recv(accepted, chunk, sizeof(chunk), 0));
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
            if (head.find("\r\n\r\n") == std::string::npos) {
                std::lock_guard<std::mutex> lock(state->mutex);
                closeHarnessSocket(accepted);
                state->clientFd = kBadHarnessSocket;
                state->disconnected = true;
                state->condition.notify_all();
                continue;
            }

            std::string path;
            {
                const size_t space = head.find(' ');
                const size_t second =
                    space == std::string::npos ? std::string::npos
                                               : head.find(' ', space + 1u);
                if (space != std::string::npos && second != std::string::npos) {
                    path = head.substr(space + 1u, second - space - 1u);
                }
            }
            std::vector<std::string> offered;
            {
                std::string protocols;
                if (headerValue(head, "sec-websocket-protocol", protocols)) {
                    std::string token;
                    for (const char byte : protocols + ",") {
                        if (byte == ',') {
                            while (!token.empty() && token.front() == ' ') {
                                token.erase(0u, 1u);
                            }
                            while (!token.empty() && token.back() == ' ') {
                                token.pop_back();
                            }
                            if (!token.empty()) offered.push_back(token);
                            token.clear();
                        } else {
                            token.push_back(byte);
                        }
                    }
                }
            }
            std::string clientKey;
            headerValue(head, "sec-websocket-key", clientKey);
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                state->requestHead = head;
                state->requestPath = path;
                state->offered = offered;
                state->condition.notify_all();
            }

            if (!respond) {
                /* Stall: hold the socket silently until stop(). */
                while (!state->stopping) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                }
                std::lock_guard<std::mutex> lock(state->mutex);
                closeHarnessSocket(accepted);
                state->clientFd = kBadHarnessSocket;
                continue;
            }

            std::string selected;
            switch (mode) {
            case ProtocolMode::SelectV1:
                selected = "gb-match-signal-v1";
                break;
            case ProtocolMode::SelectNone:
                break;
            case ProtocolMode::SelectSecond:
                if (offered.size() >= 2u) selected = offered[1];
                break;
            case ProtocolMode::SelectUnoffered:
                selected = "gb-bogus-v9";
                break;
            }
            std::string response =
                "HTTP/1.1 101 Switching Protocols\r\n"
                "Upgrade: websocket\r\n"
                "Connection: Upgrade\r\n"
                "Sec-WebSocket-Accept: " + acceptKeyFor(clientKey) + "\r\n";
            if (!selected.empty()) {
                response += "Sec-WebSocket-Protocol: " + selected + "\r\n";
            }
            if (!extraHeader.empty()) {
                response += extraHeader + "\r\n";
            }
            response += "\r\n";
            {
                std::lock_guard<std::mutex> lock(state->writeMutex);
                (void)::send(accepted, response.data(),
#ifdef _WIN32
                             static_cast<int>(response.size()),
#else
                             response.size(),
#endif
                             0);
            }
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                state->upgraded = true;
                state->condition.notify_all();
            }

            /* ---- Client frame loop ---- */
            std::string carried;
            bool open = true;
            while (!state->stopping && open) {
                if (state->pauseReading) {
                    /* stopReading(): leave the client's bytes in the kernel
                     * buffers so its send side backs up. */
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                    continue;
                }
                /* Need at least a 2-byte header. */
                if (carried.size() < 2u) {
                    char chunk[4096];
                    const int got = static_cast<int>(
                        ::recv(accepted, chunk, sizeof(chunk), 0));
                    if (got > 0) {
                        carried.append(chunk, static_cast<size_t>(got));
                        continue;
                    }
                    if (got == 0) break;
#ifdef _WIN32
                    if (WSAGetLastError() == WSAETIMEDOUT) continue;
#else
                    if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
#endif
                    break;
                }
                const uint8_t byte0 = static_cast<uint8_t>(carried[0]);
                const uint8_t byte1 = static_cast<uint8_t>(carried[1]);
                const bool masked = (byte1 & 0x80u) != 0u;
                size_t headerSize = 2u;
                uint64_t length = byte1 & 0x7fu;
                if (length == 126u) headerSize = 4u;
                else if (length == 127u) headerSize = 10u;
                if (masked) headerSize += 4u;
                if (carried.size() < headerSize) {
                    char chunk[4096];
                    const int got = static_cast<int>(
                        ::recv(accepted, chunk, sizeof(chunk), 0));
                    if (got > 0) carried.append(chunk, static_cast<size_t>(got));
                    else if (got == 0) break;
#ifdef _WIN32
                    else if (WSAGetLastError() != WSAETIMEDOUT) break;
#else
                    else if (errno != EAGAIN && errno != EWOULDBLOCK) break;
#endif
                    continue;
                }
                if ((byte1 & 0x7fu) == 126u) {
                    length = (static_cast<uint64_t>(
                                  static_cast<uint8_t>(carried[2])) << 8u) |
                             static_cast<uint64_t>(static_cast<uint8_t>(carried[3]));
                } else if ((byte1 & 0x7fu) == 127u) {
                    length = 0u;
                    for (unsigned index = 0u; index < 8u; index++) {
                        length = (length << 8u) |
                                 static_cast<uint8_t>(carried[2u + index]);
                    }
                }
                if (length > (1u << 20u)) break; /* harness sanity bound */
                if (carried.size() < headerSize + length) {
                    char chunk[8192];
                    const int got = static_cast<int>(
                        ::recv(accepted, chunk, sizeof(chunk), 0));
                    if (got > 0) carried.append(chunk, static_cast<size_t>(got));
                    else if (got == 0) break;
#ifdef _WIN32
                    else if (WSAGetLastError() != WSAETIMEDOUT) break;
#else
                    else if (errno != EAGAIN && errno != EWOULDBLOCK) break;
#endif
                    continue;
                }
                uint8_t mask[4] = {0u, 0u, 0u, 0u};
                if (masked) {
                    for (unsigned index = 0u; index < 4u; index++) {
                        mask[index] = static_cast<uint8_t>(
                            carried[headerSize - 4u + index]);
                    }
                }
                std::string payload = carried.substr(headerSize,
                                                     static_cast<size_t>(length));
                for (size_t index = 0u; index < payload.size(); index++) {
                    payload[index] = static_cast<char>(
                        static_cast<uint8_t>(payload[index]) ^ mask[index % 4u]);
                }
                carried.erase(0u, headerSize + static_cast<size_t>(length));
                const uint8_t opcode = byte0 & 0x0fu;
                std::lock_guard<std::mutex> lock(state->mutex);
                if (!masked) state->sawUnmaskedClientFrame = true;
                if (opcode == 0x1u) {
                    state->texts.push_back(std::move(payload));
                    state->condition.notify_all();
                } else if (opcode == 0x8u) {
                    state->clientClosed = true;
                    if (payload.size() >= 2u) {
                        state->closeCode = static_cast<uint16_t>(
                            (static_cast<uint16_t>(
                                 static_cast<uint8_t>(payload[0])) << 8u) |
                            static_cast<uint8_t>(payload[1]));
                        state->closeReason = payload.substr(2u);
                    }
                    state->condition.notify_all();
                    /* Echo the close per RFC, then fall out. */
                    const std::string echo = buildServerFrame(0x88u, false, payload);
                    {
                        std::lock_guard<std::mutex> writeLock(state->writeMutex);
                        (void)::send(accepted, echo.data(),
#ifdef _WIN32
                                     static_cast<int>(echo.size()),
#else
                                     echo.size(),
#endif
                                     0);
                    }
                    open = false;
                } else if (opcode == 0xau) {
                    state->pongs.push_back(std::move(payload));
                    state->condition.notify_all();
                }
                /* Client pings are ignored by the harness. */
            }
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                closeHarnessSocket(accepted);
                state->clientFd = kBadHarnessSocket;
                state->disconnected = true;
                state->condition.notify_all();
            }
        }
    });
    return true;
}

void MdkrMatchSignalTestServer::stop() {
    auto state = state_;
    if (state->stopping.exchange(true)) {
        if (state->thread.joinable()) state->thread.join();
        return;
    }
    if (state->thread.joinable()) state->thread.join();
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        closeHarnessSocket(state->clientFd);
        state->clientFd = kBadHarnessSocket;
    }
    closeHarnessSocket(state->listenFd);
    state->listenFd = kBadHarnessSocket;
    state->boundPort = 0u;
}

bool MdkrMatchSignalTestServer::waitForUpgrade(unsigned budgetMs) {
    auto state = state_;
    return state->waitUntil([&]() { return !state->requestHead.empty(); },
                            budgetMs);
}

bool MdkrMatchSignalTestServer::waitForOpen(unsigned budgetMs) {
    auto state = state_;
    return state->waitUntil([&]() { return state->upgraded; }, budgetMs);
}

std::string MdkrMatchSignalTestServer::requestHeadRaw() const {
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->requestHead;
}

std::string MdkrMatchSignalTestServer::requestPath() const {
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->requestPath;
}

std::vector<std::string> MdkrMatchSignalTestServer::offeredProtocols() const {
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->offered;
}

bool MdkrMatchSignalTestServer::sawHeader(
    const std::string &lowercaseName) const {
    std::lock_guard<std::mutex> lock(state_->mutex);
    std::string value;
    return headerValue(state_->requestHead, lowercaseName, value);
}

bool MdkrMatchSignalTestServer::sendText(const std::string &payload) {
    return sendRawFrame(0x81u, false, payload);
}

bool MdkrMatchSignalTestServer::sendBinary(const std::string &payload) {
    return sendRawFrame(0x82u, false, payload);
}

bool MdkrMatchSignalTestServer::sendPing(const std::string &payload) {
    return sendRawFrame(0x89u, false, payload);
}

bool MdkrMatchSignalTestServer::sendClose(uint16_t code,
                                          const std::string &reason) {
    std::string payload;
    payload.push_back(static_cast<char>((code >> 8u) & 0xffu));
    payload.push_back(static_cast<char>(code & 0xffu));
    payload += reason;
    return sendRawFrame(0x88u, false, payload);
}

bool MdkrMatchSignalTestServer::sendRawFrame(uint8_t firstByte, bool maskBit,
                                             const std::string &payload) {
    auto state = state_;
    HarnessSocket fd;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        fd = state->clientFd;
    }
    if (fd == kBadHarnessSocket) return false;
    const std::string frame = buildServerFrame(firstByte, maskBit, payload);
    std::lock_guard<std::mutex> lock(state->writeMutex);
    size_t sent = 0u;
    while (sent < frame.size()) {
        const int wrote = static_cast<int>(::send(
            fd, frame.data() + sent,
#ifdef _WIN32
            static_cast<int>(frame.size() - sent),
#else
            frame.size() - sent,
#endif
            0));
        if (wrote <= 0) return false;
        sent += static_cast<size_t>(wrote);
    }
    return true;
}

void MdkrMatchSignalTestServer::dropConnection() {
    auto state = state_;
    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->clientFd != kBadHarnessSocket) {
#ifdef _WIN32
        ::shutdown(state->clientFd, SD_BOTH);
#else
        ::shutdown(state->clientFd, SHUT_RDWR);
#endif
    }
}

bool MdkrMatchSignalTestServer::waitForTextMessages(size_t count,
                                                    unsigned budgetMs) {
    auto state = state_;
    return state->waitUntil([&]() { return state->texts.size() >= count; },
                            budgetMs);
}

std::vector<std::string> MdkrMatchSignalTestServer::textMessages() const {
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->texts;
}

bool MdkrMatchSignalTestServer::waitForClientClose(unsigned budgetMs) {
    auto state = state_;
    return state->waitUntil([&]() { return state->clientClosed; }, budgetMs);
}

uint16_t MdkrMatchSignalTestServer::clientCloseCode() const {
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->closeCode;
}

std::string MdkrMatchSignalTestServer::clientCloseReason() const {
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->closeReason;
}

bool MdkrMatchSignalTestServer::waitForDisconnect(unsigned budgetMs) {
    auto state = state_;
    return state->waitUntil([&]() { return state->disconnected; }, budgetMs);
}

bool MdkrMatchSignalTestServer::allClientFramesMasked() const {
    std::lock_guard<std::mutex> lock(state_->mutex);
    return !state_->sawUnmaskedClientFrame;
}

void MdkrMatchSignalTestServer::stopReading() {
    state_->pauseReading = true;
}

bool MdkrMatchSignalTestServer::waitForPongs(size_t count, unsigned budgetMs) {
    auto state = state_;
    return state->waitUntil([&]() { return state->pongs.size() >= count; },
                            budgetMs);
}

std::vector<std::string> MdkrMatchSignalTestServer::pongPayloads() const {
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->pongs;
}
