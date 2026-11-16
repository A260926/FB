#ifndef DEMO_SKIN_SURFACE_H
#define DEMO_SKIN_SURFACE_H

// Continuous skinned surface mode for IQM.
//
// The classic IQM demo only transforms the original vertices, so every face
// stays flat.  A skin surface instead treats each bound triangle as a
// parametric patch: position and bone weights are interpolated inside the
// triangle and the *current frame* palette is applied to the interpolated
// data, i.e.
//
//   p(b)   = sum_a b_a * p_a                         (bind position)
//   w_j(b) = sum_a b_a * w_aj                        (merged bone weights)
//   F(b)   = sum_j w_j(b) * M_j * (p(b), 1)
//
// where b = (b0,b1,b2) are barycentric coordinates and M_j is the palette
// matrix already computed by animateiqm() (outframe[]).  No inverse bind
// matrix is applied a second time, and the three skinned corner positions are
// never blended together as a substitute for F.
//
// F is a quadratic polynomial over each bound triangle, so a conservative
// (whole closed parameter triangle, not sample based) deviation bound can be
// derived from its quadratic control net.  The patch is adaptively subdivided
// with conforming edge bisection until every output triangle satisfies the
// chord tolerance, the depth/triangle budgets are exhausted (overall failure,
// previous output retained), or a numerical limit is hit.

#include <vector>
#include <cstddef>
#include "util.h"
#include "geom.h"
#include "iqm.h"

struct SkinSurfaceOptions
{
    double chordTolerance = 1e-3;     // strictly positive, finite
    unsigned int maxDepth = 12;       // non-negative bisection depth
    unsigned int maxTriangles = 1u << 20; // strictly positive output budget
};

// One output parameter triangle.  vertex[] stores three indices into the
// shared SkinSurfaceMesh::positions array; nodes on a shared original edge use
// the same index on both sides.  sourceTriangle is the index of the original
// IQM triangle the patch was generated from.
struct SkinSurfaceTriangle
{
    unsigned int vertex[3];
    unsigned int sourceTriangle;
    float errorBound;
};

// Per-corner data (three entries per output triangle, in triangle order).
// Barycentric coordinates are relative to the source original triangle, the
// normal is the analytic normalize(dF/du x dF/dv), and uv is the linear
// interpolation of the original corner UVs.  Corners deliberately keep
// per-face UVs and normals even where a position is shared.
struct SkinSurfaceCorner
{
    float barycentric[3];
    float unitNormal[3];
    float uv[2];
};

// Material segment: a contiguous run of output triangles corresponding to one
// original iqmmesh triangle range, in iqmmesh order, so the renderer can keep
// using its original texture/material per segment.
struct SkinSurfaceRange
{
    unsigned int firstTriangle;
    unsigned int numTriangles;
};

// Read-only snapshot of the most recent successful evaluation.  A new snapshot
// is built in private scratch storage and only published on full success, so
// a failed evaluation never mixes new positions with old indices/ranges.
struct SkinSurfaceMesh
{
    std::vector<Vec3> positions;             // shared, deduplicated nodes
    std::vector<SkinSurfaceTriangle> triangles;
    std::vector<SkinSurfaceCorner> corners;  // exactly 3 per triangle
    std::vector<SkinSurfaceRange> materialRanges; // one entry per iqmmesh
    unsigned int numSourceTriangles = 0;
    unsigned long long sequence = 0;         // bumped on every successful publish

    bool empty() const { return triangles.empty(); }
};

struct SkinSurfaceBinding;

// Creates a binding that owns snapshots of the bind pose data.  Returns false
// (leaving *out untouched) on null/non-finite data, out-of-range vertex/bone/
// material indices, degenerate input faces, weights that do not sum to 255 or
// malformed material ranges.  The byte weights are divided by 255 and slots
// addressing the same bone are merged; zero-weight slots do not participate.
bool createSkinSurfaceBinding(SkinSurfaceBinding *&out,
                              const float *positions, const float *texcoords,
                              const uchar *blendIndex, const uchar *blendWeight,
                              int numVerts, int numJoints,
                              const iqmtriangle *triangles, int numTriangles,
                              const iqmmesh *meshes, int numMeshes);
void destroySkinSurfaceBinding(SkinSurfaceBinding *&binding);

// Re-evaluates the surface for one frame.  palette must contain numJoints
// finite, invertible affine matrices (non-uniform scale is allowed); these
// are the outframe matrices and are NOT recombined with inverse-bind matrices.
// On any failure (bad palette, illegal options, tolerance unachievable within
// maxDepth/maxTriangles, degenerate sampled derivative, non-finite result) the
// previously published snapshot is retained and false is returned.
bool evaluateSkinSurface(SkinSurfaceBinding *binding,
                         const Matrix3x4 *palette, int numJoints,
                         const SkinSurfaceOptions &options);

// Latest successful snapshot for a binding (null before the first success).
// Remains valid until the next successful evaluation or binding destruction.
const SkinSurfaceMesh *getSkinSurfaceDrawMesh(const SkinSurfaceBinding *binding);
const char *getSkinSurfaceError(const SkinSurfaceBinding *binding);

// ---- Global demo integration (used by animateiqm/renderiqm) ----

void setSkinSurfaceEnabled(bool enabled);
bool getSkinSurfaceEnabled();
void setSkinSurfaceOptions(const SkinSurfaceOptions &options);
const SkinSurfaceOptions &getSkinSurfaceOptions();

// Latest successful snapshot of the demo's active binding (null if none).
// This is the read-only draw mesh consumed by renderiqm.
const SkinSurfaceMesh *getSkinSurfaceDrawMesh();
const char *getSkinSurfaceMessage();

// Hooks invoked by the IQM loader / animation entry points.
bool skinSurfaceLoad(const float *positions, const float *texcoords,
                     const uchar *blendIndex, const uchar *blendWeight,
                     int numVerts, int numJoints,
                     const iqmtriangle *triangles, int numTriangles,
                     const iqmmesh *meshes, int numMeshes);
void skinSurfaceAnimate(const Matrix3x4 *palette, int numJoints);
void skinSurfaceUnload();

#endif
