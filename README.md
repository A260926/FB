# Documentation is hosted at [geometry-central.net](http://geometry-central.net)
---

# Welcome to Geometry Central

[![actions status linux](https://github.com/nmwsharp/geometry-central/workflows/linux/badge.svg)](https://github.com/nmwsharp/geometry-central/actions)
[![actions status macOS](https://github.com/nmwsharp/geometry-central/workflows/macOS/badge.svg)](https://github.com/nmwsharp/geometry-central/actions)
[![actions status windows](https://github.com/nmwsharp/geometry-central/workflows/windows/badge.svg)](https://github.com/nmwsharp/geometry-central/actions)

Geometry-central is a modern C++ library of data structures and algorithms for geometry processing, with a particular focus on surface meshes.

Features include:

- A polished **surface mesh** class, with efficient support for mesh modification, and a system of containers for associating data with mesh elements.
- Implementations of canonical **geometric quantities** on surfaces, ranging from normals and curvatures to tangent vector bases to operators from discrete differential geometry.
- A suite of **powerful algorithms**, including computing distances on surface, generating direction fields, and manipulating intrinsic Delaunay triangulations.
- A coherent set of sparse **linear algebra tools**, based on Eigen and augmented to automatically utilize better solvers if available on your system.


**Sample:**

```cpp
// Load a mesh
std::unique_ptr<SurfaceMesh> mesh;
std::unique_ptr<VertexPositionGeometry> geometry;
std::tie(mesh, geometry) = readSurfaceMesh("spot.obj"); 

// Compute vertex areas
VertexData<double> vertexAreas(*mesh);

geometry->requireFaceAreas();
for(Vertex v : mesh->vertices()) {
  double A = 0.;
  for(Face f : v.adjacentFaces()) {
    A += geometry->faceAreas[f] / v.degree();
  }
  vertexAreas[v] = A;
}
```

Check out the docs, tutorials, and build instructions at [geometry-central.net](http://geometry-central.net).  Use the [sample project](https://github.com/nmwsharp/gc-polyscope-project-template/) to get started with a build system and a gui.


**Related alternatives:** 
[CGAL](https://www.cgal.org/),
[libIGL](https://github.com/libigl/libigl),
[OpenMesh](http://www.openmesh.org/),
[Polygon Mesh Processing Library](https://www.pmp-library.org/),
[CinoLib](https://github.com/mlivesu/cinolib)

---

**Credits**

Geometry-central is developed by [Nicholas Sharp](http://nmwsharp.com), with many contributions from 
[Keenan Crane](http://keenan.is/here), 
[Yousuf Soliman](http://www.its.caltech.edu/~ysoliman/),
[Mark Gillespie](http://markjgillespie.com/),
[Rohan Sawhney](http://rohansawhney.io/), and many others.



If geometry-central contributes to an academic publication, cite it as:
```bib
@article{geometrycentral,
  title={GeometryCentral: A modern C++ library of data structures and algorithms for geometry processing},
  author={Nicholas Sharp and Keenan Crane and others},
  howpublished="\url{https://geometry-central.net/}",
  year={2019}
}
```

Development of this software was funded in part by NSF Award 1717320, an NSF graduate research fellowship, and gifts from Adobe Research and Autodesk, Inc.

---

# Anchored LOD simplification (`SurfaceAnchorSet`)

Measurement markers stored as barycentric coordinates inside a triangle do not survive
halfedge-collapse simplification on their own: a `SurfacePoint` keeps pointing at a face that may
be deleted, and `ManifoldSurfaceMesh::compress()` rewrites every index space. The anchored
simplification path in `geometrycentral/surface/surface_anchors.h` keeps such markers on the same
piece of surface while a `MutationManager` runs its existing quadric-error simplification.

## Usage

```cpp
#include "geometrycentral/surface/mutation_manager.h"
#include "geometrycentral/surface/quadric_error_simplification.h"
#include "geometrycentral/surface/surface_anchors.h"

MutationManager mm(mesh, geom); // the usual manager, with the usual constraints and policies
std::vector<SurfaceAnchorInput> inputs = {
    // id (non-zero, unique), point on this mesh (vertex/edge/face), Euclidean distance budget
    {1, SurfacePoint(someFace, Vector3{0.2, 0.3, 0.5}), 0.05},
};
SurfaceAnchorSet anchors(mm, inputs); // binds to the manager until it is destroyed

// Existing entry point, same quadrics, same heap and halfedge collapses.
quadricErrorSimplify(mesh, geom, tol, mm);
mesh.compress(); // allowed between simplifications; anchors are repaired automatically

for (const SurfaceAnchorSnapshot& s : anchors.snapshot()) {
  // s.id, s.target (immutable initial 3D position), s.maxDistance,
  // s.face + s.barycentric (in the face's current halfedge winding), s.position
}
uint64_t visits = anchors.anchorVisits();             // marker retrievals in the last simplification
uint64_t tests  = anchors.projectionTriangleTests();  // triangle distance tests in the last simplification
```

A runnable end-to-end program is in `examples/anchored_lod.cpp`; it writes three LOD levels
(`level0..2` OBJ + CSV). Build it through the task harness
(`bash task_support/run.sh target`) or directly against the `geometry-central` target.

## What "local projection" means

For each candidate collapse, the affected patch is only the union of the faces incident on the
two edge endpoints, and only anchors currently attached to that patch are touched. The collapse is
simulated first: faces containing both endpoints disappear, every other patch face replaces an
endpoint by the surviving vertex. Each affected anchor then takes its **immutable target** (the
3D position captured at construction) and computes the Euclidean closest point on the **surviving
candidate triangles of that patch only**. Projection never continues from the marker's previous
position, never leaves the patch, and can never jump to another component or a closer distant
shell. Ties are resolved by the smallest initial face index (provenance), and boundary closest
points may carry zero barycentric weight. The candidate is rejected before anything is mutated if
the patch is empty, the simulated topology or oriented normals are invalid, or any marker would
exceed its `maxDistance` budget. Work outside initialization, heap construction, the final
compress and geometry refreshes is therefore proportional to the patch, never to the whole mesh
or the full anchor set.

## Lifecycle and guarantees

- Construction validates every input (non-zero unique ids, finite non-negative budgets, points on
  the bound mesh) and throws `std::invalid_argument` before registering any callback.
- One manager binds at most one set; the set is non-copyable. Its destructor unregisters the
  mesh/manager callbacks and detaches itself without touching the caller's constraints or
  policies, after which the manager behaves exactly as before.
- Only `MutationManager::collapseEdge` (all overloads) and `quadricErrorSimplify` calls routed
  through that manager are tracked. Direct topology edits and split/flip/reposition are not.
  Boundary vertices are fixed, boundary-incident edges never collapse, the link condition is
  enforced, and surviving triangles must keep positive area and a consistently oriented normal.
- On success the new anchor face, barycentric weights and position are published before any
  user-visible `afterEdgeCollapse` observer runs, and required geometry quantities
  (`faceAreas`, `faceNormals`, `vertexDualAreas`, ...) are refreshed first. Before/after
  observers are paired and fire only on collapses that really succeed; registration order of the
  set versus user policies does not matter.
- `mesh.compress()` remaps face/vertex handles and re-expresses barycentric weights in each
  face's new representative-halfedge winding, so weights cannot silently shift to another corner.
- Rejected candidates leave the mesh, coordinates, anchors and geometry caches untouched and do
  not stop the surrounding simplification; previously rejected edges are not blacklisted. The
  constraint layer reports the edges made dirty by each successful collapse so the production
  QEM scheduler recomputes them through the same heap.
- The unanchored public entry points, `collapsibleEdges`/`repositionableVertices` constraints
  and all existing policies keep working unchanged when no set is bound.

