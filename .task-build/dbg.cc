/*
Split sheets that share indexed edges into oriented 2-manifold patches.

The input faces are kept verbatim (order, corner order, positions).  Face
edges are paired independently per undirected index edge: first the number of
pairs is maximized, then the sum of the normal-disagreement costs is
minimized.  Only after the per-edge pairing is fixed are the corners around
each source vertex connected along the chosen pairs and split into vertex
fans, duplicating a vertex only when a source vertex carries more than one
fan.  Nothing is welded by spatial position, no face is flipped or added, and
no globally suboptimal fallback is attempted when the locally optimal pairing
produces a quotient-edge collision.
*/

#include "TriMesh.h"
#include "TriMesh_algo.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <map>
#include <utility>
#include <vector>

using namespace std;


namespace trimesh {
namespace {

// One face edge (the edge opposite local corner "corner") inside a group of
// coincident undirected index edges.  dir is +1 when the edge runs from the
// smaller vertex index to the larger one, -1 otherwise.
struct EdgeRef {
	int face;
	int corner;
	int dir;
};

// Unit face normal in double precision, computed directly from the winding
// and coordinates.  Returns false for a non-finite or degenerate triangle and
// stores twice the triangle area in twice_area.
bool unit_face_normal(const point &p0, const point &p1, const point &p2,
                      double n[3], double &twice_area)
{
	double ax = double(p1[0]) - double(p0[0]);
	double ay = double(p1[1]) - double(p0[1]);
	double az = double(p1[2]) - double(p0[2]);
	double bx = double(p2[0]) - double(p0[0]);
	double by = double(p2[1]) - double(p0[1]);
	double bz = double(p2[2]) - double(p0[2]);
	double nx = ay * bz - az * by;
	double ny = az * bx - ax * bz;
	double nz = ax * by - ay * bx;
	double len = sqrt(nx * nx + ny * ny + nz * nz);
	twice_area = len;
	if (!(len > 0.0) || !std::isfinite(len))
		return false;
	n[0] = nx / len;
	n[1] = ny / len;
	n[2] = nz / len;
	return true;
}

// Minimum-cost assignment of every row to a distinct column (rows <= cols).
// The e-maxx potentials implementation is O(rows^2 * cols); ties are resolved
// by the deterministic scan order.
void min_cost_assignment(const vector<vector<double> > &cost,
                         vector<int> &row_to_col)
{
	int n = int(cost.size());
	row_to_col.assign(n, -1);
	if (!n)
		return;
	int m = int(cost[0].size());
	const double INF = numeric_limits<double>::infinity();

	vector<double> u(n + 1, 0.0), v(m + 1, 0.0);
	vector<int> p(m + 1, 0), way(m + 1, 0);

	for (int i = 1; i <= n; i++) {
		p[0] = i;
		int j0 = 0;
		vector<double> minv(m + 1, INF);
		vector<char> used(m + 1, 0);
		do {
			used[j0] = 1;
			int i0 = p[j0], j1 = 0;
			double delta = INF;
			for (int j = 1; j <= m; j++) {
				if (used[j])
					continue;
				double cur = cost[i0 - 1][j - 1] - u[i0] - v[j];
				if (cur < minv[j]) {
					minv[j] = cur;
					way[j] = j0;
				}
				if (minv[j] < delta) {
					delta = minv[j];
					j1 = j;
				}
			}
			for (int j = 0; j <= m; j++) {
				if (used[j]) {
					u[p[j]] += delta;
					v[j] -= delta;
				} else {
					minv[j] -= delta;
				}
			}
			j0 = j1;
		} while (p[j0] != 0);

		do {
			int j1 = way[j0];
			p[j0] = p[j1];
			j0 = j1;
		} while (j0);
	}

	for (int j = 1; j <= m; j++) {
		if (p[j] >= 1 && p[j] <= n)
			row_to_col[p[j] - 1] = j - 1;
	}
}

struct DSU {
	vector<int> parent;
	explicit DSU(int n) : parent(n)
	{
		for (int i = 0; i < n; i++)
			parent[i] = i;
	}
	int find(int x)
	{
		while (parent[x] != x) {
			parent[x] = parent[parent[x]];
			x = parent[x];
		}
		return x;
	}
	void unite(int a, int b)
	{
		a = find(a);
		b = find(b);
		if (a == b)
			return;
		if (a < b)
			parent[b] = a;
		else
			parent[a] = b;
	}
};

struct FanKey {
	int vertex;
	int min_corner;
	int root;
	bool operator<(const FanKey &o) const
	{
		if (vertex != o.vertex)
			return vertex < o.vertex;
		return min_corner < o.min_corner;
	}
};

} // anonymous namespace


bool split_nonmanifold_sheets(TriMesh *mesh, SheetSplitResult &result)
{
	if (!mesh)
		return false;

	const int nv = int(mesh->vertices.size());
	const int nf = int(mesh->faces.size());

	// Attribute lengths must either be absent or match the vertex array.
	if ((!mesh->colors.empty() && int(mesh->colors.size()) != nv) ||
	    (!mesh->confidences.empty() && int(mesh->confidences.size()) != nv) ||
	    (!mesh->flags.empty() && int(mesh->flags.size()) != nv))
		return false;

	// Empty mesh: nothing to split, but still invalidate derived data.
	if (!nv) {
		if (nf)
			return false;
		SheetSplitResult out;
		result = std::move(out);

		mesh->clear_normals();
		mesh->clear_curvatures();
		mesh->clear_dcurv();
		mesh->clear_pointareas();
		mesh->clear_neighbors();
		mesh->clear_adjacentfaces();
		mesh->clear_across_edge();
		mesh->clear_tstrips();
		mesh->clear_grid();
		mesh->clear_bbox();
		mesh->clear_bsphere();
		return true;
	}

	// Sanity-check geometry and face indices without touching lazy caches.
	vector< vector<double> > face_normal(nf, vector<double>(3, 0.0));
	for (int i = 0; i < nv; i++) {
		for (int c = 0; c < 3; c++) {
			double x = double(mesh->vertices[i][c]);
			if (!std::isfinite(x) || std::fabs(x) > 1000.0)
				return false;
		}
	}
	for (int f = 0; f < nf; f++) {
		const TriMesh::Face &fc = mesh->faces[f];
		for (int k = 0; k < 3; k++) {
			int idx = fc[k];
			if (idx < 0 || idx >= nv)
				return false;
		}
		if (fc[0] == fc[1] || fc[1] == fc[2] || fc[2] == fc[0])
			return false;
		double twice_area = 0.0;
		if (!unit_face_normal(mesh->vertices[fc[0]],
		                      mesh->vertices[fc[1]],
		                      mesh->vertices[fc[2]],
		                      &face_normal[f][0], twice_area))
			return false;
		if (twice_area * 0.5 < 1.0e-8)
			return false;
	}

	// Group face edges by undirected index edge.
	map< pair<int, int>, vector<EdgeRef> > groups;
	for (int f = 0; f < nf; f++) {
		const TriMesh::Face &fc = mesh->faces[f];
		for (int k = 0; k < 3; k++) {
			int v1 = fc[NEXT_MOD3(k)];
			int v2 = fc[PREV_MOD3(k)];
			int lo = min(v1, v2);
			int hi = max(v1, v2);
			EdgeRef ref;
			ref.face = f;
			ref.corner = k;
			ref.dir = (v1 == lo) ? 1 : -1;
			groups[make_pair(lo, hi)].push_back(ref);
			if (groups[make_pair(lo, hi)].size() > 64)
				return false;
		}
	}

	// Partner lookup, same indexing convention as across_edge.
	vector<TriMesh::Face> partner(nf, TriMesh::Face(-1, -1, -1));

	typedef map< pair<int, int>, vector<EdgeRef> >::const_iterator GroupIt;
	for (GroupIt git = groups.begin(); git != groups.end(); ++git) {
		vector<EdgeRef> fwd, bwd;
		for (size_t i = 0; i < git->second.size(); i++) {
			if (git->second[i].dir > 0)
				fwd.push_back(git->second[i]);
			else
				bwd.push_back(git->second[i]);
		}

		// Same-direction edges can never be paired.  A complete bipartite
		// graph on differing directions always admits min(|fwd|,|bwd|)
		// pairs, which is the maximum.
		if (fwd.empty() || bwd.empty())
			continue;

		bool fwd_is_rows = fwd.size() <= bwd.size();
		int rows = int(fwd_is_rows ? fwd.size() : bwd.size());
		int cols = int(fwd_is_rows ? bwd.size() : fwd.size());
		vector< vector<double> > cost(rows, vector<double>(cols, 0.0));
		for (int r = 0; r < rows; r++) {
			const EdgeRef &a = fwd_is_rows ? fwd[r] : bwd[r];
			const double *na = &face_normal[a.face][0];
			for (int c = 0; c < cols; c++) {
				const EdgeRef &b = fwd_is_rows ? bwd[c] : fwd[c];
				const double *nb = &face_normal[b.face][0];
				double d = na[0] * nb[0] + na[1] * nb[1] + na[2] * nb[2];
				if (d > 1.0)
					d = 1.0;
				else if (d < -1.0)
					d = -1.0;
				cost[r][c] = 1.0 - d;
			}
		}

		vector<int> row_to_col;
		min_cost_assignment(cost, row_to_col);
		for (int r = 0; r < rows; r++) {
			int c = row_to_col[r];
			if (c < 0)
				continue;
			const EdgeRef &a = fwd_is_rows ? fwd[r] : bwd[r];
			const EdgeRef &b = fwd_is_rows ? bwd[c] : fwd[c];
			partner[a.face][a.corner] = b.face;
			partner[b.face][b.corner] = a.face;
		}
	}

	// Connect corners around each source vertex along the selected pairs.
	// Two distinct input edges can collapse onto the same output edge after
	// fan splitting; that quotient-edge conflict is detected below.
	DSU dsu(3 * nf);
	for (int f = 0; f < nf; f++) {
		for (int k = 0; k < 3; k++) {
			int g = partner[f][k];
			if (g < 0)
				continue;
			// Only link from one side so each pair is processed once.
			if (f > g)
				continue;
			int kg = -1;
			for (int m = 0; m < 3; m++) {
				if (partner[g][m] == f) {
					kg = m;
					break;
				}
			}
			if (kg < 0)
				continue;
			dsu.unite(3 * f + NEXT_MOD3(k), 3 * g + PREV_MOD3(kg));
			dsu.unite(3 * f + PREV_MOD3(k), 3 * g + NEXT_MOD3(kg));
		}
	}

	// For every source vertex, split its corners into fans and choose the
	// output vertex slot for each fan.
	vector<int> corner_out(3 * nf, -1);
	vector<int> vertex_source(nv, 0);
	for (int i = 0; i < nv; i++)
		vertex_source[i] = i;

	// Corners grouped by source vertex.
	vector< vector<int> > corners_of(nv);
	for (int f = 0; f < nf; f++)
		for (int k = 0; k < 3; k++)
			corners_of[mesh->faces[f][k]].push_back(3 * f + k);

	vector<FanKey> duplicates;
	for (int v = 0; v < nv; v++) {
		if (corners_of[v].empty())
			continue;

		// Distinct roots for this source vertex, tracked by their smallest
		// face-corner id.
		map<int, int> root_min;
		for (size_t i = 0; i < corners_of[v].size(); i++) {
			int c = corners_of[v][i];
			int r = dsu.find(c);
			map<int, int>::iterator it = root_min.find(r);
			if (it == root_min.end() || c < it->second)
				root_min[r] = c;
		}

		vector< pair<int, int> > fans; // (min_corner, root)
		for (map<int, int>::const_iterator it = root_min.begin();
		     it != root_min.end(); ++it)
			fans.push_back(make_pair(it->second, it->first));
		sort(fans.begin(), fans.end());

		// The first fan (smallest minimum corner id) keeps the original slot.
		for (size_t i = 0; i < fans.size(); i++) {
			int root = fans[i].second;
			if (i == 0) {
				for (size_t j = 0; j < corners_of[v].size(); j++) {
					int c = corners_of[v][j];
					if (dsu.find(c) == root)
						corner_out[c] = v;
				}
			} else {
				FanKey key;
				key.vertex = v;
				key.min_corner = fans[i].first;
				key.root = root;
				duplicates.push_back(key);
			}
		}
	}

	// Append duplicate slots ordered by (source vertex, minimum corner id).
	sort(duplicates.begin(), duplicates.end());
	int nv_out = nv + int(duplicates.size());
	vertex_source.resize(nv_out);
	for (size_t i = 0; i < duplicates.size(); i++) {
		int slot = nv + int(i);
		vertex_source[slot] = duplicates[i].vertex;
		for (size_t j = 0; j < corners_of[duplicates[i].vertex].size(); j++) {
			int c = corners_of[duplicates[i].vertex][j];
			if (dsu.find(c) == duplicates[i].root)
				corner_out[c] = slot;
		}
	}

	for (int f = 0; f < nf; f++)
		fprintf(stderr, "PARTNER f%d: %d %d %d  out %d %d %d\n", f, partner[f][0], partner[f][1], partner[f][2], corner_out[3*f], corner_out[3*f+1], corner_out[3*f+2]);
	// Build the output faces and verify that the resulting real index edges
	// exactly reflect edge_partner.  Each output edge must appear either once
	// (boundary) or exactly twice in opposite directions as one selected pair;
	// anything else is a quotient-edge conflict.
	vector<TriMesh::Face> out_faces(nf);
	for (int f = 0; f < nf; f++)
		for (int k = 0; k < 3; k++)
			out_faces[f][k] = corner_out[3 * f + k];

	struct Occurrence {
		int face;
		int corner;
		int dir;
	};
	map< pair<int, int>, vector<Occurrence> > out_edges;
	for (int f = 0; f < nf; f++) {
		for (int k = 0; k < 3; k++) {
			int u = out_faces[f][NEXT_MOD3(k)];
			int w = out_faces[f][PREV_MOD3(k)];
			if (u < 0 || w < 0)
				return false;
			if (u == w)
				return false;
			Occurrence occ;
			occ.face = f;
			occ.corner = k;
			occ.dir = (u < w) ? 1 : -1;
			out_edges[make_pair(min(u, w), max(u, w))].push_back(occ);
		}
	}

	typedef map< pair<int, int>, vector<Occurrence> >::const_iterator OIt;
	for (OIt oit = out_edges.begin(); oit != out_edges.end(); ++oit) {
		const vector<Occurrence> &os = oit->second;
		if (os.size() == 1) {
			if (partner[os[0].face][os[0].corner] != -1) {
				fprintf(stderr, "MISSINGPAIR face=%d corner=%d edge=(%d,%d) partner=%d\n", os[0].face, os[0].corner, oit->first.first, oit->first.second, partner[os[0].face][os[0].corner]);
				return false;
			}
			continue;
		}
		if (os.size() == 2 && os[0].dir != os[1].dir) {
			int f0 = os[0].face, k0 = os[0].corner;
			int f1 = os[1].face, k1 = os[1].corner;
			if (partner[f0][k0] == f1 && partner[f1][k1] == f0)
				continue; // the one legitimate paired edge
		}
		fprintf(stderr, "CONFLICT edge=(%d,%d) count=%zu:", oit->first.first, oit->first.second, os.size());
		for (size_t q = 0; q < os.size(); q++)
			fprintf(stderr, " [f%d k%d d%d p%d]", os[q].face, os[q].corner, os[q].dir, partner[os[q].face][os[q].corner]);
		fprintf(stderr, "\n");
		return false; // quotient-edge conflict
	}

	// Every selected pair must correspond to exactly one output edge.  The
	// checks above already imply this, but verify explicitly so a bug cannot
	// silently produce a wrong across_edge relation.
	for (int f = 0; f < nf; f++) {
		for (int k = 0; k < 3; k++) {
			int g = partner[f][k];
			if (g < 0)
				continue;
			int u0 = out_faces[f][NEXT_MOD3(k)];
			int w0 = out_faces[f][PREV_MOD3(k)];
			int kg = -1;
			for (int m = 0; m < 3; m++) {
				if (partner[g][m] == f) {
					kg = m;
					break;
				}
			}
			if (kg < 0)
				return false;
			int u1 = out_faces[g][NEXT_MOD3(kg)];
			int w1 = out_faces[g][PREV_MOD3(kg)];
			if (u0 != w1 || w0 != u1)
				return false;
		}
	}

	// Commit.  Nothing above modified the mesh or result.
	vector<point> out_vertices(nv_out);
	for (int i = 0; i < nv_out; i++)
		out_vertices[i] = mesh->vertices[vertex_source[i]];

	vector<Color> out_colors;
	vector<float> out_confidences;
	vector<unsigned> out_flags;
	if (!mesh->colors.empty()) {
		out_colors.resize(nv_out);
		for (int i = 0; i < nv_out; i++)
			out_colors[i] = mesh->colors[vertex_source[i]];
	}
	if (!mesh->confidences.empty()) {
		out_confidences.resize(nv_out);
		for (int i = 0; i < nv_out; i++)
			out_confidences[i] = mesh->confidences[vertex_source[i]];
	}
	if (!mesh->flags.empty()) {
		out_flags.resize(nv_out);
		for (int i = 0; i < nv_out; i++)
			out_flags[i] = mesh->flags[vertex_source[i]];
	}

	mesh->vertices.swap(out_vertices);
	mesh->faces.swap(out_faces);
	if (!out_colors.empty())
		mesh->colors.swap(out_colors);
	if (!out_confidences.empty())
		mesh->confidences.swap(out_confidences);
	if (!out_flags.empty())
		mesh->flags.swap(out_flags);
	// Empty attribute vectors stay empty; flag_curr is deliberately preserved.

	// Invalidate all derived data: normals must be rebuilt from the new
	// topology so split sheets never average across the cut.
	mesh->clear_normals();
	mesh->clear_curvatures();
	mesh->clear_dcurv();
	mesh->clear_pointareas();
	mesh->clear_neighbors();
	mesh->clear_adjacentfaces();
	mesh->clear_across_edge();
	mesh->clear_tstrips();
	mesh->clear_grid();
	mesh->clear_bbox();
	mesh->clear_bsphere();

	result.vertex_source.swap(vertex_source);
	result.edge_partner.swap(partner);
	return true;
}

} // namespace trimesh
