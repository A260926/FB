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

## Catmull-Clark Subdivision Plans

When the connectivity stays fixed but control values change over and over —
for example while interactively previewing a model with several UV sets —
rebuilding the subdivided mesh each time is wasteful. `pmp::SubdivisionPlan`
(from `pmp/algorithms/subdivision_plan.h`) is an *owning* alternative to the
in-place `catmull_clark_subdivision()`:

```cpp
#include <pmp/algorithms/subdivision_plan.h>

pmp::SubdivisionPlan plan;
plan.build(mesh, /*levels=*/3, channels);   // captures topology once
plan.evaluate(positions, channel_values);   // only applies sparse stencils
```

- **Inputs.** `build()` takes the control cage (an oriented 2-manifold
  polygon mesh, boundaries, multiple components, extraordinary vertices, and
  pending deletions are fine; isolated vertices and non-finite values are
  rejected), a subdivision depth of 1 to 5, and any number of texture
  channels. Each channel is an independent `TexCoord` value table plus one
  value index per source face corner. Corners follow the halfedge order of
  each source face and point to the halfedge to-vertex, so the same
  geometric vertex can reference different values on different faces.
- **Ownership.** The plan stores the mapping from source vertices, faces,
  and corners to compact indices (`source_vertex()`, `source_face()`,
  `source_corner()`). After `build()`, the source mesh may be modified or
  destroyed; `evaluate()` only needs new positions and new channel value
  tables in compact order. Change the topology or the corner indices and you
  have to build a new plan.
- **Geometry.** Positions follow the same smooth and permanent feature rules
  as `catmull_clark_subdivision()` with `BoundaryHandling::Interpolate`.
- **Channel topology.** Every channel builds its own face-corner topology:
  the two sides of a geometric interior edge are connected only if both edge
  endpoints reference the same value index on both sides. Corners are never
  welded by UV value or spatial position, and face fans around a vertex that
  are separated by seams stay separate even if they reuse the same value
  index. Channels are refined with the smooth Catmull-Clark rules on their
  cut topology (boundary vertices use `(6P + prev + next) / 8`, boundary
  edge points take the midpoint); geometric features do not alter channel
  rules. Subfaces correspond one-to-one to the geometric subdivision and all
  channels share the refined positions.
- **Outputs.** The refined quad connectivity (`faces()`), the source face of
  every subface (`face_sources()`), positions, and per-channel refined value
  tables plus per-corner indices (`channel_values()`, `channel_corners()`).
  The linear maps are exposed as CSR stencils (`position_stencil()`,
  `channel_stencil()`) with sorted, merged columns and rows summing to one,
  so results can be verified independently.
- **Errors.** Invalid levels, value table sizes, corner indices, non-finite
  values, or non-manifold input are rejected with `InvalidInputException`
  before anything is published; a failed `build()` or `evaluate()` keeps the
  previous plan and results.

See `examples/subdivision_plan.cpp` for a complete example that evaluates
two control sets with one plan and writes `before.obj`/`after.obj` with
independent `vt` indices.

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
