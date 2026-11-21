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
#include "dag_rasterizer.h"
#include "depth_hierarchy.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <utility>

namespace Cubiquity
{
	vec3d RasterCamera::pixelRay(int x, int y) const
	{
		// See the class documentation: the ray is deliberately *not* normalised, so
		// that the parameter t of a point eye + t*d equals its forward depth.
		const double tanHalfFovY = std::tan(fovY * 0.5);
		const double aspect = double(width) / double(height);
		const double sx = (2.0 * (double(x) + 0.5) / double(width) - 1.0) * tanHalfFovY * aspect;
		const double sy = (1.0 - 2.0 * (double(y) + 0.5) / double(height)) * tanHalfFovY;
		return forward + right * sx + up * sy;
	}

	namespace
	{
		// Conservative screen-space footprint of a world-space box. The pixel rectangle
		// is only ever used for conservative culling and for bounding the per-pixel
		// exact ray/box tests - never as the actual coverage.
		struct BoxCoverage
		{
			bool visible = false;
			int x0 = 0, y0 = 0, x1 = 0, y1 = 0; // Half-open conservative pixel rectangle.
			double dmin = 0.0; // Nearest depth the box could possibly write.
		};

		class Rasterizer
		{
		public:
			Rasterizer(const Volume& volume, const RasterCamera& camera, bool occlusion, DagRasterResult& result)
				: mVolume(volume)
				, mNodes(Internals::getNodes(volume))
				, mCamera(camera)
				, mOcclusion(occlusion)
				, mResult(result)
			{
				// Per-pixel ray components, matching RasterCamera::pixelRay() exactly.
				const double tanHalfFovY = std::tan(camera.fovY * 0.5);
				const double aspect = double(camera.width) / double(camera.height);
				mScreenX.reserve(camera.width);
				for (int x = 0; x < camera.width; ++x) {
					mScreenX.push_back((2.0 * (double(x) + 0.5) / double(camera.width) - 1.0) * tanHalfFovY * aspect);
				}
				mScreenY.reserve(camera.height);
				for (int y = 0; y < camera.height; ++y) {
					mScreenY.push_back((1.0 - 2.0 * (double(y) + 0.5) / double(camera.height)) * tanHalfFovY);
				}

				// The depth hierarchy starts empty every frame.
				if (mOcclusion) { mHierarchy.reset(camera.width, camera.height); }
			}

			void run()
			{
				const u32 rootNodeIndex = Internals::getRootNodeIndex(mVolume);
				if (rootNodeIndex != 0) // Material zero is empty space.
				{
					// The root has height 32 and covers [-2^31, 2^31) on each axis.
					// 64-bit positions are used throughout so that these bounds (and
					// child bounds derived from them) cannot overflow 32-bit arithmetic.
					const i64 rootPos = -(i64(1) << 31);
					visit(rootNodeIndex, rootPos, rootPos, rootPos, 32);
				}

				if (mOcclusion)
				{
					mResult.hizReads = mHierarchy.reads();
					mResult.hizUpdates = mHierarchy.updates();
				}
			}

		private:
			// Exact minimum/maximum forward depth over the box corners, computed by
			// sign selection (dotting with a corner is linear in the corner position).
			std::pair<double, double> forwardDepthRange(const vec3d& bmin, const vec3d& bmax) const
			{
				const vec3d& f = mCamera.forward;
				double lo = -dot(mCamera.eye, f);
				double hi = lo;
				for (int a = 0; a < 3; ++a)
				{
					const double mn = bmin[a] * f[a];
					const double mx = bmax[a] * f[a];
					lo += std::min(mn, mx);
					hi += std::max(mn, mx);
				}
				return std::make_pair(lo, hi);
			}

