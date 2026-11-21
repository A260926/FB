/***************************************************************************************************
* Cubiquity - A micro-voxel engine for games and other interactive applications                    *
*                                                                                                  *
* Written in 2026 by David Williams                                                                *
*                                                                                                  *
* To the extent possible under law, the author(s) have dedicated all copyright and related and     *
* neighboring rights to this software under the public domain worldwide. This software is          *
* distributed without any warranty.                                                                *
*                                                                                                  *
* You should have received a copy of the CC0 Public Domain Dedication along with this software.    *
* If not, see http://creativecommons.org/publicdomain/zero/1.0/.                                   *
***************************************************************************************************/
#ifndef CUBIQUITY_DAG_RASTERIZER_H
#define CUBIQUITY_DAG_RASTERIZER_H

#include "base.h"
#include "geometry.h"
#include "storage.h"

#include <vector>

namespace Cubiquity
{
	//! Pinhole camera for CPU rasterisation of the volume DAG (see renderDag()).
	/*!
		The camera is defined by a world-space position and an orthonormal basis, all as
		Vec3<double> (vec3d):

		  - eye     - the world-space camera position.
		  - right   - unit vector pointing right in image space.
		  - up      - unit vector pointing up in image space.
		  - forward - unit vector pointing into the screen.

		Together right/up/forward must form an orthonormal basis (they are used as-is).
		The image has width x height pixels (each in [1, 1024]), a vertical field of
		view fovY in radians within (0, pi), and depth clipping distances 0 < near < far.
		All coordinates and parameters must be finite. The caller guarantees these
		conditions; they are not re-validated.

		Pixel coordinates have their origin at the top-left of the image. Pixel (x, y)
		is sampled along the (unnormalised) ray

		  d = forward
		    + right   * (2*(x+0.5)/width  - 1) * tan(fovY/2) * width/height
		    + up      * (1 - 2*(y+0.5)/height) * tan(fovY/2)

		Because d is not normalised its forward component is exactly 1, so the ray
		parameter t of a point eye + t*d is precisely the forward depth of that point.
		pixelRay() evaluates the expression above.
	*/
	struct RasterCamera
	{
		vec3d eye     = vec3d(0.0, 0.0, 0.0);
		vec3d right   = vec3d(1.0, 0.0, 0.0);
		vec3d up      = vec3d(0.0, 1.0, 0.0);
		vec3d forward = vec3d(0.0, 0.0, 1.0);
		int    width  = 0;    //!< Image width in pixels, [1, 1024].
		int    height = 0;    //!< Image height in pixels, [1, 1024].
		double fovY   = 0.0;  //!< Vertical field of view in radians, (0, pi).
		double near   = 0.0;  //!< Near clipping distance (a forward depth), > 0.
		double far    = 0.0;  //!< Far clipping distance (a forward depth), > near.

		//! The unnormalised sampling ray for pixel (x, y); t equals forward depth.
		vec3d pixelRay(int x, int y) const;
	};

	//! Output of renderDag(): per-pixel depth/material plus actual work counters.
	/*!
		depth and material are row-major arrays of width*height elements. A pixel which
		hit nothing has depth +infinity and material 0. Material zero is empty space;
		every other material is treated as fully opaque.

		The counters are *actual* counts measured where the work happens (not theoretical
		estimates):

		  - nodeVisits  - DAG node occurrences processed. A node shared by several
	                      parents is visited once per spatial occurrence (there is no
	                      global 'visited' set).
		  - leafBoxes   - material leaf occurrences committed to the rasteriser.
		  - hizReads    - depth-hierarchy texel reads (occlusion queries).
		  - hizUpdates  - depth-hierarchy texel writes (pixel writes + ancestor updates).
		  - hizRejected - node occurrences skipped because the depth hierarchy proved
	                      their whole conservative footprint is already occluded.

		With occlusion disabled the hierarchy is not maintained, so hizReads, hizUpdates
		and hizRejected are all zero.
	*/
	struct DagRasterResult
	{
		int width = 0;
		int height = 0;
		std::vector<double>     depth;    //!< Forward depth per pixel, +infinity on miss.
		std::vector<MaterialId> material; //!< Material per pixel, 0 on miss.

		u64 nodeVisits  = 0;
		u64 leafBoxes   = 0;
		u64 hizReads    = 0;
		u64 hizUpdates  = 0;
		u64 hizRejected = 0;

		double     depthAt(int x, int y)    const { return depth   [static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)]; }
		MaterialId materialAt(int x, int y) const { return material[static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)]; }
	};

	//! Render the compressed voxel DAG to depth/material buffers on the CPU.
	/*!
		This is a windowless, GPU-free preview path which works directly on the NodeStore
		DAG: it never builds a leaf list, a voxel array, a mesh, or per-pixel rays from
		the root. Instead it walks the DAG front-to-back (children are visited
		near-to-far), rasterises the solid box of each material leaf at its true
		projected size (uniform coarse leaves are committed whole, never subdivided into
		unit cells), and maintains a DepthHierarchy (Hi-Z) so that sub-trees hidden
		behind already-committed geometry are skipped wholesale.

		Each integer voxel centre q is treated as the solid cell [q-0.5, q+0.5]^3, so a
		node of height h at integer position p covers the solid box
		[p-0.5, p+2^h-0.5]^3. The depth of a pixel is the start of the first
		positive-length interval of the intersection between the pixel ray and the
		non-empty material, restricted to [near, far]; if 'near' already lies inside the
		solid the depth is 'near'. Pixels whose ray meets no material within [near, far]
		are misses (depth +infinity, material 0). If two materials would hit at exactly
		the same depth the smaller material id wins.

		Projected bounding rectangles are only used for conservative culling - every
		candidate pixel is still tested against the leaf's true box, so bounding-rect
		pixels are never blindly treated as solid. Boxes crossing the near plane are not
		projected through a divide (they fall back to a conservative full-screen
		footprint). A sub-tree is rejected only when every pixel of its conservative
		footprint already holds a strictly nearer valid depth; holes and equal depths
		never cause a rejection. The hierarchy is rebuilt empty for every call, so camera
		changes and scene edits never reuse stale occlusion.

		Passing occlusion = false runs the identical DAG/leaf rasterisation path with
		only the occlusion rejection switched off (and the hierarchy not maintained),
		which is useful as a correctness/performance baseline.
	*/
	DagRasterResult renderDag(const Volume& volume, const RasterCamera& camera, bool occlusion = true);
}

#endif // CUBIQUITY_DAG_RASTERIZER_H
