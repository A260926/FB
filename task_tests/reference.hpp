#pragma once
// Independent exact reference for the two-dimensional tesselator.
//
// Nothing in this file uses libtess2 internals (no geometry predicates, mesh,
// or sweep code are included); it starts from the original directed input
// segments and computes the filled area with rational arithmetic.
//
// Reference model
// ---------------
// Every input contour is a closed directed polyline.  For a horizontal line
// y = h, an upward crossing at x contributes +1 and a downward crossing
// contributes -1 to a point to its left; the winding rule then decides
// whether an open interval is filled.
//
// The sign convention: the listed orientation of each contour is the base
// positive winding, and TESS_REVERSE_CONTOURS negates it.  Projection tests
// pass an explicit normal; the caller then maps the rule through the
// library's projection frame.  For the normal (-1,-2,1) the projected frame
// is orientation-preserving, so the listed convention holds as-is; the
// negated normal reverses the frame and swaps POSITIVE with NEGATIVE.
// (In particular, a CCW square fills under POSITIVE with (-1,-2,1) and under
// NEGATIVE with its negative.)
//
// The y coordinates of every input endpoint and every segment/segment
// intersection are collected into an event list.  Inside one open band
// between consecutive events no two segments cross, so the winding number is
// constant on each x-interval carved out by all segment crossings.  We sample
// two distinct heights in every band.  At a sampled height we break x at the
// crossings of all reference segments and all actual triangle edges, and
// compare the exact reference coverage (1 inside, 0 outside) with the
// multiplicity of the actual triangles at an exact rational point between
// consecutive breaks.  This detects locally overlapping or missing output
// even when the total area happens to be correct.

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "registry.hpp"
#include "exact_rational.hpp"
#include "tesselator.h"

