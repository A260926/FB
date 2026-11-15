#include "Grid.h"

#include "Geometry.h"
#include <algorithm>
#include <cmath>

// -----------------------------------------------------------------------------
// Construction: bound the scene, pick a resolution, and bin triangles.
// -----------------------------------------------------------------------------

Grid::Grid(const std::vector<ModelTriangle> &triangles) : tris(&triangles) {
	int n = static_cast<int>(triangles.size());
	if (n == 0) {
		// Empty scene: a 1x1x1 grid with no triangles. All queries miss.
		bmin = glm::vec3(0.0f);
		bmax = glm::vec3(0.0f);
		res[0] = res[1] = res[2] = 1;
		cellSize = glm::vec3(1.0f);
		invCellSize = glm::vec3(1.0f);
		cellStart.assign(2, 0);
		return;
	}

	// Scene AABB over every vertex.
	glm::vec3 lo(1e30f), hi(-1e30f);
	for (int i = 0; i < n; i++) {
		for (const glm::vec3 &v : triangles[i].vertices) {
			lo = glm::min(lo, v);
			hi = glm::max(hi, v);
		}
	}

	// Pad flat axes so every extent is strictly positive (a planar scene would
	// otherwise give a zero-thickness slab and a divide-by-zero cell size).
	glm::vec3 extent = hi - lo;
	for (int a = 0; a < 3; a++) {
		if (extent[a] < 1e-6f) {
			float pad = 1e-4f;
			lo[a] -= pad;
			hi[a] += pad;
			extent[a] = hi[a] - lo[a];
		}
	}
	bmin = lo;
	bmax = hi;

	// Resolution ~ cbrt(N) per axis, distributed by relative extent so that
	// voxels stay roughly cubic on non-cubic scenes. Clamped to a sane range.
	double cubeRoot = std::cbrt(static_cast<double>(n));
	float maxExtent = std::max({extent.x, extent.y, extent.z});
	for (int a = 0; a < 3; a++) {
		int r = static_cast<int>(std::round(cubeRoot * (extent[a] / maxExtent)));
		res[a] = std::clamp(r, 1, 128);
	}

	cellSize = extent / glm::vec3(static_cast<float>(res[0]), static_cast<float>(res[1]), static_cast<float>(res[2]));
	invCellSize = 1.0f / cellSize;

	int numCells = res[0] * res[1] * res[2];

	// Map a world position to clamped integer voxel coordinates on one axis.
	auto coordOnAxis = [&](float p, int a) {
		int c = static_cast<int>(std::floor((p - bmin[a]) * invCellSize[a]));
		return std::clamp(c, 0, res[a] - 1);
	};

	// Pass 1: count, per voxel, how many triangle AABBs overlap it.
	std::vector<int> counts(numCells, 0);
	std::vector<int> lo3(3 * n), hi3(3 * n); // cache each triangle's voxel range
	for (int i = 0; i < n; i++) {
		glm::vec3 tlo(1e30f), thi(-1e30f);
		for (const glm::vec3 &v : triangles[i].vertices) {
			tlo = glm::min(tlo, v);
			thi = glm::max(thi, v);
		}
		for (int a = 0; a < 3; a++) {
			int x0 = coordOnAxis(tlo[a], a);
			int x1 = coordOnAxis(thi[a], a);
			lo3[3 * i + a] = x0;
			hi3[3 * i + a] = x1;
		}
		for (int z = lo3[3 * i + 2]; z <= hi3[3 * i + 2]; z++)
			for (int y = lo3[3 * i + 1]; y <= hi3[3 * i + 1]; y++)
				for (int x = lo3[3 * i + 0]; x <= hi3[3 * i + 0]; x++)
					counts[cellIndex(x, y, z)]++;
	}

	// Prefix sum -> CSR offsets (cellStart has numCells + 1 entries).
	cellStart.assign(numCells + 1, 0);
	for (int c = 0; c < numCells; c++)
		cellStart[c + 1] = cellStart[c] + counts[c];
	cellTris.assign(cellStart[numCells], 0);

	// Pass 2: scatter triangle indices into their voxels.
	std::vector<int> cursor(cellStart.begin(), cellStart.begin() + numCells);
	for (int i = 0; i < n; i++) {
		for (int z = lo3[3 * i + 2]; z <= hi3[3 * i + 2]; z++)
			for (int y = lo3[3 * i + 1]; y <= hi3[3 * i + 1]; y++)
				for (int x = lo3[3 * i + 0]; x <= hi3[3 * i + 0]; x++)
					cellTris[cursor[cellIndex(x, y, z)]++] = i;
	}
}

