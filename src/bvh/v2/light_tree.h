#ifndef BVH_V2_LIGHT_TREE_H
#define BVH_V2_LIGHT_TREE_H

#include "bvh/v2/bbox.h"
#include "bvh/v2/binned_sah_builder.h"
#include "bvh/v2/bvh.h"
#include "bvh/v2/node.h"
#include "bvh/v2/tri.h"
#include "bvh/v2/utils.h"
#include "bvh/v2/vec.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace bvh::v2 {

/// Area-light sampling tree for three-dimensional, double-precision (or, more generally,
/// floating-point) triangles.
///
/// The tree stores one emissive triangle per original primitive id and reuses the regular BVH
/// builder to partition those triangles spatially. The original face ids are never changed by
/// construction or by `refit`; the usual `Bvh::prim_ids` array maps leaf slots back to them.
///
/// Distribution
/// ------------
/// For a receiver point `P` and a positive distance-softening value `rho`, an inner node with
/// children `c` has the unnormalized choice score
///
///     score(c) = F(c) / (d(P, box(c))^2 + rho^2)
///
/// where `F(c)` is the sum of the face masses in the subtree rooted at `c`, `box(c)` is the
/// closed AABB of that subtree, and `d(P, box)` is the Euclidean shortest distance from `P` to
/// that closed box (zero when `P` is inside). The face mass is
///
///     F(i) = area(triangle_i) * (.2126 * R_i + .7152 * G_i + .0722 * B_i).
///
/// Sampling starts at the root and, at every inner node, chooses a child with probability equal
/// to that child's normalized score. At a leaf it chooses a face with probability
/// `F(i) / F(leaf)`, and then chooses a point uniformly with respect to area on that triangle.
/// Consequently, for a face `i` in leaf `L` and a point `Q` on that face,
///
///     pA(Q) = [product over the root-to-L path of normalized child scores]
///            * [F(i) / F(L)] * [1 / area(i)].
///
/// This is the joint density with respect to area. It is constant over a face (the point density
/// is uniform), but depends on `P`, `rho`, and the chosen face. Branches and faces with zero
/// mass have zero probability and are never selected. If the tree is empty or all radiances are
/// zero, `sample()` returns `std::nullopt`.
///
/// The unnormalized score is also the only definition used by the reverse queries:
/// `selection_probability()` returns the subtree-path product multiplied by the in-leaf face
/// probability, `pdf_area()` divides that by the face area, and `pdf_solid_angle()` converts the
/// area density to solid-angle density with
///
///     pW(Q) = pA(Q) * |Q - P|^2 / |dot(n_i, normalize(P - Q))|,
///
/// where `n_i` is the unit geometric normal of face `i`. `pdf_solid_angle()` returns zero when
/// `P == Q` or when the direction `P - Q` is tangent to the light plane. Back-facing and
/// occluded configurations are deliberately not considered by either density query: the light
/// distribution is defined by the scores above, and a renderer applies its own one-sided and
/// visibility factors.
///
/// Cost
/// ----
/// One `sample()` visits one root-to-leaf path and scans that leaf's faces, so it costs
/// `O(tree height + leaf size)`. The path probability is accumulated during that same walk,
/// so returning the exact joint density does not add another traversal.
/// `selection_probability()`, `pdf_area()`, and `pdf_solid_angle()` locate the leaf through the
/// prepared reverse face index and walk the prepared parent links, so each costs
/// `O(tree height)`. No receiver point ever scans all lights, and no query assumes that the tree
/// is balanced.
///
/// Input, errors, and mutability
/// -----------------------------
/// Construction and `refit` accept triangles and RGB radiances in original face order. They
/// require non-degenerate triangles (strictly positive finite area), finite coordinates, and
/// finite non-negative radiance components. Invalid input throws `std::invalid_argument`; a
/// face id outside the valid range throws `std::out_of_range`. `refit` validates all arguments
/// before changing the object, so a rejected refit leaves the previous valid state usable.
/// `refit` keeps the topology, face ids, and leaf membership fixed, and only replaces positions,
/// radiances, leaf boxes, face areas/masses, and cached ancestor statistics. A different face
/// count is rejected. Turning a light off (zero radiance) and on again is supported.
///
/// `refit` mutates the object and must not run concurrently with any other operation, and the
/// face count/topology cannot be changed. Const queries (including `sample()`) do not mutate any
/// hidden state and may be called concurrently as long as no `refit` is in progress.
template <typename T = double>
class LightTree {
    static_assert(std::is_floating_point_v<T>, "LightTree requires a floating-point scalar type");

public:
    using Scalar = T;
    using Vec3 = Vec<T, 3>;
    using BBox3 = BBox<T, 3>;
    using Tri3 = Tri<T, 3>;
    using Node = bvh::v2::Node<T, 3>;
    using Bvh = bvh::v2::Bvh<Node>;

