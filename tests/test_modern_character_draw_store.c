#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "modern_character_draw_store.h"

static int failures;

static void require(int condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        failures++;
    }
}

static struct GfxModernSkinnedDraw fixture_draw(
    const struct GfxModernSkinnedAsset *asset, const float *current,
    const float *previous, uint32_t bones) {
    struct GfxModernSkinnedDraw draw;
    memset(&draw, 0, sizeof(draw));
    draw.asset = asset;
    draw.bone_matrices = current;
    draw.previous_bone_matrices = previous;
    draw.bone_count = bones;
    return draw;
}

int main(void) {
    struct GfxModernSkinnedAsset first_asset;
    struct GfxModernSkinnedAsset second_asset;
    struct GfxModernSkinnedDraw draw;
    const struct GfxModernSkinnedDraw *retained;
    float current[2u * 16u];
    float previous[2u * 16u];
    uint32_t first_token;
    uint32_t second_token;
    uint32_t index;

    memset(&first_asset, 0, sizeof(first_asset));
    memset(&second_asset, 0, sizeof(second_asset));
    first_asset.asset_id = UINT64_C(0x101);
    second_asset.asset_id = UINT64_C(0x202);
    for (index = 0u; index < 2u * 16u; index++) {
        current[index] = (float)index + 0.25f;
        previous[index] = (float)index - 0.75f;
    }

    draw = fixture_draw(&first_asset, current, previous, 2u);
    first_token = mdkr_modern_draw_store_register(&draw);
    require(first_token != 0u, "valid draw registers");
    retained = mdkr_modern_draw_store_resolve(first_token);
    require(retained != NULL, "registered token resolves");
    require(retained != NULL && retained->bone_matrices != current,
            "current palette is copied");
    require(retained != NULL && retained->previous_bone_matrices != previous,
            "previous palette is copied");
    current[0] = 999.0f;
    previous[0] = 999.0f;
    require(retained != NULL && retained->bone_matrices[0] == 0.25f,
            "retained current palette is immutable");
    require(retained != NULL && retained->previous_bone_matrices[0] == -0.75f,
            "retained previous palette is immutable");
    require(mdkr_modern_draw_store_allocated_bytes() ==
                2u * 2u * 16u * sizeof(float),
            "storage uses exact retained palette capacity");

    draw = fixture_draw(&second_asset, NULL, NULL, 0u);
    second_token = mdkr_modern_draw_store_register(&draw);
    require(second_token != 0u, "rigid draw registers without palettes");
    retained = mdkr_modern_draw_store_resolve(second_token);
    require(retained != NULL && retained->bone_matrices == NULL &&
                retained->previous_bone_matrices == NULL,
            "rigid draw retains null palettes");

    draw.bone_count = MDKR_MODERN_DRAW_STORE_MAX_BONES + 1u;
    require(mdkr_modern_draw_store_register(&draw) == 0u,
            "over-limit palette is rejected");
    draw.bone_count = 1u;
    require(mdkr_modern_draw_store_register(&draw) == 0u,
            "missing palette pointers are rejected");
    draw = fixture_draw(NULL, NULL, NULL, 0u);
    require(mdkr_modern_draw_store_register(&draw) == 0u,
            "missing asset is rejected");

    mdkr_modern_draw_store_release_asset(first_asset.asset_id);
    require(mdkr_modern_draw_store_resolve(first_token) == NULL,
            "asset release invalidates its retained draws");
    require(mdkr_modern_draw_store_resolve(second_token) != NULL,
            "asset release preserves unrelated draws");

    draw = fixture_draw(&second_asset, NULL, NULL, 0u);
    require(mdkr_modern_draw_store_overflow_count() == 0u,
            "fresh draw store has no capacity overflows");
    for (index = 0u; index < MDKR_MODERN_DRAW_STORE_CAPACITY; index++) {
        require(mdkr_modern_draw_store_register(&draw) != 0u,
                "bounded ring continues registering");
    }
    require(mdkr_modern_draw_store_overflow_count() == 0u,
            "ordinary bounded-ring reuse is not reported as an overflow");
    require(mdkr_modern_draw_store_resolve(second_token) == NULL,
            "overtaken token fails closed");
    require(mdkr_modern_draw_store_overflow_count() == 1u,
            "an attempted stale replay is counted once observed");

    mdkr_modern_draw_store_shutdown();
    require(mdkr_modern_draw_store_allocated_bytes() == 0u,
            "shutdown releases retained palette allocations");
    require(mdkr_modern_draw_store_resolve(first_token) == NULL,
            "shutdown invalidates every token");
    draw = fixture_draw(&second_asset, NULL, NULL, 0u);
    require(mdkr_modern_draw_store_register(&draw) != first_token,
            "renderer restart cannot reuse a stale generation token");
    require(mdkr_modern_draw_store_overflow_count() == 1u,
            "renderer restart preserves process-lifetime overflow evidence");
    mdkr_modern_draw_store_shutdown();

    if (failures != 0) {
        fprintf(stderr, "%d modern draw-store checks failed\n", failures);
        return 1;
    }
    puts("modern character draw store: PASS");
    return 0;
}
