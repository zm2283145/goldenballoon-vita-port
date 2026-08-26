#include "modern_character_asset.h"
#include "modern_character_registry.h"
#include "modern_character_install.h"
#include "modern_character_pose.h"
#include "modern_character_render.h"
#include "modern_character_runtime.h"
#include "modern_character_donor.h"
#include "fs_utf8.h"
#include "fast3d/gfx_pc_dkr.h"
#include "asset_enums.h"

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
static float last_model_matrix[16];

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
        require(isfinite(draw->previous_model_matrix[component]),
                "runtime previous model transform is finite");
    }
    if (draw->bone_count != 0u) {
        require(draw->bone_matrices != NULL &&
                    draw->previous_bone_matrices != NULL,
                "runtime retains both immutable skinning endpoints");
    }
    memcpy(last_model_matrix, draw->model_matrix, sizeof(last_model_matrix));
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

static void test_retained_pose_interpolation(void) {
    struct GfxModernSkinnedAsset asset;
    struct GfxModernPrimitive primitive;
    struct GfxModernSkinnedDraw retained;
    struct GfxModernSkinnedDraw resolved;
    float previous_bone[16] = {0};
    float current_bone[16] = {0};
    float scratch[16];
    unsigned diagonal;
    memset(&asset, 0, sizeof(asset));
    memset(&primitive, 0, sizeof(primitive));
    memset(&retained, 0, sizeof(retained));
    asset.primitives = &primitive;
    asset.primitive_count = 1u;
    retained.asset = &asset;
    retained.bone_count = 1u;
    retained.previous_bone_matrices = previous_bone;
    retained.bone_matrices = current_bone;
    for (diagonal = 0u; diagonal < 4u; diagonal++) {
        unsigned component = diagonal * 5u;
        retained.previous_model_matrix[component] = 1.0f;
        retained.model_matrix[component] = 1.0f;
        previous_bone[component] = 1.0f;
        current_bone[component] = 1.0f;
    }
    retained.previous_model_matrix[12] = 2.0f;
    retained.model_matrix[12] = 10.0f;
    previous_bone[13] = -4.0f;
    current_bone[13] = 8.0f;
    require(mdkr_modern_render_resolve_draw(
                &retained, 0u, 1u, &resolved, scratch, 1u),
            "resolve previous retained animation endpoint");
    require(memcmp(resolved.model_matrix, retained.previous_model_matrix,
                   sizeof(retained.model_matrix)) == 0 &&
                memcmp(resolved.bone_matrices, previous_bone,
                       sizeof(previous_bone)) == 0,
            "alpha zero preserves exact previous pose bits");
    require(mdkr_modern_render_resolve_draw(
                &retained, 1u, 2u, &resolved, scratch, 1u) &&
                resolved.model_matrix[12] == 6.0f &&
                resolved.bone_matrices[13] == 2.0f,
            "retained pose resolves model and bones at rational midpoint");
    require(mdkr_modern_render_resolve_draw(
                &retained, 1u, 1u, &resolved, scratch, 1u),
            "resolve current retained animation endpoint");
    require(memcmp(resolved.model_matrix, retained.model_matrix,
                   sizeof(retained.model_matrix)) == 0 &&
                memcmp(resolved.bone_matrices, current_bone,
                       sizeof(current_bone)) == 0,
            "alpha one preserves exact current pose bits");
}

