#!/usr/bin/env python3
"""Prove the modal online-lobby takeover suppresses the offline launcher shell.

The launcher is a nav-rail + top-tabs + panel-router shell whose generic Play
button launches the OFFLINE game. While a live online session is active that
button is one tab away and always visible, so the originator could launch the
offline game "over" the room and a joiner could beach-ball the process by
launching offline into a live WebRTC session. The fix is a modal takeover: once
a session progresses past the entry/chooser the launcher renders ONLY the lobby
and the nav rail, top tabs and generic Play are suppressed, making the offline
launch unreachable.

This gate drives the deterministic fake adapter (no network) into each active
view kind through the ordinary launcher smoke loop and reads the per-frame
[app-lobby-takeover] probe. The invariant it asserts:

    whenever the online session is active at frame start, the generic offline
    Play button and the nav rail are NOT drawn, and the takeover branch ran.

It FAILS RED on the pre-takeover launcher (the shell, and thus Play, is drawn
while online is active) and PASSES GREEN once the takeover is wired. The
"entry" chooser is checked in the opposite direction: the shell must remain so a
player can still choose Create/Join.
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import tempfile
from pathlib import Path

PROBE_RE = re.compile(
    r"\[app-lobby-takeover\] frame=(?P<frame>\d+) "
    r"online_active=(?P<online>[01]) took_takeover=(?P<took>[01]) "
    r"play_drawn=(?P<play>[01]) nav_drawn=(?P<nav>[01]) "
    r"view_kind=(?P<kind>\d+)"
)

# Active view kinds that must engage the takeover (fake gallery slugs).
#
# The two SELECTING-kind slugs (select-character, select-start) are intentionally
# absent: the launcher's per-race SELECTING selection surface was retired
# (the ImGui character/vehicle/track combos + the ImGui-initiated Start Race), so
# that legacy preview panel no longer exists to take over. Independently, the beta
# build now arms a never-silent selection-stall view-timeout on every SELECTING
# view (beta-gated, lobby_view_model.c), which makes the SELECTING-kind gallery
# specs (timeout_present=false) unbuildable in a beta build regardless of race
# admission -- so those arms could never reach an active state here anyway. The
# takeover invariant stays covered across the ROOM/PREFLIGHT/LOADING/COUNTDOWN/
# RACING/RESULTS/RECOVERY kinds below.
ACTIVE_SLUGS = (
    "room-friends",
    "preflight",
    "loading",
    "countdown",
    "racing-direct",
    "results",
    "failure-service-unavailable",
)
# The create/join chooser must KEEP the shell so a player can still pick a path.
ENTRY_SLUG = "entry"
ADMISSION_SLUGS = frozenset({"loading", "countdown", "racing-direct", "results"})


class TakeoverError(RuntimeError):
    pass


def check_source_contract() -> None:
    """Bind the policy fixture to production ownership and deferred teardown.

    These structural assertions supplement, not replace, rendered invalid-view
    and Leave Room regression coverage on an authorized executable lane.
    """
    source_root = Path(__file__).resolve().parents[1]
    room = (source_root / "platform/app/ui_online_room.cpp").read_text()
    launcher = (source_root / "platform/app/ui_launcher.cpp").read_text()
    tracker = (source_root / "platform/app/online_teardown_tracker.h").read_text()
    cleanup = (source_root / "platform/app/app_cleanup_completion.h").read_text()
    party = (source_root / "platform/party/libdatachannel_party_transport.cpp").read_text()
    party_stub = (source_root / "platform/party/libdatachannel_party_transport_stub.cpp").read_text()
    main_app = (source_root / "platform/app/main_app.cpp").read_text()
    interactive = main_app.split("int runInteractiveLauncher(", 1)[1].split(
        "\n} // namespace", 1
    )[0]
    closing = launcher.split("void Launcher::drawOnlineClosing(bool delayed) {", 1)[1].split(
        "\n}", 1
    )[0]
    quit_readiness = launcher.split("bool Launcher::quitReady() const {", 1)[1].split(
        "\n}", 1
    )[0]
    if "!state_.characterPreviewDispatched" not in quit_readiness or \
            "!Settings_characterWorkPending()" not in quit_readiness:
        raise TakeoverError("quit readiness must await preview publication and transactions")
    service = launcher.split("void Launcher::serviceCharacterWork() {", 1)[1].split(
        "\n}\n", 1
    )[0]
    if "Settings_publishCharacterPreviewResult(" not in service or "ImGui::" in service:
        raise TakeoverError("preview publication must not require an ImGui frame")
    owners = launcher.split("struct LauncherNetworkShutdown {", 1)[1].split("\n};", 1)[0]
    if owners.index("std::unique_ptr<MdkrPartyTransport> transport;") >= \
            owners.index("std::unique_ptr<MdkrNativePartyHost> host;"):
        raise TakeoverError("retiring phone host must be destroyed before its borrowed transport")
    begin_network = launcher.split("void Launcher::beginNetworkShutdown() {", 1)[1].split("\n}", 1)[0]
    for alias in ("state_.phoneParty = nullptr;", "Overlay_setPhonePartyHost(nullptr);"):
        for retirement in ("phoneParty_.reset();", "owners->host = std::move(phoneParty_);",
                           "shutdown.retiring.retire(std::move(owners))"):
            if begin_network.index(alias) >= begin_network.index(retirement):
                raise TakeoverError("all phone aliases must retract before any owner retirement")
    if begin_network.index("if (shutdown.started) return;") >= \
            begin_network.index("shutdown.started = true;") or \
            begin_network.index("shutdown.started = true;") >= \
            begin_network.index("OnlineRoom_beginAppExit();"):
        raise TakeoverError("all-owner retirement must latch once before room shutdown")
    if begin_network.index("phoneParty_.reset();") >= begin_network.index("partyTransport_.reset();"):
        raise TakeoverError("allocation-failure fallback must preserve host/transport destruction order")
    switch_party = launcher.split("void Launcher::selectPartyTransport(", 1)[1].split("\n}", 1)[0]
    if switch_party.index("if (networkShutdown_->started) return;") >= \
            switch_party.index("mdkr_create_native_party_transport()"):
        raise TakeoverError("global cleanup must prevent creation of replacement phone transports")
    launcher_draw = launcher.split("LauncherAction Launcher::draw(AppHost &host) {", 1)[1].split("\n}", 1)[0]
    if launcher_draw.index("if (networkShutdown_->started) return {};") >= \
            launcher_draw.index("phoneParty_->service("):
        raise TakeoverError("retired phone owners must not be serviced by launcher drawing")
    poll_network = launcher.split("bool Launcher::pollNetworkShutdown() {", 1)[1].split("\n}", 1)[0]
    for required in (
        "if (!shutdown.started) return false;",
        "const bool partyRetired = shutdown.retiring.pollReady();",
        "const bool roomRetired = OnlineRoom_pollAppExit();",
        "const bool roomRetired = true;",
        "const bool resolverRetired = onlineResolverWorkInUse() == 0u;",
        "partyRetired && roomRetired && resolverRetired, mdkr_native_party_cleanup",
        "std::rethrow_exception(shutdown.completion.error());",
    ):
        if required not in poll_network:
            raise TakeoverError("global cleanup lost all-owner/failure contract: " + required)
    global_poll = poll_network.index("shutdown.completion.poll(")
    if poll_network.index("if (shutdown.finished) return true;") >= \
            poll_network.index("shutdown.retiring.pollReady()") or \
            poll_network.index("shutdown.finished = finished;") <= global_poll:
        raise TakeoverError("completed polling must not revisit retired global owners")
    for retirement in ("shutdown.retiring.pollReady()", "OnlineRoom_pollAppExit()",
                       "onlineResolverWorkInUse()"):
        if poll_network.index(retirement) >= global_poll:
            raise TakeoverError("all owner groups and resolver work must be checked before global cleanup polling")
    finish_network = launcher.split("bool Launcher::finishNetworkShutdownForExit() {", 1)[1].split("\n}", 1)[0]
    if finish_network.index("if (networkShutdown_->finished) return !networkShutdownFailed();") >= \
            finish_network.index("beginNetworkShutdown();"):
        raise TakeoverError("destructor backstop must not revisit global owners after terminal cleanup")
    if finish_network.index("beginNetworkShutdown();") >= finish_network.index("OnlineRoom_shutdownForAppExit();") or \
            finish_network.index("retiring.drain(") >= finish_network.index("while (!pollNetworkShutdown())") or \
            "return !networkShutdownFailed();" not in finish_network:
        raise TakeoverError("exceptional network exit must retire, drain and report cleanup failure")
    if ".detach(" in finish_network or ".detach(" in begin_network:
        raise TakeoverError("phone owner retirement cannot detach on exit")
    real_cleanup = party.split("std::shared_future<void> mdkr_native_party_cleanup() {", 1)[1].split("\n}", 1)[0]
    stub_cleanup = party_stub.split("std::shared_future<void> mdkr_native_party_cleanup() {", 1)[1].split("\n}", 1)[0]
    if "return rtc::Cleanup();" not in real_cleanup or "rtc::" in stub_cleanup or \
            "set_value();" not in stub_cleanup:
        raise TakeoverError("global cleanup must observe RTC completion, with a completed dependency-free stub")
    # An application catch cannot handle a thread-launch exception escaping
    # RTC's noexcept final-token destructor. Bind the actual dependency patch
    # to the same reserved worker exercised by the compiled fixture.
    reservation_patch = (source_root / "cmake/patches/libdatachannel-cleanup-worker.patch").read_text()
    post_image = "\n".join(line[1:] for line in reservation_patch.splitlines()
                           if line.startswith(("+", " ")) and not line.startswith("+++"))
    token = post_image.split("struct Init::TokenPayload {", 1)[1].split("\n};", 1)[0]
    token_order = (
        ": cleanupWorker([] { Init::Instance().doCleanup(); })",
        "Init::Instance().doInit();",
        "MdkrReservedCleanupWorker cleanupWorker;",
    )
    positions = [token.index(step) for step in token_order]
    if positions != sorted(positions) or "std::thread" in token or "~TokenPayload" in token:
        raise TakeoverError("RTC token must reserve cleanup before initialization and only signal at destruction")
    transaction_patch = (source_root / "cmake/patches/libdatachannel-initialization.patch").read_text()
    transaction_post = "\n".join(line[1:] for line in transaction_patch.splitlines()
                                 if line.startswith(("+", " ")) and not line.startswith("+++"))
    updated_token = transaction_post.split("\n\t}", 1)[0]
    transaction_order = ("*cleanupFuture = cleanupWorker.future();", "cleanupWorker.arm();",
                         "Init::Instance().doInit();")
    positions = [updated_token.index(step) for step in transaction_order]
    if positions != sorted(positions):
        raise TakeoverError("partial RTC initialization must publish and arm rollback before acquisition")
    for binding in ("mInitialization.admit(mCleanupFuture);", "mGlobal = std::move(locked);",
                    "mInitialization.initialize([&]", "mInitialization.retire([&]",
                    "MdkrRtcInitializationTransaction mInitialization;",
                    "#if !USE_MBEDTLS || USE_GNUTLS || USE_NICE || RTC_ENABLE_MEDIA"):
        if binding not in transaction_post:
            raise TakeoverError("RTC transaction lost its production binding: " + binding)
    acquisition_order = ("auto &pool = ThreadPool::Instance();",
                         "mInitialization.acquired(Stage::Pool);", "pool.spawn(count);",
                         "PollService::Instance().start();", "mInitialization.acquired(Stage::Poll);",
                         "SctpTransport::Init();", "mInitialization.acquired(Stage::Sctp);",
                         "SctpTransport::SetSettings(mCurrentSctpSettings);")
    positions = [transaction_post.index(step) for step in acquisition_order]
    if positions != sorted(positions):
        raise TakeoverError("RTC stage ownership no longer matches partial/atomic start contracts")
    reservation = (source_root / "platform/online/reserved_cleanup_worker.h").read_text()
    if "set_value_at_thread_exit(" in reservation or "set_exception_at_thread_exit(" in reservation:
        raise TakeoverError("reserved cleanup cannot allocate late thread-exit registration")
    publisher = reservation.split("publisher = start([state = state_] {", 1)[1].split("\n            });", 1)[0]
    if publisher.index("state->cleanupThread.join();") >= publisher.index("state->completion.set_"):
        raise TakeoverError("RTC cleanup result must follow cleanup-worker join")
    if cleanup.index("if (!ownersRetired) return false;") >= cleanup.index("std::forward<Start>(start)()") or \
            "future_.wait_for(std::chrono::seconds(0))" not in cleanup or \
            "std::future_status::deferred" not in cleanup or \
            "error_ = std::current_exception();" not in cleanup:
        raise TakeoverError("cleanup observer lost owner gating, nonblocking poll or failure reporting")
    loop = interactive.index("while (running) {")
    if interactive.index("launcher.serviceCharacterWork();", loop) >= \
            interactive.index("if (readyToExit()) break;", loop):
        raise TakeoverError("minimized quit must service preview results before readiness")
    host_shutdown = main_app.split("void shutdownLauncherHost(AppHost &host, Launcher &launcher) {", 1)[1].split("\n}", 1)[0]
    ordered_shutdown = (
        "launcher.finishCharacterWorkForExit();",
        "Overlay_setPhonePartyHost(nullptr);",
        "launcher.finishNetworkShutdownForExit();",
        "AppLaunchHold_release();",
        "host.shutdown();",
    )
    positions = [host_shutdown.index(call) for call in ordered_shutdown]
    if positions != sorted(positions):
        raise TakeoverError("terminal host shutdown must preserve Workshop, aliases, network and SDL ownership order")
    if host_shutdown.index("if (host.window() == nullptr) return;") >= positions[0]:
        raise TakeoverError("repeated terminal cleanup must not service Workshop after host shutdown")
    # The only direct host shutdowns outside the shared helper are the two
    # startup failures before a Launcher (and its network owners) exists.
    helper_start = main_app.index("void shutdownLauncherHost(")
    helper_end = main_app.index("\n}", helper_start) + 2
    main_start = main_app.index("int main(")
    launcher_constructed = main_app.index("Launcher launcher;", main_start)
    for call in re.finditer(r"(?m)^\s*host\.shutdown\(\);", main_app):
        if not (helper_start <= call.start() < helper_end or
                main_start <= call.start() < launcher_constructed):
            raise TakeoverError("post-Launcher terminal host shutdown bypasses all-owner cleanup")
    if "if (!launcher.quitReady()) return false;" not in interactive or \
            interactive.index("if (!launcher.quitReady()) return false;") > \
            interactive.index("launcher.finishCharacterWorkForExit();") or \
            interactive.index("launcher.finishCharacterWorkForExit();") > \
            interactive.index("launcher.beginNetworkShutdown();"):
        raise TakeoverError("online closing must wait for transactional Workshop readiness")
    ready_to_exit = interactive.split("const auto readyToExit = [&]() {", 1)[1].split("\n    };", 1)[0]
    if interactive.count("if (readyToExit()) break;") != 3 or \
            "if (launcher.quitReady()) break;" in interactive or \
            "launcher.pollNetworkShutdown();" not in ready_to_exit or \
            "#if" in ready_to_exit or \
            "if (terminal && launcher.networkShutdownFailed()) exitCode = 2;" not in ready_to_exit or \
            "return terminal;" not in ready_to_exit:
        raise TakeoverError("every ordinary quit path and build profile must poll all-owner cleanup and report failure")
    main_body = main_app[main_start:]
    if "return launcher.networkShutdownFailed() && smokeResult == 0 ? 2 : smokeResult;" not in main_body:
        raise TakeoverError("shell-smoke success must not hide cleanup failure")
    autoplay = main_body.split('if (std::getenv("MDKR_APP_AUTOPLAY")) {', 1)[1].split(
        "\n    std::string bootRecoveryMessage;", 1
    )[0]
    failure_gate = autoplay.index("if (launcher.networkShutdownFailed()) {")
    failure_return = autoplay.index("return autoplayResult == 0 ? 2 : autoplayResult;")
    if failure_return < failure_gate:
        raise TakeoverError("autoplay cleanup failure must return a failing result")
    for relaunch in ("recoverRestartToLauncher(", "relaunchApplication("):
        if autoplay.index(relaunch) <= failure_return:
            raise TakeoverError("autoplay must refuse every relaunch after cleanup failure")
    terminal_main = main_body.rsplit("shutdownLauncherHost(host, launcher);", 1)[1]
    terminal_failure = terminal_main.index(
        "if (launcher.networkShutdownFailed()) return exitCode == 0 ? 2 : exitCode;"
    )
    for terminal_action in ("DiagLog_shutdown();", "recoverRestartToLauncher(", "relaunchApplication("):
        if terminal_main.index(terminal_action) <= terminal_failure:
            raise TakeoverError("main must refuse relaunch and preserve diagnostics until cleanup result is known")
    barrier = interactive.index("if (onlineClosing || launcher.quitRequested()) continue;")
    for poll in ("OnlineRoom_pollEngineRoomReady()", "OnlineRoom_pollEngineRaceBoot()"):
        if barrier >= interactive.index(poll):
            raise TakeoverError("quit must suppress both engine handoffs in the same frame")
    for forbidden in ("drawActivePanel(", "OnlineRoomPanel_draw(", "RomPanel_", "ImGui::Button("):
        if forbidden in closing:
            raise TakeoverError("closing progress must not expose new work: " + forbidden)
    if "ui::SpeakSection(title);" not in closing or "There is no reliable time estimate." not in closing:
        raise TakeoverError("closing progress lost spoken status or truthful delayed state")
    entry = room.split("void OnlineRoomPanel_draw(", 1)[1].split("\n}", 1)[0]
    if entry.index("if (sOnlineAppClosing) return;") >= entry.index("ensureInitialized();"):
        raise TakeoverError("closing must prevent room recreation before initialization")
    shutdown = room.split("void OnlineRoom_shutdownForAppExit() {", 1)[1].split(
        "\n}", 1
    )[0]
    if "sAdapterTeardowns.drain(std::chrono::seconds(10)" not in shutdown or \
            ".detach(" in shutdown or ".detach(" in tracker or \
            "if (worker.joinable()) worker.join();" not in tracker:
        raise TakeoverError("shutdown must join retiring workers even after its warning deadline")
    retirement = room.split(
        "static void teardownAdapterAsync(std::unique_ptr<IMdkrOnlineAdapter> adapter) {", 1
    )[1].split("\n}", 1)[0]
    handoff = retirement.index("sAdapterTeardowns.retire(std::move(adapter))")
    for retract in ("mdkr_online_live_adapter_retract_race_boot(",
                    "OnlineRoom_retractEngineRoomReady("):
        if retirement.index(retract) >= handoff:
            raise TakeoverError("engine registries must retract before tracked retirement")
    if "std::thread worker" in retirement or "sAdapterTeardowns.threads.push_back" in retirement:
        raise TakeoverError("room retirement must use the ownership-safe shared launcher")
    policy = room.split("bool OnlineRoom_isLobbyTakeoverActive() {", 1)[1].split(
        "\n}", 1
    )[0]
    unavailable = room.split("void drawBetaUnavailableRoom() {", 1)[1].split(
        "\n}", 1
    )[0]
    panel = room.split("void drawBetaOnlinePanel(LauncherState &state) {", 1)[1].split(
        "\n}", 1
    )[0]
    live_room = room.split("void drawBetaRoom(LauncherState &state) {", 1)[1].split(
        "// BETA-PANEL-LOCAL draw overrides", 1
    )[0]
    view_kind = room.split("static MdkrOnlineViewKind onlineCurrentViewKind() {", 1)[1].split(
        "\n}", 1
    )[0]
    takeover = launcher.split("void drawLobbyTakeover(", 1)[1].split("\n}", 1)[0]
    if "OnlineRoom_takeoverRequired(g_online.adapter != nullptr, kind)" not in policy:
        raise TakeoverError("takeover no longer uses actual adapter ownership")
    if '"Leave Room##unavailable"' not in unavailable or \
            "OnlineRoom_requestLeave();" not in unavailable or \
            "dispatch(" in unavailable:
        raise TakeoverError("unavailable room lost its view-independent deferred exit")
    if "if (!g_online.adapter)" not in panel or \
            "if (!g_online.initialized)" not in panel or \
            "drawBetaUnavailableRoom();" not in panel:
        raise TakeoverError("owned uninitialized room must not draw the chooser")
    if "if (!g_online.adapter->view(&model)) {\n        drawBetaUnavailableRoom();\n        return;" not in live_room:
        raise TakeoverError("failed live view must stop drawing before room-ready boot")
    if "OnlineRoom_readViewKind(g_online.adapter.get(), g_online.initialized)" not in view_kind:
        raise TakeoverError("shell view must use the tested read-only adapter boundary")
    if takeover.index("OnlineRoom_serviceLobbyLeave(state);") < \
            takeover.index("drawActivePanel(kLauncherPanelOnlineRoom, state, action);"):
        raise TakeoverError("room teardown must follow body drawing")
    live_body = room.split("void drawBetaRoom(LauncherState &state) {", 1)[1].split(
        "\n}", 1
    )[0]
    invite = room.split("bool drawBetaInviteCard(", 1)[1].split("\n}", 1)[0]
    selecting = room.split("bool drawBetaSelectingHandoff(", 1)[1].split("\n}", 1)[0]
    for required in (
        "if (drawBetaInviteCard(state, g_online.betaHostJourney)) return;",
        "model.local_member_is_leader)) return;",
        "if (drawBetaSelectingHandoff(state, model, lobby)) return;",
        "if (drawBetaPhraseDecision(model, state)) return;",
    ):
        if required not in live_body:
            raise TakeoverError("live room lost stale-frame barrier: " + required)
    if "rebuilt = true;" not in invite or "return rebuilt;" not in invite:
        raise TakeoverError("every invite rebuild attempt must invalidate the frame")
    if "return drawBetaStrandedRoomCard(state);" not in selecting:
        raise TakeoverError("stranded-room rebuild must propagate frame invalidation")
    actions = re.findall(r"handleAction\([^;]*\);", live_body)
    stopped_actions = re.findall(r"handleAction\([^;]*\);\s*return;", live_body)
    if len(actions) != 4 or len(stopped_actions) != len(actions):
        raise TakeoverError("live timeout/primary/secondary/cancel must end the old frame")
    phrase = room.split("bool drawBetaPhraseDecision(", 1)[1].split("\n}", 1)[0]
    if re.findall(r"handleAction\([^;]*\);", phrase) != ["handleAction(decision, state);"]:
        raise TakeoverError("phrase controls must defer one dispatch until drawing is complete")
    if phrase.index("handleAction(decision, state);") < phrase.rindex("ImGui::EndDisabled();") or \
            phrase.index("handleAction(decision, state);") < phrase.index("ui::CardEnd();"):
        raise TakeoverError("phrase dispatch must follow balanced UI scopes")
    for required in (
        "ImGui::BeginDisabled(!model.primary.enabled);",
        "model.secondary.enabled &&",
        "model.cancel.enabled &&",
        "if (decision == MDKR_ONLINE_VIEW_ACTION_NONE) return false;",
        "handleAction(decision, state);\n    return true;",
    ):
        if required not in phrase:
            raise TakeoverError("phrase decision lost admission/frame contract: " + required)
    if phrase.count("decision == MDKR_ONLINE_VIEW_ACTION_NONE;") != 2:
        raise TakeoverError("phrase secondary/cancel must not follow another same-frame decision")


def clean_environment(**updates: str) -> dict[str, str]:
    environment = {
        key: value for key, value in os.environ.items()
        if not key.startswith(("MDKR", "GE007_"))
    }
    environment.update(updates)
    return environment


def probe(binary: str, root: Path, slug: str, timeout: int) -> list[dict]:
    case_root = root / slug
    prefs = case_root / "prefs"
    saves = case_root / "saves"
    prefs.mkdir(parents=True)
    saves.mkdir()
    environment = clean_environment(
        LC_ALL="C",
        MDKR_APP_PREFS_DIR=str(prefs),
        MDKR_VIDEO_CONFIG_PATH=str(case_root / "video.ini"),
        MDKR_SAVE_DIR=str(saves),
        MDKR_AUDIO="0",
        MDKR64_HIDDEN="1",
        MDKR_APP_PANEL="Online Room",
        MDKR_APP_ONLINE_FAKE="1",
        MDKR_APP_ONLINE_GALLERY=slug,
        MDKR_ONLINE_ROOM_PREVIEW="1",
        MDKR_APP_LOBBY_TAKEOVER_PROBE="1",
        MDKR_APP_SMOKE_FRAMES="8",
    )
    if slug in ADMISSION_SLUGS:
        environment["MDKR_APP_ONLINE_FAKE_ALLOW_START"] = "1"
    completed = subprocess.run(
        [binary], env=environment, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=timeout,
    )
    if completed.returncode != 0:
        raise TakeoverError(
            f"{slug}: process exited {completed.returncode}\n"
            f"{completed.stdout[-4000:]}"
        )
    rows = [match.groupdict() for match in PROBE_RE.finditer(completed.stdout)]
    if not rows:
        raise TakeoverError(
            f"{slug}: no [app-lobby-takeover] probe rows were emitted\n"
            f"{completed.stdout[-4000:]}"
        )
    return rows


def check_active(binary: str, root: Path, slug: str, timeout: int) -> None:
    rows = probe(binary, root, slug, timeout)
    active_rows = [r for r in rows if r["online"] == "1"]
    if not active_rows:
        raise TakeoverError(
            f"{slug}: the fake session never reached an active state "
            "(online_active stayed 0)"
        )
    for row in active_rows:
        if row["play"] == "1":
            raise TakeoverError(
                f"{slug}: the generic offline Play button was drawn while the "
                f"online session was active (frame {row['frame']}) -- the "
                "offline launch is reachable during a live session"
            )
        if row["nav"] == "1":
            raise TakeoverError(
                f"{slug}: the launcher nav rail/top tabs were drawn while the "
                f"online session was active (frame {row['frame']})"
            )
        if row["took"] != "1":
            raise TakeoverError(
                f"{slug}: online was active but the modal lobby takeover did "
                f"not engage (frame {row['frame']})"
            )


def check_entry(binary: str, root: Path, slug: str, timeout: int) -> None:
    rows = probe(binary, root, slug, timeout)
    for row in rows:
        if row["online"] == "1" or row["took"] == "1":
            raise TakeoverError(
                f"{slug}: the entry/chooser must NOT take over the launcher "
                f"(frame {row['frame']})"
            )
    if not any(row["play"] == "1" for row in rows):
        raise TakeoverError(
            f"{slug}: the launcher shell (generic Play) was never drawn at the "
            "chooser -- the player has no way to reach it"
        )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build")
    parser.add_argument("--source-only", action="store_true",
                        help="check structural contracts without resolving or launching an app")
    parser.add_argument("--timeout", type=int, default=120)
    args = parser.parse_args()
    if not args.source_only and not args.build:
        parser.error("--build is required unless --source-only is selected")
    try:
        check_source_contract()
        if args.source_only:
            print("PASS online lobby takeover source contracts (no app executed)")
            return 0
        from harness_utils import resolve_binary
        binary = resolve_binary(args.build)
        if not Path(binary).exists():
            raise TakeoverError(f"binary not found: {binary}")
        with tempfile.TemporaryDirectory(prefix="mdkr-lobby-takeover-") as tmp:
            root = Path(tmp)
            for slug in ACTIVE_SLUGS:
                check_active(binary, root, slug, args.timeout)
            check_entry(binary, root, ENTRY_SLUG, args.timeout)
    except (TakeoverError, OSError, IndexError, ValueError, subprocess.TimeoutExpired) as error:
        print(f"FAIL online lobby takeover: {error}")
        return 1
    print(
        "PASS online lobby takeover: "
        f"active-cases={len(ACTIVE_SLUGS)} entry-shell=1 "
        "offline-play-suppressed=1"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
