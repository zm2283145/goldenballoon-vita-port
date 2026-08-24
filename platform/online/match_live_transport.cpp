/*
 * O-T6 production transport backends (declarations in match_live_transport.h).
 *
 * A bounded, libcurl-free HTTP/1.1 client + RFC 6455 /connect WebSocket client
 * for the MatchRoom lobby, and a thin MdkrOnlineMeshSignalBackend over the O-T1
 * MdkrMatchSignalClient. All sockets go through mbedtls: TLS (https/wss) with
 * the embedded Mozilla CA bundle + hostname verification, or plaintext TCP for
 * the loopback test lane behind mdkr_party_loopback_test_url_allowed. The
 * credential rides only in the Authorization bearer (HTTP) and the
 * gb-match.{credential} subprotocol (WS), never a URL or query.
 */
#include "match_live_transport.h"

#include "party/native_party_host.h" /* mdkr_party_loopback_test_url_allowed */
#include "mozilla_ca_bundle.h"

#include <nlohmann/json.hpp>

#include <mbedtls/base64.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/platform_util.h>
#include <mbedtls/sha1.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <cerrno>
#endif

#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using Json = nlohmann::json;

/* Bounds so a stalled/unreachable host (the exact Wave-2 NET-01 failure mode)
 * can never wedge the worker thread past a deadline: close()/join() then always
 * returns promptly. Mirrors the O-T1 signal client's discipline. */
constexpr uint64_t kConnectTimeoutMs = 8000u;
constexpr uint64_t kWriteTimeoutMs = 8000u;
constexpr uint32_t kPollSliceMs = 100u;
/* Per-address connect cap (N3, the match_signal_client discipline): one
 * blackholed address costs at most min(this, remaining/addresses-left). */
constexpr uint64_t kPerAddressConnectCapMs = 3500u;
/* /connect + /signal reconnect ladder (N1/N6b): the party resume ladder's
 * shape -- exponential from 300 ms, capped, bounded attempts, then the
 * existing terminal path. */
constexpr uint64_t kReconnectBaseDelayMs = 300u;
constexpr uint64_t kReconnectMaxDelayMs = 5000u;
constexpr unsigned kReconnectMaxAttempts = 6u;

uint64_t reconnectDelayMs(unsigned failedAttempts) {
    const unsigned exponent =
        failedAttempts > 0u ? (failedAttempts - 1u < 5u ? failedAttempts - 1u
                                                        : 5u)
                            : 0u;
    const uint64_t delay = kReconnectBaseDelayMs << exponent;
    return delay > kReconnectMaxDelayMs ? kReconnectMaxDelayMs : delay;
}
/* Total assembled-message cap across continuation frames (memory-DoS guard) --
 * a lobby snapshot is a few KiB; 256 KiB is generous and fails closed above. */
constexpr size_t kMaxWsMessageBytes = 256u * 1024u;
/* Once any byte of a frame has arrived, the rest must land within this budget;
 * a server that half-sends a frame then goes silent is otherwise a spin. */
constexpr uint64_t kFrameTimeoutMs = 15000u;

uint64_t nowMs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

#ifdef _WIN32
using SocketFd = SOCKET;
constexpr SocketFd kBadSocket = INVALID_SOCKET;
void closeSocket(SocketFd fd) { ::closesocket(fd); }
void ensureNetStartup() {
    static std::once_flag once;
    std::call_once(once, []() {
        WSADATA data;
        (void)::WSAStartup(MAKEWORD(2, 2), &data);
    });
}
bool setBlocking(SocketFd fd, bool blocking) {
    u_long mode = blocking ? 0u : 1u;
    return ::ioctlsocket(fd, FIONBIO, &mode) == 0;
}
#else
using SocketFd = int;
constexpr SocketFd kBadSocket = -1;
void closeSocket(SocketFd fd) { ::close(fd); }
void ensureNetStartup() {}
bool setBlocking(SocketFd fd, bool blocking) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) return false;
    const int updated = blocking ? (flags & ~O_NONBLOCK) : (flags | O_NONBLOCK);
    return ::fcntl(fd, F_SETFL, updated) == 0;
}
#endif

/* ---- Resolution (seamed for tests; see match_live_transport.h) -----------
 * Mirrors match_signal_client.cpp's resolver discipline; kept file-local
 * because these two clients deliberately share no translation unit. */

struct ResolvedAddress {
    int family = AF_UNSPEC;
    int socktype = SOCK_STREAM;
    int protocol = IPPROTO_TCP;
    struct sockaddr_storage storage {};
    socklen_t length = 0u;
};

std::mutex &resolverSeamMutex() {
    static std::mutex mutex;
    return mutex;
}
std::string g_prependAddressForTest;      /* guarded by resolverSeamMutex() */
uint16_t g_prependPortForTest = 0u;       /* guarded by resolverSeamMutex() */
std::atomic<unsigned> g_resolverStallMsForTest{0u};

void appendAddrinfo(const struct addrinfo *results,
                    std::vector<ResolvedAddress> &out) {
    for (const struct addrinfo *entry = results; entry != nullptr;
         entry = entry->ai_next) {
        if (entry->ai_addrlen == 0u ||
            entry->ai_addrlen > sizeof(struct sockaddr_storage)) {
            continue;
        }
        ResolvedAddress address;
        address.family = entry->ai_family;
        address.socktype = entry->ai_socktype;
        address.protocol = entry->ai_protocol;
        std::memcpy(&address.storage, entry->ai_addr, entry->ai_addrlen);
        address.length = static_cast<socklen_t>(entry->ai_addrlen);
        out.push_back(address);
    }
}

bool prependTestAddress(uint16_t port, std::vector<ResolvedAddress> &out) {
    std::string ip;
    uint16_t overridePort = 0u;
    {
        std::lock_guard<std::mutex> lock(resolverSeamMutex());
        ip = g_prependAddressForTest;
        overridePort = g_prependPortForTest;
    }
    if (ip.empty()) return false;
    ResolvedAddress address;
    struct sockaddr_in *v4 =
        reinterpret_cast<struct sockaddr_in *>(&address.storage);
    std::memset(v4, 0, sizeof(*v4));
    if (::inet_pton(AF_INET, ip.c_str(), &v4->sin_addr) != 1) return false;
    v4->sin_family = AF_INET;
    v4->sin_port = htons(overridePort != 0u ? overridePort : port);
    address.family = AF_INET;
    address.length = static_cast<socklen_t>(sizeof(*v4));
    out.push_back(address);
    return true;
}

/* Resolve on a detached helper thread, polled in kPollSliceMs slices against
 * the deadline and the abort flag (N6c): getaddrinfo is uninterruptible, and
 * it used to run directly on the worker thread that close() joins, so a DNS
 * outage could freeze the launcher for the resolver timeout. On give-up the
 * helper is ABANDONED (it frees its own result), never joined. Same
 * detached-resolver choice as match_signal_client.cpp and for the same
 * reason: pre-resolving before thread start would just move the identical
 * synchronous hit onto the launcher thread. */
struct ResolveTask {
    std::mutex mutex;
    std::condition_variable cv;
    bool done = false;
    bool abandoned = false;
    int rc = -1;
    struct addrinfo *results = nullptr;
};

bool resolveAddresses(const std::string &host, const std::string &port,
                      uint64_t deadlineMs, const std::atomic<bool> *abort,
                      std::vector<ResolvedAddress> &out) {
    out.clear();
    uint16_t numericPort = 0u;
    for (const char c : port) {
        numericPort = static_cast<uint16_t>(numericPort * 10u +
                                            static_cast<uint16_t>(c - '0'));
    }
    (void)prependTestAddress(numericPort, out);
    auto task = std::make_shared<ResolveTask>();
    const unsigned stallMs = g_resolverStallMsForTest.load();
    std::thread helper([task, host, port, stallMs]() {
        if (stallMs != 0u) {
            std::this_thread::sleep_for(std::chrono::milliseconds(stallMs));
        }
        struct addrinfo hints;
        std::memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;
        struct addrinfo *results = nullptr;
        const int rc = ::getaddrinfo(host.c_str(), port.c_str(), &hints,
                                     &results);
        std::lock_guard<std::mutex> lock(task->mutex);
        if (task->abandoned) {
            if (results != nullptr) ::freeaddrinfo(results);
        } else {
            task->rc = rc;
            task->results = results;
        }
        task->done = true;
        task->cv.notify_all();
    });
    helper.detach();

    struct addrinfo *results = nullptr;
    int rc = -1;
    {
        std::unique_lock<std::mutex> lock(task->mutex);
        while (!task->done) {
            if ((abort != nullptr && abort->load()) ||
                nowMs() >= deadlineMs) {
                task->abandoned = true;
                return !out.empty(); /* only a test prepend, if any */
            }
            task->cv.wait_for(lock, std::chrono::milliseconds(kPollSliceMs));
        }
        rc = task->rc;
        results = task->results;
        task->results = nullptr;
    }
    if (rc != 0 || results == nullptr) {
        if (results != nullptr) ::freeaddrinfo(results);
        return !out.empty();
    }
    appendAddrinfo(results, out);
    ::freeaddrinfo(results);
    return !out.empty();
}

