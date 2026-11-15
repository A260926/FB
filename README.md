# Deforming triangle-mesh area lights

This tree extends pbrt-v1 with triangle meshes whose vertices move
during the camera shutter, usable directly as motion-blurred area
lights. Geometry, occlusion and emissive sampling always observe the
same instant along a light path: the time carried by the ray.

## What "time" means

- Every `Ray` has a `float time`. The perspective camera sets it by
  interpolating the shutter interval (`shutteropen`/`shutterclose`)
  with the sample's `time` value.
- A deforming `trianglemesh` takes an optional vertex array `P1`, the
  same length as `P`, plus `motionstart` (default `0`) and
  `motionend` (default `1`). The world-space vertex at ray time `t`
  is the linear interpolation of `P` and `P1`; times outside
  `[motionstart, motionend]` are clamped to the nearest endpoint.
- Intersection, the geometric normal, UVs and the tangent
  differentials are all evaluated on the triangle interpolated to
  `ray.time`. The mesh and its grid/kd-tree acceleration structure are
  built once from a bounding box that covers both motion endpoints, so
  nothing is rebuilt per ray.
- `ObjectToWorld` stays static; non-uniform scaling and reflection are
  allowed. Deforming meshes cannot carry `N`/`S` vertex arrays.
- If a triangle has exactly zero area at some instant, rays at that
  time do not hit it and its sampling density/normal are zero (never
  NaN).
- A mesh without `P1` behaves exactly like the original static mesh.
- Photon emission and `Light::Power` keep using time zero; volume
  motion and relativistic propagation are out of scope.

## Time-aware sampling API

`Shape` gains time-taking overloads alongside every original entry
point; the original entry points mean time zero and are unchanged for
static shapes:

- `Area(float time)`
- `Sample(float time, float u1, float u2, Normal*)`
- `Sample(float time, const Point&, float u1, float u2, Normal*)`
- `Pdf(float time, const Point&)`
- `Pdf(float time, const Point&, const Vector&)`

`Light`/`AreaLight` gain matching `Sample_L(float time, ...)` and
`Pdf(float time, ...)` overloads, reachable through a `Light*` with or
without the shading normal. `ShapeSet` picks a sub-shape from its
world-space area CDF at the requested time, remaps `u1` into the
chosen shape's interval, leaves `u2` for the in-shape second
dimension, and computes its solid-angle density as the area-weighted
sum over every emissive surface a direction crosses.

Integrators (`directlighting` all/one/weighted and `path`) propagate
the camera ray's time through light sampling, visibility shadow rays,
the BSDF-sampling MIS branch, specular reflection/refraction and path
continuations, including ray differentials.

## Building and running

No EXR, windowing or downloaded assets are required.

- Static area-light baseline only:
  - `bash task_support/run.sh baseline`
- Full target (baseline, the three test groups `geometry`, `density`
  and `transport`, plus the example):
  - `bash task_support/run.sh target`

The tests live in `tests/deforming_light.cpp` (registered through
`task_cases`/`task_case_count`, no `main`). The example
`examples/deforming_light.cpp` takes an output directory as `argv[1]`
and writes `motion.ppm`, rendering one shared `Scene` at three shutter
times (0, 0.5, 1) via the production `Scene::Li`.
