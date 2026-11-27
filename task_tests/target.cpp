// Independent geometric tests for libtess2's two-dimensional fill.
//
// The tests here do not inspect library internals.  They start from small
// integer lattice contours, build an exact rational reference, call the public
// API, and compare the filled region, connectivity, boundary and projection
// behaviour against that reference.
//
// Everything is deterministic.  kFixedSeed below is a constant identifier for
// the fixed corpus; no random input is generated.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include "reference.hpp"

namespace {

const char kFixedSeed[] = "seed=0xC0FFEE (fixed deterministic corpus)";

const float kNormalZ[3] = {0.f, 0.f, 1.f};

// The complete set of winding rules exercised by every content group.
const int kRules[5] = {
    TESS_WINDING_ODD, TESS_WINDING_NONZERO, TESS_WINDING_POSITIVE,
    TESS_WINDING_NEGATIVE, TESS_WINDING_ABS_GEQ_TWO};

const char* ruleName(int rule) {
  switch (rule) {
    case TESS_WINDING_ODD:
      return "ODD";
    case TESS_WINDING_NONZERO:
      return "NONZERO";
    case TESS_WINDING_POSITIVE:
      return "POSITIVE";
    case TESS_WINDING_NEGATIVE:
      return "NEGATIVE";
    case TESS_WINDING_ABS_GEQ_TWO:
      return "ABS_GEQ_TWO";
  }
  return "UNKNOWN";
}

int flippedRule(int rule) {
  if (rule == TESS_WINDING_POSITIVE) return TESS_WINDING_NEGATIVE;
  if (rule == TESS_WINDING_NEGATIVE) return TESS_WINDING_POSITIVE;
  return rule;
}

tref::IPoint P(int x, int y) { return tref::ip(x, y); }

std::vector<tref::IPoint> ring(std::vector<tref::IPoint> v) { return v; }

// Crossed rectangles: winding 0 outside, 1 on each arm, 2 in the centre and
// -1 in the separate clockwise square.  One figure thus contains 0, 1, 2 and
// negative winding regions as required.
tref::Shape shapeCrossedWinding() {
  tref::Shape s;
  s.name = "crossed_winding";
  s.contours.push_back(ring({P(-12, -3), P(12, -3), P(12, 3), P(-12, 3)}));
  s.contours.push_back(ring({P(-3, -12), P(3, -12), P(3, 12), P(-3, 12)}));
  s.contours.push_back(ring({P(6, -8), P(10, -8), P(10, -12), P(6, -12)}));
  return s;
}

// A self-intersecting bowtie.  The two lobes have opposite winding.
tref::Shape shapeBowtie() {
  tref::Shape s;
  s.name = "self_intersecting_bowtie";
  s.contours.push_back(ring({P(0, 0), P(8, 8), P(8, 0), P(0, 8)}));
  s.contours.push_back(ring({P(12, 0), P(12, 6), P(18, 0), P(18, 6)}));
  return s;
}

// Positive and negative repetitions of the same outline.  Winding is 2 in
// the duplicated part, 1 in the positive hole and -1 in the negative square.
tref::Shape shapeRepeated() {
  tref::Shape s;
  s.name = "positive_negative_repeated";
  s.contours.push_back(ring({P(0, 0), P(8, 0), P(8, 8), P(0, 8)}));
  s.contours.push_back(ring({P(0, 0), P(8, 0), P(8, 8), P(0, 8)}));
  s.contours.push_back(ring({P(2, 2), P(2, 6), P(6, 6), P(6, 2)}));
  s.contours.push_back(ring({P(10, 0), P(10, 6), P(16, 6), P(16, 0)}));
  return s;
}

// Partially shared edges: A and C share x=0 completely; B shares the lower
// half of that edge and lies inside A.
tref::Shape shapeSharedEdges() {
  tref::Shape s;
  s.name = "partial_shared_edges";
  s.contours.push_back(ring({P(-8, 0), P(0, 0), P(0, 8), P(-8, 8)}));
  s.contours.push_back(ring({P(-4, 0), P(0, 0), P(0, 4), P(-4, 4)}));
  s.contours.push_back(ring({P(0, 0), P(8, 0), P(8, 8), P(0, 8)}));
  return s;
}

// A square with a clockwise square hole plus a disjoint positive square.
tref::Shape shapeHoleDisjoint() {
  tref::Shape s;
  s.name = "hole_and_disjoint";
  s.contours.push_back(ring({P(0, 0), P(12, 0), P(12, 12), P(0, 12)}));
  s.contours.push_back(ring({P(4, 4), P(4, 8), P(8, 8), P(8, 4)}));
  s.contours.push_back(ring({P(14, 0), P(20, 0), P(20, 6), P(14, 6)}));
  return s;
}

std::vector<tref::Shape> allShapes() {
  std::vector<tref::Shape> shapes;
  shapes.push_back(shapeCrossedWinding());
  shapes.push_back(shapeBowtie());
  shapes.push_back(shapeRepeated());
  shapes.push_back(shapeSharedEdges());
  shapes.push_back(shapeHoleDisjoint());
  return shapes;
}

// -------------------------------------------------------- tessellation I/O

struct XForm {
  int scaleNum;
  int scaleDen;
  int tx;
  int ty;
  int startRotation;   // rotate the starting vertex of every contour
  bool swapOrder;      // feed the contours in reverse input order
  bool embed3D;        // map (x,y) to (x,y,x+2y)
};

XForm identityForm() {
  XForm f;
  f.scaleNum = 1;
  f.scaleDen = 1;
  f.tx = 0;
  f.ty = 0;
  f.startRotation = 0;
  f.swapOrder = false;
  f.embed3D = false;
  return f;
}

double formScale(const XForm& f) {
  return (double)f.scaleNum / (double)f.scaleDen;
}

struct Snapshot {
  int vertexCount;
  int elementCount;
  int vertexSize;
  int elementType;
  int polySize;
  std::vector<float> verts;
  std::vector<int> elems;
};

// Coordinates are always generated with integer numerator / integer
// denominator scaling and integer translation, so the transformed input is
// itself a small rational lattice and z = x + 2y holds exactly by
// construction.
void addShape(TESStesselator* tess, const tref::Shape& shape, const XForm& f,
              bool reverseContours) {
  tessSetOption(tess, TESS_REVERSE_CONTOURS, reverseContours ? 1 : 0);
  std::vector<int> order;
  for (size_t i = 0; i < shape.contours.size(); ++i) order.push_back((int)i);
  if (f.swapOrder) {
    order.clear();
    for (size_t i = shape.contours.size(); i-- > 0;) order.push_back((int)i);
  }
  for (size_t oi = 0; oi < order.size(); ++oi) {
    const std::vector<tref::IPoint>& c = shape.contours[order[oi]];
    size_t n = c.size();
    size_t rot = (size_t)(f.startRotation % (int)n);
    std::vector<float> coords;
    if (f.embed3D) {
      for (size_t k = 0; k < n; ++k) {
        const tref::IPoint& p = c[(k + rot) % n];
        double x = (double)p.x * f.scaleNum / f.scaleDen + f.tx;
        double y = (double)p.y * f.scaleNum / f.scaleDen + f.ty;
        coords.push_back((float)x);
        coords.push_back((float)y);
        coords.push_back((float)(x + 2.0 * y));
      }
      tessAddContour(tess, 3, coords.data(), 3 * sizeof(float), (int)n);
    } else {
      for (size_t k = 0; k < n; ++k) {
        const tref::IPoint& p = c[(k + rot) % n];
        double x = (double)p.x * f.scaleNum / f.scaleDen + f.tx;
        double y = (double)p.y * f.scaleNum / f.scaleDen + f.ty;
        coords.push_back((float)x);
        coords.push_back((float)y);
      }
      tessAddContour(tess, 2, coords.data(), 2 * sizeof(float), (int)n);
    }
  }
}

bool tessellate(TESStesselator* tess, int rule, int elementType, int polySize,
                int vertexSize, const float* normal) {
  return tessTesselate(tess, rule, elementType, polySize, vertexSize, normal) != 0;
}

Snapshot takeSnapshot(TESStesselator* tess, int vertexSize, int elementType,
                      int polySize) {
  Snapshot snap;
  snap.vertexCount = tessGetVertexCount(tess);
  snap.elementCount = tessGetElementCount(tess);
  snap.vertexSize = vertexSize;
  snap.elementType = elementType;
  snap.polySize = polySize;
  const float* v = tessGetVertices(tess);
  const int* e = tessGetElements(tess);
  if (v && snap.vertexCount > 0) {
    snap.verts.assign(v, v + (size_t)snap.vertexCount * vertexSize);
  }
  if (e && snap.elementCount > 0) {
    size_t per = (elementType == TESS_CONNECTED_POLYGONS)
                     ? (size_t)polySize * 2
                     : (size_t)((elementType == TESS_BOUNDARY_CONTOURS) ? 2 : polySize);
    snap.elems.assign(e, e + (size_t)snap.elementCount * per);
  }
  return snap;
}

// ------------------------------------------------------ lattice quantisation

// Convert one output coordinate back to the original lattice.  Only points
// within 1e-4 of a half-integer are accepted; no arbitrary snapping happens.
bool quantise(double normalised, tref::Rat& out, double& distance,
              long long& nearestTwice) {
  double twice = normalised * 2.0;
  double rounded = std::floor(twice + 0.5);
  if (!(twice == twice) || rounded > 9.0e15 || rounded < -9.0e15) return false;
  long long nearest = (long long)rounded;
  distance = std::fabs(twice - (double)nearest) * 0.5;
  if (!(distance <= 1.0e-4)) return false;
  out = tref::rat(nearest, 2);
  nearestTwice = nearest;
  return true;
}

struct MappedVertex {
  tref::RVec2 p;
  bool ok;
};

// Maps an output vertex (2 or 3 components) back to the reference plane.
// For 3D output it also verifies the point lies on z = x + 2y.
MappedVertex mapOutputVertex(const Snapshot& snap, int index, const XForm& f,
                             double& maxDistance) {
  MappedVertex mv;
  mv.ok = true;
  const float* v = snap.verts.data() + (size_t)index * snap.vertexSize;
  double X = (double)v[0];
  double Y = (double)v[1];
  if (snap.vertexSize >= 3) {
    double Z = (double)v[2];
    if (std::fabs(Z - (X + 2.0 * Y)) > 1.0e-4) mv.ok = false;
  }
  double scale = formScale(f);
  double nx = (X - f.tx) / scale;
  double ny = (Y - f.ty) / scale;
  tref::Rat qx, qy;
  double dx = 0.0, dy = 0.0;
  long long rx = 0, ry = 0;
  bool xok = quantise(nx, qx, dx, rx);
  bool yok = quantise(ny, qy, dy, ry);
  if (!xok || !yok) mv.ok = false;
  if (dx > maxDistance) maxDistance = dx;
  if (dy > maxDistance) maxDistance = dy;
  mv.p = tref::vec(qx, qy);
  return mv;
}

// Maps the whole output vertex array and asserts every vertex is on the
// lattice and (for 3D) on the projection plane.
std::vector<tref::RVec2> mapAllVertices(Case& c, const Snapshot& snap,
                                        const XForm& f, const std::string& what) {
  std::vector<tref::RVec2> out;
  double maxDistance = 0.0;
  for (int i = 0; i < snap.vertexCount; ++i) {
    MappedVertex mv = mapOutputVertex(snap, i, f, maxDistance);
    std::string msg = "output vertex is not on the allowed half-integer lattice: " +
                      what + " vertex=" + std::to_string(i) + " " +
                      kFixedSeed;
    c.check(mv.ok, msg.c_str());
    out.push_back(mv.p);
  }
  std::string distMsg =
      "output vertex lattice distance exceeds 1e-4: " + what +
      " max_distance=" + std::to_string(maxDistance) + " " + kFixedSeed;
  c.check(maxDistance <= 1.0e-4, distMsg.c_str());
  return out;
}

std::vector<std::vector<tref::RVec2> > polygonsFromSnapshot(
    const Snapshot& snap, const std::vector<tref::RVec2>& verts,
    bool connected) {
  std::vector<std::vector<tref::RVec2> > polys;
  int per = snap.polySize * (connected ? 2 : 1);
  for (int i = 0; i < snap.elementCount; ++i) {
    std::vector<tref::RVec2> poly;
    for (int j = 0; j < snap.polySize; ++j) {
      int idx = snap.elems[(size_t)i * per + j];
      if (idx == TESS_UNDEF) break;
      poly.push_back(verts[idx]);
    }
    polys.push_back(poly);
  }
  return polys;
}

// ------------------------------------------------------ boundary helpers

std::vector<std::pair<int, int> > contoursFromSnapshot(const Snapshot& snap) {
  std::vector<std::pair<int, int> > contours;
  for (int i = 0; i < snap.elementCount; ++i) {
    contours.push_back(std::make_pair(snap.elems[(size_t)i * 2],
                                      snap.elems[(size_t)i * 2 + 1]));
  }
  return contours;
}

// A small deterministic lattice example used in failure messages.
std::string smallExample() {
  return "small_example=unit_square_with_hole "
         "outer=(0,0)(4,0)(4,4)(0,4) hole=(1,1)(1,3)(3,3)(3,1)";
}

// --------------------------------------------------------------- region fn

void checkRegion(Case& c, const tref::Shape& shape, bool reverse, int rule,
                 const Snapshot& snap, const XForm& f,
                 const std::vector<std::vector<tref::RVec2> >& polys,
                 const std::string& extra) {
  tref::RefData ref = tref::buildReference(shape, reverse);
  std::vector<tref::RVec2> mappedVerts = mapAllVertices(c, snap, f, shape.name);
  std::string context = "shape=" + tref::shapeDesc(shape) + " rule=" +
                       ruleName(rule) + " reverse=" +
                       (reverse ? "1" : "0") + " " + extra + " " + smallExample();
  std::vector<std::vector<tref::RVec2> > mappedPolys;
  for (size_t i = 0; i < polys.size(); ++i) {
    std::vector<tref::RVec2> p = polys[i];
    mappedPolys.push_back(p);
  }
  tref::compareCoverage(c, ref, rule, mappedPolys, mappedVerts, context,
                        ruleName(rule), extra);
}

}  // namespace

