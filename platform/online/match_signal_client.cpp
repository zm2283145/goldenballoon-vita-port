/*
 * Native match-signaling client. See match_signal_client.h for the contract
 * and for why the RFC 6455 client half is implemented here instead of on
 * rtc::WebSocket. The validation surface is a line-for-line mirror of
 * dist/web/online/match-signal-client.js; where a comment below names a JS
 * function, the behavior is that function's, byte-for-byte on the wire and
 * string-for-string in the failure codes.
 */
#include "online/match_signal_client.h"

#include "mozilla_ca_bundle.h"
#include "party/native_party_host.h"      /* mdkr_party_loopback_test_url_allowed */
#include "party/party_webrtc_signaling.h" /* mdkr_party::decodePublicKey */

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
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {

using Json = nlohmann::json;

/* The reference client's bounds (match-signal-client.js, and the $0
 * accounting table in docs/ref/match-signaling-v1.md). */
constexpr size_t kSignalBytes = 64u * 1024u;
constexpr size_t kSdpBytes = 60u * 1024u;
constexpr size_t kIceBytes = 4u * 1024u;
constexpr size_t kMetaBytes = 256u;
constexpr size_t kMaxTrackedPeerGenerations = 64u;
constexpr uint64_t kU32Max = 0xffffffffull;

/* Bounded launcher-facing event queue. The relay's own admission (60
 * messages / 10 s per sender, 256 lifetime, <= 3 peers) makes ~18 events/s
 * the hostile ceiling, so this is close to a minute of undrained backlog
 * before anything is evicted. Eviction never touches a Failure event -- the
 * terminal verdict must always reach the launcher. */
constexpr size_t kMaxQueuedEvents = 1024u;

/* Read-poll slice: every blocking wait on the socket thread wakes at this
 * cadence to honor stop/close requests and the welcome deadline. */
constexpr uint32_t kPollSliceMs = 20u;
/* Per-address connect cap (N3): one blackholed address (the classic
 * advertised-but-broken IPv6 route against a dual-stack service) may cost at
 * most min(this, remaining/addresses-left) before the loop falls through to
 * the next address. 3.5 s is comfortably past any healthy handshake RTT
 * while leaving the rest of the welcome budget for the working family. */
constexpr uint64_t kPerAddressConnectCapMs = 3500u;
/* Per-frame write budget. A healthy peer drains a 60 KiB signaling frame
 * in far less; a peer this stalled is already lost, and close() can abort
 * the wait sooner through the interrupt. */
constexpr uint32_t kDataWriteBudgetMs = 5000u;
/* The final close frame's bounded best effort. */
constexpr uint32_t kCloseFrameBudgetMs = 250u;
/* Client-originated liveness defaults (W3 N6a; see the options doc): ping
 * after this much inbound silence, declare the transport lost when nothing
 * inbound follows within the timeout on top. 20 s idle stays far above the
 * relay's own traffic cadence and well under common NAT UDP/TCP idle
 * reaping, and two probes fit inside a minute. */
constexpr unsigned kLivenessIdleDefaultMs = 20000u;
constexpr unsigned kLivenessTimeoutDefaultMs = 10000u;

uint64_t steadyNowMs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

/* ---- Validators (u32 / endpoint / text / exact / publicKey) -------------- */

bool base64UrlChars(const std::string &value) {
    for (const char byte : value) {
        const bool ok = (byte >= 'A' && byte <= 'Z') ||
                        (byte >= 'a' && byte <= 'z') ||
                        (byte >= '0' && byte <= '9') || byte == '-' ||
                        byte == '_';
        if (!ok) return false;
    }
    return true;
}

/* JS endpoint(): ^[1-9][0-9]{0,19}$ and <= 2^64-1. No leading zeros, so a
 * length-then-lexicographic compare is exact numeric order. */
bool endpointString(const std::string &value) {
    if (value.empty() || value.size() > 20u) return false;
    if (value[0] < '1' || value[0] > '9') return false;
    for (const char byte : value) {
        if (byte < '0' || byte > '9') return false;
    }
    if (value.size() == 20u && value > std::string("18446744073709551615")) {
        return false;
    }
    return true;
}

bool endpointNumericLess(const std::string &left, const std::string &right) {
    if (left.size() != right.size()) return left.size() < right.size();
    return left < right;
}

/* JS u32(): Number.isInteger(value) within [nonzero?1:0, 2^32-1]. JSON
 * numbers arrive typed in nlohmann; an integral float (JSON "5.0") is a JS
 * integer and stays one here. Booleans and strings are not numbers. */
bool jsonU32(const Json &value, bool nonzero, uint32_t &out) {
    uint64_t raw = 0u;
    if (value.is_number_unsigned()) {
        raw = value.get<uint64_t>();
    } else if (value.is_number_integer()) {
        const int64_t asSigned = value.get<int64_t>();
        if (asSigned < 0) return false;
        raw = static_cast<uint64_t>(asSigned);
    } else if (value.is_number_float()) {
        const double asDouble = value.get<double>();
        if (!(asDouble >= 0.0) || asDouble > 4294967295.0 ||
            std::floor(asDouble) != asDouble) {
            return false;
        }
        raw = static_cast<uint64_t>(asDouble);
    } else {
        return false;
    }
    if (raw > kU32Max || (nonzero && raw == 0u)) return false;
    out = static_cast<uint32_t>(raw);
    return true;
}

/* JS strict `=== <number>` against a known u32 (bool/string never equal). */
bool jsonNumberEquals(const Json &value, uint32_t target) {
    uint32_t raw = 0u;
    return jsonU32(value, false, raw) && raw == target;
}

/* RFC 3629 well-formedness (the exact table nlohmann's serializer
 * enforces): no overlongs, no surrogate code points, nothing past
 * U+10FFFF, no truncated sequences. A JS string can never hand the wire
 * invalid UTF-8; a C++ launcher can, and without this check the refusal
 * would surface as dump()'s type_error.316 escaping send() instead of the
 * documented value-refusal. */
bool validUtf8(const std::string &value) {
    size_t index = 0u;
    const size_t size = value.size();
    while (index < size) {
        const uint8_t lead = static_cast<uint8_t>(value[index]);
        size_t follow = 0u;
        uint8_t secondLow = 0x80u;
        uint8_t secondHigh = 0xbfu;
        if (lead <= 0x7fu) {
            index++;
            continue;
        } else if (lead >= 0xc2u && lead <= 0xdfu) {
            follow = 1u;
        } else if (lead == 0xe0u) {
            follow = 2u;
            secondLow = 0xa0u; /* refuse overlong 3-byte forms */
        } else if (lead >= 0xe1u && lead <= 0xecu) {
            follow = 2u;
        } else if (lead == 0xedu) {
            follow = 2u;
            secondHigh = 0x9fu; /* refuse surrogates U+D800..U+DFFF */
        } else if (lead >= 0xeeu && lead <= 0xefu) {
            follow = 2u;
        } else if (lead == 0xf0u) {
            follow = 3u;
            secondLow = 0x90u; /* refuse overlong 4-byte forms */
        } else if (lead >= 0xf1u && lead <= 0xf3u) {
            follow = 3u;
        } else if (lead == 0xf4u) {
            follow = 3u;
            secondHigh = 0x8fu; /* refuse > U+10FFFF */
        } else {
            return false; /* 0x80..0xc1, 0xf5..0xff are never leads */
        }
        if (index + follow >= size) return false;
        const uint8_t second = static_cast<uint8_t>(value[index + 1u]);
        if (second < secondLow || second > secondHigh) return false;
        for (size_t offset = 2u; offset <= follow; offset++) {
            const uint8_t byte = static_cast<uint8_t>(value[index + offset]);
            if (byte < 0x80u || byte > 0xbfu) return false;
        }
        index += follow + 1u;
    }
    return true;
}

/* JS text(): string, nonempty unless allowed, no NUL, UTF-8 bytes <= limit.
 * Inbound strings are already valid UTF-8 (nlohmann refused anything else
 * at parse); the well-formedness check is load-bearing for OUTBOUND
 * launcher-supplied strings, which are raw bytes. */
bool textString(const std::string &value, size_t limit, bool allowEmpty) {
    if (!allowEmpty && value.empty()) return false;
    if (value.find('\0') != std::string::npos) return false;
    return value.size() <= limit && validUtf8(value);
}

bool jsonText(const Json &value, size_t limit, bool allowEmpty,
              std::string &out) {
    if (!value.is_string()) return false;
    out = value.get<std::string>();
    return textString(out, limit, allowEmpty);
}

/* JS exact(): a plain object with exactly these keys. */
bool exactKeys(const Json &object, std::initializer_list<const char *> keys) {
    if (!object.is_object() || object.size() != keys.size()) return false;
    for (const char *key : keys) {
        if (!object.contains(key)) return false;
    }
    return true;
}

/* JS publicKey(): 87-char canonical base64url, 65 bytes, uncompressed
 * (0x04) with a nonzero body. mdkr_party::decodePublicKey enforces length,
 * alphabet, the canonical trailing bits and the 0x04 prefix; the nonzero
 * body check is added here to match the JS rule exactly. */
bool publicKeyString(const std::string &value) {
    std::array<uint8_t, 65> raw{};
    if (!mdkr_party::decodePublicKey(value, raw)) return false;
    for (size_t index = 1u; index < raw.size(); index++) {
        if (raw[index] != 0u) return true;
    }
    return false;
}

bool jsonStringEquals(const Json &value, const std::string &expected) {
    return value.is_string() &&
           value.get_ref<const std::string &>() == expected;
}

bool jsonEndpointNotSelf(const Json &value, const std::string &self,
                         std::string &out) {
    if (!value.is_string()) return false;
    out = value.get<std::string>();
    return endpointString(out) && out != self;
}

/* ---- parseServerMessage (the JS function, structured identically) -------- */

bool parseServerMessage(const Json &raw, const std::string &self,
                        uint32_t generation, MdkrMatchSignalEvent &out) {
    if (!raw.is_object()) return false;
    const auto version = raw.find("protocolVersion");
    if (version == raw.end() || !jsonNumberEquals(*version, 1u)) return false;
    const auto typeField = raw.find("type");
    if (typeField == raw.end() || !typeField->is_string()) return false;
    const std::string type = typeField->get<std::string>();

    if (type == "signal_welcome" && generation == 0u &&
        exactKeys(raw, {"protocolVersion", "type", "endpointId",
                        "connectionGeneration", "peers"}) &&
        jsonStringEquals(raw["endpointId"], self) &&
        jsonU32(raw["connectionGeneration"], true, out.connectionGeneration) &&
        raw["peers"].is_array() && raw["peers"].size() <= 3u) {
        out.peers.clear();
        for (const Json &item : raw["peers"]) {
            MdkrMatchSignalPeerRef peer;
            if (!exactKeys(item, {"endpointId", "connectionGeneration"}) ||
                !jsonEndpointNotSelf(item["endpointId"], self,
                                     peer.endpointId) ||
                !jsonU32(item["connectionGeneration"], true,
                         peer.connectionGeneration)) {
                return false;
            }
            out.peers.push_back(std::move(peer));
        }
        /* Strictly ascending numeric ids: sorted AND unique in one rule. */
        for (size_t index = 1u; index < out.peers.size(); index++) {
            if (!endpointNumericLess(out.peers[index - 1u].endpointId,
                                     out.peers[index].endpointId)) {
                return false;
            }
        }
        out.type = MdkrMatchSignalEventType::Welcome;
        out.endpointId = self;
        return true;
    }
    if (generation == 0u) return false;

    if (type == "peer_presence" &&
        exactKeys(raw, {"protocolVersion", "type", "endpointId",
                        "connectionGeneration", "present"}) &&
        jsonEndpointNotSelf(raw["endpointId"], self, out.endpointId) &&
        jsonU32(raw["connectionGeneration"], true, out.connectionGeneration) &&
        raw["present"].is_boolean()) {
        out.type = MdkrMatchSignalEventType::PeerPresence;
        out.present = raw["present"].get<bool>();
        return true;
    }

    if (type == "signal_error" &&
        exactKeys(raw, {"protocolVersion", "type", "sequence", "toEndpointId",
                        "toConnectionGeneration", "error"}) &&
        jsonU32(raw["sequence"], true, out.sequence) &&
        jsonEndpointNotSelf(raw["toEndpointId"], self, out.toEndpointId) &&
        jsonU32(raw["toConnectionGeneration"], true,
                out.toConnectionGeneration) &&
        jsonStringEquals(raw["error"], "peer_unavailable")) {
        out.type = MdkrMatchSignalEventType::SignalError;
        out.error = "peer_unavailable";
        return true;
    }

    /* Directed messages: common authenticated fields first, exactly like
     * the JS ordering (missing keys read as never-equal). */
    const auto sequenceField = raw.find("sequence");
    const auto toField = raw.find("toEndpointId");
    const auto toGenerationField = raw.find("toConnectionGeneration");
    const auto fromField = raw.find("fromEndpointId");
    const auto fromGenerationField = raw.find("fromConnectionGeneration");
    if (sequenceField == raw.end() ||
        !jsonU32(*sequenceField, true, out.sequence)) {
        return false;
    }
    if (toField == raw.end() || !jsonStringEquals(*toField, self)) return false;
    if (toGenerationField == raw.end() ||
        !jsonNumberEquals(*toGenerationField, generation)) {
        return false;
    }
    if (fromField == raw.end() ||
        !jsonEndpointNotSelf(*fromField, self, out.fromEndpointId)) {
        return false;
    }
    if (fromGenerationField == raw.end() ||
        !jsonU32(*fromGenerationField, true, out.fromConnectionGeneration)) {
        return false;
    }
    out.toEndpointId = self;
    out.toConnectionGeneration = generation;

    if (type == "peer_hello" &&
        exactKeys(raw, {"protocolVersion", "type", "sequence", "toEndpointId",
                        "toConnectionGeneration", "fromEndpointId",
                        "fromConnectionGeneration", "publicKey"}) &&
        raw["publicKey"].is_string()) {
        out.publicKey = raw["publicKey"].get<std::string>();
        if (!publicKeyString(out.publicKey)) return false;
        out.type = MdkrMatchSignalEventType::PeerHello;
        return true;
    }
    if ((type == "webrtc_offer" || type == "webrtc_answer") &&
        exactKeys(raw, {"protocolVersion", "type", "sequence", "toEndpointId",
                        "toConnectionGeneration", "fromEndpointId",
                        "fromConnectionGeneration", "sdp"}) &&
        jsonText(raw["sdp"], kSdpBytes, false, out.sdp)) {
        out.type = type == "webrtc_offer" ? MdkrMatchSignalEventType::WebrtcOffer
                                          : MdkrMatchSignalEventType::WebrtcAnswer;
        return true;
    }
    if (type == "webrtc_ice" &&
        exactKeys(raw, {"protocolVersion", "type", "sequence", "toEndpointId",
                        "toConnectionGeneration", "fromEndpointId",
                        "fromConnectionGeneration", "candidate", "sdpMid",
                        "sdpMLineIndex", "usernameFragment"}) &&
        jsonText(raw["candidate"], kIceBytes, false, out.candidate)) {
        const Json &sdpMid = raw["sdpMid"];
        if (sdpMid.is_null()) {
            out.hasSdpMid = false;
        } else if (jsonText(sdpMid, kMetaBytes, true, out.sdpMid)) {
            out.hasSdpMid = true;
        } else {
            return false;
        }
        const Json &fragment = raw["usernameFragment"];
        if (fragment.is_null()) {
            out.hasUsernameFragment = false;
        } else if (jsonText(fragment, kMetaBytes, true, out.usernameFragment)) {
            out.hasUsernameFragment = true;
        } else {
            return false;
        }
        const Json &line = raw["sdpMLineIndex"];
        if (line.is_null()) {
            out.hasSdpMLineIndex = false;
        } else if (jsonU32(line, false, out.sdpMLineIndex) &&
                   out.sdpMLineIndex <= 255u) {
            out.hasSdpMLineIndex = true;
        } else {
            return false;
        }
        out.type = MdkrMatchSignalEventType::WebrtcIce;
        return true;
    }
    if (type == "peer_end" &&
        exactKeys(raw, {"protocolVersion", "type", "sequence", "toEndpointId",
                        "toConnectionGeneration", "fromEndpointId",
                        "fromConnectionGeneration", "reason"}) &&
        (jsonStringEquals(raw["reason"], "restart") ||
         jsonStringEquals(raw["reason"], "close"))) {
        out.reason = raw["reason"].get<std::string>();
        out.type = MdkrMatchSignalEventType::PeerEnd;
        return true;
    }
    return false;
}

/* ---- clientMessage (the JS function, over the typed outbound struct) ----- */

/* The typed mirror of the JS exact-key rule: fields that do not belong to
 * the message type must be unset, or the whole message is invalid. */
bool iceMetaUnset(const MdkrMatchSignalOutbound &m) {
    return !m.hasSdpMid && m.sdpMid.empty() && !m.hasSdpMLineIndex &&
           m.sdpMLineIndex == 0u && !m.hasUsernameFragment &&
           m.usernameFragment.empty();
}

bool buildClientMessage(const MdkrMatchSignalOutbound &m,
                        const std::string &self, uint64_t sequence,
                        Json &out) {
    if (!endpointString(m.toEndpointId) || m.toEndpointId == self ||
        m.toConnectionGeneration == 0u) {
        return false;
    }
    out = Json{{"protocolVersion", 1u},
               {"type", m.type},
               {"sequence", sequence},
               {"toEndpointId", m.toEndpointId},
               {"toConnectionGeneration", m.toConnectionGeneration}};
    if (m.type == "peer_hello") {
        if (!publicKeyString(m.publicKey) || !m.sdp.empty() ||
            !m.candidate.empty() || !m.reason.empty() || !iceMetaUnset(m)) {
            return false;
        }
        out["publicKey"] = m.publicKey;
        return true;
    }
    if (m.type == "webrtc_offer" || m.type == "webrtc_answer") {
        if (!textString(m.sdp, kSdpBytes, false) || !m.publicKey.empty() ||
            !m.candidate.empty() || !m.reason.empty() || !iceMetaUnset(m)) {
            return false;
        }
        out["sdp"] = m.sdp;
        return true;
    }
    if (m.type == "webrtc_ice") {
        if (!textString(m.candidate, kIceBytes, false) ||
            !m.publicKey.empty() || !m.sdp.empty() || !m.reason.empty()) {
            return false;
        }
        if (m.hasSdpMid) {
            if (!textString(m.sdpMid, kMetaBytes, true)) return false;
            out["sdpMid"] = m.sdpMid;
        } else {
            if (!m.sdpMid.empty()) return false;
            out["sdpMid"] = nullptr;
        }
        if (m.hasSdpMLineIndex) {
            if (m.sdpMLineIndex > 255u) return false;
            out["sdpMLineIndex"] = m.sdpMLineIndex;
        } else {
            if (m.sdpMLineIndex != 0u) return false;
            out["sdpMLineIndex"] = nullptr;
        }
        if (m.hasUsernameFragment) {
            if (!textString(m.usernameFragment, kMetaBytes, true)) return false;
            out["usernameFragment"] = m.usernameFragment;
        } else {
            if (!m.usernameFragment.empty()) return false;
            out["usernameFragment"] = nullptr;
        }
        out["candidate"] = m.candidate;
        return true;
    }
    if (m.type == "peer_end") {
        if ((m.reason != "restart" && m.reason != "close") ||
            !m.publicKey.empty() || !m.sdp.empty() || !m.candidate.empty() ||
            !iceMetaUnset(m)) {
            return false;
        }
        out["reason"] = m.reason;
        return true;
    }
    return false;
}

/* ---- Origin parsing ------------------------------------------------------ */

struct ParsedOrigin {
    bool tls = false;
    std::string host;
    uint16_t port = 0u;
    std::string hostHeader;
};

/* scheme://host[:port][/] and nothing else. https/wss speak TLS; http/ws
 * are the loopback test lane behind the Party transport's exact token gate
 * (native_party_host.h) -- the credential never rides plaintext
 * off-machine. */
bool parseOrigin(const std::string &origin, ParsedOrigin &out) {
    std::string rest;
    std::string scheme;
    for (const char *candidate : {"https://", "wss://", "http://", "ws://"}) {
        const size_t length = std::strlen(candidate);
        if (origin.compare(0u, length, candidate) == 0) {
            scheme.assign(candidate, length - 3u); /* strip "://" */
            rest = origin.substr(length);
            break;
        }
    }
    if (scheme.empty()) return false;
    out.tls = scheme == "https" || scheme == "wss";
    if (!out.tls) {
        /* Reuse the house loopback gate verbatim by normalizing the scheme
         * it expects. */
        const std::string asHttp = "http://" + rest;
        if (!mdkr_party_loopback_test_url_allowed(asHttp)) return false;
    }
    if (!rest.empty() && rest.back() == '/') rest.pop_back();
    if (rest.empty() || rest.find('/') != std::string::npos) return false;
    const size_t colon = rest.find(':');
    std::string host = colon == std::string::npos ? rest : rest.substr(0u, colon);
    if (host.empty()) return false;
    for (const char byte : host) {
        const bool ok = (byte >= 'a' && byte <= 'z') ||
                        (byte >= 'A' && byte <= 'Z') ||
                        (byte >= '0' && byte <= '9') || byte == '.' ||
                        byte == '-';
        if (!ok) return false;
    }
    uint32_t port = out.tls ? 443u : 80u;
    if (colon != std::string::npos) {
        const std::string digits = rest.substr(colon + 1u);
        if (digits.empty() || digits.size() > 5u) return false;
        port = 0u;
        for (const char byte : digits) {
            if (byte < '0' || byte > '9') return false;
            port = port * 10u + static_cast<uint32_t>(byte - '0');
        }
        if (port == 0u || port > 65535u) return false;
    }
    out.host = host;
    out.port = static_cast<uint16_t>(port);
    out.hostHeader = host;
    if ((out.tls && out.port != 443u) || (!out.tls && out.port != 80u)) {
        out.hostHeader += ":" + std::to_string(out.port);
    }
    return true;
}

/* ---- Low-level socket helpers -------------------------------------------- */

#ifdef _WIN32
using NativeSocket = SOCKET;
constexpr NativeSocket kBadNativeSocket = INVALID_SOCKET;
#else
using NativeSocket = int;
constexpr NativeSocket kBadNativeSocket = -1;
#endif

void closeNativeSocket(NativeSocket fd) {
    if (fd == kBadNativeSocket) return;
#ifdef _WIN32
    ::closesocket(fd);
#else
    ::close(fd);
#endif
}

bool setNonBlocking(NativeSocket fd, bool nonBlocking) {
#ifdef _WIN32
    u_long mode = nonBlocking ? 1u : 0u;
    return ::ioctlsocket(fd, FIONBIO, &mode) == 0;
#else
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) return false;
    const int updated = nonBlocking ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    return ::fcntl(fd, F_SETFL, updated) == 0;
#endif
}

