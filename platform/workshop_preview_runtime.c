#include "workshop_preview_runtime.h"

#include <stdio.h>

static int s_yaw_degrees;
static int s_pitch_degrees;
static MdkrWorkshopPreviewLighting s_lighting =
    MDKR_WORKSHOP_PREVIEW_LIGHTING_NEUTRAL;
static uint64_t s_camera_override_ticks;
static uint64_t s_lighting_override_draws;

static void set_error(char *error, size_t size, const char *message) {
    if (error != NULL && size != 0u) {
        (void)snprintf(error, size, "%s", message != NULL ? message : "");
    }
}

int mdkr_workshop_preview_visual_set(
    int yaw_degrees, int pitch_degrees,
    MdkrWorkshopPreviewLighting lighting,
    char *error, size_t error_size) {
    if (yaw_degrees < -180 || yaw_degrees > 180 ||
        pitch_degrees < -45 || pitch_degrees > 45 ||
        lighting < MDKR_WORKSHOP_PREVIEW_LIGHTING_NEUTRAL ||
        lighting >= MDKR_WORKSHOP_PREVIEW_LIGHTING_COUNT) {
        set_error(error, error_size,
                  "character inspection view or lighting is invalid");
        return 0;
    }
    s_yaw_degrees = yaw_degrees;
    s_pitch_degrees = pitch_degrees;
    s_lighting = lighting;
    set_error(error, error_size, "");
    return 1;
}

void mdkr_workshop_preview_visual_clear(void) {
    s_yaw_degrees = 0;
    s_pitch_degrees = 0;
    s_lighting = MDKR_WORKSHOP_PREVIEW_LIGHTING_NEUTRAL;
}

int mdkr_workshop_preview_view(int *yaw_degrees, int *pitch_degrees) {
    if (yaw_degrees != NULL) *yaw_degrees = s_yaw_degrees;
    if (pitch_degrees != NULL) *pitch_degrees = s_pitch_degrees;
    return s_yaw_degrees != 0 || s_pitch_degrees != 0;
}

MdkrWorkshopPreviewLighting mdkr_workshop_preview_lighting(void) {
    return s_lighting;
}

void mdkr_workshop_preview_note_camera_override(void) {
    s_camera_override_ticks++;
}

void mdkr_workshop_preview_note_lighting_override(void) {
    s_lighting_override_draws++;
}

void mdkr_workshop_preview_visual_metrics_reset(void) {
    s_camera_override_ticks = 0u;
    s_lighting_override_draws = 0u;
}

void mdkr_workshop_preview_visual_metrics(
    MdkrWorkshopPreviewVisualMetrics *out) {
    if (out == NULL) return;
    out->camera_override_ticks = s_camera_override_ticks;
    out->lighting_override_draws = s_lighting_override_draws;
}
