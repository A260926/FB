/*
tests/adaptive_refinement.cc
Exhaustive checks for trimesh::AdaptiveRefiner.

The tests deliberately avoid peeking at the refiner internals: they use only
the public API (leaves, children, refine, coarsen, snapshot) plus the ordinary
TriMesh operators.  Positions, colors and confidences of exported meshes are
verified independently with barycentric interpolation directly from the
original input, never by re-interpolating the compacted output indices.
*/

#include "AdaptiveRefiner.h"
#include "TriMesh.h"
#include "TriMesh_algo.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <stdexcept>
#include <utility>
#include <vector>

using namespace trimesh;

typedef AdaptiveRefiner::NodeId NodeId;

static void check_at(bool cond, int line)
{
	if (!cond) {
		std::fprintf(stderr,
		             "ADAPTIVE_REFINEMENT assertion failed at line %d\n",
		             line);
		std::exit(1);
	}
}
#define check(x) check_at((x), __LINE__)

static bool approx(double a, double b, double eps = 1e-6)
{
	return std::fabs(a - b) <= eps;
}

template <class F>
static bool throws_invalid(F f)
{
	try {
		f();
	} catch (std::invalid_argument &) {
		return true;
	} catch (...) {
		return false;
	}
	return false;
}

template <class F>
static bool throws_runtime(F f)
{
	try {
		f();
	} catch (std::runtime_error &) {
		return true;
	} catch (...) {
		return false;
	}
	return false;
}

// ---------------------------------------------------------------------------
// Small mesh builders
// ---------------------------------------------------------------------------

struct GridMesh {
	TriMesh mesh;
	std::vector<int> vid;

	GridMesh(int nx, int ny, double (*zfunc)(int, int) = 0)
	{
		vid.resize((nx + 1) * (ny + 1));
		for (int j = 0; j <= ny; j++) {
			for (int i = 0; i <= nx; i++) {
				double z = zfunc ? zfunc(i, j) : 0.0;
				vid[j * (nx + 1) + i] = (int) mesh.vertices.size();
				mesh.vertices.push_back(
					point((float) i, (float) j, (float) z));
			}
		}
		for (int j = 0; j < ny; j++) {
			for (int i = 0; i < nx; i++) {
				int a = vid[j * (nx + 1) + i];
				int b = vid[j * (nx + 1) + i + 1];
				int c = vid[(j + 1) * (nx + 1) + i + 1];
				int d = vid[(j + 1) * (nx + 1) + i];
				mesh.faces.push_back(TriMesh::Face(a, b, c));
				mesh.faces.push_back(TriMesh::Face(a, c, d));
			}
		}
	}
};

static TriMesh make_tetra()
{
	TriMesh m;
	m.vertices.push_back(point(0, 0, 0));
	m.vertices.push_back(point(1, 0, 0));
	m.vertices.push_back(point(0, 1, 0));
	m.vertices.push_back(point(0, 0, 1));
	m.faces.push_back(TriMesh::Face(0, 2, 1));
	m.faces.push_back(TriMesh::Face(0, 1, 3));
	m.faces.push_back(TriMesh::Face(0, 3, 2));
	m.faces.push_back(TriMesh::Face(1, 2, 3));
	return m;
}

static TriMesh make_coincident_pair()
{
	TriMesh m;
	point p0(0, 0, 0), p1(1, 0, 0), p2(0, 1, 0);
	m.vertices.push_back(p0);
	m.vertices.push_back(p1);
	m.vertices.push_back(p2);
	m.vertices.push_back(p0);
	m.vertices.push_back(p1);
	m.vertices.push_back(p2);
	m.faces.push_back(TriMesh::Face(0, 1, 2));
	m.faces.push_back(TriMesh::Face(3, 4, 5));
	return m;
}

static TriMesh make_two_tetra()
{
	TriMesh a = make_tetra();
	TriMesh b = make_tetra();
	int off = (int) a.vertices.size();
	for (size_t i = 0; i < b.vertices.size(); i++)
		a.vertices.push_back(b.vertices[i] + vec(6.0f, 0, 0));
	for (size_t i = 0; i < b.faces.size(); i++)
		a.faces.push_back(TriMesh::Face(b.faces[i][0] + off,
		                                b.faces[i][1] + off,
		                                b.faces[i][2] + off));
	return a;
}

// ---------------------------------------------------------------------------
// Forest view built purely from children()
// ---------------------------------------------------------------------------

struct Forest {
	std::map<NodeId, int> level;
	std::map<NodeId, NodeId> root;
	std::map<NodeId, std::vector<NodeId> > kids;
	std::size_t node_count;

	Forest() : node_count(0) {}

	void dfs(const AdaptiveRefiner &r, NodeId id, NodeId rt, int lev)
	{
		level[id] = lev;
		root[id] = rt;
		node_count++;
		std::vector<NodeId> c = r.children(id);
		if (!c.empty()) {
			kids[id] = c;
			for (size_t i = 0; i < c.size(); i++)
				dfs(r, c[i], rt, lev + 1);
		}
	}

	void build(const AdaptiveRefiner &r, int nf)
	{
		level.clear();
		root.clear();
		kids.clear();
		node_count = 0;
		for (int i = 0; i < nf; i++)
			dfs(r, (NodeId) i, (NodeId) i, 0);
	}

};

static double cross_area(const point &a, const point &b, const point &c)
{
	vec u = b - a, v = c - a;
	return 0.5 * len(u CROSS v);
}

// ---------------------------------------------------------------------------
// Snapshot analysis
// ---------------------------------------------------------------------------

struct SnapStats {
	int class_count[4];
	int boundary_edges;
	int nfaces;
	int nverts;
	int leaves;
	SnapStats()
		: boundary_edges(0), nfaces(0), nverts(0), leaves(0)
	{
		for (int i = 0; i < 4; i++)
			class_count[i] = 0;
	}
};

static bool on_segment(const point &q, const point &a, const point &b,
                       double tol)
{
	vec ab = b - a;
	double l2 = (double) (ab DOT ab);
	if (l2 <= 0.0)
		return false;
	vec aq = q - a;
	double t = (double) (aq DOT ab) / l2;
	vec perp = aq - (float) t * ab;
	double dist = std::sqrt((double) (perp DOT perp));
	return t >= -tol && t <= 1.0 + tol && dist <= tol;
}

static void barycentric(const point &q, const point &p0, const point &p1,
                        const point &p2, double &a, double &b, double &c)
{
	vec e1 = p1 - p0, e2 = p2 - p0;
	double d00 = (double) (e1 DOT e1);
	double d01 = (double) (e1 DOT e2);
	double d11 = (double) (e2 DOT e2);
	double d20 = (double) ((q - p0) DOT e1);
	double d21 = (double) ((q - p0) DOT e2);
	double den = d00 * d11 - d01 * d01;
	b = (d11 * d20 - d01 * d21) / den;
	c = (d00 * d21 - d01 * d20) / den;
	a = 1.0 - b - c;
}

static std::vector<int> boundary_loop(
	const std::vector<std::pair<int, int> > &bedges)
{
	std::map<int, std::vector<int> > adj;
	for (size_t i = 0; i < bedges.size(); i++) {
		adj[bedges[i].first].push_back(bedges[i].second);
		adj[bedges[i].second].push_back(bedges[i].first);
	}
	for (std::map<int, std::vector<int> >::iterator it = adj.begin();
	     it != adj.end(); ++it)
		if (it->second.size() != 2)
			return std::vector<int>();
	std::vector<int> loop;
	if (adj.empty())
		return loop;
	int start = adj.begin()->first;
	int prev = -1, cur = start;
	while (true) {
		loop.push_back(cur);
		const std::vector<int> &n = adj[cur];
		int next = (n[0] == prev) ? n[1] : n[0];
		prev = cur;
		cur = next;
		if (cur == start)
			break;
		if (loop.size() > bedges.size() + 1)
			return std::vector<int>();
	}
	return loop;
}

