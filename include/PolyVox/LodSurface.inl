/*******************************************************************************
* The MIT License (MIT)
*
* Copyright (c) 2015 David Williams and Matthew Williams
*
* Permission is hereby granted, free of charge, to any person obtaining a copy
* of this software and associated documentation files (the "Software"), to deal
* in the Software without restriction, including without limitation the rights
* to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
* copies of the Software, and to permit persons to whom the Software is
* furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in all
* copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
* FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
* COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
* IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
* CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*******************************************************************************/

#include "PolyVox/Impl/MarchingCubesTables.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace PolyVox
{
	namespace
	{
		// Corner offsets of a cube, matching the PolyVox marching-cubes bit order:
		// bit n corresponds to corner (n&1, (n>>1)&1, (n>>2)&1).
		const int kCornerOffset[8][3] =
		{
			{ 0, 0, 0 }, { 1, 0, 0 }, { 0, 1, 0 }, { 1, 1, 0 },
			{ 0, 0, 1 }, { 1, 0, 1 }, { 0, 1, 1 }, { 1, 1, 1 }
		};

		// The two corner indices joined by each of the 12 marching-cubes edges.
		// This matches the PolyVox extractor's edge numbering (its Z edge at the
		// current voxel is edge 10 joining corners 3 and 7), which differs from
		// the textbook Bourke ordering for edges 10 and 11.
		const int kEdgeCorners[12][2] =
		{
			{ 0, 1 }, { 1, 3 }, { 2, 3 }, { 0, 2 },
			{ 4, 5 }, { 5, 7 }, { 6, 7 }, { 4, 6 },
			{ 0, 4 }, { 1, 5 }, { 3, 7 }, { 2, 6 }
		};

		bool isPowerOfTwo(int v)
		{
			return v > 0 && (v & (v - 1)) == 0;
		}
	}

	////////////////////////////////////////////////////////////////////////////////
	// Construction
	////////////////////////////////////////////////////////////////////////////////

	inline LodSurface::LodSurface(const Region& samples,
		const std::vector<float>& values)
		: m_volume(samples)
		, m_sampleRegion(samples)
	{
		const size_t expected = static_cast<size_t>(samples.getWidthInVoxels())
			* static_cast<size_t>(samples.getHeightInVoxels())
			* static_cast<size_t>(samples.getDepthInVoxels());
		if (values.size() != expected)
		{
			POLYVOX_THROW(std::invalid_argument,
				"LodSurface: number of values does not match the sample region");
		}
		for (size_t i = 0; i < values.size(); ++i)
		{
			if (!std::isfinite(values[i]))
			{
				POLYVOX_THROW(std::invalid_argument,
					"LodSurface: all sample values must be finite");
			}
		}

		size_t index = 0;
		for (int32_t z = samples.getLowerZ(); z <= samples.getUpperZ(); ++z)
		{
			for (int32_t y = samples.getLowerY(); y <= samples.getUpperY(); ++y)
			{
				for (int32_t x = samples.getLowerX(); x <= samples.getUpperX(); ++x)
				{
					m_volume.setVoxel(x, y, z, values[index++]);
				}
			}
		}
		m_mesh.setOffset(Vector3DInt32(0, 0, 0));
	}

	////////////////////////////////////////////////////////////////////////////////
	// Layout validation
	////////////////////////////////////////////////////////////////////////////////

	inline void LodSurface::reject(const char* message) const
	{
		POLYVOX_THROW(std::invalid_argument, message);
	}

	inline void LodSurface::validateLayout(const std::vector<LodBlock>& blocks,
		Region& outCovered) const
	{
		if (blocks.empty()) reject("LodSurface::setLayout: layout must not be empty");

		std::set<int32_t> ids;
		int32_t minX = 0, minY = 0, minZ = 0, maxX = 0, maxY = 0, maxZ = 0;

		for (size_t i = 0; i < blocks.size(); ++i)
		{
			const LodBlock& b = blocks[i];
			if (!ids.insert(b.id).second)
				reject("LodSurface::setLayout: block ids must be unique");
			if (b.cells < 4 || (b.cells % 2) != 0)
				reject("LodSurface::setLayout: cells must be an even number >= 4");
			if (!isPowerOfTwo(b.step))
				reject("LodSurface::setLayout: step must be a positive power of two");
			for (int axis = 0; axis < 3; ++axis)
			{
				if (b.origin.getElement(axis) % b.step != 0)
					reject("LodSurface::setLayout: block origin must align to step");
			}

			const int32_t ux = b.origin.getX() + static_cast<int32_t>(b.cells) * b.step;
			const int32_t uy = b.origin.getY() + static_cast<int32_t>(b.cells) * b.step;
			const int32_t uz = b.origin.getZ() + static_cast<int32_t>(b.cells) * b.step;

			if (i == 0)
			{
				minX = b.origin.getX(); minY = b.origin.getY(); minZ = b.origin.getZ();
				maxX = ux; maxY = uy; maxZ = uz;
			}
			else
			{
				minX = (std::min)(minX, b.origin.getX());
				minY = (std::min)(minY, b.origin.getY());
				minZ = (std::min)(minZ, b.origin.getZ());
				maxX = (std::max)(maxX, ux);
				maxY = (std::max)(maxY, uy);
				maxZ = (std::max)(maxZ, uz);
			}

			// The one-cell source border must cover the block box on every side.
			if (b.origin.getX() <= m_sampleRegion.getLowerX()
				|| b.origin.getY() <= m_sampleRegion.getLowerY()
				|| b.origin.getZ() <= m_sampleRegion.getLowerZ()
				|| ux >= m_sampleRegion.getUpperX()
				|| uy >= m_sampleRegion.getUpperY()
				|| uz >= m_sampleRegion.getUpperZ())
			{
				reject("LodSurface::setLayout: blocks must leave one source sample "
					"of border around the covered box");
			}
		}

		// Exact cover of one box: sum of fine cell counts equals the box volume.
		const int64_t boxCells = static_cast<int64_t>(maxX - minX)
			* static_cast<int64_t>(maxY - minY)
			* static_cast<int64_t>(maxZ - minZ);
		int64_t sumCells = 0;
		for (size_t i = 0; i < blocks.size(); ++i)
		{
			const LodBlock& b = blocks[i];
			const int64_t fineCells = static_cast<int64_t>(b.cells) * b.step;
			sumCells += fineCells * fineCells * fineCells;
		}
		if (sumCells != boxCells)
			reject("LodSurface::setLayout: blocks must exactly cover one box "
				"without overlap");

		const int minStep = [&]()
		{
			int s = blocks.front().step;
			for (size_t i = 1; i < blocks.size(); ++i)
				s = (std::min)(s, blocks[i].step);
			return s;
		}();

		for (size_t i = 0; i < blocks.size(); ++i)
		{
			const LodBlock& b = blocks[i];
			const int32_t ends[3][2] =
			{
				{ b.origin.getX(), b.origin.getX() + b.cells * b.step },
				{ b.origin.getY(), b.origin.getY() + b.cells * b.step },
				{ b.origin.getZ(), b.origin.getZ() + b.cells * b.step }
			};
			for (int axis = 0; axis < 3; ++axis)
				for (int e = 0; e < 2; ++e)
					if (ends[axis][e] % minStep != 0)
						reject("LodSurface::setLayout: block endpoints must align "
							"to the finest step");
		}

		for (size_t i = 0; i < blocks.size(); ++i)
		{
			for (size_t j = i + 1; j < blocks.size(); ++j)
			{
				const LodBlock& a = blocks[i];
				const LodBlock& b = blocks[j];
				const int32_t ax1 = a.origin.getX() + a.cells * a.step;
				const int32_t ay1 = a.origin.getY() + a.cells * a.step;
				const int32_t az1 = a.origin.getZ() + a.cells * a.step;
				const int32_t bx1 = b.origin.getX() + b.cells * b.step;
				const int32_t by1 = b.origin.getY() + b.cells * b.step;
				const int32_t bz1 = b.origin.getZ() + b.cells * b.step;

				const bool overlapX = ax1 > b.origin.getX() && bx1 > a.origin.getX();
				const bool overlapY = ay1 > b.origin.getY() && by1 > a.origin.getY();
				const bool overlapZ = az1 > b.origin.getZ() && bz1 > a.origin.getZ();
				if (overlapX && overlapY && overlapZ)
					reject("LodSurface::setLayout: block interiors must not overlap");

				// A genuine coplanar face contact needs open-interval overlap on
				// two axes and an exact face-to-face meet (no gap) on the third.
				const bool faceX = ax1 == b.origin.getX()
					|| bx1 == a.origin.getX();
				const bool faceY = ay1 == b.origin.getY()
					|| by1 == a.origin.getY();
				const bool faceZ = az1 == b.origin.getZ()
					|| bz1 == a.origin.getZ();
				const int contactAxes = (overlapX ? 1 : 0) + (overlapY ? 1 : 0)
					+ (overlapZ ? 1 : 0);
				const bool faceContact = (faceX && overlapY && overlapZ)
					|| (faceY && overlapX && overlapZ)
					|| (faceZ && overlapX && overlapY);
				if (contactAxes == 2 && faceContact)
				{
					const int hi = (std::max)(a.step, b.step);
					const int lo = (std::min)(a.step, b.step);
					if (hi != lo && hi != 2 * lo)
						reject("LodSurface::setLayout: coplanar neighbours must use "
							"the same step or steps differing by a factor of two");
				}
			}
		}

		outCovered = Region(minX, minY, minZ, maxX, maxY, maxZ);
	}

	////////////////////////////////////////////////////////////////////////////////
	// Layout queries
	////////////////////////////////////////////////////////////////////////////////

	inline const LodSurface::BlockState* LodSurface::findBlock(int32_t id) const
	{
		std::map<int32_t, BlockState>::const_iterator it = m_blocks.find(id);
		return it == m_blocks.end() ? NULL : &it->second;
	}

	inline LodSurface::BlockState* LodSurface::findBlock(int32_t id)
	{
		std::map<int32_t, BlockState>::iterator it = m_blocks.find(id);
		return it == m_blocks.end() ? NULL : &it->second;
	}

	inline int LodSurface::blockStepAt(const Vector3DInt32& p,
		const std::map<int32_t, BlockState>& blocks) const
	{
		for (std::map<int32_t, BlockState>::const_iterator it = blocks.begin();
			it != blocks.end(); ++it)
		{
			const LodBlock& b = it->second.desc;
			const int32_t x1 = b.origin.getX() + b.cells * b.step;
			const int32_t y1 = b.origin.getY() + b.cells * b.step;
			const int32_t z1 = b.origin.getZ() + b.cells * b.step;
			if (p.getX() >= b.origin.getX() && p.getX() < x1
				&& p.getY() >= b.origin.getY() && p.getY() < y1
				&& p.getZ() >= b.origin.getZ() && p.getZ() < z1)
			{
				return b.step;
			}
		}
		return 0;
	}

	inline uint8_t LodSurface::interfaceFaceMask(const BlockState& bs,
		const Vector3DInt32& cell,
		const std::map<int32_t, BlockState>& blocks) const
	{
		const LodBlock& b = bs.desc;
		const Vector3DInt32 hi = cell + Vector3DInt32(b.step, b.step, b.step);
		uint8_t mask = 0;
		for (int axis = 0; axis < 3; ++axis)
		{
			for (int side = 0; side < 2; ++side)
			{
				const int32_t face = side == 0
					? cell.getElement(axis)
					: hi.getElement(axis);
				const int32_t bound = side == 0
					? b.origin.getElement(axis)
					: b.origin.getElement(axis) + b.cells * b.step;
				if (face != bound) continue; // internal face: same block both sides

				Vector3DInt32 probe = cell
					+ Vector3DInt32(b.step / 2, b.step / 2, b.step / 2);
				probe.setElement(axis, face + (side == 0 ? -1 : 1));
				const int neighbourStep = blockStepAt(probe, blocks);
				if (neighbourStep > 0 && neighbourStep < b.step)
					mask |= static_cast<uint8_t>(1u << (axis * 2 + side));
			}
		}
		return mask;
	}

	////////////////////////////////////////////////////////////////////////////////
	// setLayout
	////////////////////////////////////////////////////////////////////////////////

	inline void LodSurface::setLayout(const std::vector<LodBlock>& blocks)
	{
		Region covered;
		validateLayout(blocks, covered); // throws before any state change

		std::map<int32_t, BlockState> newBlocks;
		for (size_t i = 0; i < blocks.size(); ++i)
		{
			BlockState bs;
			bs.desc = blocks[i];
			bs.revision = 0;
			const BlockState* old = findBlock(blocks[i].id);
			if (old && old->desc.origin == blocks[i].origin
				&& old->desc.cells == blocks[i].cells
				&& old->desc.step == blocks[i].step)
			{
				// Unchanged block keeps its revision and transition pattern.
				bs.revision = old->revision;
				bs.renderCells = old->renderCells;
			}
			newBlocks.insert(std::make_pair(blocks[i].id, bs));
		}

		std::set<int32_t> changed;
		for (std::map<int32_t, BlockState>::const_iterator it = newBlocks.begin();
			it != newBlocks.end(); ++it)
		{
			const BlockState* old = findBlock(it->first);
			if (old == NULL || old->desc.origin != it->second.desc.origin
				|| old->desc.cells != it->second.desc.cells
				|| old->desc.step != it->second.desc.step)
			{
				changed.insert(it->first);
			}
		}

		const bool firstLayout = m_blocks.empty();

		if (!firstLayout && changed.empty())
		{
			// Identical content (possibly reordered): zero rebuild.
			m_blocks = newBlocks;
			m_stats.rebuiltBlocks = 0;
			m_stats.sourceReads = 0;
			return;
		}

		// Global transition closure. The geometric seed scan reads no source;
		// source reads happen only on the surface tests and extraction.
		uint64_t reads = 0;
		ExtractionCaches closureCaches;
		std::set<int32_t> renderOwners;
		const RenderCellMap renderCells = buildRenderCells(
			newBlocks, renderOwners, closureCaches, reads);

		// A block must be re-extracted when its description changed or when its
		// transition pattern changed. Everything else is provably unaffected and
		// keeps its geometry, revision and source data.
		std::set<int32_t> rebuildSet = changed;
		for (std::map<int32_t, BlockState>::iterator it = newBlocks.begin();
			it != newBlocks.end(); ++it)
		{
			const std::map<Vector3DInt32, int, LodSurfaceDetail::Vector3DInt32Less> now =
				renderCellsOf(it->first, renderCells);
			const BlockState* old = findBlock(it->first);
			if (old == NULL || now != old->renderCells)
				rebuildSet.insert(it->first);
			it->second.renderCells = now;
		}
		if (firstLayout)
		{
			rebuildSet.clear();
			for (std::map<int32_t, BlockState>::const_iterator it = newBlocks.begin();
				it != newBlocks.end(); ++it)
				rebuildSet.insert(it->first);
		}

		std::map<int32_t, BlockGeometry> extracted = m_geometry;
		for (std::set<int32_t>::const_iterator it = rebuildSet.begin();
			it != rebuildSet.end(); ++it)
		{
			BlockGeometry geom;
			extractBlock(newBlocks[*it], renderCells, geom, closureCaches, reads);
			extracted[*it] = geom;
		}
		for (std::map<int32_t, BlockGeometry>::iterator it = extracted.begin();
			it != extracted.end();)
		{
			if (newBlocks.count(it->first) == 0)
				extracted.erase(it++);
			else
				++it;
		}

		std::map<int32_t, BlockState> published = newBlocks;
		for (std::set<int32_t>::const_iterator it = rebuildSet.begin();
			it != rebuildSet.end(); ++it)
		{
			published[*it].revision = m_blocks.count(*it)
				? m_blocks[*it].revision + 1 : 1;
		}

		publish(extracted, rebuildSet, reads);
		m_blocks = published;
	}

	////////////////////////////////////////////////////////////////////////////////
	// Sample edits
	////////////////////////////////////////////////////////////////////////////////

	inline void LodSurface::updateSamples(const std::vector<SampleEdit>& edits)
	{
		if (edits.empty())
		{
			// Zero rebuild: no block is re-extracted and no source is read.
			m_stats.rebuiltBlocks = 0;
			m_stats.sourceReads = 0;
			return;
		}

		for (size_t i = 0; i < edits.size(); ++i)
		{
			const Vector3DInt32& p = edits[i].position;
			if (!m_sampleRegion.containsPoint(p))
				POLYVOX_THROW(std::invalid_argument,
					"LodSurface::updateSamples: edit position outside the sample region");
			if (!std::isfinite(edits[i].value))
				POLYVOX_THROW(std::invalid_argument,
					"LodSurface::updateSamples: edit values must be finite");
		}

		// Collapse duplicates: the last edit at a given position wins.
		std::map<Vector3DInt32, float, LodSurfaceDetail::Vector3DInt32Less> collapsed;
		for (size_t i = 0; i < edits.size(); ++i)
			collapsed[edits[i].position] = edits[i].value;

		// Apply first so extraction sees the new field; remember old values to
		// roll back on failure and keep the old resource completely intact.
		std::vector<std::pair<Vector3DInt32, float> > oldValues;
		oldValues.reserve(collapsed.size());
		for (std::map<Vector3DInt32, float, LodSurfaceDetail::Vector3DInt32Less>::iterator it = collapsed.begin();
			it != collapsed.end(); ++it)
		{
			oldValues.push_back(std::make_pair(it->first,
				m_volume.getVoxel(it->first)));
			m_volume.setVoxel(it->first, it->second);
		}

		try
		{
			std::set<int32_t> dirty;
			for (std::map<int32_t, BlockState>::const_iterator it = m_blocks.begin();
				it != m_blocks.end(); ++it)
			{
				const LodBlock& b = it->second.desc;
				const int32_t x1 = b.origin.getX() + b.cells * b.step;
				const int32_t y1 = b.origin.getY() + b.cells * b.step;
				const int32_t z1 = b.origin.getZ() + b.cells * b.step;
				for (std::map<Vector3DInt32, float, LodSurfaceDetail::Vector3DInt32Less>::const_iterator e = collapsed.begin();
					e != collapsed.end(); ++e)
				{
					const Vector3DInt32& p = e->first;
					if (p.getX() >= b.origin.getX() - 1 && p.getX() <= x1 + 1
						&& p.getY() >= b.origin.getY() - 1 && p.getY() <= y1 + 1
						&& p.getZ() >= b.origin.getZ() - 1 && p.getZ() <= z1 + 1)
					{
						dirty.insert(b.id);
						break;
					}
				}
			}

			if (dirty.empty())
			{
				// No extracted block reads these samples; the source update still
				// takes effect, no mesh rebuild happens.
				m_stats.rebuiltBlocks = 0;
				m_stats.sourceReads = 0;
				return;
			}

			// Rebuild the transition closure on the edited field. It only follows
			// surface-carrying faces, so unrelated distant blocks are never read.
			uint64_t reads = 0;
			ExtractionCaches closureCaches;
			std::set<int32_t> renderOwners;
			const RenderCellMap renderCells = buildRenderCells(
				m_blocks, renderOwners, closureCaches, reads);

			// A block must be re-extracted when it reads an edited sample, or
			// when its transition pattern moved with the surface.
			std::set<int32_t> rebuildSet = dirty;
			std::map<int32_t, BlockState> published = m_blocks;
			for (std::map<int32_t, BlockState>::iterator it = published.begin();
				it != published.end(); ++it)
			{
				const std::map<Vector3DInt32, int, LodSurfaceDetail::Vector3DInt32Less> now =
					renderCellsOf(it->first, renderCells);
				if (now != it->second.renderCells)
					rebuildSet.insert(it->first);
				it->second.renderCells = now;
			}

			std::map<int32_t, BlockGeometry> extracted = m_geometry;
			for (std::set<int32_t>::const_iterator it = rebuildSet.begin();
				it != rebuildSet.end(); ++it)
			{
				BlockGeometry geom;
				extractBlock(m_blocks[*it], renderCells, geom,
					closureCaches, reads);
				extracted[*it] = geom;
			}

			for (std::set<int32_t>::const_iterator it = rebuildSet.begin();
				it != rebuildSet.end(); ++it)
			{
				published[*it].revision = m_blocks[*it].revision + 1;
			}

			publish(extracted, rebuildSet, reads);
			m_blocks = published;
		}
		catch (...)
		{
			for (size_t i = 0; i < oldValues.size(); ++i)
				m_volume.setVoxel(oldValues[i].first, oldValues[i].second);
			throw;
		}
	}

	////////////////////////////////////////////////////////////////////////////////
	// Field access and shared vertex construction
	////////////////////////////////////////////////////////////////////////////////

	inline float LodSurface::sampleDensity(const Vector3DInt32& p,
		ExtractionCaches& caches, uint64_t& readCounter) const
	{
		std::map<Vector3DInt32, float, LodSurfaceDetail::Vector3DInt32Less>::iterator it
			= caches.density.find(p);
		if (it != caches.density.end()) return it->second;
		++readCounter; // each cache miss is one actual source density read
		const float v = m_volume.getVoxel(p);
		if (!std::isfinite(v))
			POLYVOX_THROW(std::invalid_argument,
				"LodSurface: non-finite source density encountered");
		caches.density.insert(std::make_pair(p, v));
		return v;
	}

	inline Vector3DFloat LodSurface::sampleGradient(const Vector3DInt32& p,
		ExtractionCaches& caches, uint64_t& readCounter) const
	{
		std::map<Vector3DInt32, Vector3DFloat, LodSurfaceDetail::Vector3DInt32Less>::iterator it
			= caches.gradient.find(p);
		if (it != caches.gradient.end()) return it->second;

		// Central differences on the finest source grid, independent of the
		// current block spacing. The one-cell source border keeps these in range.
		const float nx = sampleDensity(Vector3DInt32(p.getX() - 1, p.getY(), p.getZ()), caches, readCounter);
		const float px = sampleDensity(Vector3DInt32(p.getX() + 1, p.getY(), p.getZ()), caches, readCounter);
		const float ny = sampleDensity(Vector3DInt32(p.getX(), p.getY() - 1, p.getZ()), caches, readCounter);
		const float py = sampleDensity(Vector3DInt32(p.getX(), p.getY() + 1, p.getZ()), caches, readCounter);
		const float nz = sampleDensity(Vector3DInt32(p.getX(), p.getY(), p.getZ() - 1), caches, readCounter);
		const float pz = sampleDensity(Vector3DInt32(p.getX(), p.getY(), p.getZ() + 1), caches, readCounter);
		const Vector3DFloat g(nx - px, ny - py, nz - pz);
		caches.gradient.insert(std::make_pair(p, g));
		return g;
	}

	inline Vector3DFloat LodSurface::safeNormalise(const Vector3DFloat& v) const
	{
		const float lenSq = v.lengthSquared();
		if (lenSq > 1.0e-12f)
		{
			Vector3DFloat n = v;
			n *= 1.0f / std::sqrt(lenSq);
			return n;
		}
		return Vector3DFloat(0.0f, 0.0f, 0.0f);
	}

	inline uint32_t LodSurface::vertexOnEdge(const Vector3DInt32& a,
		const Vector3DInt32& b, BlockGeometry& out, ExtractionCaches& caches,
		uint64_t& readCounter) const
	{
		// Canonical key so an edge is tessellated once however it is traversed.
		const LodSurfaceDetail::Vector3DInt32Less less;
		const std::pair<Vector3DInt32, Vector3DInt32> key = less(a, b)
			? std::make_pair(a, b) : std::make_pair(b, a);
		std::map<std::pair<Vector3DInt32, Vector3DInt32>, uint32_t,
			LodSurfaceDetail::EdgeKeyLess>::iterator cached = caches.edgeVertex.find(key);
		if (cached != caches.edgeVertex.end()) return cached->second;

		const Vector3DInt32& lo = key.first;
		const Vector3DInt32& hi = key.second;
		const float va = sampleDensity(lo, caches, readCounter);
		const float vb = sampleDensity(hi, caches, readCounter);
		if (va == 0.0f || vb == 0.0f)
			POLYVOX_THROW(std::invalid_argument, "LodSurface: the isovalue passes "
				"through a sample point; resource unchanged");
		if (va == vb)
			POLYVOX_THROW(std::invalid_argument, "LodSurface: zero-area edge "
				"(equal endpoint densities); resource unchanged");

		// Same interpolation convention as the marching-cubes extractor.
		float t = (0.0f - va) / (vb - va);
		if (t < 0.0f) t = 0.0f;
		if (t > 1.0f) t = 1.0f;

		LocalVertex lv;
		lv.pos = Vector3DFloat(
			static_cast<float>(lo.getX()) + (static_cast<float>(hi.getX()) - lo.getX()) * t,
			static_cast<float>(lo.getY()) + (static_cast<float>(hi.getY()) - lo.getY()) * t,
			static_cast<float>(lo.getZ()) + (static_cast<float>(hi.getZ()) - lo.getZ()) * t);

		// Normal: interpolate finest-grid central-difference gradients, then
		// unitise. Identical inputs give bit-identical results, so vertices
		// shared across blocks carry one common finite normal.
		const Vector3DFloat ga = safeNormalise(sampleGradient(lo, caches, readCounter));
		const Vector3DFloat gb = safeNormalise(sampleGradient(hi, caches, readCounter));
		lv.normal = safeNormalise(ga * (1.0f - t) + gb * t);
		lv.data = 0.0f;
		out.vertices.push_back(lv);
		const uint32_t idx = static_cast<uint32_t>(out.vertices.size() - 1);
		caches.edgeVertex.insert(std::make_pair(key, idx));
		return idx;
	}

	////////////////////////////////////////////////////////////////////////////////
	// Regular marching-cubes cell (identical corner/edge/triangle convention to
	// the existing extractor; step is the grid spacing of the cell)
	////////////////////////////////////////////////////////////////////////////////

	inline void LodSurface::extractRegularCell(const Vector3DInt32& cell, int step,
		BlockGeometry& out, ExtractionCaches& caches, uint64_t& readCounter)
	{
		uint8_t cellIndex = 0;
		bool sign[8];
		for (int c = 0; c < 8; ++c)
		{
			const Vector3DInt32 p(
				cell.getX() + kCornerOffset[c][0] * step,
				cell.getY() + kCornerOffset[c][1] * step,
				cell.getZ() + kCornerOffset[c][2] * step);
			sign[c] = sampleDensity(p, caches, readCounter) < 0.0f;
			if (sign[c]) cellIndex |= static_cast<uint8_t>(1u << c);
		}

		// Reject the ambiguous (alternating-sign) face cases the specification
		// does not require us to resolve. Face corner index pairs:
		const int faceCorners[6][4] =
		{
			{ 0, 1, 2, 3 }, { 4, 5, 6, 7 },
			{ 0, 1, 4, 5 }, { 2, 3, 6, 7 },
			{ 0, 2, 4, 6 }, { 1, 3, 5, 7 }
		};
		for (int f = 0; f < 6; ++f)
		{
			const bool a = sign[faceCorners[f][0]];
			const bool b = sign[faceCorners[f][1]];
			const bool c = sign[faceCorners[f][2]];
			const bool d = sign[faceCorners[f][3]];
			if ((a != b || c != d) && (a != c || b != d) && a == d && b == c)
				POLYVOX_THROW(std::invalid_argument, "LodSurface: alternating-sign "
					"(ambiguous) cell face; resource unchanged");
		}

		const uint16_t edgeMask = edgeTable[cellIndex];
		if (edgeMask == 0) return;

		uint32_t edgeVertex[12];
		for (int e = 0; e < 12; ++e)
		{
			if ((edgeMask & (1u << e)) == 0) continue;
			const int c0 = kEdgeCorners[e][0];
			const int c1 = kEdgeCorners[e][1];
			const Vector3DInt32 a(
				cell.getX() + kCornerOffset[c0][0] * step,
				cell.getY() + kCornerOffset[c0][1] * step,
				cell.getZ() + kCornerOffset[c0][2] * step);
			const Vector3DInt32 b(
				cell.getX() + kCornerOffset[c1][0] * step,
				cell.getY() + kCornerOffset[c1][1] * step,
				cell.getZ() + kCornerOffset[c1][2] * step);
			edgeVertex[e] = vertexOnEdge(a, b, out, caches, readCounter);
		}

		for (int i = 0; triTable[cellIndex][i] != -1; i += 3)
		{
			out.indices.push_back(edgeVertex[triTable[cellIndex][i]]);
			out.indices.push_back(edgeVertex[triTable[cellIndex][i + 1]]);
			out.indices.push_back(edgeVertex[triTable[cellIndex][i + 2]]);
		}
	}

	////////////////////////////////////////////////////////////////////////////////
	// Transition machinery: per-cell render steps with fixed-point red-green
	// relaxation.
	//
	// Every block cell is normally rendered at its native step. A cell touching a
	// finer neighbour is subdivided (its render step is halved). An iterative
	// relaxation then enforces that, wherever the isosurface crosses a cell face,
	// both sides use the same render step: subdivision can cascade through
	// several levels (e.g. step 4 -> 2 -> 1) and across block boundaries, but it
	// only follows faces the surface actually crosses, so blocks far from the
	// surface are never visited and never contribute a source read. All render
	// steps are integer powers of two down to one, hence every produced node is a
	// real source grid point (no averaged density is ever introduced).
	////////////////////////////////////////////////////////////////////////////////

	inline int32_t LodSurface::findOwnerId(const Vector3DInt32& cellOrigin,
		int step, const std::map<int32_t, BlockState>& blocks) const
	{
		for (std::map<int32_t, BlockState>::const_iterator it = blocks.begin();
			it != blocks.end(); ++it)
		{
			const LodBlock& b = it->second.desc;
			if (b.step != step) continue;
			const int32_t x1 = b.origin.getX() + (b.cells - 1) * b.step;
			const int32_t y1 = b.origin.getY() + (b.cells - 1) * b.step;
			const int32_t z1 = b.origin.getZ() + (b.cells - 1) * b.step;
			if (cellOrigin.getX() >= b.origin.getX() && cellOrigin.getX() <= x1
				&& cellOrigin.getY() >= b.origin.getY() && cellOrigin.getY() <= y1
				&& cellOrigin.getZ() >= b.origin.getZ() && cellOrigin.getZ() <= z1
				&& (cellOrigin.getX() - b.origin.getX()) % b.step == 0
				&& (cellOrigin.getY() - b.origin.getY()) % b.step == 0
				&& (cellOrigin.getZ() - b.origin.getZ()) % b.step == 0)
			{
				return it->first;
			}
		}
		return -1;
	}

	inline std::map<Vector3DInt32, int, LodSurfaceDetail::Vector3DInt32Less>
		LodSurface::renderCellsOf(int32_t id, const RenderCellMap& renderCells) const
	{
		std::map<Vector3DInt32, int, LodSurfaceDetail::Vector3DInt32Less> result;
		for (RenderCellMap::const_iterator it = renderCells.begin();
			it != renderCells.end(); ++it)
		{
			if (it->first.first == id)
				result.insert(std::make_pair(it->first.second, it->second));
		}
		return result;
	}

	// Tests whether the isosurface crosses one face of a native cell. The face
	// is sampled on the grid of the finer side (sampleStep divides the cell
	// step); mixed corner signs mean a crossing.
	inline bool LodSurface::faceHasSurface(const Vector3DInt32& cell, int cellStep,
		int sampleStep, int axis, int side,
		ExtractionCaches& caches, uint64_t& readCounter) const
	{
		const int32_t fixed = cell.getElement(axis) + (side == 1 ? cellStep : 0);
		const int ticks = cellStep / sampleStep;
		bool first = true, refSign = false, mixed = false;
		for (int u = 0; u <= ticks; ++u)
		for (int v = 0; v <= ticks; ++v)
		{
			int comp[3] = { 0, 0, 0 };
			comp[axis] = fixed;
			comp[(axis + 1) % 3] = cell.getElement((axis + 1) % 3) + u * sampleStep;
			comp[(axis + 2) % 3] = cell.getElement((axis + 2) % 3) + v * sampleStep;
			const bool s = sampleDensity(
				Vector3DInt32(comp[0], comp[1], comp[2]),
				caches, readCounter) < 0.0f;
			if (first) { refSign = s; first = false; }
			else if (s != refSign) { mixed = true; }
		}
		return mixed;
	}

	// Native cells immediately across one face of @c cell. The validated
	// neighbour step ratio is at most two, so the other side is covered either
	// by one same-step cell, four half-step cells, or one double-step cell
	// (in which case this cell is one of four sharing that coarse cell's face).
	inline void LodSurface::faceNeighbourCells(const LodBlock& bs,
		const Vector3DInt32& cell, int axis, int side,
		const std::map<int32_t, BlockState>& blocks,
		std::vector<std::pair<int32_t, Vector3DInt32> >& out) const
	{
		const int s0 = bs.step;
		Vector3DInt32 same = cell;
		same.setElement(axis, cell.getElement(axis) + (side == 1 ? s0 : -s0));
		const int32_t sameOwner = findOwnerId(same, s0, blocks);
		if (sameOwner >= 0)
		{
			out.push_back(std::make_pair(sameOwner, same));
			return;
		}

		const int half = s0 / 2;
		bool foundFine = false;
		for (int u = 0; u < 2; ++u)
		for (int v = 0; v < 2; ++v)
		{
			int comp[3];
			for (int k = 0; k < 3; ++k) comp[k] = cell.getElement(k);
			comp[axis] = cell.getElement(axis) + (side == 1 ? s0 : -half);
			comp[(axis + 1) % 3] += (u == 1 ? half : 0);
			comp[(axis + 2) % 3] += (v == 1 ? half : 0);
			const Vector3DInt32 origin(comp[0], comp[1], comp[2]);
			const int32_t owner = findOwnerId(origin, half, blocks);
			if (owner >= 0)
			{
				out.push_back(std::make_pair(owner, origin));
				foundFine = true;
			}
		}
		if (foundFine) return;

		// Coarser neighbour: find the double-step cell whose face overlaps this
		// cell's face. Its origin on the face axis is the boundary plane; on the
		// cross axes it is the s0-aligned multiple containing this cell.
		const int dbl = s0 * 2;
		int coarse[3];
		for (int k = 0; k < 3; ++k) coarse[k] = cell.getElement(k);
		coarse[axis] = cell.getElement(axis) + (side == 1 ? s0 : -dbl);
		const int crossA = (axis + 1) % 3;
		const int crossB = (axis + 2) % 3;
		coarse[crossA] = cell.getElement(crossA) - (cell.getElement(crossA) % dbl);
		coarse[crossB] = cell.getElement(crossB) - (cell.getElement(crossB) % dbl);
		const Vector3DInt32 coarseOrigin(coarse[0], coarse[1], coarse[2]);
		const int32_t coarseOwner = findOwnerId(coarseOrigin, dbl, blocks);
		if (coarseOwner >= 0)
			out.push_back(std::make_pair(coarseOwner, coarseOrigin));
	}

	inline LodSurface::RenderCellMap LodSurface::buildRenderCells(
		const std::map<int32_t, BlockState>& blocks,
		std::set<int32_t>& outOwners,
		ExtractionCaches& caches, uint64_t& readCounter) const
	{
		RenderCellMap render;
		std::set<std::pair<int32_t, Vector3DInt32>,
			LodSurfaceDetail::CellKeyLess> seen;
		std::vector<std::pair<int32_t, Vector3DInt32> > queue;

		// Interface seeds: every cell that touches a finer neighbour, even if it
		// is already on the finest grid (it must still propagate relaxation to
		// its interior neighbours at multi-block junctions).
		for (std::map<int32_t, BlockState>::const_iterator it = blocks.begin();
			it != blocks.end(); ++it)
		{
			const BlockState& bs = it->second;
			const LodBlock& b = bs.desc;
			for (int cz = 0; cz < b.cells; ++cz)
			for (int cy = 0; cy < b.cells; ++cy)
			for (int cx = 0; cx < b.cells; ++cx)
			{
				const Vector3DInt32 cell(
					b.origin.getX() + cx * b.step,
					b.origin.getY() + cy * b.step,
					b.origin.getZ() + cz * b.step);
				if (interfaceFaceMask(bs, cell, blocks) == 0) continue;

				const std::pair<int32_t, Vector3DInt32> key =
					std::make_pair(b.id, cell);
				if (seen.insert(key).second)
				{
					queue.push_back(key);
					outOwners.insert(b.id);
				}
			}
		}

		// Fixed-point relaxation towards matching render steps across every
		// surface-carrying face.
		for (size_t qi = 0; qi < queue.size(); ++qi)
		{
			const std::pair<int32_t, Vector3DInt32> key = queue[qi];
			const BlockState& bs = blocks.find(key.first)->second;
			const LodBlock& b = bs.desc;
			const Vector3DInt32& cell = key.second;
			const RenderCellMap::const_iterator existing = render.find(key);
			const int currentStep = (existing == render.end()) ? b.step : existing->second;
			bool subdivided = false;

			for (int d = 0; d < 6; ++d)
			{
				const int axis = d / 2;
				const int side = (d % 2 == 0) ? 1 : 0; // +axis is high face
				std::vector<std::pair<int32_t, Vector3DInt32> > neighbours;
				faceNeighbourCells(b, cell, axis, side, blocks, neighbours);
				if (neighbours.empty()) continue;

				// Reachability: native neighbours are enqueued so the relaxation
				// can walk along the surface even through cells that do not
				// themselves subdivide.
				for (size_t k = 0; k < neighbours.size(); ++k)
				{
					if (seen.insert(neighbours[k]).second)
						queue.push_back(neighbours[k]);
				}

				int finestAcross = b.step;
				for (size_t k = 0; k < neighbours.size(); ++k)
				{
					const RenderCellMap::const_iterator rt =
						render.find(neighbours[k]);
					const int neighbourStep = rt == render.end()
						? blocks.find(neighbours[k].first)->second.desc.step
						: rt->second;
					finestAcross = (std::min)(finestAcross, neighbourStep);
				}

				if (finestAcross < currentStep
					&& faceHasSurface(cell, b.step, finestAcross, axis, side,
						caches, readCounter))
				{
					render[key] = finestAcross;
					subdivided = true;
					outOwners.insert(key.first);
				}
			}

			if (subdivided)
			{
				// Re-queue this cell and every native face neighbour so the
				// subdivision cascades outward to a fixed point. Each pass
				// strictly decreases some render step (never below one), so the
				// loop terminates.
				queue.push_back(key);
				for (int d = 0; d < 6; ++d)
				{
					const int axis = d / 2;
					const int side = (d % 2 == 0) ? 1 : 0;
					std::vector<std::pair<int32_t, Vector3DInt32> > neighbours;
					faceNeighbourCells(b, cell, axis, side, blocks, neighbours);
					for (size_t k = 0; k < neighbours.size(); ++k)
					{
						queue.push_back(neighbours[k]);
						outOwners.insert(neighbours[k].first);
					}
				}
			}
		}
		return render;
	}

	////////////////////////////////////////////////////////////////////////////////
	// Block extraction
	////////////////////////////////////////////////////////////////////////////////

	inline void LodSurface::extractBlock(const BlockState& bs,
		const RenderCellMap& renderCells, BlockGeometry& out,
		ExtractionCaches& caches, uint64_t& readCounter)
	{
		const LodBlock& b = bs.desc;
		// Density/gradient stay cached across blocks; edge indices are local.
		caches.clearEdgeVertices();

		for (int cz = 0; cz < b.cells; ++cz)
		for (int cy = 0; cy < b.cells; ++cy)
		for (int cx = 0; cx < b.cells; ++cx)
		{
			const Vector3DInt32 cell(
				b.origin.getX() + cx * b.step,
				b.origin.getY() + cy * b.step,
				b.origin.getZ() + cz * b.step);
			const RenderCellMap::const_iterator rt =
				renderCells.find(std::make_pair(b.id, cell));
			const int renderStep = (rt == renderCells.end()) ? b.step : rt->second;

			if (renderStep == b.step)
			{
				extractRegularCell(cell, b.step, out, caches, readCounter);
			}
			else
			{
				// Subdivide into ordinary marching-cubes cells on the finer real
				// source grid; every node is an original sample point.
				const int per = b.step / renderStep;
				for (int sz = 0; sz < per; ++sz)
				for (int sy = 0; sy < per; ++sy)
				for (int sx = 0; sx < per; ++sx)
				{
					extractRegularCell(
						Vector3DInt32(cell.getX() + sx * renderStep,
							cell.getY() + sy * renderStep,
							cell.getZ() + sz * renderStep),
						renderStep, out, caches, readCounter);
				}
			}
		}
	}

	////////////////////////////////////////////////////////////////////////////////
	// Publication and queries
	////////////////////////////////////////////////////////////////////////////////

	inline void LodSurface::publish(const std::map<int32_t, BlockGeometry>& extracted,
		const std::set<int32_t>& rebuilt, uint64_t sourceReads)
	{
		Mesh<Vertex<float> > assembled;
		assembled.setOffset(Vector3DInt32(0, 0, 0));

		struct FloatKey
		{
			Vector3DFloat pos;
			bool operator<(const FloatKey& other) const
			{
				if (pos.getX() != other.pos.getX())
					return pos.getX() < other.pos.getX();
				if (pos.getY() != other.pos.getY())
					return pos.getY() < other.pos.getY();
				return pos.getZ() < other.pos.getZ();
			}
		};
		std::map<FloatKey, uint32_t> sharedIndex;
		std::map<int32_t, std::vector<uint32_t> > remap;

		std::vector<int32_t> order;
		for (std::map<int32_t, BlockGeometry>::const_iterator it = extracted.begin();
			it != extracted.end(); ++it)
			order.push_back(it->first);

		for (size_t oi = 0; oi < order.size(); ++oi)
		{
			const int32_t id = order[oi];
			const BlockGeometry& geom = extracted.find(id)->second;
			std::vector<uint32_t>& map = remap[id];
			map.resize(geom.vertices.size());
			for (size_t v = 0; v < geom.vertices.size(); ++v)
			{
				FloatKey key;
				// Same edge + same source values give bit-identical positions,
				// so seam vertices produced by two blocks merge exactly.
				key.pos = geom.vertices[v].pos;
				std::map<FloatKey, uint32_t>::iterator found = sharedIndex.find(key);
				if (found == sharedIndex.end())
				{
					Vertex<float> vertex;
					vertex.position = geom.vertices[v].pos;
					vertex.normal = geom.vertices[v].normal;
					vertex.data = geom.vertices[v].data;
					const uint32_t idx = assembled.addVertex(vertex);
					sharedIndex[key] = idx;
					map[v] = idx;
				}
				else
				{
					map[v] = found->second;
				}
			}
		}

		for (size_t oi = 0; oi < order.size(); ++oi)
		{
			const int32_t id = order[oi];
			const BlockGeometry& geom = extracted.find(id)->second;
			const std::vector<uint32_t>& map = remap[id];
			for (size_t i = 0; i + 2 < geom.indices.size(); i += 3)
			{
				assembled.addTriangle(
					map[geom.indices[i]],
					map[geom.indices[i + 1]],
					map[geom.indices[i + 2]]);
			}
		}

		// Atomic publication: the old mesh stays visible until this point.
		m_mesh = assembled;
		m_geometry = extracted;
		m_stats.rebuiltBlocks = static_cast<uint64_t>(rebuilt.size());
		m_stats.sourceReads = sourceReads;
	}

	inline Mesh<Vertex<float> > LodSurface::mesh() const
	{
		return m_mesh;
	}

	inline uint32_t LodSurface::blockRevision(int32_t id) const
	{
		const BlockState* bs = findBlock(id);
		if (bs == NULL)
			POLYVOX_THROW(std::invalid_argument,
				"LodSurface::blockRevision: unknown block id");
		return bs->revision;
	}

	inline LodSurfaceStats LodSurface::lastStats() const
	{
		return m_stats;
	}
}
