#include "ui_online_room.h"

#include "app_theme.h"
#include "ui_common.h"
#include "ui_launcher.h"
#include "ui_settings.h"

#include "a11y_model.h"
#include "online/match_live_adapter.h"

#include "imgui.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#if MDKR_ENABLE_ONLINE_BETA
// Native online beta only: the create/join chooser, invite QR and live status
// UX. None of this compiles into a shipping (OFF) build, so the OFF object stays
// byte-identical.
#include "online/online_track_table.h"
#include "qrcodegen.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// Defined at the bottom of this file (beta lobby-takeover support); declared
// here so handleAction's ENTER_ANOTHER_CODE contract can retire the live
// adapter without reordering the whole translation unit.
static void teardownAdapterAsync(std::unique_ptr<IMdkrOnlineAdapter> adapter);

// Beta live-adapter provenance seams, defined in online_live_wiring.cpp (the
// beta-only wiring TU; match_live_adapter.h stays the audited public seam).
// The first returns the name of a set gameplay-determinism developer env seam
// (MDKR_RNGSEED / MDKR_ARCTAN / MDKR_TRIG) that would guarantee an online
// desync, or nullptr; the second derives THIS binary's real compatibility
// identity (version + release commit stamp + the given validated ROM revision)
// via mdkr_online_compatibility_from_provenance, returning false for a build
// without release provenance.
const char *OnlineRoom_liveBlockedByDeterminismEnv(void);
bool OnlineRoom_liveCompatibilityFromProvenance(uint8_t romRevision,
                                                MdkrOnlineCompatibilityV1 *out);
#endif

