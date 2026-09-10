#ifndef MDKR_VITA_SAVE_EDITOR_BRIDGE_H
#define MDKR_VITA_SAVE_EDITOR_BRIDGE_H

#ifdef __cplusplus
extern "C" {
#endif

/* Enters the original controller-first editor. This stays the authoritative
 * save mutation path while the Vita ImGui presentation is built out. */
void mdkr_vita_save_editor_open_classic(void);
void mdkr_vita_save_editor_prepare(void);
int mdkr_vita_save_editor_selected_slot(void);
void mdkr_vita_save_editor_select_slot(int slot);
const char *mdkr_vita_save_editor_slot_name(int slot);
int mdkr_vita_save_editor_slot_balloons(int slot);
void mdkr_vita_save_editor_rename_slot(const char *name);
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
void mdkr_vita_save_editor_complete_first_boss(int world);
void mdkr_vita_save_editor_complete_second_boss(int world);
void mdkr_vita_save_editor_complete_trophy_race(int world);
void mdkr_vita_save_editor_complete_wizpig_two(void);
int mdkr_vita_save_editor_amulet_pieces(int amulet);
void mdkr_vita_save_editor_set_amulet_pieces(int amulet, int pieces);
int mdkr_vita_save_editor_key_collected(int key);
void mdkr_vita_save_editor_set_key_collected(int key, int collected);
const char *mdkr_vita_save_editor_arena_name(int arena);
int mdkr_vita_save_editor_arena_complete(int arena);
void mdkr_vita_save_editor_set_arena_complete(int arena, int complete);
const char *mdkr_vita_save_editor_main_trophy_name(int trophy);
int mdkr_vita_save_editor_main_condition_met(int trophy);
void mdkr_vita_save_editor_meet_main_condition(int trophy);
const char *mdkr_vita_save_editor_adventure_two_trophy_name(int trophy);
int mdkr_vita_save_editor_adventure_two_condition_met(int trophy);
void mdkr_vita_save_editor_meet_adventure_two_condition(int trophy);
const char *mdkr_vita_save_editor_character_trophy_name(int character);
unsigned mdkr_vita_save_editor_character_trophy_id(int character);
int mdkr_vita_save_editor_character_condition(int character);
void mdkr_vita_save_editor_meet_character_condition(int character);
const char *mdkr_vita_save_editor_powerup_trophy_name(int powerup);
unsigned mdkr_vita_save_editor_powerup_trophy_id(int powerup);
int mdkr_vita_save_editor_powerup_condition(int powerup);
void mdkr_vita_save_editor_meet_powerup_condition(int powerup);

#ifdef __cplusplus
}
#endif

#endif
