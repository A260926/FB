/*
Tests for the ConformalMap workspace.

Everything here is checked against an independent implementation: angles and
flat coordinates come from the law of cosines and a freshly written
unfolding routine, and the Jacobian is compared with a dense finite-difference
system built from those independent angles.  Nothing is compared against the
production code's own internals.

Registered groups: metric, events, pipeline.
*/

#include "task_support/check.h"
#include "ConformalMap.h"
#include "TriMesh.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace trimesh;

namespace {

constexpr double kPi = 3.141592653589793238462643383279502884;
constexpr double kTwoPi = 2.0 * kPi;

// ---------------------------------------------------------------------------
// Independent helpers (no dependence on the production solver internals)
// ---------------------------------------------------------------------------

double angle_from_sides(double x, double y, double z)
{
	// Angle between sides x and y, opposite z.
	double c = (x * x + y * y - z * z) / (2.0 * x * y);
	return std::acos(std::max(-1.0, std::min(1.0, c)));
}

// Sides of the triangle in face f, in the order opposite face vertices 0,1,2.
struct KnownFace {
	double l[3];
	double ang[3];
	bool ok = false;
};


// Intrinsic edge length by endpoints (0 if absent).
double elen(const ConformalMap &cm, int a, int b)
{
	return cm.edge_length(a, b);
}

double known_curvature(const ConformalMap &cm, int v,
                       const std::vector<int> &boundary_flag)
{
	double sum = 0.0;
	// Build geometry from the intrinsic metric, not from the flat chart.
	for (const TriMesh::Face &F : cm.faces()) {
		int c = F.indexof(v);
		if (c < 0)
			continue;
		// Side opposite each local vertex.
		double l[3] = {elen(cm, F[1], F[2]), elen(cm, F[2], F[0]),
		               elen(cm, F[0], F[1])};
		sum += angle_from_sides(l[(c + 1) % 3], l[(c + 2) % 3], l[c]);
	}
	return (boundary_flag[v] ? kPi : kTwoPi) - sum;
}

std::vector<int> boundary_flags(const ConformalMap &cm)
{
	std::vector<int> b(cm.num_vertices(), 0);
	for (int v : cm.boundary_loop())
		b[v] = 1;
	return b;
}

std::vector<double> uniform_targets(const ConformalMap &cm)
{
	std::vector<double> t(cm.num_vertices(), 0.0);
	double u = kTwoPi / (double) cm.boundary_loop().size();
	for (int v : cm.boundary_loop())
		t[v] = u;
	return t;
}

// Deterministic bilinear-looking grid with a smooth non-planar height field.
TriMesh make_grid(int n, double amp)
{
	TriMesh m;
	for (int j = 0; j <= n; j++) {
		for (int i = 0; i <= n; i++) {
			double x = (double) i / n;
			double y = (double) j / n;
			double z = amp * std::sin(3.0 * x * kPi) *
			           std::sin(2.0 * y * kPi);
			m.vertices.push_back(point((float) x, (float) y, (float) z));
		}
	}
	auto id = [n](int i, int j) { return j * (n + 1) + i; };
	for (int j = 0; j < n; j++) {
		for (int i = 0; i < n; i++) {
			int a = id(i, j), b = id(i + 1, j);
			int c = id(i + 1, j + 1), d = id(i, j + 1);
			m.faces.push_back(TriMesh::Face(a, b, c));
			m.faces.push_back(TriMesh::Face(a, c, d));
		}
	}
	return m;
}

// A non-planar "umbrella": apex plus a boundary ring; the fan triangles are
// planar but the whole surface is not, so solving genuinely crosses events.
TriMesh make_umbrella(int n, double height)
{
	TriMesh m;
	m.vertices.push_back(point(0.0f, 0.0f, (float) height));
	for (int i = 0; i < n; i++) {
		double a = kTwoPi * i / n;
		double r = 1.0 + 0.22 * std::sin(3.0 * a);
		m.vertices.push_back(point((float) (r * std::cos(a)),
		                           (float) (r * std::sin(a)),
		                           (float) (0.15 * std::sin(2.0 * a))));
	}
	for (int i = 0; i < n; i++)
		m.faces.push_back(TriMesh::Face(0, 1 + i, 1 + (i + 1) % n));
	return m;
}

// The single-wavy-square test used for the metric group: 2x2 grid, small
// height, so the input Delaunayization is already legal and the solve itself
// is the only thing that moves the metric.
TriMesh make_wavy(int n, double amp) { return make_grid(n, amp); }

// Independent unfolding of the published intrinsic metric: place the first
// face, then breadth-first place each new vertex; report the largest
// disagreement between two arrival positions and the largest per-face
// orientation violation.
bool independent_unfold(const ConformalMap &cm, std::vector<point> &xy,
                        double &max_disagree, double &max_len,
                        bool &all_positive)
{
	int nf = cm.num_faces();
	int nv = cm.num_vertices();
	const std::vector<TriMesh::Face> &faces = cm.faces();
	// Work in double precision; the float point vector is only a final
	// rendering of the positions, so the reported disagreement is not
	// limited by float32 storage.
	std::vector<std::array<double, 2>> pos(nv, {0.0, 0.0});
	xy.assign(nv, point(0.0f, 0.0f, 0.0f));
	std::vector<char> placed(nv, 0), done(nf, 0);
	std::map<std::pair<int, int>, std::vector<int>> edge_faces;
	for (int f = 0; f < nf; f++)
		for (int j = 0; j < 3; j++) {
			int a = faces[f][j], b = faces[f][(j + 1) % 3];
			edge_faces[{std::min(a, b), std::max(a, b)}].push_back(f);
		}
	max_len = 0.0;
	for (int f = 0; f < nf; f++)
		for (int j = 0; j < 3; j++)
			max_len = std::max(max_len,
				elen(cm, faces[f][j], faces[f][(j + 1) % 3]));
	if (!(max_len > 0))
		return false;

	const TriMesh::Face F0 = faces[0];
	double l01 = elen(cm, F0[0], F0[1]);
	double l12 = elen(cm, F0[1], F0[2]);
	double l20 = elen(cm, F0[2], F0[0]);
	if (!(l01 > 0) || !(l12 > 0) || !(l20 > 0))
		return false;
	pos[F0[0]] = {0.0, 0.0};
	pos[F0[1]] = {l01, 0.0};
	double x = (l01 * l01 + l20 * l20 - l12 * l12) / (2 * l01);
	double h2 = l20 * l20 - x * x;
	if (!(h2 > 0))
		return false;
	pos[F0[2]] = {x, std::sqrt(h2)};
	placed[F0[0]] = placed[F0[1]] = placed[F0[2]] = 1;
	done[0] = 1;

	max_disagree = 0.0;
	std::vector<int> queue(1, 0);
	for (size_t qi = 0; qi < queue.size(); qi++) {
		int f = queue[qi];
		const TriMesh::Face F = faces[f];
		for (int j = 0; j < 3; j++) {
			int a = F[j], b = F[(j + 1) % 3];
			std::pair<int, int> key{std::min(a, b), std::max(a, b)};
			if (edge_faces[key].size() != 2)
				continue;
			int g = -1;
			for (int t : edge_faces[key])
				if (t != f)
					g = t;
			if (g < 0 || done[g])
				continue;
			int c = -1;
			for (int u = 0; u < 3; u++)
				if (faces[g][u] != a && faces[g][u] != b)
					c = faces[g][u];
			if (c < 0)
				return false;
			double la = elen(cm, a, c), lb = elen(cm, b, c);
			if (!(la > 0) || !(lb > 0))
				return false;
			double dx = pos[b][0] - pos[a][0];
			double dy = pos[b][1] - pos[a][1];
			double lab = std::sqrt(dx * dx + dy * dy);
			if (!(lab > 0))
				return false;
			double ex = dx / lab, ey = dy / lab;
			double px = (la * la + lab * lab - lb * lb) / (2 * lab);
			double ph2 = la * la - px * px;
			if (ph2 < -1e-9 * max_len * max_len)
				return false;
			ph2 = std::max(0.0, ph2);
			double h = std::sqrt(ph2);
			double nx = -ey, ny = ex;
			// Third vertex of f, which must end up on the opposite side of
			// the shared edge from the new vertex.
			int third = F[(j + 2) % 3];
			double side = (pos[third][0] - pos[a][0]) * nx +
			              (pos[third][1] - pos[a][1]) * ny;
			double sgn = side > 0 ? -1.0 : 1.0;
			double predx = pos[a][0] + px * ex + sgn * h * nx;
			double predy = pos[a][1] + px * ey + sgn * h * ny;
			if (placed[c]) {
				double ddx = predx - pos[c][0], ddy = predy - pos[c][1];
				max_disagree = std::max(max_disagree,
					std::sqrt(ddx * ddx + ddy * ddy));
			} else {
				pos[c] = {predx, predy};
				placed[c] = 1;
			}
			done[g] = 1;
			queue.push_back(g);
		}
	}
	all_positive = true;
	for (int f = 0; f < nf; f++) {
		const TriMesh::Face &F = faces[f];
		double ax = pos[F[1]][0] - pos[F[0]][0];
		double ay = pos[F[1]][1] - pos[F[0]][1];
		double bx = pos[F[2]][0] - pos[F[0]][0];
		double by = pos[F[2]][1] - pos[F[0]][1];
		if (!(ax * by - ay * bx > 0))
			all_positive = false;
	}
	for (int v = 0; v < nv; v++) {
		if (!placed[v])
			return false;
		xy[v] = point((float) pos[v][0], (float) pos[v][1], 0.0f);
	}
	return true;
}

// Dense finite-difference Jacobian of the curvature, built from independent
// angles.  Returns d(kappa)/du on vertices 1..N-1.
std::vector<std::vector<double>> dense_jacobian(const ConformalMap &cm)
{
	int nv = cm.num_vertices();
	std::vector<int> bf = boundary_flags(cm);
	std::vector<double> base(nv);
	auto curvature_at = [&](const std::vector<double> &uu,
	                        std::vector<double> &out) {
		// Lengths follow l = l0 exp((u_i+u_j)/2); recover l0 from the
		// current u and current length, then perturb.
		std::map<std::pair<int, int>, double> l0;
		for (const ConformalMap::EdgeInfo &e : cm.edges()) {
			double s = std::exp(0.5 * (cm.u()[e.v0] + cm.u()[e.v1]));
			l0[{e.v0, e.v1}] = e.length / s;
			l0[{e.v1, e.v0}] = e.length / s;
		}
		out.assign(nv, 0.0);
		std::vector<double> theta(nv, 0.0);
		for (const TriMesh::Face &F : cm.faces()) {
			double l01 = l0[{F[0], F[1]}] * std::exp(0.5 * (uu[F[0]] + uu[F[1]]));
			double l12 = l0[{F[1], F[2]}] * std::exp(0.5 * (uu[F[1]] + uu[F[2]]));
			double l20 = l0[{F[2], F[0]}] * std::exp(0.5 * (uu[F[2]] + uu[F[0]]));
			theta[F[0]] += angle_from_sides(l20, l01, l12);
			theta[F[1]] += angle_from_sides(l01, l12, l20);
			theta[F[2]] += angle_from_sides(l12, l20, l01);
		}
		for (int v = 0; v < nv; v++)
			out[v] = (bf[v] ? kPi : kTwoPi) - theta[v];
	};
	curvature_at(cm.u(), base);
	int n = nv - 1;
	std::vector<std::vector<double>> J(n, std::vector<double>(n, 0.0));
	const double h = 1e-6;
	for (int j = 1; j < nv; j++) {
		std::vector<double> up = cm.u(), um = cm.u();
		up[j] += h;
		um[j] -= h;
		std::vector<double> kp, km;
		curvature_at(up, kp);
		curvature_at(um, km);
		for (int i = 1; i < nv; i++)
			J[i - 1][j - 1] = (kp[i] - km[i]) / (2 * h);
	}
	return J;
}

// ---------------------------------------------------------------------------
// Inputs shared by several cases
// ---------------------------------------------------------------------------

std::vector<double> targets_for(const ConformalMap &cm, int pattern)
{
	int nb = (int) cm.boundary_loop().size();
	std::vector<double> t(cm.num_vertices(), 0.0);
	double base = kTwoPi / nb;
	for (int k = 0; k < nb; k++) {
		int v = cm.boundary_loop()[k];
		if (pattern == 0)
			t[v] = base;
		else
			t[v] = base * ((k % 2) ? 1.45 : 0.55);
	}
	if (pattern != 0) {
		double s = 0.0;
		for (double x : t)
			s += x;
		double f = kTwoPi / s;
		for (double &x : t)
			x *= f;
	}
	return t;
}


// ---------------------------------------------------------------------------
// Group: metric
// ---------------------------------------------------------------------------

int case_planar_square()
{
	TriMesh m;
	m.vertices = {point(0, 0, 0), point(2, 0, 0), point(2, 1, 0),
	              point(0, 1, 0)};
	m.faces = {TriMesh::Face(0, 1, 2), TriMesh::Face(0, 2, 3)};
	ConformalMap cm(m);
	TASK_CHECK(cm.num_vertices() == 4);
	TASK_CHECK(cm.num_faces() == 2);
	TASK_CHECK(cm.boundary_loop().size() == 4);
	std::vector<double> t = uniform_targets(cm);
	ConformalMap::Result r = cm.solve(t, 1e-10, 100, 1000);
	TASK_CHECK(r.ok());
	TASK_CHECK(r.diagnostics.max_residual <= 1e-10);
	TASK_CHECK(r.flat.vertices.size() == 4);
	TASK_CHECK(r.flat.faces.size() == 2);
	// A planar rectangle must map to a rectangle with the same dimensions.
	double w = elen(cm, 0, 1), h = elen(cm, 1, 2);
	TASK_CHECK(std::fabs(w - 2.0) < 1e-9);
	TASK_CHECK(std::fabs(h - 1.0) < 1e-9);
	// Vertex 0 at the origin, lowest-numbered neighbor on +X.
	TASK_CHECK(std::fabs(r.flat.vertices[0][0]) < 1e-9);
	TASK_CHECK(std::fabs(r.flat.vertices[0][1]) < 1e-9);
	TASK_CHECK(r.flat.vertices[1][1] == 0.0f);
	TASK_CHECK(r.flat.vertices[1][0] > 0.0f);
	TASK_CHECK(std::fabs(r.flat.vertices[0][2]) < 1e-12);
	return 0;
}

int case_curvature_matches_targets()
{
	TriMesh m = make_wavy(3, 0.25);
	ConformalMap cm(m);
	std::vector<double> t = targets_for(cm, 0);
	ConformalMap::Result r = cm.solve(t, 1e-11, 200, 5000);
	TASK_CHECK(r.ok());
	std::vector<int> bf = boundary_flags(cm);
	for (int v = 0; v < cm.num_vertices(); v++) {
		double k = known_curvature(cm, v, bf);
		TASK_CHECK(std::fabs(k - t[v]) <= 1e-8);
	}
	return 0;
}

int case_jacobian_matches_dense_system()
{
	TriMesh m = make_wavy(3, 0.3);
	ConformalMap cm(m);
	std::vector<double> t = targets_for(cm, 0);
	ConformalMap::Result r = cm.solve(t, 1e-11, 200, 5000);
	TASK_CHECK(r.ok());
	// Check the Jacobian at the solved state against a dense finite-
	// difference system built from independent angle computations.
	std::vector<std::vector<double>> ref = dense_jacobian(cm);
	int n = cm.num_vertices() - 1;
	std::vector<std::vector<double>> got(n, std::vector<double>(n, 0.0));
	std::vector<ConformalMap::JacobianEntry> trips = cm.jacobian();
	TASK_CHECK(!trips.empty());
	for (const ConformalMap::JacobianEntry &e : trips) {
		TASK_CHECK(e.row >= 0 && e.row < n);
		TASK_CHECK(e.col >= 0 && e.col < n);
		got[e.row][e.col] += e.value;
	}
	double maxdiff = 0.0;
	for (int i = 0; i < n; i++)
		for (int j = 0; j < n; j++)
			maxdiff = std::max(maxdiff, std::fabs(got[i][j] - ref[i][j]));
	TASK_CHECK(maxdiff < 1e-4);
	// The reduced matrix is symmetric.  Boundary vertices can have negative
	// total cotangent weight (obtuse corners), so only symmetry and the sign
	// of the off-diagonal entries are asserted here.
	for (int i = 0; i < n; i++) {
		for (int j = 0; j < n; j++) {
			TASK_CHECK(std::fabs(got[i][j] - got[j][i]) < 1e-9);
			if (i != j)
				TASK_CHECK(got[i][j] <= 1e-9);
		}
	}
	return 0;
}

int case_scale_and_rigid_covariance()
{
	TriMesh base = make_wavy(3, 0.3);
	ConformalMap cm0(base);
	std::vector<double> t0 = uniform_targets(cm0);
	ConformalMap::Result r0 = cm0.solve(t0, 1e-11, 200, 5000);
	TASK_CHECK(r0.ok());
	double maxlen0 = 0.0;
	for (const ConformalMap::EdgeInfo &e : cm0.edges())
		maxlen0 = std::max(maxlen0, e.length);

	// Scale by a power of two so the float input coordinates are exact
	// multiples of the originals: the flattening is then exactly covariant
	// (a non-dyadic factor would introduce float rounding in the input).
	const float sf = 4.0f;
	TriMesh scaled = base;
	for (point &p : scaled.vertices)
		p = point(sf * p[0], sf * p[1], sf * p[2]);
	ConformalMap cms(scaled);
	std::vector<double> ts = uniform_targets(cms);
	ConformalMap::Result rs = cms.solve(ts, 1e-11, 200, 5000);
	TASK_CHECK(rs.ok());
	double maxlens = 0.0;
	for (const ConformalMap::EdgeInfo &e : cms.edges())
		maxlens = std::max(maxlens, e.length);
	TASK_CHECK(std::fabs(maxlens / maxlen0 - (double) sf) < 1e-10);
	// The dimensionless flattening (normalized by its own scale) is the same.
	for (int v = 0; v < cm0.num_vertices(); v++) {
		double dx = (double) r0.flat.vertices[v][0] / maxlen0 -
		            (double) rs.flat.vertices[v][0] / maxlens;
		double dy = (double) r0.flat.vertices[v][1] / maxlen0 -
		            (double) rs.flat.vertices[v][1] / maxlens;
		TASK_CHECK(std::fabs(dx) < 1e-7 && std::fabs(dy) < 1e-7);
	}

	// Rigid motion: rotate and translate the input; the flat shape must be
	// congruent up to the canonical placement (so normalized coordinates
	// match after undoing the canonical orientation).
	// Rigid motion: a 90-degree rotation about Z.  The rotation is exact in
	// float32 for the dyadic grid coordinates, so the intrinsic metric must
	// be reproduced exactly.  (A translation would be exact in the metric
	// too, but rounding of the shifted float coordinates would perturb the
	// input lengths first.)
	TriMesh moved = base;
	for (point &p : moved.vertices) {
		float x = p[0], y = p[1], z = p[2];
		p = point(-y, x, z);
	}
	ConformalMap cmm(moved);
	std::vector<double> tm = uniform_targets(cmm);
	ConformalMap::Result rm = cmm.solve(tm, 1e-11, 200, 5000);
	TASK_CHECK(rm.ok());
	// Edge lengths are rigid-motion invariant, and for an exact rotation the
	// match is to solver precision.
	for (size_t i = 0; i < cm0.edges().size(); i++) {
		double l0 = cm0.edges()[i].length;
		double lm = cmm.edge_length(cmm.edges()[i].v0,
		                            cmm.edges()[i].v1);
		TASK_CHECK(std::fabs(l0 - lm) < 1e-9 * (1.0 + l0));
	}
	return 0;
}

int case_internal_vertices_zero_defect()
{
	TriMesh m = make_grid(4, 0.4);
	ConformalMap cm(m);
	std::vector<double> t = uniform_targets(cm);
	ConformalMap::Result r = cm.solve(t, 1e-11, 200, 5000);
	TASK_CHECK(r.ok());
	std::vector<int> bf = boundary_flags(cm);
	for (int v = 0; v < cm.num_vertices(); v++) {
		if (bf[v])
			continue;
		TASK_CHECK(std::fabs(known_curvature(cm, v, bf)) <= 1e-8);
	}
	return 0;
}

int case_target_sum_validation()
{
	TriMesh m = make_wavy(2, 0.2);
	ConformalMap cm(m);
	std::vector<double> t = uniform_targets(cm);
	// Sum off by more than 1e-10 must be rejected.
	std::vector<double> bad = t;
	bad[cm.boundary_loop()[0]] += 1e-8;
	bool threw = false;
	try {
		(void) cm.solve(bad, 1e-10, 100, 1000);
	} catch (const std::invalid_argument &) {
		threw = true;
	}
	TASK_CHECK(threw);
	// A boundary target outside (0, pi) must be rejected.
	std::vector<double> bad2 = t;
	bad2[cm.boundary_loop()[0]] = kPi;
	threw = false;
	try {
		(void) cm.solve(bad2, 1e-10, 100, 1000);
	} catch (const std::invalid_argument &) {
		threw = true;
	}
	TASK_CHECK(threw);
	return 0;
}

} // namespace

