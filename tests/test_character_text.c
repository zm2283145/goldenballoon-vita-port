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

static size_t ink_row_count(uint32_t width) {
    size_t rows = 0u;
    uint32_t y;
    for (y = 0u; y < HEIGHT; ++y) {
        uint32_t x;
        for (x = 0u; x < width; ++x) {
            if (pixels[(y * WIDTH + x) * 4u + 3u] != 0u) {
                rows++;
                break;
            }
        }
    }
    return rows;
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

static void test_shaping_and_honest_fallbacks(void) {
    static const char combining[] = "e\xCC\x81";
    static const char arabic[] = "\xD8\xAF\xD9\x8A\xD8\xAF\xD9\x8A";
    static const char arabic_unjoined[] =
        "\xD8\xAF\xE2\x80\x8C\xD9\x8A\xE2\x80\x8C"
        "\xD8\xAF\xE2\x80\x8C\xD9\x8A";
    static const char hebrew[] =
        "\xD7\x93\xD7\x99\xD7\xA7\xD7\xA1\xD7\x99";
    static const char mixed[] =
        "Dixie 12 \xD7\x93\xD7\x99\xD7\xA7\xD7\xA1\xD7\x99";
    static const char emoji[] = "Dixie \xF0\x9F\x8F\x81";
    static const char bidi_override[] = "Dixie \xE2\x80\xAEgnik";
    static const char standalone_mark[] = "\xCC\x81";
    static const char nonbreaking_spaces[] = "\xC2\xA0\xC2\xA0";
    static const char stray_joiner[] = "Dixie\xE2\x80\x8D";
    static const char malformed[] = { 'x', (char)0xC0, (char)0xAF, 0 };
    GfxCharacterTextMetrics metrics;
    unsigned char joined_pixels[sizeof(pixels)];
    unsigned char hebrew_pixels[sizeof(pixels)];
    require(gfx_character_text_render_rgba(
                combining, sizeof(combining), WIDTH, HEIGHT, pixels,
                sizeof(pixels), WIDTH * 4u, &metrics) &&
                metrics.native_renderable && metrics.shaping_applied &&
                metrics.shaping_codepoints == 1u &&
                metrics.rendered_glyphs == 1u && alpha_count(metrics.width) > 0u,
            "canonical combining sequence was not shaped natively");
    require(gfx_character_text_render_rgba(
                arabic, sizeof(arabic), WIDTH, HEIGHT, pixels,
                sizeof(pixels), WIDTH * 4u, &metrics) &&
                metrics.native_renderable && metrics.right_to_left &&
                metrics.bidi_runs == 1u && metrics.shaping_codepoints == 4u &&
                alpha_count(metrics.width) > 10u &&
                ink_row_count(metrics.width) >= 3u,
            "Arabic joining name was not shaped right-to-left");
    memcpy(joined_pixels, pixels, sizeof(pixels));
    require(gfx_character_text_render_rgba(
                arabic_unjoined, sizeof(arabic_unjoined), WIDTH, HEIGHT,
                pixels, sizeof(pixels), WIDTH * 4u, &metrics) &&
                memcmp(joined_pixels, pixels, sizeof(pixels)) != 0,
            "Arabic join controls did not affect shaped output");
    require(gfx_character_text_render_rgba(
                hebrew, sizeof(hebrew), WIDTH, HEIGHT, pixels,
                sizeof(pixels), WIDTH * 4u, &metrics) &&
                metrics.native_renderable && metrics.right_to_left &&
                metrics.bidi_runs == 1u && alpha_count(metrics.width) > 10u &&
                ink_row_count(metrics.width) >= 3u,
            "Hebrew name was not rendered right-to-left");
    memcpy(hebrew_pixels, pixels, sizeof(pixels));
    require(gfx_character_text_render_rgba(
                mixed, sizeof(mixed), WIDTH, HEIGHT, pixels,
                sizeof(pixels), WIDTH * 4u, &metrics) &&
                metrics.native_renderable && metrics.right_to_left &&
                metrics.bidi_runs > 1u && alpha_count(metrics.width) > 0u,
            "mixed-direction name was not resolved into visual runs");
    require(memcmp(hebrew_pixels, pixels, sizeof(pixels)) != 0,
            "mixed-direction layout collapsed to the isolated RTL run");
    require(!gfx_character_text_measure(emoji, sizeof(emoji), WIDTH,
                                        HEIGHT, &metrics) &&
                metrics.fallback_reason ==
                    GFX_CHARACTER_TEXT_FALLBACK_MISSING_GLYPH,
            "uncovered emoji did not use the honest missing-glyph fallback");
    require(!gfx_character_text_measure(bidi_override, sizeof(bidi_override),
                                        WIDTH, HEIGHT, &metrics) &&
                metrics.fallback_reason == GFX_CHARACTER_TEXT_FALLBACK_CONTROL,
            "invisible bidi override was accepted into an identity name");
    require(!gfx_character_text_measure(standalone_mark,
                                        sizeof(standalone_mark), WIDTH,
                                        HEIGHT, &metrics) &&
                metrics.fallback_reason ==
                    GFX_CHARACTER_TEXT_FALLBACK_INVISIBLE_SEQUENCE,
            "standalone combining mark was accepted as a visible name");
    require(!gfx_character_text_measure(nonbreaking_spaces,
                                        sizeof(nonbreaking_spaces), WIDTH,
                                        HEIGHT, &metrics) &&
                metrics.fallback_reason ==
                    GFX_CHARACTER_TEXT_FALLBACK_INVISIBLE_SEQUENCE,
            "non-breaking-space-only identity was accepted");
    require(!gfx_character_text_measure(stray_joiner, sizeof(stray_joiner),
                                        WIDTH, HEIGHT, &metrics) &&
                metrics.fallback_reason ==
                    GFX_CHARACTER_TEXT_FALLBACK_INVISIBLE_SEQUENCE,
            "context-free joiner was accepted into an identity name");
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
        static const char rtl_long[] =
            "\xD7\x93\xD7\x99\xD7\xA7\xD7\xA1\xD7\x99 \xD7\xA7\xD7\x95\xD7\xA0\xD7\x92";
        require(gfx_character_text_render_rgba(
                    rtl_long, sizeof(rtl_long), 30u, HEIGHT,
                    pixels, sizeof(pixels), WIDTH * 4u, &metrics) &&
                    metrics.truncated && metrics.right_to_left &&
                    metrics.width <= 30u && alpha_count(metrics.width) > 0u,
                "RTL compact name did not use bounded visual-side ellipsis");
    }
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

static void test_launcher_face_exports(void) {
    const char *latin = gfx_character_text_latin_face_base85();
    const char *arabic = gfx_character_text_arabic_face_base85();
    const char *hebrew = gfx_character_text_hebrew_face_base85();
    require(latin != NULL && latin[0] != '\0',
            "launcher Latin/Greek/Cyrillic face is unavailable");
    require(arabic != NULL && arabic[0] != '\0',
            "launcher Arabic face is unavailable");
    require(hebrew != NULL && hebrew[0] != '\0',
            "launcher Hebrew face is unavailable");
    require(latin != arabic && latin != hebrew && arabic != hebrew,
            "launcher script faces unexpectedly alias one another");
}

int main(void) {
    test_launcher_face_exports();
    test_direct_scripts();
    test_shaping_and_honest_fallbacks();
    test_fit_and_determinism();
    test_argument_bounds();
    gfx_character_text_shutdown();
    test_direct_scripts();
    gfx_character_text_shutdown();
    puts("character text tests passed");
    return 0;
}
