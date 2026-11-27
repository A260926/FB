# C++ API: Sparse Voxel Sculpting with HGrid

This page documents the C++ API for region edits on the hierarchical voxel
grid `Geometry::HGrid` and for turning the sparse data directly into a
polygonal surface. Both live in the C++ library (headers under
`src/GEL`), not in the Python bindings.

## `Geometry::HGrid<T>` — hierarchical voxel grid

`HGrid` (header `GEL/Geometry/HGrid.h`) stores a voxel volume as a top level
grid of 8&times;8&times;8 bricks. A brick whose voxels all hold the same value
is *coalesced* into a single value, so uniform regions cost O(1) memory per
brick. The template works with ordinary numeric types (`float`, `int`, ...).

```cpp
HGrid<float> grid(Vec3i(96, 96, 96), -1.0f); // dims, default value
grid.store(Vec3i(20, 50, 50), 1.0f);         // single voxel edit
float v = grid[Vec3i(20, 50, 50)];           // read
grid.clear();                                // back to the default value
```

### `fill_box(lo, hi, value)` — region edit

```cpp
void fill_box(const Vec3i& lo, const Vec3i& hi, const T& value);
```

Fills the half-open voxel box `[lo, hi)` with `value`:

* `lo`/`hi` are integer voxel coordinates with `lo <= hi` on every axis.
  The box is clipped against the grid domain; if the intersection is empty
  the grid is left unchanged. Values outside the box are preserved.
* A brick whose whole logical (voxel domain) extent is covered is coalesced
  directly into the new value - it is **not** expanded and rewritten voxel
  by voxel. Only partially covered bricks are expanded, and only their
  affected voxels are written.
* After a partial fill, a brick whose valid voxels all ended up equal is
  coalesced again. The padding of the trailing bricks (when the grid
  dimensions are not multiples of 8) is not part of the voxel domain and
  does not take part in this test.
* `store`, `clear`, `get_cell` and copying keep working as before; copies
  are independent of the original.

## `HMesh::volume_polygonize` — sparse iso-surfacing

Header `GEL/HMesh/polygonize.h`. In addition to the dense
`RGrid<float>` overload, there is an overload taking the sparse grid
directly:

```cpp
struct SparsePolygonizeStats {
    size_t visited_voxels = 0; // voxels individually tested for face generation
    size_t sample_reads   = 0; // scalar samples read (uniform brick counts once)
};

HMesh::Manifold volume_polygonize(const Geometry::XForm& xform,
                                  const Geometry::HGrid<float>& grid,
                                  float tau = 0,
                                  bool make_triangles = true,
                                  bool high_is_inside = true,
                                  bool dual_connectivity = false,
                                  SparsePolygonizeStats* stats = nullptr);
```

The overload produces the same oriented surface as the dense entry point for
the same scalar field - threshold polarity, both connectivity modes (dual
contouring and marching cubes), optional triangulation, NaN handling
(missing samples become holes) and non-unit `XForm` transforms all behave as
before, and the two entry points share the connectivity, vertex projection
and triangulation code.

The difference is in the traversal: uniform (coalesced) bricks contribute
only their boundary faces and their interiors are never scanned voxel by
voxel, and the grid is not converted to an `RGrid`. If `stats` is non-null,
its counters are reset at the start of the call and then tally the actual
work: `visited_voxels` counts voxels individually examined for inside/outside
face determination, and `sample_reads` counts the scalar samples read, where
reading the representative value of a uniform brick counts once. For a fully
uniform 96&sup3; grid at `tau = 0`, `visited_voxels` stays within
6&middot;96&sup2; while the complete outer surface is generated.

## Example

A complete windowless program is available at
`src/demo/HGridSculpt/hgrid_sculpt.cpp`:

```cpp
HGrid<float> grid(Vec3i(96, 96, 96), -1.0f);
grid.fill_box(Vec3i(16, 16, 16), Vec3i(80, 80, 80),  1.0f); // solid block
grid.fill_box(Vec3i(40, 40, 16), Vec3i(56, 56, 80), -1.0f); // carve a shaft
grid.store(Vec3i(20, 50, 50), -1.0f);                       // single voxel edit

XForm xform(Vec3i(96, 96, 96), 0.02);
SparsePolygonizeStats stats;
Manifold mesh = volume_polygonize(xform, grid, 0.0f, true, true, false, &stats);
// mesh is a renderable Manifold; stats tells how much work was done.
```

Build it like the other C++ demos (it is part of the `src/demo` CMake
project), or directly against the GEL sources:

```sh
c++ -I src -std=c++20 src/demo/HGridSculpt/hgrid_sculpt.cpp \
    src/GEL/CGLA/*.cpp src/GEL/Geometry/*.cpp src/GEL/HMesh/*.cpp src/GEL/Util/*.cpp \
    -o hgrid_sculpt
./hgrid_sculpt sculpt.obj
```
