//! \file EditableMesh.cpp
//! \brief Implementation of FastBVH::EditableMesh (see
//! include/FastBVH/EditableMesh.h for the full documentation).

#include <FastBVH/EditableMesh.h>
#include <FastBVH/BuildStrategy.h>
#include <FastBVH/BuildStrategy1.h>
#include <FastBVH/Intersection.h>
#include <FastBVH/Traverser.h>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace FastBVH {

namespace {

//! The 3D vector type used throughout the implementation.
using Vec3 = Vector3<double>;

//! \name Numeric contract constants
//!@{
constexpr double kCoordBound = 1e6;  //!< Absolute coordinate bound.
constexpr double kMinEdge = 1e-5;    //!< Minimum triangle edge length.
constexpr double kMinHeight = 1e-5;  //!< Minimum triangle height.
constexpr double kEps = 1e-8;        //!< Absolute distance epsilon.
constexpr double kLenEps = 3e-8;     //!< Interval/overlap length epsilon.
//!@}

//! The internal narrow-phase result ("None" is not exposed to users).
enum class TriContact { None, Point, Segment, Area };

//! Converts the internal narrow-phase result to the public kind.
EditableMesh::ContactKind toKind(TriContact c) {
  return c == TriContact::Point ? EditableMesh::ContactKind::Point
                                : (c == TriContact::Segment ? EditableMesh::ContactKind::Segment
                                                            : EditableMesh::ContactKind::Area);
}

//! The sign of a value with the distance epsilon applied.
int sgnEps(double v) { return v > kEps ? 1 : (v < -kEps ? -1 : 0); }

//! A 2D point used for the coplanar (projected) intersection case.
struct Vec2 {
  double x, y;
};

Vec2 sub2(const Vec2& a, const Vec2& b) { return Vec2{a.x - b.x, a.y - b.y}; }
double cross2(const Vec2& a, const Vec2& b) { return a.x * b.y - a.y * b.x; }
double dot2(const Vec2& a, const Vec2& b) { return a.x * b.x + a.y * b.y; }
double len2(const Vec2& a) { return std::sqrt(dot2(a, a)); }

//! Projects a 3D point to 2D by dropping one axis.
Vec2 dropAxis(const Vec3& p, int axis) {
  switch (axis) {
    case 0: return Vec2{p.y, p.z};
    case 1: return Vec2{p.x, p.z};
    default: return Vec2{p.x, p.y};
  }
}

//! Appends a point to a raw point list.
void addPoint(Vec2* pts, int& n, const Vec2& p) {
  pts[n] = p;
  ++n;
}

//! \brief Intersects two 2D segments, appending 0, 1 or 2 points to the
//! output list. Exact endpoint contact and collinear overlap are handled
//! with the distance epsilon.
void segSeg2d(const Vec2& p0, const Vec2& p1, const Vec2& q0, const Vec2& q1, Vec2* pts, int& n) {
  const Vec2 e = sub2(p1, p0);
  const Vec2 f = sub2(q1, q0);
  const double le = len2(e);
  const double lf = len2(f);
  if (le < 1e-12 || lf < 1e-12) return;  // defensive; real edges project far above this

  // Signed distances of the endpoints to the opposite supporting lines.
  const double d0 = cross2(e, sub2(q0, p0)) / le;
  const double d1 = cross2(e, sub2(q1, p0)) / le;
  const double c0 = cross2(f, sub2(p0, q0)) / lf;
  const double c1 = cross2(f, sub2(p1, q0)) / lf;
  const int s0 = sgnEps(d0), s1 = sgnEps(d1);
  const int t0 = sgnEps(c0), t1 = sgnEps(c1);

  // Strictly on one side: no intersection.
  if (s0 * s1 > 0 || t0 * t1 > 0) return;

  if (s0 == 0 && s1 == 0) {
    // Collinear segments: 1D overlap in the parameter of p.
    const double il = 1.0 / (le * le);
    const double u0 = dot2(sub2(q0, p0), e) * il;
    const double u1 = dot2(sub2(q1, p0), e) * il;
    const double lo = std::max(0.0, std::min(u0, u1));
    const double hi = std::min(1.0, std::max(u0, u1));
    const double pe = kEps / le;
    if (lo > hi + pe) return;
    if (hi - lo <= pe) {
      double u = (lo + hi) * 0.5;
      u = u < 0.0 ? 0.0 : (u > 1.0 ? 1.0 : u);
      addPoint(pts, n, Vec2{p0.x + e.x * u, p0.y + e.y * u});
    } else {
      addPoint(pts, n, Vec2{p0.x + e.x * lo, p0.y + e.y * lo});
      addPoint(pts, n, Vec2{p0.x + e.x * hi, p0.y + e.y * hi});
    }
    return;
  }

  // A proper crossing: interpolate on the straddling segment.
  if (s0 * s1 < 0) {
    const double t = d0 / (d0 - d1);
    addPoint(pts, n, Vec2{q0.x + f.x * t, q0.y + f.y * t});
    return;
  }
  if (t0 * t1 < 0) {
    const double t = c0 / (c0 - c1);
    addPoint(pts, n, Vec2{p0.x + e.x * t, p0.y + e.y * t});
    return;
  }

  // Touching endpoints: collect those lying on the opposite segment.
  const double pe = kEps / le;
  const double pf = kEps / lf;
  const double il_e = 1.0 / (le * le);
  const double il_f = 1.0 / (lf * lf);
  if (s0 == 0) {
    const double u = dot2(sub2(q0, p0), e) * il_e;
    if (u >= -pe && u <= 1.0 + pe) addPoint(pts, n, q0);
  }
  if (s1 == 0) {
    const double u = dot2(sub2(q1, p0), e) * il_e;
    if (u >= -pe && u <= 1.0 + pe) addPoint(pts, n, q1);
  }
  if (t0 == 0) {
    const double u = dot2(sub2(p0, q0), f) * il_f;
    if (u >= -pf && u <= 1.0 + pf) addPoint(pts, n, p0);
  }
  if (t1 == 0) {
    const double u = dot2(sub2(p1, q0), f) * il_f;
    if (u >= -pf && u <= 1.0 + pf) addPoint(pts, n, p1);
  }
}

//! \brief Tests whether a 2D point is inside (or exactly on) a 2D triangle.
bool pointInTri2d(const Vec2& p, const Vec2 t[3]) {
  const Vec2 e0 = sub2(t[1], t[0]);
  const Vec2 e1 = sub2(t[2], t[1]);
  const Vec2 e2 = sub2(t[0], t[2]);
  const double s = cross2(e0, sub2(t[2], t[0])) >= 0.0 ? 1.0 : -1.0;
  const double d0 = s * cross2(e0, sub2(p, t[0])) / len2(e0);
  const double d1 = s * cross2(e1, sub2(p, t[1])) / len2(e1);
  const double d2 = s * cross2(e2, sub2(p, t[2])) / len2(e2);
  return d0 >= -kEps && d1 >= -kEps && d2 >= -kEps;
}

//! \brief Handles two coplanar triangles by intersecting them in 2D and
//! classifying the intersection point set.
//! \param shared The number of vertex indices the triangles share (0..2).
TriContact contactCoplanar(const Vec3 a[3], const Vec3 b[3], const Vec3& n1, int shared) {
  // Project along the dominant normal axis.
  int axis = 0;
  if (std::fabs(n1.y) > std::fabs(n1[axis])) axis = 1;
  if (std::fabs(n1.z) > std::fabs(n1[axis])) axis = 2;

  Vec2 pa[3], pb[3];
  for (int i = 0; i < 3; ++i) {
    pa[i] = dropAxis(a[i], axis);
    pb[i] = dropAxis(b[i], axis);
  }

  // The intersection polygon vertices: edge crossings plus contained vertices.
  Vec2 pts[32];
  int np = 0;
  static const int E[3][2] = {{0, 1}, {1, 2}, {2, 0}};
  for (const auto& ea : E) {
    for (const auto& eb : E) {
      segSeg2d(pa[ea[0]], pa[ea[1]], pb[eb[0]], pb[eb[1]], pts, np);
    }
  }
  for (int i = 0; i < 3; ++i) {
    if (pointInTri2d(pa[i], pb)) addPoint(pts, np, pa[i]);
  }
  for (int j = 0; j < 3; ++j) {
    if (pointInTri2d(pb[j], pa)) addPoint(pts, np, pb[j]);
  }

  // Deduplicate with the distance epsilon.
  Vec2 uniq[32];
  int m = 0;
  for (int i = 0; i < np; ++i) {
    bool dup = false;
    for (int j = 0; j < m; ++j) {
      if (len2(sub2(uniq[j], pts[i])) <= kEps) {
        dup = true;
        break;
      }
    }
    if (!dup) uniq[m++] = pts[i];
  }

  if (m == 0) return TriContact::None;

  TriContact kind;
  if (m == 1) {
    kind = TriContact::Point;
  } else if (m == 2) {
    kind = TriContact::Segment;
  } else {
    // Collinearity test: farthest pair, then max distance to that line.
    double best = -1.0;
    int bi = 0, bj = 1;
    for (int i = 0; i < m; ++i) {
      for (int j = i + 1; j < m; ++j) {
        const double d = len2(sub2(uniq[i], uniq[j]));
        if (d > best) {
          best = d;
          bi = i;
          bj = j;
        }
      }
    }
    if (best <= kEps) {
      kind = TriContact::Point;
    } else {
      const Vec2 dir = sub2(uniq[bj], uniq[bi]);
      const double ld = len2(dir);
      double maxd = 0.0;
      for (int i = 0; i < m; ++i) {
        maxd = std::max(maxd, std::fabs(cross2(dir, sub2(uniq[i], uniq[bi]))) / ld);
      }
      kind = maxd <= kLenEps ? TriContact::Segment : TriContact::Area;
    }
  }

  // Apply the shared-simplex exemption.
  if (shared == 0) return kind;
  if (shared == 1) return kind == TriContact::Point ? TriContact::None : kind;
  return kind == TriContact::Area ? TriContact::Area : TriContact::None;  // shared == 2
}

//! \brief Computes the intersection points of a triangle with a plane,
//! given the signed distances of its vertices to the plane.
//! \return The number of output points (1 or 2 for non-coplanar input).
int clipTriToPlane(const Vec3 v[3], const double d[3], Vec3 out[6]) {
  int n = 0;
  static const int E[3][2] = {{0, 1}, {1, 2}, {2, 0}};
  for (const auto& e : E) {
    const int i = e[0], j = e[1];
    const int si = sgnEps(d[i]), sj = sgnEps(d[j]);
    if (si == 0) out[n++] = v[i];
    if (si * sj < 0) {
      const double t = d[i] / (d[i] - d[j]);
      out[n++] = v[i] + (v[j] - v[i]) * t;
    }
  }
  // Deduplicate (a vertex exactly on the plane is emitted by two edges).
  int m = 0;
  for (int i = 0; i < n; ++i) {
    bool dup = false;
    for (int j = 0; j < m; ++j) {
      if (length(out[i] - out[j]) <= kEps) {
        dup = true;
        break;
      }
    }
    if (!dup) out[m++] = out[i];
  }
  return m;
}

//! \brief Handles two non-coplanar triangles: intersects each triangle
//! with the other triangle's plane and overlaps the two chords on the
//! plane-intersection line.
//! \param shared The number of vertex indices the triangles share (0..2).
TriContact contactNonCoplanar(const Vec3 a[3], const Vec3 b[3], const double da[3], const double db[3],
                              const Vec3& n1, const Vec3& n2, int shared) {
  // One-sided tests: all vertices strictly on one side of the other plane.
  const double mnA = std::min(da[0], std::min(da[1], da[2]));
  const double mxA = std::max(da[0], std::max(da[1], da[2]));
  const double mnB = std::min(db[0], std::min(db[1], db[2]));
  const double mxB = std::max(db[0], std::max(db[1], db[2]));
  if (mnA > kEps || mxA < -kEps) return TriContact::None;
  if (mnB > kEps || mxB < -kEps) return TriContact::None;

  // With a shared edge and non-coplanar planes the intersection is
  // exactly the shared edge, which is exempt by definition.
  if (shared == 2) return TriContact::None;

  Vec3 ca[6], cb[6];
  const int na = clipTriToPlane(a, da, ca);
  const int nb = clipTriToPlane(b, db, cb);
  if (na == 0 || nb == 0) return TriContact::None;       // defensive
  if (na > 2 || nb > 2) return contactCoplanar(a, b, n1, shared);  // defensive

  // Overlap the two chords on the plane-intersection line, parameterized
  // by the dominant axis of the line direction (world units, no division
  // by the possibly small line-direction length).
  const Vec3 w = cross(n1, n2);
  int axis = 0;
  if (std::fabs(w.y) > std::fabs(w[axis])) axis = 1;
  if (std::fabs(w.z) > std::fabs(w[axis])) axis = 2;
  if (w[axis] == 0.0) return TriContact::None;  // parallel distinct planes (defensive)

  double lo1 = ca[0][axis], hi1 = ca[0][axis];
  for (int i = 1; i < na; ++i) {
    lo1 = std::min(lo1, ca[i][axis]);
    hi1 = std::max(hi1, ca[i][axis]);
  }
  double lo2 = cb[0][axis], hi2 = cb[0][axis];
  for (int i = 1; i < nb; ++i) {
    lo2 = std::min(lo2, cb[i][axis]);
    hi2 = std::max(hi2, cb[i][axis]);
  }
  const double lo = std::max(lo1, lo2);
  const double hi = std::min(hi1, hi2);

  if (hi - lo > kLenEps) return TriContact::Segment;
  if (hi - lo >= -kLenEps) {
    // A single contact point. With a shared vertex the intersection is
    // exactly that vertex (the intersection is convex and must contain
    // it), which is exempt.
    return shared >= 1 ? TriContact::None : TriContact::Point;
  }
  return TriContact::None;
}

//! \brief The narrow phase: classifies the intersection of two triangles.
//! \param shared The number of vertex indices the triangles share (0..3).
TriContact classifyPair(const Vec3 a[3], const Vec3 b[3], int shared) {
  if (shared >= 3) return TriContact::None;  // identical face: fully shared

  const Vec3 n1 = cross(a[1] - a[0], a[2] - a[0]);
  const Vec3 n2 = cross(b[1] - b[0], b[2] - b[0]);
  const double l1 = length(n1);
  const double l2 = length(n2);
  if (l1 == 0.0 || l2 == 0.0) return TriContact::None;  // defensive (contract excludes)

  double da[3], db[3];
  for (int k = 0; k < 3; ++k) da[k] = dot(n2, a[k] - b[0]) / l2;
  for (int k = 0; k < 3; ++k) db[k] = dot(n1, b[k] - a[0]) / l1;

  const bool coplanar = std::fabs(da[0]) <= kEps && std::fabs(da[1]) <= kEps && std::fabs(da[2]) <= kEps;
  if (coplanar) return contactCoplanar(a, b, n1, shared);
  return contactNonCoplanar(a, b, da, db, n1, n2, shared);
}

//! \brief Counts shared vertex indices and classifies one face pair.
TriContact classifyFaces(const std::vector<Vec3>& verts, const std::vector<EditableMesh::FaceIndices>& faces,
                         uint32_t f, uint32_t g) {
  const auto& A = faces[f];
  const auto& B = faces[g];
  int shared = 0;
  for (int i = 0; i < 3; ++i) {
    for (int j = 0; j < 3; ++j) {
      if (A[i] == B[j]) {
        ++shared;
        break;
      }
    }
  }
  const Vec3 a[3] = {verts[A[0]], verts[A[1]], verts[A[2]]};
  const Vec3 b[3] = {verts[B[0]], verts[B[1]], verts[B[2]]};
  return classifyPair(a, b, shared);
}

//! \brief Exact AABB overlap test (touching boxes count as overlapping).
bool boxesOverlap(const BBox<double>& a, const BBox<double>& b) {
  return a.min.x <= b.max.x && a.max.x >= b.min.x && a.min.y <= b.max.y && a.max.y >= b.min.y &&
         a.min.z <= b.max.z && a.max.z >= b.min.z;
}

//! \brief The exact bounding box of a triangle.
BBox<double> triBox(const Vec3& p, const Vec3& q, const Vec3& r) {
  return BBox<double>(FastBVH::min(p, FastBVH::min(q, r)), FastBVH::max(p, FastBVH::max(q, r)));
}

//! \brief Validates the numeric contract for one triangle.
bool triangleValid(const Vec3 p[3]) {
  const double l0 = length(p[1] - p[0]);
  const double l1 = length(p[2] - p[1]);
  const double l2 = length(p[0] - p[2]);
  if (l0 < kMinEdge || l1 < kMinEdge || l2 < kMinEdge) return false;
  const double ln = length(cross(p[1] - p[0], p[2] - p[0]));
  const double maxe = std::max(l0, std::max(l1, l2));
  return ln >= kMinHeight * maxe;
}

//! \brief Validates and moves the vertex array (contract checks).
std::vector<Vec3> checkedVertices(std::vector<Vec3> v) {
  for (const auto& p : v) {
    if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) {
      throw std::invalid_argument("EditableMesh: non-finite vertex position");
    }
    if (std::fabs(p.x) > kCoordBound || std::fabs(p.y) > kCoordBound || std::fabs(p.z) > kCoordBound) {
      throw std::invalid_argument("EditableMesh: vertex coordinate outside the +/-1e6 bound");
    }
  }
  return v;
}

