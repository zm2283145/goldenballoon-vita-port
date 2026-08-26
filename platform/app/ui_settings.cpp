// ui_settings.cpp — see ui_settings.h.
#include "ui_settings.h"
#include "app_config.h"
#include "app_theme.h"
#include "app_ui_policy.h"
#include "app_window.h"
#include "file_dialog.h"
#include "ui_common.h"

#include "controller_mapping.h"
#include "enhancement_registry.h"
#include "fs_utf8.h"
#include "mod_registry.h"
#include "modern_character_install.h"
#include "modern_character_donor.h"
#include "modern_character_registry.h"
#include "sha256.h"
#include "user_paths.h"
#include "video_config.h"
#include "platform_os.h"

#include "imgui.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace {

// --- Status line -----------------------------------------------------------
// The panel never claims success it did not observe: every edit routes through
// mdkr_video_config_runtime_set and its verdict is shown verbatim.
std::string g_status;
ImVec4      g_statusColor;

struct EditState {
    bool initialized = false;
    bool active = false;
    bool dirty = false;
    char text[MDKR_VIDEO_STRING_MAX] = {0};
    float number = 0.0f;
    std::string error;
};

std::array<EditState, MDKR_VIDEO_KEY_COUNT> g_edits;
bool g_uiScaleInitialized = false;
bool g_uiScaleDirty = false;
float g_uiScaleEdit = 1.0f;
std::string g_uiScaleError;
/*
 * Widget rectangles the scripted gates drive, and one flag each.
 *
 * The flag answers ONE question: is the panel showing this widget right now?
 * Not "was it ever submitted" -- a widget scrolled past the bottom of the
 * panel, or sitting in a collapsed section, is still submitted by ImGui, and a
 * flag that only records submission hands a queued click coordinates the panel
 * is currently clipping. The gate then fails on whatever it was really
 * measuring (a saved value, an applied scale) and says nothing about layout.
 * That mis-attribution cost hours once; see docs/open-items/misc.md.
 *
 * So each flag is assigned from ImGui::IsItemVisible() where the rect is read,
 * and every flag is cleared at the top of Settings_draw() for the frames the
 * widget is not submitted at all. Both halves are required: IsItemVisible()
 * cannot speak for a widget whose section never drew it.
 */
bool g_frameLimitRectValid = false;
ImVec2 g_frameLimitRectMin;
ImVec2 g_frameLimitRectMax;
bool g_frameLimitPopupOpen = false;
int g_frameLimitFocusedIndex = -1;
bool g_frameLimitRetryRectValid = false;
ImVec2 g_frameLimitRetryRectMin;
ImVec2 g_frameLimitRetryRectMax;
bool g_uiScaleRectValid = false;
ImVec2 g_uiScaleRectMin;
ImVec2 g_uiScaleRectMax;
bool g_smokeGamepadFocusUsed = false;
bool g_controllerSectionRequested = false;
// Rendered rectangle of each presentation-pace choice, for smoke observation
// only. Indexed by MdkrPresentationPace, so slot 0 (Custom) stays unused —
// Custom is a reading of the two keys and never a control to press.
bool g_paceRectValid[3] = {false, false, false};
ImVec2 g_paceRectMin[3];
ImVec2 g_paceRectMax[3];

void setStatus(const char *text, const ImVec4 &color) {
    g_status = text ? text : "";
    g_statusColor = color;
}

/*
 * One settings group: a heading a player can collapse, one line saying what
 * the group is for, and an indented body.
 *
 * The heading is drawn in the section font rather than the body font. Three
 * levels of heading — page, group, setting — were previously spelled with two
 * sizes, so a group name and a setting name looked identical and the page read
 * as one flat list of thirty controls.
 *
 * A caller that gets `true` back owes an ImGui::Unindent(ui::kGapM); the indent
 * is what gives the gameplay rule in SettingLabel somewhere to live.
 */
// Reset each frame by Settings_draw. The leading gap below separates one group
// from the PREVIOUS one, so the first group on the page has nothing to be
// separated from -- and spending it there simply stacked on the page title's
// own trailing gap, leaving a band of empty panel above the first header that
// read as a rendering fault rather than as breathing room.
int g_sectionsDrawn = 0;

bool drawSettingsSectionHeader(const char *label, const char *subtitle,
                               ImGuiTreeNodeFlags flags, bool compact) {
    // Groups need air between them, and the gap belongs to the header rather
    // than to each body's exit path: a body that forgets it leaves one seam on
    // the page tighter than every other, which is exactly how a settings list
    // starts looking like a debug dump.
    if (g_sectionsDrawn++ > 0) ui::Gap(ui::kGapM);
    // These rows are independent expandable sections, not mutually exclusive
    // tabs. Keep resting and hover surfaces neutral; the chevron and a gold
    // leading rule communicate the open state without making every open
    // section look like another selected destination.
    ImGui::PushStyleColor(ImGuiCol_Header, AppTheme::groupHeader());
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, AppTheme::groupHeaderHover());
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, AppTheme::groupHeaderActive());
    // The scripted accessibility walk cannot reach a row inside a collapsed
    // section, because a collapsed section does not draw one.
    if (AppUi_a11yWalkArmed()) flags |= ImGuiTreeNodeFlags_DefaultOpen;
    ImGui::PushFont(AppTheme::fonts().section);
    const bool open = ImGui::CollapsingHeader(label, flags);
    ImGui::PopFont();
    // Landing on a header IS moving between sections; this is the one place
    // the settings panel says so. Only one header can hold focus, and
    // ui::SpeakSection drops the repeat on every subsequent frame.
    if (ImGui::IsItemFocused()) ui::SpeakSection(label);
    ImGui::PopStyleColor(3);
    if (!open) return false;
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    ImGui::GetWindowDrawList()->AddRectFilled(
        min, ImVec2(min.x + 3.0f * AppTheme::uiScale(), max.y),
        ImGui::GetColorU32(AppTheme::accent()),
        2.0f * AppTheme::uiScale());
    ImGui::Indent(ui::kGapM);
    if (!compact && subtitle != nullptr && subtitle[0] != '\0') {
        ui::Gap(ui::kGapXS);
        ImGui::PushFont(AppTheme::fonts().small);
        ui::TextSubtleUnformattedWrapped(subtitle);
        ImGui::PopFont();
    }
    ui::Gap(ui::kGapS);
    return true;
}

void reportResult(MdkrVideoRuntimeResult r, const MdkrVideoSchema *s) {
    char buf[320];
    switch (r) {
        case MDKR_VIDEO_RUNTIME_LIVE:
            /* LIVE is the setter's verdict about persistence and precedence,
             * not about WHEN the engine picks the value up — a LEVEL-scoped key
             * returns it too. Saying "applied" for one of those would claim a
             * change the player can see is not on screen yet. */
            if (s->scope == MDKR_VIDEO_SCOPE_LEVEL) {
                std::snprintf(buf, sizeof(buf),
                              "%s is set — it starts at the next race.",
                              s->label);
                setStatus(buf, AppTheme::accent());
            } else {
                std::snprintf(buf, sizeof(buf), "%s applied.", s->label);
                setStatus(buf, AppTheme::good());
            }
            break;
        case MDKR_VIDEO_RUNTIME_RESTART:
            std::snprintf(buf, sizeof(buf),
                          "%s saved — it starts the next time you press Play.",
                          s->label);
            setStatus(buf, AppTheme::accent());
            break;
        case MDKR_VIDEO_RUNTIME_LOCKED:
            std::snprintf(buf, sizeof(buf),
                          "%s is set by %s or a command-line option, so it "
                          "cannot be changed here.", s->label, s->env);
            setStatus(buf, AppTheme::subtle());
            break;
        case MDKR_VIDEO_RUNTIME_SAVE_FAILED:
            std::snprintf(buf, sizeof(buf),
                          "%s was not saved — the settings file is not "
                          "writable. Nothing changed.", s->label);
            setStatus(buf, AppTheme::bad());
            if (std::getenv("MDKR_APP_SMOKE_EXPECT_SAVE_FAILURE")) {
                std::fprintf(stderr,
                             "[app-ui-test] visible settings error: %s\n", buf);
            }
            break;
        case MDKR_VIDEO_RUNTIME_SAVE_UNCONFIRMED:
            std::snprintf(buf, sizeof(buf),
                          "%s applied, but the system could not confirm it "
                          "reached the disk. Set it again after an unexpected "
                          "shutdown.",
                          s->label);
            setStatus(buf, AppTheme::accent());
            break;
        case MDKR_VIDEO_RUNTIME_PENDING:
            std::snprintf(buf, sizeof(buf), "%s applies on the next frame.",
                          s->label);
            setStatus(buf, AppTheme::accent());
            break;
        case MDKR_VIDEO_RUNTIME_SUPERSEDED:
            std::snprintf(buf, sizeof(buf),
                          "%s applies on the next frame, replacing the choice "
                          "that was still waiting.",
                          s->label);
            setStatus(buf, AppTheme::accent());
            break;
        case MDKR_VIDEO_RUNTIME_UNAVAILABLE:
            std::snprintf(buf, sizeof(buf),
                          "%s needs a game window. Try again once the game is "
                          "showing.", s->label);
            setStatus(buf, AppTheme::bad());
            break;
        case MDKR_VIDEO_RUNTIME_APPLY_FAILED:
            std::snprintf(buf, sizeof(buf),
                          "The system refused %s. The previous setting was "
                          "kept.", s->label);
            setStatus(buf, AppTheme::bad());
            break;
        case MDKR_VIDEO_RUNTIME_ROLLBACK_FAILED:
            std::snprintf(
                buf, sizeof(buf),
                "%s could not be saved, and the window did not go back to "
                "how it was. Your saved preference is unchanged — use F11 or "
                "Alt+Enter, or restart the app.",
                s->label);
            setStatus(buf, AppTheme::bad());
            break;
        case MDKR_VIDEO_RUNTIME_INVALID:
        default:
            std::snprintf(buf, sizeof(buf), "That is not a valid %s.", s->label);
            setStatus(buf, AppTheme::bad());
            break;
    }
}

bool resultSucceeded(MdkrVideoRuntimeResult result) {
    return mdkr_video_runtime_result_applied(result) != 0;
}

bool commitEdit(SDL_Window *window, MdkrVideoKey key,
                const MdkrVideoSchema *schema,
                EditState &edit, const char *value) {
    const MdkrVideoRuntimeResult result = key == MDKR_WINDOW_MODE
        ? AppWindow_requestMode(window, value)
        : mdkr_video_config_runtime_set(key, value);
    reportResult(result, schema);
    /* Both spellings of "queued for the next frame" leave the
     * widget dirty and error-free; the completion resynchronizes it later. */
    if (result == MDKR_VIDEO_RUNTIME_PENDING ||
        result == MDKR_VIDEO_RUNTIME_SUPERSEDED) {
        edit.error.clear();
        return false;
    }
    if (resultSucceeded(result)) {
        edit.dirty = false;
        edit.initialized = false;  // resync from the authoritative desired value
        edit.error.clear();
        if (key == MDKR_INPUT_RUMBLE_ENABLED ||
            key == MDKR_INPUT_RUMBLE_PROFILE) {
            platform_pad_rumble_preferences_changed();
        }
        return true;
    }
    edit.error = g_status;
    if (mdkr_video_key_is_audio(key)) {
        mdkr_audio_config_runtime_cancel_preview();
    }
    return false;
}

// --- Value access ----------------------------------------------------------
// `desired` is what the player has chosen (LIVE + staged RESTART); `current` is
// what the running engine actually has. The difference between them is exactly
// what "needs a restart" means, so the panel shows both rather than pretending.
const MdkrVideoValue *desired(MdkrVideoKey k) {
    const MdkrVideoConfig *c = mdkr_video_config_desired();
    return c ? &c->values[k] : nullptr;
}

const MdkrVideoValue *live(MdkrVideoKey k) {
    const MdkrVideoConfig *c = mdkr_video_config_current();
    return c ? &c->values[k] : nullptr;
}

bool differsFromLive(MdkrVideoKey k, const MdkrVideoSchema *s) {
    const MdkrVideoValue *d = desired(k), *l = live(k);
    if (!d || !l) return false;
    if (s->type == MDKR_VIDEO_TYPE_STRING) return std::strcmp(d->text, l->text) != 0;
    return d->number != l->number;
}

void formatValue(MdkrVideoKey key, const MdkrVideoSchema *s,
                 const MdkrVideoValue *v, char *out, size_t cap) {
    if (!v) { std::snprintf(out, cap, "?"); return; }
    switch (s->type) {
        case MDKR_VIDEO_TYPE_STRING: std::snprintf(out, cap, "%s", v->text); break;
        case MDKR_VIDEO_TYPE_INT:
            if (mdkr_video_key_is_audio(key)) {
                std::snprintf(out, cap, "%d%%", (int)v->number);
            } else {
                std::snprintf(out, cap, "%d", (int)v->number);
            }
            break;
        default:                     std::snprintf(out, cap, "%.2f", (double)v->number); break;
    }
}

// Where a value came from, in the player's words. This is the honest answer to
// "why is this greyed out?".
const char *sourceName(MdkrVideoSource src) {
    switch (src) {
        case MDKR_VIDEO_SOURCE_DEFAULT:  return "default";
        case MDKR_VIDEO_SOURCE_FILE:     return "settings file";
        case MDKR_VIDEO_SOURCE_PRESET:   return "presentation preset";
        case MDKR_VIDEO_SOURCE_LAUNCHER: return "launcher";
        case MDKR_VIDEO_SOURCE_RUNTIME:  return "you";
        case MDKR_VIDEO_SOURCE_ENV:      return "environment variable";
        case MDKR_VIDEO_SOURCE_CLI:      return "command line";
        default:                         return "unknown";
    }
}

// --- Enumerated string options --------------------------------------------
// The schema constrains these values in mdkr_video_config_set(); the tables
// here mirror exactly what those validators accept, so the UI can only ever
// offer a value the config layer will take. Implemented settings with an
// open-ended domain (Aspect and GameplayFOV) get a text field instead.
struct Option { const char *value; const char *label; };
struct Options { const Option *items; int count; };

constexpr const char *kOriginalFrameLimitLabel =
    "Original (recommended)";
constexpr const char *kModernFrameLimitGroup =
    "Higher refresh rates";
/*
 * The one long tooltip on the page, and it earns its length: Frame limit is the
 * only setting whose right answer depends on hardware the app cannot see.
 *
 * It used to be twelve sentences rendered inline under the control, which is
 * where "reads like AI slop" came from. It is now five, behind a marker, and it
 * still carries every fact a player has actually needed: that gameplay speed is
 * unaffected, what smoothing changes about the higher rates, who Just Under
 * Display and 40 Hz are for, and what a browser does with the two it cannot
 * honour.
 *
 * Three files quote this string byte-for-byte -- CMakeLists.txt's app_schema
 * PASS_REGULAR_EXPRESSION, macos/Scripts/verify_unsigned_release.sh, and
 * tests/ci_contract_manifest.py. tests/check_ci_contract.py regenerates the
 * first two from this definition and fails when they drift, so editing this
 * text means editing those with it. Keep it free of parentheses and regex
 * metacharacters: check_ci_contract.py rejects the first outright, and the
 * others would silently widen the CTest pin.
 */
constexpr const char *kFrameLimitHelp =
    "How often the app draws to your screen. It never changes how fast the "
    "game runs. Original draws each picture the game makes, once; higher rates "
    "repeat it, or draw new in-between pictures when Motion smoothing is "
    "Interpolated. Just Under Display suits a variable-refresh display, and "
    "40 Hz suits a handheld screen that runs at 40 or 120 Hz. Rates above your "
    "display's refresh only apply where the system can drop a picture it has "
    "not shown yet, and a browser maps Uncapped and Just Under Display to "
    "Match Display.";

const Option kCadence[] = {
    {"original", "Original — 30 Hz, as authored"},
    {"enhanced", "Enhanced — 60 Hz, breaks some boss races"},
};
const Option kFrameLimit[] = {
    {"original", kOriginalFrameLimitLabel},
    {"display",  "Match Display"},
    // Named for what it does rather than for the hardware feature it suits:
    // a player who has a variable-refresh display knows they have one, and a
    // player who does not is not helped by the acronym.
    {"display-margin", "Just Under Display"},
    {"30",       "30 Hz"},
    {"40",       "40 Hz (battery friendly)"},
    {"60",       "60 Hz"},
    {"90",       "90 Hz"},
    {"120",      "120 Hz"},
    {"144",      "144 Hz"},
    {"165",      "165 Hz"},
    {"240",      "240 Hz"},
    {"uncapped", "Uncapped (native)"},
};
const Option kMotionSmoothing[] = {
    {"interpolate", "Interpolated"},
    {"off",         "Off — the game's own pictures only"},
};
const Option kAllowTearing[] = {
    {"off", "Off"},
    {"on",  "On — lowest delay, visible seam"},
};
/*
 * "Original" rather than "Pure". Pure is what the config file, the CLI flag and
 * every gate call this value and none of that changes; it was never a word a
 * player could act on. The three modes are Original, Restored and Remastered
 * everywhere a player reads about them -- the release notes, the site, the
 * device acceptance sheets -- and the settings panel was the last place still
 * spelling one of them differently.
 */
const Option kMode[] = {
    {"pure",       "Original — pixel-exact N64 picture"},
    {"restored",   "Restored — recommended"},
    {"remastered", "Remastered — in progress"},
};
// The canonical spellings only. mdkr_video_world_shadows_canonical() also takes
// "0"/"1"/"on"/"" so the MDKR_WORLD_SHADOW diagnostic seam keeps working, but a
// combo that offered two words for the same state would be a worse control.
const Option kShadows[] = {
    {"full", "Full"}, {"soft", "Soft"}, {"off", "Off"},
};
// The two player-facing spellings only. mdkr_video_camera_obstruction_canonical()
// also takes "legacy" and "center-ray" so the MDKR_CAMERA_OBSTRUCTION diagnostic
// seam keeps working, but those are A/B arms, not states to offer a player.
// Default first, as everywhere else in this table. The corrected camera was
// the default for one wave; device acceptance sent it back to opt-in, so
// Authored leads again and neither label recommends the other.
const Option kCameraObstruction[] = {
    {"observe", "Authored — the original camera"},
    {"modern", "Keep it out of walls"},
};
const Option kCameraComfort[] = {
    {"authored", "Authored"},
    {"reduced", "Reduced"},
};
const Option kMenuLanguages[] = {
    {"all", "All on the cartridge"},
    {"authentic", "Only your region's"},
};
const Option kWindowMode[] = {
    {"windowed", "Windowed"},
    {"fullscreen", "Fullscreen"},
};
const Option kRumbleProfile[] = {
    {"light", "Light — 35%"},
    {"balanced", "Balanced — 65%"},
    {"strong", "Strong — 100%"},
};
const Option kControllerAction[] = {
    {"none", "None"},
    {"a", "N64 A"},
    {"b", "N64 B"},
    {"z", "N64 Z trigger"},
    {"start", "N64 Start"},
    {"l", "N64 L"},
    {"r", "N64 R"},
    {"dpad_up", "N64 D-pad up"},
    {"dpad_down", "N64 D-pad down"},
    {"dpad_left", "N64 D-pad left"},
    {"dpad_right", "N64 D-pad right"},
    {"c_up", "N64 C-up"},
    {"c_down", "N64 C-down"},
    {"c_left", "N64 C-left"},
    {"c_right", "N64 C-right"},
};

bool optionsFor(MdkrVideoKey k, Options &out) {
    if (k >= MDKR_INPUT_CONTROLLER_A &&
        k <= MDKR_INPUT_CONTROLLER_RIGHT_STICK_RIGHT) {
        out = {kControllerAction,
               static_cast<int>(sizeof(kControllerAction) /
                                sizeof(kControllerAction[0]))};
        return true;
    }
    switch (k) {
        case MDKR_VIDEO_SIMULATION_CADENCE: out = {kCadence, 2}; return true;
        case MDKR_VIDEO_FRAME_LIMIT:
            out = {kFrameLimit,
                   static_cast<int>(std::size(kFrameLimit))};
            return true;
        case MDKR_VIDEO_MOTION_SMOOTHING:
            out = {kMotionSmoothing, 2}; return true;
        case MDKR_VIDEO_ALLOW_TEARING:      out = {kAllowTearing, 2}; return true;
        case MDKR_VIDEO_MODE:               out = {kMode, 3}; return true;
        case MDKR_VIDEO_WORLD_SHADOWS:      out = {kShadows, 3}; return true;
        case MDKR_VIDEO_CAMERA_OBSTRUCTION:
            out = {kCameraObstruction, 2}; return true;
        case MDKR_VIDEO_CAMERA_COMFORT:
            out = {kCameraComfort, 2}; return true;
        case MDKR_VIDEO_MENU_LANGUAGES:
            out = {kMenuLanguages, 2}; return true;
        case MDKR_WINDOW_MODE:              out = {kWindowMode, 2}; return true;
        case MDKR_INPUT_RUMBLE_PROFILE:     out = {kRumbleProfile, 3}; return true;
        default: return false;
    }
}

const char *optionLabel(MdkrVideoKey key, const char *value) {
    if (key == MDKR_VIDEO_MODE && std::strcmp(value, "custom") == 0) {
        return "Custom (Individual Settings)";
    }
    Options options;
    if (!optionsFor(key, options)) return value;
    for (int i = 0; i < options.count; ++i) {
        if (std::strcmp(options.items[i].value, value) == 0) {
            return options.items[i].label;
        }
    }
    return value;
}

// --- Player-facing copy ----------------------------------------------------
//
// WHY THIS TABLE EXISTS RATHER THAN THE SCHEMA'S OWN label/help. The schema
// strings are written for the person editing the ini and reading --video-list:
// they name values in their config spelling ("observe", "interpolate", "1
// engages the display policy") and explain WHY a key has the scope it has.
// Both are the right thing there and the wrong thing in front of a player, and
// trying to serve both audiences from one string is how they ended up long.
//
// The rules this table follows, and the ones the old copy broke:
//   * a label a player could say out loud;
//   * one line of description, present tense, no hedging, no "seamlessly";
//   * a tooltip only where a fact genuinely will not fit in that line;
//   * every setting that changes how the game PLAYS carries `gameplay`, and
//     nothing else does -- a marker on a row that only changes the picture is
//     what taught players to ignore the marker.
//
// A key with no entry falls back to the schema, so the table can never hide a
// setting by omission.
struct Copy {
    const char *label;
    const char *description;
    const char *tooltip;
    bool gameplay;
    bool experimental;
};

const Copy *copyFor(MdkrVideoKey key) {
    static const Copy kMode = {
        "Presentation",
        "Original is the N64 picture, pixel for pixel. Restored is that same "
        "art in widescreen at modern resolution. Remastered adds new lighting "
        "and shadows.",
        nullptr, false, false};
    static const Copy kCadenceCopy = {
        "Gameplay tick rate",
        "DKR's physics and AI were written for 30 updates a second. Enhanced "
        "runs them at 60.",
        "Enhanced is experimental. It gives you 60 FPS gameplay rather than "
        "60 FPS presentation, but parts of the authored physics are written "
        "around the 30 Hz tick, so boss races still run measurably off pace. "
        "Original is the accurate setting. Frame limit and Motion smoothing "
        "raise the frame rate without touching any of this.",
        true, true};
    static const Copy kFrameLimitCopy = {
        "Frame limit", "How often the app draws to your screen.",
        kFrameLimitHelp, false, false};
    static const Copy kSmoothingCopy = {
        "Motion smoothing",
        "Draws extra pictures between the game's own so motion reads as more "
        "continuous.",
        "The in-between pictures are invented, so fast-moving edges can show "
        "artefacts in them. Nothing else changes: physics, input, timers, "
        "audio and saves still advance only on the game's own ticks.",
        false, true};
    static const Copy kTearingCopy = {
        "Allow tearing",
        "Shows a finished frame without waiting for the display. Lowest input "
        "delay, and a visible seam while things move.",
        "Leave this off on a variable-refresh display. That display already "
        "adapts to the game, and this gives that up for a seam you do not "
        "need. Use Frame limit = Just Under Display instead.",
        false, false};
    static const Copy kCameraCopy = {
        "Camera",
        "The original camera passes through walls. The alternative pulls it in "
        "front of them.",
        "Only the view moves. Handling, results, ghosts and saves are "
        "identical either way. Authored is the default and is what the game "
        "shipped with.",
        false, true};
    static const Copy kComfortCopy = {
        "Camera shake",
        "Reduced smooths the vertical shake from bumps, landings and "
        "explosions.",
        "It also eases the camera back out more gently after it squeezes past "
        "a wall, so it has nothing to soften if you are using the authored "
        "camera.",
        false, false};
    static const Copy kLanguagesCopy = {
        "Menu languages",
        "Every cartridge carries every translation, whatever region it was "
        "sold in. Authentic shows only the ones your region's menu listed.",
        nullptr, false, false};
    static const Copy kFovCopy = {
        "Field of view",
        "authored keeps each track's original lens. Or type a number from 20 "
        "to 140.",
        nullptr, false, false};
    static const Copy kAspectCopy = {
        "Aspect ratio",
        "auto fills the window. 4:3 pillarboxes the original framing.",
        nullptr, false, false};
    static const Copy kWidescreenCopy = {
        "Widescreen",
        "Off is the old stretch-to-fill path, which distorts the picture. Turn "
        "it on.",
        nullptr, false, false};
    static const Copy kRenderScaleCopy = {
        "Render scale",
        "Renders above your window size and shrinks the result. This is the "
        "anti-aliasing control.",
        nullptr, false, false};
    static const Copy kMsaaCopy = {
        "MSAA", "Redundant with render scale. Off by default.",
        nullptr, false, false};
    static const Copy kAnisoCopy = {
        "Anisotropic filtering",
        "1 keeps the N64's own 3-point filter. Higher sharpens surfaces seen "
        "at a shallow angle.",
        nullptr, false, false};
    static const Copy kMipmapsCopy = {
        "Mipmaps", "Removes shimmer on distant track surfaces.",
        nullptr, false, false};
    static const Copy kHiresTextCopy = {
        "Sharper lettering",
        "Redraws the game's plain text from outline fonts. Same layout, "
        "sharper letters. Ignored in Original.",
        nullptr, false, false};
    static const Copy kShadowsCopy = {
        "World shadows",
        "Full is the shipped Remastered look. Soft is lighter. Off restores "
        "the original blob shadows under karts.",
        "DKR's artwork already paints its own shading in, so the shadow pass "
        "always darkens twice to some degree. This is the control for how "
        "much. It does nothing outside Remastered.",
        false, false};
    static const Copy kRemasterFxCopy = {
        "Remaster effects",
        "The master switch for Remastered's look-changing effects.",
        nullptr, false, false};
    static const Copy kWindowCopy = {
        "Window", "F11 or Alt+Enter does the same thing.",
        nullptr, false, false};
    static const Copy kRumbleCopy = {
        "Rumble",
        "Your controller's motors. The in-game Rumble Pak stays connected "
        "either way.",
        nullptr, false, false};
    static const Copy kRumbleProfileCopy = {
        "Rumble strength", nullptr, nullptr, false, false};
    static const Copy kMasterCopy = {"Master", nullptr, nullptr, false, false};
    static const Copy kMusicCopy = {"Music", nullptr, nullptr, false, false};
    static const Copy kEffectsCopy = {
        "Sound effects", nullptr, nullptr, false, false};

    switch (key) {
        case MDKR_VIDEO_MODE:               return &kMode;
        case MDKR_VIDEO_SIMULATION_CADENCE: return &kCadenceCopy;
        case MDKR_VIDEO_FRAME_LIMIT:        return &kFrameLimitCopy;
        case MDKR_VIDEO_MOTION_SMOOTHING:   return &kSmoothingCopy;
        case MDKR_VIDEO_ALLOW_TEARING:      return &kTearingCopy;
        case MDKR_VIDEO_CAMERA_OBSTRUCTION: return &kCameraCopy;
        case MDKR_VIDEO_CAMERA_COMFORT:     return &kComfortCopy;
        case MDKR_VIDEO_MENU_LANGUAGES:     return &kLanguagesCopy;
        case MDKR_VIDEO_GAMEPLAY_FOV:       return &kFovCopy;
        case MDKR_VIDEO_ASPECT:             return &kAspectCopy;
        case MDKR_VIDEO_WIDESCREEN:         return &kWidescreenCopy;
        case MDKR_VIDEO_RENDER_SCALE:       return &kRenderScaleCopy;
        case MDKR_VIDEO_MSAA:               return &kMsaaCopy;
        case MDKR_VIDEO_ANISOTROPY:         return &kAnisoCopy;
        case MDKR_VIDEO_MIPMAPS:            return &kMipmapsCopy;
        case MDKR_VIDEO_HIRES_TEXT:         return &kHiresTextCopy;
        case MDKR_VIDEO_WORLD_SHADOWS:      return &kShadowsCopy;
        case MDKR_VIDEO_REMASTER_FX:        return &kRemasterFxCopy;
        case MDKR_WINDOW_MODE:              return &kWindowCopy;
        case MDKR_INPUT_RUMBLE_ENABLED:     return &kRumbleCopy;
        case MDKR_INPUT_RUMBLE_PROFILE:     return &kRumbleProfileCopy;
        case MDKR_AUDIO_MASTER_VOLUME:      return &kMasterCopy;
        case MDKR_AUDIO_MUSIC_VOLUME:       return &kMusicCopy;
        case MDKR_AUDIO_EFFECTS_VOLUME:     return &kEffectsCopy;
        default:                            return nullptr;
    }
}