/* Deadline- and abort-aware TCP connect (O-T1 connectTcp discipline): a
 * non-blocking connect polled in short slices, so an unreachable host stops at
 * the deadline and a close() during connect aborts within one slice. Returns
 * kBadSocket on any failure. The returned fd is left BLOCKING so mbedtls's
 * recv_timeout/send operate on it exactly as before. */
SocketFd connectWithDeadline(const std::string &host, const std::string &port,
                             uint64_t deadlineMs,
                             const std::atomic<bool> *abort) {
    ensureNetStartup();
    std::vector<ResolvedAddress> addresses;
    if (!resolveAddresses(host, port, deadlineMs, abort, addresses) ||
        addresses.empty()) {
        return kBadSocket;
    }
    SocketFd fd = kBadSocket;
    for (size_t index = 0u; index < addresses.size(); index++) {
        /* N3 per-address budget: min(cap, remaining/left), so a blackholed
         * first address (broken-IPv6 household) costs one slice and the
         * loop falls through to the rest of the list; the overall deadline
         * still bounds the whole connect. */
        const uint64_t nowAtEntry = nowMs();
        if (nowAtEntry >= deadlineMs) break;
        const uint64_t remaining = deadlineMs - nowAtEntry;
        uint64_t slice = remaining / (addresses.size() - index);
        if (slice == 0u) slice = remaining;
        if (slice > kPerAddressConnectCapMs) slice = kPerAddressConnectCapMs;
        const uint64_t addressDeadlineMs = nowAtEntry + slice;
        const ResolvedAddress &entry = addresses[index];
        fd = ::socket(entry.family, entry.socktype, entry.protocol);
        if (fd == kBadSocket) continue;
#ifdef SO_NOSIGPIPE
        int one = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE,
                     reinterpret_cast<const char *>(&one), sizeof(one));
#endif
        if (!setBlocking(fd, false)) {
            closeSocket(fd);
            fd = kBadSocket;
            continue;
        }
        const int rc = ::connect(
            fd, reinterpret_cast<const struct sockaddr *>(&entry.storage),
            entry.length);
        bool pending = false;
        if (rc != 0) {
#ifdef _WIN32
            pending = ::WSAGetLastError() == WSAEWOULDBLOCK;
#else
            pending = errno == EINPROGRESS;
#endif
            if (!pending) {
                closeSocket(fd);
                fd = kBadSocket;
                continue;
            }
        }
        bool established = !pending;
        while (pending && (abort == nullptr || !abort->load())) {
            if (nowMs() >= addressDeadlineMs) break;
#ifdef _WIN32
            fd_set writable;
            FD_ZERO(&writable);
            FD_SET(fd, &writable);
            struct timeval slice;
            slice.tv_sec = 0;
            slice.tv_usec = static_cast<long>(kPollSliceMs) * 1000;
            const int ready = ::select(0, nullptr, &writable, nullptr, &slice);
#else
            struct pollfd item;
            item.fd = fd;
            item.events = POLLOUT;
            item.revents = 0;
            const int ready = ::poll(&item, 1, static_cast<int>(kPollSliceMs));
#endif
            if (ready < 0) break;
            if (ready == 0) continue;
            int soError = 0;
            socklen_t len = sizeof(soError);
            if (::getsockopt(fd, SOL_SOCKET, SO_ERROR,
                             reinterpret_cast<char *>(&soError), &len) == 0 &&
                soError == 0) {
                established = true;
            }
            break;
        }
        if (established && setBlocking(fd, true)) {
            /* Disable Nagle immediately post-connect: lobby commands and
             * /connect state frames are all small (~300 B), and Nagle +
             * delayed ACK adds up to ~40-200 ms per request against the
             * live service. Set-failure is non-fatal -- coalescing merely
             * stays on (the SO_NOSIGPIPE discipline above). */
            int noDelay = 1;
            (void)::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY,
                               reinterpret_cast<const char *>(&noDelay),
                               sizeof(noDelay));
            break;
        }
        closeSocket(fd);
        fd = kBadSocket;
        if (abort != nullptr && abort->load()) break;
    }
    return fd;
}

/* First matching HTTP header value (case-insensitive name), trimmed. */
std::string loweredCopy(std::string value) {
    for (char &byte : value)
        byte = static_cast<char>(std::tolower(static_cast<unsigned char>(byte)));
    return value;
}
bool responseHeader(const std::string &head, const std::string &name,
                    std::string &value) {
    size_t start = head.find("\r\n");
    while (start != std::string::npos && start + 2u < head.size()) {
        start += 2u;
        size_t end = head.find("\r\n", start);
        if (end == std::string::npos) end = head.size();
        const std::string line = head.substr(start, end - start);
        const size_t colon = line.find(':');
        if (colon != std::string::npos &&
            loweredCopy(line.substr(0u, colon)) == name) {
            value = line.substr(colon + 1u);
            while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
                value.erase(0u, 1u);
            while (!value.empty() && (value.back() == ' ' || value.back() == '\t'))
                value.pop_back();
            return true;
        }
        start = end;
    }
    return false;
}

/* ---- Origin parsing (mirrors match_signal_client.cpp::parseOrigin) -------- */

struct ParsedOrigin {
    bool tls = false;
    std::string host;
    std::string port;       /* decimal, for mbedtls_net_connect */
    std::string hostHeader; /* host[:port] for the HTTP Host header */
};

bool parseOrigin(const std::string &origin, ParsedOrigin &out) {
    std::string rest;
    if (origin.rfind("https://", 0) == 0) {
        out.tls = true;
        rest = origin.substr(8);
    } else if (origin.rfind("wss://", 0) == 0) {
        out.tls = true;
        rest = origin.substr(6);
    } else if (origin.rfind("http://", 0) == 0) {
        out.tls = false;
        rest = origin.substr(7);
        if (!mdkr_party_loopback_test_url_allowed(origin)) return false;
    } else if (origin.rfind("ws://", 0) == 0) {
        out.tls = false;
        rest = "http://" + origin.substr(5);
        if (!mdkr_party_loopback_test_url_allowed(rest)) return false;
        rest = origin.substr(5);
    } else {
        return false;
    }
    /* Strip any path. */
    const size_t slash = rest.find('/');
    if (slash != std::string::npos) rest = rest.substr(0, slash);
    if (rest.empty()) return false;
    std::string host = rest;
    std::string port = out.tls ? "443" : "80";
    const size_t colon = rest.rfind(':');
    if (colon != std::string::npos) {
        host = rest.substr(0, colon);
        port = rest.substr(colon + 1);
        if (host.empty() || port.empty()) return false;
        for (char c : port)
            if (c < '0' || c > '9') return false;
    }
    out.host = host;
    out.port = port;
    const bool defaultPort =
        (out.tls && port == "443") || (!out.tls && port == "80");
    out.hostHeader = defaultPort ? host : host + ":" + port;
    return true;
}

/* ---- mbedtls socket (plaintext or TLS), blocking with per-read timeout ----- */

class WireSocket {
public:
    WireSocket() {
        mbedtls_net_init(&net_);
        mbedtls_entropy_init(&entropy_);
        mbedtls_ctr_drbg_init(&drbg_);
        mbedtls_ssl_init(&ssl_);
        mbedtls_ssl_config_init(&conf_);
        mbedtls_x509_crt_init(&ca_);
    }
    ~WireSocket() { close(); }
    WireSocket(const WireSocket &) = delete;
    WireSocket &operator=(const WireSocket &) = delete;

    bool connect(const ParsedOrigin &origin, uint64_t deadline,
                 const std::atomic<bool> *abort) {
        abort_ = abort;
        const uint64_t connectDeadline =
            deadline < nowMs() + kConnectTimeoutMs ? deadline
                                                   : nowMs() + kConnectTimeoutMs;
        const SocketFd fd = connectWithDeadline(origin.host, origin.port,
                                                connectDeadline, abort);
        if (fd == kBadSocket) return false;
        net_.fd = static_cast<int>(fd);
        setSocketTimeouts(fd, static_cast<uint32_t>(kWriteTimeoutMs));
        tls_ = origin.tls;
        if (!tls_) {
            open_ = true;
            return true;
        }
        if (mbedtls_ctr_drbg_seed(&drbg_, mbedtls_entropy_func, &entropy_,
                                  nullptr, 0) != 0) {
            return false;
        }
        if (mbedtls_x509_crt_parse(
                &ca_, kMdkrMozillaCaBundle,
                static_cast<size_t>(kMdkrMozillaCaBundleLength) + 1u) != 0) {
            return false;
        }
        if (mbedtls_ssl_config_defaults(&conf_, MBEDTLS_SSL_IS_CLIENT,
                                        MBEDTLS_SSL_TRANSPORT_STREAM,
                                        MBEDTLS_SSL_PRESET_DEFAULT) != 0) {
            return false;
        }
        mbedtls_ssl_conf_authmode(&conf_, MBEDTLS_SSL_VERIFY_REQUIRED);
        mbedtls_ssl_conf_ca_chain(&conf_, &ca_, nullptr);
        mbedtls_ssl_conf_rng(&conf_, mbedtls_ctr_drbg_random, &drbg_);
        if (mbedtls_ssl_setup(&ssl_, &conf_) != 0) return false;
        if (mbedtls_ssl_set_hostname(&ssl_, origin.host.c_str()) != 0) {
            return false;
        }
        mbedtls_ssl_set_bio(&ssl_, &net_, mbedtls_net_send, nullptr,
                            mbedtls_net_recv_timeout);
        int ret = 0;
        while ((ret = mbedtls_ssl_handshake(&ssl_)) != 0) {
            if (ret != MBEDTLS_ERR_SSL_WANT_READ &&
                ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
                return false;
            }
            if (nowMs() > deadline || (abort_ && abort_->load())) return false;
        }
        if (mbedtls_ssl_get_verify_result(&ssl_) != 0) return false;
        open_ = true;
        return true;
    }