    /// Builder configuration. The BVH builder may create leaves with more than one face, which
    /// is fine: in-leaf selection is handled explicitly. `min_leaf_size` is the size at or below
    /// which a node is not split (the SAH may still keep larger leaves up to `max_leaf_size`
    /// when splitting is not worthwhile); `max_leaf_size` bounds forced/fallback leaves.
    struct Config {
        size_t min_leaf_size = 1;
        size_t max_leaf_size = 8;
    };

    /// Result of one area-light sample.
    struct Sample {
        /// Original face id, i.e. the index in the arrays passed to the constructor/refit.
        size_t face_id;
        /// Position sampled uniformly with respect to area on the selected triangle.
        Vec3 position;
        /// Unit geometric normal of the selected triangle, oriented by its winding.
        Vec3 normal;
        /// Emitted RGB radiance of the selected face.
        Vec3 radiance;
        /// Joint density with respect to area, including the whole tree path, the in-leaf face
        /// choice, and the uniform area density.
        T pdf_area;
    };

    LightTree() = default;

    /// Builds a light tree from triangles and radiances given in original face order.
    LightTree(
        std::span<const Tri3> triangles,
        std::span<const Vec3> radiance,
        const Config& config = {})
    {
        initialize(triangles, radiance, config);
    }

    /// Convenience factory with the same semantics as the constructor.
    [[nodiscard]] static LightTree build(
        std::span<const Tri3> triangles,
        std::span<const Vec3> radiance,
        const Config& config = {})
    {
        return LightTree(triangles, radiance, config);
    }

