// Tests for the reflection-symmetry constrained Fast Quadric simplification.
//
// The checks rebuild face pairing, boundary loops and the mate involution
// independently of the implementation and compare joint QEM minima against an
// independently assembled small linear system. Coordinates are only mirror
// images within a tolerance, so "looks symmetric" is never accepted as proof.
#include "src.cmd/Simplify.h"
#include "task_support/check.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <set>
#include <map>
#include <algorithm>
#include <stdexcept>

using namespace Simplify;

namespace {

const double kEps = 1e-9;

struct Tri { int v[3]; };

struct Mesh {
	std::vector<vec3f> p;
	std::vector<Tri> f;
};

void reset_mesh() {
	vertices.clear();
	triangles.clear();
	refs.clear();
	materials.clear();
	mtllib.clear();
}

void load_mesh(const Mesh &m) {
	reset_mesh();
	vertices.resize(m.p.size());
	for (size_t i = 0; i < m.p.size(); ++i) {
		Vertex v = Vertex();
		v.p = m.p[i];
		vertices[i] = v;
	}
	triangles.resize(m.f.size());
	for (size_t i = 0; i < m.f.size(); ++i) {
		Triangle t = Triangle();
		t.v[0] = m.f[i].v[0];
		t.v[1] = m.f[i].v[1];
		t.v[2] = m.f[i].v[2];
		t.attr = TEXCOORD;
		t.material = -1;
		for (int k = 0; k < 3; ++k) {
			const vec3f &q = vertices[t.v[k]].p;
			t.uvs[k] = vec3f(std::fabs(q.x) + 0.25 * q.y, q.y, 0.0);
		}
		triangles[i] = t;
	}
}

// ---------------------------------------------------------------------------
// Mesh helpers

// grid (i, j) -> vertex index; rows are continuous so neighbouring quads share
// their vertices.
struct Grid {
	int n;
	std::vector<int> left;   // (n+1)^2, left half
	std::vector<int> right;  // (n+1)^2, -1 at the seam columns
};

Grid make_grid(int n) {
	Grid g;
	g.n = n;
	g.left.assign((n + 1) * (n + 1), -1);
	g.right.assign((n + 1) * (n + 1), -1);
	int next = 0;
	for (int j = 0; j <= n; ++j)
		for (int i = 0; i <= n; ++i) g.left[j * (n + 1) + i] = next++;
	for (int j = 0; j <= n; ++j)
		for (int i = 1; i <= n; ++i) g.right[j * (n + 1) + i] = next++;
	return g;
}

// Build the double sided grid: x = -i/n on the left, x = +i/n on the right,
// both halves share the x = 0 column (self-mate vertices). The mirror maps
// left (i,j) to right (i,j). `dx` shifts the right half along x to model an
// input that only mirrors up to a tolerance.
Mesh make_symmetric_slab(int n, double dx) {
	Grid g = make_grid(n);
	Mesh m;
	int next = 0;
	for (int j = 0; j <= n; ++j)
		for (int i = 0; i <= n; ++i) {
			vec3f q(-double(i) / n, double(j) / n, 0.0);
			m.p.push_back(q);
			(void)next;
		}
	for (int j = 0; j <= n; ++j)
		for (int i = 1; i <= n; ++i)
			m.p.push_back(vec3f(double(i) / n + dx, double(j) / n, 0.0));
	// fix up indices stored by make_grid (built before vertices were added)
	g = Grid();
	g.n = n;
	g.left.assign((n + 1) * (n + 1), -1);
	g.right.assign((n + 1) * (n + 1), -1);
	int idx = 0;
	for (int j = 0; j <= n; ++j)
		for (int i = 0; i <= n; ++i) g.left[j * (n + 1) + i] = idx++;
	for (int j = 0; j <= n; ++j)
		for (int i = 1; i <= n; ++i) g.right[j * (n + 1) + i] = idx++;
	for (int j = 0; j < n; ++j)
		for (int i = 0; i < n; ++i) {
			int a = g.left[j * (n + 1) + i], b = g.left[j * (n + 1) + i + 1];
			int c = g.left[(j + 1) * (n + 1) + i], d = g.left[(j + 1) * (n + 1) + i + 1];
			Tri t1 = {{a, b, d}};
			Tri t2 = {{a, d, c}};
			m.f.push_back(t1);
			m.f.push_back(t2);
		}
	for (int j = 0; j < n; ++j)
		for (int i = 0; i < n; ++i) {
			int a = (i == 0) ? g.left[j * (n + 1)] : g.right[j * (n + 1) + i];
			int b = g.right[j * (n + 1) + i + 1];
			int c = (i == 0) ? g.left[(j + 1) * (n + 1)] : g.right[(j + 1) * (n + 1) + i];
			int d = g.right[(j + 1) * (n + 1) + i + 1];
			Tri t1 = {{a, d, b}};
			Tri t2 = {{a, c, d}};
			m.f.push_back(t1);
			m.f.push_back(t2);
		}
	return m;
}

// mate for the slab above, derived from the grid indices
std::vector<int> slab_mate(const Mesh &m, int n) {
	std::vector<int> mate(m.p.size(), -1);
	int leftCount = (n + 1) * (n + 1);
	std::vector<int> left(leftCount), right((n + 1) * (n + 1), -1);
	int idx = 0;
	for (int j = 0; j <= n; ++j)
		for (int i = 0; i <= n; ++i) left[j * (n + 1) + i] = idx++;
	for (int j = 0; j <= n; ++j)
		for (int i = 1; i <= n; ++i) right[j * (n + 1) + i] = idx++;
	for (int j = 0; j <= n; ++j)
		for (int i = 0; i <= n; ++i) {
			int l = left[j * (n + 1) + i];
			int r = (i == 0) ? l : right[j * (n + 1) + i];
			mate[l] = r;
			mate[r] = l;
		}
	return mate;
}

// Two disjoint mirrored patches: a closed-ish left box and its mirror without
// a shared seam. Exercises ordinary four point orbits (no self-mates).
Mesh make_mirrored_pair(int n, double dx) {
	Mesh m;
	int w = n + 1;
	std::vector<int> L((n + 1) * (n + 1)), R((n + 1) * (n + 1));
	for (int j = 0; j <= n; ++j)
		for (int i = 0; i <= n; ++i) {
			L[j * w + i] = (int)m.p.size();
			m.p.push_back(vec3f(-1.25 - double(i) / n, double(j) / n, 0.0));
		}
	for (int j = 0; j <= n; ++j)
		for (int i = 0; i <= n; ++i) {
			R[j * w + i] = (int)m.p.size();
			m.p.push_back(vec3f(1.25 + double(i) / n + dx, double(j) / n, 0.0));
		}
	for (int j = 0; j < n; ++j)
		for (int i = 0; i < n; ++i) {
			int a = L[j * w + i], b = L[j * w + i + 1];
			int c = L[(j + 1) * w + i], d = L[(j + 1) * w + i + 1];
			Tri t1 = {{a, b, d}}, t2 = {{a, d, c}};
			m.f.push_back(t1);
			m.f.push_back(t2);
			int ea = R[j * w + i], eb = R[j * w + i + 1];
			int ec = R[(j + 1) * w + i], ed = R[(j + 1) * w + i + 1];
			Tri u1 = {{ea, ed, eb}}, u2 = {{ea, ec, ed}};
			m.f.push_back(u1);
			m.f.push_back(u2);
		}
	return m;
}

std::vector<int> pair_mate(const Mesh &m, int n) {
	std::vector<int> mate(m.p.size(), -1);
	int w = n + 1;
	int leftCount = w * w;
	for (int j = 0; j <= n; ++j)
		for (int i = 0; i <= n; ++i) {
			int l = j * w + i;
			int r = leftCount + j * w + i;
			mate[l] = r;
			mate[r] = l;
		}
	return mate;
}

// ---------------------------------------------------------------------------
// Independent reconstruction helpers

void sorted_tri(int a, int b, int c, int out[3]) {
	if (a > b) std::swap(a, b);
	if (b > c) std::swap(b, c);
	if (a > b) std::swap(a, b);
	out[0] = a;
	out[1] = b;
	out[2] = c;
}

struct MeshView {
	std::vector<vec3f> p;
	std::vector<Tri> f;
};

MeshView snapshot() {
	MeshView v;
	v.p.resize(vertices.size());
	for (size_t i = 0; i < vertices.size(); ++i) v.p[i] = vertices[i].p;
	v.f.resize(triangles.size());
	for (size_t i = 0; i < triangles.size(); ++i)
		for (int k = 0; k < 3; ++k) v.f[i].v[k] = triangles[i].v[k];
	return v;
}

// Verify manifoldness, orientation and symmetry of the result. Returns true on
// success; on failure the caller gets a rough reason in `why`.
bool check_result(const MeshView &v, const SymmetryContext &ctx, double tol,
                  std::string &why) {
	size_t nv = v.p.size();
	if (ctx.mate.size() != nv) { why = "mate size"; return false; }
	// mate involution and reflection exactness
	for (size_t i = 0; i < nv; ++i) {
		int m = ctx.mate[i];
		if (m < 0 || (size_t)m >= nv || ctx.mate[m] != (int)i) { why = "mate involution"; return false; }
		double nx = ctx.normal.x, ny = ctx.normal.y, nz = ctx.normal.z;
		double t = nx * v.p[i].x + ny * v.p[i].y + nz * v.p[i].z + ctx.offset;
		vec3f r(v.p[i].x - 2 * t * nx, v.p[i].y - 2 * t * ny, v.p[i].z - 2 * t * nz);
		vec3f d = r - v.p[m];
		if (!(d.length() <= tol)) { why = "mirror distance"; return false; }
		if (m == (int)i && !(std::fabs(t) <= tol)) { why = "self mate off plane"; return false; }
	}
	// faces: valid indices, positive area, no duplicates, edge manifoldness
	std::set<std::vector<int> > faceSet;
	std::map<std::pair<int,int>, int> edgeCount;
	for (size_t i = 0; i < v.f.size(); ++i) {
		const Tri &t = v.f[i];
		for (int k = 0; k < 3; ++k)
			if (t.v[k] < 0 || (size_t)t.v[k] >= nv) { why = "face index"; return false; }
		if (t.v[0] == t.v[1] || t.v[1] == t.v[2] || t.v[2] == t.v[0]) { why = "degenerate face index"; return false; }
		vec3f a = v.p[t.v[0]], b = v.p[t.v[1]], c = v.p[t.v[2]];
		vec3f n;
		n.cross(b - a, c - a);
		if (!(n.length() > 0) || !std::isfinite(n.length())) { why = "zero area face"; return false; }
		std::vector<int> key(t.v, t.v + 3);
		std::sort(key.begin(), key.end());
		if (!faceSet.insert(key).second) { why = "duplicate face"; return false; }
		for (int k = 0; k < 3; ++k) {
			int x = t.v[k], y = t.v[(k + 1) % 3];
			std::pair<int,int> e(std::min(x, y), std::max(x, y));
			if (++edgeCount[e] > 2) { why = "non manifold edge"; return false; }
		}
	}
	// orientation: every directed edge appears at most once
	std::set<std::pair<int,int> > dir;
	for (size_t i = 0; i < v.f.size(); ++i)
		for (int k = 0; k < 3; ++k) {
			std::pair<int,int> e(v.f[i].v[k], v.f[i].v[(k + 1) % 3]);
			if (!dir.insert(e).second) { why = "inconsistent winding"; return false; }
		}
	// reflection of the face set with reversed winding
	std::set<std::vector<int> > canon;
	for (size_t i = 0; i < v.f.size(); ++i) {
		int a = v.f[i].v[0], b = v.f[i].v[1], c = v.f[i].v[2];
		int k[3];
		sorted_tri(ctx.mate[c], ctx.mate[b], ctx.mate[a], k);
		canon.insert(std::vector<int>(k, k + 3));
	}
	for (std::set<std::vector<int> >::iterator it = faceSet.begin(); it != faceSet.end(); ++it)
		if (!canon.count(*it)) { why = "unmirrored face"; return false; }
	return true;
}

// Boundary loops of a mesh, one vector of vertices per loop.
std::vector<std::vector<int> > boundary_loops(const MeshView &v) {
	std::map<std::pair<int,int>, int> count;
	for (size_t i = 0; i < v.f.size(); ++i)
		for (int k = 0; k < 3; ++k) {
			int x = v.f[i].v[k], y = v.f[i].v[(k + 1) % 3];
			count[std::make_pair(std::min(x, y), std::max(x, y))]++;
		}
	std::map<int, std::vector<int> > adj;
	for (std::map<std::pair<int,int>, int>::iterator it = count.begin(); it != count.end(); ++it)
		if (it->second == 1) {
			adj[it->first.first].push_back(it->first.second);
			adj[it->first.second].push_back(it->first.first);
		}
	std::vector<std::vector<int> > loops;
	std::set<int> seen;
	for (std::map<int, std::vector<int> >::iterator it = adj.begin(); it != adj.end(); ++it) {
		if (seen.count(it->first)) continue;
		std::vector<int> loop;
		int cur = it->first, prev = -1;
		while (true) {
			loop.push_back(cur);
			seen.insert(cur);
			int nxt = -1;
			for (size_t k = 0; k < adj[cur].size(); ++k)
				if (adj[cur][k] != prev) { nxt = adj[cur][k]; break; }
			if (nxt < 0) break;
			prev = cur;
			cur = nxt;
			if (cur == loop[0]) break;
		}
		if (loop.size() >= 3) loops.push_back(loop);
	}
	return loops;
}

// Vertex face fans after the result must be single components.
bool check_fans(const MeshView &v, std::string &why) {
	size_t nv = v.p.size();
	std::vector<std::vector<int> > faces(nv);
	for (size_t i = 0; i < v.f.size(); ++i)
		for (int k = 0; k < 3; ++k) faces[v.f[i].v[k]].push_back((int)i);
	for (size_t x = 0; x < nv; ++x) {
		if (faces[x].empty()) continue;
		size_t m = faces[x].size();
		std::vector<int> comp(m, -1);
		comp[0] = 0;
		std::vector<int> stack(1, 0);
		while (!stack.empty()) {
			int ci = stack.back(); stack.pop_back();
			for (size_t j = 0; j < m; ++j) {
				if (comp[j] >= 0) continue;
				int shared = 0;
				for (int p = 0; p < 3; ++p) {
					if (v.f[faces[x][ci]].v[p] == (int)x) continue;
					for (int q = 0; q < 3; ++q)
						if (v.f[faces[x][j]].v[q] == v.f[faces[x][ci]].v[p]) ++shared;
				}
				if (shared >= 1) { comp[j] = 0; stack.push_back((int)j); }
			}
		}
		for (size_t j = 0; j < m; ++j)
			if (comp[j] < 0) { why = "split fan"; return false; }
	}
	return true;
}

// Connected components of the result.
int component_count(const MeshView &v) {
	size_t nv = v.p.size();
	if (nv == 0) return 0;
	std::vector<int> parent(nv);
	for (size_t i = 0; i < nv; ++i) parent[i] = (int)i;
	std::vector<int> used(nv, 0);
	for (size_t i = 0; i < v.f.size(); ++i)
		for (int k = 0; k < 3; ++k) used[v.f[i].v[k]] = 1;
	for (size_t i = 0; i < v.f.size(); ++i) {
		int a = v.f[i].v[0], b = v.f[i].v[1], c = v.f[i].v[2];
		int r[3] = {a, b, c};
		for (int k = 1; k < 3; ++k) {
			int x = r[k], y = r[0];
			while (parent[x] != x) x = parent[x];
			while (parent[y] != y) y = parent[y];
			if (x != y) parent[x] = y;
		}
	}
	std::set<int> roots;
	int count = 0;
	for (size_t i = 0; i < nv; ++i) {
		if (!used[i]) continue;
		int x = (int)i;
		while (parent[x] != x) x = parent[x];
		if (roots.insert(x).second) ++count;
	}
	return count;
}

// Independent small QEM assembly from a set of triangles (only used for the
// objective group). Plane quadric is the outer product of (n, -n.p0).
struct QMat {
	double m[10];
	QMat() { for (int i = 0; i < 10; ++i) m[i] = 0; }
};

void add_plane(QMat &q, const vec3f &p0, const vec3f &p1, const vec3f &p2) {
	vec3f n;
	n.cross(p1 - p0, p2 - p0);
	n.normalize();
	double a = n.x, b = n.y, c = n.z, d = -(n.x * p0.x + n.y * p0.y + n.z * p0.z);
	double v[4] = {a, b, c, d};
	int idx = 0;
	for (int i = 0; i < 4; ++i)
		for (int j = i; j < 4; ++j) q.m[idx++] += v[i] * v[j];
}

void add_quadric(QMat &a, const QMat &b) { for (int i = 0; i < 10; ++i) a.m[i] += b.m[i]; }

double qerr(const QMat &q, const vec3f &x) {
	return q.m[0]*x.x*x.x + 2*q.m[1]*x.x*x.y + 2*q.m[2]*x.x*x.z + 2*q.m[3]*x.x
	     + q.m[4]*x.y*x.y + 2*q.m[5]*x.y*x.z + 2*q.m[6]*x.y
	     + q.m[7]*x.z*x.z + 2*q.m[8]*x.z + q.m[9];
}

// Solve min x^T A x + 2 b^T x (3x3 symmetric A) with a dense small solve; used
// only to cross check the implementation's joint minimum on well conditioned
// systems.
bool solve3(const double A[3][3], const double b[3], vec3f &x) {
	double det = A[0][0]*(A[1][1]*A[2][2]-A[1][2]*A[2][1])
	           - A[0][1]*(A[1][0]*A[2][2]-A[1][2]*A[2][0])
	           + A[0][2]*(A[1][0]*A[2][1]-A[1][1]*A[2][0]);
	if (std::fabs(det) < 1e-14) return false;
	x.x = -(b[0]*(A[1][1]*A[2][2]-A[1][2]*A[2][1]) - A[0][1]*(b[1]*A[2][2]-A[1][2]*b[2]) + A[0][2]*(b[1]*A[2][1]-A[1][1]*b[2])) / det;
	x.y = -(A[0][0]*(b[1]*A[2][2]-A[1][2]*b[2]) - b[0]*(A[1][0]*A[2][2]-A[1][2]*A[2][0]) + A[0][2]*(A[1][0]*b[2]-b[1]*A[2][0])) / det;
	x.z = -(A[0][0]*(A[1][1]*b[2]-b[1]*A[2][1]) - A[0][1]*(A[1][0]*b[2]-b[1]*A[2][0]) + b[0]*(A[1][0]*A[2][1]-A[1][1]*A[2][0])) / det;
	return std::isfinite(x.x) && std::isfinite(x.y) && std::isfinite(x.z);
}

bool near(const vec3f &a, const vec3f &b, double tol) { return (a - b).length() <= tol; }

SymmetryContext make_ctx(const Mesh &m, const std::vector<int> &mate,
                         const vec3f &normal, double offset, double tol) {
	SymmetryContext c = SymmetryContext();
	c.normal = normal;
	c.offset = offset;
	c.input_tolerance = tol;
	c.mate = mate;
	(void)m;
	return c;
}

// Collect the pre-call mesh so original_to_vertex can be checked against it.
struct Original {
	std::vector<vec3f> p;
	std::vector<Tri> f;
	std::vector<int> mate;
};

Original capture(const Mesh &m, const std::vector<int> &mate) {
	Original o;
	o.p = m.p;
	o.f = m.f;
	o.mate = mate;
	return o;
}

bool check_original_map(const Original &o, const MeshView &out,
                        const SymmetryContext &ctx, std::string &why) {
	if (ctx.original_to_vertex.size() != o.p.size()) { why = "map size"; return false; }
	// vertices referenced by surviving faces in the output
	size_t nv = out.p.size();
	std::vector<int> live(nv, 0);
	for (size_t i = 0; i < out.f.size(); ++i)
		for (int k = 0; k < 3; ++k) live[out.f[i].v[k]] = 1;
	// every mapping target must be a live output vertex, and each live output
	// vertex must be the representative of at least one pre-call vertex
	std::vector<int> representatives(nv, 0);
	for (size_t i = 0; i < o.p.size(); ++i) {
		int t = ctx.original_to_vertex[i];
		if (t < -1 || t >= (int)nv) { why = "map range"; return false; }
		if (t >= 0) {
			if (!live[t]) { why = "map to dead vertex"; return false; }
			++representatives[t];
		}
	}
	for (size_t j = 0; j < nv; ++j)
		if (live[j] && representatives[j] == 0) { why = "live vertex without source"; return false; }
	// symmetric mapping: mates are mapped together, or both dropped
	for (size_t i = 0; i < o.mate.size(); ++i) {
		int m = o.mate[i];
		int ti = ctx.original_to_vertex[i], tm = ctx.original_to_vertex[m];
		if ((ti < 0) != (tm < 0)) { why = "asymmetric dropped mate"; return false; }
		if (ti < 0) continue;
		if (ctx.mate[ti] != tm) { why = "map mate mismatch"; return false; }
	}
	return true;
}

// ---------------------------------------------------------------------------
// Shared fixtures

struct SlabCase {
	Mesh mesh;
	std::vector<int> mate;
	Original orig;
};

SlabCase slab(int n, double dx) {
	SlabCase c;
	c.mesh = make_symmetric_slab(n, dx);
	c.mate = slab_mate(c.mesh, n);
	c.orig = capture(c.mesh, c.mate);
	return c;
}

SlabCase pair_case(int n, double dx) {
	SlabCase c;
	c.mesh = make_mirrored_pair(n, dx);
	c.mate = pair_mate(c.mesh, n);
	c.orig = capture(c.mesh, c.mate);
	return c;
}

} // namespace

