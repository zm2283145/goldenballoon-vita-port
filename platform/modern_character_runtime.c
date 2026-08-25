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

int mdkr_modern_character_emit(int player, Gfx **display_list,
                               char *error, size_t error_size) {
    MdkrModernRuntimePlayer *slot;
    MdkrModernRuntimePool *pool;
    float package_transform[16];
    uint32_t primitive_index;
    if (player < 0 || player >= MDKR_MODERN_CHARACTER_PLAYERS ||
        display_list == NULL || *display_list == NULL ||
        (slot = &s_players[player])->pool < 0 ||
        !gfx_modern_character_supported()) {
        set_error(error, error_size, "modern character draw is unavailable");
        return 0;
    }
    pool = &s_pools[slot->pool];
    definition_matrix(&pool->definition, package_transform);
    for (primitive_index = 0u;
         primitive_index < pool->render.gpu.primitive_count;
         primitive_index++) {
        MdkrModernPrimitive primitive;
        struct GfxModernSkinnedDraw draw;
        const float *node_world;
        memset(&draw, 0, sizeof(draw));
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
        matrix_multiply(package_transform, node_world, draw.model_matrix);
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
        slot->tokens[primitive_index] =
            gfx_modern_character_register_draw(&draw);
        if (slot->tokens[primitive_index] == 0u) {
            set_error(error, error_size, "renderer refused a modern character command");
            return 0;
        }
    }
    for (primitive_index = 0u;
         primitive_index < pool->render.gpu.primitive_count;
         primitive_index++) {
        gMoveWd((*display_list)++, G_MW_DKR_MODERN_CHARACTER, 0,
                slot->tokens[primitive_index]);
    }
    set_error(error, error_size, "");
    return 1;
}