namespace {

struct OnlineRoomUiState {
    std::unique_ptr<IMdkrOnlineAdapter> adapter;
    bool initialized = false;
    bool detailsOpen = false;
    bool connectionDoctorOpen = false;
    bool updateHelpOpen = false;
    bool leaveRaceConfirm = false;
    std::uint64_t nextRequestId = 1u;
    MdkrOnlineViewKind announcedKind = static_cast<MdkrOnlineViewKind>(0);
    MdkrOnlineViewFailure announcedFailure = MDKR_ONLINE_VIEW_FAILURE_NONE;
    char announcedVerificationPhrase[64]{};
    const MdkrOnlineFakeGallerySpec *gallerySpec = nullptr;
    bool galleryTraced = false;
    MdkrOnlineViewAction focusedAction = MDKR_ONLINE_VIEW_ACTION_NONE;
    MdkrOnlineViewAction completedAction = MDKR_ONLINE_VIEW_ACTION_NONE;
    bool focusApplied = false;
    bool completedActionAccepted = false;
#if MDKR_ENABLE_ONLINE_BETA
    // Create/Join chooser state, consumed BEFORE the live adapter is built --
    // the live adapter fixes its journey + 6-digit join code at construction, so
    // the choice must be made here first.
    enum class BetaStage { Chooser, JoinCode };
    BetaStage betaStage = BetaStage::Chooser;
    char betaJoinCode[7] = {0};
    // Launcher-side journey memory: true once THIS UI dispatched CREATE in
    // buildBetaLiveAdapter. The view model's local_member_is_leader is false
    // until a lobby snapshot exists, so the pre-room states (CONNECTING) need
    // this to know the local player is the host and should see the invite card
    // (in placeholder mode) during the whole create round trip.
    bool betaHostJourney = false;
    bool betaBuildFailed = false;
    // Specific, user-facing reason the live adapter refused to build (ROM not
    // validated, determinism env seam set, no release provenance). Empty means
    // the generic "service unreachable" copy applies. Rendered by the existing
    // failure CautionBox in drawBetaChooser -- the established status path.
    char betaBuildFailedReason[512] = {0};
    // Deferred, non-blocking "Leave Race": set when the persistent takeover
    // control is pressed, consumed after the frame's lobby body has drawn.
    bool leavePending = false;
#endif
};

OnlineRoomUiState g_online;

#if MDKR_ENABLE_ONLINE_BETA
// Base-racer / vehicle labels for the beta roster strip. Only the beta lobby
// surfaces name racers and vehicles now (the legacy selection combos are
// retired), so these live under the beta gate to keep the OFF build clean.
constexpr const char *kCharacters[] = {
    "Diddy", "Timber", "Pipsy", "Tiptup", "Conker",
    "Bumper", "Banjo", "Krunch", "Drumstick", "T.T.",
};
constexpr const char *kVehicles[] = {"Car", "Hovercraft", "Plane"};
#endif

bool fakeEnabled() {
    return std::getenv("MDKR_APP_ONLINE_FAKE") != nullptr;
}

bool actionSmokeEnabled() {
    const char *token = std::getenv("MDKR_APP_ONLINE_ACTION_TOKEN");
    return token != nullptr &&
        std::strcmp(token, "mdkr64-online-action-v1") == 0;
}

MdkrOnlineViewAction focusedAction() {
    if (!actionSmokeEnabled()) return MDKR_ONLINE_VIEW_ACTION_NONE;
    if (g_online.focusedAction != MDKR_ONLINE_VIEW_ACTION_NONE) {
        return g_online.focusedAction;
    }
    const char *value = std::getenv("MDKR_APP_ONLINE_FOCUS_ACTION");
    char *end = nullptr;
    const long parsed = value != nullptr ? std::strtol(value, &end, 10) : 0;
    if (value == nullptr || end == value || *end != '\0' ||
        parsed <= MDKR_ONLINE_VIEW_ACTION_NONE ||
        parsed > MDKR_ONLINE_VIEW_ACTION_REPORT_PHRASE_MISMATCH) {
        return MDKR_ONLINE_VIEW_ACTION_NONE;
    }
    g_online.focusedAction = static_cast<MdkrOnlineViewAction>(parsed);
    return g_online.focusedAction;
}

void maybeFocusAction(MdkrOnlineViewAction action) {
    if (g_online.focusApplied || action != focusedAction()) return;
    ImGui::SetKeyboardFocusHere();
    // Programmatic focus intentionally hides Dear ImGui's navigation cursor
    // when the previous input source was the mouse. The action smoke still
    // activates through real SDL keyboard/gamepad events, but it needs the
    // same visible-nav precondition a player's first arrow/Tab press creates.
    // This is reachable only through the token-gated smoke contract above.
    ImGui::GetIO().ConfigNavCursorVisibleAlways = true;
    g_online.focusApplied = true;
    std::fprintf(stderr, "[online-ui-action] focus action=%u\n",
                 static_cast<unsigned>(action));
}

void recordAction(MdkrOnlineViewAction action, bool accepted) {
    if (!actionSmokeEnabled() || action != focusedAction()) return;
    g_online.completedAction = action;
    g_online.completedActionAccepted = accepted;
    std::fprintf(stderr, "[online-ui-action] activate action=%u accepted=%u\n",
                 static_cast<unsigned>(action), accepted ? 1u : 0u);
}

// FAKE adapter fixture ONLY (MDKR_APP_ONLINE_FAKE smokes, gallery previews and
// the gallery contract dump): a constant identity every build shares, which is
// exactly what a preview needs and exactly what live play must never carry.
// The LIVE adapter derives its identity from real provenance instead
// (buildBetaLiveAdapter -> OnlineRoom_liveCompatibilityFromProvenance), and
// OnlineRoom_makeGatedLiveAdapter refuses any compatibility -- including this
// fixture -- that does not byte-match that provenance identity.
MdkrOnlineCompatibilityV1 fakeCompatibility() {
    MdkrOnlineCompatibilityV1 value{};
    value.protocol_version = MDKR_ONLINE_PROTOCOL_VERSION;
    for (unsigned i = 0u; i < sizeof(value.build_id); ++i) {
        value.build_id[i] = static_cast<std::uint8_t>(i + 1u);
    }
    for (unsigned i = 0u; i < sizeof(value.gameplay_digest); ++i) {
        value.gameplay_digest[i] = static_cast<std::uint8_t>(0x80u + i);
    }
    value.rom_revision = 1u;
    value.cadence_hz = 30u;
    return value;
}

// Constructs the launcher-owned adapter behind the seam. The default path is
// the deterministic fake (the view-model oracle); the live adapter is swapped
// in only behind the internal-test-token gate AND the compile-time Online Room
// preview gate below -- never in a normal build.
std::unique_ptr<IMdkrOnlineAdapter> makeAdapter(
    const MdkrOnlineCompatibilityV1 &compatibility) {
#if MDKR_ENABLE_ONLINE_ROOM_PREVIEW
    if (mdkr_online_live_lobby_gate_open()
#if MDKR_ENABLE_ONLINE_BETA
        // Beta: the interactive live adapter is built by the create/join chooser
        // (drawBetaOnlinePanel) with the journey + 6-digit code the live adapter
        // fixes at construction. Here we only honor an explicit fake-adapter
        // smoke, so the fake is never shadowed by the live path.
        && !fakeEnabled()
#endif
    ) {
        // Non-beta token-gated dev builds create a room with a fixed journey.
        std::unique_ptr<IMdkrOnlineAdapter> live =
            OnlineRoom_makeGatedLiveAdapter(
                compatibility, MDKR_ONLINE_JOURNEY_CREATE, std::string());
        if (live) return live;
    }
#endif
    if (!fakeEnabled()) return nullptr;
    auto fake = std::make_unique<MdkrOnlineFakeAdapterSeam>();
    if (!fake->init(UINT64_C(0x4f4e303342), &compatibility,
                    std::getenv("MDKR_APP_ONLINE_FAKE_ALLOW_START") != nullptr)) {
        return nullptr;
    }
    const char *gallery = std::getenv("MDKR_APP_ONLINE_GALLERY");
    if (gallery != nullptr && gallery[0] != '\0') {
        g_online.gallerySpec = mdkr_online_fake_gallery_find(gallery);
        if (g_online.gallerySpec == nullptr ||
            !mdkr_online_fake_prepare_gallery(fake->fakeAdapter(), gallery)) {
            std::fprintf(stderr,
                         "[online-gallery] rejected slug=%s admission=%u\n",
                         gallery, fake->raceAdmissionEnabled() ? 1u : 0u);
            g_online.gallerySpec = nullptr;
            return nullptr;
        }
        // Gallery construction intentionally traverses the public command seam,
        // so it consumes request IDs just like a resumed persisted room would.
        // Continue after its high-water mark: restarting at 1 turns the first
        // player action into a correctly rejected replay.
        g_online.nextRequestId = fake->fakeAdapter()->last_request_id + 1u;
        if (g_online.nextRequestId == 0u) g_online.nextRequestId = 1u;
    }
    return fake;
}

void ensureInitialized() {
    if (g_online.initialized) return;
#if MDKR_ENABLE_ONLINE_BETA
    // Beta: never auto-construct the live adapter. The create/join chooser
    // (drawBetaOnlinePanel) builds it once the player picks a journey + code.
    // Only an explicit fake-adapter smoke falls through to the fake below.
    if (!fakeEnabled()) return;
#endif
    const MdkrOnlineCompatibilityV1 compatibility = fakeCompatibility();
    std::unique_ptr<IMdkrOnlineAdapter> adapter = makeAdapter(compatibility);
    if (!adapter) return;
    /* ADAPTER-LIFETIME INVARIANT (mirrors ~LiveAdapter in
     * match_live_adapter.cpp): a LIVE adapter is destroyed ONLY via
     * teardownAdapterAsync, which retracts BOTH engine registries (race-boot +
     * room-ready) on the launcher thread BEFORE the detached destruction -- so a
     * published handoff can never outlive its adapter. This assignment is SAFE
     * because it cannot destroy a live adapter inline: the initialized-guard above
     * returns early once a session is up, and the only adapter this builds is the
     * FAKE (fakeEnabled() gate) -- g_online.adapter is null here. Do NOT reset /
     * reassign g_online.adapter while it holds a live adapter without routing
     * through teardownAdapterAsync, or a registry pointer will dangle (a UAF that
     * would pass every existing gate). */
    g_online.adapter = std::move(adapter);
    g_online.initialized = true;
}

MdkrOnlineAdapterStep dispatch(MdkrOnlineViewAction action,
                               unsigned seat = 0u, unsigned value = 0u) {
    MdkrOnlineAdapterCommand command;
    command.expectedRevision = g_online.adapter->revision();
    command.requestId = g_online.nextRequestId++;
    command.action = action;
    command.seat = seat;
    command.value = value;
    const MdkrOnlineAdapterStep step = g_online.adapter->submit(command);
    if (actionSmokeEnabled() && action == focusedAction()) {
        std::fprintf(stderr,
                     "[online-ui-action] dispatch action=%u seat=%u value=%u "
                     "error=%u\n",
                     static_cast<unsigned>(action), seat, value,
                     static_cast<unsigned>(step.error));
    }
    recordAction(action, step.accepted);
    return step;
}

void speakFocused(const MdkrOnlineViewControl &control, const char *help) {
    ui::SpeakFocusedItem(control.label, control.enabled ? "Available" : "Unavailable",
                         help);
}

bool drawActionButton(const MdkrOnlineViewControl &control, bool primary) {
    if (!control.visible || control.label == nullptr) return false;
    if (!control.enabled) ImGui::BeginDisabled();
    const ImVec2 size(-1.0f, primary ? ui::kBtnPrimary().y
                                    : ui::kBtnSecondary().y);
    maybeFocusAction(control.action);
    const bool pressed = primary
        ? ui::BrandPrimaryButton(control.label, size)
        : ImGui::Button(control.label, size);
    speakFocused(control, "Activates the named launcher action.");
    if (!control.enabled) ImGui::EndDisabled();
    return pressed && control.enabled;
}

void drawUnavailablePanel() {
    ui::SectionHeader(
        "Online Room",
        "Private online rooms are being qualified. Local play and phone "
        "controllers remain available without this feature.");
    ui::CautionBox(
        "Online Racing Is Not Enabled in This Build",
        "The rollback release gate is still open. No room is created, no "
        "network request is made, and the Play action remains local.");
    ui::Gap(ui::kGapM);
    ui::GroupHeader("What Will Live Here",
                    "Create or join a private room, check compatibility, choose "
                    "racers, vote, load together and keep the party for a rematch.");
    ImGui::BulletText("Matchmaking, invites and recovery stay in the launcher.");
    ImGui::BulletText("The game receives only a frozen race descriptor and inputs.");
    ImGui::BulletText("Start Race stays absent until the written rollback GO.");
}

void announceView(const MdkrOnlineViewModel &model) {
    const char *phrase = model.verification_phrase;
    if (!ui::SpeechEnabled() ||
        (model.kind == g_online.announcedKind &&
         model.failure == g_online.announcedFailure &&
         std::strcmp(phrase, g_online.announcedVerificationPhrase) == 0)) return;
    g_online.announcedKind = model.kind;
    g_online.announcedFailure = model.failure;
    std::snprintf(g_online.announcedVerificationPhrase,
                  sizeof(g_online.announcedVerificationPhrase), "%s", phrase);
    char message[MDKR_A11Y_TEXT_MAX];
    if (phrase[0] != '\0') {
        std::snprintf(message, sizeof(message),
                      "%s. %s Verification phrase: %s. Do not continue if "
                      "even 1 word differs.",
                      model.title, model.explanation, phrase);
    } else {
        std::snprintf(message, sizeof(message), "%s. %s", model.title,
                      model.explanation);
    }
    mdkr_a11y_announce(
        MDKR_A11Y_CAT_STATUS,
        model.announcement == MDKR_ONLINE_ANNOUNCE_ASSERTIVE
            ? MDKR_A11Y_PRI_CRITICAL : MDKR_A11Y_PRI_NORMAL,
        message);
}

void drawVerificationPhrase(const MdkrOnlineViewModel &model) {
    if (model.verification_phrase[0] == '\0') return;
    ui::Gap(ui::kGapM);
    if (ui::CardBegin("##online-verification-phrase", AppTheme::accent(), 0.0f)) {
        ImGui::TextUnformatted("Compare on Every Display");
        ui::Gap(ui::kGapXS);
        ImGui::PushFont(AppTheme::fonts().title);
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(model.verification_phrase);
        ImGui::PopTextWrapPos();
        ImGui::PopFont();
        ui::Gap(ui::kGapXS);
        ui::TextSubtleWrapped(
            "Do not continue if even 1 word differs. Leave the room and retry "
            "the secure connection instead.");
    }
    ui::CardEnd();
}

void drawConnectionDetails(const MdkrOnlineViewModel &model) {
    if (!g_online.detailsOpen) return;
    ui::Gap(ui::kGapS);
    if (ui::CardBegin("##online-details", AppTheme::subtle(), 0.0f)) {
        ImGui::TextUnformatted("Connection Details");
        ui::TextSubtleWrapped(
            "Preview adapter • no external service • no addresses, invite "
            "secrets, names or input samples in diagnostics");
        ui::TextSubtle("Members %u • Racer seats %u • Ready %u",
                       model.member_count, model.seat_count, model.ready_count);
    }
    ui::CardEnd();
    if (g_online.connectionDoctorOpen &&
        ui::CardBegin("##online-connection-doctor", AppTheme::accent(), 0.0f)) {
        ImGui::TextUnformatted("Connection Doctor");
        ui::TextSubtleWrapped(
            "1. Keep this display and router online. 2. Ask each friend to "
            "retry from the newest invite. 3. If direct play still fails, "
            "use local play now and try the room again later.");
        ui::TextSubtleWrapped(
            "Diagnostics stay privacy-safe: no IP addresses, invite secrets, "
            "player names or controller inputs are shown or copied.");
    }
    if (g_online.connectionDoctorOpen) ui::CardEnd();
}

void drawUpdateHelp() {
    if (!g_online.updateHelpOpen) return;
    ui::Gap(ui::kGapS);
    if (ui::CardBegin("##online-update-help", AppTheme::accent(), 0.0f)) {
        ImGui::TextUnformatted("Update Safely");
        ui::TextSubtleWrapped(
            "Automatic updates are not enabled in this build. Close the room, "
            "download the newest published release from the same trusted source "
            "you installed, then reopen the newest invite. Your ROM and local "
            "settings are not uploaded or replaced.");
    }
    ui::CardEnd();
}

void handleAction(MdkrOnlineViewAction action, LauncherState &state) {
    if (action == MDKR_ONLINE_VIEW_ACTION_PLAY_HERE ||
        action == MDKR_ONLINE_VIEW_ACTION_CHOOSE_ROM ||
        action == MDKR_ONLINE_VIEW_ACTION_RETURN_HOME) {
        if (g_online.adapter->sessionIntent() != MDKR_INTENT_NONE) {
            const MdkrOnlineAdapterStep step = dispatch(action);
            if (!step.accepted) return;
        } else {
            recordAction(action, true);
        }
        Launcher_requestTab(state, kLauncherPanelPlay,
                            kLauncherTabPlayer);
        return;
    }
    if (action == MDKR_ONLINE_VIEW_ACTION_CHANGE_TRACK) {
        const MdkrOnlineAdapterStep step =
            dispatch(MDKR_ONLINE_VIEW_ACTION_RACE_AGAIN);
        recordAction(action, step.accepted);
    } else if (action == MDKR_ONLINE_VIEW_ACTION_CONNECTION_DETAILS) {
        g_online.detailsOpen = !g_online.detailsOpen;
        recordAction(action, true);
    } else if (action == MDKR_ONLINE_VIEW_ACTION_CONNECTION_DOCTOR) {
        g_online.detailsOpen = true;
        g_online.connectionDoctorOpen = !g_online.connectionDoctorOpen;
        recordAction(action, true);
    } else if (action == MDKR_ONLINE_VIEW_ACTION_UPDATE_GAME) {
        g_online.updateHelpOpen = !g_online.updateHelpOpen;
        recordAction(action, true);
    } else if (action == MDKR_ONLINE_VIEW_ACTION_SETUP_CONTROLLER) {
        Settings_requestControllerSection();
        Launcher_requestTab(state, kLauncherPanelSettings,
                            kLauncherTabPlayer);
        recordAction(action, true);
    } else if (action == MDKR_ONLINE_VIEW_ACTION_LEAVE_RACE) {
        if (!g_online.leaveRaceConfirm) {
            g_online.leaveRaceConfirm = true;
            recordAction(action, true);
        } else {
            const MdkrOnlineAdapterStep step = dispatch(action);
            if (step.accepted) {
                g_online.leaveRaceConfirm = false;
                Launcher_requestTab(state, kLauncherPanelPlay,
                                    kLauncherTabPlayer);
            }
        }
    } else {
        const MdkrOnlineAdapterStep step = dispatch(action);
#if MDKR_ENABLE_ONLINE_BETA
        // ENTER_ANOTHER_CODE contract (live adapter only): the room transport
        // begins exactly once per adapter, so an accepted step carrying the
        // sentinel error means "destroy this adapter and collect a fresh
        // 6-digit code". The fake adapter re-joins in place and never returns
        // the sentinel, so this branch cannot fire on the preview path.
        if (action == MDKR_ONLINE_VIEW_ACTION_ENTER_ANOTHER_CODE &&
            step.accepted &&
            step.error == kMdkrOnlineLiveStepEnterAnotherCode) {
            teardownAdapterAsync(std::move(g_online.adapter));
            g_online.initialized = false;
            g_online.detailsOpen = false;
            g_online.connectionDoctorOpen = false;
            g_online.updateHelpOpen = false;
            g_online.leaveRaceConfirm = false;
            g_online.announcedKind = static_cast<MdkrOnlineViewKind>(0);
            g_online.announcedFailure = MDKR_ONLINE_VIEW_FAILURE_NONE;
            g_online.announcedVerificationPhrase[0] = '\0';
            g_online.betaStage = OnlineRoomUiState::BetaStage::JoinCode;
            g_online.betaJoinCode[0] = '\0';
            g_online.betaHostJourney = false;  // re-entering a code -> joiner
            g_online.betaBuildFailed = false;
            g_online.betaBuildFailedReason[0] = '\0';
        }
#else
        (void)step;
#endif
    }
}

void drawLeaveRaceConfirmation(LauncherState &state) {
    if (!g_online.leaveRaceConfirm) return;
    ui::Gap(ui::kGapM);
    ui::CautionBox(
        "Leave This Race?",
        "The race keeps running for your friends. This display will stop the "
        "game and leave the private room.");
    ui::Gap(ui::kGapS);
    if (ImGui::Button("Keep Racing", ui::kBtnFullWidth())) {
        g_online.leaveRaceConfirm = false;
    }
    ui::SpeakFocusedItem("Keep Racing", "Recommended",
                         "Closes this confirmation without changing the race.");
    ui::Gap(ui::kGapS);
    if (ImGui::Button("Leave Race and Room", ui::kBtnFullWidth())) {
        handleAction(MDKR_ONLINE_VIEW_ACTION_LEAVE_RACE, state);
    }
    ui::SpeakFocusedItem(
        "Leave Race and Room", "Destructive",
        "Stops this display's game and disconnects it from the private room.");
}

void drawRoomPanel(LauncherState &state) {
    g_online.adapter->service();
    MdkrOnlineViewModel model{};
    if (!g_online.adapter->view(&model)) {
        ui::CautionBox("Preview State Rejected",
                       "The fake adapter produced an invalid composition. "
                       "Return to the Game ROM panel and inspect Diagnostics.");
        return;
    }
    if (g_online.gallerySpec != nullptr && !g_online.galleryTraced) {
        g_online.galleryTraced = true;
        std::fprintf(
            stderr,
            "[online-gallery] rendered slug=%s kind=%u failure=%u primary=%u "
            "secondary=%u cancel=%u timeout=%u timeout-visible=%u admission=%u\n",
            g_online.gallerySpec->slug, static_cast<unsigned>(model.kind),
            static_cast<unsigned>(model.failure),
            static_cast<unsigned>(model.primary.action),
            static_cast<unsigned>(model.secondary.action),
            static_cast<unsigned>(model.cancel.action),
            model.timeout.present ? 1u : 0u,
            g_online.adapter->timeoutExpired() ? 1u : 0u,
            g_online.adapter->raceAdmissionEnabled() ? 1u : 0u);
    }
    announceView(model);
    ui::SectionHeader(model.title, model.explanation);

    if (ui::CardBegin("##online-status", AppTheme::brandSky(), 0.0f)) {
        ImGui::TextUnformatted(model.status != nullptr ? model.status
                                                       : "Private Room Preview");
        ui::TextSubtle("%u members • %u racer seats • %u ready",
                       model.member_count, model.seat_count, model.ready_count);
        if (!g_online.adapter->raceAdmissionEnabled()) {
            ui::TextSubtleWrapped(
                "Interaction preview only. Start Race is held by the local "
                "rollback release gate; this screen cannot enable it.");
        }
    }
    ui::CardEnd();
    drawVerificationPhrase(model);
    ui::Gap(ui::kGapM);

    const MdkrOnlineViewAction timeoutAction =
        model.timeout.present && g_online.adapter->timeoutExpired()
            ? model.timeout.primary.action : MDKR_ONLINE_VIEW_ACTION_NONE;
    if (timeoutAction != MDKR_ONLINE_VIEW_ACTION_NONE) {
        ui::CautionBox(model.timeout.title, model.timeout.explanation);
        ui::Gap(ui::kGapS);
        if (drawActionButton(model.timeout.primary, true)) {
            handleAction(model.timeout.primary.action, state);
        }
        ui::Gap(ui::kGapM);
    }

    if (model.primary.action != timeoutAction &&
        drawActionButton(model.primary, true)) {
        handleAction(model.primary.action, state);
    }
    if (model.secondary.visible && model.secondary.action != timeoutAction) {
        ui::Gap(ui::kGapS);
        if (drawActionButton(model.secondary, false)) {
            handleAction(model.secondary.action, state);
        }
    }
    if (model.cancel.visible) {
        ui::Gap(ui::kGapS);
        if (drawActionButton(model.cancel, false)) {
            handleAction(model.cancel.action, state);
        }
    }

    if (model.kind == MDKR_ONLINE_VIEW_RACING) {
        ui::Gap(ui::kGapM);
        ImGui::BeginDisabled(!g_online.adapter->raceAdmissionEnabled());
        if (ImGui::Button("Finish Preview Race", ui::kBtnFullWidth())) {
            // Development-only result stub, valid only for the fake adapter.
            if (MdkrOnlineFakeAdapter *fake = g_online.adapter->fakeAdapter()) {
                mdkr_online_fake_finish_race(fake, fake->revision);
            }
        }
        ui::SpeakFocusedItem(
            "Finish Preview Race",
            g_online.adapter->raceAdmissionEnabled() ? "Available" : "Unavailable",
            "A development-only stand-in for a confirmed engine result.");
        ImGui::EndDisabled();
    }
    drawLeaveRaceConfirmation(state);
    drawConnectionDetails(model);
    drawUpdateHelp();

}

#if MDKR_ENABLE_ONLINE_BETA
// ===========================================================================
// Native online beta: a first-run-friendly create/join + live lobby UX.
//
// Everything below is compiled ONLY under MDKR_ENABLE_ONLINE_BETA. It drives the
// same IMdkrOnlineAdapter seam the fake path uses and only SURFACES existing
// view-model / adapter signals -- the security-audited SAS/crypto/parser
// internals and the 2-endpoint / retail-identity / STUN-only fences are never
// touched here.
// ===========================================================================

// TEST-ONLY invite override for the beta lobby render seam (drawBetaRoomFake,
// MDKR_APP_ONLINE_BETA_FAKE). drawBetaInviteCard consults this ONLY when
// `active` is set, which happens exclusively while a synthetic fake stage is
// being rendered for a headless screenshot. The live/production beta path never
// sets it, so its behaviour (OnlineRoom_liveInvite) is unchanged; the whole
// struct is compiled only under MDKR_ENABLE_ONLINE_BETA, so the OFF/release
// build never sees it.
struct BetaFakeInviteOverride {
    bool active = false;
    std::string code;
    std::string url;
};
BetaFakeInviteOverride g_betaFakeInvite;

// Records which post-pairing SELECTING surface last rendered: the forward native
// hand-off card (Handoff) or the "Return to game" re-entry control shown after a
// LEFT/ERROR native return (Reentry). The per-race ImGui grid is retired, so there
// is no third state. The render seam (drawBetaRoomFake) emits it as a semantic
// witness a headless test asserts on; written by a single enum assignment on the
// live path too (negligible), consulted only by the seam.
enum class BetaSelectingRender { None, Handoff, Reentry };
BetaSelectingRender g_betaSelectingRender = BetaSelectingRender::None;

// Records the post-pairing RESULTS surface last rendered: the concise native "the
// game is showing results" hand-off card. The full ImGui results/standings/replay
// body is retired, so Handoff is the only rendered state. Same witness discipline
// as g_betaSelectingRender: emitted by the render seam, consulted only by the seam.
enum class BetaResultsRender { None, Handoff };
BetaResultsRender g_betaResultsRender = BetaResultsRender::None;

// Restrict the join-code field to the 6 digits the fallback code uses. This
// same filter also sanitizes PASTE: ImGui runs every clipboard character
// through the CallbackCharFilter (imgui_widgets.cpp InputTextFilterCharacter,
// input_source_is_clipboard=true), so pasting "123-456" or "code 123456" keeps
// only the digits, and the 7-byte betaJoinCode buffer (6 digits + NUL) clamps
// the result to the first 6 -- exactly the "strip non-digits, take first 6"
// contract, with no extra buffer bookkeeping.
int betaDigitsOnlyFilter(ImGuiInputTextCallbackData *data) {
    return (data->EventChar < '0' || data->EventChar > '9') ? 1 : 0;
}

// A time-based cycling ellipsis (".", "..", "..."), so every "waiting" status
// line and the pre-code invite placeholder read as motion instead of a frozen
// spinner. A pure function of the ImGui frame clock -- no per-widget state to
// store or reset.
const char *betaEllipsis() {
    static const char *const kFrames[] = {".", "..", "..."};
    const int step = static_cast<int>(ImGui::GetTime() / 0.35) % 3;
    return kFrames[step < 0 ? 0 : step];
}

// Whether the headless render seam is driving this frame (defined lower down).
bool betaFakeStageEnabled();

// If a composed status line ends in the "…" the copy catalog uses, swap that
// static ellipsis for the animated dot run above so a waiting line visibly
// animates. Any line that does not end in an ellipsis is left untouched.
void betaAnimateEllipsis(char *line, std::size_t size) {
    const std::size_t len = std::strlen(line);
    static const char kEllipsis[] = "\xE2\x80\xA6";  // U+2026 HORIZONTAL ELLIPSIS
    if (len < 3u || std::memcmp(line + len - 3u, kEllipsis, 3u) != 0) return;
    // The headless render seam runs on the wall clock, not a frame the reviewer
    // controls, so a captured waiting line would freeze on an arbitrary 1-3 dot
    // phase -- which reads as a stray trailing period. Keep the static "…" glyph
    // there so captures are stable and read as a waiting line; the live path
    // still animates.
    if (betaFakeStageEnabled()) return;
    std::snprintf(line + (len - 3u), size - (len - 3u), "%s", betaEllipsis());
}

// Record a specific, user-facing build refusal for the chooser's existing
// failure CautionBox and fail the build.
bool betaBuildRefused(const char *reason) {
    std::snprintf(g_online.betaBuildFailedReason,
                  sizeof(g_online.betaBuildFailedReason), "%s", reason);
    g_online.betaBuildFailed = true;
    return false;
}

// The validated ROM's compatibility revision: the two supported decomp builds
// map to the two revisions the provenance generator accepts (us.v80 -> 1,
// pal.v80 -> 2, matching MDKR_ROM_US_11 / the PAL row and their authored
// 30/25Hz cadences). 0 means "not a supported, validated ROM".
std::uint8_t betaValidatedRomRevision(const RomInfo &info) {
    // Both flags are required: `valid` is the layout/revision verdict and
    // `integrity_verified` is the full-image SHA-256 match against the
    // reference digest -- the same rom_validation.c contract the engine
    // re-checks at boot. The Online Room tab is reachable without a ROM, so
    // this gate (not panel navigation) is what guarantees a genuinely
    // validated ROM before any live adapter exists.
    if (!info.valid || !info.integrity_verified) return 0u;
    if (std::strcmp(info.build, "us.v80") == 0) return 1u;
    if (std::strcmp(info.build, "pal.v80") == 0) return 2u;
    return 0u;
}

// Build the live adapter with the chosen journey (+ code for JOIN) and kick the
// entry action off immediately, so the chooser choice IS the create/join.
//
// The compatibility handed to the live adapter is NEVER the fake fixture: it is
// derived here, at CREATE/JOIN time, from real provenance (this build's
// version + release commit stamp) plus the launcher's genuinely validated ROM
// revision -- so two different builds, or the same build with different
// accepted ROMs, refuse each other at the lobby JOIN byte-compare instead of
// desyncing mid-race.
bool buildBetaLiveAdapter(const LauncherState &state, MdkrOnlineJourney journey,
                          const std::string &code) {
    // The gameplay-determinism developer seams change gameplay math on
    // this machine only; a one-sided setting guarantees an online desync that
    // no compatibility byte-compare can see. Refuse up front with the specific
    // variable named (OnlineRoom_makeGatedLiveAdapter enforces this too).
    if (const char *seam = OnlineRoom_liveBlockedByDeterminismEnv()) {
        char reason[256];
        std::snprintf(reason, sizeof(reason),
                      "Online play is unavailable while the %s developer "
                      "variable is set: it changes gameplay on this computer "
                      "only, which would break an online race. Unset it and "
                      "relaunch to race online.",
                      seam);
        return betaBuildRefused(reason);
    }
    const std::uint8_t romRevision = betaValidatedRomRevision(state.romInfo);
    if (romRevision == 0u) {
        return betaBuildRefused(
            "Online play needs a fully verified game ROM. Open the Play "
            "panel, add a supported ROM and let verification finish, then "
            "come back here.");
    }
    MdkrOnlineCompatibilityV1 compatibility;
    if (!OnlineRoom_liveCompatibilityFromProvenance(romRevision,
                                                    &compatibility)) {
        // Dev builds carry no release provenance stamp (MDKR_BUILD_STAMP), so
        // they cannot prove a gameplay digest to a peer. Fail closed rather
        // than fabricate an identity another build could collide with.
        return betaBuildRefused(
            "This build has no release provenance, so it cannot prove it "
            "matches your friend's game. Use a published release build to "
            "race online.");
    }
    std::unique_ptr<IMdkrOnlineAdapter> adapter =
        OnlineRoom_makeGatedLiveAdapter(compatibility, journey, code);
    if (!adapter) {
        // Generic copy (service unreachable / no compiled origin).
        g_online.betaBuildFailedReason[0] = '\0';
        g_online.betaBuildFailed = true;
        return false;
    }
    /* ADAPTER-LIFETIME INVARIANT (mirrors ~LiveAdapter in
     * match_live_adapter.cpp): a LIVE adapter is destroyed ONLY via
     * teardownAdapterAsync, which retracts BOTH engine registries (race-boot +
     * room-ready) on the launcher thread BEFORE the detached destruction. This
     * assignment is SAFE because it runs only from the create/join chooser with no
     * active session, so g_online.adapter is null and nothing is destroyed inline
     * (a live handoff and this chooser are mutually-exclusive UI states). Do NOT
     * reset / reassign g_online.adapter while it holds a live adapter without
     * routing through teardownAdapterAsync, or a registry pointer will dangle. */
    g_online.adapter = std::move(adapter);
    /* A fresh adapter/session -- re-arm the one-shot room-ready latch so
     * the next SELECTING transition (ANY online mode -- the takeover is
     * mode-agnostic) can publish this adapter for the native descriptor-less
     * takeover. */
    OnlineRoom_resetRoomReadyLatch();
    g_online.initialized = true;
    g_online.betaHostJourney = journey == MDKR_ONLINE_JOURNEY_CREATE;
    g_online.betaBuildFailed = false;
    g_online.betaBuildFailedReason[0] = '\0';
    dispatch(journey == MDKR_ONLINE_JOURNEY_CREATE
                 ? MDKR_ONLINE_VIEW_ACTION_CREATE_ROOM
                 : MDKR_ONLINE_VIEW_ACTION_JOIN_ROOM);
    return true;
}

void drawBetaChooser(LauncherState &state) {
    ui::SectionHeader(
        "Play Online",
        "Race a friend over the internet: private, invite-only, 2 players, base "
        "racers, direct peer-to-peer. Pick a side to start; the lobby takes over "
        "once you connect.");
    if (g_online.betaBuildFailed) {
        // Specific refusal reasons (unvalidated ROM, determinism env seam, no
        // release provenance) come from buildBetaLiveAdapter; the generic copy
        // covers an unreachable service.
        ui::CautionBox(
            "Couldn't Start Online",
            g_online.betaBuildFailedReason[0] != '\0'
                ? g_online.betaBuildFailedReason
                : "The online service could not be reached from this build. "
                  "Local play and phone controllers still work; try the room "
                  "again later.");
        ui::Gap(ui::kGapS);
    }
    if (g_online.betaStage == OnlineRoomUiState::BetaStage::Chooser) {
        if (ui::CardBegin("##beta-chooser", AppTheme::brandSky(), 0.0f)) {
            ImGui::TextUnformatted("How do you want to play?");
            ui::Gap(ui::kGapS);
            if (ui::BrandPrimaryButton("Host a Race", ui::kBtnFullWidth())) {
                buildBetaLiveAdapter(state, MDKR_ONLINE_JOURNEY_CREATE,
                                     std::string());
            }
            ui::SpeakFocusedItem("Host a Race", "Create a room",
                                 "Creates a private room and shows a code to share.");
            ui::Gap(ui::kGapS);
            if (ImGui::Button("Join a Race", ui::kBtnFullWidth())) {
                g_online.betaStage = OnlineRoomUiState::BetaStage::JoinCode;
                g_online.betaJoinCode[0] = '\0';
                g_online.betaBuildFailed = false;
                g_online.betaBuildFailedReason[0] = '\0';
            }
            ui::SpeakFocusedItem("Join a Race", "Enter a code",
                                 "Enter the 6-digit code your host shares with you.");
        }
        ui::CardEnd();
        ui::TextSubtleWrapped(
            "One player hosts and shares the code; the other joins with it. Before "
            "the race starts you will compare a short safety phrase together.");
    } else {
        if (ui::CardBegin("##beta-join", AppTheme::accent(), 0.0f)) {
            ImGui::TextUnformatted("Enter the 6-digit code from your host");
            ui::Gap(ui::kGapS);
            // ONE grouped code field: the big "123 4··" grouping IS the input,
            // not a separate echo above a plain box. The editable buffer stays
            // the RAW digits on purpose -- inserting the group space into the
            // buffer would fight ImGui's cursor bookkeeping (the space sits at a
            // fixed index, so a mid-string edit or a backspace desyncs it) and
            // break the "value is exactly 6 digits" contract the Join path
            // relies on. So the InputText draws its own text TRANSPARENTLY and a
            // grouped overlay is painted over it in the same title font the
            // host's invite card uses, so both sides read the code the same way.
            // Filled slots draw in the normal color; the remaining "·"
            // placeholders are dimmed so the code shape reads without competing
            // with the digits already entered. The overlay is drawn AFTER the
            // field applies this frame's edit, so it is never a frame stale.
            ImGui::PushFont(AppTheme::fonts().title);
            ImGui::SetNextItemWidth(ui::kControlWidth());
            // CallbackCharFilter sanitizes both typing AND paste (see
            // betaDigitsOnlyFilter); the 7-byte buffer keeps the first 6 digits.
            // TWO style pushes make the field visually empty so only the grouped
            // overlay shows. Transparent ImGuiCol_Text hides the raw digits.
            // Transparent ImGuiCol_InputTextCursor hides the caret: in this ImGui
            // (1.92) the caret is a SEPARATE color slot -- imgui_widgets.cpp
            // draws it with ImGuiCol_InputTextCursor, NOT ImGuiCol_Text -- and
            // the app theme never sets that slot, so it would otherwise default
            // to opaque white and blink against the field's UNGROUPED raw text,
            // one group-space left of the overlay boundary after the third digit.
            // The grouped overlay's dot placeholders already communicate the
            // entry position, so the caret adds nothing.
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
            ImGui::PushStyleColor(ImGuiCol_InputTextCursor,
                                  ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
            ImGui::InputText("##beta-join-code", g_online.betaJoinCode,
                             sizeof(g_online.betaJoinCode),
                             ImGuiInputTextFlags_CallbackCharFilter,
                             betaDigitsOnlyFilter);
            ImGui::PopStyleColor(2);
            // Paint the grouped code over the (transparent) field text. AddText
            // and CalcTextSize both read the pushed title font. Bounded-by-design
            // divergence: a click hit-tests against the field's UNGROUPED raw
            // text (the group space is overlay-only, absent from the buffer), and
            // this overlay does not track the field's horizontal scroll. Both are
            // harmless here because six digits in the title font never overflow
            // the fixed-width field -- it never scrolls, and the caret column and
            // each drawn glyph stay within a group-space of where a click lands.
            {
                const std::size_t shown = std::strlen(g_online.betaJoinCode);
                ImDrawList *draw = ImGui::GetWindowDrawList();
                const ImVec2 fieldMin = ImGui::GetItemRectMin();
                const ImVec2 pad = ImGui::GetStyle().FramePadding;
                const ImU32 filledCol = ImGui::GetColorU32(ImGuiCol_Text);
                const ImU32 restCol = ImGui::GetColorU32(AppTheme::subtle());
                const float spaceW = ImGui::CalcTextSize(" ").x;
                float x = fieldMin.x + pad.x;
                const float y = fieldMin.y + pad.y;
                for (unsigned i = 0u; i < 6u; ++i) {
                    if (i == 3u) x += spaceW;  // visual grouping only
                    const bool isFilled = i < shown;
                    char glyph[4];
                    if (isFilled) {
                        glyph[0] = g_online.betaJoinCode[i];
                        glyph[1] = '\0';
                    } else {
                        glyph[0] = '\xC2';  // U+00B7 MIDDLE DOT placeholder
                        glyph[1] = '\xB7';
                        glyph[2] = '\0';
                    }
                    draw->AddText(ImVec2(x, y), isFilled ? filledCol : restCol,
                                  glyph);
                    x += ImGui::CalcTextSize(glyph).x;
                }
            }
            ImGui::PopFont();
            // Recompute AFTER the field applied this frame's edit: the spoken
            // string, the "N of 6" hint and Join enablement must all read the
            // SAME post-edit count, or the a11y announcer speaks a stale count
            // this frame and re-utters the corrected one the next.
            const std::size_t typed = std::strlen(g_online.betaJoinCode);
            const bool ready = typed == 6u;
            char spoken[64];
            if (typed == 0u) {
                std::snprintf(spoken, sizeof(spoken), "No digits entered yet");
            } else {
                std::snprintf(spoken, sizeof(spoken), "%s, %u of 6",
                              g_online.betaJoinCode, static_cast<unsigned>(typed));
            }
            ui::SpeakFocusedItem(
                "Race code", spoken,
                "Type or paste the 6 digits your host reads to you.");
            // Fixed "N of 6" line -- rendered when complete too ("6 of 6 — ready
            // to join") rather than collapsing, so the Join button below never
            // shifts under the cursor the instant the sixth digit lands.
            if (ready) {
                ui::TextSubtle("6 of 6 — ready to join");
            } else {
                ui::TextSubtle("%u of 6 — type or paste your host's code",
                               static_cast<unsigned>(typed));
            }
            ui::Gap(ui::kGapS);
            if (ready) {
                if (ui::BrandPrimaryButton("Join", ui::kBtnFullWidth())) {
                    buildBetaLiveAdapter(state, MDKR_ONLINE_JOURNEY_JOIN,
                                         std::string(g_online.betaJoinCode));
                }
            } else {
                // A dimmed NEUTRAL slab, not a dimmed gold CTA: a disabled gold
                // button still reads as the button to press. This makes the
                // not-yet-pressable state visibly different from the gold it
                // becomes once six digits are in. BeginDisabled also drops it
                // from keyboard/gamepad focus.
                ImGui::BeginDisabled();
                ImGui::Button("Join", ui::kBtnFullWidth());
                ImGui::EndDisabled();
            }
            ui::SpeakFocusedItem("Join", ready ? "Ready" : "Enter 6 digits",
                                 "Joins the room your host created.");
            ui::Gap(ui::kGapS);
            if (ImGui::Button("Back", ui::kBtnFullWidth())) {
                g_online.betaStage = OnlineRoomUiState::BetaStage::Chooser;
            }
        }
        ui::CardEnd();
    }
}

// ===========================================================================
// AUTOPAIR (test-only): drive ONLY the interactive pairing steps.
//
// It synthesizes the SAME UI actions a player would take to pair -- the
// create/join chooser choice, the 6-digit join code, and the Words-Match
// confirmation -- then goes hands-off. Everything after pairing (the room-ready
// takeover, the native online screens, the race, the chooser, the return, the
// re-take) is left to the PRODUCTION code path to drive itself; this function
// never touches takeover/boot/selection logic. It exists so the two-process
// cloud capstone lane can bootstrap pairing without a human. Inert unless
// MDKR_APP_TEST_ONLINE_AUTOPAIR is set (create|join); compiled only under the
// beta build. It calls buildBetaLiveAdapter / dispatch(CONFIRM_PHRASE) -- the
// exact functions the "Host a Race" / "Join" / "Confirm" buttons call.
void autopairService(LauncherState &state) {
    static int resolved = -1;   // -1 unresolved, 0 off, 1 create, 2 join
    static std::string joinCode;
    static int tournamentCup = -1;  // -1 single race; >=0 tournament with that cup
    if (resolved < 0) {
        const char *role = std::getenv("MDKR_APP_TEST_ONLINE_AUTOPAIR");
        if (role == nullptr || role[0] == '\0') {
            resolved = 0;
        } else if (std::strcmp(role, "create") == 0) {
            resolved = 1;
        } else if (std::strcmp(role, "join") == 0) {
            resolved = 2;
            const char *code = std::getenv("MDKR_APP_TEST_ONLINE_JOIN_CODE");
            joinCode = (code != nullptr) ? code : "";
        } else {
            std::fprintf(stderr,
                         "[online-autopair] invalid MDKR_APP_TEST_ONLINE_AUTOPAIR="
                         "%s (expected create|join)\n", role);
            resolved = 0;
        }
        if (resolved == 2 && joinCode.size() != 6u) {
            std::fprintf(stderr,
                         "[online-autopair] role=join requires a 6-digit "
                         "MDKR_APP_TEST_ONLINE_JOIN_CODE (got %zu chars)\n",
                         joinCode.size());
            resolved = 0;
        }
        // Optional TOURNAMENT capstone: the host configures the room as a
        // tournament (mode + cup) BEFORE Check Setup, so the native takeover
        // fires on a tournament room -- the SAME pre-config shape the native
        // TRACKSELECT tournament path reads from the forward feed. Set on BOTH
        // processes so each waits for the mode to propagate before Check Setup.
        // A single race can never reach the FINISHED session-end the capstone's
        // clean-return + re-take assertions need; a tournament finals on its
        // last cup round. Value = cup id (0..4).
        if (const char *cupEnv =
                std::getenv("MDKR_APP_TEST_ONLINE_AUTOPAIR_TOURNAMENT")) {
            if (cupEnv[0] != '\0') {
                tournamentCup = std::atoi(cupEnv);
                if (tournamentCup < 0 || tournamentCup > 4) tournamentCup = 1;
            }
        }
    }
    if (resolved <= 0) return;

    // Step 1: the ROM. The Online Room tab is reachable without a validated ROM,
    // and this panel (unlike RomPanel_draw) never services validation, so drive
    // the remembered-ROM validation here until it passes -- exactly what a player
    // who validated their ROM on the Play panel first would have done. No adapter
    // can be built until betaValidatedRomRevision succeeds (its own gate).
    if (betaValidatedRomRevision(state.romInfo) == 0u) {
        RomPanel_ensureInit(state);
        RomPanel_serviceValidation(state);
        static bool waitedRomLogged = false;
        if (!waitedRomLogged) {
            waitedRomLogged = true;
            std::fprintf(stderr,
                         "[online-autopair] waiting for the remembered ROM to "
                         "validate before pairing\n");
        }
        return;
    }

    // Step 2: the chooser choice (create or join).
    if (!g_online.adapter || !g_online.initialized) {
        const MdkrOnlineJourney journey =
            resolved == 1 ? MDKR_ONLINE_JOURNEY_CREATE : MDKR_ONLINE_JOURNEY_JOIN;
        if (resolved == 2) {
            std::snprintf(g_online.betaJoinCode, sizeof(g_online.betaJoinCode),
                          "%s", joinCode.c_str());
        }
        std::fprintf(stderr, "[online-autopair] role=%s dispatching %s\n",
                     resolved == 1 ? "create" : "join",
                     resolved == 1 ? "CREATE_ROOM" : "JOIN_ROOM");
        if (!buildBetaLiveAdapter(state, journey,
                                  resolved == 2 ? joinCode : std::string())) {
            static bool buildFailLogged = false;
            if (!buildFailLogged) {
                buildFailLogged = true;
                std::fprintf(stderr,
                             "[online-autopair] buildBetaLiveAdapter refused "
                             "(reason: %s)\n",
                             g_online.betaBuildFailedReason[0] != '\0'
                                 ? g_online.betaBuildFailedReason
                                 : "service unreachable / no provenance");
            }
        }
        return;
    }

    // Step 3+: surface the invite code (create), then confirm the safety phrase
    // exactly once when PREFLIGHT fronts it. After that, hands-off -- the
    // production room-ready poll (drawBetaRoom, every frame) fires the takeover.
    MdkrOnlineViewModel model{};
    if (!g_online.adapter->view(&model)) return;

    if (resolved == 1) {
        static bool codeLogged = false;
        if (!codeLogged) {
            std::string code;
            std::string url;
            if (OnlineRoom_liveInvite(g_online.adapter.get(), &code, &url) &&
                code.size() == 6u) {
                codeLogged = true;
                std::fprintf(stderr, "[online-autopair] code=%s\n", code.c_str());
            }
        }
    }

    static bool membersLogged = false;
    if (!membersLogged && model.member_count >= 2u) {
        membersLogged = true;
        std::fprintf(stderr, "[online-autopair] members=2\n");
    }

    // Step 3b (tournament capstone only): the leader configures the room as a
    // tournament + cup while both are in the open lobby, so the takeover fires on
    // a tournament room. Leader-gated helpers (a joiner submit is a no-op).
    MdkrOnlineLobby autopairLobby{};
    const bool haveAutopairLobby =
        mdkr_online_live_adapter_lobby(g_online.adapter.get(), &autopairLobby);
    static bool tournamentConfigured = false;
    if (tournamentCup >= 0 && !tournamentConfigured && resolved == 1 &&
        model.member_count >= 2u) {
        tournamentConfigured = true;
        std::fprintf(stderr,
                     "[online-autopair] configuring TOURNAMENT cup=%d\n",
                     tournamentCup);
        mdkr_online_live_adapter_set_mode(g_online.adapter.get(),
                                          MDKR_ONLINE_MODE_TOURNAMENT);
        mdkr_online_live_adapter_set_cup(
            g_online.adapter.get(), static_cast<unsigned>(tournamentCup));
    }
    // A tournament capstone must not proceed until the room mode has propagated
    // to THIS endpoint (the leader's SET_MODE/SET_CUP reaches both via the feed),
    // so the native takeover lands on a tournament room on both sides.
    const bool tournamentReady =
        tournamentCup < 0 ||
        (haveAutopairLobby &&
         autopairLobby.mode == MDKR_ONLINE_MODE_TOURNAMENT &&
         autopairLobby.cup_id == static_cast<std::uint8_t>(tournamentCup));

    // Step 4: "Check Setup" -- the secure-handshake step that brings up the mesh
    // and computes the safety phrase. It is the ROOM view's primary action (the
    // "Check Setup" button); both endpoints press it. Without it the room sits in
    // the open lobby and the verification phrase never appears.
    static bool checkSetupDispatched = false;
    if (!checkSetupDispatched && model.kind == MDKR_ONLINE_VIEW_ROOM &&
        model.member_count >= 2u && tournamentReady &&
        model.primary.action == MDKR_ONLINE_VIEW_ACTION_CHECK_SETUP) {
        checkSetupDispatched = true;
        std::fprintf(stderr,
                     tournamentCup >= 0
                         ? "[online-autopair] tournament room ready; dispatching "
                           "CHECK_SETUP\n"
                         : "[online-autopair] dispatching CHECK_SETUP\n");
        dispatch(MDKR_ONLINE_VIEW_ACTION_CHECK_SETUP);
    }

    static bool phraseConfirmed = false;
    if (!phraseConfirmed && model.kind == MDKR_ONLINE_VIEW_PREFLIGHT &&
        model.verification_phrase[0] != '\0') {
        std::fprintf(stderr, "[online-autopair] phrase=%s\n",
                     model.verification_phrase);
        dispatch(MDKR_ONLINE_VIEW_ACTION_CONFIRM_PHRASE);
        phraseConfirmed = true;
        std::fprintf(stderr,
                     "[online-autopair] confirm dispatched -- handing off to the "
                     "production room-ready takeover\n");
    }

    static bool selectingLogged = false;
    if (!selectingLogged && model.kind == MDKR_ONLINE_VIEW_SELECTING) {
        selectingLogged = true;
        std::fprintf(stderr,
                     "[online-autopair] SELECTING reached -- pairing complete, "
                     "hands-off\n");
    }
}

const char *betaStatusLine(const MdkrOnlineViewModel &model) {
    switch (model.kind) {
    case MDKR_ONLINE_VIEW_ENTRY: return "Getting ready…";
    case MDKR_ONLINE_VIEW_CONNECTING: return "Connecting…";
    case MDKR_ONLINE_VIEW_ROOM:
        return model.member_count >= 2u ? "Connected — both players are here"
                                        : "Waiting for the other player…";
    case MDKR_ONLINE_VIEW_PREFLIGHT:
        return model.verification_phrase[0] != '\0'
                   ? "Almost there — confirm the safety phrase"
                   : "Checking setup…";
    case MDKR_ONLINE_VIEW_SELECTING: return "Connected — choose your racer";
    case MDKR_ONLINE_VIEW_LOADING: return "Loading the race…";
    case MDKR_ONLINE_VIEW_COUNTDOWN: return "Get ready!";
    case MDKR_ONLINE_VIEW_RACING: return "Racing";
    case MDKR_ONLINE_VIEW_RESULTS: return "Race complete";
    case MDKR_ONLINE_VIEW_RECOVERY:
        /* The top status line must AGREE with the specific failure body it
         * sits above -- the section-header title (model.title, e.g. "Words Did
         * Not Match") and the failure sentence (betaFailureCopy) -- never
         * contradict them with a generic "Lost connection". Each recovery
         * reason gets a matching short status; anything unmapped keeps the
         * honest generic retry line. */
        switch (model.failure) {
        case MDKR_ONLINE_VIEW_FAILURE_INVITE_EXPIRED:
        case MDKR_ONLINE_VIEW_FAILURE_INVITE_ROTATED:
            return "That invite expired — get a fresh code";
        case MDKR_ONLINE_VIEW_FAILURE_ROOM_FULL:
            return "That room is full";
        case MDKR_ONLINE_VIEW_FAILURE_SERVICE_UNAVAILABLE:
        case MDKR_ONLINE_VIEW_FAILURE_SERVICE_BUDGET_SAFE:
            return "Service unavailable — try again shortly";
        case MDKR_ONLINE_VIEW_FAILURE_DIFFERENT_BUILD:
        case MDKR_ONLINE_VIEW_FAILURE_UPDATE_REQUIRED:
            return "Game versions differ — update to match";
        case MDKR_ONLINE_VIEW_FAILURE_DIFFERENT_ROM:
            return "ROMs differ — use the same ROM";
        case MDKR_ONLINE_VIEW_FAILURE_DIFFERENT_SETTINGS:
            return "Settings differ — use the room's settings";
        case MDKR_ONLINE_VIEW_FAILURE_CONTROLLER_NEEDED:
            return "Controller needed to race online";
        case MDKR_ONLINE_VIEW_FAILURE_CONNECTION_CHECK:
        case MDKR_ONLINE_VIEW_FAILURE_RELAY_CAPACITY:
        case MDKR_ONLINE_VIEW_FAILURE_NETWORKS_CANNOT_CONNECT:
            return "Couldn't connect — check both networks";
        case MDKR_ONLINE_VIEW_FAILURE_HOST_CLOSED:
            return "The host closed the room";
        case MDKR_ONLINE_VIEW_FAILURE_ROOM_EXPIRED:
            return "The room expired";
        case MDKR_ONLINE_VIEW_FAILURE_ENGINE_FAILED:
        case MDKR_ONLINE_VIEW_FAILURE_EPOCH_MISMATCH:
            return "The race couldn't start — try again";
        case MDKR_ONLINE_VIEW_FAILURE_VERIFICATION_MISMATCH:
            return "Safety words didn't match — reconnect";
#if MDKR_ENABLE_ONLINE_BETA
        case MDKR_ONLINE_VIEW_FAILURE_OPPONENT_LEFT:
            return "Opponent disconnected — this room is done";
        case MDKR_ONLINE_VIEW_FAILURE_OPPONENT_NEVER_STARTED:
            return "Opponent couldn't start — create a fresh invite";
        case MDKR_ONLINE_VIEW_FAILURE_CONNECTION_UNPLAYABLE:
            return "Connection became unplayable — this room is done";
#endif
        default:
            return "Lost connection — you can retry";
        }
    default: return "Online race";
    }
}

// Bounded, honest failure copy: every entry is a plain sentence, no raw wire
// codes, and each recovery view already carries a clear action (Retry / Enter
// another code / Leave), so there is never an infinite spinner.
const char *betaFailureCopy(MdkrOnlineViewFailure failure) {
    switch (failure) {
    case MDKR_ONLINE_VIEW_FAILURE_INVITE_EXPIRED:
    case MDKR_ONLINE_VIEW_FAILURE_INVITE_ROTATED:
        return "That invite expired. Ask the host for a fresh code.";
    case MDKR_ONLINE_VIEW_FAILURE_ROOM_FULL:
        return "That room is already full — the online beta is 2 players.";
    case MDKR_ONLINE_VIEW_FAILURE_SERVICE_UNAVAILABLE:
    case MDKR_ONLINE_VIEW_FAILURE_SERVICE_BUDGET_SAFE:
        return "The matchmaking service is unavailable right now. Try again shortly.";
    // The four compatibility families each name their own fix, aligned with the
    // view-model's per-family primary action (UPDATE_GAME for build/update,
    // CHOOSE_ROM for the ROM, USE_ROOM_SETTINGS for gameplay settings), so the
    // status sentence and the recovery button never disagree.
    case MDKR_ONLINE_VIEW_FAILURE_DIFFERENT_BUILD:
        return "You're on different game versions — both computers need the "
               "same build.";
    case MDKR_ONLINE_VIEW_FAILURE_DIFFERENT_ROM:
        return "You're using different game ROMs — use the same ROM file on "
               "both computers.";
    case MDKR_ONLINE_VIEW_FAILURE_DIFFERENT_SETTINGS:
        return "Your gameplay settings differ — switch to the room's settings "
               "to race.";
    case MDKR_ONLINE_VIEW_FAILURE_UPDATE_REQUIRED:
        return "This build is too old to join — update to the newer build.";
    case MDKR_ONLINE_VIEW_FAILURE_CONTROLLER_NEEDED:
        return "Set up a controller before racing online.";
    case MDKR_ONLINE_VIEW_FAILURE_CONNECTION_CHECK:
    case MDKR_ONLINE_VIEW_FAILURE_RELAY_CAPACITY:
    case MDKR_ONLINE_VIEW_FAILURE_NETWORKS_CANNOT_CONNECT:
        return "Couldn't connect directly. Check both networks and retry from a "
               "fresh invite.";
    case MDKR_ONLINE_VIEW_FAILURE_HOST_CLOSED:
        return "The host closed the room. Create a room of your own, or ask "
               "them for a fresh code.";
    case MDKR_ONLINE_VIEW_FAILURE_ROOM_EXPIRED:
        return "The room expired. Create or join a new one.";
    case MDKR_ONLINE_VIEW_FAILURE_ENGINE_FAILED:
    case MDKR_ONLINE_VIEW_FAILURE_EPOCH_MISMATCH:
        return "The race couldn't start. Leave and try again.";
    case MDKR_ONLINE_VIEW_FAILURE_VERIFICATION_MISMATCH:
        return "The safety phrases didn't match — stopped for your protection. "
               "Leave and reconnect.";
#if MDKR_ENABLE_ONLINE_BETA
    case MDKR_ONLINE_VIEW_FAILURE_OPPONENT_LEFT:
        return "Your opponent lost connection, so this race ended. This room is "
               "done — create or join a new one.";
    case MDKR_ONLINE_VIEW_FAILURE_OPPONENT_NEVER_STARTED:
        return "The race was canceled before it began. Create a fresh invite "
               "and try again.";
    case MDKR_ONLINE_VIEW_FAILURE_CONNECTION_UNPLAYABLE:
        return "The connection degraded past recovery mid-race, so this race "
               "ended. This room is done — create or join a new one.";
#endif
    default:
        return "The connection was interrupted. You can retry or leave.";
    }
}

// Compose the never-silent one-line status for lobby-backed screens: every
// state names itself and the next actor. Falls back to the static per-kind
// line when no lobby snapshot exists (chooser, connecting, fake preview).
void betaComposeStatusLine(const MdkrOnlineViewModel &model,
                           const MdkrOnlineLobby *lobby, char *out,
                           std::size_t size) {
    std::snprintf(out, size, "%s", betaStatusLine(model));
    if (lobby == nullptr) return;
    const bool tournament = lobby->mode == MDKR_ONLINE_MODE_TOURNAMENT &&
                            lobby->cup_id != MDKR_ONLINE_NO_CUP;
    char series[96] = {0};
    if (tournament) {
        const char *cup = mdkr_online_cup_name(lobby->cup_id);
        std::snprintf(series, sizeof(series), "Race %u of %u — %s",
                      static_cast<unsigned>(lobby->race_index) + 1u,
                      static_cast<unsigned>(MDKR_ONLINE_CUP_ROUNDS),
                      cup != nullptr ? cup : "Trophy Tournament");
    }
    switch (model.kind) {
    case MDKR_ONLINE_VIEW_SELECTING: {
        // The game owns racer/vehicle/track select and readiness after pairing, so
        // the strip is just the pre-hand-off "Room ready" (with the tournament
        // series prefix when present) -- not the retired launcher Ready/Start copy.
        const char *next = "Room ready";
        if (series[0] != '\0') {
            std::snprintf(out, size, "%s. %s", series, next);
        } else {
            std::snprintf(out, size, "%s", next);
        }
        break;
    }
    case MDKR_ONLINE_VIEW_LOADING:
    case MDKR_ONLINE_VIEW_COUNTDOWN:
        if (series[0] != '\0') {
            std::snprintf(out, size, "%s — %s", series,
                          model.kind == MDKR_ONLINE_VIEW_COUNTDOWN
                              ? "get ready!" : "loading the race…");
        }
        break;
    case MDKR_ONLINE_VIEW_RESULTS:
        if (tournament) {
            if (lobby->race_index >= MDKR_ONLINE_CUP_ROUNDS - 1u) {
                const char *cup = mdkr_online_cup_name(lobby->cup_id);
                std::snprintf(out, size, "%s complete — the trophy is decided",
                              cup != nullptr ? cup : "Tournament");
            } else {
                std::snprintf(out, size,
                              "Race %u of %u results — %s",
                              static_cast<unsigned>(lobby->race_index) + 1u,
                              static_cast<unsigned>(MDKR_ONLINE_CUP_ROUNDS),
                              model.local_member_is_leader
                                  ? "press Next Race to continue"
                                  : "waiting for the host to start the next race");
            }
        } else if (!model.local_member_is_leader) {
            std::snprintf(out, size,
                          "Race complete — waiting for the host to choose "
                          "what's next");
        }
        break;
    default:
        break;
    }
}

// A left-to-right breadcrumb of the boot-ramp phases the view model actually
// surfaces (CONNECTING -> ROOM -> PREFLIGHT -> SELECTING): connect to the room,
// confirm the safety phrase, then pick racers. Drawn only while the room is
// still on that ramp; the active phase is the accent color and the rest stay
// subtle. No transport state is invented -- every crumb IS a real view kind the
// adapter already reports, so this only surfaces progress the model knows.
void drawBetaBootRampBreadcrumb(const MdkrOnlineViewModel &model) {
    struct Crumb { MdkrOnlineViewKind kind; const char *label; };
    static const Crumb kCrumbs[] = {
        {MDKR_ONLINE_VIEW_CONNECTING, "Connect"},
        {MDKR_ONLINE_VIEW_ROOM, "Room"},
        {MDKR_ONLINE_VIEW_PREFLIGHT, "Safety Check"},
        {MDKR_ONLINE_VIEW_SELECTING, "In Game"},
    };
    constexpr unsigned kCount = sizeof(kCrumbs) / sizeof(kCrumbs[0]);
    unsigned active = kCount;
    for (unsigned i = 0u; i < kCount; ++i) {
        if (kCrumbs[i].kind == model.kind) active = i;
    }
    if (active == kCount) return;  // not on the boot ramp
    for (unsigned i = 0u; i < kCount; ++i) {
        if (i != 0u) {
            ImGui::SameLine(0.0f, 0.0f);
            ImGui::TextDisabled("  ·  ");
            ImGui::SameLine(0.0f, 0.0f);
        }
        if (i == active) {
            ImGui::PushStyleColor(ImGuiCol_Text, AppTheme::accent());
            ImGui::TextUnformatted(kCrumbs[i].label);
            ImGui::PopStyleColor();
        } else {
            ImGui::TextDisabled("%s", kCrumbs[i].label);
        }
    }
}

void drawBetaStatusLine(const MdkrOnlineViewModel &model,
                        const MdkrOnlineLobby *lobby) {
    if (ui::CardBegin("##beta-status-line", AppTheme::brandSky(), 0.0f)) {
        char line[192];
        betaComposeStatusLine(model, lobby, line, sizeof(line));
        // Motion for every waiting line: any status that ends in "…" animates.
        betaAnimateEllipsis(line, sizeof(line));
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(line);
        ImGui::PopTextWrapPos();
        if (model.kind == MDKR_ONLINE_VIEW_RECOVERY ||
            model.failure != MDKR_ONLINE_VIEW_FAILURE_NONE) {
            ui::TextSubtleWrapped("%s", betaFailureCopy(model.failure));
        } else {
            drawBetaBootRampBreadcrumb(model);
            // The game owns readiness after pairing, so the strip drops the
            // launcher-era ready counter; the member count stays.
            ui::TextSubtle("%u of 2 players", model.member_count);
        }
    }
    ui::CardEnd();
}

// The white margin drawn around the QR modules -- its quiet zone. Published so
// the pre-code invite placeholder can reserve the same footprint the real QR
// fills, and both the code grid and the placeholder stay in step.
float betaQrMargin() { return 10.0f * AppTheme::uiScale(); }

// Draw-only QR of an arbitrary string, mirroring the phone-party invite QR. The
// code sits on a padded white panel (its quiet zone) with a short caption, so it
// reads cleanly on the dark surface and scans reliably.
void drawBetaQr(const std::string &text) {
    if (text.empty()) return;
    try {
        const qrcodegen::QrCode qr = qrcodegen::QrCode::encodeText(
            text.c_str(), qrcodegen::QrCode::Ecc::QUARTILE);
        // Sized so the padded panel + caption keep the whole invite card on one
        // 960x720 screen (the quiet-zone margin and caption cost the height the
        // 220px code used to take).
        const float maxSize = 150.0f * AppTheme::uiScale();
        const float margin = betaQrMargin();
        const float available = ImGui::GetContentRegionAvail().x;
        const float size = (std::max)(
            96.0f, (std::min)(maxSize, available - margin * 2.0f));
        const int quiet = 4;
        const int modules = qr.getSize() + quiet * 2;
        const float pixel = std::floor(size / static_cast<float>(modules));
        const float actual = pixel * static_cast<float>(modules);
        const float panel = actual + margin * 2.0f;
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        ImDrawList *draw = ImGui::GetWindowDrawList();
        const ImU32 light = ImGui::GetColorU32(AppTheme::qrLight());
        const ImU32 dark = ImGui::GetColorU32(AppTheme::qrDark());
        // White quiet-zone panel behind the code (the margin IS the quiet zone a
        // reader needs), rounded so it reads as a tidy card, not a bare block.
        draw->AddRectFilled(origin, ImVec2(origin.x + panel, origin.y + panel),
                            light, 6.0f * AppTheme::uiScale());
        for (int y = 0; y < qr.getSize(); ++y) {
            for (int x = 0; x < qr.getSize(); ++x) {
                if (!qr.getModule(x, y)) continue;
                const float left = origin.x + margin + (x + quiet) * pixel;
                const float top = origin.y + margin + (y + quiet) * pixel;
                draw->AddRectFilled(ImVec2(left, top),
                                    ImVec2(left + pixel, top + pixel), dark);
            }
        }
        ImGui::Dummy(ImVec2(panel, panel));
        ui::Gap(ui::kGapXS);
        ui::TextSubtle("Scan to join");
    } catch (...) {
        ui::TextSubtleWrapped(
            "The QR code could not be shown. Share the 6-digit code instead.");
    }
}

// Begin a bordered card capped at `maxWidth` and centered in the available
// content region, so a wide window no longer leaves a dead right column beside a
// card whose content (code + QR) is naturally narrow. Falls back to full width
// when the region is narrower than the cap. Pair with ui::CardEnd().
bool betaCenteredCardBegin(const char *id, const ImVec4 &border,
                           float maxWidth) {
    const float avail = ImGui::GetContentRegionAvail().x;
    const float width = (std::min)(maxWidth, avail);
    if (avail > width) {
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail - width) * 0.5f);
    }
    ImGui::PushStyleColor(ImGuiCol_Border,
                          ImVec4(border.x, border.y, border.z, 0.55f));
    return ImGui::BeginChild(
        id, ImVec2(width, 0.0f),
        ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY);
}