// ===========================================================================
// group: orbit
// ===========================================================================

static int orbit_ordinary_four_point() {
	SlabCase c = pair_case(6, 0.0);
	load_mesh(c.mesh);
	SymmetryContext ctx = make_ctx(c.mesh, c.mate, vec3f(1, 0, 0), 0.0, 1e-12);
	size_t before = triangles.size();
	try {
		simplify_mesh(8, 7.0, false, &ctx);
	} catch (const std::exception &) {
		TASK_CHECK(false);
	}
	MeshView out = snapshot();
	TASK_CHECK(out.f.size() < before);
	std::string why;
	TASK_CHECK(check_result(out, ctx, 1e-9, why));
	TASK_CHECK(check_fans(out, why));
	// the two halves must stay separate components and never merge
	TASK_CHECK(component_count(out) == 2);
	return 0;
}

static int orbit_shared_self_mate() {
	SlabCase c = slab(8, 0.0);
	load_mesh(c.mesh);
	SymmetryContext ctx = make_ctx(c.mesh, c.mate, vec3f(1, 0, 0), 0.0, 1e-12);
	int before = (int)triangles.size();
	try {
		simplify_mesh(6, 7.0, false, &ctx);
	} catch (const std::exception &) {
		TASK_CHECK(false);
	}
	MeshView out = snapshot();
	TASK_CHECK((int)out.f.size() < before);
	std::string why;
	TASK_CHECK(check_result(out, ctx, 1e-9, why));
	TASK_CHECK(check_fans(out, why));
	// seam vertices stay on the plane and are self mates
	bool sawSelf = false;
	for (size_t i = 0; i < out.p.size(); ++i)
		if (ctx.mate[i] == (int)i) {
			sawSelf = true;
			double d = out.p[i].x;
			TASK_CHECK(std::fabs(d) <= 1e-9);
		}
	TASK_CHECK(sawSelf);
	return 0;
}

