// engine_boot.cpp — synthesize a CLI invocation and hand it to the engine.
//
// The launcher never reimplements engine startup: it builds the exact argv a
// user would have typed and calls mdkr64_headless_main(). Anything the CLI can
// express, the launcher expresses the same way, so there is one boot path to
// reason about and no second one to drift.
#include "engine_entry.h"

#include "app_restart.h"    // AppRestart_getEnv, AppRestart_setEnv
#include "app_config.h"
#include "video_config.h"   // MdkrVideoMode, mdkr_video_schema

#include <cstdio>
#include <cstring>
#include <array>
#include <string>
#include <vector>

namespace {

std::array<std::string, 4> s_launcherCharacterEnvironment;
constexpr size_t kCharacterTuningCount = 10;
std::array<std::array<std::string, kCharacterTuningCount>, 4>
    s_launcherCharacterTuningEnvironment;

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
};

const char *modeFlag(int mode) {
    switch (mode) {
        case MDKR_VIDEO_MODE_PURE:       return "--pure";
        case MDKR_VIDEO_MODE_RESTORED:   return "--restored";
        case MDKR_VIDEO_MODE_REMASTERED: return "--remastered";
        default:                         return nullptr;  // Custom / unset
    }
}

}  // namespace

int mdkr64_engine_boot(const MdkrBootConfig *cfg) {
    // Own every string for the lifetime of the call: argv must stay valid for
    // the whole engine run, and the engine keeps pointers into it (romPath is
    // stored, not copied, by main_pc.c).
    std::vector<std::string> owned;
    owned.push_back("mdkr64");

    if (cfg != nullptr) {
        if (cfg->automation_ticks > 0 && cfg->automation_frames > 0) {
            std::fprintf(stderr,
                         "[app] boot rejected conflicting tick/frame limits\n");
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
    for (int player = 0; player < 4; ++player) {
        const std::string variable =
            "MDKR_CUSTOM_CHARACTER_P" + std::to_string(player + 1);
        const bool hasExisting =
            AppRestart_getEnv(variable.c_str(), existing) && !existing.empty();
        const std::string key =
            "custom_character_p" + std::to_string(player + 1);
        std::string activeSelection;
        if (!hasExisting || existing == s_launcherCharacterEnvironment[player]) {
            const std::string selected = AppConfig::get(key);
            AppRestart_setEnv(variable.c_str(), selected.c_str());
            s_launcherCharacterEnvironment[player] = selected;
            activeSelection = selected;
        } else {
            activeSelection = existing;
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
            const std::string profileKey = "custom_character_profile_" +
                activeSelection + "_" + mapping.preference_suffix;
            const std::string legacyKey = key + "_" + mapping.preference_suffix;
            const std::string value = activeSelection.empty()
                ? mapping.fallback
                : AppConfig::get(profileKey,
                                 AppConfig::get(legacyKey, mapping.fallback));
            AppRestart_setEnv(tuningVariable.c_str(), value.c_str());
            s_launcherCharacterTuningEnvironment[player][tuning] = value;
        }
    }

    std::fprintf(stderr, "[app] boot:");
    for (size_t i = 1; i < owned.size(); ++i) std::fprintf(stderr, " %s", owned[i].c_str());
    std::fprintf(stderr, "\n");

    return mdkr64_headless_main((int)owned.size(), argv.data());
}
