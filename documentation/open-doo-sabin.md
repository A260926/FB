# Open-Surface Doo-Sabin Subdivision

## Overview

`DooSabinModifier` originally supported closed meshes only. A single open
face (for example one quad) made the vertex-face traversal hit a missing
edge and fail with a `NullPointerException`. The modifier now subdivides
open surfaces as well: sheets, face rings with inner holes, and mixed
triangle/polygon patches.

The modifier still implements `IMeshModifier`, modifies the mesh in place
and returns the same instance. With zero subdivisions the mesh is returned
completely unchanged.

```java
Mesh3D mesh = ...;
Mesh3D result = new DooSabinModifier(2).modify(mesh);
assert result == mesh;
```

## Input requirements

A mesh that is subdivided for one or more rounds must satisfy both of the
following conditions. They are checked before the mesh is modified, and a
violation raises an `IllegalArgumentException` while leaving the mesh
untouched.

1. Every face references at least three **distinct** vertex indices.
2. Every non-isolated **interior** vertex is incident to at least three
   faces. Boundary vertices and valid isolated vertices are unrestricted.

Adjacency is always resolved by vertex index. Two components that occupy
the same coordinates but use independent vertex indices are never welded
together and keep independent subdivision results.

`TraverseHelper` itself is more permissive than the modifier: a legal
two-face interior loop (a valency-two interior vertex) produces a valid
closed fan of size two. The modifier rejects such a vertex on a positive
subdivision round, per the input requirements above.

## Output structure

Each subdivision round keeps the original boundary fixed and inserts a
connection band between the boundary and the new face corner polygons.

- **Face corner polygon** — one per original face, connecting the new
  corner points. The corner point formula is unchanged, so closed meshes
  produce exactly the same geometry and winding as before.
- **Internal edge face** — one quad per internal edge, connecting the
  corner points of the two adjacent faces. Every internal edge is
  referenced twice, once in each direction.
- **Boundary edge face** — one quad per boundary edge. The original
  boundary vertices and the boundary edge itself are retained with their
  original direction; only the two new edges toward the single adjacent
  face corner polygon are added.
- **Interior vertex face** — one closed polygon per interior vertex, as in
  the classic Doo-Sabin scheme.
- **Boundary vertex fan** — when a boundary vertex touches several faces,
  the retained original vertex connects the ordered corner points of those
  faces with triangles. A boundary vertex with a single adjacent face
  produces no face here, because it would degenerate into a digon.

Consequences that hold after every round:

- boundary positions and the connectivity of outer and inner loops never
  change;
- every output face has at least three distinct vertices, so the result can
  be subdivided again directly;
- subdividing once with `new DooSabinModifier(2)` and subdividing twice
  with `new DooSabinModifier(1)` produce the same directed mesh.

## TraverseHelper additions

Two read-only query methods join the existing single-step traversal API.

```java
// One outgoing Edge3D per adjacent face, ordered by getPairNext.
// Closed fans start at the outgoing edge whose end index is the smallest.
// Open fans start at the face whose incoming edge is on the boundary and
// end at the face whose outgoing edge is on the boundary.
List<Edge3D> fan = helper.getVertexFan(vertexIndex);

// True for an open fan; a valid isolated vertex has an empty fan and is
// not considered a boundary vertex.
boolean boundary = helper.isBoundaryVertex(vertexIndex);
```

The constructor validates the topology and throws
`IllegalArgumentException` for a duplicate directed edge, an edge shared by
more than two faces, or a vertex that carries several disconnected fans.
The legacy methods `getOutgoing`, `getNext`, `getPair`, `getPairNext`,
`getFaceByEdge` and `getAllEdges` keep their previous behavior.

## Example: subdividing an open sheet repeatedly

```java
import mesh.Mesh3D;
import mesh.modifier.subdivision.DooSabinModifier;

Mesh3D sheet = new Mesh3D();
sheet.addVertex(0, 0, 0);
sheet.addVertex(2, 0, 0);
sheet.addVertex(2, 2, 0);
sheet.addVertex(0, 2, 0);
sheet.addFace(0, 1, 2, 3);

// First round: one corner quad, four boundary strips, and the four
// retained boundary vertices. The four boundary edges keep their
// positions and directions.
new DooSabinModifier(1).modify(sheet);

// Every later round works on the previous result without any repair.
for (int round = 0; round < 3; round++) {
    new DooSabinModifier(1).modify(sheet);
}
```

## Example: a ring with an inner hole

```java
Mesh3D ring = new Mesh3D();
// Outer square 0..3 and inner square 4..7.
ring.addVertex(0, 0, 0);
ring.addVertex(4, 0, 0);
ring.addVertex(4, 4, 0);
ring.addVertex(0, 4, 0);
ring.addVertex(1, 1, 0);
ring.addVertex(3, 1, 0);
ring.addVertex(3, 3, 0);
ring.addVertex(1, 3, 0);
ring.addFace(0, 1, 5, 4);
ring.addFace(1, 2, 6, 5);
ring.addFace(2, 3, 7, 6);
ring.addFace(3, 0, 4, 7);

Mesh3D twoRounds = ring.copy();
new DooSabinModifier(2).modify(twoRounds);

Mesh3D oneRoundTwice = ring.copy();
new DooSabinModifier(1).modify(oneRoundTwice);
new DooSabinModifier(1).modify(oneRoundTwice);

// Both meshes are equivalent directed meshes, and both keep the eight
// boundary edges of the outer and inner loops.
```
