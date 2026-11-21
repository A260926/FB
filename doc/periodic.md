# Periodic axes for curves and surfaces

Periodic splines are first-class objects in BSplineLib: periodicity is an
explicit per-axis property of the `ParameterSpace`, carried by `BSpline` and
`Nurbs`, and preserved through evaluation, knot insertion, seam rotation,
copy/move and IGES input/output. A spline that merely looks closed because
its first and last control points coincide is **not** periodic unless it is
constructed with a periodic mode.

## Construction

The old constructors are unchanged and default to non-periodic (clamped)
axes. Pass explicit modes to mark individual axes periodic; the two axes of
a surface are independent.

```cpp
using namespace bsplinelib;
using parameter_spaces::ParameterSpace;
using parameter_spaces::PeriodicModes;
using parameter_spaces::Periodicity;

// curve
ParameterSpace<1>::KnotVectors_ kv{...};
ParameterSpace<1>::Degrees_ degrees{2};
PeriodicModes<1> curve_modes{Periodicity::Periodic};
auto ps = std::make_shared<ParameterSpace<1>>(kv, degrees, curve_modes);
auto curve = std::make_shared<splines::Nurbs<1>>(ps, weighted_space,
                                                  curve_modes);

// surface: U periodic, V clamped
PeriodicModes<2> surface_modes{Periodicity::Periodic,
                                Periodicity::NonPeriodic};
```

Supported configurations are parametric dimensionality 1 (curve) or 2
(surface), geometric dimensionality 3, and degrees 2 or 3. Periodic and
clamped axes may be mixed freely within one surface.

## Representation conventions

A periodic axis of degree `p` is described by a **fundamental knot
multiset**

```
a = t(0) <= t(1) <= ... <= t(m-1) < b = a + L
```

where `b` is not part of the multiset, `m > p`, and `L > 0` is the period
length. The infinite knot column satisfies the translation relation

```
t(i + m) = t(i) + L                       for every integer i.
```

The finite storage kept in the ordinary `KnotVector` contains the
`m + 2p + 1` knots `t(-p), ..., t(m + p)` and the spline holds `m + p`
control points. The last `p` control points are periodic copies of the first
`p`; for a `Nurbs` the copied data are the complete **homogeneous** control
points (weighted coordinates and weights). Construction validates

* the knot extension `t(i + m) = t(i) + L` over the finite storage,
* that the control net dimensions match the parameter space,
* that the padding control points (and homogeneous components) are exact
  periodic copies, and
* for rational splines, that every weight is finite and strictly positive.

Numeric limits: absolute knot/parameter values do not exceed `1e6`, the
period length is at least `1e-4`, and every nonzero knot spacing is at least
`1e-6`. Fundamental knot multiplicity never exceeds the degree.

The shared logic for the knot column, domain normalization and validation
lives in `Sources/BSplineLib/ParameterSpaces/periodic_axis.{hpp,cpp}`.

## Evaluation

`Evaluate`, `EvaluateDerivative` and `operator()` accept any finite real
parameter. On every periodic axis the parameter is mapped into the
half-open canonical interval `[a, b)`; a value exactly at `b` returns the
`a` side. Evaluation then wraps control point indices onto the fundamental
controls rather than constructing a temporary clamped spline.

Derivatives are taken with respect to the **original real parameter**
(wrapping is a pure translation, so the local derivatives are unchanged),
internal repeated knots take the right-hand span, and total derivative
orders up to 2 are supported.

## Knot insertion

`InsertKnot` works on periodic axes. Any parameter periodic-equivalent to a
fundamental knot addresses the same orbit; each call inserts the whole
translation orbit into the infinite knot column, then rebuilds the finite
padding knots and the periodic (homogeneous) control net. The degree is
unchanged and fundamental multiplicity is capped at the degree, so excess
requests are clipped. Insertion at the seam `a` is supported.

Positions, and first- and second-order derivatives inside smooth patches,
are unchanged after insertion at every real parameter. Repeated alternating
U/V insertion on a surface never loses the periodic state of either axis.

## Seam rotation

```cpp
curve->RotatePeriodicSeam(axis, s);
```

`s` must be periodic-equivalent to an existing fundamental knot. The
canonical interval becomes `[s, s + L)`; only the ordering of the knot
orbits and the control net changes, so the geometry at every original real
parameter is invariant. Arbitrary parameters are rejected (no refitting).
The public spline type is unchanged.

## Unsupported modifications

On a periodic axis, `RemoveKnot`, `ElevateDegree` and `ReduceDegree` throw
`std::logic_error` before modifying anything. The same operations on the
other (clamped) axes keep working and preserve the periodic padding. Failed
validations, seam rotations or allocations leave the knot vectors and
control net untouched.

## IGES 126/128

Periodic splines write the standard 126 (curve) / 128 (surface) parameter
data followed by a vendor trailer that stores the genuine per-axis
periodicity flags and the canonical parameter ranges `[a, b)`. On read the
trailer restores the periodic metadata, so the reloaded spline can continue
with knot insertion and seam rotation. Files without the trailer — including
ordinary clamped splines that happen to be geometrically closed — are never
promoted to periodic.
