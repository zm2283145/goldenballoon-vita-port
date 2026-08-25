/* CPU ownership bridge from a validated MDKC asset to immutable renderer data. */
#ifndef MDKR64_MODERN_CHARACTER_RENDER_H
#define MDKR64_MODERN_CHARACTER_RENDER_H

#include "modern_character_asset.h"
#include "fast3d/gfx_rendering_api.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MdkrModernDecodedTexture {
    uint8_t *rgba;
    uint8_t *mip_scratch;
} MdkrModernDecodedTexture;

typedef struct MdkrModernRenderAsset {
    struct GfxModernSkinnedAsset gpu;
    struct GfxModernSkinnedVertex *vertices;
    uint32_t *indices;
    struct GfxModernPrimitive *primitives;
    struct GfxModernMaterial *materials;
    struct GfxModernTexture *textures;
    MdkrModernDecodedTexture *decoded;
    size_t decoded_texture_bytes;
    int valid;
} MdkrModernRenderAsset;

int mdkr_modern_render_asset_init(MdkrModernRenderAsset *render,
                                  const MdkrModernCharacterAsset *asset,
                                  char *error, size_t error_size);
void mdkr_modern_render_asset_shutdown(MdkrModernRenderAsset *render);

#ifdef __cplusplus
}
#endif

#endif /* MDKR64_MODERN_CHARACTER_RENDER_H */
