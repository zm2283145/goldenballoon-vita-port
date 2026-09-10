#include "vita_imgui_overlay.h"
#include "vita_save_editor_bridge.h"
#include "vita_trophy.h"

#if defined(__vita__) && defined(MDKR_VITA_IMGUI_OVERLAY)
#include <cstdio>
#include "imgui.h"
#include "imgui_impl_vitagl.h"

namespace {
bool s_initialized = false;
bool s_open = false;
int s_page = 0;
int s_world = 0;
int s_time_trial = 0;
bool s_erase_armed = false;

bool initialize() {
    if (s_initialized) return true;
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    if (!ImGui_ImplVitaGL_Init_Extended()) {
        ImGui::DestroyContext();
        return false;
    }
    ImGui_ImplVitaGL_GamepadUsage(true);
    ImGui_ImplVitaGL_TouchUsage(true);
    s_initialized = true;
    return true;
}
}

extern "C" void mdkr_vita_imgui_overlay_open(void) { s_open = true; }
extern "C" void mdkr_vita_imgui_overlay_close(void) { s_open = false; }
extern "C" int mdkr_vita_imgui_overlay_is_open(void) { return s_open ? 1 : 0; }

extern "C" int mdkr_vita_imgui_overlay_render(void) {
    if (!s_open || !initialize()) return 0;
    ImGui_ImplVitaGL_NewFrame();
    ImGui::NewFrame();
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
            char label[16];
            snprintf(label, sizeof(label), "Slot %d", candidate + 1);
            if (ImGui::Selectable(label, slot == candidate, 0, ImVec2(150.0f, 38.0f))) {
                mdkr_vita_save_editor_select_slot(candidate);
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
            }
        } else if (ImGui::Button(s_erase_armed ? "Confirm Erase Save" : "Erase Save", ImVec2(310.0f, 42.0f))) {
            if (s_erase_armed) {
                mdkr_vita_save_editor_erase_slot();
                s_erase_armed = false;
            } else {
                s_erase_armed = true;
            }
        }
        if (ImGui::Button("Use Classic Save Editor", ImVec2(310.0f, 42.0f))) {
            mdkr_vita_save_editor_open_classic();
        }
    }
    ImGui::SetCursorPosY(430.0f);
    ImGui::Text("%s", mdkr_vita_save_editor_status());
    ImGui::SetCursorPosY(465.0f);
    if (ImGui::Button("Apply Changes", ImVec2(250.0f, 42.0f))) {
        mdkr_vita_save_editor_apply_changes();
    }
    ImGui::SameLine();
    if (ImGui::Button("Close", ImVec2(180.0f, 42.0f))) s_open = false;
    ImGui::SameLine();
    ImGui::TextUnformatted(mdkr_vita_save_editor_has_unsaved_changes() ? "UNSAVED CHANGES" : "SAVED");
    ImGui::End();
    ImGui::Render();
    ImGui_ImplVitaGL_RenderDrawData(ImGui::GetDrawData());
    return 1;
}

#else
extern "C" void mdkr_vita_imgui_overlay_open(void) {}
extern "C" void mdkr_vita_imgui_overlay_close(void) {}
extern "C" int mdkr_vita_imgui_overlay_is_open(void) { return 0; }
extern "C" int mdkr_vita_imgui_overlay_render(void) { return 0; }
#endif
