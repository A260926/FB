/*

Target tests for the view-dependent LOD added in DualContouringSample/view_lod.

Three groups are registered through task_support/check.h:
  projection - independent recomputation of the projected box error,
               perspective depth and near-plane box blocking;
  balance    - exact frontier coverage, real positive-area face adjacency,
               forced 2:1 refinements, empty fields and the example OBJ output;
  history    - far/near round trips, hysteresis, invalid-update preservation,
               interleaved objects and fine-path equivalence.

No main() is defined here; task_support/driver.c provides it.

*/

#include "task_support/check.h"

#include "octree.h"
#include "view_lod.h"
#include "density.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using glm::ivec3;
using glm::vec2;
using glm::vec3;

namespace
{

// ----------------------------------------------------------------------------
// Analytic test fields

const vec3 SPHERE_CENTRE(2.f, 2.f, 3.f);
const float SPHERE_RADIUS = 6.f;
const ivec3 GRID_MIN(-8, -8, -8);
const int GRID_SIZE = 16;

float sphereDensity(const vec3& p)
{
	return glm::length(p - SPHERE_CENTRE) - SPHERE_RADIUS;
}

// Fully enclosed sphere (extent +/-5 inside the +/-8 grid) so the surface is
// closed and every edge is shared by exactly two triangles.
const vec3 ENCLOSED_CENTRE(0.f, 0.f, 0.f);
const float ENCLOSED_RADIUS = 5.f;

float enclosedSphereDensity(const vec3& p)
{
	return glm::length(p - ENCLOSED_CENTRE) - ENCLOSED_RADIUS;
}

// Analytic sloped plane: dot(p, n) = offset
const vec3 PLANE_NORMAL = glm::normalize(vec3(0.3f, 1.f, 0.2f));
const float PLANE_OFFSET = 1.5f;

float planeDensity(const vec3& p)
{
	return glm::dot(p, PLANE_NORMAL) - PLANE_OFFSET;
}

// A field that only bends locally.
float curvedDensity(const vec3& p)
{
	return p.y - (2.f + 1.5f * std::sin(0.7f * p.x) * std::cos(0.5f * p.z));
}

float emptyDensity(const vec3&)
{
	return 1.f;	// always air
}

// ----------------------------------------------------------------------------

LodCamera makeCamera(const vec3& eye, const vec3& target,
                     float focalPixels = 500.f, float nearDistance = 0.5f)
{
	LodCamera camera;
	camera.eye = eye;
	camera.forward = glm::normalize(target - eye);
	const vec3 worldUp(0.f, 1.f, 0.f);
	camera.right = glm::normalize(glm::cross(camera.forward, worldUp));
	camera.up = glm::cross(camera.right, camera.forward);
	camera.focalPixels = focalPixels;
	camera.nearDistance = nearDistance;
	return camera;
}

// Independent implementation of the projection from the task description.
vec2 projectPoint(const vec3& p, const LodCamera& camera)
{
	const vec3 rel = p - camera.eye;
	const float depth = glm::dot(rel, camera.forward);
	return vec2(
		camera.focalPixels * glm::dot(rel, camera.right) / depth,
		camera.focalPixels * glm::dot(rel, camera.up) / depth);
}

float minCameraDepth(const vec3& boxMin, const vec3& boxMax, const LodCamera& camera)
{
	const vec3 centre = (boxMin + boxMax) * 0.5f;
	const vec3 extents = (boxMax - boxMin) * 0.5f;
	return glm::dot(centre - camera.eye, camera.forward)
	     - glm::dot(extents, glm::abs(camera.forward));
}

float maxCameraDepth(const vec3& boxMin, const vec3& boxMax, const LodCamera& camera)
{
	const vec3 centre = (boxMin + boxMax) * 0.5f;
	const vec3 extents = (boxMax - boxMin) * 0.5f;
	return glm::dot(centre - camera.eye, camera.forward)
	     + glm::dot(extents, glm::abs(camera.forward));
}

// Recomputes E from scratch: maximum 2D distance between the projection of the
// candidate vertex q and the projections of the eight AABB corners.
float independentError(const vec3& boxMin, const vec3& boxMax,
                       const vec3& q, const LodCamera& camera)
{
	const vec2 qp = projectPoint(q, camera);
	float error = 0.f;
	for (int ix = 0; ix < 2; ix++)
	{
		for (int iy = 0; iy < 2; iy++)
		{
			for (int iz = 0; iz < 2; iz++)
			{
				const vec3 corner(
					ix == 0 ? boxMin.x : boxMax.x,
					iy == 0 ? boxMin.y : boxMax.y,
					iz == 0 ? boxMin.z : boxMax.z);
				error = glm::max(error, glm::length(projectPoint(corner, camera) - qp));
			}
		}
	}

	return error;
}

// ----------------------------------------------------------------------------
// Frontier geometry helpers

bool cellContains(const LodFrontierRecord& cell, const ivec3& unitMin)
{
	for (int axis = 0; axis < 3; axis++)
	{
		if (unitMin[axis] < cell.min[axis] ||
		    unitMin[axis] >= cell.min[axis] + cell.size)
		{
			return false;
		}
	}

	return true;
}

// Positive-area face adjacency only; edge/corner contacts report no axis.
bool shareFace(const LodFrontierRecord& a, const LodFrontierRecord& b, int& axisOut)
{
	for (int axis = 0; axis < 3; axis++)
	{
		const bool touch =
			(a.min[axis] + a.size == b.min[axis]) ||
			(b.min[axis] + b.size == a.min[axis]);
		if (!touch)
		{
			continue;
		}

		bool positiveArea = true;
		for (int k = 0; k < 3; k++)
		{
			if (k == axis)
			{
				continue;
			}

			const int lo = glm::max(a.min[k], b.min[k]);
			const int hi = glm::min(a.min[k] + a.size, b.min[k] + b.size);
			if (hi <= lo)
			{
				positiveArea = false;
				break;
			}
		}

		if (positiveArea)
		{
			axisOut = axis;
			return true;
		}
	}

	return false;
}

struct ObjData
{
	size_t vertices = 0;
	size_t normals = 0;
	size_t faces = 0;
};

bool readObj(const std::string& path, ObjData& out)
{
	std::ifstream in(path);
	if (!in)
	{
		return false;
	}

	std::string line;
	while (std::getline(in, line))
	{
		if (line.rfind("v ", 0) == 0) ++out.vertices;
		else if (line.rfind("vn ", 0) == 0) ++out.normals;
		else if (line.rfind("f ", 0) == 0) ++out.faces;
	}

	return true;
}

bool sameRecords(const std::vector<LodFrontierRecord>& a,
                 const std::vector<LodFrontierRecord>& b)
{
	if (a.size() != b.size())
	{
		return false;
	}

	for (size_t i = 0; i < a.size(); i++)
	{
		if (a[i].min != b[i].min || a[i].size != b[i].size)
		{
			return false;
		}

		for (int k = 0; k < 3; k++)
		{
			if (!std::isfinite(a[i].position[k]) ||
			    !std::isfinite(a[i].normal[k]) ||
			    a[i].position[k] != b[i].position[k] ||
			    a[i].normal[k] != b[i].normal[k])
			{
				return false;
			}
		}
	}

	return true;
}

bool isSorted(const std::vector<LodFrontierRecord>& records)
{
	for (size_t i = 1; i < records.size(); i++)
	{
		const LodFrontierRecord& a = records[i - 1];
		const LodFrontierRecord& b = records[i];
		const int keyA[4] = { a.min.x, a.min.y, a.min.z, a.size };
		const int keyB[4] = { b.min.x, b.min.y, b.min.z, b.size };
		for (int k = 0; k < 4; k++)
		{
			if (keyA[k] != keyB[k])
			{
				if (keyA[k] > keyB[k]) return false;
				break;
			}
		}
	}

	return true;
}

} // namespace

