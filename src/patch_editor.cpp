/**
 * Implementation of the rational patch network editor.
 *
 * Mathematics
 * -----------
 * Every patch is stored in homogeneous form H(u,v) = (X(u,v), W(u,v)) with
 * fixed weights (the weights given to the constructor are never modified).
 * Because the weights are fixed, W is a known polynomial and X is affine in
 * the unknown Cartesian control points P.  The rational surface point is
 * S = X / W.
 *
 * A seam between patches A and B imposes, for every normalized along-edge
 * parameter t in [0,1]:
 *
 *     A(t) = B(t)                       (position)
 *     DA(t) + DB(t) = 0                 (cross derivative)
 *
 * where DA / DB are the derivatives with respect to the normalized transverse
 * parameter, measured from each boundary into the interior of its own patch,
 * and B is evaluated at 1-t when the seam is declared reversed.
 *
 * Cross multiplying the position condition by the (strictly positive)
 * denominators WA*WB gives the polynomial identity
 *
 *     XA*WB - XB*WA = 0.
 *
 * The cross derivative condition, multiplied by WA^2*WB^2, reduces with the
 * position identity to the polynomial identity
 *
 *     rA*(XpA*WB - XB*WpA) + rB*(XpB*WA - XA*WpB) = 0,
 *
 * where Xp / Wp are transverse parametric derivatives and rA / rB are the
 * signed parametric lengths of the inward transverse direction.  Both
 * conditions are polynomial of degree at most 2p on every span of a common
 * knot segmentation, so they are imposed by requiring all power-basis
 * coefficients on every common span to vanish.
 *
 * All constraints are therefore linear equalities C*P = b.  The editor
 * computes the orthogonal projection of the fixed rest configuration onto
 * that affine set, i.e. the minimum squared Cartesian displacement solution.
 * The coefficient matrix C is the same for all three coordinates, so it is
 * factored once (rank-revealing, row-pivoted modified Gram-Schmidt) and used
 * for the three right-hand sides.
 *
 * Use of this source code is governed by a BSD-style license that can be found in
 * the LICENSE file.
 */

#include "tinynurbs/core/patch_editor.h"

