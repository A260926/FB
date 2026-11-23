/***************************************************************************************************
* Cubiquity - A micro-voxel engine for games and other interactive applications                    *
*                                                                                                  *
* Written in 2026 by David Williams                                                                *
*                                                                                                  *
* To the extent possible under law, the author(s) have dedicated all copyright and related and     *
* neighboring rights to this software to the public domain worldwide. This software is distributed *
* without any warranty.                                                                            *
*                                                                                                  *
* You should have received a copy of the CC0 Public Domain Dedication along with this software.    *
* If not, see http://creativecommons.org/publicdomain/zero/1.0/.                                   *
***************************************************************************************************/
//
// Tests for the CPU DAG rasterizer and its depth hierarchy.
//
// Registered through task_support/check.h; this file defines no main().
// Three groups: coverage (independent voxel-box reference comparison), hierarchy (the
// DepthHierarchy in isolation), and sparse (DAG-specific behaviour: shared subgraphs,
// large coarse leaves, the preview PPM, work counters).

#include "../task_support/check.h"

#include "dag_rasterizer.h"
#include "depth_hierarchy.h"
#include "storage.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

using namespace Cubiquity;

namespace
{
	// ---------------------------------------------------------------------------------------
	// Small helpers shared by the test cases.
	// ---------------------------------------------------------------------------------------

	struct Cell
	{
		int x, y, z;
		MaterialId material;
	};

	RasterCamera makeCamera(double ex, double ey, double ez, int width, int height,
		double fovY, double nearPlane, double farPlane,
		vec3d right = vec3d(1, 0, 0), vec3d up = vec3d(0, 1, 0), vec3d forward = vec3d(0, 0, 1))
	{
		RasterCamera camera;
		camera.eye = vec3d(ex, ey, ez);
		camera.right = right;
		camera.up = up;
		camera.forward = forward;
		camera.width = width;
		camera.height = height;
		camera.fovY = fovY;
		camera.nearPlane = nearPlane;
		camera.farPlane = farPlane;
		return camera;
	}

	// Independent reference ray test against individual solid voxel boxes. The voxel at
	// integer q is the solid region [q-0.5, q+0.5]. Returns the first positive-length entry
	// inside [near, far], with ties broken by the smaller material id.
	bool referenceRay(const std::vector<Cell>& cells, const vec3d& origin, const vec3d& dir,
		double nearPlane, double farPlane, double& depthOut, MaterialId& materialOut)
	{
		depthOut = std::numeric_limits<double>::infinity();
		materialOut = 0;
		bool hit = false;
		const double o[3] = { origin.x, origin.y, origin.z };
		const double d[3] = { dir.x, dir.y, dir.z };
		for (const Cell& cell : cells)
		{
			if (cell.material == 0) continue;
			const double lo[3] = { cell.x - 0.5, cell.y - 0.5, cell.z - 0.5 };
			const double hi[3] = { cell.x + 0.5, cell.y + 0.5, cell.z + 0.5 };
			double entry = -std::numeric_limits<double>::infinity();
			double exit = std::numeric_limits<double>::infinity();
			bool outside = false;
			for (int axis = 0; axis < 3; ++axis)
			{
				if (d[axis] == 0.0)
				{
					if (o[axis] < lo[axis] || o[axis] > hi[axis]) { outside = true; break; }
				}
				else
				{
					double ta = (lo[axis] - o[axis]) / d[axis];
					double tb = (hi[axis] - o[axis]) / d[axis];
					if (ta > tb) std::swap(ta, tb);
					entry = std::max(entry, ta);
					exit = std::min(exit, tb);
				}
			}
			if (outside) continue;
			entry = std::max(entry, nearPlane);
			exit = std::min(exit, farPlane);
			if (!(entry < exit)) continue;
			if (entry < depthOut || (entry == depthOut && cell.material < materialOut))
			{
				depthOut = entry;
				materialOut = cell.material;
				hit = true;
			}
		}
		return hit;
	}

