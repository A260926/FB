#pragma once
// Progressive stochastic transparency for the CPU rasterizer.
//
// The stochastic path keeps the ordinary pixel-center coverage and material
// shading, but gives every pixel eight independent depth/color samples. A
// fragment competes for a sample only when a deterministic random draw passes
// its material opacity. Multi-frame accumulation converges toward the
// source-over expectation without storing transparent fragment lists: the
// persistent state size depends only on the framebuffer pixel count.
#include <cstdint>

namespace SoftRasterizer {

// Eight independent stochastic samples per pixel, as required by the path.
constexpr int StochasticSampleCount = 8;

// Seed selects the independent random stream; generation identifies a single
// convergence run. Both are plain unsigned integers, so every value is finite
// and legal; resetStochastic() starts a new run.
struct StochasticSettings {
  std::uint64_t seed = 1;
  std::uint64_t generation = 0;
};

// Deterministic 64-bit hash of the full stochastic sample coordinate.
//
// The value is a pure function of (seed, generation, frame, x, y, sample,
// surfaceId). It never depends on submission order, tile scheduling, SIMD
// width or batch boundaries, so splitting a draw into batches or tiles cannot
// change a fragment's random draws. Distinct surfaceId values feed distinct
// inputs, giving every surface an independent random dimension.
std::uint64_t stochasticHash64(std::uint64_t seed, std::uint64_t generation,
                               std::uint64_t frame, std::uint64_t x,
                               std::uint64_t y, std::uint64_t sample,
                               std::uint64_t surfaceId);

// Map the deterministic hash to a single-precision value in [0, 1). The 24
// lowest mantissa bits of the hash are used, so the grid has 2^24 levels.
float stochasticHashRandom(std::uint64_t seed, std::uint64_t generation,
                           std::uint64_t frame, std::uint64_t x,
                           std::uint64_t y, std::uint64_t sample,
                           std::uint64_t surfaceId);
} // namespace SoftRasterizer
