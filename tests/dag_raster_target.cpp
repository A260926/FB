/***************************************************************************************************
* Target tests for the CPU DAG rasteriser (src/library/dag_rasterizer.*) and the depth hierarchy
* (src/library/depth_hierarchy.*). Registered with the task driver via task_cases/task_case_count
* (no main() here). Groups: coverage, hierarchy, sparse.
*
* Correctness is checked against an *independent* reference: explicit solid voxel boxes
* ([q-0.5, q+0.5] per integer voxel centre) intersected with the exact pixel rays - never by
* comparing the two new code paths against each other alone.
***************************************************************************************************/
#include "task_support/check.h"

#include "dag_rasterizer.h"
#include "depth_hierarchy.h"
#include "storage.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

using namespace Cubiquity;

namespace
{
	constexpr double Inf = std::numeric_limits<double>::infinity();

	// A solid axis-aligned box with a material, used by the independent reference.
	struct RefBox { double x0, y0, z0, x1, y1, z1; int mat; };

	// Append the solid box covering the inclusive voxel range as a single box.
	void addBox(std::vector<RefBox>& boxes, int x0, int y0, int z0, int x1, int y1, int z1, int mat)
	{
		boxes.push_back({ x0 - 0.5, y0 - 0.5, z0 - 0.5, x1 + 0.5, y1 + 0.5, z1 + 0.5, mat });
	}

	void addCell(std::vector<RefBox>& boxes, int x, int y, int z, int mat)
	{
		addBox(boxes, x, y, z, x, y, z, mat);
	}

	// Append one solid box per voxel cell of the inclusive range.
	void addCells(std::vector<RefBox>& boxes, int x0, int y0, int z0, int x1, int y1, int z1, int mat)
	{
		for (int z = z0; z <= z1; ++z)
			for (int y = y0; y <= y1; ++y)
				for (int x = x0; x <= x1; ++x)
					addCell(boxes, x, y, z, mat);
	}

	void fillBox(Volume& volume, int x0, int y0, int z0, int x1, int y1, int z1, int mat)
	{
		for (int z = z0; z <= z1; ++z)
			for (int y = y0; y <= y1; ++y)
				for (int x = x0; x <= x1; ++x)
					volume.setVoxel(x, y, z, MaterialId(mat));
	}

	RasterCamera makeCam(const vec3d& eye, const vec3d& right, const vec3d& up, const vec3d& forward,
		int w, int h, double fovY, double near, double far)
	{
		RasterCamera cam;
		cam.eye = eye; cam.right = right; cam.up = up; cam.forward = forward;
		cam.width = w; cam.height = h; cam.fovY = fovY; cam.near = near; cam.far = far;
		return cam;
	}

	// Camera with an orthonormal basis built from a look direction and an up hint.
	RasterCamera makeObliqueCam(const vec3d& eye, const vec3d& lookDir, const vec3d& upHint,
		int w, int h, double fovY, double near, double far)
	{
		const vec3d forward = normalize(lookDir);
		const vec3d right = normalize(cross(upHint, forward));
		const vec3d up = cross(forward, right);
		return makeCam(eye, right, up, forward, w, h, fovY, near, far);
	}

	// Independent reference: intersect every pixel ray with every solid box and keep
	// the start of the first positive-length interval inside [near, far]
	// (ties go to the smaller material id).
	void refRender(const RasterCamera& cam, const std::vector<RefBox>& boxes,
		std::vector<double>& depth, std::vector<int>& mat)
	{
		const int w = cam.width, h = cam.height;
		depth.assign(static_cast<size_t>(w) * h, Inf);
		mat.assign(static_cast<size_t>(w) * h, 0);
		for (int y = 0; y < h; ++y)
		{
			for (int x = 0; x < w; ++x)
			{
				const vec3d d = cam.pixelRay(x, y);
				double best = Inf;
				int bestMat = 0;
				for (const RefBox& b : boxes)
				{
					double t0 = (b.x0 - cam.eye.x) / d.x, t1 = (b.x1 - cam.eye.x) / d.x;
					if (t0 > t1) std::swap(t0, t1);
					double entry = t0, exit = t1;
					t0 = (b.y0 - cam.eye.y) / d.y; t1 = (b.y1 - cam.eye.y) / d.y;
					if (t0 > t1) std::swap(t0, t1);
					entry = std::max(entry, t0); exit = std::min(exit, t1);
					t0 = (b.z0 - cam.eye.z) / d.z; t1 = (b.z1 - cam.eye.z) / d.z;
					if (t0 > t1) std::swap(t0, t1);
					entry = std::max(entry, t0); exit = std::min(exit, t1);

					const double lo = std::max(entry, cam.near);
					const double hi = std::min(exit, cam.far);
					if (lo < hi && (lo < best || (lo == best && b.mat < bestMat)))
					{
						best = lo;
						bestMat = b.mat;
					}
				}
				const size_t idx = static_cast<size_t>(y) * w + x;
				depth[idx] = best;
				mat[idx] = bestMat;
			}
		}
	}