    /// Samples one light interaction.
    ///
    /// `P` is the receiver position, `rho` is a strictly positive finite distance-softening
    /// value, and `u_select`, `u_point0`, `u_point1` are finite values in `[0, 1)`. `u_select`
    /// is used (and remapped at every choice) to walk the tree and choose a face in a leaf;
    /// the other two numbers sample a point uniformly by area on the selected triangle.
    ///
    /// Returns `std::nullopt` if the tree is empty or every face has zero mass. Invalid
    /// arguments throw `std::invalid_argument`.
    [[nodiscard]] std::optional<Sample> sample(
        const Vec3& P,
        T rho,
        T u_select,
        T u_point0,
        T u_point1) const
    {
        require_finite_position(P, "P");
        require_rho(rho);
        require_uniform(u_select, "u_select");
        require_uniform(u_point0, "u_point0");
        require_uniform(u_point1, "u_point1");

        if (face_count() == 0 || !(total_mass_ > 0))
            return std::nullopt;

        size_t node_id = 0;
        T selection_probability = 1;
        while (!bvh_.nodes[node_id].is_leaf()) {
            const size_t first_child = bvh_.nodes[node_id].index.first_id();
            const size_t left = first_child;
            const size_t right = first_child + 1;

            // Probabilities are normalized ratios of `F / (d^2 + rho^2)`. They are computed by
            // the same helper used by the reverse queries, so both directions share one
            // definition. Zero-mass children get probability zero and are never selected.
            const T left_probability = normalized_probability(P, rho, left, right);
            const T right_probability = static_cast<T>(1) - left_probability;
            if (u_select < left_probability) {
                selection_probability *= left_probability;
                u_select /= left_probability;
                node_id = left;
            } else {
                selection_probability *= right_probability;
                u_select = (u_select - left_probability) / right_probability;
                node_id = right;
            }
            // Keep the remapped value in [0, 1) in the presence of rounding at a boundary.
            u_select = std::min(u_select, std::nextafter(static_cast<T>(1), static_cast<T>(0)));
        }

        const T leaf_mass = subtree_mass_[node_id];
        if (!(leaf_mass > 0))
            return std::nullopt;

        // Select a face with probability proportional to its mass. Zero-mass faces have no
        // interval and can never be selected. `selected_face` tracks the last positive-mass face
        // so that a value of `u_select` that lands exactly on the top of the CDF is handled.
        const auto first_slot = bvh_.nodes[node_id].index.first_id();
        const auto slot_count = bvh_.nodes[node_id].index.prim_count();
        const T target_mass = u_select * leaf_mass;
        T accumulated_mass = 0;
        size_t selected_face = invalid_id;
        for (size_t k = 0; k < static_cast<size_t>(slot_count); ++k) {
            const size_t face_id = bvh_.prim_ids[first_slot + k];
            const T mass = face_mass_[face_id];
            if (!(mass > 0))
                continue;
            selected_face = face_id;
            accumulated_mass += mass;
            if (target_mass < accumulated_mass)
                break;
        }
        if (selected_face == invalid_id)
            return std::nullopt;

        selection_probability *= face_mass_[selected_face] / leaf_mass;

        // Sample uniformly with respect to area on the selected triangle.
        const T r = std::sqrt(u_point0);
        const T b0 = static_cast<T>(1) - r;
        const T b1 = r * (static_cast<T>(1) - u_point1);
        const T b2 = r * u_point1;
        const Tri3& triangle = triangles_[selected_face];
        const Vec3 position =
            triangle.p0 * b0 + triangle.p1 * b1 + triangle.p2 * b2;

        const T area = areas_[selected_face];
        if (!(selection_probability > 0) || !(area > 0))
            return std::nullopt;

        return Sample {
            selected_face,
            position,
            normals_[selected_face],
            radiance_[selected_face],
            selection_probability / area,
        };
    }

    /// Returns the probability that `sample(P, rho, ...)` selects the given original face,
    /// regardless of the sampled position on it. This is the product of normalized child scores
    /// along the root-to-leaf path and the in-leaf face mass fraction. It is deterministic, does
    /// not draw random numbers, does not depend on any previous sample, and does not apply
    /// back-face or occlusion tests.
    [[nodiscard]] T selection_probability(const Vec3& P, T rho, size_t face_id) const {
        require_finite_position(P, "P");
        require_rho(rho);
        require_face_id(face_id);
        return selection_probability_impl(P, rho, face_id);
    }

    /// Returns the joint area density `pA` for the given face. The density is constant over a
    /// face (the position is sampled uniformly by area), so no position argument is needed.
    /// This is exactly `selection_probability(P, rho, face_id) / area(face_id)`.
    [[nodiscard]] T pdf_area(const Vec3& P, T rho, size_t face_id) const {
        require_finite_position(P, "P");
        require_rho(rho);
        require_face_id(face_id);
        return pdf_area_impl(P, rho, face_id);
    }

    /// Converts the area density of a face into solid-angle density at light point `Q`:
    ///
    ///     pW = pA * |Q - P|^2 / |dot(n, normalize(P - Q))|.
    ///
    /// `Q` is intended to lie on the given face. The function returns zero when `P == Q` or when
    /// the receiver direction is tangent to the light plane. It uses the absolute value of the
    /// cosine, so back-facing receiver points do not zero the area density; the renderer is
    /// responsible for one-sided emission and occlusion. Invalid `P`, `rho`, `face_id`, or a
    /// non-finite `Q` are rejected; no back-face/occlusion rejection is performed.
    [[nodiscard]] T pdf_solid_angle(const Vec3& P, T rho, size_t face_id, const Vec3& Q) const {
        require_finite_position(P, "P");
        require_rho(rho);
        require_face_id(face_id);
        require_finite_position(Q, "Q");

        const T area_density = pdf_area_impl(P, rho, face_id);
        if (!(area_density > 0))
            return 0;

        const Vec3 to_receiver = P - Q;
        const T squared_distance = dot(to_receiver, to_receiver);
        if (!(squared_distance > 0))
            return 0;

        const T distance = std::sqrt(squared_distance);
        const Vec3 direction = to_receiver * (static_cast<T>(1) / distance);
        const T cosine = std::abs(dot(normals_[face_id], direction));
        if (!(cosine > 0))
            return 0;

        return area_density * squared_distance / cosine;
    }