//! \brief Validates and moves the face array (contract checks).
std::vector<EditableMesh::FaceIndices> checkedFaces(std::vector<EditableMesh::FaceIndices> f, std::size_t nv) {
  if (f.empty()) {
    throw std::invalid_argument("EditableMesh: at least one face is required");
  }
  for (const auto& t : f) {
    for (int k = 0; k < 3; ++k) {
      if (t[k] >= nv) {
        throw std::invalid_argument("EditableMesh: face index out of range");
      }
    }
  }
  return f;
}

//! \brief The initial face slot array (identity order; the builder reorders).
std::vector<EditableMesh::FaceSlot> makeSlots(std::size_t n) {
  std::vector<EditableMesh::FaceSlot> slots(n);
  for (std::size_t i = 0; i < n; ++i) slots[i].face = (uint32_t)i;
  return slots;
}

//! \brief The initial exact face boxes.
std::vector<BBox<double>> makeFaceBoxes(const std::vector<Vec3>& verts,
                                        const std::vector<EditableMesh::FaceIndices>& faces) {
  std::vector<BBox<double>> boxes(faces.size());
  for (std::size_t i = 0; i < faces.size(); ++i) {
    boxes[i] = triBox(verts[faces[i][0]], verts[faces[i][1]], verts[faces[i][2]]);
  }
  return boxes;
}