static int orbit_edge_crossing_plane() {
	// A single open strip crossing the mirror plane x = 0. A left grid and the
	// mirrored right grid meet at x = 0 through a self-matching row of seam
	// vertices. Collapsing a seam vertex with an interior vertex forms an
	// orbit whose edge endpoints are mutual mates.
	Mesh m;
	int n = 3;
	std::vector<std::vector<int> > L(n + 1, std::vector<int>(n + 1));
	std::vector<std::vector<int> > R(n + 1, std::vector<int>(n + 1, -1));
	for (int j = 0; j <= n; ++j)
		for (int i = 0; i <= n; ++i) {
			L[j][i] = (int)m.p.size();
			m.p.push_back(vec3f(-0.5 * double(i) / n, double(j) / n, 0.0));
		}
	for (int j = 0; j <= n; ++j)
		for (int i = 1; i <= n; ++i) {
			R[j][i] = (int)m.p.size();
			m.p.push_back(vec3f(0.5 * double(i) / n, double(j) / n, 0.0));
		}
	// left half (i = 0..n-1, the x = 0 column is the seam)
	for (int j = 0; j < n; ++j)
		for (int i = 0; i < n; ++i) {
			int a = L[j][i], b = L[j][i + 1], c = L[j + 1][i], d = L[j + 1][i + 1];
			m.f.push_back(Tri{{a, b, d}});
			m.f.push_back(Tri{{a, d, c}});
		}
	// right half, reversed winding, the seam column shared with the left
	for (int j = 0; j < n; ++j)
		for (int i = 0; i < n; ++i) {
			int a = (i == 0) ? L[j][0] : R[j][i];
			int b = R[j][i + 1];
			int c = (i == 0) ? L[j + 1][0] : R[j + 1][i];
			int d = R[j + 1][i + 1];
			m.f.push_back(Tri{{a, d, b}});
			m.f.push_back(Tri{{a, c, d}});
		}
	std::vector<int> mate(m.p.size(), -1);
	for (int j = 0; j <= n; ++j)
		for (int i = 0; i <= n; ++i) {
			int l = L[j][i];
			int r = (i == 0) ? l : R[j][i];
			mate[l] = r;
			mate[r] = l;
		}
	load_mesh(m);
	SymmetryContext ctx = make_ctx(m, mate, vec3f(1, 0, 0), 0.0, 1e-12);
	Original orig = capture(m, mate);
	try {
		simplify_mesh(8, 7.0, false, &ctx);
	} catch (const std::exception &) {
		TASK_CHECK(false);
	}
	MeshView out = snapshot();
	std::string why;
	TASK_CHECK(check_result(out, ctx, 1e-9, why));
	TASK_CHECK(check_original_map(orig, out, ctx, why));
	// both sides remain
	bool pos = false, neg = false;
	for (size_t i = 0; i < out.p.size(); ++i) {
		if (out.p[i].x < -1e-10) neg = true;
		if (out.p[i].x > 1e-10) pos = true;
	}
	TASK_CHECK(pos && neg);
	return 0;
}

