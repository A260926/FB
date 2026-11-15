#include "BVH.h"

#include "Geometry.h"
#include <algorithm>

BVH::BVH(const std::vector<ModelTriangle> &triangles) : tris(&triangles) {
	int n = static_cast<int>(triangles.size());
	order.resize(n);
	centroids.resize(n);
	for (int i = 0; i < n; i++) {
		order[i] = i;
		centroids[i] = (triangles[i].vertices[0] + triangles[i].vertices[1] + triangles[i].vertices[2]) / 3.0f;
	}
	if (n > 0)
		root = build(0, n);
}

int BVH::build(int start, int count) {
	Node node;
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

	if (count <= 4)
		return makeLeaf();

	// Split at the median centroid along the widest centroid axis.
	glm::vec3 cmin(1e30f), cmax(-1e30f);
	for (int i = start; i < start + count; i++) {
		cmin = glm::min(cmin, centroids[order[i]]);
		cmax = glm::max(cmax, centroids[order[i]]);
	}
	glm::vec3 ext = cmax - cmin;
	int axis = (ext.x > ext.y && ext.x > ext.z) ? 0 : (ext.y > ext.z ? 1 : 2);
	int mid = start + count / 2;
	std::nth_element(order.begin() + start, order.begin() + mid, order.begin() + start + count,
	                 [&](int a, int b) { return centroids[a][axis] < centroids[b][axis]; });
	if (mid == start || mid == start + count)
		return makeLeaf(); // degenerate (all centroids coincident)

	node.count = 0;
	int myIndex = static_cast<int>(nodes.size());
	nodes.push_back(node); // reserve slot before recursing
	int leftChild = build(start, mid - start);
	int rightChild = build(mid, start + count - mid);
	nodes[myIndex].left = leftChild;
	nodes[myIndex].right = rightChild;
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

RayTriangleIntersection BVH::intersect(const glm::vec3 &origin, const glm::vec3 &direction, int ignoreIndex) const {
	RayTriangleIntersection closest;
	if (root < 0)
		return closest;
	glm::vec3 invDir = 1.0f / direction;
	int stack[64];
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
			stack[sp++] = node.left;
			stack[sp++] = node.right;
		}
	}
	return closest;
}

bool BVH::occluded(const glm::vec3 &origin, const glm::vec3 &direction, float maxDistance, int ignoreIndex) const {
	if (root < 0)
		return false;
	glm::vec3 invDir = 1.0f / direction;
	int stack[64];
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
			stack[sp++] = node.left;
			stack[sp++] = node.right;
		}
	}
	return false;
}