/*
 * The value in the player's own words, for anything that has to SAY it rather
 * than draw it. A voice reading "Frame limit, original" or "Rumble, 1" is
 * reading the config file aloud; the control on screen says "Original
 * (recommended)" and shows a tick, and the two must not disagree.
 *
 * Settings_dumpSchemaContract() and the row announcement both call this, so
 * the gate's expectation and the utterance are produced by one function.
 */
void displayValue(MdkrVideoKey key, const MdkrVideoSchema *s,
                  const MdkrVideoValue *v, char *out, size_t cap) {
    char raw[MDKR_VIDEO_STRING_MAX];
    formatValue(key, s, v, raw, sizeof(raw));
    Options options;
    if (optionsFor(key, options) || key == MDKR_VIDEO_MODE) {
        std::snprintf(out, cap, "%s", optionLabel(key, raw));
        return;
    }
    // A checkbox row. "on"/"off" is what the player sees and what a voice can
    // act on; "1" is neither.
    if (s->type == MDKR_VIDEO_TYPE_INT && s->min == 0.0f && s->max == 1.0f) {
        std::snprintf(out, cap, "%s",
                      v != nullptr && v->number != 0.0f ? "on" : "off");
        return;
    }
    std::snprintf(out, cap, "%s", raw);
}

// --- Enhancement help ------------------------------------------------------
// Every enhancement row ends with its AUTHORITY CLASS in the player's words.
// The class is asserted and gated in platform/enhancement_registry.c, so that
// is where this reads it from — never from the enum name, which says nothing to
// a player, and never from a second list kept here.
constexpr const char *kEnhancementGameplayNote = "Changes how the game plays.";
constexpr const char *kEnhancementLooksNote =
    "Changes only how the game looks.";

// Composed once per key; drawn every frame.
std::array<std::string, MDKR_VIDEO_KEY_COUNT> g_enhancementHelp;

/*
 * WHEN a setting takes effect, said on every row that has room to say it.
 *
 * The page had three timings and only spelled two of them. A RESTART key gets
 * the gold "Next launch" chip, a LEVEL key gets the grey "Next race" chip, and
 * a LIVE key got NOTHING -- so "this applies immediately" was communicated by
 * the absence of a marker, which is not communication. Eighteen of the visible
 * rows were in that silent majority.
 *
 * The two scoped cases keep their chip and are left alone here: a chip on the
 * label line plus a sentence under it would be the same fact twice. Only LIVE
 * gains a sentence, and it is generated from the schema scope rather than
 * written per row, so a key added tomorrow cannot be the one that forgets.
 */
std::array<std::string, MDKR_VIDEO_KEY_COUNT> g_rowDescription;

const char *describeRow(MdkrVideoKey key, const MdkrVideoSchema *schema,
                        const char *base) {
    if (schema->scope == MDKR_VIDEO_SCOPE_RESTART ||
        schema->scope == MDKR_VIDEO_SCOPE_LEVEL) {
        return base;   // the chip on the label line already said it
    }
    std::string &text = g_rowDescription[static_cast<size_t>(key)];
    if (!text.empty()) return text.c_str();
    text = base != nullptr ? base : "";
    if (!text.empty()) text += ' ';
    text += "Applies straight away.";
    return text.c_str();
}

const char *enhancementHelp(MdkrVideoKey key,
                            const MdkrEnhancement *enhancement,
                            const MdkrVideoSchema *schema) {
    std::string &text = g_enhancementHelp[static_cast<size_t>(key)];
    if (!text.empty()) return text.c_str();
    const char *note = enhancement->authority == MDKR_ENH_GAMEPLAY
        ? kEnhancementGameplayNote : kEnhancementLooksNote;
    // The schema paragraph, because it is the one that explains what the
    // values mean; the registry's own sentence is the shorter summary the
    // enhancement table carries.
    text = schema->help != nullptr ? schema->help : enhancement->help;
    // Appended only when the paragraph does not already end with it. Most were
    // written with the sentence in place, and printing it twice reads as a
    // stutter rather than as emphasis — while a new row whose author forgot it
    // still gets the class stated, which is the part that must not be optional.
    const size_t noteLength = std::strlen(note);
    if (text.size() < noteLength ||
        text.compare(text.size() - noteLength, noteLength, note) != 0) {
        if (!text.empty()) text += ' ';
        text += note;
    }
    return text.c_str();
}

const char *helpFor(MdkrVideoKey key, const MdkrVideoSchema *schema) {
    const MdkrEnhancement *enhancement = mdkr_enhancement_for_key(key);
    if (enhancement != nullptr) {
        return enhancementHelp(key, enhancement, schema);
    }
    switch (key) {
        case MDKR_VIDEO_SIMULATION_CADENCE:
            return "Original preserves retail physics, AI, timers, and input "
                   "timing. Enhanced runs the game's logic at 60 Hz and "
                   "changes gameplay speed. It is not an FPS setting.";
        case MDKR_VIDEO_FRAME_LIMIT:
            return kFrameLimitHelp;
        case MDKR_VIDEO_MOTION_SMOOTHING:
            return "Interpolated blends the game's own adjacent pictures at "
                   "the display's exact fractional time. Simulation, input, "
                   "audio, timers, and saves still advance only on Original "
                   "gameplay ticks. Off shows the game's own pictures only.";
        case MDKR_VIDEO_MODE:
            // The section introduction directly above this control explains the
            // three modes; repeating the schema paragraph creates a text wall.
            return nullptr;
        default:
            return schema->help;
    }
}

// --- One row ---------------------------------------------------------------
bool drawKey(SDL_Window *window, MdkrVideoKey k, bool compact) {
    const MdkrVideoSchema *s = mdkr_video_schema(k);
    const MdkrVideoValue  *d = desired(k);
    if (!s || !d) return false;

    const bool locked = mdkr_video_config_runtime_locked(k) != 0;
    const MdkrVideoValue *rumbleEnabled =
        desired(MDKR_INPUT_RUMBLE_ENABLED);
    const bool rumbleProfileUnavailable =
        k == MDKR_INPUT_RUMBLE_PROFILE && rumbleEnabled != nullptr &&
        rumbleEnabled->number == 0.0f;
    bool changed = false;
    // While an open combo is being browsed, "the last item" is the option under
    // the cursor rather than this row. Announcing the row's CURRENT value then
    // would tell the player the opposite of what they are about to choose.
    bool comboBrowsing = false;
    EditState &editState = g_edits[static_cast<size_t>(k)];

    ImGui::PushID((int)k);
    if (locked || rumbleProfileUnavailable) ImGui::BeginDisabled();

    const Copy *copy = copyFor(k);
    const bool controllerBinding =
        k >= MDKR_INPUT_CONTROLLER_A &&
        k <= MDKR_INPUT_CONTROLLER_RIGHT_STICK_RIGHT;
    ui::RowStyle rowStyle;
    rowStyle.gameplay = copy != nullptr && copy->gameplay;
    rowStyle.experimental = copy != nullptr && copy->experimental;
    rowStyle.restart = s->scope == MDKR_VIDEO_SCOPE_RESTART;
    // LEVEL is not a weaker RESTART, and saying "restart" for it would be a
    // lie a player can disprove. It has its own boundary and its own word.
    if (s->scope == MDKR_VIDEO_SCOPE_LEVEL) rowStyle.badge = "Next race";
    if (copy != nullptr && !compact) rowStyle.tooltip = copy->tooltip;
    // A binding row is one of eighteen in a grid; a description under each one
    // would be a wall, and the label already says which control it is.
    //
    // A key with no Copy entry -- every row the sprint work added -- falls back
    // to helpFor(), which carries the enhancement authority-class note and the
    // update-check "not active yet" paragraph. A bare label here would silently
    // break the Enhancements header's promise that every row states whether it
    // changes gameplay.
    const char *rowLabel = copy != nullptr ? copy->label : s->label;
    const char *description =
        copy != nullptr ? copy->description : helpFor(k, s);
    const bool describable = !compact && !controllerBinding;
    ui::SettingLabel(rowLabel,
                     describable ? describeRow(k, s, description) : nullptr,
                     rowStyle);

    ImGui::SetNextItemWidth(ui::kControlWidth());

    char valueBuf[MDKR_VIDEO_STRING_MAX];
    formatValue(k, s, d, valueBuf, sizeof(valueBuf));

    Options opts;
    if (optionsFor(k, opts)) {
        if (!editState.initialized ||
            (!editState.active && !editState.dirty &&
             std::strcmp(editState.text, d->text) != 0)) {
            std::snprintf(editState.text, sizeof(editState.text), "%s", d->text);
            editState.initialized = true;
            editState.error.clear();
        }
        if (k == MDKR_VIDEO_FRAME_LIMIT && !g_smokeGamepadFocusUsed &&
            AppUi_smokeInputMode() == AppUiSmokeInputMode::Gamepad) {
            // Deterministic initial nav anchor only. Opening, movement, and
            // activation below still arrive exclusively from SDL's virtual
            // controller through the production ImGui platform backend.
            ImGui::SetKeyboardFocusHere();
            g_smokeGamepadFocusUsed = true;
        }
        int cur = -1;
        for (int i = 0; i < opts.count; ++i) {
            if (std::strcmp(opts.items[i].value, editState.text) == 0) {
                cur = i;
                break;
            }
        }
        const char *preview = cur >= 0 ? opts.items[cur].label : editState.text;
        const bool comboOpen = ImGui::BeginCombo("##v", preview);
        editState.active = comboOpen;
        if (k == MDKR_VIDEO_FRAME_LIMIT) {
            g_frameLimitPopupOpen = comboOpen;
            /* Sample only while the popup is closed. BeginCombo() returning
             * true means it has ALREADY begun the popup window, and Begin()
             * reassigns ImGui's last-item data to that window's title bar --
             * so on open frames GetItemRectMin/Max describe the popup (a
             * zero-height rect wherever it was placed) and IsItemVisible()
             * answers for the popup too. Reading them there overwrote the
             * combo's rect with popup geometry and made the off-screen
             * diagnostic below fire on a perfectly visible combo. */
            if (!comboOpen) {
                g_frameLimitRectMin = ImGui::GetItemRectMin();
                g_frameLimitRectMax = ImGui::GetItemRectMax();
                /* The one source of truth for "the panel is showing this
                 * widget right now": ImGui clears the visible bit for an item
                 * whose rectangle misses the current clip rect, whether it was
                 * scrolled past the bottom or squeezed off any other edge. The
                 * scripted pacing gates click these coordinates on the next
                 * frame, and Settings_smokeFrameLimitCenter() refuses to hand
                 * them over unless this says the widget is on screen. */
                g_frameLimitRectValid = ImGui::IsItemVisible();
            }
            /* Same fact, said out loud, because a gate that simply cannot find
             * its target reports "was not rendered" and leaves the reader to
             * guess which of the two it is. Read from the flag above so the
             * diagnostic and the refusal can never disagree. Armed only for
             * the gates: a player scrolling past this row is the same
             * observation and means nothing. */
            static const bool pacingGateArmed =
                std::getenv("MDKR_APP_SMOKE_SELECT_FRAME_LIMIT") != nullptr ||
                std::getenv("MDKR_APP_SMOKE_SELECT_PRESENTATION_PACE") != nullptr;
            static bool offscreenReported = false;
            if (pacingGateArmed && !offscreenReported && !comboOpen &&
                !g_frameLimitRectValid) {
                offscreenReported = true;
                std::fprintf(stderr,
                             "[app-ui-test] frame-limit combo is scrolled out "
                             "of the panel (rect y=%.0f..%.0f, panel height "
                             "%.0f): something drawn above it -- an open "
                             "section, or a larger UI scale -- pushed it past "
                             "the bottom\n",
                             g_frameLimitRectMin.y, g_frameLimitRectMax.y,
                             ImGui::GetIO().DisplaySize.y);
            }
            if (std::getenv("MDKR_APP_UI_INPUT_TRACE")) {
                static int smokeFrame = 0;
                const ImVec2 mouse = ImGui::GetIO().MousePos;
                std::fprintf(stderr,
                             "[app-ui-test] combo frame=%d open=%d hovered=%d "
                             "active=%d focused=%d rect=%.0f,%.0f..%.0f,%.0f "
                             "mouse=%.0f,%.0f\n",
                             smokeFrame++, comboOpen ? 1 : 0,
                             ImGui::IsItemHovered() ? 1 : 0,
                             ImGui::IsItemActive() ? 1 : 0,
                             ImGui::IsItemFocused() ? 1 : 0,
                             g_frameLimitRectMin.x, g_frameLimitRectMin.y,
                             g_frameLimitRectMax.x, g_frameLimitRectMax.y,
                             mouse.x, mouse.y);
            }
        }
        if (comboOpen) {
            for (int i = 0; i < opts.count; ++i) {
                if (k == MDKR_VIDEO_FRAME_LIMIT && i == 1) {
                    ImGui::SeparatorText(kModernFrameLimitGroup);
                }
                const bool selected = i == cur;
                if (ImGui::Selectable(
                        opts.items[i].label, selected, 0,
                        ImVec2(0.0f, ui::kTouchRowHeight()))) {
                    if (k == MDKR_VIDEO_FRAME_LIMIT &&
                        std::getenv("MDKR_APP_UI_INPUT_TRACE") != nullptr) {
                        std::fprintf(stderr,
                                     "[app-ui-test] frame-limit activated "
                                     "index=%d value=%s\n",
                                     i, opts.items[i].value);
                    }
                    std::snprintf(editState.text, sizeof(editState.text), "%s",
                                  opts.items[i].value);
                    editState.dirty = true;
                    changed = commitEdit(
                        window, k, s, editState, editState.text);
                }
                if (k == MDKR_VIDEO_FRAME_LIMIT && ImGui::IsItemFocused()) {
                    g_frameLimitFocusedIndex = i;
                    if (std::getenv("MDKR_APP_UI_INPUT_TRACE") != nullptr) {
                        std::fprintf(stderr,
                                     "[app-ui-test] frame-limit focused "
                                     "index=%d value=%s\n",
                                     i, opts.items[i].value);
                    }
                }
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
            comboBrowsing = true;
        }
    } else if (s->type == MDKR_VIDEO_TYPE_STRING) {
        // Open domain (aspect expressions, or "authored" / a FOV number).
        // Commit on Enter/blur so a half-typed value is never sent
        // to the validator and reported as invalid mid-keystroke.
        if (!editState.initialized ||
            (!editState.active && !editState.dirty &&
             std::strcmp(editState.text, d->text) != 0)) {
            std::snprintf(editState.text, sizeof(editState.text), "%s", d->text);
            editState.initialized = true;
            editState.error.clear();
        }
        const bool entered = ImGui::InputText(
            "##v", editState.text, sizeof(editState.text),
            ImGuiInputTextFlags_EnterReturnsTrue);
        if (ImGui::IsItemEdited()) editState.dirty = true;
        const bool commit = entered || ImGui::IsItemDeactivatedAfterEdit();
        editState.active = ImGui::IsItemActive();
        if (commit && editState.dirty) {
            changed = commitEdit(window, k, s, editState, editState.text);
        }
    } else if (s->type == MDKR_VIDEO_TYPE_INT && s->min == 0.0f && s->max == 1.0f) {
        if (!editState.initialized || (!editState.active && !editState.dirty)) {
            editState.number = d->number;
            editState.initialized = true;
        }
        bool on = editState.number != 0.0f;
        if (ImGui::Checkbox("##v", &on)) {
            editState.number = on ? 1.0f : 0.0f;
            editState.dirty = true;
            changed = commitEdit(window, k, s, editState, on ? "1" : "0");
        }
    } else if (s->type == MDKR_VIDEO_TYPE_INT) {
        if (!editState.initialized || (!editState.active && !editState.dirty)) {
            editState.number = d->number;
            editState.initialized = true;
        }
        int v = static_cast<int>(editState.number);
        const bool previewChanged = ImGui::SliderInt(
            "##v", &v, (int)s->min, (int)s->max,
            mdkr_video_key_is_audio(k) ? "%d%%" : "%d");
        if (previewChanged) {
            editState.number = static_cast<float>(v);
            if (mdkr_video_key_is_audio(k)) {
                (void)mdkr_audio_config_runtime_preview(k, v);
            }
        }
        const bool commit = AppUi_deferredCommit(
            previewChanged, ImGui::IsItemDeactivatedAfterEdit(), &editState.dirty);
        editState.active = ImGui::IsItemActive();
        if (commit && editState.dirty) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%d", v);
            changed = commitEdit(window, k, s, editState, buf);
        }
    } else {
        if (!editState.initialized || (!editState.active && !editState.dirty)) {
            editState.number = d->number;
            editState.initialized = true;
        }
        const bool previewChanged = ImGui::SliderFloat(
            "##v", &editState.number, s->min, s->max, "%.2f");
        const bool commit = AppUi_deferredCommit(
            previewChanged, ImGui::IsItemDeactivatedAfterEdit(), &editState.dirty);
        editState.active = ImGui::IsItemActive();
        if (commit && editState.dirty) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%.2f",
                          static_cast<double>(editState.number));
            changed = commitEdit(window, k, s, editState, buf);
        }
    }

    /*
     * THE announcement. Every settings row in the product passes through here
     * -- the launcher panel, the in-game overlay, the Enhancements and Content
     * sections all call drawKey() -- so a row added later speaks without
     * anybody remembering to make it, and a row can only be silent by being
     * hand-rolled somewhere else. It sits immediately after the widget on
     * purpose: ui::SpeakFocusedItem reads ImGui's last-submitted item, and the
     * error text, badges and help paragraph below would take that place.
     */
    if (!comboBrowsing) {
        char spoken[MDKR_VIDEO_STRING_MAX];
        displayValue(k, s, d, spoken, sizeof(spoken));
        // The drawn label and the drawn help, not the schema's: the voice
        // and the screen must not disagree (see displayValue's contract).
        const char *spokenHelp = copy != nullptr
            ? (copy->tooltip != nullptr ? copy->tooltip : copy->description)
            : helpFor(k, s);
        ui::SpeakFocusedItem(rowLabel, spoken, spokenHelp);
    }

    if (!editState.error.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, AppTheme::bad());
        ImGui::TextWrapped("%s", editState.error.c_str());
        ImGui::PopStyleColor();
        const bool retryPressed = editState.dirty && ImGui::Button(
            "Try again", ImVec2(0.0f, ui::kBtnSecondary().y));
        if (editState.dirty && k == MDKR_VIDEO_FRAME_LIMIT) {
            g_frameLimitRetryRectMin = ImGui::GetItemRectMin();
            g_frameLimitRetryRectMax = ImGui::GetItemRectMax();
            // Submitted is not the same as on screen; the same-process Retry
            // gate clicks these coordinates. See the flag block up top.
            g_frameLimitRetryRectValid = ImGui::IsItemVisible();
        }
        if (retryPressed) {
            if (k == MDKR_VIDEO_FRAME_LIMIT) {
                std::fprintf(stderr,
                             "[app-ui-test] Retry save widget activated\n");
            }
            char retry[MDKR_VIDEO_STRING_MAX];
            if (s->type == MDKR_VIDEO_TYPE_STRING) {
                std::snprintf(retry, sizeof(retry), "%s", editState.text);
            } else if (s->type == MDKR_VIDEO_TYPE_INT) {
                std::snprintf(retry, sizeof(retry), "%d",
                              static_cast<int>(editState.number));
            } else {
                std::snprintf(retry, sizeof(retry), "%.2f",
                              static_cast<double>(editState.number));
            }
            changed = commitEdit(window, k, s, editState, retry);
        }
    }

    if (locked || rumbleProfileUnavailable) {
        ImGui::EndDisabled();
        if (rumbleProfileUnavailable && !locked) {
            ui::TextSubtle("Turn rumble on to choose a strength.");
        } else if (mdkr_video_config_is_readonly()) {
            ui::TextSubtle("Locked for this Original session.");
        } else if (d->source == MDKR_VIDEO_SOURCE_CLI) {
            ui::TextSubtle("Set on the command line for this session.");
        } else if (d->source == MDKR_VIDEO_SOURCE_ENV) {
            ui::TextSubtle("Set by %s for this session.", s->env);
        } else {
            ui::TextSubtle("Set by the %s for this session.",
                           sourceName(d->source));
        }
    } else if (s->scope == MDKR_VIDEO_SCOPE_RESTART && differsFromLive(k, s)) {
        // The honest RESTART presentation: say what is running NOW and what will
        // be running next launch. Never imply the change already took effect.
        char liveBuf[MDKR_VIDEO_STRING_MAX];
        formatValue(k, s, live(k), liveBuf, sizeof(liveBuf));
        ImGui::PushStyleColor(ImGuiCol_Text, AppTheme::accent());
        ImGui::Text("Next Play: %s. Running now: %s.",
                    optionLabel(k, valueBuf), optionLabel(k, liveBuf));
        ImGui::PopStyleColor();
    } else if (s->scope == MDKR_VIDEO_SCOPE_LEVEL && differsFromLive(k, s)) {
        // Same honesty, one boundary earlier. The desired/live split is exactly
        // as real here as it is for a RESTART key — video_config_runtime.c
        // deliberately does not copy a staged LEVEL value into the live config
        // until the level applier runs — so the panel can name both without
        // guessing, and "the next time a track loads" is a promise the engine
        // keeps rather than a hope.
        char liveBuf[MDKR_VIDEO_STRING_MAX];
        formatValue(k, s, live(k), liveBuf, sizeof(liveBuf));
        ImGui::PushStyleColor(ImGuiCol_Text, AppTheme::accent());
        ImGui::Text("Next race: %s. Running now: %s.",
                    optionLabel(k, valueBuf), optionLabel(k, liveBuf));
        ImGui::PopStyleColor();
    }

    /* The description and any tooltip were drawn ABOVE the control, with the
     * label. A paragraph underneath is read after the choice has already been
     * made, which is exactly backwards, and it was the single largest source of
     * text on the page. */

    ui::Gap(ui::kGapS);
    ImGui::PopID();
    return changed;
}

// --- Frame rate: ONE control over Frame limit + Motion smoothing + tearing --
//
// WHY THIS IS ONE CONTROL NOW. The page used to offer the same setting twice:
// a "Presentation pace" quick choice with two radios, and, a few rows below it
// behind a "Set the rate yourself" disclosure, the two combos that quick choice
// writes. Both were always on screen, both always live, and a player who
// changed one watched the other move on its own with nothing saying why.
//
// So the presets and the individual rows became one control. Off Custom the
// individual rows are NOT DRAWN -- the page offers one way to set the frame
// rate, not two -- and Custom is a real, pressable third choice that reveals
// them. A line under the choices names the two values the selected preset
// writes, so a preset is never a black box.
//
// WHY THIS IS STILL SUGAR AND NOT A SETTING. The two keys below stay the source
// of truth: this writes them and reads them back and holds no persisted state
// of its own. A player who edits the config file by hand behaves identically,
// and every gate that drives Video.FrameLimit keeps testing the same thing. A
// third persisted key that the other two had to be kept in sync with has a
// wrong answer available at every layer, and this does not.
//
// WHY Custom IS STICKY. It is revealed state, not a value: pressing it writes
// nothing, because there is nothing honest to write. Without the latch, a
// player who entered Custom and then tuned their way into a pair that happens
// to spell "Smooth" would have the controls they were using vanish from under
// their hands. The latch clears only when they press Original or Smooth.
struct PaceChoice { MdkrPresentationPace pace; const char *label; };
const PaceChoice kPaceChoices[] = {
    {MDKR_PRESENTATION_PACE_ORIGINAL, "Original"},
    {MDKR_PRESENTATION_PACE_SMOOTH,   "Smooth"},
};

bool g_frameRateCustomRevealed = false;

// What the selected choice actually does, named in the player's words rather
// than left for them to discover by watching two other controls move.
const char *paceSummary(MdkrPresentationPace pace) {
    switch (pace) {
        case MDKR_PRESENTATION_PACE_ORIGINAL:
            return "Original draws each picture the game makes, once, and "
                   "nothing in between. Frame limit: Original. Motion "
                   "smoothing: Off.";
        case MDKR_PRESENTATION_PACE_SMOOTH:
            return "Smooth follows your display and fills in the pictures "
                   "between the game's own. Frame limit: Match Display. "
                   "Motion smoothing: Interpolated.";
        default:
            return "Set Frame limit and Motion smoothing yourself below.";
    }
}

bool drawFrameRate(SDL_Window *window, bool compact, bool selectingFrameLimit) {
    const MdkrVideoConfig *config = mdkr_video_config_desired();
    const MdkrVideoSchema *frameSchema =
        mdkr_video_schema(MDKR_VIDEO_FRAME_LIMIT);
    if (config == nullptr || frameSchema == nullptr) return false;

    // Locked if EITHER underlying key is pinned above RUNTIME rank: the choice
    // writes both, so it can only be offered when both can be written.
    const bool locked =
        mdkr_video_config_runtime_locked(MDKR_VIDEO_FRAME_LIMIT) != 0 ||
        mdkr_video_config_runtime_locked(MDKR_VIDEO_MOTION_SMOOTHING) != 0;
    const MdkrPresentationPace current = mdkr_video_presentation_pace(config);
    // A pair no preset names, or a scripted gate about to click the individual
    // combo, both mean the same thing: the individual rows have to be reachable.
    if (current == MDKR_PRESENTATION_PACE_CUSTOM || selectingFrameLimit) {
        g_frameRateCustomRevealed = true;
    }
    /* Same scaffolding, and the same reason, as drawSettingsSectionHeader
     * force-opening every collapsed group for the walk: the scripted
     * accessibility walk cannot reach a row that is not drawn, and off Custom
     * these three deliberately are not. check_a11y_shell caught precisely that
     * and named the three rows, which is the gate working -- so the fix is to
     * make them REACHABLE for the walk, never to stop requiring them to speak.
     * This changes what is on screen for the walk, not what any row says. */
    if (AppUi_a11yWalkArmed()) g_frameRateCustomRevealed = true;
    bool changed = false;

    ImGui::PushID("presentation-pace");
    if (locked) ImGui::BeginDisabled();
    ui::RowStyle paceStyle;
    paceStyle.tooltip = compact ? nullptr :
        "The in-between pictures Smooth draws are invented, so fast-moving "
        "edges can show artefacts in them. Race speed, timers, music and saves "
        "are identical either way.";
    ui::SettingLabel(
        "Frame rate",
        compact ? nullptr
                : "How often the app draws to your screen. It never changes "
                  "how fast the game runs. Applies straight away.",
        paceStyle);

    for (int i = 0; i < static_cast<int>(std::size(kPaceChoices)); ++i) {
        const PaceChoice &choice = kPaceChoices[i];
        if (i > 0) ImGui::SameLine(0.0f, ui::kGapM);
        const bool selected =
            !g_frameRateCustomRevealed && current == choice.pace;
        const bool pressed = ImGui::RadioButton(choice.label, selected);
        const int slot = static_cast<int>(choice.pace);
        g_paceRectMin[slot] = ImGui::GetItemRectMin();
        g_paceRectMax[slot] = ImGui::GetItemRectMax();
        // The quick-choice gate presses one of these radios at last frame's
        // coordinates, so the flag has to mean "on screen", not "submitted".
        g_paceRectValid[slot] = ImGui::IsItemVisible();
        if (!pressed) continue;
        const MdkrVideoRuntimeResult result =
            mdkr_video_config_runtime_set_presentation_pace(choice.pace);
        // One verdict for the pair, reported against Frame limit's label: the
        // transaction succeeded or failed as a whole, and two status lines for
        // one press would be two claims about one thing.
        reportResult(result, frameSchema);
        if (resultSucceeded(result)) {
            // Both underlying combos must resynchronize from the authoritative
            // desired config rather than from their own stale edit buffers.
            g_edits[static_cast<size_t>(MDKR_VIDEO_FRAME_LIMIT)] = EditState{};
            g_edits[static_cast<size_t>(MDKR_VIDEO_MOTION_SMOOTHING)] =
                EditState{};
            g_frameRateCustomRevealed = false;
            changed = true;
        }
    }
    ImGui::SameLine(0.0f, ui::kGapM);
    if (ImGui::RadioButton("Custom", g_frameRateCustomRevealed)) {
        // Reveal only. There is no honest pair to write for "Custom", and
        // writing one would silently move the player off the values they came
        // here to keep.
        g_frameRateCustomRevealed = true;
    }

    if (!compact) {
        ImGui::PushFont(AppTheme::fonts().small);
        ui::TextSubtleUnformattedWrapped(paceSummary(
            g_frameRateCustomRevealed ? MDKR_PRESENTATION_PACE_CUSTOM
                                      : current));
        ImGui::PopFont();
    }
    if (locked) {
        ImGui::EndDisabled();
        ui::TextSubtle("Fixed for this session.");
    }
    // One row per DISTINCT reading, not per frame: the panel redraws sixty
    // times a second and a per-frame row would bury the transition a gate is
    // looking for in thousands of identical ones.
    if (std::getenv("MDKR_APP_UI_TRACE") != nullptr) {
        const MdkrVideoConfig *now = mdkr_video_config_desired();
        static std::string traced;
        char row[256];
        std::snprintf(row, sizeof(row),
                      "[app-ui] presentation-pace value=%.31s frameLimit=%.31s "
                      "motionSmoothing=%.31s locked=%d",
                      mdkr_video_presentation_pace_name(
                          mdkr_video_presentation_pace(now)),
                      now->values[MDKR_VIDEO_FRAME_LIMIT].text,
                      now->values[MDKR_VIDEO_MOTION_SMOOTHING].text,
                      locked ? 1 : 0);
        if (traced != row) {
            traced = row;
            std::fprintf(stderr, "%s\n", row);
        }
    }
    ui::Gap(ui::kGapS);
    ImGui::PopID();

    // The individual rows, drawn ONLY on Custom. This is what makes the merge
    // real rather than cosmetic: off Custom they are not on the page at all, so
    // there is exactly one control for the frame rate.
    if (g_frameRateCustomRevealed) {
        ImGui::Indent(ui::kGapM);
        changed |= drawKey(window, MDKR_VIDEO_FRAME_LIMIT, compact);
        changed |= drawKey(window, MDKR_VIDEO_MOTION_SMOOTHING, compact);
        changed |= drawKey(window, MDKR_VIDEO_ALLOW_TEARING, compact);
        ImGui::Unindent(ui::kGapM);
    }
    return changed;
}

