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
#include "modern_character_semantics.h"

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
    const char *short_name;
    const char *narration_name;
    const char *sort_label;
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
    uint64_t contact_solves;
    uint64_t contact_error_micrometres_sum;
    uint64_t contact_error_micrometres_max;
    uint64_t inspection_pose_ticks;
    uint64_t inspection_pose_fallback_ticks;
} MdkrModernCharacterRuntimeMetrics;

/* Latest successfully emitted calibrated volume in the donor target frame.
 * This is exact transform evidence from the real replacement draw, not a
 * second preview renderer and not a claim about every deformed vertex. +Y is
 * up, +Z is intended forward, and zero anchor offset is the automatic
 * ground/seat alignment before the author's context correction. */
typedef struct MdkrModernCharacterFitDiagnostics {
    float bounds_min[3];
    float bounds_max[3];
    float anchor[3];
    float forward[3];
} MdkrModernCharacterFitDiagnostics;

/* Lightweight, borrowed library record for menu/workshop roster surfaces.
 * Catalog inspection never loads GPU mesh data; pointers remain valid until
 * runtime shutdown. `has_identity` is false for legacy donor-fallback media. */
typedef struct MdkrModernCharacterCatalogView {
    const char *id;
    const char *display_name;
    const char *short_name;
    const char *narration_name;
    const char *sort_label;
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
/* Exact-preview measurement seam. Rendering counters remain lifetime totals;
 * only contact quality starts a fresh post-warm-up observation window. */
void mdkr_modern_character_contact_metrics_reset(void);

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

/* Returns the most recently rendered fitted bounds in racer-object local
 * space. The Workshop camera consumes the previous complete render rather
 * than guessing a focus height from a donor or source-model convention.
 * A tuning/assignment change invalidates every context until it renders
 * successfully again. */
int mdkr_modern_character_player_focus(
    int player, MdkrModernCharacterContext context,
    float center[3], float *radius);
int mdkr_modern_character_player_fit_diagnostics(
    int player, MdkrModernCharacterContext context,
    MdkrModernCharacterFitDiagnostics *out);

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

/* Exact Workshop inspection seam. Once enabled, every assigned preview player
 * holds this validated semantic at the same normalized phase regardless of the
 * live scene's transient animation choice. Runtime shutdown always clears it. */
int mdkr_modern_character_set_inspection_pose(
    const char *semantic, float normalized_phase,
    char *error, size_t error_size);
void mdkr_modern_character_clear_inspection_pose(void);

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
