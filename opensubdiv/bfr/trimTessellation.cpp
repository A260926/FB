//
//   Copyright 2025 Pixar
//
//   Licensed under the terms set forth in the LICENSE.txt file available at
//   https://opensubdiv.org/license.
//

#include "trimTessellation.h"

#include "refinerSurfaceFactory.h"
#include "surface.h"
#include "../far/topologyRefiner.h"
#include "../sdc/types.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <queue>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace OpenSubdiv {
namespace OPENSUBDIV_VERSION {
namespace Bfr {
namespace clipdetail {

using V2 = std::array<double, 2>;
using V3 = std::array<double, 3>;

static constexpr int64_t KEY_BITS = 24;
static constexpr int64_t KEY_SCALE = int64_t(1) << KEY_BITS;

// ---------------------------------------------------------------------------
// Small vector helpers
// ---------------------------------------------------------------------------

static inline V3
vadd(V3 const & a, V3 const & b) {
    return {a[0] + b[0], a[1] + b[1], a[2] + b[2]};
}
static inline V3
vsub(V3 const & a, V3 const & b) {
    return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
}
static inline V3
vmul(V3 const & a, double s) {
    return {a[0] * s, a[1] * s, a[2] * s};
}
static inline double
vdot(V3 const & a, V3 const & b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
static inline V3
vcross(V3 const & a, V3 const & b) {
    return {a[1] * b[2] - a[2] * b[1],
            a[2] * b[0] - a[0] * b[2],
            a[0] * b[1] - a[1] * b[0]};
}
static inline double
vnorm(V3 const & a) {
    return std::sqrt(vdot(a, a));
}

static inline double
lerp(double a, double b, double t) {
    return a + (b - a) * t;
}
static inline V2
lerp2(V2 const & a, V2 const & b, double t) {
    return {lerp(a[0], b[0], t), lerp(a[1], b[1], t)};
}

// Convert values of a cubic polynomial sampled at t = 0,1/3,2/3,1 into
// cubic Bernstein/Bezier control values.
static void
nodesToBezier(double const v[4], double b[4]) {
    b[0] = v[0];
    b[3] = v[3];
    double A = 27.0 * v[1] - 8.0 * v[0] - v[3];
    double C = 27.0 * v[2] - v[0] - 8.0 * v[3];
    b[2] = (2.0 * C - A) / 18.0;
    b[1] = (A - 6.0 * b[2]) / 12.0;
}

static inline double
bezier1(double const b[4], double t) {
    double a = 1.0 - t;
    return a * a * a * b[0] + 3.0 * a * a * t * b[1] +
           3.0 * a * t * t * b[2] + t * t * t * b[3];
}

static void
splitCubic(double const b[4], double t, double l[4], double r[4]) {
    double mt = 1.0 - t;
    double b01 = mt * b[0] + t * b[1];
    double b12 = mt * b[1] + t * b[2];
    double b23 = mt * b[2] + t * b[3];
    double b012 = mt * b01 + t * b12;
    double b123 = mt * b12 + t * b23;
    double b0123 = mt * b012 + t * b123;
    l[0] = b[0]; l[1] = b01; l[2] = b012; l[3] = b0123;
    r[0] = b0123; r[1] = b123; r[2] = b23; r[3] = b[3];
}

// Extract the Bezier control values of the sub-segment [a,c] of cubic b.
static void
extractSegment1D(V3 const b[4], double a, double c, V3 out[4]) {
    auto lerpV = [](V3 const & x, V3 const & y, double t) {
        return vadd(vmul(x, 1.0 - t), vmul(y, t));
    };
    auto splitV3 = [&](V3 const in[4], double t, V3 l[4], V3 r[4]) {
        V3 b01 = lerpV(in[0], in[1], t);
        V3 b12 = lerpV(in[1], in[2], t);
        V3 b23 = lerpV(in[2], in[3], t);
        V3 b012 = lerpV(b01, b12, t);
        V3 b123 = lerpV(b12, b23, t);
        V3 b0123 = lerpV(b012, b123, t);
        l[0] = in[0]; l[1] = b01; l[2] = b012; l[3] = b0123;
        r[0] = b0123; r[1] = b123; r[2] = b23; r[3] = in[3];
    };

    V3 cur[4] = {b[0], b[1], b[2], b[3]};
    if (a > 0.0) {
        V3 l[4], r[4];
        splitV3(cur, a, l, r);
        for (int i = 0; i < 4; ++i) cur[i] = r[i]; // covers [a,1]
        double cc = (c - a) / (1.0 - a);
        if (cc < 1.0) {
            V3 l2[4], r2[4];
            splitV3(cur, cc, l2, r2);
            for (int i = 0; i < 4; ++i) out[i] = l2[i];
            return;
        }
        for (int i = 0; i < 4; ++i) out[i] = cur[i];
        return;
    }
    if (c < 1.0) {
        V3 l[4], r[4];
        splitV3(cur, c, l, r);
        for (int i = 0; i < 4; ++i) out[i] = l[i];
        return;
    }
    for (int i = 0; i < 4; ++i) out[i] = cur[i];
}

// Extract the bicubic Bezier net over parameter rectangle [u0,u1]x[v0,v1].
static void
subRectNet(V3 const net[16], double u0, double u1,
           double v0, double v1, V3 out[16]) {
    V3 unet[4][4];
    for (int iv = 0; iv < 4; ++iv) {
        V3 col[4] = {net[0 * 4 + iv], net[1 * 4 + iv],
                     net[2 * 4 + iv], net[3 * 4 + iv]};
        V3 seg[4];
        extractSegment1D(col, u0, u1, seg);
        for (int iu = 0; iu < 4; ++iu) unet[iu][iv] = seg[iu];
    }
    for (int iu = 0; iu < 4; ++iu) {
        V3 row[4] = {unet[iu][0], unet[iu][1], unet[iu][2], unet[iu][3]};
        V3 seg[4];
        extractSegment1D(row, v0, v1, seg);
        for (int iv = 0; iv < 4; ++iv) out[iu * 4 + iv] = seg[iv];
    }
}

static V3
evalBezierNet(V3 const net[16], double u, double v) {
    // Evaluate u curves for each v control, then the v curve, componentwise.
    V3 cols[4];
    for (int iv = 0; iv < 4; ++iv) {
        double b0[4] = {net[0 * 4 + iv][0], net[1 * 4 + iv][0],
                        net[2 * 4 + iv][0], net[3 * 4 + iv][0]};
        double b1[4] = {net[0 * 4 + iv][1], net[1 * 4 + iv][1],
                        net[2 * 4 + iv][1], net[3 * 4 + iv][1]};
        double b2[4] = {net[0 * 4 + iv][2], net[1 * 4 + iv][2],
                        net[2 * 4 + iv][2], net[3 * 4 + iv][2]};
        cols[iv] = {bezier1(b0, u), bezier1(b1, u), bezier1(b2, u)};
    }
    double b0[4] = {cols[0][0], cols[1][0], cols[2][0], cols[3][0]};
    double b1[4] = {cols[0][1], cols[1][1], cols[2][1], cols[3][1]};
    double b2[4] = {cols[0][2], cols[1][2], cols[2][2], cols[3][2]};
    V3 out = {bezier1(b0, v), bezier1(b1, v), bezier1(b2, v)};
    return out;
}

// ---------------------------------------------------------------------------
// Topological site identity for shared positions
// ---------------------------------------------------------------------------

struct Site {
    int     kind; // 0 coarse vertex, 1 coarse edge point, 2 face interior
    int     a;    // coarse vertex id / coarse edge id / face slot
    int64_t b;    // canonical T key / iu key
    int64_t c;    // unused for kind 0/1, iv key for kind 2

    bool operator==(Site const & o) const {
        return kind == o.kind && a == o.a && b == o.b && c == o.c;
    }
};

struct SiteHash {
    size_t operator()(Site const & s) const {
        uint64_t h = std::hash<int>()(s.kind);
        auto mix = [&](uint64_t x) {
            x += 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
            x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
            x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
            return h ^ (x >> 31);
        };
        h = mix((uint64_t) std::hash<int>()(s.a));
        h = mix((uint64_t) s.b);
        h = mix((uint64_t) s.c);
        return (size_t) h;
    }
};

using EdgeKey = std::pair<int, int>;

struct EdgeKeyHash {
    size_t operator()(EdgeKey const & k) const {
        uint64_t x = (uint64_t) (uint32_t) k.first;
        uint64_t y = (uint64_t) (uint32_t) k.second;
        uint64_t h = x * 0x9e3779b97f4a7c15ULL;
        h ^= y + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        return (size_t) h;
    }
};

static EdgeKey
makeEdgeKey(int p, int q) {
    return (p < q) ? EdgeKey(p, q) : EdgeKey(q, p);
}

// ---------------------------------------------------------------------------
// Per-face evaluated surface
// ---------------------------------------------------------------------------

struct FaceData {
    int faceId = -1;
    Bfr::Surface<double> surf;
    std::vector<double> patchPoints;

    Bfr::Surface<double> uvSurf;
    std::vector<double> uvPatchPoints;
    bool hasUV = false;

    V3 bez[16]; // [iu*4 + iv], fitted bicubic Bezier net

    int fv[4] = {-1, -1, -1, -1};
    int fe[4] = {-1, -1, -1, -1};
    int orient[4] = {1, 1, 1, 1};

    double hessianBound = 0.0;
};

struct Corner {
    int     pid = -1; // index into Impl::positions
    double  u = 0.0, v = 0.0;
    int64_t ku = 0, kv = 0;
};

struct Tria {
    int slot = -1;
    std::array<Corner, 3> c;
    int  depth = 0;
    bool alive = true;
    int  kind = 0;
};

struct RootInfo {
    bool ambiguous = false;
    std::vector<double> roots;
};

// ---------------------------------------------------------------------------
// Engine implementation
// ---------------------------------------------------------------------------

struct TrimTessellation::Impl {
    Far::TopologyRefiner const & topology;
    std::vector<double> const & meshPositions;
    std::vector<double> const & meshUV;
    std::vector<int> const & faceIds;
    std::array<double, 3> planeN;
    double planeD;
    ClipOptions options;

    Far::TopologyLevel const * level = nullptr;
    std::unique_ptr<Bfr::RefinerSurfaceFactory<>> factory;

    std::vector<FaceData> faces;

    // coarse component canonical evaluation records
    std::unordered_map<int, std::pair<int, int>> vertCanon; // vid -> slot,corner
    struct EdgeCanon { int slot; int side; int orient; };
    std::unordered_map<int, EdgeCanon> edgeCanon;

    std::vector<V3> positions;
    std::unordered_map<Site, int, SiteHash> sitePos;

    std::vector<Tria> trias;
    std::unordered_map<EdgeKey, std::vector<int>, EdgeKeyHash> edgeInc;
    std::unordered_map<EdgeKey, int, EdgeKeyHash> edgeCut;

    double coordScale = 1.0;
    double zeroEps = 1e-10;

    Impl(Far::TopologyRefiner const & topo,
         std::vector<double> const & pos,
         std::vector<double> const & uv,
         std::vector<int> const & sel,
         std::array<double, 3> const & n,
         double d,
         ClipOptions const & opt)
        : topology(topo), meshPositions(pos), meshUV(uv), faceIds(sel),
          planeN(n), planeD(d), options(opt) {}

    bool fail(std::string const & msg, std::string & error) {
        error = msg;
        return false;
    }

    double hAt(V3 const & p) const {
        return vdot(planeN, p) + planeD;
    }
    double hAtPid(int pid) const { return hAt(positions[pid]); }

    V3 evalBfr(FaceData & F, V2 const & p,
               V3 * du = nullptr, V3 * dv = nullptr) {
        double uv[2] = {p[0], p[1]};
        V3 P = {0, 0, 0};
        if (du && dv) {
            F.surf.Evaluate(uv, F.patchPoints.data(), 3,
                            P.data(), du->data(), dv->data());
        } else {
            F.surf.Evaluate(uv, F.patchPoints.data(), 3, P.data());
        }
        return P;
    }

    double hTrue(FaceData & F, V2 const & p) {
        return hAt(evalBfr(F, p));
    }

    double hPoly(FaceData const & F, V2 const & p) const {
        V3 P = evalBezierNet(F.bez, p[0], p[1]);
        return hAt(P);
    }

    // -- topology validation -------------------------------------------------

    bool validateTopology(std::string & error) {
        level = &topology.GetLevel(0);

        if (topology.GetSchemeType() != Sdc::SCHEME_CATMARK) {
            return fail("ClipRegularSurfaceMesh: only Catmull-Clark topology is supported", error);
        }

        int nv = level->GetNumVertices();
        int ne = level->GetNumEdges();

        for (int e = 0; e < ne; ++e) {
            if (level->IsEdgeNonManifold(e)) {
                return fail("ClipRegularSurfaceMesh: non-manifold edge in cage", error);
            }
            // Boundary interpolation rules may sharpen cage boundary edges;
            // only sharpness on an interior edge is an explicit crease.
            if (level->GetEdgeSharpness(e) > 0.0f &&
                !level->IsEdgeBoundary(e)) {
                std::ostringstream os;
                os << "ClipRegularSurfaceMesh: explicit sharpness on edge " << e;
                return fail(os.str(), error);
            }
        }
        for (int v = 0; v < nv; ++v) {
            if (level->IsVertexNonManifold(v)) {
                return fail("ClipRegularSurfaceMesh: non-manifold vertex in cage", error);
            }
            if (level->GetVertexSharpness(v) > 0.0f &&
                !level->IsVertexBoundary(v)) {
                std::ostringstream os;
                os << "ClipRegularSurfaceMesh: explicit sharpness at vertex " << v;
                return fail(os.str(), error);
            }
        }

        // Consistent orientation across every manifold interior edge.
        for (int e = 0; e < ne; ++e) {
            auto ef = level->GetEdgeFaces(e);
            if (ef.size() != 2) continue;
            auto ev = level->GetEdgeVertices(e);
            int a = ev[0], b = ev[1];
            int dir[2] = {0, 0};
            for (int k = 0; k < 2; ++k) {
                auto fv = level->GetFaceVertices(ef[k]);
                int na = -1, nb = -1;
                for (int i = 0; i < fv.size(); ++i) {
                    if (fv[i] == a) na = i;
                    if (fv[i] == b) nb = i;
                }
                if (na < 0 || nb < 0) { dir[k] = 0; continue; }
                int n = fv.size();
                if (fv[(na + 1) % n] == b) dir[k] = 1;       // a -> b
                else if (fv[(nb + 1) % n] == a) dir[k] = -1;  // b -> a
            }
            if (dir[0] == 0 || dir[1] == 0 || dir[0] == dir[1]) {
                return fail("ClipRegularSurfaceMesh: inconsistently oriented (non-orientable) cage", error);
            }
        }

        for (int fid : faceIds) {
            if (level->IsFaceHole(fid)) {
                std::ostringstream os;
                os << "ClipRegularSurfaceMesh: face " << fid << " is a hole";
                return fail(os.str(), error);
            }
            if (level->GetFaceVertices(fid).size() != 4) {
                std::ostringstream os;
                os << "ClipRegularSurfaceMesh: selected face " << fid << " is not a quad";
                return fail(os.str(), error);
            }
        }
        return true;
    }

    // -- per-face surface setup ----------------------------------------------

    bool initFace(int slot, std::string & error) {
        int fid = faceIds[slot];
        FaceData & F = faces[slot];
        F.faceId = fid;

        auto fv = level->GetFaceVertices(fid);
        auto fe = level->GetFaceEdges(fid);
        for (int k = 0; k < 4; ++k) {
            F.fv[k] = fv[k];
            F.fe[k] = fe[k];
            auto ev = level->GetEdgeVertices(fe[k]);
            int vk = fv[k];
            int vk1 = fv[(k + 1) % 4];
            if (ev[0] == vk && ev[1] == vk1) F.orient[k] = +1;
            else if (ev[0] == vk1 && ev[1] == vk) F.orient[k] = -1;
            else {
                return fail("ClipRegularSurfaceMesh: topology edge/vertex mismatch", error);
            }
        }

        if (!factory->InitVertexSurface(fid, &F.surf) ||
            !F.surf.IsValid() || !F.surf.IsRegular() || F.surf.IsLinear() ||
            F.surf.GetFaceSize() != 4 ||
            F.surf.GetParameterization().GetType() != Parameterization::QUAD) {
            std::ostringstream os;
            os << "ClipRegularSurfaceMesh: position Surface of selected face "
               << fid << " is not a regular bicubic quad patch";
            return fail(os.str(), error);
        }

        int np = F.surf.GetNumPatchPoints();
        if (np != 16) {
            std::ostringstream os;
            os << "ClipRegularSurfaceMesh: selected face " << fid
               << " regular patch has " << np << " patch points (expected 16)";
            return fail(os.str(), error);
        }
        F.patchPoints.assign((size_t) np * 3, 0.0);
        F.surf.PreparePatchPoints(meshPositions.data(), 3,
                                  F.patchPoints.data(), 3);
        for (double x : F.patchPoints) {
            if (!std::isfinite(x)) {
                return fail("ClipRegularSurfaceMesh: non-finite evaluated patch points", error);
            }
        }

        // Channel-0 face-varying surface for UVs.  It need not be regular at
        // seams, but when a channel exists it should initialize; failure to
        // provide UV evaluation is tolerated with zero UV coordinates.
        if (topology.GetNumFVarChannels() > 0 &&
            factory->InitFaceVaryingSurface(fid, &F.uvSurf, 0) &&
            F.uvSurf.IsValid()) {
            int nup = F.uvSurf.GetNumPatchPoints();
            F.uvPatchPoints.assign((size_t) nup * 2, 0.0);
            F.uvSurf.PreparePatchPoints(meshUV.data(), 2,
                                        F.uvPatchPoints.data(), 2);
            F.hasUV = true;
        }

        // Fit an exact bicubic Bezier net from real Bfr evaluations.
        double const nodes[4] = {0.0, 1.0 / 3.0, 2.0 / 3.0, 1.0};
        V3 val[4][4];
        for (int iu = 0; iu < 4; ++iu) {
            for (int iv = 0; iv < 4; ++iv) {
                double uv[2] = {nodes[iu], nodes[iv]};
                F.surf.Evaluate(uv, F.patchPoints.data(), 3, val[iu][iv].data());
            }
        }
        V3 tmp[4][4];
        for (int iv = 0; iv < 4; ++iv) {
            for (int c = 0; c < 3; ++c) {
                double v[4] = {val[0][iv][c], val[1][iv][c],
                               val[2][iv][c], val[3][iv][c]};
                double b[4];
                nodesToBezier(v, b);
                for (int iu = 0; iu < 4; ++iu) tmp[iu][iv][c] = b[iu];
            }
        }
        for (int iu = 0; iu < 4; ++iu) {
            for (int c = 0; c < 3; ++c) {
                double v[4] = {tmp[iu][0][c], tmp[iu][1][c],
                               tmp[iu][2][c], tmp[iu][3][c]};
                double b[4];
                nodesToBezier(v, b);
                for (int iv = 0; iv < 4; ++iv) F.bez[iu * 4 + iv][c] = b[iv];
            }
        }

        // Verify the fitted polynomial reproduces Bfr evaluation.
        for (int q = 0; q < 24; ++q) {
            double u = std::fmod((q + 1) * 0.6180339887498949, 1.0);
            double v = std::fmod((q + 3) * 0.4142135623730951, 1.0);
            V3 fit = evalBezierNet(F.bez, u, v);
            V3 real = evalBfr(F, {u, v});
            double err = std::max({std::fabs(fit[0] - real[0]),
                                   std::fabs(fit[1] - real[1]),
                                   std::fabs(fit[2] - real[2])});
            if (err > 1e-7 * std::max(1.0, coordScale)) {
                return fail("ClipRegularSurfaceMesh: regular patch is not a single bicubic polynomial", error);
            }
        }

        // Conservative global Hessian bound from second-difference Bezier
        // control nets (convex hull property).
        double A = 0.0, C = 0.0, B = 0.0;
        for (int iu = 0; iu < 2; ++iu)
            for (int iv = 0; iv < 4; ++iv) {
                V3 d = vsub(vadd(F.bez[(iu + 2) * 4 + iv],
                                 F.bez[iu * 4 + iv]),
                            vmul(F.bez[(iu + 1) * 4 + iv], 2.0));
                A = std::max(A, 6.0 * vnorm(d));
            }
        for (int iu = 0; iu < 4; ++iu)
            for (int iv = 0; iv < 2; ++iv) {
                V3 d = vsub(vadd(F.bez[iu * 4 + iv + 2],
                                 F.bez[iu * 4 + iv]),
                            vmul(F.bez[iu * 4 + iv + 1], 2.0));
                C = std::max(C, 6.0 * vnorm(d));
            }
        for (int iu = 0; iu < 3; ++iu)
            for (int iv = 0; iv < 3; ++iv) {
                V3 d = vadd(vsub(F.bez[(iu + 1) * 4 + iv + 1],
                                 F.bez[(iu + 1) * 4 + iv]),
                            vsub(F.bez[iu * 4 + iv],
                                 F.bez[iu * 4 + iv + 1]));
                B = std::max(B, 9.0 * vnorm(d));
            }
        F.hessianBound = A + B + C + 1e-12 * std::max(1.0, coordScale);
        return true;
    }

    // -- canonical sites / positions ----------------------------------------

    void buildCanonicalRecords() {
        for (int s = 0; s < (int) faces.size(); ++s) {
            FaceData & F = faces[s];
            for (int k = 0; k < 4; ++k) {
                if (!vertCanon.count(F.fv[k]))
                    vertCanon[F.fv[k]] = {s, k};
                if (!edgeCanon.count(F.fe[k]))
                    edgeCanon[F.fe[k]] = {s, k, F.orient[k]};
            }
        }
    }

    int internSite(Site const & site) {
        auto it = sitePos.find(site);
        if (it != sitePos.end()) return it->second;

        V3 P = {0, 0, 0};
        if (site.kind == 0) {
            auto rc = vertCanon.at(site.a);
            double uv[2];
            faces[rc.first].surf.GetParameterization().GetVertexCoord(
                rc.second, uv);
            P = evalBfr(faces[rc.first], {uv[0], uv[1]});
        } else if (site.kind == 1) {
            EdgeCanon const & ec = edgeCanon.at(site.a);
            double tLocal = (ec.orient > 0)
                ? (double) site.b / (double) KEY_SCALE
                : 1.0 - (double) site.b / (double) KEY_SCALE;
            double uv[2];
            // Parameterization edge index is the face side k.
            faces[ec.slot].surf.GetParameterization().GetEdgeCoord(
                ec.side, tLocal, uv);
            P = evalBfr(faces[ec.slot], {uv[0], uv[1]});
        } else {
            P = evalBfr(faces[site.a], {
                (double) site.b / (double) KEY_SCALE,
                (double) site.c / (double) KEY_SCALE});
        }
        int pid = (int) positions.size();
        positions.push_back(P);
        sitePos.emplace(site, pid);
        return pid;
    }

    Site pointSite(int slot, int64_t ku, int64_t kv) const {
        FaceData const & F = faces[slot];
        Site s{2, slot, ku, kv};

        bool cu0 = (ku == 0), cu1 = (ku == KEY_SCALE);
        bool cv0 = (kv == 0), cv1 = (kv == KEY_SCALE);
        if ((cu0 || cu1) && (cv0 || cv1)) {
            int k = cv0 ? (cu0 ? 0 : 1) : (cu1 ? 2 : 3);
            return Site{0, F.fv[k], 0, 0};
        }
        auto edgeSite = [&](int side, int64_t tLocalKey) -> Site {
            int64_t T = (F.orient[side] > 0)
                ? tLocalKey : (KEY_SCALE - tLocalKey);
            return Site{1, F.fe[side], T, 0};
        };
        if (cv0) return edgeSite(0, ku);                 // (t,0)
        if (cu1) return edgeSite(1, kv);                 // (1,t)
        if (cv1) return edgeSite(2, KEY_SCALE - ku);     // (1-t,1)
        if (cu0) return edgeSite(3, KEY_SCALE - kv);     // (0,1-t)
        return s;
    }

    int internPoint(int slot, int64_t ku, int64_t kv) {
        return internSite(pointSite(slot, ku, kv));
    }

    // -- initial mesh --------------------------------------------------------

    void buildInitialMesh() {
        for (int s = 0; s < (int) faces.size(); ++s) {
            Corner cc[4];
            for (int k = 0; k < 4; ++k) {
                double uv[2];
                faces[s].surf.GetParameterization().GetVertexCoord(k, uv);
                int64_t ku = (int64_t) std::llround(uv[0] * KEY_SCALE);
                int64_t kv = (int64_t) std::llround(uv[1] * KEY_SCALE);
                cc[k] = {internPoint(s, ku, kv), uv[0], uv[1], ku, kv};
            }
            Tria t0; t0.slot = s; t0.c = {cc[0], cc[1], cc[2]};
            Tria t1; t1.slot = s; t1.c = {cc[0], cc[2], cc[3]};
            trias.push_back(std::move(t0));
            trias.push_back(std::move(t1));
        }
        for (int t = 0; t < (int) trias.size(); ++t) indexTria(t);
    }

    static double paramArea2(Tria const & t) {
        double u0 = t.c[0].u, v0 = t.c[0].v;
        return (t.c[1].u - u0) * (t.c[2].v - v0) -
               (t.c[2].u - u0) * (t.c[1].v - v0);
    }

    static void makeCCW(std::array<Corner, 3> & c) {
        double a2 = (c[1].u - c[0].u) * (c[2].v - c[0].v) -
                    (c[2].u - c[0].u) * (c[1].v - c[0].v);
        if (a2 < 0.0) std::swap(c[1], c[2]);
    }

    void indexTria(int tid) {
        Tria & t = trias[tid];
        for (int k = 0; k < 3; ++k) {
            EdgeKey key = makeEdgeKey(t.c[k].pid, t.c[(k + 1) % 3].pid);
            edgeInc[key].push_back(tid);
        }
    }

    void unindexTria(int tid) {
        Tria const & t = trias[tid];
        for (int k = 0; k < 3; ++k) {
            EdgeKey key = makeEdgeKey(t.c[k].pid, t.c[(k + 1) % 3].pid);
            auto it = edgeInc.find(key);
            if (it == edgeInc.end()) continue;
            auto & v = it->second;
            v.erase(std::remove(v.begin(), v.end(), tid), v.end());
        }
    }

    // -- conservative root isolation on a parameter edge ---------------------

    RootInfo isolateEdgeRoots(FaceData const & F,
                              Corner const & A, Corner const & B) const {
        RootInfo out;
        double hScale = 1.0;
        for (V3 const & b : F.bez) hScale = std::max(hScale, std::fabs(hAt(b)));

        double qv[4];
        double const nodes[4] = {0.0, 1.0 / 3.0, 2.0 / 3.0, 1.0};
        for (int i = 0; i < 4; ++i) {
            double u = lerp(A.u, B.u, nodes[i]);
            double v = lerp(A.v, B.v, nodes[i]);
            qv[i] = hPoly(F, {u, v});
        }
        double qb[4];
        nodesToBezier(qv, qb);

        int nodeBudget = 20000;
        std::function<void(double[4], double, double, int)> rec =
            [&](double b[4], double t0, double t1, int depth) {
                if (--nodeBudget < 0) { out.ambiguous = true; return; }
                double eps = 1e-11 * hScale;
                double lo = std::min({b[0], b[1], b[2], b[3]}) - eps;
                double hi = std::max({b[0], b[1], b[2], b[3]}) + eps;
                if (lo > 0.0 || hi < 0.0) return;
                if (depth > 30) { out.ambiguous = true; return; }

                double db[3] = {3.0 * (b[1] - b[0]),
                                3.0 * (b[2] - b[1]),
                                3.0 * (b[3] - b[2])};
                double deps = 1e-10 * hScale;
                double dlo = std::min({db[0], db[1], db[2]}) - deps;
                double dhi = std::max({db[0], db[1], db[2]}) + deps;
                if (dlo > 0.0 || dhi < 0.0) {
                    bool crosses =
                        (b[0] <= 0.0 && b[3] >= 0.0) ||
                        (b[3] <= 0.0 && b[0] >= 0.0);
                    if (crosses) {
                        // Locate the single root in local [0,1] by bisection
                        // of the Bernstein polynomial.
                        double x0 = 0.0, x1 = 1.0;
                        double f0 = bezier1(b, x0);
                        for (int it = 0; it < 45; ++it) {
                            double xm = 0.5 * (x0 + x1);
                            double fm = bezier1(b, xm);
                            if ((fm < 0.0) == (f0 < 0.0)) { x0 = xm; f0 = fm; }
                            else x1 = xm;
                        }
                        double x = 0.5 * (x0 + x1);
                        out.roots.push_back(t0 + x * (t1 - t0));
                    }
                    return;
                }
                double l[4], r[4];
                splitCubic(b, 0.5, l, r);
                double tm = 0.5 * (t0 + t1);
                rec(l, t0, tm, depth + 1);
                rec(r, tm, t1, depth + 1);
            };
        rec(qb, 0.0, 1.0, 0);

        std::sort(out.roots.begin(), out.roots.end());
        std::vector<double> dedup;
        for (double t : out.roots) {
            if (dedup.empty() || std::fabs(t - dedup.back()) > 1e-8)
                dedup.push_back(t);
        }
        out.roots = std::move(dedup);
        return out;
    }

    // -- classification ------------------------------------------------------

    enum Kind { REFINE = 0, ALL_IN = 1, ALL_OUT = 2, DO_CLIP = 3 };

    int classify(int tid) {
        Tria const & t = trias[tid];
        FaceData const & F = faces[t.slot];

        int64_t kuMin = std::min({t.c[0].ku, t.c[1].ku, t.c[2].ku});
        int64_t kuMax = std::max({t.c[0].ku, t.c[1].ku, t.c[2].ku});
        int64_t kvMin = std::min({t.c[0].kv, t.c[1].kv, t.c[2].kv});
        int64_t kvMax = std::max({t.c[0].kv, t.c[1].kv, t.c[2].kv});
        double u0 = (double) kuMin / (double) KEY_SCALE;
        double u1 = (double) kuMax / (double) KEY_SCALE;
        double v0 = (double) kvMin / (double) KEY_SCALE;
        double v1 = (double) kvMax / (double) KEY_SCALE;
        double wu = u1 - u0;
        double wv = v1 - v0;

        // Conservative local bicubic Bernstein net over the triangle bbox.
        V3 L[16];
        subRectNet(F.bez, u0, u1, v0, v1, L);

        double hullMargin = 1e-10 * std::max(1.0, coordScale);
        double hMin = std::numeric_limits<double>::infinity();
        double hMax = -std::numeric_limits<double>::infinity();
        for (V3 const & b : L) {
            double hb = hAt(b);
            hMin = std::min(hMin, hb);
            hMax = std::max(hMax, hb);
        }
        double hLo = hMin - hullMargin;
        double hHi = hMax + hullMargin;

        // Local second-difference control magnitudes (unscaled by widths).
        double AU = 0.0, AV = 0.0, BUV = 0.0;
        for (int iu = 0; iu < 2; ++iu)
            for (int iv = 0; iv < 4; ++iv) {
                V3 d = vsub(vadd(L[(iu + 2) * 4 + iv],
                                 L[iu * 4 + iv]),
                            vmul(L[(iu + 1) * 4 + iv], 2.0));
                AU = std::max(AU, 6.0 * vnorm(d));
            }
        for (int iu = 0; iu < 4; ++iu)
            for (int iv = 0; iv < 2; ++iv) {
                V3 d = vsub(vadd(L[iu * 4 + iv + 2],
                                 L[iu * 4 + iv]),
                            vmul(L[iu * 4 + iv + 1], 2.0));
                AV = std::max(AV, 6.0 * vnorm(d));
            }
        for (int iu = 0; iu < 3; ++iu)
            for (int iv = 0; iv < 3; ++iv) {
                V3 d = vadd(vsub(L[(iu + 1) * 4 + iv + 1],
                                 L[(iu + 1) * 4 + iv]),
                            vsub(L[iu * 4 + iv],
                                 L[iu * 4 + iv + 1]));
                BUV = std::max(BUV, 9.0 * vnorm(d));
            }

        // Interpolation error bound .5*(A+B+C)*(wu^2+wv^2), expressed without
        // dividing by the (possibly very small) widths.
        double ratioU = wu / std::max(wv, 1.0 / (double) KEY_SCALE);
        double ratioV = wv / std::max(wu, 1.0 / (double) KEY_SCALE);
        double errorBound = 0.5 * (
            AU * (1.0 + ratioV * ratioV) +
            AV * (1.0 + ratioU * ratioU) +
            BUV * (ratioU + ratioV));
        bool errorOK = errorBound <= options.geometricTolerance * 0.6;

        if (hLo > 0.0) return errorOK ? ALL_IN : REFINE;
        if (hHi < 0.0) return ALL_OUT;
        if (!errorOK) return REFINE;

        // Conservative gradient component intervals from first-difference
        // derivative control nets (convex hull property).
        double guLo = std::numeric_limits<double>::infinity();
        double guHi = -std::numeric_limits<double>::infinity();
        double gvLo = guLo, gvHi = guHi;
        for (int iu = 0; iu < 3; ++iu)
            for (int iv = 0; iv < 4; ++iv) {
                double gu = vdot(planeN, vmul(
                    vsub(L[(iu + 1) * 4 + iv], L[iu * 4 + iv]),
                    3.0 / wu));
                guLo = std::min(guLo, gu);
                guHi = std::max(guHi, gu);
            }
        for (int iu = 0; iu < 4; ++iu)
            for (int iv = 0; iv < 3; ++iv) {
                double gv = vdot(planeN, vmul(
                    vsub(L[iu * 4 + iv + 1], L[iu * 4 + iv]),
                    3.0 / wv));
                gvLo = std::min(gvLo, gv);
                gvHi = std::max(gvHi, gv);
            }
        double gm = 1e-11 * std::max(1.0, coordScale);
        double mx = 0.5 * (guLo + guHi), my = 0.5 * (gvLo + gvHi);
        double ax = 0.5 * (guHi - guLo) + gm;
        double ay = 0.5 * (gvHi - gvLo) + gm;
        double cert = mx * mx + my * my -
                       std::fabs(mx) * ax - std::fabs(my) * ay;
        if (!(cert > 1e-16)) return REFINE;

        // Every edge must have a simple, countable root structure.
        for (int k = 0; k < 3; ++k) {
            RootInfo ri = isolateEdgeRoots(
                F, t.c[k], t.c[(k + 1) % 3]);
            if (ri.ambiguous) return REFINE;
            int nInterior = 0;
            for (double x : ri.roots)
                if (x > 1e-7 && x < 1.0 - 1e-7) ++nInterior;
            if (nInterior > 1) return REFINE;
        }
        return DO_CLIP;
    }

    // -- conforming edge split ----------------------------------------------

    bool splitEdge(EdgeKey const & key, int initTid,
                   std::string & error) {
        auto incIt = edgeInc.find(key);
        if (incIt == edgeInc.end() || incIt->second.empty()) {
            return fail("ClipRegularSurfaceMesh: internal edge topology error", error);
        }
        std::vector<int> incident = incIt->second;

        // Create the shared midpoint from the initiator's local keys.
        Tria const & init = trias[initTid];
        int ia0 = -1, ib0 = -1;
        for (int k = 0; k < 3; ++k) {
            if (init.c[k].pid == key.first) ia0 = k;
            if (init.c[k].pid == key.second) ib0 = k;
        }
        if (ia0 < 0 || ib0 < 0) {
            return fail("ClipRegularSurfaceMesh: internal edge endpoint mismatch", error);
        }
        int64_t kum2 = init.c[ia0].ku + init.c[ib0].ku;
        int64_t kvm2 = init.c[ia0].kv + init.c[ib0].kv;
        if ((kum2 & 1) || (kvm2 & 1)) {
            return fail("ClipRegularSurfaceMesh: subdivision depth exceeds parametric key resolution", error);
        }
        int midPid = internPoint(init.slot, kum2 / 2, kvm2 / 2);

        std::vector<int> newTrias;
        for (int tid : incident) {
            if (!trias[tid].alive) continue;
            Tria parent = trias[tid];
            int ia = -1, ib = -1;
            for (int k = 0; k < 3; ++k) {
                if (parent.c[k].pid == key.first) ia = k;
                if (parent.c[k].pid == key.second) ib = k;
            }
            if (ia < 0 || ib < 0) continue;
            int iz = 3 - ia - ib;

            unindexTria(tid);
            trias[tid].alive = false;

            int64_t ku2 = parent.c[ia].ku + parent.c[ib].ku;
            int64_t kv2 = parent.c[ia].kv + parent.c[ib].kv;
            if ((ku2 & 1) || (kv2 & 1)) {
                return fail("ClipRegularSurfaceMesh: subdivision depth exceeds parametric key resolution", error);
            }
            Corner mid;
            mid.pid = midPid;
            mid.ku = ku2 / 2;
            mid.kv = kv2 / 2;
            mid.u = (double) mid.ku / (double) KEY_SCALE;
            mid.v = (double) mid.kv / (double) KEY_SCALE;

            Tria a, b;
            a.slot = parent.slot;
            b.slot = parent.slot;
            a.depth = b.depth = parent.depth + 1;
            a.c = {parent.c[ia], mid, parent.c[iz]};
            b.c = {mid, parent.c[ib], parent.c[iz]};
            makeCCW(a.c);
            makeCCW(b.c);
            if (paramArea2(a) <= 0.0 || paramArea2(b) <= 0.0) {
                return fail("ClipRegularSurfaceMesh: degenerate subdivision triangle", error);
            }
            int ida = (int) trias.size();
            trias.push_back(std::move(a));
            int idb = (int) trias.size();
            trias.push_back(std::move(b));
            indexTria(ida);
            indexTria(idb);
            newTrias.push_back(ida);
            newTrias.push_back(idb);
        }
        if (newTrias.empty()) {
            return fail("ClipRegularSurfaceMesh: internal edge split had no incident triangles", error);
        }
        return true;
    }

    bool refine(std::string & error) {
        std::queue<int> todo;
        for (int i = 0; i < (int) trias.size(); ++i) todo.push(i);

        size_t const triaCap = 1500000;

        while (!todo.empty()) {
            int tid = todo.front();
            todo.pop();
            if (!trias[tid].alive) continue;

            int kind = classify(tid);
            trias[tid].kind = kind;
            if (kind != REFINE) continue;

            if (trias[tid].depth >= options.maxDepth) {
                std::ostringstream os;
                os << "ClipRegularSurfaceMesh: adaptive subdivision reached maxDepth="
                   << options.maxDepth << " without resolving the clipping domain";
                return fail(os.str(), error);
            }
            if (trias.size() > triaCap) {
                return fail("ClipRegularSurfaceMesh: tessellation resource limit exceeded", error);
            }

            // Bisect the longest parameter-space edge.
            int bestK = 0;
            double bestL2 = -1.0;
            for (int k = 0; k < 3; ++k) {
                Corner const & a = trias[tid].c[k];
                Corner const & b = trias[tid].c[(k + 1) % 3];
                double dx = b.u - a.u, dy = b.v - a.v;
                double l2 = dx * dx + dy * dy;
                if (l2 > bestL2) { bestL2 = l2; bestK = k; }
            }
            EdgeKey key = makeEdgeKey(trias[tid].c[bestK].pid,
                                      trias[tid].c[(bestK + 1) % 3].pid);
            int before = (int) trias.size();
            if (!splitEdge(key, tid, error)) return false;
            // Enqueue every triangle created by the split.
            for (int i = before; i < (int) trias.size(); ++i) todo.push(i);
        }
        return true;
    }

    // -- plane root refinement with real Bfr evaluation ----------------------

    V2 solveRoot(FaceData & F, V2 const & A, V2 const & B,
                 double ha, double hb) {
        double zEps = zeroEps;
        if (std::fabs(ha) <= zEps) return A;
        if (std::fabs(hb) <= zEps) return B;

        double lo = 0.0, hi = 1.0, fLo = ha;
        V2 pm = {0.5 * (A[0] + B[0]), 0.5 * (A[1] + B[1])};
        for (int it = 0; it < 75; ++it) {
            double m = 0.5 * (lo + hi);
            pm = lerp2(A, B, m);
            double fm = hTrue(F, pm);
            if (it >= 12 && std::fabs(fm) <= 0.4 * options.planeTolerance)
                return pm;
            if ((fm < 0.0) == (fLo < 0.0)) { lo = m; fLo = fm; }
            else hi = m;
        }
        return lerp2(A, B, 0.5 * (lo + hi));
    }

    struct PolyCorner { int pid; V2 p; };

    PolyCorner cutCorner(FaceData & F, Corner const & S, Corner const & E,
                         double hs, double he) {
        EdgeKey key = makeEdgeKey(S.pid, E.pid);
        V2 A = {S.u, S.v}, B = {E.u, E.v};
        V2 root = solveRoot(F, A, B, hs, he);

        int pid;
        auto it = edgeCut.find(key);
        if (it != edgeCut.end()) {
            pid = it->second;
        } else {
            pid = (int) positions.size();
            positions.push_back(evalBfr(F, root));
            edgeCut.emplace(key, pid);
        }
        return {pid, root};
    }

    // -- final assembly ------------------------------------------------------

    struct RawCorner { int pid; V2 p; };
    struct RawTria { int slot; std::array<RawCorner, 3> c; };

    bool assemble(ClippedMesh & result, std::string & error) {
        std::vector<RawTria> raw;

        for (int tid = 0; tid < (int) trias.size(); ++tid) {
            if (!trias[tid].alive) continue;
            int kind = classify(tid);
            Tria const & t = trias[tid];
            FaceData & F = faces[t.slot];

            if (kind == ALL_OUT) continue;

            if (kind == ALL_IN) {
                RawTria rt; rt.slot = t.slot;
                for (int k = 0; k < 3; ++k)
                    rt.c[k] = {t.c[k].pid, {t.c[k].u, t.c[k].v}};
                raw.push_back(std::move(rt));
                continue;
            }
            if (kind != DO_CLIP) {
                return fail("ClipRegularSurfaceMesh: unresolved triangle after refinement", error);
            }

            double hv[3];
            for (int k = 0; k < 3; ++k) {
                hv[k] = hAtPid(t.c[k].pid);
                if (std::fabs(hv[k]) <= zeroEps) hv[k] = 0.0;
            }

            std::vector<PolyCorner> poly;
            for (int e = 0; e < 3; ++e) {
                int ni = (e + 1) % 3;
                Corner const & S = t.c[e];
                Corner const & E = t.c[ni];
                double hs = hv[e], he = hv[ni];
                bool sIn = hs >= 0.0;
                bool eIn = he >= 0.0;
                if (eIn) {
                    if (!sIn) poly.push_back(cutCorner(F, S, E, hs, he));
                    poly.push_back({E.pid, {E.u, E.v}});
                } else if (sIn) {
                    poly.push_back(cutCorner(F, S, E, hs, he));
                }
            }

            // Drop consecutive duplicates.
            std::vector<PolyCorner> clean;
            for (auto const & pc : poly) {
                if (!clean.empty() &&
                    (clean.back().pid == pc.pid ||
                     (std::fabs(clean.back().p[0] - pc.p[0]) < 1e-15 &&
                      std::fabs(clean.back().p[1] - pc.p[1]) < 1e-15)))
                    continue;
                clean.push_back(pc);
            }
            if (clean.size() >= 2 &&
                clean.front().pid == clean.back().pid)
                clean.pop_back();

            for (int i = 1; i + 1 < (int) clean.size(); ++i) {
                std::array<RawCorner, 3> rc = {{
                    {clean[0].pid, clean[0].p},
                    {clean[i].pid, clean[i].p},
                    {clean[i + 1].pid, clean[i + 1].p}}};
                double a2 =
                    (rc[1].p[0] - rc[0].p[0]) * (rc[2].p[1] - rc[0].p[1]) -
                    (rc[2].p[0] - rc[0].p[0]) * (rc[1].p[1] - rc[0].p[1]);
                if (std::fabs(a2) <= 1e-24) continue;
                if (a2 < 0.0) std::swap(rc[1], rc[2]);
                RawTria rt; rt.slot = t.slot; rt.c = rc;
                raw.push_back(std::move(rt));
            }
        }

        if (raw.empty()) {
            // Legitimate empty (fully removed) mesh.
            result = ClippedMesh();
            return true;
        }

        // Verify plane-intersection vertices meet planeTolerance.
        for (auto const & kv : edgeCut) {
            double dist = std::fabs(hAtPid(kv.second));
            double allowed = options.planeTolerance +
                             1e-12 * std::max(1.0, coordScale);
            if (dist > allowed) {
                std::ostringstream os;
                os << "ClipRegularSurfaceMesh: plane intersection distance "
                   << dist << " exceeds planeTolerance "
                   << options.planeTolerance;
                return fail(os.str(), error);
            }
        }

        // Compact to referenced positions only.
        std::vector<int> remap(positions.size(), -1);
        std::vector<V3> compactPos;
        for (RawTria const & rt : raw) {
            for (int k = 0; k < 3; ++k) {
                int old = rt.c[k].pid;
                if (old < 0 || old >= (int) positions.size()) {
                    return fail("ClipRegularSurfaceMesh: internal position index error", error);
                }
                if (remap[old] < 0) {
                    remap[old] = (int) compactPos.size();
                    compactPos.push_back(positions[old]);
                }
            }
        }

        ClippedMesh out;
        out.positions.resize(compactPos.size());
        for (size_t i = 0; i < compactPos.size(); ++i)
            out.positions[i] = {compactPos[i][0], compactPos[i][1],
                                compactPos[i][2]};

        out.triangles.reserve(raw.size());
        out.sourceFaces.reserve(raw.size());
        out.corners.reserve(3 * raw.size());

        for (RawTria const & rt : raw) {
            std::array<unsigned, 3> tri;
            for (int k = 0; k < 3; ++k)
                tri[k] = (unsigned) remap[rt.c[k].pid];
            out.triangles.push_back(tri);
            out.sourceFaces.push_back(faces[rt.slot].faceId);

            for (int k = 0; k < 3; ++k) {
                V2 p = rt.c[k].p;
                p[0] = std::min(1.0, std::max(0.0, p[0]));
                p[1] = std::min(1.0, std::max(0.0, p[1]));

                FaceData & F = faces[rt.slot];
                V3 du = {0, 0, 0}, dv = {0, 0, 0};
                evalBfr(F, p, &du, &dv);
                V3 nrm = vcross(du, dv);
                double nl = vnorm(nrm);
                if (!std::isfinite(nl) || nl < 1e-30) {
                    return fail("ClipRegularSurfaceMesh: degenerate analytic normal", error);
                }
                nrm = vmul(nrm, 1.0 / nl);

                V2 uv = {0.0, 0.0};
                if (F.hasUV) {
                    double uvp[2] = {p[0], p[1]};
                    V3 dummyP;
                    (void) dummyP;
                    double tex[2] = {0, 0};
                    F.uvSurf.Evaluate(uvp, F.uvPatchPoints.data(), 2, tex);
                    uv = {tex[0], tex[1]};
                }

                ClipCorner cc;
                cc.parameter = {p[0], p[1]};
                cc.uv = {uv[0], uv[1]};
                cc.normal = {nrm[0], nrm[1], nrm[2]};
                out.corners.push_back(cc);
            }
        }

        if (out.corners.size() != 3u * out.triangles.size() ||
            out.sourceFaces.size() != out.triangles.size()) {
            return fail("ClipRegularSurfaceMesh: internal output size mismatch", error);
        }

        result = std::move(out);
        return true;
    }

    // -- upfront degeneracy rejection ---------------------------------------

    bool rejectDegenerateSurfaces(std::string & error) {
        double copEps = 1e-10 * std::max(1.0, coordScale);
        double cornerEps = 1e-8;

        for (FaceData const & F : faces) {
            // Coarse vertices on the plane.
            for (int k = 0; k < 4; ++k) {
                double uv[2];
                F.surf.GetParameterization().GetVertexCoord(k, uv);
                V3 P = evalBezierNet(F.bez, uv[0], uv[1]);
                if (std::fabs(hAt(P)) <= cornerEps) {
                    std::ostringstream os;
                    os << "ClipRegularSurfaceMesh: clipping plane passes through coarse vertex "
                       << "of face " << F.faceId;
                    return fail(os.str(), error);
                }
            }

            // Whole patch coplanar.
            double maxH = 0.0;
            for (V3 const & b : F.bez) maxH = std::max(maxH, std::fabs(hAt(b)));
            if (maxH <= copEps) {
                std::ostringstream os;
                os << "ClipRegularSurfaceMesh: selected face " << F.faceId
                   << " is entirely coplanar with the clipping plane";
                return fail(os.str(), error);
            }

            // Whole coarse edge coplanar: its four boundary Bezier controls.
            for (int side = 0; side < 4; ++side) {
                int iv0 = 0, iu0 = 0;
                if (side == 0) { iv0 = 0; }
                else if (side == 1) { iu0 = 3; }
                else if (side == 2) { iv0 = 3; }
                else { iu0 = 0; }
                double eMax = 0.0;
                for (int q = 0; q < 4; ++q) {
                    int iu = (side == 0 || side == 2) ? q : iu0;
                    int iv = (side == 1 || side == 3) ? q : iv0;
                    eMax = std::max(eMax, std::fabs(hAt(F.bez[iu * 4 + iv])));
                }
                if (eMax <= copEps) {
                    std::ostringstream os;
                    os << "ClipRegularSurfaceMesh: coarse edge " << F.fe[side]
                       << " of face " << F.faceId
                       << " is entirely coplanar with the clipping plane";
                    return fail(os.str(), error);
                }
            }
        }
        return true;
    }

    bool Build(ClippedMesh & result, std::string & error) {
        coordScale = 1.0;
        for (double x : meshPositions)
            coordScale = std::max(coordScale, std::fabs(x));
        zeroEps = std::min(1e-10, 0.25 * options.planeTolerance);

        if (!validateTopology(error)) return false;

        factory.reset(new Bfr::RefinerSurfaceFactory<>(topology));

        faces.resize(faceIds.size());
        for (int s = 0; s < (int) faceIds.size(); ++s) {
            if (!initFace(s, error)) return false;
        }

        buildCanonicalRecords();

        // Canonical records must cover every coarse component used.
        for (FaceData const & F : faces) {
            for (int k = 0; k < 4; ++k) {
                if (!vertCanon.count(F.fv[k]) || !edgeCanon.count(F.fe[k])) {
                    return fail("ClipRegularSurfaceMesh: internal canonical topology error", error);
                }
            }
        }

        if (!rejectDegenerateSurfaces(error)) return false;

        buildInitialMesh();
        if (!refine(error)) return false;
        if (!assemble(result, error)) return false;
        return true;
    }
};

// ---------------------------------------------------------------------------
// Public pimpl forwarding
// ---------------------------------------------------------------------------

TrimTessellation::TrimTessellation(
        Far::TopologyRefiner const & topology,
        std::vector<double> const & positions,
        std::vector<double> const & uv,
        std::vector<int> const & selectedFaces,
        std::array<double, 3> const & planeNormal,
        double planeDistance,
        ClipOptions const & options)
    : _impl(new Impl(topology, positions, uv, selectedFaces,
                     planeNormal, planeDistance, options)) {}

TrimTessellation::~TrimTessellation() { delete _impl; }

bool
TrimTessellation::Build(ClippedMesh & result, std::string & error) {
    return _impl->Build(result, error);
}

} // end namespace clipdetail
} // end namespace Bfr
} // end namespace OPENSUBDIV_VERSION
} // end namespace OpenSubdiv