#include "tinynurbs/core/basis.h"
#include "tinynurbs/core/check.h"
#include "tinynurbs/core/modify.h"
#include "tinynurbs/util/array2.h"
#include "tinynurbs/util/util.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace tinynurbs
{

namespace
{

using dvec3 = glm::dvec3;
using dvec4 = glm::dvec4;

// Rows are normalized to unit length before orthogonalization.  A normalized
// pivot below this tolerance is treated as a dependent equation.
constexpr double kRankTolerance = 1e-11;
// Relative residual above which a well formed system is declared
// incompatible.
constexpr double kConsistencyTolerance = 1e-8;
// Two interior break points closer than this in normalized seam coordinates
// are considered the same station.
constexpr double kNormalizedBreakTolerance = 1e-9;

bool isFinite(double v) { return std::isfinite(v); }

bool isFinite(const dvec3 &v) { return isFinite(v.x) && isFinite(v.y) && isFinite(v.z); }

double scaledTolerance(double a, double b)
{
    return 1e-12 * std::max(1.0, std::max(std::abs(a), std::abs(b)));
}

std::pair<double, double> effectiveDomain(const std::vector<double> &knots, unsigned int degree)
{
    const std::size_t num_cp = knots.size() - degree - 1;
    return std::make_pair(knots[degree], knots[num_cp]);
}

struct PatchData
{
    unsigned int degree_u = 0, degree_v = 0;
    std::vector<double> knots_u, knots_v;
    array2<dvec4> hw;   // homogeneous control points (w*P, w)
    array2<double> w;   // fixed weights
    array2<dvec3> rest; // Cartesian rest control points (compatible basis)
    std::vector<double> original_knots_u, original_knots_v;
    double original_u0 = 0, original_u1 = 0;
    double original_v0 = 0, original_v1 = 0;
    int rows = 0, cols = 0;
    int cp_base = 0; // offset into the global control-point numbering

    int lin(int i, int j) const { return i * cols + j; }
    double weightLin(int l) const { return w(l / cols, l % cols); }

    std::pair<double, double> uDomain() const { return effectiveDomain(knots_u, degree_u); }
    std::pair<double, double> vDomain() const { return effectiveDomain(knots_v, degree_v); }
};

bool isValidEdgeValue(int v) { return v >= 0 && v <= 3; }

void require(bool condition, const std::string &message)
{
    if (!condition)
    {
        throw std::invalid_argument("PatchEditor: " + message);
    }
}

void validateSurface(const RationalSurface3d &srf)
{
    require(srf.degree_u >= 1 && srf.degree_u <= 3, "patch degree_u must be in [1,3]");
    require(srf.degree_v >= 1 && srf.degree_v <= 3, "patch degree_v must be in [1,3]");

    const int rows = static_cast<int>(srf.control_points.rows());
    const int cols = static_cast<int>(srf.control_points.cols());
    require(rows >= 2 && cols >= 2, "patch must have at least a 2x2 control grid");

    require(srf.knots_u.size() == static_cast<std::size_t>(rows) + srf.degree_u + 1,
            "knots_u size does not match control points and degree");
    require(srf.knots_v.size() == static_cast<std::size_t>(cols) + srf.degree_v + 1,
            "knots_v size does not match control points and degree");

    auto checkKnots = [](const std::vector<double> &knots, unsigned int degree,
                         const char *name) {
        for (double k : knots)
        {
            require(isFinite(k), std::string(name) + " contains a non-finite knot");
        }
        for (std::size_t i = 1; i < knots.size(); ++i)
        {
            require(knots[i] >= knots[i - 1], std::string(name) + " must be non-decreasing");
        }
        const std::size_t size = knots.size();
        for (unsigned int i = 0; i <= degree; ++i)
        {
            require(std::abs(knots[i] - knots[0]) <= scaledTolerance(knots[i], knots[0]),
                    std::string(name) + " is not clamped at the start");
            require(std::abs(knots[size - 1 - i] - knots[size - 1]) <=
                        scaledTolerance(knots[size - 1 - i], knots[size - 1]),
                    std::string(name) + " is not clamped at the end");
        }
        const double lo = knots[degree];
        const double hi = knots[size - degree - 1];
        require(hi > lo, std::string(name) + " has an empty effective domain");

        for (std::size_t i = 0; i < size;)
        {
            std::size_t j = i;
            while (j < size &&
                   std::abs(knots[j] - knots[i]) <= scaledTolerance(knots[j], knots[i]))
            {
                ++j;
            }
            const double v = knots[i];
            if (v > lo + scaledTolerance(v, lo) && v < hi - scaledTolerance(v, hi))
            {
                require(j - i <= degree,
                        std::string(name) +
                            " has interior knot multiplicity greater than the degree");
            }
            i = j;
        }
    };

    checkKnots(srf.knots_u, srf.degree_u, "knots_u");
    checkKnots(srf.knots_v, srf.degree_v, "knots_v");

    require(srf.weights.rows() == srf.control_points.rows() &&
                srf.weights.cols() == srf.control_points.cols(),
            "weights dimensions do not match control points");

    dvec3 lo(std::numeric_limits<double>::infinity());
    dvec3 hi(-std::numeric_limits<double>::infinity());
    for (int i = 0; i < rows; ++i)
    {
        for (int j = 0; j < cols; ++j)
        {
            const dvec3 &p = srf.control_points(i, j);
            require(isFinite(p), "control point coordinates must be finite");
            const double w = srf.weights(i, j);
            require(isFinite(w), "weights must be finite");
            require(w > 0.0, "weights must be strictly positive");
            lo = glm::min(lo, p);
            hi = glm::max(hi, p);
        }
    }
    const double extent = glm::length(hi - lo);
    const double scale = std::max(1.0, std::max(std::abs(hi.x), std::abs(lo.x)));
    require(extent > 1e-12 * scale, "patch is degenerate (all control points coincide)");
}

struct BreakPoint
{
    double t;    // normalized to [0,1]
    double val;  // parameter value
    int mult;
};

std::vector<BreakPoint> collectBreaks(const std::vector<double> &knots, double lo, double hi)
{
    std::vector<BreakPoint> out;
    const std::size_t size = knots.size();
    for (std::size_t i = 0; i < size;)
    {
        std::size_t j = i;
        while (j < size && std::abs(knots[j] - knots[i]) <= scaledTolerance(knots[j], knots[i]))
        {
            ++j;
        }
        const double v = knots[i];
        const double tol = scaledTolerance(v, lo);
        if (v > lo + tol && v < hi - tol)
        {
            out.push_back({(v - lo) / (hi - lo), v, static_cast<int>(j - i)});
        }
        i = j;
    }
    return out;
}

void addTarget(std::vector<std::pair<double, int>> &targets, double value, int mult, double tol)
{
    for (auto &t : targets)
    {
        if (std::abs(t.first - value) <= tol)
        {
            t.second = std::max(t.second, mult);
            return;
        }
    }
    targets.emplace_back(value, mult);
}

bool snapToKnot(const std::vector<double> &knots, double &value, double tol)
{
    bool found = false;
    double best = value;
    double bestDist = tol;
    for (double k : knots)
    {
        const double d = std::abs(k - value);
        if (d <= bestDist)
        {
            bestDist = d;
            best = k;
            found = true;
        }
    }
    if (found)
    {
        value = best;
    }
    return found;
}

int multiplicityWithin(const std::vector<double> &knots, double value, double tol)
{
    int mult = 0;
    for (double k : knots)
    {
        if (std::abs(k - value) <= tol)
        {
            ++mult;
        }
    }
    return mult;
}

// Inverse Vandermonde matrix for the collocation points (q+1)/(p+2), q=0..p.
std::vector<std::vector<double>> vandermondeInverse(int p)
{
    const int n = p + 1;
    std::vector<std::vector<double>> v(n, std::vector<double>(n, 0.0));
    for (int q = 0; q < n; ++q)
    {
        const double xi = (q + 1.0) / (p + 2.0);
        double pow = 1.0;
        for (int a = 0; a < n; ++a)
        {
            v[q][a] = pow;
            pow *= xi;
        }
    }
    std::vector<std::vector<double>> inv(n, std::vector<double>(n, 0.0));
    for (int i = 0; i < n; ++i)
    {
        inv[i][i] = 1.0;
    }
    for (int col = 0; col < n; ++col)
    {
        int pivot = col;
        for (int r = col + 1; r < n; ++r)
        {
            if (std::abs(v[r][col]) > std::abs(v[pivot][col]))
            {
                pivot = r;
            }
        }
        if (std::abs(v[pivot][col]) < 1e-14)
        {
            throw std::runtime_error("PatchEditor: singular basis collocation");
        }
        std::swap(v[col], v[pivot]);
        std::swap(inv[col], inv[pivot]);
        const double d = v[col][col];
        for (int a = 0; a < n; ++a)
        {
            v[col][a] /= d;
            inv[col][a] /= d;
        }
        for (int r = 0; r < n; ++r)
        {
            if (r == col)
            {
                continue;
            }
            const double f = v[r][col];
            if (f == 0.0)
            {
                continue;
            }
            for (int a = 0; a < n; ++a)
            {
                v[r][a] -= f * v[col][a];
                inv[r][a] -= f * inv[col][a];
            }
        }
    }
    return inv;
}

const std::vector<std::vector<double>> &vandermondeInverseCached(int p)
{
    static const std::vector<std::vector<double>> inv1 = vandermondeInverse(1);
    static const std::vector<std::vector<double>> inv2 = vandermondeInverse(2);
    static const std::vector<std::vector<double>> inv3 = vandermondeInverse(3);
    if (p <= 1)
    {
        return inv1;
    }
    if (p == 2)
    {
        return inv2;
    }
    return inv3;
}

// Power-basis coefficients, in the local coordinate xi in [0,1], of the
// along-edge B-spline basis functions on the span
// tau(xi) = tau0 + xi*(tau1-tau0).
void basisSpanInfo(unsigned int p, const std::vector<double> &knots, double tau0, double tau1,
                   std::vector<int> &idx, std::vector<std::vector<double>> &coef)
{
    const int n = static_cast<int>(p) + 1;
    const double mid = 0.5 * (tau0 + tau1);
    const int span = findSpan(p, knots, mid);
    idx.resize(static_cast<std::size_t>(n));
    for (int q = 0; q < n; ++q)
    {
        idx[static_cast<std::size_t>(q)] = span - static_cast<int>(p) + q;
    }
    const auto &vinv = vandermondeInverseCached(static_cast<int>(p));
    std::vector<double> xi(static_cast<std::size_t>(n)), value(static_cast<std::size_t>(n));
    for (int q = 0; q < n; ++q)
    {
        xi[static_cast<std::size_t>(q)] = (q + 1.0) / (p + 2.0);
    }
    std::vector<std::vector<double>> samples(static_cast<std::size_t>(n),
                                             std::vector<double>(static_cast<std::size_t>(n), 0.0));
    for (int s = 0; s < n; ++s)
    {
        const double tau = tau0 + xi[static_cast<std::size_t>(s)] * (tau1 - tau0);
        samples[static_cast<std::size_t>(s)] = bsplineBasis(p, span, knots, tau);
    }
    coef.assign(static_cast<std::size_t>(n),
                std::vector<double>(static_cast<std::size_t>(n), 0.0));
    for (int q = 0; q < n; ++q)
    {
        for (int s = 0; s < n; ++s)
        {
            value[static_cast<std::size_t>(s)] =
                samples[static_cast<std::size_t>(s)][static_cast<std::size_t>(q)];
        }
        for (int a = 0; a < n; ++a)
        {
            double c = 0.0;
            for (int s = 0; s < n; ++s)
            {
                c += vinv[static_cast<std::size_t>(a)][static_cast<std::size_t>(s)] *
                     value[static_cast<std::size_t>(s)];
            }
            coef[static_cast<std::size_t>(q)][static_cast<std::size_t>(a)] = c;
        }
    }
}

using Term = std::pair<int, double>; // (global control-point index, coefficient)

void addTerms(std::vector<double> &row, const std::vector<Term> &terms, double scale)
{
    if (scale == 0.0)
    {
        return;
    }
    for (const Term &t : terms)
    {
        row[static_cast<std::size_t>(t.first)] += scale * t.second;
    }
}

double dot(const std::vector<double> &a, const std::vector<double> &b)
{
    double s = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i)
    {
        s += a[i] * b[i];
    }
    return s;
}

double norm2(const std::vector<double> &a)
{
    double s = 0.0;
    for (double v : a)
    {
        s += v * v;
    }
    return s;
}

struct HandleRowInfo
{
    int row = 0;
    double denominator = 0.0; // W(u,v)
    int patch = 0;
    double u = 0.0;
    double v = 0.0;
    int id = 0;
};

struct QRFactor
{
    int n = 0;
    int m = 0;
    int rank = 0;
    std::vector<std::vector<double>> q;     // rank orthonormal vectors of length n
    std::vector<std::vector<double>> coeff; // m vectors of length maxRank
    std::vector<int> perm;                  // perm[k] = original row index
    std::vector<double> invnorm;            // m
    std::array<std::vector<double>, 3> row_dot_rest;
    std::vector<HandleRowInfo> handles;

    // Solve C*(rest + z) = b for one coordinate: C z = b - C*rest.
    std::vector<double> solve(const std::vector<double> &rhs_unscaled, int comp) const
    {
        std::vector<double> bp(static_cast<std::size_t>(m), 0.0);
        for (int k = 0; k < m; ++k)
        {
            const int orig = perm[static_cast<std::size_t>(k)];
            const double b = rhs_unscaled[static_cast<std::size_t>(orig)];
            bp[static_cast<std::size_t>(k)] =
                (b - row_dot_rest[static_cast<std::size_t>(comp)]
                                    [static_cast<std::size_t>(orig)]) *
                invnorm[static_cast<std::size_t>(orig)];
        }
        std::vector<double> y(static_cast<std::size_t>(rank), 0.0);
        for (int k = 0; k < rank; ++k)
        {
            double s = bp[static_cast<std::size_t>(k)];
            for (int j = 0; j < k; ++j)
            {
                s -= coeff[static_cast<std::size_t>(k)][static_cast<std::size_t>(j)] *
                     y[static_cast<std::size_t>(j)];
            }
            const double d = coeff[static_cast<std::size_t>(k)][static_cast<std::size_t>(k)];
            if (!(std::abs(d) > 0.0) || !isFinite(d))
            {
                throw std::runtime_error("PatchEditor: singular constraint factorization");
            }
            y[static_cast<std::size_t>(k)] = s / d;
        }
        double max_residual = 0.0;
        double bp_scale = 1.0;
        for (int k = 0; k < m; ++k)
        {
            bp_scale = std::max(bp_scale, std::abs(bp[static_cast<std::size_t>(k)]));
        }
        for (int i = 0; i < m; ++i)
        {
            double s = -bp[static_cast<std::size_t>(i)];
            for (int j = 0; j < rank; ++j)
            {
                s += coeff[static_cast<std::size_t>(i)][static_cast<std::size_t>(j)] *
                     y[static_cast<std::size_t>(j)];
            }
            max_residual = std::max(max_residual, std::abs(s));
        }
        if (max_residual > kConsistencyTolerance * bp_scale)
        {
            throw std::runtime_error(
                "PatchEditor: seam and handle constraints are incompatible");
        }
        std::vector<double> z(static_cast<std::size_t>(n), 0.0);
        for (int j = 0; j < rank; ++j)
        {
            const double yj = y[static_cast<std::size_t>(j)];
            const std::vector<double> &qj = q[static_cast<std::size_t>(j)];
            for (int t = 0; t < n; ++t)
            {
                z[static_cast<std::size_t>(t)] += yj * qj[static_cast<std::size_t>(t)];
            }
        }
        return z;
    }
};

struct HandleKey
{
    int id = 0;
    int patch = 0;
    double u = 0.0;
    double v = 0.0;
    bool operator==(const HandleKey &other) const
    {
        return id == other.id && patch == other.patch && u == other.u && v == other.v;
    }
    bool operator<(const HandleKey &other) const
    {
        if (id != other.id)
        {
            return id < other.id;
        }
        if (patch != other.patch)
        {
            return patch < other.patch;
        }
        if (u != other.u)
        {
            return u < other.u;
        }
        return v < other.v;
    }
};

// Describes the handle *set*; targets deliberately do not participate so that
// re-targeting reuses the factorization.
struct CacheKey
{
    int seam_version = 0;
    std::vector<HandleKey> handles;
    bool operator==(const CacheKey &other) const
    {
        return seam_version == other.seam_version && handles == other.handles;
    }
};

} // namespace

