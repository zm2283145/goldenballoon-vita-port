/* Validated runtime view of a compiled custom-character cache (.mdkc).
 *
 * This is the only custom-character file format the engine consumes. GLB and
 * manifest JSON stop at the offline compiler. Every offset, count, reference,
 * float, and checksum is checked before a view is published; callers never
 * retain pointers into an unvalidated package or source archive.
 */
#ifndef MDKR64_MODERN_CHARACTER_ASSET_H
#define MDKR64_MODERN_CHARACTER_ASSET_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MDKR_MDKC_VERSION 2u
#define MDKR_MDKC_HEADER_BYTES 928u
#define MDKR_MDKC_SECTION_SLOTS 27u
#define MDKR_MDKC_FILE_MAX (1024u * 1024u * 1024u)

/* Admission ceiling for the encoded texture-data section.
 *
 * The renderer hands a slice of this section straight to stb_image, whose
 * length argument is an `int`. That is a contract between two constants, not
 * a comment: this ceiling must fit an int, and the renderer's own decode
 * ceiling must be the same number. Both relationships are enforced by
 * _Static_assert at the sites that depend on them (modern_character_asset.c
 * and modern_character_render.c), so raising one without the other fails to
 * compile rather than truncating a length at run time. */
#define MDKR_MODERN_TEXTURE_DATA_BYTES_MAX (512u * 1024u * 1024u)
#define MDKR_MODERN_HUMANOID_ROLE_COUNT 16u

typedef enum MdkrModernSectionType {
    MDKR_MDKC_STRINGS = 1,
    MDKR_MDKC_VERTICES = 2,
    MDKR_MDKC_INDICES = 3,
    MDKR_MDKC_PRIMITIVES = 4,
    MDKR_MDKC_MATERIALS = 5,
    MDKR_MDKC_TEXTURES = 6,
    MDKR_MDKC_TEXTURE_DATA = 7,
    MDKR_MDKC_NODES = 8,
    MDKR_MDKC_SKINS = 9,
    MDKR_MDKC_JOINTS = 10,
    MDKR_MDKC_ANIMATIONS = 11,
    MDKR_MDKC_CHANNELS = 12,
    MDKR_MDKC_KEYS = 13,
    MDKR_MDKC_CHARACTER = 14,
    MDKR_MDKC_SEMANTICS = 15,
    MDKR_MDKC_SOCKETS = 16,
    MDKR_MDKC_ATTACHMENTS = 17,
    MDKR_MDKC_CALIBRATION = 18,
    MDKR_MDKC_IDENTITY = 19,
    MDKR_MDKC_IDENTITY_DATA = 20,
    MDKR_MDKC_RIG = 21,
    MDKR_MDKC_RIG_ROLES = 22,
    MDKR_MDKC_PROVENANCE = 23,
    MDKR_MDKC_IDENTITY_NAMES = 24,
    MDKR_MDKC_JOINT_CONSTRAINTS = 25,
    MDKR_MDKC_SECONDARY_CHAINS = 26,
    MDKR_MDKC_SECONDARY_JOINTS = 27,
    MDKR_MDKC_SECTION_LAST = MDKR_MDKC_SECONDARY_JOINTS
} MdkrModernSectionType;

typedef struct MdkrModernSectionView {
    const uint8_t *data;
    uint64_t size;
    uint32_t count;
    uint32_t stride;
    uint32_t flags;
} MdkrModernSectionView;

typedef struct MdkrModernVertex {
    float position[3];
    float normal[3];
    float tangent[4];
    float uv[2];
    uint16_t joints[4];
    float weights[4];
} MdkrModernVertex;

typedef struct MdkrModernPrimitive {
    uint32_t first_vertex;
    uint32_t vertex_count;
    uint32_t first_index;
    uint32_t index_count;
    int32_t material;
    uint32_t node;
    int32_t skin;
    uint32_t lod;
} MdkrModernPrimitive;

typedef struct MdkrModernMaterial {
    uint32_t name;
    uint32_t flags;
    int32_t textures[5]; /* base, metallic/roughness, normal, occlusion, emissive */
    float base_color[4];
    float emissive[3];
    float metallic;
    float roughness;
    float normal_scale;
    float occlusion_strength;
    float alpha_cutoff;
} MdkrModernMaterial;

typedef struct MdkrModernTexture {
    uint32_t name;
    uint32_t mime; /* 1 PNG, 2 Basis Universal KTX2 */
    uint32_t data_offset;
    uint32_t data_size;
    int32_t wrap_s;
    int32_t wrap_t;
    int32_t min_filter;
    int32_t mag_filter;
    uint32_t flags;
    uint32_t dimensions; /* width low 16, height high 16; zero in legacy caches */
} MdkrModernTexture;

