Blend2D
-------

2D Vector Graphics Powered by a JIT Compiler.

  * [Official Home Page (blend2d.com)](https://blend2d.com)
  * [Official Repository (blend2d/blend2d)](https://github.com/blend2d/blend2d)
  * [Public Chat Channel](https://app.element.io/#/room/#blend2d:matrix.org)
  * [Zlib License](./LICENSE.md)

See [blend2d.com](https://blend2d.com) page for more details.

Project Organization
--------------------

  * **`/`**                - Project root, contains this README.md, other markdown files, CMakeLists.txt, and scripts for developers
    * **src**              - Source code (always point include path in here)
      * **blend2d**        - Blend2D source code and headers, all core classes and public headers are here
        * **codec**        - Image codecs and codec utilities (BMP, JPEG, PNG, and QOI)
        * **compression**  - Implementation of compression and decompression algorithms (DEFLATE)
        * **opentype**     - TrueType and OpenType font support
        * **pipeline**     - 2D rendering pipeline definitions and implementations (reference and JIT)
        * **pixelops**     - Pixel operations helpers
        * **raster**       - 2D rendering context implementation
        * **simd**         - SIMD abstraction library Blend2D uses
        * **support**      - Support classes and utilities (arena allocator & containers, bit operations, string operations, ...)
        * **simd**         - SIMD abstraction library Blend2D uses
        * **tables**       - Lookup tables and SIMD constants
        * **threading**    - Multithreading abstraction (POSIX threads or Windows threads)
        * **unicode**      - Support for unicode and conversion between various unicode encodings
    * **testing**          - Testing, benchmarking, samples, and interactive demos (don't embed in your project)
    * **3rdparty**         - Place where to clone [asmjit](https://github.com/asmjit/asmjit) to enable JIT code generation or where to add dependencies for testing and benchmarking purposes

Roadmap
-------

  * See [Roadmap](https://asmjit.com/roadmap.html) page for more details

Documentation
-------------

  * [Documentation Index](https://blend2d.com/doc/index.html)

Contributing Guidelines
-----------------------

  * [CONTRIBUTING.md](./CONTRIBUTING.md)

Building
--------

Blend2D is almost self-containing library. The only dependency is AsmJit, which is only required by when JIT is enabled.

To fetch both Blend2D and AsmJit, type:

```bash
$ git clone https://github.com/blend2d/blend2d
$ git clone https://github.com/asmjit/asmjit blend2d/3rdparty/asmjit
```

Then you can simply configure and build blend2d with CMake:

```bash
$ cmake -S blend2d -B blend2d/build/Release -DCMAKE_BUILD_TYPE=Release -DBLEND2D_TEST=ON
$ cmake --build blend2d/build/Release --config Release
```

And run samples:

```
$ blend2d/build/Release/bl_sample_1
$ blend2d/build/Release/bl_sample_2
...
```

To build Blend2D with interactive demos and with benchmark targeting Qt and other libraries, use `BLEND2D_DEMOS=ON`

```bash
$ cmake -S blend2d -B blend2d/build/Release -DCMAKE_BUILD_TYPE=Release -DBLEND2D_TEST=ON -DBLEND2D_DEMOS=ON
$ cmake --build blend2d/build/Release --config Release
```

Please note that to build interactive demos and benchmarking backends, you need to have Qt installed, in addition to all rendering libraries you would like to benchmark.

For more information, visit [https://blend2d.com/doc/build-instructions.html](Build Instructions)

Bindings
--------

  * [Download Page](https://blend2d.com/download.html#Bindings) provides a list of Blend2D bindings

License
-------

  * [LICENSE.md](./LICENSE.md)

Transformed A8 Mask Fills (`fill_mask`)
---------------------------------------

`BLContext::fill_mask()` paints the current fill style (or an explicit `BLRgba32`, `BLGradient`, or
`BLPattern`) through an `BL_FORMAT_A8` coverage image. In addition to the original pixel-aligned path, the
raster engine now accepts fractional origins and arbitrary affine context transforms (translation, rotation,
reflection, shear, and non-uniform scaling) on a `BL_FORMAT_PRGB32` target.

  * **Selection origin.** When a `mask_area` is given, it selects a sub-rectangle of the mask image. The
    top-left corner of that selection - mask coordinate `(mask_area.x, mask_area.y)`, not the `(0, 0)` corner
    of the whole mask - is what gets mapped to the `origin` argument through the context's final transform.
    Pixels outside the selection are treated as coverage zero; nonzero pixels sitting next to the selection are
    never read.
  * **Sampling and transparent boundaries.** Each device pixel center is inverse-mapped through the complete
    final transform into mask coordinates. Mask pixel centers are at `(i + 0.5, j + 0.5)`, and coverage comes
    from bilinear interpolation. The bilinear footprint may cross the selection boundary: the out-of-selection
    contribution is zero, the footprint is never clamped to the edge, and the last selected pixel is never
    repeated. The input matrix is required to be finite and invertible.
  * **Brush coordinates stay independent.** The mask is positioned with `origin` and the context transform,
    while gradients and patterns keep their own matrices combined with the context transform in the usual way.
    No mask-area offset or `origin` is ever added to the brush. The rectangular clip, global alpha and fill
    alpha still apply, and mask coverage is multiplied exactly once. `SRC_OVER` and `SRC_COPY` are supported;
    under partial coverage `SRC_COPY` interpolates between the old target and the source instead of writing a
    translucent source that would erase the rest of the target.
  * **Asynchronous data ownership.** On an asynchronous context the mask image is retained through the
    library's copy-on-write machinery when a render command is queued. After queueing, the caller may release
    the source `BLImage`, mutate its shared image (which detaches via copy-on-write), replace the brush, or
    change the transform; flushed pixels correspond to the source content and state captured at each call.
    Queue-owned scratch data is released when the render batch is finalized. Synchronous contexts and
    `thread_count=2` asynchronous contexts produce byte-identical output, and operations spanning multiple
    render bands keep their enqueue order.
  * Empty selections and masks fully removed by the clip succeed without writing any pixels; invalid
    `mask_area` values continue to return `BL_ERROR_INVALID_VALUE`. The public API and enumerations are
    unchanged.

See `examples/affine_mask.cpp` for a self-contained demo that generates an A8 mask procedurally and writes a
PPM image, and `task_tests/target.cpp` for the `sampling`, `styles`, `deferred`, and `compatibility` test
groups (run via `bash task_support/target.sh`).
