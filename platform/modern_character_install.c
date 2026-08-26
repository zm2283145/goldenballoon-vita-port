#include "modern_character_install.h"

#include "fs_utf8.h"
#include "modern_character_asset.h"
#include "sha256.h"
#include "miniz.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SOURCE_PACKAGE_MAX (512u * 1024u * 1024u)
#define MANIFEST_MAX (1024u * 1024u)
#define LICENSE_MAX (1024u * 1024u)
#define PORTRAIT_MAX (8u * 1024u * 1024u)
#define COMPILER_ID "mdkr-character-compiler/4"
#define LEGACY_COMPILER_ID_V3 "mdkr-character-compiler/3"
#define LEGACY_COMPILER_ID_V2 "mdkr-character-compiler/2"
#define LEGACY_COMPILER_ID_V1 "mdkr-character-compiler/1"

static unsigned s_stage_serial;

static void result_reset(MdkrModernCharacterInstallResult *result) {
    if (result != NULL) memset(result, 0, sizeof(*result));
}

static void result_message(MdkrModernCharacterInstallResult *result,
                           const char *message) {
    if (result != NULL) {
        (void)snprintf(result->message, sizeof(result->message), "%s",
                       message != NULL ? message : "unknown character import error");
    }
}

static int id_valid(const char *id) {
    size_t index;
    size_t length;
    if (id == NULL) return 0;
    length = strlen(id);
    if (length < 2u || length > 64u ||
        !((id[0] >= 'a' && id[0] <= 'z') ||
          (id[0] >= '0' && id[0] <= '9'))) return 0;
    for (index = 1u; index < length; index++) {
        const unsigned char byte = (unsigned char)id[index];
        if (!((byte >= 'a' && byte <= 'z') ||
              (byte >= '0' && byte <= '9') || byte == '.' ||
              byte == '_' || byte == '-')) return 0;
    }
    return 1;
}

static int json_escape(char *output, size_t capacity, const char *input) {
    static const char hex[] = "0123456789abcdef";
    size_t write = 0u;
    const unsigned char *read = (const unsigned char *)input;
    if (output == NULL || capacity == 0u || input == NULL) return 0;
    while (*read != 0u) {
        unsigned char byte = *read++;
        if (byte == '"' || byte == '\\') {
            if (write + 2u >= capacity) return 0;
            output[write++] = '\\';
            output[write++] = (char)byte;
        } else if (byte < 0x20u) {
            if (write + 6u >= capacity) return 0;
            output[write++] = '\\';
            output[write++] = 'u';
            output[write++] = '0';
            output[write++] = '0';
            output[write++] = hex[byte >> 4u];
            output[write++] = hex[byte & 15u];
        } else {
            if (write + 1u >= capacity) return 0;
            output[write++] = (char)byte;
        }
    }
    output[write] = '\0';
    return 1;
}

static int path_join(char *output, size_t capacity,
                     const char *directory, const char *leaf) {
    const size_t length = directory != NULL ? strlen(directory) : 0u;
    const char *separator = length != 0u &&
        (directory[length - 1u] == '/' || directory[length - 1u] == '\\')
        ? "" : "/";
    const int written = snprintf(output, capacity, "%s%s%s",
                                 directory != NULL ? directory : "",
                                 separator, leaf != NULL ? leaf : "");
    return written >= 0 && (size_t)written < capacity;
}

/* Windows reports a missing path as a successful query with exists=0; POSIX
 * reports ENOENT. Normalize that platform detail for lifecycle state checks. */
static int query_path_state(const char *path, int *exists, int *regular) {
    int result;
    errno = 0;
    result = mdkr_path_query_utf8(path, exists, regular, NULL);
    if (result == 0) return 1;
    if (errno == ENOENT) {
        if (exists != NULL) *exists = 0;
        if (regular != NULL) *regular = 0;
        return 1;
    }
    return 0;
}

