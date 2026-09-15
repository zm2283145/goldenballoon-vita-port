/* Assert-driven test: release builds must not compile its checks away. */
#undef NDEBUG

#include "modern_character_gpu_timing.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

static MdkrModernCharacterGpuTimingAccumulator s_accumulator;

static void test_empty_status_contracts(void) {
    MdkrModernCharacterGpuTimingMetrics metrics;
    memset(&metrics, 0xA5, sizeof(metrics));
    mdkr_modern_character_gpu_timing_snapshot(
        NULL, MDKR_MODERN_CHARACTER_GPU_TIMING_UNSUPPORTED, 0u, &metrics);
    assert(metrics.version == MDKR_MODERN_CHARACTER_GPU_TIMING_VERSION);
    assert(metrics.status == MDKR_MODERN_CHARACTER_GPU_TIMING_UNSUPPORTED);
    assert(mdkr_modern_character_gpu_timing_metrics_valid(&metrics));

    metrics.status = MDKR_MODERN_CHARACTER_GPU_TIMING_IDLE;
    metrics.supported_scopes = MDKR_MODERN_CHARACTER_GPU_SCOPE_SCENE_PASS;
    assert(mdkr_modern_character_gpu_timing_metrics_valid(&metrics));
    metrics.status = MDKR_MODERN_CHARACTER_GPU_TIMING_PENDING;
    metrics.pending_frames = 2u;
    assert(mdkr_modern_character_gpu_timing_metrics_valid(&metrics));

    metrics.status = MDKR_MODERN_CHARACTER_GPU_TIMING_AVAILABLE;
    assert(!mdkr_modern_character_gpu_timing_metrics_valid(&metrics));
    metrics.status = MDKR_MODERN_CHARACTER_GPU_TIMING_UNSUPPORTED;
    assert(!mdkr_modern_character_gpu_timing_metrics_valid(&metrics));
}

static void test_distribution_and_scope_contract(void) {
    MdkrModernCharacterGpuTimingMetrics metrics;
    mdkr_modern_character_gpu_timing_accumulator_reset(&s_accumulator);
    assert(!mdkr_modern_character_gpu_timing_accumulator_add_scene(
        &s_accumulator, 0u));
    assert(mdkr_modern_character_gpu_timing_accumulator_add_scene(
        &s_accumulator, 100u));
    assert(mdkr_modern_character_gpu_timing_accumulator_add_scene(
        &s_accumulator, 200u));
    assert(mdkr_modern_character_gpu_timing_accumulator_add_scene(
        &s_accumulator, 300u));
    assert(mdkr_modern_character_gpu_timing_accumulator_add_scene(
        &s_accumulator, 400u));
    assert(mdkr_modern_character_gpu_timing_accumulator_add_character(
        &s_accumulator, 25u));
    assert(mdkr_modern_character_gpu_timing_accumulator_add_character(
        &s_accumulator, 75u));
    mdkr_modern_character_gpu_timing_accumulator_note_ring_full(
        &s_accumulator);
    mdkr_modern_character_gpu_timing_accumulator_note_invalid(
        &s_accumulator);
    mdkr_modern_character_gpu_timing_snapshot(
        &s_accumulator, MDKR_MODERN_CHARACTER_GPU_TIMING_AVAILABLE,
        MDKR_MODERN_CHARACTER_GPU_SCOPE_SCENE_PASS |
            MDKR_MODERN_CHARACTER_GPU_SCOPE_CHARACTER_DRAWS,
        &metrics);
    assert(metrics.ring_full_frames == 1u);
    assert(metrics.invalid_samples == 1u);
    assert(metrics.scene_pass.samples == 4u);
    assert(metrics.scene_pass.percentile_window_samples == 4u);
    assert(metrics.scene_pass.p50_ns == 200u);
    assert(metrics.scene_pass.p95_ns == 400u);
    assert(metrics.scene_pass.p99_ns == 400u);
    assert(metrics.scene_pass.mean_ns == 250u);
    assert(metrics.scene_pass.max_ns == 400u);
    assert(metrics.character_draws.samples == 2u);
    assert(metrics.character_draws.p50_ns == 25u);
    assert(metrics.character_draws.p95_ns == 75u);
    assert(metrics.character_draws.mean_ns == 50u);
    assert(mdkr_modern_character_gpu_timing_metrics_valid(&metrics));

    metrics.supported_scopes =
        MDKR_MODERN_CHARACTER_GPU_SCOPE_CHARACTER_DRAWS;
    assert(!mdkr_modern_character_gpu_timing_metrics_valid(&metrics));
    metrics.supported_scopes = MDKR_MODERN_CHARACTER_GPU_SCOPE_SCENE_PASS;
    assert(!mdkr_modern_character_gpu_timing_metrics_valid(&metrics));
}

