# cpp-rasterizer

cpp-rasterizer is a software rasterizer written in C++. This rasterizer loads obj files and renders them interactively with Phong shading. Move with WASD, shift, space, and mouse.

## Build

You will need to install SDL2 to build. If you are on Ubuntu, then run `apt-get install libsdl2-dev`

1. Run `make`
2. Run `./rasterizer models/utah-teapot.obj`

## Screenshots

![Stanford Dragon](/img/dragon.png)
![Utah Teapot](/img/teapot.png)
![Amogus](/img/sus.png)

## Opaque CPU surfels (surface reconstruction)

`src/surfels.{h,cpp}` adds an offline, CPU-only path that draws *surfel* points
directly, without expanding every point into a triangle fan. Each surfel is

* a centre `p`,
* two linearly independent tangent half-axes `a` and `b`, and
* a linear-RGB colour,

and describes the curved patch `S(u, v) = p + a*u + b*v`, `u^2 + v^2 <= 1`.
`src/pipeline.h` holds the shared interface: the single `FragmentShader()` used
by both the triangle rasterizer and the surfel path, a common opaque `Target`
(linear RGB + view depth), and the `Mat4d` matrix view.

### How a surfel is rendered

For every pixel the renderer unprojects the pixel centre `(x + 0.5, y + 0.5)`
into a view-space ray, intersects that ray **exactly** with the surfel's plane,
and solves for the tangential coordinates `(u, v)`. The hit contributes only if
`q = u*u + v*v < 1` and its view depth is inside `[near, far]`. The kernel
weight is `(1 - q)^2`. There is no centre-Jacobian Gaussian approximation and no
per-point sorting: the ray/disk test is done per pixel, so a tilted disk has a
different true depth at each pixel and two disks can swap depth across the
screen.

Candidates are bucketed into 16x16 tiles (configurable via
`SurfelOptions::tile_size`) from a conservative screen-support rectangle, and
only the local candidate list is intersected per pixel. For disks fully inside
the depth range the published support rectangle is the **exact** extremum of the
projected disk (computed by solving the tangent-line condition on the unit
circle); the error is below `1e-6 px`. Disks crossing the near/far planes use a
conservative clipped-tangent-square bound.

### Reconstruction, not transparency

The opaque result is assembled per pixel as follows:

1. Find the nearest valid hit depth `d0`.
2. Weight every hit with `d <= d0 + delta` by `(1 - q)^2` and normalise the
   weights.
3. Reconstruct the pixel colour from `sum(w * surfaceRGB * shading)`; the
   shading is the shared `FragmentShader`, with the disk's unit normal flipped
   towards the eye, the view vector taken from the actual intersection point,
   and the caller's view-space light direction.
4. Write `d0` as the pixel depth (never an average), and leave the base image
   untouched where there is no hit.

This is **surface reconstruction**, not alpha compositing. Colours are
normalised, so a front layer cannot leak the colour of a layer behind it; a
transparent-particle renderer would instead blend front-to-back and let the back
layer tint the result. `delta = 0` keeps exactly the deepest-nearest layer, and
ties are broken by colour/geometry values (not input order), so the result is
symmetric under reordering.

### Base image

The surfel pass composites over an independent base image: linear RGB plus the
view depth `d = -z` (background `+infinity`). `RasterizeTriangle()` (declared in
`pipeline.h`, implemented in `rasterizer.cpp`) draws real triangles into a
`Target`; `BaseFromTarget()` converts it to a `BaseImage`. Surfels only take hits
*strictly* in front of that base depth, so an opaque triangle backdrop occludes
surfels behind it. The base image, the surfels and the camera are never mutated;
render calls are repeatable, order-independent and safe to interleave between
instances.

### Example

```
make surfel-example
./surfel-example out.ppm
```

`examples/surfels.cpp` draws an opaque triangle backdrop and several obliquely
intersecting coloured disks with non-uniform half-axes, then writes a binary
PPM (row order flipped; the `y`-up array is stored top-down). No SDL, GPU or
third-party runtime is needed for this path.

### Tests

`tests/surfels.cpp` registers `footprint`, `reconstruction` and `integration`
groups with the harness in `task_support/check.h`. It checks exact support
extrema against dense sampling and an independent conic computation, near-plane
clipping, non-uniform/oblique disks against an independent analytic ray/disk
reference, per-pixel depth swapping, the inclusive delta band, equal-depth colour
symmetry, back-layer colour isolation, kernel normalisation, tile-boundary
coverage, real triangle-base occlusion, that reported statistics reflect real
per-tile work (not a full-screen scan), and that invalid inputs are rejected
without disturbing the previous result.

```
bash task_support/run.sh target
```
