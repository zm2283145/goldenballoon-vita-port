#!/usr/bin/env python3
"""Bind shared socket-library leases to real first-party owners; no network I/O."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]


def section(source, start, end):
    return source.split(start, 1)[1].split(end, 1)[0]


class NetworkLifetimeSourceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.room = (ROOT / "platform/online/match_live_transport.cpp").read_text()
        cls.signal = (ROOT / "platform/online/match_signal_client.cpp").read_text()
        cls.lan = (ROOT / "platform/party/lan_party_server.cpp").read_text()
        cls.launcher = (ROOT / "platform/app/ui_launcher.cpp").read_text()

    def test_native_calls_are_centralized_behind_the_tested_policy(self):
        native = (ROOT / "platform/net/native_socket_lifetime.h").read_text()
        self.assertIn("MdkrWinsockReferencePolicy<MdkrNativeSocketApi>", native)
        self.assertIn("::WSAStartup(MAKEWORD(2, 2), &data) != 0", native)
        self.assertIn("version = data.wVersion", native)
        self.assertIn("return ::WSACleanup() == 0", native)
        for source in (self.room, self.signal, self.lan):
            self.assertNotIn("WSAStartup(", source)
            self.assertNotIn("WSACleanup(", source)
            self.assertIn('"net/native_socket_lifetime.h"', source)

    def test_resolver_results_retire_before_lease_and_global_permit(self):
        for source in (self.room, self.signal):
            task = section(source, "struct ResolveTask {", "\n};")
            self.assertLess(task.index("AsyncWorkBudget::Permit permit;"),
                            task.index("MdkrNativeSocketLease networkLease;"))
            self.assertLess(task.index("MdkrNativeSocketLease networkLease;"),
                            task.index("AddrinfoOwner results"))
            self.assertIn("networkLease(network)", task)
            self.assertIn("std::make_shared<ResolveTask>(std::move(permit), network)", source)

    def test_room_and_signal_sockets_keep_owned_prerequisites(self):
        wire = section(self.room, "class WireSocket {", "\n};")
        self.assertLess(wire.index("networkLease_.acquire()"), wire.index("connectWithDeadline("))
        self.assertLess(wire.index("mbedtls_net_free(&net_)"), wire.index("networkLease_.reset()"))
        factory = section(self.signal, "MdkrMatchSignalClient::create(", "\nbool MdkrMatchSignalClient::connect(")
        self.assertIn("if (!state->networkLease.acquire()) return refuse(kMdkrMatchSignalTransportLost)", factory)
        close = section(self.signal, "void MdkrMatchSignalClient::close()", "\nMdkrMatchSignalSnapshot")
        self.assertLess(close.index("toJoin = std::move(state.thread)"), close.index("state.enqueueLocked"))
        self.assertIn("} catch (...) { /* Closed/stopping", close)
        self.assertLess(close.index("toJoin.join()"), close.index("state.networkLease.reset()"))

    def test_cold_lan_enumeration_owns_and_releases_lookup_resources(self):
        addresses = section(self.lan, "mdkr_lan_party_machine_ipv4_addresses()", "\n    return result;")
        self.assertLess(addresses.index("network.acquire()"), addresses.index("::gethostname("))
        self.assertIn("owner(list, ::freeaddrinfo)", addresses)
        self.assertIn("owner(interfaces, ::freeifaddrs)", addresses)

    def test_listener_and_retained_aliases_own_their_lifetimes(self):
        start = section(self.lan, "bool MdkrLanPartyServer::start(", "\nvoid MdkrLanPartyServer::stop()")
        self.assertLess(start.index("network.acquire()"), start.index("::socket("))
        self.assertIn("SocketOwner socket{fd}", start)
        self.assertIn("state_->running || state_->stopping", start)
        self.assertLess(start.index("state_->networkLease = network"), start.index("std::thread(acceptLoop"))
        self.assertLess(start.index("std::thread(acceptLoop"), start.index("socket.release()"))
        rollback = start.split("} catch (...)", 1)[1]
        self.assertIn("state_->listenFd = kInvalidSocket", rollback)
        self.assertIn("state_->networkLease.reset()", rollback)
        self.assertIn("wsState->networkLease = state->networkLease", self.lan)
        stop = section(self.lan, "void MdkrLanPartyServer::stop()", "\nuint16_t MdkrLanPartyServer::port()")
        self.assertLess(stop.index("connection->thread.join()"), stop.index("state_->networkLease.reset()"))

    def test_terminal_verdict_includes_native_cleanup_failure(self):
        verdict = section(self.launcher, "bool Launcher::networkShutdownFailed()", "\nbool Launcher::finishNetworkShutdownForExit()")
        self.assertIn("completion.failed() || mdkrFirstPartyNetworkCleanupFailed.load()", verdict)
        poll = section(self.launcher, "bool Launcher::pollNetworkShutdown()", "\nbool Launcher::networkShutdownFailed()")
        self.assertIn("onlineResolverWorkInUse() == 0u", poll)
        self.assertIn("if (finished && networkShutdownFailed()", poll)
        self.assertIn("first-party socket-library cleanup FAILED", poll)

    def test_listener_checks_blocking_io_prerequisites_before_worker_admission(self):
        accept = self.lan.split("void acceptLoop(", 1)[1]
        options = section(self.lan, "bool setSocketTimeouts(", "bool lastErrorWasTimeout()")
        self.assertLess(accept.index("SocketOwner socket{client}"),
                        accept.index("!setSocketTimeouts(client)"))
        self.assertLess(accept.index("!setSocketTimeouts(client)"),
                        accept.index("std::thread(serveConnection"))
        self.assertIn("SO_RCVTIMEO", options)
        self.assertIn("SO_SNDTIMEO", options)
        self.assertIn("SO_NOSIGPIPE", options)
        self.assertEqual(options.count("!= 0) return false"), 5)

    def test_connection_retires_callback_captures_before_clearing_worker_identity(self):
        completion = section(self.lan, "struct ConnectionCompletion {", "\n};")
        self.assertLess(completion.index("webSocket->closeSent = true"),
                        completion.index("std::exchange(connection->fd, kInvalidSocket)"))
        self.assertIn("lock(connection->socketMutex)", completion)
        self.assertLess(completion.index("webSocket.reset()"),
                        completion.index("servingServer = nullptr"))
        self.assertLess(completion.index("servingServer = nullptr"),
                        completion.index("connection->done = true"))


if __name__ == "__main__":
    unittest.main()
