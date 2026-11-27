# Introduction

[![build](https://github.com/pmp-library/pmp-library/workflows/build/badge.svg)](https://github.com/pmp-library/pmp-library/actions?query=workflow%3Abuild)
[![Coverage Status](https://coveralls.io/repos/github/pmp-library/pmp-library/badge.svg?branch=master)](https://coveralls.io/github/pmp-library/pmp-library?branch=main)
[![Latest Release](https://img.shields.io/github/v/release/pmp-library/pmp-library?sort=semver)](https://github.com/pmp-library/pmp-library/releases/latest)
[![DOI](https://zenodo.org/badge/105374301.svg)](https://zenodo.org/doi/10.5281/zenodo.10866531)

The Polygon Mesh Processing Library is a modern C++ open-source library for processing and visualizing polygon surface meshes. Its main features are:

- An efficient and easy-to-use mesh data structure
- Standard algorithms such as decimation, remeshing, subdivision, or smoothing
- Ready-to-use viewers
- Seamless cross-compilation to JavaScript ([demo](https://www.pmp-library.org/demos/mpview.html))

## Get Started

Clone the repository:

```sh
git clone https://github.com/pmp-library/pmp-library.git
```

Configure and build:

```sh
cd pmp-library && mkdir build && cd build && cmake .. && make
```

Run the mesh processing app:

```sh
./mpview ../data/off/bunny.off
```

Build your own tool:

```cpp
#include <pmp/surface_mesh.h>
#include <pmp/io/io.h>

int main()
{
    pmp::SurfaceMesh mesh;
    pmp::read(mesh,"input.obj");
    // .. do awesome things with your mesh
    pmp::write(mesh,"output.obj");
}
```

## Owned subdivision plans

`pmp::SubdivisionPlan` (`pmp/algorithms/subdivision_plan.h`) pre-computes a
Catmull-Clark subdivision hierarchy once and re-applies it many times. This is
useful when only positions or texture values change but the connectivity stays
the same: the plan owns all derived data, so the source `SurfaceMesh` may be
modified or destroyed after construction.

```cpp
#include <pmp/algorithms/subdivision_plan.h>

pmp::SubdivisionPlanChannel uv;
uv.name = "uv";
uv.values = { /* TexCoord value table */ };
uv.corner_values = { /* one value index per source halfedge */ };

pmp::SubdivisionPlan plan(mesh, /*levels=*/2, {uv});

// Later, with new control data. The plan is not rebuilt.
plan.evaluate(new_positions, {new_uv_values});
const auto& fine_positions = plan.positions();
const auto& fine_uv = plan.channel_values(0);
```

The constructor takes a `SurfaceMesh`, a level count in `[1, 5]`, and any
number of texture channels. Each channel supplies its own `values` table and a
per-corner index array indexed by `Halfedge::idx()`, following the source face
halfedge order; a corner's value belongs to `to_vertex(h)`. Corners in
different faces may reference different indices with equal numeric values, and
those index seams are preserved.

Construction validates the input (oriented two-manifold polygons with boundary,
multiple components, irregular valence, and uncompressed deleted holes are
accepted; isolated vertices and non-finite values are rejected) and stores:

- compact source vertex, face and corner ids and their handle round-trips,
- the shared result face connectivity, per-face source ids and positions,
- a CSR `position_stencil()` over source vertices,
- per channel a CSR `channel_stencil()` over source corners, plus
  `channel_corner_indices()`, `channel_values()` and the build-time value
  index tables.

`evaluate()` only applies the stored CSR stencils; it never traverses a
half-edge structure or subdivides again. Position rules match PMP's existing
Catmull-Clark implementation, including the permanent `"v:feature"` /
`"e:feature"` rules and `BoundaryHandling::Interpolate` on geometric
boundaries. Each channel is subdivided on its own cut corner topology: an
interior edge connects the two sides only when both endpoints reference the
same channel index on both sides, and faces are never merged by equal UV values
or positions. A vertex fan separated by a seam stays separated, even if both
sides reuse one index value. Channel boundaries use `(6P + prev + next) / 8`
for vertices and edge midpoints for edges; geometry feature flags do not affect
channels. All channels share the geometric positions and the one-to-one
subface layout.

Invalid level counts, value table lengths, corner indices, or topology throw
before exposing a new plan or result; `rebuild()` and `evaluate()` leave the
last successful plan and results untouched on failure. The legacy in-place
`catmull_clark_subdivision()` keeps its previous behaviour. Limit-surface
evaluation, semi-sharp creases and incremental local updates are intentionally
out of scope.

`examples/subdivision_plan.cpp` demonstrates the workflow by writing
`before.obj` and `after.obj` with independent `vt` indices.

## Read the Docs

The [user guide](https://www.pmp-library.org/guide.html) contains all you need to get started using PMP, including a [tutorial](https://www.pmp-library.org/tutorial.html) covering mesh processing basics.

## Contribute

Contributions to PMP are welcome! There are many ways you can help: Report any [issues](https://github.com/pmp-library/pmp-library/issues) you find, help to improve the documentation, join our [discussions](https://github.com/pmp-library/pmp-library/discussions) forum, or [contribute](https://www.pmp-library.org/contributing.html) new code.

## Acknowledge

If you are using PMP for research projects, please acknowledge its use by referencing

```tex
@software{pmp23,
  author = {Sieger, Daniel and Botsch, Mario},
  title = {{The Polygon Mesh Processing Library}},
  year = {2023},
  month = aug,
  version = {3.0.0},
  doi = {10.5281/zenodo.10866532},
  url = {https://github.com/pmp-library/pmp-library}
}
```

We acknowledge that PMP evolved from our previous work on [Surface_mesh](http://dx.doi.org/10.1007/978-3-642-24734-7_29) and [OpenMesh](https://pub.uni-bielefeld.de/record/1961694).

## License

PMP is provided under a [MIT license](https://github.com/pmp-library/pmp-library/blob/master/LICENSE.txt), allowing for both open-source and commercial usage.
