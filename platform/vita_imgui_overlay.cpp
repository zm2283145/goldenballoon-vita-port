#include "vita_imgui_overlay.h"
#include "vita_save_editor_bridge.h"
#include "vita_trophy.h"
#include "video_config.h"

#if defined(__vita__) && defined(MDKR_VITA_IMGUI_OVERLAY)
#include <cstdio>
#include <cstring>
#include <psp2/ctrl.h>
#include <psp2/kernel/processmgr.h>
#include <vitaGL.h>
#include "imgui.h"
#include "imgui_impl_vitagl.h"

namespace {
extern "C" void mdkr_vita_boot_log(const char *msg);
extern "C" void mdkr_vita_boot_log_flush(void);
extern "C" void mdkr_vita_restore_primary_vao(void);

bool s_initialized = false;
bool s_open = false;
bool s_initialization_failed = false;
bool s_controls_mode = false;
int s_page = 0;
int s_world = 0;
int s_time_trial = 0;
int s_trophy_group = 0;
int s_main_trophy = 0;
int s_adventure_two_trophy = 0;
int s_character_trophy = 0;
int s_powerup_trophy = 0;
bool s_erase_armed = false;
bool s_hundred_percent_armed = false;
bool s_apply_armed = false;
bool s_create_adventure_two = false;
int s_name_cursor = 0;
char s_name[4] = "NEW";
GLuint s_program = 0;
GLint s_texture_uniform = -1;
int s_capture_action = -1;
bool s_capture_wait_release = false;
uint64_t s_capture_deadline = 0;
char s_controls_status[128] = "Select an action to change its Vita button.";

enum VitaIconKind {
    VITA_ICON_CROSS,
    VITA_ICON_CIRCLE,
    VITA_ICON_SQUARE,
    VITA_ICON_TRIANGLE,
    VITA_ICON_STICK,
    VITA_ICON_TEXT
};

struct VitaControlSource {
    const char *label;
    const char *short_label;
    MdkrVideoKey key;
    unsigned buttons;
    int axis;
    VitaIconKind icon;
    ImU32 colour;
};

struct GameControlAction {
    const char *label;
    const char *value;
};

const VitaControlSource k_vita_sources[] = {
    {"Cross", "X", MDKR_INPUT_CONTROLLER_A, SCE_CTRL_CROSS, 0, VITA_ICON_CROSS, IM_COL32(80, 160, 255, 255)},
    {"Circle", "O", MDKR_INPUT_CONTROLLER_B, SCE_CTRL_CIRCLE, 0, VITA_ICON_CIRCLE, IM_COL32(255, 100, 120, 255)},
    {"Square", "SQ", MDKR_INPUT_CONTROLLER_X, SCE_CTRL_SQUARE, 0, VITA_ICON_SQUARE, IM_COL32(255, 120, 210, 255)},
    {"Triangle", "TR", MDKR_INPUT_CONTROLLER_Y, SCE_CTRL_TRIANGLE, 0, VITA_ICON_TRIANGLE, IM_COL32(100, 230, 170, 255)},
    {"Start", "START", MDKR_INPUT_CONTROLLER_START, SCE_CTRL_START, 0, VITA_ICON_TEXT, IM_COL32(230, 230, 230, 255)},
    {"L", "L", MDKR_INPUT_CONTROLLER_LEFT_SHOULDER, SCE_CTRL_LTRIGGER, 0, VITA_ICON_TEXT, IM_COL32(230, 230, 230, 255)},
    {"R", "R", MDKR_INPUT_CONTROLLER_RIGHT_SHOULDER, SCE_CTRL_RTRIGGER, 0, VITA_ICON_TEXT, IM_COL32(230, 230, 230, 255)},
    {"D-pad Up", "D-UP", MDKR_INPUT_CONTROLLER_DPAD_UP, SCE_CTRL_UP, 0, VITA_ICON_TEXT, IM_COL32(210, 210, 210, 255)},
    {"D-pad Down", "D-DN", MDKR_INPUT_CONTROLLER_DPAD_DOWN, SCE_CTRL_DOWN, 0, VITA_ICON_TEXT, IM_COL32(210, 210, 210, 255)},
    {"D-pad Left", "D-LT", MDKR_INPUT_CONTROLLER_DPAD_LEFT, SCE_CTRL_LEFT, 0, VITA_ICON_TEXT, IM_COL32(210, 210, 210, 255)},
    {"D-pad Right", "D-RT", MDKR_INPUT_CONTROLLER_DPAD_RIGHT, SCE_CTRL_RIGHT, 0, VITA_ICON_TEXT, IM_COL32(210, 210, 210, 255)},
    {"Right Stick Up", "RS", MDKR_INPUT_CONTROLLER_RIGHT_STICK_UP, 0, 1, VITA_ICON_STICK, IM_COL32(245, 205, 90, 255)},
    {"Right Stick Down", "RS", MDKR_INPUT_CONTROLLER_RIGHT_STICK_DOWN, 0, 2, VITA_ICON_STICK, IM_COL32(245, 205, 90, 255)},
    {"Right Stick Left", "RS", MDKR_INPUT_CONTROLLER_RIGHT_STICK_LEFT, 0, 3, VITA_ICON_STICK, IM_COL32(245, 205, 90, 255)},
    {"Right Stick Right", "RS", MDKR_INPUT_CONTROLLER_RIGHT_STICK_RIGHT, 0, 4, VITA_ICON_STICK, IM_COL32(245, 205, 90, 255)},
};

const VitaControlSource k_left_stick_directions[] = {
    {"Left Stick Up", "LS", MDKR_VIDEO_KEY_COUNT, 0, 1, VITA_ICON_STICK, IM_COL32(130, 220, 255, 255)},
    {"Left Stick Down", "LS", MDKR_VIDEO_KEY_COUNT, 0, 2, VITA_ICON_STICK, IM_COL32(130, 220, 255, 255)},
    {"Left Stick Left", "LS", MDKR_VIDEO_KEY_COUNT, 0, 3, VITA_ICON_STICK, IM_COL32(130, 220, 255, 255)},
    {"Left Stick Right", "LS", MDKR_VIDEO_KEY_COUNT, 0, 4, VITA_ICON_STICK, IM_COL32(130, 220, 255, 255)},
};

const GameControlAction k_game_actions[] = {
    {"Gas / Accept", "a"},
    {"Brake / Back", "b"},
    {"Horn / Use Item", "z"},
    {"Pause / Start", "start"},
    {"Camera Modifier", "l"},
    {"Drift / Powerslide", "r"},
    {"Up", "dpad_up"},
    {"Down", "dpad_down"},
    {"Left", "dpad_left"},
    {"Right", "dpad_right"},
    {"Camera Up", "c_up"},
    {"Camera Down", "c_down"},
    {"Camera Left", "c_left"},
    {"Camera Right", "c_right"},
};

const int k_vita_source_count =
    (int)(sizeof(k_vita_sources) / sizeof(k_vita_sources[0]));
const int k_game_action_count =
    (int)(sizeof(k_game_actions) / sizeof(k_game_actions[0]));

const char *k_vertex_shader =
    "#version 100\n"
    "precision mediump float;\n"
    "attribute vec3 aPosition;\n"
    "attribute vec2 aTexCoord;\n"
    "attribute vec4 aColor;\n"
    "varying vec2 vTexCoord;\n"
    "varying vec4 vColor;\n"
    "void main() {\n"
    "  gl_Position = vec4(aPosition.x / 480.0 - 1.0, 1.0 - aPosition.y / 272.0, 0.0, 1.0);\n"
    "  vTexCoord = aTexCoord;\n"
    "  vColor = aColor;\n"
    "}\n";

const char *k_fragment_shader =
    "#version 100\n"
    "precision mediump float;\n"
    "varying vec2 vTexCoord;\n"
    "varying vec4 vColor;\n"
    "uniform sampler2D uTexture;\n"
    "void main() { gl_FragColor = vColor * texture2D(uTexture, vTexCoord); }\n";

GLuint compile_shader(GLenum type, const char *source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint compiled = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (compiled == GL_TRUE) return shader;
    char message[1024] = {};
    GLsizei length = 0;
    glGetShaderInfoLog(shader, sizeof(message) - 1, &length, message);
    char log_line[1200];
    snprintf(log_line, sizeof(log_line), "imgui: %s shader compile failed: %s",
             type == GL_VERTEX_SHADER ? "vertex" : "fragment", message);
    mdkr_vita_boot_log(log_line);
    mdkr_vita_boot_log_flush();
    glDeleteShader(shader);
    return 0;
}

bool create_renderer_program() {
    const GLuint vertex = compile_shader(GL_VERTEX_SHADER, k_vertex_shader);
    const GLuint fragment = compile_shader(GL_FRAGMENT_SHADER, k_fragment_shader);
    if (!vertex || !fragment) {
        if (vertex) glDeleteShader(vertex);
        if (fragment) glDeleteShader(fragment);
        return false;
    }
    s_program = glCreateProgram();
    glAttachShader(s_program, vertex);
    glAttachShader(s_program, fragment);
    glBindAttribLocation(s_program, 0, "aPosition");
    glBindAttribLocation(s_program, 1, "aTexCoord");
    glBindAttribLocation(s_program, 2, "aColor");
    glLinkProgram(s_program);
    glDeleteShader(vertex);
    glDeleteShader(fragment);
    GLint linked = GL_FALSE;
    glGetProgramiv(s_program, GL_LINK_STATUS, &linked);
    if (linked != GL_TRUE) {
        char message[1024] = {};
        GLsizei length = 0;
        glGetProgramInfoLog(s_program, sizeof(message) - 1, &length, message);
        char log_line[1200];
        snprintf(log_line, sizeof(log_line), "imgui: program link failed: %s", message);
        mdkr_vita_boot_log(log_line);
        mdkr_vita_boot_log_flush();
        glDeleteProgram(s_program);
        s_program = 0;
        return false;
    }
    s_texture_uniform = glGetUniformLocation(s_program, "uTexture");
    char log_line[160];
    snprintf(log_line, sizeof(log_line), "imgui: program ready id=%u textureUniform=%d glError=0x%x",
             (unsigned)s_program, (int)s_texture_uniform, (unsigned)glGetError());
    mdkr_vita_boot_log(log_line);
    mdkr_vita_boot_log_flush();
    return true;
}

bool initialize() {
    if (s_initialized) return true;
    if (s_initialization_failed) return false;
    mdkr_vita_boot_log("imgui: initialization starting");
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    if (!ImGui_ImplVitaGL_Init_Extended()) {
        mdkr_vita_boot_log("imgui: VitaGL backend initialization failed");
        mdkr_vita_boot_log_flush();
        ImGui::DestroyContext();
        s_initialization_failed = true;
        return false;
    }
    ImGui_ImplVitaGL_GamepadUsage(true);
    ImGui_ImplVitaGL_TouchUsage(true);
    /* The bundled Vita backend supplies Vita-native input and vertex storage,
     * but its default renderer is fixed pipeline. Golden Balloon is shader
     * based, so render those vertices with our own tiny GLES 2 program. */
    ImGui_ImplVitaGL_UseCustomShader(true);
    if (!create_renderer_program()) {
        ImGui_ImplVitaGL_Shutdown();
        ImGui::DestroyContext();
        s_initialization_failed = true;
        return false;
    }
    s_initialized = true;
    mdkr_vita_boot_log("imgui: initialization complete");
    mdkr_vita_boot_log_flush();
    return true;
}

void sync_slot_name() {
    const char *name = mdkr_vita_save_editor_slot_name(
        mdkr_vita_save_editor_selected_slot());
    for (int i = 0; i < 3; ++i) {
        const char c = name != nullptr ? name[i] : '\0';
        s_name[i] = (c >= 'A' && c <= 'Z') ? c : 'A';
    }
    s_name[3] = '\0';
}

bool runtime_result_applied(MdkrVideoRuntimeResult result) {
    return mdkr_video_runtime_result_applied(result) != 0;
}

bool source_is_active(const VitaControlSource &source, const SceCtrlData &pad) {
    if (source.buttons != 0) return (pad.buttons & source.buttons) != 0;
    switch (source.axis) {
        case 1: return pad.ry < 72;
        case 2: return pad.ry > 182;
        case 3: return pad.rx < 72;
        case 4: return pad.rx > 182;
        default: return false;
    }
}

bool any_mappable_input(const SceCtrlData &pad) {
    for (int i = 0; i < k_vita_source_count; ++i) {
        if (source_is_active(k_vita_sources[i], pad)) return true;
    }
    return false;
}

void set_control_status(MdkrVideoRuntimeResult result, const char *success) {
    if (runtime_result_applied(result)) {
        snprintf(s_controls_status, sizeof(s_controls_status), "%s", success);
    } else if (result == MDKR_VIDEO_RUNTIME_LOCKED) {
        snprintf(s_controls_status, sizeof(s_controls_status),
                 "That mapping is locked by a startup override.");
    } else {
        snprintf(s_controls_status, sizeof(s_controls_status),
                 "The mapping could not be saved. Please try again.");
    }
}

void assign_control_source(int action_index, int source_index) {
    MdkrVideoRuntimeChange changes[sizeof(k_vita_sources) /
                                   sizeof(k_vita_sources[0])];
    int count = 0;
    const MdkrVideoConfig *config = mdkr_video_config_current();
    if (action_index < 0 || action_index >= k_game_action_count ||
        source_index < 0 || source_index >= k_vita_source_count ||
        config == nullptr) {
        return;
    }
    for (int i = 0; i < k_vita_source_count; ++i) {
        const char *current = config->values[k_vita_sources[i].key].text;
        if (i == source_index) {
            changes[count++] = {k_vita_sources[i].key,
                                k_game_actions[action_index].value};
        } else if (!strcmp(current, k_game_actions[action_index].value)) {
            changes[count++] = {k_vita_sources[i].key, "none"};
        }
    }
    const MdkrVideoRuntimeResult result =
        mdkr_video_config_runtime_set_many(changes, count);
    char success[128];
    snprintf(success, sizeof(success), "%s mapped to %s.",
             k_game_actions[action_index].label,
             k_vita_sources[source_index].label);
    set_control_status(result, success);
}

void reset_control_defaults() {
    enum {
        MAPPING_COUNT = MDKR_INPUT_CONTROLLER_RIGHT_STICK_RIGHT -
                        MDKR_INPUT_CONTROLLER_A + 1
    };
    MdkrVideoConfig defaults;
    MdkrVideoRuntimeChange changes[MAPPING_COUNT];
    mdkr_video_config_defaults(&defaults);
    for (int i = 0; i < MAPPING_COUNT; ++i) {
        const MdkrVideoKey key =
            (MdkrVideoKey)(MDKR_INPUT_CONTROLLER_A + i);
        changes[i] = {key, defaults.values[key].text};
    }
    set_control_status(mdkr_video_config_runtime_set_many(changes, MAPPING_COUNT),
                       "Vita controls restored to their defaults.");
}

void poll_control_capture() {
    if (s_capture_action < 0) return;
    const uint64_t now = sceKernelGetProcessTimeWide();
    if (now >= s_capture_deadline) {
        s_capture_action = -1;
        snprintf(s_controls_status, sizeof(s_controls_status),
                 "No input received. Mapping was not changed.");
        return;
    }
    SceCtrlData pad = {};
    sceCtrlPeekBufferPositive(0, &pad, 1);
    if (s_capture_wait_release) {
        if (!any_mappable_input(pad)) s_capture_wait_release = false;
        return;
    }
    for (int source = 0; source < k_vita_source_count; ++source) {
        if (!source_is_active(k_vita_sources[source], pad)) continue;
        assign_control_source(s_capture_action, source);
        s_capture_action = -1;
        return;
    }
}

void draw_source_badge(const VitaControlSource &source) {
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const ImVec2 size(74.0f, 25.0f);
    ImGui::Dummy(size);
    ImDrawList *draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y),
                        IM_COL32(38, 42, 51, 255), 5.0f);
    draw->AddRect(pos, ImVec2(pos.x + size.x, pos.y + size.y),
                  source.colour, 5.0f, 0, 1.5f);
    const ImVec2 center(pos.x + 13.0f, pos.y + 12.5f);
    if (source.icon == VITA_ICON_CROSS) {
        draw->AddLine(ImVec2(center.x - 5, center.y - 5),
                      ImVec2(center.x + 5, center.y + 5), source.colour, 2.0f);
        draw->AddLine(ImVec2(center.x + 5, center.y - 5),
                      ImVec2(center.x - 5, center.y + 5), source.colour, 2.0f);
    } else if (source.icon == VITA_ICON_CIRCLE) {
        draw->AddCircle(center, 6.0f, source.colour, 16, 2.0f);
    } else if (source.icon == VITA_ICON_SQUARE) {
        draw->AddRect(ImVec2(center.x - 5, center.y - 5),
                      ImVec2(center.x + 5, center.y + 5), source.colour,
                      0.0f, 0, 2.0f);
    } else if (source.icon == VITA_ICON_TRIANGLE) {
        draw->AddTriangle(ImVec2(center.x, center.y - 6),
                          ImVec2(center.x - 6, center.y + 5),
                          ImVec2(center.x + 6, center.y + 5), source.colour,
                          2.0f);
    } else if (source.icon == VITA_ICON_STICK) {
        draw->AddCircle(center, 7.0f, source.colour, 16, 1.5f);
        ImVec2 tip = center;
        if (source.axis == 1) tip.y -= 8.0f;
        if (source.axis == 2) tip.y += 8.0f;
        if (source.axis == 3) tip.x -= 8.0f;
        if (source.axis == 4) tip.x += 8.0f;
        draw->AddLine(center, tip, source.colour, 2.0f);
        if (source.axis == 1 || source.axis == 2) {
            const float sign = source.axis == 1 ? 1.0f : -1.0f;
            draw->AddLine(tip, ImVec2(tip.x - 3, tip.y + 4 * sign), source.colour, 2.0f);
            draw->AddLine(tip, ImVec2(tip.x + 3, tip.y + 4 * sign), source.colour, 2.0f);
        } else {
            const float sign = source.axis == 3 ? 1.0f : -1.0f;
            draw->AddLine(tip, ImVec2(tip.x + 4 * sign, tip.y - 3), source.colour, 2.0f);
            draw->AddLine(tip, ImVec2(tip.x + 4 * sign, tip.y + 3), source.colour, 2.0f);
        }
    }
    const char *badge_text = (source.icon == VITA_ICON_TEXT ||
                              source.icon == VITA_ICON_STICK)
                                 ? source.short_label : source.label;
    draw->AddText(ImVec2(pos.x + (source.icon == VITA_ICON_TEXT ? 8.0f : 24.0f),
                             pos.y + 5.0f), source.colour, badge_text);
}

