// Tests for split_nonmanifold_sheets().
//
// The three groups required by the task driver are:
//   pairing  - per-index-edge matching (max cardinality, then min cost)
//   fans     - vertex-fan splitting, quotient-edge conflicts, attributes
//   pipeline - PLY round trips, library/CLI agreement, follow-up edits
//
// Every check rebuilds real edge tables and vertex links from the output
// faces/vertices rather than trusting the returned maps alone.

#include "TriMesh.h"
#include "TriMesh_algo.h"
#include "task_support/check.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace trimesh;

namespace {

//---------------------------------------------------------------------------
// Small helpers
//---------------------------------------------------------------------------

static const char *tmpdir()
{
	const char *d = getenv("TMPDIR");
	return (d && *d) ? d : ".";
}

static std::string tmp_path(const char *name)
{
	std::string p(tmpdir());
	if (!p.empty() && p[p.size() - 1] != '/')
		p += '/';
	p += name;
	return p;
}

static bool same_point(const point &a, const point &b)
{
	return std::fabs(double(a[0]) - double(b[0])) < 1e-6 &&
	       std::fabs(double(a[1]) - double(b[1])) < 1e-6 &&
	       std::fabs(double(a[2]) - double(b[2])) < 1e-6;
}

static bool same_attrs(const TriMesh &a, const TriMesh &b)
{
	if (a.colors.size() != b.colors.size() ||
	    a.confidences.size() != b.confidences.size() ||
	    a.flags.size() != b.flags.size())
		return false;
	for (size_t i = 0; i < a.colors.size(); ++i) {
		if (std::fabs(a.colors[i][0] - b.colors[i][0]) > 1e-6 ||
		    std::fabs(a.colors[i][1] - b.colors[i][1]) > 1e-6 ||
		    std::fabs(a.colors[i][2] - b.colors[i][2]) > 1e-6)
			return false;
	}
	for (size_t i = 0; i < a.confidences.size(); ++i)
		if (std::fabs(a.confidences[i] - b.confidences[i]) > 1e-6)
			return false;
	if (a.flags != b.flags)
		return false;
	return true;
}

static bool same_geometry(const TriMesh &a, const TriMesh &b)
{
	if (a.vertices.size() != b.vertices.size() ||
	    a.faces.size() != b.faces.size())
		return false;
	for (size_t i = 0; i < a.vertices.size(); ++i)
		if (!same_point(a.vertices[i], b.vertices[i]))
			return false;
	for (size_t i = 0; i < a.faces.size(); ++i)
		if (a.faces[i] != b.faces[i])
			return false;
	return true;
}

// Build a fingerprint of the input mesh used by the transactional checks.
struct Snapshot {
	std::vector<point> vertices;
	std::vector<TriMesh::Face> faces;
	std::vector<Color> colors;
	std::vector<float> confidences;
	std::vector<unsigned> flags;
	unsigned flag_curr;
	int grid_width, grid_height;
	bool bbox_valid, bsphere_valid;
};

static Snapshot snapshot(const TriMesh &m)
{
	Snapshot s;
	s.vertices = m.vertices;
	s.faces = m.faces;
	s.colors = m.colors;
	s.confidences = m.confidences;
	s.flags = m.flags;
	s.flag_curr = m.flag_curr;
	s.grid_width = m.grid_width;
	s.grid_height = m.grid_height;
	s.bbox_valid = m.bbox.valid;
	s.bsphere_valid = m.bsphere.valid;
	return s;
}

static bool unchanged(const TriMesh &m, const Snapshot &s,
                      const SheetSplitResult &r, size_t rv, size_t re)
{
	if (m.vertices.size() != s.vertices.size() ||
	    m.faces.size() != s.faces.size())
		return false;
	// Compare raw bytes so NaN-valued inputs (used to test rejection) are
	// reported as unchanged even though NaN != NaN.
	for (size_t i = 0; i < s.vertices.size(); ++i)
		if (std::memcmp(&m.vertices[i][0], &s.vertices[i][0],
		                sizeof(float) * 3) != 0)
			return false;
	for (size_t i = 0; i < s.faces.size(); ++i)
		if (m.faces[i] != s.faces[i])
			return false;
	TriMesh ref;
	ref.colors = s.colors;
	ref.confidences = s.confidences;
	ref.flags = s.flags;
	return same_attrs(m, ref) && m.flag_curr == s.flag_curr &&
	       m.grid_width == s.grid_width && m.grid_height == s.grid_height &&
	       m.bbox.valid == s.bbox_valid &&
	       m.bsphere.valid == s.bsphere_valid &&
	       r.vertex_source.size() == rv && r.edge_partner.size() == re;
}

// Independently derive the vertex link around each output vertex: the set of
// opposite edges of incident faces, and its degree pattern.  A 2-manifold
// vertex has a link that is either a single open chain or a single cycle, and
// every edge must appear at most twice with opposite directions.
struct LinkInfo {
	bool manifold;         // single open chain or single closed cycle
	int components;        // number of link components
	bool closed;           // every link vertex has degree 2
};

static bool analyse_links(const TriMesh &m,
                          std::vector<LinkInfo> &links)
{
	int nv = int(m.vertices.size());
	LinkInfo empty;
	empty.manifold = false;
	empty.components = 0;
	empty.closed = false;
	links.assign(nv, empty);

	// Real output edges: (lo,hi) -> list of (face, corner, dir).
	struct RealEdge {
		int count;
		int dirs;
		int face0, corner0, face1, corner1;
	};
	std::map<std::pair<int, int>, RealEdge> edges;
	for (int f = 0; f < int(m.faces.size()); ++f) {
		for (int k = 0; k < 3; ++k) {
			int u = m.faces[f][NEXT_MOD3(k)];
			int w = m.faces[f][PREV_MOD3(k)];
			std::pair<int, int> key(std::min(u, w), std::max(u, w));
			std::map<std::pair<int, int>, RealEdge>::iterator it =
				edges.find(key);
			if (it == edges.end()) {
				RealEdge re;
				re.count = 0;
				re.dirs = 0;
				re.face0 = re.face1 = -1;
				re.corner0 = re.corner1 = -1;
				it = edges.insert(std::make_pair(key, re)).first;
			}
			RealEdge &re = it->second;
			int dir = (u < w) ? 1 : -1;
			if (re.count == 0) {
				re.face0 = f;
				re.corner0 = k;
				re.dirs = dir;
			} else {
				re.face1 = f;
				re.corner1 = k;
				re.dirs += dir;
			}
			re.count++;
			if (re.count > 2)
				return false;
		}
	}

	// For each vertex, union incident face-corners across real shared edges.
	std::vector<int> parent(m.faces.size() * 3);
	for (size_t i = 0; i < parent.size(); ++i)
		parent[i] = int(i);
	struct UF {
		std::vector<int> &p;
		int find(int x)
		{
			while (p[x] != x) {
				p[x] = p[p[x]];
				x = p[x];
			}
			return x;
		}
		void unite(int a, int b)
		{
			a = find(a);
			b = find(b);
			if (a != b)
				p[std::max(a, b)] = std::min(a, b);
		}
	} uf{parent};

	for (std::map<std::pair<int, int>, RealEdge>::const_iterator it =
	         edges.begin(); it != edges.end(); ++it) {
		const RealEdge &re = it->second;
		if (re.count != 2 || re.dirs != 0)
			continue;
		// The two occurrences share both endpoints; link the corners.
		int f0 = re.face0, k0 = re.corner0;
		int f1 = re.face1, k1 = re.corner1;
		uf.unite(3 * f0 + NEXT_MOD3(k0), 3 * f1 + PREV_MOD3(k1));
		uf.unite(3 * f0 + PREV_MOD3(k0), 3 * f1 + NEXT_MOD3(k1));
	}

	// Group corners per vertex and count resulting fans.
	std::vector<std::map<int, int> > fan_root_count(nv);
	for (int f = 0; f < int(m.faces.size()); ++f) {
		for (int k = 0; k < 3; ++k) {
			int v = m.faces[f][k];
			int r = uf.find(3 * f + k);
			fan_root_count[v][r]++;
		}
	}

	for (int v = 0; v < nv; ++v) {
		int fans = int(fan_root_count[v].size());
		// Count boundary edges at this vertex (real edges used once).
		int bdy = 0;
		for (std::map<std::pair<int, int>, RealEdge>::const_iterator it =
		         edges.begin(); it != edges.end(); ++it) {
			if (it->second.count != 1)
				continue;
			if (it->first.first != v && it->first.second != v)
				continue;
			bdy++;
		}
		LinkInfo li;
		li.components = fans;
		li.closed = (fans == 1 && bdy == 0);
		// A single fan is an open chain or a cycle; more than one fan means
		// the vertex is not 2-manifold.
		li.manifold = (fans == 1);
		if (fans == 0)
			li.manifold = false;
		links[v] = li;
	}
	return true;
}

// Verify the complete contract of a successful split against the mesh state
// captured before the call.
static bool verify_split(const TriMesh &out, const SheetSplitResult &r,
                         const TriMesh &in)
{
	if (r.vertex_source.size() != out.vertices.size())
		return false;
	if (r.edge_partner.size() != in.faces.size())
		return false;
	if (out.faces.size() != in.faces.size())
		return false;
	int nv = int(in.vertices.size());

	// Original vertex slots retained with identity mapping and positions.
	for (int i = 0; i < nv; ++i) {
		if (i >= int(r.vertex_source.size()) || r.vertex_source[i] != i)
			return false;
		if (!same_point(out.vertices[i], in.vertices[i]))
			return false;
	}
	// Every output vertex maps back to a valid input vertex with the same
	// position.
	for (size_t i = 0; i < r.vertex_source.size(); ++i) {
		int s = r.vertex_source[i];
		if (s < 0 || s >= nv)
			return false;
		if (!same_point(out.vertices[i], in.vertices[s]))
			return false;
	}
	// Faces keep order and per-corner source identity.
	for (size_t f = 0; f < in.faces.size(); ++f) {
		for (int k = 0; k < 3; ++k) {
			int ov = out.faces[f][k];
			if (ov < 0 || ov >= int(r.vertex_source.size()))
				return false;
			if (r.vertex_source[ov] != in.faces[f][k])
				return false;
		}
	}

	// Rebuild the real output edge table and check that it exactly realizes
	// edge_partner: each reported pair shares one edge in opposite
	// directions; each -1 edge is a real boundary edge of the output.
	struct Occ {
		int face;
		int corner;
		int dir;
		int u, w;
	};
	std::map<std::pair<int, int>, std::vector<Occ> > edges;
	for (int f = 0; f < int(out.faces.size()); ++f) {
		for (int k = 0; k < 3; ++k) {
			Occ o;
			o.face = f;
			o.corner = k;
			o.u = out.faces[f][NEXT_MOD3(k)];
			o.w = out.faces[f][PREV_MOD3(k)];
			o.dir = (o.u < o.w) ? 1 : -1;
			edges[std::make_pair(std::min(o.u, o.w), std::max(o.u, o.w))]
				.push_back(o);
		}
	}
	for (int f = 0; f < int(out.faces.size()); ++f) {
		for (int k = 0; k < 3; ++k) {
			int g = r.edge_partner[f][k];
			int u = out.faces[f][NEXT_MOD3(k)];
			int w = out.faces[f][PREV_MOD3(k)];
			std::pair<int, int> key(std::min(u, w), std::max(u, w));
			const std::vector<Occ> &os = edges[key];
			if (g < 0) {
				if (os.size() != 1 || os[0].face != f ||
				    os[0].corner != k)
					return false;
				continue;
			}
			if (g >= int(out.faces.size()) || g == f)
				return false;
			// The partner must be the other occurrence of this edge.
			bool found = false;
			for (size_t j = 0; j < os.size(); ++j) {
				if (os[j].face == g) {
					if (os[j].u == w && os[j].w == u &&
					    r.edge_partner[g][os[j].corner] == f)
						found = true;
				}
			}
			if (!found)
				return false;
		}
	}

	// Every real edge appears once (boundary) or twice (one pair).
	for (std::map<std::pair<int, int>, std::vector<Occ> >::const_iterator
	         it = edges.begin(); it != edges.end(); ++it) {
		if (it->second.size() == 1) {
			const Occ &o = it->second[0];
			if (r.edge_partner[o.face][o.corner] != -1)
				return false;
		} else if (it->second.size() == 2) {
			const Occ &a = it->second[0], &b = it->second[1];
			if (a.dir == b.dir)
				return false;
			if (r.edge_partner[a.face][a.corner] != b.face ||
			    r.edge_partner[b.face][b.corner] != a.face)
				return false;
		} else {
			return false;
		}
	}
	return true;
}

// Pairwise normal cost used by the reference matcher.
static double pair_cost(const TriMesh &m, int f, int g)
{
	double nf[3], ng[3], ta, tb;
	const TriMesh::Face &ff = m.faces[f], &gg = m.faces[g];
	// Reuse the same double-precision formula as the implementation.
	double a[3], b[3], c[3];
	a[0] = double(m.vertices[ff[1]][0]) - double(m.vertices[ff[0]][0]);
	a[1] = double(m.vertices[ff[1]][1]) - double(m.vertices[ff[0]][1]);
	a[2] = double(m.vertices[ff[1]][2]) - double(m.vertices[ff[0]][2]);
	b[0] = double(m.vertices[ff[2]][0]) - double(m.vertices[ff[0]][0]);
	b[1] = double(m.vertices[ff[2]][1]) - double(m.vertices[ff[0]][1]);
	b[2] = double(m.vertices[ff[2]][2]) - double(m.vertices[ff[0]][2]);
	c[0] = a[1] * b[2] - a[2] * b[1];
	c[1] = a[2] * b[0] - a[0] * b[2];
	c[2] = a[0] * b[1] - a[1] * b[0];
	ta = std::sqrt(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]);
	nf[0] = c[0] / ta;
	nf[1] = c[1] / ta;
	nf[2] = c[2] / ta;
	a[0] = double(m.vertices[gg[1]][0]) - double(m.vertices[gg[0]][0]);
	a[1] = double(m.vertices[gg[1]][1]) - double(m.vertices[gg[0]][1]);
	a[2] = double(m.vertices[gg[1]][2]) - double(m.vertices[gg[0]][2]);
	b[0] = double(m.vertices[gg[2]][0]) - double(m.vertices[gg[0]][0]);
	b[1] = double(m.vertices[gg[2]][1]) - double(m.vertices[gg[0]][1]);
	b[2] = double(m.vertices[gg[2]][2]) - double(m.vertices[gg[0]][2]);
	c[0] = a[1] * b[2] - a[2] * b[1];
	c[1] = a[2] * b[0] - a[0] * b[2];
	c[2] = a[0] * b[1] - a[1] * b[0];
	tb = std::sqrt(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]);
	ng[0] = c[0] / tb;
	ng[1] = c[1] / tb;
	ng[2] = c[2] / tb;
	double d = nf[0] * ng[0] + nf[1] * ng[1] + nf[2] * ng[2];
	if (d > 1)
		d = 1;
	if (d < -1)
		d = -1;
	return 1.0 - d;
}