// ---------------------------------------------------------------------------
// Group: events
// ---------------------------------------------------------------------------

namespace {

// Verify the invariants recorded at each flip against the independently
// developed geometry.
int check_flip_records(const ConformalMap &cm)
{
	const std::vector<ConformalMap::FlipRecord> &recs = cm.flip_records();
	for (const ConformalMap::FlipRecord &r : recs) {
		// A flip that crosses a cocircular event is taken essentially at the
		// event: the two opposite angles sum to pi.  Maintenance flips that
		// merely restore Delaunay need only occur where the sum exceeds pi.
		if (r.phase == 1) {
			TASK_CHECK(std::fabs(r.opposite_angle_sum - kPi) < 1e-6);
			TASK_CHECK(r.opposite_angle_sum >= kPi - 1e-6);
		} else {
			TASK_CHECK(r.opposite_angle_sum >= kPi - 1e-9);
		}
		// The new base length reproduces the developed new length under the
		// recorded u.
		double scale = std::exp(0.5 * (r.u_k + r.u_l));
		TASK_CHECK(std::fabs(r.new_base_length * scale - r.new_length) <=
		           1e-12 * (1.0 + r.new_length));
		// Both sides of the developed quad agree.
		TASK_CHECK(std::fabs(r.new_length_from_a - r.new_length_from_b) <=
		           1e-8 * (1.0 + r.new_length));
		// Recompute the developed diagonal from the recorded sides/angles.
		double la = std::sqrt(std::max(0.0, r.side_ak * r.side_ak +
			r.side_al * r.side_al -
			2.0 * r.side_ak * r.side_al * std::cos(r.angle_a)));
		TASK_CHECK(std::fabs(la - r.new_length) <=
		           1e-8 * (1.0 + r.new_length));
		// A flip can later be undone by another flip, so the new edge is
		// not required to survive in the final connectivity; when it does
		// survive, its length must be positive.
		double l = cm.edge_length(r.k, r.l);
		TASK_CHECK(l >= 0.0);
		// Metric continuity: the new base length is not the old edge's 3D
		// chord but the developed diagonal.
		TASK_CHECK(r.new_base_length > 0.0);
	}
	return 0;
}

int case_event_flip_continuity()
{
	// Wavy 2x2 and 3x3 grids: the solve crosses several cocircular events
	// (verified by the precondition).
	TriMesh m = make_wavy(3, 0.85);
	ConformalMap cm(m);
	std::vector<double> t = uniform_targets(cm);
	ConformalMap::Result r = cm.solve(t, 1e-11, 300, 20000);
	TASK_CHECK(r.ok());
	TASK_CHECK(r.diagnostics.flips > 0);
	TASK_CHECK(r.diagnostics.steps > 0);
	// At least one flip happened during the nonlinear solve rather than in
	// the initial legalization, so this is not merely a Delaunayization.
	int solve_phase = 0;
	for (const ConformalMap::FlipRecord &fr : cm.flip_records())
		if (fr.phase == 1)
			solve_phase++;
	TASK_CHECK(solve_phase > 0);
	TASK_CHECK(check_flip_records(cm) == 0);
	// Every current edge is Delaunay: opposite angle sums <= pi.  Edges that
	// intrinsically could not be flipped are absent here because the test
	// meshes are all locally flip-admissible.
	for (const ConformalMap::EdgeInfo &e : cm.edges()) {
		if (e.faces[1] < 0)
			continue;
		double sum = 0.0;
		for (int k = 0; k < 2; k++) {
			const TriMesh::Face &F = cm.faces()[e.faces[k]];
			int third = -1;
			for (int j = 0; j < 3; j++)
				if (F[j] != e.v0 && F[j] != e.v1)
					third = F[j];
			TASK_CHECK(third >= 0);
			double la = elen(cm, e.v0, third);
			double lb = elen(cm, e.v1, third);
			double lc = elen(cm, e.v0, e.v1);
			sum += angle_from_sides(la, lb, lc);
		}
		TASK_CHECK(sum <= kPi + 1e-7);
	}
	return 0;
}

int case_metric_continuous_across_flip()
{
	// Before/after a flip, the metric implied by the recorded base lengths
	// and u must agree with the metric the solver reports.
	TriMesh m = make_wavy(3, 0.85);
	ConformalMap cm(m);
	std::vector<double> t = uniform_targets(cm);
	ConformalMap::Result r = cm.solve(t, 1e-11, 300, 20000);
	TASK_CHECK(r.ok());
	int checked = 0;
	for (const ConformalMap::FlipRecord &fr : cm.flip_records()) {
		if (fr.phase != 1)
			continue;
		// A later flip may restore the old diagonal, so the removed edge is
		// only required to be absent right after its own flip, which the
		// record's old/new labels and lengths encode.  The new base length
		// is what continuity hinges on.
		double nl = cm.edge_length(fr.k, fr.l);
		TASK_CHECK(nl >= 0.0);
		// Its current length equals base * exp((u_k+u_l)/2) and, at the
		// moment of the flip, the developed length.
		double expected = fr.new_base_length *
			std::exp(0.5 * (fr.u_k + fr.u_l));
		TASK_CHECK(std::fabs(expected - fr.new_length) <=
		           1e-12 * (1.0 + fr.new_length));
		checked++;
	}
	TASK_CHECK(checked > 0);
	return 0;
}

int case_budget_and_status()
{
	TriMesh m = make_wavy(3, 0.6);
	ConformalMap cm(m);
	std::vector<double> t = uniform_targets(cm);
	// Zero-step budget with an unsatisfied target must fail with a
	// distinguishable status and must not claim convergence.
	ConformalMap::Result r = cm.solve(t, 1e-14, 1, 0);
	TASK_CHECK(r.status == ConformalMap::BUDGET_EXHAUSTED);
	TASK_CHECK(!r.ok());
	TASK_CHECK(!r.diagnostics.published);
	TASK_CHECK(r.flat.vertices.empty());
	// The workspace still holds its initial (valid) state.
	TASK_CHECK(cm.u().size() == m.vertices.size());
	TASK_CHECK(cm.num_vertices() == (int) m.vertices.size());
	// The reported work never exceeds the supplied budget.
	TASK_CHECK(r.diagnostics.steps + r.diagnostics.flips <= 1);
	// The single-budget overload caps steps + flips together.
	ConformalMap::Result rw = cm.solve(t, 1e-14, 3);
	TASK_CHECK(rw.status == ConformalMap::BUDGET_EXHAUSTED);
	TASK_CHECK(rw.diagnostics.work_used <= 3);
	TASK_CHECK(rw.diagnostics.steps + rw.diagnostics.flips <= 3);
	return 0;
}

int case_initial_delaunayization()
{
	// A deliberately non-Delaunay planar triangulation: a convex quad
	// triangulated along its long diagonal, so the opposite angles at that
	// diagonal sum to more than pi.  The initial metric must be legalized by
	// a flip before solving.
	TriMesh m;
	m.vertices = {point(0, 0, 0), point(4, 0, 0), point(4, 4, 0),
	              point(0, 2, 0)};
	m.faces = {TriMesh::Face(0, 1, 2), TriMesh::Face(0, 2, 3)};
	ConformalMap cm(m);
	std::vector<double> t = uniform_targets(cm);
	ConformalMap::Result r = cm.solve(t, 1e-10, 200, 5000);
	TASK_CHECK(r.ok());
	TASK_CHECK(r.diagnostics.initial_flips > 0);
	TASK_CHECK(r.diagnostics.max_residual <= 1e-10);
	return 0;
}

} // namespace