/////////////////////////////////////////////////////////////////////////////

struct PatchEditor::Impl
{
    std::vector<PatchData> patches;
    std::vector<PatchSeam> seams;
    std::vector<std::vector<double>> original_knots_u;
    std::vector<std::vector<double>> original_knots_v;
    std::array<std::vector<double>, 3> rest; // per coordinate, global cp order
    std::vector<std::vector<double>> seam_rows;
    std::size_t total_control_points = 0;

    std::vector<PatchHandle> staged_handles;
    std::vector<PatchHandle> committed_handles;
    PatchEditorSnapshot committed;

    std::size_t factorizations = 0;
    bool has_cache = false;
    CacheKey cache_key;
    QRFactor cache;

    // ------------------------------------------------------------------
    // Input validation
    // ------------------------------------------------------------------
    void validateHandleList(const std::vector<PatchHandle> &handles) const
    {
        const double eps = 1e-12;
        for (std::size_t i = 0; i < handles.size(); ++i)
        {
            const PatchHandle &h = handles[i];
            require(h.patch >= 0 && static_cast<std::size_t>(h.patch) < patches.size(),
                    "handle patch index out of range");
            require(isFinite(h.u) && isFinite(h.v), "handle parameters must be finite");
            require(isFinite(h.target), "handle target must be finite");
            const PatchData &p = patches[static_cast<std::size_t>(h.patch)];
            const double ulen = p.original_u1 - p.original_u0;
            const double vlen = p.original_v1 - p.original_v0;
            const double utol = eps * std::max(1.0, std::abs(ulen));
            const double vtol = eps * std::max(1.0, std::abs(vlen));
            require(h.u >= p.original_u0 - utol && h.u <= p.original_u1 + utol,
                    "handle u is outside the patch's effective domain");
            require(h.v >= p.original_v0 - vtol && h.v <= p.original_v1 + vtol,
                    "handle v is outside the patch's effective domain");
            for (std::size_t j = 0; j < i; ++j)
            {
                require(handles[j].id != h.id, "duplicate handle id");
                const PatchHandle &g = handles[j];
                if (g.patch == h.patch && std::abs(g.u - h.u) <= utol &&
                    std::abs(g.v - h.v) <= vtol)
                {
                    const double diff =
                        std::max(std::max(std::abs(g.target.x - h.target.x),
                                          std::abs(g.target.y - h.target.y)),
                                 std::abs(g.target.z - h.target.z));
                    const double scale = std::max(
                        1.0, std::max(std::max(std::abs(g.target.x), std::abs(h.target.x)),
                                      std::max(std::abs(g.target.y), std::abs(h.target.y))));
                    require(diff <= eps * scale,
                            "two handles constrain the same surface point to different targets");
                }
            }
        }
    }

