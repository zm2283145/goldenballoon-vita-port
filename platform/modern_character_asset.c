#include "modern_character_asset.h"

#include "fs_utf8.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MDKC_SECTION_ENTRY_BYTES 32u
#define MDKC_SECTION_TABLE_OFFSET 64u

static const uint32_t s_expected_strides[MDKR_MDKC_SECTION_LAST + 1] = {
    0u, 1u, 72u, 4u, 32u, 80u, 40u, 1u, 48u, 16u, 68u,
    16u, 24u, 52u, 64u, 16u, 8u
};

static void set_error(char *error, size_t error_size, const char *message) {
    if (error != NULL && error_size != 0u) {
        (void)snprintf(error, error_size, "%s", message != NULL ? message : "unknown error");
    }
}

static uint16_t read_u16(const uint8_t *bytes) {
    return (uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8);
}

static uint32_t read_u32(const uint8_t *bytes) {
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static int32_t read_i32(const uint8_t *bytes) {
    return (int32_t)read_u32(bytes);
}

static uint64_t read_u64(const uint8_t *bytes) {
    return (uint64_t)read_u32(bytes) | ((uint64_t)read_u32(bytes + 4u) << 32);
}

static float read_f32(const uint8_t *bytes) {
    uint32_t bits = read_u32(bytes);
    float value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static uint32_t crc32_payload(const uint8_t *bytes, size_t size) {
    uint32_t crc = 0xFFFFFFFFu;
    size_t index;
    for (index = 0u; index < size; index++) {
        uint32_t value = crc ^ bytes[index];
        unsigned bit;
        for (bit = 0u; bit < 8u; bit++) {
            value = (value >> 1) ^ (0xEDB88320u & (0u - (value & 1u)));
        }
        crc = value;
    }
    return ~crc;
}

static int add_overflow_u64(uint64_t left, uint64_t right, uint64_t *out) {
    if (right > UINT64_MAX - left) return 1;
    *out = left + right;
    return 0;
}

static int multiply_overflow_u64(uint64_t left, uint64_t right, uint64_t *out) {
    if (left != 0u && right > UINT64_MAX / left) return 1;
    *out = left * right;
    return 0;
}

static const uint8_t *record(const MdkrModernCharacterAsset *asset,
                             MdkrModernSectionType type, uint32_t index) {
    const MdkrModernSectionView *section;
    if (asset == NULL || type <= 0 || type > MDKR_MDKC_SECTION_LAST) return NULL;
    section = &asset->sections[type];
    if (section->data == NULL || index >= section->count) return NULL;
    return section->data + (size_t)index * section->stride;
}

const MdkrModernSectionView *mdkr_modern_character_asset_section(
    const MdkrModernCharacterAsset *asset, MdkrModernSectionType type) {
    if (asset == NULL || type <= 0 || type > MDKR_MDKC_SECTION_LAST ||
        asset->sections[type].data == NULL) {
        return NULL;
    }
    return &asset->sections[type];
}

const char *mdkr_modern_character_asset_string(
    const MdkrModernCharacterAsset *asset, uint32_t offset) {
    const MdkrModernSectionView *strings;
    const uint8_t *end;
    if (asset == NULL) return NULL;
    strings = &asset->sections[MDKR_MDKC_STRINGS];
    if (strings->data == NULL || offset >= strings->size) return NULL;
    end = (const uint8_t *)memchr(strings->data + offset, 0,
                                 (size_t)(strings->size - offset));
    return end != NULL ? (const char *)(strings->data + offset) : NULL;
}

int mdkr_modern_character_asset_vertex(const MdkrModernCharacterAsset *asset,
                                       uint32_t index, MdkrModernVertex *out) {
    const uint8_t *data = record(asset, MDKR_MDKC_VERTICES, index);
    unsigned component;
    if (data == NULL || out == NULL) return 0;
    for (component = 0u; component < 3u; component++) out->position[component] = read_f32(data + component * 4u);
    for (component = 0u; component < 3u; component++) out->normal[component] = read_f32(data + 12u + component * 4u);
    for (component = 0u; component < 4u; component++) out->tangent[component] = read_f32(data + 24u + component * 4u);
    for (component = 0u; component < 2u; component++) out->uv[component] = read_f32(data + 40u + component * 4u);
    for (component = 0u; component < 4u; component++) out->joints[component] = read_u16(data + 48u + component * 2u);
    for (component = 0u; component < 4u; component++) out->weights[component] = read_f32(data + 56u + component * 4u);
    return 1;
}

int mdkr_modern_character_asset_primitive(const MdkrModernCharacterAsset *asset,
                                          uint32_t index, MdkrModernPrimitive *out) {
    const uint8_t *data = record(asset, MDKR_MDKC_PRIMITIVES, index);
    if (data == NULL || out == NULL) return 0;
    out->first_vertex = read_u32(data);
    out->vertex_count = read_u32(data + 4u);
    out->first_index = read_u32(data + 8u);
    out->index_count = read_u32(data + 12u);
    out->material = read_i32(data + 16u);
    out->node = read_u32(data + 20u);
    out->skin = read_i32(data + 24u);
    out->lod = read_u32(data + 28u);
    return 1;
}

int mdkr_modern_character_asset_material(const MdkrModernCharacterAsset *asset,
                                         uint32_t index, MdkrModernMaterial *out) {
    const uint8_t *data = record(asset, MDKR_MDKC_MATERIALS, index);
    unsigned component;
    if (data == NULL || out == NULL) return 0;
    memset(out, 0, sizeof(*out));
    out->name = read_u32(data);
    out->flags = read_u32(data + 4u);
    for (component = 0u; component < 5u; component++) out->textures[component] = read_i32(data + 8u + component * 4u);
    for (component = 0u; component < 4u; component++) out->base_color[component] = read_f32(data + 28u + component * 4u);
    for (component = 0u; component < 3u; component++) out->emissive[component] = read_f32(data + 44u + component * 4u);
    out->metallic = read_f32(data + 56u);
    out->roughness = read_f32(data + 60u);
    out->normal_scale = read_f32(data + 64u);
    out->occlusion_strength = read_f32(data + 68u);
    out->alpha_cutoff = read_f32(data + 72u);
    return 1;
}

int mdkr_modern_character_asset_texture(const MdkrModernCharacterAsset *asset,
                                        uint32_t index, MdkrModernTexture *out) {
    const uint8_t *data = record(asset, MDKR_MDKC_TEXTURES, index);
    if (data == NULL || out == NULL) return 0;
    out->name = read_u32(data);
    out->mime = read_u32(data + 4u);
    out->data_offset = read_u32(data + 8u);
    out->data_size = read_u32(data + 12u);
    out->wrap_s = read_i32(data + 16u);
    out->wrap_t = read_i32(data + 20u);
    out->min_filter = read_i32(data + 24u);
    out->mag_filter = read_i32(data + 28u);
    out->flags = read_u32(data + 32u);
    return 1;
}

int mdkr_modern_character_asset_node(const MdkrModernCharacterAsset *asset,
                                     uint32_t index, MdkrModernNode *out) {
    const uint8_t *data = record(asset, MDKR_MDKC_NODES, index);
    unsigned component;
    if (data == NULL || out == NULL) return 0;
    out->name = read_u32(data);
    out->parent = read_i32(data + 4u);
    for (component = 0u; component < 3u; component++) out->translation[component] = read_f32(data + 8u + component * 4u);
    for (component = 0u; component < 4u; component++) out->rotation[component] = read_f32(data + 20u + component * 4u);
    for (component = 0u; component < 3u; component++) out->scale[component] = read_f32(data + 36u + component * 4u);
    return 1;
}

int mdkr_modern_character_asset_skin(const MdkrModernCharacterAsset *asset,
                                     uint32_t index, MdkrModernSkin *out) {
    const uint8_t *data = record(asset, MDKR_MDKC_SKINS, index);
    if (data == NULL || out == NULL) return 0;
    out->name = read_u32(data);
    out->first_joint = read_u32(data + 4u);
    out->joint_count = read_u32(data + 8u);
    out->skeleton = read_u32(data + 12u);
    return 1;
}

int mdkr_modern_character_asset_joint(const MdkrModernCharacterAsset *asset,
                                      uint32_t index, MdkrModernJoint *out) {
    const uint8_t *data = record(asset, MDKR_MDKC_JOINTS, index);
    unsigned component;
    if (data == NULL || out == NULL) return 0;
    out->node = read_u32(data);
    for (component = 0u; component < 16u; component++) out->inverse_bind[component] = read_f32(data + 4u + component * 4u);
    return 1;
}

int mdkr_modern_character_asset_animation(const MdkrModernCharacterAsset *asset,
                                          uint32_t index, MdkrModernAnimation *out) {
    const uint8_t *data = record(asset, MDKR_MDKC_ANIMATIONS, index);
    if (data == NULL || out == NULL) return 0;
    out->name = read_u32(data);
    out->duration = read_f32(data + 4u);
    out->first_channel = read_u32(data + 8u);
    out->channel_count = read_u32(data + 12u);
    return 1;
}

int mdkr_modern_character_asset_channel(const MdkrModernCharacterAsset *asset,
                                        uint32_t index, MdkrModernChannel *out) {
    const uint8_t *data = record(asset, MDKR_MDKC_CHANNELS, index);
    if (data == NULL || out == NULL) return 0;
    out->node = read_u32(data);
    out->path = read_u32(data + 4u);
    out->interpolation = read_u32(data + 8u);
    out->first_key = read_u32(data + 12u);
    out->key_count = read_u32(data + 16u);
    out->components = read_u32(data + 20u);
    return 1;
}

int mdkr_modern_character_asset_key(const MdkrModernCharacterAsset *asset,
                                    uint32_t index, MdkrModernKey *out) {
    const uint8_t *data = record(asset, MDKR_MDKC_KEYS, index);
    unsigned component;
    if (data == NULL || out == NULL) return 0;
    out->time = read_f32(data);
    for (component = 0u; component < 4u; component++) out->value[component] = read_f32(data + 4u + component * 4u);
    for (component = 0u; component < 4u; component++) out->incoming[component] = read_f32(data + 20u + component * 4u);
    for (component = 0u; component < 4u; component++) out->outgoing[component] = read_f32(data + 36u + component * 4u);
    return 1;
}

int mdkr_modern_character_asset_definition(
    const MdkrModernCharacterAsset *asset, MdkrModernCharacterDefinition *out) {
    const uint8_t *data = record(asset, MDKR_MDKC_CHARACTER, 0u);
    unsigned component;
    if (data == NULL || out == NULL) return 0;
    out->id = read_u32(data);
    out->display_name = read_u32(data + 4u);
    out->donor = read_u32(data + 8u);
    out->renderer_profile = read_u32(data + 12u);
    for (component = 0u; component < 3u; component++) out->scale[component] = read_f32(data + 16u + component * 4u);
    for (component = 0u; component < 3u; component++) out->translation[component] = read_f32(data + 28u + component * 4u);
    for (component = 0u; component < 4u; component++) out->rotation[component] = read_f32(data + 40u + component * 4u);
    out->lod_bias = read_f32(data + 56u);
    out->vehicle_mask = read_u32(data + 60u);
    return 1;
}

int mdkr_modern_character_asset_semantic(const MdkrModernCharacterAsset *asset,
                                         uint32_t index, MdkrModernSemantic *out) {
    const uint8_t *data = record(asset, MDKR_MDKC_SEMANTICS, index);
    if (data == NULL || out == NULL) return 0;
    out->semantic = read_u32(data);
    out->clip = read_u32(data + 4u);
    out->flags = read_u32(data + 8u);
    out->blend_seconds = read_f32(data + 12u);
    return 1;
}

int mdkr_modern_character_asset_socket(const MdkrModernCharacterAsset *asset,
                                       uint32_t index, MdkrModernSocket *out) {
    const uint8_t *data = record(asset, MDKR_MDKC_SOCKETS, index);
    if (data == NULL || out == NULL) return 0;
    out->semantic = read_u32(data);
    out->node = read_u32(data + 4u);
    return 1;
}

static int finite_array(const float *values, size_t count) {
    size_t index;
    for (index = 0u; index < count; index++) {
        if (!isfinite(values[index])) return 0;
    }
    return 1;
}

static int range_u32(uint32_t first, uint32_t count, uint32_t total) {
    return first <= total && count <= total - first;
}

static int validate_references(const MdkrModernCharacterAsset *asset,
                               char *error, size_t error_size) {
    const MdkrModernSectionView *vertices = &asset->sections[MDKR_MDKC_VERTICES];
    const MdkrModernSectionView *indices = &asset->sections[MDKR_MDKC_INDICES];
    const MdkrModernSectionView *primitives = &asset->sections[MDKR_MDKC_PRIMITIVES];
    const MdkrModernSectionView *materials = &asset->sections[MDKR_MDKC_MATERIALS];
    const MdkrModernSectionView *textures = &asset->sections[MDKR_MDKC_TEXTURES];
    const MdkrModernSectionView *texture_data = &asset->sections[MDKR_MDKC_TEXTURE_DATA];
    const MdkrModernSectionView *nodes = &asset->sections[MDKR_MDKC_NODES];
    const MdkrModernSectionView *skins = &asset->sections[MDKR_MDKC_SKINS];
    const MdkrModernSectionView *joints = &asset->sections[MDKR_MDKC_JOINTS];
    const MdkrModernSectionView *animations = &asset->sections[MDKR_MDKC_ANIMATIONS];
    const MdkrModernSectionView *channels = &asset->sections[MDKR_MDKC_CHANNELS];
    const MdkrModernSectionView *keys = &asset->sections[MDKR_MDKC_KEYS];
    uint32_t index;
    if (vertices->count == 0u || indices->count == 0u || primitives->count == 0u ||
        materials->count == 0u || nodes->count == 0u || skins->count == 0u ||
        joints->count == 0u || animations->count == 0u || channels->count == 0u ||
        keys->count == 0u || asset->sections[MDKR_MDKC_CHARACTER].count != 1u ||
        asset->sections[MDKR_MDKC_SEMANTICS].count == 0u ||
        asset->sections[MDKR_MDKC_SOCKETS].count == 0u) {
        set_error(error, error_size, "compiled character is missing a required runtime section");
        return 0;
    }
    /* These are admission ceilings, not target budgets. They are checked
     * before walking any attacker-controlled record, keeping validation work
     * bounded while still allowing a character orders of magnitude denser
     * than retail N64 geometry. */
    if (vertices->count > 100000u || indices->count > 300000u ||
        primitives->count > 512u || materials->count > 16u ||
        textures->count > 80u || texture_data->count > 512u * 1024u * 1024u ||
        nodes->count > 4096u || skins->count > 64u || joints->count > 8192u ||
        animations->count > 64u || channels->count > 4096u ||
        keys->count > 1000000u ||
        asset->sections[MDKR_MDKC_SEMANTICS].count > 65u ||
        asset->sections[MDKR_MDKC_SOCKETS].count > 32u) {
        set_error(error, error_size, "compiled character exceeds a runtime admission ceiling");
        return 0;
    }
    for (index = 0u; index < vertices->count; index++) {
        MdkrModernVertex vertex;
        float weight_sum;
        if (!mdkr_modern_character_asset_vertex(asset, index, &vertex) ||
            !finite_array(vertex.position, 3u) || !finite_array(vertex.normal, 3u) ||
            !finite_array(vertex.tangent, 4u) || !finite_array(vertex.uv, 2u) ||
            !finite_array(vertex.weights, 4u)) {
            set_error(error, error_size, "compiled vertex contains non-finite data");
            return 0;
        }
        weight_sum = vertex.weights[0] + vertex.weights[1] + vertex.weights[2] + vertex.weights[3];
        if (vertex.weights[0] < 0.0f || vertex.weights[1] < 0.0f ||
            vertex.weights[2] < 0.0f || vertex.weights[3] < 0.0f ||
            weight_sum < 0.999f || weight_sum > 1.001f) {
            set_error(error, error_size, "compiled vertex skin weights are invalid");
            return 0;
        }
    }
    for (index = 0u; index < primitives->count; index++) {
        MdkrModernPrimitive primitive;
        uint32_t element;
        uint32_t skin_joint_count = 0u;
        if (!mdkr_modern_character_asset_primitive(asset, index, &primitive) ||
            !range_u32(primitive.first_vertex, primitive.vertex_count, vertices->count) ||
            !range_u32(primitive.first_index, primitive.index_count, indices->count) ||
            primitive.index_count == 0u || primitive.index_count % 3u != 0u ||
            primitive.material < 0 || (uint32_t)primitive.material >= materials->count ||
            primitive.node >= nodes->count ||
            (primitive.skin >= 0 && (uint32_t)primitive.skin >= skins->count)) {
            set_error(error, error_size, "compiled primitive reference is out of bounds");
            return 0;
        }
        if (primitive.skin >= 0) {
            MdkrModernSkin skin;
            (void)mdkr_modern_character_asset_skin(asset, (uint32_t)primitive.skin, &skin);
            skin_joint_count = skin.joint_count;
        }
        for (element = 0u; element < primitive.index_count; element++) {
            uint32_t vertex_index = read_u32(indices->data + (size_t)(primitive.first_index + element) * 4u);
            if (vertex_index < primitive.first_vertex ||
                vertex_index >= primitive.first_vertex + primitive.vertex_count) {
                set_error(error, error_size, "compiled primitive index escapes its vertex range");
                return 0;
            }
        }
        if (primitive.skin >= 0) {
            for (element = 0u; element < primitive.vertex_count; element++) {
                MdkrModernVertex vertex;
                unsigned influence;
                (void)mdkr_modern_character_asset_vertex(asset, primitive.first_vertex + element, &vertex);
                for (influence = 0u; influence < 4u; influence++) {
                    if (vertex.weights[influence] > 0.0f && vertex.joints[influence] >= skin_joint_count) {
                        set_error(error, error_size, "compiled vertex joint escapes its skin");
                        return 0;
                    }
                }
            }
        }
    }
    for (index = 0u; index < materials->count; index++) {
        MdkrModernMaterial material;
        unsigned slot;
        (void)mdkr_modern_character_asset_material(asset, index, &material);
        if (mdkr_modern_character_asset_string(asset, material.name) == NULL ||
            !finite_array(material.base_color, 4u) || !finite_array(material.emissive, 3u) ||
            !isfinite(material.metallic) || !isfinite(material.roughness) ||
            !isfinite(material.normal_scale) || !isfinite(material.occlusion_strength) ||
            !isfinite(material.alpha_cutoff) || (material.flags & ~7u) != 0u) {
            set_error(error, error_size, "compiled material is invalid");
            return 0;
        }
        for (slot = 0u; slot < 5u; slot++) {
            if (material.textures[slot] < -1 ||
                (material.textures[slot] >= 0 && (uint32_t)material.textures[slot] >= textures->count)) {
                set_error(error, error_size, "compiled material texture reference is invalid");
                return 0;
            }
        }
    }
    for (index = 0u; index < textures->count; index++) {
        MdkrModernTexture texture;
        (void)mdkr_modern_character_asset_texture(asset, index, &texture);
        if (mdkr_modern_character_asset_string(asset, texture.name) == NULL ||
            (texture.mime != 1u && texture.mime != 2u) || texture.data_size == 0u ||
            (texture.flags != 1u && texture.flags != 2u && texture.flags != 4u) ||
            !range_u32(texture.data_offset, texture.data_size, texture_data->count)) {
            set_error(error, error_size, "compiled texture is invalid");
            return 0;
        }
    }
    for (index = 0u; index < nodes->count; index++) {
        MdkrModernNode node;
        float quaternion_length;
        (void)mdkr_modern_character_asset_node(asset, index, &node);
        if (mdkr_modern_character_asset_string(asset, node.name) == NULL ||
            node.parent < -1 || (node.parent >= 0 && (uint32_t)node.parent >= nodes->count) ||
            (node.parent >= 0 && (uint32_t)node.parent == index) ||
            !finite_array(node.translation, 3u) || !finite_array(node.rotation, 4u) ||
            !finite_array(node.scale, 3u)) {
            set_error(error, error_size, "compiled scene node is invalid");
            return 0;
        }
        quaternion_length = node.rotation[0] * node.rotation[0] + node.rotation[1] * node.rotation[1] +
                            node.rotation[2] * node.rotation[2] + node.rotation[3] * node.rotation[3];
        if (quaternion_length < 0.999f || quaternion_length > 1.001f) {
            set_error(error, error_size, "compiled node quaternion is not normalized");
            return 0;
        }
    }
    for (index = 0u; index < skins->count; index++) {
        MdkrModernSkin skin;
        (void)mdkr_modern_character_asset_skin(asset, index, &skin);
        if (mdkr_modern_character_asset_string(asset, skin.name) == NULL ||
            skin.joint_count == 0u || skin.joint_count > 128u ||
            !range_u32(skin.first_joint, skin.joint_count, joints->count) ||
            skin.skeleton >= nodes->count) {
            set_error(error, error_size, "compiled skin is invalid");
            return 0;
        }
    }
    for (index = 0u; index < joints->count; index++) {
        MdkrModernJoint joint;
        (void)mdkr_modern_character_asset_joint(asset, index, &joint);
        if (joint.node >= nodes->count || !finite_array(joint.inverse_bind, 16u)) {
            set_error(error, error_size, "compiled joint is invalid");
            return 0;
        }
    }
    for (index = 0u; index < animations->count; index++) {
        MdkrModernAnimation animation;
        (void)mdkr_modern_character_asset_animation(asset, index, &animation);
        if (mdkr_modern_character_asset_string(asset, animation.name) == NULL ||
            !isfinite(animation.duration) || animation.duration <= 0.0f ||
            animation.channel_count == 0u ||
            !range_u32(animation.first_channel, animation.channel_count, channels->count)) {
            set_error(error, error_size, "compiled animation is invalid");
            return 0;
        }
    }
    for (index = 0u; index < channels->count; index++) {
        MdkrModernChannel channel;
        uint32_t key_index;
        float previous = -1.0f;
        (void)mdkr_modern_character_asset_channel(asset, index, &channel);
        if (channel.node >= nodes->count || channel.path > 3u || channel.interpolation > 2u ||
            channel.components < 3u || channel.components > 4u || channel.key_count == 0u ||
            !range_u32(channel.first_key, channel.key_count, keys->count)) {
            set_error(error, error_size, "compiled animation channel is invalid");
            return 0;
        }
        for (key_index = 0u; key_index < channel.key_count; key_index++) {
            MdkrModernKey key;
            (void)mdkr_modern_character_asset_key(asset, channel.first_key + key_index, &key);
            if (!isfinite(key.time) || key.time <= previous || !finite_array(key.value, 4u) ||
                !finite_array(key.incoming, 4u) || !finite_array(key.outgoing, 4u)) {
                set_error(error, error_size, "compiled animation key is invalid");
                return 0;
            }
            previous = key.time;
        }
    }
    {
        MdkrModernCharacterDefinition definition;
        float quaternion_length;
        (void)mdkr_modern_character_asset_definition(asset, &definition);
        quaternion_length = definition.rotation[0] * definition.rotation[0] +
                            definition.rotation[1] * definition.rotation[1] +
                            definition.rotation[2] * definition.rotation[2] +
                            definition.rotation[3] * definition.rotation[3];
        if (mdkr_modern_character_asset_string(asset, definition.id) == NULL ||
            mdkr_modern_character_asset_string(asset, definition.display_name) == NULL ||
            mdkr_modern_character_asset_string(asset, definition.renderer_profile) == NULL ||
            definition.donor >= 10u || definition.vehicle_mask == 0u ||
            (definition.vehicle_mask & ~7u) != 0u || !finite_array(definition.scale, 3u) ||
            !finite_array(definition.translation, 3u) || !finite_array(definition.rotation, 4u) ||
            definition.scale[0] <= 0.0f || definition.scale[1] <= 0.0f ||
            definition.scale[2] <= 0.0f || !isfinite(definition.lod_bias) ||
            definition.lod_bias < -4.0f || definition.lod_bias > 4.0f ||
            quaternion_length < 0.999f || quaternion_length > 1.001f) {
            set_error(error, error_size, "compiled character definition is invalid");
            return 0;
        }
    }
    for (index = 0u; index < asset->sections[MDKR_MDKC_SEMANTICS].count; index++) {
        MdkrModernSemantic semantic;
        (void)mdkr_modern_character_asset_semantic(asset, index, &semantic);
        if (mdkr_modern_character_asset_string(asset, semantic.semantic) == NULL ||
            mdkr_modern_character_asset_string(asset, semantic.clip) == NULL ||
            !isfinite(semantic.blend_seconds) || semantic.blend_seconds < 0.0f || semantic.blend_seconds > 5.0f) {
            set_error(error, error_size, "compiled animation semantic is invalid");
            return 0;
        }
    }
    for (index = 0u; index < asset->sections[MDKR_MDKC_SOCKETS].count; index++) {
        MdkrModernSocket socket;
        (void)mdkr_modern_character_asset_socket(asset, index, &socket);
        if (mdkr_modern_character_asset_string(asset, socket.semantic) == NULL || socket.node >= nodes->count) {
            set_error(error, error_size, "compiled socket is invalid");
            return 0;
        }
    }
    return 1;
}

static int parse_cache(MdkrModernCharacterAsset *asset,
                       char *error, size_t error_size) {
    const uint8_t *bytes = asset->owned_bytes;
    uint32_t section_count;
    uint32_t section_index;
    if (asset->size < MDKR_MDKC_HEADER_BYTES || memcmp(bytes, "MDKC", 4u) != 0 ||
        read_u32(bytes + 4u) != MDKR_MDKC_VERSION ||
        read_u32(bytes + 8u) != MDKR_MDKC_HEADER_BYTES ||
        read_u64(bytes + 12u) != asset->size) {
        set_error(error, error_size, "compiled character header is invalid");
        return 0;
    }
    if (read_u32(bytes + 52u) != crc32_payload(bytes + MDKR_MDKC_HEADER_BYTES,
                                               asset->size - MDKR_MDKC_HEADER_BYTES)) {
        set_error(error, error_size, "compiled character payload checksum does not match");
        return 0;
    }
    section_count = read_u32(bytes + 56u);
    if (section_count == 0u || section_count > MDKR_MDKC_SECTION_SLOTS) {
        set_error(error, error_size, "compiled character section count is invalid");
        return 0;
    }
    memcpy(asset->source_sha256, bytes + 20u, sizeof(asset->source_sha256));
    for (section_index = 0u; section_index < section_count; section_index++) {
        const uint8_t *entry = bytes + MDKC_SECTION_TABLE_OFFSET +
                               (size_t)section_index * MDKC_SECTION_ENTRY_BYTES;
        uint32_t type = read_u32(entry);
        uint32_t flags = read_u32(entry + 4u);
        uint64_t offset = read_u64(entry + 8u);
        uint64_t size = read_u64(entry + 16u);
        uint32_t count = read_u32(entry + 24u);
        uint32_t stride = read_u32(entry + 28u);
        uint64_t end;
        uint64_t expected_size;
        if (type == 0u || type > MDKR_MDKC_SECTION_LAST ||
            asset->sections[type].data != NULL || stride != s_expected_strides[type] ||
            multiply_overflow_u64(count, stride, &expected_size) || expected_size != size ||
            offset < MDKR_MDKC_HEADER_BYTES || (offset & 15u) != 0u ||
            add_overflow_u64(offset, size, &end) || end > asset->size) {
            set_error(error, error_size, "compiled character section table is invalid");
            return 0;
        }
        asset->sections[type].data = bytes + (size_t)offset;
        asset->sections[type].size = size;
        asset->sections[type].count = count;
        asset->sections[type].stride = stride;
        asset->sections[type].flags = flags;
    }
    for (section_index = 1u; section_index <= MDKR_MDKC_SECTION_LAST; section_index++) {
        if (asset->sections[section_index].data == NULL) {
            set_error(error, error_size, "compiled character omits a required section");
            return 0;
        }
    }
    if (asset->sections[MDKR_MDKC_STRINGS].size == 0u ||
        asset->sections[MDKR_MDKC_STRINGS].data[0] != 0u) {
        set_error(error, error_size, "compiled character string table is invalid");
        return 0;
    }
    return validate_references(asset, error, error_size);
}

int mdkr_modern_character_asset_load_memory(const void *bytes, size_t size,
                                            MdkrModernCharacterAsset *out,
                                            char *error, size_t error_size) {
    uint8_t *copy;
    if (out == NULL) {
        set_error(error, error_size, "no character asset output supplied");
        return 0;
    }
    memset(out, 0, sizeof(*out));
    if (bytes == NULL || size < MDKR_MDKC_HEADER_BYTES ||
        (uint64_t)size > (uint64_t)MDKR_MDKC_FILE_MAX) {
        set_error(error, error_size, "compiled character size is invalid");
        return 0;
    }
    copy = (uint8_t *)malloc(size);
    if (copy == NULL) {
        set_error(error, error_size, "could not allocate compiled character bytes");
        return 0;
    }
    memcpy(copy, bytes, size);
    out->owned_bytes = copy;
    out->size = size;
    if (!parse_cache(out, error, error_size)) {
        mdkr_modern_character_asset_unload(out);
        return 0;
    }
    set_error(error, error_size, "");
    return 1;
}

int mdkr_modern_character_asset_load_file(const char *path,
                                          MdkrModernCharacterAsset *out,
                                          char *error, size_t error_size) {
    FILE *file;
    long end;
    uint8_t *bytes;
    size_t size;
    int loaded;
    if (out == NULL) {
        set_error(error, error_size, "no character asset output supplied");
        return 0;
    }
    memset(out, 0, sizeof(*out));
    if (path == NULL || path[0] == '\0' || (file = mdkr_fopen_utf8(path, "rb")) == NULL) {
        set_error(error, error_size, "compiled character file could not be opened");
        return 0;
    }
    if (fseek(file, 0, SEEK_END) != 0 || (end = ftell(file)) < 0 ||
        (uint64_t)end > (uint64_t)MDKR_MDKC_FILE_MAX ||
        (size = (size_t)end) < MDKR_MDKC_HEADER_BYTES || fseek(file, 0, SEEK_SET) != 0) {
        (void)fclose(file);
        set_error(error, error_size, "compiled character file size is invalid");
        return 0;
    }
    bytes = (uint8_t *)malloc(size);
    if (bytes == NULL) {
        (void)fclose(file);
        set_error(error, error_size, "could not allocate compiled character file");
        return 0;
    }
    if (fread(bytes, 1u, size, file) != size || fclose(file) != 0) {
        free(bytes);
        set_error(error, error_size, "compiled character file could not be read");
        return 0;
    }
    loaded = mdkr_modern_character_asset_load_memory(bytes, size, out, error, error_size);
    free(bytes);
    return loaded;
}

void mdkr_modern_character_asset_unload(MdkrModernCharacterAsset *asset) {
    if (asset == NULL) return;
    free(asset->owned_bytes);
    memset(asset, 0, sizeof(*asset));
}

void mdkr_modern_character_asset_stats(const MdkrModernCharacterAsset *asset,
                                       MdkrModernCharacterStats *out) {
    if (out == NULL) return;
    memset(out, 0, sizeof(*out));
    if (asset == NULL || asset->owned_bytes == NULL) return;
    out->vertices = asset->sections[MDKR_MDKC_VERTICES].count;
    out->triangles = asset->sections[MDKR_MDKC_INDICES].count / 3u;
    out->primitives = asset->sections[MDKR_MDKC_PRIMITIVES].count;
    out->materials = asset->sections[MDKR_MDKC_MATERIALS].count;
    out->textures = asset->sections[MDKR_MDKC_TEXTURES].count;
    out->nodes = asset->sections[MDKR_MDKC_NODES].count;
    out->skins = asset->sections[MDKR_MDKC_SKINS].count;
    out->joints = asset->sections[MDKR_MDKC_JOINTS].count;
    out->animations = asset->sections[MDKR_MDKC_ANIMATIONS].count;
    out->animation_channels = asset->sections[MDKR_MDKC_CHANNELS].count;
    out->animation_keys = asset->sections[MDKR_MDKC_KEYS].count;
    out->semantics = asset->sections[MDKR_MDKC_SEMANTICS].count;
    out->sockets = asset->sections[MDKR_MDKC_SOCKETS].count;
    out->encoded_texture_bytes = asset->sections[MDKR_MDKC_TEXTURE_DATA].size;
}
