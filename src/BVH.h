#pragma once

#include <ModelTriangle.h>
#include <RayTriangleIntersection.h>
#include <glm/glm.hpp>
#include <vector>

// A bounding-volume hierarchy over a triangle mesh: median-split build, AABB
// slab traversal. Turns brute-force O(n) ray casts into roughly O(log n).
class BVH {
public:
	explicit BVH(const std::vector<ModelTriangle> &triangles);

	// Closest hit (like getClosestIntersection but accelerated). `ignoreIndex`
	// skips one triangle (for reflection/refraction self-hit avoidance).
	RayTriangleIntersection intersect(const glm::vec3 &origin, const glm::vec3 &direction, int ignoreIndex = -1) const;

	// True if anything is hit strictly before `maxDistance` (shadow test).
	bool occluded(const glm::vec3 &origin, const glm::vec3 &direction, float maxDistance, int ignoreIndex) const;

private:
	struct Node {
		glm::vec3 bmin, bmax;
		int left = -1, right = -1; // children (internal nodes)
		int start = 0, count = 0;  // triangle range in `order` (leaves have count > 0)
	};
	const std::vector<ModelTriangle> *tris;
	std::vector<glm::vec3> centroids;
	std::vector<int> order;
	std::vector<Node> nodes;
	int root = -1;

	int build(int start, int count);
};