    /// Replaces triangle positions and radiances under the existing topology. Both spans must be
    /// in original face order and have exactly `face_count()` elements. Original face ids,
    /// leaf membership, parent links, and node connections are retained. Leaf boxes, areas,
    /// masses, ancestor boxes, and cached subtree masses are recomputed from the new geometry.
    ///
    /// All input is validated before the object is modified. Invalid input (wrong count,
    /// degenerate triangle, non-finite coordinate, or non-finite/negative radiance) throws
    /// `std::invalid_argument`, and the previous state remains valid and unchanged. Refit is
    /// not thread-safe and must not overlap with queries or other refits.
    void refit(std::span<const Tri3> triangles, std::span<const Vec3> radiance) {
        if (triangles.size() != face_count() || radiance.size() != face_count())
            throw std::invalid_argument("LightTree::refit: face count must not change");

        if (face_count() == 0)
            return;

        std::vector<Tri3> new_triangles(triangles.begin(), triangles.end());
        std::vector<Vec3> new_radiance(radiance.begin(), radiance.end());
        std::vector<Vec3> new_normals(face_count());
        std::vector<T> new_areas(face_count());
        std::vector<T> new_face_masses(face_count());
        T new_total_mass = 0;

        for (size_t i = 0; i < face_count(); ++i) {
            validate_face(
                i, new_triangles[i], new_radiance[i],
                new_areas[i], new_normals[i], new_face_masses[i]);
            new_total_mass += new_face_masses[i];
        }
        if (!std::isfinite(new_total_mass))
            throw std::invalid_argument("LightTree::refit: total face mass is not finite");

        // Commit only after the whole input has been validated.
        triangles_ = std::move(new_triangles);
        radiance_ = std::move(new_radiance);
        normals_ = std::move(new_normals);
        areas_ = std::move(new_areas);
        face_mass_ = std::move(new_face_masses);
        total_mass_ = new_total_mass;
        recompute_boxes_and_masses_();
    }

    [[nodiscard]] size_t face_count() const noexcept { return triangles_.size(); }
    [[nodiscard]] size_t node_count() const noexcept { return bvh_.nodes.size(); }
    [[nodiscard]] bool empty() const noexcept { return triangles_.empty(); }
    [[nodiscard]] T total_mass() const noexcept { return total_mass_; }

    [[nodiscard]] const Bvh& bvh() const noexcept { return bvh_; }

    [[nodiscard]] const Tri3& triangle(size_t face_id) const {
        require_face_id(face_id);
        return triangles_[face_id];
    }

    [[nodiscard]] const Vec3& radiance(size_t face_id) const {
        require_face_id(face_id);
        return radiance_[face_id];
    }

    [[nodiscard]] const Vec3& normal(size_t face_id) const {
        require_face_id(face_id);
        return normals_[face_id];
    }

    [[nodiscard]] T area(size_t face_id) const {
        require_face_id(face_id);
        return areas_[face_id];
    }

    [[nodiscard]] T face_mass(size_t face_id) const {
        require_face_id(face_id);
        return face_mass_[face_id];
    }

private:
    static constexpr size_t invalid_id = std::numeric_limits<size_t>::max();

    [[nodiscard]] static T max_abs_component(const Vec3& value) {
        T maximum = 0;
        for (size_t axis = 0; axis < 3; ++axis)
            maximum = std::max(maximum, std::abs(value[axis]));
        return maximum;
    }

