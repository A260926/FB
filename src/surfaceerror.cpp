// This file is part of meshoptimizer library; see meshoptimizer.h for version/license details
#include "surfaceerror.h"

#include "meshoptimizer.h"

#include <float.h>
#include <math.h>

#include <algorithm>
#include <vector>

namespace meshopt
{

namespace
{

struct D3
{
	double x, y, z;
};

inline D3 operator+(const D3& a, const D3& b)
{
	D3 r = {a.x + b.x, a.y + b.y, a.z + b.z};
	return r;
}

inline D3 operator-(const D3& a, const D3& b)
{
	D3 r = {a.x - b.x, a.y - b.y, a.z - b.z};
	return r;
}

inline D3 operator*(const D3& a, double s)
{
	D3 r = {a.x * s, a.y * s, a.z * s};
	return r;
}

inline double dot(const D3& a, const D3& b)
{
	return a.x * b.x + a.y * b.y + a.z * b.z;
}

inline double length2(const D3& a)
{
	return dot(a, a);
}

struct Box
{
	double min[3];
	double max[3];
};

inline Box emptyBox()
{
	Box b = {{DBL_MAX, DBL_MAX, DBL_MAX}, {-DBL_MAX, -DBL_MAX, -DBL_MAX}};
	return b;
}

inline void boxAdd(Box& b, const D3& p)
{
	b.min[0] = b.min[0] < p.x ? b.min[0] : p.x;
	b.min[1] = b.min[1] < p.y ? b.min[1] : p.y;
	b.min[2] = b.min[2] < p.z ? b.min[2] : p.z;
	b.max[0] = b.max[0] > p.x ? b.max[0] : p.x;
	b.max[1] = b.max[1] > p.y ? b.max[1] : p.y;
	b.max[2] = b.max[2] > p.z ? b.max[2] : p.z;
}

inline void boxUnion(Box& b, const Box& o)
{
	for (int i = 0; i < 3; ++i)
	{
		b.min[i] = b.min[i] < o.min[i] ? b.min[i] : o.min[i];
		b.max[i] = b.max[i] > o.max[i] ? b.max[i] : o.max[i];
	}
}

inline bool boxEquals(const Box& a, const Box& b)
{
	for (int i = 0; i < 3; ++i)
		if (a.min[i] != b.min[i] || a.max[i] != b.max[i])
			return false;
	return true;
}

// euclidean distance between two axis aligned boxes (0 when they overlap)
inline double boxDistance(const Box& a, const Box& b)
{
	double d2 = 0;

	for (int i = 0; i < 3; ++i)
	{
		double d = 0;

		if (a.max[i] < b.min[i])
			d = b.min[i] - a.max[i];
		else if (b.max[i] < a.min[i])
			d = a.min[i] - b.max[i];

		d2 += d * d;
	}

	return sqrt(d2);
}

// euclidean distance between a point and an axis aligned box (0 when inside)
inline double boxDistancePoint(const D3& p, const Box& b)
{
	double d2 = 0;

	const double v[3] = {p.x, p.y, p.z};

	for (int i = 0; i < 3; ++i)
	{
		double d = 0;

		if (v[i] < b.min[i])
			d = b.min[i] - v[i];
		else if (v[i] > b.max[i])
			d = v[i] - b.max[i];

		d2 += d * d;
	}

	return sqrt(d2);
}

struct Cell
{
	D3 a, b, c;
};

inline Box cellBox(const Cell& c)
{
	Box b = emptyBox();
	boxAdd(b, c.a);
	boxAdd(b, c.b);
	boxAdd(b, c.c);
	return b;
}

// squared distance from point to a segment
inline double segmentDistance2(const D3& p, const D3& a, const D3& b)
{
	D3 ab = b - a;
	D3 ap = p - a;
	double denom = length2(ab);

	if (denom == 0)
		return length2(ap);

	double t = dot(ab, ap) / denom;
	t = t < 0 ? 0 : (t > 1 ? 1 : t);

	D3 d = ap - ab * t;
	return length2(d);
}

// distance from point to a (possibly degenerate) triangle; the triangle is treated as a closed convex set
inline double pointTriangleDistance(const D3& p, const D3& a, const D3& b, const D3& c)
{
	D3 ab = b - a;
	D3 ac = c - a;
	D3 n = {ab.y * ac.z - ab.z * ac.y, ab.z * ac.x - ab.x * ac.z, ab.x * ac.y - ab.y * ac.x};

	// degenerate triangle: fall back to the union of the three edges
	if (length2(n) == 0)
	{
		double d0 = segmentDistance2(p, a, b);
		double d1 = segmentDistance2(p, b, c);
		double d2 = segmentDistance2(p, c, a);
		double d = d0 < d1 ? d0 : d1;
		d = d < d2 ? d : d2;
		return sqrt(d);
	}

	// Ericson, Real-Time Collision Detection, closest point on triangle
	D3 ap = p - a;
	double d1 = dot(ab, ap);
	double d2 = dot(ac, ap);

	if (d1 <= 0 && d2 <= 0)
		return sqrt(length2(ap));

	D3 bp = p - b;
	double d3 = dot(ab, bp);
	double d4 = dot(ac, bp);

	if (d3 >= 0 && d4 <= d3)
		return sqrt(length2(bp));

	double vc = d1 * d4 - d3 * d2;

	if (vc <= 0 && d1 >= 0 && d3 <= 0)
	{
		double v = d1 / (d1 - d3);
		D3 q = a + ab * v;
		return sqrt(length2(p - q));
	}

	D3 cp = p - c;
	double d5 = dot(ab, cp);
	double d6 = dot(ac, cp);

	if (d6 >= 0 && d5 <= d6)
		return sqrt(length2(cp));

	double vb = d5 * d2 - d1 * d6;

	if (vb <= 0 && d2 >= 0 && d6 <= 0)
	{
		double w = d2 / (d2 - d6);
		D3 q = a + ac * w;
		return sqrt(length2(p - q));
	}

	double va = d3 * d6 - d5 * d4;

	if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0)
	{
		double w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
		D3 q = b + (c - b) * w;
		return sqrt(length2(p - q));
	}

