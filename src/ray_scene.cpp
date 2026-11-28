/**
 * Implementation of direct ray intersection with rational NURBS surfaces.
 *
 * The strategy is a two-stage hierarchical search:
 *   1. Each surface is decomposed once into rational Bezier elements (one per
 *      non-empty knot span) using local Bezier extraction on the homogeneous
 *      control net. Their perspective-projected control points give
 *      conservative convex-hull boxes, organized in a BVH.
 *   2. A ray query walks the BVHs with slab tests, then adaptively subdivides
 *      surviving Bezier patches (de Casteljau in homogeneous space). Every
 *      visited box is a conservative bound; roots are only accepted after a
 *      converged Newton solve with back-substituted residual, parameter
 *      containment and Jacobian checks. Everything that cannot be classified
 *      is reported as unresolved rather than missed.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "tinynurbs/core/ray_scene.h"
#include "tinynurbs/core/basis.h"
#include "tinynurbs/core/check.h"
#include "tinynurbs/core/evaluate.h"
#include "tinynurbs/core/modify.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <unordered_set>

namespace tinynurbs
{

namespace
{

using dvec3 = glm::dvec3;
using dvec4 = glm::dvec4;

bool finite3(const dvec3 &v)
{
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

struct Aabb
{
    dvec3 mn{std::numeric_limits<double>::infinity()};
    dvec3 mx{-std::numeric_limits<double>::infinity()};
    void expand(const dvec3 &p)
    {
        mn = glm::min(mn, p);
        mx = glm::max(mx, p);
    }
    void expand(const Aabb &other)
    {
        mn = glm::min(mn, other.mn);
        mx = glm::max(mx, other.mx);
    }
    bool valid() const { return std::isfinite(mn.x) && std::isfinite(mx.x); }
    dvec3 center() const { return 0.5 * (mn + mx); }
};

// Conservative box of a rational Bezier net: with strictly positive weights
// the rational patch is contained in the convex hull of the projected
// homogeneous control points.
Aabb netBox(const array2<dvec4> &net)
{
    Aabb box;
    for (size_t i = 0; i < net.size(); ++i)
    {
        const dvec4 &h = net[i];
        box.expand(dvec3(h.x / h.w, h.y / h.w, h.z / h.w));
    }
    return box;
}

/**
 * Decompose a homogeneous B-spline curve into one Bezier segment per
 * non-empty knot span, using the library's own tested curveSplit. Segment
 * order matches spanStarts(). Repeatedly splitting the right remainder keeps
 * the original curve identical while exposing every span's Bernstein
 * coefficients.
 */
std::vector<std::vector<dvec4>>
decomposeCurveBezier(unsigned int p, std::vector<double> knots, std::vector<dvec4> points)
{
    std::vector<std::vector<dvec4>> segments;
    while (true)
    {
        if (static_cast<int>(knots.size()) == 2 * static_cast<int>(p) + 2)
        {
            segments.push_back(std::move(points));
            break;
        }
        int last = static_cast<int>(knots.size()) - static_cast<int>(p) - 2;
        int k = static_cast<int>(p);
        while (k <= last && knots[k] == knots[k + 1]) ++k;
        double u = knots[k + 1];
        std::vector<double> left_knots, right_knots;
        std::vector<dvec4> left_points, right_points;
        internal::curveSplit(p, knots, points, u, left_knots, left_points, right_knots,
                             right_points);
        segments.emplace_back(left_points.end() - (p + 1), left_points.end());
        knots = std::move(right_knots);
        points = std::move(right_points);
    }
    return segments;
}

// de Casteljau split of a homogeneous Bezier net at a along the row (u) axis.
void splitNetU(const array2<dvec4> &net, unsigned int pu, unsigned int pv, double a,
               array2<dvec4> &left, array2<dvec4> &right)
{
    left.resize(pu + 1, pv + 1);
    right.resize(pu + 1, pv + 1);
    for (unsigned int j = 0; j <= pv; ++j)
    {
        std::vector<dvec4> b(pu + 1);
        for (unsigned int i = 0; i <= pu; ++i) b[i] = net(i, j);
        left(0, j) = b[0];
        right(pu, j) = b[pu];
        for (unsigned int r = 1; r <= pu; ++r)
        {
            for (unsigned int i = 0; i + r <= pu; ++i)
                b[i] = (1.0 - a) * b[i] + a * b[i + 1];
            left(r, j) = b[0];
            right(pu - r, j) = b[pu - r];
        }
    }
}

// de Casteljau split along the column (v) axis.
void splitNetV(const array2<dvec4> &net, unsigned int pu, unsigned int pv, double b,
               array2<dvec4> &left, array2<dvec4> &right)
{
    left.resize(pu + 1, pv + 1);
    right.resize(pu + 1, pv + 1);
    for (unsigned int i = 0; i <= pu; ++i)
    {
        std::vector<dvec4> coef(pv + 1);
        for (unsigned int j = 0; j <= pv; ++j) coef[j] = net(i, j);
        left(i, 0) = coef[0];
        right(i, pv) = coef[pv];
        for (unsigned int r = 1; r <= pv; ++r)
        {
            for (unsigned int j = 0; j + r <= pv; ++j)
                coef[j] = (1.0 - b) * coef[j] + b * coef[j + 1];
            left(i, r) = coef[0];
            right(i, pv - r) = coef[pv - r];
        }
    }
}

