// Constrained triangulation of two NURBS surfaces along their true
// intersection curves.
//
// imprint_surfaces() traces the intersection with the native SISL tracer
// (sisl_trace_grid_events, which reuses the real s1859 discovery and the
// real s1310 marcher), then triangulates every parameter cell of both
// surfaces under the constraints that
//
//  * every input grid line of both surfaces,
//  * every intersection segment (the shared stations of both surfaces),
//
// is a real mesh edge in both sheets.  Stations that only one surface's grid
// creates are propagated to the other surface as split points, so the two
// triangulations share the intersection edge chain exactly.
//
// Layout of the result (all indices refer to `positions`):
//  * positions   : owned, shared 3-D positions.
//  * triangles   : sheet (0 or 1), three position indices, the original
//                  domain UV at each corner and the unit normal at each
//                  corner (analytic surface normals; the two sheets keep
//                  their own values, they are never averaged).
//  * polylines   : per intersection branch, the ordered position indices and
//                  a closed flag.  The last index of a closed branch is not
//                  a repeat of the first.
//
// Positions are shared only between a surface's own parameter identity and
// the common intersection stations; unrelated coincident points are never
// welded.

#ifndef SISL_IMPRINT_HPP
#define SISL_IMPRINT_HPP

#include <array>
#include <cstddef>
#include <stdexcept>
#include <vector>

#include "sisl.h"
#include "sisl_trace_events.h"

namespace sislmesh
{

// Options of one imprint_surfaces() call.
struct ImprintOptions
{
    // Positive, finite length tolerance.  Back-substituted vertices must be
    // within this of their surface and the intersection polyline chord error
    // is held within it by the native tracer.
    double geometricTolerance = 1e-4;
    // Positive upper bound on the total number of stations over all branches.
    int maxStations = 100000;
    // Positive upper bound on the total number of triangles of the whole call
    // (both sheets).
    int maxTriangles = 2000000;
};

struct ImprintTriangle
{
    int sheet = -1;          // 0 or 1
    int index[3] = {-1, -1, -1};   // into positions
    double uv[3][2] = {};          // original domain parameters per corner
    double normal[3][3] = {};      // unit surface normal per corner
};

struct ImprintPolyline
{
    std::vector<int> indices;      // into positions, ordered along the branch
    bool closed = false;
};

struct ImprintResult
{
    std::vector<std::array<double, 3>> positions;
    std::vector<ImprintTriangle> triangles;
    std::vector<ImprintPolyline> polylines;
};

// Throws std::invalid_argument for malformed grids or options, and
// std::runtime_error when the intersection cannot be traced numerically or a
// budget is exhausted.  `out` is only replaced on success; on any exception
// its previous contents are preserved.
void imprint_surfaces(const SISLSurf &surf1,
                      const std::vector<double> &grid1_u,
                      const std::vector<double> &grid1_v,
                      const SISLSurf &surf2,
                      const std::vector<double> &grid2_u,
                      const std::vector<double> &grid2_v,
                      const ImprintOptions &options,
                      ImprintResult &out);

} // namespace sislmesh

#endif // SISL_IMPRINT_HPP
