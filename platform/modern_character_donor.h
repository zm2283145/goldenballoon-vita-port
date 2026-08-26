/* Retail-donor geometry qualification for modern character replacement. */
#ifndef MDKR64_MODERN_CHARACTER_DONOR_H
#define MDKR64_MODERN_CHARACTER_DONOR_H

#include "modern_character_asset.h"

#ifdef __cplusplus
extern "C" {
#endif

/* V1 deliberately supports one fully-qualified donor family. Unknown donors,
 * model ids, LODs, or fingerprints fail visible. */
int mdkr_modern_donor_model_ready(int donor, int vehicle, int model_id,
                                  int lod, int vertices, int triangles,
                                  int batches);

/* Call only after model_ready succeeds. Returns one for retail vehicle batches
 * and zero for the driver geometry replaced by the modern draw. */
int mdkr_modern_donor_batch_visible(int donor, int vehicle, int lod,
                                    int batch);

/* LOD 5 merges driver and vehicle in the supported retail donor. Retaining LOD
 * 4 at the presentation seam preserves a complete vehicle to carve. */
int mdkr_modern_donor_cap_lod(int donor, int vehicle, int lod);

/* Character-select actor qualification is deliberately separate from vehicle
 * qualification: the supported Diddy select mesh contains a four-vertex
 * numbered placard in batch zero and body geometry in every later batch. */
int mdkr_modern_donor_select_model_ready(int donor, int model_id,
                                         int vertices, int triangles,
                                         int batches);

/* Call only after select_model_ready succeeds. Keeps the numbered placard and
 * removes the retail body after the modern draw has registered atomically. */
int mdkr_modern_donor_select_batch_visible(int donor, int batch);

/* Qualified local attachment frame for each presentation context. These
 * engine-owned frames describe where a normalized package anchor lands inside
 * the donor object; packages describe their source anchors independently. */
int mdkr_modern_donor_attachment_frame(
    int donor, MdkrModernCharacterContext context, float output[16]);

/* Nominal standing height used to map author-facing meters to this donor's
 * qualified model-space body bounds. */
float mdkr_modern_donor_reference_height_m(int donor);

/* Derives the engine-side target frame from the qualified donor geometry.
 * `bounds_*` are the live, animated bounds of driver-only batches. The source
 * package has already been normalized to `normalized_height`; target_height_m
 * is the author-facing intended standing height. Character select lands on
 * the measured donor ground point, while vehicle contexts retain their
 * qualified seat frame. This pure function is shared by the game seam and
 * native tests so unit conversion never becomes an implicit renderer rule. */
int mdkr_modern_donor_fit_frame(
    int donor, MdkrModernCharacterContext context,
    const float bounds_min[3], const float bounds_max[3],
    float normalized_height, float target_height_m, float output[16]);

#ifdef __cplusplus
}
#endif

#endif /* MDKR64_MODERN_CHARACTER_DONOR_H */
