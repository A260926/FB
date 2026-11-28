#ifndef CC_UNSUBDIVIDE_H
#define CC_UNSUBDIVIDE_H

/*
 * cc_Unsubdivide.h
 *
 * Catmull-Clark "inverse subdivision": recover an editable polygon control
 * cage from one Catmull-Clark subdivision step of an unknown coarse mesh.
 *
 * ---------------------------------------------------------------------------
 * Scope and assumptions
 * ---------------------------------------------------------------------------
 * The input cc_Mesh must be:
 *   - a single connected, closed, oriented combinatorial 2-manifold;
 *   - made only of quads;
 *   - free of creases (no edge has non-zero sharpness) and free of UVs
 *     (uvCount must be 0);
 *   - a valid one-step Catmull-Clark image of some coarse mesh for which the
 *     caller knows which input vertices are the successors ("original vertex
 *     points") of the coarse vertices.
 *
 * The caller supplies one "origin vertex": any input vertex that is the
 * successor of a coarse vertex.  This marker selects, among every legal
 * inverse solution, the one whose topology is compatible with it.  The full
 * export history is not guessed: it is the marker that decides which of the
 * (up to four) equally valid parent topologies is returned.
 *
 * Input meshes whose topology is not a one-step Catmull-Clark image (for
 * example faces of degree other than four, broken twins, disconnected vertex
 * fans, and so on) are rejected with CC_UNSUBDIVIDE_NOT_SUBDIVISION.
 * Malformed arguments and unrepresentable values are rejected with
 * CC_UNSUBDIVIDE_INVALID_INPUT.  Neither case produces a result and the input
 * mesh is never modified.
 *
 * ---------------------------------------------------------------------------
 * Ownership and result lifetime
 * ---------------------------------------------------------------------------
 * Every successful call returns a freshly allocated result that owns:
 *   - `cage`: a newly allocated cc_Mesh.  It is independent of the input; it
 *     can be handed directly to ccs_Create(), ccm_Save(), or any other part
 *     of the library, and it remains valid after the input mesh is released.
 *   - the mapping arrays (parentFaceOfSubface, subfaceCornerOfSubface, ...).
 * Release the whole result exactly once with ccu_ReleaseResult().
 *
 * The input cc_Mesh is read-only and is never modified.  On failure
 * `outResult` receives the failing status and a NULL `cage` (when `outResult`
 * is non-NULL); no partial cage is ever published.
 *
 * ---------------------------------------------------------------------------
 * Numbering of the recovered cage
 * ---------------------------------------------------------------------------
 * Vertex classes of the input (decided from adjacency only, never from the
 * caller's indices):
 *   - ORIGIN  : successor of a coarse vertex.  Its valence equals the coarse
 *               valence (>= 3) and all its neighbours are edge points.
 *   - EDGE    : successor of a coarse edge point.  Always valence 4; it sees
 *               exactly two ORIGIN and two FACE neighbours.
 *   - FACE    : successor of a coarse face point.  Its valence equals the
 *               coarse face degree (>= 3); all its neighbours are edge
 *               points.
 *
 * Coarse vertices:
 *   numbered in ascending order of the input vertex index they are the
 *   successor of (i.e. ascending input index of the ORIGIN vertices).
 *
 * Coarse faces:
 *   numbered in ascending order of the input vertex index they are the
 *   successor of (i.e. ascending input index of the FACE vertices).
 *
 * Coarse face loops:
 *   each parent loop starts at its smallest-numbered coarse vertex and then
 *   follows the input orientation.  It may have 3, 4, or more corners.
 *
 * Coarse edges and halfedges (deterministic, documented ordering):
 *   - Coarse edges are numbered in lexicographic order of the ordered pair
 *     (min coarse vertex ID, max coarse vertex ID) of their two endpoints.
 *   - Coarse halfedges are grouped by coarse face, in coarse-face number
 *     order.  Inside a face they start at corner 0 (the smallest coarse
 *     vertex) and follow the parent loop.  The halfedge ID of corner k of
 *     face f is therefore  sum(deg(0..f-1)) + k.
 *   - Halfedge h has `vertexID` equal to the coarse vertex at corner h of
 *     its face loop; `nextID` reaches the following corner and `prevID` the
 *     preceding one.  `twinID` links the two halfedges of opposite
 *     orientation over the same coarse edge.
 *   - `vertexToHalfedgeIDs[v]` is the halfedge (of the lowest-numbered
 *     incident coarse face) whose `vertexID` is v; `faceToHalfedgeIDs[f]` is
 *     corner 0;
 *     `edgeToHalfedgeIDs[e]` is the halfedge of lowest ID.
 *   - All creases have zero sharpness (the coarse cage has no creases).
 *
 * ---------------------------------------------------------------------------
 * Per-input-vertex and per-input-face reports
 * ---------------------------------------------------------------------------
 * vertexClass[k] is one of CC_UNSUBDIVIDE_CLASS_{ORIGIN,EDGE,FACE}.
 * vertexParent[k] is, for an ORIGIN vertex, the coarse vertex ID; for an EDGE
 * vertex, the parent coarse edge ID; for a FACE vertex, the parent coarse
 * face ID.
 *
 * parentFaceOfSubface[q] is the coarse face whose image contains input quad
 * q.  subfaceCornerOfSubface[q] is the corner (0-based, in the recovered
 * parent loop) of that parent face: input quad q is the child at that corner
 * (the child that touches the parent corner and is bounded by the two parent
 * edges meeting there).  Together the map is a bijection between the input
 * quads and (parent face, corner) pairs.
 *
 * ---------------------------------------------------------------------------
 * Geometry: the inverse solve
 * ---------------------------------------------------------------------------
 * The ORIGIN, EDGE and FACE input vertices are already smoothed positions;
 * they are *not* the coarse control points.  We therefore build the same
 * one-step, no-crease Gather operator A used by the upstream library
 * (ccs_Refine_NoCreases_Gather) for the recovered topology, and solve the
 * least-squares problem
 *
 *        min_X || A X - Y ||^2
 *
 * where every input vertex of the mesh contributes one row, X are the coarse
 * control points, and Y are the input positions.  The solve is done entirely
 * in double.
 *
 * A is built analytically from the recovered topology, exactly the linear
 * weights used by the upstream no-crease Gather rules:
 *   face point   = mean of its parent corners;
 *   edge point   = (v0 + v1 + F0 + F1) / 4;
 *   vertex point = (Q + 2R + (n - 3)P) / n.
 * It is stored row-wise as a sparse coefficient list (one row per input
 * vertex) and never materialised densely: every product is a matrix-free
 * sparse traversal, so memory is O(nnz(A)), never O(V^2).  No control point
 * is perturbed to produce a column and no dense coarse-sized matrix is
 * allocated.  The same linear operator, applied by the native Gather code
 * path, is used afterwards to verify the candidate cage.
 *
 * Iteration: conjugate gradients on the normal equations
 *        A^T A X = A^T Y,
 * started from X = 0; the true residual
 *        || A^T (A X - Y) ||_inf <= 1e-10 * max(1, || A^T Y ||_inf)
 * is re-evaluated at every iterate (with restarts from the true residual if
 * the recursive CG quantity drifts).  If `maxIterations` iterates are used
 * before the test is met the solve reports CC_UNSUBDIVIDE_SOLVER_LIMIT.
 *
 * After a converged solve the candidate cage is actually refined with the
 * native one-step Gather operator, every vertex point and every child-face
 * adjacency is checked through the parent/child map, and the maximum point
 * distance decides success or CC_UNSUBDIVIDE_RESIDUAL_EXCEEDED.  A candidate
 * cage is never published on failure.
 *
 * ---------------------------------------------------------------------------
 * Status codes
 * ---------------------------------------------------------------------------
 *   CC_UNSUBDIVIDE_OK
 *   CC_UNSUBDIVIDE_INVALID_INPUT        bad pointer, count, index, marker,
 *                                       non-finite/out-of-range coordinate,
 *                                       crease, UV, null output slot.
 *   CC_UNSUBDIVIDE_NOT_SUBDIVISION      topology or geometry that cannot be
 *                                       a single Catmull-Clark step.
 *   CC_UNSUBDIVIDE_SOLVER_LIMIT         iteration cap reached before the
 *                                       normal-equation residual test passed.
 *   CC_UNSUBDIVIDE_RESIDUAL_EXCEEDED    converged solve, but the forward
 *                                       refinement does not match the input
 *                                       within the caller's tolerance.
 */

