/*
 * Cloud of Voxels (CoV) project
 * CPU side, GL-free multi-viewport batch frame rendering.
 * Creation Date: 2025
 */

#ifndef CPUFRAME_H
#define CPUFRAME_H

#include "Octree.h"
#include <vector>

namespace cpuframe
{
    /*
    * Fixed sampling step shared by every viewport, matching the
    * Core::process pixels path with Options::PIXEL_STEP == 1.
    */
    const unsigned int PIXEL_STEP = 1;

    /*
    * A single 8-bit RGBA sample.
    */
    struct ColorRGBA
    {
        unsigned char r;
        unsigned char g;
        unsigned char b;
        unsigned char a;
    };

    /*
    * Order in which row blocks are enqueued. It never changes the pixels,
    * only the scheduling order, so both orders must yield identical frames.
    */
    enum class TaskOrder
    {
        Sequential,
        Interleaved
    };

    /*
    * Immutable description of one viewport in a batch. The tree is read-only
    * for the whole batch; the camera basis and position are snapshots owned
    * by this struct. Transparency blending parameters (transparency switch,
    * minimum layer spacing and scan glitch repair) travel with the viewport
    * instead of being read from the global Options defaults.
    */
    struct ViewportInput
    {
        const Octree* tree;
        Mat3f basis;
        Vec3<float> position;
        unsigned int width;
        unsigned int height;
        float perspectiveFactor;
        float zoom;
        float maxRayDistance;
        unsigned int maxVoxelsByRay;
        unsigned int minVoxelProximity;
        bool transparency;
        bool avoidGlitches;
        ColorRGBA background;
    };

    /*
    * One rendered viewport. The RGBA buffer is row-major
    * (4 * width * height bytes) and is owned exclusively by this frame.
    */
    struct ViewportFrame
    {
        std::vector<unsigned char> rgba;
        unsigned int width;
        unsigned int height;
        const Octree* tree;
    };

    /*
    * Counters actually observed while running the batch.
    */
    struct BatchStats
    {
        unsigned int viewportsCompleted;
        unsigned long long pixelQueries;
        unsigned int workerThreads;
    };

    /*
    * Frames are returned in the same order as the input viewports, each with
    * its own non-shared storage, followed by the batch statistics.
    */
    struct BatchResult
    {
        std::vector<ViewportFrame> frames;
        BatchStats stats;
    };

    /*
    * Renders every viewport of the batch with a fixed pool of worker threads
    * pulling row blocks of rowBlockSize rows. All inputs are validated before
    * any thread starts, so an invalid argument throws std::invalid_argument
    * and no partial frame is ever returned. workerCount must be in [1, 32],
    * rowBlockSize must be positive. The trees are only read, so several
    * workers may query the same tree concurrently.
    */
    BatchResult renderBatch(const std::vector<ViewportInput>& viewports,
        unsigned int workerCount, unsigned int rowBlockSize,
        TaskOrder order = TaskOrder::Sequential);

    /*
    * Single-viewport, single-threaded rendering into a caller-owned
    * row-major RGBA buffer of 4 * width * height bytes. It runs the exact
    * same per-pixel implementation as renderBatch and is used to back the
    * legacy Core::process pixels path without GL. The viewport is validated
    * first and throws std::invalid_argument on bad input.
    */
    void renderPixelBuffer(const ViewportInput& viewport,
        unsigned char* pixels);
}

#endif