// The creator's invite card: big shareable code + Copy + a QR of the invite
// link. A joiner (isHost false) renders nothing. The host ALWAYS renders the
// same card frame -- through the CONNECTING create round trip AND the open ROOM,
// before the fallback code has been learned, it shows an animated "Getting your
// room code…" placeholder, then swaps in the real code + QR + Copy buttons the
// instant OnlineRoom_liveInvite reports ready (which can be mid-CONNECTING,
// before the Ready event advances the view to ROOM). The placeholder reserves
// the Copy-row + QR footprint the real card will fill, so the swap-in never
// jumps the layout. This is why the host is never left staring at a bare
// "Connecting…" with nothing to share or anticipate.
void drawBetaInviteCard(bool isHost) {
    std::string code;
    std::string url;
    bool ready;
    if (g_betaFakeInvite.active) {
        // Test-only synthetic invite (drawBetaRoomFake): render the REAL code +
        // Copy + QR card without a live transport. Never taken on the live path.
        code = g_betaFakeInvite.code;
        url = g_betaFakeInvite.url;
        ready = !code.empty();
    } else {
        ready = OnlineRoom_liveInvite(g_online.adapter.get(), &code, &url) &&
                !code.empty();
    }
    // Cap and center the card: at wide sizes a full-width card leaves a dead
    // right column beside the naturally-narrow code + QR. Both the placeholder
    // and the real card use the SAME cap so the swap-in never shifts sideways.
    const float kInviteMaxWidth = 480.0f * AppTheme::uiScale();
    if (!ready) {
        if (!isHost) return;  // a joiner has no room of its own to share
        ui::Gap(ui::kGapM);
        if (betaCenteredCardBegin("##beta-invite", AppTheme::accent(),
                                  kInviteMaxWidth)) {
            ImGui::TextUnformatted("Invite a Friend");
            ui::TextSubtleWrapped(
                "Your private room code is on its way — share it the moment it "
                "appears.");
            ui::Gap(ui::kGapS);
            // One animation form only: a "…"-terminated title run through the
            // same betaAnimateEllipsis the status line uses, so the cycling dots
            // are consistent everywhere (no second hand-rolled dot form).
            char waiting[48];
            std::snprintf(waiting, sizeof(waiting), "Getting your room code%s",
                          "\xE2\x80\xA6");  // U+2026, animated below
            betaAnimateEllipsis(waiting, sizeof(waiting));
            ImGui::PushFont(AppTheme::fonts().title);
            ImGui::TextUnformatted(waiting);
            ImGui::PopFont();
            // Reserve the exact blocks the real card fills (Copy button row +
            // the padded QR panel + its caption line), computed the same way
            // drawBetaQr sizes them, so the placeholder->real swap does not
            // shift the layout under the host's cursor.
            ui::Gap(ui::kGapS);
            ImGui::Dummy(ImVec2(0.0f, ui::kBtnSecondary().y));
            ui::Gap(ui::kGapS);
            const float qrMax = 150.0f * AppTheme::uiScale();
            const float margin = betaQrMargin();
            const float qrSide = (std::max)(
                96.0f, (std::min)(qrMax,
                                  ImGui::GetContentRegionAvail().x - margin * 2.0f));
            ImGui::Dummy(ImVec2(qrSide + margin * 2.0f, qrSide + margin * 2.0f));
            ui::Gap(ui::kGapXS);
            ImGui::Dummy(ImVec2(0.0f, ImGui::GetTextLineHeight()));  // caption line
            ui::TextSubtleWrapped(
                "Invite-only and expires. Keep this window open — the code and "
                "its QR appear here in a moment.");
        }
        ui::CardEnd();
        return;
    }
    ui::Gap(ui::kGapM);
    if (betaCenteredCardBegin("##beta-invite", AppTheme::accent(),
                              kInviteMaxWidth)) {
        ImGui::TextUnformatted("Invite a Friend");
        ui::TextSubtleWrapped(
            "Share this code. Your friend picks \"Join a Race\" and types it in.");
        ui::Gap(ui::kGapS);
        std::string grouped = code;
        if (code.size() == 6u) {
            grouped = code.substr(0, 3) + " " + code.substr(3);
        }
        ImGui::PushFont(AppTheme::fonts().title);
        ImGui::TextUnformatted(grouped.c_str());
        ImGui::PopFont();
        ui::Gap(ui::kGapS);
        if (ImGui::Button("Copy Code", ui::kBtnSecondary())) {
            ImGui::SetClipboardText(code.c_str());
        }
        ui::SpeakFocusedItem("Copy Code", grouped.c_str(),
                             "Copies the 6-digit race code to the clipboard.");
        if (!url.empty()) {
            ImGui::SameLine();
            if (ImGui::Button("Copy Link", ui::kBtnSecondary())) {
                ImGui::SetClipboardText(url.c_str());
            }
            ui::SpeakFocusedItem("Copy Link", "Invite link",
                                 "Copies the full invite link to the clipboard.");
        }
        ui::Gap(ui::kGapS);
        std::string qrTarget = url;
#ifdef MDKR_PARTY_ORIGIN
        if (!qrTarget.empty() && qrTarget[0] == '/') {
            qrTarget = std::string(MDKR_PARTY_ORIGIN) + qrTarget;
        }
#endif
        if (qrTarget.empty()) qrTarget = code;
        drawBetaQr(qrTarget);
        ui::TextSubtleWrapped(
            "Invite-only and expires. Keep this window open until your friend "
            "joins.");
    }
    ui::CardEnd();
}