bool restoreControllerDefaults() {
    constexpr size_t kInputCount =
        static_cast<size_t>(MDKR_INPUT_LAST_KEY - MDKR_INPUT_FIRST_KEY + 1);
    MdkrVideoConfig defaults;
    std::array<std::string, kInputCount> values;
    std::array<MdkrVideoRuntimeChange, kInputCount> changes;

    mdkr_video_config_defaults(&defaults);
    for (size_t i = 0; i < kInputCount; ++i) {
        const MdkrVideoKey key = static_cast<MdkrVideoKey>(
            static_cast<int>(MDKR_INPUT_FIRST_KEY) + static_cast<int>(i));
        const MdkrVideoSchema *schema = mdkr_video_schema(key);
        if (schema == nullptr) return false;
        if (schema->type == MDKR_VIDEO_TYPE_STRING) {
            values[i] = defaults.values[key].text;
        } else {
            values[i] = std::to_string(
                static_cast<int>(defaults.values[key].number));
        }
        changes[i] = {key, values[i].c_str()};
    }

    const MdkrVideoRuntimeResult result = mdkr_video_config_runtime_set_many(
        changes.data(), static_cast<int>(changes.size()));
    if (!resultSucceeded(result)) {
        reportResult(result, mdkr_video_schema(MDKR_INPUT_RUMBLE_ENABLED));
        return false;
    }
    for (int key = MDKR_INPUT_FIRST_KEY; key <= MDKR_INPUT_LAST_KEY; ++key) {
        EditState &edit = g_edits[static_cast<size_t>(key)];
        edit.initialized = false;
        edit.active = false;
        edit.dirty = false;
        edit.error.clear();
    }
    platform_pad_rumble_preferences_changed();
    setStatus("Controller mapping and rumble reset to defaults.",
              AppTheme::good());
    return true;
}

// --- Enhancements ----------------------------------------------------------
// The rows are enumerated from platform/enhancement_registry.c rather than
// listed here, for the reason that table exists: a panel carrying its own copy
// of the list keeps looking complete after somebody adds a row and forgets it.
// Grouping is the registry's category, and a category with no rows draws no
// header — COSMETIC is declared and still empty.
struct EnhancementGroup {
    MdkrEnhCategory category;
    const char *label;
};
const EnhancementGroup kEnhancementGroups[] = {
    {MDKR_ENH_CAT_DISPLAY,    "On screen"},
    {MDKR_ENH_CAT_DIFFICULTY, "Difficulty"},
    {MDKR_ENH_CAT_COSMETIC,   "Appearance"},
};

// Restores the enhancement keys, and only those, to their schema defaults.
//
// The scoping is the action. "Reset" next to a list of extras must not be a
// trap that also throws away the frame limit, the volume levels, and a
// remapped controller, so the key set comes from AppUi_enhancementResetIncludes
// and one transaction writes exactly it — the same shape as the controller
// restore above, so a partial write is not a state either can land in.
bool resetEnhancements() {
    MdkrVideoConfig defaults;
    std::array<std::string, MDKR_VIDEO_KEY_COUNT> values;
    std::array<MdkrVideoRuntimeChange, MDKR_VIDEO_KEY_COUNT> changes;
    int count = 0;

    mdkr_video_config_defaults(&defaults);
    for (int i = 0; i < MDKR_VIDEO_KEY_COUNT; ++i) {
        const MdkrVideoKey key = static_cast<MdkrVideoKey>(i);
        if (!AppUi_enhancementResetIncludes(key)) continue;
        const MdkrVideoSchema *schema = mdkr_video_schema(key);
        if (schema == nullptr) return false;
        if (schema->type == MDKR_VIDEO_TYPE_STRING) {
            values[static_cast<size_t>(count)] = defaults.values[key].text;
        } else if (schema->type == MDKR_VIDEO_TYPE_INT) {
            values[static_cast<size_t>(count)] = std::to_string(
                static_cast<int>(defaults.values[key].number));
        } else {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%.2f",
                          static_cast<double>(defaults.values[key].number));
            values[static_cast<size_t>(count)] = buf;
        }
        // `values` is a fixed array, so no element ever moves and the pointer
        // handed to the transaction stays valid until it returns.
        changes[static_cast<size_t>(count)] =
            {key, values[static_cast<size_t>(count)].c_str()};
        ++count;
    }
    if (count == 0) return false;

    const MdkrVideoRuntimeResult result =
        mdkr_video_config_runtime_set_many(changes.data(), count);
    if (!resultSucceeded(result)) {
        // One pinned row refuses the whole transaction, so name the row that
        // refused it rather than whichever one happened to be written first:
        // "Speedometer is fixed by ..." is not an answer for a player whose
        // Opponent skill is the pinned one.
        MdkrVideoKey blamed = changes[0].key;
        for (int i = 0; i < count; ++i) {
            const MdkrVideoKey key = changes[static_cast<size_t>(i)].key;
            if (mdkr_video_config_runtime_locked(key) != 0) {
                blamed = key;
                break;
            }
        }
        reportResult(result, mdkr_video_schema(blamed));
        return false;
    }
    for (int i = 0; i < count; ++i) {
        g_edits[static_cast<size_t>(changes[static_cast<size_t>(i)].key)] =
            EditState{};
    }
    // Scope is stated under the button, where it can be read BEFORE the press.
    // Repeating it here made one sentence appear twice on one page.
    setStatus("Extras are back to the way the game shipped.", AppTheme::good());
    return true;
}

int enhancementsInGroup(MdkrEnhCategory category, bool webGpuRenderer,
                        bool legacyStretchActive) {
    int rows = 0;
    for (int i = 0; i < mdkr_enhancement_count(); ++i) {
        const MdkrEnhancement *enhancement = mdkr_enhancement_at(i);
        if (enhancement == nullptr || enhancement->category != category) {
            continue;
        }
        if (!AppUi_videoSettingVisible(enhancement->key, webGpuRenderer,
                                       legacyStretchActive)) {
            continue;
        }
        ++rows;
    }
    return rows;
}

bool drawEnhancementsSection(SDL_Window *window, bool compact,
                             bool webGpuRenderer, bool legacyStretchActive) {
    bool changed = false;
    ui::Gap(ui::kGapS);
    if (!compact) {
        ui::TextSubtleWrapped(
            "Extras that stay off until you switch them on. Each one says "
            "whether it changes how the game plays or only how it looks.");
    }
    ui::Gap(ui::kGapS);
    ImGui::Indent(ui::kGapM);
    for (const EnhancementGroup &group : kEnhancementGroups) {
        if (enhancementsInGroup(group.category, webGpuRenderer,
                                legacyStretchActive) == 0) {
            continue;
        }
        ImGui::SeparatorText(group.label);
        ui::Gap(ui::kGapXS);
        for (int i = 0; i < mdkr_enhancement_count(); ++i) {
            const MdkrEnhancement *enhancement = mdkr_enhancement_at(i);
            if (enhancement == nullptr ||
                enhancement->category != group.category) {
                continue;
            }
            if (!AppUi_videoSettingVisible(enhancement->key, webGpuRenderer,
                                           legacyStretchActive)) {
                continue;
            }
            changed |= drawKey(window, enhancement->key, compact);
        }
    }
    ui::Gap(ui::kGapS);
    if (ImGui::Button("Reset extras", ui::kBtnWide())) {
        changed |= resetEnhancements();
    }
    if (!compact) {
        ui::TextSubtleWrapped(
            "Puts every extra above back to the way the game shipped. Your "
            "picture, sound, controller and content-pack settings are not "
            "touched.");
    }
    ImGui::Unindent(ui::kGapM);
    ui::Gap(ui::kGapS);
    return changed;
}

// --- UI scale --------------------------------------------------------------
//
// The one settings control with no schema key: it is a launcher preference, so
// nothing in MdkrVideoKey can route it. Which section draws it is therefore a
// policy question (AppUi_shellPreferenceSection), asked at both candidate
// sections so that exactly one of them ever answers yes. Hand-placing the
// widget instead is how a control ends up drawn twice, with two independent
// copies of the commit path underneath it.
bool drawUiScale(bool compact) {
    bool changed = false;
    if (!g_uiScaleInitialized) {
        g_uiScaleEdit = AppTheme::uiScale();
        g_uiScaleInitialized = true;
    }
    ui::RowStyle scaleStyle;
    ui::SettingLabel(
        "Interface scale",
        compact ? nullptr
                : "Text and controls together, 0.75x to 2x. Use 1.25x or "
                  "more for touch.",
        scaleStyle);
    ImGui::SetNextItemWidth(ui::kControlWidth());
    const bool scalePreviewChanged = ImGui::SliderFloat(
        "##ui-scale", &g_uiScaleEdit, 0.75f, 2.0f, "%.2fx",
        ImGuiSliderFlags_AlwaysClamp);
    g_uiScaleRectMin = ImGui::GetItemRectMin();
    g_uiScaleRectMax = ImGui::GetItemRectMax();
    /* Same rule as the frame-limit combo: the drag gate grabs this slider at
     * last frame's coordinates, so the flag has to mean "the panel is showing
     * it", not "it was submitted". A slider is its own last item -- there is no
     * popup to displace it -- so IsItemVisible() reads directly here. */
    g_uiScaleRectValid = ImGui::IsItemVisible();
    /* Say which failure this is, from the flag above rather than a second read,
     * so the diagnostic cannot contradict the refusal. */
    {
        static const bool dragGateArmed =
            std::getenv("MDKR_APP_SMOKE_UI_SCALE_DRAG") != nullptr;
        static bool offscreenReported = false;
        if (dragGateArmed && !offscreenReported && !g_uiScaleRectValid) {
            offscreenReported = true;
            std::fprintf(stderr,
                         "[app-ui-test] UI-scale slider is scrolled out of the "
                         "panel (rect y=%.0f..%.0f, panel height %.0f): "
                         "something drawn above it -- an open section, or the "
                         "scale this very slider just applied -- pushed it "
                         "past the bottom\n",
                         g_uiScaleRectMin.y, g_uiScaleRectMax.y,
                         ImGui::GetIO().DisplaySize.y);
        }
        if (dragGateArmed) {
            /* Deterministic starting viewport, and nothing more. The scripted
             * drag presses a real SDL pointer at a fixed coordinate, so the
             * slider has to be on screen before the first press. Two frames:
             * the first scrolls, the second confirms the position is already
             * correct, so the rectangle the gate captures from frame 1 onward
             * never moves. */
            static int scrolledFrames = 0;
            if (scrolledFrames < 2) {
                ImGui::SetScrollHereY(0.5f);
                ++scrolledFrames;
            }
        }
    }
    // Spoken here for the same reason drawKey() speaks its rows: this widget
    // is hand-rolled, so the shared helper cannot reach it, and a silent text
    // size control is the one a player who cannot read the panel needs most.
    {
        char value[32];
        std::snprintf(value, sizeof(value), "%.2fx",
                      static_cast<double>(g_uiScaleEdit));
        ui::SpeakFocusedItem(
            "Interface scale", value,
            "Scales text and controls together after you release the slider.");
    }
    if (AppUi_deferredCommit(scalePreviewChanged,
                             ImGui::IsItemDeactivatedAfterEdit(),
                             &g_uiScaleDirty)) {
        // Applying while held changes every widget's geometry underneath
        // the pointer. That feedback loop made the slider oscillate and
        // the whole launcher flash. Commit once, on release, and let the
        // host apply the new metrics at the next safe frame boundary.
        AppTheme::requestUiScale(g_uiScaleEdit);
        char value[32];
        std::snprintf(value, sizeof(value), "%.2f",
                      static_cast<double>(g_uiScaleEdit));
        const AppConfig::PersistResult persist =
            AppConfig::setAndSave("ui_scale", value);
        if (AppConfig::persistResultApplied(persist)) {
            g_uiScaleDirty = false;
            g_uiScaleError.clear();
            setStatus(
                persist == AppConfig::PersistResult::DurabilityUnconfirmed
                    ? "UI scale applied, but the system could not confirm it "
                      "reached the disk. Set it again after an unexpected "
                      "shutdown."
                    : "Interface scale saved.",
                persist == AppConfig::PersistResult::DurabilityUnconfirmed
                    ? AppTheme::accent() : AppTheme::good());
            changed = true;
        } else {
            g_uiScaleError =
                "Interface scale could not be saved. It stays active for this "
                "session; try again once the settings file is writable.";
            setStatus(g_uiScaleError.c_str(), AppTheme::bad());
        }
    }
    if (!g_uiScaleError.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, AppTheme::bad());
        ImGui::TextWrapped("%s", g_uiScaleError.c_str());
        ImGui::PopStyleColor();
        if (ImGui::Button(
                "Try again",
                ImVec2(0.0f, ui::kBtnSecondary().y))) {
            char value[32];
            std::snprintf(value, sizeof(value), "%.2f",
                          static_cast<double>(g_uiScaleEdit));
            const AppConfig::PersistResult persist =
                AppConfig::setAndSave("ui_scale", value);
            if (AppConfig::persistResultApplied(persist)) {
                g_uiScaleDirty = false;
                g_uiScaleError.clear();
                setStatus(
                    persist == AppConfig::PersistResult::DurabilityUnconfirmed
                        ? "UI scale applied, but the system could not confirm it "
                          "reached the disk. Set it again after an unexpected "
                          "shutdown."
                        : "Interface scale saved.",
                    persist == AppConfig::PersistResult::DurabilityUnconfirmed
                        ? AppTheme::accent() : AppTheme::good());
                changed = true;
            }
        }
    }
    ui::Gap(ui::kGapS);
    return changed;
}

// --- Accessibility ---------------------------------------------------------
//
// One place for every option a player might need before they can use the rest
// of the panel. The rows are ENUMERATED from AppUi_settingsSection rather than
// listed here, so an accessibility option added later appears the moment it is
// routed -- and so this section and the sections the keys came from cannot
// disagree about which of them draws a row.
bool drawAccessibilitySection(SDL_Window *window, bool compact,
                              bool webGpuRenderer, bool legacyStretchActive) {
    bool changed = false;
    /* No introduction here either: the group header says it, and said it
     * better. */
    ui::Gap(ui::kGapS);
    ImGui::Indent(ui::kGapM);
    if (AppUi_shellPreferenceSection(AppUiShellPreference::UiScale) ==
        AppUiSettingsSection::Accessibility) {
        changed |= drawUiScale(compact);
    }
    for (int i = 0; i < MDKR_VIDEO_KEY_COUNT; ++i) {
        const MdkrVideoKey key = static_cast<MdkrVideoKey>(i);
        if (AppUi_settingsSection(key) !=
            AppUiSettingsSection::Accessibility) continue;
        if (!AppUi_videoSettingVisible(key, webGpuRenderer,
                                       legacyStretchActive)) continue;
        changed |= drawKey(window, key, compact);
    }
    ImGui::Unindent(ui::kGapM);
    ui::Gap(ui::kGapS);
    return changed;
}

// --- Content packs ---------------------------------------------------------
// The read-only list below exists for one failure: a pack that is quietly
// ignored looks to the player exactly like a pack that loaded and did nothing.
// Every directory the scan saw is accounted for here — installed, or skipped
// with a reason — which is the same accounting the startup log prints and the
// single commonest question a pack author is asked.

// Only `name` is mandatory in a pack.ini, so the optional parts are appended
// rather than formatted in: a pack that declared neither must not read as
// "Name  by  (priority 100)".
std::string packSummary(const MdkrModEntry *entry) {
    std::string text = entry->manifest.name;
    if (entry->manifest.version[0] != '\0') {
        text += "  ";
        text += entry->manifest.version;
    }
    if (entry->manifest.author[0] != '\0') {
        text += "  by ";
        text += entry->manifest.author;
    }
    char priority[48];
    std::snprintf(priority, sizeof(priority), "  (priority %d)",
                  entry->manifest.priority);
    text += priority;
    return text;
}

// Why a disabled entry is disabled. The registry records the state but not the
// cause, so the cause is re-derived from the list the scan itself read, through
// the scan's own matcher rather than a second one that could disagree with it.
const char *packSkipReason(const MdkrModEntry *entry,
                           const char *disabledList) {
    return platform_content_pack_name_disabled(disabledList,
                                               entry->manifest.name)
        ? "you listed it under Skipped packs"
        : "its own pack.ini switches it off";
}

// One row per pack, once per process, so a gate can read what the panel drew
// without depending on whether the section happens to be expanded.
void traceContentPacks(const MdkrModRegistry *packs, const char *disabledList) {
    if (std::getenv("MDKR_APP_UI_TRACE") == nullptr) return;
    static bool traced = false;
    if (traced) return;
    traced = true;
    const int count = mdkr_mod_registry_count(packs);
    const int skipped = mdkr_mod_registry_skipped(packs);
    std::fprintf(stderr, "[app-ui] content-list found=%d unreadable=%d\n",
                 count, skipped);
    for (int i = 0; i < count; ++i) {
        const MdkrModEntry *entry = mdkr_mod_registry_entry(packs, i);
        if (entry == nullptr) continue;
        std::fprintf(
            stderr,
            "[app-ui] content-pack name=\"%s\" version=\"%s\" author=\"%s\" "
            "priority=%d state=%s reason=\"%s\"\n",
            entry->manifest.name, entry->manifest.version,
            entry->manifest.author, entry->manifest.priority,
            entry->manifest.enabled ? "installed" : "skipped",
            entry->manifest.enabled ? "" : packSkipReason(entry, disabledList));
    }
    for (int i = 0; i < skipped; ++i) {
        const char *reason = mdkr_mod_registry_skip_reason(packs, i);
        std::fprintf(stderr,
                     "[app-ui] content-pack name=\"%s\" state=skipped "
                     "reason=\"%s\"\n",
                     packs->skip_name[i], reason != nullptr ? reason : "");
    }
}

bool drawContentSection(SDL_Window *window, bool compact,
                        const MdkrModRegistry *packs,
                        const char *disabledList) {
    bool changed = false;
    const int count = mdkr_mod_registry_count(packs);
    const int unreadable = mdkr_mod_registry_skipped(packs);

    ui::Gap(ui::kGapS);
    if (!compact) {
        ui::TextSubtleWrapped(
            "Put a pack's folder in the mods folder beside your saves. "
            "Everything found there is listed below, installed or skipped "
            "with the reason.");
    }
    ui::Gap(ui::kGapS);
    ImGui::Indent(ui::kGapM);
    changed |= drawKey(window, MDKR_CONTENT_PACKS_ENABLED, compact);
    changed |= drawKey(window, MDKR_CONTENT_PACK_DISABLED, compact);

    ImGui::SeparatorText("Installed");
    ui::Gap(ui::kGapXS);
    int installed = 0;
    for (int i = 0; i < count; ++i) {
        const MdkrModEntry *entry = mdkr_mod_registry_entry(packs, i);
        if (entry == nullptr || !entry->manifest.enabled) continue;
        ImGui::BulletText("%s", packSummary(entry).c_str());
        ++installed;
    }
    if (installed == 0) {
        ui::TextSubtleWrapped(
            count == 0 && unreadable == 0
                ? "Nothing yet. A pack is a folder with a pack.ini file in it."
                : "None of the packs found are in use. Every one of them is "
                  "listed below with the reason.");
    } else if (!compact) {
        ui::TextSubtleWrapped(
            "Where two packs supply the same artwork, the higher priority "
            "wins.");
    }

    if (count - installed + unreadable > 0) {
        ImGui::SeparatorText("Skipped");
        ui::Gap(ui::kGapXS);
        for (int i = 0; i < count; ++i) {
            const MdkrModEntry *entry = mdkr_mod_registry_entry(packs, i);
            if (entry == nullptr || entry->manifest.enabled) continue;
            ImGui::BulletText("%s — %s", entry->manifest.name,
                              packSkipReason(entry, disabledList));
        }
        for (int i = 0; i < unreadable; ++i) {
            const char *reason = mdkr_mod_registry_skip_reason(packs, i);
            ImGui::BulletText("%s — %s", packs->skip_name[i],
                              reason != nullptr && reason[0] != '\0'
                                  ? reason : "it could not be read");
        }
    }
    ImGui::Unindent(ui::kGapM);
    ui::Gap(ui::kGapS);
    return changed;
}

MdkrModernCharacterRegistry g_characterRegistry{};
bool g_characterRegistryLoaded = false;
std::string g_characterRegistryDirectory;
char g_characterImportPath[MDKR_MODERN_CHARACTER_PATH_MAX] = {0};
std::string g_characterManagerReport;
std::string g_characterPendingRemoval;
std::string g_characterWorkshopSelection;

