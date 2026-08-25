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
#include "qrcodegen.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <thread>
#include <utility>
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
    bool betaBuildFailed = false;
    bool betaCharacterTaken = false;
    // Deferred, non-blocking "Leave Race": set when the persistent takeover
    // control is pressed, consumed after the frame's lobby body has drawn.
    bool leavePending = false;
#endif
};

OnlineRoomUiState g_online;

constexpr const char *kCharacters[] = {
    "Diddy", "Timber", "Pipsy", "Tiptup", "Conker",
    "Bumper", "Banjo", "Krunch", "Drumstick", "T.T.",
};
constexpr const char *kVehicles[] = {"Car", "Hovercraft", "Plane"};
struct TrackChoice { const char *name; unsigned id; };
constexpr TrackChoice kTracks[] = {
    {"Ancient Lake", 5u},
    {"Fossil Canyon", 3u},
    {"Jungle Falls", 29u},
};

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

bool drawChoiceCombo(MdkrOnlineViewAction action,
                     const char *label, const char *preview,
                     const char *const *choices, unsigned count,
                     unsigned *selected) {
    ImGui::SetNextItemWidth(ui::kControlWidth());
    bool changed = false;
    maybeFocusAction(action);
    const bool open = ImGui::BeginCombo(label, preview);
    if (open) {
        for (unsigned i = 0u; i < count; ++i) {
            const bool isSelected = selected != nullptr && *selected == i;
            if (ImGui::Selectable(choices[i], isSelected)) {
                if (selected != nullptr) *selected = i;
                changed = true;
            }
            if (isSelected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    ui::SpeakFocusedItem(label, preview,
                         "Choose with keyboard, gamepad, mouse or touch.");
    return changed;
}

bool drawSelectionControl(const MdkrOnlineViewModel &model) {
    static unsigned character = 0u;
    static unsigned vehicle = 0u;
    static unsigned track = 0u;
    if (model.primary.action == MDKR_ONLINE_VIEW_ACTION_CHOOSE_CHARACTER) {
        if (drawChoiceCombo(model.primary.action,
                            "Choose Character", "Select a character…",
                            kCharacters,
                            sizeof(kCharacters) / sizeof(kCharacters[0]),
                            &character)) {
            dispatch(model.primary.action, 0u, character);
        }
        return true;
    }
    if (model.primary.action == MDKR_ONLINE_VIEW_ACTION_CHOOSE_VEHICLE) {
        if (drawChoiceCombo(model.primary.action,
                            "Choose Vehicle", "Select a vehicle…", kVehicles,
                            sizeof(kVehicles) / sizeof(kVehicles[0]), &vehicle)) {
            dispatch(model.primary.action, 0u, vehicle);
        }
        return true;
    }
    if (model.primary.action == MDKR_ONLINE_VIEW_ACTION_VOTE_TRACK) {
        const char *names[sizeof(kTracks) / sizeof(kTracks[0])];
        for (unsigned i = 0u; i < sizeof(kTracks) / sizeof(kTracks[0]); ++i) {
            names[i] = kTracks[i].name;
        }
        if (drawChoiceCombo(model.primary.action,
                            "Choose Track", "Vote for a track…", names,
                            sizeof(kTracks) / sizeof(kTracks[0]), &track)) {
            dispatch(model.primary.action, 0u, kTracks[track].id);
        }
        return true;
    }
    return false;
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
    } else if (action == MDKR_ONLINE_VIEW_ACTION_START_RACE) {
        dispatch(action, 0u, 1u);
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
        dispatch(action);
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

    const bool selectionDrawn = drawSelectionControl(model);
    if (!selectionDrawn && model.primary.action != timeoutAction &&
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

// Restrict the join-code field to the 6 digits the fallback code uses.
int betaDigitsOnlyFilter(ImGuiInputTextCallbackData *data) {
    return (data->EventChar < '0' || data->EventChar > '9') ? 1 : 0;
}

// Build the live adapter with the chosen journey (+ code for JOIN) and kick the
// entry action off immediately, so the chooser choice IS the create/join.
bool buildBetaLiveAdapter(MdkrOnlineJourney journey, const std::string &code) {
    const MdkrOnlineCompatibilityV1 compatibility = fakeCompatibility();
    std::unique_ptr<IMdkrOnlineAdapter> adapter =
        OnlineRoom_makeGatedLiveAdapter(compatibility, journey, code);
    if (!adapter) {
        g_online.betaBuildFailed = true;
        return false;
    }
    g_online.adapter = std::move(adapter);
    g_online.initialized = true;
    g_online.betaBuildFailed = false;
    g_online.betaCharacterTaken = false;
    dispatch(journey == MDKR_ONLINE_JOURNEY_CREATE
                 ? MDKR_ONLINE_VIEW_ACTION_CREATE_ROOM
                 : MDKR_ONLINE_VIEW_ACTION_JOIN_ROOM);
    return true;
}

void drawBetaChooser(LauncherState &state) {
    (void)state;
    ui::SectionHeader(
        "Play Online",
        "Race a friend over the internet: private, invite-only, 2 players, base "
        "racers, direct peer-to-peer. Pick a side to start; the lobby takes over "
        "once you connect.");
    if (g_online.betaBuildFailed) {
        ui::CautionBox(
            "Couldn't Start Online",
            "The online service could not be reached from this build. Local play "
            "and phone controllers still work; try the room again later.");
        ui::Gap(ui::kGapS);
    }
    if (g_online.betaStage == OnlineRoomUiState::BetaStage::Chooser) {
        if (ui::CardBegin("##beta-chooser", AppTheme::brandSky(), 0.0f)) {
            ImGui::TextUnformatted("How do you want to play?");
            ui::Gap(ui::kGapS);
            if (ui::BrandPrimaryButton("Host a Race", ui::kBtnFullWidth())) {
                buildBetaLiveAdapter(MDKR_ONLINE_JOURNEY_CREATE, std::string());
            }
            ui::SpeakFocusedItem("Host a Race", "Create a room",
                                 "Creates a private room and shows a code to share.");
            ui::Gap(ui::kGapS);
            if (ImGui::Button("Join a Race", ui::kBtnFullWidth())) {
                g_online.betaStage = OnlineRoomUiState::BetaStage::JoinCode;
                g_online.betaJoinCode[0] = '\0';
                g_online.betaBuildFailed = false;
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
            ImGui::SetNextItemWidth(ui::kControlWidth());
            ImGui::InputText("##beta-join-code", g_online.betaJoinCode,
                             sizeof(g_online.betaJoinCode),
                             ImGuiInputTextFlags_CallbackCharFilter,
                             betaDigitsOnlyFilter);
            ui::SpeakFocusedItem("Race code", g_online.betaJoinCode,
                                 "Type the 6 digits your host reads to you.");
            ui::Gap(ui::kGapS);
            const bool ready = std::strlen(g_online.betaJoinCode) == 6u;
            ImGui::BeginDisabled(!ready);
            if (ui::BrandPrimaryButton("Join", ui::kBtnFullWidth())) {
                buildBetaLiveAdapter(MDKR_ONLINE_JOURNEY_JOIN,
                                     std::string(g_online.betaJoinCode));
            }
            ImGui::EndDisabled();
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
    case MDKR_ONLINE_VIEW_RECOVERY: return "Lost connection — you can retry";
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
    case MDKR_ONLINE_VIEW_FAILURE_DIFFERENT_BUILD:
    case MDKR_ONLINE_VIEW_FAILURE_DIFFERENT_ROM:
    case MDKR_ONLINE_VIEW_FAILURE_DIFFERENT_SETTINGS:
    case MDKR_ONLINE_VIEW_FAILURE_UPDATE_REQUIRED:
        return "You and your friend are on different game versions. Use the same build.";
    case MDKR_ONLINE_VIEW_FAILURE_CONTROLLER_NEEDED:
        return "Set up a controller before racing online.";
    case MDKR_ONLINE_VIEW_FAILURE_CONNECTION_CHECK:
    case MDKR_ONLINE_VIEW_FAILURE_RELAY_CAPACITY:
    case MDKR_ONLINE_VIEW_FAILURE_NETWORKS_CANNOT_CONNECT:
        return "Couldn't connect directly. Check both networks and retry from a "
               "fresh invite.";
    case MDKR_ONLINE_VIEW_FAILURE_HOST_CLOSED:
        return "The host closed the room.";
    case MDKR_ONLINE_VIEW_FAILURE_ROOM_EXPIRED:
        return "The room expired. Create or join a new one.";
    case MDKR_ONLINE_VIEW_FAILURE_ENGINE_FAILED:
    case MDKR_ONLINE_VIEW_FAILURE_EPOCH_MISMATCH:
        return "The race couldn't start. Leave and try again.";
    case MDKR_ONLINE_VIEW_FAILURE_VERIFICATION_MISMATCH:
        return "The safety phrases didn't match — stopped for your protection. "
               "Leave and reconnect.";
    default:
        return "The connection was interrupted. You can retry or leave.";
    }
}

void drawBetaStatusLine(const MdkrOnlineViewModel &model) {
    if (ui::CardBegin("##beta-status-line", AppTheme::brandSky(), 0.0f)) {
        ImGui::TextUnformatted(betaStatusLine(model));
        if (model.kind == MDKR_ONLINE_VIEW_RECOVERY ||
            model.failure != MDKR_ONLINE_VIEW_FAILURE_NONE) {
            ui::TextSubtleWrapped("%s", betaFailureCopy(model.failure));
        } else {
            ui::TextSubtle("%u of 2 players • %u ready", model.member_count,
                           model.ready_count);
        }
    }
    ui::CardEnd();
}

// Draw-only QR of an arbitrary string, mirroring the phone-party invite QR.
void drawBetaQr(const std::string &text) {
    if (text.empty()) return;
    try {
        const qrcodegen::QrCode qr = qrcodegen::QrCode::encodeText(
            text.c_str(), qrcodegen::QrCode::Ecc::QUARTILE);
        const float maxSize = 220.0f * AppTheme::uiScale();
        const float available = ImGui::GetContentRegionAvail().x;
        const float size = (std::max)(96.0f, (std::min)(maxSize, available));
        const int quiet = 4;
        const int modules = qr.getSize() + quiet * 2;
        const float pixel = std::floor(size / static_cast<float>(modules));
        const float actual = pixel * static_cast<float>(modules);
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        ImDrawList *draw = ImGui::GetWindowDrawList();
        const ImU32 light = ImGui::GetColorU32(AppTheme::qrLight());
        const ImU32 dark = ImGui::GetColorU32(AppTheme::qrDark());
        draw->AddRectFilled(origin, ImVec2(origin.x + actual, origin.y + actual),
                            light);
        for (int y = 0; y < qr.getSize(); ++y) {
            for (int x = 0; x < qr.getSize(); ++x) {
                if (!qr.getModule(x, y)) continue;
                const float left = origin.x + (x + quiet) * pixel;
                const float top = origin.y + (y + quiet) * pixel;
                draw->AddRectFilled(ImVec2(left, top),
                                    ImVec2(left + pixel, top + pixel), dark);
            }
        }
        ImGui::Dummy(ImVec2(actual, actual));
    } catch (...) {
        ui::TextSubtleWrapped(
            "The QR code could not be shown. Share the 6-digit code instead.");
    }
}

// The creator's invite card: big shareable code + Copy + a QR of the invite
// link. Renders nothing for a joiner or until the room is Ready.
void drawBetaInviteCard() {
    std::string code;
    std::string url;
    if (!OnlineRoom_liveInvite(g_online.adapter.get(), &code, &url) ||
        code.empty()) {
        return;
    }
    ui::Gap(ui::kGapM);
    if (ui::CardBegin("##beta-invite", AppTheme::accent(), 0.0f)) {
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
        ImGui::TextUnformatted("Compare These Words");
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
}

// Selection, with the retail-only note and the SELECTION_CONFLICT surfaced as
// friendly text. Returns true when a selection control was drawn.
bool drawBetaSelection(const MdkrOnlineViewModel &model) {
    if (model.primary.action == MDKR_ONLINE_VIEW_ACTION_CHOOSE_CHARACTER) {
        static unsigned character = 0u;
        if (drawChoiceCombo(model.primary.action, "Choose Character",
                            "Select a racer…", kCharacters,
                            sizeof(kCharacters) / sizeof(kCharacters[0]),
                            &character)) {
            const MdkrOnlineAdapterStep step =
                dispatch(model.primary.action, 0u, character);
            g_online.betaCharacterTaken =
                step.error ==
                static_cast<uint32_t>(MDKR_ONLINE_ERROR_SELECTION_CONFLICT);
        }
        ui::TextSubtleWrapped(
            "Online beta uses the 10 base racers only, and each racer can be "
            "taken by just one player. You race full-screen from your own "
            "camera.");
        if (g_online.betaCharacterTaken) {
            ImGui::PushStyleColor(ImGuiCol_Text, AppTheme::bad());
            ui::TextSubtleWrapped(
                "That racer is already taken by the other player. Pick another.");
            ImGui::PopStyleColor();
        }
        return true;
    }
    return drawSelectionControl(model);
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
    if (model.kind != MDKR_ONLINE_VIEW_SELECTING) {
        g_online.betaCharacterTaken = false;
    }
    announceView(model);
    drawBetaStatusLine(model);
    ui::SectionHeader(model.title, model.explanation);

    if (model.kind == MDKR_ONLINE_VIEW_ROOM) {
        drawBetaInviteCard();
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

    const bool selectionDrawn = drawBetaSelection(model);
    if (!selectionDrawn && model.primary.action != timeoutAction &&
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
    // Beta interactive path: with no fake smoke requested, run the create/join
    // chooser and the live Online Room UX. The chooser builds the live adapter
    // on the player's choice, so before that g_online.adapter is intentionally
    // null (ensureInitialized deferred it) -- this must come first.
    if (!fakeEnabled()) {
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

// Non-blocking teardown of the live adapter. Its destructor joins the room /
// mesh / signal-client worker threads and closes the WebRTC data channels and
// WebSocket -- any of which can stall for seconds. Doing that on the ImGui/main
// thread is the observed beach-ball, so the live adapter is handed to a detached
// thread and destroyed there while the UI returns home immediately. The
// launcher thread never touches the adapter again after the hand-off, so
// single-owner off-thread destruction is safe. The deterministic fake owns no
// worker threads, so it is destroyed inline.
static void teardownAdapterAsync(std::unique_ptr<IMdkrOnlineAdapter> adapter) {
    if (!adapter) return;
    if (adapter->fakeAdapter() != nullptr) {
        adapter.reset();
        return;
    }
    std::thread([owned = std::move(adapter)]() mutable {
        owned.reset();
    }).detach();
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
    g_online.betaBuildFailed = false;
    g_online.betaCharacterTaken = false;
    Launcher_requestTab(state, kLauncherPanelPlay, kLauncherTabPlayer);
}

void OnlineRoom_requestLeave() { g_online.leavePending = true; }

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