    /* Deadline-bounded write: SO_SNDTIMEO caps each stalled send() and the
     * deadline caps the whole transfer, so an unresponsive host cannot block
     * the worker thread past `deadline`. */
    bool writeAll(const uint8_t *data, size_t len, uint64_t deadline) {
        size_t sent = 0u;
        while (sent < len) {
            int n;
            if (tls_) {
                n = mbedtls_ssl_write(&ssl_, data + sent, len - sent);
            } else {
                n = mbedtls_net_send(&net_, data + sent, len - sent);
            }
            if (n == MBEDTLS_ERR_SSL_WANT_READ ||
                n == MBEDTLS_ERR_SSL_WANT_WRITE) {
                if (nowMs() >= deadline || (abort_ && abort_->load())) {
                    return false;
                }
                continue;
            }
            if (n <= 0) return false;
            sent += static_cast<size_t>(n);
        }
        return true;
    }
    bool writeAll(const std::string &s, uint64_t deadline) {
        return writeAll(reinterpret_cast<const uint8_t *>(s.data()), s.size(),
                        deadline);
    }

    /* Returns bytes read (>0), 0 on timeout, -1 on close/error. */
    int read(uint8_t *buf, size_t len, uint32_t timeoutMs) {
        int n;
        if (tls_) {
            mbedtls_ssl_conf_read_timeout(&conf_, timeoutMs);
            n = mbedtls_ssl_read(&ssl_, buf, len);
        } else {
            n = mbedtls_net_recv_timeout(&net_, buf, len, timeoutMs);
        }
        if (n == MBEDTLS_ERR_SSL_TIMEOUT || n == MBEDTLS_ERR_SSL_WANT_READ ||
            n == MBEDTLS_ERR_SSL_WANT_WRITE) {
            return 0;
        }
        if (n <= 0) return -1;
        return n;
    }

    void close() {
        if (open_ && tls_) {
            (void)mbedtls_ssl_close_notify(&ssl_);
        }
        mbedtls_ssl_free(&ssl_);
        mbedtls_ssl_config_free(&conf_);
        mbedtls_x509_crt_free(&ca_);
        mbedtls_ctr_drbg_free(&drbg_);
        mbedtls_entropy_free(&entropy_);
        mbedtls_net_free(&net_);
        open_ = false;
    }

    mbedtls_ctr_drbg_context *drbg() { return &drbg_; }

private:
    static void setSocketTimeouts(SocketFd fd, uint32_t ms) {
#ifdef _WIN32
        DWORD value = ms;
        ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO,
                     reinterpret_cast<const char *>(&value), sizeof(value));
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO,
                     reinterpret_cast<const char *>(&value), sizeof(value));
#else
        struct timeval tv;
        tv.tv_sec = static_cast<time_t>(ms / 1000u);
        tv.tv_usec = static_cast<suseconds_t>((ms % 1000u) * 1000u);
        ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
    }

    mbedtls_net_context net_;
    mbedtls_entropy_context entropy_;
    mbedtls_ctr_drbg_context drbg_;
    mbedtls_ssl_context ssl_;
    mbedtls_ssl_config conf_;
    mbedtls_x509_crt ca_;
    const std::atomic<bool> *abort_ = nullptr;
    bool tls_ = false;
    bool open_ = false;
};

/* ---- HTTP/1.1 (Connection: close, read-until-close) ---------------------- */

struct HttpResult {
    bool ok = false; /* transport succeeded (a response arrived) */
    int status = 0;
    std::string body;
};

/* base64 via mbedtls. */
std::string base64(const uint8_t *data, size_t len) {
    size_t olen = 0u;
    mbedtls_base64_encode(nullptr, 0, &olen, data, len);
    std::string out(olen, '\0');
    if (mbedtls_base64_encode(reinterpret_cast<unsigned char *>(&out[0]),
                              out.size(), &olen, data, len) != 0) {
        return std::string();
    }
    out.resize(olen);
    return out;
}

/* HTTP/1.1 response parse (status line + optional chunked decode), factored
 * out of httpRequest so the N7 fuzzer drives the EXACT shipped parser.
 * Semantics unchanged: false until a complete head has arrived. */
bool parseHttpResponse(const std::string &raw, int &statusOut,
                       std::string &bodyOut) {
    if (raw.empty()) return false;
    const size_t headerEnd = raw.find("\r\n\r\n");
    if (headerEnd == std::string::npos) return false;
    const std::string statusLine = raw.substr(0, raw.find("\r\n"));
    const size_t sp = statusLine.find(' ');
    if (sp == std::string::npos) return false;
    statusOut = std::atoi(statusLine.c_str() + sp + 1);
    std::string bodyPart = raw.substr(headerEnd + 4u);
    /* Chunked transfer decode if present, else the body is verbatim. */
    if (raw.substr(0, headerEnd).find("Transfer-Encoding: chunked") !=
            std::string::npos ||
        raw.substr(0, headerEnd).find("transfer-encoding: chunked") !=
            std::string::npos) {
        std::string decoded;
        size_t p = 0u;
        while (p < bodyPart.size()) {
            const size_t eol = bodyPart.find("\r\n", p);
            if (eol == std::string::npos) break;
            const long size = std::strtol(bodyPart.substr(p, eol - p).c_str(),
                                          nullptr, 16);
            if (size <= 0) break;
            p = eol + 2u;
            if (p + static_cast<size_t>(size) > bodyPart.size()) break;
            decoded.append(bodyPart, p, static_cast<size_t>(size));
            p += static_cast<size_t>(size) + 2u;
        }
        bodyPart = decoded;
    }
    bodyOut = bodyPart;
    return true;
}

HttpResult httpRequest(const ParsedOrigin &origin, const std::string &method,
                       const std::string &path, const std::string *body,
                       const std::string *credential,
                       const std::string &originHeader,
                       const std::atomic<bool> *abort) {
    HttpResult result;
    WireSocket socket;
    const uint64_t deadline = nowMs() + 15000u;
    if (!socket.connect(origin, deadline, abort)) return result;
    std::string request = method + " " + path + " HTTP/1.1\r\n";
    request += "Host: " + origin.hostHeader + "\r\n";
    /* The MatchRoom lobby routes are HTTP POST with strict same-origin
     * enforcement (allowedOrigin: Origin == PARTY_ORIGIN); native must send the
     * Origin explicitly (a browser sends it automatically). The value is the
     * exact service origin this client is talking to. */
    request += "Origin: " + originHeader + "\r\n";
    request += "Accept: application/json\r\n";
    if (credential != nullptr) {
        request += "Authorization: Bearer " + *credential + "\r\n";
    }
    if (body != nullptr) {
        request += "Content-Type: application/json\r\n";
        request += "Content-Length: " + std::to_string(body->size()) + "\r\n";
    }
    request += "Connection: close\r\n\r\n";
    if (body != nullptr) request += *body;
    if (!socket.writeAll(request, deadline)) return result;

    std::string raw;
    uint8_t buf[4096];
    while (nowMs() < deadline && raw.size() < (1u << 20)) {
        if (abort != nullptr && abort->load()) return result;
        const int n = socket.read(buf, sizeof(buf), 1000u);
        if (n < 0) break; /* server closed after the response */
        if (n == 0) continue;
        raw.append(reinterpret_cast<char *>(buf), static_cast<size_t>(n));
    }
    if (!parseHttpResponse(raw, result.status, result.body)) return result;
    result.ok = true;
    return result;
}

/* ---- RFC 6455 client for /connect (read-only, masked control replies) ----- */

/* One byte-level decode step over the buffered inbound bytes -- the
 * WsConnection frame parser, factored out so the N7 fuzzer drives the EXACT
 * shipped parser. Semantics unchanged: RSV bits, a masked server frame, or
 * a frame declaring more than 1 MiB is a Violation; NeedMore reports the
 * total bytes required (`needed`) so the caller's stall-bounded fill keeps
 * its exact behavior; Frame sets `consumed` for the caller to erase. */
enum class WsFrameStatus { NeedMore, Frame, Violation };
struct WsFrameOut {
    bool fin = false;
    uint8_t opcode = 0u;
    std::string payload;
    size_t needed = 0u;
};

