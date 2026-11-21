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
#include <deque>
#include <functional>
#include <map>
#include <stdexcept>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace PolyVox
{
	namespace LightVolumeImpl
	{
		inline uint8_t channelOf(const LightValue& value, int channel)
		{
			switch (channel)
			{
			case LightSky: return value.sky;
			case LightRed: return value.red;
			case LightGreen: return value.green;
			default: return value.blue;
			}
		}

		// The six face neighbours, expressed in region-local voxel space.
		extern const int32_t neighbourDeltaX[6];
		extern const int32_t neighbourDeltaY[6];
		extern const int32_t neighbourDeltaZ[6];

		// Skylight travels straight down for free; every other step (including all
		// block light steps) costs one unit.
		inline int stepCost(int channel, int32_t deltaY)
		{
			return (channel == LightSky && deltaY == -1) ? 0 : 1;
		}
	}

	// Definitions of the neighbour tables. 'inline' variables require C++17.
	inline const int32_t LightVolumeImpl::neighbourDeltaX[6] = { -1, 1, 0, 0, 0, 0 };
	inline const int32_t LightVolumeImpl::neighbourDeltaY[6] = { 0, 0, -1, 1, 0, 0 };
	inline const int32_t LightVolumeImpl::neighbourDeltaZ[6] = { 0, 0, 0, 0, -1, 1 };

	template <typename VolumeType>
	LightVolume<VolumeType>::LightVolume(VolumeType* volume, const Region& region, const std::array<LightMaterial, 256>& materials)
		: m_region(region)
		, m_volume(volume)
		, m_materials(materials)
		, m_width(0)
		, m_height(0)
		, m_depth(0)
	{
		if (!m_volume)
		{
			POLYVOX_THROW(std::invalid_argument, "LightVolume requires a non-null volume.");
		}
		if (!m_region.isValid())
		{
			POLYVOX_THROW(std::invalid_argument, "LightVolume requires a valid region.");
		}

		// Material zero is always non-opaque air which emits no light.
		m_materials[0] = LightMaterial(false, 0, 0, 0);

		m_width = m_region.getWidthInVoxels();
		m_height = m_region.getHeightInVoxels();
		m_depth = m_region.getDepthInVoxels();

		const size_t noOfVoxels = static_cast<size_t>(m_width) * static_cast<size_t>(m_height) * static_cast<size_t>(m_depth);
		m_light.assign(noOfVoxels, 0);
		m_material.assign(noOfVoxels, 0);
		m_opaque.assign(noOfVoxels, 0);

		// Full scan at construction time. This is the only time the entire region is
		// read; subsequent work is local to the submitted edits.
		size_t index = 0;
		for (int32_t z = m_region.getLowerZ(); z <= m_region.getUpperZ(); ++z)
		{
			for (int32_t y = m_region.getLowerY(); y <= m_region.getUpperY(); ++y)
			{
				for (int32_t x = m_region.getLowerX(); x <= m_region.getUpperX(); ++x, ++index)
				{
					const uint8_t material = static_cast<uint8_t>(m_volume->getVoxel(x, y, z));
					m_material[index] = material;
					m_opaque[index] = (material == 0 || !m_materials[material].opaque) ? 0 : 1;

					const LightMaterial& description = m_materials[material];
					uint16_t packed = 0;
					if (!m_opaque[index] && (y == m_region.getUpperY()))
					{
						packed |= static_cast<uint16_t>(15); // Skylight in bit 0..3
					}
					packed |= static_cast<uint16_t>((description.emitRed & 0x0F) << 4);
					packed |= static_cast<uint16_t>((description.emitGreen & 0x0F) << 8);
					packed |= static_cast<uint16_t>((description.emitBlue & 0x0F) << 12);
					m_light[index] = packed;
				}
			}
		}

		initialPropagate();

		// Construction does not contribute to batch statistics.
		m_stats = LightVolumeStats();
	}

	template <typename VolumeType>
	size_t LightVolume<VolumeType>::indexOf(int32_t x, int32_t y, int32_t z) const
	{
		const size_t localX = static_cast<size_t>(x - m_region.getLowerX());
		const size_t localY = static_cast<size_t>(y - m_region.getLowerY());
		const size_t localZ = static_cast<size_t>(z - m_region.getLowerZ());
		return localX + localY * static_cast<size_t>(m_width) + localZ * static_cast<size_t>(m_width) * static_cast<size_t>(m_height);
	}

	template <typename VolumeType>
	Vector3DInt32 LightVolume<VolumeType>::positionOf(size_t index) const
	{
		const int32_t localX = static_cast<int32_t>(index % static_cast<size_t>(m_width));
		const int32_t localY = static_cast<int32_t>((index / static_cast<size_t>(m_width)) % static_cast<size_t>(m_height));
		const int32_t localZ = static_cast<int32_t>(index / (static_cast<size_t>(m_width) * static_cast<size_t>(m_height)));
		return Vector3DInt32(m_region.getLowerX() + localX, m_region.getLowerY() + localY, m_region.getLowerZ() + localZ);
	}

	template <typename VolumeType>
	uint8_t LightVolume<VolumeType>::getChannel(size_t index, int channel) const
	{
		return static_cast<uint8_t>((m_light[index] >> (channel * 4)) & 0x0F);
	}

	template <typename VolumeType>
	void LightVolume<VolumeType>::setChannel(size_t index, int channel, uint8_t value)
	{
		const uint16_t mask = static_cast<uint16_t>(0x0F << (channel * 4));
		m_light[index] = static_cast<uint16_t>((m_light[index] & ~mask) | (static_cast<uint16_t>(value & 0x0F) << (channel * 4)));
	}

	template <typename VolumeType>
	uint8_t LightVolume<VolumeType>::sourceAt(size_t index, int channel) const
	{
		if (channel == LightSky)
		{
			if (m_opaque[index])
			{
				return 0;
			}
			const int32_t localY = static_cast<int32_t>((index / static_cast<size_t>(m_width)) % static_cast<size_t>(m_height));
			return (localY == (m_height - 1)) ? 15 : 0;
		}

		const LightMaterial& description = m_materials[m_material[index]];
		switch (channel)
		{
		case LightRed: return static_cast<uint8_t>(description.emitRed & 0x0F);
		case LightGreen: return static_cast<uint8_t>(description.emitGreen & 0x0F);
		default: return static_cast<uint8_t>(description.emitBlue & 0x0F);
		}
	}

	template <typename VolumeType>
	void LightVolume<VolumeType>::initialPropagate()
	{
		// Multi-source relaxation for each channel. Because all light values live in
		// [0, 15] and every step costs zero or one, the values can be settled with a
		// descending bucket sweep. Each cell is appended to a bucket only when its
		// value improves, so the whole pass is close to linear in the volume size.
		const size_t noOfVoxels = m_light.size();
		const size_t strideY = static_cast<size_t>(m_width);
		const size_t strideZ = static_cast<size_t>(m_width) * static_cast<size_t>(m_height);

		std::vector<std::deque<size_t> > buckets(16);

		for (int channel = 0; channel < 4; ++channel)
		{
			for (size_t i = 0; i < 16; ++i)
			{
				buckets[i].clear();
			}
			for (size_t i = 0; i < noOfVoxels; ++i)
			{
				const uint8_t value = getChannel(i, channel);
				if (value > 0)
				{
					buckets[value].push_back(i);
				}
			}

			for (int value = 15; value >= 1; --value)
			{
				std::deque<size_t>& bucket = buckets[value];
				while (!bucket.empty())
				{
					const size_t index = bucket.front();
					bucket.pop_front();
					if (getChannel(index, channel) != value)
					{
						continue; // Superseded by a brighter value.
					}

					const int32_t localX = static_cast<int32_t>(index % static_cast<size_t>(m_width));
					const int32_t localY = static_cast<int32_t>((index / static_cast<size_t>(m_width)) % static_cast<size_t>(m_height));
					const int32_t localZ = static_cast<int32_t>(index / strideZ);

					for (int direction = 0; direction < 6; ++direction)
					{
						const int32_t nx = localX + LightVolumeImpl::neighbourDeltaX[direction];
						const int32_t ny = localY + LightVolumeImpl::neighbourDeltaY[direction];
						const int32_t nz = localZ + LightVolumeImpl::neighbourDeltaZ[direction];
						if (nx < 0 || nx >= m_width || ny < 0 || ny >= m_height || nz < 0 || nz >= m_depth)
						{
							continue;
						}

						const size_t neighbour = static_cast<size_t>(nx) + static_cast<size_t>(ny) * strideY + static_cast<size_t>(nz) * strideZ;
						if (m_opaque[neighbour])
						{
							continue; // Opaque voxels never receive propagated light.
						}

						const int candidate = value - LightVolumeImpl::stepCost(channel, LightVolumeImpl::neighbourDeltaY[direction]);
						if (candidate > getChannel(neighbour, channel))
						{
							setChannel(neighbour, channel, static_cast<uint8_t>(candidate));
							buckets[candidate].push_back(neighbour);
						}
					}
				}
			}
		}
	}

	template <typename VolumeType>
	LightValue LightVolume<VolumeType>::getLight(int32_t x, int32_t y, int32_t z) const
	{
		if (!m_region.containsPoint(x, y, z))
		{
			return LightValue();
		}
		const uint16_t packed = m_light[indexOf(x, y, z)];
		return LightValue(static_cast<uint8_t>(packed & 0x0F),
			static_cast<uint8_t>((packed >> 4) & 0x0F),
			static_cast<uint8_t>((packed >> 8) & 0x0F),
			static_cast<uint8_t>((packed >> 12) & 0x0F));
	}

	template <typename VolumeType>
	LightValue LightVolume<VolumeType>::getLight(const Vector3DInt32& pos) const
	{
		return getLight(pos.getX(), pos.getY(), pos.getZ());
	}

	template <typename VolumeType>
	uint8_t LightVolume<VolumeType>::getLight(int32_t x, int32_t y, int32_t z, LightChannel channel) const
	{
		return LightVolumeImpl::channelOf(getLight(x, y, z), channel);
	}

	template <typename VolumeType>
	std::vector<Vector3DInt32> LightVolume<VolumeType>::apply(const std::vector<LightEdit>& changes)
	{
		m_stats = LightVolumeStats();

		// Collapse duplicate coordinates, keeping the final edit for each one. A sorted
		// map gives both "last wins" and a deterministic processing order.
		std::map<std::tuple<int32_t, int32_t, int32_t>, uint8_t> finalEdits;
		for (size_t i = 0; i < changes.size(); ++i)
		{
			const LightEdit& edit = changes[i];
			finalEdits[std::make_tuple(edit.x, edit.y, edit.z)] = edit.material;
		}

		// Apply the material changes to the volume and the cached material state. No
		// work happens for a batch in which nothing actually changes.
		std::vector<size_t> changedIndices;
		changedIndices.reserve(finalEdits.size());

		for (std::map<std::tuple<int32_t, int32_t, int32_t>, uint8_t>::const_iterator it = finalEdits.begin(); it != finalEdits.end(); ++it)
		{
			const int32_t x = std::get<0>(it->first);
			const int32_t y = std::get<1>(it->first);
			const int32_t z = std::get<2>(it->first);

			if (!m_region.containsPoint(x, y, z))
			{
				continue;
			}

			const uint8_t oldMaterial = static_cast<uint8_t>(m_volume->getVoxel(x, y, z));
			++m_stats.materialReads;
			if (oldMaterial == it->second)
			{
				continue;
			}

			const size_t index = indexOf(x, y, z);
			m_volume->setVoxel(x, y, z, static_cast<VoxelType>(it->second));
			m_material[index] = it->second;
			m_opaque[index] = (it->second == 0 || !m_materials[it->second].opaque) ? 0 : 1;
			changedIndices.push_back(index);
		}

		if (changedIndices.empty())
		{
			return std::vector<Vector3DInt32>();
		}

		// Snapshot the full pre-batch light value of every edited voxel. Further
		// snapshots are taken lazily as propagation touches additional voxels.
		std::unordered_map<size_t, LightValue> oldValues;
		oldValues.reserve(changedIndices.size() * 2);
		for (size_t i = 0; i < changedIndices.size(); ++i)
		{
			const uint16_t packed = m_light[changedIndices[i]];
			oldValues[changedIndices[i]] = LightValue(static_cast<uint8_t>(packed & 0x0F),
				static_cast<uint8_t>((packed >> 4) & 0x0F),
				static_cast<uint8_t>((packed >> 8) & 0x0F),
				static_cast<uint8_t>((packed >> 12) & 0x0F));
		}

		const size_t strideY = static_cast<size_t>(m_width);
		const size_t strideZ = static_cast<size_t>(m_width) * static_cast<size_t>(m_height);

		std::deque<std::pair<size_t, uint8_t> > removalQueue;
		std::deque<size_t> additionQueue;

		for (int channel = 0; channel < 4; ++channel)
		{
			removalQueue.clear();
			additionQueue.clear();

			// Hook recording the pre-batch value of any voxel the update touches.
			auto assign = [&](size_t index, uint8_t value) -> bool
			{
				if (getChannel(index, channel) == value)
				{
					return false;
				}
				if (oldValues.find(index) == oldValues.end())
				{
					const uint16_t packed = m_light[index];
					oldValues[index] = LightValue(static_cast<uint8_t>(packed & 0x0F),
						static_cast<uint8_t>((packed >> 4) & 0x0F),
						static_cast<uint8_t>((packed >> 8) & 0x0F),
						static_cast<uint8_t>((packed >> 12) & 0x0F));
				}
				setChannel(index, channel, value);
				return true;
			};

			auto visitNeighbours = [&](size_t index, const std::function<void(size_t, int)>& fn)
			{
				const int32_t localX = static_cast<int32_t>(index % static_cast<size_t>(m_width));
				const int32_t localY = static_cast<int32_t>((index / static_cast<size_t>(m_width)) % static_cast<size_t>(m_height));
				const int32_t localZ = static_cast<int32_t>(index / strideZ);
				for (int direction = 0; direction < 6; ++direction)
				{
					const int32_t nx = localX + LightVolumeImpl::neighbourDeltaX[direction];
					const int32_t ny = localY + LightVolumeImpl::neighbourDeltaY[direction];
					const int32_t nz = localZ + LightVolumeImpl::neighbourDeltaZ[direction];
					if (nx < 0 || nx >= m_width || ny < 0 || ny >= m_height || nz < 0 || nz >= m_depth)
					{
						continue;
					}
					const size_t neighbour = static_cast<size_t>(nx) + static_cast<size_t>(ny) * strideY + static_cast<size_t>(nz) * strideZ;
					fn(neighbour, direction);
				}
			};

			// Seed both queues from every edited voxel and the voxels around it.
			for (size_t i = 0; i < changedIndices.size(); ++i)
			{
				const size_t index = changedIndices[i];
				const LightValue& previous = oldValues[index];
				const uint8_t oldValue = LightVolumeImpl::channelOf(previous, channel);
				const uint8_t newSource = sourceAt(index, channel);

				if (oldValue > newSource)
				{
					assign(index, newSource);
					removalQueue.push_back(std::make_pair(index, oldValue));
					if (newSource > 0)
					{
						additionQueue.push_back(index);
					}
				}
				else if (newSource > oldValue)
				{
					assign(index, newSource);
					additionQueue.push_back(index);
				}
				else if (newSource > 0)
				{
					additionQueue.push_back(index);
				}

				visitNeighbours(index, [&](size_t neighbour, int)
				{
					if (getChannel(neighbour, channel) > 0)
					{
						additionQueue.push_back(neighbour);
					}
				});
			}

			// Removal pass: erase everything which was sustained by a source that has now
			// gone away (or by a voxel which has become opaque).
			while (!removalQueue.empty())
			{
				const size_t index = removalQueue.front().first;
				const uint8_t oldValue = removalQueue.front().second;
				removalQueue.pop_front();
				++m_stats.queuePops;

				if (getChannel(index, channel) > oldValue)
				{
					continue; // A brighter value has already taken over this voxel.
				}

				// The stored value is already the reduced source value (usually zero). Neighbours
				// which were only sustained by the old value are identified from the value that
				// was being removed, so we deliberately keep using oldValue here.

				visitNeighbours(index, [&](size_t neighbour, int direction)
				{
					const uint8_t neighbourValue = getChannel(neighbour, channel);
					if (!m_opaque[neighbour])
					{
						if (neighbourValue == 0)
						{
							return;
						}

						const int candidate = static_cast<int>(oldValue) - LightVolumeImpl::stepCost(channel, LightVolumeImpl::neighbourDeltaY[direction]);
						if (static_cast<int>(neighbourValue) <= candidate)
						{
							const uint8_t neighbourSource = sourceAt(neighbour, channel);
							assign(neighbour, neighbourSource);
							removalQueue.push_back(std::make_pair(neighbour, neighbourValue));
							if (neighbourSource > 0)
							{
								additionQueue.push_back(neighbour);
							}
							return;
						}
					}
					else if (neighbourValue == 0)
					{
						return;
					}

					// The neighbour either sustains itself independently (or is an opaque
					// emitter) and may refill the area which was just cleared.
					additionQueue.push_back(neighbour);
				});
			}

			// Addition pass: relax the new sources (and any surviving sources bordering the
			// cleared area) back through the affected neighbourhood.
			while (!additionQueue.empty())
			{
				const size_t index = additionQueue.front();
				additionQueue.pop_front();
				++m_stats.queuePops;

				const uint8_t value = getChannel(index, channel);
				if (value == 0)
				{
					continue;
				}

				visitNeighbours(index, [&](size_t neighbour, int direction)
				{
					if (m_opaque[neighbour])
					{
						return; // Cannot receive propagated light.
					}
					const int candidate = static_cast<int>(value) - LightVolumeImpl::stepCost(channel, LightVolumeImpl::neighbourDeltaY[direction]);
					if (candidate > static_cast<int>(getChannel(neighbour, channel)))
					{
						assign(neighbour, static_cast<uint8_t>(candidate));
						additionQueue.push_back(neighbour);
					}
				});
			}
		}

		// Report exactly the voxels whose final value differs from their pre-batch value.
		std::vector<Vector3DInt32> changedPositions;
		for (std::unordered_map<size_t, LightValue>::const_iterator it = oldValues.begin(); it != oldValues.end(); ++it)
		{
			const uint16_t packed = m_light[it->first];
			const LightValue current(static_cast<uint8_t>(packed & 0x0F),
				static_cast<uint8_t>((packed >> 4) & 0x0F),
				static_cast<uint8_t>((packed >> 8) & 0x0F),
				static_cast<uint8_t>((packed >> 12) & 0x0F));
			if (current != it->second)
			{
				changedPositions.push_back(positionOf(it->first));
			}
		}

		std::sort(changedPositions.begin(), changedPositions.end(), LexicographicLess());
		changedPositions.erase(std::unique(changedPositions.begin(), changedPositions.end()), changedPositions.end());
		return changedPositions;
	}
}
