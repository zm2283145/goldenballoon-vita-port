#include "modern_character_donor.h"

#include "asset_enums.h"

#include <math.h>
#include <stdint.h>

#define MDKR_DONOR_COUNT 10
#define MDKR_VEHICLE_COUNT 3
#define MDKR_CARVEABLE_LOD_COUNT 5

typedef struct MdkrDonorFingerprint {
    int16_t vertices;
    int16_t triangles;
    int16_t batches;
} MdkrDonorFingerprint;

typedef struct MdkrDonorProfile {
    int first_model[MDKR_VEHICLE_COUNT];
    MdkrDonorFingerprint model[MDKR_VEHICLE_COUNT][MDKR_CARVEABLE_LOD_COUNT];
    uint64_t driver_batches[MDKR_VEHICLE_COUNT][MDKR_CARVEABLE_LOD_COUNT];
    int select_model;
    MdkrDonorFingerprint select;
    float reference_height_m;
} MdkrDonorProfile;

/*
 * Immutable facts about the retail racer schemas, indexed by Character:
 * Krunch, Bumper, Tiptup, Conker, Timber, Banjo, Drumstick, Pipsy, T.T., Diddy.
 *
 * The fingerprints and masks were derived from every car/hovercraft/plane LOD
 * in the shared US/PAL revision-1 object-model corpus. A mask bit means that
 * the batch belongs to the animated driver rather than the retained vehicle.
 * The id plus the complete (vertex, triangle, batch) tuple is checked before a
 * mask is consumed, so a different ROM schema always fails visible.
 *
 * Masks are 64-bit deliberately: Drumstick's authored hovercraft and plane
 * models exceed 32 batches. Do not narrow these records or infer ownership at
 * draw time from a mutable texture pointer.
 */
