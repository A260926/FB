// Voro++, a 3D cell-based Voronoi library
//
// Author   : Chris H. Rycroft (Harvard University / LBL)
// Email    : chr@alum.mit.edu
// Date     : August 30th 2011

/** \file capacity.hh
 * \brief Header file for the capacity-constrained power diagram solver.
 *
 * This file provides an interface for computing a power (Laguerre) diagram
 * of a set of seed points inside a non-periodic axis-aligned box, where the
 * power weights of the seeds are adjusted so that each cell reaches a
 * prescribed target volume. Seed positions are never moved, which makes the
 * routine suitable for procedural fracture workflows where the seed layout
 * is fixed in advance.
 *
 * A power weight \f$w_i\f$ assigns to seed \f$i\f$ the cell
 * \f[ \{x : |x-p_i|^2 - w_i \le |x-p_j|^2 - w_j \ \forall j\}, \f]
 * clipped by the bounding box. The weights are only determined up to a
 * global additive constant; all results reported by this interface are
 * normalized so that the minimum weight is zero.
 *
 * Minimal example:
 * \code
 * #include "capacity.hh"
 * #include <cstdio>
 * #include <vector>
 *
 * int main() {
 *     // Unit box, three seeds, target volumes 1/2, 1/3, 1/6.
 *     std::vector<int> ids;
 *     std::vector<double> pos,target;
 *     double p[9]={0.3,0.5,0.5, 0.7,0.3,0.5, 0.7,0.7,0.5};
 *     double t[3]={0.5,1.0/3.0,1.0/6.0};
 *     for(int i=0;i<3;i++) {
 *         ids.push_back(i);
 *         for(int d=0;d<3;d++) pos.push_back(p[3*i+d]);
 *         target.push_back(t[i]);
 *     }
 *     voro::capacity_result res;
 *     voro::capacity_status st=
 *         voro::capacity_solve(0,1,0,1,0,1,ids,pos,target,res);
 *     if(st==voro::capacity_converged) {
 *         for(size_t i=0;i<res.weights.size();i++)
 *             printf("id=%d weight=%g volume=%g\n",res.geometry.cells[i].id,
 *                    res.weights[i],res.geometry.cells[i].volume);
 *     }
 *     return st==voro::capacity_converged?0:1;
 * }
 * \endcode
 */

#ifndef VOROPP_CAPACITY_HH
#define VOROPP_CAPACITY_HH

#include <vector>