// -----------------------------------------------------------------------------
// Shared helpers.
// -----------------------------------------------------------------------------

// Ray vs the grid's AABB (slab test). Returns true on overlap and reports the
// entry/exit parameters. On a hit tEnter/tExit bracket the segment inside the box
// (tEnter may be negative when the origin is already inside).
static bool boxRange(const glm::vec3 &origin, const glm::vec3 &invDir, const glm::vec3 &bmin, const glm::vec3 &bmax,
                     float &tEnter, float &tExit) {
	glm::vec3 t0 = (bmin - origin) * invDir;
	glm::vec3 t1 = (bmax - origin) * invDir;
	glm::vec3 tsmall = glm::min(t0, t1);
	glm::vec3 tbig = glm::max(t0, t1);
	tEnter = std::max({tsmall.x, tsmall.y, tsmall.z});
	tExit = std::min({tbig.x, tbig.y, tbig.z});
	return tExit >= std::max(tEnter, 0.0f);
}

// Per-ray mailbox. Because a triangle can be binned into several voxels, a ray
// may reach it more than once while marching; the mailbox lets us run the
// (relatively expensive) triangle test only the first time. Both the stamp
// buffer and the ray token live in thread_local storage so traversal stays
// reentrant across worker threads, and the token is monotonic so stamps left by
// a previous ray (or a different Grid) never falsely register as visited.
//
// Usage: bump the token once per ray, then before testing triangle `ti` call
// `if (mailboxStamp(ti, n) == token) continue; else record token`.
static int &mailboxToken() {
	static thread_local int token = 0;
	return token;
}
static std::vector<int> &mailboxStamps(int triCount) {
	static thread_local std::vector<int> stamp;
	if (static_cast<int>(stamp.size()) < triCount)
		stamp.resize(triCount, 0);
	return stamp;
}

// -----------------------------------------------------------------------------
// Closest-hit traversal (3D-DDA, Amanatides and Woo).
// -----------------------------------------------------------------------------

