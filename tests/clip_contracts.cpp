//
// Contract tests for ClipRegularSurfaceMesh.
//
// Registers the three required groups (bounds, topology, attributes) with
// task_support/check.h and provides task_cases/task_case_count.  Does not
// define main(): task_support/driver.c supplies it.
//

#include "task_support/check.h"
#include "task_support/grid.h"

#include <opensubdiv/bfr/clipSurface.h>
#include <opensubdiv/bfr/refinerSurfaceFactory.h>
#include <opensubdiv/bfr/surface.h>
#include <opensubdiv/far/topologyDescriptor.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

using namespace OpenSubdiv;

namespace {

using V2 = std::array<double, 2>;
using V3 = std::array<double, 3>;

V3 sub(V3 const & a, V3 const & b) {
    return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
}
double dot(V3 const & a, V3 const & b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
V3 cross(V3 const & a, V3 const & b) {
    return {a[1] * b[2] - a[2] * b[1],
            a[2] * b[0] - a[0] * b[2],
            a[0] * b[1] - a[1] * b[0]};
}
double norm(V3 const & a) { return std::sqrt(dot(a, a)); }

// ---------------------------------------------------------------------------
// Configurable quad grid cage
// ---------------------------------------------------------------------------

struct Cage {
    int nx = 6, ny = 6; // vertices
    std::vector<double> pos, uv;
    std::vector<int> sizes, fv, uvi;
    std::unique_ptr<Far::TopologyRefiner> topo;

    double (*height)(double, double, double) = nullptr;
    double hk = 0.0;
    bool perCornerUV = true;

    void build() {
        pos.clear();
        for (int y = 0; y < ny; ++y)
            for (int x = 0; x < nx; ++x) {
                double X = x - (nx - 1) * 0.5;
                double Y = y - (ny - 1) * 0.5;
                double z = height ? height(X, Y, hk) : 0.0;
                pos.insert(pos.end(), {X, Y, z});
            }
        sizes.clear(); fv.clear();
        for (int y = 0; y < ny - 1; ++y)
            for (int x = 0; x < nx - 1; ++x) {
                sizes.push_back(4);
                fv.insert(fv.end(),
                          {y * nx + x, y * nx + x + 1,
                           (y + 1) * nx + x + 1, (y + 1) * nx + x});
            }
        uv.clear(); uvi.clear();
        int nf = (nx - 1) * (ny - 1);
        static const double U[4] = {0, 1, 1, 0};
        static const double VV[4] = {0, 0, 1, 1};
        for (int f = 0; f < nf; ++f) {
            int gx = f % (nx - 1), gy = f / (nx - 1);
            for (int k = 0; k < 4; ++k) {
                int valueIndex;
                if (perCornerUV) {
                    valueIndex = (int) uv.size() / 2;
                    uv.push_back(gx * 10.0 + U[k] + 0.37 * k);
                    uv.push_back(gy * 10.0 + VV[k] + 0.91 * k);
                } else {
                    int vx = gx + (k == 1 || k == 2);
                    int vy = gy + (k >= 2);
                    valueIndex = vy * nx + vx;
                    if ((int) uv.size() / 2 < nx * ny) {
                        // filled below
                    }
                }
                uvi.push_back(valueIndex);
                if (perCornerUV == false) {
                    // ensure shared uv array sized nx*ny
                }
            }
        }
        if (!perCornerUV) {
            uv.assign(nx * ny * 2, 0.0);
            for (int y = 0; y < ny; ++y)
                for (int x = 0; x < nx; ++x) {
                    uv[2 * (y * nx + x) + 0] = x * 3.0;
                    uv[2 * (y * nx + x) + 1] = y * 5.0;
                }
        }

        Far::TopologyDescriptor d;
        d.numVertices = nx * ny;
        d.numFaces = nf;
        d.numVertsPerFace = sizes.data();
        d.vertIndicesPerFace = fv.data();
        Far::TopologyDescriptor::FVarChannel ch;
        ch.numValues = (int) uv.size() / 2;
        ch.valueIndices = uvi.data();
        d.numFVarChannels = 1;
        d.fvarChannels = &ch;
        Far::TopologyRefinerFactory<Far::TopologyDescriptor>::Options
            o(Sdc::SCHEME_CATMARK);
        o.schemeOptions.SetVtxBoundaryInterpolation(
            Sdc::Options::VTX_BOUNDARY_EDGE_ONLY);
        topo.reset(Far::TopologyRefinerFactory<
                   Far::TopologyDescriptor>::Create(d, o));
    }

    std::vector<int> block(int x0, int y0, int x1, int y1) const {
        std::vector<int> r;
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x)
                r.push_back(y * (nx - 1) + x);
        return r;
    }
};

double hBowl(double X, double Y, double k) { return k * (X * X + Y * Y); }
double hSaddle(double X, double Y, double k) { return k * 0.5 * (X * X - Y * Y); }
double hWave(double X, double Y, double k) {
    return k * std::sin(0.9 * X) * std::cos(0.8 * Y) + 0.15 * k * X;
}

// ---------------------------------------------------------------------------
// Independent Bfr oracle for per-face limit evaluation
// ---------------------------------------------------------------------------

struct Oracle {
    struct F {
        Bfr::Surface<double> s;
        Bfr::Surface<double> uv;
        std::vector<double> pp, upp;
        bool uvOK = false;
    };
    std::unique_ptr<Bfr::RefinerSurfaceFactory<>> fac;
    std::vector<F> all;
    Far::TopologyRefiner const * topo = nullptr;

