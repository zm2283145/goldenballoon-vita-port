#include "gfx_character_text.h"

#include <string.h>

static void mark_unavailable(GfxCharacterTextMetrics *metrics) {
    if (metrics == NULL) return;
    memset(metrics, 0, sizeof(*metrics));
    metrics->terminated = true;
    metrics->valid_utf8 = true;
    metrics->fallback_reason = GFX_CHARACTER_TEXT_FALLBACK_FONT_UNAVAILABLE;
}

bool gfx_character_text_measure(const char *source, size_t source_capacity,
                                uint32_t max_width, uint32_t height,
                                GfxCharacterTextMetrics *metrics) {
    (void)source;
    (void)source_capacity;
    (void)max_width;
    (void)height;
    mark_unavailable(metrics);
    return false;
}

bool gfx_character_text_render_rgba(
    const char *source, size_t source_capacity, uint32_t max_width,
    uint32_t height, uint8_t *output, size_t output_size,
    size_t output_stride, GfxCharacterTextMetrics *metrics) {
    (void)source;
    (void)source_capacity;
    (void)max_width;
    (void)height;
    (void)output;
    (void)output_size;
    (void)output_stride;
    mark_unavailable(metrics);
    return false;
}

const char *gfx_character_text_fallback_reason_name(
    GfxCharacterTextFallbackReason reason) {
    return reason == GFX_CHARACTER_TEXT_FALLBACK_FONT_UNAVAILABLE
               ? "font unavailable"
               : "Vita retail-font fallback";
}

const char *gfx_character_text_latin_face_base85(void) { return NULL; }
const char *gfx_character_text_arabic_face_base85(void) { return NULL; }
const char *gfx_character_text_hebrew_face_base85(void) { return NULL; }
void gfx_character_text_shutdown(void) {}