struct Element
{
    int span_i = 0, span_j = 0;       // indices into SurfaceData span lists
    int ku = 0, kv = 0;               // knot span start indices
    double u0 = 0, u1 = 0, v0 = 0, v1 = 0;
    array2<dvec4> net;                // homogeneous Bernstein coefficients
    Aabb box;
};

struct TreeNode
{
    int parent = -1;
    int left = -1, right = -1;        // children (-1 for leaves)
    int element = -1;                 // element index for leaves
    Aabb box;
};

struct SurfaceData
{
    RationalSurface3d surface;
    array2<dvec4> homo;                // homogeneous original control net
    std::vector<int> span_u, span_v;   // knot span start indices, in order
    std::vector<Element> elements;
    std::vector<TreeNode> nodes;
    int root = -1;
};

void buildHomogeneousNet(const RationalSurface3d &srf, array2<dvec4> &homo)
{
    homo.resize(srf.control_points.rows(), srf.control_points.cols());
    for (size_t i = 0; i < srf.control_points.rows(); ++i)
        for (size_t j = 0; j < srf.control_points.cols(); ++j)
        {
            const dvec3 &p = srf.control_points(i, j);
            double w = srf.weights(i, j);
            homo(i, j) = dvec4(p.x * w, p.y * w, p.z * w, w);
        }
}

// Re-extract a single Bezier element (span indices span_i, span_j) from the
// current homogeneous control net. The u pass decomposes each involved full
// column; the v pass decomposes full rows built from the u-Bezier
// coefficients at their global column positions.
array2<dvec4> extractElement(const SurfaceData &sd, int span_i, int span_j)
{
    const RationalSurface3d &s = sd.surface;
    const unsigned int pu = s.degree_u, pv = s.degree_v;
    const int kv = sd.span_v[span_j];
    const size_t rows = sd.homo.rows(), cols = sd.homo.cols();

    array2<dvec4> tmp(pu + 1, pv + 1);
    std::vector<dvec4> column(rows);
    for (unsigned int lj = 0; lj <= pv; ++lj)
    {
        size_t c = static_cast<size_t>(kv - static_cast<int>(pv) + lj);
        for (size_t r = 0; r < rows; ++r) column[r] = sd.homo(r, c);
        std::vector<std::vector<dvec4>> u_segments =
            decomposeCurveBezier(pu, s.knots_u, column);
        for (unsigned int i = 0; i <= pu; ++i) tmp(i, lj) = u_segments[span_i][i];
    }

    array2<dvec4> out(pu + 1, pv + 1);
    std::vector<dvec4> row(cols);
    for (unsigned int i = 0; i <= pu; ++i)
    {
        std::fill(row.begin(), row.end(), dvec4(0.0));
        for (unsigned int lj = 0; lj <= pv; ++lj)
            row[static_cast<size_t>(kv - static_cast<int>(pv) + lj)] = tmp(i, lj);
        std::vector<std::vector<dvec4>> v_segments =
            decomposeCurveBezier(pv, s.knots_v, row);
        for (unsigned int j = 0; j <= pv; ++j) out(i, j) = v_segments[span_j][j];
    }
    return out;
}

std::vector<int> spanStarts(unsigned int degree, const std::vector<double> &knots)
{
    std::vector<int> spans;
    int last = static_cast<int>(knots.size()) - static_cast<int>(degree) - 2;
    for (int k = static_cast<int>(degree); k <= last; ++k)
        if (knots[k] < knots[k + 1]) spans.push_back(k);
    return spans;
}

// Recursive median BVH build; children always get lower indices than parents.
int buildTree(SurfaceData &sd, const std::vector<int> &leaves, int parent,
              std::size_t &extraction_counter)
{
    if (leaves.size() == 1)
    {
        TreeNode node;
        node.parent = parent;
        node.element = leaves.front();
        node.box = sd.elements[node.element].box;
        sd.nodes.push_back(node);
        return static_cast<int>(sd.nodes.size()) - 1;
    }
    Aabb centroid;
    for (int e : leaves) centroid.expand(sd.elements[e].box.center());
    dvec3 ext = centroid.mx - centroid.mn;
    bool along_u = ext.x >= ext.y && ext.x >= ext.z;
    bool along_v = !along_u && ext.y >= ext.z;
    std::vector<int> sorted = leaves;
    std::sort(sorted.begin(), sorted.end(), [&](int a, int b) {
        const dvec3 &ca = sd.elements[a].box.center();
        const dvec3 &cb = sd.elements[b].box.center();
        if (along_u) return ca.x < cb.x;
        if (along_v) return ca.y < cb.y;
        return ca.z < cb.z;
    });
    size_t mid = sorted.size() / 2;
    std::vector<int> left_leaves(sorted.begin(), sorted.begin() + mid);
    std::vector<int> right_leaves(sorted.begin() + mid, sorted.end());
    int placeholder = static_cast<int>(sd.nodes.size());
    sd.nodes.push_back(TreeNode()); // reserve parent slot
    int left = buildTree(sd, left_leaves, placeholder, extraction_counter);
    int right = buildTree(sd, right_leaves, placeholder, extraction_counter);
    TreeNode node;
    node.parent = parent;
    node.left = left;
    node.right = right;
    node.box = sd.nodes[left].box;
    node.box.expand(sd.nodes[right].box);
    sd.nodes[placeholder] = node;
    return placeholder;
}

