#!/usr/bin/env python3
"""Exercise the standalone, engine-free phone controller in real Chromium."""

from __future__ import annotations

import argparse
import json
import sys
import tempfile
from pathlib import Path

from check_browser_runtime import (
    CDPClient,
    ChromeProcess,
    CheckFailure,
    OverlayServer,
    find_chrome,
    page_websocket,
    require,
    wait_value,
)


ROOT = Path(__file__).resolve().parent.parent


def load_controller(cdp: CDPClient, url: str, label: str, timeout: float) -> None:
    """Force a fresh controller document at ``url``.

    ``Page.navigate`` from ``/controller/`` to ``/controller/#<secret>`` is a
    same-document fragment navigation: no document is created, so neither the
    entry code nor the pending ``Page.addScriptToEvaluateOnNewDocument``
    configuration ever runs and the check would silently observe the previous
    document.  Detour through ``about:blank`` so each entry case really is a
    fresh load.
    """
    cdp.call("Page.navigate", {"url": "about:blank"})
    wait_value(cdp, "location.href", lambda value: value == "about:blank",
               f"{label} blank detour", timeout)
    cdp.call("Page.navigate", {"url": url})


def resolve(value: str) -> Path:
    path = Path(value).expanduser()
    if not path.is_absolute():
        path = ROOT / path
    return path.resolve()


