/********************************************************************************
*  This file is part of CinoLib                                                 *
*  Copyright(C) 2016: Marco Livesu                                              *
*                                                                               *
*  The MIT License                                                              *
*                                                                               *
*  Permission is hereby granted, free of charge, to any person obtaining a      *
*  copy of this software and associated documentation files (the "Software"),   *
*  to deal in the Software without restriction, including without limitation    *
*  the rights to use, copy, modify, merge, publish, distribute, sublicense,     *
*  and/or sell copies of the Software, and to permit persons to whom the        *
*  Software is furnished to do so, subject to the following conditions:         *
*                                                                               *
*  The above copyright notice and this permission notice shall be included in   *
*  all copies or substantial portions of the Software.                          *
*                                                                               *
*  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR   *
*  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,     *
*  FITNESS FOR A PARTICULAR PURPOSE AND NON INFRINGEMENT. IN NO EVENT SHALL THE *
*  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER       *
*  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING      *
*  FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS *
*  IN THE SOFTWARE.                                                             *
*                                                                               *
*  Author(s):                                                                   *
*                                                                               *
*     Marco Livesu (marco.livesu@gmail.com)                                     *
*     http://pers.ge.imati.cnr.it/livesu/                                       *
*                                                                               *
*     Italian National Research Council (CNR)                                   *
*     Institute for Applied Mathematics and Information Technologies (IMATI)    *
*     Via de Marini, 6                                                          *
*     16149 Genoa,                                                              *
*     Italy                                                                     *
*********************************************************************************/
#ifndef CINO_VOLUME_PRESERVING_FAIRING_H
#define CINO_VOLUME_PRESERVING_FAIRING_H

#include <cinolib/cino_inline.h>
#include <cinolib/meshes/trimesh.h>

#include <string>
#include <vector>
#include <set>
#include <unordered_set>