static int orbit_reject_one_side_flip() {
	// A ridge sloping down on the right half only: collapsing across it makes
	// the right side's retained faces flip while the mirrored left collapse is
	// perfectly legal. The whole orbit must be rejected, so the mesh must stay
	// untouched by that candidate.
	Mesh m;
	int n = 4;
	std::vector<std::vector<int> > L(n + 1, std::vector<int>(n + 1));
	std::vector<std::vector<int> > R(n + 1, std::vector<int>(n + 1, -1));
	for (int j = 0; j <= n; ++j)
		for (int i = 0; i <= n; ++i) {
			L[j][i] = (int)m.p.size();
			double u = double(i) / n;
			m.p.push_back(vec3f(-u, double(j) / n, -0.2 * u + 1.5 * u * u));
		}
	for (int j = 0; j <= n; ++j)
		for (int i = 1; i <= n; ++i) {
			R[j][i] = (int)m.p.size();
			double u = double(i) / n;
			m.p.push_back(vec3f(u, double(j) / n, -0.2 * u + 1.5 * u * u));
		}
	for (int j = 0; j < n; ++j)
		for (int i = 0; i < n; ++i) {
			int a = L[j][i], b = L[j][i + 1], c = L[j + 1][i], d = L[j + 1][i + 1];
			m.f.push_back(Tri{{a, b, d}});
			m.f.push_back(Tri{{a, d, c}});
		}
	for (int j = 0; j < n; ++j)
		for (int i = 0; i < n; ++i) {
			int a = (i == 0) ? L[j][0] : R[j][i];
			int b = R[j][i + 1];
			int c = (i == 0) ? L[j + 1][0] : R[j + 1][i];
			int d = R[j + 1][i + 1];
			m.f.push_back(Tri{{a, d, b}});
			m.f.push_back(Tri{{a, c, d}});
		}
	std::vector<int> mate(m.p.size(), -1);
	for (int j = 0; j <= n; ++j)
		for (int i = 0; i <= n; ++i) {
			int l = L[j][i];
			int r = (i == 0) ? l : R[j][i];
			mate[l] = r;
			mate[r] = l;
		}
	// Perturb the right half into a fold the mirror does not have. The
	// coordinates only mirror within the declared tolerance, which is exactly
	// the situation the input tolerance describes.
	for (int j = 0; j <= n; ++j)
		for (int i = 1; i <= n; ++i) m.p[R[j][i]].z -= 0.22 * double(i) / n;
	load_mesh(m);
	SymmetryContext ctx = make_ctx(m, mate, vec3f(1, 0, 0), 0.0, 0.25);
	Original orig = capture(m, mate);

	// Find at least one candidate where only one side would flip. Locate an
	// interior left edge whose joint solve is geometrically legal on the left.
	g_sym_ctx = &ctx;
	g_sym_cur_orig.resize(vertices.size());
	g_sym_uf.resize(vertices.size());
	for (size_t i = 0; i < vertices.size(); ++i) { g_sym_cur_orig[i] = (int)i; g_sym_uf[i] = (int)i; }
	update_mesh(0);
	int oneSided = 0;
	for (size_t f = 0; f < triangles.size(); ++f) {
		for (int j = 0; j < 3; ++j) {
			int a = triangles[f].v[j], b = triangles[f].v[(j + 1) % 3];
			SymOrbit o;
			if (!sym_build_orbit(a, b, o)) continue;
			SymSolution sol;
			if (!sym_solve_orbit(o, sol)) continue;
			bool f0 = false, f1 = false;
			int del = 0;
			// inspecting the candidate without committing: rejection paths must
			// not leave the mesh changed, so snapshot and verify
			MeshView before = snapshot();
			if (try_symmetry_collapse(a, b, del, &f0, &f1)) continue;
			MeshView after = snapshot();
			TASK_CHECK(before.f.size() == after.f.size());
			TASK_CHECK(before.p.size() == after.p.size());
			if (f0 != f1) ++oneSided;
		}
	}
	TASK_CHECK(oneSided > 0);
	g_sym_ctx = 0;

	// Run the real simplification; the result must still be a valid symmetric
	// decimation (no half-collapsed state).
	load_mesh(m);
	SymmetryContext ctx2 = make_ctx(m, mate, vec3f(1, 0, 0), 0.0, 0.25);
	int before = (int)triangles.size();
	try {
		simplify_mesh(8, 7.0, false, &ctx2);
	} catch (const std::exception &) {
		TASK_CHECK(false);
	}
	MeshView out = snapshot();
	TASK_CHECK((int)out.f.size() < before);
	std::string why;
	TASK_CHECK(check_result(out, ctx2, 1e-9, why));
	TASK_CHECK(check_original_map(orig, out, ctx2, why));
	return 0;
}

static int orbit_bad_input_untouched() {
	SlabCase c = slab(4, 0.0);
	load_mesh(c.mesh);
	MeshView meshBefore = snapshot();
	std::vector<int> mateBefore = c.mate;
	SymmetryContext ctx = make_ctx(c.mesh, c.mate, vec3f(1, 0, 0), 0.0, 1e-12);
	// corrupt: not an involution
	ctx.mate[0] = (ctx.mate[0] + 3) % (int)ctx.mate.size();
	std::vector<int> bad = ctx.mate;
	bool threw = false;
	try {
		simplify_mesh(8, 7.0, false, &ctx);
	} catch (const std::invalid_argument &) {
		threw = true;
	}
	TASK_CHECK(threw);
	MeshView meshAfter = snapshot();
	TASK_CHECK(meshAfter.f.size() == meshBefore.f.size());
	TASK_CHECK(meshAfter.p.size() == meshBefore.p.size());
	for (size_t i = 0; i < meshBefore.f.size(); ++i)
		for (int k = 0; k < 3; ++k) TASK_CHECK(meshAfter.f[i].v[k] == meshBefore.f[i].v[k]);
	TASK_CHECK(ctx.mate == bad);
	TASK_CHECK(ctx.original_to_vertex.empty());
	return 0;
}

static int orbit_noop_map() {
	// target above the triangle count: nothing collapses, the mapping is total
	SlabCase c = slab(3, 0.0);
	load_mesh(c.mesh);
	size_t nv = vertices.size();
	SymmetryContext ctx = make_ctx(c.mesh, c.mate, vec3f(1, 0, 0), 0.0, 1e-12);
	simplify_mesh((int)triangles.size() + 10, 7.0, false, &ctx);
	MeshView out = snapshot();
	TASK_CHECK(out.f.size() == c.mesh.f.size());
	TASK_CHECK(ctx.original_to_vertex.size() == nv);
	for (size_t i = 0; i < nv; ++i) {
		TASK_CHECK(ctx.original_to_vertex[i] >= 0);
		TASK_CHECK(ctx.original_to_vertex[i] == (int)i);
	}
	return 0;
}

// ===========================================================================
// group: objective
// ===========================================================================

