#include "modern_character_gameplay_profile.h"

#include "asset_enums.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

typedef struct MdkrRomSection {
    const uint8_t *bytes;
    uint32_t size;
} MdkrRomSection;

_Static_assert(sizeof(float) == 4u,
               "DKR gameplay tables require 32-bit IEEE float storage");

static void set_error(char *error, size_t size, const char *message) {
    if (error != NULL && size != 0u) {
        (void)snprintf(error, size, "%s",
                       message != NULL ? message : "unknown error");
    }
}

static uint32_t read_be32(const uint8_t *bytes) {
    return ((uint32_t)bytes[0] << 24u) | ((uint32_t)bytes[1] << 16u) |
           ((uint32_t)bytes[2] << 8u) | (uint32_t)bytes[3];
}

static uint16_t read_be16(const uint8_t *bytes) {
    return (uint16_t)(((uint16_t)bytes[0] << 8u) | (uint16_t)bytes[1]);
}

static float read_be_float(const uint8_t *bytes) {
    uint32_t bits = read_be32(bytes);
    float value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static int section_view(const uint8_t *rom, uint32_t rom_size,
                        const DkrRomId *id, uint32_t section,
                        MdkrRomSection *out) {
    uint32_t count;
    uint32_t start;
    uint32_t end;
    uint64_t absolute_start;
    uint64_t absolute_end;
    if (rom == NULL || id == NULL || out == NULL ||
        id->assetLutStart >= id->assetLutEnd ||
        id->assetLutEnd > rom_size || id->romEnd > rom_size ||
        (uint64_t)id->assetLutStart + 4u > id->assetLutEnd) return 0;
    count = read_be32(rom + id->assetLutStart);
    if (section >= count ||
        (uint64_t)id->assetLutStart + ((uint64_t)section + 3u) * 4u >
            id->assetLutEnd) return 0;
    start = read_be32(rom + id->assetLutStart + (section + 1u) * 4u);
    end = read_be32(rom + id->assetLutStart + (section + 2u) * 4u);
    absolute_start = (uint64_t)id->assetLutEnd + start;
    absolute_end = (uint64_t)id->assetLutEnd + end;
    if (end < start || absolute_end > id->romEnd ||
        absolute_end > rom_size || absolute_start > absolute_end) return 0;
    out->bytes = rom + absolute_start;
    out->size = end - start;
    return 1;
}

static int misc_subasset(const MdkrRomSection *misc,
                         const MdkrRomSection *table, uint32_t index,
                         MdkrRomSection *out) {
    uint32_t start_words;
    uint32_t end_words;
    uint64_t start;
    uint64_t end;
    if (misc == NULL || table == NULL || out == NULL ||
        ((uint64_t)index + 2u) * 4u > table->size) return 0;
    start_words = read_be32(table->bytes + index * 4u);
    end_words = read_be32(table->bytes + (index + 1u) * 4u);
    if (start_words == UINT32_MAX || end_words == UINT32_MAX ||
        end_words < start_words) return 0;
    start = (uint64_t)start_words * 4u;
    end = (uint64_t)end_words * 4u;
    if (end > misc->size || start > end) return 0;
    out->bytes = misc->bytes + start;
    out->size = (uint32_t)(end - start);
    return 1;
}

static int byte_offset_subasset(const MdkrRomSection *section,
                                const MdkrRomSection *table, uint32_t index,
                                MdkrRomSection *out) {
    uint32_t start;
    uint32_t end;
    if (section == NULL || table == NULL || out == NULL ||
        ((uint64_t)index + 2u) * 4u > table->size) return 0;
    start = read_be32(table->bytes + index * 4u);
    end = read_be32(table->bytes + (index + 1u) * 4u);
    if (start == UINT32_MAX || end == UINT32_MAX || end < start ||
        end > section->size) return 0;
    out->bytes = section->bytes + start;
    out->size = end - start;
    return 1;
}

static int read_float_array(const MdkrRomSection *misc,
                            const MdkrRomSection *table, uint32_t index,
                            float *output, uint32_t count) {
    MdkrRomSection source;
    uint32_t element;
    if (output == NULL || count == 0u ||
        !misc_subasset(misc, table, index, &source) ||
        (uint64_t)source.size < (uint64_t)count * 4u) return 0;
    for (element = 0u; element < count; element++) {
        output[element] = read_be_float(source.bytes + element * 4u);
        if (!isfinite(output[element])) return 0;
    }
    return 1;
}

int mdkr_donor_gameplay_profiles_from_rom(
    const uint8_t *rom, uint32_t rom_size, const DkrRomId *id,
    MdkrDonorGameplayProfiles *out, char *error, size_t error_size) {
    static const uint16_t racer_object_ids[MDKR_DONOR_VEHICLE_COUNT]
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
    MdkrRomSection misc;
    MdkrRomSection table;
    MdkrRomSection object_headers;
    MdkrRomSection object_header_table;
    MdkrRomSection object_translation;
    float weights[MDKR_DONOR_GAMEPLAY_PROFILE_COUNT];
    float handling[MDKR_DONOR_GAMEPLAY_PROFILE_COUNT];
    uint32_t donor;
    uint32_t vehicle;
    if (out == NULL) {
        set_error(error, error_size, "donor profile output is missing");
        return 0;
    }
    memset(out, 0, sizeof(*out));
    if (!section_view(rom, rom_size, id, ASSET_MISC, &misc) ||
        !section_view(rom, rom_size, id, ASSET_MISC_TABLE, &table) ||
        !section_view(rom, rom_size, id, ASSET_OBJECT_HEADERS_TABLE,
                      &object_header_table) ||
        !section_view(rom, rom_size, id, ASSET_OBJECTS, &object_headers) ||
        !section_view(rom, rom_size, id,
                      ASSET_LEVEL_OBJECT_TRANSLATION_TABLE,
                      &object_translation)) {
        set_error(error, error_size,
                  "the validated ROM has no bounded donor gameplay sections");
        return 0;
    }
    if (!read_float_array(&misc, &table, ASSET_MISC_RACER_WEIGHT,
                          weights, MDKR_DONOR_GAMEPLAY_PROFILE_COUNT) ||
        !read_float_array(&misc, &table, ASSET_MISC_RACER_HANDLING,
                          handling, MDKR_DONOR_GAMEPLAY_PROFILE_COUNT)) {
        set_error(error, error_size,
                  "the donor weight or handling table is invalid");
        return 0;
    }
    for (donor = 0u; donor < MDKR_DONOR_GAMEPLAY_PROFILE_COUNT; donor++) {
        MdkrDonorGameplayProfile *profile = &out->donor[donor];
        profile->weight = weights[donor] * 0.45f;
        profile->handling = handling[donor];
        if (!isfinite(profile->weight) || !isfinite(profile->handling)) {
            set_error(error, error_size, "a donor coefficient is invalid");
            memset(out, 0, sizeof(*out));
            return 0;
        }
        for (vehicle = 0u; vehicle < MDKR_DONOR_VEHICLE_COUNT; vehicle++) {
            MdkrRomSection header;
            uint32_t object_id = racer_object_ids[vehicle][donor];
            uint32_t header_type;
            uint32_t acceleration_id;
            if ((uint64_t)(object_id + 1u) * 2u >
                    object_translation.size) {
                set_error(error, error_size,
                          "a donor vehicle translation is out of bounds");
                memset(out, 0, sizeof(*out));
                return 0;
            }
            header_type = read_be16(
                object_translation.bytes + object_id * 2u);
            if (!byte_offset_subasset(&object_headers,
                                      &object_header_table, header_type,
                                      &header) || header.size <= 0x5Cu) {
                set_error(error, error_size,
                          "a donor vehicle header is invalid");
                memset(out, 0, sizeof(*out));
                return 0;
            }
            acceleration_id = header.bytes[0x5Cu];
            if (!read_float_array(&misc, &table, acceleration_id,
                                  profile->acceleration[vehicle],
                                  MDKR_DONOR_ACCELERATION_SAMPLES)) {
                set_error(error, error_size,
                          "a donor vehicle acceleration curve is invalid");
                memset(out, 0, sizeof(*out));
                return 0;
            }
        }
    }
    out->version = MDKR_DONOR_GAMEPLAY_PROFILE_VERSION;
    out->available = 1u;
    out->donor_count = MDKR_DONOR_GAMEPLAY_PROFILE_COUNT;
    set_error(error, error_size, "");
    return 1;
}
