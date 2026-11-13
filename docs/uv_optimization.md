# UV optimization {#uv_optimization}

When a non-flipping UV chart already exists and its boundary shape has
been laid out by hand, recomputing a full parameterization (for example
with harmonic_parameterization() or lscm_parameterization()) is not an
option, because those entries reassign the boundary.
pmp::optimize_uv() instead reduces the interior texture stretch of an
existing chart while keeping every pinned vertex fixed.

## Objective

The energy is defined from the metric of the 3D surface, never from an
XY projection. For each oriented triangle, the 3D edge vectors are
expressed in a local 2D reference frame

\f[
  q_0 = (0,0),\quad
  q_1 = (|p_1-p_0|,0),\quad
  q_2 = \bigl((p_2-p_0)\cdot e_1,\;
       |(p_2-p_0)-((p_2-p_0)\cdot e_1)e_1|\bigr),
\f]

with \f$e_1=(p_1-p_0)/|p_1-p_0|\f$. Writing
\f$D=[q_1-q_0,q_2-q_0]\f$, \f$U=[u_1-u_0,u_2-u_0]\f$ and
\f$J=U D^{-1}\f$, the objective is the area-weighted symmetric Dirichlet
energy normalized by the total surface area

\f[
  E=\frac{\sum_f A_f\bigl(\|J_f\|_F^2+\|J_f^{-1}\|_F^2\bigr)}
          {\sum_f A_f}.
\f]

All connected components share this single normalized objective; they
are not normalized separately and then averaged. Translating or rotating
the UV coordinates leaves the energy unchanged, and an
orientation-preserving isometry has energy 4. Every triangle is required
to satisfy \f$\det(J_f)>0\f$. The algorithm only guarantees this local
per-face orientation, not the absence of overlaps between different
parts of a chart.

## Pinned vertices

Only the `VertexProperty<TexCoord>` named `"v:tex"` is modified. The
following vertices stay fixed at the coordinates they hold when the call
begins:

- every vertex on a boundary loop, and
- every vertex whose existing `VertexProperty<bool>` named
  `"v:uv_fixed"` is set to true.

The fixed property is never created when it is absent. Each connected
component that contains faces must pin at least two vertices with
distinct UV positions; otherwise InvalidInputException is thrown.
Isolated vertices are retained and left untouched.

## Solver and failure behavior

Each iteration assembles the analytic gradient and a sparse
Gauss-Newton approximation of the Hessian of the true objective, solves
a sparse LDLT system (with Levenberg-Marquardt regularization when the
pure Gauss-Newton step is not a usable descent direction), and performs
an Armijo line search. Before evaluating the energy, every trial
direction is bounded per triangle by the first positive zero of
\f$\det(J_f(t))\f$; the helper
pmp::detail::uv_positive_step_limit() computes this root from the
quadratic determinant polynomial, and a 1% safety margin keeps every
accepted state strictly inside the positive-determinant region. This
catches directions whose determinant is positive at both endpoints but
crosses zero in between.

Invalid input (missing or non-finite UVs, non-triangle faces, zero or
non-positive initial Jacobian determinants, non-finite coordinates,
zero `max_iterations`, or a non-positive/non-finite tolerance) is
rejected before any UV coordinate changes. An unrecoverable numerical
failure throws SolverException and restores every UV coordinate to its
value on entry. Reaching the iteration limit or exhausting the line
search instead returns the current valid state with `converged` set to
false; only a final free-variable gradient norm at or below the
tolerance reports convergence.

The accompanying example `uv_optimization` builds an annular chart and
writes its UVs to OBJ before and after the optimization.
