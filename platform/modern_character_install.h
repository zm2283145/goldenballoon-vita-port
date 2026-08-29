/* Transactional native import/removal for portable .mdkrchar packages. */
#ifndef MDKR64_MODERN_CHARACTER_INSTALL_H
#define MDKR64_MODERN_CHARACTER_INSTALL_H

#include <stddef.h>
#include <stdint.h>

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
    unsigned cleanup_pending_files;
    char id[65];
    char display_name[97];
    char short_name[97];
    char narration_name[97];
    char sort_label[97];
    char package_sha256[65];
    char source_digest[65];
    uint32_t provenance_present;
    char license_spdx[129];
    char attribution[257];
    char source_url[2049];
    uint32_t donor;
    uint32_t vehicle_mask;
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
    uint32_t semantic_mask;
    uint32_t disabled_semantic_mask;
    uint32_t identity_present;
    uint32_t rig_mode;
    uint32_t rig_reviewed;
    uint32_t rig_roles;
    uint32_t joint_constraints;
    uint32_t secondary_chains;
    uint32_t secondary_joints;
    uint64_t encoded_texture_bytes;
    uint64_t decoded_texture_bytes;
    uint32_t ktx2_textures;
    uint64_t ktx2_source_bytes;
    uint32_t lod_vertices[4];
    uint32_t lod_triangles[4];
    uint32_t lod_primitives[4];
    char message[256];
} MdkrModernCharacterInstallResult;

/* Runs after file retirement/recovery has reached its durable decision but
 * before the cross-process character lifecycle lock is released. This lets a
 * launcher commit its own metadata without racing a same-id reinstall in a
 * second process. `native_cleanup_pending` is nonzero when only private trash
 * cleanup remains; the package itself is already retired. */
typedef int (*MdkrModernCharacterLifecycleCommit)(
    void *context, unsigned native_cleanup_pending);

/* Performs every portable-package/cache/digest validation and publishes the
 * exact compiled summary without creating a directory or installing bytes. */
int mdkr_modern_character_inspect_portable(
    const char *package_path, MdkrModernCharacterInstallResult *result);

/* Returns one only after the cache has been validated and atomically
 * published. A valid source-only package returns zero with needs_compiler=1. */
int mdkr_modern_character_install_portable(
    const char *package_path, const char *directory,
    MdkrModernCharacterInstallResult *result);

/* Commits only if the package SHA-256 and installed source digest still match
 * the candidate/base pair previously shown to the user. An empty expected
 * installed digest means the reviewed state had no package with this id. */
int mdkr_modern_character_install_portable_reviewed(
    const char *package_path, const char *directory,
    const char *expected_package_sha256,
    const char *expected_installed_source_digest,
    MdkrModernCharacterInstallResult *result);

/* Atomically moves one validated cache into or out of runtime discovery while
 * retaining every content-addressed source revision and provenance report. */
int mdkr_modern_character_set_enabled(
    const char *package_id, const char *directory, int enabled,
    MdkrModernCharacterInstallResult *result);

/* Transactionally retires active/disabled caches plus every retained source
 * and provenance revision owned by this exact package id. The complete set is
 * validated and moved into a private quarantine before any unlink occurs; a
 * publication failure rolls the whole set back. Post-commit trash cleanup is
 * best effort and reported separately from transaction failure. */
int mdkr_modern_character_remove_installed(
    const char *package_id, const char *directory,
    MdkrModernCharacterInstallResult *result);

/* Coordinated form used when the caller owns metadata outside the installed
 * directory. The callback is invoked exactly once after durable retirement,
 * while `.character-import.lock` still excludes every native/Python mutator.
 * A callback failure is reported as pending cleanup but cannot roll back an
 * already committed package retirement. */
int mdkr_modern_character_remove_installed_coordinated(
    const char *package_id, const char *directory,
    MdkrModernCharacterLifecycleCommit commit, void *context,
    MdkrModernCharacterInstallResult *result);

/* Completes or rolls back private deletion quarantines after an interrupted
 * launcher transaction. `retire` removes only validated private trash and
 * never a current same-id package in the installed root; zero restores
 * quarantined files without overwriting different bytes. The caller keeps its
 * durable cleanup marker until this returns success. */
int mdkr_modern_character_reconcile_removal(
    const char *package_id, const char *directory, int retire,
    MdkrModernCharacterInstallResult *result);

/* Coordinated recovery counterpart. The callback runs only after every
 * quarantine was reconciled successfully and before the lifecycle lock is
 * released. Unlike first-time retirement, callback failure means recovery is
 * incomplete and this function returns zero so the durable marker is kept. */
int mdkr_modern_character_reconcile_removal_coordinated(
    const char *package_id, const char *directory, int retire,
    MdkrModernCharacterLifecycleCommit commit, void *context,
    MdkrModernCharacterInstallResult *result);

#ifdef __cplusplus
}
#endif

#endif /* MDKR64_MODERN_CHARACTER_INSTALL_H */
