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

* ObjectBegin calls cannot be nested.

* This version extends the older single-level-instance behaviour:
  ObjectInstance may be used both at world scope and inside an
  ObjectBegin/ObjectEnd block. An instance written inside an object belongs to
  that object (`Instance::ownerObject`); `kInvalidIndex` means a root/world
  instance. Object names are global and may be forward referenced. All names
  are resolved after the complete file, including includes, has been read.

* Undefined object names, duplicate ObjectBegin names, and direct or indirect
  object reference cycles make `Loader::load` fail. The resulting `Error`
  points at the offending ObjectBegin or ObjectInstance directive.

* `Object::firstShape` and `Object::numShapes` describe only the object's
  direct shapes. They never include child instances. Child order is the order
  of `Scene::instances` entries with the matching `ownerObject`.

* Include is textual inclusion at the current parser state. An included
  ObjectBegin/ObjectEnd therefore has the same ownership and transform/attribute
  state as if its text appeared in the parent file. AttributeBegin/End and
  ObjectBegin/End restore their pushed transform and attribute state.


Nested scene evaluation and motion
----------------------------------

`minipbrt_scene.h/.cpp` adds `sample_scene` and `bound_scene_motion`.

Matrices in `Transform`, `SceneDraw::world` and `SceneMotionBounds` are row
major and act on column vectors. For an instance path `[i0, i1, ...]` from root
to leaf, the sampled world matrix is

```
world = instance[i0](t) * instance[i1](t) * ... * shapeToWorld(t)
```

`Object::objectToInstance` is not multiplied again: its transform was already
pushed onto the stack when the object's direct shape transforms were saved.

Each factor interpolates its own two endpoints element by element using
`Scene::startTime` and `Scene::endTime`; the interpolated factors are then
multiplied. This is not equivalent to multiplying the endpoints and
interpolating those products. Time is clamped to the scene interval, and the
start endpoint is used when start and end times are equal.

`SceneDraw::normal` is the inverse transpose of the linear 3x3 part of
`world`. `flipOrientation` XORs the shape and path-instance reverse orientation
flags with the sign of the world determinant.

Motion bounds multiply the linear endpoint polynomials first. A transformed
vertex coordinate is therefore a polynomial (generally nonlinear) in shutter
time. `bound_scene_motion` isolates its extrema/conservatively subdivides it
rather than using endpoints alone.


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

Most names are still resolved immediately. Object names are the exception in
this extended build: ObjectInstance references are late-bound after the full
scene is parsed, permitting forward references and diamond sharing.

Behaviour with undefined names:
* If a NamedMaterial directive refers to a material name that hasn't been
  defined yet, PBRT reports an error and leaves the current material unchanged.
* Where a MixMaterial refers to a material name that hasn't been defined yet, 
  it uses a default-initialised MatteMaterial instead.
* If a material parameter uses a texture name that hasn't been defined yet, it
  simply creates an unnamed ConstantTexture with the default value for that
  parameter instead.
* If an ObjectInstance directive refers to an object name that is not defined
  by the end of parsing, minipbrt records a located load error. Duplicate
  object names and reference cycles are also load errors.
* If a CoordSysTransform directive uses a name that hasn't been defined yet, a
  warning (not an error) is reported and the current transform is left 
  unchanged.
* If a MediumInterface refers to an undefined medium name, an error is logged
  but parsing continues and the medium is left undefined.

Redefining names:
* Redefining a texture or material name causes a warning in PBRT, not an error.
* Redefining a medium or coordinate system name is silently accepted.
* Redefining an Object name is a located load error in this extended build.


Other
-----

* There is one case where a `scale` parameter has type `spectrum`, where the
  docs say it should be `float`, but I can't remember where that was just now.

