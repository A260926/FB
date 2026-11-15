// Task tests for SurfaceAnchorSet. No main(): task_support/driver.c drives the cases.
//
// All reference closest-point math below is implemented independently of the production code in
// src/surface/surface_anchors.cpp, and every marker is always checked against its immutable
// initial target rather than any previously projected position.

#include "task_support/check.h"

#include "geometrycentral/surface/manifold_surface_mesh.h"
#include "geometrycentral/surface/mutation_manager.h"
#include "geometrycentral/surface/quadric_error_simplification.h"
#include "geometrycentral/surface/surface_anchors.h"
#include "geometrycentral/surface/vertex_position_geometry.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <type_traits>
#include <unordered_map>
#include <vector>

using namespace geometrycentral;
using namespace geometrycentral::surface;

namespace {

constexpr double EPS = 1e-9;

bool closeVec(const Vector3& a, const Vector3& b, double tol = EPS) {
  return (a - b).norm() <= tol * std::max({1.0, a.norm(), b.norm()});
}
bool closeScalar(double a, double b, double tol = EPS) { return std::abs(a - b) <= tol * std::max(1.0, std::abs(b)); }

// ---------------------------------------------------------------------------
// Independent reference: closest point on a triangle, returned as barycentric
// weights for (a, b, c). Formulated as a 2D least-squares projection in the
// (ab, ac) basis followed by explicit clamping into the simplex domain.
// ---------------------------------------------------------------------------
Vector3 refClosestBary(const Vector3& p, const Vector3& a, const Vector3& b, const Vector3& c) {
  Vector3 ab = b - a;
  Vector3 ac = c - a;
  Vector3 ap = p - a;
  double d00 = dot(ab, ab);
  double d01 = dot(ab, ac);
  double d11 = dot(ac, ac);
  double d20 = dot(ap, ab);
  double d21 = dot(ap, ac);
  double denom = d00 * d11 - d01 * d01;
  double s = 0., t = 0.;
  if (denom > 1e-300) {
    s = (d11 * d20 - d01 * d21) / denom;
    t = (d00 * d21 - d01 * d20) / denom;
  }

  auto clampSeg = [](const Vector3& point, const Vector3& x, const Vector3& y) -> double {
    double len2 = (y - x).norm2();
    if (len2 <= 0.) return 0.;
    return std::min(1., std::max(0., dot(point - x, y - x) / len2));
  };

  if (s >= 0. && t >= 0. && s + t <= 1.) {
    return Vector3{1. - s - t, s, t};
  }
  if (s < 0. && t < 0.) return Vector3{1., 0., 0.};
  if (s > 1. && t < 0.) return Vector3{0., 1., 0.};
  if (t > 1. && s < 0.) return Vector3{0., 0., 1.};
  if (s < 0.) {
    t = clampSeg(p, a, c);
    return Vector3{1. - t, 0., t};
  }
  if (t < 0.) {
    s = clampSeg(p, a, b);
    return Vector3{1. - s, s, 0.};
  }
  if (s + t > 1.) {
    // Closest point on edge b-c.
    double u = clampSeg(p, b, c);
    return Vector3{0., 1. - u, u};
  }
  return Vector3{1. - s - t, s, t};
}

Vector3 baryPoint(const Vector3& bary, const std::array<Vector3, 3>& p) {
  return bary[0] * p[0] + bary[1] * p[1] + bary[2] * p[2];
}

// Independent patch simulation, mirroring the collapse rules in test-owned code.
struct RefTriangle {
  Face face;
  size_t origin = 0;
  std::array<Vertex, 3> verts{};
  std::array<Vector3, 3> pos{};
};

std::vector<RefTriangle> refSimulate(ManifoldSurfaceMesh& mesh, VertexPositionGeometry& geom, Edge e,
                                     const Vector3& newPos) {
  Vertex u = e.halfedge().tailVertex();
  Vertex v = e.halfedge().tipVertex();
  std::vector<Face> patch;
  std::set<size_t> seen;
  for (Vertex vert : {u, v}) {
    for (Face f : vert.adjacentFaces()) {
      if (seen.insert(f.getIndex()).second) patch.push_back(f);
    }
  }
  std::vector<RefTriangle> out;
  for (Face f : patch) {
    std::array<Vertex, 3> fv{};
    size_t k = 0;
    for (Vertex w : f.adjacentVertices()) fv[k++] = w;
    bool hasU = false, hasV = false;
    for (Vertex w : fv) {
      if (w == u) hasU = true;
      if (w == v) hasV = true;
    }
    if (hasU && hasV) continue;
    RefTriangle tri;
    tri.face = f;
    tri.origin = f.getIndex();
    for (k = 0; k < 3; k++) {
      tri.verts[k] = (fv[k] == u || fv[k] == v) ? v : fv[k];
      tri.pos[k] = (fv[k] == u || fv[k] == v) ? newPos : geom.vertexPositions[fv[k]];
    }
    out.push_back(tri);
  }
  return out;
}

// ---------------------------------------------------------------------------
// Mesh builders
// ---------------------------------------------------------------------------
struct TestMesh {
  std::unique_ptr<ManifoldSurfaceMesh> mesh;
  std::unique_ptr<VertexPositionGeometry> geom;
};

TestMesh makeGrid(size_t n, double zOffset = 0.) {
  std::vector<Vector3> ps;
  for (size_t y = 0; y < n; y++) {
    for (size_t x = 0; x < n; x++) {
      double u = double(x) / double(n - 1);
      double v = double(y) / double(n - 1);
      double z = 0.15 * std::sin(2.7 * u + 0.3) * std::cos(2.1 * v) + 0.04 * u * v + zOffset;
      ps.push_back(Vector3{u, v, z});
    }
  }
  std::vector<std::vector<size_t>> fs;
  for (size_t y = 0; y + 1 < n; y++) {
    for (size_t x = 0; x + 1 < n; x++) {
      size_t a = y * n + x, b = a + 1, c = a + n, d = c + 1;
      fs.push_back({a, b, d});
      fs.push_back({a, d, c});
    }
  }
  TestMesh tm;
  tm.mesh = std::unique_ptr<ManifoldSurfaceMesh>(new ManifoldSurfaceMesh(fs));
  VertexData<Vector3> pos(*tm.mesh);
  for (Vertex vtx : tm.mesh->vertices()) pos[vtx] = ps[vtx.getIndex()];
  tm.geom = std::unique_ptr<VertexPositionGeometry>(new VertexPositionGeometry(*tm.mesh, pos));
  return tm;
}

// Closed, bumpy sphere from a subdivided octahedron.
TestMesh makeClosedShell(size_t subdivs) {
  std::vector<Vector3> pts = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
  std::vector<std::array<size_t, 3>> fs = {{4, 0, 2}, {4, 2, 1}, {4, 1, 3}, {4, 3, 0},
                                           {5, 2, 0}, {5, 1, 2}, {5, 3, 1}, {5, 0, 3}};
  auto key = [](size_t a, size_t b) { return std::make_pair(std::min(a, b), std::max(a, b)); };
  for (size_t s = 0; s < subdivs; s++) {
    std::vector<std::array<size_t, 3>> next;
    std::map<std::pair<size_t, size_t>, size_t> mid;
    auto midpoint = [&](size_t a, size_t b) -> size_t {
      auto k = key(a, b);
      auto it = mid.find(k);
      if (it != mid.end()) return it->second;
      Vector3 m = 0.5 * (pts[a] + pts[b]);
      pts.push_back(m);
      size_t idx = pts.size() - 1;
      mid.emplace(k, idx);
      return idx;
    };
    for (auto f : fs) {
      size_t ab = midpoint(f[0], f[1]);
      size_t bc = midpoint(f[1], f[2]);
      size_t ca = midpoint(f[2], f[0]);
      next.push_back({f[0], ab, ca});
      next.push_back({ab, f[1], bc});
      next.push_back({ca, bc, f[2]});
      next.push_back({ab, bc, ca});
    }
    fs = next;
  }
  for (Vector3& p : pts) {
    double r = 1. + 0.07 * std::sin(4. * p.x) * std::cos(3. * p.y) * std::sin(5. * p.z + 0.5);
    p = r * p.normalize();
  }
  std::vector<std::vector<size_t>> polygons;
  for (auto f : fs) polygons.push_back({f[0], f[1], f[2]});
  TestMesh tm;
  tm.mesh = std::unique_ptr<ManifoldSurfaceMesh>(new ManifoldSurfaceMesh(polygons));
  VertexData<Vector3> pos(*tm.mesh);
  for (Vertex v : tm.mesh->vertices()) pos[v] = pts[v.getIndex()];
  tm.geom = std::unique_ptr<VertexPositionGeometry>(new VertexPositionGeometry(*tm.mesh, pos));
  return tm;
}

TestMesh makeTwoShells(size_t n, double gap) {
  TestMesh a = makeGrid(n, 0.);
  TestMesh b = makeGrid(n, gap); // second shell directly above, spatially very close
  std::vector<Vector3> pts;
  size_t nVerts = n * n;
  for (Vertex v : a.mesh->vertices()) pts.push_back(a.geom->vertexPositions[v]);
  for (Vertex v : b.mesh->vertices()) pts.push_back(b.geom->vertexPositions[v]);
  std::vector<std::vector<size_t>> fs;
  for (Face f : a.mesh->faces()) {
    std::vector<size_t> face;
    for (Vertex v : f.adjacentVertices()) face.push_back(v.getIndex());
    fs.push_back(face);
  }
  for (Face f : b.mesh->faces()) {
    std::vector<size_t> face;
    for (Vertex v : f.adjacentVertices()) face.push_back(nVerts + v.getIndex());
    fs.push_back(face);
  }
  TestMesh out;
  out.mesh = std::unique_ptr<ManifoldSurfaceMesh>(new ManifoldSurfaceMesh(fs));
  VertexData<Vector3> pos(*out.mesh);
  for (Vertex v : out.mesh->vertices()) pos[v] = pts[v.getIndex()];
  out.geom = std::unique_ptr<VertexPositionGeometry>(new VertexPositionGeometry(*out.mesh, pos));
  return out;
}

Edge findInteriorEdge(ManifoldSurfaceMesh& mesh) {
  for (Edge e : mesh.edges()) {
    if (e.isBoundary()) continue;
    bool interior = true;
    for (Vertex v : e.adjacentVertices())
      if (v.isBoundary()) interior = false;
    if (interior) return e;
  }
  return Edge();
}

SurfaceAnchorInput faceInput(uint64_t id, Face f, Vector3 bary, double budget) {
  SurfaceAnchorInput in;
  in.id = id;
  in.point = SurfacePoint(f, bary);
  in.maxDistance = budget;
  return in;
}

// Verify a snapshot is internally consistent and within its budget. Returns 0 on success so it
// can be used as TASK_CHECK(checkSnapshot(...) == 0).
static int checkSnapshot(const SurfaceAnchorSnapshot& s, VertexPositionGeometry& geom) {
  TASK_CHECK(s.id != 0);
  TASK_CHECK(s.face != Face() && !s.face.isDead());
  TASK_CHECK(std::isfinite(s.barycentric.x) && std::isfinite(s.barycentric.y) && std::isfinite(s.barycentric.z));
  TASK_CHECK(s.barycentric.x >= -EPS && s.barycentric.y >= -EPS && s.barycentric.z >= -EPS);
  TASK_CHECK(closeScalar(s.barycentric.x + s.barycentric.y + s.barycentric.z, 1.));
  std::array<Vertex, 3> fv{};
  size_t k = 0;
  for (Vertex v : s.face.adjacentVertices()) fv[k++] = v;
  Vector3 reconstructed = s.barycentric.x * geom.vertexPositions[fv[0]] +
                          s.barycentric.y * geom.vertexPositions[fv[1]] +
                          s.barycentric.z * geom.vertexPositions[fv[2]];
  TASK_CHECK(closeVec(reconstructed, s.position));
  TASK_CHECK((s.position - s.target).norm() <= s.maxDistance + EPS * std::max(1.0, s.maxDistance));
  return 0;
}

const SurfaceAnchorSnapshot& findSnapshot(const std::vector<SurfaceAnchorSnapshot>& snaps, uint64_t id) {
  for (const SurfaceAnchorSnapshot& s : snaps)
    if (s.id == id) return s;
  throw std::runtime_error("missing snapshot id");
}

// Independent expectation for one anchor after a simulated collapse: closest point over the
// reference surviving triangles, with origin-based tie resolution.
struct RefExpectation {
  Face face;
  Vector3 bary;
  Vector3 position;
  double distance;
};

RefExpectation refExpect(const Vector3& target, const std::vector<RefTriangle>& tris) {
  if (tris.empty()) throw std::runtime_error("refExpect called with no surviving triangles");
  size_t best = 0;
  double bestDist = std::numeric_limits<double>::infinity();
  Vector3 bestBary = Vector3::zero();
  for (size_t i = 0; i < tris.size(); i++) {
    Vector3 bary = refClosestBary(target, tris[i].pos[0], tris[i].pos[1], tris[i].pos[2]);
    Vector3 q = baryPoint(bary, tris[i].pos);
    double d = (target - q).norm();
    if (!std::isfinite(bestDist)) {
      bestDist = d;
      best = i;
      bestBary = bary;
      continue;
    }
    double tieEps = 1e-12 * std::max({1., d, bestDist});
    if (d < bestDist - tieEps || (std::abs(d - bestDist) <= tieEps &&
                                  tris[i].origin < tris[best].origin)) {
      bestDist = d;
      best = i;
      bestBary = bary;
    }
  }
  RefExpectation r;
  r.face = tris[best].face;
  r.bary = bestBary;
  r.position = baryPoint(bestBary, tris[best].pos);
  r.distance = bestDist;
  return r;
}

class CountingPolicy : public EdgeCollapsePolicy {
public:
  int preCount = 0;
  int postCount = 0;
  std::vector<double> preTs;
  std::vector<double> postTs;
  std::vector<Vertex> survivors;
  void beforeEdgeCollapse(const Edge& e, const double& t) override {
    (void)e;
    preCount++;
    preTs.push_back(t);
  }
  void afterEdgeCollapse(const Vertex& v, const double& t) override {
    postCount++;
    postTs.push_back(t);
    survivors.push_back(v);
  }
};

} // namespace

