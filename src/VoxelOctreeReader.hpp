#ifndef VOXELOCTREEREADER_HPP_
#define VOXELOCTREEREADER_HPP_

#include "IntTypes.hpp"

#include <cstddef>

// Read-only decoder for the compact sparse voxel octree produced by
// VoxelOctree. It mirrors the offset decoding of VoxelOctree::raymarch but
// never mutates the underlying array. All geometry bookkeeping is done on the
// integer root grid [0, 2^GridBits)^3; the world-space root is [1,2]^3.
struct OctreeReader {
    enum {
        GridBits = 23,
        GridSize = 1 << GridBits
    };

    struct NodeInfo {
        uint32 word;
        bool wideOffset;   // 0x20000: own child offset is a double word, low half at desc+1
        bool wideChildren; // 0x10000: every child slot occupies two words
        uint32 childMask;
        // Stored leaf mask convention (see VoxelOctree::buildOctree and the
        // !(childMasks & 0x80) test in VoxelOctree::raymarch): a set bit means
        // the present child slot contains an internal descriptor, a clear bit
        // means it contains a raw 32 bit leaf payload.
        uint32 leafMask;
        uint64 firstSlot; // absolute index of the first child slot
    };

    const uint32 *_data;
    uint64 _size;

    OctreeReader(const uint32 *data, uint64 size)
        : _data(data), _size(size) {}

    NodeInfo decode(uint64 desc) const {
        NodeInfo info;
        info.word = _data[desc];
        info.wideOffset = (info.word & 0x20000u) != 0;
        info.wideChildren = (info.word & 0x10000u) != 0;
        info.childMask = (info.word >> 8) & 0xFFu;
        info.leafMask = info.word & 0xFFu;

        uint64 rel = uint64(info.word >> 18);
        if (info.wideOffset)
            rel = (rel << 32) | uint64(_data[desc + 1]);
        info.firstSlot = desc + rel;
        return info;
    }

    // Number of present child octants with octant index greater than i.
    // VoxelOctree::buildOctree pushes slots in reverse octant order
    // (childIndices[childCount - 1 - i]), so higher indices occupy the
    // earlier slots. Wide nodes additionally carry one offset word per slot.
    static int presentAfter(uint32 childMask, int i) {
        int count = 0;
        for (int k = i + 1; k < 8; ++k)
            count += (childMask & (128u >> k)) ? 1 : 0;
        return count;
    }

    // Absolute slot index of octant i, which must be present.
    uint64 childSlot(const NodeInfo &info, int i) const {
        int j = presentAfter(info.childMask, i);
        return info.firstSlot + uint64(j) + (info.wideChildren ? uint64(j) : 0);
    }

    // Reports whether the grid cell (ix,iy,iz) is occupied by a leaf and, when
    // so, the compact-tree slot holding its payload. Shadow rays use the slot
    // to exempt endpoint leaves from self-occlusion.
    bool leafSlotAt(int32 ix, int32 iy, int32 iz, uint64 &slotOut,
                    uint64 *visits) const {
        slotOut = 0;
        if (ix < 0 || iy < 0 || iz < 0 ||
            ix >= GridSize || iy >= GridSize || iz >= GridSize)
            return false;

        int32 lo[3] = {0, 0, 0};
        int32 size = GridSize;
        uint64 desc = 0;

        for (;;) {
            if (visits) ++*visits;
            NodeInfo info = decode(desc);

            int32 half = size >> 1;
            int octant = 0;
            int32 mid[3];
            for (int a = 0; a < 3; ++a) {
                int32 v = (a == 0 ? ix : (a == 1 ? iy : iz));
                mid[a] = lo[a] + half;
                // Stored octant convention (see VoxelOctree::buildOctree):
                // bit a is set for the low half of axis a.
                if (v < mid[a]) octant |= 1 << a;
            }

            uint32 bit = 128u >> octant;
            if (!(info.childMask & bit))
                return false;

            uint64 slot = childSlot(info, octant);
            if (!(info.leafMask & bit)) {
                slotOut = slot;
                return true;
            }

            desc = slot;
            size = half;
            for (int a = 0; a < 3; ++a)
                lo[a] = (octant & (1 << a)) ? lo[a] : mid[a];
        }
    }
};

#endif /* VOXELOCTREEREADER_HPP_ */