static int ensure_directory(const char *directory) {
    int exists = 0;
    int is_directory = 0;
    if (mdkr_path_query_utf8(directory, &exists, NULL, &is_directory) == 0) {
        return exists && is_directory &&
               mdkr_path_is_link_or_reparse_utf8(directory) == 0;
    }
    if (mdkr_mkdir_utf8(directory) != 0) return 0;
    return mdkr_path_is_link_or_reparse_utf8(directory) == 0;
}

static int open_stage(const char *destination, char *stage, size_t stage_size,
                      FILE **output) {
    unsigned attempt;
    for (attempt = 0u; attempt < 64u; attempt++) {
        const int written = snprintf(stage, stage_size, "%s.tmp.%u",
                                     destination, ++s_stage_serial);
        if (written < 0 || (size_t)written >= stage_size) return 0;
        *output = mdkr_fopen_utf8(stage, "wbx");
        if (*output != NULL) return 1;
        if (errno != EEXIST) return 0;
    }
    return 0;
}

static int write_atomic(const char *destination, const void *bytes, size_t size) {
    char stage[4096] = {0};
    FILE *output = NULL;
    int okay = open_stage(destination, stage, sizeof(stage), &output);
    if (okay && size != 0u && fwrite(bytes, 1u, size, output) != size) okay = 0;
    if (okay && mdkr_file_sync(output) != 0) okay = 0;
    if (output != NULL && fclose(output) != 0) okay = 0;
    if (okay && mdkr_move_utf8(stage, destination, 1, 1) != 0) okay = 0;
    if (!okay && stage[0] != '\0') (void)mdkr_remove_utf8(stage);
    if (okay && mdkr_parent_directory_sync_utf8(destination) != 0) {
        /* The committed file is visible and content-synced. Do not lie that it
         * vanished merely because this filesystem cannot sync a directory. */
    }
    return okay;
}

static int hash_file(FILE *file, char output[MDKR_SHA256_HEX_SIZE]);

static int stored_file_matches(const char *path, const char *expected_hash) {
    int regular = 0;
    char actual[MDKR_SHA256_HEX_SIZE];
    FILE *file;
    int matches;
    if (mdkr_path_query_utf8(path, NULL, &regular, NULL) != 0 || !regular ||
        mdkr_path_is_link_or_reparse_utf8(path) != 0) return 0;
    file = mdkr_fopen_utf8(path, "rb");
    if (file == NULL) return 0;
    matches = hash_file(file, actual) && strcmp(actual, expected_hash) == 0;
    (void)fclose(file);
    return matches;
}

static int copy_source_if_absent(const char *source, const char *destination,
                                 const char *expected_hash) {
    int exists = 0;
    FILE *input;
    FILE *output = NULL;
    char stage[4096];
    unsigned char buffer[64u * 1024u];
    size_t count;
    int okay = 1;
    if (mdkr_path_query_utf8(destination, &exists, NULL, NULL) == 0 && exists) {
        return stored_file_matches(destination, expected_hash);
    }
    input = mdkr_fopen_utf8(source, "rb");
    if (input == NULL ||
        !open_stage(destination, stage, sizeof(stage), &output)) {
        if (input != NULL) fclose(input);
        return 0;
    }
    while ((count = fread(buffer, 1u, sizeof(buffer), input)) != 0u) {
        if (fwrite(buffer, 1u, count, output) != count) {
            okay = 0;
            break;
        }
    }
    if (ferror(input)) okay = 0;
    if (fclose(input) != 0) okay = 0;
    if (okay && mdkr_file_sync(output) != 0) okay = 0;
    if (fclose(output) != 0) okay = 0;
    if (okay && mdkr_move_utf8(stage, destination, 0, 1) != 0) {
        /* A concurrent identical import may have won the content-addressed
         * source name. Treat an extant regular destination as success. */
        okay = stored_file_matches(destination, expected_hash);
    }
    (void)mdkr_remove_utf8(stage);
    return okay && stored_file_matches(destination, expected_hash);
}

