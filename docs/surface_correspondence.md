# SurfaceCorrespondence

`SurfaceCorrespondence` (see `include/cinolib/surface_correspondence.h`) keeps a
full, bijective surface-to-surface correspondence between an immutable **source**
triangle mesh and a working `Trimesh<>` copy that is progressively coarsened by
edge collapses. Painted points and picking locations can therefore be carried
back and forth between any two levels of detail without nearest-face spatial
queries and without ever re-sampling the source surface.

## Local parameterization of one collapse

For a candidate edge `(v0, v1)` the algorithm works only on the union of the two
endpoint face stars, which is a topological disk (closed umbrellas, e.g. the
link of a tetrahedron edge, are rejected). Two planar disks are built inside the
strictly convex unit circle:

1. **Boundary loop.** The boundary edges of the star complex are ordered into a
   single oriented loop. Only `v0` and `v1` are allowed to be interior vertices;
   anything else makes the star non disk-like and the move is rejected.
2. **Pre-collapse disk.** Every boundary vertex is pinned on the unit circle,
   with arc lengths proportional to the cumulative 3D edge length measured along
   the boundary winding. Interior vertices (the two endpoints) solve a
   uniform-weight harmonic system (`L x = 0`, Tutte embedding) with an LDLT
   solve. The embedding is accepted only if every planar triangle has strictly
   positive signed area; any flipped or degenerate parameter triangle rejects
   the whole candidate.
3. **Post-collapse disk.** The surviving star is synthesized *before* touching
   the mesh: each surviving face is the old face with the removed endpoint
   substituted by the kept one, repositioned at `edge_sample_at(edge, lambda)`.
   It is parameterized with the identical boundary pinning, so both disks share
   exactly the same boundary polygon and the map restricted to the boundary is
   the identity (a removed endpoint is never collapsed onto an edge).
4. **Planar overlay.** The pre disk carries the images of the existing
   correspondence patches (source side), the post disk carries the new current
   triangles. For every source-patch triangle and post triangle the convex
   intersection is obtained by Sutherland–Hodgman clipping and triangulated.
   These pieces form an exact, gap/overlap free common refinement. Degenerate,
   reversed or inconsistently covered pieces reject the move.
5. **Commit.** Only after every check passes does the real production
   `Trimesh::edge_collapse` (the same link condition and geometric
   admissibility as the legacy path) run. Hence a rejected candidate leaves the
   mesh, the patches and all queries unchanged.

## How patches grow and compose

A `CorrespondencePatch` is one affine piece: it references one immutable
`sourceFace` and one dense `currentFace`, and stores three barycentric triples
on each side, both forming a positively oriented inner triangle.

* Existing patches are never re-sampled. When a collapse touches their current
  face, their source-side triangles are embedded into the pre-collapse planar
  disk and participate in the overlay. New pieces reach the immutable source
  surface by **composing** the planar barycentric coordinates with the patch's
  existing source bary chain, so every new patch still points directly at the
  construction-time source face.
* Each collapse only touches the endpoint stars and the patches attached to
  them; no whole-mesh copy, snapshot or history replay is performed. The patch
  count grows locally with the number of overlay intersections inside the
  disk.
* The patches partition every source face and every current face exactly:
  per-face summed patch area equals the face area, and every patch has strictly
  positive 3D area on both sides.

## Rewrite reports and dense numbering

The mesh mutation paths (`poly_add`, `poly_remove`, `poly_switch_id`,
`vert_switch_id`, `edge_switch_id`) optionally stream local events into an
`ElementRewriteReport`; attaching no report keeps the historical behavior
unchanged. A report-aware `Trimesh::edge_collapse` overload wraps the legacy
implementation. `SurfaceCorrespondence` consumes those events (and the
`adj_v2p` ordering of the reborn faces) to translate old face ids into the
final compacted dense ids without rescanning the mesh, and exposes a
`CollapseRewriteReport` describing removed faces, reborn faces and the id
swaps.

## Queries

* `toCurrent(sourceFace, bary)` and `toSource(currentFace, bary)` accept finite,
  non negative weights summing to 1 within `1e-12`; vertex and edge points are
  legal. Illegal arguments throw `std::invalid_argument`.
* A point is mapped through the affine piece containing it. When a point lies on
  an edge or vertex and therefore belongs to several target faces, all incident
  pieces are considered and the target face with the smallest dense id is
  returned, so different legal starting representations of the same surface
  point give identical answers.
* Both directions are mutually inverse up to floating point error (well below
  `1e-9` at ordinary scale).

## Admissibility

`collapse` returns `false` for boundary-incident edges, link-condition or
geometric failures, non-disk stars, flipped planar parameterizations and
inconsistent overlays; it throws `std::invalid_argument` for a bad edge id or a
non finite `lambda` outside `[0,1]`. Topologically independent but spatially
overlapping shells remain separate because every decision uses connectivity and
the local parameterization, never 3D proximity.
