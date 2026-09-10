#include "vita_imgui_overlay.h"

#if defined(__vita__) && defined(MDKR_VITA_IMGUI_OVERLAY)
#include "imgui.h"
#include "imgui_impl_vitagl.h"

namespace {
bool s_initialized = false;
bool s_open = false;

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
    ImGui::TextUnformatted("Vita Save Editor overlay");
    ImGui::Separator();
    ImGui::TextWrapped("The overlay is active. Save editing controls are being wired to the existing validated editor state.");
    if (ImGui::Button("Close")) s_open = false;
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
