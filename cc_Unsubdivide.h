#ifndef CC_INCLUDE_UNSUBDIVIDE_H
#define CC_INCLUDE_UNSUBDIVIDE_H

//
// cc_Unsubdivide -- one-level inverse Catmull-Clark subdivision
//
// This module reconstructs the polygon control cage of a mesh that is the
// result of exactly one crease-free Catmull-Clark subdivision step (the same
// rules implemented by ccs_Refine_NoCreases_Gather in CatmullClark.h).
//
// Supported input
// ---------------
//   * one connected, closed, oriented, combinatorial 2-manifold;
//   * every face is a quadrilateral;
//   * no boundary edges, no (semi-)sharp creases, no UV data (uvCount == 0);
//   * finite vertex coordinates in [-10, 10];
//   * the caller marks exactly the child vertices that descend from original
//     coarse vertices ("original vertex successors") through `markedVertex`.
//
// Nothing about exported vertex / face / edge / halfedge numbering or the
// corner at which a face's halfedge loop starts is assumed: all topology is
// recovered from adjacency alone.
//
// Results
// -------
// On CC_UNSUBDIVIDE_OK the result owns an independent cc_Mesh (allocated with
// the kernel's ccm_Create, releasable with ccm_Release and directly usable
// with ccs_Create / ccm_Save).  The input mesh is never modified.  On any
// failure no mesh is published and `*outResult` is left untouched.
//
// Numbering of the recovered coarse mesh (deterministic)
// ------------------------------------------------------
//   * coarse vertex i corresponds to the i-th marked child vertex in ascending
//     child-vertex index order;
//   * coarse face i corresponds to the i-th child face-point vertex (vertices
//     that are neither marked nor edge successors) in ascending child index;
//   * each coarse face's vertex loop starts at its smallest coarse vertex and
//     follows the orientation of the input mesh;
//   * coarse halfedges are emitted in one contiguous run per coarse face, in
//     coarse-face order, following that face's loop (halfedge
//         faceBase[f] + k
//     leaves coarse vertex loop[f][k] and points at loop[f][(k+1)%n]);
//   * coarse edges are numbered by the first halfedge that uses them in that
//     emission order, and edgeToHalfedgeIDs stores that halfedge.
//
// Mappings (child -> parent, all indices refer to the input mesh)
// ----------------------------------------------------------------
//   vertexClass[v]:  0 = original-vertex successor,
//                    1 = original-edge successor,
//                    2 = original-face successor;
//   vertexParent[v]: coarse vertex index (class 0), coarse edge index
//                    (class 1) or coarse face index (class 2);
//   childParentFace[q]: coarse face containing child quad q;
//   childParentCorner[q]: which corner of that coarse face (0-based, in the
//                    coarse face's emitted loop) quad q is anchored at.
//   Every (parent face, corner) pair is hit exactly once.
//
// Geometry
// --------
// The smoothed input positions are inverted by least squares: with A the
// linear one-level crease-free Gather operator built from the recovered
// topology and Y the input positions, solve  min ||A X - Y||^2  in double
// precision using sparse matrix-vector products only (the number of stored
// non-zeros is linear in the mesh size; no dense N^2 matrix is formed).
// Iteration stops when
//     ||A^T (A X - Y)||_inf <= 1e-10 * max(1, ||A^T Y||_inf).
// The candidate cage is then re-subdivided with the production Gather path
// and accepted only if the maximum point distance to the input is within the
// caller supplied tolerance.
//
// Ownership / state
// -----------------
// The result mesh is independent of the input: releasing the input before or
// after the result is valid.  Releasing a result with ccm_Release frees only
// the mesh; cc_UnsubdivideResult_Release also frees the mapping arrays and
// the wrapper.  Repeated calls do not share state.
//

#include "CatmullClark.h"
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CC_UNSUBDIVIDE_OK = 0,
    CC_UNSUBDIVIDE_INVALID_INPUT = 1,   // malformed arrays/indices/topology,
                                        // boundary/crease/UV, bad marker set
    CC_UNSUBDIVIDE_NOT_SUBDIVISION = 2, // quad mesh cannot be one CC level
    CC_UNSUBDIVIDE_SOLVER_LIMIT = 3,    // iteration cap reached before the
                                        // normal-equations stopping test
    CC_UNSUBDIVIDE_RESIDUAL_EXCEEDED = 4// solved cage does not reproduce the
                                        // input within the given tolerance
} cc_UnsubdivideStatus;

typedef struct {
    cc_Mesh *mesh;                 // independent recovered coarse cage
    int32_t *vertexClass;          // length inputVertexCount, values 0/1/2
    int32_t *vertexParent;         // length inputVertexCount
    int32_t *childParentFace;      // length inputFaceCount
    int32_t *childParentCorner;    // length inputFaceCount
    int32_t  coarseVertexCount;
    int32_t  coarseEdgeCount;
    int32_t  coarseFaceCount;
    int32_t  iterations;           // solver iterations actually performed
    int32_t  operatorProducts;    // count of sparse A / A^T applications
    int64_t  nonZeroCount;         // stored non-zeros of A
    double   maxForwardResidual;   // max point distance after forward Gather
} cc_UnsubdivideResult;

// Release the mapping arrays, the owned coarse mesh (via ccm_Release) and the
// wrapper itself.  Safe on a zero-initialized struct.
void cc_UnsubdivideResult_Release(cc_UnsubdivideResult *result);

// Run one level of inverse subdivision.
//
//   mesh        read-only input quad mesh (never modified);
//   markedVertex    array of length markedVertexCount; entries are input
//                   vertex indices claimed to be original-vertex successors;
//   tolerance    strictly positive geometric tolerance (same units as the
//                   input coordinates);
//   maxIterations strictly positive solver iteration cap;
//   outResult    on CC_UNSUBDIVIDE_OK receives a newly allocated result.
//
// Returns an cc_UnsubdivideStatus value.  On failure *outResult is not
// touched and nothing is allocated that survives the call.
cc_UnsubdivideStatus cc_Unsubdivide(
    const cc_Mesh *mesh,
    const int32_t *markedVertex,
    int32_t markedVertexCount,
    double tolerance,
    int32_t maxIterations,
    cc_UnsubdivideResult **outResult);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // CC_INCLUDE_UNSUBDIVIDE_H