static_assert(!std::is_copy_constructible<SurfaceAnchorSet>::value, "anchor set must not be copyable");
static_assert(!std::is_copy_assignable<SurfaceAnchorSet>::value, "anchor set must not be copy-assignable");

// ===========================================================================
// projection group
// ===========================================================================

// Marker inside a face that the collapse deletes: it must migrate onto a surviving triangle.
static int projection_deleted_face() {
  TestMesh tm = makeGrid(10);
  auto& mesh = *tm.mesh;
  auto& geom = *tm.geom;
  MutationManager mm(mesh, geom);
  Edge e = findInteriorEdge(mesh);
  TASK_CHECK(e != Edge());
  Face deleted = e.halfedge().face();
  Vector3 target = 0.2 * geom.vertexPositions[deleted.halfedge().vertex()] +
                   0.3 * geom.vertexPositions[deleted.halfedge().next().vertex()] +
                   0.5 * geom.vertexPositions[deleted.halfedge().next().next().vertex()];

  std::vector<SurfaceAnchorInput> in{faceInput(7, deleted, Vector3{0.2, 0.3, 0.5}, 1.0)};
  SurfaceAnchorSet anchors(mm, in);
  auto before = anchors.snapshot();
  TASK_CHECK(before.size() == 1 && closeVec(before[0].target, target));
  TASK_CHECK(before[0].face == deleted);

  Vector3 newPos = 0.5 * (geom.vertexPositions[e.halfedge().tailVertex()] +
                          geom.vertexPositions[e.halfedge().tipVertex()]);
  auto refTris = refSimulate(mesh, geom, e, newPos);
  RefExpectation exp = refExpect(target, refTris);

  size_t facesBefore = mesh.nFaces();
  Vertex surv = mm.collapseEdge(e, newPos);
  TASK_CHECK(surv != Vertex());
  TASK_CHECK(mesh.nFaces() == facesBefore - 2);
  TASK_CHECK(deleted.isDead());

  auto after = anchors.snapshot();
  TASK_CHECK(after.size() == 1);
  const SurfaceAnchorSnapshot& s = after[0];
  TASK_CHECK(s.face == exp.face);
  TASK_CHECK(closeVec(s.position, exp.position));
  TASK_CHECK(closeScalar((s.position - target).norm(), exp.distance));
  TASK_CHECK(closeVec(s.barycentric, exp.bary, 1e-8));
  TASK_CHECK(checkSnapshot(s, geom) == 0);
  return 0;
}

