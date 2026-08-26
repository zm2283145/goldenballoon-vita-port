#include "character_test_evidence_store.h"

#include <cstdio>
#include <string>

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
    evidence.resultVersion               = 3u;
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
               encoded.rfind("mdkr-character-test-evidence-v1\t3\t", 0u) ==
                   0u,
           "test evidence serializes with a whole-inventory checksum");
    const size_t body = encoded.find('\n') + 1u;
    expect(encoded.find("org.example.alpha\t", body) != std::string::npos,
           "canonical rows retain their package key");

    Inventory parsed;
    expect(parse(encoded, parsed, error) && parsed.records.size() == 3u &&
               find(parsed, "org.example.alpha", 2u, 4u, Kind::Latest)
                       ->adapter == car.adapter &&
               find(parsed, "org.example.alpha", 2u, 4u, Kind::Latest)
                       ->intervalP99Us == car.intervalP99Us &&
               find(parsed, "org.example.alpha", 2u, 4u, Kind::Baseline)
                       ->sourceSha256 == baseline.sourceSha256,
           "round trip preserves exact timing, device, source, fit, and kind");
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
