#include "modern_character_ktx2.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace {

std::vector<uint8_t> decode_base64(const char *source) {
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::vector<uint8_t> output;
    uint32_t accumulator = 0u;
    unsigned bits = 0u;
    for (const unsigned char byte : std::string(source)) {
        if (byte == '=') break;
        const char *position = std::strchr(alphabet, byte);
        assert(position != nullptr);
        accumulator = (accumulator << 6u) |
            static_cast<uint32_t>(position - alphabet);
        bits += 6u;
        if (bits >= 8u) {
            bits -= 8u;
            output.push_back(
                static_cast<uint8_t>((accumulator >> bits) & 0xFFu));
        }
    }
    return output;
}

const char *kEtc1sSrgb =
    "q0tUWCAyMLsNChoKAAAAAAEAAAAIAAAACAAAAAAAAAAAAAAAAQAAAAQAAAABAAAAsAAAACwAAADcAAAAbAAAAEgBAAAAAAAA4QAAAAAAAAAsAgAAAAAAAAMAAAAAAAAAAAAAAAAAAAArAgAAAAAAAAEAAAAAAAAAAAAAAAAAAAAqAgAAAAAAAAEAAAAAAAAAAAAAAAAAAAApAgAAAAAAAAEAAAAAAAAAAAAAAAAAAAAsAAAAAAAAAAIAKACjAQIAAwMAAAgAAAAAAAAAAAA/AAAAAAAAAAAA/////xIAAABLVFhvcmllbnRhdGlvbgByZAAAACcAAABLVFh3cml0ZXIAdG9rdHggdjQuNC4yIC8gbGlia3R4IHY0LjQuMgAAIQAAAEtUWHdyaXRlclNjUGFyYW1zAC0tZW5jb2RlIGV0YzFzAAAAAAcABwA0AAAAHQAAACwAAAAAAAAAAAAAAAAAAAADAAAAAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAABwAQAAAAAAAACBHgYMAAAEEiaJ1AFABMAAAAAAACIAyACgAAAACDELSqnEwcEdjkkcr4fVPv7MTD4eFPj6UtNgyRHR83N5ydVVVVV9aeiogIAwUQAAAAAAADy3yuAARABQAAAABCyBgGIgCAAAAAIojUAmAAAAAAAAEAAARs9bxZRAA==";

const char *kUastcLinearZstd =
    "q0tUWCAyMLsNChoKAAAAAAEAAAAIAAAACAAAAAAAAAAAAAAAAQAAAAQAAAACAAAAsAAAACwAAADcAAAAdAAAAAAAAAAAAAAAAAAAAAAAAACbAQAAAAAAAEkAAAAAAAAAQAAAAAAAAACCAQAAAAAAABkAAAAAAAAAEAAAAAAAAABpAQAAAAAAABkAAAAAAAAAEAAAAAAAAABQAQAAAAAAABkAAAAAAAAAEAAAAAAAAAAsAAAAAAAAAAIAKACmAQEAAwMAABAAAAAAAAAAAAB/AAAAAAAAAAAA/////xIAAABLVFhvcmllbnRhdGlvbgByZAAAACcAAABLVFh3cml0ZXIAdG9rdHggdjQuNC4yIC8gbGlia3R4IHY0LjQuMgAAKgAAAEtUWHdyaXRlclNjUGFyYW1zAC0tZW5jb2RlIHVhc3RjIC0temNtcCAzAAAAKLUv/SAQgQAAV4or8D9O6gEAAAAAAAAAACi1L/0gEIEAAPuDpgz67cf8191vZm9mb2YotS/9IBCBAACTO5/XKJMBgj3GvQz/8ygAKLUv/SBAAQIAW1rlcw4CemCJmMh2nDPcIltKYGcAgjqBTMhnhLtF7wETJgDakLsFIMLhRJGwqlMAW2YkyjVBZqaQ/pX0ALW2ew==";

void expect_transcode(const std::vector<uint8_t> &bytes,
                      MdkrKtx2TargetFormat target, size_t expected_size) {
    MdkrKtx2Image image{};
    char error[192];
    assert(mdkr_ktx2_transcode(
        bytes.data(), bytes.size(), target, &image, error, sizeof(error)) == 1);
    assert(error[0] == '\0');
    assert(image.allocation != nullptr);
    assert(image.allocation_size == expected_size);
    assert(image.level_count == 4u);
    assert(image.levels[0].width == 8u && image.levels[0].height == 8u);
    assert(image.levels[3].width == 1u && image.levels[3].height == 1u);
    mdkr_ktx2_image_release(&image);
    assert(image.allocation == nullptr && image.level_count == 0u);
}