// The secure-phrase barrier as a prominent, side-by-side decision. The two
// buttons dispatch the SAME CONFIRM_PHRASE / REPORT_PHRASE_MISMATCH actions the
// view model already exposes; the SAS logic behind them is untouched.
void drawBetaPhraseDecision(const MdkrOnlineViewModel &model,
                            LauncherState &state) {
    ui::Gap(ui::kGapM);
    if (ui::CardBegin("##beta-phrase", AppTheme::accent(), 0.0f)) {
        // The "Compare These Words" title is the SectionHeader above this card;
        // it is not repeated here -- the card opens straight into the how-to.
        ui::TextSubtleWrapped(
            "Read the words aloud with your friend. They must match exactly on "
            "both screens — this is what keeps your connection private.");
        ui::Gap(ui::kGapS);
        ImGui::PushFont(AppTheme::fonts().title);
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(model.verification_phrase);
        ImGui::PopTextWrapPos();
        ImGui::PopFont();
        ui::Gap(ui::kGapM);
        const float full = ImGui::GetContentRegionAvail().x;
        const float half = (full - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
        const float height = ui::kBtnPrimary().y;
        if (model.primary.visible && model.primary.label != nullptr) {
            if (ui::BrandPrimaryButton(model.primary.label,
                                       ImVec2(half, height))) {
                handleAction(model.primary.action, state);
            }
            ui::SpeakFocusedItem(model.primary.label, "Confirm",
                                 "Confirms every word matches on both screens.");
        }
        if (model.secondary.visible && model.secondary.label != nullptr) {
            ImGui::SameLine();
            if (ImGui::Button(model.secondary.label, ImVec2(half, height))) {
                handleAction(model.secondary.action, state);
            }
            ui::SpeakFocusedItem(model.secondary.label, "Stop",
                                 "Use this if even one word is different.");
        }
        ui::Gap(ui::kGapS);
        ui::TextSubtleWrapped(
            "If even one word is different, choose \"Words Differ\" and reconnect.");
    }
    ui::CardEnd();

    // Direct exit during the compare: the view model's cancel ("Leave
    // Room") was previously undrawn on this screen, so leaving mid-compare meant
    // routing through Words Differ -> recovery -> Leave. Render it as a small,
    // tertiary control below the card so it never competes with the prominent
    // Words Match / Words Differ decision.
    if (model.cancel.visible && model.cancel.label != nullptr) {
        ui::Gap(ui::kGapS);
        if (!model.cancel.enabled) ImGui::BeginDisabled();
        if (ImGui::Button(model.cancel.label,
                          ImVec2(ui::kControlWidth(), 0.0f)) &&
            model.cancel.enabled) {
            handleAction(model.cancel.action, state);
        }
        ui::SpeakFocusedItem(model.cancel.label, "Leave",
                             "Leaves the room without comparing the words.");
        if (!model.cancel.enabled) ImGui::EndDisabled();
    }
}

// A purely decorative per-racer accent -- a color standing in for a portrait.
// Decoded ROM character-select art is not reachable here: the launcher runs
// its own ImGui shell before the emulated engine ever boots, and nothing in
// this codebase decodes N64 character-portrait textures outside the booted
// engine's own renderer (the in-engine character-select coverage --
// tests/check_taj_character_select.py and friends -- drives the BOOTED game,
// not this launcher panel). A name grid with a per-racer accent color reads
// clearly without that dependency and beats blocking the grid on unavailable
// art.
constexpr unsigned kCharacterAccentHex[] = {
    0xC98A4Bu,  // Diddy   -- warm tan
    0xE07B39u,  // Timber  -- tiger orange
    0xE0629Cu,  // Pipsy   -- pink
    0x4CAF6Du,  // Tiptup  -- shell green
    0xA8562Eu,  // Conker  -- chestnut brown
    0x8B6FB3u,  // Bumper  -- lavender
    0xC79A3Eu,  // Banjo   -- honey gold
    0x5C8A3Fu,  // Krunch  -- kremling green
    0xC94F4Fu,  // Drumstick -- rooster red
    0x3E7CB3u,  // T.T.    -- stopwatch blue
};
static_assert(sizeof(kCharacterAccentHex) / sizeof(kCharacterAccentHex[0]) ==
                 sizeof(kCharacters) / sizeof(kCharacters[0]),
             "one decorative accent per base racer");

// ---- Lobby-snapshot helpers (2-player beta: one seat per endpoint) --------

uint64_t betaLocalEndpoint(const MdkrOnlineLobby &lobby, bool localIsLeader) {
    if (localIsLeader) return lobby.leader_endpoint_id;
    for (unsigned i = 0u; i < MDKR_ONLINE_MAX_ENDPOINTS; ++i) {
        const MdkrOnlineMember &member = lobby.members[i];
        if (member.occupied &&
            member.endpoint_id != lobby.leader_endpoint_id) {
            return member.endpoint_id;
        }
    }
    return 0u;
}

const MdkrOnlineMember *betaMemberFor(const MdkrOnlineLobby &lobby,
                                      std::uint64_t endpoint) {
    if (endpoint == 0u) return nullptr;
    for (unsigned i = 0u; i < MDKR_ONLINE_MAX_ENDPOINTS; ++i) {
        if (lobby.members[i].occupied &&
            lobby.members[i].endpoint_id == endpoint) {
            return &lobby.members[i];
        }
    }
    return nullptr;
}

const char *betaPlacementLabel(unsigned placement) {
    switch (placement) {
    case 0u: return "1st";
    case 1u: return "2nd";
    case 2u: return "3rd";
    case 3u: return "4th";
    case 4u: return "5th";
    case 5u: return "6th";
    case 6u: return "7th";
    case 7u: return "8th";
    default: return "—";
    }
}

// A small drawn crown for the room leader (host) -- drawn geometry rather
// than a glyph so the marker never depends on font coverage.
void drawBetaCrown(ImDrawList *draw, const ImVec2 &pos, float height,
                   ImU32 color) {
    const float w = height * 1.35f;
    const float baseTop = pos.y + height * 0.62f;
    const float baseBottom = pos.y + height;
    draw->AddRectFilled(ImVec2(pos.x, baseTop), ImVec2(pos.x + w, baseBottom),
                        color);
    const float third = w / 3.0f;
    for (unsigned spike = 0u; spike < 3u; ++spike) {
        const float left = pos.x + third * static_cast<float>(spike);
        draw->AddTriangleFilled(
            ImVec2(left, baseTop), ImVec2(left + third, baseTop),
            ImVec2(left + third * 0.5f,
                   pos.y + (spike == 1u ? 0.0f : height * 0.2f)),
            color);
    }
}

// ---- Roster strip ----------------------------------------------------------
// One row per occupied seat on every lobby/results screen: player label
// (P1/P2, "You", host crown), the picked racer in its accent color, the
// vehicle, and the Ready state (or the finishing place on a results screen).
void drawBetaRosterStrip(const MdkrOnlineLobby &lobby,
                         std::uint64_t localEndpoint) {
    if (!ui::CardBegin("##beta-roster", AppTheme::surface(), 0.0f)) {
        ui::CardEnd();
        return;
    }
    const bool resultsPhase = lobby.phase == MDKR_ONLINE_RESULTS;
    unsigned playerNumber = 0u;
    for (unsigned i = 0u; i < MDKR_ONLINE_MAX_SEATS; ++i) {
        const MdkrOnlineSeat &seat = lobby.seats[i];
        if (!seat.occupied) continue;
        ++playerNumber;
        if (playerNumber > 1u) ui::Gap(ui::kGapXS);
        const bool isYou = seat.endpoint_id == localEndpoint;
        const bool isHost = seat.endpoint_id == lobby.leader_endpoint_id;
        const MdkrOnlineMember *member = betaMemberFor(lobby, seat.endpoint_id);
        const float rowHeight = ImGui::GetTextLineHeight();
        ImDrawList *draw = ImGui::GetWindowDrawList();

        // Host crown, then the player label.
        if (isHost) {
            const ImVec2 at = ImGui::GetCursorScreenPos();
            drawBetaCrown(draw,
                          ImVec2(at.x, at.y + rowHeight * 0.15f),
                          rowHeight * 0.62f,
                          ImGui::GetColorU32(AppTheme::accent()));
        }
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                             rowHeight * 1.15f);
        char who[48];
        std::snprintf(who, sizeof(who), "P%u · %s%s", playerNumber,
                      isYou ? "You" : "Friend", isHost ? " · Host" : "");
        ImGui::TextUnformatted(who);

        // Racer name in its accent color + vehicle, aligned mid-row.
        ImGui::SameLine(ImGui::GetWindowWidth() * 0.40f);
        const bool hasCharacter =
            seat.character_id < MDKR_ONLINE_CHARACTER_COUNT;
        if (hasCharacter) {
            ImGui::PushStyleColor(
                ImGuiCol_Text,
                AppTheme::hex(kCharacterAccentHex[seat.character_id]));
            ImGui::TextUnformatted(kCharacters[seat.character_id]);
            ImGui::PopStyleColor();
            if (seat.vehicle_id < MDKR_ONLINE_PLAYER_VEHICLE_COUNT) {
                ImGui::SameLine();
                ui::TextSubtle("· %s", kVehicles[seat.vehicle_id]);
            }
        } else {
            ui::TextSubtle("Choosing…");
        }

        // Right-aligned state: Ready in the lobby, place on results.
        char stateText[32];
        ImVec4 stateColor = AppTheme::subtle();
        if (resultsPhase) {
            const std::uint8_t place = lobby.last_placements[i];
            std::snprintf(stateText, sizeof(stateText), "%s",
                          place != MDKR_ONLINE_NO_PLACEMENT
                              ? betaPlacementLabel(place) : "—");
            if (place == 0u) stateColor = AppTheme::accent();
        } else if (member != nullptr && member->ready) {
            std::snprintf(stateText, sizeof(stateText), "READY");
            stateColor = AppTheme::good();
        } else {
            std::snprintf(stateText, sizeof(stateText), "Not Ready");
        }
        const float textWidth = ImGui::CalcTextSize(stateText).x;
        ImGui::SameLine(ImGui::GetWindowWidth() - textWidth -
                        ImGui::GetStyle().WindowPadding.x -
                        ImGui::GetStyle().ItemSpacing.x);
        ImGui::PushStyleColor(ImGuiCol_Text, stateColor);
        ImGui::TextUnformatted(stateText);
        ImGui::PopStyleColor();
    }
    ui::CardEnd();
}

