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
    uint32_t *evaluation_order;
    float *world_previous; /* node_count column-major mat4 values */
    float *world_current;
    uint32_t node_count;
    int32_t animation;
    float time;
    float blend_elapsed;
    float blend_duration;
    uint32_t animation_flags;
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