/* ---- Resolution (seamed for tests; see match_signal_client.h) ------------ */

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

/* getaddrinfo -> flat copies, so the caller owns plain values with no
 * addrinfo lifetime to thread through the connect loop. */
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

/* Resolve `host` into flat address copies, bounded by `deadlineMs` and by
 * `stopping` even though getaddrinfo itself is uninterruptible: the actual
 * resolve runs on a detached helper thread and this waiter polls it in
 * kPollSliceMs slices. On a give-up (deadline or close()) the helper is
 * ABANDONED, never joined -- it frees its own result when it eventually
 * returns -- so a DNS outage can no longer freeze close()/join() on the
 * launcher (review N6c/B5). Chosen over pre-resolve-before-thread-start
 * because the socket thread is also created per connect() and the launcher
 * thread would then take the identical getaddrinfo hit synchronously; this
 * bounds EVERY caller with one mechanism. The honored test stall models the
 * outage. Returns false with *timedOut clear on a plain resolution failure,
 * *timedOut set when the deadline expired first. */
struct ResolveTask {
    std::mutex mutex;
    std::condition_variable cv;
    bool done = false;
    bool abandoned = false;
    int rc = -1;
    struct addrinfo *results = nullptr;
};
/* Abandoned helpers accumulate only until their getaddrinfo returns (the
 * resolver timeout, worst case ~30 s), and new ones are minted only by
 * connect attempts, which the reconnect ladders bound (<= 6 per outage per
 * socket) -- so the in-principle-unbounded detached threads are ladder-
 * bounded in practice and each self-frees its result. */

