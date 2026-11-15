#include "Voxel.h"

#include <algorithm>
#include <cmath>
#include <limits>

// ---------------------------------------------------------------------------
// VoxelGrid
// ---------------------------------------------------------------------------

VoxelGrid::VoxelGrid(int nx, int ny, int nz, glm::vec3 minBound, glm::vec3 maxBound)
    : dimX(std::max(nx, 1)), dimY(std::max(ny, 1)), dimZ(std::max(nz, 1)), lo(minBound), hi(maxBound) {
	step = (hi - lo) / glm::vec3(dimX, dimY, dimZ);
	// Guard against a degenerate (flat) bound so cells always have positive size.
	if (step.x <= 0.0f)
		step.x = 1.0f;
	if (step.y <= 0.0f)
		step.y = 1.0f;
	if (step.z <= 0.0f)
		step.z = 1.0f;
	size_t total = static_cast<size_t>(dimX) * dimY * dimZ;
	occupancy.assign(total, 0);
	colours.assign(total, Colour(0, 0, 0));
}

bool VoxelGrid::inRange(int x, int y, int z) const {
	return x >= 0 && y >= 0 && z >= 0 && x < dimX && y < dimY && z < dimZ;
}

void VoxelGrid::set(int x, int y, int z, Colour c) {
	if (!inRange(x, y, z))
		return;
	int i = index(x, y, z);
	occupancy[i] = 1;
	colours[i] = c;
}

void VoxelGrid::clear(int x, int y, int z) {
	if (!inRange(x, y, z))
		return;
	occupancy[index(x, y, z)] = 0;
}

bool VoxelGrid::solid(int x, int y, int z) const {
	if (!inRange(x, y, z))
		return false;
	return occupancy[index(x, y, z)] != 0;
}

Colour VoxelGrid::colourAt(int x, int y, int z) const {
	if (!inRange(x, y, z))
		return Colour(0, 0, 0);
	return colours[index(x, y, z)];
}

glm::vec3 VoxelGrid::voxelMin(int x, int y, int z) const {
	return lo + step * glm::vec3(x, y, z);
}

glm::vec3 VoxelGrid::voxelCentre(int x, int y, int z) const {
	return lo + step * (glm::vec3(x, y, z) + glm::vec3(0.5f));
}

int VoxelGrid::count() const {
	int n = 0;
	for (uint8_t o : occupancy)
		if (o)
			++n;
	return n;
}

// ---------------------------------------------------------------------------
// Triangle / AABB overlap (Akenine-Moller separating-axis test)
// ---------------------------------------------------------------------------

