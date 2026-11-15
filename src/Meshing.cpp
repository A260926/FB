#include "Meshing.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <numeric>
#include <unordered_map>

namespace meshing {

// ---------------------------------------------------------------------------
// Circumcircle predicate
// ---------------------------------------------------------------------------

bool inCircumcircle(glm::vec2 a, glm::vec2 b, glm::vec2 c, glm::vec2 p) {
	// Signed area * 2 of (a, b, c); tells us the winding order.
	const double orient = (static_cast<double>(b.x) - a.x) * (static_cast<double>(c.y) - a.y) -
	                      (static_cast<double>(c.x) - a.x) * (static_cast<double>(b.y) - a.y);
	if (orient == 0.0) {
		return false; // degenerate triangle has no proper circumcircle
	}

	const double adx = static_cast<double>(a.x) - p.x;
	const double ady = static_cast<double>(a.y) - p.y;
	const double bdx = static_cast<double>(b.x) - p.x;
	const double bdy = static_cast<double>(b.y) - p.y;
	const double cdx = static_cast<double>(c.x) - p.x;
	const double cdy = static_cast<double>(c.y) - p.y;

	const double aSq = adx * adx + ady * ady;
	const double bSq = bdx * bdx + bdy * bdy;
	const double cSq = cdx * cdx + cdy * cdy;

	double det = adx * (bdy * cSq - bSq * cdy) - ady * (bdx * cSq - bSq * cdx) + aSq * (bdx * cdy - bdy * cdx);

	// For counter-clockwise (a, b, c) the determinant is positive when p is
	// inside. Flip the sign for clockwise input so the test is winding-agnostic.
	if (orient < 0.0) {
		det = -det;
	}
	return det > 0.0;
}

// ---------------------------------------------------------------------------
// Delaunay triangulation (Bowyer-Watson)
// ---------------------------------------------------------------------------

namespace {

// Undirected edge key with a canonical (min, max) ordering for hashing.
std::uint64_t edgeKey(int u, int v) {
	if (u > v) {
		std::swap(u, v);
	}
	return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(u)) << 32) | static_cast<std::uint32_t>(v);
}

} // namespace

std::vector<std::array<int, 3>> delaunayTriangulate(const std::vector<glm::vec2> &points) {
	std::vector<std::array<int, 3>> result;
	const int n = static_cast<int>(points.size());
	if (n < 3) {
		return result;
	}

	// Working point set = input points followed by three super-triangle corners.
	std::vector<glm::vec2> pts = points;

	float minX = points[0].x, minY = points[0].y;
	float maxX = points[0].x, maxY = points[0].y;
	for (const glm::vec2 &q : points) {
		minX = std::min(minX, q.x);
		minY = std::min(minY, q.y);
		maxX = std::max(maxX, q.x);
		maxY = std::max(maxY, q.y);
	}
	const float dx = maxX - minX;
	const float dy = maxY - minY;
	float dMax = std::max(dx, dy);
	if (dMax <= 0.0f) {
		dMax = 1.0f; // all points coincident
	}
	const float midX = 0.5f * (minX + maxX);
	const float midY = 0.5f * (minY + maxY);

	const int s0 = n;
	const int s1 = n + 1;
	const int s2 = n + 2;
	pts.emplace_back(midX - 20.0f * dMax, midY - dMax);
	pts.emplace_back(midX, midY + 20.0f * dMax);
	pts.emplace_back(midX + 20.0f * dMax, midY - dMax);

	std::vector<std::array<int, 3>> tris;
	tris.push_back({s0, s1, s2});

	std::vector<std::array<int, 3>> good;
	std::unordered_map<std::uint64_t, int> edgeCount;
	std::vector<std::array<int, 2>> boundary;

	for (int ip = 0; ip < n; ++ip) {
		const glm::vec2 p = pts[ip];

		good.clear();
		edgeCount.clear();
		boundary.clear();

		// Split triangles into those whose circumcircle contains p ("bad") and
		// the rest. Accumulate the edges of the bad triangles.
		for (const std::array<int, 3> &t : tris) {
			if (inCircumcircle(pts[t[0]], pts[t[1]], pts[t[2]], p)) {
				++edgeCount[edgeKey(t[0], t[1])];
				++edgeCount[edgeKey(t[1], t[2])];
				++edgeCount[edgeKey(t[2], t[0])];
			} else {
				good.push_back(t);
			}
		}

		// Edges used by exactly one bad triangle form the hole boundary.
		auto addBoundary = [&](int u, int v) {
			if (edgeCount[edgeKey(u, v)] == 1) {
				boundary.push_back({u, v});
			}
		};
		for (const std::array<int, 3> &t : tris) {
			if (inCircumcircle(pts[t[0]], pts[t[1]], pts[t[2]], p)) {
				addBoundary(t[0], t[1]);
				addBoundary(t[1], t[2]);
				addBoundary(t[2], t[0]);
			}
		}

		tris = std::move(good);
		good.clear();
		for (const std::array<int, 2> &e : boundary) {
			tris.push_back({e[0], e[1], ip});
		}
	}

	// Drop every triangle still touching a super-triangle corner.
	for (const std::array<int, 3> &t : tris) {
		if (t[0] < n && t[1] < n && t[2] < n) {
			result.push_back(t);
		}
	}
	return result;
}

