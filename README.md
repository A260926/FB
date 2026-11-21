![Discrete Mirage](logo_main.png)

# discrete-mirage

A C library for rendering voxel models on CPU.

Currently, it's more of a prototype / proof-of-concept, so use at your own risk. If you do, no attribution is necessary, but a mention would nevertheless be appreciated :-)

The demo folder contains a [C++ example](demo/src/main.cpp) of using the library to render a small test scene. Once you manage to build the project, run the demo with a command line argument specifying a path to the octree file (a couple of such files can be downloaded from [here](https://github.com/dairin0d/OctreeSplatting/releases)).

If you're curious about the details, take a look at the [notes](docs/notes.md).

## Deep layer capture (translucency)

Besides the usual opaque splatting, the CPU RECT splat path can capture
**multiple translucent layers per pixel**, so you can see which spatial
nodes are stacked behind a pixel.

### Sample identity

A captured sample is identified by the pair
`(submission_key, node_box)`, where `node_box` locates the sampled node in
the submission's root-local `[0,1]^3` grid (level + x/y/z coordinates) --
**not** by `data_ref` or `affine_id`. Two spatial positions of a shared
DAG, or two submissions of the same geometry, are therefore always
recorded as separate layers, while the same node repeatedly covering the
same pixel (e.g. due to internal splitting) is recorded only once, with
its minimum depth. Each layer stores the submission key, the root-local
`DMirNodeBox`, the `data_ref`, the `DMirDepth` and the linear
non-premultiplied RGBA returned by your material callback.

### Capacity and truncation

`dmir_deep_begin(framebuffer, K)` sets the per-pixel capacity (clamped to
1..32). Only the **K nearest** distinct samples are kept per pixel, sorted
near-to-far (ties broken by submission key, then box level/x/y/z); farther
layers beyond K are dropped and treated as air by the compositing. Samples
with `alpha == 0` never occupy a layer, and layers behind an `alpha == 1`
layer are still recorded. Only samples strictly nearer than the depth
buffer's content at `dmir_deep_begin` are captured (equal depth is
rejected); the capture never modifies the old depth/voxel contents or the
occlusion state. The result is independent of the submission order, the
batcher's sorting order, and of splitting the renderer's rect into
non-overlapping parts.

### Call order

```c
dmir_framebuffer_clear(fb);
// ... optional opaque pre-pass via dmir_batcher_add(...) ...
dmir_deep_begin(fb, K);                 // snapshots the old depth
for (each batch) {
    dmir_batcher_reset(batcher, viewport, frustum);
    dmir_batcher_add_deep(batcher, fb, key, material_cb, user,
                          group, cage, geometry, root, effects);
    // ... more submissions; keys must be unique per frame ...
    dmir_batcher_sort(batcher);
    dmir_renderer_draw(renderer);       // consecutive draws accumulate
}
// read layers (owned by the framebuffer):
uint32_t n = dmir_deep_count(fb, x, y);
const DMirLayer* layer = dmir_deep_get(fb, x, y, i);
// or composite over a linear RGB background:
float T = dmir_deep_composite(fb, x, y, background_rgb, out_rgb);
dmir_deep_end(fb);                      // opaque rendering resumes normally
```

The material callback (`void (*)(void* user, DMirAddress data_ref,
float* rgba)`) and its user pointer must stay valid until the end of the
draw calls that use them. Layer pointers stay valid until the next
`dmir_deep_begin`, `dmir_framebuffer_clear`, `dmir_framebuffer_resize` or
`dmir_framebuffer_free` (all of which discard the capture). Only
`DMIR_SHAPE_RECT` submissions are supported in the capture mode; other
shapes keep using `dmir_batcher_add`. See `examples/deep_headless.cpp`
for a complete window-less example that renders two translucent voxel
slabs to a PPM file.

Example screenshot from the demo (using a voxelized version of ["medieval kind of seaport" by tokabilitor](https://www.blendswap.com/blends/view/50534)):

![Example screenshot](screenshot.png)
