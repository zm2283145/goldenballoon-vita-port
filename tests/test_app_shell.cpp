// test_app_shell.cpp — ROM-free, window-free unit tests for the app shell's
// pure logic: the launcher/engine argv triage and the ROM picker's validator.
//
// The triage tests are the load-bearing ones. The shell owns main(), so a bug
// there does not merely misroute a flag — it opens a window inside a headless
// gate and hangs it. These assert the deny-by-default rule directly, including
// against a census of the flags the real harness actually passes.
#include "arg_triage.h"
#include "rom_validate.h"
#include "asset_enums.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

static int g_failures = 0;

struct ValidationProgressProbe {
    unsigned calls = 0;
    unsigned last = 0;
    unsigned total = 0;
};

static int cancel_after_first_chunk(unsigned completed, unsigned total,
                                    void *opaque) {
    auto *probe = static_cast<ValidationProgressProbe *>(opaque);
    probe->calls++;
    probe->last = completed;
    probe->total = total;
    return completed < 64u * 1024u;
}

static void expect(bool cond, const char *what) {
    if (!cond) {
        std::printf("FAIL: %s\n", what);
        ++g_failures;
    } else {
        std::printf("ok: %s\n", what);
    }
}

static void write_be32(std::vector<uint8_t> &bytes, size_t offset,
                       uint32_t value) {
    bytes[offset] = static_cast<uint8_t>(value >> 24u);
    bytes[offset + 1u] = static_cast<uint8_t>(value >> 16u);
    bytes[offset + 2u] = static_cast<uint8_t>(value >> 8u);
    bytes[offset + 3u] = static_cast<uint8_t>(value);
}

static void write_be_float(std::vector<uint8_t> &bytes, size_t offset,
                           float value) {
    uint32_t bits = 0u;
    std::memcpy(&bits, &value, sizeof(bits));
    write_be32(bytes, offset, bits);
}

struct DonorProfileRomFixture {
    static constexpr uint32_t kLutStart = 0x40u;
    static constexpr uint32_t kAssetBase = 0x200u;
    static constexpr uint32_t kMiscAssetWords = 16u;
    static constexpr uint32_t kMiscAssetCount = 46u;
    static constexpr uint32_t kMiscSize =
        kMiscAssetCount * kMiscAssetWords * 4u;
    static constexpr uint32_t kMiscTableSize =
        (kMiscAssetCount + 1u) * 4u;
    static constexpr uint32_t kObjectHeaderSize = 0x78u;
    static constexpr uint32_t kObjectHeaderCount =
        MDKR_DONOR_VEHICLE_COUNT * MDKR_DONOR_GAMEPLAY_PROFILE_COUNT;
    static constexpr uint32_t kObjectHeaderTableSize =
        (kObjectHeaderCount + 1u) * 4u;
    static constexpr uint32_t kObjectHeadersSize =
        kObjectHeaderCount * kObjectHeaderSize;
    static constexpr uint32_t kTranslationSize =
        ASSET_LEVEL_OBJECT_TRANSLATION_TABLE_COUNT * 2u;
    static constexpr uint32_t kAfterMisc = kMiscSize + kMiscTableSize;
    static constexpr uint32_t kObjectHeaderTableStart = kAfterMisc;
    static constexpr uint32_t kObjectHeadersStart =
        kObjectHeaderTableStart + kObjectHeaderTableSize;
    static constexpr uint32_t kTranslationStart =
        kObjectHeadersStart + kObjectHeadersSize;
    static constexpr uint32_t kAssetDataSize =
        kTranslationStart + kTranslationSize;
    std::vector<uint8_t> rom = std::vector<uint8_t>(
        kAssetBase + kAssetDataSize + 0x100u, 0u);
    DkrRomId id = {};