int main(int argc, char **argv) {
    MdkrModernCharacterAsset asset;
    MdkrModernCharacterAsset refused;
    MdkrModernCharacterDefinition definition;
    MdkrModernCharacterStats stats;
    MdkrModernCharacterTuning tuning;
    MdkrModernAnimation animation;
    MdkrModernChannel channel;
    MdkrModernKey key;
    MdkrModernSocket socket;
    MdkrModernAttachment attachment;
    MdkrModernCalibration calibration;
    MdkrModernIdentity identity;
    MdkrModernCharacterIdentityView identity_view;
    const uint8_t *portrait_data;
    MdkrModernCharacterRegistry registry;
    MdkrModernCharacterInstallResult install_result;
    MdkrModernPose pose;
    MdkrModernRenderAsset render;
    float socket_matrix[16];
    float palette[32];
    char error[256];
    unsigned char *bytes;
    size_t size;
    Gfx commands[8];
    Gfx *command_cursor = commands;
    char import_lock[4096];
    char prefix_witness[4096];
    FILE *lock_file;
    int player;
    float select_model_y;

    require(argc == 8,
            "usage: test_modern_character_asset <generated.mdkc> <directory> <source.mdkrchar> <portable.mdkrchar> <install-directory> <corrupt-portable.mdkrchar> <mismatched-portable.mdkrchar>");
    test_retained_pose_interpolation();
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
    {
        static const struct {
            int car_model, car_v, car_t, car_b;
            int hover_model, hover_v, hover_t, hover_b;
            int plane_model, plane_v, plane_t, plane_b;
            int select_model, select_v, select_t, select_b;
        } donors[10] = {
            {ASSET_OBJECTMODEL_KREMCAR_0,309,240,29, ASSET_OBJECTMODEL_KREMLINHOVER_0,323,237,30, ASSET_OBJECTMODEL_KREMPLANE_0,342,256,32, ASSET_OBJECTMODEL_KREMSELECT,330,252,27},
            {ASSET_OBJECTMODEL_BADGERCAR_0,347,261,32, ASSET_OBJECTMODEL_BADGERHOVER_0,360,254,33, ASSET_OBJECTMODEL_BADGERPLANE_0,370,273,34, ASSET_OBJECTMODEL_BADGERSELECT,325,249,27},
            {ASSET_OBJECTMODEL_TORTCAR_0,317,244,28, ASSET_OBJECTMODEL_TORTHOVER_0,328,235,29, ASSET_OBJECTMODEL_TORTPLANE_0,336,256,30, ASSET_OBJECTMODEL_TORTSELECT,315,244,23},
            {ASSET_OBJECTMODEL_CONKACAR_0,329,259,27, ASSET_OBJECTMODEL_CONKAHOVER_0,334,249,27, ASSET_OBJECTMODEL_CONKA_0,355,270,29, ASSET_OBJECTMODEL_CONKSELECT,306,245,24},
            {ASSET_OBJECTMODEL_TIGERCAR_0,367,258,36, ASSET_OBJECTMODEL_TIGERHOVER_0,363,243,35, ASSET_OBJECTMODEL_TIGPLANE_0,390,268,38, ASSET_OBJECTMODEL_TIGERSELECT,367,268,33},
            {ASSET_OBJECTMODEL_BANJOCAR_0,315,247,32, ASSET_OBJECTMODEL_BANJOHOVER_0,323,240,32, ASSET_OBJECTMODEL_BANJOPLANE_0,342,259,34, ASSET_OBJECTMODEL_BANJOSELECT,308,249,28},
            {ASSET_OBJECTMODEL_CHICKENCAR_0,368,245,37, ASSET_OBJECTMODEL_CHICKENHOVER_0,371,235,36, ASSET_OBJECTMODEL_CHICKENPLANE_0,399,259,39, ASSET_OBJECTMODEL_CHICKSELECT,343,238,32},
            {ASSET_OBJECTMODEL_MOUSECAR_0,284,234,25, ASSET_OBJECTMODEL_MOUSEHOVER_0,287,225,24, ASSET_OBJECTMODEL_MOUSEPLANE_0,306,244,26, ASSET_OBJECTMODEL_MOUSESELECT,287,235,24},
            {ASSET_OBJECTMODEL_SWCAR_0,303,226,25, ASSET_OBJECTMODEL_TICKTOCKHOVER_0,299,215,25, ASSET_OBJECTMODEL_TICKTOCKPLANE_0,323,237,26, ASSET_OBJECTMODEL_STOPWATCHSELECT,355,306,26},
            {ASSET_OBJECTMODEL_DIDDYCAR_0,325,257,30, ASSET_OBJECTMODEL_DIDDYHOVER_0,329,248,30, ASSET_OBJECTMODEL_DIDDYPLANE_0,348,267,32, ASSET_OBJECTMODEL_DIDDYSELECT,343,297,28},
        };
        int donor;
        for (donor = 0; donor < 10; donor++) {
            require(mdkr_modern_donor_qualified(donor),
                    "every retail gameplay donor is qualified");
            require(mdkr_modern_donor_model_ready(
                        donor, 0, donors[donor].car_model, 0,
                        donors[donor].car_v, donors[donor].car_t,
                        donors[donor].car_b) &&
                    mdkr_modern_donor_model_ready(
                        donor, 1, donors[donor].hover_model, 0,
                        donors[donor].hover_v, donors[donor].hover_t,
                        donors[donor].hover_b) &&
                    mdkr_modern_donor_model_ready(
                        donor, 2, donors[donor].plane_model, 0,
                        donors[donor].plane_v, donors[donor].plane_t,
                        donors[donor].plane_b),
                    "each donor's three vehicle schemas are exact");
            require(mdkr_modern_donor_select_model_ready(
                        donor, donors[donor].select_model,
                        donors[donor].select_v, donors[donor].select_t,
                        donors[donor].select_b),
                    "each donor's character-select schema is exact");
        }
        require(!mdkr_modern_donor_qualified(-1) &&
                    !mdkr_modern_donor_qualified(10),
                "out-of-range donors fail visible");
    }
    require(!mdkr_modern_donor_model_ready(
                9, 0, ASSET_OBJECTMODEL_DIDDYCAR_0, 0, 326, 257, 30),
            "changed donor geometry fails visible");
    require(!mdkr_modern_donor_model_ready(
                0, 0, ASSET_OBJECTMODEL_KREMCAR_0, 0, 310, 240, 29),
            "changed non-Diddy geometry also fails visible");
    require(mdkr_modern_donor_select_model_ready(
                9, ASSET_OBJECTMODEL_DIDDYSELECT, 343, 297, 28) &&
                !mdkr_modern_donor_select_model_ready(
                    9, ASSET_OBJECTMODEL_DIDDYSELECT, 344, 297, 28),
            "Diddy select actor requires its exact qualified fingerprint");
    require(mdkr_modern_donor_select_batch_visible(9, 0) &&
                !mdkr_modern_donor_select_batch_visible(9, 1) &&
                mdkr_modern_donor_select_batch_visible(0, 0) &&
                !mdkr_modern_donor_select_batch_visible(0, 1),
            "every select replacement retains only its player placard");
    require(!mdkr_modern_donor_batch_visible(9, 0, 0, 0) &&
                mdkr_modern_donor_batch_visible(9, 0, 0, 18) &&
                !mdkr_modern_donor_batch_visible(9, 0, 0, 27),
            "Diddy driver mask retains vehicle batches");
    require(!mdkr_modern_donor_batch_visible(0, 1, 0, 17) &&
                !mdkr_modern_donor_batch_visible(1, 0, 0, 2) &&
                !mdkr_modern_donor_batch_visible(2, 0, 0, 11) &&
                !mdkr_modern_donor_batch_visible(4, 0, 0, 20) &&
                !mdkr_modern_donor_batch_visible(5, 0, 0, 21) &&
                !mdkr_modern_donor_batch_visible(6, 0, 0, 27),
            "LOD-specific character materials remain in donor masks");
    require(!mdkr_modern_donor_batch_visible(6, 1, 0, 33) &&
                mdkr_modern_donor_batch_visible(6, 1, 0, 35),
            "64-bit donor masks preserve Drumstick batches above bit 31");
    require(mdkr_modern_donor_cap_lod(9, 0, 5) == 4 &&
                mdkr_modern_donor_cap_lod(0, 0, 5) == 4 &&
                mdkr_modern_donor_cap_lod(10, 0, 5) == 5,
            "all qualified donors avoid their collapsed far LOD");
    {
        const float donor_min[3] = {-50.0f, 1.0f, -116.0f};
        const float donor_max[3] = {50.0f, 179.0f, 140.0f};
        float fit[16];
        require(mdkr_modern_donor_fit_frame(
                    9, MDKR_CHARACTER_CONTEXT_SELECT,
                    donor_min, donor_max, 1.0f, 1.25f, fit) &&
                    fit[0] > 177.99f && fit[0] < 178.01f &&
                    fit[12] == 0.0f && fit[13] == 1.0f &&
                    fit[14] == 12.0f,
                "select fit converts meters and lands on measured ground");
        require(mdkr_modern_donor_fit_frame(
                    9, MDKR_CHARACTER_CONTEXT_CAR,
                    donor_min, donor_max, 1.0f, 1.25f, fit) &&
                    fit[0] > 177.99f && fit[0] < 178.01f &&
                    fit[12] == 0.0f && fit[13] == 0.0f &&
                    fit[14] == 0.0f,
                "vehicle fit retains its independent qualified seat frame");
        require(!mdkr_modern_donor_fit_frame(
                    9, MDKR_CHARACTER_CONTEXT_SELECT,
                    donor_min, donor_max, 0.0f, 1.25f, fit),
                "fit refuses an invalid normalized source height");
    }

    mdkr_modern_character_asset_stats(&asset, &stats);
    require(stats.vertices == 3u && stats.triangles == 1u &&
                stats.primitives == 1u && stats.lod_levels == 1u &&
                stats.materials == 1u,
            "compiled geometry statistics");
    require(stats.nodes == 3u && stats.skins == 1u && stats.joints == 2u,
            "compiled rig statistics");
    require(stats.animations == 1u && stats.animation_channels == 1u &&
                stats.animation_keys == 2u,
            "compiled animation statistics");
    require(stats.semantics == 2u && stats.sockets == 2u,
            "compiled presentation mapping statistics");
    require(stats.encoded_texture_bytes > 64u &&
                stats.decoded_texture_bytes == 4u,
            "compiled stats expose exact encoded and decoded texture cost");
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
    require(mdkr_modern_character_asset_calibration(&asset, &calibration) &&
                calibration.source_height > 0.99f &&
                calibration.source_height < 1.01f &&
                calibration.normalized_height > 0.99f &&
                calibration.normalized_height < 1.01f &&
                calibration.target_height > 1.24f &&
                calibration.target_height < 1.26f &&
                (calibration.flags & 1u) != 0u &&
                mdkr_modern_character_asset_attachment(&asset, 0u,
                                                       &attachment) &&
                attachment.context == MDKR_CHARACTER_CONTEXT_SELECT &&
                (attachment.flags & 1u) != 0u,
            "read explicit height, ground, facing, and context calibration");
    require(mdkr_modern_character_asset_identity(
                &asset, &identity, &portrait_data) && portrait_data != NULL &&
                identity.portrait_mime == 1u && identity.portrait_size > 64u &&
                (identity.minimap_rgba & 0xFFFFFFu) == 0x9048DCu,
            "read validated source-v3 identity media and minimap colour");
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
    require(mdkr_modern_character_registry_skipped(&registry) == 3,
            "corrupt, undecodable-identity, and duplicate caches are diagnosed");
    require(mdkr_modern_character_registry_find(
                &registry, "org.example.pipeline-proof") == 0,
            "registry lookup by stable package id");
    require((registry.entries[0].semantic_mask &
             MDKR_CHARACTER_SEMANTIC_FALLBACK) != 0u &&
                (registry.entries[0].moving_semantic_mask &
                 MDKR_CHARACTER_SEMANTIC_FALLBACK) != 0u &&
                (registry.entries[0].socket_mask &
                 MDKR_CHARACTER_SOCKET_SEAT) != 0u &&
                registry.entries[0].attachment_context_mask == 15u &&
                (registry.entries[0].calibration_flags & 1u) != 0u &&
                registry.entries[0].normalized_height > 0.99f &&
                registry.entries[0].normalized_height < 1.01f &&
                registry.entries[0].target_height > 1.24f &&
                registry.entries[0].target_height < 1.26f &&
                registry.entries[0].identity_flags == 1u &&
                registry.entries[0].portrait_bytes > 64u &&
                registry.entries[0].portrait_rgba[3] != 0u &&
                registry.entries[0].lod_vertices[0] == 3u &&
                registry.entries[0].lod_triangles[0] == 1u &&
                registry.entries[0].lod_primitives[0] == 1u &&
                registry.entries[0].lod_palette_matrices[0] == 2u &&
                (registry.entries[0].minimap_rgba & 0xFFFFFFu) == 0x9048DCu,
            "registry summarizes per-LOD authoring health and identity preview");
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
    require(mdkr_modern_pose_has_semantic(&pose, "idle") &&
                !mdkr_modern_pose_has_semantic(&pose, "race.steer"),
            "pose distinguishes an explicit semantic from fallback");
    require(mdkr_modern_pose_advance_phase(
                &pose, 0.0f, 0.0f, error, sizeof(error)) &&
                mdkr_modern_pose_socket_matrix(
                    &pose, "head", 0, socket_matrix) &&
                socket_matrix[0] > 0.99f,
            "normalized phase zero samples the left endpoint exactly");
    require(mdkr_modern_pose_advance_phase(
                &pose, 0.0f, 1.0f, error, sizeof(error)) &&
                mdkr_modern_pose_socket_matrix(
                    &pose, "head", 0, socket_matrix) &&
                socket_matrix[0] > 0.70f && socket_matrix[0] < 0.72f,
            "normalized phase one samples the right endpoint exactly");
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

    require(!mdkr_modern_character_install_portable(
                argv[3], argv[5], &install_result) &&
                install_result.needs_compiler,
            "native importer identifies a valid source-only package without publishing it");
    require(snprintf(import_lock, sizeof(import_lock),
                     "%s/.character-import.lock", argv[5]) > 0,
            "construct native import lock path");
    lock_file = mdkr_fopen_utf8(import_lock, "wb");
    require(lock_file != NULL && fclose(lock_file) == 0,
            "create an active-import witness lock");
    require(!mdkr_modern_character_install_portable(
                argv[4], argv[5], &install_result),
            "native importer respects an existing cross-tool lock");
    lock_file = mdkr_fopen_utf8(import_lock, "rb");
    require(lock_file != NULL && fclose(lock_file) == 0,
            "refused native import never removes another owner's lock");
    require(mdkr_remove_utf8(import_lock) == 0,
            "retire the import witness lock");
    require(!mdkr_modern_character_install_portable(
                argv[6], argv[5], &install_result) &&
                strstr(install_result.message, "checksum") != NULL,
            "native portable import rejects a corrupt embedded cache before publication");
    require(!mdkr_modern_character_install_portable(
                argv[7], argv[5], &install_result) &&
                strstr(install_result.message, "does not match") != NULL,
            "native portable import binds a valid cache to its exact source members");
    require(mdkr_modern_character_install_portable(
                argv[4], argv[5], &install_result),
            install_result.message);
    require(strcmp(install_result.id, "org.example.pipeline-proof") == 0,
            "native portable import publishes the compiled package identity");
    require(mdkr_modern_character_registry_init(&registry, argv[5]) == 0 &&
                mdkr_modern_character_registry_count(&registry) == 1,
            "native portable import is immediately discoverable");
    mdkr_modern_character_registry_shutdown(&registry);
    require(snprintf(prefix_witness, sizeof(prefix_witness),
                     "%s/org.example.pipeline-proof.other.%064x.json",
                     argv[5], 0) > 0,
            "construct prefix-collision provenance witness");
    lock_file = mdkr_fopen_utf8(prefix_witness, "wb");
    require(lock_file != NULL && fputs("unrelated prefix package\n", lock_file) >= 0 &&
                fclose(lock_file) == 0,
            "create unrelated longer-id provenance witness");
    require(mdkr_modern_character_remove_installed(
                "org.example.pipeline-proof", argv[5], &install_result),
            install_result.message);
    require(mdkr_modern_character_registry_init(&registry, argv[5]) == 0 &&
                mdkr_modern_character_registry_count(&registry) == 0,
            "native removal retires the cache and retained package source");
    mdkr_modern_character_registry_shutdown(&registry);
    lock_file = mdkr_fopen_utf8(prefix_witness, "rb");
    require(lock_file != NULL && fclose(lock_file) == 0,
            "native removal preserves provenance for a longer package id");
    require(mdkr_remove_utf8(prefix_witness) == 0,
            "retire prefix-collision provenance witness");

    require(mdkr_modern_characters_init(argv[2]),
            "initialize process-level character runtime");
    require(mdkr_modern_character_assign_player(
                0, "org.example.pipeline-proof", error, sizeof(error)),
            error);
    require(mdkr_modern_character_matches(0, 9, 0),
            "runtime assignment retains donor and vehicle characteristics");
    require(mdkr_modern_character_player_identity(0, &identity_view) &&
                strcmp(identity_view.display_name, "Pipeline Proof") == 0 &&
                identity_view.portrait_rgba != NULL &&
                identity_view.portrait_width == 40u &&
                identity_view.portrait_height == 40u &&
                identity_view.portrait_stride == 160u &&
                identity_view.minimap_rgba[0] == 220u &&
                identity_view.minimap_rgba[1] == 72u &&
                identity_view.minimap_rgba[2] == 144u &&
                identity_view.minimap_rgba[3] == 255u &&
                identity_view.revision != 0u,
            "runtime publishes one decoded game-ready identity view");
    mdkr_modern_character_tuning_defaults(&tuning);
    tuning.scale = 1.5f;
    tuning.translation[0] = 12.0f;
    tuning.rotation_degrees[1] = 15.0f;
    tuning.animation_speed = 0.5f;
    tuning.lod_bias = 1.0f;
    tuning.vehicle_mask = 1u;
    tuning.context[MDKR_CHARACTER_CONTEXT_SELECT].translation[1] = 0.75f;
    require(mdkr_modern_character_set_tuning(0, &tuning,
                                              error, sizeof(error)),
            error);
    memset(&tuning, 0, sizeof(tuning));
    require(mdkr_modern_character_get_tuning(0, &tuning) &&
                tuning.scale == 1.5f && tuning.vehicle_mask == 1u,
            "runtime retains bounded presentation-only tuning");
    require(mdkr_modern_character_matches(0, 9, 0) &&
                !mdkr_modern_character_matches(0, 9, 1),
            "player vehicle pairing narrows package-qualified bodies");
    tuning.scale = 9.0f;
    require(!mdkr_modern_character_set_tuning(0, &tuning,
                                               error, sizeof(error)),
            "unsafe editor tuning fails closed");
    tuning.scale = 1.5f;
    require(mdkr_modern_character_tick(0, "race.boost", 0.25f,
                                       error, sizeof(error)),
            "runtime semantic uses package fallback when optional state is absent");
    require(mdkr_modern_character_tick_phase(
                0, "race.steer", 0.25f, 1.0f,
                error, sizeof(error)),
            "missing phase-driven semantic advances fallback instead of scrubbing it");
    require(mdkr_modern_character_emit(0, MDKR_CHARACTER_CONTEXT_SELECT,
                                       NULL, 0.0f, &command_cursor,
                                       error, sizeof(error)),
            error);
    select_model_y = last_model_matrix[13];
    require(mdkr_modern_character_emit(0, MDKR_CHARACTER_CONTEXT_CAR,
                                       NULL, 0.0f, &command_cursor,
                                       error, sizeof(error)),
            error);
    require(command_cursor == commands + 2 && registered_draws == 2u,
            "runtime emits one retained command per selected primitive");
    require(select_model_y - last_model_matrix[13] > 0.70f,
            "select ground correction is independent from the car seat frame");
    require(fabsf(last_model_matrix[12]) > 1.0f,
            "runtime draw includes the player seat-offset adjustment");
    require(commands[0].words.w1 == 1u && commands[1].words.w1 == 2u,
            "display list embeds immutable draw token rather than a pointer");
    for (player = 1; player < MDKR_MODERN_CHARACTER_PLAYERS; player++) {
        require(mdkr_modern_character_assign_player(
                    player, "org.example.pipeline-proof",
                    error, sizeof(error)),
                "all four local player slots share one installed package");
        require(mdkr_modern_character_matches(player, 9, 0) &&
                    mdkr_modern_character_tick(
                        player, "select.confirm", 0.1f,
                        error, sizeof(error)),
                "each local player owns an independent semantic pose");
    }
    require(mdkr_modern_character_emit(3, MDKR_CHARACTER_CONTEXT_CAR,
                                       NULL, 0.0f, &command_cursor,
                                       error, sizeof(error)) &&
                command_cursor == commands + 3 && registered_draws == 3u,
            "four-player assignment reuses GPU ownership and emits independently");
    mdkr_modern_characters_shutdown();
    require(released_assets == 1u,
            "runtime retires GPU ownership before freeing CPU asset bytes");

    puts("PASS: compiled modern character cache and retained runtime validate and fail closed");
    return 0;
}