    void init(Far::TopologyRefiner const & t,
              std::vector<double> const & pos,
              std::vector<double> const & tex) {
        topo = &t;
        fac.reset(new Bfr::RefinerSurfaceFactory<>(t));
        int nf = t.GetLevel(0).GetNumFaces();
        all.resize(nf);
        for (int f = 0; f < nf; ++f) {
            if (fac->InitVertexSurface(f, &all[f].s) && all[f].s.IsValid()) {
                int n = all[f].s.GetNumPatchPoints();
                all[f].pp.assign(n * 3, 0.0);
                all[f].s.PreparePatchPoints(pos.data(), 3,
                                            all[f].pp.data(), 3);
            }
            if (fac->InitFaceVaryingSurface(f, &all[f].uv, 0) &&
                all[f].uv.IsValid()) {
                int n = all[f].uv.GetNumPatchPoints();
                all[f].upp.assign(n * 2, 0.0);
                all[f].uv.PreparePatchPoints(tex.data(), 2,
                                             all[f].upp.data(), 2);
                all[f].uvOK = true;
            }
        }
    }
    V3 eval(int f, V2 q, V3 * du = nullptr, V3 * dv = nullptr) {
        V3 P{0, 0, 0};
        double uv[2] = {q[0], q[1]};
        if (du && dv)
            all[f].s.Evaluate(uv, all[f].pp.data(), 3, P.data(),
                              du->data(), dv->data());
        else
            all[f].s.Evaluate(uv, all[f].pp.data(), 3, P.data());
        return P;
    }
    V2 evalUV(int f, V2 q) {
        double uv[2] = {q[0], q[1]}, tex[2] = {0, 0};
        if (all[f].uvOK)
            all[f].uv.Evaluate(uv, all[f].upp.data(), 2, tex);
        return {tex[0], tex[1]};
    }
};

// ---------------------------------------------------------------------------
// Output validation helpers
// ---------------------------------------------------------------------------

bool sizesOK(Bfr::ClippedMesh const & m) {
    if (m.corners.size() != 3 * m.triangles.size()) return false;
    if (m.sourceFaces.size() != m.triangles.size()) return false;
    for (auto const & t : m.triangles)
        for (unsigned i : t)
            if (i >= m.positions.size()) return false;
    return true;
}

// Maximum limit-surface vs linear-triangle error, densely sampled.
double maxGeometricError(Bfr::ClippedMesh const & m, Oracle & o) {
    double worst = 0.0;
    int const K = 7;
    for (size_t t = 0; t < m.triangles.size(); ++t) {
        int f = m.sourceFaces[t];
        V2 q[3] = {m.corners[3 * t + 0].parameter,
                   m.corners[3 * t + 1].parameter,
                   m.corners[3 * t + 2].parameter};
        V3 p[3];
        for (int k = 0; k < 3; ++k)
            p[k] = m.positions[m.triangles[t][k]];
        for (int i = 0; i <= K; ++i)
            for (int j = 0; j + i <= K; ++j) {
                double a = (double) i / K;
                double b = (double) j / K;
                double c = 1.0 - a - b;
                V2 uvw = {c * q[0][0] + a * q[1][0] + b * q[2][0],
                          c * q[0][1] + a * q[1][1] + b * q[2][1]};
                V3 lin = {c * p[0][0] + a * p[1][0] + b * p[2][0],
                          c * p[0][1] + a * p[1][1] + b * p[2][1],
                          c * p[0][2] + a * p[1][2] + b * p[2][2]};
                V3 ref = o.eval(f, uvw);
                worst = std::max(worst, norm(sub(ref, lin)));
            }
    }
    return worst;
}

bool pointInTri(V2 p, V2 a, V2 b, V2 c) {
    double d1 = (p[0] - b[0]) * (a[1] - b[1]) - (a[0] - b[0]) * (p[1] - b[1]);
    double d2 = (p[0] - c[0]) * (b[1] - c[1]) - (b[0] - c[0]) * (p[1] - c[1]);
    double d3 = (p[0] - a[0]) * (c[1] - a[1]) - (c[0] - a[0]) * (p[1] - a[1]);
    double eps = 1e-9;
    bool neg = (d1 < -eps) || (d2 < -eps) || (d3 < -eps);
    bool pos = (d1 > eps) || (d2 > eps) || (d3 > eps);
    return !(neg && pos);
}

// Dense coverage: positive reference samples must be covered and negative
// samples uncovered, except within the allowed plane band.
int coverageViolations(Bfr::ClippedMesh const & m, Oracle & o,
                       std::vector<int> const & faces,
                       std::array<double, 3> n, double d,
                       double band, int ND = 40) {
    int bad = 0;
    for (int f : faces) {
        std::vector<size_t> tris;
        for (size_t t = 0; t < m.triangles.size(); ++t)
            if (m.sourceFaces[t] == f) tris.push_back(t);
        for (int ix = 1; ix < ND; ++ix)
            for (int iy = 1; iy < ND; ++iy) {
                V2 q = {(double) ix / ND, (double) iy / ND};
                V3 P = o.eval(f, q);
                double h = dot(n, P) + d;
                bool covered = false;
                for (size_t t : tris) {
                    V2 a = m.corners[3 * t + 0].parameter;
                    V2 b = m.corners[3 * t + 1].parameter;
                    V2 c = m.corners[3 * t + 2].parameter;
                    if (pointInTri(q, a, b, c)) { covered = true; break; }
                }
                if (h > band && !covered) {
                    ++bad;
                    if (bad <= 5)
                        std::printf("    uncovered f=%d q=(%.3f,%.3f) h=%.4f\n",
                                    f, q[0], q[1], h);
                }
                if (h < -band && covered) {
                    ++bad;
                    if (bad <= 5)
                        std::printf("    extra-covered f=%d q=(%.3f,%.3f) h=%.4f\n",
                                    f, q[0], q[1], h);
                }
            }
    }
    return bad;
}

bool sameMesh(Bfr::ClippedMesh const & a, Bfr::ClippedMesh const & b) {
    if (a.positions != b.positions || a.triangles != b.triangles ||
        a.sourceFaces != b.sourceFaces)
        return false;
    if (a.corners.size() != b.corners.size()) return false;
    for (size_t i = 0; i < a.corners.size(); ++i) {
        if (a.corners[i].parameter != b.corners[i].parameter ||
            a.corners[i].uv != b.corners[i].uv ||
            a.corners[i].normal != b.corners[i].normal)
            return false;
    }
    return true;
}

// Maximum plane distance over vertices of boundary edges that are not lying
// on a coarse-face side (i.e. true plane-intersection edges).
double maxCutVertexDistance(Bfr::ClippedMesh const & m,
                            std::array<double, 3> n, double d) {
    std::map<std::pair<unsigned, unsigned>, int> edgeCount;
    for (auto const & t : m.triangles)
        for (int k = 0; k < 3; ++k) {
            unsigned a = t[k], b = t[(k + 1) % 3];
            if (a > b) std::swap(a, b);
            ++edgeCount[{a, b}];
        }
    double worst = 0.0;
    for (size_t t = 0; t < m.triangles.size(); ++t) {
        for (int k = 0; k < 3; ++k) {
            unsigned pa = m.triangles[t][k];
            unsigned pb = m.triangles[t][(k + 1) % 3];
            unsigned lo = std::min(pa, pb), hi = std::max(pa, pb);
            if (edgeCount[{lo, hi}] != 1) continue;
            V2 a = m.corners[3 * t + k].parameter;
            V2 b = m.corners[3 * t + ((k + 1) % 3)].parameter;
            auto side = [](V2 p, int s) {
                const double e = 1e-6;
                if (s == 0) return std::fabs(p[1]) < e;
                if (s == 1) return std::fabs(p[0] - 1) < e;
                if (s == 2) return std::fabs(p[1] - 1) < e;
                return std::fabs(p[0]) < e;
            };
            bool coarseSide = false;
            for (int s = 0; s < 4; ++s)
                if (side(a, s) && side(b, s)) coarseSide = true;
            if (coarseSide) continue;
            for (unsigned pid : {pa, pb}) {
                V3 P = m.positions[pid];
                worst = std::max(worst, std::fabs(dot(n, P) + d));
            }
        }
    }
    return worst;
}

Bfr::ClipOptions stdOpt(double gt = 1e-3, int depth = 18) {
    return {gt, 1e-8, depth};
}

} // namespace

