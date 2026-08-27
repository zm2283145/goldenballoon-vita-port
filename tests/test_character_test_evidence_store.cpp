#include "character_test_evidence_store.h"
#include "sha256.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {

int  failures;

void expect(bool condition, const char *message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

struct MemoryStorage {
    bool        present   = false;
    bool        failRead  = false;
    bool        failWrite = false;
    unsigned    writes    = 0u;
    std::string bytes;
};

int readMemory(void *context, char *text, size_t capacity, size_t *length) {
    MemoryStorage &storage = *static_cast<MemoryStorage *>(context);
    if (storage.failRead) return -1;
    if (!storage.present) return 0;
    if (storage.bytes.size() >= capacity) return -1;
    storage.bytes.copy(text, storage.bytes.size());
    *length = storage.bytes.size();
    return 1;
}

int writeMemory(void *context, const char *text, size_t length) {
    MemoryStorage &storage = *static_cast<MemoryStorage *>(context);
    if (storage.failWrite) return -1;
    storage.bytes.assign(text, length);
    storage.present = true;
    ++storage.writes;
    return 1;
}

std::string finishDigest(MdkrSha256 &digest) {
    uint8_t bytes[MDKR_SHA256_DIGEST_SIZE];
    static const char digits[] = "0123456789abcdef";
    std::string hex(MDKR_SHA256_DIGEST_SIZE * 2u, '0');
    mdkr_sha256_final(&digest, bytes);
    for (size_t index = 0u; index < sizeof(bytes); ++index) {
        hex[index * 2u] = digits[bytes[index] >> 4u];
        hex[index * 2u + 1u] = digits[bytes[index] & 0xFu];
    }
    return hex;
}

std::string digestFields(const std::vector<std::string> &fields) {
    MdkrSha256 digest;
    mdkr_sha256_init(&digest);
    for (const std::string &field : fields) {
        mdkr_sha256_update(&digest, field.data(), field.size());
        const uint8_t separator = 0u;
        mdkr_sha256_update(&digest, &separator, 1u);
    }
    return finishDigest(digest);
}

std::string digestInventory(const std::string &header,
                            const std::string &count,
                            const std::string &body) {
    MdkrSha256 digest;
    mdkr_sha256_init(&digest);
    const auto add = [&digest](const std::string &value) {
        mdkr_sha256_update(&digest, value.data(), value.size());
        const uint8_t separator = 0u;
        mdkr_sha256_update(&digest, &separator, 1u);
    };
    add(header);
    add(count);
    mdkr_sha256_update(&digest, body.data(), body.size());
    return finishDigest(digest);
}

std::vector<std::string> splitFields(const std::string &line) {
    std::vector<std::string> fields;
    size_t begin = 0u;
    while (true) {
        const size_t tab = line.find('\t', begin);
        fields.push_back(line.substr(
            begin, tab == std::string::npos ? std::string::npos
                                             : tab - begin));
        if (tab == std::string::npos) return fields;
        begin = tab + 1u;
    }
}

std::string legacyFromV3(const std::string &encoded,
                         const char *legacyHeader, size_t retainedFields) {
    const size_t headerEnd = encoded.find('\n');
    if (headerEnd == std::string::npos) return {};
    const std::vector<std::string> header = splitFields(
        encoded.substr(0u, headerEnd));
    if (header.size() != 3u ||
        header[0] != "mdkr-character-test-evidence-v3") return {};
    std::string body;
    size_t begin = headerEnd + 1u;
    while (begin < encoded.size()) {
        const size_t end = encoded.find('\n', begin);
        if (end == std::string::npos) return {};
        std::vector<std::string> fields = splitFields(
            encoded.substr(begin, end - begin));
        if (fields.size() != 105u) return {};
        fields.resize(retainedFields);
        /* V1/V2 predate result contract 10 and therefore cannot claim its
         * required contact witnesses. */
        fields[9] = "9";
        for (const std::string &field : fields) {
            body += field;
            body.push_back('\t');
        }
        body += digestFields(fields);
        body.push_back('\n');
        begin = end + 1u;
    }
    return std::string(legacyHeader) + "\t" + header[1] + "\t" +
        digestInventory(legacyHeader, header[1], body) + "\n" + body;
}

std::string legacyV1FromV3(const std::string &encoded) {
    return legacyFromV3(
        encoded, "mdkr-character-test-evidence-v1", 38u);
}

std::string legacyV2FromV3(const std::string &encoded) {
    return legacyFromV3(
        encoded, "mdkr-character-test-evidence-v2", 51u);
}

std::string authenticatedV3WithFirstRowField(
    const std::string &encoded, size_t fieldIndex,
    const std::string &replacement) {
    const size_t headerEnd = encoded.find('\n');
    const size_t rowEnd = headerEnd == std::string::npos
        ? std::string::npos : encoded.find('\n', headerEnd + 1u);
    if (headerEnd == std::string::npos || rowEnd == std::string::npos) {
        return {};
    }
    const std::vector<std::string> header = splitFields(
        encoded.substr(0u, headerEnd));
    std::vector<std::string> fields = splitFields(
        encoded.substr(headerEnd + 1u, rowEnd - headerEnd - 1u));
    if (header.size() != 3u ||
        header[0] != "mdkr-character-test-evidence-v3" ||
        fields.size() != 105u || fieldIndex >= 104u) return {};
    fields[fieldIndex] = replacement;
    fields.resize(104u);
    std::string firstRow;
    for (const std::string &field : fields) {
        firstRow += field;
        firstRow.push_back('\t');
    }
    firstRow += digestFields(fields);
    firstRow.push_back('\n');
    const std::string body = firstRow + encoded.substr(rowEnd + 1u);
    return header[0] + "\t" + header[1] + "\t" +
        digestInventory(header[0], header[1], body) + "\n" + body;
}

CharacterTestEvidenceStore::Evidence makeEvidence(
    const std::string               &packageId,
    uint32_t                         context,
    uint32_t                         players,
    CharacterTestEvidenceStore::Kind kind =
        CharacterTestEvidenceStore::Kind::Latest) {
    CharacterTestEvidenceStore::Evidence evidence;
    evidence.kind         = kind;
    evidence.packageId    = packageId;
    evidence.capturedUnix = 1777777777u;
    evidence.sourceSha256.assign(64u, 'a');
    evidence.fitSha256.assign(64u, 'b');
    evidence.presentationSha256.assign(64u, 'c');
    evidence.buildVersion                = "1.5.2-test";
    evidence.context                     = context;
    evidence.players                     = players;
    evidence.resultVersion               = 10u;
    evidence.started                     = true;
    evidence.warmupComplete              = true;
    evidence.realtime                    = true;
    evidence.warmupTicks                 = 120u;
    evidence.intervalSamples             = 180u;
    evidence.displayedFrames             = 181u;
    evidence.intervalP50Us               = 16650u;
    evidence.intervalP95Us               = 17100u;
    evidence.intervalP99Us               = 18250u;
    evidence.intervalMeanUs              = 16800u;
    evidence.intervalMaxUs               = 20000u;
    evidence.tickwallSamples             = 180u;
    evidence.tickwallMeanNs              = 1200000u;
    evidence.replacementDraws            = 360u;
    evidence.replacementPrimitives       = 720u;
    evidence.hiddenDonorBatches          = 360u;
    evidence.contactSolves               = context == 1u ? 0u : 720u;
    evidence.contactErrorMeanMicrometres = context == 1u ? 0u : 1200u;
    evidence.contactErrorMaxMicrometres  = context == 1u ? 0u : 3400u;
    if (context != 1u) {
        evidence.contactWitnessMask = 0xFu;
        for (size_t contact = 0u; contact < 4u; ++contact) {
            const int64_t side = (contact & 1u) != 0u ? -1 : 1;
            const int64_t height = contact < 2u ? 250000 : -250000;
            const uint64_t contactError = 1000u + contact * 500u;
            evidence.contactChainRootMicrometres[contact][0] = side * 120000;
            evidence.contactChainRootMicrometres[contact][1] = height + 180000;
            evidence.contactBendMicrometres[contact][0] = side * 200000;
            evidence.contactBendMicrometres[contact][1] = height + 90000;
            evidence.contactTargetMicrometres[contact][0] = side * 270000;
            evidence.contactTargetMicrometres[contact][1] = height;
            evidence.contactTargetMicrometres[contact][2] = 320000;
            for (size_t axis = 0u; axis < 3u; ++axis) {
                evidence.contactEndMicrometres[contact][axis] =
                    evidence.contactTargetMicrometres[contact][axis];
            }
            evidence.contactEndMicrometres[contact][0] +=
                static_cast<int64_t>(contactError);
            evidence.contactWitnessErrorMicrometres[contact] = contactError;
        }
    }
    evidence.fitDiagnosticsValid = true;
    evidence.fitBoundsMinMicrometres[0] = -400000;
    evidence.fitBoundsMinMicrometres[1] = context == 1u ? 0 : -600000;
    evidence.fitBoundsMinMicrometres[2] = -300000;
    evidence.fitBoundsMaxMicrometres[0] = 400000;
    evidence.fitBoundsMaxMicrometres[1] = context == 1u ? 1500000 : 900000;
    evidence.fitBoundsMaxMicrometres[2] = 300000;
    evidence.fitAnchorMicrometres[0] = 10000;
    evidence.fitAnchorMicrometres[1] = context == 1u ? 0 : 20000;
    evidence.fitAnchorMicrometres[2] = -30000;
    evidence.fitForwardMilli[2] = 1000;
    evidence.backend                     = "webgpu-metal";
    evidence.adapter                     = "Test GPU \xC2\xA9";
    evidence.driver                      = "test-driver";
    evidence.vendorId                    = 0x106Bu;
    evidence.deviceId                    = 0x1234u;
    evidence.outputWidth                 = 1280u;
    evidence.outputHeight                = 960u;
    evidence.renderWidth                 = 2560u;
    evidence.renderHeight                = 1920u;
    return evidence;
}

} // namespace

