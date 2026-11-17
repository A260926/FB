![Discrete Mirage](logo_main.png)

# discrete-mirage

A C library for rendering voxel models on CPU.

Currently, it's more of a prototype / proof-of-concept, so use at your own risk. If you do, no attribution is necessary, but a mention would nevertheless be appreciated :-)

The demo folder contains a [C++ example](demo/src/main.cpp) of using the library to render a small test scene. Once you manage to build the project, run the demo with a command line argument specifying a path to the octree file (a couple of such files can be downloaded from [here](https://github.com/dairin0d/OctreeSplatting/releases)).

If you're curious about the details, take a look at the [notes](docs/notes.md).

## Per-pixel motion vectors

For temporal reprojection, the framebuffer exposes an optional motion
channel: `framebuffer->motion` (two `float`s per pixel, `dx` then `dy`,
using the same row stride as the other channels) and
`framebuffer->motion_valid` (one byte per pixel). When valid, the vector
is *previous screen position minus current screen position*, in pixels.

To populate it, submit an instance with `dmir_batcher_add_motion(...)`
instead of `dmir_batcher_add(...)`. The trailing `const DMirMotionInput*`
describes where the same instance was during the previous frame:

```c
typedef struct DMirMotionInput {
    float previous_cage[24];        // previous view-space cage (same layout as "cage")
    DMirRect previous_viewport;     // previous viewport
    DMirFrustum previous_frustum;   // previous projection parameters
} DMirMotionInput;
```

The current frame still uses the `cage` argument, the batcher's viewport and
the batcher's frustum. The batcher copies everything it needs at call time,
so the input may be modified or freed immediately afterwards. The same
geometry (even the same root) may be submitted several times with independent
motion data, and `group` values do not have to be unique.

Motion is sampled at the center of the spatial node that actually supplied
the pixel's voxel reference. Both cages are trilinearly interpolated at that
node's root-local `[0,1]^3` center and projected with their own parameters.
Nodes that share a `data_ref` are still sampled at their own centers, so
their motion vectors are not merged. If the previous point falls outside the
previous `[min_depth, max_depth]` range, the pixel is marked invalid and its
vector is zeroed; previous-frame occlusion is not considered.

Entries added with the plain `dmir_batcher_add(...)` keep producing pixels,
but those pixels are always marked as having no valid motion. This extension
covers `DMIR_SHAPE_RECT` splats only; other shapes keep their original
behavior. `dmir_framebuffer_clear`, `dmir_framebuffer_resize` and
`dmir_batcher_reset` all reset the motion state.

A windowless two-frame example lives in
[examples/two_frame_motion.cpp](examples/two_frame_motion.cpp); build it with
CMake (target `two_frame_motion`) or manually as described at the top of the
file.

If you're curious about the details, take a look at the [notes](docs/notes.md).

Example screenshot from the demo (using a voxelized version of ["medieval kind of seaport" by tokabilitor](https://www.blendswap.com/blends/view/50534)):

![Example screenshot](screenshot.png)