    void validateSeams() const
    {
        std::vector<std::pair<int, int>> used;
        for (const PatchSeam &seam : seams)
        {
            require(seam.patch_a >= 0 && static_cast<std::size_t>(seam.patch_a) < patches.size(),
                    "seam patch_a index out of range");
            require(seam.patch_b >= 0 && static_cast<std::size_t>(seam.patch_b) < patches.size(),
                    "seam patch_b index out of range");
            require(isValidEdgeValue(static_cast<int>(seam.edge_a)), "invalid seam edge_a");
            require(isValidEdgeValue(static_cast<int>(seam.edge_b)), "invalid seam edge_b");
            auto mark = [&used](int patch, int edge) {
                for (const std::pair<int, int> &u : used)
                {
                    if (u.first == patch && u.second == edge)
                    {
                        throw std::invalid_argument(
                            "PatchEditor: a patch edge may appear in at most one seam");
                    }
                }
                used.emplace_back(patch, edge);
            };
            mark(seam.patch_a, static_cast<int>(seam.edge_a));
            mark(seam.patch_b, static_cast<int>(seam.edge_b));
        }
    }

    // ------------------------------------------------------------------
    // Seam knot-basis unification by exact homogeneous knot insertion.
    // This does not change the geometry; it only aligns the along-edge
    // polynomial segmentation of both sides.
    // ------------------------------------------------------------------
    void refineForSeams()
    {
        std::vector<std::vector<std::pair<double, int>>> ins_u(patches.size());
        std::vector<std::vector<std::pair<double, int>>> ins_v(patches.size());

        for (const PatchSeam &seam : seams)
        {
            PatchData &A = patches[static_cast<std::size_t>(seam.patch_a)];
            PatchData &B = patches[static_cast<std::size_t>(seam.patch_b)];
            const int ea = static_cast<int>(seam.edge_a);
            const int eb = static_cast<int>(seam.edge_b);
            const bool a_along_u = (ea == static_cast<int>(SurfaceEdge::VMin) ||
                                    ea == static_cast<int>(SurfaceEdge::VMax));
            const bool b_along_u = (eb == static_cast<int>(SurfaceEdge::VMin) ||
                                    eb == static_cast<int>(SurfaceEdge::VMax));
            const unsigned int p = a_along_u ? A.degree_u : A.degree_v;
            const unsigned int q = b_along_u ? B.degree_u : B.degree_v;
            require(p == q, "seam patches must have the same degree along the edge");

            const std::vector<double> &kA = a_along_u ? A.knots_u : A.knots_v;
            const std::vector<double> &kB = b_along_u ? B.knots_u : B.knots_v;
            const auto domA = a_along_u ? A.uDomain() : A.vDomain();
            const auto domB = b_along_u ? B.uDomain() : B.vDomain();

            const std::vector<BreakPoint> brA = collectBreaks(kA, domA.first, domA.second);
            const std::vector<BreakPoint> brB = collectBreaks(kB, domB.first, domB.second);

            struct Item
            {
                double t;
                int multA = 0, multB = 0;
                bool hasA = false, hasB = false;
                double exactA = 0.0, exactB = 0.0;
            };
            std::vector<Item> items;
            auto findItem = [&](double t) -> Item * {
                for (Item &it : items)
                {
                    if (std::abs(it.t - t) <= kNormalizedBreakTolerance)
                    {
                        return &it;
                    }
                }
                return nullptr;
            };
            for (const BreakPoint &bp : brA)
            {
                Item *it = findItem(bp.t);
                if (!it)
                {
                    items.push_back({bp.t, 0, 0, false, false, 0.0, 0.0});
                    it = &items.back();
                }
                it->hasA = true;
                it->exactA = bp.val;
                it->multA = std::max(it->multA, bp.mult);
            }
            for (const BreakPoint &bp : brB)
            {
                const double t = seam.reversed ? (1.0 - bp.t) : bp.t;
                Item *it = findItem(t);
                if (!it)
                {
                    items.push_back({t, 0, 0, false, false, 0.0, 0.0});
                    it = &items.back();
                }
                it->hasB = true;
                it->exactB = bp.val;
                it->multB = std::max(it->multB, bp.mult);
            }

            const double tolA = 1e-9 * (domA.second - domA.first) + 1e-13;
            const double tolB = 1e-9 * (domB.second - domB.first) + 1e-13;

            for (const Item &it : items)
            {
                // Keeping the larger multiplicity also keeps the bases closer.
                const int target_mult = std::max(it.multA, it.multB);
                require(target_mult <= static_cast<int>(p),
                        "seam refinement would exceed the allowed knot multiplicity");

                double valueA = it.hasA ? it.exactA
                                        : domA.first + it.t * (domA.second - domA.first);
                snapToKnot(kA, valueA, tolA);
                addTarget(a_along_u ? ins_u[static_cast<std::size_t>(seam.patch_a)]
                                    : ins_v[static_cast<std::size_t>(seam.patch_a)],
                          valueA, target_mult, tolA);

                const double tB = seam.reversed ? (1.0 - it.t) : it.t;
                double valueB = it.hasB ? it.exactB
                                        : domB.first + tB * (domB.second - domB.first);
                snapToKnot(kB, valueB, tolB);
                addTarget(b_along_u ? ins_u[static_cast<std::size_t>(seam.patch_b)]
                                    : ins_v[static_cast<std::size_t>(seam.patch_b)],
                          valueB, target_mult, tolB);
            }
        }

        auto apply = [this](std::size_t patch_index, bool along_u,
                            const std::vector<std::pair<double, int>> &targets) {
            PatchData &pd = patches[patch_index];
            std::vector<double> &knots = along_u ? pd.knots_u : pd.knots_v;
            array2<dvec4> &cp = pd.hw;
            const unsigned int degree = along_u ? pd.degree_u : pd.degree_v;
            for (const std::pair<double, int> &target : targets)
            {
                double value = target.first;
                const double tol = 1e-9 * std::abs(knots.back() - knots.front()) + 1e-13;
                snapToKnot(knots, value, tol);
                const int current = multiplicityWithin(knots, value, tol);
                int repeat = target.second - current;
                if (repeat > static_cast<int>(degree) - current)
                {
                    repeat = static_cast<int>(degree) - current;
                }
                if (repeat <= 0)
                {
                    continue;
                }
                std::vector<double> new_knots;
                array2<dvec4> new_cp;
                internal::surfaceKnotInsert<4, double>(degree, knots, cp, value, repeat, along_u,
                                                       new_knots, new_cp);
                knots = std::move(new_knots);
                cp = std::move(new_cp);
                pd.rows = static_cast<int>(cp.rows());
                pd.cols = static_cast<int>(cp.cols());
            }
        };

        for (std::size_t i = 0; i < patches.size(); ++i)
        {
            apply(i, true, ins_u[i]);
            apply(i, false, ins_v[i]);
        }
    }