	// Renders a volume and checks every pixel against the independent reference. Fills in the
	// number of mismatches; tolerance is a relative 1e-9.
	int compareToReference(const RasterResult& result, const RasterCamera& camera,
		const std::vector<Cell>& cells, int* pixelCount = nullptr)
	{
		int mismatches = 0;
		for (int y = 0; y < camera.height; ++y)
		{
			for (int x = 0; x < camera.width; ++x)
			{
				const vec3d dir = camera.rayDirection(x, y);
				double refDepth = 0.0;
				MaterialId refMaterial = 0;
				const bool refHit = referenceRay(cells, camera.eye, dir, camera.nearPlane, camera.farPlane, refDepth, refMaterial);

				const double gotDepth = result.depthAt(x, y);
				const MaterialId gotMaterial = result.materialAt(x, y);
				const bool gotHit = !std::isinf(gotDepth);

				bool bad = (refHit != gotHit);
				if (!bad && refHit)
				{
					const double tolerance = 1e-9 * std::max(1.0, std::fabs(refDepth));
					bad = (refMaterial != gotMaterial) || (std::fabs(refDepth - gotDepth) > tolerance);
				}
				if (bad) ++mismatches;
				if (pixelCount) ++*pixelCount;
			}
		}
		return mismatches;
	}

	bool sameImage(const RasterResult& a, const RasterResult& b)
	{
		if (a.width != b.width || a.height != b.height) return false;
		for (size_t i = 0; i < a.depth.size(); ++i)
		{
			if (a.depth[i] != b.depth[i]) return false;
			if (a.material[i] != b.material[i]) return false;
		}
		return true;
	}

	// Builds the "specification" scene: solid material-4 foreground [0,63]^3 and an
	// alternating material-2/3 background in x/y in [16,31], z in [80,95].
	void buildSpecScene(Volume& volume)
	{
		for (int z = 0; z < 64; ++z)
			for (int y = 0; y < 64; ++y)
				for (int x = 0; x < 64; ++x)
					volume.setVoxel(x, y, z, 4);

		for (int z = 80; z < 96; ++z)
			for (int y = 16; y < 32; ++y)
				for (int x = 16; x < 32; ++x)
					volume.setVoxel(x, y, z, (((x + y + z) & 1) != 0) ? 2 : 3);

		volume.bake();
	}

	RasterCamera specCamera()
	{
		return makeCamera(31.2, 31.4, -64.0, 64, 48, 0.5, 1.0, 200.0);
	}
}

// ===========================================================================================
// Group: coverage
// ===========================================================================================

namespace
{
	// A solid box rendered from a range of camera setups is compared pixel-by-pixel with the
	// independent voxel-box reference. Covers a head-on view plus holes, oblique views and a
	// near plane which cuts through the material.
	int test_coverage_solid_box()
	{
		Volume volume;
		std::vector<Cell> cells;
		for (int z = 3; z <= 7; ++z)
			for (int y = 3; y <= 7; ++y)
				for (int x = 3; x <= 7; ++x)
				{
					volume.setVoxel(x, y, z, 5);
					cells.push_back({ x, y, z, 5 });
				}
		volume.bake();

		const RasterCamera cameras[] = {
			makeCamera(5, 5, -20, 32, 24, 0.8, 1, 100),   // head on
			makeCamera(-30, 12, -11, 40, 30, 1.0, 1, 100), // oblique
			makeCamera(5, 5, 5, 32, 24, 0.8, 1, 100),      // camera inside the box
		};
		for (const RasterCamera& camera : cameras)
		{
			const RasterResult result = renderDag(volume, camera, false);
			TASK_CHECK(compareToReference(result, camera, cells) == 0);
		}

		// Near plane cutting through the front of the material: depth should clamp to near.
		{
			const RasterCamera camera = makeCamera(5, 5, -1, 32, 24, 0.8, 4, 8);
			const RasterResult result = renderDag(volume, camera, false);
			TASK_CHECK(compareToReference(result, camera, cells) == 0);
			bool sawNear = false;
			for (double d : result.depth) if (d == 4.0) sawNear = true;
			TASK_CHECK(sawNear);
		}
		return 0;
	}