static int hash_file(FILE *file, char output[MDKR_SHA256_HEX_SIZE]) {
    MdkrSha256 digest;
    uint8_t bytes[MDKR_SHA256_DIGEST_SIZE];
    unsigned char buffer[64u * 1024u];
    static const char hex[] = "0123456789abcdef";
    size_t count;
    unsigned index;
    if (fseek(file, 0, SEEK_SET) != 0) return 0;
    mdkr_sha256_init(&digest);
    while ((count = fread(buffer, 1u, sizeof(buffer), file)) != 0u) {
        mdkr_sha256_update(&digest, buffer, count);
    }
    if (ferror(file)) return 0;
    mdkr_sha256_final(&digest, bytes);
    for (index = 0u; index < sizeof(bytes); index++) {
        output[index * 2u] = hex[bytes[index] >> 4u];
        output[index * 2u + 1u] = hex[bytes[index] & 15u];
    }
    output[64] = '\0';
    return fseek(file, 0, SEEK_SET) == 0;
}

static void digest_hex(const uint8_t bytes[32], char output[65]) {
    static const char hex[] = "0123456789abcdef";
    unsigned index;
    for (index = 0u; index < 32u; index++) {
        output[index * 2u] = hex[bytes[index] >> 4u];
        output[index * 2u + 1u] = hex[bytes[index] & 15u];
    }
    output[64] = '\0';
}

static int archive_entry(mz_zip_archive *archive, mz_uint index,
                         const char *expected, mz_uint64 maximum,
                         mz_zip_archive_file_stat *stat) {
    if (!mz_zip_reader_file_stat(archive, index, stat) ||
        strcmp(stat->m_filename, expected) != 0 || stat->m_is_directory ||
        stat->m_is_encrypted || stat->m_method != 0u ||
        stat->m_uncomp_size > maximum || stat->m_comp_size != stat->m_uncomp_size) {
        return 0;
    }
    return 1;
}

typedef struct DigestExtractState {
    MdkrSha256 *digest;
    mz_uint64 expected_offset;
} DigestExtractState;

static size_t digest_extract(void *opaque, mz_uint64 offset,
                             const void *bytes, size_t size) {
    DigestExtractState *state = (DigestExtractState *)opaque;
    if (state == NULL || state->digest == NULL ||
        offset != state->expected_offset) return 0u;
    mdkr_sha256_update(state->digest, bytes, size);
    state->expected_offset += (mz_uint64)size;
    return size;
}

static int archive_source_digest(mz_zip_archive *archive,
                                 const char *const *names,
                                 const mz_uint64 *sizes,
                                 unsigned count,
                                 const char *compiler_id,
                                 uint8_t output[32]) {
    MdkrSha256 digest;
    unsigned index;
    mdkr_sha256_init(&digest);
    mdkr_sha256_update(&digest, compiler_id, strlen(compiler_id) + 1u);
    for (index = 0u; index < count; index++) {
        uint8_t length[8];
        unsigned byte;
        DigestExtractState state;
        mdkr_sha256_update(&digest, names[index], strlen(names[index]) + 1u);
        for (byte = 0u; byte < 8u; byte++) {
            length[byte] = (uint8_t)(sizes[index] >> (byte * 8u));
        }
        mdkr_sha256_update(&digest, length, sizeof(length));
        state.digest = &digest;
        state.expected_offset = 0u;
        if (!mz_zip_reader_extract_to_callback(
                archive, index, digest_extract, &state, 0u) ||
            state.expected_offset != sizes[index]) return 0;
    }
    mdkr_sha256_final(&digest, output);
    return 1;
}

