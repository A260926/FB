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
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
* AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
* OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
* SOFTWARE.
*******************************************************************************/

#include "Impl/ExceptionsImpl.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <set>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace PolyVox
{
	namespace LitCubeImpl
	{
		// A face is described by an outward normal and two tangents chosen so that
		// tangent1 x tangent2 equals the normal. Walking the corners in the order
		// (-,-), (+,-), (+,+), (-,+) in the tangent frame therefore yields
		// counter-clockwise (outward) triangles.
		struct FaceFrame
		{
			int axis;       // Axis of the normal: 0 = x, 1 = y, 2 = z.
			int sign;       // +1 or -1.
			int normal[3];
			int tangent1[3];
			int tangent2[3];
		};

		inline const FaceFrame& faceFrame(int face)
		{
			static const FaceFrame frames[6] =
			{
				{ 0,  1, {  1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } }, // +X
				{ 0, -1, { -1, 0, 0 }, { 0, 0, 1 }, { 0, 1, 0 } }, // -X
				{ 1,  1, {  0, 1, 0 }, { 0, 0, 1 }, { 1, 0, 0 } }, // +Y
				{ 1, -1, {  0,-1, 0 }, { 1, 0, 0 }, { 0, 0, 1 } }, // -Y
				{ 2,  1, {  0, 0, 1 }, { 1, 0, 0 }, { 0, 1, 0 } }, // +Z
				{ 2, -1, {  0, 0,-1 }, { 0, 1, 0 }, { 1, 0, 0 } }  // -Z
			};
			return frames[face];
		}

		inline int faceForAxis(int axis, int sign)
		{
			return axis * 2 + (sign < 0 ? 1 : 0);
		}

		// Corner signs, in ring order: (-,-), (+,-), (+,+), (-,+).
		inline int cornerSign1(int corner) { return (corner == 0 || corner == 3) ? -1 : 1; }
		inline int cornerSign2(int corner) { return (corner == 0 || corner == 1) ? -1 : 1; }
	}

	template<typename DataType>
	Vertex<DataType> decodeVertex(const LitCubicVertex<DataType>& litVertex)
	{
		Vertex<DataType> result;
		result.position = litVertex.position;
		result.normal = litVertex.normal;
		result.data = litVertex.data;
		return result;
	}

	template<typename VolumeType, typename MeshType, typename IsQuadNeeded>
	void extractLitCubicMeshCustom(
		VolumeType* volData,
		const LightVolume<VolumeType>& lightVolume,
		Region region,
		MeshType* result,
		IsQuadNeeded isQuadNeeded,
		bool bMergeQuads)
	{
		const int32_t maxRegionDimensionInVoxels = 255;
		POLYVOX_THROW_IF(region.getWidthInVoxels() > maxRegionDimensionInVoxels, std::invalid_argument, "Requested extraction region exceeds maximum dimensions");
		POLYVOX_THROW_IF(region.getHeightInVoxels() > maxRegionDimensionInVoxels, std::invalid_argument, "Requested extraction region exceeds maximum dimensions");
		POLYVOX_THROW_IF(region.getDepthInVoxels() > maxRegionDimensionInVoxels, std::invalid_argument, "Requested extraction region exceeds maximum dimensions");

		result->clear();
		result->setOffset(Vector3DInt32(0, 0, 0)); // Positions are absolute world coordinates.

		typedef typename MeshType::IndexType IndexType;
		typedef typename VolumeType::VoxelType VoxelType;
		typedef std::array<uint32_t, 4> CornerSums; // Per-channel sum of four sample voxels.

		// ------------------------------------------------------------------
		// Vertex sharing
		// ------------------------------------------------------------------

		// Two face corners may share a vertex only when position, face normal, material
		// and all four light channels are identical.
		struct VertexKey
		{
			int64_t px; // Position in half-voxel units.
			int64_t py;
			int64_t pz;
			uint8_t normal;
			uint32_t material;
			uint32_t light; // Four 6-bit sums (each channel is in [0, 60]).

			bool operator==(const VertexKey& rhs) const
			{
				return px == rhs.px && py == rhs.py && pz == rhs.pz &&
					normal == rhs.normal && material == rhs.material && light == rhs.light;
			}
		};

		struct VertexKeyHasher
		{
			size_t operator()(const VertexKey& key) const
			{
				uint64_t h = static_cast<uint64_t>(key.px) * 0x9E3779B97F4A7C15ull;
				const uint64_t values[5] = {
					static_cast<uint64_t>(key.py),
					static_cast<uint64_t>(key.pz),
					static_cast<uint64_t>(key.normal),
					static_cast<uint64_t>(key.material),
					static_cast<uint64_t>(key.light)
				};
				for (int i = 0; i < 5; ++i)
				{
					h ^= values[i] + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
				}
				return static_cast<size_t>(h);
			}
		};

		std::unordered_map<VertexKey, IndexType, VertexKeyHasher> vertexLookup;

		auto packSums = [](const CornerSums& sums) -> uint32_t
		{
			return (sums[0] & 0x3F) | ((sums[1] & 0x3F) << 6) | ((sums[2] & 0x3F) << 12) | ((sums[3] & 0x3F) << 18);
		};

		auto addVertex = [&](int face, int64_t px, int64_t py, int64_t pz, VoxelType material, const CornerSums& sums) -> IndexType
		{
			VertexKey key;
			key.px = px;
			key.py = py;
			key.pz = pz;
			key.normal = static_cast<uint8_t>(face);
			key.material = static_cast<uint32_t>(material);
			key.light = packSums(sums);

			typename std::unordered_map<VertexKey, IndexType, VertexKeyHasher>::const_iterator it = vertexLookup.find(key);
			if (it != vertexLookup.end())
			{
				return it->second;
			}

			const LitCubeImpl::FaceFrame& frame = LitCubeImpl::faceFrame(face);
			LitCubicVertex<VoxelType> vertex;
			vertex.position.setElements(static_cast<float>(px) * 0.5f, static_cast<float>(py) * 0.5f, static_cast<float>(pz) * 0.5f);
			vertex.normal.setElements(static_cast<float>(frame.normal[0]), static_cast<float>(frame.normal[1]), static_cast<float>(frame.normal[2]));
			vertex.data = material;
			for (int channel = 0; channel < 4; ++channel)
			{
				vertex.light[channel] = static_cast<float>(sums[channel]) / 60.0f;
			}

			const IndexType index = result->addVertex(vertex);
			vertexLookup.insert(std::make_pair(key, index));
			return index;
		};

		// Emits a rectangle of unit faces on \c face. The rectangle spans cells
		// [a1..b1] x [a2..b2] in the face's tangent coordinates and lies on the given
		// plane (in half-voxel units). corners supplies the light sums for the four
		// ring-ordered corners; a flat rectangle passes the same sums four times.
		auto emitQuad = [&](int face, int64_t plane, int64_t a1, int64_t b1, int64_t a2, int64_t b2,
			VoxelType material, const CornerSums corners[4])
		{
			const LitCubeImpl::FaceFrame& frame = LitCubeImpl::faceFrame(face);

			IndexType indices[4];
			for (int corner = 0; corner < 4; ++corner)
			{
				const int64_t tangent1 = (LitCubeImpl::cornerSign1(corner) < 0) ? (2 * a1 - 1) : (2 * b1 + 1);
				const int64_t tangent2 = (LitCubeImpl::cornerSign2(corner) < 0) ? (2 * a2 - 1) : (2 * b2 + 1);

				int64_t position[3] = { 0, 0, 0 };
				position[frame.axis] = plane;
				for (int axis = 0; axis < 3; ++axis)
				{
					if (frame.tangent1[axis] != 0) position[axis] = frame.tangent1[axis] * tangent1;
					if (frame.tangent2[axis] != 0) position[axis] = frame.tangent2[axis] * tangent2;
				}

				indices[corner] = addVertex(face, position[0], position[1], position[2], material, corners[corner]);
			}

			result->addTriangle(indices[0], indices[1], indices[2]);
			result->addTriangle(indices[0], indices[2], indices[3]);
		};

		// ------------------------------------------------------------------
		// Gather unit faces
		// ------------------------------------------------------------------

		struct UnitFace
		{
			int face;
			int64_t plane;
			int64_t c1;
			int64_t c2;
			VoxelType material;
			bool uniform;             // True when all four corner sums are identical.
			CornerSums corners[4];    // Per-corner sums in ring order.
		};

		std::vector<UnitFace> unitFaces;

		// Samples the four voxels outside a face corner: the outside voxel itself plus
		// its neighbours one step along each face tangent in the direction of the corner.
		auto sampleCornerSums = [&](const Vector3DInt32& back, int face, int s1, int s2, CornerSums& sums)
		{
			const LitCubeImpl::FaceFrame& frame = LitCubeImpl::faceFrame(face);
			const int32_t ox = back.getX() + frame.normal[0];
			const int32_t oy = back.getY() + frame.normal[1];
			const int32_t oz = back.getZ() + frame.normal[2];

			for (int channel = 0; channel < 4; ++channel)
			{
				sums[channel] = 0;
			}

			for (int sample = 0; sample < 4; ++sample)
			{
				const int t1 = (sample & 1) ? s1 : 0;
				const int t2 = (sample & 2) ? s2 : 0;
				const LightValue value = lightVolume.getLight(
					ox + t1 * frame.tangent1[0] + t2 * frame.tangent2[0],
					oy + t1 * frame.tangent1[1] + t2 * frame.tangent2[1],
					oz + t1 * frame.tangent1[2] + t2 * frame.tangent2[2]);
				sums[0] += value.sky;
				sums[1] += value.red;
				sums[2] += value.green;
				sums[3] += value.blue;
			}
		};

		auto recordFace = [&](const Vector3DInt32& back, int face, VoxelType material)
		{
			const LitCubeImpl::FaceFrame& frame = LitCubeImpl::faceFrame(face);

			UnitFace unit;
			unit.face = face;
			unit.plane = 2 * static_cast<int64_t>(back.getElement(frame.axis)) + frame.sign;
			unit.c1 = frame.tangent1[0] * back.getX() + frame.tangent1[1] * back.getY() + frame.tangent1[2] * back.getZ();
			unit.c2 = frame.tangent2[0] * back.getX() + frame.tangent2[1] * back.getY() + frame.tangent2[2] * back.getZ();
			unit.material = material;
			unit.uniform = true;

			for (int corner = 0; corner < 4; ++corner)
			{
				sampleCornerSums(back, face, LitCubeImpl::cornerSign1(corner), LitCubeImpl::cornerSign2(corner), unit.corners[corner]);
			}
			for (int corner = 1; corner < 4; ++corner)
			{
				if (unit.corners[corner] != unit.corners[0])
				{
					unit.uniform = false;
					break;
				}
			}
			unitFaces.push_back(unit);
		};

		typename VolumeType::Sampler volumeSampler(volData);

		for (int32_t z = region.getLowerZ(); z <= region.getUpperZ(); ++z)
		{
			for (int32_t y = region.getLowerY(); y <= region.getUpperY(); ++y)
			{
				volumeSampler.setPosition(region.getLowerX(), y, z);

				for (int32_t x = region.getLowerX(); x <= region.getUpperX(); ++x)
				{
					VoxelType material;
					const VoxelType currentVoxel = volumeSampler.getVoxel();
					const VoxelType negXVoxel = volumeSampler.peekVoxel1nx0py0pz();
					const VoxelType negYVoxel = volumeSampler.peekVoxel0px1ny0pz();
					const VoxelType negZVoxel = volumeSampler.peekVoxel0px0py1nz();

					if (isQuadNeeded(currentVoxel, negXVoxel, material))
					{
						recordFace(Vector3DInt32(x, y, z), LitCubeImpl::faceForAxis(0, -1), material);
					}
					if (isQuadNeeded(negXVoxel, currentVoxel, material))
					{
						recordFace(Vector3DInt32(x - 1, y, z), LitCubeImpl::faceForAxis(0, 1), material);
					}
					if (isQuadNeeded(currentVoxel, negYVoxel, material))
					{
						recordFace(Vector3DInt32(x, y, z), LitCubeImpl::faceForAxis(1, -1), material);
					}
					if (isQuadNeeded(negYVoxel, currentVoxel, material))
					{
						recordFace(Vector3DInt32(x, y - 1, z), LitCubeImpl::faceForAxis(1, 1), material);
					}
					if (isQuadNeeded(currentVoxel, negZVoxel, material))
					{
						recordFace(Vector3DInt32(x, y, z), LitCubeImpl::faceForAxis(2, -1), material);
					}
					if (isQuadNeeded(negZVoxel, currentVoxel, material))
					{
						recordFace(Vector3DInt32(x, y, z - 1), LitCubeImpl::faceForAxis(2, 1), material);
					}

					volumeSampler.movePositiveX();
				}
			}
		}

		// ------------------------------------------------------------------
		// Merge and emit
		// ------------------------------------------------------------------

		for (int face = 0; face < 6; ++face)
		{
			std::map<int64_t, std::vector<size_t> > byPlane;
			for (size_t i = 0; i < unitFaces.size(); ++i)
			{
				if (unitFaces[i].face == face)
				{
					byPlane[unitFaces[i].plane].push_back(i);
				}
			}

			for (typename std::map<int64_t, std::vector<size_t> >::iterator planeIt = byPlane.begin(); planeIt != byPlane.end(); ++planeIt)
			{
				const int64_t plane = planeIt->first;
				std::vector<size_t>& faceIndices = planeIt->second;

				// Faces with a lighting gradient keep their unit quads.
				struct GroupKey
				{
					uint32_t material;
					uint32_t light;
					bool operator<(const GroupKey& rhs) const
					{
						if (material != rhs.material) return material < rhs.material;
						return light < rhs.light;
					}
				};
				std::map<GroupKey, std::vector<size_t> > groups;

				for (size_t i = 0; i < faceIndices.size(); ++i)
				{
					const UnitFace& unit = unitFaces[faceIndices[i]];
					if (!bMergeQuads || !unit.uniform)
					{
						emitQuad(face, plane, unit.c1, unit.c1, unit.c2, unit.c2, unit.material, unit.corners);
					}
					else
					{
						GroupKey key;
						key.material = static_cast<uint32_t>(unit.material);
						key.light = packSums(unit.corners[0]);
						groups[key].push_back(faceIndices[i]);
					}
				}

				// Each group holds coplanar, same-facing, same-material unit faces which all
				// share the same four channel values. Greedily cover them with maximal
				// rectangles; a standalone flat rectangle is merged into a single quad.
				for (typename std::map<GroupKey, std::vector<size_t> >::iterator groupIt = groups.begin(); groupIt != groups.end(); ++groupIt)
				{
					std::vector<size_t>& cells = groupIt->second;

					std::sort(cells.begin(), cells.end(), [&](size_t a, size_t b)
					{
						const UnitFace& ua = unitFaces[a];
						const UnitFace& ub = unitFaces[b];
						if (ua.c2 != ub.c2) return ua.c2 < ub.c2;
						return ua.c1 < ub.c1;
					});

					std::set<std::pair<int64_t, int64_t> > remaining;
					for (size_t i = 0; i < cells.size(); ++i)
					{
						remaining.insert(std::make_pair(unitFaces[cells[i]].c1, unitFaces[cells[i]].c2));
					}

					for (size_t i = 0; i < cells.size(); ++i)
					{
						const int64_t startC1 = unitFaces[cells[i]].c1;
						const int64_t startC2 = unitFaces[cells[i]].c2;
						if (remaining.count(std::make_pair(startC1, startC2)) == 0)
						{
							continue; // Covered by an earlier rectangle.
						}

						int64_t endC1 = startC1;
						while (remaining.count(std::make_pair(endC1 + 1, startC2)) != 0)
						{
							++endC1;
						}

						int64_t endC2 = startC2;
						for (;;)
						{
							bool fullRow = true;
							for (int64_t c1 = startC1; c1 <= endC1; ++c1)
							{
								if (remaining.count(std::make_pair(c1, endC2 + 1)) == 0)
								{
									fullRow = false;
									break;
								}
							}
							if (!fullRow)
							{
								break;
							}
							++endC2;
						}

						for (int64_t c2 = startC2; c2 <= endC2; ++c2)
						{
							for (int64_t c1 = startC1; c1 <= endC1; ++c1)
							{
								remaining.erase(std::make_pair(c1, c2));
							}
						}

						const UnitFace& unit = unitFaces[cells[i]];
						emitQuad(face, plane, startC1, endC1, startC2, endC2, unit.material, unit.corners);
					}
				}
			}
		}

		result->removeUnusedVertices();
	}

	template<typename VolumeType, typename IsQuadNeeded>
	Mesh<LitCubicVertex<typename VolumeType::VoxelType> > extractLitCubicMesh(
		VolumeType* volData,
		const LightVolume<VolumeType>& lightVolume,
		Region region,
		IsQuadNeeded isQuadNeeded,
		bool bMergeQuads)
	{
		Mesh< LitCubicVertex<typename VolumeType::VoxelType> > result;
		extractLitCubicMeshCustom(volData, lightVolume, region, &result, isQuadNeeded, bMergeQuads);
		return result;
	}
}
