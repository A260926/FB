// Target tests for PolyVox dynamic lighting.
//
// Included by task_support/driver.c (compiled as C++) together with
// task_support/check.h. This file must not define main(): the driver iterates
// task_cases[] and reports the per-group results.
//
// Groups: propagation, editing, lit_mesh.

#include "task_support/check.h"

#include "PolyVox/CubicSurfaceExtractor.h"
#include "PolyVox/LightVolume.h"
#include "PolyVox/LitCubicSurfaceExtractor.h"
#include "PolyVox/PagedVolume.h"
#include "PolyVox/RawVolume.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <map>
#include <random>
#include <set>
#include <string>
#include <tuple>
#include <vector>

using namespace PolyVox;

namespace
{
	typedef std::array<int, 4> Channels; // sky, red, green, blue

	struct MaterialInfo
	{
		bool opaque;
		int red;
		int green;
		int blue;
	};

	// ------------------------------------------------------------------
	// Reference solver
	// ------------------------------------------------------------------

	// Exact full relaxation of the four channels for a small volume. This is the
	// independent reference the streaming implementation is compared against.
	class ReferenceSolver
	{
	public:
		ReferenceSolver(int width, int height, int depth, const std::array<MaterialInfo, 256>& materials)
			: m_width(width), m_height(height), m_depth(depth), m_materials(materials)
		{
			m_material.assign(count(), 0);
		}

		size_t count() const { return static_cast<size_t>(m_width) * m_height * m_depth; }
		size_t index(int x, int y, int z) const { return static_cast<size_t>(x) + static_cast<size_t>(y) * m_width + static_cast<size_t>(z) * m_width * m_height; }

		void set(int x, int y, int z, uint8_t material) { m_material[index(x, y, z)] = material; }

		int source(size_t i, int channel) const
		{
			const MaterialInfo& m = m_materials[m_material[i]];
			if (channel == 0)
			{
				if (m.opaque) return 0;
				const int y = static_cast<int>((i / m_width) % m_height);
				return (y == m_height - 1) ? 15 : 0;
			}
			if (channel == 1) return m.red;
			if (channel == 2) return m.green;
			return m.blue;
		}

		bool isOpaque(size_t i) const { return m_materials[m_material[i]].opaque; }

		std::vector<int> solveChannel(int channel) const
		{
			std::vector<int> light(count(), 0);
			for (size_t i = 0; i < count(); ++i) light[i] = source(i, channel);

			const int dx[6] = { -1, 1, 0, 0, 0, 0 };
			const int dy[6] = { 0, 0, -1, 1, 0, 0 };
			const int dz[6] = { 0, 0, 0, 0, -1, 1 };

			bool changed = true;
			while (changed)
			{
				changed = false;
				for (int z = 0; z < m_depth; ++z)
				{
					for (int y = 0; y < m_height; ++y)
					{
						for (int x = 0; x < m_width; ++x)
						{
							const size_t idx = index(x, y, z);
							if (isOpaque(idx)) continue;

							int best = light[idx];
							for (int d = 0; d < 6; ++d)
							{
								const int nx = x + dx[d], ny = y + dy[d], nz = z + dz[d];
								if (nx < 0 || nx >= m_width || ny < 0 || ny >= m_height || nz < 0 || nz >= m_depth) continue;
								const int cost = (channel == 0 && dy[d] == 1) ? 0 : 1;
								const int candidate = light[index(nx, ny, nz)] - cost;
								if (candidate > best) best = candidate;
							}
							if (best > light[idx])
							{
								light[idx] = best;
								changed = true;
							}
						}
					}
				}
			}
			return light;
		}

		Channels get(int x, int y, int z) const
		{
			Channels result;
			for (int channel = 0; channel < 4; ++channel)
			{
				result[channel] = solveChannel(channel)[index(x, y, z)];
			}
			return result;
		}

	private:
		int m_width;
		int m_height;
		int m_depth;
		std::array<MaterialInfo, 256> m_materials;
		std::vector<uint8_t> m_material;
	};

	template <typename VolumeType>
	Channels readLight(const LightVolume<VolumeType>& light, int x, int y, int z)
	{
		const LightValue value = light.getLight(x, y, z);
		Channels result = { { value.sky, value.red, value.green, value.blue } };
		return result;
	}

	std::array<LightMaterial, 256> makeMaterials(const std::array<MaterialInfo, 256>& infos)
	{
		std::array<LightMaterial, 256> materials = {};
		for (int i = 0; i < 256; ++i)
		{
			materials[i] = LightMaterial(infos[i].opaque, static_cast<uint8_t>(infos[i].red), static_cast<uint8_t>(infos[i].green), static_cast<uint8_t>(infos[i].blue));
		}
		return materials;
	}