static SnapStats analyze_snapshot(const AdaptiveRefiner &r,
                                  AdaptiveRefiner::Snapshot &snap,
                                  const TriMesh &orig,
                                  bool expect_colors, bool expect_conf,
                                  bool check_leaf_groups = true)
{
	SnapStats st;
	TriMesh &m = snap.mesh;
	st.nfaces = (int) m.faces.size();
	st.nverts = (int) m.vertices.size();

	check(snap.root_face.size() == (size_t) st.nfaces);
	check(snap.leaf_id.size() == (size_t) st.nfaces);
	check(m.colors.size() == (expect_colors ? (size_t) st.nverts : 0));
	check(m.confidences.size() == (expect_conf ? (size_t) st.nverts : 0));

	// Face indices in range, all output vertices used, no degenerate faces.
	std::set<int> used;
	for (int i = 0; i < st.nfaces; i++) {
		TriMesh::Face f = m.faces[i];
		for (int j = 0; j < 3; j++) {
			check(f[j] >= 0 && f[j] < st.nverts);
			check(f[j] != f[(j + 1) % 3]);
			used.insert(f[j]);
		}
		check(cross_area(m.vertices[f[0]], m.vertices[f[1]],
		                 m.vertices[f[2]]) > 0.0);
	}
	check((int) used.size() == st.nverts);

	// Leaf ids and root faces.
	std::vector<NodeId> leaves = r.leaves();
	std::set<NodeId> leafset(leaves.begin(), leaves.end());
	int nf = (int) orig.faces.size();
	for (size_t i = 0; i < leaves.size(); i++)
		check(std::find(snap.leaf_id.begin(), snap.leaf_id.end(),
		                leaves[i]) != snap.leaf_id.end());
	for (int i = 0; i < st.nfaces; i++) {
		check(snap.root_face[i] >= 0 && snap.root_face[i] < nf);
		check(leafset.find(snap.leaf_id[i]) != leafset.end());
		if (i)
			check(snap.leaf_id[i - 1] <= snap.leaf_id[i]);
	}
	st.leaves = (int) leaves.size();
	// Output faces per leaf must be nondecreasing in leaf order; every leaf
	// appears exactly once as a contiguous group.
	std::map<NodeId, int> group_count;
	for (int i = 0; i < st.nfaces; i++)
		group_count[snap.leaf_id[i]]++;
	check((int) group_count.size() == st.leaves);
	for (std::map<NodeId, int>::iterator it = group_count.begin();
	     it != group_count.end(); ++it)
		check(it->second >= 1 && it->second <= 4);

	// Index-edge incidence, no duplicate directed edge (consistent winding).
	std::map<std::pair<int, int>, int> und;
	std::map<std::pair<int, int>, int> dir;
	for (int i = 0; i < st.nfaces; i++) {
		for (int j = 0; j < 3; j++) {
			int a = m.faces[i][j], b = m.faces[i][(j + 1) % 3];
			und[std::make_pair(std::min(a, b), std::max(a, b))]++;
			dir[std::make_pair(a, b)]++;
		}
	}
	for (std::map<std::pair<int, int>, int>::iterator it = und.begin();
	     it != und.end(); ++it) {
		check(it->second == 1 || it->second == 2);
		if (it->second == 1)
			st.boundary_edges++;
	}
	for (std::map<std::pair<int, int>, int>::iterator it = dir.begin();
	     it != dir.end(); ++it)
		check(it->second == 1);
	// Euler edge count: 3F = 2(E - B) + B, so 2E = 3F + B.
	check(2 * (int) und.size() == 3 * st.nfaces + st.boundary_edges);

	// Per-leaf red level (from the public forest) and patch boundary, used
	// afterwards to verify the one-level-gap rule between leaves that share an
	// edge over positive length.
	std::map<NodeId, int> leaf_level;
	std::map<NodeId, std::vector<std::pair<point, point> > > leaf_bdy;
	{
		Forest forest;
		forest.build(r, nf);
		for (size_t i = 0; i < leaves.size(); i++)
			leaf_level[leaves[i]] = forest.level[leaves[i]];
	}

	// Orientation agrees with the containing root face.
	for (int i = 0; i < st.nfaces; i++) {
		TriMesh::Face rf = orig.faces[snap.root_face[i]];
		vec ro = (orig.vertices[rf[1]] - orig.vertices[rf[0]]) CROSS
		         (orig.vertices[rf[2]] - orig.vertices[rf[0]]);
		vec fo = (m.vertices[m.faces[i][1]] - m.vertices[m.faces[i][0]]) CROSS
		         (m.vertices[m.faces[i][2]] - m.vertices[m.faces[i][0]]);
		check((double) (ro DOT fo) > 0.0);
	}

	// Independent interpolation check: each output vertex must be the linear
	// midpoint-rule combination of the root face's original corners.
	for (int i = 0; i < st.nfaces; i++) {
		TriMesh::Face rf = orig.faces[snap.root_face[i]];
		point p0 = orig.vertices[rf[0]];
		point p1 = orig.vertices[rf[1]];
		point p2 = orig.vertices[rf[2]];
		for (int j = 0; j < 3; j++) {
			int v = m.faces[i][j];
			double a, b, c;
			barycentric(m.vertices[v], p0, p1, p2, a, b, c);
			check(a >= -1e-4 && b >= -1e-4 && c >= -1e-4);
			check(approx(a + b + c, 1.0, 1e-5));
			point interp =
				(float) a * p0 + (float) b * p1 + (float) c * p2;
			check(dist2(interp, m.vertices[v]) < 1e-8);
			// Dyadic weights k / 2^12.
			check(std::fabs(a * 4096.0 -
			                std::floor(a * 4096.0 + 0.5)) < 1e-2);
			check(std::fabs(b * 4096.0 -
			                std::floor(b * 4096.0 + 0.5)) < 1e-2);
			if (expect_colors) {
				Color ci = (float) a * orig.colors[rf[0]] +
				           (float) b * orig.colors[rf[1]] +
				           (float) c * orig.colors[rf[2]];
				check(dist2(ci, m.colors[v]) < 1e-8);
			}
			if (expect_conf) {
				double ci = a * orig.confidences[rf[0]] +
				            b * orig.confidences[rf[1]] +
				            c * orig.confidences[rf[2]];
				check(std::fabs(ci - m.confidences[v]) < 1e-5);
			}
		}
	}

	// Area preservation.
	double orig_area = 0, out_area = 0;
	for (size_t i = 0; i < orig.faces.size(); i++)
		orig_area += cross_area(orig.vertices[orig.faces[i][0]],
		                        orig.vertices[orig.faces[i][1]],
		                        orig.vertices[orig.faces[i][2]]);
	for (int i = 0; i < st.nfaces; i++)
		out_area += cross_area(m.vertices[m.faces[i][0]],
		                       m.vertices[m.faces[i][1]],
		                       m.vertices[m.faces[i][2]]);
	check(std::fabs(orig_area - out_area) <= 1e-5 * (1.0 + orig_area));

	if (!check_leaf_groups)
		return st;

	// Boundary bookkeeping: output boundary edges lie on original boundary
	// edges, and each original boundary segment is subdivided exactly.
	std::map<std::pair<int, int>, int> orig_edge_count;
	for (size_t i = 0; i < orig.faces.size(); i++)
		for (int j = 0; j < 3; j++) {
			int a = orig.faces[i][j], b = orig.faces[i][(j + 1) % 3];
			orig_edge_count[std::make_pair(std::min(a, b),
			                               std::max(a, b))]++;
		}
	std::map<std::pair<int, int>, std::vector<int> > per_orig_bdy;
	for (std::map<std::pair<int, int>, int>::iterator it =
			orig_edge_count.begin(); it != orig_edge_count.end(); ++it) {
		if (it->second != 1)
			continue;
		for (int v = 0; v < st.nverts; v++)
			if (on_segment(m.vertices[v], orig.vertices[it->first.first],
			               orig.vertices[it->first.second], 1e-4))
				per_orig_bdy[it->first].push_back(v);
	}
	int expected_boundary_segments = 0;
	for (std::map<std::pair<int, int>, std::vector<int> >::iterator it =
			per_orig_bdy.begin(); it != per_orig_bdy.end(); ++it) {
		check(!it->second.empty());
		expected_boundary_segments += (int) it->second.size() - 1;
	}
	check(st.boundary_edges == expected_boundary_segments);
	for (std::map<std::pair<int, int>, int>::iterator it = und.begin();
	     it != und.end(); ++it) {
		if (it->second != 1)
			continue;
		point qa = m.vertices[it->first.first];
		point qb = m.vertices[it->first.second];
		bool found = false;
		for (std::map<std::pair<int, int>, std::vector<int> >::iterator oit =
				per_orig_bdy.begin(); oit != per_orig_bdy.end(); ++oit) {
			if (on_segment(qa, orig.vertices[oit->first.first],
			               orig.vertices[oit->first.second], 1e-4) &&
			    on_segment(qb, orig.vertices[oit->first.first],
			               orig.vertices[oit->first.second], 1e-4)) {
				found = true;
				break;
			}
		}
		check(found);
	}

	// Per-leaf patch completeness and class-specific green structure.
	std::map<NodeId, std::vector<int> > groups;
	for (int i = 0; i < st.nfaces; i++)
		groups[snap.leaf_id[i]].push_back(i);
	for (std::map<NodeId, std::vector<int> >::iterator g = groups.begin();
	     g != groups.end(); ++g) {
		int k = (int) g->second.size();
		st.class_count[k - 1]++;

		std::map<std::pair<int, int>, int> em;
		std::vector<std::pair<int, int> > bdy;
		for (size_t fi = 0; fi < g->second.size(); fi++) {
			int f = g->second[fi];
			for (int j = 0; j < 3; j++) {
				int a = m.faces[f][j], b = m.faces[f][(j + 1) % 3];
				em[std::make_pair(std::min(a, b), std::max(a, b))]++;
			}
		}
		for (std::map<std::pair<int, int>, int>::iterator it = em.begin();
		     it != em.end(); ++it) {
			check(it->second == 1 || it->second == 2);
			if (it->second == 1)
				bdy.push_back(it->first);
		}
		std::vector<int> loop = boundary_loop(bdy);
		check(!loop.empty());
		// Record the patch boundary segments for the global gap check below.
		for (size_t i = 0; i < loop.size(); i++) {
			leaf_bdy[g->first].push_back(std::make_pair(
				m.vertices[loop[i]],
				m.vertices[loop[(i + 1) % loop.size()]]));
		}
		check((int) loop.size() == (int) bdy.size());
		check((int) bdy.size() == 3 + (k - 1));

		// Polygon area of the patch boundary equals the sum of its output
		// triangles: complete coverage, no overlap, no gap.
		// Use the projection onto the root face's plane for a robust check.
		TriMesh::Face rf = orig.faces[snap.root_face[g->second[0]]];
		vec n = (orig.vertices[rf[1]] - orig.vertices[rf[0]]) CROSS
		        (orig.vertices[rf[2]] - orig.vertices[rf[0]]);
		double nlen = len(n);
		double poly_proj = 0;
		for (size_t i = 0; i < loop.size(); i++) {
			point a = m.vertices[loop[i]];
			point b = m.vertices[loop[(i + 1) % loop.size()]];
			poly_proj += (a CROSS b) DOT (n * (1.0f / (float) nlen));
		}
		poly_proj = std::fabs(0.5 * poly_proj);
		double sum_proj = 0;
		for (size_t fi = 0; fi < g->second.size(); fi++) {
			int f = g->second[fi];
			vec u = m.vertices[m.faces[f][1]] - m.vertices[m.faces[f][0]];
			vec v = m.vertices[m.faces[f][2]] - m.vertices[m.faces[f][0]];
			sum_proj += 0.5 * std::fabs((double) ((u CROSS v) DOT
			                              (n * (1.0f / (float) nlen))));
		}
		check(std::fabs(poly_proj - sum_proj) <= 1e-5 * (1.0 + sum_proj));

		// Split-edge midpoints on the patch boundary.
		std::vector<int> mids;
		for (size_t i = 0; i < loop.size(); i++) {
			point prev =
				m.vertices[loop[(i + loop.size() - 1) % loop.size()]];
			point cur = m.vertices[loop[i]];
			point next = m.vertices[loop[(i + 1) % loop.size()]];
			if (dist2(0.5f * (prev + next), cur) < 1e-10)
				mids.push_back(loop[i]);
		}
		check((int) mids.size() == k - 1);

		if (k == 2) {
			// Exactly one split edge: the two triangles share the midpoint.
			check(mids.size() == 1);
			int midv = mids[0];
			int shared = 0;
			for (size_t fi = 0; fi < g->second.size(); fi++) {
				int f = g->second[fi];
				for (int j = 0; j < 3; j++)
					if (m.faces[f][j] == midv)
						shared++;
			}
			check(shared == 2);
		}
		if (k == 3) {
			// Two split edges meeting at one corner; check diagonal choice.
			check(mids.size() == 2);
			size_t nb = loop.size();
			int shared = -1, far_a = -1, far_b = -1;
			int m_a = -1, m_b = -1;
			for (size_t i = 0; i < nb; i++) {
				if (std::find(mids.begin(), mids.end(), loop[i]) !=
				    mids.end())
					continue;
				size_t ip = (i + nb - 1) % nb, in = (i + 1) % nb;
				bool pm = std::find(mids.begin(), mids.end(), loop[ip]) !=
				          mids.end();
				bool nm = std::find(mids.begin(), mids.end(), loop[in]) !=
				          mids.end();
				if (pm && nm) {
					shared = loop[i];
					m_a = loop[ip];
					m_b = loop[in];
					far_a = loop[(i + nb - 2) % nb];
					far_b = loop[(i + 2) % nb];
					break;
				}
			}
			check(shared >= 0);
			// Find the interior diagonal (shared edge of the two output
			// triangles that are not the middle triangle).
			std::map<std::pair<int, int>, int> twice;
			for (std::map<std::pair<int, int>, int>::iterator it = em.begin();
			     it != em.end(); ++it)
				if (it->second == 2)
					twice[it->first] = 1;
			// Middle triangle (m_a, shared, m_b) contributes two interior
			// edges; the diagonal is the third doubled edge.
			int diagonals = 0;
			for (std::map<std::pair<int, int>, int>::iterator it =
					twice.begin(); it != twice.end(); ++it) {
				int x = it->first.first, y = it->first.second;
				bool mid_mid =
					(x == std::min(m_a, m_b) && y == std::max(m_a, m_b));
				bool mid_shared =
					(x == std::min(m_a, shared) && y == std::max(m_a, shared)) ||
					(x == std::min(m_b, shared) && y == std::max(m_b, shared));
				if (mid_mid || mid_shared)
					continue;
				diagonals++;
				// Candidate diagonals: (far_a,m_b) and (m_a,far_b).  The
				// chosen one must be the lexicographically smaller pair.
				std::pair<int, int> c1(std::min(far_a, m_b),
				                       std::max(far_a, m_b));
				std::pair<int, int> c2(std::min(m_a, far_b),
				                       std::max(m_a, far_b));
				std::pair<int, int> chosen = std::min(c1, c2);
				check(std::make_pair(x, y) == chosen);
			}
			check(diagonals == 1);
		}
		if (k == 4) {
			// All three edges split.  Exactly three mids, the centre
			// triangle (m01,m12,m20) exists, and each corner triangle uses
			// its two adjacent mids.
			check(mids.size() == 3);
			std::set<int> midset(mids.begin(), mids.end());
			bool centre = false;
			int corner_tris = 0;
			for (size_t fi = 0; fi < g->second.size(); fi++) {
				int f = g->second[fi];
				int in_mid = 0;
				for (int j = 0; j < 3; j++)
					if (midset.count(m.faces[f][j]))
						in_mid++;
				if (in_mid == 3)
					centre = true;
				else if (in_mid == 2)
					corner_tris++;
			}
			check(centre);
			check(corner_tris == 3);
			// Every mid edge (mid-mid) is shared by the centre and one
			// corner triangle: three interior doubled edges.
			int doubled = 0;
			for (std::map<std::pair<int, int>, int>::iterator it = em.begin();
			     it != em.end(); ++it)
				if (it->second == 2)
					doubled++;
			check(doubled == 3);
		}
	}

	// One-level-gap rule between adjacent red leaves.  Two leaves are adjacent
	// over positive length if one has a boundary segment that is collinearly
	// contained in (or contains) a boundary segment of the other, beyond a
	// shared endpoint.  For each such pair the levels must differ by <= 1.
	{
		std::map<NodeId, std::vector<std::pair<point, point> > >::iterator a;
		for (a = leaf_bdy.begin(); a != leaf_bdy.end(); ++a) {
			std::map<NodeId, std::vector<std::pair<point, point> > >::iterator b;
			for (b = leaf_bdy.begin(); b != leaf_bdy.end(); ++b) {
				if (!(a->first < b->first))
					continue;
				bool adjacent = false;
				for (size_t i = 0; i < a->second.size() && !adjacent; i++) {
					for (size_t j = 0; j < b->second.size() && !adjacent; j++) {
						const point &p0 = a->second[i].first;
						const point &p1 = a->second[i].second;
						const point &q0 = b->second[j].first;
						const point &q1 = b->second[j].second;
						// Collinearity: directions parallel and q0 on the
						// line through p0-p1.
						vec d1 = p1 - p0, d2 = q1 - q0;
						float cross = len(d1 CROSS d2);
						if (cross > 1e-6f * len(d1) * len(d2))
							continue;
						vec perp = (q0 - p0) - d1 * ((q0 - p0) DOT d1 /
						                              (d1 DOT d1));
						if (len(perp) > 1e-4f)
							continue;
						// Overlap of the two segments along d1.
						double l2 = (double) (d1 DOT d1);
						if (l2 <= 0.0)
							continue;
						double t0 = (double) ((q0 - p0) DOT d1) / l2;
						double t1 = (double) ((q1 - p0) DOT d1) / l2;
						double lo = std::min(t0, t1), hi = std::max(t0, t1);
						double ov = std::min(hi, 1.0) - std::max(lo, 0.0);
						if (ov > 1e-3)
							adjacent = true;
					}
				}
				if (!adjacent)
					continue;
				int la = leaf_level[a->first];
				int lb = leaf_level[b->first];
				check(std::abs(la - lb) <= 1);
			}
		}
	}

	// Derived state must be empty/invalid as delivered.
	check(m.tstrips.empty());
	check(m.grid.empty());
	check(m.grid_width == -1 && m.grid_height == -1);
	check(m.flags.empty());
	check(m.normals.empty());
	check(m.pdir1.empty() && m.pdir2.empty());
	check(m.curv1.empty() && m.curv2.empty());
	check(m.dcurv.empty());
	check(m.cornerareas.empty() && m.pointareas.empty());
	check(m.neighbors.empty() && m.adjacentfaces.empty());
	check(m.across_edge.empty());
	check(!m.bbox.valid);
	check(!m.bsphere.valid);
	return st;
}