namespace tref {

struct IPoint {
  int x;
  int y;
};

struct Shape {
  std::string name;
  std::vector<std::vector<IPoint> > contours;
};

inline IPoint ip(int x, int y) {
  IPoint p;
  p.x = x;
  p.y = y;
  return p;
}

inline bool allowedEdge(const IPoint& a, const IPoint& b) {
  long long dx = (long long)b.x - a.x;
  long long dy = (long long)b.y - a.y;
  if (dx == 0 || dy == 0) return true;
  return dx == dy || dx == -dy;
}

inline std::string shapeDesc(const Shape& s) {
  std::string out = s.name;
  out += " [";
  for (size_t ci = 0; ci < s.contours.size(); ++ci) {
    if (ci) out += "; ";
    out += "c";
    out += std::to_string((int)ci);
    out += ":";
    for (size_t i = 0; i < s.contours[ci].size(); ++i) {
      if (i) out += " ";
      out += "(";
      out += std::to_string(s.contours[ci][i].x);
      out += ",";
      out += std::to_string(s.contours[ci][i].y);
      out += ")";
    }
  }
  out += "]";
  return out;
}

inline bool validateShape(const Shape& s, std::string& why) {
  if (s.contours.empty()) {
    why = "shape has no contours";
    return false;
  }
  for (size_t ci = 0; ci < s.contours.size(); ++ci) {
    const std::vector<IPoint>& c = s.contours[ci];
    if (c.size() < 3) {
      why = "contour " + std::to_string((int)ci) + " has fewer than 3 vertices";
      return false;
    }
    for (size_t i = 0; i < c.size(); ++i) {
      if (c[i].x < -32 || c[i].x > 32 || c[i].y < -32 || c[i].y > 32) {
        why = "coordinate outside the documented [-32,32] lattice in contour " +
              std::to_string((int)ci);
        return false;
      }
      const IPoint& a = c[i];
      const IPoint& b = c[(i + 1) % c.size()];
      if (a.x == b.x && a.y == b.y) {
        why = "zero-length edge in contour " + std::to_string((int)ci);
        return false;
      }
      if (!allowedEdge(a, b)) {
        why = "edge is not horizontal, vertical, or slope +-1 in contour " +
              std::to_string((int)ci);
        return false;
      }
    }
  }
  return true;
}

struct RefSeg {
  RVec2 a;
  RVec2 b;
  int sign;
};

struct RefData {
  std::vector<RefSeg> segs;
  std::vector<Rat> events;
};

struct SegHit {
  bool hit;
  RVec2 p;
};

// Exact intersection of closed segments [a,b] and [c,d].
inline SegHit segmentIntersection(const RVec2& a, const RVec2& b,
                                  const RVec2& c, const RVec2& d) {
  SegHit h;
  h.hit = false;
  RVec2 r = vSub(b, a);
  RVec2 s = vSub(d, c);
  Rat den = cross2(r, s);
  RVec2 qp = vSub(c, a);
  if (ratIsZero(den)) return h;
  Rat t = ratDiv(cross2(qp, s), den);
  Rat u = ratDiv(cross2(qp, r), den);
  if (ratLt(t, rat(0)) || ratGt(t, rat(1))) return h;
  if (ratLt(u, rat(0)) || ratGt(u, rat(1))) return h;
  h.hit = true;
  h.p = vAdd(a, vScale(r, t));
  return h;
}

inline RefData buildReference(const Shape& shape, bool reverseContours) {
  RefData ref;
  // The listed orientation of every contour is taken as the base positive
  // winding (a CCW loop has winding +1 inside).  TESS_REVERSE_CONTOURS flips
  // that sign, exactly as tessAddContour does internally.  The tests always
  // pass an explicit (0,0,1) normal for the 2D groups so the library cannot
  // silently reorient the projection frame.
  int sign = reverseContours ? -1 : 1;
  for (size_t ci = 0; ci < shape.contours.size(); ++ci) {
    const std::vector<IPoint>& c = shape.contours[ci];
    for (size_t i = 0; i < c.size(); ++i) {
      IPoint p0 = c[i];
      IPoint p1 = c[(i + 1) % c.size()];
      RefSeg s;
      s.a = vec(p0.x, p0.y);
      s.b = vec(p1.x, p1.y);
      s.sign = sign;
      ref.segs.push_back(s);

    }
  }
  for (size_t i = 0; i < ref.segs.size(); ++i) {
    ref.events.push_back(ref.segs[i].a.y);
    ref.events.push_back(ref.segs[i].b.y);
    for (size_t j = i + 1; j < ref.segs.size(); ++j) {
      SegHit h = segmentIntersection(ref.segs[i].a, ref.segs[i].b,
                                     ref.segs[j].a, ref.segs[j].b);
      if (h.hit) {
        ref.events.push_back(h.p.y);
      }
    }
  }
  ref.events = ratSortedUnique(ref.events);
  return ref;
}

// Crossing x coordinate of a non-horizontal segment at a line where the
// segment spans the line.
inline Rat crossingX(const RefSeg& s, const Rat& y) {
  Rat t = ratDiv(ratSub(y, s.a.y), ratSub(s.b.y, s.a.y));
  return ratAdd(s.a.x, ratMul(t, ratSub(s.b.x, s.a.x)));
}

inline bool spansStrictly(const RVec2& a, const RVec2& b, const Rat& y) {
  bool belowA = ratLt(a.y, y);
  bool belowB = ratLt(b.y, y);
  return belowA != belowB;
}

// Winding number at a point that does not lie on any segment.  The half-open
// convention at a vertex keeps the answer consistent when the horizontal
// probe happens to share a y coordinate with an endpoint.
inline int windingAt(const std::vector<RefSeg>& segs, const RVec2& p) {
  int n = 0;
  for (size_t i = 0; i < segs.size(); ++i) {
    const RefSeg& s = segs[i];
    // Half-open horizontal probe: [min_y, max_y).  This keeps the answer
    // consistent when the probe shares a y coordinate with a vertex.
    bool upward = !ratLt(s.b.y, p.y) && ratLt(s.a.y, p.y);
    bool downward = !ratLt(s.a.y, p.y) && ratLt(s.b.y, p.y);
    if (!upward && !downward) continue;
    Rat x = crossingX(s, p.y);
    if (ratGt(x, p.x)) n += upward ? s.sign : -s.sign;
  }
  return n;
}

inline bool windingInside(int rule, int n) {
  switch (rule) {
    case TESS_WINDING_ODD:
      return (n & 1) != 0;
    case TESS_WINDING_NONZERO:
      return n != 0;
    case TESS_WINDING_POSITIVE:
      return n > 0;
    case TESS_WINDING_NEGATIVE:
      return n < 0;
    case TESS_WINDING_ABS_GEQ_TWO:
      return n >= 2 || n <= -2;
  }
  return false;
}

inline bool insideAt(const std::vector<RefSeg>& segs, const RVec2& p, int rule) {
  return windingInside(rule, windingAt(segs, p));
}

inline bool onSegment(const RVec2& p, const RVec2& a, const RVec2& b) {
  if (!ratIsZero(cross2(vSub(b, a), vSub(p, a)))) return false;
  Rat minX = ratLt(a.x, b.x) ? a.x : b.x;
  Rat maxX = ratLt(a.x, b.x) ? b.x : a.x;
  Rat minY = ratLt(a.y, b.y) ? a.y : b.y;
  Rat maxY = ratLt(a.y, b.y) ? b.y : a.y;
  return !ratLt(p.x, minX) && !ratGt(p.x, maxX) && !ratLt(p.y, minY) &&
         !ratGt(p.y, maxY);
}

inline bool pointOnAnySegment(const std::vector<RefSeg>& segs, const RVec2& p) {
  for (size_t i = 0; i < segs.size(); ++i) {
    if (onSegment(p, segs[i].a, segs[i].b)) return true;
  }
  return false;
}

inline int orientSign(const RVec2& a, const RVec2& b, const RVec2& c) {
  Rat v = cross2(vSub(b, a), vSub(c, a));
  return ratLt(v, rat(0)) ? -1 : (ratGt(v, rat(0)) ? 1 : 0);
}

inline bool pointInTriangle(const RVec2& p, const RVec2& a, const RVec2& b,
                            const RVec2& c) {
  int o = orientSign(a, b, c);
  if (o == 0) return false;
  int s1 = orientSign(a, b, p);
  int s2 = orientSign(b, c, p);
  int s3 = orientSign(c, a, p);
  if (o > 0) return s1 > 0 && s2 > 0 && s3 > 0;
  return s1 < 0 && s2 < 0 && s3 < 0;
}

struct Tri {
  RVec2 a;
  RVec2 b;
  RVec2 c;
};

inline std::vector<Tri> fanTriangles(
    const std::vector<std::vector<RVec2> >& polygons) {
  std::vector<Tri> tris;
  for (size_t pi = 0; pi < polygons.size(); ++pi) {
    const std::vector<RVec2>& p = polygons[pi];
    if (p.size() < 3) continue;
    for (size_t j = 1; j + 1 < p.size(); ++j) {
      Tri t;
      t.a = p[0];
      t.b = p[j];
      t.c = p[j + 1];
      tris.push_back(t);
    }
  }
  return tris;
}

inline int coverageAt(const std::vector<Tri>& tris, const RVec2& p) {
  int count = 0;
  for (size_t i = 0; i < tris.size(); ++i) {
    if (pointInTriangle(p, tris[i].a, tris[i].b, tris[i].c)) ++count;
  }
  return count;
}

inline std::vector<Rat> xBreaksAt(const std::vector<RefSeg>& segs,
                                  const std::vector<Tri>& tris, const Rat& y) {
  std::vector<Rat> xs;
  for (size_t i = 0; i < segs.size(); ++i) {
    if (spansStrictly(segs[i].a, segs[i].b, y)) xs.push_back(crossingX(segs[i], y));
  }
  for (size_t i = 0; i < tris.size(); ++i) {
    RVec2 v[3] = {tris[i].a, tris[i].b, tris[i].c};
    for (int k = 0; k < 3; ++k) {
      RVec2 a = v[k];
      RVec2 b = v[(k + 1) % 3];
      if (ratEq(a.y, b.y)) continue;
      if (spansStrictly(a, b, y)) {
        Rat t = ratDiv(ratSub(y, a.y), ratSub(b.y, a.y));
        xs.push_back(ratAdd(a.x, ratMul(t, ratSub(b.x, a.x))));
      }
    }
  }
  return ratSortedUnique(xs);
}

inline std::string bandDesc(const Rat& lo, const Rat& hi, const Rat& h,
                            const Rat& x, int expected, int actual,
                            const std::string& ruleName) {
  std::string out = "rule=" + ruleName;
  out += " band=[" + ratStr(lo) + "," + ratStr(hi) + "]";
  out += " height=" + ratStr(h);
  out += " sample_x=" + ratStr(x);
  out += " expected_coverage=" + std::to_string(expected);
  out += " actual_coverage=" + std::to_string(actual);
  return out;
}

// Compares the multiplicity of the actual output polygons with the exact
// reference fill, band by band, at two distinct internal heights per band.
inline void compareCoverage(Case& c, const RefData& ref, int rule,
                            const std::vector<std::vector<RVec2> >& polygons,
                            const std::vector<RVec2>& outputVertices,
                            const std::string& shapeText,
                            const std::string& ruleName,
                            const std::string& extra) {
  std::vector<Tri> tris = fanTriangles(polygons);
  std::vector<Rat> events = ref.events;
  for (size_t i = 0; i < outputVertices.size(); ++i)
    events.push_back(outputVertices[i].y);
  events = ratSortedUnique(events);

  for (size_t bi = 0; bi + 1 < events.size(); ++bi) {
    const Rat& lo = events[bi];
    const Rat& hi = events[bi + 1];
    if (ratEq(lo, hi)) continue;
    Rat span = ratSub(hi, lo);
    Rat heights[2];
    heights[0] = ratAdd(lo, ratDiv(span, rat(4)));
    heights[1] = ratAdd(lo, ratMul(span, rat(3, 4)));
    for (int hiIndex = 0; hiIndex < 2; ++hiIndex) {
      const Rat& h = heights[hiIndex];
      std::vector<Rat> breaks = xBreaksAt(ref.segs, tris, h);
      std::vector<RVec2> samples;
      if (breaks.empty()) {
        samples.push_back(vec(rat(0), h));
      } else {
        samples.push_back(vec(ratSub(breaks.front(), rat(1)), h));
        for (size_t k = 0; k + 1 < breaks.size(); ++k) {
          Rat mid = ratDiv(ratAdd(breaks[k], breaks[k + 1]), rat(2));
          samples.push_back(vec(mid, h));
        }
        samples.push_back(vec(ratAdd(breaks.back(), rat(1)), h));
      }
      for (size_t si = 0; si < samples.size(); ++si) {
        const RVec2& p = samples[si];
        int expected = insideAt(ref.segs, p, rule) ? 1 : 0;
        int actual = coverageAt(tris, p);
        std::string msg = "region mismatch: shape=" + shapeText + " " + extra +
                          " " + bandDesc(lo, hi, h, p.x, expected, actual,
                                         ruleName);
        c.check(actual == expected, msg.c_str());
      }
    }
  }
}

// ---------------------------------------------------------------- boundary

struct DirectedEdge {
  RVec2 a;
  RVec2 b;
  int contour;
  int index;
};

inline std::vector<DirectedEdge> boundaryEdges(
    const std::vector<RVec2>& verts,
    const std::vector<std::pair<int, int> >& contours) {
  std::vector<DirectedEdge> edges;
  for (size_t ci = 0; ci < contours.size(); ++ci) {
    int base = contours[ci].first;
    int count = contours[ci].second;
    for (int i = 0; i < count; ++i) {
      DirectedEdge e;
      e.a = verts[base + i];
      e.b = verts[base + ((i + 1) % count)];
      e.contour = (int)ci;
      e.index = i;
      edges.push_back(e);
    }
  }
  return edges;
}

inline std::string edgeDesc(const DirectedEdge& e) {
  return "contour=" + std::to_string(e.contour) + " edge=" +
         std::to_string(e.index) + " " + vecStr(e.a) + "->" + vecStr(e.b);
}

// Compares the set of boundary crossings at every band height with the exact
// set of x positions where the reference fill indicator changes, and checks
// that each emitted directed boundary edge has filled area on its left and
// unfilled area on its right.
inline void compareBoundary(Case& c, const RefData& ref, int rule,
                            const std::vector<RVec2>& verts,
                            const std::vector<std::pair<int, int> >& contours,
                            const std::string& shapeText,
                            const std::string& ruleName,
                            const std::string& extra) {
  std::vector<DirectedEdge> edges = boundaryEdges(verts, contours);
  for (size_t ci = 0; ci < contours.size(); ++ci) {
    std::string msg = "boundary contour has fewer than 3 vertices: shape=" +
                      shapeText + " " + extra + " contour=" +
                      std::to_string((int)ci) + " count=" +
                      std::to_string(contours[ci].second);
    c.check(contours[ci].second >= 3, msg.c_str());
  }

  std::vector<Rat> events = ref.events;
  for (size_t i = 0; i < verts.size(); ++i) events.push_back(verts[i].y);
  events = ratSortedUnique(events);

  for (size_t bi = 0; bi + 1 < events.size(); ++bi) {
    const Rat& lo = events[bi];
    const Rat& hi = events[bi + 1];
    if (ratEq(lo, hi)) continue;
    Rat span = ratSub(hi, lo);
    Rat heights[2] = {ratAdd(lo, ratDiv(span, rat(4))),
                      ratAdd(lo, ratMul(span, rat(3, 4)))};
    for (int hIndex = 0; hIndex < 2; ++hIndex) {
      const Rat& h = heights[hIndex];
      std::vector<Rat> refCross;
      for (size_t i = 0; i < ref.segs.size(); ++i) {
        if (spansStrictly(ref.segs[i].a, ref.segs[i].b, h))
          refCross.push_back(crossingX(ref.segs[i], h));
      }
      refCross = ratSortedUnique(refCross);
      std::vector<Rat> expected;
      for (size_t k = 0; k < refCross.size(); ++k) {
        Rat leftX;
        Rat rightX;
        if (k == 0)
          leftX = ratSub(refCross[k], rat(1));
        else
          leftX = ratDiv(ratAdd(refCross[k - 1], refCross[k]), rat(2));
        if (k + 1 == refCross.size())
          rightX = ratAdd(refCross[k], rat(1));
        else
          rightX = ratDiv(ratAdd(refCross[k], refCross[k + 1]), rat(2));
        bool leftInside = insideAt(ref.segs, vec(leftX, h), rule);
        bool rightInside = insideAt(ref.segs, vec(rightX, h), rule);
        if (leftInside != rightInside) expected.push_back(refCross[k]);
      }

      std::vector<Rat> actual;
      for (size_t i = 0; i < edges.size(); ++i) {
        const DirectedEdge& e = edges[i];
        if (ratEq(e.a.y, e.b.y)) continue;
        if (spansStrictly(e.a, e.b, h)) {
          Rat t = ratDiv(ratSub(h, e.a.y), ratSub(e.b.y, e.a.y));
          actual.push_back(ratAdd(e.a.x, ratMul(t, ratSub(e.b.x, e.a.x))));
        }
      }
      actual = ratSortedUnique(actual);
      std::vector<Rat> allX = expected;
      for (size_t i = 0; i < actual.size(); ++i) allX.push_back(actual[i]);
      allX = ratSortedUnique(allX);
      for (size_t xi = 0; xi < allX.size(); ++xi) {
        int ec = 0, ac = 0;
        for (size_t i = 0; i < expected.size(); ++i)
          if (ratEq(expected[i], allX[xi])) ++ec;
        for (size_t i = 0; i < actual.size(); ++i)
          if (ratEq(actual[i], allX[xi])) ++ac;
        std::string msg = "boundary interval mismatch: shape=" + shapeText +
                          " " + extra + " rule=" + ruleName +
                          " band=[" + ratStr(lo) + "," + ratStr(hi) + "]" +
                          " height=" + ratStr(h) + " x=" + ratStr(allX[xi]) +
                          " expected_boundary_crossings=" + std::to_string(ec) +
                          " actual_boundary_crossings=" + std::to_string(ac);
        c.check(ec == ac, msg.c_str());
      }
      if (allX.empty()) {
        std::string msg = "boundary interval check: shape=" + shapeText + " " +
                          extra + " rule=" + ruleName +
                          " band=[" + ratStr(lo) + "," + ratStr(hi) + "]" +
                          " height=" + ratStr(h) +
                          " expected_boundary_crossings=0 actual_boundary_crossings=0";
        c.check(true, msg.c_str());
      }
    }
  }

  for (size_t ei = 0; ei < edges.size(); ++ei) {
    const DirectedEdge& e = edges[ei];
    std::string zeroMsg = "zero-length directed boundary edge: shape=" +
                          shapeText + " " + extra + " " + edgeDesc(e);
    bool nonZero = !ratEq(e.a.x, e.b.x) || !ratEq(e.a.y, e.b.y);
    c.check(nonZero, zeroMsg.c_str());
    if (!nonZero) continue;

    Rat dx = ratSub(e.b.x, e.a.x);
    Rat dy = ratSub(e.b.y, e.a.y);
    RVec2 midpoint = ratMid(e.a, e.b);
    RVec2 normal = vec(ratNeg(dy), dx);
    bool found = false;
    RVec2 left;
    RVec2 right;
    for (int attempt = 1; attempt <= 20; ++attempt) {
      // Use a power-of-two perpendicular probe offset and shrink it until
      // both probes are clear of every reference segment.  A boundary edge
      // can have neighbours arbitrarily close in principle, but the input
      // lattice bounds the separation, so a fixed small grid is enough.
      i128 step = ((i128)1) << (2 * attempt);
      Rat delta = ratNorm(1, step);
      left = vAdd(midpoint, vScale(normal, delta));
      right = vSub(midpoint, vScale(normal, delta));
      if (!pointOnAnySegment(ref.segs, left) &&
          !pointOnAnySegment(ref.segs, right)) {
        found = true;
        break;
      }
    }
    std::string probeMsg = "cannot place a side probe beside boundary edge: shape=" +
                           shapeText + " " + extra + " " + edgeDesc(e);
    c.check(found, probeMsg.c_str());
    if (!found) continue;

    bool leftInside = insideAt(ref.segs, left, rule);
    bool rightInside = insideAt(ref.segs, right, rule);
    std::string dirMsg = "directed boundary edge has wrong fill sides: shape=" +
                         shapeText + " " + extra + " rule=" + ruleName + " " +
                         edgeDesc(e) + " left_coverage=" +
                         std::to_string(leftInside ? 1 : 0) +
                         " right_coverage=" +
                         std::to_string(rightInside ? 1 : 0) +
                         " (expected left=1 right=0)";
    c.check(leftInside && !rightInside, dirMsg.c_str());
  }
}

}  // namespace tref