static const MdkrDonorProfile s_profiles[MDKR_DONOR_COUNT] = {
    { /* Krunch */
        { ASSET_OBJECTMODEL_KREMCAR_0, ASSET_OBJECTMODEL_KREMLINHOVER_0,
          ASSET_OBJECTMODEL_KREMPLANE_0 },
        {
            { {309,240,29},{258,168,30},{192,122,24},{129,72,17},{121,64,13} },
            { {323,237,30},{285,182,30},{234,137,29},{196,100,24},{143,78,16} },
            { {342,256,32},{287,188,31},{220,142,25},{151,90,17},{125,75,13} },
        },
        {
            { UINT64_C(0x000807ffff), UINT64_C(0x001019fdff), UINT64_C(0x0000809fdf), UINT64_C(0x00000104fe), UINT64_C(0x00000004f7) },
            { UINT64_C(0x00200ffbff), UINT64_C(0x00200cfeff), UINT64_C(0x0010009fdf), UINT64_C(0x00008004fe), UINT64_C(0x00000004fe) },
            { UINT64_C(0x00400ffbff), UINT64_C(0x00200cfeff), UINT64_C(0x0000809fdf), UINT64_C(0x00000104fe), UINT64_C(0x000000109f) },
        },
        ASSET_OBJECTMODEL_KREMSELECT, {330,252,27}, 1.25f,
    },
    { /* Bumper */
        { ASSET_OBJECTMODEL_BADGERCAR_0, ASSET_OBJECTMODEL_BADGERHOVER_0,
          ASSET_OBJECTMODEL_BADGERPLANE_0 },
        {
            { {347,261,32},{260,175,26},{230,138,25},{139,83,21},{88,53,11} },
            { {360,254,33},{285,182,27},{246,145,27},{206,111,26},{110,67,15} },
            { {370,273,34},{288,189,27},{262,160,25},{161,101,20},{135,86,15} },
        },
        {
            { UINT64_C(0x0000ffe7ff), UINT64_C(0x000003fffd), UINT64_C(0x000001fffd), UINT64_C(0x0000003ffd), UINT64_C(0x00000001ed) },
            { UINT64_C(0x00007fffff), UINT64_C(0x000003fffd), UINT64_C(0x000001fffd), UINT64_C(0x0000000ffd), UINT64_C(0x00000001fd) },
            { UINT64_C(0x0000fff7ff), UINT64_C(0x0000f03fff), UINT64_C(0x000000ffff), UINT64_C(0x0000020fff), UINT64_C(0x00000011ff) },
        },
        ASSET_OBJECTMODEL_BADGERSELECT, {325,249,27}, 1.25f,
    },
    { /* Tiptup */
        { ASSET_OBJECTMODEL_TORTCAR_0, ASSET_OBJECTMODEL_TORTHOVER_0,
          ASSET_OBJECTMODEL_TORTPLANE_0 },
        {
            { {317,244,28},{221,149,24},{163,104,21},{95,55,15},{91,47,12} },
            { {328,235,29},{248,159,26},{199,123,24},{160,90,22},{115,63,16} },
            { {336,256,30},{253,168,26},{191,124,22},{117,73,16},{107,64,15} },
        },
        {
            { UINT64_C(0x000001ffff), UINT64_C(0x0000003fff), UINT64_C(0x0000001f7f), UINT64_C(0x00000007be), UINT64_C(0x00000003be) },
            { UINT64_C(0x000001ffff), UINT64_C(0x0000003fff), UINT64_C(0x0000001f7f), UINT64_C(0x00000007fe), UINT64_C(0x00000003fe) },
            { UINT64_C(0x000003ffff), UINT64_C(0x00001f07fd), UINT64_C(0x0000020fdf), UINT64_C(0x00000031fe), UINT64_C(0x0000000cff) },
        },
        ASSET_OBJECTMODEL_TORTSELECT, {315,244,23}, 1.25f,
    },
    { /* Conker */
        { ASSET_OBJECTMODEL_CONKACAR_0, ASSET_OBJECTMODEL_CONKAHOVER_0,
          ASSET_OBJECTMODEL_CONKA_0 },
        {
            { {329,259,27},{280,203,25},{231,166,23},{170,117,20},{145,89,15} },
            { {334,249,27},{309,215,26},{266,181,25},{237,145,27},{165,101,18} },
            { {355,270,29},{307,219,27},{253,179,24},{186,131,20},{157,104,16} },
        },
        {
            { UINT64_C(0x0000073fff), UINT64_C(0x000001fffd), UINT64_C(0x000000fffd), UINT64_C(0x0000007ffd), UINT64_C(0x0000001ffe) },
            { UINT64_C(0x000001ffff), UINT64_C(0x000000ffff), UINT64_C(0x0000007fff), UINT64_C(0x000003e3fd), UINT64_C(0x0000001ffe) },
            { UINT64_C(0x000003fffd), UINT64_C(0x000001fffd), UINT64_C(0x000000fffd), UINT64_C(0x0000007ffd), UINT64_C(0x0000000fff) },
        },
        ASSET_OBJECTMODEL_CONKSELECT, {306,245,24}, 1.25f,
    },
    { /* Timber */
        { ASSET_OBJECTMODEL_TIGERCAR_0, ASSET_OBJECTMODEL_TIGERHOVER_0,
          ASSET_OBJECTMODEL_TIGPLANE_0 },
        {
            { {367,258,36},{294,179,35},{220,129,28},{148,83,23},{131,70,17} },
            { {363,243,35},{317,187,35},{257,147,29},{215,111,30},{154,84,20} },
            { {390,268,38},{326,198,37},{248,146,29},{170,101,23},{156,92,18} },
        },
        {
            { UINT64_C(0x000f3fffff), UINT64_C(0x00073ff7ff), UINT64_C(0x000119ffef), UINT64_C(0x000009bfef), UINT64_C(0x0000012ffb) },
            { UINT64_C(0x0003ffffff), UINT64_C(0x00039fffff), UINT64_C(0x0008077fff), UINT64_C(0x001001bfff), UINT64_C(0x0000042fff) },
            { UINT64_C(0x0007ffffff), UINT64_C(0x00073fffff), UINT64_C(0x00020dffff), UINT64_C(0x000011bfff), UINT64_C(0x00000097ff) },
        },
        ASSET_OBJECTMODEL_TIGERSELECT, {367,268,33}, 1.25f,
    },
    { /* Banjo */
        { ASSET_OBJECTMODEL_BANJOCAR_0, ASSET_OBJECTMODEL_BANJOHOVER_0,
          ASSET_OBJECTMODEL_BANJOPLANE_0 },
        {
            { {315,247,32},{227,160,29},{181,111,25},{120,74,19},{95,55,13} },
            { {323,240,32},{256,172,30},{217,130,27},{178,106,23},{121,71,17} },
            { {342,259,34},{260,180,30},{209,131,26},{142,92,19},{120,79,15} },
        },
        {
            { UINT64_C(0x00003fefff), UINT64_C(0x00001fb7ff), UINT64_C(0x000000fbff), UINT64_C(0x0000001f5f), UINT64_C(0x0000000f7a) },
            { UINT64_C(0x00007f6fff), UINT64_C(0x00001fb7ff), UINT64_C(0x000001f6ff), UINT64_C(0x0000000fbf), UINT64_C(0x000000077b) },
            { UINT64_C(0x00003fefff), UINT64_C(0x00000ff7ff), UINT64_C(0x000001f6ff), UINT64_C(0x0000001f7b), UINT64_C(0x00000007df) },
        },
        ASSET_OBJECTMODEL_BANJOSELECT, {308,249,28}, 1.25f,
    },
    { /* Drumstick */
        { ASSET_OBJECTMODEL_CHICKENCAR_0, ASSET_OBJECTMODEL_CHICKENHOVER_0,
          ASSET_OBJECTMODEL_CHICKENPLANE_0 },
        {
            { {368,245,37},{289,180,31},{208,129,23},{139,82,18},{117,66,14} },
            { {371,235,36},{314,190,31},{235,140,24},{199,108,23},{137,77,17} },
            { {399,259,39},{322,202,31},{240,148,24},{158,100,17},{138,87,15} },
        },
        {
            { UINT64_C(0x001fffffcf), UINT64_C(0x00001ffffb), UINT64_C(0x000001f3fe), UINT64_C(0x0000001e3e), UINT64_C(0x0000000f3e) },
            { UINT64_C(0x0601ffffff), UINT64_C(0x00000fffff), UINT64_C(0x0000007bff), UINT64_C(0x000000073f), UINT64_C(0x0000000e3f) },
            { UINT64_C(0x080fffffff), UINT64_C(0x0007f01ffd), UINT64_C(0x00001f05fe), UINT64_C(0x000000479e), UINT64_C(0x00000003cf) },
        },
        ASSET_OBJECTMODEL_CHICKSELECT, {343,238,32}, 1.25f,
    },
    { /* Pipsy */
        { ASSET_OBJECTMODEL_MOUSECAR_0, ASSET_OBJECTMODEL_MOUSEHOVER_0,
          ASSET_OBJECTMODEL_MOUSEPLANE_0 },
        {
            { {284,234,25},{239,187,22},{220,166,22},{113,76,15},{102,64,12} },
            { {287,225,24},{276,200,23},{247,180,23},{171,108,19},{122,78,15} },
            { {306,244,26},{278,206,24},{241,183,22},{133,93,15},{121,83,13} },
        },
        {
            { UINT64_C(0x000000fffc), UINT64_C(0x0000003ffe), UINT64_C(0x000000fdfc), UINT64_C(0x00000003f6), UINT64_C(0x00000003f6) },
            { UINT64_C(0x00007e01fe), UINT64_C(0x00003f00fe), UINT64_C(0x00003f00fe), UINT64_C(0x00000003f6), UINT64_C(0x00000003f6) },
            { UINT64_C(0x000000fffe), UINT64_C(0x0000003fbf), UINT64_C(0x0000003ffe), UINT64_C(0x00000003f6), UINT64_C(0x00000001fe) },
        },
        ASSET_OBJECTMODEL_MOUSESELECT, {287,235,24}, 1.25f,
    },
    { /* T.T. */
        { ASSET_OBJECTMODEL_SWCAR_0, ASSET_OBJECTMODEL_TICKTOCKHOVER_0,
          ASSET_OBJECTMODEL_TICKTOCKPLANE_0 },
        {
            { {303,226,25},{259,181,24},{197,137,22},{156,109,17},{88,63,10} },
            { {299,215,25},{269,182,24},{216,141,21},{180,116,19},{122,78,16} },
            { {323,237,26},{271,194,25},{223,156,20},{160,117,15},{100,78,11} },
        },
        {
            { UINT64_C(0x000001fffc), UINT64_C(0x0000003ffe), UINT64_C(0x0000001ffe), UINT64_C(0x00000003fc), UINT64_C(0x00000000fe) },
            { UINT64_C(0x000000fffc), UINT64_C(0x000003ffe0), UINT64_C(0x00000007fe), UINT64_C(0x00000001fe), UINT64_C(0x000000007f) },
            { UINT64_C(0x00007e03fc), UINT64_C(0x00000f03fe), UINT64_C(0x000000e0ff), UINT64_C(0x0000000e3e), UINT64_C(0x000000019f) },
        },
        ASSET_OBJECTMODEL_STOPWATCHSELECT, {355,306,26}, 1.25f,
    },
    { /* Diddy */
        { ASSET_OBJECTMODEL_DIDDYCAR_0, ASSET_OBJECTMODEL_DIDDYHOVER_0,
          ASSET_OBJECTMODEL_DIDDYPLANE_0 },
        {
            { {325,257,30},{282,206,28},{234,164,25},{172,115,19},{131,82,13} },
            { {329,248,30},{311,218,29},{272,182,27},{239,143,26},{153,96,16} },
            { {348,267,32},{314,225,30},{262,181,26},{194,133,19},{143,97,14} },
        },
        {
            { UINT64_C(0x000803ffff), UINT64_C(0x000401ffff), UINT64_C(0x0000807fff), UINT64_C(0x0000041fff), UINT64_C(0x00000007ff) },
            { UINT64_C(0x002007fbff), UINT64_C(0x001003fdff), UINT64_C(0x000400bfff), UINT64_C(0x00003ffc00), UINT64_C(0x0000007fe0) },
            { UINT64_C(0x004007fbff), UINT64_C(0x001101fffe), UINT64_C(0x0001107ffe), UINT64_C(0x000001ffe2), UINT64_C(0x00000003ff) },
        },
        ASSET_OBJECTMODEL_DIDDYSELECT, {343,297,28}, 1.25f,
    },
};

