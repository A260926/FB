// Voro++, a 3D cell-based Voronoi library
//
// Author   : Chris H. Rycroft (LBL / UC Berkeley)
// Email    : chr@alum.mit.edu
// Date     : August 30th 2011

/** \file capacity.hh
 * \brief Header file for solving capacity-constrained power diagram
 * problems.
 *
 * The routines in this file find power weights \f$w_i\f$ for a fixed set of
 * seed positions, so that the cells of the resulting power diagram (radical
 * Voronoi tessellation) inside an axis-aligned box reach prescribed target
 * volumes. A seed \f$i\f$ at position \f$p_i\f$ owns exactly the points
 * \f$x\f$ minimizing \f$|x-p_i|^2-w_i\f$, so only differences between the
 * weights matter; all publicly returned weight vectors are normalized so
 * that the smallest weight is zero. The seed positions are never altered,
 * which makes the routines suitable for procedural fracture problems where
 * the seed layout is fixed in advance.
 *
 * Two entry points are provided. capacity_solve() iteratively adjusts the
 * weights until the cell volumes match the targets, and
 * capacity_evaluate() only builds the geometry for a given weight vector,
 * additionally returning the local derivative matrix of the cell volumes
 * with respect to the weights, computed from the actual shared-face
 * geometry of the current tessellation.
 *
 * A minimal example:
 * \code
 * #include "voro++.hh"
 *
 * int main() {
 *     // Two seeds in the unit cube, aiming for a 40/60 volume split
 *     std::vector<int> ids;         ids.push_back(0); ids.push_back(1);
 *     std::vector<double> pos;      // (x,y,z) triples
 *     pos.push_back(0.3); pos.push_back(0.5); pos.push_back(0.5);
 *     pos.push_back(0.7); pos.push_back(0.5); pos.push_back(0.5);
 *     std::vector<double> target;   target.push_back(0.4); target.push_back(0.6);
 *
 *     voro::capacity_result res;
 *     voro::capacity_status st = voro::capacity_solve(
 *             0,1, 0,1, 0,1,          // non-periodic axis-aligned box
 *             ids, pos, target, res); // default options, equal-weight start
 *     if (st == voro::capacity_converged) {
 *         // res.weights[i] is the power weight of seed ids[i] (min weight 0)
 *         // res.cells[i]   is the final polyhedron of seed ids[i]
 *     }
 *     return 0;
 * }
 * \endcode
 */

#ifndef VOROPP_CAPACITY_HH
#define VOROPP_CAPACITY_HH

#include <vector>