    DonorProfileRomFixture() {
        id.assetLutStart = kLutStart;
        id.assetLutEnd = kAssetBase;
        id.romEnd = kAssetBase + kAssetDataSize;

        /* The relevant sections are deliberately separated by empty sections
         * so the fixture exercises the real master-LUT addressing contract. */
        constexpr uint32_t sectionCount =
            ASSET_LEVEL_OBJECT_TRANSLATION_TABLE + 1u;
        write_be32(rom, kLutStart, sectionCount);
        for (uint32_t section = 0u; section <= sectionCount; ++section) {
            uint32_t offset = 0u;
            if (section == ASSET_MISC_TABLE) offset = kMiscSize;
            if (section > ASSET_MISC_TABLE &&
                section <= ASSET_OBJECT_HEADERS_TABLE) offset = kAfterMisc;
            if (section == ASSET_OBJECTS) offset = kObjectHeadersStart;
            if (section == ASSET_LEVEL_OBJECT_TRANSLATION_TABLE) {
                offset = kTranslationStart;
            }
            if (section > ASSET_LEVEL_OBJECT_TRANSLATION_TABLE) {
                offset = kAssetDataSize;
            }
            write_be32(rom, kLutStart + (section + 1u) * 4u, offset);
        }

        const size_t table = kAssetBase + kMiscSize;
        for (uint32_t asset = 0u; asset <= kMiscAssetCount; ++asset) {
            write_be32(rom, table + asset * 4u,
                       asset * kMiscAssetWords);
        }

        for (uint32_t donor = 0u; donor <
             MDKR_DONOR_GAMEPLAY_PROFILE_COUNT; ++donor) {
            writeMiscFloat(ASSET_MISC_RACER_WEIGHT, donor,
                           1.0f + static_cast<float>(donor) * 0.25f);
            writeMiscFloat(ASSET_MISC_RACER_HANDLING, donor,
                           -0.5f + static_cast<float>(donor) * 0.125f);
        }
        static const uint8_t curveIds[MDKR_DONOR_GAMEPLAY_PROFILE_COUNT] = {
            ASSET_MISC_RACERACCELERATION_KRUNCH,
            ASSET_MISC_RACERACCELERATION_BUMPER,
            ASSET_MISC_RACERACCELERATION_TIPTUP,
            ASSET_MISC_RACERACCELERATION_CONKER,
            ASSET_MISC_RACERACCELERATION_TIMBER,
            ASSET_MISC_RACERACCELERATION_BANJO,
            ASSET_MISC_RACERACCELERATION_DRUMSTICK,
            ASSET_MISC_RACERACCELERATION_PIPSY,
            ASSET_MISC_RACERACCELERATION_TT,
            ASSET_MISC_RACERACCELERATION_DIDDY,
        };
        for (uint32_t curve = 0u; curve <
             MDKR_DONOR_GAMEPLAY_PROFILE_COUNT; ++curve) {
            for (uint32_t sample = 0u; sample <
                 MDKR_DONOR_ACCELERATION_SAMPLES; ++sample) {
                writeMiscFloat(curveIds[curve], sample,
                               static_cast<float>(curve) +
                               static_cast<float>(sample) * 0.01f);
            }
        }

        static const uint16_t racerObjectIds[MDKR_DONOR_VEHICLE_COUNT]
                                            [MDKR_DONOR_GAMEPLAY_PROFILE_COUNT] = {
            {
                ASSET_OBJECT_ID_KREMCAR, ASSET_OBJECT_ID_BADGERCAR,
                ASSET_OBJECT_ID_TORTCAR, ASSET_OBJECT_ID_CONKACAR,
                ASSET_OBJECT_ID_TIGERCAR, ASSET_OBJECT_ID_BANJOCAR,
                ASSET_OBJECT_ID_CHICKENCAR, ASSET_OBJECT_ID_MOUSECAR,
                ASSET_OBJECT_ID_SWCAR, ASSET_OBJECT_ID_DIDDYCAR,
            },
            {
                ASSET_OBJECT_ID_KREMLINHOVER, ASSET_OBJECT_ID_BADGERHOVER,
                ASSET_OBJECT_ID_TORTHOVER, ASSET_OBJECT_ID_CONKAHOVER,
                ASSET_OBJECT_ID_TIGERHOVER, ASSET_OBJECT_ID_BANJOHOVER,
                ASSET_OBJECT_ID_CHICKENHOVER, ASSET_OBJECT_ID_MOUSEHOVER,
                ASSET_OBJECT_ID_TICKTOCKHOVER, ASSET_OBJECT_ID_DIDDYHOVER,
            },
            {
                ASSET_OBJECT_ID_KREMPLANE, ASSET_OBJECT_ID_BADGERPLANE,
                ASSET_OBJECT_ID_TORTPLANE, ASSET_OBJECT_ID_CONKA,
                ASSET_OBJECT_ID_TIGPLANE, ASSET_OBJECT_ID_BANJOPLANE,
                ASSET_OBJECT_ID_CHICKENPLANE, ASSET_OBJECT_ID_MOUSEPLANE,
                ASSET_OBJECT_ID_TICKTOCKPLANE, ASSET_OBJECT_ID_DIDDYPLANE,
            },
        };
        const size_t headerTable = kAssetBase + kObjectHeaderTableStart;
        const size_t headers = kAssetBase + kObjectHeadersStart;
        const size_t translation = kAssetBase + kTranslationStart;
        for (uint32_t header = 0u; header <= kObjectHeaderCount; ++header) {
            write_be32(rom, headerTable + header * 4u,
                       header * kObjectHeaderSize);
        }
        for (uint32_t vehicle = 0u; vehicle < MDKR_DONOR_VEHICLE_COUNT;
             ++vehicle) {
            for (uint32_t donor = 0u; donor <
                 MDKR_DONOR_GAMEPLAY_PROFILE_COUNT; ++donor) {
                const uint32_t header =
                    vehicle * MDKR_DONOR_GAMEPLAY_PROFILE_COUNT + donor;
                const uint32_t objectId = racerObjectIds[vehicle][donor];
                const uint32_t curve =
                    (donor + vehicle) % MDKR_DONOR_GAMEPLAY_PROFILE_COUNT;
                rom[translation + objectId * 2u] =
                    static_cast<uint8_t>(header >> 8u);
                rom[translation + objectId * 2u + 1u] =
                    static_cast<uint8_t>(header);
                rom[headers + header * kObjectHeaderSize + 0x5Cu] =
                    curveIds[curve];
            }
        }
    }

