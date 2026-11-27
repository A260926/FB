ao-mesher
=========
A voxel mesher for ndarrays that handles ambient occlusion and transparency.

## Install

    npm install

### `require("ao-mesher")(array)`
Constructs a mesh for `array`.

* `array` is a 3D ndarray of voxels, padded by one cell on every side.

**Returns** A `Uint8Array` encoding the mesh (8 bytes per vertex), or else
`null` if there were no facets.  This is the same format that `ao-shader`
expects.

### `require("ao-mesher").exactAO(array, voxelSideTextureIDs?)`
An alternative mesher with the same inputs, output format and neighbourhood
conventions as the legacy entry point, implemented in `exact-mesh.js`.

The two paths differ in how they decide which unit faces may be stretched into
one large rectangle.

* **Legacy path.** The four corner AO bits of a unit face are treated as an
  ordinary face label.  Any two faces carrying the same label are merged by the
  greedy mesher, regardless of what happens inside the merged rectangle.  But
  the renderer interpolates the AO intensity *linearly inside each triangle*,
  so four identical corners do not guarantee that the shading across a stretched
  rectangle still matches the unit faces it replaced.  Faces with the same mask
  but an incompatible interior field can therefore be over-merged and render
  with distorted ambient occlusion.

* **Exact path.** Every visible unit face is built independently with the same
  `vertexAO`, `facetAO` and `AO_TABLE` rules, and the legacy `flipAO` rule picks
  its diagonal.  These unit triangles define a continuous piecewise-affine
  reference field (barycentric interpolation, not bilinear and not just corner
  values).  A rectangle is merged only when, for one of its diagonals, the two
  big triangles reproduce that reference field over the *whole* rectangle -
  including regions where the new diagonal crosses the original unit-triangle
  edges.  Equality is checked on the vertices of the overlay of the two
  triangulations using double precision with a `1e-9` tolerance; it never relies
  on point sampling or on the rectangle's outer boundary alone.

  Non-constant faces are not automatically split back into unit faces: a region
  whose field happens to be exactly expressible by one large rectangle (for
  example a smooth affine gradient, or any constant region) is still merged.
  When both diagonals of a rectangle are legal, the legacy AO diagonal
  preference is used.  Merge order is fixed: normal axis and slice first, then
  within a slice the second in-plane coordinate followed by the first; at the
  first uncovered unit face every uncovered legal rectangle is enumerated and
  the largest area wins, with equal areas resolved by the larger span along the
  first in-plane axis.

Coordinates are still written into single bytes, so each core axis is limited
to 255 cells; negative strides, non-contiguous offsets and axis transposes are
all supported.

### Example
`examples/staircase.js` builds a staircase on a flat base platform, meshes it
with the exact-AO path and writes an ASCII PLY (`examples/staircase.ply`) whose
greyscale vertex colours are the per-vertex AO intensity:

    node examples/staircase.js

The flat base collapses into a single large rectangle while the staircase keeps
its per-corner occlusion.

## Test

    sh support/target.sh

The regression suite in `test/exact-ao.js` covers the reference field, the
merge planning and the output/geometry compatibility with the legacy path.

# Credits
(c) 2013 Mikola Lysenko. MIT License