// Truthful re-entry reason line, mirroring the betaFailureCopy conventions (plain
// sentences, no wire codes): LEFT is a player backing out / a seat vacating / a
// cancel; ERROR is the wall-clock watchdog or an unplayable connection. Both now end
// with re-entry available -- the room is NOT done -- so neither says "create a new
// one".
const char *betaReentryReasonCopy(MdkrPartyLinkSessionEndReason reason) {
    switch (reason) {
    case MDKR_PARTY_LINK_SESSION_END_ERROR:
        return "The connection ran into trouble mid-race, so the race stopped. "
               "You're both still in the room — return to the game to try again.";
    case MDKR_PARTY_LINK_SESSION_END_LEFT:
    default:
        return "That race ended early. You're both still in the room — return to "
               "the game to pick and race again.";
    }
}

// ---- Native hand-off card ---------------------------------------------------
// After pairing, the descriptor-less native online screens (CHARSELECT ->
// VEHICLE SELECT -> TRACKSELECT) boot within a frame or two and OWN character,
// vehicle, and track/cup/mode for EVERY online mode -- single race and tournament
// alike (single race routes through the same descriptor-less native path, so the
// room-ready takeover fires for both modes). This concise card replaces the
// WHOLE ImGui per-race selection surface (racer grid, vehicle chips, Race
// Settings mode/cup/track picker, Ready/Start) so the human never lands on a
// stale editable grid the game is about to own.
//
// Two variants, one card:
//   - reentryReason == NONE: the FORWARD hand-off, shown while the takeover is
//     engaged (starting / boot pending) -- "handing to the game", no button.
//   - reentryReason == LEFT/ERROR: the RE-ENTRY control, shown after a native
//     session returned early. The takeover latch stays SET on a LEFT/ERROR return
//     (it must never auto re-fire -- the re-boot-loop hazard), so without an explicit
//     gesture the room would be a dead end now that the per-race ImGui fallback is
//     retired. This variant states the reason and offers a "Return to game" button
//     that re-arms (OnlineRoom_requestRoomReadyReentry) so the next poll re-takes
//     native. Returns true the frame the button is pressed.
bool drawBetaNativeHandoffCard(bool tournament,
                               MdkrPartyLinkSessionEndReason reentryReason) {
    const bool reentry = reentryReason == MDKR_PARTY_LINK_SESSION_END_LEFT ||
                         reentryReason == MDKR_PARTY_LINK_SESSION_END_ERROR;
    bool pressed = false;
    if (ui::CardBegin("##beta-native-handoff", AppTheme::accent(), 0.0f)) {
        if (reentry) {
            ImGui::TextUnformatted("Back in the room");
            ui::TextSubtleWrapped(betaReentryReasonCopy(reentryReason));
            ui::Gap(ui::kGapS);
            pressed = ui::BrandPrimaryButton("Return to game",
                                             ui::kBtnFullWidth());
        } else {
            ImGui::TextUnformatted("Starting — handing to the game…");
            ui::TextSubtleWrapped(
                tournament
                    ? "The game takes over from here. Pick your cup, racer, and "
                      "vehicle on the next screen."
                    : "The game takes over from here. Pick your racer, vehicle, "
                      "and track on the next screen.");
        }
    }
    ui::CardEnd();
    return pressed;
}

