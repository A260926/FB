// Copyright 2026 the Polygon Mesh Processing Library developers.
// SPDX-License-Identifier: MIT

#pragma once

#include "pmp/surface_mesh.h"

#include <array>
#include <vector>

namespace pmp {

//! \addtogroup algorithms
//! @{

//! Result of a fixed-boundary UV optimization run.
//! \sa optimize_uv
struct UvOptimizationResult
{
    //! Energy at the input state and after every accepted step
    //! (size is \ref accepted_steps + 1), finite and non-increasing.
    std::vector<double> energies;

    //! Number of actually accepted optimization steps.
    unsigned int accepted_steps{0};

    //! Whether the final gradient norm did not exceed the requested tolerance.
    bool converged{false};

    //! Infinity norm of the normalized objective gradient at the final state,
    //! taken over all free UV components.
    double gradient_norm{0.0};
};

namespace detail {

//! First positive zero of \f$\det(J + t\,dJ)\f$ for \f$t \ge 0\f$.
//! \details Both matrices are given row-major as four doubles:
//! \f$J=(a,b;c,d)\f$ means \c J == {a,b,c,d}.
//! \pre All entries are finite and \f$\det(J)>0\f$.
//! \return the smallest positive root of the determinant polynomial,
//! or positive infinity if no positive root exists.
double uv_positive_step_limit(const std::array<double, 4>& J,
                              const std::array<double, 4>& dJ);

} // namespace detail

//! \brief Optimize an existing UV chart while keeping its boundary fixed.
//! \details The objective is the symmetric Dirichlet energy
//! \f[
//!   E = \frac{\sum_f A_f\,(\|J_f\|_F^2 + \|J_f^{-1}\|_F^2)}{\sum_f A_f},
//! \f]
//! where for each oriented triangle \f$(p_0,p_1,p_2)\f$ the per-face
//! reference frame is built directly from the 3D geometry:
//! \f$q_0=(0,0)\f$, \f$q_1=(|p_1-p_0|,0)\f$,
//! \f$q_2=((p_2-p_0)\cdot e_1,\ |(p_2-p_0)-((p_2-p_0)\cdot e_1)e_1|)\f$
//! with \f$e_1=(p_1-p_0)/|p_1-p_0|\f$, and \f$J=U D^{-1}\f$ for
//! \f$D=[q_1-q_0,q_2-q_0]\f$, \f$U=[u_1-u_0,u_2-u_0]\f$. The energy is
//! invariant to translation and rotation of the UV chart and equals 4 for
//! an isometry. Only the vertex property \c "v:tex" is modified.
//!
//! Every vertex on a boundary loop, together with every vertex flagged in
//! the optional \c "v:uv_fixed" property, stays fixed component-wise; the
//! fixed property is never created by this function. All remaining UV
//! coordinates are optimized with a sparse Newton-type solver and a
//! descent line search. Before each trial step the quadratic determinant
//! \f$\det(J(t))\f$ of every face is analyzed and the step is capped to a
//! strictly positive interval (with a 1% safety margin), so no face can
//! flip or degenerate.
//!
//! Multiple connected components share one objective normalized by the
//! total area. Isolated vertices are preserved. The mesh topology, point
//! positions, handles and all other properties (including deleted-element
//! holes) are left untouched.
//!
//! \pre The \c "v:tex" vertex property already exists with finite UVs.
//! \pre The mesh consists of finite, non-degenerate, oriented triangles
//! (isolated vertices are allowed); every initial face has \f$\det J>0\f$.
//! \pre Each component containing faces has at least two fixed vertices
//! with distinct UV positions.
//! \param mesh The mesh whose \c "v:tex" property is optimized in place.
//! \param max_iterations Maximum number of accepted optimization steps.
//! \param tolerance Convergence threshold on the infinity norm of the
//! normalized gradient over all free UV components.
//! \throw InvalidInputException if any precondition is violated.
//! \throw SolverException if the numerical search fails irrecoverably;
//! in that case all UV coordinates are restored to their input values.
//! \ingroup algorithms
UvOptimizationResult optimize_uv(SurfaceMesh& mesh,
                                 unsigned int max_iterations = 100,
                                 double tolerance = 1e-8);

//! @}

} // namespace pmp
