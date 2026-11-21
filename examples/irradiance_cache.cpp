// Example: two real DrawTest rounds with the optional irradiance cache.
//
// Usage: irradiance_cache <output-dir>
// Writes <output-dir>/irradiance.ppm from the second round and prints the
// cache hit/gather counts. Uses only the C++17 standard library and the
// in-tree enkiTS; no window or GPU dependencies.

#include "Cpp/Source/Test.h"
#include "Cpp/Source/IrradianceCache.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static void WritePPM(const char* path, const float* pixels, int width, int height)
{
    FILE* f = std::fopen(path, "wb");
    if (!f)
    {
        std::fprintf(stderr, "cannot open %s\n", path);
        std::exit(2);
    }
    std::fprintf(f, "P6\n%d %d\n255\n", width, height);
    for (int i = 0; i < width * height; ++i)
    {
        unsigned char rgb[3];
        for (int c = 0; c < 3; ++c)
        {
            float v = pixels[i * 4 + c];
            // Simple Reinhard-ish clamp then gamma, matching a preview write.
            v = v / (1.0f + v);
            int b = static_cast<int>(255.99f * v);
            if (b < 0) b = 0;
            if (b > 255) b = 255;
            rgb[c] = static_cast<unsigned char>(b);
        }
        std::fwrite(rgb, 1, 3, f);
    }
    std::fclose(f);
}

int main(int argc, char** argv)
{
    const char* outDir = argc > 1 ? argv[1] : ".";

    InitializeTest();

    // The cache is opt-in and off until explicitly configured.
    ICConfig cfg;
    cfg.gatherSamples = 32;
    cfg.tau = 0.35f;
    cfg.minRadius = 0.05f;
    cfg.maxRadius = 0.5f;
    cfg.seed = 12345u;
    if (!GetIrradianceCache().Configure(cfg))
    {
        std::fprintf(stderr, "invalid cache configuration\n");
        return 2;
    }

    const int width = 160;
    const int height = 90;
    std::vector<float> backbuffer(width * height * 4, 0.0f);

    int rays = 0;
    // Round 1: cold cache, every diffuse vertex gathers.
    UpdateTest(0.0f, 0, width, height, kFlagProgressive | kFlagIrradianceCache);
    DrawTest(0.0f, 0, width, height, backbuffer.data(), rays,
             kFlagProgressive | kFlagIrradianceCache);
    ICStats first = GetIrradianceCache().GetStats();

    // Round 2: candidates from round 1 are now readable; nearby diffuse hits
    // interpolate instead of gathering.
    UpdateTest(0.0f, 1, width, height, kFlagProgressive | kFlagIrradianceCache);
    DrawTest(0.0f, 1, width, height, backbuffer.data(), rays,
             kFlagProgressive | kFlagIrradianceCache);
    ICStats second = GetIrradianceCache().GetStats();

    std::string ppm = std::string(outDir) + "/irradiance.ppm";
    WritePPM(ppm.c_str(), backbuffer.data(), width, height);

    std::printf("round1 queries=%llu hits=%llu gathers=%llu probes=%llu connectors=%llu records=%zu\n",
                (unsigned long long)first.queries, (unsigned long long)first.cacheHits,
                (unsigned long long)first.gathers, (unsigned long long)first.probeRays,
                (unsigned long long)first.connectionChecks,
                GetIrradianceCache().GetRecordCount());
    std::printf("round2 queries=%llu hits=%llu gathers=%llu probes=%llu connectors=%llu records=%zu\n",
                (unsigned long long)second.queries, (unsigned long long)second.cacheHits,
                (unsigned long long)second.gathers, (unsigned long long)second.probeRays,
                (unsigned long long)second.connectionChecks,
                GetIrradianceCache().GetRecordCount());
    std::printf("wrote %s\n", ppm.c_str());

    ShutdownTest();
    return 0;
}