// Recompute derived data with the ordinary TriMesh operators and cross-check
// against the output faces.
static void check_derived(AdaptiveRefiner::Snapshot &snap)
{
	TriMesh &m = snap.mesh;
	int nv = (int) m.vertices.size();
	int nf = (int) m.faces.size();

	m.need_across_edge();
	check((int) m.across_edge.size() == nf);
	std::map<std::pair<int, int>, int> und;
	for (int i = 0; i < nf; i++)
		for (int j = 0; j < 3; j++) {
			int a = m.faces[i][j], b = m.faces[i][(j + 1) % 3];
			und[std::make_pair(std::min(a, b), std::max(a, b))]++;
		}
	int boundary = 0;
	for (std::map<std::pair<int, int>, int>::iterator it = und.begin();
	     it != und.end(); ++it)
		if (it->second == 1)
			boundary++;
	int reported_boundary = 0;
	for (int i = 0; i < nf; i++) {
		for (int j = 0; j < 3; j++) {
			int g = m.across_edge[i][j];
			if (g == -1) {
				reported_boundary++;
				continue;
			}
			check(g >= 0 && g < nf && g != i);
			int v1 = m.faces[i][(j + 1) % 3];
			int v2 = m.faces[i][(j + 2) % 3];
			int k = m.faces[g].indexof(v2);
			check(k >= 0);
			check(m.faces[g][(k + 1) % 3] == v1);
		}
	}
	check(reported_boundary == boundary);

	m.need_normals();
	check((int) m.normals.size() == nv);
	for (int i = 0; i < nv; i++) {
		check(std::isfinite((double) m.normals[i][0]) &&
		      std::isfinite((double) m.normals[i][1]) &&
		      std::isfinite((double) m.normals[i][2]));
		check(std::fabs((double) len(m.normals[i]) - 1.0) < 1e-3);
	}

	m.need_pointareas();
	check((int) m.pointareas.size() == nv);
	check((int) m.cornerareas.size() == nf);
	double total = 0, pa = 0;
	for (int i = 0; i < nf; i++) {
		TriMesh::Face f = m.faces[i];
		double a = cross_area(m.vertices[f[0]], m.vertices[f[1]],
		                      m.vertices[f[2]]);
		total += a;
		double ca = m.cornerareas[i][0] + m.cornerareas[i][1] +
		            m.cornerareas[i][2];
		check(std::fabs(ca - a) <= 1e-5 * (1.0 + a));
	}
	for (int i = 0; i < nv; i++) {
		check(std::isfinite((double) m.pointareas[i]));
		check(m.pointareas[i] >= -1e-6);
		pa += m.pointareas[i];
	}
	check(std::fabs(pa - total) <= 1e-4 * (1.0 + total));
}

