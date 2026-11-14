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

## Locally adaptive red refinement (`AdaptiveRefiner`)

`include/AdaptiveRefiner.h` / `libsrc/AdaptiveRefiner.cc` add a second, persistent
subdivision model next to the existing whole-mesh `subdiv()` (whose behaviour
is unchanged).  It is meant for sculpting brushes: refine only the faces under
the brush, keep their history, and undo it again later.

```cpp
#include "AdaptiveRefiner.h"

TriMesh base = /* finite, non-degenerate, consistently oriented 2-manifold */;
AdaptiveRefiner refiner(base);

auto root_ids = refiner.leaves();      // initially every input face
refiner.refine({ 17, 42 });           // red-split these two current leaves
AdaptiveRefiner::Snapshot snap = refiner.snapshot();
snap.mesh.write("brush.obj");         // ordinary indexed triangle mesh
```

### Red-leaf numbering

* A `NodeId` is a `std::uint64_t`.  The **root nodes are numbered exactly like
  the input faces**: root face `i` is node `i`.  Every other node gets the
  next never-used number when it is first created.
* Numbers are monotone and permanent: once a node id has been published it
  always denotes the same triangle.  Coarsening a parent and refining it again
  re-activates its original four child ids rather than allocating new ones.
* `leaves()` returns the current red leaves sorted by id.
  `children(id)` returns the four children in the fixed local template order
  `(v0,m01,m20), (v1,m12,m01), (v2,m20,m12), (m01,m12,m20)`; it is empty for a
  node that has never been subdivided, and keeps returning the same four ids
  after that parent has been coarsened.

### Midpoint identity and interpolation

* Level-0 vertices keep the input vertex index as their persistent number.
* An edge midpoint is identified **topologically** by the unordered pair of
  persistent endpoint numbers, never by spatial distance.  Two vertices at the
  same position with different indices (coincident but distinct components)
  stay distinct.  A midpoint is created when first needed, gets the next free
  persistent number, and reuses that number forever.
* Midpoint ordering does not depend on request order: within a refinement
  round, leaves are processed in `NodeId` order and edges in sorted
  persistent-endpoint order, and closure is applied in rounds.
* Positions, colors and confidences of exported vertices are always
  interpolated with the linear midpoint rule **directly from the original
  mesh**, never by re-interpolating previously compacted indices.

### The one-level-gap rule and closure

* After refinement, any two red leaves that share an edge of positive length
  differ by at most one level.  Faces that only touch at a vertex are
  unconstrained.  The closure refines the *coarser* side only, propagating
  locally across root-edge boundaries; a whole connected component is never
  uniformly refined just to satisfy the rule.
* If the closure would need a level greater than `MAX_LEVEL` (12),
  `refine()` throws `std::runtime_error` and the refiner is unchanged.
  Invalid, deactivated or non-leaf ids throw `std::invalid_argument`, also
  without leaving a partial subtree.

### Coarsening blocked

`coarsen(parents)` retracts a parent only when all four of its children are
current red leaves.  The request is judged **as a whole**: if the retraction of
any requested parent would create a level gap larger than one, the call
returns `false`, changes nothing, and does not coarsen other (legal) parents
from the same request.  Bad ids, non-parents and incomplete sibling groups
throw `std::invalid_argument`.  A successful call returns `true`.

### Green transition triangles in `snapshot()`

Red leaves are exported in leaf-id order with a fixed template.  When a
red leaf has a neighbour one level finer across a shared edge, the coarser
leaf gets **green** transition triangles so the output stays watertight:

| split edges | output triangles | shape |
|-------------|------------------|-------|
| 0 | 1 | plain red leaf |
| 1 | 2 | one midpoint, edge split in two |
| 2 | 3 | one midline triangle plus two corner triangles |
| 3 | 4 | three corner triangles plus a central triangle |

For a two-split patch, the remaining quad's diagonal is chosen as the
lexicographically smaller of the two candidate diagonals expressed as ordered
persistent-endpoint pairs, which makes exports deterministic.  Green faces are
not red leaves and cannot be passed to `refine()`.

Each red leaf's own output faces cover its area exactly, the output has no
hanging vertices, overlaps, cracks or extra boundary, and winding matches the
root face.  Output vertices contain only points actually used, sorted by
persistent id.

`Snapshot::root_face[i]` and `Snapshot::leaf_id[i]` are parallel to
`Snapshot::mesh.faces`.  The exported `TriMesh` is a plain indexed mesh: all
derived fields (strips, grid, flags, normals, curvature, point areas,
connectivity) are empty and the bounding volumes are invalid, so calling
`need_normals()`, `need_pointareas()` or `need_across_edge()` recomputes them
consistently.

A complete runnable walkthrough (refine, coarsen, OBJ export) is in
`examples/adaptive_refinement.cc`; `tests/adaptive_refinement.cc` checks the
contract above and is wired into `sh support/run.sh target`.

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
