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
#include "dag_rasterizer.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace Cubiquity
{
	namespace
	{
		constexpr int RootHeight = 32; // A volume is 2^32 voxels along each axis.

		// Axis-aligned solid box in world space. A node whose lowest voxel centre is
		// (x, y, z) and which covers 2^height voxels per axis occupies the solid region
		// [x - 0.5, x + 2^height - 0.5] on each axis, because every voxel centre q stands
		// for the solid cell [q - 0.5, q + 0.5].
		struct WorldBox
		{
			double lower[3];
			double upper[3];
		};

		WorldBox makeBox(i64 x, i64 y, i64 z, int height)
		{
			const double size = std::ldexp(1.0, height); // Exact power of two.
			const i64 origin[3] = { x, y, z };
			WorldBox box;
			for (int a = 0; a < 3; ++a)
			{
				box.lower[a] = static_cast<double>(origin[a]) - 0.5;
				box.upper[a] = static_cast<double>(origin[a]) + size - 0.5;
			}
			return box;
		}

		// Conservative screen rectangle [x0, x1) x [y0, y1) produced by projecting a box.
		struct ScreenBounds
		{
			int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
			bool empty = true;
			double depthLower = 0.0; // Lower bound on any reported depth inside the box.
		};

		// Projects the box and computes a conservative (never too small) screen rectangle.
		// Returns false if the box is entirely behind the camera or entirely beyond the
		// far plane, so nothing in it can be visible.
		//
		// The projection never divides a corner which is at or behind the camera plane.
		// If any corner is not strictly in front, the whole screen is used instead, which
		// is conservative and still correct for the near-plane case where the camera is
		// inside a coarse leaf.
		bool projectBox(const WorldBox& box, const RasterCamera& camera, ScreenBounds& out)
		{
			// Forward depth of a point is dot(point - eye, forward), which is also the ray
			// parameter t for our unnormalized sampling directions.
			double minW = std::numeric_limits<double>::infinity();
			double maxW = -std::numeric_limits<double>::infinity();
			double cornerW[8];
			double cornerX[8];
			double cornerY[8];
			int index = 0;
			for (int k = 0; k < 8; ++k)
			{
				const double px = (k & 1) ? box.upper[0] : box.lower[0];
				const double py = (k & 2) ? box.upper[1] : box.lower[1];
				const double pz = (k & 4) ? box.upper[2] : box.lower[2];
				const double dx = px - camera.eye.x;
				const double dy = py - camera.eye.y;
				const double dz = pz - camera.eye.z;
				const double w = dx * camera.forward.x + dy * camera.forward.y + dz * camera.forward.z;
				cornerW[index] = w;
				cornerX[index] = dx * camera.right.x + dy * camera.right.y + dz * camera.right.z;
				cornerY[index] = dx * camera.up.x + dy * camera.up.y + dz * camera.up.z;
				minW = std::min(minW, w);
				maxW = std::max(maxW, w);
				++index;
			}

			// Entirely behind the camera, or entirely past the far plane.
			if (maxW <= 0.0) return false;
			if (minW > camera.farPlane) return false;

			out.depthLower = std::max(minW, camera.nearPlane);

			const double tanHalf = std::tan(camera.fovY * 0.5);
			const double aspect = static_cast<double>(camera.width) / static_cast<double>(camera.height);

			if (minW <= 0.0)
			{
				// At least one corner is at or behind the camera plane. Do not divide by
				// its depth; the safe conservative answer is the whole viewport.
				out.x0 = 0;
				out.y0 = 0;
				out.x1 = camera.width;
				out.y1 = camera.height;
				out.empty = false;
				return true;
			}

			double minPx = std::numeric_limits<double>::infinity();
			double maxPx = -std::numeric_limits<double>::infinity();
			double minPy = std::numeric_limits<double>::infinity();
			double maxPy = -std::numeric_limits<double>::infinity();
			for (int k = 0; k < 8; ++k)
			{
				const double w = cornerW[k];
				// sx = dot(p - eye, right) / w and sy = dot(p - eye, up) / w, matching the
				// documented sampling direction: a pixel is hit only if its continuous
				// pixel coordinate lies inside [minPx, maxPx] x [minPy, maxPy].
				const double sx = cornerX[k] / w;
				const double sy = cornerY[k] / w;
				const double px = (sx / (tanHalf * aspect) + 1.0) * camera.width * 0.5 - 0.5;
				const double py = (1.0 - sy / tanHalf) * camera.height * 0.5 - 0.5;
				minPx = std::min(minPx, px);
				maxPx = std::max(maxPx, px);
				minPy = std::min(minPy, py);
				maxPy = std::max(maxPy, py);
			}

			// Clamp to a sane range before converting, because a nearly edge-on face can
			// project to very large coordinates (the box is then simply clipped).
			const double limit = 1.0e9;
			minPx = std::max(minPx, -limit);
			maxPx = std::min(maxPx, limit);
			minPy = std::max(minPy, -limit);
			maxPy = std::min(maxPy, limit);

			// Expand by one pixel on every side so that rounding can never drop a pixel
			// which the exact test would have hit. This is used only for culling/iteration.
			const long long ix0 = static_cast<long long>(std::floor(minPx)) - 1;
			const long long ix1 = static_cast<long long>(std::floor(maxPx)) + 2;
			const long long iy0 = static_cast<long long>(std::floor(minPy)) - 1;
			const long long iy1 = static_cast<long long>(std::floor(maxPy)) + 2;

			out.x0 = static_cast<int>(std::max<long long>(ix0, 0));
			out.y0 = static_cast<int>(std::max<long long>(iy0, 0));
			out.x1 = static_cast<int>(std::min<long long>(ix1, camera.width));
			out.y1 = static_cast<int>(std::min<long long>(iy1, camera.height));
			out.empty = (out.x0 >= out.x1) || (out.y0 >= out.y1);
			return true;
		}

		// Exact ray/box test for a solid box. Returns false if the ray does not overlap the
		// box over a positive-length interval inside [near, far]. On success `depth` is the
		// forward-depth at which the ray first enters the solid part of the box (clamped to
		// the near plane, so a camera inside the box reports `near`).
		bool intersectSolidBox(const WorldBox& box, const RasterCamera& camera, const vec3d& dir, double& depth)
		{
			double t0 = -std::numeric_limits<double>::infinity();
			double t1 = std::numeric_limits<double>::infinity();

			const double o[3] = { camera.eye.x, camera.eye.y, camera.eye.z };
			const double d[3] = { dir.x, dir.y, dir.z };
			for (int a = 0; a < 3; ++a)
			{
				if (d[a] == 0.0)
				{
					// Parallel to this pair of faces: the whole ray is inside this slab or
					// outside it. (The task excludes grazing/edge degeneracies, but the
					// parallel case is cheap to handle exactly.)
					if (o[a] < box.lower[a] || o[a] > box.upper[a]) return false;
				}
				else
				{
					double ta = (box.lower[a] - o[a]) / d[a];
					double tb = (box.upper[a] - o[a]) / d[a];
					if (ta > tb) std::swap(ta, tb);
					t0 = std::max(t0, ta);
					t1 = std::min(t1, tb);
				}
			}

			const double entry = std::max(t0, camera.nearPlane);
			const double exit = std::min(t1, camera.farPlane);
			if (!(entry < exit)) return false; // No positive-length interval in [near, far].
			depth = entry;
			return true;
		}

		struct StackEntry
		{
			u32 nodeIndex;
			i64 x, y, z;
			int height;
		};

		// Squared distance from the eye to the box centre, used to visit nearer children
		// first so that the depth hierarchy becomes useful as early as possible.
		double boxCentreDistanceSquared(const WorldBox& box, const vec3d& eye)
		{
			const double cx = (box.lower[0] + box.upper[0]) * 0.5 - eye.x;
			const double cy = (box.lower[1] + box.upper[1]) * 0.5 - eye.y;
			const double cz = (box.lower[2] + box.upper[2]) * 0.5 - eye.z;
			return cx * cx + cy * cy + cz * cz;
		}
	}

	RasterResult renderDag(const Volume& volume, const RasterCamera& camera, bool occlusion)
	{
		RasterResult result;
		result.width = camera.width;
		result.height = camera.height;
		const size_t pixelCount = static_cast<size_t>(camera.width) * static_cast<size_t>(camera.height);
		result.depth.assign(pixelCount, std::numeric_limits<double>::infinity());
		result.material.assign(pixelCount, MaterialId(0));

		const Internals::NodeStore& nodes = Internals::getNodes(volume);
		const u32 rootNodeIndex = Internals::getRootNodeIndex(volume);

		// The hierarchy is created empty for every frame. It is never carried over from a
		// previous camera or scene state.
		DepthHierarchy hierarchy(camera.width, camera.height);

		std::vector<StackEntry> stack;
		stack.reserve(64);
		stack.push_back(StackEntry{ rootNodeIndex, -i64(1) << 31, -i64(1) << 31, -i64(1) << 31, RootHeight });

		StackEntry childEntries[8];

		while (!stack.empty())
		{
			const StackEntry entry = stack.back();
			stack.pop_back();

			++result.stats.nodeVisits;

			const WorldBox box = makeBox(entry.x, entry.y, entry.z, entry.height);

			ScreenBounds screen;
			if (!projectBox(box, camera, screen)) continue; // Behind camera / past far plane.

			// Try to reject the whole subtree before doing any per-pixel work. The query is
			// only allowed to reject when every pixel in the conservative screen bound has a
			// strictly nearer valid depth, which is exactly what isOccluded() checks.
			if (occlusion && !screen.empty)
			{
				std::uint64_t reads = 0;
				const bool hidden = hierarchy.isOccluded(
					screen.x0, screen.y0, screen.x1, screen.y1, screen.depthLower, &reads);
				result.stats.hizReads += reads;
				if (hidden)
				{
					++result.stats.hizRejected;
					continue;
				}
			}

			if (Internals::isMaterialNode(entry.nodeIndex))
			{
				// A node index below MaterialCount is a uniform material leaf of whatever
				// size it is reached at; it is rasterized as one solid box and is never
				// subdivided into unit voxels. Material 0 is empty and is not submitted.
				const MaterialId material = static_cast<MaterialId>(entry.nodeIndex);
				if (material == 0 || screen.empty) continue;

				++result.stats.leafBoxes;

				for (int py = screen.y0; py < screen.y1; ++py)
				{
					const size_t rowBase = static_cast<size_t>(py) * static_cast<size_t>(camera.width);
					for (int px = screen.x0; px < screen.x1; ++px)
					{
						const vec3d dir = camera.rayDirection(px, py);
						double depth = 0.0;
						if (!intersectSolidBox(box, camera, dir, depth)) continue;

						const size_t index = rowBase + static_cast<size_t>(px);
						const double existing = result.depth[index];
						// Nearest depth wins; on an exact depth tie the smaller material id wins.
						if (depth < existing || (depth == existing && material < result.material[index]))
						{
							const bool depthChanges = depth < existing;
							result.depth[index] = depth;
							result.material[index] = material;
							if (occlusion && depthChanges)
							{
								hierarchy.writePixel(px, py, depth, &result.stats.hizUpdates);
							}
						}
					}
				}
				continue;
			}

			// Internal node: split into its eight children. Positions are computed in 64-bit
			// integers so the root boundary arithmetic cannot overflow 32 bits.
			const int childHeight = entry.height - 1;
			const i64 half = i64(1) << childHeight;
			int childCount = 0;
			for (int childId = 0; childId < 8; ++childId)
			{
				const u32 childNodeIndex = nodes[entry.nodeIndex][static_cast<size_t>(childId)];
				if (childNodeIndex == 0) continue; // Empty child, skip.
				childEntries[childCount++] = StackEntry{
					childNodeIndex,
					entry.x + ((childId & 1) ? half : 0),
					entry.y + ((childId & 2) ? half : 0),
					entry.z + ((childId & 4) ? half : 0),
					childHeight
				};
			}

			// Sort descending by distance (farthest first) so that, after pushing them all,
			// the nearest child is on top of the LIFO stack and is processed first. This lets
			// front-to-back processing fill the depth hierarchy as early as possible.
			for (int i = 1; i < childCount; ++i)
			{
				StackEntry key = childEntries[i];
				const double keyDist = boxCentreDistanceSquared(makeBox(key.x, key.y, key.z, key.height), camera.eye);
				int j = i - 1;
				while (j >= 0)
				{
					const double otherDist = boxCentreDistanceSquared(
						makeBox(childEntries[j].x, childEntries[j].y, childEntries[j].z, childEntries[j].height), camera.eye);
					if (otherDist >= keyDist) break;
					childEntries[j + 1] = childEntries[j];
					--j;
				}
				childEntries[j + 1] = key;
			}

			for (int i = 0; i < childCount; ++i)
			{
				stack.push_back(childEntries[i]);
			}
		}

		return result;
	}
}
