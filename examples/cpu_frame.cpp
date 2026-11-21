/*
 * CPU frame example: two octrees and two cameras rendered by one batch,
 * then written to a PPM file.
 *
 * Usage: cpu_frame <output.ppm>
 */

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "headers/CPUFrame.h"
#include "headers/Camera.h"

namespace
{
    void configureOptions()
    {
        Options::nologs = true;
        Options::ACCELERATED = false;
        Options::SMOOTH_SIBLINGS = false;
        Options::AVOID_SCAN_GLITCHES = true;
        Options::TRANSPARENCY = true;
        Options::VOXEL_SHELL_TYPE = 0;
        Options::SHELL_COLOR = 0;
        Options::MAX_VOXELS_BY_RAY = 12;
        Options::MIN_VOXELS_PROXIMITY = 2;
        Options::PIXEL_STEP = 1;
        Options::PERSPECTIVE_FACTOR = 0.0f;
        Options::ZOOM_FACTOR = 1.0f;
    }

    struct CameraSnapshot
    {
        Mat3f basis;
        Vec3<float> position;
    };

    CameraSnapshot snapshot(Camera& camera)
    {
        CameraSnapshot value;
        camera.getBasis(&value.basis);
        value.position = camera.getPosition();
        return value;
    }

    /*
    * A small procedural cloud of voxels: a solid blocky shape with two
    * embedded opaque cores, inserted through the regular public insertion
    * path. The coordinates are spread over the tree so several depth levels
    * are exercised.
    */
    void fillTree(Octree& tree, const int rootSize, const int depth,
    const int rayLength, const Vec3<SI>& center, const int extent,
    const unsigned int tint)
    {
        tree.initRoot((SUI)rootSize, (SUI)depth, (SI)rayLength);

        std::vector<Voxel> voxels;
        for(int x = -extent; x <= extent; x++)
        {
            for(int y = -extent; y <= extent; y++)
            {
                for(int z = -extent; z <= extent; z++)
                {
                    const int manhattan = (x < 0 ? -x : x) +
                    (y < 0 ? -y : y) + (z < 0 ? -z : z);
                    const bool core = (x == 0 && z == 0) ||
                    (y == 0 && z == 0) || (x == 0 && y == 0);
                    // Drop the far corners to make the shape less cubic.
                    if(manhattan > extent * 2 + 1)
                    {
                        continue;
                    }

                    const unsigned int shade = (unsigned int)(
                    48 + ((x + extent) * 20 + (y + extent) * 11 +
                    (z + extent) * 5) % 160);
                    const unsigned int alpha = core ? 0xFF : 0xE0;
                    const unsigned int color =
                    ((((tint >> 16) & 0xFF) * shade / 255) << 24) |
                    ((((tint >> 8) & 0xFF) * shade / 255) << 16) |
                    (((tint & 0xFF) * shade / 255) << 8) | alpha;
                    Voxel voxel;
                    voxel.coordinates.x = (SI)(center.x + x);
                    voxel.coordinates.y = (SI)(center.y + y);
                    voxel.coordinates.z = (SI)(center.z + z);
                    voxel.color = color;
                    voxels.push_back(voxel);
                }
            }
        }
        if(!voxels.empty())
        {
            tree.addVoxels(voxels.data(), (UI)voxels.size());
        }
    }

    CameraSnapshot makeCamera(const Vec3<float>& position,
    const Vec3<float> axis, const float angle)
    {
        Camera camera;
        camera.setPosition(position.x, position.y, position.z);
        camera.rotate(axis, angle);
        return snapshot(camera);
    }

