/* Process/runtime owner for locally imported generic characters. */
#ifndef MDKR64_MODERN_CHARACTER_RUNTIME_H
#define MDKR64_MODERN_CHARACTER_RUNTIME_H

#include <stddef.h>
#include <stdint.h>

#ifndef _LANGUAGE_C
#define _LANGUAGE_C
#endif
#include <PR/gbi.h>

#include "modern_character_registry.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MDKR_MODERN_CHARACTER_PLAYERS 4

/* Scans the local compiled-cache directory and applies MDKR_CUSTOM_CHARACTER
 * (player one) / MDKR_CUSTOM_CHARACTER_P1..P4 diagnostic assignments. */
int mdkr_modern_characters_init(const char *directory);
void mdkr_modern_characters_shutdown(void);

const MdkrModernCharacterRegistry *mdkr_modern_characters_registry(void);
int mdkr_modern_character_assign_player(int player, const char *package_id,
                                        char *error, size_t error_size);
void mdkr_modern_character_clear_player(int player);
const char *mdkr_modern_character_player_package(int player);
int mdkr_modern_character_player_donor(int player);

/* Presentation-only adapter. Vehicle is 0 car, 1 hovercraft, 2 plane. */
int mdkr_modern_character_matches(int player, int donor, int vehicle);
int mdkr_modern_character_tick(int player, const char *semantic,
                               float seconds, char *error, size_t error_size);

/* Emit at the current object matrix in the authored display list. Returns one
 * only when the complete selected LOD was registered and emitted. */
int mdkr_modern_character_emit(int player, float view_distance,
                               Gfx **display_list,
                               char *error, size_t error_size);

/* Draw-local donor replacement evidence. The object renderer calls this only
 * when a fingerprint-qualified retail driver batch is intentionally skipped. */
void mdkr_modern_character_note_hidden_donor_batch(void);

#ifdef __cplusplus
}
#endif

#endif /* MDKR64_MODERN_CHARACTER_RUNTIME_H */
