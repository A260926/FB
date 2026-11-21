#ifndef BVH_V2_OBB_H
#define BVH_V2_OBB_H

// Oriented-bounding-box BVH for 3D triangle meshes.
//
// `OrientedBvh<T>` is built from an existing binary `Bvh<Node<T, 3>>` plus a `Tri<T, 3>` array
// indexed by the *original* primitive identifiers stored in the source tree. The topology (the
// packed `Index` of every node) and the `prim_ids` array are copied verbatim, so node identifiers,
// leaf slots and primitive identities are preserved. Every node stores its own orthonormal frame,
// a center and non-negative half extents. Once the conversion is done the source tree can be
// destroyed: an `OrientedBvh` does not point back into the source.
//
// The frames are fitted from the principal axes of the covariance matrix of all triangle vertices
// contained in a subtree. Degenerate covariance matrices (line-like, planar, coincident vertices)
// fall back to a deterministic orthonormal completion of the frame, so no NaN value is ever
// produced. The initial boxes enclose the actual vertices tightly along their own axes; the only
// outward slack is a small numerical margin bounded by 64 * epsilon * max(1, |coordinate|).

#include "bvh/v2/bbox.h"
#include "bvh/v2/bvh.h"
#include "bvh/v2/index.h"
#include "bvh/v2/node.h"
#include "bvh/v2/ray.h"
#include "bvh/v2/stack.h"
#include "bvh/v2/stream.h"
#include "bvh/v2/tri.h"
#include "bvh/v2/utils.h"
#include "bvh/v2/vec.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

namespace bvh::v2 {

template <typename T>
struct Obb;

namespace obb_detail {

template <typename T>
using Vec3 = Vec<T, 3>;

template <typename T>
BVH_ALWAYS_INLINE bool is_finite(T x) {
    return std::isfinite(static_cast<double>(x));
}

template <typename T>
BVH_ALWAYS_INLINE bool vec_is_finite(const Vec3<T>& v) {
    return is_finite(v[0]) && is_finite(v[1]) && is_finite(v[2]);
}

template <typename T>
BVH_ALWAYS_INLINE bool vec_equal(const Vec3<T>& a, const Vec3<T>& b) {
    return a[0] == b[0] && a[1] == b[1] && a[2] == b[2];
}

template <typename T>
BVH_ALWAYS_INLINE bool normalize_checked(const Vec3<T>& v, Vec3<T>& out) {
    auto len2 = dot(v, v);
    if (!(len2 > static_cast<T>(0)) || !is_finite(len2))
        return false;
    auto len = std::sqrt(len2);
    if (!(len > static_cast<T>(0)) || !is_finite(len))
        return false;
    auto result = v * (static_cast<T>(1) / len);
    if (!vec_is_finite(result))
        return false;
    out = result;
    return true;
}

template <typename T>
inline std::array<Vec3<T>, 3> identity_axes() {
    return {
        Vec3<T>(static_cast<T>(1), static_cast<T>(0), static_cast<T>(0)),
        Vec3<T>(static_cast<T>(0), static_cast<T>(1), static_cast<T>(0)),
        Vec3<T>(static_cast<T>(0), static_cast<T>(0), static_cast<T>(1)),
    };
}

/// Deterministically constructs a unit vector perpendicular to `u` (which must be a unit vector).
template <typename T>
inline Vec3<T> perpendicular_to(const Vec3<T>& u) {
    auto ax = std::fabs(u[0]);
    auto ay = std::fabs(u[1]);
    auto az = std::fabs(u[2]);
    Vec3<T> helper(static_cast<T>(0), static_cast<T>(0), static_cast<T>(0));
    if (ax <= ay && ax <= az)
        helper[0] = static_cast<T>(1);
    else if (ay <= az)
        helper[1] = static_cast<T>(1);
    else
        helper[2] = static_cast<T>(1);

    Vec3<T> result;
    if (normalize_checked(cross(u, helper), result))
        return result;

    // `cross(u, helper)` cannot vanish for a unit `u` and an axis chosen this way, but keep the
    // function total anyway.
    if (helper[0] != static_cast<T>(0))
        helper = Vec3<T>(static_cast<T>(0), static_cast<T>(1), static_cast<T>(0));
    else
        helper = Vec3<T>(static_cast<T>(0), static_cast<T>(0), static_cast<T>(1));
    if (normalize_checked(cross(u, helper), result))
        return result;
    return Vec3<T>(static_cast<T>(0), static_cast<T>(1), static_cast<T>(0));
}

/// Orthonormalizes three preliminary directions in `T` arithmetic, returning `false` if the input
/// does not define a usable frame. The result is always right-handed.
template <typename T>
inline bool build_orthonormal(const Vec3<T> prelim[3], std::array<Vec3<T>, 3>& out) {
    Vec3<T> u0, u1, u2, tmp;
    if (!normalize_checked(prelim[0], u0))
        return false;
    tmp = prelim[1] - u0 * dot(u0, prelim[1]);
    if (!normalize_checked(tmp, u1))
        return false;
    tmp = cross(u0, u1);
    if (!normalize_checked(tmp, u2))
        return false;
    // Exactly right-handed and orthogonal even after rounding.
    tmp = cross(u2, u0);
    if (!normalize_checked(tmp, u1))
        return false;
    out = { u0, u1, u2 };
    return true;
}

/// First and second moments of a vertex set, accumulated relative to a shared reference point.
/// Moments are additive, so a subtree's moments are obtained by adding the moments of its children.
template <typename T>
struct Moments {
    double count = 0;
    double sum[3] = { 0, 0, 0 };
    // xx, xy, xz, yy, yz, zz
    double prod[6] = { 0, 0, 0, 0, 0, 0 };

