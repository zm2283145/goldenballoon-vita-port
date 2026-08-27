#include "modern_character_gpu_timing.h"

#include <stdlib.h>
#include <string.h>

static int compare_u64(const void *left, const void *right) {
    const uint64_t a = *(const uint64_t *)left;
    const uint64_t b = *(const uint64_t *)right;
    return a < b ? -1 : a > b ? 1 : 0;
}

static uint64_t percentile(const uint64_t *values, size_t count,
                           uint32_t percentile_value) {
    uint64_t sorted[MDKR_MODERN_CHARACTER_GPU_TIMING_WINDOW];
    size_t index;
    if (values == NULL || count == 0u ||
        count > MDKR_MODERN_CHARACTER_GPU_TIMING_WINDOW ||
        percentile_value > 100u) {
        return 0u;
    }
    memcpy(sorted, values, count * sizeof(sorted[0]));
    qsort(sorted, count, sizeof(sorted[0]), compare_u64);
    /* Nearest-rank percentile: ceil(p * N / 100), clamped to [1, N]. */
    index = ((size_t)percentile_value * count + 99u) / 100u;
    if (index == 0u) index = 1u;
    if (index > count) index = count;
    return sorted[index - 1u];
}

static int add_sample(uint64_t *window, uint32_t *cursor,
                      uint64_t *samples, uint64_t *sum, uint64_t *maximum,
                      uint64_t duration_ns) {
    if (window == NULL || cursor == NULL || samples == NULL || sum == NULL ||
        maximum == NULL || duration_ns == 0u ||
        *samples == UINT64_MAX || *sum > UINT64_MAX - duration_ns) {
        return 0;
    }
    window[*cursor] = duration_ns;
    *cursor = (*cursor + 1u) % MDKR_MODERN_CHARACTER_GPU_TIMING_WINDOW;
    (*samples)++;
    *sum += duration_ns;
    if (duration_ns > *maximum) *maximum = duration_ns;
    return 1;
}

void mdkr_modern_character_gpu_timing_accumulator_reset(
    MdkrModernCharacterGpuTimingAccumulator *accumulator) {
    if (accumulator != NULL) memset(accumulator, 0, sizeof(*accumulator));
}

int mdkr_modern_character_gpu_timing_accumulator_add_scene(
    MdkrModernCharacterGpuTimingAccumulator *accumulator,
    uint64_t duration_ns) {
    if (accumulator == NULL) return 0;
    return add_sample(
        accumulator->scene_window, &accumulator->scene_cursor,
        &accumulator->scene_samples, &accumulator->scene_sum,
        &accumulator->scene_max, duration_ns);
}

int mdkr_modern_character_gpu_timing_accumulator_add_character(
    MdkrModernCharacterGpuTimingAccumulator *accumulator,
    uint64_t duration_ns) {
    if (accumulator == NULL) return 0;
    return add_sample(
        accumulator->character_window, &accumulator->character_cursor,
        &accumulator->character_samples, &accumulator->character_sum,
        &accumulator->character_max, duration_ns);
}

void mdkr_modern_character_gpu_timing_accumulator_note_ring_full(
    MdkrModernCharacterGpuTimingAccumulator *accumulator) {
    if (accumulator != NULL && accumulator->ring_full_frames != UINT64_MAX) {
        accumulator->ring_full_frames++;
    }
}

void mdkr_modern_character_gpu_timing_accumulator_note_invalid(
    MdkrModernCharacterGpuTimingAccumulator *accumulator) {
    if (accumulator != NULL && accumulator->invalid_samples != UINT64_MAX) {
        accumulator->invalid_samples++;
    }
}

static void distribution_snapshot(
    const uint64_t *window, uint64_t samples, uint64_t sum, uint64_t maximum,
    MdkrModernCharacterGpuDistribution *out) {
    const size_t retained = samples < MDKR_MODERN_CHARACTER_GPU_TIMING_WINDOW
        ? (size_t)samples : MDKR_MODERN_CHARACTER_GPU_TIMING_WINDOW;
    memset(out, 0, sizeof(*out));
    out->samples = samples;
    out->percentile_window_samples = retained;
    if (retained == 0u) return;
    out->p50_ns = percentile(window, retained, 50u);
    out->p95_ns = percentile(window, retained, 95u);
    out->p99_ns = percentile(window, retained, 99u);
    out->mean_ns = sum / samples;
    out->max_ns = maximum;
}

