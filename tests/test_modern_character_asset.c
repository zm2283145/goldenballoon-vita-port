#include "modern_character_asset.h"
#include "modern_character_registry.h"
#include "modern_character_install.h"
#include "modern_character_pose.h"
#include "modern_character_render.h"
#include "modern_character_runtime.h"
#include "modern_character_donor.h"
#include "workshop_preview_runtime.h"
#include "fs_utf8.h"
#include "fast3d/gfx_pc_dkr.h"
#include "asset_enums.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TRANSACTION_FIXTURES 8

#if defined(_WIN32)
static int set_env(const char *name, const char *value) {
    return _putenv_s(name, value);
}
static int clear_env(const char *name) { return _putenv_s(name, ""); }
#else
static int set_env(const char *name, const char *value) {
    return setenv(name, value, 1);
}
static int clear_env(const char *name) { return unsetenv(name); }
#endif

static void require(int condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        exit(1);
    }
}

static uint32_t registered_draws;
static uint32_t released_assets;
static float last_model_matrix[16];
static bool modern_character_supported = true;

bool gfx_modern_character_supported(void) {
    return modern_character_supported;
}

uint32_t gfx_modern_character_register_draw(
    const struct GfxModernSkinnedDraw *draw) {
    uint32_t component;
    require(draw != NULL && draw->asset != NULL,
            "runtime registers a complete renderer draw");
    require(draw->primitive < draw->asset->primitive_count,
            "runtime primitive is in range");
    require(draw->bone_count <= 256u,
            "runtime obeys the GPU palette bound");
    require(draw->player < MDKR_MODERN_CHARACTER_PLAYERS,
            "runtime draw retains a bounded player owner");
    require(draw->view < MDKR_MODERN_CHARACTER_VIEWS,
            "runtime draw retains a bounded gameplay or cutscene view owner");
    for (component = 0u; component < 16u; component++) {
        require(isfinite(draw->target_frame_matrix[component]),
                "runtime target frame is finite");
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

static void write_file(const char *path, const unsigned char *bytes,
                       size_t size) {
    FILE *file = mdkr_fopen_utf8(path, "wb");
    require(file != NULL, "open runtime transaction fixture for writing");
    require(fwrite(bytes, 1u, size, file) == size,
            "write runtime transaction fixture");
    require(fclose(file) == 0, "close runtime transaction fixture");
}

static uint32_t read_u32_le(const unsigned char *bytes) {
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8u) |
           ((uint32_t)bytes[2] << 16u) | ((uint32_t)bytes[3] << 24u);
}

static uint64_t read_u64_le(const unsigned char *bytes) {
    return (uint64_t)read_u32_le(bytes) |
           ((uint64_t)read_u32_le(bytes + 4u) << 32u);
}

static void write_u32_le(unsigned char *bytes, uint32_t value) {
    bytes[0] = (unsigned char)value;
    bytes[1] = (unsigned char)(value >> 8u);
    bytes[2] = (unsigned char)(value >> 16u);
    bytes[3] = (unsigned char)(value >> 24u);
}

static uint32_t test_crc32(const unsigned char *bytes, size_t size) {
    uint32_t crc = 0xFFFFFFFFu;
    size_t index;
    for (index = 0u; index < size; index++) {
        uint32_t value = crc ^ bytes[index];
        unsigned bit;
        for (bit = 0u; bit < 8u; bit++) {
            value = (value >> 1u) ^
                    (0xEDB88320u & (0u - (value & 1u)));
        }
        crc = value;
    }
    return ~crc;
}

static unsigned char *section_payload(unsigned char *bytes, uint32_t kind) {
    uint32_t index;
    const uint32_t count = read_u32_le(bytes + 56u);
    for (index = 0u; index < count; index++) {
        unsigned char *entry = bytes + 64u + (size_t)index * 32u;
        if (read_u32_le(entry) == kind) {
            return bytes + (size_t)read_u64_le(entry + 8u);
        }
    }
    return NULL;
}

static void refresh_payload_crc(unsigned char *bytes, size_t size) {
    write_u32_le(bytes + 52u,
                 test_crc32(bytes + MDKR_MDKC_HEADER_BYTES,
                            size - MDKR_MDKC_HEADER_BYTES));
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
    MdkrModernRig rig;
    MdkrModernRigRole rig_role;
    MdkrModernProvenance provenance;
    MdkrModernCharacterIdentityView identity_view;
    MdkrModernCharacterRuntimeMetrics runtime_metrics;
    MdkrModernCharacterFitDiagnostics fit_diagnostics;
    MdkrModernCharacterContactDiagnostics contact_diagnostics;
    MdkrWorkshopPreviewVisualMetrics visual_metrics;
    const uint8_t *portrait_data;
    MdkrModernCharacterRegistry registry;
    MdkrModernCharacterInstallResult install_result;
    MdkrModernPose pose;
    MdkrModernRenderAsset render;
    float socket_matrix[16];
    float procedural_arm_left[16];
    float procedural_arm_right[16];
    float contact_offsets[MDKR_MODERN_CHARACTER_CONTACTS][3] = {{0}};
    float bind_position[3];
    int32_t parent_joint_node;
    float palette[256];
    char error[256];
    unsigned char *bytes;
    size_t size;
    Gfx commands[8];
    Gfx *command_cursor = commands;
    char import_lock[4096];
    char prefix_witness[4096];
    char deletion_failure_witness[4096];
    char transaction_cache[TRANSACTION_FIXTURES][4096];
    char transaction_source[4096];
    char reviewed_package_sha[65];
    char reviewed_source_digest[65];
    FILE *lock_file;
    int player;
    float select_model_y;
    float focus_center[3];
    float focus_radius;
    unsigned char *transaction_bytes[TRANSACTION_FIXTURES];
    size_t transaction_size[TRANSACTION_FIXTURES];
    int original_index;
    int transaction_index[TRANSACTION_FIXTURES];
    int fixture;
    int assignment_plan[MDKR_MODERN_CHARACTER_PLAYERS];
    uint64_t stable_identity_revision;
    uint64_t roster_identity_revision[MDKR_MODERN_CHARACTER_PLAYERS];

    require(argc == 11,
            "usage: test_modern_character_asset <generated.mdkc> <directory> <source.mdkrchar> <portable.mdkrchar> <install-directory> <corrupt-portable.mdkrchar> <mismatched-portable.mdkrchar> <legacy-portable.mdkrchar> <legacy-v5-portable.mdkrchar> <transaction-fixture-directory>");
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
    require(stats.nodes == 17u && stats.skins == 1u && stats.joints == 16u,
            "compiled rig statistics");
    require(stats.animations == 1u && stats.animation_channels == 1u &&
                stats.animation_keys == 2u,
            "compiled animation statistics");
    require(stats.semantics == 3u && stats.sockets == 2u &&
                stats.rig_roles == 16u,
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
            "read validated source-v4 identity media and minimap colour");
    require(mdkr_modern_character_asset_rig(&asset, &rig) &&
                rig.mode == MDKR_MODERN_RIG_HUMANOID_RETARGET_V1 &&
                rig.flags == MDKR_MODERN_RIG_REVIEWED &&
                rig.role_count == 16u && rig.role_mask == 0xFFFFu &&
                mdkr_modern_character_asset_rig_role(
                    &asset, 0u, &rig_role) && rig_role.node == 0u &&
                rig_role.flags == 0u && rig_role.confidence_milli == 1000u,
            "read bounded source-v4 rig role contract");
    require(mdkr_modern_character_asset_provenance(&asset, &provenance) &&
                provenance.flags ==
                    MDKR_MODERN_PROVENANCE_LICENSE_TEXT_BOUND &&
                strcmp(mdkr_modern_character_asset_string(
                           &asset, provenance.spdx), "CC0-1.0") == 0 &&
                strcmp(mdkr_modern_character_asset_string(
                           &asset, provenance.attribution),
                       "Generated MDKR test fixture") == 0 &&
                strcmp(mdkr_modern_character_asset_string(
                           &asset, provenance.source_url),
                       "https://example.invalid/pipeline-proof") == 0,
            "read authenticated human-review provenance from the cache");
    require(mdkr_modern_character_asset_node_bind_position(
                &asset, 3u, bind_position) &&
                fabsf(bind_position[0]) < 1.0e-6f &&
                fabsf(bind_position[1] - 1.75f) < 1.0e-6f &&
                fabsf(bind_position[2]) < 1.0e-6f,
            "bind-pose query composes the compiled humanoid hierarchy");
    require(mdkr_modern_character_asset_node_bind_position(
                &asset, 12u, bind_position) &&
                fabsf(bind_position[0] - 0.15f) < 1.0e-6f &&
                fabsf(bind_position[1]) < 1.0e-6f &&
                fabsf(bind_position[2] - 0.10f) < 1.0e-6f,
            "bind-pose query retains lateral and depth limb placement");
    require(mdkr_modern_character_asset_joint_parent_node(
                &asset, 6u, &parent_joint_node) &&
                parent_joint_node == 5 &&
                mdkr_modern_character_asset_joint_parent_node(
                    &asset, 0u, &parent_joint_node) &&
                parent_joint_node == -1,
            "rig view query resolves nearest joint ancestry and skin roots");
    require(!mdkr_modern_character_asset_node_bind_position(
                &asset, stats.nodes, bind_position) &&
                !mdkr_modern_character_asset_joint_parent_node(
                    &asset, stats.joints, &parent_joint_node),
            "rig view queries reject out-of-range nodes and joints");
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

    bytes = read_file(argv[1], &size);
    {
        unsigned char *strings = section_payload(bytes, MDKR_MDKC_STRINGS);
        unsigned char *provenance_bytes = section_payload(
            bytes, MDKR_MDKC_PROVENANCE);
        require(strings != NULL && provenance_bytes != NULL,
                "locate compiled provenance strings for mutation test");
        strings[read_u32_le(provenance_bytes)] = 0x01u;
        refresh_payload_crc(bytes, size);
        require(!mdkr_modern_character_asset_load_memory(
                    bytes, size, &refused, error, sizeof(error)) &&
                    strstr(error, "provenance") != NULL,
                "native admission rejects forged provenance control text");
    }
    free(bytes);

    bytes = read_file(argv[1], &size);
    {
        unsigned char *strings = section_payload(bytes, MDKR_MDKC_STRINGS);
        unsigned char *definition_bytes = section_payload(
            bytes, MDKR_MDKC_CHARACTER);
        require(strings != NULL && definition_bytes != NULL,
                "locate compiled character identity for mutation test");
        strings[read_u32_le(definition_bytes)] = 'O';
        refresh_payload_crc(bytes, size);
        require(!mdkr_modern_character_asset_load_memory(
                    bytes, size, &refused, error, sizeof(error)) &&
                    strstr(error, "definition") != NULL,
                "direct cache admission enforces the lowercase identity slug");
    }
    free(bytes);

    bytes = read_file(argv[1], &size);
    {
        unsigned char *role = section_payload(bytes, MDKR_MDKC_RIG_ROLES);
        require(role != NULL, "locate compiled rig roles for mutation test");
        write_u32_le(role + 4u, 0xFFFFFFFFu);
        refresh_payload_crc(bytes, size);
        require(!mdkr_modern_character_asset_load_memory(
                    bytes, size, &refused, error, sizeof(error)) &&
                    strstr(error, "rig role") != NULL,
                "native admission rejects an out-of-range rig joint");
    }
    free(bytes);

    bytes = read_file(argv[1], &size);
    {
        unsigned char *rig_bytes = section_payload(bytes, MDKR_MDKC_RIG);
        require(rig_bytes != NULL, "locate compiled rig header for mutation test");
        write_u32_le(rig_bytes + 12u, 0u);
        refresh_payload_crc(bytes, size);
        require(!mdkr_modern_character_asset_load_memory(
                    bytes, size, &refused, error, sizeof(error)) &&
                    strstr(error, "rig role mask") != NULL,
                "native admission rejects a forged rig role mask");
    }
    free(bytes);

    bytes = read_file(argv[1], &size);
    {
        unsigned char *rig_bytes = section_payload(bytes, MDKR_MDKC_RIG);
        require(rig_bytes != NULL,
                "locate compiled rig review flag for lock test");
        write_u32_le(rig_bytes + 4u, 0u);
        refresh_payload_crc(bytes, size);
        require(mdkr_modern_character_asset_load_memory(
                    bytes, size, &refused, error, sizeof(error)) &&
                    mdkr_modern_pose_init(&pose, &refused,
                                          error, sizeof(error)) &&
                    !mdkr_modern_pose_humanoid_retarget_ready(&pose),
                "unreviewed inferred humanoid maps cannot drive reference motion");
        mdkr_modern_pose_shutdown(&pose);
        mdkr_modern_character_asset_unload(&refused);
    }
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
                registry.entries[0].source_lod_bias == 0.0f &&
                registry.entries[0].identity_flags == 1u &&
                registry.entries[0].rig_present == 1u &&
                registry.entries[0].rig_mode ==
                    MDKR_MODERN_RIG_HUMANOID_RETARGET_V1 &&
                registry.entries[0].rig_flags == MDKR_MODERN_RIG_REVIEWED &&
                registry.entries[0].rig_role_mask == 0xFFFFu &&
                registry.entries[0].inferred_rig_role_mask == 0u &&
                registry.entries[0].rig_min_confidence_milli == 1000u &&
                registry.entries[0].rig_role_node[0] == 0u &&
                registry.entries[0].rig_role_node[4] == 4u &&
                strcmp(registry.entries[0].rig_role_node_name[0],
                       "mixamorig:Hips") == 0 &&
                strcmp(registry.entries[0].rig_role_node_name[4],
                       "mixamorig:LeftArm") == 0 &&
                registry.entries[0].rig_role_flags[4] == 0u &&
                registry.entries[0].rig_role_confidence_milli[4] == 1000u &&
                registry.entries[0].rig_role_rest_rotation[4][3] == 1.0f &&
                registry.entries[0].rig_role_bend_axis[4][0] == 0.0f &&
                registry.entries[0].portrait_bytes > 64u &&
                registry.entries[0].portrait_rgba[3] != 0u &&
                registry.entries[0].lod_vertices[0] == 3u &&
                registry.entries[0].lod_triangles[0] == 1u &&
                registry.entries[0].lod_primitives[0] == 1u &&
                registry.entries[0].lod_palette_matrices[0] == 16u &&
                registry.entries[0].provenance_present == 1u &&
                strcmp(registry.entries[0].license_spdx, "CC0-1.0") == 0 &&
                strcmp(registry.entries[0].attribution,
                       "Generated MDKR test fixture") == 0 &&
                strcmp(registry.entries[0].source_url,
                       "https://example.invalid/pipeline-proof") == 0 &&
                (registry.entries[0].minimap_rgba & 0xFFFFFFu) == 0x9048DCu,
            "registry summarizes per-LOD authoring health and identity preview");
    require(mdkr_modern_character_registry_load(&registry, 0, &asset,
                                                 error, sizeof(error)),
            "load selected registry character");
    require(mdkr_modern_pose_init(&pose, &asset, error, sizeof(error)),
            "initialize semantic skeletal pose");
    require(mdkr_modern_pose_humanoid_retarget_ready(&pose),
            "reviewed complete humanoid map enables reference motion");
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
    require(mdkr_modern_pose_set_semantic(
                &pose, "race.item", error, sizeof(error)) &&
                mdkr_modern_pose_has_semantic(&pose, "race.item") &&
                mdkr_modern_pose_advance(&pose, 0.2f,
                                         error, sizeof(error)),
            "explicit race item clip resolves on a reviewed humanoid");
    memcpy(procedural_arm_left,
           mdkr_modern_pose_node_matrix(&pose, 4u, 0),
           sizeof(procedural_arm_left));
    require(fabsf(procedural_arm_left[1]) < 1.0e-6f &&
                fabsf(procedural_arm_left[4]) < 1.0e-6f &&
                mdkr_modern_pose_apply_vehicle_contacts(
                    &pose, MDKR_CHARACTER_CONTEXT_CAR, &calibration,
                    contact_offsets, error, sizeof(error)) &&
                memcmp(procedural_arm_left,
                       mdkr_modern_pose_node_matrix(&pose, 4u, 0),
                       sizeof(procedural_arm_left)) == 0,
            "explicit authored clips bypass reference motion and contact solving");
    require(mdkr_modern_pose_set_semantic(
                &pose, "select.confirm", error, sizeof(error)) &&
                mdkr_modern_pose_advance(&pose, 0.2f,
                                         error, sizeof(error)),
            "missing select clip resolves through reviewed reference motion");
    memcpy(procedural_arm_left,
           mdkr_modern_pose_node_matrix(&pose, 4u, 0),
           sizeof(procedural_arm_left));
    require(fabsf(procedural_arm_left[1]) > 0.1f ||
                fabsf(procedural_arm_left[4]) > 0.1f,
            "reference confirmation pose moves the mapped upper arm");
    require(mdkr_modern_pose_set_semantic(
                &pose, "race.steer", error, sizeof(error)) &&
                mdkr_modern_pose_advance_phase(
                    &pose, 0.2f, 0.0f, error, sizeof(error)),
            "reviewed humanoid fallback accepts left steering phase");
    memcpy(procedural_arm_left,
           mdkr_modern_pose_node_matrix(&pose, 4u, 0),
           sizeof(procedural_arm_left));
    require(mdkr_modern_pose_advance_phase(
                &pose, 0.2f, 1.0f, error, sizeof(error)),
            "reviewed humanoid fallback accepts right steering phase");
    memcpy(procedural_arm_right,
           mdkr_modern_pose_node_matrix(&pose, 4u, 0),
           sizeof(procedural_arm_right));
    require(memcmp(procedural_arm_left, procedural_arm_right,
                   sizeof(procedural_arm_left)) != 0,
            "procedural steering produces distinct left and right arm poses");
    require(mdkr_modern_pose_apply_vehicle_contacts(
                &pose, MDKR_CHARACTER_CONTEXT_CAR, &calibration,
                contact_offsets,
                error, sizeof(error)) &&
                isfinite(pose.contact_max_error) &&
                pose.contact_max_error < 0.25f &&
                pose.contact_valid_mask == 0xFu &&
                isfinite(pose.contact_target[0][0]) &&
                isfinite(pose.contact_end[3][2]) &&
                pose.contact_error[0] >= 0.0f,
            "bounded contact solver retains finite chain, target, endpoint, and error witnesses");
    memcpy(procedural_arm_left,
           mdkr_modern_pose_node_matrix(&pose, 6u, 0),
           sizeof(procedural_arm_left));
    require(mdkr_modern_pose_apply_vehicle_contacts(
                &pose, MDKR_CHARACTER_CONTEXT_CAR, &calibration,
                contact_offsets,
                error, sizeof(error)) &&
                memcmp(procedural_arm_left,
                       mdkr_modern_pose_node_matrix(&pose, 6u, 0),
                       sizeof(procedural_arm_left)) == 0,
            "repeating one contact context in a pose generation is idempotent");
    contact_offsets[MDKR_CHARACTER_CONTACT_HAND_LEFT][0] = NAN;
    require(mdkr_modern_pose_advance_phase(
                &pose, 0.01f, 0.5f, error, sizeof(error)) &&
                !mdkr_modern_pose_apply_vehicle_contacts(
                    &pose, MDKR_CHARACTER_CONTEXT_CAR, &calibration,
                    contact_offsets, error, sizeof(error)),
            "direct contact solver rejects non-finite author adjustments");
    contact_offsets[MDKR_CHARACTER_CONTACT_HAND_LEFT][0] = 0.0f;
    require(mdkr_modern_pose_skin_palette(&pose, 0u, 16u, 0,
                                           palette, 16u,
                                           error, sizeof(error)),
            "build mesh-relative GPU skin palette");
    require(isfinite(palette[0]) && isfinite(palette[255]),
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
    require(mdkr_modern_character_inspect_portable(
                argv[4], &install_result) &&
                strcmp(install_result.id, "org.example.pipeline-proof") == 0 &&
                strcmp(install_result.display_name, "Pipeline Proof") == 0 &&
                strcmp(install_result.short_name, "Proof") == 0 &&
                strcmp(install_result.narration_name,
                       "Pipeline Proof character") == 0 &&
                strcmp(install_result.sort_label,
                       "Proof, Pipeline") == 0 &&
                strlen(install_result.package_sha256) == 64u &&
                strlen(install_result.source_digest) == 64u &&
                install_result.donor == 9u &&
                install_result.vehicle_mask == 7u &&
                install_result.vertices == 3u &&
                install_result.triangles == 1u &&
                install_result.joints == 16u &&
                install_result.animations == 1u &&
                install_result.animation_channels == 1u &&
                install_result.animation_keys == 2u &&
                install_result.identity_present == 1u &&
                install_result.rig_mode == 2u &&
                install_result.rig_reviewed == 1u &&
                install_result.rig_roles == 16u &&
                install_result.provenance_present == 1u &&
                strcmp(install_result.license_spdx, "CC0-1.0") == 0 &&
                strcmp(install_result.attribution,
                       "Generated MDKR test fixture") == 0 &&
                strcmp(install_result.source_url,
                       "https://example.invalid/pipeline-proof") == 0 &&
                install_result.lod_vertices[0] == 3u &&
                install_result.lod_triangles[0] == 1u &&
                install_result.lod_primitives[0] == 1u &&
                install_result.decoded_texture_bytes == 4u,
            "mutation-free portable inspection publishes exact compiled comparison data");
    require(mdkr_modern_character_inspect_portable(
                argv[8], &install_result) &&
                strcmp(install_result.id,
                       "org.example.pipeline-proof") == 0 &&
                install_result.provenance_present == 0u &&
                install_result.license_spdx[0] == '\0' &&
                install_result.attribution[0] == '\0' &&
                install_result.source_url[0] == '\0',
            "compiler-v4 portable caches remain inspectable with explicit legacy provenance absence");
    require(mdkr_modern_character_inspect_portable(
                argv[9], &install_result) &&
                strcmp(install_result.id,
                       "org.example.pipeline-proof") == 0 &&
                install_result.identity_present == 1u &&
                strcmp(install_result.short_name, "Pipeline Proof") == 0 &&
                strcmp(install_result.narration_name,
                       "Pipeline Proof") == 0 &&
                strcmp(install_result.sort_label,
                       "Pipeline Proof") == 0 &&
                install_result.provenance_present == 1u,
            "compiler-v5 portable caches remain authenticated after the identity-name extension");
    require(mdkr_modern_character_inspect_portable(
                argv[4], &install_result),
            "restore the current portable review after legacy inspection");
    (void)snprintf(reviewed_package_sha, sizeof(reviewed_package_sha), "%s",
                   install_result.package_sha256);
    (void)snprintf(reviewed_source_digest, sizeof(reviewed_source_digest), "%s",
                   install_result.source_digest);
    require(mdkr_modern_character_registry_init(&registry, argv[5]) == 0 &&
                mdkr_modern_character_registry_count(&registry) == 0,
            "portable inspection never publishes a runtime cache");
    mdkr_modern_character_registry_shutdown(&registry);
    require(mdkr_modern_character_install_portable_reviewed(
                argv[4], argv[5], reviewed_package_sha, "", &install_result),
            install_result.message);
    require(strcmp(install_result.id, "org.example.pipeline-proof") == 0,
            "native portable import publishes the compiled package identity");
    require(mdkr_modern_character_install_portable_reviewed(
                argv[4], argv[5], reviewed_package_sha,
                reviewed_source_digest, &install_result),
            "reviewed native update accepts the exact current base");
    require(!mdkr_modern_character_install_portable_reviewed(
                argv[4], argv[5], reviewed_package_sha, "", &install_result) &&
                strstr(install_result.message, "installed after review") != NULL,
            "reviewed native new install refuses an id that appeared concurrently");
    require(!mdkr_modern_character_install_portable_reviewed(
                argv[7], argv[5], reviewed_package_sha,
                reviewed_source_digest, &install_result) &&
                strstr(install_result.message, "package file changed") != NULL,
            "reviewed native install binds the exact candidate package bytes");
    require(!mdkr_modern_character_install_portable_reviewed(
                argv[4], argv[5],
                "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA",
                reviewed_source_digest, &install_result) &&
                strstr(install_result.message, "digests are invalid") != NULL,
            "reviewed native install rejects non-canonical digest text");
    require(!mdkr_modern_character_install_portable_reviewed(
                argv[4], argv[5], reviewed_package_sha,
                "0000000000000000000000000000000000000000000000000000000000000000",
                &install_result) &&
                strstr(install_result.message, "installed character changed") != NULL,
            "reviewed native update binds the exact installed base revision");
    require(mdkr_modern_character_registry_init(&registry, argv[5]) == 0 &&
                mdkr_modern_character_registry_count(&registry) == 1,
            "native portable import is immediately discoverable");
    mdkr_modern_character_registry_shutdown(&registry);
    require(mdkr_modern_character_registry_init_inventory(
                &registry, argv[5]) == 0 &&
                mdkr_modern_character_registry_count(&registry) == 1 &&
                mdkr_modern_character_registry_entry(&registry, 0)->enabled == 1u &&
                mdkr_modern_character_registry_entry(&registry, 0)->source_revisions == 1u &&
                mdkr_modern_character_registry_entry(&registry, 0)->provenance_reports == 1u,
            "workshop inventory accounts for enabled cache and retained provenance");
    mdkr_modern_character_registry_shutdown(&registry);
    require(mdkr_modern_character_set_enabled(
                "org.example.pipeline-proof", argv[5], 0, &install_result) &&
                install_result.enabled == 0,
            install_result.message);
    require(mdkr_modern_character_registry_init(&registry, argv[5]) == 0 &&
                mdkr_modern_character_registry_count(&registry) == 0,
            "disabled cache is outside ordinary runtime discovery");
    mdkr_modern_character_registry_shutdown(&registry);
    require(mdkr_modern_character_registry_init_inventory(
                &registry, argv[5]) == 0 &&
                mdkr_modern_character_registry_count(&registry) == 1 &&
                mdkr_modern_character_registry_entry(&registry, 0)->enabled == 0u &&
                mdkr_modern_character_registry_entry(&registry, 0)->source_revisions == 1u &&
                mdkr_modern_character_registry_entry(&registry, 0)->provenance_reports == 1u,
            "disabled cache remains validated and editable with source history intact");
    mdkr_modern_character_registry_shutdown(&registry);
    require(mdkr_modern_character_install_portable(
                argv[4], argv[5], &install_result) &&
                install_result.enabled == 0,
            "updating a disabled package preserves its disabled state");
    lock_file = mdkr_fopen_utf8(import_lock, "wb");
    require(lock_file != NULL && fclose(lock_file) == 0,
            "create lifecycle witness lock");
    require(!mdkr_modern_character_set_enabled(
                "org.example.pipeline-proof", argv[5], 1, &install_result),
            "lifecycle state change respects the shared import lock");
    lock_file = mdkr_fopen_utf8(import_lock, "rb");
    require(lock_file != NULL && fclose(lock_file) == 0,
            "refused lifecycle state change preserves another owner's lock");
    require(mdkr_remove_utf8(import_lock) == 0,
            "retire lifecycle witness lock");
    require(mdkr_modern_character_set_enabled(
                "org.example.pipeline-proof", argv[5], 1, &install_result) &&
                install_result.enabled == 1,
            install_result.message);
    require(mdkr_modern_character_registry_init(&registry, argv[5]) == 0 &&
                mdkr_modern_character_registry_count(&registry) == 1 &&
                mdkr_modern_character_registry_entry(&registry, 0)->enabled == 1u,
            "re-enabled cache returns to ordinary runtime discovery");
    mdkr_modern_character_registry_shutdown(&registry);
    require(snprintf(prefix_witness, sizeof(prefix_witness),
                     "%s/org.example.pipeline-proof.other.%064x.json",
                     argv[5], 0) > 0,
            "construct prefix-collision provenance witness");
    lock_file = mdkr_fopen_utf8(prefix_witness, "wb");
    require(lock_file != NULL && fputs("unrelated prefix package\n", lock_file) >= 0 &&
                fclose(lock_file) == 0,
            "create unrelated longer-id provenance witness");
    require(mdkr_modern_character_set_enabled(
                "org.example.pipeline-proof", argv[5], 0, &install_result),
            install_result.message);
    require(snprintf(deletion_failure_witness,
                     sizeof(deletion_failure_witness),
                     "%s/org.example.pipeline-proof.%064x.json",
                     argv[5], 1) > 0 &&
                mdkr_mkdir_utf8(deletion_failure_witness) == 0,
            "create an undeletable owned-path-shaped directory witness");
    require(!mdkr_modern_character_remove_installed(
                "org.example.pipeline-proof", argv[5], &install_result) &&
                install_result.removed_files == 3u &&
                install_result.removed_source_revisions == 1u &&
                install_result.removed_provenance_reports == 1u &&
                install_result.failed_files == 1u,
            "partial destructive removal fails visible with exact completed scope");
    require(mdkr_rmdir_utf8(deletion_failure_witness) == 0,
            "retire deletion failure witness");
    require(mdkr_modern_character_install_portable(
                argv[4], argv[5], &install_result) &&
                install_result.enabled == 1,
            "reinstall after an explicitly reported partial deletion");
    require(mdkr_modern_character_set_enabled(
                "org.example.pipeline-proof", argv[5], 0, &install_result),
            install_result.message);
    require(mdkr_modern_character_remove_installed(
                "org.example.pipeline-proof", argv[5], &install_result),
            install_result.message);
    require(install_result.removed_files == 3u &&
                install_result.removed_source_revisions == 1u &&
                install_result.removed_provenance_reports == 1u &&
                install_result.failed_files == 0u,
            "destructive removal reports its exact cache/source/report scope");
    require(mdkr_modern_character_registry_init(&registry, argv[5]) == 0 &&
                mdkr_modern_character_registry_count(&registry) == 0,
            "native removal retires the cache and retained package source");
    mdkr_modern_character_registry_shutdown(&registry);
    lock_file = mdkr_fopen_utf8(prefix_witness, "rb");
    require(lock_file != NULL && fclose(lock_file) == 0,
            "native removal preserves provenance for a longer package id");
    require(mdkr_remove_utf8(prefix_witness) == 0,
            "retire prefix-collision provenance witness");

    for (fixture = 0; fixture < TRANSACTION_FIXTURES; fixture++) {
        int source_length = snprintf(
            transaction_source, sizeof(transaction_source),
            "%s/transaction-stage-%d.mdkc", argv[10], fixture);
        int cache_length = snprintf(
            transaction_cache[fixture], sizeof(transaction_cache[fixture]),
            "%s/transaction-stage-%d.mdkc", argv[2], fixture);
        require(source_length > 0 &&
                    (size_t)source_length < sizeof(transaction_source) &&
                    cache_length > 0 &&
                    (size_t)cache_length < sizeof(transaction_cache[fixture]),
                "construct bounded runtime transaction fixture paths");
        transaction_bytes[fixture] = read_file(
            transaction_source, &transaction_size[fixture]);
        write_file(transaction_cache[fixture], transaction_bytes[fixture],
                   transaction_size[fixture]);
    }

    require(set_env(
                "MDKR_CUSTOM_CHARACTER_PROFILE_org.example.pipeline-proof_SCALE",
                "1.75") == 0 &&
                set_env(
                    "MDKR_CUSTOM_CHARACTER_PROFILE_org.example.pipeline-proof_CAR_HAND_LEFT_X",
                    "0.125") == 0 &&
                clear_env("MDKR_CUSTOM_CHARACTER_P1_SCALE") == 0 &&
                set_env("MDKR_CUSTOM_CHARACTER_P2_SCALE", "1.25") == 0,
            "configure package tuning and a higher-priority player override");
    require(mdkr_modern_characters_init(argv[2]),
            "initialize process-level character runtime");
    original_index = mdkr_modern_character_registry_find(
        mdkr_modern_characters_registry(), "org.example.pipeline-proof");
    for (fixture = 0; fixture < TRANSACTION_FIXTURES; fixture++) {
        char id[65];
        int id_length = snprintf(
            id, sizeof(id), "org.example.pipeline-stage-%d", fixture);
        require(id_length > 0 && (size_t)id_length < sizeof(id),
                "construct bounded staged package id");
        transaction_index[fixture] = mdkr_modern_character_registry_find(
            mdkr_modern_characters_registry(), id);
    }
    {
        MdkrModernCharacterCatalogView catalog;
        require(mdkr_modern_character_catalog_count() == 9 &&
                    original_index >= 0 &&
                    transaction_index[0] >= 0 &&
                    transaction_index[7] >= 0 &&
                    mdkr_modern_character_catalog_entry(
                        original_index, &catalog) &&
                    strcmp(catalog.id, "org.example.pipeline-proof") == 0 &&
                    strcmp(catalog.display_name, "Pipeline Proof") == 0 &&
                    strcmp(catalog.short_name, "Proof") == 0 &&
                    strcmp(catalog.narration_name,
                           "Pipeline Proof character") == 0 &&
                    strcmp(catalog.sort_label, "Proof, Pipeline") == 0 &&
                    catalog.donor == 9u && catalog.vehicle_mask == 7u &&
                    catalog.has_identity == 1u &&
                    catalog.portrait_rgba != NULL &&
                    catalog.portrait_width == 40u &&
                    catalog.portrait_height == 40u &&
                    catalog.portrait_stride == 160u &&
                    catalog.minimap_rgba[0] == 220u &&
                    catalog.minimap_rgba[1] == 72u &&
                    catalog.minimap_rgba[2] == 144u &&
                    catalog.minimap_rgba[3] == 255u &&
                    catalog.revision != 0u,
                "runtime publishes a bounded library catalog without GPU activation");
        require(!mdkr_modern_character_catalog_entry(-1, &catalog) &&
                    !mdkr_modern_character_catalog_entry(9, &catalog),
                "runtime catalog rejects out-of-range rows");
    }
    require(mdkr_modern_character_assign_player_index(
                0, original_index, error, sizeof(error)),
            error);
    require(mdkr_modern_character_get_tuning(0, &tuning) &&
                tuning.scale == 1.75f &&
                tuning.contact_offset[MDKR_CHARACTER_CONTEXT_CAR]
                                     [MDKR_CHARACTER_CONTACT_HAND_LEFT][0] ==
                    0.125f,
            "catalog assignment restores package-keyed fit and contact tuning");
    require(mdkr_modern_character_matches(0, 9, 0),
            "runtime assignment retains donor and vehicle characteristics");
    require(mdkr_modern_character_player_identity(0, &identity_view) &&
                strcmp(identity_view.display_name, "Pipeline Proof") == 0 &&
                strcmp(identity_view.short_name, "Proof") == 0 &&
                strcmp(identity_view.narration_name,
                       "Pipeline Proof character") == 0 &&
                strcmp(identity_view.sort_label, "Proof, Pipeline") == 0 &&
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
    stable_identity_revision = identity_view.revision;
    write_file(transaction_cache[0], transaction_bytes[0],
               transaction_size[0] - 7u);
    require(!mdkr_modern_character_assign_player(
                0, "org.example.pipeline-stage-0", error, sizeof(error)) &&
                strcmp(mdkr_modern_character_player_package(0),
                       "org.example.pipeline-proof") == 0 &&
                mdkr_modern_character_player_identity(0, &identity_view) &&
                identity_view.revision == stable_identity_revision,
            "failed single-player activation preserves the complete prior assignment");
    assignment_plan[0] = original_index;
    assignment_plan[1] = transaction_index[0];
    assignment_plan[2] = -1;
    assignment_plan[3] = -1;
    require(!mdkr_modern_character_apply_catalog_plan(
                assignment_plan, error, sizeof(error)) &&
                strcmp(mdkr_modern_character_player_package(0),
                       "org.example.pipeline-proof") == 0 &&
                mdkr_modern_character_player_package(1) == NULL &&
                mdkr_modern_character_player_identity(0, &identity_view) &&
                identity_view.revision == stable_identity_revision,
            "failed multiplayer staging preserves every last-known-good slot");
    assignment_plan[1] = 999;
    require(!mdkr_modern_character_apply_catalog_plan(
                assignment_plan, error, sizeof(error)) &&
                strcmp(mdkr_modern_character_player_package(0),
                       "org.example.pipeline-proof") == 0,
            "invalid multiplayer plans reject before changing runtime state");
    write_file(transaction_cache[0], transaction_bytes[0],
               transaction_size[0]);
    for (player = 0; player < MDKR_MODERN_CHARACTER_PLAYERS; player++) {
        assignment_plan[player] = transaction_index[player];
    }
    require(mdkr_modern_character_apply_catalog_plan(
                assignment_plan, error, sizeof(error)) &&
                strcmp(mdkr_modern_character_player_package(0),
                       "org.example.pipeline-stage-0") == 0 &&
                strcmp(mdkr_modern_character_player_package(1),
                       "org.example.pipeline-stage-1") == 0 &&
                strcmp(mdkr_modern_character_player_package(3),
                       "org.example.pipeline-stage-3") == 0,
            "transaction publishes four distinct staged identities together");
    for (player = 0; player < MDKR_MODERN_CHARACTER_PLAYERS; player++) {
        require(mdkr_modern_character_player_identity(
                    player, &identity_view),
                "read each four-player identity before rollback test");
        roster_identity_revision[player] = identity_view.revision;
    }
    write_file(transaction_cache[7], transaction_bytes[7],
               transaction_size[7] - 7u);
    for (player = 0; player < MDKR_MODERN_CHARACTER_PLAYERS; player++) {
        assignment_plan[player] = transaction_index[player + 4];
    }
    require(!mdkr_modern_character_apply_catalog_plan(
                assignment_plan, error, sizeof(error)) &&
                strcmp(mdkr_modern_character_player_package(0),
                       "org.example.pipeline-stage-0") == 0 &&
                strcmp(mdkr_modern_character_player_package(1),
                       "org.example.pipeline-stage-1") == 0 &&
                strcmp(mdkr_modern_character_player_package(2),
                       "org.example.pipeline-stage-2") == 0 &&
                strcmp(mdkr_modern_character_player_package(3),
                       "org.example.pipeline-stage-3") == 0 &&
                mdkr_modern_character_player_identity(0, &identity_view) &&
                identity_view.revision == roster_identity_revision[0],
            "failure in the final disjoint stage preserves all four active players");
    for (player = 1; player < MDKR_MODERN_CHARACTER_PLAYERS; player++) {
        require(mdkr_modern_character_player_identity(
                    player, &identity_view) &&
                    identity_view.revision == roster_identity_revision[player],
                "failed disjoint stage preserves every identity revision");
    }
    write_file(transaction_cache[7], transaction_bytes[7],
               transaction_size[7]);
    require(mdkr_modern_character_apply_catalog_plan(
                assignment_plan, error, sizeof(error)) &&
                strcmp(mdkr_modern_character_player_package(0),
                       "org.example.pipeline-stage-4") == 0 &&
                strcmp(mdkr_modern_character_player_package(3),
                       "org.example.pipeline-stage-7") == 0,
            "a disjoint four-player plan commits after every stage validates");
    assignment_plan[0] = original_index;
    assignment_plan[1] = -1;
    assignment_plan[2] = -1;
    assignment_plan[3] = -1;
    require(mdkr_modern_character_apply_catalog_plan(
                assignment_plan, error, sizeof(error)) &&
                strcmp(mdkr_modern_character_player_package(0),
                       "org.example.pipeline-proof") == 0 &&
                mdkr_modern_character_player_package(1) == NULL,
            "transaction can atomically return mixed custom slots to retail");
    modern_character_supported = false;
    require(!mdkr_modern_character_apply_catalog_plan(
                assignment_plan, error, sizeof(error)) &&
                strcmp(mdkr_modern_character_player_package(0),
                       "org.example.pipeline-proof") == 0,
            "renderer capability loss preserves an active custom assignment");
    for (player = 0; player < MDKR_MODERN_CHARACTER_PLAYERS; player++) {
        assignment_plan[player] = -1;
    }
    require(mdkr_modern_character_apply_catalog_plan(
                assignment_plan, error, sizeof(error)) &&
                mdkr_modern_character_player_package(0) == NULL,
            "renderer capability loss still permits an atomic retail fallback");
    modern_character_supported = true;
    require(mdkr_modern_character_assign_player_index(
                0, original_index, error, sizeof(error)),
            "runtime recovers custom assignment after renderer support returns");
    mdkr_modern_character_tuning_defaults(&tuning);
    tuning.scale = 1.5f;
    tuning.translation[0] = 12.0f;
    tuning.rotation_degrees[1] = 15.0f;
    tuning.animation_speed = 0.5f;
    tuning.lod_bias = 1.0f;
    tuning.vehicle_mask = 1u;
    tuning.context[MDKR_CHARACTER_CONTEXT_SELECT].translation[1] = 0.75f;
    tuning.contact_offset[MDKR_CHARACTER_CONTEXT_CAR]
                         [MDKR_CHARACTER_CONTACT_HAND_LEFT][0] = 0.25f;
    require(mdkr_modern_character_set_tuning(0, &tuning,
                                              error, sizeof(error)),
            error);
    memset(&tuning, 0, sizeof(tuning));
    require(mdkr_modern_character_get_tuning(0, &tuning) &&
                tuning.scale == 1.5f && tuning.vehicle_mask == 1u &&
                tuning.contact_offset[MDKR_CHARACTER_CONTEXT_CAR]
                                     [MDKR_CHARACTER_CONTACT_HAND_LEFT][0] ==
                    0.25f,
            "runtime retains bounded presentation-only tuning");
    require(mdkr_modern_character_matches(0, 9, 0) &&
                !mdkr_modern_character_matches(0, 9, 1),
            "player vehicle pairing narrows package-qualified bodies");
    tuning.scale = 9.0f;
    require(!mdkr_modern_character_set_tuning(0, &tuning,
                                               error, sizeof(error)),
            "unsafe editor tuning fails closed");
    tuning.scale = 1.5f;
    tuning.contact_offset[MDKR_CHARACTER_CONTEXT_SELECT]
                         [MDKR_CHARACTER_CONTACT_FOOT_RIGHT][2] = NAN;
    require(!mdkr_modern_character_set_tuning(0, &tuning,
                                               error, sizeof(error)),
            "non-finite unused contact tuning cannot enter runtime state");
    tuning.contact_offset[MDKR_CHARACTER_CONTEXT_SELECT]
                         [MDKR_CHARACTER_CONTACT_FOOT_RIGHT][2] = 0.0f;
    require(mdkr_modern_character_tick(0, "race.boost", 0.25f,
                                       error, sizeof(error)),
            "runtime semantic uses package fallback when optional state is absent");
    require(mdkr_modern_character_tick_phase(
                0, "race.steer", 0.25f, 1.0f,
                error, sizeof(error)),
            "missing phase-driven semantic advances fallback instead of scrubbing it");
    require(mdkr_modern_character_set_inspection_pose(
                "race.finish_win", 0.75f, error, sizeof(error)) &&
                mdkr_modern_character_tick(
                    0, "select.idle", 0.25f, error, sizeof(error)),
            "exact inspector overrides live semantics with a validated held phase");
    mdkr_modern_character_runtime_metrics(&runtime_metrics);
    require(runtime_metrics.inspection_pose_ticks == 1u &&
                runtime_metrics.inspection_pose_fallback_ticks == 0u,
            "reviewed humanoid reference motion satisfies missing authored inspection semantics");
    require(mdkr_modern_character_set_inspection_pose(
                "select.idle", 0.25f, error, sizeof(error)) &&
                mdkr_modern_character_tick(
                    0, "race.boost", 0.25f, error, sizeof(error)),
            "inspector phase-scrubs an explicitly authored semantic");
    mdkr_modern_character_runtime_metrics(&runtime_metrics);
    require(runtime_metrics.inspection_pose_ticks == 2u &&
                runtime_metrics.inspection_pose_fallback_ticks == 0u,
            "exact authored inspection does not increment fallback evidence");
    require(!mdkr_modern_character_set_inspection_pose(
                "race.unknown", 0.5f, error, sizeof(error)) &&
                !mdkr_modern_character_set_inspection_pose(
                    "race.steer", 1.5f, error, sizeof(error)),
            "exact inspector rejects unknown semantics and unsafe phases");
    mdkr_modern_character_clear_inspection_pose();
    require(mdkr_modern_character_tick_phase(
                0, "race.steer", 0.25f, 1.0f,
                error, sizeof(error)),
            "clearing inspection restores ordinary gameplay pose selection");
    mdkr_workshop_preview_visual_clear();
    mdkr_workshop_preview_visual_metrics_reset();
    require(!mdkr_modern_character_player_focus(
                0, MDKR_CHARACTER_CONTEXT_SELECT,
                focus_center, &focus_radius),
            "focus is unavailable until the fitted context renders");
    require(!mdkr_modern_character_player_fit_diagnostics(
                0, MDKR_CHARACTER_CONTEXT_SELECT, &fit_diagnostics),
            "fit diagnostics are unavailable until the exact transform renders");
    require(mdkr_workshop_preview_visual_set(
                0, 0, MDKR_WORKSHOP_PREVIEW_LIGHTING_BRIGHT,
                error, sizeof(error)),
            "exact renderer accepts a bounded character-light preset");
    require(mdkr_modern_character_emit(0, 0, MDKR_CHARACTER_CONTEXT_SELECT,
                                       NULL, 0.0f, &command_cursor,
                                       error, sizeof(error)),
            error);
    select_model_y = last_model_matrix[13];
    require(mdkr_modern_character_emit(0, 0, MDKR_CHARACTER_CONTEXT_CAR,
                                       NULL, 0.0f, &command_cursor,
                                       error, sizeof(error)),
            error);
    require(mdkr_modern_character_player_focus(
                0, MDKR_CHARACTER_CONTEXT_SELECT,
                focus_center, &focus_radius) &&
                isfinite(focus_center[0]) && isfinite(focus_center[1]) &&
                isfinite(focus_center[2]) && isfinite(focus_radius) &&
                focus_radius > 0.0f &&
                mdkr_modern_character_player_focus(
                    0, MDKR_CHARACTER_CONTEXT_CAR,
                    focus_center, &focus_radius) && focus_radius > 0.0f &&
                !mdkr_modern_character_player_focus(
                    0, MDKR_CHARACTER_CONTEXT_HOVERCRAFT,
                    focus_center, &focus_radius) &&
                !mdkr_modern_character_player_focus(
                    -1, MDKR_CHARACTER_CONTEXT_CAR,
                    focus_center, &focus_radius),
            "runtime publishes only complete rendered fitted focus volumes");
    require(mdkr_modern_character_player_fit_diagnostics(
                0, MDKR_CHARACTER_CONTEXT_SELECT, &fit_diagnostics) &&
                fabsf(fit_diagnostics.anchor[0] - 12.0f) < 0.001f &&
                fabsf(fit_diagnostics.anchor[1] - 0.75f) < 0.001f &&
                fabsf(fit_diagnostics.bounds_min[1] - 0.75f) < 0.001f &&
                fabsf(fit_diagnostics.forward[0] - 0.258819f) < 0.001f &&
                fabsf(fit_diagnostics.forward[1]) < 0.001f &&
                fabsf(fit_diagnostics.forward[2] - 0.965926f) < 0.001f &&
                mdkr_modern_character_player_fit_diagnostics(
                    0, MDKR_CHARACTER_CONTEXT_CAR, &fit_diagnostics) &&
                fit_diagnostics.bounds_min[0] <= fit_diagnostics.bounds_max[0] &&
                fit_diagnostics.bounds_min[1] <= fit_diagnostics.bounds_max[1] &&
                fit_diagnostics.bounds_min[2] <= fit_diagnostics.bounds_max[2] &&
                !mdkr_modern_character_player_fit_diagnostics(
                    0, MDKR_CHARACTER_CONTEXT_HOVERCRAFT,
                    &fit_diagnostics) &&
                !mdkr_modern_character_player_fit_diagnostics(
                    -1, MDKR_CHARACTER_CONTEXT_CAR, &fit_diagnostics),
            "runtime publishes exact target-space anchor, calibrated bounds, and normalized facing only after a successful context draw");
    require(mdkr_modern_character_player_contact_diagnostics(
                0, MDKR_CHARACTER_CONTEXT_CAR, &contact_diagnostics) &&
                contact_diagnostics.valid_mask == 0xFu &&
                isfinite(contact_diagnostics.chain_root[0][0]) &&
                isfinite(contact_diagnostics.bend[1][1]) &&
                isfinite(contact_diagnostics.target[2][2]) &&
                isfinite(contact_diagnostics.end[3][2]) &&
                contact_diagnostics.error[0] >= 0.0f &&
                !mdkr_modern_character_player_contact_diagnostics(
                    0, MDKR_CHARACTER_CONTEXT_SELECT,
                    &contact_diagnostics) &&
                !mdkr_modern_character_player_contact_diagnostics(
                    0, MDKR_CHARACTER_CONTEXT_HOVERCRAFT,
                    &contact_diagnostics),
            "runtime publishes complete contact witnesses only for a successfully rendered solved vehicle context");
    require(command_cursor == commands + 2 && registered_draws == 2u,
            "runtime emits one retained command per selected primitive");
    require(select_model_y - last_model_matrix[13] > 0.70f,
            "select ground correction is independent from the car seat frame");
    require(fabsf(last_model_matrix[12]) > 1.0f,
            "runtime draw includes the player seat-offset adjustment");
    require(commands[0].words.w1 == 1u && commands[1].words.w1 == 2u,
            "display list embeds immutable draw token rather than a pointer");
    mdkr_workshop_preview_visual_metrics(&visual_metrics);
    require(visual_metrics.lighting_override_draws == 2u,
            "character-only lighting records one witness per successful replacement");
    mdkr_workshop_preview_visual_clear();
    mdkr_modern_character_runtime_metrics(&runtime_metrics);
    require(runtime_metrics.replacement_draws == 2u &&
                runtime_metrics.contact_solves == 1u &&
                runtime_metrics.contact_error_micrometres_sum > 0u &&
                runtime_metrics.contact_error_micrometres_max > 0u &&
                runtime_metrics.contact_error_micrometres_max < 250000u,
            "runtime publishes physical contact-fit evidence with draw metrics");
    mdkr_modern_character_contact_metrics_reset();
    mdkr_modern_character_runtime_metrics(&runtime_metrics);
    require(runtime_metrics.replacement_draws == 2u &&
                runtime_metrics.contact_solves == 0u &&
                runtime_metrics.contact_error_micrometres_sum == 0u &&
                runtime_metrics.contact_error_micrometres_max == 0u,
            "preview warm-up reset affects contact observations only");
    for (player = 1; player < MDKR_MODERN_CHARACTER_PLAYERS; player++) {
        require(mdkr_modern_character_assign_player(
                    player, "org.example.pipeline-proof",
                    error, sizeof(error)),
                "all four local player slots share one installed package");
        if (player == 1) {
            require(mdkr_modern_character_get_tuning(player, &tuning) &&
                        tuning.scale == 1.25f,
                    "explicit player tuning overrides the package profile");
        }
        require(mdkr_modern_character_matches(player, 9, 0) &&
                    mdkr_modern_character_tick(
                        player, "select.confirm", 0.1f,
                        error, sizeof(error)),
                "each local player owns an independent semantic pose");
    }
    require(mdkr_modern_character_emit(3, 3, MDKR_CHARACTER_CONTEXT_CAR,
                                       NULL, 0.0f, &command_cursor,
                                       error, sizeof(error)) &&
                command_cursor == commands + 3 && registered_draws == 3u,
            "four-player assignment reuses GPU ownership and emits independently");
    require(mdkr_modern_character_emit(3, 7, MDKR_CHARACTER_CONTEXT_CAR,
                                       NULL, 0.0f, &command_cursor,
                                       error, sizeof(error)) &&
                command_cursor == commands + 4 && registered_draws == 4u,
            "cutscene camera ownership remains a valid ordinary draw");
    require(!mdkr_modern_character_emit(
                3, MDKR_MODERN_CHARACTER_VIEWS,
                MDKR_CHARACTER_CONTEXT_CAR, NULL, 0.0f, &command_cursor,
                error, sizeof(error)) &&
                command_cursor == commands + 4 && registered_draws == 4u,
            "out-of-range camera ownership fails before draw publication");
    require(mdkr_modern_character_get_tuning(0, &tuning) &&
                mdkr_modern_character_set_tuning(
                    0, &tuning, error, sizeof(error)) &&
                !mdkr_modern_character_player_fit_diagnostics(
                    0, MDKR_CHARACTER_CONTEXT_SELECT, &fit_diagnostics) &&
                !mdkr_modern_character_player_fit_diagnostics(
                    0, MDKR_CHARACTER_CONTEXT_CAR, &fit_diagnostics),
            "any tuning publication invalidates every stale fit measurement until each context renders again");
    mdkr_modern_characters_shutdown();
    for (fixture = 0; fixture < TRANSACTION_FIXTURES; fixture++) {
        free(transaction_bytes[fixture]);
    }
    (void)clear_env(
        "MDKR_CUSTOM_CHARACTER_PROFILE_org.example.pipeline-proof_SCALE");
    (void)clear_env("MDKR_CUSTOM_CHARACTER_P2_SCALE");
    require(released_assets == 14u,
            "runtime retires GPU ownership before freeing CPU asset bytes");

    puts("PASS: compiled modern character cache and retained runtime validate and fail closed");
    return 0;
}
