// Geometry-quality task tests for libtess2.
//
// Four groups are registered: winding, connectivity, boundary, projection.
// The expected geometry always comes from the independent exact reference in
// geo_ref.hpp (rational event bands built from the original directed
// segments), never from libtess2 internals or another triangulation library.
// This file deliberately has no main(): task_support/main.cpp drives it.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "tesselator.h"
#include "registry.hpp"
#include "geo_ref.hpp"
#include "geo_scenes.hpp"

namespace lt2ref {
namespace {

using ::Case;
using ::Registry;

// ---------------------------------------------------------------------------
// small helpers
// ---------------------------------------------------------------------------

std::string edgeText(const FPoint& a, const FPoint& b) {
    return pointStr(a) + "->" + pointStr(b);
}

// Inverse-transform an output coordinate back onto the original grid.
double normalizeCoord(double v, double scale, double shift) {
    return v / scale - shift;
}

bool snapTransformed(const std::vector<float>& xy, int vertexSize, double scale,
                     double tx, double ty, std::vector<FPoint>& pts,
                     std::string& err) {
    int n = (int)(xy.size()) / vertexSize;
    pts.clear();
    pts.reserve(n);
    for (int i = 0; i < n; ++i) {
        double x = normalizeCoord(xy[(size_t)i * vertexSize], scale, tx);
        double y = normalizeCoord(xy[(size_t)i * vertexSize + 1], scale, ty);
        Frac sx, sy;
        if (!snapHalf(x, sx) || !snapHalf(y, sy)) {
            std::ostringstream os;
            os << "transformed vertex " << i << " maps to (" << x << "," << y
               << "), off the half-integer grid";
            err = os.str();
            return false;
        }
        pts.push_back({sx, sy});
    }
    return true;
}

void checkCoverageOrThrow(Case& c, const Scene& sc, int rule, bool reverseAll,
                          const std::vector<RefTri>& tris) {
    BandMismatch bad;
    std::string err;
    bool ok = compareCoverage(sc, rule, tris, bad, err);
    if (!ok) {
        std::fprintf(stderr, "SEED=%u %s\n", (unsigned)kSeed,
                     mismatchText(sc, rule, reverseAll, bad).c_str());
    }
    c.check(ok, "event-band coverage multiplicity (1 inside, 0 outside)");
}

// ---------------------------------------------------------------------------
// winding group
// ---------------------------------------------------------------------------

void windingCase(Case& c, const Scene& sc) {
    for (int rule = 0; rule < 5; ++rule) {
        for (int rev = 0; rev < 2; ++rev) {
            Tesselation r;
            std::string err;
            bool ok = run2D(sc, rule, TESS_POLYGONS, 3, rev != 0, r, err);
            c.check(ok, ("tessellate: " + sceneText(sc) + " " +
                         ruleName(rule) + " rev=" + std::to_string(rev)).c_str());
            if (!ok) continue;
            std::vector<FPoint> pts;
            c.check(snapPoints(r.verts, 2, pts, err),
                    ("output snaps to reference grid: " + err).c_str());
            std::vector<RefTri> tris;
            c.check(polygonsToTris(r, pts, tris, err),
                    ("output polygons fan to triangles: " + err).c_str());
            Scene rs = sceneForRun(sc, rev != 0);
            // The tesselator must emit triangles exactly when the reference
            // selects a region.
            if (anyReferenceFill(rs, rule))
                c.check(!tris.empty(), "filled scene must produce triangles");
            else
                c.check(tris.empty(), "empty selection must produce no triangles");
            checkCoverageOrThrow(c, rs, rule, rev != 0, tris);
        }
    }
}

// The winding-mix scene really must contain winding 0, 1, 2 and a negative
// region; otherwise the rule matrix above would not be discriminating.
void windingInvariantsCase(Case& c) {
    Scene sc = makeWindingMix();
    struct Probe { FPoint p; int want; const char* tag; };
    // Probe points sit strictly inside event bands (never on an edge or a
    // vertex height), so the crossing-based winding is well defined.
    Probe probes[] = {
        {P(10, 5), 2, "overlap of the two CCW diamonds"},
        {P(3, 5), 1, "rim inside only the first CCW diamond"},
        {P(26, 5), -1, "inside the reversed (CW) diamond"},
        {P(24, 17), 0, "inside an opposite duplicate pair"},
        {P(31, 17), 0, "fully outside every contour"},
    };
    for (const Probe& pr : probes) {
        int got = windingAtPoint(sc, pr.p);
        c.check(got == pr.want,
                ("winding mix probe " + std::string(pr.tag) + " at " +
                 pointStr(pr.p) + " want=" + std::to_string(pr.want) +
                 " got=" + std::to_string(got)).c_str());
    }
    // Rule selection at the winding-2 overlap point.
    FPoint overlap = P(10, 5);
    c.check(fillAtPoint(sc, TESS_WINDING_ODD, overlap) == false, "ODD excludes winding 2");
    c.check(fillAtPoint(sc, TESS_WINDING_NONZERO, overlap), "NONZERO includes winding 2");
    c.check(fillAtPoint(sc, TESS_WINDING_ABS_GEQ_TWO, overlap),
            "ABS_GEQ_TWO includes winding 2");
    // The winding -1 region: selected by NEGATIVE (and NONZERO/ODD), but not
    // by POSITIVE, and not by ABS_GEQ_TWO which needs |winding| >= 2.
    FPoint neg = P(26, 5);
    c.check(fillAtPoint(sc, TESS_WINDING_NEGATIVE, neg), "NEGATIVE selects winding -1");
    c.check(!fillAtPoint(sc, TESS_WINDING_POSITIVE, neg),
            "POSITIVE rejects winding -1");
    c.check(fillAtPoint(sc, TESS_WINDING_ABS_GEQ_TWO, neg) == false,
            "ABS_GEQ_TWO rejects |winding|=1");
}

// ---------------------------------------------------------------------------
// connectivity group
// ---------------------------------------------------------------------------

struct AtomicKey {
    FPoint u, v;
    bool operator<(const AtomicKey& o) const {
        if (!(u == o.u)) return u.x == o.u.x ? u.y < o.u.y : u.x < o.u.x;
        return v.x == o.v.x ? v.y < o.v.y : v.x < o.v.x;
    }
    bool operator==(const AtomicKey& o) const { return u == o.u && v == o.v; }
};

static bool pointLess(const FPoint& a, const FPoint& b) {
    if (!(a.x == b.x)) return a.x < b.x;
    return a.y < b.y;
}

// Direction-independent boundary atom: the two endpoints are canonicalized.
static AtomicKey makeAtom(const FPoint& a, const FPoint& b) {
    return pointLess(a, b) ? AtomicKey{a, b} : AtomicKey{b, a};
}

// Split a directed half-grid-aligned piece into half-unit directed atoms.
// Boundary coverage is a geometric set, not a multiset: coincident duplicate
// contours still produce one physical boundary edge.
void appendAtoms(const FPoint& a, const FPoint& b,
                 std::set<AtomicKey>& atoms) {
    FPoint d = b - a;
    long long steps;
    Frac sx(0), sy(0);
    if (d.y == Frac(0)) {
        steps = std::llabs((d.x * Frac(2)).n);
        sx = Frac(d.x.n < 0 ? -1 : 1, 2);
    } else if (d.x == Frac(0)) {
        steps = std::llabs((d.y * Frac(2)).n);
        sy = Frac(d.y.n < 0 ? -1 : 1, 2);
    } else {
        steps = std::llabs((d.x * Frac(2)).n);
        sx = Frac(d.x.n < 0 ? -1 : 1, 2);
        sy = Frac(d.y.n < 0 ? -1 : 1, 2);
    }
    FPoint cur = a;
    for (long long i = 0; i < steps; ++i) {
        FPoint nxt = FPoint(cur.x + sx, cur.y + sy);
        atoms.insert(makeAtom(cur, nxt));
        cur = nxt;
    }
}

bool ringConvex(const std::vector<FPoint>& ring) {
    int m = (int)ring.size();
    if (m < 3) return false;
    int sign = 0;
    for (int i = 0; i < m; ++i) {
        FPoint u = ring[(i + 1) % m] - ring[i];
        FPoint v = ring[(i + 2) % m] - ring[(i + 1) % m];
        Frac cr = cross(u, v);
        int s = cr == Frac(0) ? 0 : (cr > Frac(0) ? 1 : -1);
        if (s != 0) {
            if (sign == 0) sign = s;
            else if (sign != s) return false;
        }
    }
    return ringArea2(ring) != Frac(0);
}

void connectivityRun(Case& c, const Scene& sc, int rule, bool reverseAll,
                     int polySize) {
    Scene rs = sceneForRun(sc, reverseAll);
    Tesselation r;
    std::string err;
    bool ok = run2D(sc, rule, TESS_CONNECTED_POLYGONS, polySize, reverseAll, r, err);
    c.check(ok, ("connected tessellate polySize=" + std::to_string(polySize) +
                 ": " + err).c_str());
    if (!ok) return;
    // Some rule/scene/orientation combinations legitimately select nothing.
    if (r.elementCount == 0) {
        c.check(!anyReferenceFill(rs, rule),
                "empty connected output only when the reference selects nothing");
        return;
    }

    std::vector<FPoint> pts;
    c.check(snapPoints(r.verts, 2, pts, err), err.c_str());
    int stride = polySize * 2;

    // Decode every polygon once.
    struct Poly { std::vector<int> v; };
    std::vector<Poly> polys(r.elementCount);
    for (int f = 0; f < r.elementCount; ++f) {
        const TESSindex* poly = &r.elems[(size_t)f * stride];
        for (int k = 0; k < polySize; ++k) {
            if (k < (int)polys[f].v.size()) continue;
            if (poly[k] == TESS_UNDEF) break;
            c.check(poly[k] >= 0 && poly[k] < (TESSindex)pts.size(),
                    "polygon vertex index in range");
            polys[f].v.push_back(poly[k]);
        }
        c.check(polys[f].v.size() >= 3, "polygon carries at least three vertices");
        // Unused vertex slots must be TESS_UNDEF.
        for (int k = (int)polys[f].v.size(); k < polySize; ++k)
            c.check(poly[k] == TESS_UNDEF, "unused polygon slot is TESS_UNDEF");
        // Unused neighbour slots must be TESS_UNDEF as well.
        for (int k = (int)polys[f].v.size(); k < polySize; ++k)
            c.check(poly[polySize + k] == TESS_UNDEF,
                    "unused neighbour slot is TESS_UNDEF");

        std::vector<FPoint> ring;
        for (int idx : polys[f].v) ring.push_back(pts[idx]);
        c.check(ringConvex(ring), "output polygon has positive area and is convex");
    }

    // Neighbour topology and geometric twin edges.
    for (int f = 0; f < r.elementCount; ++f) {
        const TESSindex* poly = &r.elems[(size_t)f * stride];
        int m = (int)polys[f].v.size();
        for (int k = 0; k < m; ++k) {
            FPoint a = pts[polys[f].v[k]];
            FPoint b = pts[polys[f].v[(k + 1) % m]];
            TESSindex j = poly[polySize + k];
            bool lf, rf;
            c.check(sideFill(rs, rule, a, b, lf, rf),
                    ("general-position side probes resolved for " +
                     edgeText(a, b)).c_str());
            if (j == TESS_UNDEF) {
                // Only a genuine fill/no-fill boundary may lack a neighbour.
                c.check(lf != rf,
                        ("TESS_UNDEF neighbour on an interior edge " +
                         edgeText(a, b)).c_str());
                continue;
            }
            c.check(j >= 0 && j < r.elementCount, "neighbour index in range");
            c.check(j != (TESSindex)f, "interior neighbour must not point back at itself");
            c.check(lf == rf && lf,
                    ("linked neighbour edge is interior (filled on both sides) " +
                     edgeText(a, b)).c_str());
            // The neighbour must own the exact reversed geometric edge.
            const TESSindex* np = &r.elems[(size_t)j * stride];
            int nm = 0;
            while (nm < polySize && np[nm] != TESS_UNDEF) ++nm;
            int twin = -1;
            for (int q = 0; q < nm; ++q) {
                FPoint ua = pts[np[q]];
                FPoint ub = pts[np[(q + 1) % nm]];
                if (ua == b && ub == a) { twin = q; break; }
            }
            c.check(twin >= 0, "neighbour shares the exact reversed geometric edge");
            if (twin >= 0)
                c.check(np[polySize + twin] == (TESSindex)f,
                        "shared edge neighbours point at each other");
        }
    }

    std::vector<RefTri> tris;
    c.check(polygonsToTris(r, pts, tris, err),
            ("connected polygons fan to triangles: " + err).c_str());
    checkCoverageOrThrow(c, rs, rule, reverseAll, tris);
}

void connectivityCase(Case& c, const Scene& sc) {
    // Every rule, both TESS_REVERSE_CONTOURS settings, and both polySizes.
    for (int rule = 0; rule < 5; ++rule)
        for (int rev = 0; rev < 2; ++rev)
            for (int polySize : {3, 6})
                connectivityRun(c, sc, rule, rev != 0, polySize);
}

// Named case for the winding-mix figure under the NEGATIVE rule, which
// selects the reversed diamond only; run for both reverse flags/polySizes.
void connectivityNegativeCase(Case& c) {
    Scene sc = makeWindingMix();
    for (int rev = 0; rev < 2; ++rev)
        for (int polySize : {3, 6})
            connectivityRun(c, sc, TESS_WINDING_NEGATIVE, rev != 0, polySize);
}

// ---------------------------------------------------------------------------
// boundary group
// ---------------------------------------------------------------------------

// Reference boundary as directed half-unit atoms: an original segment belongs
// to the boundary wherever the fill status differs across it.
std::set<AtomicKey> referenceBoundaryAtoms(const Scene& sc, int rule) {
    std::set<AtomicKey> atoms;
    std::vector<FSeg> segs = allSegments(sc);
    std::vector<FPoint> ev = eventPoints(sc);
    for (const FSeg& e : segs) {
        std::vector<FPoint> on;
        for (const FPoint& p : ev)
            if (pointOnSegment(p, e.a, e.b)) on.push_back(p);
        std::sort(on.begin(), on.end(), [&](const FPoint& p, const FPoint& q) {
            FPoint dp = p - e.a, dq = q - e.a;
            return dp.x == dq.x ? dp.y < dq.y : dp.x < dq.x;
        });
        on.erase(std::unique(on.begin(), on.end()), on.end());
        for (size_t i = 0; i + 1 < on.size(); ++i) {
            FPoint a = on[i], b = on[i + 1];
            if (a == b) continue;
            bool lf, rf;
            if (!sideFill(sc, rule, a, b, lf, rf)) continue;
            // Direction-independent coverage: a boundary atom exists wherever
            // the two sides differ in fill status. Ring orientation is checked
            // separately on the tesselator output.
            if (lf != rf) appendAtoms(a, b, atoms);
        }
    }
    return atoms;
}

// Print a bounded, deterministic diff between the expected and actual
// boundary atom sets so a failure names the concrete directed half-edges.
void reportAtomDiff(const Scene& sc, int rule, bool reverseAll,
                    const std::set<AtomicKey>& want,
                    const std::set<AtomicKey>& got) {
    std::fprintf(stderr,
                 "SEED=%u %s rule=%s reverseAll=%d expected-atoms=%zu actual-atoms=%zu\n",
                 (unsigned)kSeed, sc.name.c_str(), ruleName(rule),
                 reverseAll ? 1 : 0, want.size(), got.size());
    int shown = 0;
    for (const AtomicKey& k : want)
        if (!got.count(k) && shown < 8) {
            std::fprintf(stderr, "  missing directed edge %s->%s\n",
                         pointStr(k.u).c_str(), pointStr(k.v).c_str());
            ++shown;
        }
    shown = 0;
    for (const AtomicKey& k : got)
        if (!want.count(k) && shown < 8) {
            std::fprintf(stderr, "  unexpected directed edge %s->%s\n",
                         pointStr(k.u).c_str(), pointStr(k.v).c_str());
            ++shown;
        }
}

void boundaryRun(Case& c, const Scene& sc, int rule, bool reverseAll) {
    Scene rs = sceneForRun(sc, reverseAll);
    Tesselation r;
    std::string err;
    bool ok = run2D(sc, rule, TESS_BOUNDARY_CONTOURS, 3, reverseAll, r, err);
    c.check(ok, ("boundary tessellate: " + err).c_str());
    if (!ok) return;
    std::vector<FPoint> pts;
    c.check(snapPoints(r.verts, 2, pts, err), err.c_str());

    std::set<AtomicKey> want = referenceBoundaryAtoms(rs, rule);
    std::set<AtomicKey> got;
    bool anyRing = false;
    for (int f = 0; f < r.elementCount; ++f) {
        int base = r.elems[(size_t)f * 2];
        int count = r.elems[(size_t)f * 2 + 1];
        c.check(count >= 3, "boundary contour is a closed ring");
        c.check(base >= 0 && base + count <= r.vertexCount,
                "boundary contour range in range");
        anyRing = true;
        std::vector<FPoint> ring;
        int fillLeft = 0, fillRight = 0;
        for (int k = 0; k < count; ++k) {
            FPoint a = pts[base + k];
            FPoint b = pts[base + (k + 1) % count];
            ring.push_back(a);
            // Every output edge must separate a filled side from an empty one.
            bool lf, rf;
            c.check(sideFill(rs, rule, a, b, lf, rf),
                    ("general-position side probes resolved for boundary " +
                     edgeText(a, b)).c_str());
            c.check(lf != rf,
                    ("output boundary edge separates filled from unfilled: " +
                     edgeText(a, b)).c_str());
            if (lf) ++fillLeft;
            if (rf) ++fillRight;
            appendAtoms(a, b, got);
        }
        // The fill must stay on one consistent side around the whole ring.
        // (Both outer loops and hole/even-odd island rings occur; their
        // signed-area sign is not fixed, only this side consistency.)
        c.check(fillLeft == count || fillRight == count,
                "boundary ring keeps the fill on one consistent side");
        c.check(ringArea2(ring) != Frac(0), "boundary ring has nonzero area");
    }
    c.check(anyRing || want.empty(), "boundary rings exist when fill exists");
    if (got != want) reportAtomDiff(rs, rule, reverseAll, want, got);
    c.check(got == want,
            "boundary covers exactly the reference interval (collinear/contact "
            "splits allowed, no gaps or overlaps)");
}

void boundaryCase(Case& c, const Scene& sc) {
    for (int rule = 0; rule < 5; ++rule) {
        boundaryRun(c, sc, rule, false);
        boundaryRun(c, sc, rule, true);
    }
}

// ---------------------------------------------------------------------------
// projection group
// ---------------------------------------------------------------------------

struct Tesselation3D {
    int vertexCount = 0;
    int elementCount = 0;
    std::vector<float> verts;
    std::vector<TESSindex> elems;
};

bool run3D(const Scene& sc, int rule, int elementType, int polySize,
           bool reverseAll, const float normal[3], double scale, double tx,
           double ty, bool rotate, bool swapOrder, Tesselation3D& out,
           std::string& err) {
    TESStesselator* t = tessNewTess(nullptr);
    if (!t) { err = "tessNewTess failed"; return false; }
    struct Del { void operator()(TESStesselator* x) const { tessDeleteTess(x); } };
    std::unique_ptr<TESStesselator, Del> guard(t);

    std::vector<Contour> cs = sc.contours;
    if (swapOrder) std::reverse(cs.begin(), cs.end());
    for (const Contour& cc : cs) {
        Contour rc = cc;
        if (rotate && !rc.pts.empty()) {
            size_t k = 1 % rc.pts.size();
            std::rotate(rc.pts.begin(), rc.pts.begin() + k, rc.pts.end());
        }
        tessSetOption(t, TESS_REVERSE_CONTOURS,
                      contourReversed(rc, reverseAll) ? 1 : 0);
        std::vector<float> coords;
        for (const FPoint& q : rc.pts) {
            double x = (q.x.toDouble() + tx) * scale;
            double y = (q.y.toDouble() + ty) * scale;
            double z = x + 2.0 * y;
            coords.push_back((float)x);
            coords.push_back((float)y);
            coords.push_back((float)z);
        }
        tessAddContour(t, 3, coords.data(), 3 * (int)sizeof(float),
                       (int)rc.pts.size());
    }
    if (tessGetStatus(t) != TESS_STATUS_OK) { err = "3D input rejected"; return false; }
    if (!tessTesselate(t, rule, elementType, polySize, 3, normal)) {
        err = "3D tessTesselate failed";
        return false;
    }
    out.vertexCount = tessGetVertexCount(t);
    out.elementCount = tessGetElementCount(t);
    const float* v = tessGetVertices(t);
    const TESSindex* e = tessGetElements(t);
    out.verts.assign(v, v + (size_t)out.vertexCount * 3);
    int stride = elementType == TESS_CONNECTED_POLYGONS ? polySize * 2
                 : elementType == TESS_POLYGONS           ? polySize : 2;
    out.elems.assign(e, e + (size_t)out.elementCount * stride);
    return true;
}

void checkOnPlane(Case& c, const Tesselation3D& r) {
    for (int i = 0; i < r.vertexCount; ++i) {
        float x = r.verts[(size_t)i * 3];
        float y = r.verts[(size_t)i * 3 + 1];
        float z = r.verts[(size_t)i * 3 + 2];
        double resid = std::fabs((double)z - (double)x - 2.0 * (double)y);
        c.check(resid <= 2e-3, "returned 3D vertex stays on the plane z=x+2y");
    }
}

void projectionCoverage(Case& c, const Scene& sc, int rule,
                        const float normal[3], int refRule, const char* tag,
                        double scale = 1.0, double tx = 0.0, double ty = 0.0,
                        bool rotate = false, bool swapOrder = false) {
    Tesselation3D r;
    std::string err;
    bool ok = run3D(sc, rule, TESS_POLYGONS, 3, false, normal, scale, tx, ty,
                    rotate, swapOrder, r, err);
    c.check(ok, (std::string("3D run: ") + tag + ": " + err).c_str());
    if (!ok) return;
    checkOnPlane(c, r);
    std::vector<FPoint> pts;
    c.check(snapTransformed(r.verts, 3, scale, tx, ty, pts, err),
            (std::string(tag) + ": " + err).c_str());
    Tesselation flat;
    flat.elementType = TESS_POLYGONS;
    flat.polySize = 3;
    flat.elementCount = r.elementCount;
    flat.elems = r.elems;
    std::vector<RefTri> tris;
    c.check(polygonsToTris(flat, pts, tris, err), err.c_str());
    checkCoverageOrThrow(c, sc, refRule, false, tris);
}

void projectionPlaneCase(Case& c) {
    Scene sc = makeWindingMix();
    const float n[3] = {-1, -2, 1};
    const float ni[3] = {1, 2, -1};
    // Explicit normal and its reverse: positive/negative swap, everything
    // else about the geometry is unchanged.
    projectionCoverage(c, sc, TESS_WINDING_POSITIVE, n, TESS_WINDING_POSITIVE,
                       "normal n, POSITIVE");
    projectionCoverage(c, sc, TESS_WINDING_NEGATIVE, ni, TESS_WINDING_POSITIVE,
                       "normal -n, NEGATIVE (must equal n,POSITIVE)");
    projectionCoverage(c, sc, TESS_WINDING_NEGATIVE, n, TESS_WINDING_NEGATIVE,
                       "normal n, NEGATIVE");
    projectionCoverage(c, sc, TESS_WINDING_POSITIVE, ni, TESS_WINDING_NEGATIVE,
                       "normal -n, POSITIVE (must equal n,NEGATIVE)");
}

void projectionTransformsCase(Case& c) {
    ScenePack pack = allScenes();
    const float n[3] = {-1, -2, 1};
    for (const Scene& sc : pack.scenes) {
        projectionCoverage(c, sc, TESS_WINDING_POSITIVE, n,
                           TESS_WINDING_POSITIVE, "scale 1/8", 1.0 / 8.0);
        projectionCoverage(c, sc, TESS_WINDING_POSITIVE, n,
                           TESS_WINDING_POSITIVE, "scale 8", 8.0);
        projectionCoverage(c, sc, TESS_WINDING_POSITIVE, n,
                           TESS_WINDING_POSITIVE, "integer translation", 1.0,
                           5, -7);
        projectionCoverage(c, sc, TESS_WINDING_POSITIVE, n,
                           TESS_WINDING_POSITIVE, "rotated contour starts",
                           1.0, 0, 0, true, false);
        projectionCoverage(c, sc, TESS_WINDING_POSITIVE, n,
                           TESS_WINDING_POSITIVE, "swapped contour order",
                           1.0, 0, 0, false, true);
    }
}

// Reusing one tesselator object across output modes: the first run's arrays
// are copied by the test before the second run invalidates them.
void projectionReuseCase(Case& c) {
    Scene sc = makeHoleAndDisjoint();
    TESStesselator* t = tessNewTess(nullptr);
    c.check(t != nullptr, "tesselator allocated for reuse run");
    struct Del { void operator()(TESStesselator* x) const { tessDeleteTess(x); } };
    std::unique_ptr<TESStesselator, Del> guard(t);

    auto feed = [&]() {
        for (const Contour& cc : sc.contours) {
            tessSetOption(t, TESS_REVERSE_CONTOURS,
                          contourReversed(cc, false) ? 1 : 0);
            std::vector<float> coords;
            for (const FPoint& q : cc.pts) {
                coords.push_back((float)q.x.toDouble());
                coords.push_back((float)q.y.toDouble());
            }
            tessAddContour(t, 2, coords.data(), 2 * (int)sizeof(float),
                           (int)cc.pts.size());
        }
    };
    feed();
    c.check(tessTesselate(t, TESS_WINDING_POSITIVE, TESS_POLYGONS, 3, 2,
                          nullptr) != 0,
            "first run on reused object succeeds");
    // Test-owned deep copies before the object is driven again.
    int vc1 = tessGetVertexCount(t);
    int ec1 = tessGetElementCount(t);
    std::vector<float> v1(tessGetVertices(t),
                          tessGetVertices(t) + (size_t)vc1 * 2);
    std::vector<TESSindex> e1(tessGetElements(t),
                              tessGetElements(t) + (size_t)ec1 * 3);

    feed();
    c.check(tessTesselate(t, TESS_WINDING_POSITIVE, TESS_BOUNDARY_CONTOURS, 3,
                          2, nullptr) != 0,
            "second run (boundary mode) on the same object succeeds");
    int ec2 = tessGetElementCount(t);
    std::vector<TESSindex> e2(tessGetElements(t),
                              tessGetElements(t) + (size_t)ec2 * 2);
    c.check(!v1.empty() && !e1.empty() && !e2.empty(),
            "both runs produced test-owned output copies");

    std::vector<FPoint> pts;
    std::string err;
    c.check(snapPoints(v1, 2, pts, err), err.c_str());
    Tesselation flat;
    flat.elementType = TESS_POLYGONS;
    flat.polySize = 3;
    flat.elementCount = ec1;
    flat.elems = e1;
    std::vector<RefTri> tris;
    c.check(polygonsToTris(flat, pts, tris, err), err.c_str());
    checkCoverageOrThrow(c, sc, TESS_WINDING_POSITIVE, false, tris);

    // The saved boundary copy must equal the exact reference boundary.
    std::set<AtomicKey> want = referenceBoundaryAtoms(sc, TESS_WINDING_POSITIVE);
    std::set<AtomicKey> got;
    const float* v2 = tessGetVertices(t);
    for (int f = 0; f < ec2; ++f) {
        int base = e2[(size_t)f * 2];
        int count = e2[(size_t)f * 2 + 1];
        for (int k = 0; k < count; ++k) {
            Frac ax, ay, bx, by;
            c.check(snapHalf(v2[(base + k) * 2], ax) &&
                        snapHalf(v2[(base + k) * 2 + 1], ay) &&
                        snapHalf(v2[(base + (k + 1) % count) * 2], bx) &&
                        snapHalf(v2[(base + (k + 1) % count) * 2 + 1], by),
                    "boundary reuse vertices snap to grid");
            appendAtoms(FPoint(ax, ay), FPoint(bx, by), got);
        }
    }
    c.check(got == want, "reused-object boundary copy matches reference");
}

}  // namespace

void registerAll(Registry& reg) {
    ScenePack pack = allScenes();
    for (const Scene& sc : pack.scenes)
        reg.add("winding", sc.name.c_str(),
                [sc](Case& c) { windingCase(c, sc); });
    reg.add("winding", "winding_mix_invariants", windingInvariantsCase);

    for (const Scene& sc : pack.scenes)
        reg.add("connectivity", sc.name.c_str(),
                [sc](Case& c) { connectivityCase(c, sc); });
    reg.add("connectivity", "negative_rule_connected", connectivityNegativeCase);

    for (const Scene& sc : pack.scenes)
        reg.add("boundary", sc.name.c_str(),
                [sc](Case& c) { boundaryCase(c, sc); });

    reg.add("projection", "plane_and_reversed_normal", projectionPlaneCase);
    reg.add("projection", "scales_translation_rotation_order",
            projectionTransformsCase);
    reg.add("projection", "object_reuse_keeps_own_copies", projectionReuseCase);
}

}  // namespace lt2ref

void register_task_tests(Registry& reg) { lt2ref::registerAll(reg); }
