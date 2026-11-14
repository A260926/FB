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

Transformed Mask Fills (`fill_mask`)
------------------------------------

`BLContext::fill_mask()` paints an A8 coverage mask through the current fill style. In addition to the legacy
pixel-aligned fast path, masks can now be positioned through the context's full final affine transform:
fractional origins, rotation/reflection, shear and non-uniform scale are all supported on PRGB32 targets.

Key semantics:

  * **Selection origin.** When a `mask_area` is supplied, the `origin` argument maps the **top-left corner of the
    selected rectangle**, not the `(0, 0)` corner of the whole mask image. Pixels outside the selection are treated
    as zero coverage; non-zero pixels adjacent to the selection are never read.
  * **Transparent boundary.** Coverage is computed per device-pixel center by inverting the final transform and
    bilinearly interpolating the A8 source, whose pixel centers sit at half-integer coordinates
    (`i + 0.5`, `j + 0.5`). The interpolation footprint is allowed to cross the selection boundary: taps outside
    contribute zero, the edge is never clamped, and the last valid pixel is not repeated or implicitly extended.
  * **Two independent coordinate spaces.** The mask follows `origin` and the context transform, while a gradient or
    pattern keeps its own matrix combined with the context transform. The mask origin / `mask_area` offset is never
    added to the brush.
  * **Coverage and compositing.** Global alpha and fill alpha continue to apply and are folded into the coverage a
    single time. `SRC_OVER` and `SRC_COPY` are supported; under partial coverage `SRC_COPY` interpolates between the
    old destination and the source instead of erasing the rest of the target.
  * **Asynchronous ownership.** On an asynchronous context the queued command retains the mask image implementation
    and all required style data. After a call returns you may release the source `BLImage`, mutate a shared image
    (which triggers copy-on-write), replace the brush, or change the transform; the pixels produced after `flush()`
    correspond to the source content and state captured at call time. No target-sized temporary image or forced
    flush is used, and asynchronous contexts are never silently switched to synchronous execution.

A self-contained demo that procedurally builds an A8 mask and writes a PPM image lives in
[`examples/affine_mask.cpp`](./examples/affine_mask.cpp). The task test suite in `task_tests/target.cpp`
(groups `sampling`, `styles`, `deferred`, `compatibility`) is built and run by `task_support/target.sh`.

License
-------

  * [LICENSE.md](./LICENSE.md)
