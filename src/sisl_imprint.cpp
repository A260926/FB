// Constrained triangulation of two NURBS surfaces along their true
// intersection curves.  See include/sisl_imprint.hpp.
//
// Pipeline:
//   1. the native SISL tracer (s1859 discovery + the real s1310 march, with
//      dual-parameter grid events) produces the ordered common stations,
//   2. both parameter domains are split at every caller grid line into
//      rectangular cells,
//   3. each branch is broken into cell-local chains; every chain edge is a
//      constraint in both sheets,
//   4. each cell is triangulated as a constrained planar straight-line graph
//      (open chains split the cell; closed interior loops are bridged and
//      both sides are covered exactly once),
//   5. a vertex is shared across sheets only for a common intersection
//      station; otherwise it belongs to one surface's parameter identity.

#include "sisl_imprint.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <utility>

namespace sislmesh
{
namespace
{

// ---------------------------------------------------------------------
// basic types
// ---------------------------------------------------------------------

struct UV
{
    double u, v;
    bool operator==(const UV &o) const { return u == o.u && v == o.v; }
    bool operator<(const UV &o) const
    {
        if (u != o.u) return u < o.u;
        return v < o.v;
    }
};

struct SurfPoint
{
    double xyz[3];
    double normal[3];
    bool ok;
};

SurfPoint evaluate(const SISLSurf &s, double u, double v)
{
    double par[2] = {u, v};
    double der[21], nrm[3];
    int lu = 0, lv = 0, st = 0;
    SurfPoint r;
    s1421(const_cast<SISLSurf *>(&s), 2, par, &lu, &lv, der, nrm, &st);
    r.ok = st >= 0;
    r.xyz[0] = der[0];
    r.xyz[1] = der[1];
    r.xyz[2] = der[2];
    double len = std::sqrt(nrm[0] * nrm[0] + nrm[1] * nrm[1] + nrm[2] * nrm[2]);
    if (len > 0.0)
    {
        r.normal[0] = nrm[0] / len;
        r.normal[1] = nrm[1] / len;
        r.normal[2] = nrm[2] / len;
    }
    else
    {
        r.normal[0] = r.normal[1] = r.normal[2] = 0.0;
        r.ok = false;
    }
    return r;
}

int surf_ik(const SISLSurf &s, int d) { return d == 0 ? s.ik1 : s.ik2; }
int surf_in(const SISLSurf &s, int d) { return d == 0 ? s.in1 : s.in2; }
const double *surf_et(const SISLSurf &s, int d) { return d == 0 ? s.et1 : s.et2; }

double domain_lo(const SISLSurf &s, int d)
{
    return surf_et(s, d)[surf_ik(s, d) - 1];
}
double domain_hi(const SISLSurf &s, int d)
{
    return surf_et(s, d)[surf_in(s, d)];
}

double dist3(const double *a, const double *b)
{
    double x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
    return std::sqrt(x * x + y * y + z * z);
}

// ---------------------------------------------------------------------
// validation
// ---------------------------------------------------------------------

bool near_value(double a, double b)
{
    double m = std::max(1.0, std::max(std::fabs(a), std::fabs(b)));
    return std::fabs(a - b) <= 1e-12 * m;
}

void validate_surface(const SISLSurf &s, const char *which)
{
    if (!s.ecoef)
        throw std::invalid_argument(std::string(which) +
                                    ": missing control point data");
    if (s.idim != 3)
        throw std::invalid_argument(std::string(which) +
                                    ": surface must be three-dimensional");
    if (s.ikind != 1 && s.ikind != 2)
        throw std::invalid_argument(std::string(which) +
                                    ": unsupported surface kind");
    if (s.ik1 < 2 || s.ik1 > 4 || s.ik2 < 2 || s.ik2 > 4)
        throw std::invalid_argument(std::string(which) +
                                    ": surface order must be 2..4");
    if (s.in1 < s.ik1 || s.in2 < s.ik2)
        throw std::invalid_argument(std::string(which) +
                                    ": too few control points");
    if (s.ikind == 2 && !s.rcoef)
        throw std::invalid_argument(std::string(which) +
                                    ": missing rational weights");
    if (s.cuopen_1 != SISL_SURF_OPEN || s.cuopen_2 != SISL_SURF_OPEN)
        throw std::invalid_argument(std::string(which) +
                                    ": surface must be open and non-periodic");
    for (int d = 0; d < 2; ++d)
    {
        const double *k = surf_et(s, d);
        int order = surf_ik(s, d), ncp = surf_in(s, d);
        for (int i = 0; i < ncp + order; ++i)
            if (!std::isfinite(k[i]))
                throw std::invalid_argument(std::string(which) +
                                            ": non-finite knot");
        for (int i = 0; i < order; ++i)
            if (!near_value(k[i], k[order - 1]) ||
                !near_value(k[ncp + i], k[ncp]))
                throw std::invalid_argument(std::string(which) +
                                            ": surface knots are not clamped");
        if (!(k[ncp] > k[order - 1]))
            throw std::invalid_argument(std::string(which) +
                                        ": empty parameter domain");
        // every interior knot must have multiplicity at most order-2, i.e.
        // the input is at least C1 continuous
        for (int i = order; i < ncp; )
        {
            int j = i;
            while (j < ncp && k[j] == k[i]) ++j;
            if (j - i > order - 2)
                throw std::invalid_argument(
                    std::string(which) +
                    ": interior knot multiplicity above C1 continuity");
            i = j;
        }
    }
}

void validate_grid(const SISLSurf &s, int dir, const std::vector<double> &g,
                   const char *which)
{
    const double *k = surf_et(s, dir);
    int order = surf_ik(s, dir), ncp = surf_in(s, dir);
    double lo = k[order - 1], hi = k[ncp];
    double sc = std::max(1.0, std::max(std::fabs(lo), std::fabs(hi)));

    if (g.size() < 2)
        throw std::invalid_argument(std::string(which) +
                                    ": grid must contain both domain ends");
    for (std::size_t i = 0; i < g.size(); ++i)
        if (!std::isfinite(g[i]))
            throw std::invalid_argument(std::string(which) +
                                        ": non-finite grid line");
    if (std::fabs(g.front() - lo) > 1e-9 * sc ||
        std::fabs(g.back() - hi) > 1e-9 * sc)
        throw std::invalid_argument(std::string(which) +
                                    ": grid does not span the param domain");
    for (std::size_t i = 0; i + 1 < g.size(); ++i)
        if (!(g[i + 1] > g[i]))
            throw std::invalid_argument(std::string(which) +
                                        ": grid is not strictly increasing");

    for (int i = 0; i < ncp + order; ++i)
    {
        double kv = k[i];
        if (!(kv > lo) || !(kv < hi)) continue;
        std::vector<double>::const_iterator it =
            std::lower_bound(g.begin(), g.end(), kv);
        bool found = false;
        if (it != g.end() && std::fabs(*it - kv) <= 1e-9 * sc) found = true;
        if (!found && it != g.begin() && std::fabs(*(it - 1) - kv) <= 1e-9 * sc)
            found = true;
        if (!found)
            throw std::invalid_argument(std::string(which) +
                                        ": interior knot missing from grid");
    }
}

void validate_options(const ImprintOptions &o)
{
    if (!(o.geometricTolerance > 0.0) || !std::isfinite(o.geometricTolerance))
        throw std::invalid_argument("imprint: geometricTolerance must be a "
                                    "positive finite length");
    if (o.maxStations <= 0)
        throw std::invalid_argument("imprint: maxStations must be positive");
    if (o.maxTriangles <= 0)
        throw std::invalid_argument("imprint: maxTriangles must be positive");
}

// ---------------------------------------------------------------------
// per-sheet state
// ---------------------------------------------------------------------

struct Sheet
{
    const SISLSurf *surf = nullptr;
    int id = 0;
    std::vector<double> gu, gv;
    std::map<UV, int> pos_of_uv;         // exact surface parameter identity
    ImprintResult *res = nullptr;
};

UV snap_to_grid(const Sheet &sh, double u, double v, double tol)
{
    UV out{u, v};
    for (int axis = 0; axis < 2; ++axis)
    {
        const std::vector<double> &g = axis == 0 ? sh.gu : sh.gv;
        double x = axis == 0 ? u : v;
        std::vector<double>::const_iterator it =
            std::lower_bound(g.begin(), g.end(), x);
        double best = x;
        bool found = false;
        if (it != g.end() && std::fabs(*it - x) <= tol)
        {
            best = *it;
            found = true;
        }
        if (it != g.begin())
        {
            --it;
            if (std::fabs(*it - x) <= tol &&
                (!found || std::fabs(*it - x) < std::fabs(best - x)))
            {
                best = *it;
                found = true;
            }
        }
        if (found)
        {
            if (axis == 0) out.u = best; else out.v = best;
        }
    }
    return out;
}

int vertex_position(Sheet &sh, const UV &uv)
{
    std::map<UV, int>::iterator it = sh.pos_of_uv.find(uv);
    if (it != sh.pos_of_uv.end()) return it->second;
    SurfPoint p = evaluate(*sh.surf, uv.u, uv.v);
    if (!p.ok)
        throw std::runtime_error("imprint: surface evaluation failed");
    int idx = (int)sh.res->positions.size();
    sh.res->positions.push_back({p.xyz[0], p.xyz[1], p.xyz[2]});
    sh.pos_of_uv[uv] = idx;
    return idx;
}

// ---------------------------------------------------------------------
// cell graph
// ---------------------------------------------------------------------

struct CellGraph
{
    std::vector<UV> uv;
    std::vector<int> pos;
    std::vector<std::vector<int>> adj;