bool resolveAddresses(const std::string &host, uint16_t port,
                      uint64_t deadlineMs, const std::atomic<bool> &stopping,
                      std::vector<ResolvedAddress> &out, bool *timedOut) {
    out.clear();
    *timedOut = false;
    (void)prependTestAddress(port, out);
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
        const int rc = ::getaddrinfo(host.c_str(),
                                     std::to_string(port).c_str(), &hints,
                                     &results);
        std::lock_guard<std::mutex> lock(task->mutex);
        if (task->abandoned) {
            /* Nobody is waiting anymore: this thread owns the cleanup. */
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
            if (stopping || steadyNowMs() >= deadlineMs) {
                task->abandoned = true;
                if (!stopping) *timedOut = true;
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
        return !out.empty(); /* the test prepend still supplies an address */
    }
    appendAddrinfo(results, out);
    ::freeaddrinfo(results);
    return !out.empty();
}

/* Deadline- and stop-aware TCP connect. Returns kBadNativeSocket on any
 * failure; *timedOut distinguishes the deadline from a refusal. */
NativeSocket connectTcp(const std::string &host, uint16_t port,
                        uint64_t deadlineMs, const std::atomic<bool> &stopping,
                        bool *timedOut) {
    *timedOut = false;
    std::vector<ResolvedAddress> addresses;
    bool resolveTimedOut = false;
    if (!resolveAddresses(host, port, deadlineMs, stopping, addresses,
                          &resolveTimedOut) ||
        addresses.empty()) {
        *timedOut = resolveTimedOut;
        return kBadNativeSocket;
    }
    NativeSocket fd = kBadNativeSocket;
    bool anyAddressTimedOut = false;
    for (size_t index = 0u; index < addresses.size(); index++) {
        /* N3 per-address budget: min(cap, remaining/left). A blackholed
         * first address costs one slice, never the whole deadline; the
         * overall deadline still bounds the whole loop. DELIBERATE edge: a
         * single-address host is also capped at 3.5 s even when the caller's
         * deadline is longer -- a healthy TCP handshake completes orders of
         * magnitude faster, so past the cap the address is a blackhole and
         * the faster typed timeout beats waiting out the full budget. */
        const uint64_t nowAtEntry = steadyNowMs();
        if (nowAtEntry >= deadlineMs) {
            *timedOut = true;
            break;
        }
        const uint64_t remaining = deadlineMs - nowAtEntry;
        uint64_t slice = remaining / (addresses.size() - index);
        if (slice == 0u) slice = remaining;
        if (slice > kPerAddressConnectCapMs) slice = kPerAddressConnectCapMs;
        const uint64_t addressDeadlineMs = nowAtEntry + slice;
        const ResolvedAddress &entry = addresses[index];
        fd = ::socket(entry.family, entry.socktype, entry.protocol);
        if (fd == kBadNativeSocket) continue;
#ifdef SO_NOSIGPIPE
        /* A write aborted by close()'s shutdown must surface as EPIPE, not
         * kill the process (BSD/macOS; Linux uses MSG_NOSIGNAL per send). */
        {
            int one = 1;
            ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE,
                         reinterpret_cast<const char *>(&one), sizeof(one));
        }
#endif
        if (!setNonBlocking(fd, true)) {
            closeNativeSocket(fd);
            fd = kBadNativeSocket;
            continue;
        }
        const int connected = ::connect(
            fd, reinterpret_cast<const struct sockaddr *>(&entry.storage),
            entry.length);
        bool pending = false;
        if (connected != 0) {
#ifdef _WIN32
            pending = WSAGetLastError() == WSAEWOULDBLOCK;
#else
            pending = errno == EINPROGRESS;
#endif
            if (!pending) {
                closeNativeSocket(fd);
                fd = kBadNativeSocket;
                continue;
            }
        }
        bool established = !pending;
        while (pending && !stopping) {
            if (steadyNowMs() >= addressDeadlineMs) {
                anyAddressTimedOut = true;
                break;
            }
            /* poll() on POSIX: select()'s fd_set is undefined behavior for
             * descriptor VALUES >= FD_SETSIZE, which a long-lived launcher
             * can reach; Windows select() has no value limit. */
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
            socklen_t errorLength = sizeof(soError);
            if (::getsockopt(fd, SOL_SOCKET, SO_ERROR,
                             reinterpret_cast<char *>(&soError),
                             &errorLength) == 0 &&
                soError == 0) {
                established = true;
            }
            break;
        }
        /* The fd stays non-blocking for its whole life: reads go through
         * timeout-sliced waits and writes through the interruptible
         * deadline-bound writeAll, so nothing on the socket thread can
         * block past a poll slice. */
        if (established) {
            /* Disable Nagle immediately post-connect: every signaling frame
             * is small (hellos ~200 B, ICE ~300 B) and Nagle + delayed ACK
             * adds up to ~40-200 ms per hop during the join/ICE-trickle
             * burst. Set-failure is non-fatal -- coalescing merely stays on
             * (the SO_NOSIGPIPE discipline above). */
            int noDelay = 1;
            (void)::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY,
                               reinterpret_cast<const char *>(&noDelay),
                               sizeof(noDelay));
            break;
        }
        closeNativeSocket(fd);
        fd = kBadNativeSocket;
        if (stopping) break;
    }
    /* Exhausted with at least one address stuck at its slice: report the
     * timeout class (the pre-N3 semantics for a blackhole), not a refusal. */
    if (fd == kBadNativeSocket && anyAddressTimedOut) *timedOut = true;
    return fd;
}