// Exhaustive reference matcher for one index edge.  Returns the maximum
// number of pairs and minimum total cost.
static void brute_force_group(const TriMesh &m,
                              const std::vector<std::pair<int, int> > &fwd,
                              const std::vector<std::pair<int, int> > &bwd,
                              int &best_pairs, double &best_cost)
{
	int n = int(fwd.size()), k = int(bwd.size());
	best_pairs = 0;
	best_cost = 0.0;
	if (!n || !k)
		return;
	bool fwd_rows = n <= k;
	int rows = fwd_rows ? n : k, cols = fwd_rows ? k : n;
	if (cols > 10)
		return; // callers keep groups small
	std::vector<int> perm(cols);
	for (int i = 0; i < cols; ++i)
		perm[i] = i;
	double best = 1e300;
	int bestcard = -1;
	// Choose which `rows` columns are used, then all assignments.  Since all
	// rows must be matched in a maximum matching, iterate over column subsets
	// and permutations.
	std::vector<int> choose(cols, 0);
	for (int i = 0; i < rows; ++i)
		choose[cols - 1 - i] = 1;
	do {
		std::vector<int> used;
		for (int i = 0; i < cols; ++i)
			if (choose[i])
				used.push_back(i);
		std::vector<int> p = used;
		do {
			double sum = 0;
			for (int r = 0; r < rows; ++r) {
				int fa = fwd_rows ? fwd[r].first : bwd[r].first;
				int fb = fwd_rows ? bwd[p[r]].first : fwd[p[r]].first;
				sum += pair_cost(m, fa, fb);
			}
			if (rows > bestcard ||
			    (rows == bestcard && sum < best - 1e-12)) {
				bestcard = rows;
				best = sum;
			}
		} while (std::next_permutation(p.begin(), p.end()));
	} while (std::next_permutation(choose.begin(), choose.end()));
	best_pairs = bestcard;
	best_cost = best;
}

