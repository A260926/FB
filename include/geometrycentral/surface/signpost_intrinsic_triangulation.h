#pragma once

#include "geometrycentral/surface/common_subdivision.h"
#include "geometrycentral/surface/embedded_geometry_interface.h"
#include "geometrycentral/surface/intrinsic_geometry_interface.h"
#include "geometrycentral/surface/intrinsic_triangulation.h"
#include "geometrycentral/surface/manifold_surface_mesh.h"
#include "geometrycentral/surface/surface_point.h"
#include "geometrycentral/utilities/elementary_geometry.h"

#include <unordered_set>
#include <vector>


// An implmementation of the Signpost datastructure from
//   > "Navigating Intrinsic Triangulations". Sharp, Soliman, and Crane. SIGGRAPH 2019


namespace geometrycentral {
namespace surface {

// Summary of the work performed by one incremental common subdivision update.
// Both lists hold unique indices: `retracedEdges` indexes edges of the intrinsic mesh, `touchedInputEdges` indexes
// edges of the input mesh. Calls which change nothing (or are rejected) leave both lists empty. The full build done
// when the incremental mode is enabled is not reported through this structure.
struct CommonSubdivisionUpdate {
  std::vector<size_t> retracedEdges;
  std::vector<size_t> touchedInputEdges;
};

class SignpostIntrinsicTriangulation : public IntrinsicTriangulation {

public:
  // Construct an intrinsic triangulation which sits atop this input mesh. Initially, the input triangulation will
  // just be a copy of the input mesh.
  SignpostIntrinsicTriangulation(ManifoldSurfaceMesh& mesh, IntrinsicGeometryInterface& inputGeom);

  // ======================================================
  // ======== Incremental common subdivision
  // ======================================================

  // Toggle the incremental common subdivision mode. It is off by default, in which case the common subdivision is
  // lazily rebuilt from scratch after every mutation, as usual.
  //
  // When enabled, the CommonSubdivision object handed out by getCommonSubdivision() is kept alive and updated in
  // place after each successful flipEdgeIfPossible(), flipEdgeIfNotDelaunay() or splitEdge() call. Only the traces
  // actually affected by an edit are recomputed; pointers to subdivision records which survive an edit remain
  // valid. Turning the mode off drops any incremental state and restores the lazy rebuild behavior.
  //
  // While the mode is on, insertVertex()/removeInsertedVertex() are not supported and throw std::logic_error, the
  // caller must not modify the intrinsic connectivity directly, and the intrinsic mesh is never compressed.
  void setIncrementalCommonSubdivision(bool enabled);
  bool getIncrementalCommonSubdivision() const;

  // Work performed by the most recent mutating call. Empty (both lists) if the last call was rejected, did nothing,
  // or happened while the incremental mode was off. The one-time full build when enabling the mode is not included.
  CommonSubdivisionUpdate lastCommonSubdivisionUpdate() const;

  // ======================================================
  // ======== Core Members
  // ======================================================

  // Direction of each halfedge, in radians from [0, angleSum]. Importantly, this is different from
  // IntrinsicGeometryInterface's `halfedgeVectorsInVertex`, because it is an _angle only_ measured in radians, and it
  // is _not_ rescaled according to the vertex curvature.
  HalfedgeData<double> signpostAngle;

  // did this edge come from the original triangulation? used mainly for optimizations.
  EdgeData<bool> edgeIsOriginal;

  // ======================================================
  // ======== Queries & Accessors
  // ======================================================

  SurfacePoint equivalentPointOnIntrinsic(const SurfacePoint& pointOnInput) override;

  SurfacePoint equivalentPointOnInput(const SurfacePoint& pointOnIntrinsic) override;

  std::vector<SurfacePoint> traceIntrinsicHalfedgeAlongInput(Halfedge intrinsicHe) override;
  std::vector<SurfacePoint> traceIntrinsicHalfedgeAlongInput(Halfedge intrinsicHe, bool trimEnd);

  std::vector<SurfacePoint> traceInputHalfedgeAlongIntrinsic(Halfedge inputHe) override;
  std::vector<SurfacePoint> traceInputHalfedgeAlongIntrinsic(Halfedge inputHe, bool trimEnd);

  bool checkEdgeOriginal(Edge e) const override;

  // ======================================================
  // ======== Low-Level Mutators
  // ======================================================

  // If the edge is not Delaunay, flip it. Returns true if flipped.
  bool flipEdgeIfNotDelaunay(Edge e) override;

  // If the edge can be flipped, flip it (must be combinatorially flippable and inside a convex quad). Returns true if
  // flipped.
  bool flipEdgeIfPossible(Edge e) override;

  // Flip an edge, where the caller specifies geometric data for the updated edge, rather than it being computed. Must
  // be flippable. Experts only.
  void flipEdgeManual(Edge e, double newLength, double forwardAngle, double reverseAngle, bool isOrig,
                      bool reverseFlip = false);

  // Insert a new vertex in to the intrinsic triangulation
  Vertex insertVertex(SurfacePoint newPositionOnIntrinsic) override;

  // Remove an (inserted) vertex from the triangulation.
  // Note: if something goes terribly (numerically?) wrong, will exit without removing the vertex.
  Face removeInsertedVertex(Vertex v) override;

