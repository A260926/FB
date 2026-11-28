# tinynurbs ray scene

This directory adds direct ray intersection against rational NURBS surfaces to
the header-only tinynurbs library. Rays are intersected with the actual
parametric surfaces — no triangulation, fixed sampling or tessellation is
involved.

All new code is C++17 and uses only the bundled GLM. The existing evaluation,
knot-insertion and splitting APIs are unchanged.

## Files

- `include/tinynurbs/core/ray_scene.h` — public API (re-exported from
  `tinynurbs/tinynurbs.h`).
- `src/ray_scene.cpp` — implementation.
- `checks/test_ray_scene.cpp` — registered `roots` / `updates` / `bounds`
  checks (no `main`; driven by `task_support/driver.c`).
- `examples/ray_scene.cpp` — renders a non-planar rational surface to a PPM.

## Public types

All symbols live in `namespace tinynurbs` and use `double` / `glm::dvec3`.

### `RaySurfaceScene`

Owning collection of `RationalSurface3d` surfaces.

```cpp
explicit RaySurfaceScene(const std::vector<RationalSurface3d>& surfaces);
```

The constructor deep-copies every surface, so releasing or mutating the input
vector afterwards never affects the scene. It throws `std::invalid_argument`
for unsupported surfaces. Copy/move semantics are value-based and the scene
owns all of its data.

Supported input surfaces:

- 3D rational surfaces, degrees 1–3 in both parameter directions;
- clamped, non-decreasing knot vectors, with interior knot multiplicity at
  most equal to the degree (multiplicity may reach the degree);
- strictly positive, finite homogeneous weights;
- finite control points and regular geometry.

Trimmed and periodic surfaces are not supported.

#### Queries

```cpp
RayQueryResult intersect(glm::dvec3 origin, glm::dvec3 direction,
                         double tmin, double tmax,
                         const RayQueryOptions& options = {}) const;
```

Finds every intersection `origin + t * direction` with `t` in the
half-open interval `[tmin, tmax)`. The direction is **not** normalized.
Validation failures are returned through `RayQueryResult::error` before any
ray work is done; on error there are no hits and no unresolved records.

Hits are sorted by `(t, surface_index, u, v)`. Roots with different parameter
values are always kept separately, even when they coincide in world space.
The only merging performed is the same parameter root on the same surface
rediscovered across a knot boundary (or shared subdivision box).

#### Batched control point edits

```cpp
RaySceneEditResult updateControlPoints(const std::vector<ControlPointEdit>& edits);
```

Applies absolute position/weight replacements. Degrees and knots never change.
The whole batch is validated before any mutation, so any rejected batch leaves
the scene untouched and queryable. After a successful batch only the knot
elements whose B-spline basis support contains an edited point are
re-extracted, and only their ancestor hierarchy nodes are refit; tree
topology, leaf numbering and unaffected elements stay the same. An empty batch
performs no extraction. Repeated edits to the same control point use the
absolute new value; the input vector may be released afterwards.

#### Counters

```cpp
RaySceneStats stats() const;
std::size_t      surfaceCount() const;
```

### `RaySurfaceHit`

One verified intersection.

| Member | Meaning |
| --- | --- |
| `std::size_t surface_index` | index of the surface in the constructor vector |
| `double u, v` | root parameters in the surface's original domain |
| `double t` | ray parameter, `point == origin + t * direction` |
| `glm::dvec3 point` | back-substituted point on the real surface |
| `glm::dvec3 normal` | unit normal oriented along `Su x Sv` |

On an interior knot line the normal is evaluated on the right-hand element;
at the upper domain bound the last (left-hand) element is used. The point and
the `O + tD` residual are both bounded by the query's position tolerance.

### `RaySurfaceUnresolved`

A parameter region that could not be classified as hit or miss because of
tangency, degeneracy or an exhausted box budget. It is never reported as a
miss, and the owning query is marked incomplete.

| Member | Meaning |
| --- | --- |
| `std::size_t surface_index` | owning surface |
| `double u_min, u_max, v_min, v_max` | UV envelope of the unclassified region |

Adjacent unresolved boxes are coalesced, so each record is a true envelope of
the underlying region.

### `RayQueryOptions`

| Member | Default | Meaning |
| --- | --- | --- |
| `double position_tolerance` | `1e-8` | world-space tolerance on the `O+tD` back-substitution residual |
| `double parameter_tolerance` | `1e-9` | parameter-space tolerance |
| `std::size_t box_budget` | `100000` | maximum number of conservative boxes visited |

