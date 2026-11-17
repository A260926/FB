# Knot removal: tolerance semantics

`Curve::RemoveKnot`, `Surface::RemoveKnotU`, `Surface::RemoveKnotV` and the
shared `KnotRemoval` entry point all take a `tolerance` argument. This note
defines exactly what that number means, how a request is accepted or rejected,
and how numerical round-off is handled.

## Definition

`tolerance` is an upper bound on the **3D Euclidean distance between two
evaluated objects at the same parameter value**:

```
for every u in [0, 1]:           | C_input(u)  - C_returned(u)  | <= tolerance
for every (u, v) in [0, 1]^2:    | S_input(u,v) - S_returned(u,v)| <= tolerance
```

The reference is always the object **as it was passed to this call**, not the
original model from some earlier edit. In particular:

* After a partial success, a later call is measured against the partially
  reduced object that was passed in — not against the very first surface.
* Errors are never accumulated or relaxed across successive edits. Each call
  carries its own bound.

A request of `times` is answered with the largest `t <= times` such that `t`
removals still satisfy the bound. `t` may legitimately be `0` or anything in
between; the return value is the truth, not an echo of the request.

## How acceptance is decided

Removing a knot is a sequence of single-copy removals (Piegl & Tiller A5.8).
For each candidate level `t = 1, 2, ...`:

1. Perform `t` single removals in homogeneous (weighted) coordinates.
2. Re-insert the same `t` copies with Boehm's algorithm. This brings the
   candidate back onto the **original knot basis**, so the input control net and
   the candidate control net are index-for-index comparable. For an exact
   removal this step reproduces the input control net.
3. Compute a **conservative bound** on the geometric error over the entire
   parameter domain from the two control nets (see below).
4. Accept level `t` only if `bound <= tolerance + roundoff_slack`; otherwise
   stop and return the last accepted level.

Because the comparison is made against the *input* net after refining the
candidate back to its basis, a partial result is not silently re-based: the
count that is returned is the largest count for which the whole object stays
within tolerance.

## The bound

For a partition-of-unity basis (B-splines, and tensor products of them), with
homogeneous control points `(w_i C_i, w_i)` and `(v_i D_i, v_i)` on a common
basis, and `W = sum N_i w_i`, `V = sum N_i v_i`, the identity

```
C - D = (A/W)(V - W)/V + (A - B)/V,     A = sum N_i w_i C_i,  B = sum N_i v_i D_i
```

gives the bound

```
|C - D| <= max|w_i (C_i - c0)| * max|w_i - v_i| / (min w * min v)
         + max|w_i C_i - v_i D_i - (w_i - v_i) c0| / min v
```

where `c0` is the first reference control point (used only to make the estimate
translation-invariant). All maxima are over the **whole control net**, so the
result bounds the distance at every parameter value — a finite set of sampled
points is never used as the acceptance criterion. For a tensor-product surface
the maxima run over every control row, and the denominator uses the shared
`W` and `V`; checking each row as an independent curve is not sufficient,
because a row with very different weights can be amplified by the common
rational denominator.

## Numerical round-off

The bound is computed in `long double` and compared as

```
bound <= tolerance + roundoff_slack
```

with

```
roundoff_slack = 64 * epsilon(double) * (1 + control-net diameter)
```

The slack is a handful of ulps. It is absolute (knots live in `[0, 1]`) and is
derived only from translation- and rotation-invariant quantities, so

* translating or rigidly rotating the model never changes which removals are
  accepted;
* uniformly scaling all weights by a positive factor never changes the count
  or the geometry;
* a genuine inexactness (a real perturbation of the model) is far above the
  slack and is still rejected.

## Zero and partial results

* On `t == 0` the input is left bit-for-bit untouched, and the returned object
  is a fully independent copy identical to the input. `KnotRemoval` leaves the
  passed-in knot vector and control points at their original values.
* On success the control points and knots each shrink by exactly `t`.
* Degree, the remaining knots, and the parameter correspondence are preserved.
* Control points continue to use the public Cartesian-plus-weight layout
  (`xyz` Cartesian, `w` weight); the arithmetic itself is homogeneous.

## Surface-specific rules

`RemoveKnotU` / `RemoveKnotV` are symmetric. All control rows are kept on one
common knot basis: the reported `t` is the largest prefix count that **every**
row can reproduce, and the returned net shrinks by exactly that amount on the
chosen axis. Rows are never left with different reduction counts, and the net
is never pre-shrunk by the requested count and then partially filled. Where a
row can only reproduce part of the prefix, the common basis stops at the
largest prefix all rows share (so capability differences produce a real partial
result, not zero).

## Out of scope

Only 3D, finite positive weights, clamped knot vectors with parameter domain
`[0, 1]`, interior knots, and `times >= 1` not exceeding the knot multiplicity
are supported. Removal is a local edit; whole-surface refitting or degree
reduction is not used.