// ---------------------------------------------------------------------------
// Test sections
// ---------------------------------------------------------------------------

static void test_input_validation()
{
	TriMesh m = make_tetra();
	m.colors.assign(m.vertices.size(), Color(0, 0, 0));
	m.confidences.assign(m.vertices.size(), 0.0f);

	TriMesh bad = m;
	bad.colors.push_back(Color(1, 0, 0));
	check(throws_invalid([&]() { AdaptiveRefiner r(bad); }));

	bad = m;
	bad.confidences.push_back(1.0f);
	check(throws_invalid([&]() { AdaptiveRefiner r(bad); }));

	bad = m;
	bad.faces[0][0] = 99;
	check(throws_invalid([&]() { AdaptiveRefiner r(bad); }));

	// Collinear / zero-area face.
	bad.vertices.clear();
	bad.faces.clear();
	bad.vertices.push_back(point(0, 0, 0));
	bad.vertices.push_back(point(1, 0, 0));
	bad.vertices.push_back(point(2, 0, 0));
	bad.faces.push_back(TriMesh::Face(0, 1, 2));
	check(throws_invalid([&]() { AdaptiveRefiner r(bad); }));

	// NaN position.
	bad = m;
	bad.vertices[0] = point(0.0f / 0.0f, 0, 0);
	check(throws_invalid([&]() { AdaptiveRefiner r(bad); }));

	// Three faces on one edge (non-manifold edge).
	bad.vertices.clear();
	bad.faces.clear();
	bad.vertices.push_back(point(0, 0, 0));
	bad.vertices.push_back(point(1, 0, 0));
	bad.vertices.push_back(point(0.5f, 1, 0));
	bad.vertices.push_back(point(0.5f, -1, 0));
	bad.faces.push_back(TriMesh::Face(0, 1, 2));
	bad.faces.push_back(TriMesh::Face(1, 0, 3));
	bad.faces.push_back(TriMesh::Face(0, 1, 3));
	check(throws_invalid([&]() { AdaptiveRefiner r(bad); }));

	// Inconsistent orientation across a shared edge.
	bad.vertices.clear();
	bad.faces.clear();
	bad.vertices.push_back(point(0, 0, 0));
	bad.vertices.push_back(point(1, 0, 0));
	bad.vertices.push_back(point(0, 1, 0));
	bad.vertices.push_back(point(1, 1, 0));
	bad.faces.push_back(TriMesh::Face(0, 1, 2));
	bad.faces.push_back(TriMesh::Face(0, 1, 3));
	check(throws_invalid([&]() { AdaptiveRefiner r(bad); }));

	// Non-manifold vertex: two triangles sharing one vertex only.
	bad.vertices.clear();
	bad.faces.clear();
	bad.vertices.push_back(point(0, 0, 0));
	bad.vertices.push_back(point(1, 0, 0));
	bad.vertices.push_back(point(0, 1, 0));
	bad.vertices.push_back(point(-1, -1, 0));
	bad.vertices.push_back(point(-1, 0, 0));
	bad.faces.push_back(TriMesh::Face(0, 1, 2));
	bad.faces.push_back(TriMesh::Face(0, 3, 4));
	check(throws_invalid([&]() { AdaptiveRefiner r(bad); }));

	// Empty mesh is legal.
	TriMesh empty;
	AdaptiveRefiner r(empty);
	check(r.leaves().empty());
	check(r.snapshot().mesh.faces.empty());
}

static void test_tetrahedron_classes()
{
	TriMesh orig = make_tetra();
	orig.colors = { Color(0, 0, 0), Color(1, 0, 0), Color(0, 1, 0),
	                Color(0, 0, 1) };
	orig.confidences = { 0, 1, 2, 3 };

	AdaptiveRefiner r(orig);
	std::vector<NodeId> roots = r.leaves();
	check(roots.size() == 4);
	for (int i = 0; i < 4; i++) {
		check(roots[i] == (NodeId) i);
		check(r.children((NodeId) i).empty());
	}

	// Refine three of the four roots.  Face 3 then has three finer neighbours
	// across all of its edges: it becomes a class-3 green patch (4 triangles).
	r.refine({ 0, 1, 2 });
	AdaptiveRefiner::Snapshot s3 = r.snapshot();
	SnapStats st = analyze_snapshot(r, s3, orig, true, true);
	check(st.class_count[3] == 1);
	check(st.class_count[0] == 12);
	check(st.leaves == 13);
	check(st.nfaces == 4 + 12);
	check(st.boundary_edges == 0);
	check_derived(s3);

	// Uniform refinement of every root: no green transitions at all.
	TriMesh t2 = make_tetra();
	AdaptiveRefiner r2(t2);
	r2.refine({ 0, 1, 2, 3 });
	AdaptiveRefiner::Snapshot s = r2.snapshot();
	SnapStats st2 = analyze_snapshot(r2, s, t2, false, false);
	check(st2.leaves == 16);
	check(st2.class_count[0] == 16);
	check(st2.nfaces == 16);
	check(st2.boundary_edges == 0);
	check_derived(s);
}