    // ------------------------------------------------------------------
    // Patch bookkeeping after refinement
    // ------------------------------------------------------------------
    void finalizePatches()
    {
        total_control_points = 0;
        for (PatchData &p : patches)
        {
            p.rows = static_cast<int>(p.hw.rows());
            p.cols = static_cast<int>(p.hw.cols());
            p.cp_base = static_cast<int>(total_control_points);
            total_control_points += static_cast<std::size_t>(p.rows) * p.cols;

            p.w.resize(p.hw.rows(), p.hw.cols());
            p.rest.resize(p.hw.rows(), p.hw.cols());
            for (int i = 0; i < p.rows; ++i)
            {
                for (int j = 0; j < p.cols; ++j)
                {
                    const dvec4 h = p.hw(i, j);
                    p.w(i, j) = h.w;
                    p.rest(i, j) = dvec3(h.x / h.w, h.y / h.w, h.z / h.w);
                }
            }
        }
        for (std::vector<double> &r : rest)
        {
            r.assign(total_control_points, 0.0);
        }
        for (const PatchData &p : patches)
        {
            for (int i = 0; i < p.rows; ++i)
            {
                for (int j = 0; j < p.cols; ++j)
                {
                    const int idx = p.cp_base + p.lin(i, j);
                    const dvec3 &v = p.rest(i, j);
                    rest[0][static_cast<std::size_t>(idx)] = v.x;
                    rest[1][static_cast<std::size_t>(idx)] = v.y;
                    rest[2][static_cast<std::size_t>(idx)] = v.z;
                }
            }
        }
    }

    // ------------------------------------------------------------------
    // Seam constraint assembly
    // ------------------------------------------------------------------
    struct SideGeom
    {
        std::size_t patch = 0;
        bool along_u = false;
        unsigned int p = 0;
        double tau0 = 0.0, tau1 = 0.0; // along domain
        double r = 1.0;                // signed parametric inward transverse length
        std::vector<int> bnd_lin;
        std::vector<int> l0_lin, l1_lin;
        std::vector<double> dc0, dc1;
        std::vector<double> knots;
    };

    SideGeom makeSide(std::size_t patch_index, SurfaceEdge edge) const
    {
        const PatchData &pd = patches[patch_index];
        SideGeom s;
        s.patch = patch_index;
        const int e = static_cast<int>(edge);
        s.along_u = (e == static_cast<int>(SurfaceEdge::VMin) ||
                     e == static_cast<int>(SurfaceEdge::VMax));
        const bool at_min =
            (e == static_cast<int>(SurfaceEdge::UMin) || e == static_cast<int>(SurfaceEdge::VMin));
        const unsigned int transverse_degree = s.along_u ? pd.degree_v : pd.degree_u;
        const std::vector<double> &knots_t = s.along_u ? pd.knots_v : pd.knots_u;
        const int transverse_count = s.along_u ? pd.cols : pd.rows;
        const double denom = at_min
                                 ? knots_t[transverse_degree + 1] - knots_t[1]
                                 : knots_t[transverse_count - 1 + transverse_degree] -
                                       knots_t[transverse_count - 1];
        require(denom > 0.0, "degenerate transverse knot domain at a seam");
        const double coefficient = static_cast<double>(transverse_degree) / denom;

        if (s.along_u)
        {
            s.p = pd.degree_u;
            s.tau0 = pd.uDomain().first;
            s.tau1 = pd.uDomain().second;
            s.knots = pd.knots_u;
            const int col0 = at_min ? 0 : pd.cols - 1;
            const int col1 = at_min ? 1 : pd.cols - 2;
            const int n = pd.rows;
            s.bnd_lin.resize(static_cast<std::size_t>(n));
            s.l0_lin.resize(static_cast<std::size_t>(n));
            s.l1_lin.resize(static_cast<std::size_t>(n));
            s.dc0.resize(static_cast<std::size_t>(n));
            s.dc1.resize(static_cast<std::size_t>(n));
            for (int i = 0; i < n; ++i)
            {
                s.bnd_lin[static_cast<std::size_t>(i)] = i * pd.cols + col0;
                s.l0_lin[static_cast<std::size_t>(i)] = i * pd.cols + col0;
                s.l1_lin[static_cast<std::size_t>(i)] = i * pd.cols + col1;
                s.dc0[static_cast<std::size_t>(i)] = at_min ? -coefficient : coefficient;
                s.dc1[static_cast<std::size_t>(i)] = at_min ? coefficient : -coefficient;
            }
            const double len = pd.original_v1 - pd.original_v0;
            s.r = at_min ? len : -len;
        }
        else
        {
            s.p = pd.degree_v;
            s.tau0 = pd.vDomain().first;
            s.tau1 = pd.vDomain().second;
            s.knots = pd.knots_v;
            const int row0 = at_min ? 0 : pd.rows - 1;
            const int row1 = at_min ? 1 : pd.rows - 2;
            const int n = pd.cols;
            s.bnd_lin.resize(static_cast<std::size_t>(n));
            s.l0_lin.resize(static_cast<std::size_t>(n));
            s.l1_lin.resize(static_cast<std::size_t>(n));
            s.dc0.resize(static_cast<std::size_t>(n));
            s.dc1.resize(static_cast<std::size_t>(n));
            for (int j = 0; j < n; ++j)
            {
                s.bnd_lin[static_cast<std::size_t>(j)] = row0 * pd.cols + j;
                s.l0_lin[static_cast<std::size_t>(j)] = row0 * pd.cols + j;
                s.l1_lin[static_cast<std::size_t>(j)] = row1 * pd.cols + j;
                s.dc0[static_cast<std::size_t>(j)] = at_min ? -coefficient : coefficient;
                s.dc1[static_cast<std::size_t>(j)] = at_min ? coefficient : -coefficient;
            }
            const double len = pd.original_u1 - pd.original_u0;
            s.r = at_min ? len : -len;
        }
        return s;
    }

    std::vector<double> commonSpans(const SideGeom &A, const SideGeom &B, bool reversed) const
    {
        std::vector<double> t;
        for (const BreakPoint &bp : collectBreaks(A.knots, A.tau0, A.tau1))
        {
            t.push_back(bp.t);
        }
        for (const BreakPoint &bp : collectBreaks(B.knots, B.tau0, B.tau1))
        {
            t.push_back(reversed ? 1.0 - bp.t : bp.t);
        }
        t.push_back(0.0);
        t.push_back(1.0);
        std::sort(t.begin(), t.end());
        std::vector<double> merged;
        for (double v : t)
        {
            if (merged.empty() || std::abs(v - merged.back()) > kNormalizedBreakTolerance)
            {
                merged.push_back(v);
            }
        }
        merged.front() = 0.0;
        merged.back() = 1.0;
        return merged;
    }