// Independently assemble the joint ordinary-orbit system for a known pair of
// mirrored patches and compare the minimiser against an independently solved
// small linear system.
static int objective_ordinary_joint_minimum() {
	// two mirrored non-coplanar patches: a bent strip on the left and its
	// mirror on the right. The joint system is then well conditioned.
	Mesh m;
	int n = 3;
	std::vector<std::vector<int> > L(n + 1, std::vector<int>(n + 1));
	std::vector<std::vector<int> > R(n + 1, std::vector<int>(n + 1));
	for (int j = 0; j <= n; ++j)
		for (int i = 0; i <= n; ++i) {
			L[j][i] = (int)m.p.size();
			double u = double(i) / n, v = double(j) / n;
			m.p.push_back(vec3f(-1.0 - u, v, 0.5 * u * u + 0.7 * u * v + 0.4 * v * v));
		}
	for (int j = 0; j <= n; ++j)
		for (int i = 0; i <= n; ++i) {
			R[j][i] = (int)m.p.size();
			double u = double(i) / n, v = double(j) / n;
			m.p.push_back(vec3f(1.0 + u, v, 0.5 * u * u + 0.7 * u * v + 0.4 * v * v));
		}
	for (int j = 0; j < n; ++j)
		for (int i = 0; i < n; ++i) {
			int a = L[j][i], b = L[j][i + 1], c = L[j + 1][i], d = L[j + 1][i + 1];
			m.f.push_back(Tri{{a, b, d}});
			m.f.push_back(Tri{{a, d, c}});
			int ea = R[j][i], eb = R[j][i + 1], ec = R[j + 1][i], ed = R[j + 1][i + 1];
			m.f.push_back(Tri{{ea, ed, eb}});
			m.f.push_back(Tri{{ea, ec, ed}});
		}
	std::vector<int> mate(m.p.size(), -1);
	for (int j = 0; j <= n; ++j)
		for (int i = 0; i <= n; ++i) {
			int l = L[j][i], r = R[j][i];
			mate[l] = r;
			mate[r] = l;
		}
	load_mesh(m);
	SymmetryContext ctx = make_ctx(m, mate, vec3f(1, 0, 0), 0.0, 1e-12);
	g_sym_ctx = &ctx;
	g_sym_cur_orig.assign(vertices.size(), 0);
	g_sym_uf.assign(vertices.size(), 0);
	update_mesh(0);
	int a = -1, b = -1;
	for (size_t i = 0; i < triangles.size(); ++i) {
		int v0 = triangles[i].v[0], v1 = triangles[i].v[1];
		if (vertices[v0].p.x < -1.1 && vertices[v1].p.x < -1.1 &&
		    vertices[v0].p.y > 0.2 && vertices[v1].p.y > 0.2 &&
		    vertices[v0].p.y < 0.8 && vertices[v1].p.y < 0.8) { a = v0; b = v1; break; }
	}
	TASK_CHECK(a >= 0 && b >= 0);
	SymOrbit o;
	TASK_CHECK(sym_build_orbit(a, b, o));
	TASK_CHECK(o.nclusters == 2);
	SymSolution sol;
	TASK_CHECK(sym_solve_orbit(o, sol));

	// independent QEM: every member vertex collects the planes of its incident
	// faces (exactly what the per-vertex quadrics of the implementation are)
	QMat qa, qb;
	for (int m2 = 0; m2 < o.nmem; ++m2) {
		int v = o.mem[m2];
		for (size_t i = 0; i < triangles.size(); ++i) {
			bool incident = false;
			for (int k = 0; k < 3; ++k)
				if (triangles[i].v[k] == v) incident = true;
			if (!incident) continue;
			QMat q;
			add_plane(q, vertices[triangles[i].v[0]].p, vertices[triangles[i].v[1]].p,
			          vertices[triangles[i].v[2]].p);
			if (o.cl[m2] == 0) add_quadric(qa, q);
			else add_quadric(qb, q);
		}
	}
	// A = A1 + S^T A2 S, b = b1 + S^T b2 for S = I - 2 n n^T (offset 0)
	double A1[3][3], A2[3][3], b1[3], b2[3];
	double *qs[2] = {qa.m, qb.m};
	double (*As[2])[3] = {A1, A2};
	for (int s = 0; s < 2; ++s) {
		double *q = qs[s];
		double A[3][3] = {{q[0], q[1], q[2]}, {q[1], q[4], q[5]}, {q[2], q[5], q[7]}};
		for (int i = 0; i < 3; ++i)
			for (int j = 0; j < 3; ++j) As[s][i][j] = A[i][j];
	}
	b1[0] = qa.m[3]; b1[1] = qa.m[6]; b1[2] = qa.m[8];
	b2[0] = qb.m[3]; b2[1] = qb.m[6]; b2[2] = qb.m[8];
	double S[3][3];
	for (int i = 0; i < 3; ++i)
		for (int j = 0; j < 3; ++j) S[i][j] = (i == j ? 1.0 : 0.0) - 2.0 * (i == 0 && j == 0 ? 1.0 : 0.0);
	double Aj[3][3], bj[3];
	for (int i = 0; i < 3; ++i) {
		for (int j = 0; j < 3; ++j) {
			double acc = 0;
			for (int r = 0; r < 3; ++r)
				for (int col = 0; col < 3; ++col) acc += S[r][i] * A2[r][col] * S[col][j];
			Aj[i][j] = A1[i][j] + acc;
		}
		double acc = 0;
		for (int r = 0; r < 3; ++r) acc += S[r][i] * b2[r];
		bj[i] = b1[i] + acc;
	}
	vec3f expected;
	TASK_CHECK(solve3(Aj, bj, expected));
	TASK_CHECK(near(sol.pos[1], vec3f(-sol.pos[0].x, sol.pos[0].y, sol.pos[0].z), 1e-12));
	double cost = qerr(qa, sol.pos[0]) + qerr(qb, sol.pos[1]);
	TASK_CHECK(std::fabs(cost - sol.cost) <= 1e-9 * (1.0 + std::fabs(cost)));
	return 0;
}

// Singular system: both clusters' quadrics only constrain x, so the y/z
// direction is flat. The tie break must be the mirror side endpoint centre.
static int objective_singular_center() {
	Mesh m;
	m.p.push_back(vec3f(-1.0, 0.0, 0.0));
	m.p.push_back(vec3f(-1.0, 1.0, 0.0));
	m.p.push_back(vec3f(-0.4, 0.25, 0.0));
	m.p.push_back(vec3f(1.0, 0.0, 0.0));
	m.p.push_back(vec3f(1.0, 1.0, 0.0));
	m.p.push_back(vec3f(0.4, 0.25, 0.0));
	Tri t1 = {{0, 1, 2}}, t2 = {{3, 5, 4}};
	m.f.push_back(t1);
	m.f.push_back(t2);
	std::vector<int> mate(m.p.size(), -1);
	mate[0] = 3; mate[3] = 0;
	mate[1] = 4; mate[4] = 1;
	mate[2] = 5; mate[5] = 2;
	load_mesh(m);
	SymmetryContext ctx = make_ctx(m, mate, vec3f(1, 0, 0), 0.0, 1e-12);
	g_sym_ctx = &ctx;
	g_sym_cur_orig.assign(vertices.size(), 0);
	g_sym_uf.assign(vertices.size(), 0);
	update_mesh(0);
	SymOrbit o;
	TASK_CHECK(sym_build_orbit(0, 1, o));
	SymSolution sol;
	TASK_CHECK(sym_solve_orbit(o, sol));
	// the minimiser must lie at the (reflected) centre in the flat directions
	vec3f pa = m.p[0], pb = m.p[1];
	vec3f pc = m.p[3], pd = m.p[4];
	// the mirror-side endpoints are reflected onto the representative side
	// before the centre is formed
	vec3f centre = (pa + pb + sym_reflect_point(ctx, pc) + sym_reflect_point(ctx, pd)) * 0.25;
	// For a flat (singular) direction the minimiser must be the nearest point
	// to that centre, i.e. the centre itself on the flat direction.
	TASK_CHECK(std::fabs(sol.pos[0].y - centre.y) <= 1e-9);
	TASK_CHECK(std::fabs(sol.pos[0].x - centre.x) <= 1e-9);
	// self corresponding singular case: all vertices on one side, single point
	Mesh s;
	// a single planar triangle in the mirror plane: the joint QEM of the only
	// orbit is singular in both in-plane directions
	s.p.push_back(vec3f(0.0, 0.2, 0.1));
	s.p.push_back(vec3f(0.0, 0.8, 0.1));
	s.p.push_back(vec3f(0.0, 0.35, 0.7));
	Tri st = {{0, 1, 2}};
	s.f.push_back(st);
	std::vector<int> smate(3, 0);
	for (int i = 0; i < 3; ++i) smate[i] = i;
	load_mesh(s);
	SymmetryContext sctx = make_ctx(s, smate, vec3f(1, 0, 0), 0.0, 1e-12);
	g_sym_ctx = &sctx;
	g_sym_cur_orig.assign(vertices.size(), 0);
	g_sym_uf.assign(vertices.size(), 0);
	update_mesh(0);
	SymOrbit so;
	TASK_CHECK(sym_build_orbit(0, 1, so));
	TASK_CHECK(so.nclusters == 1);
	SymSolution ss;
	TASK_CHECK(sym_solve_orbit(so, ss));
	// The self-corresponding orbit contains the two distinct endpoints of the
	// candidate edge (both are self mates). The QEM of that single face is flat
	// on the plane, so the tie break must return the endpoint centre projected
	// onto the plane, i.e. x = 0 exactly.
	vec3f cen = (s.p[0] + s.p[1]) * 0.5;
	TASK_CHECK(std::fabs(ss.pos[0].x) <= 1e-12);
	TASK_CHECK(std::fabs(ss.pos[0].y - cen.y) <= 1e-9);
	TASK_CHECK(std::fabs(ss.pos[0].z - cen.z) <= 1e-9);
	return 0;
}

