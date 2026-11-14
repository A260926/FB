// Test harness: fixed small-integer scenes, libtess2 glue, grid snapping and
// the event-band coverage comparison used by every test group.
#pragma once

#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "tesselator.h"
#include "geo_ref.hpp"

namespace lt2ref {

constexpr unsigned kSeed = 0x1172E55u;  // fixed seed: scenes are fully deterministic

struct ScenePack {
    std::vector<Scene> scenes;
};

inline Scene makeCross() {
    // A 45-degree diamond that genuinely crosses an axis-aligned rectangle:
    // its lower two edges cut through the rectangle's bottom/right and
    // bottom/left edges at four proper interior intersections.
    Scene s;
    s.name = "crossing_contours";
    s.contours.push_back({{P(8, 2), P(14, 8), P(8, 14), P(2, 8)}, +1});
    s.contours.push_back({{P(5, 4), P(11, 4), P(11, 9), P(5, 9)}, +1});
    return s;
}

inline Scene makeBowtie() {
    // One self-intersecting contour (allowed slopes only: h/v/+-1).
    Scene s;
    s.name = "self_intersecting_bowtie";
    s.contours.push_back({{P(2, 2), P(8, 8), P(2, 8), P(8, 2)}, +1});
    return s;
}

inline Scene makeDuplicates() {
    // Same-orientation duplicate (winding 2 inside) and an opposite pair that
    // cancels exactly (winding 0 inside its loop).
    Scene s;
    s.name = "duplicate_contours_both_orientations";
    s.contours.push_back({{P(0, 0), P(6, 0), P(6, 6), P(0, 6)}, +1});
    s.contours.push_back({{P(0, 0), P(6, 0), P(6, 6), P(0, 6)}, +1});
    s.contours.push_back({{P(10, 0), P(16, 0), P(16, 6), P(10, 6)}, +1});
    s.contours.push_back({{P(10, 0), P(16, 0), P(16, 6), P(10, 6)}, -1});
    return s;
}

inline Scene makeSharedEdge() {
    // Two squares sharing only part of an edge. The upper square's bottom
    // edge y=4 spans x in [0,4], the lower square's top edge spans x in
    // [2,6]; the shared segment (2,4)-(4,4) is a strict sub-edge of each.
    // The contact is laid out horizontally: the x-oriented sweep keeps the
    // output free of the zero-area contact faces that a vertical collinear
    // chain would introduce.
    Scene s;
    s.name = "partially_shared_edge";
    s.contours.push_back({{P(0, 4), P(4, 4), P(4, 8), P(0, 8)}, +1});
    s.contours.push_back({{P(2, 0), P(6, 0), P(6, 4), P(2, 4)}, +1});
    return s;
}

inline Scene makeHoleAndDisjoint() {
    // Square with a square hole plus a fully disjoint filled square.
    Scene s;
    s.name = "hole_and_disjoint_region";
    s.contours.push_back({{P(0, 0), P(8, 0), P(8, 8), P(0, 8)}, +1});
    s.contours.push_back({{P(2, 2), P(6, 2), P(6, 6), P(2, 6)}, -1});
    s.contours.push_back({{P(12, 2), P(16, 2), P(16, 6), P(12, 6)}, +1});
    return s;
}

inline Scene makeWindingMix() {
    // One figure whose regions carry winding 0, 1, 2 and a negative value.
    // The winding-2 region comes from two overlapping 45-degree diamonds
    // (proper interior crossings, no T-junction contacts, so every connected
    // output face keeps positive area).
    Scene s;
    s.name = "winding_mix_0_1_2_negative";
    s.contours.push_back({{P(6, 0), P(12, 6), P(6, 12), P(0, 6)}, +1});
    s.contours.push_back({{P(10, 0), P(16, 6), P(10, 12), P(4, 6)}, +1});
    // A reversed (CW) diamond: winding -1 inside.
    s.contours.push_back({{P(26, 2), P(30, 6), P(26, 10), P(22, 6)}, -1});
    // An opposite duplicate pair: winding 0 even inside the loop.
    s.contours.push_back({{P(20, 14), P(28, 14), P(28, 20), P(20, 20)}, +1});
    s.contours.push_back({{P(20, 14), P(28, 14), P(28, 20), P(20, 20)}, -1});
    return s;
}

inline ScenePack allScenes() {
    ScenePack p;
    p.scenes = {makeCross(), makeBowtie(), makeDuplicates(), makeSharedEdge(),
                makeHoleAndDisjoint(), makeWindingMix()};
    return p;
}

inline std::string sceneText(const Scene& s) {
    std::ostringstream os;
    os << "scene=" << s.name << " contours:";
    for (const Contour& c : s.contours) {
        os << " [" << (c.sign < 0 ? "CW" : "CCW") << "]";
        for (const FPoint& q : c.pts) os << " " << pointStr(q);
    }
    return os.str();
}

// Whether a contour is fed with TESS_REVERSE_CONTOURS for this run.
inline bool contourReversed(const Contour& c, bool reverseAll) {
    return (c.sign < 0) != reverseAll;
}

// Effective reference scene for a run. With an auto-computed normal the
// library's CheckOrientation() flips the projection whenever the sum of the
// as-fed signed contour areas is negative, so a uniformly reversed scene is
// flipped back. The exact equivalent is: as-fed sign s = c.sign (negated for
// a reverse-all run), then multiply every sign by the sign of the total
// as-fed signed area (no flip when it is zero).
inline Scene sceneForRun(const Scene& sc, bool reverseAll) {
    Scene out = sc;
    Frac total(0);
    for (Contour& c : out.contours) {
        int fed = c.sign * (reverseAll ? -1 : 1);
        total = total + ringArea2(c.pts) * Frac(fed);
    }
    int flip = total < Frac(0) ? -1 : 1;
    for (Contour& c : out.contours)
        c.sign = c.sign * (reverseAll ? -1 : 1) * flip;
    return out;
}

struct Tesselation {
    int vertexCount = 0;
    int elementCount = 0;
    std::vector<float> verts;      // test-owned copy, vertexSize floats each
    std::vector<TESSindex> elems;  // test-owned copy
    std::vector<TESSindex> vinds;
    int vertexSize = 2;
    int polySize = 3;
    int elementType = TESS_POLYGONS;
};

// Runs a 2D scene through a fresh tesselator and deep-copies every returned
// array, so later runs on a reused object cannot invalidate the data.
inline bool run2D(const Scene& sc, int rule, int elementType, int polySize,
                  bool reverseAll, Tesselation& out, std::string& err) {
    TESStesselator* t = tessNewTess(nullptr);
    if (!t) { err = "tessNewTess failed"; return false; }
    struct Del { void operator()(TESStesselator* x) const { tessDeleteTess(x); } };
    std::unique_ptr<TESStesselator, Del> guard(t);

    for (const Contour& c : sc.contours) {
        tessSetOption(t, TESS_REVERSE_CONTOURS, contourReversed(c, reverseAll) ? 1 : 0);
        std::vector<float> coords;
        for (const FPoint& q : c.pts) {
            coords.push_back((float)q.x.toDouble());
            coords.push_back((float)q.y.toDouble());
        }
        tessAddContour(t, 2, coords.data(), 2 * (int)sizeof(float),
                       (int)c.pts.size());
    }
    if (tessGetStatus(t) != TESS_STATUS_OK) { err = "input rejected"; return false; }
    int ok = tessTesselate(t, rule, elementType, polySize, 2, nullptr);
    if (!ok) { err = "tessTesselate failed"; return false; }

    out.vertexSize = 2;
    out.polySize = polySize;
    out.elementType = elementType;
    out.vertexCount = tessGetVertexCount(t);
    out.elementCount = tessGetElementCount(t);
    const float* v = tessGetVertices(t);
    const TESSindex* e = tessGetElements(t);
    const TESSindex* vi = tessGetVertexIndices(t);
    out.verts.assign(v, v + (size_t)out.vertexCount * 2);
    int stride = elementType == TESS_CONNECTED_POLYGONS ? polySize * 2
                 : elementType == TESS_POLYGONS           ? polySize
                                                          : 2;
    out.elems.assign(e, e + (size_t)out.elementCount * stride);
    if (vi) out.vinds.assign(vi, vi + out.vertexCount);
    return true;
}

// Snaps output vertices (already normalized back to original grid
// coordinates) onto the half-integer reference grid.
inline bool snapPoints(const std::vector<float>& xy, int vertexSize,
                       std::vector<FPoint>& pts, std::string& err) {
    int n = (int)(xy.size()) / vertexSize;
    pts.clear();
    pts.reserve(n);
    for (int i = 0; i < n; ++i) {
        Frac sx, sy;
        double x = xy[(size_t)i * vertexSize];
        double y = xy[(size_t)i * vertexSize + 1];
        if (!snapHalf(x, sx) || !snapHalf(y, sy)) {
            std::ostringstream os;
            os << "vertex " << i << "=(" << x << "," << y
               << ") too far from the half-integer grid";
            err = os.str();
            return false;
        }
        pts.push_back({sx, sy});
    }
    return true;
}

// Fans every output polygon out into triangles on the reference grid.
inline bool polygonsToTris(const Tesselation& r,
                           const std::vector<FPoint>& pts,
                           std::vector<RefTri>& tris, std::string& err) {
    tris.clear();
    int stride = r.elementType == TESS_CONNECTED_POLYGONS ? r.polySize * 2
                                                          : r.polySize;
    for (int f = 0; f < r.elementCount; ++f) {
        const TESSindex* poly = &r.elems[(size_t)f * stride];
        std::vector<int> idx;
        for (int k = 0; k < r.polySize; ++k) {
            if (poly[k] == TESS_UNDEF) break;
            if (poly[k] < 0 || poly[k] >= (TESSindex)pts.size()) {
                err = "polygon vertex index out of range";
                return false;
            }
            idx.push_back(poly[k]);
        }
        if (idx.size() < 3) { err = "polygon with fewer than three vertices"; return false; }
        std::vector<FPoint> ring;
        for (int id : idx) ring.push_back(pts[id]);
        // A merged convex polygon may carry collinear vertices (polySize 6);
        // contact sliver faces can be area-less and contribute nothing. The
        // event-band comparison still proves coverage, so skip area-less
        // pieces rather than treat them as geometry to cover.
        if (!(absFrac(ringArea2(ring)) > Frac(0))) continue;
        for (size_t k = 1; k + 1 < idx.size(); ++k) {
            RefTri tr{{pts[idx[0]], pts[idx[k]], pts[idx[k + 1]]}};
            if (!(triArea2(tr) > Frac(0))) continue;  // collinear fan piece
            tris.push_back(tr);
        }
    }
    return true;
}

struct BandMismatch {
    Frac y;
    Frac x;
    int expected;
    int actual;
};

// The core content assertion: for every event band (reference events plus
// the snapped output vertex heights), at two interior heights, split the line
// at every reference and triangle interval endpoint and require coverage
// multiplicity 1 on filled intervals and 0 elsewhere.
inline bool compareCoverage(const Scene& sc, int rule,
                            const std::vector<RefTri>& tris,
                            BandMismatch& bad, std::string& err) {
    std::vector<Frac> ys = eventYs(sc);
    for (const RefTri& t : tris)
        for (const FPoint& q : t.v) ys.push_back(q.y);
    std::sort(ys.begin(), ys.end());
    ys.erase(std::unique(ys.begin(), ys.end()), ys.end());
    if (ys.size() < 2) {
        // No vertical extent: the only legal result is an empty triangle set.
        if (!tris.empty()) { err = "triangles produced for a flat scene"; return false; }
        return true;
    }

    for (size_t b = 0; b + 1 < ys.size(); ++b) {
        Frac lo = ys[b], hi = ys[b + 1];
        if (!(hi > lo)) continue;
        Frac span = hi - lo;
        Frac heights[2] = {lo + span / Frac(3), lo + span * Frac(2) / Frac(3)};
        for (const Frac& h : heights) {
            std::vector<FInterval> ref = fillIntervals(sc, rule, h);
            std::vector<FInterval> out = triIntervalsAtY(tris, h);
            std::vector<Frac> xs;
            Frac guard(100);  // coordinates stay within +-32; this is far outside
            xs.push_back(Frac(-guard.n));
            for (const FInterval& iv : ref) { xs.push_back(iv.lo); xs.push_back(iv.hi); }
            for (const FInterval& iv : out) { xs.push_back(iv.lo); xs.push_back(iv.hi); }
            xs.push_back(guard);
            std::sort(xs.begin(), xs.end());
            xs.erase(std::unique(xs.begin(), xs.end()), xs.end());
            for (size_t i = 0; i + 1 < xs.size(); ++i) {
                Frac mid = (xs[i] + xs[i + 1]) / Frac(2);
                int expected = 0;
                for (const FInterval& iv : ref)
                    if (iv.lo < mid && mid < iv.hi) { expected = 1; break; }
                int actual = coverCount(out, mid);
                if (expected == 0) {
                    if (actual != 0) { bad = {h, mid, expected, actual}; return false; }
                } else if (actual != 1) {
                    bad = {h, mid, expected, actual};
                    return false;
                }
            }
        }
    }
    return true;
}

inline std::string mismatchText(const Scene& sc, int rule, bool reverseAll,
                                const BandMismatch& bad) {
    std::ostringstream os;
    os << sceneText(sc) << " rule=" << ruleName(rule)
       << " reverseAll=" << reverseAll << " band-y=" << fracStr(bad.y)
       << " at-x=" << fracStr(bad.x) << " expected-cover=" << bad.expected
       << " actual-cover=" << bad.actual << " (overlap or missing region)";
    return os.str();
}

inline bool pointOnSegment(const FPoint& p, const FPoint& a, const FPoint& b) {
    FPoint ab = b - a, ap = p - a;
    if (cross(ab, ap) != Frac(0)) return false;
    return fracMin(a.x, b.x) <= p.x && p.x <= fracMax(a.x, b.x) &&
           fracMin(a.y, b.y) <= p.y && p.y <= fracMax(a.y, b.y);
}

inline bool pointOnAnySegment(const Scene& sc, const FPoint& p) {
    for (const FSeg& e : allSegments(sc))
        if (pointOnSegment(p, e.a, e.b)) return true;
    return false;
}

// Whether the rule selects any region of the scene at all.
inline bool anyReferenceFill(const Scene& sc, int rule) {
    for (const Frac& y : eventYs(sc)) {
        Frac h = y + Frac(1, 8);
        if (!fillIntervals(sc, rule, h).empty()) return true;
    }
    return false;
}

// Half-grid unit vector along a directed edge (each component is 0 or +-1/2).
inline FPoint edgeUnit(const FPoint& a, const FPoint& b) {
    FPoint d = b - a;
    Frac hx(1, 2), hy(1, 2);
    if (d.x == Frac(0)) hx = Frac(0);
    else if (d.x < Frac(0)) hx = Frac(-1, 2);
    if (d.y == Frac(0)) hy = Frac(0);
    else if (d.y < Frac(0)) hy = Frac(-1, 2);
    return FPoint(hx, hy);
}

// General-position fill probes on the two sides of a directed edge. The
// midpoint can sit on an event height/line where strict crossing counts are
// degenerate. Candidates combine a perpendicular depth (1/16 .. 1/4) with a
// small tangential nudge (+-1/8, +-1/4, always within the edge). A candidate
// is accepted only if it lies on no segment and at no event height, so the
// crossing-based winding is well defined and the probe stays in the region
// immediately beside the edge.
inline bool sideProbes(const Scene& sc, const FPoint& a, const FPoint& b,
                       FPoint& left, FPoint& right) {
    FPoint m = (a + b) * Frac(1, 2);
    FPoint perp = leftOffset(a, b);   // axis: 1/8, diagonal: components 1/8
    FPoint tang = edgeUnit(a, b);     // half-grid unit along the edge
    std::vector<Frac> evY = eventYs(sc);
    const long long depths[] = {1, 2, 3, 4};          // perpendicular, /16
    const long long tangs[] = {1, -1, 2, -2, 0};      // tangential, /8
    auto atEventY = [&](const FPoint& p) {
        for (const Frac& y : evY) if (p.y == y) return true;
        return false;
    };
    for (long long dep : depths) {
        FPoint off = FPoint(perp.x * Frac(dep, 2), perp.y * Frac(dep, 2));
        for (long long tg : tangs) {
            FPoint sh = FPoint(tang.x * Frac(tg, 4), tang.y * Frac(tg, 4));
            FPoint l = m + off + sh;
            FPoint r = m - off + sh;
            if (!pointOnAnySegment(sc, l) && !pointOnAnySegment(sc, r) &&
                !atEventY(l) && !atEventY(r)) {
                left = l;
                right = r;
                return true;
            }
        }
    }
    return false;
}

// Fill status of both sides of a directed edge through general-position
// probes (never on a segment or an event line).
inline bool sideFill(const Scene& sc, int rule, const FPoint& a,
                     const FPoint& b, bool& leftFill, bool& rightFill) {
    FPoint l, r;
    if (!sideProbes(sc, a, b, l, r)) return false;
    leftFill = fillAtPoint(sc, rule, l);
    rightFill = fillAtPoint(sc, rule, r);
    return true;
}

}  // namespace lt2ref
