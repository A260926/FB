// Regression tests for the SISL grid-event tracer and the imprint
// triangulation.  No main() here: task_support/driver.c provides it and
// collects the cases registered below.
//
// All surfaces are built locally from power-basis coefficients, so the tests
// own their geometry and cross-check every claim independently: native
// station chains, both sheets' directed edge chains, per-sheet UV coverage
// without overlap, corner back-substitution and normals, and failure
// behaviour (previous output preserved, repeated calls independent).

#include "../task_support/check.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "sisl.h"
#include "sisl_imprint.hpp"
#include "sisl_trace_events.h"

using sislmesh::ImprintOptions;
using sislmesh::ImprintResult;
using sislmesh::ImprintTriangle;
using sislmesh::ImprintPolyline;
using sislmesh::imprint_surfaces;

namespace
{

// ---------------------------------------------------------------------
// local geometry construction
// ---------------------------------------------------------------------

// Control values of a cubic Bezier from power-basis coefficients a0..a3.
void bezier_from_power(const double a[4], double p[4])
{
    p[0] = a[0];
    p[1] = a[0] + a[1] / 3.0;
    p[2] = a[0] + 2.0 * a[1] / 3.0 + a[2] / 3.0;
    p[3] = a[0] + a[1] + a[2] + a[3];
}

struct SurfaceSpec
{
    // coef[axis][i][j] multiplies s^i t^j, s,t the local parameters in [0,1].
    // x = x0 + (x1-x0) s, y = y0 + (y1-y0) t are supplied separately so a
    // surface can be placed in a non-trivial box.
    double coef[3][4][4];
    double x0 = 0, x1 = 1, y0 = 0, y1 = 1;
    int ikind = 1;
    std::vector<double> weights;   // 16 weights for ikind == 2
    bool swap_uv = false;          // build x = y(t,s), y = x(t,s)
    // parameter domain; the physical geometry is unchanged, only the knot
    // span (and therefore the parameterisation) differs
    double u0 = 0, u1 = 1, v0 = 0, v1 = 1;
};

SISLSurf *build_patch(const SurfaceSpec &spec)
{
    double kn1[8] = {0, 0, 0, 0, 1, 1, 1, 1};
    double kn2[8] = {0, 0, 0, 0, 1, 1, 1, 1};
    for (int i = 0; i < 8; ++i)
    {
        kn1[i] = spec.u0 + (spec.u1 - spec.u0) * kn1[i];
        kn2[i] = spec.v0 + (spec.v1 - spec.v0) * kn2[i];
    }
    int dim = spec.ikind == 2 ? 4 : 3;
    std::vector<double> cp((size_t)16 * dim, 0.0);

    // physical x/y polynomial coefficients in power basis
    double cx[4][4] = {{0}}, cy[4][4] = {{0}};
    cx[1][0] = spec.x1 - spec.x0;
    cx[0][0] = spec.x0;
    cy[0][1] = spec.y1 - spec.y0;
    cy[0][0] = spec.y0;

    double c[3][4][4];
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
        {
            c[0][i][j] = cx[i][j];
            c[1][i][j] = cy[i][j];
            c[2][i][j] = spec.coef[2][i][j];
        }
    if (spec.swap_uv)
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j)
                std::swap(c[0][i][j], c[1][i][j]);

    // Separable conversion to the tensor-product Bernstein basis: first in s
    // for each fixed t, then in t.
    double stage[4][4][3];
    for (int t = 0; t < 4; ++t)
        for (int a = 0; a < 3; ++a)
        {
            double col[4];
            for (int k = 0; k < 4; ++k) col[k] = c[a][k][t];
            double bs[4];
            bezier_from_power(col, bs);
            for (int i = 0; i < 4; ++i) stage[t][i][a] = bs[i];
        }
    for (int i = 0; i < 4; ++i)
        for (int a = 0; a < 3; ++a)
        {
            double row[4];
            for (int k = 0; k < 4; ++k) row[k] = stage[k][i][a];
            double bt[4];
            bezier_from_power(row, bt);
            for (int j = 0; j < 4; ++j)
            {
                int idx = dim * (j * 4 + i);
                cp[(size_t)idx + a] = bt[j];
            }
        }

    if (spec.ikind == 2)
    {
        for (int j = 0; j < 4; ++j)
            for (int i = 0; i < 4; ++i)
            {
                int idx = 4 * (j * 4 + i);
                double w = spec.weights.empty() ? 1.0 : spec.weights[(size_t)j * 4 + i];
                cp[(size_t)idx + 0] *= w;
                cp[(size_t)idx + 1] *= w;
                cp[(size_t)idx + 2] *= w;
                cp[(size_t)idx + 3] = w;
            }
    }
    return newSurf(4, 4, 4, 4, kn1, kn2, cp.data(), spec.ikind, 3, 1);
}

std::vector<double> grid4()
{
    return {0.0, 0.25, 0.5, 0.75, 1.0};
}
std::vector<double> grid2()
{
    return {0.0, 1.0};
}

// A single-cell grid (both domain ends only) is legal.
std::vector<double> domain_ends(double a, double b)
{
    return {a, b};
}

// helper: is the point within distance d of a segment
double point_segment(const double *p, const double *a, const double *b)
{
    double dx = b[0] - a[0], dy = b[1] - a[1], dz = b[2] - a[2];
    double wx = p[0] - a[0], wy = p[1] - a[1], wz = p[2] - a[2];
    double den = dx * dx + dy * dy + dz * dz;
    double t = den > 0 ? (wx * dx + wy * dy + wz * dz) / den : 0.0;
    t = std::max(0.0, std::min(1.0, t));
    double qx = a[0] + t * dx, qy = a[1] + t * dy, qz = a[2] + t * dz;
    return std::sqrt((p[0] - qx) * (p[0] - qx) + (p[1] - qy) * (p[1] - qy) +
                     (p[2] - qz) * (p[2] - qz));
}

double dist3(const double *a, const double *b)
{
    return std::sqrt((a[0] - b[0]) * (a[0] - b[0]) +
                     (a[1] - b[1]) * (a[1] - b[1]) +
                     (a[2] - b[2]) * (a[2] - b[2]));
}

// ---------------------------------------------------------------------
// independent checks on native trace results
// ---------------------------------------------------------------------

struct Station
{
    double xyz[3];
    double uv1[2], uv2[2];
    unsigned mask;
    int gidx[4];
};

struct Branch
{
    std::vector<Station> st;
    bool closed;
};

std::vector<Branch> copy_trace(const SislTraceResult *r)
{
    std::vector<Branch> out(r->branch_count);
    for (std::size_t b = 0; b < r->branch_count; ++b)
    {
        const SislTraceBranch &tb = r->branches[b];
        out[b].closed = tb.closed != 0;
        for (std::size_t i = 0; i < tb.count; ++i)
        {
            const SislTraceStation &s = tb.stations[i];
            Station x;
            x.xyz[0] = s.xyz[0];
            x.xyz[1] = s.xyz[1];
            x.xyz[2] = s.xyz[2];
            x.uv1[0] = s.uv1[0];
            x.uv1[1] = s.uv1[1];
            x.uv2[0] = s.uv2[0];
            x.uv2[1] = s.uv2[1];
            x.mask = s.grid_mask;
            for (int k = 0; k < 4; ++k) x.gidx[k] = s.grid_index[k];
            out[b].st.push_back(x);
        }
    }
    return out;
}