RayTriangleIntersection Grid::intersect(const glm::vec3 &origin, const glm::vec3 &direction, int ignoreIndex) const {
	RayTriangleIntersection closest;
	int n = tris ? static_cast<int>(tris->size()) : 0;
	if (n == 0)
		return closest;

	glm::vec3 invDir = 1.0f / direction;
	float tEnter, tExit;
	if (!boxRange(origin, invDir, bmin, bmax, tEnter, tExit))
		return closest; // ray misses the whole grid

	float tStart = std::max(tEnter, 0.0f);
	glm::vec3 entry = origin + tStart * direction;

	// Integer voxel of the entry point (clamped: floating error can nudge us a
	// hair outside the box).
	int cell[3];
	for (int a = 0; a < 3; a++) {
		int c = static_cast<int>(std::floor((entry[a] - bmin[a]) * invCellSize[a]));
		cell[a] = std::clamp(c, 0, res[a] - 1);
	}

	// DDA stepping state per axis.
	int step[3];
	int outOfBounds[3]; // coordinate that means we have left the grid
	float tMax[3];      // ray t at which we cross into the next voxel
	float tDelta[3];    // ray t to traverse one full voxel
	for (int a = 0; a < 3; a++) {
		if (direction[a] > 0.0f) {
			step[a] = 1;
			outOfBounds[a] = res[a];
			float nextBoundary = bmin[a] + static_cast<float>(cell[a] + 1) * cellSize[a];
			tMax[a] = (nextBoundary - origin[a]) * invDir[a];
			tDelta[a] = cellSize[a] * std::fabs(invDir[a]);
		} else if (direction[a] < 0.0f) {
			step[a] = -1;
			outOfBounds[a] = -1;
			float nextBoundary = bmin[a] + static_cast<float>(cell[a]) * cellSize[a];
			tMax[a] = (nextBoundary - origin[a]) * invDir[a];
			tDelta[a] = cellSize[a] * std::fabs(invDir[a]);
		} else {
			step[a] = 0;
			outOfBounds[a] = -2; // never reached
			tMax[a] = 1e30f;
			tDelta[a] = 1e30f;
		}
	}

	int token = ++mailboxToken();
	std::vector<int> &stamp = mailboxStamps(n);

	while (true) {
		int c = cellIndex(cell[0], cell[1], cell[2]);
		for (int k = cellStart[c]; k < cellStart[c + 1]; k++) {
			int ti = cellTris[k];
			if (ti == ignoreIndex)
				continue;
			// Mailbox dedupe: skip triangles already tested by this ray.
			if (stamp[ti] == token)
				continue;
			stamp[ti] = token;
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

		// Pick the axis we exit the current voxel through first.
		int axis = 0;
		if (tMax[1] < tMax[axis])
			axis = 1;
		if (tMax[2] < tMax[axis])
			axis = 2;
		float tCellExit = tMax[axis];

		// If the closest confirmed hit lies within (or on) this voxel, no later
		// voxel can beat it, so we are done. A triangle straddling voxels may be
		// recorded early with a distance beyond this exit; then this test fails
		// and we keep marching, which is exactly what correctness requires.
		if (closest.hit && closest.distanceFromCamera <= tCellExit)
			break;

		if (step[axis] == 0)
			break; // degenerate direction, cannot advance
		cell[axis] += step[axis];
		if (cell[axis] == outOfBounds[axis])
			break; // stepped out of the grid
		tMax[axis] += tDelta[axis];
	}

	return closest;
}

// -----------------------------------------------------------------------------
// Occlusion (shadow) traversal: stop at the first hit before maxDistance.
// -----------------------------------------------------------------------------

bool Grid::occluded(const glm::vec3 &origin, const glm::vec3 &direction, float maxDistance, int ignoreIndex) const {
	int n = tris ? static_cast<int>(tris->size()) : 0;
	if (n == 0)
		return false;

	glm::vec3 invDir = 1.0f / direction;
	float tEnter, tExit;
	if (!boxRange(origin, invDir, bmin, bmax, tEnter, tExit))
		return false;

	float tStart = std::max(tEnter, 0.0f);
	if (tStart >= maxDistance)
		return false; // the whole grid is beyond the shadow segment
	glm::vec3 entry = origin + tStart * direction;

	int cell[3];
	for (int a = 0; a < 3; a++) {
		int c = static_cast<int>(std::floor((entry[a] - bmin[a]) * invCellSize[a]));
		cell[a] = std::clamp(c, 0, res[a] - 1);
	}

	int step[3];
	int outOfBounds[3];
	float tMax[3];
	float tDelta[3];
	for (int a = 0; a < 3; a++) {
		if (direction[a] > 0.0f) {
			step[a] = 1;
			outOfBounds[a] = res[a];
			float nextBoundary = bmin[a] + static_cast<float>(cell[a] + 1) * cellSize[a];
			tMax[a] = (nextBoundary - origin[a]) * invDir[a];
			tDelta[a] = cellSize[a] * std::fabs(invDir[a]);
		} else if (direction[a] < 0.0f) {
			step[a] = -1;
			outOfBounds[a] = -1;
			float nextBoundary = bmin[a] + static_cast<float>(cell[a]) * cellSize[a];
			tMax[a] = (nextBoundary - origin[a]) * invDir[a];
			tDelta[a] = cellSize[a] * std::fabs(invDir[a]);
		} else {
			step[a] = 0;
			outOfBounds[a] = -2;
			tMax[a] = 1e30f;
			tDelta[a] = 1e30f;
		}
	}

	int token = ++mailboxToken();
	std::vector<int> &stamp = mailboxStamps(n);
	float tCellEnter = tStart;

	while (true) {
		if (tCellEnter >= maxDistance)
			break; // no voxel from here on lies within the shadow segment

		int c = cellIndex(cell[0], cell[1], cell[2]);
		for (int k = cellStart[c]; k < cellStart[c + 1]; k++) {
			int ti = cellTris[k];
			if (ti == ignoreIndex)
				continue;
			if (stamp[ti] == token)
				continue;
			stamp[ti] = token;

			float t, u, v;
			if (intersectTriangle(origin, direction, (*tris)[ti], t, u, v) && t < maxDistance)
				return true;
		}

		int axis = 0;
		if (tMax[1] < tMax[axis])
			axis = 1;
		if (tMax[2] < tMax[axis])
			axis = 2;

		if (step[axis] == 0)
			break;
		tCellEnter = tMax[axis];
		cell[axis] += step[axis];
		if (cell[axis] == outOfBounds[axis])
			break;
		tMax[axis] += tDelta[axis];
	}

	return false;
}