//! \brief The ray-triangle intersector used by closest(). Stores the hit
//! barycentric coordinates in the intersection uv channels.
struct TriIntersector final {
  const std::vector<Vec3>* verts;
  const std::vector<EditableMesh::FaceIndices>* faces;

  Intersection<double, EditableMesh::FaceSlot> operator()(const EditableMesh::FaceSlot& slot,
                                                          const Ray<double>& ray) const {
    const auto& tri = (*faces)[slot.face];
    const Vec3& a = (*verts)[tri[0]];
    const Vec3& b = (*verts)[tri[1]];
    const Vec3& c = (*verts)[tri[2]];
    const Vec3 e1 = b - a;
    const Vec3 e2 = c - a;
    const Vec3 p = cross(ray.d, e2);
    const double det = dot(e1, p);
    if (det == 0.0) return Intersection<double, EditableMesh::FaceSlot>{};
    const double inv = 1.0 / det;
    const Vec3 tv = ray.o - a;
    const double u = dot(tv, p) * inv;
    if (u < 0.0 || u > 1.0) return Intersection<double, EditableMesh::FaceSlot>{};
    const Vec3 q = cross(tv, e1);
    const double v = dot(ray.d, q) * inv;
    if (v < 0.0 || u + v > 1.0) return Intersection<double, EditableMesh::FaceSlot>{};
    const double t = dot(e2, q) * inv;
    if (t < 0.0) return Intersection<double, EditableMesh::FaceSlot>{};
    const Vec3 nrm = normalize(cross(e1, e2));
    return Intersection<double, EditableMesh::FaceSlot>{t, &slot, nrm, {u, v}};
  }
};

}  // namespace

