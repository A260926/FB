# UV optimization {#uv_optimization}

PMP includes two parameterization algorithms,
pmp::harmonic_parameterization() and pmp::lscm_parameterization(), that
compute a full chart from scratch and freely place its boundary. Some
workflows instead start from an existing, non-flipped UV chart whose
boundary loops and pinned interior vertices were arranged by hand and must
not move. pmp::optimize_uv() addresses this case: it reduces the internal
texture distortion of the existing chart while keeping every boundary
vertex and every vertex flagged in the optional `v:uv_fixed` property
fixed.

## Objective

The objective is defined from the metric of the input 3D surface, never
from an XY projection. For each oriented triangle \f$(p_0,p_1,p_2)\f$ a
local 2D reference frame is built directly from its 3D geometry:

\f[
  q_0=(0,0),\quad q_1=(|p_1-p_0|,0),\quad
  q_2=\big((p_2-p_0)\cdot e_1,\ |(p_2-p_0)-((p_2-p_0)\cdot e_1)e_1|\big),
\f]

where \f$e_1=(p_1-p_0)/|p_1-p_0|\f$. With
\f$D=[q_1-q_0,q_2-q_0]\f$, \f$U=[u_1-u_0,u_2-u_0]\f$ and
\f$J=U D^{-1}\f$, and with \f$A\f$ the 3D triangle area, the normalized
symmetric Dirichlet energy is

\f[
  E=\frac{\sum_f A_f\,(\|J_f\|_F^2+\|J_f^{-1}\|_F^2)}{\sum_f A_f}.
\f]

The energy is invariant to translating or rotating the UV chart, equals 4
for a local isometry, and grows without bound as any triangle approaches a
flip. All components of a multi-component mesh contribute to one objective
normalized by the total area; the per-face area weights are never omitted.

Only non-fixed vertices are optimized. Each iteration assembles the exact
analytic gradient and a sparse (Newton-type) second-order system locally
per face, solves it with a sparse Eigen factorization plus adaptive
regularization, and performs a descent line search on the real objective.

## Boundary handling and flips

All vertices on boundary loops are fixed automatically, and the optional
`v:uv_fixed` vertex property pins additional interior vertices. The
property is only read; it is never created. Each component containing
faces must have at least two fixed vertices with distinct UV positions,
otherwise pmp::InvalidInputException is thrown.

Before every trial step the quadratic polynomial
\f$\det(J+t\,dJ)\f$ of every face is analyzed with
pmp::detail::uv_positive_step_limit() to find the largest interval
starting at \f$t=0\f$ on which all faces stay strictly positive. The step
length is capped inside that interval with a 1% safety margin, so a face
can never degenerate or flip even when the determinant is positive at
both endpoints of a candidate step but negative in between. Only
per-face local orientation is guaranteed; overlaps between separate
charts are not detected.

## Input requirements and failure behavior

The `v:tex` vertex property must already exist with finite coordinates;
neither harmonic nor LSCM parameterization is invoked automatically, and
the two existing parameterization entry points are unchanged. The mesh
must consist of finite, non-degenerate, oriented triangles (isolated
vertices are allowed), and every initial face must satisfy
\f$\det J>0\f$. Non-triangle faces, non-finite positions or UVs, zero or
negative initial determinants, `max_iterations == 0`, and non-positive or
non-finite tolerances are rejected before any UV coordinate is modified.

The returned pmp::UvOptimizationResult reports the energy at the input
state and after every accepted step (so `energies.size() ==
accepted_steps + 1`, all entries finite and non-increasing), the number
of accepted steps (never above `max_iterations`), whether the gradient
tolerance was reached, and the infinity norm of the normalized objective
gradient over the free UV components. Reaching the iteration budget or
failing to find an acceptable step returns the current valid result
without claiming convergence. If the numerical factorization, direction
or energy becomes non-finite and cannot be recovered,
pmp::SolverException is thrown and every UV coordinate is restored to its
value before the call.

The mesh topology, element handles, point positions and all user
properties are left untouched; deleted-element holes do not require
garbage collection and isolated vertices are preserved.

## Example

The `uv_optimization` example builds an annular triangle grid, starts
from a distorted but positive chart, runs the optimization and writes two
OBJ files containing per-vertex texture coordinates, one for the input
chart and one for the optimized chart.