/* The hostile arms below address KTX2 header fields by their byte offsets:
 * the 80-byte header holds the data-format, key/value and supercompression
 * index pairs, and the level index behind it holds 24 bytes per level. */
void patch_le(std::vector<uint8_t> &bytes, size_t offset, size_t width,
              uint64_t value) {
    for (size_t byte = 0u; byte < width; ++byte) {
        bytes[offset + byte] =
            static_cast<uint8_t>((value >> (8u * byte)) & 0xFFu);
    }
}

/* Both bridge entry points must refuse a file whose header index describes
 * work the payload cannot back, and must name the bound that refused it. */
void expect_refused(const std::vector<uint8_t> &bytes, const char *reason) {
    MdkrKtx2Info info{};
    MdkrKtx2Image image{};
    char error[192];
    assert(mdkr_ktx2_inspect(
        bytes.data(), bytes.size(), &info, error, sizeof(error)) == 0);
    assert(std::strstr(error, reason) != nullptr);
    assert(mdkr_ktx2_transcode(bytes.data(), bytes.size(),
                               MDKR_KTX2_TARGET_RGBA8, &image, error,
                               sizeof(error)) == 0);
    assert(image.allocation == nullptr && image.level_count == 0u);
}

} // namespace

int main() {
    const std::vector<uint8_t> etc1s = decode_base64(kEtc1sSrgb);
    const std::vector<uint8_t> uastc = decode_base64(kUastcLinearZstd);
    MdkrKtx2Info info{};
    char error[192];

    assert(mdkr_ktx2_inspect(
        nullptr, 0u, &info, error, sizeof(error)) == 0);
    assert(std::strstr(error, "empty") != nullptr);

    assert(mdkr_ktx2_inspect(
        etc1s.data(), etc1s.size(), &info, error, sizeof(error)) == 1);
    assert(info.width == 8u && info.height == 8u && info.levels == 4u);
    assert(info.srgb == 1u && info.uastc == 0u && info.rgba_bytes == 340u);
    expect_transcode(etc1s, MDKR_KTX2_TARGET_RGBA8, 340u);
    expect_transcode(etc1s, MDKR_KTX2_TARGET_BC7, 112u);
    expect_transcode(etc1s, MDKR_KTX2_TARGET_ETC2_RGBA8, 112u);
    expect_transcode(etc1s, MDKR_KTX2_TARGET_ASTC_4X4, 112u);

    assert(mdkr_ktx2_inspect(
        uastc.data(), uastc.size(), &info, error, sizeof(error)) == 1);
    assert(info.srgb == 0u && info.uastc == 1u && info.rgba_bytes == 340u);
    expect_transcode(uastc, MDKR_KTX2_TARGET_RGBA8, 340u);
    expect_transcode(uastc, MDKR_KTX2_TARGET_BC7, 112u);

    /* Each pair below is bounded by the transcoder as offset + length against
     * the file size, and each sum wraps. The key/value case is the minimised
     * libFuzzer out-of-memory reproducer, also seeded as
     * wrapping-key-value-length.ktx2: 0xDC + 0xFFFFFF74 wraps to 80, and the
     * key/value walk then read past the end of the file and sized a 4 GiB
     * value allocation from what it found out there. */
    const char *outside = "index describes a region outside the payload";
    std::vector<uint8_t> hostile = uastc;
    patch_le(hostile, 60u, 4u, 0xFFFFFF74ull);             /* kvdByteLength */
    expect_refused(hostile, outside);

    hostile = uastc;
    patch_le(hostile, 48u, 4u, 0xFFFFFFF0ull);             /* dfdByteOffset */
    expect_refused(hostile, outside);

    hostile = uastc;
    patch_le(hostile, 64u, 8u, 0xFFFFFFFFFFFFFFF0ull);     /* sgdByteOffset */
    patch_le(hostile, 72u, 8u, 0x20ull);                   /* sgdByteLength */
    expect_refused(hostile, outside);

    hostile = uastc;
    patch_le(hostile, 80u, 8u, 0xFFFFFFFFFFFFFFF0ull); /* level 0 byteOffset */
    expect_refused(hostile, outside);

    /* A level's declared uncompressed size is the size of the buffer the
     * Zstandard path decompresses into. The 8x8 level 0 spans four blocks of
     * 16 bytes; the transcoder would have allocated the 688 MiB below. */
    hostile = uastc;
    patch_le(hostile, 96u, 8u, 721420304ull); /* level 0 uncompressed length */
    expect_refused(hostile,
                   "declares more supercompressed output than its mip spans");

    std::vector<uint8_t> truncated(etc1s.begin(), etc1s.end() - 1);
    assert(mdkr_ktx2_inspect(
        truncated.data(), truncated.size(), &info, error, sizeof(error)) == 0);
    return 0;
}
