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

## Windowless projective texture baking

`projective_baker.h/.cpp` adds a headless interface that re-projects colors
from several observation views back onto the per-face corner UVs of the
models, producing a floating point material atlas. It never opens a window
and never re-draws the source views.

The pipeline is:

1. **Depth capture.** `SourceCapture` takes the same geometry, one
   world-to-clip matrix, the world camera position and a row-major linear
   `double` RGB image. It rasterizes the geometry with the *same*
   coverage/perspective/depth kernel as the regular `rasterize()` path
   (`rasterize_depth()` in `our_gl.cpp` shares that kernel), honoring integer
   pixels, the forward area `det >= 1` rule and the "larger `z/w` is nearer"
   depth commit. Each capture owns private copies of the geometry, image,
   projection data and depth buffer, so destroying/editing the caller's
   `Model`s or images, changing the global camera state or interleaving other
   captures does not change an existing capture.
2. **UV bake.** `bake_atlas()` walks the UV triangles (no backface culling,
   mirrored UVs allowed) with a top-left half-open coverage rule after
   normalizing winding to counter-clockwise. The world point `P` and unit
   world geometric normal `N` are reconstructed from UV-space barycentrics
   only. For every source view, `P` is projected to
   `s=(ndc.x+1)*width/2`, `t=(ndc.y+1)*height/2` and bilinearly sampled on the
   integer pixel grid. Visibility is decided **per tap**: the receiver
   triangle's three `z/w` values are extrapolated on the projection plane to
   each integer tap, and a tap is accepted only when its depth is at least the
   captured depth minus the caller supplied NDC epsilon. Each accepted tap
   contributes `bilinear_weight * pow(max(0, N·normalize(eye-P)), 4)`; RGB is
   accumulated across all taps and all views before a single division by the
   total weight. Texels with no valid source keep their coverage and ownership
   but stay at zero color/weight. True UV overlaps of distinct faces are
   reported in `OverlapConflict` instead of being resolved by submission order.
3. **State hygiene.** Both capture and bake save and restore the global
   `ModelView`/`Viewport`/`Perspective`/`zbuffer` rasterizer state, and an empty
   source list is legal (coverage-only bake).

Minimal usage:

```cpp
#include "projective_baker.h"
using namespace projective_baker;

ProjectiveGeometry geometry({&model});                 // independent geometry snapshot
SourceCapture view(geometry, world_to_clip, eye,      // captures depth with the
                   linear_rgb, width, height);         // real rasterize() kernel
std::vector<OverlapConflict> conflicts;
BakeResult atlas = bake_atlas(geometry, {&view},       // covered[], owner_model/face[],
                              atlas_w, atlas_h, 1e-9,  // rgb[] and weight[] per texel
                              &conflicts);
```

Invalid dimensions/indices, non-finite values, UVs outside `[0,1]` and image
length mismatches throw `projective_baker::bake_error` before any result is
modified.

### Demo and tests

The demo generates a seam-split grid, two differently colored observation
images and an occluder, writes the baked atlas (plus a weight image and
source views) as TGA, installs the atlas as the model's diffuse material and
renders it through the original `IShader`/`PhongShader` path:

```sh
cmake -Bbuild && cmake --build build -j
build/projective_baker <output_directory>
```

The headless numeric test suite (projection, visibility and lifetime groups,
no `main` of its own) is registered through `task_support/check.h` and runs
together with the original production baseline:

```sh
bash task_support/run.sh target
```