namespace voro {

/** \brief Status codes returned by the capacity solver and geometry
 * evaluation routines. */
enum capacity_status {
	/** The iteration reached the requested relative volume error
	 * threshold. For capacity_evaluate, indicates that a complete
	 * geometry was produced. */
	capacity_converged=0,
	/** The maximum number of iterations was reached before converging.
	 * The result holds the last accepted complete state. */
	capacity_max_iterations=1,
	/** No acceptable step could be found (the Newton system was
	 * singular or the line search made no progress). The result holds
	 * the last accepted complete state. */
	capacity_no_progress=2,
	/** The initial weights already produce an empty cell. No iteration
	 * was attempted. */
	capacity_empty_initial=3,
	/** The input failed validation; nothing was computed and the
	 * caller's arrays were left untouched. */
	capacity_invalid_input=4
};

/** \brief Options controlling the capacity solve.
 *
 * The tolerance is the relative per-cell volume error threshold
 * \f$|V_i-t_i|/t_i\f$ used to decide convergence, and max_iterations is the
 * maximum number of accepted Newton steps. */
struct capacity_options {
	/** Relative volume error threshold. */
	double tolerance;
	/** Maximum number of accepted Newton iterations. */
	int max_iterations;
	/** Default constructor: tolerance 1e-6, max_iterations 80. */
	capacity_options() : tolerance(1e-6), max_iterations(80) {}
	/** Constructor setting both fields.
	 * \param[in] tolerance_ the relative volume error threshold.
	 * \param[in] max_iterations_ the maximum number of iterations. */
	capacity_options(double tolerance_,int max_iterations_)
		: tolerance(tolerance_), max_iterations(max_iterations_) {}
};

/** \brief Geometry of a single power diagram cell. */
struct capacity_cell {
	/** The caller-supplied seed ID. */
	int id;
	/** The cell volume. */
	double volume;
	/** The cell centroid in world coordinates. */
	double centroid[3];
	/** The cell vertices in world coordinates, three values per vertex.
	 */
	std::vector<double> vertices;
	/** For each face, the ordered list of vertex indices (into the
	 * vertices array) forming that face. */
	std::vector<std::vector<int> > faces;
	/** For each face, the ID of the neighboring seed across that face.
	 * Faces on the boundary of the box keep their negative wall IDs
	 * (-1 to -6). */
	std::vector<int> face_neighbors;
	capacity_cell() : id(-1),volume(0) {centroid[0]=centroid[1]=centroid[2]=0;}
};

/** \brief Output of a geometry evaluation: the complete power diagram for
 * one weight vector, plus the local derivative of cell volumes with respect
 * to the power weights. */
struct capacity_geometry {
	/** True if every seed has a non-empty cell. If false, one or more
	 * cells vanished and the remaining output is incomplete. */
	bool ok;
	/** The cells, in the same order as the input ID array. */
	std::vector<capacity_cell> cells;
	/** Row-major n by n matrix of dV_i/dw_j, assembled from the current
	 * shared-face geometry: for adjacent seeds i and j the off-diagonal
	 * entry is -area(face ij)/(2|p_i-p_j|) and the diagonal is the
	 * negated row sum of the off-diagonal entries. Box walls are not
	 * treated as particles and contribute nothing. Every row sums to
	 * zero, consistently with the invariance of the geometry under a
	 * uniform shift of all weights. Only valid when ok is true. */
	std::vector<double> derivative;
	capacity_geometry() : ok(false) {}
};

/** \brief Record of one accepted iteration of the capacity solve. */
struct capacity_iteration {
	/** The zero-based iteration index. */
	int iteration;
	/** The maximum relative volume error max_i |V_i-t_i|/t_i of the
	 * accepted state. */
	double max_residual;
	/** The smallest cell volume of the accepted state. */
	double min_cell_volume;
	/** The number of full geometry rebuilds performed during this
	 * iteration, including rejected trial steps. A value larger than
	 * one indicates that trial steps were rejected (for example
	 * because a cell vanished) and the step had to be shortened. */
	int geometry_rebuilds;
};

/** \brief Full result of a capacity solve. */
struct capacity_result {
	/** The termination status. */
	capacity_status status;
	/** The final power weights, normalized so that the minimum weight
	 * is zero. */
	std::vector<double> weights;
	/** The complete geometry of the last accepted state, including the
	 * final polyhedra and the volume derivative matrix. */
	capacity_geometry geometry;
	/** One record per accepted iteration. */
	std::vector<capacity_iteration> history;
	/** The total number of full geometry rebuilds performed, including
	 * the initial evaluation and all rejected trial steps. */
	int total_geometry_rebuilds;
	capacity_result() : status(capacity_invalid_input),total_geometry_rebuilds(0) {}
};

/** Returns a short human-readable name for a status code.
 * \param[in] status the status code.
 * \return The name as a C string. */
const char* capacity_status_name(capacity_status status);

/** \brief Evaluates the power diagram geometry for a given weight vector
 * without attempting any solve.
 *
 * \param[in] (ax,bx,ay,by,az,bz) the non-periodic axis-aligned box.
 * \param[in] ids the unique non-negative seed IDs (2 to 64 of them).
 * \param[in] positions the seed positions, three values per seed, each
 *                      strictly inside the box.
 * \param[in] weights the power weights; any finite values are accepted
 *                    since the geometry is invariant under a uniform
 *                    shift.
 * \param[out] out the resulting geometry and volume derivative matrix,
 *                 ordered to match the input IDs.
 * \return capacity_converged if a complete geometry was produced,
 *         capacity_empty_initial if some cell is empty, or
 *         capacity_invalid_input if the input failed validation. */
capacity_status capacity_evaluate(double ax,double bx,double ay,double by,double az,double bz,
		const std::vector<int> &ids,const std::vector<double> &positions,
		const std::vector<double> &weights,capacity_geometry &out);

/** \brief Solves for power weights that achieve prescribed cell volumes,
 * starting from equal weights.
 *
 * \param[in] (ax,bx,ay,by,az,bz) the non-periodic axis-aligned box.
 * \param[in] ids the unique non-negative seed IDs (2 to 64 of them).
 * \param[in] positions the seed positions, three values per seed, each
 *                      strictly inside the box.
 * \param[in] target_volumes the positive target volume per seed; the sum
 *                           must match the box volume to a relative
 *                           accuracy of 1e-10. Targets are used as given
 *                           and are never renormalized.
 * \param[out] out the solve result.
 * \return The termination status, also stored in out.status. */
capacity_status capacity_solve(double ax,double bx,double ay,double by,double az,double bz,
		const std::vector<int> &ids,const std::vector<double> &positions,
		const std::vector<double> &target_volumes,capacity_result &out);

/** \brief Solves for power weights with explicit options, starting from
 * equal weights.
 * \param[in] (ax,bx,ay,by,az,bz) the non-periodic axis-aligned box.
 * \param[in] ids the unique non-negative seed IDs.
 * \param[in] positions the seed positions, three values per seed.
 * \param[in] target_volumes the positive target volume per seed.
 * \param[out] out the solve result.
 * \param[in] options the tolerance and iteration limit.
 * \return The termination status, also stored in out.status. */
capacity_status capacity_solve(double ax,double bx,double ay,double by,double az,double bz,
		const std::vector<int> &ids,const std::vector<double> &positions,
		const std::vector<double> &target_volumes,capacity_result &out,
		const capacity_options &options);

/** \brief Solves for power weights with explicit options and initial
 * weights.
 *
 * The initial weights may contain any mix of positive and negative finite
 * values; a uniform shift of them describes the same starting geometry.
 * If the initial weights already produce an empty cell the routine
 * returns capacity_empty_initial. During the iteration, trial steps that
 * would empty a cell are rejected and the step is shortened instead.
 *
 * \param[in] (ax,bx,ay,by,az,bz) the non-periodic axis-aligned box.
 * \param[in] ids the unique non-negative seed IDs.
 * \param[in] positions the seed positions, three values per seed.
 * \param[in] target_volumes the positive target volume per seed.
 * \param[out] out the solve result.
 * \param[in] options the tolerance and iteration limit.
 * \param[in] initial_weights the weights to start iterating from.
 * \return The termination status, also stored in out.status. */
capacity_status capacity_solve(double ax,double bx,double ay,double by,double az,double bz,
		const std::vector<int> &ids,const std::vector<double> &positions,
		const std::vector<double> &target_volumes,capacity_result &out,
		const capacity_options &options,const std::vector<double> &initial_weights);

}

#endif
