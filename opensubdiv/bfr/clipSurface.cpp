//
//   Copyright 2025 Pixar
//
//   Licensed under the terms set forth in the LICENSE.txt file available at
//   https://opensubdiv.org/license.
//

#include "clipSurface.h"
#include "trimTessellation.h"

#include "../far/topologyRefiner.h"

#include <cmath>
#include <limits>
#include <sstream>

namespace OpenSubdiv {
namespace OPENSUBDIV_VERSION {
namespace Bfr {

namespace {

bool
allFinite(double const * v, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        if (!std::isfinite(v[i])) return false;
    }
    return true;
}

} // namespace

bool
ClipRegularSurfaceMesh(Far::TopologyRefiner const & topology,
                       std::vector<double> const & positions,
                       std::vector<double> const & uv,
                       std::vector<int> const & selectedFaces,
                       ClipPlane const & plane,
                       ClipOptions const & options,
                       ClippedMesh & output,
                       std::string & error) {
    //
    // Validate everything before touching the caller's output so that any
    // failure leaves all of its fields exactly as they were.
    //
    auto fail = [&](std::string const & msg) -> bool {
        error = msg;
        return false;
    };

    if (!std::isfinite(plane.normal[0]) ||
        !std::isfinite(plane.normal[1]) ||
        !std::isfinite(plane.normal[2]) ||
        !std::isfinite(plane.offset)) {
        return fail("ClipRegularSurfaceMesh: non-finite clipping plane");
    }

    double nlen = std::sqrt(plane.normal[0] * plane.normal[0] +
                            plane.normal[1] * plane.normal[1] +
                            plane.normal[2] * plane.normal[2]);
    if (!(nlen > 0.0) || !std::isfinite(nlen)) {
        return fail("ClipRegularSurfaceMesh: zero-length plane normal");
    }

    if (!std::isfinite(options.geometricTolerance) ||
        !(options.geometricTolerance > 0.0)) {
        return fail("ClipRegularSurfaceMesh: geometricTolerance must be positive and finite");
    }
    if (!std::isfinite(options.planeTolerance) ||
        !(options.planeTolerance > 0.0)) {
        return fail("ClipRegularSurfaceMesh: planeTolerance must be positive and finite");
    }
    if (options.maxDepth < 1 || options.maxDepth > 24) {
        return fail("ClipRegularSurfaceMesh: maxDepth must be in [1,24]");
    }

    int numCoarseVerts = topology.GetLevel(0).GetNumVertices();
    int numCoarseFaces = topology.GetLevel(0).GetNumFaces();

    if (positions.size() != static_cast<size_t>(numCoarseVerts) * 3u) {
        std::ostringstream os;
        os << "ClipRegularSurfaceMesh: positions length " << positions.size()
           << " does not match coarse vertex count " << numCoarseVerts;
        return fail(os.str());
    }
    if (!positions.empty() &&
        !allFinite(positions.data(), positions.size())) {
        return fail("ClipRegularSurfaceMesh: non-finite position values");
    }

    // Empty selection is an empty success and needs no UV channel or face
    // validation.
    if (selectedFaces.empty()) {
        output = ClippedMesh();
        error.clear();
        return true;
    }

    // Channel-0 face-varying UVs are validated when present.  A cage without
    // any face-varying channel is tolerated (corners then carry zero UVs).
    if (topology.GetNumFVarChannels() > 0) {
        int numUV = topology.GetLevel(0).GetNumFVarValues(0);
        if (uv.size() != static_cast<size_t>(numUV) * 2u) {
            std::ostringstream os;
            os << "ClipRegularSurfaceMesh: uv length " << uv.size()
               << " does not match channel 0 value count " << numUV;
            return fail(os.str());
        }
        if (!uv.empty() && !allFinite(uv.data(), uv.size())) {
            return fail("ClipRegularSurfaceMesh: non-finite uv values");
        }
    }

    for (int face : selectedFaces) {
        if (face < 0 || face >= numCoarseFaces) {
            std::ostringstream os;
            os << "ClipRegularSurfaceMesh: selected face index " << face
               << " out of range [0," << numCoarseFaces << ")";
            return fail(os.str());
        }
    }
    for (size_t i = 0; i < selectedFaces.size(); ++i) {
        for (size_t j = i + 1; j < selectedFaces.size(); ++j) {
            if (selectedFaces[i] == selectedFaces[j]) {
                std::ostringstream os;
                os << "ClipRegularSurfaceMesh: selected face "
                   << selectedFaces[i] << " listed more than once";
                return fail(os.str());
            }
        }
    }

    // Normalize the plane before any distance is interpreted.
    std::array<double, 3> nrm = {{
        plane.normal[0] / nlen,
        plane.normal[1] / nlen,
        plane.normal[2] / nlen
    }};
    double dist = plane.offset / nlen;
    if (!std::isfinite(nrm[0]) || !std::isfinite(nrm[1]) ||
        !std::isfinite(nrm[2]) || !std::isfinite(dist)) {
        return fail("ClipRegularSurfaceMesh: invalid normalized plane");
    }

    try {
        clipdetail::TrimTessellation builder(topology, positions, uv,
                                            selectedFaces, nrm, dist, options);
        ClippedMesh local;
        std::string localError;
        if (!builder.Build(local, localError)) {
            if (localError.empty()) {
                localError = "ClipRegularSurfaceMesh: clipping failed";
            }
            error = localError;
            return false;
        }
        output = std::move(local);
        error.clear();
        return true;
    } catch (std::exception const & e) {
        return fail(std::string("ClipRegularSurfaceMesh: ") + e.what());
    } catch (...) {
        return fail("ClipRegularSurfaceMesh: unknown failure");
    }
}

} // end namespace Bfr
} // end namespace OPENSUBDIV_VERSION
} // end namespace OpenSubdiv
