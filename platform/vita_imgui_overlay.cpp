#include "vita_imgui_overlay.h"
#include "vita_save_editor_bridge.h"

#if defined(__vita__) && defined(MDKR_VITA_IMGUI_OVERLAY)
#include "imgui.h"
#include "imgui_impl_vitagl.h"

namespace {
bool s_initialized = false;
bool s_open = false;
int s_page = 0;

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
        ImGui::TextWrapped("Slot, balloon, track, boss, Taj, hub, and arena controls are being connected to the existing save validation layer.");
    } else if (s_page == 1) {
        ImGui::TextUnformatted("World progression");
        ImGui::TextWrapped("Track results will preserve the same prerequisite and balloon-count rules as the classic editor.");
    } else if (s_page == 2) {
        ImGui::TextUnformatted("Global Time Trials");
        ImGui::TextWrapped("T.T. and developer records will use the corrected canonical track mapping from 1.6.4.");
    } else if (s_page == 3) {
        ImGui::TextUnformatted("Trophy conditions");
        ImGui::TextWrapped("Main, Adventure 2, Time Trial, character, and power-up conditions will remain state-driven rather than directly unlocking trophies.");
    } else {
        ImGui::TextUnformatted("Editor tools");
        ImGui::TextWrapped("Save-slot management lives here. Use L+R in the classic editor to return to this overlay.");
        if (ImGui::Button("Use Classic Save Editor", ImVec2(310.0f, 42.0f))) {
            mdkr_vita_save_editor_open_classic();
        }
    }
    ImGui::SetCursorPosY(455.0f);
    if (ImGui::Button("Close", ImVec2(180.0f, 42.0f))) s_open = false;
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
