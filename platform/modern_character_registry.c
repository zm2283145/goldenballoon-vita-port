#include "modern_character_registry.h"

#include "fs_utf8.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>

static int ascii_lower(unsigned char value) {
    return value >= 'A' && value <= 'Z' ? value + ('a' - 'A') : value;
}

static int ascii_casecmp(const char *left, const char *right) {
    while (*left != '\0' && *right != '\0') {
        int a = ascii_lower((unsigned char)*left++);
        int b = ascii_lower((unsigned char)*right++);
        if (a != b) return a < b ? -1 : 1;
    }
    if (*left == *right) return 0;
    return *left == '\0' ? -1 : 1;
}

static int copy_string(char *output, size_t output_size, const char *value) {
    size_t length;
    if (output == NULL || output_size == 0u || value == NULL) return 0;
    length = strlen(value);
    if (length >= output_size) return 0;
    memcpy(output, value, length + 1u);
    return 1;
}

static int path_join(char *output, size_t output_size,
                     const char *directory, const char *name) {
    size_t length;
    const char *separator;
    int written;
    if (directory == NULL || directory[0] == '\0' || name == NULL || name[0] == '\0') return 0;
    length = strlen(directory);
    separator = directory[length - 1u] == '/' || directory[length - 1u] == '\\' ? "" : "/";
    written = snprintf(output, output_size, "%s%s%s", directory, separator, name);
    return written >= 0 && (size_t)written < output_size;
}

static int has_cache_suffix(const char *name) {
    size_t length = strlen(name);
    return length > 5u && name[length - 5u] == '.' &&
           ascii_lower((unsigned char)name[length - 4u]) == 'm' &&
           ascii_lower((unsigned char)name[length - 3u]) == 'd' &&
           ascii_lower((unsigned char)name[length - 2u]) == 'k' &&
           ascii_lower((unsigned char)name[length - 1u]) == 'c';
}

static uint32_t semantic_bit(const char *name) {
    static const struct { const char *name; uint32_t bit; } table[] = {
        {"fallback", MDKR_CHARACTER_SEMANTIC_FALLBACK},
        {"race.steer", MDKR_CHARACTER_SEMANTIC_RACE_STEER},
        {"race.reverse", MDKR_CHARACTER_SEMANTIC_RACE_REVERSE},
        {"race.boost", MDKR_CHARACTER_SEMANTIC_RACE_BOOST},
        {"race.damage", MDKR_CHARACTER_SEMANTIC_RACE_DAMAGE},
        {"race.item", MDKR_CHARACTER_SEMANTIC_RACE_ITEM},
        {"race.spin", MDKR_CHARACTER_SEMANTIC_RACE_SPIN},
        {"race.airborne", MDKR_CHARACTER_SEMANTIC_RACE_AIRBORNE},
        {"race.land", MDKR_CHARACTER_SEMANTIC_RACE_LAND},
        {"race.finish_win", MDKR_CHARACTER_SEMANTIC_RACE_FINISH_WIN},
        {"race.finish_lose", MDKR_CHARACTER_SEMANTIC_RACE_FINISH_LOSE},
        {"select.idle", MDKR_CHARACTER_SEMANTIC_SELECT_IDLE},
        {"select.hover", MDKR_CHARACTER_SEMANTIC_SELECT_HOVER},
        {"select.confirm", MDKR_CHARACTER_SEMANTIC_SELECT_CONFIRM},
    };
    size_t index;
    if (name == NULL) return 0u;
    for (index = 0u; index < sizeof(table) / sizeof(table[0]); index++) {
        if (strcmp(name, table[index].name) == 0) return table[index].bit;
    }
    return 0u;
}

static uint32_t socket_bit(const char *name) {
    if (name == NULL) return 0u;
    if (strcmp(name, "seat") == 0) return MDKR_CHARACTER_SOCKET_SEAT;
    if (strcmp(name, "head") == 0) return MDKR_CHARACTER_SOCKET_HEAD;
    if (strcmp(name, "hand") == 0) return MDKR_CHARACTER_SOCKET_HAND;
    return 0u;
}

static void add_skip(MdkrModernCharacterRegistry *registry,
                     const char *name, const char *reason) {
    int slot;
    if (registry->skipped >= MDKR_MODERN_CHARACTER_MAX) {
        slot = MDKR_MODERN_CHARACTER_MAX - 1;
        (void)copy_string(registry->skip_name[slot], sizeof(registry->skip_name[slot]),
                          "More characters");
        (void)copy_string(registry->skip_reason[slot], sizeof(registry->skip_reason[slot]),
                          "further character failures were omitted");
        return;
    }
    slot = registry->skipped++;
    (void)copy_string(registry->skip_name[slot], sizeof(registry->skip_name[slot]), name);
    (void)copy_string(registry->skip_reason[slot], sizeof(registry->skip_reason[slot]), reason);
}

