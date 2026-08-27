#include "modern_character_surface_intersection.h"

#include <float.h>
#include <math.h>
#include <stddef.h>
#include <string.h>

#define MDKR_SURFACE_BVH_NODES \
    (MDKR_MODERN_CHARACTER_SHELL_TRIANGLE_MAX * 2u - 1u)
#define MDKR_SURFACE_BVH_STACK MDKR_SURFACE_BVH_NODES
#define MDKR_SURFACE_LEAF_TRIANGLES 4u
#define MDKR_SURFACE_AXIS_EPSILON_SQUARED 1.0e-18
#define MDKR_SURFACE_SEPARATION_EPSILON 1.0e-7

typedef struct MdkrSurfaceBvhNode {
    float minimum[3];
    float maximum[3];
    uint16_t first;
    uint16_t count;
    uint16_t left;
    uint16_t right;
} MdkrSurfaceBvhNode;

typedef struct MdkrSurfaceBvh {
    MdkrModernSurfaceTriangle
        triangle[MDKR_MODERN_CHARACTER_SHELL_TRIANGLE_MAX];
    uint16_t order[MDKR_MODERN_CHARACTER_SHELL_TRIANGLE_MAX];
    MdkrSurfaceBvhNode node[MDKR_SURFACE_BVH_NODES];
    uint16_t node_count;
    uint16_t triangle_count;
} MdkrSurfaceBvh;

static int triangle_finite(const MdkrModernSurfaceTriangle *triangle) {
    unsigned point;
    unsigned axis;
    if (triangle == NULL) return 0;
    for (point = 0u; point < 3u; ++point) {
        for (axis = 0u; axis < 3u; ++axis) {
            if (!isfinite(triangle->point[point][axis])) return 0;
        }
    }
    return 1;
}

static void subtract3(const float a[3], const float b[3], double out[3]) {
    unsigned axis;
    for (axis = 0u; axis < 3u; ++axis) {
        out[axis] = (double)a[axis] - b[axis];
    }
}