// ================================================================ winding

namespace winding_group {

void run(Case& c) {
  std::vector<tref::Shape> shapes = allShapes();

  // The fixed corpus itself must obey the documented input constraints:
  // small integer lattice, |coordinate| <= 32, and edges that are
  // horizontal, vertical, or slope +-1.
  for (size_t si = 0; si < shapes.size(); ++si) {
    std::string why;
    std::string msg = "test corpus violates the documented input constraints: " +
                      shapes[si].name + " reason=" + why + " " + kFixedSeed;
    bool valid = tref::validateShape(shapes[si], why);
    c.check(valid, msg.c_str());
  }

  // One figure must genuinely contain winding regions 0, 1, 2 and negative.
  // Check that the fixed reference really produces all of them.
  {
    const tref::Shape& s = shapes[0];
    tref::RefData ref = tref::buildReference(s, false);
    int w00 = tref::windingAt(ref.segs, tref::vec(0, 0));
    int warm = tref::windingAt(ref.segs, tref::vec(6, 0));
    int warm2 = tref::windingAt(ref.segs, tref::vec(0, 8));
    int wneg = tref::windingAt(ref.segs, tref::vec(8, -8));
    int wout = tref::windingAt(ref.segs, tref::vec(20, 0));
    std::string msg = "reference corpus does not contain winding 0/1/2/negative: " +
                      s.name + " w(0,0)=" + std::to_string(w00) +
                      " w(6,0)=" + std::to_string(warm) +
                      " w(0,8)=" + std::to_string(warm2) +
                      " w(8,-8)=" + std::to_string(wneg) +
                      " w(20,0)=" + std::to_string(wout) + " " + kFixedSeed;
    // Without TESS_REVERSE_CONTOURS the listed CCW rectangles contribute
    // winding +1: 2 in the overlap, 1 on each arm, -1 in the clockwise
    // square and 0 outside.
    c.check(w00 == 2 && warm == 1 && warm2 == 1 && wneg == -1 && wout == 0,
            msg.c_str());
  }

  for (size_t si = 0; si < shapes.size(); ++si) {
    const tref::Shape& shape = shapes[si];
    for (int ri = 0; ri < 5; ++ri) {
      int rule = kRules[ri];
      for (int rev = 0; rev <= 1; ++rev) {
        XForm f = identityForm();
        TESStesselator* tess = tessNewTess(0);
        c.check(tess != 0, "tessNewTess returned null");
        addShape(tess, shape, f, rev != 0);
        bool ok = tessellate(tess, rule, TESS_POLYGONS, 3, 2, kNormalZ);
        std::string failMsg = "winding tessellation failed: shape=" + shape.name +
                              " rule=" + std::string(ruleName(rule)) +
                              " reverse=" + std::to_string(rev) + " " + kFixedSeed;
        c.check(ok, failMsg.c_str());
        if (ok) {
          Snapshot snap = takeSnapshot(tess, 2, TESS_POLYGONS, 3);
          std::vector<tref::RVec2> verts =
              mapAllVertices(c, snap, f, shape.name);
          std::vector<std::vector<tref::RVec2> > polys =
              polygonsFromSnapshot(snap, verts, false);
          checkRegion(c, shape, rev != 0, rule, snap, f, polys,
                      "output=POLYGONS polySize=3");
        }
        tessDeleteTess(tess);
      }
    }
  }
}

}  // namespace winding_group