// ---- Native RESULTS hand-off card -------------------------------------------
// After a race the native RESULTS screen + the MORE-RACES chooser + the champion
// ceremony OWN per-race placements, cumulative standings, the champion banner AND
// every replay choice (Race Again / Change Track / Change Cup / New Tournament /
// Change Character+Vehicle / Finish) for EVERY online mode -- single race and
// tournament alike. This concise card is the WHOLE launcher RESULTS body now that
// the ImGui results/standings/replay surface is retired: RESULTS always renders it
// unconditionally (drawBetaResultsHandoff), so the human never lands on a stale
// standings body -- or an editable Next-Race / New-Tournament prompt -- the game
// already owns. It is the RESULTS mirror of the SELECTING hand-off card.
void drawBetaNativeResultsHandoffCard() {
    if (ui::CardBegin("##beta-native-results-handoff", AppTheme::accent(), 0.0f)) {
        ImGui::TextUnformatted("The game is showing results…");
        ui::TextSubtleWrapped(
            "Standings, the trophy ceremony, and your options for more races "
            "are all in the game — pick what's next on screen.");
    }
    ui::CardEnd();
}

// ---- Post-pairing SELECTING surface (roster + native hand-off card) --------
// After pairing the native game owns character / vehicle / track / cup / mode
// select, so the launcher's SELECTING surface is only the roster strip plus the
// native hand-off card: the FORWARD "handing to the game" card normally, or the
// "Return to game" RE-ENTRY card after a LEFT/ERROR native return (the takeover
// latch stays set with nothing pending in that state, so the room-ready poll will
// not re-fire on its own -- the re-entry press re-arms it). Consumes the view
// model's PRIMARY slot (no Ready/Start button); the shared code below still draws
// secondary (Connection Details) and cancel (Leave Room).
void drawBetaSelectingHandoff(const MdkrOnlineViewModel &model,
                              const MdkrOnlineLobby &lobby) {
    drawBetaRosterStrip(lobby,
                        betaLocalEndpoint(lobby, model.local_member_is_leader));
    ui::Gap(ui::kGapM);
    const bool tournament = lobby.mode == MDKR_ONLINE_MODE_TOURNAMENT;
    const MdkrPartyLinkSessionEndReason reentryReason =
        OnlineRoom_roomReadyReentryReason();
    if (reentryReason == MDKR_PARTY_LINK_SESSION_END_LEFT ||
        reentryReason == MDKR_PARTY_LINK_SESSION_END_ERROR) {
        g_betaSelectingRender = BetaSelectingRender::Reentry;
        if (drawBetaNativeHandoffCard(tournament, reentryReason)) {
            OnlineRoom_requestRoomReadyReentry();
        }
    } else {
        g_betaSelectingRender = BetaSelectingRender::Handoff;
        drawBetaNativeHandoffCard(tournament, MDKR_PARTY_LINK_SESSION_END_NONE);
    }
}

// ---- Post-pairing RESULTS surface (roster + native results hand-off card) ---
// After a race the native RESULTS screen + MORE-RACES chooser + champion ceremony
// own placements, standings, the banner AND every replay choice, so the launcher's
// RESULTS surface is only the roster strip plus the concise results hand-off card.
// Consumes the PRIMARY slot; the shared code below still draws secondary
// (Connection Details) and cancel (Leave Room).
void drawBetaResultsHandoff(const MdkrOnlineViewModel &model,
                            const MdkrOnlineLobby &lobby) {
    drawBetaRosterStrip(lobby,
                        betaLocalEndpoint(lobby, model.local_member_is_leader));
    ui::Gap(ui::kGapM);
    g_betaResultsRender = BetaResultsRender::Handoff;
    drawBetaNativeResultsHandoffCard();
}

// The section header above every beta room surface. Post-pairing the SELECTING
// surface is the private-room hand-off, so it reads "Private Room" + a subtitle
// that names the native-takeover reality (the old "Pick Your Racer" / "Choose a
// racer and vehicle" copy described the retired grid and contradicted the hand-off
// card). The RESULTS surface keeps a SINGLE heading -- the section title -- with no
// subtitle, since the strip line already states the outcome; stacking a subtitle
// like "The trophy is decided." repeated a third same-meaning header. Every other
// kind renders its view-model title -- with any trailing in-progress ellipsis
// stripped so it reads as a clean heading (the waiting motion stays in the status
// strip above, which animates its own ellipsis) -- plus its explanation.
void drawBetaSectionHeader(const MdkrOnlineViewModel &model) {
    if (model.kind == MDKR_ONLINE_VIEW_SELECTING) {
        ui::SectionHeader(
            "Private Room",
            "You're connected. Picking racers, tracks, and racing all happen "
            "in the game.");
        return;
    }
    char sectionTitle[128];
    std::snprintf(sectionTitle, sizeof(sectionTitle), "%s",
                  model.title != nullptr ? model.title : "");
    // A section title is a HEADING, not a waiting line, so it reads as a clean
    // title with no trailing punctuation: strip the in-progress ellipsis some
    // view-model titles carry ("Creating Private Room…" -> "Creating Private
    // Room"). The waiting MOTION still lives in the status strip above, which
    // animates its own ellipsis.
    const std::size_t titleLen = std::strlen(sectionTitle);
    static const char kEllipsis[] = "\xE2\x80\xA6";  // U+2026
    if (titleLen >= 3u &&
        std::memcmp(sectionTitle + titleLen - 3u, kEllipsis, 3u) == 0) {
        sectionTitle[titleLen - 3u] = '\0';
    }
    ui::SectionHeader(sectionTitle, model.kind == MDKR_ONLINE_VIEW_RESULTS
                                        ? nullptr
                                        : model.explanation);
}

void drawBetaRoom(LauncherState &state) {
    g_online.adapter->service();
    MdkrOnlineViewModel model{};
    if (!g_online.adapter->view(&model)) {
        ui::CautionBox("Online Room Unavailable",
                       "The online session produced an invalid state. Leave the "
                       "room and try again.");
        return;
    }

    // The recovery view's "Play Here" secondary leaves the room for OFFLINE play
    // (handleAction PLAY_HERE requests the Play tab), so label it by that effect.
    if (model.kind == MDKR_ONLINE_VIEW_RECOVERY &&
        model.secondary.action == MDKR_ONLINE_VIEW_ACTION_PLAY_HERE) {
        model.secondary.label = "Play Offline Instead";
    }

    // The authoritative lobby snapshot behind the roster strip / hand-off bodies.
    MdkrOnlineLobby lobby{};
    const bool haveLobby =
        mdkr_online_live_adapter_lobby(g_online.adapter.get(), &lobby);

    announceView(model);
    drawBetaStatusLine(model, haveLobby ? &lobby : nullptr);
    drawBetaSectionHeader(model);

    // The host's invite card renders through the CONNECTING create round trip
    // too, not just the open ROOM: the transport learns the fallback code
    // BEFORE the Ready event advances the view to ROOM, so gating on ROOM alone
    // left the host's real wait (spent in CONNECTING) showing a bare
    // "Connecting…" with nothing to share. local_member_is_leader is false until
    // a lobby snapshot exists, so the launcher-side betaHostJourney flag is the
    // host source for the pre-room state.
    if (model.kind == MDKR_ONLINE_VIEW_CONNECTING) {
        drawBetaInviteCard(g_online.betaHostJourney);
    } else if (model.kind == MDKR_ONLINE_VIEW_ROOM) {
        drawBetaInviteCard(g_online.betaHostJourney ||
                           model.local_member_is_leader);
        if (haveLobby) {
            ui::Gap(ui::kGapM);
            drawBetaRosterStrip(
                lobby, betaLocalEndpoint(lobby, model.local_member_is_leader));
        }
    }

    // The secure-phrase confirmation is the prominent, side-by-side decision;
    // it replaces the generic stacked buttons at this step only.
    if (model.kind == MDKR_ONLINE_VIEW_PREFLIGHT &&
        model.verification_phrase[0] != '\0') {
        drawBetaPhraseDecision(model, state);
        drawLeaveRaceConfirmation(state);
        drawConnectionDetails(model);
        return;
    }

    const MdkrOnlineViewAction timeoutAction =
        model.timeout.present && g_online.adapter->timeoutExpired()
            ? model.timeout.primary.action : MDKR_ONLINE_VIEW_ACTION_NONE;
    // Only lead with a gap when something separable follows -- the roster +
    // hand-off body or an expired-timeout box. A generic invite/recovery body
    // no longer floats the section rule above a dead band.
    const bool richBody =
        haveLobby &&
        ((model.kind == MDKR_ONLINE_VIEW_SELECTING &&
          lobby.phase == MDKR_ONLINE_LOBBY) ||
         (model.kind == MDKR_ONLINE_VIEW_RESULTS &&
          lobby.phase == MDKR_ONLINE_RESULTS));
    if (richBody || timeoutAction != MDKR_ONLINE_VIEW_ACTION_NONE) {
        ui::Gap(ui::kGapM);
    }
    if (timeoutAction != MDKR_ONLINE_VIEW_ACTION_NONE) {
        ui::CautionBox(model.timeout.title, model.timeout.explanation);
        ui::Gap(ui::kGapS);
        if (drawActionButton(model.timeout.primary, true)) {
            handleAction(model.timeout.primary.action, state);
        }
        ui::Gap(ui::kGapM);
    }

    /* Complete a pending room-ready re-arm every frame, BEFORE the
     * SELECTING branch polls the trigger. A no-op unless a FINISHED return armed
     * it; then it consumes the arm and clears the latch immediately -- the
     * tournament-final FINISH wrap already took the room out of the takeover
     * window (RESULTS) and back to a fresh-series SELECTING during the session,
     * so the very next poll re-takes native: the automatic FINISHED re-take on
     * both endpoints. One clear per FINISHED arm; a LEFT/ERROR return never arms,
     * so it can never re-boot-loop. */
    OnlineRoom_observeRoomReadyRearm(g_online.adapter.get());

    /* PRODUCTION ROOM-READY takeover, polled UNCONDITIONALLY every panel frame
     * (immediately after the re-arm observer, BEFORE the body branches below). The
     * poll is self-guarded (a one-shot latch + the room-ready condition check inside,
     * online_live_wiring.cpp), so calling it every frame is idempotent: it fires
     * EXACTLY ONCE on the first frame a room reaches SELECTING with 2 members in
     * LOBBY (any mode), publishing this adapter for the descriptor-less native boot
     * that the launcher's interactive loop consumes. Polling here -- rather than only
     * inside the rich-body SELECTING branch -- makes the takeover deterministic every
     * frame regardless of which body draws. Single-race / unconfigured rooms whose
     * READY is vote-gated simply never satisfy the condition and keep the ImGui
     * fallback. The panel's OwningLiveAdapter wrapper resolves the concrete adapter
     * through the mdkrResolveLive hook, so handing the wrapper here is fine. */
    (void)OnlineRoom_pollRoomReadyTransition(g_online.adapter.get());

    // Snapshot-backed hand-off bodies (they own the PRIMARY slot); every other
    // view kind keeps the generic control stack below.
    bool primaryDrawn = false;
    if (haveLobby && model.kind == MDKR_ONLINE_VIEW_SELECTING &&
        lobby.phase == MDKR_ONLINE_LOBBY) {
        drawBetaSelectingHandoff(model, lobby);
        primaryDrawn = true;
    } else if (haveLobby && model.kind == MDKR_ONLINE_VIEW_RESULTS &&
               lobby.phase == MDKR_ONLINE_RESULTS) {
        drawBetaResultsHandoff(model, lobby);
        primaryDrawn = true;
    }
    if (!primaryDrawn && model.primary.action != timeoutAction &&
        drawActionButton(model.primary, true)) {
        handleAction(model.primary.action, state);
    }
    if (model.secondary.visible && model.secondary.action != timeoutAction) {
        ui::Gap(ui::kGapS);
        if (drawActionButton(model.secondary, false)) {
            handleAction(model.secondary.action, state);
        }
    }
    if (model.cancel.visible) {
        ui::Gap(ui::kGapS);
        if (drawActionButton(model.cancel, false)) {
            handleAction(model.cancel.action, state);
        }
    }

    drawLeaveRaceConfirmation(state);
    drawConnectionDetails(model);
    drawUpdateHelp();
}

// ===========================================================================
// TEST-ONLY beta lobby render seam (MDKR_APP_ONLINE_BETA_FAKE + _STAGE).
//
// The live drawBetaRoom only reaches the shipping beta lobby widgets behind a
// real 2-peer cloud adapter (buildBetaLiveAdapter needs a live service, release
// provenance and a verified ROM), and MDKR_APP_ONLINE_FAKE routes to the LEGACY
// drawRoomPanel instead. That left every surface past the chooser -- the invite
// card, the safety-phrase decision, the roster strip, the SELECTING body
// (single-race + tournament hand-off), the FINISHED / New-Tournament landing and
// the recovery cards -- with no solo/headless screenshot path.
//
// This seam synthesizes a faithful MdkrOnlineViewModel (+ lobby snapshot) for a
// chosen stage and drives the SAME drawBeta* widget family drawBetaRoom uses, so
// each surface renders standalone for MDKR_APP_SMOKE_SHOT. It is STRICTLY beta +
// test-only: compiled only under MDKR_ENABLE_ONLINE_BETA (a release/OFF build
// never sees it) and reached only when MDKR_APP_ONLINE_BETA_FAKE is set --
// mirroring the MDKR_APP_ONLINE_FAKE gate. No live adapter is ever built
// (g_online.adapter stays null), so the real (non-fake) path is unchanged.
// ===========================================================================

