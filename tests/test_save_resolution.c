/*
 * Issue #54: non-packaged native save-directory resolution order.
 *
 * Before this fix every non-.app launch resolved the save directory to the bare
 * relative name "save" -- i.e. $CWD/save -- so an AppImage launched from a
 * file manager, Gear Lever, a .desktop entry or a terminal scattered saves per
 * launch method (or landed in an unwritable directory). A follow-up then
 * grandfathered ANY existing $CWD/save, even an EMPTY one, which re-introduced
 * the same launch-directory dependence: a stray or user-created empty save/
 * beside the launch directory diverted every save away from the stable per-user
 * profile (the exact trap in the issue #54 report). The shell already put logs
 * and app preferences under SDL's per-user preference directory; saves did not.
 * This unifies them and keeps the location launch-independent.
 *
 * Resolution order for a non-packaged native build (this test's subject):
 *   1. MDKR_SAVE_DIR                         (env override; harness contract)
 *   2. portable.txt / write-fallback         (issue #33; covered by
 *                                             test_portable_paths.c)
 *   3. SDL_GetPrefPath("mdkr64","mdkr64")/save (per-user; the shell's own root)
 *
 * A populated legacy $CWD/save is NOT resolved to in place; instead
 * migrate_save_directory() (run from mdkr_user_paths_init) COPIES it into the
 * per-user directory once, so an existing install keeps its progress but the
 * live save location is still the stable per-user profile. An EMPTY $CWD/save
 * is ignored entirely.
 *
 * The preference provider is stubbed at link time so the test is hermetic and
 * needs no SDL. Two process modes keep the once-per-process CWD/pref caching
 * from colliding: the default mode proves the per-user default (even with an
 * empty $CWD/save present) and the env override; --legacy proves a POPULATED
 * $CWD/save is migrated into the per-user directory rather than adopted in place.
 */
#include "user_paths.h"
#include "fs_utf8.h"
#include "test_platform_compat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* The per-user preference root the stub below hands back. Set once in main, so
 * the expectation and the code under test agree on the exact bytes. */
static char s_pref_root[4096];

char *SDL_GetPrefPath(const char *organization, const char *application) {
    char *result;
    size_t length;
    (void)organization;
    (void)application;
    if (s_pref_root[0] == '\0') {
        return NULL;
    }
    length = strlen(s_pref_root);
    result = (char *)malloc(length + 1u);
    if (result != NULL) {
        memcpy(result, s_pref_root, length + 1u);
    }
    return result;
}

void SDL_free(void *memory) { free(memory); }

static int s_failures;

static void expect(const char *name, int condition) {
    if (!condition) {
        fprintf(stderr, "FAIL %s\n", name);
        s_failures++;
    }
}

static int join(char *output, size_t size, const char *dir, const char *leaf) {
    int written = snprintf(output, size, "%s/%s", dir, leaf);
    return written > 0 && (size_t)written < size;
}

static int read_equals(const char *path, const void *bytes, size_t size) {
    unsigned char buffer[256];
    FILE *file;
    size_t got;
    int trailing;
    if (size > sizeof(buffer)) {
        return 0;
    }
    file = fopen(path, "rb");
    if (file == NULL) {
        return 0;
    }
    got = fread(buffer, 1u, sizeof(buffer), file);
    trailing = fgetc(file);
    (void)fclose(file);
    return got == size && trailing == EOF && memcmp(buffer, bytes, size) == 0;
}