// ===========================================================================
// bounds group
// ===========================================================================

static int case_flat_full_area() {
    Cage g; g.nx = 7; g.ny = 7; g.build();
    Bfr::ClippedMesh m; std::string err;
    Bfr::ClipPlane pl{{0, 0, 1}, 100.0};
    bool ok = Bfr::ClipRegularSurfaceMesh(*g.topo, g.pos, g.uv,
                                          g.block(1, 1, 4, 4), pl, stdOpt(),
                                          m, err);
    if (!ok) std::printf("  flat_full err: %s\n", err.c_str());
    TASK_CHECK(ok);
    TASK_CHECK(err.empty());
    TASK_CHECK(sizesOK(m));
    TASK_CHECK(!m.triangles.empty());
    double area = 0;
    for (auto const & t : m.triangles) {
        auto a = m.positions[t[0]], b = m.positions[t[1]], c = m.positions[t[2]];
        area += 0.5 * ((b[0] - a[0]) * (c[1] - a[1]) -
                       (b[1] - a[1]) * (c[0] - a[0]));
    }
    // block x,y in 1..4 -> coarse region [ -1.5, 2.5 ]^2 area 16
    TASK_CHECK(std::fabs(area - 16.0) < 1e-7);
    return 0;
}

static int case_flat_oblique_bounds() {
    Cage g; g.nx = 8; g.ny = 8; g.build();
    auto sel = g.block(1, 1, 5, 5);
    Bfr::ClipPlane pl{{0.31, 0.47, 1.0}, -0.233};
    double nl = std::sqrt(0.31 * 0.31 + 0.47 * 0.47 + 1.0);
    std::array<double, 3> nn{0.31 / nl, 0.47 / nl, 1.0 / nl};
    double dd = -0.233 / nl;
    Bfr::ClippedMesh m; std::string err;
    bool ok = Bfr::ClipRegularSurfaceMesh(*g.topo, g.pos, g.uv, sel, pl,
                                          stdOpt(), m, err);
    if (!ok) std::printf("  oblique err: %s\n", err.c_str());
    TASK_CHECK(ok);
    TASK_CHECK(sizesOK(m));
    Oracle o; o.init(*g.topo, g.pos, g.uv);
    double werr = maxGeometricError(m, o);
    TASK_CHECK(werr <= 1e-3 + 1e-7);
    {
        std::map<int,int> perFace;
        for (int f : sel) perFace[f] = 0;
        for (int f : m.sourceFaces) perFace[f]++;
        for (auto const & kv : perFace)
            if (kv.second < 2) std::printf("    face %d tris=%d\n", kv.first, kv.second);
        for (size_t t = 0; t < m.triangles.size(); ++t)
            if (m.sourceFaces[t] == 25) {
                std::printf("    f25 tri %zu: (%.3f,%.3f)(%.3f,%.3f)(%.3f,%.3f)\n", t,
                    m.corners[3*t+0].parameter[0],m.corners[3*t+0].parameter[1],
                    m.corners[3*t+1].parameter[0],m.corners[3*t+1].parameter[1],
                    m.corners[3*t+2].parameter[0],m.corners[3*t+2].parameter[1]);
            }
    }
    int bad = coverageViolations(m, o, sel, nn, dd, 1e-3 + 1e-8 + 1e-6);
    std::printf("  [oblique coverage bad=%d maxerr=%.2e tris=%zu]\n",
                bad, werr, m.triangles.size());
    TASK_CHECK(bad == 0);
    return 0;
}

