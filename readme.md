# Yocto/GL: Tiny C++ Libraries for Data-Oriented Physically-based Graphics

Yocto/GL is a collection of small C++17 libraries for building
physically-based graphics algorithms released under the MIT license.
Yocto/GL is written in a deliberately data-oriented style for ease of
development and use.
Yocto/GL is split into small libraries to make code navigation easier.
See each header file for documentation.

- `yocto/yocto_math.{h}`: fixed-size vectors, matrices, rigid frames,
  transforms
- `yocto/yocto_color.{h}`: color conversion, color adjustment,
  tone mapping functions, color grading, color maps, color spaces
- `yocto/yocto_geometry.{h}`: rays, bounding boxes,
  geometry functions, ray-primitive intersection, point-primitive overlap
- `yocto/yocto_noise.{h}`: Perlin noise
- `yocto/yocto_sampling.{h}`: random number generation, generation of points
  and directions, Monte Carlo utilities
- `yocto/yocto_shading.{h}`: evaluation and sampling of fresnel
  functions, bsdf lobes, transmittance lobes, phase functions
- `yocto/yocto_image.{h,cpp}`: simple image data type, image resizing,
  tonemapping, color correction, procedural images, procedural sun-sky
- `yocto/yocto_shape.{h,cpp}`: simple shape data structure, utilities 
  for manipulating triangle meshes, quads meshes and line sets, computation of 
  normals and tangents, linear and Catmull-Clark subdivision, 
  procedural shapes generation, ray intersection and closest point queries
- `yocto/yocto_scene.{h,cpp}`: scene representation and properties
  evaluation
- `yocto/yocto_bvh.{h,cpp}`: ray intersection and closest point queries
  of triangle meshes, quads meshes, line sets and instances scenes using a
  two-level bounding volume hierarchy
- `yocto/yocto_trace.{h,cpp}`: path tracing of surfaces and hairs supporting
  area and environment illumination, microfacet GGX and subsurface scattering,
  multiple importance sampling
- `yocto/yocto_sceneio.{h,cpp}`: image, shape and scene serialization
- `yocto/yocto_modelio.{h,cpp}`: low-level parsing and writing for 
  Ply, Obj, Stl formats
- `yocto/yocto_pbrtio.{h,cpp}`: low-level parsing and writing for 
  Pbrt format
- `yocto/yocto_cli.{h}`: printing utilities and command line parsing
- `yocto/yocto_parallel.h`: concurrency utilities (deprecated)

You can see Yocto/GL in action in the following applications written to
test the library:

- `apps/ytonemap.cpp`: image conversion and viewing
- `apps/ycolorgrade.cpp`: image color grading
- `apps/yconvert.cpp`: scene conversion
- `apps/yconverts.cpp`: shape conversion
- `apps/ytrace.cpp`: offline and interactive scene rendering
- `apps/ycutrace.cpp`: offline and interactive scene rendering with CUDA
- `apps/yview.cpp`: interactive scene viewing

