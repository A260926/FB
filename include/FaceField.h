#ifndef TRIMESH_FACEFIELD_H
#define TRIMESH_FACEFIELD_H
/*
FaceField.h

Smooth N-symmetry direction ("cross") fields on triangle meshes.

Unlike trimesh::diffuse_vector (TriMesh_algo.h), which averages world-space
vector components at vertices, FaceFieldSystem transports directions across
each interior edge by *unfolding* the two incident triangles into a common
plane before comparing them.  Directions are represented per face by the
complex coefficient z of the n-th power of the angle in a local tangent
basis, so a unit direction at angle theta in the local frame corresponds to
z = exp(i*n*theta).  Projecting directions onto a world plane and averaging
components would give a different (and frame-dependent) result.

Supported input: consistently oriented, non-degenerate, 2-manifold triangle
meshes.  Open boundaries, multiple connected components, an empty constraint
set, and a fully constrained mesh are all legal.

Math
----
Face normals follow the winding order; the local frame of face f is
(e1_f, e2_f, normal_f) with e2_f = normal_f x e1_f.  For two faces f, g
sharing an edge, let d be the unit vector along the common edge pointing
from the smaller vertex index to the larger one, and let theta_f, theta_g
be the angles of d in the two local frames.  The transport factor is

    T_fg = exp(i*n*(theta_f - theta_g)),   T_gf = conj(T_fg).

solve() returns the unique minimizer of

    E(z) = sum_f  A_f * |z_f - u_f|^2
         + tau * sum_{interior edges (f,g)}  w_fg * |z_f - T_fg z_g|^2

subject to z_f = h_f on constrained faces, where A_f is the triangle area
and w_fg = |edge|^2 / (A_f + A_g).  Each interior edge is counted once and
boundary edges contribute nothing.  The magnitude of z is meaningful (zero
is a legal value); the result is never renormalized.

Example
-------
    TriMesh *mesh = TriMesh::read("bent.ply");
    mesh->need_normals();               // only if you want them for e1
    std::vector<vec> e1(mesh->faces.size());
    for (size_t f = 0; f < mesh->faces.size(); f++) {
        vec nrm = normalized(mesh->trinorm(f));
        vec a(1.0f, 0.0f, 0.0f);
        e1[f] = normalized(a - nrm * (a DOT nrm));   // unit, tangent
    }
    std::vector<int> fixed;
    fixed.push_back(0);
    FaceFieldSystem sys(*mesh, e1, 4, 1.0, fixed);    // 4-RoSy, tau = 1

    std::vector<std::complex<double> > u(mesh->faces.size(), 0.0);
    std::vector<std::complex<double> > h(1, 1.0);     // z_0 = 1
    FaceFieldSystem::Result r = sys.solve(u, h);      // r.z, r.residual

    // Same geometry/bases/constraints: new right-hand sides re-use the
    // factored operator and preconditioner built in the constructor.
    u[17] = std::complex<double>(0.0, 2.0);
    FaceFieldSystem::Result r2 = sys.solve(u, h);

The system stores its own copy of everything it needs; the input mesh may
be modified or destroyed after construction.  Storage is O(#faces).

All geometric validation failures throw std::invalid_argument; failure of
the iterative solver to converge throws std::runtime_error.
*/

#include "TriMesh.h"
#include <complex>
#include <vector>

namespace trimesh {

class FaceFieldSystem {
public:
	// Result of solve(): the per-face complex field (same layout as the
	// input u) plus the relative residual of the free rows,
	//   ||Hz - b||_inf / max(1, ||b||_inf),
	// which is guaranteed to be at most 1e-9 on successful return.
	struct Result {
		::std::vector< ::std::complex<double> > z;
		double residual;
	};

	// mesh: oriented, non-degenerate, 2-manifold triangle mesh.
	// e1: one unit-length tangent vector per face (world coordinates).
	// n: symmetry order, 1..8.  tau: positive smoothness weight.
	// constraints: distinct face indices whose values are fixed by solve().
	// Throws std::invalid_argument on any invalid input.  Builds the
	// connection, the free-row operator, and the preconditioner once;
	// later solve() calls with new data re-use them.
	FaceFieldSystem(const TriMesh &mesh,
	                const ::std::vector<vec> &e1,
	                int n,
	                double tau,
	                const ::std::vector<int> &constraints);

	// u: one complex input value per face.  h: one value per constraint,
	// in the same order as the constraint list passed to the constructor.
	// Returns the minimizing field z (with z_f = h_f on constrained faces)
	// and the free-row relative residual.  Throws std::invalid_argument
	// on size mismatches or non-finite data, std::runtime_error if the
	// solver does not converge.
	Result solve(const ::std::vector< ::std::complex<double> > &u,
	             const ::std::vector< ::std::complex<double> > &h) const;

	// Introspection (all storage grows linearly with the face count).
	size_t face_count() const { return area_.size(); }
	size_t free_count() const { return face_of_free_.size(); }
	size_t constraint_count() const { return constraints_.size(); }
	size_t connection_count() const { return edges_.size(); }
	size_t operator_nonzeros() const { return op_val_.size(); }
	int symmetry_order() const { return n_; }
	double smoothing_weight() const { return tau_; }

private:
	struct Edge {
		int f, g;                       // incident faces
		double w;                       // |edge|^2 / (A_f + A_g)
		::std::complex<double> t;       // T_fg = exp(i*n*(theta_f-theta_g))
	};

	// Per-face data, copied out of the input mesh at construction.
	::std::vector<double> area_;

	// Interior edges with their transport factors (the "connection").
	::std::vector<Edge> edges_;

	// Constraint bookkeeping: constraints_[k] is the face fixed by h[k].
	::std::vector<int> constraints_;
	::std::vector<int> constraint_pos_;  // face -> index into h, or -1

	// Free rows: face_of_free_[i] is the face solved for at row i.
	::std::vector<int> free_of_face_;    // face -> free row, or -1
	::std::vector<int> face_of_free_;

	// Free-row operator H in CSR form (Hermitian positive definite).
	::std::vector<int> op_rowptr_;
	::std::vector<int> op_col_;
	::std::vector< ::std::complex<double> > op_val_;

	// Jacobi preconditioner: inverse of the (real, positive) diagonal.
	::std::vector<double> diag_inv_;

	int n_;
	double tau_;
};

} // namespace trimesh

#endif
