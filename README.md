Since there are no Input validation of the data given for triangulation you need
to think about this. Poly2Tri does not support repeat points within epsilon.

* If you have a cyclic function that generates random points make sure you don't
  add the same coordinate twice.
* If you are given input and aren't sure same point exist twice you need to
  check for this yourself.
* Only simple polygons are supported. You may add holes or interior Steiner points
* Interior holes must not touch other holes, nor touch the polyline boundary
* Use the library in this order:
  1. Initialize CDT with a simple polyline (this defines the constrained edges)
  2. Add holes if necessary (also simple polylines)
  3. Add Steiner points
  4. Triangulate

Point ownership and the editing phase
-------------------------------------

The CDT never owns the `p2t::Point` objects you pass in: the polyline, hole
and Steiner-point pointers you supply stay allocated and owned by your code
for the whole life of the CDT.  The triangulation only borrows them, and
dynamic constraint editing reuses those exact same objects as mesh vertices
without copying, moving or adding any point.  The Triangle objects created by
`Triangulate()` are owned by the CDT and freed in its destructor.

After `Triangulate()` you may repeatedly add and remove crease lines with the
dynamic constraint API:

```cpp
p2t::CDT::ConstraintId line = cdt.InsertConstraint(point_a, point_b);
if (line == 0) { /* the line was rejected, the mesh is unchanged */ }
cdt.RemoveConstraint(line);
```

Restrictions while editing:

* Both endpoints must be two distinct `p2t::Point` objects actually used by
  the finished triangulation.  Null pointers, foreign points and calls made
  before `Triangulate()` make `InsertConstraint` return `0`.
* A line is split at every mesh vertex it passes through; each piece becomes
  a real constrained edge.  No vertex is ever inserted, so an intersection
  that is not an existing vertex is not created: a line that crosses a
  protected edge away from a vertex, enters a hole, or leaves the outer
  contour is rejected as a whole and leaves the mesh completely unchanged.
* Outer contour and hole edges stay constrained forever.  Several constraint
  ids may share a sub-edge; it remains protected until the last sharing id
  is removed.  Successful ids are never reused, and removing `0`, an unknown
  id or an already removed id returns `false`.
* Do not move points, call `AddPoint`/`AddHole` or re-triangulate while
  editing.  The editor changes only local triangle connectivity (edge flips)
  and re-legalizes the freed diagonals after each change, preserving the
  total area, all input point identities and the constrained boundary.

Make sure you understand the preceding notice before posting an issue. If you have
an issue not covered by the above, include your data-set with the problem.
The only easy day was yesterday; have a nice day. <Mason Green>

TESTBED INSTALLATION GUIDE
==========================

Dependencies
------------

Core poly2tri lib:

* Standard Template Library (STL)

Unit tests:
* Boost (filesystem, test framework)

Testbed:

* OpenGL
* [GLFW](http://glfw.sf.net)

Build the library
-----------------

With the ninja build system installed:

```
mkdir build && cd build
cmake -GNinja ..
cmake --build .
```

Build and run with unit tests
----------------------------

With the ninja build system:

```
mkdir build && cd build
cmake -GNinja -DP2T_BUILD_TESTS=ON ..
cmake --build .
ctest --output-on-failure
```

Build with the testbed
-----------------

```
mkdir build && cd build
cmake -GNinja -DP2T_BUILD_TESTBED=ON ..
cmake --build .
```

Running the Examples
--------------------

Load data points from a file:
```
build/testbed/p2t <filename> <center_x> <center_y> <zoom>
```
Load data points from a file and automatically fit the geometry to the window:
```
build/testbed/p2t <filename>
```
Random distribution of points inside a constrained box:
```
build/testbed/p2t random <num_points> <box_radius> <zoom>
```
Examples:
```
build/testbed/p2t testbed/data/dude.dat 350 500 3

build/testbed/p2t testbed/data/nazca_monkey.dat

build/testbed/p2t random 10 100 5.0
build/testbed/p2t random 1000 20000 0.025
```

References
==========

- Domiter V. and Zalik B. (2008) Sweep‐line algorithm for constrained Delaunay triangulation
- FlipScan by library author Thomas Åhlén

![FlipScan](doc/FlipScan.png)
