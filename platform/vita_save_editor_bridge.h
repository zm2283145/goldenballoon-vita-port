#ifndef MDKR_VITA_SAVE_EDITOR_BRIDGE_H
#define MDKR_VITA_SAVE_EDITOR_BRIDGE_H

#ifdef __cplusplus
extern "C" {
#endif

/* Enters the original controller-first editor. This stays the authoritative
 * save mutation path while the Vita ImGui presentation is built out. */
void mdkr_vita_save_editor_open_classic(void);

#ifdef __cplusplus
}
#endif

#endif
