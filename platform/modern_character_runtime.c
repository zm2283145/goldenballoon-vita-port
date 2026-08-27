#include "modern_character_runtime.h"
#include "workshop_preview_runtime.h"

#include "fast3d/gfx_pc_dkr.h"
#include "f3ddkr.h"
#include "modern_character_pose.h"
#include "modern_character_render.h"
#include "modern_character_donor.h"
#include "modern_character_identity.h"
#include "modern_character_lod.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MODERN_RUNTIME_ACTIVE_POOLS MDKR_MODERN_CHARACTER_PLAYERS
/* A complete four-player replacement must be prepared without releasing the
 * four active assets it may replace. Reused assets only add references, while
 * a disjoint plan can require all four staging slots until atomic publication. */
#define MODERN_RUNTIME_POOLS \
    (MODERN_RUNTIME_ACTIVE_POOLS * 2)
#define MODERN_RUNTIME_MAX_BONES 256u
#define MODERN_RUNTIME_PLAYABLE_LIST_MAX \
    (MDKR_MODERN_CHARACTER_MAX * (MDKR_MODERN_CHARACTER_ID_MAX + 1u))

typedef struct MdkrModernRuntimePool {
    int registry_index;
    int references;
    MdkrModernCharacterAsset asset;
    MdkrModernRenderAsset render;
    MdkrModernCharacterDefinition definition;
    MdkrModernDecodedIdentity identity;
} MdkrModernRuntimePool;

typedef struct MdkrModernRuntimePlayer {
    int pool;
    MdkrModernPose pose;
    MdkrModernCharacterTuning tuning;
    char semantic[96];
    float palette[MODERN_RUNTIME_MAX_BONES * 16u];
    float previous_palette[MODERN_RUNTIME_MAX_BONES * 16u];
    uint32_t tokens[MDKR_MODERN_CHARACTER_MAX_PRIMITIVES];
    uint64_t identity_revision;
    float focus_center[MDKR_CHARACTER_CONTEXT_COUNT][3];
    float focus_radius[MDKR_CHARACTER_CONTEXT_COUNT];
    uint32_t focus_valid_mask;
    MdkrModernCharacterFitDiagnostics
        fit_diagnostics[MDKR_CHARACTER_CONTEXT_COUNT];
    uint32_t fit_diagnostics_valid_mask;
    MdkrModernCharacterContactDiagnostics
        contact_diagnostics[MDKR_CHARACTER_CONTEXT_COUNT];
    uint32_t contact_diagnostics_valid_mask;
    uint32_t selected_lod[MDKR_CHARACTER_CONTEXT_COUNT]
                         [MDKR_MODERN_CHARACTER_VIEWS];
    uint32_t selected_lod_valid_mask;
    uint64_t inspection_generation;
    float inspection_dwell_seconds;
    int inspection_to_pose;
    int inspection_counted_blend;
} MdkrModernRuntimePlayer;

typedef struct MdkrModernPendingPlayer {
    int pool;
    MdkrModernPose pose;
    MdkrModernCharacterTuning tuning;
    char semantic[96];
} MdkrModernPendingPlayer;

static MdkrModernCharacterRegistry s_registry;
static MdkrModernRuntimePool s_pools[MODERN_RUNTIME_POOLS];
static MdkrModernRuntimePlayer s_players[MDKR_MODERN_CHARACTER_PLAYERS];
static int s_initialized;
static uint64_t s_replacement_draws;
static uint64_t s_replacement_primitives;
static uint64_t s_hidden_donor_batches;
static uint64_t s_contact_solves;
static uint64_t s_contact_error_micrometres_sum;
static uint64_t s_contact_error_micrometres_max;
static uint64_t s_identity_revision;
static uint64_t s_inspection_pose_ticks;
static uint64_t s_inspection_pose_fallback_ticks;
static uint64_t s_inspection_transition_switches;
static uint64_t s_inspection_transition_blending_ticks;
static uint64_t s_inspection_transition_completions;
static uint32_t s_inspection_from_blend_milliseconds;
static uint32_t s_inspection_to_blend_milliseconds;
static MdkrModernCharacterMotionSource s_inspection_from_motion_source;
static MdkrModernCharacterMotionSource s_inspection_to_motion_source;
static uint64_t s_inspection_generation;
static char s_inspection_semantic[32];
static float s_inspection_phase;
static char s_inspection_to_semantic[32];
static float s_inspection_to_phase;
static int s_playable_filter_active;
static char s_playable_list[MODERN_RUNTIME_PLAYABLE_LIST_MAX];
/* A disabled cache is admitted only for the one launcher-authenticated exact
 * Workshop preview that named it. Ordinary runtime discovery continues to
 * scan enabled .mdkc files only. */
static char s_workshop_preview_package[MDKR_MODERN_CHARACTER_ID_MAX + 1u];

static int playable_list_contains(const char *list, const char *id) {
    const char *begin;
    size_t id_length;
    if (list == NULL || id == NULL || id[0] == '\0') return 0;
    id_length = strlen(id);
    begin = list;
    while (*begin != '\0') {
        const char *end = strchr(begin, ',');
        size_t length = end != NULL ? (size_t)(end - begin) : strlen(begin);
        if (length == id_length && memcmp(begin, id, length) == 0) return 1;
        if (end == NULL) break;
        begin = end + 1;
    }
    return 0;
}

static void set_error(char *error, size_t size, const char *message) {
    if (error != NULL && size != 0u) {
        (void)snprintf(error, size, "%s", message != NULL ? message : "unknown error");
    }
}

static int inspection_semantic_valid(const char *semantic) {
    static const char *const known[] = {
#define MDKR_CHARACTER_INSPECTION_SEMANTIC(suffix, value, label) value,
        MDKR_MODERN_CHARACTER_INSPECTION_SEMANTICS(
            MDKR_CHARACTER_INSPECTION_SEMANTIC)
#undef MDKR_CHARACTER_INSPECTION_SEMANTIC
    };
    size_t index;
    if (semantic == NULL || semantic[0] == '\0') return 0;
    for (index = 0u; index < sizeof(known) / sizeof(known[0]); index++) {
        if (strcmp(semantic, known[index]) == 0) return 1;
    }
    return 0;
}

int mdkr_modern_character_set_inspection_pose(
    const char *semantic, float normalized_phase,
    char *error, size_t error_size) {
    if (!inspection_semantic_valid(semantic) ||
        !isfinite(normalized_phase) || normalized_phase < 0.0f ||
        normalized_phase > 1.0f) {
        set_error(error, error_size,
                  "character inspection pose or phase is invalid");
        return 0;
    }
    (void)snprintf(s_inspection_semantic,
                   sizeof(s_inspection_semantic), "%s", semantic);
    s_inspection_phase = normalized_phase;
    s_inspection_to_semantic[0] = '\0';
    s_inspection_to_phase = 0.0f;
    s_inspection_from_blend_milliseconds = 0u;
    s_inspection_to_blend_milliseconds = 0u;
    s_inspection_from_motion_source = MDKR_MODERN_CHARACTER_MOTION_NONE;
    s_inspection_to_motion_source = MDKR_MODERN_CHARACTER_MOTION_NONE;
    s_inspection_generation++;
    if (s_inspection_generation == 0u) s_inspection_generation++;
    set_error(error, error_size, "");
    return 1;
}

int mdkr_modern_character_set_inspection_transition(
    const char *from_semantic, float from_normalized_phase,
    const char *to_semantic, float to_normalized_phase,
    char *error, size_t error_size) {
    if (!inspection_semantic_valid(from_semantic) ||
        !inspection_semantic_valid(to_semantic) ||
        strcmp(from_semantic, to_semantic) == 0 ||
        !isfinite(from_normalized_phase) || from_normalized_phase < 0.0f ||
        from_normalized_phase > 1.0f || !isfinite(to_normalized_phase) ||
        to_normalized_phase < 0.0f || to_normalized_phase > 1.0f) {
        set_error(error, error_size,
                  "character inspection transition is invalid");
        return 0;
    }
    (void)snprintf(s_inspection_semantic,
                   sizeof(s_inspection_semantic), "%s", from_semantic);
    s_inspection_phase = from_normalized_phase;
    (void)snprintf(s_inspection_to_semantic,
                   sizeof(s_inspection_to_semantic), "%s", to_semantic);
    s_inspection_to_phase = to_normalized_phase;
    s_inspection_from_blend_milliseconds = 0u;
    s_inspection_to_blend_milliseconds = 0u;
    s_inspection_from_motion_source = MDKR_MODERN_CHARACTER_MOTION_NONE;
    s_inspection_to_motion_source = MDKR_MODERN_CHARACTER_MOTION_NONE;
    s_inspection_generation++;
    if (s_inspection_generation == 0u) s_inspection_generation++;
    set_error(error, error_size, "");
    return 1;
}

