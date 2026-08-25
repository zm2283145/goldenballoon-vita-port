#include "modern_character_runtime.h"

#include "fast3d/gfx_pc_dkr.h"
#include "f3ddkr.h"
#include "modern_character_pose.h"
#include "modern_character_render.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MODERN_RUNTIME_POOLS 4
#define MODERN_RUNTIME_MAX_PRIMITIVES 512u
#define MODERN_RUNTIME_MAX_BONES 128u

typedef struct MdkrModernRuntimePool {
    int registry_index;
    int references;
    MdkrModernCharacterAsset asset;
    MdkrModernRenderAsset render;
    MdkrModernCharacterDefinition definition;
} MdkrModernRuntimePool;

typedef struct MdkrModernRuntimePlayer {
    int pool;
    MdkrModernPose pose;
    char semantic[96];
    float palette[MODERN_RUNTIME_MAX_BONES * 16u];
    uint32_t tokens[MODERN_RUNTIME_MAX_PRIMITIVES];
} MdkrModernRuntimePlayer;

static MdkrModernCharacterRegistry s_registry;
static MdkrModernRuntimePool s_pools[MODERN_RUNTIME_POOLS];
static MdkrModernRuntimePlayer s_players[MDKR_MODERN_CHARACTER_PLAYERS];
static int s_initialized;
static uint64_t s_replacement_draws;
static uint64_t s_replacement_primitives;
static uint64_t s_hidden_donor_batches;

static void set_error(char *error, size_t size, const char *message) {
    if (error != NULL && size != 0u) {
        (void)snprintf(error, size, "%s", message != NULL ? message : "unknown error");
    }
}

static void matrix_identity(float output[16]) {
    memset(output, 0, sizeof(float) * 16u);
    output[0] = output[5] = output[10] = output[15] = 1.0f;
}

static void matrix_multiply(const float left[16], const float right[16],
                            float output[16]) {
    float result[16];
    unsigned column;
    unsigned row;
    unsigned inner;
    for (column = 0u; column < 4u; column++) {
        for (row = 0u; row < 4u; row++) {
            float value = 0.0f;
            for (inner = 0u; inner < 4u; inner++) {
                value += left[inner * 4u + row] * right[column * 4u + inner];
            }
            result[column * 4u + row] = value;
        }
    }
    memcpy(output, result, sizeof(result));
}

static int matrix_normal_transform(const float input[16], float output[16]) {
    float a00 = input[0], a01 = input[4], a02 = input[8];
    float a10 = input[1], a11 = input[5], a12 = input[9];
    float a20 = input[2], a21 = input[6], a22 = input[10];
    float determinant = a00 * (a11 * a22 - a12 * a21) -
                        a01 * (a10 * a22 - a12 * a20) +
                        a02 * (a10 * a21 - a11 * a20);
    float inverse;
    if (!isfinite(determinant) || fabsf(determinant) < 1.0e-12f) return 0;
    inverse = 1.0f / determinant;
    matrix_identity(output);
    /* Inverse transpose of the upper-left 3x3, stored column-major. */
    output[0] = (a11 * a22 - a12 * a21) * inverse;
    output[1] = (a02 * a21 - a01 * a22) * inverse;
    output[2] = (a01 * a12 - a02 * a11) * inverse;
    output[4] = (a12 * a20 - a10 * a22) * inverse;
    output[5] = (a00 * a22 - a02 * a20) * inverse;
    output[6] = (a02 * a10 - a00 * a12) * inverse;
    output[8] = (a10 * a21 - a11 * a20) * inverse;
    output[9] = (a01 * a20 - a00 * a21) * inverse;
    output[10] = (a00 * a11 - a01 * a10) * inverse;
    return 1;
}

