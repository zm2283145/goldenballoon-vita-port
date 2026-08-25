#include "modern_character_donor.h"

#include "asset_enums.h"

#include <stdint.h>

#define MDKR_DONOR_DIDDY 9
#define BITS(first, last) \
    ((((UINT32_C(1) << ((last) - (first) + 1)) - UINT32_C(1))) << (first))

typedef struct MdkrDonorFingerprint {
    int vertices;
    int triangles;
    int batches;
} MdkrDonorFingerprint;

static const int s_diddy_first_model[3] = {
    ASSET_OBJECTMODEL_DIDDYCAR_0,
    ASSET_OBJECTMODEL_DIDDYHOVER_0,
    ASSET_OBJECTMODEL_DIDDYPLANE_0,
};

static const MdkrDonorFingerprint s_diddy_fingerprints[3][5] = {
    {
        { 325, 257, 30 }, { 282, 206, 28 }, { 234, 164, 25 },
        { 172, 115, 19 }, { 131, 82, 13 },
    },
    {
        { 329, 248, 30 }, { 311, 218, 29 }, { 272, 182, 27 },
        { 239, 143, 26 }, { 153, 96, 16 },
    },
    {
        { 348, 267, 32 }, { 314, 225, 30 }, { 262, 181, 26 },
        { 194, 133, 19 }, { 143, 97, 14 },
    },
};

/* These masks are data about the supported retail Diddy model schemas, not a
 * heuristic. They were qualified against the US/PAL revision-1 shared asset
 * corpus. A model must pass the id and geometry fingerprint above before a bit
 * is consumed. Bit one means "driver: replace it". */
static const uint32_t s_diddy_driver_batches[3][5] = {
    {
        BITS(0, 17) | (UINT32_C(1) << 27),
        BITS(0, 16) | (UINT32_C(1) << 26),
        BITS(0, 14) | (UINT32_C(1) << 23),
        BITS(0, 12) | (UINT32_C(1) << 18),
        BITS(0, 10),
    },
    {
        BITS(0, 9) | BITS(11, 18) | (UINT32_C(1) << 29),
        BITS(0, 8) | BITS(10, 17) | (UINT32_C(1) << 28),
        BITS(0, 13) | (UINT32_C(1) << 15) | (UINT32_C(1) << 26),
        BITS(10, 21),
        BITS(5, 14),
    },
    {
        BITS(0, 9) | BITS(11, 18) | (UINT32_C(1) << 30),
        BITS(1, 16) | (UINT32_C(1) << 24) | (UINT32_C(1) << 28),
        BITS(1, 14) | (UINT32_C(1) << 20) | (UINT32_C(1) << 24),
        (UINT32_C(1) << 1) | BITS(5, 16),
        BITS(0, 9),
    },
};

int mdkr_modern_donor_model_ready(int donor, int vehicle, int model_id,
                                  int lod, int vertices, int triangles,
                                  int batches) {
    const MdkrDonorFingerprint *expected;
    if (donor != MDKR_DONOR_DIDDY || vehicle < 0 || vehicle >= 3 ||
        lod < 0 || lod >= 5 || model_id != s_diddy_first_model[vehicle] + lod) {
        return 0;
    }
    expected = &s_diddy_fingerprints[vehicle][lod];
    return vertices == expected->vertices &&
           triangles == expected->triangles && batches == expected->batches;
}

int mdkr_modern_donor_batch_visible(int donor, int vehicle, int lod,
                                    int batch) {
    if (donor != MDKR_DONOR_DIDDY || vehicle < 0 || vehicle >= 3 ||
        lod < 0 || lod >= 5 || batch < 0 || batch >= 32) {
        return 1;
    }
    return (s_diddy_driver_batches[vehicle][lod] &
            (UINT32_C(1) << batch)) == 0;
}

int mdkr_modern_donor_cap_lod(int donor, int vehicle, int lod) {
    if (donor == MDKR_DONOR_DIDDY && vehicle >= 0 && vehicle < 3 && lod > 4) {
        return 4;
    }
    return lod;
}

int mdkr_modern_donor_select_model_ready(int donor, int model_id,
                                         int vertices, int triangles,
                                         int batches) {
    return donor == MDKR_DONOR_DIDDY &&
           model_id == ASSET_OBJECTMODEL_DIDDYSELECT &&
           vertices == 343 && triangles == 297 && batches == 28;
}

int mdkr_modern_donor_select_batch_visible(int donor, int batch) {
    if (donor != MDKR_DONOR_DIDDY || batch < 0 || batch >= 28) return 1;
    return batch == 0;
}