// ============================================================================
// projection group
// ============================================================================

// Independently recompute the projected box error for every collapsed cell and
// check it obeys the collapse threshold, perspective depth stays positive and
// no collapsed cell crosses the near plane.
static int projection_box_error()
{
	ViewLod lod(GRID_MIN, GRID_SIZE, planeDensity);
	const std::vector<LodFrontierRecord> fineLeaves = lod.frontier();
	TASK_CHECK(!fineLeaves.empty());

	const float collapsePixels = 100.f;
	const float splitPixels = 300.f;
	const LodCamera camera = makeCamera(vec3(60.f, 40.f, 80.f), vec3(0.f));
	lod.update(camera, collapsePixels, splitPixels);

	const std::vector<LodFrontierRecord> frontier = lod.frontier();
	TASK_CHECK(frontier.size() < fineLeaves.size());
	TASK_CHECK(lod.lastStats().errorTests > 0);

	size_t collapsedChecks = 0;
	for (const LodFrontierRecord& cell : frontier)
	{
		if (cell.size == 1)
		{
			continue;
		}

		// rebuild this cell's AABB from its finest descendant leaf vertices
		vec3 boxMin(std::numeric_limits<float>::infinity());
		vec3 boxMax(-std::numeric_limits<float>::infinity());
		size_t covered = 0;
		for (const LodFrontierRecord& leaf : fineLeaves)
		{
			if (!cellContains(cell, leaf.min))
			{
				continue;
			}

			++covered;
			boxMin = glm::min(boxMin, leaf.position);
			boxMax = glm::max(boxMax, leaf.position);
		}

		TASK_CHECK(covered > 0);

		const float error = independentError(boxMin, boxMax, cell.position, camera);
		TASK_CHECK(error <= collapsePixels + 1e-3f);

		// a collapsed cell must lie wholly beyond the near plane
		TASK_CHECK(minCameraDepth(boxMin, boxMax, camera) > camera.nearDistance);

		// every descendant vertex must project in front of the camera
		for (const LodFrontierRecord& leaf : fineLeaves)
		{
			if (cellContains(cell, leaf.min))
			{
				const float depth = glm::dot(leaf.position - camera.eye, camera.forward);
				TASK_CHECK(depth > 0.f);
			}
		}

		++collapsedChecks;
	}

	TASK_CHECK(collapsedChecks > 0);
	return 0;
}