    std::vector<Tri3> triangles_;
    std::vector<Vec3> radiance_;
    std::vector<Vec3> normals_;
    std::vector<T> areas_;
    std::vector<T> face_mass_;

    /// `face_slot_[face_id]` is the slot in `bvh_.prim_ids` that holds the face.
    std::vector<size_t> face_slot_;
    /// `slot_leaf_[slot]` is the leaf node containing that primitive slot.
    std::vector<size_t> slot_leaf_;
    /// `parent_[node_id]` is the parent node id; the root maps to itself.
    std::vector<size_t> parent_;
    /// Cached sum of face masses in the subtree rooted at each node.
    std::vector<T> subtree_mass_;

    Bvh bvh_;
    T total_mass_ = 0;

    static void require_finite_position(const Vec3& value, const char* name) {
        for (size_t i = 0; i < 3; ++i) {
            if (!std::isfinite(value[i]))
                throw std::invalid_argument(std::string("LightTree: ") + name + " is not finite");
        }
    }

    static void require_rho(T rho) {
        if (!(rho > 0) || !std::isfinite(rho))
            throw std::invalid_argument("LightTree: rho must be finite and strictly positive");
    }

    static void require_uniform(T value, const char* name) {
        if (!(value >= 0 && value < 1) || !std::isfinite(value))
            throw std::invalid_argument(std::string("LightTree: ") + name + " must be in [0, 1)");
    }

    void require_face_id(size_t face_id) const {
        if (face_id >= face_count())
            throw std::out_of_range("LightTree: face id out of range");
    }

    static void validate_config(const Config& config) {
        if (config.min_leaf_size == 0)
            throw std::invalid_argument("LightTree: min_leaf_size must be at least one");
        if (config.min_leaf_size > config.max_leaf_size)
            throw std::invalid_argument("LightTree: min_leaf_size must not exceed max_leaf_size");
        if (config.max_leaf_size > Node::Index::max_prim_count)
            throw std::invalid_argument(
                "LightTree: max_leaf_size exceeds the BVH primitive-count limit");
    }

    static void validate_face(
        size_t face_id,
        const Tri3& triangle,
        const Vec3& radiance,
        T& area,
        Vec3& normal,
        T& mass)
    {
        require_finite_position(triangle.p0, "triangle.p0");
        require_finite_position(triangle.p1, "triangle.p1");
        require_finite_position(triangle.p2, "triangle.p2");
        for (size_t c = 0; c < 3; ++c) {
            if (!std::isfinite(radiance[c]) || radiance[c] < 0)
                throw std::invalid_argument(
                    "LightTree: radiance components must be finite and non-negative");
        }

        const Vec3 edge0 = triangle.p1 - triangle.p0;
        const Vec3 edge1 = triangle.p2 - triangle.p0;
        // Scale the edges before taking the cross product so that very small or very large
        // triangles do not underflow/overflow in the intermediate computation.
        const T scale0 = max_abs_component(edge0);
        const T scale1 = max_abs_component(edge1);
        if (!(scale0 > 0) || !(scale1 > 0) || !std::isfinite(scale0) || !std::isfinite(scale1))
            throw std::invalid_argument(
                "LightTree: face " + std::to_string(face_id) + " is degenerate");
        const Vec3 scaled_cross =
            cross(edge0 * (static_cast<T>(1) / scale0), edge1 * (static_cast<T>(1) / scale1));
        const T scaled_cross_length = length(scaled_cross);
        // Multiply the smaller scale first and apply the 1/2 factor early, so that a triangle
        // with one huge and one tiny edge does not overflow an intermediate product even when
        // its actual area is representable.
        const T small_scale = std::min(scale0, scale1);
        const T large_scale = std::max(scale0, scale1);
        area = (scaled_cross_length * small_scale * static_cast<T>(0.5)) * large_scale;
        if (!(area > 0) || !std::isfinite(area))
            throw std::invalid_argument(
                "LightTree: face " + std::to_string(face_id) + " is degenerate or has non-finite area");

        normal = scaled_cross * (static_cast<T>(1) / scaled_cross_length);
        const T luminance =
            static_cast<T>(0.2126) * radiance[0] +
            static_cast<T>(0.7152) * radiance[1] +
            static_cast<T>(0.0722) * radiance[2];
        mass = area * luminance;
        if (!(mass >= 0) || !std::isfinite(mass))
            throw std::invalid_argument(
                "LightTree: face " + std::to_string(face_id) + " has non-finite mass");
    }

