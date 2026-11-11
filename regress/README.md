# Boolean spatial regression

Order-independent regression for the geom3 boolean pipeline: lazy affine
transforms → BSP boolean operations → retessellate.

## Run

```sh
bash regress/check_booleans.sh          # gated entry point
node --test regress/boolean-regression.test.cjs   # direct
```

Only Node.js is required; no repository-wide install or frontend toolchain.

## Files

- `boolean_reference.cjs` — the independent reference implementation. It never
  calls the library under test and uses no CSG library.
- `boolean-regression.test.cjs` — the `node:test` suite, which drives the
  library only through the public entry `packages/modeling/src`.
- `check_booleans.sh` — runs the suite and requires the three named suites
  `boolean-occupancy`, `boolean-cavity-winding` and `boolean-lazy-affine` to
  actually run and pass.
- `boolean_baseline.cjs` — the pre-existing simple volume sanity script.

## Reference method

1. Every model is a finite boolean expression (union / intersect / subtract)
   over axis-aligned cuboids.
2. The six boundary planes of every cuboid partition space into open
   axis-aligned grid cells. The expression has a constant truth value inside
   each cell, so the reference volume is the exact sum of the volumes of the
   cells whose centers evaluate to true. Sets lying on grid planes have zero
   volume, so face- and edge-contact cases need no special handling.
3. Reference inside/outside answers for probes come from evaluating the same
   expression at the probe point.
4. Output polygons are checked independently: each convex polygon is fan
   triangulated and a Moller–Trumbore ray cast decides inside/outside by
   parity. Ray intersections at the same distance are merged before counting,
   so a quad split into two triangles (or coincident coplanar faces) cannot be
   counted twice along a shared edge.
5. Winding is checked by accumulating the signed tetrahedron volume of the
   triangulated output. A closed outward-oriented surface equals the occupied
   volume even under mirroring (mirrored vertex rings get flipped), while an
   inward-facing cavity wall would change the sum despite a correct-looking
   bounding box.

Expected values are never produced with `measureVolume`, BSP, retessellate or
another CSG library. The library's `measureVolume` appears only as a quantity
under test.

Every expression is compared under one shared, non-singular, non-commuting
affine combination (non-uniform scale → mirror X → shear → translation):
operands transformed before the boolean versus the boolean result transformed
afterwards. The reference world volume is the model-space volume times the
absolute determinant; probes are mapped through the same matrix. Triangle
counts are not compared.

## Excluded boundary cases

- probes or grid cells on cuboid boundary planes and at triangle vertices
  (the inside/outside convention on a boundary is undefined here; such sets
  have zero volume anyway);
- curved primitives, rotated boxes and non-affine or singular transforms;
- polygon/triangle count and vertex ordering comparisons — only occupied
  space, probe verdicts and signed volume are asserted;
- internal cache byte stability: the library may materialize lazy transform
  matrices in place; the reuse tests assert geometric equivalence only.
