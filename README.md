# tinynurbs ray scene

This repository extends [tinynurbs](docs/README.md) with direct ray
intersection against rational NURBS surfaces. Rays are intersected with the
true parametric surfaces — no triangulation or fixed sampling is involved.

The new code is C++17 and uses only the bundled GLM under `dependencies/glm`;
no other dependencies are added. The existing evaluation, knot insertion and
splitting interfaces remain unchanged.

## Files

- `include/tinynurbs/core/ray_scene.h` — public API (also exported from
  `include/tinynurbs/tinynurbs.h`)
- `src/ray_scene.cpp` — implementation
- `checks/test_ray_scene.cpp` — `roots`, `updates` and `bounds` task cases
- `examples/ray_scene.cpp` — renders a non-planar rational surface to a
  64×64 P3 PPM, shaded by surface normal

## Supported surface class

`RaySurfaceScene` accepts 3D `RationalSurface3d` surfaces only, with:

- degrees 1–3 in both parameter directions,
- clamped (open) continuous knot vectors; interior knot multiplicity may
  reach the degree,
- strictly positive finite weights and finite control points,
- a regular parametrization (`Su × Sv` does not vanish).

Trimmed and periodic surfaces are not supported. An unsupported surface
makes `status()` return `RaySceneError::InvalidSurface` and every query on
that scene is refused.

## Algorithm

1. Every surface is decomposed into its knot elements and each element is
   expressed as a local homogeneous Bézier control net via an extraction
   matrix. Convex hulls of those nets are conservative surface bounds.
2. A bounding hierarchy is built over the element leaves.
3. A query first prunes hierarchy nodes with a homogeneous ray-slab test
   (the ray window `[tmin, tmax)` is carried through the test). Surviving
   patches are subdivided in homogeneous space with de Casteljau, and roots
   on the actual rational surface are polished with damped Newton iteration
   on the two independent components of `D × (S(u,v) − O) = 0`.
4. A root is only reported after an independent transversality check and a
   world-space residual check against `spatial_tolerance`. Tangent contacts,
   degenerate regions and regions that cannot be excluded within the box
   budget produce unresolved records and mark the query incomplete.

## Public types

### `enum class RaySceneError`

| Value | Meaning |
| --- | --- |
| `None` | No error. |
| `InvalidSurface` | Construction input is outside the supported surface class. |
| `InvalidRay` | Zero direction or any non-finite ray component. |
| `InvalidInterval` | Non-finite interval end or `tmin >= tmax`. |
| `InvalidTolerance` | A tolerance is not a strictly positive finite number. |
| `InvalidBudget` | The box budget is zero. |
| `InvalidControlPoint` | An edit is out of range or has an illegal value/weight. |
| `DuplicateControlPoint` | An edit batch references the same control point twice. |

### `enum class UnresolvedReason`

`Tangency`, `Degenerate`, `Budget` — why a parameter region could not be
resolved to verified hits.

### `struct RayOptions`

- `double spatial_tolerance` (default `1e-8`) — world-space hit tolerance.
- `double parameter_tolerance` (default `1e-9`) — parameter tolerance.
- `unsigned long long box_budget` (default `100000`) — maximum number of
  parametric boxes processed.

### `struct RayHit`

`surface_index`, `u`, `v`, `t`, `position`, `normal`. The normal is unit
length and oriented by `Su × Sv`; on a knot line the right element is used,
and at the upper domain bound the left element. Hits are sorted by
`(t, surface_index, u, v)`. Distinct UV roots are all retained even when
spatially coincident; only the same parametric root rediscovered across a
knot boundary is merged.

### `struct RayUnresolved`

`surface_index`, `u_min`, `u_max`, `v_min`, `v_max`, `reason`.

### `struct RayQueryResult`

`complete`, `error`, `hits`, `unresolved`, `bvh_nodes_visited`,
`parameter_boxes_visited`, `boxes_visited`. The visit counters are measured
per query and are never constants. Results own their data and stay valid
after later scene updates.

### `struct ControlPointEdit`

`surface_index`, `row`, `column`, `position`, `weight` — one absolute
control point replacement.

### `struct RaySceneUpdateStats`

`accepted`, `error`, `elements_extracted`, `bvh_nodes_refit`.

### `class RaySurfaceScene`

- `explicit RaySurfaceScene(const std::vector<RationalSurface3d>&)` — copies
  the surfaces; the caller's input may be destroyed afterwards.
- `RaySceneError status() const`
- `std::size_t surfaceCount() const`
- `std::size_t elementCount(std::size_t surface_index) const`
- `std::size_t leafCount() const`, `std::size_t nodeCount() const`
- `unsigned long long totalElementsExtracted() const`
- `RayQueryResult intersect(const glm::dvec3& origin,
  const glm::dvec3& direction, double tmin, double tmax,
  const RayOptions&) const` — all intersections in the half-open window
  `[tmin, tmax)`; the direction is used un-normalized.
- `RaySceneUpdateStats updateControlPoints(
  const std::vector<ControlPointEdit>&)` — atomic batched absolute edits.

## Control point updates

A batch is validated before anything is applied: a duplicate address or any
illegal value rejects the whole batch and leaves the scene unchanged and
queryable. An empty batch does no work. On success, only the knot elements
supported by the edited basis functions are re-extracted, and only the
hierarchy nodes above those leaves are refit; tree topology and leaf
numbering are preserved. Knots and degrees never change. Repeated edits of
the same control point across batches use absolute values, so queries
reflect the latest position and weight.

## Example

```cpp
tinynurbs::RaySurfaceScene scene(surfaces);
if (scene.status() != tinynurbs::RaySceneError::None) { /* ... */ }

tinynurbs::RayOptions opt; // 1e-8, 1e-9, 100000
auto result = scene.intersect(origin, direction, 0.0, 1e30, opt);
if (!result.complete) {
    // inspect result.unresolved, never silently treated as misses
}
for (const auto& hit : result.hits) {
    // hit.t, hit.u, hit.v, hit.position, hit.normal
}
```

Build and run the shipped example:

```sh
c++ -std=c++17 -O1 -Iinclude -Idependencies/glm \
    examples/ray_scene.cpp src/ray_scene.cpp -o ray_scene
./ray_scene out.ppm
```