struct CharacterIdentityEdit {
    bool loaded = false;
    uint8_t sourceSha256[32] = {0};
    char portraitPath[MDKR_MODERN_CHARACTER_PATH_MAX] = {0};
    float minimapRgb[3] = {0.86f, 0.28f, 0.56f};
    std::array<uint8_t, MDKR_MODERN_PORTRAIT_BYTES> canvas{};
    std::vector<std::array<uint8_t, MDKR_MODERN_PORTRAIT_BYTES>> undo;
    std::vector<std::array<uint8_t, MDKR_MODERN_PORTRAIT_BYTES>> redo;
    float paintRgba[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    int tool = 0;
    int selectedPixel[2] = {20, 20};
    bool canvasDirty = false;
    bool strokeActive = false;
};

std::map<std::string, CharacterIdentityEdit> g_characterIdentityEdits;

struct CharacterProfileEdit {
    bool loaded = false;
    uint8_t sourceSha256[32] = {0};
    uint32_t donor = 9u;
    uint32_t vehicleMask = 7u;
};

std::map<std::string, CharacterProfileEdit> g_characterProfileEdits;
std::map<std::string, int> g_characterAssemblyPlayers;
std::map<std::string, int> g_characterTestPlayers;
std::map<std::string, MdkrCharacterPreviewResult> g_characterPreviewResults;
SettingsCharacterPreviewRequest g_characterPreviewRequest;
bool g_characterPreviewRequested = false;

struct CharacterTuningEdit {
    bool loaded = false;
    float scale = 1.0f;
    float offset[3] = {0.0f, 0.0f, 0.0f};
    float rotation[3] = {0.0f, 0.0f, 0.0f};
    float animationSpeed = 1.0f;
    float lodBias = 0.0f;
    unsigned vehicleMask = 7u;
    struct Context {
        float scale = 1.0f;
        float offset[3] = {0.0f, 0.0f, 0.0f};
        float rotation[3] = {0.0f, 0.0f, 0.0f};
        float contacts[MDKR_MODERN_CHARACTER_CONTACTS][3] = {};
    } context[MDKR_CHARACTER_CONTEXT_COUNT];
};

std::map<std::string, CharacterTuningEdit> g_characterTuning;

struct CharacterRigEdit {
    struct Joint {
        uint32_t node = 0u;
        std::string name;
        int parentJoint = -1;
        int32_t parentNode = -1;
        float bindPosition[3] = {};
    };
    struct Role {
        int joint = -1;
        bool inferred = false;
        float confidence = 1.0f;
        float rest[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        float bend[3] = {0.0f, 0.0f, 0.0f};
    };
    bool loaded = false;
    int mode = MDKR_MODERN_RIG_AUTHORED_CLIPS_ONLY;
    bool reviewed = false;
    uint8_t sourceSha256[32] = {};
    std::vector<Joint> joints;
    std::vector<int32_t> nodeParents;
    Role roles[MDKR_MODERN_HUMANOID_ROLE_COUNT];
    int selectedRole = -1;
    int selectedJoint = -1;
    int skeletonView = 0;
    bool showAllJoints = true;
    std::string error;
};

std::map<std::string, CharacterRigEdit> g_characterRigEdits;

void refreshCharacterRegistry();

float characterConfigFloat(int player, const char *packageId,
                           const char *suffix, float fallback,
                           float minimum, float maximum) {
    const std::string profileKey = "custom_character_profile_" +
        std::string(packageId) + "_" + suffix;
    const std::string legacyKey = "custom_character_p" +
        std::to_string(player + 1) + "_" + suffix;
    const std::string text = AppConfig::get(profileKey,
                                             AppConfig::get(legacyKey));
    if (text.empty()) return fallback;
    char *end = nullptr;
    errno = 0;
    const float value = std::strtof(text.c_str(), &end);
    return errno == 0 && end != text.c_str() && *end == '\0' &&
            std::isfinite(value) && value >= minimum && value <= maximum
        ? value : fallback;
}

CharacterTuningEdit &loadCharacterTuning(int player, const char *packageId) {
    static const char *contextNames[MDKR_CHARACTER_CONTEXT_COUNT] = {
        "select", "car", "hovercraft", "plane"
    };
    CharacterTuningEdit &edit = g_characterTuning[packageId];
    if (edit.loaded) return edit;
    edit.scale = characterConfigFloat(player, packageId, "scale", 1.0f, 0.1f, 5.0f);
    edit.offset[0] = characterConfigFloat(player, packageId, "offset_x", 0.0f, -500.0f, 500.0f);
    edit.offset[1] = characterConfigFloat(player, packageId, "offset_y", 0.0f, -500.0f, 500.0f);
    edit.offset[2] = characterConfigFloat(player, packageId, "offset_z", 0.0f, -500.0f, 500.0f);
    edit.rotation[0] = characterConfigFloat(player, packageId, "rotation_x", 0.0f, -180.0f, 180.0f);
    edit.rotation[1] = characterConfigFloat(player, packageId, "rotation_y", 0.0f, -180.0f, 180.0f);
    edit.rotation[2] = characterConfigFloat(player, packageId, "rotation_z", 0.0f, -180.0f, 180.0f);
    edit.animationSpeed = characterConfigFloat(player, packageId, "animation_speed", 1.0f, 0.05f, 4.0f);
    edit.lodBias = characterConfigFloat(player, packageId, "lod_bias", 0.0f, -3.0f, 3.0f);
    const float mask = characterConfigFloat(player, packageId, "vehicle_mask", 7.0f, 1.0f, 7.0f);
    edit.vehicleMask = static_cast<unsigned>(mask);
    if (edit.vehicleMask == 0u || edit.vehicleMask > 7u) edit.vehicleMask = 7u;
    for (unsigned context = 0u;
         context < MDKR_CHARACTER_CONTEXT_COUNT; ++context) {
        const std::string prefix = std::string(contextNames[context]) + "_";
        edit.context[context].scale = characterConfigFloat(
            player, packageId, (prefix + "scale").c_str(), 1.0f, 0.1f, 5.0f);
        static const char *axes[] = {"x", "y", "z"};
        for (unsigned axis = 0u; axis < 3u; ++axis) {
            edit.context[context].offset[axis] = characterConfigFloat(
                player, packageId,
                (prefix + "offset_" + axes[axis]).c_str(),
                0.0f, -10.0f, 10.0f);
            edit.context[context].rotation[axis] = characterConfigFloat(
                player, packageId,
                (prefix + "rotation_" + axes[axis]).c_str(),
                0.0f, -180.0f, 180.0f);
        }
        if (context != MDKR_CHARACTER_CONTEXT_SELECT) {
            static const char *contactNames[MDKR_MODERN_CHARACTER_CONTACTS] = {
                "hand_left", "hand_right", "foot_left", "foot_right"
            };
            for (unsigned contact = 0u;
                 contact < MDKR_MODERN_CHARACTER_CONTACTS; ++contact) {
                for (unsigned axis = 0u; axis < 3u; ++axis) {
                    edit.context[context].contacts[contact][axis] =
                        characterConfigFloat(
                            player, packageId,
                            (prefix + contactNames[contact] + "_" + axes[axis]).c_str(),
                            0.0f, -1.0f, 1.0f);
                }
            }
        }
    }
    edit.loaded = true;
    return edit;
}

std::string characterFloatText(float value) {
    char text[48];
    std::snprintf(text, sizeof(text), "%.7g", static_cast<double>(value));
    return text;
}

const char *characterFitContextId(unsigned context) {
    static const char *ids[MDKR_CHARACTER_CONTEXT_COUNT] = {
        "select", "car", "hovercraft", "plane"
    };
    return context < MDKR_CHARACTER_CONTEXT_COUNT ? ids[context] : "invalid";
}

std::string characterFitReviewSignature(
    const MdkrModernCharacterEntry *entry,
    const CharacterTuningEdit &edit,
    unsigned context) {
    if (entry == nullptr || context >= MDKR_CHARACTER_CONTEXT_COUNT) return {};
    std::string canonical = "mdkr-character-fit-review-v1\n";
    canonical.append(reinterpret_cast<const char *>(entry->source_sha256),
                     sizeof(entry->source_sha256));
    canonical += "\n" + std::to_string(entry->donor) + "\n";
    const auto appendFloat = [&canonical](float value) {
        canonical += characterFloatText(value);
        canonical.push_back('\n');
    };
    appendFloat(edit.scale);
    for (float value : edit.offset) appendFloat(value);
    for (float value : edit.rotation) appendFloat(value);
    appendFloat(edit.animationSpeed);
    const CharacterTuningEdit::Context &placement = edit.context[context];
    appendFloat(placement.scale);
    for (float value : placement.offset) appendFloat(value);
    for (float value : placement.rotation) appendFloat(value);
    if (context != MDKR_CHARACTER_CONTEXT_SELECT) {
        for (const auto &contact : placement.contacts) {
            for (float value : contact) appendFloat(value);
        }
    }
    char digest[MDKR_SHA256_HEX_SIZE];
    mdkr_sha256_hex(canonical.data(), canonical.size(), digest);
    return digest;
}

std::string characterFitReviewKey(const char *packageId, unsigned context) {
    return "custom_character_profile_" + std::string(packageId) + "_" +
        characterFitContextId(context) + "_review_signature";
}

bool characterFitReviewed(const MdkrModernCharacterEntry *entry,
                          const CharacterTuningEdit &edit,
                          unsigned context) {
    const std::string signature = characterFitReviewSignature(
        entry, edit, context);
    return !signature.empty() &&
        AppConfig::get(characterFitReviewKey(entry->id, context)) == signature;
}

bool persistCharacterFitReview(const MdkrModernCharacterEntry *entry,
                               const CharacterTuningEdit &edit,
                               unsigned context) {
    const std::string signature = characterFitReviewSignature(
        entry, edit, context);
    if (signature.empty()) return false;
    AppConfig::set(characterFitReviewKey(entry->id, context), signature);
    const AppConfig::PersistResult result = AppConfig::save();
    if (AppConfig::persistResultApplied(result)) {
        setStatus("Exact-context fit review saved for this source and tuning.",
                  AppTheme::good());
        return true;
    }
    setStatus("The exact-context fit review could not be saved.",
              AppTheme::bad());
    return false;
}

bool clearCharacterFitReview(const MdkrModernCharacterEntry *entry,
                             unsigned context) {
    if (entry == nullptr || context >= MDKR_CHARACTER_CONTEXT_COUNT) {
        return false;
    }
    AppConfig::set(characterFitReviewKey(entry->id, context), "");
    const AppConfig::PersistResult result = AppConfig::save();
    if (AppConfig::persistResultApplied(result)) {
        setStatus("This context is open for fit review again.",
                  AppTheme::accent());
        return true;
    }
    setStatus("The fit review could not be reopened.", AppTheme::bad());
    return false;
}

bool persistCharacterTuning(const char *packageId,
                            const CharacterTuningEdit &edit) {
    static const char *contextNames[MDKR_CHARACTER_CONTEXT_COUNT] = {
        "select", "car", "hovercraft", "plane"
    };
    const std::string prefix = "custom_character_profile_" +
        std::string(packageId) + "_";
    AppConfig::set(prefix + "scale", characterFloatText(edit.scale));
    AppConfig::set(prefix + "offset_x", characterFloatText(edit.offset[0]));
    AppConfig::set(prefix + "offset_y", characterFloatText(edit.offset[1]));
    AppConfig::set(prefix + "offset_z", characterFloatText(edit.offset[2]));
    AppConfig::set(prefix + "rotation_x", characterFloatText(edit.rotation[0]));
    AppConfig::set(prefix + "rotation_y", characterFloatText(edit.rotation[1]));
    AppConfig::set(prefix + "rotation_z", characterFloatText(edit.rotation[2]));
    AppConfig::set(prefix + "animation_speed", characterFloatText(edit.animationSpeed));
    AppConfig::set(prefix + "lod_bias", characterFloatText(edit.lodBias));
    AppConfig::set(prefix + "vehicle_mask", std::to_string(edit.vehicleMask));
    for (unsigned context = 0u;
         context < MDKR_CHARACTER_CONTEXT_COUNT; ++context) {
        const std::string contextPrefix = prefix + contextNames[context] + "_";
        AppConfig::set(contextPrefix + "scale",
                       characterFloatText(edit.context[context].scale));
        static const char *axes[] = {"x", "y", "z"};
        for (unsigned axis = 0u; axis < 3u; ++axis) {
            AppConfig::set(contextPrefix + "offset_" + axes[axis],
                           characterFloatText(
                               edit.context[context].offset[axis]));
            AppConfig::set(contextPrefix + "rotation_" + axes[axis],
                           characterFloatText(
                               edit.context[context].rotation[axis]));
        }
        if (context != MDKR_CHARACTER_CONTEXT_SELECT) {
            static const char *contactNames[MDKR_MODERN_CHARACTER_CONTACTS] = {
                "hand_left", "hand_right", "foot_left", "foot_right"
            };
            for (unsigned contact = 0u;
                 contact < MDKR_MODERN_CHARACTER_CONTACTS; ++contact) {
                for (unsigned axis = 0u; axis < 3u; ++axis) {
                    AppConfig::set(
                        contextPrefix + contactNames[contact] + "_" + axes[axis],
                        characterFloatText(
                            edit.context[context].contacts[contact][axis]));
                }
            }
        }
    }
    const AppConfig::PersistResult result = AppConfig::save();
    if (AppConfig::persistResultApplied(result)) {
        /* Exact-preview evidence describes the saved tuning at capture time.
         * Never leave it looking current after any fit or solver edit. */
        g_characterPreviewResults.erase(packageId);
        setStatus("Character fit settings saved; they apply on play.",
                  AppTheme::good());
        return true;
    }
    setStatus("Character fit settings could not be saved.", AppTheme::bad());
    return false;
}

std::string readCharacterManagerResult(const std::string &path) {
    std::FILE *file = mdkr_fopen_utf8(path.c_str(), "rb");
    if (file == nullptr) return {};
    std::string text;
    char buffer[1024];
    size_t count;
    while (text.size() < 64u * 1024u &&
           (count = std::fread(buffer, 1u, sizeof(buffer), file)) != 0u) {
        const size_t room = 64u * 1024u - text.size();
        text.append(buffer, count < room ? count : room);
    }
    std::fclose(file);
    return text;
}

bool runCharacterManager(const char *command,
                         const std::vector<std::string> &commandArguments) {
    char toolPath[MDKR_MODERN_CHARACTER_PATH_MAX];
    int regular = 0;
    if (g_characterRegistryDirectory.empty()) refreshCharacterRegistry();
    if (g_characterRegistryDirectory.empty()) {
        g_characterManagerReport = "No writable character directory is available.";
        return false;
    }
    const char *overrideTool = std::getenv("MDKR_CHARACTER_MANAGER");
    if (overrideTool != nullptr && overrideTool[0] != '\0') {
        std::snprintf(toolPath, sizeof(toolPath), "%s", overrideTool);
    } else if (!mdkr_user_resource_path("tools/character_package_manager.py",
                                        toolPath, sizeof(toolPath))) {
        g_characterManagerReport = "The character importer could not be located.";
        return false;
    }
    if (mdkr_path_query_utf8(toolPath, nullptr, &regular, nullptr) != 0 || !regular) {
        g_characterManagerReport =
            "This build does not include the character compiler. Install from a source build or set MDKR_CHARACTER_MANAGER.";
        return false;
    }
    const std::string resultPath = g_characterRegistryDirectory +
        "/.launcher-character-result.json";
    (void)mdkr_remove_utf8(resultPath.c_str());
    const char *pythonOverride = std::getenv("MDKR_CHARACTER_PYTHON");
    const char *interpreters[] = {
        pythonOverride != nullptr && pythonOverride[0] != '\0'
            ? pythonOverride : "python3",
        "python",
    };
    int launchError = 0;
    int exitCode = -1;
    bool launched = false;
    for (const char *interpreter : interpreters) {
        std::vector<const char *> arguments = {
            toolPath, "--directory", g_characterRegistryDirectory.c_str(),
            "--result-file", resultPath.c_str(), command,
        };
        arguments.reserve(arguments.size() + commandArguments.size() + 1u);
        for (const std::string &argument : commandArguments) {
            arguments.push_back(argument.c_str());
        }
        arguments.push_back(nullptr);
        launchError = mdkr_spawn_wait_utf8(
            interpreter, arguments.data(), &exitCode);
        if (launchError == 0) {
            launched = true;
            break;
        }
        if (pythonOverride != nullptr && pythonOverride[0] != '\0') break;
    }
    g_characterManagerReport = readCharacterManagerResult(resultPath);
    (void)mdkr_remove_utf8(resultPath.c_str());
    if (!launched) {
        g_characterManagerReport =
            "Python 3 could not be started for the bundled character compiler (error " +
            std::to_string(launchError) + ").";
        return false;
    }
    if (g_characterManagerReport.empty()) {
        g_characterManagerReport = exitCode == 0
            ? "The importer completed without a diagnostic report."
            : "The importer failed without a diagnostic report.";
    }
    if (exitCode == 0) refreshCharacterRegistry();
    return exitCode == 0;
}

bool importCharacterPackage(const std::string &path) {
    MdkrModernCharacterInstallResult result{};
    if (g_characterRegistryDirectory.empty()) refreshCharacterRegistry();
    if (!g_characterRegistryDirectory.empty() &&
        mdkr_modern_character_install_portable(
            path.c_str(), g_characterRegistryDirectory.c_str(), &result)) {
        g_characterManagerReport = result.message;
        refreshCharacterRegistry();
        return true;
    }
    g_characterManagerReport = result.message;
    if (result.needs_compiler) {
        return runCharacterManager("install", {path});
    }
    return false;
}

bool reviseCharacterIdentity(const char *packageId, const char *portraitPath,
                             const float minimapRgb[3]) {
    std::vector<std::string> arguments = {packageId, portraitPath};
    for (unsigned component = 0u; component < 3u; ++component) {
        const int byte = static_cast<int>(std::lround(
            std::clamp(minimapRgb[component], 0.0f, 1.0f) * 255.0f));
        arguments.push_back(std::to_string(byte));
    }
    return runCharacterManager("revise-identity", arguments);
}

bool reviseCharacterIdentityRgba(
    const char *packageId,
    const std::array<uint8_t, MDKR_MODERN_PORTRAIT_BYTES> &rgba,
    const float minimapRgb[3]) {
    static const char hexDigits[] = "0123456789abcdef";
    std::string encoded;
    encoded.resize(rgba.size() * 2u);
    for (size_t index = 0u; index < rgba.size(); ++index) {
        encoded[index * 2u] = hexDigits[rgba[index] >> 4u];
        encoded[index * 2u + 1u] = hexDigits[rgba[index] & 0xFu];
    }
    std::vector<std::string> arguments = {packageId, std::move(encoded)};
    for (unsigned component = 0u; component < 3u; ++component) {
        const int byte = static_cast<int>(std::lround(
            std::clamp(minimapRgb[component], 0.0f, 1.0f) * 255.0f));
        arguments.push_back(std::to_string(byte));
    }
    return runCharacterManager("revise-identity-rgba", arguments);
}

bool reviseCharacterProfile(const char *packageId, uint32_t donor,
                            uint32_t vehicleMask) {
    static const char *donorIds[] = {
        "krunch", "bumper", "tiptup", "conker", "timber",
        "banjo", "drumstick", "pipsy", "tt", "diddy",
    };
    static const char *vehicleIds[] = {"car", "hovercraft", "plane"};
    if (donor >= std::size(donorIds) || vehicleMask == 0u ||
        (vehicleMask & ~7u) != 0u) return false;
    std::vector<std::string> arguments = {packageId, donorIds[donor]};
    for (unsigned vehicle = 0u; vehicle < std::size(vehicleIds); ++vehicle) {
        if ((vehicleMask & (1u << vehicle)) != 0u) {
            arguments.emplace_back(vehicleIds[vehicle]);
        }
    }
    return runCharacterManager("revise-profile", arguments);
}

bool reviseCharacterRig(const char *packageId, const std::string &draftJson) {
    if (packageId == nullptr || packageId[0] == '\0' ||
        draftJson.empty() || draftJson.size() > 128u * 1024u ||
        g_characterRegistryDirectory.empty()) return false;
    const std::string draftPath = g_characterRegistryDirectory + "/." +
        packageId + ".launcher-rig-draft.json";
    (void)mdkr_remove_utf8(draftPath.c_str());
    std::FILE *file = mdkr_fopen_utf8(draftPath.c_str(), "wbx");
    if (file == nullptr) {
        g_characterManagerReport = "Could not create the bounded rig draft.";
        return false;
    }
    const bool payloadWritten =
        std::fwrite(draftJson.data(), 1u, draftJson.size(), file) ==
            draftJson.size();
    const bool flushed = std::fflush(file) == 0;
    const bool closed = std::fclose(file) == 0;
    const bool written = payloadWritten && flushed && closed;
    if (!written) {
        (void)mdkr_remove_utf8(draftPath.c_str());
        g_characterManagerReport = "Could not finish the bounded rig draft.";
        return false;
    }
    const bool revised = runCharacterManager(
        "revise-rig", {packageId, draftPath});
    (void)mdkr_remove_utf8(draftPath.c_str());
    return revised;
}

bool removeCharacterPackage(const std::string &id) {
    MdkrModernCharacterInstallResult result{};
    if (g_characterRegistryDirectory.empty()) refreshCharacterRegistry();
    if (!g_characterRegistryDirectory.empty() &&
        mdkr_modern_character_remove_installed(
            id.c_str(), g_characterRegistryDirectory.c_str(), &result)) {
        g_characterManagerReport = result.message;
        refreshCharacterRegistry();
        return true;
    }
    g_characterManagerReport = result.message;
    return false;
}

void refreshCharacterRegistry() {
    char directory[MDKR_MODERN_CHARACTER_PATH_MAX];
    mdkr_modern_character_registry_shutdown(&g_characterRegistry);
    /* Identity, donor, rig, or source changes invalidate every session-local
     * measurement associated with the prior registry snapshot. */
    g_characterPreviewResults.clear();
    g_characterRegistryDirectory.clear();
    if (mdkr_user_characters_directory(directory, sizeof(directory))) {
        g_characterRegistryDirectory = directory;
        (void)mdkr_modern_character_registry_init(&g_characterRegistry,
                                                   directory);
    }
    g_characterRegistryLoaded = true;
}

const char *donorName(uint32_t donor) {
    static const char *names[] = {
        "Krunch", "Bumper", "Tiptup", "Conker", "Timber",
        "Banjo", "Drumstick", "Pipsy", "T.T.", "Diddy",
    };
    return donor < std::size(names) ? names[donor] : "Unknown";
}

unsigned countCharacterBits(uint32_t value) {
    unsigned count = 0u;
    while (value != 0u) {
        count += value & 1u;
        value >>= 1u;
    }
    return count;
}

struct CharacterSemanticLabel {
    const char *name;
    uint32_t bit;
};

const CharacterSemanticLabel kRaceCharacterSemantics[] = {
    {"race.steer", MDKR_CHARACTER_SEMANTIC_RACE_STEER},
    {"race.reverse", MDKR_CHARACTER_SEMANTIC_RACE_REVERSE},
    {"race.boost", MDKR_CHARACTER_SEMANTIC_RACE_BOOST},
    {"race.damage", MDKR_CHARACTER_SEMANTIC_RACE_DAMAGE},
    {"race.item", MDKR_CHARACTER_SEMANTIC_RACE_ITEM},
    {"race.spin", MDKR_CHARACTER_SEMANTIC_RACE_SPIN},
    {"race.airborne", MDKR_CHARACTER_SEMANTIC_RACE_AIRBORNE},
    {"race.land", MDKR_CHARACTER_SEMANTIC_RACE_LAND},
    {"race.finish_win", MDKR_CHARACTER_SEMANTIC_RACE_FINISH_WIN},
    {"race.finish_lose", MDKR_CHARACTER_SEMANTIC_RACE_FINISH_LOSE},
};

const CharacterSemanticLabel kSelectCharacterSemantics[] = {
    {"select.idle", MDKR_CHARACTER_SEMANTIC_SELECT_IDLE},
    {"select.hover", MDKR_CHARACTER_SEMANTIC_SELECT_HOVER},
    {"select.confirm", MDKR_CHARACTER_SEMANTIC_SELECT_CONFIRM},
};

const CharacterSemanticLabel kHumanoidRigRoles[] = {
    {"hips", MDKR_CHARACTER_RIG_HIPS},
    {"spine", MDKR_CHARACTER_RIG_SPINE},
    {"chest", MDKR_CHARACTER_RIG_CHEST},
    {"head", MDKR_CHARACTER_RIG_HEAD},
    {"upper_arm.left", MDKR_CHARACTER_RIG_UPPER_ARM_LEFT},
    {"lower_arm.left", MDKR_CHARACTER_RIG_LOWER_ARM_LEFT},
    {"hand.left", MDKR_CHARACTER_RIG_HAND_LEFT},
    {"upper_arm.right", MDKR_CHARACTER_RIG_UPPER_ARM_RIGHT},
    {"lower_arm.right", MDKR_CHARACTER_RIG_LOWER_ARM_RIGHT},
    {"hand.right", MDKR_CHARACTER_RIG_HAND_RIGHT},
    {"upper_leg.left", MDKR_CHARACTER_RIG_UPPER_LEG_LEFT},
    {"lower_leg.left", MDKR_CHARACTER_RIG_LOWER_LEG_LEFT},
    {"foot.left", MDKR_CHARACTER_RIG_FOOT_LEFT},
    {"upper_leg.right", MDKR_CHARACTER_RIG_UPPER_LEG_RIGHT},
    {"lower_leg.right", MDKR_CHARACTER_RIG_LOWER_LEG_RIGHT},
    {"foot.right", MDKR_CHARACTER_RIG_FOOT_RIGHT},
};

CharacterRigEdit &loadCharacterRigEdit(
    const MdkrModernCharacterEntry *entry) {
    CharacterRigEdit &edit = g_characterRigEdits[entry->id];
    if (edit.loaded &&
        std::memcmp(edit.sourceSha256, entry->source_sha256,
                    sizeof(edit.sourceSha256)) == 0) return edit;
    edit = CharacterRigEdit{};
    std::memcpy(edit.sourceSha256, entry->source_sha256,
                sizeof(edit.sourceSha256));
    edit.mode = entry->rig_present != 0u
        ? static_cast<int>(entry->rig_mode)
        : MDKR_MODERN_RIG_AUTHORED_CLIPS_ONLY;
    edit.reviewed = (entry->rig_flags & MDKR_MODERN_RIG_REVIEWED) != 0u;
    MdkrModernCharacterAsset asset{};
    char error[256];
    if (!mdkr_modern_character_asset_load_file(
            entry->path, &asset, error, sizeof(error))) {
        edit.error = error;
        edit.loaded = true;
        return edit;
    }
    edit.nodeParents.resize(entry->stats.nodes, -1);
    for (uint32_t nodeIndex = 0u; nodeIndex < entry->stats.nodes; ++nodeIndex) {
        MdkrModernNode node;
        if (mdkr_modern_character_asset_node(&asset, nodeIndex, &node)) {
            edit.nodeParents[nodeIndex] = node.parent;
        }
    }
    for (uint32_t jointIndex = 0u; jointIndex < entry->stats.joints;
         ++jointIndex) {
        MdkrModernJoint joint;
        MdkrModernNode node;
        if (!mdkr_modern_character_asset_joint(
                &asset, jointIndex, &joint) ||
            !mdkr_modern_character_asset_node(&asset, joint.node, &node)) {
            continue;
        }
        const auto duplicate = std::find_if(
            edit.joints.begin(), edit.joints.end(),
            [joint](const CharacterRigEdit::Joint &candidate) {
                return candidate.node == joint.node;
            });
        if (duplicate != edit.joints.end()) continue;
        const char *name = mdkr_modern_character_asset_string(&asset, node.name);
        CharacterRigEdit::Joint target;
        target.node = joint.node;
        target.name = name != nullptr ? name : "";
        if (!mdkr_modern_character_asset_node_bind_position(
                &asset, joint.node, target.bindPosition)) {
            edit.error = "A skin joint has an invalid bind hierarchy.";
            continue;
        }
        if (!mdkr_modern_character_asset_joint_parent_node(
                &asset, jointIndex, &target.parentNode)) {
            edit.error = "A skin joint has an invalid parent hierarchy.";
            continue;
        }
        edit.joints.push_back(std::move(target));
    }
    mdkr_modern_character_asset_unload(&asset);
    std::vector<int> nodeToJoint(edit.nodeParents.size(), -1);
    for (size_t joint = 0u; joint < edit.joints.size(); ++joint) {
        nodeToJoint[edit.joints[joint].node] = static_cast<int>(joint);
    }
    for (CharacterRigEdit::Joint &joint : edit.joints) {
        if (joint.parentNode >= 0 &&
            static_cast<size_t>(joint.parentNode) < nodeToJoint.size()) {
            joint.parentJoint = nodeToJoint[joint.parentNode];
        }
    }
    for (size_t slot = 0u; slot < std::size(kHumanoidRigRoles); ++slot) {
        if ((entry->rig_role_mask & kHumanoidRigRoles[slot].bit) == 0u) continue;
        const uint32_t mappedNode = entry->rig_role_node[slot];
        const auto mapped = std::find_if(
            edit.joints.begin(), edit.joints.end(),
            [mappedNode](const CharacterRigEdit::Joint &candidate) {
                return candidate.node == mappedNode;
            });
        if (mapped == edit.joints.end()) {
            edit.error = "A compiled rig role does not resolve to a skin joint.";
            continue;
        }
        CharacterRigEdit::Role &role = edit.roles[slot];
        role.joint = static_cast<int>(mapped - edit.joints.begin());
        role.inferred = (entry->rig_role_flags[slot] & 1u) != 0u;
        role.confidence = static_cast<float>(
            entry->rig_role_confidence_milli[slot]) / 1000.0f;
        std::memcpy(role.rest, entry->rig_role_rest_rotation[slot],
                    sizeof(role.rest));
        std::memcpy(role.bend, entry->rig_role_bend_axis[slot],
                    sizeof(role.bend));
    }
    edit.loaded = true;
    return edit;
}

bool characterRigRolesComplete(const CharacterRigEdit &edit) {
    for (const CharacterRigEdit::Role &role : edit.roles) {
        if (role.joint < 0 ||
            role.joint >= static_cast<int>(edit.joints.size())) return false;
    }
    return true;
}

bool characterRigNodeUsed(const CharacterRigEdit &edit, size_t exceptRole,
                          int joint) {
    for (size_t role = 0u; role < std::size(edit.roles); ++role) {
        if (role != exceptRole && edit.roles[role].joint == joint) return true;
    }
    return false;
}

bool characterRigAncestor(const CharacterRigEdit &edit, uint32_t ancestor,
                          uint32_t descendant) {
    int32_t node = descendant < edit.nodeParents.size()
        ? edit.nodeParents[descendant] : -1;
    for (size_t depth = 0u; node >= 0 && depth < edit.nodeParents.size();
         ++depth) {
        if (static_cast<uint32_t>(node) == ancestor) return true;
        node = static_cast<size_t>(node) < edit.nodeParents.size()
            ? edit.nodeParents[static_cast<size_t>(node)] : -1;
    }
    return false;
}

void drawCharacterRigSkeleton(const MdkrModernCharacterEntry *entry,
                              CharacterRigEdit &edit) {
    if (edit.joints.empty()) return;
    ImGui::SeparatorText("Bind-pose skeleton");
    ui::TextSubtleWrapped(
        "This is the package's actual skin-joint hierarchy in bind pose. It is a spatial mapping aid; exact role controls and validation remain authoritative.");
    (void)ImGui::RadioButton("Front##rig-view", &edit.skeletonView, 0);
    ImGui::SameLine();
    (void)ImGui::RadioButton("Side##rig-view", &edit.skeletonView, 1);
    ImGui::SameLine();
    (void)ImGui::Checkbox("Show helper joints", &edit.showAllJoints);
    if (edit.selectedRole >= 0 &&
        edit.selectedRole < static_cast<int>(std::size(edit.roles))) {
        ImGui::TextDisabled("Active role: %s",
                            kHumanoidRigRoles[edit.selectedRole].name);
    }

    std::vector<int> roleByJoint(edit.joints.size(), -1);
    for (size_t role = 0u; role < std::size(edit.roles); ++role) {
        const int joint = edit.roles[role].joint;
        if (joint >= 0 && joint < static_cast<int>(edit.joints.size())) {
            roleByJoint[joint] = static_cast<int>(role);
        }
    }
    const bool sourceFacesZ = entry->source_forward < 2u;
    const unsigned horizontalAxis = edit.skeletonView == 0
        ? (sourceFacesZ ? 0u : 2u)
        : (sourceFacesZ ? 2u : 0u);
    float minimum[2] = {INFINITY, INFINITY};
    float maximum[2] = {-INFINITY, -INFINITY};
    unsigned visible = 0u;
    for (size_t joint = 0u; joint < edit.joints.size(); ++joint) {
        if (!edit.showAllJoints && roleByJoint[joint] < 0) continue;
        const float projected[2] = {
            edit.joints[joint].bindPosition[horizontalAxis],
            edit.joints[joint].bindPosition[1],
        };
        for (unsigned axis = 0u; axis < 2u; ++axis) {
            minimum[axis] = std::min(minimum[axis], projected[axis]);
            maximum[axis] = std::max(maximum[axis], projected[axis]);
        }
        ++visible;
    }
    if (visible == 0u) {
        ImGui::TextDisabled("Map at least one role to inspect it spatially.");
        return;
    }
    const float width = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    const float height = std::clamp(
        ImGui::GetFontSize() * 17.0f, 260.0f, 420.0f);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##rig-skeleton-canvas", ImVec2(width, height));
    ImDrawList *draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(origin, ImVec2(origin.x + width, origin.y + height),
                        IM_COL32(20, 24, 31, 255), 6.0f);
    draw->AddRect(origin, ImVec2(origin.x + width, origin.y + height),
                  IM_COL32(255, 255, 255, 42), 6.0f);
    const float spanX = std::max(maximum[0] - minimum[0], 1.0e-4f);
    const float spanY = std::max(maximum[1] - minimum[1], 1.0e-4f);
    const float padding = std::min(
        20.0f, std::min(width, height) * 0.1f);
    const float scale = std::min(
        (width - padding * 2.0f) / spanX,
        (height - padding * 2.0f) / spanY);
    const float centerX = (minimum[0] + maximum[0]) * 0.5f;
    const float centerY = (minimum[1] + maximum[1]) * 0.5f;
    const auto project = [&](const CharacterRigEdit::Joint &joint) {
        return ImVec2(
            origin.x + width * 0.5f +
                (joint.bindPosition[horizontalAxis] - centerX) * scale,
            origin.y + height * 0.5f -
                (joint.bindPosition[1] - centerY) * scale);
    };
    std::vector<ImVec2> points(edit.joints.size());
    for (size_t joint = 0u; joint < edit.joints.size(); ++joint) {
        points[joint] = project(edit.joints[joint]);
    }
    for (size_t joint = 0u; joint < edit.joints.size(); ++joint) {
        const CharacterRigEdit::Joint &child = edit.joints[joint];
        if ((!edit.showAllJoints && roleByJoint[joint] < 0) ||
            child.parentJoint < 0) continue;
        int parentJoint = child.parentJoint;
        size_t parentDepth = 0u;
        while (!edit.showAllJoints && parentJoint >= 0 &&
               roleByJoint[parentJoint] < 0 &&
               parentDepth++ < edit.joints.size()) {
            parentJoint = edit.joints[parentJoint].parentJoint;
        }
        if (parentDepth > edit.joints.size()) parentJoint = -1;
        if (parentJoint < 0) continue;
        const size_t parent = static_cast<size_t>(parentJoint);
        const bool selectedRoleMapped = edit.selectedRole >= 0 &&
            edit.selectedRole < static_cast<int>(std::size(edit.roles)) &&
            edit.roles[edit.selectedRole].joint >= 0 &&
            edit.roles[edit.selectedRole].joint <
                static_cast<int>(edit.joints.size());
        const bool highlighted = selectedRoleMapped &&
            characterRigAncestor(
                edit,
                edit.joints[edit.roles[edit.selectedRole].joint].node,
                child.node);
        draw->AddLine(points[parent], points[joint],
                      highlighted ? IM_COL32(104, 211, 255, 255)
                                  : IM_COL32(165, 174, 190, 150),
                      highlighted ? 3.0f : 1.5f);
    }
    int hoveredJoint = -1;
    float hoveredDistance = 144.0f;
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    for (size_t joint = 0u; joint < edit.joints.size(); ++joint) {
        if (!edit.showAllJoints && roleByJoint[joint] < 0) continue;
        const bool mapped = roleByJoint[joint] >= 0;
        const bool selected = edit.selectedJoint == static_cast<int>(joint) ||
            (edit.selectedRole >= 0 &&
             edit.selectedRole < static_cast<int>(std::size(edit.roles)) &&
             edit.roles[edit.selectedRole].joint == static_cast<int>(joint));
        draw->AddCircleFilled(
            points[joint], selected ? 6.0f : (mapped ? 4.5f : 3.0f),
            selected ? IM_COL32(255, 218, 92, 255)
                     : mapped ? IM_COL32(104, 211, 255, 255)
                              : IM_COL32(194, 201, 214, 180));
        const float dx = mouse.x - points[joint].x;
        const float dy = mouse.y - points[joint].y;
        const float distance = dx * dx + dy * dy;
        if (distance < hoveredDistance) {
            hoveredDistance = distance;
            hoveredJoint = static_cast<int>(joint);
        }
    }
    if (ImGui::IsItemHovered() && hoveredJoint >= 0) {
        const CharacterRigEdit::Joint &joint = edit.joints[hoveredJoint];
        const int role = roleByJoint[hoveredJoint];
        ImGui::SetTooltip("#%u · %s%s%s", joint.node, joint.name.c_str(),
                          role >= 0 ? "\nMapped to " : "",
                          role >= 0 ? kHumanoidRigRoles[role].name : "");
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            edit.selectedJoint = hoveredJoint;
            if (role >= 0) edit.selectedRole = role;
        }
    }
    if (edit.selectedJoint >= 0 &&
        edit.selectedJoint < static_cast<int>(edit.joints.size())) {
        const CharacterRigEdit::Joint &joint = edit.joints[edit.selectedJoint];
        ImGui::TextDisabled("Selected joint #%u · %s", joint.node,
                            joint.name.c_str());
        if (edit.selectedRole >= 0 &&
            edit.selectedRole < static_cast<int>(std::size(edit.roles)) &&
            edit.roles[edit.selectedRole].joint != edit.selectedJoint) {
            const bool used = characterRigNodeUsed(
                edit, static_cast<size_t>(edit.selectedRole),
                edit.selectedJoint);
            if (used) ImGui::BeginDisabled();
            const std::string label = "Assign to " + std::string(
                kHumanoidRigRoles[edit.selectedRole].name);
            if (ImGui::Button(label.c_str()) && !used) {
                CharacterRigEdit::Role &role = edit.roles[edit.selectedRole];
                role = CharacterRigEdit::Role{};
                role.joint = edit.selectedJoint;
                edit.reviewed = false;
            }
            if (used) ImGui::EndDisabled();
            ui::SpeakFocusedItem(
                label.c_str(),
                used ? "That joint already owns another semantic role."
                     : nullptr,
                "Assigns the selected bind-pose joint and clears rig review.");
        }
    }
}

std::string characterRigHierarchyError(const CharacterRigEdit &edit) {
    static const unsigned hierarchy[][2] = {
        {0u, 1u}, {1u, 2u}, {2u, 3u},
        {2u, 4u}, {4u, 5u}, {5u, 6u},
        {2u, 7u}, {7u, 8u}, {8u, 9u},
        {0u, 10u}, {10u, 11u}, {11u, 12u},
        {0u, 13u}, {13u, 14u}, {14u, 15u},
    };
    if (!characterRigRolesComplete(edit)) return "Map all 16 humanoid roles.";
    for (const auto &relationship : hierarchy) {
        const CharacterRigEdit::Role &parent = edit.roles[relationship[0]];
        const CharacterRigEdit::Role &child = edit.roles[relationship[1]];
        if (!characterRigAncestor(
                edit, edit.joints[parent.joint].node,
                edit.joints[child.joint].node)) {
            return std::string(kHumanoidRigRoles[relationship[0]].name) +
                " must be an ancestor of " +
                kHumanoidRigRoles[relationship[1]].name + ".";
        }
    }
    return {};
}

void normalizeCharacterRigVector(float *value, size_t count,
                                 bool allowZero) {
    double lengthSquared = 0.0;
    for (size_t index = 0u; index < count; ++index) {
        lengthSquared += static_cast<double>(value[index]) * value[index];
    }
    if (!std::isfinite(lengthSquared) || lengthSquared < 1.0e-12) {
        std::memset(value, 0, count * sizeof(*value));
        if (!allowZero && count == 4u) value[3] = 1.0f;
        return;
    }
    const float inverse = static_cast<float>(1.0 / std::sqrt(lengthSquared));
    for (size_t index = 0u; index < count; ++index) value[index] *= inverse;
}

std::string characterJsonString(const std::string &value) {
    static const char hex[] = "0123456789abcdef";
    std::string encoded = "\"";
    encoded.reserve(value.size() + 2u);
    for (unsigned char byte : value) {
        if (byte == '\"' || byte == '\\') {
            encoded.push_back('\\');
            encoded.push_back(static_cast<char>(byte));
        } else if (byte < 0x20u) {
            encoded += "\\u00";
            encoded.push_back(hex[byte >> 4u]);
            encoded.push_back(hex[byte & 0xFu]);
        } else {
            encoded.push_back(static_cast<char>(byte));
        }
    }
    encoded.push_back('\"');
    return encoded;
}

std::string characterRigDraftJson(const CharacterRigEdit &edit) {
    std::string json = "{\n  \"schema\": \"mdkr-character-rig-draft-v1\",\n";
    json += "  \"mode\": \"";
    json += edit.mode == MDKR_MODERN_RIG_HUMANOID_RETARGET_V1
        ? "humanoid-retarget-v1" : "authored-clips-only";
    json += "\",\n  \"reviewed\": ";
    json += edit.reviewed ? "true" : "false";
    json += ",\n  \"roles\": {";
    bool first = true;
    for (size_t slot = 0u; slot < std::size(kHumanoidRigRoles); ++slot) {
        const CharacterRigEdit::Role &role = edit.roles[slot];
        if (role.joint < 0 ||
            role.joint >= static_cast<int>(edit.joints.size())) continue;
        const CharacterRigEdit::Joint &joint = edit.joints[role.joint];
        json += first ? "\n" : ",\n";
        first = false;
        json += "    " + characterJsonString(kHumanoidRigRoles[slot].name) +
            ": {\"node\": " + characterJsonString(joint.name) +
            ", \"inferred\": " + (role.inferred ? "true" : "false") +
            ", \"confidence\": " + characterFloatText(role.confidence) +
            ", \"rest_rotation_xyzw\": [";
        for (size_t axis = 0u; axis < 4u; ++axis) {
            if (axis != 0u) json += ", ";
            json += characterFloatText(role.rest[axis]);
        }
        json += "], \"bend_axis\": [";
        for (size_t axis = 0u; axis < 3u; ++axis) {
            if (axis != 0u) json += ", ";
            json += characterFloatText(role.bend[axis]);
        }
        json += "]}";
    }
    if (!first) json += "\n";
    json += "  }\n}\n";
    return json;
}

template <size_t Count>
std::string missingCharacterSemantics(
    uint32_t mask, const CharacterSemanticLabel (&semantics)[Count]) {
    std::string missing;
    for (const CharacterSemanticLabel &semantic : semantics) {
        if ((mask & semantic.bit) != 0u) continue;
        if (!missing.empty()) missing += ", ";
        missing += semantic.name;
    }
    return missing;
}

void requestCharacterPreview(const MdkrModernCharacterEntry *entry,
                             MdkrCharacterPreviewContext context,
                             int players);

bool drawCharacterRigStudio(const MdkrModernCharacterEntry *entry) {
    CharacterRigEdit &edit = loadCharacterRigEdit(entry);
    if (!edit.error.empty()) {
        ImGui::TextColored(AppTheme::bad(), "%s", edit.error.c_str());
        return false;
    }
    ui::TextSubtleWrapped(
        "Map semantic anatomy to the model's actual skin joints. Saving creates a validated source-v4 revision; the current playable cache remains active unless the complete compile succeeds.");
    const char *modeNames[] = {
        "Authored clips only", "Reviewed humanoid reference motion"
    };
    int mode = edit.mode;
    if (ImGui::BeginCombo("Rig behavior", modeNames[mode])) {
        for (int candidate = 0; candidate < 2; ++candidate) {
            if (ImGui::Selectable(modeNames[candidate], candidate == mode)) {
                edit.mode = candidate;
                edit.reviewed = false;
            }
        }
        ImGui::EndCombo();
    }
    if (edit.mode == MDKR_MODERN_RIG_AUTHORED_CLIPS_ONLY) {
        edit.reviewed = false;
        ui::TextSubtleWrapped(
            "Use this for creatures, unusual skeletons, or a character whose package supplies every intended clip. Any retained role notes stay inert; the engine will not procedurally alter the rig.");
    } else {
        ui::TextSubtleWrapped(
            "Changing a joint or solver basis clears review automatically. Intervening shoulder, neck, twist, and helper joints are allowed, but every semantic chain must preserve ancestry and every role must use a distinct skin joint.");
        drawCharacterRigSkeleton(entry, edit);
        for (size_t slot = 0u; slot < std::size(kHumanoidRigRoles); ++slot) {
            CharacterRigEdit::Role &role = edit.roles[slot];
            ImGui::PushID(static_cast<int>(slot));
            std::string preview = "Not mapped";
            if (role.joint >= 0 &&
                role.joint < static_cast<int>(edit.joints.size())) {
                const CharacterRigEdit::Joint &joint = edit.joints[role.joint];
                preview = "#" + std::to_string(joint.node) + " · " + joint.name;
            }
            if (ImGui::BeginCombo(kHumanoidRigRoles[slot].name,
                                  preview.c_str())) {
                edit.selectedRole = static_cast<int>(slot);
                edit.selectedJoint = role.joint;
                if (ImGui::Selectable("Not mapped", role.joint < 0) &&
                    role.joint >= 0) {
                    role = CharacterRigEdit::Role{};
                    edit.selectedJoint = -1;
                    edit.reviewed = false;
                }
                for (size_t jointIndex = 0u; jointIndex < edit.joints.size();
                     ++jointIndex) {
                    const bool used = characterRigNodeUsed(
                        edit, slot, static_cast<int>(jointIndex));
                    const CharacterRigEdit::Joint &joint =
                        edit.joints[jointIndex];
                    const std::string label = "#" +
                        std::to_string(joint.node) + " · " + joint.name +
                        "##rig-joint";
                    ImGui::PushID(static_cast<int>(jointIndex));
                    if (used) ImGui::BeginDisabled();
                    if (ImGui::Selectable(
                            label.c_str(), role.joint ==
                                static_cast<int>(jointIndex)) && !used &&
                        role.joint != static_cast<int>(jointIndex)) {
                        role = CharacterRigEdit::Role{};
                        role.joint = static_cast<int>(jointIndex);
                        edit.selectedJoint = role.joint;
                        edit.reviewed = false;
                    }
                    if (used) ImGui::EndDisabled();
                    ImGui::PopID();
                }
                ImGui::EndCombo();
            }
            if (role.joint >= 0) {
                ImGui::SameLine();
                if (role.inferred) {
                    ImGui::TextDisabled(
                        "inferred %.1f%%",
                        static_cast<double>(role.confidence * 100.0f));
                } else {
                    ImGui::TextDisabled("authored");
                }
                if (ImGui::TreeNode("Advanced solver basis")) {
                    ui::TextSubtleWrapped(
                        "Rest correction maps canonical engine axes into this joint's local basis. Bend is the preferred joint-local axis only for the exactly-opposite contact case; zero selects a stable automatic axis.");
                    (void)ImGui::DragFloat4(
                        "Rest correction XYZW", role.rest, 0.005f,
                        -1.0f, 1.0f, "%.4f",
                        ImGuiSliderFlags_AlwaysClamp);
                    if (ImGui::IsItemDeactivatedAfterEdit()) {
                        normalizeCharacterRigVector(role.rest, 4u, false);
                        edit.reviewed = false;
                    }
                    (void)ImGui::DragFloat3(
                        "Preferred bend axis", role.bend, 0.005f,
                        -1.0f, 1.0f, "%.4f",
                        ImGuiSliderFlags_AlwaysClamp);
                    if (ImGui::IsItemDeactivatedAfterEdit()) {
                        normalizeCharacterRigVector(role.bend, 3u, true);
                        edit.reviewed = false;
                    }
                    if (ImGui::Button("Reset canonical basis")) {
                        role.rest[0] = role.rest[1] = role.rest[2] = 0.0f;
                        role.rest[3] = 1.0f;
                        role.bend[0] = role.bend[1] = role.bend[2] = 0.0f;
                        edit.reviewed = false;
                    }
                    ImGui::TreePop();
                }
            }
            ImGui::PopID();
        }
    }
    const std::string hierarchyError = edit.mode ==
            MDKR_MODERN_RIG_HUMANOID_RETARGET_V1
        ? characterRigHierarchyError(edit) : std::string{};
    if (!hierarchyError.empty()) {
        ImGui::TextColored(AppTheme::bad(), "%s", hierarchyError.c_str());
    }
    const bool canReview = edit.mode == MDKR_MODERN_RIG_HUMANOID_RETARGET_V1 &&
        hierarchyError.empty();
    if (!canReview) ImGui::BeginDisabled();
    (void)ImGui::Checkbox(
        "I reviewed all roles and solver bases in every supported context",
        &edit.reviewed);
    if (!canReview) ImGui::EndDisabled();
    ui::SpeakFocusedItem(
        "Rig review", edit.reviewed ? "Approved" : "Not approved",
        "Only an explicit author review unlocks engine reference motion and vehicle contacts.");
    const bool canSave = edit.mode == MDKR_MODERN_RIG_AUTHORED_CLIPS_ONLY ||
        hierarchyError.empty();
    if (!canSave) ImGui::BeginDisabled();
    bool saved = false;
    if (ImGui::Button("Save rig revision")) {
        saved = reviseCharacterRig(entry->id, characterRigDraftJson(edit));
        setStatus(
            saved ? "Rig map compiled, validated, and activated."
                  : "Rig revision failed; the active character was not changed.",
            saved ? AppTheme::good() : AppTheme::bad());
    }
    if (!canSave) ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Discard draft")) {
        g_characterRigEdits.erase(entry->id);
        setStatus("Rig draft restored from the active package.",
                  AppTheme::subtle());
        return false;
    }
    return saved;
}