// Perspective scaling: moving the camera farther along the same ray shrinks the
// projected error, and a near plane that reaches the geometry blocks collapse.
static int projection_perspective_and_near()
{
	ViewLod lod(GRID_MIN, GRID_SIZE, sphereDensity);
	const std::vector<LodFrontierRecord> fineLeaves = lod.frontier();

	vec3 boxMin(std::numeric_limits<float>::infinity());
	vec3 boxMax(-std::numeric_limits<float>::infinity());
	for (const LodFrontierRecord& leaf : fineLeaves)
	{
		boxMin = glm::min(boxMin, leaf.position);
		boxMax = glm::max(boxMax, leaf.position);
	}

	const vec3 q = fineLeaves[fineLeaves.size() / 2].position;

	const LodCamera nearCam = makeCamera(SPHERE_CENTRE + vec3(20.f, 8.f, 12.f), SPHERE_CENTRE);
	const LodCamera farCam = makeCamera(SPHERE_CENTRE + vec3(80.f, 32.f, 48.f), SPHERE_CENTRE);

	const float nearError = independentError(boxMin, boxMax, q, nearCam);
	const float farError = independentError(boxMin, boxMax, q, farCam);
	TASK_CHECK(std::isfinite(nearError) && std::isfinite(farError));
	TASK_CHECK(farError < nearError);

	// near plane that reaches the geometry: any cell whose AABB intersects the
	// near volume is forbidden from collapsing, even with huge thresholds.
	// Anchor it to the depth of the nearest real vertex so at least one cell is
	// guaranteed to touch while deeper cells may stay collapsed.
	LodCamera blockingCam = nearCam;
	float nearestVertexDepth = std::numeric_limits<float>::infinity();
	for (const LodFrontierRecord& leaf : fineLeaves)
	{
		nearestVertexDepth = glm::min(
			nearestVertexDepth,
			glm::dot(leaf.position - blockingCam.eye, blockingCam.forward));
	}

	blockingCam.nearDistance = nearestVertexDepth + 0.5f;
	TASK_CHECK(minCameraDepth(boxMin, boxMax, blockingCam) <= blockingCam.nearDistance);

	ViewLod blocked(GRID_MIN, GRID_SIZE, sphereDensity);
	blocked.update(blockingCam, 1e6f, 1e7f);
	const std::vector<LodFrontierRecord> blockedFrontier = blocked.frontier();
	TASK_CHECK(!blockedFrontier.empty());

	size_t fineCellsNearPlane = 0;
	for (const LodFrontierRecord& cell : blockedFrontier)
	{
		vec3 cellBoxMin(std::numeric_limits<float>::infinity());
		vec3 cellBoxMax(-std::numeric_limits<float>::infinity());
		for (const LodFrontierRecord& leaf : fineLeaves)
		{
			if (!cellContains(cell, leaf.min))
			{
				continue;
			}

			cellBoxMin = glm::min(cellBoxMin, leaf.position);
			cellBoxMax = glm::max(cellBoxMax, leaf.position);
		}

		// every retained cell stays beyond the near plane; any cell touching it
		// must have been kept at unit resolution
		const bool touches = minCameraDepth(cellBoxMin, cellBoxMax, blockingCam)
		                   <= blockingCam.nearDistance;
		if (touches)
		{
			TASK_CHECK(cell.size == 1);
			++fineCellsNearPlane;
		}
	}

	TASK_CHECK(fineCellsNearPlane > 0);

	return 0;
}

