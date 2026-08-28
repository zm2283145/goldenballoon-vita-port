#include "fast3d/gfx_character_text.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { WIDTH = 240, HEIGHT = 16 };

static unsigned char pixels[WIDTH * HEIGHT * 4];

static void require(int condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "character text test failed: %s\n", message);
        exit(1);
    }
}

static size_t alpha_count(uint32_t width) {
    size_t count = 0u;
    uint32_t x;
    uint32_t y;
    for (y = 0u; y < HEIGHT; ++y) {
        for (x = 0u; x < width; ++x) {
            if (pixels[(y * WIDTH + x) * 4u + 3u] != 0u) count++;
        }
    }
    return count;
}

static void require_transparent_rgb_zero(void) {
    size_t index;
    for (index = 0u; index < sizeof(pixels); index += 4u) {
        if (pixels[index + 3u] == 0u) {
            require(pixels[index] == 0u && pixels[index + 1u] == 0u &&
                    pixels[index + 2u] == 0u,
                    "transparent texel has hidden matte RGB");
        }
    }
}

static void test_direct_scripts(void) {
    static const char latin[] = "Dixie K\xC3\xABng";
    static const char greek[] = "\xCE\x94\xCE\xAF\xCE\xBE\xCE\xB9";
    static const char cyrillic[] = "\xD0\x96\xD1\x83\xD0\xBA";
    const char *samples[] = { latin, greek, cyrillic };
    size_t sample;
    for (sample = 0u; sample < sizeof(samples) / sizeof(samples[0]); ++sample) {
        GfxCharacterTextMetrics metrics;
        memset(pixels, 0xA5, sizeof(pixels));
        require(gfx_character_text_render_rgba(
                    samples[sample], strlen(samples[sample]) + 1u,
                    WIDTH, HEIGHT, pixels, sizeof(pixels), WIDTH * 4u,
                    &metrics),
                "supported LTR sample did not render");
        require(metrics.direct_renderable && metrics.valid_utf8 &&
                    metrics.terminated && metrics.width > 2u &&
                    metrics.width <= WIDTH && metrics.rendered_glyphs > 0u,
                "supported LTR metrics are incomplete");
        require(alpha_count(metrics.width) > 0u,
                "supported LTR sample produced no ink");
        require_transparent_rgb_zero();
    }
}

static void test_honest_fallbacks(void) {
    static const char combining[] = "e\xCC\x81";
    static const char arabic[] = "\xD8\xAF\xD9\x8A\xD8\xAF\xD9\x8A";
    static const char emoji[] = "Dixie \xF0\x9F\x8F\x81";
    static const char malformed[] = { 'x', (char)0xC0, (char)0xAF, 0 };
    GfxCharacterTextMetrics metrics;
    require(!gfx_character_text_measure(combining, sizeof(combining), WIDTH,
                                        HEIGHT, &metrics) &&
                metrics.fallback_reason ==
                    GFX_CHARACTER_TEXT_FALLBACK_SHAPING_REQUIRED &&
                metrics.shaping_codepoints == 1u,
            "combining sequence was not routed to shaping fallback");
    require(!gfx_character_text_measure(arabic, sizeof(arabic), WIDTH,
                                        HEIGHT, &metrics) &&
                metrics.fallback_reason ==
                    GFX_CHARACTER_TEXT_FALLBACK_SHAPING_REQUIRED,
            "joining script entered direct renderer");
    require(!gfx_character_text_measure(emoji, sizeof(emoji), WIDTH,
                                        HEIGHT, &metrics) &&
                metrics.fallback_reason ==
                    GFX_CHARACTER_TEXT_FALLBACK_SHAPING_REQUIRED,
            "emoji entered direct renderer");
    require(!gfx_character_text_measure(malformed, sizeof(malformed), WIDTH,
                                        HEIGHT, &metrics) &&
                !metrics.valid_utf8 &&
                metrics.fallback_reason ==
                    GFX_CHARACTER_TEXT_FALLBACK_INVALID_UTF8,
            "malformed UTF-8 was accepted");
    require(!gfx_character_text_measure("not terminated", 3u, WIDTH,
                                        HEIGHT, &metrics) &&
                metrics.fallback_reason ==
                    GFX_CHARACTER_TEXT_FALLBACK_UNTERMINATED,
            "unterminated bounded source was accepted");
}

static void test_fit_and_determinism(void) {
    static const char long_name[] =
        "Dixie Kong and the Extraordinary Racing Friends";
    unsigned char first[sizeof(pixels)];
    GfxCharacterTextMetrics metrics;
    require(gfx_character_text_render_rgba(
                long_name, sizeof(long_name), 54u, HEIGHT,
                pixels, sizeof(pixels), WIDTH * 4u, &metrics),
            "long direct name did not fit");
    require(metrics.truncated && metrics.width <= 54u,
            "long direct name did not use bounded ellipsis fitting");
    memcpy(first, pixels, sizeof(first));
    memset(pixels, 0, sizeof(pixels));
    require(gfx_character_text_render_rgba(
                long_name, sizeof(long_name), 54u, HEIGHT,
                pixels, sizeof(pixels), WIDTH * 4u, &metrics),
            "second deterministic render failed");
    require(memcmp(first, pixels, sizeof(pixels)) == 0,
            "same name produced different pixels");
    {
        char maximum_field[97];
        memset(maximum_field, 'A', 94u);
        maximum_field[94] = (char)0xC3;
        maximum_field[95] = (char)0xAB;
        maximum_field[96] = '\0';
        require(gfx_character_text_render_rgba(
                    maximum_field, sizeof(maximum_field), 54u, HEIGHT,
                    pixels, sizeof(pixels), WIDTH * 4u, &metrics) &&
                    metrics.input_codepoints == 95u && metrics.truncated,
                "maximum package identity field hit a hidden glyph cap");
    }
}

static void test_argument_bounds(void) {
    GfxCharacterTextMetrics metrics;
    require(!gfx_character_text_render_rgba(
                "Dixie", 6u, WIDTH + 1u, HEIGHT,
                pixels, sizeof(pixels), WIDTH * 4u, &metrics),
            "oversized width was accepted");
    require(!gfx_character_text_render_rgba(
                "Dixie", 6u, WIDTH, HEIGHT,
                pixels, sizeof(pixels) - 1u, WIDTH * 4u, &metrics),
            "short output buffer was accepted");
    require(!gfx_character_text_measure("", 1u, WIDTH, HEIGHT, &metrics),
            "empty identity text was accepted");
    require(!gfx_character_text_measure(
                "Dixie", 6u, 1u, HEIGHT, &metrics) &&
                metrics.fallback_reason ==
                    GFX_CHARACTER_TEXT_FALLBACK_TOO_NARROW,
            "impossible destination fit was not diagnosed");
}

int main(void) {
    test_direct_scripts();
    test_honest_fallbacks();
    test_fit_and_determinism();
    test_argument_bounds();
    gfx_character_text_shutdown();
    test_direct_scripts();
    gfx_character_text_shutdown();
    puts("character text tests passed");
    return 0;
}
