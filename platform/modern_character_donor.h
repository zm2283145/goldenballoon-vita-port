/* Retail-donor geometry qualification for modern character replacement. */
#ifndef MDKR64_MODERN_CHARACTER_DONOR_H
#define MDKR64_MODERN_CHARACTER_DONOR_H

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

#ifdef __cplusplus
}
#endif

#endif /* MDKR64_MODERN_CHARACTER_DONOR_H */