WsFrameStatus wsDecodeFrame(const std::vector<uint8_t> &inbound,
                            WsFrameOut &out, size_t &consumed) {
    out.needed = 2u;
    if (inbound.size() < 2u) return WsFrameStatus::NeedMore;
    const uint8_t b0 = inbound[0];
    const uint8_t b1 = inbound[1];
    if ((b0 & 0x70u) != 0u) return WsFrameStatus::Violation; /* RSV set */
    if ((b1 & 0x80u) != 0u) return WsFrameStatus::Violation; /* masked */
    uint64_t len = b1 & 0x7fu;
    size_t headerLen = 2u;
    if (len == 126u) headerLen = 4u;
    else if (len == 127u) headerLen = 10u;
    out.needed = headerLen;
    if (inbound.size() < headerLen) return WsFrameStatus::NeedMore;
    if (headerLen == 4u) {
        len = (static_cast<uint64_t>(inbound[2]) << 8) | inbound[3];
    } else if (headerLen == 10u) {
        len = 0u;
        for (unsigned i = 0u; i < 8u; ++i) {
            len = (len << 8) | inbound[2u + i];
        }
    }
    if (len > (1u << 20)) return WsFrameStatus::Violation;
    out.needed = headerLen + static_cast<size_t>(len);
    if (inbound.size() < out.needed) return WsFrameStatus::NeedMore;
    out.fin = (b0 & 0x80u) != 0u;
    out.opcode = b0 & 0x0fu;
    out.payload.assign(inbound.begin() + static_cast<long>(headerLen),
                       inbound.begin() +
                           static_cast<long>(headerLen + len));
    consumed = out.needed;
    return WsFrameStatus::Frame;
}

class WsConnection {
public:
    bool open(const ParsedOrigin &origin, const std::string &path,
              const std::string &subprotocol,
              const std::string &credentialSubprotocol,
              const std::atomic<bool> *abort) {
        abort_ = abort;
        const uint64_t deadline = nowMs() + 15000u;
        if (!socket_.connect(origin, deadline, abort)) return false;
        uint8_t nonce[16];
        if (mbedtls_ctr_drbg_random(socket_.drbg(), nonce, sizeof(nonce)) != 0) {
            return false;
        }
        const std::string key = base64(nonce, sizeof(nonce));
        std::string request = "GET " + path + " HTTP/1.1\r\n";
        request += "Host: " + origin.hostHeader + "\r\n";
        request += "Upgrade: websocket\r\nConnection: Upgrade\r\n";
        request += "Sec-WebSocket-Key: " + key + "\r\n";
        request += "Sec-WebSocket-Version: 13\r\n";
        request += "Sec-WebSocket-Protocol: " + subprotocol + ", " +
                   credentialSubprotocol + "\r\n\r\n";
        if (!socket_.writeAll(request, deadline)) return false;
        /* Read headers up to the blank line, keeping any trailing frame bytes. */
        std::string head;
        uint8_t buf[2048];
        while (nowMs() < deadline) {
            if (abort != nullptr && abort->load()) return false;
            const size_t marker = head.find("\r\n\r\n");
            if (marker != std::string::npos) {
                inbound_.assign(head.begin() +
                                    static_cast<long>(marker + 4u),
                                head.end());
                break;
            }
            const int n = socket_.read(buf, sizeof(buf), 1000u);
            if (n < 0) return false;
            if (n == 0) continue;
            head.append(reinterpret_cast<char *>(buf), static_cast<size_t>(n));
            if (head.size() > 65536u) return false;
        }
        if (head.find("\r\n\r\n") == std::string::npos) return false;
        /* RFC 6455 4.1 handshake validation (O-T1 discipline): 101 status,
         * Upgrade: websocket, Connection: upgrade, the exact accept key, the
         * versioned subprotocol, and NO negotiated extension (we offered none,
         * so any Sec-WebSocket-Extensions would silently misframe). */
        if (head.rfind("HTTP/1.1 101", 0) != 0) return false;
        std::string value;
        if (!responseHeader(head, "upgrade", value) ||
            loweredCopy(value) != "websocket") {
            return false;
        }
        if (!responseHeader(head, "connection", value) ||
            loweredCopy(value).find("upgrade") == std::string::npos) {
            return false;
        }
        if (!responseHeader(head, "sec-websocket-accept", value) ||
            value != acceptFor(key)) {
            return false;
        }
        if (!responseHeader(head, "sec-websocket-protocol", value) ||
            value != subprotocol) {
            return false;
        }
        if (responseHeader(head, "sec-websocket-extensions", value)) {
            return false; /* we negotiated none */
        }
        open_ = true;
        return true;
    }

    enum class Poll { None, Text, Closed };
    /* Non-terminal read: returns Text with a payload, None on quiet, Closed on
     * a close frame or transport error. */
    Poll poll(std::string &textOut, uint32_t timeoutMs) {
        for (;;) {
            uint8_t opcode = 0u;
            std::string payload;
            const int r = readFrame(opcode, payload, timeoutMs);
            if (r == 0) return Poll::None;
            if (r < 0) return Poll::Closed;
            if (opcode == 0x1u) { /* text */
                textOut = std::move(payload);
                return Poll::Text;
            }
            if (opcode == 0x8u) { /* close */
                /* Keep the application close code + reason so the service
                 * loop can tell a worker-contract 4000-class close (room
                 * gone: terminal) from a transport-shaped drop (reconnect). */
                if (payload.size() >= 2u) {
                    closeCode_ = static_cast<uint16_t>(
                        (static_cast<uint16_t>(
                             static_cast<uint8_t>(payload[0])) << 8u) |
                        static_cast<uint8_t>(payload[1]));
                    closeReason_ = payload.substr(2u);
                }
                return Poll::Closed;
            }
            if (opcode == 0x9u) {                     /* ping -> pong */
                sendControl(0xAu, payload);
                continue;
            }
            /* pong / continuation of a non-text message: ignore. */
        }
    }

    /* Application close code from a received close frame; 0 when the socket
     * died without one (EOF/reset/stall -- the transport-shaped closes). */
    uint16_t closeCode() const { return closeCode_; }
    const std::string &closeReason() const { return closeReason_; }

    void close() {
        if (open_) {
            sendControl(0x8u, std::string("\x03\xe8", 2)); /* 1000 */
        }
        socket_.close();
        open_ = false;
    }

private:
    static std::string acceptFor(const std::string &key) {
        const std::string magic =
            key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
        unsigned char digest[20];
        mbedtls_sha1(reinterpret_cast<const unsigned char *>(magic.data()),
                     magic.size(), digest);
        return base64(digest, sizeof(digest));
    }

    bool fill(uint32_t timeoutMs) {
        uint8_t buf[4096];
        const int n = socket_.read(buf, sizeof(buf), timeoutMs);
        if (n < 0) {
            closed_ = true;
            return false;
        }
        if (n == 0) return false; /* timeout, not closed */
        inbound_.insert(inbound_.end(), buf, buf + n);
        return true;
    }

    /* Block until `count` bytes are buffered. Returns 1 (have them), 0 (idle:
     * no frame in progress and nothing arrived -- normal quiet), or -1 (closed,
     * aborted, or a mid-frame STALL past the deadline). Once any frame byte has
     * arrived, a steady deadline bounds the wait so a server that sends a
     * partial frame then goes silent can never spin serviceLoop()/close(). */
    int ensure(size_t count, uint32_t timeoutMs, bool midFrame,
               uint64_t &midFrameDeadline) {
        while (inbound_.size() < count) {
            if (closed_) return -1;
            if (abort_ != nullptr && abort_->load()) return -1;
            const bool waitingMidFrame = midFrame || !inbound_.empty();
            if (waitingMidFrame && midFrameDeadline == 0u) {
                midFrameDeadline = nowMs() + kFrameTimeoutMs;
            }
            if (!fill(timeoutMs)) {
                if (closed_) return -1;
                if (!waitingMidFrame) return 0; /* idle */
                if (nowMs() >= midFrameDeadline) return -1; /* stalled */
            }
        }
        return 1;
    }

    /* Returns 1 with (opcode,payload), 0 on quiet, -1 on close/error/stall.
     * Assembles a fragmented message under a total cap. The byte-level
     * decode is the shared wsDecodeFrame (the N7 fuzzer drives the same
     * function); this wrapper owns the stall-bounded fill and the
     * cross-frame assembly cap, both unchanged. */
    int readFrame(uint8_t &opcodeOut, std::string &payloadOut,
                  uint32_t timeoutMs) {
        std::string message;
        uint8_t firstOpcode = 0u;
        bool started = false;
        uint64_t midFrameDeadline = 0u;
        for (;;) {
            WsFrameOut frame;
            size_t consumed = 0u;
            const WsFrameStatus status =
                wsDecodeFrame(inbound_, frame, consumed);
            if (status == WsFrameStatus::Violation) return -1;
            if (status == WsFrameStatus::NeedMore) {
                const int got = ensure(frame.needed, timeoutMs, started,
                                       midFrameDeadline);
                if (got <= 0) return got;
                continue;
            }
            inbound_.erase(inbound_.begin(),
                           inbound_.begin() + static_cast<long>(consumed));
            if (frame.opcode >= 0x8u) { /* control frame delivered now */
                opcodeOut = frame.opcode;
                payloadOut = std::move(frame.payload);
                return 1;
            }
            if (!started) {
                started = true;
                firstOpcode = frame.opcode; /* 0x1 text / 0x2 binary */
            }
            /* Total-message cap across continuation frames (memory-DoS guard). */
            if (message.size() + frame.payload.size() > kMaxWsMessageBytes) {
                return -1;
            }
            message += frame.payload;
            if (frame.fin) {
                opcodeOut = firstOpcode;
                payloadOut = std::move(message);
                return 1;
            }
        }
    }