bool betaFakeStageEnabled() {
    return std::getenv("MDKR_APP_ONLINE_BETA_FAKE") != nullptr;
}

// A vehicle that IS legal on `track` under the 2-player picker mask, so the
// SELECTING body's illegal-vehicle auto-correction (which would dispatch on the
// absent adapter) never fires while a fake stage is drawn.
std::uint8_t betaFakeLegalVehicle(std::uint16_t track) {
    const std::uint8_t mask =
        track != MDKR_ONLINE_NO_VOTE
            ? mdkr_online_track_picker_mask(track, 2u)
            : static_cast<std::uint8_t>(MDKR_ONLINE_VEHICLE_BIT_ALL);
    for (std::uint8_t v = 0u; v < MDKR_ONLINE_PLAYER_VEHICLE_COUNT; ++v) {
        if (mask & (1u << v)) return v;
    }
    return 0u;
}

// A faithful 2-endpoint / 2-seat lobby (seat 0 = local host, seat 1 = friend),
// matching the reducer's shape the live widgets read.
void betaFakeInitLobby(MdkrOnlineLobby *lobby, std::uint8_t mode,
                       MdkrOnlinePhase phase) {
    std::memset(lobby, 0, sizeof(*lobby));
    const std::uint64_t hostEp = UINT64_C(0x1001);
    const std::uint64_t friendEp = UINT64_C(0x1002);
    lobby->protocol_version = MDKR_ONLINE_PROTOCOL_VERSION;
    lobby->revision = 7u;
    lobby->room_id = UINT64_C(0x4245544141);
    lobby->leader_endpoint_id = hostEp;
    lobby->phase = phase;
    lobby->mode = mode;
    lobby->cup_id = MDKR_ONLINE_NO_CUP;
    lobby->configured_track = MDKR_ONLINE_NO_VOTE;
    lobby->member_count = 2u;
    lobby->seat_count = 2u;
    for (unsigned i = 0u; i < MDKR_ONLINE_MAX_SEATS; ++i) {
        lobby->last_placements[i] = MDKR_ONLINE_NO_PLACEMENT;
    }
    lobby->members[0].occupied = true;
    lobby->members[0].connected = true;
    lobby->members[0].endpoint_id = hostEp;
    lobby->members[0].seat_count = 1u;
    lobby->members[1].occupied = true;
    lobby->members[1].connected = true;
    lobby->members[1].endpoint_id = friendEp;
    lobby->members[1].seat_count = 1u;
    lobby->seats[0].occupied = true;
    lobby->seats[0].endpoint_id = hostEp;
    lobby->seats[0].character_id = MDKR_ONLINE_NO_CHARACTER;
    lobby->seats[0].vehicle_id = MDKR_ONLINE_NO_VEHICLE;
    lobby->seats[1].occupied = true;
    lobby->seats[1].endpoint_id = friendEp;
    lobby->seats[1].character_id = MDKR_ONLINE_NO_CHARACTER;
    lobby->seats[1].vehicle_id = MDKR_ONLINE_NO_VEHICLE;
}

MdkrOnlineViewControl betaFakeControl(MdkrOnlineViewAction action,
                                     const char *label) {
    MdkrOnlineViewControl c{};
    c.action = action;
    c.label = label;
    c.visible = true;
    c.enabled = true;
    return c;
}

// A faithful 2-seat SELECTING lobby + view model for the native-takeover body,
// for either mode. `fallback` reproduces the post-LEFT/ERROR recovery state (the
// takeover is no longer engaged): it forces the engaged predicate false AND records a
// LEFT re-entry reason, so the seam captures both the universal forward hand-off card
// (fallback=false) and the "Return to game" re-entry card (fallback=true).
void betaFakeBuildSelectingStage(MdkrOnlineViewModel *model,
                                 MdkrOnlineLobby *lobby, bool *haveLobby,
                                 bool tournament, bool fallback) {
    // The fallback stages simulate a LEFT native return so the SELECTING surface
    // draws the "Return to game" re-entry card the live launcher would offer after
    // such a return; a non-fallback stage draws the forward hand-off card.
    if (fallback) OnlineRoom_noteSessionReturn(MDKR_PARTY_LINK_SESSION_END_LEFT);
    betaFakeInitLobby(lobby,
                      tournament ? MDKR_ONLINE_MODE_TOURNAMENT
                                 : MDKR_ONLINE_MODE_SINGLE_RACE,
                      MDKR_ONLINE_LOBBY);
    // Single race: host has locked a track. Tournament: cup left NO_CUP. Either
    // way a legal vehicle is stamped on each seat so the roster strip reads
    // realistically (racer + vehicle).
    std::uint16_t track = MDKR_ONLINE_NO_VOTE;
    if (!tournament) {
        track = 5u;  // Ancient Lake (Dino Domain, race 1)
        lobby->configured_track = track;
    }
    const std::uint8_t veh = betaFakeLegalVehicle(track);
    lobby->seats[0].character_id = 2u;  // Pipsy (you)
    lobby->seats[0].vehicle_id = veh;
    lobby->seats[1].character_id = 5u;  // Bumper (friend)
    lobby->seats[1].vehicle_id = veh;
    if (!tournament) lobby->members[1].ready = true;  // friend ready; you choosing
    *haveLobby = true;
    model->kind = MDKR_ONLINE_VIEW_SELECTING;
    // The section header is drawn by drawBetaSectionHeader, which renders the
    // native-takeover copy for the SELECTING kind; these fields mirror it so the
    // model stays coherent with what shows.
    model->title = "Private Room";
    model->explanation =
        "You're connected. Picking racers, tracks, and racing all happen in the "
        "game.";
    model->primary = betaFakeControl(MDKR_ONLINE_VIEW_ACTION_READY, "Ready");
    model->secondary = betaFakeControl(
        MDKR_ONLINE_VIEW_ACTION_CONNECTION_DETAILS, "Connection Details");
    model->cancel =
        betaFakeControl(MDKR_ONLINE_VIEW_ACTION_LEAVE_ROOM, "Leave Room");
    model->member_count = 2u;
    model->ready_count = tournament ? 0u : 1u;
    model->seat_count = 2u;
    model->local_member_is_leader = true;
}

// A faithful 2-seat RESULTS lobby + view model for the native-RESULTS hand-off
// card, for either mode. The full ImGui results/standings/replay body is retired,
// so RESULTS always renders the concise hand-off card -- there is no fallback
// variant. Tournament uses the FINAL round (champion decided); single race uses a
// two-placement finish. The seat placements/points feed the roster strip.
void betaFakeBuildResultsStage(MdkrOnlineViewModel *model,
                               MdkrOnlineLobby *lobby, bool *haveLobby,
                               bool tournament) {
    betaFakeInitLobby(lobby,
                      tournament ? MDKR_ONLINE_MODE_TOURNAMENT
                                 : MDKR_ONLINE_MODE_SINGLE_RACE,
                      MDKR_ONLINE_RESULTS);
    lobby->seats[0].character_id = 2u;  // Pipsy (you)
    lobby->seats[1].character_id = 1u;  // Timber (friend)
    lobby->last_placements[0] = 0u;  // 1st this race
    lobby->last_placements[1] = 1u;  // 2nd this race
    if (tournament) {
        lobby->cup_id = 0u;  // first cup
        lobby->race_index =
            static_cast<std::uint8_t>(MDKR_ONLINE_CUP_ROUNDS - 1u);  // final
        lobby->points[0] = 34u;
        lobby->points[1] = 30u;
    }
    *haveLobby = true;
    model->kind = MDKR_ONLINE_VIEW_RESULTS;
    model->title = tournament ? "Race Complete" : "Race Results";
    model->explanation =
        tournament ? "The trophy is decided." : "The race is decided.";
    model->primary = betaFakeControl(
        MDKR_ONLINE_VIEW_ACTION_RACE_AGAIN,
        tournament ? "New Tournament" : "Race Again");
    model->secondary = betaFakeControl(
        MDKR_ONLINE_VIEW_ACTION_CONNECTION_DETAILS, "Connection Details");
    model->cancel =
        betaFakeControl(MDKR_ONLINE_VIEW_ACTION_LEAVE_ROOM, "Leave Room");
    model->member_count = 2u;
    model->seat_count = 2u;
    model->local_member_is_leader = true;
}

// Build (model, lobby, haveLobby) for one stage and arm/disarm the fake invite.
// Returns false for an unknown stage.
bool betaFakeBuildStage(const char *stage, MdkrOnlineViewModel *model,
                        MdkrOnlineLobby *lobby, bool *haveLobby) {
    std::memset(model, 0, sizeof(*model));
    *haveLobby = false;
    g_betaFakeInvite.active = false;
    // Clear any re-entry offer so a non-fallback stage renders the forward hand-off,
    // not the "Return to game" re-entry card; the fallback stages re-arm it below.
    OnlineRoom_noteSessionReturn(MDKR_PARTY_LINK_SESSION_END_NONE);
    g_online.betaHostJourney = true;  // the fake local player hosts

    if (std::strcmp(stage, "invite") == 0) {
        model->kind = MDKR_ONLINE_VIEW_CONNECTING;
        model->title = "Creating Private Room…";
        model->explanation =
            "Share the code with your friend — the lobby opens once they join.";
        model->member_count = 1u;
        model->local_member_is_leader = true;
        g_betaFakeInvite.active = true;
        g_betaFakeInvite.code = "123456";
        g_betaFakeInvite.url = "/join#123456";
        return true;
    }
    if (std::strcmp(stage, "phrase") == 0) {
        model->kind = MDKR_ONLINE_VIEW_PREFLIGHT;
        model->title = "Compare These Words";
        model->explanation =
            "Read all 3 groups aloud. Continue only when every display shows "
            "exactly the same words.";
        std::snprintf(model->verification_phrase,
                      sizeof(model->verification_phrase), "%s",
                      "Nimble-Pilot Jolly-Star Sunny-Falcon");
        model->primary = betaFakeControl(MDKR_ONLINE_VIEW_ACTION_CONFIRM_PHRASE,
                                         "Words Match");
        model->secondary = betaFakeControl(
            MDKR_ONLINE_VIEW_ACTION_REPORT_PHRASE_MISMATCH, "Words Differ");
        model->cancel =
            betaFakeControl(MDKR_ONLINE_VIEW_ACTION_LEAVE_ROOM, "Leave Room");
        model->member_count = 2u;
        model->local_member_is_leader = true;
        return true;
    }
    // SELECTING surface, the shipping post-pairing state for BOTH modes: roster +
    // the forward native hand-off card (no per-race grid). "handoff" is kept as a
    // legacy alias of room-tournament.
    if (std::strcmp(stage, "room-single") == 0) {
        betaFakeBuildSelectingStage(model, lobby, haveLobby, false, false);
        return true;
    }
    if (std::strcmp(stage, "room-tournament") == 0 ||
        std::strcmp(stage, "handoff") == 0) {
        betaFakeBuildSelectingStage(model, lobby, haveLobby, true, false);
        return true;
    }
    // SELECTING surface after a LEFT/ERROR native return: roster + the "Return to
    // game" re-entry card, so a mid-session drop is never a dead end -- pressing it
    // re-arms the native takeover, for either mode.
    if (std::strcmp(stage, "room-single-fallback") == 0) {
        betaFakeBuildSelectingStage(model, lobby, haveLobby, false, true);
        return true;
    }
    if (std::strcmp(stage, "room-tournament-fallback") == 0) {
        betaFakeBuildSelectingStage(model, lobby, haveLobby, true, true);
        return true;
    }
    // RESULTS surface, the shipping post-race state for BOTH modes: roster + the
    // concise "showing results" hand-off card -- the native RESULTS + MORE-RACES
    // chooser + ceremony own placements, standings and every replay choice. There is
    // no full standings/replay fallback body any more, so there is no *-fallback
    // RESULTS stage. "results" is single race; "finished" is the tournament final.
    if (std::strcmp(stage, "results") == 0) {
        betaFakeBuildResultsStage(model, lobby, haveLobby, false);
        return true;
    }
    if (std::strcmp(stage, "finished") == 0) {
        betaFakeBuildResultsStage(model, lobby, haveLobby, true);
        return true;
    }
    if (std::strcmp(stage, "recovery") == 0) {
        model->kind = MDKR_ONLINE_VIEW_RECOVERY;
        model->failure = MDKR_ONLINE_VIEW_FAILURE_VERIFICATION_MISMATCH;
        model->title = "Words Did Not Match";
        model->explanation =
            "The secure connection may have changed. Do not continue until "
            "everyone compares a new phrase.";
        model->primary =
            betaFakeControl(MDKR_ONLINE_VIEW_ACTION_RETRY, "Reconnect Securely");
        model->secondary = betaFakeControl(MDKR_ONLINE_VIEW_ACTION_PLAY_HERE,
                                           "Play Offline Instead");
        model->cancel =
            betaFakeControl(MDKR_ONLINE_VIEW_ACTION_LEAVE_ROOM, "Leave Room");
        model->announcement = MDKR_ONLINE_ANNOUNCE_ASSERTIVE;
        return true;
    }
    return false;
}

// Render one synthetic stage through the REAL beta widgets. Mirrors
// drawBetaRoom's composition but sources model/lobby from betaFakeBuildStage and
// omits every live-adapter-only call (service/view/take_refusal/room-ready
// polls). No button is ever pressed in a headless single-shot capture, so the
// press-only dispatch paths inside the shared widgets are never reached.
void drawBetaRoomFake(LauncherState &state) {
    const char *stage = std::getenv("MDKR_APP_ONLINE_BETA_STAGE");
    if (stage == nullptr || stage[0] == '\0') stage = "chooser";

    if (std::strcmp(stage, "chooser") == 0) {
        g_online.betaStage = OnlineRoomUiState::BetaStage::Chooser;
        drawBetaChooser(state);
        return;
    }
    if (std::strcmp(stage, "joincode") == 0) {
        g_online.betaStage = OnlineRoomUiState::BetaStage::JoinCode;
        std::snprintf(g_online.betaJoinCode, sizeof(g_online.betaJoinCode), "%s",
                      "1234");
        drawBetaChooser(state);
        return;
    }

    MdkrOnlineViewModel model{};
    MdkrOnlineLobby lobby{};
    bool haveLobby = false;
    if (!betaFakeBuildStage(stage, &model, &lobby, &haveLobby)) {
        ui::CautionBox(
            "Unknown Beta Stage",
            "Set MDKR_APP_ONLINE_BETA_STAGE to one of: chooser, joincode, "
            "invite, phrase, room-single, room-tournament, handoff, "
            "room-single-fallback, room-tournament-fallback, results, "
            "finished, recovery.");
        return;
    }

    announceView(model);
    drawBetaStatusLine(model, haveLobby ? &lobby : nullptr);
    drawBetaSectionHeader(model);

    if (model.kind == MDKR_ONLINE_VIEW_CONNECTING) {
        drawBetaInviteCard(g_online.betaHostJourney);
    } else if (model.kind == MDKR_ONLINE_VIEW_ROOM) {
        drawBetaInviteCard(g_online.betaHostJourney ||
                           model.local_member_is_leader);
        if (haveLobby) {
            ui::Gap(ui::kGapM);
            drawBetaRosterStrip(
                lobby, betaLocalEndpoint(lobby, model.local_member_is_leader));
        }
    }

    if (model.kind == MDKR_ONLINE_VIEW_PREFLIGHT &&
        model.verification_phrase[0] != '\0') {
        drawBetaPhraseDecision(model, state);
        drawConnectionDetails(model);
        return;
    }

    // Only lead with a gap when the roster + hand-off body follows; a generic
    // invite/recovery body does not, so the section rule no longer floats above a
    // dead band.
    const bool richBody =
        haveLobby &&
        ((model.kind == MDKR_ONLINE_VIEW_SELECTING &&
          lobby.phase == MDKR_ONLINE_LOBBY) ||
         (model.kind == MDKR_ONLINE_VIEW_RESULTS &&
          lobby.phase == MDKR_ONLINE_RESULTS));
    if (richBody) {
        ui::Gap(ui::kGapM);
    }

    bool primaryDrawn = false;
    if (haveLobby && model.kind == MDKR_ONLINE_VIEW_SELECTING &&
        lobby.phase == MDKR_ONLINE_LOBBY) {
        drawBetaSelectingHandoff(model, lobby);
        primaryDrawn = true;
        // Semantic witness for the headless takeover-retire test: which SELECTING
        // surface rendered -- the forward native hand-off card or the "Return to
        // game" re-entry card (a LEFT/ERROR native return). The per-race ImGui grid
        // is retired, so there is no third state. Emitted only from the render seam.
        std::fprintf(
            stderr,
            "[online-beta-selecting] stage=%s mode=%s render=%s\n", stage,
            lobby.mode == MDKR_ONLINE_MODE_TOURNAMENT ? "tournament" : "single",
            g_betaSelectingRender == BetaSelectingRender::Handoff
                ? "handoff"
                : g_betaSelectingRender == BetaSelectingRender::Reentry
                      ? "reentry"
                      : "none");
    } else if (haveLobby && model.kind == MDKR_ONLINE_VIEW_RESULTS &&
               lobby.phase == MDKR_ONLINE_RESULTS) {
        drawBetaResultsHandoff(model, lobby);
        primaryDrawn = true;
        // Semantic witness for the headless results-retire test: the RESULTS surface
        // is the concise native "showing results" hand-off card. The full ImGui
        // results/standings/replay body is retired, so handoff is the only state.
        // Emitted only from the render seam, never the live path.
        std::fprintf(
            stderr,
            "[online-beta-results] stage=%s mode=%s render=%s\n", stage,
            lobby.mode == MDKR_ONLINE_MODE_TOURNAMENT ? "tournament" : "single",
            g_betaResultsRender == BetaResultsRender::Handoff ? "handoff"
                                                             : "none");
    }
    if (!primaryDrawn && drawActionButton(model.primary, true)) {
        handleAction(model.primary.action, state);
    }
    if (model.secondary.visible) {
        ui::Gap(ui::kGapS);
        if (drawActionButton(model.secondary, false)) {
            handleAction(model.secondary.action, state);
        }
    }
    if (model.cancel.visible) {
        ui::Gap(ui::kGapS);
        if (drawActionButton(model.cancel, false)) {
            handleAction(model.cancel.action, state);
        }
    }
    drawConnectionDetails(model);
}

