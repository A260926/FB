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
#ifndef CUBIQUITY_DEPTH_HIERARCHY_H
#define CUBIQUITY_DEPTH_HIERARCHY_H

#include <cstdint>
#include <limits>
#include <vector>

// A small screen-space depth pyramid ("HiZ" / hierarchical Z buffer) used by the CPU
// DAG rasterizer (see dag_rasterizer.h) to reject whole subtrees which are hidden
// behind geometry that has already been drawn.
//
// The hierarchy is conservative: every level stores, for each cell, the *maximum*
// (i.e. farthest) of the depths of the actual pixels it covers. An empty cell holds
// +infinity, so it can never accidentally hide anything. This means a cell's value is
// never smaller than the depth of any real pixel below it, and a query can only
// conclude that a rectangle is hidden when every real pixel in it has been written
// with a strictly nearer depth.
//
// Runs of odd width/height are handled exactly: a parent cell which covers a single
// child simply copies that child's value, and no fictitious pixels are ever invented.
// All rectangles used by this class are integer, half-open, and clipped to the screen.

namespace Cubiquity
{
	class DepthHierarchy
	{
	public:
		// An 'unset' pixel (a miss) is stored as +infinity.
		static constexpr double EmptyDepth = std::numeric_limits<double>::infinity();

		// Creates an empty hierarchy for a width x height viewport. Every pixel and
		// every cell of every level starts as EmptyDepth.
		DepthHierarchy(int width, int height);

		int width() const { return mWidth; }
		int height() const { return mHeight; }

		// Number of levels. Level 0 is the full-resolution pixel grid and the last
		// level is a single cell covering the whole viewport (always at least one
		// level, even for an empty viewport).
		int levelCount() const { return static_cast<int>(mLevels.size()); }

		// Read-only view of one level. Level `level` is a row-major grid whose
		// dimensions are levelWidth(level) x levelHeight(level).
		const std::vector<double>& level(int level) const { return mLevels[static_cast<size_t>(level)].data; }
		int levelWidth(int level) const { return mLevels[static_cast<size_t>(level)].width; }
		int levelHeight(int level) const { return mLevels[static_cast<size_t>(level)].height; }

		// Depth of a single pixel at level 0, or +infinity outside the viewport.
		double depth(int x, int y) const;

		// Writes the depth of one pixel and updates all of its ancestors. The stored
		// depth is min(existing depth, depth) so that nearer geometry always wins
		// regardless of submission order. Only the O(log n) chain of cells covering
		// this pixel is touched; the rest of the hierarchy is left alone.
		// `depth` must not be NaN. Returns true if the hierarchy changed.
		// `updateCount` (optional) is incremented once for every hierarchy cell whose
		// stored value is actually rewritten (including the pixel itself).
		bool writePixel(int x, int y, double depth, std::uint64_t* updateCount = nullptr);

		// Conservative occlusion query for the integer half-open rectangle
		// [x0, x1) x [y0, y1). Returns true only if the rectangle contains at least
		// one pixel and *every* pixel in it has a valid (non-infinite) depth which is
		// strictly less than `depth`. Empty rectangles never return true, and a
		// rectangle which includes an unwritten pixel never returns true.
		//
		// The query walks the pyramid from the top: a cell wholly inside the rectangle
		// whose maximum depth is already strictly nearer means its whole region is
		// hidden; cells on the rectangle boundary are subdivided until they are either
		// wholly inside or reach pixel resolution. A full-screen query therefore reads
		// only the single top-level cell when the whole screen is hidden.
		//
		// `readCount` (optional) is incremented once per hierarchy cell inspected.
		bool isOccluded(int x0, int y0, int x1, int y1, double depth, std::uint64_t* readCount = nullptr) const;

		// Resets every level back to EmptyDepth. Called at the start of every frame;
		// no state is allowed to leak from a previous camera or scene edit.
		void clear();

	private:
		struct Level
		{
			int width = 0;
			int height = 0;
			std::vector<double> data;
		};

		static int coordToIndex(int coord, int cellSize);
		static double combine(double a, double b) { return a > b ? a : b; }

		int mWidth = 0;
		int mHeight = 0;
		std::vector<Level> mLevels;
	};
}

#endif // CUBIQUITY_DEPTH_HIERARCHY_H