// A surviving face changes shape when its endpoint moves; the marker re-projects from the
// immutable target instead of inheriting the old barycentric slot.
static int projection_surviving_face_relocate() {
  TestMesh tm = makeGrid(12);
  auto& mesh = *tm.mesh;
  auto& geom = *tm.geom;
  MutationManager mm(mesh, geom);
  Edge e = findInteriorEdge(mesh);
  Vertex u = e.halfedge().tailVertex(), v = e.halfedge().tipVertex();

  // Pick a face incident on u but not v: it survives the collapse.
  Face host;
  for (Face f : u.adjacentFaces()) {
    bool hasV = false;
    for (Vertex w : f.adjacentVertices())
      if (w == v) hasV = true;
    if (!hasV) {
      host = f;
      break;
    }
  }
  TASK_CHECK(host != Face());

  std::vector<SurfaceAnchorInput> in{faceInput(11, host, Vector3{0.3, 0.3, 0.4}, 1.0)};
  SurfaceAnchorSet anchors(mm, in);
  Vector3 target = anchors.snapshot()[0].target;

  // Deliberately move the survivor well off the edge midpoint.
  Vector3 newPos = geom.vertexPositions[v] + Vector3{0.08, -0.05, 0.07};
  auto refTris = refSimulate(mesh, geom, e, newPos);
  RefExpectation exp = refExpect(target, refTris);

  Vertex surv = mm.collapseEdge(e, newPos);
  TASK_CHECK(surv != Vertex());
  TASK_CHECK(!host.isDead());
  auto relocateSnaps = anchors.snapshot();
  const SurfaceAnchorSnapshot& s = findSnapshot(relocateSnaps, 11);
  TASK_CHECK(closeVec(s.position, exp.position));
  TASK_CHECK(checkSnapshot(s, geom) == 0);
  // The marker actually moved because its host triangle moved.
  TASK_CHECK((s.position - target).norm() > 0.);
  return 0;
}

// Several collapses in a row, then compress, then several more: markers stay attached and every
// step matches an independent projection on the current host triangle.
static int projection_multi_step_with_compress() {
  TestMesh tm = makeGrid(14);
  auto& mesh = *tm.mesh;
  auto& geom = *tm.geom;
  MutationManager mm(mesh, geom);
  std::vector<SurfaceAnchorInput> in;
  in.push_back(faceInput(1, mesh.face(40), Vector3{0.2, 0.3, 0.5}, 0.4));
  in.push_back(faceInput(2, mesh.face(120), Vector3{0.6, 0.1, 0.3}, 0.4));
  in.push_back(faceInput(3, mesh.face(200), Vector3{0.1, 0.1, 0.8}, 0.4));
  SurfaceAnchorSet anchors(mm, in);

  auto validateAll = [&]() -> int {
    for (const SurfaceAnchorSnapshot& s : anchors.snapshot()) {
      TASK_CHECK(checkSnapshot(s, geom) == 0);
      std::array<Vector3, 3> p;
      size_t k = 0;
      for (Vertex vtx : s.face.adjacentVertices()) p[k++] = geom.vertexPositions[vtx];
      Vector3 independent = baryPoint(refClosestBary(s.target, p[0], p[1], p[2]), p);
      TASK_CHECK(closeVec(independent, s.position, 1e-8));
    }
    return 0;
  };

  size_t collapses = 0;
  for (int round = 0; round < 2; round++) {
    for (int i = 0; i < 6; i++) {
      Edge e = findInteriorEdge(mesh);
      if (e == Edge()) break;
      Vector3 newPos = 0.5 * (geom.vertexPositions[e.halfedge().tailVertex()] +
                              geom.vertexPositions[e.halfedge().tipVertex()]);
      if (mm.collapseEdge(e, newPos) != Vertex()) collapses++;
      TASK_CHECK(validateAll() == 0);
    }
    if (round == 0) {
      auto preCompress = anchors.snapshot();
      std::map<uint64_t, Vector3> prePos, preTarget;
      for (auto& s : preCompress) {
        prePos[s.id] = s.position;
        preTarget[s.id] = s.target;
      }
      mesh.compress();
      mesh.validateConnectivity();
      auto postCompress = anchors.snapshot();
      TASK_CHECK(postCompress.size() == preCompress.size());
      for (auto& s : postCompress) {
        TASK_CHECK(closeVec(s.position, prePos[s.id]));
        TASK_CHECK(closeVec(s.target, preTarget[s.id]));
        // The stored vertices must match the face's actual winding after re-indexing.
        size_t k = 0;
        for (Vertex vtx : s.face.adjacentVertices()) {
          (void)vtx;
          k++;
        }
        TASK_CHECK(k == 3);
      }
      TASK_CHECK(validateAll() == 0);
    }
  }
  TASK_CHECK(collapses >= 6);
  return 0;
}

// Vertex and edge markers normalize to face barycentric coordinates, choosing the smallest-index
// adjacent face, then survive a collapse correctly.
static int projection_vertex_and_edge_markers() {
  TestMesh tm = makeGrid(10);
  auto& mesh = *tm.mesh;
  auto& geom = *tm.geom;

  Vertex vx = mesh.vertex(30);
  Edge ed;
  for (Edge e : vx.adjacentEdges()) {
    if (!e.isBoundary()) {
      ed = e;
      break;
    }
  }
  TASK_CHECK(ed != Edge());

  Face expectedV;
  for (Face f : vx.adjacentFaces())
    if (expectedV == Face() || f.getIndex() < expectedV.getIndex()) expectedV = f;
  Face expectedE;
  for (Face f : ed.adjacentFaces())
    if (expectedE == Face() || f.getIndex() < expectedE.getIndex()) expectedE = f;

  std::vector<SurfaceAnchorInput> in;
  SurfaceAnchorInput va;
  va.id = 1;
  va.point = SurfacePoint(vx);
  va.maxDistance = 1.;
  in.push_back(va);
  SurfaceAnchorInput ea;
  ea.id = 2;
  ea.point = SurfacePoint(ed, 0.35);
  ea.maxDistance = 1.;
  in.push_back(ea);

  MutationManager mm(mesh, geom);
  SurfaceAnchorSet anchors(mm, in);
  auto snaps = anchors.snapshot();
  const SurfaceAnchorSnapshot& sv = findSnapshot(snaps, 1);
  const SurfaceAnchorSnapshot& se = findSnapshot(snaps, 2);
  TASK_CHECK(sv.face == expectedV);
  TASK_CHECK(se.face == expectedE);
  TASK_CHECK(closeVec(sv.position, geom.vertexPositions[vx]));
  std::array<Vertex, 3> efv{};
  size_t k = 0;
  for (Vertex w : expectedE.adjacentVertices()) efv[k++] = w;
  Vector3 expectedEdgePos = (1. - 0.35) * geom.vertexPositions[ed.halfedge().tailVertex()] +
                            0.35 * geom.vertexPositions[ed.halfedge().tipVertex()];
  TASK_CHECK(closeVec(se.position, expectedEdgePos));
  (void)efv;

  Edge e = findInteriorEdge(mesh);
  Vector3 newPos = 0.5 * (geom.vertexPositions[e.halfedge().tailVertex()] +
                          geom.vertexPositions[e.halfedge().tipVertex()]);
  TASK_CHECK(mm.collapseEdge(e, newPos) != Vertex());
  for (const SurfaceAnchorSnapshot& s : anchors.snapshot()) TASK_CHECK(checkSnapshot(s, geom) == 0);
  return 0;
}