bool drawCharacterTuningEditor(int player,
                               const MdkrModernCharacterEntry *entry,
                               bool compact) {
    static const char *vehicleNames[] = {"Car", "Hovercraft", "Plane"};
    static const char *contextNames[MDKR_CHARACTER_CONTEXT_COUNT] = {
        "Character select", "Car", "Hovercraft", "Plane"
    };
    static const MdkrCharacterPreviewContext previewContexts[
        MDKR_CHARACTER_CONTEXT_COUNT] = {
            MDKR_CHARACTER_PREVIEW_SELECT,
            MDKR_CHARACTER_PREVIEW_CAR,
            MDKR_CHARACTER_PREVIEW_HOVERCRAFT,
            MDKR_CHARACTER_PREVIEW_PLANE,
        };
    static const char *forwardNames[] = {"+Z", "-Z", "+X", "-X"};
    bool changed = false;
    const bool contactReady = entry->rig_present != 0u &&
        entry->rig_mode == MDKR_MODERN_RIG_HUMANOID_RETARGET_V1 &&
        (entry->rig_flags & MDKR_MODERN_RIG_REVIEWED) != 0u &&
        entry->rig_role_mask == MDKR_CHARACTER_RIG_HUMANOID_MASK;
    CharacterTuningEdit &edit = loadCharacterTuning(player, entry->id);
    edit.vehicleMask &= entry->vehicle_mask;
    if (edit.vehicleMask == 0u) edit.vehicleMask = entry->vehicle_mask;

    ImGui::TextUnformatted("Enable this appearance in game on");
    for (unsigned vehicle = 0u; vehicle < 3u; ++vehicle) {
        if (vehicle != 0u) ImGui::SameLine();
        const unsigned bit = 1u << vehicle;
        const bool qualified = (entry->vehicle_mask & bit) != 0u;
        bool enabled = (edit.vehicleMask & bit) != 0u;
        if (!qualified) ImGui::BeginDisabled();
        const std::string label = std::string(vehicleNames[vehicle]) +
            "##vehicle-" + std::to_string(vehicle);
        if (ImGui::Checkbox(label.c_str(), &enabled)) {
            const unsigned candidate = enabled
                ? edit.vehicleMask | bit : edit.vehicleMask & ~bit;
            if ((candidate & entry->vehicle_mask) != 0u) {
                edit.vehicleMask = candidate;
                changed |= persistCharacterTuning(entry->id, edit);
            } else {
                setStatus("Keep at least one qualified vehicle pairing enabled.",
                          AppTheme::bad());
            }
        }
        if (!qualified) ImGui::EndDisabled();
    }
    ui::TextSubtleWrapped(
        "This is a local enable/disable subset of the package compatibility saved above. The in-game vehicle choice still owns physics and handling.");

    ImGui::SeparatorText("Source normalization");
    if ((entry->calibration_flags & 1u) != 0u) {
        ImGui::TextColored(
            AppTheme::good(), "Normalized v2 profile · source faces %s",
            entry->source_forward < std::size(forwardNames)
                ? forwardNames[entry->source_forward] : "unknown");
    } else {
        ImGui::TextColored(
            AppTheme::accent(),
            "Legacy transform: confirm height and facing before use");
    }
    if (entry->source_height > 0.0f && entry->target_height > 0.0f) {
        ImGui::TextDisabled(
            "Measured source extent %.4g m · intended standing height %.3g m",
            static_cast<double>(entry->source_height),
            static_cast<double>(entry->target_height));
        float height = entry->target_height * edit.scale;
        (void)ImGui::SliderFloat("Standing height", &height, 0.25f, 3.0f,
                                 "%.2f m", ImGuiSliderFlags_AlwaysClamp);
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            edit.scale = height / entry->target_height;
            if (edit.scale < 0.1f) edit.scale = 0.1f;
            if (edit.scale > 5.0f) edit.scale = 5.0f;
        }
    } else {
        (void)ImGui::SliderFloat("Character size", &edit.scale, 0.1f, 5.0f,
                                 "%.2fx", ImGuiSliderFlags_AlwaysClamp);
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        changed |= persistCharacterTuning(entry->id, edit);
    }
    if (ImGui::Button("Turn model around 180°")) {
        edit.rotation[1] += 180.0f;
        if (edit.rotation[1] > 180.0f) edit.rotation[1] -= 360.0f;
        changed |= persistCharacterTuning(entry->id, edit);
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset source facing")) {
        edit.rotation[0] = edit.rotation[1] = edit.rotation[2] = 0.0f;
        changed |= persistCharacterTuning(entry->id, edit);
    }
    ui::TextSubtleWrapped(
        "Facing cannot be inferred safely from arbitrary geometry. Use the author-declared axis first, then this explicit correction if the preview is backward.");

    ImGui::SeparatorText("Placement by context");
    ui::TextSubtleWrapped(
        "Select aligns the model's measured ground point. Vehicles align its pelvis/seat socket. Each correction is independent, so fixing one scene cannot break another.");
    unsigned reviewContexts = 1u;
    unsigned reviewedContexts = characterFitReviewed(
        entry, edit, MDKR_CHARACTER_CONTEXT_SELECT) ? 1u : 0u;
    for (unsigned context = MDKR_CHARACTER_CONTEXT_CAR;
         context < MDKR_CHARACTER_CONTEXT_COUNT; ++context) {
        if ((edit.vehicleMask & (1u << (context - 1u))) == 0u) continue;
        ++reviewContexts;
        if (characterFitReviewed(entry, edit, context)) ++reviewedContexts;
    }
    ImGui::TextColored(
        reviewedContexts == reviewContexts
            ? AppTheme::good() : AppTheme::accent(),
        "Fit review: %u of %u enabled contexts current",
        reviewedContexts, reviewContexts);
    if (ImGui::BeginTabBar("##character-placement-contexts")) {
        for (unsigned context = 0u;
             context < MDKR_CHARACTER_CONTEXT_COUNT; ++context) {
            const bool packageContext = context == MDKR_CHARACTER_CONTEXT_SELECT ||
                (entry->vehicle_mask & (1u << (context - 1u))) != 0u;
            if (!packageContext) continue;
            if (!ImGui::BeginTabItem(contextNames[context])) continue;
            CharacterTuningEdit::Context &placement = edit.context[context];
            ImGui::TextDisabled(
                "%s anchor → qualified %s %s frame",
                context == MDKR_CHARACTER_CONTEXT_SELECT ? "Ground" : "Pelvis/seat",
                donorName(entry->donor),
                context == MDKR_CHARACTER_CONTEXT_SELECT ? "select" :
                    contextNames[context]);
            ImGui::PushID(static_cast<int>(context));
            (void)ImGui::SliderFloat("Context size", &placement.scale,
                                     0.5f, 2.0f, "%.2fx",
                                     ImGuiSliderFlags_AlwaysClamp);
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                changed |= persistCharacterTuning(entry->id, edit);
            }
            (void)ImGui::DragFloat3("Position", placement.offset, 0.01f,
                                    -10.0f, 10.0f, "%.3f m",
                                    ImGuiSliderFlags_AlwaysClamp);
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                changed |= persistCharacterTuning(entry->id, edit);
            }
            (void)ImGui::DragFloat3("Rotation", placement.rotation, 0.5f,
                                    -180.0f, 180.0f, "%.1f deg",
                                    ImGuiSliderFlags_AlwaysClamp);
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                changed |= persistCharacterTuning(entry->id, edit);
            }
            if (ImGui::Button(context == MDKR_CHARACTER_CONTEXT_SELECT
                                  ? "Place feet on ground"
                                  : "Align pelvis to seat")) {
                placement.scale = 1.0f;
                placement.offset[0] = placement.offset[1] =
                    placement.offset[2] = 0.0f;
                placement.rotation[0] = placement.rotation[1] =
                    placement.rotation[2] = 0.0f;
                changed |= persistCharacterTuning(entry->id, edit);
            }
            ImGui::SameLine();
            ImGui::TextDisabled("automatic anchor reset");
            if (context != MDKR_CHARACTER_CONTEXT_SELECT) {
                if (!contactReady) ImGui::BeginDisabled();
                if (ImGui::TreeNode("Hand and foot contacts")) {
                    static const char *contactLabels[
                        MDKR_MODERN_CHARACTER_CONTACTS] = {
                            "Left hand", "Right hand", "Left foot", "Right foot"
                        };
                    ui::TextSubtleWrapped(
                        "Fine-tune the engine-owned contact targets in metres relative to the mapped hips at the seat frame. These affect missing-semantic reference motion only; explicit authored clips remain untouched.");
                    for (unsigned contact = 0u;
                         contact < MDKR_MODERN_CHARACTER_CONTACTS; ++contact) {
                        ImGui::PushID(static_cast<int>(contact));
                        (void)ImGui::DragFloat3(
                            contactLabels[contact],
                            placement.contacts[contact], 0.005f,
                            -1.0f, 1.0f, "%.3f m",
                            ImGuiSliderFlags_AlwaysClamp);
                        if (ImGui::IsItemDeactivatedAfterEdit()) {
                            changed |= persistCharacterTuning(entry->id, edit);
                        }
                        ImGui::PopID();
                    }
                    if (ImGui::Button("Reset contact targets")) {
                        std::memset(placement.contacts, 0,
                                    sizeof(placement.contacts));
                        changed |= persistCharacterTuning(entry->id, edit);
                    }
                    ImGui::TreePop();
                }
                if (!contactReady) {
                    ImGui::EndDisabled();
                    ui::TextSubtleWrapped(
                        "Contact controls require a complete, reviewed source-v4 humanoid map.");
                }
            }
            const MdkrCharacterPreviewContext previewContext =
                previewContexts[context];
            const auto result = g_characterPreviewResults.find(entry->id);
            const bool currentResult =
                result != g_characterPreviewResults.end() &&
                result->second.version ==
                    MDKR_CHARACTER_PREVIEW_RESULT_VERSION &&
                result->second.started &&
                result->second.context == previewContext &&
                result->second.warmup_complete &&
                result->second.replacement_draws != 0u;
            if (currentResult &&
                context != MDKR_CHARACTER_CONTEXT_SELECT) {
                if (result->second.contact_solves != 0u) {
                    ImGui::Text(
                        "Last exact test: %.2f mm mean · %.2f mm maximum across %llu solves",
                        result->second.contact_error_mean_micrometres / 1000.0,
                        result->second.contact_error_max_micrometres / 1000.0,
                        result->second.contact_solves);
                } else {
                    ImGui::TextDisabled(
                        "Last exact test: no procedural contacts (authored clip or solver locked)");
                }
            }
            const bool fitReviewed = characterFitReviewed(
                entry, edit, context);
            ImGui::TextColored(
                fitReviewed ? AppTheme::good() : AppTheme::accent(),
                fitReviewed
                    ? "Fit reviewed for this exact source and tuning"
                    : "Fit review required for current source or tuning");
            if (!fitReviewed) {
                if (!currentResult) ImGui::BeginDisabled();
                const std::string reviewLabel = std::string("Mark ") +
                    contextNames[context] + " fit reviewed";
                if (ImGui::Button(reviewLabel.c_str()) && currentResult) {
                    changed |= persistCharacterFitReview(
                        entry, edit, context);
                }
                if (!currentResult) ImGui::EndDisabled();
                ui::SpeakFocusedItem(
                    reviewLabel.c_str(),
                    currentResult
                        ? nullptr
                        : "Run and complete the matching exact renderer test first.",
                    "Saves review only for the current package source and fit values.");
            } else {
                const std::string reopenLabel = std::string("Reopen ") +
                    contextNames[context] + " fit review";
                if (ImGui::Button(reopenLabel.c_str())) {
                    changed |= clearCharacterFitReview(entry, context);
                }
                ui::SpeakFocusedItem(
                    reopenLabel.c_str(), nullptr,
                    "Clears this context's approval without changing its fit values.");
            }
            if (!compact) {
                int &testPlayers = g_characterTestPlayers[entry->id];
                if (testPlayers < 1 || testPlayers > 4) testPlayers = 1;
                const bool enabled =
                    context == MDKR_CHARACTER_CONTEXT_SELECT ||
                    (edit.vehicleMask & (1u << (context - 1u))) != 0u;
                if (!enabled) ImGui::BeginDisabled();
                const std::string testLabel = std::string("Test ") +
                    contextNames[context] + " fit in exact renderer";
                if (ImGui::Button(testLabel.c_str()) && enabled &&
                    persistCharacterTuning(entry->id, edit)) {
                    requestCharacterPreview(entry, previewContext, testPlayers);
                }
                if (!enabled) ImGui::EndDisabled();
                ui::SpeakFocusedItem(
                    testLabel.c_str(),
                    enabled ? nullptr
                            : "Enable this vehicle for the package first.",
                    "Saves the current fit and opens the real game context for review.");
            }
            ImGui::PopID();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    if (ImGui::TreeNode("Advanced whole-character correction")) {
        (void)ImGui::DragFloat3("Overall position", edit.offset, 0.01f,
                                -500.0f, 500.0f, "%.3f",
                                ImGuiSliderFlags_AlwaysClamp);
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            changed |= persistCharacterTuning(entry->id, edit);
        }
        (void)ImGui::DragFloat3("Overall rotation", edit.rotation, 0.5f,
                                -180.0f, 180.0f, "%.1f deg",
                                ImGuiSliderFlags_AlwaysClamp);
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            changed |= persistCharacterTuning(entry->id, edit);
        }
        ImGui::TreePop();
    }

    ImGui::SeparatorText("Motion and detail");
    (void)ImGui::SliderFloat("Animation speed", &edit.animationSpeed,
                             0.05f, 4.0f, "%.2fx",
                             ImGuiSliderFlags_AlwaysClamp);
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        changed |= persistCharacterTuning(entry->id, edit);
    }
    const bool hasMultipleLods = entry->stats.lod_levels > 1u;
    if (!hasMultipleLods) ImGui::BeginDisabled();
    (void)ImGui::SliderFloat("LOD preference", &edit.lodBias,
                             -3.0f, 3.0f, "%+.0f",
                             ImGuiSliderFlags_AlwaysClamp);
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        changed |= persistCharacterTuning(entry->id, edit);
    }
    if (!hasMultipleLods) ImGui::EndDisabled();
    if (hasMultipleLods) {
        if (ImGui::Button("Performance")) {
            edit.lodBias = -2.0f;
            changed |= persistCharacterTuning(entry->id, edit);
        }
        ImGui::SameLine();
        if (ImGui::Button("Balanced")) {
            edit.lodBias = 0.0f;
            changed |= persistCharacterTuning(entry->id, edit);
        }
        ImGui::SameLine();
        if (ImGui::Button("Quality")) {
            edit.lodBias = 2.0f;
            changed |= persistCharacterTuning(entry->id, edit);
        }
        ui::TextSubtleWrapped(
            "These presets choose among authored LODs. They do not change physics or manufacture missing detail.");
    } else {
        ui::TextSubtleWrapped(
            "One authored LOD: runtime quality controls cannot reduce this model's geometry. Re-export with LODs or build an offline simplified assembly.");
    }
    if (ImGui::Button("Reset fit and motion")) {
        edit = CharacterTuningEdit{};
        edit.loaded = true;
        edit.vehicleMask = entry->vehicle_mask;
        changed |= persistCharacterTuning(entry->id, edit);
    }
    return changed;
}

const char *characterPerformanceTier(
    const MdkrModernCharacterEntry *entry) {
    const MdkrModernCharacterStats &stats = entry->stats;
    const uint32_t triangles = entry->lod_triangles[0] != 0u
        ? entry->lod_triangles[0] : stats.triangles;
    const uint32_t vertices = entry->lod_vertices[0] != 0u
        ? entry->lod_vertices[0] : stats.vertices;
    const uint32_t primitives = entry->lod_primitives[0] != 0u
        ? entry->lod_primitives[0] : stats.primitives;
    if (stats.textures != 0u && stats.decoded_texture_bytes == 0u) {
        return "Recompile to measure";
    }
    if (triangles <= 15000u && vertices <= 20000u &&
        primitives <= 4u && stats.materials <= 4u &&
        stats.joints <= 64u && stats.textures <= 8u &&
        stats.decoded_texture_bytes <= 64u * 1024u * 1024u) return "Excellent";
    if (triangles <= 30000u && vertices <= 40000u &&
        primitives <= 8u && stats.materials <= 8u &&
        stats.joints <= 96u && stats.textures <= 12u &&
        stats.decoded_texture_bytes <= 128u * 1024u * 1024u) return "Good";
    if (triangles <= 60000u && vertices <= 70000u &&
        primitives <= 12u && stats.materials <= 12u &&
        stats.joints <= 128u && stats.textures <= 16u &&
        stats.decoded_texture_bytes <= 256u * 1024u * 1024u) return "Heavy";
    return "Very heavy";
}

