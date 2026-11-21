# Surface Guides (CPU offscreen preview)

The CPU renderer can output a second set of per-pixel *surface guide* AOVs
next to the regular RGBA beauty: instance label, primary ray distance,
world space shading normal, and hit coverage. The data is captured from
the same primary ray intersection that also feeds the regular shading, so
enabling it never re-renders the scene, never re-issues geometry queries,
and never re-evaluates materials. Recursive tracing (reflection,
refraction, shadow, diffuse) does not overwrite the primary record.

Guides describe the nearest surface only. Material transparency does not
matter, and volumes never contribute; the beauty render is unaffected by
volumes either way.

## API

```cpp
#include "fj_surface_guides.h"

fj::SurfaceGuideBuffer guides;      // owned by the caller
renderer.SetSurfaceGuides(&guides); // NULL disables the output (default)
renderer.SetGuideThresholds(.1, .05);
renderer.RenderScene();             // resets the buffer to the resolution
```

`ObjectInstance::SetGuideID(uint32_t)` assigns the label of an instance.
It defaults to `1`. `0` is reserved for the background and passing it
throws `std::invalid_argument`. Different instances may share a label.

`Renderer::SetGuideThresholds(Real depthRange, Real normalError)` tunes
the guide aware adaptive subdivision. Defaults are `0.1` and `0.05`. Both
values must be finite and positive, and `normalError` must not exceed `2`;
invalid values are ignored and the previous thresholds are kept.

## Per-pixel fields (`fj::SurfaceGuide`)

| field      | type       | meaning |
|------------|------------|---------|
| `id`       | `uint32_t` | label that collected the largest Gaussian filter weight (background weight included); ties go to the smaller id. `0` = background. |
| `depth`    | `Real`     | filter-weighted mean of the primary ray distances (`t_hit`) of the winning label's samples. `0` when the winner is the background. |
| `normal`   | `Vector`   | filter-weighted mean of the winning label's world space shading normals, normalized at the end. `(0,0,0)` when the winner is the background. |
| `coverage` | `Real`     | total filter weight of non-background samples divided by the total filter weight. |

Integer labels are never averaged, and no single center ray replaces the
filter: every sample of the pixel votes with the same Gaussian weight used
for the beauty reconstruction. Pixels with an empty sample set or zero
total weight come out all zero, as do pixels outside the render region.

## Adaptive sampling

With guides enabled, an adaptive cell is subdivided when its four corners
show different labels, when the depth range of corners sharing the same
non-zero label exceeds `depthRange`, or when any pair of unit corner
normals has `1 - dot` above `normalError`. The classic color criterion
still applies and `max_subdivision` still caps the finest cells. Objects
that no cell corner can see are not guaranteed to be found.

Only cells whose four corner labels agree bilinearly interpolate guides
into untraced grid points (depth is lerped, normals are lerped then
normalized, a zero vector stays zero). Mixed-label cells never fabricate
a new label, and guides of grid points that were really traced are never
overwritten by a neighboring cell's interpolation.

With guides on, jitter off, and time sampling off, the guide values agree
within `1e-6` across tile sizes and thread counts. With guides off the old
RGBA path is unchanged; with the fixed sampler the beauty is identical
whether guides are on or off.

## Build and run

```sh
# builds the library (including src/fj_surface_guides.cc) and the example
bash task_support/build.sh
c++ -std=c++11 -O1 -pthread -I. -Isrc examples/surface_guides.cpp \
    .task-build/fj_surface_guides.o .task-build/libfujiyama.a \
    -o .task-build/example

# writes .task-build/guides.ppm (false-color labels weighted by coverage)
.task-build/example

# runs the surface guide test groups plus the old CPU render baseline
bash task_support/run.sh target
```

Everything is C++11 and uses only the standard library.
