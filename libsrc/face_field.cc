/*
face_field.cc

Smooth N-symmetry direction fields on triangle meshes, transported across
edges by unfolding (see FaceField.h for the mathematical formulation).

The connection (per-edge transport factors), the free-row operator, and
the Jacobi preconditioner are built once in the constructor; solve() only
assembles a new right-hand side and runs preconditioned conjugate
gradients, so repeated solves with new data are cheap.  All storage is
O(#faces); everything is held in double precision.
*/

#include "FaceField.h"
#include <cmath>
#include <map>
#include <stdexcept>
#include <utility>

using namespace std;

namespace trimesh {

namespace {

typedef complex<double> cdouble;
typedef Vec<3,double> dvec;

inline bool finite3(const dvec &v)
{
	return isfinite(v[0]) && isfinite(v[1]) && isfinite(v[2]);
}

// One undirected mesh edge, as accumulated while scanning the faces.
struct EdgeAccum {
	int face[2];        // incident faces
	bool along[2];      // true if that face traverses the edge lo->hi
	int nfaces;
	EdgeAccum() : nfaces(0) {}
};

// Hermitian sparse matvec: y = H x (CSR).
static void csr_matvec(const vector<int> &rowptr,
                       const vector<int> &col,
                       const vector<cdouble> &val,
                       const vector<cdouble> &x,
                       vector<cdouble> &y)
{
	const size_t m = x.size();
	for (size_t i = 0; i < m; i++) {
		cdouble s(0.0, 0.0);
		for (int k = rowptr[i]; k < rowptr[i + 1]; k++)
			s += val[k] * x[col[k]];
		y[i] = s;
	}
}

inline cdouble cdot(const vector<cdouble> &a, const vector<cdouble> &b)
{
	cdouble s(0.0, 0.0);
	for (size_t i = 0; i < a.size(); i++)
		s += conj(a[i]) * b[i];
	return s;
}

inline double norm_inf(const vector<cdouble> &v)
{
	double r = 0.0;
	for (size_t i = 0; i < v.size(); i++)
		r = max(r, abs(v[i]));
	return r;
}

} // namespace


FaceFieldSystem::FaceFieldSystem(const TriMesh &mesh,
                                 const vector<vec> &e1,
                                 int n,
                                 double tau,
                                 const vector<int> &constraints)
	: n_(n), tau_(tau)
{
	if (n < 1 || n > 8)
		throw invalid_argument("FaceFieldSystem: symmetry order n must be in 1..8");
	if (!isfinite(tau) || tau <= 0.0)
		throw invalid_argument("FaceFieldSystem: tau must be positive and finite");

	const size_t nv = mesh.vertices.size();
	const size_t nf = mesh.faces.size();
	if (e1.size() != nf)
		throw invalid_argument("FaceFieldSystem: e1 must have one entry per face");

	// Copy out the geometry we need; the input mesh is not modified and
	// may be destroyed after this constructor returns.
	vector<dvec> pos(nv, dvec(0.0, 0.0, 0.0));
	vector<char> pos_ok(nv, 0);

	area_.assign(nf, 0.0);
	vector<dvec> e2(nf, dvec(0.0, 0.0, 0.0));
	vector<dvec> e1d(nf, dvec(0.0, 0.0, 0.0));

	for (size_t f = 0; f < nf; f++) {
		const TriMesh::Face &F = mesh.faces[f];
		for (int j = 0; j < 3; j++) {
			if (F[j] < 0 || (size_t) F[j] >= nv)
				throw invalid_argument("FaceFieldSystem: face vertex index out of range");
			if (!pos_ok[F[j]]) {
				dvec p(mesh.vertices[F[j]]);
				if (!finite3(p))
					throw invalid_argument("FaceFieldSystem: non-finite vertex coordinate");
				pos[F[j]] = p;
				pos_ok[F[j]] = 1;
			}
		}
		if (F[0] == F[1] || F[1] == F[2] || F[2] == F[0])
			throw invalid_argument("FaceFieldSystem: degenerate face (repeated vertex)");

		const dvec &p0 = pos[F[0]];
		const dvec &p1 = pos[F[1]];
		const dvec &p2 = pos[F[2]];
		dvec nrm = (p1 - p0) CROSS (p2 - p0);
		double nlen = len(nrm);          // twice the triangle area
		if (!isfinite(nlen) || nlen <= 0.0)
			throw invalid_argument("FaceFieldSystem: degenerate face (zero area)");
		area_[f] = 0.5 * nlen;
		nrm /= nlen;                     // unit normal, by winding order

		dvec b(e1[f]);
		if (!finite3(b))
			throw invalid_argument("FaceFieldSystem: non-finite basis vector");
		if (fabs(len(b) - 1.0) > 1e-3)
			throw invalid_argument("FaceFieldSystem: basis vector is not unit length");
		if (fabs(b DOT nrm) > 1e-3)
			throw invalid_argument("FaceFieldSystem: basis vector is not tangent");
		e1d[f] = b;
		e2[f] = nrm CROSS b;
	}

	// Constraint list: distinct, in-range face indices.
	constraints_ = constraints;
	constraint_pos_.assign(nf, -1);
	for (size_t k = 0; k < constraints.size(); k++) {
		int c = constraints[k];
		if (c < 0 || (size_t) c >= nf)
			throw invalid_argument("FaceFieldSystem: constraint face index out of range");
		if (constraint_pos_[c] >= 0)
			throw invalid_argument("FaceFieldSystem: duplicate constraint face");
		constraint_pos_[c] = (int) k;
	}

	// Accumulate edges; each interior edge must be shared by exactly two
	// faces that traverse it in opposite directions (orientability).
	typedef map<pair<int,int>, EdgeAccum> edgemap_t;
	edgemap_t edgemap;
	for (size_t f = 0; f < nf; f++) {
		const TriMesh::Face &F = mesh.faces[f];
		for (int j = 0; j < 3; j++) {
			int v0 = F[j], v1 = F[(j + 1) % 3];
			bool along = v0 < v1;
			pair<int,int> key = along ? make_pair(v0, v1)
			                          : make_pair(v1, v0);
			EdgeAccum &rec = edgemap[key];
			if (rec.nfaces >= 2)
				throw invalid_argument("FaceFieldSystem: edge shared by more than two faces");
			rec.face[rec.nfaces] = (int) f;
			rec.along[rec.nfaces] = along;
			rec.nfaces++;
		}
	}

	// Transport factors and weights, one per interior edge.
	edges_.reserve(edgemap.size());
	for (edgemap_t::const_iterator it = edgemap.begin();
	     it != edgemap.end(); ++it) {
		const EdgeAccum &rec = it->second;
		if (rec.nfaces != 2)
			continue;               // boundary edge: no term
		if (rec.along[0] == rec.along[1])
			throw invalid_argument("FaceFieldSystem: inconsistent face orientation along an edge");
		const int f0 = rec.face[0], f1 = rec.face[1];
		dvec d = pos[it->first.second] - pos[it->first.first];
		double elen2 = len2(d);
		d /= sqrt(elen2);           // lo -> hi unit edge direction
		double th0 = atan2(d DOT e2[f0], d DOT e1d[f0]);
		double th1 = atan2(d DOT e2[f1], d DOT e1d[f1]);
		Edge e;
		e.f = f0;
		e.g = f1;
		e.w = elen2 / (area_[f0] + area_[f1]);
		e.t = exp(cdouble(0.0, (double) n * (th0 - th1)));
		edges_.push_back(e);
	}

	// Free-row numbering.
	free_of_face_.assign(nf, -1);
	for (size_t f = 0; f < nf; f++) {
		if (constraint_pos_[f] < 0) {
			free_of_face_[f] = (int) face_of_free_.size();
			face_of_free_.push_back((int) f);
		}
	}
	const size_t m = face_of_free_.size();

	// Assemble the free-row operator H (Hermitian positive definite) in
	// CSR form: diagonal A_f + tau*sum(w), off-diagonal -tau*w*T_fg.
	vector<double> diag(m, 0.0);
	for (size_t i = 0; i < m; i++)
		diag[i] = area_[face_of_free_[i]];
	op_rowptr_.assign(m + 1, 0);
	for (size_t i = 0; i < m; i++)
		op_rowptr_[i + 1] = 1;      // room for the diagonal
	for (size_t k = 0; k < edges_.size(); k++) {
		const Edge &e = edges_[k];
		int fi = free_of_face_[e.f], gi = free_of_face_[e.g];
		double tw = tau * e.w;
		if (fi >= 0)
			diag[fi] += tw;
		if (gi >= 0)
			diag[gi] += tw;
		if (fi >= 0 && gi >= 0) {
			op_rowptr_[fi + 1]++;
			op_rowptr_[gi + 1]++;
		}
	}
	for (size_t i = 0; i < m; i++)
		op_rowptr_[i + 1] += op_rowptr_[i];
	op_col_.assign(op_rowptr_[m], 0);
	op_val_.assign(op_rowptr_[m], cdouble(0.0, 0.0));
	vector<int> next(op_rowptr_.begin(), op_rowptr_.end() - (m ? 1 : 0));
	for (size_t i = 0; i < m; i++) {   // diagonal first in each row
		int k = next[i]++;
		op_col_[k] = (int) i;
		op_val_[k] = cdouble(diag[i], 0.0);
	}
	for (size_t k = 0; k < edges_.size(); k++) {
		const Edge &e = edges_[k];
		int fi = free_of_face_[e.f], gi = free_of_face_[e.g];
		if (fi < 0 || gi < 0)
			continue;
		cdouble off = -tau_ * e.w * e.t;
		int p = next[fi]++;
		op_col_[p] = gi;
		op_val_[p] = off;
		p = next[gi]++;
		op_col_[p] = fi;
		op_val_[p] = conj(off);
	}

	// Jacobi preconditioner (diagonal is real and strictly positive).
	diag_inv_.assign(m, 0.0);
	for (size_t i = 0; i < m; i++)
		diag_inv_[i] = 1.0 / diag[i];
}


FaceFieldSystem::Result
FaceFieldSystem::solve(const vector<cdouble> &u,
                       const vector<cdouble> &h) const
{
	const size_t nf = area_.size();
	if (u.size() != nf)
		throw invalid_argument("FaceFieldSystem::solve: u must have one entry per face");
	if (h.size() != constraints_.size())
		throw invalid_argument("FaceFieldSystem::solve: h must have one entry per constraint");
	for (size_t f = 0; f < nf; f++) {
		if (!isfinite(u[f].real()) || !isfinite(u[f].imag()))
			throw invalid_argument("FaceFieldSystem::solve: non-finite input value");
	}
	for (size_t k = 0; k < h.size(); k++) {
		if (!isfinite(h[k].real()) || !isfinite(h[k].imag()))
			throw invalid_argument("FaceFieldSystem::solve: non-finite constraint value");
	}

	Result out;
	out.z.assign(nf, cdouble(0.0, 0.0));
	out.residual = 0.0;
	for (size_t k = 0; k < constraints_.size(); k++)
		out.z[constraints_[k]] = h[k];

	const size_t m = face_of_free_.size();
	if (m == 0)
		return out;                 // fully constrained: nothing to solve

	// Right-hand side: b_f = A_f u_f + tau * sum_{g constrained} w T_fg h_g.
	vector<cdouble> b(m, cdouble(0.0, 0.0));
	for (size_t i = 0; i < m; i++)
		b[i] = area_[face_of_free_[i]] * u[face_of_free_[i]];
	for (size_t k = 0; k < edges_.size(); k++) {
		const Edge &e = edges_[k];
		const int pf = constraint_pos_[e.f], pg = constraint_pos_[e.g];
		const cdouble tw = tau_ * e.w * e.t;
		if (pf < 0 && pg >= 0)
			b[free_of_face_[e.f]] += tw * h[pg];
		else if (pg < 0 && pf >= 0)
			b[free_of_face_[e.g]] += conj(tw) * h[pf];
	}

	const double bnorm = max(1.0, norm_inf(b));
	const double tol = 1e-11 * bnorm;   // 100x tighter than required

	// Preconditioned conjugate gradients on the Hermitian positive
	// definite free-row system H x = b.
	vector<cdouble> x(m, cdouble(0.0, 0.0)), r(b), z(m), p(m), q(m);
	for (size_t i = 0; i < m; i++)
		z[i] = diag_inv_[i] * r[i];
	p = z;
	double rz = cdot(r, z).real();
	double rinf = norm_inf(r);
	const size_t maxiter = 20 * m + 1000;
	size_t iter = 0;
	while (rinf > tol) {
		if (iter >= maxiter)
			throw runtime_error("FaceFieldSystem::solve: conjugate gradients did not converge");
		csr_matvec(op_rowptr_, op_col_, op_val_, p, q);
		double alpha = rz / cdot(p, q).real();
		for (size_t i = 0; i < m; i++) {
			x[i] += alpha * p[i];
			r[i] -= alpha * q[i];
		}
		iter++;
		if (iter % 32 == 0) {       // keep the recursive residual honest
			csr_matvec(op_rowptr_, op_col_, op_val_, x, q);
			for (size_t i = 0; i < m; i++)
				r[i] = b[i] - q[i];
		}
		rinf = norm_inf(r);
		for (size_t i = 0; i < m; i++)
			z[i] = diag_inv_[i] * r[i];
		double rz_new = cdot(r, z).real();
		double beta = (rz != 0.0) ? rz_new / rz : 0.0;
		for (size_t i = 0; i < m; i++)
			p[i] = z[i] + beta * p[i];
		rz = rz_new;
	}

	// Exact final residual of the free rows.
	csr_matvec(op_rowptr_, op_col_, op_val_, x, q);
	for (size_t i = 0; i < m; i++)
		r[i] = b[i] - q[i];
	rinf = norm_inf(r);
	if (rinf > 1e-9 * bnorm)
		throw runtime_error("FaceFieldSystem::solve: failed to reach the required residual");

	for (size_t i = 0; i < m; i++)
		out.z[face_of_free_[i]] = x[i];
	out.residual = rinf / bnorm;
	return out;
}

} // namespace trimesh
