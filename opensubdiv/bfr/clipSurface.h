//
//   Copyright 2025 Pixar
//
//   Licensed under the terms set forth in the LICENSE.txt file available at
//   https://opensubdiv.org/license.
//
#ifndef OPENSUBDIV3_BFR_CLIPSURFACE_H
#define OPENSUBDIV3_BFR_CLIPSURFACE_H

#include "../version.h"

#include <array>
#include <string>
#include <vector>

namespace OpenSubdiv {
namespace OPENSUBDIV_VERSION {

namespace Far {
    class TopologyRefiner;
}

namespace Bfr {

///
/// @brief An oriented plane used to clip a regular Catmull-Clark limit
///        surface.
///
/// Points are retained when dot(normal, P) + offset >= 0.  The normal is
/// normalized (and the offset scaled consistently) before any use.
///
struct ClipPlane {
    std::array<double, 3> normal;
    double                offset;
};

///
/// @brief Tolerances and resource limits for a clipping operation.
///
/// geometricTolerance bounds the maximum Euclidean distance between the
/// limit surface over the parameter domain of every output triangle and
/// the triangle's linear interpolation.  planeTolerance bounds the
/// distance of every plane-intersection vertex to the normalized plane.
/// maxDepth (1..24) bounds the adaptive parametric subdivision depth.
///
struct ClipOptions {
    double geometricTolerance;
    double planeTolerance;
    int    maxDepth;
};

///
/// @brief Per-corner attributes of an output triangle.
///
/// parameter is the [0,1]^2 coordinate of the corner in its source coarse
/// face, uv is the channel-0 face-varying value evaluated there and normal
/// is the analytic, unitized du x dv normal of the source face.
///
struct ClipCorner {
    std::array<double, 2> parameter;
    std::array<double, 2> uv;
    std::array<double, 3> normal;
};

///
/// @brief Result of ClipRegularSurfaceMesh().
///
/// corners is triangle-major: exactly three ClipCorner values are stored
/// for every entry of triangles.
///
struct ClippedMesh {
    std::vector<std::array<double, 3>> positions;
    std::vector<std::array<unsigned, 3>> triangles;
    std::vector<int>                     sourceFaces;
    std::vector<ClipCorner>              corners;
};

///
/// @brief Clip selected regular Catmull-Clark limit-surface faces against a
///        directed plane.
///
/// Only the positive half-space dot(n,P)+d >= 0 is retained; no caps are
/// generated.  The operation is restricted to oriented manifold quad cages
/// without explicit sharp edges for which every selected face has a regular
/// bicubic Bfr position Surface.  Unsupported inputs, degenerate transverse
/// configurations that cannot be resolved and exhaustion of the adaptive
/// depth budget result in false with a non-empty error message.  On failure
/// the caller's output object is left unchanged.
///
/// An empty selectedFaces vector yields an empty, successful result.
///
bool
ClipRegularSurfaceMesh(Far::TopologyRefiner const & topology,
                       std::vector<double> const & positions,
                       std::vector<double> const & uv,
                       std::vector<int> const & selectedFaces,
                       ClipPlane const & plane,
                       ClipOptions const & options,
                       ClippedMesh & output,
                       std::string & error);

} // end namespace Bfr

} // end namespace OPENSUBDIV_VERSION
using namespace OPENSUBDIV_VERSION;

} // end namespace OpenSubdiv

#endif /* OPENSUBDIV3_BFR_CLIPSURFACE_H */