static void test_bounded_latest_window(void) {
    MdkrModernCharacterGpuTimingMetrics metrics;
    mdkr_modern_character_gpu_timing_accumulator_reset(&s_accumulator);
    for (uint64_t value = 1u;
         value <= MDKR_MODERN_CHARACTER_GPU_TIMING_WINDOW + 2u; ++value) {
        assert(mdkr_modern_character_gpu_timing_accumulator_add_scene(
            &s_accumulator, value));
    }
    mdkr_modern_character_gpu_timing_snapshot(
        &s_accumulator, MDKR_MODERN_CHARACTER_GPU_TIMING_AVAILABLE,
        MDKR_MODERN_CHARACTER_GPU_SCOPE_SCENE_PASS, &metrics);
    assert(metrics.scene_pass.samples ==
           MDKR_MODERN_CHARACTER_GPU_TIMING_WINDOW + 2u);
    assert(metrics.scene_pass.percentile_window_samples ==
           MDKR_MODERN_CHARACTER_GPU_TIMING_WINDOW);
    /* Values 1 and 2 were replaced; the latest bounded window is 3..2050. */
    assert(metrics.scene_pass.p50_ns == 1026u);
    assert(metrics.scene_pass.p95_ns == 1948u);
    assert(metrics.scene_pass.p99_ns == 2030u);
    assert(metrics.scene_pass.max_ns == 2050u);
    assert(mdkr_modern_character_gpu_timing_metrics_valid(&metrics));
}

static void test_scene_sample_quality(void) {
    mdkr_modern_character_gpu_timing_accumulator_reset(&s_accumulator);
    assert(!mdkr_modern_character_gpu_timing_scene_quality_sufficient(
        &s_accumulator));
    assert(mdkr_modern_character_gpu_timing_accumulator_add_scene(
        &s_accumulator, 100u));
    mdkr_modern_character_gpu_timing_accumulator_note_scene_invalid(
        &s_accumulator);
    assert(!mdkr_modern_character_gpu_timing_scene_quality_sufficient(
        &s_accumulator));
    assert(s_accumulator.invalid_samples == 1u);
    assert(s_accumulator.scene_invalid_samples == 1u);
    assert(mdkr_modern_character_gpu_timing_accumulator_add_scene(
        &s_accumulator, 200u));
    assert(mdkr_modern_character_gpu_timing_scene_quality_sufficient(
        &s_accumulator));
    mdkr_modern_character_gpu_timing_accumulator_note_invalid(
        &s_accumulator);
    assert(mdkr_modern_character_gpu_timing_scene_quality_sufficient(
        &s_accumulator));
    assert(s_accumulator.invalid_samples == 2u);
    assert(s_accumulator.scene_invalid_samples == 1u);
}

static void test_overflow_and_malformed_metrics(void) {
    MdkrModernCharacterGpuTimingMetrics metrics;
    mdkr_modern_character_gpu_timing_accumulator_reset(&s_accumulator);
    s_accumulator.scene_samples = 1u;
    s_accumulator.scene_sum = UINT64_MAX;
    assert(!mdkr_modern_character_gpu_timing_accumulator_add_scene(
        &s_accumulator, 1u));

    memset(&metrics, 0, sizeof(metrics));
    metrics.version = MDKR_MODERN_CHARACTER_GPU_TIMING_VERSION;
    metrics.status = MDKR_MODERN_CHARACTER_GPU_TIMING_AVAILABLE;
    metrics.supported_scopes = MDKR_MODERN_CHARACTER_GPU_SCOPE_SCENE_PASS;
    metrics.scene_pass.samples = 1u;
    metrics.scene_pass.percentile_window_samples = 1u;
    metrics.scene_pass.p50_ns = 5u;
    metrics.scene_pass.p95_ns = 4u;
    metrics.scene_pass.p99_ns = 5u;
    metrics.scene_pass.mean_ns = 5u;
    metrics.scene_pass.max_ns = 5u;
    assert(!mdkr_modern_character_gpu_timing_metrics_valid(&metrics));
    metrics.scene_pass.p95_ns = 5u;
    assert(mdkr_modern_character_gpu_timing_metrics_valid(&metrics));
    metrics.version++;
    assert(!mdkr_modern_character_gpu_timing_metrics_valid(&metrics));
}

int main(void) {
    test_empty_status_contracts();
    test_distribution_and_scope_contract();
    test_bounded_latest_window();
    test_scene_sample_quality();
    test_overflow_and_malformed_metrics();
    return 0;
}
