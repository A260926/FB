// Independent checks for src/bvh/v2/light_tree.h.
//
// The reference values below are computed directly from the public BVH topology, the input
// triangles, and the RGB radiances. They do not call the private helpers of LightTree and do not
// use LightTree's sampling to validate its own density. The direct-lighting comparison at the end
// uses deterministic per-light area quadrature as the reference and a fixed stratified set of
// LightTree samples as the estimator.
#include <bvh/v2/bbox.h>
#include <bvh/v2/bvh.h>
#include <bvh/v2/light_tree.h>
#include <bvh/v2/tri.h>
#include <bvh/v2/vec.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

using Scalar = double;
using Vec3 = bvh::v2::Vec<Scalar, 3>;
using Tri = bvh::v2::Tri<Scalar, 3>;
using BBox = bvh::v2::BBox<Scalar, 3>;
using Node = bvh::v2::Node<Scalar, 3>;
using Bvh = bvh::v2::Bvh<Node>;
using LightTree = bvh::v2::LightTree<Scalar>;

static int failures = 0;

static void check(bool condition, const std::string& message) {
    if (!condition) {
        std::fprintf(stderr, "TARGET_CHECK_FAILED: %s\n", message.c_str());
        ++failures;
    }
}

static bool almost_equal(Scalar a, Scalar b, Scalar relative = 1e-10, Scalar absolute = 1e-13) {
    const Scalar difference = std::abs(a - b);
    const Scalar scale = std::max(std::abs(a), std::abs(b));
    return difference <= absolute || difference <= relative * scale;
}

static Scalar luminance(const Vec3& radiance) {
    return Scalar(0.2126) * radiance[0] + Scalar(0.7152) * radiance[1] + Scalar(0.0722) * radiance[2];
}

struct FaceStats {
    Scalar area = 0;
    Scalar mass = 0;
    Vec3 normal;
};

static FaceStats compute_face_stats(const Tri& triangle, const Vec3& radiance) {
    const Vec3 edge0 = triangle.p1 - triangle.p0;
    const Vec3 edge1 = triangle.p2 - triangle.p0;
    const Vec3 cross_product = bvh::v2::cross(edge0, edge1);
    const Scalar cross_length = bvh::v2::length(cross_product);
    FaceStats stats;
    stats.area = cross_length * Scalar(0.5);
    stats.normal = cross_product * (Scalar(1) / cross_length);
    stats.mass = stats.area * luminance(radiance);
    return stats;
}

static Scalar squared_distance_to_bbox(const Vec3& p, const BBox& bbox) {
    Scalar sum = 0;
    for (size_t axis = 0; axis < 3; ++axis) {
        Scalar delta = 0;
        if (p[axis] < bbox.min[axis])
            delta = bbox.min[axis] - p[axis];
        else if (p[axis] > bbox.max[axis])
            delta = p[axis] - bbox.max[axis];
        sum += delta * delta;
    }
    return sum;
}

// Reference tree view, built from the public topology only.
struct RefTree {
    std::vector<FaceStats> faces;
    std::vector<size_t> parent;
    std::vector<size_t> leaf_of_face;
    std::vector<Scalar> node_mass;
    const Bvh* bvh = nullptr;

    RefTree(
        std::span<const Tri> triangles,
        std::span<const Vec3> radiance,
        const Bvh& input_bvh)
        : bvh(&input_bvh)
    {
        faces.resize(triangles.size());
        for (size_t i = 0; i < triangles.size(); ++i)
            faces[i] = compute_face_stats(triangles[i], radiance[i]);

        const size_t node_count = bvh->nodes.size();
        parent.assign(node_count, 0);
        leaf_of_face.assign(triangles.size(), std::numeric_limits<size_t>::max());
        node_mass.assign(node_count, 0);

        for (size_t node_id = 0; node_id < node_count; ++node_id) {
            const Node& node = bvh->nodes[node_id];
            const size_t first = node.index.first_id();
            if (node.is_leaf()) {
                for (size_t k = 0; k < static_cast<size_t>(node.index.prim_count()); ++k)
                    leaf_of_face[bvh->prim_ids[first + k]] = node_id;
            } else {
                parent[first] = node_id;
                parent[first + 1] = node_id;
            }
        }

        // Post-order over the public node array. Node ids are allocated top-down by every builder
        // in this library, but the recursive form below does not assume any particular balance.
        std::vector<bool> seen(node_count, false);
        std::vector<size_t> stack{0};
        while (!stack.empty()) {
            const size_t node_id = stack.back();
            const Node& node = bvh->nodes[node_id];
            if (node.is_leaf()) {
                Scalar mass = 0;
                const size_t first = node.index.first_id();
                for (size_t k = 0; k < static_cast<size_t>(node.index.prim_count()); ++k)
                    mass += faces[bvh->prim_ids[first + k]].mass;
                node_mass[node_id] = mass;
                seen[node_id] = true;
                stack.pop_back();
                continue;
            }
            const size_t first = node.index.first_id();
            if (!seen[first] || !seen[first + 1]) {
                if (!seen[first]) stack.push_back(first);
                if (!seen[first + 1]) stack.push_back(first + 1);
                continue;
            }
            node_mass[node_id] = node_mass[first] + node_mass[first + 1];
            seen[node_id] = true;
            stack.pop_back();
        }
    }

    Scalar child_score(const Vec3& p, Scalar rho, size_t child) const {
        const Scalar mass = node_mass[child];
        if (!(mass > 0))
            return 0;
        const Scalar denominator =
            squared_distance_to_bbox(p, bvh->nodes[child].get_bbox()) + rho * rho;
        return mass / denominator;
    }

