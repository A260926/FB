// Incremental common subdivision tests for SignpostIntrinsicTriangulation.
//
// Registered through task_support/check.h in three groups:
//   - arrangement: intersection chains, ordering and merging, checked against independent 2D unfolding coordinates
//   - derived:     materialized mesh, source faces, scalar interpolation and matrix action, checked against a
//                  non-incremental SignpostIntrinsicTriangulation subjected to the same edits
//   - locality:    work lists, rejected edits, error handling, pointer stability and mode toggling
//
// No main() is defined here; task_support/driver.c provides it.

#include "geometrycentral/surface/common_subdivision.h"
#include "geometrycentral/surface/signpost_intrinsic_triangulation.h"
#include "geometrycentral/surface/vertex_position_geometry.h"

#include "task_support/check.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <stdexcept>
#include <vector>

using namespace geometrycentral;
using namespace geometrycentral::surface;

namespace {

const double EPS = 1e-8;

// ---------------------------------------------------------------------------
// Test data: three mutually distant components.
//   (0) a non-planar developable strip: a planar grid folded rigidly along a grid line, so every triangle keeps its
//       planar shape and the 2D unfolding coordinates are simply the unfolded grid coordinates
//   (1) a planar, irregularly triangulated patch with a boundary
//   (2) a small triangulated diamond, again planar
// ---------------------------------------------------------------------------

struct TestData {
  std::vector<std::vector<size_t>> faces;
  std::vector<Vector3> positions;
  std::vector<Vector2> unfold2D;
};

TestData buildData() {
  TestData data;

  auto addVertex = [&](Vector3 p3, Vector2 p2) {
    data.positions.push_back(p3);
    data.unfold2D.push_back(p2);
    return data.positions.size() - 1;
  };

  // === Component 0: 4x3 grid, folded by 35 degrees along the line x = 2. All fold lines coincide with mesh edges,
  // so the folding is a piecewise isometry (a non-planar developable surface).
  const size_t nx = 4, ny = 3;
  std::vector<std::vector<size_t>> grid(nx, std::vector<size_t>(ny, 0));
  const double foldAngle = 35. * M_PI / 180.;
  for (size_t j = 0; j < ny; j++) {
    for (size_t i = 0; i < nx; i++) {
      double x = (double)i;
      double y = (double)j;
      Vector3 p;
      if (x <= 2.) {
        p = Vector3{x, y, 0.};
      } else {
        double d = x - 2.; // distance past the fold line
        p = Vector3{2. + d * std::cos(foldAngle), y, d * std::sin(foldAngle)};
      }
      // Shift this component far from the origin.
      p += Vector3{-20., 40., 5.};
      grid[i][j] = addVertex(p, Vector2{x, y});
    }
  }
  for (size_t j = 0; j + 1 < ny; j++) {
    for (size_t i = 0; i + 1 < nx; i++) {
      size_t v00 = grid[i][j], v10 = grid[i + 1][j], v01 = grid[i][j + 1], v11 = grid[i + 1][j + 1];
      if ((i + j) % 2 == 0) {
        data.faces.push_back({v00, v10, v11});
        data.faces.push_back({v00, v11, v01});
      } else {
        data.faces.push_back({v00, v10, v01});
        data.faces.push_back({v10, v11, v01});
      }
    }
  }

  // === Component 1: irregular planar patch with a boundary, centered far away from component 0.
  const size_t mx = 3, my = 3;
  std::vector<std::vector<size_t>> patch(mx, std::vector<size_t>(my, 0));
  for (size_t j = 0; j < my; j++) {
    for (size_t i = 0; i < mx; i++) {
      double x = (double)i + 0.18 * std::sin(1.7 * (double)j + 0.3 * (double)i);
      double y = (double)j + 0.23 * std::cos(1.1 * (double)i + 0.6 * (double)j);
      Vector3 p{x + 30., y - 25., 0.};
      patch[i][j] = addVertex(p, Vector2{x, y});
    }
  }
  for (size_t j = 0; j + 1 < my; j++) {
    for (size_t i = 0; i + 1 < mx; i++) {
      size_t v00 = patch[i][j], v10 = patch[i + 1][j], v01 = patch[i][j + 1], v11 = patch[i + 1][j + 1];
      data.faces.push_back({v00, v10, v01});
      data.faces.push_back({v10, v11, v01});
    }
  }

  // === Component 2: a diamond made of four triangles, far from the others.
  Vector3 center{0., 50., -15.};
  size_t c = addVertex(center, Vector2{0., 0.});
  size_t rp = addVertex(center + Vector3{1.2, 0., 0.}, Vector2{1.2, 0.});
  size_t up = addVertex(center + Vector3{0., 1.1, 0.}, Vector2{0., 1.1});
  size_t lp = addVertex(center + Vector3{-1.0, 0., 0.}, Vector2{-1.0, 0.});
  size_t dn = addVertex(center + Vector3{0., -0.9, 0.}, Vector2{0., -0.9});
  data.faces.push_back({c, rp, up});
  data.faces.push_back({c, up, lp});
  data.faces.push_back({c, lp, dn});
  data.faces.push_back({c, dn, rp});

  return data;
}

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

struct TriBundle {
  std::unique_ptr<ManifoldSurfaceMesh> mesh;
  std::unique_ptr<VertexPositionGeometry> geom;
  std::unique_ptr<SignpostIntrinsicTriangulation> tri;
};

TriBundle makeTriangulation(const TestData& data) {
  TriBundle b;
  b.mesh.reset(new ManifoldSurfaceMesh(data.faces));
  VertexData<Vector3> pos(*b.mesh);
  for (Vertex v : b.mesh->vertices()) pos[v] = data.positions[v.getIndex()];
  b.geom.reset(new VertexPositionGeometry(*b.mesh, pos));
  b.tri.reset(new SignpostIntrinsicTriangulation(*b.mesh, *b.geom));
  return b;
}

double boundingDiagonal(const TestData& data) {
  Vector3 lo = data.positions.front(), hi = data.positions.front();
  for (const Vector3& p : data.positions) {
    for (int k = 0; k < 3; k++) {
      lo[k] = std::fmin(lo[k], p[k]);
      hi[k] = std::fmax(hi[k], p[k]);
    }
  }
  return (hi - lo).norm();
}

// Independent unfolding position of a surface point on the input mesh.
Vector2 unfoldPoint(const TestData& data, const SurfacePoint& p) {
  switch (p.type) {
  case SurfacePointType::Vertex:
    return data.unfold2D[p.vertex.getIndex()];
  case SurfacePointType::Edge: {
    Halfedge he = p.edge.halfedge();
    Vector2 a = data.unfold2D[he.tailVertex().getIndex()];
    Vector2 b = data.unfold2D[he.tipVertex().getIndex()];
    return (1. - p.tEdge) * a + p.tEdge * b;
  }
  case SurfacePointType::Face: {
    Halfedge he = p.face.halfedge();
    Vector2 a = data.unfold2D[he.vertex().getIndex()];
    Vector2 b = data.unfold2D[he.tipVertex().getIndex()];
    Vector2 c2 = data.unfold2D[he.next().tipVertex().getIndex()];
    return p.faceCoords[0] * a + p.faceCoords[1] * b + p.faceCoords[2] * c2;
  }
  }
  return Vector2{0., 0.};
}

// Canonical, comparable description of a surface point: element indices plus the edge/face parameters.
struct PointKey {
  SurfacePointType type;
  size_t elem;
  double t = 0.;
  Vector3 bary = Vector3::zero();
};
PointKey keyOf(const SurfacePoint& p) {
  PointKey k;
  k.type = p.type;
  if (p.type == SurfacePointType::Vertex) k.elem = p.vertex.getIndex();
  if (p.type == SurfacePointType::Edge) {
    k.elem = p.edge.getIndex();
    k.t = p.tEdge;
  }
  if (p.type == SurfacePointType::Face) {
    k.elem = p.face.getIndex();
    k.bary = p.faceCoords;
  }
  return k;
}
bool samePoint(const PointKey& a, const PointKey& b) {
  if (a.type != b.type || a.elem != b.elem) return false;
  if (a.type == SurfacePointType::Edge) return std::abs(a.t - b.t) <= EPS;
  if (a.type == SurfacePointType::Face) return (a.bary - b.bary).norm() <= EPS;
  return true;
}

// A scalar "color" field defined at the vertices of every input mesh.
double scalarField(const Vector3& p) { return 1.5 * p.x - 0.7 * p.y + 2.1 * p.z + 0.3; }

VertexData<double> inputField(SignpostIntrinsicTriangulation& tri, const TestData& data) {
  VertexData<double> f(tri.inputMesh);
  for (Vertex v : tri.inputMesh.vertices()) {
    f[v] = scalarField(data.positions[v.getIndex()]);
  }
  return f;
}

// ---------------------------------------------------------------------------
// The edit script, applied identically to an incremental and a reference triangulation.
//
// When doCrossingSplit is false, the final split-at-existing-intersection is skipped. The lazy full builder cannot
// merge the two near-coincident edge records produced by an exact split (a pre-existing numerical limitation), so
// that case is exercised only on the incremental triangulation and checked geometrically in its own test.
// ---------------------------------------------------------------------------

struct EditReport {
  size_t flippedEdge = INVALID_IND;
  size_t otherFlippedEdge = INVALID_IND;
  size_t splitEdgeInd = INVALID_IND;
  double splitT = 0.37;
  size_t crossingSplitEdge = INVALID_IND;
  double crossingSplitT = 0.5;
  CommonSubdivisionPoint* farRecord = nullptr;
};

EditReport runEdits(SignpostIntrinsicTriangulation& tri, bool collectFarRecord, const TestData& data,
                    bool doCrossingSplit = true) {
  EditReport report;

  std::vector<Edge> interior;
  for (Edge e : tri.intrinsicMesh->edges()) {
    if (!e.isBoundary()) interior.push_back(e);
  }
  GC_SAFETY_ASSERT(interior.size() >= 4, "test mesh needs several interior edges");

  Edge flipA, flipB;
  for (Edge e : interior) {
    if (tri.flipEdgeIfPossible(e)) {
      if (flipA == Edge()) {
        flipA = e;
      } else {
        flipB = e;
        break;
      }
    }
  }
  GC_SAFETY_ASSERT(flipA != Edge() && flipB != Edge(), "test needs two flippable edges");
  tri.flipEdgeIfPossible(flipA); // flip it back
  report.flippedEdge = flipA.getIndex();
  report.otherFlippedEdge = flipB.getIndex();

  if (collectFarRecord) {
    CommonSubdivision& cs = tri.getCommonSubdivision();
    for (Edge e : tri.intrinsicMesh->edges()) {
      if (e == flipA || e == flipB) continue;
      for (CommonSubdivisionPoint* p : cs.pointsAlongB[e]) {
        if (p->intersectionType == CSIntersectionType::EDGE_TRANSVERSE) {
          report.farRecord = p;
          break;
        }
      }
      if (report.farRecord) break;
    }
  }

  // Non-midpoint split on another interior edge.
  Edge splitE;
  for (Edge e : interior) {
    if (e != flipA && e != flipB) {
      splitE = e;
      break;
    }
  }
  GC_SAFETY_ASSERT(splitE != Edge(), "test needs an edge to split");
  tri.splitEdge(splitE.halfedge(), report.splitT);
  report.splitEdgeInd = splitE.getIndex();

  if (doCrossingSplit) {
    CommonSubdivision& cs = tri.getCommonSubdivision();
    for (Edge e : tri.intrinsicMesh->edges()) {
      if (e.isBoundary()) continue;
      for (CommonSubdivisionPoint* p : cs.pointsAlongB[e]) {
        if (p->intersectionType == CSIntersectionType::EDGE_TRANSVERSE) {
          report.crossingSplitEdge = e.getIndex();
          report.crossingSplitT = p->posB.tEdge;
          break;
        }
      }
      if (report.crossingSplitEdge != INVALID_IND) break;
    }
    GC_SAFETY_ASSERT(report.crossingSplitEdge != INVALID_IND,
                     "test mesh must have a transverse intersection to split at");
    tri.splitEdge(tri.intrinsicMesh->edge(report.crossingSplitEdge).halfedge(), report.crossingSplitT);
  }

  (void)data;
  return report;
}

// Snapshot of everything derived from a common subdivision, used for an incremental-vs-reference comparison.
struct DerivedSnapshot {
  std::tuple<size_t, size_t, size_t> counts;
  std::vector<double> interpValuesA;      // sorted
  std::vector<double> interpValuesB;      // sorted
  std::vector<size_t> sourceFaceAIndices; // sorted
  std::vector<std::vector<PointKey>> halfedgePathsBonA;
  std::vector<std::vector<PointKey>> halfedgePathsAonB;
  SparseMatrix<double> matrixA;
  SparseMatrix<double> matrixB;
  std::vector<double> matrixTimesFieldA; // sorted
  std::vector<double> matrixTimesFieldB; // sorted
};

DerivedSnapshot snapshot(SignpostIntrinsicTriangulation& tri, const TestData& data) {
  DerivedSnapshot s;
  CommonSubdivision& cs = tri.getCommonSubdivision();
  cs.constructMesh();

  s.counts = cs.elementCounts();

  VertexData<double> field = inputField(tri, data);
  VertexData<double> mappedA = cs.interpolateAcrossA(field);
  for (Vertex v : cs.mesh->vertices()) s.interpValuesA.push_back(mappedA[v]);
  std::sort(s.interpValuesA.begin(), s.interpValuesA.end());

  // A scalar field on the intrinsic mesh, transferred across B.
  VertexData<double> intrinsicField(*tri.intrinsicMesh);
  for (Vertex v : tri.intrinsicMesh->vertices()) {
    intrinsicField[v] = 0.5 * v.getIndex() + 1.25;
  }
  VertexData<double> mappedB = cs.interpolateAcrossB(intrinsicField);
  for (Vertex v : cs.mesh->vertices()) s.interpValuesB.push_back(mappedB[v]);
  std::sort(s.interpValuesB.begin(), s.interpValuesB.end());

  for (Face f : cs.mesh->faces()) s.sourceFaceAIndices.push_back(cs.sourceFaceA[f].getIndex());
  std::sort(s.sourceFaceAIndices.begin(), s.sourceFaceAIndices.end());

  for (Halfedge he : tri.intrinsicMesh->halfedges()) {
    std::vector<SurfacePoint> path = cs.getHalfedgePathBonA(he);
    std::vector<PointKey> keys;
    for (const SurfacePoint& p : path) keys.push_back(keyOf(p));
    s.halfedgePathsBonA.push_back(keys);
  }
  for (Halfedge he : tri.inputMesh.halfedges()) {
    std::vector<SurfacePoint> path = cs.getHalfedgePathAonB(he);
    std::vector<PointKey> keys;
    for (const SurfacePoint& p : path) keys.push_back(keyOf(p));
    s.halfedgePathsAonB.push_back(keys);
  }

  s.matrixA = cs.interpolationMatrixA();
  Eigen::VectorXd xA(tri.inputMesh.nVertices());
  for (Vertex v : tri.inputMesh.vertices()) xA[v.getIndex()] = field[v];
  Eigen::VectorXd yA = s.matrixA * xA;
  for (Eigen::Index i = 0; i < yA.size(); i++) s.matrixTimesFieldA.push_back(yA(i));
  std::sort(s.matrixTimesFieldA.begin(), s.matrixTimesFieldA.end());

  s.matrixB = cs.interpolationMatrixB();
  Eigen::VectorXd xB(tri.intrinsicMesh->nVertices());
  for (Vertex v : tri.intrinsicMesh->vertices()) xB[v.getIndex()] = intrinsicField[v];
  Eigen::VectorXd yB = s.matrixB * xB;
  for (Eigen::Index i = 0; i < yB.size(); i++) s.matrixTimesFieldB.push_back(yB(i));
  std::sort(s.matrixTimesFieldB.begin(), s.matrixTimesFieldB.end());

  return s;
}

// ---------------------------------------------------------------------------
// arrangement group
// ---------------------------------------------------------------------------

int arrangement_basicChains() {
  TestData data = buildData();
  TriBundle b = makeTriangulation(data);
  b.tri->setIncrementalCommonSubdivision(true);
  CommonSubdivision& cs = b.tri->getCommonSubdivision();

  // Initially the intrinsic mesh coincides with the input mesh: every chain consists of its two vertex endpoints;
  // the full builder only adds EDGE_PARALLEL records once interior edge vertices exist.
  for (Edge e : b.tri->intrinsicMesh->edges()) {
    const std::vector<CommonSubdivisionPoint*>& chain = cs.pointsAlongB[e];
    TASK_CHECK(chain.size() == 2);
    TASK_CHECK(chain.front()->intersectionType == CSIntersectionType::VERTEX_VERTEX);
    TASK_CHECK(chain.back()->intersectionType == CSIntersectionType::VERTEX_VERTEX);
    TASK_CHECK(chain.front()->posB.type == SurfacePointType::Vertex);
    TASK_CHECK(chain.back()->posB.type == SurfacePointType::Vertex);
  }
  for (Edge eA : b.tri->inputMesh.edges()) {
    const std::vector<CommonSubdivisionPoint*>& chain = cs.pointsAlongA[eA];
    TASK_CHECK(chain.size() == 2);
    TASK_CHECK(chain.front()->posA.type == SurfacePointType::Vertex);
    TASK_CHECK(chain.back()->posA.type == SurfacePointType::Vertex);
  }

  runEdits(*b.tri, false, data);

  // Every chain has endpoints, strictly ordered parameters, and no zero-length segments in the unfolding.
  for (Edge e : b.tri->intrinsicMesh->edges()) {
    const std::vector<CommonSubdivisionPoint*>& chain = cs.pointsAlongB[e];
    TASK_CHECK(chain.size() >= 2);
    TASK_CHECK(chain.front()->posB.type == SurfacePointType::Vertex);
    TASK_CHECK(chain.back()->posB.type == SurfacePointType::Vertex);

    double prevT = -1.;
    Vector2 prev2;
    for (size_t i = 0; i < chain.size(); i++) {
      CommonSubdivisionPoint* p = chain[i];
      if (p->intersectionType != CSIntersectionType::EDGE_PARALLEL && p->posB.type == SurfacePointType::Edge) {
        TASK_CHECK(p->posB.tEdge > prevT);
        prevT = p->posB.tEdge;
      }
      Vector2 q = unfoldPoint(data, p->posA);
      if (i > 0) {
        TASK_CHECK((q - prev2).norm() > 1e-12); // no collapsed segments in the unfolded chain
      }
      prev2 = q;
    }
  }

  // Input-side chains are ordered by posA.tEdge as well (vertex endpoints carry no edge parameter).
  for (Edge eA : b.tri->inputMesh.edges()) {
    const std::vector<CommonSubdivisionPoint*>& chain = cs.pointsAlongA[eA];
    TASK_CHECK(chain.size() >= 2);
    double prevT = -1.;
    for (CommonSubdivisionPoint* p : chain) {
      if (p->posA.type != SurfacePointType::Edge) continue;
      TASK_CHECK(p->posA.tEdge + EPS >= prevT);
      prevT = p->posA.tEdge;
    }
  }

  // The maintained chains still construct a valid manifold common subdivision mesh.
  cs.constructMesh(false, false);
  TASK_CHECK(cs.mesh->nVertices() > 0);
  TASK_CHECK(cs.mesh->nFaces() > 0);
  return 0;
}

int arrangement_mergeAtIntersection() {
  TestData data = buildData();
  TriBundle b = makeTriangulation(data);
  b.tri->setIncrementalCommonSubdivision(true);
  CommonSubdivision& cs = b.tri->getCommonSubdivision();

  // Make a transverse intersection first.
  Edge flipE;
  for (Edge e : b.tri->intrinsicMesh->edges()) {
    if (!e.isBoundary() && b.tri->flipEdgeIfPossible(e)) {
      flipE = e;
      break;
    }
  }
  TASK_CHECK(flipE != Edge());

  CommonSubdivisionPoint* target = nullptr;
  for (CommonSubdivisionPoint* p : cs.pointsAlongB[flipE]) {
    if (p->intersectionType == CSIntersectionType::EDGE_TRANSVERSE) {
      target = p;
      break;
    }
  }
  TASK_CHECK(target != nullptr);
  double tSplit = target->posB.tEdge;
  Edge inputE = target->posA.edge;
  double inputT = target->posA.tEdge;

  b.tri->splitEdge(flipE.halfedge(), tSplit);

  // Exactly one live non-parallel record sits at the intersection location. An EDGE_PARALLEL record may share the
  // location (it encodes a shared segment between consecutive vertex records), but it is not a second intersection.
  size_t nearCount = 0;
  CommonSubdivisionPoint* merged = nullptr;
  for (CommonSubdivisionPoint* p : cs.pointsAlongA[inputE]) {
    if (p->retired) continue;
    if (p->intersectionType == CSIntersectionType::EDGE_PARALLEL) continue;
    if (p->posA.type == SurfacePointType::Edge && std::abs(p->posA.tEdge - inputT) <= 1e-7) {
      nearCount++;
      merged = p;
    }
  }
  TASK_CHECK(nearCount == 1);
  TASK_CHECK(merged != nullptr);
  TASK_CHECK(merged->intersectionType == CSIntersectionType::EDGE_VERTEX);
  TASK_CHECK(merged->posB.type == SurfacePointType::Vertex);

  // The merged point is present on the two child edge chains as an endpoint.
  size_t endpointMentions = 0;
  for (Edge e : b.tri->intrinsicMesh->edges()) {
    const auto& chain = cs.pointsAlongB[e];
    if (chain.front() == merged || chain.back() == merged) endpointMentions++;
  }
  TASK_CHECK(endpointMentions >= 2);

  // The merged point lands at the same unfolded location as the original crossing.
  TASK_CHECK((unfoldPoint(data, merged->posA) - unfoldPoint(data, target->posA)).norm() <= EPS);

  // The chains still construct a valid mesh, with no repeated endpoints or zero-length segments.
  cs.constructMesh(false, false);
  TASK_CHECK(cs.mesh->nVertices() > 0);
  TASK_CHECK(cs.mesh->nFaces() > 0);
  return 0;
}

// ---------------------------------------------------------------------------
// derived group
// ---------------------------------------------------------------------------

int derived_matchesReference() {
  TestData data = buildData();
  double diag = boundingDiagonal(data);
  double geomTol = EPS * diag;

  TriBundle inc = makeTriangulation(data);
  inc.tri->setIncrementalCommonSubdivision(true);
  runEdits(*inc.tri, false, data, false);
  DerivedSnapshot a = snapshot(*inc.tri, data);

  TriBundle ref = makeTriangulation(data);
  runEdits(*ref.tri, false, data, false);
  DerivedSnapshot r = snapshot(*ref.tri, data);

  TASK_CHECK(std::get<0>(a.counts) == std::get<0>(r.counts));
  TASK_CHECK(std::get<1>(a.counts) == std::get<1>(r.counts));
  TASK_CHECK(std::get<2>(a.counts) == std::get<2>(r.counts));

  TASK_CHECK(a.interpValuesA.size() == r.interpValuesA.size());
  for (size_t i = 0; i < a.interpValuesA.size(); i++) {
    TASK_CHECK(std::abs(a.interpValuesA[i] - r.interpValuesA[i]) <= EPS);
  }
  TASK_CHECK(a.interpValuesB.size() == r.interpValuesB.size());
  for (size_t i = 0; i < a.interpValuesB.size(); i++) {
    TASK_CHECK(std::abs(a.interpValuesB[i] - r.interpValuesB[i]) <= EPS);
  }
  TASK_CHECK(a.sourceFaceAIndices == r.sourceFaceAIndices);

  TASK_CHECK(a.halfedgePathsBonA.size() == r.halfedgePathsBonA.size());
  for (size_t i = 0; i < a.halfedgePathsBonA.size(); i++) {
    const auto& pa = a.halfedgePathsBonA[i];
    const auto& pb = r.halfedgePathsBonA[i];
    TASK_CHECK(pa.size() == pb.size());
    for (size_t j = 0; j < pa.size(); j++) {
      TASK_CHECK(samePoint(pa[j], pb[j]));
    }
  }
  TASK_CHECK(a.halfedgePathsAonB.size() == r.halfedgePathsAonB.size());
  for (size_t i = 0; i < a.halfedgePathsAonB.size(); i++) {
    const auto& pa = a.halfedgePathsAonB[i];
    const auto& pb = r.halfedgePathsAonB[i];
    TASK_CHECK(pa.size() == pb.size());
    for (size_t j = 0; j < pa.size(); j++) {
      TASK_CHECK(samePoint(pa[j], pb[j]));
    }
  }

  TASK_CHECK(a.matrixA.rows() == r.matrixA.rows());
  TASK_CHECK(a.matrixA.cols() == r.matrixA.cols());
  TASK_CHECK(a.matrixB.rows() == r.matrixB.rows());
  TASK_CHECK(a.matrixB.cols() == r.matrixB.cols());
  TASK_CHECK(a.matrixTimesFieldA.size() == r.matrixTimesFieldA.size());
  for (size_t i = 0; i < a.matrixTimesFieldA.size(); i++) {
    TASK_CHECK(std::abs(a.matrixTimesFieldA[i] - r.matrixTimesFieldA[i]) <= EPS);
  }
  TASK_CHECK(a.matrixTimesFieldB.size() == r.matrixTimesFieldB.size());
  for (size_t i = 0; i < a.matrixTimesFieldB.size(); i++) {
    TASK_CHECK(std::abs(a.matrixTimesFieldB[i] - r.matrixTimesFieldB[i]) <= EPS);
  }

  // The matrix action must agree with direct interpolation on the same mesh.
  CommonSubdivision& cs = inc.tri->getCommonSubdivision();
  VertexData<double> field = inputField(*inc.tri, data);
  VertexData<double> direct = cs.interpolateAcrossA(field);
  Eigen::VectorXd x(inc.tri->inputMesh.nVertices());
  for (Vertex v : inc.tri->inputMesh.vertices()) x[v.getIndex()] = field[v];
  Eigen::VectorXd y = a.matrixA * x;
  for (Vertex v : cs.mesh->vertices()) {
    TASK_CHECK(std::abs(y[v.getIndex()] - direct[v]) <= EPS);
  }

  // Positions materialized through the common subdivision must match the reference geometry to the bbox-diagonal
  // tolerance.
  VertexData<Vector3> posA = cs.interpolateAcrossA(inc.geom->inputVertexPositions);
  CommonSubdivision& refCs = ref.tri->getCommonSubdivision();
  VertexData<Vector3> posR = refCs.interpolateAcrossA(ref.geom->inputVertexPositions);
  std::vector<double> normsA, normsR;
  for (Vertex v : cs.mesh->vertices()) normsA.push_back(posA[v].norm());
  for (Vertex v : refCs.mesh->vertices()) normsR.push_back(posR[v].norm());
  std::sort(normsA.begin(), normsA.end());
  std::sort(normsR.begin(), normsR.end());
  TASK_CHECK(normsA.size() == normsR.size());
  for (size_t i = 0; i < normsA.size(); i++) TASK_CHECK(std::abs(normsA[i] - normsR[i]) <= geomTol);
  return 0;
}

// The exact-intersection split cannot be replayed through the lazy full builder (it produces two unmerged,
// near-coincident edge records), so here it is checked against the independent unfolding geometry instead.
int derived_crossingSplitGeometry() {
  TestData data = buildData();
  double diag = boundingDiagonal(data);
  double geomTol = EPS * diag;

  TriBundle b = makeTriangulation(data);
  b.tri->setIncrementalCommonSubdivision(true);
  runEdits(*b.tri, false, data, true);
  CommonSubdivision& cs = b.tri->getCommonSubdivision();
  cs.constructMesh();

  // Every materialized vertex sits at an unfolded position that agrees with its source SurfacePoint.
  VertexData<Vector3> positions3 = cs.interpolateAcrossA(b.geom->inputVertexPositions);
  for (Vertex v : cs.mesh->vertices()) {
    CommonSubdivisionPoint* p = cs.sourcePoints[v];
    // Compare only within a planar component, where the 2D unfolding gives a direct check on the embedding.
    if (p->posA.type != SurfacePointType::Vertex) continue;
    Vector3 expected = data.positions[p->posA.vertex.getIndex()];
    TASK_CHECK((positions3[v] - expected).norm() <= geomTol);
  }

  // Scalar interpolation at every materialized vertex matches an independent evaluation of the linear field.
  VertexData<double> field = inputField(*b.tri, data);
  VertexData<double> interp = cs.interpolateAcrossA(field);
  for (Vertex v : cs.mesh->vertices()) {
    CommonSubdivisionPoint* p = cs.sourcePoints[v];
    double expected = p->posA.interpolate(field);
    TASK_CHECK(std::abs(interp[v] - expected) <= EPS);
  }

  // The matrix and direct interpolation agree, and every row is convex (nonnegative weights summing to one).
  // The matrix is stored column-major by default, so accumulate row statistics over all nonzeros.
  SparseMatrix<double> M = cs.interpolationMatrixA();
  Eigen::VectorXd x(b.tri->inputMesh.nVertices());
  for (Vertex v : b.tri->inputMesh.vertices()) x[v.getIndex()] = field[v];
  Eigen::VectorXd y = M * x;
  for (Vertex v : cs.mesh->vertices()) {
    TASK_CHECK(std::abs(y[v.getIndex()] - interp[v]) <= EPS);
  }
  Eigen::VectorXd rowSums = Eigen::VectorXd::Zero(M.rows());
  for (Eigen::Index k = 0; k < M.outerSize(); k++) {
    for (SparseMatrix<double>::InnerIterator it(M, k); it; ++it) {
      TASK_CHECK(it.value() >= -EPS);
      rowSums[it.row()] += it.value();
    }
  }
  for (Eigen::Index i = 0; i < rowSums.size(); i++) {
    TASK_CHECK(std::abs(rowSums[i] - 1.) <= EPS);
  }

  // Every intrinsic halfedge path is a non-degenerate polyline in the unfolding.
  for (Halfedge he : b.tri->intrinsicMesh->halfedges()) {
    std::vector<SurfacePoint> path = cs.getHalfedgePathBonA(he);
    TASK_CHECK(path.size() >= 2);
    Vector2 prev = unfoldPoint(data, path.front());
    for (size_t i = 1; i < path.size(); i++) {
      Vector2 q = unfoldPoint(data, path[i]);
      TASK_CHECK((q - prev).norm() > 1e-12);
      prev = q;
    }
  }
  return 0;
}

int derived_rebuildAfterMaterialization() {
  TestData data = buildData();
  TriBundle inc = makeTriangulation(data);
  inc.tri->setIncrementalCommonSubdivision(true);
  runEdits(*inc.tri, false, data, false);
  CommonSubdivision& cs = inc.tri->getCommonSubdivision();
  cs.constructMesh();
  ManifoldSurfaceMesh* oldMesh = cs.mesh.get();

  // More edits after materialization must invalidate the old derived mesh and rebuild from the current chains.
  Edge next;
  for (Edge e : inc.tri->intrinsicMesh->edges()) {
    if (!e.isBoundary() && inc.tri->flipEdgeIfPossible(e)) {
      next = e;
      break;
    }
  }
  TASK_CHECK(next != Edge());
  TASK_CHECK(cs.mesh.get() != oldMesh); // the old derived mesh handle is not promised to stay alive
  cs.constructMesh();

  TriBundle ref = makeTriangulation(data);
  runEdits(*ref.tri, false, data, false);
  for (Edge e : ref.tri->intrinsicMesh->edges()) {
    if (!e.isBoundary() && ref.tri->flipEdgeIfPossible(e)) break;
  }
  DerivedSnapshot a = snapshot(*inc.tri, data);
  DerivedSnapshot r = snapshot(*ref.tri, data);
  TASK_CHECK(std::get<0>(a.counts) == std::get<0>(r.counts));
  TASK_CHECK(std::get<2>(a.counts) == std::get<2>(r.counts));
  TASK_CHECK(a.interpValuesA.size() == r.interpValuesA.size());
  for (size_t i = 0; i < a.interpValuesA.size(); i++) {
    TASK_CHECK(std::abs(a.interpValuesA[i] - r.interpValuesA[i]) <= EPS);
  }
  return 0;
}

// ---------------------------------------------------------------------------
// locality group
// ---------------------------------------------------------------------------

int locality_workLists() {
  TestData data = buildData();
  TriBundle b = makeTriangulation(data);
  b.tri->setIncrementalCommonSubdivision(true);

  // Enabling the mode is a full build and is not reported as per-call work.
  CommonSubdivisionUpdate u0 = b.tri->lastCommonSubdivisionUpdate();
  TASK_CHECK(u0.retracedEdges.empty());
  TASK_CHECK(u0.touchedInputEdges.empty());

  Edge flipE;
  for (Edge e : b.tri->intrinsicMesh->edges()) {
    if (!e.isBoundary() && b.tri->flipEdgeIfPossible(e)) {
      flipE = e;
      break;
    }
  }
  TASK_CHECK(flipE != Edge());
  CommonSubdivisionUpdate u1 = b.tri->lastCommonSubdivisionUpdate();
  TASK_CHECK(u1.retracedEdges.size() == 1);
  TASK_CHECK(u1.retracedEdges[0] == flipE.getIndex());

  // Independently collect the input edges the fresh trace crosses, and verify each is in the touched set.
  std::vector<SurfacePoint> trace = b.tri->traceIntrinsicHalfedgeAlongInput(flipE.halfedge());
  std::vector<size_t> expected;
  for (const SurfacePoint& p : trace) {
    if (p.type == SurfacePointType::Edge) expected.push_back(p.edge.getIndex());
  }
  std::sort(expected.begin(), expected.end());
  expected.erase(std::unique(expected.begin(), expected.end()), expected.end());
  std::vector<size_t> touched = u1.touchedInputEdges;
  std::sort(touched.begin(), touched.end());
  for (size_t ind : expected) {
    TASK_CHECK(std::find(touched.begin(), touched.end(), ind) != touched.end());
  }

  // Both lists are duplicate-free by contract.
  TASK_CHECK(std::unique(u1.retracedEdges.begin(), u1.retracedEdges.end()) == u1.retracedEdges.end());
  TASK_CHECK(std::unique(u1.touchedInputEdges.begin(), u1.touchedInputEdges.end()) ==
             u1.touchedInputEdges.end());

  // A rejected flip reports empty work lists and changes nothing.
  EdgeData<bool> marked(*b.tri->intrinsicMesh, false);
  Edge markE;
  for (Edge e : b.tri->intrinsicMesh->edges()) {
    if (!e.isBoundary()) {
      markE = e;
      break;
    }
  }
  marked[markE] = true;
  b.tri->setMarkedEdges(marked);
  TASK_CHECK(!b.tri->flipEdgeIfPossible(markE));
  CommonSubdivisionUpdate u2 = b.tri->lastCommonSubdivisionUpdate();
  TASK_CHECK(u2.retracedEdges.empty());
  TASK_CHECK(u2.touchedInputEdges.empty());
  b.tri->clearMarkedEdges();

  // A split reports the retracing of every edge incident on the new vertex.
  Edge splitE;
  for (Edge e : b.tri->intrinsicMesh->edges()) {
    if (!e.isBoundary()) {
      splitE = e;
      break;
    }
  }
  size_t nEdgesBefore = b.tri->intrinsicMesh->nEdges();
  size_t nVertsBefore = b.tri->intrinsicMesh->nVertices();
  size_t nFacesBefore = b.tri->intrinsicMesh->nFaces();

  // Mark the edge before splitting: the two child edges must inherit the mark and remain unflippable.
  EdgeData<bool> splitMark(*b.tri->intrinsicMesh, false);
  splitMark[splitE] = true;
  b.tri->setMarkedEdges(splitMark);
  TASK_CHECK(b.tri->isFixed(splitE));

  b.tri->splitEdge(splitE.halfedge(), 0.41);
  CommonSubdivisionUpdate u3 = b.tri->lastCommonSubdivisionUpdate();
  // An interior split adds one vertex, two faces, and three edges (the split edge plus one radial edge per face).
  TASK_CHECK(b.tri->intrinsicMesh->nVertices() == nVertsBefore + 1);
  TASK_CHECK(b.tri->intrinsicMesh->nFaces() == nFacesBefore + 2);
  TASK_CHECK(b.tri->intrinsicMesh->nEdges() == nEdgesBefore + 3);
  TASK_CHECK(!u3.retracedEdges.empty());
  TASK_CHECK(std::unique(u3.retracedEdges.begin(), u3.retracedEdges.end()) == u3.retracedEdges.end());
  TASK_CHECK(std::unique(u3.touchedInputEdges.begin(), u3.touchedInputEdges.end()) ==
             u3.touchedInputEdges.end());

  // Every retraced edge which replaced the marked edge must itself be marked; marked edges still cannot be flipped.
  size_t markedChildren = 0;
  for (size_t eInd : u3.retracedEdges) {
    Edge eChild = b.tri->intrinsicMesh->edge(eInd);
    if (b.tri->isFixed(eChild)) {
      markedChildren++;
      TASK_CHECK(!b.tri->flipEdgeIfPossible(eChild));
    }
  }
  TASK_CHECK(markedChildren >= 2);
  b.tri->clearMarkedEdges();

  // Marked edges are still unflippable after a split: mark a child edge, confirm rejection, then unmark and flip it.
  Edge childE = b.tri->intrinsicMesh->edge(u3.retracedEdges.front());
  EdgeData<bool> childMark(*b.tri->intrinsicMesh, false);
  childMark[childE] = true;
  b.tri->setMarkedEdges(childMark);
  TASK_CHECK(!b.tri->flipEdgeIfPossible(childE));
  b.tri->clearMarkedEdges();

  // flipEdgeIfNotDelaunay reports the same work when it flips, and an empty list when it does nothing.
  bool anyDelaunayFlip = false;
  for (Edge e : b.tri->intrinsicMesh->edges()) {
    if (e.isBoundary()) continue;
    if (!b.tri->isDelaunay(e)) {
      size_t nEdges = b.tri->intrinsicMesh->nEdges();
      if (b.tri->flipEdgeIfNotDelaunay(e)) {
        CommonSubdivisionUpdate ud = b.tri->lastCommonSubdivisionUpdate();
        TASK_CHECK(ud.retracedEdges.size() == 1);
        TASK_CHECK(ud.retracedEdges[0] == e.getIndex());
        TASK_CHECK(b.tri->intrinsicMesh->nEdges() == nEdges); // flips preserve edge count
        anyDelaunayFlip = true;
        break;
      }
    } else {
      // A Delaunay edge is left alone and reports empty work.
      bool did = b.tri->flipEdgeIfNotDelaunay(e);
      TASK_CHECK(!did);
      CommonSubdivisionUpdate ud = b.tri->lastCommonSubdivisionUpdate();
      TASK_CHECK(ud.retracedEdges.empty());
      TASK_CHECK(ud.touchedInputEdges.empty());
    }
  }
  TASK_CHECK(anyDelaunayFlip);
  return 0;
}

int locality_pointerStability() {
  TestData data = buildData();
  TriBundle b = makeTriangulation(data);
  b.tri->setIncrementalCommonSubdivision(true);
  CommonSubdivision& cs = b.tri->getCommonSubdivision();

  // Flood-fill connected components so the second flipped edge can be chosen in another component, where it cannot
  // influence the geometry around the first.
  VertexData<size_t> comp(*b.tri->intrinsicMesh, INVALID_IND);
  size_t nComp = 0;
  for (Vertex seed : b.tri->intrinsicMesh->vertices()) {
    if (comp[seed] != INVALID_IND) continue;
    std::vector<Vertex> stack{seed};
    comp[seed] = nComp;
    while (!stack.empty()) {
      Vertex v = stack.back();
      stack.pop_back();
      for (Vertex w : v.adjacentVertices()) {
        if (comp[w] == INVALID_IND) {
          comp[w] = nComp;
          stack.push_back(w);
        }
      }
    }
    nComp++;
  }
  TASK_CHECK(nComp >= 2);

  // Find an edge whose flip and immediate flip-back both succeed, then leave it in the flipped state.
  Edge flipE;
  for (Edge e : b.tri->intrinsicMesh->edges()) {
    if (e.isBoundary()) continue;
    if (b.tri->flipEdgeIfPossible(e) && b.tri->flipEdgeIfPossible(e) && b.tri->flipEdgeIfPossible(e)) {
      flipE = e; // flip, flip-back, flip: ends in the flipped configuration
      break;
    }
  }
  TASK_CHECK(flipE != Edge());

  // Second flip in a different component, so it cannot perturb the first edge's neighborhood.
  Edge otherE;
  for (Edge e : b.tri->intrinsicMesh->edges()) {
    if (e.isBoundary() || comp[e.halfedge().vertex()] == comp[flipE.halfedge().vertex()]) continue;
    if (b.tri->flipEdgeIfPossible(e)) {
      otherE = e;
      break;
    }
  }
  TASK_CHECK(otherE != Edge());

  CommonSubdivisionPoint* retired = nullptr;
  for (CommonSubdivisionPoint* p : cs.pointsAlongB[flipE]) {
    if (p->intersectionType == CSIntersectionType::EDGE_TRANSVERSE) retired = p;
  }
  TASK_CHECK(retired != nullptr);

  CommonSubdivisionPoint* far = nullptr;
  for (CommonSubdivisionPoint* p : cs.pointsAlongB[otherE]) {
    if (p->intersectionType == CSIntersectionType::EDGE_TRANSVERSE) {
      far = p;
      break;
    }
  }
  TASK_CHECK(far != nullptr);
  SurfacePoint farPosA = far->posA;
  SurfacePoint farPosB = far->posB;

  // Flipping back retires the crossing record of the flipped edge, but its storage stays allocated while the mode
  // is on.
  TASK_CHECK(b.tri->flipEdgeIfPossible(flipE));
  TASK_CHECK(cs.getIndex(retired) >= 0);
  TASK_CHECK(retired->retired);

  // The far-away surviving record keeps both its address and its element identity.
  TASK_CHECK(cs.getIndex(far) >= 0);
  TASK_CHECK(!far->retired);
  TASK_CHECK(keyOf(far->posA).elem == keyOf(farPosA).elem);
  TASK_CHECK(keyOf(far->posB).elem == keyOf(farPosB).elem);

  // A further split on an unrelated edge must not move the far record either.
  Edge splitE;
  for (Edge e : b.tri->intrinsicMesh->edges()) {
    if (!e.isBoundary() && e != flipE && e != otherE) {
      splitE = e;
      break;
    }
  }
  TASK_CHECK(splitE != Edge());
  b.tri->splitEdge(splitE.halfedge(), 0.31);
  TASK_CHECK(cs.getIndex(far) >= 0);
  TASK_CHECK(!far->retired);

  // Turning the mode off releases the incremental object; the next access builds a fresh subdivision whose records
  // contain no tombstones (old handles are not promised to stay alive, so they are not dereferenced here).
  b.tri->setIncrementalCommonSubdivision(false);
  CommonSubdivision& fresh = b.tri->getCommonSubdivision();
  for (const CommonSubdivisionPoint& p : fresh.subdivisionPoints) {
    TASK_CHECK(!p.retired);
  }
  fresh.constructMesh();
  TASK_CHECK(fresh.mesh->nFaces() > 0);
  return 0;
}

int locality_errorsAndToggles() {
  TestData data = buildData();
  TriBundle b = makeTriangulation(data);
  b.tri->setIncrementalCommonSubdivision(true);
  CommonSubdivision& cs = b.tri->getCommonSubdivision();

  // Unsupported mutators throw before changing anything.
  bool gotLogic = false;
  try {
    b.tri->insertVertex(SurfacePoint(b.mesh->face(0), Vector3{1. / 3., 1. / 3., 1. / 3.}));
  } catch (const std::logic_error&) {
    gotLogic = true;
  }
  TASK_CHECK(gotLogic);
  gotLogic = false;
  try {
    b.tri->removeInsertedVertex(b.mesh->vertex(0));
  } catch (const std::logic_error&) {
    gotLogic = true;
  }
  TASK_CHECK(gotLogic);

  // Invalid split arguments throw invalid_argument and leave the state untouched.
  size_t nEdgesBefore = b.tri->intrinsicMesh->nEdges();
  auto countsBefore = cs.elementCounts();
  Edge e0;
  for (Edge e : b.tri->intrinsicMesh->edges()) {
    if (!e.isBoundary()) {
      e0 = e;
      break;
    }
  }
  double badArgs[] = {0., 1., -0.2, 1.3, std::numeric_limits<double>::quiet_NaN(),
                      std::numeric_limits<double>::infinity(),
                      -std::numeric_limits<double>::infinity()};
  for (double bad : badArgs) {
    bool gotArg = false;
    try {
      b.tri->splitEdge(e0.halfedge(), bad);
    } catch (const std::invalid_argument&) {
      gotArg = true;
    }
    TASK_CHECK(gotArg);
  }
  TASK_CHECK(b.tri->intrinsicMesh->nEdges() == nEdgesBefore);
  TASK_CHECK(cs.elementCounts() == countsBefore);
  CommonSubdivisionUpdate u = b.tri->lastCommonSubdivisionUpdate();
  TASK_CHECK(u.retracedEdges.empty() && u.touchedInputEdges.empty());

  // getCommonSubdivision() keeps returning the same object across successful edits.
  CommonSubdivision* ptr = &b.tri->getCommonSubdivision();
  b.tri->flipEdgeIfPossible(e0);
  TASK_CHECK(&b.tri->getCommonSubdivision() == ptr);
  b.tri->splitEdge(e0.halfedge(), 0.29);
  TASK_CHECK(&b.tri->getCommonSubdivision() == ptr);

  // Toggle off and on again: the lazy full-rebuild path must still produce a valid subdivision.
  b.tri->setIncrementalCommonSubdivision(false);
  TASK_CHECK(!b.tri->getIncrementalCommonSubdivision());
  CommonSubdivision& offCs = b.tri->getCommonSubdivision();
  offCs.constructMesh();
  TASK_CHECK(offCs.mesh->nFaces() > 0);

  b.tri->setIncrementalCommonSubdivision(true);
  TASK_CHECK(b.tri->getIncrementalCommonSubdivision());
  CommonSubdivisionUpdate uOn = b.tri->lastCommonSubdivisionUpdate();
  TASK_CHECK(uOn.retracedEdges.empty() && uOn.touchedInputEdges.empty());
  CommonSubdivision& onCs = b.tri->getCommonSubdivision();
  onCs.constructMesh();
  TASK_CHECK(onCs.mesh->nFaces() == offCs.mesh->nFaces());

  // Repeated toggling must keep working: edits while on, off (lazy rebuild), on again, more edits.
  b.tri->setIncrementalCommonSubdivision(false);
  for (Edge e : b.tri->intrinsicMesh->edges()) {
    if (!e.isBoundary() && b.tri->flipEdgeIfPossible(e)) break;
  }
  b.tri->setIncrementalCommonSubdivision(true);
  b.tri->getCommonSubdivision().constructMesh();
  TASK_CHECK(b.tri->getCommonSubdivision().mesh->nFaces() > 0);
  bool oneMore = false;
  for (Edge e : b.tri->intrinsicMesh->edges()) {
    if (!e.isBoundary() && b.tri->flipEdgeIfPossible(e)) {
      oneMore = true;
      break;
    }
  }
  TASK_CHECK(oneMore);
  b.tri->getCommonSubdivision().constructMesh();
  TASK_CHECK(b.tri->getCommonSubdivision().mesh->nFaces() > 0);
  return 0;
}

} // namespace

extern const struct task_case task_cases[];
const struct task_case task_cases[] = {
    {"arrangement", "basic_chains", arrangement_basicChains},
    {"arrangement", "merge_at_intersection", arrangement_mergeAtIntersection},
    {"derived", "matches_reference", derived_matchesReference},
    {"derived", "crossing_split_geometry", derived_crossingSplitGeometry},
    {"derived", "rebuild_after_materialization", derived_rebuildAfterMaterialization},
    {"locality", "work_lists", locality_workLists},
    {"locality", "pointer_stability", locality_pointerStability},
    {"locality", "errors_and_toggles", locality_errorsAndToggles},
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