static int case_curved_geom_error() {
    Cage g; g.nx = 8; g.ny = 8; g.height = hSaddle; g.hk = 0.25; g.build();
    auto sel = g.block(2, 2, 4, 4);
    Bfr::ClipPlane pl{{0.137, -0.219, 1.0}, 0.0831};
    Bfr::ClippedMesh m; std::string err;
    bool ok = Bfr::ClipRegularSurfaceMesh(*g.topo, g.pos, g.uv, sel, pl,
                                          {1e-3, 1e-8, 20}, m, err);
    if (!ok) std::printf("  curved err: %s\n", err.c_str());
    TASK_CHECK(ok);
    TASK_CHECK(sizesOK(m));
    Oracle o; o.init(*g.topo, g.pos, g.uv);
    double werr = maxGeometricError(m, o);
    std::printf("  [curved geom worst=%.3e]\n", werr);
    TASK_CHECK(werr <= 1e-3 + 1e-7);
    return 0;
}

static int case_closed_loop_same_corners() {
    Cage g; g.nx = 10; g.ny = 10; g.height = hBowl; g.hk = 0.15; g.build();
    // Symmetric 3x3 block around the origin.  The regular limit of the
    // quadratic bowl is 0.15*r^2 + 0.10; the central face corners sit at
    // z=0.175 while its centre is z=0.10.  Plane z=0.14 therefore leaves all
    // four coarse corners on the retained side while cutting a closed loop
    // entirely inside the central face.
    auto sel = g.block(3, 3, 5, 5);
    Bfr::ClipPlane pl{{0, 0, 1}, -0.14};
    std::array<double, 3> nn{0, 0, 1};
    Bfr::ClippedMesh m; std::string err;
    bool ok = Bfr::ClipRegularSurfaceMesh(*g.topo, g.pos, g.uv, sel, pl,
                                          {1e-3, 1e-8, 20}, m, err);
    if (!ok) std::printf("  loop err: %s\n", err.c_str());
    TASK_CHECK(ok);
    TASK_CHECK(sizesOK(m));
    TASK_CHECK(m.triangles.size() > 20);
    Oracle o; o.init(*g.topo, g.pos, g.uv);
    double werr = maxGeometricError(m, o);
    TASK_CHECK(werr <= 1e-3 + 1e-7);
    int bad = coverageViolations(m, o, sel, nn, -0.14,
                                 1e-3 + 1e-8 + 2e-6, 48);
    std::printf("  [loop coverage bad=%d tris=%zu]\n", bad, m.triangles.size());
    TASK_CHECK(bad == 0);

    // The removed loop must leave plane-intersection vertices within tol.
    double worstCut = maxCutVertexDistance(m, nn, -0.14);
    std::printf("  [loop worst cut dist=%.2e]\n", worstCut);
    TASK_CHECK(worstCut <= 1e-8 + 1e-9);
    return 0;
}

static int case_depth_exhaustion_fails() {
    Cage g; g.nx = 8; g.ny = 8; g.height = hWave; g.hk = 3.0; g.build();
    auto sel = g.block(2, 2, 4, 4);
    Bfr::ClippedMesh m; std::string err;
    Bfr::ClipPlane pl{{0.2, 0.1, 1}, 0.0};
    Bfr::ClipOptions opt{1e-12, 1e-10, 2};
    bool ok = Bfr::ClipRegularSurfaceMesh(*g.topo, g.pos, g.uv, sel, pl,
                                          opt, m, err);
    TASK_CHECK(!ok);
    TASK_CHECK(!err.empty());
    TASK_CHECK(m.positions.empty());
    TASK_CHECK(m.triangles.empty());
    return 0;
}

static int case_tangent_rejected() {
    Cage g; g.nx = 8; g.ny = 8; g.height = hBowl; g.hk = 0.15; g.build();
    auto sel = g.block(3, 3, 4, 4);
    // Place the plane exactly at the central face limit minimum, where the
    // analytic derivatives vanish: a strict tangency.
    Oracle o; o.init(*g.topo, g.pos, g.uv);
    double zMin = o.eval(sel[0], {0.5, 0.5})[2];
    Bfr::ClippedMesh m; std::string err;
    bool ok = Bfr::ClipRegularSurfaceMesh(
        *g.topo, g.pos, g.uv, sel, {{0, 0, 1}, -zMin},
        {1e-3, 1e-8, 11}, m, err);
    TASK_CHECK(!ok);
    TASK_CHECK(!err.empty());
    return 0;
}

static int case_coplanar_rejected() {
    Cage g; g.nx = 6; g.ny = 6; g.build(); // flat z=0
    auto sel = g.block(1, 1, 3, 3);
    Bfr::ClippedMesh m; std::string err;
    bool ok = Bfr::ClipRegularSurfaceMesh(
        *g.topo, g.pos, g.uv, sel, {{0, 0, 1}, 0.0}, stdOpt(), m, err);
    TASK_CHECK(!ok);
    TASK_CHECK(!err.empty());
    return 0;
}

static int case_all_removed_empty() {
    Cage g; g.nx = 8; g.ny = 8; g.height = hBowl; g.hk = 0.1; g.build();
    auto sel = g.block(1, 1, 5, 5);
    Bfr::ClippedMesh m; std::string err;
    bool ok = Bfr::ClipRegularSurfaceMesh(
        *g.topo, g.pos, g.uv, sel, {{0, 0, 1}, -100.0}, stdOpt(), m, err);
    TASK_CHECK(ok);
    TASK_CHECK(m.triangles.empty());
    TASK_CHECK(m.positions.empty());
    TASK_CHECK(m.corners.empty());
    return 0;
}

