#include "png_write_layout.h"

#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_WRITE_NO_STDIO
#include "stb_image.h"
#include "stb_image_write.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void expect(int condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        exit(1);
    }
}

static void test_layout_without_allocating_pixels(void) {
    MdkrPngWriteLayout layout;
    int low = 1, high = INT_MAX;
    const int widest_rgba = (INT_MAX / 128) / 4;
    expect(!mdkr_png_write_layout(1, 1, 4, NULL), "null result rejected");
    expect(!mdkr_png_write_layout(0, 1, 4, &layout), "empty image rejected");
    expect(!mdkr_png_write_layout(1, -1, 4, &layout), "negative extent rejected");
    expect(!mdkr_png_write_layout(1, 1, 2, &layout), "unsupported channels rejected");
    expect(mdkr_png_write_layout(3840, 2160, 3, &layout), "4K RGB retained");
    expect(layout.row_bytes == 11520u && layout.pixel_bytes == 24883200u,
           "packed RGB allocation and stride agree");
    expect(mdkr_png_write_layout(7680, 4320, 4, &layout), "8K RGBA retained");
    expect(mdkr_png_write_layout(16384, 16384, 4, &layout), "16K RGBA retained");
    expect(mdkr_png_write_layout(widest_rgba, 1, 4, &layout),
           "last representable filter-row width admitted");
    expect(!mdkr_png_write_layout(widest_rgba + 1, 1, 4, &layout),
           "filter-row accumulator bound enforced");
    expect(layout.row_bytes == 0 && layout.pixel_bytes == 0,
           "rejected layout cannot leave stale output sizes");
    expect(!mdkr_png_write_layout(INT_MAX, INT_MAX, 4, &layout),
           "oversized extents rejected before arithmetic narrowing");
    expect(!mdkr_png_write_layout(16384, INT_MAX, 4, &layout),
           "filtered-image total bound enforced");
    /* Find and check the output-capacity boundary using arithmetic only.
     * None of these large dimensions is passed to an allocator or encoder. */
    while (low < high) {
        const int mid = low + (high - low) / 2 + 1;
        if (mdkr_png_write_layout(4096, mid, 4, &layout)) low = mid;
        else high = mid - 1;
    }
    expect(mdkr_png_write_layout(4096, low, 4, &layout),
           "last output-capacity-safe layout admitted");
    expect(!mdkr_png_write_layout(4096, low + 1, 4, &layout),
           "next output-capacity layout rejected");
    expect((uint64_t)(4096 * 4 + 1) * (uint64_t)(low + 1) < INT_MAX,
           "output-growth bound is stronger than filtered-size bound");
}

typedef struct EncodedPng {
    unsigned char bytes[4096];
    int size;
} EncodedPng;

static void collect_png(void *context, void *data, int size) {
    EncodedPng *png = (EncodedPng *)context;
    expect(size > 0 && (size_t)size <= sizeof(png->bytes),
           "small roundtrip stays within its fixed output buffer");
    expect(png->size == 0, "pinned encoder emits one complete PNG");
    memcpy(png->bytes, data, (size_t)size);
    png->size = size;
}

static void test_small_roundtrip(int components) {
    unsigned char pixels[7 * 5 * 4];
    MdkrPngWriteLayout layout;
    EncodedPng png = {{0}, 0};
    int width, height, channels;
    unsigned char *decoded;
    size_t i;
    expect(mdkr_png_write_layout(7, 5, components, &layout), "small layout admitted");
    for (i = 0; i < layout.pixel_bytes; ++i) pixels[i] = (unsigned char)(i * 37u);
    expect(stbi_write_png_to_func(collect_png, &png, 7, 5, components, pixels,
                                 (int)layout.row_bytes), "valid image encodes");
    decoded = stbi_load_from_memory(png.bytes, png.size, &width, &height,
                                    &channels, components);
    expect(decoded != NULL && width == 7 && height == 5 && channels == components,
           "roundtrip dimensions and channels preserved");
    expect(memcmp(pixels, decoded, layout.pixel_bytes) == 0, "pixels preserved");
    stbi_image_free(decoded);
}

int main(void) {
    test_layout_without_allocating_pixels();
    test_small_roundtrip(3);
    test_small_roundtrip(4);
    puts("PNG writer layout: PASS");
    return 0;
}