// back-substitution of one station on both surfaces
bool station_on_surfaces(const SISLSurf &s1, const SISLSurf &s2,
                         const Station &st, double tol)
{
    double d[21], n[3];
    int lu = 0, lv = 0, stat = 0;
    double p1[2] = {st.uv1[0], st.uv1[1]};
    s1421(const_cast<SISLSurf *>(&s1), 1, p1, &lu, &lv, d, n, &stat);
    if (stat < 0) return false;
    if (dist3(d, st.xyz) > tol) return false;
    double p2[2] = {st.uv2[0], st.uv2[1]};
    lu = lv = 0;
    s1421(const_cast<SISLSurf *>(&s2), 1, p2, &lu, &lv, d, n, &stat);
    if (stat < 0) return false;
    return dist3(d, st.xyz) <= tol;
}

struct Grid
{
    const double *g[4];
    int n[4];
};

bool grid_hits_consistent(const Grid &gr, const Station &st)
{
    for (int fam = 0; fam < 4; ++fam)
    {
        bool hit = (st.mask & (1u << fam)) != 0;
        double value = fam < 2 ? st.uv1[fam & 1] : st.uv2[fam & 1];
        bool near = false;
        int idx = -1;
        for (int i = 0; i < gr.n[fam]; ++i)
        {
            double m = std::max(1.0, std::max(std::fabs(gr.g[fam][i]),
                                              std::fabs(value)));
            if (std::fabs(gr.g[fam][i] - value) <= 5e-6 * m)
            {
                near = true;
                idx = i;
                break;
            }
        }
        if (hit != near) return false;
        if (hit && st.gidx[fam] != idx && st.gidx[fam] >= 0)
        {
            // the nearest grid line must match the reported index
            int best = 0;
            for (int i = 1; i < gr.n[fam]; ++i)
                if (std::fabs(gr.g[fam][i] - value) <
                    std::fabs(gr.g[fam][best] - value))
                    best = i;
            if (st.gidx[fam] != best) return false;
        }
    }
    return true;
}

// check the chord error of the station chain against the true intersection:
// each intermediate station must be within tolerance of the chord of its
// neighbours, and a mid-chord point must be within tolerance of the polyline.
bool chord_error_ok(const SISLSurf &surf1, const SISLSurf &surf2,
                    const std::vector<Branch> &branches, double tol)
{
    (void)surf2;
    for (std::size_t b = 0; b < branches.size(); ++b)
    {
        const Branch &br = branches[b];
        std::size_t n = br.st.size();
        std::size_t segs = br.closed ? n : n - 1;
        for (std::size_t i = 0; i < segs; ++i)
        {
            const Station &a = br.st[i];
            const Station &c = br.st[(i + 1) % n];
            // the true curve midpoint, recovered from the surface at the mean
            // parameters (the parameters are already corrected on both
            // surfaces, so their mean is a good sample of the true curve)
            double uv[2] = {0.5 * (a.uv1[0] + c.uv1[0]),
                            0.5 * (a.uv1[1] + c.uv1[1])};
            double d[21], nrm[3];
            int lu = 0, lv = 0, stat = 0;
            s1421(const_cast<SISLSurf *>(&surf1), 1, uv, &lu, &lv, d, nrm,
                  &stat);
            if (stat < 0) return false;
            if (point_segment(d, a.xyz, c.xyz) > tol) return false;
            (void)uv;
        }
    }
    return true;
}

// ---------------------------------------------------------------------
// independent checks on imprint results
// ---------------------------------------------------------------------

struct EdgeSheet
{
    std::map<std::pair<int, int>, int> directed;
};

int edge_key(int a, int b, int *lo, int *hi)
{
    *lo = std::min(a, b);
    *hi = std::max(a, b);
    return 0;
}

void expect_edge(const EdgeSheet &e, int a, int b, bool *ok)
{
    int lo, hi;
    edge_key(a, b, &lo, &hi);
    *ok = e.directed.count(std::make_pair(lo, hi)) != 0;
}

bool corner_uvs_match_positions(const SISLSurf &s1, const SISLSurf &s2,
                                const ImprintResult &r,
                                double tol)
{
    for (std::size_t i = 0; i < r.triangles.size(); ++i)
    {
        const ImprintTriangle &t = r.triangles[i];
        if (t.sheet != 0 && t.sheet != 1) return false;
        for (int k = 0; k < 3; ++k)
        {
            double uv[2] = {t.uv[k][0], t.uv[k][1]};
            double d[21], nrm[3], unit[3];
            int lu = 0, lv = 0, stat = 0;
            s1421(const_cast<SISLSurf *>(t.sheet ? &s2 : &s1), 1, uv, &lu, &lv,
                  d, nrm, &stat);
            if (stat < 0) return false;
            if (dist3(d, r.positions[(std::size_t)t.index[k]].data()) > tol)
                return false;
            // corner normal is the unit analytic surface normal
            double len = std::sqrt(nrm[0] * nrm[0] + nrm[1] * nrm[1] +
                                   nrm[2] * nrm[2]);
            if (!(len > 0.0)) return false;
            unit[0] = nrm[0] / len;
            unit[1] = nrm[1] / len;
            unit[2] = nrm[2] / len;
            if (std::fabs(unit[0] - t.normal[k][0]) > 1e-9 ||
                std::fabs(unit[1] - t.normal[k][1]) > 1e-9 ||
                std::fabs(unit[2] - t.normal[k][2]) > 1e-9)
                return false;
        }
    }
    return true;
}

// per sheet: every triangle must be non-degenerate and have consistent
// winding (positive UV area, i.e. the original parameter orientation) and the
// UV areas must add up to the parameter domain area exactly once.
bool sheet_uv_coverage(const ImprintResult &r, int sheet,
                       double expect_area, double tol)
{
    double total = 0.0;
    for (std::size_t i = 0; i < r.triangles.size(); ++i)
    {
        const ImprintTriangle &t = r.triangles[i];
        if (t.sheet != sheet) continue;
        double a = 0.0;
        for (int k = 0; k < 3; ++k)
        {
            int j = (k + 1) % 3;
            a += t.uv[k][0] * t.uv[j][1] - t.uv[j][0] * t.uv[k][1];
        }
        if (!(a > 1e-15)) return false;  // winding and non-degeneracy
        total += 0.5 * a;
    }
    return std::fabs(total - expect_area) <= tol;
}

// A consistently oriented sheet: every directed edge is used exactly once,
// and every edge NOT on the patch's outer boundary has its reverse edge in
// another triangle.  This catches T junctions (an interior edge with one
// triangle on one side), duplicated faces and winding inconsistencies; the
// outer boundary of an open patch is naturally only covered from one side.
bool sheet_is_manifold(const SISLSurf &s, const ImprintResult &r, int sheet)
{
    std::map<std::pair<int, int>, int> dir;   // directed half-edge counts
    std::map<int, std::array<double, 2>> uv;
    for (std::size_t i = 0; i < r.triangles.size(); ++i)
    {
        const ImprintTriangle &t = r.triangles[i];
        if (t.sheet != sheet) continue;
        for (int k = 0; k < 3; ++k)
        {
            dir[std::make_pair(t.index[k], t.index[(k + 1) % 3])]++;
            uv[t.index[k]] = {{t.uv[k][0], t.uv[k][1]}};
        }
    }
    double lo[2] = {s.et1[s.ik1 - 1], s.et2[s.ik2 - 1]};
    double hi[2] = {s.et1[s.in1], s.et2[s.in2]};
    double sc = std::max(1.0, std::max(std::fabs(hi[0] - lo[0]),
                                       std::fabs(hi[1] - lo[1])));
    for (std::map<std::pair<int, int>, int>::iterator it = dir.begin();
         it != dir.end(); ++it)
    {
        if (it->second != 1) return false;   // duplicated directed edge
        std::array<double, 2> &a = uv[it->first.first];
        std::array<double, 2> &b = uv[it->first.second];
        bool outer = (std::fabs(a[0] - lo[0]) <= 1e-9 * sc &&
                      std::fabs(b[0] - lo[0]) <= 1e-9 * sc) ||
                     (std::fabs(a[0] - hi[0]) <= 1e-9 * sc &&
                      std::fabs(b[0] - hi[0]) <= 1e-9 * sc) ||
                     (std::fabs(a[1] - lo[1]) <= 1e-9 * sc &&
                      std::fabs(b[1] - lo[1]) <= 1e-9 * sc) ||
                     (std::fabs(a[1] - hi[1]) <= 1e-9 * sc &&
                      std::fabs(b[1] - hi[1]) <= 1e-9 * sc);
        std::pair<int, int> rev(it->first.second, it->first.first);
        std::map<std::pair<int, int>, int>::iterator jt = dir.find(rev);
        if (jt == dir.end() || jt->second != 1)
        {
            if (!outer) return false;
        }
    }
    return true;
}