    struct SideForm
    {
        // For each power-basis coefficient a (0..p): sparse forms of the
        // boundary position X and transverse derivative Xp, plus the scalar
        // weight W and weight derivative Wp.
        std::vector<std::vector<Term>> X;
        std::vector<std::vector<Term>> Xp;
        std::vector<double> W;
        std::vector<double> Wp;
    };

    SideForm buildSideForm(const SideGeom &side, const std::vector<int> &idx,
                           const std::vector<std::vector<double>> &coef) const
    {
        const PatchData &pd = patches[side.patch];
        const int n = static_cast<int>(side.p) + 1;
        SideForm form;
        form.X.assign(static_cast<std::size_t>(n), {});
        form.Xp.assign(static_cast<std::size_t>(n), {});
        form.W.assign(static_cast<std::size_t>(n), 0.0);
        form.Wp.assign(static_cast<std::size_t>(n), 0.0);
        for (int a = 0; a < n; ++a)
        {
            for (int q = 0; q < n; ++q)
            {
                const double ca = coef[static_cast<std::size_t>(q)][static_cast<std::size_t>(a)];
                if (ca == 0.0)
                {
                    continue;
                }
                const int j = idx[static_cast<std::size_t>(q)];
                const int lin = side.bnd_lin[static_cast<std::size_t>(j)];
                const double wj = pd.weightLin(lin);
                form.X[static_cast<std::size_t>(a)].emplace_back(pd.cp_base + lin, wj * ca);
                form.W[static_cast<std::size_t>(a)] += wj * ca;

                const int l0 = side.l0_lin[static_cast<std::size_t>(j)];
                const int l1 = side.l1_lin[static_cast<std::size_t>(j)];
                const double w0 = pd.weightLin(l0);
                const double w1 = pd.weightLin(l1);
                form.Xp[static_cast<std::size_t>(a)].emplace_back(
                    pd.cp_base + l0, side.dc0[static_cast<std::size_t>(j)] * w0 * ca);
                form.Xp[static_cast<std::size_t>(a)].emplace_back(
                    pd.cp_base + l1, side.dc1[static_cast<std::size_t>(j)] * w1 * ca);
                form.Wp[static_cast<std::size_t>(a)] +=
                    (side.dc0[static_cast<std::size_t>(j)] * w0 +
                     side.dc1[static_cast<std::size_t>(j)] * w1) *
                    ca;
            }
        }
        return form;
    }

    void appendSeamRows(const SideGeom &A, const SideGeom &B, bool reversed)
    {
        const int p = static_cast<int>(A.p);
        const std::vector<double> spans = commonSpans(A, B, reversed);
        const double LA = A.tau1 - A.tau0;
        const double LB = B.tau1 - B.tau0;

        for (std::size_t s = 0; s + 1 < spans.size(); ++s)
        {
            const double t0 = spans[s];
            const double t1 = spans[s + 1];
            if (t1 - t0 <= 0.0)
            {
                continue;
            }
            const double tau0A = A.tau0 + t0 * LA;
            const double tau1A = A.tau0 + t1 * LA;
            const double tB0 = reversed ? (1.0 - t0) : t0;
            const double tB1 = reversed ? (1.0 - t1) : t1;
            const double tau0B = B.tau0 + tB0 * LB;
            const double tau1B = B.tau0 + tB1 * LB;

            std::vector<int> idxA, idxB;
            std::vector<std::vector<double>> coefA, coefB;
            basisSpanInfo(A.p, A.knots, tau0A, tau1A, idxA, coefA);
            basisSpanInfo(B.p, B.knots, tau0B, tau1B, idxB, coefB);

            const SideForm FA = buildSideForm(A, idxA, coefA);
            const SideForm FB = buildSideForm(B, idxB, coefB);

            const int maxDegree = 2 * p;
            const std::size_t n = total_control_points;
            for (int k = 0; k <= maxDegree; ++k)
            {
                const int aBegin = std::max(0, k - p);
                const int aEnd = std::min(p, k);

                // Position: XA*WB - XB*WA = 0.
                std::vector<double> posRow(n, 0.0);
                for (int a = aBegin; a <= aEnd; ++a)
                {
                    const int b = k - a;
                    addTerms(posRow, FA.X[static_cast<std::size_t>(a)],
                             FB.W[static_cast<std::size_t>(b)]);
                    addTerms(posRow, FB.X[static_cast<std::size_t>(a)],
                             -FA.W[static_cast<std::size_t>(b)]);
                }
                seam_rows.push_back(std::move(posRow));

                // Derivative:
                //   rA*(XpA*WB - XB*WpA) + rB*(XpB*WA - XA*WpB) = 0.
                std::vector<double> derRow(n, 0.0);
                for (int a = aBegin; a <= aEnd; ++a)
                {
                    const int b = k - a;
                    addTerms(derRow, FA.Xp[static_cast<std::size_t>(a)],
                             A.r * FB.W[static_cast<std::size_t>(b)]);
                    addTerms(derRow, FB.X[static_cast<std::size_t>(a)],
                             -A.r * FA.Wp[static_cast<std::size_t>(b)]);
                    addTerms(derRow, FB.Xp[static_cast<std::size_t>(a)],
                             B.r * FA.W[static_cast<std::size_t>(b)]);
                    addTerms(derRow, FA.X[static_cast<std::size_t>(a)],
                             -B.r * FB.Wp[static_cast<std::size_t>(b)]);
                }
                seam_rows.push_back(std::move(derRow));
            }
        }
    }

    void buildSeamRows()
    {
        seam_rows.clear();
        for (const PatchSeam &seam : seams)
        {
            SideGeom A = makeSide(static_cast<std::size_t>(seam.patch_a), seam.edge_a);
            SideGeom B = makeSide(static_cast<std::size_t>(seam.patch_b), seam.edge_b);
            require(A.p == B.p, "seam patches must have the same degree along the edge");
            appendSeamRows(A, B, seam.reversed);
        }
    }

    // ------------------------------------------------------------------
    // Handle rows: one row per handle, shared by the three coordinates.
    // ------------------------------------------------------------------
    void appendHandleRows(const std::vector<PatchHandle> &handles,
                          std::vector<std::vector<double>> &rows,
                          std::vector<HandleRowInfo> &infos) const
    {
        for (const PatchHandle &h : handles)
        {
            const PatchData &pd = patches[static_cast<std::size_t>(h.patch)];
            const double u = std::min(std::max(h.u, pd.original_u0), pd.original_u1);
            const double v = std::min(std::max(h.v, pd.original_v0), pd.original_v1);
            const int span_u = findSpan(pd.degree_u, pd.knots_u, u);
            const int span_v = findSpan(pd.degree_v, pd.knots_v, v);
            const std::vector<double> Nu = bsplineBasis(pd.degree_u, span_u, pd.knots_u, u);
            const std::vector<double> Nv = bsplineBasis(pd.degree_v, span_v, pd.knots_v, v);
            std::vector<double> row(total_control_points, 0.0);
            double denominator = 0.0;
            for (int l = 0; l <= static_cast<int>(pd.degree_v); ++l)
            {
                for (int k = 0; k <= static_cast<int>(pd.degree_u); ++k)
                {
                    const int i = span_u - static_cast<int>(pd.degree_u) + k;
                    const int j = span_v - static_cast<int>(pd.degree_v) + l;
                    const double c = pd.w(i, j) * Nu[static_cast<std::size_t>(k)] *
                                     Nv[static_cast<std::size_t>(l)];
                    row[static_cast<std::size_t>(pd.cp_base + pd.lin(i, j))] += c;
                    denominator += c;
                }
            }
            HandleRowInfo info;
            info.row = static_cast<int>(rows.size());
            info.denominator = denominator;
            info.patch = h.patch;
            info.u = h.u;
            info.v = h.v;
            info.id = h.id;
            rows.push_back(std::move(row));
            infos.push_back(info);
        }
    }