// Joint cost prefers a mirror-consistent position over independently averaged
// halves: run one orbit and compare with the naive average.
static int objective_joint_beats_average() {
	Mesh m;
	m.p.push_back(vec3f(-1.0, 0.0, 0.0));
	m.p.push_back(vec3f(-1.0, 1.0, 0.0));
	m.p.push_back(vec3f(-0.3, 0.5, 1.2));
	m.p.push_back(vec3f(1.0, 0.0, 0.0));
	m.p.push_back(vec3f(1.0, 1.0, 0.0));
	m.p.push_back(vec3f(0.3, 0.5, 1.2));
	m.f.push_back(Tri{{0, 1, 2}});
	m.f.push_back(Tri{{3, 5, 4}});
	std::vector<int> mate(6, -1);
	mate[0] = 3; mate[3] = 0;
	mate[1] = 4; mate[4] = 1;
	mate[2] = 5; mate[5] = 2;
	load_mesh(m);
	SymmetryContext ctx = make_ctx(m, mate, vec3f(1, 0, 0), 0.0, 1e-12);
	g_sym_ctx = &ctx;
	g_sym_cur_orig.assign(vertices.size(), 0);
	g_sym_uf.assign(vertices.size(), 0);
	update_mesh(0);
	SymOrbit o;
	TASK_CHECK(sym_build_orbit(0, 1, o));
	SymSolution sol;
	TASK_CHECK(sym_solve_orbit(o, sol));
	QMat qa, qb;
	add_plane(qa, m.p[0], m.p[1], m.p[2]);
	add_plane(qb, m.p[3], m.p[4], m.p[5]);
	double joint = qerr(qa, sol.pos[0]) + qerr(qb, sol.pos[1]);
	// naive: separate minimisers then average with the mirror
	vec3f p1 = (m.p[0] + m.p[1] + m.p[2]) * (1.0 / 3.0);
	vec3f p2 = (m.p[3] + m.p[4] + m.p[5]) * (1.0 / 3.0);
	vec3f avg((p1.x + (-(p2.x))) * 0.5, (p1.y + p2.y) * 0.5, (p1.z + p2.z) * 0.5);
	double naive = qerr(qa, avg) + qerr(qb, vec3f(-avg.x, avg.y, avg.z));
	TASK_CHECK(joint <= naive + 1e-12);
	return 0;
}

// Ordinary orbit: the reported scheduling cost equals the joint QEM at the
// reported positions (same solution used for both).
static int objective_cost_consistency() {
	SlabCase c = pair_case(6, 0.0);
	load_mesh(c.mesh);
	SymmetryContext ctx = make_ctx(c.mesh, c.mate, vec3f(1, 0, 0), 0.0, 1e-12);
	// pick a live edge in the middle of the left patch
	int a = -1, b = -1;
	for (size_t i = 0; i < triangles.size(); ++i) {
		int v0 = triangles[i].v[0], v1 = triangles[i].v[1];
		if (vertices[v0].p.x < -1.2 && vertices[v1].p.x < -1.2 &&
		    vertices[v0].p.y > 0.2 && vertices[v1].p.y > 0.2) { a = v0; b = v1; break; }
	}
	TASK_CHECK(a >= 0 && b >= 0);
	g_sym_ctx = &ctx;
	SymOrbit o;
	TASK_CHECK(sym_build_orbit(a, b, o));
	SymSolution sol;
	TASK_CHECK(sym_solve_orbit(o, sol));
	QMat qa, qb;
	for (int k = 0; k < 10; ++k) {
		qa.m[k] = 0;
		qb.m[k] = 0;
	}
	// accumulate from the original triangles of both clusters (the initial
	// quadrics are per original face plane)
	for (size_t i = 0; i < triangles.size(); ++i) {
		bool in0 = false, in1 = false;
		for (int k = 0; k < 3; ++k) {
			int v = triangles[i].v[k];
			if (v == o.mem[0] || v == o.mem[1]) in0 = true;
			if (v == o.mem[2] || v == o.mem[3]) in1 = true;
		}
		if (!in0 && !in1) continue;
		QMat q;
		add_plane(q, vertices[triangles[i].v[0]].p, vertices[triangles[i].v[1]].p, vertices[triangles[i].v[2]].p);
		if (in0) add_quadric(qa, q);
		if (in1) add_quadric(qb, q);
	}
	double cost = qerr(qa, sol.pos[0]) + qerr(qb, sol.pos[1]);
	TASK_CHECK(std::fabs(cost - sol.cost) <= 1e-8 * (1.0 + std::fabs(cost)));
	return 0;
}

// ===========================================================================
// group: sequence
// ===========================================================================

static int sequence_multi_round() {
	SlabCase c = slab(10, 0.0);
	load_mesh(c.mesh);
	SymmetryContext ctx = make_ctx(c.mesh, c.mate, vec3f(1, 0, 0), 0.0, 1e-12);
	int before = (int)triangles.size();
	simplify_mesh(12, 7.0, false, &ctx);
	MeshView out = snapshot();
	TASK_CHECK((int)out.f.size() <= 12);
	TASK_CHECK((int)out.f.size() < before / 4);
	std::string why;
	TASK_CHECK(check_result(out, ctx, 1e-9, why));
	TASK_CHECK(check_fans(out, why));
	TASK_CHECK(check_original_map(c.orig, out, ctx, why));
	return 0;
}

static int sequence_periodic_compaction() {
	// run enough rounds that update_mesh compacts in the middle and the end
	SlabCase c = slab(12, 0.0);
	load_mesh(c.mesh);
	SymmetryContext ctx = make_ctx(c.mesh, c.mate, vec3f(1, 0, 0), 0.0, 1e-12);
	simplify_mesh(20, 7.0, false, &ctx);
	MeshView out = snapshot();
	std::string why;
	TASK_CHECK(check_result(out, ctx, 1e-9, why));
	TASK_CHECK(check_fans(out, why));
	TASK_CHECK(ctx.mate.size() == out.p.size());
	TASK_CHECK(ctx.original_to_vertex.size() == c.orig.p.size());
	// mapping indices always point into the compacted mesh
	for (size_t i = 0; i < ctx.original_to_vertex.size(); ++i) {
		int t = ctx.original_to_vertex[i];
		TASK_CHECK(t == -1 || (t >= 0 && (size_t)t < out.p.size()));
	}
	return 0;
}

static int sequence_lossless() {
	SlabCase c = slab(8, 0.0);
	load_mesh(c.mesh);
	SymmetryContext ctx = make_ctx(c.mesh, c.mate, vec3f(1, 0, 0), 0.0, 1e-12);
	int before = (int)triangles.size();
	simplify_mesh_lossless(false, &ctx);
	MeshView out = snapshot();
	TASK_CHECK((int)out.f.size() < before);
	std::string why;
	TASK_CHECK(check_result(out, ctx, 1e-9, why));
	TASK_CHECK(check_fans(out, why));
	TASK_CHECK(check_original_map(c.orig, out, ctx, why));
	return 0;
}

