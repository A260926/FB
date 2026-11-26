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


## Windowless thick-glass preview

`src/refraction.h` / `src/refraction.cpp` add a reusable, windowless
production path for a thick-glass preview. It captures real triangles
off-screen (the same barycentric traversal and Phong fragment generation as
the windowed rasterizer) and composes one refracted ray through a closed
convex glass volume:

```
camera --(air -> glass)--> entrance --(internal segment)--> exit
       --(glass -> air)--> opaque background
```

It never fakes refraction by offsetting background texture coordinates with
the screen-space normal: the camera ray is refracted at the entrance, the
real interior ray is traced to the exit, and the exit normal refracts the ray
back out into the background.

### Entry points

```cpp
#include "src/refraction.h"
using namespace gfx::refraction;

// 1. Describe the frame.
CaptureSettings settings;
settings.width = 640;                       // any size, non-square is fine
settings.height = 400;
settings.world_to_view = world_to_view;     // world -> view space
settings.normal_to_view = normal_to_view;   // world -> view for normals
settings.projection = projection;           // NDC z in [0, 1], w = -view.z
settings.near_depth = 0.5;                  // optional; derived if omitted
settings.far_depth = 100.0;

// 2. Capture real triangles: opaque background and closed convex glass.
SceneCapture scene;
std::string error;
CaptureScene(background_mesh, glass_mesh, settings, scene, &error);
// Or, individually: CaptureBackground(...), CaptureGlass(...).

// 3. Compose once (captures stay read-only; the result owns fresh storage).
Material material;
material.ior = 1.47;                        // in [1.01, 2.5]
material.sigma_a = {0.18, 0.09, 0.04};      // per-channel, >= 0
material.environment = {0.05, 0.07, 0.10};  // reflections / misses
CompositeResult result;
Compose(scene, material, result, &error);

// 4. Quantise and write only here (rows flipped for PPM).
WritePPM("glass.ppm", result, &error);
```

`CaptureBackground` stores the opaque background's linear RGB and positive
view depth. `CaptureGlass` stores the nearest front-facing and nearest
back-facing view-space position, unit outward normal and validity per pixel;
back faces bypass the old back-face culling. Positions and normals use 1/w
perspective-correct interpolation and normals are normalised only after
interpolation.

`TraceDepthField` walks a captured depth field cell by cell (at most
`width + height + 1` cells per segment, each visited once), clipping the ray
to the capture's finite near/far range and to the image. Hits use the
continuous ray parameter — the ray's linearly varying depth is matched
against the cell's recorded depth plane — rather than fixed-step sampling.
`RayHit` reports the hit pixel, continuous coordinates, the view-space
distance and the number of visited cells.

### Composition and approximation

For each pixel whose glass entrance is visible and not hidden behind the
opaque background:

```
colour = R_entrance * environment
       + T_entrance * T_exit * exp(-sigma_a * internal_length) * background
```

`R`/`T` are exact unpolarised Fresnel terms from Snell's law. Only one
transmission path is evaluated — internal reflections are not followed. A
missing exit, exit total internal reflection or an entrance hidden behind the
background drops the transmission term (the hidden-entrance case keeps the
background pixel). Missing background hits use the environment colour. The
internal length is the true distance from the entrance to the exit along the
refracted ray.

### Conventions

* Matrices are row-major with column vectors: `clip = P * V * p`.
* View space looks down `-Z`; "view depth" means `d = -view.z > 0`.
* The projection maps the finite view depth range to NDC z in `[0, 1]` and
  satisfies `clip.w = -view.z`. `PerspectiveProjection(left, right, bottom,
  top, near, far)` builds one, supporting asymmetric and non-square frusta.
* NDC maps to pixels as `sx = (ndc.x * 0.5 + 0.5) * width`,
  `sy = (ndc.y * 0.5 + 0.5) * height`; pixel `(x, y)` is the half-open cell
  with centre `(x + 0.5, y + 0.5)`, stored at `y * width + x`.
* Input vertices must be finite, inside the frustum and have positive
  `clip.w`; the new path performs no extra clipping. Only filled triangles
  are handled.

The example `examples/refraction.cpp` builds a procedural convex glass wedge
and a checkered opaque backdrop, then writes a PPM through the production
interface. Build and run it with:

```sh
c++ -std=c++20 -O1 -I. examples/refraction.cpp src/refraction.cpp -o refraction_example
./refraction_example glass.ppm
```

The windowed program and its default lighting, depth comparison, pixel
ordering and model loading are unchanged; `task_support`'s headless macro only
disables SDL presentation.