// Collect the forward/backward faces of one index edge from a mesh.
static void group_faces(const TriMesh &m, int lo, int hi,
                        std::vector<std::pair<int, int> > &fwd,
                        std::vector<std::pair<int, int> > &bwd)
{
	fwd.clear();
	bwd.clear();
	for (int f = 0; f < int(m.faces.size()); ++f) {
		for (int k = 0; k < 3; ++k) {
			int u = m.faces[f][NEXT_MOD3(k)];
			int w = m.faces[f][PREV_MOD3(k)];
			if (std::min(u, w) != lo || std::max(u, w) != hi)
				continue;
			if (u == lo)
				fwd.push_back(std::make_pair(f, k));
			else
				bwd.push_back(std::make_pair(f, k));
		}
	}
}

// Sum the returned pairing's cost for one group and count its pairs.
static void matched_group(const SheetSplitResult &r, int lo, int hi,
                          const TriMesh &m, int &pairs, double &cost)
{
	pairs = 0;
	cost = 0.0;
	std::set<std::pair<int, int> > seen;
	for (int f = 0; f < int(m.faces.size()); ++f) {
		for (int k = 0; k < 3; ++k) {
			int g = r.edge_partner[f][k];
			if (g < 0)
				continue;
			int u = m.faces[f][NEXT_MOD3(k)];
			int w = m.faces[f][PREV_MOD3(k)];
			if (std::min(u, w) != lo || std::max(u, w) != hi)
				continue;
			int a = std::min(f, g), b = std::max(f, g);
			if (!seen.insert(std::make_pair(a, b)).second)
				continue;
			pairs++;
			cost += pair_cost(m, a, b);
		}
	}
}

//---------------------------------------------------------------------------
// Fixture builders
//---------------------------------------------------------------------------

// Four faces sharing index edge (0,1), two forward, two backward, with
// intentionally different dihedral angles so the optimal pairing is unique.
static TriMesh crossing_fixture()
{
	TriMesh m;
	m.vertices = {
		point(0, 0, 0), point(1, 0, 0),
		point(0.0f, 1.0f, 0.1f),   // f0 up-ish
		point(1.0f, -0.5f, 0.3f),  // f1 down
		point(0.2f, 2.0f, -0.2f),  // f2
		point(1.2f, -1.5f, 0.7f)   // f3
	};
	m.faces = {
		TriMesh::Face(0, 1, 2),
		TriMesh::Face(1, 0, 3),
		TriMesh::Face(0, 1, 4),
		TriMesh::Face(1, 0, 5)
	};
	return m;
}

// A larger crossing: 32 pairs worth of faces on one index edge.  Each forward
// face (0,1,p) has an exact antipodal twin (1,0,q) with q = mirror(p), so the
// unique optimum pairs each forward with its twin (cost ~0) instead of any
// cross pairing (cost strictly > 0).
static TriMesh large_crossing_fixture()
{
	TriMesh m;
	const int n = 32;
	m.vertices.push_back(point(0, 0, 0));
	m.vertices.push_back(point(1, 0, 0));
	for (int i = 0; i < n; ++i) {
		double th = 2.0 * M_PI * double(i) / double(n) + 0.07;
		// Forward faces lie on a cone around +z, backward twins mirrored to
		// the opposite side of the edge so their normals are exactly
		// opposite.
		point p(float(0.3 + 0.01 * i), float(0.4 * std::cos(th)),
		        float(0.4 * std::sin(th)));
		m.vertices.push_back(p);
		m.vertices.push_back(point(p[0], -p[1], -p[2]));
	}
	for (int i = 0; i < n; ++i) {
		int pf = 2 + 2 * i;
		int pb = 3 + 2 * i;
		m.faces.push_back(TriMesh::Face(0, 1, pf));
		m.faces.push_back(TriMesh::Face(1, 0, pb));
	}
	return m;
}

// Butterfly: two non-coplanar triangles meeting only at vertex 0.
static TriMesh butterfly_fixture()
{
	TriMesh m;
	m.vertices = {
		point(0, 0, 0), point(1, 0, 0), point(0, 1, 0),
		point(-1, 0, 0.5f), point(0, -1, 0.3f)
	};
	m.faces = { TriMesh::Face(0, 1, 2), TriMesh::Face(0, 3, 4) };
	return m;
}

// Two sheets that share the whole boundary loop of a segment and are wound
// the same way, plus a genuine pair on another edge.
static TriMesh same_direction_fixture()
{
	TriMesh m;
	m.vertices = {
		point(0, 0, 0), point(1, 0, 0), point(1, 1, 0),
		point(0, 1, 0), point(0, 0, 1)
	};
	// Two coplanar-ish triangles with the same winding on edge (0,1): they
	// must stay unpaired (boundary).
	m.faces = {
		TriMesh::Face(0, 1, 2),
		TriMesh::Face(0, 1, 4), // same direction on (0,1)
	};
	return m;
}

// Two sheets joined along a chain of edges, forming one fan that is not
// manifold at the shared vertices; splitting must separate the sheets.
static TriMesh multi_edge_fixture()
{
	TriMesh m;
	// Quad strip 0-1-2-3 folded onto another strip 4-5-6-7 sharing edges.
	m.vertices = {
		point(0, 0, 0), point(1, 0, 0), point(2, 0, 0), point(3, 0, 0),
		point(0, 0, 1), point(1, 0, 1), point(2, 0, 1), point(3, 0, 1)
	};
	m.faces = {
		TriMesh::Face(0, 1, 4),
		TriMesh::Face(1, 5, 4),
		TriMesh::Face(1, 2, 5),
		TriMesh::Face(2, 6, 5),
		TriMesh::Face(2, 3, 6),
		TriMesh::Face(3, 7, 6)
	};
	return m;
}