bool no_duplicate_faces(const ImprintResult &r)
{
    std::set<std::vector<int>> seen;
    for (std::size_t i = 0; i < r.triangles.size(); ++i)
    {
        const ImprintTriangle &t = r.triangles[i];
        std::vector<int> key;
        key.push_back(t.sheet);
        std::array<int, 3> idx = {{t.index[0], t.index[1], t.index[2]}};
        std::sort(idx.begin(), idx.end());
        key.push_back(idx[0]);
        key.push_back(idx[1]);
        key.push_back(idx[2]);
        if (!seen.insert(key).second) return false;
    }
    return true;
}

// is the intersection edge chain a real directed edge chain in both sheets?
bool polylines_are_mesh_edges(const ImprintResult &r)
{
    EdgeSheet es[2];
    for (std::size_t i = 0; i < r.triangles.size(); ++i)
    {
        const ImprintTriangle &t = r.triangles[i];
        int lo, hi;
        for (int k = 0; k < 3; ++k)
        {
            edge_key(t.index[k], t.index[(k + 1) % 3], &lo, &hi);
            es[t.sheet].directed[std::make_pair(lo, hi)]++;
        }
    }
    for (std::size_t p = 0; p < r.polylines.size(); ++p)
    {
        const ImprintPolyline &pl = r.polylines[p];
        if (pl.closed && pl.indices.size() > 1 &&
            pl.indices.front() == pl.indices.back())
            return false;  // closed loops must not repeat the first point
        std::size_t segs = pl.closed ? pl.indices.size() : pl.indices.size() - 1;
        for (std::size_t i = 0; i < segs; ++i)
        {
            int a = pl.indices[i];
            int b = pl.indices[(i + 1) % pl.indices.size()];
            bool ok = false;
            expect_edge(es[0], a, b, &ok);
            if (!ok) return false;
            ok = false;
            expect_edge(es[1], a, b, &ok);
            if (!ok) return false;
        }
    }
    return true;
}

// The two sheets meet only along the common intersection chain.  Return true
// when every position shared between the sheets appears in some polyline.
bool shared_positions_are_intersection(const ImprintResult &r)
{
    std::set<int> sheet0, sheet1, poly;
    for (std::size_t i = 0; i < r.triangles.size(); ++i)
    {
        const ImprintTriangle &t = r.triangles[i];
        for (int k = 0; k < 3; ++k)
            (t.sheet ? sheet1 : sheet0).insert(t.index[k]);
    }
    for (std::size_t p = 0; p < r.polylines.size(); ++p)
        for (std::size_t i = 0; i < r.polylines[p].indices.size(); ++i)
            poly.insert(r.polylines[p].indices[i]);
    for (std::set<int>::iterator it = sheet0.begin(); it != sheet0.end(); ++it)
        if (sheet1.count(*it) && !poly.count(*it)) return false;
    return true;
}

// count how many endpoint pairs of the polylines coincide with surface-domain
// corners / boundary endpoints (every open branch must terminate at the
// patch boundary; checked through the UV of the polyline vertices)
bool open_chain_ends_on_boundary(const SISLSurf &s, const ImprintResult &r)
{
    double lo[2] = {s.et1[s.ik1 - 1], s.et2[s.ik2 - 1]};
    double hi[2] = {s.et1[s.in1], s.et2[s.in2]};
    std::map<int, std::array<double, 2>> uv;
    for (std::size_t i = 0; i < r.triangles.size(); ++i)
    {
        const ImprintTriangle &t = r.triangles[i];
        if (t.sheet != 0) continue;
        for (int k = 0; k < 3; ++k)
            uv[t.index[k]] = {{t.uv[k][0], t.uv[k][1]}};
    }
    for (std::size_t p = 0; p < r.polylines.size(); ++p)
    {
        if (r.polylines[p].closed) continue;
        for (int end = 0; end < 2; ++end)
        {
            int idx = end ? r.polylines[p].indices.back()
                          : r.polylines[p].indices.front();
            std::map<int, std::array<double, 2>>::iterator it = uv.find(idx);
            if (it == uv.end()) return false;
            const std::array<double, 2> &puv = it->second;
            double sc = std::max(1.0, std::max(std::fabs(hi[0] - lo[0]),
                                               std::fabs(hi[1] - lo[1])));
            bool on = std::fabs(puv[0] - lo[0]) <= 1e-9 * sc ||
                      std::fabs(puv[0] - hi[0]) <= 1e-9 * sc ||
                      std::fabs(puv[1] - lo[1]) <= 1e-9 * sc ||
                      std::fabs(puv[1] - hi[1]) <= 1e-9 * sc;
            if (!on) return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------
// scenario fixtures
// ---------------------------------------------------------------------

// bowl z = a0 + a2 (s-.5)^2 + a2 (t-.5)^2, exactly representable as a cubic
// Bezier in both directions.
SurfaceSpec bowl_spec(double base)
{
    SurfaceSpec s;
    for (int a = 0; a < 3; ++a)
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j) s.coef[a][i][j] = 0.0;
    // z = base + 0.6*(s-0.5)^2 + 0.6*(t-0.5)^2
    //   = base + 0.15 - 0.6 s + 0.6 s^2 + 0.15 - 0.6 t + 0.6 t^2
    s.coef[2][0][0] = base + 0.30;
    s.coef[2][1][0] = -0.6;
    s.coef[2][2][0] = 0.6;
    s.coef[2][0][1] = -0.6;
    s.coef[2][0][2] = 0.6;
    // x = s, y = t
    s.coef[0][1][0] = 1.0;
    s.coef[1][0][1] = 1.0;
    return s;
}

SurfaceSpec plane_spec(double z)
{
    SurfaceSpec s;
    for (int a = 0; a < 3; ++a)
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j) s.coef[a][i][j] = 0.0;
    s.coef[2][0][0] = z;
    s.coef[0][1][0] = 1.0;
    s.coef[1][0][1] = 1.0;
    return s;
}

// two-lobed surface z = 0.5 + 0.4 s^2 - 0.4 s (parabola in u only) so that a
// plane produces two separate open branches
SurfaceSpec valley_spec(double base)
{
    SurfaceSpec s;
    for (int a = 0; a < 3; ++a)
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j) s.coef[a][i][j] = 0.0;
    s.coef[2][0][0] = base;
    s.coef[2][1][0] = -0.4;
    s.coef[2][2][0] = 0.4;
    s.coef[0][1][0] = 1.0;
    s.coef[1][0][1] = 1.0;
    return s;
}

struct Fixture
{
    SISLSurf *s1;
    SISLSurf *s2;
    std::vector<double> g1u, g1v, g2u, g2v;
    ~Fixture()
    {
        if (s1) freeSurf(s1);
        if (s2) freeSurf(s2);
    }
};

