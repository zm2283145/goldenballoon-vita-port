#include "modern_character_draw_store.h"

#include <stdlib.h>
#include <string.h>

typedef struct MdkrModernDrawStoreEntry {
    uint32_t token;
    struct GfxModernSkinnedDraw draw;
    float *bone_storage;
    uint32_t bone_capacity;
} MdkrModernDrawStoreEntry;

static MdkrModernDrawStoreEntry
    s_entries[MDKR_MODERN_DRAW_STORE_CAPACITY];
static uint32_t s_serial = 1u;

static size_t palette_float_count(uint32_t bone_capacity) {
    return (size_t)bone_capacity * 16u;
}

uint32_t mdkr_modern_draw_store_register(
    const struct GfxModernSkinnedDraw *draw) {
    MdkrModernDrawStoreEntry *entry;
    float *storage;
    size_t palette_floats;
    uint32_t token;

    if (draw == NULL || draw->asset == NULL ||
        draw->bone_count > MDKR_MODERN_DRAW_STORE_MAX_BONES ||
        (draw->bone_count != 0u &&
         (draw->bone_matrices == NULL ||
          draw->previous_bone_matrices == NULL))) {
        return 0u;
    }

    token = s_serial;
    if (token == 0u) {
        token = 1u;
    }
    entry = &s_entries[token % MDKR_MODERN_DRAW_STORE_CAPACITY];
    if (draw->bone_count > entry->bone_capacity) {
        const size_t storage_floats =
            palette_float_count(draw->bone_count) * 2u;
        storage = (float *)realloc(
            entry->bone_storage, storage_floats * sizeof(float));
        if (storage == NULL) {
            return 0u;
        }
        entry->bone_storage = storage;
        entry->bone_capacity = draw->bone_count;
    }

    entry->token = token;
    entry->draw = *draw;
    if (draw->bone_count != 0u) {
        palette_floats = palette_float_count(draw->bone_count);
        memcpy(entry->bone_storage, draw->bone_matrices,
               palette_floats * sizeof(float));
        memcpy(entry->bone_storage +
                   palette_float_count(entry->bone_capacity),
               draw->previous_bone_matrices,
               palette_floats * sizeof(float));
        entry->draw.bone_matrices = entry->bone_storage;
        entry->draw.previous_bone_matrices =
            entry->bone_storage + palette_float_count(entry->bone_capacity);
    } else {
        entry->draw.bone_matrices = NULL;
        entry->draw.previous_bone_matrices = NULL;
    }

    s_serial = token + 1u;
    if (s_serial == 0u) {
        s_serial = 1u;
    }
    return token;
}

const struct GfxModernSkinnedDraw *mdkr_modern_draw_store_resolve(
    uint32_t token) {
    const MdkrModernDrawStoreEntry *entry;
    if (token == 0u) {
        return NULL;
    }
    entry = &s_entries[token % MDKR_MODERN_DRAW_STORE_CAPACITY];
    if (entry->token != token) {
        return NULL;
    }
    return &entry->draw;
}

void mdkr_modern_draw_store_release_asset(uint64_t asset_id) {
    uint32_t index;
    for (index = 0u; index < MDKR_MODERN_DRAW_STORE_CAPACITY; index++) {
        MdkrModernDrawStoreEntry *entry = &s_entries[index];
        if (entry->token != 0u && entry->draw.asset != NULL &&
            entry->draw.asset->asset_id == asset_id) {
            entry->token = 0u;
            memset(&entry->draw, 0, sizeof(entry->draw));
        }
    }
}

void mdkr_modern_draw_store_shutdown(void) {
    uint32_t index;
    for (index = 0u; index < MDKR_MODERN_DRAW_STORE_CAPACITY; index++) {
        free(s_entries[index].bone_storage);
        memset(&s_entries[index], 0, sizeof(s_entries[index]));
    }
}

size_t mdkr_modern_draw_store_allocated_bytes(void) {
    size_t total = 0u;
    uint32_t index;
    for (index = 0u; index < MDKR_MODERN_DRAW_STORE_CAPACITY; index++) {
        total += palette_float_count(s_entries[index].bone_capacity) *
                 2u * sizeof(float);
    }
    return total;
}