static int case_tighter_tolerance() {
    Cage g; g.nx = 6; g.ny = 6; g.height = hSaddle; g.hk = 0.2; g.build();
    auto sel = g.block(2, 2, 2, 2);
    Bfr::ClipPlane pl{{0.137, -0.219, 1.0}, 0.0831};
    Bfr::ClippedMesh m; std::string err;
    bool ok = Bfr::ClipRegularSurfaceMesh(*g.topo, g.pos, g.uv, sel, pl,
                                          {1e-4, 1e-9, 20}, m, err);
    TASK_CHECK(ok);
    TASK_CHECK(sizesOK(m));
    Oracle o; o.init(*g.topo, g.pos, g.uv);
    double werr = maxGeometricError(m, o);
    std::printf("  [tight geom worst=%.2e tris=%zu]\n", werr, m.triangles.size());
    TASK_CHECK(werr <= 1e-4 + 1e-8);
    return 0;
}

// ===========================================================================
// topology group
// ===========================================================================

static int checkEdgeIncidence(Bfr::ClippedMesh const & m) {
    std::map<std::pair<unsigned, unsigned>, int> count;
    for (auto const & t : m.triangles)
        for (int k = 0; k < 3; ++k) {
            unsigned a = t[k], b = t[(k + 1) % 3];
            if (a > b) std::swap(a, b);
            ++count[{a, b}];
        }
    for (auto const & e : count) {
        TASK_CHECK(e.second == 1 || e.second == 2);
    }
    return 0;
}

static int case_edge_incidence() {
    Cage g; g.nx = 8; g.ny = 8; g.height = hSaddle; g.hk = 0.3; g.build();
    auto sel = g.block(1, 1, 5, 5);
    Bfr::ClippedMesh m; std::string err;
    bool ok = Bfr::ClipRegularSurfaceMesh(
        *g.topo, g.pos, g.uv, sel, {{0.413, -0.287, 1}, 0.113}, stdOpt(), m, err);
    TASK_CHECK(ok);
    TASK_CHECK(sizesOK(m));
    TASK_CHECK(checkEdgeIncidence(m) == 0);
    return 0;
}

// Per-source-face parameter T-junction scan.
static int case_no_t_junctions() {
    Cage g; g.nx = 8; g.ny = 8; g.height = hBowl; g.hk = 0.12; g.build();
    auto sel = g.block(2, 2, 5, 5);
    Bfr::ClippedMesh m; std::string err;
    bool ok = Bfr::ClipRegularSurfaceMesh(
        *g.topo, g.pos, g.uv, sel, {{0.197, 0.113, 1}, -0.347},
        {1e-3, 1e-8, 20}, m, err);
    TASK_CHECK(ok);
    TASK_CHECK(sizesOK(m));

    std::map<int, std::vector<V2>> facePts;
    std::map<int, std::vector<std::pair<V2, V2>>> faceEdges;
    for (size_t t = 0; t < m.triangles.size(); ++t) {
        int f = m.sourceFaces[t];
        V2 p[3];
        for (int k = 0; k < 3; ++k) {
            p[k] = m.corners[3 * t + k].parameter;
            facePts[f].push_back(p[k]);
        }
        faceEdges[f].push_back({p[0], p[1]});
        faceEdges[f].push_back({p[1], p[2]});
        faceEdges[f].push_back({p[2], p[0]});
    }
    for (auto & fp : facePts) {
        int f = fp.first;
        for (auto const & e : faceEdges[f]) {
            V2 a = e.first, b = e.second;
            double ex = b[0] - a[0], ey = b[1] - a[1];
            double el2 = ex * ex + ey * ey;
            if (el2 < 1e-24) continue;
            for (V2 const & p : fp.second) {
                double cr = (p[0] - a[0]) * ey - (p[1] - a[1]) * ex;
                double tpar = ((p[0] - a[0]) * ex + (p[1] - a[1]) * ey) / el2;
                if (std::fabs(cr) <= 1e-8 * std::sqrt(el2) + 1e-12 &&
                    tpar > 1e-6 && tpar < 1.0 - 1e-6) {
                    std::printf("T junction f=%d t=%.4f cr=%.2e\n", f, tpar, cr);
                    TASK_CHECK(0);
                }
            }
        }
    }
    return 0;
}

