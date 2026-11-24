/*
 * Cloud of Voxels (CoV) project
 * GL-free CPU multi-viewport example.
 *
 * Builds two voxel trees of different sizes and two independent cameras,
 * renders one viewport per tree with four worker threads, and writes the
 * two frames stacked vertically into a binary P6 PPM given on the command
 * line. No window and no GL context are required.
 */

#include "headers/Octree.h"
#include "headers/Camera.h"
#include "headers/Options.h"
#include "headers/CPUFrame.h"

#include <fstream>
#include <iostream>
#include <vector>

namespace
{
    cpuframe::ViewportInput makeViewport(const Octree* const tree,
        const Mat3f& basis, const Vec3<float>& position,
        const unsigned int width, const unsigned int height,
        const cpuframe::ColorRGBA background)
    {
        cpuframe::ViewportInput viewport;
        viewport.tree = tree;
        viewport.basis = basis;
        viewport.position = position;
        viewport.width = width;
        viewport.height = height;
        viewport.perspectiveFactor = 0.0f;
        viewport.zoom = 1.0f;
        viewport.maxRayDistance = (float)tree->getRayLength();
        viewport.maxVoxelsByRay = 8;
        viewport.minVoxelProximity = 0;
        viewport.transparency = true;
        viewport.avoidGlitches = false;
        viewport.background = background;
        return viewport;
    }

    void writeFrame(std::ofstream& out,
        const cpuframe::ViewportFrame& frame)
    {
        for(unsigned int py = 0; py < frame.height; ++py)
        {
            for(unsigned int px = 0; px < frame.width; ++px)
            {
                const unsigned char* const pixel =
                    frame.rgba.data() + 4 * ((size_t)py * frame.width + px);
                const char rgb[3] = {
                    (char)pixel[0], (char)pixel[1], (char)pixel[2]};
                out.write(rgb, 3);
            }
        }
    }
}

int main(int argc, char** argv)
{
    if(argc < 2)
    {
        std::cerr << "usage: " << argv[0] << " output.ppm\n";
        return 1;
    }

    Options::nologs = true;
    Options::ACCELERATED = false;
    Options::SMOOTH_SIBLINGS = false;
    Options::VOXEL_SHELL_TYPE = 0;

    /*
    * Large tree: edge 32, depth 5. Built first.
    */
    Voxel largeVoxels[] = {
        {{0, 0, 0}, 0xF03020FF},
        {{3, 0, 0}, 0x30F02080},
        {{-2, 1, 1}, 0x2060F080}
    };
    Octree large;
    large.initRoot(32, 5, 24);
    large.addVoxels(largeVoxels, 3);

    /*
    * Small tree: edge 16, depth 4. Built second; it never retags the large
    * tree's bounds.
    */
    Voxel smallVoxels[] = {
        {{0, 0, 0}, 0xFFD060FF},
        {{1, -1, 0}, 0x60D0FF80}
    };
    Octree small;
    small.initRoot(16, 4, 20);
    small.addVoxels(smallVoxels, 2);

    /*
    * The large tree stays editable after the small one is built.
    */
    Voxel laterLarge = {{5, 2, 0}, 0x9040F0FF};
    large.addVoxels(&laterLarge, 1);

    Camera cameraLarge;
    cameraLarge.rotate({0.0f, 1.0f, 0.0f}, 0.15f);
    Mat3f basisLarge;
    cameraLarge.getBasis(&basisLarge);

    Camera cameraSmall;
    cameraSmall.rotate({1.0f, 0.0f, 0.0f}, 0.1f);
    Mat3f basisSmall;
    cameraSmall.getBasis(&basisSmall);

    const unsigned int width = 32;
    const unsigned int height = 32;
    std::vector<cpuframe::ViewportInput> viewports;
    viewports.push_back(makeViewport(&large, basisLarge,
        {0.0f, 0.0f, -20.0f}, width, height, {10, 10, 24, 255}));
    viewports.push_back(makeViewport(&small, basisSmall,
        {0.0f, 0.0f, -14.0f}, width, height, {24, 10, 10, 255}));

    const cpuframe::BatchResult batch = cpuframe::renderBatch(
        viewports, 4, 4, cpuframe::TaskOrder::Interleaved);

    std::ofstream out(argv[1], std::ios::binary | std::ios::trunc);
    if(!out)
    {
        std::cerr << "cannot open " << argv[1] << '\n';
        return 1;
    }
    out << "P6\n" << width << ' ' << (height * batch.frames.size())
        << "\n255\n";
    for(const cpuframe::ViewportFrame& frame : batch.frames)
    {
        writeFrame(out, frame);
    }
    out.close();

    std::cout << "viewports=" << batch.stats.viewportsCompleted
        << " pixel_queries=" << batch.stats.pixelQueries
        << " workers=" << batch.stats.workerThreads << '\n';
    return 0;
}
