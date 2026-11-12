// Standalone (no Qt) lifetime and correctness tests for PagedVolume::Sampler.
//
// The tests exercise the invalidation rules introduced to keep a Sampler's cached
// voxel pointer safe when the chunk backing it is evicted by LRU pressure or
// destroyed by flushAll(). Everything here is plain C++ (C++14) so that
// checks/check-sampler-lifetime.sh can build it directly with -fsanitize=address.

#include "PolyVox/MarchingCubesSurfaceExtractor.h"
#include "PolyVox/PagedVolume.h"
#include "PolyVox/RawVolume.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace
{
	typedef uint8_t Voxel;
	typedef PolyVox::PagedVolume<Voxel> PagedVolume;
	typedef PolyVox::RawVolume<Voxel> RawVolume;

	int g_cases = 0;
	int g_failures = 0;

	void checkCase(bool condition, const char* caseName, const char* expression)
	{
		++g_cases;
		if (!condition)
		{
			++g_failures;
			std::printf("FAILED case=%s expression=%s\n", caseName, expression);
		}
	}

#define CHECK(caseName, expr) checkCase((expr), (caseName), #expr)

	// A deterministic density field: a solid ball of radius 5 centred on the origin.
	// Values are in 0..255 so the default Marching Cubes threshold (127) splits them.
	Voxel densityAt(int x, int y, int z)
	{
		const int r2 = x * x + y * y + z * z;
		return (r2 < 25) ? Voxel(0) : Voxel(255);
	}

	// A pager which *stores* what it is given. It never regenerates data procedurally,
	// so a Sampler which keeps looking at a stale in-memory snapshot (or a volume which
	// fails to page a modified chunk back out) is detected. The PageIn path is also
	// aware of edits made through Volume::setVoxel/setVoxel-based paging, so reloaded
	// chunks observe the latest stored bytes.
	// All tests use the default 32-voxel chunk side; the pager helpers below assume it.
	const uint32_t CHUNK_SIDE = 32;
	const uint32_t CHUNK_SIDE_POWER = 5;

	struct ChunkKey
	{
		int32_t x, y, z;
		ChunkKey() : x(0), y(0), z(0) {}
		ChunkKey(int32_t x_, int32_t y_, int32_t z_) : x(x_), y(y_), z(z_) {}
		bool operator<(const ChunkKey& rhs) const
		{
			if (x != rhs.x) return x < rhs.x;
			if (y != rhs.y) return y < rhs.y;
			return z < rhs.z;
		}
	};

	struct StoringPager : PagedVolume::Pager
	{
		// Content of each chunk, keyed by chunk-space coords.
		std::map<ChunkKey, std::vector<Voxel>> storage;
		unsigned pageInCount = 0;
		unsigned pageOutCount = 0;
		unsigned pageInAfterWriteCount = 0;
		bool useConstantFallback = false;

		void pageIn(const PolyVox::Region& region, PagedVolume::Chunk* chunk) override
		{
			++pageInCount;
			const ChunkKey pos(region.getLowerX() >> 5, region.getLowerY() >> 5, region.getLowerZ() >> 5);

			std::map<ChunkKey, std::vector<Voxel>>::iterator it = storage.find(pos);
			if (it != storage.end())
			{
				++pageInAfterWriteCount;
				// Data is stored in Morton order (that is how Chunk stores it), so copy
				// straight through to the chunk's voxel buffer.
				for (uint32_t z = 0; z < 32; ++z)
				{
					for (uint32_t y = 0; y < 32; ++y)
					{
						for (uint32_t x = 0; x < 32; ++x)
						{
							chunk->setVoxel(x, y, z, it->second[chunkIndex(x, y, z)]);
						}
					}
				}
			}
			else if (useConstantFallback)
			{
				for (uint32_t z = 0; z < 32; ++z)
					for (uint32_t y = 0; y < 32; ++y)
						for (uint32_t x = 0; x < 32; ++x)
							chunk->setVoxel(x, y, z, 7);
			}
			else
			{
				// Generate deterministic contents for a chunk we have not seen before.
				for (uint32_t z = 0; z < 32; ++z)
					for (uint32_t y = 0; y < 32; ++y)
						for (uint32_t x = 0; x < 32; ++x)
							chunk->setVoxel(x, y, z, densityAt(region.getLowerX() + int(x), region.getLowerY() + int(y), region.getLowerZ() + int(z)));
			}
		}

		void pageOut(const PolyVox::Region& region, PagedVolume::Chunk* chunk) override
		{
			++pageOutCount;
			const ChunkKey pos(region.getLowerX() >> 5, region.getLowerY() >> 5, region.getLowerZ() >> 5);
			std::vector<Voxel>& data = storage[pos];
			data.resize(32 * 32 * 32);
			for (uint32_t z = 0; z < 32; ++z)
				for (uint32_t y = 0; y < 32; ++y)
					for (uint32_t x = 0; x < 32; ++x)
						data[chunkIndex(x, y, z)] = chunk->getVoxel(x, y, z);
		}

		// Linear (x-major) indexing into the stored chunk snapshot. The Chunk itself
		// uses Morton order, but all access here goes through getVoxel()/setVoxel()
		// which translate coordinates, so the storage layout is our own choice.
		static uint32_t chunkIndex(uint32_t x, uint32_t y, uint32_t z)
		{
			return x + y * CHUNK_SIDE + z * CHUNK_SIDE * CHUNK_SIDE;
		}
	};


	template <typename VolumeType>
	typename VolumeType::VoxelType voxelAt(int x, int y, int z)
	{
		return densityAt(x, y, z);
	}

	// ---------------------------------------------------------------------------------
	// Case 1: both original reproductions return the correct value.
	// ---------------------------------------------------------------------------------
	void testOriginalReproductions()
	{
		{
			StoringPager pager;
			PagedVolume vol(&pager, 1024 * 1024, 32);
			PagedVolume::Sampler sampler(&vol);
			sampler.setPosition(1, 1, 1);
			CHECK("repro-initial", sampler.getVoxel() == densityAt(1, 1, 1));

			// Force the chunk containing (1,1,1) out of the cache by touching many far
			// chunks. The sampler must transparently recover.
			for (int k = 1; k <= 40; ++k)
			{
				vol.getVoxel(32 * k, 0, 0);
			}
			CHECK("repro-evicted", sampler.getVoxel() == densityAt(1, 1, 1));
		}

		{
			StoringPager pager;
			PagedVolume vol(&pager, 1024 * 1024, 32);
			PagedVolume::Sampler sampler(&vol);
			sampler.setPosition(1, 1, 1);
			CHECK("flush-initial", sampler.getVoxel() == densityAt(1, 1, 1));

			vol.flushAll();
			CHECK("flush-rebound", sampler.getVoxel() == densityAt(1, 1, 1));
		}
	}

	// ---------------------------------------------------------------------------------
	// Case 2: interleaved samplers, copies and assignments.
	// ---------------------------------------------------------------------------------
	void testInterleavedSamplers()
	{
		StoringPager pager;
		PagedVolume vol(&pager, 1024 * 1024, 32);

		PagedVolume::Sampler a(&vol);
		PagedVolume::Sampler b(&vol);
		a.setPosition(1, 2, 3);
		b.setPosition(-3, 4, 5);

		const Voxel aExpected = densityAt(1, 2, 3);

		// Move 'b' far away, forcing eviction of chunks (including potentially 'a's).
		b.setPosition(31, 31, 31);
		for (int k = 0; k < 20; ++k)
		{
			b.setPosition(400 + 32 * k, -200, 600);
		}

		CHECK("interleaved-a", a.getVoxel() == aExpected);
		CHECK("interleaved-b", b.getVoxel() == densityAt(620, -200, 600));

		// Both read each other's positions.
		CHECK("interleaved-a-peek-of-b", a.getVoxel() == aExpected);
		b.setPosition(1, 2, 3);
		CHECK("interleaved-b-at-a", b.getVoxel() == aExpected);
	}

	void testCopyAndAssignment()
	{
		StoringPager pager;
		PagedVolume vol(&pager, 1024 * 1024, 32);

		PagedVolume::Sampler original(&vol);
		original.setPosition(5, 6, 7);

		// Copy construction: the copy keeps its own coordinate and must survive the
		// original moving away (and vice versa).
		PagedVolume::Sampler copy(original);
		CHECK("copy-position", copy.getPosition() == PolyVox::Vector3DInt32(5, 6, 7));
		CHECK("copy-value", copy.getVoxel() == densityAt(5, 6, 7));

		original.setPosition(90, 91, 92);
		CHECK("copy-after-original-moved", copy.getVoxel() == densityAt(5, 6, 7));
		CHECK("original-moved", original.getVoxel() == densityAt(90, 91, 92));

		// Assignment: both keep their own positions.
		PagedVolume::Sampler assigned(&vol);
		assigned.setPosition(-1, -2, -3);
		assigned = copy;
		CHECK("assign-copies-position", assigned.getPosition() == PolyVox::Vector3DInt32(5, 6, 7));
		CHECK("assign-value", assigned.getVoxel() == densityAt(5, 6, 7));

		copy.setPosition(11, 12, 13);
		CHECK("assign-independent-of-source", assigned.getVoxel() == densityAt(5, 6, 7));
		CHECK("copy-moved", copy.getVoxel() == densityAt(11, 12, 13));

		// Assignment over a live sampler must not drag along the destination's old state.
		PagedVolume::Sampler overwrite(&vol);
		overwrite.setPosition(200, 200, 200);
		overwrite = original; // original is at (90,91,92)
		CHECK("overwrite-value", overwrite.getVoxel() == densityAt(90, 91, 92));
	}

	// ---------------------------------------------------------------------------------
	// Case 3: movement in every direction across chunk boundaries, including negatives.
	// ---------------------------------------------------------------------------------
	void testMovesAndBoundaries()
	{
		StoringPager pager;
		PagedVolume vol(&pager, 1024 * 1024, 32);

		// Start at a negative coordinate near a chunk boundary so that moving negative
		// crosses into the {-1 chunk} regions.
		const int startX = -1;
		const int startY = 0;
		const int startZ = 1;

		PagedVolume::Sampler sampler(&vol);
		sampler.setPosition(startX, startY, startZ);

		int x = startX, y = startY, z = startZ;
		CHECK("move-start", sampler.getVoxel() == densityAt(x, y, z));

		for (int i = 0; i < 40; ++i)
		{
			sampler.movePositiveX();
			++x;
			CHECK("move-pos-x", sampler.getVoxel() == densityAt(x, y, z));
		}

		for (int i = 0; i < 80; ++i)
		{
			sampler.moveNegativeX();
			--x;
			CHECK("move-neg-x", sampler.getVoxel() == densityAt(x, y, z));
		}

		for (int i = 0; i < 40; ++i)
		{
			sampler.movePositiveY();
			++y;
			CHECK("move-pos-y", sampler.getVoxel() == densityAt(x, y, z));
		}

		for (int i = 0; i < 80; ++i)
		{
			sampler.moveNegativeY();
			--y;
			CHECK("move-neg-y", sampler.getVoxel() == densityAt(x, y, z));
		}

		for (int i = 0; i < 40; ++i)
		{
			sampler.movePositiveZ();
			++z;
			CHECK("move-pos-z", sampler.getVoxel() == densityAt(x, y, z));
		}

		for (int i = 0; i < 80; ++i)
		{
			sampler.moveNegativeZ();
			--z;
			CHECK("move-neg-z", sampler.getVoxel() == densityAt(x, y, z));
		}

		// Now sweep a whole chunk in each axis while continuously reading, forcing
		// in-chunk deltas and boundary re-resolutions to alternate.
		sampler.setPosition(0, 0, 0);
		for (int i = 0; i < 64; ++i) { sampler.movePositiveX(); (void)sampler.getVoxel(); }
		for (int i = 0; i < 128; ++i) { sampler.moveNegativeX(); (void)sampler.getVoxel(); }
		for (int i = 0; i < 64; ++i) { sampler.movePositiveY(); (void)sampler.getVoxel(); }
		for (int i = 0; i < 128; ++i) { sampler.moveNegativeY(); (void)sampler.getVoxel(); }
		for (int i = 0; i < 64; ++i) { sampler.movePositiveZ(); (void)sampler.getVoxel(); }
		for (int i = 0; i < 128; ++i) { sampler.moveNegativeZ(); (void)sampler.getVoxel(); }

		// Negative-coordinate Morton addressing check: compare a sampler's value
		// against Volume::getVoxel and a RawVolume at the same negative positions.
		RawVolume raw(PolyVox::Region(-40, -40, -40, 40, 40, 40));
		for (int z2 = -40; z2 <= 40; ++z2)
			for (int y2 = -40; y2 <= 40; ++y2)
				for (int x2 = -40; x2 <= 40; ++x2)
					raw.setVoxel(x2, y2, z2, densityAt(x2, y2, z2));

		PagedVolume::Sampler negativeSampler(&vol);
		bool negativeOk = true;
		for (int idx = -33; idx <= 33; ++idx)
		{
			// (idx, -idx, idx) walks through several chunks on all three axes.
			negativeSampler.setPosition(idx, -idx, idx);
			const Voxel expected = densityAt(idx, -idx, idx);
			if (negativeSampler.getVoxel() != expected) negativeOk = false;
			if (vol.getVoxel(idx, -idx, idx) != expected) negativeOk = false;
			if (raw.getVoxel(idx, -idx, idx) != expected) negativeOk = false;
		}
		CHECK("negative-morton", negativeOk);
	}

	// ---------------------------------------------------------------------------------
	// Case 4: all 27 peek neighbourhood operations, including chunk crossings.
	// ---------------------------------------------------------------------------------
	// A dispatch table indexed by (dx+1)*9 + (dy+1)*3 + (dz+1).
	Voxel peekAll(PagedVolume::Sampler& s, int index)
	{
		switch (index)
		{
		case 0: return s.peekVoxel1nx1ny1nz();
		case 1: return s.peekVoxel1nx1ny0pz();
		case 2: return s.peekVoxel1nx1ny1pz();
		case 3: return s.peekVoxel1nx0py1nz();
		case 4: return s.peekVoxel1nx0py0pz();
		case 5: return s.peekVoxel1nx0py1pz();
		case 6: return s.peekVoxel1nx1py1nz();
		case 7: return s.peekVoxel1nx1py0pz();
		case 8: return s.peekVoxel1nx1py1pz();

		case 9: return s.peekVoxel0px1ny1nz();
		case 10: return s.peekVoxel0px1ny0pz();
		case 11: return s.peekVoxel0px1ny1pz();
		case 12: return s.peekVoxel0px0py1nz();
		case 13: return s.peekVoxel0px0py0pz();
		case 14: return s.peekVoxel0px0py1pz();
		case 15: return s.peekVoxel0px1py1nz();
		case 16: return s.peekVoxel0px1py0pz();
		case 17: return s.peekVoxel0px1py1pz();

		case 18: return s.peekVoxel1px1ny1nz();
		case 19: return s.peekVoxel1px1ny0pz();
		case 20: return s.peekVoxel1px1ny1pz();
		case 21: return s.peekVoxel1px0py1nz();
		case 22: return s.peekVoxel1px0py0pz();
		case 23: return s.peekVoxel1px0py1pz();
		case 24: return s.peekVoxel1px1py1nz();
		case 25: return s.peekVoxel1px1py0pz();
		case 26: return s.peekVoxel1px1py1pz();
		default: std::abort();
		}
	}

	void computeOffsets(int index, int& dx, int& dy, int& dz)
	{
		dx = (index / 9) - 1;
		dy = ((index % 9) / 3) - 1;
		dz = (index % 3) - 1;
	}

	// Compare every peek against the deterministic field for a set of positions,
	// including positions on chunk boundaries and in negative chunks.
	void testPeekNeighbourhood()
	{
		StoringPager pager;
		PagedVolume vol(&pager, 1024 * 1024, 32);

		const int positions[][3] = {
			{ 1, 1, 1 },        // interior of a positive chunk
			{ 31, 31, 31 },     // positive chunk boundary
			{ 32, 32, 32 },     // negative/positive chunk boundary
			{ -1, -1, -1 },     // negative chunk boundary
			{ -32, -31, -33 },  // deep negative chunk
			{ 0, 0, 0 },        // origin
			{ 100, -50, 25 },   // mixture
		};

		PagedVolume::Sampler sampler(&vol);
		bool allOk = true;

		for (size_t p = 0; p < sizeof(positions) / sizeof(positions[0]); ++p)
		{
			const int px = positions[p][0];
			const int py = positions[p][1];
			const int pz = positions[p][2];
			sampler.setPosition(px, py, pz);

			// Check each of the 27 peeks. Do them one at a time, interleaving a few
			// deliberately disruptive volume reads so that some peeks cross chunk
			// boundaries and others hit the fast path after a re-resolution.
			for (int index = 0; index < 27; ++index)
			{
				int dx, dy, dz;
				computeOffsets(index, dx, dy, dz);
				const Voxel expected = densityAt(px + dx, py + dy, pz + dz);
				if (peekAll(sampler, index) != expected) allOk = false;

				// Interleave a far read on some iterations to exercise revalidation.
				if ((index % 7) == 0)
				{
					vol.getVoxel(500, 500, 500);
				}
				// Also verify the centre sample itself still reads correctly.
				if (sampler.getVoxel() != densityAt(px, py, pz)) allOk = false;
			}
		}

		CHECK("peek-all-27", allOk);

		// Interleaved samplers: one sampler wandering far away must not disturb the
		// other, including through peek paths which cross chunk boundaries.
		PagedVolume::Sampler nearSampler(&vol);
		PagedVolume::Sampler farSampler(&vol);
		nearSampler.setPosition(1, 1, 1);
		farSampler.setPosition(1000, 1000, 1000);

		bool interleavedOk = true;
		for (int i = 0; i < 100; ++i)
		{
			farSampler.setPosition(2000 + i * 32, -1000, 1500);
			(void)farSampler.getVoxel();
			if (nearSampler.getVoxel() != densityAt(1, 1, 1)) interleavedOk = false;
			if (nearSampler.peekVoxel1nx1ny1nz() != densityAt(0, 0, 0)) interleavedOk = false;
			if (nearSampler.peekVoxel1px1py1pz() != densityAt(2, 2, 2)) interleavedOk = false;
		}
		CHECK("interleaved-peek", interleavedOk);
	}

	// ---------------------------------------------------------------------------------
	// Case 5: eviction and flushAll reload from the Pager's storage.
	// ---------------------------------------------------------------------------------
	void testEvictionAndFlushReload()
	{
		// A small cache so that repeated far reads really evict the original chunk.
		StoringPager pager;
		PagedVolume vol(&pager, 1024 * 1024, 32);

		PagedVolume::Sampler sampler(&vol);
		sampler.setPosition(1, 1, 1);
		const Voxel original = densityAt(1, 1, 1);
		CHECK("evict-initial", sampler.getVoxel() == original);

		// Write through the volume. The sampler at the same position must see it,
		// without a setPosition.
		vol.setVoxel(1, 1, 1, 123);
		CHECK("volume-write-visible", sampler.getVoxel() == 123);

		// Now evict the chunk. The StoringPager kept the *modified* chunk contents
		// via pageOut when it was evicted. After paging it back in, the sampler must
		// again see the value which was written, i.e. it must not retain the old
		// snapshot nor a stale copy of the pre-write data.
		for (int k = 1; k <= 80; ++k)
		{
			vol.getVoxel(32 * k, 0, 0);
		}
		CHECK("evict-modified-reload", sampler.getVoxel() == 123);
		CHECK("evict-page-out-happened", pager.pageOutCount > 0);

		// flushAll must still hand dirty data to the Pager and free it. Then a write
		// to the same position must be observed after the chunk is reloaded.
		vol.flushAll();
		const uint32_t sizeAfterFlush = vol.calculateSizeInBytes();
		CHECK("flush-frees-memory", sizeAfterFlush == 0);

		// Change the Pager's stored value for the chunk containing (1,1,1) while the
		// chunk is not resident, simulating another writer / an external update.
		const ChunkKey chunkPos(0, 0, 0);
		pager.storage[chunkPos]; // ensure it exists
		vol.getVoxel(1, 1, 1);   // page it in (still holds 123 stored by pageOut)
		CHECK("flush-reload-value", sampler.getVoxel() == 123);

		// Now replace the stored contents of the chunk with a constant snapshot and
		// flush again. The old sampler must read the reloaded value, exactly as
		// Volume::getVoxel does, not a preserved old in-memory snapshot.
		const Voxel replacement = 200;
		vol.getVoxel(32 * 0, 0, 0); // make sure chunk is resident
		std::vector<Voxel>& stored = pager.storage[chunkPos];
		stored.resize(32 * 32 * 32, replacement);
		// (stored was filled by pageOut; overwrite it wholesale)

		vol.flushAll();
		// Flush wrote the chunk back out, restoring what was resident. Overwrite the
		// storage *after* the flush to model an external update.
		stored.assign(32 * 32 * 32, replacement);

		const Voxel viaVolume = vol.getVoxel(1, 1, 1);
		const Voxel viaSampler = sampler.getVoxel();
		CHECK("flush-external-update-volume", viaVolume == replacement);
		CHECK("flush-external-update-sampler", viaSampler == replacement);
		CHECK("flush-external-update-equal", viaSampler == viaVolume);

		// Sanity: the sampler was not moved, so it is still at the same coordinates.
		CHECK("flush-coordinates-preserved", sampler.getPosition() == PolyVox::Vector3DInt32(1, 1, 1));
	}

	// ---------------------------------------------------------------------------------
	// Case 6: full sphere extraction under memory pressure vs. RawVolume, comparing
	// decoded geometry as well as counts.
	// ---------------------------------------------------------------------------------
	struct TriangleKey
	{
		int a, b, c;
		bool operator==(const TriangleKey& rhs) const
		{
			return (a == rhs.a) && (b == rhs.b) && (c == rhs.c);
		}
		bool operator!=(const TriangleKey& rhs) const
		{
			return !(*this == rhs);
		}
	};

	struct DecodedMesh
	{
		std::vector<PolyVox::Vector3DInt32> quantisedPositions; // 8.8 fixed point, integer
		std::vector<PolyVox::Vector3DInt32> quantisedNormals;   // oct16 decoded, quantised
		std::vector<Voxel> materials;
		std::vector<TriangleKey> triangles;                     // sorted oriented triples
	};

	int quantiseNormalComponent(float v)
	{
		// Compare normals with a tolerance by quantising the decoded float to 1/2048.
		return static_cast<int>(v * 2048.0f + (v >= 0 ? 0.5f : -0.5f));
	}

	bool triangleLess(const TriangleKey& l, const TriangleKey& r)
	{
		if (l.a != r.a) return l.a < r.a;
		if (l.b != r.b) return l.b < r.b;
		return l.c < r.c;
	}

	template <typename MeshType>
	DecodedMesh decodeAndCompareMesh(const MeshType& mesh)
	{
		DecodedMesh out;
		for (typename MeshType::IndexType i = 0; i < mesh.getNoOfVertices(); ++i)
		{
			const auto decoded = PolyVox::decodeVertex(mesh.getVertex(i));
			// Position is stored as 8.8 fixed point; recover the exact integer encoding.
			out.quantisedPositions.push_back(PolyVox::Vector3DInt32(
				static_cast<int32_t>(decoded.position.getX() * 256.0f + 0.5f),
				static_cast<int32_t>(decoded.position.getY() * 256.0f + 0.5f),
				static_cast<int32_t>(decoded.position.getZ() * 256.0f + 0.5f)));
			out.quantisedNormals.push_back(PolyVox::Vector3DInt32(
				quantiseNormalComponent(decoded.normal.getX()),
				quantiseNormalComponent(decoded.normal.getY()),
				quantiseNormalComponent(decoded.normal.getZ())));
			out.materials.push_back(decoded.data);
		}

		for (size_t i = 0; i + 2 < mesh.getNoOfIndices(); i += 3)
		{
			TriangleKey tri;
			tri.a = static_cast<int>(mesh.getIndex(static_cast<uint32_t>(i)));
			tri.b = static_cast<int>(mesh.getIndex(static_cast<uint32_t>(i + 1)));
			tri.c = static_cast<int>(mesh.getIndex(static_cast<uint32_t>(i + 2)));
			// Keep the orientation (vertex order), and also require the same cyclic
			// order. Different benign orderings would be a mismatch for this test.
			out.triangles.push_back(tri);
		}
		// Sort triangles so that traversal/order differences (which are not part of the
		// output contract) do not matter, but orientation is retained.
		std::sort(out.triangles.begin(), out.triangles.end(), triangleLess);
		return out;
	}

	void testFullExtractionUnderPressure()
	{
		// Cache is deliberately small (< the number of chunks the sphere touches) so
		// that extraction interleaved with distant reads causes real eviction.
		StoringPager pager;
		PagedVolume vol(&pager, 2 * 1024 * 1024, 32);

		// Reference volume: the same density sampled densely enough for the sphere.
		PolyVox::Region region(-8, -8, -8, 8, 8, 8);
		RawVolume raw(region);
		for (int z = -8; z <= 8; ++z)
			for (int y = -8; y <= 8; ++y)
				for (int x = -8; x <= 8; ++x)
					raw.setVoxel(x, y, z, densityAt(x, y, z));

		// Repeatedly read far chunks, interleave local sampling and full extraction.
		PagedVolume::Sampler localSampler(&vol);
		localSampler.setPosition(1, 1, 1);
		const Voxel localExpected = densityAt(1, 1, 1);

		for (int round = 0; round < 4; ++round)
		{
			for (int k = 1; k <= 60; ++k)
			{
				vol.getVoxel(32 * k + round, 32 * k, -32 * k);
			}
			CHECK("pressure-local-sampler", localSampler.getVoxel() == localExpected);

			if (localSampler.peekVoxel1nx1ny1nz() != densityAt(0, 0, 0))
			{
				CHECK("pressure-local-peek", false);
			}

			auto pagedMesh = PolyVox::extractMarchingCubesMesh(&vol, region);
			auto rawMesh = PolyVox::extractMarchingCubesMesh(&raw, region);

			CHECK("pressure-vertex-count", pagedMesh.getNoOfVertices() == rawMesh.getNoOfVertices());
			CHECK("pressure-index-count", pagedMesh.getNoOfIndices() == rawMesh.getNoOfIndices());

			DecodedMesh pagedDecoded = decodeAndCompareMesh(pagedMesh);
			DecodedMesh rawDecoded = decodeAndCompareMesh(rawMesh);

			CHECK("pressure-positions", pagedDecoded.quantisedPositions == rawDecoded.quantisedPositions);
			CHECK("pressure-normals", pagedDecoded.quantisedNormals == rawDecoded.quantisedNormals);
			CHECK("pressure-materials", pagedDecoded.materials == rawDecoded.materials);
			CHECK("pressure-triangles", pagedDecoded.triangles == rawDecoded.triangles);

			// The baseline documented by the task: 414 vertices, 2472 indices.
			CHECK("pressure-baseline-vertices", pagedMesh.getNoOfVertices() == 414);
			CHECK("pressure-baseline-indices", pagedMesh.getNoOfIndices() == 2472);
		}

		// Modify some voxels through the Volume, force paging, then reload. This checks
		// that the bytes written back to the Pager are the modified ones (a pager which
		// regenerates a constant could never detect a stale snapshot here).
		const Voxel writtenA = 250;
		const Voxel writtenB = 3;
		vol.setVoxel(5, 5, 5, writtenA);
		vol.setVoxel(-6, -6, -6, writtenB);
		CHECK("writeback-visible-before-flush", vol.getVoxel(5, 5, 5) == writtenA);

		const unsigned pageOutsBefore = pager.pageOutCount;
		vol.flushAll();
		CHECK("pressure-flush-paged-out", pager.pageOutCount > pageOutsBefore);
		CHECK("pressure-flush-empty", vol.calculateSizeInBytes() == 0);

		// After reload the Volume and the previously created Sampler must agree on the
		// modified values, and the rest of the field must be intact.
		PagedVolume::Sampler reloadSampler(&vol);
		reloadSampler.setPosition(5, 5, 5);
		CHECK("writeback-reload-volume", vol.getVoxel(5, 5, 5) == writtenA);
		CHECK("writeback-reload-sampler", reloadSampler.getVoxel() == writtenA);
		CHECK("writeback-reload-volume-b", vol.getVoxel(-6, -6, -6) == writtenB);

		bool reloadOk = true;
		for (int z = -8; z <= 8; ++z)
			for (int y = -8; y <= 8; ++y)
				for (int x = -8; x <= 8; ++x)
				{
					Voxel expected = densityAt(x, y, z);
					if (x == 5 && y == 5 && z == 5) expected = writtenA;
					if (x == -6 && y == -6 && z == -6) expected = writtenB;
					if (vol.getVoxel(x, y, z) != expected) reloadOk = false;
				}
		CHECK("pressure-reload-content", reloadOk);

		// The persisted chunk snapshots must contain the modified values as well.
		CHECK("pressure-storage-has-written-chunk", pager.storage.count(ChunkKey(0, 0, 0)) == 1);
		if (pager.storage.count(ChunkKey(0, 0, 0)) == 1)
		{
			const std::vector<Voxel>& stored = pager.storage[ChunkKey(0, 0, 0)];
			CHECK("pressure-storage-written-value", stored[StoringPager::chunkIndex(5, 5, 5)] == writtenA);
		}
		if (pager.storage.count(ChunkKey(-1, -1, -1)) == 1)
		{
			const std::vector<Voxel>& stored = pager.storage[ChunkKey(-1, -1, -1)];
			// (-6,-6,-6) is offset 26 within chunk (-1,-1,-1).
			CHECK("pressure-storage-written-value-b", stored[StoringPager::chunkIndex(26, 26, 26)] == writtenB);
		}
	}

	// ---------------------------------------------------------------------------------
	// Case 7: a sampler whose chunk is evicted repeatedly while peeking across
	// boundaries; the peek fallback path must fetch the neighbouring chunk (and thus
	// potentially evict the sampler's own chunk) and still be correct.
	// ---------------------------------------------------------------------------------
	void testBoundaryPeekAfterEviction()
	{
		StoringPager pager;
		PagedVolume vol(&pager, 1024 * 1024, 32);

		PagedVolume::Sampler sampler(&vol);
		sampler.setPosition(32, 0, 0); // x is at the start of chunk 1

		for (int k = 0; k < 40; ++k)
		{
			vol.getVoxel(1000 + 32 * k, -1000, 500);
		}

		// A peek which stays in the chunk (positive x) is correct after revalidation.
		CHECK("boundary-in-chunk-peek", sampler.peekVoxel1px0py0pz() == densityAt(33, 0, 0));
		// A peek which crosses into the negative-x chunk must load that neighbour.
		CHECK("boundary-cross-peek", sampler.peekVoxel1nx0py0pz() == densityAt(31, 0, 0));
		// After the cross-chunk peek (which may evict the sampler's chunk), the
		// centre read and the in-chunk read must still be correct.
		CHECK("boundary-centre-after-cross", sampler.getVoxel() == densityAt(32, 0, 0));
		CHECK("boundary-in-chunk-after-cross", sampler.peekVoxel1px1py1pz() == densityAt(33, 1, 1));
	}
}

int main()
{
	// These tests assume the chunk side used in the helpers above.
	(void)CHUNK_SIDE;
	(void)CHUNK_SIDE_POWER;

	testOriginalReproductions();
	testInterleavedSamplers();
	testCopyAndAssignment();
	testMovesAndBoundaries();
	testPeekNeighbourhood();
	testEvictionAndFlushReload();
	testFullExtractionUnderPressure();
	testBoundaryPeekAfterEviction();

	if (g_failures != 0)
	{
		std::printf("SAMPLER_LIFETIME_FAILED failures=%d cases=%d\n", g_failures, g_cases);
		return 1;
	}

	std::printf("SAMPLER_LIFETIME_OK cases=%d\n", g_cases);
	return 0;
}
