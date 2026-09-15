#include "camera_motion_shoulder.h"

#include <stdio.h>
#include <stdlib.h>

static void expect(int condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        exit(1);
    }
}

static void test_held_eye_and_translating_pivot(void) {
    MdkrCameraShoulderHistory history = { 0 };
    const MdkrCameraVec3 pivot = { 1000.0f, 20.0f, -1000.0f };
    const MdkrCameraVec3 eye = { 950.0f, 80.0f, -950.0f };
    const MdkrCameraVec3 moved_pivot = { 1024.0f, 25.0f, -960.0f };
    const MdkrCameraVec3 moved_eye = { 974.0f, 85.0f, -910.0f };

    expect(mdkr_camera_shoulder_sample(&history, eye, pivot, 1, 1) ==
               MDKR_CAMERA_SHOULDER_UNCHANGED, "first shoulder establishes history");
    expect(mdkr_camera_shoulder_sample(&history, eye, pivot, 0, 1) ==
               MDKR_CAMERA_SHOULDER_UNCHANGED, "deadband is not a switch");
    expect(mdkr_camera_shoulder_sample(&history, moved_eye, moved_pivot, -1, 1) ==
               MDKR_CAMERA_SHOULDER_BASIS_CROSSING,
           "held relative eye with a rotating reference is not a camera flip");
    expect(mdkr_camera_shoulder_sample(&history, moved_eye, moved_pivot, 1, 1) ==
               MDKR_CAMERA_SHOULDER_BASIS_CROSSING,
           "reverse reference crossing remains observable");
}

static void test_real_switches_survive_the_deadband(void) {
    MdkrCameraShoulderHistory history = { 0 };
    const MdkrCameraVec3 pivot = { 0.0f, 0.0f, 0.0f };
    MdkrCameraVec3 eye = { 4.0f, 50.0f, 100.0f };

    (void)mdkr_camera_shoulder_sample(&history, eye, pivot, 1, 1);
    eye.x = 0.5f;
    (void)mdkr_camera_shoulder_sample(&history, eye, pivot, 0, 1);
    eye.x = -0.5f;
    (void)mdkr_camera_shoulder_sample(&history, eye, pivot, 0, 1);
    eye.x = -4.0f;
    expect(mdkr_camera_shoulder_sample(&history, eye, pivot, -1, 1) ==
               MDKR_CAMERA_SHOULDER_FLIP, "slow actual switch is still a flip");
    eye.x = 4.0f;
    expect(mdkr_camera_shoulder_sample(&history, eye, pivot, 1, 1) ==
               MDKR_CAMERA_SHOULDER_FLIP, "immediate actual return is still a flip");
    (void)mdkr_camera_shoulder_sample(&history, eye, pivot, 0, 0);
    eye.x = -4.0f;
    expect(mdkr_camera_shoulder_sample(&history, eye, pivot, -1, 1) ==
               MDKR_CAMERA_SHOULDER_UNCHANGED, "inactive shot retires history");
    eye.x = NAN;
    (void)mdkr_camera_shoulder_sample(&history, eye, pivot, 1, 1);
    eye.x = 4.0f;
    expect(mdkr_camera_shoulder_sample(&history, eye, pivot, 1, 1) ==
               MDKR_CAMERA_SHOULDER_UNCHANGED, "invalid pose retires history");
}

static void test_float_quantization_is_not_a_motion_tolerance(void) {
    MdkrCameraShoulderHistory history = { 0 };
    const MdkrCameraVec3 pivot = { 4096.0f, 20.0f, -4096.0f };
    MdkrCameraVec3 eye = { 4046.0f, 80.0f, -4046.0f };

    (void)mdkr_camera_shoulder_sample(&history, eye, pivot, 1, 1);
    eye.x = nextafterf(eye.x, INFINITY);
    expect(mdkr_camera_shoulder_sample(&history, eye, pivot, -1, 1) ==
               MDKR_CAMERA_SHOULDER_BASIS_CROSSING,
           "one quantization step is compatible with an unchanged relative eye");
    eye.x += 0.125f;
    expect(mdkr_camera_shoulder_sample(&history, eye, pivot, 1, 1) ==
               MDKR_CAMERA_SHOULDER_FLIP,
           "measurable sub-unit movement must not be excused as a basis crossing");
}

static void test_rounding_across_deadband_cannot_hide_slow_motion(void) {
    MdkrCameraShoulderHistory history = { 0 };
    const MdkrCameraVec3 pivot = { 4096.0f, 20.0f, -4096.0f };
    MdkrCameraVec3 eye = { 4046.0f, 80.0f, -4046.0f };
    int step;

    (void)mdkr_camera_shoulder_sample(&history, eye, pivot, 1, 1);
    for (step = 0; step < 3; ++step) {
        eye.x = nextafterf(eye.x, INFINITY);
        (void)mdkr_camera_shoulder_sample(&history, eye, pivot, 0, 1);
    }
    expect(mdkr_camera_shoulder_sample(&history, eye, pivot, -1, 1) ==
               MDKR_CAMERA_SHOULDER_BASIS_CROSSING,
           "successive held-pose rounding through the deadband is not a switch");
    for (step = 0; step < 8192; ++step) {
        eye.x = nextafterf(eye.x, INFINITY);
        (void)mdkr_camera_shoulder_sample(&history, eye, pivot, 0, 1);
    }
    expect(mdkr_camera_shoulder_sample(&history, eye, pivot, 1, 1) ==
               MDKR_CAMERA_SHOULDER_FLIP,
           "cumulative measurable motion cannot hide behind per-tick rounding");
}

int main(void) {
    test_held_eye_and_translating_pivot();
    test_real_switches_survive_the_deadband();
    test_float_quantization_is_not_a_motion_tolerance();
    test_rounding_across_deadband_cannot_hide_slow_motion();
    puts("camera motion shoulder census: PASS");
    return 0;
}