static void test_fold_boundary_class1()
{
	// Folded open sheet (raised mid-column, so it is not planar).  Refine one
	// root; the neighbour sharing the diagonal is one level coarser and gets
	// a class-1 green transition.
	GridMesh g(2, 1, [](int x, int y) {
		double z = 0.35 * (x == 1 ? 1.0 : 0.0) + 0.05 * y;
		return z;
	});
	TriMesh &orig = g.mesh;
	orig.colors.resize(orig.vertices.size());
	orig.confidences.resize(orig.vertices.size());
	for (size_t i = 0; i < orig.vertices.size(); i++) {
		orig.colors[i] = Color((float) orig.vertices[i][0],
		                       (float) orig.vertices[i][1], 0.25f);
		orig.confidences[i] = (float) (orig.vertices[i][0] +
		                               orig.vertices[i][1]);
	}
	AdaptiveRefiner r(orig);
	check(r.leaves().size() == 4);
	// Grid faces: 0=(00,01,11), 1=(00,11,10), 2=(01,02,12), 3=(01,12,11).
	// Face 0 and 3 share edge 01-11; face 1 and 3 share edge 01-12.
	r.refine({ 0 });
	AdaptiveRefiner::Snapshot s = r.snapshot();
	SnapStats st = analyze_snapshot(r, s, orig, true, true);
	check(st.leaves == 7);
	// Faces 1 and 3 each share exactly one edge with root 0's patch.
	check(st.class_count[1] == 2);
	check(st.class_count[0] == 5);
	check(st.nfaces == 5 + 2 * 2);
	check_derived(s);

	// Far root keeps its original single-leaf identity.
	Forest f;
	f.build(r, (int) orig.faces.size());
	check(f.level[(NodeId) 2] == 0);
	check(r.children(2).empty());
	int n = 0;
	for (size_t i = 0; i < r.leaves().size(); i++)
		if (f.root[r.leaves()[i]] == 2)
			n++;
	check(n == 1);

	// The first snapshot does not change when the refiner evolves.
	size_t before_faces = s.mesh.faces.size();
	size_t before_verts = s.mesh.vertices.size();
	std::vector<point> before_pos = s.mesh.vertices;
	std::vector<NodeId> kids = r.children(0);
	check(kids.size() == 4);
	r.refine({ kids[0] });
	check(s.mesh.faces.size() == before_faces);
	check(s.mesh.vertices.size() == before_verts);
	check(s.mesh.vertices == before_pos);

	AdaptiveRefiner::Snapshot s2 = r.snapshot();
	analyze_snapshot(r, s2, orig, true, true);
	check_derived(s2);
}

static void test_class2_diagonal()
{
	// On a tetrahedron every face touches every other.  Refining exactly two
	// roots leaves the two coarse faces with two finer neighbours each, so
	// each must become a class-2 green patch whose quad uses the
	// lexicographically smaller diagonal.
	TriMesh orig = make_tetra();
	AdaptiveRefiner r(orig);
	r.refine({ 1, 2 });
	AdaptiveRefiner::Snapshot s = r.snapshot();
	SnapStats st = analyze_snapshot(r, s, orig, false, false);
	check(st.leaves == 10);
	check(st.class_count[2] == 2); // faces 0 and 3
	check(st.class_count[0] == 8); // the eight refined children
	check(st.nfaces == 8 + 2 * 3);
	check(st.boundary_edges == 0); // closed tetrahedron
	check_derived(s);
}

static void test_order_and_permutation()
{
	GridMesh g(2, 2);
	TriMesh &orig = g.mesh;
	AdaptiveRefiner a(orig);
	AdaptiveRefiner b(orig);
	a.refine({ 0, 1 });
	b.refine({ 1, 0, 1, 0, 0 });
	check(a.leaves() == b.leaves());
	for (size_t i = 0; i < a.leaves().size(); i++)
		check(a.children(a.leaves()[i]) == b.children(b.leaves()[i]));
	AdaptiveRefiner::Snapshot sa = a.snapshot();
	AdaptiveRefiner::Snapshot sb = b.snapshot();
	check(sa.mesh.vertices.size() == sb.mesh.vertices.size());
	check(sa.mesh.faces.size() == sb.mesh.faces.size());
	for (size_t i = 0; i < sa.mesh.vertices.size(); i++)
		check(dist2(sa.mesh.vertices[i], sb.mesh.vertices[i]) < 1e-12);
	for (size_t i = 0; i < sa.mesh.faces.size(); i++)
		check(sa.mesh.faces[i] == sb.mesh.faces[i]);

	// A duplicate-heavy request must not disturb the number of leaves beyond
	// the same selection.
	AdaptiveRefiner c(orig);
	c.refine({ 0 });
	AdaptiveRefiner d(orig);
	d.refine({ 0, 0 });
	check(c.leaves() == d.leaves());
	check(c.snapshot().mesh.vertices.size() ==
	      d.snapshot().mesh.vertices.size());

	// Input relabelling: reverse vertex numbering and face order.  The
	// refinement structure must remain isomorphic and area-true.
	int nv = (int) orig.vertices.size();
	std::vector<int> perm(nv);
	for (int i = 0; i < nv; i++)
		perm[i] = (nv - 1) - i;
	TriMesh pm;
	pm.vertices.resize(nv);
	for (int i = 0; i < nv; i++)
		pm.vertices[perm[i]] = orig.vertices[i];
	int nf = (int) orig.faces.size();
	pm.faces.resize(nf);
	for (int i = 0; i < nf; i++) {
		TriMesh::Face f = orig.faces[nf - 1 - i];
		pm.faces[i] = TriMesh::Face(perm[f[0]], perm[f[1]], perm[f[2]]);
	}
	AdaptiveRefiner pr(pm);
	pr.refine({ 0 });
	// Root 0 of the permuted mesh is a different geometric face, so the
	// closure may touch a different number of neighbours; the important
	// invariants are checked on the snapshot below.
	check(pr.leaves().size() >= 5);
	AdaptiveRefiner::Snapshot ps = pr.snapshot();
	SnapStats pst = analyze_snapshot(pr, ps, pm, false, false);
	check(pst.nfaces >= 6);
	double area_orig = 0, area_perm = 0;
	for (size_t i = 0; i < orig.faces.size(); i++)
		area_orig += cross_area(orig.vertices[orig.faces[i][0]],
		                        orig.vertices[orig.faces[i][1]],
		                        orig.vertices[orig.faces[i][2]]);
	for (size_t i = 0; i < ps.mesh.faces.size(); i++)
		area_perm += cross_area(ps.mesh.vertices[ps.mesh.faces[i][0]],
		                        ps.mesh.vertices[ps.mesh.faces[i][1]],
		                        ps.mesh.vertices[ps.mesh.faces[i][2]]);
	check(std::fabs(area_orig - area_perm) < 1e-5 * (1.0 + area_orig));

	// The first two midpoints of a refine are assigned by sorted endpoint
	// pair; reversing the input corner order must not change which midpoint
	// numbers appear.  A single triangle suffices to demonstrate that ids are
	// topological rather than positional.
	TriMesh tri;
	tri.vertices = { point(0, 0, 0), point(1, 0, 0), point(0, 1, 0) };
	tri.faces = { TriMesh::Face(0, 1, 2) };
	TriMesh tri_perm;
	tri_perm.vertices = { point(0, 0, 0), point(0, 1, 0), point(1, 0, 0) };
	tri_perm.faces = { TriMesh::Face(0, 2, 1) };
	AdaptiveRefiner t1(tri);
	AdaptiveRefiner t2(tri_perm);
	t1.refine({ 0 });
	t2.refine({ 0 });
	check(t1.children(0).size() == 4 && t2.children(0).size() == 4);
	// Vertex 0 is common; the midpoints of edges 0-1 and 0-2 have distinct
	// ids in both variants and are created in sorted-pair order.
	AdaptiveRefiner::Snapshot ts1 = t1.snapshot();
	AdaptiveRefiner::Snapshot ts2 = t2.snapshot();
	check(ts1.mesh.vertices.size() == 6);
	check(ts2.mesh.vertices.size() == 6);
}