	double denom = va + vb + vc;

	if (denom == 0)
	{
		double e0 = segmentDistance2(p, a, b);
		double e1 = segmentDistance2(p, b, c);
		double e2 = segmentDistance2(p, c, a);
		double d = e0 < e1 ? e0 : e1;
		d = d < e2 ? d : e2;
		return sqrt(d);
	}

	double v = vb / denom;
	double w = vc / denom;
	D3 q = a + ab * v + ac * w;
	return sqrt(length2(p - q));
}

} // anonymous namespace

struct SurfaceErrorCertifier::Impl
{
	struct Node
	{
		Box box;
		int left; // internal: left child index, -1 for leaf
		int right; // internal: right child index, -1 for leaf
		unsigned int face; // leaf: face id
	};

	struct Leaf
	{
		Cell cell;
		unsigned int depth;
		unsigned int witness; // face id whose distance covers the cell
		unsigned int owner; // direction A: original face id; direction B: target face id
		double bound; // max corner distance to witness (without rounding margin)
		unsigned int stamp; // dedup stamp for candidate simulation
		unsigned char valid;
	};

	struct Face
	{
		unsigned int v[3]; // exact vertex ids referenced by the triangle (move targets)
		unsigned int p[3]; // canonical position ids (geometry and degeneracy)
		unsigned char alive;
		Box box;
	};

	struct BoxAxis
	{
		const std::vector<Box>* boxes;
		int axis;

		bool operator()(unsigned int lhs, unsigned int rhs) const
		{
			const Box& l = (*boxes)[lhs];
			const Box& r = (*boxes)[rhs];
			return l.min[axis] + l.max[axis] < r.min[axis] + r.max[axis];
		}
	};

	float max_error;
	unsigned int max_depth;

	std::vector<D3> positions; // canonical position coordinates
	std::vector<unsigned int> vertex_position; // vertex index -> canonical position id
	std::vector<Face> faces; // stable face pool; ids are original triangle ids
	std::vector<Box> original_boxes; // original surface triangle bounds (S index)
	std::vector<D3> original_points; // original surface triangle coordinates (3 per face)

	std::vector<Leaf> leaves_a; // direction S->T certificates
	std::vector<std::vector<unsigned int> > face_deps; // per target face: direction A leaf ids referencing it
	std::vector<unsigned int> leaf_stamp; // dedup stamps for direction A leaves
	unsigned int leaf_epoch;

	std::vector<Leaf> cert_b; // direction T->S certificates
	std::vector<std::vector<unsigned int> > face_b_leaves; // per target face: indices into cert_b

