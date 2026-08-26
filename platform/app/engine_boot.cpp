// engine_boot.cpp — synthesize a CLI invocation and hand it to the engine.
//
// The launcher never reimplements engine startup: it builds the exact argv a
// user would have typed and calls mdkr64_headless_main(). Anything the CLI can
// express, the launcher expresses the same way, so there is one boot path to
// reason about and no second one to drift.
#include "engine_entry.h"

#include "app_restart.h"    // AppRestart_getEnv, AppRestart_setEnv
#include "app_config.h"
#include "character_png_validation.h"
#include "fs_utf8.h"
#include "modern_character_registry.h"
#include "user_paths.h"
#include "video_config.h"   // MdkrVideoMode, mdkr_video_schema

#include <cstdio>
#include <cstring>
#include <array>
#include <map>
#include <string>
#include <vector>

namespace {

std::array<std::string, 4> s_launcherCharacterEnvironment;
constexpr size_t kCharacterTuningCount = 74;
std::array<std::array<std::string, kCharacterTuningCount>, 4>
    s_launcherCharacterTuningEnvironment;
std::map<std::string, std::string> s_launcherPackageTuningEnvironment;

const char *characterPreviewContextName(MdkrCharacterPreviewContext context) {
    switch (context) {
        case MDKR_CHARACTER_PREVIEW_SELECT: return "select";
        case MDKR_CHARACTER_PREVIEW_CAR: return "car";
        case MDKR_CHARACTER_PREVIEW_HOVERCRAFT: return "hovercraft";
        case MDKR_CHARACTER_PREVIEW_PLANE: return "plane";
        default: return nullptr;
    }
}

const char *characterPreviewPoseSemantic(MdkrCharacterPreviewPose pose) {
    switch (pose) {
        case MDKR_CHARACTER_PREVIEW_POSE_LIVE: return nullptr;
#define MDKR_CHARACTER_PREVIEW_POSE_CASE(suffix, semantic, label) \
        case MDKR_CHARACTER_PREVIEW_POSE_##suffix: return semantic;
        MDKR_MODERN_CHARACTER_INSPECTION_SEMANTICS(
            MDKR_CHARACTER_PREVIEW_POSE_CASE)
#undef MDKR_CHARACTER_PREVIEW_POSE_CASE
        default: return nullptr;
    }
}

const char *characterPreviewLightingName(
    MdkrWorkshopPreviewLighting lighting) {
    switch (lighting) {
        case MDKR_WORKSHOP_PREVIEW_LIGHTING_NEUTRAL: return "neutral";
        case MDKR_WORKSHOP_PREVIEW_LIGHTING_BRIGHT: return "bright";
        case MDKR_WORKSHOP_PREVIEW_LIGHTING_LOW_KEY: return "low-key";
        case MDKR_WORKSHOP_PREVIEW_LIGHTING_BACKLIT: return "backlit";
        default: return nullptr;
    }
}

const char *characterPreviewCaptureKindName(
    MdkrCharacterPreviewCaptureKind kind) {
    switch (kind) {
        case MDKR_CHARACTER_PREVIEW_CAPTURE_SCENE: return "scene";
        case MDKR_CHARACTER_PREVIEW_CAPTURE_MODEL_ALPHA:
            return "model-alpha";
        default: return nullptr;
    }
}

bool characterPreviewCapturePathValid(const char *path) {
    if (path == nullptr || path[0] == '\0') return true;
    const size_t length = std::strlen(path);
    int exists = 0;
    if (length < 4u || length >= 1024u ||
        std::strcmp(path + length - 4u, ".png") != 0) {
        return false;
    }
    (void)mdkr_path_query_utf8(path, &exists, nullptr, nullptr);
    return !exists;
}

bool characterPreviewPng(const char *path, unsigned expectedWidth,
                         unsigned expectedHeight,
                         unsigned expectedColourType,
                         unsigned long long &bytes) {
    constexpr size_t kMaximumCaptureBytes = 32u * 1024u * 1024u;
    std::array<unsigned char, 8192> block{};
    std::vector<unsigned char> payload;
    bytes = 0u;
    if (path == nullptr || path[0] == '\0') return false;
    FILE *file = mdkr_fopen_utf8(path, "rb");
    if (file == nullptr) return false;
    bool bounded = true;
    for (;;) {
        const size_t count =
            std::fread(block.data(), 1u, block.size(), file);
        if (count != 0u) {
            if (payload.size() > kMaximumCaptureBytes - count) {
                bounded = false;
                break;
            }
            payload.insert(payload.end(), block.begin(), block.begin() + count);
        }
        if (count != block.size()) break;
    }
    const bool readOk = std::ferror(file) == 0;
    const bool closeOk = std::fclose(file) == 0;
    CharacterPngValidation::Info info;
    std::string error;
    if (!bounded || !readOk || !closeOk ||
        !CharacterPngValidation::validate(
            payload.data(), payload.size(), expectedWidth, expectedHeight,
            info, error) || info.colourType != expectedColourType) return false;
    bytes = payload.size();
    return true;
}

struct CharacterTuningKey {
    const char *environment_suffix;
    const char *preference_suffix;
    const char *fallback;
};

constexpr CharacterTuningKey kCharacterTuningKeys[kCharacterTuningCount] = {
    {"SCALE", "scale", "1"},
    {"OFFSET_X", "offset_x", "0"},
    {"OFFSET_Y", "offset_y", "0"},
    {"OFFSET_Z", "offset_z", "0"},
    {"ROTATION_X", "rotation_x", "0"},
    {"ROTATION_Y", "rotation_y", "0"},
    {"ROTATION_Z", "rotation_z", "0"},
    {"ANIMATION_SPEED", "animation_speed", "1"},
    {"LOD_BIAS", "lod_bias", "0"},
    {"VEHICLE_MASK", "vehicle_mask", "7"},
    {"SELECT_SCALE", "select_scale", "1"},
    {"SELECT_OFFSET_X", "select_offset_x", "0"},
    {"SELECT_OFFSET_Y", "select_offset_y", "0"},
    {"SELECT_OFFSET_Z", "select_offset_z", "0"},
    {"SELECT_ROTATION_X", "select_rotation_x", "0"},
    {"SELECT_ROTATION_Y", "select_rotation_y", "0"},
    {"SELECT_ROTATION_Z", "select_rotation_z", "0"},
    {"CAR_SCALE", "car_scale", "1"},
    {"CAR_OFFSET_X", "car_offset_x", "0"},
    {"CAR_OFFSET_Y", "car_offset_y", "0"},
    {"CAR_OFFSET_Z", "car_offset_z", "0"},
    {"CAR_ROTATION_X", "car_rotation_x", "0"},
    {"CAR_ROTATION_Y", "car_rotation_y", "0"},
    {"CAR_ROTATION_Z", "car_rotation_z", "0"},
    {"HOVERCRAFT_SCALE", "hovercraft_scale", "1"},
    {"HOVERCRAFT_OFFSET_X", "hovercraft_offset_x", "0"},
    {"HOVERCRAFT_OFFSET_Y", "hovercraft_offset_y", "0"},
    {"HOVERCRAFT_OFFSET_Z", "hovercraft_offset_z", "0"},
    {"HOVERCRAFT_ROTATION_X", "hovercraft_rotation_x", "0"},
    {"HOVERCRAFT_ROTATION_Y", "hovercraft_rotation_y", "0"},
    {"HOVERCRAFT_ROTATION_Z", "hovercraft_rotation_z", "0"},
    {"PLANE_SCALE", "plane_scale", "1"},
    {"PLANE_OFFSET_X", "plane_offset_x", "0"},
    {"PLANE_OFFSET_Y", "plane_offset_y", "0"},
    {"PLANE_OFFSET_Z", "plane_offset_z", "0"},
    {"PLANE_ROTATION_X", "plane_rotation_x", "0"},
    {"PLANE_ROTATION_Y", "plane_rotation_y", "0"},
    {"PLANE_ROTATION_Z", "plane_rotation_z", "0"},
    {"CAR_HAND_LEFT_X", "car_hand_left_x", "0"},
    {"CAR_HAND_LEFT_Y", "car_hand_left_y", "0"},
    {"CAR_HAND_LEFT_Z", "car_hand_left_z", "0"},
    {"CAR_HAND_RIGHT_X", "car_hand_right_x", "0"},
    {"CAR_HAND_RIGHT_Y", "car_hand_right_y", "0"},
    {"CAR_HAND_RIGHT_Z", "car_hand_right_z", "0"},
    {"CAR_FOOT_LEFT_X", "car_foot_left_x", "0"},
    {"CAR_FOOT_LEFT_Y", "car_foot_left_y", "0"},
    {"CAR_FOOT_LEFT_Z", "car_foot_left_z", "0"},
    {"CAR_FOOT_RIGHT_X", "car_foot_right_x", "0"},
    {"CAR_FOOT_RIGHT_Y", "car_foot_right_y", "0"},
    {"CAR_FOOT_RIGHT_Z", "car_foot_right_z", "0"},
    {"HOVERCRAFT_HAND_LEFT_X", "hovercraft_hand_left_x", "0"},
    {"HOVERCRAFT_HAND_LEFT_Y", "hovercraft_hand_left_y", "0"},
    {"HOVERCRAFT_HAND_LEFT_Z", "hovercraft_hand_left_z", "0"},
    {"HOVERCRAFT_HAND_RIGHT_X", "hovercraft_hand_right_x", "0"},
    {"HOVERCRAFT_HAND_RIGHT_Y", "hovercraft_hand_right_y", "0"},
    {"HOVERCRAFT_HAND_RIGHT_Z", "hovercraft_hand_right_z", "0"},
    {"HOVERCRAFT_FOOT_LEFT_X", "hovercraft_foot_left_x", "0"},
    {"HOVERCRAFT_FOOT_LEFT_Y", "hovercraft_foot_left_y", "0"},
    {"HOVERCRAFT_FOOT_LEFT_Z", "hovercraft_foot_left_z", "0"},
    {"HOVERCRAFT_FOOT_RIGHT_X", "hovercraft_foot_right_x", "0"},
    {"HOVERCRAFT_FOOT_RIGHT_Y", "hovercraft_foot_right_y", "0"},
    {"HOVERCRAFT_FOOT_RIGHT_Z", "hovercraft_foot_right_z", "0"},
    {"PLANE_HAND_LEFT_X", "plane_hand_left_x", "0"},
    {"PLANE_HAND_LEFT_Y", "plane_hand_left_y", "0"},
    {"PLANE_HAND_LEFT_Z", "plane_hand_left_z", "0"},
    {"PLANE_HAND_RIGHT_X", "plane_hand_right_x", "0"},
    {"PLANE_HAND_RIGHT_Y", "plane_hand_right_y", "0"},
    {"PLANE_HAND_RIGHT_Z", "plane_hand_right_z", "0"},
    {"PLANE_FOOT_LEFT_X", "plane_foot_left_x", "0"},
    {"PLANE_FOOT_LEFT_Y", "plane_foot_left_y", "0"},
    {"PLANE_FOOT_LEFT_Z", "plane_foot_left_z", "0"},
    {"PLANE_FOOT_RIGHT_X", "plane_foot_right_x", "0"},
    {"PLANE_FOOT_RIGHT_Y", "plane_foot_right_y", "0"},
    {"PLANE_FOOT_RIGHT_Z", "plane_foot_right_z", "0"},
};

const char *modeFlag(int mode) {
    switch (mode) {
        case MDKR_VIDEO_MODE_PURE:       return "--pure";
        case MDKR_VIDEO_MODE_RESTORED:   return "--restored";
        case MDKR_VIDEO_MODE_REMASTERED: return "--remastered";
        default:                         return nullptr;  // Custom / unset
    }
}

std::string legacyCharacterTuning(
    const char *packageId, const CharacterTuningKey &mapping) {
    for (int player = 0; player < 4; ++player) {
        const std::string assignment =
            AppConfig::get("custom_character_p" + std::to_string(player + 1));
        if (assignment != packageId) continue;
        const std::string legacyKey = "custom_character_p" +
            std::to_string(player + 1) + "_" + mapping.preference_suffix;
        const std::string legacy = AppConfig::get(legacyKey);
        if (!legacy.empty()) return legacy;
    }
    return mapping.fallback;
}

void handoffCharacterPackageTuning() {
    char directory[MDKR_MODERN_CHARACTER_PATH_MAX];
    MdkrModernCharacterRegistry registry{};
    std::string existing;
    if (!mdkr_user_characters_directory(directory, sizeof(directory)) ||
        mdkr_modern_character_registry_init(&registry, directory) != 0) {
        return;
    }
    for (int index = 0;
         index < mdkr_modern_character_registry_count(&registry); ++index) {
        const MdkrModernCharacterEntry *entry =
            mdkr_modern_character_registry_entry(&registry, index);
        if (entry == nullptr) continue;
        const std::string environmentPrefix =
            "MDKR_CUSTOM_CHARACTER_PROFILE_" + std::string(entry->id);
        const std::string preferencePrefix =
            "custom_character_profile_" + std::string(entry->id) + "_";
        for (const CharacterTuningKey &mapping : kCharacterTuningKeys) {
            const std::string variable = environmentPrefix + "_" +
                mapping.environment_suffix;
            const auto previous =
                s_launcherPackageTuningEnvironment.find(variable);
            const bool hasExisting =
                AppRestart_getEnv(variable.c_str(), existing) &&
                !existing.empty();
            if (hasExisting &&
                (previous == s_launcherPackageTuningEnvironment.end() ||
                 existing != previous->second)) {
                continue;
            }
            const std::string configured = AppConfig::get(
                preferencePrefix + mapping.preference_suffix);
            const std::string value = configured.empty()
                ? legacyCharacterTuning(entry->id, mapping) : configured;
            AppRestart_setEnv(variable.c_str(), value.c_str());
            s_launcherPackageTuningEnvironment[variable] = value;
        }
    }
    mdkr_modern_character_registry_shutdown(&registry);
}

}  // namespace