/* Wait up to `ms` for writability. poll() on POSIX -- select() is
 * undefined behavior once a long-lived launcher's descriptor numbers reach
 * FD_SETSIZE; Windows select() has no such value limit. */
void waitWritable(int fd, uint32_t ms) {
#ifdef _WIN32
    fd_set writable;
    FD_ZERO(&writable);
    FD_SET(static_cast<NativeSocket>(fd), &writable);
    struct timeval slice;
    slice.tv_sec = 0;
    slice.tv_usec = static_cast<long>(ms) * 1000;
    (void)::select(0, nullptr, &writable, nullptr, &slice);
#else
    struct pollfd item;
    item.fd = fd;
    item.events = POLLOUT;
    item.revents = 0;
    (void)::poll(&item, 1, static_cast<int>(ms));
#endif
}

/* mbedtls_net_send with SIGPIPE suppressed where the socket option cannot
 * do it (MSG_NOSIGNAL). Used for plain writes and as the TLS bio send
 * callback, so both paths share one signal-safe choke point. */
int netSendNoSignal(void *context, const unsigned char *data, size_t length) {
    mbedtls_net_context *net = static_cast<mbedtls_net_context *>(context);
    int flags = 0;
#ifdef MSG_NOSIGNAL
    flags = MSG_NOSIGNAL;
#endif
#ifdef _WIN32
    const int wrote = static_cast<int>(
        ::send(static_cast<NativeSocket>(net->fd),
               reinterpret_cast<const char *>(data),
               static_cast<int>(length), flags));
    if (wrote < 0) {
        if (WSAGetLastError() == WSAEWOULDBLOCK) {
            return MBEDTLS_ERR_SSL_WANT_WRITE;
        }
        return MBEDTLS_ERR_NET_SEND_FAILED;
    }
#else
    const int wrote = static_cast<int>(
        ::send(net->fd, data, length, flags));
    if (wrote < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            return MBEDTLS_ERR_SSL_WANT_WRITE;
        }
        if (errno == EPIPE || errno == ECONNRESET) {
            return MBEDTLS_ERR_NET_CONN_RESET;
        }
        return MBEDTLS_ERR_NET_SEND_FAILED;
    }
#endif
    return wrote;
}

std::string standardBase64(const unsigned char *bytes, size_t length) {
    unsigned char encoded[128];
    size_t written = 0u;
    if (mbedtls_base64_encode(encoded, sizeof(encoded), &written, bytes,
                              length) != 0) {
        return std::string{};
    }
    return std::string(reinterpret_cast<const char *>(encoded), written);
}

std::string websocketAcceptFor(const std::string &key) {
    static const char kGuid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    const std::string joined = key + kGuid;
    unsigned char digest[20];
    if (mbedtls_sha1(reinterpret_cast<const unsigned char *>(joined.data()),
                     joined.size(), digest) != 0) {
        return std::string{};
    }
    return standardBase64(digest, sizeof(digest));
}

std::string loweredCopy(std::string value) {
    for (char &byte : value) {
        byte = static_cast<char>(std::tolower(static_cast<unsigned char>(byte)));
    }
    return value;
}

/* ---- Server-frame extraction (one step over the carried buffer) -----------
 *
 * The byte-level half of the socket thread's frame loop, factored out so the
 * N7 fuzz harness drives the EXACT shipped parser. Semantics are the frame
 * loop's, unchanged: RSV bits, a masked server frame, or an oversize/
 * fragmented control frame is a protocol Violation (transport-lost class);
 * a data frame whose DECLARED length would breach the kSignalBytes signaling
 * bound -- alone or on top of `assembledBytes` of pending continuation
 * payload -- is Oversize (invalid-message class), refused before any payload
 * is buffered. On Frame the bytes are consumed from `carried`. */
enum class ServerFrameStatus { NeedMore, Frame, Violation, Oversize };
struct ServerFrame {
    bool fin = false;
    uint8_t opcode = 0u;
    std::string payload;
};

ServerFrameStatus extractServerFrame(std::string &carried,
                                     size_t assembledBytes,
                                     ServerFrame &out) {
    if (carried.size() < 2u) return ServerFrameStatus::NeedMore;
    const uint8_t byte0 = static_cast<uint8_t>(carried[0]);
    const uint8_t byte1 = static_cast<uint8_t>(carried[1]);
    if ((byte0 & 0x70u) != 0u || (byte1 & 0x80u) != 0u) {
        /* RSV bits or a masked server frame: protocol violation. */
        return ServerFrameStatus::Violation;
    }
    const bool fin = (byte0 & 0x80u) != 0u;
    const uint8_t opcode = byte0 & 0x0fu;
    size_t headerSize = 2u;
    uint64_t length = byte1 & 0x7fu;
    if (length == 126u) headerSize = 4u;
    else if (length == 127u) headerSize = 10u;
    if (carried.size() < headerSize) return ServerFrameStatus::NeedMore;
    if (headerSize == 4u) {
        length = (static_cast<uint64_t>(
                      static_cast<uint8_t>(carried[2])) << 8u) |
                 static_cast<uint64_t>(static_cast<uint8_t>(carried[3]));
    } else if (headerSize == 10u) {
        length = 0u;
        for (unsigned index = 0u; index < 8u; index++) {
            length = (length << 8u) |
                     static_cast<uint8_t>(carried[2u + index]);
        }
    }
    const bool isControl = (opcode & 0x8u) != 0u;
    if (isControl && (length > 125u || !fin)) {
        return ServerFrameStatus::Violation;
    }
    if (!isControl) {
        /* The signaling frame bound, enforced from the declared length
         * BEFORE any payload is buffered or parsed. */
        const uint64_t pendingBytes = static_cast<uint64_t>(assembledBytes);
        if (length > kSignalBytes || pendingBytes + length > kSignalBytes) {
            return ServerFrameStatus::Oversize;
        }
    }
    if (carried.size() < headerSize + length) {
        return ServerFrameStatus::NeedMore;
    }
    out.fin = fin;
    out.opcode = opcode;
    out.payload = carried.substr(headerSize, static_cast<size_t>(length));
    carried.erase(0u, headerSize + static_cast<size_t>(length));
    return ServerFrameStatus::Frame;
}

/* First matching header (lowercase name), trimmed. */
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
            while (!value.empty() &&
                   (value.front() == ' ' || value.front() == '\t')) {
                value.erase(0u, 1u);
            }
            while (!value.empty() &&
                   (value.back() == ' ' || value.back() == '\t')) {
                value.pop_back();
            }
            return true;
        }
        start = end;
    }
    return false;
}

} // namespace

/* ---- Test seams (declarations in match_signal_client.h) ------------------- */

void mdkr_match_signal_client_prepend_address_for_test(const char *ip,
                                                       uint16_t port) {
    std::lock_guard<std::mutex> lock(resolverSeamMutex());
    g_prependAddressForTest = ip != nullptr ? ip : "";
    g_prependPortForTest = port;
}

void mdkr_match_signal_client_stall_resolver_for_test(unsigned ms) {
    g_resolverStallMsForTest.store(ms);
}

/* ---- State ---------------------------------------------------------------- */

struct MdkrMatchSignalClient::State {
    /* Immutable after create(). */
    ParsedOrigin origin;
    std::string path;
    std::string endpointId;
    unsigned timeoutMs = 10000u;
    unsigned livenessIdleMs = kLivenessIdleDefaultMs;
    unsigned livenessTimeoutMs = kLivenessTimeoutDefaultMs;