namespace cinolib
{

/* VOLUME PRESERVING FAIRING
 *
 * Smooths the vertex distribution of a closed, consistently oriented
 * triangle mesh while keeping every connected shell (component) at its
 * own initial signed volume. Fixed vertices are never displaced. The
 * connectivity of the mesh is never touched, so all vertex/edge/polygon
 * indices and all custom per-element attributes remain valid.
 *
 * ---------------------------------------------------------------------
 * ENERGY
 * ---------------------------------------------------------------------
 * The quantity that is minimized is
 *
 *     E(x) = sum_{ {i,j} in undirected mesh edges } || x_i - x_j ||^2
 *
 * i.e. the plain sum of squared edge lengths. The connectivity is fixed,
 * so the only variables are the coordinates of the free vertices. Every
 * accepted step is guaranteed to make E strictly smaller.
 *
 * ---------------------------------------------------------------------
 * CONSTRAINTS
 * ---------------------------------------------------------------------
 *  - All coordinates listed in `fixed_vertices` keep their exact input
 *    value, component by component. Isolated vertices (no incident
 *    polygon) are preserved as well (they are, de facto, fixed).
 *  - For each shell c, the signed volume
 *
 *        V_c(x) = sum_{ (a,b,c) in shell c } det(a,b,c) / 6
 *
 *    is kept equal to the value it had on input, up to
 *    `options.relative_volume_tolerance` relative error. This holds
 *    shell by shell: a shell is never allowed to shrink just because
 *    another one grows, and no global scaling of the whole model is used
 *    to balance the volumes.
 *  - Before each accepted step every polygon is checked:
 *      * dot(n_old, n_new) > 0, where n = (b-a) x (c-a) is the oriented
 *        area vector of the triangle (no face may flip), and
 *      * ||n_new|| >= 1e-6 * ||n_initial||, with n_initial taken from
 *        the input mesh (no step may squash a triangle below a factor
 *        1e-6 of its original area, which also prevents degenerate
 *        triangles).
 *  - Global self-intersections between distant parts of the surface are
 *    not detected (same limitation as the classical tangential smoother).
 *
 * ---------------------------------------------------------------------
 * SOLVING STRATEGY
 * ---------------------------------------------------------------------
 * The fairing direction and the volume correction are coupled, so a
 * plain per-point tangential move followed by a final global volume fix
 * is not sufficient. At each iteration:
 *
 *  1. For every free vertex i, compute the uniform-Laplacian descent
 *     direction d_i = (mean of neighbours of i) - x_i. This is a descent
 *     direction for E (d_i = -grad_i E / (2 deg_i)).
 *  2. For every shell c, compute the gradient g_i of the shell's signed
 *     volume with respect to the free vertices. The volume is evaluated
 *     with a shell-local reference point (the shell centroid), so the
 *     whole procedure is invariant under translations and the meaning
 *     of the solution does not depend on the position of the origin.
 *  3. On each shell, project out the component of d that would change
 *     the volume. In the natural vertex metric (weighted by vertex
 *     degree) the displaced field is
 *
 *        h_i = g_i / deg_i
 *        x_i(t, lambda) = x_i + t * d_i - lambda * h_i
 *
 *     and lambda is obtained by a scalar Newton solve on the *exact*
 *     (non-linear) shell volume. This keeps the direction and the volume
 *     correction consistent inside the same step: no fixed point is
 *     moved, and no global rescaling is applied.
 *  4. The step length t (starting from options.step) is halved until the
 *     tentative configuration:
 *        - satisfies the per-shell volume tolerance,
 *        - passes the oriented-area checks on every triangle,
 *        - strictly lowers E.
 *     Rejected attempts are computed in local buffers only: mesh
 *     coordinates and cached derived data are never modified by an
 *     attempt that is not accepted.
 *  5. Accepted coordinates are committed and the next iteration starts
 *     from the updated geometry.
 *
 * ---------------------------------------------------------------------
 * STOPPING CRITERIA
 * ---------------------------------------------------------------------
 *  - `NO_FREE_VERTICES`: every vertex is fixed (or the mesh has no
 *    triangles); no step can be taken.
 *  - `STATIONARY`: the volume-constrained descent direction vanishes
 *    (e.g. an already symmetric shell, where the edge-energy gradient is
 *    parallel to the volume gradient). Reports zero accepted steps.
 *  - `NO_LEGAL_STEP`: backtracking could not find a step satisfying the
 *    volume and face-validity constraints with a strict energy decrease.
 *  - `MAX_ITERATIONS`: `options.max_iterations` accepted steps were
 *    performed.
 *
 * The returned report keeps, for every accepted step, the step length,
 * the energy before/after the step and the achieved constraint residual,
 * plus the initial/final signed volume of every shell (shells are
 * numbered by increasing minimum vertex index) and the stop reason, so
 * the whole run can be audited.
 *
 * ---------------------------------------------------------------------
 * SUPPORTED INPUT
 * ---------------------------------------------------------------------
 * The input must be a finite, non-degenerate, consistently oriented,
 * boundary-less 2-manifold triangle surface. It may be made of several
 * disjoint closed shells. Open meshes, non-manifold edges/vertices,
 * shells with (numerically) zero signed volume, non-finite coordinates,
 * out-of-range vertex indices and invalid options are rejected with
 * std::invalid_argument *before* any coordinate is rewritten.
 */

//::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::

enum class VolumePreservingFairingStopReason
{
    MAX_ITERATIONS,
    STATIONARY,
    NO_FREE_VERTICES,
    NO_LEGAL_STEP
};

CINO_INLINE
const char * volume_preserving_fairing_stop_reason_str(const VolumePreservingFairingStopReason reason);

//::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::

struct VolumePreservingFairingOptions
{
    uint   max_iterations            = 10;   // maximum number of accepted fairing steps
    double step                      = 0.1;  // initial step length, must lie in (0,1]
    double relative_volume_tolerance = 1e-9; // max relative per-shell volume error
};

//::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::

struct VolumePreservingFairingStep
{
    uint   iteration            = 0;  // 1-based iteration in which the step was accepted
    uint   step_index           = 0;  // 1-based index of the accepted step
    uint   attempts             = 1;  // number of backtracked trials used to accept it
    double step_size            = 0.0;// t actually used
    double energy_before        = 0.0;// total edge energy before the step
    double energy               = 0.0;// total edge energy after the step
    double constraint_residual  = 0.0;// max relative per-shell volume error after the step
    double volume_residual      = 0.0;// alias of constraint_residual
};

//::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::

struct VolumePreservingFairingShell
{
    uint              id              = 0;   // 0-based rank, sorted by min vertex index
    uint              min_vertex_id   = 0;   // smallest vertex index in the shell
    std::vector<uint> vertices;              // vertices incident to the shell
    double            initial_volume  = 0.0; // signed volume on input
    double            final_volume    = 0.0; // signed volume on output
};

//::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::

struct VolumePreservingFairingReport
{
    uint   accepted_steps  = 0;  // number of accepted (completed) steps
    uint   completed_steps = 0;  // alias of accepted_steps
    uint   attempted_steps = 0;  // number of outer iterations that were tried
    double initial_energy  = 0.0;
    double final_energy    = 0.0;
    std::vector<VolumePreservingFairingStep>  steps;    // one entry per accepted step
    std::vector<double>                       energies; // energies after each accepted step
    std::vector<VolumePreservingFairingShell> shells;   // sorted by min vertex id
    VolumePreservingFairingStopReason stop = VolumePreservingFairingStopReason::NO_FREE_VERTICES;
    std::string stop_reason;  // stable textual token, e.g. "stationary"
    std::string stop_message; // human readable detail
};

//::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::

template<class M, class V, class E, class P>
CINO_INLINE
VolumePreservingFairingReport volume_preserving_fairing(      Trimesh<M,V,E,P> & m,
                                                         const std::vector<uint> & fixed_vertices = {},
                                                         const VolumePreservingFairingOptions & options = VolumePreservingFairingOptions());

//::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::

template<class M, class V, class E, class P>
CINO_INLINE
VolumePreservingFairingReport volume_preserving_fairing(      Trimesh<M,V,E,P> & m,
                                                         const std::set<uint>    & fixed_vertices,
                                                         const VolumePreservingFairingOptions & options = VolumePreservingFairingOptions());

//::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::

template<class M, class V, class E, class P>
CINO_INLINE
VolumePreservingFairingReport volume_preserving_fairing(      Trimesh<M,V,E,P> & m,
                                                         const std::unordered_set<uint> & fixed_vertices,
                                                         const VolumePreservingFairingOptions & options = VolumePreservingFairingOptions());

}

#ifndef  CINO_STATIC_LIB
#include "volume_preserving_fairing.cpp"
#endif

#endif // CINO_VOLUME_PRESERVING_FAIRING_H
