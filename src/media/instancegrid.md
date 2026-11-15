Instance grid media
===================

`type "instancegrid"` extends `MakeNamedMedium` with a heterogeneous density
field that can be placed many times through `ObjectInstance` without copying
the density array, the prototype geometry, or the prototype BVH.  This note
covers the parameter set, the coordinate conventions, the sharing and
ownership model, and the error behavior.

Definition and placement
------------------------

The medium is a *prototype resource*: it may only be created while an
`ObjectBegin` definition is active, and it is referenced from shapes inside
that definition through the ordinary `MediumInterface` directive.

```
ObjectBegin "smoke"
MakeNamedMedium "smoke"
    "string type" ["instancegrid"]
    "rgb sigma_a" [...] "rgb sigma_s" [...] "float g" [...]
    "integer nx" [...] "integer ny" [...] "integer nz" [...]
    "float density" [ ... nx*ny*nz values ... ]
    "point p0" [...] "point p1" [...]
    # optional: "preset" / "scale", resolved like heterogeneous
AttributeBegin
MediumInterface "smoke" ""
Material ""
Shape "sphere" "float radius" [1]
AttributeEnd
ObjectEnd

AttributeBegin
Translate ...   # any affine placement
ObjectInstance "smoke"
AttributeEnd
```

Every `ObjectInstance` of that prototype creates one small `InstanceGridMedium`
*view* per prototype medium.  The view holds two transforms and a
`shared_ptr` to the immutable density; the actual voxel array and the
prototype primitive BVH exist exactly once.

Only static, finite, invertible affine placements are supported.  Non-uniform
scale, shear and reflection are all fine; animated placements and animated
medium definitions are rejected.  The pre-existing restriction that
`ObjectBegin` may not nest is unchanged.  A prototype that is defined but
never placed contributes no world medium.

Coordinates
-----------

There are three frames:

* **density coordinates** — the normalized box `[0,1]^3` in which the voxel
  array lives.  Trilinear interpolation uses exactly the same kernel and
  half-voxel edge behavior as `GridDensityMedium`.
* **prototype coordinates** — the frame of the `ObjectBegin` definition
  body.  The transform in effect where `MakeNamedMedium` appears maps density
  coordinates into prototype coordinates; `p0`/`p1` place the unit cube
  inside that transform, just as for the heterogeneous medium.  The
  prototype geometry is expressed in the same frame.
* **world coordinates** — the frame of each `ObjectInstance`.  The placement
  transform maps prototype coordinates to world coordinates.

The medium view composes these once at placement time:

```
densityToWorld    = prototypeToWorld * densityToPrototype
worldToDensity    = inverse(densityToWorld)
```

`Tr` and `Sample` map the world ray origin by `worldToDensity` and the world
*direction* by the linear part, keeping `t` in world-distance units.  This is
what makes scale behave correctly: the direction carries the local stretch, so
a smoke stretched by 2x is traversed over twice the world length and
accumulates twice the optical thickness.  `sigma_a`, `sigma_s` and the derived
`sigma_t` remain per *world* unit length and are never rescaled by the
placement.  `p0`/`p1` scale the density box in prototype coordinates in the
same way the heterogeneous medium uses them.

`Medium::Sample` performs delta tracking in world-distance units and returns a
`MediumInteraction` whose position, phase-function direction and time are in
world coordinates; `GetMedium()` and the subsequent `SpawnRay` therefore keep
the placement's own medium identity.

Sharing and ownership
---------------------

* `InstanceGridDensity` is immutable after construction and owns the single
  voxel array.  It is created once per `MakeNamedMedium "instancegrid"` and
  is held through `shared_ptr<const InstanceGridDensity>`.  It contains no
  world-space state.
* `InstanceGridMedium` (one per prototype *definition* and one per placement)
  is a lightweight view.  Several views can reference the same density object
  concurrently; all queries are read-only, so they are safe to run from
  multiple threads at once.
* The prototype BVH is built once from the shapes defined inside
  `ObjectBegin`, exactly as for any other instanced prototype.  Each
  `ObjectInstance` becomes a single `TransformedPrimitive` wrapping that
  shared aggregate.
* A per-instance `MediumRemap` rewrites the medium pointers that the shared
  primitive stored (the prototype media, plus any ordinary world named
  media) to the placement's views.  It never touches empty (vacuum) slots or
  ordinary world named media, which keep their original identity.  The remap
  also owns the placement views, so their lifetime is tied to the primitives
  that reference them.
* Because all instances share one density object, no per-instance world-space
  grid is expanded.  The only per-instance extra data is the placement
  transform plus the view/remap objects described above.

Boundary behavior
-----------------

Intersections use the usual `MediumInterface` direction rule.  Inside a
prototype's `ObjectInstance`, every `instancegrid` medium in the interface is
mapped to the current placement's view; an empty reference stays vacuum, and a
normal world named medium keeps its identity.  Both materialized boundaries
and material-less boundaries follow the existing `MediumInterface` convention
(surfaces with a material terminate `IntersectTr`; material-less boundaries
are skipped and the ray continues in the medium selected by the new
interface).  Instances are treated as non-overlapping, matching the physical
setup.

Errors
------

Scene construction fails hard, rather than silently rendering without a
medium, for any of the following.  Each reports an explicit `Error` and
aborts the render at `WorldEnd`:

* `instancegrid` used outside an `ObjectBegin` definition (both in
  `MakeNamedMedium` and when an ordinary shape or light references the
  prototype name from the world block).
* `instancegrid` referenced by a light or shape that is not part of a
  prototype (there would be no placement to give it an identity).
* `instancegrid` referenced from a *different* `ObjectBegin` definition than
  the one that created it (there is no matching placement view to remap to).
* Invalid density: missing values, a count that does not equal
  `nx*ny*nz`, non-finite or negative values, all-zero density, or a degenerate
  `p0`/`p1` box.
* Dimension overflow: non-positive or overflowing `nx*ny*nz`, plus a product
  guard for the parameter/expectation computation.
* A density-to-prototype or placement transform that is singular,
  non-affine, or has non-finite entries.
* A motion (animated) `instancegrid` definition or placement.

Ordinary `heterogeneous`, `homogeneous` and medium-less instances are
unaffected.