	// A blocky structure with a tunnel and a side gap, spanning negative coordinates, viewed
	// obliquely. Verifies holes and negative-coordinate support against the reference.
	int test_coverage_holes_and_negative()
	{
		Volume volume;
		std::vector<Cell> cells;
		for (int z = -8; z < 8; ++z)
			for (int y = -8; y < 8; ++y)
				for (int x = -8; x < 8; ++x)
				{
					bool solid = true;
					if (x >= -2 && x <= 3 && y >= -2 && y <= 3) solid = false; // tunnel
					if (z == 0 && (x < -5 || x > 4)) solid = false;            // side gap
					if (solid) { volume.setVoxel(x, y, z, 3); cells.push_back({ x, y, z, 3 }); }
				}
		volume.bake();

		const RasterCamera cameras[] = {
			makeCamera(0, 0, -26, 37, 25, 0.9, 1, 100),
			makeCamera(11, 6, -24, 41, 27, 1.1, 2, 80),
			makeCamera(-15, 9, -22, 29, 33, 0.7, 1, 120),
		};
		for (const RasterCamera& camera : cameras)
		{
			const RasterResult result = renderDag(volume, camera, false);
			TASK_CHECK(compareToReference(result, camera, cells) == 0);
		}
		return 0;
	}

	// Multi-material scene: two boxes at different depths with different materials. Checks
	// that materials are reported exactly (no averaging) and that depth ties favour the
	// smaller material id.
	int test_coverage_multi_material()
	{
		Volume volume;
		std::vector<Cell> cells;
		for (int z = 10; z <= 14; ++z)
			for (int y = 10; y <= 14; ++y)
				for (int x = 10; x <= 14; ++x)
				{ volume.setVoxel(x, y, z, 3); cells.push_back({ x, y, z, 3 }); }
		for (int z = 20; z <= 24; ++z)
			for (int y = 2; y <= 6; ++y)
				for (int x = 2; x <= 6; ++x)
				{ volume.setVoxel(x, y, z, 8); cells.push_back({ x, y, z, 8 }); }
		volume.bake();

		const RasterCamera camera = makeCamera(8, 6, -20, 48, 36, 1.0, 1, 100);
		const RasterResult result = renderDag(volume, camera, false);
		TASK_CHECK(compareToReference(result, camera, cells) == 0);

		// Occlusion on and off must agree pixel-for-pixel on the same scene.
		const RasterResult occluded = renderDag(volume, camera, true);
		TASK_CHECK(sameImage(result, occluded));
		return 0;
	}

	// The exact scene from the specification, rendered with occlusion on and off. Both paths
	// must agree, and the occlusion path must do less leaf work; near surfaces must report
	// exact depths (the front face lies at forward depth 63.5).
	int test_coverage_spec_scene()
	{
		Volume volume;
		buildSpecScene(volume);
		const RasterCamera camera = specCamera();

		const RasterResult on = renderDag(volume, camera, true);
		const RasterResult off = renderDag(volume, camera, false);

		TASK_CHECK(sameImage(on, off));
		TASK_CHECK(on.materialAt(32, 24) == 4);        // centre hit by the foreground
		TASK_CHECK(on.depthAt(32, 24) == 63.5);        // front face at z = 0.5, eye z = -64
		TASK_CHECK(on.stats.hizRejected > 0);
		TASK_CHECK(on.stats.leafBoxes * 4 < off.stats.leafBoxes);

		// Uniform-material root: a single material fill must not expand into 2^32 voxels.
		Volume uniform;
		uniform.fill(5);
		const RasterResult root = renderDag(uniform, camera, false);
		TASK_CHECK(root.stats.leafBoxes <= 8);
		TASK_CHECK(root.materialAt(32, 24) == 5);
		TASK_CHECK(!std::isinf(root.depthAt(32, 24)));
		return 0;
	}

	// Pixel rays must follow the documented (unnormalized) direction formula; verify a couple
	// of directions and that the depth is the forward depth (t == dot(d, forward)).
	int test_coverage_ray_formula()
	{
		const RasterCamera camera = makeCamera(0, 0, 0, 64, 48, 0.6, 1, 100);
		const double tanHalf = std::tan(camera.fovY * 0.5);
		const double aspect = 64.0 / 48.0;
		for (int y : { 0, 7, 24, 47 })
		{
			for (int x : { 0, 3, 32, 63 })
			{
				const vec3d d = camera.rayDirection(x, y);
				vec3d expected = camera.forward
					+ camera.right * ((2.0 * (x + 0.5) / camera.width - 1.0) * tanHalf * aspect)
					+ camera.up * ((1.0 - 2.0 * (y + 0.5) / camera.height) * tanHalf);
				TASK_CHECK(std::fabs(d.x - expected.x) < 1e-15);
				TASK_CHECK(std::fabs(d.y - expected.y) < 1e-15);
				TASK_CHECK(std::fabs(d.z - expected.z) < 1e-15);
				// forward is unit and orthogonal to right/up, so dot(d, forward) == 1.
				TASK_CHECK(std::fabs(d.z - 1.0) < 1e-15);
			}
		}
		return 0;
	}

