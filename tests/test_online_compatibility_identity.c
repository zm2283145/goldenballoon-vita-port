#include "platform/online/compatibility_identity.h"

#include <stdio.h>
#include <string.h>

/* The gameplay digest folds in THIS build's OS tag (compatibility_identity.c):
 * cross-OS gameplay determinism (macOS<->Windows/Linux, native<->browser) is
 * unproven, so two identities from different OSes must differ in
 * gameplay_digest -- and ONLY there for identical provenance -- and the lobby
 * JOIN byte-compare must refuse the pair, while same-OS peers still derive
 * byte-identical values. The vectors below are exact SHA-256 values for the
 * fixed test commit (the gameplay material is commit + contract + os tag;
 * the version participates only in build_id). THIS_OS_GAMEPLAY_HEX is the
 * tag this compile must select; OTHER_OS_GAMEPLAY_HEX is a genuinely
 * different platform's vector for the SAME provenance. */
#if defined(__APPLE__)
#define THIS_OS_GAMEPLAY_HEX /* os=macos */ \
    "04077dc7ef67f7088baee5de99d4bca470c533ae0ed9fb4fabb679b7deaa0b42"
#define OTHER_OS_GAMEPLAY_HEX /* os=windows */ \
    "b62a236587d1981d6c992721aca1af3c9160b59888719f46bfb309cdb7ac085e"
#elif defined(_WIN32)
#define THIS_OS_GAMEPLAY_HEX /* os=windows */ \
    "b62a236587d1981d6c992721aca1af3c9160b59888719f46bfb309cdb7ac085e"
#define OTHER_OS_GAMEPLAY_HEX /* os=macos */ \
    "04077dc7ef67f7088baee5de99d4bca470c533ae0ed9fb4fabb679b7deaa0b42"
#elif defined(__linux__)
#define THIS_OS_GAMEPLAY_HEX /* os=linux */ \
    "60a4e5cdb9c37700263fcad00a4fae4b5381ddbee549ec70c039622c23f94abe"
#define OTHER_OS_GAMEPLAY_HEX /* os=macos */ \
    "04077dc7ef67f7088baee5de99d4bca470c533ae0ed9fb4fabb679b7deaa0b42"
#else
#error "no pinned per-OS gameplay-digest vector for this platform"
#endif

/* The pre-fence OS-independent digest for the same commit. It admitted
 * unproven cross-OS pairs, so it must be RETIRED: no platform may derive it
 * any longer. */
#define LEGACY_UNTAGGED_GAMEPLAY_HEX \
    "fb34ef8ddfcf782a852375b8ce71d1bcd552a185cf2c18fe1e7727996b749522"

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

static uint8_t hex_nibble(char value) {
    return (uint8_t)(value >= 'a' ? value - 'a' + 10 : value - '0');
}

