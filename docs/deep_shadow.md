# Point-light deep shadow map

A reusable, multi-layer transmittance cache for `PointLight`. An ordinary
shadow map stores only the nearest depth, so every translucent sheet behind
the first one is lost. A deep shadow map instead stores, per cube-map texel,
a staircase of cumulative transmittance as a function of radial distance from
the light. Shadow lookups then read the transmittance at the receiver without
tracing any geometry or evaluating any shader.

## What gets stored

For every texel of six cube faces (+X, -X, +Y, -Y, +Z, -Z) the cache keeps an
ordered list of right-continuous breakpoints:

```
struct DeepShadowBreakpoint { Real distance; Real transmittance; };
```

A staircase starts at `T = 1`. Each hit layer multiplies the running
transmittance by `1 - clamp(Os, 0, 1)`, where `Os` comes from the surface
shader evaluated under `CXT_SHADOW_RAY`. A surface without a shader is treated
as opaque (`Os = 1`), driving transmittance to zero.

- `T(d)` includes every breakpoint whose `distance <= d`.
- Distinct `(instance, primitive)` pairs are distinct layers, even when they
  sit at the same radial distance.
- Layers at identical distance are folded into one breakpoint carrying the
  product of all their factors.
- The cache owns all of its data. It keeps no reference to any
  `Intersection`, shader or geometry; if the scene changes, rebuild it.

## Building the cache

```cpp
#include "fj_point_light.h"
using namespace fj;

PointLight light;
light.SetColor(1, 1, 1);
light.SetIntensity(1);

// The context selects the target object group and the sample time.
TraceContext cxt = SlCameraContext(&shadow_group);
cxt.ray_context = CXT_SHADOW_RAY;
cxt.time = 0.0;
cxt.trace_target = &shadow_group;

const int  resolution  = 16;   // texels per cube edge, must be > 0
const Real maxDistance = 10.;  // world units, must be > 0

const bool ok = light.BuildDeepShadowMap(cxt, resolution, maxDistance);
if (!ok) {
  // invalid arguments or unsupported target geometry; see below
}
```

`BuildDeepShadowMap` returns `bool` (`false` on failure). The light position is
the same transform sample `GetSamples()` uses (time 0; light motion sampling is
not extended). Instance transforms and mesh velocity are sampled at
`cxt.time`, and hit parameters do not change when the local ray direction's
length changes.

### Supported and rejected geometry

- Supported: triangle `Mesh` surfaces only, reached through the existing
  top-level object BVH and each mesh's BVH or uniform grid.
- Explicitly rejected (build returns `false`): `Curve`, `PointCloud` and
  `Volume` targets, non-positive `resolution` or `maxDistance`.
- Shaders are evaluated with a shadow context whose secondary trace depths
  are zero, so a capture never recurses into the scene.

### How candidates are gathered

Capture never scans the whole scene per ray and never re-issues nearest-hit
queries with epsilon marching.

1. The top-level BVH is walked with a fixed ray range; every object instance
   whose bounds the ray crosses is a candidate.
2. Each candidate mesh's BVH/grid is walked the same way, collecting every
   triangle hit inside the range, not just the nearest.
3. A triangle referenced by several grid cells is tested once per ray
   (candidate ids are de-duplicated).

## Looking up visibility

The base `Light` class gains a virtual hook with a default fallback:

```cpp
virtual bool LookupShadowVisibility(const TraceContext &cxt,
    const Vector &receiver, Real bias, Real *visibility) const;
```

The base implementation returns `false`. `PointLight` implements it. The call
returns `false` (meaning "use the ordinary shadow ray") when:

- no cache has been built, or it was cleared;
- the context's target group is not the group the cache was built for;
- the context time differs from the build time;
- the receiver is farther than the captured `maxDistance`.

On success it writes a `[0, 1]` visibility and returns `true` without touching
geometry or shaders.

### Filtering

The receiver direction is mapped to its cube face and `(u, v)`. The four
integer neighbours of `floor(u*N - 0.5), floor(v*N - 0.5)` receive ordinary
bilinear weights. Each neighbour first evaluates **its own** staircase at the
receiver's radial distance minus `bias`; the four transmittances are then
averaged. Hit distances are never interpolated before the staircase lookup.
Out-of-face neighbours are remapped through the neighbouring face; the exact
coordinate and tie-breaking rules are in
`task_support/cube_contract.md`.

## Integration with `SlIlluminance`

`SlIlluminance` keeps the original light intensity and sampling unchanged.
When `cast_shadow` is on it first tries the cache with a world-space bias of
`0.0001`:

- cache hit: the visibility multiplies the direct light colour, with no
  geometry or shader work;
- cache miss (`false`): the existing `SlTrace` shadow-ray path runs exactly as
  before.

When `cast_shadow` is off the cache is not consulted.

## Invalidation and lifetime

Any transformation entry point on the `Light` base class
(`SetTranslate`, `SetRotate`, `SetScale`, `SetTransformOrder`,
`SetRotateOrder`) drops a point light's cache, because the light position or
orientation no longer matches the captured directions.

```cpp
light.ClearDeepShadowMap();   // drop the cache explicitly
```

Rebuilding and changing light transforms are expected to happen outside
rendering. Once built, the cache supports concurrent read-only lookups from
multiple render threads; it performs no mutation during a query.

## Regression coverage

`task_tests/deep_shadow.cpp` registers cases in the `capture`, `lookup` and
`transport` groups via `TASK_CHECK` (no `main`). It checks per-layer
transmittance, same-distance instances, opaque and unshaded surfaces,
rejection of curves/point clouds/volumes and bad arguments, grid cell
de-duplication, transform/velocity motion, four-neighbour and cross-face
filtering, invalidation, real `SlIlluminance` results for both the cached and
fallback paths, cast-shadow-off behaviour, out-of-range fallback, and
multithreaded read-only queries. A scene with more than 1024 separated
triangles also asserts that the triangle-test count during capture is below
one eighth of the brute-force `rays * triangles` budget.

Run everything with:

```
bash task_support/run.sh target
```
