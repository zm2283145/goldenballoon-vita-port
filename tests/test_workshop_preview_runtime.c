#include "workshop_preview_runtime.h"

#include <stdio.h>
#include <string.h>

static int failures;

static void expect(int condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        failures++;
    }
}

int main(void) {
    char error[96];
    int yaw = 99;
    int pitch = 99;
    MdkrWorkshopPreviewVisualMetrics metrics;

    mdkr_workshop_preview_visual_clear();
    mdkr_workshop_preview_visual_metrics_reset();
    expect(!mdkr_workshop_preview_view(&yaw, &pitch) && yaw == 0 &&
               pitch == 0,
           "cleared runtime has the authored camera view");
    expect(mdkr_workshop_preview_lighting() ==
               MDKR_WORKSHOP_PREVIEW_LIGHTING_NEUTRAL,
           "cleared runtime has neutral character lighting");

    expect(mdkr_workshop_preview_visual_set(
               -180, 45, MDKR_WORKSHOP_PREVIEW_LIGHTING_BACKLIT,
               error, sizeof(error)),
           "inclusive view bounds and known lighting are accepted");
    expect(error[0] == '\0' &&
               mdkr_workshop_preview_view(&yaw, &pitch) &&
               yaw == -180 && pitch == 45 &&
               mdkr_workshop_preview_lighting() ==
                   MDKR_WORKSHOP_PREVIEW_LIGHTING_BACKLIT,
           "accepted visual state is returned exactly");

    expect(!mdkr_workshop_preview_visual_set(
               181, 0, MDKR_WORKSHOP_PREVIEW_LIGHTING_NEUTRAL,
               error, sizeof(error)) && error[0] != '\0',
           "out-of-range yaw is rejected with a bounded diagnostic");
    expect(mdkr_workshop_preview_view(&yaw, &pitch) && yaw == -180 &&
               pitch == 45,
           "invalid requests do not partially mutate the current view");
    expect(!mdkr_workshop_preview_visual_set(
               0, -46, MDKR_WORKSHOP_PREVIEW_LIGHTING_NEUTRAL,
               NULL, 0u),
           "out-of-range pitch is rejected without requiring an error buffer");
    expect(!mdkr_workshop_preview_visual_set(
               0, 0, MDKR_WORKSHOP_PREVIEW_LIGHTING_COUNT,
               error, sizeof(error)),
           "unknown lighting is rejected");
    expect(mdkr_workshop_preview_lighting() ==
               MDKR_WORKSHOP_PREVIEW_LIGHTING_BACKLIT,
           "invalid lighting does not mutate the active preset");

    mdkr_workshop_preview_note_camera_override();
    mdkr_workshop_preview_note_camera_override();
    mdkr_workshop_preview_note_lighting_override();
    memset(&metrics, 0, sizeof(metrics));
    mdkr_workshop_preview_visual_metrics(&metrics);
    expect(metrics.camera_override_ticks == 2u &&
               metrics.lighting_override_draws == 1u,
           "presentation-only evidence counters are exact");
    mdkr_workshop_preview_visual_metrics(NULL);
    mdkr_workshop_preview_visual_clear();
    expect(!mdkr_workshop_preview_view(NULL, NULL) &&
               mdkr_workshop_preview_lighting() ==
                   MDKR_WORKSHOP_PREVIEW_LIGHTING_NEUTRAL,
           "clear restores visuals without requiring output pointers");
    mdkr_workshop_preview_visual_metrics(&metrics);
    expect(metrics.camera_override_ticks == 2u &&
               metrics.lighting_override_draws == 1u,
           "clearing visual state does not erase already measured evidence");
    mdkr_workshop_preview_visual_metrics_reset();
    mdkr_workshop_preview_visual_metrics(&metrics);
    expect(metrics.camera_override_ticks == 0u &&
               metrics.lighting_override_draws == 0u,
           "metric reset starts a new measurement epoch");

    if (failures != 0) return 1;
    puts("workshop preview runtime passed");
    return 0;
}
