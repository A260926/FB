/*
tests/face_field.cc

Test cases for trimesh::FaceFieldSystem (include/FaceField.h), registered
with the task driver through task_cases / task_case_count.  No main() here;
task_support/driver.c provides it.

Groups:
  connection - transport across unfolded edges, holonomy on closed shells,
               local re-basing equivariance, input validation
  diffusion  - the quadratic minimizer itself: dense reference solves,
               constraint elimination, multiple right-hand sides,
               rigid-motion / scaling invariance, no renormalization
  reuse      - one system, many solves: data independence, determinism,
               linear storage growth
*/

#include "FaceField.h"
#include "TriMesh.h"
#include "task_support/check.h"

#include <cmath>
#include <complex>
#include <limits>
#include <map>
#include <random>
#include <utility>
#include <vector>

using namespace trimesh;

typedef std::complex<double> cd;
typedef Vec<3,double> dvec;

namespace {

const double PI = 3.14159265358979323846;

// ------------------------------------------------------------------
// Mesh builders
// ------------------------------------------------------------------

// Triangulated quads x quads patch; bent into a circular arc when requested.
TriMesh make_grid(int quads, bool bent)
{
	TriMesh m;
	const int q = quads;
	for (int j = 0; j <= q; j++) {
		for (int i = 0; i <= q; i++) {
			double x, z;
			const double y = (double) j / q;
			if (bent) {
				const double s = 1.7 * ((double) i / q - 0.5);
				x = std::sin(s);
				z = 1.0 - std::cos(s);
			} else {
				x = (double) i / q;
				z = 0.0;
			}
			m.vertices.push_back(point((float) x, (float) y, (float) z));
		}
	}
	for (int j = 0; j < q; j++) {
		for (int i = 0; i < q; i++) {
			const int a = j * (q + 1) + i;
			const int b = a + 1;
			const int d = a + q + 1;
			const int c = d + 1;
			m.faces.push_back(TriMesh::Face(a, b, c));
			m.faces.push_back(TriMesh::Face(a, c, d));
		}
	}
	return m;
}

// Outward-oriented tetrahedron.
TriMesh make_tetra()
{
	TriMesh m;
	m.vertices.push_back(point(0, 0, 0));
	m.vertices.push_back(point(1, 0, 0));
	m.vertices.push_back(point(0, 1, 0));
	m.vertices.push_back(point(0, 0, 1));
	m.faces.push_back(TriMesh::Face(0, 2, 1));
	m.faces.push_back(TriMesh::Face(0, 1, 3));
	m.faces.push_back(TriMesh::Face(1, 2, 3));
	m.faces.push_back(TriMesh::Face(2, 0, 3));
	return m;
}

// Bent sheet whose coordinates are exact in binary floating point
// (for rigid-motion and scaling checks with tight tolerances).
TriMesh make_exact_sheet()
{
	TriMesh m;
	const int qi = 6, qj = 4;
	for (int j = 0; j <= qj; j++)
		for (int i = 0; i <= qi; i++)
			m.vertices.push_back(point(0.5f * i, 0.5f * j, 0.0625f * (i * i)));
	for (int j = 0; j < qj; j++) {
		for (int i = 0; i < qi; i++) {
			const int a = j * (qi + 1) + i;
			const int b = a + 1;
			const int d = a + qi + 1;
			const int c = d + 1;
			m.faces.push_back(TriMesh::Face(a, b, c));
			m.faces.push_back(TriMesh::Face(a, c, d));
		}
	}
	return m;
}

// ------------------------------------------------------------------
// Geometry helpers (independent reimplementations of the spec formulas)
// ------------------------------------------------------------------

dvec face_normal(const TriMesh &m, int f)
{
	const TriMesh::Face &F = m.faces[f];
	dvec p0(m.vertices[F[0]]), p1(m.vertices[F[1]]), p2(m.vertices[F[2]]);
	dvec nrm = (p1 - p0) CROSS (p2 - p0);
	normalize(nrm);
	return nrm;
}

double face_area(const TriMesh &m, int f)
{
	const TriMesh::Face &F = m.faces[f];
	dvec p0(m.vertices[F[0]]), p1(m.vertices[F[1]]), p2(m.vertices[F[2]]);
	return 0.5 * len((p1 - p0) CROSS (p2 - p0));
}

// A valid smooth per-face tangent basis.
std::vector<vec> reference_basis(const TriMesh &m)
{
	dvec a(0.26, 0.53, 0.81);
	normalize(a);
	std::vector<vec> e1(m.faces.size());
	for (size_t f = 0; f < m.faces.size(); f++) {
		dvec b = face_normal(m, (int) f) CROSS a;
		normalize(b);
		e1[f] = vec((float) b[0], (float) b[1], (float) b[2]);
	}
	return e1;
}

// Independent rebuild of the connection: areas, frames, edge weights and
// transport factors, straight from the specification.
struct RefEdge {
	int f, g;
	double w;
	cd T;
};

struct RefProblem {
	std::vector<double> area;
	std::vector<dvec> e1, e2;
	std::vector<RefEdge> edges;
};

RefProblem build_ref(const TriMesh &m, const std::vector<vec> &e1v, int n)
{
	RefProblem R;
	const size_t nf = m.faces.size();
	R.area.resize(nf);
	R.e1.resize(nf);
	R.e2.resize(nf);
	for (size_t f = 0; f < nf; f++) {
		R.area[f] = face_area(m, (int) f);
		dvec nrm = face_normal(m, (int) f);
		dvec b(e1v[f]);
		R.e1[f] = b;
		R.e2[f] = nrm CROSS b;
	}
	typedef std::map< std::pair<int,int>, std::vector<int> > edgemap_t;
	edgemap_t em;
	for (size_t f = 0; f < nf; f++) {
		const TriMesh::Face &F = m.faces[f];
		for (int j = 0; j < 3; j++) {
			int v0 = F[j], v1 = F[(j + 1) % 3];
			if (v0 > v1)
				std::swap(v0, v1);
			em[std::make_pair(v0, v1)].push_back((int) f);
		}
	}
	for (edgemap_t::const_iterator it = em.begin(); it != em.end(); ++it) {
		if (it->second.size() != 2)
			continue;
		const int f0 = it->second[0], f1 = it->second[1];
		dvec d(m.vertices[it->first.second]);
		d -= dvec(m.vertices[it->first.first]);
		const double l2 = len2(d);
		d /= std::sqrt(l2);
		const double th0 = std::atan2(d DOT R.e2[f0], d DOT R.e1[f0]);
		const double th1 = std::atan2(d DOT R.e2[f1], d DOT R.e1[f1]);
		RefEdge e;
		e.f = f0;
		e.g = f1;
		e.w = l2 / (R.area[f0] + R.area[f1]);
		e.T = std::exp(cd(0.0, (double) n * (th0 - th1)));
		R.edges.push_back(e);
	}
	return R;
}

const RefEdge *find_edge(const RefProblem &R, int a, int b)
{
	for (size_t i = 0; i < R.edges.size(); i++) {
		if ((R.edges[i].f == a && R.edges[i].g == b) ||
		    (R.edges[i].f == b && R.edges[i].g == a))
			return &R.edges[i];
	}
	return 0;
}

// T_ab as it appears in |z_a - T_ab z_b|^2
cd transport(const RefEdge &e, int a, int /* b */)
{
	return (e.f == a) ? e.T : std::conj(e.T);
}

double ref_energy(const RefProblem &R, double tau,
                  const std::vector<cd> &z, const std::vector<cd> &u)
{
	double E = 0.0;
	for (size_t f = 0; f < z.size(); f++)
		E += R.area[f] * std::norm(z[f] - u[f]);
	for (size_t k = 0; k < R.edges.size(); k++) {
		const RefEdge &e = R.edges[k];
		E += tau * e.w * std::norm(z[e.f] - e.T * z[e.g]);
	}
	return E;
}

// Dense Gaussian elimination with partial pivoting (complex, row major).
std::vector<cd> dense_solve(std::vector<cd> A, std::vector<cd> b, int n)
{
	for (int col = 0; col < n; col++) {
		int piv = col;
		for (int r = col + 1; r < n; r++)
			if (std::abs(A[r * n + col]) > std::abs(A[piv * n + col]))
				piv = r;
		for (int c = col; c < n; c++)
			std::swap(A[piv * n + c], A[col * n + c]);
		std::swap(b[piv], b[col]);
		for (int r = col + 1; r < n; r++) {
			cd f = A[r * n + col] / A[col * n + col];
			if (f == cd(0.0, 0.0))
				continue;
			for (int c = col; c < n; c++)
				A[r * n + c] -= f * A[col * n + c];
			b[r] -= f * b[col];
		}
	}
	std::vector<cd> x(n);
	for (int r = n - 1; r >= 0; r--) {
		cd s = b[r];
		for (int c = r + 1; c < n; c++)
			s -= A[r * n + c] * x[c];
		x[r] = s / A[r * n + r];
	}
	return x;
}

// Independent dense solve of the constrained quadratic problem:
// eliminate constrained rows/columns, solve the free system directly.
std::vector<cd> ref_solve(const RefProblem &R, double tau,
                          const std::vector<int> &cons,
                          const std::vector<cd> &u,
                          const std::vector<cd> &h)
{
	const size_t nf = R.area.size();
	std::vector<cd> z(nf, cd(0.0, 0.0));
	std::vector<int> free_of(nf, -1), pos_of(nf, -1), faces;
	for (size_t k = 0; k < cons.size(); k++) {
		pos_of[cons[k]] = (int) k;
		z[cons[k]] = h[k];
	}
	for (size_t f = 0; f < nf; f++) {
		if (pos_of[f] < 0) {
			free_of[f] = (int) faces.size();
			faces.push_back((int) f);
		}
	}
	const int m = (int) faces.size();
	if (!m)
		return z;
	std::vector<cd> H((size_t) m * m, cd(0.0, 0.0)), b(m, cd(0.0, 0.0));
	for (int i = 0; i < m; i++) {
		const int f = faces[i];
		H[i * m + i] = cd(R.area[f], 0.0);
		b[i] = R.area[f] * u[f];
	}
	for (size_t k = 0; k < R.edges.size(); k++) {
		const RefEdge &e = R.edges[k];
		const int fi = free_of[e.f], gi = free_of[e.g];
		const double tw = tau * e.w;
		if (fi >= 0)
			H[fi * m + fi] += cd(tw, 0.0);
		if (gi >= 0)
			H[gi * m + gi] += cd(tw, 0.0);
		if (fi >= 0 && gi >= 0) {
			H[fi * m + gi] -= tw * e.T;
			H[gi * m + fi] -= tw * std::conj(e.T);
		}
		if (fi >= 0 && gi < 0)
			b[fi] += tw * e.T * h[pos_of[e.g]];
		if (gi >= 0 && fi < 0)
			b[gi] += tw * std::conj(e.T) * h[pos_of[e.f]];
	}
	std::vector<cd> x = dense_solve(H, b, m);
	for (int i = 0; i < m; i++)
		z[faces[i]] = x[i];
	return z;
}

double maxdiff(const std::vector<cd> &a, const std::vector<cd> &b)
{
	double d = 0.0;
	for (size_t i = 0; i < a.size(); i++)
		d = std::max(d, std::abs(a[i] - b[i]));
	return d;
}

// Expect an std::invalid_argument from the statement.
#define EXPECT_INVALID(...) \
	do { \
		bool ff_ok = false; \
		try { __VA_ARGS__; } \
		catch (const std::invalid_argument &) { ff_ok = true; } \
		catch (...) {} \
		TASK_CHECK(ff_ok); \
	} while (0)

// ------------------------------------------------------------------
// connection group
// ------------------------------------------------------------------

// Two triangles folded along their common edge: transport must unfold the
// dihedral, not project onto a world plane.
int test_folded_pair_unfolding()
{
	TriMesh m;
	m.vertices.push_back(point(0, 0, 0));
	m.vertices.push_back(point(1, 0, 0));
	m.vertices.push_back(point(0, 1, 0));
	m.vertices.push_back(point(0.0f, -0.8f, 0.6f));
	m.faces.push_back(TriMesh::Face(0, 1, 2));
	m.faces.push_back(TriMesh::Face(1, 0, 3));

	std::vector<vec> e1;
	e1.push_back(vec(1, 0, 0));
	e1.push_back(vec(1, 0, 0));
	std::vector<int> cons(1, 0);
	const double tau = 1e6;
	const cd h1 = std::exp(cd(0.0, PI / 4.0));
	const std::vector<cd> u(2, cd(0.0, 0.0));

	// With these bases theta_A = theta_B = 0, so T = 1 and the free face
	// takes z_1 = tau*w/(A_1 + tau*w) * h with w = 1, A_1 = 0.5.
	FaceFieldSystem sys(m, e1, 1, tau, cons);
	FaceFieldSystem::Result r = sys.solve(u, std::vector<cd>(1, h1));
	const double c = tau / (0.5 + tau);
	TASK_CHECK(r.residual <= 1e-9);
	TASK_CHECK(r.z[0] == h1);
	TASK_CHECK(std::abs(std::abs(r.z[1]) - c) < 1e-9);
	TASK_CHECK(std::abs(std::arg(r.z[1]) - PI / 4.0) < 1e-9);

	// The direction z_1 represents in face 1's world frame must be the
	// unfolded image of face 0's direction.
	const dvec N1(0.0, 0.6, 0.8), E1(1.0, 0.0, 0.0), E2(0.0, 0.8, -0.6);
	const double ang = std::arg(r.z[1]);
	const dvec dir = std::cos(ang) * E1 + std::sin(ang) * E2;
	const dvec unfolded = std::cos(PI / 4.0) * E1 + std::sin(PI / 4.0) * E2;
	TASK_CHECK(len(dir - unfolded) < 1e-8);
	// Projecting face 0's direction onto face 1's plane in world space
	// would give a measurably different angle.
	const dvec v0 = std::cos(PI / 4.0) * dvec(1, 0, 0) +
	                std::sin(PI / 4.0) * dvec(0, 1, 0);
	dvec proj = v0 - N1 * (v0 DOT N1);
	normalize(proj);
	const double cosang = std::min(1.0, std::abs(proj DOT dir));
	TASK_CHECK(std::acos(cosang) > 0.05);

	// Rotate face 1's basis by +pi/2 (e1' = e2): the coefficient must
	// transform by exp(-i*pi/2) while the physics is unchanged.
	std::vector<vec> e1r;
	e1r.push_back(vec(1, 0, 0));
	e1r.push_back(vec(0.0f, 0.8f, -0.6f));
	FaceFieldSystem sysr(m, e1r, 1, tau, cons);
	FaceFieldSystem::Result rr = sysr.solve(u, std::vector<cd>(1, h1));
	TASK_CHECK(rr.residual <= 1e-9);
	TASK_CHECK(std::abs(rr.z[1] - c * std::exp(cd(0.0, PI / 4.0 - PI / 2.0))) < 1e-9);

	// n = 4 on the same geometry: h = exp(i*4*pi/4) = -1, T = 1.
	FaceFieldSystem sys4(m, e1, 4, tau, cons);
	FaceFieldSystem::Result r4 = sys4.solve(u, std::vector<cd>(1, cd(-1.0, 0.0)));
	TASK_CHECK(r4.residual <= 1e-9);
	TASK_CHECK(std::abs(r4.z[1] - cd(-c, 0.0)) < 1e-9);
	return 0;
}

// A closed tetrahedron cannot carry a globally compatible unit field:
// holonomy around a vertex is nontrivial and incompatible constraints
// force a strictly positive minimum energy.
int test_closed_shell_incompatible()
{
	TriMesh m = make_tetra();
	std::vector<vec> e1 = reference_basis(m);
	const int n = 1;
	RefProblem R = build_ref(m, e1, n);

	// Product of transport factors around the dual cycle enclosing
	// vertex 0 (faces 0 -> 1 -> 3 -> 0) is not 1.
	const RefEdge *e01 = find_edge(R, 0, 1);
	const RefEdge *e13 = find_edge(R, 1, 3);
	const RefEdge *e03 = find_edge(R, 0, 3);
	TASK_CHECK(e01 && e13 && e03);
	const cd P = transport(*e01, 0, 1) * transport(*e13, 1, 3) *
	             transport(*e03, 3, 0);
	TASK_CHECK(std::abs(P - cd(1.0, 0.0)) > 0.5);

	// Incompatible unit constraints on the two sides of edge (0,1).
	std::vector<int> cons;
	cons.push_back(0);
	cons.push_back(1);
	const std::vector<cd> h(2, cd(1.0, 0.0));
	const std::vector<cd> u(4, cd(0.0, 0.0));
	const double tau = 1.0;
	FaceFieldSystem sys(m, e1, n, tau, cons);
	FaceFieldSystem::Result r = sys.solve(u, h);
	TASK_CHECK(r.residual <= 1e-9);
	TASK_CHECK(r.z[0] == cd(1.0, 0.0) && r.z[1] == cd(1.0, 0.0));

	const double E = ref_energy(R, tau, r.z, u);
	const cd T01 = transport(*e01, 0, 1);
	const double gap = std::norm(cd(1.0, 0.0) - T01);
	TASK_CHECK(gap > 1e-4);
	TASK_CHECK(E >= tau * e01->w * gap - 1e-12);
	TASK_CHECK(E > 1e-6);

	// The returned field is the minimizer: perturbing free rows can
	// only raise the energy.
	std::mt19937 rng(7);
	std::uniform_real_distribution<double> U(-0.02, 0.02);
	for (int t = 0; t < 5; t++) {
		std::vector<cd> zp = r.z;
		zp[2] += cd(U(rng), U(rng));
		zp[3] += cd(U(rng), U(rng));
		TASK_CHECK(ref_energy(R, tau, zp, u) >= E - 1e-12);
	}
	return 0;
}

// Rotating every local basis e1_f by phi_f must rotate the complex
// coefficients by exp(-i*n*phi_f), for inputs, constraints and outputs.
int test_random_rebasis()
{
	TriMesh m = make_grid(5, true);
	const size_t nf = m.faces.size();
	const int n = 3;
	const double tau = 2.5;
	std::mt19937 rng(11);
	std::uniform_real_distribution<double> U(-1.0, 1.0);

	std::vector<dvec> N(nf);
	std::vector<vec> e1(nf);
	for (size_t f = 0; f < nf; f++) {
		N[f] = face_normal(m, (int) f);
		dvec b = N[f] CROSS dvec(0.26, 0.53, 0.81);
		normalize(b);
		const double ph = U(rng) * PI;
		const dvec e2 = N[f] CROSS b;
		const dvec r = std::cos(ph) * b + std::sin(ph) * e2;
		e1[f] = vec((float) r[0], (float) r[1], (float) r[2]);
	}
	std::vector<int> cons;
	cons.push_back(3);
	cons.push_back(17);
	cons.push_back(42);
	std::vector<cd> u(nf), h(3);
	for (size_t f = 0; f < nf; f++)
		u[f] = cd(U(rng), U(rng));
	u[7] = cd(0.0, 0.0);                    // zero input is legal
	h[0] = cd(U(rng), U(rng));
	h[1] = cd(0.0, 0.0);                    // zero constraint is legal
	h[2] = cd(U(rng), U(rng));

	FaceFieldSystem sys(m, e1, n, tau, cons);
	FaceFieldSystem::Result r1 = sys.solve(u, h);
	TASK_CHECK(r1.residual <= 1e-9);

	// Rebuild with every basis rotated by a fresh random angle.
	std::vector<vec> e1r(nf);
	std::vector<cd> ur(nf), hr(3);
	std::vector<double> phi(nf);
	for (size_t f = 0; f < nf; f++) {
		phi[f] = U(rng) * PI;
		const dvec b(e1[f]);
		const dvec e2 = N[f] CROSS b;
		const dvec r = std::cos(phi[f]) * b + std::sin(phi[f]) * e2;
		e1r[f] = vec((float) r[0], (float) r[1], (float) r[2]);
		const cd g = std::exp(cd(0.0, -(double) n * phi[f]));
		ur[f] = u[f] * g;
	}
	for (size_t k = 0; k < 3; k++)
		hr[k] = h[k] * std::exp(cd(0.0, -(double) n * phi[cons[k]]));

	FaceFieldSystem sys2(m, e1r, n, tau, cons);
	FaceFieldSystem::Result r2 = sys2.solve(ur, hr);
	TASK_CHECK(r2.residual <= 1e-9);
	double err = 0.0;
	for (size_t f = 0; f < nf; f++)
		err = std::max(err, std::abs(r2.z[f] -
		       r1.z[f] * std::exp(cd(0.0, -(double) n * phi[f]))));
	TASK_CHECK(err < 1e-5);
	return 0;
}

// Invalid geometry, bases, parameters, constraints and solve inputs all
// throw std::invalid_argument.
int test_invalid_inputs()
{
	TriMesh m = make_tetra();
	std::vector<vec> e1 = reference_basis(m);
	std::vector<int> cons(1, 0);
	const double nan = std::numeric_limits<double>::quiet_NaN();
	const float nanf = std::numeric_limits<float>::quiet_NaN();
	const double inf = std::numeric_limits<double>::infinity();

	FaceFieldSystem ok(m, e1, 2, 1.0, cons);
	TASK_CHECK(ok.face_count() == 4 && ok.constraint_count() == 1);

	EXPECT_INVALID(FaceFieldSystem(m, e1, 0, 1.0, cons));
	EXPECT_INVALID(FaceFieldSystem(m, e1, 9, 1.0, cons));
	EXPECT_INVALID(FaceFieldSystem(m, e1, -3, 1.0, cons));
	EXPECT_INVALID(FaceFieldSystem(m, e1, 2, 0.0, cons));
	EXPECT_INVALID(FaceFieldSystem(m, e1, 2, -1.0, cons));
	EXPECT_INVALID(FaceFieldSystem(m, e1, 2, nan, cons));
	EXPECT_INVALID(FaceFieldSystem(m, e1, 2, inf, cons));
	{
		std::vector<vec> b = e1;
		b.pop_back();
		EXPECT_INVALID(FaceFieldSystem(m, b, 2, 1.0, cons));
	}
	{
		std::vector<vec> b = e1;
		b[1] = vec(2, 0, 0);                    // not unit
		EXPECT_INVALID(FaceFieldSystem(m, b, 2, 1.0, cons));
	}
	{
		std::vector<vec> b = e1;
		const dvec nrm = face_normal(m, 2);
		b[2] = vec((float) nrm[0], (float) nrm[1], (float) nrm[2]);
		EXPECT_INVALID(FaceFieldSystem(m, b, 2, 1.0, cons)); // not tangent
	}
	{
		std::vector<vec> b = e1;
		b[0] = vec(nanf, 0, 0);
		EXPECT_INVALID(FaceFieldSystem(m, b, 2, 1.0, cons));
	}
	{
		std::vector<int> c;
		c.push_back(1);
		c.push_back(1);
		EXPECT_INVALID(FaceFieldSystem(m, e1, 2, 1.0, c));   // duplicate
	}
	{
		std::vector<int> c(1, -1);
		EXPECT_INVALID(FaceFieldSystem(m, e1, 2, 1.0, c));
	}
	{
		std::vector<int> c(1, 4);
		EXPECT_INVALID(FaceFieldSystem(m, e1, 2, 1.0, c));   // out of range
	}
	{
		TriMesh b = m;
		b.faces[0] = TriMesh::Face(0, 1, 99);
		EXPECT_INVALID(FaceFieldSystem(b, e1, 2, 1.0, cons));
	}
	{
		TriMesh b = m;
		b.faces[0] = TriMesh::Face(0, 1, -2);
		EXPECT_INVALID(FaceFieldSystem(b, e1, 2, 1.0, cons));
	}
	{
		TriMesh b = m;
		b.vertices[1] = point(nanf, 0, 0);
		EXPECT_INVALID(FaceFieldSystem(b, e1, 2, 1.0, cons));
	}
	{
		TriMesh b;                              // zero-area face
		b.vertices.push_back(point(0, 0, 0));
		b.vertices.push_back(point(1, 0, 0));
		b.vertices.push_back(point(2, 0, 0));
		b.faces.push_back(TriMesh::Face(0, 1, 2));
		std::vector<vec> be(1, vec(0, 1, 0));
		EXPECT_INVALID(FaceFieldSystem(b, be, 2, 1.0, std::vector<int>()));
	}
	{
		TriMesh b;                              // repeated vertex
		b.vertices.push_back(point(0, 0, 0));
		b.vertices.push_back(point(1, 0, 0));
		b.vertices.push_back(point(0, 1, 0));
		b.faces.push_back(TriMesh::Face(0, 1, 1));
		std::vector<vec> be(1, vec(1, 0, 0));
		EXPECT_INVALID(FaceFieldSystem(b, be, 2, 1.0, std::vector<int>()));
	}
	{
		TriMesh b;                              // edge shared by 3 faces
		b.vertices.push_back(point(0, 0, 0));
		b.vertices.push_back(point(1, 0, 0));
		b.vertices.push_back(point(0, 1, 0));
		b.vertices.push_back(point(0, 0, 1));
		b.vertices.push_back(point(1, 1, 0));
		b.faces.push_back(TriMesh::Face(0, 1, 2));
		b.faces.push_back(TriMesh::Face(1, 0, 3));
		b.faces.push_back(TriMesh::Face(0, 1, 4));
		std::vector<vec> be(3, vec(1, 0, 0));
		EXPECT_INVALID(FaceFieldSystem(b, be, 2, 1.0, std::vector<int>()));
	}
	{
		TriMesh b;                              // same-direction edge use
		b.vertices.push_back(point(0, 0, 0));
		b.vertices.push_back(point(1, 0, 0));
		b.vertices.push_back(point(0, 1, 0));
		b.vertices.push_back(point(0, 0, 1));
		b.faces.push_back(TriMesh::Face(0, 1, 2));
		b.faces.push_back(TriMesh::Face(0, 1, 3));
		std::vector<vec> be(2, vec(1, 0, 0));
		EXPECT_INVALID(FaceFieldSystem(b, be, 2, 1.0, std::vector<int>()));
	}

	FaceFieldSystem sys(m, e1, 2, 1.0, cons);
	const std::vector<cd> u(4, cd(0.0, 0.0)), h(1, cd(1.0, 0.0));
	EXPECT_INVALID(sys.solve(std::vector<cd>(3, cd(0.0, 0.0)), h));
	EXPECT_INVALID(sys.solve(u, std::vector<cd>(2, cd(0.0, 0.0))));
	{
		std::vector<cd> ub = u;
		ub[1] = cd(nan, 0.0);
		EXPECT_INVALID(sys.solve(ub, h));
	}
	{
		std::vector<cd> hb = h;
		hb[0] = cd(0.0, inf);
		EXPECT_INVALID(sys.solve(u, hb));
	}
	return 0;
}

// ------------------------------------------------------------------
// diffusion group
// ------------------------------------------------------------------

// The library result must match an independent dense assembly and solve
// of the documented quadratic problem, for every symmetry order.
int test_dense_reference()
{
	std::mt19937 rng(23);
	std::uniform_real_distribution<double> U(-1.0, 1.0);
	TriMesh m = make_tetra();
	std::vector<vec> e1 = reference_basis(m);
	std::vector<int> cons;
	cons.push_back(0);
	cons.push_back(2);
	const int ns[8] = {1, 2, 3, 4, 5, 6, 7, 8};
	for (int t = 0; t < 8; t++) {
		const int n = ns[t];
		std::vector<cd> u(4), h(2);
		for (int f = 0; f < 4; f++)
			u[f] = cd(U(rng), U(rng));
		u[1] = cd(0.0, 0.0);
		h[0] = (n == 3) ? cd(0.0, 0.0) : cd(U(rng), U(rng));
		h[1] = cd(U(rng), U(rng));
		const double tau = 0.4 + (double) t;
		FaceFieldSystem sys(m, e1, n, tau, cons);
		FaceFieldSystem::Result r = sys.solve(u, h);
		TASK_CHECK(r.residual <= 1e-9);
		TASK_CHECK(r.z[0] == h[0] && r.z[2] == h[1]);
		RefProblem R = build_ref(m, e1, n);
		std::vector<cd> zref = ref_solve(R, tau, cons, u, h);
		TASK_CHECK(maxdiff(r.z, zref) < 1e-8);
	}

	// Flat 3x3 grid (18 faces), n = 4, three constraints.
	TriMesh g = make_grid(3, false);
	std::vector<vec> ge = reference_basis(g);
	std::vector<int> gcons;
	gcons.push_back(0);
	gcons.push_back(5);
	gcons.push_back(17);
	std::vector<cd> gu(18), gh(3);
	for (int i = 0; i < 18; i++)
		gu[i] = cd(U(rng), U(rng));
	for (int k = 0; k < 3; k++)
		gh[k] = cd(U(rng), U(rng));
	FaceFieldSystem gsys(g, ge, 4, 0.3, gcons);
	FaceFieldSystem::Result gr = gsys.solve(gu, gh);
	TASK_CHECK(gr.residual <= 1e-9);
	RefProblem GR = build_ref(g, ge, 4);
	std::vector<cd> gzref = ref_solve(GR, 0.3, gcons, gu, gh);
	TASK_CHECK(maxdiff(gr.z, gzref) < 1e-8);
	return 0;
}

// Constraint elimination and repeated right-hand-side updates on one
// system, checked against dense reduced solves; plus degenerate cases
// (no constraints, all constrained, empty mesh) and no renormalization.
int test_constraint_elimination_rhs()
{
	TriMesh m = make_grid(8, true);           // 128 faces
	std::vector<vec> e1 = reference_basis(m);
	const int n = 2;
	const double tau = 0.7;
	std::vector<int> cons;
	const int cinit[10] = {4, 13, 27, 38, 51, 66, 79, 92, 105, 120};
	cons.assign(cinit, cinit + 10);
	std::mt19937 rng(31);
	std::uniform_real_distribution<double> U(-1.0, 1.0);

	FaceFieldSystem sys(m, e1, n, tau, cons);
	TASK_CHECK(sys.face_count() == 128);
	TASK_CHECK(sys.free_count() == 118);
	TASK_CHECK(sys.constraint_count() == 10);
	RefProblem R = build_ref(m, e1, n);

	for (int round = 0; round < 2; round++) {   // multiple RHS updates
		std::vector<cd> u(128), h(10);
		for (int i = 0; i < 128; i++)
			u[i] = cd(U(rng), U(rng));
		for (int k = 0; k < 10; k++)
			h[k] = cd(U(rng), U(rng));
		FaceFieldSystem::Result r = sys.solve(u, h);
		TASK_CHECK(r.residual <= 1e-9);
		std::vector<cd> zref = ref_solve(R, tau, cons, u, h);
		TASK_CHECK(maxdiff(r.z, zref) < 1e-6);
		bool exact = true;
		for (size_t k = 0; k < cons.size(); k++)
			if (r.z[cons[k]] != h[k])
				exact = false;
		TASK_CHECK(exact);
	}

	// No constraints at all.
	{
		FaceFieldSystem s2(m, e1, n, tau, std::vector<int>());
		std::vector<cd> u(128);
		for (int i = 0; i < 128; i++)
			u[i] = cd(U(rng), U(rng));
		FaceFieldSystem::Result r = s2.solve(u, std::vector<cd>());
		TASK_CHECK(r.residual <= 1e-9);
		std::vector<cd> zref = ref_solve(R, tau, std::vector<int>(),
		                                 u, std::vector<cd>());
		TASK_CHECK(maxdiff(r.z, zref) < 1e-6);
	}

	// All faces constrained: returned as-is, zero residual.
	{
		TriMesh t = make_tetra();
		std::vector<vec> te = reference_basis(t);
		std::vector<int> all;
		all.push_back(0);
		all.push_back(1);
		all.push_back(2);
		all.push_back(3);
		std::vector<cd> h;
		h.push_back(cd(1.0, 2.0));
		h.push_back(cd(0.0, 0.0));
		h.push_back(cd(-1.0, 0.5));
		h.push_back(cd(3.0, -2.0));
		FaceFieldSystem s3(t, te, 1, 1.0, all);
		TASK_CHECK(s3.free_count() == 0);
		FaceFieldSystem::Result r =
			s3.solve(std::vector<cd>(4, cd(9.0, 9.0)), h);
		TASK_CHECK(r.residual == 0.0);
		TASK_CHECK(r.z == h);
	}

	// Tiny inputs stay tiny: the field is never renormalized.
	{
		TriMesh g = make_grid(4, false);      // flat, aligned bases
		std::vector<vec> ge(32, vec(1, 0, 0));
		FaceFieldSystem s4(g, ge, 1, 1.0, std::vector<int>());
		const cd tiny(1e-6, 0.5e-6);
		std::vector<cd> u(32, tiny);
		FaceFieldSystem::Result r = s4.solve(u, std::vector<cd>());
		TASK_CHECK(r.residual <= 1e-9);
		double maxd = 0.0, maxm = 0.0;
		for (size_t f = 0; f < 32; f++) {
			maxd = std::max(maxd, std::abs(r.z[f] - tiny));
			maxm = std::max(maxm, std::abs(r.z[f]));
		}
		TASK_CHECK(maxd < 1e-7);
		TASK_CHECK(maxm < 1e-3);      // a normalized result would be ~1
	}

	// Multiple connected components solve independently.
	{
		TriMesh tet = make_tetra();
		TriMesh grid = make_grid(3, true);
		TriMesh both = tet;
		const int voff = (int) both.vertices.size();
		for (size_t i = 0; i < grid.vertices.size(); i++)
			both.vertices.push_back(grid.vertices[i]);
		for (size_t i = 0; i < grid.faces.size(); i++)
			both.faces.push_back(TriMesh::Face(grid.faces[i][0] + voff,
			                                   grid.faces[i][1] + voff,
			                                   grid.faces[i][2] + voff));
		const size_t nf1 = tet.faces.size();
		const size_t nf2 = grid.faces.size();
		std::vector<vec> be = reference_basis(both);
		std::vector<vec> be1(be.begin(), be.begin() + nf1);
		std::vector<vec> be2(be.begin() + nf1, be.end());
		std::vector<int> cb;
		cb.push_back(1);                    // on the tetrahedron
		cb.push_back((int) nf1 + 7);        // on the grid
		std::vector<cd> ub(nf1 + nf2), hb(2);
		for (size_t i = 0; i < ub.size(); i++)
			ub[i] = cd(U(rng), U(rng));
		hb[0] = cd(U(rng), U(rng));
		hb[1] = cd(U(rng), U(rng));
		FaceFieldSystem sb(both, be, 2, 0.8, cb);
		FaceFieldSystem::Result rb = sb.solve(ub, hb);
		TASK_CHECK(rb.residual <= 1e-9);
		std::vector<cd> u1(ub.begin(), ub.begin() + nf1);
		std::vector<cd> u2(ub.begin() + nf1, ub.end());
		std::vector<int> c1(1, 1), c2(1, 7);
		FaceFieldSystem s1(tet, be1, 2, 0.8, c1);
		FaceFieldSystem s2(grid, be2, 2, 0.8, c2);
		FaceFieldSystem::Result r1 = s1.solve(u1, std::vector<cd>(1, hb[0]));
		FaceFieldSystem::Result r2 = s2.solve(u2, std::vector<cd>(1, hb[1]));
		double err = 0.0;
		for (size_t f = 0; f < nf1; f++)
			err = std::max(err, std::abs(rb.z[f] - r1.z[f]));
		for (size_t f = 0; f < nf2; f++)
			err = std::max(err, std::abs(rb.z[nf1 + f] - r2.z[f]));
		TASK_CHECK(err < 1e-7);
	}

	// Empty mesh is legal.
	{
		TriMesh e;
		FaceFieldSystem s5(e, std::vector<vec>(), 1, 1.0, std::vector<int>());
		FaceFieldSystem::Result r = s5.solve(std::vector<cd>(), std::vector<cd>());
		TASK_CHECK(r.z.empty() && r.residual == 0.0);
		TASK_CHECK(s5.face_count() == 0 && s5.operator_nonzeros() == 0);
	}
	return 0;
}

// Rigid motions leave local coefficients unchanged; scaling the mesh by s
// together with tau by s^2 leaves them unchanged as well.
int test_rigid_scale_invariance()
{
	TriMesh m = make_exact_sheet();
	const size_t nf = m.faces.size();
	std::vector<vec> e1 = reference_basis(m);
	const int n = 3;
	const double tau = 1.3;
	std::vector<int> cons;
	cons.push_back(2);
	cons.push_back(30);
	std::mt19937 rng(41);
	std::uniform_real_distribution<double> U(-1.0, 1.0);
	std::vector<cd> u(nf), h(2);
	for (size_t f = 0; f < nf; f++)
		u[f] = cd(U(rng), U(rng));
	h[0] = cd(U(rng), U(rng));
	h[1] = cd(U(rng), U(rng));

	FaceFieldSystem sys(m, e1, n, tau, cons);
	FaceFieldSystem::Result r0 = sys.solve(u, h);
	TASK_CHECK(r0.residual <= 1e-9);

	// Exact 90-degree rotation about z plus integer translation.
	{
		TriMesh mr = m;
		for (size_t i = 0; i < mr.vertices.size(); i++) {
			const point &p = m.vertices[i];
			mr.vertices[i] = point(-p[1] + 2.0f, p[0] - 1.0f, p[2] + 3.0f);
		}
		std::vector<vec> er(nf);
		for (size_t f = 0; f < nf; f++)
			er[f] = vec(-e1[f][1], e1[f][0], e1[f][2]);
		FaceFieldSystem sr(mr, er, n, tau, cons);
		FaceFieldSystem::Result rr = sr.solve(u, h);
		TASK_CHECK(rr.residual <= 1e-9);
		TASK_CHECK(maxdiff(rr.z, r0.z) < 1e-9);
	}

	// Generic rigid motion (inexact rotation, loose tolerance).
	{
		const dvec axis = normalized(dvec(1.0, 2.0, 3.0));
		const double ca = std::cos(0.7), sa = std::sin(0.7);
		TriMesh mg = m;
		for (size_t i = 0; i < mg.vertices.size(); i++) {
			const dvec v(m.vertices[i]);
			const dvec rv = v * ca + (axis CROSS v) * sa +
			                axis * ((axis DOT v) * (1.0 - ca));
			mg.vertices[i] = point((float) (rv[0] + 0.31),
			                       (float) (rv[1] - 1.7),
			                       (float) (rv[2] + 0.83));
		}
		std::vector<vec> eg(nf);
		for (size_t f = 0; f < nf; f++) {
			const dvec v(e1[f]);
			const dvec rv = v * ca + (axis CROSS v) * sa +
			                axis * ((axis DOT v) * (1.0 - ca));
			eg[f] = vec((float) rv[0], (float) rv[1], (float) rv[2]);
		}
		FaceFieldSystem sg(mg, eg, n, tau, cons);
		FaceFieldSystem::Result rg = sg.solve(u, h);
		TASK_CHECK(rg.residual <= 1e-9);
		TASK_CHECK(maxdiff(rg.z, r0.z) < 2e-5);
	}

	// Exact scaling by 2 with tau * 4.
	{
		TriMesh ms = m;
		for (size_t i = 0; i < ms.vertices.size(); i++)
			ms.vertices[i] = m.vertices[i] * 2.0f;
		FaceFieldSystem ss(ms, e1, n, tau * 4.0, cons);
		FaceFieldSystem::Result rs = ss.solve(u, h);
		TASK_CHECK(rs.residual <= 1e-9);
		TASK_CHECK(maxdiff(rs.z, r0.z) < 1e-9);
	}

	// Inexact scaling by 1.7 with tau * 1.7^2.
	{
		TriMesh ms = m;
		for (size_t i = 0; i < ms.vertices.size(); i++)
			ms.vertices[i] = m.vertices[i] * 1.7f;
		FaceFieldSystem ss(ms, e1, n, tau * 1.7 * 1.7, cons);
		FaceFieldSystem::Result rs = ss.solve(u, h);
		TASK_CHECK(rs.residual <= 1e-9);
		TASK_CHECK(maxdiff(rs.z, r0.z) < 2e-5);
	}
	return 0;
}

// ------------------------------------------------------------------
// reuse group
// ------------------------------------------------------------------

// One system, many solves: results are deterministic, independent of the
// original mesh after construction, and the mesh is never modified.
int test_system_reuse()
{
	TriMesh m = make_grid(5, true);
	const size_t nf = m.faces.size();
	TriMesh snapshot = m;
	std::vector<vec> e1 = reference_basis(m);
	const int n = 2;
	const double tau = 0.9;
	std::vector<int> cons;
	cons.push_back(1);
	cons.push_back(40);
	std::mt19937 rng(53);
	std::uniform_real_distribution<double> U(-1.0, 1.0);
	std::vector<cd> u1(nf), h1(2);
	for (size_t f = 0; f < nf; f++)
		u1[f] = cd(U(rng), U(rng));
	h1[0] = cd(U(rng), U(rng));
	h1[1] = cd(U(rng), U(rng));

	FaceFieldSystem sys(m, e1, n, tau, cons);
	FaceFieldSystem::Result r1 = sys.solve(u1, h1);
	TASK_CHECK(r1.residual <= 1e-9);

	// Identical repeat solve.
	FaceFieldSystem::Result r1b = sys.solve(u1, h1);
	TASK_CHECK(r1b.z == r1.z && r1b.residual == r1.residual);

	// Construction and solves did not modify the mesh or its caches.
	TASK_CHECK(m.vertices == snapshot.vertices && m.faces == snapshot.faces);
	TASK_CHECK(m.normals.empty() && m.neighbors.empty() &&
	           m.across_edge.empty() && m.adjacentfaces.empty());

	// Destroy the mesh: the system is self-contained.
	m.clear();
	FaceFieldSystem::Result r2 = sys.solve(u1, h1);
	TASK_CHECK(r2.z == r1.z && r2.residual == r1.residual);

	// New right-hand side on the same system, checked densely.
	std::vector<cd> u2(nf), h2(2);
	for (size_t f = 0; f < nf; f++)
		u2[f] = cd(U(rng), U(rng));
	h2[0] = cd(U(rng), U(rng));
	h2[1] = cd(U(rng), U(rng));
	FaceFieldSystem::Result r3 = sys.solve(u2, h2);
	TASK_CHECK(r3.residual <= 1e-9);
	TriMesh m2 = make_grid(5, true);
	RefProblem R = build_ref(m2, e1, n);
	std::vector<cd> zref = ref_solve(R, tau, cons, u2, h2);
	TASK_CHECK(maxdiff(r3.z, zref) < 1e-6);

	// A second system built from identical geometry is identical.
	FaceFieldSystem sys2(m2, e1, n, tau, cons);
	FaceFieldSystem::Result r4 = sys2.solve(u1, h1);
	TASK_CHECK(r4.z == r1.z && r4.residual == r1.residual);
	return 0;
}

// Storage grows linearly with the face count (no O(F^2) matrices).
int test_storage_linear()
{
	const int qs[3] = {16, 32, 64};
	size_t prev_nnz = 0, prev_F = 0;
	for (int t = 0; t < 3; t++) {
		TriMesh g = make_grid(qs[t], true);
		std::vector<vec> e1 = reference_basis(g);
		std::vector<int> cons;
		cons.push_back(0);
		cons.push_back(qs[t]);
		cons.push_back(2 * qs[t]);
		FaceFieldSystem sys(g, e1, 4, 1.0, cons);
		const size_t F = sys.face_count();
		const size_t nnz = sys.operator_nonzeros();
		const size_t conn = sys.connection_count();
		TASK_CHECK(F == (size_t) (2 * qs[t] * qs[t]));
		TASK_CHECK(sys.free_count() + 3 == F);
		TASK_CHECK(nnz >= F);                 // at least the diagonal
		TASK_CHECK(nnz <= 8 * F);             // bounded-degree sparsity
		TASK_CHECK(conn <= 2 * F);            // ~1.5 F interior edges
		if (prev_nnz) {
			const double fratio = (double) F / (double) prev_F;
			const double zratio = (double) nnz / (double) prev_nnz;
			TASK_CHECK(zratio > 0.5 * fratio);
			TASK_CHECK(zratio < 1.3 * fratio);  // quadratic would be ~fratio^2
		}
		prev_nnz = nnz;
		prev_F = F;
	}
	return 0;
}

} // namespace

const struct task_case task_cases[] = {
	{ "connection", "folded_pair_unfolding", test_folded_pair_unfolding },
	{ "connection", "closed_shell_incompatible", test_closed_shell_incompatible },
	{ "connection", "random_rebasis", test_random_rebasis },
	{ "connection", "invalid_inputs", test_invalid_inputs },
	{ "diffusion", "dense_reference", test_dense_reference },
	{ "diffusion", "constraint_elimination_rhs", test_constraint_elimination_rhs },
	{ "diffusion", "rigid_scale_invariance", test_rigid_scale_invariance },
	{ "reuse", "system_reuse", test_system_reuse },
	{ "reuse", "storage_linear", test_storage_linear },
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
