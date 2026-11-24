// Irradiance cache example: drives the real DrawTest() preview for two
// consecutive rounds, writes the second round's image as irradiance.ppm and
// prints cache hit/gather counters. Usage: irradiance_cache <output_dir>

#include "Cpp/Source/Test.h"
#include "Cpp/Source/IrradianceCache.h"
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

static void WritePPM(const std::string& path, const std::vector<float>& pixels, int w, int h)
{
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f)
    {
        std::fprintf(stderr, "cannot open %s\n", path.c_str());
        std::exit(2);
    }
    std::fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int i = 0; i < w * h; ++i)
    {
        unsigned char rgb[3];
        for (int c = 0; c < 3; ++c)
        {
            float v = pixels[(size_t)i * 4 + c];
            // gamma-ish tone mapping matching a simple preview writer
            v = v / (1.0f + v);
            int g = (int)(255.0f * v + 0.5f);
            rgb[c] = (unsigned char)(g < 0 ? 0 : (g > 255 ? 255 : g));
        }
        std::fwrite(rgb, 1, 3, f);
    }
    std::fclose(f);
}

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::fprintf(stderr, "usage: %s <output_dir>\n", argv[0]);
        return 1;
    }
    const std::string outDir = argv[1];

    const int w = 64, h = 48;
    std::vector<float> pixels((size_t)w * h * 4, 0.0f);

    InitializeTest();
    UpdateTest(0.0f, 0, w, h, kFlagProgressive);

    ICConfig config;
    config.enabled = true;
    config.gatherSamples = 16;
    config.tau = 0.5f;
    config.minRadius = 0.05f;
    config.maxRadius = 0.5f;
    config.seed = 0x12345678u;
    if (!ConfigureIrradianceCache(config))
    {
        std::fprintf(stderr, "invalid irradiance cache configuration\n");
        return 3;
    }

    int rays = 0;
    // Two real draws: round 0 only gathers (cold cache), round 1 reuses.
    for (int frame = 0; frame < 2; ++frame)
    {
        UpdateTest(0.0f, frame, w, h, kFlagProgressive);
        DrawTest(0.0f, frame, w, h, pixels.data(), rays,
                 kFlagProgressive | kFlagIrradianceCache);
    }

    ICStats stats;
    GetIrradianceCacheStats(stats);
    std::printf("irradiance cache: queries=%llu hits=%llu gathers=%llu "
                "probeRays=%llu connectionRays=%llu records=%llu rays=%d\n",
                (unsigned long long)stats.queries,
                (unsigned long long)stats.hits,
                (unsigned long long)stats.gathers,
                (unsigned long long)stats.probeRays,
                (unsigned long long)stats.connectionRays,
                (unsigned long long)stats.recordsPublished,
                rays);

    WritePPM(outDir + "/irradiance.ppm", pixels, w, h);
    ShutdownTest();
    return 0;
}