void mdkr_modern_character_clear_inspection_pose(void) {
    s_inspection_semantic[0] = '\0';
    s_inspection_phase = 0.0f;
    s_inspection_to_semantic[0] = '\0';
    s_inspection_to_phase = 0.0f;
    s_inspection_from_blend_milliseconds = 0u;
    s_inspection_to_blend_milliseconds = 0u;
    s_inspection_from_motion_source = MDKR_MODERN_CHARACTER_MOTION_NONE;
    s_inspection_to_motion_source = MDKR_MODERN_CHARACTER_MOTION_NONE;
    s_inspection_generation++;
    if (s_inspection_generation == 0u) s_inspection_generation++;
}

static void matrix_identity(float output[16]) {
    memset(output, 0, sizeof(float) * 16u);
    output[0] = output[5] = output[10] = output[15] = 1.0f;
}

void mdkr_modern_character_tuning_defaults(MdkrModernCharacterTuning *out) {
    unsigned context;
    if (out == NULL) return;
    memset(out, 0, sizeof(*out));
    out->scale = 1.0f;
    out->animation_speed = 1.0f;
    out->vehicle_mask = 0x7u;
    for (context = 0u; context < MDKR_CHARACTER_CONTEXT_COUNT; context++) {
        out->context[context].scale = 1.0f;
    }
}

