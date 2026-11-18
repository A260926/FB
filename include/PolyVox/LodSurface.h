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

#ifndef __PolyVox_LodSurface_H__
#define __PolyVox_LodSurface_H__

#include "PolyVox/Impl/PlatformDefinitions.h"

#include "PolyVox/Mesh.h"
#include "PolyVox/RawVolume.h"
#include "PolyVox/Region.h"
#include "PolyVox/Vector.h"
#include "PolyVox/Vertex.h"

#include <cstdint>
#include <map>
#include <set>
#include <stdexcept>
#include <vector>

namespace PolyVox
{
	/// Description of one level-of-detail block.
	///
	/// A block covers the half-open cell box [origin, origin + cells*step) in
	/// source-grid coordinates. Its cells have side length @c step (a positive
	/// power of two) and there are @c cells cells along each axis (@c cells is
	/// an even number not smaller than four). All block endpoints are aligned
	/// to multiples of @c step.
	struct LodBlock
	{
		LodBlock() : id(0), origin(0, 0, 0), cells(0), step(0) {}
		LodBlock(int32_t blockId, const Vector3DInt32& blockOrigin,
			int blockCells, int blockStep)
			: id(blockId), origin(blockOrigin), cells(blockCells), step(blockStep) {}

		int32_t id;
		Vector3DInt32 origin;
		int cells;
		int step;
	};

	/// A single point edit of the source density field.
	struct SampleEdit
	{
		SampleEdit() : position(0, 0, 0), value(0.0f) {}
		SampleEdit(const Vector3DInt32& pos, float val) : position(pos), value(val) {}

		Vector3DInt32 position;
		float value;
	};

	/// Statistics describing the most recent successful modification.
	struct LodSurfaceStats
	{
		LodSurfaceStats() : rebuiltBlocks(0), sourceReads(0) {}

		/// Number of distinct blocks actually re-extracted.
		uint64_t rebuiltBlocks;
		/// Number of source density samples actually read during the rebuild.
		uint64_t sourceReads;
	};

	namespace LodSurfaceDetail
	{
		struct Vector3DInt32Less
		{
			bool operator()(const Vector3DInt32& a, const Vector3DInt32& b) const
			{
				if (a.getX() != b.getX()) return a.getX() < b.getX();
				if (a.getY() != b.getY()) return a.getY() < b.getY();
				return a.getZ() < b.getZ();
			}
		};

		struct CellKeyLess
		{
			bool operator()(const std::pair<int, Vector3DInt32>& a,
				const std::pair<int, Vector3DInt32>& b) const
			{
				if (a.first != b.first) return a.first < b.first;
				return Vector3DInt32Less()(a.second, b.second);
			}
		};

		struct EdgeKeyLess
		{
			bool operator()(const std::pair<Vector3DInt32, Vector3DInt32>& a,
				const std::pair<Vector3DInt32, Vector3DInt32>& b) const
			{
				const Vector3DInt32Less less;
				if (less(a.first, b.first)) return true;
				if (less(b.first, a.first)) return false;
				return less(a.second, b.second);
			}
		};
	}

	/**
	 * A voxel surface resource supporting locally switchable resolution.
	 *
	 * The resource owns an independent copy of the source scalar field stored
	 * in a RawVolume<float>; the isosurface is always extracted at the zero
	 * level. A layout of LodBlock%s describes how the covered box is split
	 * into coarse (large step) and fine (small step) regions. Ordinary Marching
	 * Cubes cells are used away from resolution interfaces, while cells
	 * participating in a seam are tetrahedralised on the finest source grid
	 * with a crack-free transition scheme: neighbouring blocks share every
	 * seam vertex and the published mesh is a closed, oriented two-manifold
	 * wherever the field is closed inside the domain. The coarse layer only
	 * reads original source grid points (no density averaging).
	 *
	 * Source data can only be changed through updateSamples(); concurrent
	 * editing is not handled. Layout changes and density edits rebuild only the
	 * blocks and seam neighbours actually affected, and the new mesh together
	 * with the block revisions is published atomically.
	 */
	class LodSurface
	{
	public:
		/// Builds the resource from the full source field.
		///
		/// @param samples Region of grid points covered by @c values, ordered
		///        with x fastest, then y, then z. One layer of points outside
		///        the block box is required for central-difference normals.
		/// @param values Densities, one per grid point of @c samples; all must
		///        be finite. The resource takes its own copy.
		LodSurface(const Region& samples, const std::vector<float>& values);

		/// Replaces the block layout.
		///
		/// Validates the whole layout first; an invalid layout throws
		/// std::invalid_argument and leaves the existing resource untouched.
		void setLayout(const std::vector<LodBlock>& blocks);

		/// Applies a batch of point edits; repeated positions take the last
		/// value. The whole batch is validated (bounds and finiteness) before
		/// anything is changed, and a failed call leaves the resource intact.
		void updateSamples(const std::vector<SampleEdit>& edits);

		/// Returns an independent copy of the current mesh in world
		/// coordinates; the mesh offset is zero. Never re-reads the source.
		Mesh<Vertex<float> > mesh() const;

		/// Revision counter of a block; increases each time the block is
		/// re-extracted. Throws if the id is not part of the current layout.
		uint32_t blockRevision(int32_t id) const;

		/// Statistics of the most recent successful modification. Failures
		/// preserve the previous statistics.
		LodSurfaceStats lastStats() const;

	private:
		struct BlockState
		{
			BlockState() : revision(0) {}