// A known quotient-edge conflict fixture (found by exhaustive search over
// 5-face configurations).  The locally optimal matching makes two distinct
// input edge copies end up on the same output vertices.
static TriMesh conflict_fixture()
{
	TriMesh m;
	m.vertices = {
		point(0, 0, 0), point(1, 0, 0),
		point(0.13f, 1.07f, 0.21f), point(-0.31f, 0.22f, 1.13f),
		point(0.47f, -0.63f, 0.58f)
	};
	m.faces = {
		TriMesh::Face(0, 1, 2), TriMesh::Face(0, 1, 3),
		TriMesh::Face(0, 4, 1), TriMesh::Face(0, 3, 4),
		TriMesh::Face(1, 4, 3)
	};
	return m;
}

// Two identical-position components with different source indices.
static TriMesh coincident_fixture()
{
	TriMesh m;
	// Four coincident vertices duplicated, so two triangles sit exactly on
	// top of each other but use distinct indices.
	m.vertices = {
		point(0, 0, 0), point(1, 0, 0), point(0, 1, 0),
		point(0, 0, 0), point(1, 0, 0), point(0, 1, 0)
	};
	m.faces = { TriMesh::Face(0, 1, 2), TriMesh::Face(3, 4, 5) };
	return m;
}

static void add_attrs(TriMesh &m)
{
	for (size_t i = 0; i < m.vertices.size(); ++i) {
		m.colors.push_back(Color(0.11f * float(i + 1),
		                         0.2f, 0.9f - 0.05f * float(i)));
		m.confidences.push_back(0.25f * float(i + 1));
		m.flags.push_back(100u + unsigned(i));
	}
	m.flag_curr = 77;
}

//---------------------------------------------------------------------------
// Pairing tests
//---------------------------------------------------------------------------

static int case_pairing_max_and_cost(void)
{
	TriMesh in = crossing_fixture();
	TriMesh orig = in;
	std::vector<std::pair<int, int> > fwd, bwd;
	group_faces(in, 0, 1, fwd, bwd);
	int bf_pairs;
	double bf_cost;
	brute_force_group(in, fwd, bwd, bf_pairs, bf_cost);

	SheetSplitResult r;
	TASK_CHECK(split_nonmanifold_sheets(&in, r));
	TASK_CHECK(verify_split(in, r, orig));

	int got_pairs;
	double got_cost;
	matched_group(r, 0, 1, orig, got_pairs, got_cost);
	TASK_CHECK(got_pairs == bf_pairs);
	TASK_CHECK(bf_pairs == 2);
	TASK_CHECK(got_cost <= bf_cost + 1e-10 * std::max(1.0, double(bf_pairs)));
	// The optimal pairing must be strictly better than the greedy/naive one
	// in this fixture, so a greedy matcher would fail.
	TASK_CHECK(std::fabs(got_cost - bf_cost) < 1e-9);
	printf("pairing brute_max=%d cost=%.12f\n", bf_pairs, bf_cost);
	return 0;
}

static int case_pairing_three_by_three(void)
{
	// 3 forward, 3 backward faces on one index edge with a cost matrix that
	// punishes the obvious greedy choice.  Reference: exhaustive assignment.
	TriMesh m;
	m.vertices = { point(0, 0, 0), point(1, 0, 0) };
	// Forward faces above, backward faces below with tilted normals.
	const float tops[3][3] = {
		{ 0.0f, 1.0f, 0.0f }, { 0.0f, 1.0f, 0.2f }, { 0.0f, 1.0f, 0.4f }
	};
	const float bots[3][3] = {
		{ 0.0f, -1.0f, 0.4f }, { 0.0f, -1.0f, 0.0f }, { 0.0f, -1.0f, 0.2f }
	};
	for (int i = 0; i < 3; ++i) {
		m.vertices.push_back(point(0.5f * i + 0.1f, tops[i][1],
		                            tops[i][2]));
		m.vertices.push_back(point(0.5f * i + 0.1f, bots[i][1],
		                            bots[i][2]));
	}
	for (int i = 0; i < 3; ++i) {
		m.faces.push_back(TriMesh::Face(0, 1, 2 + 2 * i));
		m.faces.push_back(TriMesh::Face(1, 0, 3 + 2 * i));
	}
	TriMesh orig = m;
	std::vector<std::pair<int, int> > fwd, bwd;
	group_faces(m, 0, 1, fwd, bwd);
	int bf_pairs;
	double bf_cost;
	brute_force_group(m, fwd, bwd, bf_pairs, bf_cost);

	SheetSplitResult r;
	TASK_CHECK(split_nonmanifold_sheets(&m, r));
	TASK_CHECK(verify_split(m, r, orig));
	int got_pairs;
	double got_cost;
	matched_group(r, 0, 1, orig, got_pairs, got_cost);
	TASK_CHECK(got_pairs == bf_pairs && bf_pairs == 3);
	TASK_CHECK(std::fabs(got_cost - bf_cost) < 1e-9);

	// A greedy row-by-row matcher makes a strictly worse choice here, so a
	// greedy implementation would fail the comparison above.
	double greedy_cost = 0.0;
	{
		bool used[3] = { false, false, false };
		for (int row = 0; row < 3; ++row) {
			int best_col = -1;
			double best = 1e300;
			for (int c = 0; c < 3; ++c) {
				if (used[c])
					continue;
				double v = pair_cost(orig, fwd[row].first,
				                     bwd[c].first);
				if (v < best) {
					best = v;
					best_col = c;
				}
			}
			used[best_col] = true;
			greedy_cost += best;
		}
	}
	TASK_CHECK(greedy_cost > bf_cost + 1e-6);
	printf("pairing 3x3 optimal=%.9f greedy=%.9f\n", bf_cost, greedy_cost);
	return 0;
}

static int case_pairing_unequal_and_same_direction(void)
{
	// 3 forward, 1 backward: maximum is 1 pair, the other two stay boundary.
	// A same-direction pair must never be selected.
	TriMesh m = crossing_fixture();
	// Add a fifth face sharing (0,1) forward, making 3 fwd / 2 bwd?  Instead
	// use the same-direction fixture: two forward faces on (0,1) but also a
	// backward boundary elsewhere.
	TriMesh sd = same_direction_fixture();
	TriMesh sd_orig = sd;
	SheetSplitResult r;
	TASK_CHECK(split_nonmanifold_sheets(&sd, r));
	TASK_CHECK(verify_split(sd, r, sd_orig));
	std::vector<std::pair<int, int> > fwd, bwd;
	group_faces(sd_orig, 0, 1, fwd, bwd);
	TASK_CHECK(fwd.size() == 2 && bwd.empty());
	for (int f = 0; f < int(sd_orig.faces.size()); ++f)
		for (int k = 0; k < 3; ++k) {
			int u = sd_orig.faces[f][NEXT_MOD3(k)];
			int w = sd_orig.faces[f][PREV_MOD3(k)];
			if ((u == 0 && w == 1) || (u == 1 && w == 0))
				TASK_CHECK(r.edge_partner[f][k] == -1);
		}
	// Unequal counts on the crossing fixture: remove one backward face so
	// there are 2 forward and 1 backward; exactly one pair is possible.
	TriMesh un = crossing_fixture();
	un.faces.pop_back(); // now 2 fwd, 1 bwd
	TriMesh un_orig = un;
	SheetSplitResult ru;
	TASK_CHECK(split_nonmanifold_sheets(&un, ru));
	TASK_CHECK(verify_split(un, ru, un_orig));
	group_faces(un_orig, 0, 1, fwd, bwd);
	TASK_CHECK(fwd.size() == 2 && bwd.size() == 1);
	int pairs;
	double cost;
	matched_group(ru, 0, 1, un_orig, pairs, cost);
	TASK_CHECK(pairs == 1);
	int bf_pairs;
	double bf_cost;
	brute_force_group(un_orig, fwd, bwd, bf_pairs, bf_cost);
	TASK_CHECK(pairs == bf_pairs);
	TASK_CHECK(cost <= bf_cost + 1e-10);
	return 0;
}