namespace voro {

/** \brief Status codes returned by capacity_solve().
 *
 * Only capacity_converged indicates that the requested volume tolerance was
 * reached. In all other cases the returned state must not be treated as a
 * converged solution. */
enum capacity_status {
	/** The relative volume error reached the requested tolerance. */
	capacity_converged=0,
	/** The iteration limit was reached. The result holds the last
	 * accepted state, which is not converged. */
	capacity_max_iterations=1,
	/** No acceptable update step could be found. The result holds the
	 * last accepted state, which is not converged. */
	capacity_no_progress=2,
	/** The initial weights leave at least one seed with an empty cell,
	 * so no iteration could be started. */
	capacity_empty_initial=3,
	/** The problem description failed the input validation. */
	capacity_invalid_input=4
};

/** \brief Tuning parameters for capacity_solve(). */
struct capacity_options {
	/** The relative volume error threshold that every cell must reach
	 * for the solve to be reported as converged. */
	double rel_tol;
	/** The maximum number of accepted update steps. */
	int max_iterations;
	/** Sets up the default options: a relative volume tolerance of
	 * 1e-6 and an iteration limit of 80. */
	capacity_options() : rel_tol(1e-6), max_iterations(80) {}
};

/** \brief Diagnostics recorded for every accepted state during a solve. */
struct capacity_step {
	/** The iteration index; zero is the initial state. */
	int iteration;
	/** The maximum relative volume error \f$|V_i-t_i|/t_i\f$ of this
	 * state. */
	double max_rel_error;
	/** The smallest cell volume of this state. */
	double min_volume;
	/** The total number of full geometry rebuilds performed up to and
	 * including this state, counting rejected trial steps. A difference
	 * larger than one between consecutive entries signals rejected
	 * trials within that iteration. */
	long geometry_builds;
};

/** \brief The computed polyhedron of a single seed. */
struct capacity_cell {
	/** The ID of the seed, as supplied by the caller. */
	int id;
	/** The volume of the cell. */
	double volume;
	/** The centroid of the cell in world coordinates. */
	double centroid[3];
	/** The cell vertices in world coordinates, as consecutive
	 * (x,y,z) triples. */
	std::vector<double> vertices;
	/** The faces of the cell. Each face is an ordered loop of indices
	 * into the vertices array. */
	std::vector<std::vector<int> > faces;
	/** The neighbor of each face, in the same order as faces: the ID of
	 * the seed sharing the face, or a negative ID for a face on the
	 * boundary box wall. */
	std::vector<int> face_neighbors;
};

/** \brief Output of capacity_evaluate(): the full geometry of a power
 * diagram for a given weight vector, plus the local volume derivatives. */
struct capacity_eval {
	/** The seed IDs, in the same order as the caller supplied them. */
	std::vector<int> ids;
	/** The cell volumes, in caller (input) order. */
	std::vector<double> volumes;
	/** The cell centroids in world coordinates, as consecutive (x,y,z)
	 * triples in caller order. */
	std::vector<double> centroids;
	/** The full cell geometry, in caller order. */
	std::vector<capacity_cell> cells;
	/** The local derivative matrix of the cell volumes with respect to
	 * the power weights, stored row-major with n rows and n columns:
	 * entry (i,j) is d(volume_i)/d(weight_j). It is assembled from the
	 * shared faces of the current tessellation; boundary walls are not
	 * treated as particles. Every row sums to zero, consistently with
	 * the geometry being invariant under a constant shift of all
	 * weights. */
	std::vector<double> dvolume;
};

/** \brief Output of capacity_solve(). */
struct capacity_result {
	/** The termination status of the solve. */
	capacity_status status;
	/** The number of accepted update steps that were performed. */
	int iterations;
	/** The total number of full geometry rebuilds, including rejected
	 * trial steps. */
	long geometry_builds;
	/** The maximum relative volume error of the returned state. */
	double max_rel_error;
	/** The power weights of the returned state, in caller order,
	 * normalized so that the smallest weight is zero. */
	std::vector<double> weights;
	/** The cell volumes of the returned state, in caller order. */
	std::vector<double> volumes;
	/** The final polyhedra, in caller order. Empty unless the status is
	 * capacity_converged, capacity_max_iterations, or
	 * capacity_no_progress. */
	std::vector<capacity_cell> cells;
	/** One entry per accepted state, starting with the initial state. */
	std::vector<capacity_step> history;
	/** Sets up an empty result. */
	capacity_result() : status(capacity_invalid_input), iterations(0),
		geometry_builds(0), max_rel_error(0) {}
};

/** \brief Builds the power diagram for a given weight vector and returns
 * its geometry together with the local volume derivatives.
 *
 * The cell of seed i is the set of points minimizing |x-p_i|^2-w_i inside
 * the non-periodic axis-aligned box. Only differences between the weights
 * affect the geometry. Between 2 and 64 seeds are allowed; IDs must be
 * unique and non-negative, positions must be finite, distinct, and inside
 * the box, and the box side lengths must be positive and finite.
 *
 * \param[in] (ax,bx) the x extents of the box.
 * \param[in] (ay,by) the y extents of the box.
 * \param[in] (az,bz) the z extents of the box.
 * \param[in] ids the unique non-negative seed IDs.
 * \param[in] positions the seed positions as consecutive (x,y,z) triples.
 * \param[in] weights the power weights, one per seed; they may have
 *                    arbitrary signs.
 * \param[out] out the geometry and derivative matrix.
 * \return True on success, false if the input is invalid or if at least
 *         one seed has an empty cell. */
bool capacity_evaluate(double ax,double bx,double ay,double by,double az,double bz,
		const std::vector<int> &ids,const std::vector<double> &positions,
		const std::vector<double> &weights,capacity_eval &out);

/** \brief Solves for power weights that make the cell volumes match given
 * target volumes, keeping the seed positions fixed.
 *
 * The targets are used exactly as supplied and are never renormalized;
 * their sum must agree with the box volume to a relative accuracy of
 * 1e-10. The caller's input arrays are never modified. The iteration
 * starts from initial_weights if given, otherwise from equal weights, and
 * every accepted state is rebuilt from scratch, so faces appearing or
 * disappearing between iterations are handled naturally. The global
 * additive freedom of the weights is removed by normalizing every
 * accepted state (and the reported result) to a minimum weight of zero.
 *
 * \param[in] (ax,bx) the x extents of the box.
 * \param[in] (ay,by) the y extents of the box.
 * \param[in] (az,bz) the z extents of the box.
 * \param[in] ids the unique non-negative seed IDs.
 * \param[in] positions the seed positions as consecutive (x,y,z) triples.
 * \param[in] target_volumes the positive target volume of each seed.
 * \param[out] out the resulting weights, polyhedra, and diagnostics.
 * \param[in] opts the tolerance and iteration limit.
 * \param[in] initial_weights an optional vector of starting weights, or
 *                            NULL to start from equal weights.
 * \return The termination status. */
capacity_status capacity_solve(double ax,double bx,double ay,double by,double az,double bz,
		const std::vector<int> &ids,const std::vector<double> &positions,
		const std::vector<double> &target_volumes,capacity_result &out,
		const capacity_options &opts=capacity_options(),
		const std::vector<double> *initial_weights=0);

}

#endif