def run(args: argparse.Namespace) -> None:
    shell = resolve(args.shell_dir)
    chrome_path = find_chrome(args.chrome)
    required = (
        shell / "controller" / "index.html",
        shell / "controller" / "controller.css",
        shell / "controller" / "controller.js",
        shell / "input" / "touch-surface.js",
        shell / "party" / "party-protocol.js",
        shell / "party" / "party-sas.js",
        shell / "_headers",
    )
    for path in required:
        require(path.is_file(), f"controller artifact is missing: {path}")
    critical_bytes = sum(path.stat().st_size for path in required[:6])
    # 127 KiB, raised across the phases: to 101 KiB when accumulated honest
    # error copy (the protocol_update_required entry) crossed the original 100
    # KiB, to 104 KiB when SAS v2 put the fingerprint parser + connection-time
    # derivation on the critical path, to 117 KiB when local (LAN) play made
    # party-sas.js vendor a pure-JS SHA-256 + P-256 ECDH twin (the phrase must
    # stay byte-identical to the native host or every pairing looks like a MITM)
    # and controller.js took on the NoSleep keep-awake, to 123 KiB when the
    # phone learned to pair over a LAN: controller.js now carries the whole
    # redeem-over-ws transport (the LAN room authenticates the first ws frame,
    # not an HTTP POST), the relaxed trusted-origin gate that trusts only the
    # host that served the page, and the crypto.subtle fallback wiring, and to
    # 126 KiB when the server-declared local/cloud mode handling had already
    # crossed 123 and the page then took on the strict validator for
    # server-delivered iceServers (the zero-cost TURN path, which must be
    # revalidated and rebuilt client-side, never trusted verbatim), and to
    # 127 KiB when that validator learned to refuse credentials on any
    # non-TURN url (relay credentials must never reach a stun server), and to
    # 132 KiB when Phase 2's join compression landed: the page now runs the
    # input-test round trip itself on channel open and advances when it
    # passes (bounded window, Press Go kept as the fallback, the pre-channel
    # press narrated honestly instead of dead-ending), says "Connecting…"
    # for a lease that never connected, leads the Approved screen with the
    # pairing phrase and keeps it one tap away while racing, and asks for
    # the phone's name BEFORE redeem plus renames it live over the control
    # channel (controller_rename), and to 135 KiB when Phase 2's
    # session/presence batch gave the racing surface an RTT status pill (a
    # bounded 5 s input_test probe over the already-open reliable channel,
    # timed at the ack) and taught the reconnect surface the specific
    # network-blocks-phone-to-display diagnosis after three straight peer
    # failures with a healthy room socket -- the same sentence the native
    # and browser hosts speak, and to 140 KiB when Phase 2's device/platform
    # batch added the Android immersive path (request fullscreen + lock
    # landscape on the Use-controller gesture, both feature-detected, deferred
    # to the first touch when the surface was auto-advanced into, released on
    # leave) and the in-app-webview User-Agent heuristic that routes known
    # embedded browsers (FBAN/Instagram/Line/"; wv"/... ) to the existing
    # Continue-in-Safari/Chrome card without false-positiving a real browser,
    # and to 141 KiB when P2.1's compare-then-trust reorder made the phone hold
    # on the compare screen until the host confirms the phrase: the page now
    # handles the seat_confirmed control message, gates the auto input test on
    # it, and leads the Approved card with the compare-and-confirm copy -- the
    # phone-side half of moving seat custody after the human's Words Match,
    # and to 148 KiB when P2.2/P2.3 gave the phone in-race feedback: it now
    # handles the race_state control message, paints a compact in-race readout
    # (item, lap, position, countdown, finish) into the status pill, and plays a
    # distinct navigator.vibrate pattern per event already crossing to it
    # (countdown beat, boost/overtake, bump, item pickup, final lap, finish) --
    # with the vibration setting hidden and honestly unsupported where the
    # Vibration API is absent (iOS Safari), no fake.
    # The guardrail's job is catching runaway
    # growth -- a bundled library, an accidental asset -- not vetoing player
    # copy, the MITM defense or a real second pairing transport, so the
    # ceiling moves by the smallest whole KiB each time.
    require(critical_bytes < 148 * 1024,
            f"controller critical path is {critical_bytes} bytes, budget is 148 KiB")
    headers = (shell / "_headers").read_text(encoding="utf-8")
    for value in ("frame-ancestors 'none'", "Referrer-Policy: no-referrer",
                  "X-Content-Type-Options: nosniff", "Cache-Control: no-store"):
        require(value in headers, f"controller header contract lacks {value!r}")

    server = OverlayServer(shell, shell)
    server.start()
    with tempfile.TemporaryDirectory(prefix="mdkr64_controller_profile_") as profile:
        chrome = ChromeProcess(chrome_path, Path(profile), args.chrome_flag, args.verbose)
        cdp: CDPClient | None = None
        try:
            cdp = CDPClient(page_websocket(chrome.wait_port()))
            for domain in ("Page", "Runtime", "Log", "Inspector", "Accessibility"):
                cdp.call(f"{domain}.enable")
            config = {
                "capability": "controller-test-capability",
                "phrase": "Swift Balloon",
                "seat": 3,
                "autoApprove": True,
            }
            cdp.call("Page.addScriptToEvaluateOnNewDocument", {"source":
                "globalThis.__mdkrControllerTestConfig=" +
                json.dumps(config, separators=(",", ":")) + ";" +
                "globalThis.__copiedControllerLink='';" +
                "globalThis.__sharedControllerLink=null;" +
                "Object.defineProperty(navigator,'clipboard',{configurable:true," +
                "value:{writeText:async(value)=>{globalThis.__copiedControllerLink=value;}}});" +
                "Object.defineProperty(navigator,'share',{configurable:true," +
                "value:async(value)=>{globalThis.__sharedControllerLink=value;}});"})
            cdp.call("Emulation.setDeviceMetricsOverride", {
                "width": 320, "height": 568, "deviceScaleFactor": 2,
                "mobile": True,
            })
            secret = "AbCdEfGhIjKlMnOpQrStUv"
            cdp.call("Page.navigate", {
                "url": server.origin + "/controller/#" + secret})

            assigned = wait_value(
                cdp,
                """(() => ({
                  phase: globalThis.__mdkrControllerTest &&
                    globalThis.__mdkrControllerTest.state().phase,
                  hash: location.hash,
                  title: document.getElementById("assigned-title")?.textContent,
                  phrase: document.getElementById("pairing-phrase")?.textContent,
                  phraseLeads: Boolean(document.querySelector('#state-assigned .phrase') &&
                    (document.querySelector('#state-assigned .phrase')
                      .compareDocumentPosition(document.getElementById('assigned-title')) &
                     Node.DOCUMENT_POSITION_FOLLOWING)),
                  overflow: document.documentElement.scrollWidth > innerWidth
                }))()""",
                lambda value: isinstance(value, dict) and value.get("phase") == "assigned",
                "approved controller state", args.timeout)
            require(assigned["hash"] == "", "bearer fragment remained in the address bar")
            require(assigned["title"] == "Controller 3", f"seat copy: {assigned}")
            require(assigned["phrase"] == "Swift Balloon", f"phrase copy: {assigned}")
            # F10: the phrase is the screen's lead — it comes before the seat
            # heading and everything else on the assigned card.
            require(assigned["phraseLeads"], f"phrase does not lead the assigned card: {assigned}")
            require(not assigned["overflow"], "320×568 controller layout overflows horizontally")

            cdp.evaluate('document.getElementById("input-test").click()')
            wait_value(cdp,
                "!document.getElementById('use-controller').disabled", bool,
                "input test success", args.timeout)
            cdp.evaluate('document.getElementById("use-controller").click()')
            wait_value(cdp,
                "globalThis.__mdkrControllerTest.state().phase",
                lambda value: value == "controller", "controller surface", args.timeout)

            # Real Pointer Events through the shared surface: Go -> Drift slide,
            # second-finger Item chord, then cancellation to exact neutral.
            result = cdp.evaluate("""(() => {
              const actions = document.querySelector('.touch-actions');
              const go = document.querySelector('.touch-go').getBoundingClientRect();
              const drift = document.querySelector('.touch-drift').getBoundingClientRect();
              const item = document.querySelector('.touch-item').getBoundingClientRect();
              const send = (target, type, id, rect) => target.dispatchEvent(
                new PointerEvent(type, {pointerId:id, pointerType:'touch',
                  clientX:(rect.left+rect.right)/2, clientY:(rect.top+rect.bottom)/2,
                  bubbles:true, cancelable:true}));
              send(actions, 'pointerdown', 11, go);
              const goBits = globalThis.__mdkrControllerTest.state().pad.buttons;
              send(window, 'pointermove', 11, drift);
              const driftBits = globalThis.__mdkrControllerTest.state().pad.buttons;
              send(actions, 'pointerdown', 12, item);
              const chordBits = globalThis.__mdkrControllerTest.state().pad.buttons;
              send(window, 'pointercancel', 12, item);
              send(window, 'pointercancel', 11, drift);
              const neutralBits = globalThis.__mdkrControllerTest.state().pad.buttons;
              return {goBits, driftBits, chordBits, neutralBits,
                packets: globalThis.__mdkrControllerTestState.packets.length};
            })()""")
            require(result.get("goBits") == 32768, f"Go did not publish A: {result}")
            require(result.get("driftBits") == 32768 | 16, f"Drift chord: {result}")
            require(result.get("chordBits") == 32768 | 16 | 8192,
                    f"three-button chord: {result}")
            require(result.get("neutralBits") == 0 and result.get("packets", 0) >= 5,
                    f"controller did not return neutral: {result}")

            keyboard_stick = cdp.evaluate("""(() => {
              const stick = document.getElementById('phone-touch-stick');
              const send = (type, key) => stick.dispatchEvent(new KeyboardEvent(type,
                {key, bubbles:true, cancelable:true}));
              stick.focus();
              send('keydown', 'ArrowUp');
              const up = {...globalThis.__mdkrControllerTest.state().pad};
              send('keydown', 'd');
              const diagonal = {...globalThis.__mdkrControllerTest.state().pad};
              const direction = stick.getAttribute('aria-valuetext');
              send('keyup', 'ArrowUp');
              send('keyup', 'd');
              const neutral = {...globalThis.__mdkrControllerTest.state().pad};
              return {up, diagonal, direction, neutral};
            })()""")
            require(keyboard_stick["up"]["stickY"] == 80 and
                    keyboard_stick["up"]["stickX"] == 0,
                    f"keyboard up did not steer: {keyboard_stick}")
            require(keyboard_stick["diagonal"]["stickX"] == 57 and
                    keyboard_stick["diagonal"]["stickY"] == 57 and
                    keyboard_stick["direction"] == "Up right",
                    f"keyboard diagonal was not bounded/described: {keyboard_stick}")
            require(keyboard_stick["neutral"]["stickX"] == 0 and
                    keyboard_stick["neutral"]["stickY"] == 0,
                    f"keyboard steering did not return neutral: {keyboard_stick}")

            lifecycle_resume = cdp.evaluate("""(() => {
              dispatchEvent(new Event('freeze'));
              const frozen=globalThis.__mdkrControllerTest.state();
              dispatchEvent(new Event('resume'));
              const resumed=globalThis.__mdkrControllerTest.state();
              const packet=globalThis.__mdkrControllerTestState.packets.at(-1).decoded;
              return {frozen,resumed,packet};
            })()""")
            require(lifecycle_resume["frozen"]["active"] is False and
                    lifecycle_resume["frozen"]["pad"] == {
                        "buttons": 0, "stickX": 0, "stickY": 0} and
                    lifecycle_resume["resumed"]["active"] is True and
                    lifecycle_resume["resumed"]["phase"] == "controller" and
                    lifecycle_resume["packet"]["buttons"] == 0 and
                    lifecycle_resume["packet"]["edges"] == [],
                    f"freeze/resume left dead controls or replayed input: "
                    f"{lifecycle_resume}")

            reconnect = cdp.evaluate("""(() => {
              globalThis.__mdkrControllerTest.reconnect(2);
              const before = globalThis.__mdkrControllerTest.state();
              globalThis.__mdkrControllerTest.reconnectComplete(9);
              return {before, after: globalThis.__mdkrControllerTest.state(),
                neutralizations: globalThis.__mdkrControllerTestState.neutralizations};
            })()""")
            require(reconnect["before"]["phase"] == "reconnecting" and
                    reconnect["before"]["pad"]["buttons"] == 0,
                    f"reconnect did not neutralize: {reconnect}")
            require(reconnect["after"]["phase"] == "controller" and
                    reconnect["after"]["connectionSequence"] == 9,
                    f"reconnect epoch did not advance: {reconnect}")

            cdp.evaluate("document.getElementById('settings-open').click()")
            wait_value(cdp, "document.getElementById('settings-dialog').open", bool,
                       "controller settings", args.timeout)
            # F1 session-alive names: renaming while connected sends
            # controller_rename over the transport and persists locally.
            rename_evidence = cdp.evaluate("""(() => {
              const field = document.getElementById('device-name-live');
              field.value = 'Zed’s phone';
              field.dispatchEvent(new Event('change', {bubbles:true}));
              return {sent: globalThis.__mdkrControllerTestState.renames,
                stored: localStorage.getItem('gb-controller-name'),
                mirrored: document.getElementById('device-name').value};
            })()""")
            require(rename_evidence == {"sent": ["Zed’s phone"],
                        "stored": "Zed’s phone", "mirrored": "Zed’s phone"},
                    f"connected rename was not sent and persisted: {rename_evidence}")
            cdp.evaluate("document.getElementById('leave-controller').click()")
            leave_prompt = wait_value(cdp, """(() => ({
              open:document.getElementById('leave-dialog').open,
              phase:globalThis.__mdkrControllerTest.state().phase,
              focus:document.activeElement?.id
            }))()""", lambda value: isinstance(value, dict) and
                value.get("open") is True, "controller leave confirmation", args.timeout)
            require(leave_prompt["phase"] == "controller" and
                    leave_prompt["focus"] == "leave-cancel",
                    f"controller leave confirmation was not safe by default: {leave_prompt}")
            cdp.evaluate("document.getElementById('leave-cancel').click()")
            wait_value(cdp, "!document.getElementById('leave-dialog').open", bool,
                       "cancelled controller leave", args.timeout)
            require(cdp.evaluate("globalThis.__mdkrControllerTest.state().phase") ==
                    "controller", "cancelling leave disconnected the controller")

            # F1: the optional device-name field sits BEFORE redeem (on the
            # code screen), not on the post-redeem waiting screen; the live
            # rename twin lives in settings.
            cdp.evaluate("globalThis.__mdkrControllerTest.showCode()")
            name_home = cdp.evaluate("""(() => ({
              inCode: document.getElementById('state-code')
                .contains(document.getElementById('device-name')),
              inWaiting: document.getElementById('state-waiting')
                .contains(document.getElementById('device-name')),
              inSettings: Boolean(document.getElementById('device-name-live'))
            }))()""")
            require(name_home == {"inCode": True, "inWaiting": False,
                                  "inSettings": True},
                    f"device name is not asked before redeem: {name_home}")
            cdp.evaluate("""(() => {
              const input = document.getElementById('room-code');
              input.value = '123456';
              input.dispatchEvent(new Event('input', {bubbles:true}));
              document.getElementById('code-form').requestSubmit();
            })()""")
            code_join = wait_value(cdp,
                "globalThis.__mdkrControllerTest.state().phase",
                lambda value: value == "assigned", "fallback-code approval", args.timeout)
            require(code_join == "assigned", "fallback code did not reach assigned state")

            cdp.call("Emulation.setPageScaleFactor", {"pageScaleFactor": 2})
            scaled = cdp.evaluate("({w:document.documentElement.scrollWidth, v:innerWidth})")
            require(scaled["w"] <= scaled["v"], f"200% layout overflowed: {scaled}")

            cdp.evaluate("""(() => {
              document.querySelector('#state-assigned [data-action="leave"]').click();
            })()""")
            wait_value(cdp, "document.getElementById('leave-dialog').open", bool,
                       "assigned leave confirmation", args.timeout)
            require(cdp.evaluate("globalThis.__mdkrControllerTest.state().phase") ==
                    "assigned", "leave prompt released the controller before confirmation")
            cdp.evaluate("document.getElementById('leave-confirm').click()")
            left = wait_value(cdp, """(() => ({
              phase:globalThis.__mdkrControllerTest.state().phase,
              title:document.getElementById('error-title').textContent,
              neutral:globalThis.__mdkrControllerTest.state().pad.buttons
            }))()""", lambda value: isinstance(value, dict) and
                value.get("phase") == "error", "confirmed controller leave", args.timeout)
            require(left["title"] == "Controller disconnected" and left["neutral"] == 0,
                    f"confirmed leave lacked truthful neutral recovery: {left}")

            cdp.call("Page.addScriptToEvaluateOnNewDocument", {"source":
                     "globalThis.__mdkrControllerTestConfig.entryMode='embedded';"})
            load_controller(cdp, server.origin + "/controller/#" + secret,
                            "embedded entry", args.timeout)
            embedded_copy = wait_value(cdp, """(() => ({
              phase:globalThis.__mdkrControllerTest?.state().phase,
              heading:document.getElementById('error-title')?.textContent,
              primary:document.getElementById('error-recovery')?.textContent,
              copy:document.getElementById('copy-link')?.textContent,
              copyHidden:document.getElementById('copy-link')?.hidden,
              hash:location.hash}))()""", lambda value: isinstance(value, dict) and
                  value.get("phase") == "error", "embedded-browser recovery",
                  args.timeout)
            require(embedded_copy == {
                        "phase": "error", "heading": "Continue in Safari or Chrome",
                        "primary": "Share private link", "copy": "Copy private link",
                        "copyHidden": False, "hash": ""},
                    f"embedded-browser recovery copy was misleading: {embedded_copy}")
            cdp.evaluate("document.getElementById('error-recovery').click()")
            shared = wait_value(cdp, "globalThis.__sharedControllerLink",
                lambda value: isinstance(value, dict),
                "embedded-browser private-link share", args.timeout)
            require(shared == {"title": "Golden Balloon phone controller",
                        "text": "Open this private controller link in Safari or Chrome.",
                        "url": server.origin +
                            "/controller/#controller-test-capability"},
                    f"private share sheet lost capability context: {shared}")
            cdp.evaluate("document.getElementById('copy-link').click()")
            copied = wait_value(cdp, "globalThis.__copiedControllerLink",
                lambda value: isinstance(value, str) and value.endswith(
                    "/controller/#controller-test-capability"),
                "embedded-browser private-link copy", args.timeout)
            require(copied == server.origin +
                        "/controller/#controller-test-capability",
                    f"copy recovery lost the private link: {copied}")

            cdp.call("Page.addScriptToEvaluateOnNewDocument", {"source":
                     "globalThis.__mdkrControllerTestConfig.entryMode='duplicate';"})
            load_controller(cdp, server.origin + "/controller/#" + secret,
                            "duplicate entry", args.timeout)
            duplicate = wait_value(cdp, """(() => ({
              phase:globalThis.__mdkrControllerTest?.state().phase,
              hash:location.hash,
              invite:globalThis.__mdkrControllerTest?.inviteUrl()}))()""",
                lambda value: isinstance(value, dict) and
                    value.get("phase") == "duplicate", "duplicate-tab recovery",
                    args.timeout)
            require(duplicate == {"phase": "duplicate", "hash": "",
                        "invite": server.origin +
                            "/controller/#controller-test-capability"},
                    f"duplicate-tab reclaim lost its private navigation: {duplicate}")
            cdp.call("Page.addScriptToEvaluateOnNewDocument", {"source":
                     "globalThis.__mdkrControllerTestConfig.entryMode='';"})
            cdp.evaluate("document.getElementById('reclaim-controller').click()")
            reclaimed = wait_value(cdp, """(() => ({
              phase:globalThis.__mdkrControllerTest?.state().phase,
              hash:location.hash,href:location.href}))()""",
                lambda value: isinstance(value, dict) and
                    value.get("phase") == "assigned",
                "duplicate-tab acknowledged takeover", args.timeout)
            require(reclaimed == {"phase": "assigned", "hash": "",
                        "href": server.origin + "/controller/"},
                    f"duplicate-tab takeover did not scrub/redeem once: {reclaimed}")

            cdp.call("Page.addScriptToEvaluateOnNewDocument", {"source":
                     "globalThis.__mdkrControllerTestConfig.forceFallbackLease=true;"})
            load_controller(cdp, server.origin + "/controller/#" + secret,
                            "fallback-lease entry", args.timeout)
            fallback_lease = wait_value(cdp, """(() => ({
              phase:globalThis.__mdkrControllerTest?.state().phase,
              mode:globalThis.__mdkrControllerTestState?.leaseMode,
              keys:Object.keys(localStorage).filter(
                key=>key==='gb-controller-tab-lease'),
              storage:Object.entries(localStorage).map(([key,value])=>key+value)
                .join('|')}))()""", lambda value: isinstance(value, dict) and
                  value.get("phase") == "assigned",
                  "no-Web-Locks controller lease", args.timeout)
            require(fallback_lease["mode"] == "broadcast-storage" and
                    len(fallback_lease["keys"]) == 1 and
                    "controller-test-capability" not in fallback_lease["storage"],
                    f"fallback lease exposed a secret or was not acquired: "
                    f"{fallback_lease}")

            cdp.call("Page.addScriptToEvaluateOnNewDocument", {"source": """
              globalThis.__mdkrControllerTestConfig.entryMode='';
              globalThis.__mdkrControllerTestConfig.codeMode=true;
              Object.defineProperty(History.prototype, 'replaceState', {
                configurable:true,
                value(){throw new DOMException('fixture denial', 'SecurityError');}
              });
            """})
            load_controller(cdp, server.origin + "/controller/#" + secret,
                            "scrub-denial entry", args.timeout)
            scrub_denied = wait_value(cdp, """(() => ({
              phase:globalThis.__mdkrControllerTest?.state().phase,
              hash:location.hash,href:location.href,
              leases:Object.keys(localStorage).filter(
                key=>key==='gb-controller-tab-lease').length}))()""",
                lambda value: isinstance(value, dict) and
                    value.get("phase") == "code" and value.get("hash") == "",
                "controller History API denial", args.timeout)
            require(scrub_denied["href"] == server.origin + "/controller/",
                    f"scrub denial retained or redeemed the capability: {scrub_denied}")
            require(scrub_denied["leases"] == 0,
                    f"pagehide retained the fallback tab lease: {scrub_denied}")
            public_recovery = cdp.evaluate("""(() => {
              globalThis.__copiedControllerLink='';
              globalThis.__sharedControllerLink=null;
              globalThis.__mdkrControllerTest.showEmbedded();
              const copy=document.getElementById('copy-link');
              const result={primary:document.getElementById('error-recovery').textContent,
                copy:copy.textContent,copyHidden:copy.hidden};
              copy.click();
              return result;
            })()""")
            require(public_recovery == {"primary": "Share controller page",
                        "copy": "Copy controller page", "copyHidden": False},
                    f"capability-free browser recovery was misleading: "
                    f"{public_recovery}")
            cdp.evaluate("document.getElementById('error-recovery').click()")
            public_shared = wait_value(cdp, "globalThis.__sharedControllerLink",
                lambda value: isinstance(value, dict),
                "capability-free controller page share", args.timeout)
            require(public_shared == {
                        "title": "Golden Balloon phone controller",
                        "text": "Open this controller page in Safari or Chrome, then enter the current room code.",
                        "url": server.origin + "/controller/"},
                    f"public share sheet implied a retained invitation: {public_shared}")
            wait_value(cdp, "globalThis.__copiedControllerLink",
                       lambda value: value == server.origin + "/controller/",
                       "capability-free controller page copy", args.timeout)

            cdp.evaluate("""(() => {
              dispatchEvent(new PageTransitionEvent('pagehide', {persisted:true}));
              dispatchEvent(new PageTransitionEvent('pageshow', {persisted:true}));
            })()""")
            restored = wait_value(cdp, """(() => ({
              phase:globalThis.__mdkrControllerTest?.state().phase,
              hash:location.hash,href:location.href,
              leases:Object.keys(localStorage).filter(
                key=>key==='gb-controller-tab-lease').length}))()""",
                lambda value: isinstance(value, dict) and
                    value.get("phase") == "code",
                "controller BFCache recovery reload", args.timeout)
            require(restored == {"phase": "code", "hash": "",
                        "href": server.origin + "/controller/", "leases": 0},
                    f"BFCache restore revived dead controller custody: {restored}")

            # F3: the pre-channel "Checking connection…" dead end is gone. A
            # press before the direct control channel exists narrates the wait
            # and resolves, within the bounded window, to a concrete retry.
            cdp.call("Page.addScriptToEvaluateOnNewDocument", {"source": """
              globalThis.__mdkrControllerTestConfig={directSignaling:true,seat:2,
                connectionSequence:1,capability:'controller-test-capability',
                request:async()=>({roomId:'abcdefghijklmnopqrstuv',
                  controllerId:'phone-two',credential:'C'.repeat(43),protocol:2,
                  hostPublicKey:'K'.repeat(87)})};
            """})
            load_controller(cdp, server.origin + "/controller/",
                            "pre-channel entry", args.timeout)
            wait_value(cdp, "globalThis.__mdkrControllerTest?.state().phase",
                       lambda value: value == "assigned",
                       "pre-channel assigned state", args.timeout)
            cdp.evaluate("document.getElementById('input-test').click()")
            pressed = cdp.evaluate(
                "document.getElementById('input-test-status').textContent")
            require(pressed == "Still connecting to the display…",
                    f"pre-channel press was not narrated honestly: {pressed!r}")
            wait_value(cdp,
                       "document.getElementById('input-test-status').textContent",
                       lambda value: value == "Not tested yet. Press Go to try again.",
                       "pre-channel test resolved to a retry", args.timeout)
            require(cdp.evaluate(
                        "globalThis.__mdkrControllerTest.state().phase") == "assigned" and
                    cdp.evaluate(
                        "document.getElementById('use-controller').disabled") is True,
                    "an unanswered input test unlocked the controller")
            # An ack that lands AFTER the bounded window must unlock the
            # manual path but never auto-advance: the person left the auto
            # flow's window, so the tap is theirs to make.
            late = cdp.evaluate("""(() => {
              globalThis.__mdkrControllerTest.passInputTest();
              return {phase: globalThis.__mdkrControllerTest.state().phase,
                unlocked: !document.getElementById('use-controller').disabled,
                status: document.getElementById('input-test-status').textContent};
            })()""")
            require(late == {"phase": "assigned", "unlocked": True,
                             "status": "Connection works"},
                    f"a late pass must unlock without auto-advancing: {late}")

            # Item 1 (Android immersive): the "Use controller" gesture requests
            # fullscreen + landscape lock, both feature-detected; a surface
            # entered without a gesture (F3 auto-advance) defers the request to
            # the first controller-surface touch (the same hook the wake-lock
            # retry uses); leaving the surface releases both. Real fullscreen is
            # stubbed to recorders so the headless run proves the calls are made
            # and guarded without actually entering fullscreen. Behavior on a
            # real Android phone is a human RC item; here we pin that the calls
            # are made, are guarded, defer correctly, and are released.
            immersive_stub = """
              globalThis.__mdkrControllerTestConfig={seat:2,autoApprove:true,
                capability:'controller-test-capability'};
              globalThis.__immersiveCalls=[];
              let __fsEl=null;
              Object.defineProperty(document,'fullscreenElement',
                {configurable:true,get:()=>__fsEl});
              Object.defineProperty(Element.prototype,'requestFullscreen',
                {configurable:true,value:function(){__fsEl=this;
                  globalThis.__immersiveCalls.push('requestFullscreen');
                  return Promise.resolve();}});
              Object.defineProperty(document,'exitFullscreen',
                {configurable:true,value:function(){__fsEl=null;
                  globalThis.__immersiveCalls.push('exitFullscreen');
                  return Promise.resolve();}});
              try{Object.defineProperty(screen.orientation,'lock',
                {configurable:true,value:function(o){
                  globalThis.__immersiveCalls.push('lock:'+o);
                  return Promise.resolve();}});}catch(e){}
              try{Object.defineProperty(screen.orientation,'unlock',
                {configurable:true,value:function(){
                  globalThis.__immersiveCalls.push('unlock');}});}catch(e){}
            """
            cdp.call("Page.addScriptToEvaluateOnNewDocument",
                     {"source": immersive_stub})
            load_controller(cdp, server.origin + "/controller/#" + secret,
                            "immersive gesture entry", args.timeout)
            wait_value(cdp, "globalThis.__mdkrControllerTest?.state().phase",
                       lambda value: value == "assigned",
                       "immersive assigned state", args.timeout)
            cdp.evaluate('document.getElementById("input-test").click()')
            wait_value(cdp, "!document.getElementById('use-controller').disabled",
                       bool, "immersive input test", args.timeout)
            cdp.evaluate('document.getElementById("use-controller").click()')
            immersive_enter = wait_value(cdp, """(() => ({
              phase:globalThis.__mdkrControllerTest.state().phase,
              calls:globalThis.__immersiveCalls.slice(),
              record:globalThis.__mdkrControllerTestState.immersive.slice()
            }))()""", lambda value: isinstance(value, dict) and
                "lock:landscape" in value.get("calls", []),
                "immersive enter on gesture", args.timeout)
            require(immersive_enter["phase"] == "controller" and
                    immersive_enter["calls"][:2] ==
                        ["requestFullscreen", "lock:landscape"] and
                    immersive_enter["record"][-1] ==
                        {"action": "enter", "fullscreen": True, "lock": True},
                    f"gesture did not request fullscreen+lock: {immersive_enter}")
            cdp.evaluate("globalThis.__mdkrControllerTest.reject('left_room')")
            immersive_exit = wait_value(cdp, """(() => ({
              calls:globalThis.__immersiveCalls.slice(),
              record:globalThis.__mdkrControllerTestState.immersive.slice()
            }))()""", lambda value: isinstance(value, dict) and
                "unlock" in value.get("calls", []),
                "immersive release on leave", args.timeout)
            require("exitFullscreen" in immersive_exit["calls"] and
                    immersive_exit["record"][-1] == {"action": "exit"},
                    f"leaving the surface did not release immersive: {immersive_exit}")

            # Auto-advance path: entering the controller surface programmatically
            # (no user activation) must NOT request immersive until the first
            # real controller-surface touch, reusing the same pointer hook.
            load_controller(cdp, server.origin + "/controller/#" + secret,
                            "immersive auto-advance entry", args.timeout)
            wait_value(cdp, "globalThis.__mdkrControllerTest?.state().phase",
                       lambda value: value == "assigned",
                       "immersive auto-advance assigned", args.timeout)
            cdp.evaluate('document.getElementById("input-test").click()')
            wait_value(cdp, "!document.getElementById('use-controller').disabled",
                       bool, "immersive auto-advance input test", args.timeout)
            deferred = cdp.evaluate("""(() => {
              globalThis.__mdkrControllerTest.useController();
              return {phase:globalThis.__mdkrControllerTest.state().phase,
                calls:globalThis.__immersiveCalls.slice(),
                record:globalThis.__mdkrControllerTestState.immersive.slice()};
            })()""")
            require(deferred["phase"] == "controller" and deferred["calls"] == [] and
                    deferred["record"] == [],
                    f"auto-advance requested immersive without a gesture: {deferred}")
            cdp.evaluate("""document.getElementById('state-controller').dispatchEvent(
              new PointerEvent('pointerdown', {pointerId:91, pointerType:'touch',
                bubbles:true, cancelable:true}))""")
            # The landscape lock is requested from the resolved-fullscreen
            # microtask, so wait for it rather than sampling synchronously.
            entered = wait_value(cdp, "globalThis.__immersiveCalls.slice()",
                lambda value: isinstance(value, list) and
                    "lock:landscape" in value,
                "auto-advance first touch enters immersive", args.timeout)
            require(entered[:2] == ["requestFullscreen", "lock:landscape"],
                    f"first controller-surface touch did not enter immersive: {entered}")

            # Unsupported (iOS Safari): no Element.requestFullscreen, no
            # orientation.lock — the gesture must degrade silently, no calls,
            # no throw, controller surface still usable.
            cdp.call("Page.addScriptToEvaluateOnNewDocument", {"source": """
              Object.defineProperty(Element.prototype,'requestFullscreen',
                {configurable:true,value:undefined});
              try{Object.defineProperty(screen.orientation,'lock',
                {configurable:true,value:undefined});}catch(e){}
            """})
            load_controller(cdp, server.origin + "/controller/#" + secret,
                            "immersive unsupported entry", args.timeout)
            wait_value(cdp, "globalThis.__mdkrControllerTest?.state().phase",
                       lambda value: value == "assigned",
                       "immersive unsupported assigned", args.timeout)
            cdp.evaluate('document.getElementById("input-test").click()')
            wait_value(cdp, "!document.getElementById('use-controller').disabled",
                       bool, "immersive unsupported input test", args.timeout)
            cdp.evaluate('document.getElementById("use-controller").click()')
            unsupported = wait_value(cdp, """(() => ({
              phase:globalThis.__mdkrControllerTest.state().phase,
              calls:globalThis.__immersiveCalls.slice(),
              record:globalThis.__mdkrControllerTestState.immersive.slice(),
              overflow:document.documentElement.scrollWidth > innerWidth
            }))()""", lambda value: isinstance(value, dict) and
                value.get("phase") == "controller" and value.get("record"),
                "immersive unsupported degrade", args.timeout)
            require(unsupported["calls"] == [] and not unsupported["overflow"] and
                    unsupported["record"][-1] ==
                        {"action": "enter", "fullscreen": False, "lock": False},
                    f"unsupported immersive did not degrade silently: {unsupported}")

            paths = [request.path for request in server.requests]
            forbidden = ("mdkr64_web", ".wasm", "/rom", "/save", "hero.jpg")
            require(not any(any(word in request for word in forbidden) for request in paths),
                    f"controller requested engine/private assets: {paths}")
            require(not any(secret in request for request in paths),
                    f"HTTP request leaked capability fragment: {paths}")
            require(not cdp.failures, "browser/CDP failures: " + "; ".join(cdp.failures))
            fatal = [line for line in cdp.console if "Uncaught" in line or "TypeError" in line]
            require(not fatal, "controller console errors: " + "; ".join(fatal))
            print(f"check_controller_page: PASS — {critical_bytes // 1024} KiB engine-free page, fragment erased, "
                  "copy/reclaim/scrub-denial recovery, "
                  "approval/test/controller/reconnect/confirmed-leave UX, three-finger chord, neutral safety, "
                  "bounded Arrow/WASD steering, 320×568/200% layout and private-asset boundary")
        finally:
            if cdp is not None:
                cdp.close()
            chrome.close()
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
        print(f"check_controller_page: FAIL — {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
