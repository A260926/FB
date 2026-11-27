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

## Differentiable renderer

`differentiable.h` / `differentiable.cpp` add a differentiable rendering path
that reuses the coverage, barycentric and depth-selection logic in
`our_gl.cpp`. The legacy `IShader`, `rasterize()` and the byte-image command
line keep their original behavior.

### Scene

`DiffScene::from_model(model, width, height)` builds an owned, editable scene
from a const `Model`:

- positions are shared by the original position index (one entry per vertex);
- UVs are stored **per face corner** and are never merged, even when two
  corners share a position;
- the floating-point RGB texture is initialized from `Model::diffuse()` by
  dividing each linear R, G, B channel by 255 (the TGA file stores BGRA, so the
  channel order is explicitly undone); it must be at least 2x2;
- `ModelView`, `Perspective` and `Viewport` are snapshotted from the global
  camera state at construction time (`ModelView` is assumed rigid).

Positions, corner UVs and texture texels are editable through the scene
accessors. UVs are the already-flipped `Model` UVs, and texture row indices
match `TGAImage::get`.

### Forward pass

```cpp
DiffImage image;
std::unique_ptr<DiffRecord> record = diff_render(scene, light_dir, image);
```

`light_dir` is a unit view-space direction pointing toward the light. The
material is linear diffuse only:

```
N     = normalize(cross(P1-P0, P2-P0))   // view-space face normal
color = T(u, v) * (0.2 + 0.8 * max(0, N dot L))
```

Integer pixel sampling, screen-area culling (`det < 1`), perspective-corrected
UV interpolation and the "larger z/w is nearer, equal depth keeps the face
drawn first" rule all come from the shared `our_gl` helpers. UVs are clamped
to `[0,1]`, mapped onto `[0,W-1] x [0,H-1]` and bilinearly interpolated; the
right/bottom edge reuses the last legal cell and grid lines pick the
right/lower cell. Outputs are unclamped `double` linear RGB, plus depth and the
winning face id (`-1` on zero background).

The returned `DiffRecord` owns every forward value it needs (matrices, view
geometry, barycentrics, 1/w, sampled texels, shading). Destroying the `Model`,
editing the scene, changing the global camera or rendering another frame does
not change an old record. At a given pixel only the final winning fragment is
stored, so an earlier occluded fragment leaves no adjoint contribution.

### Adjoint

```cpp
DiffGradients grad = diff_backward(record, g); // g: one RGB adjoint per pixel
```

Returns gradients of `sum(g dot C)` with respect to shared positions,
per-corner UVs and every RGB texel. The adjoint is hand-written (no autodiff
library, no re-rendering or numerical differences) and does work proportional
to the record's winning-pixel count. A record can be back-propagated with
different `g` any number of times; each result is independent and
zero-initialized.

Coverage, culling, face selection, rounding and texture-cell branches are
frozen (no silhouette or occlusion-switch derivatives), but the screen
barycentrics, 1/w, projection, perspective UV and unit-normal chains through
which positions affect color are all differentiated. UV-clamped regions and
the `max(0,.)` kink at zero get zero derivative; shared positions accumulate
contributions across faces; background and fully occluded faces produce no
gradient.

### Fixed-visibility limitation

Because coverage and face selection are held fixed from the forward record,
the gradients describe moving color within the currently visible pixels. They
do **not** include derivatives of triangles entering/leaving the framebuffer,
crossing silhouette edges or changing which face wins a depth contest. The
texture-fitting example therefore freezes geometry and the camera and
optimizes only the texture.

### Tests and example

```sh
# three registered groups (forward, adjoint, lifetime) plus the legacy baseline
bash task_support/run.sh target

# texture-fitting example: generates an OBJ + non-uniform TGA, descends the
# texture, writes before/after TGA images into the given directory
cmake -Bbuild && cmake --build build -j
build/fit_texture <output-directory>
```

The adjoint tests compare the hand-written gradients against two magnitudes of
central finite differences (the numerical reference exists only inside the
test file) on pixels away from coverage, face-selection and sampling branches,
and additionally verify cross-face gradient accumulation, zero contribution
from invisible faces and clamp/max kinks.
