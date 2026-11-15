#pragma once

#include <ModelTriangle.h>
#include <RayTriangleIntersection.h>
#include <glm/glm.hpp>
#include <vector>

// A uniform grid over a triangle mesh: the scene AABB is diced into a regular
// lattice of equal-sized voxels, each triangle is binned into every voxel its
// AABB overlaps, and rays are marched voxel to voxel with a 3D-DDA (the
// Amanatides and Woo algorithm). Same accelerated ray-cast interface as
// BVH / Octree / KdTree, so it is a drop-in alternative.
class Grid {
public:
	explicit Grid(const std::vector<ModelTriangle> &triangles);

	// Closest hit (like getClosestIntersection but accelerated). `ignoreIndex`
	// skips one triangle (for reflection/refraction self-hit avoidance).
	RayTriangleIntersection intersect(const glm::vec3 &origin, const glm::vec3 &direction, int ignoreIndex = -1) const;

	// True if anything is hit strictly before `maxDistance` (shadow test).
	bool occluded(const glm::vec3 &origin, const glm::vec3 &direction, float maxDistance, int ignoreIndex) const;

private:
	const std::vector<ModelTriangle> *tris;

	glm::vec3 bmin{0.0f}, bmax{0.0f}; // scene bounding box
	glm::vec3 cellSize{1.0f};         // world-space size of one voxel
	glm::vec3 invCellSize{1.0f};      // 1 / cellSize (componentwise)
	int res[3] = {1, 1, 1};           // voxel count per axis (nx, ny, nz)

	// CSR-style bins: `cellStart[c] .. cellStart[c + 1]` is the slice of
	// `cellTris` holding the triangle indices that overlap voxel `c`.
	std::vector<int> cellStart;
	std::vector<int> cellTris;

	// Flat voxel index from integer coordinates (x + nx * (y + ny * z)).
	int cellIndex(int x, int y, int z) const { return x + res[0] * (y + res[1] * z); }
};
