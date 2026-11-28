# Knot removal: tolerance semantics and numerical notes

This note documents the contract of the knot-removal path:

* `libnurbs::KnotRemoval(KnotVector&, std::vector<Vec4>&, ...)` (`include/libnurbs/Algorithm/KnotRemoval.hpp`)
* `Curve::RemoveKnot(u, times, tolerance)`
* `Surface::RemoveKnotU(u, times, tolerance)` / `Surface::RemoveKnotV(u, times, tolerance)`

It covers what `tolerance` means, how the decision is made, and how numerical
rounding is handled.  The accompanying example is
`examples/surface_edit/main.cpp`; the regression suite is
`checks/test_knot_removal.cpp` (run with `bash task_support/run.sh target`).

## 1. What `tolerance` means

> `tolerance` is an upper bound on the **3D Euclidean distance between the object
> returned by this call and the object as it was at the start of this call**, at
> every parameter value in the domain.

* For a curve, the domain is `[0, 1]` in `u`.
* For a tensor-product surface, the domain is the full square `[0, 1] x [0, 1]`
  in `(u, v)`; the surface bound couples all control rows through the shared
  rational denominator (see §3).
* The comparison is "this result vs. the input of *this* call".  It never
  accumulates across calls: a second removal is bounded relative to the surface
  that entered the second call.  `checks/test_knot_removal.cpp`
  (`error_relative_to_input`) exercises this.

The default in the public headers is `1e-12`.  Because the result control points
are computed in floating point, a mathematically exact removal already produces
rounding noise; use a slightly looser tolerance (`1e-9`…`1e-10`) when the input
is "exact up to rounding", as in the bundled fixtures.

## 2. What is returned

* The return value `t` is the number of knot copies **actually** removed:
  `0 <= t <= min(times, multiplicity(input, u))`.
* On `t > 0`, the control net and the knot vector each shrink by exactly `t`
  entries, the degree is unchanged, and every other knot and its parameter
  correspondence is unchanged.  `Surface::RemoveKnotU/V` return one common `t`
  for all rows and one common knot basis, never per-row counts.
* On `t == 0`, the returned object is an **independent** copy that is identical
  to the input (control points, knots, degree, grid size).  The source object is
  never modified.  When calling the shared `KnotRemoval` directly with `t == 0`,
  the `KnotVector` and `std::vector<Vec4>` arguments are left exactly as passed.
* Control points and knots are stored/returned in the public Cartesian
  representation `(x, y, z, w)`; internally the algorithm works in homogeneous
  coordinates and converts back at the end.

## 3. How acceptance is decided (conservative geometric bound)

Acceptance does **not** use sampling.  Sample maxima are not upper bounds, so a
fixed sample set could accept a result that is out of tolerance somewhere else.

### Curve

Let `A_i = (w_i * P_i, w_i)` be the homogeneous control points of the input
curve and `B_i` those of the candidate, both expressed over the **same** refine
of the knot vector (the candidate is refined back onto the input basis with
knot insertion; insertion reproduces the candidate exactly over that basis).
The candidate is accepted when

```
max_i |(A_i - w_i*a) - (B_i - v_i*a)| / WsMin
  + max_i |B_i - v_i*a| * max_i |w_i - v_i| / (WsMin * WcMin)
  <= tolerance
```

where `a` is a fixed anchor (the first input control point) and `WsMin`,
`WcMin` are the smallest input and candidate weights.  This is derived from the
rational quotient

```
S - C = (N - M)/Ws + M*(Wc - Ws)/(Ws*Wc),
```

with `N`, `M` the numerator curves of the offset nets, `|N-M|` bounded by the
control-net difference and `|M|`, `|Wc - Ws|` by their control values.  Since
`A`, `B` share a basis, each bound is the maximum over control points and
therefore holds for every parameter in `[0, 1]` — not just at samples.

The anchor cancels the translation of the control net, so translation and rigid
rotation by an isometry do not change the bound; it is also exactly invariant
under a uniform positive scaling of every weight (`w -> c*w`), because `c`
cancels between numerator and denominator.

### Surface

`Surface::RemoveKnotU/V` treats every control row along the removal axis as a
parallel curve over the shared knot vector.  The geometry error of a rational
tensor-product surface is **not** the maximum of per-row curve errors: the
surface denominator is `sum_{i,j} N_i(u) N_j(v) w_{ij}`, which mixes all rows.
The bound above is therefore evaluated on the **whole control grid at once**:

* `A` = homogeneous control points of the input surface,
* `B` = the candidate after refining it back onto the input knot basis in the
  removal direction (so the two grids have the same shape),
* `WsMin`, `WcMin`, `max|w - v|`, `max|A_off - B_off|`, `max|B_off|` taken over
  all grid entries.

The same rational-quotient inequality holds pointwise in `(u, v)` with the
tensor-product basis functions in place of the curve basis, and its control
maxima bound the entire parameter square.  This is why per-row acceptance is not
used to pick a common result: a set of rows can each be individually within
tolerance while the coupled surface is not.

### Choice of `t`

For each row, single-copy removals are computed sequentially (the standard
B-spline removal step); each step records the candidate control net and knot
vector.  The smallest per-row capacity is the largest number of copies that can
be removed on a common basis.  Starting from that common capacity and decreasing
to 1, the production bound is evaluated against the **original input**; the
first `t` whose bound is `<= tolerance` is accepted.  If none qualifies, the
call returns `0` and an identical copy.

Because a request for `t` copies is validated as a whole against the input, a
partial success (`t < times`) is a genuinely valid result, not a sequence of
individually-accepted steps that could drift.

## 4. Numerical rounding

* Knot arithmetic is done in `double`.  The removal "error indicator" used while
  building candidates is only a numerical sanity check; the acceptance decision
  is the bound of §3.
* Exact inverse operations (insert `t` copies, then remove them) are recognized
  because the refined candidate equals the input control net to within floating
  point rounding.  The resulting bound is on the order of `1e-14`–`1e-13` for
  well-scaled data, comfortably below the default `1e-12`.
* If a step produces a non-finite value, a zero/negative weight, or a degenerate
  denominator, that candidate is rejected (it is treated as capacity `0` for
  that row); this never yields a NaN result or a silently wrong geometry.
* `KnotVector::GetMultiplicity` uses a tolerance, so the removal path uses an
  exact equality test against the requested knot value.  The requested value is
  expected to be one of the existing interior knots; if it is absent, the call
  returns `0` and the inputs are unchanged.

## 5. Invariants checked by the regression suite

`checks/test_knot_removal.cpp` registers the `prefix`, `tensor` and
`rational_bound` groups and verifies, among other things:

* zero removals return identical, independent objects and leave the shared
  `KnotRemoval` arguments untouched;
* `t` removals shrink control points and knots by exactly `t`; degree and the
  remaining knots are preserved; the result still evaluates and can be refined
  and reduced again after a rejection;
* surface rows share one basis and one `t`, U and V are symmetric, non-square
  grids and alternating U/V edits work;
* a uniformly scaled positive weight does not change the decision or geometry;
  translation and rigid rotation do not either; exact refinements invert;
* a gently perturbed rational surface is accepted under a loose tolerance and
  rejected under a tight one (not always `0`);
* accepted results satisfy the requested tolerance, measured from the input of
  that call.