void begin_control_capture(int action) {
    s_capture_action = action;
    s_capture_wait_release = true;
    s_capture_deadline = sceKernelGetProcessTimeWide() + UINT64_C(5000000);
    snprintf(s_controls_status, sizeof(s_controls_status),
             "Waiting 5 seconds for a Vita input...");
}

void render_controls_screen() {
    poll_control_capture();
    ImGui::TextUnformatted("CONTROLS");
    ImGui::SameLine(300.0f);
    ImGui::TextUnformatted("Left Stick (fixed steering / pitch / menu):");
    for (int direction = 0; direction < 4; ++direction) {
        ImGui::SameLine();
        draw_source_badge(k_left_stick_directions[direction]);
    }
    ImGui::Separator();
    if (s_capture_action >= 0) {
        const uint64_t now = sceKernelGetProcessTimeWide();
        const unsigned seconds = now < s_capture_deadline
            ? (unsigned)((s_capture_deadline - now + UINT64_C(999999)) /
                         UINT64_C(1000000)) : 0;
        ImGui::TextColored(ImVec4(1.0f, 0.82f, 0.25f, 1.0f),
                           "Press a button or move the right stick for %s (%us)",
                           k_game_actions[s_capture_action].label, seconds);
    } else {
        ImGui::TextUnformatted("Choose an action, then press the desired Vita input within 5 seconds.");
    }
    ImGui::BeginChild("##controlMappings", ImVec2(0.0f, 350.0f), true);
    const MdkrVideoConfig *config = mdkr_video_config_current();
    for (int action = 0; action < k_game_action_count; ++action) {
        ImGui::PushID(action);
        if (ImGui::Button(k_game_actions[action].label, ImVec2(270.0f, 30.0f))) {
            begin_control_capture(action);
        }
        ImGui::SameLine(290.0f);
        bool found = false;
        if (config != nullptr) {
            for (int source = 0; source < k_vita_source_count; ++source) {
                if (strcmp(config->values[k_vita_sources[source].key].text,
                           k_game_actions[action].value) != 0) {
                    continue;
                }
                if (found) ImGui::SameLine();
                draw_source_badge(k_vita_sources[source]);
                found = true;
            }
        }
        if (!found) ImGui::TextDisabled("Not mapped");
        ImGui::PopID();
    }
    ImGui::EndChild();
    ImGui::Text("%s", s_controls_status);
    if (ImGui::Button("Reset to Default Controls", ImVec2(290.0f, 40.0f))) {
        s_capture_action = -1;
        reset_control_defaults();
    }
    ImGui::SameLine();
    if (ImGui::Button("Close", ImVec2(180.0f, 40.0f))) {
        s_capture_action = -1;
        s_open = false;
    }
}
}

