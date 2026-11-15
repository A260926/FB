#pragma once

#include <vector>

#include <glm/glm.hpp>

#include "Colour.h"
#include "ModelTriangle.h"

// Half-edge mesh and subdivision surfaces.
//
// Implements the index-based half-edge representation described in Fabian
// Giesen's "ryg" half-edge posts, plus Loop subdivision for triangle meshes
// (Charles Loop's thesis, with the Warren beta weights) and a Catmull-Clark
// pass that produces triangulated quad faces.
//
// The half-edge data structure gives O(1) local connectivity queries (the
// triangles around a vertex, the neighbours of a face) which the subdivision
// schemes need in order to gather the one-ring of every vertex.
namespace halfedge {

// Index-based half-edge mesh built from a triangle soup.
//
// Vertices live in `verts`. Every face owns three consecutive half-edges, so
// half-edge i belongs to face i / 3. For each half-edge we store its origin
// vertex, its `next` half-edge around the same face, and its `twin` (the
// oppositely-oriented half-edge in the neighbouring face, or -1 on a boundary).
struct HalfEdgeMesh {
	struct HalfEdge {
		int origin = -1; // vertex index this half-edge starts from
		int next = -1;   // next half-edge around the same face (CCW)
		int twin = -1;   // opposite half-edge, or -1 if this edge is a boundary
	};

	std::vector<glm::vec3> verts;
	std::vector<HalfEdge> halfEdges;

	// Number of triangular faces (three half-edges each).
	int faceCount() const { return static_cast<int>(halfEdges.size() / 3); }

	// The half-edge that terminates half-edge h (i.e. its `next`'s origin is the
	// destination of h).
	int destination(int h) const { return halfEdges[halfEdges[h].next].origin; }

	// Rebuilds the mesh from a triangle soup. Vertices are welded by exact
	// position so that shared edges are discovered, then twins are matched by
	// hashing undirected (sorted) vertex pairs.
	void build(const std::vector<ModelTriangle> &triangles);

	// Emits the faces back out as ModelTriangles with the supplied colour and
	// per-face normals.
	std::vector<ModelTriangle> toTriangles(const Colour &colour) const;
};

// Loop subdivision for triangle meshes, applied `levels` times.
//
// Each level: every edge gains an odd (edge) vertex, every original even
// vertex is repositioned with the Loop/Warren beta weights, and every triangle
// is split into four. Boundary edges and boundary vertices use the 1/2, 1/2
// rules so open meshes stay watertight along their border.
std::vector<ModelTriangle> loopSubdivide(const std::vector<ModelTriangle> &mesh, int levels, const Colour &colour);

// Catmull-Clark subdivision applied `levels` times, with the resulting quad
// faces triangulated (each quad becomes two triangles) so the output is a
// triangle mesh usable by the rest of the renderer. Handles boundaries with the
// standard 1/2 edge-point and boundary-vertex rules.
std::vector<ModelTriangle> catmullClarkToQuadsTriangulated(const std::vector<ModelTriangle> &mesh, int levels,
                                                           const Colour &colour);

} // namespace halfedge