	// An empty root must report a miss (depth = +infinity, material 0) everywhere.
	int test_coverage_empty_root()
	{
		Volume volume;
		const RasterResult result = renderDag(volume, specCamera(), true);
		for (size_t i = 0; i < result.depth.size(); ++i)
		{
			TASK_CHECK(std::isinf(result.depth[i]));
			TASK_CHECK(result.material[i] == 0);
		}
		TASK_CHECK(result.stats.leafBoxes == 0);
		return 0;
	}
}

// ===========================================================================================
// Group: hierarchy
// ===========================================================================================

namespace
{
	// Level 0 must equal the pixel depths, and every parent cell must equal the maximum of its
	// real children (including on odd dimensions, where a parent may have one child).
	int test_hierarchy_levels_odd()
	{
		const int width = 13;
		const int height = 9;
		DepthHierarchy hierarchy(width, height);
		TASK_CHECK(hierarchy.levelWidth(0) == width);
		TASK_CHECK(hierarchy.levelHeight(0) == height);

		std::vector<double> pixels(static_cast<size_t>(width) * height, DepthHierarchy::EmptyDepth);
		std::uint64_t updates = 0;
		for (int y = 0; y < height; ++y)
		{
			for (int x = 0; x < width; ++x)
			{
				const double d = 1.0 + (x * 7 + y * 3) % 11;
				hierarchy.writePixel(x, y, d, &updates);
				pixels[static_cast<size_t>(y) * width + x] = d;
			}
		}
		TASK_CHECK(updates > 0);

		for (int y = 0; y < height; ++y)
			for (int x = 0; x < width; ++x)
				TASK_CHECK(hierarchy.depth(x, y) == pixels[static_cast<size_t>(y) * width + x]);

		for (int level = 1; level < hierarchy.levelCount(); ++level)
		{
			const int lw = hierarchy.levelWidth(level);
			const int lh = hierarchy.levelHeight(level);
			const int cw = hierarchy.levelWidth(level - 1);
			const int ch = hierarchy.levelHeight(level - 1);
			const std::vector<double>& parent = hierarchy.level(level);
			const std::vector<double>& child = hierarchy.level(level - 1);
			for (int cy = 0; cy < lh; ++cy)
			{
				for (int cx = 0; cx < lw; ++cx)
				{
					double expected = -std::numeric_limits<double>::infinity();
					for (int dy = 0; dy < 2; ++dy)
						for (int dx = 0; dx < 2; ++dx)
						{
							const int px = cx * 2 + dx;
							const int py = cy * 2 + dy;
							if (px < cw && py < ch) expected = std::max(expected, child[static_cast<size_t>(py) * cw + px]);
						}
					if (expected == -std::numeric_limits<double>::infinity()) expected = DepthHierarchy::EmptyDepth;
					TASK_CHECK(parent[static_cast<size_t>(cy) * lw + cx] == expected);
				}
			}
		}
		return 0;
	}

	// Occlusion only succeeds when every pixel is strictly nearer; holes and equal depths must
	// not be rejected. A hole is a pixel which was never written.
	int test_hierarchy_occlusion_rules()
	{
		DepthHierarchy hierarchy(8, 8);
		TASK_CHECK(!hierarchy.isOccluded(0, 0, 8, 8, 5.0)); // empty hierarchy

		for (int y = 0; y < 8; ++y)
			for (int x = 0; x < 8; ++x)
				if (!(x == 2 && y == 2)) hierarchy.writePixel(x, y, 4.0);

		TASK_CHECK(!hierarchy.isOccluded(0, 0, 8, 8, 5.0)); // covers the hole
		// A rectangle which excludes the hole is still rejected.
		TASK_CHECK(hierarchy.isOccluded(3, 3, 8, 8, 5.0));
		// Filling the hole lets the full-screen query succeed.
		hierarchy.writePixel(2, 2, 4.0);
		TASK_CHECK(hierarchy.isOccluded(0, 0, 8, 8, 5.0));

		// Equal depth must not be rejected, and a farther candidate must not be rejected.
		DepthHierarchy equal(8, 8);
		for (int y = 0; y < 8; ++y)
			for (int x = 0; x < 8; ++x)
				equal.writePixel(x, y, 4.0);
		TASK_CHECK(!equal.isOccluded(0, 0, 8, 8, 4.0));
		TASK_CHECK(!equal.isOccluded(0, 0, 8, 8, 3.0));

		// A farther write must not raise a cell.
		const double before = equal.level(equal.levelCount() - 1)[0];
		equal.writePixel(4, 4, 100.0);
		TASK_CHECK(equal.level(equal.levelCount() - 1)[0] == before);
		return 0;
	}

