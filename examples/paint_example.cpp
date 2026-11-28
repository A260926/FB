/*
   Stand-alone demonstration of direct octree box editing.

   It shows the full headless workflow with no windowing/SDL dependency:
     1. build a small sparse octree from an in-memory raw voxel volume and save it,
     2. load the descriptor file back (no VoxelData / raw file is attached anymore),
     3. paint solid boxes and erase a box through paintBox on integer grids,
     4. save the edited descriptor file and reload it to prove the format round-trips.

   Build (from the repository root), reusing the same sources as the task build:
     c++ -std=c++11 -O2 -I. -Isrc examples/paint_example.cpp \
         .task-build/VoxelOctree.o .task-build/VoxelEditing.o \
         .task-build/VoxelData.o .task-build/PlyLoader.o .task-build/Debug.o \
         .task-build/Util.o .task-build/ThreadPool.o .task-build/ThreadUtils.o \
         .task-build/lz4.o .task-build/plyfile.o .task-build/tribox3.o \
         -pthread -o paint_example
*/

#include "VoxelData.hpp"
#include "VoxelOctree.hpp"
#include "thread/ThreadUtils.hpp"
#include "thread/ThreadPool.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <memory>

typedef std::uint32_t u32;

namespace {

// Write a tiny 8^3 raw volume containing a single filled voxel.
void writeRawVolume(const char *path) {
    FILE *fp = std::fopen(path, "wb");
    int dim[3] = {8, 8, 8};
    std::fwrite(dim, sizeof(int), 3, fp);

    const u32 material = 0xAABBCCDDu;
    for (int z = 0; z < 8; ++z)
        for (int y = 0; y < 8; ++y)
            for (int x = 0; x < 8; ++x) {
                u32 voxel = (x == 2 && y == 3 && z == 4) ? material : 0u;
                std::fwrite(&voxel, sizeof(u32), 1, fp);
            }
    std::fclose(fp);
}

} // namespace

int main() {
    ThreadUtils::startThreads(2);

    // 1. Construct an octree the classic way and persist it.
    writeRawVolume(".task-build/example.raw");
    {
        VoxelData voxels(".task-build/example.raw", 1024 * 1024);
        VoxelOctree built(&voxels);
        built.save(".task-build/example.oct");
    }

    // 2. Reload the descriptor file. This object has no VoxelData and no raw source.
    VoxelOctree tree(".task-build/example.oct");
    std::printf("loaded: %llu descriptor words, center (%g, %g, %g)\n",
                static_cast<unsigned long long>(tree.wordCount()),
                tree.center().x, tree.center().y, tree.center().z);

    // 3. Edit directly on integer grids. Grid k maps to scene coordinate 1 + k/N,
    //    and each box is half open: [lo, hi) on every axis.
    const u32 solid = 0x10203040u;
    std::array<u32, 3> fullLo = {0, 0, 0};
    std::array<u32, 3> fullHi = {8, 8, 8};
    tree.paintBox(3, fullLo, fullHi, solid);          // fill the whole 8^3 scene
    std::printf("after full fill: %llu words\n",
                static_cast<unsigned long long>(tree.wordCount()));

    // A finer grid (16^3) only positions the brush; it never resamples old content.
    std::array<u32, 3> holeLo = {6, 6, 6};
    std::array<u32, 3> holeHi = {10, 10, 10};
    tree.paintBox(4, holeLo, holeHi, 0u);             // material 0 erases
    std::printf("after carving a box on a finer grid: %llu words\n",
                static_cast<unsigned long long>(tree.wordCount()));

    // Repaint the same region with a new material and watch solid leaves merge.
    std::array<u32, 3> patchLo = {6, 6, 6};
    std::array<u32, 3> patchHi = {10, 10, 10};
    tree.paintBox(4, patchLo, patchHi, 0x55667788u);
    std::printf("after repainting the patch: %llu words\n",
                static_cast<unsigned long long>(tree.wordCount()));

    // 4. Save using the original file structure, then confirm it loads again.
    tree.save(".task-build/example_edited.oct");
    VoxelOctree reloaded(".task-build/example_edited.oct");
    std::printf("reloaded edited tree: %llu words, center unchanged (%g, %g, %g)\n",
                static_cast<unsigned long long>(reloaded.wordCount()),
                reloaded.center().x, reloaded.center().y, reloaded.center().z);

    delete ThreadUtils::pool;
    ThreadUtils::pool = nullptr;
    return 0;
}