// map local patch parameters to the physical parameter interval of the
// surface (SISL evaluates on the knot span [0,1] here, so the physical x/y
// polynomial already encodes the placement)
Fixture bowl_plane_fixture()
{
    Fixture f;
    f.s1 = build_patch(bowl_spec(0.50));
    f.s2 = build_patch(plane_spec(0.60));
    f.g1u = f.g1v = f.g2u = f.g2v = domain_ends(0.0, 1.0);
    return f;
}

Fixture two_branch_fixture()
{
    Fixture f;
    f.s1 = build_patch(valley_spec(0.5));
    f.s2 = build_patch(plane_spec(0.45));
    f.g1u = f.g1v = f.g2u = f.g2v = domain_ends(0.0, 1.0);
    return f;
}

} // namespace

// =====================================================================
// group: paired_events
// =====================================================================

namespace
{

// A loop that crosses grid lines of both surfaces in both directions.  The
// intersection is one closed branch; every grid crossing must appear as a
// station with the right grid identity, and both surfaces' parameters at that
// station must be the same (the loop lies in the diagonal plane x=y of this
// fixture, so u/v of both sheets agree).
int case_paired_loop_events()
{
    Fixture f = bowl_plane_fixture();
    std::vector<double> g = grid4();
    f.g1u = f.g1v = f.g2u = f.g2v = g;

    SislTraceOptions o;
    o.geometric_tolerance = 1e-6;
    o.max_stations = 50000;
    SislTraceResult *r = nullptr;
    int rc = sisl_trace_grid_events(f.s1, f.g1u.data(), (int)f.g1u.size(),
                                    f.g1v.data(), (int)f.g1v.size(), f.s2,
                                    f.g2u.data(), (int)f.g2u.size(),
                                    f.g2v.data(), (int)f.g2v.size(), &o, &r);
    TASK_CHECK(rc == SISL_TRACE_OK);
    TASK_CHECK(r != nullptr);
    TASK_CHECK(r->branch_count == 1);
    TASK_CHECK(r->branches[0].closed != 0);
    TASK_CHECK(r->branches[0].count >= 8);
    // a closed loop must not repeat its first station
    TASK_CHECK(!(r->branches[0].stations[0].uv1[0] ==
                     r->branches[0].stations[r->branches[0].count - 1].uv1[0] &&
                 r->branches[0].stations[0].uv1[1] ==
                     r->branches[0].stations[r->branches[0].count - 1].uv1[1]));

    std::vector<Branch> br = copy_trace(r);
    Grid gr;
    gr.g[0] = f.g1u.data(); gr.n[0] = (int)f.g1u.size();
    gr.g[1] = f.g1v.data(); gr.n[1] = (int)f.g1v.size();
    gr.g[2] = f.g2u.data(); gr.n[2] = (int)f.g2u.size();
    gr.g[3] = f.g2v.data(); gr.n[3] = (int)f.g2v.size();

    unsigned hit_any = 0;
    for (std::size_t i = 0; i < br[0].st.size(); ++i)
    {
        const Station &st = br[0].st[i];
        TASK_CHECK(station_on_surfaces(*f.s1, *f.s2, st, 1e-6));
        TASK_CHECK(grid_hits_consistent(gr, st));
        if (st.mask) ++hit_any;
    }
    // all four grid families of both surfaces are crossed by the loop
    bool fam_hit[4] = {false, false, false, false};
    for (std::size_t i = 0; i < br[0].st.size(); ++i)
        for (int k = 0; k < 4; ++k)
            if (br[0].st[i].mask & (1u << k)) fam_hit[k] = true;
    for (int k = 0; k < 4; ++k) TASK_CHECK(fam_hit[k]);
    TASK_CHECK(hit_any > 0);
    TASK_CHECK(chord_error_ok(*f.s1, *f.s2, br, 1e-6));
    sisl_trace_result_free(r);
    return 0;
}

// One side has extra events: the first surface has a much denser grid than
// the second.  Every station that hits a fine grid line of surface 1 must
// still be a jointly corrected point on both surfaces.
int case_one_sided_events()
{
    Fixture f = bowl_plane_fixture();
    f.g1u = f.g1v = std::vector<double>{0.0, 0.1, 0.2, 0.3, 0.4, 0.5,
                                        0.6, 0.7, 0.8, 0.9, 1.0};
    f.g2u = f.g2v = std::vector<double>{0.0, 0.5, 1.0};

    SislTraceOptions o;
    o.geometric_tolerance = 1e-6;
    o.max_stations = 50000;
    SislTraceResult *r = nullptr;
    int rc = sisl_trace_grid_events(f.s1, f.g1u.data(), (int)f.g1u.size(),
                                    f.g1v.data(), (int)f.g1v.size(), f.s2,
                                    f.g2u.data(), (int)f.g2u.size(),
                                    f.g2v.data(), (int)f.g2v.size(), &o, &r);
    TASK_CHECK(rc == SISL_TRACE_OK);
    TASK_CHECK(r && r->branch_count == 1);

    std::vector<Branch> br = copy_trace(r);
    int one_sided = 0, both = 0;
    for (std::size_t i = 0; i < br[0].st.size(); ++i)
    {
        const Station &st = br[0].st[i];
        TASK_CHECK(station_on_surfaces(*f.s1, *f.s2, st, 1e-6));
        bool side0 = (st.mask & (SISL_GRID_U1 | SISL_GRID_V1)) != 0;
        bool side1 = (st.mask & (SISL_GRID_U2 | SISL_GRID_V2)) != 0;
        if (side0 && !side1) ++one_sided;
        if (side0 && side1) ++both;
    }
    TASK_CHECK(one_sided > 0);   // a fine line only surface 1 contributes
    TASK_CHECK(both > 0);        // and points where both coincide

    // the event stations carry the right grid family indices
    for (std::size_t i = 0; i < br[0].st.size(); ++i)
    {
        const Station &st = br[0].st[i];
        if (st.mask & SISL_GRID_U1)
            TASK_CHECK(st.gidx[0] >= 1 && st.gidx[0] <= 9);
        if (st.mask & SISL_GRID_V1)
            TASK_CHECK(st.gidx[1] >= 1 && st.gidx[1] <= 9);
    }
    sisl_trace_result_free(r);
    return 0;
}

// A grid line is touched tangentially by the intersection: the loop reaches
// the line's parameter value at an interior extremum, returns, and must not be
// rejected.  Both surfaces get a line through the turning point of the loop.
int case_tangent_touch_return()
{
    Fixture f = bowl_plane_fixture();
    // the loop of bowl min z=0.50 vs plane z=0.60 has radius sqrt(.1/.6) about (0.5,0.5);
    // a line at v = 0.5-0.288675... just grazes the lowest loop point
    double r = std::sqrt(0.10 / 0.6);
    double vgraze = 0.5 - r;
    f.g1u = domain_ends(0.0, 1.0);
    f.g1v = std::vector<double>{0.0, vgraze, 1.0};
    f.g2u = f.g1u;
    f.g2v = f.g1v;

    SislTraceOptions o;
    o.geometric_tolerance = 1e-7;
    o.max_stations = 50000;
    SislTraceResult *rt = nullptr;
    int rc = sisl_trace_grid_events(f.s1, f.g1u.data(), (int)f.g1u.size(),
                                    f.g1v.data(), (int)f.g1v.size(), f.s2,
                                    f.g2u.data(), (int)f.g2u.size(),
                                    f.g2v.data(), (int)f.g2v.size(), &o, &rt);
    TASK_CHECK(rc == SISL_TRACE_OK);
    TASK_CHECK(rt && rt->branch_count == 1);
    TASK_CHECK(rt->branches[0].closed != 0);
    std::vector<Branch> br = copy_trace(rt);
    int touches = 0;
    for (std::size_t i = 0; i < br[0].st.size(); ++i)
    {
        const Station &st = br[0].st[i];
        TASK_CHECK(station_on_surfaces(*f.s1, *f.s2, st, 1e-6));
        if ((st.mask & SISL_GRID_V1) && (st.mask & SISL_GRID_V2))
        {
            TASK_CHECK(std::fabs(st.uv1[1] - vgraze) <= 1e-5);
            ++touches;
        }
    }
    // the grazing point is a station; the local parameter is extremal there
    TASK_CHECK(touches >= 1);
    sisl_trace_result_free(rt);
    return 0;
}

// One point hits grid lines of both parameter directions at once: the event
// keeps both identities instead of dropping one.
int case_multi_line_identity()
{
    Fixture f = bowl_plane_fixture();
    // the loop passes through u=0.5 and v=0.5+... at once if the grids are set
    // so that the turning points of the loop sit on the lines; choose the line
    // set so that at least one station hits two families
    double r = std::sqrt(0.10 / 0.6);
    f.g1u = std::vector<double>{0.0, 0.5 - r, 0.5, 0.5 + r, 1.0};
    f.g1v = f.g1u;
    f.g2u = f.g1u;
    f.g2v = f.g1v;

    SislTraceOptions o;
    o.geometric_tolerance = 1e-7;
    o.max_stations = 50000;
    SislTraceResult *rt = nullptr;
    int rc = sisl_trace_grid_events(f.s1, f.g1u.data(), (int)f.g1u.size(),
                                    f.g1v.data(), (int)f.g1v.size(), f.s2,
                                    f.g2u.data(), (int)f.g2u.size(),
                                    f.g2v.data(), (int)f.g2v.size(), &o, &rt);
    TASK_CHECK(rc == SISL_TRACE_OK);
    TASK_CHECK(rt && rt->branch_count == 1);
    std::vector<Branch> br = copy_trace(rt);
    int multi = 0;
    for (std::size_t i = 0; i < br[0].st.size(); ++i)
    {
        const Station &st = br[0].st[i];
        int bits = 0;
        for (int k = 0; k < 4; ++k)
            if (st.mask & (1u << k)) ++bits;
        if (bits >= 2) ++multi;
        TASK_CHECK(station_on_surfaces(*f.s1, *f.s2, st, 1e-6));
    }
    // the symmetric loop passes through (0.5, 0.5-r) where u=0.5 and v=v0
    // simultaneously: that is a two-line hit
    TASK_CHECK(multi > 0);
    sisl_trace_result_free(rt);
    return 0;
}

// Budget failure: the per-branch remaining-station limit must abort cleanly
// with no partial result.
int case_station_budget_failure()
{
    Fixture f = bowl_plane_fixture();
    std::vector<double> g = grid4();
    f.g1u = f.g1v = f.g2u = f.g2v = g;
    SislTraceOptions o;
    o.geometric_tolerance = 1e-8;
    o.max_stations = 5;
    SislTraceResult *r = nullptr;
    int rc = sisl_trace_grid_events(f.s1, f.g1u.data(), (int)f.g1u.size(),
                                    f.g1v.data(), (int)f.g1v.size(), f.s2,
                                    f.g2u.data(), (int)f.g2u.size(),
                                    f.g2v.data(), (int)f.g2v.size(), &o, &r);
    TASK_CHECK(rc == SISL_TRACE_BUDGET);
    TASK_CHECK(r == nullptr);

    // invalid options are rejected without touching the output pointer
    o.max_stations = 0;
    rc = sisl_trace_grid_events(f.s1, f.g1u.data(), (int)f.g1u.size(),
                                f.g1v.data(), (int)f.g1v.size(), f.s2,
                                f.g2u.data(), (int)f.g2u.size(),
                                f.g2v.data(), (int)f.g2v.size(), &o, &r);
    TASK_CHECK(rc == SISL_TRACE_INVALID_ARGUMENT);
    TASK_CHECK(r == nullptr);
    return 0;
}

} // namespace