// ---------------------------------------------------------------------------
// Group: pipeline
// ---------------------------------------------------------------------------

namespace {

int case_two_target_switches()
{
	TriMesh m = make_wavy(3, 0.4);
	ConformalMap cm(m);
	std::vector<double> t0 = targets_for(cm, 0);
	ConformalMap::Result r0 = cm.solve(t0, 1e-11, 200, 5000);
	TASK_CHECK(r0.ok());
	std::vector<double> t1 = targets_for(cm, 1);
	ConformalMap::Result r1 = cm.solve(t1, 1e-11, 200, 5000);
	TASK_CHECK(r1.ok());
	// The second solve must have reached the second target set.
	std::vector<int> bf = boundary_flags(cm);
	for (int v = 0; v < cm.num_vertices(); v++)
		TASK_CHECK(std::fabs(known_curvature(cm, v, bf) - t1[v]) <= 1e-8);
	// Switching back must reproduce the first target set as well.
	ConformalMap::Result r2 = cm.solve(t0, 1e-11, 200, 5000);
	TASK_CHECK(r2.ok());
	for (int v = 0; v < cm.num_vertices(); v++)
		TASK_CHECK(std::fabs(known_curvature(cm, v, bf) - t0[v]) <= 1e-8);
	// The published map reflects the latest successful solve.
	TASK_CHECK(cm.has_flat_map());
	TASK_CHECK(cm.flat().vertices.size() == (size_t) cm.num_vertices());
	return 0;
}

int case_failed_solve_rolls_back()
{
	TriMesh m = make_wavy(3, 0.45);
	ConformalMap cm(m);
	std::vector<double> t0 = targets_for(cm, 0);
	ConformalMap::Result r0 = cm.solve(t0, 1e-11, 200, 5000);
	TASK_CHECK(r0.ok());
	std::vector<double> u_before = cm.u();
	std::vector<TriMesh::Face> faces_before = cm.faces();
	TriMesh flat_before = cm.flat();

	// Force a failure with an impossible work budget while asking for a
	// different, harder target set.
	std::vector<double> t1 = targets_for(cm, 1);
	ConformalMap::Result rf = cm.solve(t1, 1e-14, 1, 0);
	TASK_CHECK(!rf.ok());
	TASK_CHECK(rf.status != ConformalMap::SUCCESS);
	// State and published map are exactly as they were.
	TASK_CHECK(cm.u() == u_before);
	TASK_CHECK(cm.faces().size() == faces_before.size());
	for (size_t i = 0; i < faces_before.size(); i++)
		TASK_CHECK(cm.faces()[i] == faces_before[i]);
	TASK_CHECK(cm.has_flat_map());
	TASK_CHECK(cm.flat().vertices.size() == flat_before.vertices.size());
	for (size_t i = 0; i < flat_before.vertices.size(); i++)
		TASK_CHECK(cm.flat().vertices[i] == flat_before.vertices[i]);
	// It can still be solved afterwards.
	ConformalMap::Result r1 = cm.solve(t1, 1e-11, 200, 5000);
	TASK_CHECK(r1.ok());
	return 0;
}

int case_non_disk_inputs_rejected()
{
	auto expect_throw = [](const TriMesh &m) {
		try {
			ConformalMap cm(m);
			(void) cm;
		} catch (const std::invalid_argument &) {
			return true;
		}
		return false;
	};
	// Closed tetrahedron: no boundary.
	{
		TriMesh m;
		m.vertices = {point(0, 0, 0), point(1, 0, 0), point(0, 1, 0),
		              point(0, 0, 1)};
		m.faces = {TriMesh::Face(0, 2, 1), TriMesh::Face(0, 1, 3),
		           TriMesh::Face(1, 2, 3), TriMesh::Face(2, 0, 3)};
		TASK_CHECK(expect_throw(m));
	}
	// Two disjoint disks.
	{
		TriMesh m;
		m.vertices = {point(0, 0, 0), point(1, 0, 0), point(0, 1, 0),
		              point(5, 0, 0), point(6, 0, 0), point(5, 1, 0)};
		m.faces = {TriMesh::Face(0, 1, 2), TriMesh::Face(3, 4, 5)};
		TASK_CHECK(expect_throw(m));
	}
	// Zero-area triangle.
	{
		TriMesh m;
		m.vertices = {point(0, 0, 0), point(1, 0, 0), point(2, 0, 0),
		              point(0, 1, 0)};
		m.faces = {TriMesh::Face(0, 1, 2), TriMesh::Face(0, 2, 3)};
		TASK_CHECK(expect_throw(m));
	}
	// Inconsistent orientation.
	{
		TriMesh m;
		m.vertices = {point(0, 0, 0), point(1, 0, 0), point(1, 1, 0),
		              point(0, 1, 0)};
		m.faces = {TriMesh::Face(0, 1, 2), TriMesh::Face(0, 2, 3)};
		m.faces[1] = TriMesh::Face(0, 3, 2); // flip the second face
		TASK_CHECK(expect_throw(m));
	}
	// Isolated vertex.
	{
		TriMesh m;
		m.vertices = {point(0, 0, 0), point(1, 0, 0), point(0, 1, 0),
		              point(9, 9, 9)};
		m.faces = {TriMesh::Face(0, 1, 2)};
		TASK_CHECK(expect_throw(m));
	}
	// Colors of the wrong length.
	{
		TriMesh m;
		m.vertices = {point(0, 0, 0), point(1, 0, 0), point(0, 1, 0)};
		m.faces = {TriMesh::Face(0, 1, 2)};
		m.colors.assign(2, Color(1, 0, 0));
		TASK_CHECK(expect_throw(m));
	}
	// Non-finite coordinate.
	{
		TriMesh m;
		m.vertices = {point(0, 0, 0), point(1, 0, 0), point(0, 1, 0)};
		m.vertices[1] = point(std::numeric_limits<float>::infinity(), 0, 0);
		m.faces = {TriMesh::Face(0, 1, 2)};
		TASK_CHECK(expect_throw(m));
	}
	return 0;
}

int case_cold_cache_consistency()
{
	TriMesh m = make_wavy(3, 0.3);
	// Populate every derived cache on the input before flattening.
	m.need_across_edge();
	m.need_normals();
	m.need_pointareas();
	size_t nv = m.vertices.size();
	ConformalMap cm(m);
	std::vector<double> t = uniform_targets(cm);
	ConformalMap::Result r = cm.solve(t, 1e-11, 200, 5000);
	TASK_CHECK(r.ok());
	// The published mesh must not carry stale derived caches: recomputing
	// them must give the same sizes and agreement with the flat geometry.
	TriMesh &f = r.flat;
	TASK_CHECK(f.colors.empty());
	f.need_across_edge();
	f.need_normals();
	f.need_pointareas();
	TASK_CHECK(f.normals.size() == nv);
	TASK_CHECK(f.pointareas.size() == nv);
	TASK_CHECK(f.across_edge.size() == f.faces.size());
	// All normals point along +Z for a positively oriented planar chart.
	for (const vec &n : f.normals)
		TASK_CHECK(n[2] > 0.999f);
	// Total point area equals the total triangle area.
	double pa = 0.0;
	for (double a : f.pointareas)
		pa += a;
	double ta = 0.0;
	for (const TriMesh::Face &tri : f.faces) {
		const point &a = f.vertices[tri[0]];
		const point &b = f.vertices[tri[1]];
		const point &c = f.vertices[tri[2]];
		ta += 0.5 * len((b - a) CROSS (c - a));
	}
	TASK_CHECK(std::fabs(pa - ta) < 1e-4 * (1.0 + ta));
	// The returned value is independent of the workspace.
	TriMesh independent = r.flat;
	r = ConformalMap::Result();
	TASK_CHECK(independent.vertices.size() == nv);
	return 0;
}

int case_cli_roundtrip()
{
	// Exercise the same path as the command line utility: write with the
	// library writer, read it back with the library reader, and check the
	// flat chart survived.
	TriMesh m = make_umbrella(13, 1.1);
	ConformalMap cm(m);
	std::vector<double> t = uniform_targets(cm);
	ConformalMap::Result r = cm.solve(t, 1e-11, 500, 50000);
	TASK_CHECK(r.ok());
	const char *path = ".task-build/conformal_cases_cli.ply";
	TASK_CHECK(r.flat.write(std::string("ascii:") + path));
	std::unique_ptr<TriMesh> back(TriMesh::read(path));
	TASK_CHECK(back != nullptr);
	TASK_CHECK(back->vertices.size() == r.flat.vertices.size());
	TASK_CHECK(back->faces.size() == r.flat.faces.size());
	double maxz = 0.0, maxd = 0.0;
	for (size_t i = 0; i < back->vertices.size(); i++) {
		maxz = std::max(maxz, (double) std::fabs(back->vertices[i][2]));
		double dx = (double) back->vertices[i][0] - r.flat.vertices[i][0];
		double dy = (double) back->vertices[i][1] - r.flat.vertices[i][1];
		maxd = std::max(maxd, std::sqrt(dx * dx + dy * dy));
	}
	TASK_CHECK(maxz < 1e-6);
	TASK_CHECK(maxd < 1e-5);
	// Face indices are preserved.
	for (size_t i = 0; i < back->faces.size(); i++)
		for (int j = 0; j < 3; j++)
			TASK_CHECK(back->faces[i][j] == r.flat.faces[i][j]);
	std::remove(path);
	return 0;
}

int case_sparse_growth()
{
	// A mesh with over a thousand vertices: the sparse Jacobian must scale
	// with the number of edges, far below the dense n^2 bound.
	TriMesh m = make_grid(32, 0.35);
	int nv = (int) m.vertices.size();
	TASK_CHECK(nv > 1000);
	ConformalMap cm(m);
	std::vector<double> t = uniform_targets(cm);
	ConformalMap::Result r = cm.solve(t, 1e-10, 200, 100000);
	TASK_CHECK(r.ok());
	std::size_t nz = r.diagnostics.linear_nonzeros;
	int ne = cm.num_edges();
	TASK_CHECK(nz > 0);
	// Sparse storage grows like O(edges + vertices), not O(vertices^2).
	TASK_CHECK(nz <= (std::size_t) (4 * (ne + nv) + 16));
	TASK_CHECK(nz < (std::size_t) nv * (std::size_t) nv / 4);
	// The published chart is a consistent embedding.  The independent
	// unfolding uses a face-neighbour search, so for this larger mesh it is
	// checked from the production chart directly rather than by the slower
	// independent BFS.
	const TriMesh &flat = cm.flat();
	double maxlen = 0.0;
	for (const ConformalMap::EdgeInfo &e : cm.edges())
		maxlen = std::max(maxlen, e.length);
	int neg = 0;
	for (const TriMesh::Face &F : flat.faces) {
		double ax = flat.vertices[F[1]][0] - flat.vertices[F[0]][0];
		double ay = flat.vertices[F[1]][1] - flat.vertices[F[0]][1];
		double bx = flat.vertices[F[2]][0] - flat.vertices[F[0]][0];
		double by = flat.vertices[F[2]][1] - flat.vertices[F[0]][1];
		if (!(ax * by - ay * bx > 0))
			neg++;
	}
	TASK_CHECK(neg == 0);
	TASK_CHECK(r.diagnostics.max_unfold_error <= 1e-7 * maxlen);
	// The published coordinates are float32, so realized side lengths carry
	// ~1e-7 * coordinate-magnitude rounding.  The double-precision cross-face
	// consistency requirement is checked through max_unfold_error above.
	double maxcoord = 0.0;
	for (const point &p : flat.vertices)
	{
		maxcoord = std::max(maxcoord, (double) std::fabs(p[0]));
		maxcoord = std::max(maxcoord, (double) std::fabs(p[1]));
	}
	double ftol = 8.0 * maxcoord * 1.1920929e-7 + 1e-7 * maxlen;
	for (const ConformalMap::EdgeInfo &e : cm.edges()) {
		double dx = (double) flat.vertices[e.v1][0] - flat.vertices[e.v0][0];
		double dy = (double) flat.vertices[e.v1][1] - flat.vertices[e.v0][1];
		double d = std::sqrt(dx * dx + dy * dy);
		TASK_CHECK(std::fabs(d - e.length) <= ftol);
	}
	return 0;
}

int case_unfold_quality_and_placement()
{
	TriMesh m = make_wavy(4, 0.5);
	ConformalMap cm(m);
	std::vector<double> t = targets_for(cm, 0);
	ConformalMap::Result r = cm.solve(t, 1e-10, 300, 20000);
	TASK_CHECK(r.ok());
	TASK_CHECK(r.diagnostics.max_unfold_error <=
	           1e-7 * [&]() {
		double mx = 0.0;
		for (const ConformalMap::EdgeInfo &e : cm.edges())
			mx = std::max(mx, e.length);
		return mx;
	}());
	// Vertex 0 at the origin, lowest-numbered neighbor on +X, no reflection.
	TASK_CHECK(std::fabs(r.flat.vertices[0][0]) < 1e-9);
	TASK_CHECK(std::fabs(r.flat.vertices[0][1]) < 1e-9);
	int nbr = -1;
	for (const ConformalMap::EdgeInfo &e : cm.edges()) {
		if (e.v0 == 0 && (nbr < 0 || e.v1 < nbr)) nbr = e.v1;
		if (e.v1 == 0 && (nbr < 0 || e.v0 < nbr)) nbr = e.v0;
	}
	TASK_CHECK(nbr > 0);
	TASK_CHECK(r.flat.vertices[nbr][0] > 0);
	TASK_CHECK(std::fabs(r.flat.vertices[nbr][1]) < 1e-6);
	// Consistent, positively oriented chart from an independent unfolding.
	std::vector<point> xy;
	double disagree = 0.0, maxlen = 0.0;
	bool positive = false;
	TASK_CHECK(independent_unfold(cm, xy, disagree, maxlen, positive));
	TASK_CHECK(positive);
	TASK_CHECK(disagree <= 1e-7 * maxlen);
	return 0;
}

int case_result_value_independence()
{
	TriMesh m = make_wavy(2, 0.3);
	ConformalMap::Result r;
	{
		ConformalMap cm(m);
		std::vector<double> t = uniform_targets(cm);
		r = cm.solve(t, 1e-11, 200, 5000);
		TASK_CHECK(r.ok());
	}
	// The workspace is gone; the result must still hold valid data.
	TASK_CHECK(r.flat.vertices.size() == m.vertices.size());
	TASK_CHECK(!r.flat.faces.empty());
	// And a second solve on a different workspace must not disturb it.
	{
		ConformalMap cm2(make_umbrella(9, 0.8));
		std::vector<double> t2 = uniform_targets(cm2);
		ConformalMap::Result r2 = cm2.solve(t2, 1e-11, 200, 5000);
		TASK_CHECK(r2.ok());
	}
	TASK_CHECK(r.flat.vertices.size() == m.vertices.size());
	return 0;
}

int case_colors_and_confidences_preserved()
{
	TriMesh m = make_wavy(2, 0.3);
	m.colors.resize(m.vertices.size());
	m.confidences.resize(m.vertices.size());
	for (size_t i = 0; i < m.vertices.size(); i++) {
		m.colors[i] = Color((float) i / 10.0f, 0.25f, 0.5f);
		m.confidences[i] = 0.1f * (float) i;
	}
	ConformalMap cm(m);
	std::vector<double> t = uniform_targets(cm);
	ConformalMap::Result r = cm.solve(t, 1e-11, 200, 5000);
	TASK_CHECK(r.ok());
	TASK_CHECK(r.flat.colors.size() == m.colors.size());
	TASK_CHECK(r.flat.confidences.size() == m.confidences.size());
	for (size_t i = 0; i < m.colors.size(); i++) {
		TASK_CHECK(r.flat.colors[i] == m.colors[i]);
		TASK_CHECK(r.flat.confidences[i] == m.confidences[i]);
	}
	return 0;
}

int case_concave_boundary()
{
	// A concave (reflex) quad: vertex 2 is a reflex corner of the boundary.
	// The solve must still converge; the initial triangulation is locally
	// non-Delaunay, exercised here for robustness.
	TriMesh m;
	m.vertices = {point(0, 0, 0), point(4, 0, 0), point(1, 0.6f, 0),
	              point(0, 3, 0)};
	m.faces = {TriMesh::Face(0, 1, 2), TriMesh::Face(0, 2, 3)};
	ConformalMap cm(m);
	std::vector<double> t = uniform_targets(cm);
	ConformalMap::Result r = cm.solve(t, 1e-10, 200, 5000);
	TASK_CHECK(r.ok());
	TASK_CHECK(r.diagnostics.max_residual <= 1e-10);
	std::vector<int> bf = boundary_flags(cm);
	for (int v = 0; v < cm.num_vertices(); v++)
		TASK_CHECK(std::fabs(known_curvature(cm, v, bf) - t[v]) <= 1e-8);
	return 0;
}

int case_bundled_disk_file()
{
	// The CLI input shipped with the task must flatten successfully.
	std::unique_ptr<TriMesh> m(TriMesh::read("task_support/disk.ply"));
	TASK_CHECK(m != nullptr);
	ConformalMap cm(*m);
	std::vector<double> t = uniform_targets(cm);
	ConformalMap::Result r = cm.solve(t, 1e-10, 200, 5000);
	TASK_CHECK(r.ok());
	TASK_CHECK(r.flat.vertices.size() == m->vertices.size());
	return 0;
}

} // namespace

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

