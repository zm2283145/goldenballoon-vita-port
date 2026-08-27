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

/* Runtime-only stabilization around authored distance bands. The Workshop's
 * structural scrubber deliberately keeps using the exact stateless selector,
 * while a live camera must cross an eight-percent guard band before changing
 * an already visible level. This prevents camera vibration and split-screen
 * interpolation from producing one-frame LOD chatter. */
static inline uint32_t mdkr_modern_character_select_lod_hysteretic(
    float view_distance, float source_lod_bias, float local_lod_bias,
    uint32_t authored_lod_mask, uint32_t previous_lod,
    int previous_valid) {
    const uint32_t selected = mdkr_modern_character_select_lod(
        view_distance, source_lod_bias, local_lod_bias, authored_lod_mask);
    uint32_t guarded;
    if (selected == UINT32_MAX || !previous_valid ||
        previous_lod >= MDKR_MODERN_CHARACTER_LOD_LEVELS ||
        (authored_lod_mask & (1u << previous_lod)) == 0u ||
        selected == previous_lod) return selected;
    if (selected > previous_lod) {
        guarded = mdkr_modern_character_select_lod(
            view_distance * 0.92f, source_lod_bias, local_lod_bias,
            authored_lod_mask);
        if (guarded != UINT32_MAX && guarded <= previous_lod) {
            return previous_lod;
        }
    } else {
        guarded = mdkr_modern_character_select_lod(
            view_distance * 1.08f, source_lod_bias, local_lod_bias,
            authored_lod_mask);
        if (guarded != UINT32_MAX && guarded >= previous_lod) {
            return previous_lod;
        }
    }
    return selected;
}

#ifdef __cplusplus
}
#endif

#endif /* MDKR64_MODERN_CHARACTER_LOD_H */
