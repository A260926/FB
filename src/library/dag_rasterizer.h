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
#ifndef CUBIQUITY_DAG_RASTERIZER_H
#define CUBIQUITY_DAG_RASTERIZER_H

#include "base.h"
#include "depth_hierarchy.h"
#include "geometry.h"
#include "storage.h"

#include <cstdint>
#include <vector>

// A dependency-free CPU rasterizer for Cubiquity volumes, intended for fast previews on
// machines with no display and no GPU. It walks the sparse voxel DAG in the NodeStore
// directly: visible material leaves are projected and rasterized onto a depth buffer,
// and (optionally) whole subtrees which are already hidden are skipped using a
// DepthHierarchy built from the depths written so far in this frame.
//
// Deliberately *not* used here:
//   * no window, SDL, OpenGL, or GPU code of any kind;
//   * no full leaf list, no voxel array, and no extracted mesh is built;
//   * no per-pixel ray march from the root (one ray is tested only against the
//     leaves whose projected bounds could contain that pixel);
//   * no maxFootprint / average-material approximation - every hit reports the exact
//     nearest leaf depth and its material.
//
// The existing picking (traceRayRef), storage, extraction and glyph/window code is
// untouched and remains usable. This module only reads a const Volume.

namespace Cubiquity
{
	// Camera description for renderDag(). All vectors use double precision and are
	// expressed in world space. right/up/forward must form a unit, orthonormal basis
	// (right x up = forward is not required by this code, but the three vectors must be
	// mutually perpendicular and unit length). The image origin (pixel 0,0) is the
	// top-left corner.
	//
	// For pixel (x, y) the (unnormalized) sampling direction is:
	//
	//   d = forward
	//     + right * (2*(x+0.5)/width  - 1) * tan(fovY/2) * width/height
	//     + up    * (1 - 2*(y+0.5)/height) * tan(fovY/2)
	//
	// Because forward has unit length and is orthogonal to right/up, dot(d, forward)
	// is exactly 1, so the ray parameter t of a sample point eye + t*d is also its
	// forward (view-space z) depth. This is the depth reported in RasterResult, so a
	// ray which starts at 'near' reports 'near'.
	//
	// Valid input ranges (the caller is responsible for them): width/height in [1,1024],
	// fovY in (0, pi) radians, 0 < nearPlane < farPlane, and all coordinates finite.
	struct RasterCamera
	{
		vec3d eye;
		vec3d right;
		vec3d up;
		vec3d forward;

		int width = 0;
		int height = 0;

		double fovY = 0.0;     // vertical field of view, radians
		double nearPlane = 0.0;
		double farPlane = 0.0;

		// The sampling direction for pixel (x, y), exactly as documented above.
		vec3d rayDirection(int x, int y) const
		{
			const double tanHalf = std::tan(fovY * 0.5);
			const double aspect = static_cast<double>(width) / static_cast<double>(height);
			const double sx = (2.0 * (x + 0.5) / width - 1.0) * tanHalf * aspect;
			const double sy = (1.0 - 2.0 * (y + 0.5) / height) * tanHalf;
			return forward + right * sx + up * sy;
		}
	};

	// Per-frame work counters. They are incremented at the places where the work actually
	// happens, never filled in afterwards from a theoretical estimate.
	struct RasterStats
	{
		// One per node occurrence popped for processing (the root counts once; a shared
		// node reached at N different spatial positions counts N times).
		std::uint64_t nodeVisits = 0;
		// One per material leaf actually submitted to the leaf rasterizer.
		std::uint64_t leafBoxes = 0;
		// One per depth-hierarchy cell inspected by an occlusion query.
		std::uint64_t hizReads = 0;
		// One per depth-hierarchy cell actually rewritten by a pixel write.
		std::uint64_t hizUpdates = 0;
		// One per subtree (internal node or leaf) skipped because the hierarchy proved
		// every pixel of its conservative screen bound was strictly nearer.
		std::uint64_t hizRejected = 0;
	};

	// Rendered image. Both arrays are row-major, width * height entries.
	// A miss is recorded as depth = +infinity and material = 0.
	struct RasterResult
	{
		int width = 0;
		int height = 0;
		std::vector<double> depth;
		std::vector<MaterialId> material;
		RasterStats stats;

		double depthAt(int x, int y) const { return depth[static_cast<size_t>(y) * width + x]; }
		MaterialId materialAt(int x, int y) const { return material[static_cast<size_t>(y) * width + x]; }
	};

	// Rasterizes the volume from the given camera.
	//
	// The DAG walk and leaf rasterization are identical in both modes, so they always produce
	// the same pixels. The only difference is the occlusion scheme:
	//
	//   occlusion == true  - A DepthHierarchy is created empty for this frame, grown as
	//                        nearer leaves are drawn, and consulted before descending into or
	//                        rasterizing a subtree. A subtree is skipped only when every pixel
	//                        of its conservative screen bound already holds a strictly nearer
	//                        valid depth. hizReads / hizUpdates / hizRejected report that work.
	//
	//   occlusion == false - No hierarchy is built at all: no reads, writes or rejections
	//                        (hiz* counters stay zero). This is the reference/control path
	//                        used to show that rejection never changes the image and to
	//                        measure how much leaf work the hierarchy saves.
	RasterResult renderDag(const Volume& volume, const RasterCamera& camera, bool occlusion = true);
}

#endif // CUBIQUITY_DAG_RASTERIZER_H
