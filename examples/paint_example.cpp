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

/* Headless painting example.
 *
 * Loads an octree file, paints a few boxes on it with different brush grids,
 * reports the descriptor word count and saves the result. It does not open a
 * window and needs no SDL: build it next to the normal production objects.
 *
 *   c++ -std=c++11 -O2 -Isrc examples/paint_example.cpp \
 *       src/VoxelOctree.cpp src/VoxelEditing.cpp src/Util.cpp src/Debug.cpp \
 *       src/third-party/lz4.c -o paint_example
 *   ./paint_example model.oct edited.oct
 */

#include "VoxelOctree.hpp"

#include <array>
#include <cstdio>
#include <iostream>

int main(int argc, char **argv) {
    if (argc < 3) {
        std::cout << "Usage: paint_example <input.oct> <output.oct>" << std::endl;
        return 1;
    }

    /* The loaded tree can be edited directly; no raw voxel file or VoxelData
     * instance is needed. */
    VoxelOctree tree(argv[1]);
    std::cout << "Loaded " << argv[1] << " with " << tree.wordCount()
              << " descriptor words" << std::endl;

    /* Fill the whole scene with one material on a coarse brush grid. Solid
     * regions become coarse leaves, so this collapses to a very small tree. */
    std::array<uint32, 3> allLo = {{0, 0, 0}};
    std::array<uint32, 3> allHi = {{1u << 10, 1u << 10, 1u << 10}};
    tree.paintBox(10, allLo, allHi, 0xA0B0C0D0u);
    std::cout << "After full fill: " << tree.wordCount() << " words" << std::endl;

    /* Carve a hole with a finer grid. Only the boundary of the box splits;
     * everything around it stays a coarse leaf. */
    std::array<uint32, 3> holeLo = {{500, 500, 500}};
    std::array<uint32, 3> holeHi = {{501, 501, 501}};
    tree.paintBox(10, holeLo, holeHi, 0);
    std::cout << "After carving one cell: " << tree.wordCount()
              << " words" << std::endl;

    /* Repaint the same box with a different material to prove the hole can be
     * refilled and that the brush grid is free to change between calls. */
    std::array<uint32, 3> patchLo = {{499, 499, 499}};
    std::array<uint32, 3> patchHi = {{502, 502, 502}};
    tree.paintBox(12, patchLo, patchHi, 0x10203040u);
    std::cout << "After patch: " << tree.wordCount() << " words" << std::endl;

    tree.save(argv[2]);
    std::cout << "Saved " << argv[2] << std::endl;

    /* Exact query without approximations: rayScale = 0. */
    uint32 material = 0;
    float t = 0.0f;
    Vec3 origin(0.9f, 1.0f + 500.5f/1024.0f, 1.0f + 500.5f/1024.0f);
    if (tree.raymarch(origin, Vec3(1.0f, 0.0f, 0.0f), 0.0f, material, t))
        std::cout << "First hit material 0x" << std::hex << material << std::dec
                  << " at t = " << t << std::endl;
    else
        std::cout << "Ray missed" << std::endl;

    return 0;
}
