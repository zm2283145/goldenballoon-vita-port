/* Bounded discovery of locally compiled generic characters.
 *
 * Discovery validates each cache, copies only its small identity/stat summary,
 * and unloads the heavy mesh/texture data. Activation validates and loads one
 * selected cache again. Thus startup cannot retain N high-poly characters, and
 * no renderer pointer survives a rescan or package removal.
 */
#ifndef MDKR64_MODERN_CHARACTER_REGISTRY_H
#define MDKR64_MODERN_CHARACTER_REGISTRY_H

#include "modern_character_asset.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MDKR_MODERN_CHARACTER_MAX 64
#define MDKR_MODERN_CHARACTER_ID_MAX 65
#define MDKR_MODERN_CHARACTER_NAME_MAX 97
#define MDKR_MODERN_CHARACTER_PATH_MAX 4096
#define MDKR_MODERN_CHARACTER_SKIP_REASON_MAX 192

enum MdkrModernCharacterSemanticBits {
    MDKR_CHARACTER_SEMANTIC_FALLBACK = 1u << 0,
    MDKR_CHARACTER_SEMANTIC_RACE_STEER = 1u << 1,
    MDKR_CHARACTER_SEMANTIC_RACE_REVERSE = 1u << 2,
    MDKR_CHARACTER_SEMANTIC_RACE_BOOST = 1u << 3,
    MDKR_CHARACTER_SEMANTIC_RACE_DAMAGE = 1u << 4,
    MDKR_CHARACTER_SEMANTIC_RACE_ITEM = 1u << 5,
    MDKR_CHARACTER_SEMANTIC_RACE_SPIN = 1u << 6,
    MDKR_CHARACTER_SEMANTIC_RACE_AIRBORNE = 1u << 7,
    MDKR_CHARACTER_SEMANTIC_RACE_LAND = 1u << 8,
    MDKR_CHARACTER_SEMANTIC_RACE_FINISH_WIN = 1u << 9,
    MDKR_CHARACTER_SEMANTIC_RACE_FINISH_LOSE = 1u << 10,
    MDKR_CHARACTER_SEMANTIC_SELECT_IDLE = 1u << 11,
    MDKR_CHARACTER_SEMANTIC_SELECT_HOVER = 1u << 12,
    MDKR_CHARACTER_SEMANTIC_SELECT_CONFIRM = 1u << 13,
};

enum MdkrModernCharacterSocketBits {
    MDKR_CHARACTER_SOCKET_SEAT = 1u << 0,
    MDKR_CHARACTER_SOCKET_HEAD = 1u << 1,
    MDKR_CHARACTER_SOCKET_HAND = 1u << 2,
    MDKR_CHARACTER_SOCKET_HAND_LEFT = 1u << 3,
    MDKR_CHARACTER_SOCKET_HAND_RIGHT = 1u << 4,
    MDKR_CHARACTER_SOCKET_FOOT_LEFT = 1u << 5,
    MDKR_CHARACTER_SOCKET_FOOT_RIGHT = 1u << 6,
};

typedef struct MdkrModernCharacterEntry {
    char id[MDKR_MODERN_CHARACTER_ID_MAX];
    char display_name[MDKR_MODERN_CHARACTER_NAME_MAX];
    char path[MDKR_MODERN_CHARACTER_PATH_MAX];
    uint8_t source_sha256[32];
    uint32_t donor;
    uint32_t vehicle_mask;
    uint32_t semantic_mask;
    uint32_t moving_semantic_mask;
    uint32_t socket_mask;
    uint32_t motion_channels;
    uint32_t attachment_context_mask;
    uint32_t calibration_flags;
    uint32_t source_forward;
    float bounds_min[3];
    float bounds_max[3];
    float ground[3];
    float source_height;
    float normalized_height;
    float target_height;
    MdkrModernCharacterStats stats;
} MdkrModernCharacterEntry;

typedef struct MdkrModernCharacterRegistry {
    MdkrModernCharacterEntry entries[MDKR_MODERN_CHARACTER_MAX];
    int count;
    int skipped;
    char skip_name[MDKR_MODERN_CHARACTER_MAX][MDKR_MODERN_CHARACTER_NAME_MAX];
    char skip_reason[MDKR_MODERN_CHARACTER_MAX][MDKR_MODERN_CHARACTER_SKIP_REASON_MAX];
} MdkrModernCharacterRegistry;

/* Missing directory is the ordinary zero-character state and succeeds. */
int mdkr_modern_character_registry_init(MdkrModernCharacterRegistry *registry,
                                        const char *directory);
void mdkr_modern_character_registry_shutdown(MdkrModernCharacterRegistry *registry);
int mdkr_modern_character_registry_count(const MdkrModernCharacterRegistry *registry);
const MdkrModernCharacterEntry *mdkr_modern_character_registry_entry(
    const MdkrModernCharacterRegistry *registry, int index);
int mdkr_modern_character_registry_find(const MdkrModernCharacterRegistry *registry,
                                        const char *id);
int mdkr_modern_character_registry_skipped(const MdkrModernCharacterRegistry *registry);
const char *mdkr_modern_character_registry_skip_name(
    const MdkrModernCharacterRegistry *registry, int index);
const char *mdkr_modern_character_registry_skip_reason(
    const MdkrModernCharacterRegistry *registry, int index);

/* Loads one selected cache into caller-owned storage. */
int mdkr_modern_character_registry_load(const MdkrModernCharacterRegistry *registry,
                                        int index, MdkrModernCharacterAsset *out,
                                        char *error, size_t error_size);

#ifdef __cplusplus
}
#endif

#endif /* MDKR64_MODERN_CHARACTER_REGISTRY_H */
