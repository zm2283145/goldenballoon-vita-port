/* Shared authored-LOD selection policy for runtime and Workshop accounting. */
#ifndef MDKR64_MODERN_CHARACTER_LOD_H
#define MDKR64_MODERN_CHARACTER_LOD_H

#include <math.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MDKR_MODERN_CHARACTER_LOD_LEVELS 4
#define MDKR_MODERN_CHARACTER_LOD_MASK 0xFu

/* Distance chooses a base band. Positive source/local bias retains more detail;
 * negative bias moves toward less detail. Sparse levels fall toward the closest
 * more-detailed authored level. UINT32_MAX denotes invalid input. */
static inline uint32_t mdkr_modern_character_select_lod(
    float view_distance, float source_lod_bias, float local_lod_bias,
    uint32_t authored_lod_mask) {
    uint32_t selected;
    uint32_t maximum = MDKR_MODERN_CHARACTER_LOD_LEVELS - 1u;
    long biased;
    if (!isfinite(view_distance) || !isfinite(source_lod_bias) ||
        !isfinite(local_lod_bias) || source_lod_bias < -4.0f ||
        source_lod_bias > 4.0f || local_lod_bias < -3.0f ||
        local_lod_bias > 3.0f ||
        (authored_lod_mask & MDKR_MODERN_CHARACTER_LOD_MASK) == 0u ||
        (authored_lod_mask & ~MDKR_MODERN_CHARACTER_LOD_MASK) != 0u) {
        return UINT32_MAX;
    }
    if (view_distance < 0.0f) view_distance = 0.0f;
    selected = view_distance >= 2400.0f ? 3u
        : view_distance >= 1300.0f ? 2u
        : view_distance >= 650.0f ? 1u : 0u;
    while (maximum != 0u &&
           (authored_lod_mask & (1u << maximum)) == 0u) {
        --maximum;
    }
    biased = (long)selected - lroundf(source_lod_bias + local_lod_bias);
    if (biased < 0) biased = 0;
    if (biased > (long)maximum) biased = (long)maximum;
    selected = (uint32_t)biased;
    while (selected != 0u &&
           (authored_lod_mask & (1u << selected)) == 0u) {
        --selected;
    }
    return (authored_lod_mask & (1u << selected)) != 0u
        ? selected : UINT32_MAX;
}

#ifdef __cplusplus
}
#endif

#endif /* MDKR64_MODERN_CHARACTER_LOD_H */
