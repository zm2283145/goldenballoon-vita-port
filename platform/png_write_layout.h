/* First-party admission for packed RGB/RGBA passed to the pinned stb writer. */
#ifndef MDKR_PNG_WRITE_LAYOUT_H
#define MDKR_PNG_WRITE_LAYOUT_H

#include <limits.h>
#include <stddef.h>
#include <stdint.h>

typedef struct MdkrPngWriteLayout {
    size_t row_bytes;
    size_t pixel_bytes;
} MdkrPngWriteLayout;

/* No pixels are read or allocated here. The caller still owns a packed buffer
 * of pixel_bytes bytes. These are representation bounds, not a resolution
 * preference: supported ordinary captures, including 16K RGBA, remain valid.
 * Re-review this contract whenever the pinned encoder implementation changes. */
static inline int mdkr_png_write_layout(int width, int height, int components,
                                        MdkrPngWriteLayout *out) {
    uint64_t row, filtered, encoded_bound, capacity;
    if (out == NULL) return 0;
    out->row_bytes = 0;
    out->pixel_bytes = 0;
    if (width <= 0 || height <= 0 || (components != 3 && components != 4))
        return 0;
    row = (uint64_t)width * (unsigned)components;
    /* PNG's row filter accumulates absolute signed-byte values in an int. */
    if (row > (uint64_t)INT_MAX / 128u) return 0;
    filtered = (row + 1u) * (uint64_t)height;
    if (filtered > INT_MAX || row * (uint64_t)height > SIZE_MAX) return 0;

    /* The built-in compressor's fixed codes use at most nine bits per input
     * byte, plus block framing, padding and the zlib wrapper. Its output
     * buffer grows by capacity = 2*capacity+1, starting at two; require the
     * bound to fit below the largest representable capacity (the growth check
     * reserves one slot). This also bounds PNG chunk/output length arithmetic. */
    encoded_bound = (9u * filtered + 17u) / 8u + 6u;
    capacity = 2u;
    while (capacity <= ((uint64_t)INT_MAX - 1u) / 2u)
        capacity = 2u * capacity + 1u;
    if (encoded_bound >= capacity || encoded_bound > (uint64_t)INT_MAX - 57u)
        return 0;
    out->row_bytes = (size_t)row;
    out->pixel_bytes = (size_t)(row * (uint64_t)height);
    return 1;
}

#endif