static int profile_sane(const MdkrDonorProfile *profile) {
    int vehicle;
    int lod;
    if (profile == NULL || profile->select.batches <= 1 ||
        profile->select.batches >= 64 || profile->reference_height_m <= 0.0f ||
        !isfinite(profile->reference_height_m)) return 0;
    for (vehicle = 0; vehicle < MDKR_VEHICLE_COUNT; vehicle++) {
        if (profile->first_model[vehicle] < 0) return 0;
        for (lod = 0; lod < MDKR_CARVEABLE_LOD_COUNT; lod++) {
            const MdkrDonorFingerprint *fingerprint =
                &profile->model[vehicle][lod];
            const uint64_t mask = profile->driver_batches[vehicle][lod];
            uint64_t authored_batches;
            if (fingerprint->vertices <= 0 || fingerprint->triangles <= 0 ||
                fingerprint->batches <= 1 || fingerprint->batches >= 64) return 0;
            authored_batches = (UINT64_C(1) << fingerprint->batches) - 1u;
            if (mask == 0u || mask == authored_batches ||
                (mask & ~authored_batches) != 0u) return 0;
        }
    }
    return 1;
}

static const MdkrDonorProfile *profile_for(int donor) {
    const MdkrDonorProfile *profile;
    if (donor < 0 || donor >= MDKR_DONOR_COUNT) return NULL;
    profile = &s_profiles[donor];
    return profile_sane(profile) ? profile : NULL;
}