static int entry_compare(const MdkrModernCharacterEntry *left,
                         const MdkrModernCharacterEntry *right) {
    int identity = strcmp(left->id, right->id);
    return identity != 0 ? identity : ascii_casecmp(left->path, right->path);
}

static void sort_entries(MdkrModernCharacterRegistry *registry) {
    int index;
    for (index = 1; index < registry->count; index++) {
        MdkrModernCharacterEntry held = registry->entries[index];
        int scan = index - 1;
        while (scan >= 0 && entry_compare(&registry->entries[scan], &held) > 0) {
            registry->entries[scan + 1] = registry->entries[scan];
            scan--;
        }
        registry->entries[scan + 1] = held;
    }
}

int mdkr_modern_character_registry_init(MdkrModernCharacterRegistry *registry,
                                        const char *directory) {
    DIR *handle;
    struct dirent *item;
    if (registry == NULL) return -1;
    memset(registry, 0, sizeof(*registry));
    if (directory == NULL || directory[0] == '\0') return 0;
    handle = opendir(directory);
    if (handle == NULL) return 0;
    while ((item = readdir(handle)) != NULL) {
        MdkrModernCharacterAsset asset;
        MdkrModernCharacterDefinition definition;
        MdkrModernCharacterEntry entry;
        const char *id;
        const char *display_name;
        char path[MDKR_MODERN_CHARACTER_PATH_MAX];
        char error[MDKR_MODERN_CHARACTER_SKIP_REASON_MAX];
        int regular = 0;
        if (item->d_name[0] == '.' || !has_cache_suffix(item->d_name)) continue;
        if (!path_join(path, sizeof(path), directory, item->d_name) ||
            mdkr_path_query_utf8(path, NULL, &regular, NULL) != 0 || !regular) {
            add_skip(registry, item->d_name, "cache path is not a regular readable file");
            continue;
        }
        if (!mdkr_modern_character_asset_load_file(path, &asset, error, sizeof(error))) {
            add_skip(registry, item->d_name, error);
            continue;
        }
        memset(&entry, 0, sizeof(entry));
        (void)mdkr_modern_character_asset_definition(&asset, &definition);
        id = mdkr_modern_character_asset_string(&asset, definition.id);
        display_name = mdkr_modern_character_asset_string(&asset, definition.display_name);
        if (!copy_string(entry.id, sizeof(entry.id), id) ||
            !copy_string(entry.display_name, sizeof(entry.display_name), display_name) ||
            !copy_string(entry.path, sizeof(entry.path), path)) {
            mdkr_modern_character_asset_unload(&asset);
            add_skip(registry, item->d_name, "compiled identity or path exceeds the registry bound");
            continue;
        }
        memcpy(entry.source_sha256, asset.source_sha256, sizeof(entry.source_sha256));
        entry.donor = definition.donor;
        entry.vehicle_mask = definition.vehicle_mask;
        mdkr_modern_character_asset_stats(&asset, &entry.stats);
        {
            uint32_t index;
            uint8_t animation_moves[64] = {0};
            for (index = 0u; index < entry.stats.sockets; index++) {
                MdkrModernSocket socket;
                if (mdkr_modern_character_asset_socket(&asset, index, &socket)) {
                    entry.socket_mask |= socket_bit(
                        mdkr_modern_character_asset_string(&asset,
                                                           socket.semantic));
                }
            }
            for (index = 0u; index < entry.stats.animations; index++) {
                MdkrModernAnimation animation;
                uint32_t channel_offset;
                (void)mdkr_modern_character_asset_animation(
                    &asset, index, &animation);
                for (channel_offset = 0u;
                     channel_offset < animation.channel_count;
                     channel_offset++) {
                    MdkrModernChannel channel;
                    MdkrModernKey first;
                    uint32_t key_index;
                    int moves = 0;
                    if (!mdkr_modern_character_asset_channel(
                            &asset, animation.first_channel + channel_offset,
                            &channel) ||
                        !mdkr_modern_character_asset_key(
                            &asset, channel.first_key, &first)) continue;
                    for (key_index = 1u;
                         key_index < channel.key_count && !moves;
                         key_index++) {
                        MdkrModernKey key;
                        uint32_t component;
                        (void)mdkr_modern_character_asset_key(
                            &asset, channel.first_key + key_index, &key);
                        for (component = 0u;
                             component < channel.components; component++) {
                            if (key.value[component] != first.value[component] ||
                                key.incoming[component] != 0.0f ||
                                key.outgoing[component] != 0.0f ||
                                first.incoming[component] != 0.0f ||
                                first.outgoing[component] != 0.0f) {
                                moves = 1;
                                break;
                            }
                        }
                    }
                    if (moves) {
                        animation_moves[index] = 1u;
                        entry.motion_channels++;
                    }
                }
            }
            for (index = 0u; index < entry.stats.semantics; index++) {
                MdkrModernSemantic semantic;
                const char *name;
                const char *clip;
                uint32_t bit;
                uint32_t animation_index;
                if (!mdkr_modern_character_asset_semantic(
                        &asset, index, &semantic)) continue;
                name = mdkr_modern_character_asset_string(
                    &asset, semantic.semantic);
                clip = mdkr_modern_character_asset_string(
                    &asset, semantic.clip);
                bit = semantic_bit(name);
                entry.semantic_mask |= bit;
                for (animation_index = 0u;
                     animation_index < entry.stats.animations;
                     animation_index++) {
                    MdkrModernAnimation animation;
                    const char *animation_name;
                    (void)mdkr_modern_character_asset_animation(
                        &asset, animation_index, &animation);
                    animation_name = mdkr_modern_character_asset_string(
                        &asset, animation.name);
                    if (clip != NULL && animation_name != NULL &&
                        strcmp(clip, animation_name) == 0) {
                        if (animation_moves[animation_index]) {
                            entry.moving_semantic_mask |= bit;
                        }
                        break;
                    }
                }
            }
        }
        mdkr_modern_character_asset_unload(&asset);
        if (registry->count >= MDKR_MODERN_CHARACTER_MAX) {
            add_skip(registry, item->d_name, "the 64-character local registry is full");
            continue;
        }
        registry->entries[registry->count++] = entry;
    }
    (void)closedir(handle);
    sort_entries(registry);
    {
        int read_index;
        int write_index = 0;
        for (read_index = 0; read_index < registry->count; read_index++) {
            if (write_index > 0 && strcmp(registry->entries[write_index - 1].id,
                                          registry->entries[read_index].id) == 0) {
                const char *leaf = strrchr(registry->entries[read_index].path, '/');
                add_skip(registry, leaf != NULL ? leaf + 1 : registry->entries[read_index].path,
                         "another compiled character uses the same package id");
                continue;
            }
            if (write_index != read_index) registry->entries[write_index] = registry->entries[read_index];
            write_index++;
        }
        registry->count = write_index;
    }
    return 0;
}

