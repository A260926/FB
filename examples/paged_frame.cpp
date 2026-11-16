/*
 * Cloud of Voxels (CoV) project
 *
 * Paged CPU frame example.  Builds a real octree, exports an immutable
 * snapshot, opens it read-only through a one-page cache and renders a PPM
 * image using the batched, resumable ray tracer.  No OpenGL, display or
 * network is used.
 *
 * Usage: paged_frame [output.ppm] [width] [height] [pageBudget]
 */

#include "../headers/PagedOctree.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace
{
    void setOptions()
    {
        Options::nologs = true;
        Options::TRANSPARENCY = true;
        Options::SMOOTH_SIBLINGS = false;
        Options::VOXEL_SHELL_TYPE = 0;
        Options::AVOID_SCAN_GLITCHES = false;
        Options::ACCELERATED = false;
        Options::OCTREE_SIZE = 32;
        Options::MIN_VOXELS_PROXIMITY = 0;
    }

    /*
    * A small, deterministic scene: a hollow-ish coloured cube shell plus a
    * diagonal string of voxels.  It exercises empty interior nodes, coloured
    * leaves and several depths of the real Octree.
    */
    std::vector<Voxel> buildScene()
    {
        std::vector<Voxel> voxels;
        const int half = 12;
        for(int x = -half; x <= half; ++x)
        {
            for(int y = -half; y <= half; ++y)
            {
                for(int z = -half; z <= half; ++z)
                {
                    const int shell = std::abs(x) == half ||
                    std::abs(y) == half || std::abs(z) == half;
                    if(!shell) continue;
                    Voxel v;
                    v.coordinates.x = (SI)x;
                    v.coordinates.y = (SI)y;
                    v.coordinates.z = (SI)z;
                    v.color = 0xFF0000FFu |
                    ((UI)((x + half) * 9) << 8) |
                    ((UI)((y + half) * 9) << 16);
                    voxels.push_back(v);
                }
            }
        }
        for(int i = -8; i <= 8; ++i)
        {
            Voxel v;
            v.coordinates.x = (SI)i;
            v.coordinates.y = (SI)i;
            v.coordinates.z = (SI)(-i);
            v.color = 0x00FF00FFu;
            voxels.push_back(v);
        }
        return voxels;
    }
}

int main(int argc, char** argv)
{
    const char* outPath = argc > 1 ? argv[1] : "frame.ppm";
    int width = argc > 2 ? std::atoi(argv[2]) : 96;
    int height = argc > 3 ? std::atoi(argv[3]) : 96;
    std::size_t budget = argc > 4 ? (std::size_t)std::atoi(argv[4]) : 1;
    if(width <= 0) width = 96;
    if(height <= 0) height = 96;
    if(budget == 0) budget = 1;

    setOptions();

    Octree tree;
    tree.initRoot(32, 5, 64);
    std::vector<Voxel> voxels = buildScene();
    tree.addVoxels(&voxels[0], (const UI)voxels.size());

    const std::string snapshot = std::string(outPath) + ".pcoat";
    std::string error;
    if(!tree.exportPagedSnapshot(snapshot.c_str(), 16, &error))
    {
        std::fprintf(stderr, "export failed: %s\n", error.c_str());
        return 1;
    }

    paged::SnapshotReader reader;
    if(!reader.open(snapshot.c_str(), &error))
    {
        std::fprintf(stderr, "open failed: %s\n", error.c_str());
        return 2;
    }

    /*
    * Orthographic rays along +z from in front of the scene, one per pixel.
    */
    std::vector<paged::RayRequest> rays((std::size_t)width * (std::size_t)height);
    const double scale = 40.0 / (double)width;
    for(int y = 0; y < height; ++y)
    {
        for(int x = 0; x < width; ++x)
        {
            paged::RayRequest& r =
            rays[(std::size_t)y * (std::size_t)width + (std::size_t)x];
            r.originX = ((double)x - width * 0.5) * scale;
            r.originY = ((double)y - height * 0.5) * scale;
            r.originZ = -80.0;
            r.dirX = 0.0;
            r.dirY = 0.0;
            r.dirZ = 1.0;
            r.tMin = 0.0;
            r.tMax = 200.0;
        }
    }

    std::unique_ptr<paged::PagedFrame> frame = paged::PagedFrame::openBatch(
    &reader, &rays[0], rays.size(), budget, &error);
    if(!frame.get())
    {
        std::fprintf(stderr, "frame setup failed: %s\n", error.c_str());
        return 3;
    }
    if(!frame->start())
    {
        std::fprintf(stderr, "frame start failed\n");
        return 4;
    }

    /*
    * Drive the batch.  Progress is sampled between rounds; completion only
    * appears once every ray is terminal.
    */
    int rounds = 0;
    while(!frame->step(0))
    {
        ++rounds;
        if(rounds > 1000000)
        {
            std::fprintf(stderr, "frame did not converge\n");
            return 5;
        }
        if(frame->hasFailure())
        {
            std::fprintf(stderr, "page load failed: %s\n",
            frame->pageCache() ? "io" : "unknown");
            return 6;
        }
        if(!frame->isDone() && rounds % 64 == 0)
        {
            std::fprintf(stderr, "\rprogress %.1f%%", frame->progress() * 100);
        }
    }
    std::fprintf(stderr, "\n");

    std::vector<paged::RayResult> results;
    frame->results(&results);

    const char* path = outPath;
    std::FILE* fp = std::fopen(path, "wb");
    if(fp == NULL)
    {
        std::fprintf(stderr, "cannot write %s\n", path);
        return 7;
    }
    std::fprintf(fp, "P6\n%d %d\n255\n", width, height);
    std::vector<unsigned char> pixels((std::size_t)width * (std::size_t)height * 3);
    std::size_t hits = 0;
    for(std::size_t i = 0; i < results.size(); ++i)
    {
        unsigned char r = 0, g = 0, b = 0;
        if(results[i].status == paged::TRACE_HIT)
        {
            r = results[i].r;
            g = results[i].g;
            b = results[i].b;
            ++hits;
        }
        pixels[i * 3 + 0] = r;
        pixels[i * 3 + 1] = g;
        pixels[i * 3 + 2] = b;
    }
    std::fwrite(&pixels[0], 1, pixels.size(), fp);
    std::fclose(fp);

    const paged::CacheStats& st = frame->stats();
    std::printf("PAGED_FRAME size=%dx%d rays=%zu hits=%zu budget=%zu "
    "visits=%llu requests=%llu loads=%llu evictions=%llu peak=%llu "
    "complete=%d\n", width, height, results.size(), hits, budget,
    (unsigned long long)st.nodeVisits,
    (unsigned long long)st.pageRequests,
    (unsigned long long)st.pageLoads,
    (unsigned long long)st.pageEvictions,
    (unsigned long long)st.peakResident, frame->isComplete() ? 1 : 0);
    std::remove(snapshot.c_str());
    return hits > 0 ? 0 : 8;
}