	// Compare a renderDag result against the independent box reference: misses must
	// match, materials must match exactly, and hit depths to 1e-9 relative error.
	int checkAgainstReference(const DagRasterResult& res, const RasterCamera& cam, const std::vector<RefBox>& boxes)
	{
		std::vector<double> refDepth;
		std::vector<int> refMat;
		refRender(cam, boxes, refDepth, refMat);
		const size_t count = static_cast<size_t>(cam.width) * cam.height;
		TASK_CHECK(res.depth.size() == count);
		TASK_CHECK(res.material.size() == count);
		for (size_t i = 0; i < count; ++i)
		{
			if (std::isinf(refDepth[i]))
			{
				TASK_CHECK(std::isinf(res.depth[i]));
				TASK_CHECK(res.material[i] == 0);
			}
			else
			{
				TASK_CHECK(res.material[i] == refMat[i]);
				TASK_CHECK(std::fabs(res.depth[i] - refDepth[i]) <= 1e-9 * std::fabs(refDepth[i]));
			}
		}
		return 0;
	}

	int countMaterial(const DagRasterResult& res, int mat)
	{
		int n = 0;
		for (size_t i = 0; i < res.material.size(); ++i) {
			if (res.material[i] == mat) { ++n; }
		}
		return n;
	}

	int countMisses(const DagRasterResult& res)
	{
		int n = 0;
		for (size_t i = 0; i < res.depth.size(); ++i) {
			if (std::isinf(res.depth[i])) { ++n; }
		}
		return n;
	}

	// Index of the DAG node at the given height covering voxel (x, y, z).
	u32 nodeIndexAt(const Volume& volume, int x, int y, int z, int height)
	{
		const Internals::NodeStore& nodes = Internals::getNodes(volume);
		u32 nodeIndex = Internals::getRootNodeIndex(volume);
		const u32 ux = u32(x) + 0x80000000u;
		const u32 uy = u32(y) + 0x80000000u;
		const u32 uz = u32(z) + 0x80000000u;
		for (int h = 31; h >= height; --h)
		{
			const u32 childId = ((ux >> h) & 1u) | (((uy >> h) & 1u) << 1) | (((uz >> h) & 1u) << 2);
			nodeIndex = nodes[nodeIndex][childId];
		}
		return nodeIndex;
	}

	////////////////////////////////////////////////////////////////////////////////////////////////
	// coverage group: rasterisation correctness against the independent box reference.
	////////////////////////////////////////////////////////////////////////////////////////////////

	// An occluder wall with real holes, and a back plane visible only through them.
	int caseHoles()
	{
		Volume volume;
		std::vector<RefBox> boxes;
		for (int z = 10; z <= 11; ++z)
			for (int y = 0; y <= 15; ++y)
				for (int x = 0; x <= 23; ++x)
				{
					const bool hole = (x % 8 == 4 || x % 8 == 5) && (y % 8 == 4 || y % 8 == 5);
					if (!hole) { volume.setVoxel(x, y, z, 3); addCell(boxes, x, y, z, 3); }
				}
		fillBox(volume, 0, 0, 18, 23, 15, 19, 6); // Back plane (uniform: one reference box).
		addBox(boxes, 0, 0, 18, 23, 15, 19, 6);
		volume.bake();

		RasterCamera cam = makeCam(vec3d(11.5, 7.5, -20.0), vec3d(1, 0, 0), vec3d(0, 1, 0), vec3d(0, 0, 1),
			32, 24, 0.7, 1.0, 100.0);

		DagRasterResult on = renderDag(volume, cam, true);
		TASK_CHECK(checkAgainstReference(on, cam, boxes) == 0);
		DagRasterResult off = renderDag(volume, cam, false);
		TASK_CHECK(checkAgainstReference(off, cam, boxes) == 0);

		// Content sanity: wall, back plane through the holes, and misses at the edges.
		TASK_CHECK(countMaterial(on, 3) > 100);
		TASK_CHECK(countMaterial(on, 6) > 5);
		TASK_CHECK(countMisses(on) > 0);
		return 0;
	}