			BoxCoverage computeCoverage(const vec3d& bmin, const vec3d& bmax) const
			{
				BoxCoverage coverage;

				const std::pair<double, double> wRange = forwardDepthRange(bmin, bmax);
				const double wmin = wRange.first;
				const double wmax = wRange.second;

				// The box cannot contribute a positive-length interval in [near, far].
				if (wmax <= mCamera.near || wmin >= mCamera.far) { return coverage; }

				// Nearest depth the box could write. A tiny slack keeps the value on
				// the safe (nearer) side of any floating-point rounding, so occlusion
				// can never reject a box which would actually win a pixel.
				const double slack = 1e-12 * (1.0 + std::fabs(wmin));
				coverage.dmin = std::max(wmin - slack, mCamera.near);

				if (wmin < mCamera.near)
				{
					// The box crosses the near plane (or contains the camera), so some
					// corners are not safely projectable. Rather than dividing by a
					// non-positive w, fall back to the conservative full-screen rect.
					coverage.x0 = 0;
					coverage.y0 = 0;
					coverage.x1 = mCamera.width;
					coverage.y1 = mCamera.height;
					coverage.visible = true;
					return coverage;
				}

				// Project the eight corners (all safely in front of the near plane).
				// The projection of the convex box is the convex hull of these.
				const double tanHalfFovY = std::tan(mCamera.fovY * 0.5);
				const double aspect = double(mCamera.width) / double(mCamera.height);
				double pxMin = std::numeric_limits<double>::infinity();
				double pxMax = -std::numeric_limits<double>::infinity();
				double pyMin = std::numeric_limits<double>::infinity();
				double pyMax = -std::numeric_limits<double>::infinity();
				for (int c = 0; c < 8; ++c)
				{
					const vec3d corner(
						(c & 1) ? bmax.x : bmin.x,
						(c & 2) ? bmax.y : bmin.y,
						(c & 4) ? bmax.z : bmin.z);
					const vec3d rel = corner - mCamera.eye;
					const double w = dot(rel, mCamera.forward);
					const double u = dot(rel, mCamera.right);
					const double v = dot(rel, mCamera.up);
					const double px = ((u / w) / (tanHalfFovY * aspect) + 1.0) * 0.5 * double(mCamera.width);
					const double py = (1.0 - (v / w) / tanHalfFovY) * 0.5 * double(mCamera.height);
					pxMin = std::min(pxMin, px); pxMax = std::max(pxMax, px);
					pyMin = std::min(pyMin, py); pyMax = std::max(pyMax, py);
				}

				// Conservative half-open pixel rectangle, clamped to the image. The
				// clamping is done in floating point before truncating to int.
				const double x0 = std::max(std::floor(pxMin), 0.0);
				const double x1 = std::min(std::floor(pxMax) + 1.0, double(mCamera.width));
				const double y0 = std::max(std::floor(pyMin), 0.0);
				const double y1 = std::min(std::floor(pyMax) + 1.0, double(mCamera.height));
				if (x0 >= x1 || y0 >= y1) { return coverage; } // Entirely off-screen.

				coverage.x0 = int(x0);
				coverage.y0 = int(y0);
				coverage.x1 = int(x1);
				coverage.y1 = int(y1);
				coverage.visible = true;
				return coverage;
			}

			// Process one spatial occurrence of a DAG node. Shared nodes are processed
			// once per occurrence - there is deliberately no global 'visited' set.
			void visit(u32 nodeIndex, i64 px, i64 py, i64 pz, int nodeHeight)
			{
				mResult.nodeVisits++;

				// Solid box of this node: voxel q is the cell [q-0.5, q+0.5].
				const double size = std::ldexp(1.0, nodeHeight); // 2^nodeHeight, exact.
				const vec3d bmin(double(px) - 0.5, double(py) - 0.5, double(pz) - 0.5);
				const vec3d bmax(double(px) + size - 0.5, double(py) + size - 0.5, double(pz) + size - 0.5);

				const BoxCoverage coverage = computeCoverage(bmin, bmax);
				if (!coverage.visible) { return; }

				// Occlusion: reject the whole sub-tree only if every pixel of its
				// conservative footprint already holds a strictly nearer valid depth.
				if (mOcclusion &&
					mHierarchy.isOccluded(coverage.x0, coverage.y0, coverage.x1, coverage.y1, coverage.dmin))
				{
					mResult.hizRejected++;
					return;
				}

				if (Internals::isMaterialNode(nodeIndex))
				{
					rasteriseLeaf(bmin, bmax, static_cast<MaterialId>(nodeIndex), coverage);
					return;
				}

				// Internal node: gather the occupied children and visit them
				// near-to-far so that occluders are committed before occludees.
				struct Child { u32 index; i64 x, y, z; double dmin; };
				std::array<Child, 8> children{};
				int childCount = 0;
				const i64 childSize = i64(1) << (nodeHeight - 1);
				for (int childId = 0; childId < 8; ++childId)
				{
					const u32 childIndex = mNodes[nodeIndex][childId];
					if (childIndex == 0) { continue; } // Empty child.

					const i64 cx = px + ((childId & 1) ? childSize : 0);
					const i64 cy = py + ((childId & 2) ? childSize : 0);
					const i64 cz = pz + ((childId & 4) ? childSize : 0);

					const double cs = double(childSize);
					const vec3d cmin(double(cx) - 0.5, double(cy) - 0.5, double(cz) - 0.5);
					const vec3d cmax(double(cx) + cs - 0.5, double(cy) + cs - 0.5, double(cz) + cs - 0.5);
					const double dmin = std::max(forwardDepthRange(cmin, cmax).first, mCamera.near);

					children[childCount++] = Child{ childIndex, cx, cy, cz, dmin };
				}
				std::sort(children.begin(), children.begin() + childCount,
					[](const Child& a, const Child& b) { return a.dmin < b.dmin; });

				for (int i = 0; i < childCount; ++i)
				{
					visit(children[i].index, children[i].x, children[i].y, children[i].z, nodeHeight - 1);
				}
			}

