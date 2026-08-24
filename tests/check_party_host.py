#!/usr/bin/env python3
"""Exercise launcher-owned phone pairing and approval UX in real Chromium."""

from __future__ import annotations

import argparse
import json
import sys
import tempfile
from pathlib import Path

from check_browser_runtime import (
    CDPClient, ChromeProcess, CheckFailure, OverlayServer, find_chrome,
    page_websocket, require, wait_value,
)

ROOT = Path(__file__).resolve().parent.parent


def resolve(value: str) -> Path:
    candidate = Path(value).expanduser()
    if not candidate.is_absolute():
        candidate = ROOT / candidate
    return candidate.resolve()


def run(args: argparse.Namespace) -> None:
    shell = resolve(args.shell_dir)
    for relative in ("index.html", "party/party-host.css", "party/party-host.js",
                     "party/qrcodegen.js"):
        require((shell / relative).is_file(), f"party host artifact missing: {relative}")
    server = OverlayServer(shell, shell)
    server.start()
    with tempfile.TemporaryDirectory(prefix="mdkr64_party_host_") as profile:
        chrome = ChromeProcess(find_chrome(args.chrome), Path(profile),
                               args.chrome_flag, args.verbose)
        cdp: CDPClient | None = None
        try:
            cdp = CDPClient(page_websocket(chrome.wait_port()))
            for domain in ("Page", "Runtime", "Log", "Inspector", "Accessibility"):
                cdp.call(f"{domain}.enable")
            config = {
                "initialRoomState": {"type": "room_state", "transitionId": 1,
                                     "controllers": []},
            }
            source = """
              let partyInviteGeneration = 1;
              let partyTransition = 100;
              globalThis.__mdkrPartyHostTestConfig = {
                initialRoomState: %s,
                async request(path) {
                  if (path === '/api/party/create') return {
                    roomId:'abcdefghijklmnopqrstuv', hostCredential:'H'.repeat(43),
                    fallbackCode:'123456', inviteGeneration:1, inviteExpiresInMs:120000,
                    controllerUrl:location.origin+'/controller/#'+'A'.repeat(43)
                  };
                  if (path.endsWith('/rotate')) {
                    if (globalThis.__partyHoldRotate) {
                      globalThis.__partyRotateStarted=true;
                      await new Promise(resolve=>{globalThis.__partyReleaseRotate=resolve;});
                    }
                    partyInviteGeneration++;
                    const current=globalThis.MDKRPartyHost.state().room;
                    globalThis.MDKRPartyHost.applyRoomState({
                      transitionId:++partyTransition,
                      inviteGeneration:partyInviteGeneration,
                      controllers:current.controllers || []
                    });
                    return {
                      fallbackCode:'654321', inviteGeneration:partyInviteGeneration,
                      inviteExpiresInMs:globalThis.__partyRotateTtl||120000,
                      controllerUrl:location.origin+'/controller/#'+'B'.repeat(43)
                    };
                  }
                  if (path.endsWith('/revoke')) return {
                    ok:true, transitionId:++partyTransition,
                    inviteGeneration:++partyInviteGeneration
                  };
                  return {ok:true};
                }
              };
            """ % json.dumps(config["initialRoomState"], separators=(",", ":"))
            cdp.call("Page.addScriptToEvaluateOnNewDocument", {"source": source})
            cdp.call("Emulation.setDeviceMetricsOverride", {
                "width": 390, "height": 844, "deviceScaleFactor": 2, "mobile": True,
            })
            cdp.call("Page.navigate", {"url": server.origin + "/"})
            wait_value(cdp, "Boolean(globalThis.MDKRPartyHost)", bool,
                       "party host API", args.timeout)
            # publishPartyRomReady() derives the party room's romReady from the
            # same ROM availability that ungates the launcher Play button, so a
            # real ready launcher never has Play disabled. Forcing only the
            # party half would leave Play disabled, and click() on a disabled
            # button dispatches nothing -- Start local game could not hand off.
            cdp.evaluate("globalThis.MDKRPartyHost.setRomReady(true);"
                         "document.getElementById('play').disabled=false;"
                         "globalThis.MDKRPartyHost.open()")
            ready = wait_value(cdp, """(() => ({
              room: !document.getElementById('party-room').hidden,
              code: document.getElementById('party-code').textContent,
              qr: document.getElementById('party-qr').width,
              dialog: document.getElementById('party-dialog').open
            }))()""", lambda value: isinstance(value, dict) and value.get("room"),
                "pairing sheet", args.timeout)
            require(ready["dialog"] and ready["code"] == "123 456" and ready["qr"] > 200,
                    f"pairing invite did not render: {ready}")

            # F11.5: clicking the invite QR opens a full-screen enlarge overlay
            # (re-rendered from the same URL, bigger) so a phone camera can scan
            # from across the room; it dismisses on Escape or a click and never
            # touches the invite/capability. Display-only.
            enlarged = cdp.evaluate("""(() => {
              document.getElementById('party-qr').click();
              const overlay = document.querySelector(
                '[role="dialog"][aria-label="Enlarged controller QR code"]');
              const canvas = overlay && overlay.querySelector('canvas');
              return {opens: globalThis.__mdkrPartyHostTestState.qrOverlayOpens || 0,
                present: Boolean(overlay),
                big: canvas ? canvas.width : 0,
                roomIntact: Boolean(globalThis.MDKRPartyHost.state().room &&
                  globalThis.MDKRPartyHost.state().room.controllerUrl)};
            })()""")
            require(enlarged["opens"] == 1 and enlarged["present"] and
                    enlarged["big"] > ready["qr"] and enlarged["big"] > 400 and
                    enlarged["roomIntact"],
                    f"QR enlarge overlay did not open bigger and non-destructively: "
                    f"{enlarged}")
            cdp.evaluate(
                "dispatchEvent(new KeyboardEvent('keydown',{key:'Escape',bubbles:true}))")
            wait_value(cdp, """Boolean(document.querySelector(
                '[aria-label="Enlarged controller QR code"]'))""",
                lambda value: value is False, "QR overlay closes on Escape",
                args.timeout)
            cdp.evaluate("document.getElementById('party-qr').click()")
            wait_value(cdp, """Boolean(document.querySelector(
                '[aria-label="Enlarged controller QR code"]'))""",
                lambda value: value is True, "QR overlay reopens", args.timeout)
            cdp.evaluate("""document.querySelector(
                '[aria-label="Enlarged controller QR code"]').click()""")
            wait_value(cdp, """Boolean(document.querySelector(
                '[aria-label="Enlarged controller QR code"]'))""",
                lambda value: value is False, "QR overlay closes on click",
                args.timeout)

            sas = cdp.evaluate("""(async () => {
              const host=await MDKRPartySas.createIdentity();
              const phone=await MDKRPartySas.createIdentity();
              const hostFingerprint=MDKRPartySas.sdpFingerprint(
                'a=fingerprint:sha-256 ab:cd:ef:01\\r\\n');
              const transcript={roomId:'abcdefghijklmnopqrstuv',
                hostPublicKey:host.publicKey,controllerPublicKey:phone.publicKey,
                hostFingerprint,
                controllerFingerprint:'sha-256 23:45:67:89'};
              const a=await MDKRPartySas.phrase(host.privateKey,phone.publicKey,transcript);
              const b=await MDKRPartySas.phrase(phone.privateKey,host.publicKey,transcript);
              const refused=await MDKRPartySas.phrase(host.privateKey,phone.publicKey,
                {...transcript,controllerFingerprint:''}).then(()=>false,()=>true);
              return {a,b,refused,hostFingerprint,
                hostLength:host.publicKey.length,phoneLength:phone.publicKey.length};
            })()""", await_promise=True)
            require(sas["a"] == sas["b"] and sas["hostLength"] == 87 and
                    sas["phoneLength"] == 87 and len(sas["a"].split()) == 2 and
                    sas["hostFingerprint"] == "sha-256 AB:CD:EF:01" and
                    sas["refused"] is True,
                    f"ECDH v2 pairing phrase mismatch: {sas}")

            cdp.evaluate("""globalThis.MDKRPartyHost.applyRoomState({
              type:'room_state', transitionId:2, controllers:[{
                controllerId:'phone-one', name:'Sam’s phone',
                phase:'pending', seat:null, leaseGeneration:0, connectionSequence:1
              }]});""")
            pending = wait_value(cdp, """(() => ({
              text: document.getElementById('party-pending-list').textContent,
              buttons: document.querySelectorAll('#party-pending-list button').length,
              approveDisabled: document.querySelector(
                '#party-pending-list .btn-primary').disabled,
              seat: document.querySelector('#party-pending-list select').value
            }))()""", lambda value: isinstance(value, dict) and value.get("buttons") == 2,
                "pending approval", args.timeout)
            # v2 ritual: the phrase binds the direct channel, so a pending
            # phone shows the placeholder and Approve is NOT phrase-gated.
            # F14: the copy mirrors the native pending card (ui_phone_party.cpp
            # drawPending) — same order sentence, same button label.
            require("Sam’s phone" in pending["text"] and
                    "Pick a slot and approve. The pairing phrase to compare "
                    "appears after the phone connects." in pending["text"] and
                    pending["approveDisabled"] is False and
                    pending["seat"] == "2",
                    f"identity/placeholder absent from approval: {pending}")
            approve_label = cdp.evaluate(
                "document.querySelector('#party-pending-list .btn-primary').textContent")
            require(approve_label == "Approve This Phone",
                    f"approve label diverged from the native host: {approve_label!r}")
            cdp.evaluate("document.querySelector('#party-pending-list .btn-primary').click()")
            wait_value(cdp,
                "globalThis.__mdkrPartyHostTestState.requests.some(p=>p.endsWith('/approve'))",
                bool, "approve request", args.timeout)
            approved_request = cdp.evaluate("""(() =>
              globalThis.__mdkrPartyHostTestState.requestDetails.find(
                entry => entry.path.endsWith('/approve')))()""")
            require(approved_request["body"].get("seat") == 2,
                    f"host-selected free seat was not sent: {approved_request}")

            cdp.evaluate("""globalThis.MDKRPartyHost.applyRoomState({
              type:'room_state', transitionId:3, controllers:[{
                controllerId:'phone-one', name:'Sam’s phone',
                phase:'leased', seat:2, leaseGeneration:1, connectionSequence:1
              }]});""")
            seat = wait_value(cdp, """(() => ({
              ready: document.querySelector('[data-seat="2"]').dataset.ready,
              label: document.querySelector('[data-seat="2"] small').textContent,
              startDisabled: document.getElementById('party-start').disabled
            }))()""", lambda value: isinstance(value, dict) and value.get("ready") == "true",
                "approved seat", args.timeout)
            # F2: this lease has never reached Connected, so its tile says
            # connecting — never "reconnecting", which promises a recovery of
            # something that never existed.
            require(not seat["startDisabled"] and seat["label"] == "Phone connecting…",
                    f"approved controller did not enable start: {seat}")

            # F14: with all four slots taken, the pending card names the fix
            # instead of a silently disabled Approve (mirrors the native host).
            cdp.evaluate("""globalThis.MDKRPartyHost.applyRoomState({
              type:'room_state', transitionId:4, controllers:[
                {controllerId:'phone-one', name:'Sam’s phone', phase:'leased',
                 seat:2, leaseGeneration:1, connectionSequence:1},
                {controllerId:'phone-two', name:'B', phase:'approved', seat:1,
                 leaseGeneration:1, connectionSequence:1},
                {controllerId:'phone-three', name:'C', phase:'approved', seat:3,
                 leaseGeneration:1, connectionSequence:1},
                {controllerId:'phone-four', name:'D', phase:'approved', seat:4,
                 leaseGeneration:1, connectionSequence:1},
                {controllerId:'phone-five', name:'Late phone', phase:'pending',
                 seat:null, leaseGeneration:0, connectionSequence:1}
              ]});""")
            slots_full = wait_value(cdp, """(() => ({
              text: document.getElementById('party-pending-list').textContent,
              approveDisabled: document.querySelector(
                '#party-pending-list .btn-primary')?.disabled,
              seatChoice: document.querySelector('#party-pending-list select')?.value
            }))()""", lambda value: isinstance(value, dict) and
                "Late phone" in str(value.get("text")), "full-slot pending card",
                args.timeout)
            require(slots_full["approveDisabled"] is True and
                    not slots_full["seatChoice"] and
                    "All four controller slots are taken. Remove a connected "
                    "phone below to free one." in slots_full["text"],
                    f"full slots did not name the fix: {slots_full}")

            # F2 flow pin: only after the room has said Connected may a later
            # neutral lease read as reconnecting.
            cdp.evaluate("""globalThis.MDKRPartyHost.applyRoomState({
              type:'room_state', transitionId:5, controllers:[{
                controllerId:'phone-one', name:'Sam’s phone',
                controllerPublicKey:'K'.repeat(87),
                phase:'connected', seat:2, leaseGeneration:1, connectionSequence:1
              }]});""")
            cdp.evaluate("""globalThis.MDKRPartyHost.applyRoomState({
              type:'room_state', transitionId:6, controllers:[{
                controllerId:'phone-one', name:'Sam’s phone',
                controllerPublicKey:'K'.repeat(87),
                phase:'leased', seat:2, leaseGeneration:1, connectionSequence:1
              }]});""")
            wait_value(cdp,
                "document.querySelector('[data-seat=\\\"2\\\"] small').textContent",
                lambda value: value == "Phone reconnecting — neutral",
                "dropped lease reads as reconnecting", args.timeout)
            # Connection history binds to id+key, exactly as the native model
            # does: a different phone under a reused id is connecting for the
            # first time, never "reconnecting".
            cdp.evaluate("""globalThis.MDKRPartyHost.applyRoomState({
              type:'room_state', transitionId:7, controllers:[{
                controllerId:'phone-one', name:'Sam’s phone',
                controllerPublicKey:'L'.repeat(87),
                phase:'leased', seat:2, leaseGeneration:1, connectionSequence:1
              }]});""")
            wait_value(cdp,
                "document.querySelector('[data-seat=\\\"2\\\"] small').textContent",
                lambda value: value == "Phone connecting…",
                "key swap resets connection history", args.timeout)

            cdp.evaluate("""(() => {
              globalThis.__partyQrEncode = qrcodegen.QrCode.encodeText;
              qrcodegen.QrCode.encodeText = () => { throw new Error('qr fixture failure'); };
              document.getElementById('party-extend').click();
            })()""")
            qr_fallback = wait_value(cdp, """(() => ({
              code:document.getElementById('party-code').textContent,
              hidden:document.querySelector('.party-qr-wrap').hidden,
              step:document.getElementById('party-scan-step').textContent,
              install:document.getElementById('party-install-copy').textContent
            }))()""", lambda value: isinstance(value, dict) and
                value.get("code") == "654 321", "rotated QR fallback", args.timeout)
            require(qr_fallback["hidden"] and "/controller/" in qr_fallback["install"] and
                    "Open this site" in qr_fallback["step"],
                    f"QR failure did not preserve manual code recovery: {qr_fallback}")
            cdp.evaluate("qrcodegen.QrCode.encodeText = globalThis.__partyQrEncode")
            cdp.call("Emulation.setPageScaleFactor", {"pageScaleFactor": 2})
            layout = cdp.evaluate("({width:document.documentElement.scrollWidth, viewport:innerWidth})")
            require(layout["width"] <= layout["viewport"], f"200% party sheet overflow: {layout}")

            cdp.evaluate("document.getElementById('party-close').click()")
            wait_value(cdp, "!document.getElementById('party-dialog').open", bool,
                       "closed sheet", args.timeout)
            wait_value(cdp,
                "globalThis.__mdkrPartyHostTestState.requests.some(p=>p.endsWith('/revoke'))",
                bool, "dismissed launcher invite revocation", args.timeout)
            revoked_presentation = cdp.evaluate("""(() => ({
              room:globalThis.MDKRPartyHost.state().room,
              code:document.getElementById('party-code').textContent,
              qrWidth:document.getElementById('party-qr').width,
              qrHidden:document.querySelector('.party-qr-wrap').hidden,
              expiry:document.getElementById('party-expiry').textContent
            }))()""")
            require(revoked_presentation["room"] and
                    "fallbackCode" not in revoked_presentation["room"] and
                    "controllerUrl" not in revoked_presentation["room"] and
                    revoked_presentation["code"] == "—— —— ——" and
                    revoked_presentation["qrWidth"] == 1 and
                    revoked_presentation["qrHidden"] and
                    revoked_presentation["expiry"] == "Invite is not displayed",
                    f"dismissed invite remained in launcher/DOM custody: {revoked_presentation}")
            require(cdp.evaluate("Boolean(globalThis.MDKRPartyHost.state().room)"),
                    "closing setup unexpectedly ended approved controller leases")

            cdp.evaluate("""(() => {
              try {
                document.getElementById('stage').hidden=false;
                globalThis.MDKRPartyHost.open();
                globalThis.__partyOpenDebug={ok:true};
              } catch (error) {
                globalThis.__partyOpenDebug={ok:false,error:String(error),stack:error.stack};
              }
            })()""")
            wait_value(cdp, """({
              open:globalThis.__mdkrPartyOverlayOpen,
              stageHidden:document.getElementById('stage').hidden,
              dialog:document.getElementById('party-dialog').open,
              debug:globalThis.__partyOpenDebug,
              ready:globalThis.MDKRPartyHost.state().romReady,
              room:Boolean(globalThis.MDKRPartyHost.state().room),
              api:String(globalThis.MDKRPartyHost.open),
              triggerDisabled:document.getElementById('add-phone-controllers').disabled,
              lifecycle:globalThis.__mdkrPartyHostTestState.lifecycle.slice()
            })""", lambda value: isinstance(value, dict) and value.get("open") is True,
                       "in-game local pause latch", args.timeout)
            wait_value(cdp, "Boolean(globalThis.MDKRPartyHost.state().room)", bool,
                       "in-game controller room", args.timeout)
            wait_value(cdp,
                "globalThis.MDKRPartyHost.state().room.inviteGeneration >= 4",
                bool, "reopened invite generation after revoke", args.timeout)
            cdp.evaluate("document.getElementById('party-close').click()")
            wait_value(cdp, "!globalThis.__mdkrPartyOverlayOpen", bool,
                       "in-game local pause release", args.timeout)
            wait_value(cdp,
                "globalThis.__mdkrPartyHostTestState.requests.some(p=>p.endsWith('/revoke'))",
                bool, "dismissed invite revocation", args.timeout)
            preserved = cdp.evaluate("Boolean(globalThis.MDKRPartyHost.state().room)")
            require(preserved, "in-game dismissal destroyed the controller room")

            cdp.evaluate("""(() => {
              document.getElementById('stage').hidden=true;
              globalThis.MDKRPartyHost.open();
            })()""")
            wait_value(cdp, "document.getElementById('party-dialog').open", bool,
                       "room reopened for launcher start", args.timeout)
            wait_value(cdp,
                "globalThis.MDKRPartyHost.state().room.inviteGeneration >= 6",
                bool, "second reopen correlated after revoke", args.timeout)
            cdp.evaluate("""(() => {
              globalThis.__partyPlayClicked=false;
              document.getElementById('play').addEventListener('click', event => {
                event.preventDefault();
                event.stopImmediatePropagation();
                globalThis.__partyPlayClicked=true;
              }, {capture:true, once:true});
              document.getElementById('party-start').click();
            })()""")
            started = wait_value(cdp, """(() => ({
              played:globalThis.__partyPlayClicked,
              dialog:document.getElementById('party-dialog').open,
              room:globalThis.MDKRPartyHost.state().room,
              code:document.getElementById('party-code').textContent,
              qrHidden:document.querySelector('.party-qr-wrap').hidden,
              revokes:globalThis.__mdkrPartyHostTestState.requests
                .filter(path=>path.endsWith('/revoke')).length
            }))()""", lambda value: isinstance(value, dict) and
                value.get("played") is True and value.get("dialog") is False and
                value.get("revokes", 0) >= 3,
                "launcher start invite revocation", args.timeout)
            require(started["room"] and "fallbackCode" not in started["room"] and
                    "controllerUrl" not in started["room"] and
                    started["code"] == "—— —— ——" and started["qrHidden"],
                    f"starting local play retained invite custody: {started}")

            cdp.evaluate("""(() => {
              document.getElementById('stage').hidden=false;
              globalThis.MDKRPartyHost.open();
            })()""")
            wait_value(cdp, "document.getElementById('party-dialog').open", bool,
                       "room reopened for explicit end", args.timeout)
            wait_value(cdp,
                "globalThis.MDKRPartyHost.state().room.inviteGeneration >= 8",
                bool, "post-start reopen correlated after revoke", args.timeout)
            rotate_generations = cdp.evaluate("""globalThis.__mdkrPartyHostTestState
              .requestDetails.filter(entry=>entry.path.endsWith('/rotate'))
              .map(entry=>entry.body.expectedInviteGeneration)""")
            require(rotate_generations == [1, 3, 5, 7],
                    f"rotate requests lost revoke/publication correlation: {rotate_generations}")

            # F6: while the invite card is on screen, the host rotates the
            # invite by itself before its TTL lapses (~75% elapsed), so a
            # displayed QR/code is always redeemable. The TTL itself never
            # lengthens: the fixture mints a 4 s invite and the page must
            # request a fresh /rotate with no click, replacing QR + code +
            # countdown in place instead of ever showing "expired".
            cdp.evaluate("globalThis.__partyRotateTtl=4000")
            rotates_before = cdp.evaluate("""globalThis.__mdkrPartyHostTestState
              .requests.filter(path=>path.endsWith('/rotate')).length""")
            cdp.evaluate("document.getElementById('party-extend').click()")
            wait_value(cdp, """globalThis.__mdkrPartyHostTestState
              .requests.filter(path=>path.endsWith('/rotate')).length""",
                lambda value: value == rotates_before + 1,
                "manual short-TTL rotation", args.timeout)
            wait_value(cdp, """globalThis.__mdkrPartyHostTestState
              .requests.filter(path=>path.endsWith('/rotate')).length""",
                lambda value: isinstance(value, int) and value >= rotates_before + 2,
                "displayed invite auto-rotated before its TTL lapsed",
                args.timeout)
            auto_rotated = cdp.evaluate("""(() => ({
              code:document.getElementById('party-code').textContent,
              expiry:document.getElementById('party-expiry').textContent
            }))()""")
            require(auto_rotated["code"] == "654 321" and
                    "expire" not in auto_rotated["expiry"].lower().replace(
                        "invite expires in", ""),
                    f"auto-rotation did not keep the displayed invite live: "
                    f"{auto_rotated}")
            cdp.evaluate("globalThis.__partyRotateTtl=0;"
                         "document.getElementById('party-extend').click()")
            wait_value(cdp, """globalThis.__mdkrPartyHostTestState
              .requestDetails.filter(entry=>entry.path.endsWith('/rotate'))
              .length >= 1 &&
              globalThis.MDKRPartyHost.state().room.inviteExpiresInMs === 120000""",
                bool, "invite restored to the full TTL", args.timeout)

            # F11.5 x F6: an enlarge overlay left open (item 3's whole purpose --
            # scan from across the room) must stay live when the invite rotates.
            # renderInvite re-renders the OPEN overlay canvas to the current url;
            # without that fix the giant QR shows the invalidated code while the
            # page announces the previous one expired. Proven by the overlay's
            # per-render counter incrementing and its encoded url tracking the
            # room's live controllerUrl across a rotation.
            cdp.evaluate("document.getElementById('party-qr').click()")
            wait_value(cdp, """(() => {
              const o=document.querySelector(
                '[aria-label="Enlarged controller QR code"]');
              const c=o&&o.querySelector('canvas');
              return c?c.dataset.qrRenders:null;
            })()""", lambda value: value == "1",
                "enlarge overlay open before rotation", args.timeout)
            cdp.evaluate("document.getElementById('party-extend').click()")
            rotated_overlay = wait_value(cdp, """(() => {
              const o=document.querySelector(
                '[aria-label="Enlarged controller QR code"]');
              const c=o&&o.querySelector('canvas');
              return {present:Boolean(o),
                renders:c?c.dataset.qrRenders:null,
                url:c?c.dataset.qrUrl:null,
                roomUrl:globalThis.MDKRPartyHost.state().room?.controllerUrl||null};
            })()""", lambda value: isinstance(value, dict) and
                value.get("renders") == "2",
                "enlarge overlay re-rendered on invite rotation", args.timeout)
            require(rotated_overlay["present"] and
                    rotated_overlay["url"] == rotated_overlay["roomUrl"] and
                    rotated_overlay["url"],
                    f"enlarge overlay went stale on invite rotation: "
                    f"{rotated_overlay}")
            cdp.evaluate(
                "dispatchEvent(new KeyboardEvent('keydown',{key:'Escape',bubbles:true}))")
            wait_value(cdp, """Boolean(document.querySelector(
                '[aria-label="Enlarged controller QR code"]'))""",
                lambda value: value is False,
                "enlarge overlay closed after rotation test", args.timeout)

            removal_requests_before = cdp.evaluate("""globalThis.__mdkrPartyHostTestState
              .requests.filter(path=>path.endsWith('/remove')).length""")
            cdp.evaluate("document.querySelector('[data-seat=\"2\"] .party-seat-remove').click()")
            removal_confirmation = wait_value(cdp, """(() => ({
              open:document.getElementById('party-remove-dialog').open,
              focus:document.activeElement?.id,
              copy:document.getElementById('party-remove-copy').textContent,
              requests:globalThis.__mdkrPartyHostTestState.requests
                .filter(path=>path.endsWith('/remove')).length
            }))()""", lambda value: isinstance(value, dict) and
                value.get("open") is True, "remove-phone confirmation", args.timeout)
            require(removal_confirmation["focus"] == "party-remove-cancel" and
                    "Sam’s phone" in removal_confirmation["copy"] and
                    "Controller 2" in removal_confirmation["copy"] and
                    removal_confirmation["requests"] == removal_requests_before,
                    f"phone removal was not safe and specific: {removal_confirmation}")
            cdp.evaluate("document.getElementById('party-remove-cancel').click()")
            wait_value(cdp, "!document.getElementById('party-remove-dialog').open", bool,
                       "cancelled phone removal", args.timeout)
            require(cdp.evaluate("""globalThis.__mdkrPartyHostTestState.requests
                    .filter(path=>path.endsWith('/remove')).length""") ==
                    removal_requests_before,
                    "cancelling phone removal mutated the room")
            # The dialog's `close` event, and therefore the focus return, is
            # queued after `open` flips to false. Wait for the same condition
            # instead of sampling the one task in between.
            wait_value(cdp,
                       "document.activeElement?.classList.contains('party-seat-remove')",
                       lambda value: value is True,
                       "cancelled phone removal restored focus", args.timeout)

            cdp.evaluate("""(() => {
              document.querySelector('[data-seat="2"] .party-seat-remove').click();
              document.getElementById('party-remove-confirm').click();
            })()""")
            wait_value(cdp, """globalThis.__mdkrPartyHostTestState.requests
              .filter(path=>path.endsWith('/remove')).length""",
              lambda value: value == removal_requests_before + 1,
              "confirmed phone removal", args.timeout)
            cdp.evaluate("""globalThis.MDKRPartyHost.applyRoomState({
              type:'room_state', transitionId:++partyTransition,
              inviteExpiresAt:Date.now()+120000,
              inviteGeneration:partyInviteGeneration, phase:'open', controllers:[]
            })""")
            removed_seat = wait_value(cdp, """(() => ({
              hidden:document.querySelector('[data-seat="2"] .party-seat-remove').hidden,
              label:document.querySelector('[data-seat="2"] small').textContent,
              focusSeat:document.activeElement?.dataset?.seat
            }))()""", lambda value: isinstance(value, dict) and
                value.get("hidden") is True, "removed phone seat", args.timeout)
            require(removed_seat["label"] == "Available" and
                    removed_seat["focusSeat"] == "2",
                    f"confirmed removal did not neutralize/focus safely: {removed_seat}")

            cdp.evaluate("""(() => {
              globalThis.__partyHoldRotate=true;
              globalThis.__partyRotateStarted=false;
              document.getElementById('party-extend').click();
            })()""")
            wait_value(cdp, "globalThis.__partyRotateStarted", bool,
                       "page-bound invite rotation", args.timeout)
            bfcache = cdp.evaluate("""(async () => {
              dispatchEvent(new PageTransitionEvent('pagehide', {persisted:true}));
              globalThis.__partyHoldRotate=false;
              globalThis.__partyReleaseRotate();
              await new Promise(resolve=>setTimeout(resolve, 0));
              const hidden={room:globalThis.MDKRPartyHost.state().room,
                code:document.getElementById('party-code').textContent,
                qr:document.getElementById('party-qr').width,
                pads:globalThis.MDKRPartyHost.remotePads().map(p=>({
                  active:p.active,reserved:p.reserved,packets:p.packets.length}))};
              dispatchEvent(new PageTransitionEvent('pageshow', {persisted:true}));
              return {hidden,lifecycle:
                globalThis.__mdkrPartyHostTestState.lifecycle.slice(-2)};
            })()""", await_promise=True)
            require("fallbackCode" not in bfcache["hidden"]["room"] and
                    "controllerUrl" not in bfcache["hidden"]["room"] and
                    bfcache["hidden"]["code"] == "—— —— ——" and
                    bfcache["hidden"]["qr"] == 1 and
                    not any(pad["active"] or pad["reserved"] or pad["packets"]
                            for pad in bfcache["hidden"]["pads"]) and
                    bfcache["lifecycle"] == ["pagehide", "pageshow:persisted"],
                    f"BFCache lifecycle revived invite/input custody: {bfcache}")

            # Item 4: a phone that redeems and appears as a NEW pending approval
            # plays a one-shot join-request cue (WebAudio) so a host looking away
            # notices. Fires once per new pending controller, never on approval
            # or a repeat render, and stays silent when muted (gb-party-ding=0).
            ding_base = cdp.evaluate(
                "Number(globalThis.MDKRPartyHost.state().room.transitionId)||0")
            dings0 = cdp.evaluate(
                "globalThis.__mdkrPartyHostTestState.joinDings || 0")
            def apply_ding_state(step, controllers):
                cdp.evaluate(
                    "globalThis.MDKRPartyHost.applyRoomState({type:'room_state',"
                    f"transitionId:{ding_base + step}, controllers:" +
                    json.dumps(controllers, separators=(",", ":")) + "})")
            waiting = {"controllerId": "phone-ding-1", "name": "Late arrival",
                       "phase": "pending", "seat": None, "leaseGeneration": 0,
                       "connectionSequence": 1}
            second = {"controllerId": "phone-ding-2", "name": "Another phone",
                      "phase": "pending", "seat": None, "leaseGeneration": 0,
                      "connectionSequence": 1}
            apply_ding_state(1, [waiting])
            wait_value(cdp, "globalThis.__mdkrPartyHostTestState.joinDings || 0",
                lambda value: value == dings0 + 1,
                "join-request ding on new pending", args.timeout)
            # A repeat render of the same pending phone must not re-ding.
            apply_ding_state(2, [waiting])
            # A second, different new pending phone dings exactly once more.
            apply_ding_state(3, [waiting, second])
            wait_value(cdp, "globalThis.__mdkrPartyHostTestState.joinDings || 0",
                lambda value: value == dings0 + 2,
                "second new pending dings once", args.timeout)
            # Approving a pending phone must NOT ding.
            apply_ding_state(4, [{**waiting, "phase": "approved", "seat": 1,
                                  "leaseGeneration": 1}, second])
            # Muted: a new pending while gb-party-ding is "0" stays silent.
            cdp.evaluate("localStorage.setItem('gb-party-ding','0')")
            apply_ding_state(5, [second, {"controllerId": "phone-ding-3",
                "name": "Muted phone", "phase": "pending", "seat": None,
                "leaseGeneration": 0, "connectionSequence": 1}])
            dings_after = cdp.evaluate(
                "globalThis.__mdkrPartyHostTestState.joinDings || 0")
            require(dings_after == dings0 + 2,
                    f"join ding fired on approval or while muted: {dings_after}")
            cdp.evaluate("localStorage.removeItem('gb-party-ding')")

            # S5 (F6): a routine RTT pong from an already-connected phone must
            # never rebuild the pending-approval cards. Before the fix,
            # renderRoomState ran on every 5-second pong and replaceChildren'd
            # the roster, destroying the slot picker's selection AND focus
            # exactly while the host was approving a phone. testControlPong
            # stamps an outstanding ping and runs the exact production pong
            # handler; the pending card's DOM node, its chosen slot and the
            # host's focus must all survive, while the pong'd seat tile shows
            # the fresh RTT sample.
            pong_base = cdp.evaluate(
                "Number(globalThis.MDKRPartyHost.state().room.transitionId)||0")
            cdp.evaluate("""globalThis.MDKRPartyHost.applyRoomState({
              type:'room_state', transitionId:%d, controllers:[
                {controllerId:'phone-live-rtt', name:'Connected phone',
                 controllerPublicKey:'%s', phase:'connected', seat:1,
                 leaseGeneration:1, connectionSequence:1},
                {controllerId:'phone-waiting-rtt', name:'Waiting phone',
                 phase:'pending', seat:null, leaseGeneration:0,
                 connectionSequence:1}
              ]});
              globalThis.MDKRPartyHost.receiveSignal({type:'controller_hello',
                controllerId:'phone-live-rtt'});""" % (pong_base + 1, "M" * 87))
            wait_value(cdp,
                "globalThis.MDKRPartyHost.testControlPong('phone-live-rtt', 23)",
                bool, "live peer for the RTT pong", args.timeout)
            cdp.evaluate("""(() => {
              const select = document.querySelector('#party-pending-list select');
              select.focus();
              select.value = '3';
              globalThis.__partyPongSelect = select;
              globalThis.MDKRPartyHost.remotePads()[0].active = true;
            })()""")
            pong = cdp.evaluate("""(() => {
              const before = document.querySelector('#party-pending-list select');
              const ok = globalThis.MDKRPartyHost.testControlPong(
                'phone-live-rtt', 23);
              const after = document.querySelector('#party-pending-list select');
              return {ok,
                sameNode: after === globalThis.__partyPongSelect &&
                  after === before,
                value: after ? after.value : null,
                focused: document.activeElement === after,
                seatText: document.querySelector(
                  '[data-seat="1"] small').textContent};
            })()""")
            require(pong["ok"] is True and pong["sameNode"] is True and
                    pong["value"] == "3" and pong["focused"] is True and
                    " ms · direct" in pong["seatText"],
                    f"RTT pong rebuilt the pending card mid-approve: {pong}")

            cdp.evaluate("document.getElementById('party-end').click()")
            confirmation = wait_value(cdp, """(() => ({
              open:document.getElementById('party-end-dialog').open,
              room:Boolean(globalThis.MDKRPartyHost.state().room),
              closeSent:globalThis.__mdkrPartyHostTestState.requests
                .some(path=>path.endsWith('/close')),
              focus:document.activeElement?.id
            }))()""", lambda value: isinstance(value, dict) and
                value.get("open") is True, "end-room confirmation", args.timeout)
            require(confirmation["room"] and not confirmation["closeSent"] and
                    confirmation["focus"] == "party-end-cancel",
                    f"end-room confirmation was not safe by default: {confirmation}")
            cdp.evaluate("document.getElementById('party-end-cancel').click()")
            wait_value(cdp, "!document.getElementById('party-end-dialog').open", bool,
                       "cancelled end-room confirmation", args.timeout)
            require(cdp.evaluate("Boolean(globalThis.MDKRPartyHost.state().room)"),
                    "cancelling end-room confirmation destroyed the room")
            cdp.evaluate("""(() => {
              document.getElementById('party-end').click();
              document.getElementById('party-end-confirm').click();
            })()""")
            wait_value(cdp,
                "globalThis.__mdkrPartyHostTestState.requests.some(p=>p.endsWith('/close'))",
                bool, "explicit room close request", args.timeout)
            require(not cdp.evaluate("Boolean(globalThis.MDKRPartyHost.state().room)"),
                    "End controller room did not clear the room")
            require(not cdp.failures, "browser/CDP failures: " + "; ".join(cdp.failures))
            fatal = [line for line in cdp.console if "Uncaught" in line or "TypeError" in line]
            require(not fatal, "party host console errors: " + "; ".join(fatal))
            print("check_party_host: PASS — mixed-source seats, safe phone removal, QR/code "
                  "fallback, dismiss/start revoke/preserve, pong-stable pending cards, "
                  "confirmed close and 200% layout")
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
        print(f"check_party_host: FAIL — {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