void validateSurface(const RationalSurface3d &srf)
{
    auto fail = [](const char *msg) { throw std::invalid_argument(msg); };
    if (!surfaceIsValid(srf)) fail("RaySurfaceScene: invalid rational surface");
    if (srf.degree_u < 1 || srf.degree_u > 3 || srf.degree_v < 1 || srf.degree_v > 3)
        fail("RaySurfaceScene: only degrees 1..3 are supported");
    auto check_knots = [&](unsigned int degree, const std::vector<double> &knots) {
        if (knots.size() < 2 * (degree + 1)) fail("RaySurfaceScene: knot vector too short");
        for (unsigned int i = 0; i < degree; ++i)
            if (knots[i] != knots[i + 1] ||
                knots[knots.size() - 1 - i] != knots[knots.size() - 2 - i])
                fail("RaySurfaceScene: only clamped knot vectors are supported");
        if (!std::isfinite(knots.front()) || !std::isfinite(knots.back()))
            fail("RaySurfaceScene: non-finite knot values");
        for (size_t i = degree + 1; i + degree + 1 < knots.size();)
        {
            size_t j = i;
            while (j + 1 < knots.size() && knots[j + 1] == knots[i]) ++j;
            if (j - i + 1 > degree)
                fail("RaySurfaceScene: interior knot multiplicity exceeds degree");
            i = j + 1;
        }
    };
    check_knots(srf.degree_u, srf.knots_u);
    check_knots(srf.degree_v, srf.knots_v);
    for (size_t i = 0; i < srf.weights.size(); ++i)
    {
        double w = srf.weights[i];
        if (!std::isfinite(w) || w <= 0.0)
            fail("RaySurfaceScene: weights must be strictly positive and finite");
    }
    for (size_t i = 0; i < srf.control_points.size(); ++i)
        if (!finite3(srf.control_points[i]))
            fail("RaySurfaceScene: non-finite control point");
}

// Exact infinite-ray-line / axis-aligned-box slab test, intersected with
// the finite [tmin, tmax) window. Returns false only when the line provably
// misses the box. The homogeneous-net box is conservative for the surface, so
// a missed box is a guaranteed miss for the patch inside it.
bool rayBox(const dvec3 &o, const dvec3 &d, const Aabb &box, double tmin, double tmax,
            double &tlo, double &thi)
{
    tlo = -std::numeric_limits<double>::infinity();
    thi = std::numeric_limits<double>::infinity();
    for (int axis = 0; axis < 3; ++axis)
    {
        double oa = o[axis], da = d[axis], lo = box.mn[axis], hi = box.mx[axis];
        if (da == 0.0)
        {
            if (oa < lo || oa > hi) return false;
            continue;
        }
        double t1 = (lo - oa) / da;
        double t2 = (hi - oa) / da;
        if (t1 > t2) std::swap(t1, t2);
        tlo = std::max(tlo, t1);
        thi = std::min(thi, t2);
        if (tlo > thi) return false;
    }
    if (thi < tmin || tlo >= tmax) return false;
    return true;
}

// Orthonormal frame of the ray: e1, e2 span the plane perpendicular to D.
struct RayFrame
{
    dvec3 o, d, e1, e2;
};

RayFrame makeFrame(const dvec3 &o, const dvec3 &d)
{
    RayFrame f;
    f.o = o;
    f.d = d;
    dvec3 axis = std::abs(d.x) > std::abs(d.y) ? dvec3(0, 1, 0) : dvec3(1, 0, 0);
    f.e1 = glm::normalize(glm::cross(axis, d));
    f.e2 = glm::normalize(glm::cross(d, f.e1));
    return f;
}