static int sequence_second_model() {
	// one process, two different models; the second run must not reuse state
	{
		SlabCase c = slab(6, 0.0);
		load_mesh(c.mesh);
		SymmetryContext ctx = make_ctx(c.mesh, c.mate, vec3f(1, 0, 0), 0.0, 1e-12);
		simplify_mesh(10, 7.0, false, &ctx);
		MeshView out = snapshot();
		std::string why;
		TASK_CHECK(check_result(out, ctx, 1e-9, why));
	}
	SlabCase d = pair_case(5, 0.0);
	load_mesh(d.mesh);
	SymmetryContext ctx = make_ctx(d.mesh, d.mate, vec3f(1, 0, 0), 0.0, 1e-12);
	size_t before = triangles.size();
	simplify_mesh(8, 7.0, false, &ctx);
	MeshView out = snapshot();
	TASK_CHECK(out.f.size() < before);
	std::string why;
	TASK_CHECK(check_result(out, ctx, 1e-9, why));
	TASK_CHECK(component_count(out) == 2);
	return 0;
}

static int sequence_toleranced_input() {
	// the right half is displaced by 1e-5, within the tolerance
	SlabCase c = slab(8, 1e-5);
	load_mesh(c.mesh);
	SymmetryContext ctx = make_ctx(c.mesh, c.mate, vec3f(1, 0, 0), 0.0, 2e-5);
	int before = (int)triangles.size();
	simplify_mesh(8, 7.0, false, &ctx);
	MeshView out = snapshot();
	TASK_CHECK((int)out.f.size() < before);
	std::string why;
	// the result must be symmetric from now on, tighter than the input
	TASK_CHECK(check_result(out, ctx, 1e-9, why));
	TASK_CHECK(check_fans(out, why));
	return 0;
}

static int sequence_empty_mesh() {
	reset_mesh();
	SymmetryContext ctx;
	ctx.normal = vec3f(0, 1, 0);
	ctx.offset = 0.0;
	ctx.input_tolerance = 1e-9;
	bool ok = true;
	try {
		simplify_mesh(0, 7.0, false, &ctx);
		simplify_mesh_lossless(false, &ctx);
	} catch (const std::exception &) {
		ok = false;
	}
	TASK_CHECK(ok);
	TASK_CHECK(triangles.empty());
	TASK_CHECK(ctx.original_to_vertex.empty());
	return 0;
}

// ===========================================================================
// group: compatibility
// ===========================================================================

static int compat_legacy_signature() {
	// the old calls still compile and keep working
	Mesh m = make_symmetric_slab(4, 0.0);
	load_mesh(m);
	size_t before = triangles.size();
	simplify_mesh(8);
	TASK_CHECK(triangles.size() < before);
	TASK_CHECK(!triangles.empty());
	return 0;
}

static int compat_legacy_lossless() {
	Mesh m = make_symmetric_slab(4, 0.0);
	load_mesh(m);
	size_t before = triangles.size();
	simplify_mesh_lossless();
	TASK_CHECK(triangles.size() < before);
	return 0;
}

static int compat_invalid_normal() {
	std::vector<const char *> reasons;
	reasons.push_back("not unit");
	SlabCase c = slab(4, 0.0);
	load_mesh(c.mesh);
	std::vector<int> bad = c.mate;
	SymmetryContext ctx = make_ctx(c.mesh, bad, vec3f(2, 0, 0), 0.0, 1e-9);
	bool threw = false;
	try { simplify_mesh(8, 7.0, false, &ctx); }
	catch (const std::invalid_argument &) { threw = true; }
	TASK_CHECK(threw);
	// mate untouched, mesh untouched, output mapping untouched
	TASK_CHECK(ctx.mate == bad);
	TASK_CHECK(triangles.size() == c.mesh.f.size());
	TASK_CHECK(vertices.size() == c.mesh.p.size());
	TASK_CHECK(ctx.original_to_vertex.empty());
	return 0;
}

static int compat_invalid_tolerance() {
	SlabCase c = slab(4, 0.0);
	load_mesh(c.mesh);
	std::vector<int> bad = c.mate;
	SymmetryContext ctx = make_ctx(c.mesh, bad, vec3f(1, 0, 0), 0.0, -1.0);
	bool threw = false;
	try { simplify_mesh(8, 7.0, false, &ctx); }
	catch (const std::invalid_argument &) { threw = true; }
	TASK_CHECK(threw);
	TASK_CHECK(ctx.mate == bad);
	ctx.input_tolerance = std::numeric_limits<double>::quiet_NaN();
	threw = false;
	try { simplify_mesh(8, 7.0, false, &ctx); }
	catch (const std::invalid_argument &) { threw = true; }
	TASK_CHECK(threw);
	ctx.input_tolerance = std::numeric_limits<double>::infinity();
	threw = false;
	try { simplify_mesh(8, 7.0, false, &ctx); }
	catch (const std::invalid_argument &) { threw = true; }
	TASK_CHECK(threw);
	return 0;
}

static int compat_tolerance_violation() {
	// displace one vertex beyond the stated tolerance
	Mesh m = make_symmetric_slab(4, 0.0);
	int n = 4;
	int leftCount = (n + 1) * (n + 1);
	m.p[leftCount + 3].x += 1e-3; // right half i=3
	std::vector<int> mate = slab_mate(m, n);
	load_mesh(m);
	SymmetryContext ctx = make_ctx(m, mate, vec3f(1, 0, 0), 0.0, 1e-6);
	std::vector<int> bad = mate;
	bool threw = false;
	try { simplify_mesh(8, 7.0, false, &ctx); }
	catch (const std::invalid_argument &) { threw = true; }
	TASK_CHECK(threw);
	TASK_CHECK(ctx.mate == bad);
	return 0;
}

static int compat_unpaired_faces() {
	// delete one triangle so its mirror image has no partner
	Mesh m = make_symmetric_slab(4, 0.0);
	m.f.erase(m.f.begin() + 5);
	std::vector<int> mate = slab_mate(m, 4);
	load_mesh(m);
	SymmetryContext ctx = make_ctx(m, mate, vec3f(1, 0, 0), 0.0, 1e-9);
	std::vector<int> bad = mate;
	bool threw = false;
	try { simplify_mesh(8, 7.0, false, &ctx); }
	catch (const std::invalid_argument &) { threw = true; }
	TASK_CHECK(threw);
	TASK_CHECK(ctx.mate == bad);
	TASK_CHECK(triangles.size() == m.f.size());
	return 0;
}

static int compat_offset_plane() {
	// mirror plane x = 2 with a mesh built around it
	SlabCase c = slab(6, 0.0);
	for (size_t i = 0; i < c.mesh.p.size(); ++i) c.mesh.p[i].x += 2.0;
	load_mesh(c.mesh);
	SymmetryContext ctx = make_ctx(c.mesh, c.mate, vec3f(1, 0, 0), -2.0, 1e-9);
	int before = (int)triangles.size();
	simplify_mesh(8, 7.0, false, &ctx);
	MeshView out = snapshot();
	TASK_CHECK((int)out.f.size() < before);
	std::string why;
	TASK_CHECK(check_result(out, ctx, 1e-9, why));
	// self mates sit exactly on x = 2
	for (size_t i = 0; i < out.p.size(); ++i)
		if (ctx.mate[i] == (int)i)
			TASK_CHECK(std::fabs(out.p[i].x - 2.0) <= 1e-9);
	return 0;
}

static int compat_uv_and_material() {
	SlabCase c = slab(6, 0.0);
	load_mesh(c.mesh);
	// assign a material per triangle parity and a per-corner UV
	for (size_t i = 0; i < triangles.size(); ++i) {
		triangles[i].material = (int)(i % 3);
		triangles[i].attr = TEXCOORD;
		for (int k = 0; k < 3; ++k) {
			const vec3f &p = vertices[triangles[i].v[k]].p;
			triangles[i].uvs[k] = vec3f(p.y * 2.0 + 0.1, p.x * 3.0, 0.0);
		}
	}
	SymmetryContext ctx = make_ctx(c.mesh, c.mate, vec3f(1, 0, 0), 0.0, 1e-12);
	simplify_mesh(8, 7.0, false, &ctx);
	TASK_CHECK(!triangles.empty());
	for (size_t i = 0; i < triangles.size(); ++i) {
		TASK_CHECK(triangles[i].material >= 0 && triangles[i].material < 3);
		TASK_CHECK((triangles[i].attr & TEXCOORD) == TEXCOORD);
		for (int k = 0; k < 3; ++k) {
			TASK_CHECK(std::isfinite(triangles[i].uvs[k].x));
			TASK_CHECK(std::isfinite(triangles[i].uvs[k].y));
		}
	}
	return 0;
}

