# UV environment visibility / bent-normal baker

This example bakes per-texel environment visibility (AO) and world-space bent
normals from the same UV surface reconstruction used by `examples/uv_raster`.
It does not open a window, does not load a model, and does not estimate
occlusion from a camera depth buffer: it casts rays against the actual world
triangle BVH built with NanoRT.

`baker.h` / `baker.cpp` hold the reusable C++ interface (C++17, standard
library only), so tests and other tools can call the bake directly.

## What it does

* Builds a NanoRT triangle BVH from world positions and triangle indices.
* Rasterizes each face's face-varying UV triangle over the texture, using
  texel centers and the top-left half-open ownership rule. Reversed (mirrored)
  UV winding is handled after normalizing the triangle to CCW.
* Reconstructs the world position, geometric normal and winding-independent
  surface sample at each covered texel via `examples/uv_raster/uv_surface.h`.
* For each texel, rotates the caller-supplied local +Z hemisphere directions
  into a world tangent frame, starts a ray at `P + bias * N`, and tests the
  `[0, radius)` segment against the BVH. Only the source face is skipped.
* Writes `visibility` (fraction of unoccluded directions) and `bent_normals`
  (unit vector of the summed unoccluded world directions, zero when fully
  occluded).

The example generates its own geometry (floor, back/left walls, and a near
occluding plate), its own UV chart layout, and a deterministic cosine-weighted
direction set. It writes:

* `ao.ppm` - grayscale visibility
* `bent.ppm` - bent normal mapped to RGB

## Build and run

```
make
./uv_baker
```

Requires a C++17 compiler. The PPM output is written to the current directory.

## Inputs

```c++
uv_baker::Input input;
input.positions;              // xyz per world vertex
input.position_count;
input.triangle_indices;       // 3 vertex indices per face
input.triangle_index_count;
input.face_uvs;               // (u0,v0,u1,v1,u2,v2) per face, in corner order
input.face_uv_count;
input.face_chart_ids;         // one chart id per face
input.face_chart_id_count;
input.directions;             // non-empty unit xyz directions, z >= 0
input.direction_count;

uv_baker::Options options;
options.width;
options.height;
options.radius;               // > 0, finite
options.bias;                 // >= 0, finite
options.dilation_radius;      // optional integer fill, >= 0
```

`uv_baker::Bake` never modifies the input and fully replaces `result`; empty
coverage is valid and repeat bakes or size changes leave no stale values. It
returns `false` with a message for invalid dimensions, radius, bias, directions,
indices, non-finite coordinates, inconsistent array lengths, zero world-area
faces, or ambiguous UV overlap.

## Result layout

Every array is sized `width * height`, index `y * width + x`, with `y = 0`
at the bottom (v = 0). `real_coverage` and `dilated` distinguish true raster
coverage from the optional fill; `face_ids`, `chart_ids`, `barycentric_u/v`,
`visibility` and `bent_normals` hold the bake values.

`uv_baker::Dilate` (also applied by `Bake` when `dilation_radius > 0`) fills
only uncovered texels, copies every value/attribution from the nearest
original covered texel within the Euclidean radius, and never uses a texel
filled by the same pass as a source. Ties sort by chart id, then source y,
then source x, so narrow islands cannot bleed across a scan line.

## Checks

`sh checks/target.sh` compiles this baker plus `checks/target.cpp`, runs the
independent checks (barycentric reconstruction, brute-force ray/triangle
occlusion, plane / occluder / concave corner / mirrored UV / shared edge /
overlap rejection / zero area / narrow charts / two-island dilation /
reversed traversal), generates and re-reads `ao.ppm` and `bent.ppm`, and then
runs the original `checks/baseline.sh`.
