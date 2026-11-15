# libnurbs

NURBS algorithm library for modern C++. Slowly under development.

## Features

- [x] BSpline basis evaluation.
- [x] NURBS curve & surface interpolation and derivative evaluation.
- [x] Parametric evaluation on arbitrary finite, positive-width clamped domains.
- [x] Search for parameter of point on a curve or surface using the BFGS method.
- [x] Knot insertion(refinement) and removal.
- [x] Degree elevation and reduction.
- [x] Surface tessellation with original-UV corners and one-sided analytic normals.
- [ ] NURBS curve & surface fitting.
- [ ] ...

## Parameter domains

Curves and surfaces are defined on the knot vector's **original domain**, not
an implicit unit square:

- The effective interval is `[knots[degree], knots[count - degree - 1]]`, so a
  vector `{-3, -3, -3, 1, 5, 5, 5}` describes `U = [-3, 5]` for degree 2.
- All evaluation, derivative and parameter-search entry points accept and
  return values in that original domain. Derivative magnitudes follow the
  original parameterisation: applying an affine reparameterisation
  `u' = a + b * u` scales a U derivative by `b`.
- Existing unit-domain inputs and file formats are unchanged.
- Invalid knot vectors and parameters outside the domain raise
  `std::invalid_argument`; no `assert` is used for these checks.
- Supported: degree 1..3, positive finite weights, finite control points,
  knot/coordinate magnitudes up to `1e4`, distinct knots at least `1e-6`
  apart, and interior knot multiplicities no greater than the degree.
- When a reverse operation is needed (explicit reversal or
  `AlignParameterDomain` flipping an axis) the parameter map is `u -> a + b - u`
  over the axis' original domain, so geometry and parameter correspondence are
  preserved.

Search defaults are projected onto the nearest domain boundary when the
initial guess is out of range. `epsilon` measures the spatial residual, not a
raw UV step.

## Mesh export

`libnurbs/Surface/SurfaceMesh.hpp` provides
`BuildSurfaceMesh(const Surface&, const std::vector<double>& us, const std::vector<double>& vs)`.
The supplied `us`/`vs` are strictly increasing original-domain sites that must
include both domain endpoints; every distinct interior knot is merged into the
grid. Each cell is split along its lower-left to upper-right diagonal into two
triangles that are counter-clockwise in UV. Corner UV and normals are stored
per triangle:

- `positions` are shared by parameter identity, never by spatial proximity, so
  a folded surface keeps distinct vertices even when they coincide in space.
- `corner_uv` stores the original UV of each triangle corner.
- `corner_normals` stores the analytic unit normal on the interior side of the
  knot cell the corner belongs to. On a C0 crease two sides share a position
  but keep their own normals; face-averaged normals are not used.

Invalid sites raise `std::invalid_argument`; a degenerate one-sided normal
raises `std::runtime_error`. Both leave the input unchanged.

## Running the task checks

```bash
bash task_support/run.sh baseline   # existing production path
bash task_support/run.sh target     # build + example + domain/editing/mesh checks
```

## Dependencies

1. [Eigen3](https://gitlab.com/libeigen/eigen) (for linear algebra)
2. [Catch2](https://github.com/catchorg/Catch2) (for unit tests)
3. [Matplot++](https://github.com/alandefreitas/matplotplusplus) (for visual tests)
4. [Google/Benchmark](https://github.com/google/benchmark) (for benchmark)

## Reference

1. [tinynurbs](https://github.com/pradeep-pyro/tinynurbs)

## Crease handling in the mesh export

When a surface is only C0 across an interior knot, the two knot cells meeting at
that knot have different one-sided derivatives. `BuildSurfaceMesh` evaluates
each corner normal with the knot span of the cell the corner belongs to, so a
corner on the crease appears twice in `corner_normals` with two different unit
normals while `positions` still holds a single shared vertex. This keeps hard
edges hard without averaging across the crease and without nudging the sample
point by a fixed epsilon to guess which side it is on.

