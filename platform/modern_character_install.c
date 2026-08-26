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
#define COMPILER_ID "mdkr-character-compiler/2"
#define LEGACY_COMPILER_ID "mdkr-character-compiler/1"

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
                                 const mz_uint64 sizes[4],
                                 const char *compiler_id,
                                 uint8_t output[32]) {
    static const char *names[] = {
        "manifest.json", "model.glb", "LICENSE.txt"
    };
    MdkrSha256 digest;
    unsigned index;
    mdkr_sha256_init(&digest);
    mdkr_sha256_update(&digest, compiler_id, strlen(compiler_id) + 1u);
    for (index = 0u; index < 3u; index++) {
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
    static const char *names[] = {
        "manifest.json", "model.glb", "LICENSE.txt", "compiled.mdkc"
    };
    static const mz_uint64 caps[] = {
        MANIFEST_MAX, SOURCE_PACKAGE_MAX, LICENSE_MAX, MDKR_MDKC_FILE_MAX
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
    char lock_path[4096] = {0};
    mz_uint64 package_size = 0u;
    mz_uint64 member_sizes[4] = {0u, 0u, 0u, 0u};
    uint8_t source_digest[32];
    uint8_t legacy_source_digest[32];
    char cache_source_digest[65] = {0};
    char installed_id[65] = {0};
    char report_text[2048];
    int regular = 0;
    int okay = 0;
    int lock_owned = 0;
    mz_uint file_count;
    mz_uint index;
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
        for (index = 0u; index < 3u; index++) {
            if (!archive_entry(&archive, index, names[index], caps[index],
                               &stat)) {
                result_message(
                    result,
                    "source package members are unsafe, reordered, compressed, or oversized");
                goto done;
            }
        }
        if (result != NULL) result->needs_compiler = 1;
        result_message(result,
            "This source-only package needs the author compiler; ask its author for a portable package or import from a developer checkout.");
        goto done;
    }
    if (file_count != 4u) {
        result_message(result, "portable package must contain exactly four canonical files");
        goto done;
    }
    for (index = 0u; index < 4u; index++) {
        if (!archive_entry(&archive, index, names[index], caps[index], &stat)) {
            result_message(result, "portable package members are unsafe, reordered, compressed, or oversized");
            goto done;
        }
        member_sizes[index] = stat.m_uncomp_size;
        if (index == 3u) compiled_size = (size_t)stat.m_uncomp_size;
    }
    if (!archive_source_digest(&archive, member_sizes, COMPILER_ID,
                               source_digest) ||
        !archive_source_digest(&archive, member_sizes, LEGACY_COMPILER_ID,
                               legacy_source_digest)) {
        result_message(result, "portable package source members could not be hashed");
        goto done;
    }
    compiled = malloc(compiled_size != 0u ? compiled_size : 1u);
    if (compiled == NULL ||
        !mz_zip_reader_extract_to_mem(&archive, 3u, compiled, compiled_size, 0u) ||
        !mdkr_modern_character_asset_load_memory(compiled, compiled_size,
                                                  &asset, error, sizeof(error)) ||
        !mdkr_modern_character_asset_definition(&asset, &definition)) {
        result_message(result, error[0] != '\0' ? error :
                       "embedded compiled character cache is invalid");
        goto done;
    }
    if (memcmp(asset.source_sha256, source_digest, sizeof(source_digest)) != 0 &&
        memcmp(asset.source_sha256, legacy_source_digest,
               sizeof(legacy_source_digest)) != 0) {
        result_message(result,
            "embedded cache does not match this package's manifest, model, and license");
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
            installed_id, installed_id,
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
    if (!copy_source_if_absent(package_path, source_path, hash) ||
        !write_atomic(report_path, report_text, strlen(report_text)) ||
        !write_atomic(cache_path, compiled, compiled_size)) {
        result_message(result, "character source or cache could not be installed transactionally");
        goto done;
    }
    result_message(result, "Portable character validated and installed.");
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
            candidate = content_addressed_leaf(
                            name, package_id, ".mdkrchar") ||
                        content_addressed_leaf(name, package_id, ".json");
        }
        if (!candidate || !path_join(path, sizeof(path), directory, name) ||
            mdkr_path_query_utf8(path, NULL, &regular, NULL) != 0 || !regular ||
            mdkr_path_is_link_or_reparse_utf8(path) != 0) continue;
        if (mdkr_remove_utf8(path) == 0) removed++;
    }
    (void)closedir(handle);
    (void)mdkr_remove_utf8(lock_path);
    if (removed == 0) {
        result_message(result, "no regular installed files matched that character id");
        return 0;
    }
    if (result != NULL) (void)snprintf(result->id, sizeof(result->id), "%s", package_id);
    result_message(result, "Installed character files removed.");
    return 1;
}