    void add_point(const Vec3<T>& p, const Vec3<T>& ref) {
        if (!vec_is_finite(p))
            return;
        double d[3] = {
            static_cast<double>(p[0]) - static_cast<double>(ref[0]),
            static_cast<double>(p[1]) - static_cast<double>(ref[1]),
            static_cast<double>(p[2]) - static_cast<double>(ref[2]),
        };
        count += 1.0;
        for (size_t i = 0; i < 3; ++i)
            sum[i] += d[i];
        prod[0] += d[0] * d[0];
        prod[1] += d[0] * d[1];
        prod[2] += d[0] * d[2];
        prod[3] += d[1] * d[1];
        prod[4] += d[1] * d[2];
        prod[5] += d[2] * d[2];
    }

    Moments& add(const Moments& other) {
        count += other.count;
        for (size_t i = 0; i < 3; ++i)
            sum[i] += other.sum[i];
        for (size_t i = 0; i < 6; ++i)
            prod[i] += other.prod[i];
        return *this;
    }
};

/// Jacobi eigenvalue decomposition of a symmetric 3x3 matrix. `a` is destroyed, `v` receives the
/// eigenvectors as columns, and `evals` receives the eigenvalues. Deterministic for a given input.
inline void jacobi3(double a[3][3], double v[3][3], double evals[3]) {
    for (size_t i = 0; i < 3; ++i)
        for (size_t j = 0; j < 3; ++j)
            v[i][j] = (i == j) ? 1.0 : 0.0;

    for (size_t iter = 0; iter < 32; ++iter) {
        auto off = std::fabs(a[0][1]) + std::fabs(a[0][2]) + std::fabs(a[1][2]);
        auto diag = std::fabs(a[0][0]) + std::fabs(a[1][1]) + std::fabs(a[2][2]);
        if (!(off > 1e-15 * (diag + 1.0)))
            break;

        size_t p = 0, q = 1;
        double max_off = std::fabs(a[0][1]);
        if (std::fabs(a[0][2]) > max_off) {
            p = 0; q = 2; max_off = std::fabs(a[0][2]);
        }
        if (std::fabs(a[1][2]) > max_off) {
            p = 1; q = 2; max_off = std::fabs(a[1][2]);
        }
        if (!(max_off > 0.0))
            break;

        auto app = a[p][p];
        auto aqq = a[q][q];
        auto apq = a[p][q];
        auto theta = (aqq - app) / (2.0 * apq);
        double t;
        if (theta >= 0.0)
            t = 1.0 / (theta + std::sqrt(theta * theta + 1.0));
        else
            t = 1.0 / (theta - std::sqrt(theta * theta + 1.0));
        auto c = 1.0 / std::sqrt(t * t + 1.0);
        auto s = t * c;
        auto tau = s / (1.0 + c);

        a[p][p] = app - t * apq;
        a[q][q] = aqq + t * apq;
        a[p][q] = 0.0;
        a[q][p] = 0.0;

        for (size_t k = 0; k < 3; ++k) {
            if (k == p || k == q)
                continue;
            auto akp = a[k][p];
            auto akq = a[k][q];
            a[k][p] = akp - s * (akq + akp * tau);
            a[k][q] = akq + s * (akp - akq * tau);
            a[p][k] = a[k][p];
            a[q][k] = a[k][q];
        }
        for (size_t k = 0; k < 3; ++k) {
            auto vkp = v[k][p];
            auto vkq = v[k][q];
            v[k][p] = vkp - s * (vkq + vkp * tau);
            v[k][q] = vkq + s * (vkp - vkq * tau);
        }
    }

    evals[0] = a[0][0];
    evals[1] = a[1][1];
    evals[2] = a[2][2];
}

/// Computes the deterministic principal frame of a vertex set from its moments. Always returns an
/// orthonormal, right-handed, finite basis.
template <typename T>
inline std::array<Vec3<T>, 3> principal_axes(const Moments<T>& m) {
    auto fallback = identity_axes<T>();
    if (!(m.count > 0.0))
        return fallback;

    double mean[3];
    for (size_t i = 0; i < 3; ++i)
        mean[i] = m.sum[i] / m.count;

    double cov[3][3];
    cov[0][0] = m.prod[0] / m.count - mean[0] * mean[0];
    cov[1][1] = m.prod[3] / m.count - mean[1] * mean[1];
    cov[2][2] = m.prod[5] / m.count - mean[2] * mean[2];
    cov[0][1] = cov[1][0] = m.prod[1] / m.count - mean[0] * mean[1];
    cov[0][2] = cov[2][0] = m.prod[2] / m.count - mean[0] * mean[2];
    cov[1][2] = cov[2][1] = m.prod[4] / m.count - mean[1] * mean[2];

    auto norm = std::fabs(cov[0][0]) + std::fabs(cov[1][1]) + std::fabs(cov[2][2]) +
        2.0 * (std::fabs(cov[0][1]) + std::fabs(cov[0][2]) + std::fabs(cov[1][2]));
    if (!(norm > 0.0) || !std::isfinite(norm))
        return fallback;

    double a[3][3] = {
        { cov[0][0], cov[0][1], cov[0][2] },
        { cov[1][0], cov[1][1], cov[1][2] },
        { cov[2][0], cov[2][1], cov[2][2] },
    };
    double v[3][3];
    double evals[3];
    jacobi3(a, v, evals);
    for (size_t i = 0; i < 3; ++i) {
        if (!std::isfinite(evals[i]))
            return fallback;
    }

    std::array<size_t, 3> order = { 0, 1, 2 };
    std::stable_sort(order.begin(), order.end(), [&] (size_t i, size_t j) {
        return evals[i] > evals[j];
    });

    auto e0 = evals[order[0]];
    auto e1 = evals[order[1]];
    auto max_e = std::max({ std::fabs(evals[0]), std::fabs(evals[1]), std::fabs(evals[2]) });
    if (!(e0 > 0.0) || !(max_e > 0.0))
        return fallback;

    double u0d[3] = { v[0][order[0]], v[1][order[0]], v[2][order[0]] };
    auto len0 = std::sqrt(u0d[0] * u0d[0] + u0d[1] * u0d[1] + u0d[2] * u0d[2]);
    if (!(len0 > 0.0) || !std::isfinite(len0))
        return fallback;
    for (size_t i = 0; i < 3; ++i)
        u0d[i] /= len0;

    auto tol = 1024.0 * std::numeric_limits<double>::epsilon() * max_e;
    double u1d[3];
    bool good_u1 = false;
    if (e1 > tol) {
        u1d[0] = v[0][order[1]];
        u1d[1] = v[1][order[1]];
        u1d[2] = v[2][order[1]];
        auto proj = u1d[0] * u0d[0] + u1d[1] * u0d[1] + u1d[2] * u0d[2];
        for (size_t i = 0; i < 3; ++i)
            u1d[i] -= proj * u0d[i];
        auto len1 = std::sqrt(u1d[0] * u1d[0] + u1d[1] * u1d[1] + u1d[2] * u1d[2]);
        if (len1 > 1e-12 && std::isfinite(len1)) {
            for (size_t i = 0; i < 3; ++i)
                u1d[i] /= len1;
            good_u1 = true;
        }
    }
    if (!good_u1) {
        // Deterministic completion for line-like or repeated-eigenvalue inputs.
        double ax = std::fabs(u0d[0]), ay = std::fabs(u0d[1]), az = std::fabs(u0d[2]);
        double helper[3] = { 0, 0, 0 };
        if (ax <= ay && ax <= az)
            helper[0] = 1.0;
        else if (ay <= az)
            helper[1] = 1.0;
        else
            helper[2] = 1.0;
        u1d[0] = u0d[1] * helper[2] - u0d[2] * helper[1];
        u1d[1] = u0d[2] * helper[0] - u0d[0] * helper[2];
        u1d[2] = u0d[0] * helper[1] - u0d[1] * helper[0];
        auto len1 = std::sqrt(u1d[0] * u1d[0] + u1d[1] * u1d[1] + u1d[2] * u1d[2]);
        if (!(len1 > 0.0) || !std::isfinite(len1))
            return fallback;
        for (size_t i = 0; i < 3; ++i)
            u1d[i] /= len1;
    }

    double u2d[3] = {
        u0d[1] * u1d[2] - u0d[2] * u1d[1],
        u0d[2] * u1d[0] - u0d[0] * u1d[2],
        u0d[0] * u1d[1] - u0d[1] * u1d[0],
    };
    auto len2 = std::sqrt(u2d[0] * u2d[0] + u2d[1] * u2d[1] + u2d[2] * u2d[2]);
    if (!(len2 > 0.0) || !std::isfinite(len2))
        return fallback;
    for (size_t i = 0; i < 3; ++i)
        u2d[i] /= len2;

    Vec3<T> prelim[3] = {
        Vec3<T>(static_cast<T>(u0d[0]), static_cast<T>(u0d[1]), static_cast<T>(u0d[2])),
        Vec3<T>(static_cast<T>(u1d[0]), static_cast<T>(u1d[1]), static_cast<T>(u1d[2])),
        Vec3<T>(static_cast<T>(u2d[0]), static_cast<T>(u2d[1]), static_cast<T>(u2d[2])),
    };
    std::array<Vec3<T>, 3> axes;
    if (!build_orthonormal<T>(prelim, axes))
        return fallback;
    return axes;
}

/// Small outward margin, well below the 64 * epsilon * scale budget required by the interface.
template <typename T>
BVH_ALWAYS_INLINE T bounds_margin(T max_abs_coordinate) {
    auto scale = std::max(static_cast<T>(1), max_abs_coordinate);
    return static_cast<T>(8) * std::numeric_limits<T>::epsilon() * scale;
}

/// Accumulates the tight projection range of a vertex set along a fixed frame, and the largest
/// absolute world coordinate seen (used to size the outward margin).
template <typename T>
struct ExtentAccumulator {
    const std::array<Vec3<T>, 3>& axes;
    Vec3<T> min_proj;
    Vec3<T> max_proj;
    T max_abs = static_cast<T>(0);
    bool any = false;