// Strict transverse exclusion in the ray's coordinate frame. The rational
// patch lies in the convex hull of its projected homogeneous control points;
// if that hull's bounding rectangle strictly avoids the ray axis (the origin
// of the (e1,e2) plane), the ray line cannot meet the patch. A box that only
// fails this test ambiguously is kept and subdivided further.
bool patchStrictlyMisses(const RayFrame &ray, const array2<dvec4> &net)
{
    double a_lo = std::numeric_limits<double>::infinity();
    double a_hi = -std::numeric_limits<double>::infinity();
    double b_lo = a_lo, b_hi = a_hi;
    for (size_t i = 0; i < net.size(); ++i)
    {
        const dvec4 &h = net[i];
        dvec3 p(h.x / h.w, h.y / h.w, h.z / h.w);
        dvec3 r = p - ray.o;
        double a = glm::dot(r, ray.e1);
        double b = glm::dot(r, ray.e2);
        a_lo = std::min(a_lo, a);
        a_hi = std::max(a_hi, a);
        b_lo = std::min(b_lo, b);
        b_hi = std::max(b_hi, b);
    }
    double eps = 1e-13 *
                 std::max(1e-300, std::max(std::max(std::abs(a_lo), std::abs(a_hi)),
                                           std::max(std::abs(b_lo), std::abs(b_hi))));
    return a_lo > eps || a_hi < -eps || b_lo > eps || b_hi < -eps;
}

struct WorkItem
{
    bool is_node = true;
    std::size_t surface = 0;
    int node = -1;
    double u0 = 0, u1 = 0, v0 = 0, v1 = 0;
    array2<dvec4> net;
};

struct Candidate
{
    std::size_t surface;
    double u, v, t;
    dvec3 point;
};

// Full 3x3 Newton on the ray/surface system F(t,u,v) = O + tD - S(u,v).
// Keeping t as an unknown makes the Jacobian [D, -Su, -Sv] regular at
// grazing double roots where a 2D projection onto (u,v) degenerates to
// linear convergence. Adaptive Bezier boxes only provide pruning; every
// accepted root is verified against the real surface.
bool newtonRoot(const SurfaceData &sd, const WorkItem &item, const dvec3 &origin,
                const dvec3 &direction, double d2, const RayQueryOptions &opt, double tmin,
                double tmax, Candidate &out)
{
    const RationalSurface3d &s = sd.surface;
    double u = 0.5 * (item.u0 + item.u1);
    double v = 0.5 * (item.v0 + item.v1);
    const double width_u = item.u1 - item.u0;
    const double width_v = item.v1 - item.v0;
    // A box's midpoint may sit a couple of steps away from the root it
    // encloses; iterates wandering several box widths away belong to a
    // neighbour's root.
    const double bail_u = 4.0 * width_u;
    const double bail_v = 4.0 * width_v;
    const double domain_u =
        std::max(1.0, std::abs(s.knots_u.back()) + std::abs(s.knots_u.front()));
    const double domain_v =
        std::max(1.0, std::abs(s.knots_v.back()) + std::abs(s.knots_v.front()));
    const double d_len = std::sqrt(d2);

    dvec3 point = surfacePoint(s, u, v);
    double t = glm::dot(point - origin, direction) / d2;
    bool converged = false;
    double prev_step = std::numeric_limits<double>::infinity();
    dvec3 final_residual(0.0);
    double last_du = 0.0, last_dv = 0.0;
    for (int iter = 0; iter < 50; ++iter)
    {
        array2<dvec3> ders = surfaceDerivatives(s, 1, u, v);
        point = ders(0, 0);
        dvec3 su = ders(1, 0);
        dvec3 sv = ders(0, 1);
        dvec3 residual = origin + t * direction - point;
        final_residual = residual;
        // Solve J x = F with J = [D, -Su, -Sv] via explicit cofactors.
        dvec3 c1 = -su;
        dvec3 c2 = -sv;
        double det = glm::dot(direction, glm::cross(c1, c2));
        double scale = d_len * glm::length(su) * glm::length(sv);
        if (!std::isfinite(det) || std::abs(det) <= 1e-14 * std::max(1.0, scale))
            return false; // tangent / degenerate Jacobian
        dvec3 x;
        x.x = glm::dot(residual, glm::cross(c1, c2)) / det;
        x.y = glm::dot(direction, glm::cross(residual, c2)) / det;
        x.z = glm::dot(direction, glm::cross(c1, residual)) / det;
        t -= x.x;
        u -= x.y;
        v -= x.z;
        last_du = x.y;
        last_dv = x.z;
        if (!std::isfinite(u) || !std::isfinite(v) || !std::isfinite(t)) return false;
        if (u < item.u0 - bail_u || u > item.u1 + bail_u ||
            v < item.v0 - bail_v || v > item.v1 + bail_v)
            return false; // root belongs to a distant box
        double step = std::max(std::abs(x.y) / domain_u, std::abs(x.z) / domain_v);
        // Quadratic Newton reaches a machine-precision stagnation floor; run
        // until it stalls, then verify against the query tolerances.
        if (iter > 0 && step >= 0.5 * prev_step)
        {
            converged = true;
            break;
        }
        prev_step = step;
    }
    if (!converged || glm::length(final_residual) > opt.position_tolerance ||
        std::abs(last_du) > opt.parameter_tolerance * domain_u ||
        std::abs(last_dv) > opt.parameter_tolerance * domain_v)
        return false;
    // The converged root must actually belong to this box (with a
    // tolerance-sized margin for boundary roots shared with a neighbour).
    if (u < item.u0 - opt.parameter_tolerance * domain_u ||
        u > item.u1 + opt.parameter_tolerance * domain_u ||
        v < item.v0 - opt.parameter_tolerance * domain_v ||
        v > item.v1 + opt.parameter_tolerance * domain_v)
        return false;
    // Back-substitution on the actual surface, not on the iteration residual.
    dvec3 exact = surfacePoint(s, u, v);
    t = glm::dot(exact - origin, direction) / d2;
    if (t < tmin || t >= tmax) return false;
    if (glm::length(origin + t * direction - exact) > opt.position_tolerance) return false;
    out.surface = item.surface;
    out.u = u;
    out.v = v;
    out.t = t;
    out.point = exact;
    return true;
}

