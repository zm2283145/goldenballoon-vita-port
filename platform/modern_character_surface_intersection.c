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
#define MDKR_SURFACE_CONTAINMENT_EPSILON_SCALE 1.0e-6
#define MDKR_SURFACE_PI 3.14159265358979323846

typedef struct MdkrSurfaceEdge {
    uint16_t from;
    uint16_t to;
} MdkrSurfaceEdge;

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

static int same_point(const float a[3], const float b[3]) {
    return a[0] == b[0] && a[1] == b[1] && a[2] == b[2];
}

static uint16_t shell_vertex_id(
    const MdkrSurfaceBvh *bvh, uint16_t triangle_index, unsigned point) {
    uint16_t prior_triangle;
    unsigned prior_point;
    uint16_t id = 0u;
    for (prior_triangle = 0u; prior_triangle <= triangle_index;
         ++prior_triangle) {
        const unsigned point_limit = prior_triangle == triangle_index
            ? point : 3u;
        for (prior_point = 0u; prior_point < point_limit; ++prior_point) {
            if (same_point(
                    bvh->triangle[triangle_index].point[point],
                    bvh->triangle[prior_triangle].point[prior_point])) {
                return id;
            }
            ++id;
        }
    }
    return id;
}

static unsigned shared_vertices(
    const MdkrModernSurfaceTriangle *a,
    const MdkrModernSurfaceTriangle *b) {
    unsigned shared = 0u;
    unsigned first;
    unsigned second;
    for (first = 0u; first < 3u; ++first) {
        for (second = 0u; second < 3u; ++second) {
            if (same_point(a->point[first], b->point[second])) {
                ++shared;
                break;
            }
        }
    }
    return shared;
}

static void qualify_shell_topology(
    const MdkrSurfaceBvh *bvh,
    MdkrModernSurfaceIntersectionDiagnostics *out) {
    MdkrSurfaceEdge edges[MDKR_MODERN_CHARACTER_SHELL_TRIANGLE_MAX * 3u];
    const uint32_t edge_count = (uint32_t)bvh->triangle_count * 3u;
    uint32_t edge_index;
    uint16_t triangle_index;
    for (triangle_index = 0u; triangle_index < bvh->triangle_count;
         ++triangle_index) {
        unsigned point;
        for (point = 0u; point < 3u; ++point) {
            edges[(uint32_t)triangle_index * 3u + point].from =
                shell_vertex_id(bvh, triangle_index, point);
            edges[(uint32_t)triangle_index * 3u + point].to =
                shell_vertex_id(bvh, triangle_index, (point + 1u) % 3u);
        }
    }
    for (edge_index = 0u; edge_index < edge_count; ++edge_index) {
        const MdkrSurfaceEdge *edge = &edges[edge_index];
        uint32_t match_count = 0u;
        uint32_t reverse_count = 0u;
        uint32_t candidate_index;
        int first_occurrence = 1;
        for (candidate_index = 0u; candidate_index < edge_index;
             ++candidate_index) {
            const MdkrSurfaceEdge *candidate = &edges[candidate_index];
            if ((candidate->from == edge->from && candidate->to == edge->to) ||
                (candidate->from == edge->to && candidate->to == edge->from)) {
                first_occurrence = 0;
                break;
            }
        }
        if (!first_occurrence) continue;
        for (candidate_index = edge_index; candidate_index < edge_count;
             ++candidate_index) {
            const MdkrSurfaceEdge *candidate = &edges[candidate_index];
            if ((candidate->from == edge->from && candidate->to == edge->to) ||
                (candidate->from == edge->to && candidate->to == edge->from)) {
                ++match_count;
                if (candidate->from == edge->to &&
                    candidate->to == edge->from) ++reverse_count;
            }
        }
        if (match_count == 1u) {
            ++out->shell_boundary_edges;
        } else if (match_count != 2u) {
            ++out->shell_nonmanifold_edges;
        } else if (reverse_count != 1u) {
            ++out->shell_orientation_mismatch_edges;
        }
    }
    for (triangle_index = 0u; triangle_index < bvh->triangle_count;
         ++triangle_index) {
        uint16_t candidate_index;
        for (candidate_index = (uint16_t)(triangle_index + 1u);
             candidate_index < bvh->triangle_count; ++candidate_index) {
            if (shared_vertices(
                    &bvh->triangle[triangle_index],
                    &bvh->triangle[candidate_index]) == 0u &&
                triangles_intersect(
                    &bvh->triangle[triangle_index],
                    &bvh->triangle[candidate_index])) {
                ++out->shell_self_intersection_pairs;
            }
        }
    }
    out->containment_qualified =
        out->shell_triangles_submitted == out->shell_triangles_tested &&
        out->shell_boundary_edges == 0u &&
        out->shell_nonmanifold_edges == 0u &&
        out->shell_orientation_mismatch_edges == 0u &&
        out->shell_self_intersection_pairs == 0u;
}