    void writeMiscFloat(uint32_t asset, uint32_t element, float value) {
        const size_t offset = kAssetBase +
            (asset * kMiscAssetWords + element) * 4u;
        write_be_float(rom, offset, value);
    }
};

// Build an argv from a vector of strings and run the triage over it.
static int triage(const std::vector<std::string> &args) {
    std::vector<char *> argv;
    std::vector<std::string> owned = args;
    owned.insert(owned.begin(), "mdkr64");
    for (std::string &s : owned) argv.push_back(&s[0]);
    argv.push_back(nullptr);
    return mdkr_is_automation_invocation((int)owned.size(), argv.data());
}

static void test_triage() {
    // The ONLY interactive shapes.
    expect(triage({}) == 0, "bare invocation opens the launcher");
    expect(triage({"--ui"}) == 0, "--ui opens the launcher");
    expect(triage({"--ui", "--rom", "x.z64"}) == 0, "--ui wins over other flags");

    // Every flag the harness actually passes must bypass the shell. This list is
    // the census taken from tests/, tools/ and .github/ — if someone adds a flag
    // here they are documenting a real call site, and deny-by-default means the
    // rule still holds for flags nobody thought to list.
    const char *harness[] = {
        "--rom", "--headless-frames", "--dump-frames", "--input-script",
        "--window-size", "--video-list", "--video-set", "--pure", "--restored",
        "--remastered", "--renderer", "--version", "--help", "-h",
        "--video-launch-mode", "--video-launch-set", "--video-launch-persist",
        "--aspect", "--fov", "--max-hfov", "--widescreen", "--legacy-stretch",
    };
    for (const char *f : harness) {
        char msg[128];
        std::snprintf(msg, sizeof(msg), "%s bypasses the launcher", f);
        expect(triage({f}) == 1, msg);
    }

    // A bare positional ROM is still an engine invocation, not a launcher seed.
    expect(triage({"baserom.us.v80.z64"}) == 1, "positional ROM bypasses the launcher");
    // And an unknown/future flag defaults to the SAFE side.
    expect(triage({"--some-flag-invented-tomorrow"}) == 1,
           "unknown flag bypasses the launcher (deny by default)");

    expect(mdkr_argv_requests_ui(0, nullptr) == 0, "null argv is not a --ui request");

    std::vector<std::string> versionOwned = {"mdkr64", "--version"};
    std::vector<char *> versionArgv;
    for (std::string &s : versionOwned) versionArgv.push_back(&s[0]);
    expect(mdkr_is_side_effect_free_info_invocation(
               (int)versionOwned.size(), versionArgv.data()) == 1,
           "exact --version bypasses mutable path initialization");

    std::vector<std::string> helpOwned = {"mdkr64", "--help"};
    std::vector<char *> helpArgv;
    for (std::string &s : helpOwned) helpArgv.push_back(&s[0]);
    expect(mdkr_is_side_effect_free_info_invocation(
               (int)helpOwned.size(), helpArgv.data()) == 1,
           "exact --help bypasses mutable path initialization");

    std::vector<std::string> compoundOwned = {
        "mdkr64", "--video-set", "Video.Mode=Pure", "--version"};
    std::vector<char *> compoundArgv;
    for (std::string &s : compoundOwned) compoundArgv.push_back(&s[0]);
    expect(mdkr_is_side_effect_free_info_invocation(
               (int)compoundOwned.size(), compoundArgv.data()) == 0,
           "compound --version preserves engine argument ordering");
}

