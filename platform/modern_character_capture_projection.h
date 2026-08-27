#ifndef MDKR_MODERN_CHARACTER_CAPTURE_PROJECTION_H
#define MDKR_MODERN_CHARACTER_CAPTURE_PROJECTION_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MDKR_MODERN_CHARACTER_CAPTURE_PROJECTION_VERSION 1u
#define MDKR_MODERN_CHARACTER_CAPTURE_MAX_DIMENSION 16384u

enum MdkrModernCharacterProjectionClipFlag {
    MDKR_MODERN_CHARACTER_PROJECTION_CLIP_LEFT = 1u << 0,
    MDKR_MODERN_CHARACTER_PROJECTION_CLIP_RIGHT = 1u << 1,
    MDKR_MODERN_CHARACTER_PROJECTION_CLIP_TOP = 1u << 2,
    MDKR_MODERN_CHARACTER_PROJECTION_CLIP_BOTTOM = 1u << 3,
    MDKR_MODERN_CHARACTER_PROJECTION_CLIP_NEAR = 1u << 4,
    MDKR_MODERN_CHARACTER_PROJECTION_CLIP_FAR = 1u << 5,
    MDKR_MODERN_CHARACTER_PROJECTION_CLIP_SCISSOR = 1u << 6,
};

/* Session-only renderer witness for one isolated model capture. Matrices are
 * column-major and map donor-target coordinates directly into WebGPU clip
 * space. Viewport/scissor use top-left output-PNG pixels. This internal record
 * never enters a package or durable performance result as host floats. */
typedef struct MdkrModernCharacterCaptureProjection {
    uint32_t version;
    uint32_t valid;
    uint32_t subject_player;
    uint32_t primitive_draws;
    uint32_t output_width;
    uint32_t output_height;
    int32_t viewport[4];
    int32_t scissor[4];
    float target_to_clip[16];
} MdkrModernCharacterCaptureProjection;

/* A bounded clip-space crop for author inspection. It is derived only from
 * positive-W projections of either exact fitted bounds or the renderer's
 * current posed vertices. Applying it changes an isolated replay matrix,
 * never the gameplay camera or package. */
typedef struct MdkrModernCharacterCaptureFraming {
    uint32_t valid;
    float scale;
    float center_ndc[2];
    float source_ndc_bounds[4]; /* min X/Y, max X/Y */
} MdkrModernCharacterCaptureFraming;

/* Compose camera/object MVP * donor-target transform without relying on a
 * backend-specific matrix helper. Output changes only on complete success. */
int mdkr_modern_character_capture_projection_compose(
    const float mvp[16], const float target_frame[16], float output[16]);

int mdkr_modern_character_capture_framing_solve(
    const float target_to_clip[16], const float bounds_min[3],
    const float bounds_max[3], MdkrModernCharacterCaptureFraming *output);
/* Solve the same bounded authoring crop from an already-projected subject
 * rectangle. This lets a renderer frame the exact current skinned pose while
 * retaining target-space bounds for the projection witness. */
int mdkr_modern_character_capture_framing_solve_ndc(
    const float source_ndc_bounds[4],
    MdkrModernCharacterCaptureFraming *output);
int mdkr_modern_character_capture_framing_apply(
    const float matrix[16],
    const MdkrModernCharacterCaptureFraming *framing, float output[16]);

int mdkr_modern_character_capture_projection_valid(
    const MdkrModernCharacterCaptureProjection *projection);

/* Convert one donor-target point to top-left PNG millipixels and WebGPU depth
 * millionths. Positive-W points outside the viewport/scissor remain valid and
 * carry explicit clip flags; points on/behind the camera plane are refused. */
int mdkr_modern_character_capture_project_point(
    const MdkrModernCharacterCaptureProjection *projection,
    const float point[3], int32_t output_millipixels[2],
    int32_t *depth_millionths, uint32_t *clip_flags);

#ifdef __cplusplus
}
#endif

#endif