int mdkr_modern_donor_qualified(int donor) {
    return profile_for(donor) != NULL;
}

int mdkr_modern_donor_model_ready(int donor, int vehicle, int model_id,
                                  int lod, int vertices, int triangles,
                                  int batches) {
    const MdkrDonorProfile *profile = profile_for(donor);
    const MdkrDonorFingerprint *expected;
    if (profile == NULL || vehicle < 0 || vehicle >= MDKR_VEHICLE_COUNT ||
        lod < 0 || lod >= MDKR_CARVEABLE_LOD_COUNT ||
        model_id != profile->first_model[vehicle] + lod) return 0;
    expected = &profile->model[vehicle][lod];
    return vertices == expected->vertices && triangles == expected->triangles &&
           batches == expected->batches;
}

int mdkr_modern_donor_batch_visible(int donor, int vehicle, int lod,
                                    int batch) {
    const MdkrDonorProfile *profile = profile_for(donor);
    if (profile == NULL || vehicle < 0 || vehicle >= MDKR_VEHICLE_COUNT ||
        lod < 0 || lod >= MDKR_CARVEABLE_LOD_COUNT ||
        batch < 0 || batch >= 64) return 1;
    return (profile->driver_batches[vehicle][lod] &
            (UINT64_C(1) << (unsigned)batch)) == 0u;
}