	// Local writes must change occlusion only where the new depths apply, and a full-screen
	// query over a fully covered screen must read a single top-level cell.
	int test_hierarchy_local_update_and_fullscreen_read()
	{
		DepthHierarchy hierarchy(64, 48);
		std::uint64_t updates = 0;
		for (int y = 0; y < 48; ++y)
			for (int x = 0; x < 64; ++x)
				hierarchy.writePixel(x, y, 10.0, &updates);

		std::uint64_t reads = 0;
		TASK_CHECK(hierarchy.isOccluded(0, 0, 64, 48, 11.0, &reads));
		TASK_CHECK(reads == 1); // top level is a single cell

		// Writing a *nearer* pixel lowers the cell maximum. A farther write must not raise it,
		// so a full-screen query at 11.0 still succeeds after the write at 50.0 is ignored.
		hierarchy.writePixel(10, 10, 50.0);
		TASK_CHECK(hierarchy.depth(10, 10) == 10.0);
		TASK_CHECK(hierarchy.isOccluded(0, 0, 64, 48, 11.0));

		// A write which is nearer than 10 lowers that pixel and its ancestors. The full-screen
		// query at 11.0 must still succeed (the maximum can only go down).
		hierarchy.writePixel(10, 10, 2.0);
		TASK_CHECK(hierarchy.depth(10, 10) == 2.0);
		TASK_CHECK(hierarchy.isOccluded(0, 0, 64, 48, 11.0));

		// A query at exactly the *maximum* must not reject (equal depth is not hidden).
		TASK_CHECK(!hierarchy.isOccluded(0, 0, 64, 48, 10.0));

		// Resetting recovers the empty state.
		hierarchy.clear();
		TASK_CHECK(!hierarchy.isOccluded(0, 0, 64, 48, 1e9));
		TASK_CHECK(std::isinf(hierarchy.depth(10, 10)));
		return 0;
	}

	// Two consecutive frames must not share occlusion state: after clear(), a fresh camera's
	// renderer behaves as if nothing was drawn.
	int test_hierarchy_frame_reset()
	{
		Volume volume;
		buildSpecScene(volume);
		const RasterCamera camera = specCamera();
		const RasterResult first = renderDag(volume, camera, true);
		(void)first;
		const RasterResult second = renderDag(volume, camera, true);
		TASK_CHECK(sameImage(first, second));
		TASK_CHECK(first.stats.hizRejected == second.stats.hizRejected);

		// A different camera (below the volume, looking up) must also be independent.
		const RasterCamera other = makeCamera(31.2, 31.4, -64.0, 64, 48, 1.4, 1.0, 200.0);
		const RasterResult a = renderDag(volume, other, true);
		const RasterResult b = renderDag(volume, other, false);
		TASK_CHECK(sameImage(a, b));
		return 0;
	}
}

// ===========================================================================================
// Group: sparse
// ===========================================================================================

