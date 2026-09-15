#include "lan_party_server.h"
#include "net/native_socket_lifetime.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using SocketHandle = SOCKET;
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
using SocketHandle = int;
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <new>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#ifdef MDKR_LAN_PARTY_TESTING
void (*mdkr_lan_party_test_send_hook)() = nullptr;
unsigned mdkr_lan_party_test_http_deadline_ms = 0u;
unsigned mdkr_lan_party_test_ws_idle_deadline_ms = 0u;
std::atomic<MdkrLanPartyTestFailure> mdkr_lan_party_test_failure{MdkrLanPartyTestFailure::None};
std::atomic<unsigned> mdkr_lan_party_test_failures_observed{0u};
std::atomic<unsigned> mdkr_lan_party_test_live_connections{0u};
#endif

namespace {

using Clock = std::chrono::steady_clock;

#ifdef MDKR_LAN_PARTY_TESTING
bool takeFailure(MdkrLanPartyTestFailure stage) {
    if (mdkr_lan_party_test_failure.compare_exchange_strong(stage, MdkrLanPartyTestFailure::None)) {
        ++mdkr_lan_party_test_failures_observed;
        return true;
    }
    return false;
}

void failAt(MdkrLanPartyTestFailure stage) {
    if (takeFailure(stage)) {
        if (stage == MdkrLanPartyTestFailure::StartThread || stage == MdkrLanPartyTestFailure::AcceptThread) {
            throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again));
        }
        throw std::bad_alloc();
    }
}
#endif

#ifdef _WIN32
constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;
#else
constexpr SocketHandle kInvalidSocket = -1;
#endif

/*
 * Bounds. The request-line bound exists so an attacker cannot feed this
 * server an unbounded first line; past it the connection is CUT, with no
 * status line that could serve as a parser oracle. The header bound caps
 * one whole request head; the per-connection request cap and the idle
 * deadline bound how long a keep-alive connection may hold its thread.
 */
constexpr size_t kMaxRequestLineBytes = 2048u;
constexpr size_t kMaxRequestHeaderBytes = 8192u;
constexpr size_t kMaxRequestsPerConnection = 128u;
constexpr size_t kMaxConnections = 32u;
constexpr unsigned kRecvPollMs = 250u;
constexpr unsigned kSendTimeoutMs = 5000u;
constexpr unsigned kAcceptPollMs = 200u;
constexpr unsigned kHttpIdleDeadlineMs = 15000u;

unsigned httpDeadlineMs() {
#ifdef MDKR_LAN_PARTY_TESTING
    if (mdkr_lan_party_test_http_deadline_ms != 0u) {
        return mdkr_lan_party_test_http_deadline_ms;
    }
#endif
    return kHttpIdleDeadlineMs;
}

/* Post-upgrade idle reaper: the pre-upgrade HTTP deadline governs only the
 * handshake, so an upgraded /party-ws could otherwise block on recv forever and
 * a hostile LAN device could silently hold all kMaxConnections slots. After half
 * this window of inbound silence the server sends a WebSocket ping (a browser
 * pongs automatically, so a real -- even approval-waiting -- phone keeps the
 * socket alive); after the full window with no inbound activity at all the socket
 * is reaped. Generous by design: it bounds a silent hold, not a live session. */
constexpr unsigned kWsIdleDeadlineMs = 45000u;

unsigned wsIdleDeadlineMs() {
#ifdef MDKR_LAN_PARTY_TESTING
    if (mdkr_lan_party_test_ws_idle_deadline_ms != 0u) {
        return mdkr_lan_party_test_ws_idle_deadline_ms;
    }
#endif
    return kWsIdleDeadlineMs;
}
/* After sending a close frame, how long the reader lingers to absorb the
 * peer's in-flight bytes so the close is delivered on a FIN, not lost to a
 * reset from closing with unread data queued. */
constexpr unsigned kCloseDrainMs = 500u;

const char kWsPath[] = "/party-ws";
/* RFC 6455 section 4.2.2: fixed handshake GUID, not a secret. */
const char kWsGuid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

struct ResponseHeader {
    const char *name;
    const char *value;
};

/*
 * THE header table: every HTTP response this server writes (200 and
 * refusal alike) carries exactly these, so there is no path that forgets
 * them. Reference: dist/web/_headers, its "/controller/" scope -- the cloud
 * deploy of this same controller page. Deltas for the plain-HTTP LAN
 * transport, each deliberate:
 *   - connect-src gains ws:  -- the phone's control channel is same-origin
 *     plain ws on this very port; engines have not uniformly extended
 *     'self' to ws://, and the page's own code only ever dials its origin.
 *   - manifest-src 'self' (was 'none'): this server serves the packaged
 *     webmanifest from its own asset manifest.
 *   - upgrade-insecure-requests is dropped: on an http: origin it rewrites
 *     every same-origin subresource to https:, which this server never
 *     speaks -- certificates are off the product path by owner decision.
 *   - Strict-Transport-Security is dropped: RFC 6797 ignores it over
 *     insecure transport, and an HSTS entry cached for a bare LAN host
 *     would lock the browser out of the only scheme served here.
 *   - Cache-Control: no-store added so a rebuilt game never fights a
 *     phone's stale copy of the controller page.
 */
constexpr ResponseHeader kResponseHeaders[] = {
    {"Content-Security-Policy",
     "default-src 'self'; base-uri 'none'; object-src 'none'; "
     "frame-ancestors 'none'; form-action 'self'; script-src 'self'; "
     "style-src 'self'; img-src 'self'; connect-src 'self' ws:; "
     "font-src 'none'; media-src 'none'; worker-src 'none'; "
     "manifest-src 'self'"},
    {"Referrer-Policy", "no-referrer"},
    {"X-Content-Type-Options", "nosniff"},
    {"X-Frame-Options", "DENY"},
    {"Cross-Origin-Opener-Policy", "same-origin"},
    {"Cross-Origin-Resource-Policy", "same-origin"},
    {"Permissions-Policy",
     "camera=(), microphone=(), geolocation=(), payment=(), usb=(), "
     "serial=(), bluetooth=(), accelerometer=(), gyroscope=(), "
     "magnetometer=()"},
    {"Cache-Control", "no-store"},
};

/* ---- Socket shims -------------------------------------------------------- */

void closeSocket(SocketHandle fd) {
#ifdef _WIN32
    ::closesocket(fd);
#else
    ::close(fd);
#endif
}

struct SocketOwner {
    SocketHandle fd;
    explicit SocketOwner(SocketHandle value) noexcept : fd(value) {}
    SocketOwner(const SocketOwner &) = delete;
    SocketOwner &operator=(const SocketOwner &) = delete;
    ~SocketOwner() { if (fd != kInvalidSocket) closeSocket(fd); }
    SocketHandle release() noexcept { return std::exchange(fd, kInvalidSocket); }
};

void shutdownBoth(SocketHandle fd) {
#ifdef _WIN32
    ::shutdown(fd, SD_BOTH);
#else
    ::shutdown(fd, SHUT_RDWR);
#endif
}

