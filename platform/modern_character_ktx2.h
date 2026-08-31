/* Bounded C bridge around the pinned Basis Universal KTX2 transcoder. */
#ifndef MDKR64_MODERN_CHARACTER_KTX2_H
#define MDKR64_MODERN_CHARACTER_KTX2_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MDKR_KTX2_LEVEL_MAX 13u
#define MDKR_KTX2_DIMENSION_MAX 4096u
#define MDKR_KTX2_DECODED_BYTES_MAX (512u * 1024u * 1024u)

typedef enum MdkrKtx2TargetFormat {
    MDKR_KTX2_TARGET_RGBA8 = 0,
    MDKR_KTX2_TARGET_BC7 = 1,
    MDKR_KTX2_TARGET_ETC2_RGBA8 = 2,
    MDKR_KTX2_TARGET_ASTC_4X4 = 3,
} MdkrKtx2TargetFormat;

typedef struct MdkrKtx2Info {
    uint32_t width;
    uint32_t height;
    uint32_t levels;
    uint32_t has_alpha;
    uint32_t srgb;
    uint32_t uastc;
    uint64_t rgba_bytes;
} MdkrKtx2Info;

typedef struct MdkrKtx2Level {
    const uint8_t *data;
    size_t size;
    uint32_t width;
    uint32_t height;
    uint32_t bytes_per_row;
    uint32_t rows;
} MdkrKtx2Level;

typedef struct MdkrKtx2Image {
    uint8_t *allocation;
    size_t allocation_size;
    uint32_t level_count;
    MdkrKtx2Level levels[MDKR_KTX2_LEVEL_MAX];
} MdkrKtx2Image;

int mdkr_ktx2_inspect(const uint8_t *data, size_t size, MdkrKtx2Info *info,
                      char *error, size_t error_size);
int mdkr_ktx2_transcode(const uint8_t *data, size_t size,
                        MdkrKtx2TargetFormat format, MdkrKtx2Image *image,
                        char *error, size_t error_size);
void mdkr_ktx2_image_release(MdkrKtx2Image *image);
const char *mdkr_ktx2_target_name(MdkrKtx2TargetFormat format);

#ifdef __cplusplus
}
#endif

#endif /* MDKR64_MODERN_CHARACTER_KTX2_H */
