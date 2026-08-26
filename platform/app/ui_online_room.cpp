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
#include <cmath>
#include <string>
#include <thread>
#include <utility>

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
    bool betaCharacterTaken = false;
    // Which grid tile the SELECTION_CONFLICT above refers to. Only meaningful
    // while betaCharacterTaken is true -- the fake/live adapter exposes no
    // per-seat roster, so this is the local player's own last rejected pick,
    // not a live map of every racer's owner.
    unsigned betaCharacterTakenIndex = 0u;
    // Deferred, non-blocking "Leave Race": set when the persistent takeover
    // control is pressed, consumed after the frame's lobby body has drawn.
    bool leavePending = false;
    // Never-silent Start Race: set the instant the leader presses Start, cleared
    // when the room advances past the selection screen. Drives the "Starting…"
    // acknowledgement and, if the room stays in selection, a plain "couldn't
    // start" hint -- so Start Race is never a silent dead button.
    bool startRacePressed = false;
    double startRacePressedAt = 0.0;
    // Optimistic local racer/vehicle picks, staged until the authoritative
    // lobby snapshot confirms them (or an async refusal un-stages them).
    // 0xFF == nothing staged.
    std::uint8_t betaStagedCharacter = 0xFFu;
    std::uint8_t betaStagedVehicle = 0xFFu;
    // Optimistic host session configuration (mode / track / cup), rendered
    // immediately and reconciled from the next lobby snapshot. The sentinel
    // values mean "no pending change".
    std::uint8_t betaPendingMode = 0xFFu;
    std::uint16_t betaPendingTrack = 0xFFFFu;
    std::uint8_t betaPendingCup = 0xFFu;
    // The resolved track a convenience CHOOSE_VEHICLE correction was already
    // dispatched for, so an illegal-vehicle auto-fix fires once per track
    // change instead of once per frame.
    std::uint16_t betaVehicleFixTrack = 0xFFFFu;
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
        // The live beta lobby (drawBetaRoom) routes its Start Race press through
        // this same handler, so the correct vehicle mask must be sent whenever the
        // room can start a race -- BETA or PREVIEW. Gating the fix on PREVIEW alone
        // left the shipped beta demo compiling the buggy #else (0x01, Car-only),
        // which is exactly the silent two-machine stall this fixes. The #else keeps
        // the OFF/release build (neither macro) byte-identical.
#if MDKR_ENABLE_ONLINE_ROOM_PREVIEW || MDKR_ENABLE_ONLINE_BETA
        // BEGIN_LOADING's value is the legal (usable) vehicle mask for the race.
        // It must equal the resolved track's leveltable_vehicle_usable() mask, or
        // BOTH the lobby reducer's all_vehicles_legal() gate AND the engine's race
        // admission (mdkr_match_manifest_accepts_loaded_race) reject it -- the root
        // cause of "Start Race does nothing" when a player is not on the Car.
        // With a live lobby snapshot the resolved track is authoritative: the
        // cup schedule's next round in a tournament, otherwise the host's
        // configured track. The mask sent is the RAW table mask (the value the
        // manifest/admission path must carry unmodified) -- NEVER the 2-player
        // picker narrowing, which shapes the vehicle chips only. Without a
        // snapshot (fake-adapter preview), every curated preview track permits
        // all base vehicles, so 0x07 remains exact.
        unsigned startRaceVehicleMask = MDKR_ONLINE_PLAYER_VEHICLE_MASK;
#if MDKR_ENABLE_ONLINE_BETA
        {
            MdkrOnlineLobby startLobby{};
            if (mdkr_online_live_adapter_lobby(g_online.adapter.get(),
                                               &startLobby)) {
                const std::uint16_t resolved =
                    startLobby.mode == MDKR_ONLINE_MODE_TOURNAMENT
                        ? (startLobby.cup_id != MDKR_ONLINE_NO_CUP
                               ? mdkr_online_cup_track_id(startLobby.cup_id,
                                                          startLobby.race_index)
                               : static_cast<std::uint16_t>(MDKR_ONLINE_NO_VOTE))
                        : startLobby.configured_track;
                const MdkrOnlineTrackInfo *resolvedTrack =
                    resolved != MDKR_ONLINE_NO_VOTE
                        ? mdkr_online_track_by_id(resolved) : nullptr;
                if (resolvedTrack != nullptr) {
                    startRaceVehicleMask = resolvedTrack->vehicle_mask;
                }
            }
        }
#endif
        const MdkrOnlineAdapterStep step =
            dispatch(action, 0u, startRaceVehicleMask);
        std::fprintf(stderr,
                     "[START] start-race dispatched vehicleMask=0x%02x "
                     "accepted=%u error=%u rev=%u\n",
                     startRaceVehicleMask, step.accepted ? 1u : 0u,
                     static_cast<unsigned>(step.error), step.revision);
#if MDKR_ENABLE_ONLINE_BETA
        // Never-silent: acknowledge the press immediately so the button is not a
        // dead control while the room advances (or reveals why it could not).
        g_online.startRacePressed = true;
        g_online.startRacePressedAt = ImGui::GetTime();
#endif
#else
        dispatch(action, 0u, 1u);
#endif
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
            g_online.betaCharacterTaken = false;
            g_online.betaCharacterTakenIndex = 0u;
            g_online.betaStagedCharacter = 0xFFu;
            g_online.betaStagedVehicle = 0xFFu;
            g_online.betaPendingMode = 0xFFu;
            g_online.betaPendingTrack = 0xFFFFu;
            g_online.betaPendingCup = 0xFFu;
            g_online.betaVehicleFixTrack = 0xFFFFu;
            g_online.startRacePressed = false;
            g_online.startRacePressedAt = 0.0;
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