    explicit ExtentAccumulator(const std::array<Vec3<T>, 3>& axes)
        : axes(axes)
        , min_proj(static_cast<T>(0), static_cast<T>(0), static_cast<T>(0))
        , max_proj(static_cast<T>(0), static_cast<T>(0), static_cast<T>(0))
    {}

    BVH_ALWAYS_INLINE void add(const Vec3<T>& p) {
        if (!vec_is_finite(p))
            return;
        for (size_t i = 0; i < 3; ++i)
            max_abs = std::max(max_abs, std::fabs(p[i]));
        Vec3<T> proj(
            dot(p, axes[0]),
            dot(p, axes[1]),
            dot(p, axes[2]));
        if (!any) {
            min_proj = proj;
            max_proj = proj;
            any = true;
        } else {
            min_proj = robust_min(min_proj, proj);
            max_proj = robust_max(max_proj, proj);
        }
    }
};

template <typename T>
struct ObbBuilder {
    static void finish(
        const std::array<Vec3<T>, 3>& axes,
        const Vec3<T>& min_proj,
        const Vec3<T>& max_proj,
        T max_abs,
        Vec3<T>& center,
        Vec3<T>& half_size)
    {
        auto mid = (min_proj + max_proj) * static_cast<T>(0.5);
        center = axes[0] * mid[0] + axes[1] * mid[1] + axes[2] * mid[2];
        auto margin = bounds_margin(max_abs);
        half_size = Vec3<T>(
            std::max(static_cast<T>(0), (max_proj[0] - min_proj[0]) * static_cast<T>(0.5)) + margin,
            std::max(static_cast<T>(0), (max_proj[1] - min_proj[1]) * static_cast<T>(0.5)) + margin,
            std::max(static_cast<T>(0), (max_proj[2] - min_proj[2]) * static_cast<T>(0.5)) + margin);
    }
};

/// Slab test in the box's local frame. Only the coordinates are changed: the direction is not
/// normalized, which keeps the returned parameters on the same scale as the world-space ray.
template <typename T>
inline std::pair<T, T> intersect_local(const Obb<T>& obb, const Ray<T, 3>& ray);

} // namespace obb_detail

template <typename T>
struct Obb {
    using Vec3 = Vec<T, 3>;

