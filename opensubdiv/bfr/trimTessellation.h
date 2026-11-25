//
//   Copyright 2025 Pixar
//
//   Licensed under the terms set forth in the LICENSE.txt file available at
//   https://opensubdiv.org/license.
//
#ifndef OPENSUBDIV3_BFR_TRIMTESSELLATION_H
#define OPENSUBDIV3_BFR_TRIMTESSELLATION_H

#include "../version.h"

#include <array>
#include <string>
#include <vector>

#include "clipSurface.h"

namespace OpenSubdiv {
namespace OPENSUBDIV_VERSION {

namespace Far {
    class TopologyRefiner;
}

namespace Bfr {

namespace clipdetail {

//
// Internal engine that builds the conforming, adaptively refined and
// plane-trimmed tessellation for ClipRegularSurfaceMesh().
//
// The engine works in the parameter domains of the selected regular
// bicubic faces.  Refinement is a conforming edge-bisection mesh: splitting
// an edge splits every triangle incident to it, and points on coarse edges
// are interned by coarse-topology identity, so the result has no T-junctions
// and adjacent selected faces share position indices along their common
// coarse edge.  Conservative cubic Bernstein bounds decide termination;
// all emitted vertices are evaluated through Bfr Surface evaluation.
//
class TrimTessellation {
public:
    TrimTessellation(Far::TopologyRefiner const & topology,
                     std::vector<double> const & positions,
                     std::vector<double> const & uv,
                     std::vector<int> const & selectedFaces,
                     std::array<double, 3> const & planeNormal,
                     double planeDistance,
                     ClipOptions const & options);

    // Validate topology/surfaces and build the mesh.  Returns false with a
    // non-empty error on any unsupported or exhausted configuration.  On
    // success result is fully replaced.
    bool Build(ClippedMesh & result, std::string & error);

private:
    struct Impl;
    Impl * _impl;

    TrimTessellation(TrimTessellation const &) = delete;
    TrimTessellation & operator=(TrimTessellation const &) = delete;

public:
    ~TrimTessellation();
};

} // end namespace clipdetail

} // end namespace Bfr
} // end namespace OPENSUBDIV_VERSION
using namespace OPENSUBDIV_VERSION;

} // end namespace OpenSubdiv

#endif /* OPENSUBDIV3_BFR_TRIMTESSELLATION_H */
