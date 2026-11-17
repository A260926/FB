# HGrid Region Editing and Sparse Isosurfaces (C++)

`Geometry::HGrid<T>` is a hierarchical voxel grid that can store uniform bricks
(a `Cell`) as a single value. Region editing and isosurface extraction are
provided directly on this representation so that a sculpted volume never has to
be expanded into, or copied to, a dense `Geometry::RGrid<T>`.

The two entry points described here are C++ only; PyGEL exposes the dense
`volumetric_isocontour` wrapper.

## Header and types

```cpp
#include <GEL/Geometry/HGrid.h>
#include <GEL/Geometry/XForm.h>
#include <GEL/HMesh/polygonize.h>
#include <GEL/HMesh/Manifold.h>

using namespace CGLA;
using namespace Geometry;
using namespace HMesh;
```

- `HGrid<T>` defaults to `DefaultCell<T, 8>`, i.e. bricks of `8 x 8 x 8`
  voxels. The last brick on an axis is partial when the grid dimension is not a
  multiple of 8; only its in-domain voxels are ever read or written.
- The template still works for ordinary scalar types (`float`, `double`,
  `int`, ...). Only the sparse isosurface overload is specialised for `float`.

## `HGrid::fill_box(lo, hi, value)`

```cpp
HGrid<float> volume(Vec3i(40, 40, 40), -1.0f);
volume.fill_box(Vec3i(8, 8, 8), Vec3i(32, 32, 32), 1.0f);
```

`lo` and `hi` are `Vec3i`s describing a **half-open** voxel box: voxel `p` is
filled when `lo[i] <= p[i] < hi[i]` on all three axes. `lo[i] <= hi[i]` is
required on each axis.

Semantics:

- The box is clipped to the grid; parts outside the grid are ignored. If the
  clipped intersection is empty, the grid is not modified.
- Only the box is written. Voxels outside it keep their previous values.
- `value` must be finite (checked with an assertion for floating point types).
- A top level brick that the box covers completely is merged into `value`
  directly — it is not split and written voxel by voxel.
- A brick that the box covers partially is modified voxel by voxel. Afterwards
  the brick is merged again if all of its in-domain voxels became equal.
- The padding of a partial last brick is not part of the voxel domain: it is
  neither examined nor changed.

`store`, `operator[]`, `get_cell`, `clear`, and copy construction/assignment
are unaffected and can be mixed freely with `fill_box`. A copied grid owns its
bricks, so edits to a copy (including `fill_box`, `store`, and `clear`) do not
affect the original.

## `volume_polygonize` on an `HGrid`

```cpp
Manifold volume_polygonize(const XForm& xform,
                           const HGrid<float>& grid,
                           float tau = 0,
                           bool make_triangles = true,
                           bool high_is_inside = true,
                           bool dual_connectivity = false,
                           SparsePolygonizeStats* stats = nullptr);
```

The parameters and defaults are identical to the existing `RGrid<float>`
overload. The extra trailing argument is an optional statistics pointer.

The result is the **same oriented surface** as the dense overload would produce
for the same scalar values: threshold polarity (`high_is_inside`), both
connectivity modes (`dual_connectivity`), optional triangulation, non-unit
`XForm`s, cross-brick interpolation, out-of-domain handling, and NaN-induced
holes all behave identically. Vertex numbering and traversal order may differ;
positions and connectivity match.

### Traversal and work

The sparse implementation walks the top level bricks rather than the whole
voxel domain:

- For a uniformly merged brick it reads the single representative value once
  and only descends to the boundary voxels of sides that can border a different
  value (another brick with a different value, or the grid boundary). The
  interior of a uniform brick is never scanned.
- A brick that has been split by editing is traversed voxel by voxel.
- The collected voxels are processed in the same order as the dense traversal,
  after which the original face-generation, stitching, vertex projection
  (`dual_connectivity` or marching-cubes clustering), `XForm` inverse, and
  triangulation code is shared with the dense entry point.

### `SparsePolygonizeStats`

Declared in `<GEL/HMesh/polygonize.h>`:

```cpp
struct SparsePolygonizeStats
{
    size_t visited_voxels = 0;  // voxels that underwent face determination
    size_t sample_reads = 0;    // scalar samples actually read
};
```

- The counters are reset at the beginning of every call.
- `visited_voxels` counts voxels for which the per-voxel inside/outside face
  test was actually performed. Uniform brick interiors that are skipped are not
  counted.
- `sample_reads` counts scalar samples actually read from the grid. Reading the
  representative value of a uniform brick counts once, not once per voxel.
- Passing `nullptr` is allowed and skips the counting.

For an all-uniform `96 x 96 x 96` grid with default value `1` and `tau = 0`,
`visited_voxels` is the boundary layer of the volume and stays below
`6 * 96^2`, while the complete closed outer surface is still generated.

## Complete windowless example

The repository contains a small program that needs no window system or display:
[`examples/hgrid_surface.cpp`](../../../examples/hgrid_surface.cpp). It fills a
box, carves a cavity, applies point edits, extracts the surface and optionally
saves it as OBJ.

Build it against a previously built `libgel.a`:

```sh
c++ -std=c++20 -I src examples/hgrid_surface.cpp path/to/libgel.a -o hgrid_surface
./hgrid_surface out.obj
```

The essential calls are:

```cpp
const Vec3i dims(40, 40, 40);
HGrid<float> volume(dims, -1.0f);
volume.fill_box(Vec3i(8, 8, 8), Vec3i(32, 32, 32), 1.0f);  // solid block
volume.fill_box(Vec3i(18, 18, 18), Vec3i(22, 22, 22), -1.0f); // cavity
volume.store(Vec3i(10, 20, 20), 1.0f);                       // point edit

const XForm xform(Vec3d(-1.0), Vec3d(1.0), dims, 0.0);
SparsePolygonizeStats stats;
Manifold surface = volume_polygonize(xform, volume, 0.0f,
                                     /*make_triangles=*/true,
                                     /*high_is_inside=*/true,
                                     /*dual_connectivity=*/false,
                                     &stats);
```

See `tests/hgrid_surface_target.cpp` for the regression suite that compares the
sparse result against the dense reference and checks the work counters.