// ============================================================================
// balance group
// ============================================================================

// Every non-empty fine leaf is covered by exactly one disjoint frontier cell
// and the records are returned in the required sort order.
static int balance_frontier_coverage()
{
	ViewLod lod(GRID_MIN, GRID_SIZE, sphereDensity);
	const std::vector<LodFrontierRecord> fineLeaves = lod.frontier();

	lod.update(makeCamera(SPHERE_CENTRE + vec3(35.f, 12.f, 18.f), SPHERE_CENTRE), 25.f, 70.f);
	const std::vector<LodFrontierRecord> frontier = lod.frontier();

	TASK_CHECK(isSorted(frontier));
	TASK_CHECK(!frontier.empty());
	TASK_CHECK(frontier.size() <= fineLeaves.size());

	for (const LodFrontierRecord& leaf : fineLeaves)
	{
		size_t owners = 0;
		for (const LodFrontierRecord& cell : frontier)
		{
			if (cellContains(cell, leaf.min))
			{
				++owners;
			}
		}

		TASK_CHECK(owners == 1);
	}

	// frontier cells are interior-disjoint
	for (size_t a = 0; a < frontier.size(); a++)
	{
		for (size_t b = a + 1; b < frontier.size(); b++)
		{
			bool overlap = true;
			for (int axis = 0; axis < 3; axis++)
			{
				const int lo = glm::max(frontier[a].min[axis], frontier[b].min[axis]);
				const int hi = glm::min(frontier[a].min[axis] + frontier[a].size,
				                        frontier[b].min[axis] + frontier[b].size);
				if (lo >= hi) overlap = false;
			}

			TASK_CHECK(!overlap);
		}
	}

	// one mesh vertex per frontier cell, no fabricated vertices
	VertexBuffer vertices;
	IndexBuffer indices;
	lod.mesh(vertices, indices);
	TASK_CHECK(vertices.size() == frontier.size());
	TASK_CHECK(indices.size() % 3 == 0);
	TASK_CHECK(!indices.empty());

	return 0;
}

// Brute-force the real face adjacency: 2:1 neighbours are allowed, neighbours
// differing by 4x or more must have been refined away, and edge/corner-only
// contact is not adjacency.  The mixed sphere view must force a refinement.
static int balance_face_adjacency()
{
	ViewLod lod(GRID_MIN, GRID_SIZE, sphereDensity);
	lod.update(makeCamera(SPHERE_CENTRE + vec3(35.f, 12.f, 18.f), SPHERE_CENTRE), 25.f, 70.f);
	const std::vector<LodFrontierRecord> frontier = lod.frontier();

	size_t facePairs = 0;
	size_t ratioOnePairs = 0;
	size_t ratioTwoPairs = 0;
	size_t ratioFourOrMore = 0;

	for (size_t a = 0; a < frontier.size(); a++)
	{
		for (size_t b = a + 1; b < frontier.size(); b++)
		{
			int axis = -1;
			if (!shareFace(frontier[a], frontier[b], axis))
			{
				continue;
			}

			++facePairs;
			const int big = glm::max(frontier[a].size, frontier[b].size);
			const int small = glm::min(frontier[a].size, frontier[b].size);
			const int ratio = big / small;
			if (ratio == 1) ++ratioOnePairs;
			else if (ratio == 2) ++ratioTwoPairs;
			else ++ratioFourOrMore;
		}
	}

	TASK_CHECK(facePairs > 0);
	TASK_CHECK(ratioTwoPairs > 0);				// genuine mixed-resolution boundary
	TASK_CHECK(ratioFourOrMore == 0);			// 2:1 closure
	TASK_CHECK(ratioOnePairs + ratioTwoPairs == facePairs);

	// the recorded proportion of 2:1 pairs is a real ratio in (0, 1)
	const double mixedRatio = double(ratioTwoPairs) / double(facePairs);
	TASK_CHECK(mixedRatio > 0.0 && mixedRatio < 1.0);

	// cells sharing only an edge must not count as face neighbours
	const LodFrontierRecord edgeA(ivec3(0, 0, 0), 1, vec3(0.f), vec3(0.f, 1.f, 0.f));
	const LodFrontierRecord edgeB(ivec3(1, 1, 0), 1, vec3(0.f), vec3(0.f, 1.f, 0.f));
	int ignoredAxis = -1;
	TASK_CHECK(!shareFace(edgeA, edgeB, ignoredAxis));

	// at least one forced refinement happened in this view
	TASK_CHECK(lod.lastStats().balanceSplits >= 1);

	return 0;
}

