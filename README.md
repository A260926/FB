![XYZRGB Dragon](https://raw.github.com/tunabrain/sparse-voxel-octrees/master/Header.png)

Sparse Voxel Octrees
=========

This project provides a multithreaded, CPU Sparse Voxel Octree implementation in C++, capable of raytracing large datasets in real-time, converting raw voxel files to octrees and converting mesh data (in form of PLY files) to voxel octrees.

The conversion routines are capable of handling datasets much larger than the working memory, allowing the creation and rendering of very large octrees (resolution 8192x8192x8192 and up).

This implementation closely follows the paper [Efficient Sparse Voxel Octrees](https://research.nvidia.com/publication/efficient-sparse-voxel-octrees) by Samuli Laine and Tero Karras.

The XYZRGB dragon belongs the the Stanford 3D Scanning Repository and is available from [their homepage](http://graphics.stanford.edu/data/3Dscanrep/) 

Compilation
===========

A recent compiler cupporting C++11, CMake 2.8 and SDL 1.2 are required to build.

To build on Linux, you can use the `setup_builds.sh` shell script to setup build and release configurations using CMake. After running `setup_builds.sh`, run `make` inside the newly created `build/release/` folder. Alternatively, you can use the standard CMake CLI to configure the project.

To build on Windows, you will need Visual Studio 2013 or later. Before running CMake, make sure that

* CMake is on the `PATH` environment variable. An easy check to verify that this is the case is to open CMD and type `cmake`, which should output the CMake CLI help.
* You have Windows 64bit binaries of SDL 1.2. These are available [here](https://www.libsdl.org/download-1.2.php). Make sure to grab the `SDL-devel-1.2.XX-VC.zip`. Note that if you are using a newer version of Visual Studio, you may need to compile SDL yourself in order to be compatible. Please see the SDL website for details
* The environment variable `SDLDIR` exists and is set to the path to the folder containing SDL1.2 (you will have to set it up manually - it needs to be a system environment variable, not a user variable, for CMake to find it). CMake will use this variable to find the SDL relevant files and configure MSVC to use them

After these prerequisites are setup, you can run `setup_builds.bat` to create the Visual Studio files. It will create a folder `vstudio` containing the `sparse-voxel-octrees.sln` solution.

Alternatively, you can also run CMake manually or setup the MSVC project yourself, without CMake. The sources don't require special build flags, so the latter is easily doable if you can't get CMake to work.

To build on macOS, you will need to install SDL first (i.e. `brew install sdl`). Then build it like a regular CMake project:

    mkdir build
    cd build
    cmake ../
    make
    ./sparse-voxel-octrees -viewer ../models/XYZRGB-Dragon.oct

On macOS, you may need to click+drag within the application window first to make the render visible.

Note: If building fails on macOS, you can try commenting out the follow lines in <code>CMakeLists.txt</code>

    #if (${CMAKE_SYSTEM_NAME} MATCHES "Darwin")
    #    set(Sources ${Sources} "src/SDLMain.m")
    #endif()

Usage
=====

On startup, the program will load the sample octree and render it. Left mouse rotates the model, right mouse zooms. Escape quits the program. In order to make CLI arguments easier on Windows, you can use <code>run_viewer.bat</code> to start the viewer.

Note that due to repository size considerations, the sample octree has poor resolution (256x256x256). You can generate larger octrees using the code, however. See <code>Main.cpp:initScene</code> for details. You can also use <code>run_builder.bat</code> to build the XYZ RGB dragon model. To do this, simply download the XYZ RGB dragon model from http://graphics.stanford.edu/data/3Dscanrep/ and place it in the <code>models</code> folder.

Code
====

<code>Main.cpp</code> controls application setup, thread spawning and basic rendering (should move this into a different file instead at some point).

<code>VoxelOctree.cpp</code> provides routines for octree raymarching as well as generating, saving and loading octrees. It uses <code>VoxelData.cpp</code>, which robustly handles fast access to non-square, non-power-of-two voxel data not completely loaded in memory.

The <code>VoxelData</code> class can also pull voxel data directly from <code>PlyLoader.cpp</code>, generating data from triangle meshes on demand, instead of from file, which vastly improves conversion performance due to elimination of file I/O. 

Dielectric interfaces and tracing
================================

<code>VoxelInterfaces.hpp/.cpp</code> and <code>VoxelDielectric.hpp/.cpp</code> add dielectric previews on top of an existing, unmodified <code>VoxelOctree</code>. They are headless: no window, no SDL, only C++14, the standard library and the bundled C sources.

`VoxelInterfaces` borrows the tree. Keep the `VoxelOctree` alive and unmodified while the resource (or any cursor it produced) is in use; the resource copies only the material table and the full 32 bit payload to medium mapping. Medium 0 is always air (refractive index 1, absorption 0). Other media must have a finite index of refraction greater than zero and finite, non-negative absorption per channel. Construction throws `std::invalid_argument` for invalid media, for a payload map that names an unknown medium or maps payload 0 to non-air, for a non-zero payload stored in the tree that has no mapping, and for a malformed descriptor hierarchy. Distinct payloads may map to the same medium.

Geometry is read directly from the compact hierarchy. The root is the cube [1,2]^3 (the same placement used by `VoxelOctree::raymarch`); an absent child octant and everything outside the root are air. Leaves may occur at any depth, a descriptor may mix leaf and internal children, and both short and long (two word) child addresses and whole groups of child descriptors are understood. A coarse leaf is never subdivided, no triangle mesh is built, and no leaf boxes are collected up front.

Interface queries
-----------------

`begin(origin, direction, tMax)` and `begin(origin, direction, tMin, tMax)` return a `VoxelInterfaces::Cursor`. The ray is `p(t) = origin + direction*t`; `direction` is finite and non-zero but need not be unit length, and `t` is the original ray parameter. The window must be finite with `tMax > tMin`. The first ray origin (and the point at `tMin`) must not lie exactly on a leaf boundary.

A cursor keeps only O(tree depth) state and re-locates its neighbourhood from the root as it advances, descending only into children the ray intersects. It never scans subtrees the ray misses. Moving to the next cell uses explicit boundary side state (crossed axis, travel sign and the exact dyadic face coordinate): there is no fixed epsilon, no `nextafter` coordinate nudging, and `VoxelOctree::raymarch` is not consulted to guess the two sides. Seams between two leaves with the same medium number are skipped, so air is never inserted between two touching non-air media. `descriptorReads()` counts the actual 32 bit octree words read by that cursor, and the resource exposes a cumulative count. Cursors are independent value objects; copying one starts an independent traversal.

`firstSegment()` describes the segment starting at the origin (its medium, payload, start, end and world length) even before the first `next()`. `next(event)` advances to the next medium-number change. `event` reports the ray parameter, the preceding segment length, both medium numbers, the raw far-side payload, the axial unit normal pointing towards the incident medium, the world position and whether the far side is outside the root. Reaching the parameter window or the exterior ends the walk; `lastSegment()` then describes the terminal segment and `reachedBackground()` says whether the background should be evaluated, with `backgroundDirection()` giving the unit direction. A change from air to air (including the final root escape while in air) is not an interface.

`branch(side, newDirection, tMax)` forks at the most recently returned interface. It starts exactly at the interface position with local `t = 0`, placed inside the cell on the explicit side of the boundary (`NearSide` = incident medium, `FarSide` = the other side). A reflected direction uses `NearSide`, a refracted direction uses `FarSide`; the direction must point into the chosen side, so a fork never re-hits the boundary it left. Branching before any interface throws `std::logic_error`; invalid directions or windows throw `std::invalid_argument`.

Dielectric transport
--------------------

`VoxelDielectric` traces linear RGB radiance. `trace(origin, direction, background, maxInterfaces)` (optionally with a per-event callback) splits every real interface into an exact unpolarized Fresnel reflection and, unless total internal reflection occurs, an exact Snell transmission. The reflection weight is `F`, the transmission weight is `(1-F)*(eta_i/eta_t)^2`, and total internal reflection keeps only the reflection. Each segment is attenuated per channel by `exp(-absorption * worldLength)`; there is no volume scattering and no stochastic sampling. `refract()` exposes the Fresnel/Snell result on its own for testing.

The direction-only background function is called only when a branch truly leaves the root while propagating outwards. An empty cavity inside the root keeps searching for the following medium, and a ray that starts outside is not resolved to the background before it has had a chance to enter the root.

`maxInterfaces` counts only events where the medium number actually changes; same-medium seams never consume budget. A branch that reaches the budget while further interfaces remain contributes zero, while a branch with no further interface still evaluates the background. The per-event callback receives the segment length, both medium numbers and the axial normal (traversal) together with the reflection/transmission directions and weights and the segment attenuation (optics), so traversal and optics can be checked independently. `descriptorReads()` reports the cumulative words read by the trace.

Example and tests
-----------------

<code>examples/DielectricHeadless.cpp</code> generates raw data, builds a real octree through <code>VoxelData</code>, renders two contacting glasses with an air slot to <code>.task-build/dielectric.ppm</code>, and prints the interface count and descriptor reads. It can be built with the same object list the regression uses:

    c++ -std=c++14 -O2 -pthread -Isrc -I. examples/DielectricHeadless.cpp @.task-build/objects.rsp -o .task-build/dielectric-example
    rm -f .task-build/dielectric.ppm && .task-build/dielectric-example

The regression in <code>tests/DielectricTarget.cpp</code> registers `interfaces`, `optics` and `transport` cases and is driven by <code>task_support/run.sh target</code> (which also runs the existing CPU build/ray/storage baseline and the example).

