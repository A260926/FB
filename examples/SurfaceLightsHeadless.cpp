// Headless direct-lighting demo. It builds a real sparse voxel octree from an
// in-memory raw scene, binds two emissive voxel payloads (a red and a cyan
// face-light region), places a non-emissive occluder between them and a
// receiver wall, and renders the receiver's direct illumination to PPM
// images using VoxelSurfaceLights + estimateDirect. No window, SDL or GPU is
// involved. A second image demonstrates one live setEmission re-light without
// rescanning or rebuilding any geometry.

#include "VoxelData.hpp"
#include "VoxelOctree.hpp"
#include "VoxelSurfaceLights.hpp"
#include "VoxelDirect.hpp"
#include "thread/ThreadUtils.hpp"
#include "thread/ThreadPool.hpp"

#include <cstdint>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <vector>

namespace {

const uint32 ReceiverKey = 0x70070000u;
const uint32 RedKey      = 0x52000000u;
const uint32 CyanKey     = 0x43000000u;
const uint32 BlockerKey  = 0x42000000u;

const int N = 16;
const int ImageRes = 96;
const int SamplesPerPixel = 24;

size_t voxelIndex(int x, int y, int z)
{
    return size_t(x) + N * (size_t(y) + N * size_t(z));
}

VoxelOctree *buildScene(const char *rawPath)
{
    std::vector<uint32> voxels(size_t(N) * N * N, 0);

    // Receiver wall facing +x.
    for (int y = 3; y < 13; ++y)
    for (int z = 3; z < 13; ++z)
        voxels[voxelIndex(3, y, z)] = ReceiverKey;

    // Two adjacent emissive slabs on the far side, different payloads/colors.
    for (int z = 3; z < 13; ++z) {
        for (int y = 3; y < 8; ++y)
            voxels[voxelIndex(13, y, z)] = RedKey;
        for (int y = 8; y < 13; ++y)
            voxels[voxelIndex(13, y, z)] = CyanKey;
    }

    // One-cell-thick non-emissive occluder band casting a shadow stripe.
    for (int y = 6; y < 10; ++y)
    for (int z = 3; z < 13; ++z)
        voxels[voxelIndex(8, y, z)] = BlockerKey;

    {
        std::ofstream f(rawPath, std::ios::binary);
        uint32 side = N;
        for (int k = 0; k < 3; ++k)
            f.write(reinterpret_cast<const char *>(&side), 4);
        f.write(reinterpret_cast<const char *>(voxels.data()),
                voxels.size() * sizeof(uint32));
    }

    static VoxelData *input = nullptr;
    if (!input)
        input = new VoxelData(rawPath, 256u * 1024u * 1024u);
    return new VoxelOctree(input);
}

uint8_t toneMap(float v)
{
    if (v < 0.0f) v = 0.0f;
    float mapped = v / (1.0f + v);             // simple filmic compression
    float gamma = std::pow(mapped, 1.0f / 2.2f);
    int value = int(gamma * 255.0f + 0.5f);
    return uint8_t(value > 255 ? 255 : value);
}

void render(const VoxelSurfaceLights &lights, const char *ppmPath)
{
    std::vector<uint8_t> pixels(size_t(ImageRes) * ImageRes * 3);
    Vec3 normal(1.0f, 0.0f, 0.0f);
    Vec3 albedo(0.8f, 0.8f, 0.8f);

    // Receiver points lie on the +x face of the x=3 wall.
    const float receiverX = 1.0f + 4.0f / N;
    const float lo = 1.0f + 4.0f / N;
    const float hi = 1.0f + 12.0f / N;

    for (int py = 0; py < ImageRes; ++py) {
        float y = lo + (hi - lo) * (py + 0.5f) / ImageRes;
        for (int pz = 0; pz < ImageRes; ++pz) {
            float z = lo + (hi - lo) * (pz + 0.5f) / ImageRes;
            Vec3 P(receiverX, y, z);

            Vec3 accum(0.0f, 0.0f, 0.0f);
            for (int s = 0; s < SamplesPerPixel; ++s) {
                double u0 = (s + 0.5) / SamplesPerPixel;
                double u1 = ((s * 7 + py * 3) % 101) / 101.0;
                double u2 = ((s * 13 + pz * 5) % 97) / 97.0;
                accum += estimateDirect(lights, P, normal, albedo, u0, u1, u2);
            }
            accum *= 1.0f / float(SamplesPerPixel);

            size_t out = (size_t(py) * ImageRes + pz) * 3;
            pixels[out + 0] = toneMap(accum.x);
            pixels[out + 1] = toneMap(accum.y);
            pixels[out + 2] = toneMap(accum.z);
        }
    }

    std::ofstream ppm(ppmPath, std::ios::binary);
    ppm << "P6\n" << ImageRes << ' ' << ImageRes << "\n255\n";
    ppm.write(reinterpret_cast<const char *>(pixels.data()), pixels.size());
}

} // namespace

int main()
{
    ThreadUtils::startThreads(2);

    VoxelOctree *tree = buildScene(".task-build/headless_scene.raw");
    VoxelSurfaceLights lights(tree);

    std::printf("Exposed emissive/occluder surface area: %.6f over %zu faces\n",
                lights.surfaceArea(), lights.faceCount());

    // Initial lighting: warm red and cool cyan face lights.
    lights.setEmission(RedKey, Vec3(3.0f, 0.4f, 0.25f));
    lights.setEmission(CyanKey, Vec3(0.25f, 1.6f, 3.0f));
    std::printf("Initial total mass: %.6f\n", lights.totalMass());
    render(lights, ".task-build/headless_initial.ppm");

    // One live re-light: switch the red region off and recolor the cyan one.
    // Geometry, face ids and the octree are untouched by these calls.
    size_t facesBefore = lights.faceCount();
    lights.setEmission(RedKey, Vec3(0.0f, 0.0f, 0.0f));
    lights.setEmission(CyanKey, Vec3(0.6f, 2.6f, 0.8f));
    std::printf("After re-light total mass: %.6f (faces stable: %d)\n",
                lights.totalMass(), int(lights.faceCount() == facesBefore));
    render(lights, ".task-build/headless_relit.ppm");

    std::printf("Wrote .task-build/headless_initial.ppm and "
                ".task-build/headless_relit.ppm (%dx%d P6 PPM)\n",
                ImageRes, ImageRes);
    delete ThreadUtils::pool;
    ThreadUtils::pool = nullptr;
    return 0;
}
