// ui_hero.cpp — decode the brand art once, pack it into the font atlas, draw it.
// See ui_hero.h.
#include "ui_hero.h"

#include "app_theme.h"
#include "ui_common.h"

#include "imgui_internal.h"   // ImFontAtlasTextureBlockQueueUpload

#include <algorithm>
#include <cstring>
#include <vector>

// stb_image is instantiated exactly once, in lib/stb/stb_image_impl.c, which is
// compiled with warnings off. This TU takes the declarations only and must
// configure the header identically to every other caller or the two would
// disagree about which entry points exist. STBI_ONLY_PNG is deliberate: every
// other decoder in that file is attack surface for a format nothing here reads.
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "stb_image.h"

// The byte array is generated into the build tree by tools/gen_image_header.py
// so a multi-megabyte blob never enters git.
#include "hero_png.h"

namespace ui {
namespace {

/*
 * The art goes into the FONT ATLAS as a custom rectangle rather than into a
 * texture of its own. ImTextureData says of itself that it "is only useful for
 * (1) core library and (2) backends" -- registering one from application code
 * produced a draw command referring to a texture no backend had uploaded, and
 * asserted on the first frame under both WebGPU and GL. The atlas is the
 * supported application path, and it is already created, uploaded and destroyed
 * by whichever backend is running, so this file needs no renderer knowledge.
 *
 * The cost is that the atlas owns the packing: it may be rebuilt at any time
 * (a UI-scale change or a move between displays with different framebuffer
 * scales both do it), which invalidates the rectangle AND its UVs. That is what
 * AppTheme::atlasGeneration() is for -- when it moves, the art is re-packed and
 * re-blitted before the next draw.
 */
struct Hero {
    std::vector<unsigned char> rgba;
    int  width = 0;
    int  height = 0;
    bool decoded = false;
    bool decodeAttempted = false;
    ImFontAtlasRectId rect = ImFontAtlasRectId_Invalid;
    unsigned packedGeneration = 0;
    bool packed = false;
};

Hero &hero() {
    static Hero instance;
    return instance;
}

void ensureDecoded() {
    Hero &state = hero();
    if (state.decodeAttempted) return;
    state.decodeAttempted = true;

    int channels = 0;
    stbi_uc *pixels = stbi_load_from_memory(kAppHeroPng_data,
                                            static_cast<int>(kAppHeroPng_size),
                                            &state.width, &state.height,
                                            &channels, 4);
    if (pixels == nullptr || state.width <= 0 || state.height <= 0) {
        if (pixels != nullptr) stbi_image_free(pixels);
        state.width = state.height = 0;
        return;
    }
    const size_t bytes = static_cast<size_t>(state.width) * state.height * 4u;
    state.rgba.assign(pixels, pixels + bytes);
    stbi_image_free(pixels);
    state.decoded = true;
}

// Pack (or re-pack) into the current atlas. Returns false when the art cannot
// be shown, which is a normal outcome and never an error the caller reports.
bool ensurePacked() {
    ensureDecoded();
    Hero &state = hero();
    if (!state.decoded) return false;

    ImFontAtlas *atlas = ImGui::GetIO().Fonts;
    if (atlas == nullptr || atlas->TexData == nullptr) return false;
    if (state.packed && state.packedGeneration == AppTheme::atlasGeneration()) {
        return true;
    }

    // A rebuilt atlas has already discarded the old rectangle; asking it to
    // forget one it never had is what RemoveCustomRect refuses, so only the
    // still-valid case is unregistered.
    state.packed = false;
    state.rect = atlas->AddCustomRect(state.width, state.height);
    if (state.rect == ImFontAtlasRectId_Invalid) return false;

    ImFontAtlasRect placed;
    if (!atlas->GetCustomRect(state.rect, &placed)) return false;

    ImTextureData *texture = atlas->TexData;
    if (texture->Format != ImTextureFormat_RGBA32) return false;

    // Colour pixels in the atlas: some backends choose a texture format from
    // this, and an atlas that believes it is alpha-only would drop the art.
    atlas->TexPixelsUseColors = true;
    for (int row = 0; row < state.height; ++row) {
        std::memcpy(texture->GetPixelsAt(placed.x, placed.y + row),
                    state.rgba.data() +
                        static_cast<size_t>(row) * state.width * 4u,
                    static_cast<size_t>(state.width) * 4u);
    }
    ImFontAtlasTextureBlockQueueUpload(atlas, texture, placed.x, placed.y,
                                       placed.w, placed.h);

    state.packedGeneration = AppTheme::atlasGeneration();
    state.packed = true;
    return true;
}

}  // namespace

bool HeroAvailable() { return ensurePacked(); }

bool HeroBanner(float maxHeight) {
    if (!ensurePacked()) return false;
    Hero &state = hero();

    ImFontAtlas *atlas = ImGui::GetIO().Fonts;
    ImFontAtlasRect placed;
    // Re-read every frame: the rectangle and its UVs belong to the CURRENT
    // atlas texture, and both change together when it is resized.
    if (!atlas->GetCustomRect(state.rect, &placed)) return false;

    const float available = ImGui::GetContentRegionAvail().x;
    if (available <= 1.0f) return false;

    const float aspect = static_cast<float>(state.height) /
                         static_cast<float>(state.width);
    const float ceiling = maxHeight * AppTheme::uiScale();
    const float height = (std::min)(available * aspect, ceiling);
    const float width = height / aspect;

    // Centred: the wordmark is the art's own subject, so an off-centre band
    // reads as a mistake rather than as a layout.
    const float inset = (available - width) * 0.5f;
    if (inset > 0.0f) ImGui::Indent(inset);
    ImGui::Image(atlas->TexRef, ImVec2(width, height), placed.uv0, placed.uv1);
    if (inset > 0.0f) ImGui::Unindent(inset);
    return true;
}

}  // namespace ui