int main(int argc, char **argv) {
    const int legacy_mode = argc > 1 && strcmp(argv[1], "--legacy") == 0;
    char cwd[4096];
    char resolved[4096];
    char expected[4096];
    char save_subdir[4096];
    char eeprom[4096];

    /* A stray override from the caller's shell would silence the precedence
     * assertions; strip both before anything reads them. Also strip $APPIMAGE:
     * a suite run inside an AppImage would otherwise relocate resolution beside
     * the AppImage and defeat the per-user assertions. */
    (void)mdkr_test_env_unset("MDKR_SAVE_DIR");
    (void)mdkr_test_env_unset("MDKR_VIDEO_CONFIG_PATH");
    (void)mdkr_test_env_unset("APPIMAGE");

    /* A real, empty per-user preference root for the stub to return. */
    expect("preference root created",
           mdkr_test_make_temp_directory(s_pref_root, sizeof(s_pref_root),
                                         "mdkr-save-res-pref"));

    /* A fresh working directory so the resolution does not depend on whatever
     * directory ctest happened to run us from. */
    expect("working directory created",
           mdkr_test_make_temp_directory(cwd, sizeof(cwd), "mdkr-save-res-cwd"));
    expect("entered working directory", chdir(cwd) == 0);
    /* getcwd, not the mkdtemp string: macOS resolves /tmp -> /private/tmp, and
     * user_paths.c captures the resolved spelling too. */
    expect("captured working directory", getcwd(cwd, sizeof(cwd)) != NULL);

    if (!legacy_mode) {
        /* An EMPTY $CWD/save exists -- exactly the reporter's trap. It must NOT
         * be grandfathered: a non-packaged build resolves saves under the
         * per-user preference directory regardless, so the location cannot be
         * diverted by a stray or user-created empty save/ beside the launch
         * directory. (Before the fix this returns the empty $CWD/save.) */
        expect("empty legacy save subdir path",
               join(save_subdir, sizeof(save_subdir), cwd, "save"));
        expect("created empty legacy save subdir",
               mdkr_test_make_directory(save_subdir));
        expect("an empty $CWD/save is NOT grandfathered",
               mdkr_user_save_directory(resolved, sizeof(resolved)) &&
               join(expected, sizeof(expected), s_pref_root, "save") &&
               strcmp(resolved, expected) == 0);
        expect("per-user origin is labelled",
               strcmp(mdkr_user_paths_save_origin_label(), "per-user") == 0);

        /* The env override still wins, unchanged (the harness depends on it). */
        expect("env override path",
               join(expected, sizeof(expected), cwd, "chosen-save"));
        expect("set save override",
               mdkr_test_env_set("MDKR_SAVE_DIR", expected, 1) == 0);
        expect("env override wins",
               mdkr_user_save_directory(resolved, sizeof(resolved)) &&
               strcmp(resolved, expected) == 0);
        expect("env origin is labelled",
               strcmp(mdkr_user_paths_save_origin_label(), "env") == 0);
        (void)mdkr_test_env_unset("MDKR_SAVE_DIR");
    } else {
        static const unsigned char legacy_eeprom[] = { 4, 8, 15, 16, 23, 42 };
        char migrated[4096];
        char self[4096];

        /* A POPULATED legacy $CWD/save from a prior install. mdkr_user_paths_init
         * for a non-packaged build runs migrate_save_directory(), which must COPY
         * it into the per-user directory -- not adopt it in place -- so no
         * progress is stranded yet the live location is the stable profile. */
        expect("legacy save subdir path",
               join(save_subdir, sizeof(save_subdir), cwd, "save"));
        expect("created legacy save subdir",
               mdkr_test_make_directory(save_subdir));
        expect("legacy eeprom path",
               join(eeprom, sizeof(eeprom), save_subdir, "eeprom.bin"));
        {
            FILE *file = mdkr_fopen_utf8(eeprom, "wb");
            expect("wrote legacy save file",
                   file != NULL &&
                   fwrite(legacy_eeprom, 1u, sizeof(legacy_eeprom), file) ==
                       sizeof(legacy_eeprom));
            if (file != NULL) {
                (void)fclose(file);
            }
        }

        /* A non-.app executable path selects the non-packaged branch, which
         * captures the cwd and runs the copy-migration. */
        expect("built non-packaged executable path",
               join(self, sizeof(self), cwd, "mdkr64"));
        expect("non-packaged init runs migration",
               mdkr_user_paths_init(self) == 0);

        /* The live save directory is the per-user profile, not $CWD/save. */
        expect("live save resolves below the per-user directory",
               mdkr_user_save_directory(resolved, sizeof(resolved)) &&
               join(expected, sizeof(expected), s_pref_root, "save") &&
               strcmp(resolved, expected) == 0);
        expect("per-user origin is labelled after migration",
               strcmp(mdkr_user_paths_save_origin_label(), "per-user") == 0);

        /* The legacy bytes were copied there verbatim, so an existing install
         * keeps its progress. */
        expect("migrated eeprom path",
               join(migrated, sizeof(migrated), expected, "eeprom.bin"));
        expect("legacy save migrated byte-identically",
               read_equals(migrated, legacy_eeprom, sizeof(legacy_eeprom)));

        (void)mdkr_remove_utf8(eeprom);
        (void)mdkr_rmdir_utf8(save_subdir);
    }

    if (s_failures != 0) {
        fprintf(stderr, "%d save-resolution test(s) failed\n", s_failures);
        return 1;
    }
    puts(legacy_mode ? "save resolution (legacy migration): PASS"
                     : "save resolution (per-user): PASS");
    return 0;
}
