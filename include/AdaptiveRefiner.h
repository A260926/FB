#ifndef ADAPTIVEREFINER_H
#define ADAPTIVEREFINER_H
/*
AdaptiveRefiner.h
Locally adaptive, persistent red refinement of a triangle mesh.

The refiner owns a private copy of the input mesh (positions, colors and
confidences); modifying or destroying the source TriMesh afterwards does not
affect the refiner.

Refinement levels are stored as a forest of red (1-to-4) subdivision
quadtree nodes.  A NodeId is a std::uint64_t.  The root nodes are numbered
exactly like the input faces (root face i has NodeId i).  Every further node
receives the next never-used number, in allocation order.  Numbers are
monotone and, once published, always denote the same triangle; reactivating a
coarsened parent reuses its original four child numbers.

Level-0 corner vertices keep the input vertex indices as their persistent
vertex numbers.  Each edge midpoint is identified *topologically* by the
unordered pair of persistent vertex numbers of its endpoints (no welding by
spatial distance).  A midpoint receives the next free persistent vertex number
when it is first created and keeps that number forever.  New midpoints are
created in a deterministic order that only depends on the pre-operation leaf
set: leaves are processed in NodeId order and their three edges in the order of
the sorted pair of persistent endpoint numbers.  Closed neighbourhoods are
refined in rounds.

Only finite, non-degenerate, consistently oriented 2-manifold triangle meshes
are accepted (boundaries and several connected components are fine).  Bad
input - including colors/confidences whose length is neither zero nor equal to
the vertex count - throws std::invalid_argument.  Vertices with equal
coordinates but different indices are deliberately kept distinct.

API overview
------------
leaves()
    Current red leaves, sorted by NodeId.

children(node)
    The four child NodeIds in the fixed local template order, for a node that
    has ever been refined:
        (v0,m01,m20), (v1,m12,m01), (v2,m20,m12), (m01,m12,m20)
    Returns an empty vector for a node that has never been refined (including
    a plain leaf).  A coarsened parent still reports the same four child
    numbers.  Unknown NodeIds throw std::invalid_argument.

refine(selected)
    selected must contain current red leaves (duplicates and ordering are
    irrelevant).  Every selected leaf is split into its four red children;
    original vertices are never moved.  A local closure then refines coarser
    neighbours until, along every shared edge of positive length, the levels
    of adjacent red leaves differ by at most one.  Only the coarse side is
    refined, so the closure stays local instead of refining a whole connected
    component.  Faces that touch only at a vertex are not constrained.  Bad,
    deactivated or non-leaf NodeIds throw std::invalid_argument; needing a
    level beyond MAX_LEVEL (12) throws std::runtime_error.  In every failure
    case the refiner is left completely unchanged.

coarsen(parents)
    Retracts each requested parent, but only when all four of its children are
    current red leaves.  Bad NodeIds, non-parents and incomplete sibling
    groups throw std::invalid_argument.  The request is judged as a whole:
    if the retractions would break the one-level-difference rule, false is
    returned and nothing at all is changed (no nodes outside the request are
    coarsened).  Otherwise the retraction takes place and true is returned.
    Refining a retracted parent again reuses its original child numbers.

snapshot()
    Builds an indexed TriMesh that can be fed directly to the ordinary TriMesh
    operators.  Red leaves are emitted in NodeId order, each using a fixed
    local template.  A leaf that has a neighbour one level finer across an edge
    gets green transition triangles on that edge (1+k triangles for k split
    edges, k = 0..3).  When a two-split quad remains, its diagonal is chosen as
    the lexicographically smaller of the two candidate diagonals expressed as
    ordered pairs of persistent vertex numbers.  Green faces never enter the
    red hierarchy and cannot be passed to refine().

    Output vertices include only points actually used and are sorted by
    persistent vertex number.  Positions, colors and confidences are always
    interpolated with the linear midpoint rule directly from the original
    input mesh, never by re-interpolating compressed output indices.  Absent
    attributes stay absent.  Derived data (tstrips, grid, flags, normals,
    curvature, point areas, connectivity) is left empty, and the bounding box
    and sphere are invalid; need_normals(), need_pointareas() and
    need_across_edge() then compute results consistent with the output faces.

    Snapshot::root_face[i] is the initial face index that contains output face
    i (equivalently, the root NodeId), and Snapshot::leaf_id[i] is the red leaf
    NodeId that produced it (green faces included).  Both vectors are parallel
    to Snapshot::mesh.faces.  Repeated snapshots do not change the hierarchy,
    and a returned Snapshot is independent of later operations.
*/