    void initialize(
        std::span<const Tri3> triangles,
        std::span<const Vec3> radiance,
        const Config& config)
    {
        validate_config(config);
        if (triangles.size() != radiance.size())
            throw std::invalid_argument(
                "LightTree: triangles and radiance spans must have the same size");

        triangles_.assign(triangles.begin(), triangles.end());
        radiance_.assign(radiance.begin(), radiance.end());
        normals_.resize(face_count());
        areas_.resize(face_count());
        face_mass_.resize(face_count());

        T total_mass = 0;
        for (size_t i = 0; i < face_count(); ++i) {
            validate_face(i, triangles_[i], radiance_[i], areas_[i], normals_[i], face_mass_[i]);
            total_mass += face_mass_[i];
        }
        if (!std::isfinite(total_mass))
            throw std::invalid_argument("LightTree: total face mass is not finite");
        total_mass_ = total_mass;

        if (face_count() == 0)
            return;

        std::vector<BBox3> bboxes(face_count());
        std::vector<Vec3> centers(face_count());
        for (size_t i = 0; i < face_count(); ++i) {
            bboxes[i] = triangles_[i].get_bbox();
            centers[i] = triangles_[i].get_center();
        }

        typename BinnedSahBuilder<Node>::Config builder_config;
        builder_config.min_leaf_size = config.min_leaf_size;
        builder_config.max_leaf_size = config.max_leaf_size;
        bvh_ = BinnedSahBuilder<Node>::build(bboxes, centers, builder_config);

        build_reverse_indices_();
        recompute_subtree_masses_();
    }

    void build_reverse_indices_() {
        const size_t count_faces = face_count();
        const size_t node_count = bvh_.nodes.size();
        parent_.assign(node_count, 0);
        slot_leaf_.assign(count_faces, invalid_id);
        face_slot_.assign(count_faces, invalid_id);
        subtree_mass_.assign(node_count, 0);

        for (size_t slot = 0; slot < count_faces; ++slot) {
            const size_t face_id = bvh_.prim_ids[slot];
            if (face_id >= count_faces)
                throw std::logic_error("LightTree: builder produced an invalid primitive id");
            face_slot_[face_id] = slot;
        }
        for (size_t face_id = 0; face_id < count_faces; ++face_id) {
            if (face_slot_[face_id] == invalid_id)
                throw std::logic_error("LightTree: primitive id is missing from the BVH");
        }

        for (size_t node_id = 0; node_id < node_count; ++node_id) {
            const Node& node = bvh_.nodes[node_id];
            const size_t first = node.index.first_id();
            if (node.is_leaf()) {
                const size_t prim_count = node.index.prim_count();
                if (first + prim_count > count_faces)
                    throw std::logic_error("LightTree: builder produced an out-of-range leaf");
                for (size_t k = 0; k < prim_count; ++k) {
                    if (slot_leaf_[first + k] != invalid_id)
                        throw std::logic_error("LightTree: primitive slot appears in two leaves");
                    slot_leaf_[first + k] = node_id;
                }
            } else {
                if (first + 1 >= node_count)
                    throw std::logic_error("LightTree: builder produced an invalid inner node");
                parent_[first] = node_id;
                parent_[first + 1] = node_id;
            }
        }
    }

    void recompute_subtree_masses_() {
        // `traverse_bottom_up` computes a post-order over the prepared topology, without
        // assuming a particular tree balance.
        bvh_.traverse_bottom_up(
            [&] (Node& node) {
                const size_t node_id =
                    static_cast<size_t>(&node - bvh_.nodes.data());
                T mass = 0;
                const size_t first = node.index.first_id();
                const size_t count = node.index.prim_count();
                for (size_t k = 0; k < count; ++k)
                    mass += face_mass_[bvh_.prim_ids[first + k]];
                subtree_mass_[node_id] = mass;
            },
            [&] (Node& node) {
                const size_t node_id =
                    static_cast<size_t>(&node - bvh_.nodes.data());
                const size_t first = node.index.first_id();
                subtree_mass_[node_id] =
                    subtree_mass_[first] + subtree_mass_[first + 1];
            });
    }

