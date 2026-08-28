/* Presentation-only skeletal animation for validated modern characters. */
#ifndef MDKR64_MODERN_CHARACTER_POSE_H
#define MDKR64_MODERN_CHARACTER_POSE_H

#include "modern_character_asset.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MdkrModernTrs {
    float translation[3];
    float rotation[4];
    float scale[3];
} MdkrModernTrs;

typedef struct MdkrModernPose {
    const MdkrModernCharacterAsset *asset;
    MdkrModernTrs *local;
    MdkrModernTrs *blend_from;
    MdkrModernTrs *sampled;
    MdkrModernTrs *pre_contact;
    uint32_t *evaluation_order;
    float *world_previous; /* node_count column-major mat4 values */
    float *world_current;
    uint32_t node_count;
    int32_t animation;
    float time;
    float blend_elapsed;
    float blend_duration;
    uint32_t animation_flags;
    uint32_t rig_role_nodes[MDKR_MODERN_HUMANOID_ROLE_COUNT];
    float rig_rest_rotation[MDKR_MODERN_HUMANOID_ROLE_COUNT][4];
    float rig_bend_axis[MDKR_MODERN_HUMANOID_ROLE_COUNT][3];
    uint32_t rig_role_mask;
    float procedural_time;
    float procedural_weight;
    float normalized_phase;
    char requested_semantic[65];
    int requested_semantic_explicit;
    int humanoid_retarget_ready;
    uint64_t generation;
    uint64_t contact_generation;
    uint32_t contact_context;
    float contact_max_error;
    /* Exact post-solve chain witnesses in source/model space. The runtime
     * publishes them only after applying the same target-frame transform as
     * a successful replacement draw. Stable order is left hand, right hand,
     * left foot, right foot. */
    float contact_chain_root[MDKR_MODERN_CHARACTER_CONTACTS][3];
    float contact_bend[MDKR_MODERN_CHARACTER_CONTACTS][3];
    float contact_target[MDKR_MODERN_CHARACTER_CONTACTS][3];
    float contact_end[MDKR_MODERN_CHARACTER_CONTACTS][3];
    float contact_error[MDKR_MODERN_CHARACTER_CONTACTS];
    uint32_t contact_valid_mask;
    int contacts_initialized;
    int valid;
} MdkrModernPose;

int mdkr_modern_pose_init(MdkrModernPose *pose,
                          const MdkrModernCharacterAsset *asset,
                          char *error, size_t error_size);
void mdkr_modern_pose_shutdown(MdkrModernPose *pose);

/* Select by public semantic ("race.steer", "select.idle", ...). Missing
 * semantics use the package fallback. Returns zero only when neither resolves. */
int mdkr_modern_pose_set_semantic(MdkrModernPose *pose, const char *semantic,
                                  char *error, size_t error_size);
/* Advances presentation time only; no gameplay state is read or mutated. */
int mdkr_modern_pose_advance(MdkrModernPose *pose, float seconds,
                             char *error, size_t error_size);

/* Advance blending by `seconds` while sampling the current clip at an
 * engine-owned normalized phase. Used by `race.steer`: authors place full-left
 * at 0, neutral at 0.5, and full-right at 1. Missing semantic mappings are not
 * phase-driven by the runtime, so a fallback idle clip keeps playing normally. */
int mdkr_modern_pose_advance_phase(MdkrModernPose *pose, float seconds,
                                   float normalized_phase,
                                   char *error, size_t error_size);

/* True only for an explicitly authored mapping; fallback does not count. */
int mdkr_modern_pose_has_semantic(const MdkrModernPose *pose,
                                  const char *semantic);

/* True only for a complete, author-reviewed source-v4 humanoid role map. */
int mdkr_modern_pose_humanoid_retarget_ready(const MdkrModernPose *pose);

/* Measures each reviewed semantic joint's current node-local rotation against
 * its compiled bind rotation. Values are the shortest quaternion angular
 * distance in degrees (0..180), after reference motion and contact solving.
 * This is exact inspection evidence, not an anatomical pass/fail or a clamp.
 * Output changes only after every mapped role validates. */
int mdkr_modern_pose_joint_excursions(
    const MdkrModernPose *pose,
    float degrees[MDKR_MODERN_HUMANOID_ROLE_COUNT],
    uint32_t *valid_mask);

/* Applies bounded presentation-only hand/foot contact solving for one vehicle
 * context. Target offsets are engine-owned and converted into source space by
 * the validated calibration. Repeating a context in one pose generation is
 * idempotent. Authored-clips-only and unreviewed rigs are unchanged. */
int mdkr_modern_pose_apply_vehicle_contacts(
    MdkrModernPose *pose, MdkrModernCharacterContext context,
    const MdkrModernCalibration *calibration,
    const float contact_offsets[MDKR_MODERN_CHARACTER_CONTACTS][3],
    char *error, size_t error_size);

const float *mdkr_modern_pose_node_matrix(const MdkrModernPose *pose,
                                          uint32_t node, int previous);
int mdkr_modern_pose_socket_matrix(const MdkrModernPose *pose,
                                   const char *semantic, int previous,
                                   float output[16]);

/* Build glTF skin matrices for one primitive's {skin, mesh node}. `output`
 * receives joint_count column-major matrices. This mesh-relative palette is
 * what a shader multiplies against the primitive's local POSITION. */
int mdkr_modern_pose_skin_palette(const MdkrModernPose *pose,
                                  uint32_t skin, uint32_t mesh_node,
                                  int previous, float *output,
                                  size_t output_matrices,
                                  char *error, size_t error_size);

#ifdef __cplusplus
}
#endif

#endif /* MDKR64_MODERN_CHARACTER_POSE_H */