	std::vector<std::vector<unsigned int> > adjacency; // canonical position -> face ids (may contain stale entries)
	std::vector<unsigned int> visit; // per-face simulation stamps
	unsigned int visit_epoch;

	std::vector<Node> nodes_s; // static index over the original surface
	std::vector<Node> nodes_t; // index over the active surface, rebuilt at pass boundaries
	std::vector<int> parent_t;
	std::vector<int> face_leaf_t;

	std::vector<int> stack; // query scratch
	std::vector<Leaf> scratch; // proof output scratch

	double scale; // coordinate magnitude used for the rounding margin
	double certified_error;
	double work; // per-candidate proof work guard
	bool geometry_changed;
	size_t valid_leaves; // number of live certificates in both directions (drives amortized compaction)

	size_t accepted_collapses;
	size_t rejected_distance;
	size_t rejected_budget;
	size_t invalidated_certificates;
	mutable size_t distance_tests;

	Impl(float max_surface_error, unsigned int depth)
	    : max_error(max_surface_error)
	    , max_depth(depth)
	    , leaf_epoch(0)
	    , visit_epoch(0)
	    , scale(0)
	    , certified_error(0)
	    , work(0)
	    , geometry_changed(false)
	    , valid_leaves(0)
	    , accepted_collapses(0)
	    , rejected_distance(0)
	    , rejected_budget(0)
	    , invalidated_certificates(0)
	    , distance_tests(0)
	{
	}

	// conservative outward expansion that covers subdivision rounding and double precision distance evaluation
	double margin(unsigned int depth) const
	{
		double d = double(depth) + 1.0;
		return 256.0 * DBL_EPSILON * (1.0 + scale) * d * d;
	}

	double pointDistance(const D3& p, unsigned int face) const
	{
		const Face& f = faces[face];
		++distance_tests;
		return pointTriangleDistance(p, positions[f.p[0]], positions[f.p[1]], positions[f.p[2]]);
	}

	double pointDistanceOriginal(const D3& p, unsigned int face) const
	{
		const D3* t = &original_points[face * 3];
		++distance_tests;
		return pointTriangleDistance(p, t[0], t[1], t[2]);
	}

	// ---- surface state ----

	void updateFaceBox(unsigned int face)
	{
		Face& f = faces[face];
		Box b = emptyBox();
		for (int i = 0; i < 3; ++i)
			boxAdd(b, positions[f.p[i]]);
		f.box = b;
	}

	void init(const float* vertex_positions, size_t vertex_positions_stride, const unsigned int* sparse_remap, size_t vertex_count, const unsigned int* indices, size_t index_count, const unsigned int* position_remap)
	{
		size_t stride_float = vertex_positions_stride / sizeof(float);

		positions.resize(vertex_count);
		vertex_position.resize(vertex_count);
		scale = 0;

		for (size_t i = 0; i < vertex_count; ++i)
		{
			unsigned int ri = sparse_remap ? sparse_remap[i] : unsigned(i);
			const float* v = vertex_positions + ri * stride_float;

			positions[i].x = double(v[0]);
			positions[i].y = double(v[1]);
			positions[i].z = double(v[2]);
			vertex_position[i] = position_remap[i];

			for (int k = 0; k < 3; ++k)
			{
				double a = fabs(double(v[k]));
				scale = scale < a ? a : scale;
			}
		}

		size_t face_count = index_count / 3;
		faces.resize(face_count);
		original_boxes.resize(face_count);
		original_points.resize(face_count * 3);
		face_deps.resize(face_count);
		face_b_leaves.resize(face_count);
		adjacency.resize(vertex_count);
		visit.resize(face_count, 0);
		leaf_stamp.resize(face_count, 0);

		leaves_a.reserve(face_count);
		cert_b.reserve(face_count);
		valid_leaves = 0;

		for (size_t i = 0; i < face_count; ++i)
		{
			Face& f = faces[i];
			for (int k = 0; k < 3; ++k)
			{
				unsigned int v = indices[i * 3 + k];
				f.v[k] = v;
				f.p[k] = position_remap[v];
			}
			f.alive = 1;
			updateFaceBox(unsigned(i));

			for (int k = 0; k < 3; ++k)
				original_points[i * 3 + k] = positions[f.p[k]];

			Box ob = emptyBox();
			for (int k = 0; k < 3; ++k)
				boxAdd(ob, original_points[i * 3 + k]);
			original_boxes[i] = ob;

			// both surfaces start identical: each triangle certifies itself with a zero bound
			Cell cell = {original_points[i * 3 + 0], original_points[i * 3 + 1], original_points[i * 3 + 2]};

			Leaf la = {cell, 0, unsigned(i), unsigned(i), 0.0, 0, 1};
			leaves_a.push_back(la);
			face_deps[i].push_back(unsigned(leaves_a.size() - 1));
			++valid_leaves;

			Leaf lb = {cell, 0, unsigned(i), unsigned(i), 0.0, 0, 1};
			face_b_leaves[i].push_back(unsigned(cert_b.size()));
			cert_b.push_back(lb);
			++valid_leaves;

			for (int k = 0; k < 3; ++k)
				adjacency[f.p[k]].push_back(unsigned(i));
		}

		buildStatic();
		reorganize();
	}

