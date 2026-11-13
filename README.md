[![Actions](https://github.com/sammycage/plutovg/actions/workflows/main.yml/badge.svg)](https://github.com/sammycage/plutovg/actions)
[![License](https://img.shields.io/badge/License-MIT-blue.svg)](https://github.com/sammycage/plutovg/blob/main/LICENSE)
[![Releases](https://img.shields.io/github/v/release/sammycage/plutovg)](https://github.com/sammycage/plutovg/releases)
[![CodeFactor](https://www.codefactor.io/repository/github/sammycage/plutovg/badge)](https://www.codefactor.io/repository/github/sammycage/plutovg)

# PlutoVG
PlutoVG is a standalone 2D vector graphics library in C.

## Features
- Path Filling, Stroking and Dashing
- Solid, Gradient and Texture Paints
- Tensor-Product Cubic Bézier Mesh Paints
- Fonts and Texts
- Clipping and Compositing
- Transformations
- Images

## Mesh paints

`plutovg_paint_create_mesh()` builds an ordinary `plutovg_paint_t*` from an
array of tensor-product cubic Bézier patches. Because it is a regular paint,
it flows through the existing fill, stroke, clip, opacity and compositing
machinery; there is no separate image generation step.

```c
typedef struct plutovg_mesh_patch {
    plutovg_point_t points[16];
    plutovg_color_t colors[4];
} plutovg_mesh_patch_t;

PLUTOVG_API plutovg_paint_t* plutovg_paint_create_mesh(
    const plutovg_mesh_patch_t* patches, int count,
    const plutovg_matrix_t* matrix);
```

### Parameter layout

Each patch is the tensor-product surface

```
S(u, v) = sum_{j=0}^{3} sum_{i=0}^{3}
              points[4 * j + i] * B_i^3(u) * B_j^3(v),   u, v in [0, 1]
```

so `points[4 * j + i]` is the control point of the Bernstein basis function
`B_i^3(u) * B_j^3(v)`. `colors[0..3]` are the corner colors for
`(u, v) = (0,0), (1,0), (1,1), (0,1)`. Their RGB channels are premultiplied by
their own alpha and then interpolated bilinearly in the parameter domain, so
a curved patch produces curved internal color bands rather than a straight
line between the corners.

### Transformations

The optional `matrix` is the paint matrix; `NULL` means the identity. Each
control point is mapped by the paint matrix first and then by the current
canvas matrix, producing the device-space control net.

### Requirements and behavior

- The patch control points and optional matrix must be finite, and the matrix
  must be invertible; otherwise `plutovg_paint_create_mesh()` returns `NULL`.
  A `NULL` array or a `count` of zero or less also returns `NULL`.
- The caller must guarantee that every patch is injective with a non-zero
  Jacobian. Folded or self-intersecting patches are not detected, and the
  patch orientation may be reversed (for example by a negative-determinant
  matrix).
- Inputs are copied at creation time, so the caller may modify or free the
  arrays afterwards. Reference counting, destruction, save/restore and reuse
  across canvases follow the same rules as the other paint types.
- Positions outside every patch are transparent. All color components are
  clamped to `[0, 1]`.
- Patching and shading are evaluated in device space using a regular 8x8 grid
  of subpixel centers per device pixel. Each subpixel belongs to at most one
  patch; when several patches cover it, the last patch in the array wins. The
  64 sampled premultiplied colors are averaged into the paint value, after
  which the normal path coverage, clip and compositing rules apply.
- A patch may be subdivided exactly into four patches whose corner colors
  follow the same bilinear color function without changing the result.

### Example

`examples/mesh_ppm.c` draws a curved patch onto a surface and writes a PPM
file (no image library required):

```bash
cc -std=c11 -DPLUTOVG_BUILD_STATIC -Iinclude examples/mesh_ppm.c \
   source/*.c -lm -o mesh_ppm
./mesh_ppm mesh.ppm
```

The example uses the ordinary canvas fill path and can be combined with
clipping or a non-default operator just like any other paint.

## Example
```c
#include <plutovg.h>

int main(void)
{
    const int width = 150;
    const int height = 150;

    const float center_x = width / 2.f;
    const float center_y = height / 2.f;
    const float face_radius = 70;
    const float mouth_radius = 50;
    const float eye_radius = 10;
    const float eye_offset_x = 25;
    const float eye_offset_y = 20;
    const float eye_x = center_x - eye_offset_x;
    const float eye_y = center_y - eye_offset_y;

    plutovg_surface_t* surface = plutovg_surface_create(width, height);
    plutovg_canvas_t* canvas = plutovg_canvas_create(surface);

    plutovg_canvas_save(canvas);
    plutovg_canvas_arc(canvas, center_x, center_y, face_radius, 0, PLUTOVG_TWO_PI, 0);
    plutovg_canvas_set_rgb(canvas, 1, 1, 0);
    plutovg_canvas_fill_preserve(canvas);
    plutovg_canvas_set_rgb(canvas, 0, 0, 0);
    plutovg_canvas_set_line_width(canvas, 5);
    plutovg_canvas_stroke(canvas);
    plutovg_canvas_restore(canvas);

    plutovg_canvas_save(canvas);
    plutovg_canvas_arc(canvas, eye_x, eye_y, eye_radius, 0, PLUTOVG_TWO_PI, 0);
    plutovg_canvas_arc(canvas, center_x + eye_offset_x, eye_y, eye_radius, 0, PLUTOVG_TWO_PI, 0);
    plutovg_canvas_set_rgb(canvas, 0, 0, 0);
    plutovg_canvas_fill(canvas);
    plutovg_canvas_restore(canvas);

    plutovg_canvas_save(canvas);
    plutovg_canvas_arc(canvas, center_x, center_y, mouth_radius, 0, PLUTOVG_PI, 0);
    plutovg_canvas_set_rgb(canvas, 0, 0, 0);
    plutovg_canvas_set_line_width(canvas, 5);
    plutovg_canvas_stroke(canvas);
    plutovg_canvas_restore(canvas);

    plutovg_surface_write_to_png(surface, "smiley.png");
    plutovg_canvas_destroy(canvas);
    plutovg_surface_destroy(surface);
    return 0;
}
```

output:

![smiley.png](smiley.png)

## Installation

Follow the steps below to install PlutoVG using either [Meson](https://mesonbuild.com/) or [CMake](https://cmake.org/).

### Using Meson

```bash
git clone https://github.com/sammycage/plutovg.git
cd plutovg
meson setup build
meson compile -C build
meson install -C build
```

### Using CMake

```bash
git clone https://github.com/sammycage/plutovg.git
cd plutovg
cmake -B build .
cmake --build build
cmake --install build
```

## Projects using PlutoVG
- [LunaSVG](https://github.com/sammycage/lunasvg)
- [PlutoSVG](https://github.com/sammycage/plutosvg)