// Shared coarse edges must expose the same position-index set on both sides,
// even when the two faces need unequal refinement depth / reversed mapping.
static int case_shared_coarse_edges() {
    Cage g; g.nx = 7; g.ny = 5; g.height = hSaddle; g.hk = 0.4; g.build();
    // select two adjacent faces with very different curvature exposure
    std::vector<int> sel = {1 * (6) + 2, 1 * 6 + 3, 2 * 6 + 2, 2 * 6 + 3};
    Bfr::ClippedMesh m; std::string err;
    bool ok = Bfr::ClipRegularSurfaceMesh(
        *g.topo, g.pos, g.uv, sel, {{0.0, 0.0, 1}, 0.15},
        {1e-3, 1e-8, 20}, m, err);
    TASK_CHECK(ok);
    TASK_CHECK(sizesOK(m));

    auto const & lvl = g.topo->GetLevel(0);
    // For every coarse edge joining two selected faces, compare pid sets.
    std::set<int> selSet(sel.begin(), sel.end());
    int nonEmptySharedEdges = 0;
    for (int e = 0; e < lvl.GetNumEdges(); ++e) {
        auto ef = lvl.GetEdgeFaces(e);
        if (ef.size() != 2) continue;
        if (!selSet.count(ef[0]) || !selSet.count(ef[1])) continue;

        auto ev = lvl.GetEdgeVertices(e);
        std::array<std::set<unsigned>, 2> pidSets;
        int fi[2] = {ef[0], ef[1]};
        for (int side = 0; side < 2; ++side) {
            int f = fi[side];
            auto fv = lvl.GetFaceVertices(f);
            int kEdge = -1, orient = 1;
            for (int k = 0; k < 4; ++k)
                if (lvl.GetFaceEdges(f)[k] == e) kEdge = k;
            TASK_CHECK(kEdge >= 0);
            int vk = fv[kEdge], vk1 = fv[(kEdge + 1) % 4];
            if (lvl.GetEdgeVertices(e)[0] == vk &&
                lvl.GetEdgeVertices(e)[1] == vk1)
                orient = 1;
            else
                orient = -1;
            (void) ev; (void) orient;
            for (size_t ci = 0; ci < m.corners.size(); ++ci) {
                size_t t = ci / 3, k = ci % 3;
                if (m.sourceFaces[t] != f) continue;
                V2 p = m.corners[ci].parameter;
                bool on = false;
                if (kEdge == 0 && std::fabs(p[1]) < 1e-7) on = true;
                if (kEdge == 1 && std::fabs(p[0] - 1) < 1e-7) on = true;
                if (kEdge == 2 && std::fabs(p[1] - 1) < 1e-7) on = true;
                if (kEdge == 3 && std::fabs(p[0]) < 1e-7) on = true;
                if (on) pidSets[side].insert(m.triangles[t][k]);
            }
            // An edge fully removed by the plane yields an empty set on both
            // sides; equality below still enforces coordination.
        }
        TASK_CHECK(pidSets[0] == pidSets[1]);
        if (!pidSets[0].empty()) ++nonEmptySharedEdges;
    }
    TASK_CHECK(nonEmptySharedEdges >= 3);
    TASK_CHECK(nonEmptySharedEdges >= 2);
    return 0;
}

static int case_winding_positive_area() {
    Cage g; g.nx = 8; g.ny = 8; g.height = hWave; g.hk = 1.2; g.build();
    auto sel = g.block(1, 1, 5, 5);
    Bfr::ClippedMesh m; std::string err;
    bool ok = Bfr::ClipRegularSurfaceMesh(
        *g.topo, g.pos, g.uv, sel, {{0.211, -0.397, 1}, 0.219}, stdOpt(), m, err);
    TASK_CHECK(ok);
    for (size_t t = 0; t < m.triangles.size(); ++t) {
        V2 a = m.corners[3 * t + 0].parameter;
        V2 b = m.corners[3 * t + 1].parameter;
        V2 c = m.corners[3 * t + 2].parameter;
        double ar = (b[0] - a[0]) * (c[1] - a[1]) -
                    (c[0] - a[0]) * (b[1] - a[1]);
        TASK_CHECK(ar > 0.0);
    }
    return 0;
}

static int case_coincident_components() {
    // Two disconnected but spatially identical 4x4-vertex quad grids, each
    // with an interior regular face.
    int const W = 4;
    std::vector<double> pos;
    std::vector<int> sizes, fv;
    auto addComp = [&](int voff) {
        for (int y = 0; y < W; ++y)
            for (int x = 0; x < W; ++x)
                pos.insert(pos.end(), {(double) x, (double) y, 0.0});
        for (int y = 0; y < W - 1; ++y)
            for (int x = 0; x < W - 1; ++x) {
                sizes.push_back(4);
                int b = voff + y * W + x;
                fv.insert(fv.end(), {b, b + 1, b + W + 1, b + W});
            }
    };
    addComp(0);
    addComp(W * W);
    // Make the second component spatially identical to the first.
    for (int i = W * W; i < 2 * W * W; ++i) {
        int j = i - W * W;
        pos[3 * i + 0] = pos[3 * j + 0];
        pos[3 * i + 1] = pos[3 * j + 1];
        pos[3 * i + 2] = pos[3 * j + 2];
    }

    int nFaces = 2 * (W - 1) * (W - 1);
    std::vector<double> uv(nFaces * 4 * 2, 0.0);
    std::vector<int> uvi;
    for (int v = 0; v < nFaces * 4; ++v) uvi.push_back(v);
    Far::TopologyDescriptor d;
    d.numVertices = 2 * W * W; d.numFaces = nFaces;
    d.numVertsPerFace = sizes.data(); d.vertIndicesPerFace = fv.data();
    Far::TopologyDescriptor::FVarChannel ch;
    ch.numValues = nFaces * 4; ch.valueIndices = uvi.data();
    d.numFVarChannels = 1; d.fvarChannels = &ch;
    Far::TopologyRefinerFactory<Far::TopologyDescriptor>::Options copt(
        Sdc::SCHEME_CATMARK);
    copt.schemeOptions.SetVtxBoundaryInterpolation(
        Sdc::Options::VTX_BOUNDARY_EDGE_ONLY);
    auto topo = Far::TopologyRefinerFactory<
        Far::TopologyDescriptor>::Create(d, copt);
    TASK_CHECK(topo != nullptr);

    // Interior face of each component: cell (1,1) -> id 4 and 9+4=13.
    int f0 = 1 * (W - 1) + 1;
    int f1 = (W - 1) * (W - 1) + f0;
    Bfr::ClippedMesh m; std::string err;
    bool ok = Bfr::ClipRegularSurfaceMesh(
        *topo, pos, uv, {f0, f1}, {{0, 0, 1}, 1.0}, stdOpt(), m, err);
    if (!ok) std::printf("    coincident err: %s\n", err.c_str());
    TASK_CHECK(ok);
    TASK_CHECK(sizesOK(m));
    TASK_CHECK(m.triangles.size() >= 4);

    // Coincident coordinate pairs must exist but use distinct indices.
    std::map<std::array<double, 3>, unsigned> firstCoord;
    int coincidentDistinct = 0;
    for (unsigned i = 0; i < m.positions.size(); ++i) {
        auto key = m.positions[i];
        auto it = firstCoord.find(key);
        if (it == firstCoord.end()) firstCoord[key] = i;
        else if (it->second != i) ++coincidentDistinct;
    }
    TASK_CHECK(coincidentDistinct >= 1);

    // No position index may be shared between the two source faces.
    std::set<unsigned> ids0, ids1;
    for (size_t t = 0; t < m.triangles.size(); ++t) {
        auto & dst = (m.sourceFaces[t] == f0) ? ids0 : ids1;
        for (unsigned i : m.triangles[t]) dst.insert(i);
    }
    std::vector<unsigned> inter;
    std::set_intersection(ids0.begin(), ids0.end(), ids1.begin(), ids1.end(),
                          std::back_inserter(inter));
    TASK_CHECK(inter.empty());
    delete topo;
    return 0;
}