const char* contact_kind_name(EditableMesh::ContactKind kind) noexcept {
  switch (kind) {
    case EditableMesh::ContactKind::Point: return "point";
    case EditableMesh::ContactKind::Segment: return "segment";
    case EditableMesh::ContactKind::Area: return "area";
  }
  return "unknown";
}

EditableMesh::EditableMesh(std::vector<Vec3> vertices, std::vector<FaceIndices> faces)
    : verts_(checkedVertices(std::move(vertices))),
      faces_(checkedFaces(std::move(faces), verts_.size())),
      slots_(makeSlots(faces_.size())),
      face_boxes_(makeFaceBoxes(verts_, faces_)),
      bvh_(buildTree(slots_, face_boxes_)) {
  // Point-to-face index.
  vertex_faces_.resize(verts_.size());
  for (uint32_t f = 0; f < faces_.size(); ++f) {
    for (int k = 0; k < 3; ++k) {
      vertex_faces_[faces_[f][k]].push_back(f);
    }
  }

  // Parent chain, depths and the face-to-leaf index.
  const auto& nodes = bvh_.getNodes();
  const std::size_t nn = nodes.size();
  node_parent_.assign(nn, ~0u);
  node_depth_.assign(nn, 0);
  face_leaf_.assign(faces_.size(), ~0u);
  std::vector<uint32_t> stack;
  stack.push_back(0);
  while (!stack.empty()) {
    const uint32_t i = stack.back();
    stack.pop_back();
    const auto& node = nodes[i];
    if (node.isLeaf()) {
      for (uint32_t s = node.start; s < node.start + node.primitive_count; ++s) {
        face_leaf_[slots_[s].face] = i;
      }
    } else {
      const uint32_t l = i + 1;
      const uint32_t r = i + node.right_offset;
      node_parent_[l] = i;
      node_depth_[l] = node_depth_[i] + 1;
      node_parent_[r] = i;
      node_depth_[r] = node_depth_[i] + 1;
      stack.push_back(l);
      stack.push_back(r);
    }
  }

  face_contacts_.assign(faces_.size(), std::set<ContactKey>{});
  dirty_stamp_.assign(faces_.size(), 0u);

  // Initial contact set via dual-tree self-pairing.
  detectAllContacts();
  rebuildCache();
}

