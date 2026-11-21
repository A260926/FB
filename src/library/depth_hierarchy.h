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
#ifndef CUBIQUITY_DEPTH_HIERARCHY_H
#define CUBIQUITY_DEPTH_HIERARCHY_H

#include "base.h"

#include <limits>
#include <vector>

namespace Cubiquity
{
	//! A Hi-Z (depth pyramid) acceleration structure for software occlusion culling.
	/*!
		This class implements a classic hierarchical depth buffer ("Hi-Z") over a frame's
		depth values, as used by the CPU DAG rasteriser (see dag_rasterizer.h) to reject
		occluded sub-trees without visiting their contents. It is independent of the
		rasteriser though, and can be unit-tested on its own.

		Level 0 holds one entry per pixel (width x height). Each coarser level halves the
		dimensions of the previous one, rounding *up* (ceiling) so that level L has
		dimensions ceil(width / 2^L) x ceil(height / 2^L), and the last level is 1 x 1.
		Every texel stores the *maximum* depth of the real pixels it covers. When a level
		has an odd width or height, the texels on the far edge cover fewer real pixels -
		no fictitious padding pixels are ever invented, so an edge texel's value is the
		maximum of only the pixels which actually exist.

		Pixels which have never been written hold the value EmptyDepth (positive
		infinity). Because infinity is never strictly less than any query depth, holes in
		the depth buffer can never cause a region to be reported as occluded.

		Depths are written one pixel at a time with writePixel(), which updates only the
		ancestors of that pixel (never a full-screen rebuild) and keeps every level an
		exact maximum of the pixels below it, even when a pixel is overwritten with a
		nearer (smaller) depth.

		Occlusion is queried with isOccluded(), which takes an integer half-open pixel
		rectangle and returns true only if every real pixel in that rectangle already
		holds a valid depth strictly nearer than the query depth. The query genuinely
		walks the pyramid (a fully-covered full-screen query is answered with a single
		read of the top texel).

		The hierarchy is cleared (all pixels empty, counters zeroed) by reset(), which
		should be called once per frame - stale occlusion data must never be reused
		across camera changes or scene edits.

		Instrumentation: reads() counts texel reads performed by isOccluded(), and
		updates() counts texel writes performed by writePixel() (level 0 plus ancestor
		propagation). The counters are incremented at the exact places the reads/writes
		happen and are reset by reset(). The read-only accessors (texel(), levelData(),
		levelWidth(), ...) do not affect the counters.
	*/
	class DepthHierarchy
	{
	public:
		//! Depth value held by pixels which have not been written (i.e. empty).
		static constexpr double EmptyDepth = std::numeric_limits<double>::infinity();

		DepthHierarchy() = default;

		//! (Re)initialise the pyramid for a new frame. All pixels become empty and the
		//! read/update counters are zeroed. Width and height must be positive.
		void reset(int width, int height);

		int width() const { return mWidth; }
		int height() const { return mHeight; }

		//! Number of levels. Level 0 is full resolution, the last level is 1x1.
		int levelCount() const { return mLevelCount; }
		//! Width of the given level: ceil(width / 2^level).
		int levelWidth(int level) const;
		//! Height of the given level: ceil(height / 2^level).
		int levelHeight(int level) const;

		//! Read-only access to a single texel (row-major within its level).
		double texel(int level, int x, int y) const;
		//! Read-only access to a whole level (row-major, levelWidth x levelHeight).
		const std::vector<double>& levelData(int level) const { return mLevels[level]; }

		//! Write the depth of a single pixel (level 0) and locally update its ancestors.
		//! The depth should be a valid (finite) depth; unwritten pixels are EmptyDepth.
		void writePixel(int x, int y, double depth);

		//! Occlusion query over the integer half-open rectangle [x0,x1) x [y0,y1),
		//! clamped to the image. Returns true only if every real pixel in the rectangle
		//! holds a valid depth strictly less than 'depth'. Empty rectangles, holes and
		//! depths equal to 'depth' all return false (never an over-eager rejection).
		bool isOccluded(int x0, int y0, int x1, int y1, double depth) const;

		//! Number of texel reads performed by isOccluded() since the last reset().
		u64 reads() const { return mReads; }
		//! Number of texel writes performed by writePixel() since the last reset().
		u64 updates() const { return mUpdates; }

	private:
		int mWidth = 0;
		int mHeight = 0;
		int mLevelCount = 0;
		std::vector<std::vector<double>> mLevels;
		mutable u64 mReads = 0; // isOccluded() is const but logically reads the hierarchy.
		u64 mUpdates = 0;
	};
}

#endif // CUBIQUITY_DEPTH_HIERARCHY_H