// ---------------------------------------------------------------------------
// Point cloud -> surface mesh
// ---------------------------------------------------------------------------

std::vector<ModelTriangle> pointCloudToMesh(const std::vector<glm::vec3> &points) {
	std::vector<ModelTriangle> mesh;
	if (points.size() < 3) {
		return mesh;
	}

	std::vector<glm::vec2> planar;
	planar.reserve(points.size());
	for (const glm::vec3 &p : points) {
		planar.emplace_back(p.x, p.z); // project onto the ground plane
	}

	const std::vector<std::array<int, 3>> tris = delaunayTriangulate(planar);
	mesh.reserve(tris.size());

	const Colour grey("grey", 128, 128, 128);
	for (const std::array<int, 3> &t : tris) {
		const glm::vec3 &v0 = points[t[0]];
		const glm::vec3 &v1 = points[t[1]];
		const glm::vec3 &v2 = points[t[2]];

		ModelTriangle tri(v0, v1, v2, grey);

		glm::vec3 normal = glm::cross(v1 - v0, v2 - v0);
		const float len = glm::length(normal);
		if (len > 1e-8f) {
			normal /= len;
		} else {
			normal = glm::vec3(0.0f, 1.0f, 0.0f);
		}
		// Orient consistently upward for a height field.
		if (normal.y < 0.0f) {
			normal = -normal;
		}
		tri.normal = normal;
		tri.vertexNormals = {normal, normal, normal};
		mesh.push_back(tri);
	}
	return mesh;
}

// ---------------------------------------------------------------------------
// Mesh decimation (gravitational nearest-vertex merging)
// ---------------------------------------------------------------------------

namespace {

struct VertexKey {
	long long x;
	long long y;
	long long z;
	bool operator<(const VertexKey &o) const {
		if (x != o.x) {
			return x < o.x;
		}
		if (y != o.y) {
			return y < o.y;
		}
		return z < o.z;
	}
};

VertexKey quantize(const glm::vec3 &v) {
	constexpr float scale = 1.0e4f; // ~0.1mm merge tolerance
	return {static_cast<long long>(std::llround(v.x * scale)), static_cast<long long>(std::llround(v.y * scale)),
	        static_cast<long long>(std::llround(v.z * scale))};
}

int findRoot(std::vector<int> &parent, int i) {
	while (parent[i] != i) {
		parent[i] = parent[parent[i]];
		i = parent[i];
	}
	return i;
}

} // namespace