int mdkr_modern_character_install_portable(
    const char *package_path, const char *directory,
    MdkrModernCharacterInstallResult *result) {
    static const char *const names_v2[] = {
        "manifest.json", "model.glb", "LICENSE.txt", "compiled.mdkc"
    };
    static const mz_uint64 caps_v2[] = {
        MANIFEST_MAX, SOURCE_PACKAGE_MAX, LICENSE_MAX, MDKR_MDKC_FILE_MAX
    };
    static const char *const names_v3[] = {
        "manifest.json", "model.glb", "portrait.png", "LICENSE.txt",
        "compiled.mdkc"
    };
    static const mz_uint64 caps_v3[] = {
        MANIFEST_MAX, SOURCE_PACKAGE_MAX, PORTRAIT_MAX, LICENSE_MAX,
        MDKR_MDKC_FILE_MAX
    };
    FILE *package = NULL;
    FILE *lock = NULL;
    mz_zip_archive archive;
    mz_zip_archive_file_stat stat;
    MdkrModernCharacterAsset asset;
    MdkrModernCharacterDefinition definition;
    void *compiled = NULL;
    size_t compiled_size = 0u;
    char error[256];
    char hash[MDKR_SHA256_HEX_SIZE];
    char leaf[256] = {0};
    char source_leaf[256] = {0};
    char report_leaf[256] = {0};
    char source_path[4096] = {0};
    char report_path[4096] = {0};
    char cache_path[4096] = {0};
    char disabled_cache_path[4096] = {0};
    char lock_path[4096] = {0};
    mz_uint64 package_size = 0u;
    mz_uint64 member_sizes[5] = {0u, 0u, 0u, 0u, 0u};
    uint8_t source_digest[32];
    uint8_t legacy_source_digest_v3[32];
    uint8_t legacy_source_digest_v2[32];
    uint8_t legacy_source_digest_v1[32];
    char cache_source_digest[65] = {0};
    char installed_id[65] = {0};
    char installed_display_name[97] = {0};
    char escaped_display_name[577] = {0};
    char report_text[2048];
    int regular = 0;
    int okay = 0;
    int lock_owned = 0;
    mz_uint file_count;
    mz_uint index;
    const char *const *names = NULL;
    const mz_uint64 *caps = NULL;
    unsigned source_count = 0u;
    unsigned compiled_index = 0u;
    int source_only = 0;
    int legacy_digest_allowed = 0;
    int active_exists = 0;
    int disabled_exists = 0;
    int active_regular = 0;
    int disabled_regular = 0;
    const char *publish_cache_path = NULL;
    result_reset(result);
    error[0] = '\0';
    memset(&archive, 0, sizeof(archive));
    memset(&asset, 0, sizeof(asset));
    if (package_path == NULL || directory == NULL || directory[0] == '\0' ||
        mdkr_path_query_utf8(package_path, NULL, &regular, NULL) != 0 ||
        !regular || !ensure_directory(directory)) {
        result_message(result, "package path or character directory is unavailable");
        goto done;
    }
    package = mdkr_fopen_utf8(package_path, "rb");
    if (package == NULL || fseek(package, 0, SEEK_END) != 0) {
        result_message(result, "character package could not be opened");
        goto done;
    }
    {
        const long length = ftell(package);
        if (length <= 0 || (unsigned long)length > SOURCE_PACKAGE_MAX ||
            fseek(package, 0, SEEK_SET) != 0 || !hash_file(package, hash)) {
            result_message(result, "character package exceeds its bounded size or could not be read");
            goto done;
        }
        package_size = (mz_uint64)(unsigned long)length;
    }
    if (!mz_zip_reader_init_cfile(&archive, package, package_size, 0u)) {
        result_message(result, "character package is not a readable deterministic ZIP");
        goto done;
    }
    file_count = mz_zip_reader_get_num_files(&archive);
    if (file_count == 3u) {
        names = names_v2;
        caps = caps_v2;
        source_count = 3u;
        source_only = 1;
        legacy_digest_allowed = 1;
    } else if (file_count == 4u) {
        if (!mz_zip_reader_file_stat(&archive, 2u, &stat)) {
            result_message(result, "character package inventory could not be read");
            goto done;
        }
        if (strcmp(stat.m_filename, "portrait.png") == 0) {
            names = names_v3;
            caps = caps_v3;
            source_count = 4u;
            source_only = 1;
        } else {
            names = names_v2;
            caps = caps_v2;
            source_count = 3u;
            compiled_index = 3u;
            legacy_digest_allowed = 1;
        }
    } else if (file_count == 5u) {
        names = names_v3;
        caps = caps_v3;
        source_count = 4u;
        compiled_index = 4u;
    } else {
        result_message(result,
                       "package does not match a canonical source or portable layout");
        goto done;
    }
    for (index = 0u; index < file_count; index++) {
        if (!archive_entry(&archive, index, names[index], caps[index], &stat)) {
            result_message(result,
                           "package members are unsafe, reordered, compressed, or oversized");
            goto done;
        }
        member_sizes[index] = stat.m_uncomp_size;
        if (!source_only && index == compiled_index) {
            compiled_size = (size_t)stat.m_uncomp_size;
        }
    }
    if (source_only) {
        if (result != NULL) result->needs_compiler = 1;
        result_message(result,
            "This source-only package needs the author compiler; ask its author for a portable package or import from a developer checkout.");
        goto done;
    }
    if (!archive_source_digest(&archive, names, member_sizes, source_count,
                               COMPILER_ID, source_digest) ||
        !archive_source_digest(&archive, names, member_sizes, source_count,
                               LEGACY_COMPILER_ID_V3,
                               legacy_source_digest_v3) ||
        (legacy_digest_allowed &&
         (!archive_source_digest(&archive, names, member_sizes, source_count,
                                 LEGACY_COMPILER_ID_V2,
                                 legacy_source_digest_v2) ||
          !archive_source_digest(&archive, names, member_sizes, source_count,
                                 LEGACY_COMPILER_ID_V1,
                                 legacy_source_digest_v1)))) {
        result_message(result, "portable package source members could not be hashed");
        goto done;
    }
    compiled = malloc(compiled_size != 0u ? compiled_size : 1u);
    if (compiled == NULL ||
        !mz_zip_reader_extract_to_mem(&archive, compiled_index, compiled,
                                      compiled_size, 0u) ||
        !mdkr_modern_character_asset_load_memory(compiled, compiled_size,
                                                  &asset, error, sizeof(error)) ||
        !mdkr_modern_character_asset_definition(&asset, &definition)) {
        result_message(result, error[0] != '\0' ? error :
                       "embedded compiled character cache is invalid");
        goto done;
    }
    if (memcmp(asset.source_sha256, source_digest, sizeof(source_digest)) != 0 &&
        memcmp(asset.source_sha256, legacy_source_digest_v3,
               sizeof(legacy_source_digest_v3)) != 0 &&
        (!legacy_digest_allowed ||
         (memcmp(asset.source_sha256, legacy_source_digest_v2,
                 sizeof(legacy_source_digest_v2)) != 0 &&
          memcmp(asset.source_sha256, legacy_source_digest_v1,
                 sizeof(legacy_source_digest_v1)) != 0))) {
        result_message(result,
            "embedded cache does not match this package's canonical source members");
        goto done;
    }
    {
        const char *id = mdkr_modern_character_asset_string(&asset, definition.id);
        const char *display = mdkr_modern_character_asset_string(
            &asset, definition.display_name);
        if (!id_valid(id) || display == NULL || display[0] == '\0' ||
            strlen(display) >= 97u) {
            result_message(result, "embedded character identity is unsafe");
            goto done;
        }
        if (result != NULL) {
            (void)snprintf(result->id, sizeof(result->id), "%s", id);
            (void)snprintf(result->display_name, sizeof(result->display_name),
                           "%s", display);
        }
        (void)snprintf(installed_id, sizeof(installed_id), "%s", id);
        (void)snprintf(installed_display_name,
                       sizeof(installed_display_name), "%s", display);
        if (!json_escape(escaped_display_name, sizeof(escaped_display_name),
                         installed_display_name)) {
            result_message(result, "embedded character display name cannot be recorded safely");
            goto done;
        }
        (void)snprintf(source_leaf, sizeof(source_leaf), "%s.%s.mdkrchar", id, hash);
        if (!path_join(source_path, sizeof(source_path), directory, source_leaf)) {
            result_message(result, "content-addressed source path is too long");
            goto done;
        }
        (void)snprintf(report_leaf, sizeof(report_leaf), "%s.%s.json", id, hash);
        if (!path_join(report_path, sizeof(report_path), directory, report_leaf)) {
            result_message(result, "character provenance path is too long");
            goto done;
        }
        (void)snprintf(leaf, sizeof(leaf), "%s.mdkc", id);
        if (!path_join(cache_path, sizeof(cache_path), directory, leaf) ||
            snprintf(leaf, sizeof(leaf), "%s.mdkc.disabled", id) < 0 ||
            !path_join(disabled_cache_path, sizeof(disabled_cache_path),
                       directory, leaf) ||
            !path_join(lock_path, sizeof(lock_path), directory,
                       ".character-import.lock")) {
            result_message(result, "character install path is too long");
            goto done;
        }
    }
    digest_hex(asset.source_sha256, cache_source_digest);
    {
        const int report_length = snprintf(
            report_text, sizeof(report_text),
            "{\n"
            "  \"schema\": \"mdkr-character-install-v1\",\n"
            "  \"id\": \"%s\",\n"
            "  \"display_name\": \"%s\",\n"
            "  \"source_sha256\": \"%s\",\n"
            "  \"cache_source_digest\": \"%s\",\n"
            "  \"compiler\": \"portable-embedded-mdkc/1\",\n"
            "  \"source_file\": \"%s\"\n"
            "}\n",
            installed_id, escaped_display_name,
            hash, cache_source_digest, source_leaf);
        if (report_length < 0 || (size_t)report_length >= sizeof(report_text)) {
            result_message(result, "character provenance report exceeds its bound");
            goto done;
        }
    }
    lock = mdkr_fopen_utf8(lock_path, "wbx");
    if (lock == NULL) {
        result_message(result, "another character import is active (or left a stale import lock)");
        goto done;
    }
    lock_owned = 1;
    (void)fputs("native-launcher\n", lock);
    if (fclose(lock) != 0) {
        lock = NULL;
        result_message(result, "character import lock could not be committed");
        goto done;
    }
    lock = NULL;
    if (!query_path_state(cache_path, &active_exists, &active_regular) ||
        !query_path_state(disabled_cache_path, &disabled_exists,
                          &disabled_regular) ||
        (active_exists && (!active_regular ||
         mdkr_path_is_link_or_reparse_utf8(cache_path) != 0)) ||
        (disabled_exists && (!disabled_regular ||
         mdkr_path_is_link_or_reparse_utf8(disabled_cache_path) != 0)) ||
        (active_exists && disabled_exists)) {
        result_message(result,
            "installed character cache state is ambiguous or unsafe");
        goto done;
    }
    publish_cache_path = disabled_exists ? disabled_cache_path : cache_path;
    if (!copy_source_if_absent(package_path, source_path, hash) ||
        !write_atomic(report_path, report_text, strlen(report_text)) ||
        !write_atomic(publish_cache_path, compiled, compiled_size)) {
        result_message(result, "character source or cache could not be installed transactionally");
        goto done;
    }
    result_message(result, disabled_exists
        ? "Portable character validated and updated while disabled."
        : "Portable character validated and installed.");
    if (result != NULL) result->enabled = disabled_exists ? 0 : 1;
    okay = 1;

done:
    if (lock != NULL) fclose(lock);
    if (lock_owned && lock_path[0] != '\0') (void)mdkr_remove_utf8(lock_path);
    mdkr_modern_character_asset_unload(&asset);
    free(compiled);
    if (archive.m_zip_mode != MZ_ZIP_MODE_INVALID) mz_zip_reader_end(&archive);
    if (package != NULL) fclose(package);
    return okay;
}

