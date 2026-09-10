#include "vita_imgui_overlay.h"
#include "vita_save_editor_bridge.h"
#include "vita_trophy.h"

#if defined(__vita__) && defined(MDKR_VITA_IMGUI_OVERLAY)
#include <cstdio>
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
int s_page = 0;
int s_world = 0;
int s_time_trial = 0;
bool s_erase_armed = false;
bool s_hundred_percent_armed = false;
bool s_apply_armed = false;
int s_name_cursor = 0;
char s_name[4] = "NEW";
GLuint s_program = 0;
GLint s_texture_uniform = -1;

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
}

extern "C" void mdkr_vita_imgui_overlay_open(void) {
    if (s_open) return;
    mdkr_vita_boot_log("imgui: overlay requested");
    mdkr_vita_boot_log_flush();
    mdkr_vita_save_editor_prepare();
    sync_slot_name();
    s_apply_armed = false;
    s_open = true;
}
extern "C" void mdkr_vita_imgui_overlay_close(void) { s_open = false; }
extern "C" int mdkr_vita_imgui_overlay_is_open(void) { return s_open ? 1 : 0; }

extern "C" int mdkr_vita_imgui_overlay_render(void) {
    if (!s_open) return 0;
    if (!initialize()) {
        mdkr_vita_boot_log("imgui: initialization failed; returning to classic editor");
        mdkr_vita_boot_log_flush();
        s_open = false;
        mdkr_vita_save_editor_open_classic();
        return 0;
    }
    ImGui_ImplVitaGL_NewFrame();
    /* The VitaGL backend starts the Dear ImGui frame itself. Calling
     * ImGui::NewFrame again here leaves the draw list half-initialised on the
     * old Vita backend and was one source of the corrupted overlay. */
    ImGui::SetNextWindowPos(ImVec2(12.0f, 12.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(936.0f, 520.0f), ImGuiCond_Always);
    ImGui::Begin("Golden Balloon Save Editor", &s_open,
                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize);
    ImGui::TextUnformatted("SAVE EDITOR");
    ImGui::Separator();
    const char *pages[] = { "Progress", "Worlds", "Time Trials", "Trophies", "Tools" };
    for (int page = 0; page < 5; ++page) {
        if (page != 0) ImGui::SameLine();
        if (ImGui::Selectable(pages[page], s_page == page, 0, ImVec2(165.0f, 34.0f))) {
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
            int progress = mdkr_vita_save_editor_track_progress(s_world, track);
            ImGui::Text("%s", mdkr_vita_save_editor_track_name(s_world, track));
            ImGui::SameLine(390.0f);
            if (ImGui::Button(progress == 0 ? "Not visited" : progress == 1 ? "Cleared" : "Coin complete", ImVec2(220.0f, 34.0f))) {
                mdkr_vita_save_editor_set_track_progress(s_world, track, (progress + 1) % 3);
            }
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
    } else if (s_page == 3) {
        ImGui::TextUnformatted("Trophy conditions");
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
        ImGui::Separator();
        ImGui::TextWrapped("Status is read from the Vita trophy service. Condition editing remains state-driven through the Save Editor; this page never unlocks trophies directly.");
    } else {
        ImGui::TextUnformatted("Editor tools");
        ImGui::TextWrapped("Save-slot management lives here. Use L+R in the classic editor to return to this overlay.");
        if (mdkr_vita_save_editor_slot_is_empty(mdkr_vita_save_editor_selected_slot())) {
            if (ImGui::Button("Create Empty Save", ImVec2(310.0f, 42.0f))) {
                mdkr_vita_save_editor_create_slot();
                sync_slot_name();
            }
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
extern "C" void mdkr_vita_imgui_overlay_close(void) {}
extern "C" int mdkr_vita_imgui_overlay_is_open(void) { return 0; }
extern "C" int mdkr_vita_imgui_overlay_render(void) { return 0; }
#endif