// The headless example products must exist, contain geometry and show a real
// face-count reduction from the near view to the far view.
static int balance_example_obj_products()
{
	ObjData nearObj;
	ObjData farObj;
	TASK_CHECK(readObj(".task-build/lod-near.obj", nearObj));
	TASK_CHECK(readObj(".task-build/lod-far.obj", farObj));

	TASK_CHECK(nearObj.vertices > 0 && nearObj.normals > 0 && nearObj.faces > 0);
	TASK_CHECK(farObj.vertices > 0 && farObj.normals > 0 && farObj.faces > 0);
	TASK_CHECK(farObj.faces < nearObj.faces);
	TASK_CHECK(farObj.vertices < nearObj.vertices);

	return 0;
}

// An all-air field builds an empty tree: updates and meshing stay well behaved.
static int balance_empty_field()
{
	ViewLod empty(GRID_MIN, GRID_SIZE, emptyDensity);
	TASK_CHECK(empty.frontier().empty());
	TASK_CHECK(empty.lastStats().densityCalls > 0);

	empty.update(makeCamera(vec3(30.f), vec3(0.f)), 10.f, 30.f);
	TASK_CHECK(empty.frontier().empty());
	TASK_CHECK(empty.lastStats().errorTests == 0);
	TASK_CHECK(empty.lastStats().balanceSplits == 0);

	VertexBuffer vertices;
	IndexBuffer indices;
	empty.mesh(vertices, indices);
	TASK_CHECK(vertices.empty() && indices.empty());

	return 0;
}

// ============================================================================
// history group
// ============================================================================

