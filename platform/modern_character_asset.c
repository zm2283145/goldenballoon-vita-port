#include "modern_character_asset.h"

#include "fs_utf8.h"
#include "modern_character_ktx2.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MDKC_SECTION_ENTRY_BYTES 32u
#define MDKC_SECTION_TABLE_OFFSET 64u

static const uint32_t s_expected_strides[MDKR_MDKC_SECTION_LAST + 1] = {
    0u, 1u, 72u, 4u, 32u, 80u, 40u, 1u, 48u, 16u, 68u,
    16u, 24u, 52u, 64u, 16u, 8u, 48u, 64u, 24u, 1u, 16u, 44u, 16u,
    12u, 32u, 44u, 16u
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

static int compiled_id_valid(const char *text) {
    size_t index;
    const size_t length = text != NULL ? strlen(text) : 0u;
    if (length < 2u || length > 64u ||
        !((text[0] >= 'a' && text[0] <= 'z') ||
          (text[0] >= '0' && text[0] <= '9'))) return 0;
    for (index = 1u; index < length; index++) {
        const char byte = text[index];
        if (!((byte >= 'a' && byte <= 'z') ||
              (byte >= '0' && byte <= '9') || byte == '.' ||
              byte == '_' || byte == '-')) return 0;
    }
    return 1;
}

static int compiled_semantic_valid(const char *text) {
    size_t index;
    const size_t length = text != NULL ? strlen(text) : 0u;
    if (length == 0u || length > 64u ||
        text[0] < 'a' || text[0] > 'z') return 0;
    for (index = 1u; index < length; ++index) {
        const char byte = text[index];
        if (!((byte >= 'a' && byte <= 'z') ||
              (byte >= '0' && byte <= '9') || byte == '.' ||
              byte == '_' || byte == '-')) return 0;
    }
    return 1;
}

static int bounded_printable_utf8(const char *text, size_t maximum_bytes) {
    const unsigned char *bytes = (const unsigned char *)text;
    size_t remaining;
    if (text == NULL) return 0;
    remaining = strlen(text);
    if (remaining == 0u || remaining > maximum_bytes) return 0;
    while (remaining != 0u) {
        uint32_t codepoint;
        uint32_t minimum;
        unsigned continuation;
        const unsigned char first = *bytes++;
        remaining--;
        if (first < 0x80u) {
            codepoint = first;
            minimum = 0u;
            continuation = 0u;
        } else if (first >= 0xC2u && first <= 0xDFu) {
            codepoint = first & 0x1Fu;
            minimum = 0x80u;
            continuation = 1u;
        } else if (first >= 0xE0u && first <= 0xEFu) {
            codepoint = first & 0x0Fu;
            minimum = 0x800u;
            continuation = 2u;
        } else if (first >= 0xF0u && first <= 0xF4u) {
            codepoint = first & 0x07u;
            minimum = 0x10000u;
            continuation = 3u;
        } else {
            return 0;
        }
        if ((size_t)continuation > remaining) return 0;
        while (continuation-- != 0u) {
            const unsigned char next = *bytes++;
            remaining--;
            if ((next & 0xC0u) != 0x80u) return 0;
            codepoint = (codepoint << 6u) | (next & 0x3Fu);
        }
        if (codepoint < minimum || codepoint > 0x10FFFFu ||
            (codepoint >= 0xD800u && codepoint <= 0xDFFFu) ||
            codepoint < 0x20u ||
            (codepoint >= 0x7Fu && codepoint <= 0x9Fu) ||
            (codepoint >= 0x200Bu && codepoint <= 0x200Fu) ||
            (codepoint >= 0x2028u && codepoint <= 0x202Eu) ||
            (codepoint >= 0x2060u && codepoint <= 0x206Fu) ||
            codepoint == 0xFEFFu) return 0;
    }
    return 1;
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
    out->dimensions = read_u32(data + 36u);
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

static void rotate_bind_point(const float rotation[4], const float point[3],
                              float output[3]) {
    const float tx = 2.0f * (rotation[1] * point[2] - rotation[2] * point[1]);
    const float ty = 2.0f * (rotation[2] * point[0] - rotation[0] * point[2]);
    const float tz = 2.0f * (rotation[0] * point[1] - rotation[1] * point[0]);
    output[0] = point[0] + rotation[3] * tx +
                rotation[1] * tz - rotation[2] * ty;
    output[1] = point[1] + rotation[3] * ty +
                rotation[2] * tx - rotation[0] * tz;
    output[2] = point[2] + rotation[3] * tz +
                rotation[0] * ty - rotation[1] * tx;
}

int mdkr_modern_character_asset_node_bind_position(
    const MdkrModernCharacterAsset *asset, uint32_t node_index,
    float output[3]) {
    const MdkrModernSectionView *nodes = mdkr_modern_character_asset_section(
        asset, MDKR_MDKC_NODES);
    MdkrModernNode node;
    int32_t parent;
    uint32_t depth;
    if (nodes == NULL || output == NULL || node_index >= nodes->count ||
        !mdkr_modern_character_asset_node(asset, node_index, &node)) return 0;
    memcpy(output, node.translation, sizeof(node.translation));
    parent = node.parent;
    for (depth = 0u; parent >= 0 && depth < nodes->count; depth++) {
        float scaled[3];
        float rotated[3];
        unsigned axis;
        if ((uint32_t)parent >= nodes->count ||
            !mdkr_modern_character_asset_node(asset, (uint32_t)parent,
                                              &node)) return 0;
        for (axis = 0u; axis < 3u; axis++) {
            scaled[axis] = output[axis] * node.scale[axis];
        }
        rotate_bind_point(node.rotation, scaled, rotated);
        for (axis = 0u; axis < 3u; axis++) {
            output[axis] = rotated[axis] + node.translation[axis];
        }
        parent = node.parent;
    }
    return parent < 0;
}

static void multiply_bind_quaternion(const float left[4],
                                     const float right[4], float output[4]) {
    const float value[4] = {
        left[3] * right[0] + left[0] * right[3] +
            left[1] * right[2] - left[2] * right[1],
        left[3] * right[1] - left[0] * right[2] +
            left[1] * right[3] + left[2] * right[0],
        left[3] * right[2] + left[0] * right[1] -
            left[1] * right[0] + left[2] * right[3],
        left[3] * right[3] - left[0] * right[0] -
            left[1] * right[1] - left[2] * right[2]
    };
    memcpy(output, value, sizeof(value));
}

int mdkr_modern_character_asset_node_bind_rotation(
    const MdkrModernCharacterAsset *asset, uint32_t node_index,
    float output[4]) {
    const MdkrModernSectionView *nodes = mdkr_modern_character_asset_section(
        asset, MDKR_MDKC_NODES);
    MdkrModernNode node;
    int32_t parent;
    uint32_t depth;
    float length;
    if (nodes == NULL || output == NULL || node_index >= nodes->count ||
        !mdkr_modern_character_asset_node(asset, node_index, &node)) return 0;
    memcpy(output, node.rotation, sizeof(node.rotation));
    parent = node.parent;
    for (depth = 0u; parent >= 0 && depth < nodes->count; depth++) {
        if ((uint32_t)parent >= nodes->count ||
            !mdkr_modern_character_asset_node(asset, (uint32_t)parent,
                                              &node)) return 0;
        multiply_bind_quaternion(node.rotation, output, output);
        parent = node.parent;
    }
    if (parent >= 0) return 0;
    length = sqrtf(output[0] * output[0] + output[1] * output[1] +
                   output[2] * output[2] + output[3] * output[3]);
    if (!isfinite(length) || length < 1.0e-8f) return 0;
    output[0] /= length;
    output[1] /= length;
    output[2] /= length;
    output[3] /= length;
    if (output[3] < 0.0f) {
        output[0] = -output[0]; output[1] = -output[1];
        output[2] = -output[2]; output[3] = -output[3];
    }
    return 1;
}

int mdkr_modern_character_asset_joint_parent_node(
    const MdkrModernCharacterAsset *asset, uint32_t joint_index,
    int32_t *parent_node) {
    const MdkrModernSectionView *nodes = mdkr_modern_character_asset_section(
        asset, MDKR_MDKC_NODES);
    const MdkrModernSectionView *joints = mdkr_modern_character_asset_section(
        asset, MDKR_MDKC_JOINTS);
    MdkrModernJoint joint;
    MdkrModernNode node;
    int32_t parent;
    uint32_t depth;
    if (nodes == NULL || joints == NULL || parent_node == NULL ||
        joint_index >= joints->count ||
        !mdkr_modern_character_asset_joint(asset, joint_index, &joint) ||
        !mdkr_modern_character_asset_node(asset, joint.node, &node)) return 0;
    parent = node.parent;
    for (depth = 0u; parent >= 0 && depth < nodes->count; depth++) {
        uint32_t candidate;
        if ((uint32_t)parent >= nodes->count) return 0;
        for (candidate = 0u; candidate < joints->count; candidate++) {
            MdkrModernJoint possible;
            if (!mdkr_modern_character_asset_joint(asset, candidate,
                                                    &possible)) return 0;
            if (possible.node == (uint32_t)parent) {
                *parent_node = parent;
                return 1;
            }
        }
        if (!mdkr_modern_character_asset_node(asset, (uint32_t)parent,
                                              &node)) return 0;
        parent = node.parent;
    }
    if (parent >= 0) return 0;
    *parent_node = -1;
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

int mdkr_modern_character_asset_attachment(
    const MdkrModernCharacterAsset *asset, uint32_t index,
    MdkrModernAttachment *out) {
    const uint8_t *data = record(asset, MDKR_MDKC_ATTACHMENTS, index);
    unsigned component;
    if (data == NULL || out == NULL) return 0;
    memset(out, 0, sizeof(*out));
    out->context = read_u32(data);
    out->anchor = read_u32(data + 4u);
    for (component = 0u; component < 3u; component++) {
        out->translation[component] = read_f32(data + 8u + component * 4u);
    }
    for (component = 0u; component < 4u; component++) {
        out->rotation[component] = read_f32(data + 20u + component * 4u);
    }
    out->scale = read_f32(data + 36u);
    out->flags = read_u32(data + 40u);
    return 1;
}

int mdkr_modern_character_asset_calibration(
    const MdkrModernCharacterAsset *asset, MdkrModernCalibration *out) {
    const uint8_t *data = record(asset, MDKR_MDKC_CALIBRATION, 0u);
    unsigned component;
    if (data == NULL || out == NULL) return 0;
    memset(out, 0, sizeof(*out));
    for (component = 0u; component < 3u; component++) {
        out->bounds_min[component] = read_f32(data + component * 4u);
        out->bounds_max[component] = read_f32(data + 12u + component * 4u);
        out->ground[component] = read_f32(data + 24u + component * 4u);
    }
    out->source_height = read_f32(data + 36u);
    out->source_forward = read_u32(data + 40u);
    out->flags = read_u32(data + 44u);
    out->normalized_height = read_f32(data + 48u);
    out->target_height = read_f32(data + 52u);
    return 1;
}

int mdkr_modern_character_asset_identity(
    const MdkrModernCharacterAsset *asset, MdkrModernIdentity *out,
    const uint8_t **portrait_data) {
    const uint8_t *data = record(asset, MDKR_MDKC_IDENTITY, 0u);
    const MdkrModernSectionView *media;
    uint64_t end;
    if (data == NULL || out == NULL || asset == NULL) return 0;
    media = &asset->sections[MDKR_MDKC_IDENTITY_DATA];
    out->flags = read_u32(data);
    out->portrait_mime = read_u32(data + 4u);
    out->portrait_offset = read_u32(data + 8u);
    out->portrait_size = read_u32(data + 12u);
    out->minimap_rgba = read_u32(data + 16u);
    out->short_name = read_u32(data + 20u);
    if (media->data == NULL ||
        add_overflow_u64(out->portrait_offset, out->portrait_size, &end) ||
        end > media->size) return 0;
    if (portrait_data != NULL) {
        *portrait_data = media->data + out->portrait_offset;
    }
    return 1;
}

int mdkr_modern_character_asset_identity_names(
    const MdkrModernCharacterAsset *asset, MdkrModernIdentityNames *out) {
    const uint8_t *data = record(asset, MDKR_MDKC_IDENTITY_NAMES, 0u);
    if (data == NULL || out == NULL) return 0;
    out->narration_name = read_u32(data);
    out->sort_label = read_u32(data + 4u);
    out->flags = read_u32(data + 8u);
    return 1;
}

int mdkr_modern_character_asset_rig(const MdkrModernCharacterAsset *asset,
                                    MdkrModernRig *out) {
    const uint8_t *data = record(asset, MDKR_MDKC_RIG, 0u);
    if (data == NULL || out == NULL) return 0;
    out->mode = read_u32(data);
    out->flags = read_u32(data + 4u);
    out->role_count = read_u32(data + 8u);
    out->role_mask = read_u32(data + 12u);
    return 1;
}

int mdkr_modern_character_asset_rig_role(
    const MdkrModernCharacterAsset *asset, uint32_t index,
    MdkrModernRigRole *out) {
    const uint8_t *data = record(asset, MDKR_MDKC_RIG_ROLES, index);
    unsigned component;
    if (data == NULL || out == NULL) return 0;
    out->semantic = read_u32(data);
    out->node = read_u32(data + 4u);
    out->flags = read_u32(data + 8u);
    out->confidence_milli = read_u32(data + 12u);
    for (component = 0u; component < 4u; component++) {
        out->rest_rotation[component] = read_f32(data + 16u + component * 4u);
    }
    for (component = 0u; component < 3u; component++) {
        out->bend_axis[component] = read_f32(data + 32u + component * 4u);
    }
    return 1;
}

int mdkr_modern_character_asset_joint_constraint(
    const MdkrModernCharacterAsset *asset, uint32_t index,
    MdkrModernJointConstraint *out) {
    const uint8_t *data = record(asset, MDKR_MDKC_JOINT_CONSTRAINTS, index);
    unsigned component;
    if (data == NULL || out == NULL) return 0;
    out->role = read_u32(data);
    out->node = read_u32(data + 4u);
    for (component = 0u; component < 3u; component++) {
        out->twist_axis[component] = read_f32(data + 8u + component * 4u);
    }
    out->swing_limit_degrees = read_f32(data + 20u);
    out->twist_min_degrees = read_f32(data + 24u);
    out->twist_max_degrees = read_f32(data + 28u);
    return 1;
}

int mdkr_modern_character_asset_secondary_chain(
    const MdkrModernCharacterAsset *asset, uint32_t index,
    MdkrModernSecondaryChain *out) {
    const uint8_t *data = record(asset, MDKR_MDKC_SECONDARY_CHAINS, index);
    unsigned component;
    if (data == NULL || out == NULL) return 0;
    out->name = read_u32(data);
    out->root_node = read_u32(data + 4u);
    out->first_joint = read_u32(data + 8u);
    out->joint_count = read_u32(data + 12u);
    out->stiffness_hz = read_f32(data + 16u);
    out->damping_ratio = read_f32(data + 20u);
    out->inertia = read_f32(data + 24u);
    out->max_angle_degrees = read_f32(data + 28u);
    for (component = 0u; component < 3u; component++) {
        out->bend_axis[component] = read_f32(data + 32u + component * 4u);
    }
    return 1;
}

int mdkr_modern_character_asset_secondary_joint(
    const MdkrModernCharacterAsset *asset, uint32_t index,
    MdkrModernSecondaryJoint *out) {
    const uint8_t *data = record(asset, MDKR_MDKC_SECONDARY_JOINTS, index);
    if (data == NULL || out == NULL) return 0;
    out->node = read_u32(data);
    out->chain = read_u32(data + 4u);
    out->order = read_u32(data + 8u);
    out->flags = read_u32(data + 12u);
    return 1;
}

int mdkr_modern_character_asset_provenance(
    const MdkrModernCharacterAsset *asset, MdkrModernProvenance *out) {
    const uint8_t *data = record(asset, MDKR_MDKC_PROVENANCE, 0u);
    if (data == NULL || out == NULL) return 0;
    out->spdx = read_u32(data);
    out->attribution = read_u32(data + 4u);
    out->source_url = read_u32(data + 8u);
    out->flags = read_u32(data + 12u);
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

static uint32_t rig_role_bit(const char *semantic) {
    static const char *roles[] = {
        "hips", "spine", "chest", "head",
        "upper_arm.left", "lower_arm.left", "hand.left",
        "upper_arm.right", "lower_arm.right", "hand.right",
        "upper_leg.left", "lower_leg.left", "foot.left",
        "upper_leg.right", "lower_leg.right", "foot.right"
    };
    uint32_t index;
    if (semantic == NULL) return 0u;
    for (index = 0u; index < 16u; index++) {
        if (strcmp(semantic, roles[index]) == 0) return 1u << index;
    }
    return 0u;
}

static int node_is_joint(const MdkrModernCharacterAsset *asset,
                         uint32_t node) {
    const MdkrModernSectionView *joints = &asset->sections[MDKR_MDKC_JOINTS];
    uint32_t index;
    for (index = 0u; index < joints->count; index++) {
        MdkrModernJoint joint;
        (void)mdkr_modern_character_asset_joint(asset, index, &joint);
        if (joint.node == node) return 1;
    }
    return 0;
}

static int node_is_ancestor(const MdkrModernCharacterAsset *asset,
                            uint32_t ancestor, uint32_t descendant) {
    const uint32_t count = asset->sections[MDKR_MDKC_NODES].count;
    uint32_t steps;
    for (steps = 0u; steps < count; steps++) {
        MdkrModernNode node;
        if (!mdkr_modern_character_asset_node(asset, descendant, &node) ||
            node.parent < 0) return 0;
        descendant = (uint32_t)node.parent;
        if (descendant == ancestor) return 1;
    }
    return 0;
}

static int node_world_orientation_preserving(
    const MdkrModernCharacterAsset *asset, uint32_t node_index) {
    const uint32_t count = asset->sections[MDKR_MDKC_NODES].count;
    double determinant = 1.0;
    uint32_t steps;
    for (steps = 0u; steps < count; steps++) {
        MdkrModernNode node;
        if (!mdkr_modern_character_asset_node(asset, node_index, &node)) {
            return 0;
        }
        determinant *= (double)node.scale[0] * (double)node.scale[1] *
                       (double)node.scale[2];
        if (!isfinite(determinant) || fabs(determinant) < 1.0e-12) return 0;
        if (node.parent < 0) return determinant > 0.0;
        node_index = (uint32_t)node.parent;
    }
    return 0;
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
    if (vertices->count > 1000000u || indices->count > 6000000u ||
        primitives->count > 512u || materials->count > 256u ||
        textures->count > 1024u || texture_data->count > 512u * 1024u * 1024u ||
        nodes->count > 16384u || skins->count > 256u || joints->count > 65536u ||
        animations->count > 256u || channels->count > 16384u ||
        keys->count > 4000000u ||
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
        const uint8_t *texture_bytes;
        (void)mdkr_modern_character_asset_texture(asset, index, &texture);
        if (mdkr_modern_character_asset_string(asset, texture.name) == NULL ||
            (texture.mime != 1u && texture.mime != 2u) || texture.data_size == 0u ||
            (texture.flags != 1u && texture.flags != 2u && texture.flags != 4u) ||
            !range_u32(texture.data_offset, texture.data_size, texture_data->count) ||
            (texture.dimensions != 0u &&
             ((texture.dimensions & 0xFFFFu) == 0u ||
              (texture.dimensions & 0xFFFFu) > 4096u ||
              (texture.dimensions >> 16u) == 0u ||
              (texture.dimensions >> 16u) > 4096u))) {
            set_error(error, error_size, "compiled texture is invalid");
            return 0;
        }
        texture_bytes = texture_data->data + texture.data_offset;
        if (texture.mime == 2u &&
            (texture.data_size < 44u ||
             memcmp(texture_bytes, "\xABKTX 20\xBB\r\n\x1A\n", 12u) != 0 ||
             read_u32(texture_bytes + 20u) !=
                 (texture.dimensions & 0xFFFFu) ||
             read_u32(texture_bytes + 24u) !=
                 (texture.dimensions >> 16u) ||
             read_u32(texture_bytes + 40u) == 0u ||
             read_u32(texture_bytes + 40u) > 13u)) {
            set_error(error, error_size, "compiled KTX2 texture is invalid");
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
            skin.joint_count == 0u || skin.joint_count > 256u ||
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
        const char *id;
        const char *display_name;
        const char *renderer_profile;
        float quaternion_length;
        (void)mdkr_modern_character_asset_definition(asset, &definition);
        quaternion_length = definition.rotation[0] * definition.rotation[0] +
                            definition.rotation[1] * definition.rotation[1] +
                            definition.rotation[2] * definition.rotation[2] +
                            definition.rotation[3] * definition.rotation[3];
        id = mdkr_modern_character_asset_string(asset, definition.id);
        display_name = mdkr_modern_character_asset_string(
            asset, definition.display_name);
        renderer_profile = mdkr_modern_character_asset_string(
            asset, definition.renderer_profile);
        if (!compiled_id_valid(id) ||
            !bounded_printable_utf8(display_name, 96u) ||
            renderer_profile == NULL ||
            strcmp(renderer_profile, "modern-skeletal-v1") != 0 ||
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
    {
        uint32_t fallback_count = 0u;
        for (index = 0u;
             index < asset->sections[MDKR_MDKC_SEMANTICS].count; index++) {
            MdkrModernSemantic semantic;
            const char *semantic_name;
            uint32_t previous_index;
            (void)mdkr_modern_character_asset_semantic(
                asset, index, &semantic);
            semantic_name = mdkr_modern_character_asset_string(
                asset, semantic.semantic);
            if (!compiled_semantic_valid(semantic_name) ||
                mdkr_modern_character_asset_string(
                    asset, semantic.clip) == NULL ||
                (semantic.flags & ~(MDKR_MODERN_SEMANTIC_LOOP |
                                    MDKR_MODERN_SEMANTIC_DISABLED)) != 0u ||
                (strcmp(semantic_name, "fallback") == 0 &&
                 (semantic.flags & MDKR_MODERN_SEMANTIC_DISABLED) != 0u) ||
                !isfinite(semantic.blend_seconds) ||
                semantic.blend_seconds < 0.0f ||
                semantic.blend_seconds > 5.0f) {
                set_error(error, error_size,
                          "compiled animation semantic is invalid");
                return 0;
            }
            if (strcmp(semantic_name, "fallback") == 0) fallback_count++;
            for (previous_index = 0u; previous_index < index;
                 ++previous_index) {
                MdkrModernSemantic previous;
                const char *previous_name;
                (void)mdkr_modern_character_asset_semantic(
                    asset, previous_index, &previous);
                previous_name = mdkr_modern_character_asset_string(
                    asset, previous.semantic);
                if (previous_name != NULL &&
                    strcmp(previous_name, semantic_name) == 0) {
                    set_error(error, error_size,
                              "compiled animation semantics are duplicated");
                    return 0;
                }
            }
        }
        if (fallback_count != 1u) {
            set_error(error, error_size,
                      "compiled animation fallback is missing or duplicated");
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
    if (asset->sections[MDKR_MDKC_ATTACHMENTS].data != NULL) {
        uint32_t context_mask = 0u;
        if (asset->sections[MDKR_MDKC_ATTACHMENTS].count == 0u ||
            asset->sections[MDKR_MDKC_ATTACHMENTS].count >
                MDKR_CHARACTER_CONTEXT_COUNT) {
            set_error(error, error_size,
                      "compiled attachment profile has an invalid count");
            return 0;
        }
        for (index = 0u;
             index < asset->sections[MDKR_MDKC_ATTACHMENTS].count; index++) {
            MdkrModernAttachment attachment;
            float quaternion_length;
            (void)mdkr_modern_character_asset_attachment(asset, index,
                                                         &attachment);
            quaternion_length =
                attachment.rotation[0] * attachment.rotation[0] +
                attachment.rotation[1] * attachment.rotation[1] +
                attachment.rotation[2] * attachment.rotation[2] +
                attachment.rotation[3] * attachment.rotation[3];
            if (attachment.context >= MDKR_CHARACTER_CONTEXT_COUNT ||
                (context_mask & (1u << attachment.context)) != 0u ||
                mdkr_modern_character_asset_string(asset, attachment.anchor) == NULL ||
                !finite_array(attachment.translation, 3u) ||
                !finite_array(attachment.rotation, 4u) ||
                !isfinite(attachment.scale) || attachment.scale < 0.1f ||
                attachment.scale > 5.0f || (attachment.flags & ~1u) != 0u ||
                quaternion_length < 0.999f || quaternion_length > 1.001f) {
                set_error(error, error_size,
                          "compiled attachment profile is invalid");
                return 0;
            }
            context_mask |= 1u << attachment.context;
        }
        if ((context_mask & (1u << MDKR_CHARACTER_CONTEXT_SELECT)) == 0u) {
            set_error(error, error_size,
                      "compiled attachment profile omits character select");
            return 0;
        }
    }
    if (asset->sections[MDKR_MDKC_CALIBRATION].data != NULL) {
        MdkrModernCalibration calibration;
        if (asset->sections[MDKR_MDKC_CALIBRATION].count != 1u ||
            !mdkr_modern_character_asset_calibration(asset, &calibration) ||
            !finite_array(calibration.bounds_min, 3u) ||
            !finite_array(calibration.bounds_max, 3u) ||
            !finite_array(calibration.ground, 3u) ||
            !isfinite(calibration.source_height) ||
            calibration.source_height <= 1.0e-6f ||
            !isfinite(calibration.normalized_height) ||
            calibration.normalized_height <= 1.0e-6f ||
            !isfinite(calibration.target_height) ||
            calibration.target_height < 0.1f ||
            calibration.target_height > 10.0f ||
            calibration.source_forward > 3u ||
            (calibration.flags & ~1u) != 0u) {
            set_error(error, error_size,
                      "compiled character calibration is invalid");
            return 0;
        }
        for (index = 0u; index < 3u; index++) {
            if (calibration.bounds_min[index] > calibration.bounds_max[index]) {
                set_error(error, error_size,
                          "compiled character calibration bounds are inverted");
                return 0;
            }
        }
    }
    if ((asset->sections[MDKR_MDKC_ATTACHMENTS].data == NULL) !=
        (asset->sections[MDKR_MDKC_CALIBRATION].data == NULL)) {
        set_error(error, error_size,
                  "compiled character calibration sections are incomplete");
        return 0;
    }
    if ((asset->sections[MDKR_MDKC_IDENTITY].data == NULL) !=
        (asset->sections[MDKR_MDKC_IDENTITY_DATA].data == NULL)) {
        set_error(error, error_size,
                  "compiled character identity sections are incomplete");
        return 0;
    }
    if (asset->sections[MDKR_MDKC_IDENTITY].data != NULL) {
        MdkrModernIdentity identity;
        MdkrModernIdentityNames identity_names;
        const uint8_t *portrait = NULL;
        const char *short_name;
        const char *narration_name;
        const char *sort_label;
        if (asset->sections[MDKR_MDKC_IDENTITY].count != 1u ||
            asset->sections[MDKR_MDKC_IDENTITY_DATA].size == 0u ||
            !mdkr_modern_character_asset_identity(asset, &identity,
                                                   &portrait) ||
            identity.flags != 1u || identity.portrait_mime != 1u ||
            identity.portrait_size == 0u ||
            identity.portrait_size > 8u * 1024u * 1024u || portrait == NULL ||
            (identity.minimap_rgba >> 24u) != 255u) {
            set_error(error, error_size,
                      "compiled character identity is invalid");
            return 0;
        }
        short_name = mdkr_modern_character_asset_string(asset,
                                                        identity.short_name);
        if (short_name == NULL ||
            (identity.short_name != 0u &&
             (short_name[0] == '\0' ||
              !bounded_printable_utf8(short_name, 96u)))) {
            set_error(error, error_size,
                      "compiled character identity name is invalid");
            return 0;
        }
        if (asset->sections[MDKR_MDKC_IDENTITY_NAMES].data != NULL &&
            (asset->sections[MDKR_MDKC_IDENTITY_NAMES].count != 1u ||
             !mdkr_modern_character_asset_identity_names(
                 asset, &identity_names) || identity_names.flags != 0u ||
             (narration_name = mdkr_modern_character_asset_string(
                 asset, identity_names.narration_name)) == NULL ||
             (identity_names.narration_name != 0u &&
              (narration_name[0] == '\0' ||
               !bounded_printable_utf8(narration_name, 96u))) ||
             (sort_label = mdkr_modern_character_asset_string(
                 asset, identity_names.sort_label)) == NULL ||
             (identity_names.sort_label != 0u &&
              (sort_label[0] == '\0' ||
               !bounded_printable_utf8(sort_label, 96u))))) {
            set_error(error, error_size,
                      "compiled character identity names are invalid");
            return 0;
        }
    } else if (asset->sections[MDKR_MDKC_IDENTITY_NAMES].data != NULL) {
        set_error(error, error_size,
                  "compiled character identity names have no identity media");
        return 0;
    }
    if ((asset->sections[MDKR_MDKC_RIG].data == NULL) !=
        (asset->sections[MDKR_MDKC_RIG_ROLES].data == NULL)) {
        set_error(error, error_size,
                  "compiled character rig sections are incomplete");
        return 0;
    }
    if (asset->sections[MDKR_MDKC_RIG].data != NULL) {
        static const uint8_t hierarchy[][2] = {
            {0u, 1u}, {1u, 2u}, {2u, 3u},
            {2u, 4u}, {4u, 5u}, {5u, 6u},
            {2u, 7u}, {7u, 8u}, {8u, 9u},
            {0u, 10u}, {10u, 11u}, {11u, 12u},
            {0u, 13u}, {13u, 14u}, {14u, 15u}
        };
        MdkrModernRig rig;
        uint32_t role_mask = 0u;
        uint32_t role_nodes[16] = {0};
        uint32_t role_index;
        if (asset->sections[MDKR_MDKC_RIG].count != 1u ||
            !mdkr_modern_character_asset_rig(asset, &rig) ||
            rig.mode > MDKR_MODERN_RIG_HUMANOID_RETARGET_V1 ||
            (rig.flags & ~MDKR_MODERN_RIG_REVIEWED) != 0u ||
            rig.role_count != asset->sections[MDKR_MDKC_RIG_ROLES].count ||
            rig.role_count > 16u || (rig.role_mask & ~0xFFFFu) != 0u) {
            set_error(error, error_size,
                      "compiled character rig header is invalid");
            return 0;
        }
        for (role_index = 0u; role_index < rig.role_count; role_index++) {
            MdkrModernRigRole role;
            const char *semantic;
            uint32_t bit;
            uint32_t slot = 0u;
            uint32_t prior;
            float rotation_length;
            float bend_length;
            (void)mdkr_modern_character_asset_rig_role(asset, role_index,
                                                       &role);
            semantic = mdkr_modern_character_asset_string(asset,
                                                          role.semantic);
            bit = rig_role_bit(semantic);
            if (bit != 0u) {
                while ((bit >> slot) != 1u) slot++;
            }
            rotation_length =
                role.rest_rotation[0] * role.rest_rotation[0] +
                role.rest_rotation[1] * role.rest_rotation[1] +
                role.rest_rotation[2] * role.rest_rotation[2] +
                role.rest_rotation[3] * role.rest_rotation[3];
            bend_length =
                role.bend_axis[0] * role.bend_axis[0] +
                role.bend_axis[1] * role.bend_axis[1] +
                role.bend_axis[2] * role.bend_axis[2];
            for (prior = 0u; prior < role_index; prior++) {
                MdkrModernRigRole previous;
                (void)mdkr_modern_character_asset_rig_role(asset, prior,
                                                           &previous);
                if (previous.node == role.node) break;
            }
            if (bit == 0u || (role_mask & bit) != 0u ||
                prior != role_index || role.node >= nodes->count ||
                !node_is_joint(asset, role.node) ||
                (role.flags & ~1u) != 0u || role.confidence_milli > 1000u ||
                !finite_array(role.rest_rotation, 4u) ||
                rotation_length < 0.999f || rotation_length > 1.001f ||
                !finite_array(role.bend_axis, 3u) ||
                (bend_length > 1.0e-12f &&
                 (bend_length < 0.999f || bend_length > 1.001f))) {
                set_error(error, error_size,
                          "compiled character rig role is invalid");
                return 0;
            }
            role_mask |= bit;
            role_nodes[slot] = role.node;
        }
        if (role_mask != rig.role_mask ||
            (rig.mode == MDKR_MODERN_RIG_HUMANOID_RETARGET_V1 &&
             role_mask != 0xFFFFu)) {
            set_error(error, error_size,
                      "compiled character rig role mask is invalid");
            return 0;
        }
        for (role_index = 0u;
             role_index < sizeof(hierarchy) / sizeof(hierarchy[0]);
             role_index++) {
            const uint32_t ancestor = hierarchy[role_index][0];
            const uint32_t descendant = hierarchy[role_index][1];
            const uint32_t pair = (1u << ancestor) | (1u << descendant);
            if ((role_mask & pair) == pair &&
                !node_is_ancestor(asset, role_nodes[ancestor],
                                  role_nodes[descendant])) {
                set_error(error, error_size,
                          "compiled character rig hierarchy is invalid");
                return 0;
            }
        }
    }
    if (asset->sections[MDKR_MDKC_JOINT_CONSTRAINTS].data != NULL) {
        const MdkrModernSectionView *constraints =
            &asset->sections[MDKR_MDKC_JOINT_CONSTRAINTS];
        MdkrModernRig constraint_rig;
        uint32_t constraint_mask = 0u;
        uint32_t constraint_index;
        if (asset->sections[MDKR_MDKC_RIG].data == NULL ||
            !mdkr_modern_character_asset_rig(asset, &constraint_rig) ||
            constraint_rig.mode != MDKR_MODERN_RIG_HUMANOID_RETARGET_V1 ||
            constraints->count > MDKR_MODERN_HUMANOID_ROLE_COUNT) {
            set_error(error, error_size,
                      "compiled character joint constraints require a humanoid rig");
            return 0;
        }
        for (constraint_index = 0u; constraint_index < constraints->count;
             constraint_index++) {
            MdkrModernJointConstraint constraint;
            uint32_t role_index;
            uint32_t bit;
            float axis_length;
            int mapped = 0;
            (void)mdkr_modern_character_asset_joint_constraint(
                asset, constraint_index, &constraint);
            bit = constraint.role < MDKR_MODERN_HUMANOID_ROLE_COUNT
                ? 1u << constraint.role : 0u;
            axis_length =
                constraint.twist_axis[0] * constraint.twist_axis[0] +
                constraint.twist_axis[1] * constraint.twist_axis[1] +
                constraint.twist_axis[2] * constraint.twist_axis[2];
            for (role_index = 0u;
                 role_index < asset->sections[MDKR_MDKC_RIG_ROLES].count;
                 role_index++) {
                MdkrModernRigRole role;
                const char *semantic;
                (void)mdkr_modern_character_asset_rig_role(
                    asset, role_index, &role);
                semantic = mdkr_modern_character_asset_string(
                    asset, role.semantic);
                if (rig_role_bit(semantic) == bit &&
                    role.node == constraint.node) {
                    mapped = 1;
                    break;
                }
            }
            if (bit == 0u || (constraint_mask & bit) != 0u || !mapped ||
                !finite_array(constraint.twist_axis, 3u) ||
                axis_length < 0.999f || axis_length > 1.001f ||
                !isfinite(constraint.swing_limit_degrees) ||
                constraint.swing_limit_degrees < 0.0f ||
                constraint.swing_limit_degrees > 180.0f ||
                !isfinite(constraint.twist_min_degrees) ||
                !isfinite(constraint.twist_max_degrees) ||
                constraint.twist_min_degrees < -180.0f ||
                constraint.twist_max_degrees > 180.0f ||
                constraint.twist_min_degrees > constraint.twist_max_degrees) {
                set_error(error, error_size,
                          "compiled character joint constraint is invalid");
                return 0;
            }
            constraint_mask |= bit;
        }
    }
    if ((asset->sections[MDKR_MDKC_SECONDARY_CHAINS].data == NULL) !=
        (asset->sections[MDKR_MDKC_SECONDARY_JOINTS].data == NULL)) {
        set_error(error, error_size,
                  "compiled character secondary-motion sections are incomplete");
        return 0;
    }
    if (asset->sections[MDKR_MDKC_SECONDARY_CHAINS].data != NULL) {
        const MdkrModernSectionView *chains =
            &asset->sections[MDKR_MDKC_SECONDARY_CHAINS];
        const MdkrModernSectionView *secondary_joints =
            &asset->sections[MDKR_MDKC_SECONDARY_JOINTS];
        uint32_t chain_index;
        uint32_t expected_first = 0u;
        if (chains->count == 0u || chains->count > 8u ||
            secondary_joints->count == 0u || secondary_joints->count > 64u) {
            set_error(error, error_size,
                      "compiled character secondary-motion bounds are invalid");
            return 0;
        }
        for (chain_index = 0u; chain_index < chains->count; chain_index++) {
            MdkrModernSecondaryChain chain;
            const char *name;
            uint32_t joint_index;
            uint32_t previous_node;
            float axis_length;
            (void)mdkr_modern_character_asset_secondary_chain(
                asset, chain_index, &chain);
            name = mdkr_modern_character_asset_string(asset, chain.name);
            axis_length =
                chain.bend_axis[0] * chain.bend_axis[0] +
                chain.bend_axis[1] * chain.bend_axis[1] +
                chain.bend_axis[2] * chain.bend_axis[2];
            if (!compiled_semantic_valid(name) ||
                chain.root_node >= nodes->count ||
                !node_world_orientation_preserving(asset, chain.root_node) ||
                chain.first_joint != expected_first ||
                chain.joint_count == 0u || chain.joint_count > 16u ||
                !range_u32(chain.first_joint, chain.joint_count,
                           secondary_joints->count) ||
                !isfinite(chain.stiffness_hz) || chain.stiffness_hz < 0.1f ||
                chain.stiffness_hz > 30.0f ||
                !isfinite(chain.damping_ratio) || chain.damping_ratio < 0.0f ||
                chain.damping_ratio > 2.0f ||
                !isfinite(chain.inertia) || chain.inertia < 0.0f ||
                chain.inertia > 1.0f ||
                !isfinite(chain.max_angle_degrees) ||
                chain.max_angle_degrees < 0.0f ||
                chain.max_angle_degrees > 90.0f ||
                !finite_array(chain.bend_axis, 3u) ||
                axis_length < 0.999f || axis_length > 1.001f) {
                set_error(error, error_size,
                          "compiled character secondary chain is invalid");
                return 0;
            }
            for (joint_index = 0u; joint_index < chain_index; joint_index++) {
                MdkrModernSecondaryChain previous_chain;
                const char *previous_name;
                (void)mdkr_modern_character_asset_secondary_chain(
                    asset, joint_index, &previous_chain);
                previous_name = mdkr_modern_character_asset_string(
                    asset, previous_chain.name);
                if (previous_name != NULL && strcmp(name, previous_name) == 0) {
                    set_error(error, error_size,
                              "compiled character secondary chain name is duplicated");
                    return 0;
                }
            }
            previous_node = chain.root_node;
            for (joint_index = 0u; joint_index < chain.joint_count;
                 joint_index++) {
                const uint32_t absolute = chain.first_joint + joint_index;
                MdkrModernSecondaryJoint joint;
                MdkrModernNode node;
                uint32_t prior;
                (void)mdkr_modern_character_asset_secondary_joint(
                    asset, absolute, &joint);
                if (joint.node >= nodes->count || !node_is_joint(asset, joint.node) ||
                    !node_world_orientation_preserving(asset, joint.node) ||
                    joint.chain != chain_index || joint.order != joint_index ||
                    joint.flags != 0u ||
                    !mdkr_modern_character_asset_node(asset, joint.node, &node) ||
                    node.parent < 0 || (uint32_t)node.parent != previous_node) {
                    set_error(error, error_size,
                              "compiled character secondary joint is invalid");
                    return 0;
                }
                for (prior = 0u; prior < absolute; prior++) {
                    MdkrModernSecondaryJoint previous;
                    (void)mdkr_modern_character_asset_secondary_joint(
                        asset, prior, &previous);
                    if (previous.node == joint.node) {
                        set_error(error, error_size,
                                  "compiled character secondary joint is reused");
                        return 0;
                    }
                }
                for (prior = 0u;
                     prior < asset->sections[MDKR_MDKC_RIG_ROLES].count;
                     prior++) {
                    MdkrModernRigRole role;
                    (void)mdkr_modern_character_asset_rig_role(
                        asset, prior, &role);
                    if (role.node == joint.node) {
                        set_error(error, error_size,
                                  "compiled character secondary joint overlaps its rig");
                        return 0;
                    }
                }
                previous_node = joint.node;
            }
            expected_first += chain.joint_count;
        }
        if (expected_first != secondary_joints->count) {
            set_error(error, error_size,
                      "compiled character secondary joints are unreferenced");
            return 0;
        }
    }
    if (asset->sections[MDKR_MDKC_PROVENANCE].data != NULL) {
        MdkrModernProvenance provenance;
        const char *spdx;
        const char *attribution;
        const char *source_url;
        if (asset->sections[MDKR_MDKC_PROVENANCE].count != 1u ||
            !mdkr_modern_character_asset_provenance(asset, &provenance) ||
            provenance.flags != MDKR_MODERN_PROVENANCE_LICENSE_TEXT_BOUND ||
            (spdx = mdkr_modern_character_asset_string(
                asset, provenance.spdx)) == NULL || spdx[0] == '\0' ||
            !bounded_printable_utf8(spdx, 128u) ||
            (attribution = mdkr_modern_character_asset_string(
                asset, provenance.attribution)) == NULL ||
            !bounded_printable_utf8(attribution, 256u) ||
            (source_url = mdkr_modern_character_asset_string(
                asset, provenance.source_url)) == NULL ||
            !bounded_printable_utf8(source_url, 2048u)) {
            set_error(error, error_size,
                      "compiled character provenance is invalid");
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
    for (section_index = 1u; section_index <= MDKR_MDKC_SOCKETS; section_index++) {
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
    const MdkrModernSectionView *texture_data;
    uint32_t primitive_index;
    uint32_t texture_index;
    if (out == NULL) return;
    memset(out, 0, sizeof(*out));
    if (asset == NULL || asset->owned_bytes == NULL) return;
    out->vertices = asset->sections[MDKR_MDKC_VERTICES].count;
    out->triangles = asset->sections[MDKR_MDKC_INDICES].count / 3u;
    out->primitives = asset->sections[MDKR_MDKC_PRIMITIVES].count;
    for (primitive_index = 0u; primitive_index < out->primitives;
         primitive_index++) {
        MdkrModernPrimitive primitive;
        if (mdkr_modern_character_asset_primitive(
                asset, primitive_index, &primitive) &&
            primitive.lod + 1u > out->lod_levels) {
            out->lod_levels = primitive.lod + 1u;
        }
    }
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
    out->rig_roles = asset->sections[MDKR_MDKC_RIG_ROLES].count;
    out->joint_constraints =
        asset->sections[MDKR_MDKC_JOINT_CONSTRAINTS].count;
    out->secondary_chains =
        asset->sections[MDKR_MDKC_SECONDARY_CHAINS].count;
    out->secondary_joints =
        asset->sections[MDKR_MDKC_SECONDARY_JOINTS].count;
    texture_data = &asset->sections[MDKR_MDKC_TEXTURE_DATA];
    out->encoded_texture_bytes = texture_data->size;
    for (texture_index = 0u; texture_index < out->textures; texture_index++) {
        MdkrModernTexture texture;
        uint32_t width;
        uint32_t height;
        uint32_t levels = 0u;
        uint32_t level;
        if (!mdkr_modern_character_asset_texture(asset, texture_index,
                                                 &texture)) {
            continue;
        }
        if (texture.mime == 2u) {
            if (texture_data->data == NULL || texture.data_size < 44u ||
                !range_u32(texture.data_offset, texture.data_size,
                           texture_data->count) ||
                (uint64_t)texture.data_offset + texture.data_size >
                    texture_data->size) {
                continue;
            }
            levels = read_u32(texture_data->data +
                              texture.data_offset + 40u);
            /* levelCount is attacker-controlled. A KTX2 file stores a level
             * index of 24 bytes per level immediately after its 80-byte
             * header, and one entry is present even when levelCount is zero.
             * A count whose index table does not fit the payload, or that
             * exceeds the bounded transcoder's level array, describes a table
             * this reader would have to walk past the end of the section. */
            if (levels > MDKR_KTX2_LEVEL_MAX ||
                (uint64_t)80u +
                        (uint64_t)(levels != 0u ? levels : 1u) * 24u >
                    (uint64_t)texture.data_size) {
                continue;
            }
            out->ktx2_textures++;
            out->ktx2_source_bytes += texture.data_size;
        }
        width = texture.dimensions & 0xFFFFu;
        height = texture.dimensions >> 16u;
        for (level = 0u;
             width != 0u && height != 0u &&
             (levels == 0u || level < levels);
             ++level) {
            out->decoded_texture_bytes +=
                (uint64_t)width * (uint64_t)height * 4u;
            if (width == 1u && height == 1u) break;
            if (width > 1u) width >>= 1u;
            if (height > 1u) height >>= 1u;
        }
    }
}