    std::array<Vec3, 3> axis {
        Vec3(static_cast<T>(1), static_cast<T>(0), static_cast<T>(0)),
        Vec3(static_cast<T>(0), static_cast<T>(1), static_cast<T>(0)),
        Vec3(static_cast<T>(0), static_cast<T>(0), static_cast<T>(1)),
    };
    Vec3 center { static_cast<T>(0), static_cast<T>(0), static_cast<T>(0) };
    Vec3 half_size { static_cast<T>(0), static_cast<T>(0), static_cast<T>(0) };

    Obb() = default;
    Obb(const std::array<Vec3, 3>& axis, const Vec3& center, const Vec3& half_size)
        : axis(axis), center(center), half_size(half_size)
    {}

    bool operator == (const Obb& other) const {
        for (size_t i = 0; i < 3; ++i) {
            if (!obb_detail::vec_equal(axis[i], other.axis[i]))
                return false;
        }
        return obb_detail::vec_equal(center, other.center) &&
               obb_detail::vec_equal(half_size, other.half_size);
    }
    bool operator != (const Obb& other) const { return !(*this == other); }

    /// World-space corner selected by the bits of `bits`; bit `i` chooses the positive half extent
    /// along axis `i`.
    [[nodiscard]] BVH_ALWAYS_INLINE Vec3 get_corner(uint32_t bits) const {
        auto corner = center;
        static_for<0, 3>([&] (size_t i) {
            auto positive = (bits >> i) & uint32_t{1};
            auto scale = positive ? half_size[i] : -half_size[i];
            corner = corner + axis[i] * scale;
        });
        return corner;
    }

    /// Fits a box to `points`. The frame is the principal frame of their covariance matrix.
    [[nodiscard]] static Obb fit(std::span<const Vec3> points) {
        auto fallback_axes = obb_detail::identity_axes<T>();
        if (points.empty())
            return Obb(fallback_axes, Vec3(static_cast<T>(0)), Vec3(static_cast<T>(0)));

        Vec3 ref = points[0];
        if (!obb_detail::vec_is_finite(ref))
            ref = Vec3(static_cast<T>(0));

        obb_detail::Moments<T> moments;
        for (const auto& p : points)
            moments.add_point(p, ref);
        auto axes = obb_detail::principal_axes<T>(moments);

        obb_detail::ExtentAccumulator<T> acc(axes);
        for (const auto& p : points)
            acc.add(p);
        if (!acc.any)
            return Obb(axes, Vec3(static_cast<T>(0)), Vec3(static_cast<T>(0)));

        Vec3 center, half_size;
        obb_detail::ObbBuilder<T>::finish(
            axes, acc.min_proj, acc.max_proj, acc.max_abs, center, half_size);
        return Obb(axes, center, half_size);
    }

    /// Fits a box to `points` along the given fixed frame (used by refitting).
    [[nodiscard]] static Obb fit_with_axes(
        std::span<const Vec3> points,
        const std::array<Vec3, 3>& axes)
    {
        obb_detail::ExtentAccumulator<T> acc(axes);
        for (const auto& p : points)
            acc.add(p);
        if (!acc.any)
            return Obb(axes, Vec3(static_cast<T>(0)), Vec3(static_cast<T>(0)));
        Vec3 center, half_size;
        obb_detail::ObbBuilder<T>::finish(
            axes, acc.min_proj, acc.max_proj, acc.max_abs, center, half_size);
        return Obb(axes, center, half_size);
    }

