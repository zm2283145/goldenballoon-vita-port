#!/usr/bin/env python3
"""Prove the real launcher/controller WebRTC path and input-test round trip."""

from __future__ import annotations

import argparse
import json
import sys
import tempfile
import time
from pathlib import Path

from check_browser_runtime import (
    CDPClient, ChromeProcess, CheckFailure, OverlayServer, find_chrome,
    page_websocket, require, wait_value,
)

ROOT = Path(__file__).resolve().parent.parent


def client(chrome_path: str, profile: Path, flags: list[str], verbose: bool) -> tuple[ChromeProcess, CDPClient]:
    process = ChromeProcess(chrome_path, profile, flags, verbose)
    connection = CDPClient(page_websocket(process.wait_port()))
    for domain in ("Page", "Runtime", "Log", "Inspector"):
        connection.call(f"{domain}.enable")
    return process, connection


def relay(source: CDPClient, target: CDPClient, source_expr: str, target_expr: str) -> int:
    messages = source.evaluate(source_expr) or []
    for message in messages:
        target.evaluate(f"{target_expr}({json.dumps(message, separators=(',', ':'))})")
    return len(messages)


def run(args: argparse.Namespace) -> None:
    shell = (ROOT / args.shell_dir).resolve()
    server = OverlayServer(shell, shell)
    server.start()
    chrome_path = find_chrome(args.chrome)
    with tempfile.TemporaryDirectory(prefix="mdkr64_party_rtc_") as temp:
        base = Path(temp)
        host_process, host = client(chrome_path, base / "host", args.chrome_flag, args.verbose)
        phone_process, phone = client(chrome_path, base / "phone", args.chrome_flag, args.verbose)
        try:
            room_state = {"type": "room_state", "transitionId": 2,
                          "controllers": [{"controllerId": "phone-one",
                              "name": "Test phone",
                              "phase": "leased", "seat": 1, "leaseGeneration": 7,
                              "connectionSequence": 1}]}
            host_source = """
              globalThis.__mdkrPartyHostTestConfig={
                initialRoomState:%s,
                async request(path){return {roomId:'abcdefghijklmnopqrstuv',
                  hostCredential:'H'.repeat(43),fallbackCode:'123456',
                  inviteGeneration:1,inviteExpiresInMs:120000,
                  controllerUrl:location.origin+'/controller/#'+'A'.repeat(43)}}
              };
            """ % json.dumps(room_state, separators=(",", ":"))
            phone_source = """
              Object.defineProperty(navigator,'vibrate',{configurable:true,
                value:(milliseconds)=>{globalThis.__mdkrVibration=milliseconds;return true;}});
              globalThis.__mdkrControllerTestConfig={
                directSignaling:true,autoApprove:false,seat:1,connectionSequence:1,
                capability:'A'.repeat(43),
                async request(){return {roomId:'abcdefghijklmnopqrstuv',
                  controllerId:'phone-one',credential:'C'.repeat(43),protocol:2,
                  phrase:'Bright Balloon'}}
              };
            """
            host.call("Page.addScriptToEvaluateOnNewDocument", {"source": host_source})
            phone.call("Page.addScriptToEvaluateOnNewDocument", {"source": phone_source})
            host.call("Page.navigate", {"url": server.origin + "/"})
            phone.call("Page.navigate", {"url": server.origin + "/controller/#secret"})
            wait_value(host, "Boolean(globalThis.MDKRPartyHost)", bool,
                       "host party API", args.timeout)
            wait_value(phone, "Boolean(globalThis.__mdkrControllerTest)", bool,
                       "controller test API", args.timeout)
            host.evaluate("globalThis.MDKRPartyHost.setRomReady(true); globalThis.MDKRPartyHost.open()")
            wait_value(host, "!document.getElementById('party-room').hidden", bool,
                       "host room", args.timeout)

            # P2.1 compare-then-trust: relay signaling until the direct channels
            # are up — a PROVISIONAL connection. Approval alone grants no seat
            # custody: the pad must NOT be active (input discarded at the
            # ingress), the seat row must not read as connected/racing, and the
            # phone must hold on the compare screen with its controller locked.
            # (This fixture's fake room_state carries no controllerPublicKey, so
            # the host cannot derive the phrase and the seat stays in its
            # phraseless provisional state; the real key + rendered phrase +
            # matched Words Match control are exercised in check_party_native_e2e
            # against the live Worker.)
            deadline = time.monotonic() + args.timeout
            provisional = False
            while time.monotonic() < deadline:
                relay(phone, host,
                      "globalThis.__mdkrControllerTestState.signals?.splice(0) || []",
                      "globalThis.MDKRPartyHost.receiveSignal")
                relay(host, phone,
                      "globalThis.__mdkrPartyHostTestState.signals?.splice(0) || []",
                      "globalThis.__mdkrControllerTest.receiveSignal")
                provisional = host.evaluate(
                    "globalThis.MDKRPartyHost.remotePads()[0].provisional === true")
                if provisional:
                    break
                time.sleep(0.03)
            require(bool(provisional),
                    "direct WebRTC controller never reached the provisional state")
            require(not host.evaluate(
                        "globalThis.MDKRPartyHost.remotePads()[0].active"),
                    "unconfirmed phone took input custody before Words Match")
            seat_small = host.evaluate(
                "document.querySelector('[data-seat=\\\"1\\\"] small').textContent")
            require(seat_small != "Phone connected" and
                    "reconnecting" not in seat_small.lower(),
                    f"provisional seat mislabeled as connected/reconnecting: {seat_small!r}")
            require(phone.evaluate(
                        "globalThis.__mdkrControllerTest.state().phase") == "assigned",
                    "phone advanced past the compare screen before Words Match")
            require(bool(phone.evaluate(
                        "document.getElementById('use-controller').disabled")),
                    "phone unlocked its controller before the host confirmed")

            # Words Match: the host confirms (the same call the seat tile's
            # button makes). Seat custody begins and the phone runs the auto
            # input test itself and advances — no Press Go tap.
            host.evaluate("globalThis.MDKRPartyHost.confirm('phone-one')")
            deadline = time.monotonic() + args.timeout
            connected = False
            while time.monotonic() < deadline:
                relay(phone, host,
                      "globalThis.__mdkrControllerTestState.signals?.splice(0) || []",
                      "globalThis.MDKRPartyHost.receiveSignal")
                relay(host, phone,
                      "globalThis.__mdkrPartyHostTestState.signals?.splice(0) || []",
                      "globalThis.__mdkrControllerTest.receiveSignal")
                connected = host.evaluate("globalThis.MDKRPartyHost.remotePads()[0].active")
                if connected:
                    break
                time.sleep(0.03)
            require(bool(connected),
                    "confirmed phone did not take input custody after Words Match")
            wait_value(phone, "globalThis.__mdkrControllerTest.state().phase",
                       lambda value: value == "controller",
                       "auto input test advanced to the controller surface",
                       args.timeout)
            require(bool(phone.evaluate(
                        "!document.getElementById('use-controller').disabled")),
                    "auto input test did not also unlock the manual fallback")
            phone.evaluate("""(() => {
              const go=document.querySelector('.touch-go');
              const r=go.getBoundingClientRect();
              go.parentElement.dispatchEvent(new PointerEvent('pointerdown',{
                pointerId:31,pointerType:'touch',clientX:(r.left+r.right)/2,
                clientY:(r.top+r.bottom)/2,bubbles:true,cancelable:true}));
            })()""")
            wait_value(host, "globalThis.MDKRPartyHost.remotePads()[0].packets.length",
                       lambda value: isinstance(value, int) and value > 0,
                       "unordered pad-state packet", args.timeout)
            packet = host.evaluate("""(() => {
              const p=globalThis.MDKRPartyHost.remotePads()[0];
              return {owner:p.owner,connectionSequence:p.connectionSequence,
                length:p.packets.at(-1).byteLength,drops:p.drops};
            })()""")
            require(packet["owner"] == 57 and packet["connectionSequence"] == 1 and
                    24 <= packet["length"] <= 64 and packet["drops"] == 0,
                    f"remote pad handoff metadata invalid: {packet}")
            require(bool(host.evaluate("globalThis.MDKRPartyHost.remotePads()[0].haptics")),
                    "phone vibration capability did not cross reliable control")
            require(bool(host.evaluate("globalThis.MDKRPartyHost.remotePads()[0].rumble(32768)")),
                    "host could not send bounded phone rumble")
            wait_value(phone, "globalThis.__mdkrVibration", lambda value: value == 250,
                       "optional phone vibration", args.timeout)
            require(bool(host.evaluate("globalThis.MDKRPartyHost.testPingControl()")),
                    "host refused a reliable-channel liveness probe")
            wait_value(host,
                       "globalThis.__mdkrPartyHostTestState.controlPongs",
                       lambda value: value == 1,
                       "reliable control ping/pong", args.timeout)
            # RTT instrumentation (RC checklist items 33/50 gain a number):
            # the matched pong must carry a measured round trip across the
            # seam, and the value must be plausible — above zero, and under
            # the 5 s that would mean the direct channel is unusable.
            rtt = host.evaluate(
                "globalThis.__mdkrPartyHostTestState.controlRtts?.at(-1)")
            require(isinstance(rtt, (int, float)) and 0 < rtt < 5000,
                    f"control-channel RTT sample missing or implausible: {rtt!r}")
            seat_status = host.evaluate(
                "document.querySelector('[data-seat=\\\"1\\\"] small').textContent")
            require("ms · direct" in seat_status,
                    f"seat row does not surface the RTT: {seat_status!r}")
            # The phone's own status pill: its bounded 5 s probe rides the
            # existing input_test/input_test_ack round trip, so a plausible
            # number appears without any new protocol message.
            phone_rtt = wait_value(phone,
                "(() => { const rtts = globalThis.__mdkrControllerTestState.rtts;"
                " return rtts?.length ? rtts[rtts.length-1] : null; })()",
                lambda value: isinstance(value, (int, float)) and 0 < value < 5000,
                "phone-side RTT sample", args.timeout)
            pill = phone.evaluate("""(() => {
              const pill = document.getElementById('rtt-pill');
              return {hidden: pill.hidden, text: pill.textContent};
            })()""")
            require(pill["hidden"] is False and pill["text"].endswith(" ms"),
                    f"phone RTT pill missing or unlabeled: {pill}")

            # F1 session-alive names: the phone renames itself over the live
            # control channel and the host seat row updates without any room
            # state transition.
            phone.evaluate("""(() => {
              document.getElementById('settings-open').click();
              const field = document.getElementById('device-name-live');
              field.value = 'Blue Racer';
              field.dispatchEvent(new Event('change', {bubbles:true}));
            })()""")
            wait_value(host,
                       "document.querySelector('[data-seat=\\\"1\\\"] strong').textContent",
                       lambda value: value == "Blue Racer",
                       "live rename crossed the direct control channel", args.timeout)
            # All three surfaces share the redeem-time byte bound: a 13-emoji
            # name is 13 code points but 52 UTF-8 bytes, over the 48-byte cap
            # the native host refuses — the browser host must refuse it too.
            # The control channel is ordered, so if the oversize name were
            # accepted it would re-render the room once before the valid
            # sentinel does; the render count pins the refusal exactly.
            renders_before = host.evaluate(
                "globalThis.__mdkrPartyHostTestState.rooms.length")
            phone.evaluate("""(() => {
              const field = document.getElementById('device-name-live');
              field.value = '🎈'.repeat(13);
              field.dispatchEvent(new Event('change', {bubbles:true}));
              field.value = 'Red Racer';
              field.dispatchEvent(new Event('change', {bubbles:true}));
            })()""")
            wait_value(host,
                       "document.querySelector('[data-seat=\\\"1\\\"] strong').textContent",
                       lambda value: value == "Red Racer",
                       "valid rename after the oversize one", args.timeout)
            renders_after = host.evaluate(
                "globalThis.__mdkrPartyHostTestState.rooms.length")
            require(renders_after - renders_before == 1,
                    "a 52-byte name crossed the browser host's 48-byte bound")
            phone.evaluate(
                "document.querySelector('#settings-dialog .icon-button').click()")
            wait_value(phone, "!document.getElementById('settings-dialog').open",
                       bool, "controller settings closed after rename", args.timeout)

            initial_peer_evidence = host.evaluate(
                "({creations:globalThis.__mdkrPartyHostTestState.peerCreations,"
                "generation:globalThis.MDKRPartyHost.testPeerState().generation})")
            require(initial_peer_evidence == {"creations": 1, "generation": 1},
                    f"initial direct handshake churned peers: {initial_peer_evidence}")
            initial_generation = initial_peer_evidence["generation"]
            require(bool(host.evaluate("globalThis.MDKRPartyHost.testRestartIce()",
                                       await_promise=True)),
                    "host refused an ICE restart inside the approved lease")
            deadline = time.monotonic() + args.timeout
            restart_answered = False
            while time.monotonic() < deadline:
                relay(host, phone,
                      "globalThis.__mdkrPartyHostTestState.signals?.splice(0) || []",
                      "globalThis.__mdkrControllerTest.receiveSignal")
                answered = relay(phone, host,
                      "globalThis.__mdkrControllerTestState.signals?.splice(0) || []",
                      "globalThis.MDKRPartyHost.receiveSignal")
                peer_state = host.evaluate("globalThis.MDKRPartyHost.testPeerState()")
                if answered and peer_state and peer_state["signalingState"] == "stable":
                    restart_answered = True
                    break
                time.sleep(0.03)
            require(restart_answered,
                    "ICE restart offer/answer did not settle on the existing peer")
            restart_evidence = host.evaluate(
                "({restarts:globalThis.__mdkrPartyHostTestState.iceRestarts,"
                "creations:globalThis.__mdkrPartyHostTestState.peerCreations,"
                "generation:globalThis.MDKRPartyHost.testPeerState()?.generation})")
            require(restart_evidence == {"restarts": 1,
                                        "creations": initial_peer_evidence["creations"],
                                        "generation": initial_generation},
                    f"ICE restart did not preserve the approved peer: "
                    f"before={initial_peer_evidence} after={restart_evidence}")

            phone.evaluate("dispatchEvent(new PointerEvent('pointerup',{pointerId:31,bubbles:true}))")
            require(bool(host.evaluate("globalThis.MDKRPartyHost.testCloseControl()")),
                    "control-channel failure could not be injected")
            wait_value(host, "!globalThis.MDKRPartyHost.remotePads()[0].active",
                       bool, "host fail-neutral after reliable channel close", args.timeout)
            wait_value(phone,
                       "globalThis.__mdkrControllerTest.state().phase === 'reconnecting'",
                       bool, "phone fail-neutral after reliable channel close", args.timeout)
            deadline = time.monotonic() + args.timeout
            recovered = False
            while time.monotonic() < deadline:
                relay(host, phone,
                      "globalThis.__mdkrPartyHostTestState.signals?.splice(0) || []",
                      "globalThis.__mdkrControllerTest.receiveSignal")
                relay(phone, host,
                      "globalThis.__mdkrControllerTestState.signals?.splice(0) || []",
                      "globalThis.MDKRPartyHost.receiveSignal")
                recovered = bool(host.evaluate(
                    "globalThis.MDKRPartyHost.remotePads()[0].active")) and bool(
                    phone.evaluate(
                    "globalThis.__mdkrControllerTest.state().phase === 'controller'"))
                if recovered:
                    break
                time.sleep(0.03)
            require(recovered, "controller did not recover after reliable channel close")
            recovery = host.evaluate("({creations:globalThis.__mdkrPartyHostTestState.peerCreations,"
                "failures:globalThis.__mdkrPartyHostTestState.channelFailures,"
                "generation:globalThis.MDKRPartyHost.testPeerState().generation})")
            require(recovery["creations"] == initial_peer_evidence["creations"] + 1 and
                    recovery["failures"] >= 1 and
                    recovery["generation"] > initial_generation,
                    f"channel recovery did not use one fresh peer generation: {recovery}")
            host.evaluate("globalThis.MDKRPartyHost.remotePads()[0].packets.length=0")
            phone.evaluate("""(() => {
              const go=document.querySelector('.touch-go');
              const r=go.getBoundingClientRect();
              go.parentElement.dispatchEvent(new PointerEvent('pointerdown',{
                pointerId:32,pointerType:'touch',clientX:(r.left+r.right)/2,
                clientY:(r.top+r.bottom)/2,bubbles:true,cancelable:true}));
            })()""")
            wait_value(host, "globalThis.MDKRPartyHost.remotePads()[0].packets.length",
                       lambda value: isinstance(value, int) and value > 0,
                       "pad input after control-channel recovery", args.timeout)
            require(bool(host.evaluate("globalThis.MDKRPartyHost.testExpireControl()")),
                    "control watchdog failure could not be injected")
            wait_value(host, "!globalThis.MDKRPartyHost.remotePads()[0].active",
                       bool, "host fail-neutral after control watchdog expiry", args.timeout)
            deadline = time.monotonic() + args.timeout
            watchdog_recovered = False
            while time.monotonic() < deadline:
                relay(host, phone,
                      "globalThis.__mdkrPartyHostTestState.signals?.splice(0) || []",
                      "globalThis.__mdkrControllerTest.receiveSignal")
                relay(phone, host,
                      "globalThis.__mdkrControllerTestState.signals?.splice(0) || []",
                      "globalThis.MDKRPartyHost.receiveSignal")
                watchdog_recovered = bool(host.evaluate(
                    "globalThis.MDKRPartyHost.remotePads()[0].active")) and bool(
                    phone.evaluate(
                    "globalThis.__mdkrControllerTest.state().phase === 'controller'"))
                if watchdog_recovered:
                    break
                time.sleep(0.03)
            require(watchdog_recovered,
                    "controller did not recover after control watchdog expiry")
            require(host.evaluate(
                "globalThis.__mdkrPartyHostTestState.peerCreations") ==
                    initial_peer_evidence["creations"] + 2,
                    "control watchdog recovery did not create exactly one fresh peer")
            require(not host.failures and not phone.failures,
                    "CDP failure in direct controller path")
            print("check_phone_party_webrtc: PASS — authenticated signaling relay, "
                  "direct unordered state/reliable control channels, same-lease ICE "
                  "restart, ping watchdog, fail-neutral control-channel recovery, input-test RTT, "
                  "bounded pad handoff and optional phone haptics")
        finally:
            host.close()
            phone.close()
            host_process.close()
            phone_process.close()
            server.close()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--shell-dir", default="dist/web")
    parser.add_argument("--chrome")
    parser.add_argument("--chrome-flag", action="append", default=[])
    parser.add_argument("--timeout", type=float, default=60.0)
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()
    try:
        run(args)
        return 0
    except (CheckFailure, OSError, ValueError) as error:
        print(f"check_phone_party_webrtc: FAIL — {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