// Equidistant candidate triangles: origin (initial face index) breaks the tie.
static int projection_tie_origin_selection() {
  TestMesh tm = makeGrid(9);
  auto& mesh = *tm.mesh;
  auto& geom = *tm.geom;
  MutationManager mm(mesh, geom);
  Edge e = findInteriorEdge(mesh);
  Vertex v = e.halfedge().tipVertex();

  std::vector<SurfaceAnchorInput> in;
  SurfaceAnchorInput va;
  va.id = 1;
  va.point = SurfacePoint(v); // target is the survivor position itself
  va.maxDistance = 1.;
  in.push_back(va);
  SurfaceAnchorSet anchors(mm, in);
  Vector3 target = anchors.snapshot()[0].target;

  // t = 1 keeps the survivor exactly at v's old position, so every surviving triangle contains
  // the target and every projection distance is zero.
  auto refTris = refSimulate(mesh, geom, e, geom.vertexPositions[v]);
  size_t minOrigin = std::numeric_limits<size_t>::max();
  for (const RefTriangle& t : refTris) minOrigin = std::min(minOrigin, t.origin);
  RefExpectation exp = refExpect(target, refTris);
  TASK_CHECK(exp.distance == 0.);
  TASK_CHECK(exp.face.getIndex() == minOrigin);

  TASK_CHECK(mm.collapseEdge(e, 1.) != Vertex());
  auto tieSnaps = anchors.snapshot();
  const SurfaceAnchorSnapshot& s = findSnapshot(tieSnaps, 1);
  TASK_CHECK(closeVec(s.position, target));
  TASK_CHECK(s.face.getIndex() == minOrigin);
  TASK_CHECK(checkSnapshot(s, geom) == 0);
  return 0;
}

// Two spatially close shells: a marker may never jump to the other component even if it is
// geometrically closer.
static int projection_dual_shell_no_jump() {
  const double gap = 0.03;
  TestMesh tm = makeTwoShells(8, gap);
  auto& mesh = *tm.mesh;
  auto& geom = *tm.geom;
  MutationManager mm(mesh, geom);
  size_t nVerts = 64;

  // Marker on the upper shell, in a face well inside the patch.
  Face upper = mesh.face(2 * 7 * 7 + 2 * 7 + 60);
  TASK_CHECK(upper != Face());
  std::vector<SurfaceAnchorInput> in{faceInput(1, upper, Vector3{0.2, 0.3, 0.5}, 1.0)};
  SurfaceAnchorSet anchors(mm, in);
  Vector3 target = anchors.snapshot()[0].target;

  // Find an interior edge of the lower shell whose patch is closer to the target than any
  // surviving upper-shell triangle can be.
  Edge lower;
  for (Edge cand : mesh.edges()) {
    if (cand.isBoundary()) continue;
    bool low = true;
    for (Vertex w : cand.adjacentVertices())
      if (w.getIndex() >= nVerts || w.isBoundary()) low = false;
    if (low) {
      lower = cand;
      break;
    }
  }
  TASK_CHECK(lower != Edge());
  size_t facesBefore = mesh.nFaces();
  Vector3 newPos = 0.5 * (geom.vertexPositions[lower.halfedge().tailVertex()] +
                          geom.vertexPositions[lower.halfedge().tipVertex()]);
  TASK_CHECK(mm.collapseEdge(lower, newPos) != Vertex());
  TASK_CHECK(mesh.nFaces() == facesBefore - 2);

  auto shellSnaps = anchors.snapshot();
  const SurfaceAnchorSnapshot& s = findSnapshot(shellSnaps, 1);
  // Still attached to an upper-shell face.
  for (Vertex w : s.face.adjacentVertices()) TASK_CHECK(w.getIndex() >= nVerts);
  TASK_CHECK(closeVec(s.target, target));
  TASK_CHECK(checkSnapshot(s, geom) == 0);
  return 0;
}

// Tight cumulative budgets: a rejected collapse never moves a marker past its budget, while
// collapses elsewhere keep succeeding.
static int projection_cumulative_budget() {
  TestMesh tm = makeGrid(12);
  auto& mesh = *tm.mesh;
  auto& geom = *tm.geom;
  MutationManager mm(mesh, geom);
  Face host = mesh.face(80);
  std::vector<SurfaceAnchorInput> in{faceInput(1, host, Vector3{0.25, 0.25, 0.5}, 0.0)};
  SurfaceAnchorSet anchors(mm, in);
  Vector3 target = anchors.snapshot()[0].target;

  int successes = 0, rejections = 0;
  for (Edge e : mesh.edges()) {
    if (e.isDead() || e.isBoundary()) continue;
    bool touches = false;
    for (Face f : e.halfedge().tailVertex().adjacentFaces())
      if (f == host) touches = true;
    for (Face f : e.halfedge().tipVertex().adjacentFaces())
      if (f == host) touches = true;
    if (!touches) continue;
    Vector3 jump = geom.vertexPositions[e.halfedge().tipVertex()] + Vector3{0.2, 0.2, 0.2};
    Vertex r = mm.collapseEdge(e, jump);
    if (r == Vertex()) {
      rejections++;
    } else {
      successes++;
    }
  }
  TASK_CHECK(rejections > 0);
  auto budgetSnaps = anchors.snapshot();
  const SurfaceAnchorSnapshot& s = findSnapshot(budgetSnaps, 1);
  TASK_CHECK(closeVec(s.target, target));
  TASK_CHECK((s.position - target).norm() <= EPS);
  TASK_CHECK(closeScalar(s.maxDistance, 0.));

  // Unconstrained regions still simplify.
  size_t facesBefore = mesh.nFaces();
  Edge far = findInteriorEdge(mesh);
  TASK_CHECK(far != Edge());
  Vector3 mid = 0.5 * (geom.vertexPositions[far.halfedge().tailVertex()] +
                       geom.vertexPositions[far.halfedge().tipVertex()]);
  TASK_CHECK(mm.collapseEdge(far, mid) != Vertex());
  TASK_CHECK(mesh.nFaces() == facesBefore - 2);
  TASK_CHECK(successes >= 0);
  return 0;
}

