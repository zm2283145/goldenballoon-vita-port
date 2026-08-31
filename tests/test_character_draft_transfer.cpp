#include "character_draft_transfer.h"
#include "character_draft_snapshot.h"
#include "sha256.h"

#include <cstdio>
#include <string>

namespace {

int failures;

void expect(bool condition, const char *message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

CharacterDraftStore::Draft makeDraft(const char *id, const char *name,
                                     const char *package,
                                     const std::string &digest,
                                     const char *path, uint8_t colour) {
    CharacterDraftSnapshot::Snapshot snapshot;
    snapshot.displayName = "Transfer Proof";
    snapshot.shortName = "Proof";
    snapshot.narrationName = "Transfer Proof";
    snapshot.sortLabel = "Proof, Transfer";
    snapshot.portraitSourcePath = path;
    snapshot.portrait[0] = colour;
    snapshot.portraitStyleSource[0] = colour;
    std::string error;
    CharacterDraftStore::Draft draft;
    draft.id = id;
    draft.packageId = package;
    draft.baseSourceDigest = digest;
    draft.updatedUnix = 1787760000u + colour;
    draft.name = name;
    if (!CharacterDraftSnapshot::encode(snapshot, draft.payload, error)) {
        std::fprintf(stderr, "fixture snapshot failed: %s\n", error.c_str());
        ++failures;
    }
    return draft;
}

std::string sha256Hex(const std::string &bytes) {
    char digest[MDKR_SHA256_HEX_SIZE];
    mdkr_sha256_hex(bytes.data(), bytes.size(), digest);
    return digest;
}

}  // namespace

int main() {
    using namespace CharacterDraftTransfer;
    const std::string package = "org.example.transfer";
    const std::string digest(64u, 'a');
    const std::string retainedDigest(64u, 'b');
    const std::string privatePath =
        "/Users/example/Secret Project/portrait-source.png";
    CharacterDraftStore::Inventory source;
    source.drafts.push_back(makeDraft(
        "vehicle-polish", "Vehicle polish", package.c_str(), digest,
        privatePath.c_str(), 17u));
    source.drafts.push_back(makeDraft(
        "retained-revision", "Old revision", package.c_str(), retainedDigest,
        "/private/old.png", 18u));

    Bundle created;
    std::string error;
    expect(create(source, package, digest, 1787761000u, created, error) &&
               created.inventory.drafts.size() == 1u,
           "creation selects only the exact active source revision");
    CharacterDraftSnapshot::Snapshot normalized;
    expect(CharacterDraftSnapshot::decode(
               created.inventory.drafts[0].payload, normalized, error) &&
               normalized.portraitSourcePath.empty() &&
               normalized.portrait[0] == 17u,
           "creation removes local paths while retaining exact artwork");

    std::string encoded;
    expect(encode(created, encoded, error) &&
               encoded.find(privatePath) == std::string::npos &&
               encoded.size() <= kMaximumBundleBytes,
           "bounded bundle encoding never exposes the source path");
    Bundle parsed;
    expect(parse(encoded, parsed, error) &&
               parsed.packageId == package &&
               parsed.baseSourceDigest == digest &&
               parsed.createdUnix == 1787761000u &&
               parsed.inventory.drafts.size() == 1u,
           "bundle round trip preserves exact-base metadata and draft count");

    const Bundle before = parsed;
    std::string tampered = encoded;
    tampered[tampered.size() - 4u] ^= 1;
    expect(!parse(tampered, parsed, error) &&
               parsed.packageId == before.packageId,
           "payload tampering fails without replacing the previous review");
    expect(!parse(encoded + "x", parsed, error),
           "trailing bundle bytes are rejected");
    CharacterDraftStore::Inventory pathLeaking;
    pathLeaking.drafts.push_back(source.drafts[0]);
    std::string pathPayload;
    expect(CharacterDraftStore::serialize(
               pathLeaking, pathPayload, error),
           "path-leaking adversarial payload serializes at the store layer");
    const std::string pathBundle =
        "mdkr-character-draft-bundle-v1\t" + package + "\t" + digest +
        "\t1787761000\t1\t" + std::to_string(pathPayload.size()) + "\t" +
        sha256Hex(pathPayload) + "\n" + pathPayload;
    expect(!parse(pathBundle, parsed, error),
           "a recomputed outer digest cannot authorize a local source path");
    parsed = before;

    CharacterDraftStore::Inventory local;
    local.drafts.push_back(makeDraft(
        "vehicle-polish", "Conflicting local draft", package.c_str(), digest,
        "/local/conflict.png", 99u));
    CharacterDraftStore::Inventory merged;
    MergeSummary summary;
    expect(merge(local, parsed, merged, summary, error) &&
               summary.input == 1u && summary.additions == 1u &&
               summary.duplicates == 0u && summary.renamed == 1u &&
               merged.drafts.size() == 2u &&
               CharacterDraftStore::find(merged, "vehicle-polish") != nullptr &&
               CharacterDraftStore::find(merged, "vehicle-polish")->name ==
                   "Conflicting local draft" &&
               merged.drafts[1].id.rfind("import-", 0u) == 0u,
           "different id collisions import under a deterministic fresh id");
    CharacterDraftStore::Inventory repeated;
    MergeSummary repeatedSummary;
    expect(merge(merged, parsed, repeated, repeatedSummary, error) &&
               repeatedSummary.additions == 0u &&
               repeatedSummary.duplicates == 1u &&
               repeatedSummary.renamed == 0u &&
               repeated.drafts.size() == merged.drafts.size(),
           "reimport is idempotent after collision renaming");

    CharacterDraftStore::Inventory full;
    for (size_t index = 0u;
         index < CharacterDraftStore::kMaximumDrafts; ++index) {
        full.drafts.push_back(makeDraft(
            ("local-" + std::to_string(index)).c_str(),
            ("Local " + std::to_string(index)).c_str(), package.c_str(),
            digest, "", static_cast<uint8_t>(index + 1u)));
    }
    CharacterDraftStore::Inventory unchanged;
    MergeSummary overflowSummary;
    expect(!merge(full, parsed, unchanged, overflowSummary, error) &&
               unchanged.drafts.empty(),
           "capacity failure does not publish a partial merged inventory");

    Bundle mixed = created;
    mixed.inventory.drafts[0].packageId = "org.example.other";
    expect(!encode(mixed, encoded, error),
           "one bundle cannot mix character identities");

    if (failures != 0) return 1;
    std::puts("character draft transfer passed");
    return 0;
}