dvec3 normalAt(const RationalSurface3d &s, double u, double v)
{
    // findSpan picks the right element on interior knots and the last element
    // at the upper domain bound, matching the required orientation convention.
    array2<dvec3> ders = surfaceDerivatives(s, 1, u, v);
    dvec3 n = glm::cross(ders(1, 0), ders(0, 1)); // Su x Sv
    double len = glm::length(n);
    if (len > 0.0) n /= len;
    return n;
}

} // namespace

struct RaySurfaceScene::Impl
{
    std::vector<SurfaceData> surfaces;
    std::size_t extractions = 0;

    void build(const std::vector<RationalSurface3d> &input)
    {
        surfaces.clear();
        extractions = 0;
        surfaces.reserve(input.size());
        for (const RationalSurface3d &srf : input)
        {
            validateSurface(srf);
            SurfaceData sd;
            sd.surface = srf;
            buildHomogeneousNet(sd.surface, sd.homo);
            sd.span_u = spanStarts(sd.surface.degree_u, sd.surface.knots_u);
            sd.span_v = spanStarts(sd.surface.degree_v, sd.surface.knots_v);
            if (sd.span_u.empty() || sd.span_v.empty())
                throw std::invalid_argument("RaySurfaceScene: empty parameter domain");
            sd.elements.reserve(sd.span_u.size() * sd.span_v.size());
            std::vector<int> leaf_indices;
            for (size_t si = 0; si < sd.span_u.size(); ++si)
                for (size_t sj = 0; sj < sd.span_v.size(); ++sj)
                {
                    int ku = sd.span_u[si];
                    int kv = sd.span_v[sj];
                    Element e;
                    e.span_i = static_cast<int>(si);
                    e.span_j = static_cast<int>(sj);
                    e.ku = ku;
                    e.kv = kv;
                    e.u0 = sd.surface.knots_u[ku];
                    e.u1 = sd.surface.knots_u[ku + 1];
                    e.v0 = sd.surface.knots_v[kv];
                    e.v1 = sd.surface.knots_v[kv + 1];
                    e.net = extractElement(sd, e.span_i, e.span_j);
                    e.box = netBox(e.net);
                    if (!e.box.valid())
                        throw std::invalid_argument(
                            "RaySurfaceScene: non-finite homogeneous control net");
                    leaf_indices.push_back(static_cast<int>(sd.elements.size()));
                    sd.elements.push_back(std::move(e));
                    ++extractions;
                }
            sd.root = buildTree(sd, leaf_indices, -1, extractions);
            surfaces.push_back(std::move(sd));
        }
    }
};

RaySurfaceScene::RaySurfaceScene(const std::vector<RationalSurface3d> &surfaces)
    : impl_(std::make_unique<Impl>())
{
    impl_->build(surfaces);
}

RaySurfaceScene::~RaySurfaceScene() = default;

RaySurfaceScene::RaySurfaceScene(const RaySurfaceScene &other)
    : impl_(std::make_unique<Impl>(*other.impl_))
{
}

RaySurfaceScene &RaySurfaceScene::operator=(const RaySurfaceScene &other)
{
    if (this != &other) impl_ = std::make_unique<Impl>(*other.impl_);
    return *this;
}

RaySurfaceScene::RaySurfaceScene(RaySurfaceScene &&other) noexcept = default;
RaySurfaceScene &RaySurfaceScene::operator=(RaySurfaceScene &&other) noexcept = default;

