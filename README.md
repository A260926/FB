# Pingo

[![CI Build and Test](https://github.com/fededevi/pingo/workflows/CI%20Build%20and%20Test/badge.svg)](https://github.com/fededevi/pingo/actions)

A small software 3D renderer in C. No GPU, no dynamic allocation and no
dependencies beyond `libm`, so it runs anywhere from a desktop window to a
microcontroller or a terminal. The libraries claim no memory of their own —
all of it is the buffers and meshes you hand them.

![Example](/public/viking.png)

## How it works

Pingo rasterizes triangles into a plain pixel buffer. It never owns that buffer:
a **backend** supplies it, and Pingo only fills it in.

One frame of `renderer_render()`:

1. Clear the depth buffer (and the framebuffer, if `clear` is set).
2. Ask the backend for the framebuffer and depth buffer, and call `beforeRender`.
3. Walk the scene graph from `root_renderable` with an identity transform.
4. Call `afterRender`, which is where the backend presents the pixels.

The scene graph is built from one interface — a struct whose first member is a
render function pointer:

```c
typedef struct {
    int (*render)(void *this, Mat4 transform, Renderer *renderer);
} Renderable;
```

Anything embedding a `Renderable` as its first field is drawable, which is how C
gets polymorphism here:

- **`Entity`** — a transform, a visibility flag, and child entities. Composing
  these is what makes the graph a tree; each node concatenates its transform and
  recurses.
- **`Object`** — a `Mesh` plus a `Material`. This is what actually reaches the
  rasterizer.
- **`Sprite`** — a `Texture` drawn straight through the given transform,
  bypassing the camera.

The camera lives on the `Renderer` as `camera_projection` and `camera_view`.
Backface culling, frustum culling and early-Z are toggles on the same struct
(`renderer_enable_*`), all on by default.

## Libraries

Three libraries. Each is built by its own directory, which also holds that
library's tests:

| Library | Contents |
|---------|----------|
| `pingo_math` | `Vec2/3/4`, `Mat3/4`, fixed-size and allocation-free |
| `pingo_render` | Renderer, rasterizer, scene graph, texture, depth buffer |
| `pingo_assets` | Generated mesh tables (viking, cube, teapot) |

`pingo_render` links `pingo_math`, and `pingo_assets` uses the `Mesh` type from
`pingo_render`, so linking the renderer alone brings the maths with it.

The renderer's tests draw into memory at 64x48 and compare the result against
committed PPM references, so they run on every platform - including the
cross-compiled ones, where no window backend exists. Regenerate the references
after a deliberate change by running the test executable with
`--write-references`.

## Backends

A backend is five function pointers — `init`, `beforeRender`, `afterRender`,
`getFrameBuffer`, `getZetaBuffer`. Port Pingo somewhere new by implementing
those and handing the struct to `renderer_init()`.

| Example | Output | Needs |
|---------|--------|-------|
| `linux_window` | X11 window | libX11 |
| `linux_framebuffer` | `/dev/fb0` directly | — |
| `terminal` | ASCII art in the terminal (any platform) | — |
| `render_to_image` | JPEG file | libjpeg |
| `win_window` | Win32 GDI window | Windows |

Every one of them links the same `main.c`; only the backend library differs.

## Build

Out-of-source builds only.

```bash
cmake --workflow --preset default   # configure, build, test
```

Binaries land together in the preset's build directory. The examples read their
texture relative to the working directory, so run them from there.

`--workflow` runs three steps in order — configure, build, then the test
suite — and stops at the first failure. There are three presets:

| Preset | Build type | What it produces |
|--------|-----------|------------------|
| `default` | `Release` | every library, example and test the host can build |
| `debug` | `Debug` | the same, unoptimized |
| `freestanding` | `Release` | the libraries alone, static, for a target with no OS |

`freestanding` is what a microcontroller build looks like, and takes its cross
compiler from `CC` and `CFLAGS`.

libX11 and libjpeg are probed at configure time and the examples needing them
skipped with a message, so any environment builds whatever it can without being
told what it has.

`cmake --install <build dir> --prefix <dir>` installs the libraries, headers and
a package config, so another project can `find_package(pingo)` and link
`pingo::pingo_render`.

## Baked lightmaps

For a static scene the diffuse indirect light can be baked once with
`render/lightmap.h` and reused from any number of cameras, including
off-screen renders.

`bake_scene_build()` freezes an `Entity` subtree into an owned `BakeScene`:
world-space triangles, per-corner secondary (lightmap) UVs, constant
linear-RGB reflectance and emission, and the entity path of every visible
`Object`. The source tree, meshes, bindings and materials can all be released
afterwards; the snapshot keeps its own copies and bakes independently. The
same `Object` attached in two places becomes two instances, keyed by their
child-index paths and never merged by pointer.

### Fixed-sample approximation

Transport is a deliberately small, deterministic approximation rather than
Monte Carlo. Each covered texel shoots a fixed number `S` of cosine-weighted
hemisphere rays whose directions come from a fixed low-discrepancy sequence
(a jittered cosine ring using a base-2 radical inverse), not from a random
generator. Every round therefore produces bit-identical output on every
machine, stepping one round at a time gives the same answer as stepping them
together, and there is no noise to converge - only the discretization error of
the fixed sample set. Rays find the nearest world triangle with two-sided
occlusion; back-face hits absorb, misses take a constant environment
radiance, and a round always reads one immutable copy of the previous round's
radiance. The new radiance is `emission + reflectance * mean(hit radiance)`
with no extra factor of pi.

### Nearest sampling of the secondary UV

Every lightmap is a small grid addressed by its own secondary UV channel,
independent of the mesh's texture coordinates. A surface point is rebuilt
from a texel center at `((x+0.5)/W, (y+0.5)/H)` in that UV domain, and
lighting is both written and read with **nearest-texel** lookup plus
clamp-to-edge: the coordinate selects the single texel whose cell contains
it, and coordinates at or beyond the border clamp to the edge texel rather
than wrapping. Faces are re-oriented to a common winding and rasterized with
a half-open top/left rule, so texel centers on a shared edge are covered by
exactly one face; two positive-area faces claiming the same texel is a build
error. At camera time the secondary UV is perspective-interpolated across
each fragment and sampled the same nearest way, so changing the camera never
invalidates the bake. `renderer_render_lightmapped()` draws through the same
transform, coverage and depth path as `renderer_render()`, but shades with
the baked radiance multiplied by the material base color and an exposure,
and only publishes the frame after all validation succeeds.
