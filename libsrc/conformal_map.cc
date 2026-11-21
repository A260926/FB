/*
Discrete conformal flattening of a triangle mesh disk.

See include/ConformalMap.h for the interface and the mathematical model.
Only C++17 standard library facilities are used.

Model
-----
For a triangulation T of a disk and one parameter u per vertex, the edge
lengths are

	l_ij = l0_ij * exp((u_i + u_j) / 2),

where l0 is a reference length per edge.  Corner angles follow from the
current side lengths (law of cosines).  With theta_v the sum of the angles
incident on vertex v, the discrete angle defect is

	kappa_v = 2*pi - theta_v   (interior),
	kappa_v = pi   - theta_v   (boundary).

The prescribed targets are kappa_v == 0 at interior vertices and the
caller's boundary values (which sum to 2*pi).  We solve

	r_v = kappa_v - target_v = 0

for every vertex.  Since adding a constant to all u scales every edge length
and leaves all angles unchanged, u_0 is pinned to 0 and the equation at
vertex 0 (implied by Gauss-Bonnet) is dropped.

The Jacobian of r with respect to u is the signed cotangent Laplacian:
with w_ij = (cot alpha + cot beta)/2 the half sum of the cotangents of the
two angles opposite edge ij,

	d r_i / d u_j = w_ij  (j != i),
	d r_i / d u_i = -sum_j w_ij.

Newton therefore solves the sparse SPD system M delta = r with M = -J on
the unpinned vertices, using Jacobi-preconditioned conjugate gradients.  No
dense global matrix is ever formed.

The triangulation is kept Delaunay in the current intrinsic metric.  While
u moves along a Newton direction an interior edge can become cocircular
(opposite angles summing to pi).  When continuing would make it
non-Delaunay, the step stops exactly at that event, the edge is flipped,
the new diagonal receives the reference length that makes its current
length equal to its intrinsically developed length, and the same
conformal-class trajectory continues.
*/

#include "ConformalMap.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace trimesh {

namespace {

constexpr double kPi = 3.141592653589793238462643383279502884;
constexpr double kTwoPi = 2.0 * kPi;
constexpr double kTargetSumTolerance = 1e-10;
constexpr double kUnfoldToleranceRel = 1e-7;
constexpr double kDelaunayEps = 1e-12;
constexpr double kEventSamples = 32;

inline double clampd(double v, double lo, double hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}

inline double sqrd(double v) { return v * v; }

// Angle at the vertex between the two sides x and y, opposite side z.
inline double corner_angle(double x, double y, double z)
{
	double c = (x * x + y * y - z * z) / (2.0 * x * y);
	return std::acos(clampd(c, -1.0, 1.0));
}

inline unsigned long long edge_key(int a, int b)
{
	if (a > b)
		std::swap(a, b);
	return (static_cast<unsigned long long>(static_cast<unsigned int>(a)) << 32) |
	       static_cast<unsigned int>(b);
}

// Face-local geometry.  l[j] is the side opposite local vertex j; ang[j] is
// the corner angle at local vertex j.
struct FaceGeom {
	double l[3] = {0.0, 0.0, 0.0};
	double ang[3] = {0.0, 0.0, 0.0};
	double area = 0.0;
	bool valid = false;
};

} // anonymous namespace

// ---------------------------------------------------------------------------
// Input validation
// ---------------------------------------------------------------------------