	// Oblique (non axis-aligned) cameras over scattered boxes, looking both ways.
	int caseOblique()
	{
		Volume volume;
		std::vector<RefBox> boxes;
		fillBox(volume, -10, -5, 3, -4, 2, 9, 2);   addCells(boxes, -10, -5, 3, -4, 2, 9, 2);
		fillBox(volume, 2, -8, 5, 6, -2, 12, 5);    addCells(boxes, 2, -8, 5, 6, -2, 12, 5);
		fillBox(volume, 8, 8, 8, 12, 12, 12, 1);    addCells(boxes, 8, 8, 8, 12, 12, 12, 1);
		fillBox(volume, -3, 4, 30, 4, 10, 36, 7);   addCells(boxes, -3, 4, 30, 4, 10, 36, 7);
		volume.bake();

		RasterCamera cam1 = makeObliqueCam(vec3d(0.5, -1.5, -30.0), vec3d(0.15, 0.1, 1.0), vec3d(0, 1, 0),
			48, 36, 0.6, 2.0, 200.0);
		RasterCamera cam2 = makeObliqueCam(vec3d(1.0, 0.0, 60.0), vec3d(-0.1, 0.05, -1.0), vec3d(0, 1, 0),
			48, 36, 0.6, 2.0, 200.0);

		TASK_CHECK(checkAgainstReference(renderDag(volume, cam1, true), cam1, boxes) == 0);
		TASK_CHECK(checkAgainstReference(renderDag(volume, cam1, false), cam1, boxes) == 0);
		TASK_CHECK(checkAgainstReference(renderDag(volume, cam2, true), cam2, boxes) == 0);
		TASK_CHECK(checkAgainstReference(renderDag(volume, cam2, false), cam2, boxes) == 0);
		return 0;
	}

	// The near plane cutting through material leaves, and a camera inside a coarse leaf.
	int caseNearplane()
	{
		Volume volume;
		fillBox(volume, 0, 0, 0, 15, 15, 15, 4);   // Merges into one coarse 16^3 leaf.
		fillBox(volume, 4, 4, 30, 11, 11, 35, 9);
		volume.bake();

		std::vector<RefBox> boxes;
		addBox(boxes, 0, 0, 0, 15, 15, 15, 4);     // Uniform: one reference box.
		addCells(boxes, 4, 4, 30, 11, 11, 35, 9);

		// Camera inside the coarse leaf: 'near' lies inside the solid, so depth == near.
		RasterCamera camInside = makeCam(vec3d(8.0, 8.0, 6.0), vec3d(1, 0, 0), vec3d(0, 1, 0), vec3d(0, 0, 1),
			24, 24, 0.9, 5.0, 60.0);
		DagRasterResult in = renderDag(volume, camInside, true);
		TASK_CHECK(checkAgainstReference(in, camInside, boxes) == 0);
		TASK_CHECK(countMaterial(in, 4) == 24 * 24);
		TASK_CHECK(std::fabs(in.depth[0] - 5.0) <= 1e-9 * 5.0);

		// Camera outside: the near plane slices through the leaf (which therefore
		// crosses the near plane and must not be projected through a divide).
		RasterCamera camOutside = makeCam(vec3d(8.0, 8.0, -10.0), vec3d(1, 0, 0), vec3d(0, 1, 0), vec3d(0, 0, 1),
			24, 24, 0.9, 12.0, 60.0);
		DagRasterResult out = renderDag(volume, camOutside, true);
		TASK_CHECK(checkAgainstReference(out, camOutside, boxes) == 0);
		TASK_CHECK(countMaterial(out, 4) == 24 * 24);
		TASK_CHECK(std::fabs(out.depth[0] - 12.0) <= 1e-9 * 12.0);
		return 0;
	}

	// Geometry straddling the root octant boundaries (the planes x/y/z = 0),
	// including negative coordinates, seen by an oblique camera.
	int caseOctants()
	{
		Volume volume;
		std::vector<RefBox> boxes;
		fillBox(volume, -8, -8, -8, 7, 7, 7, 1);    addCells(boxes, -8, -8, -8, 7, 7, 7, 1);
		fillBox(volume, 20, -30, 10, 27, -24, 17, 6); addCells(boxes, 20, -30, 10, 27, -24, 17, 6);
		fillBox(volume, -40, 10, 30, -33, 16, 35, 9); addCells(boxes, -40, 10, 30, -33, 16, 35, 9);
		volume.bake();

		RasterCamera cam = makeObliqueCam(vec3d(3.5, -2.5, -50.0), vec3d(0.05, 0.02, 1.0), vec3d(0, 1, 0),
			40, 30, 0.8, 1.5, 300.0);
		TASK_CHECK(checkAgainstReference(renderDag(volume, cam, true), cam, boxes) == 0);
		TASK_CHECK(checkAgainstReference(renderDag(volume, cam, false), cam, boxes) == 0);
		return 0;
	}