#include <stdint.h>

#include "CatmullClark.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum cc_UnsubdivideStatus {
    CC_UNSUBDIVIDE_OK                 = 0,
    CC_UNSUBDIVIDE_INVALID_INPUT      = 1,
    CC_UNSUBDIVIDE_NOT_SUBDIVISION    = 2,
    CC_UNSUBDIVIDE_SOLVER_LIMIT       = 3,
    CC_UNSUBDIVIDE_RESIDUAL_EXCEEDED  = 4
} cc_UnsubdivideStatus;

typedef enum cc_UnsubdivideVertexClass {
    CC_UNSUBDIVIDE_CLASS_ORIGIN = 0,
    CC_UNSUBDIVIDE_CLASS_EDGE   = 1,
    CC_UNSUBDIVIDE_CLASS_FACE   = 2
} cc_UnsubdivideVertexClass;

typedef struct cc_UnsubdivideResult {
    /* Status of the operation; CC_UNSUBDIVIDE_OK iff `cage` is non-NULL. */
    cc_UnsubdivideStatus status;

    /* Recovered coarse control cage; owned by this result. */
    cc_Mesh *cage;

    /* Input mesh sizes, repeated for convenience. */
    int32_t inputVertexCount;
    int32_t inputFaceCount;
    int32_t inputHalfedgeCount;

    /* Recovered cage sizes. */
    int32_t coarseVertexCount;
    int32_t coarseFaceCount;
    int32_t coarseEdgeCount;
    int32_t coarseHalfedgeCount;

    /* One entry per input vertex. */
    int32_t *vertexClass;      /* cc_UnsubdivideVertexClass */
    int32_t *vertexParent;     /* coarse vertex/edge/face ID, by class */

    /* One entry per input face. */
    int32_t *parentFaceOfSubface;
    int32_t *subfaceCornerOfSubface;

    /* Solver report (valid when a solve was attempted). */
    int64_t iterations;         /* actual normal-equation iterations */
    int64_t operatorProducts;   /* applications of the A / A^T operators */
    int64_t nonzeroCoefficients;/* non-zero entries of the sparse operator A */
    double  maxForwardResidual; /* max |A X - Y|_inf after the final forward */
    double  tolerance;          /* caller-supplied geometric tolerance */
} cc_UnsubdivideResult;

/*
 * Recover the coarse cage.
 *
 * mesh            read-only input (must not be NULL).
 * originVertex    index of an input vertex that succeeds a coarse vertex.
 * tolerance       finite, > 0: maximum admissible point distance.
 * maxIterations   > 0: hard cap on normal-equation iterations.
 * outResult       non-NULL; receives the owned result on success, or a
 *                 status-only result (cage == NULL) on failure.
 *
 * Returns the same value stored in outResult->status.
 */
cc_UnsubdivideStatus ccu_Unsubdivide(const cc_Mesh *mesh,
                                     int32_t originVertex,
                                     double tolerance,
                                     int32_t maxIterations,
                                     cc_UnsubdivideResult *outResult);

/* Release a result (cage, mappings and report). Safe on NULL. */
void ccu_ReleaseResult(cc_UnsubdivideResult *result);

/* Human-readable status name (never NULL). */
const char *ccu_StatusName(cc_UnsubdivideStatus status);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CC_UNSUBDIVIDE_H */
