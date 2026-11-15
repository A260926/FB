# Third-Party Notices

This project bundles third-party components. They are vendored under
`third_party/` and retain their original licenses.

## OpenGL Mathematics (GLM)

- Homepage: https://github.com/g-truc/glm
- License: The Happy Bunny License (Modified MIT) / MIT License — see
  `third_party/glm/copying.txt`
- Copyright (c) 2005 – G-Truc Creation

GLM is a header-only mathematics library for graphics software. By default the
build fetches the latest GLM from upstream (CMake FetchContent), so no version
is pinned. A trimmed offline copy (the `glm/` header tree + its license) is
vendored under `third_party/glm/` as a fallback, used when
`-DFETCH_DEPENDENCIES=OFF` or for non-CMake builds; the upstream `cmake/`,
`doc/`, `test/`, `util/` directories and the C++20 module (`glm.cppm`) are
intentionally omitted from the vendored copy.

## "sdw" teaching framework

- Location: `framework/`
- Origin: University of Bristol, Computer Graphics unit (COMS30020)

The `DrawingWindow`, `CanvasPoint`, `CanvasTriangle`, `Colour`,
`ModelTriangle`, `TextureMap`, `TexturePoint`, and `Utils` classes are
based on the framework distributed with the unit for building a software
renderer on top of SDL.

## SDL3

Linked at build time (not vendored). https://www.libsdl.org — zlib license.
Resolved via CMake `find_package(SDL3 CONFIG)` / the `SDL3::SDL3` target, or
`pkg-config sdl3` for the Makefile build.

## stb_image_write

- Location: `third_party/stb/stb_image_write.h`
- Origin: https://github.com/nothings/stb (v1.16), by Sean Barrett.

Single-header PNG/image writer, public domain (or MIT, at your option). Vendored
and used by `Canvas::savePNG` and the C ABI `rn_save_png`.

## Vulkan

Only for the optional Vulkan compute path tracer (BUILD_VULKAN). https://www.vulkan.org
Resolved find-first-then-fetch: `find_package(Vulkan)` if an SDK is present (links
`Vulkan::Vulkan`), otherwise the Khronos Vulkan-Headers are fetched
(https://github.com/KhronosGroup/Vulkan-Headers, Apache-2.0 / MIT) and the
driver's loader is used at runtime. Vulkan and the Vulkan logo are trademarks of
the Khronos Group.
