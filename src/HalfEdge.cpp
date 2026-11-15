#include "HalfEdge.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <map>
#include <tuple>
#include <utility>
#include <vector>

#include <glm/glm.hpp>

namespace halfedge {

namespace {

constexpr float kPi = 3.14159265358979323846f;

// Quantised position key used to weld coincident vertices when rebuilding a
// mesh from a triangle soup. A fixed grid (1e-5) tolerates the tiny rounding
// that creeps in through repeated subdivision while still fusing the vertices
// that are genuinely shared across neighbouring faces.
using VertexKey = std::tuple<long long, long long, long long>;

VertexKey quantise(const glm::vec3 &p) {
	const double scale = 100000.0;
	return VertexKey{static_cast<long long>(std::llround(p.x * scale)),
	                 static_cast<long long>(std::llround(p.y * scale)),
	                 static_cast<long long>(std::llround(p.z * scale))};
}

// Undirected edge key: the two endpoint indices in ascending order so a
// half-edge and its twin hash to the same slot.
std::pair<int, int> edgeKey(int a, int b) {
	return (a < b) ? std::pair<int, int>{a, b} : std::pair<int, int>{b, a};
}

glm::vec3 faceNormal(const glm::vec3 &a, const glm::vec3 &b, const glm::vec3 &c) {
	glm::vec3 n = glm::cross(b - a, c - a);
	float len = glm::length(n);
	return (len > 0.0f) ? n / len : glm::vec3(0.0f);
}

ModelTriangle makeTriangle(const glm::vec3 &a, const glm::vec3 &b, const glm::vec3 &c, const Colour &colour) {
	ModelTriangle t(a, b, c, colour);
	t.normal = faceNormal(a, b, c);
	t.vertexNormals[0] = t.normal;
	t.vertexNormals[1] = t.normal;
	t.vertexNormals[2] = t.normal;
	return t;
}

} // namespace

void HalfEdgeMesh::build(const std::vector<ModelTriangle> &triangles) {
	verts.clear();
	halfEdges.clear();

	// Weld vertices by quantised position so shared edges are discovered.
	std::map<VertexKey, int> lookup;
	auto vertexIndex = [&](const glm::vec3 &p) -> int {
		VertexKey key = quantise(p);
		auto it = lookup.find(key);
		if (it != lookup.end())
			return it->second;
		int idx = static_cast<int>(verts.size());
		verts.push_back(p);
		lookup.emplace(key, idx);
		return idx;
	};

	halfEdges.reserve(triangles.size() * 3);
	for (const ModelTriangle &tri : triangles) {
		int base = static_cast<int>(halfEdges.size());
		int i0 = vertexIndex(tri.vertices[0]);
		int i1 = vertexIndex(tri.vertices[1]);
		int i2 = vertexIndex(tri.vertices[2]);

		HalfEdge e0{i0, base + 1, -1};
		HalfEdge e1{i1, base + 2, -1};
		HalfEdge e2{i2, base + 0, -1};
		halfEdges.push_back(e0);
		halfEdges.push_back(e1);
		halfEdges.push_back(e2);
	}

	// Match twins by hashing undirected vertex pairs. The first half-edge seen
	// for a pair waits in the map; the second one pairs with it. Any further
	// half-edges on the same edge (non-manifold input) are left unmatched.
	std::map<std::pair<int, int>, int> pending;
	for (int h = 0; h < static_cast<int>(halfEdges.size()); ++h) {
		int a = halfEdges[h].origin;
		int b = halfEdges[halfEdges[h].next].origin;
		std::pair<int, int> key = edgeKey(a, b);
		auto it = pending.find(key);
		if (it == pending.end()) {
			pending.emplace(key, h);
		} else if (it->second >= 0) {
			int other = it->second;
			halfEdges[h].twin = other;
			halfEdges[other].twin = h;
			it->second = -1; // consumed; mark so a third edge stays a boundary
		}
	}
}

std::vector<ModelTriangle> HalfEdgeMesh::toTriangles(const Colour &colour) const {
	std::vector<ModelTriangle> out;
	out.reserve(faceCount());
	for (int f = 0; f < faceCount(); ++f) {
		int h0 = 3 * f;
		const glm::vec3 &a = verts[halfEdges[h0].origin];
		const glm::vec3 &b = verts[halfEdges[h0 + 1].origin];
		const glm::vec3 &c = verts[halfEdges[h0 + 2].origin];
		out.push_back(makeTriangle(a, b, c, colour));
	}
	return out;
}

namespace {

// One level of Loop subdivision on a half-edge mesh.
std::vector<ModelTriangle> loopOnce(const HalfEdgeMesh &he, const Colour &colour) {
	const int V = static_cast<int>(he.verts.size());

	// Collect the undirected edges, their two apex vertices (the third vertex
	// of each incident triangle) and whether they lie on a boundary.
	struct EdgeInfo {
		int a = -1;
		int b = -1;
		std::array<int, 2> apex{-1, -1};
		int apexCount = 0;
		bool boundary = false;
	};
	std::map<std::pair<int, int>, int> edgeId;
	std::vector<EdgeInfo> edges;

	auto apexOf = [&](int h) -> int {
		// h goes a -> b; its apex is the destination of next(h).
		return he.destination(he.halfEdges[h].next);
	};

	for (int h = 0; h < static_cast<int>(he.halfEdges.size()); ++h) {
		int a = he.halfEdges[h].origin;
		int b = he.destination(h);
		std::pair<int, int> key = edgeKey(a, b);
		auto it = edgeId.find(key);
		int id;
		if (it == edgeId.end()) {
			id = static_cast<int>(edges.size());
			edgeId.emplace(key, id);
			EdgeInfo info;
			info.a = key.first;
			info.b = key.second;
			edges.push_back(info);
		} else {
			id = it->second;
		}
		EdgeInfo &info = edges[id];
		if (info.apexCount < 2)
			info.apex[info.apexCount++] = apexOf(h);
		if (he.halfEdges[h].twin < 0)
			info.boundary = true;
	}

	// One-ring neighbours and boundary neighbours per vertex (each undirected
	// edge is visited once, so neighbour lists carry no duplicates).
	std::vector<std::vector<int>> neigh(V);
	std::vector<std::vector<int>> boundaryNeigh(V);
	for (const EdgeInfo &e : edges) {
		neigh[e.a].push_back(e.b);
		neigh[e.b].push_back(e.a);
		if (e.boundary) {
			boundaryNeigh[e.a].push_back(e.b);
			boundaryNeigh[e.b].push_back(e.a);
		}
	}

	std::vector<glm::vec3> newVerts(V + edges.size());

	// Reposition the original (even) vertices.
	for (int v = 0; v < V; ++v) {
		const glm::vec3 &P = he.verts[v];
		if (!boundaryNeigh[v].empty()) {
			glm::vec3 sum(0.0f);
			for (int nb : boundaryNeigh[v])
				sum += he.verts[nb];
			newVerts[v] = 0.75f * P + 0.125f * sum;
		} else {
			int n = static_cast<int>(neigh[v].size());
			if (n == 0) {
				newVerts[v] = P;
			} else {
				float c = 0.375f + 0.25f * std::cos(2.0f * kPi / static_cast<float>(n));
				float beta = (1.0f / static_cast<float>(n)) * (0.625f - c * c);
				glm::vec3 sum(0.0f);
				for (int nb : neigh[v])
					sum += he.verts[nb];
				newVerts[v] = (1.0f - static_cast<float>(n) * beta) * P + beta * sum;
			}
		}
	}

	// Odd (edge) vertices.
	for (int e = 0; e < static_cast<int>(edges.size()); ++e) {
		const EdgeInfo &info = edges[e];
		const glm::vec3 &va = he.verts[info.a];
		const glm::vec3 &vb = he.verts[info.b];
		glm::vec3 p;
		if (info.boundary || info.apexCount < 2) {
			p = 0.5f * (va + vb);
		} else {
			p = 0.375f * (va + vb) + 0.125f * (he.verts[info.apex[0]] + he.verts[info.apex[1]]);
		}
		newVerts[V + e] = p;
	}

	// Split every triangle into four.
	std::vector<ModelTriangle> out;
	out.reserve(he.faceCount() * 4);
	for (int f = 0; f < he.faceCount(); ++f) {
		int h0 = 3 * f;
		int a = he.halfEdges[h0].origin;
		int b = he.halfEdges[h0 + 1].origin;
		int c = he.halfEdges[h0 + 2].origin;
		int mAB = V + edgeId[edgeKey(a, b)];
		int mBC = V + edgeId[edgeKey(b, c)];
		int mCA = V + edgeId[edgeKey(c, a)];

		out.push_back(makeTriangle(newVerts[a], newVerts[mAB], newVerts[mCA], colour));
		out.push_back(makeTriangle(newVerts[mAB], newVerts[b], newVerts[mBC], colour));
		out.push_back(makeTriangle(newVerts[mCA], newVerts[mBC], newVerts[c], colour));
		out.push_back(makeTriangle(newVerts[mAB], newVerts[mBC], newVerts[mCA], colour));
	}
	return out;
}

} // namespace

std::vector<ModelTriangle> loopSubdivide(const std::vector<ModelTriangle> &mesh, int levels, const Colour &colour) {
	std::vector<ModelTriangle> current = mesh;
	for (int level = 0; level < levels; ++level) {
		HalfEdgeMesh he;
		he.build(current);
		current = loopOnce(he, colour);
	}
	return current;
}

namespace {

// Lightweight polygon mesh used internally by Catmull-Clark. Faces are stored
// as ordered vertex-index loops so the scheme can iterate on genuine quads
// across levels and only triangulate at the very end.
struct PolyMesh {
	std::vector<glm::vec3> verts;
	std::vector<std::vector<int>> faces;
};

PolyMesh trianglesToPolyMesh(const std::vector<ModelTriangle> &mesh) {
	PolyMesh pm;
	std::map<VertexKey, int> lookup;
	auto vertexIndex = [&](const glm::vec3 &p) -> int {
		VertexKey key = quantise(p);
		auto it = lookup.find(key);
		if (it != lookup.end())
			return it->second;
		int idx = static_cast<int>(pm.verts.size());
		pm.verts.push_back(p);
		lookup.emplace(key, idx);
		return idx;
	};
	for (const ModelTriangle &tri : mesh) {
		pm.faces.push_back({vertexIndex(tri.vertices[0]), vertexIndex(tri.vertices[1]), vertexIndex(tri.vertices[2])});
	}
	return pm;
}

PolyMesh catmullClarkOnce(const PolyMesh &in) {
	const int V = static_cast<int>(in.verts.size());
	const int F = static_cast<int>(in.faces.size());

	// Face points: centroid of each face.
	std::vector<glm::vec3> facePoint(F, glm::vec3(0.0f));
	for (int f = 0; f < F; ++f) {
		glm::vec3 sum(0.0f);
		for (int v : in.faces[f])
			sum += in.verts[v];
		facePoint[f] = sum / static_cast<float>(in.faces[f].size());
	}

	// Edges: endpoints and the (one or two) incident faces.
	struct EdgeInfo {
		int a = -1;
		int b = -1;
		std::array<int, 2> face{-1, -1};
		int faceCount = 0;
	};
	std::map<std::pair<int, int>, int> edgeId;
	std::vector<EdgeInfo> edges;
	auto edgeIndex = [&](int a, int b, int f) -> int {
		std::pair<int, int> key = edgeKey(a, b);
		auto it = edgeId.find(key);
		int id;
		if (it == edgeId.end()) {
			id = static_cast<int>(edges.size());
			edgeId.emplace(key, id);
			EdgeInfo info;
			info.a = key.first;
			info.b = key.second;
			edges.push_back(info);
		} else {
			id = it->second;
		}
		if (edges[id].faceCount < 2)
			edges[id].face[edges[id].faceCount++] = f;
		return id;
	};
	for (int f = 0; f < F; ++f) {
		const std::vector<int> &face = in.faces[f];
		int k = static_cast<int>(face.size());
		for (int i = 0; i < k; ++i)
			edgeIndex(face[i], face[(i + 1) % k], f);
	}

	// Edge points: average of endpoints and adjacent face points (interior) or
	// the plain midpoint (boundary).
	std::vector<glm::vec3> edgePoint(edges.size(), glm::vec3(0.0f));
	std::vector<bool> edgeBoundary(edges.size(), false);
	for (int e = 0; e < static_cast<int>(edges.size()); ++e) {
		const EdgeInfo &info = edges[e];
		glm::vec3 mid = 0.5f * (in.verts[info.a] + in.verts[info.b]);
		if (info.faceCount < 2) {
			edgeBoundary[e] = true;
			edgePoint[e] = mid;
		} else {
			edgePoint[e] =
			    0.25f * (in.verts[info.a] + in.verts[info.b] + facePoint[info.face[0]] + facePoint[info.face[1]]);
		}
	}

	// Per-vertex accumulation for the vertex update rule.
	std::vector<glm::vec3> faceAvg(V, glm::vec3(0.0f));
	std::vector<int> faceCnt(V, 0);
	std::vector<glm::vec3> edgeMidAvg(V, glm::vec3(0.0f));
	std::vector<int> edgeCnt(V, 0);
	std::vector<glm::vec3> boundarySum(V, glm::vec3(0.0f));
	std::vector<int> boundaryCnt(V, 0);

	for (int f = 0; f < F; ++f) {
		for (int v : in.faces[f]) {
			faceAvg[v] += facePoint[f];
			faceCnt[v]++;
		}
	}
	for (int e = 0; e < static_cast<int>(edges.size()); ++e) {
		const EdgeInfo &info = edges[e];
		glm::vec3 mid = 0.5f * (in.verts[info.a] + in.verts[info.b]);
		edgeMidAvg[info.a] += mid;
		edgeCnt[info.a]++;
		edgeMidAvg[info.b] += mid;
		edgeCnt[info.b]++;
		if (edgeBoundary[e]) {
			boundarySum[info.a] += in.verts[info.b];
			boundaryCnt[info.a]++;
			boundarySum[info.b] += in.verts[info.a];
			boundaryCnt[info.b]++;
		}
	}

	PolyMesh out;
	out.verts.resize(V + F + edges.size());

	// Updated original vertices.
	for (int v = 0; v < V; ++v) {
		const glm::vec3 &P = in.verts[v];
		if (boundaryCnt[v] > 0) {
			out.verts[v] = 0.75f * P + 0.125f * boundarySum[v];
		} else if (edgeCnt[v] == 0) {
			out.verts[v] = P;
		} else {
			int n = edgeCnt[v];
			glm::vec3 Favg = faceAvg[v] / static_cast<float>(faceCnt[v]);
			glm::vec3 Ravg = edgeMidAvg[v] / static_cast<float>(edgeCnt[v]);
			out.verts[v] = (Favg + 2.0f * Ravg + static_cast<float>(n - 3) * P) / static_cast<float>(n);
		}
	}
	for (int f = 0; f < F; ++f)
		out.verts[V + f] = facePoint[f];
	for (int e = 0; e < static_cast<int>(edges.size()); ++e)
		out.verts[V + F + e] = edgePoint[e];

	// New quad faces: one per corner of every original face.
	for (int f = 0; f < F; ++f) {
		const std::vector<int> &face = in.faces[f];
		int k = static_cast<int>(face.size());
		for (int i = 0; i < k; ++i) {
			int vi = face[i];
			int ePrev = V + F + edgeId[edgeKey(face[(i + k - 1) % k], vi)];
			int eNext = V + F + edgeId[edgeKey(vi, face[(i + 1) % k])];
			int fp = V + f;
			out.faces.push_back({vi, eNext, fp, ePrev});
		}
	}
	return out;
}

} // namespace

std::vector<ModelTriangle> catmullClarkToQuadsTriangulated(const std::vector<ModelTriangle> &mesh, int levels,
                                                           const Colour &colour) {
	PolyMesh pm = trianglesToPolyMesh(mesh);
	for (int level = 0; level < levels; ++level)
		pm = catmullClarkOnce(pm);

	// Triangulate each face by a simple fan for output.
	std::vector<ModelTriangle> out;
	for (const std::vector<int> &face : pm.faces) {
		for (int i = 1; i + 1 < static_cast<int>(face.size()); ++i) {
			out.push_back(makeTriangle(pm.verts[face[0]], pm.verts[face[i]], pm.verts[face[i + 1]], colour));
		}
	}
	return out;
}

} // namespace halfedge
