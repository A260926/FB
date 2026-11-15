Trilinear Surface Extraction
============================

The classic ``extractMarchingCubesMesh()`` decides every cell from the eight
corner signs alone, through a fixed lookup table. On an ambiguous face the two
cells sharing that face can therefore resolve the crossings differently, and
changing the *magnitude* of the corner densities (without changing any sign)
can change how many surface components a cell contains.

``extractTrilinearMesh()`` instead treats every cell as the unique trilinear
interpolant of its eight corner densities and derives the topology from the
field itself.

Cell classification
-------------------

For a cell with corner deviations ``q[k]`` (density minus the controller
threshold) the trilinear field over local coordinates ``(x,y,z)`` is

    F(x,y,z) = c0 + a*U + b*V + c*W + d*U*V + e*U*W + f*V*W,  U=2x-1 ...

1. Uniform cells (all eight deviations on one side of zero) contain no
   surface and are skipped.

2. On every ambiguous face the bilinear saddle point inside the face is
   located analytically. Its sign tells which sign region connects through
   the face centre; the opposite-sign corners are the isolated islands whose
   boundary crossings form the two arcs. Because the decision is a property of
   the shared field, two neighbouring cells always choose the same pairing.

3. The zero intersections on lattice edges are found by linear root finding
   along the edge; each global lattice edge is evaluated once and shared by
   every cell touching it, so chunked extraction produces matching vertices on
   shared faces.

4. The face arcs are joined into closed boundary loops on the cell boundary.
   Each loop bounds one surface sheet. Most loops are capped independently.
   Two loops share an interior vertex only when the trilinear field genuinely
   connects them: either an index-2 interior saddle above the level joins two
   positive basins, or one region borders two loops on both sign sides (a
   sheet that enters and leaves the cell without an interior critical point).
   Interior vertices are placed on the iso surface by bisection towards an
   opposite-sign corner, so a sheet is never given a fixed tetrahedral or
   subdivided-grid topology.

Degeneracy reporting
--------------------

The iso level is ambiguous when it passes (nearly) through a corner, a face
saddle or an interior critical point. Each such quantity is compared against

    degeneracy_tolerance * S

where ``S`` is the largest absolute corner deviation in the cell. When the
magnitude is below that relative bound the cell cannot be classified reliably
and ``PolyVox::trilinear_degenerate_field`` is thrown carrying the global
coordinate of the cell's lower corner. Ordinary ambiguous faces that are
comfortably decidable are not rejected, and one-sided cells are skipped
silently. The whole extraction fails as a unit, so no partial mesh is ever
returned.

Coordinates, normals and materials
----------------------------------

Vertex positions are ordinary floats relative to the region lower corner
(also stored as the mesh offset), with no 8.8 fixed point encoding. Edge
vertex normals interpolate the central-difference gradients of neighbouring
nodes, while interior vertices use the gradient of the trilinear field; every
normal is finite, unit length and oriented towards the low density side.
Triangle winding is corrected per triangle so it consistently faces low
density. Material/density voxel types keep working through the same
``DefaultMarchingCubesController`` conventions (density, threshold and
material blending).
