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

#ifndef __PolyVox_PagedVolume_H__
#define __PolyVox_PagedVolume_H__

#include "BaseVolume.h"
#include "Region.h"
#include "Vector.h"

#include <limits>
#include <cstdlib> //For abort()
#include <cstring> //For memcpy
#include <unordered_map>
#include <list>
#include <map>
#include <memory>
#include <stdexcept> //For invalid_argument
#include <vector>

namespace PolyVox
{
	/// This class provide a volume implementation which avoids storing all the data in memory at all times. Instead it breaks the volume
	/// down into a set of chunks and moves these into and out of memory on demand. This means it is much more memory efficient than the
	/// RawVolume, but may also be slower and is more complicated We encourage uses to work with RawVolume initially, and then switch to
	/// PagedVolume once they have a larger application and/or a better understanding of PolyVox.
	////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
	/// 
	/// The PagedVolume makes use of a Pager which defines the source and/or destination for data paged into and out of memory. PolyVox
	/// comes with an example FilePager though users can also implement their own approaches. For example, the Pager could instead stream
	/// data from a network connection or generate it procedurally on demand.
	///
	/// A consequence of this paging approach is that (unlike the RawVolume) the PagedVolume does not need to have a predefined size. After
	/// the volume has been created you can begin acessing voxels anywhere in space and the required data will be created automatically.
	////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
	template <typename VoxelType>
	class PagedVolume : public BaseVolume<VoxelType>
	{
	public:
		/// The PagedVolume stores it data as a set of Chunk instances which can be loaded and unloaded as memory requirements dictate.
		class Chunk;
		/// The Pager class is responsible for the loading and unloading of Chunks, and can be subclassed by the user.
		class Pager;
		/// The Sampler provides safe, cached voxel access and is defined below. It is
		/// forward declared so that Chunk can grant it access to its internal buffer
		/// and identity for cache revalidation.
		class Sampler;

		class Chunk
		{
			friend class PagedVolume;
			friend class Sampler;

		public:
			Chunk(Vector3DInt32 v3dPosition, uint16_t uSideLength, Pager* pPager = nullptr, uint64_t uSerial = 0);
			~Chunk();

			VoxelType* getData(void) const;
			uint32_t getDataSizeInBytes(void) const;

			VoxelType getVoxel(uint32_t uXPos, uint32_t uYPos, uint32_t uZPos) const;
			VoxelType getVoxel(const Vector3DUint16& v3dPos) const;

			void setVoxel(uint32_t uXPos, uint32_t uYPos, uint32_t uZPos, VoxelType tValue);
			void setVoxel(const Vector3DUint16& v3dPos, VoxelType tValue);

			void changeLinearOrderingToMorton(void);
			void changeMortonOrderingToLinear(void);

		private:
			/// Private copy constructor to prevent accisdental copying
			Chunk(const Chunk& /*rhs*/) {};

			/// Private assignment operator to prevent accisdental copying
			Chunk& operator=(const Chunk& /*rhs*/) {};

			// This is updated by the PagedVolume and used to discard the least recently used chunks.
			uint32_t m_uChunkLastAccessed;

			// This is so we can tell whether a uncompressed chunk has to be recompressed and whether
			// a compressed chunk has to be paged back to disk, or whether they can just be discarded.
			bool m_bDataModified;

			uint32_t calculateSizeInBytes(void);
			static uint32_t calculateSizeInBytes(uint32_t uSideLength);

			VoxelType* m_tData;
			uint16_t m_uSideLength;
			uint8_t m_uSideLengthPower;
			Pager* m_pPager;

			// Note: Do we really need to store this position here as well as in the block maps?
			Vector3DInt32 m_v3dChunkSpacePosition;

			// Unique, monotonically increasing identity assigned by the PagedVolume which
			// owns this chunk. Two different chunk objects never share a serial, which lets
			// a Sampler prove that the chunk it cached has not been freed and replaced by
			// a new allocation at the same address.
			uint64_t m_uSerial;
		};