extern "C" void mdkr_vita_imgui_overlay_open(void) {
    if (s_open) return;
    mdkr_vita_boot_log("imgui: overlay requested");
    mdkr_vita_boot_log_flush();
    mdkr_vita_save_editor_prepare();
    sync_slot_name();
    s_apply_armed = false;
    s_controls_mode = false;
    s_open = true;
}
extern "C" void mdkr_vita_imgui_overlay_open_controls(void) {
    if (s_open) return;
    mdkr_vita_boot_log("imgui: controls overlay requested");
    mdkr_vita_boot_log_flush();
    s_capture_action = -1;
    s_controls_mode = true;
    s_open = true;
}
extern "C" void mdkr_vita_imgui_overlay_close(void) {
    s_capture_action = -1;
    s_open = false;
}
extern "C" int mdkr_vita_imgui_overlay_is_open(void) { return s_open ? 1 : 0; }

extern "C" int mdkr_vita_imgui_overlay_render(void) {
    if (!s_open) return 0;
    if (!initialize()) {
        mdkr_vita_boot_log(s_controls_mode
            ? "imgui: initialization failed; returning to options"
            : "imgui: initialization failed; returning to classic editor");
        mdkr_vita_boot_log_flush();
        s_open = false;
        if (!s_controls_mode) mdkr_vita_save_editor_open_classic();
        return 0;
    }
    ImGui_ImplVitaGL_NewFrame();
    /* The VitaGL backend starts the Dear ImGui frame itself. Calling
     * ImGui::NewFrame again here leaves the draw list half-initialised on the
     * old Vita backend and was one source of the corrupted overlay. */
    ImGui::SetNextWindowPos(ImVec2(12.0f, 12.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(936.0f, 520.0f), ImGuiCond_Always);
    ImGui::Begin(s_controls_mode ? "Golden Balloon Controls"
                                 : "Golden Balloon Save Editor", &s_open,
                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize);
    if (s_controls_mode) {
        render_controls_screen();
    } else {
    ImGui::TextUnformatted("SAVE EDITOR");
    ImGui::Separator();
    const char *pages[] = { "Saves", "Worlds", "Items", "Unlocks", "Time Trials", "Trophies", "Tools" };
    for (int page = 0; page < 7; ++page) {
        if (page != 0) ImGui::SameLine();
        if (ImGui::Selectable(pages[page], s_page == page, 0, ImVec2(122.0f, 34.0f))) {
            s_page = page;
        }
    }
    ImGui::Separator();
    if (s_page == 0) {
        ImGui::TextUnformatted("Adventure progress");
        int slot = mdkr_vita_save_editor_selected_slot();
        ImGui::Text("Selected save slot: %d", slot + 1);
        for (int candidate = 0; candidate < 3; ++candidate) {
            if (candidate != 0) ImGui::SameLine();
            char label[48];
            snprintf(label, sizeof(label), "Slot %d: %s (%d balloons)", candidate + 1,
                     mdkr_vita_save_editor_slot_name(candidate),
                     mdkr_vita_save_editor_slot_balloons(candidate));
            if (ImGui::Selectable(label, slot == candidate, 0, ImVec2(285.0f, 38.0f))) {
                mdkr_vita_save_editor_select_slot(candidate);
                sync_slot_name();
                s_apply_armed = false;
            }
        }
        ImGui::TextWrapped("Slot selection already shares the classic editor's loaded cache. Progress controls are being connected to that same validated state.");
    } else if (s_page == 1) {
        ImGui::TextUnformatted("World progression");
        const char *worlds[] = { "Dino Domain", "Sherbet Island", "Snowflake Mountain", "Dragon Forest", "Future Fun Land" };
        for (int world = 0; world < 5; ++world) {
            if (world != 0) ImGui::SameLine();
            if (ImGui::Selectable(worlds[world], s_world == world, 0, ImVec2(175.0f, 32.0f))) s_world = world;
        }
        ImGui::Separator();
        for (int track = 0; track < 4; ++track) {
            ImGui::PushID(track);
            int progress = mdkr_vita_save_editor_track_progress(s_world, track);
            ImGui::Text("%s", mdkr_vita_save_editor_track_name(s_world, track));
            ImGui::SameLine(390.0f);
            if (ImGui::Button(progress == 0 ? "Not visited" : progress == 1 ? "Cleared" : "Coin complete", ImVec2(220.0f, 34.0f))) {
                mdkr_vita_save_editor_set_track_progress(s_world, track, (progress + 1) % 3);
            }
            ImGui::PopID();
        }
        ImGui::Separator();
        if (ImGui::Button("Complete Boss 1", ImVec2(210.0f, 38.0f))) {
            mdkr_vita_save_editor_complete_first_boss(s_world);
        }
        ImGui::SameLine();
        if (ImGui::Button("Complete Boss 2", ImVec2(210.0f, 38.0f))) {
            mdkr_vita_save_editor_complete_second_boss(s_world);
        }
        ImGui::SameLine();
        if (ImGui::Button("Complete Trophy Race", ImVec2(250.0f, 38.0f))) {
            mdkr_vita_save_editor_complete_trophy_race(s_world);
        }
    } else if (s_page == 2) {
        static const char *const key_names[] = {
            "Dino Domain key", "Sherbet Island key", "Snowflake Mountain key",
            "Dragon Forest key"
        };
        ImGui::TextUnformatted("Keys, amulets, and key arenas");
        for (int amulet = 0; amulet < 2; ++amulet) {
            const int pieces = mdkr_vita_save_editor_amulet_pieces(amulet);
            ImGui::PushID(100 + amulet);
            ImGui::Text("%s Amulet: %d / 4", amulet == 0 ? "T.T." : "Wizpig", pieces);
            ImGui::SameLine(300.0f);
            if (ImGui::Button("Previous", ImVec2(120.0f, 32.0f))) {
                mdkr_vita_save_editor_set_amulet_pieces(amulet, (pieces + 4) % 5);
            }
            ImGui::SameLine();
            if (ImGui::Button("Next", ImVec2(120.0f, 32.0f))) {
                mdkr_vita_save_editor_set_amulet_pieces(amulet, (pieces + 1) % 5);
            }
            ImGui::PopID();
        }
        ImGui::Separator();
        for (int key = 0; key < 4; ++key) {
            bool collected = mdkr_vita_save_editor_key_collected(key) != 0;
            ImGui::PushID(200 + key);
            if (ImGui::Checkbox(key_names[key], &collected)) {
                mdkr_vita_save_editor_set_key_collected(key, collected ? 1 : 0);
            }
            ImGui::PopID();
            if ((key & 1) == 0) ImGui::SameLine(460.0f);
        }
        ImGui::Separator();
        for (int arena = 0; arena < 4; ++arena) {
            bool complete = mdkr_vita_save_editor_arena_complete(arena) != 0;
            ImGui::PushID(300 + arena);
            if (ImGui::Checkbox(mdkr_vita_save_editor_arena_name(arena), &complete)) {
                mdkr_vita_save_editor_set_arena_complete(arena, complete ? 1 : 0);
            }
            ImGui::PopID();
            if ((arena & 1) == 0) ImGui::SameLine(460.0f);
        }
    } else if (s_page == 3) {
        ImGui::TextUnformatted("Global unlocks");
        ImGui::TextWrapped("These switches update the game's real persistent unlock state. T.T. also updates all twenty T.T. records.");
        ImGui::Separator();
        for (int unlock = 0; unlock < 6; ++unlock) {
            bool enabled = mdkr_vita_save_editor_global_unlock_enabled(unlock) != 0;
            ImGui::PushID(400 + unlock);
            if (ImGui::Checkbox(mdkr_vita_save_editor_global_unlock_name(unlock), &enabled)) {
                mdkr_vita_save_editor_set_global_unlock_enabled(unlock, enabled ? 1 : 0);
                s_apply_armed = false;
            }
            ImGui::PopID();
            if ((unlock & 1) == 0) ImGui::SameLine(460.0f);
        }
        ImGui::Separator();
        ImGui::TextWrapped("Use Apply Changes after editing Adventure 2, T.T., or Drumstick. Bonus-character changes are saved immediately to their roster file.");
    } else if (s_page == 4) {
        ImGui::TextUnformatted("Global Time Trials");
        ImGui::Text("%s", mdkr_vita_save_editor_time_trial_name(s_time_trial));
        if (ImGui::Button("Previous", ImVec2(150.0f, 36.0f))) {
            s_time_trial = (s_time_trial + 19) % 20;
        }
        ImGui::SameLine();
        if (ImGui::Button("Next", ImVec2(150.0f, 36.0f))) {
            s_time_trial = (s_time_trial + 1) % 20;
        }
        bool tt = mdkr_vita_save_editor_tt_beaten(s_time_trial) != 0;
        bool developer = mdkr_vita_save_editor_developer_beaten(s_time_trial) != 0;
        if (ImGui::Checkbox("T.T. defeated", &tt)) {
            mdkr_vita_save_editor_set_tt_beaten(s_time_trial, tt ? 1 : 0);
        }
        if (ImGui::Checkbox("Developer time beaten", &developer)) {
            mdkr_vita_save_editor_set_developer_beaten(s_time_trial, developer ? 1 : 0);
        }
        ImGui::TextWrapped("Developer records use the canonical credits/RetroAchievements order, not the editor's world order.");
    } else if (s_page == 5) {
        ImGui::TextUnformatted("Trophy conditions");
        const char *groups[] = { "Main", "Adventure 2", "Characters", "Power-ups" };
        for (int group = 0; group < 4; ++group) {
            if (group != 0) ImGui::SameLine();
            if (ImGui::Selectable(groups[group], s_trophy_group == group, 0,
                                  ImVec2(205.0f, 32.0f))) {
                s_trophy_group = group;
            }
        }
        ImGui::Separator();
        const char *name = "";
        unsigned trophy_id = 0;
        int condition = 0;
        int *selection = &s_main_trophy;
        int count = 18;
        if (s_trophy_group == 0) {
            name = mdkr_vita_save_editor_main_trophy_name(s_main_trophy);
            trophy_id = (unsigned)s_main_trophy;
            condition = mdkr_vita_save_editor_main_condition_met(s_main_trophy);
        } else if (s_trophy_group == 1) {
            selection = &s_adventure_two_trophy;
            count = 22;
            name = mdkr_vita_save_editor_adventure_two_trophy_name(*selection);
            trophy_id = 18u + (unsigned)*selection;
            condition = mdkr_vita_save_editor_adventure_two_condition_met(*selection);
        } else if (s_trophy_group == 2) {
            selection = &s_character_trophy;
            count = 13;
            name = mdkr_vita_save_editor_character_trophy_name(*selection);
            trophy_id = mdkr_vita_save_editor_character_trophy_id(*selection);
            condition = mdkr_vita_save_editor_character_condition(*selection) >= 5;
        } else {
            selection = &s_powerup_trophy;
            count = 5;
            name = mdkr_vita_save_editor_powerup_trophy_name(*selection);
            trophy_id = mdkr_vita_save_editor_powerup_trophy_id(*selection);
            condition = mdkr_vita_save_editor_powerup_condition(*selection);
        }
        ImGui::Text("%s trophy %d / %d", groups[s_trophy_group], *selection + 1, count);
        ImGui::Text("%s", name);
        if (ImGui::Button("Previous Trophy", ImVec2(180.0f, 36.0f))) {
            *selection = (*selection + count - 1) % count;
        }
        ImGui::SameLine();
        if (ImGui::Button("Next Trophy", ImVec2(180.0f, 36.0f))) {
            *selection = (*selection + 1) % count;
        }
        ImGui::Text("Condition: %s", condition ? "MET" : "NOT MET");
        ImGui::SameLine(310.0f);
        ImGui::Text("Trophy: %s", mdkr_vita_trophy_is_unlocked(trophy_id)
                                           ? "EARNED" : "NOT EARNED");
        if (ImGui::Button(condition ? "Reapply Unlock Condition" : "Meet Unlock Condition",
                          ImVec2(300.0f, 38.0f))) {
            if (s_trophy_group == 0) {
                mdkr_vita_save_editor_meet_main_condition(*selection);
            } else if (s_trophy_group == 1) {
                mdkr_vita_save_editor_meet_adventure_two_condition(*selection);
            } else if (s_trophy_group == 2) {
                mdkr_vita_save_editor_meet_character_condition(*selection);
            } else {
                mdkr_vita_save_editor_meet_powerup_condition(*selection);
            }
        }
        if (s_trophy_group == 1) {
            ImGui::TextWrapped("Adventure 2 trophies require an Adventure 2 save; this control changes the real track, balloon, or boss condition.");
        }
        ImGui::Separator();
        int main_unlocked = 0;
        int adventure_two_unlocked = 0;
        int time_trial_unlocked = 0;
        for (unsigned id = 0; id < 98; ++id) {
            if (!mdkr_vita_trophy_is_unlocked(id)) continue;
            if (id < 18 || id >= 80) ++main_unlocked;
            else if (id < 40) ++adventure_two_unlocked;
            else ++time_trial_unlocked;
        }
        ImGui::Text("Main set: %d / 36", main_unlocked);
        ImGui::Text("Adventure 2 bonus set: %d / 22", adventure_two_unlocked);
        ImGui::Text("Time Trial bonus set: %d / 40", time_trial_unlocked);
        ImGui::TextWrapped("Status comes from the Vita trophy service. Controls satisfy gameplay/save conditions; they do not write trophy IDs directly.");
    } else {
        ImGui::TextUnformatted("Editor tools");
        ImGui::TextWrapped("Save-slot management lives here. Use L+R in the classic editor to return to this overlay.");
        if (mdkr_vita_save_editor_slot_is_empty(mdkr_vita_save_editor_selected_slot())) {
            if (ImGui::Button("Create Empty Save", ImVec2(310.0f, 42.0f))) {
                mdkr_vita_save_editor_create_slot_mode(s_create_adventure_two ? 1 : 0);
                sync_slot_name();
            }
            ImGui::SameLine();
            ImGui::Checkbox("Adventure 2 save", &s_create_adventure_two);
        } else if (ImGui::Button(s_erase_armed ? "Confirm Erase Save" : "Erase Save", ImVec2(310.0f, 42.0f))) {
            if (s_erase_armed) {
                mdkr_vita_save_editor_erase_slot();
                s_erase_armed = false;
                sync_slot_name();
            } else {
                s_erase_armed = true;
            }
        }
        if (!mdkr_vita_save_editor_slot_is_empty(mdkr_vita_save_editor_selected_slot())) {
            ImGui::Text("Save name: %s", s_name);
            for (int i = 0; i < 3; ++i) {
                if (i != 0) ImGui::SameLine();
                char label[16];
                snprintf(label, sizeof(label), "%c##name%d", s_name[i], i);
                if (ImGui::Selectable(label, s_name_cursor == i, 0, ImVec2(64.0f, 34.0f))) {
                    s_name_cursor = i;
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Previous Letter", ImVec2(150.0f, 34.0f))) {
                s_name[s_name_cursor] = s_name[s_name_cursor] == 'A'
                                              ? 'Z' : (char)(s_name[s_name_cursor] - 1);
            }
            ImGui::SameLine();
            if (ImGui::Button("Next Letter", ImVec2(130.0f, 34.0f))) {
                s_name[s_name_cursor] = s_name[s_name_cursor] == 'Z'
                                              ? 'A' : (char)(s_name[s_name_cursor] + 1);
            }
            ImGui::SameLine();
            if (ImGui::Button("Save Name", ImVec2(120.0f, 34.0f))) {
                mdkr_vita_save_editor_rename_slot(s_name);
                sync_slot_name();
            }
        }
        if (ImGui::Button(s_hundred_percent_armed ? "Confirm 100% Progress" : "Complete 100% Progress", ImVec2(310.0f, 42.0f))) {
            if (s_hundred_percent_armed) {
                mdkr_vita_save_editor_complete_wizpig_two();
                s_hundred_percent_armed = false;
            } else {
                s_hundred_percent_armed = true;
            }
        }
        if (ImGui::Button("Use Classic Save Editor", ImVec2(310.0f, 42.0f))) {
            mdkr_vita_save_editor_open_classic();
        }
    }
    ImGui::SetCursorPosY(430.0f);
    ImGui::Text("%s", mdkr_vita_save_editor_status());
    ImGui::SetCursorPosY(465.0f);
    if (ImGui::Button(s_apply_armed ? "Confirm Apply Changes" : "Apply Changes",
                      ImVec2(250.0f, 42.0f))) {
        if (s_apply_armed) {
            mdkr_vita_save_editor_apply_changes();
            s_apply_armed = false;
        } else {
            s_apply_armed = true;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Close", ImVec2(180.0f, 42.0f))) s_open = false;
    ImGui::SameLine();
    ImGui::TextUnformatted(mdkr_vita_save_editor_has_unsaved_changes() ? "UNSAVED CHANGES" : "SAVED");
    }
    ImGui::End();
    ImGui::Render();
    static int diagnostic_frames = 0;
    if (diagnostic_frames < 8) {
        ImDrawData *draw_data = ImGui::GetDrawData();
        char log_line[192];
        snprintf(log_line, sizeof(log_line),
                 "imgui: frame=%d lists=%d vertices=%d indices=%d display=%.0fx%.0f",
                 diagnostic_frames, draw_data ? draw_data->CmdListsCount : -1,
                 draw_data ? draw_data->TotalVtxCount : -1,
                 draw_data ? draw_data->TotalIdxCount : -1,
                 ImGui::GetIO().DisplaySize.x, ImGui::GetIO().DisplaySize.y);
        mdkr_vita_boot_log(log_line);
    }
    GLint game_program = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &game_program);
    glUseProgram(s_program);
    glUniform1i(s_texture_uniform, 0);
    ImGui_ImplVitaGL_RenderDrawData(ImGui::GetDrawData());
    /* vitaGL does not expose GL_VERTEX_ARRAY_BINDING on Vita. The backend
     * therefore uses a private VAO and explicitly returns Fast3D to the
     * primary VAO that owns the game's attribute layout. */
    mdkr_vita_restore_primary_vao();
    if (diagnostic_frames < 8) {
        char log_line[96];
        snprintf(log_line, sizeof(log_line), "imgui: draw complete glError=0x%x", (unsigned)glGetError());
        mdkr_vita_boot_log(log_line);
        mdkr_vita_boot_log_flush();
        ++diagnostic_frames;
    }
    glUseProgram((GLuint)game_program);
    return 1;
}

#else
extern "C" void mdkr_vita_imgui_overlay_open(void) {}
extern "C" void mdkr_vita_imgui_overlay_open_controls(void) {}
extern "C" void mdkr_vita_imgui_overlay_close(void) {}
extern "C" int mdkr_vita_imgui_overlay_is_open(void) { return 0; }
extern "C" int mdkr_vita_imgui_overlay_render(void) { return 0; }
#endif