    void sendControl(uint8_t opcode, const std::string &payload) {
        if (payload.size() > 125u) return;
        uint8_t mask[4];
        /* A client frame MUST be masked with fresh entropy; if the DRBG fails,
         * refuse to send rather than emit a predictably (zero-)masked frame. */
        if (mbedtls_ctr_drbg_random(socket_.drbg(), mask, sizeof(mask)) != 0) {
            return;
        }
        std::string frame;
        frame.push_back(static_cast<char>(0x80u | opcode));
        frame.push_back(static_cast<char>(0x80u | payload.size()));
        frame.append(reinterpret_cast<char *>(mask), 4u);
        for (size_t i = 0u; i < payload.size(); ++i) {
            frame.push_back(static_cast<char>(
                static_cast<uint8_t>(payload[i]) ^ mask[i % 4u]));
        }
        (void)socket_.writeAll(frame, nowMs() + kWriteTimeoutMs);
    }

    WireSocket socket_;
    std::vector<uint8_t> inbound_;
    const std::atomic<bool> *abort_ = nullptr;
    bool open_ = false;
    bool closed_ = false;
    uint16_t closeCode_ = 0u;
    std::string closeReason_;
};

/* ---- JSON -> MdkrOnlineLobby -------------------------------------------- */

/* Bounded u32 field read (the signal client's jsonU32 semantics: integers
 * and INTEGRAL floats in range; anything else reads as the fallback).
 * nlohmann's own value(key, 0u) static_casts a float straight to unsigned,
 * which is UB for out-of-range values -- flagged by the N7 UBSan fuzz lane
 * (4.4e19 into `revision`). */
uint32_t readU32(const Json &object, const char *key, uint32_t fallback) {
    if (!object.is_object()) return fallback;
    const auto it = object.find(key);
    if (it == object.end()) return fallback;
    if (it->is_number_unsigned()) {
        const uint64_t raw = it->get<uint64_t>();
        return raw <= 0xffffffffull ? static_cast<uint32_t>(raw) : fallback;
    }
    if (it->is_number_integer()) {
        const int64_t raw = it->get<int64_t>();
        return raw >= 0 && raw <= 0xffffffffll ? static_cast<uint32_t>(raw)
                                               : fallback;
    }
    if (it->is_number_float()) {
        const double raw = it->get<double>();
        if (!(raw >= 0.0) || raw > 4294967295.0 || std::floor(raw) != raw) {
            return fallback;
        }
        return static_cast<uint32_t>(raw);
    }
    return fallback;
}

bool parseU64(const Json &value, uint64_t &out) {
    if (value.is_string()) {
        try {
            out = std::stoull(value.get<std::string>());
            return true;
        } catch (...) {
            return false;
        }
    }
    if (value.is_number_unsigned()) {
        out = value.get<uint64_t>();
        return true;
    }
    return false;
}

bool parseByteArray(const Json &value, uint8_t *out, size_t len) {
    if (!value.is_array() || value.size() != len) return false;
    for (size_t i = 0u; i < len; ++i) {
        if (!value[i].is_number_integer() && !value[i].is_number_unsigned()) {
            return false;
        }
        const long v = value[i].get<long>();
        if (v < 0 || v > 255) return false;
        out[i] = static_cast<uint8_t>(v);
    }
    return true;
}

bool phaseFromString(const std::string &s, MdkrOnlinePhase &out) {
    if (s == "lobby") out = MDKR_ONLINE_LOBBY;
    else if (s == "loading") out = MDKR_ONLINE_LOADING;
    else if (s == "racing") out = MDKR_ONLINE_RACING;
    else if (s == "results") out = MDKR_ONLINE_RESULTS;
    else if (s == "closed") out = MDKR_ONLINE_CLOSED;
    else return false;
    return true;
}

bool parseLobby(const Json &root, MdkrOnlineLobby &lobby) {
    if (!root.contains("lobby") || !root["lobby"].is_object()) return false;
    const Json &l = root["lobby"];
    std::memset(&lobby, 0, sizeof(lobby));
    try {
        lobby.protocol_version = readU32(l, "protocolVersion", 0u);
        lobby.revision = readU32(l, "revision", 0u);
        lobby.match_epoch = readU32(l, "matchEpoch", 0u);
        lobby.leader_generation = readU32(l, "leaderGeneration", 0u);
        if (!parseU64(l.at("roomId"), lobby.room_id) ||
            !parseU64(l.at("leaderEndpointId"), lobby.leader_endpoint_id)) {
            return false;
        }
        MdkrOnlinePhase phase;
        if (!l.contains("phase") || !l["phase"].is_string() ||
            !phaseFromString(l["phase"].get<std::string>(), phase)) {
            return false;
        }
        lobby.phase = phase;
        const Json &c = l.at("compatibility");
        lobby.compatibility.protocol_version = readU32(c, "protocolVersion", 0u);
        if (!parseByteArray(c.at("buildId"), lobby.compatibility.build_id, 16u) ||
            !parseByteArray(c.at("gameplayDigest"),
                            lobby.compatibility.gameplay_digest, 32u)) {
            return false;
        }
        lobby.compatibility.rom_revision =
            static_cast<uint8_t>(readU32(c, "romRevision", 0u));
        lobby.compatibility.cadence_hz =
            static_cast<uint8_t>(readU32(c, "cadenceHz", 0u));

        const Json &members = l.at("members");
        if (!members.is_array() || members.empty() ||
            members.size() > MDKR_ONLINE_MAX_ENDPOINTS) {
            return false;
        }
        for (size_t i = 0u; i < members.size(); ++i) {
            MdkrOnlineMember &m = lobby.members[i];
            if (!parseU64(members[i].at("endpointId"), m.endpoint_id)) {
                return false;
            }
            m.seat_count =
                static_cast<uint8_t>(readU32(members[i], "seatCount", 0u));
            m.connected = members[i].value("connected", false);
            m.ready = members[i].value("ready", false);
            m.loaded = members[i].value("loaded", false);
            m.occupied = true;
        }
        lobby.member_count = static_cast<uint8_t>(members.size());

        const Json &seats = l.at("seats");
        if (!seats.is_array() || seats.empty() ||
            seats.size() > MDKR_ONLINE_MAX_SEATS) {
            return false;
        }
        for (size_t i = 0u; i < seats.size(); ++i) {
            MdkrOnlineSeat &s = lobby.seats[i];
            if (!parseU64(seats[i].at("endpointId"), s.endpoint_id)) {
                return false;
            }
            s.selection_revision = readU32(seats[i], "selectionRevision", 0u);
            s.local_index =
                static_cast<uint8_t>(readU32(seats[i], "localIndex", 0u));
            const Json &vote = seats[i].value("voteTrack", Json());
            s.vote_track = vote.is_number_integer() || vote.is_number_unsigned()
                               ? static_cast<uint16_t>(vote.get<unsigned>())
                               : MDKR_ONLINE_NO_VOTE;
            const Json &ch = seats[i].value("characterId", Json());
            s.character_id = ch.is_number_integer() || ch.is_number_unsigned()
                                 ? static_cast<uint8_t>(ch.get<unsigned>())
                                 : MDKR_ONLINE_NO_CHARACTER;
            const Json &veh = seats[i].value("vehicleId", Json());
            s.vehicle_id = veh.is_number_integer() || veh.is_number_unsigned()
                               ? static_cast<uint8_t>(veh.get<unsigned>())
                               : MDKR_ONLINE_NO_VEHICLE;
            s.occupied = true;
        }
        lobby.seat_count = static_cast<uint8_t>(seats.size());

        const Json &track = l.value("selectedTrack", Json());
        lobby.selected_track =
            track.is_number_integer() || track.is_number_unsigned()
                ? static_cast<uint16_t>(track.get<unsigned>())
                : MDKR_ONLINE_NO_VOTE;
        lobby.selected_vehicle_mask =
            static_cast<uint8_t>(readU32(l, "selectedVehicleMask", 0u));
        lobby.next_receipt = 0u;
    } catch (...) {
        return false;
    }
    return mdkr_online_lobby_valid(&lobby);
}

void parseIceServers(const Json &root,
                     std::vector<MdkrMatchPeerIceServer> &out) {
    out.clear();
    if (!root.contains("iceServers") || !root["iceServers"].is_array()) return;
    for (const Json &entry : root["iceServers"]) {
        if (!entry.is_object() || !entry.contains("urls")) continue;
        /* Json-default value() + typed read: a wrong-typed username or
         * credential is skipped, never a worker-thread throw (N7). */
        const Json user = entry.value("username", Json());
        const Json cred = entry.value("credential", Json());
        std::string username =
            user.is_string() ? user.get<std::string>() : std::string();
        std::string credential =
            cred.is_string() ? cred.get<std::string>() : std::string();
        const Json &urls = entry["urls"];
        auto push = [&](const std::string &url) {
            MdkrMatchPeerIceServer server;
            server.url = url;
            server.username = username;
            server.credential = credential;
            out.push_back(std::move(server));
        };
        if (urls.is_string()) {
            push(urls.get<std::string>());
        } else if (urls.is_array()) {
            for (const Json &u : urls)
                if (u.is_string()) push(u.get<std::string>());
        }
    }
}