RayQueryResult RaySurfaceScene::intersect(glm::dvec3 origin, glm::dvec3 direction, double tmin,
                                          double tmax, const RayQueryOptions &options) const
{
    RayQueryResult result;
    if (direction.x == 0.0 && direction.y == 0.0 && direction.z == 0.0)
    {
        result.error = RaySceneError::ZeroDirection;
        return result;
    }
    if (!finite3(origin) || !finite3(direction))
    {
        result.error = RaySceneError::NonFiniteInput;
        return result;
    }
    if (!std::isfinite(tmin) || !std::isfinite(tmax) || tmin >= tmax)
    {
        result.error = RaySceneError::InvalidInterval;
        return result;
    }
    if (!std::isfinite(options.position_tolerance) || options.position_tolerance <= 0.0 ||
        !std::isfinite(options.parameter_tolerance) || options.parameter_tolerance <= 0.0)
    {
        result.error = RaySceneError::NonPositiveTolerance;
        return result;
    }
    if (options.box_budget == 0)
    {
        result.error = RaySceneError::ZeroBudget;
        return result;
    }

    const double d2 = glm::dot(direction, direction);
    const RayFrame frame = makeFrame(origin, direction);
    std::vector<WorkItem> stack;
    std::vector<Candidate> candidates;
    std::vector<RaySurfaceUnresolved> unresolved;
    std::size_t &visits = result.box_visits;
    std::size_t &patch_tests = result.patch_boxes_tested;

    for (std::size_t s = 0; s < impl_->surfaces.size(); ++s)
        stack.push_back(WorkItem{true, s, impl_->surfaces[s].root, 0, 0, 0, 0, {}});

    auto push_unresolved = [&](const WorkItem &item) {
        RaySurfaceUnresolved u;
        u.surface_index = item.surface;
        if (item.is_node)
        {
            // A tree node carries no parameter box; conservatively envelope
            // the whole surface domain it belongs to.
            const RationalSurface3d &srf = impl_->surfaces[item.surface].surface;
            u.u_min = srf.knots_u.front();
            u.u_max = srf.knots_u.back();
            u.v_min = srf.knots_v.front();
            u.v_max = srf.knots_v.back();
        }
        else
        {
            u.u_min = item.u0;
            u.u_max = item.u1;
            u.v_min = item.v0;
            u.v_max = item.v1;
        }
        unresolved.push_back(u);
    };

    while (!stack.empty())
    {
        WorkItem item = stack.back();
        stack.pop_back();
        if (visits >= options.box_budget)
        {
            // Everything still outstanding cannot be excluded: report it.
            push_unresolved(item);
            for (const WorkItem &rest : stack) push_unresolved(rest);
            stack.clear();
            break;
        }
        ++visits;
        const SurfaceData &sd = impl_->surfaces[item.surface];
        double tlo, thi;
        if (item.is_node)
        {
            const TreeNode &node = sd.nodes[item.node];
            if (!rayBox(origin, direction, node.box, tmin, tmax, tlo, thi)) continue;
            if (node.element >= 0)
            {
                const Element &e = sd.elements[node.element];
                WorkItem leaf;
                leaf.is_node = false;
                leaf.surface = item.surface;
                leaf.u0 = e.u0;
                leaf.u1 = e.u1;
                leaf.v0 = e.v0;
                leaf.v1 = e.v1;
                leaf.net = e.net;
                stack.push_back(std::move(leaf));
            }
            else
            {
                stack.push_back(WorkItem{true, item.surface, node.left, 0, 0, 0, 0, {}});
                stack.push_back(WorkItem{true, item.surface, node.right, 0, 0, 0, 0, {}});
            }
            continue;
        }

        ++patch_tests;
        const Aabb box = netBox(item.net);
        if (!rayBox(origin, direction, box, tmin, tmax, tlo, thi)) continue;
        // Stronger convex-hull exclusion in the ray's transverse frame.
        if (patchStrictlyMisses(frame, item.net)) continue;

        Candidate candidate;
        if (newtonRoot(sd, item, origin, direction, d2, options, tmin, tmax, candidate))
            candidates.push_back(candidate);

        double span_u = item.u1 - item.u0;
        double span_v = item.v1 - item.v0;
        // Requested parameter accuracy: a box this small that already
        // contains a verified root needs no further subdivision.
        bool fine_u = span_u <= options.parameter_tolerance;
        bool fine_v = span_v <= options.parameter_tolerance;
        auto covered_by_hit = [&](const WorkItem &it) {
            dvec3 midpoint =
                surfacePoint(sd.surface, 0.5 * (it.u0 + it.u1), 0.5 * (it.v0 + it.v1));
            for (const Candidate &c : candidates)
                if (c.surface == it.surface &&
                    glm::length(c.point - midpoint) <= options.position_tolerance)
                    return true;
            return false;
        };
        if (fine_u && fine_v)
        {
            if (covered_by_hit(item)) continue;
            // Not resolved at the requested accuracy: keep refining down to
            // the floating-point resolution floor instead of giving up, so
            // ordinary transverse misses are classified exactly.
        }
        double floor_u = 8.0 * std::numeric_limits<double>::epsilon() *
                         std::max(1.0, std::abs(item.u0) + std::abs(item.u1));
        double floor_v = 8.0 * std::numeric_limits<double>::epsilon() *
                         std::max(1.0, std::abs(item.v0) + std::abs(item.v1));
        bool can_u = span_u > floor_u;
        bool can_v = span_v > floor_v;
        if (!can_u && !can_v)
        {
            // No representable refinement is left. A verified hit accounts
            // for the region; otherwise it is genuinely unresolved.
            if (!covered_by_hit(item)) push_unresolved(item);
            continue;
        }

        WorkItem left, right;
        left.is_node = right.is_node = false;
        left.surface = right.surface = item.surface;
        if (can_u && (!can_v || span_u >= span_v))
        {
            array2<dvec4> nl, nr;
            splitNetU(item.net, sd.surface.degree_u, sd.surface.degree_v, 0.5, nl, nr);
            double um = 0.5 * (item.u0 + item.u1);
            left.u0 = item.u0;
            left.u1 = um;
            left.v0 = item.v0;
            left.v1 = item.v1;
            left.net = std::move(nl);
            right.u0 = um;
            right.u1 = item.u1;
            right.v0 = item.v0;
            right.v1 = item.v1;
            right.net = std::move(nr);
        }
        else
        {
            array2<dvec4> nl, nr;
            splitNetV(item.net, sd.surface.degree_u, sd.surface.degree_v, 0.5, nl, nr);
            double vm = 0.5 * (item.v0 + item.v1);
            left.u0 = item.u0;
            left.u1 = item.u1;
            left.v0 = item.v0;
            left.v1 = vm;
            left.net = std::move(nl);
            right.u0 = item.u0;
            right.u1 = item.u1;
            right.v0 = vm;
            right.v1 = item.v1;
            right.net = std::move(nr);
        }
        stack.push_back(std::move(left));
        stack.push_back(std::move(right));
    }

    // Merge same-surface parameter roots rediscovered across knot boundaries
    // or shared subdivision boxes. Distinct UV roots are always preserved.
    std::sort(candidates.begin(), candidates.end(), [](const Candidate &a, const Candidate &b) {
        if (a.surface != b.surface) return a.surface < b.surface;
        if (a.u != b.u) return a.u < b.u;
        return a.v < b.v;
    });
    const double merge_uv = std::max(1e-12, 8.0 * options.parameter_tolerance);
    std::vector<Candidate> merged;
    for (const Candidate &c : candidates)
    {
        const RationalSurface3d &s = impl_->surfaces[c.surface].surface;
        double scale_u =
            std::max(1.0, std::abs(s.knots_u.back()) + std::abs(s.knots_u.front()));
        double scale_v =
            std::max(1.0, std::abs(s.knots_v.back()) + std::abs(s.knots_v.front()));
        double tol_u = merge_uv * scale_u;
        double tol_v = merge_uv * scale_v;
        if (!merged.empty() && merged.back().surface == c.surface &&
            std::abs(merged.back().u - c.u) <= tol_u &&
            std::abs(merged.back().v - c.v) <= tol_v)
            continue;
        merged.push_back(c);
    }

    result.hits.reserve(merged.size());
    for (const Candidate &c : merged)
    {
        RaySurfaceHit hit;
        hit.surface_index = c.surface;
        hit.u = c.u;
        hit.v = c.v;
        hit.t = c.t;
        hit.point = c.point;
        hit.normal = normalAt(impl_->surfaces[c.surface].surface, c.u, c.v);
        result.hits.push_back(hit);
    }
    std::sort(result.hits.begin(), result.hits.end(),
              [](const RaySurfaceHit &a, const RaySurfaceHit &b) {
                  if (a.t != b.t) return a.t < b.t;
                  if (a.surface_index != b.surface_index)
                      return a.surface_index < b.surface_index;
                  if (a.u != b.u) return a.u < b.u;
                  return a.v < b.v;
              });

    // Coalesce touching/overlapping unresolved boxes per surface into the few
    // regions that actually cover the unclassifiable area. The merged ranges
    // remain true envelopes of every underlying box.
    std::sort(unresolved.begin(), unresolved.end(),
              [](const RaySurfaceUnresolved &a, const RaySurfaceUnresolved &b) {
                  if (a.surface_index != b.surface_index)
                      return a.surface_index < b.surface_index;
                  if (a.u_min != b.u_min) return a.u_min < b.u_min;
                  if (a.u_max != b.u_max) return a.u_max < b.u_max;
                  if (a.v_min != b.v_min) return a.v_min < b.v_min;
                  return a.v_max < b.v_max;
              });
    std::vector<RaySurfaceUnresolved> merged_unresolved;
    for (const RaySurfaceUnresolved &box : unresolved)
    {
        bool absorbed = false;
        for (RaySurfaceUnresolved &region : merged_unresolved)
        {
            if (region.surface_index != box.surface_index) continue;
            double eps_u = 1e-13 *
                           std::max(1.0, std::abs(region.u_max) + std::abs(box.u_max));
            double eps_v = 1e-13 *
                           std::max(1.0, std::abs(region.v_max) + std::abs(box.v_max));
            bool touch_u = box.u_min <= region.u_max + eps_u &&
                           region.u_min <= box.u_max + eps_u;
            bool touch_v = box.v_min <= region.v_max + eps_v &&
                           region.v_min <= box.v_max + eps_v;
            if (touch_u && touch_v)
            {
                region.u_min = std::min(region.u_min, box.u_min);
                region.u_max = std::max(region.u_max, box.u_max);
                region.v_min = std::min(region.v_min, box.v_min);
                region.v_max = std::max(region.v_max, box.v_max);
                absorbed = true;
                break;
            }
        }
        if (!absorbed) merged_unresolved.push_back(box);
    }
    result.unresolved = std::move(merged_unresolved);
    result.complete = result.unresolved.empty();
    return result;
}