    Scalar probability(const Vec3& p, Scalar rho, size_t face_id) const {
        if (!(faces[face_id].mass > 0))
            return 0;
        const size_t leaf = leaf_of_face[face_id];
        std::vector<size_t> path;
        for (size_t child = leaf; child != 0; child = parent[child])
            path.push_back(child);
        path.push_back(0);
        std::reverse(path.begin(), path.end());

        Scalar result = 1;
        for (size_t i = 1; i < path.size(); ++i) {
            const size_t child = path[i];
            const size_t ancestor = path[i - 1];
            const size_t first = bvh->nodes[ancestor].index.first_id();
            const size_t other = child == first ? first + 1 : first;
            const Scalar score = child_score(p, rho, child);
            const Scalar other_score = child_score(p, rho, other);
            const Scalar sum = score + other_score;
            if (!(sum > 0))
                return 0;
            result *= score / sum;
        }
        const Scalar leaf_mass = node_mass[leaf];
        return result * faces[face_id].mass / leaf_mass;
    }
};

// Segment/occluder test used by the independent direct-lighting reference.
static bool segment_hits_triangle(const Vec3& a, const Vec3& b, const Tri& triangle) {
    const Vec3 direction = b - a;
    const Vec3 edge0 = triangle.p1 - triangle.p0;
    const Vec3 edge1 = triangle.p2 - triangle.p0;
    const Vec3 normal = bvh::v2::cross(edge0, edge1);
    const Vec3 pvec = bvh::v2::cross(direction, edge1);
    const Scalar determinant = bvh::v2::dot(edge0, pvec);
    if (std::abs(determinant) < Scalar(1e-14))
        return false;
    const Scalar inverse_determinant = Scalar(1) / determinant;
    const Vec3 tvec = a - triangle.p0;
    const Scalar u = bvh::v2::dot(tvec, pvec) * inverse_determinant;
    if (!(u >= 0) || u > 1)
        return false;
    const Vec3 qvec = bvh::v2::cross(tvec, edge0);
    const Scalar v = bvh::v2::dot(direction, qvec) * inverse_determinant;
    if (!(v >= 0) || u + v > 1)
        return false;
    const Scalar t = bvh::v2::dot(edge1, qvec) * inverse_determinant;
    (void) normal;
    return t > Scalar(1e-6) && t < Scalar(1) - Scalar(1e-6);
}

static bool occluded(const Vec3& from, const Vec3& to, std::span<const Tri> blockers) {
    for (const auto& blocker : blockers) {
        if (segment_hits_triangle(from, to, blocker))
            return true;
    }
    return false;
}

static Vec3 direct_reference(
    const Vec3& receiver,
    const Vec3& receiver_normal,
    std::span<const Tri> lights,
    std::span<const Vec3> radiances,
    std::span<const Tri> blockers,
    size_t grid_size)
{
    Vec3 result(Scalar(0));
    for (size_t light = 0; light < lights.size(); ++light) {
        const auto& triangle = lights[light];
        const auto& radiance = radiances[light];
        const FaceStats stats = compute_face_stats(triangle, radiance);
        Vec3 integral(Scalar(0));
        for (size_t i = 0; i < grid_size; ++i) {
            for (size_t j = 0; j < grid_size; ++j) {
                const Scalar u = (static_cast<Scalar>(i) + Scalar(0.5)) / static_cast<Scalar>(grid_size);
                const Scalar v = (static_cast<Scalar>(j) + Scalar(0.5)) / static_cast<Scalar>(grid_size);
                const Scalar r = std::sqrt(u);
                const Scalar b0 = Scalar(1) - r;
                const Scalar b1 = r * (Scalar(1) - v);
                const Scalar b2 = r * v;
                const Vec3 point =
                    triangle.p0 * b0 + triangle.p1 * b1 + triangle.p2 * b2;
                const Vec3 to_light = point - receiver;
                const Scalar distance_squared = bvh::v2::dot(to_light, to_light);
                if (!(distance_squared > 0))
                    continue;
                const Vec3 direction = to_light * (Scalar(1) / std::sqrt(distance_squared));
                const Scalar receiver_cosine = bvh::v2::dot(receiver_normal, direction);
                const Scalar light_cosine = -bvh::v2::dot(stats.normal, direction);
                if (!(receiver_cosine > 0) || !(light_cosine > 0))
                    continue;
                if (occluded(receiver, point, blockers))
                    continue;
                const Scalar geometry =
                    receiver_cosine * light_cosine / distance_squared;
                integral = integral + radiance * (geometry * stats.area /
                    static_cast<Scalar>(grid_size * grid_size));
            }
        }
        result = result + integral;
    }
    return result;
}

