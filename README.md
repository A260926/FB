# cpp-rasterizer

cpp-rasterizer is a software rasterizer written in C++. This rasterizer loads obj files and renders them interactively with Phong shading. Move with WASD, shift, space, and mouse.

## Build

You will need to install SDL2 to build. If you are on Ubuntu, then run `apt-get install libsdl2-dev`

1. Run `make`
2. Run `./rasterizer models/utah-teapot.obj`

## Headless thick-glass refraction preview

`src/refraction.h` / `src/refraction.cpp` add a windowless, dependency-free
thick-glass preview. The camera ray actually enters the glass, is refracted at
the entry interface, travels through the object, is refracted again at the
real exit surface and only then reaches the background — there is no
screen-normal background-coordinate offset. The original SDL program, its
lighting, depth comparison, pixel packing and model loading are untouched;
the headless build (`RASTERIZER_HEADLESS`, used by the task harness) only
disables the SDL presentation.

### Entry points

- `DepthTarget` / `BackgroundTarget` — size-configurable offscreen capture
  layers. `RasterizeTriangle` is the shared fill-triangle production path
  (the same geometry walk and fragment generation as the old
  `Rasterize`/`Render`, reusable for capture and tracing).
  `FaceMode::BackFaces` inverts the original back-face culling so the glass
  exit shell is captured.
- `TraceDepthField` / `TraceBackground` — analytic, continuous-parameter ray
  walks over a captured depth field. Each cell is visited at most once
  (bounded by `width + height + 1`); intersections are solved against the
  cell's depth plane, never found by fixed stepping. They return the hit
  pixel, continuous NDC coordinates, view-space segment distances and the
  number of cells visited.
- `RefractSnell` / `FresnelUnpolarized` — exact vector Snell refraction and
  unpolarized Fresnel reflectance (including total internal reflection).
- `Synthesize` — the full per-pixel composite; returns independent
  linear-RGB storage and statistics. The captures stay read-only, so
  re-running, changing `sigma_a`, or interleaving captures never feeds back.
- `WritePPM` — quantizes to 8-bit only at file time and writes a binary P6
  PPM, flipping the y-up buffer into top-down rows.
- `examples/refraction.cpp` drives the production interface end to end and
  writes a PPM.

### Transport approximation

One transmission only, no internal reflections:

```
colour = F_entry * ambient
       + T_entry * T_exit * exp(-sigmaA * internal_path_length) * background
```

`F`/`T` are exact unpolarized Fresnel terms at both interfaces; the internal
path length is the true view-space distance from entry to exit (not a
same-pixel depth difference). A missing exit or total internal reflection
zeros the transmitted term while keeping the entry Fresnel reflection; a
background miss uses the ambient colour. An entry behind the opaque
background keeps the background.

### Matrix and coordinate conventions

- The caller supplies the world→view, projection and normal matrices.
- View space: camera at the origin looking down **−Z**.
- Projection: finite perspective with `clip.w == -view.z`; positive view
  depth maps to NDC `z` in `[0, 1]`, NDC `x/y` in `[-1, 1]`. Non-square
  targets and asymmetric frusta are supported
  (`MakePerspective(l, r, b, t, n, f)` is provided).
- Input vertices are finite, already inside the frustum and have positive
  clip `w`; no clipping is added. Only filled triangles are processed.
- Offscreen pixel `(x, y)` is centred at `(x + 0.5, y + 0.5)`, x right /
  y up, stored at `y * width + x`; PPM rows are flipped on write.
- Positions use 1/w perspective interpolation; only normals are normalized
  after interpolation.

## Screenshots

![Stanford Dragon](/img/dragon.png)
![Utah Teapot](/img/teapot.png)
![Amogus](/img/sus.png)
