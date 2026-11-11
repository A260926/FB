// Sanity test for the window-independent CPU tile accumulator.
// Renders the existing material scene through TileAccumulator and checks:
//  - full image, 8 samples in one submit
//  - same image as 3+5 samples in two submits (sample-count weighted merge)
//  - same image as unequal rectangles submitted in shuffled order
//  - cropped tile matches the full image at the same pixels; untouched
//    pixels stay zero with zero count (no NaN)
//  - different seeds give different images; a fresh accumulator reproduces
//  - invalid submits are rejected before writing and leave data untouched
// Also writes a small PPM resolved from the accumulator.
#include "Test.h"
#include "TileAccumulator.h"
#include <vector>
#include <cstdio>
#include <cmath>
#include <cassert>

static const int W = 64, H = 36;
static const uint32_t kSeed = 12345;
static const float kTol = 2e-5f;

static std::vector<float> Resolve(const TileAccumulator& acc)
{
    std::vector<float> rgb((size_t)W * H * 3);
    TileAccumulatorResolve(acc, rgb.data());
    return rgb;
}

static void CheckClose(const std::vector<float>& a, const std::vector<float>& b, float tol, const char* what)
{
    assert(a.size() == b.size());
    for (size_t i = 0; i < a.size(); ++i)
    {
        assert(std::isfinite(a[i]));
        float d = std::fabs(a[i] - b[i]);
        if (d > tol)
        {
            printf("FAIL %s: channel %zu differs by %g (%f vs %f)\n", what, i, d, a[i], b[i]);
            assert(false);
        }
    }
}

static void CheckCounts(const TileAccumulator& acc, uint32_t expected, const char* what)
{
    for (size_t i = 0; i < acc.counts.size(); ++i)
    {
        if (acc.counts[i] != expected)
        {
            printf("FAIL %s: pixel %zu count %u != %u\n", what, i, acc.counts[i], expected);
            assert(false);
        }
    }
}

