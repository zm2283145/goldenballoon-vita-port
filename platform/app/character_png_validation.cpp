#include "character_png_validation.h"

#include <array>
#include <cstring>

namespace {

constexpr size_t kSignatureBytes = 8u;
constexpr unsigned char kSignature[kSignatureBytes] = {
    0x89u, 'P', 'N', 'G', '\r', '\n', 0x1Au, '\n',
};

uint32_t bigEndian32(const unsigned char *bytes) {
    return (static_cast<uint32_t>(bytes[0]) << 24u) |
           (static_cast<uint32_t>(bytes[1]) << 16u) |
           (static_cast<uint32_t>(bytes[2]) << 8u) |
           static_cast<uint32_t>(bytes[3]);
}

const std::array<uint32_t, 256> &crcTable() {
    static const std::array<uint32_t, 256> table = [] {
        std::array<uint32_t, 256> generated{};
        for (uint32_t value = 0u; value < generated.size(); ++value) {
            uint32_t crc = value;
            for (unsigned bit = 0u; bit < 8u; ++bit) {
                crc = (crc >> 1u) ^
                    ((crc & 1u) != 0u ? 0xEDB88320u : 0u);
            }
            generated[value] = crc;
        }
        return generated;
    }();
    return table;
}

uint32_t crc32(const unsigned char *bytes, size_t size) {
    uint32_t crc = 0xFFFFFFFFu;
    const auto &table = crcTable();
    for (size_t index = 0u; index < size; ++index) {
        crc = table[(crc ^ bytes[index]) & 0xFFu] ^ (crc >> 8u);
    }
    return crc ^ 0xFFFFFFFFu;
}

bool chunkNameValid(const unsigned char *name) {
    for (unsigned index = 0u; index < 4u; ++index) {
        if (!((name[index] >= 'A' && name[index] <= 'Z') ||
              (name[index] >= 'a' && name[index] <= 'z'))) return false;
    }
    /* PNG reserves the third letter's lowercase bit. */
    return (name[2] & 0x20u) == 0u;
}

bool bitDepthValid(unsigned depth, unsigned colourType) {
    switch (colourType) {
        case 0u: return depth == 1u || depth == 2u || depth == 4u ||
                         depth == 8u || depth == 16u;
        case 2u: return depth == 8u || depth == 16u;
        case 3u: return depth == 1u || depth == 2u || depth == 4u ||
                         depth == 8u;
        case 4u:
        case 6u: return depth == 8u || depth == 16u;
        default: return false;
    }
}

} // namespace

namespace CharacterPngValidation {

bool validate(const unsigned char *bytes, size_t size,
              uint32_t expectedWidth, uint32_t expectedHeight,
              Info &info, std::string &error) {
    info = Info{};
    if (bytes == nullptr || size < 57u ||
        std::memcmp(bytes, kSignature, sizeof(kSignature)) != 0) {
        error = "The capture is not a complete PNG.";
        return false;
    }
    size_t offset = kSignatureBytes;
    bool sawHeader = false;
    bool sawPalette = false;
    bool sawData = false;
    bool endedData = false;
    size_t dataBytes = 0u;
    unsigned colourType = 0u;
    while (offset < size) {
        if (size - offset < 12u) {
            error = "The capture has a truncated PNG chunk.";
            return false;
        }
        const uint32_t length = bigEndian32(bytes + offset);
        if (length > size - offset - 12u) {
            error = "The capture has an oversized PNG chunk.";
            return false;
        }
        const unsigned char *name = bytes + offset + 4u;
        const unsigned char *payload = bytes + offset + 8u;
        const size_t end = offset + 12u + length;
        if (!chunkNameValid(name) ||
            crc32(name, static_cast<size_t>(length) + 4u) !=
                bigEndian32(payload + length)) {
            error = "The capture has an invalid PNG chunk name or CRC.";
            return false;
        }
        const bool isHeader = std::memcmp(name, "IHDR", 4u) == 0;
        const bool isPalette = std::memcmp(name, "PLTE", 4u) == 0;
        const bool isData = std::memcmp(name, "IDAT", 4u) == 0;
        const bool isEnd = std::memcmp(name, "IEND", 4u) == 0;
        if (!sawHeader && !isHeader) {
            error = "The capture PNG does not begin with IHDR.";
            return false;
        }
        if (isHeader) {
            if (sawHeader || offset != kSignatureBytes || length != 13u) {
                error = "The capture has a duplicate or malformed IHDR.";
                return false;
            }
            info.width = bigEndian32(payload);
            info.height = bigEndian32(payload + 4u);
            const unsigned depth = payload[8];
            colourType = payload[9];
            if (info.width == 0u || info.height == 0u ||
                info.width > 16384u || info.height > 16384u ||
                !bitDepthValid(depth, colourType) || payload[10] != 0u ||
                payload[11] != 0u || payload[12] != 0u ||
                (expectedWidth != 0u && info.width != expectedWidth) ||
                (expectedHeight != 0u && info.height != expectedHeight)) {
                error = "The capture PNG has incompatible image metadata.";
                return false;
            }
            info.bitDepth = static_cast<uint8_t>(depth);
            info.colourType = static_cast<uint8_t>(colourType);
            sawHeader = true;
        } else if (isPalette) {
            if (sawPalette || sawData || length == 0u || length > 768u ||
                length % 3u != 0u || colourType == 0u || colourType == 4u) {
                error = "The capture PNG has an invalid palette.";
                return false;
            }
            sawPalette = true;
        } else if (isData) {
            if (endedData || (colourType == 3u && !sawPalette)) {
                error = "The capture PNG has out-of-order image data.";
                return false;
            }
            sawData = true;
            if (dataBytes > size - length) {
                error = "The capture PNG data size overflowed.";
                return false;
            }
            dataBytes += length;
        } else if (isEnd) {
            if (length != 0u || !sawData || dataBytes == 0u || end != size) {
                error = "The capture PNG has an invalid terminator.";
                return false;
            }
            error.clear();
            return true;
        } else {
            if (std::memcmp(name, "acTL", 4u) == 0 ||
                std::memcmp(name, "fcTL", 4u) == 0 ||
                std::memcmp(name, "fdAT", 4u) == 0) {
                error = "Animated PNG is not supported for Workshop evidence.";
                return false;
            }
            if (sawData) endedData = true;
            /* Unknown uppercase-first chunks are critical and cannot be
             * interpreted safely by this evidence reader. */
            if ((name[0] & 0x20u) == 0u) {
                error = "The capture PNG uses an unknown critical chunk.";
                return false;
            }
        }
        if (sawData && !isData) endedData = true;
        offset = end;
    }
    error = "The capture PNG is missing its terminal IEND.";
    return false;
}

} // namespace CharacterPngValidation
