/* App-safe live tuning and exact-fit diagnostics for Character Workshop.
 * This seam deliberately excludes display-list/GPU types so the launcher can
 * author presentation settings without depending on the engine renderer. */
#ifndef MDKR64_MODERN_CHARACTER_STUDIO_BRIDGE_H
#define MDKR64_MODERN_CHARACTER_STUDIO_BRIDGE_H

#include <stddef.h>
#include <stdint.h>

#include "modern_character_asset.h"
#include "modern_character_limits.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MdkrModernCharacterAdjustment {
    float scale;
    float translation[3];
    float rotation_degrees[3];
} MdkrModernCharacterAdjustment;

/* Presentation-only adjustments. They cannot change identity, stats, physics,
 * or the vehicle selected by gameplay. */
typedef struct MdkrModernCharacterTuning {
    float scale;
    float translation[3];
    float rotation_degrees[3];
    float animation_speed;
    float lod_bias;
    uint32_t vehicle_mask;
    MdkrModernCharacterAdjustment context[MDKR_CHARACTER_CONTEXT_COUNT];
    float contact_offset[MDKR_CHARACTER_CONTEXT_COUNT]
                        [MDKR_MODERN_CHARACTER_CONTACTS][3];
} MdkrModernCharacterTuning;

#define MDKR_MODERN_CHARACTER_FIT_LANDMARKS 3u

typedef enum MdkrModernCharacterFitLandmark {
    MDKR_MODERN_CHARACTER_FIT_LANDMARK_HIPS = 0,
    MDKR_MODERN_CHARACTER_FIT_LANDMARK_CHEST,
    MDKR_MODERN_CHARACTER_FIT_LANDMARK_HEAD,
} MdkrModernCharacterFitLandmark;

/* Latest successfully emitted calibrated volume in the donor target frame. */
typedef struct MdkrModernCharacterFitDiagnostics {
    float bounds_min[3];
    float bounds_max[3];
    float anchor[3];
    float forward[3];
    float landmarks[MDKR_MODERN_CHARACTER_FIT_LANDMARKS][3];
    uint32_t landmark_valid_mask;
} MdkrModernCharacterFitDiagnostics;

void mdkr_modern_character_tuning_defaults(MdkrModernCharacterTuning *out);
int mdkr_modern_character_tuning_validate(MdkrModernCharacterTuning *tuning,
                                          char *error, size_t error_size);
int mdkr_modern_character_set_tuning(int player,
                                     const MdkrModernCharacterTuning *tuning,
                                     char *error, size_t error_size);
int mdkr_modern_character_get_tuning(int player,
                                     MdkrModernCharacterTuning *out);
int mdkr_modern_character_player_fit_diagnostics(
    int player, MdkrModernCharacterContext context,
    MdkrModernCharacterFitDiagnostics *out);

#ifdef __cplusplus
}
#endif

#endif  // MDKR64_MODERN_CHARACTER_STUDIO_BRIDGE_H
