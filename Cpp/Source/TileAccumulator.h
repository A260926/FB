#pragma once

#include <stdint.h>
#include <vector>

// CPU-only, window-independent tile accumulator for ToyPathTracer.
//
// This is an offline rendering entry point that sits next to DrawTest:
// instead of pushing whole frames into a backbuffer, the caller submits
// half-open pixel rectangles [x0,x1) x [y0,y1) together with an absolute
// sample index range [firstSample, firstSample+sampleCount). Results are
// accumulated per pixel as linear RGB sums plus a sample count, so tiles
// can be rendered in any order, with any per-submit sample count, and still
// resolve to the same full-frame image.
//
// Randomness identity: each path is seeded only by (user seed, full-frame
// pixel x, full-frame pixel y, absolute sample index). Bounce count, worker
// thread and tile edges never feed the RNG. Camera jitter and aperture
// sampling consume the same per-path state, and rays are always generated
// in full-frame coordinates with the full-frame camera (no per-tile aspect
// change). Scattering and light sampling go through the exact same
// Scatter/Trace code as DrawTest.
//
// Usage:
//   InitializeTest();
//   UpdateTest(0, 0, fullWidth, fullHeight, 0);   // sets scene + full-frame camera
//   TileAccumulator acc;
//   TileAccumulatorInit(acc, fullWidth, fullHeight, userSeed);
//   TileAccumulatorSubmit(acc, 0, 0, fullWidth, fullHeight, 0, 8);      // samples [0,8)
//   TileAccumulatorSubmit(acc, x0, y0, x1, y1, 8, 4);                    // samples [8,12) on a tile
//   std::vector<float> rgb(fullWidth*fullHeight*3);
//   TileAccumulatorResolve(acc, rgb.data());                             // mean linear RGB
//
// The caller must guarantee that submitted sample ranges do not overlap for
// any given pixel. The accumulator itself is not thread-safe across callers,
// but Submit parallelizes internally with the existing enkiTS task scheduler.

struct TileAccumulator
{
    TileAccumulator() : width(0), height(0), seed(0) {}

    int width;                  // full-frame width in pixels
    int height;                 // full-frame height in pixels
    uint32_t seed;              // user seed, part of every path's random identity
    std::vector<float> rgbSum;  // width*height*3 linear RGB sample sums
    std::vector<uint32_t> counts; // width*height per-pixel sample counts
};

enum TileSubmitResult
{
    kTileSubmitOk = 0,     // submitted (or a valid no-op: zero count / zero-area rect)
    kTileSubmitBadRect,    // out of bounds, reversed or negative-size rectangle
    kTileSubmitOverflow,   // sample index/count arithmetic would overflow
};

// (Re)initializes the accumulator for a full frame of width x height with the
// given user seed. All pixels start empty (zero sum, zero count).
// Returns false for non-positive dimensions.
bool TileAccumulatorInit(TileAccumulator& acc, int width, int height, uint32_t seed);

// Renders samples [firstSample, firstSample+sampleCount) for the half-open
// rectangle [x0,x1) x [y0,y1) in full-frame coordinates and accumulates them.
// sampleCount == 0 is a no-op. Invalid rectangles and overflowing counts are
// reported *before* anything is written; on any non-Ok result the accumulator
// is left completely untouched.
TileSubmitResult TileAccumulatorSubmit(TileAccumulator& acc, int x0, int y0, int x1, int y1, uint32_t firstSample, uint32_t sampleCount);

// Writes width*height*3 mean linear RGB values (sum / count per pixel).
// Empty pixels resolve to exactly (0,0,0), never NaN.
void TileAccumulatorResolve(const TileAccumulator& acc, float* outRGB);