static double point_triangle_distance_squared(
    const double point[3], const MdkrModernSurfaceTriangle *triangle) {
    double a[3];
    double b[3];
    double c[3];
    double ab[3];
    double ac[3];
    double ap[3];
    double bp[3];
    double cp[3];
    double projection[3];
    double d1;
    double d2;
    double d3;
    double d4;
    double d5;
    double d6;
    unsigned axis;
    for (axis = 0u; axis < 3u; ++axis) {
        a[axis] = triangle->point[0][axis];
        b[axis] = triangle->point[1][axis];
        c[axis] = triangle->point[2][axis];
        ab[axis] = b[axis] - a[axis];
        ac[axis] = c[axis] - a[axis];
        ap[axis] = point[axis] - a[axis];
        bp[axis] = point[axis] - b[axis];
        cp[axis] = point[axis] - c[axis];
    }
#define MDKR_DOT3(x, y) \
    ((x)[0] * (y)[0] + (x)[1] * (y)[1] + (x)[2] * (y)[2])
    d1 = MDKR_DOT3(ab, ap);
    d2 = MDKR_DOT3(ac, ap);
    if (d1 <= 0.0 && d2 <= 0.0) return MDKR_DOT3(ap, ap);
    d3 = MDKR_DOT3(ab, bp);
    d4 = MDKR_DOT3(ac, bp);
    if (d3 >= 0.0 && d4 <= d3) return MDKR_DOT3(bp, bp);
    {
        const double vc = d1 * d4 - d3 * d2;
        if (vc <= 0.0 && d1 >= 0.0 && d3 <= 0.0) {
            const double v = d1 / (d1 - d3);
            for (axis = 0u; axis < 3u; ++axis) {
                projection[axis] = ap[axis] - v * ab[axis];
            }
            return MDKR_DOT3(projection, projection);
        }
    }
    d5 = MDKR_DOT3(ab, cp);
    d6 = MDKR_DOT3(ac, cp);
    if (d6 >= 0.0 && d5 <= d6) return MDKR_DOT3(cp, cp);
    {
        const double vb = d5 * d2 - d1 * d6;
        if (vb <= 0.0 && d2 >= 0.0 && d6 <= 0.0) {
            const double w = d2 / (d2 - d6);
            for (axis = 0u; axis < 3u; ++axis) {
                projection[axis] = ap[axis] - w * ac[axis];
            }
            return MDKR_DOT3(projection, projection);
        }
    }
    {
        const double va = d3 * d6 - d5 * d4;
        if (va <= 0.0 && d4 - d3 >= 0.0 && d5 - d6 >= 0.0) {
            const double denominator =
                (d4 - d3) + (d5 - d6);
            const double w = (d4 - d3) / denominator;
            double bc[3];
            for (axis = 0u; axis < 3u; ++axis) {
                bc[axis] = c[axis] - b[axis];
                projection[axis] = bp[axis] - w * bc[axis];
            }
            return MDKR_DOT3(projection, projection);
        }
    }
    {
        const double denominator = 1.0 /
            ((d1 * d4 - d3 * d2) + (d5 * d2 - d1 * d6) +
             (d3 * d6 - d5 * d4));
        const double v = (d5 * d2 - d1 * d6) * denominator;
        const double w = (d1 * d4 - d3 * d2) * denominator;
        for (axis = 0u; axis < 3u; ++axis) {
            projection[axis] = ap[axis] - v * ab[axis] - w * ac[axis];
        }
    }
#undef MDKR_DOT3
    return length_squared3(projection);
}