Here are some test images rendered with the path tracer. More images are
included in the [project site](https://xelatihy.github.io/yocto-gl/).

![Example materials: matte, plastic, metal, glass, subsurface, normal mapping](images/features1.jpg)

![Example shapes: procedural shapes, Catmull-Clark subdivision, hairs, displacement mapping](images/features2.jpg)

![Image rendered with Yocto/GL path tracer. Model by Disney Animation Studios.](images/island.jpg)

## Scene nodes and frame-to-frame updates

Scenes can optionally organize instances under an editable node hierarchy.
`scene_data::nodes` holds `scene_node` elements with a `parent` index (-1 for
a root), an `instance` index (-1 for a pure grouping node), a rigid `local`
frame (rotation and translation) and a `visible` flag. Node indices do not
need to be sorted by hierarchy and multiple roots are allowed, but each
instance is bound to at most one node. `update_scene_nodes(scene)` computes
world frames as parent-world times local and publishes them, together with
the logical and of the ancestors' visibility, to the bound instances;
unbound instances keep their own `frame` and `visible` fields.
`reparent_scene_node(scene, node, parent, keep_world)` rebinds a node to a
new parent, either preserving the subtree's world pose or its local frame.
Invalid hierarchies (duplicate bindings, out-of-range indices, cycles,
self-parenting, non-finite or non-rigid local frames) throw
`std::invalid_argument` without publishing partial results.

Hidden instances are skipped by `intersect_scene_bvh` and
`intersect_instance_bvh` (both closest and `find_any` queries), do not
occlude visible geometry and do not contribute area lights. Between frames,
`update_trace_scene(scene, bvh, lights, state, params, updated_shapes)`
validates the hierarchy and the update list, publishes node state, refits
the existing shape and top-level bvhs reusing their topology, rebuilds the
light set and resets the progressive trace state, so the CPU path tracer
uses the new hierarchy state on the next frame:

```cpp
auto bvh = make_trace_bvh(scene, params);      // once
auto lights = make_trace_lights(scene, params);
auto state = make_trace_state(scene, params);
// each frame: edit nodes/shapes, then
update_trace_scene(scene, bvh, lights, state, params, updated_shapes);
while (state.samples < params.samples)
  trace_samples(state, scene, bvh, lights, params);
```

See `examples/scene_nodes.cpp` for a complete example that renders a scene
of shared-mesh cards to `before.ppm` and `after.ppm` across a group move, a
visibility toggle and a shared-geometry edit. Build and run it, together
with the checks in `checks/scene_nodes.cpp`, with `bash task_support/run.sh
target`.

## Design Considerations

Yocto/GL follows a "data-oriented programming model" that makes data explicit.
Data is stored in simple structs and accessed with free functions or directly.
All data is public, so we make no attempt at encapsulation.
We do this since this makes Yocto/GL easier to extend and quicker to learn,
with a more explicit data flow that is easier when writing parallel code.
Since Yocto/GL is mainly used for research and teaching,
explicit data is both more hackable and easier to understand.

Nearly all objects in Yocto/GL have value semantic. This means that everything
can be trivially copied and serialized and there is no need for memory management. 
While this has the drawback of potentially introducing spurious copies, it does
have the benefit of ensuring that no memory corruption can occur, which
turned out was a major issue for novice C++ users, even in a very small
library like this one.

In terms of code style we prefer a functional approach rather than an
object oriented one, favoring free functions to class methods. All functions
and data are defined in the `yocto` namespace so libraries can call each others
trivially.

The use of templates in Yocto was the reason for many refactoring, going
from no template to heavy template use. At this point, Yocto uses some templates
for readability. In the future, we will increase the use of templates in math
code, while keeping many APIs explicitly typed.

For error handling in IO we either return status object or an interface that
uses boolean flags and error strings. Internally exceptions are used when used
by external libraries, but otherwise no exception are used. At the moment,
exceptions are only used to report "programmer errors", namely when
preconditions or post conditions are violated in functions, just lime the
standard library does.

## License

The library is released under the MIT license. We include various external
dependencies in the distribution that each have thir own license, compatible
with the chosen one.

## Compilation

This library requires a C++17 compiler and is know to compiled on
OsX (Xcode >= 11), Windows (MSVC >= 2019) and Linux (gcc >= 9, clang >= 9).

You can build the example applications using CMake with
`mkdir build; cd build; cmake ..; cmake --build .`

Yocto/GL required dependencies are included in the distribution and do not
need to be installed separately.

Yocto/GL optionally supports building OpenGL demos. OpenGL support is enabled
by defining the cmake option `YOCTO_OPENGL`. 
OpenGL dependencies are included in this repo.

Yocto/GL optionally supports the use of Intel's Embree for ray casting.
See the main CMake file for how to link to it. Embree support is enabled by
defining the cmake option `YOCTO_EMBREE`. Embree needs to be installed separately.

Yocto/GL optionally supports the use of Intel's Open Image Denoise for denoising.
See the main CMake file for how to link to it. Open Image Denoise support
is enabled by defining the cmake option `YOCTO_DENOISE`.
OIDN needs to be installed separately.