    int node(Sheet &sh, const UV &p)
    {
        for (std::size_t i = 0; i < uv.size(); ++i)
            if (uv[i] == p) return (int)i;
        int id = (int)uv.size();
        uv.push_back(p);
        pos.push_back(vertex_position(sh, p));
        adj.push_back(std::vector<int>());
        return id;
    }

    void edge(int a, int b)
    {
        if (a == b) return;
        for (std::size_t i = 0; i < adj[a].size(); ++i)
            if (adj[a][i] == b) return;
        adj[a].push_back(b);
        adj[b].push_back(a);
    }
};

double cross2(const UV &a, const UV &b, const UV &c)
{
    return (b.u - a.u) * (c.v - a.v) - (b.v - a.v) * (c.u - a.u);
}

bool on_boundary(const UV &p, double u0, double u1, double v0, double v1,
                 double tol)
{
    bool in_u = p.u >= u0 - tol && p.u <= u1 + tol;
    bool in_v = p.v >= v0 - tol && p.v <= v1 + tol;
    if (!in_u || !in_v) return false;
    return std::fabs(p.u - u0) <= tol || std::fabs(p.u - u1) <= tol ||
           std::fabs(p.v - v0) <= tol || std::fabs(p.v - v1) <= tol;
}

// Cyclic position along the cell boundary, counter-clockwise from (u0,v0).
double boundary_position(const UV &p, double u0, double u1, double v0, double v1,
                         double tol)
{
    double w = u1 - u0, h = v1 - v0;
    if (std::fabs(p.v - v0) <= tol) return p.u - u0;
    if (std::fabs(p.u - u1) <= tol) return w + (p.v - v0);
    if (std::fabs(p.v - v1) <= tol) return w + h + (u1 - p.u);
    return 2.0 * w + h + (v1 - p.v);
}

// Triangulate one simple polygon (CCW node list) with ear clipping.
bool ear_clip(const CellGraph &g, std::vector<int> poly,
              const std::function<void(int, int, int)> &emit)
{
    std::vector<int> p;
    for (std::size_t i = 0; i < poly.size(); ++i)
        if (p.empty() || p.back() != poly[i]) p.push_back(poly[i]);
    if (p.size() > 1 && p.front() == p.back()) p.pop_back();
    if (p.size() < 3) return true;

    std::size_t guard = 0;
    while (p.size() > 3)
    {
        bool clipped = false;
        std::size_t n = p.size();
        for (std::size_t i = 0; i < n; ++i)
        {
            int ia = p[(i + n - 1) % n], ib = p[i], ic = p[(i + 1) % n];
            const UV &a = g.uv[ia], &b = g.uv[ib], &c = g.uv[ic];
            if (!(cross2(a, b, c) > 1e-20)) continue;
            bool inside = false;
            for (std::size_t j = 0; j < n; ++j)
            {
                int iv = p[j];
                if (iv == ia || iv == ib || iv == ic) continue;
                const UV &q = g.uv[iv];
                if (q == a || q == b || q == c) continue;
                if (cross2(a, b, q) >= 0.0 && cross2(b, c, q) >= 0.0 &&
                    cross2(c, a, q) >= 0.0)
                {
                    inside = true;
                    break;
                }
            }
            if (inside) continue;
            emit(ia, ib, ic);
            p.erase(p.begin() + i);
            clipped = true;
            break;
        }
        if (!clipped) return false;
        if (++guard > 1000000) return false;
    }
    if (p.size() == 3)
    {
        const UV &a = g.uv[p[0]], &b = g.uv[p[1]], &c = g.uv[p[2]];
        if (cross2(a, b, c) > 1e-20) emit(p[0], p[1], p[2]);
        else if (cross2(a, c, b) > 1e-20) emit(p[0], p[2], p[1]);
    }
    return true;
}

// One chain inside a cell: an ordered node path.  chain_closed means the
// last node connects back to the first.
struct CellChain
{
    std::vector<UV> pts;
    bool chain_closed = false;
};

void emit_triangle(Sheet &sh, const CellGraph &g, int ia, int ib, int ic,
                   int tri_limit)
{
    if ((int)sh.res->triangles.size() >= tri_limit)
        throw std::runtime_error("imprint: triangle budget exhausted");

    ImprintTriangle t;
    t.sheet = sh.id;
    int idx[3] = {ia, ib, ic};
    double xyz[3][3];
    for (int k = 0; k < 3; ++k)
    {
        const UV &uv = g.uv[idx[k]];
        SurfPoint p = evaluate(*sh.surf, uv.u, uv.v);
        if (!p.ok)
            throw std::runtime_error("imprint: surface evaluation failed");
        xyz[k][0] = p.xyz[0];
        xyz[k][1] = p.xyz[1];
        xyz[k][2] = p.xyz[2];
        t.uv[k][0] = uv.u;
        t.uv[k][1] = uv.v;
        t.normal[k][0] = p.normal[0];
        t.normal[k][1] = p.normal[1];
        t.normal[k][2] = p.normal[2];
        t.index[k] = g.pos[idx[k]];
    }
    double ax = xyz[1][0] - xyz[0][0], ay = xyz[1][1] - xyz[0][1],
           az = xyz[1][2] - xyz[0][2];
    double bx = xyz[2][0] - xyz[0][0], by = xyz[2][1] - xyz[0][1],
           bz = xyz[2][2] - xyz[0][2];
    double cx = ay * bz - az * by, cy = az * bx - ax * bz,
           cz = ax * by - ay * bx;
    if (!(std::sqrt(cx * cx + cy * cy + cz * cz) > 1e-24))
        return;  // never emit a zero-area triangle
    sh.res->triangles.push_back(t);
}

void triangulate_cell(Sheet &sh, const std::vector<CellChain> &chains,
                      double u0, double u1, double v0, double v1,
                      int tri_limit)
{
    const double tol = 1e-9 * std::max(1.0, std::max(u1 - u0, v1 - v0));
    CellGraph g;

    // gather all points that lie on this cell boundary: corners, boundary
    // splits of the global grid lines and chain points on the boundary
    std::vector<UV> bpts;
    bpts.push_back(UV{u0, v0});
    bpts.push_back(UV{u1, v0});
    bpts.push_back(UV{u1, v1});
    bpts.push_back(UV{u0, v1});
    for (std::size_t k = 0; k < chains.size(); ++k)
        for (std::size_t i = 0; i < chains[k].pts.size(); ++i)
            if (on_boundary(chains[k].pts[i], u0, u1, v0, v1, tol))
                bpts.push_back(chains[k].pts[i]);
    // every station on this cell's four grid lines must become a boundary
    // vertex as well, so the neighbouring cell shares it (no T-junction)
    {
        std::vector<UV> extra;
        for (std::map<UV, int>::const_iterator it = sh.pos_of_uv.begin();
             it != sh.pos_of_uv.end(); ++it)
            if (on_boundary(it->first, u0, u1, v0, v1, tol))
                extra.push_back(it->first);
        bpts.insert(bpts.end(), extra.begin(), extra.end());
    }

    std::vector<std::pair<double, UV>> ordered;
    for (std::size_t i = 0; i < bpts.size(); ++i)
    {
        bool dup = false;
        for (std::size_t j = 0; j < ordered.size(); ++j)
            if (ordered[j].second == bpts[i]) { dup = true; break; }
        if (dup) continue;
        ordered.push_back(std::make_pair(
            boundary_position(bpts[i], u0, u1, v0, v1, tol), bpts[i]));
    }
    std::sort(ordered.begin(), ordered.end(),
              [](const std::pair<double, UV> &a,
                 const std::pair<double, UV> &b) {
                  return a.first < b.first;
              });
    std::vector<int> boundary;
    for (std::size_t i = 0; i < ordered.size(); ++i)
        boundary.push_back(g.node(sh, ordered[i].second));
    for (std::size_t i = 0; i < boundary.size(); ++i)
        g.edge(boundary[i], boundary[(i + 1) % boundary.size()]);

    // insert the interior constraint chains
    std::vector<std::vector<int>> loops;
    for (std::size_t k = 0; k < chains.size(); ++k)
    {
        const CellChain &ch = chains[k];
        std::vector<int> nodes;
        for (std::size_t i = 0; i < ch.pts.size(); ++i)
            nodes.push_back(g.node(sh, ch.pts[i]));
        for (std::size_t i = 0; i + 1 < nodes.size(); ++i)
            g.edge(nodes[i], nodes[i + 1]);
        if (ch.chain_closed)
        {
            if (nodes.size() > 1) g.edge(nodes.back(), nodes.front());
            loops.push_back(nodes);
        }
        else
        {
            if (!on_boundary(ch.pts.front(), u0, u1, v0, v1, tol) ||
                !on_boundary(ch.pts.back(), u0, u1, v0, v1, tol))
                throw std::runtime_error(
                    "imprint: open intersection chain does not end on a cell "
                    "boundary");
        }
    }

    // bridge closed loops that are fully interior to a boundary vertex; the
    // bridge edge is traversed once by each side, so the region inside and
    // outside the loop is each covered exactly once and no boundary point is
    // added (no T-junction in the neighbouring cell)
    for (std::size_t l = 0; l < loops.size(); ++l)
    {
        std::vector<int> &lp = loops[l];
        bool touches = false;
        for (std::size_t i = 0; i < lp.size(); ++i)
            if (on_boundary(g.uv[lp[i]], u0, u1, v0, v1, tol)) { touches = true; break; }
        if (touches) continue;

        int best_node = -1, best_boundary = -1;
        double best_d = 0.0;
        for (std::size_t bi = 0; bi < boundary.size(); ++bi)
        {
            const UV &bp = g.uv[boundary[bi]];
            for (std::size_t li = 0; li < lp.size(); ++li)
            {
                const UV &lv = g.uv[lp[li]];
                bool clear = true;
                for (std::size_t e = 0; e < lp.size() && clear; ++e)
                {
                    int i0 = lp[e], i1 = lp[(e + 1) % lp.size()];
                    if (i0 == lp[li] || i1 == lp[li]) continue;
                    const UV &p = g.uv[i0], &q = g.uv[i1];
                    double d1 = cross2(bp, lv, p);
                    double d2 = cross2(bp, lv, q);
                    double d3 = cross2(p, q, bp);
                    double d4 = cross2(p, q, lv);
                    if (((d1 > 0 && d2 < 0) || (d1 < 0 && d2 > 0)) &&
                        ((d3 > 0 && d4 < 0) || (d3 < 0 && d4 > 0)))
                        clear = false;
                }
                if (!clear) continue;
                double d = (bp.u - lv.u) * (bp.u - lv.u) +
                           (bp.v - lv.v) * (bp.v - lv.v);
                if (best_node < 0 || d < best_d)
                {
                    best_node = lp[li];
                    best_boundary = boundary[bi];
                    best_d = d;
                }
            }
        }
        if (best_node < 0)
            throw std::runtime_error(
                "imprint: cannot bridge an interior intersection loop");
        g.edge(best_node, best_boundary);
    }

    // planar face walk: for a half-edge (u,v), the face lies to its left and
    // the next half-edge at v is the neighbour just clockwise from u
    std::vector<std::vector<int>> nb = g.adj;
    std::map<std::pair<int, int>, int> angle_pos;
    for (std::size_t i = 0; i < nb.size(); ++i)
    {
        std::vector<int> list = nb[i];
        const UV &c = g.uv[i];
        std::sort(list.begin(), list.end(), [&](int a, int b) {
            double aa = std::atan2(g.uv[a].v - c.v, g.uv[a].u - c.u);
            double ab = std::atan2(g.uv[b].v - c.v, g.uv[b].u - c.u);
            return aa < ab;
        });
        for (std::size_t j = 0; j < list.size(); ++j)
            angle_pos[std::make_pair((int)i, list[j])] = (int)j;
        nb[i] = list;
    }
    std::map<std::pair<int, int>, bool> visited;
    std::vector<std::vector<int>> faces;
    for (std::size_t u = 0; u < nb.size(); ++u)
    {
        for (std::size_t j = 0; j < nb[u].size(); ++j)
        {
            int v = nb[u][j];
            if (visited[std::make_pair((int)u, v)]) continue;
            std::vector<int> cyc;
            int cu = (int)u, cv = v;
            std::size_t guard = 0;
            while (!visited[std::make_pair(cu, cv)])
            {
                visited[std::make_pair(cu, cv)] = true;
                cyc.push_back(cu);
                const std::vector<int> &ring = nb[cv];
                int pos = angle_pos[std::make_pair(cv, cu)];
                int n = (int)ring.size();
                int nw = ring[(pos + n - 1) % n];
                cu = cv;
                cv = nw;
                if (++guard > 1000000)
                    throw std::runtime_error("imprint: face walk did not close");
            }
            if (cyc.size() >= 3) faces.push_back(cyc);
        }
    }

    double cell_area = (u1 - u0) * (v1 - v0);
    double area_tol = 1e-14 * std::max(1.0, cell_area);
    long emitted = 0;
    for (std::size_t f = 0; f < faces.size(); ++f)
    {
        std::vector<int> &cyc = faces[f];
        double area = 0.0;
        bool all_boundary = true;
        for (std::size_t i = 0; i < cyc.size(); ++i)
        {
            const UV &a = g.uv[cyc[i]];
            const UV &b = g.uv[cyc[(i + 1) % cyc.size()]];
            area += a.u * b.v - b.u * a.v;
            if (!on_boundary(a, u0, u1, v0, v1, tol)) all_boundary = false;
        }
        area *= 0.5;
        if (area < 0.0 && all_boundary) continue;  // the outer face
        if (std::fabs(area) <= area_tol) continue;

        std::vector<int> poly = cyc;
        if (area < 0.0) std::reverse(poly.begin(), poly.end());
        bool ok = ear_clip(g, poly, [&](int ia, int ib, int ic) {
            emit_triangle(sh, g, ia, ib, ic, tri_limit);
            ++emitted;
        });
        if (!ok)
            throw std::runtime_error(
                "imprint: constrained cell triangulation failed");
    }
    if (emitted == 0)
        throw std::runtime_error("imprint: cell produced no triangles");
}

// ---------------------------------------------------------------------
// branch assembly
// ---------------------------------------------------------------------

int cell_index(const std::vector<double> &grid, double x)
{
    if (grid.size() < 2) return -1;
    if (x < grid.front() - 1e-9 || x > grid.back() + 1e-9) return -1;
    std::vector<double>::const_iterator it =
        std::upper_bound(grid.begin(), grid.end(), x);
    int idx = (int)(it - grid.begin()) - 1;
    if (idx < 0) idx = 0;
    if (idx > (int)grid.size() - 2) idx = (int)grid.size() - 2;
    return idx;
}

struct Branch
{
    std::vector<UV> uv[2];
    std::vector<int> pos;
    bool closed = false;
};

// Split every branch into per-cell chains for one sheet.  A segment between
// consecutive stations never crosses a grid line of either surface (the
// native tracer inserted an event for every crossing), so every segment lies
// in exactly one cell.  Consecutive segments of the same cell are joined into
// one chain; a closed branch whose first and last cell coincide is joined
// across the wrap as well.
std::map<std::pair<int, int>, std::vector<CellChain>>
build_cell_chains(const Sheet &sh, const std::vector<Branch> &branches,
                  int sheet_index)
{
    std::map<std::pair<int, int>, std::vector<CellChain>> cells;
    for (std::size_t b = 0; b < branches.size(); ++b)
    {
        const Branch &br = branches[b];
        int n = (int)br.uv[sheet_index].size();
        if (n < 2) continue;
        int segs = br.closed ? n : n - 1;

        int rotate = 0;
        if (br.closed && segs > 1)
        {
            // start the ring at a cell transition, so that every emitted open
            // chain ends on a cell boundary
            for (int s = 0; s < segs; ++s)
            {
                const UV &p0 = br.uv[sheet_index][(s + segs - 1) % n];
                const UV &p1 = br.uv[sheet_index][s];
                const UV &p2 = br.uv[sheet_index][(s + 1) % n];
                UV m0{(p0.u + p1.u) * 0.5, (p0.v + p1.v) * 0.5};
                UV m1{(p1.u + p2.u) * 0.5, (p1.v + p2.v) * 0.5};
                bool t0 = cell_index(sh.gu, m0.u) != cell_index(sh.gu, m1.u) ||
                          cell_index(sh.gv, m0.v) != cell_index(sh.gv, m1.v);
                if (t0) { rotate = s; break; }
            }
        }

        std::vector<CellChain> run;
        std::vector<std::pair<int, int>> run_cell;

        for (int s = 0; s < segs; ++s)
        {
            int idx0 = (rotate + s) % n;
            int idx1 = (rotate + s + 1) % n;
            const UV &a = br.uv[sheet_index][idx0];
            const UV &bb = br.uv[sheet_index][idx1];
            UV mid{(a.u + bb.u) * 0.5, (a.v + bb.v) * 0.5};

            int ci = cell_index(sh.gu, mid.u);
            int cj = cell_index(sh.gv, mid.v);
            if (ci < 0 || cj < 0)
                throw std::runtime_error(
                    "imprint: intersection station outside the parameter domain");

            if (run.empty() || ci != run_cell.back().first ||
                cj != run_cell.back().second)
            {
                CellChain ch;
                ch.pts.push_back(a);
                ch.pts.push_back(bb);
                run.push_back(ch);
                run_cell.push_back(std::make_pair(ci, cj));
            }
            else
            {
                CellChain &ch = run.back();
                if (!(ch.pts.back() == bb)) ch.pts.push_back(bb);
            }
        }
        if (run.empty()) continue;

        if (br.closed && run.size() == 1)
        {
            CellChain &ch = run[0];
            ch.chain_closed = true;
            if (ch.pts.size() > 1 && ch.pts.back() == ch.pts.front())
                ch.pts.pop_back();
        }

        for (std::size_t k = 0; k < run.size(); ++k)
            cells[run_cell[k]].push_back(run[k]);
    }
    return cells;
}

} // anonymous namespace

// ---------------------------------------------------------------------
// public entry point
// ---------------------------------------------------------------------

void imprint_surfaces(const SISLSurf &surf1,
                      const std::vector<double> &grid1_u,
                      const std::vector<double> &grid1_v,
                      const SISLSurf &surf2,
                      const std::vector<double> &grid2_u,
                      const std::vector<double> &grid2_v,
                      const ImprintOptions &options,
                      ImprintResult &out)
{
    validate_options(options);
    validate_surface(surf1, "imprint surface 1");
    validate_surface(surf2, "imprint surface 2");
    validate_grid(surf1, 0, grid1_u, "imprint grid 1u");
    validate_grid(surf1, 1, grid1_v, "imprint grid 1v");
    validate_grid(surf2, 0, grid2_u, "imprint grid 2u");
    validate_grid(surf2, 1, grid2_v, "imprint grid 2v");

    // 1. native tracing (s1859 discovery + real s1310 march)
    SislTraceOptions topts;
    topts.geometric_tolerance = options.geometricTolerance;
    topts.max_stations = options.maxStations;
    SislTraceResult *trace = nullptr;
    int trc = sisl_trace_grid_events(&surf1,
                                     grid1_u.data(), (int)grid1_u.size(),
                                     grid1_v.data(), (int)grid1_v.size(),
                                     &surf2,
                                     grid2_u.data(), (int)grid2_u.size(),
                                     grid2_v.data(), (int)grid2_v.size(),
                                     &topts, &trace);
    if (trc == SISL_TRACE_INVALID_ARGUMENT)
        throw std::invalid_argument("imprint: the native tracer rejected the "
                                    "surfaces, grids or options");
    if (trc != SISL_TRACE_OK || !trace)
        throw std::runtime_error("imprint: intersection tracing failed "
                                 "(numeric failure or budget exhausted)");

    // 2. working result, committed on success only
    ImprintResult work;
    std::vector<Branch> branches(trace->branch_count);

    auto shared_position = [&](const double *xyz) {
        int idx = (int)work.positions.size();
        work.positions.push_back({xyz[0], xyz[1], xyz[2]});
        return idx;
    };

    // 3. per-sheet state
    Sheet sheets[2];
    sheets[0].surf = &surf1;
    sheets[0].id = 0;
    sheets[0].gu = grid1_u;
    sheets[0].gv = grid1_v;
    sheets[0].res = &work;
    sheets[1].surf = &surf2;
    sheets[1].id = 1;
    sheets[1].gu = grid2_u;
    sheets[1].gv = grid2_v;
    sheets[1].res = &work;

    double snap = 1e-8 * std::max(1.0, std::max(
        std::max(std::fabs(grid1_u.front()), std::fabs(grid1_u.back())),
        std::max(std::fabs(grid1_v.front()), std::fabs(grid1_v.back()))));

    for (std::size_t b = 0; b < trace->branch_count; ++b)
    {
        const SislTraceBranch &tb = trace->branches[b];
        Branch &br = branches[b];
        br.closed = tb.closed != 0;
        if (tb.count == 0) continue;
        for (std::size_t i = 0; i < tb.count; ++i)
        {
            const SislTraceStation &st = tb.stations[i];
            int pos = shared_position(st.xyz);
            br.pos.push_back(pos);
            UV s0 = snap_to_grid(sheets[0], st.uv1[0], st.uv1[1], snap);
            UV s1 = snap_to_grid(sheets[1], st.uv2[0], st.uv2[1], snap);
            br.uv[0].push_back(s0);
            br.uv[1].push_back(s1);
            // the same common station is one position in both sheets
            sheets[0].pos_of_uv[s0] = pos;
            sheets[1].pos_of_uv[s1] = pos;
        }
    }
    sisl_trace_result_free(trace);
    trace = nullptr;

    // 4. polylines: one ordered position chain per branch
    for (std::size_t b = 0; b < branches.size(); ++b)
    {
        if (branches[b].pos.size() < 2) continue;
        ImprintPolyline pl;
        pl.indices = branches[b].pos;
        pl.closed = branches[b].closed;
        if (pl.closed && pl.indices.size() > 1 &&
            pl.indices.front() == pl.indices.back())
            pl.indices.pop_back();
        work.polylines.push_back(pl);
    }

    // 5. triangulate every cell of both sheets
    for (int sheet = 0; sheet < 2; ++sheet)
    {
        Sheet &sh = sheets[sheet];
        std::map<std::pair<int, int>, std::vector<CellChain>> cells =
            build_cell_chains(sh, branches, sheet);

        for (std::size_t i = 0; i + 1 < sh.gu.size(); ++i)
        {
            for (std::size_t j = 0; j + 1 < sh.gv.size(); ++j)
            {
                std::pair<int, int> key((int)i, (int)j);
                std::vector<CellChain> chains;
                std::map<std::pair<int, int>, std::vector<CellChain>>::iterator
                    it = cells.find(key);
                if (it != cells.end()) chains = it->second;
                triangulate_cell(sh, chains, sh.gu[i], sh.gu[i + 1],
                                 sh.gv[j], sh.gv[j + 1], options.maxTriangles);
            }
        }
    }

    if (work.triangles.empty())
        throw std::runtime_error("imprint: no triangles produced");

    // 6. commit
    out = std::move(work);
}

} // namespace sislmesh