	// The same sub-structure instantiated at three different locations: the DAG
	// shares the nodes, and every spatial occurrence must still be drawn (no
	// global 'visited' set may swallow the repeats).
	int caseRepeat()
	{
		Volume volume;
		std::vector<RefBox> boxes;
		const int offsets[3][3] = { {0, 0, 0}, {48, 56, 64}, {-80, 40, 112} };
		for (int c = 0; c < 3; ++c)
		{
			const int ox = offsets[c][0], oy = offsets[c][1], oz = offsets[c][2];
			for (int z = 0; z < 8; ++z)
				for (int y = 0; y < 8; ++y)
					for (int x = 0; x < 8; ++x)
					{
						if (x == 7 && y == 7 && z == 7) { continue; } // Distinctive corner notch.
						volume.setVoxel(ox + x, oy + y, oz + z, 2);
						addCell(boxes, ox + x, oy + y, oz + z, 2);
					}
		}
		volume.bake();

		// The three 8^3 sub-trees really are shared in the DAG.
		const u32 n0 = nodeIndexAt(volume, 0, 0, 0, 3);
		const u32 n1 = nodeIndexAt(volume, 48, 56, 64, 3);
		const u32 n2 = nodeIndexAt(volume, -80, 40, 112, 3);
		TASK_CHECK(!Internals::isMaterialNode(n0));
		TASK_CHECK(n0 == n1 && n1 == n2);

		RasterCamera cam = makeCam(vec3d(-10.0, 35.0, -150.0), vec3d(1, 0, 0), vec3d(0, 1, 0), vec3d(0, 0, 1),
			64, 48, 0.7, 1.0, 500.0);
		DagRasterResult on = renderDag(volume, cam, true);
		TASK_CHECK(checkAgainstReference(on, cam, boxes) == 0);
		TASK_CHECK(checkAgainstReference(renderDag(volume, cam, false), cam, boxes) == 0);
		TASK_CHECK(countMaterial(on, 2) >= 3); // All three occurrences visible.
		return 0;
	}

	// Two rounds of camera changes and scene edits: every render starts from an
	// empty hierarchy, so stale occlusion must never leak across frames.
	int caseTwoRounds()
	{
		Volume volume;
		fillBox(volume, 0, 0, 10, 10, 10, 15, 3);
		fillBox(volume, 20, 0, 10, 30, 10, 20, 8);
		volume.bake();

		std::vector<RefBox> boxes1;
		addCells(boxes1, 0, 0, 10, 10, 10, 15, 3);
		addCells(boxes1, 20, 0, 10, 30, 10, 20, 8);

		RasterCamera cam1 = makeCam(vec3d(15.0, 5.0, -20.0), vec3d(1, 0, 0), vec3d(0, 1, 0), vec3d(0, 0, 1),
			32, 32, 0.8, 1.0, 100.0);
		RasterCamera cam2 = makeObliqueCam(vec3d(15.0, 5.0, -25.0), vec3d(0.05, 0.0, 1.0), vec3d(0, 1, 0),
			32, 32, 0.8, 1.0, 100.0);

		// Round 1: first camera.
		DagRasterResult r1 = renderDag(volume, cam1, true);
		TASK_CHECK(checkAgainstReference(r1, cam1, boxes1) == 0);
		DagRasterResult r1off = renderDag(volume, cam1, false);
		TASK_CHECK(r1off.depth == r1.depth && r1off.material == r1.material);

		// Round 2: camera change (same scene).
		DagRasterResult r2 = renderDag(volume, cam2, true);
		TASK_CHECK(checkAgainstReference(r2, cam2, boxes1) == 0);

		// Round 3: scene edit (carve a through-hole, add a new box), then re-render.
		for (int z = 10; z <= 15; ++z)
			for (int y = 3; y <= 6; ++y)
				for (int x = 3; x <= 6; ++x)
					volume.setVoxel(x, y, z, 0);
		fillBox(volume, 8, 8, 5, 12, 12, 9, 5);
		volume.bake();

		std::vector<RefBox> boxes2;
		for (int z = 10; z <= 15; ++z)
			for (int y = 0; y <= 10; ++y)
				for (int x = 0; x <= 10; ++x)
				{
					const bool carved = (x >= 3 && x <= 6 && y >= 3 && y <= 6);
					if (!carved) { addCell(boxes2, x, y, z, 3); }
				}
		addCells(boxes2, 20, 0, 10, 30, 10, 20, 8);
		addCells(boxes2, 8, 8, 5, 12, 12, 9, 5);

		DagRasterResult r3 = renderDag(volume, cam2, true);
		TASK_CHECK(checkAgainstReference(r3, cam2, boxes2) == 0);
		DagRasterResult r3off = renderDag(volume, cam2, false);
		TASK_CHECK(r3off.depth == r3.depth && r3off.material == r3.material);
		DagRasterResult r4 = renderDag(volume, cam1, true);
		TASK_CHECK(checkAgainstReference(r4, cam1, boxes2) == 0);
		return 0;
	}

