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

static void quat_multiply(const float left[4], const float right[4],
                          float output[4]) {
    float result[4];
    result[0] = left[3] * right[0] + left[0] * right[3] +
                left[1] * right[2] - left[2] * right[1];
    result[1] = left[3] * right[1] - left[0] * right[2] +
                left[1] * right[3] + left[2] * right[0];
    result[2] = left[3] * right[2] + left[0] * right[1] -
                left[1] * right[0] + left[2] * right[3];
    result[3] = left[3] * right[3] - left[0] * right[0] -
                left[1] * right[1] - left[2] * right[2];
    memcpy(output, result, sizeof(result));
    quat_normalize(output);
}

static void quat_axis_angle(const float axis[3], float radians,
                            float output[4]) {
    const float half = radians * 0.5f;
    const float sine = sinf(half);
    output[0] = axis[0] * sine;
    output[1] = axis[1] * sine;
    output[2] = axis[2] * sine;
    output[3] = cosf(half);
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

static int rig_role_index(const char *semantic) {
    static const char *roles[] = {
        "hips", "spine", "chest", "head",
        "upper_arm.left", "lower_arm.left", "hand.left",
        "upper_arm.right", "lower_arm.right", "hand.right",
        "upper_leg.left", "lower_leg.left", "foot.left",
        "upper_leg.right", "lower_leg.right", "foot.right"
    };
    int index;
    if (semantic == NULL) return -1;
    for (index = 0; index < (int)MDKR_MODERN_HUMANOID_ROLE_COUNT; index++) {
        if (strcmp(semantic, roles[index]) == 0) return index;
    }
    return -1;
}

static int semantic_lookup(const MdkrModernCharacterAsset *asset,
                           const char *semantic, int *animation,
                           uint32_t *flags, float *blend, int *explicit_match) {
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
            /* Keep scanning for the package fallback. Current compilers emit it
             * first, but runtime correctness must not depend on record order. */
            if ((mapping.flags & MDKR_MODERN_SEMANTIC_DISABLED) != 0u) continue;
            fallback = (int)index;
            *explicit_match = 1;
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

int mdkr_modern_pose_humanoid_retarget_ready(const MdkrModernPose *pose) {
    return pose != NULL && pose->valid && pose->humanoid_retarget_ready;
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
        if (name != NULL && strcmp(name, semantic) == 0 &&
            (mapping.flags & MDKR_MODERN_SEMANTIC_DISABLED) == 0u) return 1;
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

static void apply_role_axis(MdkrModernPose *pose, int role,
                            const float axis[3], float radians) {
    const uint32_t node = pose->rig_role_nodes[role];
    const float *correction = pose->rig_rest_rotation[role];
    float inverse_correction[4] = {
        -correction[0], -correction[1], -correction[2], correction[3]
    };
    float canonical_delta[4];
    float corrected_delta[4];
    float temporary[4];
    radians *= pose->procedural_weight;
    if (fabsf(radians) < 1.0e-7f) return;
    quat_axis_angle(axis, radians, canonical_delta);
    quat_multiply(correction, canonical_delta, temporary);
    quat_multiply(temporary, inverse_correction, corrected_delta);
    quat_multiply(pose->local[node].rotation, corrected_delta,
                  pose->local[node].rotation);
}

static void apply_role_xyz(MdkrModernPose *pose, int role,
                           float x, float y, float z) {
    static const float axis_x[3] = {1.0f, 0.0f, 0.0f};
    static const float axis_y[3] = {0.0f, 1.0f, 0.0f};
    static const float axis_z[3] = {0.0f, 0.0f, 1.0f};
    apply_role_axis(pose, role, axis_x, x);
    apply_role_axis(pose, role, axis_y, y);
    apply_role_axis(pose, role, axis_z, z);
}

/* Engine-owned reference motion for a reviewed humanoid when the package has
 * no clip for the requested semantic. These are small canonical-space deltas,
 * conjugated through each role's authored rest correction. They are not used
 * for authored-clips-only rigs or over an explicitly mapped clip. */
static void apply_reference_pose(MdkrModernPose *pose) {
    const char *semantic = pose->requested_semantic;
    const float wave = sinf(pose->procedural_time * 5.026548246f);
    const int race = strncmp(semantic, "race.", 5u) == 0;
    const int select = strncmp(semantic, "select.", 7u) == 0;
    float steer = clamp01(pose->normalized_phase) * 2.0f - 1.0f;
    if (!pose->humanoid_retarget_ready ||
        pose->requested_semantic_explicit || (!race && !select)) return;

    if (race) {
        /* Seated reference stance: shoulders reach down/forward, elbows bend,
         * and the legs fold into a kart without translating the hips. */
        apply_role_xyz(pose, 1, -0.12f + wave * 0.015f, 0.0f, 0.0f);
        apply_role_xyz(pose, 2, -0.12f, steer * 0.10f, 0.0f);
        apply_role_xyz(pose, 3, 0.03f, steer * -0.08f, 0.0f);
        apply_role_xyz(pose, 4, 0.0f, -0.42f - steer * 0.10f,
                       -0.96f + steer * 0.08f);
        apply_role_xyz(pose, 5, 0.0f, -1.02f - steer * 0.12f, 0.0f);
        apply_role_xyz(pose, 7, 0.0f, 0.42f - steer * 0.10f,
                       0.96f + steer * 0.08f);
        apply_role_xyz(pose, 8, 0.0f, 1.02f - steer * 0.12f, 0.0f);
        apply_role_xyz(pose, 10, -0.92f, 0.0f, 0.10f);
        apply_role_xyz(pose, 11, 1.12f, 0.0f, 0.0f);
        apply_role_xyz(pose, 13, -0.92f, 0.0f, -0.10f);
        apply_role_xyz(pose, 14, 1.12f, 0.0f, 0.0f);
        if (strcmp(semantic, "race.boost") == 0) {
            apply_role_xyz(pose, 1, -0.16f, 0.0f, 0.0f);
            apply_role_xyz(pose, 3, 0.10f, 0.0f, 0.0f);
        } else if (strcmp(semantic, "race.damage") == 0 ||
                   strcmp(semantic, "race.spin") == 0) {
            apply_role_xyz(pose, 1, 0.0f, 0.0f, wave * 0.22f);
            apply_role_xyz(pose, 3, 0.0f, wave * -0.28f, 0.0f);
        } else if (strcmp(semantic, "race.airborne") == 0) {
            apply_role_xyz(pose, 4, 0.0f, 0.0f, 0.30f);
            apply_role_xyz(pose, 7, 0.0f, 0.0f, -0.30f);
        } else if (strcmp(semantic, "race.land") == 0) {
            apply_role_xyz(pose, 1, 0.24f, 0.0f, 0.0f);
            apply_role_xyz(pose, 10, -0.20f, 0.0f, 0.0f);
            apply_role_xyz(pose, 13, -0.20f, 0.0f, 0.0f);
        } else if (strcmp(semantic, "race.finish_win") == 0) {
            apply_role_xyz(pose, 4, 0.0f, 0.0f, 1.65f);
            apply_role_xyz(pose, 7, 0.0f, 0.0f, -1.65f);
        } else if (strcmp(semantic, "race.finish_lose") == 0) {
            apply_role_xyz(pose, 2, 0.25f, 0.0f, 0.0f);
            apply_role_xyz(pose, 3, 0.32f, 0.0f, 0.0f);
        }
    } else {
        /* Standing reference stance replaces a raw T-pose while preserving
         * the authored hips and root translation used by ground anchoring. */
        apply_role_xyz(pose, 1, wave * 0.015f, 0.0f, 0.0f);
        apply_role_xyz(pose, 2, wave * -0.02f, 0.0f, 0.0f);
        apply_role_xyz(pose, 4, 0.0f, 0.0f, -1.18f + wave * 0.025f);
        apply_role_xyz(pose, 5, 0.0f, -0.12f, 0.0f);
        apply_role_xyz(pose, 7, 0.0f, 0.0f, 1.18f - wave * 0.025f);
        apply_role_xyz(pose, 8, 0.0f, 0.12f, 0.0f);
        if (strcmp(semantic, "select.hover") == 0) {
            apply_role_xyz(pose, 3, 0.0f, wave * 0.12f, 0.0f);
            apply_role_xyz(pose, 8, 0.0f, 0.45f, -0.18f);
        } else if (strcmp(semantic, "select.confirm") == 0) {
            apply_role_xyz(pose, 4, 0.0f, 0.0f, 1.90f);
            apply_role_xyz(pose, 7, 0.0f, 0.0f, -1.90f);
            apply_role_xyz(pose, 1, -0.12f, 0.0f, 0.0f);
        }
    }
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
    pose->pre_contact = (MdkrModernTrs *)malloc(trs_bytes);
    pose->evaluation_order = (uint32_t *)malloc(
        (size_t)pose->node_count * sizeof(*pose->evaluation_order));
    pose->world_previous = (float *)calloc(1u, matrix_bytes);
    pose->world_current = (float *)calloc(1u, matrix_bytes);
    if (pose->local == NULL || pose->blend_from == NULL ||
        pose->sampled == NULL || pose->pre_contact == NULL ||
        pose->evaluation_order == NULL ||
        pose->world_previous == NULL || pose->world_current == NULL) {
        mdkr_modern_pose_shutdown(pose);
        set_error(error, error_size, "could not allocate character pose buffers");
        return 0;
    }
    bind_pose(asset, pose->local, pose->node_count);
    memcpy(pose->blend_from, pose->local, trs_bytes);
    {
        MdkrModernRig rig;
        if (mdkr_modern_character_asset_rig(asset, &rig) &&
            rig.mode == MDKR_MODERN_RIG_HUMANOID_RETARGET_V1 &&
            (rig.flags & MDKR_MODERN_RIG_REVIEWED) != 0u &&
            rig.role_mask == 0xFFFFu &&
            rig.role_count == MDKR_MODERN_HUMANOID_ROLE_COUNT) {
            uint32_t role_record;
            for (role_record = 0u; role_record < rig.role_count;
                 role_record++) {
                MdkrModernRigRole role;
                const char *semantic;
                int role_index;
                (void)mdkr_modern_character_asset_rig_role(
                    asset, role_record, &role);
                semantic = mdkr_modern_character_asset_string(
                    asset, role.semantic);
                role_index = rig_role_index(semantic);
                if (role_index < 0) continue;
                pose->rig_role_nodes[role_index] = role.node;
                memcpy(pose->rig_rest_rotation[role_index],
                       role.rest_rotation, sizeof(role.rest_rotation));
                memcpy(pose->rig_bend_axis[role_index], role.bend_axis,
                       sizeof(role.bend_axis));
                pose->rig_role_mask |= 1u << (unsigned)role_index;
            }
            pose->humanoid_retarget_ready =
                pose->rig_role_mask == 0xFFFFu;
        }
    }
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
    free(pose->pre_contact);
    free(pose->evaluation_order);
    free(pose->world_previous);
    free(pose->world_current);
    memset(pose, 0, sizeof(*pose));
}

int mdkr_modern_pose_set_semantic(MdkrModernPose *pose, const char *semantic,
                                  char *error, size_t error_size) {
    int animation;
    int explicit_match = 0;
    int semantic_changed;
    uint32_t flags;
    float blend;
    if (pose == NULL || !pose->valid || semantic == NULL || semantic[0] == '\0' ||
        !semantic_lookup(pose->asset, semantic, &animation, &flags, &blend,
                         &explicit_match)) {
        set_error(error, error_size, "character semantic and fallback could not resolve");
        return 0;
    }
    semantic_changed = strcmp(pose->requested_semantic, semantic) != 0;
    if (semantic_changed) {
        (void)snprintf(pose->requested_semantic,
                       sizeof(pose->requested_semantic), "%s", semantic);
        pose->requested_semantic_explicit = explicit_match;
        pose->procedural_time = 0.0f;
        pose->normalized_phase = 0.5f;
    }
    if (pose->animation == animation) {
        pose->animation_flags = flags;
        if (semantic_changed) {
            memcpy(pose->blend_from, pose->local,
                   (size_t)pose->node_count * sizeof(*pose->local));
            pose->blend_elapsed = 0.0f;
            pose->blend_duration = blend;
        }
        return 1;
    }
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
        pose->normalized_phase = clamp01(normalized_phase);
    } else {
        pose->time += seconds;
    }
    pose->procedural_time += seconds;
    if (!phase_driven &&
        (pose->animation_flags & MDKR_MODERN_SEMANTIC_LOOP) != 0u) {
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
    pose->procedural_weight = blend_amount;
    apply_reference_pose(pose);
    memcpy(pose->pre_contact, pose->local,
           (size_t)pose->node_count * sizeof(*pose->local));
    if (!evaluate_world(pose, pose->world_current, error, error_size)) return 0;
    pose->generation++;
    if (pose->generation == 0u) pose->generation++;
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

static float vector_normalize(float value[3]) {
    const float length = sqrtf(value[0] * value[0] +
                               value[1] * value[1] +
                               value[2] * value[2]);
    if (!isfinite(length) || length < 1.0e-8f) return 0.0f;
    value[0] /= length;
    value[1] /= length;
    value[2] /= length;
    return length;
}

static void vector_cross(const float left[3], const float right[3],
                         float output[3]) {
    output[0] = left[1] * right[2] - left[2] * right[1];
    output[1] = left[2] * right[0] - left[0] * right[2];
    output[2] = left[0] * right[1] - left[1] * right[0];
}

static void quat_rotate_vector(const float rotation[4], const float input[3],
                               float output[3]) {
    const float twice_cross[3] = {
        2.0f * (rotation[1] * input[2] - rotation[2] * input[1]),
        2.0f * (rotation[2] * input[0] - rotation[0] * input[2]),
        2.0f * (rotation[0] * input[1] - rotation[1] * input[0])
    };
    output[0] = input[0] + rotation[3] * twice_cross[0] +
        rotation[1] * twice_cross[2] - rotation[2] * twice_cross[1];
    output[1] = input[1] + rotation[3] * twice_cross[1] +
        rotation[2] * twice_cross[0] - rotation[0] * twice_cross[2];
    output[2] = input[2] + rotation[3] * twice_cross[2] +
        rotation[0] * twice_cross[1] - rotation[1] * twice_cross[0];
}

static int axis_world_to_parent(const float *parent_world,
                                const float world_axis[3],
                                float local_axis[3]) {
    float inverse[16];
    if (parent_world == NULL) {
        memcpy(local_axis, world_axis, sizeof(float) * 3u);
        return 1;
    }
    if (!matrix_inverse_affine(parent_world, inverse)) return 0;
    local_axis[0] = inverse[0] * world_axis[0] +
                    inverse[4] * world_axis[1] +
                    inverse[8] * world_axis[2];
    local_axis[1] = inverse[1] * world_axis[0] +
                    inverse[5] * world_axis[1] +
                    inverse[9] * world_axis[2];
    local_axis[2] = inverse[2] * world_axis[0] +
                    inverse[6] * world_axis[1] +
                    inverse[10] * world_axis[2];
    return vector_normalize(local_axis) != 0.0f;
}

static int ccd_contact_step(MdkrModernPose *pose, int joint_role,
                            int end_role, const float target[3],
                            char *error, size_t error_size) {
    const uint32_t joint_node = pose->rig_role_nodes[joint_role];
    const uint32_t end_node = pose->rig_role_nodes[end_role];
    const float *joint_world = pose->world_current +
        (size_t)joint_node * 16u;
    const float *end_world = pose->world_current + (size_t)end_node * 16u;
    MdkrModernNode joint;
    const float *parent_world = NULL;
    float current[3] = {
        end_world[12] - joint_world[12],
        end_world[13] - joint_world[13],
        end_world[14] - joint_world[14]
    };
    float desired[3] = {
        target[0] - joint_world[12], target[1] - joint_world[13],
        target[2] - joint_world[14]
    };
    float world_axis[3];
    float local_axis[3];
    float dot;
    float angle;
    float delta[4];
    float rotated[4];
    if (vector_normalize(current) == 0.0f ||
        vector_normalize(desired) == 0.0f) return 1;
    if (!mdkr_modern_character_asset_node(pose->asset, joint_node, &joint)) {
        set_error(error, error_size,
                  "vehicle contact joint could not be resolved");
        return 0;
    }
    if (joint.parent >= 0) {
        parent_world = pose->world_current +
            (size_t)joint.parent * 16u;
    }
    dot = current[0] * desired[0] + current[1] * desired[1] +
          current[2] * desired[2];
    if (dot > 1.0f) dot = 1.0f;
    if (dot < -1.0f) dot = -1.0f;
    vector_cross(current, desired, world_axis);
    if (vector_normalize(world_axis) == 0.0f) {
        if (dot > 0.99999f) return 1;
        memcpy(local_axis, pose->rig_bend_axis[joint_role],
               sizeof(local_axis));
        if (vector_normalize(local_axis) != 0.0f) {
            float parent_axis[3];
            /* The package contract is joint-local. This solver pre-multiplies
             * the node rotation, so convert the preference into parent space
             * before constructing that delta. */
            quat_rotate_vector(pose->local[joint_node].rotation, local_axis,
                               parent_axis);
            memcpy(local_axis, parent_axis, sizeof(parent_axis));
            (void)vector_normalize(local_axis);
        } else {
            const float candidate[3] = {
                fabsf(current[0]) < 0.8f ? 1.0f : 0.0f,
                fabsf(current[0]) < 0.8f ? 0.0f : 1.0f,
                0.0f
            };
            vector_cross(current, candidate, world_axis);
            if (vector_normalize(world_axis) == 0.0f ||
                !axis_world_to_parent(parent_world, world_axis,
                                      local_axis)) {
                set_error(error, error_size,
                          "automatic contact bend axis could not be resolved");
                return 0;
            }
        }
    } else {
        if (!axis_world_to_parent(parent_world, world_axis, local_axis)) {
            set_error(error, error_size,
                      "vehicle contact joint parent is not invertible");
            return 0;
        }
    }
    angle = acosf(dot);
    if (angle > 0.45f) angle = 0.45f;
    quat_axis_angle(local_axis, angle, delta);
    quat_multiply(delta, pose->local[joint_node].rotation, rotated);
    memcpy(pose->local[joint_node].rotation, rotated, sizeof(rotated));
    return evaluate_world(pose, pose->world_current, error, error_size);
}

static void contact_offset_to_source(uint32_t source_forward, float scale,
                                     const float canonical[3],
                                     float source[3]) {
    const float x = canonical[0] * scale;
    const float y = canonical[1] * scale;
    const float z = canonical[2] * scale;
    source[1] = y;
    if (source_forward == 1u) {
        source[0] = -x; source[2] = -z;
    } else if (source_forward == 2u) {
        source[0] = z; source[2] = -x;
    } else if (source_forward == 3u) {
        source[0] = -z; source[2] = x;
    } else {
        source[0] = x; source[2] = z;
    }
}

int mdkr_modern_pose_apply_vehicle_contacts(
    MdkrModernPose *pose, MdkrModernCharacterContext context,
    const MdkrModernCalibration *calibration,
    const float contact_offsets[MDKR_MODERN_CHARACTER_CONTACTS][3],
    char *error, size_t error_size) {
    static const float offsets[3][MDKR_MODERN_CHARACTER_CONTACTS][3] = {
        { {0.27f, 0.20f, 0.34f}, {-0.27f, 0.20f, 0.34f},
          {0.17f, -0.35f, 0.23f}, {-0.17f, -0.35f, 0.23f} },
        { {0.31f, 0.16f, 0.31f}, {-0.31f, 0.16f, 0.31f},
          {0.20f, -0.30f, 0.28f}, {-0.20f, -0.30f, 0.28f} },
        { {0.25f, 0.18f, 0.30f}, {-0.13f, 0.08f, 0.38f},
          {0.18f, -0.31f, 0.26f}, {-0.18f, -0.31f, 0.26f} }
    };
    static const int chains[MDKR_MODERN_CHARACTER_CONTACTS][3] = {
        {4, 5, 6}, {7, 8, 9}, {10, 11, 12}, {13, 14, 15}
    };
    const float *hips;
    float source_units_per_metre;
    float max_error = 0.0f;
    unsigned contact;
    unsigned iteration;
    if (pose == NULL || !pose->valid || calibration == NULL ||
        contact_offsets == NULL ||
        context < MDKR_CHARACTER_CONTEXT_CAR ||
        context > MDKR_CHARACTER_CONTEXT_PLANE) {
        set_error(error, error_size, "vehicle contact arguments are invalid");
        return 0;
    }
    if (!pose->humanoid_retarget_ready ||
        pose->requested_semantic_explicit) return 1;
    if (pose->contact_generation == pose->generation &&
        pose->contact_context == (uint32_t)context) return 1;
    if (!isfinite(calibration->source_height) ||
        !isfinite(calibration->target_height) ||
        calibration->source_height <= 1.0e-6f ||
        calibration->target_height <= 1.0e-6f ||
        calibration->source_forward > 3u) {
        set_error(error, error_size, "vehicle contact calibration is invalid");
        return 0;
    }
    for (contact = 0u; contact < MDKR_MODERN_CHARACTER_CONTACTS; contact++) {
        unsigned axis;
        for (axis = 0u; axis < 3u; axis++) {
            if (!isfinite(contact_offsets[contact][axis]) ||
                contact_offsets[contact][axis] < -1.0f ||
                contact_offsets[contact][axis] > 1.0f) {
                set_error(error, error_size,
                          "vehicle contact offset is outside its safe range");
                return 0;
            }
        }
    }
    source_units_per_metre = calibration->source_height /
                             calibration->target_height;
    memcpy(pose->local, pose->pre_contact,
           (size_t)pose->node_count * sizeof(*pose->local));
    if (!evaluate_world(pose, pose->world_current, error, error_size)) return 0;
    hips = pose->world_current +
           (size_t)pose->rig_role_nodes[0] * 16u;
    for (contact = 0u; contact < MDKR_MODERN_CHARACTER_CONTACTS; contact++) {
        float adjusted_offset[3];
        float offset[3];
        float target[3];
        adjusted_offset[0] = offsets[(unsigned)context - 1u][contact][0] +
                             contact_offsets[contact][0];
        adjusted_offset[1] = offsets[(unsigned)context - 1u][contact][1] +
                             contact_offsets[contact][1];
        adjusted_offset[2] = offsets[(unsigned)context - 1u][contact][2] +
                             contact_offsets[contact][2];
        contact_offset_to_source(
            calibration->source_forward, source_units_per_metre,
            adjusted_offset, offset);
        target[0] = hips[12] + offset[0];
        target[1] = hips[13] + offset[1];
        target[2] = hips[14] + offset[2];
        memcpy(pose->contact_target[contact], target, sizeof(target));
        for (iteration = 0u; iteration < 4u; iteration++) {
            if (!ccd_contact_step(pose, chains[contact][1],
                                  chains[contact][2], target,
                                  error, error_size) ||
                !ccd_contact_step(pose, chains[contact][0],
                                  chains[contact][2], target,
                                  error, error_size)) return 0;
        }
        {
            const float *end = pose->world_current +
                (size_t)pose->rig_role_nodes[chains[contact][2]] * 16u;
            float difference[3] = {
                end[12] - target[0], end[13] - target[1],
                end[14] - target[2]
            };
            const float error_source = sqrtf(
                difference[0] * difference[0] +
                difference[1] * difference[1] +
                difference[2] * difference[2]);
            const float error_metres = error_source / source_units_per_metre;
            if (error_metres > max_error) max_error = error_metres;
        }
    }
    if (!isfinite(max_error)) {
        set_error(error, error_size, "vehicle contact solve became non-finite");
        return 0;
    }
    pose->contact_valid_mask = 0u;
    for (contact = 0u; contact < MDKR_MODERN_CHARACTER_CONTACTS; contact++) {
        const float *root = pose->world_current +
            (size_t)pose->rig_role_nodes[chains[contact][0]] * 16u;
        const float *bend = pose->world_current +
            (size_t)pose->rig_role_nodes[chains[contact][1]] * 16u;
        const float *end = pose->world_current +
            (size_t)pose->rig_role_nodes[chains[contact][2]] * 16u;
        float difference[3];
        unsigned axis;
        for (axis = 0u; axis < 3u; axis++) {
            pose->contact_chain_root[contact][axis] = root[12u + axis];
            pose->contact_bend[contact][axis] = bend[12u + axis];
            pose->contact_end[contact][axis] = end[12u + axis];
            difference[axis] = pose->contact_end[contact][axis] -
                pose->contact_target[contact][axis];
        }
        pose->contact_error[contact] = sqrtf(
            difference[0] * difference[0] +
            difference[1] * difference[1] +
            difference[2] * difference[2]);
        if (!isfinite(pose->contact_error[contact])) {
            pose->contact_valid_mask = 0u;
            set_error(error, error_size,
                      "vehicle contact witness became non-finite");
            return 0;
        }
        pose->contact_valid_mask |= 1u << contact;
    }
    pose->contact_max_error = max_error;
    pose->contact_context = (uint32_t)context;
    pose->contact_generation = pose->generation;
    if (!pose->contacts_initialized) {
        memcpy(pose->world_previous, pose->world_current,
               (size_t)pose->node_count * 16u * sizeof(float));
        pose->contacts_initialized = 1;
    }
    set_error(error, error_size, "");
    return 1;
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
