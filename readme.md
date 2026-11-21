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
- `yocto/yocto_lens.{h,cpp}`: owning compound spherical lens configuration,
  single-ray lens tracing and mechanical focus
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

## Compound lens camera

`yocto/yocto_lens.{h,cpp}` adds a physically modeled compound spherical lens to
the CPU camera, as an alternative to the legacy thin-lens blur. When a camera
has no lens group configured, the original camera rays, random-number
consumption and images are unchanged.

### Coordinates and units

All lens quantities are in **meters** and live in lens-local coordinates:

- the optical axis is **+z** and points toward the scene;
- the film plane is at `z = -g`, with `g` the film distance;
- the aperture stop is a disk of radius `a` centered on the optical axis at
  `z = 0`;
- interfaces are listed with **strictly increasing** vertex `z`.

Each `lens_interface` has a curvature radius `R`, an effective (clear) radius
`r`, and the refractive index `eta` **after** crossing the interface. The
medium before the first interface and after the last one is air (`eta = 1`).

For a nonzero `R` the sphere center sits at `z + R` and only the half sphere
containing the vertex is used, so the effective radius must satisfy
`0 < r < |R|`. `R = 0` is a flat diaphragm that does not refract, so its `eta`
must equal the previous medium's index.

### Tracing and camera sampling

A ray through the lens takes the nearest valid intersection at every step.
Missing the aperture, missing an interface's clear radius, missing an
intersection, or total internal reflection makes the sample produce **no
contribution**; it is never resampled. Spherical surfaces refract by Snell's
law and multiply the unpolarized Fresnel transmittance. The exit ray leaves the
last interface and maps lens-local **+z** to `camera.frame`'s **-z**.

Film sampling keeps the legacy `film`/`aspect` physical size: the old pixel
jitter is drawn first and the aperture disk is then sampled uniformly by area
at `z = 0`. Each sample's exposure weight is

```
pi * a^2 * cos(theta)^4 / g^2 * product_of_transmittances
```

where `theta` is the angle between the initial film-to-aperture direction and
the optical axis. This is a physical irradiance weight and is not normalized by
aperture area. Blocked samples still occupy the full camera sample count, so
they stay in the denominator instead of being averaged away.

`params.clamp` limits the per-sample RGB peak **after** multiplying by the
exposure weight. The weight and validity diagnostics are not clamped. To
compare irradiance levels, set `clamp` high enough that it never triggers; the
display truncation never feeds back into the diagnostic channels.

The offscreen diagnostics are `get_weight_image` (mean exposure weight per
camera sample, geometric factor included, blocked samples zero) and
`get_valid_image` (fraction of successful samples), both linear float.

### Mechanical focus

Focusing uses an on-axis paraxial ray: from the film center through the
aperture point `(h, 0, 0)` with `h = a * 1e-3`. The ray is traced through the
lens and its `x` intercept on the scene plane `z = focus_z` is driven to zero
by solving for the film distance `g`. This uses the real lens trace, never a
thin-lens focal length formula.

```cpp
auto camera = scene.cameras[0];
auto lens   = lens_config{};  // fill aperture, interfaces
set_camera_lens(camera, lens);
auto ok = focus_camera(camera, focus_z, 1e-3f, 1.0f);
```

`focus_camera` (and `solve_lens_focus` in `yocto_lens.h`) takes a finite,
positive, non-empty interval of film distances and a `focus_z` larger than the
last interface `z`. Endpoints that are already roots are accepted. The solve
fails explicitly when the interval does not bracket a root, when any required
trace is blocked, when it cannot converge, or when the configuration is
invalid. On failure the previous camera configuration, existing `film` and the
caller's RNG are left untouched.

### Validation

Non-finite numbers, out-of-order interfaces, non-positive refractive indices,
a lens that does not exit back into air, and invalid sizes are rejected before
rendering (`validate_camera_lens`, also enforced by `make_trace_state`). The
compound lens is limited to finite non-degenerate lenses, positive film
distances and perspective cameras.

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