static void test_coarsen_and_ids()
{
	GridMesh g(2, 2);
	TriMesh &orig = g.mesh;
	AdaptiveRefiner r(orig);

	const size_t initial_leaves = r.leaves().size();
	check(initial_leaves == (size_t) orig.faces.size());

	r.refine({ 0, 1 });
	std::vector<NodeId> c0 = r.children(0);
	std::vector<NodeId> c1 = r.children(1);
	check(c0.size() == 4 && c1.size() == 4);
	check(r.leaves().size() > initial_leaves);

	// Simultaneous coarsening of two complete sibling groups.
	check(r.coarsen({ 0, 1 }));
	check(r.leaves().size() == initial_leaves);
	check(r.children(0) == c0);
	check(r.children(1) == c1);

	// Refining coarsened parents reuses the original child numbers.
	r.refine({ 0, 1 });
	check(r.children(0) == c0);
	check(r.children(1) == c1);
	check(r.leaves().size() > initial_leaves);

	// Subdivide one child, leaving an incomplete sibling group for parent 0.
	NodeId child = c0[1];
	r.refine({ child });
	std::vector<NodeId> gc = r.children(child);
	check(gc.size() == 4);
	// Parent 0 can no longer be retracted: c0[1] is not a leaf.
	check(throws_invalid([&]() { r.coarsen({ 0 }); }));

	// Coarsening a never-refined leaf is invalid.
	check(throws_invalid([&]() { r.coarsen({ c0[0] }); }));
	// A deactivated (subdivided, no longer leaf) node cannot be refined.
	check(throws_invalid([&]() { r.refine({ c0[1] }); }));
	// A subdivided parent is not a current leaf.
	check(throws_invalid([&]() { r.refine({ 0 }); }));
	// Unknown ids.
	check(throws_invalid([&]() { r.refine({ (NodeId) 1000000 }); }));
	check(throws_invalid([&]() { r.coarsen({ (NodeId) 1000000 }); }));
	check(throws_invalid([&]() { r.children((NodeId) 1000000); }));

	// Failures leave the state untouched and the object usable.
	Forest before;
	before.build(r, (int) orig.faces.size());
	std::vector<NodeId> lv = r.leaves();
	check(throws_invalid([&]() { r.refine({ 0 }); }));
	check(r.leaves() == lv);
	Forest after;
	after.build(r, (int) orig.faces.size());
	check(after.node_count == before.node_count);
	check(after.level == before.level);

	AdaptiveRefiner fresh(orig);
	check(throws_invalid([&]() { fresh.refine({ 999 }); }));
	check(fresh.leaves().size() == (size_t) orig.faces.size());
	fresh.refine({ 2 });
	check(fresh.leaves().size() > (size_t) orig.faces.size());
}

static void test_coarsen_blocked()
{
	// 2x1 grid, adjacency chain 1 -- 0 -- 3 -- 2 (faces 0 and 3 share edge
	// 1-4, faces 0/1 share the diagonal 0-4, faces 2/3 share 1-5).
	GridMesh g(2, 1);
	TriMesh &orig = g.mesh;
	AdaptiveRefiner r(orig);

	// Drive root 2 to level 2 on its corner touching root 3.  The closure
	// then refines root 3 to level 1, so all four children of root 3 are
	// current leaves while a level-2 leaf sits across their shared edge.
	r.refine({ 2 });
	std::vector<NodeId> k2 = r.children(2);
	check(k2.size() == 4);
	r.refine({ k2[0] });

	Forest f;
	f.build(r, (int) orig.faces.size());
	std::vector<NodeId> leaves = r.leaves();
	int mlev[4] = { 0, 0, 0, 0 };
	for (size_t i = 0; i < leaves.size(); i++)
		mlev[(int) f.root[leaves[i]]] =
			std::max(mlev[(int) f.root[leaves[i]]],
			         f.level[leaves[i]]);
	check(mlev[2] >= 2);
	check(mlev[3] == 1);

	// Retracting root 3 is structurally well-formed, but would leave a
	// level-0 leaf next to a level-2 leaf: rejected as false.
	Forest before;
	before.build(r, (int) orig.faces.size());
	check(!r.coarsen({ 3 }));
	check(r.leaves() == leaves);
	Forest after;
	after.build(r, (int) orig.faces.size());
	check(after.node_count == before.node_count);
	check(after.level == before.level);

	// A blocked request must also shield an otherwise legal parent supplied
	// alongside it: root 1 is untouched, so its four children are leaves and
	// could be coarsened on their own.
	r.refine({ 1 });
	leaves = r.leaves();
	check(r.children(1).size() == 4);
	check(!r.coarsen({ 1, 3 }));
	check(r.leaves() == leaves); // nothing was retracted
	check(r.children(1).size() == 4);

	// After removing the deeper level, root 3 becomes retractable.
	check(r.coarsen({ k2[0] }));
	check(r.coarsen({ 3 }));
	check(r.leaves().size() < leaves.size());
}

static void test_two_round_closure()
{
	// 2x1 grid: adjacency chain 1 -- 0 -- 3 -- 2.
	// Face 0=(00,01,11), 1=(00,11,10), 2=(01,02,12), 3=(01,12,11).
	GridMesh g(2, 1);
	TriMesh &orig = g.mesh;
	AdaptiveRefiner r(orig);

	// Drive root 2 to level 2.  Child 0 of root 2 is (01,m01,m20), i.e. it
	// contains the corner 01 shared with root 3, so its refinement forces a
	// closure across the 2-3 root boundary.
	r.refine({ 2 });
	std::vector<NodeId> k = r.children(2);
	check(k.size() == 4);
	r.refine({ k[0] });
	std::vector<NodeId> kk = r.children(k[0]);
	check(kk.size() == 4);

	Forest f1;
	f1.build(r, (int) orig.faces.size());
	std::vector<NodeId> leaves = r.leaves();
	int maxlev[4] = { 0, 0, 0, 0 };
	for (size_t i = 0; i < leaves.size(); i++) {
		int rt = (int) f1.root[leaves[i]];
		check(rt >= 0 && rt < 4);
		maxlev[rt] = std::max(maxlev[rt], f1.level[leaves[i]]);
	}
	check(maxlev[2] >= 2);
	check(maxlev[3] >= 1); // closure crossed the 2-3 root edge

	// Refine the deepest leaf once more: the gap chain now spans several root
	// edges and must be repaired in successive rounds.
	r.refine({ kk[0] });
	Forest f2;
	f2.build(r, (int) orig.faces.size());
	leaves = r.leaves();
	int mlev[4] = { 0, 0, 0, 0 };
	for (size_t i = 0; i < leaves.size(); i++) {
		int rt = (int) f2.root[leaves[i]];
		mlev[rt] = std::max(mlev[rt], f2.level[leaves[i]]);
	}
	check(mlev[2] >= 3);
	// The far root 1 must not have been refined wholesale: locality holds.
	check(mlev[1] <= 1);
	AdaptiveRefiner::Snapshot s = r.snapshot();
	analyze_snapshot(r, s, orig, false, false);
	check_derived(s);
}

// A slightly zig-zag, consistently oriented open triangle strip.  Face i
// shares an edge with face i+1, so the faces form a chain of root edges.
static TriMesh make_zigzag_strip(int nfaces)
{
	TriMesh m;
	for (int i = 0; i <= nfaces + 1; i++) {
		if (i % 2 == 0)
			m.vertices.push_back(point((float) (i / 2), 0, 0));
		else
			m.vertices.push_back(point((float) (i / 2) + 0.5f, 1, 0));
	}
	for (int f = 0; f < nfaces; f++) {
		if (f % 2 == 0)
			m.faces.push_back(TriMesh::Face(f, f + 1, f + 2));
		else
			m.faces.push_back(TriMesh::Face(f, f + 2, f + 1));
	}
	return m;
}

static void test_multi_round_closure()
{
	// Zig-zag strip of 8 faces.  Repeatedly refining the corner child of root
	// 0 forces the closure to propagate across root boundaries 0-1 and 1-2,
	// which cannot happen in a single round.  The far end must stay coarse.
	TriMesh orig = make_zigzag_strip(8);
	AdaptiveRefiner r(orig);
	check(r.leaves().size() == 8);

	NodeId node = 0;
	int rounds_needed = 0;
	for (int depth = 1; depth <= 3; depth++) {
		r.refine({ node });
		std::vector<NodeId> c = r.children(node);
		check(c.size() == 4);
		node = c[2]; // corner child near the next face along the strip
		if (depth >= 2)
			rounds_needed++;
	}
	check(rounds_needed == 2);

	Forest f;
	f.build(r, (int) orig.faces.size());
	std::vector<NodeId> leaves = r.leaves();
	int mlev[8] = { 0 };
	for (size_t i = 0; i < leaves.size(); i++) {
		int rt = (int) f.root[leaves[i]];
		check(rt >= 0 && rt < 8);
		mlev[rt] = std::max(mlev[rt], f.level[leaves[i]]);
	}
	// The peak is at least level 3, and the gap constraint has propagated
	// through at least two root boundaries (0 -> 1 -> 2).
	check(mlev[0] >= 3);
	check(mlev[1] >= 2);
	check(mlev[2] >= 1);
	// Nothing beyond the local neighbourhood was uniformly refined.
	check(mlev[7] == 0);
	check(mlev[6] == 0);

	// Snapshot is watertight and respects the gap rule everywhere.
	AdaptiveRefiner::Snapshot s = r.snapshot();
	analyze_snapshot(r, s, orig, false, false);
	check_derived(s);
}