// ============================================================= connectivity

namespace connectivity_group {

bool samePoint(const tref::RVec2& a, const tref::RVec2& b) {
  return tref::ratEq(a.x, b.x) && tref::ratEq(a.y, b.y);
}

int findReversedEdge(const std::vector<tref::RVec2>& poly,
                     const tref::RVec2& from, const tref::RVec2& to) {
  int n = (int)poly.size();
  for (int j = 0; j < n; ++j) {
    if (samePoint(poly[j], to) && samePoint(poly[(j + 1) % n], from)) return j;
  }
  return -1;
}

// Side probes for an output boundary edge.  Polygons are emitted with positive
// (CCW) area, so filled area must be on the left of every boundary edge.
void checkUndefSide(Case& c, const tref::RefData& ref, int rule,
                    const tref::DirectedEdge& e, const std::string& context) {
  tref::RVec2 midpoint = tref::ratMid(e.a, e.b);
  tref::RVec2 d = tref::vSub(e.b, e.a);
  tref::RVec2 normal = tref::vec(tref::ratNeg(d.y), d.x);
  bool found = false;
  tref::RVec2 left, right;
  for (int attempt = 1; attempt <= 24; ++attempt) {
    tref::Rat delta = tref::ratNorm(1, ((tref::i128)1) << (2 * attempt));
    left = tref::vAdd(midpoint, tref::vScale(normal, delta));
    right = tref::vSub(midpoint, tref::vScale(normal, delta));
    if (!tref::pointOnAnySegment(ref.segs, left) &&
        !tref::pointOnAnySegment(ref.segs, right)) {
      found = true;
      break;
    }
  }
  std::string probeMsg =
      "could not place probes beside boundary edge: " + context + " " +
      tref::edgeDesc(e);
  c.check(found, probeMsg.c_str());
  if (!found) return;
  bool li = tref::insideAt(ref.segs, left, rule);
  bool ri = tref::insideAt(ref.segs, right, rule);
  std::string sideMsg = "TESS_UNDEF neighbour is not a genuine fill boundary: " +
                        context + " rule=" + ruleName(rule) + " " +
                        tref::edgeDesc(e) + " left_coverage=" +
                        std::to_string(li ? 1 : 0) + " right_coverage=" +
                        std::to_string(ri ? 1 : 0) +
                        " expected left=1 right=0 (the edge lies on the "
                        "reference boundary only if the sides differ)";
  c.check(li && !ri, sideMsg.c_str());
}

void runOne(Case& c, const tref::Shape& shape, int rule, bool reverse, int polySize,
            const XForm& f, const std::string& extra) {
  tref::RefData ref = tref::buildReference(shape, reverse);
  TESStesselator* tess = tessNewTess(0);
  c.check(tess != 0, "tessNewTess returned null");
  addShape(tess, shape, f, reverse);
  bool ok = tessellate(tess, rule, TESS_CONNECTED_POLYGONS, polySize, 2, kNormalZ);
  std::string failMsg = "connected-polygon tessellation failed: shape=" +
                        shape.name + " rule=" + std::string(ruleName(rule)) +
                        " polySize=" + std::to_string(polySize) +
                        " reverse=" + std::to_string(reverse ? 1 : 0) + " " +
                        extra + " " + kFixedSeed;
  c.check(ok, failMsg.c_str());
  if (!ok) {
    tessDeleteTess(tess);
    return;
  }
  Snapshot snap = takeSnapshot(tess, 2, TESS_CONNECTED_POLYGONS, polySize);
  std::vector<tref::RVec2> verts = mapAllVertices(c, snap, f, shape.name);
  int per = polySize * 2;

  struct PolyInfo {
    std::vector<tref::RVec2> v;
    std::vector<int> nei;
  };
  std::vector<PolyInfo> polys;
  for (int i = 0; i < snap.elementCount; ++i) {
    PolyInfo pi;
    int faceVerts = 0;
    bool undefSeen = false;
    for (int j = 0; j < polySize; ++j) {
      int idx = snap.elems[(size_t)i * per + j];
      if (idx == TESS_UNDEF) {
        undefSeen = true;
        continue;
      }
      std::string vmsg = "invalid vertex index in polygon: shape=" + shape.name +
                        " rule=" + std::string(ruleName(rule)) +
                        " polySize=" + std::to_string(polySize) +
                        " polygon=" + std::to_string(i) + " slot=" +
                        std::to_string(j) + " index=" + std::to_string(idx) +
                        " vertexCount=" + std::to_string(snap.vertexCount) +
                        " " + extra + " " + kFixedSeed;
      c.check(!undefSeen, ("vertex index after TESS_UNDEF slot: " + vmsg).c_str());
      c.check(idx >= 0 && idx < snap.vertexCount, vmsg.c_str());
      if (idx < 0 || idx >= snap.vertexCount) continue;
      pi.v.push_back(verts[idx]);
      ++faceVerts;
    }
    // Unused vertex slots must be TESS_UNDEF (already enforced by contiguous
    // parsing above, but state it explicitly).
    for (int j = faceVerts; j < polySize; ++j) {
      std::string msg = "unused vertex slot is not TESS_UNDEF: shape=" +
                        shape.name + " polygon=" + std::to_string(i) + " slot=" +
                        std::to_string(j) + " " + extra;
      c.check(snap.elems[(size_t)i * per + j] == TESS_UNDEF, msg.c_str());
    }
    for (int j = 0; j < polySize; ++j) {
      int nb = snap.elems[(size_t)i * per + polySize + j];
      if (j >= faceVerts) {
        std::string msg = "unused neighbour slot is not TESS_UNDEF: shape=" +
                          shape.name + " polygon=" + std::to_string(i) +
                          " slot=" + std::to_string(j) + " " + extra;
        c.check(nb == TESS_UNDEF, msg.c_str());
        pi.nei.push_back(TESS_UNDEF);
        continue;
      }
      std::string nmsg = "invalid neighbour index: shape=" + shape.name +
                         " polygon=" + std::to_string(i) + " slot=" +
                         std::to_string(j) + " neighbour=" +
                         std::to_string(nb) + " elementCount=" +
                         std::to_string(snap.elementCount) + " " + extra;
      c.check(nb == TESS_UNDEF || (nb >= 0 && nb < snap.elementCount),
              nmsg.c_str());
      pi.nei.push_back(nb);
    }
    polys.push_back(pi);
  }

  for (int i = 0; i < (int)polys.size(); ++i) {
    std::vector<tref::RVec2>& v = polys[i].v;
    std::string pctx = "shape=" + shape.name + " rule=" +
                       std::string(ruleName(rule)) + " polySize=" +
                       std::to_string(polySize) + " polygon=" +
                       std::to_string(i) + " " + extra;
    c.check(v.size() >= 3, ("polygon has fewer than 3 vertices: " + pctx).c_str());
    if (v.size() < 3) continue;

    // Positive area.
    tref::Rat area = tref::rat(0);
    for (size_t j = 0; j < v.size(); ++j)
      area = tref::ratAdd(area, tref::cross2(v[j], v[(j + 1) % v.size()]));
    std::string areaMsg = "polygon area is not positive: " + pctx +
                          " area2=" + tref::ratStr(area);
    c.check(!tref::ratIsZero(area) && tref::ratGt(area, tref::rat(0)),
            areaMsg.c_str());

    // Convexity (allow collinear points).
    bool convex = true;
    int n = (int)v.size();
    for (int j = 0; j < n; ++j) {
      tref::Rat cr = tref::cross2(tref::vSub(v[(j + 1) % n], v[j]),
                                  tref::vSub(v[(j + 2) % n], v[(j + 1) % n]));
      if (tref::ratLt(cr, tref::rat(0))) convex = false;
    }
    c.check(convex, ("polygon is not convex: " + pctx).c_str());

    // Neighbour relations, checked against the reversed geometric edge.
    for (int j = 0; j < n; ++j) {
      int nb = polys[i].nei[j];
      tref::RVec2 from = v[j];
      tref::RVec2 to = v[(j + 1) % n];
      tref::DirectedEdge de;
      de.a = from;
      de.b = to;
      de.contour = i;
      de.index = j;
      if (nb == TESS_UNDEF) {
        // No polygon may own the reversed edge; otherwise the edge is
        // internal and UNDEF would be wrong.
        bool ownedElsewhere = false;
        int owner = -1;
        for (int q = 0; q < (int)polys.size() && !ownedElsewhere; ++q) {
          if (q == i) continue;
          int jj = findReversedEdge(polys[q].v, from, to);
          if (jj >= 0) {
            ownedElsewhere = true;
            owner = q;
          }
        }
        std::string msg =
            "TESS_UNDEF neighbour on an edge that another polygon shares: " + pctx +
            " edge=" + std::to_string(j) + " other_polygon=" +
            std::to_string(owner);
        c.check(!ownedElsewhere, msg.c_str());
        // The edge must be a real boundary of the reference fill.
        checkUndefSide(c, ref, rule, de, pctx);
      } else {
        std::string bmsg =
            "neighbour index out of range in edge relation: " + pctx +
            " edge=" + std::to_string(j) + " neighbour=" + std::to_string(nb);
        c.check(nb >= 0 && nb < (int)polys.size(), bmsg.c_str());
        if (nb < 0 || nb >= (int)polys.size()) continue;
        int jj = findReversedEdge(polys[nb].v, from, to);
        std::string msg = "neighbour does not share the same reversed geometric edge: " +
                          pctx + " edge=" + std::to_string(j) + " neighbour=" +
                          std::to_string(nb);
        c.check(jj >= 0, msg.c_str());
        if (jj < 0) continue;
        std::string backMsg =
            "neighbour relation is not two-way: " + pctx + " edge=" +
            std::to_string(j) + " neighbour=" + std::to_string(nb) +
            " neighbour_edge=" + std::to_string(jj) + " neighbour_neighbour=" +
            std::to_string(polys[nb].nei[jj]);
        c.check(polys[nb].nei[jj] == i, backMsg.c_str());
      }
    }
  }

  // The polygons are themselves split into triangles by their own vertices
  // and take part in the exact region comparison.
  std::vector<std::vector<tref::RVec2> > polysForRegion;
  for (int i = 0; i < (int)polys.size(); ++i) polysForRegion.push_back(polys[i].v);
  std::string context = "shape=" + tref::shapeDesc(shape) + " rule=" +
                       ruleName(rule) + " reverse=" +
                       (reverse ? "1" : "0") + " output=CONNECTED_POLYGONS "
                       "polySize=" + std::to_string(polySize) + " " + extra + " " +
                       smallExample();
  tref::compareCoverage(c, ref, rule, polysForRegion, verts, context,
                        ruleName(rule), extra);

  tessDeleteTess(tess);
}

void run(Case& c) {
  std::vector<tref::Shape> shapes = allShapes();
  for (size_t si = 0; si < shapes.size(); ++si) {
    for (int ri = 0; ri < 5; ++ri) {
      int rule = kRules[ri];
      for (int rev = 0; rev <= 0; ++rev) {  // reverse variant covered in winding
        for (int ps = 0; ps < 2; ++ps) {
          int polySize = ps == 0 ? 3 : 6;
          XForm f = identityForm();
          runOne(c, shapes[si], rule, rev != 0, polySize, f,
                 "output=CONNECTED_POLYGONS");
        }
      }
    }
  }
  // One reverse-contours combination for connectivity as well.
  {
    const tref::Shape& shape = shapes[2];
    int rule = TESS_WINDING_NONZERO;
    XForm f = identityForm();
    runOne(c, shape, rule, true, 6, f, "reverse_combination");
    runOne(c, shape, rule, true, 3, f, "reverse_combination");
  }
}

}  // namespace connectivity_group

