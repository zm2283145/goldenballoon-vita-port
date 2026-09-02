#ifndef MDKR_MODERN_CHARACTER_DRAW_STORE_H
#define MDKR_MODERN_CHARACTER_DRAW_STORE_H

#include <stddef.h>
#include <stdint.h>

#include "fast3d/gfx_rendering_api.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MDKR_MODERN_DRAW_STORE_CAPACITY 2048u
#define MDKR_MODERN_DRAW_STORE_MAX_BONES 256u

/*
 * Copy one draw and its exact current/previous bone palettes into bounded
 * retained storage. The returned generation token is never a host pointer.
 */
uint32_t mdkr_modern_draw_store_register(
    const struct GfxModernSkinnedDraw *draw);

/* Resolve a live generation token, or NULL after bounded storage overtakes it. */
const struct GfxModernSkinnedDraw *mdkr_modern_draw_store_resolve(
    uint32_t token);

/* Invalidate commands that retain an asset before its CPU/GPU owners free it. */
void mdkr_modern_draw_store_release_asset(uint64_t asset_id);

/*
 * Release every lazily allocated palette block. The generation counter stays
 * monotonic so a stale token cannot resolve after a renderer restart.
 */
void mdkr_modern_draw_store_shutdown(void);

/* Exact retained palette capacity, exposed for diagnostics and unit tests. */
size_t mdkr_modern_draw_store_allocated_bytes(void);

/*
 * Process-lifetime count of stale generations that a renderer actually tried
 * to resolve after bounded storage displaced them. Ordinary slot reuse is not
 * an overflow; a nonzero value proves a retained replay exceeded capacity.
 */
uint64_t mdkr_modern_draw_store_overflow_count(void);

#ifdef __cplusplus
}
#endif

#endif