static Vec3 direct_stratified(
    const LightTree& light_tree,
    const Vec3& receiver,
    const Vec3& receiver_normal,
    std::span<const Tri> blockers,
    size_t select_strata,
    size_t point_strata)
{
    Vec3 sum(Scalar(0));
    const size_t point_count = point_strata * point_strata;
    const size_t total = select_strata * point_count;
    for (size_t k = 0; k < total; ++k) {
        const size_t select_index = k / point_count;
        const size_t point_index = k % point_count;
        const Scalar u_select =
            (static_cast<Scalar>(select_index) + Scalar(0.5)) / static_cast<Scalar>(select_strata);
        const Scalar u0 =
            (static_cast<Scalar>(point_index % point_strata) + Scalar(0.5)) /
            static_cast<Scalar>(point_strata);
        const Scalar u1 =
            (static_cast<Scalar>(point_index / point_strata) + Scalar(0.5)) /
            static_cast<Scalar>(point_strata);

        const auto sample = light_tree.sample(receiver, Scalar(0.35), u_select, u0, u1);
        if (!sample || !(sample->pdf_area > 0))
            continue;
        const Vec3 to_light = sample->position - receiver;
        const Scalar distance_squared = bvh::v2::dot(to_light, to_light);
        if (!(distance_squared > 0))
            continue;
        const Vec3 direction = to_light * (Scalar(1) / std::sqrt(distance_squared));
        const Scalar receiver_cosine = bvh::v2::dot(receiver_normal, direction);
        const Scalar light_cosine = -bvh::v2::dot(sample->normal, direction);
        if (!(receiver_cosine > 0) || !(light_cosine > 0))
            continue;
        if (occluded(receiver, sample->position, blockers))
            continue;
        const Scalar geometry = receiver_cosine * light_cosine / distance_squared;
        sum = sum + sample->radiance * (geometry / sample->pdf_area);
    }
    return sum * (Scalar(1) / static_cast<Scalar>(total));
}

static std::vector<Tri> make_main_lights() {
    // Deliberately interleaved spatial order: the BVH builder is expected to reorder primitives.
    std::vector<Tri> lights;
    const Scalar xs[7] = { -3.0, 3.0, -2.5, 2.5, -2.0, 2.0, 0.0 };
    for (size_t i = 0; i < 7; ++i) {
        const Scalar x = xs[i];
        const Scalar y = Scalar(0.2) * static_cast<Scalar>(i);
        const Scalar z = Scalar(1.3) + Scalar(0.15) * static_cast<Scalar>(i % 3);
        lights.emplace_back(
            Vec3(x - Scalar(0.45), y - Scalar(0.4), z),
            Vec3(x - Scalar(0.05), y + Scalar(0.35), z),
            Vec3(x + Scalar(0.4), y - Scalar(0.25), z));
    }
    return lights;
}

