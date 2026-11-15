# `vcg::tri::SplitTextureTiles` — integer UV tile splitting

Header: `vcg/complex/algorithms/udim_tiles.h`
Namespace: `vcg::tri`
Language: C++17, header-only template, no new dependencies.

## Synopsis

```cpp
namespace vcg {
namespace tri {

struct TileAddress {
  int source_texture; // index into the source mesh textures vector
  int u;              // integer grid coordinate
  int v;
  bool operator<(const TileAddress &) const; // lexicographic (texture, u, v)
  bool operator==(const TileAddress &) const;
};

struct TileSplitMap {
  std::vector<int> face_source;                                 // per output face
  std::vector<std::array<std::array<double, 3>, 3>> corner_bary; // per output face
  std::vector<TileAddress> tiles;                               // sorted, unique
};

template <class MeshType>
void SplitTextureTiles(const MeshType &source,
                       MeshType &result,
                       TileSplitMap &map);

} // namespace tri
} // namespace vcg
```

The template works with any VCGLib triangle mesh whose vertices carry
`Coord3` (float or double), `Normal3`, `Quality` and `Color4b`, and whose
faces carry `VertexRef`, `FFAdj`, `Normal3`, wedge `TexCoord2` (float or
double) and `BitFlags` — including `UMesh` from `task_support/mesh.hpp` and
a same-component mesh with double coordinates and `WedgeTexCoord2d`.

## What it does

Each source triangle has an affine map from its 3D plane to its three wedge
UVs. The function cuts every triangle along all integer `u`/`v` grid lines,
triangulates the convex polygon that falls in each integer cell
`[u,u+1] × [v,v+1]`, and emits one independent mesh piece per non-empty
`(source_texture, u, v)` address.

- The 3D triangle is covered exactly once, preserving the original 3D
  winding; mirrored (negative UV orientation) triangles are supported.
- Grid corners and grid-line intersections are single events. Across a
  shared source edge the two faces exchange the **union** of their edge
  parameters (even across a UV seam, since the edge parameter is the
  geometric/barycentric parameter), so no output edge ever spans an existing
  point and there are no T-junctions.
- Vertices/edges are shared only along a locally connected face corner fan
  whose wedge UVs are continuous: same source texture and both endpoint UVs
  matching within `1e-7`. Grid boundaries, texture seams and mesh borders are
  disconnected. Coincident but unconnected geometry is never welded; a source
  vertex whose fan is split by a seam produces distinct output vertices.
- Local wedge UVs are the barycentric-interpolated global UV minus the cell
  address, so they lie in the closed unit square (boundary values `0`/`1` are
  allowed). Each output wedge stores the tile id in `WT().N()`: a
  non-negative `short`, the position of the address in `map.tiles`.
- Vertex position and quality are barycentric interpolated; colors are
  barycentric interpolated with per-channel rounding and clamping to
  `0..255`, while the original corner values are copied unchanged. Face and
  vertex normals are recomputed from the resulting surface and normalized.
  Other components are not copied.
- `result.textures` has one entry per tile, named
  `<source texture name>.tile.<u>.<v>` with signed decimal `u` and `v`,
  e.g. `paint.png.tile.-2.3`.

## Tile counting and limits

Addresses are counted after de-duplicating the tiles actually covered with
non-zero area — never estimated from the UV bounding box, so a sparse
triangle that overlaps few cells is not rejected. At most **32768** distinct
non-empty addresses are supported (the non-negative `short` range of
`WT().N()`). If the real count exceeds 32768 the function throws
`std::length_error`; `result` and `map` are left exactly as they were before
the call (no truncation, wraparound or partial output).

## Errors and guarantees

- `source` is read-only. `source == result` (aliasing) and any invalid input
  throw `std::invalid_argument`. Invalid input includes deleted/unreferenced
  elements, null vertex pointers, non-manifold edges, zero 3D or UV area,
  out-of-range coordinates (`|xyz| ≤ 1000`, `|uv| ≤ 32`), non-finite values,
  inconsistent face texture indices, empty/duplicate/whitespace texture
  names, or texture indices outside the textures vector.
- On any failure `result` and `map` keep their previous contents. Out-of-memory
  propagates the standard exception.
- Distinct events separated by more than `1e-6` in edge parameter or
  barycentric domain are kept apart; events on a shared edge within `1e-10`
  are merged. Exact on-line, grid-corner and coincident events are allowed.
- Geometric/barycentric reconstruction error is within `1e-9` at unit scale
  and float local UV error is within `1e-5`.

## PLY round trip and further processing

The output uses the ordinary `ExporterPLY`/`ImporterPLY` path with the
`IOM_WEDGTEXCOORD` mask (plus normal/quality/color masks as needed). Wedge
`texcoord`, per-face `texnumber` and the `TextureFile` comments all round
trip, as do vertex color, quality and normals. Reading a tile name together
with the local UV recovers the original global UV:

```
global_u = local_u + tile.u,  global_v = local_v + tile.v
source texture = name with the trailing ".tile.u.v" removed
```

The result keeps FF adjacency and border flags, so the standard
`RefineE`/`Refine` edge splitting works on it unchanged. The original
`RefineE` behavior and PLY defaults are not modified by this header.

## Minimal use

```cpp
UMesh source, pieces;
vcg::tri::TileSplitMap map;
vcg::tri::SplitTextureTiles(source, pieces, map);

int mask = vcg::tri::io::Mask::IOM_WEDGTEXCOORD |
           vcg::tri::io::Mask::IOM_VERTNORMAL |
           vcg::tri::io::Mask::IOM_VERTQUALITY |
           vcg::tri::io::Mask::IOM_VERTCOLOR;
vcg::tri::io::ExporterPLY<UMesh>::Save(pieces, "pieces.ply", mask, false);
```

See `examples/udim_tiles.cpp` for a complete program and `tests/udim_tiles.cpp`
for the behavioral coverage (tiles, topology, persistence groups).