static int matrix_affine_inverse(const float input[16], float output[16]) {
    float a00 = input[0], a01 = input[4], a02 = input[8];
    float a10 = input[1], a11 = input[5], a12 = input[9];
    float a20 = input[2], a21 = input[6], a22 = input[10];
    float tx = input[12], ty = input[13], tz = input[14];
    float determinant = a00 * (a11 * a22 - a12 * a21) -
                        a01 * (a10 * a22 - a12 * a20) +
                        a02 * (a10 * a21 - a11 * a20);
    float inverse;
    if (!isfinite(determinant) || fabsf(determinant) < 1.0e-12f) return 0;
    inverse = 1.0f / determinant;
    matrix_identity(output);
    output[0] = (a11 * a22 - a12 * a21) * inverse;
    output[4] = (a02 * a21 - a01 * a22) * inverse;
    output[8] = (a01 * a12 - a02 * a11) * inverse;
    output[1] = (a12 * a20 - a10 * a22) * inverse;
    output[5] = (a00 * a22 - a02 * a20) * inverse;
    output[9] = (a02 * a10 - a00 * a12) * inverse;
    output[2] = (a10 * a21 - a11 * a20) * inverse;
    output[6] = (a01 * a20 - a00 * a21) * inverse;
    output[10] = (a00 * a11 - a01 * a10) * inverse;
    output[12] = -(output[0] * tx + output[4] * ty + output[8] * tz);
    output[13] = -(output[1] * tx + output[5] * ty + output[9] * tz);
    output[14] = -(output[2] * tx + output[6] * ty + output[10] * tz);
    return 1;
}

static void definition_matrix(const MdkrModernCharacterDefinition *definition,
                              float output[16]) {
    float x = definition->rotation[0];
    float y = definition->rotation[1];
    float z = definition->rotation[2];
    float w = definition->rotation[3];
    float sx = definition->scale[0];
    float sy = definition->scale[1];
    float sz = definition->scale[2];
    matrix_identity(output);
    output[0] = (1.0f - 2.0f * (y * y + z * z)) * sx;
    output[1] = (2.0f * (x * y + z * w)) * sx;
    output[2] = (2.0f * (x * z - y * w)) * sx;
    output[4] = (2.0f * (x * y - z * w)) * sy;
    output[5] = (1.0f - 2.0f * (x * x + z * z)) * sy;
    output[6] = (2.0f * (y * z + x * w)) * sy;
    output[8] = (2.0f * (x * z + y * w)) * sz;
    output[9] = (2.0f * (y * z - x * w)) * sz;
    output[10] = (1.0f - 2.0f * (x * x + y * y)) * sz;
    output[12] = definition->translation[0];
    output[13] = definition->translation[1];
    output[14] = definition->translation[2];
}

static void pool_release(int index) {
    MdkrModernRuntimePool *pool;
    if (index < 0 || index >= MODERN_RUNTIME_POOLS) return;
    pool = &s_pools[index];
    if (pool->references > 0) pool->references--;
    if (pool->references != 0 || pool->registry_index < 0) return;
    if (pool->render.valid) {
        gfx_modern_character_release_asset(pool->render.gpu.asset_id);
    }
    mdkr_modern_render_asset_shutdown(&pool->render);
    mdkr_modern_character_asset_unload(&pool->asset);
    memset(pool, 0, sizeof(*pool));
    pool->registry_index = -1;
}

static int pool_acquire(int registry_index, char *error, size_t error_size) {
    int index;
    for (index = 0; index < MODERN_RUNTIME_POOLS; index++) {
        if (s_pools[index].registry_index == registry_index &&
            s_pools[index].references > 0) {
            s_pools[index].references++;
            return index;
        }
    }
    for (index = 0; index < MODERN_RUNTIME_POOLS; index++) {
        MdkrModernRuntimePool *pool = &s_pools[index];
        if (pool->references != 0) continue;
        memset(pool, 0, sizeof(*pool));
        pool->registry_index = registry_index;
        if (!mdkr_modern_character_registry_load(
                &s_registry, registry_index, &pool->asset, error, error_size) ||
            !mdkr_modern_character_asset_definition(&pool->asset,
                                                     &pool->definition) ||
            !mdkr_modern_render_asset_init(&pool->render, &pool->asset,
                                           error, error_size)) {
            mdkr_modern_render_asset_shutdown(&pool->render);
            mdkr_modern_character_asset_unload(&pool->asset);
            memset(pool, 0, sizeof(*pool));
            pool->registry_index = -1;
            return -1;
        }
        if (pool->render.gpu.primitive_count > MODERN_RUNTIME_MAX_PRIMITIVES) {
            set_error(error, error_size,
                      "character exceeds the 512-primitive runtime budget");
            mdkr_modern_render_asset_shutdown(&pool->render);
            mdkr_modern_character_asset_unload(&pool->asset);
            memset(pool, 0, sizeof(*pool));
            pool->registry_index = -1;
            return -1;
        }
        pool->references = 1;
        return index;
    }
    set_error(error, error_size, "four distinct active character assets are already loaded");
    return -1;
}

