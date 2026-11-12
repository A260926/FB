## Introduction
**LNLib is a C++ NURBS Algorithms Library.** <br/>These algorithms are primary referenced from [The NURBS Book 2nd Edition](https://link.springer.com/book/10.1007/978-3-642-97385-7). <br/>The APIs are re-designed to make it more friendly to users.

## Run LNLib
Please run build.bat first and it will construct solution by CMake.

## Features
Basic Elements:
- UV
- XYZ
- XYZW
- Matrix4d
- LNObject

Algorithms in ***The Nurbs Book***:
- ***Chapter 1***:
    - Basis Function Computation
- ***Chapter 1,2,3,4***:
    - Bezier/B-Spline/NURBS Curve and Surface
- ***Chapter 5***:
    - Curve and Surface Decomposition
    - Knot Insertion/Refinement/Removal
    - Degree Elevation and Reduction
- ***Chapter 6***:
    - Curve/Surface Point Inversion
    - Surface Tangent Vector Inversion
    - Curve/Surface Reparameterization
    - Curve Transform and Reverse
    - Surface Swap and Reverse
- ***Chapter 7***:
    - Create Arc/Conic Curve
- ***Chapter 8***:
    - Create Bilinear/Cylindrical/Ruled/Revolved/CornerFillet Surface
- ***Chapter 9***:
    - Global/Local Curve/Surface Interpolation and Approximation
- ***Chapter 10***:
    - Create Swung/Loft/Sweep/Gordon/Coons Surface
- ***Chapter 11***:
    - Curve Modification in Control Point Locations or Weight Values
- ***Chapter 12***:
    - Curve Clamp/UnClamp/IsClamp
    - KnotVector IsUniform
    - Curve IsClosed/IsPeriodic

Additional Algorithms:  
- ***Basic Properties***:
    - Curve/Surface Curvature and Normal
    - Curve Split/Segment/Merge/Offset
    - Curve IsLinear/IsArc
    - Curve Approximate Length
    - Surface Approximate Area
- ***Curve Creation***
    - Create Line/Cubic Hermite
- ***Tessellation***:
    - Curve Tessellation
    - Surface Triangulation

## Visualization
[LNLibViewer](https://github.com/BIMCoderLiang/LNLibViewer) based on [VTK](https://vtk.org/)

## Surface Geodesic Tracing
`LNLib::SurfaceGeodesic::Trace` performs arc-length initial-value geodesic
tracing on a clamped tensor-product NURBS surface. Given a start parameter and
an initial UV velocity, it normalizes the velocity to unit spatial speed with
the first fundamental form and integrates the geodesic equation in spatial arc
length; it is not a two-point shortest path and not a straight line drawn in
the UV plane. Multiplying the initial direction by any positive factor gives
the same trace.

Supported surfaces have degrees 2 to 7 in both directions, a rectangular
control net whose counts match the knot vectors, finite knots and homogeneous
coordinates, strictly positive finite weights, a positive-width valid
parameter rectangle, and interior knot multiplicities no greater than
`degree - 1` (at least C1 continuity). The surface is never modified.

```cpp
#include "SurfaceGeodesic.h"
#include "NurbsSurface.h"
#include "LNObject.h"
#include "UV.h"
#include "XYZ.h"

#include <vector>

using namespace LNLib;

void TraceExample(const LN_NurbsSurface& surface)
{
    UV start(0.3, 0.4);
    UV direction(1.0, -0.6);          // any positive scaling gives the same line
    std::vector<double> distances = { 0.0, 0.1, 0.25, 0.5 }; // arc-length stations

    GeodesicOptions options;
    options.tolerance = 1e-8;         // finite, positive
    options.maxStep = 0.1;            // max accepted step in spatial arc length
    options.maxTrials = 100000;       // accepted + rejected integration trials

    GeodesicTrace trace =
        SurfaceGeodesic::Trace(surface, start, direction, distances, options);

    // trace.status, trace.acceptedSteps, trace.rejectedSteps
    for (const GeodesicSample& sample : trace.samples)
    {
        double arcLength = sample.distance;       // cumulative spatial arc length
        UV uv = sample.parameter;                 // surface parameter
        XYZ point = sample.position;              // S(uv)
        XYZ unitTangent = sample.tangent;         // unit spatial tangent
    }
}
```

`distances` must be non-empty, start with `0.0`, and then list finite, strictly
increasing cumulative arc lengths; `{ 0.0 }` alone is valid. Every interior
knot crossing is added as a sample, coincident stations and events are stored
once, and sample distances are strictly increasing.

Status values returned in `GeodesicTrace::status`:

- `GeodesicStatus::completed` — every requested station was reached while the
  trace stayed inside the parameter rectangle.
- `GeodesicStatus::boundary` — the trace first touched the outer boundary; the
  final sample sits exactly on the first edge reached (a corner lies on two
  edges). Contact stops the trace without sliding along the edge or clamping
  out-of-range candidates back into the rectangle. A last station that lands
  exactly on the boundary still reports `boundary`.
- `GeodesicStatus::step_limit` — the accepted/rejected trial budget was
  exhausted. Only accepted states are published; rejected candidates never
  appear, and `acceptedSteps + rejectedSteps` never exceeds `maxTrials`.

Bad structure, degrees, knots, control net, weights, start parameter,
direction, station list, or options throw `std::invalid_argument` before any
surface evaluator is called. Zero progress or a degenerate metric
(`E <= 0`, `G <= 0`, or `(E*G - F*F)/(E*G) <= 1e-12`) throws
`std::runtime_error`.

## Contributing
Welcome join this project including discussions in **Issues** and make **Pull requests**.

## Author
LNLib is created by Yuqing Liang (BIMCoder Liang).

- bim.frankliang@foxmail.com
- 微信公众号：**BIMCoder梁老师**

## License
The source code is published under [GNU General Public License v3.0](https://www.gnu.org/licenses/), the license is available [here](LICENSE).

## Primary Reference
[The NURBS Book 2nd Edition](https://link.springer.com/book/10.1007/978-3-642-97385-7) by **Les Piegl & Wayne Tiller**
