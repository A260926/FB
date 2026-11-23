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
#include "depth_hierarchy.h"

#include <algorithm>
#include <cmath>

namespace Cubiquity
{
	namespace
	{
		// Number of cells needed to cover `size` pixels with cells of `cellSize` pixels.
		int cellCount(int size, int cellSize)
		{
			return (size + cellSize - 1) / cellSize;
		}
	}

	int DepthHierarchy::coordToIndex(int coord, int cellSize)
	{
		// Careful with negatives: integer division truncates towards zero.
		if (coord < 0) return (coord - cellSize + 1) / cellSize;
		return coord / cellSize;
	}

	DepthHierarchy::DepthHierarchy(int width, int height)
		: mWidth(std::max(width, 0))
		, mHeight(std::max(height, 0))
	{
		// Build the pyramid bottom-up. Even a degenerate (empty) viewport gets one level
		// so that levelCount() is always at least one and level access is well defined.
		Level base;
		base.width = std::max(mWidth, 1);
		base.height = std::max(mHeight, 1);
		if (mWidth == 0 || mHeight == 0) base.width = base.height = 0;
		base.data.assign(static_cast<size_t>(base.width) * static_cast<size_t>(base.height), EmptyDepth);
		mLevels.push_back(std::move(base));

		while (mLevels.back().width > 1 || mLevels.back().height > 1)
		{
			const Level& child = mLevels.back();
			Level parent;
			parent.width = cellCount(child.width, 2);
			parent.height = cellCount(child.height, 2);
			parent.data.assign(static_cast<size_t>(parent.width) * static_cast<size_t>(parent.height), EmptyDepth);
			mLevels.push_back(std::move(parent));
		}

		// The single top-level cell of an empty viewport is an empty rectangle.
		if (mWidth == 0 || mHeight == 0)
		{
			mLevels.clear();
			mLevels.push_back(Level{});
		}
	}

	double DepthHierarchy::depth(int x, int y) const
	{
		if (x < 0 || y < 0 || x >= mWidth || y >= mHeight) return EmptyDepth;
		const Level& base = mLevels[0];
		return base.data[static_cast<size_t>(y) * static_cast<size_t>(base.width) + static_cast<size_t>(x)];
	}

	bool DepthHierarchy::writePixel(int x, int y, double depthIn, std::uint64_t* updateCount)
	{
		if (x < 0 || y < 0 || x >= mWidth || y >= mHeight) return false;
		if (std::isnan(depthIn)) return false;

		Level& base = mLevels[0];
		const size_t index = static_cast<size_t>(y) * static_cast<size_t>(base.width) + static_cast<size_t>(x);
		const double combined = std::min(base.data[index], depthIn);
		if (!(combined < base.data[index])) return false; // No change (also catches +inf writes)
		base.data[index] = combined;
		if (updateCount) ++*updateCount;

		// Propagate only the ancestors of this pixel. Each level's cell is the maximum
		// of its real children; we recompute it from those children rather than assuming
		// the new value dominates, because a write can only lower the maximum but we want
		// an exact (not merely conservative-by-accident) value.
		int cx = x;
		int cy = y;
		for (size_t level = 1; level < mLevels.size(); ++level)
		{
			Level& parent = mLevels[level];
			const Level& child = mLevels[level - 1];
			cx /= 2;
			cy /= 2;

			// Start from -infinity and take the maximum of the real children. A child which
			// is still a hole holds +infinity, which correctly makes the whole cell +infinity
			// (i.e. it can never be treated as hidden by a query).
			double cellMax = -std::numeric_limits<double>::infinity();
			const int childX0 = cx * 2;
			const int childY0 = cy * 2;
			const int childX1 = std::min(childX0 + 2, child.width);
			const int childY1 = std::min(childY0 + 2, child.height);
			for (int yy = childY0; yy < childY1; ++yy)
			{
				const double* row = &child.data[static_cast<size_t>(yy) * static_cast<size_t>(child.width)];
				for (int xx = childX0; xx < childX1; ++xx)
				{
					cellMax = combine(cellMax, row[xx]);
				}
			}
			if (!std::isfinite(cellMax) && cellMax < 0.0) cellMax = EmptyDepth; // No children.

			double& slot = parent.data[static_cast<size_t>(cy) * static_cast<size_t>(parent.width) + static_cast<size_t>(cx)];
			if (cellMax == slot)
			{
				// The maximum did not change, so no ancestor above this can change either.
				break;
			}
			slot = cellMax;
			if (updateCount) ++*updateCount;
		}
		return true;
	}

