#include "modern_character_ktx2.h"

#include "basisu_transcoder.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace {

void set_error(char *error, size_t size, const char *message) {
    if (error != nullptr && size != 0u) {
        std::snprintf(error, size, "%s", message != nullptr ? message : "unknown error");
    }
}

bool checked_add(size_t left, size_t right, size_t *result) {
    if (result == nullptr || right > std::numeric_limits<size_t>::max() - left) return false;
    *result = left + right;
    return true;
}

bool initialize_transcoder() {
    static const bool initialized = []() {
        basist::basisu_transcoder_init();
        return true;
    }();
    return initialized;
}

bool inspect_transcoder(const uint8_t *data, size_t size,
                        basist::ktx2_transcoder *transcoder,
                        MdkrKtx2Info *info, char *error, size_t error_size) {
    if (data == nullptr || transcoder == nullptr || info == nullptr ||
        size == 0u || size > UINT32_MAX) {
        set_error(error, error_size, "KTX2 payload is empty or exceeds the 4 GiB container limit");
        return false;
    }
    if (!initialize_transcoder() || !transcoder->init(data, static_cast<uint32_t>(size))) {
        set_error(error, error_size, "KTX2 BasisU header or data-format descriptor is invalid");
        return false;
    }
    if (transcoder->get_faces() != 1u || transcoder->get_layers() != 0u ||
        transcoder->is_video()) {
        set_error(error, error_size, "character KTX2 must be one ordinary 2D image (no array, cube, or video)");
        return false;
    }
    const uint32_t width = transcoder->get_width();
    const uint32_t height = transcoder->get_height();
    const uint32_t levels = transcoder->get_levels();
    uint32_t maximum_levels = 1u;
    for (uint32_t dimension = width > height ? width : height;
         dimension > 1u; dimension /= 2u) {
        maximum_levels++;
    }
    if (width == 0u || height == 0u || width > MDKR_KTX2_DIMENSION_MAX ||
        height > MDKR_KTX2_DIMENSION_MAX || levels == 0u ||
        levels > MDKR_KTX2_LEVEL_MAX || levels > maximum_levels) {
        set_error(error, error_size, "character KTX2 dimensions or mip count exceed the bounded renderer profile");
        return false;
    }
    uint64_t rgba_bytes = 0u;
    uint32_t level_width = width;
    uint32_t level_height = height;
    for (uint32_t level = 0u; level < levels; ++level) {
        const uint64_t level_bytes =
            static_cast<uint64_t>(level_width) * level_height * 4u;
        if (level_bytes > MDKR_KTX2_DECODED_BYTES_MAX - rgba_bytes) {
            set_error(error, error_size, "decoded KTX2 mip chain exceeds the 512 MiB character budget");
            return false;
        }
        rgba_bytes += level_bytes;
        level_width = level_width > 1u ? level_width / 2u : 1u;
        level_height = level_height > 1u ? level_height / 2u : 1u;
    }
    info->width = width;
    info->height = height;
    info->levels = levels;
    info->has_alpha = transcoder->get_has_alpha() ? 1u : 0u;
    info->srgb = transcoder->get_dfd_transfer_func() ==
                         basist::KTX2_KHR_DF_TRANSFER_SRGB
                     ? 1u
                     : 0u;
    info->uastc = transcoder->is_uastc() ? 1u : 0u;
    info->rgba_bytes = rgba_bytes;
    set_error(error, error_size, "");
    return true;
}

bool target_details(MdkrKtx2TargetFormat format,
                    basist::transcoder_texture_format *basis_format,
                    uint32_t *bytes_per_unit, bool *compressed) {
    if (basis_format == nullptr || bytes_per_unit == nullptr || compressed == nullptr) return false;
    switch (format) {
    case MDKR_KTX2_TARGET_RGBA8:
        *basis_format = basist::transcoder_texture_format::cTFRGBA32;
        *bytes_per_unit = 4u;
        *compressed = false;
        return true;
    case MDKR_KTX2_TARGET_BC7:
        *basis_format = basist::transcoder_texture_format::cTFBC7_RGBA;
        *bytes_per_unit = 16u;
        *compressed = true;
        return true;
    case MDKR_KTX2_TARGET_ETC2_RGBA8:
        *basis_format = basist::transcoder_texture_format::cTFETC2_RGBA;
        *bytes_per_unit = 16u;
        *compressed = true;
        return true;
    case MDKR_KTX2_TARGET_ASTC_4X4:
        *basis_format = basist::transcoder_texture_format::cTFASTC_4x4_RGBA;
        *bytes_per_unit = 16u;
        *compressed = true;
        return true;
    }
    return false;
}

} // namespace