  // Split an edge
  Halfedge splitEdge(Halfedge he, double tSplit) override;

protected:
private:
  // ======================================================
  // ======== Geometry Interface
  // ======================================================
  //
  // Satisfy the requirements of the IntrinsicGeometryInterface

  // Override the halfedge vectors method from intrinsic geometry
  virtual void computeHalfedgeVectorsInVertex() override;


  // ======================================================
  // ======== Helpers
  // ======================================================

  // Insertion helpers
  Vertex insertVertex_face(SurfacePoint newPositionOnIntrinsic);
  Halfedge insertVertex_edge(SurfacePoint newPositionOnIntrinsic);
  void resolveNewVertex(Vertex newV, SurfacePoint intrinsicPoint);

  // Update a signpost angle from the (counter-)clockwise neighboring angle
  void updateAngleFromCWNeighor(Halfedge he);

  // Map angle to range [0, angleSum)
  double standardizeAngle(Vertex vert, double angle) const;

  // Get vector in rescaled vertex coordinates (either halfedge or arbitrary vector)
  Vector2 halfedgeVector(Halfedge he) const;
  Vector2 rescaledVertexVector(Vertex v, double angle, double len) const;

  // Scale factor to take Euclidean data to cone data
  double vertexAngleScaling(Vertex v) const;

  // Construct the common subdivision for the current triangulation.
  // WARNING: this signpost implementation does not properly populate some fields
  // of the common subdivision, in particular the `orientation` field for intersections.
  void constructCommonSubdivision() override;

  // ======================================================
  // ======== Incremental common subdivision internals
  // ======================================================

  bool incrementalMode = false;
  CommonSubdivisionUpdate lastUpdate;

  // Reset the per-call work list to empty.
  void clearLastUpdate();

  // Make sure commonSubdivision exists while the incremental mode is on. The first construction after enabling the
  // mode is a full build, whose work is not reported in lastCommonSubdivisionUpdate().
  void ensureIncrementalCommonSubdivision();

  // Detach every interior record along intrinsic edge e from the intrinsic-side chain, and detach the crossing
  // records owned by the edge from the input-side chains as well. Intrinsic-vertex records (EDGE_VERTEX) are left on
  // the input chain, because they may still be referenced by other edges. Detached crossing records stay alive in
  // subdivisionPoints (marked retired) until the mode is turned off, and are handed back through detachedWhich for
  // reuse within the same edit.
  void detachEdgeRecords(Edge e, std::unordered_set<size_t>& touchedInput,
                         std::vector<CommonSubdivisionPoint*>& detachedWhich);

  // Build the intrinsic-side chain of e from a fresh trace, reusing the edge's own surviving crossing records and
  // creating new ones for anything else. tailRec/tipRec are the endpoint subdivision records. Every input edge the
  // trace meets is added to touchedInput.
  void buildEdgeChain(Edge e, CommonSubdivisionPoint* tailRec, CommonSubdivisionPoint* tipRec,
                      std::unordered_set<size_t>& touchedInput,
                      std::vector<CommonSubdivisionPoint*>& detachedWhich);

  // Find the subdivision record representing intrinsic vertex vB, scanning the endpoint slots of its incident edge
  // chains.
  CommonSubdivisionPoint* findIntrinsicVertexRecord(Vertex vB);

  // Create (or merge with an existing record at the same input location) the subdivision record for a newly inserted
  // intrinsic vertex. The record is attached to the input-side chain, but not yet to any intrinsic edge chain.
  // Records in detachedWhich were retired earlier in the same edit are eligible for reuse, so splitting exactly at an
  // existing intersection yields a single merged record at the stable address.
  CommonSubdivisionPoint* createIntrinsicVertexRecord(Vertex vB, std::unordered_set<size_t>& touchedInput,
                                                      std::vector<CommonSubdivisionPoint*>& detachedWhich);

  // Find an existing non-endpoint record along input edge eA whose posA.tEdge is within the merge tolerance.
  CommonSubdivisionPoint* findRecordOnInputEdge(Edge eA, double tEdge, bool includeEdgeVertices);

  // Locally recover EDGE_PARALLEL records on every touched input edge, mirroring the shared-edge detection of a full
  // build without scanning any other input edge.
  void recoverParallelEdges(const std::unordered_set<size_t>& touchedInput);

  // Incremental edit implementations. They assume the incremental mode is on and the common subdivision is populated.
  void incrementallyUpdateFlip(Edge e);
  // tSplit is the edge-oriented split parameter on oldEdge, used to detect splits landing exactly on an existing
  // intersection record.
  void incrementallyUpdateSplit(Edge oldEdge, Halfedge newHeFront, Halfedge newHeBack, Vertex newV, double tSplit);

  // Finish an update: invalidate derived connectivity and publish the unique work lists.
  void publishUpdate(const std::unordered_set<size_t>& touchedInput, const std::vector<Edge>& retracedEdges);

  // Insert p into the input-side edge chain, keeping the chain ordered by posA.tEdge and leaving the endpoint
  // records in place.
  static void insertOrderedOnA(CommonSubdivision& cs, Edge eA, CommonSubdivisionPoint* p);

  // Remove a record from one chain.
  static void detachFromChain(std::vector<CommonSubdivisionPoint*>& chain, CommonSubdivisionPoint* p);
};


} // namespace surface
} // namespace geometrycentral


#include "geometrycentral/surface/signpost_intrinsic_triangulation.ipp"