static void bytes_from_hex(uint8_t *bytes, size_t count, const char *hex) {
    size_t index;
    for (index = 0u; index < count; index++)
        bytes[index] = (uint8_t)((hex_nibble(hex[index * 2u]) << 4u) |
                                 hex_nibble(hex[index * 2u + 1u]));
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
                               THIS_OS_GAMEPLAY_HEX),
           "build_id equals the shared native/browser vector and the gameplay "
           "digest equals this OS's exact per-tag vector");
    expect(!bytes_equal_hex(compatibility.gameplay_digest,
                            sizeof(compatibility.gameplay_digest),
                            LEGACY_UNTAGGED_GAMEPLAY_HEX),
           "the OS-untagged legacy gameplay digest is retired (it admitted "
           "unproven cross-OS pairs)");
    expect(mdkr_online_compatibility_from_provenance("1.3.0", commit, false, 2u,
                                                     &compatibility) &&
               compatibility.rom_revision == 1u &&
               compatibility.cadence_hz == 30u &&
               bytes_equal_hex(compatibility.build_id,
                               sizeof(compatibility.build_id),
                               "336892f0abf0a2c25c91a7ebcb266364"),
           "the accepted PAL payload publishes the shared online identity");

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
     * change at least one compared field. The byte-identical arm is also the
     * self-consistency proof for the C1 fence
     * (online_live_wiring.cpp OnlineRoom_makeGatedLiveAdapter): it recomputes
     * the identity through this same derivation, so with the OS tag folded
     * into the gameplay digest a same-binary recomputation still compares
     * equal by construction. Lobby-level rejection of a mismatched join is
     * covered by test_online_lobby_core.c's "incompatible" cases. */
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
        /* Cross-region admission: us.v80 (revision 1) and pal.v80 (revision
         * 2) carry byte-identical race payloads, so both accepted revisions
         * must derive ONE shared identity -- revision 1 at the online 30 Hz
         * cadence -- and the lobby JOIN byte-compare must admit a US<->EU
         * pair exactly like a same-region pair.  This is the loopback
         * equivalent of the reducer's admission decision. */
        expect(mdkr_online_compatibility_from_provenance(
                   "1.6.0", commit, false, 2u, &second) &&
                   memcmp(&second, &first, sizeof(first)) == 0 &&
                   second.rom_revision == 1u && second.cadence_hz == 30u,
               "US and EU ROM revisions derive one byte-identical identity");
        {
            MdkrOnlineLobby lobby;
            MdkrOnlineCommand join_command;
            MdkrOnlineStep step;
            expect(mdkr_online_lobby_init(&lobby, 1u, 100u, &first, 1u),
                   "US-derived identity hosts a lobby");
            memset(&join_command, 0, sizeof(join_command));
            join_command.protocol_version = MDKR_ONLINE_PROTOCOL_VERSION;
            join_command.expected_revision = lobby.revision;
            join_command.command_id = 1u;
            join_command.actor_endpoint_id = 200u;
            join_command.type = MDKR_ONLINE_JOIN;
            join_command.value = 1u;
            join_command.compatibility = second;
            step = mdkr_online_lobby_dispatch(&lobby, &join_command);
            expect(step.accepted && step.error == MDKR_ONLINE_OK,
                   "EU-derived endpoint is admitted by a US-hosted lobby");

            /* Same-OS fence, refusal side: a peer whose identity differs from
             * the host's ONLY in the gameplay digest's OS tag (same version,
             * commit, ROM revision and cadence -- build_id identical) must
             * hit the EXISTING clean incompatibility refusal, exactly the
             * flow a mismatched build already gets
             * (MDKR_ONLINE_ERROR_INCOMPATIBLE ->
             * match_live_adapter.cpp MDKR_ONLINE_VIEW_FAILURE_DIFFERENT_BUILD
             * -> lobby_view_model.c "Game Update Needed"). */
            {
                MdkrOnlineCompatibilityV1 cross_os = first;
                bytes_from_hex(cross_os.gameplay_digest,
                               sizeof(cross_os.gameplay_digest),
                               OTHER_OS_GAMEPLAY_HEX);
                expect(memcmp(cross_os.build_id, first.build_id,
                              sizeof(first.build_id)) == 0 &&
                           memcmp(cross_os.gameplay_digest,
                                  first.gameplay_digest,
                                  sizeof(first.gameplay_digest)) != 0,
                       "the cross-OS pair differs ONLY in the gameplay "
                       "digest's OS tag");
                join_command.command_id = 2u;
                join_command.actor_endpoint_id = 300u;
                join_command.expected_revision = lobby.revision;
                join_command.compatibility = cross_os;
                step = mdkr_online_lobby_dispatch(&lobby, &join_command);
                expect(!step.accepted &&
                           step.error == MDKR_ONLINE_ERROR_INCOMPATIBLE,
                       "a cross-OS endpoint is refused by the JOIN "
                       "byte-compare with the existing INCOMPATIBLE error");
            }
        }
    }

    if (failures != 0)
        return 1;
    puts("online compatibility identity: PASS (exact per-OS vectors; "
         "same-OS admit, cross-OS refusal)");
    return 0;
}