typedef struct MdkrModernNode {
    uint32_t name;
    int32_t parent;
    float translation[3];
    float rotation[4];
    float scale[3];
} MdkrModernNode;

typedef struct MdkrModernSkin {
    uint32_t name;
    uint32_t first_joint;
    uint32_t joint_count;
    uint32_t skeleton;
} MdkrModernSkin;

typedef struct MdkrModernJoint {
    uint32_t node;
    float inverse_bind[16];
} MdkrModernJoint;

typedef struct MdkrModernAnimation {
    uint32_t name;
    float duration;
    uint32_t first_channel;
    uint32_t channel_count;
} MdkrModernAnimation;

typedef struct MdkrModernChannel {
    uint32_t node;
    uint32_t path;          /* translation, rotation, scale, weights */
    uint32_t interpolation; /* linear, step, cubic spline */
    uint32_t first_key;
    uint32_t key_count;
    uint32_t components;
} MdkrModernChannel;

typedef struct MdkrModernKey {
    float time;
    float value[4];
    float incoming[4];
    float outgoing[4];
} MdkrModernKey;

typedef struct MdkrModernCharacterDefinition {
    uint32_t id;
    uint32_t display_name;
    uint32_t donor;
    uint32_t renderer_profile;
    float scale[3];
    float translation[3];
    float rotation[4];
    float lod_bias;
    uint32_t vehicle_mask;
} MdkrModernCharacterDefinition;

typedef struct MdkrModernSemantic {
    uint32_t semantic;
    uint32_t clip;
    uint32_t flags;
    float blend_seconds;
} MdkrModernSemantic;

enum MdkrModernSemanticFlags {
    MDKR_MODERN_SEMANTIC_LOOP = 1u << 0,
    /* The authored mapping and clip remain in the authenticated source/cache,
     * but runtime selection treats this semantic as intentionally unmapped. */
    MDKR_MODERN_SEMANTIC_DISABLED = 1u << 1,
};

typedef struct MdkrModernSocket {
    uint32_t semantic;
    uint32_t node;
} MdkrModernSocket;

typedef enum MdkrModernCharacterContext {
    MDKR_CHARACTER_CONTEXT_SELECT = 0,
    MDKR_CHARACTER_CONTEXT_CAR = 1,
    MDKR_CHARACTER_CONTEXT_HOVERCRAFT = 2,
    MDKR_CHARACTER_CONTEXT_PLANE = 3,
    MDKR_CHARACTER_CONTEXT_COUNT = 4
} MdkrModernCharacterContext;

/* Stable authoring/runtime order for vehicle contact adjustments. Keep this
 * in the lightweight asset contract so tools and launcher UI do not need the
 * renderer-facing runtime header merely to share the semantic identities. */
typedef enum MdkrModernCharacterContact {
    MDKR_CHARACTER_CONTACT_HAND_LEFT = 0,
    MDKR_CHARACTER_CONTACT_HAND_RIGHT = 1,
    MDKR_CHARACTER_CONTACT_FOOT_LEFT = 2,
    MDKR_CHARACTER_CONTACT_FOOT_RIGHT = 3,
    MDKR_MODERN_CHARACTER_CONTACTS = 4
} MdkrModernCharacterContact;

/* Package-authored adjustment in the target context's coordinate system.
 * `anchor` names either the synthetic `ground` anchor or a socket such as
 * `seat`. Flags bit zero identifies a fixed ground anchor. */
typedef struct MdkrModernAttachment {
    uint32_t context;
    uint32_t anchor;
    float translation[3];
    float rotation[4];
    float scale;
    uint32_t flags;
} MdkrModernAttachment;

typedef struct MdkrModernCalibration {
    float bounds_min[3];
    float bounds_max[3];
    float ground[3];
    float source_height;
    uint32_t source_forward;
    uint32_t flags; /* bit zero: explicit calibrated source profile */
    float normalized_height;
    float target_height;
} MdkrModernCalibration;

/* Optional source-v3/v4/v5 presentation identity. The encoded portrait remains
 * immutable cache data; a bounded runtime adapter owns decoded pixels. */
typedef struct MdkrModernIdentity {
    uint32_t flags; /* bit zero: authored portrait and minimap colour */
    uint32_t portrait_mime; /* 1 = PNG */
    uint32_t portrait_offset;
    uint32_t portrait_size;
    uint32_t minimap_rgba; /* R in least-significant byte */
    uint32_t short_name; /* reserved string offset; zero means display_name */
} MdkrModernIdentity;

