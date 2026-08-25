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

typedef struct MdkrModernCharacterEntry {
    char id[MDKR_MODERN_CHARACTER_ID_MAX];
    char display_name[MDKR_MODERN_CHARACTER_NAME_MAX];
    char path[MDKR_MODERN_CHARACTER_PATH_MAX];
    uint8_t source_sha256[32];
    uint32_t donor;
    uint32_t vehicle_mask;
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