static void test_large_mesh_locality()
{
	GridMesh g(24, 24);
	TriMesh &orig = g.mesh;
	int nf = (int) orig.faces.size();
	check(nf == 2 * 24 * 24);
	AdaptiveRefiner r(orig);
	check(r.leaves().size() == (size_t) nf);

	r.refine({ 0 });
	std::vector<NodeId> c0 = r.children(0);
	check(c0.size() == 4);
	r.refine({ c0[0] });

	Forest f;
	f.build(r, nf);
	std::vector<NodeId> leaves = r.leaves();

	// A far root near the opposite corner must still be the original single
	// leaf.  Picking the exact id of the second-to-last face.
	int far = 2 * (23 + 23 * 24) + 1;
	check(far < nf);
	check(f.level[(NodeId) far] == 0);
	check(f.kids.find((NodeId) far) == f.kids.end());
	check(r.children((NodeId) far).empty());
	check(std::find(leaves.begin(), leaves.end(), (NodeId) far) !=
	      leaves.end());
	int farleaves = 0;
	for (size_t i = 0; i < leaves.size(); i++)
		if (f.root[leaves[i]] == (NodeId) far)
			farleaves++;
	check(farleaves == 1);

	// The exported far region consists of exactly one triangle.
	AdaptiveRefiner::Snapshot s = r.snapshot();
	int farfaces = 0;
	for (int i = 0; i < (int) s.mesh.faces.size(); i++)
		if (s.root_face[i] == far)
			farfaces++;
	check(farfaces == 1);

	// Local refinement happened; only a small neighbourhood was touched.
	int refined_roots = 0;
	for (int rt = 0; rt < nf; rt++) {
		int cnt = 0;
		for (size_t i = 0; i < leaves.size(); i++)
			if (f.root[leaves[i]] == (NodeId) rt)
				cnt++;
		if (cnt > 1)
			refined_roots++;
	}
	check(refined_roots >= 2);
	check(refined_roots < nf / 10);
	analyze_snapshot(r, s, orig, false, false);
	check_derived(s);
}

static void test_components_and_coincident()
{
	TriMesh orig = make_two_tetra();
	AdaptiveRefiner r(orig);
	r.refine({ 0 });
	Forest f;
	f.build(r, 8);
	std::vector<NodeId> leaves = r.leaves();
	for (int rt = 4; rt < 8; rt++) {
		check(f.kids.find((NodeId) rt) == f.kids.end());
		int cnt = 0;
		for (size_t i = 0; i < leaves.size(); i++)
			if (f.root[leaves[i]] == (NodeId) rt)
				cnt++;
		check(cnt == 1);
	}
	AdaptiveRefiner::Snapshot s = r.snapshot();
	analyze_snapshot(r, s, orig, false, false);
	check_derived(s);

	// Coincident coordinates with distinct indices must never be welded.
	TriMesh coin = make_coincident_pair();
	AdaptiveRefiner rc(coin);
	check(rc.leaves().size() == 2);
	check(rc.children(0).empty() && rc.children(1).empty());
	rc.refine({ 0, 1 });
	AdaptiveRefiner::Snapshot cs = rc.snapshot();
	check(cs.mesh.vertices.size() == 12);
	check(cs.mesh.faces.size() == 8);
	double area = 0;
	for (size_t i = 0; i < cs.mesh.faces.size(); i++)
		area += cross_area(cs.mesh.vertices[cs.mesh.faces[i][0]],
		                   cs.mesh.vertices[cs.mesh.faces[i][1]],
		                   cs.mesh.vertices[cs.mesh.faces[i][2]]);
	check(approx(area, 1.0, 1e-5)); // two coincident unit-right triangles
	check_derived(cs);
}

static void test_level_limit_and_recovery()
{
	TriMesh tri;
	tri.vertices = { point(0, 0, 0), point(1, 0, 0), point(0, 1, 0) };
	tri.faces = { TriMesh::Face(0, 1, 2) };
	AdaptiveRefiner r(tri);
	r.refine({ 0 });
	NodeId deep = r.children(0)[0];
	NodeId deep_parent = 0;
	for (int level = 1; level < AdaptiveRefiner::MAX_LEVEL; level++) {
		deep_parent = deep;
		r.refine({ deep });
		std::vector<NodeId> c = r.children(deep);
		check(c.size() == 4);
		deep = c[0];
	}

	std::vector<NodeId> lv = r.leaves();
	Forest before;
	before.build(r, 1);
	// Level-12 leaf; one more refinement crosses the limit.
	check(throws_runtime([&]() { r.refine({ deep }); }));
	check(r.leaves() == lv);
	Forest after;
	after.build(r, 1);
	check(after.node_count == before.node_count);
	check(after.level == before.level);

	// Still usable afterwards.
	AdaptiveRefiner::Snapshot s = r.snapshot();
	check(s.mesh.faces.size() > 0);
	check(r.coarsen({ deep_parent }));
}

static void test_snapshot_immutability()
{
	GridMesh g(2, 2);
	TriMesh &orig = g.mesh;
	AdaptiveRefiner r(orig);
	r.refine({ 0 });
	AdaptiveRefiner::Snapshot snap1 = r.snapshot();
	size_t v1 = snap1.mesh.vertices.size();
	size_t f1 = snap1.mesh.faces.size();
	std::vector<point> pos1 = snap1.mesh.vertices;
	std::vector<TriMesh::Face> faces1 = snap1.mesh.faces;
	std::vector<NodeId> leaves1 = r.leaves();

	AdaptiveRefiner::Snapshot snap2 = r.snapshot();
	check(snap2.mesh.vertices.size() == v1);
	check(snap2.mesh.faces.size() == f1);
	check(r.leaves() == leaves1);
	// Validate the second snapshot while the hierarchy still matches it.
	analyze_snapshot(r, snap2, orig, false, false);

	r.refine({ r.children(0)[1] });
	check(snap1.mesh.vertices.size() == v1);
	check(snap1.mesh.faces.size() == f1);
	check(snap1.mesh.vertices == pos1);
	check(snap1.mesh.faces == faces1);

	// Destroying the source after construction leaves the refiner intact.
	TriMesh copy = orig;
	AdaptiveRefiner made(copy);
	copy.vertices.clear();
	copy.faces.clear();
	copy.colors.clear();
	copy.confidences.clear();
	check(made.leaves().size() == (size_t) orig.faces.size());
	made.refine({ 0 });
	AdaptiveRefiner::Snapshot ms = made.snapshot();
	check(ms.mesh.faces.size() >= 4);
	double area = 0, oarea = 0;
	for (size_t i = 0; i < ms.mesh.faces.size(); i++)
		area += cross_area(ms.mesh.vertices[ms.mesh.faces[i][0]],
		                   ms.mesh.vertices[ms.mesh.faces[i][1]],
		                   ms.mesh.vertices[ms.mesh.faces[i][2]]);
	for (size_t i = 0; i < orig.faces.size(); i++)
		oarea += cross_area(orig.vertices[orig.faces[i][0]],
		                    orig.vertices[orig.faces[i][1]],
		                    orig.vertices[orig.faces[i][2]]);
	check(std::fabs(area - oarea) < 1e-5 * (1.0 + oarea));
}