static void test_rom_validate() {
    // A path that does not exist must be reported, not crash.
    RomInfo missing = mdkr_validate_rom("/nonexistent/definitely-not-here.z64");
    expect(missing.valid == 0, "missing file is not valid");
    expect(missing.message[0] != '\0', "missing file has a message");

    RomInfo none = mdkr_validate_rom(nullptr);
    expect(none.valid == 0, "null path is not valid");
    expect(none.message[0] != '\0', "null path has a message");

    // A file that is too short to hold a header.
    const char *shortPath = "test_app_shell_short.bin";
    if (std::FILE *f = std::fopen(shortPath, "wb")) {
        std::fputs("nope", f);
        std::fclose(f);
        RomInfo tiny = mdkr_validate_rom(shortPath);
        expect(tiny.valid == 0, "truncated file is not valid");
        expect(std::strstr(tiny.message, "truncated") != nullptr,
               "truncated file explains that it is truncated");
        std::remove(shortPath);
    }

    // A full cartridge-sized file with the wrong magic reaches the byte-order
    // gate rather than being rejected only because it is short.
    const char *junkPath = "test_app_shell_junk.bin";
    if (std::FILE *f = std::fopen(junkPath, "wb")) {
        unsigned char junk[64 * 1024];
        std::memset(junk, 0xAB, sizeof(junk));
        for (unsigned i = 0; i < DKR_ROM_SIZE_BYTES / sizeof(junk); ++i) {
            std::fwrite(junk, 1, sizeof(junk), f);
        }
        std::fclose(f);
        ValidationProgressProbe probe;
        RomInfo cancelled = mdkr_validate_rom_progress(
            junkPath, cancel_after_first_chunk, &probe);
        expect(cancelled.cancelled == 1 && cancelled.valid == 0,
               "chunked ROM validation honors cancellation");
        expect(probe.calls == 2 && probe.last == 64u * 1024u &&
                   probe.total == DKR_ROM_SIZE_BYTES,
               "ROM validation reports bounded read progress");
        RomInfo j = mdkr_validate_rom(junkPath);
        expect(j.valid == 0, "non-N64 header is not valid");
        expect(j.validation_code == DKR_ROM_VALIDATION_WRONG_BYTE_ORDER,
               "full-size non-N64 reaches the byte-order verdict");
        expect(std::strstr(j.message, "not an N64 ROM") != nullptr,
               "non-N64 header says so");
        std::remove(junkPath);
    }

    // The supported list must be non-empty and name both accepted revisions —
    // this is the string the picker shows when a player's ROM is refused, so an
    // empty one would leave them with no idea what to get.
    const char *list = mdkr_supported_rom_list();
    expect(list != nullptr && list[0] != '\0', "supported list is non-empty");
    expect(std::strstr(list, "us.v80") != nullptr, "supported list names us.v80");
    expect(std::strstr(list, "pal.v80") != nullptr, "supported list names pal.v80");

    const char *usSha = dkr_rom_reference_sha256("us.v80");
    const char *palSha = dkr_rom_reference_sha256("pal.v80");
    expect(usSha != nullptr && std::strlen(usSha) == 64,
           "us.v80 has a complete SHA-256 identity");
    expect(palSha != nullptr && std::strlen(palSha) == 64,
           "pal.v80 has a complete SHA-256 identity");
    expect(dkr_rom_reference_sha256("us.v77") == nullptr,
           "unsupported revisions have no accidental acceptance identity");
}