	// ---- BVH construction ----

	int buildRec(std::vector<Node>& nodes, std::vector<unsigned int>& ids, size_t begin, size_t end, const std::vector<Box>& boxes, std::vector<int>* parents)
	{
		int index = int(nodes.size());
		nodes.push_back(Node());

		if (parents)
			parents->push_back(-1);

		if (end - begin == 1)
		{
			Node& n = nodes[index];
			n.left = n.right = -1;
			n.face = ids[begin];
			n.box = boxes[ids[begin]];
			return index;
		}

		Box total = emptyBox();
		for (size_t i = begin; i < end; ++i)
			boxUnion(total, boxes[ids[i]]);

		int axis = 0;
		double extent = total.max[0] - total.min[0];
		for (int k = 1; k < 3; ++k)
		{
			double e = total.max[k] - total.min[k];
			if (e > extent)
				axis = k, extent = e;
		}

		size_t mid = begin + (end - begin) / 2;

		BoxAxis pred;
		pred.boxes = &boxes;
		pred.axis = axis;
		std::nth_element(ids.begin() + begin, ids.begin() + mid, ids.begin() + end, pred);

		int left = buildRec(nodes, ids, begin, mid, boxes, parents);
		int right = buildRec(nodes, ids, mid, end, boxes, parents);

		nodes[index].left = left;
		nodes[index].right = right;
		nodes[index].face = ~0u;
		nodes[index].box = nodes[left].box;
		boxUnion(nodes[index].box, nodes[right].box);

		if (parents)
		{
			(*parents)[left] = index;
			(*parents)[right] = index;
		}

		return index;
	}

	void buildStatic()
	{
		nodes_s.clear();

		if (faces.empty())
			return;

		std::vector<unsigned int> ids(faces.size());
		for (size_t i = 0; i < ids.size(); ++i)
			ids[i] = unsigned(i);

		nodes_s.reserve(faces.size() * 2);
		buildRec(nodes_s, ids, 0, ids.size(), original_boxes, NULL);
	}

	void rebuildActive()
	{
		nodes_t.clear();
		parent_t.clear();
		face_leaf_t.assign(faces.size(), -1);

		std::vector<unsigned int> ids;
		std::vector<Box> boxes(faces.size());
		ids.reserve(faces.size());

		for (size_t i = 0; i < faces.size(); ++i)
			if (faces[i].alive)
			{
				ids.push_back(unsigned(i));
				boxes[i] = faces[i].box;
			}

		if (ids.empty())
			return;

		nodes_t.reserve(ids.size() * 2);
		buildRec(nodes_t, ids, 0, ids.size(), boxes, &parent_t);

		for (size_t i = 0; i < nodes_t.size(); ++i)
			if (nodes_t[i].left == -1)
				face_leaf_t[nodes_t[i].face] = int(i);
	}

	// grows the leaf and ancestor bounds after a face geometry update; bounds never shrink inside a pass
	void refit(unsigned int face)
	{
		if (face_leaf_t.size() != faces.size())
			return;

		int node = face_leaf_t[face];

		if (node < 0)
			return;

		Box nb = nodes_t[node].box;
		boxUnion(nb, faces[face].box);
		nodes_t[node].box = nb;

		while (parent_t[node] >= 0)
		{
			int parent = parent_t[node];
			Box pb = nodes_t[parent].box;
			boxUnion(pb, nodes_t[node].box);

			if (boxEquals(pb, nodes_t[parent].box))
				break;

			nodes_t[parent].box = pb;
			node = parent;
		}
	}

