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

### Point lights

An `Entity` can carry a borrowed list of point lights, in its own local space:

```c
typedef struct PointLight {
    Vec3f position;   // local to the entity
    Vec3f intensity;  // non-negative linear RGB
    float range;      // world distance, not scaled with the node
} PointLight;

int entity_set_point_lights(Entity *entity, const PointLight *lights,
                            size_t count);
```

The list is borrowed — it must outlive rendering, and is not copied. A NULL
pointer with a count of zero (the default for both `entity_init` entry points)
means "no lights". A light's position goes through the node's full accumulated
transform, including scale, and applies to that node's own model *and* every
visible descendant. Descendants append their own lists; leaving the subtree
drops them again. The same `Object` may be referenced by several nodes — each
drawing uses the lights of the node currently being walked, never a list shared
between visits.

Lighting itself is off by default, so an existing scene keeps its old image.
Turn it on with:

```c
renderer_set_point_lighting(&renderer, true, (Vec3f){0.05f, 0.05f, 0.05f});
```

The third argument is an ambient colour added to every shaded fragment. With
lighting on, each fragment gets a world position (the screen barycentrics
scaled by the vertices' inverse clip `w` and normalised) and its triangle a
world-space unit normal from the winding. For each light in the effective
list, with `d` the distance to the fragment:

```
intensity * max(dot(N, (position - P) / d), 0) * (1 - d/range)^2 / (1 + d*d)
```

only while `0 < d < range`; anything else contributes zero. The ambient plus
all contributions is multiplied per channel by the base colour (texture or
white with no material), clamped to the byte range and truncated, keeping the
base alpha.

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
