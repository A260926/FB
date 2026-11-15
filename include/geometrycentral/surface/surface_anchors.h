#pragma once

#include "geometrycentral/surface/manifold_surface_mesh.h"
#include "geometrycentral/surface/mutation_manager.h"
#include "geometrycentral/surface/surface_point.h"
#include "geometrycentral/surface/vertex_position_geometry.h"

#include <array>
#include <cstdint>
#include <list>
#include <vector>

namespace geometrycentral {
namespace surface {

// A single measurement marker registration.
//   id:          non-zero, unique within one SurfaceAnchorSet
//   point:       a vertex / edge / face point on the mesh the manager is bound to
//   maxDistance: non-negative finite budget for the Euclidean distance of the marker to its
//                immutable target while the mesh is simplified
struct SurfaceAnchorInput {
  uint64_t id = 0;
  SurfacePoint point;
  double maxDistance = 0.;
};

// An owning, by-id-sorted snapshot of the current state of one marker. The barycentric weights
// follow the winding of the face's current representative halfedge, i.e. the order of
// face.adjacentVertices().
struct SurfaceAnchorSnapshot {
  uint64_t id = 0;
  Vector3 target{0., 0., 0.};
  double maxDistance = 0.;
  Face face;
  Vector3 barycentric{0., 0., 0.};
  Vector3 position{0., 0., 0.};
};

/*
 * SurfaceAnchorSet
 * ----------------
 *
 * Keeps a set of surface measurement markers attached to the same piece of surface while a
 * MutationManager performs halfedge collapses (in particular, quadricErrorSimplify()).
 *
 * Each marker stores an immutable 3D target taken from the initial geometry, a distance budget,
 * and its current attachment as a (face, barycentric) pair. When a collapse is requested through
 * the bound manager, the set first simulates the collapse on the local patch (the union of the
 * faces incident on either endpoint):
 *
 *   - faces containing both endpoints disappear,
 *   - every other patch face replaces either endpoint by the surviving vertex,
 *   - each marker attached to the patch is re-projected from its immutable target onto the
 *     surviving candidate triangles only (never onto another component or a distant patch),
 *   - the candidate is rejected (with no mesh/geometry/anchor mutation and without firing the
 *     manager's collapse observers) if the topology is illegal, a surviving triangle degenerates
 *     or flips, or any marker would exceed its budget.
 *
 * Only collapseEdge() calls made through the bound manager are tracked. Direct topology edits,
 * splits, flips and repositions are not. Explicit mesh.compress() calls between simplifications
 * are handled: face provenance, face handles and the barycentric winding are repaired.
 *
 * A manager binds at most one set at a time, the set is non-copyable, and its destructor
 * unregisters all of its callbacks without touching the manager's other constraints or policies.
 */
class SurfaceAnchorSet {

public:
  // Throws std::invalid_argument on any illegal input, before any callback is registered.
  SurfaceAnchorSet(MutationManager& manager_, const std::vector<SurfaceAnchorInput>& inputs);

  // Unregisters every mesh callback and detaches from the manager.
  ~SurfaceAnchorSet();

  SurfaceAnchorSet(const SurfaceAnchorSet&) = delete;
  SurfaceAnchorSet& operator=(const SurfaceAnchorSet&) = delete;
  SurfaceAnchorSet(SurfaceAnchorSet&&) = delete;
  SurfaceAnchorSet& operator=(SurfaceAnchorSet&&) = delete;

  // Owned copy of the marker states, sorted by id.
  std::vector<SurfaceAnchorSnapshot> snapshot() const;

  // Counters from the most recent simplification (reset by quadricErrorSimplify(), and
  // available for direct collapseEdge() experiments as well).
  uint64_t anchorVisits() const { return anchorVisitCount; }
  uint64_t projectionTriangleTests() const { return projectionTriangleTestCount; }
  void resetCounters();

  // Constraint-layer reporting channel used by the QEM scheduler: after a successful collapse
  // every edge whose anchor feasibility may have changed is marked dirty; takeDirtyEdges()
  // appends the still-live dirty edges to `out` and clears the marks.
  void takeDirtyEdges(std::vector<Edge>& out);

  MutationManager& manager;

private:
  struct Anchor {
    uint64_t id = 0;
    Vector3 target{0., 0., 0.};
    double maxDistance = 0.;
    Face face;
    Vector3 bary{0., 0., 0.};      // weights in face's current halfedge winding
    std::array<Vertex, 3> verts{}; // vertices of the face, matching `bary`
  };

  struct SimTriangle {
    Face face;
    size_t origin = 0;
    std::array<Vertex, 3> verts{};
    std::array<Vector3, 3> pos{};
  };

  struct TentativeAnchor {
    size_t anchorIndex = 0;
    Face face;
    std::array<Vertex, 3> verts{};
    Vector3 bary{0., 0., 0.};
    Vector3 position{0., 0., 0.};
    double distance = 0.;
  };

  ManifoldSurfaceMesh& mesh;
  VertexPositionGeometry* geometry = nullptr;

  std::vector<Anchor> anchors; // always sorted by id
  FaceData<std::vector<size_t>> faceIndex;
  FaceData<size_t> faceOrigin;
  EdgeData<unsigned char> edgeDirty;
  std::vector<Edge> pendingDirtyList;

  // State of the candidate currently being evaluated, published by commitCollapse().
  std::vector<TentativeAnchor> tentative;
  Vertex pendingSurvivor;

  uint64_t anchorVisitCount = 0;
  uint64_t projectionTriangleTestCount = 0;

  // mesh.compress() bookkeeping
  std::list<std::function<void(const std::vector<size_t>&)>>::iterator facePermuteCallbackIt;
  std::list<std::function<void(const std::vector<size_t>&)>>::iterator vertexPermuteCallbackIt;
  std::list<std::function<void(void)>>::iterator compressCallbackIt;

  // Called by MutationManager::collapseEdge() before any observer runs. Returns false to reject
  // the candidate; a rejected call mutates nothing.
  bool evaluateCollapse(Edge e, double tCollapse, const Vector3& newVertexPosition);

  // Called by MutationManager::collapseEdge() after the mesh collapse and the position write,
  // but before the user-visible after-edge-collapse observers.
  void commitCollapse();

  void markEdgeDirty(Edge e);

  friend class MutationManager;
};

} // namespace surface
} // namespace geometrycentral
