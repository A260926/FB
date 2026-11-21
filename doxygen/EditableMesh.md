Editable Meshes
===============

`FastBVH::EditableMesh` (see `include/FastBVH/EditableMesh.h`) is a triangle
mesh that owns its vertices and indices, keeps exactly one BVH (built with
@ref FastBVH::DefaultBuilder), and maintains two things as vertices are
edited:

 * the set of self-intersection contacts between mesh triangles, and
 * the bounding boxes of that same BVH, refit in place, so the native
   @ref FastBVH::Traverser keeps answering ray queries against the current
   geometry without any rebuild, re-ordering, or copy of the tree.

```cxx
#include <FastBVH.h>

std::vector<FastBVH::Vector3<double>> vertices = /* ... */;
std::vector<std::array<uint32_t, 3>> faces = /* ... */;

FastBVH::EditableMesh mesh(vertices, faces);

// The current self-intersection contacts, sorted by face id pair.
for (const auto& c : mesh.contacts()) {
  printf("faces %u / %u: %s\n", c.face_a, c.face_b,
         FastBVH::contact_kind_name(c.kind));
}

// Apply a batch of vertex moves atomically.
auto result = mesh.update_vertices({
  FastBVH::EditableMesh::VertexMove{17, {1.0, 2.0, 0.0}},
});
if (result.accepted) {
  /* result.added / result.removed: the kind-aware contact difference,
     sorted by (face_a, face_b). result.stats: actual work counters. */
}

// Ray preview through the updated native traverser. The direction does
// not need to be normalized; t is the parameter of the given ray.
FastBVH::Ray<double> ray({0, 0, 10}, {0, 0, -1});
FastBVH::EditableMesh::RayHit hit;
if (mesh.closest(ray, hit)) {
  /* hit.face: original face id; hit.t; hit.u / hit.v: barycentrics. */
}
```

Face ids always follow the input order, even though the builder reorders
its internal face slots. `node_diagnostics()`, `leaf_slots()` and
`face_leaf()` expose readable tree node / leaf slot diagnostics.

## What counts as a self-intersection

For two closed triangles T1, T2, let K be the simplex spanned by the vertex
*indices* they share. A contact is reported iff T1 ∩ T2 is not entirely
contained in K:

 * ordinary shared edges / shared vertices are *not* reported;
 * two faces that share an edge but fold back coplanar over each other
   *are* reported (the overlap exceeds the shared edge);
 * coincident points with *different* vertex indices do not count as
   topological adjacency and are reported.

Contacts are classified by the geometry of the intersection:
`ContactKind::Point` (single point), `ContactKind::Segment` (line segment,
typical for transversal crossings) and `ContactKind::Area` (positive-area
coplanar overlap). Every unordered pair appears at most once in
`contacts()`, stored as (smaller face id, larger face id) and sorted.

## Numeric contract

 * Vertex coordinates must be finite and satisfy |coord| <= 1e6.
 * Faces must be non-degenerate: edge lengths >= 1e-5 and shortest
   triangle height >= 1e-5. `update_vertices()` rejects batches that would
   violate this (also batches with unknown or duplicated vertices,
   non-finite positions, or positions outside the coordinate bound).
   Rejected batches leave vertices, tree, contacts and ray results
   untouched.
 * Exact coplanarity and exact contact are supported. Everything else that
   does not intersect is assumed to be separated by at least 1e-7.
 * Signed distances below 1e-8 are treated as zero (exact contact), one
   decade below the 1e-7 separation guarantee. Box overlap is only a
   broad-phase filter, never a face intersection, and tolerances are not
   inflated to swallow clearly separated faces.
 * Compile without "fast math" flags: the exact-contact guarantees rely on
   IEEE double-precision semantics.

## Work statistics

`UpdateStats` reports the *actual* work of an accepted update:

 * `leaves_recomputed` - leaf node boxes refit (the leaves of dirty faces,
   each counted once);
 * `internal_nodes_recomputed` - internal node boxes refit (the distinct
   ancestors of dirty leaves);
 * `node_pair_checks` - broad-phase AABB tests (dirty face box vs. tree
   node box) performed while pairing;
 * `face_box_checks` - broad-phase AABB tests between two face boxes;
 * `triangle_pair_tests` - narrow-phase triangle/triangle tests executed.

Only dirty faces (faces incident to a moved vertex) are refit and queried;
contacts not incident to a moved vertex are preserved without re-running
their narrow-phase test. As a reference point: with 4096 independent
triangles spaced 8 units apart along X, moving one vertex of the middle
face inside its own interval costs 1 leaf refit, about a dozen node
refits, and zero narrow-phase tests.
