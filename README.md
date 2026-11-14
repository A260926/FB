Ray Tracing in One Weekend Book Series
====================================================================================================

| ![RT in One Weekend][cover1] | ![RT The Next Week][cover2] | ![RT The Rest of Your Life][cover3] |
|:----------------------------:|:---------------------------:|:-----------------------------------:|
|   [In One Weekend][book1]    |   [The Next Week][book2]    |   [The Rest of Your Life][book3]    |


Getting the Books
------------------
The _Ray Tracing in One Weekend_ series of books are now available to the public for free directly
from the web.

### Version 4.0.1

  - [Ray Tracing in One Weekend][web1]
  - [Ray Tracing: The Next Week][web2]
  - [Ray Tracing: The Rest of Your Life][web3]

These books have been formatted for both screen and print. For more information about printing your
own copies, or on getting PDFs of the books, see [PRINTING.md][] for more information.


Contributing
-------------
If you'd like to contribute a PR _**please read our [contribution guidelines][CONTRIBUTING]
first**_.


Project Status
---------------
If you'd like to check out the latest updates and watch our progress, we're on the `dev-patch`,
`dev-minor`, and `dev-major` branches. You can also browse our issues and milestones to see what
we're planning.

If you're interested in contributing, email us! You can find our contact info at the head of each
book. Or just start [a new discussion][discussions] or [issue][issues].


GitHub Discussions
------------------
Do you have general questions about raytracing code, issues with your own implmentation, or general
raytracing ideas you'd like to share? Check out our [GitHub discussions][discussions] forum!


Directory Structure
-------------------
The organization of this repository is meant to be simple and self-evident at a glance:

  - `books/` --
    This folder contains the three raytracing books (in HTML), and some supporting material.

  - `images/` --
    Contains all of the images and figures of the books. Can also be used to compare your
    results.

  - `style/` --
    Contains the css for the books and the site.

  - `src/` --
    Contains the source.

  - `src/<book>/` --
    Contains the final source code for each book.


Source Code
-----------
### Intent
This repository is not meant to act as its own tutorial. The source is provided so you can compare
your work when progressing through the book. We strongly recommend reading and following along with
the book to understand the source. Ideally, you'll be developing your own implementation as you go,
in order to deeply understand how a raytracer works.

### Downloading The Source Code
The [GitHub home][] for this project contains all source and documentation associated with the _Ray
Tracing in One Weekend_ book series. To clone or download the source code, see the green "Clone or
download" button in the upper right of the project home page.

### Programming Language
This book is written in C++, and uses some modern features of C++11. The language and features were
chosen to be broadly understood by the largest collection of programmers. It is not meant to
represent ideal (or optimized) C++ code.

### Implementations in Other Languages
The _Ray Tracing in One Weekend_ series has a long history of implementations in other programming
languages (see [Implementations in Other Languages][implementations]), and across different
operating systems. Feel free to add your own implementation to the list!

### Branches
In general, ongoing development, with all of the latest changes, can be found in the `dev-patch`,
`dev-minor`, and `dev-major` branches, minor and major changes, depending on the change level and
release in progress. We try to keep CHANGELOG.md up to date, so you can easily browse what's new in
each development branch. We may from time to time use additional development branches, so stay up to
date by reviewing the [CONTRIBUTING][] page.

The `release` branch contains the latest released (and live) assets. This is the branch from which
GitHub pages serves up https://raytracing.github.io/.


Building and Running
---------------------
Copies of the source are provided for you to check your work and compare against. If you wish to
build the provided source, this project uses CMake. To build, go to the root of the project
directory and run the following commands to create the debug version of every executable:

    $ cmake -B build
    $ cmake --build build

You should run `cmake -B build` whenever you change your project `CMakeLists.txt` file (like when
adding a new source file).

You can specify the target with the `--target <program>` option, where the program may be
`inOneWeekend`, `theNextWeek`, `theRestOfYourLife`, or any of the demonstration programs. By default
(with no `--target` option), CMake will build all targets.

    $ cmake --build build --target inOneWeekend

### Optimized Builds
CMake supports Release and Debug configurations. These require slightly different invocations
across Windows (MSVC) and Linux/macOS (using GCC or Clang). The following instructions will place
optimized binaries under `build/Release` and debug binaries (unoptimized and containing debug
symbols) under `build/Debug`:

On Windows:

```shell
$ cmake -B build
$ cmake --build build --config Release  # Create release binaries in `build\Release`
$ cmake --build build --config Debug    # Create debug binaries in `build\Debug`
```

On Linux / macOS:

```shell
# Configure and build release binaries under `build/Release`
$ cmake -B build/Release -DCMAKE_BUILD_TYPE=Release
$ cmake --build build/Release

# Configure and build debug binaries under `build/Debug`
$ cmake -B build/Debug -DCMAKE_BUILD_TYPE=Debug
$ cmake --build build/Debug
```

We recommend building and running the `Release` version (especially before the final render) for
the fastest results, unless you need the extra debug information provided by the (default) debug
build.

### CMake GUI on Windows
You may choose to use the CMake GUI when building on windows.

1. Open CMake GUI on Windows
2. For "Where is the source code:", set to location of the copied directory. For example,
   `C:\Users\Peter\raytracing.github.io`.
3. Add the folder "build" within the location of the copied directory. For example,
   `C:\Users\Peter\raytracing.github.io\build`.
