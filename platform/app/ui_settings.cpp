// ui_settings.cpp — see ui_settings.h.
#include "ui_settings.h"
#include "app_config.h"
#include "app_theme.h"
#include "app_ui_policy.h"
#include "app_version.h"
#include "app_window.h"
#include "character_candidate_index.h"
#include "character_draft_snapshot.h"
#include "character_draft_store.h"
#include "character_edit_history.h"
#include "character_portrait_import.h"
#include "character_portrait_studio.h"
#include "character_raw_draft_store.h"
#include "character_raw_intake_index.h"
#include "character_revision_index.h"
#include "character_test_evidence_store.h"
#include "character_visual_report.h"
#include "character_workshop_model.h"
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
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <ctime>
#include <map>
#include <set>
#include <string>
#include <type_traits>
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
char g_characterConversionOutputPath[MDKR_MODERN_CHARACTER_PATH_MAX] = {0};
bool g_characterConversionSmokePrefilled = false;
std::string g_characterManagerReport;
std::string g_characterPendingRemoval;
std::string g_characterWorkshopSelection;
bool g_characterWorkshopSelectionLoaded = false;
CharacterWorkshopTab g_characterWorkshopTab = CharacterWorkshopTab::Overview;
bool g_characterWorkshopTabLoaded = false;
bool g_characterWorkshopTabForceSelection = false;
bool g_characterWorkshopOpenRequested = false;

void persistCharacterWorkshopTab(CharacterWorkshopTab tab,
                                 bool forceSelection);

struct CharacterImportCandidate {
    bool ready = false;
    bool portable = false;
    bool installed = false;
    bool installedEnabled = false;
    bool rightsConfirmed = false;
    bool disposableRawCandidate = false;
    std::string rawDraftId;
    std::string packagePath;
    std::string reviewedInstalledDigest;
    CharacterCandidateIndex::Candidate next;
    CharacterCandidateIndex::Candidate current;
};

CharacterImportCandidate g_characterImportCandidate;

struct CharacterRawIntake {
    bool loaded = false;
    bool inspected = false;
    std::string draftId;
    char modelPath[MDKR_MODERN_CHARACTER_PATH_MAX] = {0};
    char licensePath[MDKR_MODERN_CHARACTER_PATH_MAX] = {0};
    char packageId[65] = {0};
    char displayName[97] = {0};
    char spdx[129] = {0};
    char attribution[257] = {0};
    char sourceUrl[2049] = {0};
    int donor = 9;
    bool vehicles[3] = {true, true, true};
    int sourceForward = 0;
    float targetHeight = 1.25f;
    int fallback = -1;
    int seat = -1;
    int head = -1;
    std::string savedMappingModelSha256;
    std::string savedFallback;
    std::string savedSeat;
    std::string savedHead;
    CharacterRawIntakeIndex::Inventory inventory;
};

CharacterRawIntake g_characterRawIntake;
bool g_characterRawIntakeTracePrinted = false;
// Token-gated rendered-test actions. They call the same transactional paths as
// the widgets; the two-frame install delay ensures the ordinary candidate
// review is submitted and rendered before the test confirms local rights.
bool g_characterRawDraftSmokeActionApplied = false;
int g_characterRawDraftSmokeInstallFrames = 0;
CharacterRawDraftStore::Inventory g_characterRawDrafts;
bool g_characterRawDraftsLoaded = false;
bool g_characterRawDraftsWritable = false;
bool g_characterRawDraftStoreTracePrinted = false;
bool g_characterRawDraftClosedTracePrinted = false;
bool g_characterRawEditorOpen = false;
std::string g_characterRawDraftError;
MdkrTextStateFileSpec g_characterRawDraftFileSpec{
    "character_raw_drafts-v1.tsv", nullptr, nullptr,
};

using CharacterRevisionRow = CharacterRevisionIndex::Row;

struct CharacterRevisionInventory {
    bool loaded = false;
    unsigned total = 0u;
    int selected = 0;
    char exportPath[MDKR_MODERN_CHARACTER_PATH_MAX] = {0};
    char portableExportPath[MDKR_MODERN_CHARACTER_PATH_MAX] = {0};
    std::vector<CharacterRevisionRow> rows;
};

std::map<std::string, CharacterRevisionInventory>
    g_characterRevisionInventories;

struct CharacterIdentityEdit {
    bool loaded = false;
    uint8_t sourceSha256[32] = {0};
    char displayName[MDKR_MODERN_CHARACTER_NAME_MAX] = {0};
    char shortName[MDKR_MODERN_CHARACTER_SHORT_NAME_MAX] = {0};
    char narrationName[MDKR_MODERN_CHARACTER_NAME_MAX] = {0};
    char sortLabel[MDKR_MODERN_CHARACTER_NAME_MAX] = {0};
    char portraitPath[MDKR_MODERN_CHARACTER_PATH_MAX] = {0};
    float minimapRgb[3] = {0.86f, 0.28f, 0.56f};
    std::array<uint8_t, MDKR_MODERN_PORTRAIT_BYTES> canvas{};
    CharacterPortraitStudio::Canvas styleSource{};
    CharacterPortraitStudio::Canvas stylePreview{};
    CharacterPortraitStudio::Recipe styleRecipe{};
    CharacterPortraitImport::SourceRecord portraitSourceRecord{};
    char importPath[MDKR_MODERN_CHARACTER_PATH_MAX] = {0};
    CharacterPortraitImport::Image importImage{};
    CharacterPortraitImport::Recipe importRecipe{};
    CharacterPortraitImport::Thumbnail importThumbnail{};
    CharacterPortraitStudio::Canvas importPreview{};
    std::string importError;
    CharacterEditHistory::Track importHistory{};
    uint32_t importDragCropX = 0u;
    uint32_t importDragCropY = 0u;
    bool importPreviewValid = false;
    bool importPreviewDirty = false;
    bool importFromExactRenderer = false;
    float paintRgba[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    float replaceFromRgba[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    int tool = 0;
    int selectedPixel[2] = {20, 20};
    int selection[4] = {0, 0, MDKR_MODERN_PORTRAIT_SIZE,
                        MDKR_MODERN_PORTRAIT_SIZE};
    int selectionDelta[2] = {0, 0};
    int replaceTolerance = 0;
    bool canvasDirty = false;
    bool strokeActive = false;
    bool stylePreviewValid = false;
    bool showOnion = false;
};

std::map<std::string, CharacterIdentityEdit> g_characterIdentityEdits;
std::set<std::string> g_characterPortraitStyleTraceKeys;
std::set<std::string> g_characterPortraitSourceTraceKeys;
std::set<std::string> g_characterPortraitSourceSmokePackages;

struct CharacterPendingPortraitSource {
    std::string path;
    bool exactRenderer = false;
};
std::map<std::string, CharacterPendingPortraitSource>
    g_characterPendingPortraitSources;

struct CharacterProfileEdit {
    bool loaded = false;
    uint8_t sourceSha256[32] = {0};
    uint32_t donor = 9u;
    uint32_t vehicleMask = 7u;
    uint32_t comparisonVehicle = MDKR_DONOR_VEHICLE_CAR;
};

std::map<std::string, CharacterProfileEdit> g_characterProfileEdits;
MdkrDonorGameplayProfiles g_donorGameplayProfiles{};
std::string g_donorGameplayProfilesUnavailableReason;
std::map<std::string, int> g_characterAssemblyPlayers;
std::map<std::string, int> g_characterTestPlayers;
std::map<std::string, int> g_characterTestPoses;
std::map<std::string, int> g_characterTestPosePhases;
std::map<std::string, int> g_characterTestViewYawDegrees;
std::map<std::string, int> g_characterTestViewPitchDegrees;
std::map<std::string, int> g_characterTestLighting;

struct CharacterCaptureEdit {
    bool enabled = false;
    char pngPath[1024] = {0};
    char reportPath[1024] = {0};
};
std::map<std::string, CharacterCaptureEdit> g_characterCaptureEdits;
std::set<std::string> g_characterPoseInspectionTracePackages;

struct CharacterInspectionPose {
    MdkrCharacterPreviewPose pose;
    const char *label;
    const char *semantic;
};

constexpr CharacterInspectionPose kCharacterInspectionPoses[] = {
#define MDKR_CHARACTER_INSPECTION_POSE_ROW(suffix, semantic, label) \
    {MDKR_CHARACTER_PREVIEW_POSE_##suffix, label, semantic},
    MDKR_MODERN_CHARACTER_INSPECTION_SEMANTICS(
        MDKR_CHARACTER_INSPECTION_POSE_ROW)
#undef MDKR_CHARACTER_INSPECTION_POSE_ROW
};
static_assert(std::size(kCharacterInspectionPoses) ==
                  MDKR_MODERN_CHARACTER_INSPECTION_SEMANTIC_COUNT &&
                  std::size(kCharacterInspectionPoses) + 1u ==
                      MDKR_CHARACTER_PREVIEW_POSE_COUNT,
              "pose inspector must expose every exact-renderer semantic");

struct CharacterInspectionLighting {
    MdkrWorkshopPreviewLighting lighting;
    const char *label;
    const char *help;
};

constexpr CharacterInspectionLighting kCharacterInspectionLighting[] = {
    {MDKR_WORKSHOP_PREVIEW_LIGHTING_NEUTRAL, "Neutral",
     "Balanced character light used during ordinary play."},
    {MDKR_WORKSHOP_PREVIEW_LIGHTING_BRIGHT, "Bright",
     "High ambient fill for finding texture, normal, and silhouette defects."},
    {MDKR_WORKSHOP_PREVIEW_LIGHTING_LOW_KEY, "Low key",
     "Low ambient fill for checking facial and costume readability in shadow."},
    {MDKR_WORKSHOP_PREVIEW_LIGHTING_BACKLIT, "Backlit",
     "Light from behind the model for checking outline and thin geometry."},
};
static_assert(std::size(kCharacterInspectionLighting) ==
                  MDKR_WORKSHOP_PREVIEW_LIGHTING_COUNT,
              "lighting inspector must expose every runtime preset");
struct CharacterPreviewSessionResult {
    MdkrCharacterPreviewResult result{};
    std::string sourceSha256;
    std::string fitSha256;
    std::string presentationSha256;
    std::string capturePng;
};
std::map<std::string, CharacterPreviewSessionResult>
    g_characterPreviewResults;
std::map<std::string, std::vector<CharacterVisualReport::Capture>>
    g_characterVisualCaptures;
SettingsCharacterPreviewRequest g_characterPreviewRequest;
bool g_characterPreviewRequested = false;
CharacterTestEvidenceStore::Inventory g_characterTestEvidence;
bool g_characterTestEvidenceLoaded = false;
bool g_characterTestEvidenceWritable = false;
std::string g_characterTestEvidenceError;
MdkrTextStateFileSpec g_characterTestEvidenceFileSpec{
    // Keep the established filename so authenticated v1 inventories are found
    // and migrated in place; the serialized header carries the v2 schema.
    "character_test_evidence-v1.tsv", nullptr, nullptr,
};
std::map<std::string, unsigned> g_characterTestEvidenceSelectedCell;
bool g_characterTestEvidenceSmokeActionApplied = false;
bool g_characterTestEvidenceErrorTracePrinted = false;
std::set<std::string> g_characterTestEvidenceTracePackages;
std::set<std::string> g_characterFitEvidenceTraceContexts;

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
std::map<std::string, int> g_characterFitSpatialViews;
std::map<std::string, int> g_characterContactSpatialViews;
std::map<std::string, int> g_characterSelectedContacts;
std::map<ImGuiID, bool> g_characterSpatialGestureDirty;
std::set<std::string> g_characterSpatialControlTracePackages;

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

enum class CharacterHistoryTool : size_t {
    Identity = 0u,
    Profile,
    Rig,
    Fit,
    Performance,
    Test,
    Count,
};

struct CharacterPackageHistory {
    std::string sourceDigest;
    std::array<CharacterEditHistory::Track,
               static_cast<size_t>(CharacterHistoryTool::Count)> tracks;
};

struct CharacterHistoryFrame {
    bool ready = false;
    bool actionApplied = false;
    CharacterHistoryTool tool = CharacterHistoryTool::Identity;
    std::string before;
};

std::map<std::string, CharacterPackageHistory> g_characterEditHistories;
std::set<std::string> g_characterHistoryTraceKeys;

CharacterHistoryFrame beginCharacterHistory(
    const MdkrModernCharacterEntry *entry, CharacterHistoryTool tool);
void finishCharacterHistory(const MdkrModernCharacterEntry *entry,
                            CharacterHistoryFrame &frame);

CharacterDraftStore::Inventory g_characterDrafts;
bool g_characterDraftsLoaded = false;
bool g_characterDraftsWritable = false;
std::string g_characterDraftError;
std::map<std::string, std::string> g_characterActiveDrafts;
struct CharacterDraftReviewState {
    uint32_t mask = 0u;
    std::string signature[MDKR_CHARACTER_CONTEXT_COUNT];
};
std::map<std::string, CharacterDraftReviewState> g_characterDraftReviews;
std::map<std::string, bool> g_characterPendingDraftFit;
std::string g_characterPendingDraftRemoval;
char g_characterDraftName[CharacterDraftStore::kMaximumNameBytes + 1u] = {};
std::string g_characterDraftNameOwner;
MdkrTextStateFileSpec g_characterDraftFileSpec{
    "character_workshop_drafts-v1.tsv", nullptr, nullptr,
};

MdkrTextStateStorage characterDraftStorage() {
    return MdkrTextStateStorage{
        &g_characterDraftFileSpec,
        mdkr_text_state_file_read,
        mdkr_text_state_file_write,
    };
}

void loadCharacterDraftInventory() {
    if (g_characterDraftsLoaded) return;
    CharacterDraftStore::Inventory inventory;
    const CharacterDraftStore::LoadResult result =
        CharacterDraftStore::load(
            characterDraftStorage(), inventory, g_characterDraftError);
    g_characterDraftsLoaded = true;
    g_characterDraftsWritable =
        result == CharacterDraftStore::LoadResult::Loaded ||
        result == CharacterDraftStore::LoadResult::Missing;
    if (g_characterDraftsWritable) {
        g_characterDrafts = std::move(inventory);
    }
}

bool replaceCharacterDraftInventory(
    CharacterDraftStore::Inventory inventory) {
    if (!g_characterDraftsWritable ||
        !CharacterDraftStore::save(
            characterDraftStorage(), inventory, g_characterDraftError)) {
        return false;
    }
    g_characterDrafts = std::move(inventory);
    return true;
}

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

std::string characterTestPresentationSignature() {
    static const MdkrVideoKey keys[] = {
        MDKR_VIDEO_REMASTER_FX,
        MDKR_VIDEO_WIDESCREEN,
        MDKR_VIDEO_ASPECT,
        MDKR_VIDEO_RENDER_SCALE,
        MDKR_VIDEO_MSAA,
        MDKR_VIDEO_ANISOTROPY,
        MDKR_VIDEO_MIPMAPS,
        MDKR_VIDEO_TEXTURE_PACK,
        MDKR_VIDEO_GAMEPLAY_FOV,
        MDKR_VIDEO_SIMULATION_CADENCE,
        MDKR_VIDEO_FRAME_LIMIT,
        MDKR_VIDEO_MOTION_SMOOTHING,
        MDKR_VIDEO_MODE,
        MDKR_VIDEO_WORLD_SHADOWS,
        MDKR_VIDEO_CAMERA_OBSTRUCTION,
        MDKR_VIDEO_ALLOW_TEARING,
        MDKR_VIDEO_CAMERA_COMFORT,
        MDKR_VIDEO_HIRES_TEXT,
        MDKR_CONTENT_PACKS_ENABLED,
        MDKR_CONTENT_PACK_DISABLED,
        MDKR_ENH_DRAW_DISTANCE,
        MDKR_ENH_LOD_BIAS,
        MDKR_VIDEO_WIDESCREEN_HUD,
    };
    std::string canonical = "mdkr-character-test-presentation-v1\n";
    canonical += AppVersion();
    canonical.push_back('\n');
    for (MdkrVideoKey key : keys) {
        const MdkrVideoSchema *schema = mdkr_video_schema(key);
        const MdkrVideoValue *value = desired(key);
        if (schema == nullptr || value == nullptr) return {};
        canonical += schema->name;
        canonical.push_back('=');
        if (schema->type == MDKR_VIDEO_TYPE_STRING) {
            canonical += value->text;
        } else if (schema->type == MDKR_VIDEO_TYPE_INT) {
            canonical += std::to_string(static_cast<int>(value->number));
        } else {
            canonical += characterFloatText(value->number);
        }
        canonical.push_back('\n');
    }
    char digest[MDKR_SHA256_HEX_SIZE];
    mdkr_sha256_hex(canonical.data(), canonical.size(), digest);
    return digest;
}

MdkrTextStateStorage characterTestEvidenceStorage() {
    return MdkrTextStateStorage{
        &g_characterTestEvidenceFileSpec,
        mdkr_text_state_file_read,
        mdkr_text_state_file_write,
    };
}

void loadCharacterTestEvidence() {
    if (g_characterTestEvidenceLoaded) return;
    g_characterTestEvidenceLoaded = true;
    CharacterTestEvidenceStore::Inventory inventory;
    const CharacterTestEvidenceStore::LoadResult result =
        CharacterTestEvidenceStore::load(
            characterTestEvidenceStorage(), inventory,
            g_characterTestEvidenceError);
    g_characterTestEvidenceWritable =
        result == CharacterTestEvidenceStore::LoadResult::Loaded ||
        result == CharacterTestEvidenceStore::LoadResult::Missing;
    if (g_characterTestEvidenceWritable) {
        g_characterTestEvidence = std::move(inventory);
    }
}

bool replaceCharacterTestEvidence(
    CharacterTestEvidenceStore::Inventory replacement) {
    if (!g_characterTestEvidenceWritable ||
        !CharacterTestEvidenceStore::save(
            characterTestEvidenceStorage(), replacement,
            g_characterTestEvidenceError)) {
        return false;
    }
    g_characterTestEvidence = std::move(replacement);
    return true;
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
    const auto activeDraft = g_characterActiveDrafts.find(entry->id);
    const auto draftReview = g_characterDraftReviews.find(entry->id);
    if (activeDraft != g_characterActiveDrafts.end() &&
        draftReview != g_characterDraftReviews.end()) {
        return context < MDKR_CHARACTER_CONTEXT_COUNT &&
            (draftReview->second.mask & (1u << context)) != 0u &&
            draftReview->second.signature[context] == signature;
    }
    return !signature.empty() &&
        AppConfig::get(characterFitReviewKey(entry->id, context)) == signature;
}

bool autosaveActiveCharacterDraft(const MdkrModernCharacterEntry *entry);

bool persistCharacterFitReview(const MdkrModernCharacterEntry *entry,
                               const CharacterTuningEdit &edit,
                               unsigned context) {
    const std::string signature = characterFitReviewSignature(
        entry, edit, context);
    if (signature.empty()) return false;
    if (g_characterActiveDrafts.find(entry->id) !=
        g_characterActiveDrafts.end()) {
        CharacterDraftReviewState &review =
            g_characterDraftReviews[entry->id];
        review.mask |= 1u << context;
        review.signature[context] = signature;
        if (autosaveActiveCharacterDraft(entry)) {
            setStatus("Exact-context review autosaved in the named draft.",
                      AppTheme::good());
            return true;
        }
        setStatus("Draft review could not be autosaved.", AppTheme::bad());
        return false;
    }
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
    if (g_characterActiveDrafts.find(entry->id) !=
        g_characterActiveDrafts.end()) {
        CharacterDraftReviewState &review =
            g_characterDraftReviews[entry->id];
        review.mask &= ~(1u << context);
        review.signature[context].clear();
        if (autosaveActiveCharacterDraft(entry)) {
            setStatus("Fit review reopened in the named draft.",
                      AppTheme::accent());
            return true;
        }
        setStatus("Draft review change could not be autosaved.",
                  AppTheme::bad());
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

void stageCharacterTuningConfig(const char *packageId,
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
}

bool persistCharacterTuning(const char *packageId,
                            const CharacterTuningEdit &edit) {
    if (g_characterActiveDrafts.find(packageId) !=
        g_characterActiveDrafts.end()) {
        g_characterPreviewResults.erase(packageId);
        setStatus("Fit change staged in the named draft.", AppTheme::good());
        return true;
    }
    stageCharacterTuningConfig(packageId, edit);
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
                         const std::vector<std::string> &commandArguments,
                         bool refreshOnSuccess = true) {
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
    if (exitCode == 0 && refreshOnSuccess) refreshCharacterRegistry();
    return exitCode == 0;
}

bool parseCharacterRevisionInventory(
    const std::string &text, CharacterRevisionInventory &inventory) {
    CharacterRevisionIndex::Inventory index;
    CharacterRevisionInventory parsed;
    if (!CharacterRevisionIndex::parse(text, index)) return false;
    parsed.loaded = true;
    parsed.total = index.total;
    parsed.selected = 0;
    parsed.rows = std::move(index.rows);
    inventory = std::move(parsed);
    return true;
}

bool loadCharacterRevisionInventory(const std::string &packageId) {
    if (g_characterRegistryDirectory.empty()) return false;
    const std::string path = g_characterRegistryDirectory +
        "/.launcher-character-revisions.tsv";
    (void)mdkr_remove_utf8(path.c_str());
    const bool indexed = runCharacterManager(
        "write-revision-index", {packageId, path}, false);
    const std::string index = indexed ? readCharacterManagerResult(path) : "";
    (void)mdkr_remove_utf8(path.c_str());
    CharacterRevisionInventory inventory;
    if (!indexed || !parseCharacterRevisionInventory(index, inventory)) {
        if (indexed) {
            g_characterManagerReport =
                "The revision index was malformed; no recovery action was enabled.";
        }
        return false;
    }
    const auto previous = g_characterRevisionInventories.find(packageId);
    if (previous != g_characterRevisionInventories.end()) {
        std::snprintf(inventory.exportPath, sizeof(inventory.exportPath), "%s",
                      previous->second.exportPath);
        std::snprintf(
            inventory.portableExportPath, sizeof(inventory.portableExportPath),
            "%s", previous->second.portableExportPath);
    }
    g_characterRevisionInventories[packageId] = std::move(inventory);
    return true;
}

bool restoreCharacterRevision(const std::string &packageId,
                              const std::string &sourceSha256) {
    return runCharacterManager("restore", {packageId, sourceSha256});
}

bool rebuildCharacterAssembly(const std::string &packageId) {
    return runCharacterManager("rebuild", {packageId});
}

bool exportCharacterRevision(const std::string &packageId,
                             const std::string &sourceSha256,
                             const std::string &outputPath) {
    if (outputPath.empty()) return false;
    return runCharacterManager(
        "export", {packageId, sourceSha256, outputPath}, false);
}

bool exportPortableCharacterRevision(const std::string &packageId,
                                     const std::string &sourceSha256,
                                     const std::string &outputPath) {
    if (outputPath.empty()) return false;
    return runCharacterManager(
        "export-portable", {packageId, sourceSha256, outputPath}, false);
}

std::string characterDigestHex(const uint8_t digest[32]) {
    static const char digits[] = "0123456789abcdef";
    std::string text(64u, '0');
    for (size_t index = 0u; index < 32u; ++index) {
        text[index * 2u] = digits[digest[index] >> 4u];
        text[index * 2u + 1u] = digits[digest[index] & 0xFu];
    }
    return text;
}

bool characterDigestTextValid(const std::string &digest) {
    return digest.size() == 64u &&
        std::all_of(digest.begin(), digest.end(), [](char byte) {
            return (byte >= '0' && byte <= '9') ||
                   (byte >= 'a' && byte <= 'f');
        });
}

CharacterCandidateIndex::Candidate installedCharacterSummary(
    const MdkrModernCharacterEntry &entry) {
    CharacterCandidateIndex::Candidate summary;
    summary.id = entry.id;
    summary.displayName = entry.display_name;
    summary.shortName = entry.short_name;
    summary.narrationName = entry.narration_name;
    summary.sortLabel = entry.sort_label;
    summary.sourceDigest = characterDigestHex(entry.source_sha256);
    summary.donor = entry.donor;
    summary.vehicleMask = entry.vehicle_mask;
    summary.vertices = entry.stats.vertices;
    summary.triangles = entry.stats.triangles;
    summary.primitives = entry.stats.primitives;
    summary.lodLevels = entry.stats.lod_levels;
    summary.materials = entry.stats.materials;
    summary.textures = entry.stats.textures;
    summary.nodes = entry.stats.nodes;
    summary.skins = entry.stats.skins;
    summary.joints = entry.stats.joints;
    summary.animations = entry.stats.animations;
    summary.animationChannels = entry.stats.animation_channels;
    summary.animationKeys = entry.stats.animation_keys;
    summary.identityPresent =
        (entry.identity_flags & 1u) != 0u && entry.portrait_bytes != 0u;
    summary.rigMode = entry.rig_present != 0u ? entry.rig_mode + 1u : 0u;
    summary.rigReviewed =
        (entry.rig_flags & MDKR_MODERN_RIG_REVIEWED) != 0u;
    summary.rigRoles = entry.stats.rig_roles;
    summary.encodedTextureBytes = entry.stats.encoded_texture_bytes;
    summary.decodedTextureBytes = entry.stats.decoded_texture_bytes;
    summary.provenancePresent = entry.provenance_present != 0u;
    if (summary.provenancePresent) {
        summary.licenseSpdx = entry.license_spdx;
        summary.attribution = entry.attribution;
        summary.sourceUrl = entry.source_url;
    }
    for (size_t lod = 0u; lod < 4u; ++lod) {
        summary.lodVertices[lod] = entry.lod_vertices[lod];
        summary.lodTriangles[lod] = entry.lod_triangles[lod];
        summary.lodPrimitives[lod] = entry.lod_primitives[lod];
    }
    return summary;
}

CharacterCandidateIndex::Candidate nativeCharacterSummary(
    const MdkrModernCharacterInstallResult &result) {
    CharacterCandidateIndex::Candidate summary;
    summary.id = result.id;
    summary.displayName = result.display_name;
    summary.shortName = result.short_name;
    summary.narrationName = result.narration_name;
    summary.sortLabel = result.sort_label;
    summary.packageSha256 = result.package_sha256;
    summary.sourceDigest = result.source_digest;
    summary.donor = result.donor;
    summary.vehicleMask = result.vehicle_mask;
    summary.vertices = result.vertices;
    summary.triangles = result.triangles;
    summary.primitives = result.primitives;
    summary.lodLevels = result.lod_levels;
    summary.materials = result.materials;
    summary.textures = result.textures;
    summary.nodes = result.nodes;
    summary.skins = result.skins;
    summary.joints = result.joints;
    summary.animations = result.animations;
    summary.animationChannels = result.animation_channels;
    summary.animationKeys = result.animation_keys;
    summary.identityPresent = result.identity_present != 0u;
    summary.rigMode = result.rig_mode;
    summary.rigReviewed = result.rig_reviewed != 0u;
    summary.rigRoles = result.rig_roles;
    summary.encodedTextureBytes = result.encoded_texture_bytes;
    summary.decodedTextureBytes = result.decoded_texture_bytes;
    summary.provenancePresent = result.provenance_present != 0u;
    if (summary.provenancePresent) {
        summary.licenseSpdx = result.license_spdx;
        summary.attribution = result.attribution;
        summary.sourceUrl = result.source_url;
    }
    for (size_t lod = 0u; lod < 4u; ++lod) {
        summary.lodVertices[lod] = result.lod_vertices[lod];
        summary.lodTriangles[lod] = result.lod_triangles[lod];
        summary.lodPrimitives[lod] = result.lod_primitives[lod];
    }
    return summary;
}

bool characterPathHasExtension(const std::string &path,
                               const char        *extension) {
    const size_t length = std::strlen(extension);
    if (path.size() < length) return false;
    const size_t offset = path.size() - length;
    for (size_t index = 0u; index < length; ++index) {
        const unsigned char byte =
            static_cast<unsigned char>(path[offset + index]);
        if (static_cast<char>(std::tolower(byte)) != extension[index]) {
            return false;
        }
    }
    return true;
}

enum class CharacterSourceKind {
    Unknown,
    Package,
    Glb,
    Dae,
    Zip,
    Gltf,
    Fbx,
    Obj,
    Blend,
    Usd,
    NativeDcc,
};

CharacterSourceKind characterSourceKind(const std::string &path) {
    if (characterPathHasExtension(path, ".mdkrchar")) {
        return CharacterSourceKind::Package;
    }
    if (characterPathHasExtension(path, ".glb")) {
        return CharacterSourceKind::Glb;
    }
    if (characterPathHasExtension(path, ".dae")) {
        return CharacterSourceKind::Dae;
    }
    if (characterPathHasExtension(path, ".zip")) {
        return CharacterSourceKind::Zip;
    }
    if (characterPathHasExtension(path, ".gltf")) {
        return CharacterSourceKind::Gltf;
    }
    if (characterPathHasExtension(path, ".fbx")) {
        return CharacterSourceKind::Fbx;
    }
    if (characterPathHasExtension(path, ".obj")) {
        return CharacterSourceKind::Obj;
    }
    if (characterPathHasExtension(path, ".blend")) {
        return CharacterSourceKind::Blend;
    }
    if (characterPathHasExtension(path, ".usd") ||
        characterPathHasExtension(path, ".usda") ||
        characterPathHasExtension(path, ".usdc") ||
        characterPathHasExtension(path, ".usdz")) {
        return CharacterSourceKind::Usd;
    }
    if (characterPathHasExtension(path, ".ma") ||
        characterPathHasExtension(path, ".mb") ||
        characterPathHasExtension(path, ".max") ||
        characterPathHasExtension(path, ".c4d") ||
        characterPathHasExtension(path, ".3ds")) {
        return CharacterSourceKind::NativeDcc;
    }
    return CharacterSourceKind::Unknown;
}

bool characterSourceNeedsDccExport(CharacterSourceKind kind) {
    return kind == CharacterSourceKind::Gltf ||
        kind == CharacterSourceKind::Fbx ||
        kind == CharacterSourceKind::Obj ||
        kind == CharacterSourceKind::Blend ||
        kind == CharacterSourceKind::Usd ||
        kind == CharacterSourceKind::NativeDcc;
}

const char *characterSourceFormatName(CharacterSourceKind kind) {
    switch (kind) {
        case CharacterSourceKind::Gltf: return "glTF JSON";
        case CharacterSourceKind::Fbx: return "FBX";
        case CharacterSourceKind::Obj: return "OBJ";
        case CharacterSourceKind::Blend: return "Blender scene";
        case CharacterSourceKind::Usd: return "USD";
        case CharacterSourceKind::NativeDcc: return "native DCC scene";
        default: return "authoring source";
    }
}

std::string characterSourceExportGuidance(CharacterSourceKind kind) {
    std::string reason;
    switch (kind) {
        case CharacterSourceKind::Gltf:
            reason =
                "This JSON glTF may depend on loose buffers and images. Re-export or pack it as one binary GLB so the Workshop can authenticate every byte as a single source.";
            break;
        case CharacterSourceKind::Fbx:
            reason =
                "FBX interpretation varies by SDK and exporter. Open it in Blender, Maya, 3ds Max, or another trusted DCC and export the evaluated result instead of asking the game to guess at FBX semantics.";
            break;
        case CharacterSourceKind::Obj:
            reason =
                "OBJ carries geometry and loose material references but no usable character skin or animation. Import it into a DCC, rig and skin it, and author at least one fallback clip before export.";
            break;
        case CharacterSourceKind::Blend:
            reason =
                "A Blender scene is an editable project, not a portable runtime asset. Open it in the Blender version you trust and export only the intended character result.";
            break;
        case CharacterSourceKind::Usd:
            reason =
                "USD composition can resolve external layers, payloads, materials, and application-specific rig schemas. Flatten the intended character in a trusted DCC and export a self-contained runtime asset.";
            break;
        case CharacterSourceKind::NativeDcc:
            reason =
                "This native DCC scene requires its owning authoring application. Open it there and export the evaluated character; the game will never execute a project file or silently discard application-specific rig data.";
            break;
        default:
            return {};
    }
    return std::string(characterSourceFormatName(kind)) +
        " needs a GLB 2.0 export.\n\n" + reason +
        "\n\nExport checklist:\n"
        "1. Export glTF 2.0 Binary (.glb), with buffers and images embedded.\n"
        "2. Include the deforming meshes, armature/skin, materials, and only the clips the character should use. Bake procedural constraints into those clips.\n"
        "3. Use metres and glTF's +Y-up coordinate system. Do not destructively turn the character just for this game; declare its authored forward axis during Workshop intake.\n"
        "4. Apply or export triangulation consistently, verify normal/tangent direction, and keep every texture inside the GLB.\n"
        "5. Drop the exported GLB here. The Workshop will inventory it, preserve the original project, and require explicit mapping, licensing, review, and install steps.";
}

MdkrTextStateStorage characterRawDraftStorage() {
    return MdkrTextStateStorage{
        &g_characterRawDraftFileSpec,
        mdkr_text_state_file_read,
        mdkr_text_state_file_write,
    };
}

bool setCharacterRawEditorOpen(bool open) {
    g_characterRawEditorOpen = open;
    return AppConfig::persistResultApplied(AppConfig::setAndSave(
        "character_raw_editor_open", open ? "1" : "0"));
}

std::string newCharacterRawDraftId(const std::string &modelPath) {
    static uint64_t serial;
    std::string seed = modelPath;
    seed.push_back('\0');
    seed += std::to_string(static_cast<uint64_t>(std::time(nullptr)));
    seed.push_back('\0');
    seed += std::to_string(++serial);
    char digest[MDKR_SHA256_HEX_SIZE];
    mdkr_sha256_hex(seed.data(), seed.size(), digest);
    return std::string("raw-") + std::string(digest, 24u);
}

void applyCharacterRawDraft(const CharacterRawDraftStore::Draft *draft) {
    g_characterRawIntake = CharacterRawIntake{};
    CharacterRawIntake &intake = g_characterRawIntake;
    intake.loaded = true;
    g_characterRawIntakeTracePrinted = false;
    if (draft == nullptr) return;
    intake.draftId = draft->id;
    const auto copy = [](char *target, size_t capacity,
                         const std::string &value) {
        std::snprintf(target, capacity, "%s", value.c_str());
    };
    copy(intake.modelPath, sizeof(intake.modelPath), draft->modelPath);
    copy(intake.licensePath, sizeof(intake.licensePath), draft->licensePath);
    copy(intake.packageId, sizeof(intake.packageId), draft->packageId);
    copy(intake.displayName, sizeof(intake.displayName), draft->displayName);
    copy(intake.spdx, sizeof(intake.spdx), draft->spdx);
    copy(intake.attribution, sizeof(intake.attribution), draft->attribution);
    copy(intake.sourceUrl, sizeof(intake.sourceUrl), draft->sourceUrl);
    intake.donor = static_cast<int>(draft->donor);
    for (int vehicle = 0; vehicle < 3; ++vehicle) {
        intake.vehicles[vehicle] =
            (draft->vehicleMask & (1u << vehicle)) != 0u;
    }
    intake.sourceForward = static_cast<int>(draft->sourceForward);
    intake.targetHeight = draft->targetHeight;
    intake.savedMappingModelSha256 = draft->mappingModelSha256;
    intake.savedFallback = draft->fallback;
    intake.savedSeat = draft->seat;
    intake.savedHead = draft->head;
}

bool replaceCharacterRawDraftInventory(
    CharacterRawDraftStore::Inventory inventory) {
    std::sort(
        inventory.drafts.begin(), inventory.drafts.end(),
        [](const CharacterRawDraftStore::Draft &left,
           const CharacterRawDraftStore::Draft &right) {
            if (left.updatedUnix != right.updatedUnix) {
                return left.updatedUnix > right.updatedUnix;
            }
            return left.id < right.id;
        });
    if (!g_characterRawDraftsWritable ||
        !CharacterRawDraftStore::save(
            characterRawDraftStorage(), inventory,
            g_characterRawDraftError)) {
        return false;
    }
    g_characterRawDrafts = std::move(inventory);
    return true;
}

int legacyRawDraftInteger(const char *key, int fallback,
                          int minimum, int maximum) {
    const std::string text = AppConfig::get(key);
    if (text.empty()) return fallback;
    char *end = nullptr;
    errno = 0;
    const long value = std::strtol(text.c_str(), &end, 10);
    return errno == 0 && end != text.c_str() && *end == '\0' &&
                   value >= minimum && value <= maximum
        ? static_cast<int>(value) : fallback;
}

CharacterRawDraftStore::Draft legacyCharacterRawDraft() {
    CharacterRawDraftStore::Draft draft;
    draft.id = newCharacterRawDraftId(
        AppConfig::get("character_raw_intake_model"));
    const std::time_t now = std::time(nullptr);
    draft.updatedUnix = now >= 0 ? static_cast<uint64_t>(now) : 0u;
    draft.modelPath = AppConfig::get("character_raw_intake_model");
    draft.licensePath = AppConfig::get("character_raw_intake_license");
    draft.packageId = AppConfig::get("character_raw_intake_id");
    draft.displayName = AppConfig::get("character_raw_intake_display_name");
    draft.spdx = AppConfig::get("character_raw_intake_spdx");
    draft.attribution = AppConfig::get("character_raw_intake_attribution");
    draft.sourceUrl = AppConfig::get("character_raw_intake_source_url");
    draft.donor = static_cast<uint32_t>(legacyRawDraftInteger(
        "character_raw_intake_donor", 9, 0, 9));
    draft.vehicleMask = static_cast<uint32_t>(legacyRawDraftInteger(
        "character_raw_intake_vehicles", 7, 0, 7));
    draft.sourceForward = static_cast<uint32_t>(legacyRawDraftInteger(
        "character_raw_intake_forward", 0, 0, 3));
    const std::string height = AppConfig::get("character_raw_intake_height");
    if (!height.empty()) {
        char *end = nullptr;
        errno = 0;
        const float parsed = std::strtof(height.c_str(), &end);
        if (errno == 0 && end == height.c_str() + height.size() &&
            std::isfinite(parsed) && parsed >= 0.1f && parsed <= 10.0f) {
            draft.targetHeight = parsed;
        }
    }
    draft.mappingModelSha256 =
        AppConfig::get("character_raw_intake_mapping_sha256");
    draft.fallback = AppConfig::get("character_raw_intake_fallback");
    draft.seat = AppConfig::get("character_raw_intake_seat");
    draft.head = AppConfig::get("character_raw_intake_head");
    return draft;
}

void loadCharacterRawIntake() {
    if (g_characterRawDraftsLoaded) return;
    g_characterRawDraftsLoaded = true;
    CharacterRawDraftStore::Inventory inventory;
    const CharacterRawDraftStore::LoadResult result =
        CharacterRawDraftStore::load(
            characterRawDraftStorage(), inventory,
            g_characterRawDraftError);
    g_characterRawDraftsWritable =
        result == CharacterRawDraftStore::LoadResult::Loaded ||
        result == CharacterRawDraftStore::LoadResult::Missing;
    if (!g_characterRawDraftsWritable) {
        g_characterRawIntake.loaded = true;
        return;
    }
    g_characterRawDrafts = std::move(inventory);

    // Version-zero migration is committed to the transactional inventory
    // before the old independent preference keys are forgotten.
    if (result == CharacterRawDraftStore::LoadResult::Missing &&
        !AppConfig::get("character_raw_intake_model").empty()) {
        CharacterRawDraftStore::Draft legacy = legacyCharacterRawDraft();
        CharacterRawDraftStore::Inventory replacement;
        std::string error;
        if (CharacterRawDraftStore::upsert(replacement, legacy, error)) {
            replacement.selectedId = legacy.id;
        }
        if (!error.empty() ||
            !replaceCharacterRawDraftInventory(std::move(replacement))) {
            if (!error.empty()) g_characterRawDraftError = error;
            g_characterRawIntake.loaded = true;
            return;
        }
        (void)AppConfig::erasePrefix("character_raw_intake_");
        (void)AppConfig::save();
    }
    applyCharacterRawDraft(CharacterRawDraftStore::find(
        g_characterRawDrafts, g_characterRawDrafts.selectedId));
    const std::string savedOpen = AppConfig::get(
        "character_raw_editor_open");
    g_characterRawEditorOpen =
        !g_characterRawDrafts.selectedId.empty() && savedOpen != "0";
}

CharacterRawDraftStore::Draft captureCharacterRawDraft() {
    const CharacterRawIntake &intake = g_characterRawIntake;
    CharacterRawDraftStore::Draft draft;
    draft.id = intake.draftId;
    const CharacterRawDraftStore::Draft *saved =
        CharacterRawDraftStore::find(g_characterRawDrafts, draft.id);
    const std::time_t now = std::time(nullptr);
    draft.updatedUnix = now >= 0
        ? static_cast<uint64_t>(now)
        : (saved != nullptr ? saved->updatedUnix : 0u);
    draft.modelPath = intake.modelPath;
    draft.licensePath = intake.licensePath;
    draft.packageId = intake.packageId;
    draft.displayName = intake.displayName;
    draft.spdx = intake.spdx;
    draft.attribution = intake.attribution;
    draft.sourceUrl = intake.sourceUrl;
    draft.donor = static_cast<uint32_t>(intake.donor);
    draft.vehicleMask = 0u;
    for (int vehicle = 0; vehicle < 3; ++vehicle) {
        if (intake.vehicles[vehicle]) draft.vehicleMask |= 1u << vehicle;
    }
    draft.sourceForward = static_cast<uint32_t>(intake.sourceForward);
    draft.targetHeight = intake.targetHeight;
    draft.mappingModelSha256 = intake.savedMappingModelSha256;
    draft.fallback = intake.savedFallback;
    draft.seat = intake.savedSeat;
    draft.head = intake.savedHead;
    if (intake.inspected) {
        const auto selectedName = [](const std::vector<std::string> &choices,
                                     int selected) -> std::string {
            return selected >= 0 && selected < static_cast<int>(choices.size())
                ? choices[static_cast<size_t>(selected)] : "";
        };
        draft.mappingModelSha256 = intake.inventory.modelSha256;
        draft.fallback = selectedName(intake.inventory.clips, intake.fallback);
        draft.seat = selectedName(intake.inventory.nodes, intake.seat);
        draft.head = selectedName(intake.inventory.nodes, intake.head);
    }
    return draft;
}

bool saveCharacterRawIntake() {
    if (g_characterRawIntake.draftId.empty() ||
        !g_characterRawDraftsWritable) return false;
    CharacterRawDraftStore::Inventory replacement = g_characterRawDrafts;
    CharacterRawDraftStore::Draft draft = captureCharacterRawDraft();
    std::string error;
    if (!CharacterRawDraftStore::upsert(replacement, draft, error)) {
        g_characterRawDraftError = error;
        return false;
    }
    replacement.selectedId = draft.id;
    if (!replaceCharacterRawDraftInventory(std::move(replacement))) {
        return false;
    }
    g_characterRawIntake.savedMappingModelSha256 =
        draft.mappingModelSha256;
    g_characterRawIntake.savedFallback = draft.fallback;
    g_characterRawIntake.savedSeat = draft.seat;
    g_characterRawIntake.savedHead = draft.head;
    return true;
}

void removeDisposableRawCandidate(const std::string &draftId) {
    const bool matchingRawCandidate =
        g_characterImportCandidate.disposableRawCandidate &&
        (draftId.empty() ||
         g_characterImportCandidate.rawDraftId == draftId);
    if (matchingRawCandidate) {
        if (!g_characterImportCandidate.packagePath.empty()) {
            (void)mdkr_remove_utf8(
                g_characterImportCandidate.packagePath.c_str());
        }
        g_characterImportCandidate = CharacterImportCandidate{};
    } else if (draftId.empty() && g_characterImportCandidate.ready) {
        // Selecting a different source closes an ordinary mutation-free
        // review as well. Its caller-owned package bytes remain untouched.
        g_characterImportCandidate = CharacterImportCandidate{};
    }
    if ((draftId.empty() || matchingRawCandidate) &&
        !g_characterRegistryDirectory.empty()) {
        const std::string candidate = g_characterRegistryDirectory +
            "/.launcher-character-raw-candidate.mdkrchar";
        (void)mdkr_remove_utf8(candidate.c_str());
    }
}

bool activateCharacterRawDraft(std::string draftId) {
    const CharacterRawDraftStore::Draft *draft =
        CharacterRawDraftStore::find(g_characterRawDrafts, draftId);
    if (draft == nullptr) return false;
    CharacterRawDraftStore::Inventory replacement = g_characterRawDrafts;
    replacement.selectedId = draftId;
    if (!replaceCharacterRawDraftInventory(std::move(replacement))) {
        return false;
    }
    removeDisposableRawCandidate("");
    applyCharacterRawDraft(CharacterRawDraftStore::find(
        g_characterRawDrafts, draftId));
    (void)setCharacterRawEditorOpen(true);
    std::snprintf(g_characterImportPath, sizeof(g_characterImportPath), "%s",
                  g_characterRawIntake.modelPath);
    return true;
}

bool deleteCharacterRawDraft(std::string draftId) {
    const CharacterRawDraftStore::Draft *current =
        CharacterRawDraftStore::find(g_characterRawDrafts, draftId);
    if (current == nullptr) return false;
    const std::string modelPath = current->modelPath;
    CharacterRawDraftStore::Inventory replacement = g_characterRawDrafts;
    if (!CharacterRawDraftStore::erase(replacement, draftId)) return false;
    if (!replaceCharacterRawDraftInventory(std::move(replacement))) {
        return false;
    }
    removeDisposableRawCandidate(draftId);
    applyCharacterRawDraft(CharacterRawDraftStore::find(
        g_characterRawDrafts, g_characterRawDrafts.selectedId));
    (void)setCharacterRawEditorOpen(
        !g_characterRawDrafts.selectedId.empty());
    if (modelPath == g_characterImportPath) {
        if (g_characterRawIntake.modelPath[0] != '\0') {
            std::snprintf(g_characterImportPath,
                          sizeof(g_characterImportPath), "%s",
                          g_characterRawIntake.modelPath);
        } else {
            g_characterImportPath[0] = '\0';
        }
    }
    return true;
}

bool clearCharacterRawIntake() {
    return !g_characterRawIntake.draftId.empty() &&
        deleteCharacterRawDraft(g_characterRawIntake.draftId);
}

bool beginCharacterRawDraft(const std::string &modelPath) {
    loadCharacterRawIntake();
    if (!g_characterRawDraftsWritable) return false;
    if (modelPath == g_characterRawIntake.modelPath &&
        CharacterRawDraftStore::find(
            g_characterRawDrafts, g_characterRawIntake.draftId) != nullptr) {
        return activateCharacterRawDraft(g_characterRawIntake.draftId);
    }
    const auto existing = std::find_if(
        g_characterRawDrafts.drafts.begin(),
        g_characterRawDrafts.drafts.end(),
        [&modelPath](const CharacterRawDraftStore::Draft &draft) {
            return draft.modelPath == modelPath;
        });
    if (existing != g_characterRawDrafts.drafts.end()) {
        return activateCharacterRawDraft(existing->id);
    }
    CharacterRawDraftStore::Draft draft;
    for (size_t attempt = 0u;
         attempt <= CharacterRawDraftStore::kMaximumDrafts; ++attempt) {
        draft.id = newCharacterRawDraftId(modelPath);
        if (CharacterRawDraftStore::find(g_characterRawDrafts, draft.id) ==
            nullptr) break;
    }
    if (CharacterRawDraftStore::find(g_characterRawDrafts, draft.id) !=
        nullptr) {
        g_characterRawDraftError =
            "Could not allocate a unique raw authoring draft id.";
        return false;
    }
    const std::time_t now = std::time(nullptr);
    draft.updatedUnix = now >= 0 ? static_cast<uint64_t>(now) : 0u;
    draft.modelPath = modelPath;
    CharacterRawDraftStore::Inventory replacement = g_characterRawDrafts;
    std::string error;
    if (!CharacterRawDraftStore::upsert(replacement, draft, error)) {
        g_characterRawDraftError = error;
        return false;
    }
    replacement.selectedId = draft.id;
    if (!replaceCharacterRawDraftInventory(std::move(replacement))) {
        return false;
    }
    removeDisposableRawCandidate("");
    applyCharacterRawDraft(CharacterRawDraftStore::find(
        g_characterRawDrafts, draft.id));
    (void)setCharacterRawEditorOpen(true);
    return true;
}

bool duplicateCharacterRawDraft() {
    if (!saveCharacterRawIntake()) return false;
    const CharacterRawDraftStore::Draft *source =
        CharacterRawDraftStore::find(
            g_characterRawDrafts, g_characterRawIntake.draftId);
    if (source == nullptr) return false;
    CharacterRawDraftStore::Draft duplicate = *source;
    for (size_t attempt = 0u;
         attempt <= CharacterRawDraftStore::kMaximumDrafts; ++attempt) {
        duplicate.id = newCharacterRawDraftId(source->modelPath);
        if (CharacterRawDraftStore::find(
                g_characterRawDrafts, duplicate.id) == nullptr) break;
    }
    if (CharacterRawDraftStore::find(g_characterRawDrafts, duplicate.id) !=
        nullptr) {
        g_characterRawDraftError =
            "Could not allocate a unique id for the duplicated raw draft.";
        return false;
    }
    const std::time_t now = std::time(nullptr);
    duplicate.updatedUnix = now >= 0
        ? static_cast<uint64_t>(now) : source->updatedUnix;
    CharacterRawDraftStore::Inventory replacement = g_characterRawDrafts;
    std::string error;
    if (!CharacterRawDraftStore::upsert(replacement, duplicate, error)) {
        g_characterRawDraftError = error;
        return false;
    }
    replacement.selectedId = duplicate.id;
    if (!replaceCharacterRawDraftInventory(std::move(replacement))) {
        return false;
    }
    removeDisposableRawCandidate("");
    applyCharacterRawDraft(CharacterRawDraftStore::find(
        g_characterRawDrafts, duplicate.id));
    (void)setCharacterRawEditorOpen(true);
    return true;
}

std::string characterLauncherHex(const std::string &value) {
    static const char digits[] = "0123456789abcdef";
    std::string encoded(value.size() * 2u, '0');
    for (size_t index = 0u; index < value.size(); ++index) {
        const unsigned char byte = static_cast<unsigned char>(value[index]);
        encoded[index * 2u] = digits[byte >> 4u];
        encoded[index * 2u + 1u] = digits[byte & 0xFu];
    }
    return encoded;
}

int characterChoiceIndex(const std::vector<std::string> &choices,
                         const std::string              &choice) {
    const auto found = std::find(choices.begin(), choices.end(), choice);
    return found == choices.end()
        ? -1 : static_cast<int>(found - choices.begin());
}

bool stageCharacterPackage(const std::string &path);

bool inspectCharacterRawGlb(const std::string &path) {
    if (g_characterRegistryDirectory.empty()) refreshCharacterRegistry();
    if (g_characterRegistryDirectory.empty()) return false;
    CharacterRawIntake &intake = g_characterRawIntake;
    intake.inspected = false;
    intake.inventory = CharacterRawIntakeIndex::Inventory{};
    intake.fallback = -1;
    intake.seat = -1;
    intake.head = -1;
    const std::string indexPath = g_characterRegistryDirectory +
        "/.launcher-character-glb-intake.tsv";
    (void)mdkr_remove_utf8(indexPath.c_str());
    const bool indexed = runCharacterManager(
        "write-raw-glb-index", {path, indexPath}, false);
    const std::string text = indexed
        ? readCharacterManagerResult(indexPath) : "";
    (void)mdkr_remove_utf8(indexPath.c_str());
    CharacterRawIntakeIndex::Inventory inventory;
    if (!indexed || !CharacterRawIntakeIndex::parse(text, inventory)) {
        if (indexed) {
            g_characterManagerReport =
                "The raw GLB inventory was malformed; no authoring action was enabled.";
        }
        return false;
    }
    std::snprintf(intake.modelPath, sizeof(intake.modelPath), "%s",
                  path.c_str());
    intake.inventory = std::move(inventory);
    const bool sameFingerprint = intake.savedMappingModelSha256 ==
        intake.inventory.modelSha256;
    const auto restoredChoice = [sameFingerprint](
                                   const std::string &savedMapping,
                                   const std::vector<std::string> &choices,
                                   const std::string &inferred) {
        const std::string saved = sameFingerprint ? savedMapping : "";
        const int restored = characterChoiceIndex(choices, saved);
        return restored >= 0 ? restored
                             : characterChoiceIndex(choices, inferred);
    };
    intake.fallback = restoredChoice(
        intake.savedFallback, intake.inventory.clips,
        intake.inventory.fallback);
    intake.seat = restoredChoice(
        intake.savedSeat, intake.inventory.nodes,
        intake.inventory.seat);
    intake.head = restoredChoice(
        intake.savedHead, intake.inventory.nodes,
        intake.inventory.head);
    intake.inspected = true;
    if (!saveCharacterRawIntake()) {
        intake.inspected = false;
        g_characterManagerReport =
            "The GLB passed inspection, but its source-bound mappings could not be committed to the raw draft inventory: " +
            g_characterRawDraftError;
        return false;
    }
    return true;
}

bool buildCharacterRawGlbCandidate() {
    CharacterRawIntake &intake = g_characterRawIntake;
    if (!saveCharacterRawIntake()) return false;
    if (!intake.inspected || intake.fallback < 0 || intake.seat < 0 ||
        intake.head < 0 || intake.fallback >=
            static_cast<int>(intake.inventory.clips.size()) ||
        intake.seat >= static_cast<int>(intake.inventory.nodes.size()) ||
        intake.head >= static_cast<int>(intake.inventory.nodes.size())) {
        return false;
    }
    static const char *donors[] = {
        "krunch", "bumper", "tiptup", "conker", "timber",
        "banjo", "drumstick", "pipsy", "tt", "diddy",
    };
    static const char *forwards[] = {"+z", "-z", "+x", "-x"};
    int vehicleMask = 0;
    for (int vehicle = 0; vehicle < 3; ++vehicle) {
        if (intake.vehicles[vehicle]) vehicleMask |= 1 << vehicle;
    }
    if (intake.donor < 0 || intake.donor >= 10 ||
        intake.sourceForward < 0 || intake.sourceForward >= 4 ||
        vehicleMask == 0) return false;
    std::vector<std::string> arguments = {
        characterLauncherHex(intake.modelPath),
        characterLauncherHex(intake.licensePath),
        characterLauncherHex(intake.packageId),
        characterLauncherHex(intake.displayName),
        characterLauncherHex(intake.spdx),
        characterLauncherHex(intake.attribution),
        characterLauncherHex(intake.sourceUrl),
        characterLauncherHex(donors[intake.donor]),
        characterLauncherHex(forwards[intake.sourceForward]),
        characterLauncherHex(intake.inventory.clips[
            static_cast<size_t>(intake.fallback)]),
        characterLauncherHex(intake.inventory.nodes[
            static_cast<size_t>(intake.seat)]),
        characterLauncherHex(intake.inventory.nodes[
            static_cast<size_t>(intake.head)]),
        intake.inventory.modelSha256,
        std::to_string(vehicleMask),
        std::to_string(intake.targetHeight),
    };
    if (!runCharacterManager("build-raw-glb", arguments, false)) return false;
    const std::string candidate = g_characterRegistryDirectory +
        "/.launcher-character-raw-candidate.mdkrchar";
    if (!stageCharacterPackage(candidate)) return false;
    g_characterImportCandidate.disposableRawCandidate = true;
    g_characterImportCandidate.rawDraftId = intake.draftId;
    return true;
}

bool stageCharacterPackage(const std::string &path) {
    MdkrModernCharacterInstallResult nativeResult{};
    CharacterImportCandidate staged;
    if (g_characterRegistryDirectory.empty()) refreshCharacterRegistry();
    if (g_characterRegistryDirectory.empty()) {
        g_characterManagerReport =
            "No writable character directory is available.";
        return false;
    }
    if (mdkr_modern_character_inspect_portable(path.c_str(), &nativeResult)) {
        staged.next = nativeCharacterSummary(nativeResult);
        staged.portable = true;
        g_characterManagerReport = nativeResult.message;
    } else if (nativeResult.needs_compiler != 0) {
        const std::string indexPath = g_characterRegistryDirectory +
            "/.launcher-character-candidate.tsv";
        (void)mdkr_remove_utf8(indexPath.c_str());
        const bool indexed = runCharacterManager(
            "write-candidate-index", {path, indexPath}, false);
        const std::string index = indexed
            ? readCharacterManagerResult(indexPath) : "";
        (void)mdkr_remove_utf8(indexPath.c_str());
        if (!indexed ||
            !CharacterCandidateIndex::parse(index, staged.next)) {
            if (indexed) {
                g_characterManagerReport =
                    "The compiler candidate summary was malformed; review and install remain disabled.";
            }
            return false;
        }
        staged.portable = false;
    } else {
        g_characterManagerReport = nativeResult.message;
        return false;
    }
    const int installedIndex = mdkr_modern_character_registry_find(
        &g_characterRegistry, staged.next.id.c_str());
    const MdkrModernCharacterEntry *installed =
        mdkr_modern_character_registry_entry(&g_characterRegistry,
                                              installedIndex);
    if (installed != nullptr) {
        staged.installed = true;
        staged.installedEnabled = installed->enabled != 0u;
        staged.current = installedCharacterSummary(*installed);
        staged.reviewedInstalledDigest = staged.current.sourceDigest;
    }
    staged.packagePath = path;
    staged.ready = true;
    g_characterImportCandidate = std::move(staged);
    return true;
}

bool installReviewedCharacterPackage() {
    if (!g_characterImportCandidate.ready) return false;
    const CharacterImportCandidate reviewed = g_characterImportCandidate;
    bool installed = false;
    if (reviewed.portable) {
        MdkrModernCharacterInstallResult result{};
        installed = mdkr_modern_character_install_portable_reviewed(
            reviewed.packagePath.c_str(), g_characterRegistryDirectory.c_str(),
            reviewed.next.packageSha256.c_str(),
            reviewed.reviewedInstalledDigest.c_str(), &result) != 0;
        g_characterManagerReport = result.message;
        if (installed) refreshCharacterRegistry();
    } else {
        installed = runCharacterManager(
            "install-reviewed",
            {reviewed.packagePath, reviewed.next.packageSha256,
             reviewed.installed ? reviewed.reviewedInstalledDigest : "absent"});
    }
    if (!installed) {
        /* A failed commit never remains armed: file bytes or installed state
         * may have changed, and retrying requires a fresh visible review. */
        g_characterImportCandidate = CharacterImportCandidate{};
        return false;
    }
    if (reviewed.disposableRawCandidate) {
        (void)mdkr_remove_utf8(reviewed.packagePath.c_str());
        if (reviewed.rawDraftId.empty() ||
            !deleteCharacterRawDraft(reviewed.rawDraftId)) {
            g_characterManagerReport +=
                " The character was installed, but its completed local raw authoring draft could not be removed.";
        }
        (void)setCharacterRawEditorOpen(false);
    }
    g_characterWorkshopSelection = reviewed.next.id;
    g_characterWorkshopSelectionLoaded = true;
    AppConfig::set("character_workshop_last_selected",
                   g_characterWorkshopSelection);
    (void)AppConfig::save();
    g_characterImportCandidate = CharacterImportCandidate{};
    g_characterImportPath[0] = '\0';
    return true;
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

bool setCharacterPackageEnabled(const std::string &id, bool enabled) {
    MdkrModernCharacterInstallResult result{};
    if (g_characterRegistryDirectory.empty()) refreshCharacterRegistry();
    if (!g_characterRegistryDirectory.empty() &&
        mdkr_modern_character_set_enabled(
            id.c_str(), g_characterRegistryDirectory.c_str(),
            enabled ? 1 : 0, &result)) {
        g_characterManagerReport = result.message;
        refreshCharacterRegistry();
        return true;
    }
    g_characterManagerReport = result.message;
    return false;
}

AppConfig::PersistResult forgetCharacterPackagePreferences(
    const std::string &id) {
    for (int slot = 0; slot < 4; ++slot) {
        const std::string slotKey = "custom_character_p" +
            std::to_string(slot + 1);
        if (AppConfig::get(slotKey) == id) AppConfig::set(slotKey, "");
    }
    (void)AppConfig::erasePrefix(
        "custom_character_profile_" + id + "_");
    g_characterIdentityEdits.erase(id);
    g_characterProfileEdits.erase(id);
    g_characterRigEdits.erase(id);
    g_characterEditHistories.erase(id);
    g_characterTuning.erase(id);
    g_characterAssemblyPlayers.erase(id);
    g_characterTestPlayers.erase(id);
    g_characterTestPoses.erase(id);
    g_characterTestPosePhases.erase(id);
    g_characterTestViewYawDegrees.erase(id);
    g_characterTestViewPitchDegrees.erase(id);
    g_characterTestLighting.erase(id);
    g_characterCaptureEdits.erase(id);
    g_characterVisualCaptures.erase(id);
    g_characterPendingPortraitSources.erase(id);
    g_characterPoseInspectionTracePackages.erase(id);
    g_characterPreviewResults.erase(id);
    g_characterTestEvidenceSelectedCell.erase(id);
    g_characterActiveDrafts.erase(id);
    g_characterDraftReviews.erase(id);
    g_characterPendingDraftFit.erase(id);
    if (g_characterDraftNameOwner == id) {
        g_characterDraftNameOwner.clear();
        g_characterDraftName[0] = '\0';
    }
    if (g_characterWorkshopSelection == id) {
        g_characterWorkshopSelection.clear();
        AppConfig::set("character_workshop_last_selected", "");
    }
    return AppConfig::save();
}

size_t characterPackageDraftCount(const std::string &id) {
    loadCharacterDraftInventory();
    return static_cast<size_t>(std::count_if(
        g_characterDrafts.drafts.begin(), g_characterDrafts.drafts.end(),
        [&id](const CharacterDraftStore::Draft &draft) {
            return draft.packageId == id;
        }));
}

bool forgetCharacterPackageDrafts(const std::string &id) {
    loadCharacterDraftInventory();
    if (!g_characterDraftsWritable) return false;
    CharacterDraftStore::Inventory replacement = g_characterDrafts;
    replacement.drafts.erase(
        std::remove_if(
            replacement.drafts.begin(), replacement.drafts.end(),
            [&id](const CharacterDraftStore::Draft &draft) {
                return draft.packageId == id;
            }),
        replacement.drafts.end());
    return replaceCharacterDraftInventory(std::move(replacement));
}

size_t characterPackageTestEvidenceCount(const std::string &id) {
    loadCharacterTestEvidence();
    return static_cast<size_t>(std::count_if(
        g_characterTestEvidence.records.begin(),
        g_characterTestEvidence.records.end(),
        [&id](const CharacterTestEvidenceStore::Evidence &evidence) {
            return evidence.packageId == id;
        }));
}

bool forgetCharacterPackageTestEvidence(const std::string &id) {
    loadCharacterTestEvidence();
    if (!g_characterTestEvidenceWritable) return false;
    CharacterTestEvidenceStore::Inventory replacement =
        g_characterTestEvidence;
    (void)CharacterTestEvidenceStore::erasePackage(replacement, id);
    if (!replaceCharacterTestEvidence(std::move(replacement))) return false;
    g_characterTestEvidenceSelectedCell.erase(id);
    return true;
}

void refreshCharacterRegistry() {
    char directory[MDKR_MODERN_CHARACTER_PATH_MAX];
    mdkr_modern_character_registry_shutdown(&g_characterRegistry);
    /* Identity, donor, rig, or source changes invalidate every session-local
     * measurement associated with the prior registry snapshot. */
    g_characterPreviewResults.clear();
    g_characterRevisionInventories.clear();
    g_characterRegistryDirectory.clear();
    if (mdkr_user_characters_directory(directory, sizeof(directory))) {
        g_characterRegistryDirectory = directory;
        (void)mdkr_modern_character_registry_init_inventory(
            &g_characterRegistry, directory);
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

bool donorProfilesAvailable() {
    return g_donorGameplayProfiles.available == 1u &&
        g_donorGameplayProfiles.version ==
            MDKR_DONOR_GAMEPLAY_PROFILE_VERSION &&
        g_donorGameplayProfiles.donor_count ==
            MDKR_DONOR_GAMEPLAY_PROFILE_COUNT;
}

std::string donorChoiceLabel(uint32_t donor) {
    if (!donorProfilesAvailable() ||
        donor >= MDKR_DONOR_GAMEPLAY_PROFILE_COUNT) {
        return donorName(donor);
    }
    const MdkrDonorGameplayProfile &profile =
        g_donorGameplayProfiles.donor[donor];
    char label[128];
    std::snprintf(label, sizeof(label), "%s  ·  W %.3f  ·  H %.3f",
                  donorName(donor), static_cast<double>(profile.weight),
                  static_cast<double>(profile.handling));
    return label;
}

void donorMetricRange(float MdkrDonorGameplayProfile::*member,
                      float &minimum, float &maximum) {
    minimum = g_donorGameplayProfiles.donor[0].*member;
    maximum = minimum;
    for (uint32_t donor = 1u;
         donor < MDKR_DONOR_GAMEPLAY_PROFILE_COUNT; ++donor) {
        const float value = g_donorGameplayProfiles.donor[donor].*member;
        minimum = std::min(minimum, value);
        maximum = std::max(maximum, value);
    }
}

void drawDonorMetricRow(const char *label, float value,
                        float minimum, float maximum) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(label);
    ImGui::TableNextColumn();
    ImGui::Text("%.4f", static_cast<double>(value));
    ImGui::TableNextColumn();
    const float range = maximum - minimum;
    const float normalized = range > 0.0f
        ? std::clamp((value - minimum) / range, 0.0f, 1.0f) : 0.5f;
    char overlay[96];
    std::snprintf(overlay, sizeof(overlay), "roster %.4f – %.4f",
                  static_cast<double>(minimum),
                  static_cast<double>(maximum));
    ImGui::ProgressBar(normalized, ImVec2(-1.0f, 0.0f), overlay);
}

void drawDonorGameplayComparison(uint32_t donor, uint32_t &vehicle) {
    if (!donorProfilesAvailable() ||
        donor >= MDKR_DONOR_GAMEPLAY_PROFILE_COUNT) {
        if (!g_donorGameplayProfilesUnavailableReason.empty()) {
            ui::TextSubtleWrapped(
                "Exact comparison unavailable: %s. Donor selection and "
                "package saving remain available.",
                g_donorGameplayProfilesUnavailableReason.c_str());
        } else {
            ui::TextSubtleWrapped(
                "Select and verify a supported base ROM on the Play page to "
                "compare exact built-in coefficients. Donor selection and "
                "package saving remain available without this optional "
                "evidence view.");
        }
        return;
    }
    const MdkrDonorGameplayProfile &profile =
        g_donorGameplayProfiles.donor[donor];
    ui::TextSubtleWrapped(
        "Exact values from the verified base ROM. Relative bars show where "
        "this profile sits within the built-in roster; they do not rank, copy, "
        "or alter gameplay data.");
    float weightMinimum = 0.0f;
    float weightMaximum = 0.0f;
    float handlingMinimum = 0.0f;
    float handlingMaximum = 0.0f;
    donorMetricRange(&MdkrDonorGameplayProfile::weight,
                     weightMinimum, weightMaximum);
    donorMetricRange(&MdkrDonorGameplayProfile::handling,
                     handlingMinimum, handlingMaximum);
    if (ImGui::BeginTable(
            "##donor-gameplay-comparison", 3,
            ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg |
            ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Coefficient");
        ImGui::TableSetupColumn("Exact value");
        ImGui::TableSetupColumn("Built-in roster range");
        ImGui::TableHeadersRow();
        drawDonorMetricRow("Effective weight", profile.weight,
                           weightMinimum, weightMaximum);
        drawDonorMetricRow("Handling", profile.handling,
                           handlingMinimum, handlingMaximum);
        ImGui::EndTable();
    }

    ui::Gap(ui::kGapS);
    ImGui::TextUnformatted("Acceleration curve by vehicle");
    static const char *vehicleNames[MDKR_DONOR_VEHICLE_COUNT] = {
        "Car", "Hovercraft", "Plane",
    };
    if (vehicle >= MDKR_DONOR_VEHICLE_COUNT) {
        vehicle = MDKR_DONOR_VEHICLE_CAR;
    }
    for (uint32_t candidate = 0u; candidate < MDKR_DONOR_VEHICLE_COUNT;
         ++candidate) {
        if (candidate != 0u) ImGui::SameLine();
        const std::string label = std::string(vehicleNames[candidate]) +
            "##donor-curve-vehicle-" + std::to_string(candidate);
        int selectedVehicle = static_cast<int>(vehicle);
        if (ImGui::RadioButton(label.c_str(), &selectedVehicle,
                               static_cast<int>(candidate))) {
            vehicle = static_cast<uint32_t>(selectedVehicle);
        }
        ui::SpeakFocusedItem(
            vehicleNames[candidate],
            candidate == vehicle ? "selected" : "not selected",
            "Choose which retail vehicle's exact acceleration curve to compare.");
    }
    ui::TextSubtleWrapped(
        "The game interpolates these 14 authored multipliers across its "
        "clamped speed indices 0–13. The plot uses the whole roster's range, "
        "so switching profiles remains directly comparable.");
    float curveMinimum =
        g_donorGameplayProfiles.donor[0].acceleration[vehicle][0];
    float curveMaximum = curveMinimum;
    for (uint32_t candidate = 0u;
         candidate < MDKR_DONOR_GAMEPLAY_PROFILE_COUNT; ++candidate) {
        for (uint32_t sample = 0u;
             sample < MDKR_DONOR_ACCELERATION_SAMPLES; ++sample) {
            const float value = g_donorGameplayProfiles.donor[candidate]
                .acceleration[vehicle][sample];
            curveMinimum = std::min(curveMinimum, value);
            curveMaximum = std::max(curveMaximum, value);
        }
    }
    ImGui::PlotLines("##donor-acceleration-curve",
                     profile.acceleration[vehicle],
                     MDKR_DONOR_ACCELERATION_SAMPLES, 0, nullptr,
                     curveMinimum, curveMaximum,
                     ImVec2(-1.0f, 92.0f));
    if (ImGui::IsItemHovered()) {
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        const ImVec2 min = ImGui::GetItemRectMin();
        const ImVec2 max = ImGui::GetItemRectMax();
        const float width = std::max(max.x - min.x, 1.0f);
        const int sample = std::clamp(
            static_cast<int>(((mouse.x - min.x) / width) *
                             MDKR_DONOR_ACCELERATION_SAMPLES),
            0, static_cast<int>(MDKR_DONOR_ACCELERATION_SAMPLES) - 1);
        ImGui::SetTooltip("Speed index %d\nExact multiplier %.4f", sample,
                          static_cast<double>(
                              profile.acceleration[vehicle][sample]));
    }
    const bool exactValuesOpen = ImGui::TreeNode("Exact 14-sample values");
    ui::SpeakFocusedItem(
        "Exact acceleration values",
        exactValuesOpen ? "expanded" : "collapsed",
        "Expand to read all 14 speed-index multipliers as text.");
    if (exactValuesOpen) {
        if (ImGui::BeginTable("##donor-acceleration-values", 2,
                              ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("Speed index");
            ImGui::TableSetupColumn("Multiplier");
            ImGui::TableHeadersRow();
            for (uint32_t sample = 0u;
                 sample < MDKR_DONOR_ACCELERATION_SAMPLES; ++sample) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Text("%u", sample);
                ImGui::TableNextColumn();
                ImGui::Text("%.4f", static_cast<double>(
                    profile.acceleration[vehicle][sample]));
            }
            ImGui::EndTable();
        }
        ImGui::TreePop();
    }
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
                             int players,
                             MdkrCharacterPreviewPose pose =
                                 MDKR_CHARACTER_PREVIEW_POSE_LIVE,
                             unsigned posePhaseMilli = 0u,
                             int viewYawDegrees = 0,
                             int viewPitchDegrees = 0,
                             MdkrWorkshopPreviewLighting lighting =
                                 MDKR_WORKSHOP_PREVIEW_LIGHTING_NEUTRAL,
                             const char *capturePng = nullptr);

bool drawCharacterRigStudio(const MdkrModernCharacterEntry *entry) {
    CharacterRigEdit &edit = loadCharacterRigEdit(entry);
    if (!edit.error.empty()) {
        ImGui::TextColored(AppTheme::bad(), "%s", edit.error.c_str());
        return false;
    }
    CharacterHistoryFrame history = beginCharacterHistory(
        entry, CharacterHistoryTool::Rig);
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
    const bool stagingDraft = g_characterActiveDrafts.find(entry->id) !=
        g_characterActiveDrafts.end();
    if (!canSave || stagingDraft) ImGui::BeginDisabled();
    bool saved = false;
    if (ImGui::Button("Save rig revision")) {
        saved = reviseCharacterRig(entry->id, characterRigDraftJson(edit));
        setStatus(
            saved ? "Rig map compiled, validated, and activated."
                  : "Rig revision failed; the active character was not changed.",
            saved ? AppTheme::good() : AppTheme::bad());
    }
    if (!canSave || stagingDraft) ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Discard draft")) {
        g_characterRigEdits.erase(entry->id);
        setStatus("Rig draft restored from the active package.",
                  AppTheme::subtle());
        finishCharacterHistory(entry, history);
        return false;
    }
    if (stagingDraft) {
        ui::TextSubtleWrapped(
            "Rig changes are part of the resumed named draft. Build from Named drafts to compile identity, profile, and rig as one source revision; this panel cannot publish a partial revision while that draft is open.");
    }
    if (!saved) finishCharacterHistory(entry, history);
    return saved;
}

bool characterPreviewFitDiagnosticsValid(
    const MdkrCharacterPreviewResult &result);
MdkrCharacterPreviewResult characterPreviewResultFromEvidence(
    const CharacterTestEvidenceStore::Evidence &evidence);
void drawCharacterFitDiagnostics(
    const MdkrCharacterPreviewResult &result, bool compact);

bool characterTestEvidenceMatchesFit(
    const MdkrModernCharacterEntry *entry,
    const CharacterTuningEdit &tuning,
    const CharacterTestEvidenceStore::Evidence &evidence) {
    if (entry == nullptr || evidence.context < 1u || evidence.context > 4u) {
        return false;
    }
    return evidence.sourceSha256 == characterDigestHex(entry->source_sha256) &&
           evidence.fitSha256 == characterFitReviewSignature(
               entry, tuning, evidence.context - 1u);
}

bool characterPreviewSessionMatchesFit(
    const MdkrModernCharacterEntry *entry,
    const CharacterTuningEdit &tuning,
    MdkrCharacterPreviewContext context,
    const CharacterPreviewSessionResult &session) {
    if (entry == nullptr || context < MDKR_CHARACTER_PREVIEW_SELECT ||
        context > MDKR_CHARACTER_PREVIEW_PLANE) {
        return false;
    }
    return session.result.version ==
               MDKR_CHARACTER_PREVIEW_RESULT_VERSION &&
           session.result.started && session.result.context == context &&
           session.result.warmup_complete &&
           session.result.replacement_draws != 0u &&
           session.result.fit_diagnostics_valid != 0 &&
           characterPreviewFitDiagnosticsValid(session.result) &&
           ((session.result.pose == MDKR_CHARACTER_PREVIEW_POSE_LIVE &&
             session.result.pose_phase_milli == 0u &&
             session.result.inspection_pose_ticks == 0u &&
             session.result.inspection_pose_fallback_ticks == 0u &&
             session.result.view_yaw_degrees == 0 &&
             session.result.view_pitch_degrees == 0 &&
             session.result.lighting ==
                 MDKR_WORKSHOP_PREVIEW_LIGHTING_NEUTRAL &&
             session.result.camera_override_ticks == 0u &&
             session.result.lighting_override_draws == 0u &&
             !session.result.capture_requested &&
             !session.result.capture_armed &&
             session.result.capture_stable_frames == 0u &&
             !session.result.capture_written &&
             session.result.capture_png_bytes == 0u) ||
            (session.result.pose > MDKR_CHARACTER_PREVIEW_POSE_LIVE &&
             session.result.pose < MDKR_CHARACTER_PREVIEW_POSE_COUNT &&
             session.result.pose_phase_milli <= 1000u &&
             session.result.view_yaw_degrees >= -180 &&
             session.result.view_yaw_degrees <= 180 &&
             session.result.view_pitch_degrees >= -45 &&
             session.result.view_pitch_degrees <= 45 &&
             session.result.lighting >=
                 MDKR_WORKSHOP_PREVIEW_LIGHTING_NEUTRAL &&
             session.result.lighting <
                 MDKR_WORKSHOP_PREVIEW_LIGHTING_COUNT &&
             (session.result.lighting ==
                      MDKR_WORKSHOP_PREVIEW_LIGHTING_NEUTRAL
                  ? session.result.lighting_override_draws == 0u
                  : session.result.lighting_override_draws != 0u) &&
             ((session.result.view_yaw_degrees == 0 &&
               session.result.view_pitch_degrees == 0)
                  ? session.result.camera_override_ticks == 0u
                  : session.result.camera_override_ticks != 0u) &&
             (context != MDKR_CHARACTER_PREVIEW_SELECT ||
              (session.result.view_yaw_degrees == 0 &&
               session.result.view_pitch_degrees == 0 &&
               session.result.camera_override_ticks == 0u)) &&
             session.result.inspection_pose_ticks != 0u &&
             session.result.inspection_pose_fallback_ticks == 0u)) &&
           (session.result.capture_requested ==
                !session.capturePng.empty()) &&
           (session.result.capture_requested
                ? ((session.result.capture_armed
                        ? session.result.capture_stable_frames >=
                              MDKR_CHARACTER_PREVIEW_CAPTURE_STABLE_FRAMES
                        : session.result.capture_stable_frames <
                              MDKR_CHARACTER_PREVIEW_CAPTURE_STABLE_FRAMES) &&
                   (session.result.capture_written
                        ? session.result.capture_armed &&
                              session.result.capture_png_bytes != 0u
                        : session.result.capture_png_bytes == 0u))
                : (!session.result.capture_armed &&
                   session.result.capture_stable_frames == 0u &&
                   !session.result.capture_written &&
                   session.result.capture_png_bytes == 0u)) &&
           session.sourceSha256 ==
               characterDigestHex(entry->source_sha256) &&
           session.fitSha256 == characterFitReviewSignature(
               entry, tuning, static_cast<unsigned>(context - 1));
}

const CharacterTestEvidenceStore::Evidence *currentRenderedCharacterTestEvidence(
    const MdkrModernCharacterEntry *entry,
    const CharacterTuningEdit &tuning,
    MdkrCharacterPreviewContext context) {
    loadCharacterTestEvidence();
    const CharacterTestEvidenceStore::Evidence *newest = nullptr;
    for (uint32_t players = 1u; players <= 4u; ++players) {
        const CharacterTestEvidenceStore::Evidence *evidence =
            CharacterTestEvidenceStore::find(
                g_characterTestEvidence, entry->id,
                static_cast<uint32_t>(context), players,
                CharacterTestEvidenceStore::Kind::Latest);
        if (evidence != nullptr &&
            evidence->resultVersion ==
                MDKR_CHARACTER_PREVIEW_RESULT_VERSION &&
            evidence->started && evidence->warmupComplete &&
            evidence->replacementDraws != 0u &&
            evidence->fitDiagnosticsValid &&
            characterTestEvidenceMatchesFit(entry, tuning, *evidence) &&
            (newest == nullptr ||
             evidence->capturedUnix > newest->capturedUnix)) {
            newest = evidence;
        }
    }
    return newest;
}

enum class CharacterSpatialPlane : int {
    Front = 0,
    Side,
    Top,
};

struct CharacterSpatialAxes {
    unsigned horizontal;
    unsigned vertical;
    const char *horizontalName;
    const char *verticalName;
};

struct CharacterSpatialEditResult {
    bool changed = false;
    bool commit = false;
};

CharacterSpatialAxes characterSpatialAxes(CharacterSpatialPlane plane) {
    switch (plane) {
        case CharacterSpatialPlane::Side:
            return {2u, 1u, "Z", "Y"};
        case CharacterSpatialPlane::Top:
            return {0u, 2u, "X", "Z"};
        case CharacterSpatialPlane::Front:
        default:
            return {0u, 1u, "X", "Y"};
    }
}

CharacterSpatialPlane drawCharacterSpatialPlaneSelector(
    const char *id, int &stored) {
    stored = std::clamp(stored, 0, 2);
    static const char *names[] = {"Front X/Y", "Side Z/Y", "Top X/Z"};
    ImGui::PushID(id);
    const int columns = ImGui::GetContentRegionAvail().x >= 410.0f ? 3 : 1;
    if (ImGui::BeginTable(
            "##spatial-plane-options", columns,
            ImGuiTableFlags_SizingStretchSame)) {
        for (int candidate = 0; candidate < 3; ++candidate) {
            ImGui::TableNextColumn();
            if (ImGui::RadioButton(names[candidate], &stored, candidate)) {
                stored = candidate;
            }
            ui::SpeakFocusedItem(
                names[candidate],
                candidate == stored ? "selected" : "not selected",
                "Changes only the spatial control view; it never changes the saved fit.");
        }
        ImGui::EndTable();
    }
    ImGui::PopID();
    return static_cast<CharacterSpatialPlane>(stored);
}

float characterSpatialRange(const float (*markers)[3], unsigned markerCount,
                            const CharacterSpatialAxes &axes,
                            float minimumRange, float maximumValue) {
    float range = minimumRange;
    for (unsigned marker = 0u; marker < markerCount; ++marker) {
        range = std::max(range,
                         std::fabs(markers[marker][axes.horizontal]) * 1.25f);
        range = std::max(range,
                         std::fabs(markers[marker][axes.vertical]) * 1.25f);
    }
    return std::min(range, maximumValue);
}

CharacterSpatialEditResult drawCharacterSpatialPad(
    const char *id, const char *accessibleName, float (*markers)[3],
    unsigned markerCount, unsigned selectedMarker,
    CharacterSpatialPlane plane, float minimumRange, float maximumValue,
    float nudgeStep, const char *zeroLabel,
    const char *const *markerNames) {
    CharacterSpatialEditResult result;
    if (markers == nullptr || markerCount == 0u ||
        selectedMarker >= markerCount) return result;
    const CharacterSpatialAxes axes = characterSpatialAxes(plane);
    const float range = characterSpatialRange(
        markers, markerCount, axes, minimumRange, maximumValue);
    const float available = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    const float side = std::min(available, 340.0f);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(id, ImVec2(side, side));
    const ImGuiID itemId = ImGui::GetItemID();
    ImDrawList *draw = ImGui::GetWindowDrawList();
    const ImVec2 maximum(origin.x + side, origin.y + side);
    const ImVec2 center(origin.x + side * 0.5f, origin.y + side * 0.5f);
    const float half = side * 0.43f;
    draw->AddRectFilled(origin, maximum, IM_COL32(20, 24, 31, 255), 6.0f);
    draw->AddRect(origin, maximum, IM_COL32(255, 255, 255, 42), 6.0f);
    for (int grid = -2; grid <= 2; ++grid) {
        const float delta = half * static_cast<float>(grid) / 2.0f;
        const ImU32 color = grid == 0
            ? IM_COL32(255, 255, 255, 105)
            : IM_COL32(255, 255, 255, 28);
        draw->AddLine(ImVec2(center.x + delta, center.y - half),
                      ImVec2(center.x + delta, center.y + half), color,
                      grid == 0 ? 1.5f : 1.0f);
        draw->AddLine(ImVec2(center.x - half, center.y + delta),
                      ImVec2(center.x + half, center.y + delta), color,
                      grid == 0 ? 1.5f : 1.0f);
    }
    draw->AddText(ImVec2(origin.x + 8.0f, origin.y + 7.0f),
                  IM_COL32(180, 190, 205, 255), axes.verticalName);
    draw->AddText(ImVec2(maximum.x - 18.0f, center.y + 5.0f),
                  IM_COL32(180, 190, 205, 255), axes.horizontalName);
    if (zeroLabel != nullptr) {
        draw->AddText(ImVec2(origin.x + 8.0f, maximum.y - 22.0f),
                      IM_COL32(180, 190, 205, 255), zeroLabel);
    }
    const ImU32 markerColors[MDKR_MODERN_CHARACTER_CONTACTS] = {
        IM_COL32(74, 191, 255, 255), IM_COL32(255, 119, 196, 255),
        IM_COL32(106, 221, 138, 255), IM_COL32(255, 194, 92, 255),
    };
    for (unsigned marker = 0u; marker < markerCount; ++marker) {
        const float x = std::clamp(
            markers[marker][axes.horizontal] / range, -1.0f, 1.0f);
        const float y = std::clamp(
            markers[marker][axes.vertical] / range, -1.0f, 1.0f);
        const ImVec2 point(center.x + x * half, center.y - y * half);
        const ImU32 color = markerCount == 1u
            ? IM_COL32(91, 192, 255, 255)
            : markerColors[marker % MDKR_MODERN_CHARACTER_CONTACTS];
        draw->AddCircleFilled(point, marker == selectedMarker ? 7.0f : 4.5f,
                              color);
        if (marker == selectedMarker) {
            draw->AddCircle(point, 11.0f, IM_COL32(255, 255, 255, 210),
                            0, 1.5f);
        }
    }
    if (ImGui::IsItemActive()) {
        const ImVec2 delta = ImGui::GetIO().MouseDelta;
        if (delta.x != 0.0f || delta.y != 0.0f) {
            float *value = markers[selectedMarker];
            const float unitsPerPixel = range / half;
            value[axes.horizontal] = std::clamp(
                value[axes.horizontal] + delta.x * unitsPerPixel,
                -maximumValue, maximumValue);
            value[axes.vertical] = std::clamp(
                value[axes.vertical] - delta.y * unitsPerPixel,
                -maximumValue, maximumValue);
            g_characterSpatialGestureDirty[itemId] = true;
            result.changed = true;
        }
    }
    if (ImGui::IsItemHovered() &&
        ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        markers[selectedMarker][axes.horizontal] = 0.0f;
        markers[selectedMarker][axes.vertical] = 0.0f;
        g_characterSpatialGestureDirty.erase(itemId);
        result.changed = result.commit = true;
    } else if (ImGui::IsItemDeactivated()) {
        const auto dirty = g_characterSpatialGestureDirty.find(itemId);
        if (dirty != g_characterSpatialGestureDirty.end() && dirty->second) {
            g_characterSpatialGestureDirty.erase(dirty);
            result.commit = true;
        }
    }
    char state[192];
    std::snprintf(
        state, sizeof(state), "%s %.3f metres, %s %.3f metres",
        axes.horizontalName,
        static_cast<double>(markers[selectedMarker][axes.horizontal]),
        axes.verticalName,
        static_cast<double>(markers[selectedMarker][axes.vertical]));
    ui::SpeakFocusedItem(
        accessibleName, state,
        "Drag to move on this plane. Double-click resets these two axes; the adjacent buttons and numeric fields provide keyboard and controller input.");

    const auto nudge = [&](const char *visible, unsigned axis, float amount) {
        const std::string label = std::string(visible) + "##" + id;
        if (ImGui::Button(label.c_str())) {
            markers[selectedMarker][axis] = std::clamp(
                markers[selectedMarker][axis] + amount,
                -maximumValue, maximumValue);
            result.changed = result.commit = true;
        }
        char consequence[128];
        std::snprintf(consequence, sizeof(consequence),
                      "Moves the selected marker by %.3f metres on %s.",
                      static_cast<double>(amount),
                      axis == axes.horizontal ? axes.horizontalName
                                              : axes.verticalName);
        ui::SpeakFocusedItem(visible, nullptr, consequence);
    };
    const std::string horizontalMinus =
        std::string(axes.horizontalName) + " -";
    const std::string horizontalPlus =
        std::string(axes.horizontalName) + " +";
    const std::string verticalMinus =
        std::string(axes.verticalName) + " -";
    const std::string verticalPlus =
        std::string(axes.verticalName) + " +";
    nudge(horizontalMinus.c_str(), axes.horizontal, -nudgeStep);
    ImGui::SameLine();
    nudge(horizontalPlus.c_str(), axes.horizontal, nudgeStep);
    nudge(verticalMinus.c_str(), axes.vertical, -nudgeStep);
    ImGui::SameLine();
    nudge(verticalPlus.c_str(), axes.vertical, nudgeStep);
    const std::string resetLabel = std::string("Reset plane##") + id;
    if (ImGui::Button(resetLabel.c_str())) {
        markers[selectedMarker][axes.horizontal] = 0.0f;
        markers[selectedMarker][axes.vertical] = 0.0f;
        result.changed = result.commit = true;
    }
    ui::SpeakFocusedItem(
        "Reset plane", nullptr,
        "Resets only the two visible axes for the selected marker.");
    if (markerNames != nullptr && markerCount > 1u) {
        for (unsigned marker = 0u; marker < markerCount; ++marker) {
            if ((marker & 1u) != 0u) ImGui::SameLine();
            ImGui::TextColored(
                ImGui::ColorConvertU32ToFloat4(
                    markerColors[marker % MDKR_MODERN_CHARACTER_CONTACTS]),
                "%s%s", marker == selectedMarker ? "* " : "",
                markerNames[marker]);
        }
    }
    ImGui::TextDisabled("Visible range: +/- %.2f m", range);
    return result;
}

CharacterSpatialEditResult drawCharacterYawDial(
    const char *id, const char *accessibleName, float &yawDegrees) {
    CharacterSpatialEditResult result;
    const float side = 126.0f;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(id, ImVec2(side, side));
    const ImGuiID itemId = ImGui::GetItemID();
    const ImVec2 center(origin.x + side * 0.5f, origin.y + side * 0.5f);
    ImDrawList *draw = ImGui::GetWindowDrawList();
    draw->AddCircleFilled(center, side * 0.46f, IM_COL32(20, 24, 31, 255));
    draw->AddCircle(center, side * 0.46f, IM_COL32(255, 255, 255, 55),
                    0, 1.5f);
    draw->AddText(ImVec2(center.x - 8.0f, origin.y + 4.0f),
                  IM_COL32(180, 190, 205, 255), "+Z");
    const float radians = yawDegrees * 0.01745329251994329577f;
    const ImVec2 tip(center.x + std::sin(radians) * side * 0.35f,
                     center.y - std::cos(radians) * side * 0.35f);
    draw->AddLine(center, tip, IM_COL32(91, 192, 255, 255), 4.0f);
    draw->AddCircleFilled(tip, 6.0f, IM_COL32(255, 255, 255, 255));
    if (ImGui::IsItemActive()) {
        const ImVec2 pointer = ImGui::GetIO().MousePos;
        const float dx = pointer.x - center.x;
        const float dy = pointer.y - center.y;
        if (dx * dx + dy * dy > 16.0f) {
            yawDegrees = std::atan2(dx, -dy) * 57.295779513082320876f;
            yawDegrees = std::clamp(yawDegrees, -180.0f, 180.0f);
            g_characterSpatialGestureDirty[itemId] = true;
            result.changed = true;
        }
    }
    if (ImGui::IsItemHovered() &&
        ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        yawDegrees = 0.0f;
        g_characterSpatialGestureDirty.erase(itemId);
        result.changed = result.commit = true;
    } else if (ImGui::IsItemDeactivated()) {
        const auto dirty = g_characterSpatialGestureDirty.find(itemId);
        if (dirty != g_characterSpatialGestureDirty.end() && dirty->second) {
            g_characterSpatialGestureDirty.erase(dirty);
            result.commit = true;
        }
    }
    char state[64];
    std::snprintf(state, sizeof(state), "yaw %.1f degrees",
                  static_cast<double>(yawDegrees));
    ui::SpeakFocusedItem(
        accessibleName, state,
        "Drag the arrow toward the intended forward direction. Double-click resets yaw; the adjacent buttons and numeric rotation field provide keyboard and controller input.");
    if (ImGui::Button("Turn 180 degrees##yaw")) {
        yawDegrees += yawDegrees > 0.0f ? -180.0f : 180.0f;
        result.changed = result.commit = true;
    }
    ui::SpeakFocusedItem(
        "Turn this context 180 degrees", nullptr,
        "Reverses only this context's facing direction and leaves every other fit unchanged.");
    if (ImGui::Button("Reset yaw##yaw")) {
        yawDegrees = 0.0f;
        result.changed = result.commit = true;
    }
    ui::SpeakFocusedItem(
        "Reset this context yaw", nullptr,
        "Resets only this context's Y-axis rotation.");
    return result;
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
    CharacterHistoryFrame history = beginCharacterHistory(
        entry, CharacterHistoryTool::Fit);
    if (std::getenv("MDKR_APP_UI_TRACE") != nullptr &&
        g_characterSpatialControlTracePackages.insert(entry->id).second) {
        std::fprintf(
            stderr,
            "[app-ui] character-spatial-controls package=%s planes=front,side,top placement=ground-or-seat yaw=context contacts=4 copy=vehicle-only undo=fit-history\n",
            entry->id);
    }

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
            if (ImGui::TreeNodeEx(
                    "Spatial controls",
                    ImGuiTreeNodeFlags_DefaultOpen)) {
                ui::TextSubtleWrapped(
                    "Move the fitted character around its automatic ground or seat anchor in front, side, or top view. The blue forward arrow is the saved context yaw, not the camera. These controls and the exact numeric fields above edit the same values.");
                const std::string spatialKey = std::string(entry->id) + "#" +
                    std::to_string(context);
                int &spatialView = g_characterFitSpatialViews[spatialKey];
                const CharacterSpatialPlane plane =
                    drawCharacterSpatialPlaneSelector(
                        "fit-plane", spatialView);
                const bool wideSpatial =
                    ImGui::GetContentRegionAvail().x >= 500.0f;
                ImGui::BeginGroup();
                CharacterSpatialEditResult positionResult =
                    drawCharacterSpatialPad(
                        "##fit-position-pad",
                        context == MDKR_CHARACTER_CONTEXT_SELECT
                            ? "Ground-fit correction control"
                            : "Seat-fit correction control",
                        &placement.offset, 1u, 0u, plane,
                        std::max(0.5f, entry->target_height * edit.scale *
                                          placement.scale),
                        10.0f, 0.01f, "zero = automatic anchor", nullptr);
                ImGui::EndGroup();
                if (wideSpatial) ImGui::SameLine();
                ImGui::BeginGroup();
                ImGui::TextUnformatted("Facing in target space");
                CharacterSpatialEditResult yawResult =
                    drawCharacterYawDial(
                        "##fit-yaw-dial", "Context-facing control",
                        placement.rotation[1]);
                ImGui::EndGroup();
                if (positionResult.commit || yawResult.commit) {
                    changed |= persistCharacterTuning(entry->id, edit);
                }
                ImGui::TreePop();
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
                    const std::string contactKey =
                        std::string(entry->id) + "#" +
                        std::to_string(context);
                    int &selectedContact =
                        g_characterSelectedContacts[contactKey];
                    selectedContact = std::clamp(
                        selectedContact, 0,
                        static_cast<int>(MDKR_MODERN_CHARACTER_CONTACTS) - 1);
                    const float contactWidth =
                        ImGui::GetContentRegionAvail().x;
                    const int contactColumns = contactWidth >= 620.0f
                        ? 4 : contactWidth >= 300.0f ? 2 : 1;
                    if (ImGui::BeginTable(
                            "##contact-target-selection", contactColumns,
                            ImGuiTableFlags_SizingStretchSame)) {
                        for (unsigned contact = 0u;
                             contact < MDKR_MODERN_CHARACTER_CONTACTS;
                             ++contact) {
                            ImGui::TableNextColumn();
                            const std::string selectLabel =
                                std::string(contactLabels[contact]) +
                                "##contact-select";
                            if (ImGui::RadioButton(
                                    selectLabel.c_str(), &selectedContact,
                                    static_cast<int>(contact))) {
                                selectedContact = static_cast<int>(contact);
                            }
                            ui::SpeakFocusedItem(
                                contactLabels[contact],
                                selectedContact == static_cast<int>(contact)
                                    ? "selected" : "not selected",
                                "Chooses the contact target edited by the spatial control and numeric fields.");
                        }
                        ImGui::EndTable();
                    }
                    int &contactView =
                        g_characterContactSpatialViews[contactKey];
                    const CharacterSpatialPlane contactPlane =
                        drawCharacterSpatialPlaneSelector(
                            "contact-plane", contactView);
                    CharacterSpatialEditResult contactSpatial =
                        drawCharacterSpatialPad(
                            "##contact-target-pad",
                            "Hand and foot target adjustment control",
                            placement.contacts,
                            MDKR_MODERN_CHARACTER_CONTACTS,
                            static_cast<unsigned>(selectedContact),
                            contactPlane, 0.15f, 1.0f, 0.005f,
                            "zero = engine target",
                            contactLabels);
                    if (contactSpatial.commit) {
                        changed |= persistCharacterTuning(entry->id, edit);
                    }
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
            if (context != MDKR_CHARACTER_CONTEXT_SELECT &&
                ImGui::TreeNode("Copy this vehicle fit")) {
                ui::TextSubtleWrapped(
                    "Copies size, position, rotation, and all four contact adjustments to one other qualified vehicle. The destination's prior fit remains available through Undo Fit; select placement is never included.");
                for (unsigned destination = MDKR_CHARACTER_CONTEXT_CAR;
                     destination < MDKR_CHARACTER_CONTEXT_COUNT;
                     ++destination) {
                    if (destination == context ||
                        (entry->vehicle_mask &
                         (1u << (destination - 1u))) == 0u) continue;
                    const std::string copyLabel = std::string("Copy to ") +
                        contextNames[destination];
                    if (ImGui::Button(copyLabel.c_str())) {
                        edit.context[destination] = placement;
                        changed |= persistCharacterTuning(entry->id, edit);
                    }
                    ui::SpeakFocusedItem(
                        copyLabel.c_str(), nullptr,
                        "Replaces only the named vehicle's fit and contact adjustments; select placement and other vehicles stay unchanged.");
                }
                ImGui::TreePop();
            }
            const MdkrCharacterPreviewContext previewContext =
                previewContexts[context];
            const auto result = g_characterPreviewResults.find(entry->id);
            const bool currentSessionResult =
                result != g_characterPreviewResults.end() &&
                characterPreviewSessionMatchesFit(
                    entry, edit, previewContext, result->second);
            const CharacterTestEvidenceStore::Evidence *durableResult =
                currentRenderedCharacterTestEvidence(
                    entry, edit, previewContext);
            const bool currentResult = currentSessionResult ||
                durableResult != nullptr;
            if (currentSessionResult) {
                if (characterPreviewFitDiagnosticsValid(
                        result->second.result)) {
                    drawCharacterFitDiagnostics(
                        result->second.result, true);
                } else {
                    ImGui::TextColored(
                        AppTheme::bad(),
                        "Last exact test returned invalid fit measurements.");
                }
            } else if (durableResult != nullptr) {
                drawCharacterFitDiagnostics(
                    characterPreviewResultFromEvidence(*durableResult), true);
                if (std::getenv("MDKR_APP_UI_TRACE") != nullptr) {
                    const std::string traceKey = std::string(entry->id) + ":" +
                        std::to_string(static_cast<unsigned>(previewContext));
                    if (g_characterFitEvidenceTraceContexts.insert(
                            traceKey).second) {
                        std::fprintf(
                            stderr,
                            "[app-ui] character-fit-evidence durable=1 package=%s context=%u players=%u fit=%d fitAnchorUm=%lld,%lld,%lld fitBoundsYUm=%lld,%lld fitForwardMilli=%d,%d,%d\n",
                            entry->id,
                            static_cast<unsigned>(previewContext),
                            durableResult->players,
                            durableResult->fitDiagnosticsValid ? 1 : 0,
                            static_cast<long long>(
                                durableResult->fitAnchorMicrometres[0]),
                            static_cast<long long>(
                                durableResult->fitAnchorMicrometres[1]),
                            static_cast<long long>(
                                durableResult->fitAnchorMicrometres[2]),
                            static_cast<long long>(
                                durableResult->fitBoundsMinMicrometres[1]),
                            static_cast<long long>(
                                durableResult->fitBoundsMaxMicrometres[1]),
                            durableResult->fitForwardMilli[0],
                            durableResult->fitForwardMilli[1],
                            durableResult->fitForwardMilli[2]);
                    }
                }
            }
            if (currentResult &&
                context != MDKR_CHARACTER_CONTEXT_SELECT) {
                const uint64_t contactSolves = currentSessionResult
                    ? result->second.result.contact_solves
                    : durableResult->contactSolves;
                const uint64_t contactMean = currentSessionResult
                    ? result->second.result.contact_error_mean_micrometres
                    : durableResult->contactErrorMeanMicrometres;
                const uint64_t contactMaximum = currentSessionResult
                    ? result->second.result.contact_error_max_micrometres
                    : durableResult->contactErrorMaxMicrometres;
                if (contactSolves != 0u) {
                    ImGui::Text(
                        "Last exact test: %.2f mm mean · %.2f mm maximum across %llu solves",
                        contactMean / 1000.0,
                        contactMaximum / 1000.0,
                        static_cast<unsigned long long>(contactSolves));
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
                const bool contextEnabled =
                    context == MDKR_CHARACTER_CONTEXT_SELECT ||
                    (edit.vehicleMask & (1u << (context - 1u))) != 0u;
                const bool testEnabled = entry->enabled != 0u && contextEnabled;
                if (!testEnabled) ImGui::BeginDisabled();
                const std::string testLabel = std::string("Test ") +
                    contextNames[context] + " fit in exact renderer";
                if (ImGui::Button(testLabel.c_str()) && testEnabled &&
                    persistCharacterTuning(entry->id, edit)) {
                    requestCharacterPreview(entry, previewContext, testPlayers);
                }
                if (!testEnabled) ImGui::EndDisabled();
                ui::SpeakFocusedItem(
                    testLabel.c_str(),
                    testEnabled ? nullptr
                        : entry->enabled == 0u
                            ? "Enable this character package first."
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
    finishCharacterHistory(entry, history);
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
    CharacterHistoryFrame history = beginCharacterHistory(
        entry, CharacterHistoryTool::Performance);
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
        /* SizingStretchProp derives unspecified weights from the previous
         * frame's measured content.  Both measurements are zero while this
         * table is first appearing, so explicit weights avoid a transient
         * 0 / 0 in ImGui's table layout and keep the child content extent
         * finite under UBSan. */
        ImGui::TableSetupColumn(
            "Metric", ImGuiTableColumnFlags_WidthStretch, 2.0f);
        ImGui::TableSetupColumn(
            "Assembly value", ImGuiTableColumnFlags_WidthStretch, 1.0f);
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
    finishCharacterHistory(entry, history);
}

void requestCharacterPreview(const MdkrModernCharacterEntry *entry,
                             MdkrCharacterPreviewContext context,
                             int players,
                             MdkrCharacterPreviewPose pose,
                             unsigned posePhaseMilli,
                             int viewYawDegrees,
                             int viewPitchDegrees,
                             MdkrWorkshopPreviewLighting lighting,
                             const char *capturePng) {
    const CharacterTuningEdit &tuning = loadCharacterTuning(0, entry->id);
    const std::string fitSignature = characterFitReviewSignature(
        entry, tuning, static_cast<unsigned>(context - 1));
    const std::string presentationSignature =
        characterTestPresentationSignature();
    if (fitSignature.empty() || presentationSignature.empty()) {
        setStatus(
            "The exact test could not bind its source, fit, and presentation settings; no test was started.",
            AppTheme::bad());
        return;
    }
    const bool inspection = pose != MDKR_CHARACTER_PREVIEW_POSE_LIVE;
    const bool capture = capturePng != nullptr && capturePng[0] != '\0';
    int captureExists = 0;
    const size_t captureLength = capture ? std::strlen(capturePng) : 0u;
    if (capture) {
        (void)mdkr_path_query_utf8(
            capturePng, &captureExists, nullptr, nullptr);
    }
    if (viewYawDegrees < -180 || viewYawDegrees > 180 ||
        viewPitchDegrees < -45 || viewPitchDegrees > 45 ||
        lighting < MDKR_WORKSHOP_PREVIEW_LIGHTING_NEUTRAL ||
        lighting >= MDKR_WORKSHOP_PREVIEW_LIGHTING_COUNT ||
        (!inspection && (viewYawDegrees != 0 || viewPitchDegrees != 0 ||
                         lighting !=
                             MDKR_WORKSHOP_PREVIEW_LIGHTING_NEUTRAL ||
                         capture)) ||
        (context == MDKR_CHARACTER_PREVIEW_SELECT &&
         (viewYawDegrees != 0 || viewPitchDegrees != 0)) ||
        (capture &&
         (captureLength < 4u || captureLength >= 1024u ||
          std::strcmp(capturePng + captureLength - 4u, ".png") != 0 ||
          captureExists))) {
        setStatus(
            capture
                ? "Choose a new writable .png filename; captures never overwrite an existing file."
                : "The inspection view or lighting request was invalid; no preview was started.",
            AppTheme::bad());
        return;
    }
    if (pose < MDKR_CHARACTER_PREVIEW_POSE_LIVE ||
        pose >= MDKR_CHARACTER_PREVIEW_POSE_COUNT ||
        posePhaseMilli > 1000u) {
        setStatus("The pose inspection request was invalid; no preview was started.",
                  AppTheme::bad());
        return;
    }
    g_characterPreviewRequest = SettingsCharacterPreviewRequest{};
    g_characterPreviewRequest.packageId = entry->id;
    g_characterPreviewRequest.sourceSha256 =
        characterDigestHex(entry->source_sha256);
    g_characterPreviewRequest.fitSha256 = fitSignature;
    g_characterPreviewRequest.presentationSha256 = presentationSignature;
    g_characterPreviewRequest.context = context;
    g_characterPreviewRequest.players = players;
    g_characterPreviewRequest.pose = pose;
    g_characterPreviewRequest.posePhaseMilli =
        inspection ? posePhaseMilli : 0u;
    g_characterPreviewRequest.viewYawDegrees =
        inspection ? viewYawDegrees : 0;
    g_characterPreviewRequest.viewPitchDegrees =
        inspection ? viewPitchDegrees : 0;
    g_characterPreviewRequest.lighting = inspection
        ? lighting : MDKR_WORKSHOP_PREVIEW_LIGHTING_NEUTRAL;
    if (capture) g_characterPreviewRequest.capturePng = capturePng;
    g_characterPreviewRequested = true;
    setStatus(
        pose == MDKR_CHARACTER_PREVIEW_POSE_LIVE
            ? "Checking the selected ROM, then opening the exact game context."
            : capture
                ? "Checking the selected ROM, then opening the held inspection pose and saving one stabilized PNG."
                : "Checking the selected ROM, then opening the exact game context with the inspection pose held.",
        AppTheme::good());
}

std::string boundedCharacterPreviewText(const char *text, size_t capacity) {
    if (text == nullptr) return {};
    size_t length = 0u;
    while (length < capacity && text[length] != '\0') ++length;
    return std::string(text, length);
}

CharacterTestEvidenceStore::Evidence characterTestEvidenceFromResult(
    const std::string &packageId, const std::string &sourceSha256,
    const std::string &fitSha256,
    const std::string &presentationSha256,
    const MdkrCharacterPreviewResult &result) {
    CharacterTestEvidenceStore::Evidence evidence;
    evidence.packageId = packageId;
    const std::time_t now = std::time(nullptr);
    evidence.capturedUnix = now >= 0 ? static_cast<uint64_t>(now) : 0u;
    evidence.sourceSha256 = sourceSha256;
    evidence.fitSha256 = fitSha256;
    evidence.presentationSha256 = presentationSha256;
    evidence.buildVersion = AppVersion();
    evidence.context = static_cast<uint32_t>(result.context);
    evidence.players = static_cast<uint32_t>(result.players);
    evidence.resultVersion = result.version;
    evidence.started = result.started != 0;
    evidence.warmupComplete = result.warmup_complete != 0;
    evidence.realtime = result.realtime != 0;
    evidence.warmupTicks = result.warmup_ticks;
    evidence.intervalSamples = result.interval_samples;
    evidence.displayedFrames = result.displayed_frames;
    evidence.intervalP50Us = result.interval_p50_us;
    evidence.intervalP95Us = result.interval_p95_us;
    evidence.intervalP99Us = result.interval_p99_us;
    evidence.intervalMeanUs = result.interval_mean_us;
    evidence.intervalMaxUs = result.interval_max_us;
    evidence.tickwallSamples = result.tickwall_samples;
    evidence.tickwallMeanNs = result.tickwall_mean_ns;
    evidence.replacementDraws = result.replacement_draws;
    evidence.replacementPrimitives = result.replacement_primitives;
    evidence.hiddenDonorBatches = result.hidden_donor_batches;
    evidence.contactSolves = result.contact_solves;
    evidence.contactErrorMeanMicrometres =
        result.contact_error_mean_micrometres;
    evidence.contactErrorMaxMicrometres =
        result.contact_error_max_micrometres;
    evidence.fitDiagnosticsValid = result.fit_diagnostics_valid != 0;
    for (unsigned axis = 0u; axis < 3u; ++axis) {
        evidence.fitBoundsMinMicrometres[axis] =
            result.fit_bounds_min_micrometres[axis];
        evidence.fitBoundsMaxMicrometres[axis] =
            result.fit_bounds_max_micrometres[axis];
        evidence.fitAnchorMicrometres[axis] =
            result.fit_anchor_micrometres[axis];
        evidence.fitForwardMilli[axis] = result.fit_forward_milli[axis];
    }
    evidence.backend = boundedCharacterPreviewText(
        result.renderer_backend, sizeof(result.renderer_backend));
    evidence.adapter = boundedCharacterPreviewText(
        result.adapter, sizeof(result.adapter));
    evidence.driver = boundedCharacterPreviewText(
        result.driver, sizeof(result.driver));
    evidence.vendorId = result.vendor_id;
    evidence.deviceId = result.device_id;
    evidence.outputWidth = result.output_width;
    evidence.outputHeight = result.output_height;
    evidence.renderWidth = result.render_width;
    evidence.renderHeight = result.render_height;
    return evidence;
}

MdkrCharacterPreviewResult characterPreviewResultFromEvidence(
    const CharacterTestEvidenceStore::Evidence &evidence) {
    MdkrCharacterPreviewResult result{};
    result.version = evidence.resultVersion;
    result.started = evidence.started ? 1 : 0;
    result.context = static_cast<MdkrCharacterPreviewContext>(
        evidence.context);
    result.players = static_cast<int>(evidence.players);
    result.replacement_draws = evidence.replacementDraws;
    result.fit_diagnostics_valid = evidence.fitDiagnosticsValid ? 1 : 0;
    for (unsigned axis = 0u; axis < 3u; ++axis) {
        result.fit_bounds_min_micrometres[axis] =
            evidence.fitBoundsMinMicrometres[axis];
        result.fit_bounds_max_micrometres[axis] =
            evidence.fitBoundsMaxMicrometres[axis];
        result.fit_anchor_micrometres[axis] =
            evidence.fitAnchorMicrometres[axis];
        result.fit_forward_milli[axis] = evidence.fitForwardMilli[axis];
    }
    return result;
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

bool characterPreviewFitDiagnosticsValid(
    const MdkrCharacterPreviewResult &result) {
    if (result.fit_diagnostics_valid != 0 &&
        result.fit_diagnostics_valid != 1) return false;
    if (!result.fit_diagnostics_valid) {
        for (unsigned axis = 0u; axis < 3u; ++axis) {
            if (result.fit_bounds_min_micrometres[axis] != 0 ||
                result.fit_bounds_max_micrometres[axis] != 0 ||
                result.fit_anchor_micrometres[axis] != 0 ||
                result.fit_forward_milli[axis] != 0) return false;
        }
        return true;
    }
    if (result.replacement_draws == 0u) return false;
    long long forwardLengthSquared = 0;
    constexpr long long kMaximumFitMicrometres = 1000000000LL;
    const auto withinFitRange = [](long long value) {
        return value >= -kMaximumFitMicrometres &&
               value <= kMaximumFitMicrometres;
    };
    for (unsigned axis = 0u; axis < 3u; ++axis) {
        if (result.fit_bounds_min_micrometres[axis] >
                result.fit_bounds_max_micrometres[axis] ||
            !withinFitRange(result.fit_bounds_min_micrometres[axis]) ||
            !withinFitRange(result.fit_bounds_max_micrometres[axis]) ||
            !withinFitRange(result.fit_anchor_micrometres[axis]) ||
            result.fit_forward_milli[axis] < -1001 ||
            result.fit_forward_milli[axis] > 1001) return false;
        forwardLengthSquared +=
            static_cast<long long>(result.fit_forward_milli[axis]) *
            result.fit_forward_milli[axis];
    }
    return forwardLengthSquared >= 995000LL &&
           forwardLengthSquared <= 1005000LL;
}

void drawCharacterFitDiagnostics(
    const MdkrCharacterPreviewResult &result, bool compact) {
    if (!result.fit_diagnostics_valid) {
        if (!compact) {
            ui::TextSubtleWrapped(
                "This package has no renderer fit measurement. Legacy packages without explicit calibration can still render, but cannot claim measured ground, seat, bounds, or facing evidence.");
        }
        return;
    }
    const double anchorX =
        result.fit_anchor_micrometres[0] / 1000000.0;
    const double anchorY =
        result.fit_anchor_micrometres[1] / 1000000.0;
    const double anchorZ =
        result.fit_anchor_micrometres[2] / 1000000.0;
    const double minimumY =
        result.fit_bounds_min_micrometres[1] / 1000000.0;
    const double maximumY =
        result.fit_bounds_max_micrometres[1] / 1000000.0;
    const double forwardX = result.fit_forward_milli[0] / 1000.0;
    const double forwardY = result.fit_forward_milli[1] / 1000.0;
    const double forwardZ = result.fit_forward_milli[2] / 1000.0;
    const double forwardLength = std::sqrt(
        forwardX * forwardX + forwardY * forwardY + forwardZ * forwardZ);
    const double facingDegrees = std::acos(std::clamp(
        forwardZ / forwardLength, -1.0, 1.0)) *
        57.295779513082320876;
    const bool backward = forwardZ < 0.0;
    const bool offAxis = facingDegrees > 15.0;
    if (result.context == MDKR_CHARACTER_PREVIEW_SELECT) {
        const bool belowFloor = minimumY < -0.005;
        const bool aboveFloor = minimumY > 0.005;
        ImGui::TextColored(
            belowFloor ? AppTheme::bad()
                       : aboveFloor ? AppTheme::accent() : AppTheme::good(),
            belowFloor
                ? "Calibrated volume passes %.1f mm below the roster floor"
                : aboveFloor
                    ? "Calibrated volume floats %.1f mm above the roster floor"
                    : "Calibrated volume is floor-aligned within %.1f mm",
            std::fabs(minimumY) * 1000.0);
        ImGui::Text(
            "Ground correction: X %+.3f m · Y %+.3f m · Z %+.3f m",
            anchorX, anchorY, anchorZ);
    } else {
        ImGui::Text(
            "Seat correction: X %+.3f m · Y %+.3f m · Z %+.3f m",
            anchorX, anchorY, anchorZ);
        ImGui::Text("Calibrated vertical span: %+.3f to %+.3f m from seat",
                    minimumY, maximumY);
    }
    ImGui::TextColored(
        backward ? AppTheme::bad()
                 : offAxis ? AppTheme::accent() : AppTheme::good(),
        backward
            ? "Facing is backward: %.1f degrees from target +Z"
            : offAxis
                ? "Facing is off-axis: %.1f degrees from target +Z"
                : "Facing: %.1f degrees from target +Z",
        facingDegrees);
    if (!compact) {
        const double width =
            (result.fit_bounds_max_micrometres[0] -
             result.fit_bounds_min_micrometres[0]) / 1000000.0;
        const double height = maximumY - minimumY;
        const double depth =
            (result.fit_bounds_max_micrometres[2] -
             result.fit_bounds_min_micrometres[2]) / 1000000.0;
        ImGui::Text("Calibrated fitted volume: %.3f x %.3f x %.3f m",
                    width, height, depth);
        ui::TextSubtleWrapped(
            "These values use the actual replacement draw transform and donor target frame. Bounds come from the package's calibrated source volume; animated limbs and cloth can extend beyond them, so inspect held poses and contact error as well.");
    }
}

const CharacterInspectionPose *characterInspectionPose(
    MdkrCharacterPreviewPose pose) {
    const auto found = std::find_if(
        std::begin(kCharacterInspectionPoses),
        std::end(kCharacterInspectionPoses),
        [pose](const CharacterInspectionPose &candidate) {
            return candidate.pose == pose;
        });
    return found != std::end(kCharacterInspectionPoses) ? &*found : nullptr;
}

const CharacterInspectionLighting *characterInspectionLighting(
    MdkrWorkshopPreviewLighting lighting) {
    const auto found = std::find_if(
        std::begin(kCharacterInspectionLighting),
        std::end(kCharacterInspectionLighting),
        [lighting](const CharacterInspectionLighting &candidate) {
            return candidate.lighting == lighting;
        });
    return found != std::end(kCharacterInspectionLighting) ? &*found : nullptr;
}

void drawCharacterPreviewResult(const MdkrModernCharacterEntry *entry) {
    const auto found = g_characterPreviewResults.find(entry->id);
    if (found == g_characterPreviewResults.end()) return;
    const MdkrCharacterPreviewResult &result = found->second.result;
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
    if (!characterPreviewFitDiagnosticsValid(result)) {
        ImGui::TextColored(
            AppTheme::bad(),
            "The engine returned an invalid renderer fit measurement.");
        ui::TextSubtleWrapped(
            "No fit conclusion or performance evidence was saved.");
        ui::CardEnd();
        return;
    }
    ImGui::Text("%s  •  %d %s",
                characterPreviewResultContext(result.context), result.players,
                result.players == 1 ? "player" : "players");
    drawCharacterFitDiagnostics(result, false);
    if (result.pose != MDKR_CHARACTER_PREVIEW_POSE_LIVE) {
        const CharacterInspectionPose *pose =
            characterInspectionPose(result.pose);
        const CharacterInspectionLighting *lighting =
            characterInspectionLighting(result.lighting);
        if (pose == nullptr || result.pose_phase_milli > 1000u ||
            lighting == nullptr || result.view_yaw_degrees < -180 ||
            result.view_yaw_degrees > 180 ||
            result.view_pitch_degrees < -45 ||
            result.view_pitch_degrees > 45 ||
            (result.context == MDKR_CHARACTER_PREVIEW_SELECT &&
             (result.view_yaw_degrees != 0 ||
              result.view_pitch_degrees != 0 ||
              result.camera_override_ticks != 0u)) ||
            (result.lighting == MDKR_WORKSHOP_PREVIEW_LIGHTING_NEUTRAL &&
             result.lighting_override_draws != 0u) ||
            (result.warmup_complete && result.replacement_draws != 0u &&
             result.lighting != MDKR_WORKSHOP_PREVIEW_LIGHTING_NEUTRAL &&
             result.lighting_override_draws == 0u) ||
            (result.warmup_complete &&
             result.context != MDKR_CHARACTER_PREVIEW_SELECT &&
             (result.view_yaw_degrees != 0 ||
              result.view_pitch_degrees != 0) &&
             result.camera_override_ticks == 0u) ||
            (result.capture_requested !=
             !found->second.capturePng.empty()) ||
            (!result.capture_requested &&
             (result.capture_armed || result.capture_stable_frames != 0u ||
              result.capture_written || result.capture_png_bytes != 0u)) ||
            (result.capture_armed &&
             result.capture_stable_frames <
                 MDKR_CHARACTER_PREVIEW_CAPTURE_STABLE_FRAMES) ||
            (!result.capture_armed &&
             result.capture_stable_frames >=
                 MDKR_CHARACTER_PREVIEW_CAPTURE_STABLE_FRAMES) ||
            (!result.capture_written && result.capture_png_bytes != 0u) ||
            (result.capture_written &&
             (!result.capture_armed || result.capture_png_bytes == 0u ||
              found->second.capturePng.empty())) ||
            result.inspection_pose_fallback_ticks >
                result.inspection_pose_ticks) {
            ImGui::TextColored(
                AppTheme::bad(),
                "The engine returned an invalid pose-inspection contract.");
            ui::TextSubtleWrapped(
                "No performance evidence or fit conclusion was saved.");
            ui::CardEnd();
            return;
        }
        ImGui::Text("%s  •  phase %.1f%%",
                    pose->label, result.pose_phase_milli / 10.0);
        ImGui::Text(
            "%s light  •  vehicle view yaw %d° · pitch %d°",
            lighting->label, result.view_yaw_degrees,
            result.view_pitch_degrees);
        const bool renderedCharacter = result.replacement_draws != 0u;
        const bool poseMeasured = result.inspection_pose_ticks != 0u;
        const bool exactPose = poseMeasured &&
            result.inspection_pose_fallback_ticks == 0u;
        ImGui::PushStyleColor(
            ImGuiCol_Text,
            renderedCharacter && exactPose
                ? AppTheme::good() : AppTheme::accent());
        ImGui::TextUnformatted(
            !renderedCharacter
                ? "No character replacement — inspection inconclusive"
                : !poseMeasured
                    ? "Inspection ended before held-pose evidence began"
                : exactPose
                    ? "Exact pose inspection captured"
                    : "Requested semantic unavailable — source fallback shown");
        ImGui::PopStyleColor();
        if (!result.warmup_complete) {
            ui::TextSubtleWrapped(
                "The scene ended before the 120-tick warm-up boundary. Keep the inspection open longer before returning with F1.");
        }
        ImGui::Text("%llu replacement draws · %llu rendered parts · %llu inspected ticks",
                    result.replacement_draws,
                    result.replacement_primitives,
                    result.inspection_pose_ticks);
        if (result.inspection_pose_fallback_ticks != 0u) {
            ImGui::Text(
                "%llu ticks used source fallback instead of phase-scrubbable motion",
                result.inspection_pose_fallback_ticks);
        }
        if (result.view_yaw_degrees != 0 ||
            result.view_pitch_degrees != 0) {
            ImGui::Text(
                "%llu vehicle-camera override ticks",
                result.camera_override_ticks);
        }
        if (result.lighting != MDKR_WORKSHOP_PREVIEW_LIGHTING_NEUTRAL) {
            ImGui::Text(
                "%llu replacement draws used the inspection light",
                result.lighting_override_draws);
        }
        if (result.capture_requested) {
            if (result.capture_written) {
                ImGui::TextColored(
                    AppTheme::good(),
                    "PNG saved  •  %llu bytes  •  %llu stable frames",
                    result.capture_png_bytes,
                    result.capture_stable_frames);
                ui::TextSubtleUnformattedWrapped(
                    found->second.capturePng.c_str());
            } else if (!result.capture_armed) {
                ImGui::TextColored(
                    AppTheme::accent(),
                    "PNG not taken  •  camera settled for %llu / %u frames",
                    result.capture_stable_frames,
                    MDKR_CHARACTER_PREVIEW_CAPTURE_STABLE_FRAMES);
                ui::TextSubtleWrapped(
                    "Keep the inspection open through warm-up and 12 consecutive fully rendered character/view/light frames. No file was created or replaced.");
            } else {
                ImGui::TextColored(
                    AppTheme::bad(),
                    "PNG capture failed — no existing file was replaced");
                ui::TextSubtleWrapped(
                    "Choose a new writable filename and run the inspection again.");
            }
        }
        if (result.context != MDKR_CHARACTER_PREVIEW_SELECT &&
            result.contact_solves != 0u) {
            ImGui::Text(
                "Contact reach: %.2f mm mean · %.2f mm maximum across %llu solves",
                result.contact_error_mean_micrometres / 1000.0,
                result.contact_error_max_micrometres / 1000.0,
                result.contact_solves);
        }
        ui::TextSubtleWrapped(
            exactPose
                ? "This session-only visual proof may support fit review, but it is intentionally excluded from the durable performance matrix and pinned baselines. Run the live qualification route for timing evidence."
                : "Fallback or pre-measurement inspection cannot approve the current fit. Map the semantic or review the humanoid rig, then inspect again; run the live qualification route for timing evidence.");
        ui::CardEnd();
        return;
    }
    if (result.pose_phase_milli != 0u ||
        result.inspection_pose_ticks != 0u ||
        result.inspection_pose_fallback_ticks != 0u ||
        result.view_yaw_degrees != 0 ||
        result.view_pitch_degrees != 0 ||
        result.lighting != MDKR_WORKSHOP_PREVIEW_LIGHTING_NEUTRAL ||
        result.camera_override_ticks != 0u ||
        result.lighting_override_draws != 0u ||
        result.capture_requested || result.capture_armed ||
        result.capture_stable_frames != 0u || result.capture_written ||
        result.capture_png_bytes != 0u) {
        ImGui::TextColored(
            AppTheme::bad(),
            "Mixed live-test and pose-inspection result — diagnostic only");
        ui::TextSubtleWrapped(
            "The engine result contract was internally inconsistent. It cannot approve fit, enter the performance matrix, or replace a pinned baseline.");
        ui::CardEnd();
        return;
    }
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
    const bool renderedCharacter = result.replacement_draws != 0u;
    const bool qualified = result.realtime && enoughSamples &&
        renderedCharacter;
    ImGui::PushStyleColor(
        ImGuiCol_Text, qualified ? AppTheme::good() : AppTheme::accent());
    ImGui::TextUnformatted(
        qualified ? "Steady-state sample captured"
                  : (!renderedCharacter
                         ? "No character replacement — diagnostic only"
                         : (!result.realtime
                                ? "Synthetic pacing — diagnostic only"
                                : "Short sample — diagnostic only")));
    ImGui::PopStyleColor();
    if (ImGui::BeginTable("##character-preview-measurement", 2,
                          ImGuiTableFlags_SizingStretchProp |
                          ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn(
            "Metric", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn(
            "Measured value", ImGuiTableColumnFlags_WidthStretch, 1.0f);
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

void drawCharacterVisualCaptureTray(
    const MdkrModernCharacterEntry *entry,
    CharacterCaptureEdit &edit) {
    auto &captures = g_characterVisualCaptures[entry->id];
    ImGui::SeparatorText("Visual qualification report");
    if (captures.empty()) {
        ui::TextSubtleWrapped(
            "Successful one-shot inspections appear here with their exact context, pose, view, light, source, and fit identity. Capture several views, then export one portable contact sheet.");
        return;
    }

    ImGui::Text("%zu captured %s in this Workshop session",
                captures.size(), captures.size() == 1u ? "view" : "views");
    ui::TextSubtleWrapped(
        "The list is session-only metadata. Its PNG files are independent local documents; removing an entry or closing the Workshop never deletes them.");

    const size_t shown = std::min<size_t>(captures.size(), 8u);
    size_t removeIndex = captures.size();
    for (size_t row = 0u; row < shown; ++row) {
        const size_t index = captures.size() - 1u - row;
        const CharacterVisualReport::Capture &capture = captures[index];
        ImGui::PushID(static_cast<int>(index));
        if (ui::CardBegin("##character-visual-capture", AppTheme::surface(),
                          0.0f)) {
            ImGui::Text("%s · %s · phase %.1f%%",
                        capture.context.c_str(), capture.pose.c_str(),
                        capture.phaseMilli / 10.0);
            ImGui::TextDisabled(
                "%s light · view %d°/%d° · %u×%u · %s",
                capture.lighting.c_str(), capture.viewYawDegrees,
                capture.viewPitchDegrees, capture.width, capture.height,
                capture.exactPose ? "exact semantic" : "source fallback");
            ImGui::TextDisabled("Source %.8s · fit %.8s",
                                capture.sourceSha256.c_str(),
                                capture.fitSha256.c_str());
            ui::TextSubtleUnformattedWrapped(capture.pngPath.c_str());
            if (ImGui::Button("Use for portrait")) {
                g_characterPendingPortraitSources[entry->id] = {
                    capture.pngPath, true,
                };
                persistCharacterWorkshopTab(
                    CharacterWorkshopTab::Identity, true);
                setStatus(
                    "Exact-renderer capture sent to Portrait Studio; frame the subject before applying it.",
                    AppTheme::good());
            }
            ui::SpeakFocusedItem(
                "Use capture for portrait", nullptr,
                "Opens Portrait Studio with this exact stabilized PNG as a reversible local source. The capture file is not changed or deleted.");
            ImGui::SameLine();
            if (ImGui::Button("Remove from report")) removeIndex = index;
            ui::SpeakFocusedItem(
                "Remove from report", nullptr,
                "Removes only this session's report entry. The PNG file remains untouched.");
            ui::CardEnd();
        } else {
            ui::CardEnd();
        }
        ImGui::PopID();
    }
    if (captures.size() > shown) {
        ImGui::TextDisabled(
            "%zu earlier views are included in export but collapsed here.",
            captures.size() - shown);
    }
    if (removeIndex < captures.size()) {
        captures.erase(captures.begin() +
                       static_cast<std::ptrdiff_t>(removeIndex));
        setStatus(
            "Capture removed from the session report; its PNG file was not deleted.",
            AppTheme::subtle());
    }

    ImGui::SetNextItemWidth(
        filedialog::isAvailable()
            ? std::max(120.0f, ImGui::GetContentRegionAvail().x -
                                  ui::kBtnSecondary().x - ui::kGapS)
            : -1.0f);
    ImGui::InputTextWithHint(
        "##character-visual-report-path",
        "/path/to/character-visual-report.html",
        edit.reportPath, sizeof(edit.reportPath));
    ui::SpeakFocusedItem(
        "Visual report path", nullptr,
        "Names a new self-contained HTML report. Existing files are always preserved.");
    if (filedialog::isAvailable()) {
        ImGui::SameLine();
        if (ImGui::Button("Choose report...", ui::kBtnSecondary())) {
            std::string path;
            if (filedialog::saveCharacterReport(path)) {
                std::snprintf(edit.reportPath, sizeof(edit.reportPath), "%s",
                              path.c_str());
            }
        }
        ui::SpeakFocusedItem(
            "Choose report", nullptr,
            "Opens the operating system save panel for a new portable HTML contact sheet.");
    }
    const bool reportPathReady = edit.reportPath[0] != '\0';
    if (!reportPathReady) ImGui::BeginDisabled();
    if (ImGui::Button("Export self-contained report") && reportPathReady) {
        const std::string destination = edit.reportPath;
        std::string error;
        if (CharacterVisualReport::exportHtml(
                destination, entry->id, entry->display_name,
                captures, error)) {
            edit.reportPath[0] = '\0';
            setStatus(
                ("Visual report exported without model or ROM bytes: " +
                 destination).c_str(),
                AppTheme::good());
        } else {
            setStatus(
                ("Visual report was not created: " + error).c_str(),
                AppTheme::bad());
        }
    }
    if (!reportPathReady) ImGui::EndDisabled();
    ui::SpeakFocusedItem(
        "Export self-contained report",
        reportPathReady ? nullptr : "Choose a new HTML filename first.",
        "Embeds every listed PNG and machine-readable qualification metadata in one portable HTML file. It contains no model, package, or ROM bytes.");
    ImGui::SameLine();
    if (ImGui::Button("Clear capture list")) {
        captures.clear();
        setStatus(
            "Session capture list cleared; no PNG or report file was deleted.",
            AppTheme::subtle());
    }
    ui::SpeakFocusedItem(
        "Clear capture list", nullptr,
        "Clears only the session report entries and never deletes the PNG files.");
}

bool characterTestEvidenceCurrent(
    const MdkrModernCharacterEntry *entry,
    const CharacterTuningEdit &tuning,
    const CharacterTestEvidenceStore::Evidence &evidence,
    const std::string &presentationSignature) {
    return evidence.resultVersion == MDKR_CHARACTER_PREVIEW_RESULT_VERSION &&
           evidence.buildVersion == AppVersion() &&
           evidence.presentationSha256 == presentationSignature &&
           characterTestEvidenceMatchesFit(entry, tuning, evidence);
}

const char *characterTestEvidenceState(
    const MdkrModernCharacterEntry *entry,
    const CharacterTuningEdit &tuning,
    const CharacterTestEvidenceStore::Evidence *evidence,
    const std::string &presentationSignature) {
    if (evidence == nullptr) return "Not run";
    if (!characterTestEvidenceMatchesFit(entry, tuning, *evidence)) {
        return "Stale";
    }
    if (evidence->resultVersion != MDKR_CHARACTER_PREVIEW_RESULT_VERSION ||
        evidence->buildVersion != AppVersion() ||
        evidence->presentationSha256 != presentationSignature) {
        return "Setup changed";
    }
    if (!evidence->started) return "Did not start";
    if (!evidence->warmupComplete) return "Warm-up";
    if (evidence->replacementDraws == 0u) return "No character";
    if (evidence->adapter.empty()) return "Device unknown";
    if (!evidence->realtime) return "Synthetic";
    if (evidence->intervalSamples < 60u) return "Short";
    return "Qualified";
}

bool pinCharacterTestBaseline(
    const CharacterTestEvidenceStore::Evidence &latest) {
    loadCharacterTestEvidence();
    if (!g_characterTestEvidenceWritable ||
        latest.kind != CharacterTestEvidenceStore::Kind::Latest ||
        !CharacterTestEvidenceStore::qualified(latest)) {
        return false;
    }
    CharacterTestEvidenceStore::Inventory replacement =
        g_characterTestEvidence;
    CharacterTestEvidenceStore::Evidence baseline = latest;
    baseline.kind = CharacterTestEvidenceStore::Kind::Baseline;
    std::string error;
    if (!CharacterTestEvidenceStore::upsert(
            replacement, std::move(baseline), error)) {
        g_characterTestEvidenceError = error;
        return false;
    }
    return replaceCharacterTestEvidence(std::move(replacement));
}

bool clearCharacterTestBaseline(
    const CharacterTestEvidenceStore::Evidence &baseline) {
    CharacterTestEvidenceStore::Inventory replacement =
        g_characterTestEvidence;
    if (!CharacterTestEvidenceStore::erase(
            replacement, baseline.packageId, baseline.context,
            baseline.players,
            CharacterTestEvidenceStore::Kind::Baseline)) {
        return false;
    }
    return replaceCharacterTestEvidence(std::move(replacement));
}

void drawCharacterTestEvidenceMatrix(
    const MdkrModernCharacterEntry *entry,
    const CharacterTuningEdit &tuning) {
    static const char *contextNames[] = {
        "Character select", "Car", "Hovercraft", "Plane",
    };
    loadCharacterTestEvidence();
    const char *smokeAction = std::getenv(
        "MDKR_APP_SMOKE_CHARACTER_TEST_EVIDENCE_ACTION");
    const char *smokeToken = std::getenv(
        "MDKR_APP_SMOKE_CHARACTER_TEST_EVIDENCE_TOKEN");
    // Test-only lifecycle seam. The explicit paired token prevents an
    // ordinary launcher environment from accidentally manufacturing or
    // deleting qualification evidence.
    // The rendered gate drives the same publication and transactional storage
    // paths as the real engine result and visible controls below.
    if (!g_characterTestEvidenceSmokeActionApplied &&
        smokeAction != nullptr && smokeToken != nullptr &&
        std::strcmp(
            smokeToken, "mdkr64-character-test-evidence-v1") == 0) {
        g_characterTestEvidenceSmokeActionApplied = true;
        bool applied = false;
        if (std::strcmp(smokeAction, "publish-qualified") == 0 ||
            std::strcmp(smokeAction, "publish-inspection") == 0 ||
            std::strcmp(
                smokeAction, "publish-inspection-capture") == 0 ||
            std::strcmp(
                smokeAction, "publish-inspection-fallback") == 0 ||
            std::strcmp(smokeAction, "publish-mixed-mode") == 0 ||
            std::strcmp(smokeAction, "publish-invalid-fit") == 0 ||
            std::strcmp(
                smokeAction, "publish-stale-fit-session") == 0) {
            MdkrCharacterPreviewResult result{};
            result.version = MDKR_CHARACTER_PREVIEW_RESULT_VERSION;
            result.started = 1;
            result.warmup_complete = 1;
            result.realtime = 1;
            result.context = MDKR_CHARACTER_PREVIEW_CAR;
            result.players = 4;
            result.warmup_ticks = 120u;
            result.interval_samples = 180u;
            result.displayed_frames = 181u;
            result.interval_p50_us = 16650u;
            result.interval_p95_us = 17100u;
            result.interval_p99_us = 18250u;
            result.interval_mean_us = 16800u;
            result.interval_max_us = 20000u;
            result.tickwall_samples = 180u;
            result.tickwall_mean_ns = 1200000u;
            result.replacement_draws = 720u;
            result.replacement_primitives = 1440u;
            result.hidden_donor_batches = 720u;
            result.contact_solves = 1440u;
            result.contact_error_mean_micrometres = 1200u;
            result.contact_error_max_micrometres = 3400u;
            result.fit_diagnostics_valid = 1;
            result.fit_bounds_min_micrometres[0] = -400000;
            result.fit_bounds_min_micrometres[1] = -600000;
            result.fit_bounds_min_micrometres[2] = -300000;
            result.fit_bounds_max_micrometres[0] = 400000;
            result.fit_bounds_max_micrometres[1] = 900000;
            result.fit_bounds_max_micrometres[2] = 300000;
            result.fit_anchor_micrometres[0] = 10000;
            result.fit_anchor_micrometres[1] = 20000;
            result.fit_anchor_micrometres[2] = -30000;
            result.fit_forward_milli[2] = 1000;
            std::snprintf(result.renderer_backend,
                          sizeof(result.renderer_backend), "%s",
                          "webgpu-test");
            std::snprintf(result.adapter, sizeof(result.adapter), "%s",
                          "Rendered evidence fixture GPU");
            std::snprintf(result.driver, sizeof(result.driver), "%s",
                          "fixture-driver");
            result.vendor_id = 0x106Bu;
            result.device_id = 0x1234u;
            result.output_width = 1280u;
            result.output_height = 960u;
            result.render_width = 2560u;
            result.render_height = 1920u;
            const bool inspection =
                std::strcmp(smokeAction, "publish-inspection") == 0 ||
                std::strcmp(
                    smokeAction, "publish-inspection-capture") == 0 ||
                std::strcmp(
                    smokeAction, "publish-inspection-fallback") == 0;
            const bool inspectionCapture = std::strcmp(
                smokeAction, "publish-inspection-capture") == 0;
            const bool inspectionFallback = std::strcmp(
                smokeAction, "publish-inspection-fallback") == 0;
            const bool mixedMode = std::strcmp(
                smokeAction, "publish-mixed-mode") == 0;
            const bool invalidFit = std::strcmp(
                smokeAction, "publish-invalid-fit") == 0;
            if (invalidFit) result.fit_diagnostics_valid = 2;
            if (inspection) {
                result.pose = MDKR_CHARACTER_PREVIEW_POSE_RACE_STEER;
                result.pose_phase_milli = 250u;
                result.inspection_pose_ticks = 180u;
                result.inspection_pose_fallback_ticks =
                    inspectionFallback ? 180u : 0u;
                if (inspectionCapture) {
                    result.view_yaw_degrees = 90;
                    result.view_pitch_degrees = 15;
                    result.lighting =
                        MDKR_WORKSHOP_PREVIEW_LIGHTING_BRIGHT;
                    result.camera_override_ticks = 180u;
                    result.lighting_override_draws = 720u;
                    result.capture_requested = 1;
                    result.capture_armed = 1;
                    result.capture_stable_frames =
                        MDKR_CHARACTER_PREVIEW_CAPTURE_STABLE_FRAMES;
                    result.capture_written = 1;
                    result.capture_png_bytes = 123u;
                    result.output_width = 40u;
                    result.output_height = 40u;
                    const char *smokeReportPath = std::getenv(
                        "MDKR_APP_SMOKE_CHARACTER_VISUAL_REPORT");
                    if (smokeReportPath != nullptr) {
                        std::snprintf(
                            g_characterCaptureEdits[entry->id].reportPath,
                            sizeof(g_characterCaptureEdits[entry->id]
                                       .reportPath),
                            "%s", smokeReportPath);
                    }
                }
            } else if (mixedMode) {
                result.inspection_pose_ticks = 180u;
            }
            const std::string source =
                characterDigestHex(entry->source_sha256);
            const bool staleFit = std::strcmp(
                smokeAction, "publish-stale-fit-session") == 0;
            const std::string fit = staleFit
                ? std::string(64u, 'd')
                : characterFitReviewSignature(
                      entry, tuning, MDKR_CHARACTER_CONTEXT_CAR);
            const std::string presentation =
                characterTestPresentationSignature();
            const char *smokeCapturePath = inspectionCapture
                ? std::getenv(
                      "MDKR_APP_SMOKE_CHARACTER_INSPECTION_CAPTURE")
                : nullptr;
            const std::string capturePath = smokeCapturePath != nullptr
                ? smokeCapturePath : std::string();
            Settings_publishCharacterPreviewResult(
                entry->id, source, fit, presentation, capturePath, result);
            const auto session = g_characterPreviewResults.find(entry->id);
            const CharacterTestEvidenceStore::Evidence *latest =
                CharacterTestEvidenceStore::find(
                    g_characterTestEvidence, entry->id, 2u, 4u,
                    CharacterTestEvidenceStore::Kind::Latest);
            const bool sessionMatches =
                session != g_characterPreviewResults.end() &&
                characterPreviewSessionMatchesFit(
                    entry, tuning, MDKR_CHARACTER_PREVIEW_CAR,
                    session->second);
            if (invalidFit) {
                applied = session != g_characterPreviewResults.end() &&
                    latest == nullptr && !sessionMatches;
            } else if (mixedMode) {
                applied = session != g_characterPreviewResults.end() &&
                    latest == nullptr && !sessionMatches;
            } else if (inspection) {
                applied = session != g_characterPreviewResults.end() &&
                    latest == nullptr &&
                    session->second.result.pose ==
                        MDKR_CHARACTER_PREVIEW_POSE_RACE_STEER &&
                    (inspectionFallback ? !sessionMatches : sessionMatches) &&
                    (!inspectionCapture ||
                     (!capturePath.empty() &&
                      g_characterVisualCaptures[entry->id].size() == 1u));
            } else {
                applied = session != g_characterPreviewResults.end() &&
                    latest != nullptr && (!staleFit || !sessionMatches);
            }
        } else if (std::strcmp(smokeAction, "pin-car-4p") == 0) {
            const CharacterTestEvidenceStore::Evidence *latest =
                CharacterTestEvidenceStore::find(
                    g_characterTestEvidence, entry->id, 2u, 4u,
                    CharacterTestEvidenceStore::Kind::Latest);
            applied = latest != nullptr &&
                pinCharacterTestBaseline(*latest);
        } else if (std::strcmp(smokeAction, "clear-car-4p-baseline") == 0) {
            const CharacterTestEvidenceStore::Evidence *baseline =
                CharacterTestEvidenceStore::find(
                    g_characterTestEvidence, entry->id, 2u, 4u,
                    CharacterTestEvidenceStore::Kind::Baseline);
            applied = baseline != nullptr &&
                clearCharacterTestBaseline(*baseline);
        } else if (std::strcmp(smokeAction, "clear-package") == 0) {
            CharacterTestEvidenceStore::Inventory replacement =
                g_characterTestEvidence;
            applied = CharacterTestEvidenceStore::erasePackage(
                replacement, entry->id) != 0u &&
                replaceCharacterTestEvidence(std::move(replacement));
        }
        const size_t baselines = static_cast<size_t>(std::count_if(
            g_characterTestEvidence.records.begin(),
            g_characterTestEvidence.records.end(),
            [&](const CharacterTestEvidenceStore::Evidence &evidence) {
                return evidence.packageId == entry->id &&
                    evidence.kind ==
                        CharacterTestEvidenceStore::Kind::Baseline;
            }));
        std::fprintf(
            stderr,
            "[app-ui] character-test-evidence-action action=%s applied=%d records=%zu baselines=%zu\n",
            smokeAction, applied ? 1 : 0,
            g_characterTestEvidence.records.size(), baselines);
    }
    ImGui::SeparatorText("Exact test evidence");
    ui::TextSubtleWrapped(
        "Each cell is one real game context and local-player layout. Results are bound to the exact package source, fit, app build, presentation settings, render resolution, and GPU identity; stale or incomparable evidence stays visible instead of silently passing.");
    if (!g_characterTestEvidenceWritable) {
        if (!g_characterTestEvidenceErrorTracePrinted &&
            std::getenv("MDKR_APP_UI_TRACE") != nullptr) {
            std::fprintf(
                stderr,
                "[app-ui] character-test-evidence writable=0 error=%s\n",
                g_characterTestEvidenceError.c_str());
            g_characterTestEvidenceErrorTracePrinted = true;
        }
        ImGui::TextColored(
            AppTheme::bad(),
            "Saved test evidence is read-only: %s",
            g_characterTestEvidenceError.c_str());
        ui::TextSubtleWrapped(
            "The malformed or unreadable inventory is preserved byte-for-byte. This session's last result remains visible below, but it will not overwrite durable evidence.");
        return;
    }
    const std::string presentationSignature =
        characterTestPresentationSignature();
    unsigned &selectedCell =
        g_characterTestEvidenceSelectedCell[entry->id];
    if (selectedCell >= 16u) selectedCell = 0u;
    bool haveSelected = false;
    uint64_t newestTime = 0u;
    unsigned newestCell = selectedCell;
    unsigned applicableCells = 0u;
    unsigned qualifiedCells = 0u;
    if (ImGui::BeginTable(
            "##character-test-evidence-matrix", 5,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn(
            "Context", ImGuiTableColumnFlags_WidthStretch, 0.28f);
        for (int players = 1; players <= 4; ++players) {
            const std::string heading = std::to_string(players) + "P";
            ImGui::TableSetupColumn(
                heading.c_str(), ImGuiTableColumnFlags_WidthStretch, 0.18f);
        }
        ImGui::TableHeadersRow();
        for (uint32_t context = 1u; context <= 4u; ++context) {
            const bool applicable = context == 1u ||
                (tuning.vehicleMask & (1u << (context - 2u))) != 0u;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(contextNames[context - 1u]);
            for (uint32_t players = 1u; players <= 4u; ++players) {
                ImGui::TableNextColumn();
                const unsigned cell = (context - 1u) * 4u + players - 1u;
                const CharacterTestEvidenceStore::Evidence *evidence =
                    applicable
                        ? CharacterTestEvidenceStore::find(
                              g_characterTestEvidence, entry->id, context,
                              players,
                              CharacterTestEvidenceStore::Kind::Latest)
                        : nullptr;
                if (applicable) {
                    ++applicableCells;
                    if (evidence != nullptr &&
                        characterTestEvidenceCurrent(
                            entry, tuning, *evidence,
                            presentationSignature) &&
                        CharacterTestEvidenceStore::qualified(*evidence)) {
                        ++qualifiedCells;
                    }
                    if (evidence != nullptr &&
                        (!haveSelected || evidence->capturedUnix > newestTime)) {
                        newestTime = evidence->capturedUnix;
                        newestCell = cell;
                    }
                }
                ImGui::PushID(static_cast<int>(cell));
                if (!applicable) ImGui::BeginDisabled();
                const char *state = applicable
                    ? characterTestEvidenceState(
                          entry, tuning, evidence, presentationSignature)
                    : "N/A";
                if (ImGui::Selectable(
                        state, selectedCell == cell,
                        ImGuiSelectableFlags_None, ImVec2(-1.0f, 0.0f)) &&
                    applicable) {
                    selectedCell = cell;
                    haveSelected = true;
                }
                const std::string spokenState =
                    std::to_string(players) +
                    (players == 1u ? " player, " : " players, ") + state;
                ui::SpeakFocusedItem(
                    contextNames[context - 1u], spokenState.c_str(),
                    applicable
                        ? "Selects this exact test result for timing, device, fit, and baseline details. It does not run or approve a test."
                        : "This vehicle context is not enabled for the package.");
                if (!applicable) ImGui::EndDisabled();
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }
    if (!haveSelected && newestTime != 0u &&
        CharacterTestEvidenceStore::find(
            g_characterTestEvidence, entry->id, selectedCell / 4u + 1u,
            selectedCell % 4u + 1u,
            CharacterTestEvidenceStore::Kind::Latest) == nullptr) {
        selectedCell = newestCell;
    }
    ImGui::TextColored(
        qualifiedCells == applicableCells && applicableCells != 0u
            ? AppTheme::good() : AppTheme::accent(),
        "Current qualified matrix: %u of %u enabled context/layout cells",
        qualifiedCells, applicableCells);

    const uint32_t selectedContext = selectedCell / 4u + 1u;
    const uint32_t selectedPlayers = selectedCell % 4u + 1u;
    const CharacterTestEvidenceStore::Evidence *latest =
        CharacterTestEvidenceStore::find(
            g_characterTestEvidence, entry->id, selectedContext,
            selectedPlayers, CharacterTestEvidenceStore::Kind::Latest);
    const CharacterTestEvidenceStore::Evidence *baseline =
        CharacterTestEvidenceStore::find(
            g_characterTestEvidence, entry->id, selectedContext,
            selectedPlayers, CharacterTestEvidenceStore::Kind::Baseline);
    if (std::getenv("MDKR_APP_UI_TRACE") != nullptr &&
        g_characterTestEvidenceTracePackages.insert(entry->id).second) {
        std::fprintf(
            stderr,
            "[app-ui] character-test-matrix package=%s current=%u required=%u selected=%u:%u state=%s latest=%d baseline=%d comparable=%d fit=%d fitAnchorUm=%lld,%lld,%lld fitBoundsYUm=%lld,%lld fitForwardMilli=%d,%d,%d\n",
            entry->id, qualifiedCells, applicableCells, selectedContext,
            selectedPlayers,
            characterTestEvidenceState(
                entry, tuning, latest, presentationSignature),
            latest != nullptr ? 1 : 0,
            baseline != nullptr ? 1 : 0,
            latest != nullptr && baseline != nullptr &&
                    CharacterTestEvidenceStore::comparable(*latest, *baseline)
                ? 1 : 0,
            latest != nullptr && latest->fitDiagnosticsValid ? 1 : 0,
            static_cast<long long>(
                latest != nullptr ? latest->fitAnchorMicrometres[0] : 0),
            static_cast<long long>(
                latest != nullptr ? latest->fitAnchorMicrometres[1] : 0),
            static_cast<long long>(
                latest != nullptr ? latest->fitAnchorMicrometres[2] : 0),
            static_cast<long long>(
                latest != nullptr ? latest->fitBoundsMinMicrometres[1] : 0),
            static_cast<long long>(
                latest != nullptr ? latest->fitBoundsMaxMicrometres[1] : 0),
            latest != nullptr ? latest->fitForwardMilli[0] : 0,
            latest != nullptr ? latest->fitForwardMilli[1] : 0,
            latest != nullptr ? latest->fitForwardMilli[2] : 0);
    }
    if (latest != nullptr) {
        ImGui::PushID(static_cast<int>(selectedCell));
        ImGui::SeparatorText("Selected evidence");
        ImGui::Text(
            "%s · %u %s · %s",
            contextNames[selectedContext - 1u], selectedPlayers,
            selectedPlayers == 1u ? "player" : "players",
            characterTestEvidenceState(
                entry, tuning, latest, presentationSignature));
        ImGui::TextDisabled(
            "Captured Unix %llu · app %s · result contract v%u",
            static_cast<unsigned long long>(latest->capturedUnix),
            latest->buildVersion.c_str(), latest->resultVersion);
        ImGui::TextWrapped(
            "Device: %s · %s · vendor %04x / device %04x",
            latest->backend.c_str(),
            latest->adapter.empty() ? "unnamed adapter"
                                    : latest->adapter.c_str(),
            latest->vendorId, latest->deviceId);
        ImGui::TextDisabled(
            "Output %ux%u · rendered %ux%u · driver %s",
            latest->outputWidth, latest->outputHeight,
            latest->renderWidth, latest->renderHeight,
            latest->driver.empty() ? "not reported"
                                   : latest->driver.c_str());
        ImGui::Text(
            "Median %.2f ms · p95 %.2f ms · p99 %.2f ms · worst %.2f ms",
            latest->intervalP50Us / 1000.0,
            latest->intervalP95Us / 1000.0,
            latest->intervalP99Us / 1000.0,
            latest->intervalMaxUs / 1000.0);
        ImGui::TextDisabled(
            "%llu intervals · %llu replacement draws · %llu submitted parts",
            static_cast<unsigned long long>(latest->intervalSamples),
            static_cast<unsigned long long>(latest->replacementDraws),
            static_cast<unsigned long long>(latest->replacementPrimitives));
        if (selectedContext != 1u) {
            ImGui::TextDisabled(
                "%llu contact solves · %.2f / %.2f mm mean / maximum error",
                static_cast<unsigned long long>(latest->contactSolves),
                latest->contactErrorMeanMicrometres / 1000.0,
                latest->contactErrorMaxMicrometres / 1000.0);
        }
        ImGui::SeparatorText("Renderer fit");
        drawCharacterFitDiagnostics(
            characterPreviewResultFromEvidence(*latest), false);
        const bool currentQualified =
            characterTestEvidenceCurrent(
                entry, tuning, *latest, presentationSignature) &&
            CharacterTestEvidenceStore::qualified(*latest);
        if (!currentQualified) ImGui::BeginDisabled();
        if (ImGui::Button("Pin as comparison baseline") &&
            currentQualified) {
            if (pinCharacterTestBaseline(*latest)) {
                setStatus(
                    "Exact same-device comparison baseline saved for this context and layout.",
                    AppTheme::good());
                latest = CharacterTestEvidenceStore::find(
                    g_characterTestEvidence, entry->id, selectedContext,
                    selectedPlayers,
                    CharacterTestEvidenceStore::Kind::Latest);
                baseline = CharacterTestEvidenceStore::find(
                    g_characterTestEvidence, entry->id, selectedContext,
                    selectedPlayers,
                    CharacterTestEvidenceStore::Kind::Baseline);
            } else {
                setStatus(
                    "The comparison baseline could not be saved; existing evidence remains unchanged.",
                    AppTheme::bad());
            }
        }
        if (!currentQualified) ImGui::EndDisabled();
        ui::SpeakFocusedItem(
            "Pin as comparison baseline",
            currentQualified ? nullptr
                : "Requires a current real-time sample with at least 60 intervals and visible character replacement.",
            "Copies this exact result into the durable baseline slot for the same context and player layout. It never changes character or game settings.");
        if (baseline != nullptr) {
            ImGui::SeparatorText("Pinned baseline");
            ImGui::TextDisabled(
                "Captured Unix %llu · %s · %ux%u output / %ux%u render",
                static_cast<unsigned long long>(baseline->capturedUnix),
                baseline->adapter.empty() ? baseline->backend.c_str()
                                          : baseline->adapter.c_str(),
                baseline->outputWidth, baseline->outputHeight,
                baseline->renderWidth, baseline->renderHeight);
            if (baseline->fitDiagnosticsValid) {
                ImGui::TextDisabled("Pinned renderer fit:");
                drawCharacterFitDiagnostics(
                    characterPreviewResultFromEvidence(*baseline), true);
            } else {
                ImGui::TextDisabled(
                    "Pinned renderer fit: unavailable in legacy evidence");
            }
            if (CharacterTestEvidenceStore::comparable(*latest, *baseline)) {
                const auto delta = [](uint64_t value, uint64_t reference) {
                    return reference == 0u ? 0.0
                        : (static_cast<double>(value) -
                           static_cast<double>(reference)) * 100.0 /
                              static_cast<double>(reference);
                };
                ImGui::Text(
                    "Same-environment delta: median %+.1f%% · p95 %+.1f%% · p99 %+.1f%%",
                    delta(latest->intervalP50Us, baseline->intervalP50Us),
                    delta(latest->intervalP95Us, baseline->intervalP95Us),
                    delta(latest->intervalP99Us, baseline->intervalP99Us));
                ui::TextSubtleWrapped(
                    "Negative means a shorter displayed interval. This is a repeatable same-environment comparison, not a GPU-only cost or an automatic pass/fail verdict.");
            } else {
                ImGui::TextColored(
                    AppTheme::accent(),
                    "Not directly comparable: app build, presentation settings, GPU identity, or output/render dimensions differ.");
            }
            if (ImGui::Button("Clear pinned baseline")) {
                if (clearCharacterTestBaseline(*baseline)) {
                    setStatus(
                        "Pinned comparison baseline cleared; the latest result remains.",
                        AppTheme::subtle());
                    baseline = nullptr;
                } else {
                    setStatus(
                        "The pinned baseline could not be cleared; no evidence changed.",
                        AppTheme::bad());
                }
            }
            ui::SpeakFocusedItem(
                "Clear pinned baseline", nullptr,
                "Deletes only this context and layout's local comparison baseline. The latest result and character are preserved.");
        }
        ImGui::PopID();
    }
    if (ImGui::Button("Clear all evidence for this character...")) {
        ImGui::OpenPopup("Clear character test evidence?");
    }
    ui::SpeakFocusedItem(
        "Clear all evidence for this character", nullptr,
        "Opens a confirmation to delete only locally saved exact-test results and baselines for this package.");
    if (ImGui::BeginPopupModal(
            "Clear character test evidence?", nullptr,
            ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped(
            "Delete every latest result and pinned baseline for %s? The package, source, fit, author review, and external files are unchanged.",
            entry->display_name);
        if (ImGui::Button("Clear test evidence")) {
            CharacterTestEvidenceStore::Inventory replacement =
                g_characterTestEvidence;
            (void)CharacterTestEvidenceStore::erasePackage(
                replacement, entry->id);
            if (replaceCharacterTestEvidence(std::move(replacement))) {
                g_characterPreviewResults.erase(entry->id);
                g_characterTestEvidenceSelectedCell.erase(entry->id);
                setStatus(
                    "This character's local test evidence and baselines were cleared.",
                    AppTheme::subtle());
                ImGui::CloseCurrentPopup();
            } else {
                setStatus(
                    "Test evidence could not be cleared; no durable evidence changed.",
                    AppTheme::bad());
            }
        }
        ui::SpeakFocusedItem(
            "Clear test evidence", nullptr,
            "Permanently deletes only this package's local test-result matrix and pinned baselines.");
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ui::SpeakFocusedItem(
            "Cancel", nullptr,
            "Keeps every exact-test result and comparison baseline.");
        ImGui::EndPopup();
    }
}

void drawCharacterExactTests(const MdkrModernCharacterEntry *entry,
                             bool compact) {
    if (compact) {
        ui::TextSubtleWrapped(
            "Return to the launcher Workshop to start an exact game-context test. A running engine cannot safely start a second engine inside itself.");
        return;
    }
    int &players = g_characterTestPlayers[entry->id];
    int &inspectionPose = g_characterTestPoses.try_emplace(
        entry->id, MDKR_CHARACTER_PREVIEW_POSE_RACE_STEER).first->second;
    int &inspectionPhase = g_characterTestPosePhases.try_emplace(
        entry->id, 500).first->second;
    int &viewYaw = g_characterTestViewYawDegrees.try_emplace(
        entry->id, 0).first->second;
    int &viewPitch = g_characterTestViewPitchDegrees.try_emplace(
        entry->id, 0).first->second;
    int &inspectionLighting = g_characterTestLighting.try_emplace(
        entry->id, MDKR_WORKSHOP_PREVIEW_LIGHTING_NEUTRAL).first->second;
    CharacterCaptureEdit &capture = g_characterCaptureEdits[entry->id];
    const CharacterTuningEdit &tuning = loadCharacterTuning(0, entry->id);
    if (players < 1 || players > 4) players = 1;
    if (inspectionPose <= MDKR_CHARACTER_PREVIEW_POSE_LIVE ||
        inspectionPose >= MDKR_CHARACTER_PREVIEW_POSE_COUNT) {
        inspectionPose = MDKR_CHARACTER_PREVIEW_POSE_RACE_STEER;
    }
    if (inspectionPhase < 0 || inspectionPhase > 1000) {
        inspectionPhase = 500;
    }
    if (viewYaw < -180 || viewYaw > 180) viewYaw = 0;
    if (viewPitch < -45 || viewPitch > 45) viewPitch = 0;
    if (inspectionLighting < MDKR_WORKSHOP_PREVIEW_LIGHTING_NEUTRAL ||
        inspectionLighting >= MDKR_WORKSHOP_PREVIEW_LIGHTING_COUNT) {
        inspectionLighting = MDKR_WORKSHOP_PREVIEW_LIGHTING_NEUTRAL;
    }
    if (std::getenv("MDKR_APP_UI_TRACE") != nullptr &&
        g_characterPoseInspectionTracePackages.insert(entry->id).second) {
        std::fprintf(
            stderr,
            "[app-ui] character-pose-inspector package=%s semantics=%zu defaultPose=%d defaultPhase=%d view=%d,%d lighting=%d capture=png-create-only performanceEvidence=session-excluded\n",
            entry->id, std::size(kCharacterInspectionPoses),
            inspectionPose, inspectionPhase, viewYaw, viewPitch,
            inspectionLighting);
    }
    CharacterHistoryFrame history = beginCharacterHistory(
        entry, CharacterHistoryTool::Test);
    ui::TextSubtleWrapped(
        "Launch this package directly into the real game renderer with its saved fit. The test is temporary: it does not replace Player assignments or skip the final ROM integrity check. For a useful timing sample, stay at least three seconds beyond the 120-tick warm-up; opening F1 freezes the sample before you navigate back.");
    drawCharacterTestEvidenceMatrix(entry, tuning);
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
    const float testActionWidth = ImGui::GetContentRegionAvail().x;
    const int testActionColumns =
        testActionWidth >= ui::kPairMinWidth() * 4.0f ? 4
        : testActionWidth >= ui::kPairMinWidth() * 2.0f ? 2 : 1;
    const bool carQualified = (tuning.vehicleMask & 1u) != 0u;
    const bool hoverQualified = (tuning.vehicleMask & 2u) != 0u;
    const bool planeQualified = (tuning.vehicleMask & 4u) != 0u;
    if (ImGui::BeginTable(
            "##character-live-test-actions", testActionColumns,
            ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableNextColumn();
        if (ImGui::Button("Character select", ui::kBtnFullWidth())) {
            requestCharacterPreview(entry, MDKR_CHARACTER_PREVIEW_SELECT,
                                    players);
        }
        ui::SpeakFocusedItem(
            "Character select", nullptr,
            "Tests the selected package in the exact character select scene.");
        ImGui::TableNextColumn();
        if (!carQualified) ImGui::BeginDisabled();
        if (ImGui::Button("Car", ui::kBtnFullWidth()) && carQualified) {
            requestCharacterPreview(entry, MDKR_CHARACTER_PREVIEW_CAR,
                                    players);
        }
        if (!carQualified) ImGui::EndDisabled();
        ui::SpeakFocusedItem(
            "Car", carQualified ? nullptr : "Disabled for this package.",
            "Tests the selected package in a real car race.");
        ImGui::TableNextColumn();
        if (!hoverQualified) ImGui::BeginDisabled();
        if (ImGui::Button("Hovercraft", ui::kBtnFullWidth()) &&
            hoverQualified) {
            requestCharacterPreview(
                entry, MDKR_CHARACTER_PREVIEW_HOVERCRAFT, players);
        }
        if (!hoverQualified) ImGui::EndDisabled();
        ui::SpeakFocusedItem(
            "Hovercraft",
            hoverQualified ? nullptr : "Not supported by this package.",
            "Tests the selected package in a real hovercraft race.");
        ImGui::TableNextColumn();
        if (!planeQualified) ImGui::BeginDisabled();
        if (ImGui::Button("Plane", ui::kBtnFullWidth()) && planeQualified) {
            requestCharacterPreview(entry, MDKR_CHARACTER_PREVIEW_PLANE,
                                    players);
        }
        if (!planeQualified) ImGui::EndDisabled();
        ui::SpeakFocusedItem(
            "Plane",
            planeQualified ? nullptr : "Not supported by this package.",
            "Tests the selected package in a real plane race.");
        ImGui::EndTable();
    }

    ImGui::SeparatorText("Pose inspection");
    ui::TextSubtleWrapped(
        "Freeze any supported animation semantic at an exact normalized phase in the real game renderer. Use this to inspect grounding, facing, seat placement, silhouette, deformation, and hand or foot reach. Inspection results stay in this session and never replace performance evidence or pinned baselines.");
    const auto selectedPose = std::find_if(
        std::begin(kCharacterInspectionPoses),
        std::end(kCharacterInspectionPoses),
        [inspectionPose](const CharacterInspectionPose &candidate) {
            return candidate.pose == inspectionPose;
        });
    const CharacterInspectionPose &pose =
        selectedPose != std::end(kCharacterInspectionPoses)
            ? *selectedPose : kCharacterInspectionPoses[3];
    if (ImGui::BeginCombo("Semantic pose", pose.label)) {
        for (const CharacterInspectionPose &candidate :
             kCharacterInspectionPoses) {
            const bool selected = candidate.pose == inspectionPose;
            if (ImGui::Selectable(candidate.label, selected)) {
                inspectionPose = candidate.pose;
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    ui::SpeakFocusedItem(
        "Semantic pose", nullptr,
        "Chooses the authored clip or reviewed humanoid reference motion to hold in the exact renderer.");
    (void)ImGui::SliderInt(
        "Normalized phase", &inspectionPhase, 0, 1000,
        "%d / 1000", ImGuiSliderFlags_AlwaysClamp);
    ui::SpeakFocusedItem(
        "Normalized phase", nullptr,
        "Scrubs from the beginning to the end of the selected semantic without changing animation speed.");
    ImGui::TextDisabled("Runtime semantic: %s", pose.semantic);

    ImGui::SeparatorText("Inspection view and light");
    ui::TextSubtleWrapped(
        "Gameplay view uses the ordinary vehicle camera. Every other preset is an absolute racer-relative orbit around the imported model's fitted bounds, automatically pulls back for the full vehicle, suppresses transient cutscene cameras, and still passes through the existing obstruction resolver. Character select keeps its authored camera. Lighting changes only the imported character, never the course, vehicle, simulation, or saved package.");
    (void)ImGui::SliderInt(
        "Vehicle camera yaw", &viewYaw, -180, 180, "%d degrees",
        ImGuiSliderFlags_AlwaysClamp);
    ui::SpeakFocusedItem(
        "Vehicle camera yaw", nullptr,
        "Sets a repeatable racer-relative inspection angle. Zero with zero pitch uses the ordinary gameplay camera; saved fit is unchanged.");
    (void)ImGui::SliderInt(
        "Vehicle camera pitch", &viewPitch, -45, 45, "%d degrees",
        ImGuiSliderFlags_AlwaysClamp);
    ui::SpeakFocusedItem(
        "Vehicle camera pitch", nullptr,
        "Moves the exact vehicle camera above or below its ordinary view without changing saved fit.");
    if (ImGui::BeginTable(
            "##character-view-presets",
            testActionColumns,
            ImGuiTableFlags_SizingStretchSame)) {
        const auto viewPreset = [&](const char *label, int yaw, int pitch) {
            ImGui::TableNextColumn();
            if (ImGui::Button(label, ui::kBtnFullWidth())) {
                viewYaw = yaw;
                viewPitch = pitch;
            }
            ui::SpeakFocusedItem(
                label, nullptr,
                "Sets a repeatable vehicle inspection orbit; the saved character transform is unchanged.");
        };
        viewPreset("Gameplay view", 0, 0);
        viewPreset("Front view", 180, 0);
        viewPreset("Left view", -90, 0);
        viewPreset("Right view", 90, 0);
        ImGui::EndTable();
    }
    const CharacterInspectionLighting &selectedLighting =
        kCharacterInspectionLighting[inspectionLighting];
    if (ImGui::BeginCombo("Character lighting", selectedLighting.label)) {
        for (const CharacterInspectionLighting &candidate :
             kCharacterInspectionLighting) {
            const bool selected = candidate.lighting == inspectionLighting;
            if (ImGui::Selectable(candidate.label, selected)) {
                inspectionLighting = candidate.lighting;
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    ui::SpeakFocusedItem(
        "Character lighting", selectedLighting.help,
        "Chooses a deterministic light applied only to the custom character in the exact renderer.");

    ImGui::SeparatorText("One-shot capture");
    (void)ImGui::Checkbox(
        "Save stabilized PNG during next inspection", &capture.enabled);
    ui::SpeakFocusedItem(
        "Save stabilized PNG during next inspection",
        capture.enabled && capture.pngPath[0] == '\0'
            ? "Choose a new PNG filename before starting." : nullptr,
        "Creates exactly one PNG after 120 warm-up ticks and 12 consecutive fully rendered character/view/light frames; it never overwrites an existing file.");
    ImGui::SetNextItemWidth(
        filedialog::isAvailable()
            ? std::max(120.0f, ImGui::GetContentRegionAvail().x -
                                  ui::kBtnSecondary().x - ui::kGapS)
            : -1.0f);
    ImGui::InputTextWithHint(
        "##character-inspection-capture-path",
        "/path/to/character-inspection.png",
        capture.pngPath, sizeof(capture.pngPath));
    ui::SpeakFocusedItem(
        "Capture PNG path", nullptr,
        "Names a new local PNG; an existing file is always preserved.");
    if (filedialog::isAvailable()) {
        ImGui::SameLine();
        if (ImGui::Button("Choose PNG...", ui::kBtnSecondary())) {
            std::string path;
            if (filedialog::saveCharacterCapture(path)) {
                std::snprintf(capture.pngPath, sizeof(capture.pngPath), "%s",
                              path.c_str());
            }
        }
        ui::SpeakFocusedItem(
            "Choose PNG", nullptr,
            "Opens the operating system save panel for a new inspection capture.");
    }

    const auto inspectButton = [&](const char *label,
                                   MdkrCharacterPreviewContext context,
                                   bool enabled, const char *disabledReason) {
        ImGui::TableNextColumn();
        if (!enabled) ImGui::BeginDisabled();
        if (ImGui::Button(label, ui::kBtnFullWidth()) && enabled) {
            const bool vehicle = context != MDKR_CHARACTER_PREVIEW_SELECT;
            requestCharacterPreview(
                entry, context, players,
                static_cast<MdkrCharacterPreviewPose>(inspectionPose),
                static_cast<unsigned>(inspectionPhase),
                vehicle ? viewYaw : 0,
                vehicle ? viewPitch : 0,
                static_cast<MdkrWorkshopPreviewLighting>(
                    inspectionLighting),
                capture.enabled ? capture.pngPath : nullptr);
            if (g_characterPreviewRequested && capture.enabled) {
                capture.enabled = false;
            }
        }
        if (!enabled) ImGui::EndDisabled();
        ui::SpeakFocusedItem(
            label, enabled ? nullptr : disabledReason,
            "Opens the exact game scene and holds the selected semantic phase for visual fit review; no performance evidence is saved.");
    };
    if (ImGui::BeginTable(
            "##character-pose-inspection-actions", testActionColumns,
            ImGuiTableFlags_SizingStretchSame)) {
        inspectButton("Inspect character select",
                      MDKR_CHARACTER_PREVIEW_SELECT, true, nullptr);
        inspectButton("Inspect car", MDKR_CHARACTER_PREVIEW_CAR,
                      carQualified, "Car is disabled for this package.");
        inspectButton("Inspect hovercraft",
                      MDKR_CHARACTER_PREVIEW_HOVERCRAFT,
                      hoverQualified,
                      "Hovercraft is disabled for this package.");
        inspectButton("Inspect plane", MDKR_CHARACTER_PREVIEW_PLANE,
                      planeQualified, "Plane is disabled for this package.");
        ImGui::EndTable();
    }
    drawCharacterVisualCaptureTray(entry, capture);
    finishCharacterHistory(entry, history);
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

void drawCharacterProfileAuthorityCard(
    const MdkrModernCharacterEntry *entry,
    const CharacterProfileEdit &edit) {
    const bool donorDirty = edit.donor != entry->donor;
    ImGui::Text("%s appearance  →  %s gameplay profile",
                entry->display_name, donorName(edit.donor));
    if (donorDirty) {
        ImGui::SameLine();
        ImGui::TextColored(AppTheme::accent(), "draft");
    }
    ui::TextSubtleWrapped(
        "This is an ownership contract, not a stat copy. Saving records the "
        "chosen retail donor and package vehicle mask in a new source revision.");

    const bool wide = ImGui::GetContentRegionAvail().x >= 700.0f;
    const bool sideBySide = wide && ImGui::BeginTable(
        "##character-profile-authority-columns", 2,
        ImGuiTableFlags_SizingStretchSame);
    if (sideBySide) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
    }
    auto card = [](const char *id, const char *title, const ImVec4 &colour,
                   const char *line1, const char *line2, const char *line3,
                   const char *line4) {
        if (ui::CardBegin(id, colour, 0.0f)) {
            ImGui::PushStyleColor(ImGuiCol_Text, colour);
            ImGui::TextUnformatted(title);
            ImGui::PopStyleColor();
            const auto bullet = [](const char *text) {
                ImGui::Bullet();
                ImGui::SameLine();
                ui::TextSubtleWrapped("%s", text);
            };
            bullet(line1);
            bullet(line2);
            bullet(line3);
            bullet(line4);
        }
        ui::CardEnd();
    };
    card("##character-profile-presentation-owns", "Custom package owns",
         AppTheme::good(),
         "3D mesh, materials, rig mapping, animation and fit tuning",
         "Display name, 40 × 40 portrait and minimap colour",
         "Local character-select identity; selection audio stays neutral",
         "Presentation only; a missing package falls back to the retail actor");
    if (sideBySide) {
        ImGui::TableNextColumn();
    } else {
        ui::Gap(ui::kGapS);
    }
    card("##character-profile-donor-owns", "Built-in profile remains authoritative",
         AppTheme::accent(),
         "Weight, handling and per-vehicle acceleration curves",
         "Collision, hitbox, vehicle state, items and race simulation",
         "In-race character voice, horn and vehicle audio",
         "Ghost and network/rollback character ID");
    if (sideBySide) ImGui::EndTable();
    ui::TextSubtleWrapped(
        "Course records and adventure saves remain ordinary game data; they "
        "do not embed the custom package. Package negotiation for online peers "
        "is not implemented, so the donor is always the safe authoritative "
        "identity and presentation fallback.");
}

CharacterProfileEdit &loadCharacterProfileEdit(
    const MdkrModernCharacterEntry *entry) {
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
    return edit;
}

bool drawCharacterProfileStudio(const MdkrModernCharacterEntry *entry) {
    static const char *vehicleNames[] = {"Car", "Hovercraft", "Plane"};
    CharacterProfileEdit &edit = loadCharacterProfileEdit(entry);
    CharacterHistoryFrame history = beginCharacterHistory(
        entry, CharacterHistoryTool::Profile);
    ui::TextSubtleWrapped(
        "Choose which built-in racer supplies authoritative gameplay and which vehicle scenes this appearance supports. The package never copies or edits the donor's simulation tables.");
    ImGui::SetNextItemWidth(std::min(440.0f, ImGui::GetContentRegionAvail().x));
    const std::string selectedLabel = donorChoiceLabel(edit.donor);
    const bool profileComboOpen = ImGui::BeginCombo(
        "Built-in gameplay profile", selectedLabel.c_str());
    ui::SpeakFocusedItem(
        "Built-in gameplay profile", donorName(edit.donor),
        "Choose the built-in racer that supplies authoritative gameplay. "
        "The appearance package does not replace simulation stats.");
    if (profileComboOpen) {
        for (uint32_t donor = 0u; donor < 10u; ++donor) {
            const bool selected = edit.donor == donor;
            const std::string optionLabel = donorChoiceLabel(donor);
            if (ImGui::Selectable(optionLabel.c_str(), selected)) {
                edit.donor = donor;
            }
            char spoken[160];
            if (donorProfilesAvailable()) {
                const MdkrDonorGameplayProfile &profile =
                    g_donorGameplayProfiles.donor[donor];
                std::snprintf(
                    spoken, sizeof(spoken),
                    "effective weight %.4f, handling %.4f%s",
                    static_cast<double>(profile.weight),
                    static_cast<double>(profile.handling),
                    selected ? ", selected" : "");
            } else {
                std::snprintf(spoken, sizeof(spoken), "%s",
                              selected ? "selected" : "available");
            }
            ui::SpeakFocusedItem(
                donorName(donor), spoken,
                "Select this built-in authoritative gameplay profile.");
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    drawCharacterProfileAuthorityCard(entry, edit);
    drawDonorGameplayComparison(edit.donor, edit.comparisonVehicle);
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
    const bool stagingDraft = g_characterActiveDrafts.find(entry->id) !=
        g_characterActiveDrafts.end();
    if (!dirty || stagingDraft) ImGui::BeginDisabled();
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
    if (!dirty || stagingDraft) ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled(dirty ? "unsaved source revision" : "saved in package");
    if (stagingDraft) {
        ui::TextSubtleWrapped(
            "Gameplay and compatibility changes are staged in the named draft and will be compiled together with its identity and rig.");
    }
    if (!saved) finishCharacterHistory(entry, history);
    return saved;
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

void drawPortraitStudioCanvas(
    const CharacterPortraitStudio::Canvas &canvas, const char *label,
    float maximumExtent = 176.0f) {
    constexpr int size = CharacterPortraitStudio::kSize;
    ImGui::PushID(label);
    ImGui::BeginGroup();
    const float available = std::max(80.0f, ImGui::GetContentRegionAvail().x);
    const float pixelSize = std::clamp(
        std::floor(std::min(available, maximumExtent) / size), 2.0f, 5.0f);
    const float extent = pixelSize * size;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImDrawList *draw = ImGui::GetWindowDrawList();
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const ImU32 background = ((x / 4 + y / 4) & 1)
                ? IM_COL32(73, 79, 89, 255) : IM_COL32(48, 53, 62, 255);
            const ImVec2 minimum(origin.x + x * pixelSize,
                                 origin.y + y * pixelSize);
            draw->AddRectFilled(
                minimum, ImVec2(minimum.x + pixelSize,
                                minimum.y + pixelSize), background);
        }
        int x = 0;
        while (x < size) {
            const uint8_t *pixel = canvas.data() + (y * size + x) * 4;
            if (pixel[3] == 0u) {
                ++x;
                continue;
            }
            int end = x + 1;
            while (end < size &&
                   std::memcmp(pixel,
                               canvas.data() + (y * size + end) * 4,
                               4u) == 0) {
                ++end;
            }
            draw->AddRectFilled(
                ImVec2(origin.x + x * pixelSize, origin.y + y * pixelSize),
                ImVec2(origin.x + end * pixelSize,
                       origin.y + (y + 1) * pixelSize),
                IM_COL32(pixel[0], pixel[1], pixel[2], pixel[3]));
            x = end;
        }
    }
    draw->AddRect(origin, ImVec2(origin.x + extent, origin.y + extent),
                  IM_COL32(255, 255, 255, 140));
    ImGui::InvisibleButton("canvas", ImVec2(extent, extent));
    const std::string spokenLabel = std::string(label) + " portrait preview";
    ui::SpeakFocusedItem(
        spokenLabel.c_str(), "exact forty by forty pixels",
        "Preview only. Use the labelled recipe and pixel controls to change the draft.");
    ImGui::TextDisabled("%s · exact 40 × 40", label);
    ImGui::EndGroup();
    ImGui::PopID();
}

void refreshPortraitStylePreview(CharacterIdentityEdit &edit) {
    edit.stylePreview = CharacterPortraitStudio::applyRecipe(
        edit.styleSource, edit.styleRecipe);
    edit.stylePreviewValid = true;
}

bool refreshPortraitImportPreview(CharacterIdentityEdit &edit) {
    CharacterPortraitStudio::Canvas preview{};
    std::string error;
    if (!CharacterPortraitImport::render(
            edit.importImage, edit.importRecipe, preview, error)) {
        edit.importPreviewValid = false;
        edit.importError = std::move(error);
        return false;
    }
    edit.importPreview = preview;
    edit.importPreviewValid = true;
    edit.importPreviewDirty = false;
    edit.importError.clear();
    return true;
}

void commitPortraitImportSource(CharacterIdentityEdit &edit) {
    edit.portraitSourceRecord = CharacterPortraitImport::sourceRecord(
        edit.importImage, edit.importRecipe,
        edit.importFromExactRenderer
            ? CharacterPortraitImport::SourceKind::ExactRenderer
            : CharacterPortraitImport::SourceKind::LocalPng);
    std::snprintf(edit.portraitPath, sizeof(edit.portraitPath), "%s",
                  edit.importPath);
}

bool loadPortraitImportSource(CharacterIdentityEdit &edit,
                              const std::string &path,
                              bool exactRenderer) {
    CharacterPortraitImport::Image image;
    CharacterPortraitImport::Thumbnail thumbnail;
    std::string error;
    if (!CharacterPortraitImport::loadPng(path, image, error) ||
        !CharacterPortraitImport::makeThumbnail(image, thumbnail)) {
        edit.importError = error.empty()
            ? "The portrait source thumbnail could not be prepared." : error;
        return false;
    }
    CharacterPortraitImport::Recipe recipe =
        CharacterPortraitImport::centredRecipe(image);
    bool resolvedExactRenderer = exactRenderer;
    if (edit.portraitSourceRecord.kind !=
            CharacterPortraitImport::SourceKind::Canvas &&
        edit.portraitSourceRecord.sha256 == image.sha256 &&
        edit.portraitSourceRecord.width == image.width &&
        edit.portraitSourceRecord.height == image.height) {
        recipe = edit.portraitSourceRecord.recipe;
        resolvedExactRenderer = edit.portraitSourceRecord.kind ==
            CharacterPortraitImport::SourceKind::ExactRenderer;
    }
    CharacterPortraitStudio::Canvas preview{};
    if (!CharacterPortraitImport::render(image, recipe, preview, error)) {
        edit.importError = std::move(error);
        return false;
    }
    edit.importImage = std::move(image);
    edit.importThumbnail = std::move(thumbnail);
    edit.importRecipe = recipe;
    edit.importPreview = preview;
    edit.importPreviewValid = true;
    edit.importPreviewDirty = false;
    edit.importFromExactRenderer = resolvedExactRenderer;
    edit.importError.clear();
    CharacterEditHistory::clear(edit.importHistory);
    std::snprintf(edit.importPath, sizeof(edit.importPath), "%s", path.c_str());
    return true;
}

std::string portraitImportRecipePayload(
    const CharacterPortraitImport::Recipe &recipe) {
    const uint32_t values[] = {
        recipe.cropX, recipe.cropY, recipe.cropSize,
        recipe.edgeMatteTolerance,
        static_cast<uint32_t>(recipe.sampling),
        static_cast<uint32_t>(recipe.background),
    };
    std::string payload = "mdkr-portrait-frame-v1\n";
    for (uint32_t value : values) {
        for (unsigned shift = 0u; shift < 32u; shift += 8u) {
            payload.push_back(static_cast<char>(value >> shift));
        }
    }
    return payload;
}

bool decodePortraitImportRecipe(
    const std::string &payload,
    const CharacterPortraitImport::Image &image,
    CharacterPortraitImport::Recipe &recipe) {
    constexpr char header[] = "mdkr-portrait-frame-v1\n";
    constexpr size_t valueCount = 6u;
    if (payload.size() != sizeof(header) - 1u + valueCount * 4u ||
        payload.compare(0u, sizeof(header) - 1u, header) != 0) {
        return false;
    }
    uint32_t values[valueCount] = {};
    size_t offset = sizeof(header) - 1u;
    for (uint32_t &value : values) {
        for (unsigned shift = 0u; shift < 32u; shift += 8u) {
            value |= static_cast<uint32_t>(
                static_cast<unsigned char>(payload[offset++])) << shift;
        }
    }
    CharacterPortraitImport::Recipe parsed;
    parsed.cropX = values[0];
    parsed.cropY = values[1];
    parsed.cropSize = values[2];
    parsed.edgeMatteTolerance = values[3];
    parsed.sampling = static_cast<CharacterPortraitImport::Sampling>(
        values[4]);
    parsed.background = static_cast<CharacterPortraitImport::Background>(
        values[5]);
    if (!CharacterPortraitImport::validRecipe(image, parsed)) return false;
    recipe = parsed;
    return true;
}

bool clampPortraitImportCrop(CharacterIdentityEdit &edit) {
    if (!CharacterPortraitImport::validImage(edit.importImage)) return false;
    auto &recipe = edit.importRecipe;
    const uint32_t maximum = std::min(
        edit.importImage.width, edit.importImage.height);
    const uint32_t oldX = recipe.cropX;
    const uint32_t oldY = recipe.cropY;
    const uint32_t oldSize = recipe.cropSize;
    recipe.cropSize = std::clamp(recipe.cropSize, 1u, maximum);
    recipe.cropX = std::min(
        recipe.cropX, edit.importImage.width - recipe.cropSize);
    recipe.cropY = std::min(
        recipe.cropY, edit.importImage.height - recipe.cropSize);
    return oldX != recipe.cropX || oldY != recipe.cropY ||
           oldSize != recipe.cropSize;
}

bool drawPortraitImportThumbnail(CharacterIdentityEdit &edit) {
    const auto &thumbnail = edit.importThumbnail;
    if (thumbnail.width == 0u || thumbnail.height == 0u ||
        thumbnail.rgba.size() !=
            static_cast<size_t>(thumbnail.width) * thumbnail.height * 4u) {
        return false;
    }
    const float available = std::max(160.0f, ImGui::GetContentRegionAvail().x);
    const float scale = std::clamp(
        std::min(available, 360.0f) /
            static_cast<float>(thumbnail.width),
        1.0f, 5.0f);
    const ImVec2 extent(thumbnail.width * scale, thumbnail.height * scale);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(
        "##portrait-source-crop", extent,
        ImGuiButtonFlags_MouseButtonLeft);
    const bool hovered = ImGui::IsItemHovered();
    bool changed = false;
    if (ImGui::IsItemActivated()) {
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        const double sourceX = std::clamp(
            static_cast<double>((mouse.x - origin.x) / extent.x) *
                edit.importImage.width,
            0.0, static_cast<double>(edit.importImage.width - 1u));
        const double sourceY = std::clamp(
            static_cast<double>((mouse.y - origin.y) / extent.y) *
                edit.importImage.height,
            0.0, static_cast<double>(edit.importImage.height - 1u));
        const bool inside =
            sourceX >= edit.importRecipe.cropX &&
            sourceX < edit.importRecipe.cropX + edit.importRecipe.cropSize &&
            sourceY >= edit.importRecipe.cropY &&
            sourceY < edit.importRecipe.cropY + edit.importRecipe.cropSize;
        if (!inside) {
            const int64_t half = edit.importRecipe.cropSize / 2u;
            const int64_t nextX = static_cast<int64_t>(sourceX) - half;
            const int64_t nextY = static_cast<int64_t>(sourceY) - half;
            edit.importRecipe.cropX = static_cast<uint32_t>(std::clamp<int64_t>(
                nextX, 0, edit.importImage.width -
                              edit.importRecipe.cropSize));
            edit.importRecipe.cropY = static_cast<uint32_t>(std::clamp<int64_t>(
                nextY, 0, edit.importImage.height -
                              edit.importRecipe.cropSize));
            changed = true;
        }
        edit.importDragCropX = edit.importRecipe.cropX;
        edit.importDragCropY = edit.importRecipe.cropY;
        ImGui::ResetMouseDragDelta(ImGuiMouseButton_Left);
    }
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(
            ImGuiMouseButton_Left, 1.0f)) {
        const ImVec2 delta = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left);
        const int64_t dx = static_cast<int64_t>(std::lround(
            delta.x / extent.x * edit.importImage.width));
        const int64_t dy = static_cast<int64_t>(std::lround(
            delta.y / extent.y * edit.importImage.height));
        const int64_t maximumX = edit.importImage.width -
            edit.importRecipe.cropSize;
        const int64_t maximumY = edit.importImage.height -
            edit.importRecipe.cropSize;
        const uint32_t nextX = static_cast<uint32_t>(std::clamp<int64_t>(
            static_cast<int64_t>(edit.importDragCropX) + dx, 0, maximumX));
        const uint32_t nextY = static_cast<uint32_t>(std::clamp<int64_t>(
            static_cast<int64_t>(edit.importDragCropY) + dy, 0, maximumY));
        if (nextX != edit.importRecipe.cropX ||
            nextY != edit.importRecipe.cropY) {
            edit.importRecipe.cropX = nextX;
            edit.importRecipe.cropY = nextY;
            changed = true;
        }
    }
    if (hovered && ImGui::GetIO().MouseWheel != 0.0f) {
        const int64_t oldSize = edit.importRecipe.cropSize;
        const int64_t step = std::max<int64_t>(1, oldSize / 12);
        const int64_t maximum = std::min(
            edit.importImage.width, edit.importImage.height);
        const int64_t nextSize = std::clamp<int64_t>(
            oldSize + (ImGui::GetIO().MouseWheel < 0.0f ? step : -step),
            1, maximum);
        const int64_t centreX = edit.importRecipe.cropX + oldSize / 2;
        const int64_t centreY = edit.importRecipe.cropY + oldSize / 2;
        edit.importRecipe.cropSize = static_cast<uint32_t>(nextSize);
        edit.importRecipe.cropX = static_cast<uint32_t>(std::clamp<int64_t>(
            centreX - nextSize / 2, 0,
            edit.importImage.width - nextSize));
        edit.importRecipe.cropY = static_cast<uint32_t>(std::clamp<int64_t>(
            centreY - nextSize / 2, 0,
            edit.importImage.height - nextSize));
        changed = nextSize != oldSize;
    }

    ImDrawList *draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(
        origin, ImVec2(origin.x + extent.x, origin.y + extent.y),
        IM_COL32(42, 47, 56, 255));
    for (uint32_t y = 0u; y < thumbnail.height; ++y) {
        for (uint32_t x = 0u; x < thumbnail.width; ++x) {
            const uint8_t *pixel = thumbnail.rgba.data() +
                (static_cast<size_t>(y) * thumbnail.width + x) * 4u;
            if (pixel[3] == 0u) continue;
            draw->AddRectFilled(
                ImVec2(origin.x + x * scale, origin.y + y * scale),
                ImVec2(origin.x + (x + 1u) * scale,
                       origin.y + (y + 1u) * scale),
                IM_COL32(pixel[0], pixel[1], pixel[2], pixel[3]));
        }
    }
    const float x0 = origin.x + extent.x * edit.importRecipe.cropX /
        edit.importImage.width;
    const float y0 = origin.y + extent.y * edit.importRecipe.cropY /
        edit.importImage.height;
    const float x1 = origin.x + extent.x *
        (edit.importRecipe.cropX + edit.importRecipe.cropSize) /
        edit.importImage.width;
    const float y1 = origin.y + extent.y *
        (edit.importRecipe.cropY + edit.importRecipe.cropSize) /
        edit.importImage.height;
    const ImU32 shade = IM_COL32(8, 12, 18, 150);
    draw->AddRectFilled(origin, ImVec2(origin.x + extent.x, y0), shade);
    draw->AddRectFilled(
        ImVec2(origin.x, y1),
        ImVec2(origin.x + extent.x, origin.y + extent.y), shade);
    draw->AddRectFilled(ImVec2(origin.x, y0), ImVec2(x0, y1), shade);
    draw->AddRectFilled(ImVec2(x1, y0), ImVec2(origin.x + extent.x, y1), shade);
    draw->AddRect(ImVec2(x0, y0), ImVec2(x1, y1),
                  IM_COL32(255, 220, 116, 255), 0.0f, 0, 2.0f);
    draw->AddLine(ImVec2(x0 + (x1 - x0) / 3.0f, y0),
                  ImVec2(x0 + (x1 - x0) / 3.0f, y1),
                  IM_COL32(255, 255, 255, 110));
    draw->AddLine(ImVec2(x0 + (x1 - x0) * 2.0f / 3.0f, y0),
                  ImVec2(x0 + (x1 - x0) * 2.0f / 3.0f, y1),
                  IM_COL32(255, 255, 255, 110));
    draw->AddLine(ImVec2(x0, y0 + (y1 - y0) / 3.0f),
                  ImVec2(x1, y0 + (y1 - y0) / 3.0f),
                  IM_COL32(255, 255, 255, 110));
    draw->AddLine(ImVec2(x0, y0 + (y1 - y0) * 2.0f / 3.0f),
                  ImVec2(x1, y0 + (y1 - y0) * 2.0f / 3.0f),
                  IM_COL32(255, 255, 255, 110));
    ui::SpeakFocusedItem(
        "Portrait subject crop",
        "drag to move; mouse wheel changes crop size",
        "The numeric crop controls below provide the same operation for keyboard and controller users.");
    return changed;
}

bool drawPortraitSourceImport(const MdkrModernCharacterEntry *entry,
                              CharacterIdentityEdit &edit) {
    bool changed = false;
    const char *smokeSource = std::getenv("MDKR_APP_SMOKE_PORTRAIT_SOURCE");
    const char *smokeToken =
        std::getenv("MDKR_APP_SMOKE_PORTRAIT_SOURCE_TOKEN");
    if (entry != nullptr && smokeSource != nullptr && smokeSource[0] != '\0' &&
        smokeToken != nullptr &&
        std::strcmp(smokeToken, "mdkr64-portrait-source-v1") == 0 &&
        g_characterPortraitSourceSmokePackages.insert(entry->id).second) {
        if (loadPortraitImportSource(edit, smokeSource, false)) {
            const uint32_t maximum = std::min(
                edit.importImage.width, edit.importImage.height);
            edit.importRecipe.cropSize = std::max<uint32_t>(1u, maximum * 3u / 4u);
            edit.importRecipe.cropX =
                (edit.importImage.width - edit.importRecipe.cropSize) / 2u;
            edit.importRecipe.cropY =
                (edit.importImage.height - edit.importRecipe.cropSize) / 2u;
            edit.importRecipe.background =
                CharacterPortraitImport::Background::Sky;
            (void)refreshPortraitImportPreview(edit);
            std::fprintf(
                stderr,
                "[app-ui-test] character-portrait-source-action package=%s loaded=1 applied=0\n",
                entry->id);
        } else {
            std::fprintf(
                stderr,
                "[app-ui-test] character-portrait-source-action package=%s loaded=0 error=%s\n",
                entry->id, edit.importError.c_str());
        }
    }
    ImGui::SeparatorText("Start portrait artwork");
    ui::TextSubtleWrapped(
        "Start from an exact-renderer model capture, any local RGB/RGBA PNG, or the pixel canvas. Source decoding and conversion stay local; applying a framed source records the exact 40 × 40 result in the draft, so a moved external PNG cannot invalidate saved work.");
    ImGui::SetNextItemWidth(
        filedialog::isAvailable()
            ? std::max(120.0f, ImGui::GetContentRegionAvail().x -
                                  ui::kBtnSecondary().x - ui::kGapS)
            : -1.0f);
    ImGui::InputTextWithHint(
        "Portrait input PNG##character-portrait-import-path",
        "/path/to/source-or-capture.png", edit.importPath,
        sizeof(edit.importPath));
    ui::SpeakFocusedItem(
        "Portrait input PNG", nullptr,
        "Accepts a bounded, non-animated, non-interlaced RGB or RGBA PNG from sixteen through four thousand ninety-six pixels per side.");
    if (filedialog::isAvailable()) {
        ImGui::SameLine();
        if (ImGui::Button("Choose image...", ui::kBtnSecondary())) {
            std::string picked;
            if (filedialog::openPortraitImage(picked)) {
                std::snprintf(edit.importPath, sizeof(edit.importPath), "%s",
                              picked.c_str());
            }
        }
        ui::SpeakFocusedItem(
            "Choose portrait image", nullptr,
            "Opens the operating system picker for a local PNG source.");
    }
    const bool pathReady = edit.importPath[0] != '\0';
    if (!pathReady) ImGui::BeginDisabled();
    if (ImGui::Button("Load and frame image") && pathReady) {
        if (loadPortraitImportSource(edit, edit.importPath, false)) {
            setStatus(
                "Portrait source decoded; adjust the crop and conversion before applying it.",
                AppTheme::good());
        } else {
            setStatus(
                ("Portrait source was not loaded: " + edit.importError).c_str(),
                AppTheme::bad());
        }
    }
    if (!pathReady) ImGui::EndDisabled();
    ui::SpeakFocusedItem(
        "Load and frame image",
        pathReady ? nullptr : "Choose or enter a PNG path first.",
        "Validates and decodes the source without changing the draft canvas.");
    ImGui::SameLine();
    if (ImGui::Button("Capture model in exact renderer")) {
        persistCharacterWorkshopTab(CharacterWorkshopTab::Test, true);
        setStatus(
            "Choose a semantic pose and view, enable one-shot PNG capture, then use the saved capture for the portrait from the Test tray.",
            AppTheme::accent());
    }
    ui::SpeakFocusedItem(
        "Capture model in exact renderer", nullptr,
        "Opens Test. Capture a stabilized exact game-renderer frame, then choose Use for portrait in its report card.");

    if (!CharacterPortraitImport::validImage(edit.importImage)) {
        if (edit.portraitSourceRecord.kind !=
            CharacterPortraitImport::SourceKind::Canvas) {
            const auto &record = edit.portraitSourceRecord;
            ImGui::Text(
                "Retained %s source · %u×%u · PNG SHA-256 %.12s…",
                record.kind ==
                        CharacterPortraitImport::SourceKind::ExactRenderer
                    ? "exact-renderer" : "local image",
                record.width, record.height, record.sha256.c_str());
            ui::TextSubtleWrapped(
                "This draft retains the exact framed 40 × 40 source and conversion recipe. Reload the original path only to revise the high-resolution crop; moving or deleting that file does not invalidate the draft.");
        }
        if (!edit.importError.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, AppTheme::bad());
            ImGui::TextWrapped("%s", edit.importError.c_str());
            ImGui::PopStyleColor();
        }
        return changed;
    }
    ImGui::Text(
        "%s source · %u×%u · PNG SHA-256 %.12s…",
        edit.importFromExactRenderer ? "Exact-renderer capture" : "Local image",
        edit.importImage.width, edit.importImage.height,
        edit.importImage.sha256.c_str());
    ui::TextSubtleWrapped(
        "Drag the highlighted square to move it; use the mouse wheel over the image to resize it. Exact numeric controls remain authoritative and keyboard accessible.");
    const std::string framingBefore = portraitImportRecipePayload(
        edit.importRecipe);
    bool framingHistoryApplied = false;
    changed |= drawPortraitImportThumbnail(edit);

    const bool canUndoFraming =
        CharacterEditHistory::canUndo(edit.importHistory);
    if (ImGui::Button("Undo framing") && canUndoFraming) {
        std::string target;
        CharacterPortraitImport::Recipe restored;
        if (CharacterEditHistory::undoTarget(edit.importHistory, target) &&
            decodePortraitImportRecipe(target, edit.importImage, restored) &&
            CharacterEditHistory::commitUndo(
                edit.importHistory, framingBefore)) {
            edit.importRecipe = restored;
            (void)refreshPortraitImportPreview(edit);
            framingHistoryApplied = true;
            changed = true;
        }
    }
    ui::SpeakFocusedItem(
        "Undo portrait framing",
        CharacterEditHistory::canUndo(edit.importHistory)
            ? nullptr : "No earlier framing edit.",
        "Restores the preceding crop, sampling, matte, and background recipe without changing the draft canvas.");
    ImGui::SameLine();
    const bool canRedoFraming =
        CharacterEditHistory::canRedo(edit.importHistory);
    if (ImGui::Button("Redo framing") && canRedoFraming) {
        std::string target;
        CharacterPortraitImport::Recipe restored;
        if (CharacterEditHistory::redoTarget(edit.importHistory, target) &&
            decodePortraitImportRecipe(target, edit.importImage, restored) &&
            CharacterEditHistory::commitRedo(
                edit.importHistory, framingBefore)) {
            edit.importRecipe = restored;
            (void)refreshPortraitImportPreview(edit);
            framingHistoryApplied = true;
            changed = true;
        }
    }
    ui::SpeakFocusedItem(
        "Redo portrait framing",
        CharacterEditHistory::canRedo(edit.importHistory)
            ? nullptr : "No later framing edit.",
        "Reapplies the next crop, sampling, matte, and background recipe without changing the draft canvas.");
    if (!canUndoFraming && !canRedoFraming) {
        ImGui::SameLine();
        ImGui::TextDisabled("No framing edits yet");
    }

    int cropOrigin[2] = {
        static_cast<int>(edit.importRecipe.cropX),
        static_cast<int>(edit.importRecipe.cropY),
    };
    ImGui::SetNextItemWidth(std::min(320.0f, ImGui::GetContentRegionAvail().x));
    if (ImGui::InputInt2("Crop top-left X/Y", cropOrigin)) {
        edit.importRecipe.cropX = static_cast<uint32_t>(std::max(0, cropOrigin[0]));
        edit.importRecipe.cropY = static_cast<uint32_t>(std::max(0, cropOrigin[1]));
        clampPortraitImportCrop(edit);
        changed = true;
    }
    int cropSize = static_cast<int>(edit.importRecipe.cropSize);
    ImGui::SetNextItemWidth(std::min(320.0f, ImGui::GetContentRegionAvail().x));
    if (ImGui::SliderInt(
            "Square crop size", &cropSize, 1,
            static_cast<int>(std::min(
                edit.importImage.width, edit.importImage.height)))) {
        edit.importRecipe.cropSize = static_cast<uint32_t>(cropSize);
        clampPortraitImportCrop(edit);
        changed = true;
    }
    ui::SpeakFocusedItem(
        "Square crop size", (std::to_string(cropSize) + " source pixels").c_str(),
        "Changes the subject frame without stretching the source.");
    if (ImGui::Button("Centre maximum square")) {
        edit.importRecipe = CharacterPortraitImport::centredRecipe(
            edit.importImage);
        changed = true;
    }
    ui::SpeakFocusedItem(
        "Centre maximum square", nullptr,
        "Restores the largest centred square while preserving no hidden crop state.");

    int sampling = static_cast<int>(edit.importRecipe.sampling);
    ImGui::SetNextItemWidth(std::min(320.0f, ImGui::GetContentRegionAvail().x));
    if (ImGui::Combo("Source resampling", &sampling,
                     "Crisp nearest pixel\0Premultiplied area\0")) {
        edit.importRecipe.sampling =
            static_cast<CharacterPortraitImport::Sampling>(sampling);
        changed = true;
    }
    ui::SpeakFocusedItem(
        "Source resampling",
        edit.importRecipe.sampling == CharacterPortraitImport::Sampling::Crisp
            ? "Crisp nearest pixel" : "Premultiplied area",
        "Area resampling is recommended for high-resolution art and preserves transparent edge colour correctly.");
    int matte = static_cast<int>(edit.importRecipe.edgeMatteTolerance);
    ImGui::SetNextItemWidth(std::min(320.0f, ImGui::GetContentRegionAvail().x));
    if (ImGui::SliderInt("Edge-connected matte removal", &matte, 0, 96)) {
        edit.importRecipe.edgeMatteTolerance = static_cast<uint32_t>(matte);
        changed = true;
    }
    ui::SpeakFocusedItem(
        "Edge-connected matte removal",
        matte == 0 ? "Off" : ("Tolerance " + std::to_string(matte)).c_str(),
        "Removes only pixels connected to a source corner and similar to that corner colour. Review hair and outlines after using it.");
    int background = static_cast<int>(edit.importRecipe.background);
    ImGui::SetNextItemWidth(std::min(320.0f, ImGui::GetContentRegionAvail().x));
    if (ImGui::Combo("Background frame", &background,
                     "Transparent\0Sunset gradient\0Sky gradient\0Charcoal gradient\0")) {
        edit.importRecipe.background =
            static_cast<CharacterPortraitImport::Background>(background);
        changed = true;
    }
    static constexpr const char *backgroundNames[] = {
        "Transparent", "Sunset gradient", "Sky gradient", "Charcoal gradient",
    };
    ui::SpeakFocusedItem(
        "Background frame", backgroundNames[std::clamp(background, 0, 3)],
        "Project-owned background colours fill transparent or matte-removed pixels without altering opaque subject pixels.");
    if (changed && !framingHistoryApplied) edit.importPreviewDirty = true;
    if (edit.importPreviewDirty && !ImGui::IsAnyItemActive()) {
        (void)refreshPortraitImportPreview(edit);
    }

    if (edit.importPreviewValid) {
        drawPortraitStudioCanvas(edit.importPreview, "Framed source");
        ImGui::SameLine();
        ImGui::BeginGroup();
        if (ImGui::Button("Send to DKR-style lab")) {
            edit.styleSource = edit.importPreview;
            commitPortraitImportSource(edit);
            refreshPortraitStylePreview(edit);
            changed = true;
            setStatus(
                "Framed source copied into the reversible DKR-style recipe.",
                AppTheme::good());
        }
        ui::SpeakFocusedItem(
            "Send to DKR-style lab", nullptr,
            "Copies the exact framed forty by forty result into the reversible style source without changing the pixel canvas.");
        if (ImGui::Button("Apply styled source to pixel canvas")) {
            edit.styleSource = edit.importPreview;
            commitPortraitImportSource(edit);
            refreshPortraitStylePreview(edit);
            edit.canvas = edit.stylePreview;
            edit.canvasDirty = true;
            changed = true;
            setStatus(
                "Portrait source framed, styled, and copied to the exact game canvas; review it before Build.",
                AppTheme::good());
        }
        ui::SpeakFocusedItem(
            "Apply styled source to pixel canvas", nullptr,
            "Runs the current deterministic style recipe and copies the result into the exact game portrait. Undo Identity restores the previous source and canvas.");
        ImGui::EndGroup();
    } else if (!edit.importError.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, AppTheme::bad());
        ImGui::TextWrapped("%s", edit.importError.c_str());
        ImGui::PopStyleColor();
    }
    if (!framingHistoryApplied) {
        (void)CharacterEditHistory::observe(
            edit.importHistory, framingBefore,
            portraitImportRecipePayload(edit.importRecipe),
            ImGui::IsAnyItemActive());
    }
    if (std::getenv("MDKR_APP_UI_TRACE") != nullptr && entry != nullptr) {
        const std::string traceKey = std::string(entry->id) + "\n" +
            edit.importImage.sha256 + "\n" +
            std::to_string(edit.importRecipe.cropX) + "," +
            std::to_string(edit.importRecipe.cropY) + "," +
            std::to_string(edit.importRecipe.cropSize) + "," +
            std::to_string(edit.importRecipe.edgeMatteTolerance) + "," +
            std::to_string(static_cast<unsigned>(edit.importRecipe.sampling)) +
            "," + std::to_string(
                static_cast<unsigned>(edit.importRecipe.background));
        if (g_characterPortraitSourceTraceKeys.insert(traceKey).second) {
            std::fprintf(
                stderr,
                "[app-ui] character-portrait-source package=%s kind=%s dimensions=%ux%u crop=%u,%u,%u sampling=%u matte=%u background=%u digest=%.12s\n",
                entry->id,
                edit.importFromExactRenderer ? "exact-renderer" : "local-png",
                edit.importImage.width, edit.importImage.height,
                edit.importRecipe.cropX, edit.importRecipe.cropY,
                edit.importRecipe.cropSize,
                static_cast<unsigned>(edit.importRecipe.sampling),
                edit.importRecipe.edgeMatteTolerance,
                static_cast<unsigned>(edit.importRecipe.background),
                edit.importImage.sha256.c_str());
        }
    }
    return changed;
}

uint8_t portraitFloatByte(float value) {
    return static_cast<uint8_t>(std::lround(
        std::clamp(value, 0.0f, 1.0f) * 255.0f));
}

void drawPortraitQualityReport(
    const CharacterPortraitStudio::Analysis &analysis,
    uint32_t paletteTarget) {
    const auto status = [](const char *text, const ImVec4 &colour) {
        ImGui::PushStyleColor(ImGuiCol_Text, colour);
        ImGui::TextWrapped("%s", text);
        ImGui::PopStyleColor();
    };
    ImGui::SeparatorText("Automatic quality checks");
    if (analysis.empty) {
        status(
            "Missing — the styled result has no visible subject pixels.",
            AppTheme::bad());
        return;
    }
    ImGui::Text("Subject occupancy: %.0f%% of card · %.0f%% inside bounds",
                analysis.canvasOccupancy * 100.0f,
                analysis.boundsOccupancy * 100.0f);
    ImGui::Text("Visible colours: %u / %u target · luminance span: %u / 255",
                analysis.uniqueVisibleColors, paletteTarget,
                analysis.luminanceRange);
    ImGui::Text("Transparent holes: %u · semitransparent pixels: %u",
                analysis.enclosedTransparentPixels,
                analysis.semitransparentPixels);
    if (analysis.touchesEdge) {
        status(
            "Review — visible artwork touches the card edge; hair, ears, or outline may be clipped.",
            AppTheme::accent());
    }
    if (analysis.lowOccupancy) {
        status(
            "Review — the subject occupies less than 18% of the card. Increase zoom or adjust framing for HUD readability.",
            AppTheme::accent());
    }
    if (analysis.lowContrast) {
        status(
            "Review — the visible luminance range is narrow. Check the portrait against both light and dark HUD backgrounds.",
            AppTheme::accent());
    }
    if (!analysis.touchesEdge && !analysis.lowOccupancy &&
        !analysis.lowContrast && analysis.enclosedTransparentPixels == 0u) {
        status(
            "Ready — framing, silhouette continuity, palette, and tonal separation pass the deterministic checks.",
            AppTheme::good());
    }
    ui::TextSubtleWrapped(
        "Automatic checks cannot recognize a face or judge artistic likeness. Review the native-size result yourself before approving identity.");
}

bool drawPortraitStyleLab(const MdkrModernCharacterEntry *entry,
                          CharacterIdentityEdit &edit) {
    bool changed = false;
    ui::TextSubtleWrapped(
        "Reframe one local source and generate a deterministic game-size result. The source, recipe, and exact output stay in the named draft; no network service or generative model is used.");
    ImGui::SetNextItemWidth(std::min(280.0f, ImGui::GetContentRegionAvail().x));
    changed |= ImGui::SliderInt("Framing zoom (%)", &edit.styleRecipe.zoomPercent,
                                50, 250);
    const std::string zoomState =
        std::to_string(edit.styleRecipe.zoomPercent) + " percent";
    ui::SpeakFocusedItem(
        "Framing zoom", zoomState.c_str(),
        "Changes portrait framing only; it does not resize the in-game model.");
    int pan[2] = {edit.styleRecipe.panX, edit.styleRecipe.panY};
    ImGui::SetNextItemWidth(std::min(280.0f, ImGui::GetContentRegionAvail().x));
    if (ImGui::InputInt2("Framing pan (pixels)", pan)) {
        edit.styleRecipe.panX = std::clamp(pan[0], -40, 40);
        edit.styleRecipe.panY = std::clamp(pan[1], -40, 40);
        changed = true;
    }
    const std::string panState = "x " + std::to_string(edit.styleRecipe.panX) +
        ", y " + std::to_string(edit.styleRecipe.panY) + " pixels";
    ui::SpeakFocusedItem(
        "Framing pan", panState.c_str(),
        "Moves the portrait subject inside the fixed forty by forty card.");
    int sampling = static_cast<int>(edit.styleRecipe.sampling);
    ImGui::SetNextItemWidth(std::min(280.0f, ImGui::GetContentRegionAvail().x));
    if (ImGui::Combo("Resampling", &sampling,
                     "Crisp pixels\0Smooth premultiplied alpha\0")) {
        edit.styleRecipe.sampling =
            static_cast<CharacterPortraitStudio::Sampling>(sampling);
        changed = true;
    }
    ui::SpeakFocusedItem(
        "Resampling",
        edit.styleRecipe.sampling == CharacterPortraitStudio::Sampling::Crisp
            ? "Crisp pixels" : "Smooth premultiplied alpha",
        "Chooses deterministic pixel sampling for reframing transparency.");
    int palette = edit.styleRecipe.paletteColors == 16u ? 0 :
                  edit.styleRecipe.paletteColors == 32u ? 1 : 2;
    ImGui::SetNextItemWidth(std::min(280.0f, ImGui::GetContentRegionAvail().x));
    if (ImGui::Combo("Palette target", &palette,
                     "16 colours\0" "32 colours\0" "64 colours\0")) {
        static constexpr uint32_t choices[] = {16u, 32u, 64u};
        edit.styleRecipe.paletteColors = choices[std::clamp(palette, 0, 2)];
        changed = true;
    }
    const std::string paletteState =
        std::to_string(edit.styleRecipe.paletteColors) + " colours";
    ui::SpeakFocusedItem(
        "Palette target", paletteState.c_str(),
        "Limits the styled result to this deterministic maximum colour count.");
    ImGui::SetNextItemWidth(std::min(280.0f, ImGui::GetContentRegionAvail().x));
    changed |= ImGui::SliderFloat("Ordered dither strength",
                                  &edit.styleRecipe.ditherStrength,
                                  0.0f, 1.0f, "%.2f");
    char ditherState[32];
    std::snprintf(ditherState, sizeof(ditherState), "%.2f",
                  edit.styleRecipe.ditherStrength);
    ui::SpeakFocusedItem(
        "Ordered dither strength", ditherState,
        "Controls only the fixed local four by four ordered dither pattern.");
    ImGui::SetNextItemWidth(std::min(280.0f, ImGui::GetContentRegionAvail().x));
    changed |= ImGui::SliderInt("Silhouette outline (pixels)",
                                &edit.styleRecipe.outlinePixels, 0, 2);
    const std::string outlineState =
        std::to_string(edit.styleRecipe.outlinePixels) + " pixels";
    ui::SpeakFocusedItem(
        "Silhouette outline", outlineState.c_str(),
        "Adds a bounded dark outline outside visible portrait pixels.");
    ImGui::SetNextItemWidth(std::min(280.0f, ImGui::GetContentRegionAvail().x));
    changed |= ImGui::SliderInt("Transparent alpha cutoff",
                                &edit.styleRecipe.alphaThreshold, 0, 255);
    const std::string alphaCutoffState =
        std::to_string(edit.styleRecipe.alphaThreshold);
    ui::SpeakFocusedItem(
        "Transparent alpha cutoff",
        alphaCutoffState.c_str(),
        "Pixels below this alpha value become fully transparent before styling.");
    changed |= ImGui::Checkbox("Fill isolated one-pixel holes",
                               &edit.styleRecipe.fillPinholes);
    ui::SpeakFocusedItem(
        "Fill isolated one-pixel holes",
        edit.styleRecipe.fillPinholes ? "On" : "Off",
        "When on, fills only transparent pixels enclosed by four visible neighbours.");
    if (changed || !edit.stylePreviewValid) {
        refreshPortraitStylePreview(edit);
    }

    if (ImGui::Button("Use current canvas as style source")) {
        edit.styleSource = edit.canvas;
        edit.portraitSourceRecord =
            CharacterPortraitImport::SourceRecord{};
        refreshPortraitStylePreview(edit);
        changed = true;
    }
    ui::SpeakFocusedItem(
        "Use current canvas as style source", nullptr,
        "Replaces only the draft's reversible portrait source. The installed portrait is unchanged until Build.");
    ImGui::SameLine();
    if (ImGui::Button("Reset style recipe")) {
        edit.styleRecipe = CharacterPortraitStudio::Recipe{};
        refreshPortraitStylePreview(edit);
        changed = true;
    }
    ui::SpeakFocusedItem(
        "Reset style recipe", nullptr,
        "Restores the bounded framing and style defaults without changing the source canvas.");

    const bool sideBySide = ImGui::GetContentRegionAvail().x >= 380.0f;
    drawPortraitStudioCanvas(edit.styleSource, "Before");
    if (sideBySide) ImGui::SameLine();
    drawPortraitStudioCanvas(edit.stylePreview, "Styled result");
    const auto analysis = CharacterPortraitStudio::analyse(edit.stylePreview);
    if (std::getenv("MDKR_APP_UI_TRACE") != nullptr && entry != nullptr) {
        const std::string digest = characterDigestHex(entry->source_sha256);
        const std::string traceKey = std::string(entry->id) + "\n" + digest;
        if (g_characterPortraitStyleTraceKeys.insert(traceKey).second) {
            std::fprintf(
                stderr,
                "[app-ui] character-portrait-style package=%s source=%.12s palette=%u visible=%u colours=%u holes=%u\n",
                entry->id, digest.c_str(),
                edit.styleRecipe.paletteColors, analysis.visiblePixels,
                analysis.uniqueVisibleColors,
                analysis.enclosedTransparentPixels);
        }
    }
    drawPortraitQualityReport(analysis, edit.styleRecipe.paletteColors);
    const bool canApply = !analysis.empty && edit.stylePreview != edit.canvas;
    if (!canApply) ImGui::BeginDisabled();
    bool applied = false;
    if (ImGui::Button("Apply styled result to pixel canvas")) {
        edit.canvas = edit.stylePreview;
        edit.canvasDirty = true;
        applied = true;
    }
    if (!canApply) ImGui::EndDisabled();
    ui::SpeakFocusedItem(
        "Apply styled result to pixel canvas",
        canApply ? nullptr : "The styled result is empty or already applied.",
        "Copies the deterministic preview into the editable draft canvas. Undo Identity restores the previous canvas and recipe.");
    return changed || applied;
}

bool drawPortraitPixelEditor(const MdkrModernCharacterEntry *entry,
                             CharacterIdentityEdit &edit) {
    constexpr int size = MDKR_MODERN_PORTRAIT_SIZE;
    static const char *tools[] = {"Pencil", "Eraser", "Fill", "Eyedropper"};
    for (int tool = 0; tool < static_cast<int>(std::size(tools)); ++tool) {
        if (tool != 0) ImGui::SameLine();
        (void)ImGui::RadioButton(tools[tool], &edit.tool, tool);
        ui::SpeakFocusedItem(
            tools[tool], edit.tool == tool ? "selected" : "available",
            "Selects the tool used by the portrait canvas and numeric pixel action.");
    }
    ImGui::SetNextItemWidth(std::min(360.0f, ImGui::GetContentRegionAvail().x));
    (void)ImGui::ColorEdit4(
        "Paint colour", edit.paintRgba,
        ImGuiColorEditFlags_DisplayRGB | ImGuiColorEditFlags_InputRGB |
            ImGuiColorEditFlags_AlphaBar);
    char paintState[64];
    std::snprintf(
        paintState, sizeof(paintState), "red %u, green %u, blue %u, alpha %u",
        portraitFloatByte(edit.paintRgba[0]),
        portraitFloatByte(edit.paintRgba[1]),
        portraitFloatByte(edit.paintRgba[2]),
        portraitFloatByte(edit.paintRgba[3]));
    ui::SpeakFocusedItem(
        "Paint colour", paintState,
        "Sets the exact RGBA colour for pencil, fill, and palette replacement.");
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
            if (edit.showOnion) {
                const uint8_t *source = edit.styleSource.data() +
                    (y * size + x) * 4;
                if (source[3] != 0u) {
                    draw->AddRectFilled(
                        minimum, maximum,
                        IM_COL32(source[0], source[1], source[2],
                                 std::min<unsigned>(source[3], 88u)));
                }
            }
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
    const std::string canvasState =
        "selected pixel " + std::to_string(edit.selectedPixel[0]) + ", " +
        std::to_string(edit.selectedPixel[1]);
    ui::SpeakFocusedItem(
        "Portrait pixel canvas", canvasState.c_str(),
        "Pointer painting is optional. Keyboard and controller users can edit the numeric pixel coordinates below.");
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
    const std::string coordinateState =
        "x " + std::to_string(edit.selectedPixel[0]) + ", y " +
        std::to_string(edit.selectedPixel[1]);
    ui::SpeakFocusedItem(
        "Pixel coordinates", coordinateState.c_str(),
        "Chooses the exact pixel used by Apply tool and replace-source actions.");
    if (ImGui::Button("Apply tool to selected pixel")) {
        if (edit.tool == 2) {
            portraitFill(edit, edit.selectedPixel[0], edit.selectedPixel[1]);
        } else if (edit.tool == 3) {
            portraitPickPixel(edit, edit.selectedPixel[0], edit.selectedPixel[1]);
        } else {
            portraitSetPixel(edit, edit.selectedPixel[0], edit.selectedPixel[1],
                             edit.tool == 1);
        }
    }
    ui::SpeakFocusedItem(
        "Apply tool to selected pixel", tools[edit.tool],
        "Applies the selected portrait tool at the numeric coordinates and can be undone in Identity history.");
    if (ImGui::Button("Mirror horizontally")) {
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
    ui::SpeakFocusedItem(
        "Mirror horizontally", nullptr,
        "Mirrors only the exact draft portrait canvas and can be undone in Identity history.");
    ImGui::SameLine();
    (void)ImGui::Checkbox("Onion source", &edit.showOnion);
    ui::SpeakFocusedItem(
        "Onion source", edit.showOnion ? "On" : "Off",
        "Shows the reversible style source beneath transparent canvas pixels; it does not change saved artwork.");
    if (ImGui::TreeNodeEx("Selection and palette tools",
                          ImGuiTreeNodeFlags_DefaultOpen)) {
        ui::TextSubtleWrapped(
            "The numeric rectangle is the keyboard/controller-accessible selection path. Move clears its old pixels; Copy preserves them. Pixels clipped by the card edge are deliberately discarded.");
        ImGui::SetNextItemWidth(
            std::min(320.0f, ImGui::GetContentRegionAvail().x));
        (void)ImGui::InputInt4("Selection x, y, width, height",
                               edit.selection);
        edit.selection[0] = std::clamp(edit.selection[0], 0, size - 1);
        edit.selection[1] = std::clamp(edit.selection[1], 0, size - 1);
        edit.selection[2] = std::clamp(edit.selection[2], 1,
                                       size - edit.selection[0]);
        edit.selection[3] = std::clamp(edit.selection[3], 1,
                                       size - edit.selection[1]);
        const std::string selectionState =
            "x " + std::to_string(edit.selection[0]) + ", y " +
            std::to_string(edit.selection[1]) + ", width " +
            std::to_string(edit.selection[2]) + ", height " +
            std::to_string(edit.selection[3]);
        ui::SpeakFocusedItem(
            "Selection x, y, width, height", selectionState.c_str(),
            "Defines the bounded rectangular region used by Move and Copy selection.");
        ImGui::SetNextItemWidth(
            std::min(240.0f, ImGui::GetContentRegionAvail().x));
        (void)ImGui::InputInt2("Move by x, y", edit.selectionDelta);
        for (int &delta : edit.selectionDelta) {
            delta = std::clamp(delta, -size, size);
        }
        const std::string deltaState =
            "x " + std::to_string(edit.selectionDelta[0]) + ", y " +
            std::to_string(edit.selectionDelta[1]);
        ui::SpeakFocusedItem(
            "Move by x, y", deltaState.c_str(),
            "Sets the signed pixel displacement used by Move and Copy selection.");
        const CharacterPortraitStudio::Selection selection = {
            edit.selection[0], edit.selection[1],
            edit.selection[2], edit.selection[3],
        };
        if (ImGui::Button("Move selection")) {
            edit.canvasDirty |= CharacterPortraitStudio::moveSelection(
                edit.canvas, selection, edit.selectionDelta[0],
                edit.selectionDelta[1], false);
        }
        ui::SpeakFocusedItem(
            "Move selection", nullptr,
            "Moves the selected pixels, clears their old location, clips at the card edge, and can be undone.");
        ImGui::SameLine();
        if (ImGui::Button("Copy selection")) {
            edit.canvasDirty |= CharacterPortraitStudio::moveSelection(
                edit.canvas, selection, edit.selectionDelta[0],
                edit.selectionDelta[1], true);
        }
        ui::SpeakFocusedItem(
            "Copy selection", nullptr,
            "Copies the selected pixels by the entered displacement and can be undone.");
        if (ImGui::Button("Use selected pixel as replace source")) {
            const uint8_t *source = edit.canvas.data() +
                (edit.selectedPixel[1] * size + edit.selectedPixel[0]) * 4;
            for (int component = 0; component < 4; ++component) {
                edit.replaceFromRgba[component] =
                    static_cast<float>(source[component]) / 255.0f;
            }
        }
        ui::SpeakFocusedItem(
            "Use selected pixel as replace source", nullptr,
            "Copies the selected pixel RGBA into the palette replacement source without changing the canvas.");
        ImGui::SetNextItemWidth(
            std::min(320.0f, ImGui::GetContentRegionAvail().x));
        (void)ImGui::ColorEdit4(
            "Replace source colour", edit.replaceFromRgba,
            ImGuiColorEditFlags_DisplayRGB | ImGuiColorEditFlags_InputRGB |
                ImGuiColorEditFlags_AlphaBar);
        ui::SpeakFocusedItem(
            "Replace source colour", nullptr,
            "Sets the RGBA colour that palette replacement searches for.");
        ImGui::SetNextItemWidth(
            std::min(240.0f, ImGui::GetContentRegionAvail().x));
        (void)ImGui::SliderInt("Replace tolerance", &edit.replaceTolerance,
                               0, 64);
        const std::string toleranceState =
            std::to_string(edit.replaceTolerance);
        ui::SpeakFocusedItem(
            "Replace tolerance",
            toleranceState.c_str(),
            "Sets a bounded RGBA distance; zero replaces exact matches only.");
        if (ImGui::Button("Replace matching colours with paint colour")) {
            uint8_t from[4];
            uint8_t to[4];
            for (int component = 0; component < 4; ++component) {
                from[component] = portraitFloatByte(
                    edit.replaceFromRgba[component]);
                to[component] = portraitFloatByte(edit.paintRgba[component]);
            }
            edit.canvasDirty |= CharacterPortraitStudio::replaceColour(
                edit.canvas, from, to,
                static_cast<uint8_t>(edit.replaceTolerance)) != 0u;
        }
        ui::SpeakFocusedItem(
            "Replace matching colours with paint colour", nullptr,
            "Replaces every bounded match in the draft canvas and can be undone in Identity history.");
        ImGui::TreePop();
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
    const bool stagingDraft = g_characterActiveDrafts.find(entry->id) !=
        g_characterActiveDrafts.end();
    if (!canSaveCanvas || stagingDraft) ImGui::BeginDisabled();
    if (ImGui::Button("Save pixel canvas revision")) {
        saved = reviseCharacterIdentityRgba(
            entry->id, edit.canvas, edit.minimapRgb);
        setStatus(saved
                ? "Pixel portrait compiled and activated."
                : "Pixel portrait failed; the active character was not changed.",
            saved ? AppTheme::good() : AppTheme::bad());
    }
    ui::SpeakFocusedItem(
        "Save pixel canvas revision",
        canSaveCanvas && !stagingDraft
            ? "available" : stagingDraft
                ? "unavailable while a named draft is open"
                : "no canvas or minimap changes",
        "Compiles and atomically activates the exact canvas. Named drafts use Build so identity, profile, and rig publish together.");
    if (!canSaveCanvas || stagingDraft) ImGui::EndDisabled();
    if (stagingDraft) {
        ui::TextSubtleWrapped(
            "The exact canvas and minimap colour are staged in the named draft. Build it from Named drafts to avoid publishing a partial source revision.");
    }
    return saved;
}

CharacterIdentityEdit &loadCharacterIdentityEdit(
    const MdkrModernCharacterEntry *entry) {
    static const uint8_t donorColours[][3] = {
        {194, 72, 58}, {54, 120, 197}, {66, 166, 110}, {76, 153, 190},
        {232, 145, 49}, {143, 91, 53}, {224, 93, 52}, {220, 80, 151},
        {150, 99, 198}, {237, 186, 48},
    };
    CharacterIdentityEdit &edit = g_characterIdentityEdits[entry->id];
    if (!edit.loaded ||
        std::memcmp(edit.sourceSha256, entry->source_sha256,
                    sizeof(edit.sourceSha256)) != 0) {
        edit = CharacterIdentityEdit{};
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
        edit.styleSource = edit.canvas;
        refreshPortraitStylePreview(edit);
        edit.canvasDirty = false;
        edit.strokeActive = false;
        std::snprintf(edit.displayName, sizeof(edit.displayName), "%s",
                      entry->display_name);
        std::snprintf(edit.shortName, sizeof(edit.shortName), "%s",
                      entry->short_name);
        std::snprintf(edit.narrationName, sizeof(edit.narrationName), "%s",
                      entry->narration_name);
        std::snprintf(edit.sortLabel, sizeof(edit.sortLabel), "%s",
                      entry->sort_label);
        std::memcpy(edit.sourceSha256, entry->source_sha256,
                    sizeof(edit.sourceSha256));
        edit.loaded = true;
    }
    return edit;
}

bool drawCharacterPortraitStudio(const MdkrModernCharacterEntry *entry) {
    CharacterIdentityEdit &edit = loadCharacterIdentityEdit(entry);
    const auto pending = g_characterPendingPortraitSources.find(entry->id);
    if (pending != g_characterPendingPortraitSources.end()) {
        const CharacterPendingPortraitSource source = pending->second;
        g_characterPendingPortraitSources.erase(pending);
        if (loadPortraitImportSource(
                edit, source.path, source.exactRenderer)) {
            setStatus(
                "Exact-renderer capture loaded into Portrait Studio; frame the subject and review the styled result.",
                AppTheme::good());
        } else {
            setStatus(
                ("The selected capture could not enter Portrait Studio: " +
                 edit.importError).c_str(),
                AppTheme::bad());
        }
    }
    CharacterHistoryFrame history = beginCharacterHistory(
        entry, CharacterHistoryTool::Identity);
    const bool stagingDraft = g_characterActiveDrafts.find(entry->id) !=
        g_characterActiveDrafts.end();
    ui::TextSubtleWrapped(
        "Author the character's player-facing names, square portrait artwork, and readable minimap colour. Named drafts compile these identity fields with gameplay and rig choices as one reviewed source revision.");
    if (!stagingDraft) ImGui::BeginDisabled();
    ImGui::SetNextItemWidth(std::min(420.0f, ImGui::GetContentRegionAvail().x));
    (void)ImGui::InputTextWithHint(
        "Display name##character-display-name", "Dixie Kong",
        edit.displayName, sizeof(edit.displayName));
    ImGui::SetNextItemWidth(std::min(420.0f, ImGui::GetContentRegionAvail().x));
    (void)ImGui::InputTextWithHint(
        "Short name##character-short-name", "Dixie",
        edit.shortName, sizeof(edit.shortName));
    ImGui::SetNextItemWidth(std::min(420.0f, ImGui::GetContentRegionAvail().x));
    (void)ImGui::InputTextWithHint(
        "Narration name##character-narration-name", "Dixie Kong",
        edit.narrationName, sizeof(edit.narrationName));
    ImGui::SetNextItemWidth(std::min(420.0f, ImGui::GetContentRegionAvail().x));
    (void)ImGui::InputTextWithHint(
        "Sort label##character-sort-label", "Kong, Dixie",
        edit.sortLabel, sizeof(edit.sortLabel));
    if (!stagingDraft) ImGui::EndDisabled();
    ui::TextSubtleWrapped(
        "Display name is the full visible label; short name fits compact roster tiles; narration name is the accessible spoken label; sort label controls alphabetical roster order. Each must be non-empty printable UTF-8. The current game font safely substitutes unsupported glyphs until full text shaping is available.");
    if (!stagingDraft) {
        ui::TextSubtleWrapped(
            "Create or resume a named draft to edit names. This prevents a metadata-only shortcut from publishing a partial identity revision.");
    }
    (void)drawPortraitSourceImport(entry, edit);
    ImGui::SeparatorText("Quick-publish authored square PNG");
    ui::TextSubtleWrapped(
        "This compatibility shortcut compiles an already-finished square portrait directly. For model captures, non-square art, background cleanup, or pixel styling, use the framed source workflow above.");
    if (stagingDraft) ImGui::BeginDisabled();
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
    if (stagingDraft) ImGui::EndDisabled();
    ImGui::SetNextItemWidth(std::min(360.0f, ImGui::GetContentRegionAvail().x));
    (void)ImGui::ColorEdit3(
        "Minimap colour##character-minimap-colour", edit.minimapRgb,
        ImGuiColorEditFlags_DisplayRGB | ImGuiColorEditFlags_InputRGB |
            ImGuiColorEditFlags_PickerHueWheel);
    ui::TextSubtleWrapped(
        "PNG profile: 16–1024 px square, 8-bit RGB/RGBA, non-animated and non-interlaced. Transparency is preserved. The exact current in-game pixels and colour are shown in Overview above.");
    const bool canSave = edit.portraitPath[0] != '\0' && !stagingDraft;
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
    if (stagingDraft) {
        ui::TextSubtleWrapped(
            "A named draft builds from its exact 40 × 40 canvas. Use the pixel editor below while the draft is open; close the draft first if you want the PNG revision shortcut.");
    }
    if (saved) {
        return true;
    }
    if (ImGui::TreeNodeEx(
            "DKR-style lab##character-portrait-style",
            ImGuiTreeNodeFlags_DefaultOpen)) {
        (void)drawPortraitStyleLab(entry, edit);
        ImGui::TreePop();
    }
    if (ImGui::TreeNodeEx(
            "Pixel editor##character-portrait-pixels",
            ImGuiTreeNodeFlags_DefaultOpen)) {
        ui::TextSubtleWrapped(
            "Edit the exact 40 × 40 runtime canvas. Pencil, eraser, fill, eyedropper, alpha, mirror, rectangular move/copy, palette replacement, source onion, and bounded undo/redo are deterministic and stay local until you save a source revision.");
        const bool pixelSaved = drawPortraitPixelEditor(entry, edit);
        ImGui::TreePop();
        if (pixelSaved) {
            return true;
        }
    }
    finishCharacterHistory(entry, history);
    return saved;
}

template <typename Value>
void appendCharacterHistoryValue(std::string &payload, const Value &value) {
    static_assert(std::is_trivially_copyable<Value>::value,
                  "history values must be trivially copyable");
    payload.append(reinterpret_cast<const char *>(&value), sizeof(value));
}

template <typename Value>
bool readCharacterHistoryValue(const std::string &payload, size_t &offset,
                               Value &value) {
    static_assert(std::is_trivially_copyable<Value>::value,
                  "history values must be trivially copyable");
    if (offset > payload.size() || sizeof(value) > payload.size() - offset) {
        return false;
    }
    std::memcpy(&value, payload.data() + offset, sizeof(value));
    offset += sizeof(value);
    return true;
}

bool captureCharacterHistoryPayload(
    const MdkrModernCharacterEntry *entry, CharacterHistoryTool tool,
    std::string &payload) {
    if (entry == nullptr) return false;
    payload.clear();
    if (tool == CharacterHistoryTool::Identity) {
        const CharacterIdentityEdit &edit = loadCharacterIdentityEdit(entry);
        if (!CharacterPortraitImport::validSourceRecord(
                edit.portraitSourceRecord)) {
            return false;
        }
        payload = "mdkr-identity-history-v3\n";
        payload.append(edit.displayName, sizeof(edit.displayName));
        payload.append(edit.shortName, sizeof(edit.shortName));
        payload.append(edit.narrationName, sizeof(edit.narrationName));
        payload.append(edit.sortLabel, sizeof(edit.sortLabel));
        payload.append(edit.portraitPath, sizeof(edit.portraitPath));
        payload.append(reinterpret_cast<const char *>(edit.minimapRgb),
                       sizeof(edit.minimapRgb));
        payload.append(reinterpret_cast<const char *>(edit.canvas.data()),
                       edit.canvas.size());
        payload.append(
            reinterpret_cast<const char *>(edit.styleSource.data()),
            edit.styleSource.size());
        appendCharacterHistoryValue(payload, edit.styleRecipe.zoomPercent);
        appendCharacterHistoryValue(payload, edit.styleRecipe.panX);
        appendCharacterHistoryValue(payload, edit.styleRecipe.panY);
        appendCharacterHistoryValue(payload, edit.styleRecipe.paletteColors);
        appendCharacterHistoryValue(payload, edit.styleRecipe.ditherStrength);
        appendCharacterHistoryValue(payload, edit.styleRecipe.outlinePixels);
        appendCharacterHistoryValue(payload, edit.styleRecipe.alphaThreshold);
        const uint32_t sampling = static_cast<uint32_t>(
            edit.styleRecipe.sampling);
        const uint8_t fillPinholes = edit.styleRecipe.fillPinholes ? 1u : 0u;
        appendCharacterHistoryValue(payload, sampling);
        appendCharacterHistoryValue(payload, fillPinholes);
        const uint32_t sourceKind = static_cast<uint32_t>(
            edit.portraitSourceRecord.kind);
        appendCharacterHistoryValue(payload, sourceKind);
        appendCharacterHistoryValue(
            payload, edit.portraitSourceRecord.width);
        appendCharacterHistoryValue(
            payload, edit.portraitSourceRecord.height);
        appendCharacterHistoryValue(
            payload, edit.portraitSourceRecord.recipe.cropX);
        appendCharacterHistoryValue(
            payload, edit.portraitSourceRecord.recipe.cropY);
        appendCharacterHistoryValue(
            payload, edit.portraitSourceRecord.recipe.cropSize);
        appendCharacterHistoryValue(
            payload,
            edit.portraitSourceRecord.recipe.edgeMatteTolerance);
        const uint32_t sourceSampling = static_cast<uint32_t>(
            edit.portraitSourceRecord.recipe.sampling);
        const uint32_t sourceBackground = static_cast<uint32_t>(
            edit.portraitSourceRecord.recipe.background);
        appendCharacterHistoryValue(payload, sourceSampling);
        appendCharacterHistoryValue(payload, sourceBackground);
        if (edit.portraitSourceRecord.sha256.empty()) {
            payload.append(64u, '\0');
        } else {
            payload += edit.portraitSourceRecord.sha256;
        }
    } else if (tool == CharacterHistoryTool::Profile) {
        const CharacterProfileEdit &edit = loadCharacterProfileEdit(entry);
        payload = "mdkr-profile-history-v1\n";
        appendCharacterHistoryValue(payload, edit.donor);
        appendCharacterHistoryValue(payload, edit.vehicleMask);
    } else if (tool == CharacterHistoryTool::Rig) {
        const CharacterRigEdit &edit = loadCharacterRigEdit(entry);
        if (!edit.error.empty()) return false;
        payload = "mdkr-rig-history-v1\n";
        appendCharacterHistoryValue(payload, edit.mode);
        const uint8_t reviewed = edit.reviewed ? 1u : 0u;
        appendCharacterHistoryValue(payload, reviewed);
        for (const CharacterRigEdit::Role &role : edit.roles) {
            appendCharacterHistoryValue(payload, role.joint);
            const uint8_t inferred = role.inferred ? 1u : 0u;
            appendCharacterHistoryValue(payload, inferred);
            appendCharacterHistoryValue(payload, role.confidence);
            for (float value : role.rest) {
                appendCharacterHistoryValue(payload, value);
            }
            for (float value : role.bend) {
                appendCharacterHistoryValue(payload, value);
            }
        }
    } else if (tool == CharacterHistoryTool::Fit) {
        const CharacterTuningEdit &edit = loadCharacterTuning(0, entry->id);
        payload = "mdkr-fit-history-v1\n";
        appendCharacterHistoryValue(payload, edit.scale);
        for (float value : edit.offset) {
            appendCharacterHistoryValue(payload, value);
        }
        for (float value : edit.rotation) {
            appendCharacterHistoryValue(payload, value);
        }
        appendCharacterHistoryValue(payload, edit.animationSpeed);
        appendCharacterHistoryValue(payload, edit.lodBias);
        appendCharacterHistoryValue(payload, edit.vehicleMask);
        for (const CharacterTuningEdit::Context &context : edit.context) {
            appendCharacterHistoryValue(payload, context.scale);
            for (float value : context.offset) {
                appendCharacterHistoryValue(payload, value);
            }
            for (float value : context.rotation) {
                appendCharacterHistoryValue(payload, value);
            }
            for (const auto &contact : context.contacts) {
                for (float value : contact) {
                    appendCharacterHistoryValue(payload, value);
                }
            }
        }
        uint32_t reviewed = 0u;
        for (unsigned context = 0u;
             context < MDKR_CHARACTER_CONTEXT_COUNT; ++context) {
            if (characterFitReviewed(entry, edit, context)) {
                reviewed |= 1u << context;
            }
        }
        appendCharacterHistoryValue(payload, reviewed);
    } else if (tool == CharacterHistoryTool::Performance) {
        int players = g_characterAssemblyPlayers[entry->id];
        if (players < 1 || players > 4) players = 4;
        payload = "mdkr-performance-history-v1\n";
        appendCharacterHistoryValue(payload, players);
    } else if (tool == CharacterHistoryTool::Test) {
        int players = g_characterTestPlayers[entry->id];
        int pose = g_characterTestPoses[entry->id];
        int phase = g_characterTestPosePhases[entry->id];
        int yaw = g_characterTestViewYawDegrees[entry->id];
        int pitch = g_characterTestViewPitchDegrees[entry->id];
        int lighting = g_characterTestLighting[entry->id];
        if (players < 1 || players > 4) players = 1;
        if (pose <= MDKR_CHARACTER_PREVIEW_POSE_LIVE ||
            pose >= MDKR_CHARACTER_PREVIEW_POSE_COUNT) {
            pose = MDKR_CHARACTER_PREVIEW_POSE_RACE_STEER;
        }
        if (phase < 0 || phase > 1000) phase = 500;
        if (yaw < -180 || yaw > 180) yaw = 0;
        if (pitch < -45 || pitch > 45) pitch = 0;
        if (lighting < MDKR_WORKSHOP_PREVIEW_LIGHTING_NEUTRAL ||
            lighting >= MDKR_WORKSHOP_PREVIEW_LIGHTING_COUNT) {
            lighting = MDKR_WORKSHOP_PREVIEW_LIGHTING_NEUTRAL;
        }
        payload = "mdkr-test-history-v3\n";
        appendCharacterHistoryValue(payload, players);
        appendCharacterHistoryValue(payload, pose);
        appendCharacterHistoryValue(payload, phase);
        appendCharacterHistoryValue(payload, yaw);
        appendCharacterHistoryValue(payload, pitch);
        appendCharacterHistoryValue(payload, lighting);
    } else {
        return false;
    }
    return !payload.empty() &&
        payload.size() <= CharacterEditHistory::kMaximumSnapshotBytes;
}

bool characterHistoryBufferTerminated(const char *value, size_t size) {
    return size != 0u && value[size - 1u] == '\0' &&
        std::memchr(value, '\0', size) != nullptr;
}

bool applyCharacterHistoryPayload(
    const MdkrModernCharacterEntry *entry, CharacterHistoryTool tool,
    const std::string &payload, std::string &error) {
    if (entry == nullptr) {
        error = "No character is selected.";
        return false;
    }
    size_t offset = 0u;
    const auto consumeHeader = [&payload, &offset](const char *header) {
        const size_t size = std::strlen(header);
        if (payload.size() < size || payload.compare(0u, size, header) != 0) {
            return false;
        }
        offset = size;
        return true;
    };
    if (tool == CharacterHistoryTool::Identity) {
        const bool hasSourceRecord =
            consumeHeader("mdkr-identity-history-v3\n");
        if (!hasSourceRecord &&
            !consumeHeader("mdkr-identity-history-v2\n")) {
            error = "Identity history header is invalid.";
            return false;
        }
        CharacterIdentityEdit replacement = loadCharacterIdentityEdit(entry);
        auto readBuffer = [&payload, &offset](char *target, size_t size) {
            if (offset > payload.size() || size > payload.size() - offset) {
                return false;
            }
            std::memcpy(target, payload.data() + offset, size);
            offset += size;
            return characterHistoryBufferTerminated(target, size);
        };
        if (!readBuffer(replacement.displayName,
                        sizeof(replacement.displayName)) ||
            !readBuffer(replacement.shortName,
                        sizeof(replacement.shortName)) ||
            !readBuffer(replacement.narrationName,
                        sizeof(replacement.narrationName)) ||
            !readBuffer(replacement.sortLabel,
                        sizeof(replacement.sortLabel)) ||
            !readBuffer(replacement.portraitPath,
                        sizeof(replacement.portraitPath)) ||
            offset > payload.size() ||
            sizeof(replacement.minimapRgb) > payload.size() - offset) {
            error = "Identity history payload is truncated.";
            return false;
        }
        std::memcpy(replacement.minimapRgb, payload.data() + offset,
                    sizeof(replacement.minimapRgb));
        offset += sizeof(replacement.minimapRgb);
        if (replacement.canvas.size() > payload.size() - offset) {
            error = "Identity canvas history is truncated.";
            return false;
        }
        std::memcpy(replacement.canvas.data(), payload.data() + offset,
                    replacement.canvas.size());
        offset += replacement.canvas.size();
        if (replacement.styleSource.size() > payload.size() - offset) {
            error = "Identity style source history is truncated.";
            return false;
        }
        std::memcpy(replacement.styleSource.data(), payload.data() + offset,
                    replacement.styleSource.size());
        offset += replacement.styleSource.size();
        uint32_t sampling;
        uint8_t fillPinholes;
        if (!readCharacterHistoryValue(
                payload, offset, replacement.styleRecipe.zoomPercent) ||
            !readCharacterHistoryValue(
                payload, offset, replacement.styleRecipe.panX) ||
            !readCharacterHistoryValue(
                payload, offset, replacement.styleRecipe.panY) ||
            !readCharacterHistoryValue(
                payload, offset, replacement.styleRecipe.paletteColors) ||
            !readCharacterHistoryValue(
                payload, offset, replacement.styleRecipe.ditherStrength) ||
            !readCharacterHistoryValue(
                payload, offset, replacement.styleRecipe.outlinePixels) ||
            !readCharacterHistoryValue(
                payload, offset, replacement.styleRecipe.alphaThreshold) ||
            !readCharacterHistoryValue(payload, offset, sampling) ||
            sampling > 1u ||
            !readCharacterHistoryValue(payload, offset, fillPinholes) ||
            fillPinholes > 1u) {
            error = "Identity style recipe history is invalid.";
            return false;
        }
        replacement.styleRecipe.sampling =
            static_cast<CharacterPortraitStudio::Sampling>(sampling);
        replacement.styleRecipe.fillPinholes = fillPinholes != 0u;
        replacement.portraitSourceRecord =
            CharacterPortraitImport::SourceRecord{};
        if (hasSourceRecord) {
            uint32_t sourceKind;
            uint32_t sourceSampling;
            uint32_t sourceBackground;
            auto &record = replacement.portraitSourceRecord;
            if (!readCharacterHistoryValue(payload, offset, sourceKind) ||
                sourceKind > 2u ||
                !readCharacterHistoryValue(
                    payload, offset, record.width) ||
                !readCharacterHistoryValue(
                    payload, offset, record.height) ||
                !readCharacterHistoryValue(
                    payload, offset, record.recipe.cropX) ||
                !readCharacterHistoryValue(
                    payload, offset, record.recipe.cropY) ||
                !readCharacterHistoryValue(
                    payload, offset, record.recipe.cropSize) ||
                !readCharacterHistoryValue(
                    payload, offset,
                    record.recipe.edgeMatteTolerance) ||
                !readCharacterHistoryValue(
                    payload, offset, sourceSampling) ||
                sourceSampling > 1u ||
                !readCharacterHistoryValue(
                    payload, offset, sourceBackground) ||
                sourceBackground > 3u || offset > payload.size() ||
                payload.size() - offset < 64u) {
                error = "Identity portrait source history is invalid.";
                return false;
            }
            record.kind = static_cast<CharacterPortraitImport::SourceKind>(
                sourceKind);
            record.recipe.sampling =
                static_cast<CharacterPortraitImport::Sampling>(
                    sourceSampling);
            record.recipe.background =
                static_cast<CharacterPortraitImport::Background>(
                    sourceBackground);
            const bool emptyDigest = std::all_of(
                payload.begin() + static_cast<std::ptrdiff_t>(offset),
                payload.begin() + static_cast<std::ptrdiff_t>(offset + 64u),
                [](char byte) { return byte == '\0'; });
            if (!emptyDigest) {
                record.sha256.assign(payload.data() + offset, 64u);
            }
            offset += 64u;
        }
        if (offset != payload.size() ||
            !CharacterPortraitStudio::validRecipe(replacement.styleRecipe) ||
            !CharacterPortraitImport::validSourceRecord(
                replacement.portraitSourceRecord) ||
            !std::all_of(std::begin(replacement.minimapRgb),
                         std::end(replacement.minimapRgb), [](float value) {
                             return std::isfinite(value) &&
                                    value >= 0.0f && value <= 1.0f;
                         })) {
            error = "Identity history values are invalid.";
            return false;
        }
        replacement.canvasDirty =
            (entry->identity_flags & 1u) == 0u ||
            !std::equal(replacement.canvas.begin(), replacement.canvas.end(),
                        std::begin(entry->portrait_rgba));
        replacement.strokeActive = false;
        replacement.importImage = CharacterPortraitImport::Image{};
        replacement.importThumbnail = CharacterPortraitImport::Thumbnail{};
        replacement.importPreviewValid = false;
        replacement.importPreviewDirty = false;
        replacement.importFromExactRenderer = false;
        replacement.importError.clear();
        CharacterEditHistory::clear(replacement.importHistory);
        std::snprintf(replacement.importPath,
                      sizeof(replacement.importPath), "%s",
                      replacement.portraitPath);
        refreshPortraitStylePreview(replacement);
        g_characterIdentityEdits[entry->id] = std::move(replacement);
    } else if (tool == CharacterHistoryTool::Profile) {
        if (!consumeHeader("mdkr-profile-history-v1\n")) {
            error = "Profile history header is invalid.";
            return false;
        }
        CharacterProfileEdit replacement = loadCharacterProfileEdit(entry);
        if (!readCharacterHistoryValue(
                payload, offset, replacement.donor) ||
            !readCharacterHistoryValue(
                payload, offset, replacement.vehicleMask) ||
            offset != payload.size() || replacement.donor >= 10u ||
            replacement.vehicleMask == 0u ||
            (replacement.vehicleMask & ~7u) != 0u) {
            error = "Profile history values are invalid.";
            return false;
        }
        g_characterProfileEdits[entry->id] = std::move(replacement);
    } else if (tool == CharacterHistoryTool::Rig) {
        if (!consumeHeader("mdkr-rig-history-v1\n")) {
            error = "Rig history header is invalid.";
            return false;
        }
        CharacterRigEdit replacement = loadCharacterRigEdit(entry);
        uint8_t reviewed = 0u;
        if (!readCharacterHistoryValue(payload, offset, replacement.mode) ||
            !readCharacterHistoryValue(payload, offset, reviewed) ||
            replacement.mode < MDKR_MODERN_RIG_AUTHORED_CLIPS_ONLY ||
            replacement.mode > MDKR_MODERN_RIG_HUMANOID_RETARGET_V1 ||
            reviewed > 1u) {
            error = "Rig history mode is invalid.";
            return false;
        }
        replacement.reviewed = reviewed != 0u;
        std::set<int> mappedJoints;
        for (CharacterRigEdit::Role &role : replacement.roles) {
            uint8_t inferred = 0u;
            if (!readCharacterHistoryValue(payload, offset, role.joint) ||
                !readCharacterHistoryValue(payload, offset, inferred) ||
                !readCharacterHistoryValue(
                    payload, offset, role.confidence)) {
                error = "Rig history role is truncated.";
                return false;
            }
            for (float &value : role.rest) {
                if (!readCharacterHistoryValue(payload, offset, value)) {
                    error = "Rig history rest basis is truncated.";
                    return false;
                }
            }
            for (float &value : role.bend) {
                if (!readCharacterHistoryValue(payload, offset, value)) {
                    error = "Rig history bend basis is truncated.";
                    return false;
                }
            }
            if (inferred > 1u || role.joint < -1 ||
                role.joint >= static_cast<int>(replacement.joints.size()) ||
                (role.joint >= 0 && !mappedJoints.insert(role.joint).second) ||
                !std::isfinite(role.confidence) || role.confidence < 0.0f ||
                role.confidence > 1.0f ||
                !std::all_of(std::begin(role.rest), std::end(role.rest),
                             [](float value) {
                                 return std::isfinite(value) &&
                                        value >= -1.0f && value <= 1.0f;
                             }) ||
                !std::all_of(std::begin(role.bend), std::end(role.bend),
                             [](float value) {
                                 return std::isfinite(value) &&
                                        value >= -1.0f && value <= 1.0f;
                             })) {
                error = "Rig history role values are invalid.";
                return false;
            }
            role.inferred = inferred != 0u;
        }
        if (offset != payload.size() ||
            (replacement.mode == MDKR_MODERN_RIG_AUTHORED_CLIPS_ONLY &&
             replacement.reviewed)) {
            error = "Rig history has trailing or inconsistent state.";
            return false;
        }
        g_characterRigEdits[entry->id] = std::move(replacement);
    } else if (tool == CharacterHistoryTool::Fit) {
        if (!consumeHeader("mdkr-fit-history-v1\n")) {
            error = "Fit history header is invalid.";
            return false;
        }
        CharacterTuningEdit replacement{};
        replacement.loaded = true;
        const auto readFloatArray = [&payload, &offset](float *values,
                                                        size_t count) {
            for (size_t index = 0u; index < count; ++index) {
                if (!readCharacterHistoryValue(
                        payload, offset, values[index])) return false;
            }
            return true;
        };
        if (!readCharacterHistoryValue(payload, offset, replacement.scale) ||
            !readFloatArray(replacement.offset, 3u) ||
            !readFloatArray(replacement.rotation, 3u) ||
            !readCharacterHistoryValue(
                payload, offset, replacement.animationSpeed) ||
            !readCharacterHistoryValue(
                payload, offset, replacement.lodBias) ||
            !readCharacterHistoryValue(
                payload, offset, replacement.vehicleMask)) {
            error = "Fit history global state is truncated.";
            return false;
        }
        for (CharacterTuningEdit::Context &context : replacement.context) {
            if (!readCharacterHistoryValue(payload, offset, context.scale) ||
                !readFloatArray(context.offset, 3u) ||
                !readFloatArray(context.rotation, 3u)) {
                error = "Fit history context is truncated.";
                return false;
            }
            for (auto &contact : context.contacts) {
                if (!readFloatArray(contact, 3u)) {
                    error = "Fit history contacts are truncated.";
                    return false;
                }
            }
        }
        uint32_t reviewed = 0u;
        if (!readCharacterHistoryValue(payload, offset, reviewed) ||
            offset != payload.size()) {
            error = "Fit history review state is malformed.";
            return false;
        }
        const auto inRange = [](float value, float minimum, float maximum) {
            return std::isfinite(value) && value >= minimum &&
                   value <= maximum;
        };
        if (!inRange(replacement.scale, 0.1f, 5.0f) ||
            !std::all_of(std::begin(replacement.offset),
                         std::end(replacement.offset),
                         [&inRange](float value) {
                             return inRange(value, -500.0f, 500.0f);
                         }) ||
            !std::all_of(std::begin(replacement.rotation),
                         std::end(replacement.rotation),
                         [&inRange](float value) {
                             return inRange(value, -180.0f, 180.0f);
                         }) ||
            !inRange(replacement.animationSpeed, 0.05f, 4.0f) ||
            !inRange(replacement.lodBias, -3.0f, 3.0f) ||
            replacement.vehicleMask == 0u ||
            (replacement.vehicleMask & ~entry->vehicle_mask) != 0u ||
            (reviewed & ~0xFu) != 0u) {
            error = "Fit history global values are invalid.";
            return false;
        }
        for (const CharacterTuningEdit::Context &context :
             replacement.context) {
            if (!inRange(context.scale, 0.1f, 5.0f) ||
                !std::all_of(std::begin(context.offset),
                             std::end(context.offset),
                             [&inRange](float value) {
                                 return inRange(value, -10.0f, 10.0f);
                             }) ||
                !std::all_of(std::begin(context.rotation),
                             std::end(context.rotation),
                             [&inRange](float value) {
                                 return inRange(value, -180.0f, 180.0f);
                             })) {
                error = "Fit history context values are invalid.";
                return false;
            }
            for (const auto &contact : context.contacts) {
                if (!std::all_of(
                        std::begin(contact), std::end(contact),
                        [&inRange](float value) {
                            return inRange(value, -1.0f, 1.0f);
                        })) {
                    error = "Fit history contact values are invalid.";
                    return false;
                }
            }
        }
        const auto previousTuning = g_characterTuning.find(entry->id);
        const bool hadTuning = previousTuning != g_characterTuning.end();
        CharacterTuningEdit oldTuning;
        if (hadTuning) oldTuning = previousTuning->second;
        const auto previousReview = g_characterDraftReviews.find(entry->id);
        const bool hadReview = previousReview != g_characterDraftReviews.end();
        CharacterDraftReviewState oldReview;
        if (hadReview) oldReview = previousReview->second;
        g_characterTuning[entry->id] = replacement;
        CharacterDraftReviewState review;
        review.mask = reviewed;
        for (unsigned context = 0u;
             context < MDKR_CHARACTER_CONTEXT_COUNT; ++context) {
            if ((reviewed & (1u << context)) != 0u) {
                review.signature[context] = characterFitReviewSignature(
                    entry, replacement, context);
            }
        }
        const bool activeDraft = g_characterActiveDrafts.find(entry->id) !=
            g_characterActiveDrafts.end();
        std::array<std::string, MDKR_CHARACTER_CONTEXT_COUNT>
            oldPersistedReviews;
        bool persisted = false;
        if (activeDraft) {
            g_characterDraftReviews[entry->id] = review;
            persisted = autosaveActiveCharacterDraft(entry);
        } else {
            for (unsigned context = 0u;
                 context < MDKR_CHARACTER_CONTEXT_COUNT; ++context) {
                oldPersistedReviews[context] = AppConfig::get(
                    characterFitReviewKey(entry->id, context));
            }
            stageCharacterTuningConfig(entry->id, replacement);
            for (unsigned context = 0u;
                 context < MDKR_CHARACTER_CONTEXT_COUNT; ++context) {
                AppConfig::set(
                    characterFitReviewKey(entry->id, context),
                    (reviewed & (1u << context)) != 0u
                        ? review.signature[context] : "");
            }
            persisted = AppConfig::persistResultApplied(AppConfig::save());
        }
        if (!persisted) {
            if (hadTuning) {
                g_characterTuning[entry->id] = oldTuning;
            } else {
                g_characterTuning.erase(entry->id);
            }
            if (hadReview) {
                g_characterDraftReviews[entry->id] = std::move(oldReview);
            } else {
                g_characterDraftReviews.erase(entry->id);
            }
            if (!activeDraft && hadTuning) {
                stageCharacterTuningConfig(entry->id, oldTuning);
                for (unsigned context = 0u;
                     context < MDKR_CHARACTER_CONTEXT_COUNT; ++context) {
                    AppConfig::set(
                        characterFitReviewKey(entry->id, context),
                        oldPersistedReviews[context]);
                }
            }
            error = "Fit history could not be persisted; history was not consumed.";
            return false;
        }
    } else if (tool == CharacterHistoryTool::Performance) {
        int players = 0;
        if (!consumeHeader("mdkr-performance-history-v1\n") ||
            !readCharacterHistoryValue(payload, offset, players) ||
            offset != payload.size() || players < 1 || players > 4) {
            error = "Assembly history value is invalid.";
            return false;
        }
        g_characterAssemblyPlayers[entry->id] = players;
    } else if (tool == CharacterHistoryTool::Test) {
        const bool current = consumeHeader("mdkr-test-history-v3\n");
        const bool poseVersion = current ||
            consumeHeader("mdkr-test-history-v2\n");
        if (!poseVersion && !consumeHeader("mdkr-test-history-v1\n")) {
            error = "Test history header is invalid.";
            return false;
        }
        int players = 0;
        int pose = MDKR_CHARACTER_PREVIEW_POSE_RACE_STEER;
        int phase = 500;
        int yaw = 0;
        int pitch = 0;
        int lighting = MDKR_WORKSHOP_PREVIEW_LIGHTING_NEUTRAL;
        if (!readCharacterHistoryValue(payload, offset, players) ||
            (poseVersion &&
             (!readCharacterHistoryValue(payload, offset, pose) ||
              !readCharacterHistoryValue(payload, offset, phase))) ||
            (current &&
             (!readCharacterHistoryValue(payload, offset, yaw) ||
              !readCharacterHistoryValue(payload, offset, pitch) ||
              !readCharacterHistoryValue(payload, offset, lighting))) ||
            offset != payload.size() || players < 1 || players > 4 ||
            pose <= MDKR_CHARACTER_PREVIEW_POSE_LIVE ||
            pose >= MDKR_CHARACTER_PREVIEW_POSE_COUNT ||
            phase < 0 || phase > 1000 || yaw < -180 || yaw > 180 ||
            pitch < -45 || pitch > 45 ||
            lighting < MDKR_WORKSHOP_PREVIEW_LIGHTING_NEUTRAL ||
            lighting >= MDKR_WORKSHOP_PREVIEW_LIGHTING_COUNT) {
            error = "Test history values are invalid.";
            return false;
        }
        g_characterTestPlayers[entry->id] = players;
        g_characterTestPoses[entry->id] = pose;
        g_characterTestPosePhases[entry->id] = phase;
        g_characterTestViewYawDegrees[entry->id] = yaw;
        g_characterTestViewPitchDegrees[entry->id] = pitch;
        g_characterTestLighting[entry->id] = lighting;
    } else {
        error = "Unknown character history tool.";
        return false;
    }
    g_characterPreviewResults.erase(entry->id);
    error.clear();
    return true;
}

const char *characterHistoryToolName(CharacterHistoryTool tool) {
    switch (tool) {
        case CharacterHistoryTool::Identity: return "Identity";
        case CharacterHistoryTool::Profile: return "Profile";
        case CharacterHistoryTool::Rig: return "Rig";
        case CharacterHistoryTool::Fit: return "Fit";
        case CharacterHistoryTool::Performance: return "Performance";
        case CharacterHistoryTool::Test: return "Test setup";
        default: return "Editor";
    }
}

CharacterHistoryFrame beginCharacterHistory(
    const MdkrModernCharacterEntry *entry, CharacterHistoryTool tool) {
    CharacterHistoryFrame frame;
    frame.tool = tool;
    if (entry == nullptr ||
        !captureCharacterHistoryPayload(entry, tool, frame.before)) {
        return frame;
    }
    CharacterPackageHistory &package = g_characterEditHistories[entry->id];
    const std::string digest = characterDigestHex(entry->source_sha256);
    if (package.sourceDigest != digest) {
        package = CharacterPackageHistory{};
        package.sourceDigest = digest;
    }
    CharacterEditHistory::Track &track =
        package.tracks[static_cast<size_t>(tool)];
    if (std::getenv("MDKR_APP_UI_TRACE") != nullptr) {
        const std::string traceKey = std::string(entry->id) + "\n" +
            characterHistoryToolName(tool);
        if (g_characterHistoryTraceKeys.insert(traceKey).second) {
            std::fprintf(
                stderr,
                "[app-ui] character-history tool=%s source=%.12s undo=%zu redo=%zu\n",
                characterHistoryToolName(tool), digest.c_str(),
                track.undo.size(), track.redo.size());
        }
    }
    if (!ImGui::IsAnyItemActive()) CharacterEditHistory::endGesture(track);
    frame.ready = true;
    const std::string undoLabel = std::string("Undo ") +
        characterHistoryToolName(tool);
    const bool undoReady = CharacterEditHistory::canUndo(track);
    if (!undoReady) ImGui::BeginDisabled();
    if (ImGui::Button(undoLabel.c_str()) && undoReady) {
        std::string target;
        std::string error;
        if (CharacterEditHistory::undoTarget(track, target) &&
            applyCharacterHistoryPayload(entry, tool, target, error) &&
            CharacterEditHistory::commitUndo(track, frame.before)) {
            frame.actionApplied = true;
            const std::string status =
                undoLabel + " applied to this source-bound draft.";
            setStatus(status.c_str(), AppTheme::good());
        } else {
            const std::string status = error.empty()
                ? undoLabel + " could not be applied." : error;
            setStatus(status.c_str(), AppTheme::bad());
        }
    }
    if (!undoReady) ImGui::EndDisabled();
    ui::SpeakFocusedItem(
        undoLabel.c_str(), undoReady ? nullptr : "No earlier edit in this tool.",
        "Restores only this tool's prior source-bound authoring state; installed character bytes are unchanged.");
    ImGui::SameLine();
    const std::string redoLabel = std::string("Redo ") +
        characterHistoryToolName(tool);
    const bool redoReady = CharacterEditHistory::canRedo(track);
    if (!redoReady) ImGui::BeginDisabled();
    if (ImGui::Button(redoLabel.c_str()) && redoReady) {
        std::string target;
        std::string error;
        if (CharacterEditHistory::redoTarget(track, target) &&
            applyCharacterHistoryPayload(entry, tool, target, error) &&
            CharacterEditHistory::commitRedo(track, frame.before)) {
            frame.actionApplied = true;
            const std::string status =
                redoLabel + " applied to this source-bound draft.";
            setStatus(status.c_str(), AppTheme::good());
        } else {
            const std::string status = error.empty()
                ? redoLabel + " could not be applied." : error;
            setStatus(status.c_str(), AppTheme::bad());
        }
    }
    if (!redoReady) ImGui::EndDisabled();
    ui::SpeakFocusedItem(
        redoLabel.c_str(), redoReady ? nullptr : "No later edit in this tool.",
        "Reapplies only this tool's next source-bound authoring state; installed character bytes are unchanged.");
    return frame;
}

void finishCharacterHistory(const MdkrModernCharacterEntry *entry,
                            CharacterHistoryFrame &frame) {
    if (!frame.ready || frame.actionApplied || entry == nullptr) return;
    std::string after;
    if (!captureCharacterHistoryPayload(entry, frame.tool, after)) return;
    CharacterPackageHistory &package = g_characterEditHistories[entry->id];
    CharacterEditHistory::Track &track =
        package.tracks[static_cast<size_t>(frame.tool)];
    (void)CharacterEditHistory::observe(
        track, frame.before, after, ImGui::IsAnyItemActive());
}

bool captureCharacterDraftSnapshot(
    const MdkrModernCharacterEntry *entry,
    CharacterDraftSnapshot::Snapshot &snapshot, std::string &error) {
    if (entry == nullptr) {
        error = "No character is selected for the draft.";
        return false;
    }
    CharacterProfileEdit &profile = loadCharacterProfileEdit(entry);
    CharacterIdentityEdit &identity = loadCharacterIdentityEdit(entry);
    CharacterRigEdit &rig = loadCharacterRigEdit(entry);
    CharacterTuningEdit &tuning = loadCharacterTuning(0, entry->id);
    if (!rig.error.empty()) {
        error = "The active rig could not be captured: " + rig.error;
        return false;
    }
    snapshot = CharacterDraftSnapshot::Snapshot{};
    snapshot.donor = profile.donor;
    snapshot.packageVehicleMask = profile.vehicleMask;
    snapshot.enabledVehicleMask = tuning.vehicleMask & profile.vehicleMask;
    if (snapshot.enabledVehicleMask == 0u) {
        snapshot.enabledVehicleMask = profile.vehicleMask &
            (0u - profile.vehicleMask);
    }
    for (size_t component = 0u; component < 3u; ++component) {
        snapshot.minimapRgb[component] = static_cast<uint8_t>(std::lround(
            std::clamp(identity.minimapRgb[component], 0.0f, 1.0f) * 255.0f));
        snapshot.offset[component] = tuning.offset[component];
        snapshot.rotation[component] = tuning.rotation[component];
    }
    snapshot.portrait = identity.canvas;
    snapshot.portraitStyleSource = identity.styleSource;
    snapshot.portraitRecipe = identity.styleRecipe;
    snapshot.portraitSourceRecord = identity.portraitSourceRecord;
    snapshot.portraitSourcePath = identity.portraitPath;
    snapshot.displayName = identity.displayName;
    snapshot.shortName = identity.shortName;
    snapshot.narrationName = identity.narrationName;
    snapshot.sortLabel = identity.sortLabel;
    snapshot.scale = tuning.scale;
    snapshot.animationSpeed = tuning.animationSpeed;
    snapshot.lodBias = tuning.lodBias;
    snapshot.rigMode = static_cast<uint32_t>(rig.mode);
    snapshot.rigReviewed = rig.reviewed;
    for (size_t context = 0u;
         context < CharacterDraftSnapshot::kContexts; ++context) {
        snapshot.contexts[context].scale = tuning.context[context].scale;
        std::copy(std::begin(tuning.context[context].offset),
                  std::end(tuning.context[context].offset),
                  snapshot.contexts[context].offset);
        std::copy(std::begin(tuning.context[context].rotation),
                  std::end(tuning.context[context].rotation),
                  snapshot.contexts[context].rotation);
        std::memcpy(snapshot.contexts[context].contacts,
                    tuning.context[context].contacts,
                    sizeof(snapshot.contexts[context].contacts));
        if (characterFitReviewed(entry, tuning, context)) {
            snapshot.reviewedContexts |= 1u << context;
        }
    }
    for (size_t slot = 0u;
         slot < CharacterDraftSnapshot::kRoles; ++slot) {
        const CharacterRigEdit::Role &source = rig.roles[slot];
        CharacterDraftSnapshot::RigRole &target = snapshot.roles[slot];
        if (source.joint >= 0 &&
            source.joint < static_cast<int>(rig.joints.size())) {
            target.node = rig.joints[source.joint].node;
        }
        target.inferred = source.inferred;
        target.confidence = source.confidence;
        std::copy(std::begin(source.rest), std::end(source.rest), target.rest);
        std::copy(std::begin(source.bend), std::end(source.bend), target.bend);
    }
    int assemblyPlayers = g_characterAssemblyPlayers[entry->id];
    int testPlayers = g_characterTestPlayers[entry->id];
    int testPose = g_characterTestPoses.try_emplace(
        entry->id, MDKR_CHARACTER_PREVIEW_POSE_RACE_STEER).first->second;
    int testPosePhase = g_characterTestPosePhases.try_emplace(
        entry->id, 500).first->second;
    int testViewYaw = g_characterTestViewYawDegrees.try_emplace(
        entry->id, 0).first->second;
    int testViewPitch = g_characterTestViewPitchDegrees.try_emplace(
        entry->id, 0).first->second;
    int testLighting = g_characterTestLighting.try_emplace(
        entry->id, MDKR_WORKSHOP_PREVIEW_LIGHTING_NEUTRAL).first->second;
    snapshot.assemblyPlayers = assemblyPlayers >= 1 && assemblyPlayers <= 4
        ? assemblyPlayers : 4;
    snapshot.testPlayers = testPlayers >= 1 && testPlayers <= 4
        ? testPlayers : 1;
    snapshot.testPose =
        testPose > MDKR_CHARACTER_PREVIEW_POSE_LIVE &&
                testPose < MDKR_CHARACTER_PREVIEW_POSE_COUNT
            ? static_cast<uint32_t>(testPose)
            : static_cast<uint32_t>(MDKR_CHARACTER_PREVIEW_POSE_RACE_STEER);
    snapshot.testPosePhaseMilli =
        testPosePhase >= 0 && testPosePhase <= 1000
            ? static_cast<uint32_t>(testPosePhase) : 500u;
    snapshot.testViewYawDegrees =
        testViewYaw >= -180 && testViewYaw <= 180 ? testViewYaw : 0;
    snapshot.testViewPitchDegrees =
        testViewPitch >= -45 && testViewPitch <= 45 ? testViewPitch : 0;
    snapshot.testLighting =
        testLighting >= MDKR_WORKSHOP_PREVIEW_LIGHTING_NEUTRAL &&
                testLighting < MDKR_WORKSHOP_PREVIEW_LIGHTING_COUNT
            ? static_cast<uint32_t>(testLighting)
            : static_cast<uint32_t>(
                  MDKR_WORKSHOP_PREVIEW_LIGHTING_NEUTRAL);
    error.clear();
    return true;
}

bool applyCharacterDraftSnapshot(
    const MdkrModernCharacterEntry *entry,
    const CharacterDraftStore::Draft &draft, std::string &error) {
    if (entry == nullptr || draft.packageId != entry->id ||
        draft.baseSourceDigest != characterDigestHex(entry->source_sha256)) {
        error = "Draft base does not match the active character source.";
        return false;
    }
    CharacterDraftSnapshot::Snapshot snapshot;
    if (!CharacterDraftSnapshot::decode(draft.payload, snapshot, error)) {
        return false;
    }
    CharacterRigEdit rig = loadCharacterRigEdit(entry);
    if (!rig.error.empty()) {
        error = "The active rig could not be loaded: " + rig.error;
        return false;
    }
    for (size_t slot = 0u;
         slot < CharacterDraftSnapshot::kRoles; ++slot) {
        const CharacterDraftSnapshot::RigRole &source = snapshot.roles[slot];
        CharacterRigEdit::Role &target = rig.roles[slot];
        target = CharacterRigEdit::Role{};
        if (source.node != CharacterDraftSnapshot::kNoNode) {
            const auto joint = std::find_if(
                rig.joints.begin(), rig.joints.end(),
                [&source](const CharacterRigEdit::Joint &candidate) {
                    return candidate.node == source.node;
                });
            if (joint == rig.joints.end()) {
                error = "A draft rig node is absent from its exact base source.";
                return false;
            }
            target.joint = static_cast<int>(
                std::distance(rig.joints.begin(), joint));
        }
        target.inferred = source.inferred;
        target.confidence = source.confidence;
        std::copy(std::begin(source.rest), std::end(source.rest), target.rest);
        std::copy(std::begin(source.bend), std::end(source.bend), target.bend);
    }
    rig.mode = static_cast<int>(snapshot.rigMode);
    rig.reviewed = snapshot.rigReviewed;

    CharacterTuningEdit tuning{};
    tuning.loaded = true;
    tuning.scale = snapshot.scale;
    tuning.animationSpeed = snapshot.animationSpeed;
    tuning.lodBias = snapshot.lodBias;
    tuning.vehicleMask = snapshot.enabledVehicleMask;
    for (size_t component = 0u; component < 3u; ++component) {
        tuning.offset[component] = snapshot.offset[component];
        tuning.rotation[component] = snapshot.rotation[component];
    }
    for (size_t context = 0u;
         context < CharacterDraftSnapshot::kContexts; ++context) {
        tuning.context[context].scale = snapshot.contexts[context].scale;
        std::copy(std::begin(snapshot.contexts[context].offset),
                  std::end(snapshot.contexts[context].offset),
                  tuning.context[context].offset);
        std::copy(std::begin(snapshot.contexts[context].rotation),
                  std::end(snapshot.contexts[context].rotation),
                  tuning.context[context].rotation);
        std::memcpy(tuning.context[context].contacts,
                    snapshot.contexts[context].contacts,
                    sizeof(tuning.context[context].contacts));
    }

    CharacterProfileEdit profile{};
    profile.loaded = true;
    profile.donor = snapshot.donor;
    profile.vehicleMask = snapshot.packageVehicleMask;
    std::memcpy(profile.sourceSha256, entry->source_sha256,
                sizeof(profile.sourceSha256));
    CharacterIdentityEdit identity{};
    identity.loaded = true;
    identity.canvas = snapshot.portrait;
    identity.styleSource = snapshot.portraitStyleSource;
    identity.styleRecipe = snapshot.portraitRecipe;
    identity.portraitSourceRecord = snapshot.portraitSourceRecord;
    refreshPortraitStylePreview(identity);
    identity.minimapRgb[0] = snapshot.minimapRgb[0] / 255.0f;
    identity.minimapRgb[1] = snapshot.minimapRgb[1] / 255.0f;
    identity.minimapRgb[2] = snapshot.minimapRgb[2] / 255.0f;
    identity.canvasDirty =
        (entry->identity_flags & 1u) == 0u ||
        !std::equal(identity.canvas.begin(), identity.canvas.end(),
                    std::begin(entry->portrait_rgba));
    std::snprintf(identity.portraitPath, sizeof(identity.portraitPath), "%s",
                  snapshot.portraitSourcePath.c_str());
    std::snprintf(identity.importPath, sizeof(identity.importPath), "%s",
                  snapshot.portraitSourcePath.c_str());
    const char *displayName = snapshot.displayName.empty()
        ? entry->display_name : snapshot.displayName.c_str();
    const char *shortName = snapshot.shortName.empty()
        ? entry->short_name : snapshot.shortName.c_str();
    const char *narrationName = snapshot.narrationName.empty()
        ? entry->narration_name : snapshot.narrationName.c_str();
    const char *sortLabel = snapshot.sortLabel.empty()
        ? entry->sort_label : snapshot.sortLabel.c_str();
    std::snprintf(identity.displayName, sizeof(identity.displayName), "%s",
                  displayName);
    std::snprintf(identity.shortName, sizeof(identity.shortName), "%s",
                  shortName);
    std::snprintf(identity.narrationName, sizeof(identity.narrationName), "%s",
                  narrationName);
    std::snprintf(identity.sortLabel, sizeof(identity.sortLabel), "%s",
                  sortLabel);
    std::memcpy(identity.sourceSha256, entry->source_sha256,
                sizeof(identity.sourceSha256));

    g_characterTuning[entry->id] = std::move(tuning);
    g_characterProfileEdits[entry->id] = std::move(profile);
    g_characterIdentityEdits[entry->id] = std::move(identity);
    g_characterRigEdits[entry->id] = std::move(rig);
    g_characterAssemblyPlayers[entry->id] = snapshot.assemblyPlayers;
    g_characterTestPlayers[entry->id] = snapshot.testPlayers;
    g_characterTestPoses[entry->id] = static_cast<int>(snapshot.testPose);
    g_characterTestPosePhases[entry->id] =
        static_cast<int>(snapshot.testPosePhaseMilli);
    g_characterTestViewYawDegrees[entry->id] =
        snapshot.testViewYawDegrees;
    g_characterTestViewPitchDegrees[entry->id] =
        snapshot.testViewPitchDegrees;
    g_characterTestLighting[entry->id] =
        static_cast<int>(snapshot.testLighting);
    CharacterDraftReviewState review;
    review.mask = snapshot.reviewedContexts;
    for (size_t context = 0u;
         context < CharacterDraftSnapshot::kContexts; ++context) {
        review.signature[context] = characterFitReviewSignature(
            entry, g_characterTuning[entry->id], context);
    }
    g_characterDraftReviews[entry->id] = std::move(review);
    g_characterEditHistories.erase(entry->id);
    g_characterActiveDrafts[entry->id] = draft.id;
    g_characterDraftNameOwner = entry->id;
    std::snprintf(g_characterDraftName, sizeof(g_characterDraftName), "%s",
                  draft.name.c_str());
    g_characterPreviewResults.erase(entry->id);
    error.clear();
    return true;
}

std::string newCharacterDraftId(const MdkrModernCharacterEntry *entry,
                                const std::string &name) {
    static uint64_t serial;
    std::string seed = entry->id;
    seed.push_back('\0');
    seed += name;
    seed.push_back('\0');
    seed += std::to_string(static_cast<uint64_t>(std::time(nullptr)));
    seed.push_back('\0');
    seed += std::to_string(++serial);
    char digest[MDKR_SHA256_HEX_SIZE];
    mdkr_sha256_hex(seed.data(), seed.size(), digest);
    return std::string("draft-") + std::string(digest, 24u);
}

bool saveCharacterDraft(const MdkrModernCharacterEntry *entry,
                        bool saveAsNew) {
    loadCharacterDraftInventory();
    if (!g_characterDraftsWritable) return false;
    CharacterDraftSnapshot::Snapshot snapshot;
    std::string error;
    std::string payload;
    if (!captureCharacterDraftSnapshot(entry, snapshot, error) ||
        !CharacterDraftSnapshot::encode(snapshot, payload, error)) {
        g_characterDraftError = error;
        return false;
    }
    const std::string name = g_characterDraftName;
    CharacterDraftStore::Draft draft;
    const auto active = g_characterActiveDrafts.find(entry->id);
    if (!saveAsNew && active != g_characterActiveDrafts.end()) {
        draft.id = active->second;
    } else {
        for (size_t attempt = 0u;
             attempt <= CharacterDraftStore::kMaximumDrafts; ++attempt) {
            draft.id = newCharacterDraftId(entry, name);
            if (CharacterDraftStore::find(g_characterDrafts, draft.id) ==
                nullptr) break;
        }
        if (CharacterDraftStore::find(g_characterDrafts, draft.id) != nullptr) {
            g_characterDraftError =
                "Could not allocate a unique id for the new draft.";
            return false;
        }
    }
    draft.packageId = entry->id;
    draft.baseSourceDigest = characterDigestHex(entry->source_sha256);
    const std::time_t now = std::time(nullptr);
    draft.updatedUnix = now >= 0 ? static_cast<uint64_t>(now) : 0u;
    draft.name = name;
    draft.payload = std::move(payload);
    CharacterDraftStore::Inventory replacement = g_characterDrafts;
    if (!CharacterDraftStore::upsert(replacement, draft, error) ||
        !replaceCharacterDraftInventory(std::move(replacement))) {
        if (!error.empty()) g_characterDraftError = error;
        return false;
    }
    g_characterActiveDrafts[entry->id] = draft.id;
    CharacterDraftReviewState review;
    review.mask = snapshot.reviewedContexts;
    CharacterTuningEdit &tuning = loadCharacterTuning(0, entry->id);
    for (size_t context = 0u;
         context < CharacterDraftSnapshot::kContexts; ++context) {
        review.signature[context] = characterFitReviewSignature(
            entry, tuning, context);
    }
    g_characterDraftReviews[entry->id] = std::move(review);
    g_characterDraftNameOwner = entry->id;
    g_characterDraftError.clear();
    return true;
}

bool autosaveActiveCharacterDraft(const MdkrModernCharacterEntry *entry) {
    if (entry == nullptr) return false;
    loadCharacterDraftInventory();
    const auto active = g_characterActiveDrafts.find(entry->id);
    if (active == g_characterActiveDrafts.end()) return true;
    const CharacterDraftStore::Draft *saved =
        CharacterDraftStore::find(g_characterDrafts, active->second);
    if (!g_characterDraftsWritable || saved == nullptr ||
        saved->baseSourceDigest != characterDigestHex(entry->source_sha256)) {
        return false;
    }
    CharacterDraftSnapshot::Snapshot snapshot;
    std::string payload;
    std::string error;
    if (!captureCharacterDraftSnapshot(entry, snapshot, error) ||
        !CharacterDraftSnapshot::encode(snapshot, payload, error)) {
        g_characterDraftError = error;
        return false;
    }
    if (payload == saved->payload) return true;
    CharacterDraftStore::Draft replacementDraft = *saved;
    replacementDraft.payload = std::move(payload);
    const std::time_t now = std::time(nullptr);
    replacementDraft.updatedUnix = now >= 0
        ? static_cast<uint64_t>(now) : replacementDraft.updatedUnix;
    CharacterDraftStore::Inventory replacement = g_characterDrafts;
    if (!CharacterDraftStore::upsert(
            replacement, std::move(replacementDraft), error) ||
        !replaceCharacterDraftInventory(std::move(replacement))) {
        if (!error.empty()) g_characterDraftError = error;
        return false;
    }
    g_characterDraftError.clear();
    return true;
}

void closeCharacterDraftEditor(const std::string &packageId,
                               bool preserveFitEditor) {
    g_characterActiveDrafts.erase(packageId);
    g_characterDraftReviews.erase(packageId);
    g_characterProfileEdits.erase(packageId);
    g_characterIdentityEdits.erase(packageId);
    g_characterRigEdits.erase(packageId);
    g_characterEditHistories.erase(packageId);
    if (!preserveFitEditor) {
        g_characterTuning.erase(packageId);
        g_characterAssemblyPlayers.erase(packageId);
        g_characterTestPlayers.erase(packageId);
        g_characterTestPoses.erase(packageId);
        g_characterTestPosePhases.erase(packageId);
        g_characterTestViewYawDegrees.erase(packageId);
        g_characterTestViewPitchDegrees.erase(packageId);
        g_characterTestLighting.erase(packageId);
        g_characterCaptureEdits.erase(packageId);
        g_characterPendingDraftFit.erase(packageId);
    } else {
        g_characterPendingDraftFit[packageId] = true;
    }
    g_characterPreviewResults.erase(packageId);
    if (g_characterDraftNameOwner == packageId) {
        g_characterDraftNameOwner.clear();
        g_characterDraftName[0] = '\0';
    }
}

bool buildCharacterDraftSource(const MdkrModernCharacterEntry *entry) {
    static const char *donorIds[] = {
        "krunch", "bumper", "tiptup", "conker", "timber",
        "banjo", "drumstick", "pipsy", "tt", "diddy",
    };
    static const char *vehicleIds[] = {"car", "hovercraft", "plane"};
    if (entry == nullptr || g_characterRegistryDirectory.empty()) return false;
    CharacterProfileEdit &profile = loadCharacterProfileEdit(entry);
    CharacterIdentityEdit &identity = loadCharacterIdentityEdit(entry);
    CharacterRigEdit &rig = loadCharacterRigEdit(entry);
    if (profile.donor >= std::size(donorIds) || profile.vehicleMask == 0u ||
        (profile.vehicleMask & ~7u) != 0u || !rig.error.empty()) {
        g_characterManagerReport =
            "The staged profile or rig is not valid enough to build.";
        return false;
    }
    static const char hexDigits[] = "0123456789abcdef";
    std::string portraitHex(identity.canvas.size() * 2u, '0');
    for (size_t index = 0u; index < identity.canvas.size(); ++index) {
        portraitHex[index * 2u] = hexDigits[identity.canvas[index] >> 4u];
        portraitHex[index * 2u + 1u] =
            hexDigits[identity.canvas[index] & 0xFu];
    }
    std::string vehicles;
    for (size_t vehicle = 0u; vehicle < std::size(vehicleIds); ++vehicle) {
        if ((profile.vehicleMask & (1u << vehicle)) == 0u) continue;
        if (!vehicles.empty()) vehicles += ", ";
        vehicles += "\"" + std::string(vehicleIds[vehicle]) + "\"";
    }
    int minimap[3];
    for (size_t component = 0u; component < 3u; ++component) {
        minimap[component] = static_cast<int>(std::lround(
            std::clamp(identity.minimapRgb[component], 0.0f, 1.0f) *
            255.0f));
    }
    std::string json =
        "{\n  \"schema\": \"mdkr-workshop-build-v1\",\n"
        "  \"base_cache_source_digest\": \"" +
        characterDigestHex(entry->source_sha256) + "\",\n"
        "  \"display_name\": " +
        characterJsonString(identity.displayName) + ",\n"
        "  \"short_name\": " +
        characterJsonString(identity.shortName) + ",\n"
        "  \"narration_name\": " +
        characterJsonString(identity.narrationName) + ",\n"
        "  \"sort_label\": " +
        characterJsonString(identity.sortLabel) + ",\n"
        "  \"donor\": \"" + donorIds[profile.donor] + "\",\n"
        "  \"vehicles\": [" + vehicles + "],\n"
        "  \"portrait_rgba_hex\": \"" + portraitHex + "\",\n"
        "  \"minimap_rgb\": [" + std::to_string(minimap[0]) + ", " +
        std::to_string(minimap[1]) + ", " + std::to_string(minimap[2]) +
        "],\n  \"rig_draft\": " + characterRigDraftJson(rig) + "\n}\n";
    if (json.size() > 256u * 1024u) {
        g_characterManagerReport = "The bounded Workshop build draft is too large.";
        return false;
    }
    const std::string packageId = entry->id;
    const std::string draftPath = g_characterRegistryDirectory + "/." +
        packageId + ".launcher-workshop-build.json";
    (void)mdkr_remove_utf8(draftPath.c_str());
    std::FILE *file = mdkr_fopen_utf8(draftPath.c_str(), "wbx");
    if (file == nullptr) {
        g_characterManagerReport =
            "Could not create the bounded Workshop build draft.";
        return false;
    }
    const bool payloadWritten =
        std::fwrite(json.data(), 1u, json.size(), file) == json.size();
    const bool flushed = std::fflush(file) == 0;
    const bool closed = std::fclose(file) == 0;
    if (!payloadWritten || !flushed || !closed) {
        (void)mdkr_remove_utf8(draftPath.c_str());
        g_characterManagerReport =
            "Could not finish the bounded Workshop build draft.";
        return false;
    }
    const bool built = runCharacterManager(
        "build-draft", {packageId, draftPath});
    (void)mdkr_remove_utf8(draftPath.c_str());
    if (built) {
        /* The named snapshot remains retained against its exact old base. The
         * source editors must reload the new revision, while fit stays in its
         * editor so the user can explicitly apply that separate local state. */
        closeCharacterDraftEditor(packageId, true);
    }
    return built;
}

std::string characterRevisionTimestamp(uint64_t installedUnix);

bool drawCharacterDraftLifecycle(const MdkrModernCharacterEntry *entry) {
    loadCharacterDraftInventory();
    ui::TextSubtleWrapped(
        "Named drafts preserve portrait pixels, profile choices, rig mapping, fit/contact tuning, quality layout, and review acknowledgements without compiling or replacing the playable last-known-good character.");
    if (!g_characterDraftsWritable) {
        ImGui::TextColored(
            AppTheme::bad(), "Draft inventory unavailable: %s",
            g_characterDraftError.c_str());
        ui::TextSubtleWrapped(
            "The existing file is left untouched. Draft saving stays disabled so corrupt or unreadable work is never overwritten silently.");
        return false;
    }
    const std::string currentDigest = characterDigestHex(entry->source_sha256);
    if (g_characterDraftNameOwner != entry->id) {
        g_characterDraftNameOwner = entry->id;
        g_characterDraftName[0] = '\0';
        const auto selected = g_characterActiveDrafts.find(entry->id);
        const CharacterDraftStore::Draft *selectedDraft =
            selected != g_characterActiveDrafts.end()
            ? CharacterDraftStore::find(g_characterDrafts, selected->second)
            : nullptr;
        if (selectedDraft != nullptr) {
            std::snprintf(g_characterDraftName,
                          sizeof(g_characterDraftName), "%s",
                          selectedDraft->name.c_str());
        }
    }
    const auto active = g_characterActiveDrafts.find(entry->id);
    const CharacterDraftStore::Draft *activeDraft = active !=
            g_characterActiveDrafts.end()
        ? CharacterDraftStore::find(g_characterDrafts, active->second)
        : nullptr;
    if (activeDraft != nullptr) {
        ImGui::TextColored(AppTheme::good(), "Editing draft: %s",
                           activeDraft->name.c_str());
        ImGui::TextDisabled("Base source: %.12s… · saved %s",
                            activeDraft->baseSourceDigest.c_str(),
                            characterRevisionTimestamp(
                                activeDraft->updatedUnix).c_str());
    } else {
        ImGui::TextDisabled(
            "No draft resumed — editors currently reflect the active package and saved local fit.");
    }
    if (g_characterPendingDraftFit.find(entry->id) !=
        g_characterPendingDraftFit.end()) {
        ImGui::PushStyleColor(ImGuiCol_Text, AppTheme::accent());
        ImGui::TextWrapped(
            "The source revision built successfully. Its draft fit, contact, motion-speed, LOD, and vehicle-enable settings are still staged locally and have not changed play.");
        ImGui::PopStyleColor();
        if (ImGui::Button("Apply staged local fit")) {
            const auto staged = g_characterTuning.find(entry->id);
            if (staged != g_characterTuning.end() &&
                persistCharacterTuning(entry->id, staged->second)) {
                g_characterPendingDraftFit.erase(entry->id);
                setStatus(
                    "Draft source and local fit are now active. Exact-context reviews must be repeated for the new source digest.",
                    AppTheme::good());
            } else {
                setStatus(
                    "Local fit could not be applied; it remains staged and the built source stays recoverable.",
                    AppTheme::bad());
            }
        }
        ui::SpeakFocusedItem(
            "Apply staged local fit", nullptr,
            "Saves only package-local presentation settings. It does not change gameplay authority, physics, records, or player assignment.");
        ImGui::SameLine();
        if (ImGui::Button("Discard staged fit")) {
            g_characterPendingDraftFit.erase(entry->id);
            g_characterTuning.erase(entry->id);
            g_characterPreviewResults.erase(entry->id);
            setStatus(
                "Staged fit discarded from the editor; its values remain recoverable in the retained named draft.",
                AppTheme::subtle());
        }
        ui::SpeakFocusedItem(
            "Discard staged fit", nullptr,
            "Restores the active saved local fit in the editor. The named draft remains retained.");
    }
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint(
        "Draft name##character-draft-name", "e.g. Vehicle fit polish",
        g_characterDraftName, sizeof(g_characterDraftName));
    const bool named = g_characterDraftName[0] != '\0';
    if (!named) ImGui::BeginDisabled();
    if (ImGui::Button(activeDraft != nullptr ? "Save draft" : "Save new draft")) {
        if (saveCharacterDraft(entry, activeDraft == nullptr)) {
            setStatus(
                "Draft saved atomically; the playable character was not changed.",
                AppTheme::good());
            return false;
        } else {
            setStatus("Draft save failed; the prior saved draft is intact.",
                      AppTheme::bad());
        }
    }
    if (activeDraft != nullptr) {
        ImGui::SameLine();
        if (ImGui::Button("Save as new draft")) {
            if (saveCharacterDraft(entry, true)) {
                setStatus(
                    "New named draft saved; the original draft and playable character were retained.",
                    AppTheme::good());
                return false;
            } else {
                setStatus("New draft could not be saved.", AppTheme::bad());
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Close draft")) {
            if (!autosaveActiveCharacterDraft(entry)) {
                setStatus(
                    "Draft could not be closed because its latest state was not saved.",
                    AppTheme::bad());
                return false;
            }
            closeCharacterDraftEditor(entry->id, false);
            setStatus(
                "Draft closed after autosave; the editors now follow the active package and local fit settings.",
                AppTheme::good());
            return false;
        }
        ui::SpeakFocusedItem(
            "Close draft", nullptr,
            "Autosaves and closes only the editor session. The named draft and playable character are retained.");
    }
    if (!named) ImGui::EndDisabled();
    if (!g_characterDraftError.empty()) {
        ImGui::TextColored(AppTheme::bad(), "%s",
                           g_characterDraftError.c_str());
    }

    std::vector<const CharacterDraftStore::Draft *> packageDrafts;
    for (const CharacterDraftStore::Draft &draft : g_characterDrafts.drafts) {
        if (draft.packageId == entry->id) packageDrafts.push_back(&draft);
    }
    const bool canBuild = activeDraft != nullptr &&
        activeDraft->baseSourceDigest == currentDigest;
    if (!canBuild) ImGui::BeginDisabled();
    if (ImGui::Button("Build and activate draft")) {
        if (!autosaveActiveCharacterDraft(entry)) {
            setStatus(
                "Draft build stopped because the latest editor state could not be saved.",
                AppTheme::bad());
            if (!canBuild) ImGui::EndDisabled();
            return false;
        }
        if (buildCharacterDraftSource(entry)) {
            setStatus(
                "Draft identity, gameplay profile, and rig were compiled and activated as one retained source revision.",
                AppTheme::good());
            if (!canBuild) ImGui::EndDisabled();
            return true;
        }
        setStatus(
            "Draft build failed; the playable source and retained draft were not replaced.",
            AppTheme::bad());
        /* A compare-and-swap failure can mean another process changed the
         * installed base. Rescan before drawing any more controls that hold a
         * pointer into the old registry allocation. */
        refreshCharacterRegistry();
        if (!canBuild) ImGui::EndDisabled();
        return true;
    }
    if (!canBuild) ImGui::EndDisabled();
    ui::SpeakFocusedItem(
        "Build and activate draft",
        canBuild ? nullptr : "Resume a draft based on the current source first.",
        "Autosaves the editor snapshot, validates every staged source-owned field, and publishes one new retained source revision only after the complete compile succeeds. Local fit settings remain in the named draft until explicitly applied.");
    if (packageDrafts.empty()) return false;
    ImGui::TextUnformatted("Saved drafts");
    for (const CharacterDraftStore::Draft *draft : packageDrafts) {
        ImGui::PushID(draft->id.c_str());
        const bool currentBase = draft->baseSourceDigest == currentDigest;
        const bool resumed = activeDraft != nullptr &&
            activeDraft->id == draft->id;
        ImGui::TextWrapped("%s%s", draft->name.c_str(),
                           resumed ? " · resumed" : "");
        ImGui::SameLine();
        ImGui::TextDisabled("%s · %.12s…",
                            currentBase ? "current base" : "retained base",
                            draft->baseSourceDigest.c_str());
        if (!currentBase) ImGui::BeginDisabled();
        if (ImGui::Button("Resume") && currentBase) {
            std::string error;
            if (applyCharacterDraftSnapshot(entry, *draft, error)) {
                setStatus(
                    "Draft resumed against its exact source; the playable character remains unchanged.",
                    AppTheme::good());
            } else {
                g_characterDraftError = error;
                setStatus("Draft could not be resumed safely.", AppTheme::bad());
            }
        }
        if (!currentBase) ImGui::EndDisabled();
        ui::SpeakFocusedItem(
            "Resume draft",
            currentBase ? (resumed ? "currently resumed" : nullptr)
                        : "Restore this draft's retained source revision first.",
            "Loads editor state only; it does not build, activate, assign, or change the playable cache.");
        ImGui::SameLine();
        const std::string deleteLabel = "Delete draft...##" + draft->id;
        if (ImGui::Button(deleteLabel.c_str())) {
            g_characterPendingDraftRemoval = draft->id;
            ImGui::OpenPopup("Delete named draft?");
        }
        ui::SpeakFocusedItem(
            "Delete draft", nullptr,
            "Deletes only this named editor snapshot. Source revisions, installed assembly, settings, and player assignments remain.");
        if (g_characterPendingDraftRemoval == draft->id &&
            ImGui::BeginPopupModal(
                "Delete named draft?", nullptr,
                ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextWrapped(
                "Delete the named draft “%s”? This removes only its editor snapshot based on %.12s….",
                draft->name.c_str(), draft->baseSourceDigest.c_str());
            ui::TextSubtleWrapped(
                "The playable cache, retained source revisions, package settings, exact-context evidence, and player assignments are not changed.");
            if (ImGui::Button("Delete this draft")) {
                CharacterDraftStore::Inventory replacement =
                    g_characterDrafts;
                if (CharacterDraftStore::erase(replacement, draft->id) &&
                    replaceCharacterDraftInventory(std::move(replacement))) {
                    if (resumed) {
                        closeCharacterDraftEditor(entry->id, false);
                    }
                    g_characterPendingDraftRemoval.clear();
                    setStatus(
                        "Named draft deleted; source revisions and the playable assembly were retained.",
                        AppTheme::good());
                    ImGui::CloseCurrentPopup();
                    ImGui::EndPopup();
                    ImGui::PopID();
                    break;
                }
                setStatus(
                    "Draft deletion failed; its saved bytes remain intact.",
                    AppTheme::bad());
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) {
                g_characterPendingDraftRemoval.clear();
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        if (!currentBase) {
            ui::TextSubtleWrapped(
                "Resume is locked because the active source changed. Restore the matching retained source in Revision history, then resume; no automatic rebase can apply rig nodes to the wrong model.");
        }
        ImGui::PopID();
    }
    return false;
}

std::string characterRevisionTimestamp(uint64_t installedUnix) {
    if (installedUnix == 0u) return "time unavailable";
    const std::time_t timestamp = static_cast<std::time_t>(installedUnix);
    if (timestamp < 0 || static_cast<uint64_t>(timestamp) != installedUnix) {
        return "time unavailable";
    }
    std::tm local{};
#if defined(_WIN32)
    if (localtime_s(&local, &timestamp) != 0) return "time unavailable";
#else
    if (localtime_r(&timestamp, &local) == nullptr) return "time unavailable";
#endif
    char text[64];
    if (std::strftime(text, sizeof(text), "%Y-%m-%d %H:%M:%S", &local) == 0u) {
        return "time unavailable";
    }
    return text;
}

bool drawCharacterRevisionRecovery(const MdkrModernCharacterEntry *entry) {
    CharacterRevisionInventory &inventory =
        g_characterRevisionInventories[entry->id];
    if (!inventory.loaded) {
        ui::TextSubtleWrapped(
            "Load the authenticated source history to restore or export any revision retained by the Workshop. Loading does not change the installed character.");
        if (ImGui::Button("Load revision history")) {
            if (loadCharacterRevisionInventory(entry->id)) {
                setStatus("Authenticated revision history loaded.",
                          AppTheme::good());
            } else {
                setStatus("Revision history could not be loaded; open the manager report.",
                          AppTheme::bad());
            }
        }
        ui::SpeakFocusedItem(
            "Load revision history", nullptr,
            "Authenticates retained sources and lists them without changing the current package.");
        return false;
    }
    if (inventory.rows.empty()) return false;
    if (inventory.selected < 0 ||
        inventory.selected >= static_cast<int>(inventory.rows.size())) {
        inventory.selected = 0;
    }
    const CharacterRevisionRow &previewRow =
        inventory.rows[static_cast<size_t>(inventory.selected)];
    const std::string previewLabel =
        std::string(previewRow.current ? "Current · " : "Retained · ") +
        previewRow.sourceSha256.substr(0u, 12u) + "…";
    ImGui::TextDisabled(
        "%u authenticated revision(s)%s", inventory.total,
        inventory.total > inventory.rows.size()
            ? " · showing the current plus a bounded retained set" : "");
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::BeginCombo("Revision##character-revision", previewLabel.c_str())) {
        for (size_t index = 0u; index < inventory.rows.size(); ++index) {
            const CharacterRevisionRow &row = inventory.rows[index];
            const std::string label =
                std::string(row.current ? "Current · " : "Retained · ") +
                row.sourceSha256.substr(0u, 12u) + "…##revision-" +
                std::to_string(index);
            if (ImGui::Selectable(
                    label.c_str(), inventory.selected == static_cast<int>(index))) {
                inventory.selected = static_cast<int>(index);
            }
        }
        ImGui::EndCombo();
    }
    const CharacterRevisionRow &selected =
        inventory.rows[static_cast<size_t>(inventory.selected)];
    const std::string selectedLabel =
        std::string(selected.current ? "Current · " : "Retained · ") +
        selected.sourceSha256.substr(0u, 12u) + "…";
    ui::SpeakFocusedItem(
        "Character revision", selectedLabel.c_str(),
        "Choose any authenticated retained source for restore or export.");
    ImGui::TextDisabled("Full source SHA-256: %s",
                        selected.sourceSha256.c_str());
    ImGui::TextDisabled("Revision recorded: %s",
        characterRevisionTimestamp(selected.installedUnix).c_str());

    if (ImGui::Button("Rebuild current assembly...")) {
        ImGui::OpenPopup("Rebuild current character assembly?");
    }
    ui::SpeakFocusedItem(
        "Rebuild current assembly", nullptr,
        "Re-authenticates the active source and rebuilds its disposable runtime cache with the current compiler. The source revision and enabled state do not change.");
    if (ImGui::BeginPopupModal(
            "Rebuild current character assembly?", nullptr,
            ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped(
            "Rebuild %s from its authenticated current source? The replacement cache is validated before publication. The package remains %s and the last known-good cache stays active if rebuilding fails.",
            entry->display_name,
            entry->enabled != 0u ? "enabled" : "disabled");
        if (ImGui::Button("Rebuild assembly")) {
            const std::string id = entry->id;
            if (rebuildCharacterAssembly(id)) {
                setStatus(
                    "Current character assembly rebuilt transactionally.",
                    AppTheme::good());
                ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
                return true;
            }
            setStatus(
                "Assembly rebuild failed; the last known-good character is unchanged.",
                AppTheme::bad());
            ImGui::CloseCurrentPopup();
        }
        ui::SpeakFocusedItem(
            "Rebuild assembly", nullptr,
            "Compiles and validates the authenticated current source before atomically replacing its disposable runtime cache.");
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ui::SpeakFocusedItem("Cancel", nullptr,
                             "Closes without changing the character.");
        ImGui::EndPopup();
    }

    if (selected.current) ImGui::BeginDisabled();
    if (ImGui::Button("Restore selected revision...") && !selected.current) {
        ImGui::OpenPopup("Restore retained revision?");
    }
    if (selected.current) ImGui::EndDisabled();
    ui::SpeakFocusedItem(
        "Restore selected revision",
        selected.current ? "This is already the current revision." : nullptr,
        "Revalidates and compiles the selected source, preserving enabled or disabled state; the current source remains in history.");
    ImGui::SameLine();
    bool reloaded = false;
    if (ImGui::Button("Reload history")) {
        reloaded = loadCharacterRevisionInventory(entry->id);
        if (!reloaded) {
            setStatus("Revision history could not be reloaded.", AppTheme::bad());
        }
    }
    ui::SpeakFocusedItem(
        "Reload history", nullptr,
        "Re-authenticates every retained source and refreshes this list.");
    if (reloaded) return false;

    if (ImGui::BeginPopupModal("Restore retained revision?", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped(
            "Restore source revision %s…? It will be fully revalidated and compiled before becoming current. The present source stays retained, and the package remains %s.",
            selected.sourceSha256.substr(0u, 12u).c_str(),
            entry->enabled != 0u ? "enabled" : "disabled");
        if (ImGui::Button("Restore revision")) {
            const std::string id = entry->id;
            const std::string digest = selected.sourceSha256;
            if (restoreCharacterRevision(id, digest)) {
                setStatus("Retained character revision restored transactionally.",
                          AppTheme::good());
                ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
                return true;
            }
            setStatus("Revision restore failed; the current character was unchanged.",
                      AppTheme::bad());
            ImGui::CloseCurrentPopup();
        }
        ui::SpeakFocusedItem(
            "Restore revision", nullptr,
            "Revalidates and atomically activates this exact retained source without deleting the current source.");
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ui::SpeakFocusedItem("Cancel", nullptr,
                             "Closes without changing the character.");
        ImGui::EndPopup();
    }

    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint(
        "##character-revision-export", "/path/to/recovered-character.mdkrchar",
        inventory.exportPath, sizeof(inventory.exportPath));
    const bool canExport = inventory.exportPath[0] != '\0';
    if (!canExport) ImGui::BeginDisabled();
    if (ImGui::Button("Export selected source") && canExport) {
        if (exportCharacterRevision(
                entry->id, selected.sourceSha256, inventory.exportPath)) {
            setStatus("Retained source exported without overwriting another file.",
                      AppTheme::good());
        } else {
            setStatus("Source export failed; open the manager report.",
                      AppTheme::bad());
        }
    }
    if (!canExport) ImGui::EndDisabled();
    ui::SpeakFocusedItem(
        "Export selected source",
        canExport ? nullptr : "Enter a destination path first.",
        "Writes the exact authenticated mdkrchar source and refuses to overwrite an existing file.");

    ui::Gap(ui::kGapS);
    ui::TextSubtleWrapped(
        "Portable export embeds a cache built by this compiler so another player can import without installing authoring tools. It does not alter the installed character or the retained source.");
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint(
        "##character-revision-portable-export",
        "/path/to/shareable-character.mdkrchar",
        inventory.portableExportPath,
        sizeof(inventory.portableExportPath));
    const bool canExportPortable = inventory.portableExportPath[0] != '\0';
    if (!canExportPortable) ImGui::BeginDisabled();
    if (ImGui::Button("Export portable package") && canExportPortable) {
        if (exportPortableCharacterRevision(
                entry->id, selected.sourceSha256,
                inventory.portableExportPath)) {
            setStatus(
                "Portable package compiled and exported without overwriting another file.",
                AppTheme::good());
        } else {
            setStatus(
                "Portable package export failed; open the manager report.",
                AppTheme::bad());
        }
    }
    if (!canExportPortable) ImGui::EndDisabled();
    ui::SpeakFocusedItem(
        "Export portable package",
        canExportPortable ? nullptr : "Enter a destination path first.",
        "Compiles this exact authenticated retained source into a shareable mdkrchar package and refuses to overwrite an existing file.");
    return false;
}

void persistCharacterWorkshopTab(CharacterWorkshopTab tab,
                                 bool                 forceSelection) {
    g_characterWorkshopTab               = tab;
    g_characterWorkshopTabLoaded         = true;
    g_characterWorkshopTabForceSelection = forceSelection;
    (void)AppConfig::setAndSave(
        "character_workshop_last_tab",
        CharacterWorkshop_tabStorageId(tab));
}

void loadCharacterWorkshopTab() {
    if (g_characterWorkshopTabLoaded) return;
    const std::string stored             = AppConfig::get("character_workshop_last_tab");
    g_characterWorkshopTab               = CharacterWorkshop_parseTab(stored.c_str());
    g_characterWorkshopTabLoaded         = true;
    g_characterWorkshopTabForceSelection = true;
}

void drawCharacterWorkshopTabs() {
    loadCharacterWorkshopTab();
    CharacterWorkshopTab visible = g_characterWorkshopTab;
    if (ImGui::BeginTabBar(
            "##character-workshop-tabs",
            ImGuiTabBarFlags_FittingPolicyScroll)) {
        for (size_t index = 0u;
             index < static_cast<size_t>(CharacterWorkshopTab::Count);
             ++index) {
            const CharacterWorkshopTab tab =
                static_cast<CharacterWorkshopTab>(index);
            const ImGuiTabItemFlags flags =
                g_characterWorkshopTabForceSelection &&
                        tab == g_characterWorkshopTab
                    ? ImGuiTabItemFlags_SetSelected
                    : ImGuiTabItemFlags_None;
            if (ImGui::BeginTabItem(
                    CharacterWorkshop_tabLabel(tab),
                    nullptr,
                    flags)) {
                visible = tab;
                ui::SpeakFocusedItem(
                    CharacterWorkshop_tabLabel(tab),
                    "selected workspace",
                    "Switches tools without discarding or publishing the current draft.");
                ImGui::EndTabItem();
            }
        }
        ImGui::EndTabBar();
    }
    g_characterWorkshopTabForceSelection = false;
    if (visible != g_characterWorkshopTab) {
        persistCharacterWorkshopTab(visible, false);
    }
}

CharacterWorkshopReadiness characterWorkshopReadiness(
    const MdkrModernCharacterEntry *entry,
    bool                            normalized,
    bool                            anchored,
    bool                            attachmentSocketsMapped,
    bool                            motionReady,
    bool                            qualified,
    bool                            identityReady,
    bool                            humanoidRig,
    bool                            rigReviewed) {
    CharacterWorkshopFacts facts;
    facts.geometryAvailable      = entry != nullptr &&
                                   entry->stats.vertices != 0u && entry->stats.triangles != 0u;
    facts.identityReady          = identityReady;
    facts.normalized             = normalized;
    facts.anchorsReady           = anchored;
    facts.attachmentSocketsReady = attachmentSocketsMapped;
    facts.rigPresent             = humanoidRig;
    facts.rigReviewed            = !humanoidRig || rigReviewed;
    facts.motionReady            = motionReady;
    facts.donorQualified         = qualified;
    facts.performanceMeasured    = entry != nullptr &&
                                   entry->stats.lod_levels != 0u &&
                                   (entry->stats.textures == 0u ||
                                    entry->stats.decoded_texture_bytes != 0u);
    facts.enabled                = entry != nullptr && entry->enabled != 0u;
    if (entry != nullptr) {
        const CharacterTuningEdit &tuning =
            loadCharacterTuning(0, entry->id);
        facts.supportedVehicleMask = entry->vehicle_mask & tuning.vehicleMask;
        for (unsigned context = 0u;
             context < MDKR_CHARACTER_CONTEXT_COUNT;
             ++context) {
            if (context != MDKR_CHARACTER_CONTEXT_SELECT &&
                (facts.supportedVehicleMask & (1u << (context - 1u))) == 0u) {
                continue;
            }
            if (characterFitReviewed(entry, tuning, context)) {
                facts.reviewedContextMask |= 1u << context;
            }
        }
    }
    return CharacterWorkshop_evaluate(facts);
}

CharacterWorkshopReadiness characterWorkshopReadinessForEntry(
    const MdkrModernCharacterEntry *entry) {
    if (entry == nullptr) return CharacterWorkshop_evaluate({});
    constexpr uint32_t raceStates =
        (MDKR_CHARACTER_SEMANTIC_SELECT_IDLE - 1u) &
        ~MDKR_CHARACTER_SEMANTIC_FALLBACK;
    constexpr uint32_t selectStates =
        MDKR_CHARACTER_SEMANTIC_SELECT_IDLE |
        MDKR_CHARACTER_SEMANTIC_SELECT_HOVER |
        MDKR_CHARACTER_SEMANTIC_SELECT_CONFIRM;
    const uint32_t requiredContexts =
        (1u << MDKR_CHARACTER_CONTEXT_SELECT) |
        ((entry->vehicle_mask & 7u) << 1u);
    const bool normalized = (entry->calibration_flags & 1u) != 0u;
    const bool anchored =
        (entry->attachment_context_mask & requiredContexts) == requiredContexts;
    const bool sockets =
        (entry->socket_mask &
         (MDKR_CHARACTER_SOCKET_SEAT | MDKR_CHARACTER_SOCKET_HEAD)) ==
        (MDKR_CHARACTER_SOCKET_SEAT | MDKR_CHARACTER_SOCKET_HEAD);
    const bool humanoid      = entry->rig_present != 0u &&
                               entry->rig_mode == MDKR_MODERN_RIG_HUMANOID_RETARGET_V1;
    const bool rolesComplete = humanoid &&
                               entry->rig_role_mask == MDKR_CHARACTER_RIG_HUMANOID_MASK;
    const bool rigReviewed =
        (entry->rig_flags & MDKR_MODERN_RIG_REVIEWED) != 0u;
    const uint32_t requiredMotion = raceStates | selectStates;
    const uint32_t solverCovered  = rolesComplete && rigReviewed
                                        ? requiredMotion & ~entry->semantic_mask
                                        : 0u;
    const bool     motionReady =
        (((entry->moving_semantic_mask & requiredMotion) | solverCovered) &
         requiredMotion) == requiredMotion;
    const bool qualified     = mdkr_modern_donor_qualified(
                                   static_cast<int>(entry->donor)) != 0;
    const bool identityReady = (entry->identity_flags & 1u) != 0u &&
                               entry->portrait_bytes != 0u;
    return characterWorkshopReadiness(
        entry,
        normalized,
        anchored,
        sockets,
        motionReady,
        qualified,
        identityReady,
        humanoid,
        rigReviewed);
}

ImVec4 characterWorkshopStatusColour(
    CharacterWorkshopReadinessStatus status) {
    switch (status) {
        case CharacterWorkshopReadinessStatus::Ready:
            return AppTheme::good();
        case CharacterWorkshopReadinessStatus::Review:
            return AppTheme::accent();
        case CharacterWorkshopReadinessStatus::Missing:
        case CharacterWorkshopReadinessStatus::Unavailable:
            return AppTheme::bad();
    }
    return AppTheme::subtle();
}

void drawCharacterReadiness(
    const MdkrModernCharacterEntry   *entry,
    const CharacterWorkshopReadiness &readiness,
    bool                              normalized,
    bool                              anchored,
    bool                              attachmentSocketsMapped,
    bool                              motionReady,
    bool                              qualified,
    bool                              identityReady,
    const char                       *rigStatus) {
    ImGui::SeparatorText("Readiness");
    if (ImGui::BeginTable(
            "##character-workshop-readiness",
            3,
            ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg |
                ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Area", ImGuiTableColumnFlags_WidthStretch, 1.1f);
        ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthFixed, 100.0f * AppTheme::uiScale());
        ImGui::TableSetupColumn("Evidence", ImGuiTableColumnFlags_WidthStretch, 2.0f);
        ImGui::TableHeadersRow();
        for (const CharacterWorkshopReadinessRow &row : readiness.rows) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(CharacterWorkshop_readinessLabel(row.id));
            ImGui::TableNextColumn();
            ImGui::TextColored(
                characterWorkshopStatusColour(row.status),
                "%s",
                CharacterWorkshop_statusLabel(row.status));
            ImGui::TableNextColumn();
            switch (row.id) {
                case CharacterWorkshopReadinessId::Identity:
                    ImGui::TextWrapped(
                        identityReady
                            ? "Four authored names and exact 40 × 40 portrait"
                            : "Author names, portrait, and minimap colour");
                    break;
                case CharacterWorkshopReadinessId::Calibration:
                    ImGui::TextWrapped(
                        "Normalization %s · anchors %s · seat/head sockets %s",
                        normalized ? "confirmed" : "needs review",
                        anchored ? "complete" : "missing",
                        attachmentSocketsMapped ? "mapped" : "missing");
                    break;
                case CharacterWorkshopReadinessId::RigMotion:
                    ImGui::TextWrapped("%s · motion %s", rigStatus, motionReady ? "complete" : "incomplete");
                    break;
                case CharacterWorkshopReadinessId::GameplayProfile:
                    ImGui::TextWrapped("%s · %s", donorName(entry->donor), qualified ? "fingerprint-qualified" : "ROM evidence unavailable");
                    break;
                case CharacterWorkshopReadinessId::VehicleFit: {
                    const CharacterTuningEdit &tuning =
                        loadCharacterTuning(0, entry->id);
                    unsigned       required = 1u;
                    unsigned       reviewed = characterFitReviewed(
                                                  entry,
                                                  tuning,
                                                  MDKR_CHARACTER_CONTEXT_SELECT)
                                                  ? 1u
                                                  : 0u;
                    const uint32_t supported =
                        entry->vehicle_mask & tuning.vehicleMask;
                    for (unsigned context = 1u;
                         context < MDKR_CHARACTER_CONTEXT_COUNT;
                         ++context) {
                        if ((supported & (1u << (context - 1u))) == 0u) continue;
                        ++required;
                        if (characterFitReviewed(entry, tuning, context)) {
                            ++reviewed;
                        }
                    }
                    ImGui::TextWrapped("%u of %u enabled contexts reviewed",
                                       reviewed,
                                       required);
                    break;
                }
                case CharacterWorkshopReadinessId::Performance:
                    ImGui::TextWrapped(
                        "%s · %u authored LOD%s · %s texture accounting",
                        characterPerformanceTier(entry),
                        entry->stats.lod_levels,
                        entry->stats.lod_levels == 1u ? "" : "s",
                        entry->stats.textures == 0u ||
                                entry->stats.decoded_texture_bytes != 0u
                            ? "exact"
                            : "legacy");
                    break;
                case CharacterWorkshopReadinessId::Count:
                    break;
            }
        }
        ImGui::EndTable();
    }
    ImGui::TextDisabled(
        "%u/%zu areas ready · Preview %s · Play %s",
        readiness.readyCount,
        readiness.rows.size(),
        readiness.readyToPreview ? "ready" : "unavailable",
        readiness.readyToPlay ? "ready" : "not ready");
    const std::string action = std::string("Next: ") +
                               readiness.nextActionLabel;
    if (ImGui::Button(action.c_str(), ui::kBtnFullWidth())) {
        persistCharacterWorkshopTab(readiness.nextActionTab, true);
    }
    ui::SpeakFocusedItem(
        readiness.nextActionLabel,
        CharacterWorkshop_tabLabel(readiness.nextActionTab),
        "Opens the single highest-priority unfinished Workshop area. It does not save, build, enable, or assign the character.");
}

bool drawCharacterPackageInspector(const MdkrModernCharacterEntry *entry,
                                   bool                            compact) {
    bool               changed = false;
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
    const bool humanoidRig           = entry->rig_present != 0u &&
                                       entry->rig_mode == MDKR_MODERN_RIG_HUMANOID_RETARGET_V1;
    const bool humanoidRolesComplete = humanoidRig &&
                                       entry->rig_role_mask == MDKR_CHARACTER_RIG_HUMANOID_MASK;
    const bool rigReviewed =
        (entry->rig_flags & MDKR_MODERN_RIG_REVIEWED) != 0u;
    const bool     referenceFallbackReady = humanoidRolesComplete && rigReviewed;
    const uint32_t requiredMotionStates   = raceStates | selectStates;
    const uint32_t solverCoveredStates    = referenceFallbackReady
                                                ? requiredMotionStates & ~entry->semantic_mask
                                                : 0u;
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
                            : !rigReviewed ? "review required"
                                           : "reference/contact fallback ready";
    const bool  motionReady =
        (effectiveMovingStates & requiredMotionStates) == requiredMotionStates;
    const bool                       qualified     = mdkr_modern_donor_qualified(
                                                         static_cast<int>(entry->donor)) != 0;
    const bool                       identityReady = (entry->identity_flags & 1u) != 0u &&
                                                     entry->portrait_bytes != 0u;
    const CharacterWorkshopReadiness readiness     = characterWorkshopReadiness(
        entry,
        normalized,
        anchored,
        attachmentSocketsMapped,
        motionReady,
        qualified,
        identityReady,
        humanoidRig,
        rigReviewed);

    ImGui::PushID(entry->id);
    ImGui::PushFont(AppTheme::fonts().section);
    ImGui::TextUnformatted(entry->display_name);
    ImGui::PopFont();
    ImGui::SameLine();
    ImGui::TextDisabled("%s · %s", entry->enabled != 0u ? "Enabled" : "Disabled", readiness.readyToPlay ? "Ready to play" : "Workshop incomplete");
    ImGui::TextDisabled("%s · %s gameplay profile",
                        entry->short_name,
                        donorName(entry->donor));
    drawCharacterWorkshopTabs();

    if (g_characterWorkshopTab == CharacterWorkshopTab::Overview) {
        ImGui::SeparatorText("Overview");
        ImGui::TextDisabled(
            "Short label: %s · Narration: %s · Sort: %s",
            entry->short_name,
            entry->narration_name,
            entry->sort_label);
        ImGui::TextDisabled(
            "Appearance package · %s gameplay profile · local presentation only",
            donorName(entry->donor));
        ui::TextSubtleWrapped(
            "The package owns local presentation. Its selected retail donor still "
            "owns simulation, collision, race audio, ghost identity, and network/rollback authority; ordinary records and saves never embed the package.");
        ImGui::SeparatorText("Provenance");
        if (entry->provenance_present != 0u) {
            ImGui::TextWrapped("License declaration: %s", entry->license_spdx);
            ImGui::TextWrapped("Creator / attribution: %s", entry->attribution);
            ImGui::TextWrapped("Source: %s", entry->source_url);
            ui::TextSubtleWrapped(
                "These declarations and the exact LICENSE.txt bytes are authenticated by the active source digest. They describe the package; the importer cannot independently establish copyright, trademark, attribution, or redistribution rights.");
        } else {
            ImGui::TextDisabled(
                "Metadata unavailable in this legacy cache. Workshop-installed source history retains and authenticates its exact LICENSE.txt when available; rebuild from that source with current authoring tools to make SPDX, attribution, and source declarations reviewable here.");
        }
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
                "Minimap colour",
                minimap,
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
            characterPerformanceTier(entry),
            entry->lod_triangles[0],
            entry->lod_vertices[0],
            entry->lod_primitives[0],
            entry->stats.materials,
            entry->stats.joints,
            entry->stats.textures,
            entry->stats.lod_levels);
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

        drawCharacterReadiness(
            entry,
            readiness,
            normalized,
            anchored,
            attachmentSocketsMapped,
            motionReady,
            qualified,
            identityReady,
            rigStatus);
    }

    if (g_characterWorkshopTab == CharacterWorkshopTab::RigMotion) {
        ImGui::SeparatorText("Rig and motion status");
        if (entry->rig_role_mask != 0u) {
            ImGui::TextDisabled(
                "Rig contract: %s · %u/16 humanoid roles · %u inferred · minimum confidence %.0f%%",
                rigStatus,
                countCharacterBits(entry->rig_role_mask),
                countCharacterBits(entry->inferred_rig_role_mask),
                static_cast<double>(entry->rig_min_confidence_milli) / 10.0);
        } else {
            ImGui::TextDisabled(
                "Rig contract: %s · no humanoid roles mapped",
                rigStatus);
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
                        "##character-rig-role-review",
                        4,
                        ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg |
                            ImGuiTableFlags_SizingStretchProp)) {
                    ImGui::TableSetupColumn("Role");
                    ImGui::TableSetupColumn("Source joint");
                    ImGui::TableSetupColumn("Mapping");
                    ImGui::TableSetupColumn("Solver basis");
                    ImGui::TableHeadersRow();
                    for (size_t slot = 0u;
                         slot < std::size(kHumanoidRigRoles);
                         ++slot) {
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
                                entry->rig_role_confidence_milli[slot]) /
                            10.0f;
                        const ImVec4 mappingColour =
                            inferred && !rigReviewed ? AppTheme::accent()
                                                     : AppTheme::subtle();
                        ImGui::TextColored(
                            mappingColour,
                            "%s · %.1f%%",
                            inferred ? (rigReviewed ? "inferred, reviewed"
                                                    : "inferred, review needed")
                                     : "authored",
                            static_cast<double>(confidence));
                        ImGui::TableNextColumn();
                        const float *rest = entry->rig_role_rest_rotation[slot];
                        const float *bend = entry->rig_role_bend_axis[slot];
                        const bool   customRest =
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
                entry->rig_role_mask,
                kHumanoidRigRoles);
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
            mappedRaceStates,
            mappedSelectStates,
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
                ~staticAuthoredStates,
                kRaceCharacterSemantics);
            const std::string staticSelect = missingCharacterSemantics(
                ~staticAuthoredStates,
                kSelectCharacterSemantics);
            if (!staticStates.empty() && !staticSelect.empty()) staticStates += ", ";
            staticStates += staticSelect;
            ImGui::PushStyleColor(ImGuiCol_Text, AppTheme::accent());
            ImGui::TextWrapped(
                "One or more explicitly mapped states are static. Authored mappings intentionally bypass reference motion; remove or animate those mappings before calling motion complete. Review: %s",
                staticStates.c_str());
            ImGui::PopStyleColor();
        }
        const std::string missingRace = missingCharacterSemantics(
            entry->semantic_mask,
            kRaceCharacterSemantics);
        const std::string missingSelect = missingCharacterSemantics(
            entry->semantic_mask,
            kSelectCharacterSemantics);
        if (!missingRace.empty()) {
            ImGui::TextWrapped("%s covers missing race states: %s",
                               referenceFallbackReady
                                   ? "Reviewed reference motion"
                                   : "Fallback clip",
                               missingRace.c_str());
        }
        if (!missingSelect.empty()) {
            ImGui::TextWrapped("%s covers missing select states: %s",
                               referenceFallbackReady
                                   ? "Reviewed reference motion"
                                   : "Fallback clip",
                               missingSelect.c_str());
        }
        ImGui::TextDisabled(
            "Sockets: seat %s · head %s · hands L/R %s/%s · feet L/R %s/%s",
            (entry->socket_mask & MDKR_CHARACTER_SOCKET_SEAT)
                ? "authored"
                : "required fallback",
            (entry->socket_mask & MDKR_CHARACTER_SOCKET_HEAD)
                ? "authored"
                : "absent",
            (entry->socket_mask & MDKR_CHARACTER_SOCKET_HAND_LEFT)
                ? "authored"
                : "absent",
            (entry->socket_mask & MDKR_CHARACTER_SOCKET_HAND_RIGHT)
                ? "authored"
                : "absent",
            (entry->socket_mask & MDKR_CHARACTER_SOCKET_FOOT_LEFT)
                ? "authored"
                : "absent",
            (entry->socket_mask & MDKR_CHARACTER_SOCKET_FOOT_RIGHT)
                ? "authored"
                : "absent");
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
    }

    if (g_characterWorkshopTab == CharacterWorkshopTab::Vehicles) {
        ImGui::SeparatorText("Gameplay profile and vehicle compatibility");
        if (drawCharacterProfileStudio(entry)) {
            /* Saving refreshes the registry and invalidates `entry`; finish this
             * inspector immediately and draw the replacement on the next frame. */
            ImGui::PopID();
            return true;
        }
        ImGui::SeparatorText("Fit, motion, and vehicles");
        changed |= drawCharacterTuningEditor(0, entry, compact);
    }

    if (g_characterWorkshopTab == CharacterWorkshopTab::Identity) {
        ImGui::SeparatorText("Portrait Studio");
        if (drawCharacterPortraitStudio(entry)) {
            /* Saving refreshes the registry and invalidates `entry`; finish this
             * inspector immediately and draw the replacement on the next frame. */
            ImGui::PopID();
            return true;
        }
    }

    if (g_characterWorkshopTab == CharacterWorkshopTab::Performance) {
        ImGui::SeparatorText("Performance assembly");
        drawCharacterPerformanceAssembly(entry);
    }

    if (g_characterWorkshopTab == CharacterWorkshopTab::Test) {
        ImGui::SeparatorText("Test in the exact game renderer");
        if (entry->enabled != 0u) {
            drawCharacterExactTests(entry, compact);
        } else {
            ui::TextSubtleWrapped(
                "Exact game tests are unavailable while this package is disabled because the runtime deliberately cannot discover it. Re-enable it to test; source editing and structural performance review remain available above.");
        }
    }

    if (g_characterWorkshopTab == CharacterWorkshopTab::Package) {
        ImGui::SeparatorText("Named drafts and build");
        if (drawCharacterDraftLifecycle(entry)) {
            /* Building refreshes the registry and invalidates `entry`. */
            ImGui::PopID();
            return true;
        }
        if (entry->enabled == 0u) {
            ImGui::PushStyleColor(ImGuiCol_Text, AppTheme::accent());
            ImGui::TextWrapped(
                "Disabled — the game uses the built-in racer for any retained player assignment. Workshop source history and package settings are preserved.");
            ImGui::PopStyleColor();
            if (ImGui::Button("Enable character")) {
                const std::string id = entry->id;
                if (setCharacterPackageEnabled(id, true)) {
                    setStatus(
                        "Custom character enabled; retained player assignments apply on play.",
                        AppTheme::good());
                    ImGui::PopID();
                    return true;
                }
                setStatus("The custom character could not be enabled; open the lifecycle report.",
                          AppTheme::bad());
            }
            ui::SpeakFocusedItem(
                "Enable character",
                nullptr,
                "Makes this retained package available to the game without changing its source revisions or fit settings.");
        }
        ImGui::SeparatorText("Revision history and recovery");
        if (drawCharacterRevisionRecovery(entry)) {
            /* Restore rescans and invalidates this registry row. */
            ImGui::PopID();
            return true;
        }
        ImGui::SeparatorText("Package lifecycle");
        const size_t packageDrafts = characterPackageDraftCount(entry->id);
        const size_t packageTestEvidence =
            characterPackageTestEvidenceCount(entry->id);
        ui::TextSubtleWrapped(
            "Disable is reversible and retains every Workshop revision, fit setting, review, and player assignment. It also retains every named draft. Permanent deletion removes this package's local cache, named drafts, retained revisions, provenance, and package-owned settings.");
        if (entry->enabled != 0u) {
            if (ImGui::Button("Disable without deleting")) {
                const std::string id = entry->id;
                if (setCharacterPackageEnabled(id, false)) {
                    setStatus(
                        "Custom character disabled; its sources, settings, and assignments were retained.",
                        AppTheme::good());
                    ImGui::PopID();
                    return true;
                }
                setStatus("The custom character could not be disabled; open the lifecycle report.",
                          AppTheme::bad());
            }
            ui::SpeakFocusedItem(
                "Disable without deleting",
                nullptr,
                "Uses the built-in racer in game while retaining all package sources, settings, reviews, and player assignments.");
        }
        ui::Gap(ui::kGapS);
        const bool deletionInventoriesWritable =
            g_characterDraftsWritable && g_characterTestEvidenceWritable;
        if (!deletionInventoriesWritable) ImGui::BeginDisabled();
        if (ImGui::Button("Permanently delete package...")) {
            g_characterPendingRemoval = entry->id;
            ImGui::OpenPopup("Permanently delete custom character?");
        }
        if (!deletionInventoriesWritable) ImGui::EndDisabled();
        ui::SpeakFocusedItem(
            "Permanently delete package",
            nullptr,
            "Opens a confirmation for destructive deletion of the local cache, named editor drafts, retained Workshop source revisions, provenance reports, and package-owned settings.");
        if (!deletionInventoriesWritable) {
            ui::TextSubtleWrapped(
                "Permanent deletion is locked because the named-draft or exact-test evidence inventory cannot be read and atomically replaced. Repair that local state first so package-owned work is never orphaned or silently omitted from the confirmation scope.");
        }
        if (ImGui::BeginPopupModal("Permanently delete custom character?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextWrapped(
                "Permanently delete %s from this computer? This removes its %s cache, %zu named draft(s), %zu exact-test result/baseline record(s), %u retained Workshop source revision(s), %u provenance report(s), fit settings, review evidence, and player assignments.",
                entry->display_name,
                entry->enabled != 0u ? "enabled" : "disabled",
                packageDrafts,
                packageTestEvidence,
                entry->source_revisions,
                entry->provenance_reports);
            ui::TextSubtleWrapped(
                "The external .mdkrchar file you originally chose is not touched. A revision created only inside the Workshop may have no other copy. This action cannot be undone here.");
            if (ImGui::Button("Delete package and revisions")) {
                const std::string removedId = g_characterPendingRemoval;
                if (removeCharacterPackage(removedId)) {
                    const AppConfig::PersistResult persist =
                        forgetCharacterPackagePreferences(removedId);
                    const bool preferencesSaved =
                        AppConfig::persistResultApplied(persist);
                    const bool draftsRemoved =
                        forgetCharacterPackageDrafts(removedId);
                    const bool testEvidenceRemoved =
                        forgetCharacterPackageTestEvidence(removedId);
                    setStatus(
                        preferencesSaved && draftsRemoved && testEvidenceRemoved
                            ? "Custom character, named drafts, exact-test evidence, retained revisions, and package settings permanently deleted."
                            : "Character files were deleted, but some local draft, test-evidence, or preference cleanup could not be saved yet.",
                        preferencesSaved && draftsRemoved && testEvidenceRemoved
                            ? AppTheme::good()
                            : AppTheme::bad());
                    g_characterPendingRemoval.clear();
                    ImGui::CloseCurrentPopup();
                    ImGui::EndPopup();
                    ImGui::PopID();
                    return true;
                } else {
                    setStatus(
                        "Character deletion failed or was partial; open the lifecycle report before retrying.",
                        AppTheme::bad());
                }
                ImGui::CloseCurrentPopup();
            }
            ui::SpeakFocusedItem(
                "Delete package and revisions",
                nullptr,
                "Permanently removes every locally retained file and setting owned by this exact package identity. The external file originally imported is unchanged.");
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
            ui::SpeakFocusedItem(
                "Cancel",
                nullptr,
                "Closes this confirmation without changing the package.");
            ImGui::EndPopup();
        }
    }
    (void)autosaveActiveCharacterDraft(entry);
    ImGui::PopID();
    (void)compact;
    return changed;
}

const char *candidateRigName(uint32_t mode) {
    switch (mode) {
        case 0u:
            return "No rig contract";
        case 1u:
            return "Authored clips only";
        case 2u:
            return "Humanoid retarget map";
        default:
            return "Invalid rig contract";
    }
}

const char *candidatePerformanceTier(
    const CharacterCandidateIndex::Candidate &candidate) {
    const uint32_t triangles  = candidate.lodTriangles[0] != 0u
                                    ? candidate.lodTriangles[0]
                                    : candidate.triangles;
    const uint32_t vertices   = candidate.lodVertices[0] != 0u
                                    ? candidate.lodVertices[0]
                                    : candidate.vertices;
    const uint32_t primitives = candidate.lodPrimitives[0] != 0u
                                    ? candidate.lodPrimitives[0]
                                    : candidate.primitives;
    if (candidate.textures != 0u && candidate.decodedTextureBytes == 0u) {
        return "Recompile to measure";
    }
    if (triangles <= 15000u && vertices <= 20000u && primitives <= 4u &&
        candidate.materials <= 4u && candidate.joints <= 64u &&
        candidate.textures <= 8u &&
        candidate.decodedTextureBytes <= 64u * 1024u * 1024u) {
        return "Excellent";
    }
    if (triangles <= 30000u && vertices <= 40000u && primitives <= 8u &&
        candidate.materials <= 8u && candidate.joints <= 96u &&
        candidate.textures <= 12u &&
        candidate.decodedTextureBytes <= 128u * 1024u * 1024u) {
        return "Good";
    }
    if (triangles <= 60000u && vertices <= 70000u && primitives <= 12u &&
        candidate.materials <= 12u && candidate.joints <= 128u &&
        candidate.textures <= 16u &&
        candidate.decodedTextureBytes <= 256u * 1024u * 1024u) {
        return "Heavy";
    }
    return "Very heavy";
}

std::string candidateVehicleName(uint32_t mask) {
    std::string        text;
    static const char *names[] = {"Car", "Hovercraft", "Plane"};
    for (unsigned vehicle = 0u; vehicle < std::size(names); ++vehicle) {
        if ((mask & (1u << vehicle)) == 0u) continue;
        if (!text.empty()) text += ", ";
        text += names[vehicle];
    }
    return text.empty() ? "None" : text;
}

std::string candidateBytes(uint64_t bytes) {
    char text[64];
    std::snprintf(text, sizeof(text), "%.2f MiB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    return text;
}

std::string candidateDelta(uint64_t current, uint64_t next) {
    if (current == next) return "No change";
    if (next > current) return "+" + std::to_string(next - current);
    return "-" + std::to_string(current - next);
}

struct CandidateComparisonRow {
    std::string label;
    std::string current;
    std::string next;
    std::string change;
};

void addCandidateTextRow(std::vector<CandidateComparisonRow> &rows,
                         const char                          *label,
                         const std::string                   &current,
                         const std::string                   &next,
                         bool                                 installed) {
    rows.push_back({label, installed ? current : "Not installed", next, !installed ? "New" : current == next ? "No change"
                                                                                                             : "Changed"});
}

void addCandidateNumberRow(std::vector<CandidateComparisonRow> &rows,
                           const char                          *label,
                           uint64_t                             current,
                           uint64_t                             next,
                           bool                                 installed) {
    rows.push_back({label, installed ? std::to_string(current) : "Not installed", std::to_string(next), installed ? candidateDelta(current, next) : "New"});
}

bool drawCharacterCandidateReview(bool compact) {
    if (!g_characterImportCandidate.ready) return false;
    if (g_characterRawDraftSmokeInstallFrames > 0 &&
        g_characterImportCandidate.disposableRawCandidate) {
        --g_characterRawDraftSmokeInstallFrames;
        if (g_characterRawDraftSmokeInstallFrames == 0) {
            const std::string draftId =
                g_characterImportCandidate.rawDraftId;
            const std::string packageId =
                g_characterImportCandidate.next.id;
            g_characterImportCandidate.rightsConfirmed = true;
            const bool installed = installReviewedCharacterPackage();
            std::fprintf(
                stderr,
                "[app-ui] raw-reviewed-install reviewed=1 installed=%d draft=%s package=%s remaining=%zu\n",
                installed ? 1 : 0, draftId.c_str(), packageId.c_str(),
                g_characterRawDrafts.drafts.size());
            setStatus(
                installed
                    ? "Reviewed raw-source character installed; only its completed authoring draft was removed."
                    : "Automated reviewed raw-source install failed; a fresh review is required.",
                installed ? AppTheme::good() : AppTheme::bad());
            return installed;
        }
    }
    const CharacterImportCandidate           &review     = g_characterImportCandidate;
    const CharacterCandidateIndex::Candidate &next       = review.next;
    const CharacterCandidateIndex::Candidate &current    = review.current;
    const bool                                sameSource = review.installed &&
                                                           current.sourceDigest == next.sourceDigest;
    std::vector<CandidateComparisonRow>       rows;
    addCandidateTextRow(rows, "Display name", current.displayName, next.displayName, review.installed);
    addCandidateTextRow(rows, "Short name", current.shortName, next.shortName, review.installed);
    addCandidateTextRow(rows, "Narration name", current.narrationName, next.narrationName, review.installed);
    addCandidateTextRow(rows, "Sort label", current.sortLabel, next.sortLabel, review.installed);
    addCandidateTextRow(
        rows,
        "License (SPDX)",
        current.provenancePresent ? current.licenseSpdx
                                  : "Unavailable (legacy cache)",
        next.provenancePresent ? next.licenseSpdx
                               : "Unavailable (legacy cache)",
        review.installed);
    addCandidateTextRow(
        rows,
        "Creator / attribution",
        current.provenancePresent ? current.attribution
                                  : "Unavailable (legacy cache)",
        next.provenancePresent ? next.attribution
                               : "Unavailable (legacy cache)",
        review.installed);
    addCandidateTextRow(
        rows,
        "Source",
        current.provenancePresent ? current.sourceUrl
                                  : "Unavailable (legacy cache)",
        next.provenancePresent ? next.sourceUrl
                               : "Unavailable (legacy cache)",
        review.installed);
    addCandidateTextRow(rows, "Portrait", current.identityPresent ? "Authored" : "Generated fallback", next.identityPresent ? "Authored" : "Generated fallback", review.installed);
    addCandidateTextRow(rows, "Gameplay donor", donorName(current.donor), donorName(next.donor), review.installed);
    addCandidateTextRow(rows, "Vehicles", candidateVehicleName(current.vehicleMask), candidateVehicleName(next.vehicleMask), review.installed);
    addCandidateTextRow(rows, "Rig", candidateRigName(current.rigMode), candidateRigName(next.rigMode), review.installed);
    addCandidateTextRow(rows, "Rig review", current.rigMode == 0u ? "Not applicable" : current.rigReviewed ? "Author reviewed"
                                                                                                           : "Review required",
                        next.rigMode == 0u ? "Not applicable" : next.rigReviewed ? "Author reviewed"
                                                                                 : "Review required",
                        review.installed);
    addCandidateNumberRow(rows, "Rig roles", current.rigRoles, next.rigRoles, review.installed);
    addCandidateTextRow(rows, "Performance profile", candidatePerformanceTier(current), candidatePerformanceTier(next), review.installed);
    addCandidateNumberRow(rows, "LOD0 vertices", current.lodVertices[0], next.lodVertices[0], review.installed);
    addCandidateNumberRow(rows, "LOD0 triangles", current.lodTriangles[0], next.lodTriangles[0], review.installed);
    addCandidateNumberRow(rows, "LOD0 draw parts", current.lodPrimitives[0], next.lodPrimitives[0], review.installed);
    for (size_t lod = 1u; lod < 4u; ++lod) {
        if (current.lodVertices[lod] == 0u &&
            next.lodVertices[lod] == 0u) continue;
        const std::string prefix = "LOD" + std::to_string(lod);
        addCandidateNumberRow(rows, (prefix + " vertices").c_str(), current.lodVertices[lod], next.lodVertices[lod], review.installed);
        addCandidateNumberRow(rows, (prefix + " triangles").c_str(), current.lodTriangles[lod], next.lodTriangles[lod], review.installed);
        addCandidateNumberRow(rows, (prefix + " draw parts").c_str(), current.lodPrimitives[lod], next.lodPrimitives[lod], review.installed);
    }
    addCandidateNumberRow(rows, "Vertices", current.vertices, next.vertices, review.installed);
    addCandidateNumberRow(rows, "Triangles", current.triangles, next.triangles, review.installed);
    addCandidateNumberRow(rows, "Draw parts", current.primitives, next.primitives, review.installed);
    addCandidateNumberRow(rows, "LOD levels", current.lodLevels, next.lodLevels, review.installed);
    addCandidateNumberRow(rows, "Materials", current.materials, next.materials, review.installed);
    addCandidateNumberRow(rows, "Textures", current.textures, next.textures, review.installed);
    addCandidateNumberRow(rows, "Joints", current.joints, next.joints, review.installed);
    addCandidateNumberRow(rows, "Animations", current.animations, next.animations, review.installed);
    addCandidateNumberRow(rows, "Animation channels", current.animationChannels, next.animationChannels, review.installed);
    addCandidateNumberRow(rows, "Animation keys", current.animationKeys, next.animationKeys, review.installed);
    rows.push_back({"Decoded texture memory",
                    review.installed ? candidateBytes(current.decodedTextureBytes)
                                     : "Not installed",
                    candidateBytes(next.decodedTextureBytes),
                    review.installed
                        ? candidateDelta(current.decodedTextureBytes,
                                         next.decodedTextureBytes) +
                              " bytes"
                        : "New"});

    ImGui::SeparatorText(review.installed ? "Review update" : "Ready to install");
    ImGui::PushFont(AppTheme::fonts().section);
    ImGui::TextWrapped("%s — %s", next.displayName.c_str(), next.id.c_str());
    ImGui::PopFont();
    ui::TextSubtleWrapped(
        "This package controls local appearance, portrait, animation, fit, and presentation. The selected built-in donor continues to own handling, weight, acceleration, voice, and game authority.");
    if (review.installed && !review.installedEnabled) {
        ImGui::TextDisabled(
            "This package is disabled. Installing the update will preserve that state.");
    }
    if (next.rigMode == 2u && !next.rigReviewed) {
        ImGui::PushStyleColor(ImGuiCol_Text, AppTheme::accent());
        ImGui::TextWrapped(
            "Humanoid mappings are present but not author-reviewed; automatic retargeting remains unavailable until Rig Studio review.");
        ImGui::PopStyleColor();
    }
    if (!next.provenancePresent) {
        ImGui::PushStyleColor(ImGuiCol_Text, AppTheme::accent());
        ImGui::TextWrapped(
            "Legacy portable cache — its exact LICENSE.txt remains bound to the compiled source, but this cache predates reviewable SPDX, attribution, and source metadata. Rebuild it with the current authoring tools to show those declarations here.");
        ImGui::PopStyleColor();
    }

    const bool wide = !compact && ImGui::GetContentRegionAvail().x >=
                                      700.0f * AppTheme::uiScale();
    if (wide && ImGui::BeginTable(
                    "##character-candidate-comparison",
                    4,
                    ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                        ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn(
            "Property", ImGuiTableColumnFlags_WidthStretch, 0.24f);
        ImGui::TableSetupColumn(
            review.installed ? "Installed" : "Current",
            ImGuiTableColumnFlags_WidthStretch, 0.24f);
        ImGui::TableSetupColumn(
            "Candidate", ImGuiTableColumnFlags_WidthStretch, 0.30f);
        ImGui::TableSetupColumn(
            "Change", ImGuiTableColumnFlags_WidthStretch, 0.22f);
        ImGui::TableHeadersRow();
        for (const CandidateComparisonRow &row : rows) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(row.label.c_str());
            ImGui::TableNextColumn();
            ImGui::TextWrapped("%s", row.current.c_str());
            ImGui::TableNextColumn();
            ImGui::TextWrapped("%s", row.next.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(row.change.c_str());
        }
        ImGui::EndTable();
    } else {
        for (const CandidateComparisonRow &row : rows) {
            if (review.installed) {
                ImGui::TextWrapped("%s: %s (installed: %s; %s)",
                                   row.label.c_str(),
                                   row.next.c_str(),
                                   row.current.c_str(),
                                   row.change.c_str());
            } else {
                ImGui::TextWrapped("%s: %s", row.label.c_str(), row.next.c_str());
            }
        }
    }
    if (ImGui::TreeNode("Verified package details")) {
        ImGui::TextWrapped("File: %s", review.packagePath.c_str());
        ImGui::TextWrapped("Package SHA-256: %s",
                           next.packageSha256.c_str());
        ImGui::TextWrapped("Compiled source digest: %s",
                           next.sourceDigest.c_str());
        ImGui::TextWrapped(
            "Provenance metadata: %s",
            next.provenancePresent ? "authenticated and reviewable"
                                   : "unavailable in legacy cache");
        ImGui::TextWrapped("Encoded texture data: %s",
                           candidateBytes(next.encodedTextureBytes).c_str());
        ImGui::Text("Nodes: %u · Skins: %u", next.nodes, next.skins);
        ImGui::TreePop();
    }
    if (sameSource) {
        ImGui::TextDisabled(
            "This exact compiled source is already active. Installing will retain the reviewed package file as a revision and refresh the same runtime cache.");
    }
    (void)ImGui::Checkbox(
        "I confirm I have the right to use this package locally",
        &g_characterImportCandidate.rightsConfirmed);
    ui::SpeakFocusedItem(
        "Local-use rights confirmation",
        nullptr,
        "Required before install. The package includes license text, but the importer cannot verify copyright, trademark, attribution, or redistribution rights and never uploads this content.");
    ui::TextSubtleWrapped(
        "The package includes cryptographically bound license text, but the importer cannot verify copyright, trademark, attribution, or redistribution rights. Installation is local and never uploads the package.");
    if (!g_characterImportCandidate.rightsConfirmed) ImGui::BeginDisabled();
    const char *installLabel = sameSource         ? "Retain reviewed package"
                               : review.installed ? "Install reviewed update"
                                                  : "Install reviewed character";
    if (ImGui::Button(installLabel)) {
        const bool wasUpdate = review.installed;
        if (installReviewedCharacterPackage()) {
            setStatus(
                wasUpdate
                    ? "Reviewed character update installed; local settings and enabled state were preserved."
                    : "Reviewed character installed and ready for Workshop setup.",
                AppTheme::good());
            return true;
        } else {
            setStatus(
                "Nothing was installed. The package or installed character may have changed; validate and review it again.",
                AppTheme::bad());
            return false;
        }
    }
    if (!g_characterImportCandidate.rightsConfirmed) ImGui::EndDisabled();
    ui::SpeakFocusedItem(
        installLabel,
        nullptr,
        "Commits only the exact package bytes and installed base shown in this review. Gameplay authority remains with the named built-in donor.");
    ImGui::SameLine();
    const bool disposableCandidate =
        g_characterImportCandidate.disposableRawCandidate;
    if (ImGui::Button("Discard candidate")) {
        if (disposableCandidate &&
            !g_characterImportCandidate.packagePath.empty()) {
            (void)mdkr_remove_utf8(
                g_characterImportCandidate.packagePath.c_str());
        }
        g_characterImportCandidate = CharacterImportCandidate{};
        setStatus(
            disposableCandidate
                ? "Generated candidate removed; the raw draft and external source files remain unchanged."
                : "Candidate review discarded; no installed files changed.",
            AppTheme::subtle());
        return false;
    }
    ui::SpeakFocusedItem(
        "Discard candidate",
        nullptr,
        disposableCandidate
            ? "Closes this review and removes only the disposable generated package. The raw draft and external model and license remain unchanged."
            : "Closes this review without installing or deleting any files.");
    return false;
}

bool characterRawPackageIdValid(const char *value) {
    if (value == nullptr) return false;
    const size_t length = std::strlen(value);
    if (length < 2u || length > 64u ||
        !((value[0] >= 'a' && value[0] <= 'z') ||
          (value[0] >= '0' && value[0] <= '9'))) return false;
    for (size_t index = 1u; index < length; ++index) {
        const char byte = value[index];
        if (!((byte >= 'a' && byte <= 'z') ||
              (byte >= '0' && byte <= '9') || byte == '.' ||
              byte == '_' || byte == '-')) return false;
    }
    return true;
}

bool drawCharacterRawChoice(const char *label,
                            const std::vector<std::string> &choices,
                            int &selected, const char *help) {
    const char *preview = selected >= 0 &&
            selected < static_cast<int>(choices.size())
        ? choices[static_cast<size_t>(selected)].c_str() : "Choose mapping";
    ImGui::SetNextItemWidth(-1.0f);
    const bool open = ImGui::BeginCombo(label, preview);
    ui::SpeakFocusedItem(label, preview, help);
    bool changed = false;
    if (open) {
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(choices.size()));
        while (clipper.Step()) {
            for (int index = clipper.DisplayStart;
                 index < clipper.DisplayEnd; ++index) {
                ImGui::PushID(index);
                if (ImGui::Selectable(
                        choices[static_cast<size_t>(index)].c_str(),
                        selected == index)) {
                    selected = index;
                    changed = true;
                }
                ImGui::PopID();
            }
        }
        ImGui::EndCombo();
    }
    return changed;
}

void drawCharacterRawIntakeEditor(bool rail) {
    loadCharacterRawIntake();
    CharacterRawIntake &intake = g_characterRawIntake;
    if (intake.modelPath[0] == '\0') return;
    const char *smokeAction = std::getenv(
        "MDKR_APP_SMOKE_RAW_DRAFT_ACTION");
    const char *smokeToken = std::getenv(
        "MDKR_APP_SMOKE_RAW_DRAFT_ACTION_TOKEN");
    if (!g_characterRawDraftSmokeActionApplied && smokeAction != nullptr &&
        smokeToken != nullptr &&
        std::strcmp(smokeToken, "mdkr64-app-raw-draft-v1") == 0 &&
        std::strcmp(smokeAction, "build-reviewed-install") != 0) {
        g_characterRawDraftSmokeActionApplied = true;
        bool applied = false;
        if (std::strncmp(smokeAction, "select-sha:", 11u) == 0) {
            const std::string digest = smokeAction + 11u;
            const auto matching = std::find_if(
                g_characterRawDrafts.drafts.begin(),
                g_characterRawDrafts.drafts.end(),
                [&digest](const CharacterRawDraftStore::Draft &draft) {
                    return draft.mappingModelSha256 == digest;
                });
            if (matching != g_characterRawDrafts.drafts.end()) {
                const std::string matchingId = matching->id;
                applied = activateCharacterRawDraft(matchingId);
            }
        } else if (std::strcmp(smokeAction, "delete-selected") == 0) {
            applied = clearCharacterRawIntake();
        } else if (std::strcmp(smokeAction, "duplicate-selected") == 0) {
            applied = duplicateCharacterRawDraft();
        } else if (std::strcmp(smokeAction, "close-editor") == 0) {
            applied = setCharacterRawEditorOpen(false);
        }
        std::fprintf(
            stderr,
            "[app-ui] raw-draft-action action=%s applied=%d drafts=%zu selected=%s\n",
            smokeAction, applied ? 1 : 0,
            g_characterRawDrafts.drafts.size(),
            g_characterRawDrafts.selectedId.c_str());
        if (intake.modelPath[0] == '\0') return;
    }
    if (!g_characterRawIntakeTracePrinted &&
        std::getenv("MDKR_APP_UI_TRACE") != nullptr) {
        std::fprintf(
            stderr,
            "[app-ui] raw-intake resumed=1 inspected=%d mappings=%d drafts=%zu selected=%s\n",
            intake.inspected ? 1 : 0,
            intake.fallback >= 0 && intake.seat >= 0 && intake.head >= 0
                ? 1 : 0,
            g_characterRawDrafts.drafts.size(), intake.draftId.c_str());
        g_characterRawIntakeTracePrinted = true;
    }
    ImGui::SeparatorText("Raw GLB authoring draft");
    ui::TextSubtleWrapped(
        "This resumable draft creates a source-only package, then hands it to the same mutation-free review and local-rights confirmation as every other import. Nothing here changes the installed library.");
    const std::string activeLabel = intake.displayName[0] != '\0'
        ? intake.displayName : intake.modelPath;
    ImGui::SetNextItemWidth(-1.0f);
    const bool draftListOpen = ImGui::BeginCombo(
        "Raw authoring draft", activeLabel.c_str());
    ui::SpeakFocusedItem(
        "Raw authoring draft", activeLabel.c_str(),
        "Switches among independently saved source authoring sessions. Switching closes a disposable candidate review and requires the selected source to be inspected again; it never changes external files.");
    std::string switchToDraft;
    if (draftListOpen) {
        for (const CharacterRawDraftStore::Draft &draft :
             g_characterRawDrafts.drafts) {
            const std::string label =
                (draft.displayName.empty() ? draft.modelPath
                                           : draft.displayName) +
                "##raw-draft-" + draft.id;
            if (ImGui::Selectable(
                    label.c_str(), draft.id == intake.draftId)) {
                switchToDraft = draft.id;
            }
            const std::string state = draft.id == intake.draftId
                ? "selected" : "saved";
            ui::SpeakFocusedItem(
                draft.displayName.empty() ? draft.modelPath.c_str()
                                          : draft.displayName.c_str(),
                state.c_str(),
                "Selects this exact local authoring draft. Its model and license files are not copied, modified, or deleted.");
        }
        ImGui::EndCombo();
    }
    if (!switchToDraft.empty() && switchToDraft != intake.draftId) {
        if (activateCharacterRawDraft(switchToDraft)) {
            setStatus(
                "Raw authoring draft selected; inspect its exact GLB before building.",
                AppTheme::good());
        } else {
            setStatus(
                "The selected raw authoring draft could not be persisted; the current draft remains active.",
                AppTheme::bad());
        }
    }
    ImGui::TextDisabled(
        "%zu of %zu local raw drafts · Draft ID: %s",
        g_characterRawDrafts.drafts.size(),
        CharacterRawDraftStore::kMaximumDrafts,
        intake.draftId.c_str());
    const bool rawDraftCapacityAvailable =
        g_characterRawDrafts.drafts.size() <
        CharacterRawDraftStore::kMaximumDrafts;
    if (!rawDraftCapacityAvailable) ImGui::BeginDisabled();
    if (ImGui::Button("Duplicate as a new raw draft")) {
        if (duplicateCharacterRawDraft()) {
            setStatus(
                "Independent raw draft created; inspect its source and change the copied package ID if this branch should install as a separate character.",
                AppTheme::good());
        } else {
            setStatus(
                "The raw draft could not be duplicated; the source draft remains unchanged.",
                AppTheme::bad());
        }
    }
    if (!rawDraftCapacityAvailable) ImGui::EndDisabled();
    ui::SpeakFocusedItem(
        "Duplicate as a new raw draft",
        rawDraftCapacityAvailable ? nullptr : "The bounded draft library is full.",
        "Creates an independent authoring branch from this model and its saved choices without copying or changing external files. Reinspection is required. The package ID is copied; change it to install a separate character.");
    if (ImGui::Button("Close raw editor")) {
        if (setCharacterRawEditorOpen(false)) {
            setStatus(
                "Raw authoring closed; every draft remains saved and can be resumed from the import area.",
                AppTheme::subtle());
        } else {
            setStatus(
                "Raw authoring closed for this session, but the launcher could not remember that navigation state.",
                AppTheme::accent());
        }
    }
    ui::SpeakFocusedItem(
        "Close raw editor", nullptr,
        "Returns to the installed character library without deleting this draft, its authoring choices, or external source files. Resume it from the import area.");
    ui::TextSubtleWrapped(
        "Choose another GLB in the source field above to create or resume another draft. Each model keeps independent identity, provenance, donor, vehicle, axis, scale, and source-bound mapping choices.");
    ImGui::TextWrapped("Model: %s", intake.modelPath);
    bool changed = false;
    if (!intake.inspected) {
        ImGui::PushStyleColor(ImGuiCol_Text, AppTheme::accent());
        ImGui::TextWrapped(
            "Inspection is required%s. It fingerprints the current GLB and inventories exact animation and node names; changing the file invalidates the build.",
            intake.savedMappingModelSha256.empty()
                ? "" : " to resume and verify this saved draft");
        ImGui::PopStyleColor();
        if (ImGui::Button("Inspect GLB model")) {
            if (inspectCharacterRawGlb(intake.modelPath)) {
                setStatus("GLB inspected; review every inferred authoring choice.",
                          AppTheme::good());
            } else {
                setStatus("GLB inspection failed; open the manager report.",
                          AppTheme::bad());
            }
        }
        ui::SpeakFocusedItem(
            "Inspect GLB model", nullptr,
            "Validates and fingerprints the model, then inventories animation and node names without building or installing a package.");
    } else {
        ImGui::TextDisabled(
            "%u vertices · %u triangles · %u joints · %.3g m source height",
            intake.inventory.vertices, intake.inventory.triangles,
            intake.inventory.joints, intake.inventory.sourceHeightM);
        ImGui::TextDisabled("Inspected GLB SHA-256: %s",
                            intake.inventory.modelSha256.c_str());
    }

    ImGui::TextUnformatted("Package ID");
    ImGui::SetNextItemWidth(-1.0f);
    changed |= ImGui::InputTextWithHint(
        "##raw-package-id", "org.example.character", intake.packageId,
        sizeof(intake.packageId));
    ui::SpeakFocusedItem(
        "Package ID", intake.packageId,
        "A stable 2 to 64 character lowercase identifier. Updating the same ID preserves package-owned settings and assignments.");
    if (intake.packageId[0] != '\0' &&
        !characterRawPackageIdValid(intake.packageId)) {
        ImGui::TextColored(
            AppTheme::bad(),
            "Use 2–64 lowercase letters, digits, dots, underscores, or hyphens.");
    }
    ImGui::TextUnformatted("Display name");
    ImGui::SetNextItemWidth(-1.0f);
    changed |= ImGui::InputText(
        "##raw-display-name", intake.displayName, sizeof(intake.displayName));
    ui::SpeakFocusedItem("Display name", intake.displayName,
                         "The authored roster and Workshop name.");

    ImGui::TextUnformatted("Exact license or notice file");
    ImGui::SetNextItemWidth(-1.0f);
    changed |= ImGui::InputTextWithHint(
        "##raw-license-path", "/path/to/LICENSE", intake.licensePath,
        sizeof(intake.licensePath));
    ui::SpeakFocusedItem(
        "License file", intake.licensePath,
        "The exact bounded text bytes embedded and authenticated in the package.");
    if (filedialog::isAvailable()) {
        if (ImGui::Button("Browse for license...")) {
            std::string picked;
            if (filedialog::openCharacterLicense(picked)) {
                std::snprintf(intake.licensePath, sizeof(intake.licensePath),
                              "%s", picked.c_str());
                changed = true;
            }
        }
        ui::SpeakFocusedItem(
            "Browse for license", nullptr,
            "Chooses the exact LICENSE, COPYING, or notice file to embed. It does not infer or grant rights.");
    }
    ImGui::TextUnformatted("SPDX license expression");
    ImGui::SetNextItemWidth(-1.0f);
    changed |= ImGui::InputTextWithHint(
        "##raw-spdx", "CC-BY-4.0", intake.spdx, sizeof(intake.spdx));
    ui::SpeakFocusedItem(
        "SPDX license expression", intake.spdx,
        "A declaration supplied by the author; the Workshop does not guess it from the license file.");
    ImGui::TextUnformatted("Creator / attribution");
    ImGui::SetNextItemWidth(-1.0f);
    changed |= ImGui::InputText(
        "##raw-attribution", intake.attribution,
        sizeof(intake.attribution));
    ui::SpeakFocusedItem("Creator and attribution", intake.attribution,
                         "The credit authenticated in the package manifest.");
    ImGui::TextUnformatted("Source URL");
    ImGui::SetNextItemWidth(-1.0f);
    changed |= ImGui::InputText(
        "##raw-source-url", intake.sourceUrl, sizeof(intake.sourceUrl));
    ui::SpeakFocusedItem("Source URL", intake.sourceUrl,
                         "The author-declared origin of these model bytes.");

    static const char *donors[] = {
        "Krunch", "Bumper", "Tiptup", "Conker", "Timber",
        "Banjo", "Drumstick", "Pipsy", "T.T.", "Diddy",
    };
    ImGui::SetNextItemWidth(-1.0f);
    changed |= ImGui::Combo(
        "Gameplay donor", &intake.donor, donors,
        static_cast<int>(std::size(donors)));
    ui::SpeakFocusedItem(
        "Gameplay donor", donors[intake.donor],
        "Chooses the built-in racer that remains authoritative for physics, collision, audio, ghosts, records, and network identity.");
    ImGui::TextUnformatted("Supported vehicles");
    static const char *vehicles[] = {"Car", "Hovercraft", "Plane"};
    for (int vehicle = 0; vehicle < 3; ++vehicle) {
        changed |= ImGui::Checkbox(vehicles[vehicle], &intake.vehicles[vehicle]);
        ui::SpeakFocusedItem(
            vehicles[vehicle], intake.vehicles[vehicle] ? "Supported" : "Excluded",
            "Declares whether this source package exposes an authoring and test context for the vehicle.");
        if (!rail && vehicle != 2) ImGui::SameLine();
    }

    static const char *forwards[] = {"+Z", "-Z", "+X", "-X"};
    ImGui::SetNextItemWidth(-1.0f);
    changed |= ImGui::Combo(
        "Model faces", &intake.sourceForward, forwards,
        static_cast<int>(std::size(forwards)));
    ui::SpeakFocusedItem(
        "Model faces", forwards[intake.sourceForward],
        "Declares the model's unmodified horizontal forward axis. Geometry cannot infer facing reliably.");
    ImGui::SetNextItemWidth(-1.0f);
    changed |= ImGui::InputFloat(
        "Standing height in metres", &intake.targetHeight, 0.01f, 0.1f,
        "%.3f");
    ui::SpeakFocusedItem(
        "Standing height in metres", nullptr,
        "Sets normalized authored height from 0.1 to 10 metres; vehicle placement is calibrated separately after import.");

    if (intake.inspected) {
        changed |= drawCharacterRawChoice(
            "Fallback animation", intake.inventory.clips, intake.fallback,
            "Required motion source used when a semantic clip is absent.");
        changed |= drawCharacterRawChoice(
            "Seat or pelvis node", intake.inventory.nodes, intake.seat,
            "Required vehicle anchor. Review the inference; a root node is not always the pelvis.");
        changed |= drawCharacterRawChoice(
            "Head node", intake.inventory.nodes, intake.head,
            "Required head anchor used by presentation and camera-aware placement.");
    }

    if (changed && !saveCharacterRawIntake()) {
        setStatus(("Raw import draft changed but could not be persisted: " +
                   g_characterRawDraftError).c_str(),
                  AppTheme::bad());
    }
    const bool hasVehicle = intake.vehicles[0] || intake.vehicles[1] ||
                            intake.vehicles[2];
    const bool ready = intake.inspected &&
        characterRawPackageIdValid(intake.packageId) &&
        intake.displayName[0] != '\0' && intake.licensePath[0] != '\0' &&
        intake.spdx[0] != '\0' && intake.attribution[0] != '\0' &&
        intake.sourceUrl[0] != '\0' && hasVehicle &&
        intake.targetHeight >= 0.1f && intake.targetHeight <= 10.0f &&
        intake.fallback >= 0 && intake.seat >= 0 && intake.head >= 0;
    if (!ready) ImGui::BeginDisabled();
    bool buildRequested = ImGui::Button("Build source package for review") &&
                          ready;
    const bool smokeBuildRequested =
        !g_characterRawDraftSmokeActionApplied && ready &&
        smokeAction != nullptr && smokeToken != nullptr &&
        std::strcmp(smokeAction, "build-reviewed-install") == 0 &&
        std::strcmp(smokeToken, "mdkr64-app-raw-draft-v1") == 0;
    if (smokeBuildRequested) {
        g_characterRawDraftSmokeActionApplied = true;
        buildRequested = true;
    }
    if (buildRequested) {
        const bool built = buildCharacterRawGlbCandidate();
        if (built) {
            if (smokeBuildRequested) {
                g_characterRawDraftSmokeInstallFrames = 2;
            }
            setStatus(
                "Source package built from the inspected GLB; review its exact diff and provenance before installing.",
                AppTheme::good());
        } else {
            setStatus(
                "Raw GLB build failed; no installed character changed. Open the manager report.",
                AppTheme::bad());
        }
        if (smokeBuildRequested) {
            std::fprintf(
                stderr,
                "[app-ui] raw-draft-action action=%s applied=%d drafts=%zu selected=%s\n",
                smokeAction, built ? 1 : 0,
                g_characterRawDrafts.drafts.size(),
                g_characterRawDrafts.selectedId.c_str());
        }
    }
    if (!ready) ImGui::EndDisabled();
    ui::SpeakFocusedItem(
        "Build source package for review",
        ready ? nullptr : "Complete inspection, identity, provenance, vehicle, calibration, and required mappings first.",
        "Snapshots the GLB and license, builds a deterministic source package, and opens the ordinary mutation-free package review. It does not install the character.");
    if (!rail) ImGui::SameLine();
    if (ImGui::Button("Delete raw authoring draft...")) {
        ImGui::OpenPopup("Delete raw character authoring draft?");
    }
    ui::SpeakFocusedItem(
        "Delete raw authoring draft", nullptr,
        "Opens a confirmation to delete only this local authoring record and its disposable generated candidate. Other drafts and external model and license files are never deleted.");
    if (ImGui::BeginPopupModal(
            "Delete raw character authoring draft?", nullptr,
            ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped(
            "Delete only this saved authoring draft and its disposable generated candidate? The other raw drafts, installed characters, external GLB, and license file are not changed or deleted.");
        if (ImGui::Button("Delete this draft")) {
            if (clearCharacterRawIntake()) {
                setStatus(
                    "Raw authoring draft deleted; source files and other drafts remain unchanged.",
                    AppTheme::good());
                ImGui::CloseCurrentPopup();
            } else {
                setStatus(
                    "The raw authoring draft could not be deleted; no draft changed.",
                    AppTheme::bad());
            }
        }
        ui::SpeakFocusedItem(
            "Delete this draft", nullptr,
            "Deletes this exact local authoring record and generated candidate without touching other drafts, source files, or installed characters.");
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ui::SpeakFocusedItem("Cancel", nullptr,
                             "Keeps the raw import draft unchanged.");
        ImGui::EndPopup();
    }
}

void drawCharacterImportControls(bool rail) {
    loadCharacterRawIntake();
    const char *smokeSource = std::getenv(
        "MDKR_APP_SMOKE_CHARACTER_CONVERSION_SOURCE");
    const char *smokeOutput = std::getenv(
        "MDKR_APP_SMOKE_CHARACTER_CONVERSION_OUTPUT");
    const char *smokeToken = std::getenv(
        "MDKR_APP_SMOKE_CHARACTER_CONVERSION_TOKEN");
    if (!g_characterConversionSmokePrefilled &&
        smokeSource != nullptr && smokeSource[0] != '\0' &&
        smokeOutput != nullptr && smokeOutput[0] != '\0' &&
        smokeToken != nullptr && std::strcmp(
            smokeToken, "mdkr64-character-conversion-v1") == 0) {
        g_characterConversionSmokePrefilled = true;
        std::snprintf(g_characterImportPath,
                      sizeof(g_characterImportPath), "%s", smokeSource);
        std::snprintf(g_characterConversionOutputPath,
                      sizeof(g_characterConversionOutputPath), "%s",
                      smokeOutput);
    }
    if (!g_characterRawDraftsWritable &&
        !g_characterRawDraftError.empty()) {
        if (!g_characterRawDraftStoreTracePrinted &&
            std::getenv("MDKR_APP_UI_TRACE") != nullptr) {
            std::fprintf(
                stderr,
                "[app-ui] raw-draft-store writable=0 error=%s\n",
                g_characterRawDraftError.c_str());
            g_characterRawDraftStoreTracePrinted = true;
        }
        ImGui::PushStyleColor(ImGuiCol_Text, AppTheme::bad());
        ImGui::TextWrapped(
            "Raw authoring drafts are read-only because their local inventory failed validation: %s",
            g_characterRawDraftError.c_str());
        ImGui::PopStyleColor();
        ui::TextSubtleWrapped(
            "Installed characters and external source files are unaffected. Preserve or repair the inventory before starting another raw draft; malformed bytes are never partially loaded or overwritten.");
    }
    if (!g_characterRawEditorOpen &&
        !g_characterRawDrafts.selectedId.empty()) {
        const char *smokeAction = std::getenv(
            "MDKR_APP_SMOKE_RAW_DRAFT_ACTION");
        const char *smokeToken = std::getenv(
            "MDKR_APP_SMOKE_RAW_DRAFT_ACTION_TOKEN");
        if (!g_characterRawDraftClosedTracePrinted &&
            std::getenv("MDKR_APP_UI_TRACE") != nullptr) {
            std::fprintf(
                stderr,
                "[app-ui] raw-draft-library open=0 drafts=%zu selected=%s\n",
                g_characterRawDrafts.drafts.size(),
                g_characterRawDrafts.selectedId.c_str());
            g_characterRawDraftClosedTracePrinted = true;
        }
        if (!g_characterRawDraftSmokeActionApplied &&
            smokeAction != nullptr && smokeToken != nullptr &&
            std::strcmp(smokeAction, "resume-selected") == 0 &&
            std::strcmp(smokeToken, "mdkr64-app-raw-draft-v1") == 0) {
            g_characterRawDraftSmokeActionApplied = true;
            const bool applied = activateCharacterRawDraft(
                g_characterRawDrafts.selectedId);
            std::fprintf(
                stderr,
                "[app-ui] raw-draft-action action=%s applied=%d drafts=%zu selected=%s\n",
                smokeAction, applied ? 1 : 0,
                g_characterRawDrafts.drafts.size(),
                g_characterRawDrafts.selectedId.c_str());
        }
        const CharacterRawDraftStore::Draft *saved =
            CharacterRawDraftStore::find(
                g_characterRawDrafts, g_characterRawDrafts.selectedId);
        const std::string resumeLabel =
            "Resume raw draft (" +
            std::to_string(g_characterRawDrafts.drafts.size()) +
            " saved)";
        if (ImGui::Button(resumeLabel.c_str(), ui::kBtnFullWidth())) {
            if (activateCharacterRawDraft(
                    g_characterRawDrafts.selectedId)) {
                setStatus(
                    "Raw authoring resumed; inspect the exact GLB before building.",
                    AppTheme::good());
            } else {
                setStatus(
                    "The saved raw authoring draft could not be opened; no draft changed.",
                    AppTheme::bad());
            }
        }
        ui::SpeakFocusedItem(
            "Resume raw authoring draft",
            saved != nullptr && !saved->displayName.empty()
                ? saved->displayName.c_str()
                : "Saved source draft",
            "Opens the selected local raw draft and closes any uninstalled candidate review. External model and license files remain unchanged; source reinspection is required.");
    }
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##character-package-path",
                             "/path/to/package, model, or DCC source",
                             g_characterImportPath,
                             sizeof(g_characterImportPath));
    const bool inlineActions = !rail &&
                               ImGui::GetContentRegionAvail().x >=
                                   760.0f * AppTheme::uiScale();
    if (filedialog::isAvailable()) {
        if (ImGui::Button("Browse for source...")) {
            std::string picked;
            if (filedialog::openCharacterSource(picked)) {
                std::snprintf(g_characterImportPath,
                              sizeof(g_characterImportPath),
                              "%s",
                              picked.c_str());
            }
        }
        ui::SpeakFocusedItem(
            "Browse for character source",
            nullptr,
            "Chooses a local mdkrchar package, self-contained GLB, COLLADA model, authoring ZIP, or common DCC source. DCC projects receive safe GLB export guidance; nothing is installed until validation, review, and explicit confirmation succeed.");
    }
    const CharacterSourceKind sourceKind = characterSourceKind(
        g_characterImportPath);
    const bool rawGlb = sourceKind == CharacterSourceKind::Glb;
    const bool convertibleSource = sourceKind == CharacterSourceKind::Dae ||
        sourceKind == CharacterSourceKind::Zip;
    const bool dccExportRequired = characterSourceNeedsDccExport(sourceKind);
    const std::string exportGuidance = dccExportRequired
        ? characterSourceExportGuidance(sourceKind) : std::string{};
    if (dccExportRequired) {
        if (ui::CardBegin("##character-dcc-export-guidance",
                          AppTheme::accent(), 0.0f)) {
            ImGui::TextColored(
                AppTheme::accent(), "%s needs a GLB export",
                characterSourceFormatName(sourceKind));
            ui::TextSubtleWrapped("%s", exportGuidance.c_str());
            if (ImGui::Button("Copy GLB export checklist")) {
                ImGui::SetClipboardText(exportGuidance.c_str());
                setStatus(
                    "GLB export checklist copied; the selected authoring source was not changed.",
                    AppTheme::good());
            }
            ui::SpeakFocusedItem(
                "Copy GLB export checklist", nullptr,
                "Copies the format-specific self-contained GLB export steps. It does not open, execute, convert, or change the selected DCC source.");
        }
        ui::CardEnd();
    }
    if (convertibleSource) {
        ui::TextSubtleWrapped(
            "DAE and ZIP are conversion inputs, never installable packages. Choose a new GLB destination; the bounded converter refuses traversal, encrypted/symlink members, ambiguous model choices, external textures, authored COLLADA animation, and every overwrite. The resulting self-contained GLB enters the ordinary resumable authoring and review flow.");
        ImGui::SetNextItemWidth(
            filedialog::isAvailable()
                ? std::max(120.0f, ImGui::GetContentRegionAvail().x -
                                      ui::kBtnSecondary().x - ui::kGapS)
                : -1.0f);
        ImGui::InputTextWithHint(
            "Converted GLB destination##character-conversion-output",
            "/path/to/new-character.glb",
            g_characterConversionOutputPath,
            sizeof(g_characterConversionOutputPath));
        ui::SpeakFocusedItem(
            "Converted GLB destination", g_characterConversionOutputPath,
            "Must be a new GLB filename in an existing real directory. Conversion never replaces an existing path.");
        if (filedialog::isAvailable()) {
            ImGui::SameLine();
            if (ImGui::Button("Choose output...", ui::kBtnSecondary())) {
                std::string picked;
                if (filedialog::saveCharacterConvertedGlb(picked)) {
                    std::snprintf(
                        g_characterConversionOutputPath,
                        sizeof(g_characterConversionOutputPath), "%s",
                        picked.c_str());
                }
            }
            ui::SpeakFocusedItem(
                "Choose converted GLB output", nullptr,
                "Opens the operating system destination picker. Selecting an existing filename still does not grant overwrite authority.");
        }
    }
    const bool canImport = g_characterImportPath[0] != '\0' &&
        !dccExportRequired &&
        (!convertibleSource || g_characterConversionOutputPath[0] != '\0');
    if (filedialog::isAvailable() && inlineActions &&
        !convertibleSource && !dccExportRequired) {
        ImGui::SameLine();
    }
    if (!canImport) ImGui::BeginDisabled();
    if (ImGui::Button(
            dccExportRequired ? "Export a self-contained GLB to continue" :
            convertibleSource ? "Convert, inspect, and continue" :
            rawGlb ? "Inspect GLB and continue" :
                     "Validate and review")) {
        (void)Settings_importCharacterPackage(g_characterImportPath);
    }
    if (!canImport) ImGui::EndDisabled();
    ui::SpeakFocusedItem(
        dccExportRequired ? "Export a self-contained GLB to continue" :
        convertibleSource ? "Convert, inspect, and continue" :
        rawGlb ? "Inspect GLB and continue" : "Validate and review",
        canImport ? nullptr :
            dccExportRequired
                ? "Use the format-specific checklist above, then choose or drop the exported GLB."
                : convertibleSource
                ? "Choose a new converted GLB destination first."
                : "Choose or enter a character source path first.",
        dccExportRequired
            ? "The Workshop does not execute native project files or guess at unstable interchange semantics. Export one self-contained GLB while preserving the original source."
            : convertibleSource
            ? "Converts one explicit or unambiguous safe COLLADA source to a new self-contained GLB, then opens the same resumable authoring draft. It never installs or overwrites a file."
            : rawGlb
            ? "Validates and fingerprints the raw model, then opens a resumable package-authoring draft. It does not build or install yet."
            : "Stages a mutation-free inventory and installed-version comparison. It does not install the package.");
    if (inlineActions) ImGui::SameLine();
    if (ImGui::Button("Rescan installed characters")) {
        refreshCharacterRegistry();
    }
    ui::SpeakFocusedItem(
        "Rescan installed characters",
        nullptr,
        "Reloads the bounded local library without enabling, disabling, or changing any package.");
    if (!g_characterRegistryDirectory.empty()) {
        int       enabledCount = 0;
        const int inventoryCount =
            mdkr_modern_character_registry_count(&g_characterRegistry);
        for (int index = 0; index < inventoryCount; ++index) {
            const MdkrModernCharacterEntry *entry =
                mdkr_modern_character_registry_entry(&g_characterRegistry,
                                                     index);
            if (entry != nullptr && entry->enabled != 0u) ++enabledCount;
        }
        ImGui::TextDisabled("%d enabled · %d disabled · Folder: %s",
                            enabledCount,
                            inventoryCount - enabledCount,
                            g_characterRegistryDirectory.c_str());
    }
    if (!g_characterManagerReport.empty() &&
        ImGui::TreeNode("Last importer report")) {
        ImGui::TextWrapped("%s", g_characterManagerReport.c_str());
        ImGui::TreePop();
    }
}

const MdkrModernCharacterEntry *resolveCharacterWorkshopSelection() {
    const int characterCount =
        mdkr_modern_character_registry_count(&g_characterRegistry);
    if (characterCount <= 0) return nullptr;
    if (!g_characterWorkshopSelectionLoaded) {
        g_characterWorkshopSelection = AppConfig::get(
            "character_workshop_last_selected");
        g_characterWorkshopSelectionLoaded = true;
    }
    int index = mdkr_modern_character_registry_find(
        &g_characterRegistry,
        g_characterWorkshopSelection.c_str());
    if (index < 0) {
        index = 0;
        const MdkrModernCharacterEntry *first =
            mdkr_modern_character_registry_entry(&g_characterRegistry, 0);
        g_characterWorkshopSelection = first != nullptr ? first->id : "";
        (void)AppConfig::setAndSave(
            "character_workshop_last_selected",
            g_characterWorkshopSelection);
    }
    return mdkr_modern_character_registry_entry(&g_characterRegistry, index);
}

void selectCharacterWorkshopEntry(const MdkrModernCharacterEntry *entry) {
    if (entry == nullptr) return;
    if (g_characterRawEditorOpen) {
        (void)setCharacterRawEditorOpen(false);
    }
    if (g_characterWorkshopSelection == entry->id) return;
    g_characterWorkshopSelection          = entry->id;
    const AppConfig::PersistResult result = AppConfig::setAndSave(
        "character_workshop_last_selected",
        g_characterWorkshopSelection);
    if (!AppConfig::persistResultApplied(result)) {
        setStatus(
            "Character selected, but the launcher could not remember it for next time.",
            AppTheme::accent());
    }
}

const MdkrModernCharacterEntry *drawCharacterLibrary(bool rail) {
    const int characterCount =
        mdkr_modern_character_registry_count(&g_characterRegistry);
    const MdkrModernCharacterEntry *workshopEntry =
        resolveCharacterWorkshopSelection();
    if (rail) {
        ImGui::TextUnformatted("Installed characters");
    } else {
        ImGui::SeparatorText("Character library");
    }
    if (characterCount <= 0) {
        ui::TextSubtleWrapped(
            "No characters are installed yet. Choose a package above, validate its inventory, and explicitly install the reviewed candidate.");
        return nullptr;
    }
    if (!rail) {
        ImGui::SetNextItemWidth(-1.0f);
        const std::string workshopPreview  = workshopEntry != nullptr
                                                 ? std::string(workshopEntry->display_name) +
                                                       (workshopEntry->enabled != 0u ? "" : " (disabled)")
                                                 : "Choose a character";
        const bool        libraryComboOpen = ImGui::BeginCombo(
            "Character to edit##character-workshop-library",
            workshopPreview.c_str());
        ui::SpeakFocusedItem(
            "Character to edit",
            workshopEntry != nullptr ? workshopEntry->narration_name
                                     : "No character selected",
            "Choose an installed character package to inspect or edit.");
        if (libraryComboOpen) {
            for (int index = 0; index < characterCount; ++index) {
                const MdkrModernCharacterEntry *entry =
                    mdkr_modern_character_registry_entry(&g_characterRegistry,
                                                         index);
                if (entry == nullptr) continue;
                const bool        qualified = mdkr_modern_donor_qualified(
                                                  static_cast<int>(entry->donor)) != 0;
                const std::string item      = std::string(entry->display_name) +
                                              (entry->enabled != 0u ? "" : " (disabled)") +
                                              (qualified ? "" : " (review only)");
                if (ImGui::Selectable(
                        item.c_str(),
                        g_characterWorkshopSelection == entry->id)) {
                    selectCharacterWorkshopEntry(entry);
                    workshopEntry = entry;
                }
                const std::string spokenState =
                    std::string(entry->enabled != 0u ? "enabled" : "disabled") +
                    (qualified ? "" : ", donor review required");
                ui::SpeakFocusedItem(
                    entry->narration_name,
                    spokenState.c_str(),
                    "Select this package for Workshop editing.");
            }
            ImGui::EndCombo();
        }
        return workshopEntry;
    }

    for (int index = 0; index < characterCount; ++index) {
        const MdkrModernCharacterEntry *entry =
            mdkr_modern_character_registry_entry(&g_characterRegistry, index);
        if (entry == nullptr) continue;
        const CharacterWorkshopReadiness readiness =
            characterWorkshopReadinessForEntry(entry);
        const bool        selected = g_characterWorkshopSelection == entry->id;
        const std::string label    = std::string(entry->display_name) + "\n" +
                                     std::to_string(readiness.readyCount) + "/" +
                                     std::to_string(readiness.rows.size()) + " ready · " +
                                     (entry->enabled != 0u ? "Enabled" : "Disabled") +
                                     "##character-library-" + entry->id;
        if (ImGui::Selectable(
                label.c_str(),
                selected,
                0,
                ImVec2(0.0f, ui::kTouchRowHeight() * 1.35f))) {
            selectCharacterWorkshopEntry(entry);
            workshopEntry = entry;
        }
        const std::string spokenState =
            std::to_string(readiness.readyCount) + " of " +
            std::to_string(readiness.rows.size()) + " areas ready, " +
            (entry->enabled != 0u ? "enabled" : "disabled");
        ui::SpeakFocusedItem(
            entry->narration_name,
            spokenState.c_str(),
            readiness.nextActionLabel);
    }
    return workshopEntry;
}

bool drawCharacterAssignments() {
    bool      changed = false;
    const int characterCount =
        mdkr_modern_character_registry_count(&g_characterRegistry);
    ImGui::SeparatorText("Use in game");
    ui::TextSubtleWrapped(
        "Assignments are separate from editing. A character's saved fit follows the package, regardless of which local player uses it.");
    for (int player = 0; player < 4; ++player) {
        ImGui::PushID(player);
        const std::string key =
            "custom_character_p" + std::to_string(player + 1);
        const std::string selected      = AppConfig::get(key);
        const int         selectedIndex = mdkr_modern_character_registry_find(
            &g_characterRegistry,
            selected.c_str());
        const MdkrModernCharacterEntry *selectedEntry =
            mdkr_modern_character_registry_entry(&g_characterRegistry,
                                                 selectedIndex);
        const bool selectedAvailable =
            selectedEntry != nullptr && selectedEntry->enabled != 0u;
        const std::string preview = selectedAvailable
                                        ? selectedEntry->display_name
                                    : selectedEntry != nullptr
                                        ? std::string("Built-in racer — ") +
                                              selectedEntry->display_name + " is disabled"
                                        : "Built-in racer";
        const std::string label =
            "Player " + std::to_string(player + 1) + "##custom-character";
        const std::string spokenLabel =
            "Player " + std::to_string(player + 1) + " character";
        const bool assignmentComboOpen = ImGui::BeginCombo(
            label.c_str(),
            preview.c_str());
        ui::SpeakFocusedItem(
            spokenLabel.c_str(),
            selectedAvailable ? selectedEntry->narration_name
                              : "Built-in racer",
            "Choose a local presentation for this player. Gameplay remains owned by its built-in donor.");
        if (assignmentComboOpen) {
            const bool noneSelected =
                selected.empty() || !selectedAvailable;
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
            ui::SpeakFocusedItem(
                "Built-in racer",
                noneSelected ? "selected" : "available",
                "Use the original built-in character presentation.");
            for (int index = 0; index < characterCount; ++index) {
                const MdkrModernCharacterEntry *entry =
                    mdkr_modern_character_registry_entry(&g_characterRegistry,
                                                         index);
                if (entry == nullptr) continue;
                const bool        qualified  = mdkr_modern_donor_qualified(
                                                   static_cast<int>(entry->donor)) != 0;
                const std::string item       = std::string(entry->display_name) +
                                               (entry->enabled != 0u ? "" : " (disabled)") +
                                               (qualified ? "" : " (donor not qualified)");
                const bool        assignable = qualified && entry->enabled != 0u;
                if (!assignable) ImGui::BeginDisabled();
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
                const std::string spokenState =
                    std::string(assignable ? "available" : "not assignable") +
                    (selected == entry->id ? ", selected" : "");
                ui::SpeakFocusedItem(
                    entry->narration_name,
                    spokenState.c_str(),
                    "Assign this local character presentation to the player.");
                if (!assignable) ImGui::EndDisabled();
            }
            ImGui::EndCombo();
        }
        if (selectedEntry != nullptr && selectedEntry->enabled == 0u) {
            ImGui::TextDisabled(
                "Assignment retained; the built-in racer is used until %s is enabled.",
                selectedEntry->display_name);
        }
        ImGui::PopID();
    }
    return changed;
}

void drawSkippedCharacterInventory() {
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
}

bool drawCustomCharactersSection(bool compact) {
    bool changed = false;
    if (!g_characterRegistryLoaded) refreshCharacterRegistry();
    ui::TextSubtleWrapped(
        "Appearance packages are local presentation only. The selected fingerprint-qualified built-in donor still owns simulation, collision, audio, ghosts, records, and network/rollback identity; no second ROM is required.");
    if (mdkr_render_backend() != MDKR_BACKEND_WEBGPU) {
        ImGui::PushStyleColor(ImGuiCol_Text, AppTheme::accent());
        ImGui::TextWrapped(
            "Modern characters require the WebGPU renderer. This backend keeps the built-in racer visible, so installed packages remain safe but cannot appear in game.");
        ImGui::PopStyleColor();
    }

    const bool rail = !compact &&
                      ImGui::GetContentRegionAvail().x >=
                          980.0f * AppTheme::uiScale();
    if (!rail) {
        ImGui::Indent(ui::kGapM);
        drawCharacterImportControls(false);
        bool editorRouteRendered = false;
        if (g_characterImportCandidate.ready) {
            editorRouteRendered = true;
            changed |= drawCharacterCandidateReview(compact);
        } else if (g_characterRawEditorOpen &&
                   g_characterRawIntake.modelPath[0] != '\0') {
            editorRouteRendered = true;
            drawCharacterRawIntakeEditor(false);
        }
        const MdkrModernCharacterEntry *entry =
            drawCharacterLibrary(false);
        if (!editorRouteRendered && !g_characterImportCandidate.ready &&
            !g_characterRawEditorOpen && entry != nullptr) {
            changed |= drawCharacterPackageInspector(entry, compact);
        }
        changed |= drawCharacterAssignments();
        drawSkippedCharacterInventory();
        ImGui::Unindent(ui::kGapM);
        ui::Gap(ui::kGapS);
        return changed;
    }

    if (ImGui::BeginTable(
            "##character-workshop-shell",
            2,
            ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable |
                ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn(
            "Library",
            ImGuiTableColumnFlags_WidthFixed,
            320.0f * AppTheme::uiScale());
        ImGui::TableSetupColumn("Editor", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::BeginChild(
            "##character-workshop-library-rail",
            ImVec2(0.0f, 0.0f),
            ImGuiChildFlags_NavFlattened,
            ImGuiWindowFlags_AlwaysVerticalScrollbar);
        ImGui::PushFont(AppTheme::fonts().section);
        ImGui::TextUnformatted("Library");
        ImGui::PopFont();
        ui::TextSubtleWrapped(
            "Import or choose a character. Drafts and installed revisions remain independent.");
        drawCharacterImportControls(true);
        const MdkrModernCharacterEntry *entry = drawCharacterLibrary(true);
        changed |= drawCharacterAssignments();
        drawSkippedCharacterInventory();
        ui::TouchScrollCurrentWindow();
        ImGui::EndChild();

        ImGui::TableNextColumn();
        ImGui::BeginChild(
            "##character-workshop-editor",
            ImVec2(0.0f, 0.0f),
            ImGuiChildFlags_NavFlattened,
            ImGuiWindowFlags_AlwaysVerticalScrollbar);
        if (g_characterImportCandidate.ready) {
            changed |= drawCharacterCandidateReview(false);
        } else if (g_characterRawEditorOpen &&
                   g_characterRawIntake.modelPath[0] != '\0') {
            drawCharacterRawIntakeEditor(false);
        } else if (entry != nullptr) {
            changed |= drawCharacterPackageInspector(entry, false);
        } else {
            ImGui::PushFont(AppTheme::fonts().section);
            ImGui::TextUnformatted("Start with a reviewed package");
            ImGui::PopFont();
            ui::TextSubtleWrapped(
                "Choose a self-contained .mdkrchar file in the library. Validation is mutation-free; the complete inventory and any installed-version differences appear here before an explicit install.");
        }
        ui::TouchScrollCurrentWindow();
        ImGui::EndChild();
        ImGui::EndTable();
    }
    return changed;
}

bool drawCustomCharactersSettingsSummary(bool compact) {
    if (!g_characterRegistryLoaded) refreshCharacterRegistry();
    const int characterCount =
        mdkr_modern_character_registry_count(&g_characterRegistry);
    int enabledCount = 0;
    for (int index = 0; index < characterCount; ++index) {
        const MdkrModernCharacterEntry *entry =
            mdkr_modern_character_registry_entry(&g_characterRegistry, index);
        if (entry != nullptr && entry->enabled != 0u) ++enabledCount;
    }
    ImGui::TextWrapped("%d installed · %d enabled · %d disabled",
                       characterCount,
                       enabledCount,
                       characterCount - enabledCount);
    ui::TextSubtleWrapped(
        "Importing, identity, portraits, rigging, vehicle fit, performance, testing, and packaging live in the dedicated Character Workshop.");

    ImGui::TextUnformatted("Current player assignments");
    for (int player = 0; player < 4; ++player) {
        const std::string key =
            "custom_character_p" + std::to_string(player + 1);
        const std::string selected      = AppConfig::get(key);
        const int         selectedIndex = mdkr_modern_character_registry_find(
            &g_characterRegistry,
            selected.c_str());
        const MdkrModernCharacterEntry *entry =
            mdkr_modern_character_registry_entry(&g_characterRegistry,
                                                 selectedIndex);
        const bool active = entry != nullptr && entry->enabled != 0u &&
                            mdkr_modern_donor_qualified(static_cast<int>(entry->donor)) != 0;
        if (active) {
            ImGui::BulletText("Player %d: %s", player + 1, entry->display_name);
        } else if (entry != nullptr) {
            ImGui::BulletText("Player %d: Built-in racer (%s retained)",
                              player + 1,
                              entry->display_name);
        } else {
            ImGui::BulletText("Player %d: Built-in racer", player + 1);
        }
    }

    if (!compact) {
        if (ImGui::Button("Open Character Workshop",
                          ui::kBtnFullWidth())) {
            g_characterWorkshopOpenRequested = true;
        }
        ui::SpeakFocusedItem(
            "Open Character Workshop",
            nullptr,
            "Opens the dedicated character library and authoring workspaces. Current settings and drafts are preserved.");
    } else {
        ui::TextSubtleWrapped(
            "Return to the launcher to open Character Workshop. The in-game overlay never starts a second renderer or changes character packages during a running session.");
    }
    return false;
}

} // namespace

void Settings_setDonorGameplayProfiles(
    const MdkrDonorGameplayProfiles *profiles,
    const char                      *unavailableReason) {
    std::memset(&g_donorGameplayProfiles, 0, sizeof(g_donorGameplayProfiles));
    g_donorGameplayProfilesUnavailableReason =
        unavailableReason != nullptr ? unavailableReason : "";
    if (profiles == nullptr || profiles->available != 1u ||
        profiles->version != MDKR_DONOR_GAMEPLAY_PROFILE_VERSION ||
        profiles->donor_count != MDKR_DONOR_GAMEPLAY_PROFILE_COUNT) {
        return;
    }
    for (uint32_t donor = 0u;
         donor < MDKR_DONOR_GAMEPLAY_PROFILE_COUNT;
         ++donor) {
        if (!std::isfinite(profiles->donor[donor].weight) ||
            !std::isfinite(profiles->donor[donor].handling)) {
            return;
        }
        for (uint32_t sample = 0u;
             sample < MDKR_DONOR_ACCELERATION_SAMPLES; ++sample) {
            for (uint32_t vehicle = 0u;
                 vehicle < MDKR_DONOR_VEHICLE_COUNT; ++vehicle) {
                if (!std::isfinite(profiles->donor[donor]
                                       .acceleration[vehicle][sample])) {
                    return;
                }
            }
        }
    }
    g_donorGameplayProfiles = *profiles;
    g_donorGameplayProfilesUnavailableReason.clear();
}

bool Settings_importCharacterPackage(const char *path) {
    if (path == nullptr || path[0] == '\0') {
        g_characterManagerReport =
            "Choose a .mdkrchar package, self-contained .glb, COLLADA .dae, authoring .zip, or recognized DCC source first.";
        setStatus("Character import needs a source path.", AppTheme::bad());
        return false;
    }
    if (std::strlen(path) >= sizeof(g_characterImportPath)) {
        g_characterManagerReport =
            "The character source path exceeds the launcher's bounded path profile.";
        setStatus("Character source path is too long.", AppTheme::bad());
        return false;
    }
    std::snprintf(g_characterImportPath, sizeof(g_characterImportPath), "%s",
                  path);
    const CharacterSourceKind sourceKind = characterSourceKind(path);
    if (characterSourceNeedsDccExport(sourceKind)) {
        g_characterManagerReport = characterSourceExportGuidance(sourceKind);
        setStatus(
            (std::string(characterSourceFormatName(sourceKind)) +
             " recognized. Export a self-contained GLB; no source or draft changed.").c_str(),
            AppTheme::accent());
        if (std::getenv("MDKR_APP_UI_TRACE") != nullptr) {
            std::fprintf(
                stderr,
                "[app-ui] character-source-guidance kind=%s direct_import=0 mutated=0\n",
                characterSourceFormatName(sourceKind));
        }
        return false;
    }
    const bool convertibleSource = sourceKind == CharacterSourceKind::Dae ||
        sourceKind == CharacterSourceKind::Zip;
    if (convertibleSource) {
        const char *smokeOutput = std::getenv(
            "MDKR_APP_SMOKE_CHARACTER_CONVERSION_OUTPUT");
        const char *smokeToken = std::getenv(
            "MDKR_APP_SMOKE_CHARACTER_CONVERSION_TOKEN");
        if (g_characterConversionOutputPath[0] == '\0' &&
            smokeOutput != nullptr && smokeOutput[0] != '\0' &&
            smokeToken != nullptr &&
            std::strcmp(
                smokeToken, "mdkr64-character-conversion-v1") == 0) {
            std::snprintf(
                g_characterConversionOutputPath,
                sizeof(g_characterConversionOutputPath), "%s",
                smokeOutput);
        }
        if (g_characterConversionOutputPath[0] == '\0') {
            g_characterManagerReport =
                "Choose a new .glb destination for the converted authoring source.";
            setStatus(
                "DAE/ZIP conversion needs an explicit new GLB destination.",
                AppTheme::accent());
            return false;
        }
        const std::string convertedPath = g_characterConversionOutputPath;
        if (!runCharacterManager(
                "convert-authoring-source", {path, convertedPath}, false)) {
            setStatus(
                "Character source conversion did not complete cleanly and no draft changed. The exclusive destination may exist only if diagnostic persistence failed; inspect the report and path before retrying.",
                AppTheme::bad());
            return false;
        }
        const std::string conversionReport = g_characterManagerReport;
        const bool archiveMissingLicense =
            sourceKind == CharacterSourceKind::Zip &&
            conversionReport.find(
                "\"archive_license_present\": false") !=
                std::string::npos;
        std::snprintf(g_characterImportPath,
                      sizeof(g_characterImportPath), "%s",
                      convertedPath.c_str());
        g_characterConversionOutputPath[0] = '\0';
        if (!beginCharacterRawDraft(convertedPath)) {
            g_characterManagerReport = conversionReport +
                "\n\nThe converted GLB was created, but its raw authoring draft could not be opened: " +
                g_characterRawDraftError;
            setStatus(
                "GLB conversion succeeded, but the authoring draft could not open; the new GLB remains available.",
                AppTheme::bad());
            return false;
        }
        const bool inspected = inspectCharacterRawGlb(convertedPath);
        const std::string inspectionReport = g_characterManagerReport;
        g_characterManagerReport = conversionReport + "\n\n" +
            inspectionReport;
        if (std::getenv("MDKR_APP_UI_TRACE") != nullptr) {
            std::fprintf(
                stderr,
                "[app-ui] character-source-conversion kind=%s converted=1 inspected=%d missing_license=%d output=%s\n",
                sourceKind == CharacterSourceKind::Zip ? "zip" : "dae",
                inspected ? 1 : 0, archiveMissingLicense ? 1 : 0,
                convertedPath.c_str());
        }
        setStatus(
            inspected
                ? archiveMissingLicense
                    ? "Archive converted and inspected. Choose the exact license/notice before building; none was embedded in the archive."
                    : "Character source converted and inspected; complete the resumable authoring draft."
                : "The new GLB was created but failed character inspection; review the importer report.",
            inspected ? (archiveMissingLicense ? AppTheme::accent()
                                               : AppTheme::good())
                      : AppTheme::bad());
        return inspected;
    }
    if (sourceKind == CharacterSourceKind::Glb) {
        if (!beginCharacterRawDraft(path)) {
            g_characterManagerReport =
                "The raw authoring draft could not be created or selected: " +
                g_characterRawDraftError;
            setStatus(
                "GLB authoring could not start; no existing draft or installed character changed.",
                AppTheme::bad());
            return false;
        }
        const bool inspected = inspectCharacterRawGlb(path);
        setStatus(
            inspected
                ? "GLB inspected; complete and review the resumable authoring draft."
                : "GLB inspection failed; no package was built or installed.",
            inspected ? AppTheme::good() : AppTheme::bad());
        return inspected;
    }
    if (!stageCharacterPackage(path)) {
        g_characterImportCandidate = CharacterImportCandidate{};
        setStatus("Character validation failed; open the importer report below.",
                  AppTheme::bad());
        return false;
    }
    setStatus(
        g_characterImportCandidate.installed
            ? "Character update validated; review every change before installing."
            : "Character validated; review its identity, gameplay donor, rig, and performance before installing.",
        AppTheme::good());
    return true;
}

bool Settings_drawCharacterWorkshop(SDL_Window *window, bool compact) {
    (void)window;
    const bool changed = drawCustomCharactersSection(compact);
    if (!g_status.empty()) {
        ui::Gap(ui::kGapS);
        ImGui::PushStyleColor(ImGuiCol_Text, g_statusColor);
        ImGui::TextWrapped("%s", g_status.c_str());
        ImGui::PopStyleColor();
    }
    return changed;
}

bool Settings_takeCharacterWorkshopOpenRequest() {
    if (!g_characterWorkshopOpenRequested) return false;
    g_characterWorkshopOpenRequested = false;
    return true;
}

bool Settings_takeCharacterPreviewRequest(
    SettingsCharacterPreviewRequest &request) {
    if (!g_characterPreviewRequested) return false;
    request                     = std::move(g_characterPreviewRequest);
    g_characterPreviewRequest   = SettingsCharacterPreviewRequest{};
    g_characterPreviewRequested = false;
    return true;
}

void Settings_publishCharacterPreviewResult(
    const std::string &packageId,
    const std::string &sourceSha256,
    const std::string &fitSha256,
    const std::string &presentationSha256,
    const std::string &capturePng,
    const MdkrCharacterPreviewResult &result) {
    if (packageId.empty()) return;
    g_characterPreviewResults[packageId] = CharacterPreviewSessionResult{
        result, sourceSha256, fitSha256, presentationSha256, capturePng,
    };
    if (result.pose != MDKR_CHARACTER_PREVIEW_POSE_LIVE) {
        const CharacterInspectionPose *pose =
            characterInspectionPose(result.pose);
        const CharacterInspectionLighting *lighting =
            characterInspectionLighting(result.lighting);
        if (result.capture_written) {
            auto &captures = g_characterVisualCaptures[packageId];
            const bool recordValid =
                result.version == MDKR_CHARACTER_PREVIEW_RESULT_VERSION &&
                result.started && result.warmup_complete &&
                characterPreviewFitDiagnosticsValid(result) &&
                result.capture_requested && result.capture_armed &&
                result.capture_stable_frames >=
                    MDKR_CHARACTER_PREVIEW_CAPTURE_STABLE_FRAMES &&
                result.capture_png_bytes != 0u &&
                !capturePng.empty() && pose != nullptr && lighting != nullptr &&
                result.context >= MDKR_CHARACTER_PREVIEW_SELECT &&
                result.context <= MDKR_CHARACTER_PREVIEW_PLANE &&
                result.players >= 1 && result.players <= 4 &&
                result.pose_phase_milli <= 1000u &&
                result.inspection_pose_ticks != 0u &&
                result.inspection_pose_fallback_ticks <=
                    result.inspection_pose_ticks &&
                result.view_yaw_degrees >= -180 &&
                result.view_yaw_degrees <= 180 &&
                result.view_pitch_degrees >= -45 &&
                result.view_pitch_degrees <= 45 &&
                (result.context != MDKR_CHARACTER_PREVIEW_SELECT ||
                 (result.view_yaw_degrees == 0 &&
                  result.view_pitch_degrees == 0 &&
                  result.camera_override_ticks == 0u)) &&
                ((result.view_yaw_degrees == 0 &&
                  result.view_pitch_degrees == 0) ||
                 result.camera_override_ticks != 0u) &&
                result.lighting >=
                    MDKR_WORKSHOP_PREVIEW_LIGHTING_NEUTRAL &&
                result.lighting < MDKR_WORKSHOP_PREVIEW_LIGHTING_COUNT &&
                (result.lighting ==
                     MDKR_WORKSHOP_PREVIEW_LIGHTING_NEUTRAL
                     ? result.lighting_override_draws == 0u
                     : result.lighting_override_draws != 0u) &&
                result.output_width >= 1u &&
                result.output_width <= 16384u &&
                result.output_height >= 1u &&
                result.output_height <= 16384u &&
                characterDigestTextValid(sourceSha256) &&
                characterDigestTextValid(fitSha256);
            const bool alreadyListed = std::any_of(
                captures.begin(), captures.end(),
                [&capturePng](const CharacterVisualReport::Capture &capture) {
                    return capture.pngPath == capturePng;
                });
            if (recordValid && !alreadyListed &&
                captures.size() < CharacterVisualReport::kMaximumCaptures) {
                CharacterVisualReport::Capture capture{};
                capture.pngPath = capturePng;
                capture.sourceSha256 = sourceSha256;
                capture.fitSha256 = fitSha256;
                capture.context = characterPreviewResultContext(result.context);
                capture.pose = pose->label;
                capture.lighting = lighting->label;
                capture.players = static_cast<uint32_t>(result.players);
                capture.phaseMilli = result.pose_phase_milli;
                capture.viewYawDegrees = result.view_yaw_degrees;
                capture.viewPitchDegrees = result.view_pitch_degrees;
                capture.width = result.output_width;
                capture.height = result.output_height;
                capture.stableFrames = result.capture_stable_frames;
                capture.exactPose = result.inspection_pose_ticks != 0u &&
                    result.inspection_pose_fallback_ticks == 0u;
                captures.push_back(std::move(capture));
                setStatus(
                    "Inspection PNG saved and added to the visual report tray.",
                    AppTheme::good());
            } else if (!alreadyListed) {
                setStatus(
                    recordValid
                        ? "The PNG was saved, but the session report reached its safety capacity; export or clear the tray before capturing more."
                        : "The PNG was saved, but inconsistent inspection metadata prevented adding it to the report tray.",
                    AppTheme::accent());
            }
        }
        if (std::getenv("MDKR_APP_UI_TRACE") != nullptr) {
            std::fprintf(
                stderr,
                "[app-ui] character-pose-inspection session-only=1 package=%s context=%u players=%d pose=%d phase=%u capture=%d tray=%zu\n",
                packageId.c_str(), static_cast<unsigned>(result.context),
                result.players, static_cast<int>(result.pose),
                result.pose_phase_milli, result.capture_written,
                g_characterVisualCaptures[packageId].size());
        }
        return;
    }
    if (result.pose_phase_milli != 0u ||
        result.inspection_pose_ticks != 0u ||
        result.inspection_pose_fallback_ticks != 0u ||
        result.view_yaw_degrees != 0 ||
        result.view_pitch_degrees != 0 ||
        result.lighting != MDKR_WORKSHOP_PREVIEW_LIGHTING_NEUTRAL ||
        result.camera_override_ticks != 0u ||
        result.lighting_override_draws != 0u ||
        result.capture_requested || result.capture_armed ||
        result.capture_stable_frames != 0u || result.capture_written ||
        result.capture_png_bytes != 0u || !capturePng.empty()) {
        if (std::getenv("MDKR_APP_UI_TRACE") != nullptr) {
            std::fprintf(
                stderr,
                "[app-ui] character-preview-result rejected-evidence=mixed-mode package=%s phase=%u poseTicks=%llu poseFallback=%llu\n",
                packageId.c_str(), result.pose_phase_milli,
                result.inspection_pose_ticks,
                result.inspection_pose_fallback_ticks);
        }
        setStatus(
            "The engine returned mixed live-test and pose-inspection fields; the session is visible for diagnosis but no durable performance evidence was saved.",
            AppTheme::bad());
        return;
    }
    if (result.version != MDKR_CHARACTER_PREVIEW_RESULT_VERSION ||
        !result.started || !characterPreviewFitDiagnosticsValid(result)) {
        if (std::getenv("MDKR_APP_UI_TRACE") != nullptr) {
            std::fprintf(
                stderr,
                "[app-ui] character-preview-result rejected-evidence=fit-contract package=%s version=%u started=%d fit=%d\n",
                packageId.c_str(), result.version, result.started,
                result.fit_diagnostics_valid);
        }
        setStatus(
            "The engine returned an invalid renderer fit contract; the session is visible for diagnosis but no durable performance evidence was saved.",
            AppTheme::bad());
        return;
    }
    if (result.context >= MDKR_CHARACTER_PREVIEW_SELECT &&
        result.context <= MDKR_CHARACTER_PREVIEW_PLANE &&
        result.players >= 1 && result.players <= 4) {
        g_characterTestEvidenceSelectedCell[packageId] =
            (static_cast<unsigned>(result.context) - 1u) * 4u +
            static_cast<unsigned>(result.players - 1);
    }
    loadCharacterTestEvidence();
    if (!g_characterTestEvidenceWritable) return;
    CharacterTestEvidenceStore::Inventory replacement =
        g_characterTestEvidence;
    CharacterTestEvidenceStore::Evidence evidence =
        characterTestEvidenceFromResult(
            packageId, sourceSha256, fitSha256, presentationSha256, result);
    std::string error;
    if (!CharacterTestEvidenceStore::upsert(
            replacement, evidence, error)) {
        g_characterTestEvidenceError = error;
        setStatus(
            ("The exact test returned, but its evidence was invalid and was not saved: " +
             error).c_str(),
            AppTheme::bad());
        return;
    }
    if (!replaceCharacterTestEvidence(std::move(replacement))) {
        setStatus(
            ("The exact test returned for this session, but its evidence could not be saved: " +
             g_characterTestEvidenceError).c_str(),
            AppTheme::bad());
        return;
    }
    if (std::getenv("MDKR_APP_UI_TRACE") != nullptr) {
        std::fprintf(
            stderr,
            "[app-ui] character-test-evidence saved=1 package=%s context=%u players=%u qualified=%d records=%zu\n",
            packageId.c_str(), evidence.context, evidence.players,
            CharacterTestEvidenceStore::qualified(evidence) ? 1 : 0,
            g_characterTestEvidence.records.size());
    }
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
        changed |= drawCustomCharactersSettingsSummary(compact);
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