    // ------------------------------------------------------------------
    // Rank-revealing factorization with row pivoting.
    // ------------------------------------------------------------------
    QRFactor factorize(const std::vector<std::vector<double>> &rows) const
    {
        QRFactor f;
        f.m = static_cast<int>(rows.size());
        f.n = static_cast<int>(total_control_points);
        f.invnorm.assign(static_cast<std::size_t>(f.m), 1.0);
        f.perm.resize(static_cast<std::size_t>(f.m));
        std::iota(f.perm.begin(), f.perm.end(), 0);
        for (std::vector<double> &v : f.row_dot_rest)
        {
            v.assign(static_cast<std::size_t>(f.m), 0.0);
        }
        for (int i = 0; i < f.m; ++i)
        {
            for (int c = 0; c < 3; ++c)
            {
                f.row_dot_rest[static_cast<std::size_t>(c)][static_cast<std::size_t>(i)] =
                    dot(rows[static_cast<std::size_t>(i)], rest[static_cast<std::size_t>(c)]);
            }
        }

        const int maxRank = std::min(f.m, f.n);
        std::vector<std::vector<double>> v(static_cast<std::size_t>(f.m));
        std::vector<std::vector<double>> coeff(
            static_cast<std::size_t>(f.m),
            std::vector<double>(static_cast<std::size_t>(std::max(0, maxRank)), 0.0));
        for (int i = 0; i < f.m; ++i)
        {
            const double nrm = std::sqrt(norm2(rows[static_cast<std::size_t>(i)]));
            if (nrm > 0.0)
            {
                f.invnorm[static_cast<std::size_t>(i)] = 1.0 / nrm;
                v[static_cast<std::size_t>(i)].resize(static_cast<std::size_t>(f.n));
                for (int t = 0; t < f.n; ++t)
                {
                    v[static_cast<std::size_t>(i)][static_cast<std::size_t>(t)] =
                        rows[static_cast<std::size_t>(i)][static_cast<std::size_t>(t)] / nrm;
                }
            }
            else
            {
                v[static_cast<std::size_t>(i)] = rows[static_cast<std::size_t>(i)];
            }
        }

        int rank = 0;
        for (int k = 0; k < maxRank; ++k)
        {
            int best = -1;
            double bestNorm2 = 0.0;
            for (int i = k; i < f.m; ++i)
            {
                const double nn = norm2(v[static_cast<std::size_t>(i)]);
                if (nn > bestNorm2)
                {
                    bestNorm2 = nn;
                    best = i;
                }
            }
            if (best < 0 || std::sqrt(bestNorm2) <= kRankTolerance)
            {
                break;
            }
            if (best != k)
            {
                std::swap(v[static_cast<std::size_t>(k)], v[static_cast<std::size_t>(best)]);
                std::swap(coeff[static_cast<std::size_t>(k)],
                          coeff[static_cast<std::size_t>(best)]);
                std::swap(f.perm[static_cast<std::size_t>(k)],
                          f.perm[static_cast<std::size_t>(best)]);
            }
            for (int j = 0; j < k; ++j)
            {
                const double d =
                    dot(f.q[static_cast<std::size_t>(j)], v[static_cast<std::size_t>(k)]);
                if (d != 0.0)
                {
                    for (int t = 0; t < f.n; ++t)
                    {
                        v[static_cast<std::size_t>(k)][static_cast<std::size_t>(t)] -=
                            d * f.q[static_cast<std::size_t>(j)][static_cast<std::size_t>(t)];
                    }
                    coeff[static_cast<std::size_t>(k)][static_cast<std::size_t>(j)] += d;
                }
            }
            double nk = std::sqrt(norm2(v[static_cast<std::size_t>(k)]));
            if (nk <= kRankTolerance)
            {
                break;
            }
            f.q.push_back(v[static_cast<std::size_t>(k)]);
            for (double &x : f.q.back())
            {
                x /= nk;
            }
            coeff[static_cast<std::size_t>(k)][static_cast<std::size_t>(k)] = nk;
            std::fill(v[static_cast<std::size_t>(k)].begin(),
                      v[static_cast<std::size_t>(k)].end(), 0.0);
            const std::vector<double> &qk = f.q.back();
            for (int i = k + 1; i < f.m; ++i)
            {
                double d = dot(qk, v[static_cast<std::size_t>(i)]);
                for (int t = 0; t < f.n; ++t)
                {
                    v[static_cast<std::size_t>(i)][static_cast<std::size_t>(t)] -=
                        d * qk[static_cast<std::size_t>(t)];
                }
                // A second pass keeps the Gram-Schmidt basis orthogonal.
                const double d2 = dot(qk, v[static_cast<std::size_t>(i)]);
                for (int t = 0; t < f.n; ++t)
                {
                    v[static_cast<std::size_t>(i)][static_cast<std::size_t>(t)] -=
                        d2 * qk[static_cast<std::size_t>(t)];
                }
                coeff[static_cast<std::size_t>(i)][static_cast<std::size_t>(k)] = d + d2;
            }
            ++rank;
        }
        f.rank = rank;
        f.coeff = std::move(coeff);
        return f;
    }

    // ------------------------------------------------------------------
    // Snapshots
    // ------------------------------------------------------------------
    PatchEditorSnapshot makeSnapshot(const std::array<std::vector<double>, 3> &x,
                                     const std::vector<PatchHandle> &handles) const
    {
        PatchEditorSnapshot snap;
        snap.seams = seams;
        snap.handles = handles;
        snap.original_knots_u = original_knots_u;
        snap.original_knots_v = original_knots_v;
        snap.patches.resize(patches.size());
        for (std::size_t pi = 0; pi < patches.size(); ++pi)
        {
            const PatchData &src = patches[pi];
            RationalSurface3d &dst = snap.patches[pi];
            dst.degree_u = src.degree_u;
            dst.degree_v = src.degree_v;
            dst.knots_u = src.knots_u;
            dst.knots_v = src.knots_v;
            dst.control_points.resize(static_cast<std::size_t>(src.rows),
                                      static_cast<std::size_t>(src.cols));
            dst.weights.resize(static_cast<std::size_t>(src.rows),
                               static_cast<std::size_t>(src.cols));
            for (int i = 0; i < src.rows; ++i)
            {
                for (int j = 0; j < src.cols; ++j)
                {
                    const int idx = src.cp_base + src.lin(i, j);
                    dst.control_points(static_cast<std::size_t>(i), static_cast<std::size_t>(j)) =
                        dvec3(x[0][static_cast<std::size_t>(idx)],
                              x[1][static_cast<std::size_t>(idx)],
                              x[2][static_cast<std::size_t>(idx)]);
                    dst.weights(static_cast<std::size_t>(i), static_cast<std::size_t>(j)) =
                        src.w(i, j);
                }
            }
        }
        return snap;
    }

