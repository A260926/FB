# libnurbs

NURBS algorithm library for modern C++. Slowly under development.

## Features

- [x] BSpline basis evaluation.
- [x] NURBS curve & surface interpolation and derivative evaluation.
- [x] Search for parameter of point on a curve or surface using the BFGS method.
- [x] Knot insertion(refinement) and removal.
- [x] Degree elevation and reduction.
- [ ] NURBS curve & surface fitting.
- [ ] ...

## Dependencies

1. [Eigen3](https://gitlab.com/libeigen/eigen) (for linear algebra)
2. [Catch2](https://github.com/catchorg/Catch2) (for unit tests)
3. [Matplot++](https://github.com/alandefreitas/matplotplusplus) (for visual tests)
4. [Google/Benchmark](https://github.com/google/benchmark) (for benchmark)

## Reference

1. [tinynurbs](https://github.com/pradeep-pyro/tinynurbs)

## Parameter domains

`KnotVector`, `Curve` and `Surface` support arbitrary clamped parameter domains,
not just the unit interval: a curve may live on U = [-3, 5] and a surface on
U = [-3, 5], V = [10, 12]. The valid evaluation interval is
`knots[degree]` .. `knots[count - degree - 1]`; knot values are stored and
reported exactly as given, and the domain is preserved through
`LoadFromFile`/`SaveToFile` (text and binary), knot insertion, degree
elevation, reversal and `AlignParameterDomain`. Reversing an axis maps
`u -> a + b - u` over the original domain `[a, b]`.

Supported envelope: degrees 1-3, clamped knot vectors whose internal
multiplicity does not exceed the degree, finite knots and coordinates with
absolute value up to 1e4, distinct knots separated by at least 1e-6, and
weights in [0.1, 10]. Illegal knot vectors and out-of-domain evaluation throw
`std::invalid_argument` (never `assert`). Points and first/second/mixed
derivatives are always returned with respect to the original parameters, so a
positive affine reparametrization `u' = alpha * u + beta` of either axis leaves
positions unchanged and scales derivatives by the chain rule. The parameter
searches (`SearchParameter`, `BinarySearchParameter`,
`SearchParameterOn`, `BinarySearchParameterOn`) take and return original-domain
parameters; an initial guess outside the domain is projected onto the nearest
domain boundary, and `epsilon` measures the spatial residual.

## Mesh export

`BuildSurfaceMesh(surface, us, vs)` (`libnurbs/Surface/SurfaceMesh.hpp`) samples
a surface into a `SurfaceMesh` with `positions`, `triangles`, `corner_uv` and
`corner_normals` (the latter three per triangle). `us`/`vs` are strictly
increasing original-domain stations that must contain both domain endpoints;
all distinct internal knots are merged into the grid. Each cell is split along
its lower-left to upper-right diagonal into two triangles with positive winding
in UV space. Positions are shared by parameter identity (the same `(u, v)`
station yields one position, never merged by spatial proximity), and every
corner stores its original-domain UV together with the analytic unit normal
taken from the *inside* of the knot cell the triangle belongs to. Across a C0
crease the two sides therefore share positions but keep their own one-sided
normals — normals are never averaged between faces, and no epsilon offset is
used to guess which side a corner is on. Illegal stations throw
`std::invalid_argument`, a degenerate one-sided normal throws
`std::runtime_error`, and in both cases the inputs are left unchanged.

`examples/domain_mesh.cpp` demonstrates the full pipeline: a rational surface
on a non-unit domain is edited (knot insertion, degree elevation), round-tripped
through the file format, meshed, and written as OBJ (`v`/`vt`/`vn`/`f`).