    /// Entry/exit parameters of the ray with this box, in the same units as `ray.dir`. When the
    /// box is missed, the first component is larger than the second.
    [[nodiscard]] BVH_ALWAYS_INLINE std::pair<T, T> intersect(const Ray<T, 3>& ray) const {
        return obb_detail::intersect_local<T>(*this, ray);
    }
};

namespace obb_detail {

template <typename T>
inline std::pair<T, T> intersect_local(const Obb<T>& obb, const Ray<T, 3>& ray) {
    using Vec3 = Vec<T, 3>;
    // Local ray: rotate the origin (relative to the box center) and the direction into the box
    // frame. The direction is deliberately not normalized.
    Vec3 local_org(
        dot(ray.org - obb.center, obb.axis[0]),
        dot(ray.org - obb.center, obb.axis[1]),
        dot(ray.org - obb.center, obb.axis[2]));
    Vec3 local_dir(
        dot(ray.dir, obb.axis[0]),
        dot(ray.dir, obb.axis[1]),
        dot(ray.dir, obb.axis[2]));

    T t0 = ray.tmin;
    T t1 = ray.tmax;
    for (size_t i = 0; i < 3; ++i) {
        auto half = obb.half_size[i];
        auto f = local_dir[i];
        auto e = local_org[i];
        if (f == static_cast<T>(0)) {
            // Parallel to the slab. `-0.0` compares equal to `0.0` here, which is intended.
            if (e < -half || e > half)
                return std::make_pair(
                    std::numeric_limits<T>::infinity(),
                    -std::numeric_limits<T>::infinity());
        } else {
            auto inv = static_cast<T>(1) / f;
            auto ta = (-e - half) * inv;
            auto tb = (-e + half) * inv;
            if (ta > tb)
                std::swap(ta, tb);
            t0 = robust_max(ta, t0);
            t1 = robust_min(tb, t1);
            if (t0 > t1)
                return std::make_pair(t0, t1);
        }
    }
    return std::make_pair(t0, t1);
}

} // namespace obb_detail

/// Binary oriented BVH. The topology and the primitive identifiers match the source `Bvh` exactly.
template <typename T>
class OrientedBvh {
public:
    using Scalar = T;
    using Vec3 = Vec<T, 3>;
    using ObbT = Obb<T>;
    using Index = typename Node<T, 3>::Index;
    using SourceBvh = Bvh<Node<T, 3>>;
    using Triangle = Tri<T, 3>;

    static constexpr size_t dimension = 3;

    /// Per-query traversal counters. A separate instance must be used for every concurrent query;
    /// no counter is stored in the BVH itself.
    struct TraversalStats {
        size_t node_tests = 0;
        size_t leaf_tests = 0;
        size_t primitive_tests = 0;
    };

    struct TraversalEntry {
        Index index {};
        T t_enter = static_cast<T>(0);
    };

    using DefaultStack = GrowingStack<TraversalEntry>;

    struct NodeData {
        std::array<Vec3, 3> axis {
            Vec3(static_cast<T>(1), static_cast<T>(0), static_cast<T>(0)),
            Vec3(static_cast<T>(0), static_cast<T>(1), static_cast<T>(0)),
            Vec3(static_cast<T>(0), static_cast<T>(0), static_cast<T>(1)),
        };
        Vec3 center { static_cast<T>(0), static_cast<T>(0), static_cast<T>(0) };
        Vec3 half_size { static_cast<T>(0), static_cast<T>(0), static_cast<T>(0) };
        Index index {};

        NodeData() = default;

        bool operator == (const NodeData& other) const {
            for (size_t i = 0; i < 3; ++i) {
                if (!obb_detail::vec_equal(axis[i], other.axis[i]))
                    return false;
            }
            return obb_detail::vec_equal(center, other.center) &&
                   obb_detail::vec_equal(half_size, other.half_size) &&
                   index == other.index;
        }
        bool operator != (const NodeData& other) const { return !(*this == other); }

        BVH_ALWAYS_INLINE bool is_leaf() const { return index.is_leaf(); }
        BVH_ALWAYS_INLINE bool is_inner() const { return !index.is_leaf(); }

        BVH_ALWAYS_INLINE ObbT get_obb() const { return ObbT(axis, center, half_size); }

        BVH_ALWAYS_INLINE void set_obb(const ObbT& obb) {
            axis = obb.axis;
            center = obb.center;
            half_size = obb.half_size;
        }

        /// Projects all eight world-space corners of this box onto `frame`, returning the
        /// axis-aligned bounds in that frame.
        BVH_ALWAYS_INLINE std::pair<Vec3, Vec3> project_corners(
            const std::array<Vec3, 3>& frame) const
        {
            auto obb = get_obb();
            auto min_proj = Vec3(
                std::numeric_limits<T>::max(),
                std::numeric_limits<T>::max(),
                std::numeric_limits<T>::max());
            auto max_proj = Vec3(
                -std::numeric_limits<T>::max(),
                -std::numeric_limits<T>::max(),
                -std::numeric_limits<T>::max());
            for (uint32_t bits = 0; bits < 8; ++bits) {
                auto corner = obb.get_corner(bits);
                Vec3 proj(
                    dot(corner, frame[0]),
                    dot(corner, frame[1]),
                    dot(corner, frame[2]));
                min_proj = robust_min(min_proj, proj);
                max_proj = robust_max(max_proj, proj);
            }
            return std::make_pair(min_proj, max_proj);
        }
    };

    OrientedBvh() = default;
    OrientedBvh(const OrientedBvh&) = default;
    OrientedBvh(OrientedBvh&&) = default;
    OrientedBvh& operator = (const OrientedBvh&) = default;
    OrientedBvh& operator = (OrientedBvh&&) = default;