namespace {

bool axisTestOverlap(const glm::vec3 &axis, const glm::vec3 &v0, const glm::vec3 &v1, const glm::vec3 &v2,
                     const glm::vec3 &halfSize) {
	float p0 = glm::dot(axis, v0);
	float p1 = glm::dot(axis, v1);
	float p2 = glm::dot(axis, v2);
	float mn = std::min({p0, p1, p2});
	float mx = std::max({p0, p1, p2});
	// Projection radius of the box onto the axis.
	float r = halfSize.x * std::abs(axis.x) + halfSize.y * std::abs(axis.y) + halfSize.z * std::abs(axis.z);
	return !(mn > r || mx < -r);
}

// Tests whether the triangle overlaps the box centred at boxCentre with the
// given half extents. Vertices are given in world space.
bool triBoxOverlap(const glm::vec3 &boxCentre, const glm::vec3 &halfSize, const glm::vec3 &a, const glm::vec3 &b,
                   const glm::vec3 &c) {
	// Move triangle into the box-local frame.
	glm::vec3 v0 = a - boxCentre;
	glm::vec3 v1 = b - boxCentre;
	glm::vec3 v2 = c - boxCentre;

	glm::vec3 e0 = v1 - v0;
	glm::vec3 e1 = v2 - v1;
	glm::vec3 e2 = v0 - v2;

	// 9 edge cross-axis tests (edge x {x,y,z} unit axes).
	const glm::vec3 edges[3] = {e0, e1, e2};
	for (const glm::vec3 &e : edges) {
		glm::vec3 axes[3] = {glm::vec3(0.0f, -e.z, e.y), glm::vec3(e.z, 0.0f, -e.x), glm::vec3(-e.y, e.x, 0.0f)};
		for (const glm::vec3 &axis : axes) {
			if (glm::dot(axis, axis) < 1e-20f)
				continue; // degenerate edge component
			if (!axisTestOverlap(axis, v0, v1, v2, halfSize))
				return false;
		}
	}

	// 3 face-normal tests of the box (the triangle's AABB vs the box).
	for (int i = 0; i < 3; ++i) {
		float mn = std::min({v0[i], v1[i], v2[i]});
		float mx = std::max({v0[i], v1[i], v2[i]});
		if (mn > halfSize[i] || mx < -halfSize[i])
			return false;
	}

	// Triangle-plane vs box test.
	glm::vec3 n = glm::cross(e0, e1);
	if (glm::dot(n, n) < 1e-20f)
		return true; // degenerate triangle already covered by AABB tests
	float d = glm::dot(n, v0);
	float r = halfSize.x * std::abs(n.x) + halfSize.y * std::abs(n.y) + halfSize.z * std::abs(n.z);
	return std::abs(d) <= r;
}

} // namespace

// ---------------------------------------------------------------------------
// voxelize
// ---------------------------------------------------------------------------

VoxelGrid voxelize(const std::vector<ModelTriangle> &mesh, int resolution) {
	resolution = std::max(resolution, 1);

	// Compute the mesh bounding box.
	glm::vec3 mn(std::numeric_limits<float>::max());
	glm::vec3 mx(std::numeric_limits<float>::lowest());
	for (const ModelTriangle &t : mesh) {
		for (const glm::vec3 &v : t.vertices) {
			mn = glm::min(mn, v);
			mx = glm::max(mx, v);
		}
	}
	if (mesh.empty()) {
		mn = glm::vec3(0.0f);
		mx = glm::vec3(1.0f);
	}

	// Pad slightly so surface geometry on the boundary is captured.
	glm::vec3 extent = mx - mn;
	float longest = std::max({extent.x, extent.y, extent.z, 1e-6f});
	glm::vec3 pad = glm::vec3(longest * 0.01f);
	mn -= pad;
	mx += pad;
	extent = mx - mn;

	// Cube-ish cells: size cells off the longest axis, scale the others.
	float cell = longest / static_cast<float>(resolution);
	if (cell <= 0.0f)
		cell = 1.0f;
	int nx = std::max(1, static_cast<int>(std::ceil(extent.x / cell)));
	int ny = std::max(1, static_cast<int>(std::ceil(extent.y / cell)));
	int nz = std::max(1, static_cast<int>(std::ceil(extent.z / cell)));

	// Grid upper corner grown to the cell grid so cells stay cubic.
	glm::vec3 gridMax = mn + glm::vec3(nx, ny, nz) * cell;
	VoxelGrid grid(nx, ny, nz, mn, gridMax);

	glm::vec3 half = grid.cellSize() * 0.5f;

	for (const ModelTriangle &tri : mesh) {
		const glm::vec3 &a = tri.vertices[0];
		const glm::vec3 &b = tri.vertices[1];
		const glm::vec3 &c = tri.vertices[2];

		// Restrict the search to the triangle's cell-range AABB.
		glm::vec3 tmn = glm::min(a, glm::min(b, c));
		glm::vec3 tmx = glm::max(a, glm::max(b, c));
		glm::vec3 rel0 = (tmn - mn) / grid.cellSize();
		glm::vec3 rel1 = (tmx - mn) / grid.cellSize();

		int x0 = std::max(0, static_cast<int>(std::floor(rel0.x)));
		int y0 = std::max(0, static_cast<int>(std::floor(rel0.y)));
		int z0 = std::max(0, static_cast<int>(std::floor(rel0.z)));
		int x1 = std::min(nx - 1, static_cast<int>(std::floor(rel1.x)));
		int y1 = std::min(ny - 1, static_cast<int>(std::floor(rel1.y)));
		int z1 = std::min(nz - 1, static_cast<int>(std::floor(rel1.z)));

		for (int z = z0; z <= z1; ++z) {
			for (int y = y0; y <= y1; ++y) {
				for (int x = x0; x <= x1; ++x) {
					glm::vec3 centre = grid.voxelCentre(x, y, z);
					if (triBoxOverlap(centre, half, a, b, c))
						grid.set(x, y, z, tri.colour);
				}
			}
		}
	}

	return grid;
}

