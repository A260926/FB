# Understandable RayTracing in 256 lines of bare C++

This repository is a support code for my computer graphics lectures. It is not meant to be the ultimate rendering code or even physically realistic. It is meant to be **simple**. This project is distributed under the [DO WHAT THE FUCK YOU WANT TO PUBLIC LICENSE](https://en.wikipedia.org/wiki/WTFPL).

**Check [the wiki](https://github.com/ssloy/tinyraytracer/wiki) that accompanies the source code. The second raytracing chapter is available [in the tinykaboom repository](https://github.com/ssloy/tinykaboom/wiki). If you are looking for a software rasterizer, check the [other part of the lectures](https://github.com/ssloy/tinyrenderer/wiki).**

In my lectures I tend to avoid third party libraries as long as it is reasonable, because it forces to understand what is happening under the hood. So, the raytracing 256 lines of plain C++ give us this result:
![](https://raw.githubusercontent.com/ssloy/tinyraytracer/master/out.jpg)

## compilation
```sh
git clone https://github.com/ssloy/tinyraytracer.git
cd tinyraytracer
mkdir build
cd build
cmake ..
make
```

You can open the project in Gitpod, a free online dev evironment for GitHub:

[![Open in Gitpod](https://gitpod.io/button/open-in-gitpod.svg)](https://gitpod.io/#https://github.com/ssloy/tinyraytracer)

On open, the editor will compile & run the program as well as open the resulting image in the editor's preview.
Just change the code in the editor and rerun the script (use the terminal's history) to see updated images.

## Fiber preview path

A second, self-contained rendering path previews thin dielectric fibers
alongside the untouched sphere tracer. It uses the C++17 standard library
only.

### Layout

- `core.h` — shared core: the original float `vec3`, `Material`, `Sphere`
  and the original sphere/ray intersection, plus a double-precision
  `dvec3` used by all fiber math. The legacy `tinyraytracer.cpp`, its
  `Material`, `cast_ray` and default image are unchanged in behavior.
- `fiber.h` / `fiber.cpp` — fiber geometry and the scattering model.
- `fiber_renderer.h` / `fiber_renderer.cpp` — transport over fibers, the
  original spheres (black occluders) and a caller-supplied directional
  background.
- `tests/fiber_cases.cpp` — `geometry`, `scattering` and `transport`
  cases registered through `task_support/check.h` (defines no `main`).
- `examples/fiber_preview.cpp` — deterministic PPM preview.

### Geometry

A fiber is the lateral surface of a finite open cylinder given by
endpoints `A`, `B` and radius `r`: side hits only, no end caps and no
triangle mesh. A unit ray is intersected over the half-open interval
`[tmin, tmax)` with the axial parameter restricted to `[0,1]`. Axis-parallel
rays never hit; an exact tangency counts as a hit. Each hit reports a
stable fiber id, `t`, the position, the outward normal and the
cross-section coordinate `h = dot(N, Y)` of the material frame
`T = normalize(B-A)`, `wo = -ray_dir`,
`X = normalize(wo - T*dot(wo,T))`, `Y = cross(T,X)`. The geometric `h`
and `r` are passed straight into the material — hits are never treated as
centerline hits.

### Approximate scattering model

This is an explicit four-lobe approximation, **not** a full Marschner
model: there is no scale/tilt angle and no volume scattering. All material
math runs in double precision, with `eta in [1.001, 3]`, RGB absorption
`sigma_a >= 0` per world length and `beta_m, beta_n in [0.02, 1]`.

With `so = dot(wo,T)`, `co = sqrt(1-so²)`, `gamma_o = asin(h)`,
`eta_p = sqrt(eta²-so²)/co`, `gamma_t = asin(h/eta_p)` and
`ct = sqrt(1-so²/eta²)`, the chord length is
`l = 2 r cos(gamma_t)/ct` and the per-channel transmission is
`C = exp(-sigma_a l)`. `F` is the exact unpolarized air-to-eta Fresnel
term at incidence cosine `co cos(gamma_o)`. The four attenuations are

```
A0 = F
A1 = (1-F)² C
A2 = A1 C F
A3 = A2 C F / (1 - C F)     // all higher orders merged
```

In the grazing limit where numerator and denominator vanish together,
`A3 = 0`. Absorbed energy disappears: the attenuated lobes are never
renormalized back to unit energy.

Directions `wi` use `s = dot(wi,T)` in `[-1,1]` and
`phi = atan2(dot(wi,Y), dot(wi,X))`, with solid angle `dω = ds dφ`.
Each lobe combines a truncated-normal longitudinal density on `[-1,1]`
(mean `-so`, standard deviation `(p+1) beta_m`) with an azimuthal
wrapped-Cauchy kernel `N_p` (lobe 3 is azimuthally uniform). The kernel
`K = sum_p A_p M_p N_p` is already directional energy per solid angle, so
no surface cosine is multiplied on top. Lobe selection uses the arithmetic
mean of each lobe's RGB attenuation; `sample` takes three `[0,1)` samples,
never retries, and returns both `K` and the full four-lobe mixture `pdf`
(not the picked lobe's density).

### Transport

`trace_fiber` takes the fibers, black `Sphere` occluders, a background
function of direction, a fiber-scatter budget and a deterministic sample
stream. A sphere hit in front returns zero; a clean miss returns the
background. At a fiber hit the per-channel weight is multiplied by
`K/pdf` and the ray continues along the sampled direction. The whole
internal chord is one material event; the next segment only skips the
fiber just processed, so every other fiber and sphere still participates
and the same fiber can be re-encountered after scattering elsewhere. The
minimum hit distance is `0.001`; exact ties go to the sphere first, then
to the smaller fiber id.

### Commands

```sh
# Compile all new production files and the three test groups, run them,
# then run the legacy sphere-scene baseline.
bash task_support/run.sh target

# Build and run the deterministic preview (argument: output directory).
c++ -std=c++17 -O2 -I. examples/fiber_preview.cpp fiber.cpp fiber_renderer.cpp -o fiber_preview
./fiber_preview ./preview_out
# writes preview_out/fiber_preview.ppm and preview_out/fiber_preview_reps.txt
```

The PPM shows highlight bands stretched along each filament, and the
representative-ray text file records the linear RGB (including the color
shift after light passes through absorbing fibers).
