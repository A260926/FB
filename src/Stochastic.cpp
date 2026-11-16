#include <render/Stochastic.hpp>

namespace {
// SplitMix64 finalizer (Stafford's Mix13/Mix14 constants): avalanche-style
// bit mixing with no hidden state, so the hash is reproducible on every
// platform and independent of scheduling.
std::uint64_t mix64(std::uint64_t value) {
  value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
  value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
  return value ^ (value >> 31);
}
} // namespace

std::uint64_t SoftRasterizer::stochasticHash64(
    std::uint64_t seed, std::uint64_t generation, std::uint64_t frame,
    std::uint64_t x, std::uint64_t y, std::uint64_t sample,
    std::uint64_t surfaceId) {
  // Every coordinate of the sample, including the surface identity, enters
  // the hash. The construction is order-sensitive and stateless, so batching,
  // tile assignment and SIMD width cannot influence the result.
  std::uint64_t hash = mix64(seed + 0x9e3779b97f4a7c15ULL);
  const std::uint64_t inputs[6] = {generation, frame, x, y, sample,
                                   surfaceId};
  for (std::uint64_t value : inputs) {
    hash ^= mix64(value + 0x9e3779b97f4a7c15ULL + (hash << 6) +
                  (hash >> 2));
  }
  return mix64(hash);
}

float SoftRasterizer::stochasticHashRandom(
    std::uint64_t seed, std::uint64_t generation, std::uint64_t frame,
    std::uint64_t x, std::uint64_t y, std::uint64_t sample,
    std::uint64_t surfaceId) {
  // 24 mantissa bits give a uniform grid of 2^24 levels in [0, 1); zero is
  // reachable and 1 is not, so r < 1 always passes and r < 0 never does.
  const std::uint64_t hash =
      stochasticHash64(seed, generation, frame, x, y, sample, surfaceId);
  return static_cast<float>(hash >> 40) *
         (1.0f / 16777216.0f); // 1 / 2^24
}
