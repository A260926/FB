# zgotrace

## Portal scenes

`raytrace.PortalScene` connects several separate scene regions with rigid-body
portal doors. A portal gives a view into another region without rendering that
region to a texture first: rays, including the rays produced by specular and
diffuse scattering, cross the door directly and continue inside the
destination region's own coordinate system.

### Regions and frames

A `PortalRegion` bundles the solid `World` hitable (typically a BVH), an
optional `Lights` hitable used for next-event estimation, and a `Background`
function. Several doors may reference the same region; its BVH, materials and
lights are shared read-only and are never copied.

Each `Portal` carries two row-major 3x4 affine frames, `Entry` and `Exit`,
whose implicit fourth row is `[0 0 0 1]`. A frame is a rigid transform: its
3x3 rotation must be orthonormal with determinant +1, validated element by
element within a tolerance of `1e-9`.

* The door's local plane is `z = 0` and its aperture is the closed rectangle
  `[-HalfWidth, HalfWidth] x [-HalfHeight, HalfHeight]`.
* A ray crosses only when it travels from local `z > 0` toward local `z < 0`.
* On a crossing, the hit point and direction are transformed into the `Entry`
  frame, rotated by pi about the local Y axis, then transformed out through
  the `Exit` frame. Direction length is preserved and the new ray origin is
  nudged `1e-5` world units along the exit frame's `+Z`.
* The teleport costs no scattering depth, consumes no random numbers and does
  not change throughput. Hit records, material UV/normals, light proposals
  and the background all use the destination region's coordinates afterwards.

### One-way doors

Doors are strictly one-way. Rays arriving at the back of the door (local
`z < 0`) or outside the aperture never teleport and are traced normally in
the region they already occupy. A disabled portal (`Enabled == false`) does
not participate at all. When a solid surface and a portal are hit at the same
distance, the solid surface wins; between two portals at the same distance
the portal with the lower input index wins.

`maxHops` bounds the number of portal crossings for an entire pixel sample
path. The budget is shared across specular and diffuse scattering, so it is
never reset by a bounce. Attempting the `maxHops + 1` crossing returns black
for that path. A ray that misses every solid and every portal returns the
background of the region it currently occupies.

### Updating doors between renders

`ReplacePortals([]Portal)` swaps the whole door table between renders. The
new table is fully validated first; if anything is invalid the call returns
an error and the previous table, its inverse matrices and rendered results
remain usable. On success, moving a frame, changing a target or toggling
`Enabled` takes effect on the next `Renderer.Render` call, while region and
BVH identities stay the same. Editing the portal table while a render is in
progress is not supported.

A `PortalScene` implements the ordinary `Scene` interface, so it renders
unchanged through:

```go
renderer.Render(raytrace.NewMonteCarloTracer(), pixelBuffer, portalScene, config)
```

The `examples/portals` command renders `before.png` and `after.png` from two
doors sharing one region, then moves one door and closes the other with
`ReplacePortals` between the two frames.