BVH<double, EditableMesh::FaceSlot> EditableMesh::buildTree(std::vector<FaceSlot>& slots,
                                                            const std::vector<BBox<double>>& boxes) {
  DefaultBuilder<double> builder;
  auto converter = [&boxes](const FaceSlot& s) { return boxes[s.face]; };
  return builder(slots, converter);
}

BBox<double> EditableMesh::faceBox(uint32_t face) const {
  const auto& t = faces_[face];
  return triBox(verts_[t[0]], verts_[t[1]], verts_[t[2]]);
}

void EditableMesh::detectAllContacts() {
  const auto& nodes = bvh_.getNodes();

  struct NodePair {
    uint32_t a, b;
  };
  std::vector<NodePair> stack;
  stack.push_back(NodePair{0, 0});

  while (!stack.empty()) {
    const NodePair pr = stack.back();
    stack.pop_back();
    const auto& na = nodes[pr.a];
    const auto& nb = nodes[pr.b];
    if (pr.a != pr.b && !boxesOverlap(na.bbox, nb.bbox)) continue;

    const bool la = na.isLeaf();
    const bool lb = nb.isLeaf();
    if (la && lb) {
      for (uint32_t i = na.start; i < na.start + na.primitive_count; ++i) {
        const uint32_t j0 = (pr.a == pr.b) ? i + 1 : nb.start;
        for (uint32_t j = j0; j < nb.start + nb.primitive_count; ++j) {
          const uint32_t f = slots_[i].face;
          const uint32_t g = slots_[j].face;
          if (f == g) continue;
          if (!boxesOverlap(face_boxes_[f], face_boxes_[g])) continue;
          const TriContact k = classifyFaces(verts_, faces_, f, g);
          if (k != TriContact::None) {
            const ContactKey key = std::minmax(f, g);
            contact_map_[key] = toKind(k);
            face_contacts_[key.first].insert(key);
            face_contacts_[key.second].insert(key);
          }
        }
      }
    } else if (la) {
      stack.push_back(NodePair{pr.a, pr.b + 1});
      stack.push_back(NodePair{pr.a, pr.b + nb.right_offset});
    } else if (lb) {
      stack.push_back(NodePair{pr.a + 1, pr.b});
      stack.push_back(NodePair{pr.a + na.right_offset, pr.b});
    } else if (pr.a == pr.b) {
      const uint32_t r = pr.a + na.right_offset;
      stack.push_back(NodePair{pr.a + 1, pr.a + 1});
      stack.push_back(NodePair{pr.a + 1, r});
      stack.push_back(NodePair{r, r});
    } else {
      const uint32_t ra = pr.a + na.right_offset;
      const uint32_t rb = pr.b + nb.right_offset;
      stack.push_back(NodePair{pr.a + 1, pr.b + 1});
      stack.push_back(NodePair{pr.a + 1, rb});
      stack.push_back(NodePair{ra, pr.b + 1});
      stack.push_back(NodePair{ra, rb});
    }
  }
}