typedef struct MdkrModernIdentityNames {
    uint32_t narration_name; /* zero means display_name */
    uint32_t sort_label; /* zero means display_name */
    uint32_t flags; /* reserved; must be zero */
} MdkrModernIdentityNames;

typedef enum MdkrModernRigMode {
    MDKR_MODERN_RIG_AUTHORED_CLIPS_ONLY = 0,
    MDKR_MODERN_RIG_HUMANOID_RETARGET_V1 = 1
} MdkrModernRigMode;

enum MdkrModernRigFlags {
    MDKR_MODERN_RIG_REVIEWED = 1u << 0
};

/* Optional source-v4/v5 semantic skeleton contract. A reviewed humanoid map is
 * structurally ready for a retargeter; authored-clips-only remains a complete,
 * supported mode and never asks the runtime to distort a non-humanoid rig. */
typedef struct MdkrModernRig {
    uint32_t mode;
    uint32_t flags;
    uint32_t role_count;
    uint32_t role_mask;
} MdkrModernRig;

typedef struct MdkrModernRigRole {
    uint32_t semantic;
    uint32_t node;
    uint32_t flags; /* bit zero: mapping was inferred */
    uint32_t confidence_milli;
    float rest_rotation[4];
    float bend_axis[3];
} MdkrModernRigRole;

/* Optional source-v5 cone/twist limit, expressed relative to the mapped
 * role node's GLB bind rotation in node-local coordinates. */
typedef struct MdkrModernJointConstraint {
    uint32_t role;
    uint32_t node;
    float twist_axis[3];
    float swing_limit_degrees;
    float twist_min_degrees;
    float twist_max_degrees;
} MdkrModernJointConstraint;

/* Bounded source-v5 inertial chain. The root follows the ordinary evaluated
 * pose; only the contiguous records beginning at first_joint are dynamic. */
typedef struct MdkrModernSecondaryChain {
    uint32_t name;
    uint32_t root_node;
    uint32_t first_joint;
    uint32_t joint_count;
    float stiffness_hz;
    float damping_ratio;
    float inertia;
    float max_angle_degrees;
    float bend_axis[3];
} MdkrModernSecondaryChain;

typedef struct MdkrModernSecondaryJoint {
    uint32_t node;
    uint32_t chain;
    uint32_t order;
    uint32_t flags;
} MdkrModernSecondaryJoint;

enum MdkrModernProvenanceFlags {
    MDKR_MODERN_PROVENANCE_LICENSE_TEXT_BOUND = 1u << 0
};

/* Optional compiler-v5 source metadata. The cache source digest separately
 * authenticates the complete LICENSE.txt bytes; these bounded string offsets
 * make the manifest's human-readable provenance available without JSON. */
typedef struct MdkrModernProvenance {
    uint32_t spdx;
    uint32_t attribution;
    uint32_t source_url;
    uint32_t flags;
} MdkrModernProvenance;

typedef struct MdkrModernCharacterAsset {
    uint8_t *owned_bytes;
    size_t size;
    uint8_t source_sha256[32];
    MdkrModernSectionView sections[MDKR_MDKC_SECTION_LAST + 1];
} MdkrModernCharacterAsset;

typedef struct MdkrModernCharacterStats {
    uint32_t vertices;
    uint32_t triangles;
    uint32_t primitives;
    uint32_t lod_levels;
    uint32_t materials;
    uint32_t textures;
    uint32_t nodes;
    uint32_t skins;
    uint32_t joints;
    uint32_t animations;
    uint32_t animation_channels;
    uint32_t animation_keys;
    uint32_t semantics;
    uint32_t sockets;
    uint32_t rig_roles;
    uint32_t joint_constraints;
    uint32_t secondary_chains;
    uint32_t secondary_joints;
    uint64_t encoded_texture_bytes;
    uint64_t decoded_texture_bytes;
    uint32_t ktx2_textures;
    uint64_t ktx2_source_bytes;
} MdkrModernCharacterStats;

/* Copies and validates `bytes`; a failed load leaves `out` empty. */
int mdkr_modern_character_asset_load_memory(const void *bytes, size_t size,
                                            MdkrModernCharacterAsset *out,
                                            char *error, size_t error_size);
/* Reads through the port's UTF-8 filesystem boundary, then applies the same
 * validator. The file-size cap is checked before allocation. */
int mdkr_modern_character_asset_load_file(const char *path,
                                          MdkrModernCharacterAsset *out,
                                          char *error, size_t error_size);
void mdkr_modern_character_asset_unload(MdkrModernCharacterAsset *asset);

const MdkrModernSectionView *mdkr_modern_character_asset_section(
    const MdkrModernCharacterAsset *asset, MdkrModernSectionType type);
