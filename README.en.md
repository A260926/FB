# mini3d

3D software rendering tutorial, without any performance optimization, mainly explains how to write a fixed pipeline software renderer. Although the main code is only 700 lines, it is small but complete.

## Features

* Single File: The source code consists of only one file, mini3d.c, which implements all the functionality. It is easy to read and understand.
* Independent Compilation: There is no third-party library dependency and no complex project directory.
* Model Standard: Uses the standard D3D coordinate model, left-handed system with WORLD/VIEW/PROJECTION matrices.
* Clipping Implementation: Implements simple CVV (Canonical View Volume) clipping.
* Texture Support: Supports textures up to a maximum of 1024 x 1024.
* Depth Buffering: Uses a depth buffer to determine the order of image rendering.
* Perspective Texture Mapping: Implements perspective texture mapping and perspective color filling.
* Edge Calculation: Accurately calculates polygon edge coverage.
* Simplified Implementation: The rendering engine consists of only 700 lines of code, with clear modules and a prominent main structure.
* Detailed Comments: The primary code is well-documented with detailed comments.

## Compile

* mingw: 
		gcc -O3 mini3d.c -o mini3d.exe -lgdi32
* msvc:
		cl -O2 -nologo mini3d.c 
* Compiled version:
[https://github.com/skywind3000/mini3d/releases](https://github.com/skywind3000/mini3d/releases)

## Demonstration

Texture Mapping：RENDER_STATE_TEXTURE
![](https://raw.githubusercontent.com/skywind3000/mini3d/master/images/mini_1.png)

Color Filling：RENDER_STATE_COLOR
![](https://raw.githubusercontent.com/skywind3000/mini3d/master/images/mini_0.png)

Wireframe Rendering：RENDER_STATE_WIREFRAME
![](https://raw.githubusercontent.com/skywind3000/mini3d/master/images/mini_2.png)

Added Lighting and Bicubic Interpolation (screenshot of lighting effect added by a friend to Mini3D)
![](https://raw.githubusercontent.com/skywind3000/mini3d/master/images/mini_3.png)

## Basic TO-DO:

* Add backface culling
* Implement simple lighting
* Provide more rendering modes
* Implement texture sampling with bicubic interpolation

## Advanced TO-DO:

* Derive and prove all geometric knowledge used in the program
* Optimize vertex computation performance
* Optimize draw_scanline performance
* Load textures from BMP/TGA files
* Load BSP scenes and implement roaming.

## Explanation of principles

- [Wei Yixiao: How do OpenGL and DirectX determine the position of pixels only based on vertices?](https://skywind.me/blog/archives/2594)
- [Wei Yixiao: How does the computer access the graphics card at a low level?](https://skywind.me/blog/archives/1774)

## Related Projects

- [RenderHelp](https://github.com/skywind3000/RenderHelp)：Another implementation of a streamlined software renderer with support for programmable rendering pipelines, for more details see https://skywind.me/blog/archives/2589

## Offline Glass Caustics Pipeline

A deterministic beam-flux caustics channel was added on top of the original
matrix / trapezoid / scanline pipeline. It is plain C99 with no window, GPU,
or third-party library:

* `mini3d.h` exposes the renderer's public interface. `vertex_t` gains a
  `world` field for perspective-correct world positions and `device_t` gains
  caustics binding fields; when unbound, output is identical to the original.
* `caustics.h` / `caustics.c` implement single-interface parallel-light
  refraction: an orthographic light view rasterizes the input triangles
  (six homogeneous clip planes, pixel-center sampling, minimum z/w with the
  smaller face id winning ties), each sample is refracted once with Snell's
  law onto the receiver plane, groups of four light samples form two beams
  along a fixed diagonal, and each triangular footprint deposits its flux by
  its true overlap area with receiver texels. Overlapping and flipped
  footprints accumulate; off-map flux is lost without renormalization.
* `caustics_bind_device` attaches the map to a `device_t`; fragments whose
  perspective-interpolated world position lies within an explicit tolerance
  of the receiver plane are shaded as
  `base * (ambient + gain * bilinear_irradiance)` with a single quantization.
  Binding never changes rhw occlusion or texture-over-color priority, never
  writes to the map, and two devices or repeated draws never interfere.

### Usage

```c
#include "caustics.h"

caustics_map *map = caustics_map_create(&receiver);   // plane origin, unit axes, ranges
caustics_capture(map, &input);    // triangles, normals, transmission, light matrix, IORs
caustics_bind_device(map, &device, tolerance, ambient, gain);
device_draw_primitive(&device, &v1, &v2, &v3);       // fill vertex world positions
caustics_unbind_device(&device);
caustics_map_release(map);
```

Read-only queries: `caustics_get_sample`, `caustics_get_irradiance`,
`caustics_sample_irradiance` (bilinear, zero outside the map). See `caustics.h`
for full parameter and ownership documentation.

```sh
# Offline demo: writes caustics_map.ppm and caustics_camera.ppm
cc -std=c99 -DMINI3D_HEADLESS -I. examples/caustics.c \
   mini3d.c caustics.c -lm -o caustics_demo && ./caustics_demo

# capture / flux / camera test groups plus the original color/texture baseline
bash task_support/run.sh target
```

## Welcome to donate:

![](https://raw.githubusercontent.com/skywind3000/mini3d/master/images/donation.png)

Your donation is the greatest recognition for this tutorial. You are welcome to use Alipay to scan the QR code above to make a donation. The donated funds will be used to improve the tutorial documentation and illustrations, as well as to help me write more interesting tutorials.



## Welcome to follow us:

blog: https://skywind.me/blog

zhihu: https://www.zhihu.com/people/skywind3000 