int mdkr_modern_character_set_enabled(
    const char *package_id, const char *directory, int enabled,
    MdkrModernCharacterInstallResult *result) {
    MdkrModernCharacterAsset asset;
    MdkrModernCharacterDefinition definition;
    char active_leaf[256];
    char disabled_leaf[256];
    char active_path[4096];
    char disabled_path[4096];
    char lock_path[4096];
    char error[256];
    const char *source;
    const char *destination;
    const char *asset_id;
    FILE *lock = NULL;
    int regular = 0;
    int destination_exists = 0;
    int okay = 0;
    result_reset(result);
    memset(&asset, 0, sizeof(asset));
    error[0] = '\0';
    if (!id_valid(package_id) || directory == NULL ||
        snprintf(active_leaf, sizeof(active_leaf), "%s.mdkc", package_id) < 0 ||
        snprintf(disabled_leaf, sizeof(disabled_leaf), "%s.mdkc.disabled",
                 package_id) < 0 ||
        !path_join(active_path, sizeof(active_path), directory, active_leaf) ||
        !path_join(disabled_path, sizeof(disabled_path), directory,
                   disabled_leaf) ||
        !path_join(lock_path, sizeof(lock_path), directory,
                   ".character-import.lock")) {
        result_message(result, "installed character directory or id is invalid");
        return 0;
    }
    source = enabled ? disabled_path : active_path;
    destination = enabled ? active_path : disabled_path;
    lock = mdkr_fopen_utf8(lock_path, "wbx");
    if (lock == NULL) {
        result_message(result, "another character import or lifecycle change is active");
        return 0;
    }
    (void)fputs(enabled ? "native-launcher-enable\n"
                        : "native-launcher-disable\n", lock);
    if (fclose(lock) != 0) {
        lock = NULL;
        (void)mdkr_remove_utf8(lock_path);
        result_message(result, "character lifecycle lock could not be committed");
        return 0;
    }
    lock = NULL;
    if (!query_path_state(source, NULL, &regular) || !regular ||
        mdkr_path_is_link_or_reparse_utf8(source) != 0 ||
        !query_path_state(destination, &destination_exists, NULL) ||
        destination_exists) {
        result_message(result, enabled
            ? "disabled character cache is unavailable or an active cache already exists"
            : "active character cache is unavailable or a disabled cache already exists");
        goto done;
    }
    if (!mdkr_modern_character_asset_load_file(
            source, &asset, error, sizeof(error)) ||
        !mdkr_modern_character_asset_definition(&asset, &definition) ||
        (asset_id = mdkr_modern_character_asset_string(
             &asset, definition.id)) == NULL ||
        strcmp(asset_id, package_id) != 0) {
        result_message(result, error[0] != '\0' ? error :
                       "character cache identity does not match its filename");
        goto done;
    }
    if (mdkr_move_utf8(source, destination, 0, 1) == 0) {
        if (result != NULL) {
            result->enabled = enabled ? 1 : 0;
            (void)snprintf(result->id, sizeof(result->id), "%s", package_id);
        }
        result_message(result, enabled
            ? "Custom character enabled; retained source history was unchanged."
            : "Custom character disabled; retained source history was unchanged.");
        okay = 1;
    } else {
        result_message(result, "character cache state could not be changed atomically");
    }
done:
    mdkr_modern_character_asset_unload(&asset);
    (void)mdkr_remove_utf8(lock_path);
    return okay;
}