		/**
		* Users can override this class and provide an instance of the derived class to the PagedVolume constructor. This derived class
		* could then perform tasks such as compression and decompression of the data, and read/writing it to a file, database, network,
		* or other storage as appropriate. See FilePager for a simple example of such a derived class.
		*/
		class Pager
		{
		public:
			/// Constructor
			Pager() {};
			/// Destructor
			virtual ~Pager() {};

			virtual void pageIn(const Region& region, Chunk* pChunk) = 0;
			virtual void pageOut(const Region& region, Chunk* pChunk) = 0;
		};

		//There seems to be some descrepency between Visual Studio and GCC about how the following class should be declared.
		//There is a work around (see also See http://goo.gl/qu1wn) given below which appears to work on VS2010 and GCC, but
		//which seems to cause internal compiler errors on VS2008 when building with the /Gm 'Enable Minimal Rebuild' compiler
		//option. For now it seems best to 'fix' it with the preprocessor insstead, but maybe the workaround can be reinstated
		//in the future
		//typedef Volume<VoxelType> VolumeOfVoxelType; //Workaround for GCC/VS2010 differences.
		//class Sampler : public VolumeOfVoxelType::template Sampler< PagedVolume<VoxelType> >
#ifndef SWIG
#if defined(_MSC_VER)
		class Sampler : public BaseVolume<VoxelType>::Sampler< PagedVolume<VoxelType> > //This line works on VS2010
#else
		class Sampler : public BaseVolume<VoxelType>::template Sampler< PagedVolume<VoxelType> > //This line works on GCC
#endif
		{
		public:
			////////////////////////////////////////////////////////////////////////////////
			/// The Sampler caches a pointer straight into the voxel data of a resident
			/// chunk. That pointer is only valid for as long as the chunk stays resident:
			/// chunks can be evicted by the LRU cache when other chunks are paged in,
			/// and flushAll() destroys all of them.
			///
			/// To keep this cache safe the sampler remembers which slot of the volume's
			/// chunk hash table holds its chunk, together with the chunk's unique serial
			/// number. Every access performs a constant-time check that the slot still
			/// contains the same live chunk before dereferencing the cached pointer.
			/// If it does not (because of eviction, flush or slot reuse) the sampler
			/// transparently re-resolves its chunk from the volume using its current
			/// position, so callers never have to call setPosition() again. The first
			/// access after such an invalidation pays for a chunk lookup; subsequent
			/// accesses within the same chunk use the pointer again.
			///
			/// Samplers are copyable and assignable. A copy keeps the coordinates and
			/// cache of the original, but all of that state is stored in the copy
			/// itself, so moving or re-positioning one sampler never invalidates or
			/// moves another sampler (including the one it was copied from).
			////////////////////////////////////////////////////////////////////////////////

			Sampler(PagedVolume<VoxelType>* volume);
			~Sampler();

			inline VoxelType getVoxel(void) const;

			void setPosition(const Vector3DInt32& v3dNewPos);
			void setPosition(int32_t xPos, int32_t yPos, int32_t zPos);
			inline bool setVoxel(VoxelType tValue);

			void movePositiveX(void);
			void movePositiveY(void);
			void movePositiveZ(void);

			void moveNegativeX(void);
			void moveNegativeY(void);
			void moveNegativeZ(void);

			inline VoxelType peekVoxel1nx1ny1nz(void) const;
			inline VoxelType peekVoxel1nx1ny0pz(void) const;
			inline VoxelType peekVoxel1nx1ny1pz(void) const;
			inline VoxelType peekVoxel1nx0py1nz(void) const;
			inline VoxelType peekVoxel1nx0py0pz(void) const;
			inline VoxelType peekVoxel1nx0py1pz(void) const;
			inline VoxelType peekVoxel1nx1py1nz(void) const;
			inline VoxelType peekVoxel1nx1py0pz(void) const;
			inline VoxelType peekVoxel1nx1py1pz(void) const;

			inline VoxelType peekVoxel0px1ny1nz(void) const;
			inline VoxelType peekVoxel0px1ny0pz(void) const;
			inline VoxelType peekVoxel0px1ny1pz(void) const;
			inline VoxelType peekVoxel0px0py1nz(void) const;
			inline VoxelType peekVoxel0px0py0pz(void) const;
			inline VoxelType peekVoxel0px0py1pz(void) const;
			inline VoxelType peekVoxel0px1py1nz(void) const;
			inline VoxelType peekVoxel0px1py0pz(void) const;
			inline VoxelType peekVoxel0px1py1pz(void) const;

			inline VoxelType peekVoxel1px1ny1nz(void) const;
			inline VoxelType peekVoxel1px1ny0pz(void) const;
			inline VoxelType peekVoxel1px1ny1pz(void) const;
			inline VoxelType peekVoxel1px0py1nz(void) const;
			inline VoxelType peekVoxel1px0py0pz(void) const;
			inline VoxelType peekVoxel1px0py1pz(void) const;
			inline VoxelType peekVoxel1px1py1nz(void) const;
			inline VoxelType peekVoxel1px1py0pz(void) const;
			inline VoxelType peekVoxel1px1py1pz(void) const;

		private:
			/// Returns true if mCurrentVoxel still points into the voxel data of the chunk
			/// which currently occupies the slot recorded in m_pChunkSlot. This never
			/// dereferences m_pCachedChunk unless the slot itself still refers to it, so
			/// it is safe even after the chunk has been evicted or flushed.
			bool isCurrentVoxelCacheValid(void) const;

			/// Recomputes mCurrentVoxel (and the position-within-chunk fields) for the
			/// sampler's current volume position, looking the chunk up through the volume.
			/// Used after eviction, flushing, or any other operation which may have
			/// invalidated the cached pointer.
			void refreshCurrentVoxel(void) const;

			/// Ensures the cached voxel pointer is valid and returns it. A constant-time
			/// check is performed; if it fails the chunk is resolved again transparently.
			/// The returned pointer is only guaranteed until the next volume operation.
			VoxelType* ensureCurrentVoxel(void) const;
			// These are 'mutable' so that the pointer can be repaired from const methods
			// (getVoxel() and the peek functions) without requiring callers to re-position.
			mutable VoxelType* mCurrentVoxel;

			// Identity of the chunk currently backing mCurrentVoxel. The slot is a stable
			// address in the volume's chunk array; the serial proves the object living in
			// that slot is still the exact chunk which was cached (a freed chunk could
			// otherwise be replaced at the same address by a new one).
			mutable std::unique_ptr<Chunk>* m_pChunkSlot;
			mutable Chunk* m_pCachedChunk;
			mutable uint64_t m_uCachedChunkSerial;

			mutable uint16_t m_uXPosInChunk;
			mutable uint16_t m_uYPosInChunk;
			mutable uint16_t m_uZPosInChunk;

			// This should ideally be const, but that prevent automatic generation of an assignment operator (https://goo.gl/Sn7KpZ).
			// We could provide one manually, but it's currently unused so there is no real test for if it works. I'm putting
			// together a new release at the moment so I'd rathern not make 'risky' changes.
			uint16_t m_uChunkSideLengthMinusOne;
		};

#endif // SWIG