    bool writePpm(const char* const path,
    const std::vector<CPUFrameResult>& frames)
    {
        // Lay the viewports out side by side, in input order.
        int width = 0;
        int height = 0;
        for(size_t i = 0; i < frames.size(); i++)
        {
            width += frames[i].width;
            if(frames[i].height > height)
            {
                height = frames[i].height;
            }
        }
        if(width <= 0 || height <= 0)
        {
            return false;
        }

        std::vector<UC> canvas((size_t)width * (size_t)height * 3, 0);
        int offsetX = 0;
        for(size_t i = 0; i < frames.size(); i++)
        {
            const CPUFrameResult& frame = frames[i];
            for(int y = 0; y < frame.height; y++)
            {
                for(int x = 0; x < frame.width; x++)
                {
                    const size_t src =
                    ((size_t)y * frame.width + x) * 4;
                    const size_t dst =
                    ((size_t)y * width + (offsetX + x)) * 3;
                    canvas[dst] = frame.pixels[src];
                    canvas[dst + 1] = frame.pixels[src + 1];
                    canvas[dst + 2] = frame.pixels[src + 2];
                }
            }
            offsetX += frame.width;
        }

        FILE* file = fopen(path, "wb");
        if(file == NULL)
        {
            return false;
        }
        fprintf(file, "P6\n%d %d\n255\n", width, height);
        const size_t written =
        fwrite(canvas.data(), 1, canvas.size(), file);
        fclose(file);
        return written == canvas.size();
    }
}

int main(int argc, char** argv)
{
    if(argc < 2 || argv[1][0] == '\0')
    {
        fprintf(stderr, "Usage: %s <output.ppm>\n", argv[0]);
        return 1;
    }

    configureOptions();

    // The batch settings are explicit; the legacy globals above only matter
    // to the window entry point.
    Octree large;
    fillTree(large, 32, 5, 24, {0, 0, 0}, 10, 0xFF5522);
    Octree small;
    fillTree(small, 16, 4, 16, {0, 0, 0}, 5, 0x22AAFF);

    // Two independent cameras, one per viewport.
    const CameraSnapshot front = makeCamera(
    {0.0f, 0.0f, -12.0f}, {0.0f, 1.0f, 0.0f}, 0.15f);
    const CameraSnapshot side = makeCamera(
    {0.0f, 0.0f, -9.0f}, {1.0f, 0.0f, 0.0f}, -0.25f);

    std::vector<CPUFrameViewport> viewports;

    CPUFrameViewport first;
    first.octree = &large;
    first.basis = front.basis;
    first.position = front.position;
    first.width = 96;
    first.height = 96;
    first.pixelStep = 1;
    first.perspectiveFactor = 0.06f;
    first.zoom = 1.0f;
    first.background = {6, 8, 12, 255};
    first.transparentR = 1.0f;
    first.transparentG = 1.0f;
    first.transparentB = 1.0f;
    first.transparentA = 1.0f;
    viewports.push_back(first);

    CPUFrameViewport second;
    second.octree = &small;
    second.basis = side.basis;
    second.position = side.position;
    second.width = 64;
    second.height = 96;
    second.pixelStep = 1;
    second.perspectiveFactor = 0.0f;
    second.zoom = 1.0f;
    second.background = {6, 8, 12, 255};
    second.transparentR = 1.0f;
    second.transparentG = 1.0f;
    second.transparentB = 1.0f;
    second.transparentA = 1.0f;
    viewports.push_back(second);

    CPUFrameOptions options = cpuFrameDefaultOptions();
    options.workers = 4;
    options.tileRows = 4;
    options.transparency = true;
    options.minVoxelsProximity = 2.0f;
    options.maxVoxelsByRay = 12;
    options.avoidScanGlitches = true;

    CPUFrameStats stats;
    const std::vector<CPUFrameResult> frames =
    cpuFrameRender(viewports, options, &stats);

    if(!writePpm(argv[1], frames))
    {
        fprintf(stderr, "Failed to write %s\n", argv[1]);
        return 1;
    }

    printf("Wrote %s: %u viewports, %llu pixel queries, %u workers\n",
    argv[1], stats.viewports, stats.pixelQueries, stats.workers);
    return 0;
}
