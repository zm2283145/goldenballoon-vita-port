/* Decoded, game-ready presentation identity owned by a runtime asset pool. */
#ifndef MDKR64_MODERN_CHARACTER_IDENTITY_H
#define MDKR64_MODERN_CHARACTER_IDENTITY_H

#include <stddef.h>
#include <stdint.h>

#include "modern_character_asset.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MDKR_MODERN_PORTRAIT_SIZE 40u
#define MDKR_MODERN_PORTRAIT_BYTES \
    (MDKR_MODERN_PORTRAIT_SIZE * MDKR_MODERN_PORTRAIT_SIZE * 4u)

typedef struct MdkrModernDecodedIdentity {
    uint8_t portrait_rgba[MDKR_MODERN_PORTRAIT_BYTES];
    uint8_t minimap_rgba[4];
    int has_portrait;
} MdkrModernDecodedIdentity;

/* Legacy caches succeed with has_portrait=0. A source-v3 identity is decoded
 * and resampled once; malformed media makes pool publication fail closed. */
int mdkr_modern_identity_init(const MdkrModernCharacterAsset *asset,
                              MdkrModernDecodedIdentity *out,
                              char *error, size_t error_size);

#ifdef __cplusplus
}
#endif

#endif /* MDKR64_MODERN_CHARACTER_IDENTITY_H */