static void test_donor_gameplay_profiles() {
    DonorProfileRomFixture fixture;
    MdkrDonorGameplayProfiles profiles = {};
    char error[192] = "not cleared";
    expect(mdkr_donor_gameplay_profiles_from_rom(
               fixture.rom.data(), static_cast<uint32_t>(fixture.rom.size()),
               &fixture.id, &profiles, error, sizeof(error)) == 1,
           "bounded donor gameplay tables parse from canonical ROM data");
    expect(error[0] == '\0', "successful donor parse clears stale error text");
    expect(profiles.available == 1u &&
               profiles.version == MDKR_DONOR_GAMEPLAY_PROFILE_VERSION &&
               profiles.donor_count == MDKR_DONOR_GAMEPLAY_PROFILE_COUNT,
           "donor profile summary publishes its exact bounded shape");
    expect(std::fabs(profiles.donor[0].weight - 0.45f) < 0.00001f &&
               std::fabs(profiles.donor[9].weight - 1.4625f) < 0.00001f,
           "donor weights include the game's authored 0.45 coefficient");
    expect(std::fabs(profiles.donor[3].handling - (-0.125f)) < 0.00001f,
           "donor handling preserves signed authored coefficients");
    expect(std::fabs(profiles.donor[0].acceleration
                         [MDKR_DONOR_VEHICLE_CAR][13] - 0.13f) < 0.00001f &&
               std::fabs(profiles.donor[9].acceleration
                         [MDKR_DONOR_VEHICLE_PLANE][6] - 1.06f) < 0.00001f,
           "donor and vehicle headers resolve each exact acceleration curve");

    DkrRomId shortLut = fixture.id;
    shortLut.assetLutEnd = DonorProfileRomFixture::kLutStart + 8u;
    std::memset(&profiles, 0xA5, sizeof(profiles));
    expect(mdkr_donor_gameplay_profiles_from_rom(
               fixture.rom.data(), static_cast<uint32_t>(fixture.rom.size()),
               &shortLut, &profiles, error, sizeof(error)) == 0,
           "truncated asset LUT cannot escape donor parser bounds");
    expect(profiles.available == 0u && profiles.donor_count == 0u,
           "failed donor parsing withholds the entire summary");

    DonorProfileRomFixture invalidHeader;
    const size_t translation = DonorProfileRomFixture::kAssetBase +
        DonorProfileRomFixture::kTranslationStart;
    invalidHeader.rom[translation + ASSET_OBJECT_ID_KREMCAR * 2u] = 0xFFu;
    invalidHeader.rom[translation + ASSET_OBJECT_ID_KREMCAR * 2u + 1u] =
        0xFEu;
    std::memset(&profiles, 0xA5, sizeof(profiles));
    expect(mdkr_donor_gameplay_profiles_from_rom(
               invalidHeader.rom.data(),
               static_cast<uint32_t>(invalidHeader.rom.size()),
               &invalidHeader.id, &profiles, error, sizeof(error)) == 0,
           "out-of-range donor object-header translations are rejected");
    expect(profiles.available == 0u && profiles.donor[0].handling == 0.0f,
           "invalid vehicle metadata cannot leak scalar profile evidence");

    fixture.writeMiscFloat(ASSET_MISC_RACERACCELERATION_KRUNCH, 4u,
                           NAN);
    std::memset(&profiles, 0xA5, sizeof(profiles));
    expect(mdkr_donor_gameplay_profiles_from_rom(
               fixture.rom.data(), static_cast<uint32_t>(fixture.rom.size()),
               &fixture.id, &profiles, error, sizeof(error)) == 0,
           "non-finite acceleration samples are rejected");
    expect(profiles.available == 0u && profiles.donor[0].weight == 0.0f,
           "invalid donor data cannot leak a partial trusted summary");

    expect(mdkr_donor_gameplay_profiles_from_rom(
               fixture.rom.data(), static_cast<uint32_t>(fixture.rom.size()),
               &fixture.id, nullptr, error, sizeof(error)) == 0 &&
               std::strstr(error, "output") != nullptr,
           "missing donor output is rejected with a bounded explanation");
}

int main(void) {
    test_triage();
    test_rom_validate();
    test_donor_gameplay_profiles();
    if (g_failures) {
        std::printf("\n%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("\nall app-shell tests passed\n");
    return 0;
}