int mdkr_modern_donor_cap_lod(int donor, int vehicle, int lod) {
    if (profile_for(donor) != NULL && vehicle >= 0 &&
        vehicle < MDKR_VEHICLE_COUNT && lod >= MDKR_CARVEABLE_LOD_COUNT) {
        return MDKR_CARVEABLE_LOD_COUNT - 1;
    }
    return lod;
}

int mdkr_modern_donor_select_model_ready(int donor, int model_id,
                                         int vertices, int triangles,
                                         int batches) {
    const MdkrDonorProfile *profile = profile_for(donor);
    return profile != NULL && model_id == profile->select_model &&
           vertices == profile->select.vertices &&
           triangles == profile->select.triangles &&
           batches == profile->select.batches;
}

int mdkr_modern_donor_select_batch_visible(int donor, int batch) {
    const MdkrDonorProfile *profile = profile_for(donor);
    if (profile == NULL || batch < 0 || batch >= profile->select.batches) return 1;
    return batch == 0;
}

int mdkr_modern_donor_attachment_frame(
    int donor, MdkrModernCharacterContext context, float output[16]) {
    int index;
    if (profile_for(donor) == NULL || output == NULL ||
        context < MDKR_CHARACTER_CONTEXT_SELECT ||
        context >= MDKR_CHARACTER_CONTEXT_COUNT) return 0;
    for (index = 0; index < 16; index++) output[index] = 0.0f;
    output[0] = output[5] = output[10] = output[15] = 1.0f;
    /* All ten qualified object-model families express their ground/seat frame
     * at local origin. Keeping that corpus fact here prevents the renderer from
     * growing donor-specific coordinate guesses. */
    return 1;
}

float mdkr_modern_donor_reference_height_m(int donor) {
    const MdkrDonorProfile *profile = profile_for(donor);
    return profile != NULL ? profile->reference_height_m : 0.0f;
}

int mdkr_modern_donor_fit_frame(
    int donor, MdkrModernCharacterContext context,
    const float bounds_min[3], const float bounds_max[3],
    float normalized_height, float target_height_m, float output[16]) {
    float reference_height;
    float donor_height;
    float target_scale;
    int axis;
    int index;
    if (bounds_min == NULL || bounds_max == NULL || output == NULL ||
        context < MDKR_CHARACTER_CONTEXT_SELECT ||
        context >= MDKR_CHARACTER_CONTEXT_COUNT ||
        !isfinite(normalized_height) || normalized_height <= 0.0f ||
        !isfinite(target_height_m) || target_height_m <= 0.0f ||
        !mdkr_modern_donor_attachment_frame(donor, context, output)) return 0;
    for (axis = 0; axis < 3; axis++) {
        if (!isfinite(bounds_min[axis]) || !isfinite(bounds_max[axis]) ||
            bounds_max[axis] < bounds_min[axis]) return 0;
    }
    reference_height = mdkr_modern_donor_reference_height_m(donor);
    donor_height = bounds_max[1] - bounds_min[1];
    if (!isfinite(reference_height) || reference_height <= 0.0f ||
        !isfinite(donor_height) || donor_height <= 0.0f) return 0;
    target_scale = donor_height * target_height_m /
                   (reference_height * normalized_height);
    if (!isfinite(target_scale) || target_scale <= 0.0f) return 0;

    /* Preserve a donor profile's rotation while applying its measured unit
     * conversion. Translation is never scaled. */
    for (index = 0; index < 12; index++) {
        if ((index & 3) != 3) output[index] *= target_scale;
    }
    if (context == MDKR_CHARACTER_CONTEXT_SELECT) {
        output[12] += (bounds_min[0] + bounds_max[0]) * 0.5f;
        output[13] += bounds_min[1];
        output[14] += (bounds_min[2] + bounds_max[2]) * 0.5f;
    }
    return 1;
}
