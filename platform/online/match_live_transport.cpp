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

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using Json = nlohmann::json;

uint64_t nowMs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
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

    bool connect(const ParsedOrigin &origin, uint64_t deadline) {
        if (mbedtls_net_connect(&net_, origin.host.c_str(), origin.port.c_str(),
                                MBEDTLS_NET_PROTO_TCP) != 0) {
            return false;
        }
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
            if (nowMs() > deadline) return false;
        }
        if (mbedtls_ssl_get_verify_result(&ssl_) != 0) return false;
        open_ = true;
        return true;
    }

    bool writeAll(const uint8_t *data, size_t len) {
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
                continue;
            }
            if (n <= 0) return false;
            sent += static_cast<size_t>(n);
        }
        return true;
    }
    bool writeAll(const std::string &s) {
        return writeAll(reinterpret_cast<const uint8_t *>(s.data()), s.size());
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
    mbedtls_net_context net_;
    mbedtls_entropy_context entropy_;
    mbedtls_ctr_drbg_context drbg_;
    mbedtls_ssl_context ssl_;
    mbedtls_ssl_config conf_;
    mbedtls_x509_crt ca_;
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

HttpResult httpRequest(const ParsedOrigin &origin, const std::string &method,
                       const std::string &path, const std::string *body,
                       const std::string *credential,
                       const std::string &originHeader) {
    HttpResult result;
    WireSocket socket;
    const uint64_t deadline = nowMs() + 15000u;
    if (!socket.connect(origin, deadline)) return result;
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
    if (!socket.writeAll(request)) return result;

    std::string raw;
    uint8_t buf[4096];
    while (nowMs() < deadline && raw.size() < (1u << 20)) {
        const int n = socket.read(buf, sizeof(buf), 1000u);
        if (n < 0) break; /* server closed after the response */
        if (n == 0) continue;
        raw.append(reinterpret_cast<char *>(buf), static_cast<size_t>(n));
    }
    if (raw.empty()) return result;
    const size_t headerEnd = raw.find("\r\n\r\n");
    if (headerEnd == std::string::npos) return result;
    const std::string statusLine = raw.substr(0, raw.find("\r\n"));
    const size_t sp = statusLine.find(' ');
    if (sp == std::string::npos) return result;
    result.status = std::atoi(statusLine.c_str() + sp + 1);
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
    result.body = bodyPart;
    result.ok = true;
    return result;
}

/* ---- RFC 6455 client for /connect (read-only, masked control replies) ----- */

class WsConnection {
public:
    bool open(const ParsedOrigin &origin, const std::string &path,
              const std::string &subprotocol,
              const std::string &credentialSubprotocol) {
        const uint64_t deadline = nowMs() + 15000u;
        if (!socket_.connect(origin, deadline)) return false;
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
        if (!socket_.writeAll(request)) return false;
        /* Read headers up to the blank line, keeping any trailing frame bytes. */
        std::string head;
        uint8_t buf[2048];
        while (nowMs() < deadline) {
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
        if (head.rfind("HTTP/1.1 101", 0) != 0) return false;
        if (head.find("Sec-WebSocket-Accept: " + acceptFor(key)) ==
            std::string::npos) {
            return false;
        }
        /* The server MUST select the versioned subprotocol. */
        if (head.find("Sec-WebSocket-Protocol: " + subprotocol) ==
            std::string::npos) {
            return false;
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
            if (opcode == 0x8u) return Poll::Closed; /* close */
            if (opcode == 0x9u) {                     /* ping -> pong */
                sendControl(0xAu, payload);
                continue;
            }
            /* pong / continuation of a non-text message: ignore. */
        }
    }

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
        if (n <= 0) return n == 0 ? false : throwClosed();
        inbound_.insert(inbound_.end(), buf, buf + n);
        return true;
    }
    bool throwClosed() {
        closed_ = true;
        return false;
    }

    /* Returns 1 with (opcode,payload), 0 on timeout, -1 on close/error. Handles
     * fragmentation into a single logical message. */
    int readFrame(uint8_t &opcodeOut, std::string &payloadOut,
                  uint32_t timeoutMs) {
        std::string message;
        uint8_t firstOpcode = 0u;
        bool started = false;
        const uint64_t deadline = nowMs() + timeoutMs + 2000u;
        for (;;) {
            /* Need at least a 2-byte header. */
            while (inbound_.size() < 2u) {
                if (closed_) return -1;
                if (!fill(timeoutMs)) {
                    if (closed_) return -1;
                    if (!started && nowMs() > deadline) return 0;
                    if (!started) return 0;
                    continue;
                }
            }
            const uint8_t b0 = inbound_[0];
            const uint8_t b1 = inbound_[1];
            const bool fin = (b0 & 0x80u) != 0u;
            const uint8_t opcode = b0 & 0x0fu;
            if ((b1 & 0x80u) != 0u) return -1; /* server frames must not mask */
            uint64_t len = b1 & 0x7fu;
            size_t headerLen = 2u;
            if (len == 126u) headerLen = 4u;
            else if (len == 127u) headerLen = 10u;
            while (inbound_.size() < headerLen) {
                if (!fill(timeoutMs) && closed_) return -1;
            }
            if (headerLen == 4u) {
                len = (static_cast<uint64_t>(inbound_[2]) << 8) | inbound_[3];
            } else if (headerLen == 10u) {
                len = 0u;
                for (unsigned i = 0u; i < 8u; ++i) {
                    len = (len << 8) | inbound_[2u + i];
                }
            }
            if (len > (1u << 20)) return -1;
            while (inbound_.size() < headerLen + len) {
                if (!fill(timeoutMs) && closed_) return -1;
            }
            std::string payload(inbound_.begin() +
                                    static_cast<long>(headerLen),
                                inbound_.begin() +
                                    static_cast<long>(headerLen + len));
            inbound_.erase(inbound_.begin(),
                           inbound_.begin() +
                               static_cast<long>(headerLen + len));
            if (opcode >= 0x8u) { /* control frame delivered immediately */
                opcodeOut = opcode;
                payloadOut = std::move(payload);
                return 1;
            }
            if (!started) {
                started = true;
                firstOpcode = opcode; /* 0x1 text / 0x2 binary */
            }
            message += payload;
            if (fin) {
                opcodeOut = firstOpcode;
                payloadOut = std::move(message);
                return 1;
            }
        }
    }

    void sendControl(uint8_t opcode, const std::string &payload) {
        if (payload.size() > 125u) return;
        std::string frame;
        frame.push_back(static_cast<char>(0x80u | opcode));
        frame.push_back(static_cast<char>(0x80u | payload.size()));
        uint8_t mask[4];
        if (mbedtls_ctr_drbg_random(socket_.drbg(), mask, sizeof(mask)) != 0) {
            std::memset(mask, 0, sizeof(mask));
        }
        frame.append(reinterpret_cast<char *>(mask), 4u);
        for (size_t i = 0u; i < payload.size(); ++i) {
            frame.push_back(static_cast<char>(
                static_cast<uint8_t>(payload[i]) ^ mask[i % 4u]));
        }
        (void)socket_.writeAll(frame);
    }

    WireSocket socket_;
    std::vector<uint8_t> inbound_;
    bool open_ = false;
    bool closed_ = false;
};

/* ---- JSON -> MdkrOnlineLobby -------------------------------------------- */

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
        lobby.protocol_version = l.value("protocolVersion", 0u);
        lobby.revision = l.value("revision", 0u);
        lobby.match_epoch = l.value("matchEpoch", 0u);
        lobby.leader_generation = l.value("leaderGeneration", 0u);
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
        lobby.compatibility.protocol_version = c.value("protocolVersion", 0u);
        if (!parseByteArray(c.at("buildId"), lobby.compatibility.build_id, 16u) ||
            !parseByteArray(c.at("gameplayDigest"),
                            lobby.compatibility.gameplay_digest, 32u)) {
            return false;
        }
        lobby.compatibility.rom_revision =
            static_cast<uint8_t>(c.value("romRevision", 0u));
        lobby.compatibility.cadence_hz =
            static_cast<uint8_t>(c.value("cadenceHz", 0u));

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
                static_cast<uint8_t>(members[i].value("seatCount", 0u));
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
            s.selection_revision = seats[i].value("selectionRevision", 0u);
            s.local_index =
                static_cast<uint8_t>(seats[i].value("localIndex", 0u));
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
            static_cast<uint8_t>(l.value("selectedVehicleMask", 0u));
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
        std::string username = entry.value("username", std::string());
        std::string credential = entry.value("credential", std::string());
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
            httpRequest(origin_, "POST", path, &bodyStr, nullptr, originString_);
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
            invite_.fallbackCode = root.value("fallbackCode", std::string());
            invite_.inviteUrl = root.value("inviteUrl", std::string());
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

    void serviceLoop() {
        WsConnection ws;
        const std::string path = "/api/match/" + roomId_ + "/connect";
        if (!ws.open(origin_, path, "gb-match-v1", "gb-match." + credential_)) {
            /* The lobby is usable over HTTP polling even if the push socket
             * failed; surface a soft recovery instead of tearing the room. */
            enqueueFailure(MDKR_ONLINE_VIEW_FAILURE_CONNECTION_CHECK);
        }
        while (true) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (stop_) break;
            }
            drainCommands();
            std::string text;
            const WsConnection::Poll p = ws.poll(text, 40u);
            if (p == WsConnection::Poll::Closed) {
                enqueueFailure(MDKR_ONLINE_VIEW_FAILURE_HOST_CLOSED);
                break;
            }
            if (p == WsConnection::Poll::Text) {
                applyStateFrame(text);
            }
        }
        ws.close();
    }

    void applyStateFrame(const std::string &text) {
        Json root;
        try {
            root = Json::parse(text);
        } catch (...) {
            return;
        }
        if (root.value("closedReason", Json()).is_string()) {
            const std::string reason =
                root["closedReason"].get<std::string>();
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
            const HttpResult res = httpRequest(origin_, "POST", path, &bodyStr,
                                               &credential_, originString_);
            MdkrOnlineRoomEvent ev;
            ev.type = MdkrOnlineRoomEvent::Type::CommandResult;
            if (!res.ok) {
                ev.step.accepted = false;
                ev.step.error = MDKR_ONLINE_ERROR_DISCONNECTED;
                enqueue(std::move(ev));
                continue;
            }
            Json root;
            try {
                root = Json::parse(res.body);
            } catch (...) {
                ev.step.accepted = false;
                ev.step.error = MDKR_ONLINE_ERROR_PROTOCOL;
                enqueue(std::move(ev));
                continue;
            }
            ev.step.accepted = root.value("accepted", false);
            ev.step.duplicate = root.value("duplicate", false);
            ev.step.error =
                mapMatchError(root.value("error", std::string("protocol")));
            ev.step.revision = root.value("revision", 0u);
            ev.step.match_epoch = root.value("matchEpoch", 0u);
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

    std::thread worker_;
};

/* ---- The mesh signal backend -------------------------------------------- */

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
        std::string error;
        client_ = MdkrMatchSignalClient::create(options, &error);
        if (!client_) return nullptr;
        std::string code;
        if (!client_->connect(&code)) {
            client_.reset();
            return nullptr;
        }
        feed_.reset(new MdkrMatchSignalClientFeed(client_.get()));
        return feed_.get();
    }

    void reset() override {
        feed_.reset();
        if (client_) {
            client_->close();
            client_.reset();
        }
    }

private:
    std::string origin_;
    std::unique_ptr<MdkrMatchSignalClient> client_;
    std::unique_ptr<MdkrMatchSignalClientFeed> feed_;
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
