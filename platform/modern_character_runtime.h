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
#include "modern_character_limits.h"
#include "modern_character_semantics.h"
#include "modern_character_studio_bridge.h"
#include "modern_character_surface_intersection.h"

#ifdef __cplusplus
extern "C" {
#endif

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
    uint64_t reference_draws;
    uint64_t reference_primitives;
    uint64_t hidden_donor_batches;
    uint64_t contact_solves;
    uint64_t contact_error_micrometres_sum;
    uint64_t contact_error_micrometres_max;
    uint64_t contact_residual_step_observations
        [MDKR_MODERN_CHARACTER_CONTACTS];
    uint64_t contact_residual_step_max_micrometres
        [MDKR_MODERN_CHARACTER_CONTACTS];
    uint64_t inspection_pose_ticks;
    uint64_t inspection_pose_fallback_ticks;
    uint64_t inspection_transition_switches;
    uint64_t inspection_transition_blending_ticks;
    uint64_t inspection_transition_completions;
    uint32_t inspection_from_blend_milliseconds;
    uint32_t inspection_to_blend_milliseconds;
    uint32_t inspection_from_motion_source;
    uint32_t inspection_to_motion_source;
} MdkrModernCharacterRuntimeMetrics;

typedef enum MdkrModernCharacterMotionSource {
    MDKR_MODERN_CHARACTER_MOTION_NONE = 0,
    MDKR_MODERN_CHARACTER_MOTION_AUTHORED,
    MDKR_MODERN_CHARACTER_MOTION_REVIEWED_REFERENCE,
    MDKR_MODERN_CHARACTER_MOTION_PACKAGE_FALLBACK,
    MDKR_MODERN_CHARACTER_MOTION_COUNT,
} MdkrModernCharacterMotionSource;

/* Latest exact post-solve contact witnesses in the donor target frame. These
 * are populated only by a successful vehicle replacement draw. */
typedef struct MdkrModernCharacterContactDiagnostics {
    float chain_root[MDKR_MODERN_CHARACTER_CONTACTS][3];
    float bend[MDKR_MODERN_CHARACTER_CONTACTS][3];
    float target[MDKR_MODERN_CHARACTER_CONTACTS][3];
    float end[MDKR_MODERN_CHARACTER_CONTACTS][3];
    float error[MDKR_MODERN_CHARACTER_CONTACTS];
    uint32_t valid_mask;
} MdkrModernCharacterContactDiagnostics;

/* Latest exact post-solve node-local angular excursion from each semantic
 * joint's compiled bind rotation. Stable order is MDKR's 16 humanoid roles.
 * These values are inspection evidence only and never clamp presentation. */
typedef struct MdkrModernCharacterJointDiagnostics {
    float excursion_degrees[MDKR_MODERN_HUMANOID_ROLE_COUNT];
    uint32_t valid_mask;
    uint32_t constraint_clamped_mask;
    uint32_t secondary_chain_count;
    uint32_t secondary_joint_count;
    uint32_t secondary_active_joint_count;
    float secondary_max_deflection_degrees;
    uint64_t secondary_discontinuity_resets;
} MdkrModernCharacterJointDiagnostics;

typedef struct MdkrModernCharacterVehicleShell {
    const MdkrModernSurfaceTriangle *triangles;
    uint32_t triangle_count;
} MdkrModernCharacterVehicleShell;

/* Immutable camera evidence captured after the donor object's model push.
 * object_mvp maps donor-object local coordinates directly to clip space. */
typedef struct MdkrModernCharacterLodView {
    float object_mvp[16];
    float logical_viewport_height;
    uint64_t projection_generation;
} MdkrModernCharacterLodView;

typedef struct MdkrModernCharacterLodDiagnostics {
    float projected_height_pixels;
    float fallback_distance;
    uint64_t projection_generation;
    uint32_t selected_lod;
    uint32_t authored_lod_mask;
    uint32_t used_projected_height;
    uint32_t opaque_masked_primitives;
    uint32_t blend_primitives;
    uint32_t blend_sort_mode;
} MdkrModernCharacterLodDiagnostics;