int main(int argc, char** argv) {
    (void) argv;
    if (argc < 1)
        return 2;

    auto lights = make_main_lights();
    std::vector<Vec3> radiances = {
        Vec3(8.0, 2.0, 1.0),
        Vec3(1.0, 7.0, 2.0),
        Vec3(2.0, 1.0, 6.0),
        Vec3(5.0, 5.0, 0.5),
        Vec3(0.5, 4.0, 4.0),
        Vec3(0.0, 0.0, 0.0), // zero-power face
        Vec3(3.0, 0.2, 3.5),
    };

    LightTree::Config config;
    config.min_leaf_size = 3;
    config.max_leaf_size = 8;
    LightTree light_tree(lights, radiances, config);
    const Bvh& built_bvh = light_tree.bvh();

    check(light_tree.face_count() == lights.size(), "face count");
    check(light_tree.node_count() == built_bvh.nodes.size(), "node count");

    // Cover original-face reordering.
    bool reordered = false;
    for (size_t i = 0; i < built_bvh.prim_ids.size(); ++i) {
        if (built_bvh.prim_ids[i] != i)
            reordered = true;
    }
    check(reordered, "builder reorders original faces");

    // Cover non-single-face leaves.
    size_t largest_leaf = 0;
    for (const auto& node : built_bvh.nodes) {
        if (node.is_leaf())
            largest_leaf = std::max(largest_leaf, static_cast<size_t>(node.index.prim_count()));
    }
    check(largest_leaf >= 2, "at least one leaf contains multiple faces");

    RefTree reference(lights, radiances, built_bvh);

    // Check the mass bookkeeping against the independent face statistics.
    for (size_t face = 0; face < lights.size(); ++face) {
        check(almost_equal(light_tree.area(face), reference.faces[face].area),
            "face area matches reference");
        check(almost_equal(light_tree.face_mass(face), reference.faces[face].mass),
            "face mass matches reference");
        check(almost_equal(bvh::v2::length(light_tree.normal(face)), Scalar(1)),
            "face normal is unit length");
        check(std::abs(bvh::v2::dot(light_tree.normal(face), reference.faces[face].normal) - Scalar(1)) < Scalar(1e-12),
            "face normal orientation matches winding");
    }

    const Vec3 receiver(Scalar(0.25), Scalar(-0.15), Scalar(0));
    const Scalar rho = Scalar(0.7);

    // Compare each face's forward/reverse density against independent path products, and check
    // that the selection probabilities sum to one.
    Scalar probability_sum = 0;
    for (size_t face = 0; face < lights.size(); ++face) {
        const Scalar expected_probability = reference.probability(receiver, rho, face);
        const Scalar actual_probability = light_tree.selection_probability(receiver, rho, face);
        check(almost_equal(actual_probability, expected_probability, 1e-11, 1e-14),
            "selection probability matches independent path product (face " + std::to_string(face) + ")");
        const Scalar expected_area_density = expected_probability / reference.faces[face].area;
        check(almost_equal(light_tree.pdf_area(receiver, rho, face), expected_area_density, 1e-11, 1e-14),
            "area density matches independent path product / area (face " + std::to_string(face) + ")");
        probability_sum += actual_probability;
    }
    check(almost_equal(probability_sum, Scalar(1), 1e-11, 1e-12),
        "selection probabilities over all faces sum to one");

    // Zero-power face must have exactly zero probability and must never be sampled.
    check(light_tree.selection_probability(receiver, rho, 5) == 0,
        "zero-power face has zero selection probability");
    check(light_tree.pdf_area(receiver, rho, 5) == 0,
        "zero-power face has zero area density");
    check(light_tree.pdf_solid_angle(receiver, rho, 5,
        (lights[5].p0 + lights[5].p1 + lights[5].p2) * (Scalar(1) / Scalar(3))) == 0,
        "zero-power face has zero solid-angle density");

    // Forward density, position sampling, and solid-angle conversion. Use a few fixed random-like
    // uniform values, all in [0, 1).
    for (size_t k = 0; k < 64; ++k) {
        const Scalar u_select = (static_cast<Scalar>(k) + Scalar(0.37)) / Scalar(64);
        const Scalar u0 = (static_cast<Scalar>((k * 13) % 64) + Scalar(0.21)) / Scalar(64);
        const Scalar u1 = (static_cast<Scalar>((k * 29) % 64) + Scalar(0.83)) / Scalar(64);
        const auto sample = light_tree.sample(receiver, rho, u_select, u0, u1);
        check(sample.has_value() && sample->face_id != 5,
            "sample never selects the zero-power face");
        if (!sample)
            continue;
        check(sample->face_id < lights.size(), "sample face id is in the original range");
        check(almost_equal(sample->pdf_area, reference.probability(receiver, rho, sample->face_id) / reference.faces[sample->face_id].area, 1e-11, 1e-14),
            "sample joint area density includes tree path and leaf choice");
        check(almost_equal(sample->pdf_area, light_tree.pdf_area(receiver, rho, sample->face_id), 1e-12, 1e-15),
            "sample density agrees with reverse area density");
        check(almost_equal(bvh::v2::length(sample->normal), Scalar(1)),
            "sample normal is unit length");
        check(almost_equal(bvh::v2::dot(sample->radiance, sample->radiance),
            bvh::v2::dot(radiances[sample->face_id], radiances[sample->face_id])),
            "sample radiance matches input");

        // Verify that the returned position is on the selected face.
        const Vec3 edge0 = lights[sample->face_id].p1 - lights[sample->face_id].p0;
        const Vec3 edge1 = lights[sample->face_id].p2 - lights[sample->face_id].p0;
        const Vec3 relative = sample->position - lights[sample->face_id].p0;
        const Scalar on_plane = std::abs(bvh::v2::dot(
            bvh::v2::cross(edge0, edge1), relative));
        const Scalar scale = reference.faces[sample->face_id].area;
        check(on_plane < Scalar(1e-11) * std::max(scale, Scalar(1)), "sample lies on the light plane");

        const Scalar expected_pW = [&] {
            const Vec3 to_receiver = receiver - sample->position;
            const Scalar squared_distance = bvh::v2::dot(to_receiver, to_receiver);
            if (!(squared_distance > 0))
                return Scalar(0);
            const Vec3 direction = to_receiver * (Scalar(1) / std::sqrt(squared_distance));
            const Scalar cosine = std::abs(bvh::v2::dot(reference.faces[sample->face_id].normal, direction));
            if (!(cosine > 0))
                return Scalar(0);
            return sample->pdf_area * squared_distance / cosine;
        }();
        check(almost_equal(light_tree.pdf_solid_angle(receiver, rho, sample->face_id, sample->position),
            expected_pW, 1e-10, 1e-13), "solid-angle density conversion");
    }

    // The density/product must not depend on which path a previous sample took, and the reverse
    // query must not consume randomness. Interleave arbitrary samples with the queries.
    {
        std::array<Scalar, 7> before{};
        for (size_t face = 0; face < lights.size(); ++face)
            before[face] = light_tree.selection_probability(receiver, rho, face);
        for (size_t k = 0; k < 32; ++k) {
            (void) light_tree.sample(receiver, rho,
                (static_cast<Scalar>(k) + Scalar(0.1)) / Scalar(32),
                Scalar(0.9), Scalar(0.4));
        }
        for (size_t face = 0; face < lights.size(); ++face) {
            check(light_tree.selection_probability(receiver, rho, face) == before[face],
                "reverse probability is independent of prior sample paths");
        }
        const auto first = light_tree.sample(receiver, rho, Scalar(0.33), Scalar(0.21), Scalar(0.66));
        const auto second = light_tree.sample(receiver, rho, Scalar(0.33), Scalar(0.21), Scalar(0.66));
        check(first.has_value() && second.has_value() &&
            first->face_id == second->face_id &&
            first->pdf_area == second->pdf_area &&
            first->position[0] == second->position[0] &&
            first->position[1] == second->position[1] &&
            first->position[2] == second->position[2],
            "sampling and density are deterministic functions of the inputs");
    }

    // P inside boxes: the centroid of a light lies inside its leaf AABB, and the root AABB contains
    // the receiver area. The independent and actual probabilities must still agree.
    const Vec3 inside_centroid =
        (lights[0].p0 + lights[0].p1 + lights[0].p2) * (Scalar(1) / Scalar(3));
    const Vec3 inside_root = built_bvh.get_root().get_bbox().get_center();
    for (const Vec3& inside_point : { inside_centroid, inside_root }) {
        Scalar inside_sum = 0;
        for (size_t face = 0; face < lights.size(); ++face) {
            check(almost_equal(
                light_tree.selection_probability(inside_point, rho, face),
                reference.probability(inside_point, rho, face), 1e-11, 1e-14),
                "probability with P inside boxes matches reference");
            inside_sum += light_tree.selection_probability(inside_point, rho, face);
        }
        check(almost_equal(inside_sum, Scalar(1), 1e-11, 1e-12),
            "probability sums to one with P inside boxes");
    }

    // Tangential and zero-distance solid-angle queries return zero.
    {
        const size_t face = 3;
        const Vec3 point = (lights[face].p0 + lights[face].p1 + lights[face].p2) * (Scalar(1) / Scalar(3));
        // P == Q.
        check(light_tree.pdf_solid_angle(point, rho, face, point) == 0,
            "solid-angle density is zero at zero distance");
        // P in the light plane, not equal to Q: the direction is tangential.
        const Vec3 tangent_point = point + (lights[face].p1 - lights[face].p0);
        check(light_tree.pdf_solid_angle(tangent_point, rho, face, point) == 0,
            "solid-angle density is zero for a tangential direction");
    }

    // Back-face and occlusion must not change the area density / selection probability.
    {
        const Vec3 behind = receiver - Vec3(0, 0, Scalar(2));
        for (size_t face = 0; face < lights.size(); ++face) {
            check(almost_equal(
                light_tree.selection_probability(behind, rho, face),
                reference.probability(behind, rho, face), 1e-11, 1e-14),
                "back-side receiver still uses the same area distribution");
        }
    }

    // Refit: same topology and face ids, new geometry/radiances.
    {
        std::vector<size_t> ids_before = built_bvh.prim_ids;
        auto moved = lights;
        auto new_radiances = radiances;
        moved[0] = Tri(Vec3(-3.2, -0.6, 1.7), Vec3(-2.6, 0.2, 1.7), Vec3(-2.9, -0.5, 2.1));
        moved[3] = Tri(Vec3(2.2, -0.6, 1.2), Vec3(2.9, 0.3, 1.2), Vec3(2.5, 0.1, 1.9));
        new_radiances[1] = Vec3(0.2, 9.0, 0.2);
        new_radiances[3] = Vec3(0.0, 0.0, 0.0);
        new_radiances[5] = Vec3(4.0, 4.0, 4.0);

        light_tree.refit(moved, new_radiances);
        check(built_bvh.prim_ids == ids_before, "refit keeps primitive-slot to face-id mapping");
        check(light_tree.face_count() == moved.size(), "refit keeps face count");

        RefTree refit_reference(moved, new_radiances, built_bvh);
        const Vec3 refit_receiver(Scalar(-0.4), Scalar(0.1), Scalar(0));
        Scalar refit_sum = 0;
        for (size_t face = 0; face < moved.size(); ++face) {
            check(almost_equal(light_tree.area(face), refit_reference.faces[face].area),
                "refit updates area");
            check(almost_equal(light_tree.face_mass(face), refit_reference.faces[face].mass),
                "refit updates face mass");
            check(almost_equal(light_tree.selection_probability(refit_receiver, rho, face),
                refit_reference.probability(refit_receiver, rho, face), 1e-11, 1e-14),
                "refit updates path probabilities");
            refit_sum += light_tree.selection_probability(refit_receiver, rho, face);
        }
        check(almost_equal(refit_sum, Scalar(1), 1e-11, 1e-12), "refit probabilities sum to one");

        // The formerly zero face 3 is now off and face 5 is on.
        check(light_tree.selection_probability(refit_receiver, rho, 3) == 0,
            "refit can turn a light off");
        check(light_tree.selection_probability(refit_receiver, rho, 5) > 0,
            "refit can turn a light on");

        // All-off input returns no sample.
        std::vector<Vec3> all_off(moved.size());
        light_tree.refit(moved, all_off);
        check(!light_tree.sample(refit_receiver, rho, Scalar(0.3), Scalar(0.2), Scalar(0.7)).has_value(),
            "all-off refit returns no sample");
        check(light_tree.total_mass() == 0, "all-off refit has zero total mass");

        // Turn everything back on and check that sampling is usable again.
        light_tree.refit(moved, new_radiances);
        check(light_tree.sample(refit_receiver, rho, Scalar(0.3), Scalar(0.2), Scalar(0.7)).has_value(),
            "re-enabling lights after an all-off refit works");

        // An invalid refit is rejected without corrupting the previous valid state.
        auto bad = new_radiances;
        bad[2] = Vec3(-1.0, 0.0, 0.0);
        bool threw = false;
        try {
            light_tree.refit(moved, bad);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        check(threw, "negative radiance refit is rejected");
        check(light_tree.face_count() == moved.size(), "rejected refit keeps face count");
        check(almost_equal(light_tree.total_mass(), [&] {
            Scalar sum = 0;
            for (size_t face = 0; face < moved.size(); ++face)
                sum += compute_face_stats(moved[face], new_radiances[face]).mass;
            return sum;
        }()), "rejected refit keeps previous mass");

        // A degenerate replacement is also rejected, and the prior state still samples.
        auto degenerate = moved;
        degenerate[4] = Tri(Vec3(0, 0, 0), Vec3(1, 0, 0), Vec3(2, 0, 0));
        threw = false;
        try {
            light_tree.refit(degenerate, new_radiances);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        check(threw, "degenerate triangle refit is rejected");
        check(light_tree.sample(refit_receiver, rho, Scalar(0.3), Scalar(0.2), Scalar(0.7)).has_value(),
            "rejected refit leaves a usable object");

        // Refit must keep the same topology: node ids, child links, and leaf slot ranges.
        const auto topology_before = [&] {
            std::vector<std::array<size_t, 2>> result;
            for (const auto& node : built_bvh.nodes) {
                if (node.is_leaf())
                    result.push_back({node.index.first_id(), node.index.prim_count()});
                else
                    result.push_back({node.index.first_id(), 0});
            }
            return result;
        }();
        light_tree.refit(lights, radiances);
        const auto topology_after = [&] {
            std::vector<std::array<size_t, 2>> result;
            for (const auto& node : built_bvh.nodes) {
                if (node.is_leaf())
                    result.push_back({node.index.first_id(), node.index.prim_count()});
                else
                    result.push_back({node.index.first_id(), 0});
            }
            return result;
        }();
        check(topology_before == topology_after, "refit keeps node topology and leaf ranges");
        check(built_bvh.nodes.size() == topology_after.size(),
            "refit does not allocate or remove nodes");
    }

    // Single-face tree: the root is also the only leaf. The joint density must be 1/area and the
    // selection probability must be exactly one.
    {
        std::vector<Tri> one{ Tri(Vec3(-0.5, -0.4, 1), Vec3(0.6, -0.2, 1), Vec3(0.0, 0.7, 1)) };
        std::vector<Vec3> one_radiance{ Vec3(2.0, 3.0, 1.0) };
        LightTree single(one, one_radiance);
        const Bvh& single_bvh = single.bvh();
        check(single_bvh.nodes.size() == 1, "single-face tree has one node");
        check(single_bvh.get_root().is_leaf(), "single-face tree root is a leaf");
        const Vec3 single_receiver(Scalar(0.1), Scalar(0.2), Scalar(0));
        const Scalar single_area = compute_face_stats(one[0], one_radiance[0]).area;
        check(single.selection_probability(single_receiver, rho, 0) == 1,
            "single-face selection probability is one");
        check(almost_equal(single.pdf_area(single_receiver, rho, 0), Scalar(1) / single_area),
            "single-face joint density is one over area");
        for (size_t k = 0; k < 64; ++k) {
            const auto sample = single.sample(single_receiver, rho,
                (static_cast<Scalar>(k) + Scalar(0.5)) / Scalar(64),
                (static_cast<Scalar>(k * 7 % 64) + Scalar(0.5)) / Scalar(64),
                (static_cast<Scalar>(k * 11 % 64) + Scalar(0.5)) / Scalar(64));
            check(sample.has_value() && sample->face_id == 0,
                "single-face tree always selects face zero");
            check(sample.has_value() &&
                almost_equal(sample->pdf_area, Scalar(1) / single_area),
                "single-face sample density is one over area");
        }
    }

    // Empty input returns no sample and accepts queries only for valid (zero) ranges.
    {
        LightTree empty_tree(std::span<const Tri>{}, std::span<const Vec3>{});
        check(empty_tree.empty(), "empty light tree is empty");
        check(!empty_tree.sample(receiver, rho, Scalar(0.5), Scalar(0.5), Scalar(0.5)).has_value(),
            "empty light tree returns no sample");
    }

    // ---------------------------------------------------------------------------------------
    // A branch whose whole subtree has zero power. The two clusters are far apart so that the
    // builder separates them; the right cluster is entirely black. Sampling must never enter
    // that branch, and the positive probabilities must renormalize to one.
    // ---------------------------------------------------------------------------------------
    {
        std::vector<Tri> branch_lights;
        std::vector<Vec3> branch_radiances;
        for (size_t i = 0; i < 8; ++i) {
            const Scalar x = i < 4 ? Scalar(-40) + Scalar(i) : Scalar(40) + Scalar(i - 4);
            branch_lights.emplace_back(
                Vec3(x, 0, 1), Vec3(x + Scalar(0.8), 0, 1), Vec3(x, Scalar(0.7), 1));
            branch_radiances.emplace_back(
                i < 4 ? Vec3(Scalar(1) + Scalar(i), Scalar(2), Scalar(0.5))
                      : Vec3(Scalar(0)));
        }
        LightTree::Config branch_config;
        branch_config.min_leaf_size = 1;
        branch_config.max_leaf_size = 2;
        LightTree branch_tree(branch_lights, branch_radiances, branch_config);
        RefTree branch_reference(branch_lights, branch_radiances, branch_tree.bvh());

        bool found_zero_node = false;
        for (Scalar node_mass : branch_reference.node_mass) {
            if (!(node_mass > 0))
                found_zero_node = true;
        }
        check(found_zero_node, "scene contains a zero-mass tree branch");

        const Vec3 branch_receiver(Scalar(0.3), Scalar(-0.2), Scalar(0));
        Scalar branch_sum = 0;
        for (size_t face = 0; face < branch_lights.size(); ++face) {
            const Scalar actual = branch_tree.selection_probability(branch_receiver, rho, face);
            check(almost_equal(actual, branch_reference.probability(branch_receiver, rho, face), 1e-11, 1e-14),
                "zero-branch scene probability matches independent reference");
            if (face >= 4)
                check(actual == 0, "face in an all-zero branch has zero probability");
            branch_sum += actual;
        }
        check(almost_equal(branch_sum, Scalar(1), 1e-11, 1e-12),
            "positive branches renormalize over the zero-mass branch");
        for (size_t k = 0; k < 512; ++k) {
            const Scalar u_select = (static_cast<Scalar>(k) + Scalar(0.5)) / Scalar(512);
            const auto sample = branch_tree.sample(
                branch_receiver, rho, u_select, Scalar(0.37), Scalar(0.61));
            check(sample.has_value() && sample->face_id < 4,
                "sampling never selects a face from the all-zero branch");
        }

        // Turning the positive cluster off leaves the other cluster as the only positive branch.
        auto flipped_radiances = branch_radiances;
        for (size_t face = 0; face < 4; ++face)
            flipped_radiances[face] = Vec3(0);
        for (size_t face = 4; face < 8; ++face)
            flipped_radiances[face] = Vec3(Scalar(2), Scalar(1), Scalar(3));
        branch_tree.refit(branch_lights, flipped_radiances);
        check(branch_tree.selection_probability(branch_receiver, rho, 0) == 0,
            "refit can move all power out of a branch");
        Scalar flipped_sum = 0;
        for (size_t face = 0; face < 8; ++face)
            flipped_sum += branch_tree.selection_probability(branch_receiver, rho, face);
        check(almost_equal(flipped_sum, Scalar(1), 1e-11, 1e-12),
            "refit renormalizes after a branch loses all power");
        check(branch_tree.sample(branch_receiver, rho, Scalar(0.1), Scalar(0.2), Scalar(0.3))->face_id >= 4,
            "sampling follows the refitted positive branch");
    }

    // ---------------------------------------------------------------------------------------
    // Invalid inputs and arguments are rejected explicitly.
    // ---------------------------------------------------------------------------------------
    {
        bool threw = false;
        try {
            LightTree bad(std::span<const Tri>(lights.data(), 3), std::span<const Vec3>(radiances.data(), 2));
        } catch (const std::invalid_argument&) { threw = true; }
        check(threw, "mismatched constructor spans are rejected");

        auto degenerate = lights;
        degenerate[1] = Tri(Vec3(0, 0, 0), Vec3(1, 0, 0), Vec3(2, 0, 0));
        threw = false;
        try { LightTree bad(degenerate, radiances); } catch (const std::invalid_argument&) { threw = true; }
        check(threw, "degenerate constructor triangle is rejected");

        auto nan_triangle = lights;
        nan_triangle[1].p0[1] = std::numeric_limits<Scalar>::quiet_NaN();
        threw = false;
        try { LightTree bad(nan_triangle, radiances); } catch (const std::invalid_argument&) { threw = true; }
        check(threw, "non-finite triangle coordinate is rejected");

        auto negative = radiances;
        negative[0][2] = Scalar(-0.5);
        threw = false;
        try { LightTree bad(lights, negative); } catch (const std::invalid_argument&) { threw = true; }
        check(threw, "negative constructor radiance is rejected");

        auto infinite = radiances;
        infinite[0][0] = std::numeric_limits<Scalar>::infinity();
        threw = false;
        try { LightTree bad(lights, infinite); } catch (const std::invalid_argument&) { threw = true; }
        check(threw, "non-finite constructor radiance is rejected");

        LightTree::Config bad_config;
        bad_config.min_leaf_size = 0;
        threw = false;
        try { LightTree bad(lights, radiances, bad_config); } catch (const std::invalid_argument&) { threw = true; }
        check(threw, "zero min_leaf_size is rejected");

        bad_config = LightTree::Config{};
        bad_config.min_leaf_size = 9;
        bad_config.max_leaf_size = 2;
        threw = false;
        try { LightTree bad(lights, radiances, bad_config); } catch (const std::invalid_argument&) { threw = true; }
        check(threw, "min_leaf_size > max_leaf_size is rejected");

        threw = false;
        try { LightTree bad(std::span<const Tri>(lights.data(), 1), std::span<const Vec3>(radiances.data(), 1), [] {
            LightTree::Config c; c.max_leaf_size = Node::Index::max_prim_count + 1; return c; }());
        } catch (const std::invalid_argument&) { threw = true; }
        check(threw, "oversized max_leaf_size is rejected");

        // Query argument validation.
        threw = false;
        try { (void) light_tree.sample(receiver, Scalar(0), Scalar(0.5), Scalar(0.5), Scalar(0.5)); }
        catch (const std::invalid_argument&) { threw = true; }
        check(threw, "zero rho is rejected");

        threw = false;
        try { (void) light_tree.sample(receiver, std::numeric_limits<Scalar>::quiet_NaN(), Scalar(0.5), Scalar(0.5), Scalar(0.5)); }
        catch (const std::invalid_argument&) { threw = true; }
        check(threw, "NaN rho is rejected");

        threw = false;
        try { (void) light_tree.sample(receiver, rho, Scalar(1), Scalar(0.5), Scalar(0.5)); }
        catch (const std::invalid_argument&) { threw = true; }
        check(threw, "u_select equal to one is rejected");

        threw = false;
        try { (void) light_tree.sample(receiver, rho, Scalar(-0.1), Scalar(0.5), Scalar(0.5)); }
        catch (const std::invalid_argument&) { threw = true; }
        check(threw, "negative uniform is rejected");

        const Vec3 nan_point(
            std::numeric_limits<Scalar>::quiet_NaN(), Scalar(0), Scalar(0));
        threw = false;
        try { (void) light_tree.selection_probability(nan_point, rho, 0); }
        catch (const std::invalid_argument&) { threw = true; }
        check(threw, "non-finite receiver position is rejected");

        threw = false;
        try { (void) light_tree.selection_probability(receiver, rho, lights.size()); }
        catch (const std::out_of_range&) { threw = true; }
        check(threw, "out-of-range face id is rejected");

        threw = false;
        try { light_tree.refit(std::span<const Tri>(lights.data(), 2), std::span<const Vec3>(radiances.data(), 2)); }
        catch (const std::invalid_argument&) { threw = true; }
        check(threw, "refit with a different face count is rejected");

        // Rejected queries must not have damaged the object.
        check(almost_equal(
            light_tree.pdf_area(receiver, rho, 1),
            reference.probability(receiver, rho, 1) / reference.faces[1].area, 1e-11, 1e-14),
            "object remains valid after rejected queries");
    }

    // ---------------------------------------------------------------------------------------
    // Direct-lighting image check.
    //
    // The reference is a deterministic per-light area integral (uniform triangle quadrature).
    // The estimator uses fixed stratified LightTree samples and divides by the returned joint
    // pdf. This compares the sampler against geometry, not against the reverse query.
    // ---------------------------------------------------------------------------------------
    {
        std::vector<Tri> image_lights;
        // Four one-sided area lights with different areas and colors, all facing downward.
        image_lights.emplace_back(
            Vec3(-1.30, -0.70, 1.55), Vec3(-0.55, 0.55, 1.55), Vec3(-0.35, -0.65, 1.55));
        image_lights.emplace_back(
            Vec3(0.25, -0.90, 1.20), Vec3(1.15, -0.10, 1.20), Vec3(0.55, 0.65, 1.20));
        image_lights.emplace_back(
            Vec3(-0.75, 0.45, 2.05), Vec3(0.30, 0.30, 2.05), Vec3(-0.10, 1.05, 2.05));
        image_lights.emplace_back(
            Vec3(0.75, 0.25, 1.85), Vec3(1.45, 0.55, 1.85), Vec3(0.95, 1.10, 1.85));
        std::vector<Vec3> image_radiances = {
            Vec3(7.0, 1.8, 1.0),
            Vec3(1.0, 6.0, 2.0),
            Vec3(1.5, 1.5, 5.0),
            Vec3(3.5, 3.0, 0.6),
        };
        std::vector<Tri> blockers;
        // A vertical occluder board between some lights and part of the receiver.
        blockers.emplace_back(
            Vec3(0.05, -0.75, 0.25), Vec3(0.05, 0.75, 0.25), Vec3(0.05, 0.05, 1.00));
        blockers.emplace_back(
            Vec3(0.05, -0.75, 0.25), Vec3(0.05, 0.05, 1.00), Vec3(0.05, -0.75, 1.00));

        LightTree::Config image_config;
        image_config.min_leaf_size = 2;
        image_config.max_leaf_size = 8;
        LightTree image_tree(image_lights, image_radiances, image_config);

        const size_t image_size = 24;
        const size_t reference_grid = 56;
        const size_t select_strata = 32;
        const size_t point_strata = 16;
        const Vec3 receiver_normal(0, 0, 1);

        Vec3 reference_mean(Scalar(0));
        Vec3 estimator_mean(Scalar(0));
        Vec3 squared_error(Scalar(0));
        Scalar reference_total = 0;
        Scalar estimator_total = 0;
        for (size_t py = 0; py < image_size; ++py) {
            for (size_t px = 0; px < image_size; ++px) {
                const Scalar x =
                    (static_cast<Scalar>(px) + Scalar(0.5)) / static_cast<Scalar>(image_size) * Scalar(2.4) - Scalar(1.2);
                const Scalar y =
                    (static_cast<Scalar>(py) + Scalar(0.5)) / static_cast<Scalar>(image_size) * Scalar(2.4) - Scalar(1.2);
                const Vec3 receiver_point(x, y, 0);
                const Vec3 reference_value = direct_reference(
                    receiver_point, receiver_normal, image_lights, image_radiances, blockers, reference_grid);
                const Vec3 estimator_value = direct_stratified(
                    image_tree, receiver_point, receiver_normal, blockers, select_strata, point_strata);
                reference_mean = reference_mean + reference_value;
                estimator_mean = estimator_mean + estimator_value;
                const Vec3 difference = estimator_value - reference_value;
                squared_error = squared_error + difference * difference;
            }
        }
        const Scalar pixel_count = static_cast<Scalar>(image_size * image_size);
        reference_mean = reference_mean * (Scalar(1) / pixel_count);
        estimator_mean = estimator_mean * (Scalar(1) / pixel_count);
        squared_error = squared_error * (Scalar(1) / pixel_count);

        for (size_t channel = 0; channel < 3; ++channel) {
            reference_total += reference_mean[channel];
            estimator_total += estimator_mean[channel];
        }
        const Scalar relative_error = std::abs(estimator_total - reference_total) /
            std::max(reference_total, Scalar(1e-12));
        const Scalar rms = std::sqrt(
            squared_error[0] + squared_error[1] + squared_error[2]);
        std::printf(
            "DIRECT_IMAGE pixels=%zu lighttree_samples_per_pixel=%zu reference_samples_per_light=%zu\n",
            image_size * image_size,
            select_strata * point_strata * point_strata,
            reference_grid * reference_grid);
        std::printf(
            "DIRECT_IMAGE reference_mean_rgb=(%.9g %.9g %.9g) estimator_mean_rgb=(%.9g %.9g %.9g)\n",
            reference_mean[0], reference_mean[1], reference_mean[2],
            estimator_mean[0], estimator_mean[1], estimator_mean[2]);
        std::printf(
            "DIRECT_IMAGE relative_mean_error=%.9g rms_per_pixel=%.9g tolerance=%.3g\n",
            relative_error, rms, Scalar(0.05));
        check(relative_error < Scalar(0.05),
            "stratified LightTree direct image agrees with per-light area quadrature");
    }

    if (failures == 0) {
        std::puts("LIGHT_TREE_TARGET_OK");
        return 0;
    }
    std::fprintf(stderr, "LIGHT_TREE_TARGET_FAILED: %d checks\n", failures);
    return 1;
}
