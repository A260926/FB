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

## Fiber material preview path

The repository adds a deterministic, double-precision fiber preview path next
to the unchanged sphere tracer:

- `core.h/.cpp` — the shared core: the original `vec3`, `Material`, `Sphere`
  and sphere intersection (behaviour unchanged), plus a `dvec3` double vector
  used by all fiber math.
- `fiber.h/.cpp` — finite open-cylinder fiber geometry and the scattering
  material.
- `fiber_renderer.h/.cpp` — transport through fibers plus black sphere
  occluders and a caller-supplied directional background.
- `examples/fiber_preview.cpp` — a standalone preview program.
- `tests/fiber_cases.cpp` — geometry / scattering / transport checks.

### Geometry

A fiber is the lateral surface of a finite open cylinder given by endpoints
`A`, `B` and radius `r`: there are no end caps and no triangle mesh, and a ray
parallel to the axis never hits. Intersection searches `t in [tmin, tmax)` and
only accepts hits whose axial parameter is in `[0, 1]`; tangent rays count as
hits. Each hit reports a stable fiber id, `t`, the position, the outward unit
normal and the section coordinate `h = dot(N, Y)`, where
`T = normalize(B - A)`, `wo = -ray direction`,
`X = normalize(wo - T*dot(wo, T))`, `Y = cross(T, X)`.

### Approximate four-lobe scattering model

This is **not** a full Marschner model: the cylinder is azimuthally symmetric
(no cuticle tilt) and there is no volumetric scattering. All shading math is
in `double`. With `so = dot(wo, T)`, `co = sqrt(1 - so^2)`,
`gamma_o = asin(h)`, the projected IOR, refracted section angle, axial cosine
and chord length are

```
eta_p   = sqrt(eta^2 - so^2) / co
gamma_t = asin(h / eta_p)
ct      = sqrt(1 - so^2 / eta^2)
l       = 2*r*cos(gamma_t) / ct
C       = exp(-sigma_a * l)              # per RGB channel, sigma_a per world length
F       = exact unpolarized air->eta Fresnel at co*cos(gamma_o)
A0 = F, A1 = (1-F)^2 C, A2 = A1 C F, A3 = A2 C F / (1 - C F)
```

`A3` merges every bounce order above TRT; where its numerator and denominator
vanish together (the grazing limit) `A3` is taken as zero. The absorbed
four-lobe energy is reported as-is and is never renormalized back to one.

The directional energy kernel with respect to solid angle
(`d omega = ds dphi`, `s = dot(wi, T)`,
`phi = atan2(dot(wi, Y), dot(wi, X))`) is

```
K(wi) = sum_p A_p * M_p(s) * N_p(phi)
```

where `M_p` is a truncated normal on `[-1, 1]` with mean `-so` and standard
deviation `(p+1)*beta_m`, `N_p` (p = 0,1,2) is the periodic Poisson-kernel
density centered at `2*p*gamma_t - 2*gamma_o + p*pi` with
`rho_p = exp(-(p+1)*beta_n)`, and `N3 = 1/(2*pi)` is uniform. K already
contains every cosine factor; callers do not multiply by another surface
cosine. Lobe selection probabilities use the RGB arithmetic mean of each
lobe's attenuation; `fiber_sample` consumes exactly three `[0, 1)` samples
(lobe, longitudinal CDF, azimuthal CDF) without rejection and returns K with
the full four-lobe mixture pdf. Parameter ranges are validated before
tracing: `eta in [1.001, 3]`, `sigma_a >= 0` per channel,
`beta_m, beta_n in [0.02, 1]`; non-finite input, zero-length fibers and
non-positive radii are rejected.

### Commands

Run the complete target pipeline — syntax checks of the new files, the three
test groups and the original sphere scene baseline:

```sh
bash task_support/run.sh target
```

Build and run the preview (it takes only an output directory and writes a PPM
plus a text record of representative linear RGB rays):

```sh
c++ -std=c++17 -O2 -I. examples/fiber_preview.cpp core.cpp fiber.cpp \
    fiber_renderer.cpp -o fiber_preview
./fiber_preview ./preview_out
```

Everything uses the C++17 standard library only.