void shutdownWrite(SocketHandle fd) {
#ifdef _WIN32
    ::shutdown(fd, SD_SEND);
#else
    ::shutdown(fd, SHUT_WR);
#endif
}

bool setSocketBlocking(SocketHandle fd, bool blocking) {
#ifdef _WIN32
    u_long nonblocking = blocking ? 0u : 1u;
    return ::ioctlsocket(fd, FIONBIO, &nonblocking) == 0;
#else
    const int flags = ::fcntl(fd, F_GETFL, 0);
    return flags >= 0 && ::fcntl(fd, F_SETFL,
        blocking ? (flags & ~O_NONBLOCK) : (flags | O_NONBLOCK)) == 0;
#endif
}

bool setSocketTimeouts(SocketHandle fd) {
#ifdef MDKR_LAN_PARTY_TESTING
    if (takeFailure(MdkrLanPartyTestFailure::AcceptTimeout)) return false;
#endif
#ifdef _WIN32
    DWORD receiveTimeout = kRecvPollMs;
    if (::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO,
                 reinterpret_cast<const char *>(&receiveTimeout),
                 sizeof(receiveTimeout)) != 0) return false;
    DWORD sendTimeout = kSendTimeoutMs;
    if (::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO,
                 reinterpret_cast<const char *>(&sendTimeout),
                 sizeof(sendTimeout)) != 0) return false;
#else
    struct timeval receiveTimeout;
    receiveTimeout.tv_sec = 0;
    receiveTimeout.tv_usec = static_cast<suseconds_t>(kRecvPollMs) * 1000;
    if (::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO,
                 reinterpret_cast<const char *>(&receiveTimeout),
                 sizeof(receiveTimeout)) != 0) return false;
    struct timeval sendTimeout;
    sendTimeout.tv_sec = kSendTimeoutMs / 1000u;
    sendTimeout.tv_usec = 0;
    if (::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO,
                 reinterpret_cast<const char *>(&sendTimeout),
                 sizeof(sendTimeout)) != 0) return false;
#endif
#ifdef SO_NOSIGPIPE
    int one = 1;
    if (::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE,
                 reinterpret_cast<const char *>(&one), sizeof(one)) != 0) return false;
#endif
    return true;
}

bool lastErrorWasTimeout() {
#ifdef _WIN32
    const int code = WSAGetLastError();
    return code == WSAETIMEDOUT || code == WSAEWOULDBLOCK || code == WSAEINTR;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
#endif
}

/* > 0 bytes read, 0 on orderly EOF, -1 on hard error/reset, -2 on the
 * bounded poll timeout (the caller re-checks its stop condition). */
int recvSome(SocketHandle fd, char *buffer, size_t capacity) {
#ifdef _WIN32
    const int got = ::recv(fd, buffer, static_cast<int>(capacity), 0);
#else
    const int got = static_cast<int>(::recv(fd, buffer, capacity, 0));
#endif
    if (got > 0) return got;
    if (got == 0) return 0;
    return lastErrorWasTimeout() ? -2 : -1;
}

bool sendAll(SocketHandle fd, const void *data, size_t size) {
    const char *bytes = static_cast<const char *>(data);
    size_t sent = 0u;
    while (sent < size) {
#ifdef MSG_NOSIGNAL
        const int flags = MSG_NOSIGNAL;
#else
        const int flags = 0;
#endif
#ifdef _WIN32
        const int wrote =
            ::send(fd, bytes + sent, static_cast<int>(size - sent), flags);
#else
        const int wrote =
            static_cast<int>(::send(fd, bytes + sent, size - sent, flags));
#endif
        if (wrote <= 0) return false; /* SO_SNDTIMEO bounds a stalled peer. */
        sent += static_cast<size_t>(wrote);
    }
    return true;
}

/* Absorb whatever the peer already sent, bounded, so the close frame we
 * just wrote arrives on a clean FIN instead of dying to an RST. */
void drainBriefly(SocketHandle fd) {
    const Clock::time_point deadline =
        Clock::now() + std::chrono::milliseconds(kCloseDrainMs);
    char discard[1024];
    while (Clock::now() < deadline) {
        const int got = recvSome(fd, discard, sizeof(discard));
        if (got == 0 || got == -1) return;
    }
}

/* ---- SHA-1 + base64 (RFC 6455 Sec-WebSocket-Accept derivation) ----------- */

uint32_t rotateLeft(uint32_t value, unsigned bits) {
    return (value << bits) | (value >> (32u - bits));
}

void sha1(const std::string &input, uint8_t digest[20]) {
    uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u,
                     0xC3D2E1F0u};
    std::vector<uint8_t> data(input.begin(), input.end());
    const uint64_t bitLength = static_cast<uint64_t>(input.size()) * 8u;
    data.push_back(0x80u);
    while (data.size() % 64u != 56u) data.push_back(0u);
    for (int shift = 56; shift >= 0; shift -= 8) {
        data.push_back(static_cast<uint8_t>((bitLength >> shift) & 0xffu));
    }
    for (size_t offset = 0u; offset < data.size(); offset += 64u) {
        uint32_t w[80];
        for (unsigned index = 0u; index < 16u; index++) {
            w[index] =
                (static_cast<uint32_t>(data[offset + index * 4u]) << 24u) |
                (static_cast<uint32_t>(data[offset + index * 4u + 1u]) << 16u) |
                (static_cast<uint32_t>(data[offset + index * 4u + 2u]) << 8u) |
                static_cast<uint32_t>(data[offset + index * 4u + 3u]);
        }
        for (unsigned index = 16u; index < 80u; index++) {
            w[index] = rotateLeft(w[index - 3u] ^ w[index - 8u] ^
                                      w[index - 14u] ^ w[index - 16u],
                                  1u);
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (unsigned index = 0u; index < 80u; index++) {
            uint32_t f;
            uint32_t k;
            if (index < 20u) {
                f = (b & c) | ((~b) & d);
                k = 0x5A827999u;
            } else if (index < 40u) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1u;
            } else if (index < 60u) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDCu;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6u;
            }
            const uint32_t next = rotateLeft(a, 5u) + f + e + k + w[index];
            e = d;
            d = c;
            c = rotateLeft(b, 30u);
            b = a;
            a = next;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
    }
    for (unsigned index = 0u; index < 5u; index++) {
        digest[index * 4u] = static_cast<uint8_t>((h[index] >> 24u) & 0xffu);
        digest[index * 4u + 1u] =
            static_cast<uint8_t>((h[index] >> 16u) & 0xffu);
        digest[index * 4u + 2u] = static_cast<uint8_t>((h[index] >> 8u) & 0xffu);
        digest[index * 4u + 3u] = static_cast<uint8_t>(h[index] & 0xffu);
    }
}