MdkrOnlineError mapMatchError(const std::string &code) {
    if (code == "ok") return MDKR_ONLINE_OK;
    if (code == "stale_revision") return MDKR_ONLINE_ERROR_STALE_REVISION;
    if (code == "stale_command") return MDKR_ONLINE_ERROR_STALE_COMMAND;
    if (code == "command_conflict") return MDKR_ONLINE_ERROR_COMMAND_CONFLICT;
    if (code == "incompatible") return MDKR_ONLINE_ERROR_INCOMPATIBLE;
    if (code == "capacity") return MDKR_ONLINE_ERROR_CAPACITY;
    if (code == "unauthorized") return MDKR_ONLINE_ERROR_UNAUTHORIZED;
    if (code == "not_found") return MDKR_ONLINE_ERROR_NOT_FOUND;
    if (code == "not_ready") return MDKR_ONLINE_ERROR_NOT_READY;
    if (code == "invalid_state") return MDKR_ONLINE_ERROR_INVALID_STATE;
    if (code == "selection_conflict")
        return MDKR_ONLINE_ERROR_SELECTION_CONFLICT;
    if (code == "illegal_vehicle") return MDKR_ONLINE_ERROR_ILLEGAL_VEHICLE;
    return MDKR_ONLINE_ERROR_PROTOCOL;
}

/* Step extraction for a /command response body (drainCommands' shape,
 * shared with the N7 fuzz seam so the two can never drift). The WHOLE parse
 * sits behind one catch: nlohmann's value() throws type_error.302 when a
 * key is present with the wrong type ({"error": 0}), and pre-N7 that throw
 * escaped the worker thread as std::terminate -- found by the online-wire
 * fuzzer at exec #352448 of its first smoke. A wrong-typed field is the
 * same typed PROTOCOL refusal as unparseable JSON. */
void parseCommandStep(const std::string &body, MdkrOnlineStep &step) {
    try {
        const Json root = Json::parse(body);
        step.accepted = root.value("accepted", false);
        step.duplicate = root.value("duplicate", false);
        step.error =
            mapMatchError(root.value("error", std::string("protocol")));
        step.revision = readU32(root, "revision", 0u);
        step.match_epoch = readU32(root, "matchEpoch", 0u);
    } catch (...) {
        step = MdkrOnlineStep{};
        step.accepted = false;
        step.error = MDKR_ONLINE_ERROR_PROTOCOL;
    }
}

const char *commandTypeName(MdkrOnlineCommandType type) {
    switch (type) {
        case MDKR_ONLINE_SET_READY: return "set_ready";
        case MDKR_ONLINE_SET_VOTE: return "set_vote";
        case MDKR_ONLINE_BEGIN_LOADING: return "begin_loading";
        case MDKR_ONLINE_ACK_LOADED: return "ack_loaded";
        case MDKR_ONLINE_BEGIN_RACE: return "begin_race";
        case MDKR_ONLINE_PUBLISH_RESULTS: return "publish_results";
        case MDKR_ONLINE_REMATCH: return "rematch";
        case MDKR_ONLINE_TRANSFER_LEADER: return "transfer_leader";
        case MDKR_ONLINE_CLOSE: return "close";
        case MDKR_ONLINE_SET_CHARACTER: return "set_character";
        case MDKR_ONLINE_SET_VEHICLE: return "set_vehicle";
        case MDKR_ONLINE_CANCEL_LOADING: return "cancel_loading";
        case MDKR_ONLINE_LEAVE: return "leave";
        case MDKR_ONLINE_DISCONNECT: return "disconnect";
        case MDKR_ONLINE_RECONNECT: return "reconnect";
        case MDKR_ONLINE_JOIN: return "join";
        default: return nullptr;
    }
}

/* Native compatibility -> the worker's exact 5-key JSON compatibility object. */
Json compatibilityJson(const MdkrOnlineCompatibilityV1 &c) {
    Json out;
    out["protocolVersion"] = c.protocol_version;
    out["buildId"] = Json::array();
    for (unsigned i = 0u; i < sizeof(c.build_id); ++i)
        out["buildId"].push_back(c.build_id[i]);
    out["gameplayDigest"] = Json::array();
    for (unsigned i = 0u; i < sizeof(c.gameplay_digest); ++i)
        out["gameplayDigest"].push_back(c.gameplay_digest[i]);
    out["romRevision"] = c.rom_revision;
    out["cadenceHz"] = c.cadence_hz;
    return out;
}

/* ---- The room transport -------------------------------------------------- */

class RoomHttpTransport final : public MdkrOnlineRoomTransport {
public:
    explicit RoomHttpTransport(const ParsedOrigin &origin, std::string origString)
        : origin_(origin), originString_(std::move(origString)) {
        worker_ = std::thread([this]() { run(); });
    }
    ~RoomHttpTransport() override { close(); }

    bool beginCreate(const MdkrOnlineCompatibilityV1 &compatibility,
                     unsigned seatCount) override {
        return begin(Kind::Create, std::string(), std::string(), compatibility,
                     seatCount);
    }
    bool beginJoin(const std::string &capability,
                   const MdkrOnlineCompatibilityV1 &compatibility,
                   unsigned seatCount) override {
        return begin(Kind::Join, capability, std::string(), compatibility,
                     seatCount);
    }
    bool beginJoinByCode(const std::string &code,
                         const MdkrOnlineCompatibilityV1 &compatibility,
                         unsigned seatCount) override {
        return begin(Kind::JoinByCode, std::string(), code, compatibility,
                     seatCount);
    }

    bool submitCommand(const MdkrOnlineCommand &command) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stop_) return false;
        outbound_.push_back(command);
        return true;
    }

    void pump(std::vector<MdkrOnlineRoomEvent> &out) override {
        out.clear();
        std::lock_guard<std::mutex> lock(mutex_);
        for (MdkrOnlineRoomEvent &ev : events_) out.push_back(std::move(ev));
        events_.clear();
    }

    void close() override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stop_) return;
            stop_ = true;
        }
        /* Abort any in-flight connect/handshake/read/write on the worker thread
         * within a poll slice so join() returns promptly even against a stalled
         * or unreachable host. */
        aborting_.store(true);
        if (worker_.joinable()) worker_.join();
    }

    bool invite(MdkrOnlineRoomHttpInvite *out) {
        std::lock_guard<std::mutex> lock(mutex_);
        *out = invite_;
        return invite_.ready;
    }

