#ifndef CONFORMALMAP_H
#define CONFORMALMAP_H
/*
Discrete conformal flattening of a triangulated topological disk.

The map is defined by an intrinsic edge length for every edge of the
triangulation.  Starting from the original 3D edge lengths l0_ij, the
lengths are scaled by one degree of freedom per vertex:

	l_ij = exp((u_i + u_j) / 2) * l0_ij,

with u_0 == 0 to remove the global scale.  The solver looks for the u
that realizes a prescribed angle defect (curvature) at every vertex:
2*pi minus the sum of incident angles at interior vertices, and
pi minus the sum of incident angles at boundary vertices.  The
triangulation may change by intrinsic edge flips, both while initially
making the input metric Delaunay and at cocircular events during the
scale update, so the result is a Delaunay triangulation in the final
discrete conformal class.

Only C++17 standard library facilities are used.
*/

#include "TriMesh.h"

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace trimesh {

class ConformalMap {
public:
	// Distinguishable solver outcomes.
	enum Status {
		SUCCESS = 0,        // converged and a consistent flat map was published
		BUDGET_EXHAUSTED,   // nonlinear steps and/or flips ran out
		STAGNATED,          // numerical stagnation / linear solver failure
		TOPOLOGY_FAILURE,   // a required flip could not be completed
		UNFOLD_FAILURE,     // converged, but unfolding was inconsistent
		INVALID_STATE       // no usable solve has been performed
	};

	static const char *status_name(Status status);

	// Read-only description of the current intrinsic mesh.  length already
	// includes u; base_length is the l0 of the exponential parameterization.
	// faces[0]/faces[1] are -1 when the edge is on the boundary.
	struct EdgeInfo {
		int v0;
		int v1;
		double base_length;
		double length;
		int faces[2];
	};

	// One intrinsic edge flip.  a,b is the removed diagonal and k,l the new
	// one.  new_base_length is chosen so that
	// new_base_length * exp((u_k + u_l)/2) == new_length exactly at the
	// event, which keeps the metric continuous across the flip.
	struct FlipRecord {
		int step;                  // nonlinear step at which the flip happened
		// Where the flip came from: 0 = initial Delaunayization,
		// 1 = a cocircular event crossed by a Newton step,
		// 2 = maintenance of Delaunay after a step.
		int phase;
		int a, b, k, l;            // removed diagonal (a,b), new diagonal (k,l)
		int old_face0, old_face1;
		double u_a, u_b, u_k, u_l;
		double old_base_length;
		double old_length;         // length of a-b at the flip
		double new_length;         // intrinsically developed length of k-l
		double new_length_from_a;  // same, developed from vertex a only
		double new_length_from_b;  // same, developed from vertex b only
		double new_base_length;    // l0 assigned to k-l
		double opposite_angle_sum; // sum of the two angles opposite a-b
		// Developed quad data, usable to recompute new_length independently
		// from either side with the law of cosines.
		double angle_a;            // sum of the two face angles at a
		double angle_b;            // sum of the two face angles at b
		double side_ak, side_al, side_bk, side_bl;
	};

	struct Diagnostics {
		Status status = INVALID_STATE;
		int steps = 0;               // nonlinear updates applied by this solve
		int flips = 0;               // edge flips performed by this solve
		int initial_flips = 0;       // flips done while legalizing the input
		int work_used = 0;           // steps + flips of this solve
		int linear_iterations = 0;   // PCG iterations of the last solve
		std::size_t linear_nonzeros = 0; // nonzeros in the last Jacobian
		double max_residual = 0;     // max |angle defect - target|
		double tolerance = 0;        // tolerance supplied to solve
		double max_unfold_error = 0; // max cross-face position disagreement
		bool published = false;      // a flat map was published
		::std::string last_error;    // human-readable reason on failure
	};

	// Returned by solve().  The flat mesh is an independent value: it stays
	// valid after the workspace is destroyed, re-solved, or the source mesh
	// is released.
	struct Result {
		Status status = INVALID_STATE;
		Diagnostics diagnostics;
		TriMesh flat;                 // empty unless status == SUCCESS
		bool ok() const { return status == SUCCESS; }
		explicit operator bool() const { return ok(); }
	};

	// Copies the vertices, faces, colors and confidences needed by the
	// workspace.  Throws std::invalid_argument before any state is
	// established if the mesh is not a connected, consistently oriented,
	// isolated-vertex-free manifold triangle disk with exactly one boundary
	// loop, if the geometry is non-finite or a triangle has zero area, or if
	// colors/confidences have a bad length.
	explicit ConformalMap(const TriMesh &mesh);
	~ConformalMap();
	ConformalMap(const ConformalMap &) = delete;
	ConformalMap &operator=(const ConformalMap &) = delete;

	// boundary_targets is indexed by the original vertex number.  Entries
	// for interior vertices are ignored; every boundary entry must be
	// finite and strictly between 0 and pi, and the boundary entries must
	// sum to 2*pi (absolute error <= 1e-10).  Bad targets throw
	// std::invalid_argument before any solver state is touched.
	//
	// max_work bounds steps + flips of this call.  The overload with
	// separate caps bounds nonlinear steps and flip events independently.
	//
	// On any outcome other than SUCCESS the previously published flat map
	// and the committed state (u, base lengths, connectivity) are left
	// untouched, so another solve may be attempted.
	Result solve(const std::vector<double> &boundary_targets,
	             double tolerance, int max_work);
	Result solve(const std::vector<double> &boundary_targets,
	             double tolerance, int max_steps, int max_flips);

	// Read-only view of the committed state.
	const std::vector<double> &u() const;
	const std::vector<TriMesh::Face> &faces() const;
	const std::vector<EdgeInfo> &edges() const;
	const std::vector<int> &boundary_loop() const;
	bool is_boundary_vertex(int v) const;
	int num_vertices() const;
	int num_faces() const;
	int num_edges() const;

	// Length of the current intrinsic edge between v0 and v1, or 0 when the
	// two vertices are not connected by an edge.
	double edge_length(int v0, int v1) const;

	// Angle defect (curvature) actually realized by the committed state.
	std::vector<double> angle_defects() const;

	// Sparse Jacobian d(kappa)/du of the committed metric, restricted to the
	// unpinned vertices.  row and col are original vertex numbers minus one
	// (the pinned vertex 0 is removed), so the matrix has num_vertices()-1
	// rows and columns.  It is the cotangent Laplacian the solver uses
	// (positive diagonal, non-positive off-diagonals), exposed for
	// verification; it is never assembled densely.
	struct JacobianEntry {
		int row;
		int col;
		double value;
	};
	std::vector<JacobianEntry> jacobian() const;

	const Diagnostics &diagnostics() const;
	const std::vector<FlipRecord> &flip_records() const;

	bool has_flat_map() const;
	const TriMesh &flat() const;      // published map, owned by the workspace
	TriMesh flat_mesh() const;        // independent copy

private:
	Result solve_impl(const ::std::vector<double> &boundary_targets,
	                  double tolerance, int max_steps, int max_flips,
	                  int total_cap);

	struct Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace trimesh

#endif