std::string base64(const uint8_t *bytes, size_t size) {
    static constexpr char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    result.reserve(((size + 2u) / 3u) * 4u);
    size_t index = 0u;
    while (index + 3u <= size) {
        const uint32_t triple = (static_cast<uint32_t>(bytes[index]) << 16u) |
                                (static_cast<uint32_t>(bytes[index + 1u]) << 8u) |
                                static_cast<uint32_t>(bytes[index + 2u]);
        result.push_back(kAlphabet[(triple >> 18u) & 63u]);
        result.push_back(kAlphabet[(triple >> 12u) & 63u]);
        result.push_back(kAlphabet[(triple >> 6u) & 63u]);
        result.push_back(kAlphabet[triple & 63u]);
        index += 3u;
    }
    if (index + 1u == size) {
        const uint32_t rest = static_cast<uint32_t>(bytes[index]) << 16u;
        result.push_back(kAlphabet[(rest >> 18u) & 63u]);
        result.push_back(kAlphabet[(rest >> 12u) & 63u]);
        result += "==";
    } else if (index + 2u == size) {
        const uint32_t rest = (static_cast<uint32_t>(bytes[index]) << 16u) |
                              (static_cast<uint32_t>(bytes[index + 1u]) << 8u);
        result.push_back(kAlphabet[(rest >> 18u) & 63u]);
        result.push_back(kAlphabet[(rest >> 12u) & 63u]);
        result.push_back(kAlphabet[(rest >> 6u) & 63u]);
        result.push_back('=');
    }
    return result;
}

std::string wsAcceptValue(const std::string &key) {
    uint8_t digest[20];
    sha1(key + kWsGuid, digest);
    return base64(digest, sizeof(digest));
}

/* ---- HTTP parsing --------------------------------------------------------- */

struct Request {
    std::string method;
    std::string target;
    std::string version;
    std::map<std::string, std::string> headers; /* lowercase names */
};

std::string loweredCopy(const std::string &value) {
    std::string result = value;
    for (char &byte : result) {
        if (byte >= 'A' && byte <= 'Z') {
            byte = static_cast<char>(byte - 'A' + 'a');
        }
    }
    return result;
}

std::string trimmedCopy(const std::string &value) {
    size_t begin = 0u;
    size_t end = value.size();
    while (begin < end && (value[begin] == ' ' || value[begin] == '\t')) {
        begin++;
    }
    while (end > begin &&
           (value[end - 1u] == ' ' || value[end - 1u] == '\t')) {
        end--;
    }
    return value.substr(begin, end - begin);
}

bool parseRequestHead(const std::string &head, Request &request) {
    size_t lineEnd = head.find("\r\n");
    if (lineEnd == std::string::npos) lineEnd = head.size();
    const std::string line = head.substr(0u, lineEnd);
    const size_t firstSpace = line.find(' ');
    const size_t lastSpace = line.rfind(' ');
    if (firstSpace == std::string::npos || lastSpace == firstSpace) {
        return false;
    }
    request.method = line.substr(0u, firstSpace);
    request.target = line.substr(firstSpace + 1u, lastSpace - firstSpace - 1u);
    request.version = line.substr(lastSpace + 1u);
    if (request.method.empty() || request.target.empty() ||
        request.target.find(' ') != std::string::npos ||
        (request.version != "HTTP/1.1" && request.version != "HTTP/1.0")) {
        return false;
    }
    size_t cursor = lineEnd;
    while (cursor < head.size()) {
        cursor += 2u; /* the CRLF */
        size_t nextEnd = head.find("\r\n", cursor);
        if (nextEnd == std::string::npos) nextEnd = head.size();
        const std::string headerLine = head.substr(cursor, nextEnd - cursor);
        if (!headerLine.empty()) {
            const size_t colon = headerLine.find(':');
            if (colon == std::string::npos || colon == 0u) return false;
            const std::string name =
                loweredCopy(trimmedCopy(headerLine.substr(0u, colon)));
            const std::string value =
                trimmedCopy(headerLine.substr(colon + 1u));
            auto existing = request.headers.find(name);
            if (existing == request.headers.end()) {
                request.headers[name] = value;
            } else {
                existing->second += ", " + value;
            }
        }
        cursor = nextEnd;
    }
    return true;
}

bool tokenListContains(const std::string &value, const char *token) {
    const std::string wanted = loweredCopy(token);
    size_t start = 0u;
    while (start <= value.size()) {
        size_t end = value.find(',', start);
        if (end == std::string::npos) end = value.size();
        if (loweredCopy(trimmedCopy(value.substr(start, end - start))) ==
            wanted) {
            return true;
        }
        if (end == value.size()) break;
        start = end + 1u;
    }
    return false;
}

bool requestWantsClose(const Request &request) {
    const auto connection = request.headers.find("connection");
    if (connection != request.headers.end() &&
        tokenListContains(connection->second, "close")) {
        return true;
    }
    if (request.version == "HTTP/1.0") {
        return connection == request.headers.end() ||
               !tokenListContains(connection->second, "keep-alive");
    }
    return false;
}

/* ---- HTTP responses -------------------------------------------------------- */

bool sendResponse(SocketHandle fd, int status, const char *reason,
                  const char *contentType, const uint8_t *body,
                  size_t bodySize, const std::string &extraHeaders,
                  bool keepAlive) {
    std::string head = "HTTP/1.1 " + std::to_string(status) + " " + reason +
                       "\r\nContent-Type: " + contentType +
                       "\r\nContent-Length: " + std::to_string(bodySize) +
                       "\r\nConnection: " +
                       (keepAlive ? "keep-alive" : "close") + "\r\n" +
                       extraHeaders;
    for (const ResponseHeader &header : kResponseHeaders) {
        head += header.name;
        head += ": ";
        head += header.value;
        head += "\r\n";
    }
    head += "\r\n";
    if (!sendAll(fd, head.data(), head.size())) return false;
    return bodySize == 0u || sendAll(fd, body, bodySize);
}

bool sendPlainResponse(SocketHandle fd, int status, const char *reason,
                       const char *body, const std::string &extraHeaders,
                       bool keepAlive) {
    return sendResponse(fd, status, reason, "text/plain; charset=utf-8",
                        reinterpret_cast<const uint8_t *>(body),
                        std::strlen(body), extraHeaders, keepAlive);
}

/* Fixed refusal bodies. Nothing the requester sent is ever reflected:
 * reflected content on a LAN HTTP server is a phishing canvas. */
bool send404(SocketHandle fd, bool keepAlive) {
    return sendPlainResponse(fd, 404, "Not Found", "Not found.\n",
                             std::string{}, keepAlive);
}

} /* namespace */

/* ---- Host allowlist (DNS-rebinding gate for /party-ws) -------------------- */

/*
 * Every IPv4 address this machine answers on. A phone's controller page
 * only ever dials the exact host the QR/invite named -- one of these -- so
 * this is the complete legitimate Host set for the upgrade gate below, and the
 * exact set local play advertises from (lan_party_launch.h). Public so the
 * launcher draws the QR host from the same enumeration that allowlists it.
 */