static void cross3(const double a[3], const double b[3], double out[3]) {
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

static double length_squared3(const double value[3]) {
    return value[0] * value[0] + value[1] * value[1] +
           value[2] * value[2];
}

static int triangle_edges(
    const MdkrModernSurfaceTriangle *triangle, double edges[3][3],
    double normal[3]) {
    subtract3(triangle->point[1], triangle->point[0], edges[0]);
    subtract3(triangle->point[2], triangle->point[1], edges[1]);
    subtract3(triangle->point[0], triangle->point[2], edges[2]);
    {
        double second[3];
        subtract3(triangle->point[2], triangle->point[0], second);
        cross3(edges[0], second, normal);
    }
    return length_squared3(normal) > MDKR_SURFACE_AXIS_EPSILON_SQUARED;
}

static void triangle_bounds(
    const MdkrModernSurfaceTriangle *triangle, float minimum[3],
    float maximum[3]) {
    unsigned axis;
    unsigned point;
    for (axis = 0u; axis < 3u; ++axis) {
        minimum[axis] = maximum[axis] = triangle->point[0][axis];
        for (point = 1u; point < 3u; ++point) {
            if (triangle->point[point][axis] < minimum[axis]) {
                minimum[axis] = triangle->point[point][axis];
            }
            if (triangle->point[point][axis] > maximum[axis]) {
                maximum[axis] = triangle->point[point][axis];
            }
        }
    }
}

static double triangle_centroid_axis(
    const MdkrModernSurfaceTriangle *triangle, unsigned axis) {
    return ((double)triangle->point[0][axis] +
            triangle->point[1][axis] + triangle->point[2][axis]) /
           3.0f;
}

static int axis_separates(
    const MdkrModernSurfaceTriangle *a,
    const MdkrModernSurfaceTriangle *b, const double input_axis[3]) {
    const double length_squared = length_squared3(input_axis);
    double axis[3];
    double minimum_a = DBL_MAX;
    double maximum_a = -DBL_MAX;
    double minimum_b = DBL_MAX;
    double maximum_b = -DBL_MAX;
    unsigned component;
    unsigned point;
    if (length_squared <= MDKR_SURFACE_AXIS_EPSILON_SQUARED) return 0;
    {
        const double inverse_length = 1.0 / sqrt(length_squared);
        for (component = 0u; component < 3u; ++component) {
            axis[component] = input_axis[component] * inverse_length;
        }
    }
    for (point = 0u; point < 3u; ++point) {
        double projection_a = 0.0;
        double projection_b = 0.0;
        for (component = 0u; component < 3u; ++component) {
            projection_a += axis[component] * a->point[point][component];
            projection_b += axis[component] * b->point[point][component];
        }
        if (projection_a < minimum_a) minimum_a = projection_a;
        if (projection_a > maximum_a) maximum_a = projection_a;
        if (projection_b < minimum_b) minimum_b = projection_b;
        if (projection_b > maximum_b) maximum_b = projection_b;
    }
    return maximum_a < minimum_b - MDKR_SURFACE_SEPARATION_EPSILON ||
           maximum_b < minimum_a - MDKR_SURFACE_SEPARATION_EPSILON;
}

/* Separating axes include both face normals, every edge cross-product, and
 * in-plane edge normals. The final family is required for disjoint coplanar
 * triangles, where face normals and edge cross-products alone are silent. */
static int triangles_intersect(
    const MdkrModernSurfaceTriangle *a,
    const MdkrModernSurfaceTriangle *b) {
    double edge_a[3][3];
    double edge_b[3][3];
    double normal_a[3];
    double normal_b[3];
    unsigned first;
    unsigned second;
    if (!triangle_edges(a, edge_a, normal_a) ||
        !triangle_edges(b, edge_b, normal_b)) return 0;
    if (axis_separates(a, b, normal_a) ||
        axis_separates(a, b, normal_b)) return 0;
    for (first = 0u; first < 3u; ++first) {
        double axis[3];
        cross3(normal_a, edge_a[first], axis);
        if (axis_separates(a, b, axis)) return 0;
        cross3(normal_b, edge_b[first], axis);
        if (axis_separates(a, b, axis)) return 0;
        for (second = 0u; second < 3u; ++second) {
            cross3(edge_a[first], edge_b[second], axis);
            if (axis_separates(a, b, axis)) return 0;
        }
    }
    return 1;
}

static void node_bounds(
    const MdkrSurfaceBvh *bvh, uint16_t first, uint16_t count,
    float minimum[3], float maximum[3]) {
    uint16_t offset;
    unsigned axis;
    for (axis = 0u; axis < 3u; ++axis) {
        minimum[axis] = FLT_MAX;
        maximum[axis] = -FLT_MAX;
    }
    for (offset = 0u; offset < count; ++offset) {
        float triangle_minimum[3];
        float triangle_maximum[3];
        triangle_bounds(
            &bvh->triangle[bvh->order[first + offset]],
            triangle_minimum, triangle_maximum);
        for (axis = 0u; axis < 3u; ++axis) {
            if (triangle_minimum[axis] < minimum[axis]) {
                minimum[axis] = triangle_minimum[axis];
            }
            if (triangle_maximum[axis] > maximum[axis]) {
                maximum[axis] = triangle_maximum[axis];
            }
        }
    }
}

static void sort_centroids(
    MdkrSurfaceBvh *bvh, uint16_t first, uint16_t count, unsigned axis) {
    uint16_t offset;
    for (offset = 1u; offset < count; ++offset) {
        const uint16_t value = bvh->order[first + offset];
        const double centroid = triangle_centroid_axis(
            &bvh->triangle[value], axis);
        uint16_t insertion = offset;
        while (insertion > 0u) {
            const uint16_t previous = bvh->order[first + insertion - 1u];
            if (triangle_centroid_axis(
                    &bvh->triangle[previous], axis) <= centroid) break;
            bvh->order[first + insertion] = previous;
            --insertion;
        }
        bvh->order[first + insertion] = value;
    }
}

static uint16_t build_node(
    MdkrSurfaceBvh *bvh, uint16_t first, uint16_t count) {
    const uint16_t node_index = bvh->node_count++;
    MdkrSurfaceBvhNode *node = &bvh->node[node_index];
    unsigned split_axis = 0u;
    unsigned axis;
    node->first = first;
    node->count = count;
    node->left = node->right = UINT16_MAX;
    node_bounds(bvh, first, count, node->minimum, node->maximum);
    if (count <= MDKR_SURFACE_LEAF_TRIANGLES) return node_index;
    for (axis = 1u; axis < 3u; ++axis) {
        if (node->maximum[axis] - node->minimum[axis] >
            node->maximum[split_axis] - node->minimum[split_axis]) {
            split_axis = axis;
        }
    }
    sort_centroids(bvh, first, count, split_axis);
    {
        const uint16_t left_count = count / 2u;
        node->count = 0u;
        node->left = build_node(bvh, first, left_count);
        node->right = build_node(
            bvh, (uint16_t)(first + left_count),
            (uint16_t)(count - left_count));
    }
    return node_index;
}

static int bounds_overlap(
    const float minimum_a[3], const float maximum_a[3],
    const float minimum_b[3], const float maximum_b[3]) {
    unsigned axis;
    for (axis = 0u; axis < 3u; ++axis) {
        if ((double)maximum_a[axis] <
                (double)minimum_b[axis] - MDKR_SURFACE_SEPARATION_EPSILON ||
            (double)maximum_b[axis] <
                (double)minimum_a[axis] - MDKR_SURFACE_SEPARATION_EPSILON) {
            return 0;
        }
    }
    return 1;
}

int mdkr_modern_surface_intersections(
    const MdkrModernSurfaceTriangle *shell, uint32_t shell_triangles,
    uint32_t subject_triangles, MdkrModernSurfaceTriangleReader subject_reader,
    void *subject_user, MdkrModernSurfaceIntersectionDiagnostics *out) {
    MdkrSurfaceBvh bvh;
    uint32_t shell_index;
    uint32_t subject_index;
    if (out == NULL) return 0;
    memset(out, 0, sizeof(*out));
    if (shell == NULL || shell_triangles == 0u ||
        shell_triangles > MDKR_MODERN_CHARACTER_SHELL_TRIANGLE_MAX ||
        subject_triangles == 0u || subject_reader == NULL) return 0;
    memset(&bvh, 0, sizeof(bvh));
    out->shell_triangles_submitted = shell_triangles;
    out->subject_triangles_submitted = subject_triangles;
    for (shell_index = 0u; shell_index < shell_triangles; ++shell_index) {
        double edges[3][3];
        double normal[3];
        if (!triangle_finite(&shell[shell_index])) {
            memset(out, 0, sizeof(*out));
            return 0;
        }
        if (!triangle_edges(&shell[shell_index], edges, normal)) continue;
        bvh.triangle[bvh.triangle_count] = shell[shell_index];
        bvh.order[bvh.triangle_count] = bvh.triangle_count;
        ++bvh.triangle_count;
    }
    if (bvh.triangle_count == 0u) {
        memset(out, 0, sizeof(*out));
        return 0;
    }
    out->shell_triangles_tested = bvh.triangle_count;
    (void)build_node(&bvh, 0u, bvh.triangle_count);
    for (subject_index = 0u; subject_index < subject_triangles;
         ++subject_index) {
        MdkrModernSurfaceTriangle subject;
        double edges[3][3];
        double normal[3];
        float subject_minimum[3];
        float subject_maximum[3];
        uint16_t stack[MDKR_SURFACE_BVH_STACK];
        uint16_t stack_size = 0u;
        uint32_t pairs = 0u;
        if (!subject_reader(subject_user, subject_index, &subject) ||
            !triangle_finite(&subject)) {
            memset(out, 0, sizeof(*out));
            return 0;
        }
        if (!triangle_edges(&subject, edges, normal)) continue;
        ++out->subject_triangles_tested;
        triangle_bounds(&subject, subject_minimum, subject_maximum);
        stack[stack_size++] = 0u;
        while (stack_size != 0u) {
            const MdkrSurfaceBvhNode *node =
                &bvh.node[stack[--stack_size]];
            uint16_t offset;
            if (!bounds_overlap(
                    subject_minimum, subject_maximum,
                    node->minimum, node->maximum)) continue;
            if (node->count == 0u) {
                if (stack_size + 2u > MDKR_SURFACE_BVH_STACK) {
                    memset(out, 0, sizeof(*out));
                    return 0;
                }
                stack[stack_size++] = node->left;
                stack[stack_size++] = node->right;
                continue;
            }
            for (offset = 0u; offset < node->count; ++offset) {
                const MdkrModernSurfaceTriangle *candidate =
                    &bvh.triangle[bvh.order[node->first + offset]];
                float candidate_minimum[3];
                float candidate_maximum[3];
                triangle_bounds(
                    candidate, candidate_minimum, candidate_maximum);
                if (bounds_overlap(
                        subject_minimum, subject_maximum,
                        candidate_minimum, candidate_maximum) &&
                    triangles_intersect(&subject, candidate)) {
                    ++pairs;
                }
            }
        }
        if (pairs != 0u) {
            unsigned axis;
            if (out->crossing_subject_triangles == 0u) {
                for (axis = 0u; axis < 3u; ++axis) {
                    out->first_crossing_subject_center[axis] =
                        (float)(((double)subject.point[0][axis] +
                                 subject.point[1][axis] +
                                 subject.point[2][axis]) /
                                3.0);
                }
            }
            if (out->crossing_subject_triangles == UINT32_MAX) {
                memset(out, 0, sizeof(*out));
                return 0;
            }
            ++out->crossing_subject_triangles;
            if (UINT32_MAX - out->crossing_pairs < pairs) {
                memset(out, 0, sizeof(*out));
                return 0;
            }
            out->crossing_pairs += pairs;
        }
    }
    if (out->subject_triangles_tested == 0u) {
        memset(out, 0, sizeof(*out));
        return 0;
    }
    return 1;
}