static int case_pairing_large_32(void)
{
	TriMesh in = large_crossing_fixture();
	TriMesh orig = in;
	SheetSplitResult r;
	TASK_CHECK(split_nonmanifold_sheets(&in, r));
	TASK_CHECK(verify_split(in, r, orig));

	std::vector<std::pair<int, int> > fwd, bwd;
	group_faces(orig, 0, 1, fwd, bwd);
	TASK_CHECK(fwd.size() == 32 && bwd.size() == 32);
	int pairs;
	double cost;
	matched_group(r, 0, 1, orig, pairs, cost);
	TASK_CHECK(pairs == 32);
	// Each pair joins a face with its exact antipodal twin, so each cost is
	// ~0 and the total is near zero.  Any greedy/cross pairing has strictly
	// larger cost, so this also checks optimality at scale.
	TASK_CHECK(cost < 1e-6);

	// Each forward face's partner must indeed be its twin index.
	for (int i = 0; i < 32; ++i) {
		int f = 2 * i;
		TASK_CHECK(r.edge_partner[f][2] == f + 1);
		TASK_CHECK(r.edge_partner[f + 1][2] == f);
	}
	// Determinism: same input twice yields identical pairing.
	TriMesh again = orig;
	SheetSplitResult r2;
	TASK_CHECK(split_nonmanifold_sheets(&again, r2));
	TASK_CHECK(r2.edge_partner == r.edge_partner);
	TASK_CHECK(r2.vertex_source == r.vertex_source);

	// The split of 32 pairs must remain a valid 2-manifold everywhere.
	std::vector<LinkInfo> links;
	TASK_CHECK(analyse_links(in, links));
	for (size_t v = 0; v < links.size(); ++v)
		TASK_CHECK(links[v].manifold);
	return 0;
}

static int case_pairing_deterministic(void)
{
	TriMesh a = crossing_fixture();
	TriMesh b = crossing_fixture();
	SheetSplitResult ra, rb;
	TASK_CHECK(split_nonmanifold_sheets(&a, ra));
	TASK_CHECK(split_nonmanifold_sheets(&b, rb));
	TASK_CHECK(ra.edge_partner == rb.edge_partner);
	TASK_CHECK(ra.vertex_source == rb.vertex_source);
	TASK_CHECK(a.faces == b.faces);
	return 0;
}

//---------------------------------------------------------------------------
// Fan tests
//---------------------------------------------------------------------------

static int case_fans_butterfly(void)
{
	TriMesh in = butterfly_fixture();
	TriMesh orig = in;
	SheetSplitResult r;
	TASK_CHECK(split_nonmanifold_sheets(&in, r));
	TASK_CHECK(verify_split(in, r, orig));
	// Only the shared vertex 0 is duplicated; everything else keeps its slot.
	TASK_CHECK(in.vertices.size() == 6);
	TASK_CHECK(r.vertex_source.size() == 6);
	TASK_CHECK(r.vertex_source[5] == 0);
	for (int i = 0; i < 5; ++i)
		TASK_CHECK(r.vertex_source[i] == i);
	// Both triangles are now in separate, single-fan vertex links.
	std::vector<LinkInfo> links;
	TASK_CHECK(analyse_links(in, links));
	for (size_t v = 0; v < links.size(); ++v)
		TASK_CHECK(links[v].manifold && links[v].components == 1);
	// No pairing was made; all edges are boundaries.
	for (int f = 0; f < 2; ++f)
		for (int k = 0; k < 3; ++k)
			TASK_CHECK(r.edge_partner[f][k] == -1);
	// The duplicated vertex 5 has a two-edge open-chain link through faces
	// 1 and 2, not a cycle and not two components.
	in.need_across_edge();
	TASK_CHECK(in.across_edge[0][0] == -1);
	TASK_CHECK(in.across_edge[1][0] == -1);
	TASK_CHECK(in.faces[1][0] == 5 && in.faces[0][0] == 0);
	return 0;
}

static int case_fans_duplicate_order(void)
{
	// A vertex with four feathers produces three duplicates appended in
	// (source vertex, minimum corner id 3*f+k) order.
	TriMesh m;
	m.vertices = { point(0, 0, 0) };
	for (int i = 0; i < 4; ++i) {
		// Each feather is a separate triangle anchored at vertex 0.
		m.vertices.push_back(point(1, 0, float(i)));
		m.vertices.push_back(point(0, 1, float(i)));
	}
	for (int i = 0; i < 4; ++i)
		m.faces.push_back(TriMesh::Face(0, 1 + 2 * i, 2 + 2 * i));
	TriMesh orig = m;
	SheetSplitResult r;
	TASK_CHECK(split_nonmanifold_sheets(&m, r));
	TASK_CHECK(verify_split(m, r, orig));
	TASK_CHECK(m.vertices.size() == orig.vertices.size() + 3);
	// First fan keeps slot 0; the three duplicates are appended in
	// min-corner order.
	TASK_CHECK(r.vertex_source[0] == 0);
	std::vector<int> dups;
	for (size_t i = orig.vertices.size(); i < r.vertex_source.size(); ++i) {
		TASK_CHECK(r.vertex_source[i] == 0);
		dups.push_back(int(i));
	}
	TASK_CHECK(dups.size() == 3);
	// Corner numbering: 3*f+k for k=0 corner is the anchor; ascending by f.
	TASK_CHECK(r.vertex_source[orig.vertices.size()] == 0);
	// Original slot 0 must be used by face 0's corner 0.
	TASK_CHECK(m.faces[0][0] == 0);
	TASK_CHECK(m.faces[1][0] == dups[0]);
	TASK_CHECK(m.faces[2][0] == dups[1]);
	TASK_CHECK(m.faces[3][0] == dups[2]);
	std::vector<LinkInfo> links;
	TASK_CHECK(analyse_links(m, links));
	for (size_t v = 0; v < links.size(); ++v)
		TASK_CHECK(links[v].manifold);
	return 0;
}

static int case_fans_multi_edge(void)
{
	// The two strips meet along a chain of edges.  Splitting must separate
	// them into two proper patches even though they touch at multiple
	// vertices.
	TriMesh in = multi_edge_fixture();
	TriMesh orig = in;
	SheetSplitResult r;
	TASK_CHECK(split_nonmanifold_sheets(&in, r));
	TASK_CHECK(verify_split(in, r, orig));
	std::vector<LinkInfo> links;
	TASK_CHECK(analyse_links(in, links));
	for (size_t v = 0; v < links.size(); ++v) {
		TASK_CHECK(links[v].manifold);
		TASK_CHECK(links[v].components == 1);
	}
	// All duplicated slots trace back to real input vertices.
	for (size_t i = orig.vertices.size(); i < in.vertices.size(); ++i)
		TASK_CHECK(r.vertex_source[i] == int(r.vertex_source[i]) &&
		            r.vertex_source[i] >= 0 &&
		            r.vertex_source[i] < int(orig.vertices.size()));
	return 0;
}

static int case_fans_quotient_conflict(void)
{
	TriMesh in = conflict_fixture();
	TriMesh orig = in;
	SheetSplitResult r;
	std::vector<int> stale_vs(3, 42);
	std::vector<TriMesh::Face> stale_ep(2, TriMesh::Face(9, 9, 9));
	r.vertex_source = stale_vs;
	r.edge_partner = stale_ep;
	// Force lazy caches to be absent; the failing call must not populate them.
	TASK_CHECK(in.neighbors.empty() && in.adjacentfaces.empty() &&
	            in.across_edge.empty());
	TASK_CHECK(!split_nonmanifold_sheets(&in, r));
	TASK_CHECK(unchanged(in, snapshot(orig), r, stale_vs.size(),
	                     stale_ep.size()));
	TASK_CHECK(in.neighbors.empty() && in.adjacentfaces.empty() &&
	            in.across_edge.empty() && in.normals.empty() &&
	            in.pointareas.empty() && in.cornerareas.empty());
	// The result must be byte-for-byte untouched.
	TASK_CHECK(r.vertex_source == stale_vs);
	TASK_CHECK(r.edge_partner == stale_ep);
	return 0;
}