std::vector<std::string> mdkr_lan_party_machine_ipv4_addresses() {
    std::vector<std::string> result;
#ifdef _WIN32
    MdkrNativeSocketLease network;
    if (!network.acquire()) return result;
    char name[256] = {0};
    if (::gethostname(name, sizeof(name) - 1) == 0) {
        struct addrinfo hints;
        std::memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_INET;
        struct addrinfo *list = nullptr;
        if (::getaddrinfo(name, nullptr, &hints, &list) == 0) {
            std::unique_ptr<struct addrinfo, decltype(&::freeaddrinfo)> owner(list, ::freeaddrinfo);
            for (const struct addrinfo *entry = list; entry != nullptr;
                 entry = entry->ai_next) {
                char text[INET_ADDRSTRLEN] = {0};
                const struct sockaddr_in *address =
                    reinterpret_cast<const struct sockaddr_in *>(
                        entry->ai_addr);
                if (::inet_ntop(AF_INET, &address->sin_addr, text,
                                sizeof(text)) != nullptr) {
                    result.emplace_back(text);
                }
            }
        }
    }
#else
    struct ifaddrs *interfaces = nullptr;
    if (::getifaddrs(&interfaces) == 0) {
        std::unique_ptr<struct ifaddrs, decltype(&::freeifaddrs)> owner(interfaces, ::freeifaddrs);
        for (const struct ifaddrs *entry = interfaces; entry != nullptr;
             entry = entry->ifa_next) {
            if (entry->ifa_addr == nullptr ||
                entry->ifa_addr->sa_family != AF_INET) {
                continue;
            }
            char text[INET_ADDRSTRLEN] = {0};
            const struct sockaddr_in *address =
                reinterpret_cast<const struct sockaddr_in *>(entry->ifa_addr);
            if (::inet_ntop(AF_INET, &address->sin_addr, text,
                            sizeof(text)) != nullptr) {
                result.emplace_back(text);
            }
        }
    }
#endif
    return result;
}

namespace {

/*
 * DNS-rebinding gate: an internet page that rebinds its own hostname to
 * this machine's LAN IP reaches this port as "same-origin" plain HTTP+WS
 * (no TLS mismatch stops it; Safari ships no Private Network Access), but
 * the browser still sends the ATTACKER'S name in Host. Legitimate phones
 * dial exactly what the invite named -- loopback or a machine address --
 * so the whole Host token (host, plus an optional all-digits port) must
 * match the allowlist byte-for-byte after lowercasing. '@'/extra-':'
 * smuggle shapes fail the digits-only port rule; name suffixes fail the
 * exact host compare. Refusals reflect nothing.
 */
bool hostAllowed(const Request &request,
                 const std::vector<std::string> &allowed) {
    const auto found = request.headers.find("host");
    if (found == request.headers.end()) return false;
    std::string host = loweredCopy(found->second);
    const size_t colon = host.find(':');
    if (colon != std::string::npos) {
        const std::string port = host.substr(colon + 1u);
        if (port.empty() || port.size() > 5u) return false;
        for (const char byte : port) {
            if (byte < '0' || byte > '9') return false;
        }
        host.erase(colon);
    }
    if (host.empty()) return false;
    for (const std::string &candidate : allowed) {
        if (host == candidate) return true;
    }
    return false;
}

} /* namespace */

/* ---- WebSocket connection state ------------------------------------------ */

struct MdkrLanPartyWsState {
    MdkrNativeSocketLease networkLease;
    SocketHandle fd = kInvalidSocket;
    /* Serializes every frame write; sendText() may race the reader thread's
     * pong or close. closeSent is guarded by it: after a close frame goes
     * out, no other frame ever may (RFC 6455 5.5.1). */
    std::mutex sendMutex;
    bool closeSent = false;
    std::atomic<bool> open{true};
    std::mutex callbackMutex;
    std::function<void(const std::string &)> messageCallback;
    std::function<void()> closedCallback;
    bool closedNotified = false; /* guarded by callbackMutex */
};

namespace {

std::string wsFrameHeader(uint8_t opcode, size_t payloadSize) {
    std::string header;
    header.push_back(static_cast<char>(0x80u | opcode));
    if (payloadSize < 126u) {
        header.push_back(static_cast<char>(payloadSize));
    } else {
        /* The payload bound fits 16 bits with room to spare, so the 64-bit
         * length form is never produced. */
        header.push_back(static_cast<char>(126u));
        header.push_back(static_cast<char>((payloadSize >> 8u) & 0xffu));
        header.push_back(static_cast<char>(payloadSize & 0xffu));
    }
    return header;
}

bool wsSendFrame(MdkrLanPartyWsState &socket, uint8_t opcode,
                 const char *payload, size_t size) {
    std::lock_guard<std::mutex> lock(socket.sendMutex);
    if (socket.closeSent) return false;
#ifdef MDKR_LAN_PARTY_TESTING
    /* Inside the critical section, past the closeSent gate: exactly where
     * a sender sits when teardown must not pull the fd out from under it. */
    if (mdkr_lan_party_test_send_hook) mdkr_lan_party_test_send_hook();
#endif
    const std::string header = wsFrameHeader(opcode, size);
    if (!sendAll(socket.fd, header.data(), header.size())) return false;
    return size == 0u || sendAll(socket.fd, payload, size);
}

/* Latches closeSent, sends the close frame (code big-endian when present)
 * and half-closes the write side so the peer sees frame-then-FIN. */
void wsSendClose(MdkrLanPartyWsState &socket, const char *payload,
                 size_t size) {
    std::lock_guard<std::mutex> lock(socket.sendMutex);
    if (socket.closeSent) return;
    socket.closeSent = true;
    socket.open = false;
    // Close frames always fit the control-frame budget: their two-byte header
    // needs no allocation after closeSent is latched.
    const char header[2] = {static_cast<char>(0x88u), static_cast<char>(size)};
    if (sendAll(socket.fd, header, sizeof(header)) && size != 0u) {
        sendAll(socket.fd, payload, size);
    }
    shutdownWrite(socket.fd);
}

void wsSendCloseCode(MdkrLanPartyWsState &socket, uint16_t code) {
    char payload[2];
    payload[0] = static_cast<char>((code >> 8u) & 0xffu);
    payload[1] = static_cast<char>(code & 0xffu);
    wsSendClose(socket, payload, sizeof(payload));
}

/* Close payload is the 2-byte code plus an optional UTF-8 reason (RFC 6455
 * 5.5.1); the whole control-frame payload must stay <= 125 bytes, so the reason
 * is capped at 123. */
void wsSendCloseReason(MdkrLanPartyWsState &socket, uint16_t code,
                       const std::string &reason) {
    if (reason.empty()) {
        wsSendCloseCode(socket, code);
        return;
    }
    std::string payload;
    payload.push_back(static_cast<char>((code >> 8u) & 0xffu));
    payload.push_back(static_cast<char>(code & 0xffu));
    payload.append(reason, 0u, std::min<size_t>(reason.size(), 123u));
    wsSendClose(socket, payload.data(), payload.size());
}

void wsNotifyClosed(MdkrLanPartyWsState &socket) {
    socket.open = false;
    std::function<void()> callback;
    {
        std::lock_guard<std::mutex> lock(socket.callbackMutex);
        if (socket.closedNotified) return;
        socket.closedNotified = true;
        callback = std::move(socket.closedCallback);
    }
    if (callback) callback();
}

void wsDeliver(MdkrLanPartyWsState &socket, const std::string &payload) {
    std::function<void(const std::string &)> callback;
    {
        std::lock_guard<std::mutex> lock(socket.callbackMutex);
        callback = socket.messageCallback;
    }
    if (callback) callback(payload);
}

} /* namespace */

