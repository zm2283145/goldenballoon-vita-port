#include "modern_character_capture_projection.h"

#include "modern_character_limits.h"

#include <float.h>
#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <string.h>

static int finite_matrix(const float matrix[16]) {
    unsigned component;
    if (matrix == NULL) return 0;
    for (component = 0u; component < 16u; ++component) {
        if (!isfinite(matrix[component])) return 0;
    }
    return 1;
}

int mdkr_modern_character_capture_projection_compose(
    const float mvp[16], const float target_frame[16], float output[16]) {
    float composed[16];
    unsigned column;
    unsigned row;
    unsigned inner;
    if (!finite_matrix(mvp) || !finite_matrix(target_frame) || output == NULL) {
        return 0;
    }
    for (column = 0u; column < 4u; ++column) {
        for (row = 0u; row < 4u; ++row) {
            double value = 0.0;
            for (inner = 0u; inner < 4u; ++inner) {
                value += (double)mvp[inner * 4u + row] *
                    target_frame[column * 4u + inner];
            }
            if (!isfinite(value) || value < -FLT_MAX || value > FLT_MAX) {
                return 0;
            }
            composed[column * 4u + row] = (float)value;
        }
    }
    memcpy(output, composed, sizeof(composed));
    return 1;
}

static int rect_valid(const int32_t rect[4], uint32_t width,
                      uint32_t height) {
    int64_t right;
    int64_t bottom;
    if (rect == NULL || rect[0] < 0 || rect[1] < 0 ||
        rect[2] <= 0 || rect[3] <= 0) return 0;
    right = (int64_t)rect[0] + rect[2];
    bottom = (int64_t)rect[1] + rect[3];
    return right <= (int64_t)width && bottom <= (int64_t)height;
}

int mdkr_modern_character_capture_projection_valid(
    const MdkrModernCharacterCaptureProjection *projection) {
    if (projection == NULL ||
        projection->version !=
            MDKR_MODERN_CHARACTER_CAPTURE_PROJECTION_VERSION ||
        projection->valid != 1u ||
        projection->subject_player >= MDKR_MODERN_CHARACTER_PLAYERS ||
        projection->primitive_draws == 0u ||
        projection->primitive_draws > MDKR_MODERN_CHARACTER_MAX_PRIMITIVES ||
        projection->output_width == 0u ||
        projection->output_height == 0u ||
        projection->output_width >
            MDKR_MODERN_CHARACTER_CAPTURE_MAX_DIMENSION ||
        projection->output_height >
            MDKR_MODERN_CHARACTER_CAPTURE_MAX_DIMENSION ||
        !rect_valid(projection->viewport, projection->output_width,
                    projection->output_height) ||
        !rect_valid(projection->scissor, projection->output_width,
                    projection->output_height) ||
        !finite_matrix(projection->target_to_clip)) {
        return 0;
    }
    return 1;
}

static int quantize(double value, double scale, int32_t *output) {
    const double scaled = value * scale;
    if (output == NULL || !isfinite(scaled) || scaled < INT32_MIN ||
        scaled > INT32_MAX) return 0;
    *output = (int32_t)(scaled + (scaled < 0.0 ? -0.5 : 0.5));
    return 1;
}

int mdkr_modern_character_capture_project_point(
    const MdkrModernCharacterCaptureProjection *projection,
    const float point[3], int32_t output_millipixels[2],
    int32_t *depth_millionths, uint32_t *clip_flags) {
    double clip[4];
    double ndc[3];
    double pixel_x;
    double pixel_y;
    uint32_t flags = 0u;
    unsigned row;
    if (!mdkr_modern_character_capture_projection_valid(projection) ||
        point == NULL || output_millipixels == NULL ||
        depth_millionths == NULL || clip_flags == NULL ||
        !isfinite(point[0]) || !isfinite(point[1]) || !isfinite(point[2])) {
        return 0;
    }
    for (row = 0u; row < 4u; ++row) {
        clip[row] = (double)projection->target_to_clip[row] * point[0] +
            (double)projection->target_to_clip[4u + row] * point[1] +
            (double)projection->target_to_clip[8u + row] * point[2] +
            projection->target_to_clip[12u + row];
        if (!isfinite(clip[row])) return 0;
    }
    if (clip[3] <= 1.0e-9) return 0;
    for (row = 0u; row < 3u; ++row) ndc[row] = clip[row] / clip[3];
    if (!isfinite(ndc[0]) || !isfinite(ndc[1]) || !isfinite(ndc[2])) {
        return 0;
    }
    pixel_x = projection->viewport[0] +
        (ndc[0] + 1.0) * 0.5 * projection->viewport[2];
    pixel_y = projection->viewport[1] +
        (1.0 - ndc[1]) * 0.5 * projection->viewport[3];
    if (!quantize(pixel_x, 1000.0, &output_millipixels[0]) ||
        !quantize(pixel_y, 1000.0, &output_millipixels[1]) ||
        !quantize(ndc[2], 1000000.0, depth_millionths)) return 0;
    if (ndc[0] < -1.0) flags |= MDKR_MODERN_CHARACTER_PROJECTION_CLIP_LEFT;
    if (ndc[0] > 1.0) flags |= MDKR_MODERN_CHARACTER_PROJECTION_CLIP_RIGHT;
    if (ndc[1] > 1.0) flags |= MDKR_MODERN_CHARACTER_PROJECTION_CLIP_TOP;
    if (ndc[1] < -1.0) flags |= MDKR_MODERN_CHARACTER_PROJECTION_CLIP_BOTTOM;
    if (ndc[2] < 0.0) flags |= MDKR_MODERN_CHARACTER_PROJECTION_CLIP_NEAR;
    if (ndc[2] > 1.0) flags |= MDKR_MODERN_CHARACTER_PROJECTION_CLIP_FAR;
    if (pixel_x < projection->scissor[0] ||
        pixel_y < projection->scissor[1] ||
        pixel_x > (double)projection->scissor[0] + projection->scissor[2] ||
        pixel_y > (double)projection->scissor[1] + projection->scissor[3]) {
        flags |= MDKR_MODERN_CHARACTER_PROJECTION_CLIP_SCISSOR;
    }
    *clip_flags = flags;
    return 1;
}