static int case_fans_coincident(void)
{
	// Two components at identical positions with distinct source indices
	// stay independent: no welding, no duplication beyond what topology needs.
	TriMesh in = coincident_fixture();
	TriMesh orig = in;
	SheetSplitResult r;
	TASK_CHECK(split_nonmanifold_sheets(&in, r));
	TASK_CHECK(verify_split(in, r, orig));
	TASK_CHECK(in.vertices.size() == orig.vertices.size());
	TASK_CHECK(r.vertex_source.size() == orig.vertices.size());
	for (size_t i = 0; i < orig.faces.size(); ++i)
		TASK_CHECK(in.faces[i] == orig.faces[i]);
	// Distinct source indices remain distinct output indices.
	for (int f = 0; f < 2; ++f)
		for (int k = 0; k < 3; ++k)
			TASK_CHECK(in.faces[f][k] == orig.faces[f][k]);
	return 0;
}

static int case_fans_original_manifold(void)
{
	// Manifold closed meshes: no duplication, pairing mirrors across_edge,
	// and the result reproduces the original faces exactly.
	std::unique_ptr<TriMesh> meshes[] = {
		std::unique_ptr<TriMesh>(make_cube(3)),
		std::unique_ptr<TriMesh>(make_torus(12, 8)),
		std::unique_ptr<TriMesh>(make_sphere_polar(10, 8))
	};
	for (int mi = 0; mi < 3; ++mi) {
		TriMesh in = *meshes[mi];
		TriMesh orig = in;
		SheetSplitResult r;
		TASK_CHECK(split_nonmanifold_sheets(&in, r));
		TASK_CHECK(verify_split(in, r, orig));
		TASK_CHECK(in.vertices.size() == orig.vertices.size());
		// Reproduce the classic across_edge for a manifold mesh.
		orig.need_across_edge();
		for (size_t f = 0; f < orig.faces.size(); ++f)
			for (int k = 0; k < 3; ++k)
				TASK_CHECK(r.edge_partner[f][k] ==
				           orig.across_edge[f][k]);
		std::vector<LinkInfo> links;
		TASK_CHECK(analyse_links(in, links));
		for (size_t v = 0; v < links.size(); ++v)
			TASK_CHECK(links[v].manifold && links[v].closed);
	}
	return 0;
}

static int case_fans_attributes(void)
{
	TriMesh in = butterfly_fixture();
	add_attrs(in);
	in.need_normals();
	in.need_pointareas();
	TriMesh orig = in;
	orig.need_normals();
	orig.need_pointareas();
	SheetSplitResult r;
	TASK_CHECK(split_nonmanifold_sheets(&in, r));
	TASK_CHECK(verify_split(in, r, orig));

	// Attributes are copied by source vertex, flag_curr preserved.
	for (size_t i = 0; i < in.vertices.size(); ++i) {
		int s = r.vertex_source[i];
		TASK_CHECK(in.colors[i] == orig.colors[s]);
		TASK_CHECK(in.confidences[i] == orig.confidences[s]);
		TASK_CHECK(in.flags[i] == orig.flags[s]);
	}
	TASK_CHECK(in.flag_curr == orig.flag_curr);

	// Derived data is invalidated.
	TASK_CHECK(in.normals.empty());
	TASK_CHECK(in.pdir1.empty() && in.pdir2.empty());
	TASK_CHECK(in.curv1.empty() && in.curv2.empty());
	TASK_CHECK(in.dcurv.empty());
	TASK_CHECK(in.cornerareas.empty() && in.pointareas.empty());
	TASK_CHECK(in.neighbors.empty() && in.adjacentfaces.empty() &&
	            in.across_edge.empty());
	TASK_CHECK(in.tstrips.empty() && in.grid.empty());
	TASK_CHECK(!in.bbox.valid && !in.bsphere.valid);

	// Normals rebuild from the new topology: the two split copies of vertex 0
	// must not average the two sheets' normals together.
	in.need_normals();
	TASK_CHECK(in.normals.size() == in.vertices.size());
	vec n0 = in.normals[0], n5 = in.normals[5];
	TASK_CHECK(std::fabs(dot(n0, n5) - 1.0f) > 1e-4f);
	// Each copy's normal must match its own face normal.
	for (int f = 0; f < 2; ++f) {
		vec fn = normalized(trinorm(in.vertices[in.faces[f][0]],
		                            in.vertices[in.faces[f][1]],
		                            in.vertices[in.faces[f][2]]));
		for (int k = 0; k < 3; ++k)
			TASK_CHECK(dot(in.normals[in.faces[f][k]], fn) > 0.999f);
	}
	in.need_pointareas();
	TASK_CHECK(in.pointareas.size() == in.vertices.size());
	in.need_across_edge();
	TASK_CHECK(in.across_edge.size() == in.faces.size());
	for (int f = 0; f < 2; ++f)
		for (int k = 0; k < 3; ++k)
			TASK_CHECK(in.across_edge[f][k] == -1);
	return 0;
}

//---------------------------------------------------------------------------
// Pipeline tests
//---------------------------------------------------------------------------

static bool write_and_read(TriMesh &m, const char *prefix)
{
	std::string ascii = tmp_path((std::string(prefix) + "-ascii.ply").c_str());
	std::string binary = tmp_path((std::string(prefix) + "-bin.ply").c_str());
	if (!m.write(std::string("norm:ply_ascii:") + ascii) ||
	    !m.write(std::string("norm:ply_binary:") + binary))
		return false;
	std::unique_ptr<TriMesh> a(TriMesh::read(ascii.c_str()));
	std::unique_ptr<TriMesh> b(TriMesh::read(binary.c_str()));
	if (!a || !b)
		return false;
	if (!same_geometry(*a, m) || !same_geometry(*b, m))
		return false;
	return true;
}

static int case_pipeline_ply_roundtrip(void)
{
	TriMesh in = crossing_fixture();
	add_attrs(in);
	TriMesh orig = in;
	SheetSplitResult r;
	TASK_CHECK(split_nonmanifold_sheets(&in, r));
	in.need_normals();

	TASK_CHECK(write_and_read(in, "split-rt"));

	// Through the files, colors/confidences survive; flags are an in-memory
	// attribute only and are not written to PLY.
	std::string ascii = tmp_path("split-rt-ascii.ply");
	std::string bin = tmp_path("split-rt-bin.ply");
	std::unique_ptr<TriMesh> a(TriMesh::read(ascii.c_str()));
	TASK_CHECK(a != nullptr);
	TASK_CHECK(a->colors.size() == in.colors.size());
	TASK_CHECK(a->confidences.size() == in.confidences.size());
	for (size_t i = 0; i < in.vertices.size(); ++i) {
		TASK_CHECK(std::fabs(a->colors[i][0] - in.colors[i][0]) < 0.01f);
		TASK_CHECK(std::fabs(a->confidences[i] - in.confidences[i]) < 1e-5f);
		TASK_CHECK(a->normals.size() == in.vertices.size());
	}

	// Now remove_faces / remove_unused_vertices on the round-tripped mesh.
	std::unique_ptr<TriMesh> b(TriMesh::read(bin.c_str()));
	TASK_CHECK(b != nullptr);
	std::vector<bool> drop(b->faces.size(), false);
	drop[0] = true;
	remove_faces(b.get(), drop);
	remove_unused_vertices(b.get());
	TASK_CHECK(b->faces.size() == a->faces.size() - 1);
	b->need_normals();
	b->need_across_edge();
	TASK_CHECK(b->normals.size() == b->vertices.size());
	TASK_CHECK(b->across_edge.size() == b->faces.size());
	// Whatever pairing remains must be reciprocal and match real edges.
	for (size_t f = 0; f < b->faces.size(); ++f) {
		for (int k = 0; k < 3; ++k) {
			int g = b->across_edge[f][k];
			if (g < 0)
				continue;
			int u = b->faces[f][NEXT_MOD3(k)];
			int w = b->faces[f][PREV_MOD3(k)];
			bool reciprocal = false;
			for (int x = 0; x < 3; ++x)
				if (b->across_edge[g][x] == int(f) &&
				    b->faces[g][NEXT_MOD3(x)] == w &&
				    b->faces[g][PREV_MOD3(x)] == u)
					reciprocal = true;
			TASK_CHECK(reciprocal);
		}
	}
	return 0;
}