/* ---- MdkrLanPartyWebSocket ------------------------------------------------ */

MdkrLanPartyWebSocket::MdkrLanPartyWebSocket(
    std::shared_ptr<MdkrLanPartyWsState> state)
    : state_(std::move(state)) {}

MdkrLanPartyWebSocket::~MdkrLanPartyWebSocket() = default;

void MdkrLanPartyWebSocket::onMessage(
    std::function<void(const std::string &)> callback) {
    std::lock_guard<std::mutex> lock(state_->callbackMutex);
    state_->messageCallback = std::move(callback);
}

void MdkrLanPartyWebSocket::onClosed(std::function<void()> callback) {
    bool alreadyClosed = false;
    {
        std::lock_guard<std::mutex> lock(state_->callbackMutex);
        if (state_->closedNotified) {
            alreadyClosed = true; /* Late attach must not miss the close. */
        } else {
            state_->closedCallback = std::move(callback);
        }
    }
    if (alreadyClosed && callback) callback();
}

bool MdkrLanPartyWebSocket::sendText(const std::string &payload) {
    if (payload.size() > kMdkrLanPartyMaxWsPayloadBytes) return false;
    if (!state_->open) return false;
    try { return wsSendFrame(*state_, 0x1u, payload.data(), payload.size()); }
    catch (...) { return false; }
}

void MdkrLanPartyWebSocket::close(uint16_t code, const std::string &reason) {
    try { wsSendCloseReason(*state_, code, reason); }
    catch (...) { wsSendCloseCode(*state_, code); }
    /* The connection's reader thread completes the handshake and fires
     * onClosed; it owns the socket's lifetime end to end. */
}

bool MdkrLanPartyWebSocket::isOpen() const { return state_->open; }

/* ---- Server state ---------------------------------------------------------- */

namespace {

struct Connection {
    SocketHandle fd = kInvalidSocket;
    // The serving thread alone retires fd. stop() only requests shutdown while
    // holding this mutex, so it cannot act on an already-recycled descriptor.
    std::mutex socketMutex;
    std::thread thread;
    std::atomic<bool> done{false};
};

} /* namespace */

struct MdkrLanPartyServerState {
    MdkrNativeSocketLease networkLease;
    std::mutex mutex;
    bool running = false;
    std::atomic<bool> stopping{false};
    SocketHandle listenFd = kInvalidSocket;
    uint16_t boundPort = 0u;
    /* Frozen before the accept thread exists, cleared after every
     * connection thread is joined: connection threads read both unlocked. */
    MdkrLanPartyManifest manifest;
    /* Lowercased Host values that may open /party-ws: loopback plus the
     * machine's own IPv4 addresses (see hostAllowed). Same freeze rule. */
    std::vector<std::string> allowedHosts;
    std::function<void(std::shared_ptr<MdkrLanPartyWebSocket>)> wsCallback;
    std::thread acceptThread;
    std::vector<std::shared_ptr<Connection>> connections; /* guarded */
    // Only terminal destruction-time join failure uses this nonallocating
    // quarantine. It preserves live thread/socket/lease ownership and the
    // sticky failure flag prevents claiming complete network cleanup.
    std::shared_ptr<MdkrLanPartyServerState> failedRetirementOwner;
};