private:
    enum class Kind { None, Create, Join, JoinByCode };

    bool begin(Kind kind, const std::string &capability,
               const std::string &code,
               const MdkrOnlineCompatibilityV1 &compatibility,
               unsigned seatCount) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stop_ || kind_ != Kind::None) return false;
        kind_ = kind;
        capability_ = capability;
        code_ = code;
        compatibility_ = compatibility;
        seatCount_ = seatCount;
        return true;
    }

    void enqueue(MdkrOnlineRoomEvent &&ev) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (events_.size() < 256u) events_.push_back(std::move(ev));
    }

    void enqueueFailure(MdkrOnlineViewFailure failure) {
        MdkrOnlineRoomEvent ev;
        ev.type = MdkrOnlineRoomEvent::Type::Failure;
        ev.failure = failure;
        enqueue(std::move(ev));
    }

    void run() {
        /* Wait for the adapter's begin* call. */
        Kind kind = Kind::None;
        std::string capability, code;
        MdkrOnlineCompatibilityV1 compatibility{};
        unsigned seatCount = 1u;
        while (true) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (stop_) return;
                if (kind_ != Kind::None) {
                    kind = kind_;
                    capability = capability_;
                    code = code_;
                    compatibility = compatibility_;
                    seatCount = seatCount_;
                }
            }
            if (kind != Kind::None) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }

        Json body;
        std::string path;
        if (kind == Kind::Create) {
            path = "/api/match/create";
            body["compatibility"] = compatibilityJson(compatibility);
            body["seatCount"] = seatCount;
        } else if (kind == Kind::Join) {
            path = "/api/match/join";
            body["capability"] = capability;
            body["compatibility"] = compatibilityJson(compatibility);
            body["seatCount"] = seatCount;
        } else {
            path = "/api/match/code";
            body["code"] = code;
            body["compatibility"] = compatibilityJson(compatibility);
            body["seatCount"] = seatCount;
        }
        const std::string bodyStr = body.dump();
        const HttpResult res =
            httpRequest(origin_, "POST", path, &bodyStr, nullptr, originString_,
                        &aborting_);
        if (!res.ok) {
            enqueueFailure(MDKR_ONLINE_VIEW_FAILURE_SERVICE_UNAVAILABLE);
            return;
        }
        if (res.status != 201) {
            enqueueFailure(statusToFailure(res.status, res.body));
            return;
        }
        Json root;
        try {
            root = Json::parse(res.body);
        } catch (...) {
            enqueueFailure(MDKR_ONLINE_VIEW_FAILURE_SERVICE_UNAVAILABLE);
            return;
        }
        std::string roomId, credential, endpointId;
        try {
            roomId = root.at("roomId").get<std::string>();
            credential = root.at("credential").get<std::string>();
            endpointId = root.at("endpointId").get<std::string>();
        } catch (...) {
            enqueueFailure(MDKR_ONLINE_VIEW_FAILURE_SERVICE_UNAVAILABLE);
            return;
        }
        MdkrOnlineLobby lobby;
        if (!parseLobby(root, lobby)) {
            enqueueFailure(MDKR_ONLINE_VIEW_FAILURE_SERVICE_UNAVAILABLE);
            return;
        }
        std::vector<MdkrMatchPeerIceServer> iceServers;
        parseIceServers(root, iceServers);

        {
            std::lock_guard<std::mutex> lock(mutex_);
            invite_.ready = true;
            invite_.roomId = roomId;
            invite_.credential = credential;
            invite_.endpointId = endpointId;
            /* Json-default value(): no conversion, so a wrong-typed field
             * can never throw here (the N7 hazard); non-strings read as
             * absent. */
            const Json fallback = root.value("fallbackCode", Json());
            invite_.fallbackCode =
                fallback.is_string() ? fallback.get<std::string>()
                                     : std::string();
            const Json inviteUrl = root.value("inviteUrl", Json());
            invite_.inviteUrl = inviteUrl.is_string()
                                    ? inviteUrl.get<std::string>()
                                    : std::string();
        }

        roomId_ = roomId;
        credential_ = credential;
        try {
            localEndpointId_ = std::stoull(endpointId);
        } catch (...) {
            localEndpointId_ = 0u;
        }

        MdkrOnlineRoomEvent ready;
        ready.type = MdkrOnlineRoomEvent::Type::Ready;
        ready.localEndpointId = localEndpointId_;
        ready.roomId = roomId;
        ready.credential = credential;
        ready.iceServers = iceServers;
        ready.lobby = lobby;
        ready.haveLobby = true;
        lastRevision_ = lobby.revision;
        enqueue(std::move(ready));

        serviceLoop();
    }

    /* The /connect service loop (W3 N1).
     *
     * A transport-shaped loss of the push socket (EOF/reset, a stalled
     * frame, a failed open) is NOT the room ending: reopen it on the party
     * ladder's shape (300 ms doubling to 5 s, kReconnectMaxAttempts
     * consecutive failed opens) with the same credential. The fresh socket
     * re-delivers the current state and lastRevision_ dedupe makes that
     * idempotent, so nothing above this loop ever observes the blip.
     * Application closes are different: the worker ends /connect with a
     * 4000-class code (match-room.ts: 4000 room_expired / host_closed,
     * 4001 membership/replacement, 4003 protocol) -- those are the service
     * declaring this subscription over, so they stay terminal immediately
     * and the ladder never reopens a room the service closed. Ladder
     * exhaustion lands on the same pre-existing terminal HOST_CLOSED path
     * a close used to take. Commands keep draining over HTTP between
     * reopen attempts. */
    void serviceLoop() {
        const std::string path = "/api/match/" + roomId_ + "/connect";
        std::unique_ptr<WsConnection> ws;
        bool wsOpen = false;
        bool everOpened = false;
        unsigned failedOpens = 0u;
        uint64_t nextOpenAtMs = nowMs();
        while (true) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (stop_) break;
            }
            drainCommands();
            if (!wsOpen) {
                if (nowMs() < nextOpenAtMs) {
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(20));
                    continue;
                }
                ws.reset(new WsConnection());
                if (ws->open(origin_, path, "gb-match-v1",
                             "gb-match." + credential_, &aborting_)) {
                    wsOpen = true;
                    everOpened = true;
                    failedOpens = 0u;
                    continue;
                }
                ws.reset();
                if (aborting_.load()) break;
                ++failedOpens;
                if (!everOpened && failedOpens == 1u) {
                    /* The lobby is usable over HTTP while the ladder runs;
                     * surface a soft recovery instead of tearing the room
                     * (the pre-existing first-open behavior). */
                    enqueueFailure(MDKR_ONLINE_VIEW_FAILURE_CONNECTION_CHECK);
                }
                if (failedOpens >= kReconnectMaxAttempts) {
                    enqueueFailure(MDKR_ONLINE_VIEW_FAILURE_HOST_CLOSED);
                    break;
                }
                nextOpenAtMs = nowMs() + reconnectDelayMs(failedOpens);
                continue;
            }
            std::string text;
            const WsConnection::Poll p = ws->poll(text, 40u);
            if (p == WsConnection::Poll::Closed) {
                const uint16_t code = ws->closeCode();
                const std::string reason = ws->closeReason();
                ws->close();
                ws.reset();
                wsOpen = false;
                if (roomClosedSeen_) break; /* failure already delivered */
                if (code >= 4000u && code <= 4999u) {
                    enqueueFailure(
                        reason == "room_expired"
                            ? MDKR_ONLINE_VIEW_FAILURE_ROOM_EXPIRED
                            : MDKR_ONLINE_VIEW_FAILURE_HOST_CLOSED);
                    break;
                }
                nextOpenAtMs = nowMs() + kReconnectBaseDelayMs;
                continue;
            }
            if (p == WsConnection::Poll::Text) {
                applyStateFrame(text);
            }
        }
        if (ws) ws->close();
    }

    void applyStateFrame(const std::string &text) {
        Json root;
        try {
            root = Json::parse(text);
        } catch (...) {
            return;
        }
        /* value() on a non-object throws (type_error.306), and that throw
         * would escape into the worker thread: a state frame that is valid
         * JSON but not an object is just ignored. Found while wiring the
         * N7 fuzz lane over this parser; the lane pins the shape. */
        if (!root.is_object()) return;
        if (root.value("closedReason", Json()).is_string()) {
            const std::string reason =
                root["closedReason"].get<std::string>();
            /* The room itself ended: latch it so the WS close that follows
             * is treated as terminal, never re-laddered (N1). */
            roomClosedSeen_ = true;
            enqueueFailure(reason == "room_expired"
                               ? MDKR_ONLINE_VIEW_FAILURE_ROOM_EXPIRED
                               : MDKR_ONLINE_VIEW_FAILURE_HOST_CLOSED);
            return;
        }
        MdkrOnlineLobby lobby;
        if (!parseLobby(root, lobby)) return;
        if (lobby.revision == lastRevision_) return;
        lastRevision_ = lobby.revision;
        MdkrOnlineRoomEvent ev;
        ev.type = MdkrOnlineRoomEvent::Type::State;
        ev.lobby = lobby;
        ev.haveLobby = true;
        enqueue(std::move(ev));
    }

    void drainCommands() {
        std::deque<MdkrOnlineCommand> pending;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            pending.swap(outbound_);
        }
        for (const MdkrOnlineCommand &command : pending) {
            const char *typeName = commandTypeName(command.type);
            if (typeName == nullptr) continue;
            Json body;
            body["protocolVersion"] = MDKR_ONLINE_PROTOCOL_VERSION;
            body["expectedRevision"] = command.expected_revision;
            body["commandId"] = std::to_string(command.command_id);
            body["type"] = typeName;
            body["value"] = command.value;
            body["targetEndpointId"] =
                std::to_string(command.target_endpoint_id);
            const std::string bodyStr = body.dump();
            const std::string path = "/api/match/" + roomId_ + "/command";
            const HttpResult res =
                httpRequest(origin_, "POST", path, &bodyStr, &credential_,
                            originString_, &aborting_);
            MdkrOnlineRoomEvent ev;
            ev.type = MdkrOnlineRoomEvent::Type::CommandResult;
            if (!res.ok) {
                ev.step.accepted = false;
                ev.step.error = MDKR_ONLINE_ERROR_DISCONNECTED;
                enqueue(std::move(ev));
                continue;
            }
            parseCommandStep(res.body, ev.step);
            enqueue(std::move(ev));
        }
    }

    static MdkrOnlineViewFailure statusToFailure(int status,
                                                 const std::string &body) {
        std::string code;
        try {
            code = Json::parse(body).value("error", std::string());
        } catch (...) {
        }
        if (code == "incompatible")
            return MDKR_ONLINE_VIEW_FAILURE_DIFFERENT_BUILD;
        if (code == "capacity" || status == 409)
            return MDKR_ONLINE_VIEW_FAILURE_ROOM_FULL;
        if (code == "invite_expired")
            return MDKR_ONLINE_VIEW_FAILURE_INVITE_EXPIRED;
        if (code == "invalid_code" || code == "invalid_invite" || status == 404)
            return MDKR_ONLINE_VIEW_FAILURE_INVITE_EXPIRED;
        if (code == "service_budget_safe")
            return MDKR_ONLINE_VIEW_FAILURE_SERVICE_BUDGET_SAFE;
        return MDKR_ONLINE_VIEW_FAILURE_SERVICE_UNAVAILABLE;
    }

    ParsedOrigin origin_;
    std::string originString_;

    std::mutex mutex_;
    bool stop_ = false;
    std::atomic<bool> aborting_{false};
    Kind kind_ = Kind::None;
    std::string capability_;
    std::string code_;
    MdkrOnlineCompatibilityV1 compatibility_{};
    unsigned seatCount_ = 1u;
    std::deque<MdkrOnlineCommand> outbound_;
    std::deque<MdkrOnlineRoomEvent> events_;
    MdkrOnlineRoomHttpInvite invite_;

    /* Worker-thread-owned once Ready has been produced. */
    std::string roomId_;
    std::string credential_;
    uint64_t localEndpointId_ = 0u;
    uint32_t lastRevision_ = 0u;
    bool roomClosedSeen_ = false;

    std::thread worker_;
};