    CacheKey makeCacheKey(const std::vector<PatchHandle> &handles) const
    {
        CacheKey key;
        key.seam_version = 1;
        key.handles.reserve(handles.size());
        for (const PatchHandle &h : handles)
        {
            key.handles.push_back({h.id, h.patch, h.u, h.v});
        }
        std::sort(key.handles.begin(), key.handles.end());
        return key;
    }

    const HandleRowInfo *findHandleRow(const PatchHandle &h) const
    {
        for (const HandleRowInfo &info : cache.handles)
        {
            if (info.id == h.id && info.patch == h.patch && std::abs(info.u - h.u) <= 1e-12 &&
                std::abs(info.v - h.v) <= 1e-12)
            {
                return &info;
            }
        }
        return nullptr;
    }
};

/////////////////////////////////////////////////////////////////////////////

PatchEditor::PatchEditor(std::vector<RationalSurface3d> patches, std::vector<PatchSeam> seams)
    : impl_(new Impl())
{
    Impl &I = *impl_;
    if (patches.empty())
    {
        require(seams.empty(), "seams require at least one patch");
        I.seams = std::move(seams);
        I.committed = PatchEditorSnapshot();
        I.committed.seams = I.seams;
        return;
    }
    for (const RationalSurface3d &srf : patches)
    {
        validateSurface(srf);
    }
    require(seams.size() <= 4096, "too many seam declarations");

    I.patches.resize(patches.size());
    for (std::size_t pi = 0; pi < patches.size(); ++pi)
    {
        const RationalSurface3d &srf = patches[pi];
        PatchData &p = I.patches[pi];
        p.degree_u = srf.degree_u;
        p.degree_v = srf.degree_v;
        p.knots_u = srf.knots_u;
        p.knots_v = srf.knots_v;
        p.original_knots_u = srf.knots_u;
        p.original_knots_v = srf.knots_v;
        p.original_u0 = p.uDomain().first;
        p.original_u1 = p.uDomain().second;
        p.original_v0 = p.vDomain().first;
        p.original_v1 = p.vDomain().second;
        p.rows = static_cast<int>(srf.control_points.rows());
        p.cols = static_cast<int>(srf.control_points.cols());
        p.hw.resize(static_cast<std::size_t>(p.rows), static_cast<std::size_t>(p.cols));
        for (int i = 0; i < p.rows; ++i)
        {
            for (int j = 0; j < p.cols; ++j)
            {
                const double w = srf.weights(static_cast<std::size_t>(i),
                                             static_cast<std::size_t>(j));
                p.hw(static_cast<std::size_t>(i), static_cast<std::size_t>(j)) =
                    dvec4(srf.control_points(static_cast<std::size_t>(i),
                                             static_cast<std::size_t>(j)) *
                              w,
                          w);
            }
        }
    }
    I.seams = std::move(seams);
    I.validateSeams();

    I.refineForSeams();
    I.finalizePatches();
    I.buildSeamRows();

    I.original_knots_u.resize(I.patches.size());
    I.original_knots_v.resize(I.patches.size());
    for (std::size_t pi = 0; pi < I.patches.size(); ++pi)
    {
        I.original_knots_u[pi] = I.patches[pi].original_knots_u;
        I.original_knots_v[pi] = I.patches[pi].original_knots_v;
    }

    I.committed = I.makeSnapshot(I.rest, {});
    I.committed_handles.clear();
}

PatchEditor::~PatchEditor() = default;
PatchEditor::PatchEditor(PatchEditor &&) noexcept = default;
PatchEditor &PatchEditor::operator=(PatchEditor &&) noexcept = default;

void PatchEditor::set_handles(std::vector<PatchHandle> handles)
{
    Impl &I = *impl_;
    I.validateHandleList(handles);
    I.staged_handles = std::move(handles);
}

const std::vector<PatchHandle> &PatchEditor::staged_handles() const
{
    return impl_->staged_handles;
}

std::size_t PatchEditor::factorization_count() const { return impl_->factorizations; }

std::size_t PatchEditor::patch_count() const { return impl_->patches.size(); }

std::size_t PatchEditor::control_point_count() const { return impl_->total_control_points; }

PatchEditorSnapshot PatchEditor::snapshot() const { return impl_->committed; }

PatchEditorSnapshot PatchEditor::solve()
{
    Impl &I = *impl_;
    if (I.patches.empty())
    {
        I.committed = PatchEditorSnapshot();
        I.committed.seams = I.seams;
        I.committed_handles.clear();
        I.staged_handles.clear();
        return I.committed;
    }

    try
    {
        I.validateHandleList(I.staged_handles);

        const CacheKey key = I.makeCacheKey(I.staged_handles);
        if (!I.has_cache || !(I.cache_key == key))
        {
            std::vector<std::vector<double>> rows = I.seam_rows;
            std::vector<HandleRowInfo> infos;
            I.appendHandleRows(I.staged_handles, rows, infos);
            QRFactor factor;
            if (rows.empty())
            {
                factor.n = static_cast<int>(I.total_control_points);
                factor.m = 0;
                factor.rank = 0;
                for (std::vector<double> &v : factor.row_dot_rest)
                {
                    v.clear();
                }
            }
            else
            {
                factor = I.factorize(rows);
                ++I.factorizations;
            }
            factor.handles = std::move(infos);
            // Publish only a complete factorization.
            I.cache = std::move(factor);
            I.cache_key = key;
            I.has_cache = true;
        }

        std::array<std::vector<double>, 3> x = I.rest;
        for (int comp = 0; comp < 3; ++comp)
        {
            std::vector<double> rhs(static_cast<std::size_t>(I.cache.m), 0.0);
            for (const PatchHandle &h : I.staged_handles)
            {
                const HandleRowInfo *info = I.findHandleRow(h);
                if (!info)
                {
                    throw std::runtime_error("PatchEditor: internal handle cache mismatch");
                }
                rhs[static_cast<std::size_t>(info->row)] =
                    info->denominator * h.target[comp];
            }
            const std::vector<double> z = I.cache.solve(rhs, comp);
            for (std::size_t i = 0; i < x[static_cast<std::size_t>(comp)].size(); ++i)
            {
                x[static_cast<std::size_t>(comp)][i] += z[i];
            }
        }

        PatchEditorSnapshot snap = I.makeSnapshot(x, I.staged_handles);
        I.committed = snap;
        I.committed_handles = I.staged_handles;
        return snap;
    }
    catch (...)
    {
        // Restore the last successfully committed state.
        I.staged_handles = I.committed_handles;
        throw;
    }
}

} // namespace tinynurbs
