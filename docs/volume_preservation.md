# Volume preserving quadric edge collapse

Ordinary QEM edge collapse (`TriEdgeCollapseQuadric`) changes the enclosed
volume of a closed scanned shell: vertices are moved to the unconstrained
quadric minimum, so each collapse lets the surface cut a little volume.
This document describes the optional exact volume preserving mode added to
the existing collapse chain.

## Parameter

`vcg::tri::TriEdgeCollapseQuadricParameter::PreserveVolume` (bool, default
`false`).

```cpp
TriEdgeCollapseQuadricParameter p;
p.OptimalPlacement = true;   // required
p.PreserveTopology = true;   // required
p.PreserveVolume   = true;

LocalOptimization<Mesh> s(m, &p);
s.Init<Collapse>();
s.SetTargetSimplices(targetFaces);
s.DoOptimization();
s.Finalize<Collapse>();
```

Nothing else changes. The candidate ordering, VF adjacency, LinkConditions,
the heap update machinery and the post-collapse quadric merge/neighbourhood
update are all the original ones. The mode is not a separate simplifier and
there is no post-hoc global scaling of the model. When the flag is `false`
the code paths, accepted inputs, candidate results and cleanup are byte for
byte the original behavior (see `checks/baseline.cpp` and the recorded
`checks/default-baseline.txt`).

## What is preserved

For a candidate collapsing `v0` onto `v1` to a final position `x`, the
directed volume of the one-ring before and after the collapse is kept equal.
Directed volume is, per oriented triangle,

```
V = sum_f  a . (b x c) / 6
```

- the two faces shared by `v0` and `v1` are deleted: they occur once in the
  old ring and nowhere in the new ring;
- every other incident face is rewritten so its collapsing endpoint becomes
  `x`; each such face is accumulated exactly once on both sides.

For a surviving face `(x, p, q)` in stored cyclic order,
`x . (p x q)` is linear in `x`, so the new ring volume is an affine function
of `x`. The constraint is therefore a single linear equality

```
n . u = d
```

where `u` is the position in a local frame centered on the edge midpoint
`o = (P(v0) + P(v1)) / 2`, `n = sum (p x q)` over the surviving faces and
`d` is six times the old ring volume in the same frame.

On this hyperplane the placement minimizes the original summed quadric
`Qd(v0) + Qd(v1)`. It is an exact constrained minimum, not a displacement
along a face normal and not a finite-weight volume penalty term; no
regularizer is added to the error function.

## Numerical locality

Signed volumes are sums of triple products. Evaluated in world coordinates,
a model placed near `1e6` loses its volume (and even its sign) to
large-number cancellation. Every ring quantity above — `n`, `d`, the QEM
linear term and the solve — is computed in the midpoint local frame, where
the coordinates have the size of the ring. The world position is reconstructed
only at the end. The quadric matrix itself is translation invariant, while
its linear term is shifted as `b_local = 2 A o + b_world`.

## Applicable inputs

With `PreserveVolume == true`, `Init` validates the mesh *before* building
any candidate, changing any vertex mark or touching any quadric. The
following combinations throw `std::invalid_argument` and leave the mesh
untouched:

- `OptimalPlacement == false` or `PreserveTopology == false`;
- non-finite (`inf`/`nan`) vertex coordinates;
- zero-area / degenerate faces (area at or below a bbox-relative floor);
- open boundaries (an edge with a single incident face);
- non-manifold edges (more than two incident faces) or pinched vertices
  (incident faces not forming a single fan cycle);
- inconsistent orientation across an edge (the two faces do not traverse
  their shared edge in opposite directions).

Supported inputs:

- finite coordinates, strictly positive face area;
- closed, coherently oriented, 2-manifold triangle meshes;
- several mutually disconnected shells in one mesh;
- a shell that is globally inverted (its directed volume is negative);
- self-intersecting geometry is accepted as-is and is not "repaired".

## Rank deficient cases

Both the summed QEM matrix and the constraint can be singular. Examples that
occur in practice are planar rings, plane-only quadrics and the base edge of
a bipyramid. The solve is an SVD null-space reduction, so it never assumes
full rank:

- **Full rank constraint.** The plane's direction space (dimension 2) is an
  orthonormal basis `Z`; the problem reduces to a 2x2 quadratic
  `min z' H z + g' z`. Small singular values of `H` are treated as zero. If
  the gradient has a component in a null direction the objective is
  unbounded and the candidate is rejected; otherwise a finite minimizer is
  returned and, when the minimizer is not unique, the one whose distance to
  the edge midpoint is smallest is chosen.
- **Identically zero equality (`n = 0`, `d = 0`).** The constraint carries no
  information, so the unconstrained QEM minimum is taken with the same
  smallest-singular-value and closest-to-midpoint rules.
- **Incompatible identity equality (`n = 0`, `d != 0`).** No point can
  satisfy it: the single candidate is rejected, the optimization continues
  with the remaining heap, and no coordinate/quadric/mark/VF datum is
  modified by the trial.
- Any solve that yields a non-finite point, or a point that fails the
  equality to a tight local tolerance, likewise rejects only that candidate.

A rejected trial never mutates mesh state: the trial position lives on the
candidate object, `Execute` runs only for accepted candidates, and the
quadric merge, `EdgeCollapser::Do` and the heap/neighbourhood update happen
only afterwards along the original success path.

## Guarantees measured by the tests

The target tests in `tests/volume_preservation.cpp` use independent
reference code (they do not call the production helper) and check, per
shell with initial bbox diagonal `D`:

- single accepted collapse: volume error `<= 1e-10 * D^3`;
- cumulative error after repeated reduction: `<= 1e-8 * D^3`;
- the tolerance is absolute in `D^3`, never divided by the volume, so
  near-zero-volume shells use the same scale;
- on positive-definite, independent-constraint rings the point matches an
  independently solved KKT constrained system to within `1e-8 * D`;
- rank deficient cases verify the closest-to-midpoint choice explicitly,
  not only the preserved volume.

Run them from the source root:

```
bash checks/run-target.sh
```

which prints `VCG_VOLUME_TARGET_OK` after the six functions pass and then
rebuilds and diffs the unmodified default baseline.

## Example

`apps/sample/trimesh_volume_preserve` builds one or two asymmetric closed
shells (the second can be globally inverted), simplifies them with the flag
enabled, writes before/after OBJ files and prints each shell's directed
volume before and after:

```
trimesh_volume_preserve [before.obj] [after.obj] [target_faces] [two|one]
```