// Far/near round trip restores the construction geometry, which matches the
// original unsimplified BuildOctree path exactly; the sphere mesh is finite,
// consistently oriented and its edges are manifold.
static int history_round_trip_matches_fine_path()
{
	ViewLod lod(GRID_MIN, GRID_SIZE, enclosedSphereDensity);

	VertexBuffer fineBefore;
	IndexBuffer fineIndicesBefore;
	lod.mesh(fineBefore, fineIndicesBefore);
	TASK_CHECK(!fineBefore.empty() && !fineIndicesBefore.empty());

	const std::vector<LodFrontierRecord> fineFrontier = lod.frontier();
	for (const MeshVertex& vertex : fineBefore)
	{
		for (int k = 0; k < 3; k++)
		{
			TASK_CHECK(std::isfinite(vertex.xyz[k]));
			TASK_CHECK(std::isfinite(vertex.normal[k]));
		}

		TASK_CHECK(std::abs(glm::length(vertex.normal) - 1.f) < 1e-4f);
	}

	// collapse, then return to a near camera whose near plane reaches the
	// whole surface, so every cell is forced back to unit resolution
	vec3 surfaceBoxMin(std::numeric_limits<float>::infinity());
	vec3 surfaceBoxMax(-std::numeric_limits<float>::infinity());
	for (const LodFrontierRecord& leaf : fineFrontier)
	{
		surfaceBoxMin = glm::min(surfaceBoxMin, leaf.position);
		surfaceBoxMax = glm::max(surfaceBoxMax, leaf.position);
	}

	LodCamera nearCam = makeCamera(ENCLOSED_CENTRE + vec3(10.f, 3.f, 4.f), ENCLOSED_CENTRE);
	nearCam.nearDistance = maxCameraDepth(surfaceBoxMin, surfaceBoxMax, nearCam) + 1.f;
	const LodCamera farCam = makeCamera(ENCLOSED_CENTRE + vec3(140.f, 60.f, 90.f), ENCLOSED_CENTRE);
	lod.update(farCam, 25.f, 70.f);
	TASK_CHECK(lod.frontier().size() < fineFrontier.size());

	lod.update(nearCam, 25.f, 70.f);
	const std::vector<LodFrontierRecord> roundTripFrontier = lod.frontier();
	TASK_CHECK(sameRecords(roundTripFrontier, fineFrontier));

	VertexBuffer fineAfter;
	IndexBuffer fineIndicesAfter;
	lod.mesh(fineAfter, fineIndicesAfter);
	TASK_CHECK(fineAfter.size() == fineBefore.size());
	TASK_CHECK(fineIndicesAfter.size() == fineIndicesBefore.size());
	for (size_t i = 0; i < fineBefore.size(); i++)
	{
		for (int k = 0; k < 3; k++)
		{
			TASK_CHECK(fineAfter[i].xyz[k] == fineBefore[i].xyz[k]);
			TASK_CHECK(fineAfter[i].normal[k] == fineBefore[i].normal[k]);
		}
	}
	for (size_t i = 0; i < fineIndicesBefore.size(); i++)
	{
		TASK_CHECK(fineIndicesAfter[i] == fineIndicesBefore[i]);
	}

	// mesh() is idempotent and does not move the frontier
	VertexBuffer again;
	IndexBuffer againIndices;
	lod.mesh(again, againIndices);
	TASK_CHECK(again.size() == fineAfter.size());
	TASK_CHECK(againIndices.size() == fineIndicesAfter.size());
	TASK_CHECK(sameRecords(lod.frontier(), roundTripFrontier));

	// the fine ViewLod path is the same geometry as BuildOctree with the
	// non-simplifying threshold when fed the same density function
	OctreeNode* reference = BuildOctree(ivec3(-16, 0, -16), 32, -1.0f);
	TASK_CHECK(reference != nullptr);
	VertexBuffer referenceVertices;
	IndexBuffer referenceIndices;
	GenerateMeshFromOctree(reference, referenceVertices, referenceIndices);

	ViewLod referenceLod(ivec3(-16, 0, -16), 32,
	                     std::function<float(const vec3&)>(Density_Func));
	VertexBuffer lodVertices;
	IndexBuffer lodIndices;
	referenceLod.mesh(lodVertices, lodIndices);
	TASK_CHECK(referenceVertices.size() == lodVertices.size());
	TASK_CHECK(referenceIndices.size() == lodIndices.size());
	for (size_t i = 0; i < referenceVertices.size(); i++)
	{
		for (int k = 0; k < 3; k++)
		{
			TASK_CHECK(referenceVertices[i].xyz[k] == lodVertices[i].xyz[k]);
			TASK_CHECK(referenceVertices[i].normal[k] == lodVertices[i].normal[k]);
		}
	}
	for (size_t i = 0; i < referenceIndices.size(); i++)
	{
		TASK_CHECK(referenceIndices[i] == lodIndices[i]);
	}
	DestroyOctree(reference);

	// manifold edges: every edge of the closed sphere belongs to two triangles
	std::map<std::pair<int, int>, int> edgeUse;
	for (size_t i = 0; i + 2 < fineIndicesAfter.size(); i += 3)
	{
		const int tri[3] = { fineIndicesAfter[i], fineIndicesAfter[i + 1], fineIndicesAfter[i + 2] };
		for (int e = 0; e < 3; e++)
		{
			const int a = tri[e];
			const int b = tri[(e + 1) % 3];
			TASK_CHECK(a >= 0 && b >= 0);
			TASK_CHECK(size_t(a) < fineAfter.size() && size_t(b) < fineAfter.size());
			++edgeUse[std::make_pair(glm::min(a, b), glm::max(a, b))];
		}
	}

	TASK_CHECK(!edgeUse.empty());
	for (const auto& entry : edgeUse)
	{
		TASK_CHECK(entry.second == 2);
	}

	// outward orientation relative to the sphere centre
	size_t outward = 0;
	for (size_t i = 0; i + 2 < fineIndicesAfter.size(); i += 3)
	{
		const vec3& a = fineAfter[fineIndicesAfter[i]].xyz;
		const vec3& b = fineAfter[fineIndicesAfter[i + 1]].xyz;
		const vec3& c = fineAfter[fineIndicesAfter[i + 2]].xyz;
		const vec3 normal = glm::cross(b - a, c - a);
		const vec3 centre = (a + b + c) / 3.f;
		if (glm::dot(normal, centre - ENCLOSED_CENTRE) > 0.f)
		{
			++outward;
		}
	}

	TASK_CHECK(outward == fineIndicesAfter.size() / 3);

	return 0;
}

