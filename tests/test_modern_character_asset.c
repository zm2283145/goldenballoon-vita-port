#include "modern_character_asset.h"
#include "modern_character_registry.h"
#include "modern_character_pose.h"
#include "modern_character_render.h"
#include "modern_character_runtime.h"
#include "fast3d/gfx_pc_dkr.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void require(int condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        exit(1);
    }
}

static uint32_t registered_draws;
static uint32_t released_assets;

bool gfx_modern_character_supported(void) {
    return true;
}

uint32_t gfx_modern_character_register_draw(
    const struct GfxModernSkinnedDraw *draw) {
    uint32_t component;
    require(draw != NULL && draw->asset != NULL,
            "runtime registers a complete renderer draw");
    require(draw->primitive < draw->asset->primitive_count,
            "runtime primitive is in range");
    require(draw->bone_count <= 128u,
            "runtime obeys the GPU palette bound");
    for (component = 0u; component < 16u; component++) {
        require(isfinite(draw->model_matrix[component]),
                "runtime model transform is finite");
        require(isfinite(draw->normal_matrix[component]),
                "runtime normal transform is finite");
    }
    return ++registered_draws;
}

void gfx_modern_character_release_asset(uint64_t asset_id) {
    require(asset_id != 0u, "runtime releases a stable renderer asset id");
    released_assets++;
}

static unsigned char *read_file(const char *path, size_t *out_size) {
    FILE *file = fopen(path, "rb");
    long end;
    unsigned char *bytes;
    require(file != NULL, "open generated cache");
    require(fseek(file, 0, SEEK_END) == 0, "seek generated cache");
    end = ftell(file);
    require(end > 0 && fseek(file, 0, SEEK_SET) == 0, "size generated cache");
    bytes = (unsigned char *)malloc((size_t)end);
    require(bytes != NULL, "allocate generated cache copy");
    require(fread(bytes, 1u, (size_t)end, file) == (size_t)end,
            "read generated cache");
    require(fclose(file) == 0, "close generated cache");
    *out_size = (size_t)end;
    return bytes;
}

