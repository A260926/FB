# Software Renderer ![](https://github.com/zauonlok/renderer/workflows/Windows/badge.svg) ![](https://github.com/zauonlok/renderer/workflows/macOS/badge.svg) ![](https://github.com/zauonlok/renderer/workflows/Linux/badge.svg)

<img src="assets/helmet/screenshot2.png" width="410"> <img src="assets/helmet/screenshot3.png" width="410">
<img src="assets/drone/screenshot1.gif" width="410"> <img src="assets/drone/screenshot2.gif" width="410">

This is a shader-based software renderer written from scratch in C89 with
minimal dependencies, available for Windows, macOS, and Linux.

## Features

* Cross platform
* Minimal dependencies
* Shader based
* Homogeneous clipping
* Back-face culling
* Perspective correct interpolation
* Depth testing
* Alpha testing
* Alpha blending
* Cubemapped skybox
* Skeletal animation
* Tangent space normal mapping
* Shadow mapping
* ACES tone mapping
* Blinn–Phong reflection model
* Physically based rendering (PBR)
* Metallic-roughness workflow
* Specular-glossiness workflow
* Image-based lighting (IBL)
* Orbital camera controls
* PBR material inspector

## Download

Pre-built binaries for Windows, macOS, and Linux are available for download
from the [Releases page](https://github.com/zauonlok/renderer/releases).

## Build

To build the software renderer from source, a C89 compiler and development
files for your window system are required.

### Windows

Install [Visual Studio](https://visualstudio.microsoft.com/downloads/)
with C++ support and run `build_win32.bat`.

### macOS

Install Command Line Tools for Xcode with the command below and
run `build_macos.sh`.

```
xcode-select --install
```

### Linux

Install GCC and Xlib with the following commands and run `build_linux.sh`.

#### Ubuntu / Debian

```
sudo apt install gcc libx11-dev
```

#### Fedora / RHEL

```
sudo dnf install gcc libX11-devel
```

#### openSUSE / SUSE

```
sudo zypper install gcc libX11-devel
```

### Bonus

A `CMakeLists.txt` file is provided for generating project files using
[CMake](https://cmake.org/) (see examples below).

#### Visual Studio

```
mkdir build
cd build
cmake -G "Visual Studio 16 2019" ..
start Renderer.sln
```

#### Xcode

```
mkdir build
cd build
cmake -G Xcode ..
open Renderer.xcodeproj
```

#### Makefile

```
mkdir build
cd build
cmake -G "Unix Makefiles" -D CMAKE_BUILD_TYPE=Release ..
make
```

## Usage

### Visibility buffer (re-colorable preview)

`renderer/core/visibility.h` provides an opaque `visibility_t` buffer that
records the final visible triangles once and can then be shaded repeatedly
with new uniforms — handy for offline material previews, where changing a
color scheme should not re-run vertex transformation, clipping or
occlusion.

The geometry stage runs the same clipping, coverage tolerance, back-face
culling, `[0, 1]` depth range and `<=` equal-depth replacement rules as
`graphics_draw_triangle`; both pipelines call the shared internals in
`renderer/core/graphics_internal.h`, so they cannot diverge.  Each covered
pixel retains only the original triangle's id, its barycentric weights in
the caller's three original vertices, its depth and its facing.  The
buffer itself owns a copy of the three original-vertex varying blocks for
every submission, so callers may overwrite or release the program after
`visibility_draw_triangle` returns.

```c
#include <stdint.h>
#include "renderer/core/graphics.h"
#include "renderer/core/visibility.h"

/* program must have blending disabled; fragment shader is never called */
visibility_t *visibility = visibility_create(640, 480);

/* geometry stage: three vertex-shader calls, no fragment shading */
visibility_draw_triangle(visibility, program, /* primitive_id */ 7, coverage);

/* query the retained sample; background reports id UINT32_MAX */
visibility_sample_t sample;
int covered = visibility_read(visibility, x, y, &sample);
/* sample.bary[0..2] weight the caller's original three vertices, sum to 1 */
/* sample.depth, sample.backface, sample.primitive_id */

/* shading stage: one shader call per covered pixel, re-runnable */
palette_t palette_a;   /* different uniforms, same geometry */
visibility_resolve(visibility, framebuffer, resolve_palette, &palette_a);
visibility_resolve(visibility, framebuffer, resolve_palette, &palette_b);

visibility_clear(visibility);   /* drop all submissions, depth back to 1 */
visibility_release(visibility);
```

`visibility_draw_triangle` returns 0 on success.  `primitive_id` must be
unique until the next `visibility_clear` and must not be `UINT32_MAX`.
Programs created with `enable_blend` set are rejected before any callback
runs or any buffer state changes.  The optional coverage callback

```c
int visibility_coverage_t(void *varyings, void *uniforms, int backface);
```

receives the perspective-correct interpolated varyings and the program's
uniforms and keeps the candidate when it returns non-zero.  A rejected
candidate does not occupy depth.  Passing `NULL` keeps every candidate.

The resolve shader

```c
vec4_t visibility_shader_t(const void *varyings, void *uniforms,
                           uint32_t primitive_id, int backface);
```

is called exactly once per covered pixel with the stored original-vertex
varyings recombined by the stored barycentric weights.  There is no
discard.  Colors are saturated and converted to bytes the same way as the
framebuffer pipeline, the retained depth is written, and the target's
alpha byte and all background pixels are left untouched.  A target whose
dimensions differ from the buffer is rejected before any write.
`visibility_resolve` never re-runs geometry or coverage.

Build and run the tests:

```
bash task_support/run.sh target
```

This compiles the production visibility source and `renderer/tests/visibility_target.c`,
runs the `capture`, `replay` and `compat` groups, and then re-runs the
existing clipped/masked baseline.  The replay group also writes
`.task-build/visibility_palette_a.ppm` and `.task-build/visibility_palette_b.ppm`
(two color schemes of the same generated intersecting triangles) and checks
their pixels.

### Launch

If the software renderer is launched without arguments, one of the available
scenes will be chosen randomly. To display a specific scene (see below),
additional arguments should be supplied. The command line syntax is:

```
Viewer [test_name [scene_name]]
```

### Controls

* Orbit: left mouse button
* Pan: right mouse button
* Zoom: mouse wheel
* Rotate lighting: <kbd>A</kbd> <kbd>D</kbd> <kbd>S</kbd> <kbd>W</kbd>
* Reset everything: <kbd>Space</kbd>

### Inspector

For PBR scenes, a material inspector that is very similar to the layers view
of [Marmoset Viewer](https://marmoset.co/viewer/) is provided. Double click
to bring it up.

## Screenshots

| Scene                                                                                   | Command                   |
| --------------------------------------------------------------------------------------- | ------------------------- |
| [<img src="assets/azura/screenshot.png" width="600">](assets/azura/README.md)           | `Viewer blinn azura`      |
| [<img src="assets/centaur/screenshot.png" width="600">](assets/centaur/README.md)       | `Viewer blinn centaur`    |
| [<img src="assets/craftsman/screenshot2.gif" width="600">](assets/craftsman/README.md)  | `Viewer blinn craftsman`  |
| [<img src="assets/elfgirl/screenshot2.png" width="600">](assets/elfgirl/README.md)      | `Viewer blinn elfgirl`    |
| [<img src="assets/kgirl/screenshot.gif" width="600">](assets/kgirl/README.md)           | `Viewer blinn kgirl`      |
| [<img src="assets/lighthouse/screenshot.png" width="600">](assets/lighthouse/README.md) | `Viewer blinn lighthouse` |
| [<img src="assets/mccree/screenshot.png" width="600">](assets/mccree/README.md)         | `Viewer blinn mccree`     |
| [<img src="assets/nier2b/screenshot.png" width="600">](assets/nier2b/README.md)         | `Viewer blinn nier2b`     |
| [<img src="assets/phoenix/screenshot.gif" width="600">](assets/phoenix/README.md)       | `Viewer blinn phoenix`    |
| [<img src="assets/vivi/screenshot.png" width="600">](assets/vivi/README.md)             | `Viewer blinn vivi`       |
| [<img src="assets/whip/screenshot.gif" width="600">](assets/whip/README.md)             | `Viewer blinn whip`       |
| [<img src="assets/witch/screenshot.png" width="600">](assets/witch/README.md)           | `Viewer blinn witch`      |
| [<img src="assets/assassin/screenshot.gif" width="600">](assets/assassin/README.md)     | `Viewer pbr assassin`     |
| [<img src="assets/buster/screenshot1.gif" width="600">](assets/buster/README.md)        | `Viewer pbr buster`       |
| [<img src="assets/crab/screenshot.gif" width="600">](assets/crab/README.md)             | `Viewer pbr crab`         |
| [<img src="assets/dieselpunk/screenshot.png" width="600">](assets/dieselpunk/README.md) | `Viewer pbr dieselpunk`   |
| [<img src="assets/drone/screenshot1.gif" width="600">](assets/drone/README.md)          | `Viewer pbr drone`        |
| [<img src="assets/helmet/screenshot1.png" width="600">](assets/helmet/README.md)        | `Viewer pbr helmet`       |
| [<img src="assets/horse/screenshot.gif" width="600">](assets/horse/README.md)           | `Viewer pbr horse`        |
| [<img src="assets/junkrat/screenshot.gif" width="600">](assets/junkrat/README.md)       | `Viewer pbr junkrat`      |
| [<img src="assets/ornitier/screenshot.png" width="600">](assets/ornitier/README.md)     | `Viewer pbr ornitier`     |
| [<img src="assets/ponycar/screenshot2.png" width="600">](assets/ponycar/README.md)      | `Viewer pbr ponycar`      |
| [<img src="assets/robot/screenshot.png" width="600">](assets/robot/README.md)           | `Viewer pbr robot`        |
| [<img src="assets/spruit/screenshot1.png" width="600">](assets/spruit/README.md)        | `Viewer pbr sphere`       |
| [<img src="assets/workshop/screenshot2.png" width="600">](assets/workshop/README.md)    | `Viewer pbr spheres`      |

## References

### Rendering pipeline

* [Tiny Renderer](https://github.com/ssloy/tinyrenderer)
* [OpenGL ES 2.0 Specification](https://www.khronos.org/registry/OpenGL/specs/es/2.0/es_full_spec_2.0.pdf)

### Physically based rendering

* [glTF 2.0 Sample Viewer](https://github.com/KhronosGroup/glTF-Sample-Viewer)
* [SIGGRAPH 2013 Shading Course](https://blog.selfshadow.com/publications/s2013-shading-course/)
* [Physically Based Rendering in Filament](https://google.github.io/filament/Filament.html)

### File formats

* [Truevision TGA](http://www.dca.fee.unicamp.br/~martino/disciplinas/ea978/tgaffs.pdf)
* [Radiance HDR](https://www.graphics.cornell.edu/~bjw/rgbe.html)
* [Wavefront OBJ](http://www.cs.utah.edu/~boulos/cs3505/obj_spec.pdf)
* [Khronos glTF](https://github.com/KhronosGroup/glTF/blob/master/specification/2.0/README.md)

### Miscellaneous

* [Homogeneous clipping](http://fabiensanglard.net/polygon_codec/)
* [Barycentric coordinates](http://blackpawn.com/texts/pointinpoly/)
* [Perspective correct interpolation](https://www.comp.nus.edu.sg/~lowkl/publications/lowk_persp_interp_techrep.pdf)
* [Skeletal animation](https://people.rennes.inria.fr/Ludovic.Hoyet/teaching/IMO/05_IMO2016_Skinning.pdf)
* [Tangent space normal mapping](http://www.terathon.com/code/tangent.html)
* [Ambient occlusion](http://mentalraytips.blogspot.com/2008/11/joy-of-little-ambience.html)
* [ACES tone mapping](https://knarkowicz.wordpress.com/2016/01/06/aces-filmic-tone-mapping-curve/)
* [Orbital camera controls](https://github.com/mrdoob/three.js/blob/master/examples/js/controls/OrbitControls.js)
* [Xlib programming guide](https://web.archive.org/web/20071018025425/http://users.actcom.co.il/~choo/lupg/tutorials/xlib-programming/xlib-programming.html)
* [Type-safe dynamic array](https://github.com/nothings/stb/blob/master/stretchy_buffer.h)

## License

[MIT License](LICENSE)