static int compat_boundary_loops() {
	// two separate open patches, one boundary loop each; collapsing must not
	// open extra holes, weld the loops shut or join the two components
	SlabCase c = pair_case(6, 0.0);
	load_mesh(c.mesh);
	SymmetryContext ctx = make_ctx(c.mesh, c.mate, vec3f(1, 0, 0), 0.0, 1e-12);
	MeshView in = snapshot();
	std::vector<std::vector<int> > loopsIn = boundary_loops(in);
	size_t loopsInCount = loopsIn.size();
	TASK_CHECK(loopsInCount >= 2);
	simplify_mesh(12, 7.0, false, &ctx);
	MeshView out = snapshot();
	std::vector<std::vector<int> > loopsOut = boundary_loops(out);
	TASK_CHECK(loopsOut.size() == loopsInCount);
	// each boundary loop must remain a simple cycle (no vertex twice)
	for (size_t l = 0; l < loopsOut.size(); ++l) {
		std::set<int> seen;
		for (size_t k = 0; k < loopsOut[l].size(); ++k)
			TASK_CHECK(seen.insert(loopsOut[l][k]).second);
		TASK_CHECK(loopsOut[l].size() >= 3);
	}
	// the loops must still live on the two halves, not merge across the seam
	int negLoop = 0, posLoop = 0;
	for (size_t l = 0; l < loopsOut.size(); ++l) {
		double sum = 0;
		for (size_t k = 0; k < loopsOut[l].size(); ++k) sum += out.p[loopsOut[l][k]].x;
		if (sum < 0) ++negLoop; else ++posLoop;
	}
	TASK_CHECK(negLoop >= 1 && posLoop >= 1);
	TASK_CHECK(negLoop + posLoop == (int)loopsOut.size());
	std::string why;
	TASK_CHECK(check_result(out, ctx, 1e-9, why));
	return 0;
}

static int compat_dense_both_sides() {
	// fine mesh: both halves and the plane neighbourhood must really decimate
	SlabCase c = slab(16, 0.0);
	load_mesh(c.mesh);
	SymmetryContext ctx = make_ctx(c.mesh, c.mate, vec3f(1, 0, 0), 0.0, 1e-12);
	int before = (int)triangles.size();
	// count initial faces near the plane
	int nearBefore = 0;
	for (size_t i = 0; i < triangles.size(); ++i) {
		bool nearPlane = false;
		for (int k = 0; k < 3; ++k)
			if (std::fabs(vertices[triangles[i].v[k]].p.x) < 0.15) nearPlane = true;
		if (nearPlane) ++nearBefore;
	}
	TASK_CHECK(nearBefore > 0);
	simplify_mesh(before / 8, 7.0, false, &ctx);
	MeshView out = snapshot();
	TASK_CHECK((int)out.f.size() <= before / 8 + 4);
	std::string why;
	TASK_CHECK(check_result(out, ctx, 1e-9, why));
	// both halves shrank
	int left = 0, right = 0;
	for (size_t i = 0; i < out.p.size(); ++i) {
		if (out.p[i].x < -1e-9) ++left;
		else if (out.p[i].x > 1e-9) ++right;
	}
	TASK_CHECK(left >= 2 && right >= 2);
	TASK_CHECK(left == right);
	return 0;
}

// Non-axis mirror plane: a planar triangle in the plane is a self
// corresponding orbit with both in-plane directions flat. The tie break must
// return the endpoint centre (already on the plane), not a displaced point.
static int objective_oblique_plane_center() {
	Mesh m;
	vec3f n(1.0, 2.0, 3.0);
	n.normalize();
	double off = -0.5;
	vec3f e1(0, 1, 0), e2;
	e1.cross(n, vec3f(0, 1, 0));
	e1.normalize();
	e2.cross(n, e1);
	e2.normalize();
	vec3f p0 = n * (-off);
	m.p.push_back(p0);
	m.p.push_back(p0 + e1 * 1.0);
	m.p.push_back(p0 + e2 * 1.0);
	m.f.push_back(Tri{{0, 1, 2}});
	std::vector<int> mate(3, 0);
	for (int i = 0; i < 3; ++i) mate[i] = i;
	load_mesh(m);
	SymmetryContext ctx = make_ctx(m, mate, n, off, 1e-9);
	g_sym_ctx = &ctx;
	g_sym_cur_orig.assign(vertices.size(), 0);
	g_sym_uf.assign(vertices.size(), 0);
	update_mesh(0);
	SymOrbit o;
	TASK_CHECK(sym_build_orbit(0, 1, o));
	TASK_CHECK(o.nclusters == 1);
	SymSolution sol;
	TASK_CHECK(sym_solve_orbit(o, sol));
	vec3f centre = (m.p[0] + m.p[1]) * 0.5;
	double nd = n.x * centre.x + n.y * centre.y + n.z * centre.z + off;
	vec3f projected(centre.x - nd * n.x, centre.y - nd * n.y, centre.z - nd * n.z);
	TASK_CHECK(near(sol.pos[0], projected, 1e-9));
	TASK_CHECK(std::fabs(n.x * sol.pos[0].x + n.y * sol.pos[0].y + n.z * sol.pos[0].z + off) <= 1e-12);
	return 0;
}

// A candidate whose orbit mixes a self mate with an ordinary pair: two
// adjacent seam vertices collapsing must commit both clusters and keep every
// surviving seam vertex exactly on the plane.
static int sequence_mixed_orbit() {
	SlabCase c = slab(6, 0.0);
	load_mesh(c.mesh);
	SymmetryContext ctx = make_ctx(c.mesh, c.mate, vec3f(1, 0, 0), 0.0, 1e-12);
	int before = (int)triangles.size();
	simplify_mesh(before / 4, 7.0, false, &ctx);
	MeshView out = snapshot();
	std::string why;
	TASK_CHECK((int)out.f.size() < before);
	TASK_CHECK(check_result(out, ctx, 1e-9, why));
	TASK_CHECK(check_fans(out, why));
	// every self mate is on the plane and ordinary pairs mirror exactly
	int selfCount = 0, pairCount = 0;
	for (size_t i = 0; i < out.p.size(); ++i) {
		int m = ctx.mate[i];
		if (m == (int)i) {
			++selfCount;
			TASK_CHECK(std::fabs(out.p[i].x) <= 1e-9);
		} else if ((int)i < m) {
			++pairCount;
			TASK_CHECK(std::fabs(out.p[i].x + out.p[m].x) <= 1e-9);
			TASK_CHECK(std::fabs(out.p[i].y - out.p[m].y) <= 1e-9);
			TASK_CHECK(std::fabs(out.p[i].z - out.p[m].z) <= 1e-9);
		}
	}
	TASK_CHECK(selfCount > 0 && pairCount > 0);
	return 0;
}

// ===========================================================================
// registration
// ===========================================================================

const struct task_case task_cases[] = {
	{"orbit", "ordinary_four_point", orbit_ordinary_four_point},
	{"orbit", "shared_self_mate", orbit_shared_self_mate},
	{"orbit", "edge_crossing_plane", orbit_edge_crossing_plane},
	{"orbit", "reject_one_side_flip", orbit_reject_one_side_flip},
	{"orbit", "bad_input_untouched", orbit_bad_input_untouched},
	{"orbit", "noop_map", orbit_noop_map},
	{"objective", "ordinary_joint_minimum", objective_ordinary_joint_minimum},
	{"objective", "singular_center", objective_singular_center},
	{"objective", "joint_beats_average", objective_joint_beats_average},
	{"objective", "cost_consistency", objective_cost_consistency},
	{"objective", "oblique_plane_center", objective_oblique_plane_center},
	{"sequence", "multi_round", sequence_multi_round},
	{"sequence", "periodic_compaction", sequence_periodic_compaction},
	{"sequence", "lossless", sequence_lossless},
	{"sequence", "second_model", sequence_second_model},
	{"sequence", "toleranced_input", sequence_toleranced_input},
	{"sequence", "empty_mesh", sequence_empty_mesh},
	{"sequence", "mixed_orbit", sequence_mixed_orbit},
	{"compatibility", "legacy_signature", compat_legacy_signature},
	{"compatibility", "legacy_lossless", compat_legacy_lossless},
	{"compatibility", "invalid_normal", compat_invalid_normal},
	{"compatibility", "invalid_tolerance", compat_invalid_tolerance},
	{"compatibility", "tolerance_violation", compat_tolerance_violation},
	{"compatibility", "unpaired_faces", compat_unpaired_faces},
	{"compatibility", "offset_plane", compat_offset_plane},
	{"compatibility", "uv_and_material", compat_uv_and_material},
	{"compatibility", "boundary_loops", compat_boundary_loops},
	{"compatibility", "dense_both_sides", compat_dense_both_sides},
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
