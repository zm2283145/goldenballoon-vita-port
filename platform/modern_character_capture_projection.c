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

int mdkr_modern_character_capture_framing_solve_ndc(
    const float source_ndc_bounds[4],
    MdkrModernCharacterCaptureFraming *output) {
    MdkrModernCharacterCaptureFraming solved;
    double span_x;
    double span_y;
    double scale;
    unsigned component;
    if (source_ndc_bounds == NULL || output == NULL) return 0;
    for (component = 0u; component < 4u; ++component) {
        if (!isfinite(source_ndc_bounds[component])) return 0;
    }
    if (source_ndc_bounds[0] > source_ndc_bounds[2] ||
        source_ndc_bounds[1] > source_ndc_bounds[3]) return 0;
    span_x = (double)source_ndc_bounds[2] - source_ndc_bounds[0];
    span_y = (double)source_ndc_bounds[3] - source_ndc_bounds[1];
    if (!isfinite(span_x) || !isfinite(span_y) ||
        span_x <= 1.0e-9 || span_y <= 1.0e-9) return 0;
    /* 82% horizontal and 72% vertical occupancy leave space for hair,
     * animation overshoot, outline generation, and a portrait crop. */
    scale = fmin(1.64 / span_x, 1.44 / span_y);
    if (scale < 0.25) scale = 0.25;
    if (scale > 8.0) scale = 8.0;
    memset(&solved, 0, sizeof(solved));
    solved.scale = (float)scale;
    solved.center_ndc[0] =
        (float)(((double)source_ndc_bounds[0] + source_ndc_bounds[2]) * 0.5);
    solved.center_ndc[1] =
        (float)(((double)source_ndc_bounds[1] + source_ndc_bounds[3]) * 0.5);
    memcpy(solved.source_ndc_bounds, source_ndc_bounds,
           sizeof(solved.source_ndc_bounds));
    solved.valid = 1u;
    *output = solved;
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

int mdkr_modern_character_capture_framing_solve(
    const float target_to_clip[16], const float bounds_min[3],
    const float bounds_max[3], MdkrModernCharacterCaptureFraming *output) {
    double minimum[2] = {DBL_MAX, DBL_MAX};
    double maximum[2] = {-DBL_MAX, -DBL_MAX};
    unsigned corner;
    unsigned axis;
    if (!finite_matrix(target_to_clip) || bounds_min == NULL ||
        bounds_max == NULL || output == NULL) return 0;
    for (axis = 0u; axis < 3u; ++axis) {
        if (!isfinite(bounds_min[axis]) || !isfinite(bounds_max[axis]) ||
            bounds_min[axis] > bounds_max[axis]) return 0;
    }
    for (corner = 0u; corner < 8u; ++corner) {
        double clip[4];
        float point[3];
        unsigned row;
        for (axis = 0u; axis < 3u; ++axis) {
            point[axis] = (corner & (1u << axis)) != 0u
                ? bounds_max[axis] : bounds_min[axis];
        }
        for (row = 0u; row < 4u; ++row) {
            clip[row] = (double)target_to_clip[row] * point[0] +
                (double)target_to_clip[4u + row] * point[1] +
                (double)target_to_clip[8u + row] * point[2] +
                target_to_clip[12u + row];
            if (!isfinite(clip[row])) return 0;
        }
        if (clip[3] <= 1.0e-9) return 0;
        for (axis = 0u; axis < 2u; ++axis) {
            const double ndc = clip[axis] / clip[3];
            if (!isfinite(ndc)) return 0;
            if (ndc < minimum[axis]) minimum[axis] = ndc;
            if (ndc > maximum[axis]) maximum[axis] = ndc;
        }
    }
    {
        const float ndc_bounds[4] = {
            (float)minimum[0], (float)minimum[1],
            (float)maximum[0], (float)maximum[1],
        };
        return mdkr_modern_character_capture_framing_solve_ndc(
            ndc_bounds, output);
    }
}

int mdkr_modern_character_capture_framing_apply(
    const float matrix[16],
    const MdkrModernCharacterCaptureFraming *framing, float output[16]) {
    float framed[16];
    unsigned column;
    if (!finite_matrix(matrix) || framing == NULL || output == NULL ||
        framing->valid != 1u || !isfinite(framing->scale) ||
        framing->scale < 0.25f || framing->scale > 8.0f ||
        !isfinite(framing->center_ndc[0]) ||
        !isfinite(framing->center_ndc[1])) return 0;
    memcpy(framed, matrix, sizeof(framed));
    for (column = 0u; column < 4u; ++column) {
        const unsigned base = column * 4u;
        const double x = (double)framing->scale *
            ((double)matrix[base] -
             framing->center_ndc[0] * matrix[base + 3u]);
        const double y = (double)framing->scale *
            ((double)matrix[base + 1u] -
             framing->center_ndc[1] * matrix[base + 3u]);
        if (!isfinite(x) || !isfinite(y) || x < -FLT_MAX || x > FLT_MAX ||
            y < -FLT_MAX || y > FLT_MAX) return 0;
        framed[base] = (float)x;
        framed[base + 1u] = (float)y;
    }
    memcpy(output, framed, sizeof(framed));
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