std::vector<ModelTriangle> decimate(const std::vector<ModelTriangle> &mesh, float targetRatio) {
	std::vector<ModelTriangle> out;
	const int originalCount = static_cast<int>(mesh.size());
	if (originalCount == 0) {
		return out;
	}
	const float ratio = std::clamp(targetRatio, 0.0f, 1.0f);

	// 1. Build a unique vertex list and remap each triangle onto it.
	std::map<VertexKey, int> lookup;
	std::vector<glm::vec3> verts;
	std::vector<std::array<int, 3>> faces(mesh.size());

	auto vertexIndex = [&](const glm::vec3 &v) {
		const VertexKey key = quantize(v);
		auto it = lookup.find(key);
		if (it != lookup.end()) {
			return it->second;
		}
		const int idx = static_cast<int>(verts.size());
		lookup.emplace(key, idx);
		verts.push_back(v);
		return idx;
	};

	for (std::size_t f = 0; f < mesh.size(); ++f) {
		faces[f] = {vertexIndex(mesh[f].vertices[0]), vertexIndex(mesh[f].vertices[1]),
		            vertexIndex(mesh[f].vertices[2])};
	}
	const int vCount = static_cast<int>(verts.size());

	// 2. Identify anchor vertices: boundary edges (used by a single triangle)
	//    and sharp creases (large angle between incident face normals).
	std::vector<char> anchor(vCount, 0);

	std::unordered_map<std::uint64_t, int> edgeUse;
	std::vector<glm::vec3> faceNormal(faces.size(), glm::vec3(0.0f));
	for (std::size_t f = 0; f < faces.size(); ++f) {
		const std::array<int, 3> &t = faces[f];
		++edgeUse[edgeKey(t[0], t[1])];
		++edgeUse[edgeKey(t[1], t[2])];
		++edgeUse[edgeKey(t[2], t[0])];

		glm::vec3 nrm = glm::cross(verts[t[1]] - verts[t[0]], verts[t[2]] - verts[t[0]]);
		const float len = glm::length(nrm);
		faceNormal[f] = (len > 1e-8f) ? nrm / len : glm::vec3(0.0f);
	}
	for (const auto &[key, count] : edgeUse) {
		if (count == 1) {
			anchor[static_cast<int>(key >> 32)] = 1;
			anchor[static_cast<int>(key & 0xffffffffu)] = 1;
		}
	}

	// Sharp-crease detection: compare incident face normals per vertex.
	std::vector<std::vector<int>> vertFaces(vCount);
	for (std::size_t f = 0; f < faces.size(); ++f) {
		for (int k = 0; k < 3; ++k) {
			vertFaces[faces[f][k]].push_back(static_cast<int>(f));
		}
	}
	constexpr float sharpCos = 0.5f; // ~60 degrees
	for (int v = 0; v < vCount; ++v) {
		const std::vector<int> &fs = vertFaces[v];
		bool sharp = false;
		for (std::size_t i = 0; i < fs.size() && !sharp; ++i) {
			for (std::size_t j = i + 1; j < fs.size(); ++j) {
				if (glm::dot(faceNormal[fs[i]], faceNormal[fs[j]]) < sharpCos) {
					sharp = true;
					break;
				}
			}
		}
		if (sharp) {
			anchor[v] = 1;
		}
	}

	// 3. Collect collapsible edges (at least one non-anchor endpoint), shortest
	//    first, and repeatedly merge until the triangle budget is met.
	struct Edge {
		int u;
		int v;
		float lengthSq;
	};
	std::vector<Edge> edges;
	{
		std::unordered_map<std::uint64_t, char> seen;
		auto tryAdd = [&](int a, int b) {
			const std::uint64_t key = edgeKey(a, b);
			if (seen.emplace(key, 1).second) {
				edges.push_back({a, b, glm::dot(verts[a] - verts[b], verts[a] - verts[b])});
			}
		};
		for (const std::array<int, 3> &t : faces) {
			tryAdd(t[0], t[1]);
			tryAdd(t[1], t[2]);
			tryAdd(t[2], t[0]);
		}
	}
	std::sort(edges.begin(), edges.end(), [](const Edge &a, const Edge &b) { return a.lengthSq < b.lengthSq; });

	std::vector<int> parent(vCount);
	std::iota(parent.begin(), parent.end(), 0);
	std::vector<glm::vec3> pos = verts;
	std::vector<char> rootAnchor = anchor;

	auto liveTriangleCount = [&]() {
		int live = 0;
		for (const std::array<int, 3> &t : faces) {
			const int a = findRoot(parent, t[0]);
			const int b = findRoot(parent, t[1]);
			const int c = findRoot(parent, t[2]);
			if (a != b && b != c && a != c) {
				++live;
			}
		}
		return live;
	};

	const int targetCount = std::max(1, static_cast<int>(std::lround(originalCount * ratio)));
	int current = liveTriangleCount();

	for (const Edge &e : edges) {
		if (current <= targetCount) {
			break;
		}
		const int ru = findRoot(parent, e.u);
		const int rv = findRoot(parent, e.v);
		if (ru == rv) {
			continue;
		}
		if (rootAnchor[ru] && rootAnchor[rv]) {
			continue; // never collapse an anchor-to-anchor edge
		}

		int keep = ru;
		int drop = rv;
		glm::vec3 newPos;
		if (rootAnchor[ru]) {
			newPos = pos[ru]; // anchor stays put, other endpoint gravitates in
		} else if (rootAnchor[rv]) {
			keep = rv;
			drop = ru;
			newPos = pos[rv];
		} else {
			newPos = 0.5f * (pos[ru] + pos[rv]); // both free: merge to midpoint
		}

		parent[drop] = keep;
		pos[keep] = newPos;
		rootAnchor[keep] = static_cast<char>(rootAnchor[ru] || rootAnchor[rv]);
		current = liveTriangleCount();
	}

	// 4. Rebuild surviving triangles at their merged positions.
	out.reserve(static_cast<std::size_t>(current));
	for (std::size_t f = 0; f < faces.size(); ++f) {
		const int a = findRoot(parent, faces[f][0]);
		const int b = findRoot(parent, faces[f][1]);
		const int c = findRoot(parent, faces[f][2]);
		if (a == b || b == c || a == c) {
			continue; // collapsed to a sliver / point
		}

		ModelTriangle tri = mesh[f];
		tri.vertices = {pos[a], pos[b], pos[c]};

		glm::vec3 nrm = glm::cross(pos[b] - pos[a], pos[c] - pos[a]);
		const float len = glm::length(nrm);
		if (len <= 1e-10f) {
			continue; // degenerate area
		}
		nrm /= len;
		if (glm::dot(nrm, tri.normal) < 0.0f) {
			nrm = -nrm; // preserve original facing
		}
		tri.normal = nrm;
		tri.vertexNormals = {nrm, nrm, nrm};
		out.push_back(tri);
	}
	return out;
}

} // namespace meshing
