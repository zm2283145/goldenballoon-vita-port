/* Deterministic triangle-surface contact/intersection evidence for Character Workshop.
 *
 * This module deliberately knows nothing about DKR object models or custom
 * character assets. Callers provide already-qualified triangle geometry in one
 * shared coordinate frame. The bounded retained surface builds a small BVH;
 * subject triangles can then be streamed from a high-fidelity posed mesh
 * without allocating a second copy of that mesh.
 */
#ifndef MDKR64_MODERN_CHARACTER_SURFACE_INTERSECTION_H
#define MDKR64_MODERN_CHARACTER_SURFACE_INTERSECTION_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MDKR_MODERN_CHARACTER_SHELL_TRIANGLE_MAX 512u
#define MDKR_MODERN_CHARACTER_CONTAINMENT_SAMPLE_MAX 2048u

typedef struct MdkrModernSurfaceTriangle {
    float point[3][3];
} MdkrModernSurfaceTriangle;

typedef int (*MdkrModernSurfaceTriangleReader)(
    void *user, uint32_t triangle_index,
    MdkrModernSurfaceTriangle *triangle);

typedef struct MdkrModernSurfaceIntersectionDiagnostics {
    uint32_t shell_triangles_submitted;
    uint32_t shell_triangles_tested;
    uint32_t subject_triangles_submitted;
    uint32_t subject_triangles_tested;
    uint32_t crossing_subject_triangles;
    uint32_t crossing_pairs;
    /* Center of the first subject triangle that contacts/intersects the retained surface.
     * This is an exact source-triangle locator, not a penetration depth or the
     * mathematical intersection segment. */
    float first_crossing_subject_center[3];
    /* A containment result is published only when the retained shell proves
     * a closed, consistently oriented two-manifold topology and has no
     * non-adjacent triangle intersections. Subject triangle centroids are
     * sampled deterministically up to the fixed bound above. This is a
     * bounded posed-mesh witness, not a claim about unsampled points. */
    uint32_t shell_boundary_edges;
    uint32_t shell_nonmanifold_edges;
    uint32_t shell_orientation_mismatch_edges;
    uint32_t shell_self_intersection_pairs;
    uint32_t containment_qualified;
    uint32_t containment_samples_tested;
    uint32_t containment_inside_samples;
    uint32_t containment_boundary_samples;
    uint32_t containment_outside_samples;
    float containment_maximum_inside_depth;
    float containment_deepest_subject_point[3];
} MdkrModernSurfaceIntersectionDiagnostics;

/* Returns one for a complete, structurally valid measurement, including a
 * valid result with zero crossings. Degenerate zero-area triangles are skipped
 * and reported through submitted-versus-tested counts. Invalid/non-finite
 * input, an empty testable surface, callback failure, or a shell above the
 * fixed safety bound fails closed and clears `out`. */
int mdkr_modern_surface_intersections(
    const MdkrModernSurfaceTriangle *shell, uint32_t shell_triangles,
    uint32_t subject_triangles, MdkrModernSurfaceTriangleReader subject_reader,
    void *subject_user, MdkrModernSurfaceIntersectionDiagnostics *out);

#ifdef __cplusplus
}
#endif

#endif /* MDKR64_MODERN_CHARACTER_SURFACE_INTERSECTION_H */
