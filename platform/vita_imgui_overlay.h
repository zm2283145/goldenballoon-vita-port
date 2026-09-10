#ifndef MDKR_VITA_IMGUI_OVERLAY_H
#define MDKR_VITA_IMGUI_OVERLAY_H

/* The native Vita overlay is deliberately a small C ABI seam.  Menu code and
 * the SDL/vitaGL backend stay C-only; Dear ImGui and its VitaGL renderer live
 * in the C++ implementation. */
#ifdef __cplusplus
extern "C" {
#endif
void mdkr_vita_imgui_overlay_open(void);
void mdkr_vita_imgui_overlay_close(void);
int mdkr_vita_imgui_overlay_is_open(void);
int mdkr_vita_imgui_overlay_render(void);
#ifdef __cplusplus
}
#endif

#endif
