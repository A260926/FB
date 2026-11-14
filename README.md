# TriMesh2 (v2.16)
[![C/C++ CI](https://github.com/Forceflow/trimesh2/actions/workflows/c-cpp.yml/badge.svg)](https://github.com/Forceflow/trimesh2/actions/workflows/c-cpp.yml) [![License: GPL v3](https://img.shields.io/badge/License-GPL%20v3-blue.svg)](https://www.gnu.org/licenses/gpl-3.0)

A C++ library and set of utilities for input, output, and basic manipulation of 3D triangle meshes.

![trimesh2 logo](https://raw.githubusercontent.com/Forceflow/trimesh2/main/html/trimesh_logo.jpg)

This is a fork of the [TriMesh2 library](http://gfx.cs.princeton.edu/proj/trimesh2/) (originally by [Szymon Rusinkiewicz](https://www.cs.princeton.edu/~smr/)), which I use a lot in my [other](https://github.com/Forceflow/ooc_svo_builder) [graphics](https://github.com/Forceflow/cuda_voxelizer) [projects](https://github.com/Forceflow/gpu_suggestive_contours). I like TriMesh2 because of the low setup costs required to do model loading, as well as the robust and powerful implementation of various model manipulation techniques.

The original TriMesh2 project is quite Linux/GCC-oriented, and only has limited Win32 support (through MinGW compilation targets). The primary aim of this fork is to add a stable Visual Studio solution, for both x86 and x64 targets, whilst staying as close as possible to the original codebase (and subsequent updates).
 
## Getting started
 * Download a [prebuilt release](https://github.com/Forceflow/trimesh2/releases) of trimesh2 or build the library yourself (see further below)
   * The static library will be called `trimesh.lib`, the debug version is `trimeshd.lib`.
 * Include the header `include/TriMesh.h`, and make sure the static library is in your build path. All Trimesh2 functions will be in the `TriMesh` namespace.
 * Typical ways to get started:
   * Loading a model : `TriMesh* themesh = TriMesh::read(filename);`.
   * This mesh class contains a data member `vertices` which will be filled with all the vertices of your model, and a data member `faces`, which will tell you which vertices make up a face.
   * If your model contains vertex normals, they will be in `normals`. You can (re)compute them by calling `need_normals` on the mesh. There's also `need_bbox` for a bounding box, `need_dcurv` for curvature, etc.
   * For inspiration on how to use the library and its various features, check out `include/TriMesh.h` and the utilities in the `utilsrc` folder.
## Building
### Dependencies
 * The library itself has no dependencies other than the standard C++ STL
 * The (optional) ``mesh_view`` utility has dependencies on OpenGL and Freeglut, because it needs to display a window with a textured model

### Build steps
  * For **Windows**, build solutions for VS2022 and VS2019 are provided in the `mscv`folder, verified working with the [free Community Editions](https://visualstudio.microsoft.com/vs/community/) of Visual Studio. The solutions contain both Debug and Release profiles for 32-bit and 64-bit builds.
    * The built libraries will be placed in a folder named `lib.(architecture).(visual studio version)` in the trimesh2 root folder. For example, for a 64-bit Visual Studio 2017 build, it will be `lib.win64.vs141`. The utilities will be placed in `util.(architecture).(visual studio version)`. This naming scheme is in place to avoid clashing trimesh2 versions.
   * For **Linux**, a makefile is provided. You might need additional packages before you can build the utilities on your system. On Ubuntu these are: `mesa-common-dev libglu1-mesa-dev libxi-dev`.
   * For **OSX**, I'm being told it builds using the makefile, but I have no way to check. If you encounter problems, please, file an issue report :)

## Info
For the original TriMesh2 project, see [the Trimesh2 homepage](http://gfx.cs.princeton.edu/proj/trimesh2/).
 
Features: 

 * Support for reading/writing PLY, OFF, OBJ files. Read-only: 3DS, SM, RAY.
 * Vec: a templated C++ class for constant-length vectors, with support for the usual arithmetic operations and XForm: a class for rigid-body transformations.
 * An OpenGL trackball/arcball implementation, with automatic selection of rotation center.
 * Algorithms for subdivision, smoothing, curvature estimation, triangle stripping, and various other simple mesh manipulations.

The following utility programs are included:

 * **mesh_view**: A simple 3D mesh viewer
 * **mesh_make**: Create arbitrarily-tessellated meshes of various simple shapes
 * **mesh_filter**: Applies a variety of simple transformations to a mesh, such as converting formats, flipping faces, subdivision, smoothing, rigid-body transformations, etc.
 * **mesh_cc**: List and/or extract connected components from a mesh
 * **mesh_cat**: Combine several meshes into a single file
 * **mesh_align**: Align 2 meshes using ICP
 * **mesh_shade**: A few procedural shaders for adding per-vertex color
 * **mesh_check**: Check for some kinds of topological oddities (e.g., more than 2 faces at an edge) in a mesh file.
 * **mesh_crunch**: Quick-n-dirty mesh decimation using the Rossignac-Borrel method of vertex collapse
 * **mesh_info**: Print out some information about a mesh
 * **xf**: Create or compose transformations in .xf files

## Face direction fields

`include/FaceField.h` provides `trimesh::FaceFieldSystem` (implemented in
`libsrc/face_field.cc`), a solver for smooth N-symmetry direction ("cross")
fields on oriented, non-degenerate, 2-manifold triangle meshes.  Unlike
`diffuse_vector`, which averages world-space vector components, directions
are transported across each interior edge by unfolding the two incident
triangles into a common plane first.  Directions are stored per face as the
complex coefficient `z` of the n-th power of the angle in a local tangent
frame `(e1, e2, normal)` with `e2 = normal x e1`, so a unit direction at
angle `theta` corresponds to `z = exp(i*n*theta)`.  For two faces `f`, `g`
sharing an edge, with `d` the unit vector along the common edge from the
smaller to the larger vertex index and `theta_f`, `theta_g` its angles in
the two frames, the transport factor is
`T_fg = exp(i*n*(theta_f - theta_g))`, and `solve()` returns the unique
minimizer of

```
E(z) = sum_f  A_f * |z_f - u_f|^2
     + tau * sum_{interior edges (f,g)}  w_fg * |z_f - T_fg z_g|^2 ,
     z_f = h_f on constrained faces
```

with `A_f` the triangle area and `w_fg = |edge|^2 / (A_f + A_g)`.  The
magnitude of `z` is meaningful and the result is never renormalized.  The
constructor builds the connection, the free-row operator and the
preconditioner once (all storage is O(#faces), in double precision), so
repeated solves with new data are cheap, and the mesh may be modified or
destroyed after construction.  Invalid input (bad indices, non-finite
data, non-unit or non-tangent bases, `n` outside 1..8, non-positive
`tau`, duplicate constraints, non-manifold or inconsistently oriented
edges) throws `std::invalid_argument`.

```cpp
#include "FaceField.h"
using namespace trimesh;

TriMesh *mesh = TriMesh::read("bent.ply");
std::vector<vec> e1(mesh->faces.size());
for (size_t f = 0; f < mesh->faces.size(); f++) {
    vec nrm = normalized(mesh->trinorm(f));
    vec a(1.0f, 0.0f, 0.0f);
    e1[f] = normalized(a - nrm * (a DOT nrm));   // unit tangent per face
}
std::vector<int> fixed;
fixed.push_back(0);
FaceFieldSystem sys(*mesh, e1, 4, 1.0, fixed);   // 4-RoSy field, tau = 1

std::vector<std::complex<double> > u(mesh->faces.size(), 0.0);
std::vector<std::complex<double> > h(1, 1.0);    // z_0 = 1
FaceFieldSystem::Result r = sys.solve(u, h);     // r.z and r.residual

// Same geometry, bases and constraints: new right-hand sides re-use the
// operator and preconditioner built in the constructor.
u[17] = std::complex<double>(0.0, 2.0);
FaceFieldSystem::Result r2 = sys.solve(u, h);
```

## Fork Details

This fork stays as close as possible to the original [trimesh2](http://gfx.cs.princeton.edu/proj/trimesh2/) code, only changing the actual source files when a solution for compilation errors cannot be reached through VS pre-build steps or preprocessor magic.

Notable changes compared to vanilla trimesh2
 * MSVC project for Visual Studio Community Edition 2022, 2019 and 2017
 * Fixes for FreeGlut / Gluit compilation
 * Fixes for wingetopt replacement in MSVC
 * Added 64-bit MSVC compilation support

## See Also

Other software for importing and manipulating 3D models:
 * [Tiny OBJ Loader](https://github.com/syoyo/tinyobjloader) (by @syoyo)
 * [Tiny PLY](https://github.com/ddiakopoulos/tinyply) (by @ddiakopoulos)
 * [Open Asset Import Library](http://www.assimp.org/) (ASSIMP)