int main() {
    using namespace CharacterTestEvidenceStore;
    std::string error;
    Inventory   inventory;
    Evidence    select   = makeEvidence("org.example.alpha", 1u, 1u);
    Evidence    car      = makeEvidence("org.example.alpha", 2u, 4u);
    Evidence    baseline = car;
    baseline.kind        = Kind::Baseline;
    baseline.capturedUnix--;
    baseline.sourceSha256.assign(64u, 'd');
    baseline.fitSha256.assign(64u, 'e');
    expect(upsert(inventory, car, error) &&
               upsert(inventory, select, error) &&
               upsert(inventory, baseline, error) &&
               inventory.records.size() == 3u,
           "latest results and a pinned baseline coexist by exact cell");

    std::string encoded;
    expect(serialize(inventory, encoded, error) &&
               encoded.rfind("mdkr-character-test-evidence-v3\t3\t", 0u) ==
                   0u,
           "v3 test evidence serializes with a whole-inventory checksum");
    const size_t body = encoded.find('\n') + 1u;
    expect(encoded.find("org.example.alpha\t", body) != std::string::npos,
           "canonical rows retain their package key");

    Inventory parsed;
    expect(parse(encoded, parsed, error) && parsed.records.size() == 3u &&
               find(parsed, "org.example.alpha", 2u, 4u, Kind::Latest)
                       ->adapter == car.adapter &&
               find(parsed, "org.example.alpha", 2u, 4u, Kind::Latest)
                       ->intervalP99Us == car.intervalP99Us &&
               find(parsed, "org.example.alpha", 2u, 4u, Kind::Latest)
                       ->fitBoundsMinMicrometres[1] == -600000 &&
               find(parsed, "org.example.alpha", 2u, 4u, Kind::Latest)
                       ->fitAnchorMicrometres[2] == -30000 &&
               find(parsed, "org.example.alpha", 2u, 4u, Kind::Latest)
                       ->fitForwardMilli[2] == 1000 &&
               find(parsed, "org.example.alpha", 2u, 4u, Kind::Latest)
                       ->contactWitnessMask == 0xFu &&
               find(parsed, "org.example.alpha", 2u, 4u, Kind::Latest)
                       ->contactWitnessErrorMicrometres[3] == 2500u &&
               find(parsed, "org.example.alpha", 2u, 4u, Kind::Baseline)
                       ->sourceSha256 == baseline.sourceSha256,
           "round trip preserves timing, device, source, renderer fit, contact witnesses, and kind");
    const std::string legacyV1 = legacyV1FromV3(encoded);
    Inventory legacyParsed;
    const Evidence *legacyCar = nullptr;
    expect(!legacyV1.empty() && parse(legacyV1, legacyParsed, error) &&
               legacyParsed.records.size() == 3u &&
               (legacyCar = find(
                    legacyParsed, "org.example.alpha", 2u, 4u,
                    Kind::Latest)) != nullptr &&
               !legacyCar->fitDiagnosticsValid &&
               legacyCar->fitBoundsMinMicrometres[1] == 0 &&
               legacyCar->fitForwardMilli[2] == 0,
           "authenticated v1 rows load with an explicit unavailable fit state");
    std::string migrated;
    expect(serialize(legacyParsed, migrated, error) &&
               migrated.rfind(
                   "mdkr-character-test-evidence-v3\t3\t", 0u) == 0u,
           "the next successful write migrates a v1 inventory to v3 in place");
    const std::string legacyV2 = legacyV2FromV3(encoded);
    Inventory legacyV2Parsed;
    expect(!legacyV2.empty() && parse(legacyV2, legacyV2Parsed, error) &&
               find(legacyV2Parsed, "org.example.alpha", 2u, 4u,
                    Kind::Latest)->contactWitnessMask == 0u,
           "authenticated v2 rows migrate with explicitly unavailable contact witnesses");
    expect(qualified(car) && comparable(car, baseline),
           "a source or fit change remains comparable under one exact environment");
    Evidence anotherDevice = baseline;
    anotherDevice.adapter  = "Other GPU";
    expect(!comparable(car, anotherDevice),
           "a different adapter is never presented as an honest baseline");
    Evidence anotherPresentation = baseline;
    anotherPresentation.presentationSha256.assign(64u, 'f');
    expect(!comparable(car, anotherPresentation),
           "different presentation settings refuse baseline comparison");
    Evidence anotherContract = baseline;
    anotherContract.resultVersion++;
    expect(!comparable(car, anotherContract),
           "different result contracts refuse baseline comparison");
    Evidence anotherPackage  = baseline;
    anotherPackage.packageId = "org.example.beta";
    expect(!comparable(car, anotherPackage),
           "results from different packages cannot be compared accidentally");
    Evidence wrongKind = baseline;
    wrongKind.kind     = Kind::Latest;
    expect(!comparable(car, wrongKind),
           "comparison requires an explicitly pinned baseline");

    Evidence shortBaseline        = baseline;
    shortBaseline.intervalSamples = 20u;
    shortBaseline.kind            = Kind::Baseline;
    expect(!upsert(inventory, shortBaseline, error),
           "short diagnostic samples cannot become baselines");
    Evidence unknownDeviceBaseline = baseline;
    unknownDeviceBaseline.adapter.clear();
    expect(!qualified(unknownDeviceBaseline) &&
               !upsert(inventory, unknownDeviceBaseline, error),
           "evidence without exact adapter identity cannot become a baseline");
    Evidence invalid = select;
    invalid.context  = 0u;
    expect(!upsert(inventory, invalid, error),
           "invalid exact contexts are rejected");
    invalid         = select;
    invalid.players = 5u;
    expect(!upsert(inventory, invalid, error),
           "invalid player layouts are rejected");
    invalid                 = select;
    invalid.sourceSha256[0] = 'A';
    expect(!upsert(inventory, invalid, error),
           "fingerprints require canonical lowercase hex");
    invalid         = select;
    invalid.adapter = std::string("hostile\nGPU");
    expect(!upsert(inventory, invalid, error),
           "device text cannot inject control rows");
    invalid = select;
    invalid.adapter = std::string("\xc0\xaf", 2u);
    expect(!upsert(inventory, invalid, error),
           "device text must be canonical printable UTF-8");
    invalid = select;
    invalid.backend.clear();
    expect(!upsert(inventory, invalid, error),
           "a started result requires a named renderer backend");
    invalid               = select;
    invalid.intervalP95Us = invalid.intervalP50Us - 1u;
    expect(!upsert(inventory, invalid, error),
           "non-monotonic percentile evidence is rejected");
    invalid               = select;
    invalid.intervalP50Us = 0u;
    expect(!upsert(inventory, invalid, error),
           "sampled evidence requires a nonzero cadence distribution");
    invalid                 = select;
    invalid.displayedFrames = invalid.intervalSamples - 1u;
    expect(!upsert(inventory, invalid, error),
           "sampled evidence cannot exceed its displayed-frame census");
    invalid                 = select;
    invalid.tickwallSamples = 0u;
    expect(!upsert(inventory, invalid, error),
           "a tick-wall mean requires a tick-wall sample count");
    invalid                  = select;
    invalid.replacementDraws = 0u;
    expect(!upsert(inventory, invalid, error),
           "submitted parts require a replacement draw");
    invalid              = select;
    invalid.outputHeight = 0u;
    expect(!upsert(inventory, invalid, error),
           "partial output dimensions are rejected");
    invalid               = car;
    invalid.contactSolves = 0u;
    expect(!upsert(inventory, invalid, error),
           "contact measurements cannot detach from a solve count");
    invalid                             = select;
    invalid.contactSolves               = 1u;
    invalid.contactErrorMeanMicrometres = 1u;
    invalid.contactErrorMaxMicrometres  = 1u;
    expect(!upsert(inventory, invalid, error),
           "character-select evidence cannot claim vehicle contact solves");
    invalid = car;
    invalid.contactWitnessMask = 0u;
    for (size_t contact = 0u; contact < 4u; ++contact) {
        for (size_t axis = 0u; axis < 3u; ++axis) {
            invalid.contactChainRootMicrometres[contact][axis] = 0;
            invalid.contactBendMicrometres[contact][axis] = 0;
            invalid.contactTargetMicrometres[contact][axis] = 0;
            invalid.contactEndMicrometres[contact][axis] = 0;
        }
        invalid.contactWitnessErrorMicrometres[contact] = 0u;
    }
    expect(!upsert(inventory, invalid, error),
           "current solved vehicle evidence requires all four exact witnesses");
    invalid = car;
    invalid.contactWitnessMask = 0x3u;
    expect(!upsert(inventory, invalid, error),
           "partial hand/foot witness publication is rejected");
    invalid = car;
    invalid.contactWitnessErrorMicrometres[0] += 100u;
    expect(!upsert(inventory, invalid, error),
           "reported contact error must match target-to-endpoint distance");
    invalid = select;
    invalid.contactWitnessMask = 0xFu;
    expect(!upsert(inventory, invalid, error),
           "character-select evidence cannot fabricate vehicle witnesses");
    invalid = select;
    invalid.fitDiagnosticsValid = false;
    expect(!upsert(inventory, invalid, error),
           "unavailable fit evidence cannot retain stale measurements");
    invalid = select;
    invalid.fitBoundsMinMicrometres[1] = 2000000;
    expect(!upsert(inventory, invalid, error),
           "renderer fit bounds must remain ordered on every axis");
    invalid = select;
    invalid.fitForwardMilli[2] = 900;
    expect(!upsert(inventory, invalid, error),
           "renderer forward evidence must remain a normalized direction");
    invalid = select;
    invalid.fitForwardMilli[0] = INT32_MAX;
    invalid.fitForwardMilli[1] = INT32_MAX;
    invalid.fitForwardMilli[2] = INT32_MAX;
    expect(!upsert(inventory, invalid, error),
           "extreme in-memory facing vectors fail without overflowing validation");
    invalid = select;
    invalid.replacementDraws = 0u;
    invalid.replacementPrimitives = 0u;
    invalid.hiddenDonorBatches = 0u;
    expect(!upsert(inventory, invalid, error),
           "renderer fit evidence requires a successful replacement draw");

    const Inventory before      = parsed;
    std::string     tampered    = encoded;
    const size_t    adapterText = tampered.find("546573742047505520c2a9");
    expect(adapterText != std::string::npos,
           "encoded adapter fixture is present");
    tampered[adapterText] = tampered[adapterText] == '5' ? '4' : '5';
    expect(!parse(tampered, parsed, error) &&
               parsed.records.size() == before.records.size() &&
               error == "test evidence checksum, key, or order is invalid",
           "row tampering cannot replace the prior inventory or reuse a stale diagnostic");
    expect(!parse(encoded + "trailing", parsed, error),
           "trailing bytes are rejected");
    const std::string negativeZero = authenticatedV3WithFirstRowField(
        encoded, 45u, "-0");
    expect(!negativeZero.empty() && !parse(negativeZero, parsed, error) &&
               error == "test evidence fit fields are invalid",
           "authenticated signed fields reject negative zero");
    const std::string outOfRange = authenticatedV3WithFirstRowField(
        encoded, 39u, "-1000000001");
    expect(!outOfRange.empty() && !parse(outOfRange, parsed, error) &&
               error == "test evidence fit fields are invalid",
           "authenticated signed fields reject values outside the fit bound");
    const std::string reversedBounds = authenticatedV3WithFirstRowField(
        encoded, 39u, "500000");
    expect(!reversedBounds.empty() && !parse(reversedBounds, parsed, error) &&
               error == "test evidence fit diagnostics are inconsistent",
           "authenticated but reversed renderer bounds fail semantic validation");

    const size_t      headerEnd = encoded.find('\n') + 1u;
    const size_t      rowOneEnd = encoded.find('\n', headerEnd) + 1u;
    const size_t      rowTwoEnd = encoded.find('\n', rowOneEnd) + 1u;
    const std::string header    = encoded.substr(0u, headerEnd);
    const std::string rowOne    = encoded.substr(headerEnd, rowOneEnd - headerEnd);
    const std::string rowTwo    = encoded.substr(rowOneEnd, rowTwoEnd - rowOneEnd);
    const std::string rowThree  = encoded.substr(rowTwoEnd);
    expect(!parse(header + rowTwo + rowOne + rowThree, parsed, error),
           "non-canonical row ordering is rejected");
    std::string omitted = header + rowOne + rowTwo;
    omitted.replace(omitted.find("\t3\t"), 3u, "\t2\t");
    expect(!parse(omitted, parsed, error),
           "removing a complete row breaks inventory authentication");

    Inventory duplicate = inventory;
    duplicate.records.push_back(select);
    expect(!serialize(duplicate, encoded, error),
           "duplicate cell/kind keys cannot be serialized");

    Inventory full;
    for (size_t package = 0u; package < kMaximumPackages; ++package) {
        for (uint32_t context = 1u; context <= 4u; ++context) {
            for (uint32_t players = 1u; players <= 4u; ++players) {
                for (Kind kind : {Kind::Latest, Kind::Baseline}) {
                    Evidence evidence = makeEvidence(
                        "pkg-" + std::to_string(package),
                        context,
                        players,
                        kind);
                    expect(upsert(full, std::move(evidence), error),
                           "every documented evidence slot is admitted");
                }
            }
        }
    }
    Evidence overflow = makeEvidence("pkg-overflow", 1u, 1u);
    expect(full.records.size() == kMaximumRecords &&
               !upsert(full, overflow, error),
           "the first record beyond the complete 64-package matrix is refused");

    Inventory sparsePackages;
    for (size_t package = 0u; package < kMaximumPackages; ++package) {
        expect(upsert(
                   sparsePackages,
                   makeEvidence("sparse-" + std::to_string(package),
                                1u,
                                1u),
                   error),
               "one exact result is admitted for every bounded package");
    }
    expect(sparsePackages.records.size() == kMaximumPackages &&
               !upsert(sparsePackages,
                       makeEvidence("sparse-overflow", 1u, 1u),
                       error),
           "a sparse sixty-fifth package is refused before the record bound");

    Inventory erased = inventory;
    expect(erase(erased, car.packageId, car.context, car.players, Kind::Baseline) &&
               find(erased, car.packageId, car.context, car.players, Kind::Latest) != nullptr &&
               find(erased, car.packageId, car.context, car.players, Kind::Baseline) == nullptr,
           "clearing a baseline preserves the latest result in that cell");
    expect(erasePackage(erased, select.packageId) == 2u &&
               erased.records.empty() &&
               erasePackage(erased, select.packageId) == 0u,
           "package cleanup removes only all evidence owned by that package");

    MemoryStorage        memory;
    MdkrTextStateStorage storage{&memory, readMemory, writeMemory};
    Inventory            loaded;
    expect(load(storage, loaded, error) == LoadResult::Missing &&
               loaded.records.empty(),
           "missing storage is an ordinary empty first run");
    expect(save(storage, inventory, error) && memory.writes == 1u &&
               load(storage, loaded, error) == LoadResult::Loaded &&
               loaded.records.size() == inventory.records.size(),
           "atomic storage round trips the complete evidence inventory");
    const std::string durable = memory.bytes;
    memory.failWrite          = true;
    expect(!save(storage, Inventory{}, error) && memory.bytes == durable,
           "a failed write preserves durable evidence");
    memory.failWrite = false;
    memory.bytes[memory.bytes.size() / 2u] ^= 1;
    expect(load(storage, loaded, error) == LoadResult::Invalid &&
               loaded.records.size() == inventory.records.size(),
           "corruption cannot replace the last loaded evidence inventory");
    memory.failRead = true;
    expect(load(storage, loaded, error) == LoadResult::IoError &&
               loaded.records.size() == inventory.records.size(),
           "read failure cannot replace the last loaded evidence inventory");

    if (failures != 0) return 1;
    std::puts("character test evidence store passed");
    return 0;
}
