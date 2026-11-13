// Copyright 2026 the Polygon Mesh Processing Library developers.
// SPDX-License-Identifier: MIT

#pragma once

#include <array>
#include <vector>

#include "pmp/surface_mesh.h"

namespace pmp {

//! \addtogroup algorithms
//! @{

//! Result of a constrained UV optimization run.
//! \sa optimize_uv
struct UvOptimizationResult
{
    //! Objective value at the input state and after every accepted step,
    //! so that <tt>energies.size() == accepted_steps + 1</tt>.
    std::vector<double> energies;

    //! Number of line-search steps that actually lowered the objective.
    unsigned int accepted_steps{0};

    //! Whether the final free-gradient norm is at or below the tolerance.
    bool converged{false};

    //! Infinity norm of the normalized objective gradient over all free UV
    //! components at the final state. Zero when there are no free variables.
    double gradient_norm{0.0};
};

namespace detail {

//! First positive zero of \f$\det(J+t\,dJ)\f$.
//! \details The two \f$2\times2\f$ matrices are passed row-major as four
//! doubles each. The function returns the smallest strictly positive root
//! of the quadratic determinant polynomial, or positive infinity if the
//! determinant stays positive for every positive \f$t\f$ (or has no
//! positive real root).
//! \pre All eight entries are finite and \f$\det(J) > 0\f$.
double uv_positive_step_limit(const std::array<double, 4>& J,
                              const std::array<double, 4>& dJ);

//! Analytic gradient of the normalized UV objective over all vertex UV
//! components (fixed vertices included), in vertex order as
//! <tt>(u0x, u0y, u1x, u1y, ...)</tt>. Read-only diagnostic helper; the
//! mesh is not modified. The mesh must satisfy the optimize_uv()
//! preconditions.
std::vector<double> uv_objective_gradient(const SurfaceMesh& mesh);

} // namespace detail

//! Optimize an existing UV chart without moving its pinned vertices.
//! \details The objective is the area-weighted symmetric Dirichlet energy
//! of the map from the 3D surface (measured with per-triangle intrinsic
//! reference coordinates) into the UV plane:
//! \f[
//!   E(U)=\frac{\sum_f A_f\,\bigl(\|J_f\|_F^2+\|J_f^{-1}\|_F^2\bigr)}
//!               {\sum_f A_f},
//! \f]
//! where for each oriented triangle \f$J_f=U_fD_f^{-1}\f$ maps its local
//! 2D reference frame to the texture coordinates and \f$A_f\f$ is the 3D
//! triangle area. All connected components contribute to one objective
//! normalized by the total area. An orientation-preserving isometry has
//! energy 4; translating or rotating the UV coordinates leaves the energy
//! unchanged.
//!
//! Only the VertexProperty<TexCoord> named \c "v:tex" is modified. Every
//! vertex on a boundary loop stays fixed at its coordinates at entry, and
//! an existing VertexProperty<bool> named \c "v:uv_fixed" may pin further
//! interior vertices. The fixed property is never created when absent.
//! Every connected component containing faces needs at least two pinned
//! vertices with distinct UV positions.
//!
//! Each iteration assembles the analytic first derivative and a sparse
//! Gauss-Newton approximation of the second derivative of the true
//! objective, solves for a search direction with a regularized sparse
//! LDLT factorization, and performs a descent line search. The trial
//! interval is bounded per triangle by the first positive zero of
//! \f$\det(J_f(t))\f$ (with a 1% safety margin), so every accepted state
//! keeps \f$\det(J_f)>0\f$ on all faces; only local per-face orientation
//! is guaranteed, not absence of global chart overlaps.
//!
//! \param mesh The input triangle mesh. Must already carry finite
//!     per-vertex \c "v:tex" coordinates with strictly positive initial
//!     Jacobian determinants.
//! \param max_iterations Maximum number of accepted optimization steps.
//!     Must be greater than zero.
//! \param tolerance Convergence threshold for the infinity norm of the
//!     normalized free-variable gradient. Must be finite and positive.
//! \throw InvalidInputException if any precondition is violated. No UV
//!     coordinate is modified in that case.
//! \throw SolverException if the numerical search fails unrecoverably.
//!     On failure all UV coordinates are restored to the input state.
//! \ingroup algorithms
UvOptimizationResult optimize_uv(SurfaceMesh& mesh,
                                 unsigned int max_iterations = 100,
                                 double tolerance = 1e-8);

//! @}

} // namespace pmp