	// A pure material root (the whole 2^32 domain is one leaf) checked against an
	// independent big-box intersection, plus the empty root.
	int casePureRoot()
	{
		Volume full;
		full.fill(7);
		RasterCamera cam = makeObliqueCam(vec3d(100.5, -50.25, 1000.0), vec3d(-0.1, 0.05, -1.0), vec3d(0, 1, 0),
			32, 24, 0.7, 3.0, 5000.0);

		std::vector<RefBox> boxes;
		boxes.push_back({ -2147483648.5, -2147483648.5, -2147483648.5,
						   2147483647.5,  2147483647.5,  2147483647.5, 7 });

		DagRasterResult res = renderDag(full, cam, true);
		TASK_CHECK(checkAgainstReference(res, cam, boxes) == 0);
		TASK_CHECK(countMaterial(res, 7) == 32 * 24); // Camera is inside the solid.
		TASK_CHECK(std::fabs(res.depth[0] - 3.0) <= 1e-9 * 3.0); // depth == near
		TASK_CHECK(res.leafBoxes <= 8);   // One giant leaf, never expanded by 2^32.
		TASK_CHECK(res.leafBoxes == 1);
		TASK_CHECK(res.nodeVisits == 1);

		// Empty root: everything misses.
		Volume empty;
		DagRasterResult resEmpty = renderDag(empty, cam, true);
		TASK_CHECK(resEmpty.nodeVisits == 0);
		TASK_CHECK(resEmpty.leafBoxes == 0);
		TASK_CHECK(countMisses(resEmpty) == 32 * 24);
		std::vector<RefBox> noBoxes;
		TASK_CHECK(checkAgainstReference(resEmpty, cam, noBoxes) == 0);
		return 0;
	}

	////////////////////////////////////////////////////////////////////////////////////////////////
	// hierarchy group: DepthHierarchy unit tests.
	////////////////////////////////////////////////////////////////////////////////////////////////

	// Odd dimensions: level sizes use ceiling halving and edge texels cover only
	// real pixels (no fictitious padding).
	int caseOddSize()
	{
		DepthHierarchy h;
		h.reset(63, 47);
		TASK_CHECK(h.width() == 63 && h.height() == 47);
		TASK_CHECK(h.levelCount() == 7);
		TASK_CHECK(h.levelWidth(0) == 63 && h.levelHeight(0) == 47);
		TASK_CHECK(h.levelWidth(1) == 32 && h.levelHeight(1) == 24);
		TASK_CHECK(h.levelWidth(2) == 16 && h.levelHeight(2) == 12);
		TASK_CHECK(h.levelWidth(3) == 8 && h.levelHeight(3) == 6);
		TASK_CHECK(h.levelWidth(4) == 4 && h.levelHeight(4) == 3);
		TASK_CHECK(h.levelWidth(5) == 2 && h.levelHeight(5) == 2);
		TASK_CHECK(h.levelWidth(6) == 1 && h.levelHeight(6) == 1);
		TASK_CHECK(h.texel(6, 0, 0) == Inf);
		TASK_CHECK(h.reads() == 0 && h.updates() == 0);

		// Write the extreme corner pixel. Its level-1 parent covers only that one
		// real pixel (63x47 is odd), so it takes the value exactly; wider texels
		// still cover unwritten (empty) pixels and therefore remain EmptyDepth.
		h.writePixel(62, 46, 5.0);
		TASK_CHECK(h.texel(0, 62, 46) == 5.0);
		TASK_CHECK(h.texel(1, 31, 23) == 5.0); // Edge texel covers only pixel (62,46).
		TASK_CHECK(h.texel(2, 15, 11) == Inf); // Still covers empty pixels too.
		TASK_CHECK(h.texel(6, 0, 0) == Inf);
		TASK_CHECK(h.updates() == 2); // Level 0 plus the one fully-covered ancestor.

		// No padding: neighbouring edge texels (only unwritten pixels) stay empty.
		TASK_CHECK(h.texel(1, 30, 23) == Inf); // Pixels (60..61, 46).
		TASK_CHECK(h.texel(1, 31, 22) == Inf); // Pixels (62, 44..45).

		// Overwriting with a nearer depth lowers exactly the fully-covered ancestor.
		h.writePixel(62, 46, 3.0);
		TASK_CHECK(h.texel(1, 31, 23) == 3.0);
		TASK_CHECK(h.updates() == 4);

		// Complete the 3x3 real pixel coverage of level-2 texel (15,11) at depth 4:
		// it now takes the maximum (4), while level-1 edge texels keep reflecting
		// only their real pixels.
		for (int y = 44; y <= 46; ++y)
			for (int x = 60; x <= 62; ++x)
				if (x != 62 || y != 46) { h.writePixel(x, y, 4.0); }
		TASK_CHECK(h.texel(2, 15, 11) == 4.0);
		TASK_CHECK(h.texel(1, 31, 23) == 3.0); // Still only pixel (62,46).
		TASK_CHECK(h.texel(1, 30, 23) == 4.0); // Pixels (60..61, 46).
		TASK_CHECK(h.texel(1, 31, 22) == 4.0); // Pixels (62, 44..45).
		TASK_CHECK(h.texel(6, 0, 0) == Inf);   // Most of the screen is still empty.

		// Fill the rest of the screen at depth 8: the top texel now has a value.
		for (int y = 0; y < 47; ++y)
			for (int x = 0; x < 63; ++x)
				if (!(x >= 60 && x <= 62 && y >= 44 && y <= 46)) { h.writePixel(x, y, 8.0); }
		TASK_CHECK(h.texel(6, 0, 0) == 8.0);
		TASK_CHECK(h.texel(3, 7, 5) == 8.0); // Covers pixels [56,63)x[40,47): max(3,4,8).
		TASK_CHECK(h.isOccluded(0, 0, 63, 47, 9.0));  // Everything nearer than 9.
		TASK_CHECK(!h.isOccluded(0, 0, 63, 47, 8.0)); // Equal depth must not occlude.
		return 0;
	}

