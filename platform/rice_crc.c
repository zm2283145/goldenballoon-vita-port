// rice_crc.c — see rice_crc.h.
#include "rice_crc.h"

int mdkr_rice_crc32(const uint8_t *texels, size_t length, int width,
                    int height, int siz, int pitch, uint32_t *out_key) {
    if (texels == NULL || out_key == NULL) return 0;
    if (siz < 0 || siz > 3) return 0;              /* undefined RDP size code */
    if (width <= 0 || height <= 0) return 0;

    /* Four bits per pixel at siz=0, doubling per code; the +1 rounds a 4-bit
     * row up to whole bytes exactly as the reference does. */
    const long bytes_per_line = (((long)width << siz) + 1) / 2;
    if (bytes_per_line < 4) return 0;   /* the accumulator reads 4 at a time */

    const long row_pitch = (pitch > 0) ? (long)pitch : bytes_per_line;
    if (row_pitch < bytes_per_line) return 0;

    const long needed = (long)(height - 1) * row_pitch + bytes_per_line;
    if (needed < 0 || (size_t)needed > length) return 0;

    uint32_t crc = 0;
    long start = 0;
    for (int y = height - 1; y >= 0; --y) {
        uint32_t esi = 0;
        for (long x = bytes_per_line - 4; x >= 0; x -= 4) {
            const uint8_t *p = texels + start + x;
            /* Byte-swapped (big-endian) DWORD: the emulator holds RDRAM
             * swapped relative to the cartridge, so this is the dword Rice
             * hashed. Read byte-by-byte rather than casting, so the result does
             * not depend on this host's alignment rules or endianness. */
            esi = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
                  ((uint32_t)p[2] << 8) | (uint32_t)p[3];
            esi ^= (uint32_t)x;
            crc = (crc << 4) + ((crc >> 28) & 15u);   /* ROL 4 */
            crc += esi;
        }
        /* esi deliberately survives the x loop: the row term xors y into the
         * LAST dword read, not into a fresh zero. */
        esi ^= (uint32_t)y;
        crc += esi;
        start += row_pitch;
    }
    *out_key = crc;
    return 1;
}