#include "TriMesh.h"
#include <cstdint>
#include <map>
#include <set>
#include <utility>
#include <vector>

namespace trimesh {

class AdaptiveRefiner {
public:
	typedef std::uint64_t NodeId;

	enum { MAX_LEVEL = 12 };

	struct Snapshot {
		TriMesh mesh;
		// Parallel to mesh.faces: root (initial) face index and red leaf id.
		std::vector<int> root_face;
		std::vector<NodeId> leaf_id;
	};

	// Validates and deep-copies the input.  Throws std::invalid_argument.
	explicit AdaptiveRefiner(const TriMesh &mesh);

	// Current red leaves, sorted by NodeId.
	std::vector<NodeId> leaves() const;

	// Four children in local template order, or empty if never refined.
	std::vector<NodeId> children(NodeId node) const;

	// Refine current red leaves, including the level-gap closure.
	void refine(const std::vector<NodeId> &selected);

	// Try to retract the given parents.  See the class comment.
	bool coarsen(const std::vector<NodeId> &parents);

	// Export the current red leaves (plus green transitions).
	Snapshot snapshot() const;

private:
	static const NodeId INVALID_NODE;

	struct Node {
		NodeId parent;
		NodeId root;
		int level;
		int v[3];
		NodeId child[4];
		bool subdivided;

		Node()
			: parent(INVALID_NODE), root(INVALID_NODE), level(0),
			  subdivided(false)
		{
			v[0] = v[1] = v[2] = -1;
			child[0] = child[1] = child[2] = child[3] = INVALID_NODE;
		}
	};

	struct PVertex {
		dvec3 pos;
		dvec3 col;
		double conf;
		PVertex() : pos(0, 0, 0), col(0, 0, 0), conf(0) {}
	};

	// A red-leaf edge placed on its maximal topological line: [lo,hi] is the
	// dyadic interval it occupies inside that line (0..1).
	struct LeafSeg {
		NodeId leaf;
		int edge;
		double lo, hi;
		LeafSeg() : leaf(0), edge(0), lo(0), hi(0) {}
		LeafSeg(NodeId l, int e, double a, double b)
			: leaf(l), edge(e), lo(a), hi(b) {}
	};
	typedef std::map<std::pair<int, int>, std::vector<LeafSeg> > LineMap;

	// Persistent red-refinement state.
	std::vector<Node> nodes_;
	std::set<NodeId> leaves_;
	std::vector<PVertex> pverts_;
	std::size_t original_vertex_count_;
	bool has_colors_;
	bool has_confidences_;
	// Topological edge (sorted persistent vertex pair) -> midpoint vertex id.
	std::map<std::pair<int, int>, int> edge_midpoint_;
	// For every persistent vertex: its parent edge (-1,-1 for originals).
	std::vector<std::pair<int, int> > midpoint_parent_edge_;

	// Construction / validation.
	void build_from(const TriMesh &mesh);
	void validate_topology() const;

	// Geometry / topology helpers.
	static std::pair<int, int> edge_key(int a, int b)
	{
		return (a < b) ? std::make_pair(a, b) : std::make_pair(b, a);
	}
	bool positive_edge(int a, int b) const;
	int midpoint_of(int a, int b) const;      // -1 if none
	bool is_midpoint_vertex(int v) const
	{
		return (std::size_t) v >= original_vertex_count_;
	}
	void create_midpoint(int a, int b);
	void allocate_children(NodeId parent);
	void reactivate_children(NodeId parent);

	static bool interval_contains(const LeafSeg &outer, const LeafSeg &inner);
	static double overlap_length(const LeafSeg &a, const LeafSeg &b);
	LineMap build_lines() const;
	bool line_coord(int x, int root_a, int root_b, int depth,
	                double &out) const;
	bool canonical_interval(int a, int b, std::pair<int, int> &root,
	                        double &lo, double &hi) const;
	bool level_gap_ok(const LineMap &lines) const;
	void create_round_midpoints(const std::vector<NodeId> &batch);
	void subdivide_leaf(NodeId id);

	// Implementation entry points that work on a disposable copy.
	void refine_impl(const std::vector<NodeId> &selected);
	bool coarsen_impl(const std::vector<NodeId> &parents);
};

} // namespace trimesh

#endif