void drawCharacterPerformanceAssembly(
    const MdkrModernCharacterEntry *entry) {
    int &players = g_characterAssemblyPlayers[entry->id];
    if (players < 1 || players > 4) players = 4;
    ui::TextSubtleWrapped(
        "Inspect the selected package repeated across local players. The worst-visible case assumes every custom racer is visible in every split-screen viewport at LOD0. Immutable mesh and texture uploads remain shared once for this package.");
    ImGui::TextUnformatted("Local-player assembly");
    for (int option : {1, 2, 3, 4}) {
        if (option != 1) ImGui::SameLine();
        const std::string label = std::to_string(option) +
            (option == 1 ? " player" : " players");
        (void)ImGui::RadioButton(label.c_str(), &players, option);
    }
    const uint64_t visibleInstances =
        static_cast<uint64_t>(players) * static_cast<uint64_t>(players);
    const uint64_t triangles =
        static_cast<uint64_t>(entry->lod_triangles[0]) * visibleInstances;
    const uint64_t vertices =
        static_cast<uint64_t>(entry->lod_vertices[0]) * visibleInstances;
    const uint64_t draws =
        static_cast<uint64_t>(entry->lod_primitives[0]) * visibleInstances;
    const uint64_t paletteMatrices =
        static_cast<uint64_t>(entry->lod_palette_matrices[0]) *
        visibleInstances * 2u;
    const uint64_t geometryBytes =
        static_cast<uint64_t>(entry->stats.vertices) *
            sizeof(MdkrModernVertex) +
        static_cast<uint64_t>(entry->stats.triangles) * 3u * sizeof(uint32_t);
    if (ImGui::BeginTable(
            "##character-performance-assembly", 2,
            ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp)) {
        auto countRow = [](const char *label, uint64_t value) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(label);
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%llu", static_cast<unsigned long long>(value));
        };
        auto memoryRow = [](const char *label, uint64_t bytes) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(label);
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%.2f MiB", static_cast<double>(bytes) /
                (1024.0 * 1024.0));
        };
        countRow("Worst-visible character instances", visibleInstances);
        countRow("LOD0 triangles submitted", triangles);
        countRow("LOD0 vertices referenced", vertices);
        countRow("Character draw submissions", draws);
        countRow("Current + previous bone matrices prepared", paletteMatrices);
        memoryRow("Shared geometry upload", geometryBytes);
        memoryRow("Shared decoded texture upload",
                  entry->stats.decoded_texture_bytes);
        ImGui::EndTable();
    }
    ImGui::TextDisabled(
        "LOD0 authoring guide: %s · timing still requires the exact-context stress test",
        characterPerformanceTier(entry));
    for (uint32_t lod = 0u;
         lod < entry->stats.lod_levels &&
         lod < MDKR_MODERN_CHARACTER_LOD_LEVELS; ++lod) {
        ImGui::BulletText(
            "LOD%u: %u triangles · %u vertices · %u draw part(s)",
            lod, entry->lod_triangles[lod], entry->lod_vertices[lod],
            entry->lod_primitives[lod]);
    }
    ui::TextSubtleWrapped(
        "These are exact structural counts, not a frame-time prediction. Materials, transparency, overdraw, skinning, driver visibility, camera framing, GPU, resolution, and other racers still affect measured performance; the Workshop must not turn a budget guide into an artificial import ceiling.");
}

void requestCharacterPreview(const MdkrModernCharacterEntry *entry,
                             MdkrCharacterPreviewContext context,
                             int players) {
    g_characterPreviewRequest.packageId = entry->id;
    g_characterPreviewRequest.context = context;
    g_characterPreviewRequest.players = players;
    g_characterPreviewRequested = true;
    setStatus("Checking the selected ROM, then opening the exact game context.",
              AppTheme::good());
}

const char *characterPreviewResultContext(
    MdkrCharacterPreviewContext context) {
    switch (context) {
        case MDKR_CHARACTER_PREVIEW_SELECT: return "Character select";
        case MDKR_CHARACTER_PREVIEW_CAR: return "Car";
        case MDKR_CHARACTER_PREVIEW_HOVERCRAFT: return "Hovercraft";
        case MDKR_CHARACTER_PREVIEW_PLANE: return "Plane";
        default: return "Unknown context";
    }
}

void drawCharacterPreviewResult(const MdkrModernCharacterEntry *entry) {
    const auto found = g_characterPreviewResults.find(entry->id);
    if (found == g_characterPreviewResults.end()) return;
    const MdkrCharacterPreviewResult &result = found->second;
    ui::Gap(ui::kGapS);
    if (!ui::CardBegin("##character-preview-result", AppTheme::surface(),
                       0.0f)) {
        ui::CardEnd();
        return;
    }
    ImGui::TextUnformatted("Last exact-context result");
    if (result.version != MDKR_CHARACTER_PREVIEW_RESULT_VERSION ||
        !result.started) {
        ImGui::PushStyleColor(ImGuiCol_Text, AppTheme::bad());
        ImGui::TextUnformatted("The test did not reach the selected context.");
        ImGui::PopStyleColor();
        ui::TextSubtleWrapped(
            "No performance conclusion was saved. Review Diagnostics and run the test again.");
        ui::CardEnd();
        return;
    }
    ImGui::Text("%s  •  %d %s",
                characterPreviewResultContext(result.context), result.players,
                result.players == 1 ? "player" : "players");
    if (!result.warmup_complete) {
        ImGui::PushStyleColor(ImGuiCol_Text, AppTheme::accent());
        ImGui::TextUnformatted("Ended during the 120-tick warm-up");
        ImGui::PopStyleColor();
        ui::TextSubtleWrapped(
            "Keep the exact context open beyond the warm-up (about four seconds on NTSC) before pressing F1. Loading and shader creation are deliberately excluded from steady-state timing.");
        ui::CardEnd();
        return;
    }
    const bool enoughSamples = result.interval_samples >= 60u;
    const bool qualified = result.realtime && enoughSamples;
    ImGui::PushStyleColor(
        ImGuiCol_Text, qualified ? AppTheme::good() : AppTheme::accent());
    ImGui::TextUnformatted(
        qualified ? "Steady-state sample captured"
                  : (!result.realtime ? "Synthetic pacing — diagnostic only"
                                      : "Short sample — diagnostic only"));
    ImGui::PopStyleColor();
    if (ImGui::BeginTable("##character-preview-measurement", 2,
                          ImGuiTableFlags_SizingStretchProp |
                          ImGuiTableFlags_RowBg)) {
        const auto metric = [](const char *name, const char *value) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ui::TextSubtle("%s", name);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(value);
        };
        char value[96];
        std::snprintf(value, sizeof(value), "%llu intervals / %llu frames",
                      result.interval_samples, result.displayed_frames);
        metric("Displayed sample", value);
        std::snprintf(value, sizeof(value), "%.2f ms  •  %.1f fps",
                      result.interval_p50_us / 1000.0,
                      result.interval_p50_us != 0u
                          ? 1000000.0 / result.interval_p50_us : 0.0);
        metric("Median cadence", value);
        std::snprintf(value, sizeof(value), "%.2f / %.2f ms",
                      result.interval_p95_us / 1000.0,
                      result.interval_p99_us / 1000.0);
        metric("95th / 99th percentile", value);
        std::snprintf(value, sizeof(value), "%.2f ms",
                      result.interval_max_us / 1000.0);
        metric("Worst displayed interval", value);
        std::snprintf(value, sizeof(value), "%llu / %llu",
                      result.replacement_draws,
                      result.replacement_primitives);
        metric("Character replacements / parts", value);
        if (result.context != MDKR_CHARACTER_PREVIEW_SELECT) {
            if (result.contact_solves != 0u) {
                std::snprintf(value, sizeof(value), "%llu",
                              result.contact_solves);
                metric("Vehicle contact solves", value);
                std::snprintf(
                    value, sizeof(value), "%.2f / %.2f mm",
                    result.contact_error_mean_micrometres / 1000.0,
                    result.contact_error_max_micrometres / 1000.0);
                metric("Mean / maximum contact error", value);
            } else {
                metric("Vehicle contact solves",
                       "None — authored clip or solver locked");
            }
        }
        std::snprintf(value, sizeof(value), "%.2f ms across %llu ticks",
                      result.tickwall_mean_ns / 1000000.0,
                      result.tickwall_samples);
        metric("Authored tick wall mean", value);
        ImGui::EndTable();
    }
    if (!enoughSamples) {
        ui::TextSubtleWrapped(
            "Collect at least 60 displayed intervals after warm-up before comparing runs.");
    }
    ui::TextSubtleWrapped(
        "Measured wall cadence includes the selected presentation policy, renderer, scene, resolution, other racers and this device. It is not a GPU timestamp, spare GPU headroom, or a character-only cost; compare the same context and settings, and use the four-player route for worst-visible stress.");
    if (result.context != MDKR_CHARACTER_PREVIEW_SELECT) {
        ui::TextSubtleWrapped(
            "Contact error is the physical distance from each solved hand/foot endpoint to its tuned target. It is evidence for fit review, not an import ceiling: body proportions and intentionally unreachable targets can make a valid character report a larger value.");
    }
    ui::CardEnd();
}

void drawCharacterExactTests(const MdkrModernCharacterEntry *entry,
                             bool compact) {
    if (compact) {
        ui::TextSubtleWrapped(
            "Return to the launcher Workshop to start an exact game-context test. A running engine cannot safely start a second engine inside itself.");
        return;
    }
    int &players = g_characterTestPlayers[entry->id];
    const CharacterTuningEdit &tuning = loadCharacterTuning(0, entry->id);
    if (players < 1 || players > 4) players = 1;
    ui::TextSubtleWrapped(
        "Launch this package directly into the real game renderer with its saved fit. The test is temporary: it does not replace Player assignments or skip the final ROM integrity check. For a useful timing sample, stay at least three seconds beyond the 120-tick warm-up; opening F1 freezes the sample before you navigate back.");
    drawCharacterPreviewResult(entry);
    ImGui::TextUnformatted("Test layout");
    for (int option : {1, 2, 3, 4}) {
        if (option != 1) ImGui::SameLine();
        const std::string label = std::to_string(option) +
            (option == 1 ? " player##character-test-" :
                           " players##character-test-") +
            std::to_string(option);
        (void)ImGui::RadioButton(label.c_str(), &players, option);
    }
    if (ImGui::Button("Character select")) {
        requestCharacterPreview(entry, MDKR_CHARACTER_PREVIEW_SELECT,
                                players);
    }
    ui::SpeakFocusedItem(
        "Character select", nullptr,
        "Tests the selected package in the exact character select scene.");
    ImGui::SameLine();
    const bool carQualified = (tuning.vehicleMask & 1u) != 0u;
    if (!carQualified) ImGui::BeginDisabled();
    if (ImGui::Button("Car") && carQualified) {
        requestCharacterPreview(entry, MDKR_CHARACTER_PREVIEW_CAR,
                                players);
    }
    if (!carQualified) ImGui::EndDisabled();
    ui::SpeakFocusedItem("Car",
                         carQualified ? nullptr : "Disabled for this package.",
                         "Tests the selected package in a real car race.");
    ImGui::SameLine();
    const bool hoverQualified = (tuning.vehicleMask & 2u) != 0u;
    if (!hoverQualified) ImGui::BeginDisabled();
    if (ImGui::Button("Hovercraft") && hoverQualified) {
        requestCharacterPreview(
            entry, MDKR_CHARACTER_PREVIEW_HOVERCRAFT, players);
    }
    if (!hoverQualified) ImGui::EndDisabled();
    ui::SpeakFocusedItem(
        "Hovercraft", hoverQualified ? nullptr : "Not supported by this package.",
        "Tests the selected package in a real hovercraft race.");
    ImGui::SameLine();
    const bool planeQualified = (tuning.vehicleMask & 4u) != 0u;
    if (!planeQualified) ImGui::BeginDisabled();
    if (ImGui::Button("Plane") && planeQualified) {
        requestCharacterPreview(entry, MDKR_CHARACTER_PREVIEW_PLANE,
                                players);
    }
    if (!planeQualified) ImGui::EndDisabled();
    ui::SpeakFocusedItem(
        "Plane", planeQualified ? nullptr : "Not supported by this package.",
        "Tests the selected package in a real plane race.");
}

void drawCharacterPortraitPreview(const MdkrModernCharacterEntry *entry) {
    const float pixelSize = std::max(
        2.0f, std::floor(ImGui::GetFontSize() * 0.2f + 0.5f));
    const float extent = pixelSize * MDKR_MODERN_PORTRAIT_SIZE;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImDrawList *draw = ImGui::GetWindowDrawList();
    const unsigned checkerPixels = 4u;
    unsigned y;
    for (y = 0u; y < MDKR_MODERN_PORTRAIT_SIZE; y += checkerPixels) {
        unsigned x;
        for (x = 0u; x < MDKR_MODERN_PORTRAIT_SIZE; x += checkerPixels) {
            const ImU32 colour = ((x / checkerPixels + y / checkerPixels) & 1u)
                ? IM_COL32(73, 79, 89, 255)
                : IM_COL32(48, 53, 62, 255);
            draw->AddRectFilled(
                ImVec2(origin.x + x * pixelSize,
                       origin.y + y * pixelSize),
                ImVec2(origin.x +
                           std::min(x + checkerPixels,
                                    MDKR_MODERN_PORTRAIT_SIZE) * pixelSize,
                       origin.y +
                           std::min(y + checkerPixels,
                                    MDKR_MODERN_PORTRAIT_SIZE) * pixelSize),
                colour);
        }
    }
    /* Coalesce identical horizontal pixels. A 40x40 portrait remains crisp at
     * every UI scale without creating a renderer-owned texture lifecycle. */
    for (y = 0u; y < MDKR_MODERN_PORTRAIT_SIZE; ++y) {
        unsigned x = 0u;
        while (x < MDKR_MODERN_PORTRAIT_SIZE) {
            const uint8_t *pixel = entry->portrait_rgba +
                (y * MDKR_MODERN_PORTRAIT_SIZE + x) * 4u;
            unsigned end = x + 1u;
            if (pixel[3] == 0u) {
                x = end;
                continue;
            }
            while (end < MDKR_MODERN_PORTRAIT_SIZE &&
                   std::memcmp(pixel, entry->portrait_rgba +
                       (y * MDKR_MODERN_PORTRAIT_SIZE + end) * 4u, 4u) == 0) {
                ++end;
            }
            draw->AddRectFilled(
                ImVec2(origin.x + x * pixelSize,
                       origin.y + y * pixelSize),
                ImVec2(origin.x + end * pixelSize,
                       origin.y + (y + 1u) * pixelSize),
                IM_COL32(pixel[0], pixel[1], pixel[2], pixel[3]));
            x = end;
        }
    }
    draw->AddRect(origin, ImVec2(origin.x + extent, origin.y + extent),
                  IM_COL32(255, 255, 255, 115), 0.0f, 0, 1.0f);
    ImGui::Dummy(ImVec2(extent, extent));
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "%s\nExact 40 × 40 in-game portrait preview",
            entry->display_name);
    }
}

bool drawCharacterProfileStudio(const MdkrModernCharacterEntry *entry) {
    static const char *vehicleNames[] = {"Car", "Hovercraft", "Plane"};
    CharacterProfileEdit &edit = g_characterProfileEdits[entry->id];
    if (!edit.loaded ||
        std::memcmp(edit.sourceSha256, entry->source_sha256,
                    sizeof(edit.sourceSha256)) != 0) {
        edit.donor = entry->donor;
        edit.vehicleMask = entry->vehicle_mask;
        std::memcpy(edit.sourceSha256, entry->source_sha256,
                    sizeof(edit.sourceSha256));
        edit.loaded = true;
    }
    ui::TextSubtleWrapped(
        "Choose which built-in racer supplies gameplay and which vehicle scenes this appearance supports. This never copies or edits stats: handling, weight, acceleration, hitbox, voice, horn, records, ghosts, saves, and ordinary online authority remain the selected built-in profile's own data.");
    ImGui::SetNextItemWidth(std::min(360.0f, ImGui::GetContentRegionAvail().x));
    if (ImGui::BeginCombo("Built-in gameplay profile", donorName(edit.donor))) {
        for (uint32_t donor = 0u; donor < 10u; ++donor) {
            const bool selected = edit.donor == donor;
            if (ImGui::Selectable(donorName(donor), selected)) {
                edit.donor = donor;
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    ImGui::TextColored(
        AppTheme::good(),
        "%s: fingerprint-qualified select, car, hovercraft, and plane seams",
        donorName(edit.donor));
    ImGui::TextUnformatted("Package compatibility");
    for (unsigned vehicle = 0u; vehicle < 3u; ++vehicle) {
        if (vehicle != 0u) ImGui::SameLine();
        const uint32_t bit = 1u << vehicle;
        bool enabled = (edit.vehicleMask & bit) != 0u;
        const std::string label = std::string(vehicleNames[vehicle]) +
            "##package-vehicle-" + std::to_string(vehicle);
        if (ImGui::Checkbox(label.c_str(), &enabled)) {
            const uint32_t candidate = enabled
                ? edit.vehicleMask | bit : edit.vehicleMask & ~bit;
            if (candidate != 0u) {
                edit.vehicleMask = candidate;
            } else {
                setStatus(
                    "A character package must support at least one vehicle.",
                    AppTheme::bad());
            }
        }
    }
    ui::TextSubtleWrapped(
        "Adding a vehicle creates a neutral seat-anchored context that you can tune independently below. Removing one preserves its authored context in revision history, so it can be restored later.");
    ImGui::TextDisabled(
        "In character select, choose %s to use this appearance.",
        donorName(edit.donor));
    const bool dirty = edit.donor != entry->donor ||
        edit.vehicleMask != entry->vehicle_mask;
    if (!dirty) ImGui::BeginDisabled();
    bool saved = false;
    if (ImGui::Button("Save gameplay and compatibility revision")) {
        const std::string packageId = entry->id;
        saved = reviseCharacterProfile(
            packageId.c_str(), edit.donor, edit.vehicleMask);
        if (saved) {
            CharacterTuningEdit &tuning = loadCharacterTuning(
                0, packageId.c_str());
            tuning.vehicleMask = edit.vehicleMask;
            const bool tuningSaved = persistCharacterTuning(
                packageId.c_str(), tuning);
            setStatus(tuningSaved
                    ? "Built-in gameplay profile and vehicle compatibility activated."
                    : "Package profile activated, but its local vehicle enablement could not be saved.",
                tuningSaved ? AppTheme::good() : AppTheme::accent());
        } else {
            setStatus(
                "Profile revision failed; the active character was not changed.",
                AppTheme::bad());
        }
    }
    if (!dirty) ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled(dirty ? "unsaved source revision" : "saved in package");
    return saved;
}

void portraitPushUndo(CharacterIdentityEdit &edit) {
    if (edit.undo.size() == 32u) edit.undo.erase(edit.undo.begin());
    edit.undo.push_back(edit.canvas);
    edit.redo.clear();
}

void portraitSetPixel(CharacterIdentityEdit &edit, int x, int y,
                      bool erase) {
    if (x < 0 || x >= static_cast<int>(MDKR_MODERN_PORTRAIT_SIZE) ||
        y < 0 || y >= static_cast<int>(MDKR_MODERN_PORTRAIT_SIZE)) return;
    uint8_t *pixel = edit.canvas.data() +
        (y * MDKR_MODERN_PORTRAIT_SIZE + x) * 4u;
    for (unsigned component = 0u; component < 4u; ++component) {
        pixel[component] = erase ? 0u : static_cast<uint8_t>(std::lround(
            std::clamp(edit.paintRgba[component], 0.0f, 1.0f) * 255.0f));
    }
    edit.canvasDirty = true;
}

void portraitPickPixel(CharacterIdentityEdit &edit, int x, int y) {
    const uint8_t *pixel = edit.canvas.data() +
        (y * MDKR_MODERN_PORTRAIT_SIZE + x) * 4u;
    for (unsigned component = 0u; component < 4u; ++component) {
        edit.paintRgba[component] = static_cast<float>(pixel[component]) / 255.0f;
    }
}

void portraitFill(CharacterIdentityEdit &edit, int startX, int startY) {
    constexpr int size = MDKR_MODERN_PORTRAIT_SIZE;
    const int start = startY * size + startX;
    const std::array<uint8_t, 4> target = {
        edit.canvas[start * 4], edit.canvas[start * 4 + 1],
        edit.canvas[start * 4 + 2], edit.canvas[start * 4 + 3],
    };
    std::array<uint8_t, 4> replacement{};
    for (unsigned component = 0u; component < 4u; ++component) {
        replacement[component] = static_cast<uint8_t>(std::lround(
            std::clamp(edit.paintRgba[component], 0.0f, 1.0f) * 255.0f));
    }
    if (target == replacement) return;
    portraitPushUndo(edit);
    std::array<int, size * size> queue{};
    int read = 0;
    int write = 0;
    std::memcpy(edit.canvas.data() + start * 4, replacement.data(), 4u);
    queue[write++] = start;
    while (read < write) {
        const int index = queue[read++];
        const int x = index % size;
        const int y = index / size;
        const int neighbours[4] = {
            x > 0 ? index - 1 : -1,
            x + 1 < size ? index + 1 : -1,
            y > 0 ? index - size : -1,
            y + 1 < size ? index + size : -1,
        };
        for (int neighbour : neighbours) {
            if (neighbour >= 0 &&
                std::memcmp(edit.canvas.data() + neighbour * 4,
                            target.data(), 4u) == 0) {
                std::memcpy(edit.canvas.data() + neighbour * 4,
                            replacement.data(), 4u);
                queue[write++] = neighbour;
            }
        }
    }
    edit.canvasDirty = true;
}

bool drawPortraitPixelEditor(const MdkrModernCharacterEntry *entry,
                             CharacterIdentityEdit &edit) {
    constexpr int size = MDKR_MODERN_PORTRAIT_SIZE;
    static const char *tools[] = {"Pencil", "Eraser", "Fill", "Eyedropper"};
    for (int tool = 0; tool < static_cast<int>(std::size(tools)); ++tool) {
        if (tool != 0) ImGui::SameLine();
        (void)ImGui::RadioButton(tools[tool], &edit.tool, tool);
    }
    ImGui::SetNextItemWidth(std::min(360.0f, ImGui::GetContentRegionAvail().x));
    (void)ImGui::ColorEdit4(
        "Paint colour", edit.paintRgba,
        ImGuiColorEditFlags_DisplayRGB | ImGuiColorEditFlags_DisplayHex |
            ImGuiColorEditFlags_InputRGB | ImGuiColorEditFlags_AlphaBar);
    const float pixelSize = std::clamp(
        std::floor(ImGui::GetContentRegionAvail().x / size), 3.0f, 8.0f);
    const float extent = pixelSize * size;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImDrawList *draw = ImGui::GetWindowDrawList();
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const uint8_t *pixel = edit.canvas.data() + (y * size + x) * 4;
            const ImU32 background = ((x / 4 + y / 4) & 1)
                ? IM_COL32(73, 79, 89, 255) : IM_COL32(48, 53, 62, 255);
            const ImVec2 minimum(origin.x + x * pixelSize,
                                 origin.y + y * pixelSize);
            const ImVec2 maximum(minimum.x + pixelSize,
                                 minimum.y + pixelSize);
            draw->AddRectFilled(minimum, maximum, background);
            if (pixel[3] != 0u) {
                draw->AddRectFilled(
                    minimum, maximum,
                    IM_COL32(pixel[0], pixel[1], pixel[2], pixel[3]));
            }
        }
    }
    draw->AddRect(origin, ImVec2(origin.x + extent, origin.y + extent),
                  IM_COL32(255, 255, 255, 140));
    ImGui::InvisibleButton("##portrait-pixel-canvas", ImVec2(extent, extent));
    const bool hovered = ImGui::IsItemHovered();
    if (hovered) {
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        const int x = std::clamp(
            static_cast<int>((mouse.x - origin.x) / pixelSize), 0, size - 1);
        const int y = std::clamp(
            static_cast<int>((mouse.y - origin.y) / pixelSize), 0, size - 1);
        edit.selectedPixel[0] = x;
        edit.selectedPixel[1] = y;
        draw->AddRect(
            ImVec2(origin.x + x * pixelSize, origin.y + y * pixelSize),
            ImVec2(origin.x + (x + 1) * pixelSize,
                   origin.y + (y + 1) * pixelSize),
            IM_COL32(255, 255, 255, 255), 0.0f, 0, 2.0f);
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            if (edit.tool == 2) {
                portraitFill(edit, x, y);
            } else if (edit.tool == 3) {
                portraitPickPixel(edit, x, y);
            } else {
                portraitPushUndo(edit);
                edit.strokeActive = true;
                portraitSetPixel(edit, x, y, edit.tool == 1);
            }
        } else if (edit.strokeActive &&
                   ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            portraitSetPixel(edit, x, y, edit.tool == 1);
        }
    }
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        edit.strokeActive = false;
    }
    ImGui::TextDisabled("Selected pixel: %d, %d", edit.selectedPixel[0],
                        edit.selectedPixel[1]);
    ImGui::SetNextItemWidth(std::min(220.0f, ImGui::GetContentRegionAvail().x));
    (void)ImGui::InputInt2("Pixel coordinates", edit.selectedPixel);
    edit.selectedPixel[0] = std::clamp(edit.selectedPixel[0], 0, size - 1);
    edit.selectedPixel[1] = std::clamp(edit.selectedPixel[1], 0, size - 1);
    if (ImGui::Button("Apply tool to selected pixel")) {
        if (edit.tool == 2) {
            portraitFill(edit, edit.selectedPixel[0], edit.selectedPixel[1]);
        } else if (edit.tool == 3) {
            portraitPickPixel(edit, edit.selectedPixel[0], edit.selectedPixel[1]);
        } else {
            portraitPushUndo(edit);
            portraitSetPixel(edit, edit.selectedPixel[0], edit.selectedPixel[1],
                             edit.tool == 1);
        }
    }
    const bool canUndo = !edit.undo.empty();
    if (!canUndo) ImGui::BeginDisabled();
    if (ImGui::Button("Undo")) {
        edit.redo.push_back(edit.canvas);
        edit.canvas = edit.undo.back();
        edit.undo.pop_back();
        edit.canvasDirty = true;
    }
    if (!canUndo) ImGui::EndDisabled();
    ImGui::SameLine();
    const bool canRedo = !edit.redo.empty();
    if (!canRedo) ImGui::BeginDisabled();
    if (ImGui::Button("Redo")) {
        if (edit.undo.size() == 32u) edit.undo.erase(edit.undo.begin());
        edit.undo.push_back(edit.canvas);
        edit.canvas = edit.redo.back();
        edit.redo.pop_back();
        edit.canvasDirty = true;
    }
    if (!canRedo) ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Mirror horizontally")) {
        portraitPushUndo(edit);
        for (int y = 0; y < size; ++y) {
            for (int x = 0; x < size / 2; ++x) {
                for (int component = 0; component < 4; ++component) {
                    std::swap(edit.canvas[(y * size + x) * 4 + component],
                              edit.canvas[(y * size + size - 1 - x) * 4 + component]);
                }
            }
        }
        edit.canvasDirty = true;
    }
    bool saved = false;
    const uint32_t authoredMinimap =
        static_cast<uint32_t>(std::lround(
            std::clamp(edit.minimapRgb[0], 0.0f, 1.0f) * 255.0f)) |
        static_cast<uint32_t>(std::lround(
            std::clamp(edit.minimapRgb[1], 0.0f, 1.0f) * 255.0f)) << 8u |
        static_cast<uint32_t>(std::lround(
            std::clamp(edit.minimapRgb[2], 0.0f, 1.0f) * 255.0f)) << 16u;
    const bool minimapDirty = (entry->identity_flags & 1u) != 0u &&
        authoredMinimap != (entry->minimap_rgba & 0xFFFFFFu);
    const bool canSaveCanvas = edit.canvasDirty || minimapDirty;
    if (!canSaveCanvas) ImGui::BeginDisabled();
    if (ImGui::Button("Save pixel canvas revision")) {
        saved = reviseCharacterIdentityRgba(
            entry->id, edit.canvas, edit.minimapRgb);
        setStatus(saved
                ? "Pixel portrait compiled and activated."
                : "Pixel portrait failed; the active character was not changed.",
            saved ? AppTheme::good() : AppTheme::bad());
    }
    if (!canSaveCanvas) ImGui::EndDisabled();
    return saved;
}