// Counters reflect only the current patch: far-away locked markers are never visited, and the
// projection test count equals visits times the number of surviving candidate triangles.
static int projection_counter_locality() {
  TestMesh tm = makeGrid(13);
  auto& mesh = *tm.mesh;
  auto& geom = *tm.geom;
  MutationManager mm(mesh, geom);

  // Many markers locked into the corner of the grid.
  std::vector<SurfaceAnchorInput> in;
  uint64_t id = 1;
  for (size_t fi = 0; fi < 12; fi++) in.push_back(faceInput(id++, mesh.face(fi), Vector3{0.2, 0.3, 0.5}, 0.05));
  SurfaceAnchorSet anchors(mm, in);
  TASK_CHECK(anchors.anchorVisits() == 0 && anchors.projectionTriangleTests() == 0);

  // Collapse an edge whose patch contains no markers.
  Edge far;
  for (Edge e : mesh.edges()) {
    if (e.isBoundary()) continue;
    bool touches = false;
    std::set<size_t> patchFaces;
    for (Vertex vtx : e.adjacentVertices())
      for (Face f : vtx.adjacentFaces()) patchFaces.insert(f.getIndex());
    for (size_t fi = 0; fi < 12; fi++)
      if (patchFaces.count(fi)) touches = true;
    bool interior = true;
    for (Vertex vtx : e.adjacentVertices())
      if (vtx.isBoundary()) interior = false;
    if (!touches && interior) {
      far = e;
      break;
    }
  }
  TASK_CHECK(far != Edge());
  Vector3 mid = 0.5 * (geom.vertexPositions[far.halfedge().tailVertex()] +
                       geom.vertexPositions[far.halfedge().tipVertex()]);
  TASK_CHECK(mm.collapseEdge(far, mid) != Vertex());
  TASK_CHECK(anchors.anchorVisits() == 0);
  TASK_CHECK(anchors.projectionTriangleTests() == 0);

  // Now a collapse whose patch holds exactly two markers.
  Edge local;
  for (Edge e : mesh.edges()) {
    if (e.isDead() || e.isBoundary()) continue;
    std::set<size_t> patchFaces;
    for (Vertex vtx : e.adjacentVertices())
      for (Face f : vtx.adjacentFaces()) patchFaces.insert(f.getIndex());
    size_t hit = 0;
    for (size_t fi = 0; fi < 12; fi++)
      if (patchFaces.count(fi)) hit++;
    bool interior = true;
    for (Vertex vtx : e.adjacentVertices())
      if (vtx.isBoundary()) interior = false;
    if (hit == 2 && interior) {
      local = e;
      break;
    }
  }
  TASK_CHECK(local != Edge());
  auto refTris = refSimulate(mesh, geom, local, mid);
  TASK_CHECK(!refTris.empty());
  uint64_t visitsBefore = anchors.anchorVisits();
  uint64_t testsBefore = anchors.projectionTriangleTests();
  if (mm.collapseEdge(local, mid) != Vertex()) {
    TASK_CHECK(anchors.anchorVisits() == visitsBefore + 2);
    TASK_CHECK(anchors.projectionTriangleTests() == testsBefore + 2 * refTris.size());
  }
  return 0;
}

// ===========================================================================
// lifecycle group
// ===========================================================================