	// ---- queries ----

	double testCorners(bool active, const Cell& cell, unsigned int face, double limit) const
	{
		const D3 corners[3] = {cell.a, cell.b, cell.c};
		double bound = 0;

		for (int i = 0; i < 3; ++i)
		{
			double d = active ? pointDistance(corners[i], face) : pointDistanceOriginal(corners[i], face);

			if (d > limit)
				return -1;

			bound = bound < d ? d : bound;
		}

		return bound;
	}

	bool findWitness(bool active, const Cell& cell, double limit, unsigned int* out_face, double* out_bound)
	{
		const std::vector<Node>& nodes = active ? nodes_t : nodes_s;

		if (nodes.empty())
			return false;

		Box cb = cellBox(cell);
		stack.clear();
		stack.push_back(0);

		while (!stack.empty())
		{
			int index = stack.back();
			stack.pop_back();

			const Node& n = nodes[index];

			if (boxDistance(cb, n.box) > limit)
				continue;

			if (n.left < 0)
			{
				if (active && !faces[n.face].alive)
					continue;

				double bound = testCorners(active, cell, n.face, limit);

				if (bound >= 0)
				{
					*out_face = n.face;
					*out_bound = bound;
					return true;
				}
			}
			else
			{
				stack.push_back(n.left);
				stack.push_back(n.right);
			}
		}

		return false;
	}

	// lower bound on the distance from any point of the cell to the surface: dist(x, T) >= dist(centroid, T) - radius,
	// where centroid is the cell centroid and radius the largest distance from centroid to a cell vertex (the maximum
	// of a convex function over the cell's convex hull is attained at a vertex). This is the only route to a provable
	// distance rejection; the depth/budget path handles everything else.
	bool provablyFar(bool active, const Cell& cell, double limit)
	{
		const std::vector<Node>& nodes = active ? nodes_t : nodes_s;

		if (nodes.empty())
			return false;

		D3 centroid = (cell.a + cell.b + cell.c) * (1.0 / 3.0);

		double radius = length2(cell.a - centroid);
		radius = radius < length2(cell.b - centroid) ? length2(cell.b - centroid) : radius;
		radius = radius < length2(cell.c - centroid) ? length2(cell.c - centroid) : radius;
		radius = sqrt(radius);

		// branch and bound nearest-triangle search from the centroid
		double best = DBL_MAX;
		stack.clear();
		stack.push_back(0);

		while (!stack.empty())
		{
			int index = stack.back();
			stack.pop_back();

			const Node& n = nodes[index];

			// the closest possible point in this node is farther than the best found triangle
			if (boxDistancePoint(centroid, n.box) > best)
				continue;

			if (n.left < 0)
			{
				if (active && !faces[n.face].alive)
					continue;

				double d = active ? pointDistance(centroid, n.face) : pointDistanceOriginal(centroid, n.face);
				best = best < d ? best : d;
			}
			else
			{
				stack.push_back(n.left);
				stack.push_back(n.right);
			}
		}

		return best != DBL_MAX && best - radius > limit;
	}

	// ---- adaptive proofs ----

	static void subdivide(const Cell& c, Cell out[4])
	{
		D3 ab = (c.a + c.b) * 0.5;
		D3 bc = (c.b + c.c) * 0.5;
		D3 ca = (c.c + c.a) * 0.5;

		Cell c0 = {c.a, ab, ca};
		Cell c1 = {ab, c.b, bc};
		Cell c2 = {ca, bc, c.c};
		Cell c3 = {ab, bc, ca};

		out[0] = c0;
		out[1] = c1;
		out[2] = c2;
		out[3] = c3;
	}