	// Occlusion state changes as local writes land: holes, equal depths and
	// overwrites in both directions.
	int caseWrites()
	{
		DepthHierarchy h;
		h.reset(8, 8);
		TASK_CHECK(h.levelCount() == 4);
		TASK_CHECK(!h.isOccluded(2, 2, 4, 4, 10.0)); // Empty.

		h.writePixel(2, 2, 5.0);
		h.writePixel(3, 2, 5.0);
		h.writePixel(2, 3, 5.0);
		TASK_CHECK(!h.isOccluded(2, 2, 4, 4, 10.0)); // Hole at (3,3).
		h.writePixel(3, 3, 5.0);
		TASK_CHECK(h.isOccluded(2, 2, 4, 4, 10.0));  // Fully covered by nearer depths.
		TASK_CHECK(!h.isOccluded(2, 2, 4, 4, 5.0));  // Equal depth must not occlude.
		TASK_CHECK(h.isOccluded(2, 2, 4, 4, 5.5));   // Strictly nearer blockers.

		// Farther overwrite: the local maximum grows.
		h.writePixel(2, 2, 7.0);
		TASK_CHECK(h.texel(1, 1, 1) == 7.0);
		TASK_CHECK(h.isOccluded(2, 2, 4, 4, 10.0));
		TASK_CHECK(!h.isOccluded(2, 2, 4, 4, 7.0));

		// Nearer overwrite: the local maximum is recomputed exactly.
		h.writePixel(2, 2, 3.0);
		TASK_CHECK(h.texel(1, 1, 1) == 5.0);
		TASK_CHECK(h.isOccluded(2, 2, 4, 4, 6.0));
		TASK_CHECK(!h.isOccluded(2, 2, 4, 4, 5.0));

		// Wider ancestors still cover unwritten pixels, so they remain empty.
		TASK_CHECK(h.texel(2, 0, 0) == Inf);
		TASK_CHECK(h.texel(2, 1, 1) == Inf);
		TASK_CHECK(h.texel(3, 0, 0) == Inf);

		// Complete the real coverage of level-2 texel (0,0): it takes the max.
		for (int y = 0; y < 4; ++y)
			for (int x = 0; x < 4; ++x)
				if (!(x >= 2 && x <= 3 && y >= 2 && y <= 3)) { h.writePixel(x, y, 6.0); }
		TASK_CHECK(h.texel(2, 0, 0) == 6.0); // max(3, 5, 6) over [0,4)^2
		TASK_CHECK(h.texel(2, 1, 1) == Inf); // Untouched region stays empty.

		// Fill the rest of the screen: the top texel becomes the global maximum.
		for (int y = 0; y < 8; ++y)
			for (int x = 0; x < 8; ++x)
				if (x >= 4 || y >= 4) { h.writePixel(x, y, 9.0); }
		TASK_CHECK(h.texel(3, 0, 0) == 9.0);
		TASK_CHECK(h.isOccluded(0, 0, 8, 8, 10.0));
		TASK_CHECK(!h.isOccluded(0, 0, 8, 8, 9.0));
		return 0;
	}