void EditableMesh::queryPairs(uint32_t f, const std::vector<uint32_t>& dirty_stamp, uint32_t dirty_gen,
                              std::map<ContactKey, ContactKind>& found, UpdateStats& stats) const {
  const auto& nodes = bvh_.getNodes();
  const BBox<double>& fb = face_boxes_[f];

  uint32_t stack[64];
  int sp = 0;
  stack[sp++] = 0;
  while (sp > 0) {
    const uint32_t ni = stack[--sp];
    const auto& node = nodes[ni];
    ++stats.node_pair_checks;
    if (!boxesOverlap(fb, node.bbox)) continue;
    if (node.isLeaf()) {
      for (uint32_t s = node.start; s < node.start + node.primitive_count; ++s) {
        const uint32_t g = slots_[s].face;
        if (g == f) continue;
        // Pairs of two dirty faces are tested once, by the smaller face id.
        if (g < f && dirty_stamp[g] == dirty_gen) continue;
        ++stats.face_box_checks;
        if (!boxesOverlap(fb, face_boxes_[g])) continue;
        const ContactKey key = std::minmax(f, g);
        if (found.find(key) != found.end()) continue;
        ++stats.triangle_pair_tests;
        const TriContact k = classifyFaces(verts_, faces_, f, g);
        if (k != TriContact::None) found[key] = toKind(k);
      }
    } else {
      stack[sp++] = ni + 1;
      stack[sp++] = ni + node.right_offset;
    }
  }
}

