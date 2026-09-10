#ifndef MDKR_VITA_SAVE_EDITOR_BRIDGE_H
#define MDKR_VITA_SAVE_EDITOR_BRIDGE_H

#ifdef __cplusplus
extern "C" {
#endif

/* Enters the original controller-first editor. This stays the authoritative
 * save mutation path while the Vita ImGui presentation is built out. */
void mdkr_vita_save_editor_open_classic(void);
int mdkr_vita_save_editor_selected_slot(void);
void mdkr_vita_save_editor_select_slot(int slot);
int mdkr_vita_save_editor_track_progress(int world, int track);
const char *mdkr_vita_save_editor_track_name(int world, int track);
void mdkr_vita_save_editor_set_track_progress(int world, int track, int progress);
int mdkr_vita_save_editor_has_unsaved_changes(void);
const char *mdkr_vita_save_editor_status(void);
void mdkr_vita_save_editor_apply_changes(void);
const char *mdkr_vita_save_editor_time_trial_name(int track);
int mdkr_vita_save_editor_tt_beaten(int track);
int mdkr_vita_save_editor_developer_beaten(int track);
void mdkr_vita_save_editor_set_tt_beaten(int track, int beaten);
void mdkr_vita_save_editor_set_developer_beaten(int track, int beaten);
int mdkr_vita_save_editor_slot_is_empty(int slot);
void mdkr_vita_save_editor_create_slot(void);
void mdkr_vita_save_editor_erase_slot(void);

#ifdef __cplusplus
}
#endif

#endif