int mdkr_modern_character_tuning_validate(MdkrModernCharacterTuning *tuning,
                                          char *error, size_t error_size) {
    unsigned axis;
    unsigned context;
    if (tuning == NULL || !isfinite(tuning->scale) ||
        tuning->scale < 0.1f || tuning->scale > 5.0f ||
        !isfinite(tuning->animation_speed) ||
        tuning->animation_speed < 0.05f || tuning->animation_speed > 4.0f ||
        !isfinite(tuning->lod_bias) || tuning->lod_bias < -3.0f ||
        tuning->lod_bias > 3.0f || (tuning->vehicle_mask & ~0x7u) != 0u ||
        tuning->vehicle_mask == 0u) {
        set_error(error, error_size,
                  "character tuning is outside its safe range");
        return 0;
    }
    for (axis = 0u; axis < 3u; axis++) {
        if (!isfinite(tuning->translation[axis]) ||
            tuning->translation[axis] < -500.0f ||
            tuning->translation[axis] > 500.0f ||
            !isfinite(tuning->rotation_degrees[axis]) ||
            tuning->rotation_degrees[axis] < -180.0f ||
            tuning->rotation_degrees[axis] > 180.0f) {
            set_error(error, error_size,
                      "character translation or rotation is outside its safe range");
            return 0;
        }
    }
    for (context = 0u; context < MDKR_CHARACTER_CONTEXT_COUNT; context++) {
        MdkrModernCharacterAdjustment *adjustment = &tuning->context[context];
        if (!isfinite(adjustment->scale) || adjustment->scale < 0.1f ||
            adjustment->scale > 5.0f) {
            set_error(error, error_size,
                      "character context scale is outside its safe range");
            return 0;
        }
        for (axis = 0u; axis < 3u; axis++) {
            if (!isfinite(adjustment->translation[axis]) ||
                adjustment->translation[axis] < -10.0f ||
                adjustment->translation[axis] > 10.0f ||
                !isfinite(adjustment->rotation_degrees[axis]) ||
                adjustment->rotation_degrees[axis] < -180.0f ||
                adjustment->rotation_degrees[axis] > 180.0f) {
                set_error(error, error_size,
                          "character context transform is outside its safe range");
                return 0;
            }
        }
        {
            unsigned contact;
            for (contact = 0u; contact < MDKR_MODERN_CHARACTER_CONTACTS;
                 contact++) {
                for (axis = 0u; axis < 3u; axis++) {
                    if (!isfinite(
                            tuning->contact_offset[context][contact][axis]) ||
                        tuning->contact_offset[context][contact][axis] < -1.0f ||
                        tuning->contact_offset[context][contact][axis] > 1.0f) {
                        set_error(error, error_size,
                                  "character contact offset is outside its safe range");
                        return 0;
                    }
                }
            }
        }
    }
    set_error(error, error_size, "");
    return 1;
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

static void matrix_transform_point(const float matrix[16],
                                   const float point[3], float output[3]) {
    output[0] = matrix[0] * point[0] + matrix[4] * point[1] +
                matrix[8] * point[2] + matrix[12];
    output[1] = matrix[1] * point[0] + matrix[5] * point[1] +
                matrix[9] * point[2] + matrix[13];
    output[2] = matrix[2] * point[0] + matrix[6] * point[1] +
                matrix[10] * point[2] + matrix[14];
}

static int calibration_fit_diagnostics(
    const MdkrModernCalibration *calibration, const float transform[16],
    const float source_anchor[3],
    MdkrModernCharacterFitDiagnostics *out) {
    float source_forward[3] = {0.0f, 0.0f, 1.0f};
    float forward_length_squared;
    unsigned axis;
    unsigned corner;
    if (calibration == NULL || transform == NULL || source_anchor == NULL ||
        out == NULL || calibration->source_forward > 3u) return 0;
    memset(out, 0, sizeof(*out));
    if (calibration->source_forward == 1u) {
        source_forward[2] = -1.0f;
    } else if (calibration->source_forward == 2u) {
        source_forward[0] = 1.0f;
        source_forward[2] = 0.0f;
    } else if (calibration->source_forward == 3u) {
        source_forward[0] = -1.0f;
        source_forward[2] = 0.0f;
    }
    for (axis = 0u; axis < 3u; ++axis) {
        if (!isfinite(calibration->bounds_min[axis]) ||
            !isfinite(calibration->bounds_max[axis]) ||
            calibration->bounds_min[axis] > calibration->bounds_max[axis] ||
            !isfinite(source_anchor[axis])) return 0;
        out->bounds_min[axis] = INFINITY;
        out->bounds_max[axis] = -INFINITY;
        out->forward[axis] = transform[axis] * source_forward[0] +
            transform[4u + axis] * source_forward[1] +
            transform[8u + axis] * source_forward[2];
    }
    for (corner = 0u; corner < 8u; ++corner) {
        float source[3];
        float fitted[3];
        for (axis = 0u; axis < 3u; ++axis) {
            source[axis] = (corner & (1u << axis)) != 0u
                ? calibration->bounds_max[axis]
                : calibration->bounds_min[axis];
        }
        matrix_transform_point(transform, source, fitted);
        for (axis = 0u; axis < 3u; ++axis) {
            if (!isfinite(fitted[axis])) return 0;
            if (fitted[axis] < out->bounds_min[axis]) {
                out->bounds_min[axis] = fitted[axis];
            }
            if (fitted[axis] > out->bounds_max[axis]) {
                out->bounds_max[axis] = fitted[axis];
            }
        }
    }
    matrix_transform_point(transform, source_anchor, out->anchor);
    forward_length_squared = out->forward[0] * out->forward[0] +
        out->forward[1] * out->forward[1] +
        out->forward[2] * out->forward[2];
    if (!isfinite(forward_length_squared) ||
        forward_length_squared < 1.0e-12f) return 0;
    {
        const float inverse_length = 1.0f / sqrtf(forward_length_squared);
        for (axis = 0u; axis < 3u; ++axis) {
            out->forward[axis] *= inverse_length;
            if (!isfinite(out->anchor[axis]) ||
                !isfinite(out->forward[axis])) return 0;
        }
    }
    return 1;
}

static int calibration_focus(const MdkrModernCalibration *calibration,
                             const float transform[16], float center[3],
                             float *radius) {
    float source_center[3];
    float maximum_distance_squared = 0.0f;
    unsigned axis;
    unsigned corner;
    if (calibration == NULL || transform == NULL || center == NULL ||
        radius == NULL) return 0;
    for (axis = 0u; axis < 3u; axis++) {
        if (!isfinite(calibration->bounds_min[axis]) ||
            !isfinite(calibration->bounds_max[axis]) ||
            calibration->bounds_min[axis] > calibration->bounds_max[axis]) {
            return 0;
        }
        source_center[axis] =
            (calibration->bounds_min[axis] +
             calibration->bounds_max[axis]) * 0.5f;
    }
    matrix_transform_point(transform, source_center, center);
    for (corner = 0u; corner < 8u; corner++) {
        float source[3];
        float fitted[3];
        float distance_squared = 0.0f;
        for (axis = 0u; axis < 3u; axis++) {
            source[axis] = (corner & (1u << axis)) != 0u
                ? calibration->bounds_max[axis]
                : calibration->bounds_min[axis];
        }
        matrix_transform_point(transform, source, fitted);
        for (axis = 0u; axis < 3u; axis++) {
            const float delta = fitted[axis] - center[axis];
            distance_squared += delta * delta;
        }
        if (!isfinite(distance_squared)) return 0;
        if (distance_squared > maximum_distance_squared) {
            maximum_distance_squared = distance_squared;
        }
    }
    *radius = sqrtf(maximum_distance_squared);
    return isfinite(center[0]) && isfinite(center[1]) &&
           isfinite(center[2]) && isfinite(*radius) && *radius > 0.0f;
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

static void quaternion_multiply(const float left[4], const float right[4],
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
}

static void definition_matrix(const MdkrModernCharacterDefinition *definition,
                              const MdkrModernCharacterTuning *tuning,
                              float output[16]) {
    const float radians = 0.00872664625997164788f;
    float half_x = tuning->rotation_degrees[0] * radians;
    float half_y = tuning->rotation_degrees[1] * radians;
    float half_z = tuning->rotation_degrees[2] * radians;
    float cx = cosf(half_x), sxr = sinf(half_x);
    float cy = cosf(half_y), syr = sinf(half_y);
    float cz = cosf(half_z), szr = sinf(half_z);
    float adjustment[4] = {
        sxr * cy * cz - cx * syr * szr,
        cx * syr * cz + sxr * cy * szr,
        cx * cy * szr - sxr * syr * cz,
        cx * cy * cz + sxr * syr * szr,
    };
    float rotation[4];
    float x, y, z, w;
    float sx = definition->scale[0] * tuning->scale;
    float sy = definition->scale[1] * tuning->scale;
    float sz = definition->scale[2] * tuning->scale;
    quaternion_multiply(adjustment, definition->rotation, rotation);
    x = rotation[0];
    y = rotation[1];
    z = rotation[2];
    w = rotation[3];
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
    output[12] = definition->translation[0] + tuning->translation[0];
    output[13] = definition->translation[1] + tuning->translation[1];
    output[14] = definition->translation[2] + tuning->translation[2];
}

static void quaternion_transform_matrix(const float rotation[4], float scale,
                                        const float translation[3],
                                        float output[16]) {
    const float x = rotation[0], y = rotation[1];
    const float z = rotation[2], w = rotation[3];
    matrix_identity(output);
    output[0] = (1.0f - 2.0f * (y * y + z * z)) * scale;
    output[1] = (2.0f * (x * y + z * w)) * scale;
    output[2] = (2.0f * (x * z - y * w)) * scale;
    output[4] = (2.0f * (x * y - z * w)) * scale;
    output[5] = (1.0f - 2.0f * (x * x + z * z)) * scale;
    output[6] = (2.0f * (y * z + x * w)) * scale;
    output[8] = (2.0f * (x * z + y * w)) * scale;
    output[9] = (2.0f * (y * z - x * w)) * scale;
    output[10] = (1.0f - 2.0f * (x * x + y * y)) * scale;
    output[12] = translation[0];
    output[13] = translation[1];
    output[14] = translation[2];
}

static void adjustment_matrix(const MdkrModernCharacterAdjustment *adjustment,
                              float output[16]) {
    const float radians = 0.00872664625997164788f;
    const float half_x = adjustment->rotation_degrees[0] * radians;
    const float half_y = adjustment->rotation_degrees[1] * radians;
    const float half_z = adjustment->rotation_degrees[2] * radians;
    const float cx = cosf(half_x), sx = sinf(half_x);
    const float cy = cosf(half_y), sy = sinf(half_y);
    const float cz = cosf(half_z), sz = sinf(half_z);
    const float rotation[4] = {
        sx * cy * cz - cx * sy * sz,
        cx * sy * cz + sx * cy * sz,
        cx * cy * sz - sx * sy * cz,
        cx * cy * cz + sx * sy * sz,
    };
    quaternion_transform_matrix(rotation, adjustment->scale,
                                adjustment->translation, output);
}

static int attachment_for_context(const MdkrModernCharacterAsset *asset,
                                  MdkrModernCharacterContext context,
                                  MdkrModernAttachment *out) {
    const MdkrModernSectionView *attachments =
        mdkr_modern_character_asset_section(asset, MDKR_MDKC_ATTACHMENTS);
    uint32_t index;
    memset(out, 0, sizeof(*out));
    out->context = (uint32_t)context;
    out->rotation[3] = 1.0f;
    out->scale = 1.0f;
    if (attachments == NULL) {
        out->flags = context == MDKR_CHARACTER_CONTEXT_SELECT ? 1u : 0u;
        return 1;
    }
    for (index = 0u; index < attachments->count; index++) {
        MdkrModernAttachment candidate;
        (void)mdkr_modern_character_asset_attachment(asset, index, &candidate);
        if (candidate.context == (uint32_t)context) {
            *out = candidate;
            return 1;
        }
    }
    return 0;
}

static int parse_environment_float(const char *prefix, const char *suffix,
                                   float minimum, float maximum, float *output) {
    char name[128];
    char *end = NULL;
    const char *text;
    float value;
    int written = snprintf(name, sizeof(name), "%s_%s", prefix, suffix);
    if (written < 0 || (size_t)written >= sizeof(name)) return 0;
    text = getenv(name);
    if (text == NULL || text[0] == '\0') return 1;
    if (text[0] == ' ' || text[0] == '\t' || text[0] == '\r' ||
        text[0] == '\n') return 0;
    errno = 0;
    value = strtof(text, &end);
    if (errno != 0 || end == text || *end != '\0' || !isfinite(value) ||
        value < minimum || value > maximum) return 0;
    *output = value;
    return 1;
}

static int apply_tuning_environment(const char *prefix,
                                    MdkrModernCharacterTuning *tuning) {
    static const char *context_names[MDKR_CHARACTER_CONTEXT_COUNT] = {
        "SELECT", "CAR", "HOVERCRAFT", "PLANE"
    };
    char name[128];
    char *end = NULL;
    const char *text;
    unsigned long mask;
    int valid = 1;
    int written;
    unsigned context;
    unsigned axis;
    valid &= parse_environment_float(prefix, "SCALE", 0.1f, 5.0f,
                                     &tuning->scale);
    valid &= parse_environment_float(prefix, "OFFSET_X", -500.0f, 500.0f,
                                     &tuning->translation[0]);
    valid &= parse_environment_float(prefix, "OFFSET_Y", -500.0f, 500.0f,
                                     &tuning->translation[1]);
    valid &= parse_environment_float(prefix, "OFFSET_Z", -500.0f, 500.0f,
                                     &tuning->translation[2]);
    valid &= parse_environment_float(prefix, "ROTATION_X", -180.0f, 180.0f,
                                     &tuning->rotation_degrees[0]);
    valid &= parse_environment_float(prefix, "ROTATION_Y", -180.0f, 180.0f,
                                     &tuning->rotation_degrees[1]);
    valid &= parse_environment_float(prefix, "ROTATION_Z", -180.0f, 180.0f,
                                     &tuning->rotation_degrees[2]);
    valid &= parse_environment_float(prefix, "ANIMATION_SPEED", 0.05f, 4.0f,
                                     &tuning->animation_speed);
    valid &= parse_environment_float(prefix, "LOD_BIAS", -3.0f, 3.0f,
                                     &tuning->lod_bias);
    written = snprintf(name, sizeof(name), "%s_VEHICLE_MASK", prefix);
    if (written < 0 || (size_t)written >= sizeof(name)) return 0;
    text = getenv(name);
    if (text != NULL && text[0] != '\0') {
        errno = 0;
        mask = strtoul(text, &end, 10);
        if (errno != 0 || text[0] == ' ' || text[0] == '\t' ||
            text[0] == '\r' || text[0] == '\n' || end == text ||
            *end != '\0' || mask == 0u || mask > 7u) valid = 0;
        else tuning->vehicle_mask = (uint32_t)mask;
    }
    for (context = 0u; context < MDKR_CHARACTER_CONTEXT_COUNT; context++) {
        static const char *axis_names[3] = {"X", "Y", "Z"};
        char suffix[64];
        (void)snprintf(suffix, sizeof(suffix), "%s_SCALE",
                       context_names[context]);
        valid &= parse_environment_float(
            prefix, suffix, 0.1f, 5.0f,
            &tuning->context[context].scale);
        for (axis = 0u; axis < 3u; axis++) {
            (void)snprintf(suffix, sizeof(suffix), "%s_OFFSET_%s",
                           context_names[context], axis_names[axis]);
            valid &= parse_environment_float(
                prefix, suffix, -10.0f, 10.0f,
                &tuning->context[context].translation[axis]);
            (void)snprintf(suffix, sizeof(suffix), "%s_ROTATION_%s",
                           context_names[context], axis_names[axis]);
            valid &= parse_environment_float(
                prefix, suffix, -180.0f, 180.0f,
                &tuning->context[context].rotation_degrees[axis]);
        }
        if (context != MDKR_CHARACTER_CONTEXT_SELECT) {
            static const char *contact_names[MDKR_MODERN_CHARACTER_CONTACTS] = {
                "HAND_LEFT", "HAND_RIGHT", "FOOT_LEFT", "FOOT_RIGHT"
            };
            unsigned contact;
            for (contact = 0u; contact < MDKR_MODERN_CHARACTER_CONTACTS;
                 contact++) {
                for (axis = 0u; axis < 3u; axis++) {
                    (void)snprintf(
                        suffix, sizeof(suffix), "%s_%s_%s",
                        context_names[context], contact_names[contact],
                        axis_names[axis]);
                    valid &= parse_environment_float(
                        prefix, suffix, -1.0f, 1.0f,
                        &tuning->contact_offset[context][contact][axis]);
                }
            }
        }
    }
    return valid;
}

static void player_tuning_from_environment(
    int player, const char *package_id, MdkrModernCharacterTuning *tuning) {
    char package_prefix[128];
    char player_prefix[64];
    int package_valid;
    int player_valid;
    mdkr_modern_character_tuning_defaults(tuning);
    (void)snprintf(package_prefix, sizeof(package_prefix),
                   "MDKR_CUSTOM_CHARACTER_PROFILE_%s", package_id);
    (void)snprintf(player_prefix, sizeof(player_prefix),
                   "MDKR_CUSTOM_CHARACTER_P%d", player + 1);
    package_valid = apply_tuning_environment(package_prefix, tuning);
    /* Explicit per-player diagnostic values deliberately outrank the package
     * profile. This keeps CLI test/repair workflows authoritative. */
    player_valid = apply_tuning_environment(player_prefix, tuning);
    if (!package_valid || !player_valid) {
        fprintf(stderr,
                "[modern-character] P%d ignored one or more invalid tuning values\n",
                player + 1);
    }
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
            !mdkr_modern_identity_init(&pool->asset, &pool->identity,
                                       error, error_size) ||
            !mdkr_modern_render_asset_init(&pool->render, &pool->asset,
                                           error, error_size)) {
            mdkr_modern_render_asset_shutdown(&pool->render);
            mdkr_modern_character_asset_unload(&pool->asset);
            memset(pool, 0, sizeof(*pool));
            pool->registry_index = -1;
            return -1;
        }
        if (pool->render.gpu.primitive_count >
            MDKR_MODERN_CHARACTER_MAX_PRIMITIVES) {
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
    set_error(error, error_size,
              "character assignment staging capacity is exhausted");
    return -1;
}

static void pending_player_init(MdkrModernPendingPlayer *pending) {
    memset(pending, 0, sizeof(*pending));
    pending->pool = -1;
    mdkr_modern_character_tuning_defaults(&pending->tuning);
}

static void pending_player_release(MdkrModernPendingPlayer *pending) {
    int pool;
    if (pending == NULL) return;
    pool = pending->pool;
    mdkr_modern_pose_shutdown(&pending->pose);
    pending_player_init(pending);
    pool_release(pool);
}

static int pending_player_prepare(int player, int registry_index,
                                  MdkrModernPendingPlayer *pending,
                                  char *error, size_t error_size) {
    const MdkrModernCharacterEntry *entry;
    int pool;
    pending_player_init(pending);
    entry = mdkr_modern_character_registry_entry(&s_registry, registry_index);
    if (entry == NULL) {
        set_error(error, error_size,
                  "character catalog selection is unavailable");
        return 0;
    }
    if (!mdkr_modern_character_catalog_playable(registry_index)) {
        set_error(error, error_size,
                  "character has not passed the current Workshop playability gates");
        return 0;
    }
    pool = pool_acquire(registry_index, error, error_size);
    if (pool < 0) return 0;
    pending->pool = pool;
    player_tuning_from_environment(player, entry->id, &pending->tuning);
    pending->tuning.vehicle_mask &= s_pools[pool].definition.vehicle_mask;
    if (pending->tuning.vehicle_mask == 0u) {
        pending->tuning.vehicle_mask = s_pools[pool].definition.vehicle_mask;
    }
    if (!mdkr_modern_pose_init(&pending->pose, &s_pools[pool].asset,
                               error, error_size)) {
        pending_player_release(pending);
        return 0;
    }
    (void)snprintf(pending->semantic, sizeof(pending->semantic), "%s",
                   "fallback");
    return 1;
}

static void pending_player_publish(int player,
                                   MdkrModernPendingPlayer *pending) {
    MdkrModernRuntimePlayer *slot = &s_players[player];
    int old_pool = slot->pool;
    int new_pool = pending->pool;
    mdkr_modern_pose_shutdown(&slot->pose);
    memset(slot, 0, sizeof(*slot));
    slot->pool = new_pool;
    slot->pose = pending->pose;
    slot->tuning = pending->tuning;
    (void)snprintf(slot->semantic, sizeof(slot->semantic), "%s",
                   pending->semantic);
    memset(&pending->pose, 0, sizeof(pending->pose));
    pending->pool = -1;
    if (new_pool >= 0) {
        slot->identity_revision = ++s_identity_revision;
        if (slot->identity_revision == 0u) {
            slot->identity_revision = ++s_identity_revision;
        }
    } else {
        mdkr_modern_character_tuning_defaults(&slot->tuning);
    }
    pool_release(old_pool);
}

int mdkr_modern_characters_init(const char *directory) {
    int index;
    char error[256];
    const char *base;
    const char *playable;
    mdkr_modern_characters_shutdown();
    s_replacement_draws = 0u;
    s_replacement_primitives = 0u;
    s_hidden_donor_batches = 0u;
    s_contact_solves = 0u;
    s_contact_error_micrometres_sum = 0u;
    s_contact_error_micrometres_max = 0u;
    s_inspection_pose_ticks = 0u;
    s_inspection_pose_fallback_ticks = 0u;
    s_inspection_transition_switches = 0u;
    s_inspection_transition_blending_ticks = 0u;
    s_inspection_transition_completions = 0u;
    s_inspection_from_blend_milliseconds = 0u;
    s_inspection_to_blend_milliseconds = 0u;
    s_inspection_from_motion_source = MDKR_MODERN_CHARACTER_MOTION_NONE;
    s_inspection_to_motion_source = MDKR_MODERN_CHARACTER_MOTION_NONE;
    for (index = 0; index < MODERN_RUNTIME_POOLS; index++) {
        s_pools[index].registry_index = -1;
    }
    for (index = 0; index < MDKR_MODERN_CHARACTER_PLAYERS; index++) {
        s_players[index].pool = -1;
        mdkr_modern_character_tuning_defaults(&s_players[index].tuning);
    }
    {
        const char *preview = getenv("MDKR_CHARACTER_WORKSHOP_PREVIEW_PACKAGE");
        s_workshop_preview_package[0] = '\0';
        if (preview != NULL && preview[0] != '\0' &&
            strlen(preview) < sizeof(s_workshop_preview_package)) {
            memcpy(s_workshop_preview_package, preview, strlen(preview) + 1u);
        }
    }
    if ((s_workshop_preview_package[0] != '\0'
             ? mdkr_modern_character_registry_init_inventory(
                   &s_registry, directory)
             : mdkr_modern_character_registry_init(&s_registry, directory)) != 0) {
        s_workshop_preview_package[0] = '\0';
        return 0;
    }
    s_initialized = 1;
    playable = getenv("MDKR_CUSTOM_CHARACTER_PLAYABLE");
    s_playable_filter_active = playable != NULL;
    s_playable_list[0] = '\0';
    if (playable != NULL &&
        strlen(playable) < sizeof(s_playable_list)) {
        memcpy(s_playable_list, playable, strlen(playable) + 1u);
    }
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
    mdkr_modern_character_clear_inspection_pose();
    s_playable_filter_active = 0;
    s_playable_list[0] = '\0';
    s_workshop_preview_package[0] = '\0';
    s_initialized = 0;
}

void mdkr_modern_character_runtime_metrics(
    MdkrModernCharacterRuntimeMetrics *out) {
    if (out == NULL) return;
    out->replacement_draws = s_replacement_draws;
    out->replacement_primitives = s_replacement_primitives;
    out->hidden_donor_batches = s_hidden_donor_batches;
    out->contact_solves = s_contact_solves;
    out->contact_error_micrometres_sum =
        s_contact_error_micrometres_sum;
    out->contact_error_micrometres_max =
        s_contact_error_micrometres_max;
    out->inspection_pose_ticks = s_inspection_pose_ticks;
    out->inspection_pose_fallback_ticks =
        s_inspection_pose_fallback_ticks;
    out->inspection_transition_switches =
        s_inspection_transition_switches;
    out->inspection_transition_blending_ticks =
        s_inspection_transition_blending_ticks;
    out->inspection_transition_completions =
        s_inspection_transition_completions;
    out->inspection_from_blend_milliseconds =
        s_inspection_from_blend_milliseconds;
    out->inspection_to_blend_milliseconds =
        s_inspection_to_blend_milliseconds;
    out->inspection_from_motion_source =
        (uint32_t)s_inspection_from_motion_source;
    out->inspection_to_motion_source =
        (uint32_t)s_inspection_to_motion_source;
}

void mdkr_modern_character_contact_metrics_reset(void) {
    s_contact_solves = 0u;
    s_contact_error_micrometres_sum = 0u;
    s_contact_error_micrometres_max = 0u;
}

void mdkr_modern_character_note_hidden_donor_batch(void) {
    s_hidden_donor_batches++;
}

const MdkrModernCharacterRegistry *mdkr_modern_characters_registry(void) {
    return s_initialized ? &s_registry : NULL;
}

int mdkr_modern_character_catalog_count(void) {
    return s_initialized ? mdkr_modern_character_registry_count(&s_registry)
                         : 0;
}

int mdkr_modern_character_catalog_playable(int index) {
    const MdkrModernCharacterEntry *entry;
    if (!s_initialized) return 0;
    entry = mdkr_modern_character_registry_entry(&s_registry, index);
    if (entry == NULL) return 0;
    if (!entry->enabled &&
        (s_workshop_preview_package[0] == '\0' ||
         strcmp(entry->id, s_workshop_preview_package) != 0)) {
        return 0;
    }
    return !s_playable_filter_active ||
           playable_list_contains(s_playable_list, entry->id);
}

int mdkr_modern_character_catalog_entry(
    int index, MdkrModernCharacterCatalogView *out) {
    const MdkrModernCharacterEntry *entry;
    uint32_t packed;
    uint64_t revision = 0u;
    unsigned byte;
    if (!s_initialized || out == NULL) return 0;
    entry = mdkr_modern_character_registry_entry(&s_registry, index);
    if (entry == NULL) return 0;
    memset(out, 0, sizeof(*out));
    out->id = entry->id;
    out->display_name = entry->display_name;
    out->short_name = entry->short_name;
    out->narration_name = entry->narration_name;
    out->sort_label = entry->sort_label;
    out->donor = entry->donor;
    out->vehicle_mask = entry->vehicle_mask;
    out->has_identity = (entry->identity_flags & 1u) != 0u;
    if (out->has_identity) {
        out->portrait_rgba = entry->portrait_rgba;
        out->portrait_width = MDKR_MODERN_PORTRAIT_SIZE;
        out->portrait_height = MDKR_MODERN_PORTRAIT_SIZE;
        out->portrait_stride = MDKR_MODERN_PORTRAIT_SIZE * 4u;
        packed = entry->minimap_rgba;
        out->minimap_rgba[0] = (uint8_t)(packed & 0xFFu);
        out->minimap_rgba[1] = (uint8_t)((packed >> 8u) & 0xFFu);
        out->minimap_rgba[2] = (uint8_t)((packed >> 16u) & 0xFFu);
        out->minimap_rgba[3] = (uint8_t)((packed >> 24u) & 0xFFu);
    }
    for (byte = 0u; byte < 8u; byte++) {
        revision |= (uint64_t)entry->source_sha256[byte] << (byte * 8u);
    }
    out->revision = revision != 0u ? revision : 1u;
    return 1;
}

int mdkr_modern_character_assign_player_index(
    int player, int catalog_index, char *error, size_t error_size) {
    MdkrModernPendingPlayer pending;
    const MdkrModernCharacterEntry *entry = NULL;
    if (!s_initialized || player < 0 ||
        player >= MDKR_MODERN_CHARACTER_PLAYERS ||
        (entry = mdkr_modern_character_registry_entry(
             &s_registry, catalog_index)) == NULL) {
        set_error(error, error_size,
                  "character catalog selection is unavailable");
        return 0;
    }
    if (!gfx_modern_character_supported()) {
        set_error(error, error_size,
                  "active renderer does not support GPU-skinned characters");
        return 0;
    }
    if (!pending_player_prepare(player, catalog_index, &pending,
                                error, error_size)) return 0;
    pending_player_publish(player, &pending);
    fprintf(stderr, "[modern-character] P%d=%s donor=%u triangles=%u\n",
            player + 1, entry->id,
            s_pools[s_players[player].pool].definition.donor,
            s_pools[s_players[player].pool].render.gpu.index_count / 3u);
    set_error(error, error_size, "");
    return 1;
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
    mdkr_modern_character_tuning_defaults(&slot->tuning);
    pool_release(pool);
}

int mdkr_modern_character_assign_player(int player, const char *package_id,
                                        char *error, size_t error_size) {
    int registry_index;
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
    return mdkr_modern_character_assign_player_index(
        player, registry_index, error, error_size);
}

int mdkr_modern_character_apply_catalog_plan(
    const int catalog_indices[MDKR_MODERN_CHARACTER_PLAYERS],
    char *error, size_t error_size) {
    MdkrModernPendingPlayer pending[MDKR_MODERN_CHARACTER_PLAYERS];
    int player;
    if (!s_initialized || catalog_indices == NULL) {
        set_error(error, error_size,
                  "character assignment plan is unavailable");
        return 0;
    }
    for (player = 0; player < MDKR_MODERN_CHARACTER_PLAYERS; player++) {
        pending_player_init(&pending[player]);
        if (catalog_indices[player] < -1 ||
            (catalog_indices[player] >= 0 &&
             mdkr_modern_character_registry_entry(
                 &s_registry, catalog_indices[player]) == NULL)) {
            set_error(error, error_size,
                      "character assignment plan contains an unavailable selection");
            return 0;
        }
    }
    if (!gfx_modern_character_supported()) {
        for (player = 0; player < MDKR_MODERN_CHARACTER_PLAYERS; player++) {
            if (catalog_indices[player] >= 0) {
                set_error(error, error_size,
                          "active renderer does not support GPU-skinned characters");
                return 0;
            }
        }
    }
    for (player = 0; player < MDKR_MODERN_CHARACTER_PLAYERS; player++) {
        if (catalog_indices[player] >= 0 &&
            !pending_player_prepare(player, catalog_indices[player],
                                    &pending[player], error, error_size)) {
            int staged;
            for (staged = 0; staged < MDKR_MODERN_CHARACTER_PLAYERS;
                 staged++) {
                pending_player_release(&pending[staged]);
            }
            return 0;
        }
    }
    for (player = 0; player < MDKR_MODERN_CHARACTER_PLAYERS; player++) {
        pending_player_publish(player, &pending[player]);
        if (s_players[player].pool >= 0) {
            const MdkrModernRuntimePool *pool =
                &s_pools[s_players[player].pool];
            const char *id = mdkr_modern_character_asset_string(
                &pool->asset, pool->definition.id);
            fprintf(stderr,
                    "[modern-character] P%d=%s donor=%u triangles=%u\n",
                    player + 1, id != NULL ? id : "<invalid>",
                    pool->definition.donor,
                    pool->render.gpu.index_count / 3u);
        }
    }
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

int mdkr_modern_character_player_identity(
    int player, MdkrModernCharacterIdentityView *out) {
    MdkrModernRuntimePlayer *slot;
    MdkrModernRuntimePool *pool;
    const char *display_name;
    const char *candidate;
    MdkrModernIdentity identity;
    MdkrModernIdentityNames identity_names;
    if (out == NULL || player < 0 ||
        player >= MDKR_MODERN_CHARACTER_PLAYERS) return 0;
    memset(out, 0, sizeof(*out));
    slot = &s_players[player];
    if (slot->pool < 0) return 0;
    pool = &s_pools[slot->pool];
    if (!pool->identity.has_portrait) return 0;
    display_name = mdkr_modern_character_asset_string(
        &pool->asset, pool->definition.display_name);
    if (display_name == NULL || display_name[0] == '\0') return 0;
    out->display_name = display_name;
    out->short_name = display_name;
    out->narration_name = display_name;
    out->sort_label = display_name;
    if (mdkr_modern_character_asset_identity(
            &pool->asset, &identity, NULL) && identity.short_name != 0u &&
        (candidate = mdkr_modern_character_asset_string(
            &pool->asset, identity.short_name)) != NULL &&
        candidate[0] != '\0') {
        out->short_name = candidate;
    }
    if (mdkr_modern_character_asset_identity_names(
            &pool->asset, &identity_names)) {
        if (identity_names.narration_name != 0u &&
            (candidate = mdkr_modern_character_asset_string(
                &pool->asset, identity_names.narration_name)) != NULL &&
            candidate[0] != '\0') {
            out->narration_name = candidate;
        }
        if (identity_names.sort_label != 0u &&
            (candidate = mdkr_modern_character_asset_string(
                &pool->asset, identity_names.sort_label)) != NULL &&
            candidate[0] != '\0') {
            out->sort_label = candidate;
        }
    }
    out->portrait_rgba = pool->identity.portrait_rgba;
    out->portrait_width = MDKR_MODERN_PORTRAIT_SIZE;
    out->portrait_height = MDKR_MODERN_PORTRAIT_SIZE;
    out->portrait_stride = MDKR_MODERN_PORTRAIT_SIZE * 4u;
    memcpy(out->minimap_rgba, pool->identity.minimap_rgba,
           sizeof(out->minimap_rgba));
    out->revision = slot->identity_revision;
    return 1;
}

int mdkr_modern_character_set_tuning(int player,
                                     const MdkrModernCharacterTuning *tuning,
                                     char *error, size_t error_size) {
    MdkrModernCharacterTuning checked;
    if (player < 0 || player >= MDKR_MODERN_CHARACTER_PLAYERS ||
        tuning == NULL || s_players[player].pool < 0) {
        set_error(error, error_size, "character tuning target is unavailable");
        return 0;
    }
    checked = *tuning;
    if (!mdkr_modern_character_tuning_validate(&checked, error, error_size)) {
        return 0;
    }
    checked.vehicle_mask &= s_pools[s_players[player].pool].definition.vehicle_mask;
    if (checked.vehicle_mask == 0u) {
        set_error(error, error_size,
                  "character tuning selects no package-qualified vehicle");
        return 0;
    }
    s_players[player].tuning = checked;
    s_players[player].focus_valid_mask = 0u;
    s_players[player].fit_diagnostics_valid_mask = 0u;
    s_players[player].contact_diagnostics_valid_mask = 0u;
    s_players[player].selected_lod_valid_mask = 0u;
    return 1;
}

int mdkr_modern_character_get_tuning(int player,
                                     MdkrModernCharacterTuning *out) {
    if (player < 0 || player >= MDKR_MODERN_CHARACTER_PLAYERS || out == NULL ||
        s_players[player].pool < 0) return 0;
    *out = s_players[player].tuning;
    return 1;
}

int mdkr_modern_character_player_focus(
    int player, MdkrModernCharacterContext context,
    float center[3], float *radius) {
    const MdkrModernRuntimePlayer *slot;
    if (player < 0 || player >= MDKR_MODERN_CHARACTER_PLAYERS ||
        context < MDKR_CHARACTER_CONTEXT_SELECT ||
        context >= MDKR_CHARACTER_CONTEXT_COUNT || center == NULL ||
        radius == NULL) return 0;
    slot = &s_players[player];
    if (slot->pool < 0 ||
        (slot->focus_valid_mask & (1u << (unsigned)context)) == 0u) {
        return 0;
    }
    memcpy(center, slot->focus_center[context],
           sizeof(slot->focus_center[context]));
    *radius = slot->focus_radius[context];
    return 1;
}

int mdkr_modern_character_player_fit_diagnostics(
    int player, MdkrModernCharacterContext context,
    MdkrModernCharacterFitDiagnostics *out) {
    const MdkrModernRuntimePlayer *slot;
    if (player < 0 || player >= MDKR_MODERN_CHARACTER_PLAYERS ||
        context < MDKR_CHARACTER_CONTEXT_SELECT ||
        context >= MDKR_CHARACTER_CONTEXT_COUNT || out == NULL) return 0;
    slot = &s_players[player];
    if (slot->pool < 0 ||
        (slot->fit_diagnostics_valid_mask &
         (1u << (unsigned)context)) == 0u) return 0;
    *out = slot->fit_diagnostics[context];
    return 1;
}

int mdkr_modern_character_player_contact_diagnostics(
    int player, MdkrModernCharacterContext context,
    MdkrModernCharacterContactDiagnostics *out) {
    const MdkrModernRuntimePlayer *slot;
    if (player < 0 || player >= MDKR_MODERN_CHARACTER_PLAYERS ||
        context < MDKR_CHARACTER_CONTEXT_CAR ||
        context > MDKR_CHARACTER_CONTEXT_PLANE || out == NULL) return 0;
    slot = &s_players[player];
    if (slot->pool < 0 ||
        (slot->contact_diagnostics_valid_mask &
         (1u << (unsigned)context)) == 0u) return 0;
    *out = slot->contact_diagnostics[context];
    return 1;
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
           (pool->definition.vehicle_mask & slot->tuning.vehicle_mask &
            (1u << (unsigned)vehicle)) != 0u;
}

static MdkrModernCharacterMotionSource inspection_motion_source(
    const MdkrModernPose *pose, const char *semantic) {
    if (mdkr_modern_pose_has_semantic(pose, semantic)) {
        return MDKR_MODERN_CHARACTER_MOTION_AUTHORED;
    }
    if (mdkr_modern_pose_humanoid_retarget_ready(pose)) {
        return MDKR_MODERN_CHARACTER_MOTION_REVIEWED_REFERENCE;
    }
    return MDKR_MODERN_CHARACTER_MOTION_PACKAGE_FALLBACK;
}

static uint32_t inspection_blend_milliseconds(const MdkrModernPose *pose) {
    double milliseconds;
    if (pose == NULL || !isfinite(pose->blend_duration) ||
        pose->blend_duration <= 0.0f) return 0u;
    milliseconds = (double)pose->blend_duration * 1000.0;
    if (milliseconds >= 4294967295.0) return UINT32_MAX;
    return (uint32_t)(milliseconds + 0.5);
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
    if (s_inspection_semantic[0] != '\0') {
        return mdkr_modern_character_tick_phase(
            player, s_inspection_semantic, seconds, s_inspection_phase,
            error, error_size);
    }
    if (strcmp(slot->semantic, semantic) != 0) {
        if (!mdkr_modern_pose_set_semantic(&slot->pose, semantic,
                                           error, error_size)) return 0;
        (void)snprintf(slot->semantic, sizeof(slot->semantic), "%s", semantic);
    }
    return mdkr_modern_pose_advance(&slot->pose,
                                    seconds * slot->tuning.animation_speed,
                                    error, error_size);
}

int mdkr_modern_character_tick_phase(int player, const char *semantic,
                                     float seconds, float normalized_phase,
                                     char *error, size_t error_size) {
    MdkrModernRuntimePlayer *slot;
    int inspection;
    int transition = 0;
    int transition_switch = 0;
    int inspection_reset = 0;
    float unscaled_seconds = seconds;
    if (player < 0 || player >= MDKR_MODERN_CHARACTER_PLAYERS ||
        (slot = &s_players[player])->pool < 0 || semantic == NULL ||
        !isfinite(seconds) || seconds < 0.0f ||
        !isfinite(normalized_phase)) {
        set_error(error, error_size,
                  "modern character phase tick arguments are invalid");
        return 0;
    }
    inspection = s_inspection_semantic[0] != '\0';
    if (inspection) {
        transition = s_inspection_to_semantic[0] != '\0';
        if (slot->inspection_generation != s_inspection_generation) {
            slot->inspection_generation = s_inspection_generation;
            slot->inspection_dwell_seconds = 0.0f;
            slot->inspection_to_pose = 0;
            slot->inspection_counted_blend = 0;
            inspection_reset = 1;
        } else if (transition) {
            slot->inspection_dwell_seconds += unscaled_seconds;
            const float dwellSeconds =
                (float)MDKR_MODERN_CHARACTER_INSPECTION_TRANSITION_DWELL_MILLI /
                1000.0f;
            if (slot->inspection_dwell_seconds >= dwellSeconds) {
                slot->inspection_dwell_seconds = fmodf(
                    slot->inspection_dwell_seconds, dwellSeconds);
                slot->inspection_to_pose = !slot->inspection_to_pose;
                transition_switch = 1;
                s_inspection_transition_switches++;
            }
        }
        semantic = transition && slot->inspection_to_pose
            ? s_inspection_to_semantic : s_inspection_semantic;
        normalized_phase = transition && slot->inspection_to_pose
            ? s_inspection_to_phase : s_inspection_phase;
    }
    if (strcmp(slot->semantic, semantic) != 0) {
        if (!mdkr_modern_pose_set_semantic(&slot->pose, semantic,
                                           error, error_size)) return 0;
        (void)snprintf(slot->semantic, sizeof(slot->semantic), "%s", semantic);
        if (transition) {
            const uint32_t blend = inspection_blend_milliseconds(&slot->pose);
            if (slot->inspection_to_pose) {
                s_inspection_to_blend_milliseconds = blend;
            } else {
                s_inspection_from_blend_milliseconds = blend;
            }
            slot->inspection_counted_blend = transition_switch;
        }
    }
    if (transition && inspection_reset) {
        s_inspection_from_blend_milliseconds =
            inspection_blend_milliseconds(&slot->pose);
    }
    if (inspection) s_inspection_pose_ticks++;
    seconds *= slot->tuning.animation_speed;
    if (inspection) {
        const MdkrModernCharacterMotionSource source =
            inspection_motion_source(&slot->pose, semantic);
        if (transition && slot->inspection_to_pose) {
            s_inspection_to_motion_source = source;
        } else {
            s_inspection_from_motion_source = source;
        }
    }
    {
        const int package_fallback =
            !mdkr_modern_pose_has_semantic(&slot->pose, semantic) &&
            !mdkr_modern_pose_humanoid_retarget_ready(&slot->pose);
        int advanced;
        const int blending = transition &&
            slot->pose.blend_duration > 0.0f &&
            slot->pose.blend_elapsed < slot->pose.blend_duration;
        if (package_fallback && inspection) {
            s_inspection_pose_fallback_ticks++;
        }
        advanced = package_fallback
            ? mdkr_modern_pose_advance(
                  &slot->pose, seconds, error, error_size)
            : mdkr_modern_pose_advance_phase(
                  &slot->pose, seconds, normalized_phase, error, error_size);
        if (!advanced) return 0;
        if (blending && slot->inspection_counted_blend) {
            s_inspection_transition_blending_ticks++;
            if (slot->pose.blend_elapsed >= slot->pose.blend_duration) {
                s_inspection_transition_completions++;
                slot->inspection_counted_blend = 0;
            }
        }
        return 1;
    }
}

int mdkr_modern_character_emit(int player, int view,
                               MdkrModernCharacterContext context,
                               const float target_frame[16],
                               float view_distance, Gfx **display_list,
                               char *error, size_t error_size) {
    MdkrModernRuntimePlayer *slot;
    MdkrModernRuntimePool *pool;
    float package_transform[16];
    float seat_transform[16];
    float previous_seat_transform[16];
    float inverse_seat[16];
    float previous_inverse_seat[16];
    float authored_context[16];
    float user_context[16];
    float target_context[16];
    float donor_context[16];
    float source_normalized[16];
    float previous_source_normalized[16];
    float authored_normalized[16];
    float previous_authored_normalized[16];
    float adjusted_transform[16];
    float previous_adjusted_transform[16];
    float anchored_transform[16];
    float previous_anchored_transform[16];
    float source_anchor[3] = {0.0f, 0.0f, 0.0f};
    MdkrModernAttachment attachment;
    MdkrModernCalibration calibration;
    MdkrModernCharacterFitDiagnostics fit_diagnostics;
    MdkrModernCharacterContactDiagnostics contact_diagnostics;
    int has_calibration;
    int fit_diagnostics_ready = 0;
    int contact_diagnostics_ready = 0;
    int contact_solved = 0;
    uint64_t contact_error_micrometres = 0u;
    uint32_t primitive_index;
    uint32_t selected_lod;
    uint32_t authored_lod_mask = 0u;
    uint32_t emitted = 0u;
    const MdkrWorkshopPreviewLighting inspection_lighting =
        mdkr_workshop_preview_lighting();
    if (player < 0 || player >= MDKR_MODERN_CHARACTER_PLAYERS ||
        view < 0 || view >= MDKR_MODERN_CHARACTER_VIEWS ||
        display_list == NULL || *display_list == NULL ||
        (slot = &s_players[player])->pool < 0 ||
        context < MDKR_CHARACTER_CONTEXT_SELECT ||
        context >= MDKR_CHARACTER_CONTEXT_COUNT ||
        !gfx_modern_character_supported() || !isfinite(view_distance)) {
        set_error(error, error_size, "modern character draw is unavailable");
        return 0;
    }
    pool = &s_pools[slot->pool];
    has_calibration = mdkr_modern_character_asset_calibration(
        &pool->asset, &calibration);
    if (context != MDKR_CHARACTER_CONTEXT_SELECT && has_calibration) {
        const uint64_t previous_contact_generation =
            slot->pose.contact_generation;
        const uint32_t previous_contact_context = slot->pose.contact_context;
        if (!mdkr_modern_pose_apply_vehicle_contacts(
                &slot->pose, context, &calibration,
                slot->tuning.contact_offset[context], error, error_size)) {
            return 0;
        }
        contact_solved =
            slot->pose.contact_generation == slot->pose.generation &&
            (previous_contact_generation != slot->pose.contact_generation ||
             previous_contact_context != slot->pose.contact_context);
    }
    for (primitive_index = 0u;
         primitive_index < pool->render.gpu.primitive_count;
         primitive_index++) {
        const uint32_t lod = pool->render.gpu.primitives[primitive_index].lod;
        if (lod < MDKR_MODERN_CHARACTER_LOD_LEVELS) {
            authored_lod_mask |= 1u << lod;
        }
    }
    {
        const uint32_t lod_state =
            (uint32_t)context * MDKR_MODERN_CHARACTER_VIEWS +
            (uint32_t)view;
        const uint32_t lod_state_bit = 1u << lod_state;
        selected_lod = mdkr_modern_character_select_lod_hysteretic(
            view_distance, pool->definition.lod_bias, slot->tuning.lod_bias,
            authored_lod_mask, slot->selected_lod[context][view],
            (slot->selected_lod_valid_mask & lod_state_bit) != 0u);
        if (selected_lod != UINT32_MAX) {
            slot->selected_lod[context][view] = selected_lod;
            slot->selected_lod_valid_mask |= lod_state_bit;
        }
    }
    if (selected_lod == UINT32_MAX) {
        set_error(error, error_size,
                  "character LOD policy or authored levels are invalid");
        return 0;
    }
    if (!attachment_for_context(&pool->asset, context, &attachment)) {
        set_error(error, error_size,
                  "character package has no attachment for this context");
        return 0;
    }
    if ((attachment.flags & 1u) != 0u) {
        matrix_identity(seat_transform);
        if (has_calibration) {
            seat_transform[12] = calibration.ground[0];
            seat_transform[13] = calibration.ground[1];
            seat_transform[14] = calibration.ground[2];
            memcpy(source_anchor, calibration.ground,
                   sizeof(source_anchor));
        }
        memcpy(previous_seat_transform, seat_transform,
               sizeof(previous_seat_transform));
    } else {
        const char *anchor = attachment.anchor != 0u
            ? mdkr_modern_character_asset_string(&pool->asset,
                                                  attachment.anchor)
            : "seat";
        if (anchor == NULL ||
            !mdkr_modern_pose_socket_matrix(&slot->pose, anchor, 0,
                                            seat_transform) ||
            !mdkr_modern_pose_socket_matrix(&slot->pose, anchor, 1,
                                            previous_seat_transform)) {
            set_error(error, error_size,
                      "character attachment socket is unavailable");
            return 0;
        }
        /* Attachment sockets contribute their animated position only. Source
         * unit/up-axis scale and facing are normalized exactly once by the
         * package definition; inheriting a pelvis node's basis here would
         * silently apply exporter conversion a second time. */
        {
            const float current_translation[3] = {
                seat_transform[12], seat_transform[13], seat_transform[14]
            };
            const float previous_translation[3] = {
                previous_seat_transform[12], previous_seat_transform[13],
                previous_seat_transform[14]
            };
            memcpy(source_anchor, current_translation,
                   sizeof(source_anchor));
            matrix_identity(seat_transform);
            matrix_identity(previous_seat_transform);
            memcpy(seat_transform + 12, current_translation,
                   sizeof(current_translation));
            memcpy(previous_seat_transform + 12, previous_translation,
                   sizeof(previous_translation));
        }
    }
    if (!matrix_affine_inverse(seat_transform, inverse_seat) ||
        !matrix_affine_inverse(previous_seat_transform,
                               previous_inverse_seat)) {
        set_error(error, error_size,
                  "character attachment anchor has a singular transform");
        return 0;
    }
    definition_matrix(&pool->definition, &slot->tuning, package_transform);
    quaternion_transform_matrix(attachment.rotation, attachment.scale,
                                attachment.translation, authored_context);
    adjustment_matrix(&slot->tuning.context[context], user_context);
    if (target_frame != NULL) {
        memcpy(target_context, target_frame, sizeof(target_context));
    } else if (mdkr_modern_donor_attachment_frame(
                   (int)pool->definition.donor, context, donor_context)) {
        memcpy(target_context, donor_context, sizeof(target_context));
    } else {
        set_error(error, error_size,
                  "character donor has no qualified attachment frame");
        return 0;
    }
    /* Source and target frames remain independent. Ground anchoring is fixed
     * in bind space, while a vehicle seat may move with an authored clip.
     * Context and user corrections operate in DKR target space, so fixing the
     * select floor can never perturb the car/hover/plane fit. */
    matrix_multiply(package_transform, inverse_seat, source_normalized);
    matrix_multiply(package_transform, previous_inverse_seat,
                    previous_source_normalized);
    matrix_multiply(authored_context, source_normalized,
                    authored_normalized);
    matrix_multiply(authored_context, previous_source_normalized,
                    previous_authored_normalized);
    matrix_multiply(user_context, authored_normalized, adjusted_transform);
    matrix_multiply(user_context, previous_authored_normalized,
                    previous_adjusted_transform);
    matrix_multiply(target_context, adjusted_transform, anchored_transform);
    matrix_multiply(target_context, previous_adjusted_transform,
                    previous_anchored_transform);
    if (has_calibration) {
        fit_diagnostics_ready = calibration_fit_diagnostics(
            &calibration, adjusted_transform, source_anchor,
            &fit_diagnostics);
        if (context != MDKR_CHARACTER_CONTEXT_SELECT &&
            slot->pose.contact_generation == slot->pose.generation &&
            slot->pose.contact_context == (uint32_t)context &&
            slot->pose.contact_valid_mask ==
                ((1u << MDKR_MODERN_CHARACTER_CONTACTS) - 1u)) {
            unsigned contact;
            memset(&contact_diagnostics, 0, sizeof(contact_diagnostics));
            for (contact = 0u; contact < MDKR_MODERN_CHARACTER_CONTACTS;
                 contact++) {
                float difference[3];
                unsigned axis;
                matrix_transform_point(
                    adjusted_transform,
                    slot->pose.contact_chain_root[contact],
                    contact_diagnostics.chain_root[contact]);
                matrix_transform_point(
                    adjusted_transform, slot->pose.contact_bend[contact],
                    contact_diagnostics.bend[contact]);
                matrix_transform_point(
                    adjusted_transform, slot->pose.contact_target[contact],
                    contact_diagnostics.target[contact]);
                matrix_transform_point(
                    adjusted_transform, slot->pose.contact_end[contact],
                    contact_diagnostics.end[contact]);
                for (axis = 0u; axis < 3u; axis++) {
                    difference[axis] =
                        contact_diagnostics.end[contact][axis] -
                        contact_diagnostics.target[contact][axis];
                    if (!isfinite(contact_diagnostics.chain_root[contact][axis]) ||
                        !isfinite(contact_diagnostics.bend[contact][axis]) ||
                        !isfinite(contact_diagnostics.target[contact][axis]) ||
                        !isfinite(contact_diagnostics.end[contact][axis])) {
                        set_error(error, error_size,
                                  "vehicle contact witness is outside its safe range");
                        return 0;
                    }
                }
                contact_diagnostics.error[contact] = sqrtf(
                    difference[0] * difference[0] +
                    difference[1] * difference[1] +
                    difference[2] * difference[2]);
                if (!isfinite(contact_diagnostics.error[contact])) {
                    set_error(error, error_size,
                              "vehicle contact error is outside its safe range");
                    return 0;
                }
                contact_diagnostics.valid_mask |= 1u << contact;
            }
            contact_diagnostics_ready = 1;
            if (contact_solved) {
                double micrometres = 0.0;
                for (contact = 0u; contact < MDKR_MODERN_CHARACTER_CONTACTS;
                     contact++) {
                    const double candidate =
                        (double)contact_diagnostics.error[contact] * 1000000.0;
                    if (candidate > micrometres) micrometres = candidate;
                }
                if (!isfinite(micrometres) || micrometres < 0.0 ||
                    micrometres > (double)UINT64_MAX) {
                    set_error(error, error_size,
                              "vehicle contact metric is outside its safe range");
                    return 0;
                }
                contact_error_micrometres =
                    (uint64_t)(micrometres + 0.5);
            }
        }
    }
    for (primitive_index = 0u;
         primitive_index < pool->render.gpu.primitive_count;
         primitive_index++) {
        MdkrModernPrimitive primitive;
        struct GfxModernSkinnedDraw draw;
        const float *node_world;
        const float *previous_node_world;
        memset(&draw, 0, sizeof(draw));
        if (pool->render.gpu.primitives[primitive_index].lod != selected_lod) continue;
        (void)mdkr_modern_character_asset_primitive(
            &pool->asset, primitive_index, &primitive);
        node_world = mdkr_modern_pose_node_matrix(&slot->pose,
                                                  primitive.node, 0);
        previous_node_world = mdkr_modern_pose_node_matrix(
            &slot->pose, primitive.node, 1);
        if (node_world == NULL || previous_node_world == NULL) {
            set_error(error, error_size, "character primitive node pose is unavailable");
            return 0;
        }
        draw.asset = &pool->render.gpu;
        draw.primitive = primitive_index;
        draw.player = (uint32_t)player;
        draw.view = (uint32_t)view;
        memcpy(draw.target_frame_matrix, target_context,
               sizeof(draw.target_frame_matrix));
        if (fit_diagnostics_ready) {
            draw.capture_bounds_valid = 1u;
            memcpy(draw.capture_bounds_min, fit_diagnostics.bounds_min,
                   sizeof(draw.capture_bounds_min));
            memcpy(draw.capture_bounds_max, fit_diagnostics.bounds_max,
                   sizeof(draw.capture_bounds_max));
        }
        matrix_multiply(anchored_transform, node_world, draw.model_matrix);
        matrix_multiply(previous_anchored_transform, previous_node_world,
                        draw.previous_model_matrix);
        if (!matrix_normal_transform(draw.model_matrix, draw.normal_matrix)) {
            set_error(error, error_size,
                      "character primitive has a singular normal transform");
            return 0;
        }
        switch (inspection_lighting) {
            case MDKR_WORKSHOP_PREVIEW_LIGHTING_BRIGHT:
                draw.light_direction[0] = 0.20f;
                draw.light_direction[1] = -0.95f;
                draw.light_direction[2] = 0.23f;
                draw.ambient = 0.55f;
                break;
            case MDKR_WORKSHOP_PREVIEW_LIGHTING_LOW_KEY:
                draw.light_direction[0] = 0.65f;
                draw.light_direction[1] = -0.40f;
                draw.light_direction[2] = 0.65f;
                draw.ambient = 0.12f;
                break;
            case MDKR_WORKSHOP_PREVIEW_LIGHTING_BACKLIT:
                draw.light_direction[0] = 0.00f;
                draw.light_direction[1] = -0.25f;
                draw.light_direction[2] = -0.97f;
                draw.ambient = 0.18f;
                break;
            case MDKR_WORKSHOP_PREVIEW_LIGHTING_NEUTRAL:
            default:
                draw.light_direction[0] = 0.35f;
                draw.light_direction[1] = -0.85f;
                draw.light_direction[2] = 0.38f;
                draw.ambient = 0.32f;
                break;
        }
        if (primitive.skin >= 0) {
            MdkrModernSkin skin;
            (void)mdkr_modern_character_asset_skin(
                &pool->asset, (uint32_t)primitive.skin, &skin);
            if (skin.joint_count > MODERN_RUNTIME_MAX_BONES ||
                !mdkr_modern_pose_skin_palette(
                    &slot->pose, (uint32_t)primitive.skin, primitive.node, 0,
                    slot->palette, MODERN_RUNTIME_MAX_BONES,
                    error, error_size) ||
                !mdkr_modern_pose_skin_palette(
                    &slot->pose, (uint32_t)primitive.skin, primitive.node, 1,
                    slot->previous_palette, MODERN_RUNTIME_MAX_BONES,
                    error, error_size)) return 0;
            draw.bone_matrices = slot->palette;
            draw.previous_bone_matrices = slot->previous_palette;
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
    if (has_calibration && calibration_focus(
            &calibration, anchored_transform,
            slot->focus_center[context], &slot->focus_radius[context])) {
        slot->focus_valid_mask |= 1u << (unsigned)context;
    } else {
        slot->focus_valid_mask &= ~(1u << (unsigned)context);
    }
    if (fit_diagnostics_ready) {
        slot->fit_diagnostics[context] = fit_diagnostics;
        slot->fit_diagnostics_valid_mask |= 1u << (unsigned)context;
    } else {
        slot->fit_diagnostics_valid_mask &= ~(1u << (unsigned)context);
    }
    if (contact_diagnostics_ready) {
        slot->contact_diagnostics[context] = contact_diagnostics;
        slot->contact_diagnostics_valid_mask |= 1u << (unsigned)context;
    } else {
        slot->contact_diagnostics_valid_mask &= ~(1u << (unsigned)context);
    }
    if (inspection_lighting != MDKR_WORKSHOP_PREVIEW_LIGHTING_NEUTRAL) {
        mdkr_workshop_preview_note_lighting_override();
    }
    s_replacement_draws++;
    s_replacement_primitives += emitted;
    if (contact_solved) {
        s_contact_solves++;
        if (UINT64_MAX - s_contact_error_micrometres_sum <
            contact_error_micrometres) {
            s_contact_error_micrometres_sum = UINT64_MAX;
        } else {
            s_contact_error_micrometres_sum += contact_error_micrometres;
        }
        if (contact_error_micrometres >
            s_contact_error_micrometres_max) {
            s_contact_error_micrometres_max = contact_error_micrometres;
        }
    }
    set_error(error, error_size, "");
    return 1;
}

int mdkr_modern_character_player_calibration(
    int player, MdkrModernCalibration *out,
    uint32_t *attachment_context_mask) {
    MdkrModernRuntimePlayer *slot;
    MdkrModernRuntimePool *pool;
    const MdkrModernSectionView *attachments;
    uint32_t mask = 0u;
    uint32_t index;
    if (player < 0 || player >= MDKR_MODERN_CHARACTER_PLAYERS || out == NULL ||
        (slot = &s_players[player])->pool < 0) return 0;
    pool = &s_pools[slot->pool];
    if (!mdkr_modern_character_asset_calibration(&pool->asset, out)) return 0;
    attachments = mdkr_modern_character_asset_section(
        &pool->asset, MDKR_MDKC_ATTACHMENTS);
    if (attachments != NULL) {
        for (index = 0u; index < attachments->count; index++) {
            MdkrModernAttachment attachment;
            (void)mdkr_modern_character_asset_attachment(
                &pool->asset, index, &attachment);
            mask |= 1u << attachment.context;
        }
    }
    if (attachment_context_mask != NULL) *attachment_context_mask = mask;
    return 1;
}
