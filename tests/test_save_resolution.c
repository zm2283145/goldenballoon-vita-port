/*
 * Issue #54: non-packaged native save-directory resolution order.
 *
 * Before this fix every non-.app launch resolved the save directory to the bare
 * relative name "save" -- i.e. $CWD/save -- so an AppImage launched from a
 * file manager, Gear Lever, a .desktop entry or a terminal scattered saves per
 * launch method (or landed in an unwritable directory). The shell already put
 * logs and app preferences under SDL's per-user preference directory; saves did
 * not. This unifies them.
 *
 * Resolution order for a non-packaged native build (this test's subject):
 *   1. MDKR_SAVE_DIR                         (env override; harness contract)
 *   2. portable.txt / write-fallback         (issue #33; covered by
 *                                             test_portable_paths.c)
 *   3. a POPULATED legacy $CWD/save          (existing installs keep working)
 *   4. SDL_GetPrefPath("mdkr64","mdkr64")/save (per-user; the shell's own root)
 *
 * The preference provider is stubbed at link time so the test is hermetic and
 * needs no SDL. Two process modes keep the once-per-process CWD/pref caching
 * from colliding: the default mode proves the per-user default and the env
 * override; --legacy proves a populated $CWD/save is grandfathered in place.
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

int main(int argc, char **argv) {
    const int legacy_mode = argc > 1 && strcmp(argv[1], "--legacy") == 0;
    char cwd[4096];
    char resolved[4096];
    char expected[4096];
    char save_subdir[4096];
    char eeprom[4096];

    /* A stray override from the caller's shell would silence the precedence
     * assertions; strip both before anything reads them. */
    (void)mdkr_test_env_unset("MDKR_SAVE_DIR");
    (void)mdkr_test_env_unset("MDKR_VIDEO_CONFIG_PATH");

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
        /* No env, no portable.txt, no populated legacy save: a non-packaged
         * build must resolve saves under the per-user preference directory,
         * NOT the historical $CWD/save. (Before the fix this returns "save".) */
        expect("per-user save resolves below the preference directory",
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
        /* An existing legacy $CWD/save is grandfathered in place so no install
         * is stranded -- the resolver returns that directory, absolute, rather
         * than the per-user default. Existence is enough: a save/ folder beside
         * the launch directory is the portable-app "put saves here" signal, and
         * this is what keeps every regression check that runs from a temporary
         * working directory pointed at its own save/ (issue #54). */
        expect("legacy save subdir path",
               join(save_subdir, sizeof(save_subdir), cwd, "save"));
        expect("created legacy save subdir",
               mdkr_test_make_directory(save_subdir));
        expect("an empty existing legacy save is grandfathered in place",
               mdkr_user_save_directory(resolved, sizeof(resolved)) &&
               strcmp(resolved, save_subdir) == 0);
        expect("legacy origin is labelled",
               strcmp(mdkr_user_paths_save_origin_label(), "legacy") == 0);
        /* And a populated one, a fortiori. */
        expect("legacy eeprom path",
               join(eeprom, sizeof(eeprom), save_subdir, "eeprom.bin"));
        {
            FILE *file = mdkr_fopen_utf8(eeprom, "wb");
            expect("wrote legacy save file", file != NULL);
            if (file != NULL) {
                (void)fputc(0, file);
                (void)fclose(file);
            }
        }
        expect("a populated legacy save is grandfathered too",
               mdkr_user_save_directory(resolved, sizeof(resolved)) &&
               strcmp(resolved, save_subdir) == 0);
        (void)mdkr_remove_utf8(eeprom);
        (void)mdkr_rmdir_utf8(save_subdir);
    }

    if (s_failures != 0) {
        fprintf(stderr, "%d save-resolution test(s) failed\n", s_failures);
        return 1;
    }
    puts(legacy_mode ? "save resolution (legacy): PASS"
                     : "save resolution (per-user): PASS");
    return 0;
}
