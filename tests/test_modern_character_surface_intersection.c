#include "modern_character_surface_intersection.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void require(int condition, const char *message) {
    if (condition) return;
    fprintf(stderr, "test_modern_character_surface_intersection: %s\n",
            message);
    exit(1);
}

typedef struct SubjectFixture {
    const MdkrModernSurfaceTriangle *triangle;
    uint32_t count;
    int fail;
} SubjectFixture;

static int read_subject(
    void *user, uint32_t index, MdkrModernSurfaceTriangle *triangle) {
    const SubjectFixture *fixture = (const SubjectFixture *)user;
    if (fixture == NULL || triangle == NULL || fixture->fail ||
        index >= fixture->count) return 0;
    *triangle = fixture->triangle[index];
    return 1;
}

static MdkrModernSurfaceTriangle triangle(
    float ax, float ay, float az, float bx, float by, float bz,
    float cx, float cy, float cz) {
    MdkrModernSurfaceTriangle result = {{
        {ax, ay, az}, {bx, by, bz}, {cx, cy, cz},
    }};
    return result;
}

int main(void) {
    MdkrModernSurfaceTriangle shell[5];
    MdkrModernSurfaceTriangle subject[6];
    /* Two coplanar leaves force both BVH and coplanar separation paths. */
    shell[0] = triangle(0.0f, 0.0f, 0.0f, 2.0f, 0.0f, 0.0f,
                        0.0f, 2.0f, 0.0f);
    shell[1] = triangle(2.0f, 0.0f, 0.0f, 2.0f, 2.0f, 0.0f,
                        0.0f, 2.0f, 0.0f);
    shell[2] = triangle(10.0f, 10.0f, 0.0f, 11.0f, 10.0f, 0.0f,
                        10.0f, 11.0f, 0.0f);
    shell[3] = triangle(20.0f, 20.0f, 0.0f, 21.0f, 20.0f, 0.0f,
                        20.0f, 21.0f, 0.0f);
    /* Degenerate retained geometry is reported but cannot intersect. */
    shell[4] = triangle(1.0f, 1.0f, 0.0f, 1.0f, 1.0f, 0.0f,
                        1.0f, 1.0f, 0.0f);
    /* Orthogonal crossing. */
    subject[0] = triangle(0.5f, 0.5f, -1.0f, 0.5f, 0.5f, 1.0f,
                          1.5f, 0.5f, 0.0f);
    /* Disjoint parallel triangle. */
    subject[1] = triangle(0.2f, 0.2f, 2.0f, 0.8f, 0.2f, 2.0f,
                          0.2f, 0.8f, 2.0f);
    /* Disjoint but coplanar: must not become a false crossing. */
    subject[2] = triangle(4.0f, 4.0f, 0.0f, 5.0f, 4.0f, 0.0f,
                          4.0f, 5.0f, 0.0f);
    /* Coplanar overlap. */
    subject[3] = triangle(0.25f, 0.25f, 0.0f, 0.75f, 0.25f, 0.0f,
                          0.25f, 0.75f, 0.0f);
    /* Degenerate subject geometry is skipped honestly. */
    subject[4] = triangle(3.0f, 3.0f, 3.0f, 3.0f, 3.0f, 3.0f,
                          3.0f, 3.0f, 3.0f);
    /* Coplanar and disjoint, but every separating axis belonging to shell[0]
     * overlaps. Only an edge normal belonging to this subject separates it;
     * this prevents a one-sided coplanar SAT implementation. */
    subject[5] = triangle(1.8f, -0.5f, 0.0f, 2.8f, 0.5f, 0.0f,
                          2.8f, -0.5f, 0.0f);
    SubjectFixture fixture = {
        subject, (uint32_t)(sizeof(subject) / sizeof(subject[0])), 0,
    };
    MdkrModernSurfaceIntersectionDiagnostics diagnostics;
    require(mdkr_modern_surface_intersections(
                shell, (uint32_t)(sizeof(shell) / sizeof(shell[0])),
                fixture.count, read_subject, &fixture, &diagnostics),
            "qualified surfaces did not produce a complete measurement");
    require(diagnostics.shell_triangles_submitted == 5u &&
                diagnostics.shell_triangles_tested == 4u &&
                diagnostics.subject_triangles_submitted == 6u &&
                diagnostics.subject_triangles_tested == 5u &&
                diagnostics.crossing_subject_triangles == 2u &&
                diagnostics.crossing_pairs >= 2u &&
                fabsf(diagnostics.first_crossing_subject_center[0] -
                      0.8333333f) < 0.0001f &&
                fabsf(diagnostics.first_crossing_subject_center[1] - 0.5f) <
                    0.0001f &&
                fabsf(diagnostics.first_crossing_subject_center[2]) <
                    0.0001f,
            "crossing counts or the first source-triangle locator are wrong");

    fixture.fail = 1;
    memset(&diagnostics, 0xA5, sizeof(diagnostics));
    require(!mdkr_modern_surface_intersections(
                shell, 5u, fixture.count, read_subject, &fixture,
                &diagnostics) &&
                diagnostics.shell_triangles_submitted == 0u,
            "callback failure did not clear and refuse partial evidence");
    fixture.fail = 0;

    {
        MdkrModernSurfaceTriangle invalid = shell[0];
        invalid.point[0][0] = NAN;
        require(!mdkr_modern_surface_intersections(
                    &invalid, 1u, fixture.count, read_subject, &fixture,
                    &diagnostics),
                "non-finite retained geometry was accepted");
    }
    require(!mdkr_modern_surface_intersections(
                shell, MDKR_MODERN_CHARACTER_SHELL_TRIANGLE_MAX + 1u,
                fixture.count, read_subject, &fixture, &diagnostics),
            "the retained shell safety bound was not enforced");
    require(!mdkr_modern_surface_intersections(
                shell, 5u, 0u, read_subject, &fixture, &diagnostics),
            "an empty subject surface was accepted");

    puts("test_modern_character_surface_intersection: PASS");
    return 0;
}