bool drawCharacterPortraitStudio(const MdkrModernCharacterEntry *entry) {
    static const uint8_t donorColours[][3] = {
        {194, 72, 58}, {54, 120, 197}, {66, 166, 110}, {76, 153, 190},
        {232, 145, 49}, {143, 91, 53}, {224, 93, 52}, {220, 80, 151},
        {150, 99, 198}, {237, 186, 48},
    };
    CharacterIdentityEdit &edit = g_characterIdentityEdits[entry->id];
    if (!edit.loaded ||
        std::memcmp(edit.sourceSha256, entry->source_sha256,
                    sizeof(edit.sourceSha256)) != 0) {
        const size_t donorIndex = entry->donor < std::size(donorColours)
            ? entry->donor : std::size(donorColours) - 1u;
        const uint32_t rgba = entry->identity_flags != 0u
            ? entry->minimap_rgba
            : (static_cast<uint32_t>(donorColours[donorIndex][0]) |
               static_cast<uint32_t>(donorColours[donorIndex][1]) << 8u |
               static_cast<uint32_t>(donorColours[donorIndex][2]) << 16u);
        edit.minimapRgb[0] = static_cast<float>(rgba & 0xFFu) / 255.0f;
        edit.minimapRgb[1] =
            static_cast<float>((rgba >> 8u) & 0xFFu) / 255.0f;
        edit.minimapRgb[2] =
            static_cast<float>((rgba >> 16u) & 0xFFu) / 255.0f;
        if ((entry->identity_flags & 1u) != 0u) {
            std::copy(std::begin(entry->portrait_rgba),
                      std::end(entry->portrait_rgba), edit.canvas.begin());
        } else {
            edit.canvas.fill(0u);
        }
        edit.undo.clear();
        edit.redo.clear();
        edit.canvasDirty = false;
        edit.strokeActive = false;
        std::memcpy(edit.sourceSha256, entry->source_sha256,
                    sizeof(edit.sourceSha256));
        edit.loaded = true;
    }
    ui::TextSubtleWrapped(
        "Choose square PNG artwork and a readable minimap colour. The importer validates the source, downsamples it once to the exact 40 × 40 game format, and atomically activates a new local package revision. The model, license, gameplay profile, and previous source revision are preserved.");
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint(
        "Portrait source PNG##character-portrait-path",
        "/path/to/square-portrait.png", edit.portraitPath,
        sizeof(edit.portraitPath));
    if (filedialog::isAvailable() && ImGui::Button("Browse for portrait...")) {
        std::string picked;
        if (filedialog::openPortraitImage(picked)) {
            std::snprintf(edit.portraitPath, sizeof(edit.portraitPath), "%s",
                          picked.c_str());
        }
    }
    ImGui::SetNextItemWidth(std::min(360.0f, ImGui::GetContentRegionAvail().x));
    (void)ImGui::ColorEdit3(
        "Minimap colour##character-minimap-colour", edit.minimapRgb,
        ImGuiColorEditFlags_DisplayRGB | ImGuiColorEditFlags_InputRGB |
            ImGuiColorEditFlags_PickerHueWheel);
    ui::TextSubtleWrapped(
        "PNG profile: 16–1024 px square, 8-bit RGB/RGBA, non-animated and non-interlaced. Transparency is preserved. The exact current in-game pixels and colour are shown in Overview above.");
    const bool canSave = edit.portraitPath[0] != '\0';
    if (!canSave) ImGui::BeginDisabled();
    bool saved = false;
    if (ImGui::Button("Save identity revision")) {
        saved = reviseCharacterIdentity(
            entry->id, edit.portraitPath, edit.minimapRgb);
        if (saved) {
            edit.portraitPath[0] = '\0';
            setStatus(
                "Portrait and minimap identity compiled and activated.",
                AppTheme::good());
        } else {
            setStatus(
                "Identity revision failed; the active character was not changed.",
                AppTheme::bad());
        }
    }
    if (!canSave) ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("non-destructive local revision");
    if (saved) return true;
    if (ImGui::TreeNodeEx(
            "Pixel editor##character-portrait-pixels",
            ImGuiTreeNodeFlags_DefaultOpen)) {
        ui::TextSubtleWrapped(
            "Edit the exact 40 × 40 runtime canvas. Pencil, eraser, fill, eyedropper, alpha, mirror, and bounded undo/redo are deterministic and stay local until you save a source revision.");
        const bool pixelSaved = drawPortraitPixelEditor(entry, edit);
        ImGui::TreePop();
        if (pixelSaved) return true;
    }
    return saved;
}

bool drawCharacterPackageInspector(const MdkrModernCharacterEntry *entry,
                                   bool compact) {
    bool changed = false;
    constexpr uint32_t raceStates =
        (MDKR_CHARACTER_SEMANTIC_SELECT_IDLE - 1u) &
        ~MDKR_CHARACTER_SEMANTIC_FALLBACK;
    constexpr uint32_t selectStates =
        MDKR_CHARACTER_SEMANTIC_SELECT_IDLE |
        MDKR_CHARACTER_SEMANTIC_SELECT_HOVER |
        MDKR_CHARACTER_SEMANTIC_SELECT_CONFIRM;
    const unsigned mappedRaceStates = countCharacterBits(
        entry->semantic_mask & raceStates);
    const unsigned mappedSelectStates = countCharacterBits(
        entry->semantic_mask & selectStates);
    const uint32_t requiredContexts =
        (1u << MDKR_CHARACTER_CONTEXT_SELECT) |
        ((entry->vehicle_mask & 7u) << 1u);
    const bool normalized = (entry->calibration_flags & 1u) != 0u;
    const bool anchored =
        (entry->attachment_context_mask & requiredContexts) ==
        requiredContexts;
    const bool attachmentSocketsMapped =
        (entry->socket_mask &
         (MDKR_CHARACTER_SOCKET_SEAT | MDKR_CHARACTER_SOCKET_HEAD)) ==
        (MDKR_CHARACTER_SOCKET_SEAT | MDKR_CHARACTER_SOCKET_HEAD);
    const bool humanoidRig = entry->rig_present != 0u &&
        entry->rig_mode == MDKR_MODERN_RIG_HUMANOID_RETARGET_V1;
    const bool humanoidRolesComplete = humanoidRig &&
        entry->rig_role_mask == MDKR_CHARACTER_RIG_HUMANOID_MASK;
    const bool rigReviewed =
        (entry->rig_flags & MDKR_MODERN_RIG_REVIEWED) != 0u;
    const bool referenceFallbackReady = humanoidRolesComplete && rigReviewed;
    const uint32_t requiredMotionStates = raceStates | selectStates;
    const uint32_t solverCoveredStates = referenceFallbackReady
        ? requiredMotionStates & ~entry->semantic_mask : 0u;
    const uint32_t effectiveMovingStates =
        (entry->moving_semantic_mask & requiredMotionStates) |
        solverCoveredStates;
    const uint32_t staticAuthoredStates =
        entry->semantic_mask & ~entry->moving_semantic_mask &
        requiredMotionStates;
    const char *rigStatus = entry->rig_present == 0u
        ? "not authored"
        : entry->rig_mode == MDKR_MODERN_RIG_AUTHORED_CLIPS_ONLY
            ? "authored clips only"
            : !humanoidRolesComplete
                ? "roles incomplete"
                : !rigReviewed ? "review required" : "reference/contact fallback ready";
    const bool motionReady =
        (effectiveMovingStates & requiredMotionStates) == requiredMotionStates;
    const bool qualified = mdkr_modern_donor_qualified(
        static_cast<int>(entry->donor)) != 0;
    const bool identityReady = (entry->identity_flags & 1u) != 0u &&
        entry->portrait_bytes != 0u;

    ImGui::PushID(entry->id);
    ImGui::SeparatorText("Overview");
    ImGui::TextUnformatted(entry->display_name);
    ImGui::TextDisabled(
        "Appearance package · %s gameplay profile · local presentation only",
        donorName(entry->donor));
    ui::TextSubtleWrapped(
        "The built-in profile still owns stats, handling, hitbox, voice, horn, records, ghosts, and ordinary online authority.");
    if (identityReady) {
        drawCharacterPortraitPreview(entry);
        ImGui::SameLine(0.0f, ui::kGapM);
        ImGui::BeginGroup();
        ImGui::TextDisabled("Exact in-game portrait · 40 × 40");
        ImGui::TextDisabled("%u encoded source bytes", entry->portrait_bytes);
        const ImVec4 minimap(
            static_cast<float>(entry->minimap_rgba & 0xFFu) / 255.0f,
            static_cast<float>((entry->minimap_rgba >> 8u) & 0xFFu) / 255.0f,
            static_cast<float>((entry->minimap_rgba >> 16u) & 0xFFu) / 255.0f,
            1.0f);
        ImGui::ColorButton(
            "Minimap colour", minimap,
            ImGuiColorEditFlags_NoPicker | ImGuiColorEditFlags_NoDragDrop,
            ImVec2(ImGui::GetFrameHeight(), ImGui::GetFrameHeight()));
        ImGui::SameLine();
        ImGui::TextDisabled("Minimap #%02X%02X%02X",
            entry->minimap_rgba & 0xFFu,
            (entry->minimap_rgba >> 8u) & 0xFFu,
            (entry->minimap_rgba >> 16u) & 0xFFu);
        ImGui::EndGroup();
    } else {
        ImGui::TextDisabled(
            "Roster identity: donor fallback · Portrait: donor fallback · Import a source-v3/v4 package to author identity media");
    }
    ImGui::TextDisabled(
        "LOD0 performance guide: %s · %u triangles · %u vertices · %u draw parts · %u package materials · %u joints · %u texture(s) · %u LOD(s)",
        characterPerformanceTier(entry), entry->lod_triangles[0],
        entry->lod_vertices[0], entry->lod_primitives[0],
        entry->stats.materials, entry->stats.joints,
        entry->stats.textures, entry->stats.lod_levels);
    if (entry->stats.decoded_texture_bytes != 0u) {
        ImGui::TextDisabled(
            "Texture memory: %.1f MiB decoded with mip levels · %.1f MiB package data",
            static_cast<double>(entry->stats.decoded_texture_bytes) /
                (1024.0 * 1024.0),
            static_cast<double>(entry->stats.encoded_texture_bytes) /
                (1024.0 * 1024.0));
    } else if (entry->stats.textures != 0u) {
        ImGui::TextDisabled(
            "Texture memory: unavailable in this legacy cache; recompile for exact accounting");
    }
    ui::TextSubtleWrapped(
        "The guide uses the actual nearest LOD plus package-wide resource costs. It is not measured frame time; use the assembly below to inspect split-screen structural load before an exact-context stress run.");

    ImGui::SeparatorText("Readiness");
    ImGui::TextDisabled(
        "Geometry ready · Normalized %s · Anchored %s · Sockets %s · Motion %s · Donor %s · Identity %s",
        normalized ? "ready" : "review",
        anchored ? "ready" : "missing",
        attachmentSocketsMapped ? "ready" : "missing",
        motionReady ? "ready" : "incomplete",
        qualified ? "qualified" : "pending",
        identityReady ? "ready" : "missing");
    if (entry->rig_role_mask != 0u) {
        ImGui::TextDisabled(
            "Rig contract: %s · %u/16 humanoid roles · %u inferred · minimum confidence %.0f%%",
            rigStatus, countCharacterBits(entry->rig_role_mask),
            countCharacterBits(entry->inferred_rig_role_mask),
            static_cast<double>(entry->rig_min_confidence_milli) / 10.0);
    } else {
        ImGui::TextDisabled(
            "Rig contract: %s · no humanoid roles mapped", rigStatus);
    }
    if (entry->rig_present != 0u) {
        ImGuiTreeNodeFlags roleFlags = 0;
        if (humanoidRig && (!humanoidRolesComplete || !rigReviewed)) {
            roleFlags |= ImGuiTreeNodeFlags_DefaultOpen;
        }
        if (ImGui::TreeNodeEx("Humanoid role review", roleFlags)) {
            ui::TextSubtleWrapped(
                "Verify anatomy against the actual compiled skin joints before enabling the solver. Node numbers remain unambiguous even when an unusually long glTF name is shortened for display.");
            if (ImGui::BeginTable(
                    "##character-rig-role-review", 4,
                    ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg |
                    ImGuiTableFlags_SizingStretchProp)) {
                ImGui::TableSetupColumn("Role");
                ImGui::TableSetupColumn("Source joint");
                ImGui::TableSetupColumn("Mapping");
                ImGui::TableSetupColumn("Solver basis");
                ImGui::TableHeadersRow();
                for (size_t slot = 0u;
                     slot < std::size(kHumanoidRigRoles); ++slot) {
                    const CharacterSemanticLabel &role =
                        kHumanoidRigRoles[slot];
                    const bool mapped = (entry->rig_role_mask & role.bit) != 0u;
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(role.name);
                    ImGui::TableNextColumn();
                    if (!mapped) {
                        ImGui::TextColored(AppTheme::bad(), "Not mapped");
                        ImGui::TableNextColumn();
                        ImGui::TextDisabled("—");
                        ImGui::TableNextColumn();
                        ImGui::TextDisabled("—");
                        continue;
                    }
                    ImGui::Text("%s  (#%u)",
                                entry->rig_role_node_name[slot],
                                entry->rig_role_node[slot]);
                    ImGui::TableNextColumn();
                    const bool inferred =
                        (entry->rig_role_flags[slot] & 1u) != 0u;
                    const float confidence =
                        static_cast<float>(
                            entry->rig_role_confidence_milli[slot]) / 10.0f;
                    const ImVec4 mappingColour =
                        inferred && !rigReviewed ? AppTheme::accent()
                                                : AppTheme::subtle();
                    ImGui::TextColored(
                        mappingColour, "%s · %.1f%%",
                        inferred ? (rigReviewed ? "inferred, reviewed"
                                                : "inferred, review needed")
                                 : "authored",
                        static_cast<double>(confidence));
                    ImGui::TableNextColumn();
                    const float *rest = entry->rig_role_rest_rotation[slot];
                    const float *bend = entry->rig_role_bend_axis[slot];
                    const bool customRest =
                        std::fabs(rest[0]) > 1.0e-5f ||
                        std::fabs(rest[1]) > 1.0e-5f ||
                        std::fabs(rest[2]) > 1.0e-5f ||
                        std::fabs(rest[3] - 1.0f) > 1.0e-5f;
                    const bool customBend =
                        std::fabs(bend[0]) > 1.0e-5f ||
                        std::fabs(bend[1]) > 1.0e-5f ||
                        std::fabs(bend[2]) > 1.0e-5f;
                    ImGui::TextDisabled(
                        "%s rest · %s bend",
                        customRest ? "corrected" : "canonical",
                        customBend ? "authored" : "automatic");
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip(
                            "Rest correction XYZW: %.4f, %.4f, %.4f, %.4f\n"
                            "Preferred bend axis: %.4f, %.4f, %.4f",
                            static_cast<double>(rest[0]),
                            static_cast<double>(rest[1]),
                            static_cast<double>(rest[2]),
                            static_cast<double>(rest[3]),
                            static_cast<double>(bend[0]),
                            static_cast<double>(bend[1]),
                            static_cast<double>(bend[2]));
                    }
                }
                ImGui::EndTable();
            }
            if (humanoidRig && !rigReviewed) {
                ui::TextSubtleWrapped(
                    "The runtime lock is deliberate: inference is a starting point, not author approval. Correct the source-v4 role map as needed, then set reviewed only after checking every row in select and all supported vehicles.");
            }
            ImGui::TreePop();
        }
    }
    if (humanoidRig && !humanoidRolesComplete) {
        const std::string missingRig = missingCharacterSemantics(
            entry->rig_role_mask, kHumanoidRigRoles);
        ImGui::TextWrapped("Missing humanoid roles: %s", missingRig.c_str());
    } else if (humanoidRolesComplete && !rigReviewed) {
        ImGui::PushStyleColor(ImGuiCol_Text, AppTheme::accent());
        ImGui::TextWrapped(
            "Every role was mapped, but the author has not reviewed the inferred skeleton. Retargeting remains locked until the mapping is explicitly reviewed.");
        ImGui::PopStyleColor();
    } else if (humanoidRolesComplete && rigReviewed) {
        ImGui::TextWrapped(
            "The humanoid hierarchy is structurally validated. Engine reference motion and bounded vehicle hand/foot contacts fill missing semantic clips; authored package clips win.");
    } else if (entry->rig_present != 0u) {
        ImGui::TextWrapped(
            "Authored-clips-only is a supported final mode for creatures and unusual skeletons; no humanoid solver will alter this character.");
    } else {
        ImGui::TextWrapped(
            "This legacy package has attachment sockets but no semantic skeleton contract. Its authored clips remain usable; re-author as source-v4 to opt into reviewed humanoid roles.");
    }
    ImGui::TextDisabled(
        "%u/10 race states · %u/3 select states · %u/%u mapped clips move",
        mappedRaceStates, mappedSelectStates,
        countCharacterBits(entry->moving_semantic_mask &
                           (raceStates | selectStates)),
        mappedRaceStates + mappedSelectStates);
    if (!qualified) {
        ImGui::PushStyleColor(ImGuiCol_Text, AppTheme::accent());
        ImGui::TextWrapped(
            "%s is not a fingerprint-qualified replacement profile yet. You may inspect this package, but activation remains unavailable.",
            donorName(entry->donor));
        ImGui::PopStyleColor();
    }
    if ((entry->stats.animations == 0u || entry->motion_channels == 0u) &&
        !referenceFallbackReady) {
        ImGui::PushStyleColor(ImGuiCol_Text, AppTheme::accent());
        ImGui::TextWrapped(
            "Static bind pose: this model has no changing animation keys. Geometry and fit can be tested, but it cannot receive a polished-motion status until clips are authored or retargeted.");
        ImGui::PopStyleColor();
    } else if ((entry->moving_semantic_mask &
                MDKR_CHARACTER_SEMANTIC_FALLBACK) == 0u &&
               !referenceFallbackReady) {
        ImGui::PushStyleColor(ImGuiCol_Text, AppTheme::accent());
        ImGui::TextWrapped(
            "The fallback clip is static. Mapped motion can still play, but every missing state holds the fallback pose.");
        ImGui::PopStyleColor();
    } else if (staticAuthoredStates != 0u) {
        std::string staticStates = missingCharacterSemantics(
            ~staticAuthoredStates, kRaceCharacterSemantics);
        const std::string staticSelect = missingCharacterSemantics(
            ~staticAuthoredStates, kSelectCharacterSemantics);
        if (!staticStates.empty() && !staticSelect.empty()) staticStates += ", ";
        staticStates += staticSelect;
        ImGui::PushStyleColor(ImGuiCol_Text, AppTheme::accent());
        ImGui::TextWrapped(
            "One or more explicitly mapped states are static. Authored mappings intentionally bypass reference motion; remove or animate those mappings before calling motion complete. Review: %s",
            staticStates.c_str());
        ImGui::PopStyleColor();
    }
    const std::string missingRace = missingCharacterSemantics(
        entry->semantic_mask, kRaceCharacterSemantics);
    const std::string missingSelect = missingCharacterSemantics(
        entry->semantic_mask, kSelectCharacterSemantics);
    if (!missingRace.empty()) {
        ImGui::TextWrapped("%s covers missing race states: %s",
                           referenceFallbackReady
                               ? "Reviewed reference motion" : "Fallback clip",
                           missingRace.c_str());
    }
    if (!missingSelect.empty()) {
        ImGui::TextWrapped("%s covers missing select states: %s",
                           referenceFallbackReady
                               ? "Reviewed reference motion" : "Fallback clip",
                           missingSelect.c_str());
    }
    ImGui::TextDisabled(
        "Sockets: seat %s · head %s · hands L/R %s/%s · feet L/R %s/%s",
        (entry->socket_mask & MDKR_CHARACTER_SOCKET_SEAT)
            ? "authored" : "required fallback",
        (entry->socket_mask & MDKR_CHARACTER_SOCKET_HEAD)
            ? "authored" : "absent",
        (entry->socket_mask & MDKR_CHARACTER_SOCKET_HAND_LEFT)
            ? "authored" : "absent",
        (entry->socket_mask & MDKR_CHARACTER_SOCKET_HAND_RIGHT)
            ? "authored" : "absent",
        (entry->socket_mask & MDKR_CHARACTER_SOCKET_FOOT_LEFT)
            ? "authored" : "absent",
        (entry->socket_mask & MDKR_CHARACTER_SOCKET_FOOT_RIGHT)
            ? "authored" : "absent");
    if ((entry->semantic_mask &
         MDKR_CHARACTER_SEMANTIC_RACE_STEER) != 0u) {
        ImGui::TextDisabled(
            "race.steer phase: 0 full left · 0.5 neutral · 1 full right");
    }

    ImGui::SeparatorText("Rig Studio");
    if (identityReady) {
        if (drawCharacterRigStudio(entry)) {
            /* The transaction rescans the registry and invalidates `entry`. */
            ImGui::PopID();
            return true;
        }
    } else {
        ui::TextSubtleWrapped(
            "Rig Studio requires an identity-capable source-v3/v4 package so a source-v4 revision can preserve its portrait and provenance exactly.");
    }

    ImGui::SeparatorText("Gameplay profile and vehicle compatibility");
    if (drawCharacterProfileStudio(entry)) {
        /* Saving refreshes the registry and invalidates `entry`; finish this
         * inspector immediately and draw the replacement on the next frame. */
        ImGui::PopID();
        return true;
    }

    ImGui::SeparatorText("Portrait Studio");
    if (drawCharacterPortraitStudio(entry)) {
        /* Saving refreshes the registry and invalidates `entry`; finish this
         * inspector immediately and draw the replacement on the next frame. */
        ImGui::PopID();
        return true;
    }

    ImGui::SeparatorText("Fit, motion, vehicles, and performance");
    changed |= drawCharacterTuningEditor(0, entry, compact);
    ImGui::SeparatorText("Performance assembly");
    drawCharacterPerformanceAssembly(entry);
    ImGui::SeparatorText("Test in the exact game renderer");
    drawCharacterExactTests(entry, compact);
    ui::Gap(ui::kGapS);
    if (ImGui::Button("Remove package from this computer...")) {
        g_characterPendingRemoval = entry->id;
        ImGui::OpenPopup("Remove custom character?");
    }
    if (ImGui::BeginPopupModal("Remove custom character?", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped(
            "Remove %s and its local compiled cache? The original file you imported is not touched.",
            entry->display_name);
        if (ImGui::Button("Remove")) {
            const std::string removedId = g_characterPendingRemoval;
            if (removeCharacterPackage(removedId)) {
                for (int slot = 0; slot < 4; ++slot) {
                    const std::string slotKey = "custom_character_p" +
                        std::to_string(slot + 1);
                    if (AppConfig::get(slotKey) == removedId) {
                        AppConfig::set(slotKey, "");
                    }
                }
                const AppConfig::PersistResult result = AppConfig::save();
                changed |= AppConfig::persistResultApplied(result);
                setStatus("Custom character removed from this computer.",
                          AppTheme::good());
            } else {
                setStatus(
                    "Character removal failed; open the importer report.",
                    AppTheme::bad());
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::PopID();
    (void)compact;
    return changed;
}

bool drawCustomCharactersSection(bool compact) {
    bool changed = false;
    if (!g_characterRegistryLoaded) refreshCharacterRegistry();
    ui::Gap(ui::kGapS);
    if (!compact) {
        ui::TextSubtleWrapped(
            "Import a self-contained .mdkrchar package, inspect and tune one "
            "package-level workshop profile, then assign it to local players. The game never "
            "needs a second ROM and never puts these local presentation choices "
            "into saves, ghosts, physics, or network authority. Every built-in "
            "gameplay profile has fingerprint-qualified car, hovercraft, plane, "
            "and character-select presentation seams.");
    }
    ImGui::Indent(ui::kGapM);

    if (mdkr_render_backend() != MDKR_BACKEND_WEBGPU) {
        ImGui::PushStyleColor(ImGuiCol_Text, AppTheme::accent());
        ImGui::TextWrapped(
            "Modern characters require the WebGPU renderer. This backend will "
            "keep the built-in racer visible, so packages remain safe but will "
            "not appear in-game.");
        ImGui::PopStyleColor();
    }

    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##character-package-path",
                             "/path/to/character.mdkrchar",
                             g_characterImportPath,
                             sizeof(g_characterImportPath));
    if (filedialog::isAvailable()) {
        if (ImGui::Button("Browse for package...")) {
            std::string picked;
            if (filedialog::openCharacterPackage(picked)) {
                std::snprintf(g_characterImportPath,
                              sizeof(g_characterImportPath), "%s",
                              picked.c_str());
            }
        }
        ImGui::SameLine();
    }
    const bool canImport = g_characterImportPath[0] != '\0';
    if (!canImport) ImGui::BeginDisabled();
    if (ImGui::Button("Validate and import")) {
        if (Settings_importCharacterPackage(g_characterImportPath)) {
            changed = true;
        }
    }
    if (!canImport) ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Rescan installed characters")) {
        refreshCharacterRegistry();
    }
    if (!g_characterRegistryDirectory.empty()) {
        ImGui::TextDisabled("%d installed · Folder: %s",
            mdkr_modern_character_registry_count(&g_characterRegistry),
            g_characterRegistryDirectory.c_str());
    }
    if (!g_characterManagerReport.empty() &&
        ImGui::TreeNode("Last importer report")) {
        ImGui::TextWrapped("%s", g_characterManagerReport.c_str());
        ImGui::TreePop();
    }

    const int characterCount =
        mdkr_modern_character_registry_count(&g_characterRegistry);
    const MdkrModernCharacterEntry *workshopEntry = nullptr;
    if (characterCount > 0) {
        int workshopIndex = mdkr_modern_character_registry_find(
            &g_characterRegistry, g_characterWorkshopSelection.c_str());
        if (workshopIndex < 0) {
            workshopIndex = 0;
            const MdkrModernCharacterEntry *first =
                mdkr_modern_character_registry_entry(&g_characterRegistry, 0);
            g_characterWorkshopSelection = first != nullptr ? first->id : "";
        }
        workshopEntry = mdkr_modern_character_registry_entry(
            &g_characterRegistry, workshopIndex);
        ImGui::SeparatorText("Character library");
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::BeginCombo(
                "Character to edit##character-workshop-library",
                workshopEntry != nullptr ? workshopEntry->display_name
                                         : "Choose a character")) {
            for (int index = 0; index < characterCount; ++index) {
                const MdkrModernCharacterEntry *entry =
                    mdkr_modern_character_registry_entry(&g_characterRegistry,
                                                          index);
                if (entry == nullptr) continue;
                const bool qualified = mdkr_modern_donor_qualified(
                    static_cast<int>(entry->donor)) != 0;
                const std::string item = std::string(entry->display_name) +
                    (qualified ? "" : " (review only)");
                if (ImGui::Selectable(
                        item.c_str(),
                        g_characterWorkshopSelection == entry->id)) {
                    g_characterWorkshopSelection = entry->id;
                    workshopEntry = entry;
                }
            }
            ImGui::EndCombo();
        }
        if (workshopEntry != nullptr) {
            changed |= drawCharacterPackageInspector(workshopEntry, compact);
        }
    } else {
        ImGui::SeparatorText("Character library");
        ui::TextSubtleWrapped(
            "No characters are installed yet. Import a validated package above to begin a local workshop draft.");
    }

    ImGui::SeparatorText("Use in game");
    ui::TextSubtleWrapped(
        "Assignments are separate from editing. A character's saved fit follows the package, regardless of which local player uses it.");
    for (int player = 0; player < 4; ++player) {
        ImGui::PushID(player);
        const std::string key =
            "custom_character_p" + std::to_string(player + 1);
        const std::string selected = AppConfig::get(key);
        const int selectedIndex = mdkr_modern_character_registry_find(
            &g_characterRegistry, selected.c_str());
        const MdkrModernCharacterEntry *selectedEntry =
            mdkr_modern_character_registry_entry(&g_characterRegistry,
                                                  selectedIndex);
        const char *preview = selectedEntry != nullptr
            ? selectedEntry->display_name : "Built-in racer";
        const std::string label =
            "Player " + std::to_string(player + 1) + "##custom-character";
        if (ImGui::BeginCombo(label.c_str(), preview)) {
            const bool noneSelected =
                selected.empty() || selectedEntry == nullptr;
            if (ImGui::Selectable("Built-in racer", noneSelected)) {
                const AppConfig::PersistResult result =
                    AppConfig::setAndSave(key, "");
                if (AppConfig::persistResultApplied(result)) {
                    changed = true;
                    setStatus("Custom character assignment saved.",
                              AppTheme::good());
                } else {
                    setStatus(
                        "The custom character assignment could not be saved.",
                        AppTheme::bad());
                }
            }
            for (int index = 0; index < characterCount; ++index) {
                const MdkrModernCharacterEntry *entry =
                    mdkr_modern_character_registry_entry(&g_characterRegistry,
                                                          index);
                if (entry == nullptr) continue;
                const bool qualified = mdkr_modern_donor_qualified(
                    static_cast<int>(entry->donor)) != 0;
                const std::string item = std::string(entry->display_name) +
                    (qualified ? "" : " (donor not qualified)");
                if (!qualified) ImGui::BeginDisabled();
                if (ImGui::Selectable(item.c_str(), selected == entry->id)) {
                    const AppConfig::PersistResult result =
                        AppConfig::setAndSave(key, entry->id);
                    if (AppConfig::persistResultApplied(result)) {
                        changed = true;
                        setStatus(
                            "Custom character assignment saved; it applies on play.",
                            AppTheme::good());
                    } else {
                        setStatus(
                            "The custom character assignment could not be saved.",
                            AppTheme::bad());
                    }
                }
                if (!qualified) ImGui::EndDisabled();
            }
            ImGui::EndCombo();
        }
        ImGui::PopID();
    }

    const int skipped =
        mdkr_modern_character_registry_skipped(&g_characterRegistry);
    if (skipped > 0) {
        ImGui::SeparatorText("Skipped");
        for (int index = 0; index < skipped; ++index) {
            ImGui::BulletText(
                "%s — %s",
                mdkr_modern_character_registry_skip_name(&g_characterRegistry,
                                                         index),
                mdkr_modern_character_registry_skip_reason(&g_characterRegistry,
                                                           index));
        }
    }
    ImGui::Unindent(ui::kGapM);
    ui::Gap(ui::kGapS);
    return changed;
}

}  // namespace

bool Settings_importCharacterPackage(const char *path) {
    if (path == nullptr || path[0] == '\0') {
        g_characterManagerReport = "Choose a .mdkrchar package first.";
        setStatus("Character import needs a package path.", AppTheme::bad());
        return false;
    }
    std::snprintf(g_characterImportPath, sizeof(g_characterImportPath), "%s",
                  path);
    if (!importCharacterPackage(path)) {
        setStatus("Character import failed; open the importer report below.",
                  AppTheme::bad());
        return false;
    }
    g_characterImportPath[0] = '\0';
    setStatus("Character package validated, compiled, and installed.",
              AppTheme::good());
    return true;
}

bool Settings_takeCharacterPreviewRequest(
    SettingsCharacterPreviewRequest &request) {
    if (!g_characterPreviewRequested) return false;
    request = std::move(g_characterPreviewRequest);
    g_characterPreviewRequest = SettingsCharacterPreviewRequest{};
    g_characterPreviewRequested = false;
    return true;
}

void Settings_publishCharacterPreviewResult(
    const std::string &packageId,
    const MdkrCharacterPreviewResult &result) {
    if (packageId.empty()) return;
    g_characterPreviewResults[packageId] = result;
}