EditableMesh::UpdateResult EditableMesh::update_vertices(const std::vector<VertexMove>& moves) {
  UpdateResult out;
  auto reject = [&out](const char* msg, uint32_t v) {
    out.accepted = false;
    out.error = msg;
    out.error_vertex = v;
    return out;
  };

  // ---- Validation phase (nothing is mutated here) ----
  std::set<uint32_t> seen;
  for (const auto& m : moves) {
    if (m.vertex >= verts_.size()) {
      return reject("vertex index out of range", m.vertex);
    }
    if (!seen.insert(m.vertex).second) {
      return reject("duplicate vertex in update batch", m.vertex);
    }
    if (!std::isfinite(m.position.x) || !std::isfinite(m.position.y) || !std::isfinite(m.position.z)) {
      return reject("non-finite vertex position", m.vertex);
    }
    if (std::fabs(m.position.x) > kCoordBound || std::fabs(m.position.y) > kCoordBound ||
        std::fabs(m.position.z) > kCoordBound) {
      return reject("vertex position outside the +/-1e6 bound", m.vertex);
    }
  }

  std::map<uint32_t, Vec3> pending;
  for (const auto& m : moves) pending[m.vertex] = m.position;

  // Dirty faces: every face incident to a moved vertex.
  std::set<uint32_t> affected;
  for (const auto& m : moves) {
    for (const uint32_t f : vertex_faces_[m.vertex]) affected.insert(f);
  }

  // Reject updates that would make any incident face degenerate.
  for (const uint32_t f : affected) {
    const auto& tri = faces_[f];
    Vec3 p[3];
    uint32_t moved = ~0u;
    for (int k = 0; k < 3; ++k) {
      const auto it = pending.find(tri[k]);
      if (it != pending.end()) {
        p[k] = it->second;
        moved = tri[k];
      } else {
        p[k] = verts_[tri[k]];
      }
    }
    if (!triangleValid(p)) {
      return reject("update would create a degenerate face", moved);
    }
  }

  // ---- Commit phase ----
  UpdateStats stats;
  const std::vector<uint32_t> dirty(affected.begin(), affected.end());

  // Snapshot the old contacts incident to dirty faces.
  std::map<ContactKey, ContactKind> old_incident;
  for (const uint32_t f : dirty) {
    for (const auto& key : face_contacts_[f]) old_incident[key] = contact_map_.at(key);
  }

  // Apply the new positions.
  for (const auto& m : moves) verts_[m.vertex] = m.position;

  // Refit the exact face boxes of dirty faces.
  for (const uint32_t f : dirty) face_boxes_[f] = faceBox(f);

  auto& nodes = bvh_.getMutableNodes();

  // Refit the leaves holding dirty faces (each leaf once).
  std::vector<uint32_t> dirty_leaves;
  dirty_leaves.reserve(dirty.size());
  for (const uint32_t f : dirty) dirty_leaves.push_back(face_leaf_[f]);
  std::sort(dirty_leaves.begin(), dirty_leaves.end());
  dirty_leaves.erase(std::unique(dirty_leaves.begin(), dirty_leaves.end()), dirty_leaves.end());
  for (const uint32_t L : dirty_leaves) {
    const auto& node = nodes[L];
    BBox<double> bb = face_boxes_[slots_[node.start].face];
    for (uint32_t s = node.start + 1; s < node.start + node.primitive_count; ++s) {
      bb.expandToInclude(face_boxes_[slots_[s].face]);
    }
    nodes[L].bbox = bb;
    ++stats.leaves_recomputed;
  }

  // Refit the ancestors of dirty leaves, deepest first.
  std::vector<uint32_t> ancestors;
  for (const uint32_t L : dirty_leaves) {
    for (uint32_t p = node_parent_[L]; p != ~0u; p = node_parent_[p]) ancestors.push_back(p);
  }
  std::sort(ancestors.begin(), ancestors.end(), [this](uint32_t x, uint32_t y) {
    if (node_depth_[x] != node_depth_[y]) return node_depth_[x] > node_depth_[y];
    return x < y;
  });
  ancestors.erase(std::unique(ancestors.begin(), ancestors.end()), ancestors.end());
  for (const uint32_t n : ancestors) {
    const uint32_t right = n + nodes[n].right_offset;
    BBox<double> bb = nodes[n + 1].bbox;
    bb.expandToInclude(nodes[right].bbox);
    nodes[n].bbox = bb;
    ++stats.internal_nodes_recomputed;
  }

  // Pair queries for the dirty faces with real node pruning.
  ++dirty_gen_;
  if (dirty_gen_ == 0u) {  // generation wrap-around: reset all stamps
    std::fill(dirty_stamp_.begin(), dirty_stamp_.end(), 0u);
    dirty_gen_ = 1;
  }
  for (const uint32_t f : dirty) dirty_stamp_[f] = dirty_gen_;
  std::map<ContactKey, ContactKind> found;
  for (const uint32_t f : dirty) queryPairs(f, dirty_stamp_, dirty_gen_, found, stats);

  // Swap the incident contacts: drop the old ones, insert the new ones.
  for (const auto& kv : old_incident) {
    contact_map_.erase(kv.first);
    face_contacts_[kv.first.first].erase(kv.first);
    face_contacts_[kv.first.second].erase(kv.first);
  }
  for (const auto& kv : found) {
    contact_map_[kv.first] = kv.second;
    face_contacts_[kv.first.first].insert(kv.first);
    face_contacts_[kv.first.second].insert(kv.first);
  }

  // The kind-aware set difference.
  for (const auto& kv : found) {
    const auto it = old_incident.find(kv.first);
    if (it == old_incident.end() || it->second != kv.second) {
      out.added.push_back(Contact{kv.first.first, kv.first.second, kv.second});
    }
  }
  for (const auto& kv : old_incident) {
    const auto it = found.find(kv.first);
    if (it == found.end() || it->second != kv.second) {
      out.removed.push_back(Contact{kv.first.first, kv.first.second, kv.second});
    }
  }
  std::sort(out.added.begin(), out.added.end());
  std::sort(out.removed.begin(), out.removed.end());

  rebuildCache();

  out.accepted = true;
  out.error = nullptr;
  out.error_vertex = ~0u;
  out.stats = stats;
  return out;
}