int main(int argc, char **argv) {
    MdkrModernCharacterAsset asset;
    MdkrModernCharacterAsset refused;
    MdkrModernCharacterDefinition definition;
    MdkrModernCharacterStats stats;
    MdkrModernAnimation animation;
    MdkrModernChannel channel;
    MdkrModernKey key;
    MdkrModernSocket socket;
    MdkrModernCharacterRegistry registry;
    MdkrModernPose pose;
    MdkrModernRenderAsset render;
    float socket_matrix[16];
    float palette[32];
    char error[256];
    unsigned char *bytes;
    size_t size;
    Gfx commands[8];
    Gfx *command_cursor = commands;

    require(argc == 3,
            "usage: test_modern_character_asset <generated.mdkc> <directory>");
    require(mdkr_modern_character_asset_load_file(argv[1], &asset,
                                                   error, sizeof(error)),
            error);
    require(mdkr_modern_character_asset_definition(&asset, &definition),
            "read character definition");
    require(strcmp(mdkr_modern_character_asset_string(&asset, definition.id),
                   "org.example.pipeline-proof") == 0,
            "compiled character id");
    require(strcmp(mdkr_modern_character_asset_string(
                       &asset, definition.display_name),
                   "Pipeline Proof") == 0,
            "compiled display name");
    require(definition.donor == 9u && definition.vehicle_mask == 7u,
            "donor and vehicle characteristics");

    mdkr_modern_character_asset_stats(&asset, &stats);
    require(stats.vertices == 3u && stats.triangles == 1u &&
                stats.primitives == 1u && stats.materials == 1u,
            "compiled geometry statistics");
    require(stats.nodes == 3u && stats.skins == 1u && stats.joints == 2u,
            "compiled rig statistics");
    require(stats.animations == 1u && stats.animation_channels == 1u &&
                stats.animation_keys == 2u,
            "compiled animation statistics");
    require(stats.semantics == 2u && stats.sockets == 2u,
            "compiled presentation mapping statistics");
    require(mdkr_modern_character_asset_animation(&asset, 0u, &animation) &&
                animation.duration == 1.0f,
            "read compiled animation");
    require(mdkr_modern_character_asset_channel(&asset, 0u, &channel) &&
                channel.path == 1u && channel.components == 4u,
            "read compiled rotation channel");
    require(mdkr_modern_character_asset_key(&asset, 1u, &key) &&
                key.time == 1.0f && key.value[3] > 0.92f,
            "read compiled animation endpoint");
    require(mdkr_modern_character_asset_socket(&asset, 0u, &socket) &&
                mdkr_modern_character_asset_string(&asset, socket.semantic) != NULL,
            "read compiled socket");
    mdkr_modern_character_asset_unload(&asset);
    mdkr_modern_character_asset_unload(&asset);

    bytes = read_file(argv[1], &size);
    bytes[size - 1u] ^= 0x80u;
    require(!mdkr_modern_character_asset_load_memory(bytes, size, &refused,
                                                      error, sizeof(error)) &&
                strstr(error, "checksum") != NULL,
            "payload corruption is rejected before publication");
    free(bytes);

    bytes = read_file(argv[1], &size);
    bytes[12] ^= 1u; /* declared file length */
    require(!mdkr_modern_character_asset_load_memory(bytes, size, &refused,
                                                      error, sizeof(error)) &&
                strstr(error, "header") != NULL,
            "header corruption is rejected before publication");
    free(bytes);

    require(mdkr_modern_character_registry_init(&registry, argv[2]) == 0,
            "scan generated character directory");
    require(mdkr_modern_character_registry_count(&registry) == 1,
            "duplicate package identity is collapsed");
    require(mdkr_modern_character_registry_skipped(&registry) == 2,
            "corrupt and duplicate caches are diagnosed");
    require(mdkr_modern_character_registry_find(
                &registry, "org.example.pipeline-proof") == 0,
            "registry lookup by stable package id");
    require(mdkr_modern_character_registry_load(&registry, 0, &asset,
                                                 error, sizeof(error)),
            "load selected registry character");
    require(mdkr_modern_pose_init(&pose, &asset, error, sizeof(error)),
            "initialize semantic skeletal pose");
    require(mdkr_modern_pose_advance(&pose, 0.5f, error, sizeof(error)),
            "advance semantic skeletal pose");
    require(mdkr_modern_pose_socket_matrix(&pose, "head", 0, socket_matrix),
            "resolve animated head socket");
    require(socket_matrix[0] > 0.90f && socket_matrix[0] < 0.95f,
            "half-time quaternion sampling reaches the expected angle");
    require(mdkr_modern_pose_skin_palette(&pose, 0u, 2u, 0,
                                           palette, 2u,
                                           error, sizeof(error)),
            "build mesh-relative GPU skin palette");
    require(isfinite(palette[0]) && isfinite(palette[31]),
            "skin palette contains finite matrices");
    require(mdkr_modern_render_asset_init(&render, &asset,
                                          error, sizeof(error)),
            "build immutable renderer asset");
    require(render.gpu.vertex_count == 3u && render.gpu.index_count == 3u &&
                render.gpu.primitive_count == 1u &&
                sizeof(*render.gpu.vertices) == 72u,
            "renderer receives high-range indexed skinned geometry");
    require(render.gpu.texture_count == 1u &&
                render.decoded_texture_bytes == 4u &&
                render.gpu.textures[0].level_count == 1,
            "renderer decodes bounded embedded PNG texture ownership");
    mdkr_modern_render_asset_shutdown(&render);
    mdkr_modern_pose_shutdown(&pose);
    mdkr_modern_character_asset_unload(&asset);
    mdkr_modern_character_registry_shutdown(&registry);

    require(mdkr_modern_characters_init(argv[2]),
            "initialize process-level character runtime");
    require(mdkr_modern_character_assign_player(
                0, "org.example.pipeline-proof", error, sizeof(error)),
            error);
    require(mdkr_modern_character_matches(0, 9, 0),
            "runtime assignment retains donor and vehicle characteristics");
    require(mdkr_modern_character_tick(0, "race.boost", 0.25f,
                                       error, sizeof(error)),
            "runtime semantic uses package fallback when optional state is absent");
    require(mdkr_modern_character_emit(0, 0.0f, &command_cursor,
                                       error, sizeof(error)),
            error);
    require(command_cursor == commands + 1 && registered_draws == 1u,
            "runtime emits one retained command per selected primitive");
    require(commands[0].words.w1 == 1u,
            "display list embeds immutable draw token rather than a pointer");
    mdkr_modern_characters_shutdown();
    require(released_assets == 1u,
            "runtime retires GPU ownership before freeing CPU asset bytes");

    puts("PASS: compiled modern character cache and retained runtime validate and fail closed");
    return 0;
}