			// Commit a material leaf: exact per-pixel ray/box tests over the
			// conservative footprint, writing depth and material on a nearest win
			// (ties go to the smaller material id).
			void rasteriseLeaf(const vec3d& bmin, const vec3d& bmax, MaterialId material, const BoxCoverage& coverage)
			{
				mResult.leafBoxes++;

				for (int y = coverage.y0; y < coverage.y1; ++y)
				{
					const double sy = mScreenY[y];
					for (int x = coverage.x0; x < coverage.x1; ++x)
					{
						const vec3d d = mCamera.forward + mCamera.right * mScreenX[x] + mCamera.up * sy;

						// Slab test against the leaf's solid box. IEEE semantics handle
						// zero direction components (infinities), as documented.
						double t0 = (bmin.x - mCamera.eye.x) / d.x;
						double t1 = (bmax.x - mCamera.eye.x) / d.x;
						if (t0 > t1) { std::swap(t0, t1); }
						double entry = t0;
						double exit = t1;

						t0 = (bmin.y - mCamera.eye.y) / d.y;
						t1 = (bmax.y - mCamera.eye.y) / d.y;
						if (t0 > t1) { std::swap(t0, t1); }
						entry = std::max(entry, t0);
						exit = std::min(exit, t1);

						t0 = (bmin.z - mCamera.eye.z) / d.z;
						t1 = (bmax.z - mCamera.eye.z) / d.z;
						if (t0 > t1) { std::swap(t0, t1); }
						entry = std::max(entry, t0);
						exit = std::min(exit, t1);

						// First positive-length interval within [near, far]. If 'near'
						// is already inside the solid the depth is exactly 'near'.
						const double lo = std::max(entry, mCamera.near);
						const double hi = std::min(exit, mCamera.far);
						if (lo < hi)
						{
							const size_t idx = static_cast<size_t>(y) * static_cast<size_t>(mCamera.width) + static_cast<size_t>(x);
							if (lo < mResult.depth[idx] ||
								(lo == mResult.depth[idx] && material < mResult.material[idx]))
							{
								mResult.depth[idx] = lo;
								mResult.material[idx] = material;
								if (mOcclusion) { mHierarchy.writePixel(x, y, lo); }
							}
						}
					}
				}
			}

		private:
			const Volume& mVolume;
			const Internals::NodeStore& mNodes;
			const RasterCamera& mCamera;
			const bool mOcclusion;
			DagRasterResult& mResult;

			DepthHierarchy mHierarchy;
			std::vector<double> mScreenX; // Per-column ray component (see pixelRay()).
			std::vector<double> mScreenY; // Per-row ray component (see pixelRay()).
		};
	}

	DagRasterResult renderDag(const Volume& volume, const RasterCamera& camera, bool occlusion)
	{
		DagRasterResult result;
		result.width = camera.width;
		result.height = camera.height;
		const size_t pixelCount = static_cast<size_t>(camera.width) * static_cast<size_t>(camera.height);
		result.depth.assign(pixelCount, std::numeric_limits<double>::infinity());
		result.material.assign(pixelCount, MaterialId(0));

		Rasterizer rasterizer(volume, camera, occlusion, result);
		rasterizer.run();
		return result;
	}
}