int mdkr_modern_characters_init(const char *directory) {
    int index;
    char error[256];
    const char *base;
    mdkr_modern_characters_shutdown();
    s_replacement_draws = 0u;
    s_replacement_primitives = 0u;
    s_hidden_donor_batches = 0u;
    for (index = 0; index < MODERN_RUNTIME_POOLS; index++) {
        s_pools[index].registry_index = -1;
    }
    for (index = 0; index < MDKR_MODERN_CHARACTER_PLAYERS; index++) {
        s_players[index].pool = -1;
    }
    if (mdkr_modern_character_registry_init(&s_registry, directory) != 0) return 0;
    s_initialized = 1;
    base = getenv("MDKR_CUSTOM_CHARACTER");
    for (index = 0; index < MDKR_MODERN_CHARACTER_PLAYERS; index++) {
        char name[40];
        const char *selection;
        (void)snprintf(name, sizeof(name), "MDKR_CUSTOM_CHARACTER_P%d", index + 1);
        selection = getenv(name);
        if ((selection == NULL || selection[0] == '\0') && index == 0) selection = base;
        if (selection == NULL || selection[0] == '\0') continue;
        if (!mdkr_modern_character_assign_player(index, selection,
                                                 error, sizeof(error))) {
            fprintf(stderr, "[modern-character] P%d %s: %s\n",
                    index + 1, selection, error);
        }
    }
    for (index = 0; index < s_registry.skipped; index++) {
        fprintf(stderr, "[modern-character] skipped %s: %s\n",
                s_registry.skip_name[index], s_registry.skip_reason[index]);
    }
    return 1;
}

void mdkr_modern_characters_shutdown(void) {
    int index;
    if (s_replacement_draws != 0u || s_hidden_donor_batches != 0u) {
        fprintf(stderr,
                "[MODERN-CHARACTER] replacements=%llu primitives=%llu "
                "hiddenDonorBatches=%llu\n",
                (unsigned long long)s_replacement_draws,
                (unsigned long long)s_replacement_primitives,
                (unsigned long long)s_hidden_donor_batches);
    }
    for (index = 0; index < MDKR_MODERN_CHARACTER_PLAYERS; index++) {
        mdkr_modern_character_clear_player(index);
    }
    for (index = 0; index < MODERN_RUNTIME_POOLS; index++) {
        if (s_pools[index].references != 0) {
            s_pools[index].references = 1;
            pool_release(index);
        }
        s_pools[index].registry_index = -1;
    }
    mdkr_modern_character_registry_shutdown(&s_registry);
    s_initialized = 0;
}

void mdkr_modern_character_note_hidden_donor_batch(void) {
    s_hidden_donor_batches++;
}

const MdkrModernCharacterRegistry *mdkr_modern_characters_registry(void) {
    return s_initialized ? &s_registry : NULL;
}

void mdkr_modern_character_clear_player(int player) {
    MdkrModernRuntimePlayer *slot;
    int pool;
    if (player < 0 || player >= MDKR_MODERN_CHARACTER_PLAYERS) return;
    slot = &s_players[player];
    pool = slot->pool;
    mdkr_modern_pose_shutdown(&slot->pose);
    memset(slot, 0, sizeof(*slot));
    slot->pool = -1;
    pool_release(pool);
}