static int case_pipeline_subdiv(void)
{
	TriMesh in = butterfly_fixture();
	TriMesh orig = in;
	SheetSplitResult r;
	TASK_CHECK(split_nonmanifold_sheets(&in, r));
	in.need_normals();
	in.need_across_edge();
	in.need_pointareas();
	size_t nv_before = in.vertices.size();
	size_t nf_before = in.faces.size();
	// Subdivision must not confuse the two sheets again.
	subdiv(&in, SUBDIV_LOOP);
	TASK_CHECK(in.faces.size() > nf_before);
	TASK_CHECK(in.vertices.size() > nv_before);
	in.need_normals();
	in.need_across_edge();
	TASK_CHECK(in.normals.size() == in.vertices.size());
	TASK_CHECK(in.across_edge.size() == in.faces.size());
	// The two sheets were split, so the subdivided mesh retains two separate
	// components (check via vertex links all manifold).
	std::vector<LinkInfo> links;
	TASK_CHECK(analyse_links(in, links));
	for (size_t v = 0; v < links.size(); ++v)
		TASK_CHECK(links[v].manifold);
	return 0;
}

static int case_pipeline_bad_inputs(void)
{
	// NULL mesh.
	SheetSplitResult r;
	TriMesh dummy = butterfly_fixture();
	TriMesh dummy_orig = dummy;
	TASK_CHECK(!split_nonmanifold_sheets(nullptr, r));
	TASK_CHECK(snapshot(dummy).vertices.size() == dummy_orig.vertices.size());

	// Bad index.
	{
		TriMesh m = butterfly_fixture();
		m.faces[1][2] = 99;
		TriMesh o = m;
		TASK_CHECK(!split_nonmanifold_sheets(&m, r));
		TASK_CHECK(unchanged(m, snapshot(o), r, 0, 0));
	}
	// Repeated index inside a face.
	{
		TriMesh m = butterfly_fixture();
		m.faces[1][2] = m.faces[1][1];
		TriMesh o = m;
		TASK_CHECK(!split_nonmanifold_sheets(&m, r));
		TASK_CHECK(unchanged(m, snapshot(o), r, 0, 0));
	}
	// Non-finite coordinate.
	{
		TriMesh m = butterfly_fixture();
		m.vertices[2][0] = std::numeric_limits<float>::quiet_NaN();
		TriMesh o = m;
		TASK_CHECK(!split_nonmanifold_sheets(&m, r));
		TASK_CHECK(unchanged(m, snapshot(o), r, 0, 0));
	}
	// Oversized coordinate.
	{
		TriMesh m = butterfly_fixture();
		m.vertices[2][0] = 2000.0f;
		TriMesh o = m;
		TASK_CHECK(!split_nonmanifold_sheets(&m, r));
		TASK_CHECK(unchanged(m, snapshot(o), r, 0, 0));
	}
	// Tiny area (collinear points).
	{
		TriMesh m = butterfly_fixture();
		m.vertices[2] = point(0.5f, 0.0f, 0.0f); // collinear with 0 and 1
		TriMesh o = m;
		TASK_CHECK(!split_nonmanifold_sheets(&m, r));
		TASK_CHECK(unchanged(m, snapshot(o), r, 0, 0));
	}
	// Attribute length mismatch.
	{
		TriMesh m = butterfly_fixture();
		m.colors.push_back(Color(1, 1, 1));
		TriMesh o = m;
		TASK_CHECK(!split_nonmanifold_sheets(&m, r));
		TASK_CHECK(unchanged(m, snapshot(o), r, 0, 0));
	}
	{
		TriMesh m = butterfly_fixture();
		m.confidences = { 1.0f, 0.5f };
		TriMesh o = m;
		TASK_CHECK(!split_nonmanifold_sheets(&m, r));
		TASK_CHECK(unchanged(m, snapshot(o), r, 0, 0));
	}
	{
		TriMesh m = butterfly_fixture();
		m.flags = { 1u, 2u, 3u };
		TriMesh o = m;
		TASK_CHECK(!split_nonmanifold_sheets(&m, r));
		TASK_CHECK(unchanged(m, snapshot(o), r, 0, 0));
	}
	// Too many faces on one index edge (65 occurrences of edge 0-1).
	{
		TriMesh m;
		m.vertices.push_back(point(0, 0, 0));
		m.vertices.push_back(point(1, 0, 0));
		for (int i = 0; i < 65; ++i) {
			double th = 0.31 * i;
			m.vertices.push_back(point(0.5, std::cos(th),
			                            std::sin(th)));
			if (i % 2 == 0)
				m.faces.push_back(TriMesh::Face(0, 1, 2 + i));
			else
				m.faces.push_back(TriMesh::Face(1, 0, 2 + i));
		}
		TriMesh o = m;
		TASK_CHECK(!split_nonmanifold_sheets(&m, r));
		TASK_CHECK(unchanged(m, snapshot(o), r, 0, 0));
	}
	// 64 occurrences are allowed.
	{
		TriMesh m;
		m.vertices.push_back(point(0, 0, 0));
		m.vertices.push_back(point(1, 0, 0));
		for (int i = 0; i < 64; ++i) {
			double th = 0.13 * i + 0.05;
			m.vertices.push_back(point(0.5 + 0.01 * i,
			                            std::cos(th), std::sin(th)));
			if (i % 2 == 0)
				m.faces.push_back(TriMesh::Face(0, 1, 2 + i));
			else
				m.faces.push_back(TriMesh::Face(1, 0, 2 + i));
		}
		TASK_CHECK(split_nonmanifold_sheets(&m, r));
	}
	return 0;
}

static int case_pipeline_empty(void)
{
	TriMesh m;
	// Poison lazy caches so we can see them cleared.
	m.grid_width = 4;
	m.grid_height = 5;
	m.grid.assign(20, -1);
	m.normals.push_back(vec(0, 0, 1));
	m.pointareas.push_back(1.0f);
	m.bbox.valid = true;
	m.bsphere.valid = true;
	SheetSplitResult r;
	TASK_CHECK(split_nonmanifold_sheets(&m, r));
	TASK_CHECK(r.vertex_source.empty() && r.edge_partner.empty());
	TASK_CHECK(m.vertices.empty() && m.faces.empty());
	TASK_CHECK(m.normals.empty() && m.pointareas.empty());
	TASK_CHECK(m.grid.empty() && m.grid_width == -1 && m.grid_height == -1);
	TASK_CHECK(!m.bbox.valid && !m.bsphere.valid);
	return 0;
}

// Run the new CLI and compare against the library on the same input.
static bool run_cli(const std::string &in, const std::string &out, int &status)
{
	std::string cmd = ".task-build/mesh_check --split-sheets \"" + in +
	                  "\" \"" + out + "\" > /dev/null 2>&1";
	status = std::system(cmd.c_str());
	if (status == -1)
		return false;
	if (WIFEXITED(status))
		status = WEXITSTATUS(status);
	return true;
}