// ================================================================= boundary

namespace boundary_group {

void runOne(Case& c, const tref::Shape& shape, int rule, bool reverse,
            const XForm& f, const std::string& extra) {
  tref::RefData ref = tref::buildReference(shape, reverse);
  TESStesselator* tess = tessNewTess(0);
  c.check(tess != 0, "tessNewTess returned null");
  addShape(tess, shape, f, reverse);
  bool ok = tessellate(tess, rule, TESS_BOUNDARY_CONTOURS, 0, 2, kNormalZ);
  std::string failMsg = "boundary-contour tessellation failed: shape=" +
                        shape.name + " rule=" + std::string(ruleName(rule)) +
                        " reverse=" + std::to_string(reverse ? 1 : 0) + " " +
                        extra + " " + kFixedSeed;
  c.check(ok, failMsg.c_str());
  if (!ok) {
    tessDeleteTess(tess);
    return;
  }
  Snapshot snap = takeSnapshot(tess, 2, TESS_BOUNDARY_CONTOURS, 0);
  std::vector<tref::RVec2> verts = mapAllVertices(c, snap, f, shape.name);
  std::vector<std::pair<int, int> > contours = contoursFromSnapshot(snap);
  std::string context = "shape=" + tref::shapeDesc(shape) + " rule=" +
                       ruleName(rule) + " reverse=" +
                       (reverse ? "1" : "0") + " output=BOUNDARY_CONTOURS " +
                       extra + " " + smallExample();
  tref::compareBoundary(c, ref, rule, verts, contours, context, ruleName(rule),
                        extra);
  tessDeleteTess(tess);
}

void run(Case& c) {
  std::vector<tref::Shape> shapes = allShapes();
  for (size_t si = 0; si < shapes.size(); ++si) {
    for (int ri = 0; ri < 5; ++ri) {
      int rule = kRules[ri];
      XForm f = identityForm();
      runOne(c, shapes[si], rule, false, f, "");
    }
  }
  // Reverse-contours combinations.
  for (size_t si = 0; si < shapes.size(); ++si) {
    for (int ri = 0; ri < 5; ++ri) {
      int rule = kRules[ri];
      XForm f = identityForm();
      runOne(c, shapes[si], rule, true, f, "reverse_combination");
    }
  }
}

}  // namespace boundary_group