namespace {

thread_local MdkrLanPartyServerState *servingServer = nullptr;

struct ConnectionCompletion {
    std::shared_ptr<Connection> connection;
    std::shared_ptr<MdkrLanPartyWsState> webSocket;
    ~ConnectionCompletion() noexcept {
        if (webSocket) {
            // Seal every send BEFORE close, including when request parsing,
            // upgrade allocation, or a consumer callback throws.
            std::lock_guard<std::mutex> lock(webSocket->sendMutex);
            webSocket->closeSent = true;
            webSocket->open = false;
        }
        {
            std::lock_guard<std::mutex> lock(connection->socketMutex);
            const auto fd = std::exchange(connection->fd, kInvalidSocket);
            if (fd != kInvalidSocket) {
                shutdownBoth(fd);
                closeSocket(fd);
            }
        }
        if (webSocket) {
            try { wsNotifyClosed(*webSocket); } catch (...) { /* Consumer failure cannot skip retirement. */ }
            // Destruction of onMessage/callback captures is user code too.
            // Keep the server-thread marker through that final release so a
            // capture destructor's stop() remains a request, never a self-join.
            webSocket.reset();
        }
#ifdef MDKR_LAN_PARTY_TESTING
        --mdkr_lan_party_test_live_connections;
#endif
        servingServer = nullptr;
        connection->done = true;
    }
};

/* ---- The WebSocket frame loop --------------------------------------------- */

void runWebSocket(const std::shared_ptr<MdkrLanPartyServerState> &state,
                  const std::shared_ptr<MdkrLanPartyWsState> &socket,
                  std::string carried) {
    bool drainBeforeTeardown = false;
    const uint64_t idleCloseMs = wsIdleDeadlineMs();
    const uint64_t idlePingMs = idleCloseMs / 2u;
    const auto steadyMs = []() -> uint64_t {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                Clock::now().time_since_epoch()).count());
    };
    uint64_t lastActivityMs = steadyMs();
    bool pingOutstanding = false;
    const auto need = [&](size_t count) -> bool {
        while (carried.size() < count) {
            if (state->stopping) return false;
            char chunk[4096];
            const int got = recvSome(socket->fd, chunk, sizeof(chunk));
            if (got == 0 || got == -1) return false;
            if (got == -2) {
                /* Idle-reaper poll. A live socket resets this on any inbound
                 * byte, including the automatic pong a browser sends for the
                 * ping below, so only a silent hold reaches the close. */
                const uint64_t idle = steadyMs() - lastActivityMs;
                if (idle >= idleCloseMs) {
                    wsSendCloseCode(*socket, 1000u);
                    return false;
                }
                if (idle >= idlePingMs && !pingOutstanding) {
                    pingOutstanding = wsSendFrame(*socket, 0x9u, "", 0u);
                }
                continue;
            }
            lastActivityMs = steadyMs();
            pingOutstanding = false;
            carried.append(chunk, static_cast<size_t>(got));
        }
        return true;
    };
    for (;;) {
        if (!need(2u)) break;
        const uint8_t byte0 = static_cast<uint8_t>(carried[0]);
        const uint8_t byte1 = static_cast<uint8_t>(carried[1]);
        const bool fin = (byte0 & 0x80u) != 0u;
        const uint8_t reserved = byte0 & 0x70u;
        const uint8_t opcode = byte0 & 0x0fu;
        const bool masked = (byte1 & 0x80u) != 0u;
        size_t length = byte1 & 0x7fu;
        /* Masked client frames are REQUIRED (RFC 6455 5.1); an unmasked or
         * reserved-bit frame is a protocol error. */
        if (reserved != 0u || !masked) {
            wsSendCloseCode(*socket, 1002u);
            drainBeforeTeardown = true;
            break;
        }
        size_t headerSize = 2u;
        if (length == 127u) {
            /* A 64-bit length can only ever declare more than the party
             * control-message bound: refuse from the declaration alone. */
            wsSendCloseCode(*socket, 1009u);
            drainBeforeTeardown = true;
            break;
        }
        if (length == 126u) {
            if (!need(4u)) break;
            length =
                (static_cast<size_t>(static_cast<uint8_t>(carried[2])) << 8u) |
                static_cast<size_t>(static_cast<uint8_t>(carried[3]));
            headerSize = 4u;
        }
        if (length > kMdkrLanPartyMaxWsPayloadBytes) {
            wsSendCloseCode(*socket, 1009u);
            drainBeforeTeardown = true;
            break;
        }
        const bool control = (opcode & 0x8u) != 0u;
        if (opcode == 0x0u || (!control && !fin)) {
            /* Fragmentation refused politely: bounded control messages
             * never need it, and a reassembly buffer is attack surface. */
            wsSendCloseCode(*socket, 1003u);
            drainBeforeTeardown = true;
            break;
        }
        if (control && (!fin || length > 125u)) {
            wsSendCloseCode(*socket, 1002u);
            drainBeforeTeardown = true;
            break;
        }
        if (!need(headerSize + 4u + length)) break;
        uint8_t mask[4];
        for (unsigned index = 0u; index < 4u; index++) {
            mask[index] = static_cast<uint8_t>(carried[headerSize + index]);
        }
        std::string payload = carried.substr(headerSize + 4u, length);
        for (size_t index = 0u; index < payload.size(); index++) {
            payload[index] = static_cast<char>(
                static_cast<uint8_t>(payload[index]) ^ mask[index % 4u]);
        }
        carried.erase(0u, headerSize + 4u + length);
        if (opcode == 0x1u || opcode == 0x2u) {
            wsDeliver(*socket, payload);
        } else if (opcode == 0x9u) {
            /* Ping -> pong with the identical payload. */
            wsSendFrame(*socket, 0xau, payload.data(), payload.size());
        } else if (opcode == 0xau) {
            /* Unsolicited pong: ignored. */
        } else if (opcode == 0x8u) {
            /* Close handshake: reply with OUR normal-closure 1000, never
             * the peer's bytes -- RFC 6455 7.4 reserves code ranges, and
             * this file's non-reflection rule wins over the echo the RFC
             * merely suggests. No-op if our side already sent a close. */
            wsSendCloseCode(*socket, 1000u);
            break;
        } else {
            wsSendCloseCode(*socket, 1002u);
            drainBeforeTeardown = true;
            break;
        }
    }
    if (drainBeforeTeardown) drainBriefly(socket->fd);
    // ConnectionCompletion performs the same sealed retirement on normal
    // return and exceptional exits, including callback/allocator failures.
}

/* ---- The per-connection HTTP loop ------------------------------------------ */

/* The control subprotocol the shipped controller page offers on /party-ws for
 * local play (dist/web/controller/controller.js) and the cloud Worker echoes on
 * the controller connect (services/party worker.ts). A browser that offered a
 * Sec-WebSocket-Protocol fails the handshake unless the server SELECTS one, so
 * the embedded server must echo this back or no LAN phone can complete the
 * upgrade -- exactly what the end-to-end lane pins. */
constexpr char kMdkrLanPartyControlSubprotocol[] = "gb-control-v1";

enum class UpgradeCheck { Ok, NotAnUpgrade, BadKey };

UpgradeCheck validateUpgrade(const Request &request, std::string &key) {
    const auto upgrade = request.headers.find("upgrade");
    const auto connection = request.headers.find("connection");
    const auto version = request.headers.find("sec-websocket-version");
    if (upgrade == request.headers.end() ||
        !tokenListContains(upgrade->second, "websocket") ||
        connection == request.headers.end() ||
        !tokenListContains(connection->second, "upgrade") ||
        version == request.headers.end() || version->second != "13") {
        return UpgradeCheck::NotAnUpgrade;
    }
    const auto found = request.headers.find("sec-websocket-key");
    if (found == request.headers.end() || found->second.size() != 24u) {
        return UpgradeCheck::BadKey;
    }
    key = found->second;
    return UpgradeCheck::Ok;
}

