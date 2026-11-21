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
#include "depth_hierarchy.h"

#include <algorithm>
#include <cassert>

namespace Cubiquity
{
	void DepthHierarchy::reset(int width, int height)
	{
		assert(width > 0 && height > 0);

		mWidth = width;
		mHeight = height;
		mReads = 0;
		mUpdates = 0;
		mLevels.clear();
		mLevelCount = 0;

		// Level 0 is full resolution. Each subsequent level halves the dimensions
		// (rounding up) until we reach a single texel.
		int w = width;
		int h = height;
		while (true)
		{
			mLevels.emplace_back(static_cast<size_t>(w) * static_cast<size_t>(h), EmptyDepth);
			mLevelCount++;
			if (w == 1 && h == 1) { break; }
			w = (w + 1) / 2;
			h = (h + 1) / 2;
		}
	}

	int DepthHierarchy::levelWidth(int level) const
	{
		return (mWidth + (1 << level) - 1) >> level;
	}

	int DepthHierarchy::levelHeight(int level) const
	{
		return (mHeight + (1 << level) - 1) >> level;
	}

	double DepthHierarchy::texel(int level, int x, int y) const
	{
		return mLevels[level][static_cast<size_t>(y) * static_cast<size_t>(levelWidth(level)) + static_cast<size_t>(x)];
	}

	void DepthHierarchy::writePixel(int x, int y, double depth)
	{
		assert(mLevelCount > 0);
		assert(x >= 0 && x < mWidth && y >= 0 && y < mHeight);

		mLevels[0][static_cast<size_t>(y) * static_cast<size_t>(mWidth) + static_cast<size_t>(x)] = depth;
		mUpdates++;

		// Propagate to the ancestors of this pixel only. Each parent is recomputed as
		// the exact maximum of its (up to) four children, so overwrites with a nearer
		// depth are handled correctly. Texels on odd edges have fewer children - only
		// the children which actually exist are considered (no fictitious padding).
		int cx = x;
		int cy = y;
		for (int level = 0; level + 1 < mLevelCount; ++level)
		{
			const int px = cx >> 1;
			const int py = cy >> 1;
			const int lw = levelWidth(level);
			const int lh = levelHeight(level);

			double maxDepth = std::numeric_limits<double>::lowest();
			for (int dy = 0; dy < 2; ++dy)
			{
				for (int dx = 0; dx < 2; ++dx)
				{
					const int sx = 2 * px + dx;
					const int sy = 2 * py + dy;
					if (sx < lw && sy < lh)
					{
						maxDepth = std::max(maxDepth,
							mLevels[level][static_cast<size_t>(sy) * static_cast<size_t>(lw) + static_cast<size_t>(sx)]);
					}
				}
			}

			double& slot = mLevels[level + 1][static_cast<size_t>(py) * static_cast<size_t>(levelWidth(level + 1)) + static_cast<size_t>(px)];
			if (slot == maxDepth) { break; } // Unchanged, so the remaining ancestors are too.
			slot = maxDepth;
			mUpdates++;

			cx = px;
			cy = py;
		}
	}

	bool DepthHierarchy::isOccluded(int x0, int y0, int x1, int y1, double depth) const
	{
		// Clamp the query rectangle to the image. An empty rectangle has no pixels to
		// prove anything about, so it cannot be occluded.
		x0 = std::clamp(x0, 0, mWidth);
		x1 = std::clamp(x1, 0, mWidth);
		y0 = std::clamp(y0, 0, mHeight);
		y1 = std::clamp(y1, 0, mHeight);
		if (x0 >= x1 || y0 >= y1 || mLevelCount == 0) { return false; }

		// Walk the pyramid from the top. A texel whose maximum depth is strictly less
		// than the query depth is entirely nearer and needs no further inspection. A
		// texel which is not (some pixel below it has depth >= the query depth) either
		// disproves occlusion directly (if it lies fully inside the rectangle, or is an
		// actual pixel) or is refined by descending to its children.
		struct Item { int level, tx, ty; };
		std::vector<Item> stack;
		stack.reserve(64);

		const int top = mLevelCount - 1;
		for (int ty = y0 >> top; ty <= (y1 - 1) >> top; ++ty)
		{
			for (int tx = x0 >> top; tx <= (x1 - 1) >> top; ++tx)
			{
				stack.push_back({ top, tx, ty });
			}
		}

		while (!stack.empty())
		{
			const Item item = stack.back();
			stack.pop_back();

			mReads++;
			const double maxDepth = mLevels[item.level]
				[static_cast<size_t>(item.ty) * static_cast<size_t>(levelWidth(item.level)) + static_cast<size_t>(item.tx)];

			if (maxDepth < depth) { continue; } // Everything below this texel is strictly nearer.
			if (item.level == 0) { return false; } // A real pixel inside the rectangle blocks it.

			// Pixel coverage of this texel (clamped to the real pixels on odd edges).
			const int px0 = item.tx << item.level;
			const int py0 = item.ty << item.level;
			const int px1 = std::min((item.tx + 1) << item.level, mWidth);
			const int py1 = std::min((item.ty + 1) << item.level, mHeight);
			if (px0 >= x0 && py0 >= y0 && px1 <= x1 && py1 <= y1)
			{
				return false; // Fully inside the rectangle: the blocking pixel is in range.
			}

			// Partially overlapping: descend to the children which touch the rectangle.
			const int childLevel = item.level - 1;
			const int cx0 = std::max(2 * item.tx, x0 >> childLevel);
			const int cx1 = std::min(2 * item.tx + 1, (x1 - 1) >> childLevel);
			const int cy0 = std::max(2 * item.ty, y0 >> childLevel);
			const int cy1 = std::min(2 * item.ty + 1, (y1 - 1) >> childLevel);
			for (int cy = cy0; cy <= cy1; ++cy)
			{
				for (int cx = cx0; cx <= cx1; ++cx)
				{
					stack.push_back({ childLevel, cx, cy });
				}
			}
		}

		return true;
	}
}
