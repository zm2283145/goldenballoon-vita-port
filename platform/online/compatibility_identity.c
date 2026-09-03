#include "compatibility_identity.h"

#include <string.h>

#include "sha256.h"

/* Same-OS fence: this release ships online enabled on every platform, but
 * cross-OS gameplay determinism (macOS<->Windows/Linux, native<->browser) is
 * UNPROVEN -- an admitted cross-OS pair would desync mid-race. The gameplay
 * contract below therefore folds in a compile-time OS tag, so same-OS peers
 * of the same build still derive byte-identical identities (the lobby JOIN
 * byte-compare admits them) while a cross-OS pair differs in gameplay_digest
 * and hits the existing clean incompatibility refusal at join. The browser
 * publisher (dist/web/mdkr64-shell.js publishOnlineCompatibility) mirrors
 * this exactly with its fixed "os=browser" field; a browser build of this
 * file must agree with it. An unmapped platform fails the compile: shipping
 * online without deciding its determinism domain is exactly the mistake this
 * fence exists to prevent. */
#if defined(__EMSCRIPTEN__)
#define MDKR_ONLINE_OS_TAG "browser"
#elif defined(__APPLE__)
#define MDKR_ONLINE_OS_TAG "macos"
#elif defined(_WIN32)
#define MDKR_ONLINE_OS_TAG "windows"
#elif defined(__linux__)
#define MDKR_ONLINE_OS_TAG "linux"
#elif defined(__vita__)
/* Online itself stays off on Vita (MDKR_ENABLE_ONLINE_BETA is not set by the
 * Vita build) -- this only satisfies the compile-time fence above so the
 * file (unconditionally part of PLATFORM_SOURCES) compiles. If online is
 * ever enabled on Vita later, this correctly makes it its own determinism
 * domain rather than silently colliding with another platform's tag. */
#define MDKR_ONLINE_OS_TAG "vita"
#else
#error "online OS tag unmapped for this platform: prove its gameplay \
determinism domain and add it here (and to the pinned unit vectors) before \
shipping online on it"
#endif

static bool decimal_component(const char **cursor) {
    const char *value = *cursor;
    if (*value < '0' || *value > '9')
        return false;
    do {
        value++;
    } while (*value >= '0' && *value <= '9');
    *cursor = value;
    return true;
}

static size_t bounded_length(const char *value, size_t maximum) {
    size_t length;
    if (value == NULL)
        return maximum + 1u;
    for (length = 0u; length <= maximum; length++) {
        if (value[length] == '\0')
            return length;
    }
    return maximum + 1u;
}

static bool valid_version(const char *version) {
    const char *cursor = version;
    size_t length;
    if (version == NULL)
        return false;
    length = bounded_length(version, 32u);
    if (length == 0u || length > 32u || !decimal_component(&cursor) ||
        *cursor++ != '.' || !decimal_component(&cursor) || *cursor++ != '.' ||
        !decimal_component(&cursor))
        return false;
    return *cursor == '\0';
}

static bool valid_commit(const char *commit) {
    unsigned index;
    if (bounded_length(commit, 40u) != 40u)
        return false;
    for (index = 0u; index < 40u; index++) {
        const char value = commit[index];
        if (!((value >= '0' && value <= '9') || (value >= 'a' && value <= 'f')))
            return false;
    }
    return true;
}

static void hash_prefix(MdkrSha256 *hash, const char *label) {
    static const char product[] = "golden-balloon\n";
    static const char protocol[] = "\nv1\n";
    mdkr_sha256_init(hash);
    mdkr_sha256_update(hash, product, sizeof(product) - 1u);
    mdkr_sha256_update(hash, label, strlen(label));
    mdkr_sha256_update(hash, protocol, sizeof(protocol) - 1u);
}

bool mdkr_online_compatibility_from_provenance(
    const char *version, const char *source_commit, bool source_dirty,
    uint8_t rom_revision, MdkrOnlineCompatibilityV1 *output) {
    static const char build_label[] = "online-build";
    static const char gameplay_label[] = "gameplay-contract";
    static const char gameplay_contract[] =
        "\nprotocol=1\nrules=standard-race\nrollback=bounded-v1"
        "\nos=" MDKR_ONLINE_OS_TAG;
    MdkrOnlineCompatibilityV1 next;
    MdkrSha256 hash;
    uint8_t digest[MDKR_SHA256_DIGEST_SIZE];
    if (output == NULL || source_dirty || !valid_version(version) ||
        !valid_commit(source_commit) ||
        (rom_revision != 1u && rom_revision != 2u))
        return false;

    memset(&next, 0, sizeof(next));
    next.protocol_version = MDKR_ONLINE_PROTOCOL_VERSION;
    hash_prefix(&hash, build_label);
    mdkr_sha256_update(&hash, version, strlen(version));
    mdkr_sha256_update(&hash, "\n", 1u);
    mdkr_sha256_update(&hash, source_commit, 40u);
    mdkr_sha256_final(&hash, digest);
    memcpy(next.build_id, digest, sizeof(next.build_id));

    hash_prefix(&hash, gameplay_label);
    mdkr_sha256_update(&hash, source_commit, 40u);
    mdkr_sha256_update(&hash, gameplay_contract,
                       sizeof(gameplay_contract) - 1u);
    mdkr_sha256_final(&hash, next.gameplay_digest);
    /* Both accepted revisions (us.v80=1, pal.v80=2) carry byte-identical race
     * payloads -- the regions differ only in authored cadence, and an online
     * session always races the 30 Hz online cadence (a PAL endpoint's online
     * epoch adopts the NTSC source identity; see platform/rom_io.c).  Publish
     * the ONE shared identity so the lobby JOIN byte-compare admits
     * cross-region peers; the ROM's true region stays with provenance and
     * language, never in this comparand. */
    next.rom_revision = 1u;
    next.cadence_hz = 30u;
    if (!mdkr_online_compatibility_valid(&next))
        return false;
    *output = next;
    return true;
}
