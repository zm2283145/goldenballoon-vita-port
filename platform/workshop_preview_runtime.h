#ifndef MDKR64_WORKSHOP_PREVIEW_RUNTIME_H
#define MDKR64_WORKSHOP_PREVIEW_RUNTIME_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum MdkrWorkshopPreviewLighting {
    MDKR_WORKSHOP_PREVIEW_LIGHTING_NEUTRAL = 0,
    MDKR_WORKSHOP_PREVIEW_LIGHTING_BRIGHT,
    MDKR_WORKSHOP_PREVIEW_LIGHTING_LOW_KEY,
    MDKR_WORKSHOP_PREVIEW_LIGHTING_BACKLIT,
    MDKR_WORKSHOP_PREVIEW_LIGHTING_COUNT,
} MdkrWorkshopPreviewLighting;

typedef struct MdkrWorkshopPreviewVisualMetrics {
    uint64_t camera_override_ticks;
    uint64_t lighting_override_draws;
} MdkrWorkshopPreviewVisualMetrics;

/* Presentation-only state for an exact Workshop inspection. Zero view angles
 * preserve the ordinary gameplay camera; nonzero angles select an absolute
 * racer-relative orbit which the game aims at renderer-published fitted
 * bounds. Lighting affects only the replacement character. Nothing writes
 * racer, collision, package tuning, or save authority. */
int mdkr_workshop_preview_visual_set(
    int yaw_degrees, int pitch_degrees,
    MdkrWorkshopPreviewLighting lighting,
    char *error, size_t error_size);
void mdkr_workshop_preview_visual_clear(void);
int mdkr_workshop_preview_view(int *yaw_degrees, int *pitch_degrees);
MdkrWorkshopPreviewLighting mdkr_workshop_preview_lighting(void);
void mdkr_workshop_preview_note_camera_override(void);
void mdkr_workshop_preview_note_lighting_override(void);
void mdkr_workshop_preview_visual_metrics_reset(void);
void mdkr_workshop_preview_visual_metrics(
    MdkrWorkshopPreviewVisualMetrics *out);

#ifdef __cplusplus
}
#endif

#endif /* MDKR64_WORKSHOP_PREVIEW_RUNTIME_H */