bool EditableMesh::closest(const Ray<double>& ray, RayHit& out) const {
  const TriIntersector intersector{&verts_, &faces_};
  const Traverser<double, FaceSlot, TriIntersector> traverser(bvh_, intersector);
  const auto hit = traverser.traverse(ray);
  if (!hit) return false;
  out.face = hit.object->face;
  out.t = hit.t;
  out.u = hit.uv[0];
  out.v = hit.uv[1];
  return true;
}

std::vector<EditableMesh::NodeDiag> EditableMesh::node_diagnostics() const {
  const auto& nodes = bvh_.getNodes();
  std::vector<NodeDiag> out;
  out.reserve(nodes.size());
  for (uint32_t i = 0; i < nodes.size(); ++i) {
    const auto& n = nodes[i];
    out.push_back(NodeDiag{i,
                           node_parent_[i] == ~0u ? -1 : (int32_t)node_parent_[i],
                           node_depth_[i],
                           n.isLeaf(),
                           n.start,
                           n.primitive_count,
                           n.bbox});
  }
  return out;
}

std::vector<uint32_t> EditableMesh::leaf_slots(uint32_t node_index) const {
  const auto& nodes = bvh_.getNodes();
  std::vector<uint32_t> out;
  if (node_index >= nodes.size() || !nodes[node_index].isLeaf()) return out;
  const auto& n = nodes[node_index];
  out.reserve(n.primitive_count);
  for (uint32_t s = n.start; s < n.start + n.primitive_count; ++s) {
    out.push_back(slots_[s].face);
  }
  return out;
}

uint32_t EditableMesh::face_leaf(uint32_t face) const {
  return face < face_leaf_.size() ? face_leaf_[face] : ~0u;
}

void EditableMesh::rebuildCache() {
  contacts_cache_.clear();
  contacts_cache_.reserve(contact_map_.size());
  for (const auto& kv : contact_map_) {
    contacts_cache_.push_back(Contact{kv.first.first, kv.first.second, kv.second});
  }
}

}  // namespace FastBVH