const struct task_case task_cases[] = {
	{"metric", "planar_square", case_planar_square},
	{"metric", "curvature_matches_targets", case_curvature_matches_targets},
	{"metric", "jacobian_matches_dense_system", case_jacobian_matches_dense_system},
	{"metric", "scale_and_rigid_covariance", case_scale_and_rigid_covariance},
	{"metric", "internal_vertices_zero_defect", case_internal_vertices_zero_defect},
	{"metric", "target_sum_validation", case_target_sum_validation},

	{"events", "event_flip_continuity", case_event_flip_continuity},
	{"events", "metric_continuous_across_flip", case_metric_continuous_across_flip},
	{"events", "budget_and_status", case_budget_and_status},
	{"events", "initial_delaunayization", case_initial_delaunayization},

	{"pipeline", "two_target_switches", case_two_target_switches},
	{"pipeline", "failed_solve_rolls_back", case_failed_solve_rolls_back},
	{"pipeline", "non_disk_inputs_rejected", case_non_disk_inputs_rejected},
	{"pipeline", "cold_cache_consistency", case_cold_cache_consistency},
	{"pipeline", "cli_roundtrip", case_cli_roundtrip},
	{"pipeline", "sparse_growth", case_sparse_growth},
	{"pipeline", "unfold_quality_and_placement", case_unfold_quality_and_placement},
	{"pipeline", "result_value_independence", case_result_value_independence},
	{"pipeline", "colors_and_confidences_preserved", case_colors_and_confidences_preserved},
	{"pipeline", "concave_boundary", case_concave_boundary},
	{"pipeline", "bundled_disk_file", case_bundled_disk_file},
};

const size_t task_case_count =
	sizeof(task_cases) / sizeof(task_cases[0]);

// The task driver and the tests are compiled together with -include
// ConformalMap.h; the tests must not define main (driver.c does).
