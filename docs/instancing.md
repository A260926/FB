# Shared immutable meshes

Define `JET_MESH_INSTANCING=1` in the application's `JetConfig.hpp` to enable
shared geometry. The default is zero: existing applications retain their Object
layout and renderer path. Use the same configuration throughout the application
and rebuild all translation units when changing it.

Instancing primarily saves memory. Each visible placement still needs its own
transforms, clipping, lighting and rasterization. There is additional transform
and material-selection overhead, which cache reuse can sometimes offset. Measure
the complete scene on its target; a smaller PSRAM footprint does not guarantee
less traffic or faster rendering.

## Author once, place repeatedly

```cpp
// Include Object.hpp and Primitives.hpp; keep paint and both Objects alive.
using namespace Renderer;
Material paint(0xf800);
std::unique_ptr<Object> authored(Primitives::createCube(100, 100, 100, &paint));
auto mesh = Object::freezeMesh(std::move(*authored));
authored.reset();

Object first, second;
first.addInstance(mesh, {});
second.addInstance(mesh, Object::InstanceTransform::rotated({0, 45, 0}));
first.calculateBoundingBox();
second.calculateBoundingBox();
first.setPosition(-100, 0, 500);
second.setPosition(100, 0, 500);
scene.addObject(&first);
scene.addObject(&second);
```

`freezeMesh(Object&&)` prepares bounds and packed positions and moves the geometry
into a `shared_ptr<const Object>`. It rejects nested instances by returning an
empty pointer without consuming the input. `addInstance()` returns false for an
invalid prototype, a per-triangle material table with the wrong length, or a
non-finite or singular `InstanceTransform` basis.

An owner can contain its own unique geometry and multiple instances. Rendering
visits the unique mesh first, then instances in insertion order. Placements
inherit the owner's transform, culling, blend, depth, fade and LOD behaviour.
Frustum culling and distance/LOD selection operate on the owner's complete bounds.
Use separate owners when independent visibility or movement matters.

`InstanceTransform` maps prototype-local positions into owner-local space. Its
row-major basis acts on column vectors, so positions become `basis * p + position`
(the translation keeps the same integer units as vertex positions). Any finite,
invertible affine basis is supported: non-uniform scale, shear and mirroring are
all valid. A matrix is rejected when any entry is non-finite or when it is
singular (for example a collapsed axis or a rank-deficient shear).

Normals are transformed by the inverse transpose of the basis and renormalised to
`FIXED_POINT_SCALE`, so a scaled or sheared placement keeps its lighting
perpendicular to the transformed surface. A negative determinant mirrors the
mesh; the renderer compensates for the reversed winding so a material's front and
back do not swap — `CULL_BACKFACES`, `CULL_FRONTFACES` and `NO_CULLING` all keep
the prototype's authored front face front. `rotated(degrees, position)`
constructs a rigid rotation for callers that only need the legacy orthogonal path;
its output is unchanged.

## Per-frame placement updates

Call `calculateBoundingBox()` once after adding instances. After that, change a
placement between frames with `setInstanceTransform(index, transform)`:

```cpp
// After the previous frame's draw has completed, before the next prepareFrame():
batch.setInstanceTransform(0, affinePlacementFor(nextFrame));
scene.prepareFrame();   // uses the new placement, refreshed bounds
scene.rasterizeBand(0, height);
```

`setInstanceTransform()` updates the stored placement and refreshes the batch
bounding box in place; the caller does not call `calculateBoundingBox()` and the
frozen prototype is never rebuilt. It returns false for an out-of-range index or
a non-finite/singular basis, leaving the existing placement, bounds and material
bindings unchanged. Ordinary owner `position`/`rotation` animation requires no
mesh rebuild. Direct edits to the public `instances` array keep the original
manual-refresh convention: call `calculateBoundingBox()` yourself afterwards.

Frozen vertex positions, normals, UVs and topology must remain immutable.


## Materials, lifetime and statistics

The optional `Material*` argument to `addInstance()` replaces every material on
that placement. `Object::TriangleMaterials` holds an immutable vector of
`Material*`, indexed by original prototype triangle index. A null table entry
retains the source material and baked colour. A uniform override takes precedence;
any replacement also overrides a triangle's baked colour.

Prototype and table storage are reference counted. Material and texture pointers
remain borrowed, as for ordinary triangles. Keep them alive until their last
render/scanout consumer has finished. Copying an owner shares its prototypes and
tables, while copying the placement records so their transforms can change
independently. Removing the last reference releases the prototype.

`Object::vertexCount()` and `triangleCount()` include all placements. Direct
`vertices.size()` and `triangles.size()` report only an owner's unique mesh.
`Scene::getStatistics()` includes instances when the capability is enabled.

With `MAX_PICK_QUERIES>0`, `PickResult::object` identifies the submitted owner,
`mesh` identifies the actual prototype or selected LOD, and `instanceIndex`
identifies its placement (`-1` for ordinary geometry). `triangleIndex` refers to
the original triangle in `mesh`, even with `SORT_TRIANGLES=1`; sorting uses a
temporary index array without mutating the shared topology. Results borrow their
pointers and remain usable only while the associated objects/meshes live.