// ===========================================================================
// attributes group
// ===========================================================================

static int case_parameters_and_uv_normals() {
    Cage g; g.nx = 8; g.ny = 8; g.height = hSaddle; g.hk = 0.35; g.build();
    auto sel = g.block(2, 2, 5, 5);
    Bfr::ClippedMesh m; std::string err;
    bool ok = Bfr::ClipRegularSurfaceMesh(
        *g.topo, g.pos, g.uv, sel, {{0.313, 0.107, 1}, 0.013}, stdOpt(), m, err);
    TASK_CHECK(ok);
    TASK_CHECK(sizesOK(m));
    Oracle o; o.init(*g.topo, g.pos, g.uv);

    double uvWorst = 0, nWorst = 0;
    for (size_t ci = 0; ci < m.corners.size(); ++ci) {
        auto const & cc = m.corners[ci];
        TASK_CHECK(cc.parameter[0] >= -1e-12 && cc.parameter[0] <= 1 + 1e-12);
        TASK_CHECK(cc.parameter[1] >= -1e-12 && cc.parameter[1] <= 1 + 1e-12);
        size_t t = ci / 3;
        int f = m.sourceFaces[t];
        V2 refUV = o.evalUV(f, cc.parameter);
        uvWorst = std::max(uvWorst, std::max(std::fabs(refUV[0] - cc.uv[0]),
                                             std::fabs(refUV[1] - cc.uv[1])));
        V3 du, dv;
        o.eval(f, cc.parameter, &du, &dv);
        V3 n = cross(du, dv);
        double nl = norm(n);
        TASK_CHECK(nl > 1e-6);
        n = {n[0] / nl, n[1] / nl, n[2] / nl};
        nWorst = std::max(nWorst, norm(sub(n, cc.normal)));
        TASK_CHECK(std::fabs(norm(cc.normal) - 1.0) < 1e-9);
    }
    std::printf("  [uv worst=%.2e normal worst=%.2e]\n", uvWorst, nWorst);
    TASK_CHECK(uvWorst < 1e-6);
    TASK_CHECK(nWorst < 1e-6);
    return 0;
}

static int case_uv_seam_preserved() {
    // task_support Grid: unique face-varying values per face corner.
    Grid g;
    Bfr::ClippedMesh m; std::string err;
    bool ok = Bfr::ClipRegularSurfaceMesh(
        *g.topology, g.positions, g.uv, {5, 6, 9, 10},
        {{0.211, 0.103, 1}, -0.217}, stdOpt(), m, err);
    TASK_CHECK(ok);
    TASK_CHECK(sizesOK(m));

    // Find a position index reached from two different source faces and
    // confirm the per-corner UV attributes can differ (seam retained).
    std::map<unsigned, std::map<int, V2>> uvByPidFace;
    for (size_t ci = 0; ci < m.corners.size(); ++ci) {
        size_t t = ci / 3, k = ci % 3;
        unsigned pid = m.triangles[t][k];
        int f = m.sourceFaces[t];
        uvByPidFace[pid][f] = m.corners[ci].uv;
    }
    int seams = 0;
    for (auto const & pe : uvByPidFace) {
        if (pe.second.size() < 2) continue;
        auto it = pe.second.begin();
        V2 a = it->second;
        double maxd = 0;
        for (auto const & fe : pe.second) {
            maxd = std::max(maxd, std::hypot(fe.second[0] - a[0],
                                             fe.second[1] - a[1]));
        }
        if (maxd > 1e-4) ++seams;
    }
    TASK_CHECK(seams >= 1);
    return 0;
}