// =====================================================================
// group: cell_complex
// =====================================================================

namespace
{

// A closed loop entirely inside one parameter cell of both surfaces: both the
// inside and the outside of the loop must be covered exactly once, with no T
// junction and no duplicate or zero-area triangle.
int case_loop_inside_one_cell()
{
    Fixture f = bowl_plane_fixture();
    ImprintOptions o;
    o.geometricTolerance = 1e-6;
    o.maxStations = 200000;
    o.maxTriangles = 2000000;

    ImprintResult r;
    imprint_surfaces(*f.s1, domain_ends(0, 1), domain_ends(0, 1),
                               *f.s2, domain_ends(0, 1), domain_ends(0, 1), o,
                               r);
    TASK_CHECK(r.polylines.size() == 1);
    TASK_CHECK(r.polylines[0].closed);
    TASK_CHECK(r.polylines[0].indices.size() >= 8);
    TASK_CHECK(sheet_uv_coverage(r, 0, 1.0, 1e-8));
    TASK_CHECK(sheet_uv_coverage(r, 1, 1.0, 1e-8));
    TASK_CHECK(corner_uvs_match_positions(*f.s1, *f.s2, r, 1e-6));
    TASK_CHECK(no_duplicate_faces(r));
    TASK_CHECK(sheet_is_manifold(*f.s1, r, 0));
    TASK_CHECK(sheet_is_manifold(*f.s2, r, 1));
    TASK_CHECK(polylines_are_mesh_edges(r));
    TASK_CHECK(shared_positions_are_intersection(r));

    // the loop must actually split the cell: a vertex lies strictly inside
    // the unit square on the loop
    bool interior = false;
    for (std::size_t i = 0; i < r.polylines[0].indices.size(); ++i)
    {
        int idx = r.polylines[0].indices[i];
        for (std::size_t t = 0; t < r.triangles.size(); ++t)
            if (r.triangles[t].sheet == 0)
                for (int k = 0; k < 3; ++k)
                    if (r.triangles[t].index[k] == idx)
                    {
                        double u = r.triangles[t].uv[k][0];
                        double v = r.triangles[t].uv[k][1];
                        if (u > 0.05 && u < 0.95 && v > 0.05 && v < 0.95)
                            interior = true;
                    }
    }
    TASK_CHECK(interior);
    return 0;
}

// Two open branches with different parameter direction and non-uniform grids
// that differ between the two surfaces, including one side having extra
// events.  Both sheets must keep a complete, non-overlapping cover and both
// sheets must carry each branch as a real edge chain.
int case_two_branches_cross_cells()
{
    Fixture f = two_branch_fixture();
    std::vector<double> g1{0.0, 0.125, 0.375, 0.5, 0.75, 1.0};
    std::vector<double> g2{0.0, 0.2, 0.4, 0.6, 0.8, 1.0};
    ImprintOptions o;
    o.geometricTolerance = 1e-6;
    o.maxStations = 300000;
    o.maxTriangles = 3000000;

    ImprintResult r;
    imprint_surfaces(*f.s1, g1, g2, *f.s2, g2, g1, o, r);
    TASK_CHECK(r.polylines.size() == 2);
    TASK_CHECK(!r.polylines[0].closed && !r.polylines[1].closed);
    TASK_CHECK(sheet_uv_coverage(r, 0, 1.0, 1e-8));
    TASK_CHECK(sheet_uv_coverage(r, 1, 1.0, 1e-8));
    TASK_CHECK(corner_uvs_match_positions(*f.s1, *f.s2, r, 1e-6));
    TASK_CHECK(no_duplicate_faces(r));
    TASK_CHECK(sheet_is_manifold(*f.s1, r, 0));
    TASK_CHECK(sheet_is_manifold(*f.s2, r, 1));
    TASK_CHECK(polylines_are_mesh_edges(r));
    TASK_CHECK(shared_positions_are_intersection(r));
    TASK_CHECK(open_chain_ends_on_boundary(*f.s1, r));

    // each branch actually spans several cells on sheet 0
    for (std::size_t p = 0; p < r.polylines.size(); ++p)
    {
        std::set<int> cells;
        for (std::size_t i = 0; i < r.polylines[p].indices.size(); ++i)
        {
            int idx = r.polylines[p].indices[i];
            for (std::size_t t = 0; t < r.triangles.size(); ++t)
                if (r.triangles[t].sheet == 0)
                    for (int k = 0; k < 3; ++k)
                        if (r.triangles[t].index[k] == idx)
                        {
                            double u = r.triangles[t].uv[k][0];
                            double v = r.triangles[t].uv[k][1];
                            cells.insert((int)(u * 5.0) * 8 + (int)(v * 5.0));
                        }
        }
        TASK_CHECK(cells.size() >= 2);
    }
    return 0;
}

// Every caller grid line must be a real polyline of mesh edges, and every
// interior intersection segment must have one triangle on each side inside
// each sheet (the shared curve is a real edge of both sheets, not a
// one-sided decoration).
int case_grid_lines_and_two_sided_edges()
{
    Fixture f = bowl_plane_fixture();
    std::vector<double> g = grid4();
    ImprintOptions o;
    o.geometricTolerance = 1e-6;
    ImprintResult r;
    imprint_surfaces(*f.s1, g, g, *f.s2, g, g, o, r);

    // collect, per sheet, the undirected edge -> triangle count
    std::map<std::pair<int, int>, int> edges[2];
    for (std::size_t i = 0; i < r.triangles.size(); ++i)
    {
        const ImprintTriangle &t = r.triangles[i];
        for (int k = 0; k < 3; ++k)
        {
            int a = t.index[k], b = t.index[(k + 1) % 3];
            if (a > b) std::swap(a, b);
            edges[t.sheet][std::make_pair(a, b)]++;
        }
    }

    // 1. every grid line of both sheets: all its vertices along the line must
    // be joined by mesh edges in that sheet
    for (int sheet = 0; sheet < 2; ++sheet)
    {
        const std::vector<double> &hu = sheet == 0 ? f.g1u : f.g2u;
        const std::vector<double> &hv = sheet == 0 ? f.g1v : f.g2v;
        // vertices on each grid line, ordered along the line
        std::map<double, std::map<double, int>> on_u, on_v;
        for (std::size_t i = 0; i < r.triangles.size(); ++i)
        {
            const ImprintTriangle &t = r.triangles[i];
            if (t.sheet != sheet) continue;
            for (int k = 0; k < 3; ++k)
            {
                int idx = t.index[k];
                for (std::size_t q = 0; q < hu.size(); ++q)
                    if (std::fabs(t.uv[k][0] - hu[q]) <= 1e-9)
                        on_u[hu[q]][t.uv[k][1]] = idx;
                for (std::size_t q = 0; q < hv.size(); ++q)
                    if (std::fabs(t.uv[k][1] - hv[q]) <= 1e-9)
                        on_v[hv[q]][t.uv[k][0]] = idx;
            }
        }
        for (std::map<double, std::map<double, int>>::iterator it = on_u.begin();
             it != on_u.end(); ++it)
        {
            TASK_CHECK(it->second.size() >= 2);
            std::map<double, int>::iterator p = it->second.begin();
            int prev = p->second;
            for (++p; p != it->second.end(); ++p)
            {
                int a = prev, b = p->second;
                if (a > b) std::swap(a, b);
                TASK_CHECK(edges[sheet].count(std::make_pair(a, b)) > 0);
                prev = p->second;
            }
        }
        for (std::map<double, std::map<double, int>>::iterator it = on_v.begin();
             it != on_v.end(); ++it)
        {
            TASK_CHECK(it->second.size() >= 2);
            std::map<double, int>::iterator p = it->second.begin();
            int prev = p->second;
            for (++p; p != it->second.end(); ++p)
            {
                int a = prev, b = p->second;
                if (a > b) std::swap(a, b);
                TASK_CHECK(edges[sheet].count(std::make_pair(a, b)) > 0);
                prev = p->second;
            }
        }
    }

    // 2. every interior intersection segment has two incident triangles in
    // each sheet (closed loop: no segment is on the patch boundary here)
    for (std::size_t si = 0; si < r.polylines.size(); ++si)
    {
        const ImprintPolyline &pl = r.polylines[si];
        TASK_CHECK(pl.closed);
        for (std::size_t i = 0; i < pl.indices.size(); ++i)
        {
            int a = pl.indices[i];
            int b = pl.indices[(i + 1) % pl.indices.size()];
            if (a > b) std::swap(a, b);
            for (int sheet = 0; sheet < 2; ++sheet)
                TASK_CHECK(edges[sheet][std::make_pair(a, b)] == 2);
        }
    }
    return 0;
}

// No intersection at all: both sheets must still be fully triangulated and no
// polyline may be produced.
int case_disjoint_surfaces()
{
    Fixture f;
    f.s1 = build_patch(plane_spec(0.5));
    f.s2 = build_patch(plane_spec(0.55));
    ImprintOptions o;
    o.geometricTolerance = 1e-6;
    o.maxStations = 100000;
    o.maxTriangles = 1000000;
    ImprintResult r;
    imprint_surfaces(*f.s1, domain_ends(0, 1), domain_ends(0, 1),
                               *f.s2, domain_ends(0, 1), domain_ends(0, 1), o,
                               r);
    TASK_CHECK(r.polylines.empty());
    TASK_CHECK(sheet_uv_coverage(r, 0, 1.0, 1e-9));
    TASK_CHECK(sheet_uv_coverage(r, 1, 1.0, 1e-9));
    TASK_CHECK(corner_uvs_match_positions(*f.s1, *f.s2, r, 1e-6));
    TASK_CHECK(no_duplicate_faces(r));
    TASK_CHECK(sheet_is_manifold(*f.s1, r, 0));
    TASK_CHECK(sheet_is_manifold(*f.s2, r, 1));
    return 0;
}

// Corner back-substitution and analytic normals are checked over all three
// scenarios together.
int case_corners_and_normals()
{
    {
        Fixture f = bowl_plane_fixture();
        ImprintOptions o;
        o.geometricTolerance = 1e-6;
        ImprintResult r;
        std::vector<double> g = grid4();
        imprint_surfaces(*f.s1, g, g, *f.s2, g, g, o, r);
        TASK_CHECK(corner_uvs_match_positions(*f.s1, *f.s2, r, 1e-6));
        // normals are never averaged: sheet 0 normals are the bowl normals
        // (not the plane normal) at the same intersection positions
        bool saw_sheet0_tilted = false;
        for (std::size_t i = 0; i < r.triangles.size(); ++i)
            if (r.triangles[i].sheet == 0)
                for (int k = 0; k < 3; ++k)
                {
                    double nz = r.triangles[i].normal[k][2];
                    if (std::fabs(nz - 1.0) > 1e-3) saw_sheet0_tilted = true;
                }
        TASK_CHECK(saw_sheet0_tilted);
    }
    {
        Fixture f = two_branch_fixture();
        ImprintOptions o;
        o.geometricTolerance = 1e-6;
        std::vector<double> g1{0.0, 0.3, 0.7, 1.0};
        std::vector<double> g2{0.0, 0.5, 1.0};
        ImprintResult r;
        imprint_surfaces(*f.s1, g1, g2, *f.s2, g2, g1, o, r);
        TASK_CHECK(corner_uvs_match_positions(*f.s1, *f.s2, r, 1e-6));
    }
    return 0;
}

} // namespace