    mutable std::mutex mutex;
    MdkrMatchSignalPhase phase = MdkrMatchSignalPhase::Idle;
    std::string credential; /* wiped by close() */
    uint32_t generation = 0u;
    uint64_t nextSequence = 1u;
    std::map<std::string, uint32_t> peerGenerations;
    /* High-water marks are never deleted (a departed peer's mark is what
     * refuses a rollback), so the map is append-only and carries the
     * explicit 64-entry bound that fails CLOSED. */
    std::map<std::string, uint32_t> peerGenerationHighWater;
    std::map<std::string, uint32_t> receivedSequences;
    std::map<uint32_t, MdkrMatchSignalPeerRef> sentTargets;

    std::deque<MdkrMatchSignalEvent> events;
    uint64_t droppedEvents = 0u;

    std::deque<std::string> outbound; /* serialized payloads, FIFO */

    std::thread thread;
    std::atomic<bool> stopping{false};
    std::atomic<bool> closeRequested{false};
    bool wantCloseFrame1000 = false; /* set by close() while upgraded */
    bool socketOpen = false;         /* 101 completed (socket thread) */

    /* ---- Event queue (bounded; Failure events are never evicted) ---- */
    void enqueueLocked(MdkrMatchSignalEvent &&event) {
        if (events.size() >= kMaxQueuedEvents) {
            for (auto iterator = events.begin(); iterator != events.end();
                 ++iterator) {
                if (iterator->type != MdkrMatchSignalEventType::Failure) {
                    events.erase(iterator);
                    droppedEvents++;
                    break;
                }
            }
            if (events.size() >= kMaxQueuedEvents) return; /* all terminal */
        }
        events.push_back(std::move(event));
    }

    /* Latch the terminal failure (JS fail()): once failed or closed,
     * nothing further is recorded. */
    bool latchFailureLocked(const char *code) {
        if (phase == MdkrMatchSignalPhase::Failed ||
            phase == MdkrMatchSignalPhase::Closed) {
            return false;
        }
        phase = MdkrMatchSignalPhase::Failed;
        MdkrMatchSignalEvent event;
        event.type = MdkrMatchSignalEventType::Failure;
        event.failureCode = code;
        enqueueLocked(std::move(event));
        return true;
    }

    bool rememberPeerGenerationLocked(const std::string &id, uint32_t value) {
        if (peerGenerationHighWater.find(id) == peerGenerationHighWater.end() &&
            peerGenerationHighWater.size() >= kMaxTrackedPeerGenerations) {
            return false;
        }
        peerGenerationHighWater[id] = value;
        return true;
    }

    /* The JS onmessage handler after JSON.parse. Returns nullptr on
     * success, otherwise the terminal failure code. */
    const char *handleServerText(const std::string &text) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (phase != MdkrMatchSignalPhase::Connecting &&
                phase != MdkrMatchSignalPhase::Open) {
                return nullptr;
            }
        }
        if (text.size() > kSignalBytes) return kMdkrMatchSignalInvalidMessage;
        const Json raw = Json::parse(text, nullptr, false);
        if (raw.is_discarded()) return kMdkrMatchSignalInvalidMessage;

        std::lock_guard<std::mutex> lock(mutex);
        if (phase != MdkrMatchSignalPhase::Connecting &&
            phase != MdkrMatchSignalPhase::Open) {
            return nullptr;
        }
        MdkrMatchSignalEvent event;
        if (!parseServerMessage(raw, endpointId, generation, event)) {
            return kMdkrMatchSignalInvalidMessage;
        }
        switch (event.type) {
        case MdkrMatchSignalEventType::Welcome: {
            for (const MdkrMatchSignalPeerRef &peer : event.peers) {
                peerGenerations[peer.endpointId] = peer.connectionGeneration;
                if (!rememberPeerGenerationLocked(peer.endpointId,
                                                  peer.connectionGeneration)) {
                    return kMdkrMatchSignalPeerGenerationOverflow;
                }
            }
            generation = event.connectionGeneration;
            phase = MdkrMatchSignalPhase::Open;
            break;
        }
        case MdkrMatchSignalEventType::PeerPresence: {
            const auto current = peerGenerations.find(event.endpointId);
            const auto mark = peerGenerationHighWater.find(event.endpointId);
            const uint32_t highWater =
                mark == peerGenerationHighWater.end() ? 0u : mark->second;
            if ((event.present && event.connectionGeneration <= highWater) ||
                (!event.present &&
                 (current == peerGenerations.end() ||
                  current->second != event.connectionGeneration))) {
                return kMdkrMatchSignalInvalidMessage;
            }
            if (event.present) {
                if (!rememberPeerGenerationLocked(event.endpointId,
                                                  event.connectionGeneration)) {
                    return kMdkrMatchSignalPeerGenerationOverflow;
                }
                peerGenerations[event.endpointId] = event.connectionGeneration;
                receivedSequences.erase(event.endpointId);
            } else {
                peerGenerations.erase(event.endpointId);
                receivedSequences.erase(event.endpointId);
            }
            break;
        }
        case MdkrMatchSignalEventType::SignalError: {
            const auto target = sentTargets.find(event.sequence);
            if (target == sentTargets.end() ||
                target->second.endpointId != event.toEndpointId ||
                target->second.connectionGeneration !=
                    event.toConnectionGeneration) {
                return kMdkrMatchSignalInvalidMessage;
            }
            sentTargets.erase(target);
            break;
        }
        default: { /* Directed message. */
            const auto sender = peerGenerations.find(event.fromEndpointId);
            const auto prior = receivedSequences.find(event.fromEndpointId);
            const uint32_t floor =
                prior == receivedSequences.end() ? 0u : prior->second;
            if (sender == peerGenerations.end() ||
                sender->second != event.fromConnectionGeneration ||
                event.sequence <= floor) {
                return kMdkrMatchSignalInvalidMessage;
            }
            receivedSequences[event.fromEndpointId] = event.sequence;
            break;
        }
        }
        enqueueLocked(std::move(event));
        return nullptr;
    }

    bool isTerminalOrClosing() const {
        std::lock_guard<std::mutex> lock(mutex);
        return phase == MdkrMatchSignalPhase::Failed ||
               phase == MdkrMatchSignalPhase::Closed;
    }

    bool welcomePending() const {
        std::lock_guard<std::mutex> lock(mutex);
        return phase == MdkrMatchSignalPhase::Connecting;
    }

    /* ---- Socket thread ---- */
    void run();
};

namespace {

/* Everything the socket thread owns for one connection attempt. */
struct Transport {
    bool tls = false;
    mbedtls_net_context net;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config config;
    mbedtls_x509_crt ca;
    bool alive = false;
    /* close() sets this (closeRequested); a data write in progress aborts
     * at its next poll slice instead of blocking behind a stalled peer. */
    const std::atomic<bool> *interrupt = nullptr;

    Transport() {
        mbedtls_net_init(&net);
        mbedtls_entropy_init(&entropy);
        mbedtls_ctr_drbg_init(&drbg);
        mbedtls_ssl_init(&ssl);
        mbedtls_ssl_config_init(&config);
        mbedtls_x509_crt_init(&ca);
    }

    ~Transport() {
        teardown();
        mbedtls_x509_crt_free(&ca);
        mbedtls_ssl_config_free(&config);
        mbedtls_ssl_free(&ssl);
        mbedtls_ctr_drbg_free(&drbg);
        mbedtls_entropy_free(&entropy);
        mbedtls_net_free(&net);
    }

    Transport(const Transport &) = delete;
    Transport &operator=(const Transport &) = delete;

    bool seedRandom() {
        static const char kPersonalization[] = "mdkr-match-signal-v1";
        return mbedtls_ctr_drbg_seed(
                   &drbg, mbedtls_entropy_func, &entropy,
                   reinterpret_cast<const unsigned char *>(kPersonalization),
                   sizeof(kPersonalization) - 1u) == 0;
    }

    bool startTls(const std::string &hostname, uint64_t deadlineMs,
                  const std::atomic<bool> &stopping, bool *timedOut) {
        *timedOut = false;
        /* The generated header ends with an explicit 0x00 the PEM parser
         * requires; Length excludes it, parse length includes it. */
        if (mbedtls_x509_crt_parse(&ca, kMdkrMozillaCaBundle,
                                   static_cast<size_t>(
                                       kMdkrMozillaCaBundleLength) + 1u) != 0) {
            return false;
        }
        if (mbedtls_ssl_config_defaults(&config, MBEDTLS_SSL_IS_CLIENT,
                                        MBEDTLS_SSL_TRANSPORT_STREAM,
                                        MBEDTLS_SSL_PRESET_DEFAULT) != 0) {
            return false;
        }
        mbedtls_ssl_conf_authmode(&config, MBEDTLS_SSL_VERIFY_REQUIRED);
        mbedtls_ssl_conf_ca_chain(&config, &ca, nullptr);
        mbedtls_ssl_conf_rng(&config, mbedtls_ctr_drbg_random, &drbg);
        mbedtls_ssl_conf_read_timeout(&config, kPollSliceMs);
        if (mbedtls_ssl_setup(&ssl, &config) != 0 ||
            mbedtls_ssl_set_hostname(&ssl, hostname.c_str()) != 0) {
            return false;
        }
        mbedtls_ssl_set_bio(&ssl, &net, netSendNoSignal, nullptr,
                            mbedtls_net_recv_timeout);
        for (;;) {
            if (stopping) return false;
            if (steadyNowMs() >= deadlineMs) {
                *timedOut = true;
                return false;
            }
            const int ret = mbedtls_ssl_handshake(&ssl);
            if (ret == 0) return true;
            if (ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
                waitWritable(net.fd, kPollSliceMs);
            } else if (ret != MBEDTLS_ERR_SSL_WANT_READ &&
                       ret != MBEDTLS_ERR_SSL_TIMEOUT) {
                return false;
            }
        }
    }

