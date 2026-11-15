# Periodic boundary pairs for MMGS

This feature lets a single MMGS surface mesh carry two boundary chains that
are geometric copies of one another through a rigid motion
`x_b = R x_a + t`. During remeshing the two chains are edited together so the
mesh stays directly stitchable across the periodic interface: an edge split,
an edge collapse or a tangential relocation accepted on one chain is applied
to the partner chain with the same operator, and a candidate rejected on
either side is rejected on both, leaving the mesh untouched there.

## Public interface

Declared in `src/mmgs/libmmgs.h`, implemented in `src/mmgs/periodic_s.c`.

```c
int MMGS_Set_periodicPair(MMG5_pMesh mesh, MMG5_int n,
    const MMG5_int a[], const MMG5_int b[],
    const double R[9], const double t[3], int closed);

int MMGS_Get_periodicPair(MMG5_pMesh mesh, MMG5_int capacity,
    MMG5_int a[], MMG5_int b[], MMG5_int *n);
```

* `a` and `b` are the two chains, given in matching order: `b[k]` is the
  image of `a[k]` through the rigid motion. `n` is the number of points per
  chain (at least 2 for an open chain, at least 3 for a closed chain).
* `R` is a row-major 3x3 matrix; it must be finite, orthogonal and have a
  positive determinant (a rotation). `t` is the translation.
* `closed == 1` makes the last node connect back to the first one on each
  chain; `closed == 0` keeps the two end nodes fixed.
* Every consecutive pair of nodes (and the last-to-first pair of a closed
  chain) must be a real open edge of the triangulation, point numbers must be
  valid and unique inside a chain, and the two chains cannot share a point.
* The mapped points must coincide within `1e-10 * max(1, bbox_diagonal)`.

`Set` validates everything before touching an existing configuration, so an
invalid call keeps any previously registered pair. Calling `Set` with `n == 0`
clears the configuration. The pair is owned by the mesh: it is released with
`MMGS_Free_all`, re-indexed when `MMGS_packMesh` compacts the point table, and
dropped when `MMGS_Set_meshSize` rebuilds the mesh.

`Get` always writes the required (currently compacted) chain length through
`n` first. It returns `0` without touching `a`/`b` when `capacity` is too
small, otherwise it fills the two arrays in chain order and returns `1`. When
no pair is registered it reports length `0`.

## Remeshing behavior

Only finite, strictly positive **scalar** target edge lengths are accepted.
A tensorial metric makes `MMGS_mmgslib` return `MMG5_STRONGFAILURE` before any
modification. The level-set discretization entry points are not supported.

* **Common size field.** After the size map is built, the two points of every
  pair share `min(h(a[k]), h(b[k]))`. New points receive the common size
  interpolated by arc length along the original chain.
* **Split.** A chain edge that needs refinement creates a point on each side,
  positioned through the rigid motion. Both candidates must pass the standard
  bulge/quality simulation (`MMGS_simbulgept`) before either topology change is
  committed. A rejected candidate removes both provisional points.
* **Collapse.** Both chain edges are simulated read-only through the regular
  `chkcol` geometry/quality/topology checks, with the local edge orientation
  chosen so the same chain slot is removed on both sides. Only when both sides
  accept are the two collapses committed. Open-chain endpoints are never
  removed.
* **Tangential move.** Ridge points move along their chain with the regular
  `movridpt` operator; the partner point follows through the rigid motion and
  both balls are re-validated. A rejected move restores both points.
* **Reference polylines.** The chains captured at the start of the run are
  kept as immutable corner anchors. Every moved or inserted chain point is
  checked to stay on the corresponding reference segment, so points cannot
  drift past an original corner.
* **No required seam.** The chains carry the ordinary geometric boundary
  tags; none of their points or edges is globally required. Interior edits
  next to a seam are still governed by the standard adjacency checks.

The `noinsert`, `noswap` and `nomove` options keep their meaning. With
`noinsert` the seam cannot gain points, so its sample count is preserved.

## Files

* `src/common/libmmgtypes.h` — `MMG5_PeriodicPair` structure and the `per`
  field of `MMG5_Mesh`.
* `src/mmgs/periodic_s.c` — public API and all paired operators/hooks.
* `src/mmgs/libmmgs_private.h` — internal hook prototypes.
* `src/mmgs/libmmgs.c` — run activation, tensor rejection, pack re-indexing.
* `src/mmgs/mmgs1.c` — split/collapse/move hook call sites and common sizes.
* `src/mmgs/API_functions_s.c`, `src/mmgs/variadic_s.c` — lifecycle.
* `tests/periodic_target.c` — target test suite (groups `paired_edits`,
  `rejection`, `lifecycle`).
* `libexamples/mmgs/periodic_headless.c` — headless end-to-end example that
  generates a periodic strip, remeshes it and saves the result as a Medit
  mesh.

## Build and test

```sh
bash task_support/run.sh baseline   # original production path, must stay green
bash task_support/run.sh target     # builds the library, example and tests
```
