/*
Copyright (c) 2013 Benedikt Bitterli

This software is provided 'as-is', without any express or implied
warranty. In no event will the authors be held liable for any damages
arising from the use of this software.

Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it
freely, subject to the following restrictions:

   1. The origin of this software must not be misrepresented; you must not
   claim that you wrote the original software. If you use this software
   in a product, an acknowledgment in the product documentation would be
   appreciated but is not required.

   2. Altered source versions must be plainly marked as such, and must not be
   misrepresented as being the original software.

   3. This notice may not be removed or altered from any source
   distribution.
*/

#ifndef VOXELOCTREE_HPP_
#define VOXELOCTREE_HPP_

#include "math/Vec3.hpp"

#include "ChunkedAllocator.hpp"
#include "IntTypes.hpp"

#include <memory>
#include <vector>
#include <array>
#include <cstdint>

class VoxelData;

class VoxelOctree {
    static const int32 MaxScale = 23;

    uint64 _octreeSize;
    std::unique_ptr<uint32[]> _octree;

    VoxelData *_voxels;
    Vec3 _center;

    uint64 buildOctree(ChunkedAllocator<uint32> &allocator, int x, int y, int z, int size, uint64 descriptorIndex);

public:
    VoxelOctree(const char *path);
    VoxelOctree(VoxelData *voxels);

    void save(const char *path);
    bool raymarch(const Vec3 &o, const Vec3 &d, float rayScale, uint32 &normal, float &t);

    /*
       Box brush on an integer grid of side N = 2^gridDepth (1 <= gridDepth <= 20).
       Grid coordinate k maps to normalized scene coordinate 1 + k/N and the box is
       half open: [lo, hi) on every axis. Cells inside are overwritten with material
       (0 erases), cells outside are untouched. Different calls may use different
       grid depths; the brush only relocates existing content, it never resamples it.
       Works on trees loaded from descriptor files without any VoxelData attached.
    */
    void paintBox(unsigned gridDepth,
                  const std::array<uint32, 3> &lo, const std::array<uint32, 3> &hi,
                  uint32 material);

    /* Number of descriptor words currently held by the octree. */
    uint64 wordCount() const {
        return _octreeSize;
    }

    Vec3 center() const {
        return _center;
    }
};

#endif /* VOXELOCTREE_HPP_ */