// ---------------------------------------------------------------------------
// voxelsToMesh
// ---------------------------------------------------------------------------

std::vector<ModelTriangle> voxelsToMesh(const VoxelGrid &grid) {
	std::vector<ModelTriangle> out;
	glm::vec3 s = grid.cellSize();

	// The six faces: neighbour offset and outward normal.
	struct Face {
		int dx, dy, dz;
		glm::vec3 normal;
	};
	const Face faces[6] = {
	    {+1, 0, 0, glm::vec3(1, 0, 0)},  {-1, 0, 0, glm::vec3(-1, 0, 0)}, {0, +1, 0, glm::vec3(0, 1, 0)},
	    {0, -1, 0, glm::vec3(0, -1, 0)}, {0, 0, +1, glm::vec3(0, 0, 1)},  {0, 0, -1, glm::vec3(0, 0, -1)},
	};

	auto emit = [&](const glm::vec3 &p0, const glm::vec3 &p1, const glm::vec3 &p2, const glm::vec3 &p3,
	                const glm::vec3 &n, const Colour &c) {
		ModelTriangle t0(p0, p1, p2, c);
		ModelTriangle t1(p0, p2, p3, c);
		t0.normal = n;
		t1.normal = n;
		t0.vertexNormals = {n, n, n};
		t1.vertexNormals = {n, n, n};
		out.push_back(t0);
		out.push_back(t1);
	};

	for (int z = 0; z < grid.nz(); ++z) {
		for (int y = 0; y < grid.ny(); ++y) {
			for (int x = 0; x < grid.nx(); ++x) {
				if (!grid.solid(x, y, z))
					continue;
				Colour c = grid.colourAt(x, y, z);
				glm::vec3 mn = grid.voxelMin(x, y, z);
				glm::vec3 mx = mn + s;

				// Eight cube corners.
				glm::vec3 c000(mn.x, mn.y, mn.z);
				glm::vec3 c100(mx.x, mn.y, mn.z);
				glm::vec3 c010(mn.x, mx.y, mn.z);
				glm::vec3 c110(mx.x, mx.y, mn.z);
				glm::vec3 c001(mn.x, mn.y, mx.z);
				glm::vec3 c101(mx.x, mn.y, mx.z);
				glm::vec3 c011(mn.x, mx.y, mx.z);
				glm::vec3 c111(mx.x, mx.y, mx.z);

				for (const Face &f : faces) {
					if (grid.solid(x + f.dx, y + f.dy, z + f.dz))
						continue; // face is internal

					// Emit the face quad with a counter-clockwise winding when
					// viewed from outside (along the outward normal).
					if (f.dx == 1)
						emit(c100, c110, c111, c101, f.normal, c);
					else if (f.dx == -1)
						emit(c000, c001, c011, c010, f.normal, c);
					else if (f.dy == 1)
						emit(c010, c011, c111, c110, f.normal, c);
					else if (f.dy == -1)
						emit(c000, c100, c101, c001, f.normal, c);
					else if (f.dz == 1)
						emit(c001, c101, c111, c011, f.normal, c);
					else // f.dz == -1
						emit(c000, c010, c110, c100, f.normal, c);
				}
			}
		}
	}

	return out;
}