void serveConnection(std::shared_ptr<MdkrLanPartyServerState> state,
                     std::shared_ptr<Connection> connection) noexcept {
    servingServer = state.get();
#ifdef MDKR_LAN_PARTY_TESTING
    ++mdkr_lan_party_test_live_connections;
#endif
    ConnectionCompletion completion{connection, {}};
    try {
#ifdef MDKR_LAN_PARTY_TESTING
    failAt(MdkrLanPartyTestFailure::ServeBuffer);
#endif
    std::string buffer;
    size_t served = 0u;
    Clock::time_point deadline =
        Clock::now() + std::chrono::milliseconds(httpDeadlineMs());
    while (!state->stopping) {
        const size_t headerEnd = buffer.find("\r\n\r\n");
        if (headerEnd == std::string::npos) {
            /* Bounds first: an oversized request line (or head) cuts the
             * connection with no status line an attacker could probe. */
            const size_t lineEnd = buffer.find("\r\n");
            const size_t lineSize =
                lineEnd == std::string::npos ? buffer.size() : lineEnd;
            if (lineSize > kMaxRequestLineBytes ||
                buffer.size() > kMaxRequestHeaderBytes) {
                break;
            }
            char chunk[4096];
            const int got = recvSome(connection->fd, chunk, sizeof(chunk));
            if (got == 0 || got == -1) break;
            /* The deadline is anchored at the last COMPLETE request (reset
             * only after a response goes out), and enforced on EVERY
             * buffering iteration -- silence and slow drip alike. A byte
             * per poll interval is not progress: pre-fix it renewed the
             * idle check forever, letting one hostile device hold a slot
             * for the ~34 minutes the head cap took to fill, x32 slots.
             * A real controller page head is under a kilobyte and arrives
             * in milliseconds; the full window is pure headroom. */
            if (Clock::now() >= deadline) break;
            if (got == -2) continue;
            buffer.append(chunk, static_cast<size_t>(got));
            continue;
        }
        Request request;
        if (!parseRequestHead(buffer.substr(0u, headerEnd), request)) {
            sendPlainResponse(connection->fd, 400, "Bad Request",
                              "Bad request.\n", std::string{}, false);
            break;
        }
        buffer.erase(0u, headerEnd + 4u);
        served++;
        if (request.method != "GET") {
            /* HEAD included, deliberately: browsers fetch these assets with
             * GET only, and refusing HEAD keeps the surface at exactly one
             * method. Any body that came with the request stays unread, so
             * this connection cannot stay in framing sync: refuse and
             * close. */
            sendPlainResponse(connection->fd, 405, "Method Not Allowed",
                              "Method not allowed.\n", "Allow: GET\r\n",
                              false);
            break;
        }
        /* A GET carrying a body would desync keep-alive framing: this
         * server never reads bodies, so the body bytes would parse as the
         * NEXT request head. No browser fetches assets that way; refuse
         * and close. Content-Length: 0 stays accepted -- some HTTP
         * libraries attach it to every request. */
        const auto contentLength = request.headers.find("content-length");
        if (request.headers.count("transfer-encoding") != 0u ||
            (contentLength != request.headers.end() &&
             contentLength->second != "0")) {
            sendPlainResponse(connection->fd, 400, "Bad Request",
                              "Bad request.\n", std::string{}, false);
            break;
        }
        std::string path = request.target;
        const size_t cut = path.find_first_of("?#");
        if (cut != std::string::npos) path.erase(cut);
        if (path == kWsPath) {
            if (!hostAllowed(request, state->allowedHosts)) {
                /* DNS-rebinding gate (see hostAllowed): a Host this server
                 * does not answer as may never open the control socket. */
                sendPlainResponse(connection->fd, 403, "Forbidden",
                                  "Forbidden.\n", std::string{}, false);
                break;
            }
            std::string key;
            const UpgradeCheck check = validateUpgrade(request, key);
            if (check == UpgradeCheck::NotAnUpgrade) {
                sendPlainResponse(connection->fd, 426, "Upgrade Required",
                                  "WebSocket upgrade required.\n",
                                  "Upgrade: websocket\r\n"
                                  "Sec-WebSocket-Version: 13\r\n",
                                  false);
                break;
            }
            if (check == UpgradeCheck::BadKey) {
                sendPlainResponse(connection->fd, 400, "Bad Request",
                                  "Bad request.\n", std::string{}, false);
                break;
            }
            std::function<void(std::shared_ptr<MdkrLanPartyWebSocket>)>
                callback;
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                callback = state->wsCallback;
            }
            if (!callback) {
                /* No consumer registered: refuse without upgrading. */
                send404(connection->fd, false);
                break;
            }
            /* RFC 6455 subprotocol negotiation: select the page's control
             * subprotocol when it was offered, mirroring the cloud Worker.
             * Without this echo Chromium fails the whole handshake. */
            std::string selectedProtocol;
            const auto offered =
                request.headers.find("sec-websocket-protocol");
            if (offered != request.headers.end() &&
                tokenListContains(offered->second,
                                  kMdkrLanPartyControlSubprotocol)) {
                selectedProtocol = "Sec-WebSocket-Protocol: " +
                    std::string(kMdkrLanPartyControlSubprotocol) + "\r\n";
            }
            const std::string response =
                "HTTP/1.1 101 Switching Protocols\r\n"
                "Upgrade: websocket\r\n"
                "Connection: Upgrade\r\n"
                "Sec-WebSocket-Accept: " +
                wsAcceptValue(key) + "\r\n" + selectedProtocol + "\r\n";
            if (!sendAll(connection->fd, response.data(), response.size())) {
                break;
            }
#ifdef MDKR_LAN_PARTY_TESTING
            failAt(MdkrLanPartyTestFailure::WebSocketState);
#endif
            auto wsState = std::make_shared<MdkrLanPartyWsState>();
            completion.webSocket = wsState;
            wsState->networkLease = state->networkLease;
            wsState->fd = connection->fd;
            /* The consumer gets the socket BEFORE any frame is parsed, on
             * this thread, so callbacks it attaches inside the callback
             * can never miss a message. Any pipelined bytes after the
             * upgrade head are the first frames -- they ride along. */
#ifdef MDKR_LAN_PARTY_TESTING
            failAt(MdkrLanPartyTestFailure::WebSocketWrapper);
#endif
            callback(std::make_shared<MdkrLanPartyWebSocket>(wsState));
            runWebSocket(state, wsState, std::move(buffer));
            break;
        }
        const auto found = state->manifest.find(path);
        const bool keepAlive = !requestWantsClose(request) &&
                               served < kMaxRequestsPerConnection;
        bool sent;
        if (found == state->manifest.end()) {
            sent = send404(connection->fd, keepAlive);
        } else {
            sent = sendResponse(
                connection->fd, 200, "OK", found->second.contentType.c_str(),
                found->second.bytes.data(), found->second.bytes.size(),
                std::string{}, keepAlive);
        }
        if (!sent || !keepAlive) break;
        deadline = Clock::now() + std::chrono::milliseconds(httpDeadlineMs());
    }
    } catch (...) { /* Fail only this connection; completion owns mandatory cleanup. */ }
}

/* ---- Accept thread ----------------------------------------------------------- */

void reapFinishedConnections(
    const std::shared_ptr<MdkrLanPartyServerState> &state) {
    for (;;) {
        std::shared_ptr<Connection> finished;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            for (const auto &connection : state->connections) {
                if (connection->done) {
                    finished = connection;
                    break;
                }
            }
        }
        if (!finished) return;
        // No temporary allocating vector and no lost join owner on exception.
        // stop() joins the accept thread before it can touch these handles.
        if (finished->thread.joinable()) finished->thread.join();
        std::lock_guard<std::mutex> lock(state->mutex);
        const auto found = std::find(state->connections.begin(), state->connections.end(), finished);
        if (found != state->connections.end()) state->connections.erase(found);
    }
}