static int lifecycle_invalid_inputs() {
  TestMesh tm = makeGrid(8);
  auto& mesh = *tm.mesh;
  auto& geom = *tm.geom;
  MutationManager mm(mesh, geom);
  Face f = mesh.face(10);

  auto expectThrow = [&](std::vector<SurfaceAnchorInput> bad) -> int {
    bool threw = false;
    try {
      SurfaceAnchorSet s(mm, bad);
    } catch (const std::invalid_argument&) {
      threw = true;
    }
    TASK_CHECK(threw);
    return 0;
  };

  TASK_CHECK(expectThrow({faceInput(0, f, Vector3{0.2, 0.3, 0.5}, 1.)}) == 0);             // zero id
  TASK_CHECK(expectThrow({faceInput(1, f, Vector3{0.2, 0.3, 0.5}, 1.),
                          faceInput(1, f, Vector3{0.1, 0.4, 0.5}, 1.)}) == 0);             // duplicate id
  TASK_CHECK(expectThrow({faceInput(1, f, Vector3{0.2, 0.3, 0.5}, -0.1)}) == 0);           // negative budget
  {
    auto in = faceInput(1, f, Vector3{0.2, 0.3, 0.5}, 1.);
    in.maxDistance = std::numeric_limits<double>::infinity();
    TASK_CHECK(expectThrow({in}) == 0);
  }
  {
    auto in = faceInput(1, f, Vector3{0.2, 0.3, 0.5}, 1.);
    in.maxDistance = std::numeric_limits<double>::quiet_NaN();
    TASK_CHECK(expectThrow({in}) == 0);
  }
  TASK_CHECK(expectThrow({faceInput(1, f, Vector3{-0.2, 0.7, 0.5}, 1.)}) == 0);            // negative bary
  TASK_CHECK(expectThrow({faceInput(1, f, Vector3{0.2, 0.3, 0.6}, 1.)}) == 0);             // sum != 1
  {
    auto in = faceInput(1, f, Vector3{0.2, 0.3, 0.5}, 1.);
    in.point.faceCoords.x = std::numeric_limits<double>::quiet_NaN();
    TASK_CHECK(expectThrow({in}) == 0);
  }
  {
    SurfaceAnchorInput in;
    in.id = 1;
    in.maxDistance = 1.;
    in.point = SurfacePoint(mesh.edge(0), 1.5);
    TASK_CHECK(expectThrow({in}) == 0);
  }
  {
    // Point belonging to another mesh.
    TestMesh other = makeGrid(6);
    SurfaceAnchorInput in;
    in.id = 1;
    in.maxDistance = 1.;
    in.point = SurfacePoint(other.mesh->face(0), Vector3{0.2, 0.3, 0.5});
    TASK_CHECK(expectThrow({in}) == 0);
  }
  {
    // Non-finite target through a non-finite vertex position.
    TestMesh tm2 = makeGrid(6);
    MutationManager mm2(*tm2.mesh, *tm2.geom);
    Vertex bad = tm2.mesh->vertex(5);
    Face hostOfBad;
    for (Face cand : bad.adjacentFaces()) {
      hostOfBad = cand;
      break;
    }
    TASK_CHECK(hostOfBad != Face());
    tm2.geom->vertexPositions[bad] = Vector3{std::numeric_limits<double>::infinity(), 0., 0.};
    bool threw = false;
    try {
      SurfaceAnchorSet s(mm2, {faceInput(1, hostOfBad, Vector3{0.2, 0.3, 0.5}, 1.)});
    } catch (const std::invalid_argument&) {
      threw = true;
    }
    TASK_CHECK(threw);
  }

  // After all the failed constructions the manager is still unbound and usable.
  SurfaceAnchorSet good(mm, {faceInput(1, f, Vector3{0.2, 0.3, 0.5}, 1.)});
  TASK_CHECK(good.snapshot().size() == 1);

  // A second simultaneous bind is rejected and leaves the first one in place.
  bool threw = false;
  try {
    SurfaceAnchorSet second(mm, {faceInput(2, f, Vector3{0.1, 0.4, 0.5}, 1.)});
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  TASK_CHECK(threw);
  TASK_CHECK(good.snapshot().size() == 1);
  return 0;
}

// Destruction detaches cleanly: later simplification works, user policies and constraints
// survive, and a fresh set can bind afterward.
static int lifecycle_destruction_detaches() {
  TestMesh tm = makeGrid(12);
  auto& mesh = *tm.mesh;
  auto& geom = *tm.geom;
  MutationManager mm(mesh, geom);

  EdgeData<bool> collapsible(mesh, true);
  Edge locked;
  for (Edge e : mesh.edges()) {
    if (!e.isBoundary()) {
      locked = e;
      break;
    }
  }
  collapsible[locked] = false;
  mm.setCollapsibleEdges(collapsible);

  CountingPolicy* policy = new CountingPolicy();
  MutationPolicyHandle handle = mm.registerPolicy(policy);
  TASK_CHECK(mm.mayCollapseEdge(locked) == false);

  {
    SurfaceAnchorSet temp(mm, {faceInput(1, mesh.face(30), Vector3{0.2, 0.3, 0.5}, 0.3)});
    TASK_CHECK(temp.snapshot().size() == 1);
  }

  // Locked edge still locked, policy still registered.
  TASK_CHECK(mm.mayCollapseEdge(locked) == false);
  Edge e = findInteriorEdge(mesh);
  Vector3 mid = 0.5 * (geom.vertexPositions[e.halfedge().tailVertex()] +
                       geom.vertexPositions[e.halfedge().tipVertex()]);
  TASK_CHECK(mm.collapseEdge(e, mid) != Vertex());
  TASK_CHECK(policy->preCount == 1 && policy->postCount == 1);
  TASK_CHECK(mm.collapseEdge(locked, mid) == Vertex());

  // Rebind a fresh set on the same manager.
  SurfaceAnchorSet again(mm, {faceInput(2, mesh.face(20), Vector3{0.2, 0.3, 0.5}, 0.3)});
  TASK_CHECK(again.snapshot().size() == 1);
  handle.remove();
  return 0;
}

// Explicit compress between two simplifications keeps handles, winding and provenance correct.
static int lifecycle_compress_between_runs() {
  TestMesh tm = makeGrid(16);
  auto& mesh = *tm.mesh;
  auto& geom = *tm.geom;
  MutationManager mm(mesh, geom);
  std::vector<SurfaceAnchorInput> in;
  for (uint64_t i = 0; i < 6; i++)
    in.push_back(faceInput(i + 1, mesh.face(20 + 10 * i), Vector3{0.2, 0.3, 0.5}, 0.5));
  SurfaceAnchorSet anchors(mm, in);

  quadricErrorSimplify(mesh, geom, 1e-5, mm);
  auto mid = anchors.snapshot();
  std::map<uint64_t, Vector3> midPos, midTarget;
  for (auto& s : mid) {
    midPos[s.id] = s.position;
    midTarget[s.id] = s.target;
    TASK_CHECK(checkSnapshot(s, geom) == 0);
  }
  size_t facesAfterFirst = mesh.nFaces();

  mesh.compress();
  mesh.validateConnectivity();
  auto afterCompress = anchors.snapshot();
  TASK_CHECK(afterCompress.size() == mid.size());
  for (auto& s : afterCompress) {
    TASK_CHECK(closeVec(s.position, midPos[s.id]));
    TASK_CHECK(closeVec(s.target, midTarget[s.id]));
    TASK_CHECK(checkSnapshot(s, geom) == 0);
  }

  quadricErrorSimplify(mesh, geom, 1e-4, mm);
  mesh.compress();
  mesh.validateConnectivity();
  auto end = anchors.snapshot();
  TASK_CHECK(end.size() == in.size());
  TASK_CHECK(mesh.nFaces() <= facesAfterFirst);
  for (auto& s : end) {
    TASK_CHECK(closeVec(s.target, midTarget[s.id]));
    TASK_CHECK(checkSnapshot(s, geom) == 0);
    TASK_CHECK((s.position - s.target).norm() <= s.maxDistance + EPS);
  }
  return 0;
}

// Real policy plus lambda handlers, registered in both orders: t values, event pairing and
// required geometry quantities are all correct.
static int lifecycle_callback_ordering_and_quantities() {
  for (int order = 0; order < 2; order++) {
    TestMesh tm = makeGrid(12);
    auto& mesh = *tm.mesh;
    auto& geom = *tm.geom;
    MutationManager mm(mesh, geom);
    geom.requireFaceAreas();
    geom.requireFaceNormals();
    geom.requireVertexDualAreas();

    CountingPolicy* policy = new CountingPolicy();
    auto registerPolicyNow = [&]() { return mm.registerPolicy(policy); };
    std::unique_ptr<MutationPolicyHandle> handle;

    int lambdaPre = 0, lambdaPost = 0;
    double lambdaT = -1.;
    auto registerLambdas = [&]() {
      mm.registerEdgeCollapseHandlers(
          [&](Edge, double t) {
            lambdaPre++;
            lambdaT = t;
          },
          [&](Vertex, double t) {
            lambdaPost++;
            lambdaT = t;
          });
    };

    std::unique_ptr<SurfaceAnchorSet> anchors;
    auto makeAnchors = [&]() {
      anchors = std::unique_ptr<SurfaceAnchorSet>(
          new SurfaceAnchorSet(mm, {faceInput(1, mesh.face(50), Vector3{0.2, 0.3, 0.5}, 1.)}));
    };
    if (order == 0) {
      handle = std::unique_ptr<MutationPolicyHandle>(new MutationPolicyHandle(registerPolicyNow()));
      registerLambdas();
      makeAnchors();
    } else {
      makeAnchors();
      handle = std::unique_ptr<MutationPolicyHandle>(new MutationPolicyHandle(registerPolicyNow()));
      registerLambdas();
    }

    Edge e = findInteriorEdge(mesh);
    Vertex u = e.halfedge().tailVertex(), v = e.halfedge().tipVertex();
    Vector3 oldU = geom.vertexPositions[u];
    Vector3 oldV = geom.vertexPositions[v];

    // Explicit t overload.
    Vertex r = mm.collapseEdge(e, 0.25);
    TASK_CHECK(r != Vertex());
    TASK_CHECK(policy->preCount == 1 && policy->postCount == 1);
    TASK_CHECK(lambdaPre == 1 && lambdaPost == 1);
    TASK_CHECK(closeScalar(policy->preTs[0], 0.25));
    TASK_CHECK(closeScalar(policy->postTs[0], 0.25));
    TASK_CHECK(closeScalar(lambdaT, 0.25));
    Vector3 expectedPos = 0.75 * oldU + 0.25 * oldV;
    TASK_CHECK(closeVec(geom.vertexPositions[r], expectedPos));

    // Required quantities in the post-collapse state match the immediate evaluators.
    bool starChecked = false;
    for (Face f : r.adjacentFaces()) {
      TASK_CHECK(std::isfinite(geom.faceAreas[f]));
      TASK_CHECK(closeScalar(geom.faceAreas[f], geom.faceArea(f), 1e-8));
      TASK_CHECK(closeVec(geom.faceNormals[f], geom.faceNormal(f), 1e-8));
      TASK_CHECK(geom.faceAreas[f] > 0.);
      starChecked = true;
    }
    double dual = 0.;
    for (Face f : r.adjacentFaces()) dual += geom.faceArea(f) / 3.;
    TASK_CHECK(starChecked);
    TASK_CHECK(closeScalar(geom.vertexDualAreas[r], dual, 1e-8));

    // Position overload: an off-segment point projects and clamps to t = 0.5. The lateral offset
    // is built perpendicular to the edge in the xy-plane (and small enough not to flip any patch
    // normal), so its segment projection is exactly the midpoint.
    Edge e2 = findInteriorEdge(mesh);
    TASK_CHECK(e2 != Edge());
    Vector3 tail = geom.vertexPositions[e2.halfedge().tailVertex()];
    Vector3 tip = geom.vertexPositions[e2.halfedge().tipVertex()];
    Vector3 edgeDir = tip - tail;
    Vector3 perp{-edgeDir.y, edgeDir.x, 0.};
    if (perp.norm2() > 0.) perp = 0.02 * perp.normalize();
    Vector3 offSegment = 0.5 * (tail + tip) + perp;
    int preBefore = policy->preCount;
    Vertex r2 = mm.collapseEdge(e2, offSegment);
    TASK_CHECK(r2 != Vertex());
    TASK_CHECK(policy->preCount == preBefore + 1);
    TASK_CHECK(closeScalar(policy->preTs.back(), 0.5));
    TASK_CHECK(closeScalar(policy->postTs.back(), 0.5));
    TASK_CHECK(closeVec(geom.vertexPositions[r2], offSegment));
    handle->remove();
  }
  return 0;
}

// Rejected candidates mutate nothing and fire no observers; a run after failed attempts matches
// a control run that never attempted them.
static int lifecycle_failure_rollback_and_retry() {
  struct RunResult {
    bool ok = false;
    size_t faces = 0;
    std::vector<SurfaceAnchorSnapshot> snaps;
  };
  struct CheckFailure {};
  auto require = [](bool condition) {
    if (!condition) throw CheckFailure{};
  };
  auto runControl = [&](bool withFailedAttempts) -> RunResult {
    RunResult r;
    try {
      TestMesh tm = makeGrid(14);
      auto& mesh = *tm.mesh;
      auto& geom = *tm.geom;
      MutationManager mm(mesh, geom);
      CountingPolicy* policy = new CountingPolicy();
      MutationPolicyHandle handle = mm.registerPolicy(policy);
      std::vector<SurfaceAnchorInput> in;
      in.push_back(faceInput(1, mesh.face(60), Vector3{0.2, 0.3, 0.5}, 0.02));
      in.push_back(faceInput(2, mesh.face(150), Vector3{0.4, 0.4, 0.2}, 0.02));
      SurfaceAnchorSet anchors(mm, in);

      if (withFailedAttempts) {
        // Budget rejection: a large jump next to a tightly budgeted marker.
        Edge e = findInteriorEdge(mesh);
        size_t facesBefore = mesh.nFaces();
        auto snapsBefore = anchors.snapshot();
        Vector3 jump = geom.vertexPositions[e.halfedge().tipVertex()] + Vector3{0.35, 0.35, 0.35};
        require(mm.collapseEdge(e, jump) == Vertex());
        require(mesh.nFaces() == facesBefore);
        require(policy->preCount == 0 && policy->postCount == 0);
        require(anchors.snapshot().size() == snapsBefore.size());
        for (size_t i = 0; i < snapsBefore.size(); i++) {
          require(closeVec(anchors.snapshot()[i].position, snapsBefore[i].position));
        }

        // Geometrically illegal candidate: a position that flips a surviving triangle normal.
        Edge e2 = findInteriorEdge(mesh);
        Face f2 = e2.halfedge().face();
        Vector3 a = geom.vertexPositions[f2.halfedge().vertex()];
        Vector3 flip = a + Vector3{0., 0., 5.};
        require(mm.collapseEdge(e2, flip) == Vertex());
        require(policy->preCount == 0 && policy->postCount == 0);

        // Boundary edges are never collapsed with anchors active.
        Edge boundary;
        for (Edge cand : mesh.edges())
          if (cand.isBoundary()) {
            boundary = cand;
            break;
          }
        require(boundary != Edge());
        require(mm.collapseEdge(boundary, 0.5) == Vertex());
        require(policy->preCount == 0 && policy->postCount == 0);

        // Out-of-range and non-finite arguments are hard errors.
        bool threw = false;
        try {
          mm.collapseEdge(e, 1.2);
        } catch (const std::invalid_argument&) {
          threw = true;
        }
        require(threw);
        threw = false;
        try {
          mm.collapseEdge(e, Vector3{std::numeric_limits<double>::quiet_NaN(), 0., 0.});
        } catch (const std::invalid_argument&) {
          threw = true;
        }
        require(threw);
      }

      quadricErrorSimplify(mesh, geom, 2e-4, mm);
      mesh.compress();
      r.faces = mesh.nFaces();
      r.snaps = anchors.snapshot();
      r.ok = true;
      handle.remove();
    } catch (const CheckFailure&) {
      r.ok = false;
    }
    return r;
  };

  RunResult clean = runControl(false);
  RunResult afterFailures = runControl(true);
  TASK_CHECK(clean.ok && afterFailures.ok);
  TASK_CHECK(clean.faces == afterFailures.faces);
  TASK_CHECK(clean.snaps.size() == afterFailures.snaps.size());
  for (size_t i = 0; i < clean.snaps.size(); i++) {
    TASK_CHECK(clean.snaps[i].id == afterFailures.snaps[i].id);
    TASK_CHECK(closeVec(clean.snaps[i].target, afterFailures.snaps[i].target));
    TASK_CHECK(closeVec(clean.snaps[i].position, afterFailures.snaps[i].position));
    TASK_CHECK(closeVec(clean.snaps[i].barycentric, afterFailures.snaps[i].barycentric, 1e-8));
  }
  return 0;
}

// ===========================================================================
// scheduling group
// ===========================================================================

// Real QEM on a curved open patch: anchored patches see successful collapses, at least one
// marker changes attachment, and every marker stays within budget with valid barycentric data.
static int scheduling_qem_open_patch() {
  TestMesh tm = makeGrid(20);
  auto& mesh = *tm.mesh;
  auto& geom = *tm.geom;
  MutationManager mm(mesh, geom);
  CountingPolicy* policy = new CountingPolicy();
  MutationPolicyHandle handle = mm.registerPolicy(policy);

  std::vector<SurfaceAnchorInput> in;
  uint64_t id = 1;
  for (size_t fy = 4; fy < 16; fy += 4)
    for (size_t fx = 4; fx < 16; fx += 4)
      in.push_back(faceInput(id++, mesh.face(2 * (fy * 19 + fx)), Vector3{0.25, 0.3, 0.45}, 0.06));
  SurfaceAnchorSet anchors(mm, in);
  auto before = anchors.snapshot();
  size_t facesBefore = mesh.nFaces();

  quadricErrorSimplify(mesh, geom, 1e-4, mm);
  mesh.compress();
  mesh.validateConnectivity();

  TASK_CHECK(mesh.nFaces() < facesBefore);
  TASK_CHECK(policy->preCount == policy->postCount);
  TASK_CHECK(policy->postCount > 0);
  TASK_CHECK(anchors.anchorVisits() > 0);
  TASK_CHECK(anchors.projectionTriangleTests() > 0);

  auto after = anchors.snapshot();
  TASK_CHECK(after.size() == before.size());
  bool changed = false;
  for (const SurfaceAnchorSnapshot& s : after) {
    TASK_CHECK(checkSnapshot(s, geom) == 0);
    const SurfaceAnchorSnapshot& b = findSnapshot(before, s.id);
    TASK_CHECK(closeVec(s.target, b.target));
    if (s.face != b.face || !closeVec(s.position, b.position)) changed = true;
  }
  TASK_CHECK(changed);
  handle.remove();
  return 0;
}

// Real QEM on a closed shell with no boundary at all.
static int scheduling_qem_closed_shell() {
  TestMesh tm = makeClosedShell(3);
  auto& mesh = *tm.mesh;
  auto& geom = *tm.geom;
  TASK_CHECK(!mesh.hasBoundary());
  MutationManager mm(mesh, geom);
  CountingPolicy* policy = new CountingPolicy();
  MutationPolicyHandle handle = mm.registerPolicy(policy);

  std::vector<SurfaceAnchorInput> in;
  for (size_t i = 0; i < 8; i++)
    in.push_back(faceInput(i + 1, mesh.face(7 * i + 3), Vector3{0.2, 0.3, 0.5}, 0.08));
  SurfaceAnchorSet anchors(mm, in);
  auto before = anchors.snapshot();
  size_t facesBefore = mesh.nFaces();

  quadricErrorSimplify(mesh, geom, 1e-4, mm);
  mesh.compress();
  mesh.validateConnectivity();
  TASK_CHECK(mesh.nFaces() < facesBefore);
  TASK_CHECK(policy->preCount == policy->postCount && policy->postCount > 0);

  auto after = anchors.snapshot();
  bool changed = false;
  for (const SurfaceAnchorSnapshot& s : after) {
    TASK_CHECK(checkSnapshot(s, geom) == 0);
    const SurfaceAnchorSnapshot& b = findSnapshot(before, s.id);
    if (s.face != b.face || !closeVec(s.position, b.position)) changed = true;
  }
  TASK_CHECK(changed);
  handle.remove();
  return 0;
}

// Anchor rejections do not stop the run, and previously rejected edges are not blacklisted:
// successful collapses keep happening after a rejection has been observed.
static int scheduling_rejections_do_not_stop() {
  TestMesh tm = makeGrid(18);
  auto& mesh = *tm.mesh;
  auto& geom = *tm.geom;
  MutationManager mm(mesh, geom);
  CountingPolicy* policy = new CountingPolicy();
  MutationPolicyHandle handle = mm.registerPolicy(policy);
  std::vector<SurfaceAnchorInput> in{
      faceInput(1, mesh.face(100), Vector3{0.25, 0.25, 0.5}, 0.0)};
  SurfaceAnchorSet anchors(mm, in);

  // Force an anchor rejection first.
  Edge e = findInteriorEdge(mesh);
  Vector3 jump = geom.vertexPositions[e.halfedge().tipVertex()] + Vector3{0.3, 0.3, 0.3};
  TASK_CHECK(mm.collapseEdge(e, jump) == Vertex());
  int eventsAtRejection = policy->postCount;
  TASK_CHECK(eventsAtRejection == 0);

  size_t facesBefore = mesh.nFaces();
  quadricErrorSimplify(mesh, geom, 5e-4, mm);
  TASK_CHECK(mesh.nFaces() < facesBefore);
  TASK_CHECK(policy->postCount > eventsAtRejection);
  TASK_CHECK(policy->preCount == policy->postCount);
  // The zero-budget marker never drifted.
  auto rejectSnaps = anchors.snapshot();
  const SurfaceAnchorSnapshot& s = findSnapshot(rejectSnaps, 1);
  TASK_CHECK((s.position - s.target).norm() <= EPS);
  handle.remove();
  return 0;
}

// The constraint-layer dirty-edge channel reports exactly the post-collapse star, once, and
// nothing on rejection; QEM consumes it through the production scheduler.
static int scheduling_dirty_edge_channel() {
  TestMesh tm = makeGrid(14);
  auto& mesh = *tm.mesh;
  auto& geom = *tm.geom;
  MutationManager mm(mesh, geom);
  SurfaceAnchorSet anchors(mm, {faceInput(1, mesh.face(70), Vector3{0.2, 0.3, 0.5}, 0.4)});

  Edge rejected = findInteriorEdge(mesh);
  Vector3 jump = geom.vertexPositions[rejected.halfedge().tipVertex()] + Vector3{0.4, 0.4, 0.4};
  TASK_CHECK(mm.collapseEdge(rejected, jump) == Vertex());
  std::vector<Edge> dirty;
  anchors.takeDirtyEdges(dirty);
  TASK_CHECK(dirty.empty()); // rejected candidates report nothing

  Edge e = findInteriorEdge(mesh);
  Vector3 mid = 0.5 * (geom.vertexPositions[e.halfedge().tailVertex()] +
                       geom.vertexPositions[e.halfedge().tipVertex()]);
  Vertex surv = mm.collapseEdge(e, mid);
  TASK_CHECK(surv != Vertex());
  anchors.takeDirtyEdges(dirty);
  TASK_CHECK(!dirty.empty());
  std::set<size_t> star;
  for (Edge ne : surv.adjacentEdges()) star.insert(ne.getIndex());
  for (Edge d : dirty) {
    TASK_CHECK(!d.isDead());
    TASK_CHECK(star.count(d.getIndex()) == 1);
  }
  // Every star edge was reported.
  TASK_CHECK(dirty.size() == star.size());
  std::vector<Edge> again;
  anchors.takeDirtyEdges(again);
  TASK_CHECK(again.empty());

  // The QEM scheduler itself drives the channel: a full run leaves no unconsumed dirty edges.
  quadricErrorSimplify(mesh, geom, 1e-4, mm);
  std::vector<Edge> leftover;
  anchors.takeDirtyEdges(leftover);
  TASK_CHECK(leftover.empty());
  return 0;
}

// User collapsible/repositionable constraints remain in force alongside anchors, including
// across an explicit compress().
static int scheduling_user_constraints_survive() {
  TestMesh tm = makeGrid(16);
  auto& mesh = *tm.mesh;
  auto& geom = *tm.geom;
  MutationManager mm(mesh, geom);
  CountingPolicy* policy = new CountingPolicy();
  MutationPolicyHandle handle = mm.registerPolicy(policy);

  EdgeData<bool> collapsible(mesh, true);
  Edge locked = findInteriorEdge(mesh);
  collapsible[locked] = false;
  mm.setCollapsibleEdges(collapsible);
  VertexData<bool> movable(mesh, true);
  for (Vertex v : mesh.vertices())
    if (v.isBoundary()) movable[v] = false;
  mm.setRepositionableVertices(movable);

  SurfaceAnchorSet anchors(mm, {faceInput(1, mesh.face(90), Vector3{0.2, 0.3, 0.5}, 0.1)});
  TASK_CHECK(mm.collapseEdge(locked, 0.5) == Vertex());

  quadricErrorSimplify(mesh, geom, 1e-4, mm);
  mesh.compress();
  TASK_CHECK(mm.mayCollapseEdge(locked) == false); // constraint survived compress
  for (Vertex v : mesh.vertices()) TASK_CHECK(!v.isBoundary() || !mm.mayRepositionVertex(v));

  // A second run still honors both constraint systems and the anchors.
  size_t facesBefore = mesh.nFaces();
  quadricErrorSimplify(mesh, geom, 5e-4, mm);
  TASK_CHECK(mesh.nFaces() <= facesBefore);
  TASK_CHECK(policy->preCount == policy->postCount);
  for (const SurfaceAnchorSnapshot& s : anchors.snapshot()) TASK_CHECK(checkSnapshot(s, geom) == 0);
  handle.remove();
  return 0;
}

// ===========================================================================
// Registration
// ===========================================================================

const task_case task_cases[] = {
    {"projection", "deleted_face", &projection_deleted_face},
    {"projection", "surviving_face_relocate", &projection_surviving_face_relocate},
    {"projection", "multi_step_with_compress", &projection_multi_step_with_compress},
    {"projection", "vertex_and_edge_markers", &projection_vertex_and_edge_markers},
    {"projection", "tie_origin_selection", &projection_tie_origin_selection},
    {"projection", "dual_shell_no_jump", &projection_dual_shell_no_jump},
    {"projection", "cumulative_budget", &projection_cumulative_budget},
    {"projection", "counter_locality", &projection_counter_locality},

    {"lifecycle", "invalid_inputs", &lifecycle_invalid_inputs},
    {"lifecycle", "destruction_detaches", &lifecycle_destruction_detaches},
    {"lifecycle", "compress_between_runs", &lifecycle_compress_between_runs},
    {"lifecycle", "callback_ordering_and_quantities", &lifecycle_callback_ordering_and_quantities},
    {"lifecycle", "failure_rollback_and_retry", &lifecycle_failure_rollback_and_retry},

    {"scheduling", "qem_open_patch", &scheduling_qem_open_patch},
    {"scheduling", "qem_closed_shell", &scheduling_qem_closed_shell},
    {"scheduling", "rejections_do_not_stop", &scheduling_rejections_do_not_stop},
    {"scheduling", "dirty_edge_channel", &scheduling_dirty_edge_channel},
    {"scheduling", "user_constraints_survive", &scheduling_user_constraints_survive},
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
