#include "Octree.h"

#include "Geometry.h"
#include <algorithm>

Octree::Octree(const std::vector<ModelTriangle> &triangles) : tris(&triangles) {
	int n = static_cast<int>(triangles.size());
	order.resize(n);
	centroids.resize(n);
	glm::vec3 bmin(1e30f), bmax(-1e30f);
	for (int i = 0; i < n; i++) {
		order[i] = i;
		centroids[i] = (triangles[i].vertices[0] + triangles[i].vertices[1] + triangles[i].vertices[2]) / 3.0f;
		for (const glm::vec3 &v : triangles[i].vertices) {
			bmin = glm::min(bmin, v);
			bmax = glm::max(bmax, v);
		}
	}
	if (n > 0)
		root = build(0, n, bmin, bmax, 0);
}

int Octree::build(int start, int count, const glm::vec3 &, const glm::vec3 &, int depth) {
	Node node;
	// Tight AABB over the triangles in this range (drives the slab test).
	glm::vec3 bmin(1e30f), bmax(-1e30f);
	for (int i = start; i < start + count; i++) {
		const ModelTriangle &t = (*tris)[order[i]];
		for (const glm::vec3 &v : t.vertices) {
			bmin = glm::min(bmin, v);
			bmax = glm::max(bmax, v);
		}
	}
	node.bmin = bmin;
	node.bmax = bmax;

	auto makeLeaf = [&]() {
		node.start = start;
		node.count = count;
		nodes.push_back(node);
		return static_cast<int>(nodes.size()) - 1;
	};

	if (count <= 4 || depth >= 16)
		return makeLeaf();

	// Split the range into eight octants around the box centre by centroid.
	glm::vec3 center = 0.5f * (bmin + bmax);
	int counts[8] = {0, 0, 0, 0, 0, 0, 0, 0};
	std::vector<int> octant(count);
	for (int i = 0; i < count; i++) {
		const glm::vec3 &c = centroids[order[start + i]];
		int o = (c.x >= center.x ? 1 : 0) | (c.y >= center.y ? 2 : 0) | (c.z >= center.z ? 4 : 0);
		octant[i] = o;
		counts[o]++;
	}

	// Degenerate: everything landed in one octant, no progress possible.
	int used = 0;
	for (int o = 0; o < 8; o++)
		if (counts[o] > 0)
			used++;
	if (used <= 1)
		return makeLeaf();

	int offset[8];
	int acc = 0;
	for (int o = 0; o < 8; o++) {
		offset[o] = acc;
		acc += counts[o];
	}
	std::vector<int> sorted(count);
	int cursor[8];
	for (int o = 0; o < 8; o++)
		cursor[o] = offset[o];
	for (int i = 0; i < count; i++)
		sorted[cursor[octant[i]]++] = order[start + i];
	for (int i = 0; i < count; i++)
		order[start + i] = sorted[i];

	node.count = 0;
	int myIndex = static_cast<int>(nodes.size());
	nodes.push_back(node); // reserve slot before recursing
	for (int o = 0; o < 8; o++) {
		if (counts[o] == 0)
			continue;
		int child = build(start + offset[o], counts[o], bmin, bmax, depth + 1);
		nodes[myIndex].children[o] = child;
	}
	return myIndex;
}

// Ray vs AABB (slab test). invDir is 1/direction componentwise.
static bool slabHit(const glm::vec3 &origin, const glm::vec3 &invDir, const glm::vec3 &bmin, const glm::vec3 &bmax,
                    float tMax) {
	glm::vec3 t0 = (bmin - origin) * invDir;
	glm::vec3 t1 = (bmax - origin) * invDir;
	glm::vec3 tsmall = glm::min(t0, t1);
	glm::vec3 tbig = glm::max(t0, t1);
	float tenter = std::max({tsmall.x, tsmall.y, tsmall.z});
	float texit = std::min({tbig.x, tbig.y, tbig.z});
	return texit >= std::max(tenter, 0.0f) && tenter < tMax;
}

RayTriangleIntersection Octree::intersect(const glm::vec3 &origin, const glm::vec3 &direction, int ignoreIndex) const {
	RayTriangleIntersection closest;
	if (root < 0)
		return closest;
	glm::vec3 invDir = 1.0f / direction;
	int stack[256];
	int sp = 0;
	stack[sp++] = root;
	while (sp > 0) {
		const Node &node = nodes[stack[--sp]];
		if (!slabHit(origin, invDir, node.bmin, node.bmax, closest.distanceFromCamera))
			continue;
		if (node.count > 0) {
			for (int i = node.start; i < node.start + node.count; i++) {
				int ti = order[i];
				if (ti == ignoreIndex)
					continue;
				float t, u, v;
				if (intersectTriangle(origin, direction, (*tris)[ti], t, u, v) && t < closest.distanceFromCamera) {
					closest.hit = true;
					closest.distanceFromCamera = t;
					closest.intersectionPoint = origin + t * direction;
					closest.intersectedTriangle = (*tris)[ti];
					closest.triangleIndex = static_cast<size_t>(ti);
					closest.u = u;
					closest.v = v;
				}
			}
		} else {
			for (int o = 0; o < 8; o++)
				if (node.children[o] >= 0)
					stack[sp++] = node.children[o];
		}
	}
	return closest;
}

bool Octree::occluded(const glm::vec3 &origin, const glm::vec3 &direction, float maxDistance, int ignoreIndex) const {
	if (root < 0)
		return false;
	glm::vec3 invDir = 1.0f / direction;
	int stack[256];
	int sp = 0;
	stack[sp++] = root;
	while (sp > 0) {
		const Node &node = nodes[stack[--sp]];
		if (!slabHit(origin, invDir, node.bmin, node.bmax, maxDistance))
			continue;
		if (node.count > 0) {
			for (int i = node.start; i < node.start + node.count; i++) {
				int ti = order[i];
				if (ti == ignoreIndex)
					continue;
				float t, u, v;
				if (intersectTriangle(origin, direction, (*tris)[ti], t, u, v) && t < maxDistance)
					return true;
			}
		} else {
			for (int o = 0; o < 8; o++)
				if (node.children[o] >= 0)
					stack[sp++] = node.children[o];
		}
	}
	return false;
}