void mdkr_modern_character_gpu_timing_snapshot(
    const MdkrModernCharacterGpuTimingAccumulator *accumulator,
    MdkrModernCharacterGpuTimingStatus status, uint32_t supported_scopes,
    MdkrModernCharacterGpuTimingMetrics *out) {
    if (out == NULL) return;
    memset(out, 0, sizeof(*out));
    out->version = MDKR_MODERN_CHARACTER_GPU_TIMING_VERSION;
    out->status = status;
    out->supported_scopes = supported_scopes &
        (MDKR_MODERN_CHARACTER_GPU_SCOPE_SCENE_PASS |
         MDKR_MODERN_CHARACTER_GPU_SCOPE_CHARACTER_DRAWS);
    if (accumulator == NULL) return;
    out->ring_full_frames = accumulator->ring_full_frames;
    out->invalid_samples = accumulator->invalid_samples;
    distribution_snapshot(
        accumulator->scene_window, accumulator->scene_samples,
        accumulator->scene_sum, accumulator->scene_max, &out->scene_pass);
    distribution_snapshot(
        accumulator->character_window, accumulator->character_samples,
        accumulator->character_sum, accumulator->character_max,
        &out->character_draws);
}

static int distribution_valid(
    const MdkrModernCharacterGpuDistribution *distribution,
    int supported) {
    if (!supported) {
        return distribution->samples == 0u &&
            distribution->percentile_window_samples == 0u &&
            distribution->p50_ns == 0u && distribution->p95_ns == 0u &&
            distribution->p99_ns == 0u && distribution->mean_ns == 0u &&
            distribution->max_ns == 0u;
    }
    if (distribution->samples == 0u) {
        return distribution->percentile_window_samples == 0u &&
            distribution->p50_ns == 0u && distribution->p95_ns == 0u &&
            distribution->p99_ns == 0u && distribution->mean_ns == 0u &&
            distribution->max_ns == 0u;
    }
    return distribution->percentile_window_samples > 0u &&
        distribution->percentile_window_samples <=
            MDKR_MODERN_CHARACTER_GPU_TIMING_WINDOW &&
        distribution->percentile_window_samples <= distribution->samples &&
        distribution->p50_ns > 0u &&
        distribution->p50_ns <= distribution->p95_ns &&
        distribution->p95_ns <= distribution->p99_ns &&
        distribution->p99_ns <= distribution->max_ns &&
        distribution->mean_ns > 0u &&
        distribution->mean_ns <= distribution->max_ns;
}

int mdkr_modern_character_gpu_timing_metrics_valid(
    const MdkrModernCharacterGpuTimingMetrics *metrics) {
    const uint32_t known_scopes =
        MDKR_MODERN_CHARACTER_GPU_SCOPE_SCENE_PASS |
        MDKR_MODERN_CHARACTER_GPU_SCOPE_CHARACTER_DRAWS;
    if (metrics == NULL ||
        metrics->version != MDKR_MODERN_CHARACTER_GPU_TIMING_VERSION ||
        metrics->status < MDKR_MODERN_CHARACTER_GPU_TIMING_UNSUPPORTED ||
        metrics->status > MDKR_MODERN_CHARACTER_GPU_TIMING_ERROR ||
        metrics->pending_frames > 64u ||
        (metrics->supported_scopes & ~known_scopes) != 0u ||
        ((metrics->supported_scopes &
              MDKR_MODERN_CHARACTER_GPU_SCOPE_CHARACTER_DRAWS) != 0u &&
         (metrics->supported_scopes &
              MDKR_MODERN_CHARACTER_GPU_SCOPE_SCENE_PASS) == 0u)) {
        return 0;
    }
    if (metrics->status == MDKR_MODERN_CHARACTER_GPU_TIMING_UNSUPPORTED &&
        (metrics->supported_scopes != 0u ||
         metrics->ring_full_frames != 0u ||
         metrics->pending_frames != 0u ||
         metrics->invalid_samples != 0u)) {
        return 0;
    }
    if ((metrics->status == MDKR_MODERN_CHARACTER_GPU_TIMING_IDLE ||
         metrics->status == MDKR_MODERN_CHARACTER_GPU_TIMING_PENDING) &&
        (metrics->scene_pass.samples != 0u ||
         metrics->character_draws.samples != 0u)) {
        return 0;
    }
    if (metrics->status == MDKR_MODERN_CHARACTER_GPU_TIMING_IDLE &&
        (metrics->pending_frames != 0u ||
         metrics->ring_full_frames != 0u ||
         metrics->invalid_samples != 0u)) {
        return 0;
    }
    if (metrics->status == MDKR_MODERN_CHARACTER_GPU_TIMING_AVAILABLE &&
        metrics->scene_pass.samples == 0u &&
        metrics->character_draws.samples == 0u) {
        return 0;
    }
    return distribution_valid(
               &metrics->scene_pass,
               (metrics->supported_scopes &
                    MDKR_MODERN_CHARACTER_GPU_SCOPE_SCENE_PASS) != 0u) &&
        distribution_valid(
            &metrics->character_draws,
            (metrics->supported_scopes &
                 MDKR_MODERN_CHARACTER_GPU_SCOPE_CHARACTER_DRAWS) != 0u);
}