	bool DepthHierarchy::isOccluded(int x0, int y0, int x1, int y1, double depthIn, std::uint64_t* readCount) const
	{
		// Clip to the viewport and reject empty rectangles.
		const int qx0 = std::max(x0, 0);
		const int qy0 = std::max(y0, 0);
		const int qx1 = std::min(x1, mWidth);
		const int qy1 = std::min(y1, mHeight);
		if (qx0 >= qx1 || qy0 >= qy1) return false;
		if (std::isnan(depthIn)) return false;

		std::uint64_t reads = 0;

		// Recursive test of the cells at `level` which intersect the query rectangle.
		// Cell ranges are inclusive. Returns true only when every pixel of the query
		// rectangle in this region is hidden by a strictly nearer real depth.
		const auto test = [&](auto&& self, int level, int cx0, int cy0, int cx1, int cy1) -> bool
		{
			const Level& lvl = mLevels[static_cast<size_t>(level)];
			const int cellSpan = 1 << level;
			for (int cy = cy0; cy <= cy1; ++cy)
			{
				const double* row = &lvl.data[static_cast<size_t>(cy) * static_cast<size_t>(lvl.width)];
				const int py0 = cy * cellSpan;
				const int py1 = std::min(py0 + cellSpan, mHeight);
				if (py0 >= qy1 || py1 <= qy0) continue; // Cell is outside the query rectangle.
				const bool yInside = (py0 >= qy0) && (py1 <= qy1);
				for (int cx = cx0; cx <= cx1; ++cx)
				{
					const int px0 = cx * cellSpan;
					const int px1 = std::min(px0 + cellSpan, mWidth);
					if (px0 >= qx1 || px1 <= qx0) continue; // Outside the query rectangle.
					++reads;
					const bool whollyInside = yInside && (px0 >= qx0) && (px1 <= qx1);
					if (whollyInside)
					{
						// The stored value is the maximum over the cell's *real* pixels, so
						// requiring it to be strictly nearer proves every pixel is hidden.
						if (!(row[cx] < depthIn)) return false;
					}
					else if (level == 0)
					{
						// A boundary pixel: hidden only if it holds a strictly nearer depth.
						if (!(row[cx] < depthIn)) return false;
					}
					else
					{
						// Boundary cell at a coarse level: subdivide it. The child ranges are
						// clipped to the child level's real dimensions (which matter for odd
						// sizes, where the last parent cell may have only one child).
						const Level& child = mLevels[static_cast<size_t>(level - 1)];
						const int childCx1 = std::min(cx * 2 + 2, child.width) - 1;
						const int childCy1 = std::min(cy * 2 + 2, child.height) - 1;
						if (childCx1 < cx * 2 || childCy1 < cy * 2) continue; // No real children.
						if (!self(self, level - 1, cx * 2, cy * 2, childCx1, childCy1))
						{
							return false;
						}
					}
				}
			}
			return true;
		};

		const int top = levelCount() - 1;
		const int cellSize = 1 << top;
		// Cell ranges are inclusive, because the top level may be a single 1x1 cell while
		// the query rectangle covers the whole screen (then cx0 == cx1 == 0).
		const int cx0 = coordToIndex(qx0, cellSize);
		const int cy0 = coordToIndex(qy0, cellSize);
		const int cx1 = coordToIndex(qx1 - 1, cellSize);
		const int cy1 = coordToIndex(qy1 - 1, cellSize);
		const bool hidden = test(test, top, cx0, cy0, cx1, cy1);
		if (readCount) *readCount += reads;
		return hidden;
	}

	void DepthHierarchy::clear()
	{
		for (Level& level : mLevels)
		{
			std::fill(level.data.begin(), level.data.end(), EmptyDepth);
		}
	}
}
