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

/*
 * Headless dielectric example.
 *
 * Generates raw voxel data, builds a real VoxelOctree through VoxelData,
 * renders two contacting glasses separated by an air slot with linear RGB
 * dielectric transport, and writes .task-build/dielectric.ppm.  It also
 * reports interface counts and cumulative descriptor reads.
 *
 * The example intentionally does not open a window and does not touch SDL.
 */

#include "VoxelData.hpp"
#include "VoxelOctree.hpp"
#include "VoxelInterfaces.hpp"
#include "VoxelDielectric.hpp"

#include "thread/ThreadUtils.hpp"
#include "thread/ThreadPool.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

const int Resolution = 64;
const int ImageWidth = 320;
const int ImageHeight = 240;

const uint32 PayloadGlassA = 0x00C0FFEEu;
const uint32 PayloadGlassB = 0x00DEAD01u;

void writeRaw(const char *path) {
    std::vector<uint32> voxels(size_t(Resolution)*Resolution*Resolution, 0);

    for (int z = 0; z < Resolution; ++z) {
        for (int y = 0; y < Resolution; ++y) {
            for (int x = 0; x < Resolution; ++x) {
                /* Two touching glass slabs (a direct glass/glass contact) on
                 * the left, a narrow air slot, then a second glass slab.  The
                 * surround is air so the ray can enter and leave the root. */
                uint32 value = 0;
                if (z >= 16 && z < 48 && y >= 8 && y < 56) {
                    if (x >= 4 && x < 22)
                        value = PayloadGlassA;
                    else if (x >= 22 && x < 40)
                        value = PayloadGlassB;
                    else if (x >= 40 && x < 46)
                        value = 0; /* air slot */
                    else if (x >= 46 && x < 60)
                        value = PayloadGlassA;
                }
                voxels[size_t(x) + size_t(Resolution)*(size_t(y) +
                    size_t(Resolution)*size_t(z))] = value;
            }
        }
    }

    std::ofstream file(path, std::ios::binary);
    int header = Resolution;
    for (int i = 0; i < 3; ++i)
        file.write(reinterpret_cast<const char *>(&header), 4);
    file.write(reinterpret_cast<const char *>(voxels.data()),
               std::streamsize(voxels.size()*4));
}

std::vector<DielectricMaterial> buildMaterials() {
    std::vector<DielectricMaterial> materials;
    /* 0: air */
    materials.push_back(DielectricMaterial(1.0f, Vec3(0.0f)));
    /* 1: dense flint-ish glass with a faint blue-green tint */
    materials.push_back(DielectricMaterial(1.62f, Vec3(0.05f, 0.02f, 0.10f)));
    /* 2: crown-ish glass with a warmer tint */
    materials.push_back(DielectricMaterial(1.42f, Vec3(0.10f, 0.04f, 0.01f)));
    return materials;
}

std::unordered_map<uint32, uint32> buildPayloadMap() {
    std::unordered_map<uint32, uint32> map;
    map[PayloadGlassA] = 1;
    map[PayloadGlassB] = 2;
    return map;
}

Vec3 background(const Vec3 &direction) {
    /* A direction-encoding environment.  Because the two glasses and the air
     * slot refract rays by different amounts, the transmitted directions (and
     * therefore colours) differ visibly across the slabs. */
    Vec3 n = direction.normalize();
    return Vec3(0.5f + 0.5f*n.x,
                0.5f + 0.5f*n.y,
                0.5f + 0.5f*n.z);
}

} /* namespace */

int main() {
    ThreadUtils::startThreads(2);

    const std::string rawPath = ".task-build/dielectric.raw";
    const std::string ppmPath = ".task-build/dielectric.ppm";

    writeRaw(rawPath.c_str());

    VoxelData data(rawPath.c_str(), size_t(64)*1024*1024);
    VoxelOctree tree(&data);

    std::vector<DielectricMaterial> materials = buildMaterials();
    std::unordered_map<uint32, uint32> payloadMap = buildPayloadMap();
    VoxelInterfaces interfaces(tree, materials, payloadMap);
    VoxelDielectric transport(interfaces);

    /* Camera sits in the air region to the left of the slabs, looking along
     * +x but tilted so rays meet the slab faces obliquely; that makes the
     * Snell bend and the glass/glass and glass/air boundaries visible. */
    const Vec3 camera(1.02f, 1.42f, 1.30f);
    const float focal = 1.0f;

    std::vector<unsigned char> image(size_t(ImageWidth)*ImageHeight*3);

    /* The root is the cube [1,2]^3; the slabs occupy z in [1.25,1.75) and
     * y in [1.125,1.875). */
    const float halfWidth = 0.34f;
    const float halfHeight = 0.30f;

    uint64 totalInterfaces = 0;
    for (int py = 0; py < ImageHeight; ++py) {
        for (int px = 0; px < ImageWidth; ++px) {
            float sx = (float(px) + 0.5f)/ImageWidth*2.0f - 1.0f;
            float sy = 1.0f - (float(py) + 0.5f)/ImageHeight*2.0f;
            Vec3 direction(focal, sx*halfWidth, sy*halfHeight);
            direction = direction.normalize();

            Vec3 origin = camera;

            DielectricTraceResult result =
                transport.trace(origin, direction, background, 8);

            totalInterfaces += result.interfaces;

            float r = std::min(1.0f, std::max(0.0f, result.radiance.x));
            float g = std::min(1.0f, std::max(0.0f, result.radiance.y));
            float b = std::min(1.0f, std::max(0.0f, result.radiance.z));
            size_t index = (size_t(py)*ImageWidth + size_t(px))*3;
            image[index + 0] = (unsigned char)(r*255.0f + 0.5f);
            image[index + 1] = (unsigned char)(g*255.0f + 0.5f);
            image[index + 2] = (unsigned char)(b*255.0f + 0.5f);
        }
    }

    FILE *file = fopen(ppmPath.c_str(), "wb");
    if (!file) {
        fprintf(stderr, "failed to open %s\n", ppmPath.c_str());
        return 1;
    }
    fprintf(file, "P6\n%d %d\n255\n", ImageWidth, ImageHeight);
    fwrite(image.data(), 1, image.size(), file);
    fclose(file);

    printf("wrote %s interfaces=%llu descriptorReads=%llu\n",
           ppmPath.c_str(),
           (unsigned long long)totalInterfaces,
           (unsigned long long)interfaces.descriptorReads());

    delete ThreadUtils::pool;
    ThreadUtils::pool = 0;
    return 0;
}
