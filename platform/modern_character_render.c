#include "modern_character_render.h"

#include "fast3d/gfx_mipgen.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "stb_image.h"

#define MODERN_TEXTURE_DIMENSION_MAX 4096
#define MODERN_DECODED_TEXTURE_BYTES_MAX (512u * 1024u * 1024u)

static void set_error(char *error, size_t size, const char *message) {
    if (error != NULL && size != 0u) {
        (void)snprintf(error, size, "%s", message != NULL ? message : "unknown error");
    }
}

static uint32_t read_u32(const uint8_t *bytes) {
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static uint64_t digest_id(const uint8_t digest[32]) {
    uint64_t value = 0u;
    unsigned index;
    for (index = 0u; index < 8u; index++) value |= (uint64_t)digest[index] << (index * 8u);
    return value != 0u ? value : 1u;
}

static float exact_lerp(float previous, float current,
                        uint64_t numerator, uint64_t denominator) {
    double alpha;
    if (denominator == 0u || numerator == 0u) return previous;
    if (numerator >= denominator) return current;
    alpha = (double)numerator / (double)denominator;
    return (float)((double)previous +
                   ((double)current - (double)previous) * alpha);
}

static int normal_transform(const float input[16], float output[16]) {
    const float a00 = input[0], a01 = input[4], a02 = input[8];
    const float a10 = input[1], a11 = input[5], a12 = input[9];
    const float a20 = input[2], a21 = input[6], a22 = input[10];
    const float determinant = a00 * (a11 * a22 - a12 * a21) -
                              a01 * (a10 * a22 - a12 * a20) +
                              a02 * (a10 * a21 - a11 * a20);
    float inverse;
    if (!isfinite(determinant) || fabsf(determinant) < 1.0e-12f) return 0;
    inverse = 1.0f / determinant;
    memset(output, 0, sizeof(float) * 16u);
    output[0] = (a11 * a22 - a12 * a21) * inverse;
    output[1] = (a02 * a21 - a01 * a22) * inverse;
    output[2] = (a01 * a12 - a02 * a11) * inverse;
    output[4] = (a12 * a20 - a10 * a22) * inverse;
    output[5] = (a00 * a22 - a02 * a20) * inverse;
    output[6] = (a02 * a10 - a00 * a12) * inverse;
    output[8] = (a10 * a21 - a11 * a20) * inverse;
    output[9] = (a01 * a20 - a00 * a21) * inverse;
    output[10] = (a00 * a11 - a01 * a10) * inverse;
    output[15] = 1.0f;
    return 1;
}

int mdkr_modern_render_resolve_draw(
    const struct GfxModernSkinnedDraw *retained,
    uint64_t numerator, uint64_t denominator,
    struct GfxModernSkinnedDraw *resolved,
    float *palette_scratch, size_t palette_matrices) {
    uint32_t component;
    if (retained == NULL || resolved == NULL || retained->asset == NULL ||
        retained->primitive >= retained->asset->primitive_count ||
        (retained->bone_count != 0u &&
         (retained->bone_matrices == NULL ||
          retained->previous_bone_matrices == NULL ||
          palette_scratch == NULL ||
          palette_matrices < retained->bone_count))) {
        return 0;
    }
    *resolved = *retained;
    for (component = 0u; component < 16u; component++) {
        resolved->model_matrix[component] = exact_lerp(
            retained->previous_model_matrix[component],
            retained->model_matrix[component], numerator, denominator);
    }
    if (!normal_transform(resolved->model_matrix, resolved->normal_matrix)) {
        return 0;
    }
    for (component = 0u; component < retained->bone_count * 16u; component++) {
        palette_scratch[component] = exact_lerp(
            retained->previous_bone_matrices[component],
            retained->bone_matrices[component], numerator, denominator);
    }
    if (retained->bone_count != 0u) {
        resolved->bone_matrices = palette_scratch;
    }
    return 1;
}

static int allocate_arrays(MdkrModernRenderAsset *render,
                           const MdkrModernCharacterAsset *asset) {
    const MdkrModernSectionView *vertices = mdkr_modern_character_asset_section(asset, MDKR_MDKC_VERTICES);
    const MdkrModernSectionView *indices = mdkr_modern_character_asset_section(asset, MDKR_MDKC_INDICES);
    const MdkrModernSectionView *primitives = mdkr_modern_character_asset_section(asset, MDKR_MDKC_PRIMITIVES);
    const MdkrModernSectionView *materials = mdkr_modern_character_asset_section(asset, MDKR_MDKC_MATERIALS);
    const MdkrModernSectionView *textures = mdkr_modern_character_asset_section(asset, MDKR_MDKC_TEXTURES);
    render->vertices = (struct GfxModernSkinnedVertex *)calloc(vertices->count, sizeof(*render->vertices));
    render->indices = (uint32_t *)calloc(indices->count, sizeof(*render->indices));
    render->primitives = (struct GfxModernPrimitive *)calloc(primitives->count, sizeof(*render->primitives));
    render->materials = (struct GfxModernMaterial *)calloc(materials->count, sizeof(*render->materials));
    render->primitive_sort = (MdkrModernPrimitiveSortData *)calloc(
        primitives->count, sizeof(*render->primitive_sort));
    if (textures->count != 0u) {
        render->textures = (struct GfxModernTexture *)calloc(textures->count, sizeof(*render->textures));
        render->decoded = (MdkrModernDecodedTexture *)calloc(textures->count, sizeof(*render->decoded));
    }
    return render->vertices != NULL && render->indices != NULL &&
           render->primitives != NULL && render->materials != NULL &&
           render->primitive_sort != NULL &&
           (textures->count == 0u || (render->textures != NULL && render->decoded != NULL));
}

static int build_primitive_sort_data(
    MdkrModernRenderAsset *render, const MdkrModernCharacterAsset *asset,
    uint32_t primitive_count, char *error, size_t error_size) {
    size_t moment_count = 0u;
    uint32_t primitive_index;
    for (primitive_index = 0u; primitive_index < primitive_count;
         ++primitive_index) {
        MdkrModernPrimitive primitive;
        MdkrModernSkin skin;
        if (!mdkr_modern_character_asset_primitive(
                asset, primitive_index, &primitive)) return 0;
        if (primitive.skin >= 0) {
            if (!mdkr_modern_character_asset_skin(
                    asset, (uint32_t)primitive.skin, &skin) ||
                skin.joint_count > SIZE_MAX - moment_count) return 0;
            moment_count += skin.joint_count;
        }
    }
    if (moment_count > SIZE_MAX / (sizeof(float) * 4u)) {
        set_error(error, error_size,
                  "character transparent-sort moments exceed addressable memory");
        return 0;
    }
    if (moment_count != 0u) {
        render->sort_moments = (float *)calloc(
            moment_count * 4u, sizeof(*render->sort_moments));
        if (render->sort_moments == NULL) {
            set_error(error, error_size,
                      "could not allocate character transparent-sort moments");
            return 0;
        }
    }
    render->sort_moment_count = moment_count;
    moment_count = 0u;
    for (primitive_index = 0u; primitive_index < primitive_count;
         ++primitive_index) {
        MdkrModernPrimitive primitive;
        MdkrModernPrimitiveSortData *sort =
            &render->primitive_sort[primitive_index];
        uint32_t vertex_offset;
        if (!mdkr_modern_character_asset_primitive(
                asset, primitive_index, &primitive) ||
            primitive.vertex_count == 0u) return 0;
        sort->inverse_vertex_count = 1.0f / (float)primitive.vertex_count;
        if (primitive.skin >= 0) {
            MdkrModernSkin skin;
            if (!mdkr_modern_character_asset_skin(
                    asset, (uint32_t)primitive.skin, &skin) ||
                moment_count > UINT32_MAX ||
                skin.joint_count > UINT32_MAX - (uint32_t)moment_count) {
                return 0;
            }
            sort->first_moment = (uint32_t)moment_count;
            sort->moment_count = skin.joint_count;
            moment_count += skin.joint_count;
        }
        for (vertex_offset = 0u; vertex_offset < primitive.vertex_count;
             ++vertex_offset) {
            const uint32_t vertex_index =
                primitive.first_vertex + vertex_offset;
            const struct GfxModernSkinnedVertex *vertex =
                &render->vertices[vertex_index];
            if (sort->moment_count == 0u) {
                unsigned axis;
                for (axis = 0u; axis < 3u; ++axis) {
                    sort->rigid_center[axis] += vertex->position[axis];
                }
            } else {
                unsigned influence;
                for (influence = 0u; influence < 4u; ++influence) {
                    const float weight = vertex->weights[influence];
                    const uint32_t joint = vertex->joints[influence];
                    float *moment;
                    unsigned axis;
                    if (weight == 0.0f) continue;
                    if (joint >= sort->moment_count) return 0;
                    moment = &render->sort_moments[
                        (size_t)(sort->first_moment + joint) * 4u];
                    for (axis = 0u; axis < 3u; ++axis) {
                        moment[axis] += weight * vertex->position[axis];
                    }
                    moment[3] += weight;
                }
            }
        }
        if (sort->moment_count == 0u) {
            unsigned axis;
            for (axis = 0u; axis < 3u; ++axis) {
                sort->rigid_center[axis] *= sort->inverse_vertex_count;
                if (!isfinite(sort->rigid_center[axis])) return 0;
            }
        }
    }
    return 1;
}

int mdkr_modern_render_primitive_sort_center(
    const MdkrModernRenderAsset *render, uint32_t primitive,
    const float *bone_matrices, size_t bone_count, float output[3]) {
    const MdkrModernPrimitiveSortData *sort;
    unsigned axis;
    if (render == NULL || !render->valid || output == NULL ||
        primitive >= render->gpu.primitive_count ||
        render->primitive_sort == NULL) return 0;
    sort = &render->primitive_sort[primitive];
    if (sort->moment_count == 0u) {
        memcpy(output, sort->rigid_center, sizeof(sort->rigid_center));
        return isfinite(output[0]) && isfinite(output[1]) &&
            isfinite(output[2]);
    }
    if (bone_matrices == NULL || bone_count < sort->moment_count ||
        sort->first_moment > render->sort_moment_count ||
        sort->moment_count >
            render->sort_moment_count - sort->first_moment) return 0;
    for (axis = 0u; axis < 3u; ++axis) {
        double sum = 0.0;
        uint32_t joint;
        for (joint = 0u; joint < sort->moment_count; ++joint) {
            const float *moment = &render->sort_moments[
                (size_t)(sort->first_moment + joint) * 4u];
            const float *bone = &bone_matrices[(size_t)joint * 16u];
            sum += (double)bone[axis] * moment[0] +
                (double)bone[4u + axis] * moment[1] +
                (double)bone[8u + axis] * moment[2] +
                (double)bone[12u + axis] * moment[3];
        }
        output[axis] = (float)(sum * sort->inverse_vertex_count);
        if (!isfinite(output[axis])) return 0;
    }
    return 1;
}

int mdkr_modern_render_asset_init(MdkrModernRenderAsset *render,
                                  const MdkrModernCharacterAsset *asset,
                                  char *error, size_t error_size) {
    const MdkrModernSectionView *vertex_section;
    const MdkrModernSectionView *index_section;
    const MdkrModernSectionView *primitive_section;
    const MdkrModernSectionView *material_section;
    const MdkrModernSectionView *texture_section;
    const MdkrModernSectionView *texture_data;
    uint32_t index;
    if (render == NULL || asset == NULL || asset->owned_bytes == NULL) {
        set_error(error, error_size, "render asset requires a validated character");
        return 0;
    }
    memset(render, 0, sizeof(*render));
    vertex_section = mdkr_modern_character_asset_section(asset, MDKR_MDKC_VERTICES);
    index_section = mdkr_modern_character_asset_section(asset, MDKR_MDKC_INDICES);
    primitive_section = mdkr_modern_character_asset_section(asset, MDKR_MDKC_PRIMITIVES);
    material_section = mdkr_modern_character_asset_section(asset, MDKR_MDKC_MATERIALS);
    texture_section = mdkr_modern_character_asset_section(asset, MDKR_MDKC_TEXTURES);
    texture_data = mdkr_modern_character_asset_section(asset, MDKR_MDKC_TEXTURE_DATA);
    if (!allocate_arrays(render, asset)) {
        mdkr_modern_render_asset_shutdown(render);
        set_error(error, error_size, "could not allocate immutable character render data");
        return 0;
    }
    /* Establish cleanup ownership before the first decode can fail. */
    render->gpu.texture_count = texture_section->count;
    for (index = 0u; index < vertex_section->count; index++) {
        MdkrModernVertex source;
        struct GfxModernSkinnedVertex *destination = &render->vertices[index];
        (void)mdkr_modern_character_asset_vertex(asset, index, &source);
        memcpy(destination->position, source.position, sizeof(source.position));
        memcpy(destination->normal, source.normal, sizeof(source.normal));
        memcpy(destination->tangent, source.tangent, sizeof(source.tangent));
        memcpy(destination->uv, source.uv, sizeof(source.uv));
        memcpy(destination->joints, source.joints, sizeof(source.joints));
        memcpy(destination->weights, source.weights, sizeof(source.weights));
    }
    for (index = 0u; index < index_section->count; index++) {
        render->indices[index] = read_u32(index_section->data + (size_t)index * 4u);
    }
    for (index = 0u; index < primitive_section->count; index++) {
        MdkrModernPrimitive source;
        struct GfxModernPrimitive *destination = &render->primitives[index];
        (void)mdkr_modern_character_asset_primitive(asset, index, &source);
        destination->first_index = source.first_index;
        destination->index_count = source.index_count;
        destination->material = (uint32_t)source.material;
        destination->node = source.node;
        destination->skin = source.skin;
        destination->lod = source.lod;
    }
    for (index = 0u; index < material_section->count; index++) {
        MdkrModernMaterial source;
        struct GfxModernMaterial *destination = &render->materials[index];
        (void)mdkr_modern_character_asset_material(asset, index, &source);
        memcpy(destination->texture, source.textures, sizeof(source.textures));
        memcpy(destination->base_color, source.base_color, sizeof(source.base_color));
        memcpy(destination->emissive, source.emissive, sizeof(source.emissive));
        destination->metallic = source.metallic;
        destination->roughness = source.roughness;
        destination->normal_scale = source.normal_scale;
        destination->occlusion_strength = source.occlusion_strength;
        destination->alpha_cutoff = source.alpha_cutoff;
        destination->flags = source.flags;
    }
    set_error(error, error_size, "");
    if (!build_primitive_sort_data(
            render, asset, primitive_section->count, error, error_size)) {
        mdkr_modern_render_asset_shutdown(render);
        if (error == NULL || error_size == 0u || error[0] == '\0') {
            set_error(error, error_size,
                      "could not prepare character transparent ordering");
        }
        return 0;
    }
    for (index = 0u; index < texture_section->count; index++) {
        MdkrModernTexture source;
        struct GfxModernTexture *destination = &render->textures[index];
        GfxMipChain chain;
        size_t level_zero_bytes;
        size_t mip_bytes;
        int width;
        int height;
        int components;
        int cutout_threshold = -1;
        uint8_t *rgba;
        uint32_t material_index;
        (void)mdkr_modern_character_asset_texture(asset, index, &source);
        if (source.mime != 1u) {
            mdkr_modern_render_asset_shutdown(render);
            set_error(error, error_size, "KTX2 texture decode is unavailable in this build");
            return 0;
        }
        if (!stbi_info_from_memory(texture_data->data + source.data_offset,
                                   (int)source.data_size, &width, &height,
                                   &components) ||
            width <= 0 || height <= 0 ||
            width > MODERN_TEXTURE_DIMENSION_MAX ||
            height > MODERN_TEXTURE_DIMENSION_MAX) {
            mdkr_modern_render_asset_shutdown(render);
            set_error(error, error_size,
                      "character PNG failed bounded header inspection");
            return 0;
        }
        rgba = stbi_load_from_memory(texture_data->data + source.data_offset,
                                    (int)source.data_size, &width, &height,
                                    &components, 4);
        if (rgba == NULL || width <= 0 || height <= 0 ||
            width > MODERN_TEXTURE_DIMENSION_MAX || height > MODERN_TEXTURE_DIMENSION_MAX) {
            stbi_image_free(rgba);
            mdkr_modern_render_asset_shutdown(render);
            set_error(error, error_size, "character PNG failed bounded RGBA decode");
            return 0;
        }
        level_zero_bytes = (size_t)width * (size_t)height * 4u;
        mip_bytes = gfx_mip_chain_bytes(width, height);
        if (level_zero_bytes > MODERN_DECODED_TEXTURE_BYTES_MAX - render->decoded_texture_bytes ||
            mip_bytes > MODERN_DECODED_TEXTURE_BYTES_MAX - render->decoded_texture_bytes - level_zero_bytes) {
            stbi_image_free(rgba);
            mdkr_modern_render_asset_shutdown(render);
            set_error(error, error_size, "decoded character textures exceed the 512 MiB budget");
            return 0;
        }
        render->decoded[index].rgba = rgba;
        if (mip_bytes != 0u) {
            render->decoded[index].mip_scratch = (uint8_t *)malloc(mip_bytes);
            if (render->decoded[index].mip_scratch == NULL) {
                mdkr_modern_render_asset_shutdown(render);
                set_error(error, error_size, "could not allocate character texture mip chain");
                return 0;
            }
        }
        for (material_index = 0u; material_index < material_section->count;
             material_index++) {
            const struct GfxModernMaterial *material =
                &render->materials[material_index];
            if ((material->flags & 3u) == 1u &&
                material->texture[0] == (int)index) {
                int threshold = (int)lroundf(material->alpha_cutoff * 255.0f);
                if (threshold < 0) threshold = 0;
                if (threshold > 255) threshold = 255;
                if (cutout_threshold >= 0 && cutout_threshold != threshold) {
                    mdkr_modern_render_asset_shutdown(render);
                    set_error(error, error_size,
                              "one base texture uses conflicting alpha cutoffs");
                    return 0;
                }
                cutout_threshold = threshold;
            }
        }
        if (!(cutout_threshold >= 0
                  ? gfx_mip_build_cutout(
                        rgba, width, height,
                        render->decoded[index].mip_scratch, mip_bytes,
                        (uint8_t)cutout_threshold, &chain)
                  : (source.flags & 4u) != 0u
                  ? gfx_mip_build_normal(
                        rgba, width, height,
                        render->decoded[index].mip_scratch, mip_bytes, &chain)
                  : (source.flags & 2u) != 0u
                        ? gfx_mip_build_linear(
                              rgba, width, height,
                              render->decoded[index].mip_scratch, mip_bytes,
                              &chain)
                        : gfx_mip_build(
                              rgba, width, height,
                              render->decoded[index].mip_scratch, mip_bytes,
                              &chain))) {
            mdkr_modern_render_asset_shutdown(render);
            set_error(error, error_size, "could not build character texture mip chain");
            return 0;
        }
        destination->level_count = chain.level_count;
        memcpy(destination->level_rgba, chain.level, sizeof(chain.level));
        memcpy(destination->level_width, chain.width, sizeof(chain.width));
        memcpy(destination->level_height, chain.height, sizeof(chain.height));
        destination->wrap_s = source.wrap_s;
        destination->wrap_t = source.wrap_t;
        destination->min_filter = source.min_filter;
        destination->mag_filter = source.mag_filter;
        render->decoded_texture_bytes += level_zero_bytes + mip_bytes;
    }
    render->gpu.asset_id = digest_id(asset->source_sha256);
    render->gpu.vertices = render->vertices;
    render->gpu.vertex_count = vertex_section->count;
    render->gpu.indices = render->indices;
    render->gpu.index_count = index_section->count;
    render->gpu.primitives = render->primitives;
    render->gpu.primitive_count = primitive_section->count;
    render->gpu.materials = render->materials;
    render->gpu.material_count = material_section->count;
    render->gpu.textures = render->textures;
    render->gpu.texture_count = texture_section->count;
    render->valid = 1;
    set_error(error, error_size, "");
    return 1;
}

void mdkr_modern_render_asset_shutdown(MdkrModernRenderAsset *render) {
    uint32_t index;
    if (render == NULL) return;
    if (render->decoded != NULL) {
        for (index = 0u; index < render->gpu.texture_count; index++) {
            stbi_image_free(render->decoded[index].rgba);
            free(render->decoded[index].mip_scratch);
        }
    }
    free(render->decoded);
    free(render->sort_moments);
    free(render->primitive_sort);
    free(render->textures);
    free(render->materials);
    free(render->primitives);
    free(render->indices);
    free(render->vertices);
    memset(render, 0, sizeof(*render));
}