static int case_failure_keeps_output() {
    Grid g;
    Bfr::ClippedMesh sentinel;
    sentinel.positions = {{1.0, 2.0, 3.0}, {4, 5, 6}};
    sentinel.triangles = {{0u, 1u, 0u}};
    sentinel.sourceFaces = {42};
    Bfr::ClipCorner cc;
    cc.parameter = {0.25, 0.75};
    cc.uv = {7.0, 8.0};
    cc.normal = {0, 0, 1};
    sentinel.corners = {cc, cc, cc};

    auto runBad = [&](std::vector<double> const & pos,
                      std::vector<double> const & uv,
                      std::vector<int> const & faces,
                      Bfr::ClipPlane const & pl,
                      Bfr::ClipOptions const & opt) -> int {
        Bfr::ClippedMesh m = sentinel;
        std::string err = "preexisting";
        bool ok = Bfr::ClipRegularSurfaceMesh(*g.topology, pos, uv, faces,
                                              pl, opt, m, err);
        TASK_CHECK(!ok);
        TASK_CHECK(!err.empty());
        TASK_CHECK(sameMesh(m, sentinel));
        return 0;
    };

    Bfr::ClipPlane goodP{{0, 0, 1}, 1};
    Bfr::ClipOptions goodO{1e-3, 1e-8, 16};
    TASK_CHECK(runBad(g.positions, g.uv, {5, 99}, goodP, goodO) == 0);
    TASK_CHECK(runBad(g.positions, g.uv, {5, 5}, goodP, goodO) == 0);
    TASK_CHECK(runBad(g.positions, g.uv, {5}, {{0, 0, 0}, 1}, goodO) == 0);
    TASK_CHECK(runBad(g.positions, g.uv, {5}, goodP, {0.0, 1e-8, 16}) == 0);
    TASK_CHECK(runBad(g.positions, g.uv, {5}, goodP, {-1.0, 1e-8, 16}) == 0);
    TASK_CHECK(runBad(g.positions, g.uv, {5}, goodP, {1e-3, 0.0, 16}) == 0);
    TASK_CHECK(runBad(g.positions, g.uv, {5}, goodP, {1e-3, 1e-8, 0}) == 0);
    TASK_CHECK(runBad(g.positions, g.uv, {5}, goodP, {1e-3, 1e-8, 25}) == 0);

    std::vector<double> badPos = g.positions;
    badPos.push_back(1.0);
    TASK_CHECK(runBad(badPos, g.uv, {5}, goodP, goodO) == 0);
    std::vector<double> nanPos = g.positions;
    nanPos[0] = std::numeric_limits<double>::quiet_NaN();
    TASK_CHECK(runBad(nanPos, g.uv, {5}, goodP, goodO) == 0);
    std::vector<double> badUV = g.uv;
    badUV.push_back(1.0);
    TASK_CHECK(runBad(g.positions, badUV, {5}, goodP, goodO) == 0);
    return 0;
}

static int case_empty_selection() {
    Grid g;
    Bfr::ClippedMesh m;
    m.positions.push_back({9, 9, 9});
    std::string err = "x";
    bool ok = Bfr::ClipRegularSurfaceMesh(
        *g.topology, g.positions, g.uv, {}, {{0, 0, 1}, 1},
        stdOpt(), m, err);
    TASK_CHECK(ok);
    TASK_CHECK(err.empty());
    TASK_CHECK(m.positions.empty());
    TASK_CHECK(m.triangles.empty());
    TASK_CHECK(m.corners.empty());
    return 0;
}

static int case_crease_rejected() {
    // 4x4 vert grid with an interior crease pair.
    int N = 4;
    std::vector<double> pos;
    std::vector<int> sizes, fv;
    for (int y = 0; y < N; ++y)
        for (int x = 0; x < N; ++x)
            pos.insert(pos.end(), {(double) x, (double) y, 0.0});
    for (int y = 0; y < N - 1; ++y)
        for (int x = 0; x < N - 1; ++x) {
            sizes.push_back(4);
            fv.insert(fv.end(), {y * N + x, y * N + x + 1,
                                 (y + 1) * N + x + 1, (y + 1) * N + x});
        }
    int pair[2] = {5, 6};
    float w = 5.0f;
    std::vector<double> uv(9 * 4 * 2, 0.0);
    std::vector<int> uvi;
    for (int i = 0; i < 36; ++i) uvi.push_back(i);
    Far::TopologyDescriptor d;
    d.numVertices = N * N; d.numFaces = 9;
    d.numVertsPerFace = sizes.data(); d.vertIndicesPerFace = fv.data();
    d.numCreases = 1; d.creaseVertexIndexPairs = pair; d.creaseWeights = &w;
    Far::TopologyDescriptor::FVarChannel ch;
    ch.numValues = 36; ch.valueIndices = uvi.data();
    d.numFVarChannels = 1; d.fvarChannels = &ch;
    Far::TopologyRefinerFactory<Far::TopologyDescriptor>::Options o(
        Sdc::SCHEME_CATMARK);
    o.schemeOptions.SetVtxBoundaryInterpolation(
        Sdc::Options::VTX_BOUNDARY_EDGE_ONLY);
    auto topo = Far::TopologyRefinerFactory<
        Far::TopologyDescriptor>::Create(d, o);
    TASK_CHECK(topo != nullptr);
    Bfr::ClippedMesh m; std::string err;
    bool ok = Bfr::ClipRegularSurfaceMesh(
        *topo, pos, uv, {4}, {{0, 0, 1}, 1}, stdOpt(), m, err);
    TASK_CHECK(!ok);
    TASK_CHECK(!err.empty());
    delete topo;
    return 0;
}

// ---------------------------------------------------------------------------
// Registry
// ---------------------------------------------------------------------------

task_case const task_cases[] = {
    {"bounds", "flat_full_area", case_flat_full_area},
    {"bounds", "flat_oblique_bounds", case_flat_oblique_bounds},
    {"bounds", "curved_geom_error", case_curved_geom_error},
    {"bounds", "closed_loop_same_corners", case_closed_loop_same_corners},
    {"bounds", "depth_exhaustion_fails", case_depth_exhaustion_fails},
    {"bounds", "tangent_rejected", case_tangent_rejected},
    {"bounds", "coplanar_rejected", case_coplanar_rejected},
    {"bounds", "all_removed_empty", case_all_removed_empty},
    {"bounds", "tighter_tolerance", case_tighter_tolerance},

    {"topology", "edge_incidence", case_edge_incidence},
    {"topology", "no_t_junctions", case_no_t_junctions},
    {"topology", "shared_coarse_edges", case_shared_coarse_edges},
    {"topology", "winding_positive_area", case_winding_positive_area},
    {"topology", "coincident_components", case_coincident_components},

    {"attributes", "parameters_uv_normals", case_parameters_and_uv_normals},
    {"attributes", "uv_seam_preserved", case_uv_seam_preserved},
    {"attributes", "failure_keeps_output", case_failure_keeps_output},
    {"attributes", "empty_selection", case_empty_selection},
    {"attributes", "crease_rejected", case_crease_rejected},
};

size_t const task_case_count =
    sizeof(task_cases) / sizeof(task_cases[0]);