// A collapsed root stays collapsed while its error sits in the hysteresis band
// and only expands once the split threshold is crossed.
static int history_hysteresis_band()
{
	ViewLod lod(GRID_MIN, GRID_SIZE, curvedDensity);
	const std::vector<LodFrontierRecord> fineLeaves = lod.frontier();

	vec3 boxMin(std::numeric_limits<float>::infinity());
	vec3 boxMax(-std::numeric_limits<float>::infinity());
	for (const LodFrontierRecord& leaf : fineLeaves)
	{
		boxMin = glm::min(boxMin, leaf.position);
		boxMax = glm::max(boxMax, leaf.position);
	}

	const float collapsePixels = 25.f;
	const float splitPixels = 70.f;
	const vec3 target(0.f, 2.f, 0.f);
	const vec3 rayDir = glm::normalize(vec3(140.f, 120.f, 160.f));

	// far view collapses the field down to its root cell
	const LodCamera farCam = makeCamera(target + rayDir * 240.f, target);
	lod.update(farCam, collapsePixels, splitPixels);
	TASK_CHECK(lod.frontier().size() == 1);
	TASK_CHECK(lod.frontier()[0].size == GRID_SIZE);
	const vec3 rootQ = lod.frontier()[0].position;

	bool foundBand = false;
	bool foundSplit = false;
	for (float distance = 230.f; distance >= 20.f; distance -= 5.f)
	{
		const LodCamera camera = makeCamera(target + rayDir * distance, target);
		const float error = independentError(boxMin, boxMax, rootQ, camera);

		if (!foundBand && error > collapsePixels && error <= splitPixels)
		{
			// inside the band: hysteresis keeps the root collapsed
			lod.update(camera, collapsePixels, splitPixels);
			TASK_CHECK(lod.frontier().size() == 1);
			TASK_CHECK(lod.frontier()[0].size == GRID_SIZE);
			foundBand = true;
		}

		if (foundBand && !foundSplit && error > splitPixels)
		{
			// past the split threshold the root must expand
			lod.update(camera, collapsePixels, splitPixels);
			TASK_CHECK(lod.frontier().size() > 1);
			foundSplit = true;
			break;
		}
	}

	TASK_CHECK(foundBand);
	TASK_CHECK(foundSplit);

	return 0;
}

// Invalid arguments throw and leave the previous frontier, mesh and stats
// completely intact.
static int history_invalid_updates_preserve_state()
{
	ViewLod lod(GRID_MIN, GRID_SIZE, sphereDensity);
	lod.update(makeCamera(SPHERE_CENTRE + vec3(35.f, 12.f, 18.f), SPHERE_CENTRE), 25.f, 70.f);

	const std::vector<LodFrontierRecord> savedFrontier = lod.frontier();
	VertexBuffer savedVertices;
	IndexBuffer savedIndices;
	lod.mesh(savedVertices, savedIndices);
	const LodStats savedStats = lod.lastStats();
	TASK_CHECK(!savedFrontier.empty());

	auto expectThrow = [&](const LodCamera& camera, float collapse, float split)
	{
		bool threw = false;
		try
		{
			lod.update(camera, collapse, split);
		}
		catch (const std::invalid_argument&)
		{
			threw = true;
		}

		TASK_CHECK(threw);
		return 0;
	};

	LodCamera camera = makeCamera(SPHERE_CENTRE + vec3(35.f, 12.f, 18.f), SPHERE_CENTRE);

	LodCamera bad = camera;
	bad.eye.x = std::numeric_limits<float>::quiet_NaN();
	expectThrow(bad, 25.f, 70.f);

	bad = camera;
	bad.forward = vec3(0.f, 1.f, 0.f);
	bad.forward.y = std::numeric_limits<float>::infinity();
	expectThrow(bad, 25.f, 70.f);

	expectThrow(camera, -1.f, 70.f);		// negative collapse threshold
	expectThrow(camera, 70.f, 70.f);		// thresholds must be strictly ordered
	expectThrow(camera, 80.f, 70.f);		// split smaller than collapse
	expectThrow(camera, std::nanf(""), 70.f);

	bad = camera;
	bad.focalPixels = 0.f;
	expectThrow(bad, 25.f, 70.f);

	bad = camera;
	bad.nearDistance = -2.f;
	expectThrow(bad, 25.f, 70.f);

	// old state is untouched
	TASK_CHECK(sameRecords(lod.frontier(), savedFrontier));
	VertexBuffer currentVertices;
	IndexBuffer currentIndices;
	lod.mesh(currentVertices, currentIndices);
	TASK_CHECK(currentVertices.size() == savedVertices.size());
	TASK_CHECK(currentIndices.size() == savedIndices.size());
	TASK_CHECK(lod.lastStats().densityCalls == savedStats.densityCalls);
	TASK_CHECK(lod.lastStats().errorTests == savedStats.errorTests);
	TASK_CHECK(lod.lastStats().balanceSplits == savedStats.balanceSplits);

	return 0;
}

