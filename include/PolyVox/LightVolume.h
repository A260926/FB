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

#ifndef __PolyVox_LightVolume_H__
#define __PolyVox_LightVolume_H__

#include "Impl/PlatformDefinitions.h"

#include "Region.h"
#include "Vector.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace PolyVox
{
	/// Identifies one of the four light channels stored by LightVolume.
	enum LightChannel
	{
		LightSky = 0, ///< Skylight. Only present in non-opaque voxels.
		LightRed = 1, ///< Red block light.
		LightGreen = 2, ///< Green block light.
		LightBlue = 3  ///< Blue block light.
	};

	/// The four channel values stored for a single voxel. Every value is in [0, 15].
	struct LightValue
	{
		LightValue() : sky(0), red(0), green(0), blue(0) {}
		LightValue(uint8_t skyIn, uint8_t redIn, uint8_t greenIn, uint8_t blueIn)
			: sky(skyIn), red(redIn), green(greenIn), blue(blueIn) {}

		uint8_t sky;
		uint8_t red;
		uint8_t green;
		uint8_t blue;

		bool operator==(const LightValue& rhs) const
		{
			return (sky == rhs.sky) && (red == rhs.red) && (green == rhs.green) && (blue == rhs.blue);
		}

		bool operator!=(const LightValue& rhs) const
		{
			return !(*this == rhs);
		}
	};

	/// One entry of the fixed 256 entry material table supplied to LightVolume.
	///
	/// Material 0 is always treated as non-opaque air which emits no light, regardless
	/// of the values stored here. For every other entry \c opaque determines whether the
	/// material blocks light and the three emit values are the per-channel amount of
	/// red, green and blue block light the material emits (each in [0, 15]).
	struct LightMaterial
	{
		LightMaterial() : opaque(false), emitRed(0), emitGreen(0), emitBlue(0) {}
		LightMaterial(bool opaqueIn, uint8_t emitRedIn, uint8_t emitGreenIn, uint8_t emitBlueIn)
			: opaque(opaqueIn), emitRed(emitRedIn), emitGreen(emitGreenIn), emitBlue(emitBlueIn) {}

		bool opaque;
		uint8_t emitRed;
		uint8_t emitGreen;
		uint8_t emitBlue;
	};

	/// A single requested material change submitted to LightVolume::apply().
	struct LightEdit
	{
		LightEdit() : x(0), y(0), z(0), material(0) {}
		LightEdit(int32_t xIn, int32_t yIn, int32_t zIn, uint8_t materialIn)
			: x(xIn), y(yIn), z(zIn), material(materialIn) {}
		LightEdit(const Vector3DInt32& pos, uint8_t materialIn)
			: x(pos.getX()), y(pos.getY()), z(pos.getZ()), material(materialIn) {}

		int32_t x;
		int32_t y;
		int32_t z;
		uint8_t material;
	};

	/// Work performed by the most recent LightVolume::apply() call.
	///
	/// Both counters are reset to zero at the start of every batch. \c materialReads is
	/// incremented whenever a material value is actually read from the bound volume, and
	/// \c queuePops is incremented whenever a position is removed from either the light
	/// removal queue or the light addition (propagation) queue.
	struct LightVolumeStats
	{
		LightVolumeStats() : materialReads(0), queuePops(0) {}

		uint64_t materialReads;
		uint64_t queuePops;
	};

	/// Dynamic sky and RGB block lighting for a volume of material indices.
	///
	/// LightVolume binds to a VolumeType (normally RawVolume<uint8_t> or
	/// PagedVolume<uint8_t>), a finite Region and a fixed table of 256 materials. The
	/// region follows the usual inclusive Region convention and voxels outside it have
	/// a light value of zero. The volume must outlive the LightVolume.
	///
	/// Internally four 4-bit channels (0 to 15) are stored per voxel. The constructor
	/// performs one full scan and establishes the lighting baseline. After construction
	/// all voxel edits must be submitted through apply(); concurrent access, direct
	/// writes to the underlying volume which bypass this class, and re-entrant Pager
	/// callbacks are not supported.
	template <typename VolumeType>
	class LightVolume
	{
	public:
		typedef typename VolumeType::VoxelType VoxelType;

		/// Binds to \p volume, takes ownership of a copy of \p materials and computes
		/// the lighting for \p region. \p materials must contain exactly 256 entries;
		/// entry zero is forced to be non-opaque air.
		LightVolume(VolumeType* volume, const Region& region, const std::array<LightMaterial, 256>& materials);

		/// Returns the light stored at a position. Positions outside the bound Region
		/// return a zero LightValue.
		LightValue getLight(int32_t x, int32_t y, int32_t z) const;
		/// \overload
		LightValue getLight(const Vector3DInt32& pos) const;
		/// Returns a single channel at a position.
		uint8_t getLight(int32_t x, int32_t y, int32_t z, LightChannel channel) const;

		/// Submits a batch of material changes and updates the underlying volume and the
		/// light field.
		///
		/// When several edits refer to the same position the last one wins. Only positions
		/// whose material actually changes cause a volume write, and a batch in which no
		/// final material changes performs no writes and no propagation work at all.
		///
		/// The returned positions are exactly those whose final light differs from the
		/// value they had before the call, in (x, y, z) lexicographic order. Positions
		/// submitted more than once, and positions whose light changed and then changed
		/// back within the batch, are not reported.
		std::vector<Vector3DInt32> apply(const std::vector<LightEdit>& changes);

		/// Statistics for the most recent apply() call.
		const LightVolumeStats& lastStats() const { return m_stats; }
		/// Material reads performed by the most recent apply() call.
		uint64_t materialReads() const { return m_stats.materialReads; }
		/// Propagation queue pops performed by the most recent apply() call.
		uint64_t queuePops() const { return m_stats.queuePops; }

		/// The region this light volume is responsible for.
		const Region& getRegion() const { return m_region; }

	private:
		struct LexicographicLess
		{
			bool operator()(const Vector3DInt32& a, const Vector3DInt32& b) const
			{
				if (a.getX() != b.getX()) return a.getX() < b.getX();
				if (a.getY() != b.getY()) return a.getY() < b.getY();
				return a.getZ() < b.getZ();
			}
		};

		size_t indexOf(int32_t x, int32_t y, int32_t z) const;
		Vector3DInt32 positionOf(size_t index) const;

		uint8_t getChannel(size_t index, int channel) const;
		void setChannel(size_t index, int channel, uint8_t value);
		uint8_t sourceAt(size_t index, int channel) const;

		void initialPropagate();

		Region m_region;
		VolumeType* m_volume;
		std::array<LightMaterial, 256> m_materials;

		int32_t m_width;
		int32_t m_height;
		int32_t m_depth;

		std::vector<uint16_t> m_light;   // Four packed 4-bit channels per voxel.
		std::vector<uint8_t> m_material; // Cached material index per voxel.
		std::vector<uint8_t> m_opaque;   // Cached opacity flag per voxel.

		LightVolumeStats m_stats;
	};

}

#include "LightVolume.inl"

#endif //__PolyVox_LightVolume_H__
