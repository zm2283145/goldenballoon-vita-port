#include "modern_character_pose.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void set_error(char *error, size_t size, const char *message) {
    if (error != NULL && size != 0u) {
        (void)snprintf(error, size, "%s", message != NULL ? message : "unknown error");
    }
}

static float clamp01(float value) {
    if (value < 0.0f) return 0.0f;
    if (value > 1.0f) return 1.0f;
    return value;
}

static void quat_normalize(float value[4]) {
    float length = sqrtf(value[0] * value[0] + value[1] * value[1] +
                         value[2] * value[2] + value[3] * value[3]);
    if (length < 1.0e-12f || !isfinite(length)) {
        value[0] = value[1] = value[2] = 0.0f;
        value[3] = 1.0f;
        return;
    }
    value[0] /= length;
    value[1] /= length;
    value[2] /= length;
    value[3] /= length;
}

static void quat_slerp(const float left[4], const float right_input[4],
                       float amount, float output[4]) {
    float right[4] = { right_input[0], right_input[1], right_input[2], right_input[3] };
    float dot = left[0] * right[0] + left[1] * right[1] +
                left[2] * right[2] + left[3] * right[3];
    float left_scale;
    float right_scale;
    unsigned index;
    if (dot < 0.0f) {
        dot = -dot;
        for (index = 0u; index < 4u; index++) right[index] = -right[index];
    }
    if (dot > 0.9995f) {
        left_scale = 1.0f - amount;
        right_scale = amount;
    } else {
        float angle = acosf(dot > 1.0f ? 1.0f : dot);
        float denominator = sinf(angle);
        left_scale = sinf((1.0f - amount) * angle) / denominator;
        right_scale = sinf(amount * angle) / denominator;
    }
    for (index = 0u; index < 4u; index++) {
        output[index] = left[index] * left_scale + right[index] * right_scale;
    }
    quat_normalize(output);
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

static void matrix_from_trs(const MdkrModernTrs *trs, float output[16]) {
    float x = trs->rotation[0];
    float y = trs->rotation[1];
    float z = trs->rotation[2];
    float w = trs->rotation[3];
    float sx = trs->scale[0];
    float sy = trs->scale[1];
    float sz = trs->scale[2];
    output[0] = (1.0f - 2.0f * (y * y + z * z)) * sx;
    output[1] = (2.0f * (x * y + z * w)) * sx;
    output[2] = (2.0f * (x * z - y * w)) * sx;
    output[3] = 0.0f;
    output[4] = (2.0f * (x * y - z * w)) * sy;
    output[5] = (1.0f - 2.0f * (x * x + z * z)) * sy;
    output[6] = (2.0f * (y * z + x * w)) * sy;
    output[7] = 0.0f;
    output[8] = (2.0f * (x * z + y * w)) * sz;
    output[9] = (2.0f * (y * z - x * w)) * sz;
    output[10] = (1.0f - 2.0f * (x * x + y * y)) * sz;
    output[11] = 0.0f;
    output[12] = trs->translation[0];
    output[13] = trs->translation[1];
    output[14] = trs->translation[2];
    output[15] = 1.0f;
}

static int matrix_inverse_affine(const float input[16], float output[16]) {
    float a00 = input[0], a01 = input[4], a02 = input[8];
    float a10 = input[1], a11 = input[5], a12 = input[9];
    float a20 = input[2], a21 = input[6], a22 = input[10];
    float determinant = a00 * (a11 * a22 - a12 * a21) -
                        a01 * (a10 * a22 - a12 * a20) +
                        a02 * (a10 * a21 - a11 * a20);
    float inverse;
    float tx;
    float ty;
    float tz;
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
    tx = input[12]; ty = input[13]; tz = input[14];
    output[12] = -(output[0] * tx + output[4] * ty + output[8] * tz);
    output[13] = -(output[1] * tx + output[5] * ty + output[9] * tz);
    output[14] = -(output[2] * tx + output[6] * ty + output[10] * tz);
    return 1;
}

static int animation_by_name(const MdkrModernCharacterAsset *asset,
                             const char *name) {
    const MdkrModernSectionView *animations = mdkr_modern_character_asset_section(
        asset, MDKR_MDKC_ANIMATIONS);
    uint32_t index;
    if (animations == NULL || name == NULL) return -1;
    for (index = 0u; index < animations->count; index++) {
        MdkrModernAnimation animation;
        (void)mdkr_modern_character_asset_animation(asset, index, &animation);
        if (strcmp(mdkr_modern_character_asset_string(asset, animation.name), name) == 0) {
            return (int)index;
        }
    }
    return -1;
}

static int semantic_lookup(const MdkrModernCharacterAsset *asset,
                           const char *semantic, int *animation,
                           uint32_t *flags, float *blend) {
    const MdkrModernSectionView *semantics = mdkr_modern_character_asset_section(
        asset, MDKR_MDKC_SEMANTICS);
    int fallback = -1;
    uint32_t index;
    if (semantics == NULL) return 0;
    for (index = 0u; index < semantics->count; index++) {
        MdkrModernSemantic mapping;
        const char *mapping_name;
        (void)mdkr_modern_character_asset_semantic(asset, index, &mapping);
        mapping_name = mdkr_modern_character_asset_string(asset, mapping.semantic);
        if (strcmp(mapping_name, "fallback") == 0) fallback = (int)index;
        if (semantic != NULL && strcmp(mapping_name, semantic) == 0) {
            fallback = (int)index;
            break;
        }
    }
    if (fallback < 0) return 0;
    {
        MdkrModernSemantic mapping;
        const char *clip;
        (void)mdkr_modern_character_asset_semantic(asset, (uint32_t)fallback, &mapping);
        clip = mdkr_modern_character_asset_string(asset, mapping.clip);
        *animation = animation_by_name(asset, clip);
        *flags = mapping.flags;
        *blend = mapping.blend_seconds;
    }
    return *animation >= 0;
}

int mdkr_modern_pose_has_semantic(const MdkrModernPose *pose,
                                  const char *semantic) {
    const MdkrModernSectionView *semantics;
    uint32_t index;
    if (pose == NULL || !pose->valid || semantic == NULL) return 0;
    semantics = mdkr_modern_character_asset_section(
        pose->asset, MDKR_MDKC_SEMANTICS);
    if (semantics == NULL) return 0;
    for (index = 0u; index < semantics->count; index++) {
        MdkrModernSemantic mapping;
        const char *name;
        (void)mdkr_modern_character_asset_semantic(
            pose->asset, index, &mapping);
        name = mdkr_modern_character_asset_string(pose->asset,
                                                   mapping.semantic);
        if (name != NULL && strcmp(name, semantic) == 0) return 1;
    }
    return 0;
}

static void bind_pose(const MdkrModernCharacterAsset *asset,
                      MdkrModernTrs *output, uint32_t count) {
    uint32_t index;
    for (index = 0u; index < count; index++) {
        MdkrModernNode node;
        (void)mdkr_modern_character_asset_node(asset, index, &node);
        memcpy(output[index].translation, node.translation, sizeof(node.translation));
        memcpy(output[index].rotation, node.rotation, sizeof(node.rotation));
        memcpy(output[index].scale, node.scale, sizeof(node.scale));
    }
}

static int sample_channel(const MdkrModernCharacterAsset *asset,
                          const MdkrModernChannel *channel, float time,
                          float output[4]) {
    MdkrModernKey left;
    MdkrModernKey right;
    uint32_t lower = 0u;
    uint32_t upper = channel->key_count - 1u;
    float amount;
    float interval;
    unsigned component;
    (void)mdkr_modern_character_asset_key(asset, channel->first_key, &left);
    if (time <= left.time || channel->key_count == 1u) {
        memcpy(output, left.value, sizeof(left.value));
        return 1;
    }
    (void)mdkr_modern_character_asset_key(asset,
                                          channel->first_key + upper, &right);
    if (time >= right.time) {
        memcpy(output, right.value, sizeof(right.value));
        return 1;
    }
    while (upper - lower > 1u) {
        uint32_t middle = lower + (upper - lower) / 2u;
        MdkrModernKey key;
        (void)mdkr_modern_character_asset_key(asset,
                                              channel->first_key + middle, &key);
        if (key.time <= time) lower = middle;
        else upper = middle;
    }
    (void)mdkr_modern_character_asset_key(asset,
                                          channel->first_key + lower, &left);
    (void)mdkr_modern_character_asset_key(asset,
                                          channel->first_key + upper, &right);
    interval = right.time - left.time;
    amount = interval > 0.0f ? clamp01((time - left.time) / interval) : 0.0f;
    if (channel->interpolation == 1u) {
        memcpy(output, left.value, sizeof(left.value));
    } else if (channel->interpolation == 2u) {
        float t2 = amount * amount;
        float t3 = t2 * amount;
        float h00 = 2.0f * t3 - 3.0f * t2 + 1.0f;
        float h10 = t3 - 2.0f * t2 + amount;
        float h01 = -2.0f * t3 + 3.0f * t2;
        float h11 = t3 - t2;
        for (component = 0u; component < 4u; component++) {
            output[component] = h00 * left.value[component] +
                                h10 * interval * left.outgoing[component] +
                                h01 * right.value[component] +
                                h11 * interval * right.incoming[component];
        }
        if (channel->path == 1u) quat_normalize(output);
    } else if (channel->path == 1u) {
        quat_slerp(left.value, right.value, amount, output);
    } else {
        for (component = 0u; component < 4u; component++) {
            output[component] = left.value[component] +
                                (right.value[component] - left.value[component]) * amount;
        }
    }
    return 1;
}

static int evaluate_local(MdkrModernPose *pose, MdkrModernTrs *target,
                          char *error, size_t error_size) {
    MdkrModernAnimation animation;
    uint32_t channel_index;
    bind_pose(pose->asset, target, pose->node_count);
    if (pose->animation < 0 ||
        !mdkr_modern_character_asset_animation(
            pose->asset, (uint32_t)pose->animation, &animation)) {
        set_error(error, error_size, "pose has no valid animation");
        return 0;
    }
    for (channel_index = 0u; channel_index < animation.channel_count; channel_index++) {
        MdkrModernChannel channel;
        float sampled[4];
        (void)mdkr_modern_character_asset_channel(
            pose->asset, animation.first_channel + channel_index, &channel);
        if (!sample_channel(pose->asset, &channel, pose->time, sampled)) return 0;
        if (channel.path == 0u) memcpy(target[channel.node].translation, sampled, sizeof(float) * 3u);
        else if (channel.path == 1u) memcpy(target[channel.node].rotation, sampled, sizeof(float) * 4u);
        else if (channel.path == 2u) memcpy(target[channel.node].scale, sampled, sizeof(float) * 3u);
    }
    return 1;
}

static int evaluate_world(MdkrModernPose *pose, float *output,
                          char *error, size_t error_size) {
    uint32_t order_index;
    if (pose->evaluation_order == NULL) {
        set_error(error, error_size, "pose has no validated evaluation order");
        return 0;
    }
    for (order_index = 0u; order_index < pose->node_count; order_index++) {
        uint32_t node_index = pose->evaluation_order[order_index];
        MdkrModernNode node;
        float local[16];
        (void)mdkr_modern_character_asset_node(pose->asset, node_index, &node);
        matrix_from_trs(&pose->local[node_index], local);
        if (node.parent >= 0) {
            matrix_multiply(output + (size_t)node.parent * 16u, local,
                            output + (size_t)node_index * 16u);
        } else {
            memcpy(output + (size_t)node_index * 16u, local, sizeof(local));
        }
    }
    return 1;
}

static int build_evaluation_order(MdkrModernPose *pose,
                                  char *error, size_t error_size) {
    uint8_t *resolved;
    uint32_t count = 0u;
    resolved = (uint8_t *)calloc(pose->node_count, 1u);
    if (resolved == NULL) {
        set_error(error, error_size, "could not allocate pose hierarchy validation state");
        return 0;
    }
    while (count < pose->node_count) {
        uint32_t node_index;
        int progress = 0;
        for (node_index = 0u; node_index < pose->node_count; node_index++) {
            MdkrModernNode node;
            if (resolved[node_index]) continue;
            (void)mdkr_modern_character_asset_node(pose->asset, node_index, &node);
            if (node.parent >= 0 && !resolved[(uint32_t)node.parent]) continue;
            pose->evaluation_order[count++] = node_index;
            resolved[node_index] = 1u;
            progress = 1;
        }
        if (!progress) {
            free(resolved);
            set_error(error, error_size, "pose node hierarchy contains a cycle");
            return 0;
        }
    }
    free(resolved);
    return 1;
}

int mdkr_modern_pose_init(MdkrModernPose *pose,
                          const MdkrModernCharacterAsset *asset,
                          char *error, size_t error_size) {
    const MdkrModernSectionView *nodes;
    size_t trs_bytes;
    size_t matrix_bytes;
    if (pose == NULL || asset == NULL || asset->owned_bytes == NULL) {
        set_error(error, error_size, "pose requires a validated character asset");
        return 0;
    }
    memset(pose, 0, sizeof(*pose));
    nodes = mdkr_modern_character_asset_section(asset, MDKR_MDKC_NODES);
    if (nodes == NULL || nodes->count == 0u) {
        set_error(error, error_size, "character has no scene nodes");
        return 0;
    }
    pose->asset = asset;
    pose->node_count = nodes->count;
    trs_bytes = (size_t)pose->node_count * sizeof(MdkrModernTrs);
    matrix_bytes = (size_t)pose->node_count * 16u * sizeof(float);
    pose->local = (MdkrModernTrs *)malloc(trs_bytes);
    pose->blend_from = (MdkrModernTrs *)malloc(trs_bytes);
    pose->sampled = (MdkrModernTrs *)malloc(trs_bytes);
    pose->evaluation_order = (uint32_t *)malloc(
        (size_t)pose->node_count * sizeof(*pose->evaluation_order));
    pose->world_previous = (float *)calloc(1u, matrix_bytes);
    pose->world_current = (float *)calloc(1u, matrix_bytes);
    if (pose->local == NULL || pose->blend_from == NULL || pose->sampled == NULL ||
        pose->evaluation_order == NULL ||
        pose->world_previous == NULL || pose->world_current == NULL) {
        mdkr_modern_pose_shutdown(pose);
        set_error(error, error_size, "could not allocate character pose buffers");
        return 0;
    }
    bind_pose(asset, pose->local, pose->node_count);
    memcpy(pose->blend_from, pose->local, trs_bytes);
    if (!build_evaluation_order(pose, error, error_size)) {
        mdkr_modern_pose_shutdown(pose);
        return 0;
    }
    pose->animation = -1;
    pose->valid = 1;
    if (!mdkr_modern_pose_set_semantic(pose, "fallback", error, error_size) ||
        !mdkr_modern_pose_advance(pose, 0.0f, error, error_size)) {
        mdkr_modern_pose_shutdown(pose);
        return 0;
    }
    memcpy(pose->world_previous, pose->world_current, matrix_bytes);
    return 1;
}

void mdkr_modern_pose_shutdown(MdkrModernPose *pose) {
    if (pose == NULL) return;
    free(pose->local);
    free(pose->blend_from);
    free(pose->sampled);
    free(pose->evaluation_order);
    free(pose->world_previous);
    free(pose->world_current);
    memset(pose, 0, sizeof(*pose));
}

int mdkr_modern_pose_set_semantic(MdkrModernPose *pose, const char *semantic,
                                  char *error, size_t error_size) {
    int animation;
    uint32_t flags;
    float blend;
    if (pose == NULL || !pose->valid ||
        !semantic_lookup(pose->asset, semantic, &animation, &flags, &blend)) {
        set_error(error, error_size, "character semantic and fallback could not resolve");
        return 0;
    }
    if (pose->animation == animation) return 1;
    memcpy(pose->blend_from, pose->local,
           (size_t)pose->node_count * sizeof(*pose->local));
    pose->animation = animation;
    pose->animation_flags = flags;
    pose->time = 0.0f;
    pose->blend_elapsed = 0.0f;
    pose->blend_duration = blend;
    set_error(error, error_size, "");
    return 1;
}

static int pose_advance(MdkrModernPose *pose, float seconds,
                        int phase_driven, float normalized_phase,
                        char *error, size_t error_size) {
    MdkrModernAnimation animation;
    float blend_amount;
    uint32_t node;
    if (pose == NULL || !pose->valid || !isfinite(seconds) || seconds < 0.0f ||
        !mdkr_modern_character_asset_animation(
            pose->asset, (uint32_t)pose->animation, &animation)) {
        set_error(error, error_size, "pose advance arguments are invalid");
        return 0;
    }
    memcpy(pose->world_previous, pose->world_current,
           (size_t)pose->node_count * 16u * sizeof(float));
    if (phase_driven) {
        pose->time = clamp01(normalized_phase) * animation.duration;
    } else {
        pose->time += seconds;
    }
    if (!phase_driven && (pose->animation_flags & 1u) != 0u) {
        if (animation.duration > 0.0f) pose->time = fmodf(pose->time, animation.duration);
    } else if (!phase_driven && pose->time > animation.duration) {
        pose->time = animation.duration;
    }
    if (!evaluate_local(pose, pose->sampled, error, error_size)) return 0;
    pose->blend_elapsed += seconds;
    blend_amount = pose->blend_duration > 0.0f
        ? clamp01(pose->blend_elapsed / pose->blend_duration) : 1.0f;
    for (node = 0u; node < pose->node_count; node++) {
        unsigned component;
        for (component = 0u; component < 3u; component++) {
            pose->local[node].translation[component] =
                pose->blend_from[node].translation[component] +
                (pose->sampled[node].translation[component] - pose->blend_from[node].translation[component]) * blend_amount;
            pose->local[node].scale[component] =
                pose->blend_from[node].scale[component] +
                (pose->sampled[node].scale[component] - pose->blend_from[node].scale[component]) * blend_amount;
        }
        quat_slerp(pose->blend_from[node].rotation, pose->sampled[node].rotation,
                   blend_amount, pose->local[node].rotation);
    }
    if (!evaluate_world(pose, pose->world_current, error, error_size)) return 0;
    set_error(error, error_size, "");
    return 1;
}

int mdkr_modern_pose_advance(MdkrModernPose *pose, float seconds,
                             char *error, size_t error_size) {
    return pose_advance(pose, seconds, 0, 0.0f, error, error_size);
}

int mdkr_modern_pose_advance_phase(MdkrModernPose *pose, float seconds,
                                   float normalized_phase,
                                   char *error, size_t error_size) {
    if (!isfinite(normalized_phase)) {
        set_error(error, error_size,
                  "pose normalized phase must be finite");
        return 0;
    }
    return pose_advance(pose, seconds, 1, normalized_phase,
                        error, error_size);
}

const float *mdkr_modern_pose_node_matrix(const MdkrModernPose *pose,
                                          uint32_t node, int previous) {
    if (pose == NULL || !pose->valid || node >= pose->node_count) return NULL;
    return (previous ? pose->world_previous : pose->world_current) +
           (size_t)node * 16u;
}

int mdkr_modern_pose_socket_matrix(const MdkrModernPose *pose,
                                   const char *semantic, int previous,
                                   float output[16]) {
    const MdkrModernSectionView *sockets;
    uint32_t index;
    if (pose == NULL || semantic == NULL || output == NULL) return 0;
    sockets = mdkr_modern_character_asset_section(pose->asset, MDKR_MDKC_SOCKETS);
    if (sockets == NULL) return 0;
    for (index = 0u; index < sockets->count; index++) {
        MdkrModernSocket socket;
        const float *matrix;
        (void)mdkr_modern_character_asset_socket(pose->asset, index, &socket);
        if (strcmp(mdkr_modern_character_asset_string(pose->asset, socket.semantic), semantic) != 0) continue;
        matrix = mdkr_modern_pose_node_matrix(pose, socket.node, previous);
        if (matrix == NULL) return 0;
        memcpy(output, matrix, sizeof(float) * 16u);
        return 1;
    }
    return 0;
}

int mdkr_modern_pose_skin_palette(const MdkrModernPose *pose,
                                  uint32_t skin_index, uint32_t mesh_node,
                                  int previous, float *output,
                                  size_t output_matrices,
                                  char *error, size_t error_size) {
    MdkrModernSkin skin;
    const float *mesh_world;
    float inverse_mesh[16];
    uint32_t index;
    if (pose == NULL || output == NULL || mesh_node >= pose->node_count ||
        !mdkr_modern_character_asset_skin(pose->asset, skin_index, &skin) ||
        output_matrices < skin.joint_count) {
        set_error(error, error_size, "skin palette arguments are invalid");
        return 0;
    }
    mesh_world = mdkr_modern_pose_node_matrix(pose, mesh_node, previous);
    if (mesh_world == NULL || !matrix_inverse_affine(mesh_world, inverse_mesh)) {
        set_error(error, error_size, "skinned mesh transform is singular");
        return 0;
    }
    for (index = 0u; index < skin.joint_count; index++) {
        MdkrModernJoint joint;
        const float *joint_world;
        float joint_bind[16];
        (void)mdkr_modern_character_asset_joint(
            pose->asset, skin.first_joint + index, &joint);
        joint_world = mdkr_modern_pose_node_matrix(pose, joint.node, previous);
        matrix_multiply(joint_world, joint.inverse_bind, joint_bind);
        matrix_multiply(inverse_mesh, joint_bind, output + (size_t)index * 16u);
    }
    set_error(error, error_size, "");
    return 1;
}