	public:
		/// Constructor for creating a fixed size volume.
		PagedVolume(Pager* pPager, uint32_t uTargetMemoryUsageInBytes = 256 * 1024 * 1024, uint16_t uChunkSideLength = 32);
		/// Destructor
		~PagedVolume();

		/// Gets a voxel at the position given by <tt>x,y,z</tt> coordinates
		VoxelType getVoxel(int32_t uXPos, int32_t uYPos, int32_t uZPos) const;
		/// Gets a voxel at the position given by a 3D vector
		VoxelType getVoxel(const Vector3DInt32& v3dPos) const;

		/// Sets the voxel at the position given by <tt>x,y,z</tt> coordinates
		void setVoxel(int32_t uXPos, int32_t uYPos, int32_t uZPos, VoxelType tValue);
		/// Sets the voxel at the position given by a 3D vector
		void setVoxel(const Vector3DInt32& v3dPos, VoxelType tValue);

		/// Tries to ensure that the voxels within the specified Region are loaded into memory.
		void prefetch(Region regPrefetch);
		/// Removes all voxels from memory
		void flushAll();

		/// Calculates approximatly how many bytes of memory the volume is currently using.
		uint32_t calculateSizeInBytes(void);

	protected:
		/// Copy constructor
		PagedVolume(const PagedVolume& rhs);