namespace
{
	// The same shared subtree reached at several different spatial positions must be visited
	// and rasterized at each position, not swallowed by a global visited set. Four identical
	// boxes make the DAG actually share internal nodes (baking is required to see sharing).
	int test_sparse_shared_subgraph()
	{
		Volume volume;
		const int offsets[4] = { 0, 64, 128, 192 };
		for (int offset : offsets)
			for (int z = 0; z < 16; ++z)
				for (int y = 0; y < 16; ++y)
					for (int x = 0; x < 16; ++x)
						volume.setVoxel(x + offset, y, z, 7);
		volume.bake();

		// Confirm that the baked DAG really does share: the number of internal nodes visited
		// as tree occurrences must exceed the number of distinct internal node indices.
		int occurrences = 0;
		struct Walker
		{
			const Internals::NodeStore& nodes;
			int* count;
			void walk(u32 index)
			{
				if (Internals::isMaterialNode(index)) return;
				++*count;
				for (u32 child = 0; child < 8; ++child) walk(nodes.getNodeChild(index, child));
			}
		} walker{ Internals::getNodes(volume), &occurrences };
		walker.walk(volume.rootNodeIndex());
		TASK_CHECK(occurrences > static_cast<int>(volume.countNodes()));

		// Each copy must produce a hit at its own location.
		for (int offset : offsets)
		{
			const RasterCamera camera = makeCamera(offset + 7.5, 7.5, -40, 40, 30, 0.6, 1, 200);
			const RasterResult result = renderDag(volume, camera, false);
			TASK_CHECK(result.materialAt(20, 15) == 7);
			TASK_CHECK(!std::isinf(result.depthAt(20, 15)));
		}

		// All four copies must appear in a single view too: look down from above the row of
		// boxes with up = +z and forward = +y.
		RasterCamera wide;
		wide.eye = vec3d(96.0, -150.0, -60.0);
		wide.right = vec3d(1.0, 0.0, 0.0);
		wide.up = vec3d(0.0, 0.0, 1.0);
		wide.forward = vec3d(0.0, 1.0, 0.0);
		wide.width = 240;
		wide.height = 80;
		wide.fovY = 1.2;
		wide.nearPlane = 1.0;
		wide.farPlane = 500.0;
		const RasterResult all = renderDag(volume, wide, false);
		int nonZero = 0;
		for (MaterialId m : all.material) if (m == 7) ++nonZero;
		TASK_CHECK(nonZero > 100);

		// The four boxes must not collapse onto one screen region: the material should span
		// a wide horizontal range.
		int minX = wide.width, maxX = -1;
		for (int y = 0; y < wide.height; ++y)
			for (int x = 0; x < wide.width; ++x)
				if (all.materialAt(x, y) == 7) { minX = std::min(minX, x); maxX = std::max(maxX, x); }
		TASK_CHECK(maxX - minX > 40);
		return 0;
	}

	// A large uniformly filled coarse region must be submitted as a small number of boxes,
	// not subdivided into unit voxels. leafBoxes must stay tiny relative to the voxel count.
	int test_sparse_large_coarse_leaf()
	{
		Volume volume;
		for (int z = 0; z < 64; ++z)
			for (int y = 0; y < 64; ++y)
				for (int x = 0; x < 64; ++x)
					volume.setVoxel(x, y, z, 4);
		volume.bake();

		const RasterCamera camera = makeCamera(31.5, 31.5, -200, 64, 48, 0.5, 1, 400);
		const RasterResult result = renderDag(volume, camera, false);
		TASK_CHECK(result.stats.leafBoxes <= 8);
		TASK_CHECK(result.materialAt(32, 24) == 4);

		// Occluded version does at most as much leaf work.
		const RasterResult occluded = renderDag(volume, camera, true);
		TASK_CHECK(occluded.stats.leafBoxes <= result.stats.leafBoxes);
		TASK_CHECK(sameImage(result, occluded));
		return 0;
	}

