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

#ifndef __PolyVox_LitCubicSurfaceExtractor_H__
#define __PolyVox_LitCubicSurfaceExtractor_H__

#include "Impl/PlatformDefinitions.h"

#include "DefaultIsQuadNeeded.h"
#include "LightVolume.h"
#include "Mesh.h"
#include "Region.h"
#include "Vertex.h"

#include <array>

namespace PolyVox
{
	/// The vertex format produced by extractLitCubicMesh().
	///
	/// Unlike CubicVertex this format is not compressed: it stores the world-space
	/// position and normal directly, along with the source material and the four
	/// light channels. Positions are absolute world coordinates (the mesh offset is
	/// therefore zero) and the light channels are stored in the range [0, 1], i.e.
	/// the raw 0..15 value divided by 15.
	template <typename _DataType>
	struct LitCubicVertex
	{
		typedef _DataType DataType;

		/// World-space position of the face corner.
		Vector3DFloat position;
		/// Outward facing, axis aligned normal of the face which produced this vertex.
		Vector3DFloat normal;
		/// The material of the voxel which owns the face.
		DataType data;
		/// Sky, red, green and blue light (in that order) divided by 15.
		std::array<float, 4> light;
	};

	/// Decodes a LitCubicVertex into the generic Vertex type. The light channels are
	/// not representable in Vertex and are therefore dropped; read them from the
	/// LitCubicVertex directly when they are needed.
	template<typename DataType>
	Vertex<DataType> decodeVertex(const LitCubicVertex<DataType>& litVertex);

	/// Generates a lit cubic mesh from \p volData using the surface test and winding of
	/// extractCubicMesh().
	///
	/// The light of a face corner is the average of the four voxels immediately outside
	/// that corner (the outside voxel plus the voxels one step away along each of the two
	/// face tangents, in the direction of the corner). Light outside the LightVolume's
	/// region is treated as zero. Vertices are only shared when their position, face
	/// normal, material and light are all identical.
	///
	/// When \p bMergeQuads is true, coplanar unit faces of the same material are merged
	/// into larger rectangles only if every original unit face corner in the rectangle
	/// carries the same four light channels. Areas with a lighting gradient keep their
	/// unit faces.
	template<typename VolumeType, typename IsQuadNeeded = DefaultIsQuadNeeded<typename VolumeType::VoxelType> >
	Mesh<LitCubicVertex<typename VolumeType::VoxelType> > extractLitCubicMesh(
		VolumeType* volData,
		const LightVolume<VolumeType>& lightVolume,
		Region region,
		IsQuadNeeded isQuadNeeded = IsQuadNeeded(),
		bool bMergeQuads = true);

	/// As extractLitCubicMesh() but writes into a user-provided mesh.
	template<typename VolumeType, typename MeshType, typename IsQuadNeeded = DefaultIsQuadNeeded<typename VolumeType::VoxelType> >
	void extractLitCubicMeshCustom(
		VolumeType* volData,
		const LightVolume<VolumeType>& lightVolume,
		Region region,
		MeshType* result,
		IsQuadNeeded isQuadNeeded = IsQuadNeeded(),
		bool bMergeQuads = true);
}

#include "LitCubicSurfaceExtractor.inl"

#endif //__PolyVox_LitCubicSurfaceExtractor_H__
