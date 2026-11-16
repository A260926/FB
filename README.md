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

Voxel surface lights and direct illumination
=====

The windowless CPU build adds emissive voxel face lights on top of the existing
single directional light. The new code only *reads* the compact octree; normal
encoding, raymarching and the save/load container are unchanged.

Files
-----

* `src/VoxelOctreeReader.hpp` - read-only decoder for the compact hierarchy
  (child/leaf masks, narrow and double-word child offsets, reverse slot order).
* `src/VoxelSurfaceLights.hpp/.cpp` - a `VoxelSurfaceLights` resource bound to
  an immutable `VoxelOctree`. It extracts the exposed outer-surface rectangles
  and maintains the area-luminance sampling structure.
* `src/VoxelDirect.hpp/.cpp` - the `estimateDirect(...)` production entry point
  plus an exact open-interval shadow traversal of the same octree.
* `examples/SurfaceLightsHeadless.cpp` - builds a raw scene into a real octree,
  renders colored face lights and an occluder to PPM, and re-lights once.
* `tests/SurfaceLightsTarget.cpp` - the `exposure`, `distribution` and
  `transport` task cases (no `main`; driven by `task_support/driver.c`).

Space convention
----------------

The root still occupies the world box `[1,2]^3`. Internally every leaf is an
axis-aligned solid box on an integer root grid of `2^23` cells per axis, so a
grid cell maps to `world = 1 + cell / 2^23` with no dependence on the source
voxel resolution. Mixed-depth leaves keep their true (coarse) box size; the
geometry pass reads the compact hierarchy directly with hierarchical neighbor
queries and rectangle subtraction. It never expands to finest voxels, never
enumerates unit faces and never builds a triangle mesh.

Only the outer surface of occupied leaves is emissive. Two touching non-empty
leaves - even with different payloads - share an internal face that never
emits; when a small neighbor covers only part of a large leaf face, just the
remaining exposed area survives as one or more rectangles. Each rectangle keeps
a stable `faceId`, its leaf `payload`, an outward unit geometric `normal` and
its world-space `area`; these never change when lights are toggled.

Emission table and PDF
----------------------

`setEmission(uint32 key, Vec3 radiance)` stores finite, non-negative RGB keyed
by the full 32-bit leaf payload. Unset keys are black. Invalid values throw
`std::invalid_argument` and leave the previous distribution untouched; assigning
the identical value again does no work. Several resources may bind the same
tree with independent emission tables.

The luminance weight is `Y = 0.2126 R + 0.7152 G + 0.0722 B`. A face carries
mass `area * Y`; `totalMass()` is the sum over all lit faces and is zero for a
black table. `sample(u0,u1,u2,out)` picks a face with probability proportional
to its mass and then a point uniformly over that rectangle. `pdfArea(faceId)`
is the joint *area* density `Y(face) / totalMass` (zero for dark faces), so its
integral over the surface is one - it is deliberately not the reciprocal of the
selected face area. A black table produces no samples and every PDF is zero.

Update complexity
-----------------

Geometry is scanned once when the resource is constructed (counted separately
as `geometryNodeVisits`). Recoloring a key walks only that key's patch leaves
and their mass ancestors, then recomputes the touched ancestor masses bottom-up
from their children (so blacking a key yields an exact zero). It never rescans
the octree, rebuilds the face list or rebuilds the whole CDF, and does not touch
the subtrees of unrelated keys. `lastUpdateStats()` reports
`geometryNodeVisits`, `massNodesVisited` and `massNodesUpdated` for the most
recent `setEmission`; the initial geometry build is not counted as an update.

Direct illumination and visibility
-----------------------------------

`estimateDirect(lights, P, N, albedo, u0, u1, u2)` takes one area sample and
returns `radiance * albedo / pi * max(0,N.wi) * max(0,Nlight.-wi) /
(dist^2 * pdfArea)`, and zero for back faces or zero distance. Visibility is
resolved against the same octree with a double-precision hierarchical ray/box
traversal using the open parameter interval between the receiver and the light
sample. Only an occupied intersection of positive length strictly inside that
interval casts a shadow: contact at the receiving surface or the light surface
cannot self-occlude, and no epsilon is used, so one-cell-thick blockers remain
fully effective.

Call order
----------

1. Build and keep the `VoxelOctree` alive (the resource holds a const pointer).
2. Construct `VoxelSurfaceLights lights(&tree)` once, after geometry is ready.
3. Call `lights.setEmission(key, radiance)` any number of times; toggle keys
   off by assigning black, then light them again - `faceId`s stay fixed.
4. Per shading point, call `estimateDirect(lights, P, N, albedo, u0, u1, u2)`
   with `N` unit length, non-negative finite `albedo` and samples in `[0,1)`.

Build and test
--------------

The SDL viewer, CMake build and octree save/load are retained and untouched. The
headless flow needs no SDL, GPU or downloaded assets:

    bash task_support/run.sh           # existing CPU build/ray/storage baseline
    bash task_support/run.sh target    # baseline + new groups + headless example