	// A pure material root: the whole volume is one material. Its leafBoxes must stay tiny
	// (never expanded to a 2^32-sided grid), and the exact hit depths must match an
	// independent single large box [-(2^31) + 0.5, 2^31 - 0.5] on each axis.
	int test_sparse_pure_material_root()
	{
		Volume volume;
		volume.fill(5);

		const RasterCamera camera = makeCamera(31.5, 31.5, -200, 48, 36, 0.7, 1, 500);
		const RasterResult result = renderDag(volume, camera, false);
		TASK_CHECK(result.stats.leafBoxes <= 8);
		TASK_CHECK(result.stats.leafBoxes >= 1);

		// Independent reference: one huge solid box covering the entire finite volume. The
		// entry depth for this camera is the front face at z = -(2^31) + 0.5.
		const double bigLower = -std::ldexp(1.0, 31) + 0.5;
		const double bigUpper = std::ldexp(1.0, 31) - 0.5;
		for (int y = 0; y < camera.height; ++y)
		{
			for (int x = 0; x < camera.width; ++x)
			{
				const vec3d d = camera.rayDirection(x, y);
				const double ox = camera.eye.x, oy = camera.eye.y, oz = camera.eye.z;
				// Slab test against the big box.
				double t0 = -std::numeric_limits<double>::infinity();
				double t1 = std::numeric_limits<double>::infinity();
				const double o[3] = { ox, oy, oz };
				const double dir[3] = { d.x, d.y, d.z };
				bool miss = false;
				for (int a = 0; a < 3 && !miss; ++a)
				{
					if (dir[a] == 0.0) { if (o[a] < bigLower || o[a] > bigUpper) miss = true; }
					else
					{
						double ta = (bigLower - o[a]) / dir[a];
						double tb = (bigUpper - o[a]) / dir[a];
						if (ta > tb) std::swap(ta, tb);
						t0 = std::max(t0, ta);
						t1 = std::min(t1, tb);
					}
				}
				double entry = std::max(t0, camera.nearPlane);
				double exit = std::min(t1, camera.farPlane);
				const bool refHit = !miss && (entry < exit);
				TASK_CHECK(refHit == !std::isinf(result.depthAt(x, y)));
				if (refHit)
				{
					TASK_CHECK(result.materialAt(x, y) == 5);
					TASK_CHECK(std::fabs(result.depthAt(x, y) - entry) <= 1e-9 * std::max(1.0, std::fabs(entry)));
				}
			}
		}
		return 0;
	}

	// The specified occlusion scene: on/off identical, and turning occlusion on does less than
	// a quarter of the leaf work while rejecting at least one subtree.
	int test_sparse_occlusion_spec()
	{
		Volume volume;
		buildSpecScene(volume);
		const RasterCamera camera = specCamera();

		const RasterResult on = renderDag(volume, camera, true);
		const RasterResult off = renderDag(volume, camera, false);

		TASK_CHECK(sameImage(on, off));
		TASK_CHECK(on.stats.leafBoxes * 4 < off.stats.leafBoxes);
		TASK_CHECK(on.stats.hizRejected > 0);
		TASK_CHECK(on.stats.hizReads > 0);
		TASK_CHECK(on.stats.hizUpdates > 0);
		TASK_CHECK(off.stats.hizReads == 0);
		TASK_CHECK(off.stats.hizRejected == 0);
		TASK_CHECK(off.stats.hizUpdates == 0);
		return 0;
	}

	// Counters must be real: nodeVisits equals the number of processed node occurrences and
	// grows when occlusion is disabled (it cannot skip subtrees), while the pixel result is
	// identical.
	int test_sparse_counters()
	{
		Volume volume;
		buildSpecScene(volume);
		const RasterCamera camera = specCamera();
		const RasterResult on = renderDag(volume, camera, true);
		const RasterResult off = renderDag(volume, camera, false);

		TASK_CHECK(on.stats.nodeVisits > 0);
		TASK_CHECK(off.stats.nodeVisits >= on.stats.nodeVisits);
		TASK_CHECK(on.stats.leafBoxes <= off.stats.leafBoxes);
		TASK_CHECK(on.stats.hizRejected <= off.stats.leafBoxes);
		TASK_CHECK(sameImage(on, off));
		return 0;
	}

	// A scene edit between two renders must be reflected immediately: no occlusion state may
	// be reused. Render, carve a hole in the nearest material, and check the newly exposed
	// background is visible and matches the disabled-occlusion path.
	int test_sparse_scene_edit()
	{
		Volume volume;
		buildSpecScene(volume);
		// A second camera which looks through where the fg surface is, so that removing a
		// chunk of the foreground reveals the backdrop.
		const RasterCamera camera = makeCamera(31.5, 31.5, -100, 64, 48, 0.5, 1, 200);

		const RasterResult before = renderDag(volume, camera, true);
		TASK_CHECK(before.materialAt(24, 24) == 4);
		const double beforeDepth = before.depthAt(24, 24);
		TASK_CHECK(beforeDepth == 99.5);

		// Carve a narrow vertical shaft through the *entire* depth of the foreground, at the
		// exact x/y band that pixel (24, 24) traverses on its way to the backdrop (the ray
		// crosses the foreground around x = 23.5, y = 31 and the backdrop around x = 17,
		// y = 30.5). Voxel y = 30..31 covers solid y in [29.5, 31.5], and x = 17..24 covers
		// x in [16.5, 24.5], so the whole ray is carved free.
		for (int z = 0; z < 64; ++z)
			for (int y = 30; y <= 31; ++y)
				for (int x = 17; x <= 24; ++x)
					volume.setVoxel(x, y, z, 0);

		const RasterResult after = renderDag(volume, camera, true);
		const RasterResult afterNoOcc = renderDag(volume, camera, false);
		TASK_CHECK(sameImage(after, afterNoOcc));
		// Now the ray goes through the shaft and hits the backdrop (material 2 or 3).
		TASK_CHECK(after.materialAt(24, 24) == 2 || after.materialAt(24, 24) == 3);
		TASK_CHECK(after.depthAt(24, 24) > beforeDepth);
		return 0;
	}