namespace {

struct MeshAnalysis {
	int nv = 0;
	std::vector<TriMesh::Face> faces;
	struct EdgeRec {
		int a = -1, b = -1;
		int f[2] = {-1, -1};
		int local[2] = {-1, -1};
	};
	std::vector<EdgeRec> edges;
	std::unordered_map<unsigned long long, int> edge_index;
	std::vector<int> boundary_loop;
	std::vector<char> is_boundary_vertex;
};

bool analyze_mesh(const TriMesh &mesh, MeshAnalysis &out, std::string &why)
{
	using Face = TriMesh::Face;
	const int nv = (int) mesh.vertices.size();
	const int nf = (int) mesh.faces.size();
	out.faces.clear();
	out.edges.clear();
	out.edge_index.clear();
	out.boundary_loop.clear();
	out.is_boundary_vertex.clear();

	if (nv == 0 || nf == 0) {
		why = "mesh has no vertices or no faces";
		return false;
	}
	if (!mesh.colors.empty() && (int) mesh.colors.size() != nv) {
		why = "colors must be empty or have one entry per vertex";
		return false;
	}
	if (!mesh.confidences.empty() && (int) mesh.confidences.size() != nv) {
		why = "confidences must be empty or have one entry per vertex";
		return false;
	}
	for (int i = 0; i < nv; i++) {
		const point &p = mesh.vertices[i];
		if (!std::isfinite(p[0]) || !std::isfinite(p[1]) ||
		    !std::isfinite(p[2])) {
			why = "vertex coordinates must be finite";
			return false;
		}
	}

	out.nv = nv;
	out.faces = mesh.faces;

	std::vector<std::array<int, 3>> sorted_faces(nf);
	for (int f = 0; f < nf; f++) {
		const Face &F = mesh.faces[f];
		for (int j = 0; j < 3; j++) {
			if (F[j] < 0 || F[j] >= nv) {
				why = "face references a vertex out of range";
				return false;
			}
		}
		if (F[0] == F[1] || F[1] == F[2] || F[2] == F[0]) {
			why = "face has a repeated vertex";
			return false;
		}
		point e1 = mesh.vertices[F[1]] - mesh.vertices[F[0]];
		point e2 = mesh.vertices[F[2]] - mesh.vertices[F[0]];
		double area2 = len(e1 CROSS e2);
		if (!(area2 > 0.0) || !std::isfinite(area2)) {
			why = "triangle area must be strictly positive";
			return false;
		}
		std::array<int, 3> s = {F[0], F[1], F[2]};
		std::sort(s.begin(), s.end());
		sorted_faces[f] = s;
	}
	{
		std::vector<std::array<int, 3>> sorted = sorted_faces;
		std::sort(sorted.begin(), sorted.end());
		for (int f = 1; f < nf; f++) {
			if (sorted[f] == sorted[f - 1]) {
				why = "duplicate face over the same three vertices";
				return false;
			}
		}
	}

	// Edge table; each edge has at most two faces, and the two faces must
	// traverse it in opposite directions (consistent orientation).
	out.edges.reserve((size_t) nf * 2 + 8);
	for (int f = 0; f < nf; f++) {
		const Face &F = mesh.faces[f];
		for (int j = 0; j < 3; j++) {
			int a = F[NEXT_MOD3(j)];
			int b = F[PREV_MOD3(j)];
			unsigned long long key = edge_key(a, b);
			auto it = out.edge_index.find(key);
			int ei;
			if (it == out.edge_index.end()) {
				ei = (int) out.edges.size();
				out.edge_index.emplace(key, ei);
				MeshAnalysis::EdgeRec rec;
				rec.a = std::min(a, b);
				rec.b = std::max(a, b);
				out.edges.push_back(rec);
			} else {
				ei = it->second;
			}
			MeshAnalysis::EdgeRec &rec = out.edges[ei];
			int slot = (rec.f[0] < 0) ? 0 : 1;
			if (slot == 1) {
				if (rec.f[1] >= 0) {
					why = "non-manifold edge with more than two faces";
					return false;
				}
				const Face &G = mesh.faces[rec.f[0]];
				int l0 = rec.local[0];
				int da = G[NEXT_MOD3(l0)];
				int db = G[PREV_MOD3(l0)];
				if (da == a && db == b) {
					why = "inconsistent face orientation across an edge";
					return false;
				}
			}
			rec.f[slot] = f;
			rec.local[slot] = j;
		}
	}

	{
		std::vector<char> used(nv, 0);
		for (int f = 0; f < nf; f++)
			for (int j = 0; j < 3; j++)
				used[mesh.faces[f][j]] = 1;
		for (int v = 0; v < nv; v++) {
			if (!used[v]) {
				why = "mesh has an isolated vertex";
				return false;
			}
		}
	}

	// Boundary edges: exactly one incident face; the boundary must be one
	// simple loop.
	std::vector<int> boundary_edges;
	for (int e = 0; e < (int) out.edges.size(); e++)
		if (out.edges[e].f[1] < 0)
			boundary_edges.push_back(e);
	if (boundary_edges.empty()) {
		why = "mesh has no boundary, so it is not a disk";
		return false;
	}
	out.is_boundary_vertex.assign(nv, 0);
	std::unordered_map<int, int> next_bdy;
	for (int ei : boundary_edges) {
		const MeshAnalysis::EdgeRec &rec = out.edges[ei];
		const Face &G = mesh.faces[rec.f[0]];
		int l0 = rec.local[0];
		int a = G[NEXT_MOD3(l0)];
		int b = G[PREV_MOD3(l0)];
		auto it = next_bdy.find(a);
		if (it != next_bdy.end() && it->second != b) {
			why = "boundary is not a single simple loop";
			return false;
		}
		next_bdy[a] = b;
		out.is_boundary_vertex[a] = 1;
		out.is_boundary_vertex[b] = 1;
	}
	std::vector<int> indeg(nv, 0);
	int start = -1;
	for (auto &kv : next_bdy) {
		indeg[kv.second]++;
		if (start < 0)
			start = kv.first;
	}
	int nb = 0;
	for (int v = 0; v < nv; v++)
		if (out.is_boundary_vertex[v])
			nb++;
	if ((int) next_bdy.size() != nb) {
		why = "boundary is not a single simple loop";
		return false;
	}
	for (int v = 0; v < nv; v++) {
		if (out.is_boundary_vertex[v] && indeg[v] != 1) {
			why = "boundary is not a single simple loop";
			return false;
		}
	}
	if (start < 0) {
		why = "mesh has no boundary loop";
		return false;
	}
	{
		int cur = start;
		for (size_t guard = 0; guard <= (size_t) nv; guard++) {
			out.boundary_loop.push_back(cur);
			auto it = next_bdy.find(cur);
			if (it == next_bdy.end()) {
				why = "boundary loop is malformed";
				return false;
			}
			cur = it->second;
			if (cur == start)
				break;
		}
		if ((int) out.boundary_loop.size() != nb) {
			why = "boundary consists of more than one loop";
			return false;
		}
	}
	if (out.boundary_loop.size() < 3) {
		why = "boundary loop needs at least three vertices";
		return false;
	}

	// Connectedness.
	{
		std::vector<std::vector<int>> adj(nv);
		for (const auto &rec : out.edges) {
			adj[rec.a].push_back(rec.b);
			adj[rec.b].push_back(rec.a);
		}
		std::vector<char> seen(nv, 0);
		std::vector<int> stack(1, 0);
		seen[0] = 1;
		int count = 0;
		while (!stack.empty()) {
			int v = stack.back();
			stack.pop_back();
			count++;
			for (int w : adj[v]) {
				if (!seen[w]) {
					seen[w] = 1;
					stack.push_back(w);
				}
			}
		}
		if (count != nv) {
			why = "mesh is not connected";
			return false;
		}
	}

	return true;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Implementation
// ---------------------------------------------------------------------------

struct ConformalMap::Impl {
	using Face = TriMesh::Face;

	struct Edge {
		unsigned long long key = 0;
		int v0 = -1, v1 = -1;
		int f[2] = {-1, -1};
		double base = 0.0;
	};

	// ---- Input copy -----------------------------------------------------
	std::vector<point> vertices3;
	std::vector<Face> faces;
	std::vector<Color> colors;
	std::vector<float> confidences;

	// ---- State ----------------------------------------------------------
	std::vector<double> u;
	std::vector<char> bdy;
	std::vector<int> bdy_loop;
	std::vector<Edge> edges;
	std::unordered_map<unsigned long long, int> eidx;
	std::vector<std::array<int, 3>> face_edge; // edge opposite local corner j

	// Committed state used for transactional solves.
	std::vector<double> committed_u;
	std::vector<Face> committed_faces;
	std::vector<Edge> committed_edges;
	bool committed_valid = false;

	// Published map.
	TriMesh flat;
	bool has_flat = false;

	// Diagnostics and flip log of the last solve.
	Diagnostics diag;
	std::vector<FlipRecord> flip_log;

	// Cached read-only edge view.
	mutable std::vector<EdgeInfo> edge_view;
	mutable bool edge_view_valid = false;

	// ==================================================================
	// Edge / geometry helpers
	// ==================================================================

	int find_edge(int a, int b) const
	{
		auto it = eidx.find(edge_key(a, b));
		return it == eidx.end() ? -1 : it->second;
	}

	double length_of(const Edge &e, const std::vector<double> &uu) const
	{
		return e.base * std::exp(0.5 * (uu[e.v0] + uu[e.v1]));
	}

	double length_now(const Edge &e) const { return length_of(e, u); }

	double length_uu(int a, int b, const std::vector<double> &uu) const
	{
		int ei = find_edge(a, b);
		return ei < 0 ? 0.0 : length_of(edges[ei], uu);
	}

	// Geometry of face f under u.  Also enforces the strict triangle
	// inequality and finiteness.
	FaceGeom face_geometry(int f, const std::vector<double> &uu) const
	{
		FaceGeom g;
		int e0 = face_edge[f][0], e1 = face_edge[f][1], e2 = face_edge[f][2];
		if (e0 < 0 || e1 < 0 || e2 < 0)
			return g;
		g.l[0] = length_of(edges[e0], uu);
		g.l[1] = length_of(edges[e1], uu);
		g.l[2] = length_of(edges[e2], uu);
		for (int j = 0; j < 3; j++) {
			if (!(g.l[j] > 0.0) || !std::isfinite(g.l[j]))
				return g;
		}
		if (!(g.l[0] < g.l[1] + g.l[2]) ||
		    !(g.l[1] < g.l[0] + g.l[2]) ||
		    !(g.l[2] < g.l[0] + g.l[1]))
			return g;
		g.ang[0] = corner_angle(g.l[1], g.l[2], g.l[0]);
		g.ang[1] = corner_angle(g.l[2], g.l[0], g.l[1]);
		g.ang[2] = corner_angle(g.l[0], g.l[1], g.l[2]);
		g.area = 0.5 * g.l[1] * g.l[2] * std::sin(g.ang[0]);
		g.valid = std::isfinite(g.area) && g.area > 0.0 &&
		          std::isfinite(g.ang[0]) && std::isfinite(g.ang[1]) &&
		          std::isfinite(g.ang[2]);
		return g;
	}

	// Angle at vertex v in face f, computed from g (the geometry of f).
	// Returns NaN if v is not a corner of f.
	double angle_at(const FaceGeom &g, int f, int v) const
	{
		int c = faces[f].indexof(v);
		return (c < 0 || !g.valid) ? std::numeric_limits<double>::quiet_NaN()
		                           : g.ang[c];
	}

	bool edge_slack(int ei, const std::vector<double> &uu, double &slack) const
	{
		const Edge &e = edges[ei];
		if (e.f[0] < 0 || e.f[1] < 0)
			return false;
		FaceGeom g0 = face_geometry(e.f[0], uu);
		FaceGeom g1 = face_geometry(e.f[1], uu);
		if (!g0.valid || !g1.valid)
			return false;
		double a0 = angle_opposite(g0, e.f[0], e.v0, e.v1);
		double a1 = angle_opposite(g1, e.f[1], e.v0, e.v1);
		if (!std::isfinite(a0) || !std::isfinite(a1))
			return false;
		slack = kPi - (a0 + a1);
		return std::isfinite(slack);
	}

	// Angle in face f opposite the edge (a,b).
	double angle_opposite(const FaceGeom &g, int f, int a, int b) const
	{
		const Face &F = faces[f];
		int c = -1;
		for (int j = 0; j < 3; j++)
			if (F[j] != a && F[j] != b)
				c = j;
		return (c < 0 || !g.valid) ? std::numeric_limits<double>::quiet_NaN()
		                           : g.ang[c];
	}

	// ---- Topology maintenance ------------------------------------------

	void build_edges()
	{
		std::unordered_map<unsigned long long, double> old_base;
		old_base.reserve(edges.size() * 2 + 1);
		for (const Edge &e : edges)
			old_base.emplace(e.key, e.base);

		int nf = (int) faces.size();
		edges.clear();
		eidx.clear();
		edges.reserve((size_t) nf * 2 + 8);
		face_edge.assign(nf, {{-1, -1, -1}});
		for (int f = 0; f < nf; f++) {
			const Face &F = faces[f];
			for (int j = 0; j < 3; j++) {
				int a = F[NEXT_MOD3(j)];
				int b = F[PREV_MOD3(j)];
				unsigned long long key = edge_key(a, b);
				auto it = eidx.find(key);
				int ei;
				if (it == eidx.end()) {
					ei = (int) edges.size();
					eidx.emplace(key, ei);
					Edge e;
					e.key = key;
					e.v0 = std::min(a, b);
					e.v1 = std::max(a, b);
					auto ob = old_base.find(key);
					e.base = (ob == old_base.end()) ? 0.0 : ob->second;
					edges.push_back(e);
				} else {
					ei = it->second;
				}
				Edge &e = edges[ei];
				if (e.f[0] < 0)
					e.f[0] = f;
				else if (e.f[1] < 0)
					e.f[1] = f;
				face_edge[f][j] = ei;
			}
		}
		edge_view_valid = false;
	}

	// No separate vertex->face table is needed: per-face geometry is
	// computed by iterating the (small) face list, and all topology queries
	// go through the edge table.
	void build_adjacency() {}

	void build_boundary()
	{
		bdy.assign(u.size(), 0);
		bdy_loop.clear();
		std::unordered_map<int, int> next;
		for (const Edge &e : edges) {
			if (e.f[1] >= 0)
				continue;
			const Face &G = faces[e.f[0]];
			int c = G.indexof(e.v0);
			int a, b;
			if (c >= 0 && G[NEXT_MOD3(c)] == e.v1) {
				a = e.v0;
				b = e.v1;
			} else if (c >= 0 && G[PREV_MOD3(c)] == e.v1) {
				a = e.v1;
				b = e.v0;
			} else {
				continue;
			}
			next[a] = b;
			bdy[a] = 1;
			bdy[b] = 1;
		}
		if (next.empty())
			return;
		// Start at the smallest boundary vertex number so the loop identity
		// (and its first element) does not depend on hash-map iteration.
		int start = -1;
		for (auto &kv : next)
			if (start < 0 || kv.first < start)
				start = kv.first;
		int cur = start;
		for (size_t guard = 0; guard <= u.size(); guard++) {
			bdy_loop.push_back(cur);
			auto it = next.find(cur);
			if (it == next.end())
				break;
			cur = it->second;
			if (cur == start)
				break;
		}
	}

	// ---- Residuals ------------------------------------------------------

	// theta_v for every vertex.
	bool angle_sums(const std::vector<double> &uu, std::vector<double> &theta) const
	{
		theta.assign(uu.size(), 0.0);
		for (int f = 0; f < (int) faces.size(); f++) {
			FaceGeom g = face_geometry(f, uu);
			if (!g.valid)
				return false;
			theta[faces[f][0]] += g.ang[0];
			theta[faces[f][1]] += g.ang[1];
			theta[faces[f][2]] += g.ang[2];
		}
		for (double t : theta)
			if (!std::isfinite(t))
				return false;
		return true;
	}

	// r_v = kappa_v - target_v, where kappa_v = sign_v - theta_v with sign_v
	// equal to 2*pi inside and pi on the boundary.
	bool residual(const std::vector<double> &uu,
	              const std::vector<double> &target,
	              std::vector<double> &r, double &rnorm) const
	{
		std::vector<double> theta;
		if (!angle_sums(uu, theta))
			return false;
		int nv = (int) uu.size();
		r.resize(nv);
		rnorm = 0.0;
		for (int v = 0; v < nv; v++) {
			double curvature = (bdy[v] ? kPi : kTwoPi) - theta[v];
			r[v] = curvature - target[v];
			if (!std::isfinite(r[v]))
				return false;
			rnorm = std::max(rnorm, std::fabs(r[v]));
		}
		return true;
	}

	// ---- Edge flip ------------------------------------------------------

	// Perform the intrinsic flip of edge ei.  The new diagonal reference
	// length makes its current length exactly its intrinsically developed
	// length, so the metric is continuous at the event.
	//
	// f0 is oriented so that it traverses a -> b; its third vertex is k and
	// f1's third vertex is l.  The new triangles are (k,a,l) and (l,b,k).
	bool apply_flip(int ei, int phase, int step_index, bool record,
	                FlipRecord &rec, std::string &why)
	{
		Edge e = edges[ei];
		if (e.f[0] < 0 || e.f[1] < 0) {
			why = "flip requested on a boundary edge";
			return false;
		}
		int f0 = e.f[0], f1 = e.f[1];
		int a = e.v0, b = e.v1;
		{
			// Orient the labels so that f0 traverses a -> b.  Only the
			// labels swap; the two faces keep their roles (f1 traverses
			// b -> a because the mesh is consistently oriented).
			const Face &F = faces[f0];
			int c = F.indexof(a);
			if (c < 0 || F[NEXT_MOD3(c)] != b)
				std::swap(a, b);
		}
		const Face F0 = faces[f0];
		const Face F1 = faces[f1];
		int k = -1, l = -1;
		for (int j = 0; j < 3; j++) {
			if (F0[j] != a && F0[j] != b)
				k = F0[j];
			if (F1[j] != a && F1[j] != b)
				l = F1[j];
		}
		if (k < 0 || l < 0 || k == l) {
			why = "flip quad is degenerate";
			return false;
		}
		// The new diagonal must not already exist (that would be
		// non-manifold).
		int existing = find_edge(k, l);
		if (existing >= 0) {
			why = "new diagonal already exists";
			return false;
		}

		FaceGeom g0 = face_geometry(f0, u);
		FaceGeom g1 = face_geometry(f1, u);
		if (!g0.valid || !g1.valid) {
			why = "invalid geometry at the flip";
			return false;
		}
		double ak = length_uu(a, k, u);
		double al = length_uu(a, l, u);
		double bk = length_uu(b, k, u);
		double bl = length_uu(b, l, u);
		if (!(ak > 0) || !(al > 0) || !(bk > 0) || !(bl > 0)) {
			why = "missing side length at the flip";
			return false;
		}
		double angle_a0 = angle_at(g0, f0, a);
		double angle_a1 = angle_at(g1, f1, a);
		double angle_b0 = angle_at(g0, f0, b);
		double angle_b1 = angle_at(g1, f1, b);
		if (!std::isfinite(angle_a0) || !std::isfinite(angle_a1) ||
		    !std::isfinite(angle_b0) || !std::isfinite(angle_b1)) {
			why = "cannot develop the flip quad";
			return false;
		}
		double angle_a = angle_a0 + angle_a1;
		double angle_b = angle_b0 + angle_b1;
		if (!(angle_a > 0.0) || !(angle_a < kPi) ||
		    !(angle_b > 0.0) || !(angle_b < kPi)) {
			why = "flip quad is not developable";
			return false;
		}
		double len_a = std::sqrt(std::max(0.0,
			sqrd(ak) + sqrd(al) - 2.0 * ak * al * std::cos(angle_a)));
		double len_b = std::sqrt(std::max(0.0,
			sqrd(bk) + sqrd(bl) - 2.0 * bk * bl * std::cos(angle_b)));
		double new_len = 0.5 * (len_a + len_b);
		double side_scale = std::max(std::max(ak, al), std::max(bk, bl));
		if (!(new_len > 0.0) || !std::isfinite(new_len) ||
		    std::fabs(len_a - len_b) > 1e-9 * side_scale) {
			why = "inconsistent developed diagonal length";
			return false;
		}
		// Both new triangles must have positive area (strict triangle
		// inequality).
		{
			double s0 = 0.5 * (ak + al + new_len);
			double h0 = s0 * (s0 - ak) * (s0 - al) * (s0 - new_len);
			double s1 = 0.5 * (bk + bl + new_len);
			double h1 = s1 * (s1 - bk) * (s1 - bl) * (s1 - new_len);
			if (!(h0 > 0.0) || !(h1 > 0.0)) {
				why = "flip would create a degenerate triangle";
				return false;
			}
		}
		double u_k = u[k], u_l = u[l];
		double scale = std::exp(0.5 * (u_k + u_l));
		if (!(scale > 0.0) || !std::isfinite(scale)) {
			why = "non-finite scale for the new diagonal";
			return false;
		}
		double new_base = new_len / scale;
		if (!(new_base > 0.0) || !std::isfinite(new_base)) {
			why = "non-finite reference length for the new diagonal";
			return false;
		}
		double old_len = length_now(e);
		double old_base = e.base;
		double opp_sum = 0.0;
		{
			double oa = angle_opposite(g0, f0, a, b);
			double ob = angle_opposite(g1, f1, a, b);
			opp_sum = oa + ob;
		}

		if (record) {
			rec.step = step_index;
			rec.phase = phase;
			rec.a = a;
			rec.b = b;
			rec.k = k;
			rec.l = l;
			rec.old_face0 = f0;
			rec.old_face1 = f1;
			rec.u_a = u[a];
			rec.u_b = u[b];
			rec.u_k = u_k;
			rec.u_l = u_l;
			rec.old_base_length = old_base;
			rec.old_length = old_len;
			rec.new_length = new_len;
			rec.new_length_from_a = len_a;
			rec.new_length_from_b = len_b;
			rec.new_base_length = new_base;
			rec.opposite_angle_sum = opp_sum;
			rec.angle_a = angle_a;
			rec.angle_b = angle_b;
			rec.side_ak = ak;
			rec.side_al = al;
			rec.side_bk = bk;
			rec.side_bl = bl;
		}

		Face old0 = faces[f0], old1 = faces[f1];
		faces[f0] = Face(k, a, l);
		faces[f1] = Face(l, b, k);
		build_edges();
		build_adjacency();
		build_boundary();
		int nei = find_edge(k, l);
		if (nei >= 0)
			edges[nei].base = new_base;
		bool v0 = (nei >= 0) && face_geometry(f0, u).valid;
		bool v1 = (nei >= 0) && face_geometry(f1, u).valid;
		if (nei < 0 || !v0 || !v1) {
			// Undo the mutation so a failed flip leaves no trace.
			faces[f0] = old0;
			faces[f1] = old1;
			build_edges();
			build_adjacency();
			build_boundary();
			why = (nei < 0) ? "new diagonal missing after flip"
			    : (!v0 ? "flip produced invalid triangle 0"
			           : "flip produced invalid triangle 1");
			return false;
		}
		edge_view_valid = false;
		return true;
	}

	// Flip all interior edges that are strictly non-Delaunay.  Returns
	// SUCCESS when none remain, or a distinguishable failure status.
	Status repair_delaunay(int phase, int step_index, int steps_used,
	                       int &flips_used, int flip_cap, int total_cap,
	                       bool enforce_total)
	{
		// Edges whose local quad cannot be flipped (for example because the
		// new diagonal already exists) are left alone: the local topology
		// simply does not permit the flip.  The triangulation is then
		// Delaunay up to those constrained edges.
		std::vector<unsigned long long> blocked;
		for (;;) {
			bool progress = false;
			for (int ei = 0; ei < (int) edges.size(); ei++) {
				if (edges[ei].f[1] < 0)
					continue;
				double slack;
				if (!edge_slack(ei, u, slack))
					continue;
				if (slack >= -kDelaunayEps)
					continue;
				unsigned long long key = edges[ei].key;
				if (std::find(blocked.begin(), blocked.end(), key) !=
				    blocked.end())
					continue;
				if (flips_used >= flip_cap ||
				    (enforce_total &&
				     steps_used + flips_used >= total_cap))
					return BUDGET_EXHAUSTED;
				FlipRecord rec;
				std::string why;
				if (!apply_flip(ei, phase == 0 ? 0 : 2, step_index,
				                true, rec, why)) {
					blocked.push_back(key);
					continue;
				}
				flip_log.push_back(rec);
				flips_used++;
				progress = true;
				break; // edge table changed
			}
			if (!progress)
				return SUCCESS;
		}
	}

	// ==================================================================
	// Sparse cotangent system and PCG
	// ==================================================================

	struct Csr {
		int n = 0;
		std::vector<int> row;
		std::vector<int> col;
		std::vector<double> val;
		std::vector<double> diag;
		std::size_t nonzeros = 0;
	};

	// Build M = -J on the unpinned vertices (1..N-1).  Vertex 0 is pinned.
	bool build_system(Csr &A) const
	{
		int nv = (int) u.size();
		int n = nv - 1;
		if (n <= 0)
			return false;
		std::vector<double> theta;
		if (!angle_sums(u, theta))
			return false;
		std::unordered_map<unsigned long long, double> w;
		w.reserve(edges.size() * 2 + 1);
		std::vector<FaceGeom> geoms(faces.size());
		for (int f = 0; f < (int) faces.size(); f++) {
			geoms[f] = face_geometry(f, u);
			if (!geoms[f].valid)
				return false;
		}
		for (int f = 0; f < (int) faces.size(); f++) {
			for (int j = 0; j < 3; j++) {
				int ei = face_edge[f][j];
				if (ei < 0)
					return false;
				double a = geoms[f].ang[j];
				double s = std::sin(a);
				double cot = (std::fabs(s) < 1e-14)
					? (std::cos(a) >= 0 ? 1e14 : -1e14)
					: std::cos(a) / s;
				if (!std::isfinite(cot))
					return false;
				w[edges[ei].key] += 0.5 * cot;
			}
		}
		std::vector<std::vector<std::pair<int, double>>> rows(n);
		std::vector<double> vertex_weight(nv, 0.0);
		for (const Edge &e : edges) {
			auto it = w.find(e.key);
			double wij = (it == w.end()) ? 0.0 : it->second;
			if (!std::isfinite(wij))
				return false;
			vertex_weight[e.v0] += wij;
			vertex_weight[e.v1] += wij;
			if (e.v0 > 0 && e.v1 > 0) {
				int i = e.v0 - 1, j = e.v1 - 1;
				rows[i].push_back({j, -wij});
				rows[j].push_back({i, -wij});
			}
		}
		A.n = n;
		A.row.assign(n + 1, 0);
		A.col.clear();
		A.val.clear();
		A.diag.resize(n);
		std::size_t nnz = (std::size_t) n;
		for (int i = 0; i < n; i++)
			nnz += rows[i].size();
		A.col.reserve(nnz);
		A.val.reserve(nnz);
		for (int i = 0; i < n; i++) {
			A.row[i] = (int) A.val.size();
			A.diag[i] = vertex_weight[i + 1];
			if (!(A.diag[i] > 0.0) || !std::isfinite(A.diag[i]))
				return false;
			for (auto &p : rows[i]) {
				A.col.push_back(p.first);
				A.val.push_back(p.second);
			}
			A.col.push_back(i);
			A.val.push_back(A.diag[i]);
		}
		A.row[n] = (int) A.val.size();
		A.nonzeros = A.val.size();
		return true;
	}

	static double dotv(const std::vector<double> &a, const std::vector<double> &b)
	{
		double s = 0.0;
		for (size_t i = 0; i < a.size(); i++)
			s += a[i] * b[i];
		return s;
	}

	static void matvec(const Csr &A, const std::vector<double> &x,
	                   std::vector<double> &y)
	{
		for (int i = 0; i < A.n; i++) {
			double s = 0.0;
			for (int k = A.row[i]; k < A.row[i + 1]; k++)
				s += A.val[k] * x[A.col[k]];
			y[i] = s;
		}
	}

	bool pcg(const Csr &A, const std::vector<double> &b,
	         std::vector<double> &x, int &iterations)
	{
		int n = A.n;
		x.assign(n, 0.0);
		std::vector<double> r = b, z(n), p(n), Ap(n);
		double bnorm = std::sqrt(dotv(b, b));
		double binf = 0.0;
		for (double v : b)
			binf = std::max(binf, std::fabs(v));
		for (int i = 0; i < n; i++)
			z[i] = r[i] / A.diag[i];
		p = z;
		double rz = dotv(r, z);
		if (!std::isfinite(rz))
			return false;
		int maxit = std::max(200, 4 * n);
		iterations = 0;
		for (int it = 0; it < maxit; it++) {
			iterations = it + 1;
			matvec(A, p, Ap);
			double pAp = dotv(p, Ap);
			if (!(pAp > 0.0) || !std::isfinite(pAp))
				return false;
			double alpha = rz / pAp;
			double rinf = 0.0;
			for (int i = 0; i < n; i++) {
				x[i] += alpha * p[i];
				r[i] -= alpha * Ap[i];
				rinf = std::max(rinf, std::fabs(r[i]));
			}
			if (std::sqrt(dotv(r, r)) <= 1e-12 * (bnorm + 1e-300) ||
			    rinf <= 1e-14 * (1.0 + binf))
				return true;
			for (int i = 0; i < n; i++)
				z[i] = r[i] / A.diag[i];
			double rz_new = dotv(r, z);
			if (!std::isfinite(rz_new))
				return false;
			double beta = rz_new / rz;
			for (int i = 0; i < n; i++)
				p[i] = z[i] + beta * p[i];
			rz = rz_new;
		}
		double rinf = 0.0;
		for (double v : r)
			rinf = std::max(rinf, std::fabs(v));
		return rinf <= 1e-10 * (1.0 + binf);
	}

	// Triplet form of the Jacobian M = d r / d u on vertices 1..N-1.
	// (Row/col are original vertex numbers minus one.)
	bool jacobian_triplets(std::vector<JacobianEntry> &out) const
	{
		Csr A;
		if (!build_system(A))
			return false;
		out.clear();
		out.reserve(A.nonzeros);
		for (int i = 0; i < A.n; i++) {
			for (int k = A.row[i]; k < A.row[i + 1]; k++)
				out.push_back({i, A.col[k], A.val[k]});
		}
		return true;
	}

	// ==================================================================
	// Event-limited stepping
	// ==================================================================

	bool slacks_at(const std::vector<double> &dir, double t,
	               const std::vector<double> &u0,
	               std::vector<double> &slacks,
	               std::vector<double> &ut) const
	{
		ut.resize(u0.size());
		for (size_t i = 0; i < u0.size(); i++)
			ut[i] = u0[i] + t * dir[i];
		slacks.assign(edges.size(), 0.0);
		for (int ei = 0; ei < (int) edges.size(); ei++) {
			if (edges[ei].f[1] < 0) {
				slacks[ei] = 1.0;
				continue;
			}
			double s;
			if (!edge_slack(ei, ut, s))
				return false;
			slacks[ei] = s;
		}
		return true;
	}

	// Locate the first t in (0, tmax] where an interior edge stops being
	// Delaunay, or where the metric becomes invalid.
	bool find_first_event(const std::vector<double> &dir, double tmax,
	                      const std::vector<double> &u0, double &t_event,
	                      int &edge_event, bool &is_invalid) const
	{
		std::vector<double> prev, cur, ut;
		if (!slacks_at(dir, 0.0, u0, prev, ut))
			return false;
		double t_prev = 0.0;
		for (int k = 1; k <= (int) kEventSamples; k++) {
			double t = tmax * (double) k / (double) kEventSamples;
			if (!slacks_at(dir, t, u0, cur, ut)) {
				double lo = t_prev, hi = t;
				for (int it = 0; it < 48; it++) {
					double mid = 0.5 * (lo + hi);
					std::vector<double> s2, u2;
					if (slacks_at(dir, mid, u0, s2, u2))
						lo = mid;
					else
						hi = mid;
				}
				t_event = hi;
				edge_event = -1;
				is_invalid = true;
				return true;
			}
			int bad = -1;
			double worst = 0.0;
			for (int ei = 0; ei < (int) edges.size(); ei++) {
				if (edges[ei].f[1] < 0)
					continue;
				if (prev[ei] >= 0.0 && cur[ei] < 0.0) {
					double drop = prev[ei] - cur[ei];
					if (bad < 0 || drop > worst) {
						bad = ei;
						worst = drop;
					}
				}
			}
			if (bad >= 0) {
				int va = edges[bad].v0, vb = edges[bad].v1;
				double lo = t_prev, hi = t;
				for (int it = 0; it < 48; it++) {
					double mid = 0.5 * (lo + hi);
					std::vector<double> u2(u0.size());
					for (size_t i = 0; i < u0.size(); i++)
						u2[i] = u0[i] + mid * dir[i];
					int ei = find_edge(va, vb);
					double s2 = -1.0;
					if (ei >= 0)
						edge_slack(ei, u2, s2);
					else
						break;
					if (s2 >= 0.0)
						lo = mid;
					else
						hi = mid;
				}
				t_event = hi;
				edge_event = bad;
				is_invalid = false;
				return true;
			}
			prev.swap(cur);
			t_prev = t;
		}
		return false;
	}

	struct StepResult {
		Status status = SUCCESS;
		int flips = 0;
		bool invalid_metric = false;
		std::string why;
	};

	// Advance u by tmax*dir, stopping at and flipping cocircular edges as
	// required.  Mutates the state; the caller may restore it.
	StepResult apply_step(const std::vector<double> &dir, double tmax,
	                      int step_index, int flips_used, int flip_cap,
	                      int steps_used, int total_cap, bool enforce_total)
	{
		StepResult result;
		double remaining = tmax;
		int local_flips = 0;
		for (int guard = 0; guard < 200000; guard++) {
			std::vector<double> u_start = u;
			double t_event = 0.0;
			int edge_event = -1;
			bool is_invalid = false;
			bool found = find_first_event(dir, remaining, u_start,
			                              t_event, edge_event, is_invalid);
			if (is_invalid) {
				// Advancing all the way to the degeneracy would leave a
				// nearly collapsed triangle and an ill-conditioned
				// cotangent system.  Back off from the event and, in case
				// the valid interval is narrower than expected, halve until
				// every triangle is representable again.  The next Newton
				// iteration re-linearizes from the safer metric.
				double t = 0.5 * t_event;
				if (!(t > 0.0))
					t = 0.0;
				for (int guard2 = 0; guard2 < 60; guard2++) {
					for (size_t i = 0; i < u.size(); i++)
						u[i] = u_start[i] + t * dir[i];
					bool valid = true;
					for (int f = 0; f < (int) faces.size() && valid; f++)
						valid = face_geometry(f, u).valid;
					if (valid || t == 0.0)
						break;
					t *= 0.5;
				}
				result.invalid_metric = true;
				result.flips = local_flips;
				return result;
			}
			if (!found) {
				for (size_t i = 0; i < u.size(); i++)
					u[i] = u_start[i] + remaining * dir[i];
				result.flips = local_flips;
				return result;
			}
			// Advance to the event.
			for (size_t i = 0; i < u.size(); i++)
				u[i] = u_start[i] + t_event * dir[i];
			remaining -= t_event;
			if (remaining < 1e-14 * std::max(1.0, tmax))
				remaining = 0.0;

			int va = edges[edge_event].v0;
			int vb = edges[edge_event].v1;
			bool flipped = false;
			std::string why;
			for (int attempt = 0; attempt < 10 && !flipped; attempt++) {
				if (flips_used + local_flips >= flip_cap ||
				    (enforce_total &&
				     steps_used + flips_used + local_flips >= total_cap)) {
					result.status = BUDGET_EXHAUSTED;
					result.flips = local_flips;
					return result;
				}
				int ei = find_edge(va, vb);
				if (ei < 0) {
					result.status = TOPOLOGY_FAILURE;
					result.why = "event edge disappeared";
					result.flips = local_flips;
					return result;
				}
				FlipRecord rec;
				if (apply_flip(ei, 1, step_index, true, rec, why)) {
					flip_log.push_back(rec);
					flipped = true;
					local_flips++;
					break;
				}
				// Numerically degenerate exact event: back off a tiny amount
				// along the direction and retry.  The state is rolled back
				// after the attempt so failed trials do not drift the map.
				double back = (attempt + 1) * 1e-11 *
					std::max(1.0, tmax);
				for (size_t i = 0; i < u.size(); i++)
					u[i] -= back * dir[i];
				remaining += back;
			}
			if (!flipped) {
				// The local quad does not admit a flip.  Stop at the event
				// without crossing it: the metric stays valid and Delaunay
				// up to this constrained edge.
				result.status = SUCCESS;
				result.why = why;
				result.flips = local_flips;
				return result;
			}
			if (remaining <= 0.0)
				break;
		}
		result.flips = local_flips;
		return result;
	}

	// ==================================================================
	// Unfolding
	// ==================================================================

	struct XY {
		double x = 0.0, y = 0.0;
	};

	bool unfold(double &max_error, TriMesh &out, std::string &why) const
	{
		int nv = (int) u.size();
		int nf = (int) faces.size();
		std::vector<XY> pos(nv);
		std::vector<char> placed(nv, 0), face_done(nf, 0);
		double max_len = 0.0;
		for (const Edge &e : edges)
			max_len = std::max(max_len, length_now(e));
		if (!(max_len > 0.0) || !std::isfinite(max_len)) {
			why = "no finite edge length to unfold";
			return false;
		}
		max_error = 0.0;

		// Seed with face 0, preserving its input winding.
		{
			const Face &F = faces[0];
			double l01 = length_uu(F[0], F[1], u);
			double l12 = length_uu(F[1], F[2], u);
			double l20 = length_uu(F[2], F[0], u);
			if (!(l01 > 0) || !(l12 > 0) || !(l20 > 0)) {
				why = "degenerate starting face";
				return false;
			}
			pos[F[0]] = {0.0, 0.0};
			pos[F[1]] = {l01, 0.0};
			double x = (sqrd(l01) + sqrd(l20) - sqrd(l12)) / (2.0 * l01);
			double h2 = sqrd(l20) - sqrd(x);
			if (!(h2 > 0.0) || !std::isfinite(h2)) {
				why = "starting face cannot be unfolded";
				return false;
			}
			pos[F[2]] = {x, std::sqrt(h2)};
			placed[F[0]] = placed[F[1]] = placed[F[2]] = 1;
			face_done[0] = 1;
		}

		std::vector<int> queue(1, 0);
		for (size_t qi = 0; qi < queue.size(); qi++) {
			int f = queue[qi];
			const Face &F = faces[f];
			for (int j = 0; j < 3; j++) {
				int a = F[NEXT_MOD3(j)];
				int b = F[PREV_MOD3(j)];
				int ei = face_edge[f][j];
				if (ei < 0)
					continue;
				const Edge &e = edges[ei];
				if (e.f[1] < 0)
					continue;
				int g = (e.f[0] == f) ? e.f[1] : e.f[0];
				if (g < 0 || face_done[g])
					continue;
				const Face &G = faces[g];
				int c = -1;
				for (int t = 0; t < 3; t++)
					if (G[t] != a && G[t] != b)
						c = G[t];
				if (c < 0) {
					why = "adjacent face does not share the edge";
					return false;
				}
				double la = length_uu(a, c, u);
				double lb = length_uu(b, c, u);
				if (!(la > 0) || !(lb > 0)) {
					why = "missing length while unfolding";
					return false;
				}
				XY pa = pos[a], pb = pos[b];
				double dx = pb.x - pa.x, dy = pb.y - pa.y;
				double lab = std::sqrt(dx * dx + dy * dy);
				if (!(lab > 0.0)) {
					why = "zero-length edge while unfolding";
					return false;
				}
				double ex = dx / lab, ey = dy / lab;
				double x = (sqrd(la) + sqrd(lab) - sqrd(lb)) / (2.0 * lab);
				double h2 = sqrd(la) - sqrd(x);
				if (h2 < -1e-9 * sqrd(max_len)) {
					why = "inconsistent edge lengths while unfolding";
					return false;
				}
				h2 = std::max(0.0, h2);
				double h = std::sqrt(h2);
				double nx = -ey, ny = ex;
				XY pc = pos[F[j]];
				double side = (pc.x - pa.x) * nx + (pc.y - pa.y) * ny;
				double sgn = (side > 0.0) ? -1.0 : 1.0;
				XY pred{pa.x + x * ex + sgn * h * nx,
				        pa.y + x * ey + sgn * h * ny};
				if (placed[c]) {
					double ddx = pred.x - pos[c].x;
					double ddy = pred.y - pos[c].y;
					double de = std::sqrt(ddx * ddx + ddy * ddy);
					if (de > 1e-8 * max_len)
							max_error = std::max(max_error, de);
					// The already-placed position is authoritative; do not
					// overwrite it with the newly predicted one.
				} else {
					pos[c] = pred;
					placed[c] = 1;
				}
				face_done[g] = 1;
				queue.push_back(g);
			}
		}

		for (int f = 0; f < nf; f++) {
			if (!face_done[f]) {
				why = "unfolding did not reach every face";
				return false;
			}
		}
		for (int v = 0; v < nv; v++) {
			if (!placed[v]) {
				why = "unfolding did not reach every vertex";
				return false;
			}
		}

		// Every face must be positively oriented and every realized side must
		// match the intrinsic length.
		for (int f = 0; f < nf; f++) {
			const Face &F = faces[f];
			double ax = pos[F[1]].x - pos[F[0]].x;
			double ay = pos[F[1]].y - pos[F[0]].y;
			double bx = pos[F[2]].x - pos[F[0]].x;
			double by = pos[F[2]].y - pos[F[0]].y;
			double cross = ax * by - ay * bx;
			if (!(cross > 0.0) || !std::isfinite(cross)) {
				why = "unfolded face is not positively oriented";
				return false;
			}
			for (int j = 0; j < 3; j++) {
				int a = F[j];
				int b = F[NEXT_MOD3(j)];
				double ddx = pos[b].x - pos[a].x;
				double ddy = pos[b].y - pos[a].y;
				double dd = std::sqrt(ddx * ddx + ddy * ddy);
				double l = length_uu(a, b, u);
				max_error = std::max(max_error, std::fabs(dd - l));
			}
		}
		if (!(max_error <= kUnfoldToleranceRel * max_len)) {
			why = "unfolded positions disagree by more than 1e-7 * max edge length";
			return false;
		}

		// Rigid placement: vertex 0 at the origin, its lowest-numbered
		// neighbor along +X, no reflection.
		int nbr = -1;
		for (const Edge &e : edges) {
			if (e.v0 == 0 && (nbr < 0 || e.v1 < nbr))
				nbr = e.v1;
			else if (e.v1 == 0 && (nbr < 0 || e.v0 < nbr))
				nbr = e.v0;
		}
		if (nbr < 0) {
			why = "vertex 0 has no neighbor";
			return false;
		}
		XY o = pos[0];
		for (int v = 0; v < nv; v++) {
			pos[v].x -= o.x;
			pos[v].y -= o.y;
		}
		double ang = std::atan2(pos[nbr].y, pos[nbr].x);
		double ca = std::cos(-ang), sa = std::sin(-ang);
		for (int v = 0; v < nv; v++) {
			double x = pos[v].x, y = pos[v].y;
			pos[v].x = ca * x - sa * y;
			pos[v].y = sa * x + ca * y;
		}
		if (!(pos[nbr].x > 0.0) ||
		    std::fabs(pos[nbr].y) > 1e-9 * max_len) {
			why = "failed to orient the flat map";
			return false;
		}
		for (int v = 0; v < nv; v++) {
			if (!std::isfinite(pos[v].x) || !std::isfinite(pos[v].y)) {
				why = "non-finite flat coordinate";
				return false;
			}
		}

		out.clear();
		out.vertices.resize(nv);
		for (int v = 0; v < nv; v++)
			out.vertices[v] = point((float) pos[v].x, (float) pos[v].y, 0.0f);
		out.faces = faces;
		if (!colors.empty())
			out.colors = colors;
		if (!confidences.empty())
			out.confidences = confidences;
		return true;
	}

	// ==================================================================
	// Transactional solve
	// ==================================================================

	void snapshot()
	{
		committed_u = u;
		committed_faces = faces;
		committed_edges = edges;
		committed_valid = true;
	}

	void restore()
	{
		u = committed_u;
		faces = committed_faces;
		edges = committed_edges;
		eidx.clear();
		for (int i = 0; i < (int) edges.size(); i++)
			eidx[edges[i].key] = i;
		build_edges();
		build_adjacency();
		build_boundary();
		edge_view_valid = false;
	}

	Status run_solve(const std::vector<double> &target, double tol,
	                 int step_cap, int flip_cap, int total_cap,
	                 bool enforce_total, Diagnostics &out)
	{
		int steps_used = 0;
		int flips_used = 0;
		int initial_flips = 0;
		out.linear_iterations = 0;
		out.linear_nonzeros = 0;
		out.max_residual = std::numeric_limits<double>::infinity();

		Status st = repair_delaunay(0, 0, steps_used, flips_used,
		                            flip_cap, total_cap, enforce_total);
		initial_flips = flips_used;
		if (st != SUCCESS) {
			out.steps = steps_used;
			out.flips = flips_used;
			out.initial_flips = initial_flips;
			out.work_used = steps_used + flips_used;
			std::vector<double> r;
			double rn = 0.0;
			if (residual(u, target, r, rn))
				out.max_residual = rn;
			return st;
		}

		std::vector<double> r;
		double rnorm = 0.0;
		for (;;) {
			if (!residual(u, target, r, rnorm)) {
				out.steps = steps_used;
				out.flips = flips_used;
				out.initial_flips = initial_flips;
				out.work_used = steps_used + flips_used;
				return STAGNATED;
			}
			out.max_residual = rnorm;
			if (rnorm <= tol)
				break;
			if (steps_used >= step_cap ||
			    (enforce_total && steps_used + flips_used >= total_cap)) {
				out.steps = steps_used;
				out.flips = flips_used;
				out.initial_flips = initial_flips;
				out.work_used = steps_used + flips_used;
				return BUDGET_EXHAUSTED;
			}
			st = repair_delaunay(1, steps_used, steps_used, flips_used,
			                     flip_cap, total_cap, enforce_total);
			if (st != SUCCESS) {
				out.steps = steps_used;
				out.flips = flips_used;
				out.initial_flips = initial_flips;
				out.work_used = steps_used + flips_used;
				return st;
			}

			Csr A;
			if (!build_system(A)) {
				out.steps = steps_used;
				out.flips = flips_used;
				out.initial_flips = initial_flips;
				out.work_used = steps_used + flips_used;
				return STAGNATED;
			}
			out.linear_nonzeros = A.nonzeros;
			// With L the standard cotangent Laplacian (L_ii = sum w,
			// L_ij = -w), the residual Jacobian is J = L because the angle
			// sum decreases when the vertex's own u grows.  Newton solves
			// J delta = -r, i.e. L delta = -r.  Row 0 (pinned) drops.
			std::vector<double> b(r.size() - 1);
			for (size_t i = 0; i + 1 < r.size(); i++)
				b[i] = -r[i + 1];
			std::vector<double> delta_red;
			int iters = 0;
			if (!pcg(A, b, delta_red, iters)) {
				out.linear_iterations = iters;
				out.steps = steps_used;
				out.flips = flips_used;
				out.initial_flips = initial_flips;
				out.work_used = steps_used + flips_used;
				return STAGNATED;
			}
			out.linear_iterations = iters;
			std::vector<double> dir(u.size(), 0.0);
			for (int i = 0; i < (int) delta_red.size(); i++)
				dir[i + 1] = delta_red[i];
			double dmax = 0.0;
			for (double v : dir)
				dmax = std::max(dmax, std::fabs(v));
			if (!(dmax > 0.0) || !std::isfinite(dmax)) {
				out.steps = steps_used;
				out.flips = flips_used;
				out.initial_flips = initial_flips;
				out.work_used = steps_used + flips_used;
				return STAGNATED;
			}
			// Keep the exponential parameterization in range.
			if (dmax > 50.0) {
				double s = 50.0 / dmax;
				for (double &v : dir)
					v *= s;
			}

			// Backtracking line search.
			bool accepted = false;
			Status fail_status = STAGNATED;
			std::vector<double> u_back = u;
			std::vector<Face> faces_back = faces;
			std::vector<Edge> edges_back = edges;
			std::vector<FlipRecord> log_back = flip_log;
			for (int trial = 0; trial < 40; trial++) {
				double t = std::ldexp(1.0, -trial);
				u = u_back;
				faces = faces_back;
				edges = edges_back;
				flip_log = log_back;
				eidx.clear();
				for (int i = 0; i < (int) edges.size(); i++)
					eidx[edges[i].key] = i;
				build_edges();
				build_adjacency();
				build_boundary();
				StepResult sr = apply_step(dir, t, steps_used, flips_used,
				                           flip_cap, steps_used, total_cap,
				                           enforce_total);
				if (sr.status == BUDGET_EXHAUSTED ||
				    sr.status == TOPOLOGY_FAILURE) {
					fail_status = sr.status;
					break;
				}
				std::vector<double> r2;
				double rn2 = 0.0;
				if (!residual(u, target, r2, rn2))
					continue;
				if (rn2 <= tol || rn2 < rnorm * (1.0 - 1e-4 * t)) {
					flips_used += sr.flips;
					accepted = true;
					break;
				}
			}
			if (!accepted) {
				u = u_back;
				faces = faces_back;
				edges = edges_back;
				flip_log = log_back;
				eidx.clear();
				for (int i = 0; i < (int) edges.size(); i++)
					eidx[edges[i].key] = i;
				build_edges();
				build_adjacency();
				build_boundary();
				out.steps = steps_used;
				out.flips = flips_used;
				out.initial_flips = initial_flips;
				out.work_used = steps_used + flips_used;
				return fail_status;
			}
			// Account for the step and the flips it needed.  If the combined
			// work exceeds the total budget, undo the step so the reported
			// counts and the committed state never exceed the budget.
			if (enforce_total && steps_used + 1 + flips_used > total_cap) {
				u = u_back;
				faces = faces_back;
				edges = edges_back;
				flip_log = log_back;
				eidx.clear();
				for (int i = 0; i < (int) edges.size(); i++)
					eidx[edges[i].key] = i;
				build_edges();
				build_adjacency();
				build_boundary();
				out.steps = steps_used;
				out.flips = flips_used;
				out.initial_flips = initial_flips;
				out.work_used = steps_used + flips_used;
				return BUDGET_EXHAUSTED;
			}
			steps_used++;

		}

		double max_error = 0.0;
		TriMesh candidate;
		std::string why;
		Status final_status = SUCCESS;
		if (!unfold(max_error, candidate, why)) {
			final_status = UNFOLD_FAILURE;
			out.last_error = why;
		}
		out.max_unfold_error = max_error;
		out.steps = steps_used;
		out.flips = flips_used;
		out.initial_flips = initial_flips;
		out.work_used = steps_used + flips_used;
		out.max_residual = rnorm;
		out.published = (final_status == SUCCESS);
		if (final_status == SUCCESS) {
			flat = std::move(candidate);
			has_flat = true;
		}
		return final_status;
	}

	void refresh_edge_view() const
	{
		edge_view.clear();
		edge_view.reserve(edges.size());
		for (const Edge &e : edges) {
			EdgeInfo info;
			info.v0 = e.v0;
			info.v1 = e.v1;
			info.base_length = e.base;
			info.length = length_of(e, u);
			info.faces[0] = e.f[0];
			info.faces[1] = e.f[1];
			edge_view.push_back(info);
		}
		edge_view_valid = true;
	}
};

// ---------------------------------------------------------------------------
// Public interface
// ---------------------------------------------------------------------------

const char *ConformalMap::status_name(Status status)
{
	switch (status) {
	case SUCCESS: return "SUCCESS";
	case BUDGET_EXHAUSTED: return "BUDGET_EXHAUSTED";
	case STAGNATED: return "STAGNATED";
	case TOPOLOGY_FAILURE: return "TOPOLOGY_FAILURE";
	case UNFOLD_FAILURE: return "UNFOLD_FAILURE";
	case INVALID_STATE: return "INVALID_STATE";
	}
	return "UNKNOWN";
}

ConformalMap::ConformalMap(const TriMesh &mesh)
{
	// Validate before allocating or establishing any state, so an invalid
	// input throws with the object untouched.
	MeshAnalysis analysis;
	std::string why;
	if (!analyze_mesh(mesh, analysis, why))
		throw std::invalid_argument("ConformalMap: " + why);

	impl_.reset(new Impl);
	impl_->vertices3 = mesh.vertices;
	impl_->faces = analysis.faces;
	impl_->colors = mesh.colors;
	impl_->confidences = mesh.confidences;
	impl_->u.assign(mesh.vertices.size(), 0.0);
	impl_->bdy = analysis.is_boundary_vertex;
	impl_->bdy_loop = analysis.boundary_loop;

	impl_->build_edges();
	for (auto &e : impl_->edges) {
		const point &p0 = mesh.vertices[e.v0];
		const point &p1 = mesh.vertices[e.v1];
		vec d = p1 - p0;
		e.base = std::sqrt((double) d[0] * d[0] + (double) d[1] * d[1] +
		                   (double) d[2] * d[2]);
	}
	impl_->build_adjacency();
	impl_->build_boundary();
	impl_->snapshot();
	impl_->diag.status = INVALID_STATE;
}

ConformalMap::~ConformalMap() = default;

ConformalMap::Result ConformalMap::solve(const std::vector<double> &boundary_targets,
                                         double tolerance, int max_work)
{
	if (max_work <= 0)
		throw std::invalid_argument(
			"ConformalMap::solve: work budget must be positive");
	return solve_impl(boundary_targets, tolerance, max_work, max_work,
	                  max_work);
}

ConformalMap::Result ConformalMap::solve(const std::vector<double> &boundary_targets,
                                         double tolerance, int max_steps,
                                         int max_flips)
{
	if (max_steps <= 0)
		throw std::invalid_argument(
			"ConformalMap::solve: step budget must be positive");
	if (max_flips < 0)
		throw std::invalid_argument(
			"ConformalMap::solve: flip budget must be non-negative");
	return solve_impl(boundary_targets, tolerance, max_steps, max_flips,
	                  max_steps + max_flips);
}

ConformalMap::Result ConformalMap::solve_impl(
	const std::vector<double> &boundary_targets, double tolerance,
	int max_steps, int max_flips, int total_cap)
{
	Impl &I = *impl_;
	const int nv = (int) I.u.size();
	if (!(tolerance > 0.0) || !std::isfinite(tolerance))
		throw std::invalid_argument(
			"ConformalMap::solve: tolerance must be positive and finite");
	if ((int) boundary_targets.size() < nv)
		throw std::invalid_argument(
			"ConformalMap::solve: boundary target vector is too short");

	double sum = 0.0;
	for (int v = 0; v < nv; v++) {
		if (!I.bdy[v])
			continue;
		double t = boundary_targets[v];
		if (!std::isfinite(t) || !(t > 0.0) || !(t < kPi))
			throw std::invalid_argument(
				"ConformalMap::solve: boundary targets must be strictly in (0, pi)");
		sum += t;
	}
	if (std::fabs(sum - kTwoPi) > kTargetSumTolerance)
		throw std::invalid_argument(
			"ConformalMap::solve: boundary targets must sum to 2*pi");

	std::vector<double> target(nv, 0.0);
	for (int v = 0; v < nv; v++)
		if (I.bdy[v])
			target[v] = boundary_targets[v];

	I.snapshot();
	I.diag = Diagnostics();
	I.diag.tolerance = tolerance;
	I.flip_log.clear();

	Status st = I.run_solve(target, tolerance, max_steps, max_flips,
	                        total_cap, true, I.diag);
	I.diag.status = st;
	if (st != SUCCESS) {
		I.restore();
		I.diag.published = false;
		I.edge_view_valid = false;
		I.flip_log.clear();
		// The workspace is back at its committed state, so report the
		// residual of the state the caller can actually observe.  The step
		// and flip counters still describe the work the failed attempt did.
		std::vector<double> r_after;
		double rn_after = 0.0;
		if (I.residual(I.u, target, r_after, rn_after))
			I.diag.max_residual = rn_after;
		Result res;
		res.status = st;
		res.diagnostics = I.diag;
		return res;
	}
	I.committed_u = I.u;
	I.committed_faces = I.faces;
	I.committed_edges = I.edges;
	I.edge_view_valid = false;
	Result res;
	res.status = SUCCESS;
	res.diagnostics = I.diag;
	res.flat = I.flat;
	return res;
}

const std::vector<double> &ConformalMap::u() const { return impl_->u; }
const std::vector<TriMesh::Face> &ConformalMap::faces() const { return impl_->faces; }

const std::vector<ConformalMap::EdgeInfo> &ConformalMap::edges() const
{
	if (!impl_->edge_view_valid)
		impl_->refresh_edge_view();
	return impl_->edge_view;
}

const std::vector<int> &ConformalMap::boundary_loop() const { return impl_->bdy_loop; }

bool ConformalMap::is_boundary_vertex(int v) const
{
	return v >= 0 && v < (int) impl_->bdy.size() && impl_->bdy[v];
}

int ConformalMap::num_vertices() const { return (int) impl_->u.size(); }
int ConformalMap::num_faces() const { return (int) impl_->faces.size(); }
int ConformalMap::num_edges() const { return (int) impl_->edges.size(); }

double ConformalMap::edge_length(int v0, int v1) const
{
	int ei = impl_->find_edge(v0, v1);
	return ei < 0 ? 0.0 : impl_->length_now(impl_->edges[ei]);
}

std::vector<double> ConformalMap::angle_defects() const
{
	std::vector<double> theta;
	std::vector<double> out(impl_->u.size(), 0.0);
	if (!impl_->angle_sums(impl_->u, theta))
		return out;
	for (int v = 0; v < (int) out.size(); v++)
		out[v] = (impl_->bdy[v] ? kPi : kTwoPi) - theta[v];
	return out;
}

const ConformalMap::Diagnostics &ConformalMap::diagnostics() const
{
	return impl_->diag;
}

const std::vector<ConformalMap::FlipRecord> &ConformalMap::flip_records() const
{
	return impl_->flip_log;
}

std::vector<ConformalMap::JacobianEntry> ConformalMap::jacobian() const
{
	std::vector<JacobianEntry> out;
	if (!impl_->jacobian_triplets(out))
		out.clear();
	return out;
}

bool ConformalMap::has_flat_map() const { return impl_->has_flat; }
const TriMesh &ConformalMap::flat() const { return impl_->flat; }
TriMesh ConformalMap::flat_mesh() const { return impl_->flat; }

} // namespace trimesh