    /// Converts `source` into an oriented tree, fitting every node from the vertices of its
    /// subtree. `triangles` is indexed by the original primitive identifiers in `source.prim_ids`.
    /// The source tree is only read and may be destroyed afterwards.
    [[nodiscard]] static OrientedBvh from_bvh(
        const SourceBvh& source,
        std::span<const Triangle> triangles)
    {
        OrientedBvh result;
        const size_t node_count = source.nodes.size();
        if (node_count == 0)
            return result;

        result.nodes_.resize(node_count);
        result.prim_ids_ = source.prim_ids;
        for (size_t i = 0; i < node_count; ++i)
            result.nodes_[i].index = source.nodes[i].index;

        // Shared reference point for the moment accumulation. Subtracting a nearby point avoids
        // catastrophic cancellation when coordinates are large compared to the spread.
        Vec3 ref(static_cast<T>(0), static_cast<T>(0), static_cast<T>(0));
        auto root_bbox = source.nodes[0].get_bbox();
        bool bbox_ok = true;
        for (size_t i = 0; i < 3; ++i) {
            if (!obb_detail::is_finite(root_bbox.min[i]) ||
                !obb_detail::is_finite(root_bbox.max[i]) ||
                root_bbox.min[i] > root_bbox.max[i])
                bbox_ok = false;
        }
        if (bbox_ok)
            ref = (root_bbox.min + root_bbox.max) * static_cast<T>(0.5);
        if (!obb_detail::vec_is_finite(ref))
            ref = Vec3(static_cast<T>(0), static_cast<T>(0), static_cast<T>(0));

        // Post-order over the source topology. Children are always visited before their parent.
        std::vector<size_t> order;
        order.reserve(node_count);
        std::vector<std::pair<size_t, bool>> stack;
        stack.emplace_back(size_t{0}, false);
        while (!stack.empty()) {
            auto [id, visited] = stack.back();
            stack.pop_back();
            if (id >= node_count)
                continue;
            const auto& node = source.nodes[id];
            if (visited) {
                order.push_back(id);
                continue;
            }
            stack.emplace_back(id, true);
            if (!node.is_leaf()) {
                auto first = static_cast<size_t>(node.index.first_id());
                if (first + 1 < node_count) {
                    stack.emplace_back(first + 1, false);
                    stack.emplace_back(first + 0, false);
                }
            }
        }

        std::vector<obb_detail::Moments<T>> moments(node_count);
        for (auto id : order) {
            const auto& node = source.nodes[id];
            if (node.is_leaf()) {
                auto begin = static_cast<size_t>(node.index.first_id());
                auto end = begin + static_cast<size_t>(node.index.prim_count());
                for (size_t k = begin; k < end; ++k) {
                    auto prim_id = source.prim_ids[k];
                    if (prim_id >= triangles.size())
                        continue;
                    const auto& tri = triangles[prim_id];
                    moments[id].add_point(tri.p0, ref);
                    moments[id].add_point(tri.p1, ref);
                    moments[id].add_point(tri.p2, ref);
                }
            } else {
                auto first = static_cast<size_t>(node.index.first_id());
                if (first + 1 < node_count)
                    moments[id].add(moments[first + 0]).add(moments[first + 1]);
            }
        }

        std::vector<size_t> subtree_stack;
        for (auto id : order) {
            auto axes = obb_detail::principal_axes<T>(moments[id]);
            obb_detail::ExtentAccumulator<T> acc(axes);

            // Walk the subtree once to obtain the tight projection range of its real vertices.
            // The stack is reused across nodes to avoid a heap allocation per node.
            subtree_stack.clear();
            subtree_stack.push_back(id);
            while (!subtree_stack.empty()) {
                auto cur = subtree_stack.back();
                subtree_stack.pop_back();
                if (cur >= node_count)
                    continue;
                const auto& cur_node = source.nodes[cur];
                if (cur_node.is_leaf()) {
                    auto begin = static_cast<size_t>(cur_node.index.first_id());
                    auto end = begin + static_cast<size_t>(cur_node.index.prim_count());
                    for (size_t k = begin; k < end; ++k) {
                        auto prim_id = source.prim_ids[k];
                        if (prim_id >= triangles.size())
                            continue;
                        const auto& tri = triangles[prim_id];
                        acc.add(tri.p0);
                        acc.add(tri.p1);
                        acc.add(tri.p2);
                    }
                } else {
                    auto first = static_cast<size_t>(cur_node.index.first_id());
                    if (first + 1 < node_count) {
                        subtree_stack.push_back(first + 0);
                        subtree_stack.push_back(first + 1);
                    }
                }
            }

            result.nodes_[id].axis = axes;
            if (acc.any) {
                Vec3 center, half_size;
                obb_detail::ObbBuilder<T>::finish(
                    axes, acc.min_proj, acc.max_proj, acc.max_abs, center, half_size);
                result.nodes_[id].center = center;
                result.nodes_[id].half_size = half_size;
            } else {
                result.nodes_[id].center = Vec3(static_cast<T>(0));
                result.nodes_[id].half_size = Vec3(static_cast<T>(0));
            }
        }
        return result;
    }

    /// Reads the node vector. Nodes, their axes and the primitive identifiers are immutable from
    /// the outside; use refit() to update the boxes.
    BVH_ALWAYS_INLINE const std::vector<NodeData>& nodes() const { return nodes_; }
    BVH_ALWAYS_INLINE const std::vector<NodeData>& get_nodes() const { return nodes_; }
    BVH_ALWAYS_INLINE const std::vector<size_t>& prim_ids() const { return prim_ids_; }
    BVH_ALWAYS_INLINE const std::vector<size_t>& get_prim_ids() const { return prim_ids_; }
    BVH_ALWAYS_INLINE size_t node_count() const { return nodes_.size(); }
    BVH_ALWAYS_INLINE size_t prim_count() const { return prim_ids_.size(); }
    BVH_ALWAYS_INLINE bool empty() const { return nodes_.empty(); }
    BVH_ALWAYS_INLINE const NodeData& get_root() const { return nodes_[0]; }

    bool operator == (const OrientedBvh& other) const {
        if (nodes_.size() != other.nodes_.size() || prim_ids_ != other.prim_ids_)
            return false;
        for (size_t i = 0; i < nodes_.size(); ++i) {
            if (!(nodes_[i] == other.nodes_[i]))
                return false;
        }
        return true;
    }
    bool operator != (const OrientedBvh& other) const { return !(*this == other); }