	// Read the PPM produced by examples/dag_preview.cpp: check its dimensions and that the
	// expected visible colours (wall plus both stripe materials) appear.
	int test_sparse_preview_ppm()
	{
		FILE* file = std::fopen(".task-build/dag.ppm", "rb");
		TASK_CHECK(file != nullptr);

		char magic[3] = { 0, 0, 0 };
		int width = 0;
		int height = 0;
		int maxValue = 0;
		TASK_CHECK(std::fscanf(file, "%2s %d %d %d", magic, &width, &height, &maxValue) == 4);
		TASK_CHECK(std::strcmp(magic, "P6") == 0);
		TASK_CHECK(width >= 63 && height >= 47);
		TASK_CHECK(maxValue == 255);

		// Skip exactly one whitespace byte after the header.
		std::fgetc(file);
		std::vector<unsigned char> data(static_cast<size_t>(width) * height * 3);
		const size_t readBytes = std::fread(data.data(), 1, data.size(), file);
		std::fclose(file);
		TASK_CHECK(readBytes == data.size());

		bool sawWall = false, sawStripeA = false, sawStripeB = false, sawMiss = false;
		for (size_t i = 0; i + 2 < data.size(); i += 3)
		{
			const unsigned r = data[i], g = data[i + 1], b = data[i + 2];
			// The wall is a warm colour (red dominant); the backdrop stripes are blue-dominant
			// and green-dominant; a miss is near-black.
			if (r > 120 && r > g && g > b) sawWall = true;
			if (b > g && b > r) sawStripeA = true;
			if (g > b && g > r && r < 120) sawStripeB = true;
			if (r < 20 && g < 20 && b < 25) sawMiss = true;
		}
		TASK_CHECK(sawWall);
		TASK_CHECK(sawStripeA);
		TASK_CHECK(sawStripeB);
		// The preview scene is fully covered by wall or backdrop; there must be no misses.
		TASK_CHECK(!sawMiss);
		return 0;
	}
}

// ===========================================================================================
// Registration
// ===========================================================================================

const struct task_case task_cases[] = {
	{ "coverage", "solid_box", test_coverage_solid_box },
	{ "coverage", "holes_and_negative", test_coverage_holes_and_negative },
	{ "coverage", "multi_material", test_coverage_multi_material },
	{ "coverage", "spec_scene", test_coverage_spec_scene },
	{ "coverage", "ray_formula", test_coverage_ray_formula },
	{ "coverage", "empty_root", test_coverage_empty_root },

	{ "hierarchy", "levels_odd", test_hierarchy_levels_odd },
	{ "hierarchy", "occlusion_rules", test_hierarchy_occlusion_rules },
	{ "hierarchy", "local_update_and_fullscreen_read", test_hierarchy_local_update_and_fullscreen_read },
	{ "hierarchy", "frame_reset", test_hierarchy_frame_reset },

	{ "sparse", "shared_subgraph", test_sparse_shared_subgraph },
	{ "sparse", "large_coarse_leaf", test_sparse_large_coarse_leaf },
	{ "sparse", "pure_material_root", test_sparse_pure_material_root },
	{ "sparse", "occlusion_spec", test_sparse_occlusion_spec },
	{ "sparse", "counters", test_sparse_counters },
	{ "sparse", "scene_edit", test_sparse_scene_edit },
	{ "sparse", "preview_ppm", test_sparse_preview_ppm },
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