	// proof result: 0 accepted, 1 distance exceeded, 2 unresolved at the depth/work limit
	// on acceptance appends leaves to out; on failure leaves appended past the caller's mark must be discarded
	int proveCell(bool active, const Cell& cell, unsigned int depth, unsigned int owner, std::vector<Leaf>& out)
	{
		work += 1;

		if (work > 200000)
			return 2;

		double limit = double(max_error) - margin(depth);

		if (limit >= 0)
		{
			unsigned int witness = 0;
			double bound = 0;

			if (findWitness(active, cell, limit, &witness, &bound))
			{
				Leaf leaf = {cell, depth, witness, owner, bound, 0, 1};
				out.push_back(leaf);
				return 0;
			}
		}

		if (depth >= max_depth)
			return provablyFar(active, cell, double(max_error)) ? 1 : 2;

		Cell children[4];
		subdivide(cell, children);

		for (int i = 0; i < 4; ++i)
		{
			size_t mark = out.size();
			int r = proveCell(active, children[i], depth + 1, owner, out);

			if (r != 0)
			{
				out.resize(mark);
				return r;
			}
		}

		return 0;
	}

	bool faceContains(unsigned int face, unsigned int pos) const
	{
		const Face& f = faces[face];
		return f.p[0] == pos || f.p[1] == pos || f.p[2] == pos;
	}

	// ---- candidate simulation ----

	struct SavedFace
	{
		unsigned int face;
		unsigned int v[3];
		unsigned int p[3];
		unsigned char alive;
		Box box;
	};

	void restore(std::vector<SavedFace>& saved)
	{
		for (size_t i = 0; i < saved.size(); ++i)
		{
			Face& f = faces[saved[i].face];
			f.alive = saved[i].alive;
			f.box = saved[i].box;
			for (int k = 0; k < 3; ++k)
			{
				f.v[k] = saved[i].v[k];
				f.p[k] = saved[i].p[k];
			}
		}
	}