const char *mdkr_modern_character_asset_string(
    const MdkrModernCharacterAsset *asset, uint32_t offset);

int mdkr_modern_character_asset_vertex(const MdkrModernCharacterAsset *asset,
                                       uint32_t index, MdkrModernVertex *out);
int mdkr_modern_character_asset_primitive(const MdkrModernCharacterAsset *asset,
                                          uint32_t index, MdkrModernPrimitive *out);
int mdkr_modern_character_asset_material(const MdkrModernCharacterAsset *asset,
                                         uint32_t index, MdkrModernMaterial *out);
int mdkr_modern_character_asset_texture(const MdkrModernCharacterAsset *asset,
                                        uint32_t index, MdkrModernTexture *out);
int mdkr_modern_character_asset_node(const MdkrModernCharacterAsset *asset,
                                     uint32_t index, MdkrModernNode *out);
/* Bind-pose spatial queries shared by authoring diagnostics and tests. The
 * position follows the same glTF parent TRS convention as pose evaluation.
 * Rotation is the normalized node-local-to-source-model quaternion composed
 * through the complete parent hierarchy; scale and translation do not affect
 * that orientation query.
 * Joint parent returns the node index of the nearest skin-joint ancestor,
 * skipping helpers, or -1 for a skin root. */
int mdkr_modern_character_asset_node_bind_position(
    const MdkrModernCharacterAsset *asset, uint32_t node, float output[3]);
int mdkr_modern_character_asset_node_bind_rotation(
    const MdkrModernCharacterAsset *asset, uint32_t node, float output[4]);
int mdkr_modern_character_asset_joint_parent_node(
    const MdkrModernCharacterAsset *asset, uint32_t joint,
    int32_t *parent_node);
int mdkr_modern_character_asset_skin(const MdkrModernCharacterAsset *asset,
                                     uint32_t index, MdkrModernSkin *out);
int mdkr_modern_character_asset_joint(const MdkrModernCharacterAsset *asset,
                                      uint32_t index, MdkrModernJoint *out);
int mdkr_modern_character_asset_animation(const MdkrModernCharacterAsset *asset,
                                          uint32_t index, MdkrModernAnimation *out);
int mdkr_modern_character_asset_channel(const MdkrModernCharacterAsset *asset,
                                        uint32_t index, MdkrModernChannel *out);
int mdkr_modern_character_asset_key(const MdkrModernCharacterAsset *asset,
                                    uint32_t index, MdkrModernKey *out);
int mdkr_modern_character_asset_definition(
    const MdkrModernCharacterAsset *asset, MdkrModernCharacterDefinition *out);
int mdkr_modern_character_asset_semantic(const MdkrModernCharacterAsset *asset,
                                         uint32_t index, MdkrModernSemantic *out);
int mdkr_modern_character_asset_identity(
    const MdkrModernCharacterAsset *asset, MdkrModernIdentity *out,
    const uint8_t **portrait_data);
int mdkr_modern_character_asset_identity_names(
    const MdkrModernCharacterAsset *asset, MdkrModernIdentityNames *out);
int mdkr_modern_character_asset_rig(const MdkrModernCharacterAsset *asset,
                                    MdkrModernRig *out);
int mdkr_modern_character_asset_rig_role(
    const MdkrModernCharacterAsset *asset, uint32_t index,
    MdkrModernRigRole *out);
int mdkr_modern_character_asset_joint_constraint(
    const MdkrModernCharacterAsset *asset, uint32_t index,
    MdkrModernJointConstraint *out);
int mdkr_modern_character_asset_secondary_chain(
    const MdkrModernCharacterAsset *asset, uint32_t index,
    MdkrModernSecondaryChain *out);
int mdkr_modern_character_asset_secondary_joint(
    const MdkrModernCharacterAsset *asset, uint32_t index,
    MdkrModernSecondaryJoint *out);
int mdkr_modern_character_asset_provenance(
    const MdkrModernCharacterAsset *asset, MdkrModernProvenance *out);
int mdkr_modern_character_asset_socket(const MdkrModernCharacterAsset *asset,
                                       uint32_t index, MdkrModernSocket *out);
int mdkr_modern_character_asset_attachment(
    const MdkrModernCharacterAsset *asset, uint32_t index,
    MdkrModernAttachment *out);
int mdkr_modern_character_asset_calibration(
    const MdkrModernCharacterAsset *asset, MdkrModernCalibration *out);

void mdkr_modern_character_asset_stats(const MdkrModernCharacterAsset *asset,
                                       MdkrModernCharacterStats *out);

#ifdef __cplusplus
}
#endif

#endif /* MDKR64_MODERN_CHARACTER_ASSET_H */
