/**
 * Implementation of direct ray intersection with rational NURBS surfaces.
 *
 * The pipeline is:
 *   1. Validate the supported surface class.
 *   2. Decompose each surface into knot elements and extract their local
 *      homogeneous Bezier control nets (conservative convex-hull bounds).
 *   3. Build a bounding hierarchy over those leaves.
 *   4. For a query: prune hierarchy nodes with a homogeneous slab test that
 *      also carries the ray parameter window, then subdivide the true
 *      parametric patch and polish every root with Newton iteration.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "tinynurbs/core/ray_scene.h"
#include "tinynurbs/tinynurbs.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <tuple>
#include <unordered_set>

namespace tinynurbs
{

namespace
{

using dvec3 = glm::dvec3;
using dvec4 = glm::dvec4;

bool finiteVec3(const dvec3 &v)
{
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

// Move u onto a knot value that lies within parameter_tolerance of it.
void snapToKnot(const std::vector<double> &knots, unsigned int degree, double tol, double &u)
{
    const size_t begin = degree;
    const size_t end = knots.size() - degree;
    for (size_t i = begin; i < end; ++i)
    {
        if (std::abs(knots[i] - u) <= tol)
        {
            u = knots[i];
            return;
        }
    }
}

// ---------------------------------------------------------------------------
// Surface class validation
// ---------------------------------------------------------------------------

bool clampedKnotVector(unsigned int degree, const std::vector<double> &knots)
{
    if (knots.size() < size_t(degree) * 2 + 2) return false;
    const double front = knots.front();
    const double back = knots.back();
    if (!std::isfinite(front) || !std::isfinite(back) || !(back > front)) return false;
    for (unsigned int i = 0; i <= degree; ++i)
    {
        if (knots[i] != front || knots[knots.size() - 1 - i] != back) return false;
    }
    // Interior knots: monotonic, multiplicity <= degree.
    size_t i = degree;
    const size_t last = knots.size() - degree;
    while (i < last)
    {
        size_t j = i + 1;
        while (j < last && knots[j] == knots[i]) ++j;
        if (j - i > degree) return false;
        if (j < last && !(knots[j] > knots[i])) return false;
        i = j;
    }
    return true;
}

bool validateSurface(const RationalSurface3d &srf)
{
    if (srf.degree_u < 1 || srf.degree_u > 3 || srf.degree_v < 1 || srf.degree_v > 3)
        return false;
    if (!clampedKnotVector(srf.degree_u, srf.knots_u) ||
        !clampedKnotVector(srf.degree_v, srf.knots_v))
        return false;
    const size_t nu = srf.control_points.rows();
    const size_t nv = srf.control_points.cols();
    if (nu == 0 || nv == 0) return false;
    if (srf.knots_u.size() != nu + srf.degree_u + 1 ||
        srf.knots_v.size() != nv + srf.degree_v + 1)
        return false;
    if (srf.weights.rows() != nu || srf.weights.cols() != nv) return false;
    for (size_t i = 0; i < nu; ++i)
    {
        for (size_t j = 0; j < nv; ++j)
        {
            if (!finiteVec3(srf.control_points(i, j))) return false;
            const double w = srf.weights(i, j);
            if (!std::isfinite(w) || !(w > 0.0)) return false;
        }
    }
    // Regularity: Su x Sv must not vanish on any element. Sample the quarter
    // points and the midpoint of every knot element.
    for (size_t s = srf.degree_u; s + 1 < srf.knots_u.size() - srf.degree_u; ++s)
    {
        if (!(srf.knots_u[s + 1] > srf.knots_u[s])) continue;
        for (size_t t = srf.degree_v; t + 1 < srf.knots_v.size() - srf.degree_v; ++t)
        {
            if (!(srf.knots_v[t + 1] > srf.knots_v[t])) continue;
            for (int fu = 1; fu <= 3; ++fu)
            {
                const double u =
                    srf.knots_u[s] + (srf.knots_u[s + 1] - srf.knots_u[s]) * fu / 4.0;
                for (int fv = 1; fv <= 3; ++fv)
                {
                    const double v =
                        srf.knots_v[t] + (srf.knots_v[t + 1] - srf.knots_v[t]) * fv / 4.0;
                    array2<dvec3> ders = surfaceDerivatives(srf, 1, u, v);
                    const double jac = glm::length(glm::cross(ders(1, 0), ders(0, 1)));
                    const double scale =
                        1.0 + glm::length(ders(1, 0)) * glm::length(ders(0, 1));
                    if (!(jac > 1e-12 * scale)) return false;
                }
            }
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Bezier extraction on one knot element
// ---------------------------------------------------------------------------

typedef std::vector<std::vector<double>> Matrix;

// Extraction matrix C such that N_j(u) = sum_k C[k][j] B_k(u) on the element
// whose span index is `span`. Local basis indices 0..degree map to global
// control point indices span-degree .. span.
Matrix extractionMatrix(unsigned int degree, const std::vector<double> &knots, int span)
{
    const int n = int(degree) + 1;
    const double a = knots[span];
    const double b = knots[span + 1];
    std::vector<double> sites(n);
    for (int r = 0; r < n; ++r)
    {
        sites[r] = a + (b - a) * (r + 0.5) / n;
    }
    // M[r][k] = Bernstein B_k(site_r), Nmat[r][j] = B-spline basis value.
    std::vector<std::vector<double>> M(n, std::vector<double>(n, 0.0));
    std::vector<std::vector<double>> rhs(n, std::vector<double>(n, 0.0));
    for (int r = 0; r < n; ++r)
    {
        const double s = (sites[r] - a) / (b - a);
        double bern[4];
        for (int k = 0; k < n; ++k)
        {
            bern[k] = double(util::binomial(degree, k)) * std::pow(s, k) *
                      std::pow(1.0 - s, degree - k);
            M[r][k] = bern[k];
        }
        const int site_span = findSpan(degree, knots, sites[r]);
        const std::vector<double> N =
            bsplineBasis(degree, site_span, knots, sites[r]);
        for (int j = 0; j < n; ++j)
        {
            const int global_index = span - int(degree) + j;
            rhs[r][j] = (site_span - int(degree) <= global_index &&
                         global_index <= site_span)
                            ? N[global_index - (site_span - int(degree))]
                            : 0.0;
        }
    }
    // Solve M * C = rhs by Gauss elimination with partial pivoting.
    std::vector<std::vector<double>> aug(n, std::vector<double>(2 * n, 0.0));
    for (int r = 0; r < n; ++r)
    {
        for (int k = 0; k < n; ++k) aug[r][k] = M[r][k];
        for (int k = 0; k < n; ++k) aug[r][n + k] = rhs[r][k];
    }
    for (int col = 0; col < n; ++col)
    {
        int pivot = col;
        for (int r = col + 1; r < n; ++r)
        {
            if (std::abs(aug[r][col]) > std::abs(aug[pivot][col])) pivot = r;
        }
        std::swap(aug[pivot], aug[col]);
        const double piv = aug[col][col];
        for (int k = 0; k < 2 * n; ++k) aug[col][k] /= piv;
        for (int r = 0; r < n; ++r)
        {
            if (r == col) continue;
            const double f = aug[r][col];
            if (f == 0.0) continue;
            for (int k = 0; k < 2 * n; ++k) aug[r][k] -= f * aug[col][k];
        }
    }
    Matrix c(n, std::vector<double>(n, 0.0));
    for (int k = 0; k < n; ++k)
        for (int j = 0; j < n; ++j) c[k][j] = aug[k][n + j];
    return c;
}

array2<dvec4> homogeneousNet(const RationalSurface3d &srf)
{
    array2<dvec4> net(srf.control_points.rows(), srf.control_points.cols());
    for (size_t i = 0; i < srf.control_points.rows(); ++i)
        for (size_t j = 0; j < srf.control_points.cols(); ++j)
            net(i, j) = dvec4(util::cartesianToHomogenous(srf.control_points(i, j),
                                                          srf.weights(i, j)));
    return net;
}

// ---------------------------------------------------------------------------
// Four-dimensional (homogeneous) bounding boxes
// ---------------------------------------------------------------------------

struct Box4
{
    dvec4 lo{dvec4(std::numeric_limits<double>::infinity())};
    dvec4 hi{dvec4(-std::numeric_limits<double>::infinity())};
};

Box4 boxOfNet(const array2<dvec4> &net)
{
    Box4 box;
    for (size_t i = 0; i < net.size(); ++i)
    {
        const dvec4 &p = net[i];
        box.lo = glm::min(box.lo, p);
        box.hi = glm::max(box.hi, p);
    }
    return box;
}

void expandBox(Box4 &box, const Box4 &other)
{
    box.lo = glm::min(box.lo, other.lo);
    box.hi = glm::max(box.hi, other.hi);
}

// Conservative ray test against a homogeneous convex-hull box. The rational
// surface point is S = H.xyz / H.w with H.w > 0; the ray equation S = O + tD
// becomes H.xyz = H.w * (O + tD), and H.w stays inside the box's w interval.
// Returns the feasible ray parameter interval (intersected with [tmin,tmax)).
bool rayHomogeneousBox(const dvec3 &O, const dvec3 &D, const Box4 &box, double tmin,
                       double tmax, double &tlo, double &thi)
{
    double wmin = box.lo.w;
    const double wmax = box.hi.w;
    if (!(wmax > 0.0) || !std::isfinite(wmax)) return false;
    if (!(wmin > 0.0))
    {
        // Extraction coefficients are non-negative convex weights; a
        // non-positive lower w can only be roundoff. Loosen conservatively.
        wmin = std::numeric_limits<double>::min();
    }
    tlo = tmin;
    thi = tmax;
    for (int axis = 0; axis < 3; ++axis)
    {
        const double lo = box.lo[axis];
        const double hi = box.hi[axis];
        struct GI
        {
            double g0, g1;
        };
        GI candidates[2];
        int count = 0;
        // g >= 0 branch: g in [lo/wmax, hi/wmin], intersected with [0, inf).
        const double ap = lo / wmax;
        const double bp = hi / wmin;
        if (ap <= bp && bp >= 0.0) candidates[count++] = {std::max(0.0, ap), bp};
        // g < 0 branch: g in [lo/wmin, hi/wmax], intersected with (-inf, 0].
        const double am = lo / wmin;
        const double bm = hi / wmax;
        if (am <= bm && am <= 0.0) candidates[count++] = {am, std::min(0.0, bm)};
        // The two branches are alternatives (their g-intervals meet at 0);
        // map each to a t interval and keep their union, conservatively
        // enlarged to one enclosing interval for the running window.
        double best_lo = std::numeric_limits<double>::infinity();
        double best_hi = -std::numeric_limits<double>::infinity();
        bool axis_feasible = false;
        const double d = D[axis];
        for (int c = 0; c < count; ++c)
        {
            const double l = candidates[c].g0;
            const double h = candidates[c].g1;
            double a, b;
            if (d == 0.0)
            {
                if (O[axis] < l || O[axis] > h) continue;
                a = tlo;
                b = thi;
            }
            else
            {
                a = (l - O[axis]) / d;
                b = (h - O[axis]) / d;
                if (a > b) std::swap(a, b);
            }
            if (a > thi || b < tlo) continue;
            axis_feasible = true;
            best_lo = std::min(best_lo, a);
            best_hi = std::max(best_hi, b);
        }
        if (!axis_feasible) return false;
        tlo = std::max(tlo, best_lo);
        thi = std::min(thi, best_hi);
        if (tlo > thi) return false;
    }
    return tlo <= thi;
}

// ---------------------------------------------------------------------------
// Homogeneous de Casteljau subdivision
// ---------------------------------------------------------------------------

void splitNetU(const array2<dvec4> &net, unsigned int degree_u, unsigned int degree_v,
               double s, array2<dvec4> &left, array2<dvec4> &right)
{
    const int p = int(degree_u);
    const int q = int(degree_v);
    left.resize(p + 1, q + 1);
    right.resize(p + 1, q + 1);
    for (int col = 0; col <= q; ++col)
    {
        std::vector<dvec4> tmp(p + 1);
        for (int k = 0; k <= p; ++k) tmp[k] = net(k, col);
        left(0, col) = tmp[0];
        right(p, col) = tmp[p];
        for (int r = 1; r <= p; ++r)
        {
            for (int k = 0; k <= p - r; ++k)
                tmp[k] = (1.0 - s) * tmp[k] + s * tmp[k + 1];
            left(r, col) = tmp[0];
            right(p - r, col) = tmp[p - r];
        }
    }
}

void splitNetV(const array2<dvec4> &net, unsigned int degree_u, unsigned int degree_v,
               double s, array2<dvec4> &left, array2<dvec4> &right)
{
    const int p = int(degree_u);
    const int q = int(degree_v);
    left.resize(p + 1, q + 1);
    right.resize(p + 1, q + 1);
    for (int row = 0; row <= p; ++row)
    {
        std::vector<dvec4> tmp(q + 1);
        for (int k = 0; k <= q; ++k) tmp[k] = net(row, k);
        left(row, 0) = tmp[0];
        right(row, q) = tmp[q];
        for (int r = 1; r <= q; ++r)
        {
            for (int k = 0; k <= q - r; ++k)
                tmp[k] = (1.0 - s) * tmp[k] + s * tmp[k + 1];
            left(row, r) = tmp[0];
            right(row, q - r) = tmp[q - r];
        }
    }
}

// ---------------------------------------------------------------------------
// Scene data
// ---------------------------------------------------------------------------

struct Element
{
    unsigned int surface = 0;
    double umin = 0.0, umax = 0.0;
    double vmin = 0.0, vmax = 0.0;
    Matrix cu, cv;
    int row0 = 0, col0 = 0;
    array2<dvec4> net;
    Box4 box;
    int bvh_node = -1;
};

struct BvhNode
{
    Box4 box;
    int left = -1;
    int right = -1;
    int parent = -1;
    int leaf = -1; // >= 0 for a leaf node, -1 for an internal node
};

struct SurfaceData
{
    RationalSurface3d surface;
    array2<dvec4> homo;
    std::vector<int> span_u; // nonzero knot intervals
    std::vector<int> span_v;
};

struct SubBox
{
    array2<dvec4> net;
    double umin, umax, vmin, vmax;
};

} // namespace

struct RaySurfaceScene::Impl
{
    RaySceneError status = RaySceneError::None;
    std::vector<SurfaceData> surfaces;
    std::vector<Element> elements;       // index == leaf id
    std::vector<BvhNode> nodes;
    int root = -1;
    unsigned long long total_extracted = 0;

    explicit Impl(const std::vector<RationalSurface3d> &input)
    {
        surfaces.reserve(input.size());
        for (const RationalSurface3d &srf : input)
        {
            if (!validateSurface(srf))
            {
                status = RaySceneError::InvalidSurface;
                return;
            }
            SurfaceData data;
            data.surface = srf;
            data.homo = homogeneousNet(srf);
            for (size_t s = srf.degree_u; s < srf.knots_u.size() - srf.degree_u; ++s)
                if (srf.knots_u[s + 1] > srf.knots_u[s]) data.span_u.push_back(int(s));
            for (size_t t = srf.degree_v; t < srf.knots_v.size() - srf.degree_v; ++t)
                if (srf.knots_v[t + 1] > srf.knots_v[t]) data.span_v.push_back(int(t));
            surfaces.push_back(std::move(data));
        }
        buildElements();
        buildBvh();
    }

    void extractElement(Element &el)
    {
        const SurfaceData &data = surfaces[el.surface];
        const unsigned int pu = data.surface.degree_u;
        const unsigned int pv = data.surface.degree_v;
        el.net.resize(pu + 1, pv + 1);
        for (int k = 0; k <= int(pu); ++k)
        {
            for (int l = 0; l <= int(pv); ++l)
            {
                dvec4 acc(0.0);
                for (int a = 0; a <= int(pu); ++a)
                {
                    if (el.cu[k][a] == 0.0) continue;
                    for (int b = 0; b <= int(pv); ++b)
                    {
                        if (el.cv[l][b] == 0.0) continue;
                        acc += el.cu[k][a] * el.cv[l][b] *
                               data.homo(el.row0 + a, el.col0 + b);
                    }
                }
                el.net(k, l) = acc;
            }
        }
        el.box = boxOfNet(el.net);
        ++total_extracted;
    }

    void buildElements()
    {
        for (unsigned int si = 0; si < surfaces.size(); ++si)
        {
            SurfaceData &data = surfaces[si];
            const RationalSurface3d &srf = data.surface;
            for (int su : data.span_u)
            {
                Matrix cu = extractionMatrix(srf.degree_u, srf.knots_u, su);
                for (int sv : data.span_v)
                {
                    Matrix cv = extractionMatrix(srf.degree_v, srf.knots_v, sv);
                    Element el;
                    el.surface = si;
                    el.umin = srf.knots_u[su];
                    el.umax = srf.knots_u[su + 1];
                    el.vmin = srf.knots_v[sv];
                    el.vmax = srf.knots_v[sv + 1];
                    el.row0 = su - int(srf.degree_u);
                    el.col0 = sv - int(srf.degree_v);
                    el.cu = std::move(cu);
                    el.cv = std::move(cv);
                    extractElement(el);
                    elements.push_back(std::move(el));
                }
            }
        }
    }

    int buildBvhRecursive(std::vector<int> &leaf_ids, int depth)
    {
        const int node_index = int(nodes.size());
        nodes.emplace_back();
        if (leaf_ids.size() == 1)
        {
            BvhNode &node = nodes[node_index];
            node.leaf = leaf_ids[0];
            node.box = elements[leaf_ids[0]].box;
            elements[leaf_ids[0]].bvh_node = node_index;
            return node_index;
        }
        std::sort(leaf_ids.begin(), leaf_ids.end(), [&](int a, int b) {
            const Element &ea = elements[a];
            const Element &eb = elements[b];
            if (ea.surface != eb.surface) return ea.surface < eb.surface;
            const double ca = 0.5 * (ea.umin + ea.umax);
            const double cb = 0.5 * (eb.umin + eb.umax);
            if (ca != cb) return ca < cb;
            return 0.5 * (ea.vmin + ea.vmax) < 0.5 * (eb.vmin + eb.vmax);
        });
        const size_t mid = leaf_ids.size() / 2;
        std::vector<int> left_ids(leaf_ids.begin(), leaf_ids.begin() + mid);
        std::vector<int> right_ids(leaf_ids.begin() + mid, leaf_ids.end());
        const int left = buildBvhRecursive(left_ids, depth + 1);
        const int right = buildBvhRecursive(right_ids, depth + 1);
        BvhNode &node = nodes[node_index];
        node.left = left;
        node.right = right;
        node.box = nodes[left].box;
        expandBox(node.box, nodes[right].box);
        nodes[left].parent = node_index;
        nodes[right].parent = node_index;
        return node_index;
    }

    void buildBvh()
    {
        std::vector<int> leaf_ids(elements.size());
        for (size_t i = 0; i < elements.size(); ++i) leaf_ids[i] = int(i);
        if (leaf_ids.empty())
        {
            root = -1;
            return;
        }
        root = buildBvhRecursive(leaf_ids, 0);
        nodes[root].parent = -1;
    }

    // -----------------------------------------------------------------------
    // Root finding inside one leaf element
    // -----------------------------------------------------------------------

    // Newton polish constrained to [bumin,bumax] x [bvmin,bvmax]. Returns
    // true only after an independently verified residual check. `transverse`
    // reports whether the root is a genuine transverse crossing; a residual
    // small enough but tangent contact is never accepted as a hit.
    bool newtonRoot(const RationalSurface3d &srf, double bumin, double bumax, double bvmin,
                    double bvmax, const dvec3 &O, const dvec3 &D, double &u, double &v,
                    double &t, dvec3 &position, dvec3 &normal, bool &degenerate,
                    bool &transverse, const RayOptions &opt) const
    {
        const int drop = (std::abs(D.x) >= std::abs(D.y) && std::abs(D.x) >= std::abs(D.z))
                             ? 0
                             : (std::abs(D.y) >= std::abs(D.z) ? 1 : 2);
        const int ax = (drop + 1) % 3;
        const int ay = (drop + 2) % 3;
        const double dlen2 = glm::dot(D, D);
        degenerate = false;
        transverse = false;
        for (int iter = 0; iter < 30; ++iter)
        {
            array2<dvec3> ders = surfaceDerivatives(srf, 1, u, v);
            const dvec3 S = surfacePoint(srf, u, v);
            const dvec3 Su = ders(1, 0);
            const dvec3 Sv = ders(0, 1);
            const dvec3 r = S - O;
            const dvec3 cr = glm::cross(D, r);
            const double resid = glm::length(cr) / std::sqrt(dlen2);
            const dvec3 nrm = glm::cross(Su, Sv);
            if (glm::length(nrm) <= 1e-14 * (1.0 + glm::length(Su) * glm::length(Sv)))
            {
                degenerate = true;
                return false;
            }
            const dvec3 csu = glm::cross(D, Su);
            const dvec3 csv = glm::cross(D, Sv);
            const double J00 = csu[ax], J01 = csv[ax];
            const double J10 = csu[ay], J11 = csv[ay];
            const double det = J00 * J11 - J01 * J10;
            const double jscale = dlen2 * glm::length(Su) * glm::length(Sv);
            const bool jac_ok = std::abs(det) > 1e-12 * (jscale + 1e-300);
            if (!jac_ok) return false; // tangency suspected: caller subdivides
            double fu = cr[ax];
            double fv = cr[ay];
            double du = (fu * J11 - J01 * fv) / det;
            double dv = (J00 * fv - fu * J10) / det;
            // Damped step, clamped to the enclosing parameter box.
            double damp = 1.0;
            bool improved = false;
            for (int shrink = 0; shrink < 8; ++shrink)
            {
                double nu = std::min(bumax, std::max(bumin, u - damp * du));
                double nv = std::min(bvmax, std::max(bvmin, v - damp * dv));
                const dvec3 nS = surfacePoint(srf, nu, nv);
                const double nres =
                    glm::length(glm::cross(D, nS - O)) / std::sqrt(dlen2);
                if (nres < resid)
                {
                    u = nu;
                    v = nv;
                    improved = true;
                    break;
                }
                damp *= 0.5;
            }
            if (!improved) return false;
            array2<dvec3> nd = surfaceDerivatives(srf, 1, u, v);
            const dvec3 nS = surfacePoint(srf, u, v);
            const double nres =
                glm::length(glm::cross(D, nS - O)) / std::sqrt(dlen2);
            const double step = std::hypot(du * damp, dv * damp);
            if (nres <= opt.spatial_tolerance && step <= opt.parameter_tolerance)
            {
                // Independent transversality check at the converged point.
                const dvec3 fsu = glm::cross(D, nd(1, 0));
                const dvec3 fsv = glm::cross(D, nd(0, 1));
                const double fdet =
                    fsu[ax] * fsv[ay] - fsv[ax] * fsu[ay];
                const double fscale =
                    dlen2 * glm::length(nd(1, 0)) * glm::length(nd(0, 1));
                transverse = std::abs(fdet) > 1e-10 * (fscale + 1e-300);
                const dvec3 nr = nS - O;
                t = glm::dot(nr, D) / dlen2;
                position = nS;
                dvec3 nn = glm::cross(nd(1, 0), nd(0, 1)); // Su x Sv
                const double nl = glm::length(nn);
                if (!(nl > 0.0))
                {
                    degenerate = true;
                    return false;
                }
                normal = nn / nl;
                return glm::length(position - (O + t * D)) <= opt.spatial_tolerance;
            }
        }
        return false;
    }

    // Merge overlapping unresolved UV ranges of one surface.
    static void mergeUnresolved(std::vector<RayUnresolved> &records)
    {
        bool changed = true;
        while (changed)
        {
            changed = false;
            for (size_t i = 0; i < records.size() && !changed; ++i)
            {
                for (size_t j = i + 1; j < records.size(); ++j)
                {
                    RayUnresolved &a = records[i];
                    const RayUnresolved &b = records[j];
                    if (a.surface_index != b.surface_index) continue;
                    if (a.u_max < b.u_min || b.u_max < a.u_min ||
                        a.v_max < b.v_min || b.v_max < a.v_min)
                        continue;
                    a.u_min = std::min(a.u_min, b.u_min);
                    a.u_max = std::max(a.u_max, b.u_max);
                    a.v_min = std::min(a.v_min, b.v_min);
                    a.v_max = std::max(a.v_max, b.v_max);
                    if (b.reason == UnresolvedReason::Degenerate ||
                        a.reason == UnresolvedReason::Degenerate)
                        a.reason = UnresolvedReason::Degenerate;
                    records.erase(records.begin() + j);
                    changed = true;
                    break;
                }
            }
        }
    }

    void solveLeaf(const Element &el, const dvec3 &O, const dvec3 &D, double tmin, double tmax,
                   const RayOptions &opt, RayQueryResult &result, bool &budget_exceeded) const
    {
        const RationalSurface3d &srf = surfaces[el.surface].surface;
        if (budget_exceeded)
        {
            RayUnresolved ur;
            ur.surface_index = el.surface;
            ur.u_min = el.umin;
            ur.u_max = el.umax;
            ur.v_min = el.vmin;
            ur.v_max = el.vmax;
            ur.reason = UnresolvedReason::Budget;
            result.unresolved.push_back(ur);
            return;
        }
        std::vector<SubBox> queue;
        std::vector<RayUnresolved> local_unresolved;
        queue.push_back({el.net, el.umin, el.umax, el.vmin, el.vmax});
        const double floor = opt.parameter_tolerance / 256.0;
        while (!queue.empty())
        {
            SubBox cur = std::move(queue.back());
            queue.pop_back();
            const Box4 cur_box = boxOfNet(cur.net);
            double tlo, thi;
            if (!rayHomogeneousBox(O, D, cur_box, tmin, tmax, tlo, thi)) continue;
            if (result.parameter_boxes_visited >= opt.box_budget)
            {
                budget_exceeded = true;
                RayUnresolved ur;
                ur.surface_index = el.surface;
                ur.u_min = el.umin;
                ur.u_max = el.umax;
                ur.v_min = el.vmin;
                ur.v_max = el.vmax;
                ur.reason = UnresolvedReason::Budget;
                result.unresolved.push_back(ur);
                return;
            }
            ++result.parameter_boxes_visited;
            const double duspan = cur.umax - cur.umin;
            const double dvspan = cur.vmax - cur.vmin;
            const bool terminal = duspan <= opt.parameter_tolerance &&
                                  dvspan <= opt.parameter_tolerance;
            if (terminal)
            {
                double u = 0.5 * (cur.umin + cur.umax);
                double v = 0.5 * (cur.vmin + cur.vmax);
                // Fast tangency test: a verified contact with a singular ray
                // Jacobian cannot be a transverse root, so report it directly
                // instead of refining down to the floor.
                array2<dvec3> cd = surfaceDerivatives(srf, 1, u, v);
                const dvec3 cS = surfacePoint(srf, u, v);
                const double c_res =
                    glm::length(glm::cross(D, cS - O)) / std::sqrt(glm::dot(D, D));
                const dvec3 cj0 = glm::cross(D, cd(1, 0));
                const dvec3 cj1 = glm::cross(D, cd(0, 1));
                const double cj_scale =
                    glm::dot(D, D) * glm::length(cd(1, 0)) * glm::length(cd(0, 1));
                bool cj_singular = false;
                {
                    const int drop =
                        (std::abs(D.x) >= std::abs(D.y) && std::abs(D.x) >= std::abs(D.z))
                            ? 0
                            : (std::abs(D.y) >= std::abs(D.z) ? 1 : 2);
                    const int ax = (drop + 1) % 3;
                    const int ay = (drop + 2) % 3;
                    cj_singular =
                        std::abs(cj0[ax] * cj1[ay] - cj1[ax] * cj0[ay]) <=
                        1e-12 * (cj_scale + 1e-300);
                }
                if (c_res <= opt.spatial_tolerance && cj_singular)
                {
                    RayUnresolved ur;
                    ur.surface_index = el.surface;
                    ur.u_min = cur.umin;
                    ur.u_max = cur.umax;
                    ur.v_min = cur.vmin;
                    ur.v_max = cur.vmax;
                    ur.reason = UnresolvedReason::Tangency;
                    local_unresolved.push_back(ur);
                    continue;
                }
                double t = 0.0;
                dvec3 position, normal;
                bool degenerate = false;
                bool transverse = false;
                const bool converged =
                    newtonRoot(srf, cur.umin, cur.umax, cur.vmin, cur.vmax, O, D, u, v, t,
                               position, normal, degenerate, transverse, opt);
                // Half-open ownership: a root on a box's lower internal face
                // is owned by the neighbour box whose upper face it is. A box
                // starting at the element's lower domain edge owns that edge.
                const bool owned_u = u > cur.umin || cur.umin == el.umin;
                const bool owned_v = v > cur.vmin || cur.vmin == el.vmin;
                const bool owned = owned_u && owned_v;
                if (converged && transverse && owned &&
                    (t < tmin || t >= tmax || u < srf.knots_u.front() ||
                     u > srf.knots_u.back() || v < srf.knots_v.front() ||
                     v > srf.knots_v.back()))
                {
                    // Verified contact outside the requested half-open window
                    // or domain: a clean miss, not an unresolved region.
                    continue;
                }
                if (converged && transverse && owned && t >= tmin && t < tmax &&
                    u >= srf.knots_u.front() && u <= srf.knots_u.back() &&
                    v >= srf.knots_v.front() && v <= srf.knots_v.back())
                {
                    RayHit hit;
                    hit.surface_index = el.surface;
                    hit.u = u;
                    hit.v = v;
                    hit.t = t;
                    hit.position = position;
                    hit.normal = normal;
                    result.hits.push_back(hit);
                    continue;
                }
                if (converged && transverse && !owned)
                {
                    // A verified transverse contact on a lower internal
                    // subdivision face: the owning neighbour box handles it.
                    continue;
                }
                if (converged && !transverse && !degenerate)
                {
                    // Verified contact but tangent: never claim it as a hit.
                    // Overlapping records from neighbouring boxes are merged.
                    RayUnresolved ur;
                    ur.surface_index = el.surface;
                    ur.u_min = cur.umin;
                    ur.u_max = cur.umax;
                    ur.v_min = cur.vmin;
                    ur.v_max = cur.vmax;
                    ur.reason = UnresolvedReason::Tangency;
                    local_unresolved.push_back(ur);
                    continue;
                }
                // Not verified: keep tightening while the hull can shrink and
                // the budget allows. Below the floor the region stays
                // unresolved rather than being silently called a miss.
                if (!degenerate && (duspan > floor || dvspan > floor))
                {
                    array2<dvec4> left, right;
                    if (duspan >= dvspan)
                    {
                        const double umid = 0.5 * (cur.umin + cur.umax);
                        const double s = (umid - cur.umin) / duspan;
                        splitNetU(cur.net, srf.degree_u, srf.degree_v, s, left, right);
                        queue.push_back({left, cur.umin, umid, cur.vmin, cur.vmax});
                        queue.push_back({right, umid, cur.umax, cur.vmin, cur.vmax});
                    }
                    else
                    {
                        const double vmid = 0.5 * (cur.vmin + cur.vmax);
                        const double s = (vmid - cur.vmin) / dvspan;
                        splitNetV(cur.net, srf.degree_u, srf.degree_v, s, left, right);
                        queue.push_back({left, cur.umin, cur.umax, cur.vmin, vmid});
                        queue.push_back({right, cur.umin, cur.umax, vmid, cur.vmax});
                    }
                    continue;
                }
                RayUnresolved ur;
                ur.surface_index = el.surface;
                ur.u_min = cur.umin;
                ur.u_max = cur.umax;
                ur.v_min = cur.vmin;
                ur.v_max = cur.vmax;
                ur.reason = degenerate ? UnresolvedReason::Degenerate
                                       : UnresolvedReason::Tangency;
                local_unresolved.push_back(ur);
                continue;
            }
            array2<dvec4> left, right;
            if (duspan >= dvspan)
            {
                const double umid = 0.5 * (cur.umin + cur.umax);
                const double s = (umid - cur.umin) / duspan;
                splitNetU(cur.net, srf.degree_u, srf.degree_v, s, left, right);
                queue.push_back({left, cur.umin, umid, cur.vmin, cur.vmax});
                queue.push_back({right, umid, cur.umax, cur.vmin, cur.vmax});
            }
            else
            {
                const double vmid = 0.5 * (cur.vmin + cur.vmax);
                const double s = (vmid - cur.vmin) / dvspan;
                splitNetV(cur.net, srf.degree_u, srf.degree_v, s, left, right);
                queue.push_back({left, cur.umin, cur.umax, cur.vmin, vmid});
                queue.push_back({right, cur.umin, cur.umax, vmid, cur.vmax});
            }
        }
        mergeUnresolved(local_unresolved);
        for (const RayUnresolved &ur : local_unresolved) result.unresolved.push_back(ur);
    }

    void traverse(int node_index, const dvec3 &O, const dvec3 &D, double tmin, double tmax,
                  const RayOptions &opt, RayQueryResult &result, bool &budget_exceeded) const
    {
        double tlo, thi;
        if (!rayHomogeneousBox(O, D, nodes[node_index].box, tmin, tmax, tlo, thi)) return;
        ++result.bvh_nodes_visited;
        const BvhNode &node = nodes[node_index];
        if (node.leaf >= 0)
        {
            solveLeaf(elements[node.leaf], O, D, tmin, tmax, opt, result, budget_exceeded);
            return;
        }
        // The full hierarchy is always walked; the parameter-box budget is
        // enforced inside the leaves and unresolved leaves are reported.
        traverse(node.left, O, D, tmin, tmax, opt, result, budget_exceeded);
        traverse(node.right, O, D, tmin, tmax, opt, result, budget_exceeded);
    }

    RayQueryResult query(const dvec3 &O, const dvec3 &D, double tmin, double tmax,
                         const RayOptions &opt) const
    {
        RayQueryResult result;
        if (!finiteVec3(O) || !finiteVec3(D) || D == dvec3(0.0))
        {
            result.error = RaySceneError::InvalidRay;
            return result;
        }
        if (!std::isfinite(tmin) || !std::isfinite(tmax) || !(tmin < tmax))
        {
            result.error = RaySceneError::InvalidInterval;
            return result;
        }
        if (!std::isfinite(opt.spatial_tolerance) || !(opt.spatial_tolerance > 0.0) ||
            !std::isfinite(opt.parameter_tolerance) || !(opt.parameter_tolerance > 0.0))
        {
            result.error = RaySceneError::InvalidTolerance;
            return result;
        }
        if (opt.box_budget == 0)
        {
            result.error = RaySceneError::InvalidBudget;
            return result;
        }
        if (root < 0)
        {
            result.complete = true;
            return result;
        }
        bool budget_exceeded = false;
        traverse(root, O, D, tmin, tmax, opt, result, budget_exceeded);
        // Snap roots that landed on a knot line, then re-evaluate the point
        // and the Su x Sv normal there. findSpan() evaluates an interior knot
        // with the right element and the upper domain bound with the left one,
        // which is exactly the required convention. Hits whose back-substituted
        // point leaves the spatial tolerance are dropped rather than reported.
        std::vector<RayHit> snapped;
        snapped.reserve(result.hits.size());
        for (RayHit &hit : result.hits)
        {
            const RationalSurface3d &srf = surfaces[hit.surface_index].surface;
            snapToKnot(srf.knots_u, srf.degree_u, opt.parameter_tolerance, hit.u);
            snapToKnot(srf.knots_v, srf.degree_v, opt.parameter_tolerance, hit.v);
            const dvec3 S = surfacePoint(srf, hit.u, hit.v);
            const double new_t = glm::dot(S - O, D) / glm::dot(D, D);
            if (glm::length(S - (O + new_t * D)) > opt.spatial_tolerance) continue;
            hit.t = new_t;
            hit.position = S;
            array2<dvec3> ders = surfaceDerivatives(srf, 1, hit.u, hit.v);
            const dvec3 nn = glm::cross(ders(1, 0), ders(0, 1));
            const double nl = glm::length(nn);
            if (nl > 0.0) hit.normal = nn / nl;
            snapped.push_back(hit);
        }
        result.hits = std::move(snapped);
        // Merge roots reached twice from two elements or two subdivision
        // faces; distinct UV roots (even when spatially coincident) stay.
        std::vector<RayHit> merged;
        std::sort(result.hits.begin(), result.hits.end(),
                  [](const RayHit &a, const RayHit &b) {
                      if (a.t != b.t) return a.t < b.t;
                      if (a.surface_index != b.surface_index)
                          return a.surface_index < b.surface_index;
                      if (a.u != b.u) return a.u < b.u;
                      return a.v < b.v;
                  });
        for (const RayHit &hit : result.hits)
        {
            bool duplicate = false;
            for (RayHit &kept : merged)
            {
                if (kept.surface_index != hit.surface_index) continue;
                if (std::abs(kept.u - hit.u) <= opt.parameter_tolerance &&
                    std::abs(kept.v - hit.v) <= opt.parameter_tolerance)
                {
                    // Same parametric root reached from two elements or two
                    // subdivision faces. Keep the lower-residual copy.
                    const double r_kept = glm::length(kept.position - (O + kept.t * D));
                    const double r_hit = glm::length(hit.position - (O + hit.t * D));
                    if (r_hit < r_kept) kept = hit;
                    duplicate = true;
                    break;
                }
            }
            if (!duplicate) merged.push_back(hit);
        }
        result.hits = std::move(merged);
        // Enforce the half-open ray window once more after t recomputation.
        result.hits.erase(std::remove_if(result.hits.begin(), result.hits.end(),
                                         [&](const RayHit &h) {
                                             return h.t < tmin || h.t >= tmax;
                                         }),
                          result.hits.end());
        std::sort(result.hits.begin(), result.hits.end(),
                  [](const RayHit &a, const RayHit &b) {
                      if (a.t != b.t) return a.t < b.t;
                      if (a.surface_index != b.surface_index)
                          return a.surface_index < b.surface_index;
                      if (a.u != b.u) return a.u < b.u;
                      return a.v < b.v;
                  });
        mergeUnresolved(result.unresolved);
        result.boxes_visited = result.bvh_nodes_visited + result.parameter_boxes_visited;
        result.complete = result.unresolved.empty();
        return result;
    }

    RaySceneUpdateStats applyUpdates(const std::vector<ControlPointEdit> &edits)
    {
        RaySceneUpdateStats stats;
        if (edits.empty())
        {
            stats.accepted = true;
            return stats;
        }
        // Validate the whole batch before touching anything.
        std::set<std::tuple<unsigned int, unsigned int, unsigned int>> seen;
        for (const ControlPointEdit &e : edits)
        {
            if (e.surface_index >= surfaces.size())
            {
                stats.error = RaySceneError::InvalidControlPoint;
                return stats;
            }
            const RationalSurface3d &srf = surfaces[e.surface_index].surface;
            if (e.row >= srf.control_points.rows() || e.column >= srf.control_points.cols() ||
                !finiteVec3(e.position) || !std::isfinite(e.weight) || !(e.weight > 0.0))
            {
                stats.error = RaySceneError::InvalidControlPoint;
                return stats;
            }
            if (!seen.insert(std::make_tuple(e.surface_index, e.row, e.column)).second)
            {
                stats.error = RaySceneError::DuplicateControlPoint;
                return stats;
            }
        }
        // Apply absolute values.
        for (const ControlPointEdit &e : edits)
        {
            SurfaceData &data = surfaces[e.surface_index];
            data.surface.control_points(e.row, e.column) = e.position;
            data.surface.weights(e.row, e.column) = e.weight;
            data.homo(e.row, e.column) =
                dvec4(util::cartesianToHomogenous(e.position, e.weight));
        }
        // Basis support: control point (i, j) influences elements whose spans
        // satisfy span_p in [i, i+p] (u) and span_q in [j, j+q] (v).
        std::unordered_set<size_t> affected_leaves;
        for (const ControlPointEdit &e : edits)
        {
            SurfaceData &data = surfaces[e.surface_index];
            const RationalSurface3d &srf = data.surface;
            const int pu = int(srf.degree_u);
            const int pv = int(srf.degree_v);
            for (size_t li = 0; li < elements.size(); ++li)
            {
                Element &el = elements[li];
                if (el.surface != e.surface_index) continue;
                const int su = el.row0 + pu; // span index used at extraction
                const int sv = el.col0 + pv;
                if (su >= int(e.row) && su <= int(e.row) + pu &&
                    sv >= int(e.column) && sv <= int(e.column) + pv)
                {
                    affected_leaves.insert(li);
                }
            }
        }
        std::vector<char> node_dirty(nodes.size(), 0);
        for (size_t li : affected_leaves)
        {
            extractElement(elements[li]);
            ++stats.elements_extracted;
            for (int ni = elements[li].bvh_node; ni >= 0; ni = nodes[ni].parent)
                node_dirty[ni] = 1;
        }
        // Children always carry higher indices than their parent (depth-first
        // build), so a reverse sweep refits bottom-up.
        for (int ni = int(nodes.size()) - 1; ni >= 0; --ni)
        {
            if (!node_dirty[ni]) continue;
            BvhNode &node = nodes[ni];
            if (node.leaf >= 0)
            {
                node.box = elements[node.leaf].box;
            }
            else
            {
                node.box = nodes[node.left].box;
                expandBox(node.box, nodes[node.right].box);
                ++stats.bvh_nodes_refit;
            }
        }
        stats.accepted = true;
        return stats;
    }
};

RaySurfaceScene::RaySurfaceScene(const std::vector<RationalSurface3d> &surfaces)
    : impl_(std::make_unique<Impl>(surfaces))
{
}

RaySurfaceScene::~RaySurfaceScene() = default;
RaySurfaceScene::RaySurfaceScene(RaySurfaceScene &&) noexcept = default;
RaySurfaceScene &RaySurfaceScene::operator=(RaySurfaceScene &&) noexcept = default;

RaySceneError RaySurfaceScene::status() const { return impl_->status; }

std::size_t RaySurfaceScene::surfaceCount() const { return impl_->surfaces.size(); }

std::size_t RaySurfaceScene::elementCount(std::size_t surface_index) const
{
    if (surface_index >= impl_->surfaces.size()) return 0;
    const SurfaceData &data = impl_->surfaces[surface_index];
    return data.span_u.size() * data.span_v.size();
}

std::size_t RaySurfaceScene::leafCount() const { return impl_->elements.size(); }

std::size_t RaySurfaceScene::nodeCount() const { return impl_->nodes.size(); }

unsigned long long RaySurfaceScene::totalElementsExtracted() const
{
    return impl_->total_extracted;
}

RayQueryResult RaySurfaceScene::intersect(const glm::dvec3 &origin, const glm::dvec3 &direction,
                                          double tmin, double tmax,
                                          const RayOptions &options) const
{
    if (impl_->status != RaySceneError::None)
    {
        RayQueryResult result;
        result.error = impl_->status;
        return result;
    }
    return impl_->query(origin, direction, tmin, tmax, options);
}

RaySceneUpdateStats
RaySurfaceScene::updateControlPoints(const std::vector<ControlPointEdit> &edits)
{
    RaySceneUpdateStats stats;
    if (impl_->status != RaySceneError::None)
    {
        stats.error = impl_->status;
        return stats;
    }
    return impl_->applyUpdates(edits);
}

} // namespace tinynurbs
