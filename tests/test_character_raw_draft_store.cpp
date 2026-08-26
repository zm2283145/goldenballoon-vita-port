#include "character_raw_draft_store.h"

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

CharacterRawDraftStore::Draft makeDraft(const char *id,
                                        const char *model,
                                        uint64_t    updated) {
    CharacterRawDraftStore::Draft draft;
    draft.id            = id;
    draft.updatedUnix   = updated;
    draft.modelPath     = model;
    draft.licensePath   = "/source/LICENSE";
    draft.packageId     = std::string("org.example.") + id;
    draft.displayName   = std::string("Authoring ") + id;
    draft.spdx          = "CC-BY-4.0";
    draft.attribution   = "Community artist \xC2\xA9";
    draft.sourceUrl     = "https://example.invalid/source";
    draft.donor         = 9u;
    draft.vehicleMask   = 7u;
    draft.sourceForward = 1u;
    draft.targetHeight  = 1.375f;
    draft.mappingModelSha256.assign(64u, id[4] == 'a' ? 'a' : 'b');
    draft.fallback = "idle";
    draft.seat     = "pelvis";
    draft.head     = "head";
    return draft;
}

} // namespace

int main() {
    using namespace CharacterRawDraftStore;
    std::string error;
    Inventory   inventory;
    const Draft first  = makeDraft("raw-alpha", "/source/alpha.glb", 20u);
    const Draft second = makeDraft("raw-beta", "/source/beta.glb", 30u);
    expect(upsert(inventory, first, error) &&
               upsert(inventory, second, error),
           "multiple raw authoring drafts can coexist");
    inventory.selectedId = first.id;

    Inventory   empty;
    std::string emptyEncoded;
    Inventory   emptyParsed;
    expect(serialize(empty, emptyEncoded, error) &&
               emptyEncoded.rfind(
                   "mdkr-character-raw-drafts-v1\t0\t-\t",
                   0u) == 0u &&
               emptyEncoded.size() ==
                   std::string("mdkr-character-raw-drafts-v1\t0\t-\t").size() +
                       65u &&
               parse(emptyEncoded, emptyParsed, error) &&
               emptyParsed.drafts.empty() && emptyParsed.selectedId.empty(),
           "empty first-run inventories round trip canonically");

    Draft uninspected;
    uninspected.id        = "raw-uninspected";
    uninspected.modelPath = "/source/new.glb";
    expect(upsert(empty, uninspected, error) &&
               empty.selectedId == uninspected.id,
           "an incomplete first draft is persisted and made reachable");

    std::string encoded;
    expect(serialize(inventory, encoded, error),
           "raw inventory serializes");
    const size_t encodedBody = encoded.find('\n') + 1u;
    expect(encoded.rfind(
               "mdkr-character-raw-drafts-v1\t2\traw-alpha\t",
               0u) == 0u &&
               encoded.find("raw-beta\t", encodedBody) <
                   encoded.find("raw-alpha\t", encodedBody),
           "serialization is canonical newest-first and preserves selection");

    Inventory parsed;
    expect(parse(encoded, parsed, error) && parsed.drafts.size() == 2u &&
               parsed.selectedId == first.id &&
               find(parsed, first.id)->modelPath == first.modelPath &&
               find(parsed, first.id)->targetHeight == first.targetHeight &&
               find(parsed, second.id)->mappingModelSha256 ==
                   second.mappingModelSha256,
           "round trip preserves exact source-bound authoring state");

    const Inventory before   = parsed;
    std::string     tampered = encoded;
    const size_t    model    = tampered.find("2f736f757263652f626574612e676c62");
    expect(model != std::string::npos, "encoded model fixture is present");
    tampered[model] = tampered[model] == '2' ? '3' : '2';
    expect(!parse(tampered, parsed, error) &&
               parsed.selectedId == before.selectedId &&
               parsed.drafts.size() == before.drafts.size(),
           "tampering fails without replacing the prior inventory");
    expect(!parse(encoded + "trailing", parsed, error),
           "trailing raw inventory bytes are rejected");

    std::string badSelection = encoded;
    badSelection.replace(badSelection.find("raw-alpha\t"), 9u, "raw-ghost\t");
    expect(!parse(badSelection, parsed, error),
           "selection must reference an exact stored draft");
    std::string validSelectionTamper = encoded;
    validSelectionTamper.replace(
        validSelectionTamper.find("raw-alpha\t"),
        9u,
        "raw-beta\t");
    expect(!parse(validSelectionTamper, parsed, error),
           "changing selection to another valid id breaks authentication");
    const size_t      firstRowEnd  = encoded.find('\n', encoded.find('\n') + 1u);
    const size_t      secondRowEnd = encoded.find('\n', firstRowEnd + 1u);
    const std::string header       = encoded.substr(0u, encoded.find('\n') + 1u);
    const std::string rowOne       = encoded.substr(
        header.size(),
        firstRowEnd + 1u - header.size());
    const std::string rowTwo = encoded.substr(
        firstRowEnd + 1u,
        secondRowEnd - firstRowEnd);
    expect(!parse(header + rowTwo + rowOne, parsed, error),
           "non-canonical raw draft row ordering is rejected");
    std::string omittedRow = header + rowTwo;
    omittedRow.replace(omittedRow.find("\t2\t"), 3u, "\t1\t");
    expect(!parse(omittedRow, parsed, error),
           "removing a complete peer row breaks inventory authentication");

    Draft hostile     = first;
    hostile.modelPath = "bad\npath.glb";
    expect(!upsert(inventory, hostile, error),
           "control characters are rejected from paths");
    hostile             = first;
    hostile.displayName = std::string("bidi ") + "\xE2\x81\xA6" + "name";
    expect(!upsert(inventory, hostile, error),
           "bidirectional controls are rejected from visible metadata");
    hostile = first;
    hostile.mappingModelSha256.clear();
    expect(!upsert(inventory, hostile, error),
           "mapping names cannot detach from their model fingerprint");
    hostile              = first;
    hostile.targetHeight = 100.0f;
    expect(!upsert(inventory, hostile, error),
           "out-of-range calibration is rejected");
    hostile           = first;
    hostile.modelPath = "   ";
    expect(!upsert(inventory, hostile, error),
           "a whitespace-only model path is rejected");
    hostile             = first;
    hostile.displayName = std::string("invalid ") + "\xC0\xAF";
    expect(!upsert(inventory, hostile, error),
           "non-canonical UTF-8 is rejected");
    hostile = first;
    hostile.modelPath.assign(kMaximumPathBytes + 1u, 'x');
    expect(!upsert(inventory, hostile, error),
           "oversized source paths are rejected");
    hostile                       = first;
    hostile.mappingModelSha256[0] = 'A';
    expect(!upsert(inventory, hostile, error),
           "mapping fingerprints require canonical lowercase hex");

    Inventory full;
    for (size_t index = 0u; index < kMaximumDrafts; ++index) {
        Draft draft;
        draft.id        = "raw-capacity-" + std::to_string(index);
        draft.modelPath = "/source/" + std::to_string(index) + ".glb";
        expect(upsert(full, std::move(draft), error),
               "raw draft capacity accepts every documented slot");
    }
    Draft overflow;
    overflow.id        = "raw-capacity-overflow";
    overflow.modelPath = "/source/overflow.glb";
    expect(!upsert(full, overflow, error),
           "raw draft capacity rejects only the first excess entry");

    Inventory duplicate = inventory;
    duplicate.drafts.push_back(first);
    expect(!serialize(duplicate, encoded, error),
           "duplicate raw draft ids cannot be serialized");

    MemoryStorage        memory;
    MdkrTextStateStorage storage{&memory, readMemory, writeMemory};
    Inventory            loaded;
    expect(load(storage, loaded, error) == LoadResult::Missing &&
               loaded.drafts.empty(),
           "missing storage is a valid empty first-run inventory");
    expect(save(storage, inventory, error) && memory.writes == 1u,
           "save performs one complete storage replacement");
    expect(load(storage, loaded, error) == LoadResult::Loaded &&
               loaded.drafts.size() == 2u &&
               loaded.selectedId == first.id,
           "stored raw drafts resume through the storage boundary");
    memory.failWrite          = true;
    const std::string durable = memory.bytes;
    expect(!save(storage, Inventory{}, error) && memory.bytes == durable,
           "failed save preserves durable state");
    memory.failWrite = false;
    memory.bytes[memory.bytes.size() / 2u] ^= 1;
    expect(load(storage, loaded, error) == LoadResult::Invalid &&
               loaded.drafts.size() == 2u,
           "corruption cannot replace the last loaded inventory");
    memory.failRead = true;
    expect(load(storage, loaded, error) == LoadResult::IoError &&
               loaded.drafts.size() == 2u,
           "I/O failure cannot replace the last loaded inventory");
    memory.failRead = false;

    Draft updated       = first;
    updated.displayName = "Alpha final";
    expect(upsert(inventory, updated, error) &&
               inventory.drafts.size() == 2u &&
               find(inventory, first.id)->displayName == "Alpha final",
           "upsert updates one exact draft without duplication");
    expect(erase(inventory, first.id) &&
               inventory.selectedId == second.id &&
               find(inventory, second.id) != nullptr &&
               !erase(inventory, first.id),
           "exact deletion preserves and selects the newest peer draft");

    Inventory invalidSelection  = inventory;
    invalidSelection.selectedId = "raw-missing";
    expect(!serialize(invalidSelection, encoded, error),
           "serialization fails closed on an invalid selection");

    if (failures != 0) return 1;
    std::puts("character raw draft store passed");
    return 0;
}