4. For "Where to build the binaries", set this to the newly-created "build" directory.
5. Click "Configure".
6. For "Specify the generator for this project", set this to your version of Visual Studio.
7. Click "Done".
8. Click "Configure" again.
9. Click "Generate".
10. In File Explorer, navigate to build directory and double click the newly-created `.sln` project.
11. Build in Visual Studio.

If the project is succesfully cloned and built, you can then use the native terminal of your
operating system to simply print the image to file.

### Running The Programs

You can run the programs by executing the binaries placed in the build directory:

    $ build\Debug\inOneWeekend > image.ppm

or, run the optimized version (if you compiled with the release configuration):

    $ build\Release\inOneWeekend > image.ppm

The generated PPM file can be viewed directly as a regular computer image, if your operating system
supports this image type. If your system doesn't handle PPM files, then you should be able to find
PPM file viewers online. We like [ImageMagick][].


Corrections & Contributions
----------------------------
If you spot errors, have suggested corrections, or would like to help out with the project,
_**please review the [CONTRIBUTING][] document for the most effective way to proceed.**_


Time-Dependent Affine Light Instances (`TheRestOfYourLife`)
------------------------------------------------------------

`src/TheRestOfYourLife/affine_light.h` adds `affine_instance`, a reusable wrapper that turns
any hittable (a sphere, quad, moving sphere, or another wrapper) into a moving, arbitrarily
deformed light. It can be nested, and one underlying object may be shared by several
instances; the object itself is never moved.

### World transform and matrix layout

A local point `p` is mapped to world space by

    x_world(time) = A * p + b(time),   b(time) = (1 - time) * b0 + time * b1,
    time in [0, 1].

`mat3` stores its nine entries **row-major** as `e[row][column]` and multiplies a column
vector, so `(A * v)[i] = sum_j A(i,j) * v[j]`. `mat3::from_rows` / `mat3::from_columns`
construct matrices from basis vectors, and `inverse()` / `transpose()` / `determinant()`
are provided. The linear part may contain non-uniform scale and shear, and may have a
negative determinant. The matrix and the two translation endpoints are copied at
construction; non-finite or singular input is rejected with `std::invalid_argument`.

Rays are transformed by the inverse map using one shared shutter time. The ray parameter is
unchanged, so a non-normalized world direction still yields a `t` for which `ray.at(t)` is
the world hit point. Normals use the inverse transpose of `A` and are re-normalized; the
local object's exterior side is authoritative, so mirroring never swaps inside and outside.
Material, UV coordinates and the ray time are preserved, and the bounding box covers the
whole shutter interval (including both translation endpoints).

### Time-aware sampling entry points

`hittable` gains two virtuals alongside the original time-less ones:

    virtual double pdf_value(const point3& origin, const vec3& direction, double time) const;
    virtual vec3   random     (const point3& origin, double time) const;

Both default to the legacy two-/one-argument virtuals, so a custom object that only
implements the old interface keeps working unchanged. `hittable_list`, `translate`,
`rotate_y`, `sphere` (including moving spheres) and `quad` override them and forward the
same time; `affine_instance` evaluates its wrapped object at the given instant and treats
the legacy entry points as a `time = 0` request.

`hittable_pdf` has a three-argument constructor `(objects, origin, time)` that dispatches
through the time-aware entry points; the original two-argument constructor keeps its exact
legacy dispatch. `camera` adds `enable_time_lights` (default `false`). When enabled, the
`mixture_pdf` is built at the current path ray's time, the scattered ray keeps that time,
and every later bounce reuses it, so the proposal, density query and visibility hit always
share one instant. With the flag off, the random-number order and output of static legacy
scenes are unchanged.

### Density measure: world solid angle with the direction Jacobian

Proposals reuse the local object's own `random()`/`pdf_value()`, evaluated after mapping
the observation point back into local space; the generated local direction is mapped back
with `A`. `pdf_value` returns a **world-space solid-angle density**. For the direction map
`d_world = A * d_local`,

    d(omega_world) / d(omega_local) = |det A| * |d_local|^3 / |d_world|^3,
    pdf_world(d_world) = pdf_local(d_local) * |d_world|^3
                                      / (|det A| * |d_local|^3).

This is a genuine solid-angle Jacobian, not an area ratio or a bare division by the
determinant. It is invariant when the query direction is scaled by any positive factor, and
the absolute value of the determinant keeps the density non-negative under mirroring. A
zero or non-finite query direction returns zero density; an out-of-range or non-finite time
is rejected.



[book1]:           books/RayTracingInOneWeekend.html
[book2]:           books/RayTracingTheNextWeek.html
[book3]:           books/RayTracingTheRestOfYourLife.html
[CONTRIBUTING]:    CONTRIBUTING.md
[cover1]:          images/cover/CoverRTW1-small.jpg
[cover2]:          images/cover/CoverRTW2-small.jpg
[cover3]:          images/cover/CoverRTW3-small.jpg
[discussions]:     https://github.com/RayTracing/raytracing.github.io/discussions/
[GitHub home]:     https://github.com/RayTracing/raytracing.github.io/
[ImageMagick]:     https://imagemagick.org/
[implementations]: https://github.com/RayTracing/raytracing.github.io/wiki/Implementations
[issues]:          https://github.com/RayTracing/raytracing.github.io/issues/
[PRINTING.md]:     PRINTING.md
[web1]:            https://raytracing.github.io/books/RayTracingInOneWeekend.html
[web2]:            https://raytracing.github.io/books/RayTracingTheNextWeek.html
[web3]:            https://raytracing.github.io/books/RayTracingTheRestOfYourLife.html