// =============================================================== projection

namespace projection_group {

const float kNormalA[3] = {-1.f, -2.f, 1.f};   // z = x + 2y preserved
const float kNormalB[3] = {1.f, 2.f, -1.f};    // reversed normal

void runOne(Case& c, const tref::Shape& shape, int rule, const XForm& f,
            const float* normal, bool reversedNormal,
            const std::string& extra, int vertexSize) {
  bool reverseContours = false;
  int effectiveRule = reversedNormal ? flippedRule(rule) : rule;
  tref::RefData ref = tref::buildReference(shape, reverseContours);

  TESStesselator* tess = tessNewTess(0);
  c.check(tess != 0, "tessNewTess returned null");
  addShape(tess, shape, f, reverseContours);
  bool ok = tessellate(tess, rule, TESS_POLYGONS, 3, vertexSize, normal);
  std::string failMsg = "projection tessellation failed: shape=" + shape.name +
                        " rule=" + std::string(ruleName(rule)) +
                        " normal=" + (reversedNormal ? "(-1,-2,1) reversed" : "(-1,-2,1)") +
                        " vertexSize=" + std::to_string(vertexSize) + " " + extra +
                        " " + kFixedSeed;
  c.check(ok, failMsg.c_str());
  if (!ok) {
    tessDeleteTess(tess);
    return;
  }
  Snapshot snap = takeSnapshot(tess, vertexSize, TESS_POLYGONS, 3);
  std::vector<tref::RVec2> verts = mapAllVertices(c, snap, f, shape.name);
  std::vector<std::vector<tref::RVec2> > polys =
      polygonsFromSnapshot(snap, verts, false);

  // Verify every returned 3D point really lies on the requested plane.
  // Both (-1,-2,1) and its negative describe the same geometric plane
  // x + 2y - z = 0; the input was built with z = x + 2y, so any correct
  // projection must return points on that plane.  A float computation after
  // scaling/translation can lose a few ulps, so allow a 1e-4 absolute gap
  // (the same tolerance used for lattice quantisation below).
  for (int i = 0; i < snap.vertexCount; ++i) {
    const float* v = snap.verts.data() + (size_t)i * snap.vertexSize;
    if (snap.vertexSize < 3) continue;
    double planeGap = std::fabs((double)v[2] - ((double)v[0] + 2.0 * (double)v[1]));
    std::string msg = "returned 3D vertex is not on the requested plane: shape=" +
                      shape.name + " vertex=" + std::to_string(i) +
                      " normal=" + (reversedNormal ? "reversed" : "base") + " " +
                      extra + " gap=" + std::to_string(planeGap);
    c.check(planeGap <= 1.0e-4, msg.c_str());
  }

  std::string context = "shape=" + tref::shapeDesc(shape) + " rule=" +
                       ruleName(rule) + " effective_rule=" +
                       ruleName(effectiveRule) + " normal=" +
                       (reversedNormal ? "reversed" : "base") + " " + extra +
                       " " + smallExample();
  tref::compareCoverage(c, ref, effectiveRule, polys, verts, context,
                        ruleName(effectiveRule), extra);
  tessDeleteTess(tess);
}

void runBasicPlane(Case& c) {
  std::vector<tref::Shape> shapes = allShapes();
  for (size_t si = 0; si < shapes.size(); ++si) {
    for (int ri = 0; ri < 5; ++ri) {
      int rule = kRules[ri];
      XForm f = identityForm();
      f.embed3D = true;
      runOne(c, shapes[si], rule, f, kNormalA, false, "base_normal", 3);
      runOne(c, shapes[si], rule, f, kNormalB, true, "reversed_normal", 3);
    }
  }
}

void runTransforms(Case& c) {
  // Re-run with 1/8 and 8 scaling, integer translations, contour start
  // rotations and input order swaps.  Start with one winding rule and cover
  // the rest on the smallest shape.
  struct Variant {
    int scaleNum;
    int scaleDen;
    int tx;
    int ty;
    int rot;
    bool swap;
    const char* name;
  };
  const Variant variants[] = {
      {1, 8, 5, -3, 0, false, "scale_1_8_translate"},
      {8, 1, -7, 4, 0, false, "scale_8_translate"},
      {1, 1, 3, 2, 1, false, "start_rotation"},
      {1, 1, 0, 0, 0, true, "input_order_swap"},
      {8, 1, -7, 4, 2, true, "scale_8_rotation_swap"},
      {1, 8, 5, -3, 1, true, "scale_1_8_rotation_swap"}};
  std::vector<tref::Shape> shapes;
  shapes.push_back(shapeCrossedWinding());
  shapes.push_back(shapeRepeated());
  for (size_t si = 0; si < shapes.size(); ++si) {
    for (size_t vi = 0; vi < sizeof(variants) / sizeof(variants[0]); ++vi) {
      for (int ni = 0; ni < 2; ++ni) {
        for (int ri = 0; ri < 5; ++ri) {
          int rule = kRules[ri];
          XForm f = identityForm();
          f.scaleNum = variants[vi].scaleNum;
          f.scaleDen = variants[vi].scaleDen;
          f.tx = variants[vi].tx;
          f.ty = variants[vi].ty;
          f.startRotation = variants[vi].rot;
          f.swapOrder = variants[vi].swap;
          f.embed3D = true;
          runOne(c, shapes[si], rule, f,
                 ni == 0 ? kNormalA : kNormalB, ni != 0,
                 std::string("variant=") + variants[vi].name, 3);
        }
      }
    }
  }
}

// Object reuse: tessellate one output mode, keep a private copy of the arrays,
// then rebuild the contours on the same object and request a different output
// mode.  The first copy must remain independently correct.
void runReuse(Case& c) {
  const tref::Shape& shape = shapeCrossedWinding();
  int rule = TESS_WINDING_NONZERO;
  XForm f = identityForm();
  TESStesselator* tess = tessNewTess(0);
  c.check(tess != 0, "tessNewTess returned null");

  addShape(tess, shape, f, false);
  bool ok1 = tessellate(tess, rule, TESS_CONNECTED_POLYGONS, 3, 2, kNormalZ);
  c.check(ok1, "reuse: first tessellation failed");
  Snapshot first = takeSnapshot(tess, 2, TESS_CONNECTED_POLYGONS, 3);
  c.check(first.elementCount > 0 && first.vertexCount > 0,
          "reuse: first tessellation produced no data");

  // Rebuild contours on the same object and switch output mode.
  addShape(tess, shape, f, false);
  bool ok2 = tessellate(tess, rule, TESS_BOUNDARY_CONTOURS, 0, 2, kNormalZ);
  c.check(ok2, "reuse: second tessellation failed");
  Snapshot second = takeSnapshot(tess, 2, TESS_BOUNDARY_CONTOURS, 0);

  // Validate both private copies against the exact reference.  The second
  // call invalidated the pointers from the first call, but not the copies.
  std::vector<tref::RVec2> firstVerts =
      mapAllVertices(c, first, f, "reuse_first");
  std::vector<tref::RVec2> secondVerts =
      mapAllVertices(c, second, f, "reuse_second");

  std::vector<std::vector<tref::RVec2> > firstPolys =
      polygonsFromSnapshot(first, firstVerts, true);
  tref::RefData ref = tref::buildReference(shape, false);
  std::string ctxFirst = "shape=" + tref::shapeDesc(shape) +
                         " reuse_first=CONNECTED_POLYGONS rule=" +
                         ruleName(rule) + " " + smallExample();
  tref::compareCoverage(c, ref, rule, firstPolys, firstVerts, ctxFirst,
                        ruleName(rule), "reuse_first_copy");

  std::vector<std::pair<int, int> > contours = contoursFromSnapshot(second);
  std::string ctxSecond = "shape=" + tref::shapeDesc(shape) +
                          " reuse_second=BOUNDARY_CONTOURS rule=" +
                          ruleName(rule) + " " + smallExample();
  tref::compareBoundary(c, ref, rule, secondVerts, contours, ctxSecond,
                        ruleName(rule), "reuse_second_copy");

  // The snapshot copies must remain valid after the second call: re-check the
  // stored counts and re-derive geometry from the copy alone.
  c.check(first.vertexCount > 0 && first.elementCount > 0 &&
              (int)first.verts.size() == first.vertexCount * 2 &&
              (int)first.elems.size() == first.elementCount * 6,
          "reuse: first snapshot copy was not preserved");
  tessDeleteTess(tess);
}

void run(Case& c) {
  runBasicPlane(c);
  runTransforms(c);
  runReuse(c);
}

}  // namespace projection_group

// ============================================================= registration

void register_task_tests(Registry& r) {
  r.add("winding", "winding_rules_and_regions", [](Case& c) {
    winding_group::run(c);
  });
  r.add("connectivity", "connected_polygons_3_6", [](Case& c) {
    connectivity_group::run(c);
  });
  r.add("boundary", "boundary_contour_coverage", [](Case& c) {
    boundary_group::run(c);
  });
  r.add("projection", "projection_normals_and_transforms", [](Case& c) {
    projection_group::run(c);
  });
}