static void classify_containment_sample(
    const MdkrSurfaceBvh *bvh, const float sample[3], double epsilon,
    MdkrModernSurfaceIntersectionDiagnostics *out) {
    const double point[3] = {sample[0], sample[1], sample[2]};
    double minimum_distance_squared = DBL_MAX;
    double solid_angle = 0.0;
    uint16_t triangle_index;
    for (triangle_index = 0u; triangle_index < bvh->triangle_count;
         ++triangle_index) {
        const MdkrModernSurfaceTriangle *triangle =
            &bvh->triangle[triangle_index];
        double vector[3][3];
        double cross[3];
        double lengths[3];
        double denominator;
        double numerator;
        unsigned vertex;
        unsigned axis;
        const double distance_squared =
            point_triangle_distance_squared(point, triangle);
        if (distance_squared < minimum_distance_squared) {
            minimum_distance_squared = distance_squared;
        }
        for (vertex = 0u; vertex < 3u; ++vertex) {
            for (axis = 0u; axis < 3u; ++axis) {
                vector[vertex][axis] =
                    (double)triangle->point[vertex][axis] - point[axis];
            }
            lengths[vertex] = sqrt(length_squared3(vector[vertex]));
        }
        cross3(vector[1], vector[2], cross);
        numerator = vector[0][0] * cross[0] +
            vector[0][1] * cross[1] + vector[0][2] * cross[2];
        denominator = lengths[0] * lengths[1] * lengths[2] +
            (vector[0][0] * vector[1][0] +
             vector[0][1] * vector[1][1] +
             vector[0][2] * vector[1][2]) * lengths[2] +
            (vector[1][0] * vector[2][0] +
             vector[1][1] * vector[2][1] +
             vector[1][2] * vector[2][2]) * lengths[0] +
            (vector[2][0] * vector[0][0] +
             vector[2][1] * vector[0][1] +
             vector[2][2] * vector[0][2]) * lengths[1];
        solid_angle += 2.0 * atan2(numerator, denominator);
    }
    ++out->containment_samples_tested;
    if (minimum_distance_squared <= epsilon * epsilon) {
        ++out->containment_boundary_samples;
    } else if (fabs(solid_angle) > 2.0 * MDKR_SURFACE_PI) {
        const float depth = (float)sqrt(minimum_distance_squared);
        ++out->containment_inside_samples;
        if (depth > out->containment_maximum_inside_depth) {
            unsigned axis;
            out->containment_maximum_inside_depth = depth;
            for (axis = 0u; axis < 3u; ++axis) {
                out->containment_deepest_subject_point[axis] = sample[axis];
            }
        }
    } else {
        ++out->containment_outside_samples;
    }
}

int mdkr_modern_surface_intersections(
    const MdkrModernSurfaceTriangle *shell, uint32_t shell_triangles,
    uint32_t subject_triangles, MdkrModernSurfaceTriangleReader subject_reader,
    void *subject_user, MdkrModernSurfaceIntersectionDiagnostics *out) {
    MdkrSurfaceBvh bvh;
    uint32_t shell_index;
    uint32_t subject_index;
    uint32_t containment_sample_ordinal = 0u;
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
    qualify_shell_topology(&bvh, out);
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
        int containment_sample = 0;
        if (!subject_reader(subject_user, subject_index, &subject) ||
            !triangle_finite(&subject)) {
            memset(out, 0, sizeof(*out));
            return 0;
        }
        if (out->containment_qualified) {
            if (subject_triangles <=
                    MDKR_MODERN_CHARACTER_CONTAINMENT_SAMPLE_MAX) {
                containment_sample = 1;
            } else if (containment_sample_ordinal <
                    MDKR_MODERN_CHARACTER_CONTAINMENT_SAMPLE_MAX &&
                subject_index == (uint32_t)(
                    ((uint64_t)containment_sample_ordinal *
                     subject_triangles) /
                    MDKR_MODERN_CHARACTER_CONTAINMENT_SAMPLE_MAX)) {
                containment_sample = 1;
            }
            if (containment_sample) ++containment_sample_ordinal;
        }
        if (!triangle_edges(&subject, edges, normal)) continue;
        ++out->subject_triangles_tested;
        if (containment_sample) {
            float sample[3];
            double diagonal_squared = 0.0;
            unsigned axis;
            for (axis = 0u; axis < 3u; ++axis) {
                const double extent =
                    (double)bvh.node[0].maximum[axis] -
                    bvh.node[0].minimum[axis];
                diagonal_squared += extent * extent;
                sample[axis] = (float)(((double)subject.point[0][axis] +
                    subject.point[1][axis] + subject.point[2][axis]) / 3.0);
            }
            classify_containment_sample(
                &bvh, sample,
                fmax(MDKR_SURFACE_SEPARATION_EPSILON,
                     sqrt(diagonal_squared) *
                         MDKR_SURFACE_CONTAINMENT_EPSILON_SCALE),
                out);
        }
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
    if (out->containment_qualified &&
        (out->containment_samples_tested == 0u ||
         out->containment_samples_tested >
             MDKR_MODERN_CHARACTER_CONTAINMENT_SAMPLE_MAX ||
         out->containment_inside_samples +
                 out->containment_boundary_samples +
                 out->containment_outside_samples !=
             out->containment_samples_tested)) {
        memset(out, 0, sizeof(*out));
        return 0;
    }
    return 1;
}