    /* One slice: >0 bytes read, 0 nothing yet, -1 transport gone. */
    int readSlice(unsigned char *buffer, size_t capacity) {
        if (!alive) return -1;
        int ret;
        if (tls) {
            ret = mbedtls_ssl_read(&ssl, buffer, capacity);
            if (ret > 0) return ret;
            if (ret == MBEDTLS_ERR_SSL_WANT_READ ||
                ret == MBEDTLS_ERR_SSL_WANT_WRITE ||
                ret == MBEDTLS_ERR_SSL_TIMEOUT) {
                return 0;
            }
            alive = false;
            return -1;
        }
        ret = mbedtls_net_recv_timeout(&net, buffer, capacity, kPollSliceMs);
        if (ret > 0) return ret;
        if (ret == MBEDTLS_ERR_SSL_TIMEOUT ||
            ret == MBEDTLS_ERR_SSL_WANT_READ) {
            /* WANT_READ: select claimed readable but the non-blocking recv
             * raced to empty -- same as a timeout slice. */
            return 0;
        }
        alive = false;
        return -1;
    }

    /* Deadline-bound, interruptible write. Every stall waits one poll
     * slice at a time, honoring `interrupt` (unless the frame is a final
     * close frame, which must get its bounded best effort even while a
     * close is in progress) and the budget, so a peer with a zero receive
     * window can never wedge the socket thread -- and with it close(). */
    bool writeAll(const unsigned char *data, size_t length, uint32_t budgetMs,
                  bool honorInterrupt) {
        if (!alive) return false;
        const uint64_t deadline = steadyNowMs() + budgetMs;
        size_t sent = 0u;
        while (sent < length) {
            int ret;
            if (tls) {
                ret = mbedtls_ssl_write(&ssl, data + sent, length - sent);
                if (ret == MBEDTLS_ERR_SSL_WANT_READ ||
                    ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
                    ret = MBEDTLS_ERR_SSL_WANT_WRITE;
                }
            } else {
                ret = netSendNoSignal(&net, data + sent, length - sent);
            }
            if (ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
                if ((honorInterrupt && interrupt != nullptr && *interrupt) ||
                    steadyNowMs() >= deadline) {
                    return false; /* aborted mid-frame; the socket is done */
                }
                waitWritable(net.fd, kPollSliceMs);
                continue;
            }
            if (ret <= 0) {
                alive = false;
                return false;
            }
            sent += static_cast<size_t>(ret);
        }
        return true;
    }

    bool writeAll(const std::string &data, uint32_t budgetMs,
                  bool honorInterrupt = true) {
        return writeAll(reinterpret_cast<const unsigned char *>(data.data()),
                        data.size(), budgetMs, honorInterrupt);
    }

    void teardown() {
        if (net.fd >= 0) {
#ifdef _WIN32
            ::shutdown(static_cast<NativeSocket>(net.fd), SD_BOTH);
#else
            ::shutdown(net.fd, SHUT_RDWR);
#endif
        }
        mbedtls_net_free(&net); /* closes and re-inits */
        alive = false;
    }

    /* Masked client frame (RFC 6455 5.3). False when the DRBG cannot
     * produce mask bytes: predictable all-zero masking is a protocol
     * violation in spirit, so the frame is refused rather than sent. */
    bool clientFrame(uint8_t opcode, const std::string &payload,
                     std::string &out) {
        std::string frame;
        frame.push_back(static_cast<char>(0x80u | opcode));
        if (payload.size() < 126u) {
            frame.push_back(
                static_cast<char>(0x80u | static_cast<uint8_t>(payload.size())));
        } else if (payload.size() <= 0xffffu) {
            frame.push_back(static_cast<char>(0x80u | 126u));
            frame.push_back(static_cast<char>((payload.size() >> 8u) & 0xffu));
            frame.push_back(static_cast<char>(payload.size() & 0xffu));
        } else {
            frame.push_back(static_cast<char>(0x80u | 127u));
            for (int shift = 56; shift >= 0; shift -= 8) {
                frame.push_back(static_cast<char>(
                    (static_cast<uint64_t>(payload.size()) >> shift) & 0xffu));
            }
        }
        unsigned char mask[4] = {0u, 0u, 0u, 0u};
        if (mbedtls_ctr_drbg_random(&drbg, mask, sizeof(mask)) != 0) {
            return false;
        }
        for (const unsigned char byte : mask) {
            frame.push_back(static_cast<char>(byte));
        }
        for (size_t index = 0u; index < payload.size(); index++) {
            frame.push_back(static_cast<char>(
                static_cast<uint8_t>(payload[index]) ^ mask[index % 4u]));
        }
        out = std::move(frame);
        return true;
    }

    bool sendCloseFrame(uint16_t code, const std::string &reason) {
        std::string payload;
        payload.push_back(static_cast<char>((code >> 8u) & 0xffu));
        payload.push_back(static_cast<char>(code & 0xffu));
        payload += reason;
        std::string frame;
        if (!clientFrame(0x8u, payload, frame)) return false;
        /* Bounded best effort, deliberately NOT interruptible: this is the
         * one frame close() itself wants written, and a peer that has
         * stopped reading only costs the short budget. */
        return writeAll(frame, kCloseFrameBudgetMs,
                        /*honorInterrupt=*/false);
    }
};

} // namespace