static int content_addressed_leaf(const char *name, const char *package_id,
                                  const char *suffix) {
    const size_t id_length = strlen(package_id);
    const size_t suffix_length = strlen(suffix);
    size_t index;
    if (strlen(name) != id_length + 1u + 64u + suffix_length ||
        strncmp(name, package_id, id_length) != 0 ||
        name[id_length] != '.' ||
        strcmp(name + id_length + 1u + 64u, suffix) != 0) return 0;
    for (index = id_length + 1u; index < id_length + 1u + 64u; index++) {
        const char byte = name[index];
        if (!((byte >= '0' && byte <= '9') ||
              (byte >= 'a' && byte <= 'f'))) return 0;
    }
    return 1;
}

int mdkr_modern_character_remove_installed(
    const char *package_id, const char *directory,
    MdkrModernCharacterInstallResult *result) {
    DIR *handle;
    struct dirent *item;
    char path[4096];
    char lock_path[4096];
    FILE *lock = NULL;
    const size_t id_length = package_id != NULL ? strlen(package_id) : 0u;
    int removed = 0;
    int removed_sources = 0;
    int removed_reports = 0;
    int failed = 0;
    result_reset(result);
    if (!id_valid(package_id) || directory == NULL ||
        !path_join(lock_path, sizeof(lock_path), directory,
                   ".character-import.lock")) {
        result_message(result, "installed character directory or id is invalid");
        return 0;
    }
    lock = mdkr_fopen_utf8(lock_path, "wbx");
    if (lock == NULL) {
        result_message(result, "another character import or removal is active");
        return 0;
    }
    (void)fputs("native-launcher-remove\n", lock);
    if (fclose(lock) != 0 || (handle = opendir(directory)) == NULL) {
        (void)mdkr_remove_utf8(lock_path);
        result_message(result, "installed character directory could not be opened");
        return 0;
    }
    while ((item = readdir(handle)) != NULL) {
        int regular = 0;
        const char *name = item->d_name;
        int candidate = strlen(name) == id_length + 5u &&
                        strncmp(name, package_id, id_length) == 0 &&
                        strcmp(name + id_length, ".mdkc") == 0;
        if (!candidate) {
            candidate = strlen(name) == id_length + 14u &&
                        strncmp(name, package_id, id_length) == 0 &&
                        strcmp(name + id_length, ".mdkc.disabled") == 0;
        }
        {
            int source_revision = content_addressed_leaf(
                name, package_id, ".mdkrchar");
            int provenance_report = content_addressed_leaf(
                name, package_id, ".json");
            if (!candidate) {
                candidate = source_revision || provenance_report;
            }
            if (!candidate) continue;
            if (!path_join(path, sizeof(path), directory, name) ||
                mdkr_path_query_utf8(path, NULL, &regular, NULL) != 0 ||
                !regular || mdkr_path_is_link_or_reparse_utf8(path) != 0) {
                failed++;
                continue;
            }
            if (mdkr_remove_utf8(path) == 0) {
                removed++;
                if (source_revision) removed_sources++;
                if (provenance_report) removed_reports++;
            } else {
                failed++;
            }
        }
    }
    (void)closedir(handle);
    (void)mdkr_remove_utf8(lock_path);
    if (result != NULL) {
        (void)snprintf(result->id, sizeof(result->id), "%s", package_id);
        result->removed_files = (unsigned)removed;
        result->removed_source_revisions = (unsigned)removed_sources;
        result->removed_provenance_reports = (unsigned)removed_reports;
        result->failed_files = (unsigned)failed;
    }
    if (failed != 0) {
        result_message(result,
            "Character deletion was partial; one or more owned files could not be removed.");
        return 0;
    }
    if (removed == 0) {
        result_message(result, "no regular installed files matched that character id");
        return 0;
    }
    result_message(result, "Installed character files removed.");
    return 1;
}