    /// True when the topology and primitive identifiers match `source` exactly.
    bool same_topology(const SourceBvh& source) const {
        if (nodes_.size() != source.nodes.size() || prim_ids_ != source.prim_ids)
            return false;
        for (size_t i = 0; i < nodes_.size(); ++i) {
            if (!(nodes_[i].index == source.nodes[i].index))
                return false;
        }
        return true;
    }

    /// Refits all boxes in place. The topology, leaf slots and every axis are preserved. Leaves
    /// recompute their projection range from the current vertices; inner nodes project the eight
    /// corners of both child OBBs (through the children's own frames) onto their own frame and
    /// reduce the result. World-space axis-aligned boxes are never substituted for child OBBs.
    void refit(std::span<const Triangle> triangles) {
        if (nodes_.empty())
            return;

        // Explicit post-order walk from the root. This does not rely on node identifiers being
        // topologically ordered, which the reinsertion optimizer does not guarantee.
        const size_t node_count = nodes_.size();
        std::vector<std::pair<size_t, bool>> stack;
        stack.emplace_back(size_t{0}, false);
        while (!stack.empty()) {
            auto [id, visited] = stack.back();
            stack.pop_back();
            if (id >= node_count)
                continue;
            auto& node = nodes_[id];
            if (visited) {
                if (node.is_inner())
                    refit_inner(id);
                continue;
            }
            if (node.is_leaf()) {
                refit_leaf(id, triangles);
            } else {
                auto first = static_cast<size_t>(node.index.first_id());
                if (first + 1 >= node_count)
                    continue;
                stack.emplace_back(id, true);
                stack.emplace_back(first + 0, false);
                stack.emplace_back(first + 1, false);
            }
        }
    }

    /// Closest-hit traversal. `leaf_fn(begin, end)` receives a half-open range of slots into
    /// `prim_ids()`; it should shorten the ray's `tmax` when it finds a closer hit. The stack is
    /// caller-owned and must not be shared between concurrent queries.
    template <bool IsAnyHit, typename Stack, typename LeafFn>
    void intersect(
        Ray<T, 3>& ray,
        const Index& start,
        Stack& stack,
        LeafFn&& leaf_fn,
        TraversalStats* stats = nullptr) const
    {
        if (nodes_.empty())
            return;

        stack.push(TraversalEntry { start, ray.tmin });
        while (!stack.is_empty()) {
            auto entry = stack.pop();

            // Re-check the distance window with the latest `ray.tmax` before expanding anything.
            if (entry.t_enter > ray.tmax)
                continue;

            if (entry.index.is_leaf()) {
                auto begin = static_cast<size_t>(entry.index.first_id());
                auto end = begin + static_cast<size_t>(entry.index.prim_count());
                if (begin > prim_ids_.size() || end > prim_ids_.size())
                    continue;
                if (stats) {
                    stats->leaf_tests += 1;
                    stats->primitive_tests += end - begin;
                }
                if constexpr (IsAnyHit) {
                    if (leaf_fn(begin, end))
                        return;
                } else {
                    leaf_fn(begin, end);
                }
                continue;
            }

            auto first = static_cast<size_t>(entry.index.first_id());
            if (first + 1 >= nodes_.size())
                continue;

            const auto& left = nodes_[first + 0];
            const auto& right = nodes_[first + 1];
            auto left_intr = obb_detail::intersect_local<T>(left.get_obb(), ray);
            auto right_intr = obb_detail::intersect_local<T>(right.get_obb(), ray);
            if (stats)
                stats->node_tests += 2;

            auto left_hit = left_intr.first <= left_intr.second && left_intr.first <= ray.tmax;
            auto right_hit = right_intr.first <= right_intr.second && right_intr.first <= ray.tmax;

            if (left_hit && right_hit) {
                // Order children by their own OBB entry parameter. The farther entry is pushed
                // first so that the nearest one is expanded next.
                const NodeData* near_node;
                const NodeData* far_node;
                T near_t, far_t;
                if (left_intr.first <= right_intr.first) {
                    near_node = &left;  near_t = left_intr.first;
                    far_node  = &right; far_t  = right_intr.first;
                } else {
                    near_node = &right; near_t = right_intr.first;
                    far_node  = &left;  far_t  = left_intr.first;
                }
                stack.push(TraversalEntry { far_node->index, far_t });
                stack.push(TraversalEntry { near_node->index, near_t });
            } else if (left_hit) {
                stack.push(TraversalEntry { left.index, left_intr.first });
            } else if (right_hit) {
                stack.push(TraversalEntry { right.index, right_intr.first });
            }
        }
    }

    /// Convenience overload that starts at the root.
    template <bool IsAnyHit, typename Stack, typename LeafFn>
    BVH_ALWAYS_INLINE void intersect(
        Ray<T, 3>& ray,
        Stack& stack,
        LeafFn&& leaf_fn,
        TraversalStats* stats = nullptr) const
    {
        if (nodes_.empty())
            return;
        intersect<IsAnyHit>(ray, nodes_[0].index, stack, std::forward<LeafFn>(leaf_fn), stats);
    }

    template <typename LeafFn>
    BVH_ALWAYS_INLINE void intersect_closest(Ray<T, 3>& ray, LeafFn&& leaf_fn) const {
        DefaultStack stack;
        intersect<false>(ray, stack, std::forward<LeafFn>(leaf_fn));
    }

    template <typename LeafFn>
    BVH_ALWAYS_INLINE void intersect_closest(
        Ray<T, 3>& ray, LeafFn&& leaf_fn, TraversalStats& stats) const
    {
        DefaultStack stack;
        intersect<false>(ray, stack, std::forward<LeafFn>(leaf_fn), &stats);
    }