RaySceneEditResult
RaySurfaceScene::updateControlPoints(const std::vector<ControlPointEdit> &edits)
{
    RaySceneEditResult er;
    if (edits.empty()) return er;

    // Whole-batch validation before any mutation.
    std::unordered_set<unsigned long long> seen;
    for (const ControlPointEdit &e : edits)
    {
        if (e.surface_index >= impl_->surfaces.size())
        {
            er.error = RaySceneEditError::SurfaceIndexOutOfRange;
            return er;
        }
        const SurfaceData &sd = impl_->surfaces[e.surface_index];
        if (e.row >= sd.surface.control_points.rows() ||
            e.column >= sd.surface.control_points.cols())
        {
            er.error = RaySceneEditError::ControlPointIndexOutOfRange;
            return er;
        }
        if (!finite3(e.position) || !std::isfinite(e.weight))
        {
            er.error = RaySceneEditError::NonFiniteValue;
            return er;
        }
        if (e.weight <= 0.0)
        {
            er.error = RaySceneEditError::NonPositiveWeight;
            return er;
        }
        unsigned long long key =
            (static_cast<unsigned long long>(e.surface_index) << 48) ^
            (static_cast<unsigned long long>(e.row) << 24) ^
            static_cast<unsigned long long>(e.column);
        if (!seen.insert(key).second)
        {
            er.error = RaySceneEditError::DuplicateAddress;
            return er;
        }
    }

    struct Addr
    {
        std::size_t s, row, col;
    };
    std::vector<Addr> addresses;
    addresses.reserve(edits.size());

    // Apply absolute values to the owned surface data.
    for (const ControlPointEdit &e : edits)
    {
        SurfaceData &sd = impl_->surfaces[e.surface_index];
        sd.surface.control_points(e.row, e.column) = e.position;
        sd.surface.weights(e.row, e.column) = e.weight;
        sd.homo(e.row, e.column) =
            dvec4(e.position.x * e.weight, e.position.y * e.weight,
                  e.position.z * e.weight, e.weight);
        addresses.push_back(Addr{e.surface_index, e.row, e.column});
    }

    for (std::size_t si = 0; si < impl_->surfaces.size(); ++si)
    {
        SurfaceData &sd = impl_->surfaces[si];
        std::unordered_set<int> dirty_elements;
        for (const Addr &a : addresses)
        {
            if (a.s != si) continue;
            // B-spline local support: element span k uses control rows
            // [k-p, k] and columns [l-q, l].
            for (int ei = 0; ei < static_cast<int>(sd.elements.size()); ++ei)
            {
                const Element &el = sd.elements[ei];
                unsigned int pu = sd.surface.degree_u, pv = sd.surface.degree_v;
                bool in_u = a.row >= static_cast<size_t>(el.ku - static_cast<int>(pu)) &&
                            a.row <= static_cast<size_t>(el.ku);
                bool in_v = a.col >= static_cast<size_t>(el.kv - static_cast<int>(pv)) &&
                            a.col <= static_cast<size_t>(el.kv);
                if (in_u && in_v) dirty_elements.insert(ei);
            }
        }
        if (dirty_elements.empty()) continue;

        std::unordered_set<int> dirty_nodes;
        for (int ei : dirty_elements)
        {
            Element &el = sd.elements[ei];
            el.net = extractElement(sd, el.span_i, el.span_j);
            el.box = netBox(el.net);
            ++impl_->extractions;
            ++er.reextracted_elements;
            for (int ni = 0; ni < static_cast<int>(sd.nodes.size()); ++ni)
                if (sd.nodes[ni].element == ei)
                {
                    sd.nodes[ni].box = el.box;
                    int p = sd.nodes[ni].parent;
                    while (p >= 0)
                    {
                        dirty_nodes.insert(p);
                        p = sd.nodes[p].parent;
                    }
                    break;
                }
        }
        // Parent slots are reserved before their children, so parent indices
        // are smaller; a descending index walk therefore refits children
        // before parents while the tree topology stays untouched.
        std::vector<int> nodes(dirty_nodes.begin(), dirty_nodes.end());
        std::sort(nodes.begin(), nodes.end(), std::greater<int>());
        for (int ni : nodes)
        {
            TreeNode &node = sd.nodes[ni];
            node.box = sd.nodes[node.left].box;
            node.box.expand(sd.nodes[node.right].box);
            ++er.refit_nodes;
        }
    }
    return er;
}

RaySceneStats RaySurfaceScene::stats() const
{
    RaySceneStats st;
    st.surface_count = impl_->surfaces.size();
    st.element_extractions = impl_->extractions;
    for (const SurfaceData &sd : impl_->surfaces)
    {
        st.total_elements += sd.elements.size();
        st.total_tree_nodes += sd.nodes.size();
    }
    return st;
}

std::size_t RaySurfaceScene::surfaceCount() const { return impl_->surfaces.size(); }

} // namespace tinynurbs
