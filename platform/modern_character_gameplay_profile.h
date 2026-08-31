/* Exact, bounded donor gameplay summaries extracted from the player's already
 * validated base ROM. No ROM bytes or mutable simulation pointers escape. */
#ifndef MDKR64_MODERN_CHARACTER_GAMEPLAY_PROFILE_H
#define MDKR64_MODERN_CHARACTER_GAMEPLAY_PROFILE_H

#include <stddef.h>
#include <stdint.h>

#include "rom_id.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MDKR_DONOR_GAMEPLAY_PROFILE_VERSION 1u
#define MDKR_DONOR_GAMEPLAY_PROFILE_COUNT 10u
#define MDKR_DONOR_VEHICLE_COUNT 3u
/* Every retail vehicle path samples indices 0..13 after clamping speed to 12. */
#define MDKR_DONOR_ACCELERATION_SAMPLES 14u

typedef enum MdkrDonorVehicle {
    MDKR_DONOR_VEHICLE_CAR = 0,
    MDKR_DONOR_VEHICLE_HOVERCRAFT = 1,
    MDKR_DONOR_VEHICLE_PLANE = 2
} MdkrDonorVehicle;

typedef struct MdkrDonorGameplayProfile {
    /* Exact coefficient consumed after the game's authored 0.45 weight scale. */
    float weight;
    float handling;
    /* Curves resolved through each donor vehicle's live ObjectHeader.unk5C. */
    float acceleration[MDKR_DONOR_VEHICLE_COUNT]
                      [MDKR_DONOR_ACCELERATION_SAMPLES];
} MdkrDonorGameplayProfile;

typedef struct MdkrDonorGameplayProfiles {
    uint32_t version;
    uint32_t available;
    uint32_t donor_count;
    MdkrDonorGameplayProfile donor[MDKR_DONOR_GAMEPLAY_PROFILE_COUNT];
} MdkrDonorGameplayProfiles;

/* `rom` must already be normalized to canonical big-endian order. The caller
 * retains ownership; this function copies only the bounded numeric summary. */
int mdkr_donor_gameplay_profiles_from_rom(
    const uint8_t *rom, uint32_t rom_size, const DkrRomId *id,
    MdkrDonorGameplayProfiles *out, char *error, size_t error_size);

#ifdef __cplusplus
}
#endif

#endif /* MDKR64_MODERN_CHARACTER_GAMEPLAY_PROFILE_H */