	// Query semantics: full-screen single-read top-level query, clamping, holes,
	// equal depths, mixed depths, and reset.
	int caseQueries()
	{
		DepthHierarchy h;
		h.reset(16, 16);
		TASK_CHECK(!h.isOccluded(0, 0, 16, 16, 1e9)); // Empty hierarchy occludes nothing.

		for (int y = 0; y < 16; ++y)
			for (int x = 0; x < 16; ++x)
				h.writePixel(x, y, 10.0);

		TASK_CHECK(h.isOccluded(0, 0, 16, 16, 20.0));  // Strictly nearer blockers.
		TASK_CHECK(!h.isOccluded(0, 0, 16, 16, 10.0)); // Equal depth must not occlude.
		TASK_CHECK(!h.isOccluded(0, 0, 16, 16, 5.0));  // Query is nearer than blockers.

		// A fully-covered full-screen query is answered with a single top-level read.
		const u64 readsBefore = h.reads();
		TASK_CHECK(h.isOccluded(0, 0, 16, 16, 20.0));
		TASK_CHECK(h.reads() - readsBefore == 1);

		// Rectangles are clamped to the image; empty ones never occlude.
		TASK_CHECK(h.isOccluded(-10, -10, 100, 100, 20.0));
		TASK_CHECK(!h.isOccluded(100, 100, 200, 200, 20.0));
		TASK_CHECK(!h.isOccluded(4, 4, 4, 8, 20.0));

		// A single hole anywhere defeats occlusion, but only where it is.
		DepthHierarchy h2;
		h2.reset(8, 8);
		for (int y = 0; y < 8; ++y)
			for (int x = 0; x < 8; ++x)
				if (x != 3 || y != 5) { h2.writePixel(x, y, 10.0); }
		TASK_CHECK(!h2.isOccluded(0, 0, 8, 8, 20.0));
		TASK_CHECK(!h2.isOccluded(3, 5, 4, 6, 20.0));
		TASK_CHECK(h2.isOccluded(4, 5, 8, 6, 20.0));

		// Mixed depths: the hierarchy stores maxima.
		DepthHierarchy h3;
		h3.reset(8, 8);
		for (int y = 0; y < 8; ++y)
			for (int x = 0; x < 8; ++x)
				h3.writePixel(x, y, x < 4 ? 10.0 : 30.0);
		TASK_CHECK(h3.isOccluded(0, 0, 4, 8, 20.0));
		TASK_CHECK(!h3.isOccluded(0, 0, 8, 8, 20.0));
		TASK_CHECK(h3.isOccluded(4, 0, 8, 8, 40.0));

		// reset() clears depths and counters for the next frame.
		h3.reset(8, 8);
		TASK_CHECK(h3.reads() == 0 && h3.updates() == 0);
		TASK_CHECK(h3.texel(0, 0, 0) == Inf);
		TASK_CHECK(!h3.isOccluded(0, 0, 8, 8, 1e9));
		return 0;
	}

	////////////////////////////////////////////////////////////////////////////////////////////////
	// sparse group: occlusion efficacy on the DAG, coarse-leaf workload, example image.
	////////////////////////////////////////////////////////////////////////////////////////////////

	// The specified scene: solid material-4 foreground [0,63]^3 and an alternating
	// material background (x/y in [16,31], z in [80,95]), with the specified camera.
	int caseOcclusion()
	{
		Volume volume;
		fillBox(volume, 0, 0, 0, 63, 63, 63, 4);
		for (int z = 80; z <= 95; ++z)
			for (int y = 16; y <= 31; ++y)
				for (int x = 16; x <= 31; ++x)
					volume.setVoxel(x, y, z, ((x ^ y ^ z) & 1) ? 2 : 3);
		volume.bake();

		RasterCamera cam;
		cam.eye     = vec3d(31.2, 31.4, -64.0);
		cam.right   = vec3d(1.0, 0.0, 0.0);
		cam.up      = vec3d(0.0, 1.0, 0.0);
		cam.forward = vec3d(0.0, 0.0, 1.0);
		cam.width   = 64;
		cam.height  = 48;
		cam.fovY    = 0.5;
		cam.near    = 1.0;
		cam.far     = 200.0;

		DagRasterResult on = renderDag(volume, cam, true);
		DagRasterResult off = renderDag(volume, cam, false);

		// Occlusion culling must not change the image.
		TASK_CHECK(on.width == 64 && on.height == 48);
		TASK_CHECK(on.depth == off.depth);
		TASK_CHECK(on.material == off.material);

		// ... but it must do dramatically less work, and actually reject sub-trees.
		TASK_CHECK(on.hizRejected > 0);
		TASK_CHECK(on.leafBoxes * 4 < off.leafBoxes);
		TASK_CHECK(on.leafBoxes == 1);      // Just the merged foreground leaf.
		TASK_CHECK(off.leafBoxes == 4097);  // Foreground + 16^3 checkerboard leaves.
		TASK_CHECK(on.hizReads > 0 && on.hizUpdates > 0);
		TASK_CHECK(off.hizReads == 0 && off.hizUpdates == 0 && off.hizRejected == 0);

		// Independent reference: the foreground as one solid box, the background per cell.
		std::vector<RefBox> boxes;
		addBox(boxes, 0, 0, 0, 63, 63, 63, 4);
		for (int z = 80; z <= 95; ++z)
			for (int y = 16; y <= 31; ++y)
				for (int x = 16; x <= 31; ++x)
					addCell(boxes, x, y, z, ((x ^ y ^ z) & 1) ? 2 : 3);
		TASK_CHECK(checkAgainstReference(on, cam, boxes) == 0);

		// The foreground covers the whole frame at a uniform depth of 63.5.
		TASK_CHECK(countMaterial(on, 4) == 64 * 48);
		TASK_CHECK(std::fabs(on.depth[0] - 63.5) <= 1e-9 * 63.5);
		return 0;
	}

