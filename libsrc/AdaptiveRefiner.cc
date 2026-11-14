/*
AdaptiveRefiner.cc
Persistent, locally adaptive red refinement of a triangle mesh.

See AdaptiveRefiner.h for the public contract.
*/

#include "AdaptiveRefiner.h"

#include <algorithm>
#include <cmath>
#include <queue>
#include <stdexcept>

using namespace std;

namespace trimesh {

const AdaptiveRefiner::NodeId AdaptiveRefiner::INVALID_NODE =
	~(AdaptiveRefiner::NodeId) 0;

AdaptiveRefiner::AdaptiveRefiner(const TriMesh &mesh)
	: original_vertex_count_(0),
	  has_colors_(false),
	  has_confidences_(false)
{
	build_from(mesh);
}


// ---------------------------------------------------------------------------
// Construction and input validation
// ---------------------------------------------------------------------------

static bool finite_point(const point &p)
{
	return std::isfinite((double) p[0]) &&
	       std::isfinite((double) p[1]) &&
	       std::isfinite((double) p[2]);
}

static double triangle_area2(const point &a, const point &b, const point &c)
{
	double ux = (double) b[0] - a[0];
	double uy = (double) b[1] - a[1];
	double uz = (double) b[2] - a[2];
	double vx = (double) c[0] - a[0];
	double vy = (double) c[1] - a[1];
	double vz = (double) c[2] - a[2];
	double nx = uy * vz - uz * vy;
	double ny = uz * vx - ux * vz;
	double nz = ux * vy - uy * vx;
	return nx * nx + ny * ny + nz * nz;
}

void AdaptiveRefiner::build_from(const TriMesh &mesh)
{
	const size_t nv = mesh.vertices.size();
	const size_t nf = mesh.faces.size();

	if (!mesh.colors.empty() && mesh.colors.size() != nv)
		throw std::invalid_argument(
			"AdaptiveRefiner: colors must be empty or match vertices");
	if (!mesh.confidences.empty() && mesh.confidences.size() != nv)
		throw std::invalid_argument(
			"AdaptiveRefiner: confidences must be empty or match vertices");

	has_colors_ = !mesh.colors.empty();
	has_confidences_ = !mesh.confidences.empty();

	for (size_t i = 0; i < nv; i++) {
		if (!finite_point(mesh.vertices[i]))
			throw std::invalid_argument(
				"AdaptiveRefiner: non-finite vertex position");
	}
	for (size_t i = 0; i < nf; i++) {
		const TriMesh::Face &f = mesh.faces[i];
		for (int j = 0; j < 3; j++) {
			if (f[j] < 0 || (size_t) f[j] >= nv)
				throw std::invalid_argument(
					"AdaptiveRefiner: face index out of range");
		}
		if (f[0] == f[1] || f[1] == f[2] || f[2] == f[0] ||
		    triangle_area2(mesh.vertices[f[0]], mesh.vertices[f[1]],
		                   mesh.vertices[f[2]]) == 0.0)
			throw std::invalid_argument(
				"AdaptiveRefiner: degenerate face");
	}

	// Deep copy all data that later operations read.  The source mesh can be
	// modified or destroyed afterwards.
	pverts_.resize(nv);
	for (size_t i = 0; i < nv; i++) {
		pverts_[i].pos = dvec3((double) mesh.vertices[i][0],
		                       (double) mesh.vertices[i][1],
		                       (double) mesh.vertices[i][2]);
		if (has_colors_)
			pverts_[i].col = dvec3((double) mesh.colors[i][0],
			                       (double) mesh.colors[i][1],
			                       (double) mesh.colors[i][2]);
		if (has_confidences_)
			pverts_[i].conf = (double) mesh.confidences[i];
	}
	original_vertex_count_ = nv;
	midpoint_parent_edge_.assign(nv, std::make_pair(-1, -1));

	nodes_.clear();
	leaves_.clear();
	nodes_.resize(nf);
	for (size_t i = 0; i < nf; i++) {
		Node &n = nodes_[i];
		n.parent = INVALID_NODE;
		n.root = (NodeId) i;
		n.level = 0;
		n.v[0] = mesh.faces[i][0];
		n.v[1] = mesh.faces[i][1];
		n.v[2] = mesh.faces[i][2];
		n.subdivided = false;
		leaves_.insert((NodeId) i);
	}

	validate_topology();
}

void AdaptiveRefiner::validate_topology() const
{
	const size_t nv = original_vertex_count_;
	const size_t nf = nodes_.size();

	struct EdgeRec {
		int count;
		int dir;
		std::vector<int> faces;
		EdgeRec() : count(0), dir(0) {}
	};
	std::map<std::pair<int, int>, EdgeRec> edges;
	std::vector<std::vector<std::pair<int, int> > > vertex_links(nv);
	std::vector<std::vector<int> > vertex_faces(nv);

	for (size_t i = 0; i < nf; i++) {
		const Node &n = nodes_[i];
		for (int j = 0; j < 3; j++)
			vertex_faces[n.v[j]].push_back((int) i);
		for (int j = 0; j < 3; j++) {
			int a = n.v[j];
			int b = n.v[NEXT_MOD3(j)];
			EdgeRec &rec = edges[edge_key(a, b)];
			rec.count++;
			rec.dir += (a < b) ? 1 : -1;
			rec.faces.push_back((int) i);
		}
	}

	for (std::map<std::pair<int, int>, EdgeRec>::const_iterator it =
			edges.begin(); it != edges.end(); ++it) {
		const EdgeRec &rec = it->second;
		if (rec.count > 2)
			throw std::invalid_argument(
				"AdaptiveRefiner: non-manifold edge");
		if (rec.count == 2) {
			if (rec.dir != 0)
				throw std::invalid_argument(
					"AdaptiveRefiner: inconsistent face orientation");
			int f1 = rec.faces[0], f2 = rec.faces[1];
			vertex_links[it->first.first].push_back(
				std::make_pair(f1, f2));
			vertex_links[it->first.second].push_back(
				std::make_pair(f1, f2));
		}
	}

	// Each vertex must have a single fan of incident faces.
	std::vector<int> mark(nf, -1);
	for (size_t v = 0; v < nv; v++) {
		if (vertex_faces[v].empty())
			continue;
		const std::vector<std::pair<int, int> > &links = vertex_links[v];
		std::queue<int> q;
		int start = vertex_faces[v][0];
		int seen = 1;
		mark[start] = (int) v;
		q.push(start);
		while (!q.empty()) {
			int f = q.front();
			q.pop();
			for (size_t k = 0; k < links.size(); k++) {
				int g = -1;
				if (links[k].first == f)
					g = links[k].second;
				else if (links[k].second == f)
					g = links[k].first;
				if (g < 0 || mark[g] == (int) v)
					continue;
				mark[g] = (int) v;
				seen++;
				q.push(g);
			}
		}
		if ((size_t) seen != vertex_faces[v].size())
			throw std::invalid_argument(
				"AdaptiveRefiner: non-manifold vertex");
	}
}


// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

bool AdaptiveRefiner::positive_edge(int a, int b) const
{
	const dvec3 &pa = pverts_[a].pos;
	const dvec3 &pb = pverts_[b].pos;
	double dx = pa[0] - pb[0];
	double dy = pa[1] - pb[1];
	double dz = pa[2] - pb[2];
	return dx * dx + dy * dy + dz * dz > 0.0;
}

int AdaptiveRefiner::midpoint_of(int a, int b) const
{
	std::map<std::pair<int, int>, int>::const_iterator it =
		edge_midpoint_.find(edge_key(a, b));
	return (it == edge_midpoint_.end()) ? -1 : it->second;
}

void AdaptiveRefiner::create_midpoint(int a, int b)
{
	std::pair<int, int> key = edge_key(a, b);
	if (edge_midpoint_.find(key) != edge_midpoint_.end())
		return;

	PVertex pv;
	pv.pos = 0.5 * (pverts_[a].pos + pverts_[b].pos);
	if (has_colors_)
		pv.col = 0.5 * (pverts_[a].col + pverts_[b].col);
	if (has_confidences_)
		pv.conf = 0.5 * (pverts_[a].conf + pverts_[b].conf);

	int id = (int) pverts_.size();
	pverts_.push_back(pv);
	midpoint_parent_edge_.push_back(key);
	edge_midpoint_[key] = id;
}

void AdaptiveRefiner::allocate_children(NodeId parent)
{
	if (nodes_[parent].subdivided)
		return;

	// Copy everything needed before nodes_ can be reallocated by resize().
	const int pv0 = nodes_[parent].v[0];
	const int pv1 = nodes_[parent].v[1];
	const int pv2 = nodes_[parent].v[2];
	const int level = nodes_[parent].level;
	const NodeId root = nodes_[parent].root;

	int m01 = midpoint_of(pv0, pv1);
	int m12 = midpoint_of(pv1, pv2);
	int m20 = midpoint_of(pv2, pv0);
	if (m01 < 0 || m12 < 0 || m20 < 0)
		throw std::logic_error("AdaptiveRefiner: missing midpoint");

	int cv[4][3];
	cv[0][0] = pv0; cv[0][1] = m01; cv[0][2] = m20;
	cv[1][0] = pv1; cv[1][1] = m12; cv[1][2] = m01;
	cv[2][0] = pv2; cv[2][1] = m20; cv[2][2] = m12;
	cv[3][0] = m01; cv[3][1] = m12; cv[3][2] = m20;

	NodeId first = (NodeId) nodes_.size();
	nodes_.resize(nodes_.size() + 4);

	Node &n = nodes_[parent];
	n.subdivided = true;
	n.child[0] = first;
	n.child[1] = first + 1;
	n.child[2] = first + 2;
	n.child[3] = first + 3;

	for (int k = 0; k < 4; k++) {
		Node &c = nodes_[first + k];
		c.parent = parent;
		c.root = root;
		c.level = level + 1;
		c.v[0] = cv[k][0];
		c.v[1] = cv[k][1];
		c.v[2] = cv[k][2];
		c.subdivided = false;
	}
}

void AdaptiveRefiner::reactivate_children(NodeId parent)
{
	Node &n = nodes_[parent];
	leaves_.erase(parent);
	for (int k = 0; k < 4; k++)
		leaves_.insert(n.child[k]);
}


// ---------------------------------------------------------------------------
// Topological 1D views of the current red-leaf set
// ---------------------------------------------------------------------------

bool AdaptiveRefiner::interval_contains(const LeafSeg &outer,
                                        const LeafSeg &inner)
{
	return outer.lo <= inner.lo + 1e-12 &&
	       inner.hi <= outer.hi + 1e-12 &&
	       (outer.hi - outer.lo) > (inner.hi - inner.lo) + 1e-12;
}

double AdaptiveRefiner::overlap_length(const LeafSeg &a, const LeafSeg &b)
{
	double lo = std::max(a.lo, b.lo);
	double hi = std::min(a.hi, b.hi);
	return hi - lo;
}

// Dyadic position of persistent vertex x along the line from root_a to
// root_b.  Returns false if x does not lie on that line.  All values are
// multiples of 2^-MAX_LEVEL and therefore exact in double.
bool AdaptiveRefiner::line_coord(int x, int root_a, int root_b,
                                 int depth, double &out) const
{
	if (x == root_a) {
		out = 0.0;
		return true;
	}
	if (x == root_b) {
		out = 1.0;
		return true;
	}
	if (!is_midpoint_vertex(x) || depth > MAX_LEVEL + 1)
		return false;
	const std::pair<int, int> &pe = midpoint_parent_edge_[x];
	if (pe.first < 0)
		return false;
	double a, b;
	if (!line_coord(pe.first, root_a, root_b, depth + 1, a) ||
	    !line_coord(pe.second, root_a, root_b, depth + 1, b))
		return false;
	out = 0.5 * (a + b);
	return true;
}

// Map a persistent segment (a,b) to the maximal segment containing it and to
// its dyadic interval inside that maximal segment.
bool AdaptiveRefiner::canonical_interval(int a, int b,
	std::pair<int, int> &root, double &lo, double &hi) const
{
	int x = a, y = b;
	// Climb towards the coarsest segment of the topological edge line.
	while (true) {
		bool climbed = false;
		if (is_midpoint_vertex(x)) {
			const std::pair<int, int> &pe = midpoint_parent_edge_[x];
			if (y == pe.first || y == pe.second) {
				x = pe.first;
				y = pe.second;
				climbed = true;
			}
		}
		if (!climbed && is_midpoint_vertex(y)) {
			const std::pair<int, int> &pe = midpoint_parent_edge_[y];
			if (x == pe.first || x == pe.second) {
				x = pe.first;
				y = pe.second;
				climbed = true;
			}
		}
		if (!climbed)
			break;
	}

	root = edge_key(x, y);
	double ca, cb;
	if (!line_coord(a, root.first, root.second, 0, ca) ||
	    !line_coord(b, root.first, root.second, 0, cb))
		return false;
	lo = std::min(ca, cb);
	hi = std::max(ca, cb);
	return hi > lo;
}

AdaptiveRefiner::LineMap AdaptiveRefiner::build_lines() const
{
	LineMap lines;
	for (std::set<NodeId>::const_iterator it = leaves_.begin();
	     it != leaves_.end(); ++it) {
		NodeId id = *it;
		const Node &n = nodes_[id];
		for (int e = 0; e < 3; e++) {
			int a = n.v[e];
			int b = n.v[NEXT_MOD3(e)];
			if (!positive_edge(a, b))
				continue;
			std::pair<int, int> root;
			double lo, hi;
			if (!canonical_interval(a, b, root, lo, hi))
				continue;
			lines[root].push_back(LeafSeg(id, e, lo, hi));
		}
	}
	for (LineMap::iterator it = lines.begin(); it != lines.end(); ++it) {
		std::vector<LeafSeg> &v = it->second;
		std::sort(v.begin(), v.end(),
		          [](const LeafSeg &a, const LeafSeg &b) {
			          if (a.lo != b.lo)
				          return a.lo < b.lo;
			          if (a.hi != b.hi)
				          return a.hi < b.hi;
			          return a.leaf < b.leaf;
		          });
	}
	return lines;
}

bool AdaptiveRefiner::level_gap_ok(const LineMap &lines) const
{
	for (LineMap::const_iterator it = lines.begin(); it != lines.end(); ++it) {
		const std::vector<LeafSeg> &v = it->second;
		for (size_t i = 0; i < v.size(); i++) {
			for (size_t j = i + 1; j < v.size(); j++) {
				if (v[j].lo >= v[i].hi - 1e-12)
					break;
				if (v[i].leaf == v[j].leaf)
					continue;
				if (overlap_length(v[i], v[j]) <= 1e-12)
					continue;
				int li = nodes_[v[i].leaf].level;
				int lj = nodes_[v[j].leaf].level;
				if (li - lj > 1 || lj - li > 1)
					return false;
			}
		}
	}
	return true;
}


// ---------------------------------------------------------------------------
// Refinement
// ---------------------------------------------------------------------------

// Allocate the four children of a leaf whose three midpoints already exist.
void AdaptiveRefiner::subdivide_leaf(NodeId id)
{
	allocate_children(id);
	reactivate_children(id);
}

// Create, in global sorted-edge order, all midpoints needed by this round's
// batch of leaves.  Splitting the round into "create midpoints" and "allocate
// children" phases makes new persistent vertex numbers depend only on the set
// of leaves being refined, never on request order or on which leaf happens to
// list a shared edge first.
void AdaptiveRefiner::create_round_midpoints(const std::vector<NodeId> &batch)
{
	std::set<std::pair<int, int> > edges;
	for (size_t i = 0; i < batch.size(); i++) {
		const Node &n = nodes_[batch[i]];
		edges.insert(edge_key(n.v[0], n.v[1]));
		edges.insert(edge_key(n.v[1], n.v[2]));
		edges.insert(edge_key(n.v[2], n.v[0]));
	}
	for (std::set<std::pair<int, int> >::const_iterator it = edges.begin();
	     it != edges.end(); ++it)
		create_midpoint(it->first, it->second);
}

void AdaptiveRefiner::refine_impl(const vector<NodeId> &selected)
{
	std::set<NodeId> round;
	for (size_t i = 0; i < selected.size(); i++) {
		NodeId id = selected[i];
		if (id >= (NodeId) nodes_.size() || id == INVALID_NODE)
			throw std::invalid_argument(
				"AdaptiveRefiner: unknown node id in refine");
		if (leaves_.find(id) == leaves_.end())
			throw std::invalid_argument(
				"AdaptiveRefiner: refine requires current red leaves");
		round.insert(id);
	}

	while (!round.empty()) {
		std::vector<NodeId> batch(round.begin(), round.end()); // sorted
		round.clear();

		// Level check for the whole round before mutating anything, so a
		// failure never leaves a partial subtree behind.
		for (size_t i = 0; i < batch.size(); i++) {
			if (nodes_[batch[i]].level >= MAX_LEVEL)
				throw std::runtime_error(
					"AdaptiveRefiner: maximum refinement level exceeded");
		}

		create_round_midpoints(batch);
		for (size_t i = 0; i < batch.size(); i++)
			subdivide_leaf(batch[i]);

		// Enforce the one-level-difference constraint.  Only the coarser
		// side is refined; this propagates locally and stops at the region
		// boundary instead of refining whole connected components.
		LineMap lines = build_lines();
		for (LineMap::const_iterator it = lines.begin();
		     it != lines.end(); ++it) {
			const std::vector<LeafSeg> &v = it->second;
			for (size_t i = 0; i < v.size(); i++) {
				for (size_t j = i + 1; j < v.size(); j++) {
					if (v[j].lo >= v[i].hi - 1e-12)
						break;
					if (v[i].leaf == v[j].leaf)
						continue;
					if (overlap_length(v[i], v[j]) <= 1e-12)
						continue;
					int li = nodes_[v[i].leaf].level;
					int lj = nodes_[v[j].leaf].level;
					if (li - lj > 1)
						round.insert(v[j].leaf);
					else if (lj - li > 1)
						round.insert(v[i].leaf);
				}
			}
		}
	}
}

void AdaptiveRefiner::refine(const std::vector<NodeId> &selected)
{
	// Work on a copy: any exception leaves the refiner untouched.
	AdaptiveRefiner tmp(*this);
	tmp.refine_impl(selected);
	*this = tmp;
}


// ---------------------------------------------------------------------------
// Coarsening
// ---------------------------------------------------------------------------

bool AdaptiveRefiner::coarsen_impl(const std::vector<NodeId> &parents)
{
	std::vector<NodeId> req;
	std::set<NodeId> seen;
	for (size_t i = 0; i < parents.size(); i++) {
		NodeId id = parents[i];
		if (id >= (NodeId) nodes_.size() || id == INVALID_NODE)
			throw std::invalid_argument(
				"AdaptiveRefiner: unknown node id in coarsen");
		const Node &n = nodes_[id];
		if (!n.subdivided)
			throw std::invalid_argument(
				"AdaptiveRefiner: coarsen requires a refined parent");
		for (int k = 0; k < 4; k++) {
			if (leaves_.find(n.child[k]) == leaves_.end())
				throw std::invalid_argument(
					"AdaptiveRefiner: incomplete sibling group");
		}
		if (seen.insert(id).second)
			req.push_back(id);
	}
	std::sort(req.begin(), req.end());
	if (req.empty())
		return true;

	for (size_t i = 0; i < req.size(); i++) {
		Node &n = nodes_[req[i]];
		for (int k = 0; k < 4; k++)
			leaves_.erase(n.child[k]);
		leaves_.insert(req[i]);
	}

	LineMap lines = build_lines();
	if (!level_gap_ok(lines)) {
		// The request as a whole is rejected.  The public entry point works on
		// a disposable copy, so the visible refiner keeps its original state.
		return false;
	}
	return true;
}

bool AdaptiveRefiner::coarsen(const std::vector<NodeId> &parents)
{
	AdaptiveRefiner tmp(*this);
	bool ok = tmp.coarsen_impl(parents);
	if (ok)
		*this = tmp;
	return ok;
}


// ---------------------------------------------------------------------------
// Snapshot / export
// ---------------------------------------------------------------------------

// Append the triangles covering one red leaf, using the fixed local template.
static void emit_leaf_triangles(const int v[3],
                                const bool split[3],
                                const int mid[3],
                                std::vector<TriMesh::Face> &out)
{
	const int v0 = v[0], v1 = v[1], v2 = v[2];
	const int m0 = mid[0], m1 = mid[1], m2 = mid[2]; // edges 01,12,20
	int mask = (split[0] ? 1 : 0) | (split[1] ? 2 : 0) | (split[2] ? 4 : 0);

	switch (mask) {
	case 0:
		out.push_back(TriMesh::Face(v0, v1, v2));
		break;
	case 1: // edge v0-v1
		out.push_back(TriMesh::Face(v0, m0, v2));
		out.push_back(TriMesh::Face(m0, v1, v2));
		break;
	case 2: // edge v1-v2
		out.push_back(TriMesh::Face(v1, m1, v0));
		out.push_back(TriMesh::Face(m1, v2, v0));
		break;
	case 4: // edge v2-v0
		out.push_back(TriMesh::Face(v2, m2, v1));
		out.push_back(TriMesh::Face(m2, v0, v1));
		break;
	case 3: { // edges v0-v1 and v1-v2; quad v0,m0,m1,v2
		out.push_back(TriMesh::Face(m0, v1, m1));
		std::pair<int, int> diag_a = std::make_pair(std::min(v0, m1),
		                                            std::max(v0, m1));
		std::pair<int, int> diag_b = std::make_pair(std::min(m0, v2),
		                                            std::max(m0, v2));
		if (diag_a < diag_b) {
			out.push_back(TriMesh::Face(v0, m0, m1));
			out.push_back(TriMesh::Face(v0, m1, v2));
		} else {
			out.push_back(TriMesh::Face(v0, m0, v2));
			out.push_back(TriMesh::Face(m0, m1, v2));
		}
		break;
	}
	case 5: { // edges v0-v1 and v2-v0; quad v2,m2,m0,v1
		out.push_back(TriMesh::Face(m2, v0, m0));
		std::pair<int, int> diag_a = std::make_pair(std::min(v2, m0),
		                                            std::max(v2, m0));
		std::pair<int, int> diag_b = std::make_pair(std::min(m2, v1),
		                                            std::max(m2, v1));
		if (diag_a < diag_b) {
			out.push_back(TriMesh::Face(v2, m2, m0));
			out.push_back(TriMesh::Face(v2, m0, v1));
		} else {
			out.push_back(TriMesh::Face(v2, m2, v1));
			out.push_back(TriMesh::Face(m2, m0, v1));
		}
		break;
	}
	case 6: { // edges v1-v2 and v2-v0; quad v1,m1,m2,v0
		out.push_back(TriMesh::Face(m1, v2, m2));
		std::pair<int, int> diag_a = std::make_pair(std::min(v1, m2),
		                                            std::max(v1, m2));
		std::pair<int, int> diag_b = std::make_pair(std::min(m1, v0),
		                                            std::max(m1, v0));
		if (diag_a < diag_b) {
			out.push_back(TriMesh::Face(v1, m1, m2));
			out.push_back(TriMesh::Face(v1, m2, v0));
		} else {
			out.push_back(TriMesh::Face(v1, m1, v0));
			out.push_back(TriMesh::Face(m1, m2, v0));
		}
		break;
	}
	default: // 7, all three edges split
		out.push_back(TriMesh::Face(v0, m0, m2));
		out.push_back(TriMesh::Face(v1, m1, m0));
		out.push_back(TriMesh::Face(v2, m2, m1));
		out.push_back(TriMesh::Face(m0, m1, m2));
		break;
	}
}

AdaptiveRefiner::Snapshot AdaptiveRefiner::snapshot() const
{
	Snapshot snap;

	// Current red-leaf edges, grouped by topological line.
	AdaptiveRefiner::LineMap lines = build_lines();

	// A leaf edge has to be split when some adjacent leaf covers only a
	// proper sub-segment of it (a T-junction).  After the closure pass this
	// happens exactly for edges whose neighbour is one level finer.
	std::set<std::pair<NodeId, int> > split_edges;
	for (LineMap::const_iterator it = lines.begin(); it != lines.end(); ++it) {
		const std::vector<LeafSeg> &v = it->second;
		for (size_t i = 0; i < v.size(); i++) {
			for (size_t j = i + 1; j < v.size(); j++) {
				if (v[j].lo >= v[i].hi - 1e-12)
					break;
				if (v[i].leaf == v[j].leaf)
					continue;
				if (overlap_length(v[i], v[j]) <= 1e-12)
					continue;
				if (interval_contains(v[i], v[j]))
					split_edges.insert(
						std::make_pair(v[i].leaf, v[i].edge));
				else if (interval_contains(v[j], v[i]))
					split_edges.insert(
						std::make_pair(v[j].leaf, v[j].edge));
			}
		}
	}

	// Emit faces in red-leaf NodeId order, each with the fixed local template.
	std::vector<TriMesh::Face> faces;
	std::vector<int> face_root;
	std::vector<NodeId> face_leaf;
	std::set<int> used_vertices;

	for (std::set<NodeId>::const_iterator it = leaves_.begin();
	     it != leaves_.end(); ++it) {
		NodeId id = *it;
		const Node &n = nodes_[id];
		bool split[3] = { false, false, false };
		int mid[3] = { -1, -1, -1 };
		for (int e = 0; e < 3; e++) {
			if (split_edges.find(std::make_pair(id, e)) ==
			    split_edges.end())
				continue;
			int m = midpoint_of(n.v[e], n.v[NEXT_MOD3(e)]);
			if (m < 0)
				continue; // cannot happen for a valid closure
			split[e] = true;
			mid[e] = m;
		}

		size_t before = faces.size();
		emit_leaf_triangles(n.v, split, mid, faces);
		for (size_t k = before; k < faces.size(); k++) {
			face_root.push_back((int) n.root);
			face_leaf.push_back(id);
			used_vertices.insert(faces[k][0]);
			used_vertices.insert(faces[k][1]);
			used_vertices.insert(faces[k][2]);
		}
	}

	// Compact vertices: only used persistent vertices, sorted by their
	// persistent id.  Attributes were interpolated per persistent vertex and
	// are never re-interpolated from the compacted indices.
	std::vector<int> remap(pverts_.size(), -1);
	int next = 0;
	for (std::set<int>::const_iterator it = used_vertices.begin();
	     it != used_vertices.end(); ++it)
		remap[*it] = next++;

	snap.mesh.vertices.reserve(used_vertices.size());
	if (has_colors_)
		snap.mesh.colors.reserve(used_vertices.size());
	if (has_confidences_)
		snap.mesh.confidences.reserve(used_vertices.size());
	for (std::set<int>::const_iterator it = used_vertices.begin();
	     it != used_vertices.end(); ++it) {
		const PVertex &pv = pverts_[*it];
		snap.mesh.vertices.push_back(
			point((float) pv.pos[0], (float) pv.pos[1],
			      (float) pv.pos[2]));
		if (has_colors_)
			snap.mesh.colors.push_back(
				Color((float) pv.col[0], (float) pv.col[1],
				      (float) pv.col[2]));
		if (has_confidences_)
			snap.mesh.confidences.push_back((float) pv.conf);
	}

	snap.mesh.faces.reserve(faces.size());
	snap.root_face.reserve(faces.size());
	snap.leaf_id.reserve(faces.size());
	for (size_t i = 0; i < faces.size(); i++) {
		TriMesh::Face f(remap[faces[i][0]],
		                remap[faces[i][1]],
		                remap[faces[i][2]]);
		snap.mesh.faces.push_back(f);
		snap.root_face.push_back(face_root[i]);
		snap.leaf_id.push_back(face_leaf[i]);
	}

	// Freshly built mesh: all derived data is empty/invalid and is
	// recomputed consistently by the ordinary TriMesh operators.
	snap.mesh.tstrips.clear();
	snap.mesh.grid.clear();
	snap.mesh.grid_width = snap.mesh.grid_height = -1;
	snap.mesh.flags.clear();
	snap.mesh.flag_curr = 0;
	snap.mesh.normals.clear();
	snap.mesh.pdir1.clear();
	snap.mesh.pdir2.clear();
	snap.mesh.curv1.clear();
	snap.mesh.curv2.clear();
	snap.mesh.dcurv.clear();
	snap.mesh.cornerareas.clear();
	snap.mesh.pointareas.clear();
	snap.mesh.neighbors.clear();
	snap.mesh.adjacentfaces.clear();
	snap.mesh.across_edge.clear();
	snap.mesh.bbox.clear();
	snap.mesh.bsphere.valid = false;

	return snap;
}


// ---------------------------------------------------------------------------
// Simple queries
// ---------------------------------------------------------------------------

std::vector<AdaptiveRefiner::NodeId> AdaptiveRefiner::leaves() const
{
	return std::vector<NodeId>(leaves_.begin(), leaves_.end());
}

std::vector<AdaptiveRefiner::NodeId>
AdaptiveRefiner::children(NodeId node) const
{
		if (node >= (NodeId) nodes_.size() || node == INVALID_NODE)
			throw std::invalid_argument(
				"AdaptiveRefiner: unknown node id in children");
	const Node &n = nodes_[node];
	if (!n.subdivided)
		return std::vector<NodeId>();
	return std::vector<NodeId>(n.child, n.child + 4);
}

} // namespace trimesh
