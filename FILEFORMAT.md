The PBRT v3 File Format
=======================

The canonical reference for the file format is its implementation in
[PBRT v3](https://github.com/mmp/pbrt-v3).

There is a page documenting the file format here:

https://www.pbrt.org/fileformat-v3.html

However there are cases where that document doesn't match the PBRT
implementation. Some details are incorrect, some are missing. Details of those
are below.


General parsing
---------------

* Brackets are allowed around arg lists, e.g. for Transform the 16 floats can
  be enclosed in '[' and ']' characters.

* PBRT itself doesn't seem to allow forward references to named items, but
  some of the scenes in the pbrt-v3-scenes collection do this, specifically 
  for materials.

* Errors get logged but usually don't prevent the file from loading.


Curve shapes
------------

* The `P` array does not hold 4 points, as the doc suggests. It holds a number 
  determined by the number of curve segments, with different rules for bezier 
  and bspline curves.

* The `N` array holds `degree + 1` normals, not 2 as per the doc.


Textures
--------

There is an additional texture type, "ptex", which isn't mentioned in the
docs. It has two parameters:
* `"string filename"` - the name of a PTex file on the local disk
* `"float gamma"` - a gamma value to apply to colours from the PTex file.

Float textures exist in a different namespace to spectrum textures. You can 
have a float texture and a spectrum texture both called "foo". PBRT hardcodes
which namespace to do the lookup in for any given parameter.


Integrators
-----------

* Parameters for the Whitted integrator are not documented.
* PBRT supports two additional integrator types which aren't mentioned in the doc:
  * `"volpath"`
  * `"ao"`


Coordinate Systems
------------------

* The Camera statement defines a coordinate system called "camera" as a side
  effect.


Objects
-------

* ObjectBegin does an implicit AttributeBegin. Likewise ObjectEnd does an 
  implicit AttributeEnd.

* ObjectBegin calls cannot be nested. Every ObjectBegin creates exactly one
  `Object`, in file order; the `Object` index is its position in
  `Scene::objects`.

* Instancing can be nested: an `ObjectInstance` may appear inside an
  `ObjectBegin/End` block as well as directly in the world. An instance that
  appears inside a block belongs to the object being defined; an instance that
  appears in the world is a root instance. This is recorded in
  `Instance::ownerObject` (`kInvalidIndex` means "directly in the world").

* Shapes and instances are independent lists, and their order and indexes are
  exactly the order they appear in the file. `Object::firstShape` and
  `Object::numShapes` describe only the *direct* shapes defined in that
  object's block. Child instances are not folded into that range: they live in
  `Scene::instances` with the owning object recorded in `ownerObject`.

* Geometry is shared, never copied. An object that is instanced from several
  places contributes the same shape entries to every instance path.

* A `shapeToWorld` saved for a shape that belongs to an object is really a
  shape-to-object transform: it already includes the object's
  `objectToInstance` matrix. Consequently a renderer must not multiply
  `objectToInstance` again; it composes the `instanceToWorld` matrices along
  the instance path and then multiplies by `shapeToWorld`. The production
  helpers in `minipbrt_scene.h` implement this ordering (see "Transform
  composition" below).


Nested objects and name resolution
----------------------------------

* Object names are resolved after the whole file (including all includes) has
  been read. This allows forward references, e.g. an `ObjectInstance` for an
  object that is only defined later in the file. (PBRT v3 resolves at the point
  of use; minipbrt deliberately defers so the same file can be used either
  way.)

* An `ObjectInstance` that names an object which is never defined is an error
  and makes loading fail. The reported error points at the `ObjectInstance`
  directive, including inside an included file.

* Defining two objects with the same name is an error and makes loading fail.

* Direct and indirect reference cycles are errors and make loading fail. A
  direct cycle is an object whose block contains `ObjectInstance` for itself;
  an indirect cycle is any longer loop through object references.

* Include files, AttributeBegin/End scoping and transform save/restore inside
  `ObjectBegin/End` behave exactly as they do elsewhere: the object records the
  current transform at the point each shape or instance appears, and any
  attribute or transform changes inside the block are discarded at
  `ObjectEnd`.


Transform composition
---------------------

Let the instance path from the world root to a shape be `I0, I1, ... In-1`
(the indexes in `SceneDraw::path`) and let `S` be the shape. The world matrix
for the shape at time `t` is

    world(t) = I0(t) * I1(t) * ... * In-1(t) * S(t)

with each factor interpolated element-wise between its `start` and `end`
matrix, using the normalised scene time

    u = (clamp(t, startTime, endTime) - startTime) / (endTime - startTime)

(`u = 0` when `startTime == endTime`). The interpolation is applied to each
matrix *before* multiplying them together; multiplying the two endpoint
matrices and interpolating the product is not equivalent and is not used. When
`startTime == endTime`, the `start` matrix is used.

`objectToInstance` is not a separate factor: it is already baked into the
object's shapes' `shapeToWorld`. Instances whose `ownerObject` is
`kInvalidIndex` are the roots; instances with an owner are reached only through
that owner's block.

Shapes are visited in a deterministic order: first world-direct shapes (empty
`path`), then the root instances in `Scene::instances` order. Within one object,
direct shapes are visited before child instances, and child instances are
visited in `Scene::instances` order. `SceneDraw::shape` is an index into
`Scene::shapes`.



Named items
-----------

These are all the different things that can be referenced by name:
* Material
* Medium
* Object
* Texture
* Coordinate system

Material and texture names are scoped to the current AttributeBegin/End block
(ObjectBegin/End counts for this purpose as well). Medium and Object names are
global, as are Coordinate Systems.

There are no forward references/late binding of material, medium, texture or
coordinate-system names: whenever we encounter a reference it is resolved to
the item it refers to at that time, and redefining the name later has no effect
on earlier references. Object names are the one exception: they are resolved
after the whole file has been read, so an `ObjectInstance` may refer to an
object defined later (including in another file pulled in by `Include`).

Behaviour with undefined names:
* If a NamedMaterial directive refers to a material name that hasn't been
  defined yet, PBRT reports an error and leaves the current material unchanged.
* Where a MixMaterial refers to a material name that hasn't been defined yet, 
  it uses a default-initialised MatteMaterial instead.
* If a material parameter uses a texture name that hasn't been defined yet, it
  simply creates an unnamed ConstantTexture with the default value for that
  parameter instead.
* If an ObjectInstance directive refers to an object name that hasn't been
  defined anywhere in the scene, minipbrt reports an error and fails the load.
  (PBRT v3 reports an error and skips the instance; minipbrt treats it as a
  load failure so the reference graph is unambiguous.)
* If a CoordSysTransform directive uses a name that hasn't been defined yet, a
  warning (not an error) is reported and the current transform is left 
  unchanged.
* If a MediumInterface refers to an undefined medium name, an error is logged
  but parsing continues and the medium is left undefined.

Redefining names:
* Redefining a texture or material name causes a warning in PBRT, not an error.
* Redefining a medium, object or coordinate system name is silently accepted by
  PBRT. minipbrt instead rejects a duplicate object name as a load error,
  because object references are resolved late and a duplicate would be
  ambiguous.


Other
-----

* There is one case where a `scale` parameter has type `spectrum`, where the
  docs say it should be `float`, but I can't remember where that was just now.

