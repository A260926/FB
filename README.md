# libnurbs

NURBS algorithm library for modern C++. Slowly under development.

## Features

- [x] BSpline basis evaluation.
- [x] NURBS curve & surface interpolation and derivative evaluation.
- [x] Search for parameter of point on a curve or surface using the BFGS method.
- [x] Knot insertion(refinement) and removal.
- [x] Degree elevation and reduction.
- [x] Certified normal-offset surface fitting (`FitNormalOffset`) with an
      independent continuous error certificate (`CertifyNormalOffset`).
- [ ] NURBS curve & surface fitting.
- [ ] ...

## Normal-offset fitting

Given a source NURBS surface `S(u,v)` and a signed distance `d`, the target of
the offset operation is the continuous surface

```
T(u,v) = S(u,v) + d * normalize(S_u(u,v) x S_v(u,v))
```

with a positive `d` following the existing `S_u x S_v` orientation. The
implementation lives in:

- `include/libnurbs/Surface/OffsetFit.hpp`, `src/Surface/OffsetFit.cpp`
- `include/libnurbs/Algorithm/OffsetBounds.hpp`, `src/Algorithm/OffsetBounds.cpp`
- shared interval/Bezier machinery in `include/libnurbs/Detail/OffsetDetail.hpp`
  and `src/Detail/OffsetDetail.cpp`.

### Fitting algorithm

`FitNormalOffset(const Surface&, const OffsetOptions&)` produces an owning
bicubic (degree 3 x 3), open-clamped, unit-domain `Surface` whose weights are
all exactly one and whose interior knot multiplicities never exceed two.

1. The source is validated explicitly (3D, degrees 2 or 3 on both axes, open
   clamped unit knot vectors, interior continuity at least C2, strictly
   positive finite weights, the documented coordinate/weight/knot-spacing and
   option ranges) before any legacy kernel routine can assert.
2. The fit starts from the coarsest bicubic basis (a 4 x 4 control net, no
   interior knots). Every control coefficient comes from a real
   tensor-product least-squares solve: the one-dimensional design matrices use
   Householder QR (so the condition number is not squared the way normal
   equations do) and the two triangular factors recover the full control net
   from the exact offset target evaluated on an oversampled grid.
3. If the continuous certificate cannot close, the clamped uniform mesh grows
   by one span on the axis with the largest sampled residual weighted by the
   intrinsic directional curvature; the other axis follows only when it also
   needs resolution. Refinement is therefore anisotropic (an extruded surface
   accumulates knots only along its curved axis) and the maximal dense net is
   never built up front. Each iteration fully re-solves and re-checks the new
   surface, so residual or bound figures are never reused as evidence after a
   geometry change.
4. The owning `Surface` is published only after the independent
   `CertifyNormalOffset` check returns `certified`. A fit is never a shifted
   copy of the source control points and independent patch coefficients are
   never stitched into a control net.

The per-iteration record (`OffsetFitIteration`) reports the knot counts, the
control-point count, the sampling residual, the continuous error bound and the
cross-product lower bound for that iteration's own surface.

### Continuous error enclosure

`CertifyNormalOffset(source, candidate, distance, tolerance, max_boxes)`
performs an independent check that never mutates either surface and never
refits the candidate; subdivision only tightens the proof. It returns
`certified`, `unresolved` or `invalid_input` together with a partition of
`[0,1]^2` into leaf boxes with pairwise disjoint interiors. Each box carries a
non-negative lower bound of `||S_u x S_v||` and a conservative upper bound of
`||candidate - T||` over the whole box.

The bound is an enclosure of the continuous function over the box, never a
maximum over corners, centers or any finite sample set:

1. Each NURBS surface is treated per knot span as a rational
   Bernstein-Bezier patch (source and candidate are brought onto a common knot
   partition by knot insertion). Sub-patches after subdivision come from
   interval de Casteljau splits, and the Bernstein convex-hull property makes
   the coefficient boxes valid whole-box enclosures.
2. Derivatives up to third order come from differentiated Bernstein forms.
   Because a (sub-)patch is parametrized locally on `[0,1]^2`, chain-rule
   factors `1/h_u`, `1/h_v` convert local jets to physical `(u,v)`
   derivatives. Rational derivatives use the same quotient rule as
   `Surface::EvaluateAll`, evaluated with outward-rounded interval arithmetic.
3. The cross product is enclosed by a centered Taylor form (exact center value
   plus interval gradient and a quadratic Hessian remainder), intersected with
   the coefficient hull. The unit normal combines the direct interval quotient
   with a centered Lipschitz ball whose radius follows from
   `dn = (I - n n^T) dG / r`.
4. The error `C - T` uses a second-order centered Taylor form built from the
   exact value and Jacobian at the box center and interval Hessian boxes for
   the remainder, intersected with the convex-hull enclosure. Using the point
   Jacobian makes the remainder `O(h^2)`, so bounds tighten quadratically as
   leaves are bisected.

`certified` requires every leaf to have a strictly positive cross-product
lower bound and an error upper bound not exceeding the tolerance. A box that
cannot prove one of these is bisected on its longest side; the check reports
`unresolved` if the next split would exceed `max_boxes`, and `invalid_input`
for structural or numerical precondition violations.

### Budget states

`OffsetOptions` carries `distance`, `tolerance`, `max_control_points`
(default 256, a hard limit on the total number of published control points)
and `max_boxes` (default 4096, the leaf-box limit of one certification run).

- `success`: an owning surface was produced and independently certified.
- `budget_exhausted`: the control-point or leaf-box budget was reached before a
  certified fit existed. No half-finished surface is returned.
- `invalid_input`: an illegal structure, non-positive weight, non-finite
  parameter or illegal budget was rejected before any kernel assertion.

No computation that exceeds a budget is hidden from the reported statistics,
and the source and candidate surfaces are never modified.

## Dependencies

1. [Eigen3](https://gitlab.com/libeigen/eigen) (for linear algebra)
2. [Catch2](https://github.com/catchorg/Catch2) (for unit tests)
3. [Matplot++](https://github.com/alandefreitas/matplotplusplus) (for visual tests)
4. [Google/Benchmark](https://github.com/google/benchmark) (for benchmark)

## Reference

1. [tinynurbs](https://github.com/pradeep-pyro/tinynurbs)


