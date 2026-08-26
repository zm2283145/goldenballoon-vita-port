#include "platform/online/compatibility_identity.h"

#include <stdio.h>
#include <string.h>

static int failures;

static void expect(int condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        failures++;
    }
}

static int bytes_equal_hex(const uint8_t *bytes, size_t count,
                           const char *hex) {
    static const char digits[] = "0123456789abcdef";
    size_t index;
    for (index = 0u; index < count; index++) {
        if (hex[index * 2u] != digits[bytes[index] >> 4u] ||
            hex[index * 2u + 1u] != digits[bytes[index] & 15u])
            return 0;
    }
    return hex[count * 2u] == '\0';
}

int main(void) {
    static const char commit[] = "0123456789abcdef0123456789abcdef01234567";
    MdkrOnlineCompatibilityV1 compatibility;
    MdkrOnlineCompatibilityV1 untouched;

    memset(&compatibility, 0xa5, sizeof(compatibility));
    untouched = compatibility;
    expect(mdkr_online_compatibility_from_provenance("1.3.0", commit, false, 1u,
                                                     &compatibility),
           "clean semver, commit and supported ROM derive compatibility");
    expect(compatibility.protocol_version == 1u &&
               compatibility.rom_revision == 1u &&
               compatibility.cadence_hz == 30u &&
               bytes_equal_hex(compatibility.build_id,
                               sizeof(compatibility.build_id),
                               "336892f0abf0a2c25c91a7ebcb266364") &&
               bytes_equal_hex(compatibility.gameplay_digest,
                               sizeof(compatibility.gameplay_digest),
                               "fb34ef8ddfcf782a852375b8ce71d1bcd552a185cf2c18f"
                               "e1e7727996b749522"),
           "native bytes equal the browser provenance vector exactly");
    expect(mdkr_online_compatibility_from_provenance("1.3.0", commit, false, 2u,
                                                     &compatibility) &&
               compatibility.rom_revision == 2u &&
               compatibility.cadence_hz == 25u &&
               bytes_equal_hex(compatibility.build_id,
                               sizeof(compatibility.build_id),
                               "336892f0abf0a2c25c91a7ebcb266364"),
           "PAL changes only ROM identity and authored cadence");

    compatibility = untouched;
    expect(!mdkr_online_compatibility_from_provenance("1.3.0", commit, true, 1u,
                                                      &compatibility) &&
               memcmp(&compatibility, &untouched, sizeof(compatibility)) == 0,
           "dirty provenance fails atomically");
    expect(!mdkr_online_compatibility_from_provenance("1.3", commit, false, 1u,
                                                      &compatibility) &&
               !mdkr_online_compatibility_from_provenance(
                   "v1.3.0", commit, false, 1u, &compatibility) &&
               !mdkr_online_compatibility_from_provenance(
                   "1.3.0-dev", commit, false, 1u, &compatibility),
           "only the published three-component version grammar is accepted");
    expect(!mdkr_online_compatibility_from_provenance(
               "12345678901234567890123456789.0.0", commit, false, 1u,
               &compatibility) &&
               memcmp(&compatibility, &untouched, sizeof(compatibility)) == 0,
           "published version identity is bounded to 32 bytes cross-platform");
    expect(!mdkr_online_compatibility_from_provenance(
               "1.3.0", "0123456789ABCDEF0123456789ABCDEF01234567", false, 1u,
               &compatibility) &&
               !mdkr_online_compatibility_from_provenance(
                   "1.3.0", "01234567", false, 1u, &compatibility),
           "commit must be exact lowercase forty-character hex");
    expect(!mdkr_online_compatibility_from_provenance("1.3.0", commit, false,
                                                      3u, &compatibility) &&
               memcmp(&compatibility, &untouched, sizeof(compatibility)) == 0,
           "unsupported ROM revision fails without output mutation");

    /* The property the lobby's JOIN byte-compare (lobby_core.c compatible())
     * depends on, now that the LIVE adapter derives its identity here: two
     * processes of the SAME build with the SAME accepted ROM must derive
     * identical bytes, and any change in version, commit or ROM revision must
     * change at least one compared field. Lobby-level rejection of a
     * mismatched join is covered by test_online_lobby_core.c's
     * "incompatible" cases. */
    {
        static const char other_commit[] =
            "fedcba9876543210fedcba9876543210fedcba98";
        MdkrOnlineCompatibilityV1 first;
        MdkrOnlineCompatibilityV1 second;
        memset(&first, 0x11, sizeof(first));
        memset(&second, 0x22, sizeof(second));
        expect(mdkr_online_compatibility_from_provenance(
                   "1.6.0", commit, false, 1u, &first) &&
                   mdkr_online_compatibility_from_provenance(
                       "1.6.0", commit, false, 1u, &second) &&
                   memcmp(&first, &second, sizeof(first)) == 0,
               "same version+commit+ROM derive byte-identical compatibility");
        expect(mdkr_online_compatibility_from_provenance(
                   "1.6.1", commit, false, 1u, &second) &&
                   memcmp(second.build_id, first.build_id,
                          sizeof(first.build_id)) != 0,
               "a different version changes the build identity");
        expect(mdkr_online_compatibility_from_provenance(
                   "1.6.0", other_commit, false, 1u, &second) &&
                   memcmp(second.build_id, first.build_id,
                          sizeof(first.build_id)) != 0 &&
                   memcmp(second.gameplay_digest, first.gameplay_digest,
                          sizeof(first.gameplay_digest)) != 0,
               "a different commit changes build identity AND gameplay digest");
        expect(mdkr_online_compatibility_from_provenance(
                   "1.6.0", commit, false, 2u, &second) &&
                   (second.rom_revision != first.rom_revision ||
                    second.cadence_hz != first.cadence_hz),
               "a different accepted ROM revision changes the ROM identity");
    }

    if (failures != 0)
        return 1;
    puts("online compatibility identity: PASS (native/browser exact vector)");
    return 0;
}