void mdkr_modern_character_registry_shutdown(MdkrModernCharacterRegistry *registry) {
    if (registry != NULL) memset(registry, 0, sizeof(*registry));
}

int mdkr_modern_character_registry_count(const MdkrModernCharacterRegistry *registry) {
    return registry != NULL ? registry->count : 0;
}

const MdkrModernCharacterEntry *mdkr_modern_character_registry_entry(
    const MdkrModernCharacterRegistry *registry, int index) {
    if (registry == NULL || index < 0 || index >= registry->count) return NULL;
    return &registry->entries[index];
}

int mdkr_modern_character_registry_find(const MdkrModernCharacterRegistry *registry,
                                        const char *id) {
    int low = 0;
    int high;
    if (registry == NULL || id == NULL) return -1;
    high = registry->count - 1;
    while (low <= high) {
        int middle = low + (high - low) / 2;
        int comparison = strcmp(registry->entries[middle].id, id);
        if (comparison == 0) return middle;
        if (comparison < 0) low = middle + 1;
        else high = middle - 1;
    }
    return -1;
}

int mdkr_modern_character_registry_skipped(const MdkrModernCharacterRegistry *registry) {
    return registry != NULL ? registry->skipped : 0;
}

const char *mdkr_modern_character_registry_skip_name(
    const MdkrModernCharacterRegistry *registry, int index) {
    if (registry == NULL || index < 0 || index >= registry->skipped) return NULL;
    return registry->skip_name[index];
}

const char *mdkr_modern_character_registry_skip_reason(
    const MdkrModernCharacterRegistry *registry, int index) {
    if (registry == NULL || index < 0 || index >= registry->skipped) return NULL;
    return registry->skip_reason[index];
}

int mdkr_modern_character_registry_load(const MdkrModernCharacterRegistry *registry,
                                        int index, MdkrModernCharacterAsset *out,
                                        char *error, size_t error_size) {
    const MdkrModernCharacterEntry *entry = mdkr_modern_character_registry_entry(registry, index);
    if (entry == NULL) {
        if (out != NULL) memset(out, 0, sizeof(*out));
        if (error != NULL && error_size != 0u) (void)snprintf(error, error_size, "character registry index is invalid");
        return 0;
    }
    return mdkr_modern_character_asset_load_file(entry->path, out, error, error_size);
}
