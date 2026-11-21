# MiniLight C++ — CPU diffuse suffix reconnection

This repository keeps the original MiniLight path tracer unchanged and adds
one optional mode: **CPU diffuse suffix reconnection** (`PathReuse`).
Neighbouring camera pixels reuse one real indirect suffix instead of each
re-tracing the same tail of a diffuse path.

## What the mode does

For every pass the renderer:

1. Projects all camera rays with the real `Camera` (same sub-pixel jitter) and
   freezes each pixel's first hit `x`, the unit normal facing the camera and
   the hit triangle identity.
2. For every valid source pixel, draws one cosine-weighted edge from the
   camera-facing side of `x` to a first hit `Y = (y, triangle, side)`, or an
   empty candidate if the edge misses every surface.
3. Traces exactly **one** real finite-depth diffuse suffix per generated
   candidate and records the outgoing estimate `B` at `y`: the self-emission
   of `y` plus later reflections, with no `x` albedo, source-edge cosine or
   source proposal density included.
4. Reconnects each receiver pixel to the candidates of itself and its four
   cardinal neighbours using area-density weights, then accumulates one linear
   colour per pixel.

There is no Russian roulette and no explicit emitter sampling in this mode:
only a bounded cosine random walk (maximum surface vertices `D`, `2 <= D <=
12`) that keeps each hit's single-sided self-emission and continues by the
albedo. The sky and ground background must both be black.

The source set `S` of a pixel is itself and its up/down/left/right neighbours
with valid first hits, deduplicated and fixed before any candidate is sampled.
Empty and black candidates still occupy a source slot. For a candidate `Y`,
the area density of source `k` is

```
q_k(Y) = max(0, n_k . w_ky) / pi * | n_y . (-w_ky) | / r^2
```

and is zero unless the ray from `k` first hits the recorded triangle at the
recorded position on the recorded side. The receiver colour is the first-hit
emission plus, for every source `j`,

```
F_i(Y_j) / sum_{k in S} q_k(Y_j)
```

with `F_i` built from the receiver albedo/pi, the receiver cosine, the
absolute cosine at `y`, the squared distance and the recorded `B`, zeroed on
occlusion, side mismatch or any degenerate (coplanar / zero-distance /
grazing) connection. Every visibility query goes through the original
`Scene` spatial index.

## Usage

```cpp
#include "Camera.hpp"
#include "Scene.hpp"
#include "Image.hpp"
#include "PathReuse.hpp"

using namespace hxa7241_minilight;

Camera camera( modelStream );                 // original, unchanged
Scene  scene ( modelStream, camera.getEyePoint() );
Image  image ( sizeStream );                  // original, unchanged

PathReuse reuse;                              // default ctor is unbound
reuse.bind( camera, scene,
            image.getWidth(), image.getHeight(),
            /*D*/ 4, /*seed*/ 12345u );       // explicit seed is optional

for ( int32 pass = 0; pass < 8; ++pass )
{
   if ( !reuse.runPass( camera, scene, image ) )
      break;                                  // a zero-density transport error
}

image.getFormatted( out, reuse.getCompletedPasses() );   // original tonemap
```

`bind` throws `std::invalid_argument` when `D` is outside `[2,12]`, the image
size is invalid, or the background is not black. Calling `runPass` with a
different `Camera`, `Scene` or image size throws instead of reusing stale
cache; call `clear()` and `bind()` again for a new configuration.

### Read-only state

- `getCompletedPasses()`, `getLastSuffixCount()`, `getTotalSuffixCount()`,
  `getLastValidSourceCount()`, `getDensityErrorCount()`
- `getLinearPixel(x, y)` — linear accumulated colour (never the display image)
- `getCandidatePixel(x, y)` — colour produced by the most recent pass
- `getFirstHit` / `getCandidate` — frozen geometry and the recorded suffix
- `getSourceSet(i, sources)` — the deduplicated neighbour source set
- `sourceDensity(k, scene, j)` and `receiverContribution(i, scene, j)`
- `getDiagnostics(...)` — density sum/min/max, zero-density and occlusion
  counts

Each `PathReuse` instance is independent of every other instance.

## Compatibility

The old CLI, scene format, `Camera::getFrame`, `RayTracer::getRadiance` and
the default `Random` stream are untouched, including their random-number
consumption. The only additions to existing classes are new, non-virtual
methods (`Camera::getPixelRayDirection`, `Scene::isBackgroundBlack`,
`Random(seed)`); no existing method changed behaviour. The PPM output still
uses the original Ward tone mapping, applied to the linear accumulation.

## Example and tests

- `examples/reuse_room.cpp` — takes one output directory and writes
  `reuse.ppm`, `linear.txt` and `stats.txt`, reporting the pass count and the
  real suffix trace count (one suffix per valid source per pass).
- `tests/path_reuse.cpp` — `measure`, `transport` and `passes` groups driven by
  `task_support/driver.c`; it defines no `main` of its own.

Build and run everything with:

```
bash task_support/run.sh target
```

Only the C++17 standard library and the sources in this repository are used;
there is no windowing or GPU dependency.
