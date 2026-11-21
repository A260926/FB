// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

#ifndef _USE_MATH_DEFINES
#define _USE_MATH_DEFINES
#endif
#include <cmath>
#include <chrono>
#include <vector>

#if defined(NANOVDB_USE_CUDA)
#include <nanovdb/cuda/GridHandle.cuh> // for cuda::copyTo, the explicit host->device grid transfer
#endif
using BufferT = nanovdb::HostBuffer;
#include <nanovdb/GridHandle.h>
#include <nanovdb/io/IO.h>
#include <nanovdb/math/Ray.h>
#include <nanovdb/tools/FogAdjoint.h>

#include "common.h"

void runNanoVDB(nanovdb::GridHandle<BufferT>& handle, int numIterations, int width, int height, BufferT& imageBuffer)
{
    using GridT = nanovdb::FloatGrid;
    using CoordT = nanovdb::Coord;
    using RealT = float;
    using Vec3T = nanovdb::math::Vec3<RealT>;
    using RayT = nanovdb::math::Ray<RealT>;

    auto *h_grid = handle.grid<float>();
    if (!h_grid)
        throw std::runtime_error("GridHandle does not contain a valid host grid");

    float* h_outImage = reinterpret_cast<float*>(imageBuffer.data());

    float              wBBoxDimZ = (float)h_grid->worldBBox().dim()[2] * 2;
    Vec3T              wBBoxCenter = Vec3T(h_grid->worldBBox().min() + h_grid->worldBBox().dim() * 0.5f);
    nanovdb::CoordBBox treeIndexBbox = h_grid->tree().bbox();
    std::cout << "Bounds: "
              << "[" << treeIndexBbox.min()[0] << "," << treeIndexBbox.min()[1] << "," << treeIndexBbox.min()[2] << "] -> ["
              << treeIndexBbox.max()[0] << "," << treeIndexBbox.max()[1] << "," << treeIndexBbox.max()[2] << "]" << std::endl;

    RayGenOp<Vec3T> rayGenOp(wBBoxDimZ, wBBoxCenter);

    {
        // CPU path: march every pixel through the shared fog::forward
        // implementation with h=0.5, k=0.1 and zero emission. The catalog and
        // the ray batch (original RayGenOp, checkerboard background, rays
        // clipped to the world bounds like the original index-bbox clip) are
        // built once; only the forward integration is timed.
        using ClockT = std::chrono::high_resolution_clock;
        namespace fog = nanovdb::tools::fog;

        fog::Catalog catalog(*h_grid);
        const nanovdb::Vec3dBBox& wBBox = h_grid->worldBBox();

        std::vector<fog::Ray> rays(size_t(width) * size_t(height));
        std::vector<char>   hits(size_t(width) * size_t(height), 0);
        std::vector<float>  backgrounds(size_t(width) * size_t(height), 0.0f);
        for (int i = 0; i < width * height; ++i) {
            Vec3T rayEye;
            Vec3T rayDir;
            rayGenOp(i, width, height, rayEye, rayDir);
            // checkerboard background, identical to CompositeOp
            const uint32_t x = uint32_t(i % width), y = uint32_t(i / width);
            const int      mask = 1 << 7;
            const float    bg = ((x & mask) ^ (y & mask)) ? 1.0f : 0.5f;
            backgrounds[size_t(i)] = bg;

            fog::Ray& ray = rays[size_t(i)];
            ray.origin = nanovdb::Vec3d(rayEye[0], rayEye[1], rayEye[2]);
            ray.direction = nanovdb::Vec3d(rayDir[0], rayDir[1], rayDir[2]);
            ray.background = nanovdb::Vec3d(double(bg));
            nanovdb::math::Ray<double> wRay(ray.origin, ray.direction);
            if (wRay.clip(wBBox)) {
                ray.t0 = wRay.t0();
                ray.t1 = wRay.t1();
                hits[size_t(i)] = 1;
            } else {
                ray.t0 = 0.0;
                ray.t1 = 1.0; // unused, the pixel only shows the background
            }
        }

        float durationAvg = 0;
        for (int iter = 0; iter < numIterations; ++iter) {
            auto t0 = ClockT::now();
            fog::ForwardResult res = fog::forward(catalog, catalog.initialDensity(),
                                                  0.1, nanovdb::Vec3d(0.0), rays, 0.5);
            auto t1 = ClockT::now();
            for (int i = 0; i < width * height; ++i) {
                h_outImage[i] = hits[size_t(i)] ? float(res.rgb[size_t(i)][0]) : backgrounds[size_t(i)];
            }
            float duration = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count() / 1000.f;
            //std::cout << "Duration(NanoVDB-Host) = " << duration << " ms" << std::endl;
            durationAvg += duration;
        }
        durationAvg /= numIterations;
        std::cout << "Average Duration(NanoVDB-Host) = " << durationAvg << " ms" << std::endl;

        saveImage("raytrace_fog_volume-nanovdb-host.pfm", width, height, (float*)imageBuffer.data());
    }

#if defined(NANOVDB_USE_CUDA)
    CompositeOp compositeOp;
    // device path: the original per-pixel integrator, unchanged
    auto renderOp = [width, height, rayGenOp, compositeOp, treeIndexBbox] __hostdev__(int start, int end, float* image, const GridT* grid) {
        // get an accessor.
        auto acc = grid->tree().getAccessor();

        for (int i = start; i < end; ++i) {
            Vec3T rayEye;
            Vec3T rayDir;
            rayGenOp(i, width, height, rayEye, rayDir);
            // generate ray.
            RayT wRay(rayEye, rayDir);
            // transform the ray to the grid's index-space.
            RayT iRay = wRay.worldToIndexF(*grid);
            // clip to bounds.
            if (iRay.clip(treeIndexBbox) == false) {
                compositeOp(image, i, width, height, 0.0f, 0.0f);
                return;
            }
            // integrate...
            const float dt = 0.5f;
            float       transmittance = 1.0f;
            for (float t = iRay.t0(); t < iRay.t1(); t += dt) {
                float sigma = acc.getValue(CoordT::Floor(iRay(t))) * 0.1f;
                transmittance *= 1.0f - sigma * dt;
            }
            // write transmittance.
            compositeOp(image, i, width, height, 0.0f, 1.0f - transmittance);
        }
    };

    // deep-copy the grid to the device; the returned handle validates it there
    auto deviceHandle = nanovdb::cuda::copyTo<nanovdb::cuda::Buffer<std::byte>>(handle);

    auto* d_grid = deviceHandle.deviceGrid<float>();
    if (!d_grid)
        throw std::runtime_error("GridHandle does not contain a valid device grid");

    nanovdb::cuda::Buffer<float> deviceImage(cudaStream_t(0), size_t(width) * height, nanovdb::cuda::noInit);
    float* d_outImage = deviceImage.data();

    {
        float durationAvg = 0;
        for (int i = 0; i < numIterations; ++i) {
            float duration = renderImage(true, renderOp, width, height, d_outImage, d_grid);
            //std::cout << "Duration(NanoVDB-Cuda) = " << duration << " ms" << std::endl;
            durationAvg += duration;
        }
        durationAvg /= numIterations;
        std::cout << "Average Duration(NanoVDB-Cuda) = " << durationAvg << " ms" << std::endl;

        cudaMemcpy(imageBuffer.data(), deviceImage.data(), size_t(width) * height * sizeof(float), cudaMemcpyDeviceToHost);
        saveImage("raytrace_fog_volume-nanovdb-cuda.pfm", width, height, (float*)imageBuffer.data());
    }
#endif
}