		/// Assignment operator
		PagedVolume& operator=(const PagedVolume& rhs);

	private:
		bool canReuseLastAccessedChunk(int32_t iChunkX, int32_t iChunkY, int32_t iChunkZ) const;
		Chunk* getChunk(int32_t uChunkX, int32_t uChunkY, int32_t uChunkZ) const;

		/// Looks up an already-resident chunk without paging anything in or altering the
		/// LRU timestamps. Returns the chunk's slot in m_arrayChunks (or nullptr) and
		/// optionally the chunk pointer. Used by Sampler to safely revalidate a cached
		/// data pointer.
		std::unique_ptr<Chunk>* findChunkSlot(int32_t uChunkX, int32_t uChunkY, int32_t uChunkZ, Chunk** ppChunk) const;

		/// Destroys the chunk occupying the given slot (paging dirty data out via its
		/// destructor) and clears the slot. Also drops the volume's last-accessed cache
		/// if it referred to that chunk. Constant-time, and does not touch any Sampler's
		/// state: samplers detect the change via the slot's contents.
		void evictChunk(uint32_t uIndex) const;

		// Storing these properties individually has proved to be faster than keeping
		// them in a Vector3DInt32 as it avoids constructions and comparison overheads.
		// They are also at the start of the class in the hope that they will be pulled
		// into cache - I've got no idea if this actually makes a difference.
		mutable int32_t m_v3dLastAccessedChunkX = 0;
		mutable int32_t m_v3dLastAccessedChunkY = 0;
		mutable int32_t m_v3dLastAccessedChunkZ = 0;
		mutable Chunk* m_pLastAccessedChunk = nullptr;

		mutable uint32_t m_uTimestamper = 0;

		// Hands out a unique identity to every Chunk created by this volume. Used by
		// Sampler to distinguish a live cached chunk from a freed one whose memory may
		// have been reused. Not intended to be shared between threads (see class docs).
		mutable uint64_t m_uNextChunkSerial = 0;

		uint32_t m_uChunkCountLimit = 0;

		// Chunks are stored in the following array which is used as a hash-table. Conventional wisdom is that such a hash-table
		// should not be more than half full to avoid conflicts, and a practical chunk size seems to be 64^3. With this configuration
		// there can be up to 32768*64^3 = 8 gigavoxels (with each voxel perhaps being many bytes). This should effectively make use 
		// of even high end machines. Of course, the user can choose to limit the memory usage in which case much less of the chunk 
		// array will actually be used. None-the-less, we have chosen to use a fixed size array (rather than a vector) as it appears to 
		// be slightly faster (probably due to the extra pointer indirection in a vector?) and the actual size of this array should
		// just be 1Mb or so.
		static const uint32_t uChunkArraySize = 65536;
		mutable std::unique_ptr< Chunk > m_arrayChunks[uChunkArraySize];

		// The size of the chunks
		uint16_t m_uChunkSideLength;
		uint8_t m_uChunkSideLengthPower;
		int32_t m_iChunkMask;

		Pager* m_pPager = nullptr;
	};
}

#include "PagedVolume.inl"
#include "PagedVolumeChunk.inl"
#include "PagedVolumeSampler.inl"

#endif //__PolyVox_PagedVolume_H__
