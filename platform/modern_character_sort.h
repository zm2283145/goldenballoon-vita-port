/* Deterministic presentation-only ordering for modern character primitives. */
#ifndef MDKR64_MODERN_CHARACTER_SORT_H
#define MDKR64_MODERN_CHARACTER_SORT_H

#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* OPAQUE/MASK retain authored order and always precede BLEND. If every BLEND
 * row has a finite camera-depth key, BLEND rows are stably sorted far-to-near
 * (larger key first); otherwise all BLEND rows retain authored order. Input
 * arrays and output_rows are separate runtime-owned storage. */
static inline int mdkr_modern_character_order_primitive_rows(
    const uint32_t *alpha_mode, const float *camera_depth, size_t count,
    uint32_t *output_rows, size_t *opaque_masked_count, size_t *blend_count,
    int *blend_sort_exact) {
    size_t opaque_count = 0u;
    size_t transparent_count = 0u;
    size_t index;
    int exact = 1;
    if ((count != 0u && (alpha_mode == NULL || camera_depth == NULL ||
                        output_rows == NULL)) ||
        count > (size_t)UINT32_MAX) return 0;
    for (index = 0u; index < count; ++index) {
        if (alpha_mode[index] > 2u) return 0;
        if (alpha_mode[index] != 2u) {
            output_rows[opaque_count++] = (uint32_t)index;
        }
    }
    for (index = 0u; index < count; ++index) {
        if (alpha_mode[index] == 2u) {
            output_rows[opaque_count + transparent_count++] =
                (uint32_t)index;
            if (!isfinite(camera_depth[index])) exact = 0;
        }
    }
    if (exact && transparent_count > 1u) {
        /* Stable insertion sort keeps authored order for equal depths. The
         * package limit is deliberately small (512 primitives), while this
         * avoids allocation and nondeterministic library sort callbacks. */
        for (index = 1u; index < transparent_count; ++index) {
            const uint32_t candidate = output_rows[opaque_count + index];
            const float candidate_depth = camera_depth[candidate];
            size_t insertion = index;
            while (insertion != 0u) {
                const uint32_t previous =
                    output_rows[opaque_count + insertion - 1u];
                if (camera_depth[previous] >= candidate_depth) break;
                output_rows[opaque_count + insertion] = previous;
                --insertion;
            }
            output_rows[opaque_count + insertion] = candidate;
        }
    }
    if (opaque_masked_count != NULL) *opaque_masked_count = opaque_count;
    if (blend_count != NULL) *blend_count = transparent_count;
    if (blend_sort_exact != NULL) *blend_sort_exact = exact;
    return 1;
}

#ifdef __cplusplus
}
#endif

#endif /* MDKR64_MODERN_CHARACTER_SORT_H */
