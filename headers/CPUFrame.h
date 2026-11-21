/*
 * Cloud of Voxels (CoV) project
 * Author: m-c/d, mcidclan
 * Creation Date: 2025
 *
 * CPU side, GL-free batched frame renderer.
 *
 * Every call renders a batch of independent viewports, each one owning its
 * octree, its camera snapshot and its output buffer. The octrees are only
 * read: from the moment the batch starts until it returns, no tree is
 * mutated, and queries may run concurrently on the same tree.
 */

#ifndef CPU_FRAME_H
#define CPU_FRAME_H

    #include <cstddef>
    #include <vector>

    #include "Octree.h"

    /*
    * One viewport of a batch.
    *
    * basis and position are a snapshot of the camera: basis must be an
    * orthonormal basis (unit length, mutually orthogonal), position the
    * camera origin. width/height are positive and even. background is the
    * RGBA value written where no voxel is hit. transpR/G/B/A are the
    * per-channel opacity (0..255) used when blending transparent hits.
    */
    struct CPUFrameViewport
    {
        const Octree* octree;
        Mat3f basis;
        Vec3<float> position;
        int width;
        int height;
        unsigned int pixelStep;
        float perspectiveFactor;
        float zoom;
        Color background;
        float transparentR;
        float transparentG;
        float transparentB;
        float transparentA;
    };

    /*
    * Batch configuration.
    *
    * workers:   number of worker threads, in [1, 32]. At least two workers
    *            query the same tree concurrently when the batch has more than
    *            one row block per viewport.
    * tileRows:  height, in pixel rows, of the row blocks the viewports are
    *            split into.
    * transparency / minVoxelsProximity / maxVoxelsByRay: the query and
    *            blending rules, copied once at the start of the batch.
    * avoidScanGlitches: enables the scan patch used while stepping.
    */
    struct CPUFrameOptions
    {
        int workers;
        int tileRows;
        bool transparency;
        float minVoxelsProximity;
        unsigned int maxVoxelsByRay;
        bool avoidScanGlitches;
    };

    /*
    * Ready to use options, matching the legacy window defaults for the
    * geometric parameters (no transparency, single voxel per ray).
    */
    CPUFrameOptions cpuFrameDefaultOptions();

    /*
    * One rendered viewport. pixels is row-major RGBA (4 bytes per pixel),
    * width*height*4 bytes, not shared with any other output.
    */
    struct CPUFrameResult
    {
        int width;
        int height;
        std::vector<UC> pixels;
    };

    /*
    * Batch statistics: number of viewports actually completed, total number
    * of pixel ray queries, and number of worker threads created.
    */
    struct CPUFrameStats
    {
        unsigned int viewports;
        unsigned long long pixelQueries;
        unsigned int workers;
    };

    /*
    * Renders the batch. Throws std::invalid_argument, before starting any
    * work, if a viewport or the options are invalid.
    */
    std::vector<CPUFrameResult> cpuFrameRender(
    const std::vector<CPUFrameViewport>& viewports,
    const CPUFrameOptions& options,
    CPUFrameStats* const stats = NULL);

    /*
    * Per pixel parameters of the shared implementation. Used by cpuFrameRender
    * and by the legacy Core pixel path (GL only displays the result).
    *
    * maxRayDistance <= 0 falls back to the octree's own ray length.
    * transparentR/G/B/A are per-channel opacity weights in [0, 1] applied to
    * every hit; background is the RGBA written on a miss.
    */
    struct CPUFramePixelConfig
    {
        unsigned int pixelStep;
        float perspectiveFactor;
        float zoom;
        Color background;
        float transparentR;
        float transparentG;
        float transparentB;
        float transparentA;
        unsigned int maxVoxelsByRay;
        bool transparency;
        float minVoxelsProximity;
        bool avoidScanGlitches;
        float maxRayDistance;
    };

    void cpuFrameRenderPixel(const Octree& octree, const Mat3f& basis,
    const Vec3<float>& position, const int pixelX, const int pixelY,
    const CPUFramePixelConfig& config, UC* const rgba);

#endif