	// A large uniform region is committed as a single coarse leaf: the workload
	// follows the screen size, not the leaf's voxel count.
	int caseCoarseLeaf()
	{
		Volume volume;
		fillBox(volume, 0, 0, 128, 63, 63, 191, 9); // One aligned 64^3 block.
		volume.bake();

		RasterCamera cam = makeCam(vec3d(32.0, 32.0, 0.0), vec3d(1, 0, 0), vec3d(0, 1, 0), vec3d(0, 0, 1),
			64, 64, 0.4, 1.0, 500.0);
		DagRasterResult res = renderDag(volume, cam, true);
		TASK_CHECK(res.leafBoxes == 1);   // Not subdivided into unit cells.
		TASK_CHECK(res.nodeVisits <= 64); // Root chain plus the leaf.
		TASK_CHECK(countMaterial(res, 9) == 64 * 64);

		std::vector<RefBox> boxes;
		addBox(boxes, 0, 0, 128, 63, 63, 191, 9);
		TASK_CHECK(checkAgainstReference(res, cam, boxes) == 0);
		return 0;
	}

	// The PPM produced by examples/dag_preview.cpp: dimensions and expected colours.
	int casePpm()
	{
		std::ifstream file(".task-build/dag.ppm", std::ios::binary);
		TASK_CHECK(file.is_open());

		std::string magic;
		int w = 0, h = 0, maxval = 0;
		file >> magic >> w >> h >> maxval;
		TASK_CHECK(magic == "P6");
		TASK_CHECK(w >= 63);
		TASK_CHECK(h >= 47);
		TASK_CHECK(maxval == 255);
		file.get(); // Single whitespace after the header.

		std::vector<unsigned char> pixels(static_cast<size_t>(w) * h * 3);
		file.read(reinterpret_cast<char*>(pixels.data()), std::streamsize(pixels.size()));
		TASK_CHECK(file.gcount() == std::streamsize(pixels.size()));

		int occluder = 0, rearA = 0, rearB = 0;
		for (size_t i = 0; i + 2 < pixels.size(); i += 3)
		{
			const int r = pixels[i], g = pixels[i + 1], b = pixels[i + 2];
			if (r == 204 && g == 68 && b == 68) { ++occluder; }       // Red wall.
			else if (r == 68 && g == 204 && b == 221) { ++rearA; }    // Cyan stripe.
			else if (r == 240 && g == 220 && b == 90) { ++rearB; }    // Yellow stripe.
		}
		// The occluder dominates, and both rear stripe colours show through the holes.
		TASK_CHECK(occluder > 500);
		TASK_CHECK(rearA > 30);
		TASK_CHECK(rearB > 30);
		return 0;
	}
}

const struct task_case task_cases[] =
{
	{ "coverage", "holes", caseHoles },
	{ "coverage", "oblique", caseOblique },
	{ "coverage", "nearplane", caseNearplane },
	{ "coverage", "octants", caseOctants },
	{ "coverage", "repeat", caseRepeat },
	{ "coverage", "tworounds", caseTwoRounds },
	{ "coverage", "pureroot", casePureRoot },
	{ "hierarchy", "oddsize", caseOddSize },
	{ "hierarchy", "writes", caseWrites },
	{ "hierarchy", "queries", caseQueries },
	{ "sparse", "occlusion", caseOcclusion },
	{ "sparse", "coarseleaf", caseCoarseLeaf },
	{ "sparse", "ppm", casePpm },
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