extern "C" int mdkr_ktx2_inspect(const uint8_t *data, size_t size,
                                  MdkrKtx2Info *info, char *error,
                                  size_t error_size) {
    if (info == nullptr) {
        set_error(error, error_size, "KTX2 inspection output is missing");
        return 0;
    }
    std::memset(info, 0, sizeof(*info));
    basist::ktx2_transcoder transcoder;
    return inspect_transcoder(data, size, &transcoder, info, error, error_size) ? 1 : 0;
}

extern "C" int mdkr_ktx2_transcode(const uint8_t *data, size_t size,
                                    MdkrKtx2TargetFormat format,
                                    MdkrKtx2Image *image, char *error,
                                    size_t error_size) {
    if (image == nullptr) {
        set_error(error, error_size, "KTX2 transcode output is missing");
        return 0;
    }
    std::memset(image, 0, sizeof(*image));
    basist::transcoder_texture_format basis_format;
    uint32_t bytes_per_unit = 0u;
    bool compressed = false;
    if (!target_details(format, &basis_format, &bytes_per_unit, &compressed)) {
        set_error(error, error_size, "requested KTX2 target format is unsupported");
        return 0;
    }
    basist::ktx2_transcoder transcoder;
    MdkrKtx2Info info{};
    if (!inspect_transcoder(data, size, &transcoder, &info, error, error_size)) return 0;
    if (!transcoder.start_transcoding()) {
        set_error(error, error_size, "KTX2 BasisU codebooks or supercompression stream are invalid");
        return 0;
    }

    size_t allocation_size = 0u;
    uint32_t width = info.width;
    uint32_t height = info.height;
    for (uint32_t level = 0u; level < info.levels; ++level) {
        const uint32_t columns = compressed ? (width + 3u) / 4u : width;
        const uint32_t rows = compressed ? (height + 3u) / 4u : height;
        const uint64_t level_size64 =
            static_cast<uint64_t>(columns) * rows * bytes_per_unit;
        if (level_size64 > MDKR_KTX2_DECODED_BYTES_MAX ||
            !checked_add(allocation_size, static_cast<size_t>(level_size64),
                         &allocation_size) ||
            allocation_size > MDKR_KTX2_DECODED_BYTES_MAX) {
            set_error(error, error_size, "transcoded KTX2 exceeds the 512 MiB character budget");
            return 0;
        }
        width = width > 1u ? width / 2u : 1u;
        height = height > 1u ? height / 2u : 1u;
    }
    image->allocation = static_cast<uint8_t *>(std::malloc(allocation_size));
    if (image->allocation == nullptr) {
        set_error(error, error_size, "could not allocate bounded KTX2 transcode output");
        return 0;
    }
    image->allocation_size = allocation_size;
    image->level_count = info.levels;

    size_t offset = 0u;
    width = info.width;
    height = info.height;
    for (uint32_t level = 0u; level < info.levels; ++level) {
        const uint32_t columns = compressed ? (width + 3u) / 4u : width;
        const uint32_t rows = compressed ? (height + 3u) / 4u : height;
        const uint32_t units = columns * rows;
        const size_t level_size = static_cast<size_t>(units) * bytes_per_unit;
        MdkrKtx2Level *destination = &image->levels[level];
        destination->data = image->allocation + offset;
        destination->size = level_size;
        destination->width = width;
        destination->height = height;
        destination->bytes_per_row = columns * bytes_per_unit;
        destination->rows = rows;
        const uint32_t output_units = compressed ? units : width * height;
        if (!transcoder.transcode_image_level(
                level, 0u, 0u, image->allocation + offset, output_units,
                basis_format)) {
            mdkr_ktx2_image_release(image);
            set_error(error, error_size, "KTX2 BasisU mip transcode failed integrity checks");
            return 0;
        }
        offset += level_size;
        width = width > 1u ? width / 2u : 1u;
        height = height > 1u ? height / 2u : 1u;
    }
    set_error(error, error_size, "");
    return 1;
}

extern "C" void mdkr_ktx2_image_release(MdkrKtx2Image *image) {
    if (image == nullptr) return;
    std::free(image->allocation);
    std::memset(image, 0, sizeof(*image));
}

extern "C" const char *mdkr_ktx2_target_name(MdkrKtx2TargetFormat format) {
    switch (format) {
    case MDKR_KTX2_TARGET_RGBA8: return "rgba8-fallback";
    case MDKR_KTX2_TARGET_BC7: return "bc7";
    case MDKR_KTX2_TARGET_ETC2_RGBA8: return "etc2-rgba8";
    case MDKR_KTX2_TARGET_ASTC_4X4: return "astc-4x4";
    }
    return "unsupported";
}
