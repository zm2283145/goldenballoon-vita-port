/* Coverage-guided gate for the custom-character binary trust boundaries: the
 * exact MDKC memory loader, PNG-only stb_image build, and bounded BasisU KTX2
 * bridge. */
#include "modern_character_asset.h"
#include "modern_character_ktx2.h"

extern "C" {
#include "stb_image.h"
}

#include <cstddef>
#include <cstdint>
#include <climits>

namespace {

constexpr uint64_t kFuzzTranscodeBudget = 16u * 1024u * 1024u;

void exercisePng(const uint8_t *data, size_t size) {
    int width = 0;
    int height = 0;
    int components = 0;
    if (data == nullptr || size > static_cast<size_t>(INT_MAX) ||
        stbi_info_from_memory(data, static_cast<int>(size), &width, &height,
                              &components) == 0 ||
        width <= 0 || height <= 0 ||
        static_cast<uint64_t>(width) * static_cast<uint64_t>(height) * 4u >
            kFuzzTranscodeBudget) {
        return;
    }
    unsigned char *rgba = stbi_load_from_memory(
        data, static_cast<int>(size), &width, &height, &components, 4);
    stbi_image_free(rgba);
}

void exerciseKtx2(const uint8_t *data, size_t size,
                  MdkrKtx2TargetFormat target) {
    MdkrKtx2Info info{};
    MdkrKtx2Image image{};
    char error[192];
    if (mdkr_ktx2_inspect(data, size, &info, error, sizeof(error)) != 0 &&
        info.rgba_bytes <= kFuzzTranscodeBudget) {
        (void)mdkr_ktx2_transcode(
            data, size, target, &image, error, sizeof(error));
    }
    mdkr_ktx2_image_release(&image);
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    const auto target = static_cast<MdkrKtx2TargetFormat>(size & 3u);
    MdkrModernCharacterAsset asset{};
    char error[192];

    /* Raw inputs reach the texture parser directly; valid MDKC inputs also
     * reach it through their authenticated texture table below. */
    exerciseKtx2(data, size, target);
    exercisePng(data, size);

    if (mdkr_modern_character_asset_load_memory(
            data, size, &asset, error, sizeof(error)) != 0) {
        MdkrModernCharacterStats stats{};
        MdkrModernIdentity identity{};
        const uint8_t *portrait = nullptr;
        const MdkrModernSectionView *payload =
            mdkr_modern_character_asset_section(
                &asset, MDKR_MDKC_TEXTURE_DATA);
        mdkr_modern_character_asset_stats(&asset, &stats);
        if (mdkr_modern_character_asset_identity(
                &asset, &identity, &portrait) != 0 && portrait != nullptr) {
            exercisePng(portrait, identity.portrait_size);
        }
        /* Walk the animation graph the way the character registry does.
         *
         * This section was entirely unvisited, which is how a consumer came to
         * index a fixed 64-entry local with a count validate_references()
         * admits up to 256 -- a 192-byte stack overflow that an ordinary
         * 65-clip character triggers at every boot. The accessors below are the
         * ones that scan reaches, so exercising them over the full validated
         * range is what makes an out-of-bounds read in any of them reachable
         * from a corpus rather than only from play.
         *
         * NOT the whole class: the overflow itself lived in a local array
         * inside registry_init(), which is directory-driven and so is still
         * outside this harness. Covering that needs either a filesystem
         * fixture here or a seam that builds one entry from one asset.
         */
        for (uint32_t index = 0u; index < stats.animations; ++index) {
            MdkrModernAnimation animation{};
            if (!mdkr_modern_character_asset_animation(&asset, index,
                                                       &animation)) {
                continue;
            }
            for (uint32_t offset = 0u; offset < animation.channel_count;
                 ++offset) {
                MdkrModernChannel channel{};
                if (!mdkr_modern_character_asset_channel(
                        &asset, animation.first_channel + offset, &channel)) {
                    continue;
                }
                for (uint32_t key = 0u; key < channel.key_count; ++key) {
                    MdkrModernKey record{};
                    (void)mdkr_modern_character_asset_key(
                        &asset, channel.first_key + key, &record);
                }
            }
        }
        for (uint32_t index = 0u; index < stats.sockets; ++index) {
            MdkrModernSocket socket{};
            (void)mdkr_modern_character_asset_socket(&asset, index, &socket);
        }
        for (uint32_t index = 0u; index < stats.semantics; ++index) {
            MdkrModernSemantic semantic{};
            (void)mdkr_modern_character_asset_semantic(&asset, index,
                                                       &semantic);
        }
        for (uint32_t index = 0u; index < stats.textures; ++index) {
            MdkrModernTexture texture{};
            if (!mdkr_modern_character_asset_texture(
                    &asset, index, &texture) ||
                payload == nullptr || payload->data == nullptr ||
                static_cast<uint64_t>(texture.data_offset) > payload->size ||
                static_cast<uint64_t>(texture.data_size) >
                    payload->size - texture.data_offset) {
                continue;
            }
            const uint8_t *textureData =
                payload->data + texture.data_offset;
            if (texture.mime == 1u) {
                exercisePng(textureData, texture.data_size);
            } else if (texture.mime == 2u) {
                exerciseKtx2(textureData, texture.data_size, target);
            }
        }
    }
    mdkr_modern_character_asset_unload(&asset);
    return 0;
}
