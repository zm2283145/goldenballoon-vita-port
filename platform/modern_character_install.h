/* Transactional native import/removal for portable .mdkrchar packages. */
#ifndef MDKR64_MODERN_CHARACTER_INSTALL_H
#define MDKR64_MODERN_CHARACTER_INSTALL_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MdkrModernCharacterInstallResult {
    int needs_compiler;
    char id[65];
    char display_name[97];
    char message[256];
} MdkrModernCharacterInstallResult;

/* Returns one only after the cache has been validated and atomically
 * published. A valid source-only package returns zero with needs_compiler=1. */
int mdkr_modern_character_install_portable(
    const char *package_path, const char *directory,
    MdkrModernCharacterInstallResult *result);

/* Removes the selected cache plus retained source/provenance files. */
int mdkr_modern_character_remove_installed(
    const char *package_id, const char *directory,
    MdkrModernCharacterInstallResult *result);

#ifdef __cplusplus
}
#endif

#endif /* MDKR64_MODERN_CHARACTER_INSTALL_H */