    template <typename LeafFn>
    BVH_ALWAYS_INLINE void intersect_any(Ray<T, 3>& ray, LeafFn&& leaf_fn) const {
        DefaultStack stack;
        intersect<true>(ray, stack, std::forward<LeafFn>(leaf_fn));
    }

    template <typename LeafFn>
    BVH_ALWAYS_INLINE void intersect_any(
        Ray<T, 3>& ray, LeafFn&& leaf_fn, TraversalStats& stats) const
    {
        DefaultStack stack;
        intersect<true>(ray, stack, std::forward<LeafFn>(leaf_fn), &stats);
    }

    /// Serializes to a format that is intentionally distinct from the AABB `Bvh` archive. All
    /// frames, centers, half extents, topology and primitive identifiers are stored.
    void serialize(OutputStream& stream) const {
        static constexpr uint32_t magic = 0x4F424232u; // "OBB2"
        stream.write(magic);
        stream.write(static_cast<uint32_t>(sizeof(T)));
        stream.write(static_cast<uint32_t>(3));
        stream.write(static_cast<uint64_t>(nodes_.size()));
        stream.write(static_cast<uint64_t>(prim_ids_.size()));
        for (const auto& node : nodes_) {
            for (const auto& axis : node.axis) {
                stream.write(axis[0]);
                stream.write(axis[1]);
                stream.write(axis[2]);
            }
            stream.write(node.center[0]);
            stream.write(node.center[1]);
            stream.write(node.center[2]);
            stream.write(node.half_size[0]);
            stream.write(node.half_size[1]);
            stream.write(node.half_size[2]);
            stream.write(node.index.value);
        }
        for (auto prim_id : prim_ids_)
            stream.write(static_cast<uint64_t>(prim_id));
    }

    [[nodiscard]] static OrientedBvh deserialize(InputStream& stream) {
        static constexpr uint32_t magic = 0x4F424232u;
        if (stream.read<uint32_t>() != magic)
            return OrientedBvh {};
        if (stream.read<uint32_t>() != sizeof(T))
            return OrientedBvh {};
        if (stream.read<uint32_t>() != 3)
            return OrientedBvh {};
        auto node_count = stream.read<uint64_t>();
        auto prim_count = stream.read<uint64_t>();
        // Refuse absurd sizes from corrupted streams.
        if (node_count > (uint64_t{1} << 40) || prim_count > (uint64_t{1} << 40))
            return OrientedBvh {};

        OrientedBvh result;
        result.nodes_.resize(static_cast<size_t>(node_count));
        result.prim_ids_.resize(static_cast<size_t>(prim_count));
        for (auto& node : result.nodes_) {
            for (auto& axis : node.axis) {
                axis[0] = stream.read<T>();
                axis[1] = stream.read<T>();
                axis[2] = stream.read<T>();
            }
            node.center[0] = stream.read<T>();
            node.center[1] = stream.read<T>();
            node.center[2] = stream.read<T>();
            node.half_size[0] = stream.read<T>();
            node.half_size[1] = stream.read<T>();
            node.half_size[2] = stream.read<T>();
            node.index = Index(stream.read<typename Index::Type>());
        }
        for (auto& prim_id : result.prim_ids_)
            prim_id = static_cast<size_t>(stream.read<uint64_t>());
        return result;
    }

private:
    std::vector<NodeData> nodes_;
    std::vector<size_t> prim_ids_;

    void refit_leaf(size_t node_id, std::span<const Triangle> triangles) {
        auto& node = nodes_[node_id];
        obb_detail::ExtentAccumulator<T> acc(node.axis);
        auto begin = static_cast<size_t>(node.index.first_id());
        auto end = begin + static_cast<size_t>(node.index.prim_count());
        for (size_t k = begin; k < end && k < prim_ids_.size(); ++k) {
            auto prim_id = prim_ids_[k];
            if (prim_id >= triangles.size())
                continue;
            const auto& tri = triangles[prim_id];
            acc.add(tri.p0);
            acc.add(tri.p1);
            acc.add(tri.p2);
        }
        if (!acc.any) {
            node.half_size = Vec3(static_cast<T>(0));
            return;
        }
        Vec3 center, half_size;
        obb_detail::ObbBuilder<T>::finish(
            node.axis, acc.min_proj, acc.max_proj, acc.max_abs, center, half_size);
        node.center = center;
        node.half_size = half_size;
    }

    void refit_inner(size_t node_id) {
        auto& node = nodes_[node_id];
        auto first = static_cast<size_t>(node.index.first_id());
        const auto& left = nodes_[first + 0];
        const auto& right = nodes_[first + 1];

        auto [left_min, left_max] = left.project_corners(node.axis);
        auto [right_min, right_max] = right.project_corners(node.axis);
        auto min_proj = robust_min(left_min, right_min);
        auto max_proj = robust_max(left_max, right_max);

        auto mid = (min_proj + max_proj) * static_cast<T>(0.5);
        node.center = node.axis[0] * mid[0] + node.axis[1] * mid[1] + node.axis[2] * mid[2];
        node.half_size = Vec3(
            std::max(static_cast<T>(0), (max_proj[0] - min_proj[0]) * static_cast<T>(0.5)),
            std::max(static_cast<T>(0), (max_proj[1] - min_proj[1]) * static_cast<T>(0.5)),
            std::max(static_cast<T>(0), (max_proj[2] - min_proj[2]) * static_cast<T>(0.5)));
    }
};

} // namespace bvh::v2

#endif