	// returns 0 on commit, 1 on provable distance rejection, 2 on depth/budget rejection
	int simulate(const SurfaceErrorMove* moves, size_t move_count)
	{
		if (faces.empty())
			return 0;

		++visit_epoch;
		work = 0;

		std::vector<unsigned int> affected;

		for (size_t m = 0; m < move_count; ++m)
		{
			unsigned int from = moves[m].from;

			if (from >= vertex_position.size())
				continue;

			unsigned int pos = vertex_position[from];

			if (pos >= adjacency.size())
				continue;

			const std::vector<unsigned int>& list = adjacency[pos];

			for (size_t i = 0; i < list.size(); ++i)
			{
				unsigned int f = list[i];

				if (f >= faces.size() || !faces[f].alive || visit[f] == visit_epoch || !faceContains(f, pos))
					continue;

				visit[f] = visit_epoch;
				affected.push_back(f);
			}
		}

		if (affected.empty())
		{
			++accepted_collapses;
			return 0;
		}

		std::vector<SavedFace> saved(affected.size());
		std::vector<unsigned char> survives(affected.size(), 1);

		for (size_t i = 0; i < affected.size(); ++i)
		{
			unsigned int f = affected[i];
			Face& face = faces[f];

			SavedFace s;
			s.face = f;
			s.alive = face.alive;
			s.box = face.box;
			for (int k = 0; k < 3; ++k)
			{
				s.v[k] = face.v[k];
				s.p[k] = face.p[k];
			}
			saved[i] = s;

			unsigned int nv[3];
			unsigned int np[3];
			for (int k = 0; k < 3; ++k)
			{
				// exact vertex moves: the triangle keeps the target vertex slot, whose position may be shared with
				// other wedges; collapse_remap is built by sequential assignment, so later moves win (same as here)
				nv[k] = face.v[k];

				for (size_t m = move_count; m > 0; --m)
				{
					unsigned int from = moves[m - 1].from;

					if (face.v[k] == from)
					{
						nv[k] = moves[m - 1].to;
						break;
					}
				}

				np[k] = nv[k] < vertex_position.size() ? vertex_position[nv[k]] : face.p[k];
			}

			if (np[0] == np[1] || np[0] == np[2] || np[1] == np[2])
			{
				// remapIndexBuffer drops triangles whose canonical positions collapse, even when the target
				// vertex ids differ; mirror that exactly so the tracked surface matches the emitted index buffer
				survives[i] = 0;
				face.alive = 0;
			}
			else
			{
				for (int k = 0; k < 3; ++k)
				{
					face.v[k] = nv[k];
					face.p[k] = np[k];
				}
				updateFaceBox(f);
				refit(f);
			}
		}

		int failure = 0;

		// direction T->S: each surviving changed face must be re-proven against the fixed original surface
		std::vector<std::vector<Leaf> > new_b(affected.size());

		for (size_t i = 0; i < affected.size(); ++i)
		{
			if (!survives[i])
				continue;

			Cell cell;
			cell.a = positions[faces[affected[i]].p[0]];
			cell.b = positions[faces[affected[i]].p[1]];
			cell.c = positions[faces[affected[i]].p[2]];

			std::vector<Leaf> leaves;
			int r = proveCell(false, cell, 0, affected[i], leaves);

			if (r != 0)
			{
				// a rejection is only a "distance" rejection when every failing cell was provably beyond the limit;
				// any unresolved cell downgrades the classification to the budget bucket
				failure = (failure == 2 || r == 2) ? 2 : 1;
				break;
			}

			new_b[i].swap(leaves);
		}

		// direction S->T: certificates that reference a changed face must be re-proven against the candidate surface
		std::vector<unsigned int> invalidated;

		if (failure == 0)
		{
			++leaf_epoch;

			if (leaf_stamp.size() < leaves_a.size())
				leaf_stamp.resize(leaves_a.size(), 0);

			for (size_t i = 0; i < affected.size(); ++i)
			{
				const std::vector<unsigned int>& deps = face_deps[affected[i]];

				for (size_t j = 0; j < deps.size(); ++j)
				{
					unsigned int id = deps[j];

					if (id >= leaves_a.size() || !leaves_a[id].valid || leaf_stamp[id] == leaf_epoch)
						continue;

					leaf_stamp[id] = leaf_epoch;
					invalidated.push_back(id);
				}
			}
		}

		std::vector<Leaf> new_a;
		std::vector<unsigned int> new_a_dep;

		if (failure == 0)
			for (size_t i = 0; i < invalidated.size(); ++i)
			{
				const Leaf& old = leaves_a[invalidated[i]];

				scratch.clear();
				int r = proveCell(true, old.cell, old.depth, old.owner, scratch);

				if (r != 0)
				{
					failure = (failure == 2 || r == 2) ? 2 : 1;
					break;
				}

				for (size_t k = 0; k < scratch.size(); ++k)
				{
					new_a_dep.push_back(scratch[k].witness);
					new_a.push_back(scratch[k]);
				}
			}

		if (failure != 0)
		{
			restore(saved);

			if (failure == 1)
				++rejected_distance;
			else
				++rejected_budget;

			return failure;
		}

		// commit: install new certificates, drop superseded ones
		geometry_changed = true;

		for (size_t i = 0; i < invalidated.size(); ++i)
		{
			leaves_a[invalidated[i]].valid = 0;
			--valid_leaves;
			++invalidated_certificates;
		}

		for (size_t i = 0; i < affected.size(); ++i)
			face_deps[affected[i]].clear();

		for (size_t i = 0; i < new_a.size(); ++i)
		{
			unsigned int id = unsigned(leaves_a.size());
			leaves_a.push_back(new_a[i]);
			++valid_leaves;

			if (leaf_stamp.size() <= id)
				leaf_stamp.resize(id + 1, 0);

			face_deps[new_a_dep[i]].push_back(id);
		}

		for (size_t i = 0; i < affected.size(); ++i)
		{
			unsigned int f = affected[i];
			std::vector<unsigned int>& ids = face_b_leaves[f];

			for (size_t j = 0; j < ids.size(); ++j)
			{
				cert_b[ids[j]].valid = 0;
				--valid_leaves;
				++invalidated_certificates;
			}

			ids.clear();

			if (survives[i])
			{
				for (size_t j = 0; j < new_b[i].size(); ++j)
				{
					ids.push_back(unsigned(cert_b.size()));
					cert_b.push_back(new_b[i][j]);
					++valid_leaves;
				}

				for (int k = 0; k < 3; ++k)
					adjacency[faces[f].p[k]].push_back(f);
			}
		}

		++accepted_collapses;

		// amortized compaction: dead certificates accumulate as collapses are committed, so reclaim them when the
		// pools have doubled relative to the live set instead of waiting for the pass boundary
		size_t pool = leaves_a.size() + cert_b.size();

		if (pool > 2 * valid_leaves + 1024)
			compactCertificates();

		return 0;
	}

	// ---- maintenance ----

