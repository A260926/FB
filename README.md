# zgotrace
## Page-resident Bezier micro-meshes (`raytrace/hitables/patch_pages.go`)

`PatchDesc{Control [16]mgl64.Vec3, Material raytrace.Material}` describes a
bicubic tensor-product Bernstein surface over `[0,1]^2`.  Control points are
stored **v-row major, u-column**: `Control[v*4+u]`, where `u` runs left to
right and `v` bottom to top.

`NewPatchSet(patches []PatchDesc, level, residentPages int) (*PatchSet, error)`
validates and copies the descriptions.  `level` must be in `[3,10]`,
`residentPages` must be at least 1, every control point must be finite, every
material must be non-nil, and an empty slice is allowed.  A `PatchSet` is a
`raytrace.Hitable`, so existing `BvhNode`s, materials and the
`MonteCarloTracer` use it directly.

### Parameter grid and discrete geometry

The parameter square is divided into `N = 2^level` cells per axis.  Surface
positions are evaluated at the `(N+1)x(N+1)` grid vertices, and each cell is
triangulated as `(00,10,11)` and `(00,11,01)` in that order.  No coarser mesh is
substituted: a hit is always against one of these `2*N*N` triangles.  Normals
are the oriented geometric normals of the discrete triangles; UVs are the
barycentric interpolation of the triangle's parameter vertices.  Degenerate
triangles (zero-area or non-finite) are skipped.

Hits return the raw ray parameter `t` of `origin + t*direction`; the direction
need not be normalized, and the `[tMin,tMax]` window is closed.  Exact ties are
resolved in this order: patch index, then cell in v-row major order
(`cell = vIndex*N + uIndex`), then triangle order inside the cell.

### Page residency

Each `8x8`-cell block is one **page**.  Page bounds are computed ahead of time
from the control net of the corresponding parameter sub-domain (Bezier convex
hull property), and those bounds are organized in a lazy quadtree.  Only
conservative control-net bounds and hierarchy tests run before a page is
needed; triangle data is generated on the first query that reaches the page.
Building every page's bounds and index is allowed; building every micro-triangle
up front is not.

The resident requirement is measured in pages: one page's triangulated micro-
mesh **plus** its local triangle acceleration structure counts as one page.
Pages evicted during a query and pages held temporarily by a query all count
against `residentPages`, so residency never exceeds the budget.  Active pages
are protected by the same serial lock that guards `Hit` and `Stats`; a hit never
releases a page another query is using, and no query ever observes residency
above the configured cap.  `Hit` and `Stats` are safe to call concurrently; the
shared cache is serialized (no parallel speedup is claimed).

`Stats()` reports:

- `Resident` — pages currently cached.
- `PeakResident` — high-water mark of `Resident` (never above the cap).
- `PageBuilds` — pages whose triangle data was generated.
- `Evictions` — pages discarded by eviction or invalidation.
- `TriangleTests` — triangle intersection attempts.
- `BoundsTests` — conservative box tests (hierarchy and per-page structure).

All counters are incremented only where the corresponding work actually occurs.

### Updates and shared roots

`ReplacePatch(index int, desc PatchDesc) error` is for use **between frames**.
An out-of-range index or an invalid description changes nothing.  On success the
patch keeps its index and all of that patch's cached pages are invalidated, so
the next query cannot observe old control points or old material.
`ReplaceAll(patches []PatchDesc) error` replaces the whole set atomically: the
entire batch is validated before anything is committed, and `level` and the
residency budget are unchanged.

`BvhNode.Refit(time0, time1 float64)` recomputes boxes bottom-up along the
existing children.  It does not re-sort, allocate a replacement tree, or change
leaf identity.  The intended discipline is: call `ReplacePatch` (or
`ReplaceAll`), then `Refit` the original root that contains the set.  A single
`PatchSet` may be shared by several roots; each root must be refitted
separately.  Multi-level trees refit recursively.  Refit works for a set that
was empty and later populated.

The empty set's `GetBoundingBox` returns a non-nil empty box with
`Min = (+Inf,+Inf,+Inf)` and `Max = (-Inf,-Inf,-Inf)`.  Empty boxes never hit,
and merging an empty box into another box does not enlarge it; `AABB` now
handles this directly.

### Unsupported surface-light interface

`PatchSet.GetPdfValue` always returns `0` and `PatchSet.Random` always returns a
fixed direction (`UnitX`).  The micro-mesh is **not** a surface light: it is not
sampled by `HitablePdf`/`MixturePdf`, so use non-emissive patch materials and
keep light sampling on the existing light objects.

### Example

`examples/patch_pages/main.go` takes an output directory and renders
`before.png` and `after.png` with the real `Renderer`, an original sphere, a
diffuse light and the new patch.  Between the frames it edits the control net
with `ReplacePatch` and refits the original `BvhNode`; both images contain patch
pixels and differ.