void drawBetaOnlinePanel(LauncherState &state) {
    if (!g_online.adapter || !g_online.initialized) {
        drawBetaChooser(state);
        return;
    }
    drawBetaRoom(state);
}
#endif  // MDKR_ENABLE_ONLINE_BETA

}  // namespace

void OnlineRoomPanel_draw(LauncherState &state, LauncherAction &action) {
    (void)action;
    ensureInitialized();
#if MDKR_ENABLE_ONLINE_BETA
    // TEST-ONLY: MDKR_APP_ONLINE_BETA_FAKE renders a synthetic beta lobby stage
    // (MDKR_APP_ONLINE_BETA_STAGE) through the real drawBeta* widgets so each
    // shipping lobby surface can be screenshotted headlessly. No adapter is
    // built (ensureInitialized deferred it: MDKR_APP_ONLINE_FAKE is unset here),
    // and a release/OFF build never compiles this branch. Mirrors the
    // fakeEnabled() gate below.
    if (betaFakeStageEnabled()) {
        drawBetaRoomFake(state);
        return;
    }
    // Beta interactive path: with no fake smoke requested, run the create/join
    // chooser and the live Online Room UX. The chooser builds the live adapter
    // on the player's choice, so before that g_online.adapter is intentionally
    // null (ensureInitialized deferred it) -- this must come first.
    if (!fakeEnabled()) {
        // Test-only pairing bootstrap: drive the create/join + confirm steps
        // (inert unless MDKR_APP_TEST_ONLINE_AUTOPAIR is set) BEFORE the panel
        // draws, so the frame it builds the adapter the panel below services it
        // and polls the production room-ready takeover.
        autopairService(state);
        drawBetaOnlinePanel(state);
        return;
    }
#endif
    if (!g_online.adapter || !g_online.initialized) {
        drawUnavailablePanel();
        return;
    }
    drawRoomPanel(state);
}

int OnlineRoom_dumpGalleryContract() {
    const size_t count = mdkr_online_fake_gallery_count();
    const MdkrOnlineCompatibilityV1 compatibility = fakeCompatibility();
    std::printf("online-gallery-v1 count=%zu\n", count);
    for (size_t index = 0u; index < count; ++index) {
        const MdkrOnlineFakeGallerySpec *spec =
            mdkr_online_fake_gallery_at(index);
        MdkrOnlineFakeAdapter adapter{};
        MdkrOnlineViewModel model{};
        if (spec == nullptr ||
            !mdkr_online_fake_init(&adapter, UINT64_C(0x47414c4c) + index,
                                   &compatibility,
                                   spec->requires_race_admission) ||
            !mdkr_online_fake_prepare_gallery(&adapter, spec->slug) ||
            !mdkr_online_fake_view(&adapter, &model)) return 1;
        std::printf(
            "online-gallery-v1 slug=%s kind=%u failure=%u primary=%u "
            "timeout=%u admission=%u timeout-visible=%u\n",
            spec->slug, static_cast<unsigned>(spec->kind),
            static_cast<unsigned>(spec->failure),
            static_cast<unsigned>(spec->primary_action),
            spec->timeout_present ? 1u : 0u,
            spec->requires_race_admission ? 1u : 0u,
            std::strncmp(spec->slug, "timeout-", 8u) == 0 ? 1u : 0u);
        const char *copy[] = {
            spec->slug,
            model.title,
            model.primary.visible ? model.primary.label : "",
            model.secondary.visible ? model.secondary.label : "",
            model.cancel.visible ? model.cancel.label : "",
            adapter.timeout_expired && model.timeout.present &&
                    model.timeout.primary.visible
                ? model.timeout.primary.label : "",
            model.verification_phrase,
        };
        for (const char *field : copy) {
            if (field == nullptr || std::strchr(field, '|') != nullptr ||
                std::strchr(field, '\n') != nullptr ||
                std::strchr(field, '\r') != nullptr) return 1;
        }
        std::printf("online-gallery-copy-v2|%s|%s|%s|%s|%s|%s|%s\n",
                    copy[0], copy[1], copy[2], copy[3], copy[4], copy[5],
                    copy[6]);
        std::printf(
            "online-gallery-actions-v1|%s|%u|%u|%u|%u\n",
            spec->slug,
            model.primary.visible
                ? static_cast<unsigned>(model.primary.action) : 0u,
            model.secondary.visible
                ? static_cast<unsigned>(model.secondary.action) : 0u,
            model.cancel.visible
                ? static_cast<unsigned>(model.cancel.action) : 0u,
            adapter.timeout_expired && model.timeout.present &&
                    model.timeout.primary.visible
                ? static_cast<unsigned>(model.timeout.primary.action) : 0u);
    }
    return 0;
}

bool OnlineRoom_smokeActionResult(unsigned action, bool *accepted) {
    if (accepted == nullptr || action == MDKR_ONLINE_VIEW_ACTION_NONE ||
        action > MDKR_ONLINE_VIEW_ACTION_REPORT_PHRASE_MISMATCH ||
        g_online.completedAction != static_cast<MdkrOnlineViewAction>(action)) {
        return false;
    }
    *accepted = g_online.completedActionAccepted;
    return true;
}

#if MDKR_ENABLE_ONLINE_BETA
// ===========================================================================
// Native online beta: modal lobby takeover support.
//
// The launcher shell renders ONLY the full-screen lobby -- suppressing the nav
// rail, the top tabs, the panel router and the generic offline Play button --
// whenever a live online session has progressed past the entry/chooser. The
// decision is driven by the current view kind, so the one rule covers both the
// beta live adapter and the deterministic fake used by the headless proof.
// ===========================================================================

// Read-only peek at the current view kind. Returns 0 before an adapter exists
// or when it produces an invalid composition, so a half-built or torn-down
// session never engages the takeover.
static MdkrOnlineViewKind onlineCurrentViewKind() {
    if (!g_online.initialized || !g_online.adapter) {
        return static_cast<MdkrOnlineViewKind>(0);
    }
    MdkrOnlineViewModel model{};
    if (!g_online.adapter->view(&model)) {
        return static_cast<MdkrOnlineViewKind>(0);
    }
    return model.kind;
}

bool OnlineRoom_isLobbyTakeoverActive() {
    const MdkrOnlineViewKind kind = onlineCurrentViewKind();
    // ENTRY is the create/join chooser: the shell stays so a player can still
    // reach it. Every later kind is a live session and takes over the window.
    return kind != static_cast<MdkrOnlineViewKind>(0) &&
           kind != MDKR_ONLINE_VIEW_ENTRY;
}

int OnlineRoom_lobbyProbeViewKind() {
    return static_cast<int>(onlineCurrentViewKind());
}

// Tracker for the background adapter-teardown threads spawned by
// teardownAdapterAsync below. The threads used to be DETACHED, which left a
// SECOND door into the app-exit static-destruction race the ordered
// OnlineRoom_shutdownForAppExit closes for the PANEL-owned adapter: quit the
// app within seconds of an in-session Leave and the still-running teardown
// destructor (mesh/WebSocket close, worker joins) races destroyed globals
// after main returns -- the same uncaught "mutex lock failed" SIGABRT class,
// via the other path. The threads are now kept JOINABLE here, counted with a
// condition variable, and OnlineRoom_shutdownForAppExit waits (bounded) for
// the count to drain before joining them -- so every teardown destructor
// finishes before static destruction begins. Launcher-thread only (spawn and
// join both happen on the UI thread); the mutex guards against the worker
// threads' own decrements.
namespace {
struct AdapterTeardownTracker {
    std::mutex mutex;
    std::condition_variable done;
    unsigned live = 0u;            // teardown destructors still running
    std::vector<std::thread> threads;  // joinable handles (joined at app exit)
};
AdapterTeardownTracker sAdapterTeardowns;
}  // namespace

// Non-blocking teardown of the live adapter. Its destructor joins the room /
// mesh / signal-client worker threads and closes the WebRTC data channels and
// WebSocket -- any of which can stall for seconds. Doing that on the ImGui/main
// thread is the observed beach-ball, so the live adapter is handed to a
// TRACKED background thread (joined, bounded, at app exit -- see
// AdapterTeardownTracker) and destroyed there while the UI returns home
// immediately. The launcher thread never touches the adapter again after the
// hand-off, so single-owner off-thread destruction is safe. The deterministic
// fake owns no worker threads, so it is destroyed inline.
static void teardownAdapterAsync(std::unique_ptr<IMdkrOnlineAdapter> adapter) {
    if (!adapter) return;
    if (adapter->fakeAdapter() != nullptr) {
        adapter.reset();
        return;
    }
    // Retract any pending engine-race-boot handoff ON THE LAUNCHER THREAD
    // before the adapter crosses to the teardown thread: a detached-thread
    // destruction would race OnlineRoom_pollEngineRaceBoot() against a dying
    // adapter. race-boot is PUBLISHED with the RESOLVED RAW inner LiveAdapter
    // (setUpRace -> OnlineRoom_publishEngineRaceBoot(this)), so it must be
    // RETRACTED with that same raw pointer for the registry's pointer-identity
    // match -- the panel owns the OwningLiveAdapter WRAPPER, so resolve to the
    // concrete inner adapter (via the mdkrResolveLive hook) exactly as the
    // room-ready resolve-raw two lines below does.
    (void)mdkr_online_live_adapter_retract_race_boot(
        OnlineRoom_resolveRawLiveAdapter(adapter.get()));
    // Same hazard class for the room-ready registry: the
    // room-ready poll publishes the RESOLVED RAW inner LiveAdapter pointer, so a
    // "Leave Race" click on the very frame the room first hits SELECTING+2members+
    // LOBBY (any mode) could hand runInteractiveLauncher a dying adapter (UAF on
    // visible->service() + engine boot on freed memory). Retract it here, on the
    // launcher thread, BEFORE the detached destruction -- using the SAME wrapper->raw
    // resolution the publish used (a retract-by-wrapper-pointer would not match).
    OnlineRoom_retractEngineRoomReady(
        OnlineRoom_resolveRawLiveAdapter(adapter.get()));
    {
        std::lock_guard<std::mutex> lock(sAdapterTeardowns.mutex);
        ++sAdapterTeardowns.live;
    }
    std::thread worker([owned = std::move(adapter)]() mutable {
        owned.reset();
        {
            std::lock_guard<std::mutex> lock(sAdapterTeardowns.mutex);
            --sAdapterTeardowns.live;
        }
        sAdapterTeardowns.done.notify_all();
    });
    {
        std::lock_guard<std::mutex> lock(sAdapterTeardowns.mutex);
        sAdapterTeardowns.threads.push_back(std::move(worker));
    }
}

// The single, clean exit from an active online session. Hands the live adapter
// off for non-blocking teardown, resets the session UI back to the create/join
// chooser and asks the shell to return to the launcher home. The smoke witness
// fields are deliberately preserved so a token-gated action smoke can still read
// the outcome of a leave/return action after the adapter is gone.
static void leaveOnlineSession(LauncherState &state) {
    teardownAdapterAsync(std::move(g_online.adapter));
    g_online.initialized = false;
    g_online.detailsOpen = false;
    g_online.connectionDoctorOpen = false;
    g_online.updateHelpOpen = false;
    g_online.leaveRaceConfirm = false;
    g_online.leavePending = false;
    g_online.gallerySpec = nullptr;
    g_online.galleryTraced = false;
    g_online.announcedKind = static_cast<MdkrOnlineViewKind>(0);
    g_online.announcedFailure = MDKR_ONLINE_VIEW_FAILURE_NONE;
    g_online.announcedVerificationPhrase[0] = '\0';
    g_online.betaStage = OnlineRoomUiState::BetaStage::Chooser;
    g_online.betaJoinCode[0] = '\0';
    g_online.betaHostJourney = false;
    g_online.betaBuildFailed = false;
    Launcher_requestTab(state, kLauncherPanelPlay, kLauncherTabPlayer);
}

void OnlineRoom_requestLeave() { g_online.leavePending = true; }

void OnlineRoom_shutdownForAppExit() {
    // ORDERED app-exit teardown (see the header). Unlike the in-session leave
    // (teardownAdapterAsync, which backgrounds destruction so the UI never
    // beach-balls), the app is exiting: destroy the live adapter INLINE on this
    // thread so its mesh / signal-client worker threads are joined BEFORE main
    // returns and static destruction begins. Without this, quitting the app
    // with a live room up let an ICE-state callback race destroyed globals --
    // an uncaught "mutex lock failed" SIGABRT (first observed on the capstone's
    // scripted quit right after the FINISHED re-take put both endpoints back in
    // a live session). The registries are retracted with the same resolved-raw
    // pointers the async path uses.
    if (g_online.adapter) {
        (void)mdkr_online_live_adapter_retract_race_boot(
            OnlineRoom_resolveRawLiveAdapter(g_online.adapter.get()));
        OnlineRoom_retractEngineRoomReady(
            OnlineRoom_resolveRawLiveAdapter(g_online.adapter.get()));
        g_online.adapter.reset();
        g_online.initialized = false;
    }
    // SECOND DOOR into the same race: an adapter handed to
    // teardownAdapterAsync moments before quit (an in-session Leave) is still
    // being destroyed on its background thread. Wait -- BOUNDED -- for every
    // in-flight teardown destructor to finish, then join the (now-returning)
    // handles, so no teardown thread can outlive main. The bound is generous
    // for a mesh/WebSocket close; if a pathological close exceeds it, detach
    // the stragglers with a loud diagnostic (the pre-fix behavior, now
    // impossible to hit silently) rather than hanging exit forever.
    std::vector<std::thread> teardowns;
    bool drained = true;
    {
        std::unique_lock<std::mutex> lock(sAdapterTeardowns.mutex);
        drained = sAdapterTeardowns.done.wait_for(
            lock, std::chrono::seconds(10),
            [] { return sAdapterTeardowns.live == 0u; });
        teardowns.swap(sAdapterTeardowns.threads);
    }
    for (std::thread &worker : teardowns) {
        if (!worker.joinable()) continue;
        if (drained) {
            worker.join(); // destructor finished; join returns promptly
        } else {
            std::fprintf(stderr,
                         "[online-room] app-exit: an adapter teardown exceeded "
                         "the 10s drain bound; detaching it (exit proceeds)\n");
            worker.detach();
        }
    }
}

void OnlineRoom_serviceLobbyLeave(LauncherState &state) {
    // A body control that navigates home (Return Home / Play Here / confirmed
    // Leave Race) requests the Play tab; the persistent takeover control sets
    // leavePending. Either way, a live session must be torn down before the
    // shell returns, so returning home never leaves a live adapter behind (the
    // structural cause of the offline-launch beach-ball).
    const bool goingHome = state.requestTab == kLauncherPanelPlay;
    if (!g_online.leavePending && !goingHome) return;
    leaveOnlineSession(state);
}

bool OnlineRoom_lobbyHeaderInfo(OnlineLobbyHeaderInfo *out) {
    if (out == nullptr || !g_online.initialized || !g_online.adapter) {
        return false;
    }
    MdkrOnlineViewModel model{};
    if (!g_online.adapter->view(&model)) return false;
    out->memberCount = model.member_count;
    out->readyCount = model.ready_count;
    out->seatCount = model.seat_count;
    out->localIsLeader = model.local_member_is_leader;
    out->statusLine = betaStatusLine(model);
    return true;
}
#endif  // MDKR_ENABLE_ONLINE_BETA
