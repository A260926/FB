#pragma once

#include <Colour.h>
#include <ModelTriangle.h>
#include <cstdint>
#include <glm/glm.hpp>
#include <vector>

// A dense axis-aligned voxel grid with per-cell occupancy and colour. The grid
// spans the world-space box [minBound, maxBound] divided into nx*ny*nz cells.
class VoxelGrid {
public:
	VoxelGrid(int nx, int ny, int nz, glm::vec3 minBound, glm::vec3 maxBound);

	// Mark a cell solid and give it a colour. Out-of-range indices are ignored.
	void set(int x, int y, int z, Colour c);

	// Clear a cell.
	void clear(int x, int y, int z);

	// True when the cell exists and is occupied. Out-of-range reads are empty.
	bool solid(int x, int y, int z) const;

	// Colour of a cell (black when empty / out of range).
	Colour colourAt(int x, int y, int z) const;

	int nx() const { return dimX; }
	int ny() const { return dimY; }
	int nz() const { return dimZ; }

	glm::vec3 minCorner() const { return lo; }
	glm::vec3 maxCorner() const { return hi; }

	// Edge length of one cell along each axis.
	glm::vec3 cellSize() const { return step; }

	// World-space centre of a cell.
	glm::vec3 voxelCentre(int x, int y, int z) const;

	// World-space min corner of a cell.
	glm::vec3 voxelMin(int x, int y, int z) const;

	// Number of occupied cells.
	int count() const;

private:
	int index(int x, int y, int z) const { return (z * dimY + y) * dimX + x; }
	bool inRange(int x, int y, int z) const;

	int dimX, dimY, dimZ;
	glm::vec3 lo, hi, step;
	std::vector<uint8_t> occupancy;
	std::vector<Colour> colours;
};

// Sample a triangle mesh into a voxel grid. The grid is sized `resolution`
// cells along the longest axis of the mesh bounding box (other axes scaled to
// keep cells roughly cubic), then every cell whose AABB overlaps a triangle is
// marked solid and coloured from that triangle.
VoxelGrid voxelize(const std::vector<ModelTriangle> &mesh, int resolution);

// Build a Minecraft-style surface mesh: two triangles for every exposed face of
// a solid voxel (a face is exposed when its neighbour cell is empty). Faces are
// coloured per voxel and carry axis-aligned normals.
std::vector<ModelTriangle> voxelsToMesh(const VoxelGrid &grid);
