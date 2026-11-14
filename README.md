# Software rendering in 500 lines of bare C++

The code itself is of little interest. Check the course notes:
1. [Introduction](https://haqr.eu/tinyrenderer/)
2. [Bresenham’s line drawing algorithm](https://haqr.eu/tinyrenderer/bresenham/)
3. [Triangle rasterization](https://haqr.eu/tinyrenderer/rasterization/)
4. [Primer on barycentric coordinates](https://haqr.eu/tinyrenderer/barycentric/)
5. [Hidden faces removal](https://haqr.eu/tinyrenderer/z-buffer/)
6. [A crude (but simple) approach to camera handling](https://haqr.eu/tinyrenderer/camera-naive/)
7. [Better camera handling](https://haqr.eu/tinyrenderer/camera/)
8. [Shading](https://haqr.eu/tinyrenderer/shading/)
9. [More data!](https://haqr.eu/tinyrenderer/textures/)
10. [Tangent space normal mapping](https://haqr.eu/tinyrenderer/tangent/)
11. [Shadow mapping](https://haqr.eu/tinyrenderer/shadow/)
12. [Indirect lighting](https://haqr.eu/tinyrenderer/ssao/)
13. [Bonus: toon shading](https://haqr.eu/tinyrenderer/toon/)
14. [Afterword](https://haqr.eu/tinyrenderer/afterword/)

In this series of articles, I aim to demonstrate how OpenGL, Vulkan, Metal, and DirectX work by writing a simplified clone from scratch.
Surprisingly, many people struggle with the initial hurdle of learning a 3D graphics API.
To help with this, I have prepared a short series of lectures, after which my students are able to produce quite capable renderers.

The task is as follows: using no third-party libraries (especially graphics-related ones), we will generate an image like this:

![](https://haqr.eu/tinyrenderer/home/africanhead.png)

_Warning: This is a training material that loosely follows the structure of modern 3D graphics libraries.
It is a **software renderer**.
**I do not intend to show how to write GPU applications — I want to show how they work.**
I firmly believe that understanding this is essential for writing efficient applications using 3D libraries._

## The starting point

The final code consists of about 500 lines.
My students typically require 10 to 20 hours of programming to start producing such renderers.
The input is a 3D model composed of a triangulated mesh and textures.
The output is a rendering.
There is no graphical interface, the program simply generates an image.

To minimize external dependencies, I provide my students with a single class for handling [TGA](http://en.wikipedia.org/wiki/Truevision_TGA) files —
one of the simplest formats supporting RGB, RGBA, and grayscale images.
This serves as our foundation for image manipulation.
At the beginning, the only available functionality (besides loading and saving images) is the ability to set the color of a single pixel.

There are no built-in functions for drawing line segments or triangles — we will implement all of this manually.
While I provide my own source code, written alongside my students, I do not recommend using it directly, as doing the work yourself is essential to understanding the concepts.
The complete code is available on [github](https://github.com/ssloy/tinyrenderer), and you can find the initial source code I provide to my students [here](https://github.com/ssloy/tinyrenderer/tree/706b2dfecff65daeb93de568ee2c2bd87f277860).
Behold, here is the starting point:

```cpp
#include "tgaimage.h"

constexpr TGAColor white   = {255, 255, 255, 255}; // attention, BGRA order
constexpr TGAColor green   = {  0, 255,   0, 255};
constexpr TGAColor red     = {  0,   0, 255, 255};
constexpr TGAColor blue    = {255, 128,  64, 255};
constexpr TGAColor yellow  = {  0, 200, 255, 255};

int main(int argc, char** argv) {
    constexpr int width  = 64; 
    constexpr int height = 64; 
    TGAImage framebuffer(width, height, TGAImage::RGB);

    int ax =  7, ay =  3;  
    int bx = 12, by = 37; 
    int cx = 62, cy = 53; 

    framebuffer.set(ax, ay, white);
    framebuffer.set(bx, by, white);
    framebuffer.set(cx, cy, white);

    framebuffer.write_tga_file("framebuffer.tga");
    return 0;
}
```

It produces the 64x64 image `framebuffer.tga`, here I scaled it for better readability:

![](https://haqr.eu/tinyrenderer/bresenham/bresenham0.png)


## Teaser: few examples made with the renderer

![](https://haqr.eu/tinyrenderer/home/demon.png)

![](https://haqr.eu/tinyrenderer/home/diablo-glow.png)

![](https://haqr.eu/tinyrenderer/home/boggie.png)

![](https://haqr.eu/tinyrenderer/home/diablo-ssao.png)

## Compilation

```sh
git clone https://github.com/ssloy/tinyrenderer.git &&
cd tinyrenderer &&
cmake -Bbuild &&
cmake --build build -j &&
build/tinyrenderer obj/diablo3_pose/diablo3_pose.obj obj/floor.obj
```
The rendered image is saved to `framebuffer.tga`.

## Differentiable rasterizer

`differentiable.h` / `differentiable.cpp` add a differentiable rendering
interface next to the classic `rasterize()` path. The classic `IShader`,
`rasterize()` and the command-line byte images are untouched: both paths share
the coverage, barycentric and depth-selection logic in `raster_core.h`
(`raster_core::prepare()`, `bary_screen()`, `bary_clip()`, `depth()`,
`depth_passes()`, `pixel_bounds()`).

### Interface

- `DiffScene make_scene(const Model&)` creates an owning scene from a mesh
loaded by `Model`. Positions are shared by the original position index; UVs are
stored per face corner and never merged. `Model::face_pos(face, corner)` /
`Model::face_positions()` expose the read-only position indices.
- A scene is editable: `positions`, `corner_uv`, and a float RGB texture of at
least 2x2 texels (`tex_width`, `tex_height`, `tex_rgb`). The initial texture is
`diffuse()`'s R/G/B divided by 255 (TGA stores BGR, so the channels are
swapped), and its row order matches `TGAImage::get`.
- `DiffForward diff_forward(scene, width, height, light)` snapshots the current
global `ModelView`, `Perspective` and `Viewport` and renders one frame. The
`light` argument is the unit *view-space* direction towards the light source.
The ModelView must be rigid; callers guarantee positive finite non-degenerate
`w` inside the frustum. Output size is the given width and height.
- Pixel sampling, area culling and the "larger z/w is nearer" depth rule are
identical to `rasterize()`: integer pixel centres, `z <= zbuffer` rejected,
equal depths keep the face drawn first, and background pixels are zero
(zero colour, zero depth, face `-1`).
- The material is linear diffuse only: `C = T(u,v) * (0.2 + 0.8*max(0, N.L))`,
with `N = normalize(cross(P1-P0, P2-P0))` in view space. Output is unclamped
`double` RGB. UVs are interpolated with the classic perspective-correct
weights. Sampling clamps `u,v` to `[0,1]`, maps them to
`(u*(W-1), v*(H-1))` and mixes bilinearly; the origin is `floor`, the right/lower
cell wins on an interior grid line, and the bottom-right endpoint uses the
last legal cell.
- `diff_forward` returns linear RGB, depth and the winning face id per pixel.
The `DiffForward` record owns all forward values it needs: it remains valid
after the source Model is destroyed, after the scene is edited, after the
global camera changes, or after another frame is rendered.
- `DiffGrad diff_backward(const DiffForward&, g_rgb)` returns the gradient of
`sum_pixels g . C` with respect to positions, per-corner UVs and each RGB
texel. Gradients start from zero on every call, so one record can be backpropagated
repeatedly with different `g`. Work is proportional to the number of stored
records, not a re-render or a per-parameter image sweep. The implementation is
hand-written adjoint code; no autodiff library is used.

### Fixed-visibility limitation

Backpropagation differentiates the colour within the coverage, culling, face
selection, pixel rounding and bilinear-cell branches fixed by the forward
pass. It does *not* differentiate silhouette or occlusion changes, a change of
the winning face, or a jump to another texel cell. The screen-space
barycentric weights, clip `w` and the unit face normal are all treated as
differentiable; the chain through projection, perspective-correct UVs, the
face normal and shading is preserved. UV gradients are zero outside the clamp
and exactly on its endpoints. The `max(0, N.L)` derivative is zero at zero.
Contributions of faces sharing a position are summed; background pixels,
hidden faces and never-sampled texels get zero.

### Example

`examples/fit_texture.cpp` generates a small OBJ and an uneven TGA diffuse
map, loads them through `Model`, and fits the texture with a few gradient
descent steps. It reports the squared error on the initially visible pixels
before and after, and writes before/after renderings plus the fitted texture.

```sh
# from the repository root
c++ -std=c++20 -O2 -I. examples/fit_texture.cpp \
    our_gl.cpp model.cpp tgaimage.cpp differentiable.cpp -o fit_texture
mkdir -p out
./fit_texture out
```

It prints `visible_pixels=...`, `mse_before=...` and `mse_after=...`; the
error decreases substantially. Outputs are `out/fit_texture_before.tga`,
`out/fit_texture_after.tga`, `out/fit_texture_target.tga` and
`out/fit_texture_fitted.tga`.

### Tests

```sh
bash task_support/run.sh target
```

This builds and runs `tests/differentiable_cases.cpp` (the `forward`,
`adjoint` and `lifetime` groups) with `TASK_CHECK`, then builds and runs the
existing legacy rasterization baseline. The adjoint cases use their own
scalar loss and two-step central differences on parameters chosen away from
branch switches, with an independent reference renderer for the forward
values.