int mdkr_modern_character_assign_player(int player, const char *package_id,
                                        char *error, size_t error_size) {
    MdkrModernRuntimePlayer *slot;
    int registry_index;
    int pool;
    if (!s_initialized || player < 0 || player >= MDKR_MODERN_CHARACTER_PLAYERS ||
        package_id == NULL || package_id[0] == '\0') {
        set_error(error, error_size, "character assignment arguments are invalid");
        return 0;
    }
    if (!gfx_modern_character_supported()) {
        set_error(error, error_size,
                  "active renderer does not support GPU-skinned characters");
        return 0;
    }
    registry_index = mdkr_modern_character_registry_find(&s_registry, package_id);
    if (registry_index < 0) {
        set_error(error, error_size, "character package id is not installed");
        return 0;
    }
    mdkr_modern_character_clear_player(player);
    pool = pool_acquire(registry_index, error, error_size);
    if (pool < 0) return 0;
    slot = &s_players[player];
    slot->pool = pool;
    if (!mdkr_modern_pose_init(&slot->pose, &s_pools[pool].asset,
                               error, error_size)) {
        slot->pool = -1;
        pool_release(pool);
        return 0;
    }
    (void)snprintf(slot->semantic, sizeof(slot->semantic), "%s", "fallback");
    fprintf(stderr, "[modern-character] P%d=%s donor=%u triangles=%u\n",
            player + 1, package_id, s_pools[pool].definition.donor,
            s_pools[pool].render.gpu.index_count / 3u);
    set_error(error, error_size, "");
    return 1;
}

const char *mdkr_modern_character_player_package(int player) {
    MdkrModernRuntimePlayer *slot;
    MdkrModernRuntimePool *pool;
    if (player < 0 || player >= MDKR_MODERN_CHARACTER_PLAYERS) return NULL;
    slot = &s_players[player];
    if (slot->pool < 0) return NULL;
    pool = &s_pools[slot->pool];
    return mdkr_modern_character_asset_string(&pool->asset, pool->definition.id);
}

int mdkr_modern_character_player_donor(int player) {
    if (player < 0 || player >= MDKR_MODERN_CHARACTER_PLAYERS ||
        s_players[player].pool < 0) return -1;
    return (int)s_pools[s_players[player].pool].definition.donor;
}

int mdkr_modern_character_matches(int player, int donor, int vehicle) {
    MdkrModernRuntimePlayer *slot;
    MdkrModernRuntimePool *pool;
    if (player < 0 || player >= MDKR_MODERN_CHARACTER_PLAYERS ||
        vehicle < 0 || vehicle > 2) return 0;
    slot = &s_players[player];
    if (slot->pool < 0) return 0;
    pool = &s_pools[slot->pool];
    return (int)pool->definition.donor == donor &&
           (pool->definition.vehicle_mask & (1u << (unsigned)vehicle)) != 0u;
}

int mdkr_modern_character_tick(int player, const char *semantic,
                               float seconds, char *error, size_t error_size) {
    MdkrModernRuntimePlayer *slot;
    if (player < 0 || player >= MDKR_MODERN_CHARACTER_PLAYERS ||
        (slot = &s_players[player])->pool < 0 || semantic == NULL ||
        !isfinite(seconds) || seconds < 0.0f) {
        set_error(error, error_size, "modern character tick arguments are invalid");
        return 0;
    }
    if (strcmp(slot->semantic, semantic) != 0) {
        if (!mdkr_modern_pose_set_semantic(&slot->pose, semantic,
                                           error, error_size)) return 0;
        (void)snprintf(slot->semantic, sizeof(slot->semantic), "%s", semantic);
    }
    return mdkr_modern_pose_advance(&slot->pose, seconds, error, error_size);
}

