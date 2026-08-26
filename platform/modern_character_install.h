/* Transactional native import/removal for portable .mdkrchar packages. */
#ifndef MDKR64_MODERN_CHARACTER_INSTALL_H
#define MDKR64_MODERN_CHARACTER_INSTALL_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MdkrModernCharacterInstallResult {
    int needs_compiler;
    int enabled;
    unsigned removed_files;
    unsigned removed_source_revisions;
    unsigned removed_provenance_reports;
    unsigned failed_files;
    char id[65];
    char display_name[97];
    char message[256];
} MdkrModernCharacterInstallResult;

/* Returns one only after the cache has been validated and atomically
 * published. A valid source-only package returns zero with needs_compiler=1. */
int mdkr_modern_character_install_portable(
    const char *package_path, const char *directory,
    MdkrModernCharacterInstallResult *result);

/* Atomically moves one validated cache into or out of runtime discovery while
 * retaining every content-addressed source revision and provenance report. */
int mdkr_modern_character_set_enabled(
    const char *package_id, const char *directory, int enabled,
    MdkrModernCharacterInstallResult *result);

/* Destructive: removes active/disabled caches plus every retained source and
 * provenance revision owned by this exact package id. */
int mdkr_modern_character_remove_installed(
    const char *package_id, const char *directory,
    MdkrModernCharacterInstallResult *result);

#ifdef __cplusplus
}
#endif

#endif /* MDKR64_MODERN_CHARACTER_INSTALL_H */