// If a composed status line ends in the "…" the copy catalog uses, swap that
// static ellipsis for the animated dot run above so a waiting line visibly
// animates. Any line that does not end in an ellipsis is left untouched.
void betaAnimateEllipsis(char *line, std::size_t size) {
    const std::size_t len = std::strlen(line);
    static const char kEllipsis[] = "\xE2\x80\xA6";  // U+2026 HORIZONTAL ELLIPSIS
    if (len < 3u || std::memcmp(line + len - 3u, kEllipsis, 3u) != 0) return;
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
    // m2: the gameplay-determinism developer seams change gameplay math on
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
    g_online.adapter = std::move(adapter);
    g_online.initialized = true;
    g_online.betaHostJourney = journey == MDKR_ONLINE_JOURNEY_CREATE;
    g_online.betaBuildFailed = false;
    g_online.betaBuildFailedReason[0] = '\0';
    g_online.betaCharacterTaken = false;
    g_online.betaCharacterTakenIndex = 0u;
    g_online.betaStagedCharacter = 0xFFu;
    g_online.betaStagedVehicle = 0xFFu;
    g_online.betaPendingMode = 0xFFu;
    g_online.betaPendingTrack = 0xFFFFu;
    g_online.betaPendingCup = 0xFFu;
    g_online.betaVehicleFixTrack = 0xFFFFu;
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
            // Grouped ECHO ("123 456") drawn ABOVE a plain 6-digit field, in the
            // same title font as the host's invite card so both sides read the
            // code the same way. The editable buffer stays the RAW digits on
            // purpose: inserting the group space into the buffer itself would
            // fight ImGui's cursor bookkeeping (the space sits at a fixed index,
            // so a mid-string edit or a backspace desyncs it) and break the
            // "value is exactly 6 digits" contract the Join path relies on. A
            // read-only echo gives the grouping affordance with none of that
            // risk. It reflects the buffer as of the start of the frame (the
            // field mutates it below); a one-frame lag on a decorative echo is
            // imperceptible. Filled slots draw in the normal color; unfilled "·"
            // placeholders are dimmed so the code shape reads without competing
            // with the digits already entered.
            const std::size_t shown = std::strlen(g_online.betaJoinCode);
            std::string filled;
            std::string rest;
            for (unsigned i = 0u; i < 6u; ++i) {
                std::string &seg = i < shown ? filled : rest;
                if (i == 3u) seg += ' ';  // visual grouping only
                if (i < shown) seg += g_online.betaJoinCode[i];
                else seg += "\xC2\xB7";  // U+00B7 MIDDLE DOT placeholder
            }
            ImGui::PushFont(AppTheme::fonts().title);
            if (!filled.empty()) ImGui::TextUnformatted(filled.c_str());
            if (!rest.empty()) {
                if (!filled.empty()) ImGui::SameLine(0.0f, 0.0f);
                ImGui::PushStyleColor(ImGuiCol_Text, AppTheme::subtle());
                ImGui::TextUnformatted(rest.c_str());
                ImGui::PopStyleColor();
            }
            ImGui::PopFont();
            ui::Gap(ui::kGapXS);
            ImGui::SetNextItemWidth(ui::kControlWidth());
            // CallbackCharFilter sanitizes both typing AND paste (see
            // betaDigitsOnlyFilter); the 7-byte buffer keeps the first 6 digits.
            ImGui::InputText("##beta-join-code", g_online.betaJoinCode,
                             sizeof(g_online.betaJoinCode),
                             ImGuiInputTextFlags_CallbackCharFilter,
                             betaDigitsOnlyFilter);
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
            ImGui::BeginDisabled(!ready);
            if (ui::BrandPrimaryButton("Join", ui::kBtnFullWidth())) {
                buildBetaLiveAdapter(state, MDKR_ONLINE_JOURNEY_JOIN,
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
    case MDKR_ONLINE_VIEW_RECOVERY:
#if MDKR_ENABLE_ONLINE_BETA
        if (model.failure == MDKR_ONLINE_VIEW_FAILURE_OPPONENT_LEFT)
            return "Opponent disconnected — this room is done";
        if (model.failure == MDKR_ONLINE_VIEW_FAILURE_OPPONENT_NEVER_STARTED)
            return "Opponent couldn't start — create a fresh invite";
        if (model.failure == MDKR_ONLINE_VIEW_FAILURE_CONNECTION_UNPLAYABLE)
            return "Connection became unplayable — this room is done";
#endif
        return "Lost connection — you can retry";
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
        const bool everyoneReady = model.member_count >= 2u &&
            model.ready_count == model.member_count;
        const char *next;
        if (everyoneReady) {
            next = model.local_member_is_leader
                ? "Everyone is ready — press Start Race"
                : "Everyone is ready — waiting for the host to start";
        } else if (model.member_count >= 2u &&
                   model.ready_count == model.member_count - 1u &&
                   model.primary.action ==
                       MDKR_ONLINE_VIEW_ACTION_CHANGE_SELECTION) {
            next = "Waiting for the other player to ready up";
        } else {
            next = "Choose your racer and vehicle, then Ready up";
        }
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
        {MDKR_ONLINE_VIEW_SELECTING, "Pick Racers"},
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
    const bool ready =
        OnlineRoom_liveInvite(g_online.adapter.get(), &code, &url) &&
        !code.empty();
    if (!ready) {
        if (!isHost) return;  // a joiner has no room of its own to share
        ui::Gap(ui::kGapM);
        if (ui::CardBegin("##beta-invite", AppTheme::accent(), 0.0f)) {
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
            // the up-to-220px QR), computed the same way drawBetaQr sizes the
            // QR, so the placeholder->real swap does not shift the layout under
            // the host's cursor.
            ui::Gap(ui::kGapS);
            ImGui::Dummy(ImVec2(0.0f, ui::kBtnSecondary().y));
            ui::Gap(ui::kGapS);
            const float qrMax = 220.0f * AppTheme::uiScale();
            const float qrSide = (std::max)(
                96.0f, (std::min)(qrMax, ImGui::GetContentRegionAvail().x));
            ImGui::Dummy(ImVec2(qrSide, qrSide));
            ui::TextSubtleWrapped(
                "Invite-only and expires. Keep this window open — the code and "
                "its QR appear here in a moment.");
        }
        ui::CardEnd();
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
constexpr unsigned kCharacterGridColumns = 5u;

// DKR's authentic trophy scoring (gTrophyRacePointsArray): 9/7/5/3/1 by
// placement, nothing below 5th. Mirrors the lobby reducer's kTrophyPoints.
constexpr unsigned kBetaTrophyPoints[MDKR_ONLINE_PLACEMENT_COUNT] = {
    9u, 7u, 5u, 3u, 1u, 0u, 0u, 0u};

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

const MdkrOnlineSeat *betaSeatFor(const MdkrOnlineLobby &lobby,
                                  std::uint64_t endpoint) {
    if (endpoint == 0u) return nullptr;
    for (unsigned i = 0u; i < MDKR_ONLINE_MAX_SEATS; ++i) {
        if (lobby.seats[i].occupied &&
            lobby.seats[i].endpoint_id == endpoint) {
            return &lobby.seats[i];
        }
    }
    return nullptr;
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

// The track the NEXT race will run on: the cup schedule's upcoming round in a
// tournament, otherwise the host's configured track. MDKR_ONLINE_NO_VOTE
// (0xFFFF) when the host has not decided yet.
std::uint16_t betaResolvedTrackId(const MdkrOnlineLobby &lobby) {
    if (lobby.mode == MDKR_ONLINE_MODE_TOURNAMENT) {
        if (lobby.cup_id == MDKR_ONLINE_NO_CUP) return MDKR_ONLINE_NO_VOTE;
        return mdkr_online_cup_track_id(lobby.cup_id, lobby.race_index);
    }
    return lobby.configured_track;
}

// The local player's effective (staged-or-authoritative) selections.
std::uint8_t betaEffectiveCharacter(const MdkrOnlineSeat *seat) {
    if (g_online.betaStagedCharacter != 0xFFu) {
        return g_online.betaStagedCharacter;
    }
    return seat != nullptr ? seat->character_id : MDKR_ONLINE_NO_CHARACTER;
}

std::uint8_t betaEffectiveVehicle(const MdkrOnlineSeat *seat) {
    if (g_online.betaStagedVehicle != 0xFFu) return g_online.betaStagedVehicle;
    return seat != nullptr ? seat->vehicle_id : MDKR_ONLINE_NO_VEHICLE;
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

// ---- Host settings card / joiner read-only mirror --------------------------
// The room leader exclusively controls mode, track and cup through the
// leader-only adapter setters; joiners see the same card rendered read-only
// from the snapshot ("Host is choosing…" until a value lands). Every host
// change renders optimistically and reconciles from the next snapshot; the
// reducer clears everyone's Ready on a config change, which is expected.

void betaSelectableChipColors(bool selected) {
    ImGui::PushStyleColor(ImGuiCol_Button,
                          selected ? AppTheme::navSelected()
                                   : AppTheme::field());
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                          selected ? AppTheme::navSelectedActive()
                                   : AppTheme::navHover());
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                          AppTheme::navSelectedActive());
}

void drawBetaSessionCard(const MdkrOnlineLobby &lobby, bool isLeader) {
    IMdkrOnlineAdapter *adapter = g_online.adapter.get();
    const std::uint8_t mode =
        isLeader && g_online.betaPendingMode != 0xFFu
            ? g_online.betaPendingMode : lobby.mode;
    const std::uint16_t configuredTrack =
        isLeader && g_online.betaPendingTrack != 0xFFFFu
            ? g_online.betaPendingTrack : lobby.configured_track;
    const std::uint8_t cup =
        isLeader && g_online.betaPendingCup != 0xFFu
            ? g_online.betaPendingCup : lobby.cup_id;
    const bool tournament = mode == MDKR_ONLINE_MODE_TOURNAMENT;

    if (!ui::CardBegin("##beta-session", AppTheme::accent(), 0.0f)) {
        ui::CardEnd();
        return;
    }
    ImGui::TextUnformatted("Race Settings");
    ui::TextSubtle(isLeader ? "You are the host — your picks apply to everyone"
                            : "Chosen by the host");
    ui::Gap(ui::kGapS);

    // Mode: Single Race / Trophy Tournament.
    if (isLeader) {
        const float full = ImGui::GetContentRegionAvail().x;
        const float half =
            (full - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
        const struct { const char *label; unsigned value; } modes[] = {
            {"Single Race", MDKR_ONLINE_MODE_SINGLE_RACE},
            {"Trophy Tournament", MDKR_ONLINE_MODE_TOURNAMENT},
        };
        for (unsigned m = 0u; m < 2u; ++m) {
            if (m != 0u) ImGui::SameLine();
            const bool selected = mode == modes[m].value;
            betaSelectableChipColors(selected);
            if (ImGui::Button(modes[m].label, ImVec2(half, 0.0f)) &&
                !selected) {
                g_online.betaPendingMode =
                    static_cast<std::uint8_t>(modes[m].value);
                mdkr_online_live_adapter_set_mode(adapter, modes[m].value);
            }
            ImGui::PopStyleColor(3);
            ui::SpeakFocusedItem(
                modes[m].label, selected ? "Selected" : "Not selected",
                "Sets the session mode for everyone in this room.");
        }
        ui::Gap(ui::kGapS);
    } else {
        ui::TextSubtle("Mode");
        ImGui::TextUnformatted(tournament ? "Trophy Tournament"
                                          : "Single Race");
        ui::Gap(ui::kGapS);
    }

    if (!tournament) {
        // Single Race: all 20 tracks, grouped by world.
        if (isLeader) {
            const float full = ImGui::GetContentRegionAvail().x;
            const float half =
                (full - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
            std::uint8_t lastWorld = 0xFFu;
            unsigned column = 0u;
            for (unsigned t = 0u; t < mdkr_online_track_count(); ++t) {
                const MdkrOnlineTrackInfo *track = mdkr_online_track_at(t);
                if (track == nullptr) continue;
                if (track->world != lastWorld) {
                    lastWorld = track->world;
                    column = 0u;
                    ui::Gap(ui::kGapXS);
                    ui::TextSubtle("%s",
                                   mdkr_online_world_name(track->world));
                }
                if (column % 2u == 1u) ImGui::SameLine();
                ++column;
                const bool selected = configuredTrack == track->id;
                betaSelectableChipColors(selected);
                ImGui::PushID(static_cast<int>(track->id));
                if (ImGui::Button(track->name, ImVec2(half, 0.0f)) &&
                    !selected) {
                    g_online.betaPendingTrack = track->id;
                    mdkr_online_live_adapter_set_config_track(adapter,
                                                              track->id);
                }
                ImGui::PopID();
                ImGui::PopStyleColor(3);
                char spoken[96];
                std::snprintf(spoken, sizeof(spoken), "%s, %s%s",
                              track->name,
                              mdkr_online_world_name(track->world),
                              selected ? ", selected" : "");
                ui::SpeakFocusedItem("Choose Track", spoken,
                                     "Sets the race track for everyone.");
            }
            if (configuredTrack == MDKR_ONLINE_NO_VOTE) {
                ui::Gap(ui::kGapS);
                ui::TextSubtleWrapped(
                    "Pick the track — Ready unlocks for everyone once it's "
                    "set.");
            }
        } else {
            ui::TextSubtle("Track");
            const MdkrOnlineTrackInfo *track =
                configuredTrack != MDKR_ONLINE_NO_VOTE
                    ? mdkr_online_track_by_id(configuredTrack) : nullptr;
            if (track != nullptr) {
                char line[96];
                std::snprintf(line, sizeof(line), "%s — %s", track->name,
                              mdkr_online_world_name(track->world));
                ImGui::TextUnformatted(line);
            } else {
                ImGui::TextUnformatted("Host is choosing…");
            }
        }
    } else {
        // Trophy Tournament: one of the 5 cups (4 scheduled races each,
        // authentic 9/7/5/3/1 points, champion after race 4).
        if (isLeader) {
            for (unsigned c = 0u; c < MDKR_ONLINE_CUP_COUNT; ++c) {
                const bool selected = cup == c;
                betaSelectableChipColors(selected);
                ImGui::PushID(static_cast<int>(c));
                if (ImGui::Button(mdkr_online_cup_name(c),
                                  ImVec2(-1.0f, 0.0f)) &&
                    !selected) {
                    g_online.betaPendingCup = static_cast<std::uint8_t>(c);
                    mdkr_online_live_adapter_set_cup(adapter, c);
                }
                ImGui::PopID();
                ImGui::PopStyleColor(3);
                ui::SpeakFocusedItem(
                    mdkr_online_cup_name(c),
                    selected ? "Selected" : "Not selected",
                    "Sets the 4-race trophy cup for everyone.");
                char rounds[160];
                rounds[0] = '\0';
                for (unsigned r = 0u; r < MDKR_ONLINE_CUP_ROUNDS; ++r) {
                    const MdkrOnlineTrackInfo *track =
                        mdkr_online_track_by_id(
                            mdkr_online_cup_track_id(c, r));
                    if (track == nullptr) continue;
                    std::snprintf(rounds + std::strlen(rounds),
                                  sizeof(rounds) - std::strlen(rounds),
                                  "%s%s", r == 0u ? "" : " · ", track->name);
                }
                ui::TextSubtleWrapped("%s", rounds);
                ui::Gap(ui::kGapXS);
            }
            if (cup == MDKR_ONLINE_NO_CUP) {
                ui::TextSubtleWrapped(
                    "Pick a cup — 4 races, 9/7/5/3/1 points, champion after "
                    "race 4.");
            }
        } else {
            ui::TextSubtle("Cup");
            if (cup != MDKR_ONLINE_NO_CUP && cup < MDKR_ONLINE_CUP_COUNT) {
                ImGui::TextUnformatted(mdkr_online_cup_name(cup));
                char rounds[160];
                rounds[0] = '\0';
                for (unsigned r = 0u; r < MDKR_ONLINE_CUP_ROUNDS; ++r) {
                    const MdkrOnlineTrackInfo *track =
                        mdkr_online_track_by_id(
                            mdkr_online_cup_track_id(cup, r));
                    if (track == nullptr) continue;
                    std::snprintf(rounds + std::strlen(rounds),
                                  sizeof(rounds) - std::strlen(rounds),
                                  "%s%s", r == 0u ? "" : " · ", track->name);
                }
                ui::TextSubtleWrapped("%s", rounds);
            } else {
                ImGui::TextUnformatted("Host is choosing…");
            }
        }
        // Series progress: which race the room lines up for next.
        if (lobby.cup_id != MDKR_ONLINE_NO_CUP &&
            lobby.mode == MDKR_ONLINE_MODE_TOURNAMENT) {
            const MdkrOnlineTrackInfo *nextTrack = mdkr_online_track_by_id(
                mdkr_online_cup_track_id(lobby.cup_id, lobby.race_index));
            if (nextTrack != nullptr) {
                ui::Gap(ui::kGapXS);
                ui::TextSubtle("Race %u of %u — next up: %s",
                               static_cast<unsigned>(lobby.race_index) + 1u,
                               static_cast<unsigned>(MDKR_ONLINE_CUP_ROUNDS),
                               nextTrack->name);
            }
        }
    }
    ui::CardEnd();
}

// ---- Vehicle row ------------------------------------------------------------
// Car / Hovercraft / Plane as selectable chips, enabled per the resolved
// track's 2-player PICKER mask (retail parity: Spaceport Alpha drops the
// hovercraft, Frosty Village drops the plane with 2 players). All three stay
// selectable while no track is resolved yet.
void drawBetaVehicleRow(const MdkrOnlineLobby &lobby,
                        const MdkrOnlineSeat *localSeat,
                        std::uint16_t resolvedTrack) {
    (void)lobby;
    if (localSeat == nullptr) return;
    const std::uint8_t pickerMask =
        resolvedTrack != MDKR_ONLINE_NO_VOTE
            ? mdkr_online_track_picker_mask(resolvedTrack, 2u)
            : static_cast<std::uint8_t>(MDKR_ONLINE_VEHICLE_BIT_ALL);
    const std::uint8_t current = betaEffectiveVehicle(localSeat);
    ImGui::TextUnformatted("Your Vehicle");
    const float full = ImGui::GetContentRegionAvail().x;
    const float third =
        (full - ImGui::GetStyle().ItemSpacing.x * 2.0f) / 3.0f;
    for (unsigned v = 0u; v < MDKR_ONLINE_PLAYER_VEHICLE_COUNT; ++v) {
        if (v != 0u) ImGui::SameLine();
        const bool allowed = (pickerMask & (1u << v)) != 0u;
        const bool selected = current == v;
        if (!allowed) ImGui::BeginDisabled();
        betaSelectableChipColors(selected && allowed);
        if (ImGui::Button(kVehicles[v], ImVec2(third, 0.0f)) && allowed &&
            !selected) {
            g_online.betaStagedVehicle = static_cast<std::uint8_t>(v);
            dispatch(MDKR_ONLINE_VIEW_ACTION_CHOOSE_VEHICLE, 0u, v);
        }
        ImGui::PopStyleColor(3);
        char spoken[64];
        std::snprintf(spoken, sizeof(spoken), "%s%s%s", kVehicles[v],
                      selected ? ", your pick" : "",
                      allowed ? "" : ", not allowed on this track");
        ui::SpeakFocusedItem("Choose Vehicle", spoken,
                             allowed
                                 ? "Chooses your vehicle for the next race."
                                 : "This track does not allow this vehicle.");
        if (!allowed) ImGui::EndDisabled();
    }
}
// rule, and the local player's selection/taken state -- the same "paint the
// state, then submit a plain hit target" shape drawRailPanelItem uses for the
// nav rail, kept consistent here so the grid reads as part of one system.
bool drawRacerTile(unsigned index, const char *name, unsigned accentHex,
                   bool isYourPick, bool isTaken, const ImVec2 &size,
                   const char *speakValue, const char *speakHelp) {
    ImGui::PushID(static_cast<int>(index));
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const ImVec2 max(min.x + size.x, min.y + size.y);
    const bool hovered = !isTaken &&
        ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) &&
        ImGui::IsMouseHoveringRect(min, max);
    ImDrawList *draw = ImGui::GetWindowDrawList();
    const float rounding = 8.0f * AppTheme::uiScale();
    ImVec4 fill = isTaken ? AppTheme::surface()
                : (isYourPick ? AppTheme::navSelected()
                             : (hovered ? AppTheme::navHover()
                                       : AppTheme::field()));
    if (isTaken) fill.w *= 0.55f;
    draw->AddRectFilled(min, max, ImGui::GetColorU32(fill), rounding);
    if (isYourPick) {
        draw->AddRect(min, max, ImGui::GetColorU32(AppTheme::accent()),
                      rounding, 0, 2.0f * AppTheme::uiScale());
    } else if (!isTaken) {
        const float ruleW = 4.0f * AppTheme::uiScale();
        draw->AddRectFilled(min, ImVec2(min.x + ruleW, max.y),
                            ImGui::GetColorU32(AppTheme::hex(accentHex)),
                            rounding, ImDrawFlags_RoundCornersLeft);
    }
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0, 0, 0, 0));
    if (isTaken) ImGui::BeginDisabled();
    const bool pressed = ImGui::Button("##tile", size);
    // Same call shape as drawActionButton: speak while the item is still the
    // "last item" and still inside BeginDisabled/EndDisabled, so a disabled
    // tile is silent for the same reason every other disabled control here is.
    ui::SpeakFocusedItem("Choose Character", speakValue, speakHelp);
    // Decorative label, drawn straight to the draw list (not another ImGui
    // item) so it cannot itself become "the last item".
    ImGui::PushFont(AppTheme::fonts().body);
    const ImVec2 nameSize = ImGui::CalcTextSize(name);
    ImGui::PopFont();
    const ImU32 textColor = ImGui::GetColorU32(
        isTaken ? AppTheme::subtle() : ImVec4(1, 1, 1, 1));
    const float nameY = isTaken ? min.y + size.y * 0.5f - nameSize.y - 1.0f
                                : min.y + (size.y - nameSize.y) * 0.5f;
    draw->AddText(ImVec2(min.x + (size.x - nameSize.x) * 0.5f, nameY),
                  textColor, name);
    if (isTaken) {
        ImGui::PushFont(AppTheme::fonts().small);
        const char *caption = "Taken";
        const ImVec2 capSize = ImGui::CalcTextSize(caption);
        draw->AddText(ImVec2(min.x + (size.x - capSize.x) * 0.5f,
                             min.y + size.y * 0.5f + 3.0f),
                      ImGui::GetColorU32(AppTheme::bad()), caption);
        ImGui::PopFont();
    }
    if (isTaken) ImGui::EndDisabled();
    ImGui::PopStyleColor(3);
    ImGui::PopID();
    return pressed;
}

// The character step as a 5x2 grid of racer tiles, replacing the plain
// dropdown. Dispatches the SAME MDKR_ONLINE_VIEW_ACTION_CHOOSE_CHARACTER the
// old combo did, from the same seat, with the same value semantics -- only
// the widget changed. With a lobby snapshot the grid is roster-aware: your
// authoritative (or staged) pick is highlighted and every racer another seat
// holds shows as Taken. The grid stays interactive for the whole selection
// phase -- re-picks are allowed; the reducer clears Ready on a change.
void drawCharacterGrid(MdkrOnlineViewAction action,
                       const MdkrOnlineLobby *lobby,
                       std::uint64_t localEndpoint) {
    constexpr unsigned kCount = sizeof(kCharacters) / sizeof(kCharacters[0]);
    const MdkrOnlineSeat *localSeat =
        lobby != nullptr ? betaSeatFor(*lobby, localEndpoint) : nullptr;
    const std::uint8_t yourPick = betaEffectiveCharacter(localSeat);
    maybeFocusAction(action);
    const float avail = ImGui::GetContentRegionAvail().x;
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float tileW =
        (avail - spacing * (kCharacterGridColumns - 1u)) /
        static_cast<float>(kCharacterGridColumns);
    const float tileH = ui::kBtnPrimary().y * 1.2f;
    for (unsigned i = 0u; i < kCount; ++i) {
        if (i % kCharacterGridColumns != 0u) ImGui::SameLine();
        bool isTaken = g_online.betaCharacterTaken &&
            g_online.betaCharacterTakenIndex == i;
        if (lobby != nullptr) {
            for (unsigned s = 0u; s < MDKR_ONLINE_MAX_SEATS; ++s) {
                const MdkrOnlineSeat &seat = lobby->seats[s];
                if (seat.occupied && seat.endpoint_id != localEndpoint &&
                    seat.character_id == i) {
                    isTaken = true;
                    break;
                }
            }
        }
        const bool isYourPick = !isTaken && yourPick == i;
        char value[64];
        std::snprintf(value, sizeof(value), "%s%s", kCharacters[i],
                     isTaken ? ", taken by the other player"
                            : isYourPick ? ", your pick" : "");
        const char *help = isTaken
            ? "Already taken by the other player. Pick another."
            : "Choose with keyboard, gamepad, mouse or touch.";
        if (drawRacerTile(i, kCharacters[i], kCharacterAccentHex[i],
                          isYourPick, isTaken, ImVec2(tileW, tileH), value,
                          help)) {
            g_online.betaStagedCharacter = static_cast<std::uint8_t>(i);
            g_online.betaCharacterTaken = false;
            g_online.betaCharacterTakenIndex = 0u;
            const MdkrOnlineAdapterStep step = dispatch(action, 0u, i);
            // The fake adapter surfaces a same-frame conflict; the live
            // adapter's arrives asynchronously through take_refusal (polled
            // in drawBetaRoom), which un-stages the optimistic pick.
            const bool conflict = step.error ==
                static_cast<uint32_t>(MDKR_ONLINE_ERROR_SELECTION_CONFLICT);
            if (conflict) {
                g_online.betaCharacterTaken = true;
                g_online.betaCharacterTakenIndex = i;
                g_online.betaStagedCharacter = 0xFFu;
            }
        }
        if (i % kCharacterGridColumns == kCharacterGridColumns - 1u) {
            ui::Gap(ui::kGapS);
        }
    }
}

// Legacy fallback selection (no lobby snapshot yet), with the retail-only
// note and the SELECTION_CONFLICT surfaced as friendly text. Returns true
// when a selection control was drawn. The snapshot-backed lobby uses
// drawBetaSelectingBody below instead.
bool drawBetaSelection(const MdkrOnlineViewModel &model) {
    if (model.primary.action == MDKR_ONLINE_VIEW_ACTION_CHOOSE_CHARACTER) {
        drawCharacterGrid(model.primary.action, nullptr, 0u);
        ui::TextSubtleWrapped(
            "Online beta uses the 10 base racers only, and each racer can be "
            "taken by just one player. You race full-screen from your own "
            "camera.");
        if (g_online.betaCharacterTaken) {
            ImGui::PushStyleColor(ImGuiCol_Text, AppTheme::bad());
            ui::TextSubtleWrapped("Taken — pick another racer.");
            ImGui::PopStyleColor();
        }
        return true;
    }
    return drawSelectionControl(model);
}

// ---- The MK8D-style lobby body (SELECTING, snapshot-backed) ----------------
// Roster strip, host settings card (or the joiner's read-only mirror), the
// always-interactive racer grid, the vehicle chips, and the Ready/Start
// region. Draws the view model's PRIMARY slot itself; the shared code below
// still renders secondary (Connection Details) and cancel (Leave Room).
void drawBetaSelectingBody(LauncherState &state,
                           const MdkrOnlineViewModel &model,
                           const MdkrOnlineLobby &lobby) {
    const bool isLeader = model.local_member_is_leader;
    const std::uint64_t localEndpoint = betaLocalEndpoint(lobby, isLeader);
    const MdkrOnlineSeat *localSeat = betaSeatFor(lobby, localEndpoint);

    // Reconcile optimistic staging with the authoritative snapshot.
    if (localSeat != nullptr) {
        if (g_online.betaStagedCharacter != 0xFFu &&
            localSeat->character_id == g_online.betaStagedCharacter) {
            g_online.betaStagedCharacter = 0xFFu;
        }
        if (g_online.betaStagedVehicle != 0xFFu &&
            localSeat->vehicle_id == g_online.betaStagedVehicle) {
            g_online.betaStagedVehicle = 0xFFu;
        }
    }
    if (g_online.betaPendingMode != 0xFFu &&
        lobby.mode == g_online.betaPendingMode) {
        g_online.betaPendingMode = 0xFFu;
    }
    if (g_online.betaPendingTrack != 0xFFFFu &&
        lobby.configured_track == g_online.betaPendingTrack) {
        g_online.betaPendingTrack = 0xFFFFu;
    }
    if (g_online.betaPendingCup != 0xFFu &&
        lobby.cup_id == g_online.betaPendingCup) {
        g_online.betaPendingCup = 0xFFu;
    }

    const std::uint16_t resolvedTrack = betaResolvedTrackId(lobby);
    const MdkrOnlineTrackInfo *resolvedInfo =
        resolvedTrack != MDKR_ONLINE_NO_VOTE
            ? mdkr_online_track_by_id(resolvedTrack) : nullptr;

    drawBetaRosterStrip(lobby, localEndpoint);
    ui::Gap(ui::kGapM);
    drawBetaSessionCard(lobby, isLeader);
    ui::Gap(ui::kGapM);

    // Racer grid: interactive for the whole selection phase.
    ImGui::TextUnformatted("Your Racer");
    drawCharacterGrid(MDKR_ONLINE_VIEW_ACTION_CHOOSE_CHARACTER, &lobby,
                      localEndpoint);
    if (g_online.betaCharacterTaken) {
        ImGui::PushStyleColor(ImGuiCol_Text, AppTheme::bad());
        ui::TextSubtleWrapped("Taken — pick another racer.");
        ImGui::PopStyleColor();
    }
    ui::TextSubtleWrapped(
        "Online beta uses the 10 base racers only, and each racer can be "
        "taken by just one player.");
    ui::Gap(ui::kGapM);

    // Vehicle chips + the convenience correction: when the host's track
    // change makes the current vehicle illegal, auto-select the track's
    // default vehicle once and say so (the reducer already cleared Ready).
    const std::uint8_t vehicle = betaEffectiveVehicle(localSeat);
    const std::uint8_t pickerMask =
        resolvedTrack != MDKR_ONLINE_NO_VOTE
            ? mdkr_online_track_picker_mask(resolvedTrack, 2u)
            : static_cast<std::uint8_t>(MDKR_ONLINE_VEHICLE_BIT_ALL);
    const bool vehicleChosen = vehicle < MDKR_ONLINE_PLAYER_VEHICLE_COUNT;
    const bool vehicleLegal =
        vehicleChosen && (pickerMask & (1u << vehicle)) != 0u;
    if (vehicleChosen && !vehicleLegal && resolvedInfo != nullptr) {
        if (g_online.betaVehicleFixTrack != resolvedTrack &&
            (pickerMask & (1u << resolvedInfo->default_vehicle)) != 0u) {
            g_online.betaVehicleFixTrack = resolvedTrack;
            g_online.betaStagedVehicle = resolvedInfo->default_vehicle;
            dispatch(MDKR_ONLINE_VIEW_ACTION_CHOOSE_VEHICLE, 0u,
                     resolvedInfo->default_vehicle);
        }
    } else if (vehicleLegal) {
        g_online.betaVehicleFixTrack = 0xFFFFu;
    }
    drawBetaVehicleRow(lobby, localSeat, resolvedTrack);
    if (vehicleChosen && !vehicleLegal && resolvedInfo != nullptr) {
        ImGui::PushStyleColor(ImGuiCol_Text, AppTheme::bad());
        char warning[128];
        std::snprintf(warning, sizeof(warning),
                      "%s doesn't allow the %s — switching you to the %s.",
                      resolvedInfo->name, kVehicles[vehicle],
                      kVehicles[resolvedInfo->default_vehicle]);
        ui::TextSubtleWrapped("%s", warning);
        ImGui::PopStyleColor();
    }
    ui::Gap(ui::kGapM);

    // Ready / Start region. The view model stays the single source of the
    // primary action; a picker-step primary renders as a disabled Ready with
    // the reason spelled out, so this screen is never silently locked.
    const MdkrOnlineViewAction primary = model.primary.action;
    const char *blockedReason = nullptr;
    if (primary == MDKR_ONLINE_VIEW_ACTION_CHOOSE_CHARACTER) {
        blockedReason = "Pick your racer to continue.";
    } else if (primary == MDKR_ONLINE_VIEW_ACTION_CHOOSE_VEHICLE) {
        blockedReason = "Pick your vehicle to continue.";
    } else if (primary == MDKR_ONLINE_VIEW_ACTION_VOTE_TRACK) {
        blockedReason = isLeader
            ? "Pick the track in Race Settings to unlock Ready."
            : "The host is choosing the track — Ready unlocks once it's set.";
    } else if (primary == MDKR_ONLINE_VIEW_ACTION_READY && !vehicleLegal) {
        blockedReason =
            "This track doesn't allow your vehicle — pick another to Ready "
            "up.";
    }
    if (blockedReason != nullptr) {
        ImGui::BeginDisabled();
        ui::BrandPrimaryButton("Ready", ui::kBtnFullWidth());
        ui::SpeakFocusedItem("Ready", "Unavailable", blockedReason);
        ImGui::EndDisabled();
        ui::TextSubtleWrapped("%s", blockedReason);
    } else if (drawActionButton(model.primary, true)) {
        handleAction(model.primary.action, state);
    }
}

// ---- Results / standings body (RESULTS, snapshot-backed) -------------------
// Single race: both placements + the leader's Race Again. Tournament: the
// round's points, cumulative standings, the next scheduled track, and the
// champion banner after race 4. Draws the PRIMARY slot itself.
void drawBetaResultsBody(LauncherState &state,
                         const MdkrOnlineViewModel &model,
                         const MdkrOnlineLobby &lobby) {
    const bool isLeader = model.local_member_is_leader;
    const std::uint64_t localEndpoint = betaLocalEndpoint(lobby, isLeader);
    drawBetaRosterStrip(lobby, localEndpoint);
    ui::Gap(ui::kGapM);

    const bool tournament = lobby.mode == MDKR_ONLINE_MODE_TOURNAMENT &&
                            lobby.cup_id != MDKR_ONLINE_NO_CUP;
    const bool finalRound =
        tournament && lobby.race_index >= MDKR_ONLINE_CUP_ROUNDS - 1u;

    // Occupied seats in standings order: cumulative points descending,
    // ties resolved to the LOWER seat index (matching the series reducer).
    unsigned order[MDKR_ONLINE_MAX_SEATS];
    unsigned seatCount = 0u;
    for (unsigned i = 0u; i < MDKR_ONLINE_MAX_SEATS; ++i) {
        if (!lobby.seats[i].occupied) continue;
        unsigned at = seatCount;
        while (at > 0u &&
               lobby.points[order[at - 1u]] < lobby.points[i]) {
            order[at] = order[at - 1u];
            --at;
        }
        order[at] = i;
        ++seatCount;
    }

    if (ui::CardBegin("##beta-results", AppTheme::accent(), 0.0f)) {
        char heading[112];
        if (tournament) {
            const char *cupName = mdkr_online_cup_name(lobby.cup_id);
            std::snprintf(heading, sizeof(heading), "Race %u of %u — %s",
                          static_cast<unsigned>(lobby.race_index) + 1u,
                          static_cast<unsigned>(MDKR_ONLINE_CUP_ROUNDS),
                          cupName != nullptr ? cupName : "Trophy Tournament");
        } else {
            std::snprintf(heading, sizeof(heading), "Race Results");
        }
        ImGui::TextUnformatted(heading);
        ui::Gap(ui::kGapS);

        if (tournament) {
            if (finalRound) {
                const unsigned champion = seatCount != 0u ? order[0] : 0u;
                char banner[96];
                const std::uint8_t championCharacter =
                    lobby.seats[champion].character_id;
                std::snprintf(
                    banner, sizeof(banner), "Champion: %s (%s)",
                    championCharacter < MDKR_ONLINE_CHARACTER_COUNT
                        ? kCharacters[championCharacter] : "Racer",
                    lobby.seats[champion].endpoint_id == localEndpoint
                        ? "You" : "Friend");
                ImGui::PushFont(AppTheme::fonts().title);
                ImGui::PushStyleColor(ImGuiCol_Text, AppTheme::accent());
                ImGui::TextUnformatted(banner);
                ImGui::PopStyleColor();
                ImGui::PopFont();
                ui::Gap(ui::kGapS);
                ImGui::TextUnformatted("Final Standings");
            } else {
                ImGui::TextUnformatted("Standings");
            }
            for (unsigned rank = 0u; rank < seatCount; ++rank) {
                const unsigned seatIndex = order[rank];
                const MdkrOnlineSeat &seat = lobby.seats[seatIndex];
                const std::uint8_t place = lobby.last_placements[seatIndex];
                const unsigned earned =
                    place < MDKR_ONLINE_PLACEMENT_COUNT
                        ? kBetaTrophyPoints[place] : 0u;
                char row[128];
                std::snprintf(
                    row, sizeof(row), "%s  %s (%s) — %u pts (+%u this race)",
                    betaPlacementLabel(rank),
                    seat.character_id < MDKR_ONLINE_CHARACTER_COUNT
                        ? kCharacters[seat.character_id] : "Racer",
                    seat.endpoint_id == localEndpoint ? "You" : "Friend",
                    static_cast<unsigned>(lobby.points[seatIndex]), earned);
                if (seat.character_id < MDKR_ONLINE_CHARACTER_COUNT) {
                    ImGui::PushStyleColor(
                        ImGuiCol_Text,
                        AppTheme::hex(
                            kCharacterAccentHex[seat.character_id]));
                    ImGui::TextUnformatted(row);
                    ImGui::PopStyleColor();
                } else {
                    ImGui::TextUnformatted(row);
                }
            }
            ui::Gap(ui::kGapS);
            if (finalRound) {
                ui::TextSubtleWrapped(
                    isLeader
                        ? "New Tournament starts the same cup again from race "
                          "1 with fresh points."
                        : "Waiting for the host to start a new tournament or "
                          "end the session.");
            } else {
                const MdkrOnlineTrackInfo *nextTrack =
                    mdkr_online_track_by_id(mdkr_online_cup_track_id(
                        lobby.cup_id,
                        static_cast<unsigned>(lobby.race_index) + 1u));
                if (nextTrack != nullptr) {
                    char nextLine[96];
                    std::snprintf(nextLine, sizeof(nextLine), "Next: %s",
                                  nextTrack->name);
                    ImGui::TextUnformatted(nextLine);
                }
                ui::TextSubtleWrapped(
                    isLeader
                        ? "Press Next Race when everyone is ready to "
                          "continue."
                        : "Waiting for the host to start the next race.");
            }
        } else {
            // Single race: placements, first place first.
            for (unsigned place = 0u; place < MDKR_ONLINE_MAX_SEATS;
                 ++place) {
                for (unsigned i = 0u; i < MDKR_ONLINE_MAX_SEATS; ++i) {
                    const MdkrOnlineSeat &seat = lobby.seats[i];
                    if (!seat.occupied ||
                        lobby.last_placements[i] != place) {
                        continue;
                    }
                    char row[96];
                    std::snprintf(
                        row, sizeof(row), "%s — %s (%s)",
                        betaPlacementLabel(place),
                        seat.character_id < MDKR_ONLINE_CHARACTER_COUNT
                            ? kCharacters[seat.character_id] : "Racer",
                        seat.endpoint_id == localEndpoint ? "You"
                                                          : "Friend");
                    if (seat.character_id < MDKR_ONLINE_CHARACTER_COUNT) {
                        ImGui::PushStyleColor(
                            ImGuiCol_Text,
                            AppTheme::hex(
                                kCharacterAccentHex[seat.character_id]));
                        ImGui::TextUnformatted(row);
                        ImGui::PopStyleColor();
                    } else {
                        ImGui::TextUnformatted(row);
                    }
                }
            }
            ui::Gap(ui::kGapS);
            ui::TextSubtleWrapped(
                isLeader ? "Race again on this track, or change the track "
                           "back in the lobby."
                         : "Waiting for the host to race again or change "
                           "the track.");
        }
    }
    ui::CardEnd();
    ui::Gap(ui::kGapM);

    if (isLeader) {
        if (drawActionButton(model.primary, true)) {
            handleAction(model.primary.action, state);
        }
    } else {
        // The joiner's honest wait state: the host owns the next step. The
        // view model's primary (Connection Details) renders below it.
        ImGui::BeginDisabled();
        ui::BrandPrimaryButton("Waiting for the Host…", ui::kBtnFullWidth());
        ui::SpeakFocusedItem("Waiting for the Host", "Unavailable",
                             "The host chooses the next race for the room.");
        ImGui::EndDisabled();
        ui::Gap(ui::kGapS);
        if (drawActionButton(model.primary, false)) {
            handleAction(model.primary.action, state);
        }
    }
}

// Never-silent Start Race: while the leader's press is pending and the room has
// not yet advanced past the selection screen, show a clear status. First a brief
// "Starting the race…" acknowledgement, then -- if the room stays in selection --
// a plain reason so the button is never a silent dead control. A hard stall in a
// later phase (LOADING/preflight/connecting) is surfaced by the adapter's real
// timeout card, which the generic timeout path below already renders.
void drawBetaStartRaceFeedback(const MdkrOnlineViewModel &model) {
    if (!g_online.startRacePressed) return;
    const double elapsed = ImGui::GetTime() - g_online.startRacePressedAt;
    ui::Gap(ui::kGapS);
    if (elapsed < 2.5) {
        if (ui::CardBegin("##beta-starting", AppTheme::accent(), 0.0f)) {
            ImGui::TextUnformatted("Starting the race…");
            ui::TextSubtleWrapped(
                "Getting everyone onto the same race start.");
        }
        ui::CardEnd();
    } else {
        const bool everyoneReady =
            model.member_count >= 2u && model.ready_count == model.member_count;
        ui::CautionBox(
            "Couldn't Start the Race Yet",
            everyoneReady
                ? "The room did not begin loading. Check Connection Details, then "
                  "press Start Race again."
                : "Both players must show Ready before the race can start. Wait "
                  "for the other player, then press Start Race again.");
    }
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

    // One-shot async command refusals: the live adapter reports a refused
    // lobby command (no auto-recovery) exactly once. A SET_CHARACTER
    // SELECTION_CONFLICT un-stages the optimistic pick and marks the tile --
    // the old same-frame error path never fires on the live adapter.
    std::uint32_t refusedCommand = 0u;
    std::uint32_t refusedError = 0u;
    if (mdkr_online_live_adapter_take_refusal(g_online.adapter.get(),
                                              &refusedCommand,
                                              &refusedError)) {
        if (refusedCommand == MDKR_ONLINE_SET_CHARACTER &&
            refusedError == MDKR_ONLINE_ERROR_SELECTION_CONFLICT) {
            if (g_online.betaStagedCharacter != 0xFFu) {
                g_online.betaCharacterTaken = true;
                g_online.betaCharacterTakenIndex =
                    g_online.betaStagedCharacter;
            }
            g_online.betaStagedCharacter = 0xFFu;
        } else {
            std::fprintf(stderr,
                         "[online-ui] room refused command=%u error=%u\n",
                         refusedCommand, refusedError);
        }
    }

    // The authoritative lobby snapshot behind the rich lobby/results bodies.
    MdkrOnlineLobby lobby{};
    const bool haveLobby =
        mdkr_online_live_adapter_lobby(g_online.adapter.get(), &lobby);

    if (model.kind != MDKR_ONLINE_VIEW_SELECTING) {
        g_online.betaCharacterTaken = false;
        g_online.betaCharacterTakenIndex = 0u;
        g_online.betaStagedCharacter = 0xFFu;
        g_online.betaStagedVehicle = 0xFFu;
        g_online.betaVehicleFixTrack = 0xFFFFu;
        g_online.betaPendingMode = 0xFFu;
        g_online.betaPendingTrack = 0xFFFFu;
        g_online.betaPendingCup = 0xFFu;
        // The room advanced out of selection (loading/countdown/racing) or reset:
        // the press was consumed, so retire the pending acknowledgement.
        g_online.startRacePressed = false;
    }
    announceView(model);
    drawBetaStatusLine(model, haveLobby ? &lobby : nullptr);
    ui::SectionHeader(model.title, model.explanation);

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

    drawBetaStartRaceFeedback(model);

    // Snapshot-backed rich bodies (they own the PRIMARY slot); everything
    // else keeps the generic control stack.
    bool primaryDrawn = false;
    if (haveLobby && model.kind == MDKR_ONLINE_VIEW_SELECTING &&
        lobby.phase == MDKR_ONLINE_LOBBY) {
        drawBetaSelectingBody(state, model, lobby);
        primaryDrawn = true;
    } else if (haveLobby && model.kind == MDKR_ONLINE_VIEW_RESULTS &&
               lobby.phase == MDKR_ONLINE_RESULTS) {
        drawBetaResultsBody(state, model, lobby);
        primaryDrawn = true;
    } else {
        primaryDrawn = drawBetaSelection(model);
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
    // Retract any pending engine-race-boot handoff ON THE LAUNCHER THREAD
    // before the adapter crosses to the teardown thread: the destructor's own
    // retract is only a backstop, and a detached-thread destruction would race
    // OnlineRoom_pollEngineRaceBoot() against a dying adapter.
    (void)mdkr_online_live_adapter_retract_race_boot(adapter.get());
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
    g_online.betaHostJourney = false;
    g_online.betaBuildFailed = false;
    g_online.betaCharacterTaken = false;
    g_online.betaCharacterTakenIndex = 0u;
    g_online.betaStagedCharacter = 0xFFu;
    g_online.betaStagedVehicle = 0xFFu;
    g_online.betaPendingMode = 0xFFu;
    g_online.betaPendingTrack = 0xFFFFu;
    g_online.betaPendingCup = 0xFFu;
    g_online.betaVehicleFixTrack = 0xFFFFu;
    g_online.startRacePressed = false;
    g_online.startRacePressedAt = 0.0;
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
