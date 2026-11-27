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
- [Documentation](#documentation)
- [Knot Removal and Simplification](#knot-removal-and-simplification)
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
- Knot removal and simplification
  - Certified Euclidean error bound over the whole parameter domain
  - Fracture barriers (full-multiplicity internal knots) are preserved
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

### Documentation

The latest Doxygen documentation can be found at:
https://msteinbeck.github.io/tinyspline/

The documentation of the C interface
(https://msteinbeck.github.io/tinyspline/tinyspline_8h.html) is quite
extensive and also serves as an entry point for the C++ interface
documentation (as well as the documentation for the bindings created
from the C++ interface).

### Knot Removal and Simplification

The header `src/tinyspline_reduce.h` adds two C entry points,
`ts_bspline_remove_knot` and `ts_bspline_simplify`, for shrinking an
over-refined clamped, non-rational curve while keeping the same
parameter axis. They support degree 1 to 3, dimension 2 or 3, and
tolerances between 1e-9 and 1e-2.

#### Tolerance meaning

The `tolerance` argument bounds the Euclidean distance between the
original and the resulting curve measured at the *same* parameter
value, uniformly over the whole parameter domain. It is not a
resampling tolerance: knots are only deleted, never moved, and no new
sample knot vector is created. A removal that cannot be reconstructed
within the budget is a regular rejection — the call still returns
`TS_SUCCESS`, `removed` simply reports fewer removals (possibly zero).

The reported `error_bound` is a certificate, not a sampled maximum.
Both curves are expressed as Bézier segments on the original knot
partition; on every non-zero interval the bound is the maximum norm of
the coefficient-wise Bernstein difference vectors (the convex hull
property), covering the one-sided limits on both sides of break knots.
A loose certificate is tightened by subdividing the difference curve,
never by fixed-density sampling. Ordinary floating point roundoff is
covered by a conservative magnitude-proportional slack term; exactly
invertible material (insert a knot and remove it again) yields a bound
below 1e-10 at coordinate magnitudes under ten. Every accepted removal
is checked against the curve originally passed to the call, so the
tolerance is one global error budget that is never relaxed between
steps.

#### Fracture barriers

An internal knot whose multiplicity equals the curve's order splits the
curve into independent, possibly discontinuous pieces. Such a knot is a
fracture barrier: it is never removed, even when the control points on
its two sides happen to be spatially close. Removing it would change the
connectivity of the curve, not just its shape.

#### Memory ownership

On success the output spline owns freshly allocated, independent memory
(a deep copy even when nothing was removed), and `source` and `out` may
point to the same object. On error the existing `out`, `removed` and
`error_bound` values are left untouched. A complete modeling workflow
can be found in `examples/c/reduce.c`.

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

