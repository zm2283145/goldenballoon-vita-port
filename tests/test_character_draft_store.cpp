#include "character_draft_store.h"

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

struct MemoryStorage {
    bool present = false;
    bool failRead = false;
    bool failWrite = false;
    unsigned writes = 0u;
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

CharacterDraftStore::Draft makeDraft(const char *id, const char *package,
                                     char digestByte, uint64_t updated,
                                     const char *name,
                                     const std::string &payload) {
    CharacterDraftStore::Draft draft;
    draft.id = id;
    draft.packageId = package;
    draft.baseSourceDigest.assign(64u, digestByte);
    draft.updatedUnix = updated;
    draft.name = name;
    draft.payload = payload;
    return draft;
}

}  // namespace

int main() {
    using namespace CharacterDraftStore;
    Inventory inventory;
    std::string error;
    const Draft first = makeDraft(
        "dixie-polish", "org.example.dixie", 'a', 1787760000u,
        "Dixie polish \xC2\xA9", std::string("draft-v1\0payload", 16u));
    const Draft second = makeDraft(
        "tiny-performance", "org.example.tiny", 'b', 1787760100u,
        "Tiny performance", "draft-v1\nquality=balanced\n");
    expect(upsert(inventory, first, error) &&
               upsert(inventory, second, error),
           "bounded named drafts can be inserted");

    std::string encoded;
    expect(serialize(inventory, encoded, error),
           "draft inventory serializes");
    expect(encoded.rfind("mdkr-character-drafts-v1\t2\n", 0u) == 0u &&
               encoded.find("tiny-performance\t") <
                   encoded.find("dixie-polish\t"),
           "serialization is deterministic newest-first");
    Inventory parsed;
    expect(parse(encoded, parsed, error) && parsed.drafts.size() == 2u &&
               parsed.drafts[0].id == "tiny-performance" &&
               parsed.drafts[1].name == "Dixie polish \xC2\xA9" &&
               parsed.drafts[1].payload == first.payload,
           "round trip preserves UTF-8 names and opaque binary payloads");

    const Inventory before = parsed;
    std::string tampered = encoded;
    const size_t payload = tampered.find("64726166742d7631");
    expect(payload != std::string::npos, "payload fixture is present");
    tampered[payload] = tampered[payload] == '6' ? '7' : '6';
    expect(!parse(tampered, parsed, error) &&
               parsed.drafts[0].id == before.drafts[0].id,
           "tampering fails without changing the prior inventory");
    expect(!parse(encoded + "trailing", parsed, error),
           "trailing draft bytes are rejected");

    Draft hostile = first;
    hostile.id = "Hostile";
    expect(!upsert(inventory, hostile, error),
           "draft ids use the safe lowercase slug profile");
    hostile = first;
    hostile.name = "spoof\nname";
    expect(!upsert(inventory, hostile, error),
           "control characters are rejected from visible draft names");
    hostile = first;
    hostile.name = std::string("bidi ") + "\xE2\x81\xA6" + "name";
    expect(!upsert(inventory, hostile, error),
           "bidirectional isolate controls are rejected from draft names");
    hostile = first;
    hostile.payload.assign(kMaximumPayloadBytes + 1u, 'x');
    expect(!upsert(inventory, hostile, error),
           "oversized draft payloads are rejected before persistence");

    MemoryStorage memory;
    MdkrTextStateStorage storage{&memory, readMemory, writeMemory};
    Inventory loaded;
    expect(load(storage, loaded, error) == LoadResult::Missing &&
               loaded.drafts.empty(),
           "a missing draft file is an empty first-run inventory");
    expect(save(storage, inventory, error) && memory.writes == 1u,
           "save delegates one complete atomic replacement");
    expect(load(storage, loaded, error) == LoadResult::Loaded &&
               loaded.drafts.size() == 2u,
           "saved drafts resume through the storage boundary");
    memory.failWrite = true;
    const std::string durable = memory.bytes;
    expect(!save(storage, Inventory{}, error) && memory.bytes == durable,
           "failed replacement preserves the durable inventory");
    memory.failWrite = false;
    memory.bytes[memory.bytes.size() / 2u] ^= 1;
    expect(load(storage, loaded, error) == LoadResult::Invalid &&
               loaded.drafts.size() == 2u,
           "corrupt durable state does not replace the last loaded inventory");

    Draft updated = first;
    updated.name = "Dixie final polish";
    updated.updatedUnix++;
    expect(upsert(inventory, updated, error) &&
               inventory.drafts.size() == 2u &&
               find(inventory, first.id) != nullptr &&
               find(inventory, first.id)->name == updated.name,
           "saving an existing draft updates it without duplication");
    expect(erase(inventory, first.id) && !erase(inventory, first.id) &&
               find(inventory, first.id) == nullptr,
           "draft deletion is exact and idempotently reports absence");

    if (failures != 0) return 1;
    std::puts("character draft store passed");
    return 0;
}
