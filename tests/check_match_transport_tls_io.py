#!/usr/bin/env python3
"""Source contracts for nonblocking TLS retry/cancellation; launches no endpoint."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parent.parent


def section(source, start, end):
    return source.split(start, 1)[1].split(end, 1)[0]


class TlsIoContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.source = (ROOT / "platform/online/match_live_transport.cpp").read_text()
        cls.wire = section(cls.source, "class WireSocket {", "/* ---- HTTP/1.1")

    def test_connection_stays_nonblocking_and_keeps_authentication(self):
        connect = section(self.source, "SocketFd connectWithDeadline(", "struct ParsedOrigin")
        self.assertIn("setBlocking(fd, false)", connect)
        self.assertNotIn("setBlocking(fd, true)", connect)
        self.assertIn("mbedtls_ssl_conf_authmode(&conf_, MBEDTLS_SSL_VERIFY_REQUIRED)", self.wire)
        self.assertIn("mbedtls_ssl_conf_ca_chain(&conf_, &ca_, nullptr)", self.wire)
        self.assertIn("mbedtls_ssl_set_hostname(&ssl_, origin.host.c_str())", self.wire)
        self.assertIn("mbedtls_ssl_get_verify_result(&ssl_) != 0", self.wire)

    def test_tls_uses_nonblocking_bio_not_fatal_timeout_as_polling(self):
        self.assertIn("mbedtls_ssl_set_bio(&ssl_, this, sendTls, recvTls, nullptr)", self.wire)
        self.assertNotIn("mbedtls_net_recv_timeout", self.wire)
        self.assertNotIn("mbedtls_ssl_conf_read_timeout", self.wire)
        self.assertNotIn("MBEDTLS_ERR_SSL_TIMEOUT", self.wire)

    def test_every_tls_socket_call_checks_cancellation_and_operation_deadline(self):
        for name, end, direction, error, want in (
            ("sendTls", "static int recvTls", "send", "SEND_FAILED", "WRITE"),
            ("recvTls", "// True means readiness", "recv", "RECV_FAILED", "READ"),
        ):
            with self.subTest(callback=name):
                callback = section(self.wire, "static int " + name, end)
                cancelled = callback.index("socket.cancelled()")
                deadline = callback.index("nowMs() >= socket.ioDeadline_")
                call = callback.index("return netSendNoSignal" if direction == "send"
                                      else "return mbedtls_net_recv")
                self.assertLess(cancelled, call)
                self.assertLess(deadline, call)
                self.assertIn("MBEDTLS_ERR_NET_" + error, callback)
                self.assertIn("MBEDTLS_ERR_SSL_WANT_" + want, callback)

    def test_plaintext_and_tls_share_sigpipe_safe_send(self):
        helper = section(self.source, "int netSendNoSignal(", "class WireSocket")
        self.assertIn("::send(net->fd, data, length, MSG_NOSIGNAL)", helper)
        self.assertIn("errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR", helper)
        self.assertIn("errno == EPIPE || errno == ECONNRESET", helper)
        self.assertIn("return MBEDTLS_ERR_SSL_WANT_WRITE;", helper)
        self.assertIn("return MBEDTLS_ERR_NET_CONN_RESET;", helper)
        self.assertIn("netSendNoSignal(&net_, data + sent, len - sent)", self.wire)
        self.assertIn("netSendNoSignal(&socket.net_, data, length)", self.wire)
        self.assertNotIn("mbedtls_net_send", self.wire)

    def test_progress_cannot_reset_handshake_or_write_budget(self):
        handshake = section(self.wire, "bool connect(", "/* Every send")
        write = section(self.wire, "bool writeAll(const uint8_t", "bool writeAll(const std::string")
        self.assertLess(handshake.index("ioDeadline_ = deadline"), handshake.index("for (;;)"))
        self.assertLess(handshake.index("cancelled() || nowMs() >= deadline", handshake.index("for (;;)")),
                        handshake.index("mbedtls_ssl_handshake(&ssl_)"))
        self.assertLess(write.index("ioDeadline_ = deadline"), write.index("while (sent < len)"))
        self.assertLess(write.index("cancelled() || nowMs() >= deadline"),
                        write.index("mbedtls_ssl_write(&ssl_, data + sent, len - sent)"))
        retry = section(write, "if (n == MBEDTLS_ERR_SSL_WANT_READ", "if (n <= 0)")
        self.assertIn("continue;", retry)
        self.assertNotIn("sent +=", retry)
        self.assertIn("sent += static_cast<size_t>(n);", write)
        self.assertIn("if (!open_) return false;", write)

    def test_read_polling_is_recoverable_but_errors_disable_reuse(self):
        read = section(self.wire, "int read(", "void close()")
        self.assertIn("if (!open_ || cancelled()) return -1;", read)
        self.assertIn("ioDeadline_ = deadline;", read)
        self.assertIn("if (nowMs() >= deadline) return 0;", read)
        self.assertIn("mbedtls_net_recv(&net_, buf, len)", read)
        self.assertIn("MBEDTLS_ERR_SSL_WANT_READ", read)
        self.assertIn("MBEDTLS_ERR_SSL_WANT_WRITE", read)
        self.assertIn("open_ = false;", section(read, "if (n <= 0)", "return n;"))

    def test_readiness_wait_is_clamped_and_does_not_hide_permanent_errors(self):
        wait = section(self.wire, "bool waitReady(", "mbedtls_net_context net_;")
        self.assertIn("if (cancelled()) return false;", wait)
        self.assertIn("if (now >= deadline) return true;", wait)
        self.assertIn("remaining < kPollSliceMs", wait)
        self.assertIn("static_cast<long>(waitMs)", wait)
        self.assertIn("static_cast<int>(waitMs)", wait)
        self.assertIn("return result >= 0 || WSAGetLastError() == WSAEINTR;", wait)
        self.assertIn("return result >= 0 || errno == EINTR;", wait)


if __name__ == "__main__":
    unittest.main()
