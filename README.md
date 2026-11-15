# scimesh

Scimesh is a 3D renderer built for scientific mesh visualization, delivering publication-quality imagery entirely on the CPU.


Scientific compute environments are often constrained by the absence of graphical interfaces, specialized hardware, or complex driver stacks. Scimesh eliminates these bottlenecks by providing a zero-dependency, headless rendering pipeline—no OpenGL, no X11, and no GPU drivers required. Whether you are running complex visualization scripts on HPC clusters, inside isolated containers, or even on a modern macOS laptop where missing support for X11 makes traditional tools like rgl a challenge, Scimesh works reliably wherever your C++17 or R code runs.


<!-- badges: start -->
  [![DOI](https://zenodo.org/badge/DOI/10.5281/zenodo.21374840.svg)](https://doi.org/10.5281/zenodo.21374840)
  [![R-CMD-check](https://github.com/dfsp-spirit/scimesh/actions/workflows/R-CMD-check.yaml/badge.svg)](https://github.com/dfsp-spirit/scimesh/actions/workflows/R-CMD-check.yaml)
  [![tests_cpp](https://github.com/dfsp-spirit/scimesh/actions/workflows/tests_cpp.yml/badge.svg?branch=main)](https://github.com/dfsp-spirit/scimesh/actions)
  [![tests_R](https://github.com/dfsp-spirit/scimesh/actions/workflows/tests_R.yml/badge.svg?branch=main)](https://github.com/dfsp-spirit/scimesh/actions)
  [![docs](https://github.com/dfsp-spirit/scimesh/actions/workflows/docs.yml/badge.svg?branch=main)](https://github.com/dfsp-spirit/scimesh/actions)
<!-- badges: end -->

## Visualization Examples

![Stanford Dragon rendered with scimesh](web/images/dragon.png)

*The [Stanford Dragon](https://graphics.stanford.edu/data/3Dscanrep/) (871k triangles), rendered with scimesh's
software rasterizer using smooth shading, three-point Blinn-Phong
lighting, and 4x anti-aliasing. Render time 5.2 seconds on the author's mini PC (AMD Ryzen 7 8745H CPU). Source code:
[R](examples/R/dragon/run.R) | [C++](examples/cpp/dragon/main.cpp)*


![FreeSurfer reconstruction of human brain rendered with scimesh](web/images/whole_brain_sulc.png)

*A FreeSurfer reconstruction of a human brain surface based on an MRI scan (300k triangles), rendered with scimesh's
software rasterizer using smooth shading, no anti-aliasing. Render time 1.8 seconds on the author's mini PC. Source code:
[R](examples/R/whole_brain_sulc/run.R) | [C++](examples/cpp/whole_brain_sulc/main.cpp)*

## What scimesh is

- A **headless software renderer** — renders 3D triangle meshes to
  images without any GPU or display server. This means you could use scimesh for non-interactive rendering
  when rgl/OpenGL is unavailable.
- Designed for **scientific mesh visualization** (neuroimaging, molecular structures, geometric primitives).
- **Beautiful** enough for publication figures, and still **fast** enough for batch processing on headless servers: all rendering is done in optimized C++, optionally with OpenMP parallelization.

## Features

### C++ layer

- Software rasterization with z-buffer, smooth and flat shading
- Multi-light Blinn-Phong illumination with specular highlights
- Anti-aliasing via ordered-grid supersampling (2x, 4x SSAA)
- Semi-transparent mesh overlays with depth-sorted alpha blending
- Wireframe rendering with adaptive edge thickness
- Orthographic and perspective projection
- Screen-space ambient occlusion (SSAO)
- Depth cueing (fog)
- Clip planes
- Procedural geometry: spheres, cylinders, cuboids, pyramids, tetrahedra, tori, planes
- Batched primitives: thousands of spheres/cylinders as a single mesh
- Polyline tubes with rotation-minimizing frames (curved edges, streamlines)
- Scene line layers: pixel-wide lines without geometry (wireframes, graph edges)
- Text labels: anti-aliased TrueType glyphs anchored in world space (labels that
  follow the camera and can be hidden by the geometry) or in screen space
  (titles, panel tags, captions), with optional halos, in a bundled font
- Mesh I/O: STL (binary/ASCII), Wavefront OBJ, Stanford PLY
- Image I/O: PNG, PPM, BMP
- Automatic camera framing (`camera_fit_mesh`, `camera_fit_scene`)
- Per-vertex and per-face coloring (vertex colors, face colors)
- Texture mapping with bilinear sampling
- Paged (streamed) texture rendering: LRU page cache, explicit per-page
  requests/delivery and incremental `step()` jobs, byte-identical to the
  dense renderer
- Basic mesh transform utilities: translate, scale, rotate, arbitrary 4x4 matrices
- Basic image composition: cropping, stacking

### R Layer

The R layer gives access to all C++ layer features, and adds on top:

- Easy, high-level access from R
- More advanced image composition, like grids
- Colorbars (combine with your main plot with image composition!)
- Session-wide render defaults, e.g. `options(scimesh.aa_samples = 2)` to
  anti-alias every render call


If you need colorbars in C++, have a look at [scibar](https://github.com/dfsp-spirit/scibar).

See the [R vignette](vignettes/scimesh.Rmd) for usage details (anti-aliasing,
text labels, compositing, and more).


## What scimesh is not

- Not a plotting framework — use ggplot2, lattice, or plotly for
  statistical plots.  scimesh renders 3D geometry.
- Not a hardware renderer — no OpenGL, Vulkan, or Metal.  For
  interactive 3D rotation, use rgl. In general, real-time rendering requires hardware support (a graphics card and the full associated software stack).
- Not a system for physical based rendering — no PBR materials, no raytracing, etc.
- Not a mesh editing/manipulation framework — it reads, transforms,
  and renders meshes but does not provide interactive editing, mesh
  repair, remeshing, or boolean operations.
  For mesh repair and remeshing in R, see the
  [Rvcg](https://cran.r-project.org/package=Rvcg) package (wraps VCGLib).


## Installation

### R package

```r
# Install from GitHub
remotes::install_github("dfsp-spirit/scimesh")

# For FreeSurfer neuroimaging data
remotes::install_github("dfsp-spirit/freesurferformats")
```

### C++

#### Via CMake FetchContent (recommended)

```cmake
include(FetchContent)
FetchContent_Declare(
    scimesh
    GIT_REPOSITORY https://github.com/dfsp-spirit/scimesh.git
    GIT_TAG        main          # or a release tag, e.g. v0.2.8
)
FetchContent_MakeAvailable(scimesh)

add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE scimesh)
```

That's it — no manual source listing, include paths, or preprocessor
defines needed.

#### Manually (header + source)

The C++ core lives in `src/core/`.  To use scimesh in a C++ project
without CMake, add the source files and include paths to your build
system.  See [`CPP_GETTING_STARTED.md`](CPP_GETTING_STARTED.md) for details.

## Quick Start

### R

```r
library(scimesh)

sphere <- generate_sphere(c(0, 0, 0), radius = 1.2,
                          segments = 32, color = c(0.9, 0.3, 0.2, 1.0))
cam <- camera_auto(sphere, direction = c(1.2, 0.8, 1))

# Flat-shaded sphere
img_shaded <- render_mesh(sphere$vertices, sphere$triangles,
    colors = sphere$colors, camera = cam,
    options = render_options(
        background_color = c(1, 1, 1, 1),
        lights = list(
            list(position = c(0.5, 1.0, 0.8), intensity = 1.5),
            list(position = c(-0.5, 0.2, 0.6), intensity = 0.5))))

# Wireframe sphere
img_wire <- render_mesh(sphere$vertices, sphere$triangles,
    colors = sphere$colors, camera = cam,
    options = render_options(wireframe = TRUE,
        wireframe_color = c(0, 0, 0, 1),
        background_color = c(1, 1, 1, 1),
        backface_culling = FALSE))

# Side-by-side
img <- stack_horizontal(img_shaded, img_wire)
write_png(img, "sphere.png")
```


### C++

## Paged texture rendering (`PagedCache` / `PagedRenderJob`)

When texture data arrives a page at a time — for example streamed over the
network — the renderer never needs the whole texture resident. Two classes in
`<scimesh/paged_texture.h>` and `<scimesh/paged_renderer.h>` add an
incremental, page-driven render path that reuses the existing model
transforms, clipping, coverage rules, material lighting and depth
submission, and produces the same RGBA bytes as `Renderer::render_scene()`
for the same inputs.

### Public interface

- `PagedCache(capacity)` — an LRU cache of whole texture pages (capacity
  `N >= 1`, counted in pages regardless of edge-page size). It never stores a
  complete texture and keeps no copy of an evicted page.
  - `publish(TextureDescriptor{width, height, page_size, logical_id})`
    returns an immutable, non-zero **version** handle. Re-publishing the same
    logical id yields a distinct version; pages of different versions are
    never mixed.
  - `request_page(version, page_x, page_y, waiter)` returns `0` on a cache
    hit (and refreshes LRU order) or a unique **ticket** for a new, shared
    in-flight request. Multiple jobs waiting on the same version/page share
    one request and ticket.
  - `supply(ticket, width, height, bytes, byte_count)` delivers row-major
    RGBA8 page data. Mismatched ticket, dimensions or byte count — including
    null data — are rejected without changing state, and completing a ticket
    twice has no second effect. Deliveries may arrive in any order.
  - Observability: `pending_requests()`, `resident_page_count()`,
    `capacity()`, `descriptor()`, `page_rect()`, plus `release(version)` and
    `detach_waiter()` for lifecycle management.
- `PagedRenderJob(cache, scene, camera, options, bindings, K)` copies every
  geometric input and setting it needs and pins the named texture versions,
  so the caller may mutate or free the `Scene`, `Camera` and `RenderOptions`
  afterwards. `bindings` maps a mesh index to a texture version. `K >= 1` is
  the maximum number of fragments parked waiting on pages at once.
  - `step(budget)` advances the render by at most `budget` **raster steps**
    and returns a `PagedJobStatus`: `READY`, `WAITING`, `COMPLETE`, `FAILED`
    or `CANCELLED`. Examining one pixel of a triangle bounding box is one
    step; resuming one parked fragment is one step. A budget of 0 never
    advances the job.
  - `cancel()` withdraws the job's cache waiters immediately; shared
    requests keep serving other jobs, and a late response cannot revive it.
  - Observability: `outstanding_requests()`, `pending_fragment_count()`,
    `submission_count()`, `total_steps()`, `status()`, and `image()` /
    `take_image()` once `COMPLETE`.
- A parked fragment stores only its own (at most four) bilinear texels and
  weights. Sampling follows `Image::sample_bilinear()` exactly — the same
  clamp, `u*(width-1)` / `v*(height-1)` coordinates and arithmetic — and
  zero-weight taps are never requested. Texture alpha must be 255; any other
  value fails the job.

### What a step means and how delivery works

A fragment whose taps are not all resident is parked *before* any color,
depth or normal write and asks the cache for the missing pages. When a page
is supplied, the cache hands its texels to every waiting fragment at once;
even if that page is evicted before the job's next `step()`, the delivered
taps survive inside the fragments, so a capacity-1 cache never re-requests a
page forever. When the pending limit `K` is reached and no work can
continue, `step()` returns `WAITING`; the caller then supplies pages and
steps again. Parked fragments resume in traversal order, and equal-depth
surfaces keep the original submission order, so the final image is
byte-identical to the dense renderer.

### Resource ownership

- The **cache owns** resident page bytes and in-flight request bookkeeping.
- Each **job owns** its copied scene/camera/options, the output `Image`, the
  depth/normal buffers and its parked fragments; it holds only a non-owning
  reference to the cache, which must outlive it. Jobs detach their waiters in
  their destructor (and on cancel/failure), leaving no dangling waiters.
- A **completed `Image` owns its pixels**; cache evictions, texture version
  release and job destruction never alter it.
- The first version supports opaque triangle meshes, single sampling and
  single-threaded rendering only; lines, points, text, blending, SSAO and any
  anti-aliasing sample count other than 1, along with non-255 texture alpha,
  are rejected up front with `std::invalid_argument`.

A complete runnable program is in
[`examples/paged_material.cpp`](examples/paged_material.cpp): it generates a
mesh and texture pages in C++, renders with a capacity-1 cache, and writes
`paged.ppm` plus `residency.txt` into an output directory.

## Documentation

- **R vignette**: `vignettes/scimesh.Rmd` — comprehensive guide to the
  R package with examples for all major features.
- **R examples**: [`examples/R/`](examples/R/) — runnable scripts
  demonstrating textured rendering, transparency, and neuroimaging
  visualization.
- **R live help**: Help for a specific function can be accessed in the usual R manner: `?<function>`, where you replace `<function>` with a function name. Like this: `?read_ply`.
- **R live demos**: Run `example(<function>)` to see a live demo that uses the function `<function>`. Like this: `example(read_ply)`.
- **R API Documentation**: [dfsp-spirit.github.io/scimesh/r](https://dfsp-spirit.github.io/scimesh/r)
- **C++ getting started**: [`cpp_getting_started.html`](https://dfsp-spirit.github.io/scimesh/cpp/cpp_getting_started.html)
  — gentle introduction to the C++ renderer.
  \subpage cpp_getting_started
- **C++ examples**: [`examples/cpp/`](examples/cpp/) — standalone
  programs covering textured OBJ, transparency, protein visualization,
  and whole-brain rendering.
- **C++ API Documentation**: [dfsp-spirit.github.io/scimesh/cpp](https://dfsp-spirit.github.io/scimesh/cpp)
- **Tests:** Some of the [unit tests for cpp](./cpp_tests/) and [for R]((./tests/testthat/)) that come with this package are essentially examples that illustrate how to use the functions.


### Direct Links to Example Program Source Code

| Example | Language | Description |
|---------|----------|-------------|
| [`examples/R/spot_cow/`](examples/R/spot_cow/) | R | Textured OBJ mesh (Spot cow) with multi-light setup |
| [`examples/R/transparency/`](examples/R/transparency/) | R | Semi-transparent pial overlay on white matter |
| [`examples/R/primitives/`](examples/R/primitives/) | R | All geometric primitives demo |
| [`examples/R/dragon/`](examples/R/dragon/) | R | Stanford Dragon with SSAO, specular, and 4x AA |
| [`examples/R/whole_brain_sulc/`](examples/R/whole_brain_sulc/) | R | Whole-brain sulcal depth rendering |
| [`examples/R/video_frames_orbit/`](examples/R/video_frames_orbit/) | R | Turntable video from orbit frames |
| [`examples/cpp/spot_cow/`](examples/cpp/spot_cow/) | C++ | Textured OBJ rendering with bilinear sampling |
| [`examples/cpp/bunny/`](examples/cpp/bunny/) | C++ | High-quality Stanford Bunny with SSAO, specular, and 4x AA |
| [`examples/cpp/dragon/`](examples/cpp/dragon/) | C++ | Stanford Dragon with SSAO, specular, and 4x AA |
| [`examples/cpp/all_primitives/`](examples/cpp/all_primitives/) | C++ | All geometric primitives demo |
| [`examples/cpp/transparency/`](examples/cpp/transparency/) | C++ | Multi-mesh alpha blending with FreeSurfer data |
| [`examples/cpp/protein_data_bank_pdb_file/`](examples/cpp/protein_data_bank_pdb_file/) | C++ | Protein ball-and-stick visualization from PDB files |
| [`examples/cpp/whole_brain_sulc/`](examples/cpp/whole_brain_sulc/) | C++ | Whole-brain sulcal depth rendering |
| [`examples/cpp/whole_brain_sulc_fsaverage/`](examples/cpp/whole_brain_sulc_fsaverage/) | C++ | Same on fsaverage template |
| [`examples/cpp/whole_brain_annot/`](examples/cpp/whole_brain_annot/) | C++ | Cortical parcellation visualization |
| [`examples/cpp/brain_video/`](examples/cpp/brain_video/) | C++ | Turntable brain animation via camera_orbit() |
| [`examples/cpp/full_CLI_renderer/`](examples/cpp/full_CLI_renderer/) | C++ | Self-contained CLI renderer with TOML config + CLI overrides |


## Acknowledgements

Thanks heaps to the authors of these great software packages that scimesh is built upon:

- `cpp_tests/catch_amalgamated.{h,cpp}`: [catchorg/Catch2](https://github.com/catchorg/Catch2/tree/devel/extras) — C++ test framework by Martin Hořeňovský
- `src/third_party/glm/`: [g-truc/glm](https://github.com/g-truc/glm) — header-only C++ math library.
- `src/third_party/tinyply.{h,cpp}`: [ddiakopoulos/tinyply](https://github.com/ddiakopoulos/tinyply) — PLY file reader by Dimitri Diakopoulos
- `src/third_party/libfs.h`: [dfsp-spirit/libfs](https://github.com/dfsp-spirit/libfs) — FreeSurfer file format reader + OBJ mesh loader by Tim Schäfer
- `src/third_party/stl_reader.h`: [sreiter/stl_reader](https://github.com/sreiter/stl_reader) — STL file reader, maintained by Sebastian Reiter
- `src/third_party/stb_image.h`, `src/third_party/stb_image_write.h`: [nothings/stb](https://github.com/nothings/stb) — image loading/saving, maintained by Sean Barrett

And of course, thanks to the authors of the dependencies of these packages, and their dependencies...

Detailed information on the licenses for all these packages (all MIT-compatible) can be found in the file [THIRD_PARTY_LICENSES.md](./src/third_party/THIRD_PARTY_LICENSES.md).

*Note: This section is to give credit only. Users do **not** need to worry about installing these dependencies, they come vendored with scimesh already.*


## Developer Information

Please refer to [README_DEVELOPMENT.md](./README_DEVELOPMENT.md).


## License and Author

License: [MIT](./LICENSE)

Author: [Tim Schäfer](https://ts.rcmd.org/)