int mdkr_modern_character_emit(int player, float view_distance,
                               Gfx **display_list,
                               char *error, size_t error_size) {
    MdkrModernRuntimePlayer *slot;
    MdkrModernRuntimePool *pool;
    float package_transform[16];
    float seat_transform[16];
    float inverse_seat[16];
    float anchored_transform[16];
    uint32_t primitive_index;
    uint32_t selected_lod;
    uint32_t available_lod = 0u;
    uint32_t emitted = 0u;
    if (player < 0 || player >= MDKR_MODERN_CHARACTER_PLAYERS ||
        display_list == NULL || *display_list == NULL ||
        (slot = &s_players[player])->pool < 0 ||
        !gfx_modern_character_supported() || !isfinite(view_distance)) {
        set_error(error, error_size, "modern character draw is unavailable");
        return 0;
    }
    pool = &s_pools[slot->pool];
    for (primitive_index = 0u;
         primitive_index < pool->render.gpu.primitive_count;
         primitive_index++) {
        if (pool->render.gpu.primitives[primitive_index].lod > available_lod) {
            available_lod = pool->render.gpu.primitives[primitive_index].lod;
        }
    }
    if (view_distance < 0.0f) view_distance = 0.0f;
    selected_lod = view_distance >= 2400.0f ? 3u
        : view_distance >= 1300.0f ? 2u
        : view_distance >= 650.0f ? 1u : 0u;
    {
        int biased = (int)selected_lod - (int)lroundf(pool->definition.lod_bias);
        if (biased < 0) biased = 0;
        if ((uint32_t)biased > available_lod) biased = (int)available_lod;
        selected_lod = (uint32_t)biased;
    }
    /* Sparse authoring is legal: select the closest more-detailed complete
     * level rather than drawing nothing at an absent distance band. */
    for (;;) {
        int found = 0;
        for (primitive_index = 0u;
             primitive_index < pool->render.gpu.primitive_count;
             primitive_index++) {
            if (pool->render.gpu.primitives[primitive_index].lod == selected_lod) {
                found = 1;
                break;
            }
        }
        if (found || selected_lod == 0u) break;
        selected_lod--;
    }
    if (!mdkr_modern_pose_socket_matrix(&slot->pose, "seat", 0,
                                        seat_transform) ||
        !matrix_affine_inverse(seat_transform, inverse_seat)) {
        set_error(error, error_size,
                  "character seat socket has a singular transform");
        return 0;
    }
    definition_matrix(&pool->definition, package_transform);
    /* A character package defines its own seated origin. Cancelling that
     * socket before applying the author adjustment keeps root-motion clips and
     * differently-authored rigs attached to the vehicle origin. */
    matrix_multiply(package_transform, inverse_seat, anchored_transform);
    for (primitive_index = 0u;
         primitive_index < pool->render.gpu.primitive_count;
         primitive_index++) {
        MdkrModernPrimitive primitive;
        struct GfxModernSkinnedDraw draw;
        const float *node_world;
        memset(&draw, 0, sizeof(draw));
        if (pool->render.gpu.primitives[primitive_index].lod != selected_lod) continue;
        (void)mdkr_modern_character_asset_primitive(
            &pool->asset, primitive_index, &primitive);
        node_world = mdkr_modern_pose_node_matrix(&slot->pose,
                                                  primitive.node, 0);
        if (node_world == NULL) {
            set_error(error, error_size, "character primitive node pose is unavailable");
            return 0;
        }
        draw.asset = &pool->render.gpu;
        draw.primitive = primitive_index;
        matrix_multiply(anchored_transform, node_world, draw.model_matrix);
        if (!matrix_normal_transform(draw.model_matrix, draw.normal_matrix)) {
            set_error(error, error_size,
                      "character primitive has a singular normal transform");
            return 0;
        }
        draw.light_direction[0] = 0.35f;
        draw.light_direction[1] = -0.85f;
        draw.light_direction[2] = 0.38f;
        draw.ambient = 0.32f;
        if (primitive.skin >= 0) {
            MdkrModernSkin skin;
            (void)mdkr_modern_character_asset_skin(
                &pool->asset, (uint32_t)primitive.skin, &skin);
            if (skin.joint_count > MODERN_RUNTIME_MAX_BONES ||
                !mdkr_modern_pose_skin_palette(
                    &slot->pose, (uint32_t)primitive.skin, primitive.node, 0,
                    slot->palette, MODERN_RUNTIME_MAX_BONES,
                    error, error_size)) return 0;
            draw.bone_matrices = slot->palette;
            draw.bone_count = skin.joint_count;
        }
        slot->tokens[emitted] =
            gfx_modern_character_register_draw(&draw);
        if (slot->tokens[emitted] == 0u) {
            set_error(error, error_size, "renderer refused a modern character command");
            return 0;
        }
        emitted++;
    }
    if (emitted == 0u) {
        set_error(error, error_size, "selected character LOD contains no primitives");
        return 0;
    }
    for (primitive_index = 0u; primitive_index < emitted; primitive_index++) {
        gMoveWd((*display_list)++, G_MW_DKR_MODERN_CHARACTER, 0,
                slot->tokens[primitive_index]);
    }
    s_replacement_draws++;
    s_replacement_primitives += emitted;
    set_error(error, error_size, "");
    return 1;
}