void MdkrMatchSignalClient::State::run() {
    const uint64_t deadlineMs = steadyNowMs() + timeoutMs;
    Transport transport;
    transport.tls = origin.tls;

    /* Terminal exit for this attempt. JS fail() sends close 4003
     * "invalid_server_message" on every fail path with a live upgraded
     * socket; the browser's own handshake aborts (subprotocol absent or
     * unoffered, refused 101) never carry a close frame. */
    const auto terminate = [&](const char *code, bool sendFrame) {
        bool latched = false;
        {
            std::lock_guard<std::mutex> lock(mutex);
            latched = latchFailureLocked(code);
        }
        if (latched && sendFrame && transport.alive) {
            (void)transport.sendCloseFrame(4003u, "invalid_server_message");
        }
        transport.teardown();
    };

    /* Clean-close exit requested by close(). */
    const auto finishClose = [&]() {
        bool wantFrame = false;
        {
            std::lock_guard<std::mutex> lock(mutex);
            wantFrame = wantCloseFrame1000 && socketOpen;
        }
        if (wantFrame && transport.alive) {
            (void)transport.sendCloseFrame(1000u, "launcher_route_changed");
        }
        transport.teardown();
    };

    if (!transport.seedRandom()) {
        terminate(kMdkrMatchSignalTransportLost, false);
        return;
    }

    /* ---- TCP ---- */
    bool timedOut = false;
    const NativeSocket fd =
        connectTcp(origin.host, origin.port, deadlineMs, stopping, &timedOut);
    if (stopping || closeRequested) {
        closeNativeSocket(fd);
        finishClose();
        return;
    }
    if (fd == kBadNativeSocket) {
        terminate(timedOut ? kMdkrMatchSignalTimeout
                           : kMdkrMatchSignalTransportLost,
                  false);
        return;
    }
    transport.net.fd = static_cast<int>(fd);
    transport.alive = true;
    transport.interrupt = &closeRequested;

    /* ---- TLS ---- */
    if (transport.tls) {
        bool tlsTimedOut = false;
        if (!transport.startTls(origin.host, deadlineMs, stopping,
                                &tlsTimedOut)) {
            if (stopping || closeRequested) {
                finishClose();
                return;
            }
            terminate(tlsTimedOut ? kMdkrMatchSignalTimeout
                                  : kMdkrMatchSignalTransportLost,
                      false);
            return;
        }
    }

    /* ---- Upgrade ---- */
    unsigned char nonce[16];
    if (mbedtls_ctr_drbg_random(&transport.drbg, nonce, sizeof(nonce)) != 0) {
        terminate(kMdkrMatchSignalTransportLost, false);
        return;
    }
    const std::string key = standardBase64(nonce, sizeof(nonce));
    std::string credentialOffer;
    {
        std::lock_guard<std::mutex> lock(mutex);
        credentialOffer = "gb-match." + credential;
    }
    /* Native is originless: no Origin header, and the credential rides ONLY
     * in the subprotocol offer. A plain, benign User-Agent is sent as
     * belt-and-suspenders: Cloudflare's Browser Integrity Check bans known
     * scripting-tool signatures (python-urllib, curl, Go-http-client, ...) but
     * passes normal or empty UAs, so a product UA lets this hand-rolled RFC 6455
     * client reliably clear the edge. It carries no identity and is never
     * parsed by the MatchRoom worker. */
    std::string request = "GET " + path + " HTTP/1.1\r\n" +
                          "Host: " + origin.hostHeader + "\r\n" +
                          "User-Agent: GoldenBalloon/1.6.0\r\n" +
                          "Upgrade: websocket\r\n" +
                          "Connection: Upgrade\r\n" +
                          "Sec-WebSocket-Key: " + key + "\r\n" +
                          "Sec-WebSocket-Version: 13\r\n" +
                          "Sec-WebSocket-Protocol: " +
                          kMdkrMatchSignalSubprotocol + ", " +
                          credentialOffer + "\r\n\r\n";
    const bool requestSent = transport.writeAll(request, kDataWriteBudgetMs);
    mbedtls_platform_zeroize(&request[0], request.size());
    mbedtls_platform_zeroize(&credentialOffer[0], credentialOffer.size());
    if (!requestSent) {
        terminate(kMdkrMatchSignalTransportLost, false);
        return;
    }

    std::string head;
    unsigned char buffer[16384];
    while (head.find("\r\n\r\n") == std::string::npos) {
        if (stopping || closeRequested) {
            finishClose();
            return;
        }
        if (steadyNowMs() >= deadlineMs) {
            terminate(kMdkrMatchSignalTimeout, false);
            return;
        }
        if (head.size() > sizeof(buffer)) {
            terminate(kMdkrMatchSignalTransportLost, false);
            return;
        }
        const int got = transport.readSlice(buffer, sizeof(buffer));
        if (got < 0) {
            terminate(kMdkrMatchSignalTransportLost, false);
            return;
        }
        if (got > 0) head.append(reinterpret_cast<char *>(buffer),
                                 static_cast<size_t>(got));
    }
    const size_t headEnd = head.find("\r\n\r\n");
    std::string carried = head.substr(headEnd + 4u);
    head.resize(headEnd + 2u);

    if (head.compare(0u, 12u, "HTTP/1.1 101") != 0) {
        terminate(kMdkrMatchSignalTransportLost, false);
        return;
    }
    std::string headerField;
    if (!responseHeader(head, "upgrade", headerField) ||
        loweredCopy(headerField) != "websocket") {
        terminate(kMdkrMatchSignalTransportLost, false);
        return;
    }
    if (!responseHeader(head, "connection", headerField) ||
        loweredCopy(headerField).find("upgrade") == std::string::npos) {
        terminate(kMdkrMatchSignalTransportLost, false);
        return;
    }
    if (!responseHeader(head, "sec-websocket-accept", headerField) ||
        headerField != websocketAcceptFor(key)) {
        terminate(kMdkrMatchSignalTransportLost, false);
        return;
    }
    /* RFC 6455 4.1: a 101 naming an extension the client never requested
     * (this client requests none) MUST fail the connection -- an extension
     * changes the framing itself, so ignoring the header would mean
     * parsing frames under a contract we never agreed to. */
    if (responseHeader(head, "sec-websocket-extensions", headerField)) {
        terminate(kMdkrMatchSignalTransportLost, false);
        return;
    }
    std::string selected;
    const bool hasSelection =
        responseHeader(head, "sec-websocket-protocol", selected);
    if (selected != kMdkrMatchSignalSubprotocol) {
        std::string storedOffer;
        {
            std::lock_guard<std::mutex> lock(mutex);
            storedOffer = "gb-match." + credential;
        }
        const bool offeredButWrong =
            hasSelection && selected == storedOffer;
        mbedtls_platform_zeroize(&storedOffer[0], storedOffer.size());
        if (offeredButWrong) {
            /* The JS open-handler check: the socket opened, but with the
             * credential subprotocol selected instead of the public
             * version token. */
            {
                std::lock_guard<std::mutex> lock(mutex);
                socketOpen = true;
            }
            terminate(kMdkrMatchSignalInvalidSubprotocol, true);
        } else {
            /* Absent or never offered: the browser fails the WebSocket
             * connection itself -- the reference client only ever sees the
             * transport loss. */
            terminate(kMdkrMatchSignalTransportLost, false);
        }
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex);
        socketOpen = true;
    }

    /* ---- Frame loop ---- */
    std::string assembled;
    bool assembling = false;
    /* Client-originated liveness (W3 N6a): ANY inbound byte proves the
     * transport; a ping is only the probe that forces a silent one to
     * speak. Native-only -- the browser reference client cannot originate
     * pings -- and the worker echoes pongs per RFC 6455 5.5.2/5.5.3. */
    uint64_t lastInboundMs = steadyNowMs();
    bool livenessPingSent = false;
    uint32_t livenessPingCounter = 0u;
    for (;;) {
        if (stopping || closeRequested) {
            finishClose();
            return;
        }
        /* Welcome deadline (the JS connect timer, cleared by welcome). */
        if (welcomePending() && steadyNowMs() >= deadlineMs) {
            terminate(kMdkrMatchSignalTimeout, true);
            return;
        }
        /* Liveness ladder: idle -> one probe; idle + timeout -> the
         * transport is dead (a half-open socket looks exactly like this),
         * through the existing terminal path. */
        {
            const uint64_t quietMs = steadyNowMs() - lastInboundMs;
            if (livenessPingSent &&
                quietMs >= static_cast<uint64_t>(livenessIdleMs) +
                               livenessTimeoutMs) {
                terminate(kMdkrMatchSignalTransportLost, false);
                return;
            }
            if (!livenessPingSent && quietMs >= livenessIdleMs) {
                std::string ping;
                const std::string probe =
                    "gb-live-" + std::to_string(++livenessPingCounter);
                if (!transport.clientFrame(0x9u, probe, ping) ||
                    !transport.writeAll(ping, kDataWriteBudgetMs)) {
                    if (closeRequested) {
                        finishClose();
                    } else {
                        terminate(kMdkrMatchSignalTransportLost, false);
                    }
                    return;
                }
                livenessPingSent = true;
            }
        }
        /* Single-writer discipline: every wire write happens on this
         * thread, so TLS state is never touched concurrently. */
        {
            std::deque<std::string> pending;
            {
                std::lock_guard<std::mutex> lock(mutex);
                pending.swap(outbound);
            }
            for (const std::string &payload : pending) {
                std::string frame;
                if (!transport.clientFrame(0x1u, payload, frame) ||
                    !transport.writeAll(frame, kDataWriteBudgetMs)) {
                    /* A close() abort lands here mid-flush; everything
                     * else is a dead or hopelessly stalled transport. */
                    if (closeRequested) {
                        finishClose();
                    } else {
                        terminate(kMdkrMatchSignalTransportLost, false);
                    }
                    return;
                }
            }
        }

        /* Extract complete frames from `carried` (the shared byte-level
         * extractor; the N7 fuzzer drives the same function). */
        bool needMore = false;
        while (!needMore) {
            ServerFrame frame;
            const ServerFrameStatus frameStatus =
                extractServerFrame(carried, assembled.size(), frame);
            if (frameStatus == ServerFrameStatus::NeedMore) {
                needMore = true;
                break;
            }
            if (frameStatus == ServerFrameStatus::Violation) {
                /* RSV bits, a masked server frame, or a bad control frame:
                 * the browser kills such a connection at the protocol
                 * layer; the reference client sees transport loss. */
                terminate(kMdkrMatchSignalTransportLost, false);
                return;
            }
            if (frameStatus == ServerFrameStatus::Oversize) {
                terminate(kMdkrMatchSignalInvalidMessage, true);
                return;
            }
            const bool fin = frame.fin;
            const uint8_t opcode = frame.opcode;
            std::string payload = std::move(frame.payload);

            if (opcode == 0x9u) { /* ping -> pong */
                std::string pong;
                if (!transport.clientFrame(0xau, payload, pong) ||
                    !transport.writeAll(pong, kDataWriteBudgetMs)) {
                    if (closeRequested) {
                        finishClose();
                    } else {
                        terminate(kMdkrMatchSignalTransportLost, false);
                    }
                    return;
                }
                continue;
            }
            if (opcode == 0xau) continue; /* pong */
            if (opcode == 0x8u) {
                /* Server close: echo per RFC, then the JS close-event path
                 * (terminal transport loss, no 4003). */
                std::string echo;
                if (transport.clientFrame(0x8u, payload, echo)) {
                    (void)transport.writeAll(echo, kCloseFrameBudgetMs);
                }
                terminate(kMdkrMatchSignalTransportLost, false);
                return;
            }
            if (opcode == 0x2u) {
                /* Binary data can never be a valid signal message (the JS
                 * typeof check). */
                terminate(kMdkrMatchSignalInvalidMessage, true);
                return;
            }
            if (opcode == 0x1u) {
                if (assembling) {
                    terminate(kMdkrMatchSignalTransportLost, false);
                    return;
                }
                if (!fin) {
                    assembling = true;
                    assembled = std::move(payload);
                    continue;
                }
                const char *code = handleServerText(payload);
                if (code != nullptr) {
                    terminate(code, true);
                    return;
                }
                continue;
            }
            if (opcode == 0x0u) {
                if (!assembling) {
                    terminate(kMdkrMatchSignalTransportLost, false);
                    return;
                }
                assembled += payload;
                if (!fin) continue;
                assembling = false;
                std::string message;
                message.swap(assembled);
                const char *code = handleServerText(message);
                if (code != nullptr) {
                    terminate(code, true);
                    return;
                }
                continue;
            }
            /* Unknown opcode: protocol violation. */
            terminate(kMdkrMatchSignalTransportLost, false);
            return;
        }

        const int got = transport.readSlice(buffer, sizeof(buffer));
        if (got < 0) {
            /* EOF/reset: the JS socket close event -- terminal transport
             * loss, no close frame (the socket is already gone). */
            terminate(kMdkrMatchSignalTransportLost, false);
            return;
        }
        if (got > 0) {
            /* Any inbound byte proves the transport is alive. */
            lastInboundMs = steadyNowMs();
            livenessPingSent = false;
            carried.append(reinterpret_cast<char *>(buffer),
                           static_cast<size_t>(got));
        }
    }
}

/* ---- Public API ------------------------------------------------------------ */