    void recompute_boxes_and_masses_() {
        bvh_.traverse_bottom_up(
            [&] (Node& node) {
                const size_t node_id =
                    static_cast<size_t>(&node - bvh_.nodes.data());
                auto bbox = BBox3::make_empty();
                T mass = 0;
                const size_t first = node.index.first_id();
                const size_t count = node.index.prim_count();
                for (size_t k = 0; k < count; ++k) {
                    const size_t face_id = bvh_.prim_ids[first + k];
                    bbox.extend(triangles_[face_id].get_bbox());
                    mass += face_mass_[face_id];
                }
                node.set_bbox(bbox);
                subtree_mass_[node_id] = mass;
            },
            [&] (Node& node) {
                const size_t node_id =
                    static_cast<size_t>(&node - bvh_.nodes.data());
                const size_t first = node.index.first_id();
                node.set_bbox(
                    bvh_.nodes[first].get_bbox().extend(bvh_.nodes[first + 1].get_bbox()));
                subtree_mass_[node_id] =
                    subtree_mass_[first] + subtree_mass_[first + 1];
            });
    }

    [[nodiscard]] T squared_distance_to_bbox(const Vec3& P, const BBox3& bbox) const {
        T squared_distance = 0;
        for (size_t axis = 0; axis < 3; ++axis) {
            T delta = 0;
            if (P[axis] < bbox.min[axis])
                delta = bbox.min[axis] - P[axis];
            else if (P[axis] > bbox.max[axis])
                delta = P[axis] - bbox.max[axis];
            squared_distance += delta * delta;
        }
        return squared_distance;
    }

    /// Returns log of the Euclidean distance from `P` to the closed box, or -infinity when `P`
    /// is inside. The computation is scaled per axis so that squared distances do not overflow
    /// even for coordinates near the representable range.
    [[nodiscard]] T log_distance_to_bbox(const Vec3& P, const BBox3& bbox) const {
        T deltas[3];
        T largest = 0;
        for (size_t axis = 0; axis < 3; ++axis) {
            T delta = 0;
            if (P[axis] < bbox.min[axis])
                delta = bbox.min[axis] - P[axis];
            else if (P[axis] > bbox.max[axis])
                delta = P[axis] - bbox.max[axis];
            deltas[axis] = delta;
            largest = std::max(largest, std::abs(delta));
        }
        if (!(largest > 0))
            return -std::numeric_limits<T>::infinity();
        if (!std::isfinite(largest))
            return std::numeric_limits<T>::infinity();
        T sum = 0;
        for (T delta : deltas) {
            const T scaled = delta / largest;
            sum += scaled * scaled;
        }
        return std::log(largest) + static_cast<T>(0.5) * std::log(sum);
    }

    /// Returns log(exp(a) + exp(b)) without overflow, for a and b in [-inf, +inf].
    [[nodiscard]] static T log_add_exp(T a, T b) {
        const T maximum = std::max(a, b);
        if (!std::isfinite(maximum))
            return maximum; // both are -infinity (or at least one is +infinity)
        return maximum + std::log(
            std::exp(a - maximum) + std::exp(b - maximum));
    }

    /// Returns log of the unnormalized child score `F / (d^2 + rho^2)`, or -infinity when the
    /// child has zero mass. The denominator is evaluated in log space, which keeps the score
    /// finite (or -infinity) for very large coordinates and for very large or very small `rho`.
    [[nodiscard]] T log_child_score(const Vec3& P, T rho, size_t child_id) const {
        const T mass = subtree_mass_[child_id];
        if (!(mass > 0))
            return -std::numeric_limits<T>::infinity();
        const T log_distance = log_distance_to_bbox(P, bvh_.nodes[child_id].get_bbox());
        const T log_denominator = log_add_exp(
            static_cast<T>(2) * log_distance,
            static_cast<T>(2) * std::log(rho));
        return std::log(mass) - log_denominator;
    }

