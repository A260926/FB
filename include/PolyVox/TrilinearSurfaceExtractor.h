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

#ifndef __PolyVox_TrilinearSurfaceExtractor_H__
#define __PolyVox_TrilinearSurfaceExtractor_H__

#include "DefaultMarchingCubesController.h"
#include "Exceptions.h"
#include "Impl/PlatformDefinitions.h"
#include "Mesh.h"
#include "Region.h"
#include "Vertex.h"

#include <stdexcept>
#include <string>

namespace PolyVox
{
	/// Thrown by extractTrilinearMesh() when the field of a cell is too close to a
	/// degenerate configuration (the iso level passing through a corner, a face
	/// saddle or an interior critical point) for the topology to be decided
	/// reliably. The position carried by the exception is the global coordinate of
	/// the lower corner of the offending cell (not a vertex position).
	///
	/// Because the exception is thrown while extracting, no partial mesh is
	/// returned: the mesh object supplied to the extraction is only populated once
	/// every cell has been classified successfully.
	class trilinear_degenerate_field : public std::runtime_error
	{
	public:
		trilinear_degenerate_field(const Vector3DInt32& v3dCellLowerCorner, const std::string& strMessage);

		/// Global coordinate of the lower corner of the cell which could not be classified.
		Vector3DInt32 getCellPosition(void) const;

	private:
		Vector3DInt32 m_v3dCellLowerCorner;
	};

	/// Generates a mesh whose topology follows the trilinear scalar field defined
	/// by the eight corner densities of each cell, rather than the sign-only
	/// Marching Cubes lookup table.
	///
	/// Every cell is treated as the unique trilinear interpolant of its eight
	/// corner densities. Ambiguous faces are resolved from the sign of the
	/// bilinear saddle inside the face (so two cells sharing a face always make
	/// the same pairing), and the interior critical point of the trilinear field
	/// is examined to decide whether a cell contains one connected surface sheet
	/// or two separate ones. No fixed tetrahedralisation or subdivision grid is
	/// used.
	///
	/// Vertices on lattice edges are shared across cells and edges: a given
	/// global lattice edge always yields a single vertex index, positioned by
	/// linear root finding along the edge. Additional vertices may be created
	/// inside a cell when its boundary loops have to be capped or joined.
	/// Positions use plain floating point coordinates (there is no 8.8 fixed
	/// point encoding) and are relative to the Region lower corner, which is also
	/// stored as the mesh offset, exactly as for extractMarchingCubesMesh().
	///
	/// \param volData          Volume to extract from. Must not be null.
	/// \param region           Inclusive range of sample points; the cells are the
	///                         gaps between them. Adjacent extraction regions may
	///                         share one layer of sample points.
	/// \param controller       Controller supplying convertToDensity(),
	///                         getThreshold() and blendMaterials(), with the same
	///                         conventions as the Marching Cubes extractor.
	/// \param degeneracy_tolerance Positive relative tolerance (see below).
	///
	/// The degeneracy test is <b>normalised by the density range inside the
	/// cell</b>. Let \c S be the largest absolute corner deviation from the
	/// threshold in the cell. A cell is rejected (trilinear_degenerate_field is
	/// thrown) when any corner deviation, any ambiguous face saddle value or any
	/// interior critical point value is smaller than
	/// <c>degeneracy_tolerance * S</c> in absolute magnitude. Thus the default
	/// value of <c>1e-6f</c> rejects inputs within roughly one part per million of
	/// a topology change, while ordinary decidable ambiguous faces are processed
	/// normally. Cells lying entirely on one side of the threshold are skipped
	/// without any degeneracy test.
	///
	/// \code
	/// PolyVox::RawVolume<float> volume(PolyVox::Region(-1, -1, -1, 2, 2, 2));
	/// // ... fill the volume with analytic density values ...
	/// auto mesh = PolyVox::extractTrilinearMesh(&volume, PolyVox::Region(0, 0, 0, 1, 1, 1));
	/// \endcode
	///
	/// A custom controller and an explicit tolerance can be passed in the same
	/// way as for the classic extractor:
	///
	/// \code
	/// MyController controller(/* threshold = */ 0.0f);
	/// auto mesh = PolyVox::extractTrilinearMesh(&volume, region, controller, 1.0e-5f);
	/// \endcode
	///
	/// \sa extractMarchingCubesMesh
	template< typename VolumeType, typename ControllerType = DefaultMarchingCubesController<typename VolumeType::VoxelType> >
	Mesh< Vertex<typename VolumeType::VoxelType> > extractTrilinearMesh(VolumeType* volData, Region region,
		ControllerType controller = ControllerType(), float degeneracy_tolerance = 1.0e-6f);
}

#include "TrilinearSurfaceExtractor.inl"

#endif //__PolyVox_TrilinearSurfaceExtractor_H__