MdkrMatchSignalClient::MdkrMatchSignalClient(std::shared_ptr<State> state)
    : state_(std::move(state)) {}

MdkrMatchSignalClient::~MdkrMatchSignalClient() { close(); }

std::unique_ptr<MdkrMatchSignalClient> MdkrMatchSignalClient::create(
    const MdkrMatchSignalClientOptions &options, std::string *errorMessage) {
    const auto refuse = [&](const char *message)
        -> std::unique_ptr<MdkrMatchSignalClient> {
        if (errorMessage != nullptr) *errorMessage = message;
        return nullptr;
    };
    if (options.roomId.size() != 22u || !base64UrlChars(options.roomId) ||
        !endpointString(options.endpointId) ||
        options.credential.size() != 43u ||
        !base64UrlChars(options.credential)) {
        return refuse(kMdkrMatchSignalInvalidIdentity);
    }
    ParsedOrigin origin;
    if (!parseOrigin(options.serviceOrigin, origin)) {
        return refuse(kMdkrMatchSignalCrossOriginRefused);
    }
#ifdef _WIN32
    WSADATA data;
    WSAStartup(MAKEWORD(2, 2), &data);
#endif
    auto state = std::make_shared<State>();
    state->origin = origin;
    state->path = "/api/match/" + options.roomId + "/signal";
    state->endpointId = options.endpointId;
    state->credential = options.credential;
    const unsigned requested = options.timeoutMs == 0u ? 10000u : options.timeoutMs;
    state->timeoutMs = requested < 2000u    ? 2000u
                       : requested > 30000u ? 30000u
                                            : requested;
    state->livenessIdleMs = options.livenessIdleMs == 0u
                                ? kLivenessIdleDefaultMs
                                : options.livenessIdleMs;
    state->livenessTimeoutMs = options.livenessTimeoutMs == 0u
                                   ? kLivenessTimeoutDefaultMs
                                   : options.livenessTimeoutMs;
    return std::unique_ptr<MdkrMatchSignalClient>(
        new MdkrMatchSignalClient(std::move(state)));
}

bool MdkrMatchSignalClient::connect(std::string *errorCode) {
    State &state = *state_;
    std::lock_guard<std::mutex> lock(state.mutex);
    if (state.phase == MdkrMatchSignalPhase::Open ||
        state.phase == MdkrMatchSignalPhase::Connecting) {
        return true;
    }
    if (state.phase != MdkrMatchSignalPhase::Idle) {
        if (errorCode != nullptr) *errorCode = kMdkrMatchSignalClientClosed;
        return false;
    }
    state.phase = MdkrMatchSignalPhase::Connecting;
    state.generation = 0u;
    state.nextSequence = 1u;
    state.peerGenerations.clear();
    state.peerGenerationHighWater.clear();
    state.receivedSequences.clear();
    state.sentTargets.clear();
    state.stopping = false;
    state.closeRequested = false;
    std::shared_ptr<State> shared = state_;
    state.thread = std::thread([shared]() { shared->run(); });
    return true;
}

MdkrMatchSignalSendResult MdkrMatchSignalClient::send(
    const MdkrMatchSignalOutbound &message) {
    State &state = *state_;
    MdkrMatchSignalSendResult result;
    std::lock_guard<std::mutex> lock(state.mutex);
    if (state.phase != MdkrMatchSignalPhase::Open ||
        state.nextSequence > kU32Max) {
        result.error = kMdkrMatchSignalNotConnected;
        return result;
    }
    Json wire;
    if (!buildClientMessage(message, state.endpointId, state.nextSequence,
                            wire)) {
        result.error = kMdkrMatchSignalInvalidClientMessage;
        return result;
    }
    const auto tracked = state.peerGenerations.find(message.toEndpointId);
    if (tracked == state.peerGenerations.end() ||
        tracked->second != message.toConnectionGeneration) {
        result.error = kMdkrMatchSignalPeerUnavailable;
        return result;
    }
    const uint32_t sequence = static_cast<uint32_t>(state.nextSequence);
    /* Belt and suspenders for the throws-as-values contract: every string
     * reaching `wire` is validated above (validUtf8 for text fields,
     * charset/literal checks for the rest), so dump() cannot throw -- but
     * if that invariant is ever broken, the refusal stays a value. */
    std::string payload;
    try {
        payload = wire.dump();
    } catch (...) {
        result.error = kMdkrMatchSignalInvalidClientMessage;
        return result;
    }
    state.outbound.push_back(std::move(payload));
    MdkrMatchSignalPeerRef target;
    target.endpointId = message.toEndpointId;
    target.connectionGeneration = message.toConnectionGeneration;
    state.sentTargets[sequence] = std::move(target);
    state.nextSequence++;
    result.ok = true;
    result.sequence = sequence;
    return result;
}

void MdkrMatchSignalClient::close() {
    State &state = *state_;
    std::thread toJoin;
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        if (state.phase == MdkrMatchSignalPhase::Closed) return;
        const bool wasConnecting =
            state.phase == MdkrMatchSignalPhase::Connecting;
        state.phase = MdkrMatchSignalPhase::Closed;
        state.generation = 0u;
        state.peerGenerations.clear();
        state.peerGenerationHighWater.clear();
        state.receivedSequences.clear();
        state.sentTargets.clear();
        state.outbound.clear();
        if (!state.credential.empty()) {
            mbedtls_platform_zeroize(&state.credential[0],
                                     state.credential.size());
            state.credential.clear();
        }
        if (wasConnecting) {
            /* The JS pending-connect rejection. */
            MdkrMatchSignalEvent event;
            event.type = MdkrMatchSignalEventType::Failure;
            event.failureCode = kMdkrMatchSignalClientClosed;
            state.enqueueLocked(std::move(event));
        }
        state.wantCloseFrame1000 = state.socketOpen;
        state.closeRequested = true;
        state.stopping = true;
        toJoin = std::move(state.thread);
    }
    /* The join is bounded by construction: every socket-thread wait is a
     * kPollSliceMs slice, and every write is interruptible (closeRequested
     * aborts a data write mid-flush; the clean-close frame itself carries
     * its own short deadline), so a peer with a stalled receive window
     * cannot hold this join. */
    if (toJoin.joinable()) toJoin.join();
}

MdkrMatchSignalSnapshot MdkrMatchSignalClient::snapshot() const {
    const State &state = *state_;
    std::lock_guard<std::mutex> lock(state.mutex);
    MdkrMatchSignalSnapshot out;
    out.phase = state.phase;
    out.connectionGeneration = state.generation;
    out.nextSequence = state.nextSequence;
    out.connected = state.phase == MdkrMatchSignalPhase::Open;
    out.droppedEvents = state.droppedEvents;
    return out;
}

void MdkrMatchSignalClient::drainEvents(
    std::vector<MdkrMatchSignalEvent> &out) {
    out.clear();
    State &state = *state_;
    std::lock_guard<std::mutex> lock(state.mutex);
    out.reserve(state.events.size());
    while (!state.events.empty()) {
        out.push_back(std::move(state.events.front()));
        state.events.pop_front();
    }
}

/* ---- Fuzz seam (declaration in match_signal_client.h) --------------------- */

void mdkr_match_signal_fuzz_wire(const uint8_t *data, size_t size) {
    /* Lane A: the byte-level frame extractor, accumulating exactly like the
     * socket thread (fragmented text assembles under the shared bound;
     * control frames pass through; any violation ends the stream). Complete
     * text messages feed lane B. */
    std::string carried(reinterpret_cast<const char *>(data), size);
    std::string assembled;
    bool assembling = false;
    std::vector<std::string> texts;
    for (;;) {
        ServerFrame frame;
        const ServerFrameStatus status =
            extractServerFrame(carried, assembled.size(), frame);
        if (status != ServerFrameStatus::Frame) break;
        if (frame.opcode == 0x1u) {
            if (assembling) break; /* run() terminates here */
            if (!frame.fin) {
                assembling = true;
                assembled = std::move(frame.payload);
                continue;
            }
            texts.push_back(std::move(frame.payload));
        } else if (frame.opcode == 0x0u) {
            if (!assembling) break;
            assembled += frame.payload;
            if (frame.fin) {
                assembling = false;
                texts.push_back(std::move(assembled));
                assembled.clear();
            }
        }
        /* control frames: extracted and dropped, like run()'s replies. */
    }

    /* Lane B: the server-message validation state machine, in both phases
     * (pre-welcome generation 0, and open at generation 7 with a tracked
     * peer + a recorded sent target so the presence/sequence/signal_error
     * rules are all reachable). */
    const auto drive = [&](bool open) {
        MdkrMatchSignalClient::State state;
        state.endpointId = "101";
        state.phase = open ? MdkrMatchSignalPhase::Open
                           : MdkrMatchSignalPhase::Connecting;
        if (open) {
            state.generation = 7u;
            state.peerGenerations["202"] = 5u;
            state.peerGenerationHighWater["202"] = 5u;
            state.receivedSequences["202"] = 3u;
            MdkrMatchSignalPeerRef target;
            target.endpointId = "202";
            target.connectionGeneration = 5u;
            state.sentTargets[1u] = std::move(target);
        }
        for (const std::string &text : texts) {
            (void)state.handleServerText(text);
        }
        /* The raw input as one message too, so a pure-JSON corpus needs no
         * WS framing to reach the validators. */
        (void)state.handleServerText(
            std::string(reinterpret_cast<const char *>(data), size));
    };
    drive(false);
    drive(true);
}
