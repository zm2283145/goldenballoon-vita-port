/* Exact asynchronous opaque-depth visibility evidence for Character Workshop. */
#ifndef MDKR64_MODERN_CHARACTER_VISIBILITY_H
#define MDKR64_MODERN_CHARACTER_VISIBILITY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MDKR_MODERN_CHARACTER_VISIBILITY_VERSION 2u
#define MDKR_MODERN_CHARACTER_OCCLUDER_CLASSES 3u

typedef enum MdkrModernCharacterVisibilityStatus {
    MDKR_MODERN_CHARACTER_VISIBILITY_IDLE = 0,
    MDKR_MODERN_CHARACTER_VISIBILITY_REQUESTED,
    MDKR_MODERN_CHARACTER_VISIBILITY_IN_FLIGHT,
    MDKR_MODERN_CHARACTER_VISIBILITY_AVAILABLE,
    MDKR_MODERN_CHARACTER_VISIBILITY_UNAVAILABLE,
} MdkrModernCharacterVisibilityStatus;

typedef struct MdkrModernCharacterVisibilityDiagnostics {
    uint32_t version;
    uint32_t valid;
    uint32_t qualified;
    uint32_t output_width;
    uint32_t output_height;
    int32_t viewport[4];
    int32_t scissor[4];
    uint32_t primitive_draws;
    uint32_t opaque_draws;
    uint32_t masked_draws;
    uint32_t transparent_draws;
    uint32_t grid_columns;
    uint32_t grid_rows;
    uint32_t isolated_visible_tiles;
    uint32_t scene_visible_tiles;
    uint64_t isolated_tile_mask;
    uint64_t scene_tile_mask;
    /* Per-class opaque-depth attribution in stable order: retained vehicle
     * body, attached vehicle-part sprites, held object. `present_mask` means
     * the game emitted at least one named batch; `qualified_mask` means every
     * such batch had replayable opaque depth semantics. An absent or
     * unqualified class always carries a zero overlap result. */
    uint32_t occluder_present_mask;
    uint32_t occluder_qualified_mask;
    uint32_t occluder_draws[MDKR_MODERN_CHARACTER_OCCLUDER_CLASSES];
    uint32_t occluder_unqualified_draws
        [MDKR_MODERN_CHARACTER_OCCLUDER_CLASSES];
    uint32_t occluder_overlap_tiles
        [MDKR_MODERN_CHARACTER_OCCLUDER_CLASSES];
    uint64_t occluder_overlap_tile_mask
        [MDKR_MODERN_CHARACTER_OCCLUDER_CLASSES];
} MdkrModernCharacterVisibilityDiagnostics;

#ifdef __cplusplus
}
#endif

#endif