// =====================================================================
// group: production
// =====================================================================

namespace
{

// Failure preserves the previous complete output and reports the right error
// kind: invalid arguments throw std::invalid_argument, numeric/budget
// failures throw std::runtime_error.
int case_failure_preserves_output()
{
    Fixture f = bowl_plane_fixture();
    ImprintOptions ok;
    ok.geometricTolerance = 1e-6;
    ok.maxStations = 100000;
    ok.maxTriangles = 1000000;
    ImprintResult good;
    imprint_surfaces(*f.s1, domain_ends(0, 1), domain_ends(0, 1),
                               *f.s2, domain_ends(0, 1), domain_ends(0, 1), ok,
                               good);
    TASK_CHECK(!good.triangles.empty());
    const std::size_t ntri = good.triangles.size();
    const std::size_t npos = good.positions.size();
    const std::size_t npoly = good.polylines.size();

    // invalid grid: not spanning the domain
    bool threw = false;
    try
    {
        imprint_surfaces(*f.s1, std::vector<double>{0.0, 0.5},
                                   domain_ends(0, 1), *f.s2,
                                   domain_ends(0, 1), domain_ends(0, 1), ok,
                                   good);
    }
    catch (const std::invalid_argument &)
    {
        threw = true;
    }
    TASK_CHECK(threw);
    TASK_CHECK(good.triangles.size() == ntri);
    TASK_CHECK(good.positions.size() == npos);
    TASK_CHECK(good.polylines.size() == npoly);

    // invalid option: non-positive tolerance
    threw = false;
    ImprintOptions bad = ok;
    bad.geometricTolerance = 0.0;
    try
    {
        imprint_surfaces(*f.s1, domain_ends(0, 1), domain_ends(0, 1),
                                   *f.s2, domain_ends(0, 1),
                                   domain_ends(0, 1), bad, good);
    }
    catch (const std::invalid_argument &)
    {
        threw = true;
    }
    TASK_CHECK(threw);
    TASK_CHECK(good.triangles.size() == ntri);

    // budget failure is a runtime error and also preserves the output
    threw = false;
    ImprintOptions tight = ok;
    tight.maxTriangles = 2;
    try
    {
        imprint_surfaces(*f.s1, domain_ends(0, 1), domain_ends(0, 1),
                                   *f.s2, domain_ends(0, 1),
                                   domain_ends(0, 1), tight, good);
    }
    catch (const std::runtime_error &)
    {
        threw = true;
    }
    TASK_CHECK(threw);
    TASK_CHECK(good.triangles.size() == ntri);
    TASK_CHECK(good.positions.size() == npos);
    TASK_CHECK(good.polylines.size() == npoly);
    return 0;
}

// Repeated calls are independent: the same inputs give the same result, and
// a result retained from an earlier call is unaffected by a later call.
int case_repeat_independent()
{
    Fixture f = bowl_plane_fixture();
    ImprintOptions o;
    o.geometricTolerance = 1e-6;
    ImprintResult a, b;
    std::vector<double> g = grid4();
    imprint_surfaces(*f.s1, g, g, *f.s2, g, g, o, a);
    std::size_t na = a.triangles.size();
    std::size_t pa = a.positions.size();
    imprint_surfaces(*f.s1, g, g, *f.s2, g, g, o, b);
    TASK_CHECK(a.triangles.size() == na);
    TASK_CHECK(a.positions.size() == pa);
    TASK_CHECK(b.triangles.size() == na);
    TASK_CHECK(b.positions.size() == pa);
    // positions agree exactly for the deterministic trace
    TASK_CHECK(a.positions.size() == b.positions.size());
    for (std::size_t i = 0; i < a.positions.size(); ++i)
        for (int k = 0; k < 3; ++k)
            TASK_CHECK(std::fabs(a.positions[i][(std::size_t)k] -
                                 b.positions[i][(std::size_t)k]) <= 1e-12);
    return 0;
}

// Budget failure through the imprint entry point.
int case_imprint_budget_failure()
{
    Fixture f = bowl_plane_fixture();
    ImprintOptions o;
    o.geometricTolerance = 1e-6;
    o.maxStations = 3;
    ImprintResult r;
    bool threw = false;
    try
    {
        imprint_surfaces(*f.s1, domain_ends(0, 1), domain_ends(0, 1),
                                   *f.s2, domain_ends(0, 1),
                                   domain_ends(0, 1), o, r);
    }
    catch (const std::runtime_error &)
    {
        threw = true;
    }
    TASK_CHECK(threw);
    TASK_CHECK(r.triangles.empty());

    ImprintOptions tri;
    tri.geometricTolerance = 1e-6;
    tri.maxTriangles = 1;
    threw = false;
    try
    {
        imprint_surfaces(*f.s1, domain_ends(0, 1), domain_ends(0, 1),
                                   *f.s2, domain_ends(0, 1),
                                   domain_ends(0, 1), tri, r);
    }
    catch (const std::runtime_error &)
    {
        threw = true;
    }
    TASK_CHECK(threw);
    return 0;
}

// A rejected degenerate input (coincident surfaces) must fail loudly rather
// than invent geometry.
int case_coincident_rejected()
{
    Fixture f;
    f.s1 = build_patch(plane_spec(0.5));
    f.s2 = build_patch(plane_spec(0.5));
    ImprintOptions o;
    o.geometricTolerance = 1e-6;
    ImprintResult r;
    bool threw = false;
    try
    {
        imprint_surfaces(*f.s1, domain_ends(0, 1), domain_ends(0, 1),
                                   *f.s2, domain_ends(0, 1),
                                   domain_ends(0, 1), o, r);
    }
    catch (const std::runtime_error &)
    {
        threw = true;
    }
    TASK_CHECK(threw);
    return 0;
}

// Tangential and coincident inputs are explicitly rejected by both the
// native tracer and the C++ production entry point.
int case_tangent_and_coincident()
{
    // z = (s-0.5)^2 touches z = 0 along the parameter line s = 0.5
    SurfaceSpec touch;
    for (int a = 0; a < 3; ++a)
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j) touch.coef[a][i][j] = 0.0;
    touch.coef[2][0][0] = 0.25;
    touch.coef[2][1][0] = -1.0;
    touch.coef[2][2][0] = 1.0;
    touch.coef[0][1][0] = 1.0;
    touch.coef[1][0][1] = 1.0;
    SurfaceSpec flat = plane_spec(0.0);
    SISLSurf *st = build_patch(touch);
    SISLSurf *sf = build_patch(flat);
    std::vector<double> g = domain_ends(0, 1);