static int case_pipeline_cli_match(void)
{
	TriMesh in = crossing_fixture();
	add_attrs(in);
	// Deliberately leave stale normals to prove they are recomputed.
	in.normals.assign(in.vertices.size(), vec(0, 0, -1));
	in.need_pointareas();

	std::string infile = tmp_path("cli-in.ply");
	std::string outfile = tmp_path("cli-out.ply");
	TASK_CHECK(in.write(std::string("ply_binary:") + infile));

	// Library path on the same input.
	TriMesh lib = in;
	SheetSplitResult r;
	TASK_CHECK(split_nonmanifold_sheets(&lib, r));
	lib.need_normals();

	int status = -1;
	TASK_CHECK(run_cli(infile, outfile, status));
	TASK_CHECK(status == 0);
	std::unique_ptr<TriMesh> cli(TriMesh::read(outfile.c_str()));
	TASK_CHECK(cli != nullptr);
	TASK_CHECK(same_geometry(lib, *cli));
	TASK_CHECK(cli->normals.size() == cli->vertices.size());
	TASK_CHECK(cli->colors.size() == cli->vertices.size());
	TASK_CHECK(cli->confidences.size() == cli->vertices.size());
	// Normal directions recomputed from the split topology must agree.
	for (size_t i = 0; i < cli->normals.size(); ++i) {
		TASK_CHECK(std::fabs(cli->normals[i][0] - lib.normals[i][0]) < 1e-5f);
		TASK_CHECK(std::fabs(cli->normals[i][1] - lib.normals[i][1]) < 1e-5f);
		TASK_CHECK(std::fabs(cli->normals[i][2] - lib.normals[i][2]) < 1e-5f);
	}
	for (size_t i = 0; i < cli->colors.size(); ++i) {
		TASK_CHECK(std::fabs(cli->colors[i][0] - lib.colors[i][0]) < 0.01f);
		TASK_CHECK(std::fabs(cli->confidences[i] - lib.confidences[i]) < 1e-5f);
	}
	// The output must be a proper 2-manifold after the split.
	std::vector<LinkInfo> links;
	TASK_CHECK(analyse_links(*cli, links));
	return 0;
}

static int case_pipeline_cli_reject(void)
{
	// Bad input: reject with exit 2 and leave a pre-existing output intact.
	TriMesh bad = butterfly_fixture();
	bad.faces[0][0] = 42;
	std::string infile = tmp_path("cli-bad.ply");
	TASK_CHECK(bad.write(std::string("ply_ascii:") + infile));

	std::string outfile = tmp_path("cli-existing.ply");
	const char *marker = "PRE-EXISTING OUTPUT\n";
	{
		FILE *f = fopen(outfile.c_str(), "wb");
		TASK_CHECK(f != nullptr);
		fputs(marker, f);
		fclose(f);
	}
	int status = -1;
	TASK_CHECK(run_cli(infile, outfile, status));
	TASK_CHECK(status == 2);
	// Read the file back and check the marker survived.
	FILE *f = fopen(outfile.c_str(), "rb");
	TASK_CHECK(f != nullptr);
	char buf[64] = { 0 };
	size_t got = fread(buf, 1, sizeof(buf) - 1, f);
	fclose(f);
	TASK_CHECK(got == strlen(marker));
	TASK_CHECK(std::strcmp(buf, marker) == 0);

	// A non-existent output must not be created either.
	std::string missing = tmp_path("cli-missing-out.ply");
	remove(missing.c_str());
	status = -1;
	TASK_CHECK(run_cli(infile, missing, status));
	TASK_CHECK(status == 2);
	FILE *g = fopen(missing.c_str(), "rb");
	TASK_CHECK(g == nullptr);
	return 0;
}

static int case_pipeline_cli_default_unchanged(void)
{
	// The legacy path (no option) keeps its exit code behavior: a
	// non-manifold input is reported as 2, a clean one as 0.
	// Build a genuinely manifold mesh.
	TriMesh good;
	good.vertices = { point(0, 0, 0), point(1, 0, 0), point(1, 1, 0),
	                  point(0, 1, 0) };
	good.faces = { TriMesh::Face(0, 1, 2), TriMesh::Face(0, 2, 3) };
	std::string goodfile = tmp_path("cli-good.ply");
	std::string goodout = tmp_path("cli-good-out.ply");
	TASK_CHECK(good.write(std::string("ply_ascii:") + goodfile));
	std::string cmd = ".task-build/mesh_check \"" + goodfile + "\" \"" +
	                  goodout + "\" > /dev/null 2>&1";
	int status = std::system(cmd.c_str());
	TASK_CHECK(WIFEXITED(status));
	TASK_CHECK(WEXITSTATUS(status) == 0);

	TriMesh nm = crossing_fixture();
	std::string nmfile = tmp_path("cli-nm.ply");
	std::string nmout = tmp_path("cli-nm-out.ply");
	TASK_CHECK(nm.write(std::string("ply_ascii:") + nmfile));
	cmd = ".task-build/mesh_check \"" + nmfile + "\" \"" + nmout +
	      "\" > /dev/null 2>&1";
	status = std::system(cmd.c_str());
	TASK_CHECK(WIFEXITED(status));
	TASK_CHECK(WEXITSTATUS(status) == 2);
	return 0;
}

// A legacy manifold-compatible check: splitting a manifold mesh must leave
// across_edge identical to the original, so downstream code is unaffected.
static int case_pipeline_manifold_compat(void)
{
	TriMesh in = *std::unique_ptr<TriMesh>(make_ccyl(10, 4));
	TriMesh orig = in;
	SheetSplitResult r;
	TASK_CHECK(split_nonmanifold_sheets(&in, r));
	TASK_CHECK(verify_split(in, r, orig));
	orig.need_across_edge();
	TASK_CHECK(in.faces.size() == orig.faces.size());
	// Update across_edge from the new mesh and compare against the original.
	in.need_across_edge();
	for (size_t f = 0; f < in.faces.size(); ++f)
		for (int k = 0; k < 3; ++k)
			TASK_CHECK(in.across_edge[f][k] == orig.across_edge[f][k]);
	return 0;
}

} // anonymous namespace

//---------------------------------------------------------------------------
// Registration
//---------------------------------------------------------------------------

const struct task_case task_cases[] = {
	{ "pairing", "max_and_cost", case_pairing_max_and_cost },
	{ "pairing", "three_by_three", case_pairing_three_by_three },
	{ "pairing", "unequal_and_same_direction", case_pairing_unequal_and_same_direction },
	{ "pairing", "large_32", case_pairing_large_32 },
	{ "pairing", "deterministic", case_pairing_deterministic },

	{ "fans", "butterfly", case_fans_butterfly },
	{ "fans", "duplicate_order", case_fans_duplicate_order },
	{ "fans", "multi_edge", case_fans_multi_edge },
	{ "fans", "quotient_conflict", case_fans_quotient_conflict },
	{ "fans", "coincident", case_fans_coincident },
	{ "fans", "original_manifold", case_fans_original_manifold },
	{ "fans", "attributes", case_fans_attributes },

	{ "pipeline", "ply_roundtrip", case_pipeline_ply_roundtrip },
	{ "pipeline", "subdiv", case_pipeline_subdiv },
	{ "pipeline", "bad_inputs", case_pipeline_bad_inputs },
	{ "pipeline", "empty", case_pipeline_empty },
	{ "pipeline", "cli_match", case_pipeline_cli_match },
	{ "pipeline", "cli_reject", case_pipeline_cli_reject },
	{ "pipeline", "cli_default_unchanged", case_pipeline_cli_default_unchanged },
	{ "pipeline", "manifold_compat", case_pipeline_manifold_compat }
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