enum {
    MDKR_MODERN_CHARACTER_BLEND_SORT_NONE = 0,
    MDKR_MODERN_CHARACTER_BLEND_SORT_POSED_CENTROID = 1,
    MDKR_MODERN_CHARACTER_BLEND_SORT_AUTHORED_FALLBACK = 2
};

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
/* The launcher publishes a source-bound playability allow-list. When that
 * contract is present, catalog browsing and every assignment path reject
 * incomplete Workshop packages. A missing contract preserves explicit CLI
 * and standalone diagnostic workflows. */
int mdkr_modern_character_catalog_playable(int index);
int mdkr_modern_character_assign_player_index(
    int player, int catalog_index, char *error, size_t error_size);
int mdkr_modern_character_assign_player(int player, const char *package_id,
                                        char *error, size_t error_size);
/* Atomically publishes the complete race-player assignment. Each entry is a
 * catalog index or -1 for a retail/unassigned player. Every selected cache,
 * decoded render asset, pose, and tuning profile is staged before any current
 * player changes; failure preserves the entire last-known-good assignment. */
int mdkr_modern_character_apply_catalog_plan(
    const int catalog_indices[MDKR_MODERN_CHARACTER_PLAYERS],
    char *error, size_t error_size);
void mdkr_modern_character_clear_player(int player);
const char *mdkr_modern_character_player_package(int player);
int mdkr_modern_character_player_donor(int player);
/* Borrowed pointers remain valid until that player's assignment changes or
 * the runtime shuts down. Returns zero for an unassigned or legacy package. */
int mdkr_modern_character_player_identity(
    int player, MdkrModernCharacterIdentityView *out);

/* Returns the most recently rendered fitted bounds in racer-object local
 * space. The Workshop camera consumes the previous complete render rather
 * than guessing a focus height from a donor or source-model convention.
 * A tuning/assignment change invalidates every context until it renders
 * successfully again. */
int mdkr_modern_character_player_focus(
    int player, MdkrModernCharacterContext context,
    float center[3], float *radius);
int mdkr_modern_character_player_contact_diagnostics(
    int player, MdkrModernCharacterContext context,
    MdkrModernCharacterContactDiagnostics *out);
int mdkr_modern_character_player_joint_diagnostics(
    int player, MdkrModernCharacterContext context,
    MdkrModernCharacterJointDiagnostics *out);
int mdkr_modern_character_player_lod_diagnostics(
    int player, int view, MdkrModernCharacterContext context,
    MdkrModernCharacterLodDiagnostics *out);

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
/* True only after this player has consumed the current held-pose generation,
 * evaluated its exact target sample, and completed any residual pose blend.
 * Exact Workshop evidence uses this as an engine-owned settling witness before
 * it starts counting stable replacement draws. */
int mdkr_modern_character_inspection_pose_settled(int player);
/* Repeatedly alternates between two exact held samples, using the ordinary
 * pose player's semantic-change blend each way. The one-second dwell is an
 * inspection cadence, not an authored animation duration. */
int mdkr_modern_character_set_inspection_transition(
    const char *from_semantic, float from_normalized_phase,
    const char *to_semantic, float to_normalized_phase,
    char *error, size_t error_size);
void mdkr_modern_character_clear_inspection_pose(void);

/* Emit at the current object matrix in the authored display list. Returns one
 * only when the complete selected LOD was registered and emitted. */
int mdkr_modern_character_emit(int player, int view,
                               MdkrModernCharacterContext context,
                               const float target_frame[16],
                               const MdkrModernCharacterVehicleShell *shell,
                               const MdkrModernCharacterLodView *lod_view,
                               float view_distance, Gfx **display_list,
                               char *error, size_t error_size);

/* One-shot Workshop-only surface witness. Requesting clears any prior result;
 * the next complete player/context draw consumes the request. Vehicle shell
 * geometry must come from fingerprint-qualified retained batches in the same
 * donor-target frame as the replacement. Ordinary gameplay never requests or
 * pays for this CPU geometry walk. */
int mdkr_modern_character_request_surface_diagnostics(
    int player, MdkrModernCharacterContext context);
int mdkr_modern_character_surface_diagnostics_requested(
    int player, MdkrModernCharacterContext context);
int mdkr_modern_character_player_surface_diagnostics(
    int player, MdkrModernCharacterContext context,
    MdkrModernSurfaceIntersectionDiagnostics *out);

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