			LodBlock desc;
			uint32_t revision;
			// Render cells of this block at the last extraction: cell origin ->
			// render step (the native step, or a power-of-two fraction of it
			// after transition subdivision). Lets a later modification tell
			// whether the block's transition pattern actually changed.
			std::map<Vector3DInt32, int, LodSurfaceDetail::Vector3DInt32Less> renderCells;
		};

		struct LocalVertex
		{
			LocalVertex() : pos(0.0f, 0.0f, 0.0f), normal(0.0f, 0.0f, 0.0f), data(0.0f) {}

			Vector3DFloat pos;
			Vector3DFloat normal;
			float data;
		};

		struct BlockGeometry
		{
			std::vector<LocalVertex> vertices;
			std::vector<uint32_t> indices;
		};

		// Rebuild caches. Density and gradient are shared across every block of
		// one rebuild so each distinct source sample is read at most once; the
		// edge-vertex map holds per-block local indices and is cleared between
		// blocks.
		struct ExtractionCaches
		{
			std::map<Vector3DInt32, float, LodSurfaceDetail::Vector3DInt32Less> density;
			std::map<Vector3DInt32, Vector3DFloat, LodSurfaceDetail::Vector3DInt32Less> gradient;
			std::map<std::pair<Vector3DInt32, Vector3DInt32>, uint32_t,
				LodSurfaceDetail::EdgeKeyLess> edgeVertex;

			void clearEdgeVertices() { edgeVertex.clear(); }
		};

		// Native cell (block id + native cell origin) mapped to the step at
		// which it is rendered: the native step, or a power-of-two fraction of
		// it after transition subdivision. Only subdivided cells are stored.
		typedef std::map<std::pair<int32_t, Vector3DInt32>, int,
			LodSurfaceDetail::CellKeyLess> RenderCellMap;

		// --- Validation ---
		void validateLayout(const std::vector<LodBlock>& blocks,
			Region& outCovered) const;
		void reject(const char* message) const;

		// --- Rebuild machinery ---
		// Builds per-cell render steps for a layout by fixed-point red-green
		// relaxation: wherever the isosurface crosses a face, the two sides are
		// forced to the same (finer) render step, which may recursively halve a
		// cell more than once. Geometry queries read no source; only surface
		// tests read density, so blocks far from the surface contribute no
		// source reads. Every block owning a subdivided cell is added to
		// @c outOwners.
		RenderCellMap buildRenderCells(
			const std::map<int32_t, BlockState>& blocks,
			std::set<int32_t>& outOwners,
			ExtractionCaches& caches, uint64_t& readCounter) const;
		void extractBlock(const BlockState& bs,
			const RenderCellMap& renderCells, BlockGeometry& out,
			ExtractionCaches& caches, uint64_t& readCounter);
		void extractRegularCell(const Vector3DInt32& cell, int step,
			BlockGeometry& out, ExtractionCaches& caches,
			uint64_t& readCounter);
		// Whether the isosurface crosses a cell face, sampled on the fine grid.
		bool faceHasSurface(const Vector3DInt32& cell, int cellStep,
			int sampleStep, int axis, int side,
			ExtractionCaches& caches, uint64_t& readCounter) const;
		// Owner block of a native cell origin on the given-step grid.
		int32_t findOwnerId(const Vector3DInt32& cellOrigin, int step,
			const std::map<int32_t, BlockState>& blocks) const;
		// Bit mask of the cell's block-boundary faces that meet a finer block.
		uint8_t interfaceFaceMask(const BlockState& bs, const Vector3DInt32& cell,
			const std::map<int32_t, BlockState>& blocks) const;
		// Native cells immediately across one face: one same-step cell or four
		// half-step cells (the validated neighbour ratio is at most two).
		void faceNeighbourCells(const LodBlock& bs, const Vector3DInt32& cell,
			int axis, int side,
			const std::map<int32_t, BlockState>& blocks,
			std::vector<std::pair<int32_t, Vector3DInt32> >& out) const;
		// Per-block subdivision map of a global render-cell map.
		std::map<Vector3DInt32, int, LodSurfaceDetail::Vector3DInt32Less>
			renderCellsOf(int32_t id, const RenderCellMap& renderCells) const;

		// --- Field access (every actual source read is counted) ---
		float sampleDensity(const Vector3DInt32& p, ExtractionCaches& caches,
			uint64_t& readCounter) const;
		Vector3DFloat sampleGradient(const Vector3DInt32& p,
			ExtractionCaches& caches, uint64_t& readCounter) const;
		Vector3DFloat safeNormalise(const Vector3DFloat& v) const;
		uint32_t vertexOnEdge(const Vector3DInt32& a, const Vector3DInt32& b,
			BlockGeometry& out, ExtractionCaches& caches,
			uint64_t& readCounter) const;

		// --- Layout queries ---
		const BlockState* findBlock(int32_t id) const;
		BlockState* findBlock(int32_t id);
		int blockStepAt(const Vector3DInt32& p,
			const std::map<int32_t, BlockState>& blocks) const;

		// --- Publication ---
		void publish(const std::map<int32_t, BlockGeometry>& extracted,
			const std::set<int32_t>& rebuilt, uint64_t sourceReads);

		// Independent copy of the source field.
		RawVolume<float> m_volume;
		const Region m_sampleRegion;

		// Current, published state.
		std::map<int32_t, BlockState> m_blocks;
		std::map<int32_t, BlockGeometry> m_geometry;
		Mesh<Vertex<float> > m_mesh;
		LodSurfaceStats m_stats;
	};
}

#include "PolyVox/LodSurface.inl"

#endif // __PolyVox_LodSurface_H__