// Two ViewLod instances keep independent hysteresis; the density is only used
// during construction and its call count never changes afterwards.
static int history_two_objects_interleaved()
{
	size_t sphereCalls = 0;
	size_t curvedCalls = 0;
	auto countedSphere = [&](const vec3& p) { ++sphereCalls; return sphereDensity(p); };
	auto countedCurved = [&](const vec3& p) { ++curvedCalls; return curvedDensity(p); };

	ViewLod sphere(GRID_MIN, GRID_SIZE, countedSphere);
	ViewLod curved(GRID_MIN, GRID_SIZE, countedCurved);

	TASK_CHECK(sphere.lastStats().densityCalls == sphereCalls);
	TASK_CHECK(curved.lastStats().densityCalls == curvedCalls);
	TASK_CHECK(sphere.lastStats().densityCalls > 0);
	TASK_CHECK(curved.lastStats().densityCalls > 0);

	const LodCamera sphereNear = makeCamera(SPHERE_CENTRE + vec3(10.f, 3.f, 4.f), SPHERE_CENTRE);
	const LodCamera sphereFar = makeCamera(SPHERE_CENTRE + vec3(140.f, 60.f, 90.f), SPHERE_CENTRE);
	const LodCamera curveNear = makeCamera(vec3(10.f, 12.f, 14.f), vec3(0.f, 2.f, 0.f));
	const LodCamera curveFar = makeCamera(vec3(240.f, 120.f, 160.f), vec3(0.f, 2.f, 0.f));

	sphere.update(sphereFar, 25.f, 70.f);
	const size_t sphereFarCount = sphere.frontier().size();
	TASK_CHECK(sphereFarCount < curved.frontier().size());

	curved.update(curveFar, 25.f, 70.f);
	TASK_CHECK(curved.frontier().size() == 1);
	TASK_CHECK(sphere.frontier().size() == sphereFarCount);	// untouched neighbour

	sphere.update(sphereNear, 25.f, 70.f);
	const size_t sphereNearCount = sphere.frontier().size();
	TASK_CHECK(sphereNearCount > sphereFarCount);
	TASK_CHECK(curved.frontier().size() == 1);

	curved.update(curveNear, 25.f, 70.f);
	TASK_CHECK(curved.frontier().size() > 1);
	TASK_CHECK(sphere.frontier().size() == sphereNearCount);

	// no density work after construction, on either object
	TASK_CHECK(sphere.lastStats().densityCalls == sphereCalls);
	TASK_CHECK(curved.lastStats().densityCalls == curvedCalls);
	TASK_CHECK(sphere.lastStats().errorTests > 0);
	TASK_CHECK(curved.lastStats().errorTests > 0);

	return 0;
}

// ============================================================================
// Registration
// ============================================================================

const struct task_case task_cases[] =
{
	{ "projection", "box_error",              projection_box_error },
	{ "projection", "perspective_near_plane", projection_perspective_and_near },
	{ "balance",    "frontier_coverage",      balance_frontier_coverage },
	{ "balance",    "face_adjacency",         balance_face_adjacency },
	{ "balance",    "example_obj_products",   balance_example_obj_products },
	{ "balance",    "empty_field",            balance_empty_field },
	{ "history",    "round_trip_fine_path",   history_round_trip_matches_fine_path },
	{ "history",    "hysteresis_band",        history_hysteresis_band },
	{ "history",    "invalid_update_state",   history_invalid_updates_preserve_state },
	{ "history",    "two_objects_interleaved", history_two_objects_interleaved },
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