int main(int argc, char** argv)
{
    InitializeTest();
    UpdateTest(0, 0, W, H, 0); // scene + full-frame camera

    // A: whole image, one submit of samples [0,8)
    TileAccumulator accFull;
    assert(TileAccumulatorInit(accFull, W, H, kSeed));
    assert(TileAccumulatorSubmit(accFull, 0, 0, W, H, 0, 8) == kTileSubmitOk);
    CheckCounts(accFull, 8, "full");
    std::vector<float> imgFull = Resolve(accFull);

    // B: same image as [0,3) then [3,8); merge must be sample-count weighted
    TileAccumulator accSplit;
    assert(TileAccumulatorInit(accSplit, W, H, kSeed));
    assert(TileAccumulatorSubmit(accSplit, 0, 0, W, H, 0, 3) == kTileSubmitOk);
    assert(TileAccumulatorSubmit(accSplit, 0, 0, W, H, 3, 5) == kTileSubmitOk);
    CheckCounts(accSplit, 8, "split");
    CheckClose(Resolve(accSplit), imgFull, kTol, "split-vs-full");

    // C: unequal rectangles, submitted in shuffled order, one [0,8) each
    struct Rect { int x0, y0, x1, y1; };
    const Rect rects[] = {
        {  0,  0, 17, H }, // tall left strip
        { 17,  0, W, 13 }, // wide top band
        { 17, 13, 40, H }, // bottom middle
        { 40, 13, W, H },  // bottom right
    };
    const int order[] = { 3, 1, 2, 0 }; // shuffled
    TileAccumulator accTiles;
    assert(TileAccumulatorInit(accTiles, W, H, kSeed));
    for (int i = 0; i < 4; ++i)
    {
        const Rect& r = rects[order[i]];
        assert(TileAccumulatorSubmit(accTiles, r.x0, r.y0, r.x1, r.y1, 0, 8) == kTileSubmitOk);
    }
    CheckCounts(accTiles, 8, "tiles");
    CheckClose(Resolve(accTiles), imgFull, kTol, "tiles-vs-full");

    // D: cropped tile matches full image at same pixels; outside untouched
    const Rect crop = { 9, 7, 31, 25 };
    TileAccumulator accCrop;
    assert(TileAccumulatorInit(accCrop, W, H, kSeed));
    assert(TileAccumulatorSubmit(accCrop, crop.x0, crop.y0, crop.x1, crop.y1, 0, 8) == kTileSubmitOk);
    std::vector<float> imgCrop = Resolve(accCrop);
    for (int y = 0; y < H; ++y)
    {
        for (int x = 0; x < W; ++x)
        {
            size_t pix = (size_t)y * W + x;
            bool inside = x >= crop.x0 && x < crop.x1 && y >= crop.y0 && y < crop.y1;
            if (inside)
            {
                assert(accCrop.counts[pix] == 8);
                for (int c = 0; c < 3; ++c)
                    assert(std::fabs(imgCrop[pix*3+c] - imgFull[pix*3+c]) <= kTol);
            }
            else
            {
                assert(accCrop.counts[pix] == 0);
                for (int c = 0; c < 3; ++c)
                {
                    assert(imgCrop[pix*3+c] == 0.0f);
                    assert(std::isfinite(imgCrop[pix*3+c])); // no NaN
                }
            }
        }
    }

    // E: a different seed must produce a different image
    TileAccumulator accSeed2;
    assert(TileAccumulatorInit(accSeed2, W, H, kSeed + 1));
    assert(TileAccumulatorSubmit(accSeed2, 0, 0, W, H, 0, 8) == kTileSubmitOk);
    std::vector<float> imgSeed2 = Resolve(accSeed2);
    int differ = 0;
    for (size_t i = 0; i < imgFull.size(); ++i)
        if (std::fabs(imgFull[i] - imgSeed2[i]) > 1e-3f)
            ++differ;
    assert(differ > (int)imgFull.size() / 2);

    // F: rebuilding the accumulator with the same seed reproduces exactly
    TileAccumulator accAgain;
    assert(TileAccumulatorInit(accAgain, W, H, kSeed));
    assert(TileAccumulatorSubmit(accAgain, 0, 0, W, H, 0, 3) == kTileSubmitOk);
    assert(TileAccumulatorSubmit(accAgain, 0, 0, W, H, 3, 5) == kTileSubmitOk);
    std::vector<float> imgAgain = Resolve(accAgain);
    std::vector<float> imgSplit = Resolve(accSplit);
    for (size_t i = 0; i < imgSplit.size(); ++i)
        assert(imgAgain[i] == imgSplit[i]);

    // G: invalid submits are rejected before writing and change nothing
    TileAccumulator accGuard;
    assert(TileAccumulatorInit(accGuard, W, H, kSeed));
    assert(TileAccumulatorSubmit(accGuard, 0, 0, W, H, 0, 4) == kTileSubmitOk);
    accGuard.counts[W] = 0xFFFFFFFFu; // pretend one pixel is saturated
    std::vector<float> sumBefore = accGuard.rgbSum;
    std::vector<uint32_t> cntBefore = accGuard.counts;
    assert(TileAccumulatorSubmit(accGuard, 10, 0, 5, 10, 4, 1) == kTileSubmitBadRect);   // reversed x
    assert(TileAccumulatorSubmit(accGuard, 0, 20, 10, 10, 4, 1) == kTileSubmitBadRect);  // reversed y
    assert(TileAccumulatorSubmit(accGuard, -1, 0, 10, 10, 4, 1) == kTileSubmitBadRect);  // negative origin
    assert(TileAccumulatorSubmit(accGuard, 0, 0, W + 1, H, 4, 1) == kTileSubmitBadRect); // past right edge
    assert(TileAccumulatorSubmit(accGuard, 0, 0, W, H + 1, 4, 1) == kTileSubmitBadRect); // past bottom edge
    assert(TileAccumulatorSubmit(accGuard, 0, 0, W, H, 0xFFFFFFFFu, 2) == kTileSubmitOverflow); // sample index wraps
    assert(TileAccumulatorSubmit(accGuard, 0, 0, W, H, 4, 1) == kTileSubmitOverflow);   // count would wrap
    assert(TileAccumulatorSubmit(accGuard, 0, 0, W, H, 4, 0) == kTileSubmitOk);         // zero count: no-op
    assert(accGuard.rgbSum == sumBefore);
    assert(accGuard.counts == cntBefore);

    // H: write a small PPM resolved from the accumulator
    const char* ppmPath = argc > 1 ? argv[1] : "tile_samples.ppm";
    FILE* f = fopen(ppmPath, "wb");
    assert(f);
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (size_t i = 0; i < (size_t)W * H; ++i)
    {
        unsigned char px[3];
        for (int c = 0; c < 3; ++c)
        {
            float v = imgFull[i*3+c];
            v = v < 0 ? 0 : (v > 1 ? 1 : v);
            px[c] = (unsigned char)(powf(v, 1.0f / 2.2f) * 255.0f + 0.5f);
        }
        fwrite(px, 1, 3, f);
    }
    fclose(f);
    printf("PPM_WRITTEN %s %dx%d\n", ppmPath, W, H);

    ShutdownTest();
    puts("TILE_SAMPLES_OK");
    return 0;
}