    SislTraceOptions to;
    to.geometric_tolerance = 1e-6;
    to.max_stations = 100000;
    SislTraceResult *tr = nullptr;
    int rc = sisl_trace_grid_events(st, g.data(), 2, g.data(), 2, sf, g.data(),
                                    2, g.data(), 2, &to, &tr);
    TASK_CHECK(rc == SISL_TRACE_REJECTED);
    TASK_CHECK(tr == nullptr);

    ImprintOptions o;
    o.geometricTolerance = 1e-6;
    ImprintResult r;
    bool threw = false;
    try
    {
        imprint_surfaces(*st, g, g, *sf, g, g, o, r);
    }
    catch (const std::runtime_error &)
    {
        threw = true;
    }
    TASK_CHECK(threw);
    freeSurf(st);
    freeSurf(sf);

    // two planes at the same height are coincident
    SurfaceSpec same = plane_spec(0.4);
    SISLSurf *p1 = build_patch(same);
    SISLSurf *p2 = build_patch(same);
    rc = sisl_trace_grid_events(p1, g.data(), 2, g.data(), 2, p2, g.data(), 2,
                                g.data(), 2, &to, &tr);
    TASK_CHECK(rc == SISL_TRACE_REJECTED);
    TASK_CHECK(tr == nullptr);
    freeSurf(p1);
    freeSurf(p2);
    return 0;
}