    /// Returns the normalized probability of choosing `child` over `other` at a node. This is
    /// the single definition shared by forward sampling and the reverse queries. Zero-mass
    /// children get zero probability; the computation uses log-space scores so it remains well
    /// defined when `rho^2` or squared distances would overflow or underflow.
    [[nodiscard]] T normalized_probability(
        const Vec3& P,
        T rho,
        size_t child,
        size_t other) const
    {
        const T child_mass = subtree_mass_[child];
        const T other_mass = subtree_mass_[other];
        if (!(child_mass > 0))
            return static_cast<T>(0);
        if (!(other_mass > 0))
            return static_cast<T>(1);

        // Fast path: the direct formula `F / (d^2 + rho^2)` is exact whenever the squared
        // distances and denominators are representable.
        const T rho_squared = rho * rho;
        const T child_distance =
            squared_distance_to_bbox(P, bvh_.nodes[child].get_bbox());
        const T other_distance =
            squared_distance_to_bbox(P, bvh_.nodes[other].get_bbox());
        const T child_denominator = child_distance + rho_squared;
        const T other_denominator = other_distance + rho_squared;
        if (std::isfinite(child_denominator) && std::isfinite(other_denominator) &&
            child_denominator > 0 && other_denominator > 0)
        {
            const T child_score = child_mass / child_denominator;
            const T other_score = other_mass / other_denominator;
            const T score_sum = child_score + other_score;
            if (std::isfinite(child_score) && std::isfinite(other_score) && score_sum > 0)
                return child_score / score_sum;
        }

        // Robust path for extreme scales: evaluate the same ratio in log space.
        const T child_log_score = log_child_score(P, rho, child);
        const T other_log_score = log_child_score(P, rho, other);
        if (!std::isfinite(child_log_score) || !std::isfinite(other_log_score)) {
            // This can only happen when a distance overflowed. If both distances overflowed,
            // fall back to the mass ratio of the two positive-mass children.
            if (!std::isfinite(child_log_score) && !std::isfinite(other_log_score))
                return child_mass / (child_mass + other_mass);
            return !std::isfinite(child_log_score) ? static_cast<T>(0) : static_cast<T>(1);
        }

        const T difference = other_log_score - child_log_score;
        if (difference >= 0) {
            const T exponential = std::exp(-difference);
            return exponential / (static_cast<T>(1) + exponential);
        }
        const T exponential = std::exp(difference);
        return static_cast<T>(1) / (static_cast<T>(1) + exponential);
    }

    [[nodiscard]] T selection_probability_impl(const Vec3& P, T rho, size_t face_id) const {
        const T mass = face_mass_[face_id];
        if (!(mass > 0))
            return 0;

        const size_t leaf = slot_leaf_[face_slot_[face_id]];

        // Walk from the leaf to the root without allocating or scanning the tree. At each parent,
        // the score of the child on the path is divided by the sum of that parent's two child
        // scores. This is the same normalized choice rule as `sample()`.
        T probability = 1;
        for (size_t child = leaf; child != 0; child = parent_[child]) {
            const size_t ancestor = parent_[child];
            const size_t first = bvh_.nodes[ancestor].index.first_id();
            const size_t other = child == first ? first + 1 : first;
            probability *= normalized_probability(P, rho, child, other);
            if (!(probability > 0))
                return 0;
        }

        const T leaf_mass = subtree_mass_[leaf];
        if (!(leaf_mass > 0))
            return 0;
        return probability * (mass / leaf_mass);
    }

    [[nodiscard]] T pdf_area_impl(const Vec3& P, T rho, size_t face_id) const {
        const T probability = selection_probability_impl(P, rho, face_id);
        if (!(probability > 0))
            return 0;
        return probability / areas_[face_id];
    }
};

} // namespace bvh::v2

#endif
