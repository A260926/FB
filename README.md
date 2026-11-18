TinySpline
========

![CI](https://github.com/msteinbeck/tinyspline/actions/workflows/ci.yml/badge.svg)
![Security](https://github.com/msteinbeck/tinyspline/actions/workflows/codeql-analysis.yml/badge.svg)

TinySpline is a small, yet powerful library for interpolating, transforming,
and querying arbitrary NURBS, B-Splines, and Bézier curves. The core of the
library is written in ANSI C (C89) with a C++ wrapper for an object-oriented
programming model. Based on the C++ wrapper, auto-generated bindings for C#, D,
Go, Java, Javascript, Lua, Octave, PHP, Python, R, and Ruby are provided.

### Table of Contents

- [License](#license)
- [Features](#features)
- [Installation](#installation)
  * [Pre-built Binaries](#pre-built-binaries)
  * [Compiling From Source](#compiling-from-source)
- [Getting Started](#getting-started)
- [Curve Intersection](#curve-intersection)
- [Documentation](#documentation)
- [Publications](#publications)
- [Theoretical Backgrounds](#theoretical-backgrounds)

### License
MIT License - see the LICENSE file in the source distribution.

### Features

- Object-oriented programming model
- B-Splines of any degree and dimensionality
- Spline interpolation
  - Cubic natural
  - Centripetal Catmull–Rom
- Evaluation
  - Knots
  - Sampling (multiple knots at once)
  - Equidistant points
  - Components (find y for given x)
- Reparametrization by arc length
  - Mapping length <--> knot
- Knot insertion (refinement)
- Sub-spline extraction
- Bézier curve decomposition
  - (also known as subdivision)
- Derivative
- Degree elevation
- Computation of rotation minimizing frames
- Morphing
- Serialization (JSON)
- Vector math

### Installation

#### Pre-built Binaries

Releases can be downloaded from the
[releases](https://github.com/msteinbeck/tinyspline/releases) page. In
addition, the following package manager are supported:

Conan (C/C++):  
https://conan.io/center/tinyspline

NuGet (C#):
```xml
<PackageReference Include="tinyspline" Version="0.6.0.1" />
```

Go:
```bash
go get github.com/tinyspline/go@v0.6.0
```

Luarocks (Lua):
```bash
luarocks install --server=https://tinyspline.github.io/lua tinyspline
```

Maven (Java):
```xml
<dependency>
   <groupId>org.tinyspline</groupId>
   <artifactId>tinyspline</artifactId>
   <version>0.6.0-1</version>
</dependency>
```

PyPI (Python):
```bash
python -m pip install tinyspline
```

RubyGems (Ruby):
```bash
gem install tinyspline
```

#### Compiling From Source

See [BUILD.md](BUILD.md).

### Getting Started

A variety of examples (tests) can be found in the [test](test)
subdirectory.

The following listing shows a Python example:

```python
from tinyspline import *
import matplotlib.pyplot as plt

spline = BSpline.interpolate_cubic_natural(
  [
     100, -100, # P1
    -100,  200, # P2
     100,  400, # P3
     400,  300, # P4
     700,  500  # P5
  ], 2) # <- dimensionality of the points

# Draw spline as polyline.
points = spline.sample(100)
x = points[0::2]
y = points[1::2]
plt.plot(x, y)

# Draw point at knot 0.3.
vec2 = spline.eval(0.3).result_vec2()
plt.plot(vec2.x, vec2.y, 'ro')

# Draw tangent at knot 0.7.
pos = spline(0.7).result_vec2() # operator () -> eval
der = spline.derive()(0.7).result_vec2().normalize() * 200
s = pos - der
t = pos + der
plt.plot([s.x, t.x], [s.y, t.y])

# Draw 15 normals with equidistant distribution.
knots = spline.equidistant_knot_seq(15)
frames = spline.compute_rmf(knots)
for i in range(frames.size()):
    pos = frames.at(i).position
    nor = pos + frames.at(i).normal * 20
    # You can also fetch the tangent and binormal:
    #     frames.at(i).tangent
    #     frames.at(i).binormal
    plt.plot([pos.x, nor.x], [pos.y, nor.y], 'g')

plt.show()
```
Result:

![Getting Started](res/getting_started.png)

### Curve Intersection

TinySpline can intersect two clamped, non-rational B-spline curves of
degree 1 to 3 in two or three dimensions. The query reports two kinds of
results:

- **Isolated intersections**, each stored with a small interval on both
  original parameter axes, a representative parameter pair, the
  back-substituted 3D position (the third component is zero for 2D curves),
  and the lower indices of the two original non-zero knot intervals.
- **Coincident (overlapping) ranges**, recorded as the maximal parameter
  segment of the first curve that coincides with an arc of the second curve
  under the affine correspondence `v = a*u + b`, including the endpoint
  directions when the correspondence is reversed.

The C entry point is `ts_bspline_intersect` and the C++ convenience method
is `BSpline::intersect`:

```cpp
#include <tinysplinecxx.h>
using namespace tinyspline;

BSpline a(2, 2, 1, BSpline::Clamped);
a.setControlPoints({0, 0, 4, 2});
BSpline b(2, 2, 1, BSpline::Clamped);
b.setControlPoints({0, 3, 4, -1});

IntersectionResult r = a.intersect(b);   // spatial/param eps default 1e-7
if (r.complete()) {
    for (const auto &q : r.isolated())
        // q.u(), q.v() are parameters on a and b, q.point() the position.
        ;
    for (const auto &o : r.overlaps())
        // v = o.a()*u + o.b() over [o.u0(), o.u1()].
        ;
}
```

How the query works, and when it returns an unresolved result:

1. **Conservative exclusion.** Each curve is decomposed into its Bézier
   pieces (one per non-zero knot interval) and every working parameter box
   is restricted to those pieces. A box is discarded only when the
   component-wise bounding boxes of the two restricted arcs, enlarged by the
   spatial tolerance plus a small numerical safety margin, are disjoint.
   Bézier control points bound their curve by convex hull, so exclusion
   never discards a box that contains a solution.
2. **Coincidence by polynomial identity.** A candidate overlap is accepted
   only when the two restricted arcs are identical after composing one
   polynomial with the recovered affine map `v = a*u + b`. Both polynomials
   are converted to the power basis and all coefficients are compared
   component-wise; merely tangent or tolerance-close parallel curves fail
   this check and are never reported as coincident. A geometric extent guard
   prevents a near-constant tangency sliver from looking like an overlap.
3. **Root existence.** Surviving boxes are bisected on their widest
   tolerance-normalized axis. At parameter-tolerance width a multi-start,
   constrained Gauss-Newton solve locates the minimum-distance point. A
   point is reported as an isolated intersection only when its
   back-substituted residual reaches machine scale relative to the point
   magnitude; a merely small distance is never treated as proof of a root.
4. **Stopping conditions.** `complete` is set only when the whole parameter
   domain has either been excluded or assigned to a result. If the work-box
   budget is exhausted, the remaining boxes are returned with reason `0`. If
   subdivision reaches floating-point precision, the boxes that cannot be
   resolved are returned with reason `1`. In both cases the unresolved boxes
   cover every remaining possible solution, so an unresolved query is never
   confused with an empty intersection.

The same space point reached by two different parameter pairs (as in a
self-intersecting curve) yields two isolated records; deduplication is on
parameter pairs, not on world position. Points that lie on a recorded
coincidence correspondence are suppressed so the overlap is not reported as a
string of isolated points. A complete runnable example is in
[`examples/cxx/intersections.cpp`](examples/cxx/intersections.cpp).

### Documentation

The latest Doxygen documentation can be found at:
https://msteinbeck.github.io/tinyspline/

The documentation of the C interface
(https://msteinbeck.github.io/tinyspline/tinyspline_8h.html) is quite
extensive and also serves as an entry point for the C++ interface
documentation (as well as the documentation for the bindings created
from the C++ interface).

### Publications

If you use TinySpline in your research, please cite it as below.

```
@INPROCEEDINGS{Steinbeck:SANER:21,
  author =       {Steinbeck, Marcel and Koschke, Rainer},
  booktitle =    {2021 IEEE International Conference on Software
                  Analysis, Evolution and Reengineering (SANER)},
  title =        {TinySpline: A Small, yet Powerful Library for
                  Interpolating, Transforming, and Querying NURBS,
                  B-Splines, and Bézier Curves},
  year =         {2021},
  pages =        {572-576},
  doi =          {10.1109/SANER50967.2021.00068}
}
```

Other publications:

```
@INPROCEEDINGS{Steinbeck:VISSOFT:22,
  author =       {Steinbeck, Marcel and Koschke, Rainer},
  booktitle =    {2022 Working Conference on Software Visualization
                  (VISSOFT)},
  title =        {Edge Animation in Software Visualization},
  year =         {2022},
  pages =        {63-74},
  doi =          {10.1109/VISSOFT55257.2022.00015}
}
```

### Theoretical Backgrounds
[[1]](http://www.cs.mtu.edu/~shene/COURSES/cs3621/NOTES/spline/B-spline/bspline-curve.html)
is a very good starting point for B-Splines.

[[2]](http://www.cs.mtu.edu/~shene/COURSES/cs3621/NOTES/spline/B-spline/de-Boor.html)
explains De Boor's Algorithm and gives some pseudo code.

[[3]](http://www.codeproject.com/Articles/996281/NURBS-curve-made-easy)
provides a good overview of NURBS with some mathematical background.

[[4]](http://www.cs.mtu.edu/~shene/COURSES/cs3621/NOTES/spline/NURBS/NURBS-def.html)
is useful if you want to use NURBS in TinySpline.