/* ---- The mesh signal backend -------------------------------------------- */

/* W3 N6b: the /signal socket's replacement path. The signal client itself is
 * deliberately one-shot (the JS reference client's contract: any terminal
 * failure latches), and replacement sockets are first-class in the wire
 * contract (docs/ref/match-signaling-v1.md: the service assigns the fresh
 * socket a HIGHER connection generation and announces it to peers as
 * presence). The mesh borrows exactly one MdkrMatchPeerSignalFeed pointer
 * for its whole life, so replacement lives HERE, inside the feed: when the
 * current client fails (silent death surfaces via its liveness ping, N6a),
 * the Failure event flows through to the mesh (which reports the SignalLost
 * status), and this feed then mints a replacement client on the party
 * ladder's shape (300 ms doubling to 5 s, kReconnectMaxAttempts consecutive
 * failures). The replacement's welcome -- the fresh generation -- reaches
 * the mesh through the same feed, which re-arms every recovery ladder
 * (match_peer_transport handleWelcome's re-welcome path). Launcher-thread
 * only, like every feed. */
class ReconnectingSignalFeed final : public MdkrMatchPeerSignalFeed {
public:
    explicit ReconnectingSignalFeed(MdkrMatchSignalClientOptions options)
        : options_(std::move(options)) {}
    ~ReconnectingSignalFeed() override {
        if (client_) client_->close();
    }

    bool start(std::string *error) {
        client_ = MdkrMatchSignalClient::create(options_, error);
        if (!client_) return false;
        std::string code;
        if (!client_->connect(&code)) {
            if (error != nullptr) *error = code;
            client_.reset();
            return false;
        }
        return true;
    }

    MdkrMatchSignalSendResult send(
        const MdkrMatchSignalOutbound &message) override {
        if (!client_) {
            MdkrMatchSignalSendResult refused;
            refused.error = kMdkrMatchSignalNotConnected;
            return refused;
        }
        return client_->send(message);
    }

    void drainEvents(std::vector<MdkrMatchSignalEvent> &out) override {
        out.clear();
        if (client_) client_->drainEvents(out);
        for (const MdkrMatchSignalEvent &event : out) {
            if (event.type == MdkrMatchSignalEventType::Failure) {
                scheduleReplacement();
            } else if (event.type == MdkrMatchSignalEventType::Welcome) {
                /* Healthy (again): the ladder re-arms from scratch. */
                failedAttempts_ = 0u;
                replacementDue_ = false;
            }
        }
        if (replacementDue_ && nowMs() >= nextAttemptAtMs_) {
            replacementDue_ = false;
            replaceClient();
        }
    }

private:
    void scheduleReplacement() {
        if (failedAttempts_ >= kReconnectMaxAttempts) return; /* gave up */
        ++failedAttempts_;
        replacementDue_ = true;
        nextAttemptAtMs_ = nowMs() + reconnectDelayMs(failedAttempts_);
    }

    void replaceClient() {
        if (client_) client_->close();
        client_.reset();
        client_ = MdkrMatchSignalClient::create(options_, nullptr);
        std::string code;
        if (client_ != nullptr && client_->connect(&code)) {
            /* The outcome arrives on the queue: Welcome resets the ladder,
             * Failure schedules the next bounded rung. */
            return;
        }
        client_.reset();
        scheduleReplacement();
    }

    MdkrMatchSignalClientOptions options_;
    std::unique_ptr<MdkrMatchSignalClient> client_;
    unsigned failedAttempts_ = 0u;
    bool replacementDue_ = false;
    uint64_t nextAttemptAtMs_ = 0u;
};

class MeshSignalClientBackend final : public MdkrOnlineMeshSignalBackend {
public:
    explicit MeshSignalClientBackend(std::string origin)
        : origin_(std::move(origin)) {}
    ~MeshSignalClientBackend() override { reset(); }

    MdkrMatchPeerSignalFeed *beginSignaling(
        uint64_t localEndpointId, uint32_t /*generation adopted from welcome*/,
        const std::string &roomId, const std::string &credential,
        const std::vector<MdkrMatchPeerIceServer> & /*ice: mesh options*/)
        override {
        reset();
        MdkrMatchSignalClientOptions options;
        options.serviceOrigin = origin_;
        options.roomId = roomId;
        options.endpointId = std::to_string(localEndpointId);
        options.credential = credential;
        feed_.reset(new ReconnectingSignalFeed(std::move(options)));
        std::string error;
        if (!feed_->start(&error)) {
            feed_.reset();
            return nullptr;
        }
        return feed_.get();
    }

    void reset() override { feed_.reset(); }

private:
    std::string origin_;
    std::unique_ptr<ReconnectingSignalFeed> feed_;
};

}  // namespace

std::unique_ptr<MdkrOnlineRoomTransport> mdkr_online_room_http_transport_create(
    const std::string &origin, std::string *error) {
    ParsedOrigin parsed;
    if (!parseOrigin(origin, parsed)) {
        if (error) *error = "cross-origin lobby transport refused";
        return nullptr;
    }
    return std::unique_ptr<MdkrOnlineRoomTransport>(
        new RoomHttpTransport(parsed, origin));
}

bool mdkr_online_room_http_transport_invite(MdkrOnlineRoomTransport *transport,
                                            MdkrOnlineRoomHttpInvite *out) {
    if (transport == nullptr || out == nullptr) return false;
    RoomHttpTransport *room = dynamic_cast<RoomHttpTransport *>(transport);
    return room != nullptr && room->invite(out);
}

std::unique_ptr<MdkrOnlineMeshSignalBackend>
mdkr_online_mesh_signal_backend_create(const std::string &origin) {
    return std::unique_ptr<MdkrOnlineMeshSignalBackend>(
        new MeshSignalClientBackend(origin));
}

/* ---- Test seams (declarations in match_live_transport.h) ------------------ */

void mdkr_online_room_transport_prepend_address_for_test(const char *ip,
                                                         uint16_t port) {
    std::lock_guard<std::mutex> lock(resolverSeamMutex());
    g_prependAddressForTest = ip != nullptr ? ip : "";
    g_prependPortForTest = port;
}

void mdkr_online_room_transport_stall_resolver_for_test(unsigned ms) {
    g_resolverStallMsForTest.store(ms);
}

/* ---- Fuzz seam (declaration in match_live_transport.h) -------------------- */

void mdkr_online_room_fuzz_wire(const uint8_t *data, size_t size) {
    const std::string raw(reinterpret_cast<const char *>(data), size);

    /* Lane A: the HTTP/1.1 response parse, then the JSON -> lobby/ice
     * mapping over whatever body it yields (the create/join path). */
    int status = 0;
    std::string body;
    if (parseHttpResponse(raw, status, body)) {
        const Json root = Json::parse(body, nullptr, false);
        if (!root.is_discarded()) {
            MdkrOnlineLobby lobby;
            (void)parseLobby(root, lobby);
            std::vector<MdkrMatchPeerIceServer> ice;
            parseIceServers(root, ice);
        }
        /* The /command step extraction -- the SHARED production function,
         * which is exactly where the first smoke's terminate lived. */
        MdkrOnlineStep step{};
        parseCommandStep(body, step);
    }

    /* Lane B: the /connect WS frame decoder over the same bytes, with the
     * serviceLoop's state-frame JSON mapping on every complete message. */
    std::vector<uint8_t> inbound(data, data + size);
    size_t assembledBytes = 0u;
    for (;;) {
        WsFrameOut frame;
        size_t consumed = 0u;
        const WsFrameStatus frameStatus =
            wsDecodeFrame(inbound, frame, consumed);
        if (frameStatus != WsFrameStatus::Frame) break;
        inbound.erase(inbound.begin(),
                      inbound.begin() + static_cast<long>(consumed));
        if (frame.opcode >= 0x8u) continue; /* control: extracted, dropped */
        if (assembledBytes + frame.payload.size() > kMaxWsMessageBytes) break;
        assembledBytes += frame.payload.size();
        if (frame.fin) {
            const Json root = Json::parse(frame.payload, nullptr, false);
            if (!root.is_discarded()) {
                /* The serviceLoop's state-frame shape: closedReason first
                 * (object-guarded -- value() throws on non-objects), then
                 * the lobby mapping. */
                if (root.is_object()) {
                    (void)root.value("closedReason", Json()).is_string();
                }
                MdkrOnlineLobby lobby;
                (void)parseLobby(root, lobby);
            }
            assembledBytes = 0u;
        }
    }
}