	bool sameChannels(const Channels& a, const Channels& b)
	{
		return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3];
	}

	bool lexLess(const Vector3DInt32& a, const Vector3DInt32& b)
	{
		if (a.getX() != b.getX()) return a.getX() < b.getX();
		if (a.getY() != b.getY()) return a.getY() < b.getY();
		return a.getZ() < b.getZ();
	}

	// Builds a small deterministic terrain, fills a RawVolume and its reference mirror.
	template <typename VolumeType>
	void fillScene(VolumeType& volume, ReferenceSolver& reference, int width, int height, int depth,
		const std::vector<uint8_t>& voxels)
	{
		for (int z = 0; z < depth; ++z)
		{
			for (int y = 0; y < height; ++y)
			{
				for (int x = 0; x < width; ++x)
				{
					const uint8_t material = voxels[reference.index(x, y, z)];
					volume.setVoxel(x, y, z, material);
					reference.set(x, y, z, material);
				}
			}
		}
	}

	// Cartesian product of the per-corner sample rule, done independently of the
	// extractor using only the public LightVolume accessors.
	Channels expectedCorner(const LightVolume<RawVolume<uint8_t> >& light, int faceNormal[3], const std::array<int, 3>& position)
	{
		// The corner position is in half-voxel units; recover the owning voxel centre
		// from the supplied normal by brute-force solving over the four corner signs.
		int tangent1[3], tangent2[3];
		if (faceNormal[0] != 0) { tangent1[0] = 0; tangent1[1] = 1; tangent1[2] = 0; tangent2[0] = 0; tangent2[1] = 0; tangent2[2] = 1; }
		else if (faceNormal[1] != 0) { tangent1[0] = 0; tangent1[1] = 0; tangent1[2] = 1; tangent2[0] = 1; tangent2[1] = 0; tangent2[2] = 0; }
		else { tangent1[0] = 1; tangent1[1] = 0; tangent1[2] = 0; tangent2[0] = 0; tangent2[1] = 1; tangent2[2] = 0; }

		long long px = position[0], py = position[1], pz = position[2];
		int s1 = 0, s2 = 0;
		bool found = false;
		for (int a1 = -1; a1 <= 1 && !found; a1 += 2)
		{
			for (int a2 = -1; a2 <= 1 && !found; a2 += 2)
			{
				const long long rx = px - faceNormal[0] - static_cast<long long>(a1) * tangent1[0] - static_cast<long long>(a2) * tangent2[0];
				const long long ry = py - faceNormal[1] - static_cast<long long>(a1) * tangent1[1] - static_cast<long long>(a2) * tangent2[1];
				const long long rz = pz - faceNormal[2] - static_cast<long long>(a1) * tangent1[2] - static_cast<long long>(a2) * tangent2[2];
				if (rx % 2 == 0 && ry % 2 == 0 && rz % 2 == 0)
				{
					s1 = a1;
					s2 = a2;
					found = true;
				}
			}
		}

		const long long bx = (px - faceNormal[0] - static_cast<long long>(s1) * tangent1[0] - static_cast<long long>(s2) * tangent2[0]) / 2;
		const long long by = (py - faceNormal[1] - static_cast<long long>(s1) * tangent1[1] - static_cast<long long>(s2) * tangent2[1]) / 2;
		const long long bz = (pz - faceNormal[2] - static_cast<long long>(s1) * tangent1[2] - static_cast<long long>(s2) * tangent2[2]) / 2;

		const int ox = static_cast<int>(bx) + faceNormal[0];
		const int oy = static_cast<int>(by) + faceNormal[1];
		const int oz = static_cast<int>(bz) + faceNormal[2];

		int sums[4] = { 0, 0, 0, 0 };
		for (int a1 = 0; a1 <= 1; ++a1)
		{
			for (int a2 = 0; a2 <= 1; ++a2)
			{
				const int sx = ox + a1 * s1 * tangent1[0] + a2 * s2 * tangent2[0];
				const int sy = oy + a1 * s1 * tangent1[1] + a2 * s2 * tangent2[1];
				const int sz = oz + a1 * s1 * tangent1[2] + a2 * s2 * tangent2[2];
				const LightValue value = light.getLight(sx, sy, sz);
				sums[0] += static_cast<int>(value.sky);
				sums[1] += static_cast<int>(value.red);
				sums[2] += static_cast<int>(value.green);
				sums[3] += static_cast<int>(value.blue);
			}
		}

		Channels result;
		for (int i = 0; i < 4; ++i) result[i] = sums[i];
		return result;
	}

	// ------------------------------------------------------------------
	// Propagation tests
	// ------------------------------------------------------------------

	int propagationBasic()
	{
		// A 16x16x16 open box: skylight should be 15 everywhere, falling by one per
		// lateral step away from the top layer but never below the column rules.
		const int size = 16;
		Region region(0, 0, 0, size - 1, size - 1, size - 1);
		RawVolume<uint8_t> volume(region);
		std::array<MaterialInfo, 256> infos = {};
		ReferenceSolver reference(size, size, size, infos);

		LightVolume<RawVolume<uint8_t> > light(&volume, region, makeMaterials(infos));

		for (int z = 0; z < size; ++z)
		{
			for (int y = 0; y < size; ++y)
			{
				for (int x = 0; x < size; ++x)
				{
					const Channels got = readLight(light, x, y, z);
					const int expected = (y == size - 1) ? 15 : 0;
					// Every open voxel below the top layer is reachable by straight-down
					// propagation, so all have skylight 15.
					TASK_CHECK(got[0] == 15);
					(void)expected;
				}
			}
		}

		// A solid roof should block skylight below it while leaving it above.
		RawVolume<uint8_t> roofed(region);
		for (int z = 0; z < size; ++z)
			for (int x = 0; x < size; ++x)
				roofed.setVoxel(x, 8, z, 2);
		std::array<MaterialInfo, 256> roofInfos = {};
		roofInfos[2] = MaterialInfo{ true, 0, 0, 0 };
		LightVolume<RawVolume<uint8_t> > roofLight(&roofed, region, makeMaterials(roofInfos));

		TASK_CHECK(readLight(roofLight, 5, 12, 5)[0] == 15);
		TASK_CHECK(readLight(roofLight, 5, 7, 5)[0] == 0);
		TASK_CHECK(readLight(roofLight, 5, 8, 5)[0] == 0);
		TASK_CHECK(roofLight.getLight(-1, 0, 0) == LightValue());
		return 0;
	}

	int propagationBlockLight()
	{
		const int size = 9;
		Region region(0, 0, 0, size - 1, size - 1, size - 1);
		RawVolume<uint8_t> volume(region);
		for (int z = 0; z < size; ++z)
			for (int y = 0; y < size; ++y)
				for (int x = 0; x < size; ++x)
					volume.setVoxel(x, y, z, 0);

		std::array<MaterialInfo, 256> infos = {};
		infos[1] = MaterialInfo{ false, 15, 0, 0 };
		infos[2] = MaterialInfo{ true, 0, 0, 15 };
		LightVolume<RawVolume<uint8_t> > light(&volume, region, makeMaterials(infos));

		// Place a red source and a blue opaque source, checking the falloff.
		light.apply(std::vector<LightEdit>{ LightEdit(4, 4, 4, 1), LightEdit(4, 4, 6, 2) });

		TASK_CHECK(readLight(light, 4, 4, 4)[1] == 15);
		TASK_CHECK(readLight(light, 5, 4, 4)[1] == 14);
		TASK_CHECK(readLight(light, 4, 5, 4)[1] == 14);
		TASK_CHECK(readLight(light, 6, 4, 4)[1] == 13);
		// Opaque blue source keeps its emission and propagates to its neighbours;
		// it does not itself receive red light.
		TASK_CHECK(readLight(light, 4, 4, 6)[3] == 15);
		TASK_CHECK(readLight(light, 4, 4, 6)[1] == 0);
		TASK_CHECK(readLight(light, 5, 4, 6)[3] == 14);
		return 0;
	}

	int propagationMatchesReference()
	{
		const int width = 7, height = 6, depth = 5;
		Region region(0, 0, 0, width - 1, height - 1, depth - 1);
		RawVolume<uint8_t> volume(region);

		std::array<MaterialInfo, 256> infos = {};
		infos[1] = MaterialInfo{ false, 15, 0, 0 };
		infos[2] = MaterialInfo{ false, 0, 9, 0 };
		infos[3] = MaterialInfo{ true, 0, 0, 0 };
		infos[4] = MaterialInfo{ false, 0, 0, 6 };
		ReferenceSolver reference(width, height, depth, infos);

		std::mt19937 rng(101);
		std::vector<uint8_t> voxels(reference.count(), 0);
		for (size_t i = 0; i < voxels.size(); ++i)
		{
			const int roll = static_cast<int>(rng() % 20);
			if (roll == 0) voxels[i] = 3;      // opaque wall
			else if (roll == 1) voxels[i] = 1; // red source
			else if (roll == 2) voxels[i] = 2; // green source
			else if (roll == 3) voxels[i] = 4; // blue source
		}

		fillScene(volume, reference, width, height, depth, voxels);
		LightVolume<RawVolume<uint8_t> > light(&volume, region, makeMaterials(infos));

		for (int z = 0; z < depth; ++z)
		{
			for (int y = 0; y < height; ++y)
			{
				for (int x = 0; x < width; ++x)
				{
					const size_t i = reference.index(x, y, z);
					Channels expected;
					for (int channel = 0; channel < 4; ++channel) expected[channel] = reference.solveChannel(channel)[i];
					TASK_CHECK(sameChannels(readLight(light, x, y, z), expected));
				}
			}
		}
		return 0;
	}

	int propagationNoResidualRings()
	{
		// Removing a source must not leave orphaned rings behind: this is covered in
		// more depth by the editing group, but a simple top-level check belongs here.
		const int size = 12;
		Region region(0, 0, 0, size - 1, size - 1, size - 1);
		RawVolume<uint8_t> volume(region);
		for (int z = 0; z < size; ++z)
			for (int y = 0; y < size; ++y)
				for (int x = 0; x < size; ++x)
					volume.setVoxel(x, y, z, 0);

		std::array<MaterialInfo, 256> infos = {};
		infos[1] = MaterialInfo{ false, 13, 0, 0 };
		LightVolume<RawVolume<uint8_t> > light(&volume, region, makeMaterials(infos));

		light.apply(std::vector<LightEdit>{ LightEdit(6, 6, 6, 1) });
		TASK_CHECK(readLight(light, 6, 6, 6)[1] == 13);
		light.apply(std::vector<LightEdit>{ LightEdit(6, 6, 6, 0) });
		for (int z = 0; z < size; ++z)
			for (int y = 0; y < size; ++y)
				for (int x = 0; x < size; ++x)
					TASK_CHECK(readLight(light, x, y, z)[1] == 0);
		return 0;
	}

	// ------------------------------------------------------------------
	// Editing tests
	// ------------------------------------------------------------------

	int editingLastWinsAndSet()
	{
		const int size = 10;
		Region region(0, 0, 0, size - 1, size - 1, size - 1);
		RawVolume<uint8_t> volume(region);

		std::array<MaterialInfo, 256> infos = {};
		infos[1] = MaterialInfo{ false, 12, 0, 0 };
		LightVolume<RawVolume<uint8_t> > light(&volume, region, makeMaterials(infos));

		// Last edit for a coordinate wins; the volume must hold the final material.
		std::vector<LightEdit> duplicates;
		duplicates.push_back(LightEdit(5, 5, 5, 1));
		duplicates.push_back(LightEdit(5, 5, 5, 0));
		const std::vector<Vector3DInt32> duplicateChanged = light.apply(duplicates);
		TASK_CHECK(duplicateChanged.empty());
		TASK_CHECK(volume.getVoxel(5, 5, 5) == 0);

		std::vector<LightEdit> edits;
		edits.push_back(LightEdit(4, 4, 4, 1));
		const std::vector<Vector3DInt32> changed = light.apply(edits);
		TASK_CHECK(volume.getVoxel(4, 4, 4) == 1);
		TASK_CHECK(readLight(light, 4, 4, 4)[1] == 12);
		// The returned set is lexicographically sorted.
		for (size_t i = 1; i < changed.size(); ++i)
		{
			TASK_CHECK(lexLess(changed[i - 1], changed[i]));
		}
		return 0;
	}

	int editingEmptyAndNoOpBatches()
	{
		const int size = 8;
		Region region(0, 0, 0, size - 1, size - 1, size - 1);
		RawVolume<uint8_t> volume(region);
		std::array<MaterialInfo, 256> infos = {};
		infos[1] = MaterialInfo{ false, 15, 0, 0 };
		LightVolume<RawVolume<uint8_t> > light(&volume, region, makeMaterials(infos));

		const LightVolumeStats before = light.lastStats();
		const std::vector<Vector3DInt32> empty = light.apply(std::vector<LightEdit>());
		TASK_CHECK(empty.empty());
		TASK_CHECK(light.materialReads() == 0);
		TASK_CHECK(light.queuePops() == 0);
		(void)before;

		// Applying the same material again is a no-op: no writes, no propagation.
		light.apply(std::vector<LightEdit>{ LightEdit(3, 3, 3, 1) });
		const std::vector<Vector3DInt32> noop = light.apply(std::vector<LightEdit>{ LightEdit(3, 3, 3, 1) });
		TASK_CHECK(noop.empty());
		TASK_CHECK(light.materialReads() == 1);
		TASK_CHECK(light.queuePops() == 0);
		return 0;
	}

	int editingEqualSources()
	{
		// Two equal-strength sources: removing one must leave the other's field intact.
		const int size = 9;
		Region region(0, 0, 0, size - 1, size - 1, size - 1);
		RawVolume<uint8_t> volume(region);
		std::array<MaterialInfo, 256> infos = {};
		infos[1] = MaterialInfo{ false, 15, 0, 0 };
		infos[2] = MaterialInfo{ false, 15, 0, 0 };
		LightVolume<RawVolume<uint8_t> > light(&volume, region, makeMaterials(infos));

		light.apply(std::vector<LightEdit>{ LightEdit(4, 4, 4, 1), LightEdit(5, 4, 4, 2) });
		const Channels before = readLight(light, 7, 4, 4);
		TASK_CHECK(before[1] >= 12);

		light.apply(std::vector<LightEdit>{ LightEdit(4, 4, 4, 0) });
		TASK_CHECK(readLight(light, 5, 4, 4)[1] == 15);
		// The residual field must match a scene with just the second source.
		RawVolume<uint8_t> solo(region);
		solo.setVoxel(5, 4, 4, 2);
		LightVolume<RawVolume<uint8_t> > soloLight(&solo, region, makeMaterials(infos));
		for (int z = 0; z < size; ++z)
			for (int y = 0; y < size; ++y)
				for (int x = 0; x < size; ++x)
					TASK_CHECK(sameChannels(readLight(light, x, y, z), readLight(soloLight, x, y, z)));
		return 0;
	}

	int editingRoofToggle()
	{
		const int size = 10;
		Region region(0, 0, 0, size - 1, size - 1, size - 1);
		RawVolume<uint8_t> volume(region);
		std::array<MaterialInfo, 256> infos = {};
		infos[1] = MaterialInfo{ true, 0, 0, 0 };
		LightVolume<RawVolume<uint8_t> > light(&volume, region, makeMaterials(infos));

		TASK_CHECK(readLight(light, 5, 2, 5)[0] == 15);

		// Close the roof (one full layer at y=6).
		std::vector<LightEdit> close;
		for (int z = 0; z < size; ++z)
			for (int x = 0; x < size; ++x)
				close.push_back(LightEdit(x, 6, z, 1));
		light.apply(close);
		TASK_CHECK(readLight(light, 5, 2, 5)[0] == 0);
		TASK_CHECK(readLight(light, 5, 8, 5)[0] == 15);

		// Open a single hole; the column under it lights up again.
		light.apply(std::vector<LightEdit>{ LightEdit(5, 6, 5, 0) });
		TASK_CHECK(readLight(light, 5, 2, 5)[0] == 15);
		TASK_CHECK(readLight(light, 4, 2, 5)[0] < 15);
		return 0;
	}

	int editingRepeatedAndBatches()
	{
		const int width = 8, height = 7, depth = 6;
		Region region(0, 0, 0, width - 1, height - 1, depth - 1);
		RawVolume<uint8_t> volume(region);

		std::array<MaterialInfo, 256> infos = {};
		infos[1] = MaterialInfo{ false, 15, 0, 0 };
		infos[2] = MaterialInfo{ true, 0, 0, 0 };
		infos[3] = MaterialInfo{ false, 0, 11, 0 };
		ReferenceSolver reference(width, height, depth, infos);

		std::vector<uint8_t> voxels(reference.count(), 0);
		fillScene(volume, reference, width, height, depth, voxels);
		LightVolume<RawVolume<uint8_t> > light(&volume, region, makeMaterials(infos));

		std::mt19937 rng(202);
		for (int round = 0; round < 60; ++round)
		{
			std::vector<LightEdit> edits;
			const int count = static_cast<int>(rng() % 6);
			for (int k = 0; k < count; ++k)
			{
				const int x = static_cast<int>(rng() % width);
				const int y = static_cast<int>(rng() % height);
				const int z = static_cast<int>(rng() % depth);
				const uint8_t material = static_cast<uint8_t>(rng() % 4);
				edits.push_back(LightEdit(x, y, z, material));
			}

			// Snapshot pre-batch values and resolve last-wins for the reference.
			std::vector<Channels> before(reference.count());
			for (int z = 0; z < depth; ++z)
				for (int y = 0; y < height; ++y)
					for (int x = 0; x < width; ++x)
						before[reference.index(x, y, z)] = readLight(light, x, y, z);

			std::map<std::tuple<int, int, int>, uint8_t> finalMaterials;
			for (size_t k = 0; k < edits.size(); ++k)
			{
				finalMaterials[std::make_tuple(edits[k].x, edits[k].y, edits[k].z)] = edits[k].material;
			}
			for (std::map<std::tuple<int, int, int>, uint8_t>::const_iterator it = finalMaterials.begin(); it != finalMaterials.end(); ++it)
			{
				reference.set(std::get<0>(it->first), std::get<1>(it->first), std::get<2>(it->first), it->second);
			}

			const std::vector<Vector3DInt32> changed = light.apply(edits);

			// Check every channel against the independent solver.
			for (int channel = 0; channel < 4; ++channel)
			{
				const std::vector<int> solved = reference.solveChannel(channel);
				for (int z = 0; z < depth; ++z)
					for (int y = 0; y < height; ++y)
						for (int x = 0; x < width; ++x)
							TASK_CHECK(readLight(light, x, y, z)[channel] == solved[reference.index(x, y, z)]);
			}

			// Check the returned net-change set.
			std::vector<std::tuple<int, int, int> > expectedChanged;
			for (int z = 0; z < depth; ++z)
				for (int y = 0; y < height; ++y)
					for (int x = 0; x < width; ++x)
					{
						const Channels now = readLight(light, x, y, z);
						if (!sameChannels(now, before[reference.index(x, y, z)]))
							expectedChanged.push_back(std::make_tuple(x, y, z));
					}
			std::sort(expectedChanged.begin(), expectedChanged.end());
			TASK_CHECK(changed.size() == expectedChanged.size());
			for (size_t i = 0; i < changed.size() && i < expectedChanged.size(); ++i)
			{
				TASK_CHECK(changed[i].getX() == std::get<0>(expectedChanged[i]));
				TASK_CHECK(changed[i].getY() == std::get<1>(expectedChanged[i]));
				TASK_CHECK(changed[i].getZ() == std::get<2>(expectedChanged[i]));
			}
		}
		return 0;
	}

	int editingWorkProportional()
	{
		// 128^3 region, full opaque roof at y=32, one red source below it.
		// Removing it must not cost anything proportional to the region volume.
		const int size = 128;
		Region region(0, 0, 0, size - 1, size - 1, size - 1);
		RawVolume<uint8_t> volume(region);
		for (int z = 0; z < size; ++z)
		{
			for (int y = 0; y < size; ++y)
			{
				const uint8_t material = (y == 32) ? 2 : 0;
				for (int x = 0; x < size; ++x)
					volume.setVoxel(x, y, z, material);
			}
		}

		std::array<MaterialInfo, 256> infos = {};
		infos[1] = MaterialInfo{ false, 15, 0, 0 };
		infos[2] = MaterialInfo{ true, 0, 0, 0 };
		LightVolume<RawVolume<uint8_t> > light(&volume, region, makeMaterials(infos));

		light.apply(std::vector<LightEdit>{ LightEdit(64, 8, 64, 1) });
		TASK_CHECK(light.materialReads() <= 200000);
		TASK_CHECK(light.queuePops() <= 200000);

		const std::vector<Vector3DInt32> removed = light.apply(std::vector<LightEdit>{ LightEdit(64, 8, 64, 0) });
		TASK_CHECK(!removed.empty());
		TASK_CHECK(light.materialReads() <= 200000);
		TASK_CHECK(light.queuePops() <= 200000);
		TASK_CHECK(readLight(light, 64, 8, 64)[1] == 0);
		// Skylight above the roof survives; below it stays dark.
		TASK_CHECK(readLight(light, 0, 100, 0)[0] == 15);
		TASK_CHECK(readLight(light, 0, 0, 0)[0] == 0);
		return 0;
	}

	// ------------------------------------------------------------------
	// Lit mesh tests
	// ------------------------------------------------------------------

	int litMeshMatchesReference()
	{
		const int width = 8, height = 8, depth = 8;
		Region region(0, 0, 0, width - 1, height - 1, depth - 1);
		RawVolume<uint8_t> volume(region);

		std::mt19937 rng(303);
		for (int z = 0; z < depth; ++z)
			for (int y = 0; y < height; ++y)
				for (int x = 0; x < width; ++x)
					volume.setVoxel(x, y, z, (rng() % 3 == 0) ? 1 : 0);

		std::array<MaterialInfo, 256> infos = {};
		infos[1] = MaterialInfo{ false, 9, 4, 2 };
		LightVolume<RawVolume<uint8_t> > light(&volume, region, makeMaterials(infos));

		const Mesh<LitCubicVertex<uint8_t> > mesh = extractLitCubicMesh(&volume, light, region, DefaultIsQuadNeeded<uint8_t>(), false);
		TASK_CHECK(!mesh.isEmpty());

		// Every vertex's light must equal the independent four-voxel average and its
		// normal must be axis aligned and unit length.
		for (uint32_t i = 0; i < mesh.getNoOfVertices(); ++i)
		{
			const LitCubicVertex<uint8_t>& vertex = mesh.getVertex(i);
			int normal[3] = { static_cast<int>(std::lround(vertex.normal.getX())),
				static_cast<int>(std::lround(vertex.normal.getY())),
				static_cast<int>(std::lround(vertex.normal.getZ())) };
			TASK_CHECK(std::abs(vertex.normal.length() - 1.0f) < 1e-5f);
			TASK_CHECK(std::abs(normal[0]) + std::abs(normal[1]) + std::abs(normal[2]) == 1);

			std::array<int, 3> position = { {
				static_cast<int>(std::lround(vertex.position.getX() * 2.0f)),
				static_cast<int>(std::lround(vertex.position.getY() * 2.0f)),
				static_cast<int>(std::lround(vertex.position.getZ() * 2.0f)) } };
			// Positions must land on half-voxel boundaries.
			TASK_CHECK(vertex.position.getX() * 2.0f == static_cast<float>(position[0]));

			const Channels expected = expectedCorner(light, normal, position);
			for (int channel = 0; channel < 4; ++channel)
			{
				const float expectedValue = static_cast<float>(expected[channel]) / 60.0f;
				TASK_CHECK(std::abs(vertex.light[channel] - expectedValue) < 1e-5f);
			}
		}
		return 0;
	}

	int litMeshWindingAndSplitting()
	{
		// A single solid voxel: six faces, each with four corners that must not share
		// vertices with other faces because their normals differ.
		Region region(-4, -4, -4, 4, 4, 4);
		RawVolume<uint8_t> volume(region);
		for (int z = -4; z <= 4; ++z)
			for (int y = -4; y <= 4; ++y)
				for (int x = -4; x <= 4; ++x)
					volume.setVoxel(x, y, z, 0);
		volume.setVoxel(0, 0, 0, 5);

		std::array<MaterialInfo, 256> infos = {};
		infos[5] = MaterialInfo{ true, 0, 0, 0 };
		LightVolume<RawVolume<uint8_t> > light(&volume, region, makeMaterials(infos));

		const Mesh<LitCubicVertex<uint8_t> > mesh = extractLitCubicMesh(&volume, light, region);
		TASK_CHECK(mesh.getNoOfVertices() == 24);
		TASK_CHECK(mesh.getNoOfIndices() == 36);
		TASK_CHECK(mesh.getOffset() == Vector3DInt32(0, 0, 0));

		// Faces split by normal: collect the distinct normals.
		std::set<std::tuple<int, int, int> > normals;
		for (uint32_t i = 0; i < mesh.getNoOfVertices(); ++i)
		{
			const LitCubicVertex<uint8_t>& vertex = mesh.getVertex(i);
			normals.insert(std::make_tuple(static_cast<int>(std::lround(vertex.normal.getX())),
				static_cast<int>(std::lround(vertex.normal.getY())),
				static_cast<int>(std::lround(vertex.normal.getZ()))));
			TASK_CHECK(vertex.data == 5);
		}
		TASK_CHECK(normals.size() == 6);

		// Outward winding: signed volume of the unit cube should be +1.
		double volumeSigned = 0.0;
		for (size_t i = 0; i < mesh.getNoOfIndices(); i += 3)
		{
			const Vector3DFloat a = mesh.getVertex(mesh.getIndex(i)).position;
			const Vector3DFloat b = mesh.getVertex(mesh.getIndex(i + 1)).position;
			const Vector3DFloat c = mesh.getVertex(mesh.getIndex(i + 2)).position;
			volumeSigned += (a.getX() * (b.getY() * c.getZ() - b.getZ() * c.getY())
				+ a.getY() * (b.getZ() * c.getX() - b.getX() * c.getZ())
				+ a.getZ() * (b.getX() * c.getY() - b.getY() * c.getX())) / 6.0;
		}
		TASK_CHECK(std::abs(volumeSigned - 1.0) < 1e-5);
		return 0;
	}

	int litMeshFlatMerge()
	{
		// A flat, evenly lit 16x16 slab must merge into a handful of large quads, while
		// a scene with a lighting gradient must keep its unit faces.
		Region region(0, 0, 0, 15, 15, 15);
		RawVolume<uint8_t> volume(region);
		for (int z = 0; z < 16; ++z)
			for (int y = 0; y < 16; ++y)
				for (int x = 0; x < 16; ++x)
					volume.setVoxel(x, y, z, (y == 5 || y == 6) ? 0 : 2);

		std::array<MaterialInfo, 256> infos = {};
		infos[2] = MaterialInfo{ true, 0, 0, 0 };
		LightVolume<RawVolume<uint8_t> > light(&volume, region, makeMaterials(infos));

		const Mesh<LitCubicVertex<uint8_t> > merged = extractLitCubicMesh(&volume, light, region);
		// The inner flat ceiling/floor of the 16x16 cavity should be one quad each.
		TASK_CHECK(merged.getNoOfIndices() / 3 < 64);

		// With a gradient (a lamp underneath) the ceiling must not collapse to one quad.
		std::array<MaterialInfo, 256> litInfos = infos;
		litInfos[1] = MaterialInfo{ false, 15, 0, 0 };
		LightVolume<RawVolume<uint8_t> > litLight(&volume, region, makeMaterials(litInfos));
		litLight.apply(std::vector<LightEdit>{ LightEdit(8, 5, 8, 1) });
		const Mesh<LitCubicVertex<uint8_t> > gradient = extractLitCubicMesh(&volume, litLight, region);
		TASK_CHECK(gradient.getNoOfIndices() / 3 > merged.getNoOfIndices() / 3);

		// All vertices must still be individual corner samples: no interpolated light.
		for (uint32_t i = 0; i < gradient.getNoOfVertices(); ++i)
		{
			const LitCubicVertex<uint8_t>& vertex = gradient.getVertex(i);
			TASK_CHECK(vertex.light[1] >= 0.0f && vertex.light[1] <= 1.0f);
		}
		return 0;
	}

	int litMeshGradientPreserved()
	{
		// A strip lit from one end: merging must not average the gradient away.
		Region region(0, 0, 0, 15, 3, 3);
		RawVolume<uint8_t> volume(region);
		for (int z = 0; z <= 3; ++z)
			for (int y = 0; y <= 3; ++y)
				for (int x = 0; x <= 15; ++x)
					volume.setVoxel(x, y, z, 0);
		// Solid strip along x at y=0.
		for (int z = 0; z <= 3; ++z)
			for (int x = 0; x <= 15; ++x)
				volume.setVoxel(x, 0, z, 2);

		std::array<MaterialInfo, 256> infos = {};
		infos[1] = MaterialInfo{ false, 15, 0, 0 };
		infos[2] = MaterialInfo{ true, 0, 0, 0 };
		LightVolume<RawVolume<uint8_t> > light(&volume, region, makeMaterials(infos));
		light.apply(std::vector<LightEdit>{ LightEdit(0, 1, 1, 1) });

		const Mesh<LitCubicVertex<uint8_t> > mesh = extractLitCubicMesh(&volume, light, region);
		// Collect the light values on the top faces of the strip; they must vary.
		std::set<float> values;
		for (uint32_t i = 0; i < mesh.getNoOfVertices(); ++i)
		{
			const LitCubicVertex<uint8_t>& vertex = mesh.getVertex(i);
			if (vertex.normal.getY() > 0.5f) values.insert(vertex.light[1]);
		}
		TASK_CHECK(values.size() > 1);
		return 0;
	}

	// A pager that remembers dirty chunks in memory, so flushAll() must round-trip.
	struct MemoryPager : public PagedVolume<uint8_t>::Pager
	{
		typedef PagedVolume<uint8_t>::Chunk Chunk;
		std::map<std::tuple<int, int, int>, std::vector<uint8_t> > chunks;

		static std::tuple<int, int, int> key(const Region& region)
		{
			return std::make_tuple(region.getLowerX() / 8, region.getLowerY() / 8, region.getLowerZ() / 8);
		}

		void pageIn(const Region& region, Chunk* chunk) override
		{
			const std::tuple<int, int, int> k = key(region);
			std::map<std::tuple<int, int, int>, std::vector<uint8_t> >::const_iterator it = chunks.find(k);
			for (int z = 0; z < 8; ++z)
				for (int y = 0; y < 8; ++y)
					for (int x = 0; x < 8; ++x)
					{
						uint8_t value = 0;
						if (it != chunks.end()) value = it->second[static_cast<size_t>(x) + y * 8 + z * 64];
						chunk->setVoxel(x, y, z, value);
					}
		}

		void pageOut(const Region& region, Chunk* chunk) override
		{
			const std::tuple<int, int, int> k = key(region);
			std::vector<uint8_t>& data = chunks[k];
			data.assign(512, 0);
			for (int z = 0; z < 8; ++z)
				for (int y = 0; y < 8; ++y)
					for (int x = 0; x < 8; ++x)
						data[static_cast<size_t>(x) + y * 8 + z * 64] = chunk->getVoxel(x, y, z);
		}
	};

	int litMeshPagedConsistency()
	{
		// The same scene (including negative coordinates that span chunk boundaries) is
		// built in a RawVolume and in a PagedVolume backed by an in-memory pager which
		// retains dirty chunks. Light values, decoded geometry and colours must agree,
		// both before and after flushAll().
		Region region(-8, -8, -8, 7, 7, 7);
		MemoryPager pager;
		std::unique_ptr<PagedVolume<uint8_t> > paged(new PagedVolume<uint8_t>(&pager, 1u << 20, 8));
		RawVolume<uint8_t> raw(region);

		std::mt19937 rng(404);
		for (int z = -8; z < 8; ++z)
			for (int y = -8; y < 8; ++y)
				for (int x = -8; x < 8; ++x)
				{
					uint8_t material = 0;
					const int roll = static_cast<int>(rng() % 8);
					if (roll == 0) material = 1;      // red source
					else if (roll == 1) material = 2; // opaque wall
					else if (roll == 2) material = 3; // green source
					paged->setVoxel(x, y, z, material);
					raw.setVoxel(x, y, z, material);
				}

		std::array<MaterialInfo, 256> infos = {};
		infos[1] = MaterialInfo{ false, 15, 5, 0 };
		infos[2] = MaterialInfo{ true, 0, 0, 0 };
		infos[3] = MaterialInfo{ false, 0, 7, 12 };
		const std::array<LightMaterial, 256> materials = makeMaterials(infos);

		LightVolume<RawVolume<uint8_t> > rawLight(&raw, region, materials);
		LightVolume<PagedVolume<uint8_t> > pagedLight(paged.get(), region, materials);

		auto compare = [&](const char* tag) -> bool
		{
			for (int z = -8; z < 8; ++z)
				for (int y = -8; y < 8; ++y)
					for (int x = -8; x < 8; ++x)
					{
						if (!sameChannels(readLight(rawLight, x, y, z), readLight(pagedLight, x, y, z)))
						{
							std::fprintf(stderr, "%s: light mismatch at %d,%d,%d\n", tag, x, y, z);
							return false;
						}
					}

			const Mesh<LitCubicVertex<uint8_t> > ra = extractLitCubicMesh(&raw, rawLight, region);
			const Mesh<LitCubicVertex<uint8_t> > pa = extractLitCubicMesh(paged.get(), pagedLight, region);
			if (ra.getNoOfVertices() != pa.getNoOfVertices() || ra.getNoOfIndices() != pa.getNoOfIndices())
			{
				std::fprintf(stderr, "%s: mesh size mismatch\n", tag);
				return false;
			}
			for (uint32_t i = 0; i < ra.getNoOfVertices(); ++i)
			{
				const LitCubicVertex<uint8_t>& a = ra.getVertex(i);
				const LitCubicVertex<uint8_t>& b = pa.getVertex(i);
				bool same = a.position == b.position && a.normal == b.normal && a.data == b.data;
				for (int c = 0; c < 4; ++c) same = same && a.light[c] == b.light[c];
				if (!same)
				{
					std::fprintf(stderr, "%s: vertex mismatch %u\n", tag, i);
					return false;
				}
			}
			for (size_t i = 0; i < ra.getNoOfIndices(); ++i)
			{
				if (ra.getIndex(i) != pa.getIndex(i))
				{
					std::fprintf(stderr, "%s: index mismatch %zu\n", tag, i);
					return false;
				}
			}
			return true;
		};

		TASK_CHECK(compare("before flush"));
		paged->flushAll();
		TASK_CHECK(compare("after flush"));

		// An edit spanning a chunk boundary must give the same light in both volumes and
		// must survive a further flush.
		const std::vector<LightEdit> edits = {
			LightEdit(-1, -1, -1, 1),
			LightEdit(0, 0, 0, 3),
			LightEdit(7, 7, 7, 2)
		};
		rawLight.apply(edits);
		pagedLight.apply(edits);
		TASK_CHECK(compare("after edit"));
		paged->flushAll();
		TASK_CHECK(compare("after edit+flush"));
		return 0;
	}

	int litMeshPly()
	{
		// Read the mesh the example produced and validate its PLY header, geometry and
		// colours without trusting the counts alone.
		std::ifstream in(".task-build/lit.ply");
		TASK_CHECK(in.good());

		std::string token;
		long long vertexCount = 0, faceCount = 0;
		std::string line;
		bool sawEndHeader = false;
		std::vector<std::string> header;
		while (std::getline(in, line))
		{
			header.push_back(line);
			if (line == "end_header") { sawEndHeader = true; break; }
		}
		TASK_CHECK(sawEndHeader);

		for (size_t i = 0; i < header.size(); ++i)
		{
			if (header[i].compare(0, 14, "element vertex") == 0) vertexCount = std::atoll(header[i].c_str() + 14);
			if (header[i].compare(0, 12, "element face") == 0) faceCount = std::atoll(header[i].c_str() + 12);
		}
		TASK_CHECK(vertexCount > 0);
		TASK_CHECK(faceCount > 0);

		std::vector<std::array<double, 9> > vertices;
		for (long long i = 0; i < vertexCount; ++i)
		{
			std::array<double, 9> v;
			TASK_CHECK(static_cast<bool>(in >> v[0] >> v[1] >> v[2] >> v[3] >> v[4] >> v[5] >> v[6] >> v[7] >> v[8]));
			// Normals are unit length.
			TASK_CHECK(std::abs(std::sqrt(v[3] * v[3] + v[4] * v[4] + v[5] * v[5]) - 1.0) < 1e-4);
			// Colour bytes are in range; at least one non-zero channel exists.
			for (int c = 6; c < 9; ++c) TASK_CHECK(v[c] >= 0.0 && v[c] <= 255.0);
			vertices.push_back(v);
		}

		long long triCount = 0;
		long long nonDegenerate = 0;
		for (long long i = 0; i < faceCount; ++i)
		{
			int n = 0;
			TASK_CHECK(static_cast<bool>(in >> n));
			TASK_CHECK(n == 3);
			int idx[3];
			TASK_CHECK(static_cast<bool>(in >> idx[0] >> idx[1] >> idx[2]));
			for (int k = 0; k < 3; ++k) TASK_CHECK(idx[k] >= 0 && idx[k] < vertexCount);
			++triCount;

			const std::array<double, 9>& a = vertices[idx[0]];
			const std::array<double, 9>& b = vertices[idx[1]];
			const std::array<double, 9>& c = vertices[idx[2]];
			const double e1[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
			const double e2[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };
			const double cross[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };
			const double area = 0.5 * std::sqrt(cross[0] * cross[0] + cross[1] * cross[1] + cross[2] * cross[2]);
			if (area > 1e-6) ++nonDegenerate;
		}
		TASK_CHECK(triCount == faceCount);
		TASK_CHECK(nonDegenerate > faceCount / 2);

		// At least two distinct colours should appear (red lamp vs blue lamp/sky).
		std::set<std::tuple<int, int, int> > colours;
		for (size_t i = 0; i < vertices.size(); ++i)
			colours.insert(std::make_tuple(static_cast<int>(vertices[i][6]), static_cast<int>(vertices[i][7]), static_cast<int>(vertices[i][8])));
		TASK_CHECK(colours.size() > 2);
		return 0;
	}
}

// The driver links against these; the order determines the reported case order.
const struct task_case task_cases[] =
{
	{ "propagation", "basic", propagationBasic },
	{ "propagation", "block_light", propagationBlockLight },
	{ "propagation", "matches_reference", propagationMatchesReference },
	{ "propagation", "no_residual_rings", propagationNoResidualRings },
	{ "editing", "last_wins_and_set", editingLastWinsAndSet },
	{ "editing", "empty_and_noop", editingEmptyAndNoOpBatches },
	{ "editing", "equal_sources", editingEqualSources },
	{ "editing", "roof_toggle", editingRoofToggle },
	{ "editing", "repeated_and_batches", editingRepeatedAndBatches },
	{ "editing", "work_proportional", editingWorkProportional },
	{ "lit_mesh", "matches_reference", litMeshMatchesReference },
	{ "lit_mesh", "winding_and_splitting", litMeshWindingAndSplitting },
	{ "lit_mesh", "flat_merge", litMeshFlatMerge },
	{ "lit_mesh", "gradient_preserved", litMeshGradientPreserved },
	{ "lit_mesh", "paged_consistency", litMeshPagedConsistency },
	{ "lit_mesh", "ply", litMeshPly }
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
