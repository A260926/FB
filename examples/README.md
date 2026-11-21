# DAG preview (CPU rasteriser)

This example renders the compressed voxel DAG directly on the CPU - no window, GPU,
SDL or OpenGL is needed - and writes the frame to `.task-build/dag.ppm` (a binary
PPM, 64x48 pixels). Build and run it from the repository root with:

    bash task_support/run.sh target

which compiles `src/library/dag_rasterizer.cpp` and `src/library/depth_hierarchy.cpp`,
links this example, deletes any old image, and then runs it.

## Camera

`renderDag(const Volume&, const RasterCamera&, bool occlusion = true)` (see
`src/library/dag_rasterizer.h`) takes a pinhole camera made of `double` vectors:
`eye` plus a unit orthonormal basis (`right`, `up`, `forward`), an image size
(`width` x `height`, 1..1024), a vertical field of view `fovY` in (0, pi), and clip
distances `0 < near < far`. The pixel origin is the top-left corner and pixel (x, y)
is sampled along the unnormalised ray

    d = forward + right * (2*(x+0.5)/width  - 1) * tan(fovY/2) * width/height
                  + up    * (1 - 2*(y+0.5)/height) * tan(fovY/2)

so the ray parameter `t` is exactly the forward depth (`d` has unit forward
component). The example uses eye (31.5, 23.5, -30), basis = identity, 64x48 pixels,
fovY = 0.6 rad, near = 1, far = 300, looking along +z.

## Depth

Each integer voxel centre `q` is the solid cell `[q-0.5, q+0.5]^3`. A pixel's depth
is the start of the first positive-length interval where its ray intersects
non-empty material within `[near, far]`; if `near` is already inside the solid the
depth is `near`. Material 0 is empty and every other material is opaque. Misses are
recorded as depth `+infinity` and material `0`; depth ties are won by the smaller
material id.

The rasteriser walks the NodeStore DAG front-to-back and commits whole material
leaves at their true projected size (coarse uniform leaves are never subdivided into
unit cells, and projected bounding rectangles are only used for conservative
culling). While doing so it maintains a Hi-Z depth hierarchy
(`src/library/depth_hierarchy.h`): a pyramid where each texel holds the maximum
depth of the real pixels below it (odd edges are not padded with fictitious
pixels). After each committed leaf only the ancestors of the written pixels are
updated. A sub-tree is rejected only when every pixel of its conservative footprint
already holds a strictly nearer valid depth - holes and equal depths never reject -
and the hierarchy is rebuilt empty for every frame, so camera changes or scene edits
never reuse stale occlusion. Passing `occlusion = false` runs the identical
rasterisation path with only the rejection switched off, as a baseline.

## Statistics

`DagRasterResult` reports actual per-frame counters (incremented where the work
happens, never theoretical values):

* `nodeVisits`  - DAG node occurrences processed (a shared node counts once per
  spatial occurrence; there is no global visited set).
* `leafBoxes`   - material leaf occurrences committed to the rasteriser.
* `hizReads`    - depth-hierarchy texel reads (occlusion queries).
* `hizUpdates`  - depth-hierarchy texel writes (pixel writes plus ancestor updates).
* `hizRejected` - node occurrences skipped because the hierarchy proved their whole
  conservative footprint is already occluded.

The example prints these counters for the same frame with occlusion on and off.

## The scene and image

The scene is an occluder wall (material 2, red) with a grid of 6x6 holes, in front
of a rear structure of vertical stripes (materials 5 and 6, cyan/yellow) which is
only visible through the holes. Materials are mapped to RGB with a fixed palette
(empty = dark sky) and written as a binary PPM (`P6`, maxval 255) to
`.task-build/dag.ppm`. The tests in `tests/dag_raster_target.cpp` verify the image
dimensions and the expected visible colours.
