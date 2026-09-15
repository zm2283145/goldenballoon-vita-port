/* The registry's per-asset scan, driven over a real installed-cache directory.
 *
 * This exists for one defect. registry_init() declared `uint8_t
 * animation_moves[64]` and indexed it with entry.stats.animations, which
 * validate_references() admits up to 256, so an asset with 65 or more
 * animations wrote past it -- 192 bytes of the surrounding stack frame at the
 * ceiling. It needs no malice: the asset compiler emits one record per glTF
 * animation with no cap, so an ordinary 65-clip character does it, and
 * registry_init runs at every boot and every roster refresh without the
 * character ever being selected.
 *
 * Nothing could see it. fuzz_modern_character_asset never called registry_init,
 * and check_array_bounds_sweep drives retail routes whose fixtures never exceed
 * 64 animations. This test is the missing instrument: it walks the same public
 * entry point the launcher does, over a fixture carrying exactly one more
 * animation than the old array held. Built with ASan in the sanitizer arm, the
 * pre-fix code reports a stack-buffer-overflow WRITE at the
 * `animation_moves[index] = 1u` line; the fixed code is clean.
 */
#include "modern_character_registry.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures;

static void expect(int condition, const char *what) {
    printf("%s   %s\n", condition ? "ok  " : "FAIL", what);
    if (!condition) failures++;
}

static int copy_file(const char *from, const char *to) {
    FILE *in = fopen(from, "rb");
    FILE *out;
    unsigned char buffer[4096];
    size_t got;
    if (in == NULL) return 0;
    out = fopen(to, "wb");
    if (out == NULL) { fclose(in); return 0; }
    while ((got = fread(buffer, 1, sizeof buffer, in)) != 0u) {
        if (fwrite(buffer, 1, got, out) != got) { fclose(in); fclose(out); return 0; }
    }
    fclose(in);
    return fclose(out) == 0;
}

int main(int argc, char **argv) {
    MdkrModernCharacterRegistry registry;
    char directory[] = "/tmp/mdkr_registry_test_XXXXXX";
    char installed[512];
    const char *fixture;

    if (argc < 2) {
        fprintf(stderr, "usage: %s <sixtyfive-animations.mdkc>\n", argv[0]);
        return 2;
    }
    fixture = argv[1];
    if (mkdtemp(directory) == NULL) {
        fprintf(stderr, "could not create a scratch cache directory\n");
        return 2;
    }
    snprintf(installed, sizeof installed, "%s/probe.mdkc", directory);
    if (!copy_file(fixture, installed)) {
        fprintf(stderr, "could not stage %s\n", fixture);
        return 2;
    }

    memset(&registry, 0, sizeof registry);
    (void)mdkr_modern_character_registry_init(&registry, directory);

    /* The point is that the scan COMPLETES over an asset with more animations
     * than the old local held. A skip would mean the fixture never reached the
     * loop, and the test would be proving nothing. */
    expect(mdkr_modern_character_registry_count(&registry) == 1,
           "a 65-animation character is admitted by the registry scan");
    expect(mdkr_modern_character_registry_skipped(&registry) == 0,
           "and is not skipped before the animation walk runs");

    (void)remove(installed);
    (void)remove(directory);
    if (failures != 0) {
        fprintf(stderr, "modern character registry: %d failure(s)\n", failures);
        return 1;
    }
    puts("modern character registry: PASS -- the animation walk survives a "
         "count past the old fixed bound");
    return 0;
}