void acceptLoop(std::shared_ptr<MdkrLanPartyServerState> state, SocketHandle listenFd) noexcept {
    try {
    while (!state->stopping) {
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(listenFd, &readable);
        struct timeval interval;
        interval.tv_sec = 0;
        interval.tv_usec =
            static_cast<decltype(interval.tv_usec)>(kAcceptPollMs) * 1000;
#ifdef _WIN32
        const int ready = ::select(0, &readable, nullptr, nullptr, &interval);
#else
        const int ready =
            ::select(listenFd + 1, &readable, nullptr, nullptr, &interval);
#endif
        reapFinishedConnections(state);
        if (state->stopping) break;
        if (ready <= 0) continue;
        const SocketHandle client = ::accept(listenFd, nullptr, nullptr);
        if (client == kInvalidSocket) continue;
        SocketOwner socket{client};
        // Explicitly restore bounded blocking I/O on platforms that inherit
        // the listener's nonblocking mode for accepted sockets.
        if (!setSocketBlocking(client, true) || !setSocketTimeouts(client)) continue;
        {
            /* Disable Nagle immediately post-accept: every frame this server
             * writes is small (control JSON, WS frames), and Nagle + the
             * phone's delayed ACK adds tens of ms per exchange during
             * pairing. Set-failure is non-fatal -- coalescing merely stays
             * on. Mandatory timeouts and signal safety were admitted above. */
            int noDelay = 1;
            (void)::setsockopt(client, IPPROTO_TCP, TCP_NODELAY,
                               reinterpret_cast<const char *>(&noDelay),
                               sizeof(noDelay));
        }
        try {
#ifdef MDKR_LAN_PARTY_TESTING
            failAt(MdkrLanPartyTestFailure::AcceptAllocation);
#endif
            auto connection = std::make_shared<Connection>();
            connection->fd = client;
            std::lock_guard<std::mutex> lock(state->mutex);
            if (!state->stopping &&
                state->connections.size() < kMaxConnections) {
#ifdef MDKR_LAN_PARTY_TESTING
                failAt(MdkrLanPartyTestFailure::AcceptRegistry);
#endif
                state->connections.push_back(connection);
                try {
#ifdef MDKR_LAN_PARTY_TESTING
                    failAt(MdkrLanPartyTestFailure::AcceptThread);
#endif
                    connection->thread = std::thread(serveConnection, state, connection);
                    (void)socket.release(); // worker owns retirement from here
                } catch (...) {
                    // No worker exists when thread construction throws. Undo
                    // its registry admission before the accepted-fd owner exits.
                    state->connections.pop_back();
                    throw;
                }
            }
        } catch (...) { /* Refuse this admission; listener and existing clients remain live. */ }
    }
    } catch (...) {
        // Keep any joinable records in state for the launcher-owned stop().
        state->stopping = true;
    }
}

} /* namespace */

/* ---- MdkrLanPartyServer -------------------------------------------------------- */

MdkrLanPartyServer::MdkrLanPartyServer()
    : state_(std::make_shared<MdkrLanPartyServerState>()) {}

MdkrLanPartyServer::~MdkrLanPartyServer() {
    if (servingServer == state_.get()) {
        // Destruction is launcher-owned by contract. If a callback violates
        // that contract, joining itself is impossible; retain rather than let
        // the final worker destroy its own still-joinable thread handle.
        state_->stopping = true;
        state_->failedRetirementOwner = state_;
        mdkrFirstPartyNetworkCleanupFailed.store(true);
        return;
    }
    try {
        stop();
    } catch (const std::system_error &) {
        state_->failedRetirementOwner = state_;
        mdkrFirstPartyNetworkCleanupFailed.store(true);
    }
}

void MdkrLanPartyServer::onWebSocket(
    std::function<void(std::shared_ptr<MdkrLanPartyWebSocket>)> callback) {
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->wsCallback = std::move(callback);
}

bool MdkrLanPartyServer::start(uint16_t port, MdkrLanPartyManifest manifest) {
    MdkrNativeSocketLease network;
    if (!network.acquire()) return false;
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (state_->running || state_->stopping) return false;
    const SocketHandle fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd == kInvalidSocket) return false;
    SocketOwner socket{fd};
#ifndef _WIN32
    if (fd >= FD_SETSIZE) return false; // select's fixed fd_set cannot represent it
#endif
    // Readiness can vanish between select and accept. A nonblocking listener
    // prevents that ordinary disconnect race from stranding stop() in join.
    if (!setSocketBlocking(fd, false)) return false;
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR,
                 reinterpret_cast<const char *>(&one), sizeof(one));
    struct sockaddr_in address;
    std::memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    /* Every interface: the phones are on the LAN, and one port here is the
     * one firewall prompt the player ever sees. */
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    if (::bind(fd, reinterpret_cast<struct sockaddr *>(&address),
               sizeof(address)) != 0 ||
        ::listen(fd, 16) != 0) {
        return false;
    }
    struct sockaddr_in bound;
    std::memset(&bound, 0, sizeof(bound));
    socklen_t boundSize = sizeof(bound);
    if (::getsockname(fd, reinterpret_cast<struct sockaddr *>(&bound),
                      &boundSize) != 0) {
        return false;
    }
    try {
#ifdef MDKR_LAN_PARTY_TESTING
        failAt(MdkrLanPartyTestFailure::StartHosts);
#endif
        std::vector<std::string> hosts{"localhost", "127.0.0.1"};
        for (std::string &address : mdkr_lan_party_machine_ipv4_addresses()) {
            hosts.push_back(std::move(address));
        }
        state_->manifest = std::move(manifest);
        state_->allowedHosts = std::move(hosts);
        state_->networkLease = network;
        state_->listenFd = fd;
        state_->boundPort = ntohs(bound.sin_port);
        state_->stopping = false;
#ifdef MDKR_LAN_PARTY_TESTING
        failAt(MdkrLanPartyTestFailure::StartThread);
#endif
        state_->acceptThread = std::thread(acceptLoop, state_, fd);
        state_->running = true;
        (void)socket.release();
    } catch (...) {
        // A refused thread start owns no worker. The local socket/lease retire
        // in order, and another start must not see a phantom running listener.
        state_->listenFd = kInvalidSocket;
        state_->boundPort = 0u;
        state_->manifest.clear();
        state_->allowedHosts.clear();
        state_->networkLease.reset();
        return false;
    }
    return true;
}

void MdkrLanPartyServer::stop() {
    if (servingServer == state_.get()) {
        // Callbacks are not join owners. Request stop without waiting on this
        // very thread or on the accept reaper that could already be joining it.
        state_->stopping = true;
        return;
    }
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (!state_->running && !state_->stopping) return;
        state_->running = false;
        state_->stopping = true;
        state_->boundPort = 0u;
    }
    /* Join the accept thread FIRST (it wakes within its poll interval): once
     * it is gone, no new connection can join or leave the registry. Handles
     * stay in state until joined, including if the OS rejects a join. */
    if (state_->acceptThread.joinable()) state_->acceptThread.join();
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        const auto listenFd = std::exchange(state_->listenFd, kInvalidSocket);
        if (listenFd != kInvalidSocket) closeSocket(listenFd);
    }
    for (const auto &connection : state_->connections) {
        /* Wakes any blocking read; each thread then tears itself down and
         * fires its socket's onClosed on the way out. */
        std::lock_guard<std::mutex> lock(connection->socketMutex);
        if (connection->fd != kInvalidSocket) shutdownBoth(connection->fd);
    }
    for (const auto &connection : state_->connections) {
        if (connection->thread.joinable()) connection->thread.join();
    }
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->connections.clear();
    state_->manifest.clear();
    state_->allowedHosts.clear();
    state_->stopping = false;
    state_->networkLease.reset(); // Joined handlers; retained WS aliases own copies.
}

uint16_t MdkrLanPartyServer::port() const {
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->boundPort;
}
