/* Honest GPU timestamp evidence for the Custom Character Workshop. */
#ifndef MDKR64_MODERN_CHARACTER_GPU_TIMING_H
#define MDKR64_MODERN_CHARACTER_GPU_TIMING_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MDKR_MODERN_CHARACTER_GPU_TIMING_VERSION 1u
#define MDKR_MODERN_CHARACTER_GPU_TIMING_WINDOW 2048u

typedef enum MdkrModernCharacterGpuTimingStatus {
    /* The selected renderer/device cannot issue timestamp queries. */
    MDKR_MODERN_CHARACTER_GPU_TIMING_UNSUPPORTED = 0,
    /* Timestamp queries are supported, but no Workshop measurement is active. */
    MDKR_MODERN_CHARACTER_GPU_TIMING_IDLE,
    /* A measurement is active, but no asynchronous readback has completed. */
    MDKR_MODERN_CHARACTER_GPU_TIMING_PENDING,
    /* At least one valid timestamp sample completed. */
    MDKR_MODERN_CHARACTER_GPU_TIMING_AVAILABLE,
    /* The device was lost while evidence was being collected. */
    MDKR_MODERN_CHARACTER_GPU_TIMING_DEVICE_LOST,
    /* Query allocation, resolution, mapping, or timestamp validation failed. */
    MDKR_MODERN_CHARACTER_GPU_TIMING_ERROR,
} MdkrModernCharacterGpuTimingStatus;

enum {
    /* Initial load/store gameplay scene pass. Includes world, racers, and the
     * visible custom-character draws; excludes post effects, UI, and present. */
    MDKR_MODERN_CHARACTER_GPU_SCOPE_SCENE_PASS = 1u << 0,
    /* Sum of exact in-pass ranges around accepted modern skinned primitives.
     * This excludes world draws and rejected character draws. */
    MDKR_MODERN_CHARACTER_GPU_SCOPE_CHARACTER_DRAWS = 1u << 1,
};

typedef struct MdkrModernCharacterGpuDistribution {
    /* All valid samples observed since measurement reset. */
    uint64_t samples;
    /* Samples retained for the bounded percentile window. This is equal to
     * samples until the window fills, then remains fixed at the latest 2048. */
    uint64_t percentile_window_samples;
    uint64_t p50_ns;
    uint64_t p95_ns;
    uint64_t p99_ns;
    uint64_t mean_ns;
    uint64_t max_ns;
} MdkrModernCharacterGpuDistribution;

typedef struct MdkrModernCharacterGpuTimingMetrics {
    uint32_t version;
    MdkrModernCharacterGpuTimingStatus status;
    uint32_t supported_scopes;
    uint64_t ring_full_frames;
    /* Submitted query frames still awaiting nonblocking map completion when
     * the snapshot was taken; they are intentionally absent from statistics. */
    uint64_t pending_frames;
    uint64_t invalid_samples;
    MdkrModernCharacterGpuDistribution scene_pass;
    MdkrModernCharacterGpuDistribution character_draws;
} MdkrModernCharacterGpuTimingMetrics;

/* Backend-owned streaming accumulator. It is public so its arithmetic and
 * percentile contracts can be unit-tested without a GPU. */
typedef struct MdkrModernCharacterGpuTimingAccumulator {
    uint64_t scene_window[MDKR_MODERN_CHARACTER_GPU_TIMING_WINDOW];
    uint64_t character_window[MDKR_MODERN_CHARACTER_GPU_TIMING_WINDOW];
    uint64_t scene_samples;
    uint64_t character_samples;
    uint64_t scene_sum;
    uint64_t character_sum;
    uint64_t scene_max;
    uint64_t character_max;
    uint32_t scene_cursor;
    uint32_t character_cursor;
    uint64_t ring_full_frames;
    uint64_t invalid_samples;
    /* Invalid beginning/end scene-pass pairs. This remains separate from the
     * public aggregate because a valid scene sample can share a frame with an
     * invalid optional character-draw range. */
    uint64_t scene_invalid_samples;
} MdkrModernCharacterGpuTimingAccumulator;

void mdkr_modern_character_gpu_timing_accumulator_reset(
    MdkrModernCharacterGpuTimingAccumulator *accumulator);

/* A zero duration is invalid: it represents either an unwritten query or
 * insufficient device timestamp resolution, never a useful cost sample. */
int mdkr_modern_character_gpu_timing_accumulator_add_scene(
    MdkrModernCharacterGpuTimingAccumulator *accumulator,
    uint64_t duration_ns);
int mdkr_modern_character_gpu_timing_accumulator_add_character(
    MdkrModernCharacterGpuTimingAccumulator *accumulator,
    uint64_t duration_ns);
void mdkr_modern_character_gpu_timing_accumulator_note_ring_full(
    MdkrModernCharacterGpuTimingAccumulator *accumulator);
void mdkr_modern_character_gpu_timing_accumulator_note_invalid(
    MdkrModernCharacterGpuTimingAccumulator *accumulator);
void mdkr_modern_character_gpu_timing_accumulator_note_scene_invalid(
    MdkrModernCharacterGpuTimingAccumulator *accumulator);

/* A percentile window is actionable only when at least two thirds of its
 * completed scene-pass queries are valid. Devices may expose TimestampQuery
 * yet return zero pairs under some Metal/driver conditions; callers must
 * present that as a measurement error, never as a one-sample performance
 * conclusion. */
int mdkr_modern_character_gpu_timing_scene_quality_sufficient(
    const MdkrModernCharacterGpuTimingAccumulator *accumulator);

void mdkr_modern_character_gpu_timing_snapshot(
    const MdkrModernCharacterGpuTimingAccumulator *accumulator,
    MdkrModernCharacterGpuTimingStatus status, uint32_t supported_scopes,
    MdkrModernCharacterGpuTimingMetrics *out);

/* Structural validation for app-boundary and durable evidence consumers. */
int mdkr_modern_character_gpu_timing_metrics_valid(
    const MdkrModernCharacterGpuTimingMetrics *metrics);

#ifdef __cplusplus
}
#endif

#endif /* MDKR64_MODERN_CHARACTER_GPU_TIMING_H */