### `RayQueryResult`

| Member | Meaning |
| --- | --- |
| `RaySceneError error` | validation outcome (`None` when the query ran) |
| `bool complete` | `false` if any unresolved record was produced |
| `std::vector<RaySurfaceHit> hits` | sorted, verified intersections |
| `std::vector<RaySurfaceUnresolved> unresolved` | unclassified UV regions |
| `std::size_t box_visits` | actual conservative box/ray tests performed |
| `std::size_t patch_boxes_tested` | subset of `box_visits` spent inside leaf patches |

The counters are measured per query and are never constants.

### `ControlPointEdit`

| Member | Meaning |
| --- | --- |
| `std::size_t surface_index, row, column` | absolute address of one control point |
| `glm::dvec3 position` | new position |
| `double weight` | new strictly-positive weight |

### `RaySceneEditResult`

| Member | Meaning |
| --- | --- |
| `RaySceneEditError error` | batch validation outcome |
| `std::size_t reextracted_elements` | leaf elements actually re-extracted (`0` for an empty batch) |
| `std::size_t refit_nodes` | hierarchy nodes refit in place |

### `RaySceneStats`

| Member | Meaning |
| --- | --- |
| `std::size_t surface_count` | number of owned surfaces |
| `std::size_t total_elements` | total Bezier elements across surfaces |
| `std::size_t total_tree_nodes` | total bounding-hierarchy nodes |
| `std::size_t element_extractions` | cumulative element extractions (build + updates) |

## Error categories

### `enum class RaySceneError`

| Value | Trigger |
| --- | --- |
| `None` | query accepted |
| `ZeroDirection` | direction is the zero vector |
| `NonFiniteInput` | origin or direction contains NaN/infinity |
| `InvalidInterval` | `tmin`/`tmax` non-finite or `tmin >= tmax` |
| `NonPositiveTolerance` | a tolerance is non-finite or `<= 0` |
| `ZeroBudget` | `box_budget == 0` |

### `enum class RaySceneEditError`

| Value | Trigger |
| --- | --- |
| `None` | batch applied |
| `SurfaceIndexOutOfRange` | unknown surface index |
| `ControlPointIndexOutOfRange` | row/column outside the control grid |
| `DuplicateAddress` | the same `(surface, row, column)` appears twice |
| `NonFiniteValue` | position or weight contains NaN/infinity |
| `NonPositiveWeight` | weight `<= 0` |

## Numerical method

1. Each homogeneous control net is decomposed once into rational Bezier
   elements (one per non-empty knot span) using the library's own
   `curveSplit`. With strictly positive weights each rational element lies in
   the convex hull of its projected homogeneous control points, giving a
   conservative world-space box.
2. Element boxes are organized in a per-surface bounding hierarchy. A query
   walks these hierarchies with exact ray/box slab tests, so distant patches
   are rejected at high-level nodes.
3. Surviving leaf patches are adaptively subdivided by de Casteljau in
   homogeneous space. Every visited box is conservative; a strict
   transverse-frame convex-hull test additionally excludes patches whose hull
   provably avoids the ray line. Fixed sampling and triangular approximations
   are never used.
4. Roots are found on the **real parametric surface** with a full 3×3 Newton
   solve of `F(t,u,v) = O + tD - S(u,v)`. Keeping `t` as an unknown keeps the
   Jacobian `[D, -Su, -Sv]` regular at grazing double roots where a 2D
   parameter projection degenerates to linear convergence. A single Newton
   start from one box is never relied upon: subdivision supplies independent
   enclosing boxes, and an iterate is only accepted after containment,
   back-substitution and residual checks.
5. Tangent, degenerate or budget-exhausted regions that cannot be excluded are
   returned as unresolved and mark the query incomplete; unverified iterates
   are never reported as hits.

## Example

```cpp
tinynurbs::RaySurfaceScene scene(surfaces);
auto result = scene.intersect(origin, direction, 0.0, 1e9,
                              tinynurbs::RayQueryOptions{});
if (result.error != tinynurbs::RaySceneError::None) { /* reject */ }
for (const auto& hit : result.hits)
    /* hit.surface_index, hit.u, hit.v, hit.t, hit.point, hit.normal */;
if (!result.complete)
    for (const auto& region : result.unresolved) /* region UV envelope */;
```

`examples/ray_scene.cpp` renders a rational quarter-cylinder band this way to
a 64×64 P3 PPM.
