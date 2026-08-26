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

typedef struct MdkrModernCharacterAdjustment {
    float scale;
    float translation[3];
    float rotation_degrees[3];
} MdkrModernCharacterAdjustment;

/* Presentation-only adjustments layered over the package's authored
 * transform. They deliberately cannot change racer identity, stats, physics,
 * or the vehicle chosen by game logic. `vehicle_mask` only chooses which of
 * the package-qualified car/hover/plane bodies receive the replacement. */
typedef struct MdkrModernCharacterTuning {
    float scale;
    float translation[3];
    float rotation_degrees[3];
    float animation_speed;
    float lod_bias;
    uint32_t vehicle_mask;
    MdkrModernCharacterAdjustment context[MDKR_CHARACTER_CONTEXT_COUNT];
    /* Per-vehicle local presentation offsets, ordered left hand, right hand,
     * left foot, right foot. They tune contact appearance only. */
    float contact_offset[MDKR_CHARACTER_CONTEXT_COUNT]
                        [MDKR_MODERN_CHARACTER_CONTACTS][3];
} MdkrModernCharacterTuning;

typedef struct MdkrModernCharacterIdentityView {
    const char *display_name;
    const uint8_t *portrait_rgba;
    uint32_t portrait_width;
    uint32_t portrait_height;
    uint32_t portrait_stride;
    uint8_t minimap_rgba[4];
    uint64_t revision;
} MdkrModernCharacterIdentityView;

typedef struct MdkrModernCharacterRuntimeMetrics {
    uint64_t replacement_draws;
    uint64_t replacement_primitives;
    uint64_t hidden_donor_batches;
} MdkrModernCharacterRuntimeMetrics;

/* Lightweight, borrowed library record for menu/workshop roster surfaces.
 * Catalog inspection never loads GPU mesh data; pointers remain valid until
 * runtime shutdown. `has_identity` is false for legacy donor-fallback media. */
typedef struct MdkrModernCharacterCatalogView {
    const char *id;
    const char *display_name;
    const uint8_t *portrait_rgba;
    uint32_t portrait_width;
    uint32_t portrait_height;
    uint32_t portrait_stride;
    uint8_t minimap_rgba[4];
    uint32_t donor;
    uint32_t vehicle_mask;
    uint32_t has_identity;
    uint64_t revision;
} MdkrModernCharacterCatalogView;

void mdkr_modern_character_tuning_defaults(MdkrModernCharacterTuning *out);
int mdkr_modern_character_tuning_validate(MdkrModernCharacterTuning *tuning,
                                          char *error, size_t error_size);

/* Scans the local compiled-cache directory and applies MDKR_CUSTOM_CHARACTER
 * (player one) / MDKR_CUSTOM_CHARACTER_P1..P4 diagnostic assignments. */
int mdkr_modern_characters_init(const char *directory);
void mdkr_modern_characters_shutdown(void);
void mdkr_modern_character_runtime_metrics(
    MdkrModernCharacterRuntimeMetrics *out);

const MdkrModernCharacterRegistry *mdkr_modern_characters_registry(void);
int mdkr_modern_character_catalog_count(void);
int mdkr_modern_character_catalog_entry(
    int index, MdkrModernCharacterCatalogView *out);
int mdkr_modern_character_assign_player_index(
    int player, int catalog_index, char *error, size_t error_size);
int mdkr_modern_character_assign_player(int player, const char *package_id,
                                        char *error, size_t error_size);
void mdkr_modern_character_clear_player(int player);
const char *mdkr_modern_character_player_package(int player);
int mdkr_modern_character_player_donor(int player);
/* Borrowed pointers remain valid until that player's assignment changes or
 * the runtime shuts down. Returns zero for an unassigned or legacy package. */
int mdkr_modern_character_player_identity(
    int player, MdkrModernCharacterIdentityView *out);
int mdkr_modern_character_set_tuning(int player,
                                     const MdkrModernCharacterTuning *tuning,
                                     char *error, size_t error_size);
int mdkr_modern_character_get_tuning(int player,
                                     MdkrModernCharacterTuning *out);

/* Presentation-only adapter. Vehicle is 0 car, 1 hovercraft, 2 plane. */
int mdkr_modern_character_matches(int player, int donor, int vehicle);
int mdkr_modern_character_tick(int player, const char *semantic,
                               float seconds, char *error, size_t error_size);
/* As above, but phase-drives an explicitly mapped semantic. A missing mapping
 * uses fallback with ordinary time playback rather than scrubbing the fallback
 * clip with an unrelated gameplay parameter. */
int mdkr_modern_character_tick_phase(int player, const char *semantic,
                                     float seconds, float normalized_phase,
                                     char *error, size_t error_size);

/* Emit at the current object matrix in the authored display list. Returns one
 * only when the complete selected LOD was registered and emitted. */
int mdkr_modern_character_emit(int player, MdkrModernCharacterContext context,
                               const float target_frame[16],
                               float view_distance, Gfx **display_list,
                               char *error, size_t error_size);

/* Copies the package's validated bind-space measurements for diagnostics and
 * assisted calibration. Returns zero for a legacy cache compiled before the
 * calibration sections existed. */
int mdkr_modern_character_player_calibration(
    int player, MdkrModernCalibration *out,
    uint32_t *attachment_context_mask);

/* Draw-local donor replacement evidence. The object renderer calls this only
 * when a fingerprint-qualified retail driver batch is intentionally skipped. */
void mdkr_modern_character_note_hidden_donor_batch(void);

#ifdef __cplusplus
}
#endif

#endif /* MDKR64_MODERN_CHARACTER_RUNTIME_H */