void Settings_cancelAudioPreview() {
    mdkr_audio_config_runtime_cancel_preview();
    for (MdkrVideoKey key : {MDKR_AUDIO_MASTER_VOLUME,
                             MDKR_AUDIO_MUSIC_VOLUME,
                             MDKR_AUDIO_EFFECTS_VOLUME}) {
        EditState &edit = g_edits[static_cast<size_t>(key)];
        edit.initialized = false;
        edit.active = false;
        edit.dirty = false;
        edit.error.clear();
    }
}

void Settings_loadUiScalePreference() {
    const std::string stored = AppConfig::get("ui_scale", "1.0");
    float parsed = 1.0f;
    const float scale = AppUi_parseScale(stored.c_str(), &parsed) ? parsed : 1.0f;
    AppTheme::setUiScale(scale);
    g_uiScaleEdit = scale;
    g_uiScaleInitialized = true;
    g_uiScaleDirty = false;
    g_uiScaleError.clear();
    g_smokeGamepadFocusUsed = false;
    if (std::getenv("MDKR_APP_UI_TRACE")) {
        std::fprintf(stderr, "[app-ui] ui-scale loaded=%.2f\n",
                     static_cast<double>(scale));
    }
}

bool Settings_smokeFrameLimitCenter(int *x, int *y) {
    if (!x || !y || !g_frameLimitRectValid) return false;
    *x = static_cast<int>((g_frameLimitRectMin.x + g_frameLimitRectMax.x) * 0.5f);
    *y = static_cast<int>((g_frameLimitRectMin.y + g_frameLimitRectMax.y) * 0.5f);
    return true;
}

bool Settings_smokeFrameLimitPopup(int *focusedIndex) {
    if (!focusedIndex || !g_frameLimitPopupOpen) return false;
    *focusedIndex = g_frameLimitFocusedIndex;
    return true;
}

int Settings_smokeFrameLimitDownSteps(const char *from, const char *to) {
    if (!from || !to) return -1;
    int fromIndex = -1;
    int toIndex = -1;
    for (int i = 0; i < static_cast<int>(std::size(kFrameLimit)); ++i) {
        if (std::strcmp(kFrameLimit[i].value, from) == 0) fromIndex = i;
        if (std::strcmp(kFrameLimit[i].value, to) == 0) toIndex = i;
    }
    return fromIndex >= 0 && toIndex >= fromIndex ? toIndex - fromIndex : -1;
}

bool Settings_smokeFrameLimitRetryCenter(int *x, int *y) {
    if (!x || !y || !g_frameLimitRetryRectValid) return false;
    *x = static_cast<int>(
        (g_frameLimitRetryRectMin.x + g_frameLimitRetryRectMax.x) * 0.5f);
    *y = static_cast<int>(
        (g_frameLimitRetryRectMin.y + g_frameLimitRetryRectMax.y) * 0.5f);
    return true;
}

bool Settings_smokePresentationPaceCenter(const char *pace, int *x, int *y) {
    if (!pace || !x || !y) return false;
    const int resolved = mdkr_video_presentation_pace_from_name(pace);
    if (resolved <= 0 ||
        resolved >= static_cast<int>(std::size(g_paceRectValid)) ||
        !g_paceRectValid[resolved]) {
        return false;
    }
    *x = static_cast<int>(
        (g_paceRectMin[resolved].x + g_paceRectMax[resolved].x) * 0.5f);
    *y = static_cast<int>(
        (g_paceRectMin[resolved].y + g_paceRectMax[resolved].y) * 0.5f);
    return true;
}

bool Settings_smokeUiScaleRect(int *minX, int *minY, int *maxX, int *maxY) {
    if (!minX || !minY || !maxX || !maxY || !g_uiScaleRectValid) return false;
    *minX = static_cast<int>(g_uiScaleRectMin.x);
    *minY = static_cast<int>(g_uiScaleRectMin.y);
    *maxX = static_cast<int>(g_uiScaleRectMax.x);
    *maxY = static_cast<int>(g_uiScaleRectMax.y);
    return true;
}

void Settings_dumpSchemaContract() {
    std::printf(
        "[app] frame-limit UI contract: recommended=\"%s\" group=\"%s\" "
        "caveat=\"%s\"\n",
        kOriginalFrameLimitLabel, kModernFrameLimitGroup,
        kFrameLimitHelp);

    /*
     * The control inventory, one row per schema key: the name a voice would
     * say, the value it would say, and whether the product offers the setting
     * at all.
     *
     * tests/check_a11y_shell.py grades the walk against THIS, rather than
     * against a list kept in the test, which is the whole point: a setting
     * added tomorrow gets a row here the moment it has a schema entry, and the
     * gate then demands an utterance for it without anybody remembering to
     * extend the test. The label and value come from the same displayValue()
     * the announcement uses, so the expectation and the utterance cannot drift
     * apart while both still look right.
     *
     * Visibility is reported for the qualified WebGPU renderer with widescreen
     * engaged -- the shipped configuration. A diagnostic OpenGL session offers
     * one setting MORE (MSAA), which can only ever add an utterance the gate
     * did not require, never remove one it did.
     */
    const MdkrVideoConfig *config = mdkr_video_config_desired();
    for (int i = 0; i < MDKR_VIDEO_KEY_COUNT; ++i) {
        const MdkrVideoKey     key = static_cast<MdkrVideoKey>(i);
        const MdkrVideoSchema *s   = mdkr_video_schema(key);
        if (s == nullptr) continue;
        char value[MDKR_VIDEO_STRING_MAX];
        displayValue(key, s, config != nullptr ? &config->values[i] : nullptr,
                     value, sizeof(value));
        /* The DRAWN label -- the Copy table's where one exists -- for the same
         * reason the value comes from displayValue(): the gate's expectation
         * and the on-screen text must be one string, and the announcement
         * speaks the drawn label. */
        const Copy *copy = copyFor(key);
        std::printf(
            "[app-a11y] control key=%s visible=%d label=\"%s\" value=\"%s\"\n",
            s->name,
            AppUi_videoSettingVisible(key, /*webGpuRenderer=*/true,
                                      /*legacyStretchActive=*/false) ? 1 : 0,
            copy != nullptr ? copy->label : s->label, value);
    }
}

bool Settings_restartPending() {
    /* Keep the launcher, overlay, and original in-game settings on one
     * predicate. In particular, Video.Mode is a preset label rather than a
     * staged engine override; the runtime helper correctly evaluates the
     * individual values expanded by that preset. */
    return mdkr_video_config_restart_pending() != 0;
}

void Settings_requestControllerSection() {
    g_controllerSectionRequested = true;
}

const char *Settings_effectiveLabel(int videoKey) {
    const MdkrVideoKey key = static_cast<MdkrVideoKey>(videoKey);
    // Video.Mode is a preset label the player recognizes as the presentation
    // style ("Restored"); the individual keys read their own option label.
    const MdkrVideoValue *value = desired(key);
    if (value == nullptr) return "";
    return optionLabel(key, value->text);
}

const char *Settings_effectivePaceLabel() {
    // The frame rate is now ONE merged control (drawFrameRate) with the choices
    // Original / Smooth / Custom, so its effective value is named in that
    // vocabulary rather than by the underlying Frame limit option label the
    // player no longer selects directly. Read from the desired (effective)
    // config so a staged edit shows immediately, exactly like the raw labels.
    const MdkrVideoConfig *config = mdkr_video_config_desired();
    if (config == nullptr) return "";
    const MdkrPresentationPace pace = mdkr_video_presentation_pace(config);
    for (const PaceChoice &choice : kPaceChoices) {
        if (choice.pace == pace) return choice.label;
    }
    // Custom is a revealed state rather than a kPaceChoices value; it shares its
    // one spelling with the "Custom" radio in drawFrameRate.
    return "Custom";
}

int Settings_collectStagedOverrides(const char **out, int cap) {
    // Static storage: the caller (the launcher's boot config) holds these
    // pointers until mdkr64_engine_boot copies them, which happens on the same
    // stack, so a per-key static buffer is both sufficient and lifetime-safe.
    static char s_buf[MDKR_VIDEO_KEY_COUNT][MDKR_VIDEO_NAME_MAX + MDKR_VIDEO_STRING_MAX + 2];
    int n = 0;
    for (int i = 0; i < MDKR_VIDEO_KEY_COUNT && n < cap; ++i) {
        MdkrVideoKey k = (MdkrVideoKey)i;
        const MdkrVideoSchema *s = mdkr_video_schema(k);
        if (!s || s->scope != MDKR_VIDEO_SCOPE_RESTART) continue;
        if (!differsFromLive(k, s)) continue;
        // Video.Mode is a preset label, not a single engine override. Passing it
        // here would re-expand the preset over the individual keys the player
        // just staged; the engine reads the already-persisted resolved config.
        if (k == MDKR_VIDEO_MODE) continue;
        char v[MDKR_VIDEO_STRING_MAX];
        formatValue(k, s, desired(k), v, sizeof(v));
        std::snprintf(s_buf[n], sizeof(s_buf[n]), "%s=%s", s->name, v);
        out[n] = s_buf[n];
        ++n;
    }
    return n;
}

bool Settings_draw(SDL_Window *window, bool compact) {
    bool changed = false;
    g_frameLimitPopupOpen = false;
    g_frameLimitFocusedIndex = -1;
    /*
     * Every widget-rect flag starts each frame false, and only the widget's own
     * submission can raise it again. This is the half IsItemVisible() cannot
     * cover: a collapsed section returns from here without ever reaching the
     * widget, so nothing would run to lower a flag left true by an earlier
     * frame -- which is precisely the stale latch this panel used to have. The
     * whole set is cleared in one place so a rect added later is either listed
     * here or is not per-frame, rather than being quietly sticky.
     *
     * Limit worth knowing: this is per Settings_draw() call, so it says nothing
     * about frames where the panel is not drawn at all (another launcher tab,
     * or the overlay with settings hidden). Every scripted gate pins
     * MDKR_APP_PANEL=Settings, so for them "drawn" and "this frame" coincide.
     */
    g_frameLimitRectValid = false;
    g_frameLimitRetryRectValid = false;
    g_uiScaleRectValid = false;
    g_sectionsDrawn = 0;
    for (bool &paceValid : g_paceRectValid) paceValid = false;
    MdkrVideoRuntimeResult windowResult = MDKR_VIDEO_RUNTIME_INVALID;
    bool windowResultFresh = false;
    if (AppWindow_consumeCompleted(&windowResult, &windowResultFresh)) {
        const MdkrVideoSchema *schema = mdkr_video_schema(MDKR_WINDOW_MODE);
        EditState &edit = g_edits[static_cast<size_t>(MDKR_WINDOW_MODE)];
        // A stale completion still resynchronizes the widget from the
        // authoritative desired value; only its status line is suppressed.
        if (windowResultFresh) reportResult(windowResult, schema);
        if (resultSucceeded(windowResult)) {
            edit.dirty = false;
            edit.initialized = false;
            edit.error.clear();
            changed = true;
        } else if (windowResultFresh) {
            edit.error = g_status;
        }
    }
    const bool controllerSettingsSmoke =
        std::getenv("MDKR_APP_SMOKE_CONTROLLER_SETTINGS") != nullptr;
    const bool smokeControllerRestore =
        std::getenv("MDKR_APP_SMOKE_CONTROLLER_RESTORE") != nullptr;
    /* Both scripted pacing gates need the same thing: the frame-rate controls
     * in view without scrolling, so a queued click lands on the widget rather
     * than on whatever the panel happened to have scrolled under it. */
    const bool selectingFrameLimit =
        std::getenv("MDKR_APP_SMOKE_SELECT_FRAME_LIMIT") != nullptr ||
        std::getenv("MDKR_APP_SMOKE_SELECT_PRESENTATION_PACE") != nullptr;
    /* Same need, one group further down: the scripted interface-scale drag
     * holds a real pointer on the slider, so the slider has to be on screen
     * without a scroll the script has no way to perform. */
    const bool draggingUiScale =
        std::getenv("MDKR_APP_SMOKE_UI_SCALE_DRAG") != nullptr;
    int controllerMappingWidgets = 0;
    int controllerRumbleWidgets = 0;
    bool controllerRestoreAvailable = false;
    bool controllerRestoreSucceeded = false;

    const bool webGpuRenderer =
        mdkr_render_backend() == MDKR_BACKEND_WEBGPU;
    /* Video.Widescreen is normally hidden because no preset ever selects its
     * 0 branch (see AppUi_videoSettingVisible). A config that already resolved
     * to 0 is the exception: the player is looking at the pre-widescreen
     * stretch and needs a control to leave it. */
    const bool legacyStretchActive =
        mdkr_video_config_current()
            ->values[MDKR_VIDEO_WIDESCREEN].number == 0.0f;

    /*
     * WHY THE GROUPS ARE NAMED HERE AND NOT TAKEN FROM MdkrVideoCategory.
     *
     * The schema's categories answer "which subsystem owns this key", which is
     * the right question for the config layer and the wrong one for a player.
     * It put Camera and Menu languages under Presentation because both are
     * published by the presentation path, and it put Simulation cadence beside
     * Frame limit under Pacing because both are about time -- which is exactly
     * the confusion behind the reports that Enhanced was a frame-rate setting.
     *
     * So the page groups by what a player came to do, and the schema stays the
     * source of truth for everything else. The safety net is `drawnKey`: any
     * visible key no group claims is rendered under Other settings at the
     * bottom, so a key added to the schema tomorrow can never silently vanish
     * from the panel the way an explicit list would normally allow.
     */
    bool drawnKey[MDKR_VIDEO_KEY_COUNT] = {};
    const auto visible = [&](MdkrVideoKey key) {
        return AppUi_videoSettingVisible(key, webGpuRenderer,
                                         legacyStretchActive);
    };
    const auto row = [&](MdkrVideoKey key) {
        if (!visible(key)) return;
        // Keys the routing policy assigns to a dedicated section
        // (Accessibility, Enhancements, Content) are drawn by that section,
        // never under a group here -- one owner per control.
        if (AppUi_settingsSection(key) != AppUiSettingsSection::Category) return;
        drawnKey[static_cast<size_t>(key)] = true;
        changed |= drawKey(window, key, compact);
    };
    const auto flagsFor = [](bool open) {
        return open ? ImGuiTreeNodeFlags_DefaultOpen : ImGuiTreeNodeFlags_None;
    };

    /* The packaged-default proof. verify_unsigned_release greps this exact
     * row, and check_app_ui_input's recovery arm reads it after a reload. It
     * reports the DESIRED value from the config, deliberately not the drawn
     * row: the frame-limit control lives inside a collapsed-by-default tree,
     * and a proof of the shipped default must not depend on whether a player
     * (or a passive smoke) happened to open it. */
    if (std::getenv("MDKR_APP_UI_TRACE") != nullptr) {
        static bool tracedFrameLimit = false;
        const MdkrVideoValue *frameLimit = desired(MDKR_VIDEO_FRAME_LIMIT);
        if (!tracedFrameLimit && frameLimit != nullptr) {
            std::fprintf(stderr,
                         "[app-ui] frame-limit value=%s label=%s restartPending=%d\n",
                         frameLimit->text,
                         optionLabel(MDKR_VIDEO_FRAME_LIMIT, frameLimit->text),
                         Settings_restartPending() ? 1 : 0);
            std::fprintf(
                stderr,
                "[app-ui] frame-limit-contract recommended=\"%s\" "
                "group=\"%s\" caveat=\"%s\"\n",
                kOriginalFrameLimitLabel, kModernFrameLimitGroup,
                kFrameLimitHelp);
            tracedFrameLimit = true;
        }
    }

    if (mdkr_video_config_is_readonly()) {
        // Explicit --pure locks this session's resolved values. Timing and
        // smoothing are intentionally independent of art-direction presets,
        // so never describe a retained enhanced choice as "original".
        ui::CautionBox(
            "Original session — picture and timing are locked",
            "Frame limit, Motion smoothing, and Simulation cadence keep the values selected "
            "before launch, so a timing comparison stays honest. Sound, window and "
            "controller settings still work.");
        ui::Gap(ui::kGapM);
    }

    /*
     * NINE GROUPS, NAMED FOR WHAT A PLAYER CAME TO DO.
     *
     * There were eleven, and they answered three different questions. Picture,
     * Sound and Camera named a SUBSYSTEM. Accessibility and Gameplay named a
     * player CONCERN. App window named neither -- it was a lid over the window
     * mode, the update check and the developer-tools switch, which have nothing
     * to do with each other or with a window. Camera held exactly one row.
     *
     * Six of the eleven opened expanded, so the first screen was a wall rather
     * than a menu. Two open by default now -- Display, because it is what most
     * players came for, and Accessibility, because someone who needs it needs
     * it before they can use anything below it.
     *
     * See docs/architecture/launcher-design.md for the full mapping. The safety
     * net at the bottom is unchanged and still catches any visible key no group
     * claims, so regrouping cannot silently lose a setting.
     */
    const ImGuiTreeNodeFlags openUnlessScripted =
        flagsFor(!controllerSettingsSmoke && !draggingUiScale);

    // --- Display ------------------------------------------------------------
    if (drawSettingsSectionHeader(
            "Display",
            "Restored is the recommended default: widescreen and sharper "
            "output, with the original art direction untouched.",
            openUnlessScripted, compact)) {
        row(MDKR_VIDEO_MODE);
        // The preset's third state, said out loud. A default session reads
        // "Custom (Individual Settings)" -- a value that is not in the control's
        // own list -- and the page used to leave the player to work out why.
        const MdkrVideoValue *mode = desired(MDKR_VIDEO_MODE);
        if (mode != nullptr && std::strcmp(mode->text, "custom") == 0 &&
            !compact) {
            ui::TextSubtleWrapped(
                "Custom means your Graphics settings below no longer match any "
                "of the three presets. Choosing one here sets them all again.");
            ui::Gap(ui::kGapS);
        }
        if (webGpuRenderer) {
            ui::TextSubtle("Graphics backend: WebGPU (recommended)");
            ui::Gap(ui::kGapS);
        } else if (!compact) {
            if (ui::CardBegin("##renderer-warning", AppTheme::accent(), 0.0f)) {
                ImGui::PushStyleColor(ImGuiCol_Text, AppTheme::accent());
                ImGui::TextUnformatted("OpenGL diagnostic renderer active");
                ImGui::PopStyleColor();
                ui::TextSubtleWrapped(
                    "For the qualified visual path, use WebGPU with Restored.");
            }
            ui::CardEnd();
            ui::Gap(ui::kGapS);
        }

        const bool pacingVisible = visible(MDKR_VIDEO_FRAME_LIMIT) &&
                                   visible(MDKR_VIDEO_MOTION_SMOOTHING);
        if (pacingVisible) {
            changed |= drawFrameRate(window, compact, selectingFrameLimit);
            // drawFrameRate owns these three rows, so it owns claiming them:
            // the safety net below asks "did any group draw this key", and a
            // key drawn by a dedicated control is drawn.
            for (MdkrVideoKey key : {MDKR_VIDEO_FRAME_LIMIT,
                                     MDKR_VIDEO_MOTION_SMOOTHING,
                                     MDKR_VIDEO_ALLOW_TEARING}) {
                drawnKey[static_cast<size_t>(key)] = true;
            }
            if (std::getenv("MDKR_APP_UI_TRACE") != nullptr) {
                static bool tracedFrameRateControls = false;
                if (!tracedFrameRateControls) {
                    std::fprintf(
                        stderr,
                        "[app-ui] frame-rate-controls visible=1 "
                        "gameplay-accuracy-separated=1\n");
                    tracedFrameRateControls = true;
                }
            }
        }

        // The Camera group held this one row and nothing else. It changes what
        // you see and not how the game plays, so it belongs here; Camera shake
        // is an access need first and the routing policy keeps it under
        // Accessibility.
        row(MDKR_VIDEO_CAMERA_OBSTRUCTION);
        row(MDKR_VIDEO_GAMEPLAY_FOV);
        row(MDKR_VIDEO_ASPECT);
        row(MDKR_WINDOW_MODE);
        row(MDKR_VIDEO_WIDESCREEN);
        ImGui::Unindent(ui::kGapM);
    }

    // --- Graphics -----------------------------------------------------------
    if (drawSettingsSectionHeader(
            "Graphics",
            "Image quality only. None of these change the art direction.",
            flagsFor(false), compact)) {
        row(MDKR_VIDEO_RENDER_SCALE);
        row(MDKR_VIDEO_MSAA);
        row(MDKR_VIDEO_ANISOTROPY);
        row(MDKR_VIDEO_MIPMAPS);
        row(MDKR_VIDEO_HIRES_TEXT);
        row(MDKR_VIDEO_WORLD_SHADOWS);
        row(MDKR_VIDEO_REMASTER_FX);
        ImGui::Unindent(ui::kGapM);
    }

    // --- Audio --------------------------------------------------------------
    if (drawSettingsSectionHeader(
            "Audio", "Volume levels, remembered as you set them.",
            flagsFor(false), compact)) {
        row(MDKR_AUDIO_MASTER_VOLUME);
        row(MDKR_AUDIO_MUSIC_VOLUME);
        row(MDKR_AUDIO_EFFECTS_VOLUME);
        ImGui::Unindent(ui::kGapM);
    }

    // --- Controls -----------------------------------------------------------
    if (g_controllerSectionRequested) {
        ImGui::SetNextItemOpen(true, ImGuiCond_Always);
    }
    if (drawSettingsSectionHeader(
            "Controls",
            "Rumble, and which N64 button each control presses. The left "
            "stick is always steering.",
            flagsFor(controllerSettingsSmoke), compact)) {
        if (g_controllerSectionRequested) ImGui::SetScrollHereY(0.0f);
        row(MDKR_INPUT_RUMBLE_ENABLED);
        row(MDKR_INPUT_RUMBLE_PROFILE);
        if (visible(MDKR_INPUT_RUMBLE_ENABLED)) controllerRumbleWidgets++;
        if (visible(MDKR_INPUT_RUMBLE_PROFILE)) controllerRumbleWidgets++;
        if (g_controllerSectionRequested) {
            ImGui::SetNextItemOpen(true, ImGuiCond_Always);
        }
        if (ImGui::TreeNodeEx("Button mapping",
                              ImGuiTreeNodeFlags_SpanAvailWidth |
                                  flagsFor(controllerSettingsSmoke))) {
            for (int i = MDKR_INPUT_CONTROLLER_A;
                 i <= MDKR_INPUT_CONTROLLER_RIGHT_STICK_RIGHT; ++i) {
                const MdkrVideoKey key = static_cast<MdkrVideoKey>(i);
                if (!visible(key)) continue;
                row(key);
                controllerMappingWidgets++;
            }
            const bool restorePressed = ImGui::Button(
                "Restore defaults", ui::kBtnWide());
            controllerRestoreAvailable = true;
            static bool smokeRestoreAttempted = false;
            const bool runSmokeRestore = controllerSettingsSmoke &&
                smokeControllerRestore && !smokeRestoreAttempted;
            if (restorePressed || runSmokeRestore) {
                if (runSmokeRestore) smokeRestoreAttempted = true;
                const bool restored = restoreControllerDefaults();
                controllerRestoreSucceeded |= restored;
                changed |= restored;
            }
            ImGui::TreePop();
        }
        ImGui::Unindent(ui::kGapM);
    }
    g_controllerSectionRequested = false;

    // --- Accessibility ------------------------------------------------------
    // Open by default: a player who needs it needs it before they can use
    // anything else on the page. Rows come from the routing policy, so an
    // accessibility key added tomorrow is drawn -- and voiced -- here.
    if (drawSettingsSectionHeader(
            "Accessibility",
            "How the game reads and speaks. None of these change how it "
            "plays.",
            flagsFor(draggingUiScale ||
                     (!controllerSettingsSmoke && !selectingFrameLimit)),
            compact)) {
        ImGui::Unindent(ui::kGapM);  // the section helper manages its own indent
        changed |= drawAccessibilitySection(window, compact, webGpuRenderer,
                                            legacyStretchActive);
    }

    // --- Gameplay -----------------------------------------------------------
    if (drawSettingsSectionHeader(
            "Gameplay",
            "The only settings on this page that change how the game plays.",
            flagsFor(false), compact)) {
        if (visible(MDKR_VIDEO_SIMULATION_CADENCE) && !compact) {
            const MdkrVideoValue *cadence =
                desired(MDKR_VIDEO_SIMULATION_CADENCE);
            const bool enhanced =
                cadence != nullptr &&
                std::strcmp(cadence->text, "enhanced") == 0;
            ui::CautionBox(
                enhanced ? "Enhanced is on — experimental, and races run off pace"
                         : "Enhanced is experimental and changes how the game plays",
                "Enhanced runs DKR's logic at 60 Hz instead of the 30 Hz it "
                "was written for. The boss rematches are winnable, but they "
                "still finish measurably off the pace the game intends, "
                "because the original physics is written around the 30 Hz "
                "step rather than derived from it. Original is exact — "
                "bit for bit the game as it shipped. Frame rate under Display "
                "gives you a smooth 60 FPS picture without changing the game "
                "at all, and is the recommended way to get one.");
            ui::Gap(ui::kGapS);
        }
        row(MDKR_VIDEO_SIMULATION_CADENCE);
        row(MDKR_VIDEO_MENU_LANGUAGES);
        ImGui::Unindent(ui::kGapM);
    }

    // Two sections that are not a schema category. Both gather keys the
    // categories would otherwise scatter -- see AppUi_settingsSection -- and
    // both add something no generated row can: the extras get one action that
    // resets them and nothing else, and the content packs get the list of what
    // the scan actually found.
    if (drawSettingsSectionHeader(
            "Extras",
            "Things you opt into. Each one says whether it changes how the "
            "game plays.",
            ImGuiTreeNodeFlags_None, compact)) {
        ImGui::Unindent(ui::kGapM);  // the section helper manages its own indent
        changed |= drawEnhancementsSection(window, compact, webGpuRenderer,
                                           legacyStretchActive);
    }

    const MdkrModRegistry *packs = platform_content_packs_registry();
    const MdkrVideoConfig *liveConfig = mdkr_video_config_current();
    const char *disabledList = liveConfig != nullptr
        ? liveConfig->values[MDKR_CONTENT_PACK_DISABLED].text : "";
    traceContentPacks(packs, disabledList);
    // Open when the scan found anything at all, including something it could
    // not read. A player who installed a pack has a question this section
    // answers; a player who has never installed one does not, and a collapsed
    // header keeps the panel that player's size.
    const bool anyPacks = mdkr_mod_registry_count(packs) > 0 ||
                          mdkr_mod_registry_skipped(packs) > 0;
    if (drawSettingsSectionHeader(
            "Content packs",
            "Packs that replace artwork or music, from the mods folder.",
            anyPacks ? ImGuiTreeNodeFlags_DefaultOpen
                     : ImGuiTreeNodeFlags_None,
            compact)) {
        ImGui::Unindent(ui::kGapM);  // the section helper manages its own indent
        changed |= drawContentSection(window, compact, packs, disabledList);
    }

    if (!g_characterRegistryLoaded) refreshCharacterRegistry();
    const bool anyCharacters =
        mdkr_modern_character_registry_count(&g_characterRegistry) > 0 ||
        mdkr_modern_character_registry_skipped(&g_characterRegistry) > 0;
    if (drawSettingsSectionHeader(
            "Custom characters",
            "Locally authored high-fidelity character presentation.",
            anyCharacters ? ImGuiTreeNodeFlags_DefaultOpen
                          : ImGuiTreeNodeFlags_None,
            compact)) {
        ImGui::Unindent(ui::kGapM);
        changed |= drawCustomCharactersSection(compact);
    }

    // --- Advanced -----------------------------------------------------------
    // What "App window" used to hold once the window mode moved to Display: the
    // update check and the developer-tools switch, which are diagnostic rather
    // than about a window. The catch-all loop comes with them, so a key like a
    // future Interface option still cannot end up with a schema row, an
    // environment variable, and no control anywhere in the product. The
    // unclaimed-key safety net is folded in here too, so the page has one
    // last-resort home rather than two.
    int unclaimed = 0;
    for (int i = 0; i < MDKR_VIDEO_KEY_COUNT; ++i) {
        const MdkrVideoKey key = static_cast<MdkrVideoKey>(i);
        if (drawnKey[i] || !visible(key)) continue;
        if (AppUi_settingsSection(key) != AppUiSettingsSection::Category)
            continue;  // owned by a dedicated section above
        if (mdkr_video_schema(key) == nullptr) continue;
        ++unclaimed;
    }
    if (unclaimed > 0 && drawSettingsSectionHeader(
            "Advanced",
            "Updates, developer tools, and anything without a home above.",
            flagsFor(false), compact)) {
        for (int i = 0; i < MDKR_VIDEO_KEY_COUNT; ++i) {
            const MdkrVideoKey key = static_cast<MdkrVideoKey>(i);
            if (drawnKey[i]) continue;
            row(key);
        }
        ui::Gap(ui::kGapS);
        ImGui::Unindent(ui::kGapM);
    }

    if (controllerSettingsSmoke) {
        static bool tracedControllerSettings = false;
        if (!tracedControllerSettings) {
            std::fprintf(
                stderr,
                "[app-ui] controller settings rendered mappings=%d rumble=%d "
                "restoreAvailable=%d restoreSucceeded=%d\n",
                controllerMappingWidgets, controllerRumbleWidgets,
                controllerRestoreAvailable ? 1 : 0,
                controllerRestoreSucceeded ? 1 : 0);
            tracedControllerSettings = true;
        }
    }

    /* No third "your changes are staged" line here. The card at the top of the
     * page names the action, and each staged row already says what will run
     * next and what is running now; a footer repeating it made one fact appear
     * three times on one page. */

    if (!g_status.empty()) {
        ui::Gap(ui::kGapS);
        ImGui::PushStyleColor(ImGuiCol_Text, g_statusColor);
        ImGui::TextWrapped("%s", g_status.c_str());
        ImGui::PopStyleColor();
    }

    return changed;
}
