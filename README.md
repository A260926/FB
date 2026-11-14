ao-mesher
=========
A voxel mesher for ndarrays that handles ambient occlusion and transparency.

## Install

    npm install ao-mesher

### `require("ao-mesher")(array[, voxelSideTextureIDs])`
Constructs a mesh for `array`.

* `array` is a 3D ndarray of voxels (padded by a one-cell neighbour margin)
* `voxelSideTextureIDs` is an optional table with a `get(voxel, side)` method
  giving the per-side texture id

**Returns** A `Uint8Array` encoding the mesh, or else `null` if there were no
facets.  This is in the same format that `ao-shader` expects: eight bytes per
vertex `(x, y, z, ao, nx, ny, nz, tex_id)`.

### `require("ao-mesher").exactAO(array[, voxelSideTextureIDs])`
An alternative mesher with the **same inputs and the same 8-byte vertex
format**, but stricter face merging.

#### Why a second path?
The legacy path treats a unit face's four-corner AO mask as an ordinary face
label: faces carrying the same packed value are greedily stretched into one
large rectangle.  Four equal corner labels do not, however, guarantee that the
*interior* shading survives the stretch.  What is actually rendered is the
barycentric (affine) interpolation inside each unit face's two triangles, and
a stretched rectangle re-triangulates that area with two big triangles whose
interior values can differ from the original piecewise-affine field.

`exactAO` keeps the per-unit-face occlusion field.  A rectangle is merged only
when all of the following hold:

* every unit face in the rectangle is visible and shares the normal and
  texture, and
* the two big triangles produced by one of the rectangle's diagonals reproduce
  the reference field over the **whole** rectangle.

The reference field is defined independently per unit face: its four corner
intensities come from the existing `vertexAO` / `facetAO` / `AO_TABLE` logic,
its diagonal from the existing `flipAO` rule, and each unit triangle is filled
by barycentric linear interpolation.  To decide exactness without point
sampling, the unit-face triangulation and the candidate triangulation are
overlaid.  Equality is affine, so it is enough to compare the fields at every
vertex of the overlay — the unit lattice corners, the points where the big
diagonal crosses unit edges, and the points where it crosses the unit diagonals
(including regions formed inside a unit cell).  Intermediate comparisons use
double precision and a tolerance of `1e-9`.

When both rectangle diagonals are legal, the existing AO diagonal preference
(`flipAO`) is kept.  Faces that cannot be expressed exactly are left as unit
faces; constant and genuinely affine regions are still merged aggressively, so
non-constant faces are **not** all split back to unit squares.

#### Merge order
For each normal axis and slice, the scan runs over the second in-plane
coordinate and then the first.  The first uncovered unit face is used as the
lower-left corner, every still-uncovered legal rectangle rooted there is
enumerated, and the largest by area is taken (ties favour the larger first-axis
span).  Both diagonals are tested for each candidate before the rectangle is
accepted or shrunk.

#### Limits
Coordinates must fit in the legacy one-byte vertex positions (an interior core
of at most 255 cells per axis); larger inputs throw a `RangeError`.  The empty
model still returns `null`, and negative strides, offsets and axis transposes
on the input ndarray are supported.

## Example

```javascript
var ndarray = require("ndarray")
var mesh = require("ao-mesher")

var voxels = ndarray(new Uint16Array(7 * 7 * 7), [7, 7, 7])
voxels.set(3, 3, 3, 1 << 15 | 1)

var legacy = mesh(voxels)        // mask-labelled greedy merge
var exact  = mesh.exactAO(voxels) // field-exact merge, same byte format
```

A runnable staircase example that writes an ASCII PLY with the AO intensity as
the per-vertex grey colour lives in `examples/staircase-ply.js`:

    node examples/staircase-ply.js staircase.ply

## Tests

    sh support/target.sh

# Credits
(c) 2013 Mikola Lysenko. MIT License