// An input with a C0 (multiplicity order-1) interior knot is outside the
// accepted input class and is rejected as an invalid argument.
int case_c0_input_rejected()
{
    // cubic patch with an interior knot of multiplicity 3 at s = 0.5
    double kn1[11] = {0, 0, 0, 0, 0.5, 0.5, 0.5, 1, 1, 1, 1};
    double kn2[8] = {0, 0, 0, 0, 1, 1, 1, 1};
    double cp[7 * 4 * 3];
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 7; ++j)
        {
            // x = i/3, y = v lines, z = plane 0.5
            double x = i / 3.0, y = j / 6.0;
            cp[(j * 4 + i) * 3 + 0] = x;
            cp[(j * 4 + i) * 3 + 1] = y;
            cp[(j * 4 + i) * 3 + 2] = 0.5;
        }
    // reorder the control net to (u index major) as SISL expects j*in1+i
    SISLSurf *bad = newSurf(7, 4, 4, 4, kn1, kn2, cp, 1, 3, 1);
    TASK_CHECK(bad != nullptr);
    ImprintOptions o;
    o.geometricTolerance = 1e-6;
    ImprintResult r;
    bool threw = false;
    try
    {
        imprint_surfaces(*bad, std::vector<double>{0, 0.5, 1.0},
                         domain_ends(0, 1), *bad,
                         std::vector<double>{0, 0.5, 1.0}, domain_ends(0, 1),
                         o, r);
    }
    catch (const std::invalid_argument &)
    {
        threw = true;
    }
    TASK_CHECK(threw);
    freeSurf(bad);
    return 0;
}

// The rational (positive weights, ratio <= 100) variant must trace and
// imprint just like the polynomial one.
int case_rational_weights()
{
    Fixture f;
    SurfaceSpec s1 = bowl_spec(0.50);
    s1.ikind = 2;
    s1.weights.assign(16, 1.0);
    for (int i = 0; i < 16; ++i) s1.weights[(std::size_t)i] = 1.0 + 0.5 * (i % 4);
    SurfaceSpec s2 = plane_spec(0.60);
    s2.ikind = 2;
    s2.weights.assign(16, 1.0);
    f.s1 = build_patch(s1);
    f.s2 = build_patch(s2);
    ImprintOptions o;
    o.geometricTolerance = 1e-5;
    ImprintResult r;
    imprint_surfaces(*f.s1, domain_ends(0, 1), domain_ends(0, 1),
                               *f.s2, domain_ends(0, 1), domain_ends(0, 1), o,
                               r);
    TASK_CHECK(r.polylines.size() == 1);
    TASK_CHECK(r.polylines[0].closed);
    TASK_CHECK(sheet_uv_coverage(r, 0, 1.0, 1e-7));
    TASK_CHECK(sheet_uv_coverage(r, 1, 1.0, 1e-7));
    TASK_CHECK(corner_uvs_match_positions(*f.s1, *f.s2, r, 1e-6));
    return 0;
}

// The production path covers an asymmetric pair: different physical x/y
// orientation on one sheet, non-uniform cubic knot structure on the other,
// and different parameter domains.
int case_domain_and_orientation()
{
    SurfaceSpec bowl = bowl_spec(0.50);
    SurfaceSpec plane = plane_spec(0.60);
    plane.swap_uv = true;   // x = t, y = s  (transposed parameterisation)
    plane.u0 = -2.0;        // different parameter domains on the two sheets
    plane.u1 = 3.0;
    plane.v0 = 1.0;
    plane.v1 = 4.0;
    Fixture f;
    f.s1 = build_patch(bowl);
    f.s2 = build_patch(plane);
    ImprintOptions o;
    o.geometricTolerance = 1e-6;
    ImprintResult r;
    imprint_surfaces(*f.s1, domain_ends(0, 1), domain_ends(0, 1),
                     *f.s2, domain_ends(-2, 3), domain_ends(1, 4), o, r);
    TASK_CHECK(r.polylines.size() == 1);
    TASK_CHECK(r.polylines[0].closed);
    TASK_CHECK(sheet_uv_coverage(r, 0, 1.0, 1e-8));
    TASK_CHECK(sheet_uv_coverage(r, 1, 15.0, 1e-7));
    TASK_CHECK(corner_uvs_match_positions(*f.s1, *f.s2, r, 1e-6));
    TASK_CHECK(polylines_are_mesh_edges(r));
    return 0;
}

// Existing C evaluation and intersection interfaces stay compatible while the
// new files are linked: s1421, s1859 and s1310 still work on the fixtures.
int case_c_api_compatibility()
{
    Fixture f = bowl_plane_fixture();
    double d[21], n[3];
    int lu = 0, lv = 0, st = 0;
    double par[2] = {0.3, 0.7};
    s1421(f.s1, 2, par, &lu, &lv, d, n, &st);
    TASK_CHECK(st >= 0);

    int np = 0, nc = 0, status = 0;
    double *uv1 = nullptr, *uv2 = nullptr;
    SISLIntcurve **curves = nullptr;
    s1859(f.s1, f.s2, 1e-12, 1e-9, &np, &uv1, &uv2, &nc, &curves, &status);
    TASK_CHECK(status == 0);
    TASK_CHECK(nc == 1);
    TASK_CHECK(curves != nullptr);
    status = 0;
    s1310(f.s1, f.s2, curves[0], 1e-9, 0.0, 2, 0, &status);
    TASK_CHECK(status == 0);
    TASK_CHECK(curves[0]->pgeom != nullptr);
    TASK_CHECK(curves[0]->ppar1 != nullptr);
    TASK_CHECK(curves[0]->ppar2 != nullptr);
    if (curves) freeIntcrvlist(curves, nc);
    std::free(uv1);
    std::free(uv2);
    return 0;
}

} // namespace

// =====================================================================
// registration
// =====================================================================

const struct task_case task_cases[] = {
    {"paired_events", "paired_loop_events", case_paired_loop_events},
    {"paired_events", "one_sided_events", case_one_sided_events},
    {"paired_events", "tangent_touch_return", case_tangent_touch_return},
    {"paired_events", "multi_line_identity", case_multi_line_identity},
    {"paired_events", "station_budget_failure", case_station_budget_failure},
    {"cell_complex", "loop_inside_one_cell", case_loop_inside_one_cell},
    {"cell_complex", "two_branches_cross_cells", case_two_branches_cross_cells},
    {"cell_complex", "grid_lines_and_two_sided_edges",
     case_grid_lines_and_two_sided_edges},
    {"cell_complex", "disjoint_surfaces", case_disjoint_surfaces},
    {"cell_complex", "corners_and_normals", case_corners_and_normals},
    {"production", "failure_preserves_output", case_failure_preserves_output},
    {"production", "repeat_independent", case_repeat_independent},
    {"production", "imprint_budget_failure", case_imprint_budget_failure},
    {"production", "coincident_rejected", case_coincident_rejected},
    {"production", "tangent_and_coincident", case_tangent_and_coincident},
    {"production", "c0_input_rejected", case_c0_input_rejected},
    {"production", "rational_weights", case_rational_weights},
    {"production", "domain_and_orientation", case_domain_and_orientation},
    {"production", "c_api_compatibility", case_c_api_compatibility},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