int mdkr64_engine_boot(const MdkrBootConfig *cfg) {
    // Own every string for the lifetime of the call: argv must stay valid for
    // the whole engine run, and the engine keeps pointers into it (romPath is
    // stored, not copied, by main_pc.c).
    std::vector<std::string> owned;
    AppEnvironmentTransaction previewEnvironment;
    g_mdkrCharacterPreviewResult = nullptr;
    owned.push_back("mdkr64");

    if (cfg != nullptr) {
        if (cfg->automation_ticks > 0 && cfg->automation_frames > 0) {
            std::fprintf(stderr,
                         "[app] boot rejected conflicting tick/frame limits\n");
            return 2;
        }
        if (cfg->character_preview_context != MDKR_CHARACTER_PREVIEW_NONE &&
            (cfg->character_preview_package == nullptr ||
             cfg->character_preview_package[0] == '\0' ||
             characterPreviewContextName(cfg->character_preview_context) ==
                 nullptr ||
             cfg->character_preview_players < 1 ||
             cfg->character_preview_players > 4 ||
             cfg->character_preview_pose <
                 MDKR_CHARACTER_PREVIEW_POSE_LIVE ||
             cfg->character_preview_pose >=
                 MDKR_CHARACTER_PREVIEW_POSE_COUNT ||
             cfg->character_preview_pose_phase_milli > 1000u ||
             cfg->character_preview_view_yaw_degrees < -180 ||
             cfg->character_preview_view_yaw_degrees > 180 ||
             cfg->character_preview_view_pitch_degrees < -45 ||
             cfg->character_preview_view_pitch_degrees > 45 ||
             characterPreviewLightingName(
                 cfg->character_preview_lighting) == nullptr ||
             characterPreviewCaptureKindName(
                 cfg->character_preview_capture_kind) == nullptr ||
             !characterPreviewCapturePathValid(
                 cfg->character_preview_capture_png) ||
             ((cfg->character_preview_capture_png == nullptr ||
               cfg->character_preview_capture_png[0] == '\0') &&
              cfg->character_preview_capture_kind !=
                  MDKR_CHARACTER_PREVIEW_CAPTURE_SCENE) ||
             (cfg->character_preview_context ==
                  MDKR_CHARACTER_PREVIEW_SELECT &&
              (cfg->character_preview_view_yaw_degrees != 0 ||
               cfg->character_preview_view_pitch_degrees != 0)) ||
             (cfg->character_preview_pose ==
                  MDKR_CHARACTER_PREVIEW_POSE_LIVE &&
              (cfg->character_preview_pose_phase_milli != 0u ||
               cfg->character_preview_view_yaw_degrees != 0 ||
               cfg->character_preview_view_pitch_degrees != 0 ||
               cfg->character_preview_lighting !=
                   MDKR_WORKSHOP_PREVIEW_LIGHTING_NEUTRAL ||
               (cfg->character_preview_capture_png != nullptr &&
                cfg->character_preview_capture_png[0] != '\0'))) ||
             (cfg->character_preview_pose !=
                  MDKR_CHARACTER_PREVIEW_POSE_LIVE &&
              characterPreviewPoseSemantic(cfg->character_preview_pose) ==
                  nullptr))) {
            std::fprintf(stderr,
                         "[app] boot rejected invalid character preview\n");
            return 2;
        }
        if (cfg->rom_path != nullptr && cfg->rom_path[0] != '\0') {
            owned.push_back("--rom");
            owned.push_back(cfg->rom_path);
        }
        if (const char *mf = modeFlag(cfg->video_mode)) {
            owned.push_back(mf);
        }
        if (cfg->window_width > 0 && cfg->window_height > 0) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%dx%d", cfg->window_width, cfg->window_height);
            owned.push_back("--window-size");
            owned.push_back(buf);
        }
        if (cfg->automation_ticks > 0) {
            owned.push_back("--headless-ticks");
            owned.push_back(std::to_string(cfg->automation_ticks));
        }
        if (cfg->automation_frames > 0) {
            owned.push_back("--headless-frames");
            owned.push_back(std::to_string(cfg->automation_frames));
        }
        if (cfg->input_script != nullptr && cfg->input_script[0] != '\0') {
            owned.push_back("--input-script");
            owned.push_back(cfg->input_script);
        }
        // Automation-only: let an app-shell run capture frames the same way the
        // CLI does, so a gate can inspect what the app actually presents while
        // its overlay is open. Inert unless the variable is set.
        if (cfg->automation_ticks > 0 || cfg->automation_frames > 0) {
            const char *dump = std::getenv("MDKR_APP_AUTOPLAY_DUMP_FRAMES");
            if (dump != nullptr && dump[0] != '\0') {
                owned.push_back("--dump-frames");
                owned.push_back(dump);
            }
        }
        // Staged RESTART-scope settings ride in as --video-set, which sits at
        // MDKR_VIDEO_SOURCE_CLI — above the ini the panel also wrote them to.
        // Same value from both layers, so precedence is a no-op here; passing
        // them explicitly is what makes the choice apply to THIS boot.
        int n = cfg->override_count;
        if (n > MDKR_BOOT_MAX_OVERRIDES) n = MDKR_BOOT_MAX_OVERRIDES;
        for (int i = 0; i < n; ++i) {
            if (cfg->overrides[i] == nullptr || cfg->overrides[i][0] == '\0') continue;
            owned.push_back("--video-set");
            owned.push_back(cfg->overrides[i]);
        }
    }

    std::vector<char *> argv;
    argv.reserve(owned.size() + 1);
    for (std::string &s : owned) argv.push_back(&s[0]);
    argv.push_back(nullptr);

    // The shell initialized video config before showing Settings. Commit the
    // settings it staged, plus these synthesized CLI overrides, to the active
    // engine config now. main_pc.c's init remains idempotent and publishes this
    // already-resolved state before any presentation subsystem can latch it.
    if (!mdkr_video_config_handoff_to_engine(
            static_cast<int>(owned.size()), argv.data())) {
        std::fprintf(stderr,
                     "[app] video-config handoff was missing or repeated; "
                     "engine boot stopped\n");
        return 2;
    }

    // Camera.Obstruction is the one setting the engine does not read from the
    // resolved config: game/src/camera_obstruction_runtime.c latches
    // MDKR_CAMERA_OBSTRUCTION at boot. The launcher's value rides that same
    // seam, and only where nothing has claimed it -- an environment value
    // outranks the launcher (video_config.h's precedence ranking), so a set
    // variable is left exactly as the caller wrote it, diagnostic arms
    // included.
    std::string existing;
    if (!AppRestart_getEnv("MDKR_CAMERA_OBSTRUCTION", existing) ||
        existing.empty()) {
        const MdkrVideoConfig *resolved = mdkr_video_config_current();
        if (resolved != nullptr) {
            AppRestart_setEnv(
                "MDKR_CAMERA_OBSTRUCTION",
                resolved->values[MDKR_VIDEO_CAMERA_OBSTRUCTION].text);
        }
    }

    /* Custom characters are local presentation choices. Persist them in the
     * launcher preferences, then hand them to the engine through the same
     * diagnostic override the CLI supports. An explicit caller environment
     * remains higher priority. */
    handoffCharacterPackageTuning();
    for (int player = 0; player < 4; ++player) {
        const std::string variable =
            "MDKR_CUSTOM_CHARACTER_P" + std::to_string(player + 1);
        const bool hasExisting =
            AppRestart_getEnv(variable.c_str(), existing) && !existing.empty();
        const std::string key =
            "custom_character_p" + std::to_string(player + 1);
        if (!hasExisting || existing == s_launcherCharacterEnvironment[player]) {
            const std::string selected = AppConfig::get(key);
            AppRestart_setEnv(variable.c_str(), selected.c_str());
            s_launcherCharacterEnvironment[player] = selected;
        }

        for (size_t tuning = 0; tuning < kCharacterTuningCount; ++tuning) {
            const CharacterTuningKey &mapping = kCharacterTuningKeys[tuning];
            const std::string tuningVariable = variable + "_" +
                                               mapping.environment_suffix;
            const bool tuningHasExisting =
                AppRestart_getEnv(tuningVariable.c_str(), existing) &&
                !existing.empty();
            if (tuningHasExisting &&
                existing != s_launcherCharacterTuningEnvironment[player][tuning]) {
                continue;
            }
            /* Launcher-authored tuning is package keyed above. Retain Pn
             * variables only for explicit CLI diagnostics; scrub values this
             * launcher wrote on an earlier in-process boot. */
            if (!tuningHasExisting ||
                existing == s_launcherCharacterTuningEnvironment[player][tuning]) {
                AppRestart_setEnv(tuningVariable.c_str(), "");
                s_launcherCharacterTuningEnvironment[player][tuning].clear();
            }
        }
    }

    if (cfg != nullptr &&
        cfg->character_preview_context != MDKR_CHARACTER_PREVIEW_NONE) {
        const char *context =
            characterPreviewContextName(cfg->character_preview_context);
        const char *pose =
            characterPreviewPoseSemantic(cfg->character_preview_pose);
        const char *lighting = characterPreviewLightingName(
            cfg->character_preview_lighting);
        const char *captureKind = characterPreviewCaptureKindName(
            cfg->character_preview_capture_kind);
        const bool captureRequested =
            cfg->character_preview_capture_png != nullptr &&
            cfg->character_preview_capture_png[0] != '\0';
        bool environmentReady = previewEnvironment.set(
            "MDKR_CHARACTER_WORKSHOP_PREVIEW", context) &&
            previewEnvironment.set(
                "MDKR_CHARACTER_WORKSHOP_PREVIEW_PLAYERS",
                std::to_string(cfg->character_preview_players).c_str()) &&
            previewEnvironment.set("MDKR_PRESENT_PERF", "1");
        const std::string posePhase = pose != nullptr
            ? std::to_string(cfg->character_preview_pose_phase_milli)
            : std::string();
        environmentReady = previewEnvironment.set(
            "MDKR_CHARACTER_WORKSHOP_PREVIEW_POSE",
            pose != nullptr ? pose : "") &&
            previewEnvironment.set(
                "MDKR_CHARACTER_WORKSHOP_PREVIEW_POSE_PHASE",
                posePhase.c_str()) && environmentReady;
        environmentReady = previewEnvironment.set(
            "MDKR_CHARACTER_WORKSHOP_VIEW_YAW_DEGREES",
            pose != nullptr
                ? std::to_string(
                      cfg->character_preview_view_yaw_degrees).c_str()
                : "") &&
            previewEnvironment.set(
                "MDKR_CHARACTER_WORKSHOP_VIEW_PITCH_DEGREES",
                pose != nullptr
                    ? std::to_string(
                          cfg->character_preview_view_pitch_degrees).c_str()
                    : "") &&
            previewEnvironment.set(
                "MDKR_CHARACTER_WORKSHOP_LIGHTING",
                pose != nullptr ? lighting : "") &&
            previewEnvironment.set(
                "MDKR_CHARACTER_WORKSHOP_CAPTURE_PNG",
                pose != nullptr &&
                        cfg->character_preview_capture_png != nullptr
                    ? cfg->character_preview_capture_png : "") &&
            previewEnvironment.set(
                "MDKR_CHARACTER_WORKSHOP_CAPTURE_KIND",
                pose != nullptr && captureRequested ? captureKind : "") &&
            environmentReady;
        for (int player = 0; player < 4; ++player) {
            const std::string variable =
                "MDKR_CUSTOM_CHARACTER_P" + std::to_string(player + 1);
            environmentReady = previewEnvironment.set(
                variable.c_str(), player < cfg->character_preview_players
                    ? cfg->character_preview_package : "") && environmentReady;
        }
        if (!environmentReady) {
            (void)previewEnvironment.restore();
            std::fprintf(stderr,
                         "[app] character preview environment failed\n");
            return 2;
        }
        std::fprintf(
            stderr,
            "[app] character preview: package=%s context=%s players=%d "
            "pose=%s phase=%u view=%d,%d lighting=%s captureKind=%s "
            "capture=%s\n",
            cfg->character_preview_package, context,
            cfg->character_preview_players,
            pose != nullptr ? pose : "live",
            cfg->character_preview_pose_phase_milli,
            cfg->character_preview_view_yaw_degrees,
            cfg->character_preview_view_pitch_degrees,
            lighting != nullptr ? lighting : "neutral",
            captureRequested ? captureKind : "none",
            cfg->character_preview_capture_png != nullptr &&
                    cfg->character_preview_capture_png[0] != '\0'
                ? cfg->character_preview_capture_png : "none");
        if (cfg->character_preview_result != nullptr) {
            *cfg->character_preview_result = MdkrCharacterPreviewResult{};
            cfg->character_preview_result->version =
                MDKR_CHARACTER_PREVIEW_RESULT_VERSION;
            cfg->character_preview_result->context =
                cfg->character_preview_context;
            cfg->character_preview_result->players =
                cfg->character_preview_players;
            cfg->character_preview_result->pose =
                cfg->character_preview_pose;
            cfg->character_preview_result->pose_phase_milli =
                cfg->character_preview_pose_phase_milli;
            cfg->character_preview_result->view_yaw_degrees =
                cfg->character_preview_view_yaw_degrees;
            cfg->character_preview_result->view_pitch_degrees =
                cfg->character_preview_view_pitch_degrees;
            cfg->character_preview_result->lighting =
                cfg->character_preview_lighting;
            cfg->character_preview_result->capture_requested =
                captureRequested;
            cfg->character_preview_result->capture_kind =
                cfg->character_preview_capture_kind;
            g_mdkrCharacterPreviewResult = cfg->character_preview_result;
        }
    }

    std::fprintf(stderr, "[app] boot:");
    for (size_t i = 1; i < owned.size(); ++i) std::fprintf(stderr, " %s", owned[i].c_str());
    std::fprintf(stderr, "\n");

    const int result =
        mdkr64_headless_main((int)owned.size(), argv.data());
    if (cfg != nullptr && cfg->character_preview_result != nullptr &&
        cfg->character_preview_capture_png != nullptr &&
        cfg->character_preview_capture_png[0] != '\0') {
        unsigned long long bytes = 0u;
        cfg->character_preview_result->capture_written =
            cfg->character_preview_result->capture_armed &&
            characterPreviewPng(
                cfg->character_preview_capture_png,
                cfg->character_preview_result->output_width,
                cfg->character_preview_result->output_height,
                cfg->character_preview_result->capture_kind ==
                        MDKR_CHARACTER_PREVIEW_CAPTURE_MODEL_ALPHA
                    ? 6u : 2u,
                bytes) ? 1 : 0;
        cfg->character_preview_result->capture_png_bytes =
            cfg->character_preview_result->capture_written ? bytes : 0u;
        std::fprintf(
            stderr,
            "[app] character preview capture: requested=1 kind=%s armed=%d stableFrames=%llu written=%d bytes=%llu output=%ux%u path=%s\n",
            characterPreviewCaptureKindName(
                cfg->character_preview_result->capture_kind),
            cfg->character_preview_result->capture_armed,
            cfg->character_preview_result->capture_stable_frames,
            cfg->character_preview_result->capture_written,
            cfg->character_preview_result->capture_png_bytes,
            cfg->character_preview_result->output_width,
            cfg->character_preview_result->output_height,
            cfg->character_preview_capture_png);
    }
    g_mdkrCharacterPreviewResult = nullptr;
    if (!previewEnvironment.restore()) {
        std::fprintf(stderr,
                     "[app] character preview environment restore failed\n");
        return result == 0 ? 2 : result;
    }
    return result;
}