	// compacts certificate pools while preserving face identities and dependency lists; called at pass boundaries and
	// amortized during a pass so memory stays proportional to the live certificate count
	void compactCertificates()
	{
		// compact direction A leaves; rebuild dependency lists
		std::vector<Leaf> compact;
		compact.reserve(valid_leaves + 1);

		for (size_t i = 0; i < face_deps.size(); ++i)
			face_deps[i].clear();

		for (size_t i = 0; i < leaves_a.size(); ++i)
		{
			if (!leaves_a[i].valid)
				continue;

			unsigned int id = unsigned(compact.size());
			compact.push_back(leaves_a[i]);
			face_deps[leaves_a[i].witness].push_back(id);
		}

		leaves_a.swap(compact);
		leaf_stamp.assign(leaves_a.size(), 0);

		// compact direction B certificates, dropping ones that reference dead faces
		std::vector<Leaf> compact_b;
		compact_b.reserve(valid_leaves + 1);
		std::vector<unsigned int> remap_b(cert_b.size(), ~0u);

		for (size_t f = 0; f < face_b_leaves.size(); ++f)
		{
			std::vector<unsigned int> ids;
			ids.reserve(face_b_leaves[f].size());

			if (faces[f].alive)
				for (size_t j = 0; j < face_b_leaves[f].size(); ++j)
				{
					unsigned int id = face_b_leaves[f][j];

					if (!cert_b[id].valid)
						continue;

					remap_b[id] = unsigned(compact_b.size());
					compact_b.push_back(cert_b[id]);
					ids.push_back(remap_b[id]);
				}

			face_b_leaves[f].swap(ids);
		}

		cert_b.swap(compact_b);
	}

	void reorganize()
	{
		compactCertificates();

		// rebuild adjacency from live faces
		for (size_t i = 0; i < adjacency.size(); ++i)
			adjacency[i].clear();

		for (size_t i = 0; i < faces.size(); ++i)
			if (faces[i].alive)
				for (int k = 0; k < 3; ++k)
					adjacency[faces[i].p[k]].push_back(unsigned(i));

		rebuildActive();
	}

	size_t liveTriangles() const
	{
		size_t count = 0;
		for (size_t i = 0; i < faces.size(); ++i)
			count += faces[i].alive;
		return count;
	}

	void finalize(struct meshopt_SurfaceErrorStats* stats)
	{
		certified_error = 0;

		if (geometry_changed)
		{
			for (size_t i = 0; i < leaves_a.size(); ++i)
			{
				if (!leaves_a[i].valid)
					continue;

				double b = leaves_a[i].bound + margin(leaves_a[i].depth);
				certified_error = certified_error < b ? b : certified_error;
			}

			for (size_t f = 0; f < faces.size(); ++f)
			{
				if (!faces[f].alive)
					continue;

				for (size_t j = 0; j < face_b_leaves[f].size(); ++j)
				{
					const Leaf& leaf = cert_b[face_b_leaves[f][j]];

					if (!leaf.valid)
						continue;

					double b = leaf.bound + margin(leaf.depth);
					certified_error = certified_error < b ? b : certified_error;
				}
			}
		}

		if (stats)
		{
			stats->certified_error = certified_error;
			stats->accepted_collapses = accepted_collapses;
			stats->rejected_distance = rejected_distance;
			stats->rejected_budget = rejected_budget;
			stats->invalidated_certificates = invalidated_certificates;
			stats->distance_tests = distance_tests;
		}
	}
};

SurfaceErrorCertifier::SurfaceErrorCertifier(float max_surface_error, unsigned int max_depth)
    : impl(new Impl(max_surface_error, max_depth))
{
}

SurfaceErrorCertifier::~SurfaceErrorCertifier()
{
	delete impl;
}

void SurfaceErrorCertifier::init(const float* vertex_positions, size_t vertex_positions_stride, const unsigned int* sparse_remap, size_t vertex_count, const unsigned int* indices, size_t index_count, const unsigned int* position_remap)
{
	impl->init(vertex_positions, vertex_positions_stride, sparse_remap, vertex_count, indices, index_count, position_remap);
}

void SurfaceErrorCertifier::reorganize()
{
	impl->reorganize();
}

bool SurfaceErrorCertifier::simulate(const SurfaceErrorMove* moves, size_t move_count)
{
	return impl->simulate(moves, move_count) == 0;
}

void SurfaceErrorCertifier::finalize(struct meshopt_SurfaceErrorStats* stats)
{
	impl->finalize(stats);
}

size_t SurfaceErrorCertifier::liveTriangles() const
{
	return impl->liveTriangles();
}

} // namespace meshopt