static void test_persistent_numbering()
{
	// A single triangle.  Refining it must create the three midpoints in the
	// order of the sorted persistent endpoint pairs (0,1), (0,2), (1,2),
	// giving persistent vertex ids 3, 4, 5.  The snapshot emits only used
	// vertices, sorted by persistent id, so its vertex order is exactly
	// 0,1,2,mid01,mid02,mid12.
	TriMesh tri;
	tri.vertices = { point(0, 0, 0), point(1, 0, 0), point(0, 1, 0) };
	tri.faces = { TriMesh::Face(0, 1, 2) };
	AdaptiveRefiner r(tri);
	r.refine({ 0 });
	AdaptiveRefiner::Snapshot s = r.snapshot();
	check(s.mesh.vertices.size() == 6);
	check(dist2(s.mesh.vertices[0], point(0, 0, 0)) < 1e-9);
	check(dist2(s.mesh.vertices[1], point(1, 0, 0)) < 1e-9);
	check(dist2(s.mesh.vertices[2], point(0, 1, 0)) < 1e-9);
	check(dist2(s.mesh.vertices[3], point(0.5f, 0, 0)) < 1e-9);
	check(dist2(s.mesh.vertices[4], point(0, 0.5f, 0)) < 1e-9);
	check(dist2(s.mesh.vertices[5], point(0.5f, 0.5f, 0)) < 1e-9);
	// The four children are numbered 1..4 in the fixed template order:
	// (v0,m01,m20), (v1,m12,m01), (v2,m20,m12), (m01,m12,m20).
	std::vector<NodeId> c = r.children(0);
	check(c.size() == 4);
	check(c[0] == 1 && c[1] == 2 && c[2] == 3 && c[3] == 4);
	// Each child is a plain leaf with no split edges, so it exports exactly
	// one face; the faces are emitted in leaf-id order.
	check(s.mesh.faces.size() == 4);
	for (int leaf = 0; leaf < 4; leaf++) {
		check(s.leaf_id[leaf] == (NodeId) (leaf + 1));
		check(s.root_face[leaf] == 0);
	}
	// Template winding: child 0 is (v0,m01,m20) = (0,3,4).
	check(s.mesh.faces[0] == TriMesh::Face(0, 3, 4));
	check(s.mesh.faces[1] == TriMesh::Face(1, 5, 3));
	check(s.mesh.faces[2] == TriMesh::Face(2, 4, 5));
	check(s.mesh.faces[3] == TriMesh::Face(3, 5, 4));

	// Refining child 0 creates only the midpoints it needs.  Child 0 is
	// (v0,m01,m20) = persistent (0,3,4).  Its edges sorted: (0,3), (0,4),
	// (3,4), so persistent ids 6,7,8 belong to their midpoints.
	r.refine({ c[0] });
	AdaptiveRefiner::Snapshot s2 = r.snapshot();
	// 3 originals + 3 outer midpoints + 3 inner midpoints, all used.
	check(s2.mesh.vertices.size() == 9);
	check(dist2(s2.mesh.vertices[6], point(0.25f, 0, 0)) < 1e-9);   // mid(0,3)
	check(dist2(s2.mesh.vertices[7], point(0, 0.25f, 0)) < 1e-9);   // mid(0,4)
	check(dist2(s2.mesh.vertices[8], point(0.25f, 0.25f, 0)) < 1e-9); // mid(3,4)

	// Coarsening child 0 retracts to a level-1 leaf.  The inner midpoints are
	// no longer used, so they disappear from the snapshot; the outer ones
	// remain.
	check(r.coarsen({ c[0] }));
	AdaptiveRefiner::Snapshot s3 = r.snapshot();
	check(s3.mesh.vertices.size() == 6);
	// Re-refining child 0 reuses its old child ids and the same midpoints.
	r.refine({ c[0] });
	AdaptiveRefiner::Snapshot s4 = r.snapshot();
	check(s4.mesh.vertices.size() == 9);
	for (size_t i = 0; i < s4.mesh.vertices.size(); i++)
		check(dist2(s4.mesh.vertices[i], s2.mesh.vertices[i]) < 1e-9);

	// Unused persistent vertices are omitted entirely: coarsen back to the
	// root and the snapshot has only the original three corners.
	check(r.coarsen({ c[0] }));
	check(r.coarsen({ 0 }));
	AdaptiveRefiner::Snapshot s5 = r.snapshot();
	check(s5.mesh.vertices.size() == 3);
	check(s5.mesh.faces.size() == 1);
}

static void test_deep_refinement()
{
	// A 8x8 grid refined six levels deep around the centre.  This exercises
	// nested dyadic edge intervals (depth > 2), which a naive two-level
	// interval bookkeeping would mis-canonicalise.
	GridMesh g(8, 8);
	TriMesh &orig = g.mesh;
	AdaptiveRefiner r(orig);
	int centre = 2 * (4 + 4 * 8);
	NodeId n = (NodeId) centre;
	for (int d = 0; d < 6; d++) {
		r.refine({ n });
		std::vector<NodeId> c = r.children(n);
		check(c.size() == 4);
		n = c[0];
	}
	Forest f;
	f.build(r, (int) orig.faces.size());
	int deep_level = f.level[n];
	check(deep_level == 6);

	AdaptiveRefiner::Snapshot s = r.snapshot();
	SnapStats st = analyze_snapshot(r, s, orig, false, false);
	check(st.leaves >= 16);
	// Area, manifoldness, boundary and the gap rule are all checked inside
	// analyze_snapshot, including deep nested T-junctions.
	check_derived(s);

	// The whole sheet's area is preserved exactly.
	double area = 0;
	for (size_t i = 0; i < s.mesh.faces.size(); i++)
		area += cross_area(s.mesh.vertices[s.mesh.faces[i][0]],
		                   s.mesh.vertices[s.mesh.faces[i][1]],
		                   s.mesh.vertices[s.mesh.faces[i][2]]);
	check(std::fabs(area - 64.0) < 1e-4);

	// A far root is still one leaf.
	Forest f2;
	f2.build(r, (int) orig.faces.size());
	int far = 2 * (0 + 7 * 8);
	check(f2.level[(NodeId) far] == 0);
	check(f2.kids.find((NodeId) far) == f2.kids.end());
}

// Enumerate every subset of roots on a small mesh and refine it.  This
// exercises all green-patch masks and winding combinations, and checks that
// each exported mesh is manifold, orientation-consistent and area-true.
static void test_exhaustive_subsets()
{
	{
		TriMesh orig = make_tetra();
		int nf = (int) orig.faces.size();
		for (int mask = 0; mask < (1 << nf); mask++) {
			AdaptiveRefiner r(orig);
			std::vector<NodeId> sel;
			for (int i = 0; i < nf; i++)
				if (mask & (1 << i))
					sel.push_back((NodeId) i);
			if (!sel.empty())
				r.refine(sel);
			AdaptiveRefiner::Snapshot s = r.snapshot();
			analyze_snapshot(r, s, orig, false, false);
			check_derived(s);
			check(s.mesh.faces.size() >= 4);
		}
	}
	{
		// Open 2x2 grid with 8 roots: all subsets -> every green configuration
		// on a sheet with boundary.
		GridMesh g(2, 2);
		TriMesh &orig = g.mesh;
		int nf = (int) orig.faces.size();
		for (int mask = 0; mask < (1 << nf); mask++) {
			AdaptiveRefiner r(orig);
			std::vector<NodeId> sel;
			for (int i = 0; i < nf; i++)
				if (mask & (1 << i))
					sel.push_back((NodeId) i);
			if (!sel.empty())
				r.refine(sel);
			AdaptiveRefiner::Snapshot s = r.snapshot();
			analyze_snapshot(r, s, orig, false, false);
			check_derived(s);
		}
	}
}

static void test_attributes_absent()
{
	GridMesh g(1, 1);
	AdaptiveRefiner r(g.mesh);
	r.refine({ 0 });
	AdaptiveRefiner::Snapshot s = r.snapshot();
	check(s.mesh.colors.empty());
	check(s.mesh.confidences.empty());
	check(s.mesh.faces.size() >= 4);
	check_derived(s);
}

int main()
{
	std::puts("adaptive refinement: input validation");
	test_input_validation();
	std::puts("adaptive refinement: tetrahedron / green classes");
	test_tetrahedron_classes();
	std::puts("adaptive refinement: folded boundary class 1");
	test_fold_boundary_class1();
	std::puts("adaptive refinement: class 2 diagonal");
	test_class2_diagonal();
	std::puts("adaptive refinement: order/permutation invariance");
	test_order_and_permutation();
	std::puts("adaptive refinement: coarsening and id reuse");
	test_coarsen_and_ids();
	std::puts("adaptive refinement: blocked coarsening");
	test_coarsen_blocked();
	std::puts("adaptive refinement: two-round closure");
	test_two_round_closure();
	std::puts("adaptive refinement: multi-round closure cascade");
	test_multi_round_closure();
	std::puts("adaptive refinement: large mesh locality");
	test_large_mesh_locality();
	std::puts("adaptive refinement: components / coincident vertices");
	test_components_and_coincident();
	std::puts("adaptive refinement: level limit and recovery");
	test_level_limit_and_recovery();
	std::puts("adaptive refinement: snapshot immutability");
	test_snapshot_immutability();
	std::puts("adaptive refinement: deep nested refinement");
	test_deep_refinement();
	std::puts("adaptive refinement: persistent numbering");
	test_persistent_numbering();
	std::puts("adaptive refinement: exhaustive root subsets");
	test_exhaustive_subsets();
	std::puts("adaptive refinement: absent attributes");
	test_attributes_absent();

	std::puts("ADAPTIVE_REFINEMENT_OK");
	return 0;
}
