/*
 * Scriptable loopback fake of the match-signaling WebSocket endpoint, for
 * tests/test_match_signal_client.cpp only. Follows the wire-level harness
 * patterns of tests/test_lan_party_server.cpp (raw sockets on 127.0.0.1,
 * bounded reads everywhere) with the roles reversed: here the TEST is the
 * RFC 6455 server so it can misbehave on demand -- select the wrong
 * subprotocol, send oversize/binary/malformed frames, or stay silent -- and
 * observe exactly what the native client puts on the wire (offered
 * subprotocols, request path, close codes and reasons).
 *
 * One connection at a time: every test case drives a single client socket.
 * All waits are bounded so a client defect fails the test instead of
 * hanging the suite.
 */
#ifndef MDKR_MATCH_SIGNAL_TEST_SERVER_H
#define MDKR_MATCH_SIGNAL_TEST_SERVER_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct MdkrMatchSignalTestServerState;

class MdkrMatchSignalTestServer {
public:
    enum class ProtocolMode {
        SelectV1,         /* select gb-match-signal-v1 (the correct server) */
        SelectNone,       /* 101 with no Sec-WebSocket-Protocol header */
        SelectSecond,     /* select the client's second offer (gb-match.*) */
        SelectUnoffered,  /* select a token the client never offered */
    };

    MdkrMatchSignalTestServer();
    ~MdkrMatchSignalTestServer();

    MdkrMatchSignalTestServer(const MdkrMatchSignalTestServer &) = delete;
    MdkrMatchSignalTestServer &operator=(const MdkrMatchSignalTestServer &) =
        delete;

    ProtocolMode protocolMode = ProtocolMode::SelectV1;
    /* When false the upgrade request is read and recorded but never
     * answered: the handshake stalls silently. */
    bool respondToUpgrade = true;
    /* Extra raw header line appended to the 101 (no CRLF), e.g.
     * "Sec-WebSocket-Extensions: permessage-deflate". Set before start(). */
    std::string extra101Header;

    /* Bind 127.0.0.1 on an ephemeral port and start accepting. */
    bool start();
    void stop();
    uint16_t port() const;

    /* ---- Upgrade observation ---- */
    bool waitForUpgrade(unsigned budgetMs = 5000u);
    /* True once the 101 has been written (never true when respondToUpgrade
     * is false); scripted frames sent after this cannot outrun it. */
    bool waitForOpen(unsigned budgetMs = 5000u);
    std::string requestHeadRaw() const; /* full request head, verbatim */
    std::string requestPath() const;
    /* Every token offered via Sec-WebSocket-Protocol, in offer order. */
    std::vector<std::string> offeredProtocols() const;
    bool sawHeader(const std::string &lowercaseName) const;

    /* ---- Scripted server -> client traffic (post-upgrade) ---- */
    bool sendText(const std::string &payload);
    bool sendBinary(const std::string &payload);
    bool sendPing(const std::string &payload);
    /* An unmasked frame whose payload deliberately carries the masked bit /
     * arbitrary opcode, for protocol-violation cases. */
    bool sendRawFrame(uint8_t firstByte, bool maskBit,
                      const std::string &payload);
    bool sendClose(uint16_t code, const std::string &reason);
    /* Abrupt TCP close, no close frame. */
    void dropConnection();
    /* Stop draining the client's bytes: the kernel receive window fills and
     * a client that keeps writing eventually blocks in send. The connection
     * stays open; only reading pauses. */
    void stopReading();

    /* ---- Client -> server observation ---- */
    bool waitForTextMessages(size_t count, unsigned budgetMs = 5000u);
    std::vector<std::string> textMessages() const;
    /* True once the client's close frame arrived. */
    bool waitForClientClose(unsigned budgetMs = 5000u);
    uint16_t clientCloseCode() const;
    std::string clientCloseReason() const;
    /* True once the client's TCP side is gone (EOF/reset). */
    bool waitForDisconnect(unsigned budgetMs = 5000u);
    /* Every data/close frame the client sent carried the RFC-required
     * mask bit. */
    bool allClientFramesMasked() const;
    /* Pong frames the client sent (payloads, demasked, in order). */
    bool waitForPongs(size_t count, unsigned budgetMs = 5000u);
    std::vector<std::string> pongPayloads() const;

private:
    std::shared_ptr<MdkrMatchSignalTestServerState> state_;
};

#endif /* MDKR_MATCH_SIGNAL_TEST_SERVER_H */
